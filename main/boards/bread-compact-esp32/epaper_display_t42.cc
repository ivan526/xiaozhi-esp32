#include "epaper_display_t42.h"
#include "config.h"
#include "assets/lang_config.h"

#include <algorithm>
#include <cstring>

#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>

#define TAG "EpaperGDEY075T7"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

namespace {
constexpr spi_host_device_t kSpiHost = SPI3_HOST;
constexpr size_t kSpiChunk = 4096;
constexpr size_t kMaxUserBytes = 240;
constexpr size_t kMaxAssistantBytes = 780;
constexpr uint32_t kPartialDebounceMs = 250;
constexpr uint32_t kRefreshTriggerDelayMs = 100;
constexpr uint32_t kPartialRefreshLimit = 20;

// Full-width partial bands. Keeping X = 0..799 makes the LVGL capture path
// simple and robust while still avoiding a full 480-line waveform refresh.
constexpr int kStatusY0 = 0;
constexpr int kStatusY1 = 72;
constexpr int kUserY0 = 64;
constexpr int kUserY1 = 196;
constexpr int kAssistantY0 = 196;
constexpr int kAssistantY1 = EPD_HEIGHT;
}

EpaperDisplayT42::EpaperDisplayT42()
    : LcdDisplay(nullptr, nullptr, EPD_WIDTH, EPD_HEIGHT) {
    ESP_LOGI(TAG, "Heap before e-paper: free=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));

    if (!InitializeHardware()) {
        ESP_LOGE(TAG, "Hardware initialization failed");
        return;
    }

    if (xTaskCreate(
            RefreshTaskEntry,
            "epaper_refresh",
            4096,
            this,
            2,
            &refresh_task_handle_) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create e-paper refresh task");
        return;
    }

    if (!InitializeLvgl()) {
        ESP_LOGE(TAG, "LVGL initialization failed");
        return;
    }

    ready_ = true;
    ESP_LOGI(TAG,
             "Ready: GDEY075T7 800x480, partial refresh enabled, PWR=3V3(always-on) BUSY=%d RST=%d DC=%d CS=%d CLK=%d DIN=%d",
             EPD_BUSY_PIN, EPD_RST_PIN, EPD_DC_PIN,
             EPD_CS_PIN, EPD_SCLK_PIN, EPD_MOSI_PIN);
    ESP_LOGI(TAG, "Heap after e-paper: free=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));
}

EpaperDisplayT42::~EpaperDisplayT42() {
    if (refresh_task_handle_ != nullptr) {
        vTaskDelete(refresh_task_handle_);
        refresh_task_handle_ = nullptr;
    }

    SleepPanel();
    ReleasePartialBuffer();

    if (spi_ != nullptr) {
        spi_bus_remove_device(spi_);
        spi_ = nullptr;
        spi_bus_free(kSpiHost);
    }

    if (lvgl_buffer_ != nullptr) {
        heap_caps_free(lvgl_buffer_);
        lvgl_buffer_ = nullptr;
    }

    if (mono_line_ != nullptr) {
        heap_caps_free(mono_line_);
        mono_line_ = nullptr;
    }
}

bool EpaperDisplayT42::InitializeHardware() {
    gpio_config_t out_cfg = {};
    out_cfg.pin_bit_mask =
        (1ULL << EPD_RST_PIN) |
        (1ULL << EPD_DC_PIN);
    out_cfg.mode = GPIO_MODE_OUTPUT;
    out_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    out_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    out_cfg.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&out_cfg) != ESP_OK) {
        return false;
    }

    gpio_config_t busy_cfg = {};
    busy_cfg.pin_bit_mask = (1ULL << EPD_BUSY_PIN);
    busy_cfg.mode = GPIO_MODE_INPUT;
    busy_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    busy_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    busy_cfg.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&busy_cfg) != ESP_OK) {
        return false;
    }

    gpio_set_level(EPD_RST_PIN, 1);
    gpio_set_level(EPD_DC_PIN, 1);

    spi_bus_config_t bus_cfg = {};
    bus_cfg.mosi_io_num = EPD_MOSI_PIN;
    bus_cfg.miso_io_num = -1;
    bus_cfg.sclk_io_num = EPD_SCLK_PIN;
    bus_cfg.quadwp_io_num = -1;
    bus_cfg.quadhd_io_num = -1;
    bus_cfg.max_transfer_sz = kSpiChunk;

    esp_err_t err = spi_bus_initialize(kSpiHost, &bus_cfg, SPI_DMA_CH_AUTO);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_initialize failed: %s", esp_err_to_name(err));
        return false;
    }

    spi_device_interface_config_t dev_cfg = {};
    dev_cfg.clock_speed_hz = EPD_SPI_CLOCK_HZ;
    dev_cfg.mode = 0;
    dev_cfg.spics_io_num = EPD_CS_PIN;
    dev_cfg.queue_size = 1;

    err = spi_bus_add_device(kSpiHost, &dev_cfg, &spi_);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi_bus_add_device failed: %s", esp_err_to_name(err));
        spi_bus_free(kSpiHost);
        return false;
    }

    mono_line_ = static_cast<uint8_t*>(
        heap_caps_malloc(MONO_LINE_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    if (mono_line_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate monochrome line buffer");
        spi_bus_remove_device(spi_);
        spi_ = nullptr;
        spi_bus_free(kSpiHost);
        return false;
    }

    return true;
}

bool EpaperDisplayT42::InitializeLvgl() {
    ESP_LOGI(TAG, "Initialize LVGL for low-memory e-paper streaming");

    lv_init();

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 2;
    port_cfg.timer_period_ms = 50;

    esp_err_t err = lvgl_port_init(&port_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed: %s", esp_err_to_name(err));
        return false;
    }

    const size_t buffer_size =
        EPD_WIDTH * LVGL_BUFFER_ROWS *
        LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565);

    lvgl_buffer_ = static_cast<uint8_t*>(
        heap_caps_malloc(buffer_size, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (lvgl_buffer_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate %u-byte LVGL draw buffer",
                 static_cast<unsigned>(buffer_size));
        return false;
    }

    lvgl_port_lock(0);
    display_ = lv_display_create(EPD_WIDTH, EPD_HEIGHT);
    if (display_ == nullptr) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "lv_display_create failed");
        return false;
    }

    lv_display_set_color_format(display_, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display_, LvglFlushCb);
    lv_display_set_user_data(display_, this);
    lv_display_set_buffers(
        display_,
        lvgl_buffer_,
        nullptr,
        buffer_size,
        LV_DISPLAY_RENDER_MODE_PARTIAL);
    lvgl_port_unlock();

    return true;
}

void EpaperDisplayT42::SetupUI() {
    if (setup_ui_called_) {
        return;
    }

    Display::SetupUI();
    DisplayLockGuard lock(this);

    lv_obj_t* screen = lv_display_get_screen_active(display_);
    if (screen == nullptr) {
        ESP_LOGE(TAG, "No active LVGL screen");
        return;
    }

    lv_obj_clean(screen);
    lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
    lv_obj_set_style_text_color(screen, lv_color_black(), 0);
    lv_obj_set_style_text_font(screen, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_pad_all(screen, 0, 0);

    title_label_ = lv_label_create(screen);
    lv_label_set_text(title_label_, "小智");
    lv_obj_set_pos(title_label_, 32, 20);
    lv_obj_set_style_text_color(title_label_, lv_color_black(), 0);
    lv_obj_set_style_text_font(title_label_, &BUILTIN_TEXT_FONT, 0);

    status_label_ = lv_label_create(screen);
    status_text_ = "正在启动";
    lv_label_set_text(status_label_, status_text_.c_str());
    lv_obj_set_width(status_label_, 560);
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(status_label_, lv_color_black(), 0);
    lv_obj_set_style_text_font(status_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_align(status_label_, LV_ALIGN_TOP_RIGHT, -32, 20);

    auto add_divider = [screen](int y) {
        lv_obj_t* line = lv_obj_create(screen);
        lv_obj_set_pos(line, 32, y);
        lv_obj_set_size(line, EPD_WIDTH - 64, 2);
        lv_obj_set_style_radius(line, 0, 0);
        lv_obj_set_style_border_width(line, 0, 0);
        lv_obj_set_style_pad_all(line, 0, 0);
        lv_obj_set_style_bg_color(line, lv_color_black(), 0);
        lv_obj_set_style_bg_opa(line, LV_OPA_COVER, 0);
        lv_obj_clear_flag(line, LV_OBJ_FLAG_SCROLLABLE);
    };

    add_divider(58);
    add_divider(188);

    user_label_ = lv_label_create(screen);
    lv_obj_set_pos(user_label_, 32, 80);
    lv_obj_set_size(user_label_, EPD_WIDTH - 64, 88);
    lv_label_set_long_mode(user_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(user_label_, lv_color_black(), 0);
    lv_obj_set_style_text_font(user_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_line_space(user_label_, 5, 0);

    assistant_label_ = lv_label_create(screen);
    lv_obj_set_pos(assistant_label_, 32, 210);
    lv_obj_set_size(assistant_label_, EPD_WIDTH - 64, 238);
    lv_label_set_long_mode(assistant_label_, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(assistant_label_, lv_color_black(), 0);
    lv_obj_set_style_text_font(assistant_label_, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_line_space(assistant_label_, 6, 0);

    UpdateUserLabelLocked();
    UpdateAssistantLabelLocked();

    ESP_LOGI(TAG, "E-paper compact UI ready");
    NotifyRefresh(REFRESH_FULL);
}

std::string EpaperDisplayT42::TruncateUtf8(const std::string& text, size_t max_bytes) {
    if (text.size() <= max_bytes) {
        return text;
    }

    size_t end = max_bytes;
    while (end > 0 && end < text.size() &&
           (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
        --end;
    }
    return text.substr(0, end) + "...";
}

void EpaperDisplayT42::UpdateUserLabelLocked() {
    if (user_label_ == nullptr) {
        return;
    }
    std::string text = "你：";
    text += user_text_.empty() ? "等待你说话" : TruncateUtf8(user_text_, kMaxUserBytes);
    lv_label_set_text(user_label_, text.c_str());
}

void EpaperDisplayT42::UpdateAssistantLabelLocked() {
    if (assistant_label_ == nullptr) {
        return;
    }
    std::string text = "小智：";
    text += assistant_text_.empty() ? "准备好了，随时可以聊。" : TruncateUtf8(assistant_text_, kMaxAssistantBytes);
    lv_label_set_text(assistant_label_, text.c_str());
}

void EpaperDisplayT42::SetStatus(const char* status) {
    if (status == nullptr) {
        return;
    }

    Display::SetStatus(status);
    const bool is_speaking = (std::strcmp(status, Lang::Strings::SPEAKING) == 0);
    speaking_ = is_speaking;
    status_text_ = status;

    if (setup_ui_called_ && status_label_ != nullptr) {
        DisplayLockGuard lock(this);
        lv_label_set_text(status_label_, status_text_.c_str());
    }

    pending_refresh_mask_.fetch_or(REFRESH_STATUS, std::memory_order_relaxed);
    // While TTS is active, assistant chunks accumulate without physical refresh.
    // The status transition after speaking ends wakes the task and refreshes all
    // dirty bands in one partial-mode session.
    if (!is_speaking) {
        NotifyRefresh(REFRESH_STATUS);
    }
}

void EpaperDisplayT42::ShowNotification(const char* notification, int duration_ms) {
    (void)duration_ms;
    if (notification == nullptr || notification[0] == '\0') {
        return;
    }
    Display::ShowNotification(notification, duration_ms);
    status_text_ = notification;
    if (setup_ui_called_ && status_label_ != nullptr) {
        DisplayLockGuard lock(this);
        lv_label_set_text(status_label_, status_text_.c_str());
    }
    pending_refresh_mask_.fetch_or(REFRESH_STATUS, std::memory_order_relaxed);
    if (!speaking_) {
        NotifyRefresh(REFRESH_STATUS);
    }
}

void EpaperDisplayT42::ShowNotification(const std::string& notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void EpaperDisplayT42::SetEmotion(const char* emotion) {
    Display::SetEmotion(emotion != nullptr ? emotion : "");
}

void EpaperDisplayT42::SetChatMessage(const char* role, const char* content) {
    if (role == nullptr || content == nullptr) {
        return;
    }

    Display::SetChatMessage(role, content);

    uint32_t dirty_mask = REFRESH_NONE;
    if (std::strcmp(role, "user") == 0) {
        user_text_ = content;
        assistant_text_.clear();
        dirty_mask = REFRESH_USER | REFRESH_ASSISTANT;
    } else if (std::strcmp(role, "assistant") == 0) {
        if (content[0] != '\0') {
            assistant_text_ += content;
            dirty_mask = REFRESH_ASSISTANT;
        }
    } else if (std::strcmp(role, "system") == 0) {
        if (content[0] != '\0' && std::strstr(content, "bread-compact-esp32") == nullptr) {
            assistant_text_ = content;
            dirty_mask = REFRESH_ASSISTANT;
        }
    }

    if (dirty_mask == REFRESH_NONE) {
        return;
    }

    if (setup_ui_called_) {
        DisplayLockGuard lock(this);
        UpdateUserLabelLocked();
        UpdateAssistantLabelLocked();
    }

    pending_refresh_mask_.fetch_or(dirty_mask, std::memory_order_relaxed);
    if (!speaking_) {
        NotifyRefresh(dirty_mask);
    }
}

void EpaperDisplayT42::ClearChatMessages() {
    ESP_LOGI(TAG, "Preserve last Q&A on ClearChatMessages");
}

void EpaperDisplayT42::UpdateStatusBar(bool update_all) {
    (void)update_all;
    // No periodic 1 Hz refresh on e-paper.
}

void EpaperDisplayT42::SetPowerSaveMode(bool on) {
    (void)on;
    // HAT PWR is tied to 3V3. UC8179 deep sleep is entered after each physical batch.
}

void EpaperDisplayT42::LvglFlushCb(
    lv_display_t* disp,
    const lv_area_t* area,
    uint8_t* color_p) {
    auto* self = static_cast<EpaperDisplayT42*>(lv_display_get_user_data(disp));
    if (self == nullptr) {
        lv_display_flush_ready(disp);
        return;
    }

    if (!self->streaming_refresh_) {
        lv_display_flush_ready(disp);
        return;
    }

    const int area_width = area->x2 - area->x1 + 1;
    const int area_height = area->y2 - area->y1 + 1;
    auto* pixels = reinterpret_cast<uint16_t*>(color_p);

    if (self->capture_partial_) {
        if (self->partial_buffer_ == nullptr) {
            self->stream_error_ = true;
            lv_display_flush_ready(disp);
            return;
        }

        const int x0 = std::max<int>(area->x1, self->capture_area_.x1);
        const int x1 = std::min<int>(area->x2, self->capture_area_.x2);
        const int y0 = std::max<int>(area->y1, self->capture_area_.y1);
        const int y1 = std::min<int>(area->y2, self->capture_area_.y2);

        if (x0 <= x1 && y0 <= y1) {
            for (int y = y0; y <= y1; ++y) {
                const size_t dst_row = static_cast<size_t>(y - self->capture_area_.y1) * MONO_LINE_BYTES;
                const size_t src_row = static_cast<size_t>(y - area->y1) * area_width;
                for (int x = x0; x <= x1; ++x) {
                    const uint16_t p = pixels[src_row + static_cast<size_t>(x - area->x1)];
                    const uint32_t r = (p >> 11) & 0x1F;
                    const uint32_t g = (p >> 5) & 0x3F;
                    const uint32_t b = p & 0x1F;
                    const uint32_t lum =
                        (r * 255 / 31) * 299 +
                        (g * 255 / 63) * 587 +
                        (b * 255 / 31) * 114;

                    uint8_t& out = self->partial_buffer_[dst_row + (x >> 3)];
                    const uint8_t bit = static_cast<uint8_t>(0x80 >> (x & 7));
                    if (lum < 128000) {
                        out &= static_cast<uint8_t>(~bit); // 0 = black
                    } else {
                        out |= bit; // 1 = white
                    }
                }
            }
        }

        lv_display_flush_ready(disp);
        return;
    }

    // Full-screen streaming intentionally keeps no 48KB framebuffer. A full
    // invalidation with the 4-row LVGL buffer arrives as full-width stripes.
    if (self->mono_line_ == nullptr ||
        area->x1 != 0 || area->x2 != (EPD_WIDTH - 1)) {
        ESP_LOGE(TAG, "Unexpected full stream area x=%d..%d y=%d..%d",
                 area->x1, area->x2, area->y1, area->y2);
        self->stream_error_ = true;
        lv_display_flush_ready(disp);
        return;
    }

    for (int row = 0; row < area_height; ++row) {
        std::memset(self->mono_line_, 0xFF, MONO_LINE_BYTES);
        for (int x = 0; x < EPD_WIDTH; ++x) {
            const uint16_t p = pixels[static_cast<size_t>(row) * EPD_WIDTH + x];
            const uint32_t r = (p >> 11) & 0x1F;
            const uint32_t g = (p >> 5) & 0x3F;
            const uint32_t b = p & 0x1F;
            const uint32_t lum =
                (r * 255 / 31) * 299 +
                (g * 255 / 63) * 587 +
                (b * 255 / 31) * 114;

            if (lum < 128000) {
                self->mono_line_[x >> 3] &=
                    static_cast<uint8_t>(~(0x80 >> (x & 7)));
            }
        }

        gpio_set_level(EPD_DC_PIN, 1);
        if (self->SpiWrite(self->mono_line_, MONO_LINE_BYTES) != ESP_OK) {
            self->stream_error_ = true;
            break;
        }
    }

    lv_display_flush_ready(disp);
}

void EpaperDisplayT42::NotifyRefresh(uint32_t mask) {
    pending_refresh_mask_.fetch_or(mask, std::memory_order_relaxed);
    if (refresh_task_handle_ != nullptr && !streaming_refresh_) {
        xTaskNotifyGive(refresh_task_handle_);
    }
}

void EpaperDisplayT42::RefreshTaskEntry(void* arg) {
    static_cast<EpaperDisplayT42*>(arg)->RefreshTaskLoop();
}

void EpaperDisplayT42::RefreshTaskLoop() {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

        while (ulTaskNotifyTake(
                   pdTRUE,
                   pdMS_TO_TICKS(kPartialDebounceMs)) > 0) {
        }

        if (speaking_) {
            ESP_LOGI(TAG, "Refresh deferred while TTS is speaking");
            continue;
        }

        uint32_t mask = pending_refresh_mask_.exchange(REFRESH_NONE, std::memory_order_acq_rel);
        if (mask == REFRESH_NONE) {
            continue;
        }

        const bool force_full =
            !full_refresh_done_ ||
            ((mask & REFRESH_FULL) != 0) ||
            (partial_refresh_count_ >= kPartialRefreshLimit);

        bool ok = false;
        if (force_full) {
            ok = RefreshPanelFull();
        } else {
            ok = RefreshPanelPartial(mask);
            if (!ok) {
                ESP_LOGW(TAG, "Partial refresh failed; falling back to full refresh");
                ok = RefreshPanelFull();
            }
        }

        if (!ok) {
            ESP_LOGE(TAG, "Panel refresh failed; dirty mask retained for retry");
            pending_refresh_mask_.fetch_or(mask, std::memory_order_relaxed);
        }
    }
}

bool EpaperDisplayT42::StreamCurrentUiToPanel() {
    if (display_ == nullptr) {
        return false;
    }

    stream_error_ = false;
    capture_partial_ = false;
    streaming_refresh_ = true;

    lvgl_port_lock(0);
    lv_obj_t* screen = lv_display_get_screen_active(display_);
    if (screen != nullptr) {
        lv_obj_invalidate(screen);
        lv_refr_now(display_);
    } else {
        stream_error_ = true;
    }
    lvgl_port_unlock();

    streaming_refresh_ = false;
    return !stream_error_;
}

bool EpaperDisplayT42::StreamSolidPlane(uint8_t value) {
    if (mono_line_ == nullptr) {
        return false;
    }

    std::memset(mono_line_, value, MONO_LINE_BYTES);
    gpio_set_level(EPD_DC_PIN, 1);
    for (int y = 0; y < EPD_HEIGHT; ++y) {
        if (SpiWrite(mono_line_, MONO_LINE_BYTES) != ESP_OK) {
            return false;
        }
    }
    return true;
}

bool EpaperDisplayT42::CaptureUiRegion(int y_start, int y_end) {
    y_start = std::max(0, y_start);
    y_end = std::min(EPD_HEIGHT, y_end);
    if (display_ == nullptr || y_start >= y_end) {
        return false;
    }

    const size_t required = MONO_LINE_BYTES * static_cast<size_t>(y_end - y_start);
    ReleasePartialBuffer();
    partial_buffer_ = static_cast<uint8_t*>(
        heap_caps_malloc(required, MALLOC_CAP_8BIT));
    if (partial_buffer_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate %u-byte partial capture buffer",
                 static_cast<unsigned>(required));
        return false;
    }
    partial_buffer_size_ = required;
    std::memset(partial_buffer_, 0xFF, required);

    capture_area_.x1 = 0;
    capture_area_.x2 = EPD_WIDTH - 1;
    capture_area_.y1 = y_start;
    capture_area_.y2 = y_end - 1;

    stream_error_ = false;
    capture_partial_ = true;
    streaming_refresh_ = true;

    lvgl_port_lock(0);
    lv_obj_t* screen = lv_display_get_screen_active(display_);
    if (screen != nullptr) {
        lv_obj_invalidate_area(screen, &capture_area_);
        lv_refr_now(display_);
    } else {
        stream_error_ = true;
    }
    lvgl_port_unlock();

    capture_partial_ = false;
    streaming_refresh_ = false;
    return !stream_error_;
}

bool EpaperDisplayT42::WriteCapturedPartialRegion(int y_start, int y_end) {
    if (partial_buffer_ == nullptr || y_start >= y_end) {
        return false;
    }

    if (!SetPartialWindow(0, y_start, EPD_WIDTH, y_end)) {
        return false;
    }

    SendCommand(0x13); // current image RAM, matching Waveshare partial example
    gpio_set_level(EPD_DC_PIN, 1);
    const int rows = y_end - y_start;
    for (int row = 0; row < rows; ++row) {
        std::memcpy(mono_line_,
                    partial_buffer_ + static_cast<size_t>(row) * MONO_LINE_BYTES,
                    MONO_LINE_BYTES);
        if (SpiWrite(mono_line_, MONO_LINE_BYTES) != ESP_OK) {
            return false;
        }
    }

    SendCommand(0x12); // DISPLAY REFRESH
    vTaskDelay(pdMS_TO_TICKS(kRefreshTriggerDelayMs));
    return WaitBusyRelease("partial-refresh", EPD_BUSY_TIMEOUT_MS);
}

void EpaperDisplayT42::ReleasePartialBuffer() {
    if (partial_buffer_ != nullptr) {
        heap_caps_free(partial_buffer_);
        partial_buffer_ = nullptr;
        partial_buffer_size_ = 0;
    }
}

esp_err_t EpaperDisplayT42::SpiWrite(const uint8_t* data, size_t len) {
    if (spi_ == nullptr || data == nullptr || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    size_t offset = 0;
    while (offset < len) {
        const size_t n = std::min(kSpiChunk, len - offset);
        spi_transaction_t t = {};
        t.length = n * 8;
        t.tx_buffer = data + offset;

        esp_err_t err = spi_device_polling_transmit(spi_, &t);
        if (err != ESP_OK) {
            return err;
        }
        offset += n;
    }
    return ESP_OK;
}

void EpaperDisplayT42::SendCommand(uint8_t cmd) {
    gpio_set_level(EPD_DC_PIN, 0);
    ESP_ERROR_CHECK_WITHOUT_ABORT(SpiWrite(&cmd, 1));
}

void EpaperDisplayT42::SendData(uint8_t data) {
    gpio_set_level(EPD_DC_PIN, 1);
    ESP_ERROR_CHECK_WITHOUT_ABORT(SpiWrite(&data, 1));
}

void EpaperDisplayT42::HardwareReset() {
    // Known-good timing on the user's Waveshare Rev2.3 HAT.
    gpio_set_level(EPD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(EPD_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10));
    gpio_set_level(EPD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
}

bool EpaperDisplayT42::WaitBusyRelease(const char* reason, uint32_t timeout_ms) {
    const TickType_t start = xTaskGetTickCount();
    while (gpio_get_level(EPD_BUSY_PIN) == 0) {
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(timeout_ms)) {
            ESP_LOGE(TAG, "BUSY timeout while %s, level=%d",
                     reason, gpio_get_level(EPD_BUSY_PIN));
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    vTaskDelay(pdMS_TO_TICKS(5));
    return true;
}

void EpaperDisplayT42::ConfigurePanelBase() {
    // GDEY075T7 / UC8179 base sequence from the known-good GxEPD2 path.
    SendCommand(0x00); // PANEL SETTING
    SendData(0x1F);

    SendCommand(0x01); // POWER SETTING
    SendData(0x07);
    SendData(0x07);
    SendData(0x3F);
    SendData(0x3F);
    SendData(0x09);

    SendCommand(0x06); // BOOSTER SOFT START
    SendData(0x17);
    SendData(0x17);
    SendData(0x28);
    SendData(0x17);

    SendCommand(0x61); // TRES: 800 x 480
    SendData(0x03);
    SendData(0x20);
    SendData(0x01);
    SendData(0xE0);

    SendCommand(0x15); // DUSPI disabled
    SendData(0x00);

    SendCommand(0x50); // VCOM AND DATA INTERVAL
    SendData(0x29);
    SendData(0x07);

    SendCommand(0x60); // TCON
    SendData(0x22);

    SendCommand(0xE3); // PWS
    SendData(0x22);
}

bool EpaperDisplayT42::InitPanelFullRefresh() {
    HardwareReset();
    ConfigurePanelBase();

    SendCommand(0x00);
    SendData(0x1F); // full-update LUT from OTP

    SendCommand(0x04); // POWER ON
    vTaskDelay(pdMS_TO_TICKS(kRefreshTriggerDelayMs));
    if (!WaitBusyRelease("power-on/full", EPD_BUSY_TIMEOUT_MS)) {
        panel_powered_ = false;
        return false;
    }

    panel_powered_ = true;
    return true;
}

bool EpaperDisplayT42::InitPanelPartialRefresh() {
    HardwareReset();
    ConfigurePanelBase();

    // Waveshare 7.5 V2 partial-refresh mode, also matching GxEPD2's OTP fast
    // partial path for GDEY075T7: fixed temperature waveform at 0x6E.
    SendCommand(0xE0);
    SendData(0x02);
    SendCommand(0xE5);
    SendData(0x6E);

    SendCommand(0x04); // POWER ON
    vTaskDelay(pdMS_TO_TICKS(kRefreshTriggerDelayMs));
    if (!WaitBusyRelease("power-on/partial", EPD_BUSY_TIMEOUT_MS)) {
        panel_powered_ = false;
        return false;
    }

    panel_powered_ = true;
    return true;
}

bool EpaperDisplayT42::SetPartialWindow(
    int x_start, int y_start, int x_end, int y_end) {
    x_start = std::max(0, x_start);
    y_start = std::max(0, y_start);
    x_end = std::min(EPD_WIDTH, x_end);
    y_end = std::min(EPD_HEIGHT, y_end);
    if (x_start >= x_end || y_start >= y_end) {
        return false;
    }

    // Reproduce Waveshare's official byte-aligned partial-window convention.
    const int x_start_byte = x_start / 8;
    const int x_end_byte = (x_end + 7) / 8; // exclusive byte index
    const int panel_x_start = x_start_byte * 8;
    const int panel_x_end = (x_end_byte - 1) * 8;
    const int panel_y_end = y_end - 1;

    SendCommand(0x50);
    SendData(0xA9);
    SendData(0x07);

    SendCommand(0x91); // PARTIAL IN
    SendCommand(0x90); // PARTIAL WINDOW
    SendData(static_cast<uint8_t>((panel_x_start >> 8) & 0xFF));
    SendData(static_cast<uint8_t>(panel_x_start & 0xFF));
    SendData(static_cast<uint8_t>((panel_x_end >> 8) & 0xFF));
    SendData(static_cast<uint8_t>(panel_x_end & 0xFF));
    SendData(static_cast<uint8_t>((y_start >> 8) & 0xFF));
    SendData(static_cast<uint8_t>(y_start & 0xFF));
    SendData(static_cast<uint8_t>((panel_y_end >> 8) & 0xFF));
    SendData(static_cast<uint8_t>(panel_y_end & 0xFF));
    SendData(0x01);
    return true;
}

bool EpaperDisplayT42::RefreshPanelFull() {
    ESP_LOGI(TAG, "Full refresh begin (GDEY075T7 current-plane streaming)");

    if (!InitPanelFullRefresh()) {
        SleepPanel();
        return false;
    }

    SendCommand(0x10); // previous image plane
    if (!StreamSolidPlane(0x00)) {
        ESP_LOGE(TAG, "Previous plane 0x10 streaming failed");
        SleepPanel();
        return false;
    }

    SendCommand(0x13); // current image plane
    if (!StreamCurrentUiToPanel()) {
        ESP_LOGE(TAG, "Current plane 0x13 streaming failed");
        SleepPanel();
        return false;
    }

    SendCommand(0xE0);
    SendData(0x00);
    SendCommand(0x41); // internal temperature sensor
    SendData(0x00);
    SendCommand(0x12); // DISPLAY REFRESH
    vTaskDelay(pdMS_TO_TICKS(kRefreshTriggerDelayMs));

    const bool ok = WaitBusyRelease("display-refresh/full", EPD_BUSY_TIMEOUT_MS);
    SleepPanel();

    if (ok) {
        full_refresh_done_ = true;
        partial_refresh_count_ = 0;
    }
    ESP_LOGI(TAG, "Full refresh %s", ok ? "done" : "failed");
    return ok;
}

bool EpaperDisplayT42::RefreshPanelPartial(uint32_t mask) {
    ESP_LOGI(TAG, "Partial refresh batch begin: mask=0x%08lx", static_cast<unsigned long>(mask));

    if (!InitPanelPartialRefresh()) {
        SleepPanel();
        return false;
    }

    bool ok = true;
    uint32_t refreshed_regions = 0;

    if ((mask & REFRESH_STATUS) != 0) {
        ok = RefreshPartialRegion(kStatusY0, kStatusY1, "status");
        if (ok) ++refreshed_regions;
    }
    if (ok && (mask & REFRESH_USER) != 0) {
        ok = RefreshPartialRegion(kUserY0, kUserY1, "user");
        if (ok) ++refreshed_regions;
    }
    if (ok && (mask & REFRESH_ASSISTANT) != 0) {
        ok = RefreshPartialRegion(kAssistantY0, kAssistantY1, "assistant");
        if (ok) ++refreshed_regions;
    }

    ReleasePartialBuffer();
    SleepPanel();

    if (ok) {
        partial_refresh_count_ += refreshed_regions;
    }

    ESP_LOGI(TAG, "Partial refresh batch %s, count=%lu/%u",
             ok ? "done" : "failed",
             static_cast<unsigned long>(partial_refresh_count_),
             static_cast<unsigned>(kPartialRefreshLimit));
    return ok;
}

bool EpaperDisplayT42::RefreshPartialRegion(int y_start, int y_end, const char* name) {
    ESP_LOGI(TAG, "Partial region %s: x=0..%d y=%d..%d",
             name, EPD_WIDTH - 1, y_start, y_end - 1);

    if (!CaptureUiRegion(y_start, y_end)) {
        ESP_LOGE(TAG, "Capture failed for partial region %s", name);
        return false;
    }

    const bool ok = WriteCapturedPartialRegion(y_start, y_end);
    ReleasePartialBuffer();
    if (!ok) {
        ESP_LOGE(TAG, "Panel write failed for partial region %s", name);
    }
    return ok;
}

void EpaperDisplayT42::SleepPanel() {
    if (spi_ == nullptr || !panel_powered_) {
        return;
    }

    // Waveshare official sleep sequence: restore interval, power off, deep sleep.
    SendCommand(0x50);
    SendData(0xF7);
    SendCommand(0x02);
    vTaskDelay(pdMS_TO_TICKS(20));
    WaitBusyRelease("power-off", 3000);
    SendCommand(0x07);
    SendData(0xA5);
    panel_powered_ = false;
}

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
#include <esp_rom_sys.h>

#define TAG "EpaperT42"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

namespace {
constexpr spi_host_device_t kSpiHost = SPI3_HOST;
constexpr size_t kSpiChunk = 4096;
constexpr size_t kMaxUserBytes = 240;
constexpr size_t kMaxAssistantBytes = 780;
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
             "Ready: 800x480 T42 e-paper, PWR=%d BUSY=%d RST=%d DC=%d CS=%d CLK=%d DIN=%d",
             EPD_PWR_PIN, EPD_BUSY_PIN, EPD_RST_PIN, EPD_DC_PIN,
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

    SleepAndPowerOff();

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
        (1ULL << EPD_PWR_PIN) |
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
    // GPIO34 has no internal pull resistor on classic ESP32. The HAT drives BUSY.
    busy_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    busy_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    busy_cfg.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&busy_cfg) != ESP_OK) {
        return false;
    }

    gpio_set_level(EPD_PWR_PIN, 0);
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
    NotifyRefresh();
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

    // During TTS playback we intentionally do not full-refresh. Assistant
    // sentence_start events accumulate in RAM; when speaking ends the next
    // status change triggers one final e-paper update with the complete answer.
    if (!is_speaking) {
        NotifyRefresh();
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
    if (!speaking_) {
        NotifyRefresh();
    }
}

void EpaperDisplayT42::ShowNotification(const std::string& notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void EpaperDisplayT42::SetEmotion(const char* emotion) {
    // E-paper UI intentionally avoids animated emoji/emotion redraws.
    Display::SetEmotion(emotion != nullptr ? emotion : "");
}

void EpaperDisplayT42::SetChatMessage(const char* role, const char* content) {
    if (role == nullptr || content == nullptr) {
        return;
    }

    Display::SetChatMessage(role, content);

    bool changed = false;
    if (std::strcmp(role, "user") == 0) {
        user_text_ = content;
        assistant_text_.clear();
        changed = true;
    } else if (std::strcmp(role, "assistant") == 0) {
        if (content[0] != '\0') {
            assistant_text_ += content;
            changed = true;
        }
    } else if (std::strcmp(role, "system") == 0) {
        // Hide the startup user-agent string, but keep useful system/alert text
        // such as Wi-Fi configuration instructions.
        if (content[0] != '\0' && std::strstr(content, "bread-compact-esp32") == nullptr) {
            assistant_text_ = content;
            changed = true;
        }
    }

    if (!changed) {
        return;
    }

    if (setup_ui_called_) {
        DisplayLockGuard lock(this);
        UpdateUserLabelLocked();
        UpdateAssistantLabelLocked();
    }

    if (!speaking_) {
        NotifyRefresh();
    }
}

void EpaperDisplayT42::ClearChatMessages() {
    // Preserve the last Q&A on an e-paper screen. A new user utterance replaces
    // it naturally, so idle-state transitions do not erase useful content.
    ESP_LOGI(TAG, "Preserve last Q&A on ClearChatMessages");
}

void EpaperDisplayT42::UpdateStatusBar(bool update_all) {
    (void)update_all;
    // Intentionally no 1 Hz clock/network redraws on e-paper.
}

void EpaperDisplayT42::SetPowerSaveMode(bool on) {
    (void)on;
    // Panel power is controlled per refresh via EPD_PWR_PIN.
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

    // Normal LVGL draws only update the in-memory UI model. Physical refreshes
    // are scheduled explicitly by SetStatus/SetChatMessage/SetupUI.
    if (!self->streaming_refresh_) {
        lv_display_flush_ready(disp);
        return;
    }

    if (self->mono_line_ == nullptr ||
        area->x1 != 0 || area->x2 != (EPD_WIDTH - 1)) {
        ESP_LOGE(TAG, "Unexpected LVGL stream area x=%d..%d y=%d..%d",
                 area->x1, area->x2, area->y1, area->y2);
        self->stream_error_ = true;
        lv_display_flush_ready(disp);
        return;
    }

    auto* pixels = reinterpret_cast<uint16_t*>(color_p);
    const int rows = area->y2 - area->y1 + 1;

    for (int row = 0; row < rows; ++row) {
        std::memset(self->mono_line_, 0xFF, MONO_LINE_BYTES); // 1 = white

        for (int x = 0; x < EPD_WIDTH; ++x) {
            const uint16_t p = pixels[row * EPD_WIDTH + x];
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

        if (self->stream_invert_) {
            for (size_t i = 0; i < MONO_LINE_BYTES; ++i) {
                self->mono_line_[i] = static_cast<uint8_t>(~self->mono_line_[i]);
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

void EpaperDisplayT42::NotifyRefresh() {
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
                   pdMS_TO_TICKS(EPD_REFRESH_DEBOUNCE_MS)) > 0) {
        }

        if (speaking_) {
            ESP_LOGI(TAG, "Refresh deferred while TTS is speaking");
            continue;
        }

        if (!RefreshPanelFull()) {
            ESP_LOGE(TAG, "Panel refresh failed");
        }
    }
}

bool EpaperDisplayT42::StreamCurrentUiToPanel(bool invert) {
    if (display_ == nullptr) {
        return false;
    }

    stream_error_ = false;
    streaming_refresh_ = true;
    stream_invert_ = invert;

    lvgl_port_lock(0);
    lv_obj_t* screen = lv_display_get_screen_active(display_);
    if (screen != nullptr) {
        lv_obj_invalidate(screen);
        lv_refr_now(display_);
    } else {
        stream_error_ = true;
    }
    lvgl_port_unlock();

    stream_invert_ = false;
    streaming_refresh_ = false;
    return !stream_error_;
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

void EpaperDisplayT42::PowerRail(bool on) {
    gpio_set_level(EPD_PWR_PIN, on ? 1 : 0);
    power_rail_on_ = on;
    if (on) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

void EpaperDisplayT42::HardwareReset() {
    // Verified with the working diagnostic build / Waveshare Rev2.3 timing.
    gpio_set_level(EPD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_RST_PIN, 0);
    esp_rom_delay_us(2000);
    gpio_set_level(EPD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
}

bool EpaperDisplayT42::WaitBusyRelease(const char* reason, uint32_t timeout_ms) {
    const TickType_t start = xTaskGetTickCount();
    while (gpio_get_level(EPD_BUSY_PIN) == 0) {
        if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(timeout_ms)) {
            ESP_LOGE(TAG, "BUSY timeout while %s, level=%d",
                     reason, gpio_get_level(EPD_BUSY_PIN));
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

bool EpaperDisplayT42::InitPanelFullRefresh() {
    PowerRail(true);
    HardwareReset();

    // Sequence proven by the split-screen hardware self-test after CS wiring
    // was corrected. This mirrors the Waveshare 7.5-inch V2 full-refresh path.
    SendCommand(0x01); // POWER SETTING
    SendData(0x07);
    SendData(0x07);
    SendData(0x3F);
    SendData(0x3F);

    SendCommand(0x06); // BOOSTER SOFT START
    SendData(0x17);
    SendData(0x17);
    SendData(0x28);
    SendData(0x17);

    SendCommand(0x04); // POWER ON
    vTaskDelay(pdMS_TO_TICKS(100));
    if (!WaitBusyRelease("power-on", 5000)) {
        return false;
    }

    SendCommand(0x00); // PANEL SETTING
    SendData(0x1F);

    SendCommand(0x61); // 800 x 480
    SendData(0x03);
    SendData(0x20);
    SendData(0x01);
    SendData(0xE0);

    SendCommand(0x15);
    SendData(0x00);

    SendCommand(0x50); // VCOM / DATA INTERVAL
    SendData(0x10);
    SendData(0x07);

    SendCommand(0x60); // TCON
    SendData(0x22);

    return true;
}

bool EpaperDisplayT42::RefreshPanelFull() {
    ESP_LOGI(TAG, "Full refresh begin (compact UI, two-plane streaming)");

    if (!InitPanelFullRefresh()) {
        SleepAndPowerOff();
        return false;
    }

    // Waveshare's monochrome 7.5-inch path writes the image to 0x10 and the
    // inverse image to 0x13. Render the LVGL screen twice without a 48KB frame.
    SendCommand(0x10);
    if (!StreamCurrentUiToPanel(false)) {
        ESP_LOGE(TAG, "LVGL plane 0x10 streaming failed");
        SleepAndPowerOff();
        return false;
    }

    SendCommand(0x13);
    if (!StreamCurrentUiToPanel(true)) {
        ESP_LOGE(TAG, "LVGL plane 0x13 streaming failed");
        SleepAndPowerOff();
        return false;
    }

    SendCommand(0x12); // DISPLAY REFRESH
    vTaskDelay(pdMS_TO_TICKS(100));
    const bool ok = WaitBusyRelease("display-refresh", EPD_BUSY_TIMEOUT_MS);

    SleepAndPowerOff();
    ESP_LOGI(TAG, "Full refresh %s", ok ? "done" : "failed");
    return ok;
}

void EpaperDisplayT42::SleepAndPowerOff() {
    if (spi_ == nullptr || !power_rail_on_) {
        return;
    }

    SendCommand(0x02); // POWER OFF
    vTaskDelay(pdMS_TO_TICKS(100));
    WaitBusyRelease("power-off", 2000);
    PowerRail(false);
}

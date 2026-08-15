#include "epaper_display_t42.h"
#include "config.h"

#include <algorithm>
#include <cstring>

#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>

#define TAG "EpaperT42"

namespace {
constexpr spi_host_device_t kSpiHost = SPI3_HOST;
constexpr size_t kSpiChunk = 4096;
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
             "Ready: 800x480 T42/UC8179, PWR=%d BUSY=%d RST=%d DC=%d CS=%d CLK=%d DIN=%d",
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
    // GPIO34 is input-only and has no internal pull-up on classic ESP32.
    // The Waveshare HAT actively drives BUSY, so no pull resistor is needed.
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

    // One monochrome scan line is only 100 bytes for an 800px-wide panel.
    mono_line_ = static_cast<uint8_t*>(
        heap_caps_malloc(MONO_LINE_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_8BIT));
    if (mono_line_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate %u-byte monochrome line buffer",
                 static_cast<unsigned>(MONO_LINE_BYTES));
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

    // 800 x 4 rows x RGB565 = 6,400 bytes instead of a permanent 48KB
    // monochrome framebuffer plus the LVGL buffer.
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

void EpaperDisplayT42::LvglFlushCb(
    lv_display_t* disp,
    const lv_area_t* area,
    uint8_t* color_p) {
    auto* self = static_cast<EpaperDisplayT42*>(lv_display_get_user_data(disp));
    if (self == nullptr) {
        lv_display_flush_ready(disp);
        return;
    }

    // Normal Xiaozhi UI updates only mark the screen dirty. We deliberately do
    // not keep a full framebuffer. After the UI has been quiet for a short
    // period the refresh task invalidates the full screen and LVGL renders it
    // again, four rows at a time, directly into the e-paper controller RAM.
    if (!self->streaming_refresh_) {
        self->NotifyRefresh();
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

            // RGB565 luminance approximation. T42 is monochrome, so Xiaozhi's
            // colored UI is thresholded to black/white.
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

        // Coalesce Xiaozhi's rapidly changing state/text into one e-paper
        // update. This is especially important for streamed AI responses.
        while (ulTaskNotifyTake(
                   pdTRUE,
                   pdMS_TO_TICKS(EPD_REFRESH_DEBOUNCE_MS)) > 0) {
        }

        if (!RefreshPanelFull()) {
            ESP_LOGE(TAG, "Panel refresh failed");
        }
    }
}

bool EpaperDisplayT42::StreamCurrentUiToPanel() {
    if (display_ == nullptr) {
        return false;
    }

    stream_error_ = false;
    streaming_refresh_ = true;

    // LVGL 9 can redraw invalidated content immediately. In PARTIAL render
    // mode the 6.4KB buffer yields full-width strips, which the flush callback
    // converts to 1bpp and sends straight to UC8179 RAM.
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

void EpaperDisplayT42::SendRepeated(uint8_t value, size_t len) {
    uint8_t block[256];
    std::memset(block, value, sizeof(block));

    gpio_set_level(EPD_DC_PIN, 1);
    while (len > 0) {
        const size_t n = std::min(len, sizeof(block));
        ESP_ERROR_CHECK_WITHOUT_ABORT(SpiWrite(block, n));
        len -= n;
    }
}

void EpaperDisplayT42::PowerRail(bool on) {
    gpio_set_level(EPD_PWR_PIN, on ? 1 : 0);
    power_rail_on_ = on;
    if (on) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

void EpaperDisplayT42::HardwareReset() {
    gpio_set_level(EPD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_RST_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(50));
}

bool EpaperDisplayT42::WaitBusy(const char* reason, uint32_t timeout_ms) {
    const TickType_t start = xTaskGetTickCount();
    const TickType_t timeout_ticks = pdMS_TO_TICKS(timeout_ms);

    // UC8179 / Waveshare 7.5" HAT BUSY is active LOW in this verified setup.
    while (gpio_get_level(EPD_BUSY_PIN) == 0) {
        if ((xTaskGetTickCount() - start) > timeout_ticks) {
            ESP_LOGE(TAG, "BUSY timeout while %s", reason);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    return true;
}

bool EpaperDisplayT42::InitPanelFullRefresh() {
    PowerRail(true);
    HardwareReset();

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

    SendCommand(0x61); // RESOLUTION: 800 x 480
    SendData(static_cast<uint8_t>(EPD_WIDTH >> 8));
    SendData(static_cast<uint8_t>(EPD_WIDTH & 0xFF));
    SendData(static_cast<uint8_t>(EPD_HEIGHT >> 8));
    SendData(static_cast<uint8_t>(EPD_HEIGHT & 0xFF));

    SendCommand(0x15); // DUAL SPI disabled
    SendData(0x00);

    SendCommand(0x50); // VCOM / DATA INTERVAL
    SendData(0x29);
    SendData(0x07);

    SendCommand(0x60); // TCON
    SendData(0x22);

    SendCommand(0xE3); // POWER SAVING
    SendData(0x22);

    SendCommand(0x04); // POWER ON
    return WaitBusy("power-on", 3000);
}

bool EpaperDisplayT42::RefreshPanelFull() {
    ESP_LOGI(TAG, "Full refresh begin (low-memory streaming)");

    if (!InitPanelFullRefresh()) {
        SleepAndPowerOff();
        return false;
    }

    // Initialize previous-image RAM to white on the first update.
    if (first_refresh_) {
        SendCommand(0x10);
        SendRepeated(0xFF, static_cast<size_t>(EPD_WIDTH) * EPD_HEIGHT / 8);
        first_refresh_ = false;
    }

    // Select current-image RAM, then ask LVGL to render the entire UI. The
    // flush callback streams each 800px strip directly to the controller.
    SendCommand(0x13);
    if (!StreamCurrentUiToPanel()) {
        ESP_LOGE(TAG, "LVGL-to-e-paper streaming failed");
        SleepAndPowerOff();
        return false;
    }

    // Use UC8179's internal temperature sensor for a conservative full update.
    SendCommand(0xE0);
    SendData(0x00);
    SendCommand(0x41);
    SendData(0x00);

    SendCommand(0x12); // DISPLAY REFRESH
    const bool ok = WaitBusy("full-refresh", EPD_BUSY_TIMEOUT_MS);

    SleepAndPowerOff();
    ESP_LOGI(TAG, "Full refresh %s", ok ? "done" : "failed");
    return ok;
}

void EpaperDisplayT42::SleepAndPowerOff() {
    if (spi_ == nullptr || !power_rail_on_) {
        return;
    }

    SendCommand(0x02); // POWER OFF
    WaitBusy("power-off", 1500);

    SendCommand(0x07); // DEEP SLEEP
    SendData(0xA5);

    vTaskDelay(pdMS_TO_TICKS(5));
    PowerRail(false);
}

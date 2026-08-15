from pathlib import Path
import re

hp = Path('main/boards/bread-compact-esp32/epaper_display_t42.h')
h = hp.read_text()
anchor = "    volatile bool stream_error_ = false;\n"
assert anchor in h
h = h.replace(anchor, anchor +
    "    int stream_rows_ = 0;\n"
    "    int stream_flushes_ = 0;\n"
    "    int stream_next_y_ = 0;\n", 1)
hp.write_text(h)

sp = Path('main/boards/bread-compact-esp32/epaper_display_t42.cc')
s = sp.read_text()

old = 'constexpr size_t kSpiChunk = 4096;\n'
assert old in s
s = s.replace(old, 'constexpr uint32_t kSpiHalfPeriodUs = 1;\n', 1)

new_hw = r'''bool EpaperDisplayT42::InitializeHardware() {
    gpio_config_t out_cfg = {};
    out_cfg.pin_bit_mask =
        (1ULL << EPD_PWR_PIN) |
        (1ULL << EPD_RST_PIN) |
        (1ULL << EPD_DC_PIN) |
        (1ULL << EPD_CS_PIN) |
        (1ULL << EPD_SCLK_PIN) |
        (1ULL << EPD_MOSI_PIN);
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

    gpio_set_level(EPD_CS_PIN, 1);
    gpio_set_level(EPD_SCLK_PIN, 0);
    gpio_set_level(EPD_MOSI_PIN, 0);
    gpio_set_level(EPD_RST_PIN, 1);
    gpio_set_level(EPD_DC_PIN, 1);

    gpio_set_level(EPD_PWR_PIN, 1);
    power_rail_on_ = true;
    vTaskDelay(pdMS_TO_TICKS(100));

    mono_line_ = static_cast<uint8_t*>(
        heap_caps_malloc(MONO_LINE_BYTES, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    if (mono_line_ == nullptr) {
        ESP_LOGE(TAG, "Failed to allocate monochrome line buffer");
        return false;
    }

    ESP_LOGI(TAG, "Hardware ready: Waveshare row-framed bit-bang SPI, PWR held HIGH");
    return true;
}'''
pattern = (r'bool EpaperDisplayT42::InitializeHardware\(\) \{.*?\n\}'
           r'(?=\n\nbool EpaperDisplayT42::InitializeLvgl)')
s, n = re.subn(pattern, new_hw, s, count=1, flags=re.S)
assert n == 1

anchor = "        area->x1 != 0 || area->x2 != (EPD_WIDTH - 1)) {\n"
assert anchor in s
s = s.replace(anchor,
    "        area->x1 != 0 || area->x2 != (EPD_WIDTH - 1) ||\n"
    "        area->y1 < 0 || area->y2 >= EPD_HEIGHT) {\n", 1)

anchor = "    auto* pixels = reinterpret_cast<uint16_t*>(color_p);\n"
assert anchor in s
s = s.replace(anchor,
    "    if (area->y1 != self->stream_next_y_) {\n"
    "        ESP_LOGE(TAG, \"Non-contiguous LVGL stream: expected y=%d got %d..%d\",\n"
    "                 self->stream_next_y_, area->y1, area->y2);\n"
    "        self->stream_error_ = true;\n"
    "        lv_display_flush_ready(disp);\n"
    "        return;\n"
    "    }\n\n" + anchor, 1)

old = (
    "        gpio_set_level(EPD_DC_PIN, 1);\n"
    "        if (self->SpiWrite(self->mono_line_, MONO_LINE_BYTES) != ESP_OK) {\n"
    "            self->stream_error_ = true;\n"
    "            break;\n"
    "        }\n"
    "    }\n\n"
    "    lv_display_flush_ready(disp);\n"
)
new = (
    "        gpio_set_level(EPD_DC_PIN, 1);\n"
    "        gpio_set_level(EPD_CS_PIN, 0);\n"
    "        const esp_err_t row_err = self->SpiWrite(self->mono_line_, MONO_LINE_BYTES);\n"
    "        gpio_set_level(EPD_CS_PIN, 1);\n"
    "        if (row_err != ESP_OK) {\n"
    "            self->stream_error_ = true;\n"
    "            break;\n"
    "        }\n"
    "        ++self->stream_rows_;\n"
    "        ++self->stream_next_y_;\n"
    "    }\n\n"
    "    ++self->stream_flushes_;\n"
    "    lv_display_flush_ready(disp);\n"
)
assert old in s
s = s.replace(old, new, 1)

new_stream = r'''bool EpaperDisplayT42::StreamCurrentUiToPanel(bool invert) {
    if (display_ == nullptr) {
        return false;
    }

    stream_error_ = false;
    streaming_refresh_ = true;
    stream_invert_ = invert;
    stream_rows_ = 0;
    stream_flushes_ = 0;
    stream_next_y_ = 0;

    lvgl_port_lock(0);
    lv_obj_t* screen = lv_display_get_screen_active(display_);
    if (screen != nullptr) {
        lv_area_t full_area;
        full_area.x1 = 0;
        full_area.y1 = 0;
        full_area.x2 = EPD_WIDTH - 1;
        full_area.y2 = EPD_HEIGHT - 1;
        const lv_result_t inv = lv_obj_invalidate_area(screen, &full_area);
        if (inv != LV_RESULT_OK) {
            ESP_LOGE(TAG, "Full-screen LVGL invalidation failed: %d", static_cast<int>(inv));
            stream_error_ = true;
        } else {
            lv_refr_now(display_);
        }
    } else {
        stream_error_ = true;
    }
    lvgl_port_unlock();

    const bool complete =
        !stream_error_ && stream_rows_ == EPD_HEIGHT && stream_next_y_ == EPD_HEIGHT;
    ESP_LOGI(TAG,
             "LVGL plane stream: invert=%d rows=%d/%d flushes=%d result=%s",
             invert ? 1 : 0,
             stream_rows_, EPD_HEIGHT, stream_flushes_,
             complete ? "complete" : "INCOMPLETE");
    if (!complete) {
        stream_error_ = true;
    }

    stream_invert_ = false;
    streaming_refresh_ = false;
    return !stream_error_;
}'''
pattern = (r'bool EpaperDisplayT42::StreamCurrentUiToPanel\(bool invert\) \{.*?\n\}'
           r'(?=\n\nesp_err_t EpaperDisplayT42::SpiWrite)')
s, n = re.subn(pattern, new_stream, s, count=1, flags=re.S)
assert n == 1

new_spi = r'''esp_err_t EpaperDisplayT42::SpiWrite(const uint8_t* data, size_t len) {
    if (data == nullptr || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }

    // Match Waveshare DEV_SPI_Write_nByte semantics: CS is controlled by the
    // caller and remains LOW for the complete command/data transaction.
    for (size_t i = 0; i < len; ++i) {
        uint8_t value = data[i];
        for (int bit = 0; bit < 8; ++bit) {
            gpio_set_level(EPD_MOSI_PIN, (value & 0x80) ? 1 : 0);
            value <<= 1;
            gpio_set_level(EPD_SCLK_PIN, 1);
            esp_rom_delay_us(kSpiHalfPeriodUs);
            gpio_set_level(EPD_SCLK_PIN, 0);
            esp_rom_delay_us(kSpiHalfPeriodUs);
        }
    }
    return ESP_OK;
}'''
pattern = (r'esp_err_t EpaperDisplayT42::SpiWrite\(const uint8_t\* data, size_t len\) \{.*?\n\}'
           r'(?=\n\nvoid EpaperDisplayT42::SendCommand)')
s, n = re.subn(pattern, new_spi, s, count=1, flags=re.S)
assert n == 1

new_send = r'''void EpaperDisplayT42::SendCommand(uint8_t cmd) {
    gpio_set_level(EPD_DC_PIN, 0);
    gpio_set_level(EPD_CS_PIN, 0);
    ESP_ERROR_CHECK_WITHOUT_ABORT(SpiWrite(&cmd, 1));
    gpio_set_level(EPD_CS_PIN, 1);
}

void EpaperDisplayT42::SendData(uint8_t data) {
    gpio_set_level(EPD_DC_PIN, 1);
    gpio_set_level(EPD_CS_PIN, 0);
    ESP_ERROR_CHECK_WITHOUT_ABORT(SpiWrite(&data, 1));
    gpio_set_level(EPD_CS_PIN, 1);
}'''
pattern = (r'void EpaperDisplayT42::SendCommand\(uint8_t cmd\) \{.*?\n\}\n\n'
           r'void EpaperDisplayT42::SendData\(uint8_t data\) \{.*?\n\}')
s, n = re.subn(pattern, new_send, s, count=1, flags=re.S)
assert n == 1

new_reset = r'''void EpaperDisplayT42::HardwareReset() {
    gpio_set_level(EPD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
    gpio_set_level(EPD_RST_PIN, 0);
    esp_rom_delay_us(2000);
    gpio_set_level(EPD_RST_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(20));
}'''
pattern = (r'void EpaperDisplayT42::HardwareReset\(\) \{.*?\n\}'
           r'(?=\n\nbool EpaperDisplayT42::WaitBusyRelease)')
s, n = re.subn(pattern, new_reset, s, count=1, flags=re.S)
assert n == 1

new_wait = r'''bool EpaperDisplayT42::WaitBusyRelease(const char* reason, uint32_t timeout_ms) {
    const TickType_t start = xTaskGetTickCount();
    ESP_LOGI(TAG, "%s: BUSY wait start level=%d", reason, gpio_get_level(EPD_BUSY_PIN));

    do {
        vTaskDelay(pdMS_TO_TICKS(5));
        if (gpio_get_level(EPD_BUSY_PIN) != 0) {
            const uint32_t elapsed =
                static_cast<uint32_t>((xTaskGetTickCount() - start) * portTICK_PERIOD_MS);
            vTaskDelay(pdMS_TO_TICKS(5));
            ESP_LOGI(TAG, "%s: BUSY released HIGH after %u ms",
                     reason, static_cast<unsigned>(elapsed));
            return true;
        }
    } while ((xTaskGetTickCount() - start) <= pdMS_TO_TICKS(timeout_ms));

    ESP_LOGW(TAG, "%s: BUSY still LOW after %u ms; using timed fallback",
             reason, static_cast<unsigned>(timeout_ms));
    return false;
}'''
pattern = (r'bool EpaperDisplayT42::WaitBusyRelease\(const char\* reason, uint32_t timeout_ms\) \{.*?\n\}'
           r'(?=\n\nbool EpaperDisplayT42::InitPanelFullRefresh)')
s, n = re.subn(pattern, new_wait, s, count=1, flags=re.S)
assert n == 1

new_init = r'''bool EpaperDisplayT42::InitPanelFullRefresh() {
    if (!power_rail_on_) {
        PowerRail(true);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    HardwareReset();
    ESP_LOGI(TAG, "After reset BUSY=%d", gpio_get_level(EPD_BUSY_PIN));

    SendCommand(0x01);
    SendData(0x07);
    SendData(0x07);
    SendData(0x3F);
    SendData(0x3F);

    SendCommand(0x06);
    SendData(0x17);
    SendData(0x17);
    SendData(0x28);
    SendData(0x17);

    SendCommand(0x04);
    vTaskDelay(pdMS_TO_TICKS(100));
    if (!WaitBusyRelease("power-on", 3000)) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

    SendCommand(0x00);
    SendData(0x1F);

    SendCommand(0x61);
    SendData(0x03);
    SendData(0x20);
    SendData(0x01);
    SendData(0xE0);

    SendCommand(0x15);
    SendData(0x00);

    SendCommand(0x50);
    SendData(0x10);
    SendData(0x07);

    SendCommand(0x60);
    SendData(0x22);
    return true;
}'''
pattern = (r'bool EpaperDisplayT42::InitPanelFullRefresh\(\) \{.*?\n\}'
           r'(?=\n\nbool EpaperDisplayT42::RefreshPanelFull)')
s, n = re.subn(pattern, new_init, s, count=1, flags=re.S)
assert n == 1

new_refresh = r'''bool EpaperDisplayT42::RefreshPanelFull() {
    ESP_LOGI(TAG, "Full refresh begin (Waveshare row-framed path)");

    if (!InitPanelFullRefresh()) {
        return false;
    }

    static bool first_refresh = true;
    if (first_refresh) {
        ESP_LOGI(TAG, "Initial official white clear begin");

        std::memset(mono_line_, 0xFF, MONO_LINE_BYTES);
        SendCommand(0x10);
        gpio_set_level(EPD_DC_PIN, 1);
        for (int y = 0; y < EPD_HEIGHT; ++y) {
            gpio_set_level(EPD_CS_PIN, 0);
            const esp_err_t err = SpiWrite(mono_line_, MONO_LINE_BYTES);
            gpio_set_level(EPD_CS_PIN, 1);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Initial clear 0x10 row %d failed", y);
                return false;
            }
        }

        std::memset(mono_line_, 0x00, MONO_LINE_BYTES);
        SendCommand(0x13);
        gpio_set_level(EPD_DC_PIN, 1);
        for (int y = 0; y < EPD_HEIGHT; ++y) {
            gpio_set_level(EPD_CS_PIN, 0);
            const esp_err_t err = SpiWrite(mono_line_, MONO_LINE_BYTES);
            gpio_set_level(EPD_CS_PIN, 1);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "Initial clear 0x13 row %d failed", y);
                return false;
            }
        }

        SendCommand(0x12);
        vTaskDelay(pdMS_TO_TICKS(100));
        if (!WaitBusyRelease("initial-clear", 8000)) {
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
        ESP_LOGI(TAG, "Initial official white clear done");
        first_refresh = false;

        // Re-run the official init before loading the actual UI image.
        if (!InitPanelFullRefresh()) {
            return false;
        }
    }

    SendCommand(0x10);
    if (!StreamCurrentUiToPanel(false)) {
        ESP_LOGE(TAG, "LVGL plane 0x10 streaming failed");
        return false;
    }

    SendCommand(0x13);
    if (!StreamCurrentUiToPanel(true)) {
        ESP_LOGE(TAG, "LVGL plane 0x13 streaming failed");
        return false;
    }

    SendCommand(0x12);
    vTaskDelay(pdMS_TO_TICKS(100));
    const bool busy_ok = WaitBusyRelease("display-refresh", 8000);
    if (!busy_ok) {
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    ESP_LOGI(TAG, "Full refresh done (%s)",
             busy_ok ? "BUSY confirmed" : "timed fallback");
    return true;
}'''
pattern = (r'bool EpaperDisplayT42::RefreshPanelFull\(\) \{.*?\n\}'
           r'(?=\n\nvoid EpaperDisplayT42::SleepAndPowerOff)')
s, n = re.subn(pattern, new_refresh, s, count=1, flags=re.S)
assert n == 1

new_sleep = r'''void EpaperDisplayT42::SleepAndPowerOff() {
    if (!power_rail_on_) {
        return;
    }

    SendCommand(0x50);
    SendData(0xF7);
    SendCommand(0x02);
    vTaskDelay(pdMS_TO_TICKS(100));
    WaitBusyRelease("power-off", 2000);
    SendCommand(0x07);
    SendData(0xA5);
    PowerRail(false);
}'''
pattern = r'void EpaperDisplayT42::SleepAndPowerOff\(\) \{.*?\n\}'
s, n = re.subn(pattern, new_sleep, s, count=1, flags=re.S)
assert n == 1

sp.write_text(s)
print('Run19 patch applied: Waveshare row-framed 100-byte transfers + initial official clear')

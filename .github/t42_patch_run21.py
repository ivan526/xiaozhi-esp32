from pathlib import Path
import re

# Run21: combine the exact hardware-SPI transport that produced the successful
# split-screen diagnostic image with non-blocking BUSY handling and a race-free
# LVGL full-screen stream.

hp = Path('main/boards/bread-compact-esp32/epaper_display_t42.h')
h = hp.read_text()
anchor = "    volatile bool stream_error_ = false;\n"
assert anchor in h
h = h.replace(anchor, anchor +
    "    int stream_rows_ = 0;\n"
    "    int stream_flushes_ = 0;\n"
    "    int stream_next_y_ = 0;\n", 1)
hp.write_text(h)

cp = Path('main/boards/bread-compact-esp32/config.h')
c = cp.read_text()
old_clock = '#define EPD_SPI_CLOCK_HZ        (4 * 1000 * 1000)\n'
assert old_clock in c
c = c.replace(old_clock, '#define EPD_SPI_CLOCK_HZ        (2 * 1000 * 1000)\n', 1)
cp.write_text(c)

sp = Path('main/boards/bread-compact-esp32/epaper_display_t42.cc')
s = sp.read_text()

# Match the successful diagnostic transport exactly: HW SPI, 2 MHz, one
# transaction per 100-byte panel row, max transfer size 256 bytes.
old_chunk = 'constexpr size_t kSpiChunk = 4096;\n'
assert old_chunk in s
s = s.replace(old_chunk, 'constexpr size_t kSpiChunk = 256;\n', 1)

# Validate that LVGL gives us the full 800-pixel row strips in order.
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
    "        if (self->SpiWrite(self->mono_line_, MONO_LINE_BYTES) != ESP_OK) {\n"
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

# Drain any pending normal LVGL invalidations while streaming is disabled, then
# atomically enable the physical stream under the LVGL lock. This prevents theme
# or status updates from being mistaken for panel-plane data between 0x10/0x13.
new_stream = r'''bool EpaperDisplayT42::StreamCurrentUiToPanel(bool invert) {
    if (display_ == nullptr) {
        return false;
    }

    if (!lvgl_port_lock(0)) {
        ESP_LOGE(TAG, "Failed to lock LVGL for panel stream");
        return false;
    }

    // Drain stale/pending partial invalidations with physical streaming disabled.
    streaming_refresh_ = false;
    lv_refr_now(display_);

    stream_error_ = false;
    stream_invert_ = invert;
    stream_rows_ = 0;
    stream_flushes_ = 0;
    stream_next_y_ = 0;

    lv_obj_t* screen = lv_display_get_screen_active(display_);
    if (screen == nullptr) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "Active LVGL screen is null");
        return false;
    }

    lv_area_t full_area;
    full_area.x1 = 0;
    full_area.y1 = 0;
    full_area.x2 = EPD_WIDTH - 1;
    full_area.y2 = EPD_HEIGHT - 1;

    const lv_result_t inv = lv_obj_invalidate_area(screen, &full_area);
    if (inv != LV_RESULT_OK) {
        lvgl_port_unlock();
        ESP_LOGE(TAG, "Full-screen LVGL invalidation failed: %d", static_cast<int>(inv));
        return false;
    }

    streaming_refresh_ = true;
    lv_refr_now(display_);
    streaming_refresh_ = false;
    stream_invert_ = false;

    const bool complete =
        !stream_error_ && stream_rows_ == EPD_HEIGHT && stream_next_y_ == EPD_HEIGHT;
    ESP_LOGI(TAG,
             "LVGL plane stream: invert=%d rows=%d/%d flushes=%d result=%s",
             invert ? 1 : 0,
             stream_rows_, EPD_HEIGHT, stream_flushes_,
             complete ? "complete" : "INCOMPLETE");

    lvgl_port_unlock();
    return complete;
}'''
pattern = (r'bool EpaperDisplayT42::StreamCurrentUiToPanel\(bool invert\) \{.*?\n\}'
           r'(?=\n\nesp_err_t EpaperDisplayT42::SpiWrite)')
s, n = re.subn(pattern, new_stream, s, count=1, flags=re.S)
assert n == 1, 'StreamCurrentUiToPanel replacement failed'

# Same register sequence as the successful diagnostic. BUSY timeout is logged but
# no longer aborts data transfer, matching the diagnostic behavior that actually
# changed the panel image.
new_init = r'''bool EpaperDisplayT42::InitPanelFullRefresh() {
    // Start every refresh from the same electrical state as the successful
    // one-shot diagnostic: HAT off -> on -> 50 ms -> reset.
    if (power_rail_on_) {
        PowerRail(false);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    PowerRail(true);
    vTaskDelay(pdMS_TO_TICKS(50));
    HardwareReset();
    ESP_LOGI(TAG, "After reset BUSY=%d", gpio_get_level(EPD_BUSY_PIN));

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
        ESP_LOGW(TAG, "POWER ON BUSY did not release; continuing exactly like proven diagnostic");
        vTaskDelay(pdMS_TO_TICKS(500));
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
assert n == 1, 'InitPanelFullRefresh replacement failed'

# First refresh deliberately clears white using the exact diagnostic hardware SPI
# path. Then hard-cycle and load the LVGL UI. The old split diagnostic image must
# disappear if this transport reaches the UC8179 RAM/refresh engine.
new_refresh = r'''bool EpaperDisplayT42::RefreshPanelFull() {
    ESP_LOGI(TAG, "Full refresh begin (PROVEN HW-SPI 2MHz diagnostic transport)");

    if (!InitPanelFullRefresh()) {
        return false;
    }

    static bool first_refresh = true;
    if (first_refresh) {
        ESP_LOGI(TAG, "HW-SPI official white clear begin");

        std::memset(mono_line_, 0xFF, MONO_LINE_BYTES);
        SendCommand(0x10);
        gpio_set_level(EPD_DC_PIN, 1);
        for (int y = 0; y < EPD_HEIGHT; ++y) {
            if (SpiWrite(mono_line_, MONO_LINE_BYTES) != ESP_OK) {
                ESP_LOGE(TAG, "White clear 0x10 row %d failed", y);
                return false;
            }
        }

        std::memset(mono_line_, 0x00, MONO_LINE_BYTES);
        SendCommand(0x13);
        gpio_set_level(EPD_DC_PIN, 1);
        for (int y = 0; y < EPD_HEIGHT; ++y) {
            if (SpiWrite(mono_line_, MONO_LINE_BYTES) != ESP_OK) {
                ESP_LOGE(TAG, "White clear 0x13 row %d failed", y);
                return false;
            }
        }

        SendCommand(0x12);
        vTaskDelay(pdMS_TO_TICKS(100));
        const bool clear_busy = WaitBusyRelease("hwspi-initial-clear", 12000);
        if (!clear_busy) {
            ESP_LOGW(TAG, "Initial clear BUSY unavailable; allow fixed 3 s settling");
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
        ESP_LOGI(TAG, "HW-SPI official white clear done (%s)",
                 clear_busy ? "BUSY confirmed" : "timed fallback");

        // Hard power-cycle exactly as the diagnostic before loading the UI.
        PowerRail(false);
        vTaskDelay(pdMS_TO_TICKS(500));
        first_refresh = false;
        if (!InitPanelFullRefresh()) {
            return false;
        }
    }

    SendCommand(0x10);
    if (!StreamCurrentUiToPanel(false)) {
        ESP_LOGE(TAG, "LVGL plane 0x10 streaming failed");
        PowerRail(false);
        return false;
    }

    SendCommand(0x13);
    if (!StreamCurrentUiToPanel(true)) {
        ESP_LOGE(TAG, "LVGL plane 0x13 streaming failed");
        PowerRail(false);
        return false;
    }

    SendCommand(0x12);
    vTaskDelay(pdMS_TO_TICKS(100));
    const bool busy_ok = WaitBusyRelease("display-refresh", 12000);
    if (!busy_ok) {
        ESP_LOGW(TAG, "Display BUSY unavailable; allow fixed 3 s settling");
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    ESP_LOGI(TAG, "Full refresh done (%s)",
             busy_ok ? "BUSY confirmed" : "timed fallback");

    // Mirror the diagnostic end state: power the HAT down; e-paper keeps image.
    SendCommand(0x02);
    vTaskDelay(pdMS_TO_TICKS(100));
    WaitBusyRelease("power-off", 2000);
    PowerRail(false);
    return true;
}'''
pattern = (r'bool EpaperDisplayT42::RefreshPanelFull\(\) \{.*?\n\}'
           r'(?=\n\nvoid EpaperDisplayT42::SleepAndPowerOff)')
s, n = re.subn(pattern, new_refresh, s, count=1, flags=re.S)
assert n == 1, 'RefreshPanelFull replacement failed'

sp.write_text(s)
print('Run21 applied: proven diagnostic HW SPI 2MHz + BUSY fallback + LVGL race fix')

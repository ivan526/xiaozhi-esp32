from pathlib import Path
import re

# Run23: build on Run21's proven 2 MHz hardware-SPI transport, but perform the
# only cold POWER ON before Wi-Fi/audio/LVGL worker activity. Keep the HAT and
# UC8179 powered afterwards so normal UI refreshes do not repeat 0x04.

sp = Path('main/boards/bread-compact-esp32/epaper_display_t42.cc')
s = sp.read_text()

# Insert a one-shot pre-WiFi clear immediately after hardware initialization,
# before the e-paper refresh task and LVGL are started. This reproduces the
# successful diagnostic execution context as closely as possible.
anchor = '''    if (!InitializeHardware()) {
        ESP_LOGE(TAG, "Hardware initialization failed");
        return;
    }

    if (xTaskCreate(
'''
assert anchor in s, 'constructor anchor not found'
insert = '''    if (!InitializeHardware()) {
        ESP_LOGE(TAG, "Hardware initialization failed");
        return;
    }

    ESP_LOGI(TAG, "PRE-WIFI cold panel init begin (no WiFi/audio/LVGL activity yet)");
    if (InitPanelFullRefresh()) {
        ESP_LOGI(TAG, "PRE-WIFI official white clear begin");

        std::memset(mono_line_, 0xFF, MONO_LINE_BYTES);
        SendCommand(0x10);
        gpio_set_level(EPD_DC_PIN, 1);
        for (int y = 0; y < EPD_HEIGHT; ++y) {
            if (SpiWrite(mono_line_, MONO_LINE_BYTES) != ESP_OK) {
                ESP_LOGE(TAG, "PRE-WIFI white clear 0x10 row %d failed", y);
                break;
            }
        }

        std::memset(mono_line_, 0x00, MONO_LINE_BYTES);
        SendCommand(0x13);
        gpio_set_level(EPD_DC_PIN, 1);
        for (int y = 0; y < EPD_HEIGHT; ++y) {
            if (SpiWrite(mono_line_, MONO_LINE_BYTES) != ESP_OK) {
                ESP_LOGE(TAG, "PRE-WIFI white clear 0x13 row %d failed", y);
                break;
            }
        }

        SendCommand(0x12);
        vTaskDelay(pdMS_TO_TICKS(100));
        const bool prewifi_busy = WaitBusyRelease("prewifi-clear", 12000);
        if (!prewifi_busy) {
            ESP_LOGW(TAG, "PRE-WIFI clear BUSY unavailable; fixed 3 s settling");
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
        ESP_LOGI(TAG,
                 "PRE-WIFI official white clear done (%s); PWR stays HIGH, controller stays warm",
                 prewifi_busy ? "BUSY confirmed" : "timed fallback");
    } else {
        ESP_LOGE(TAG, "PRE-WIFI panel init failed; application will continue for diagnostics");
    }

    if (xTaskCreate(
'''
s = s.replace(anchor, insert, 1)

# Replace the Run21 refresh path. Do not cold-start, reset, send 0x04, send
# power-off, or drop PWR on each UI update. The pre-WiFi init above owns the
# cold start. If power was unexpectedly lost, recover with a normal init once.
new_refresh = r'''bool EpaperDisplayT42::RefreshPanelFull() {
    ESP_LOGI(TAG, "Full refresh begin (WARM HW-SPI 2MHz; no repeated POWER ON)");

    if (!power_rail_on_) {
        ESP_LOGW(TAG, "Warm panel state lost; performing recovery cold init");
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
    const bool busy_ok = WaitBusyRelease("warm-display-refresh", 12000);
    if (!busy_ok) {
        ESP_LOGW(TAG, "Warm display BUSY unavailable; fixed 3 s settling");
        vTaskDelay(pdMS_TO_TICKS(3000));
    }

    ESP_LOGI(TAG, "Full refresh done (%s); panel remains powered",
             busy_ok ? "BUSY confirmed" : "timed fallback");
    return true;
}'''
pattern = (r'bool EpaperDisplayT42::RefreshPanelFull\(\) \{.*?\n\}'
           r'(?=\n\nvoid EpaperDisplayT42::SleepAndPowerOff)')
s, n = re.subn(pattern, new_refresh, s, count=1, flags=re.S)
assert n == 1, 'RefreshPanelFull replacement failed'

sp.write_text(s)
print('Run23 applied: pre-WiFi cold init/clear + warm persistent panel + no repeated 0x04')

from pathlib import Path
import re

p = Path('main/boards/bread-compact-esp32/esp32_bread_board.cc')
s = p.read_text()

# Replace the complete e-paper self-test class with a safe, no-refresh wiring tester.
start = s.index('class EpaperHardwareSelfTest {')
end = s.index('\n};\n\n} // namespace', start) + len('\n};')

new_class = r'''class EpaperHardwareSelfTest {
public:
    bool Run() {
        ESP_LOGW(EPD_DIAG_TAG,
                 "WIRE TEST MODE: no e-paper refresh will be issued. Measure HAT header pins to GND.");
        ESP_LOGI(EPD_DIAG_TAG,
                 "Expected wiring: PWR=%d BUSY=%d RST=%d DC=%d CS=%d CLK=%d DIN=%d",
                 EPD_PWR_PIN, EPD_BUSY_PIN, EPD_RST_PIN, EPD_DC_PIN,
                 EPD_CS_PIN, EPD_SCLK_PIN, EPD_MOSI_PIN);

        gpio_config_t out = {};
        out.pin_bit_mask =
            (1ULL << EPD_PWR_PIN) |
            (1ULL << EPD_RST_PIN) |
            (1ULL << EPD_DC_PIN) |
            (1ULL << EPD_CS_PIN) |
            (1ULL << EPD_SCLK_PIN) |
            (1ULL << EPD_MOSI_PIN);
        out.mode = GPIO_MODE_OUTPUT;
        out.pull_up_en = GPIO_PULLUP_DISABLE;
        out.pull_down_en = GPIO_PULLDOWN_DISABLE;
        out.intr_type = GPIO_INTR_DISABLE;
        if (gpio_config(&out) != ESP_OK) {
            ESP_LOGE(EPD_DIAG_TAG, "GPIO output config failed");
            return false;
        }

        gpio_config_t busy = {};
        busy.pin_bit_mask = (1ULL << EPD_BUSY_PIN);
        busy.mode = GPIO_MODE_INPUT;
        busy.pull_up_en = GPIO_PULLUP_DISABLE;
        busy.pull_down_en = GPIO_PULLDOWN_DISABLE;
        busy.intr_type = GPIO_INTR_DISABLE;
        if (gpio_config(&busy) != ESP_OK) {
            ESP_LOGE(EPD_DIAG_TAG, "BUSY input config failed");
            return false;
        }

        // Safe baseline: HAT power off, reset high, CS high, clock/data low.
        gpio_set_level(EPD_PWR_PIN, 0);
        gpio_set_level(EPD_RST_PIN, 1);
        gpio_set_level(EPD_DC_PIN, 0);
        gpio_set_level(EPD_CS_PIN, 1);
        gpio_set_level(EPD_SCLK_PIN, 0);
        gpio_set_level(EPD_MOSI_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(1000));

        while (true) {
            TestPin("PWR", EPD_PWR_PIN, true);
            TestPin("RST", EPD_RST_PIN, false);
            TestPin("DC", EPD_DC_PIN, false);
            TestPin("CS", EPD_CS_PIN, false);
            TestPin("CLK", EPD_SCLK_PIN, false);
            TestPin("DIN", EPD_MOSI_PIN, false);
            ESP_LOGI(EPD_DIAG_TAG,
                     "WIRE TEST cycle complete. BUSY digital=%d. Repeating in 3 seconds...",
                     gpio_get_level(EPD_BUSY_PIN));
            vTaskDelay(pdMS_TO_TICKS(3000));
        }
        return true;
    }

private:
    void SafeBaseline() {
        gpio_set_level(EPD_PWR_PIN, 0);
        gpio_set_level(EPD_RST_PIN, 1);
        gpio_set_level(EPD_DC_PIN, 0);
        gpio_set_level(EPD_CS_PIN, 1);
        gpio_set_level(EPD_SCLK_PIN, 0);
        gpio_set_level(EPD_MOSI_PIN, 0);
    }

    void LogSecond(const char* name, int level, int second) {
        ESP_LOGI(EPD_DIAG_TAG,
                 "WIRETEST %-3s GPIO=%s | second=%d/5 | BUSY=%d",
                 name, level ? "HIGH (~3.3V)" : "LOW (~0V)", second,
                 gpio_get_level(EPD_BUSY_PIN));
    }

    void Hold(const char* name, int level) {
        for (int i = 1; i <= 5; ++i) {
            LogSecond(name, level, i);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }

    void TestPin(const char* name, gpio_num_t pin, bool is_power) {
        SafeBaseline();
        if (!is_power && pin == EPD_RST_PIN) {
            // RST baseline is HIGH, so test LOW first.
            ESP_LOGW(EPD_DIAG_TAG,
                     "TEST %-3s: measure HAT %-3s now. First ~0V for 5s, then ~3.3V for 5s",
                     name, name);
            gpio_set_level(pin, 0);
            Hold(name, 0);
            gpio_set_level(pin, 1);
            Hold(name, 1);
        } else if (!is_power && pin == EPD_CS_PIN) {
            // CS baseline is HIGH; test LOW then HIGH while HAT power is OFF.
            ESP_LOGW(EPD_DIAG_TAG,
                     "TEST %-3s: measure HAT %-3s now. First ~0V for 5s, then ~3.3V for 5s",
                     name, name);
            gpio_set_level(pin, 0);
            Hold(name, 0);
            gpio_set_level(pin, 1);
            Hold(name, 1);
        } else {
            ESP_LOGW(EPD_DIAG_TAG,
                     "TEST %-3s: measure HAT %-3s now. First ~0V for 5s, then ~3.3V for 5s",
                     name, name);
            gpio_set_level(pin, 0);
            Hold(name, 0);
            if (is_power) {
                // Keep RST and CS safely HIGH while powering the HAT; no SPI clocks are generated.
                gpio_set_level(EPD_RST_PIN, 1);
                gpio_set_level(EPD_CS_PIN, 1);
            }
            gpio_set_level(pin, 1);
            Hold(name, 1);
        }
        SafeBaseline();
        vTaskDelay(pdMS_TO_TICKS(500));
    }
};'''

s = s[:start] + new_class + s[end:]
p.write_text(s)
print('T42 safe slow wiring test patch applied')

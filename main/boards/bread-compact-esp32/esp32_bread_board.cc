#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "epaper_display_t42.h"

#include <algorithm>
#include <cstdint>
#include <cstring>

#include <driver/gpio.h>
#include <driver/spi_master.h>
#include <esp_log.h>
#include <esp_rom_sys.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

#define TAG "BreadESP32-EpaperT42"
#define EPD_DIAG_TAG "EpaperDiag"
#define MIC_DIAG_TAG "MicDiag"

namespace {

constexpr spi_host_device_t kDiagSpiHost = SPI3_HOST;
constexpr int kDiagSpiHz = 2 * 1000 * 1000;
constexpr size_t kEpdLineBytes = EPD_WIDTH / 8;

class DiagnosticNoAudioCodecSimplex : public NoAudioCodecSimplex {
public:
    DiagnosticNoAudioCodecSimplex(
        int input_sample_rate,
        int output_sample_rate,
        gpio_num_t spk_bclk,
        gpio_num_t spk_ws,
        gpio_num_t spk_dout,
        gpio_num_t mic_sck,
        gpio_num_t mic_ws,
        gpio_num_t mic_din)
        : NoAudioCodecSimplex(
              input_sample_rate,
              output_sample_rate,
              spk_bclk,
              spk_ws,
              spk_dout,
              mic_sck,
              mic_ws,
              mic_din) {}

protected:
    int Read(int16_t* dest, int samples) override {
        const int n = NoAudioCodec::Read(dest, samples);
        if (n <= 0) {
            return n;
        }

        for (int i = 0; i < n; ++i) {
            int32_t v = dest[i];
            if (v < 0) {
                v = -v;
            }
            if (v > peak_) {
                peak_ = v;
            }
            abs_sum_ += v;
        }
        sample_count_ += n;

        if (sample_count_ >= input_sample_rate_) {
            const int32_t avg_abs =
                sample_count_ > 0 ? static_cast<int32_t>(abs_sum_ / sample_count_) : 0;
            ESP_LOGI(MIC_DIAG_TAG,
                     "MIC_LEVEL peak=%ld avg_abs=%ld samples=%ld (speak near INMP441 now)",
                     static_cast<long>(peak_),
                     static_cast<long>(avg_abs),
                     static_cast<long>(sample_count_));
            peak_ = 0;
            abs_sum_ = 0;
            sample_count_ = 0;
        }

        return n;
    }

private:
    int32_t peak_ = 0;
    int64_t abs_sum_ = 0;
    int32_t sample_count_ = 0;
};

class EpaperHardwareSelfTest {
public:
    bool Run() {
        ESP_LOGI(EPD_DIAG_TAG,
                 "BOOT SELF-TEST start: 800x480 split pattern, PWR=%d BUSY=%d RST=%d DC=%d CS=%d CLK=%d DIN=%d",
                 EPD_PWR_PIN, EPD_BUSY_PIN, EPD_RST_PIN, EPD_DC_PIN,
                 EPD_CS_PIN, EPD_SCLK_PIN, EPD_MOSI_PIN);

        if (!InitGpioAndSpi()) {
            ESP_LOGE(EPD_DIAG_TAG, "BOOT SELF-TEST: GPIO/SPI init failed");
            Cleanup();
            return false;
        }

        gpio_set_level(EPD_PWR_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(50));

        ESP_LOGI(EPD_DIAG_TAG, "PWR=HIGH, BUSY=%d", gpio_get_level(EPD_BUSY_PIN));
        HardwareReset();

        // Waveshare official 7.5" V2 full-refresh initialization sequence.
        Command(0x01);  // POWER SETTING
        Data(0x07);
        Data(0x07);
        Data(0x3F);
        Data(0x3F);

        Command(0x06);  // BOOSTER SOFT START
        Data(0x17);
        Data(0x17);
        Data(0x28);
        Data(0x17);

        Command(0x04);  // POWER ON
        vTaskDelay(pdMS_TO_TICKS(100));
        const bool power_busy_ok = WaitBusyCycle("power-on", 800, 5000);
        if (!power_busy_ok) {
            ESP_LOGW(EPD_DIAG_TAG,
                     "POWER ON BUSY cycle not observed; continuing self-test for diagnosis");
        }

        Command(0x00);  // PANEL SETTING
        Data(0x1F);

        Command(0x61);  // 800 x 480
        Data(0x03);
        Data(0x20);
        Data(0x01);
        Data(0xE0);

        Command(0x15);
        Data(0x00);

        Command(0x50);
        Data(0x10);
        Data(0x07);

        Command(0x60);
        Data(0x22);

        // Persistent test image: one half black / one half white. The second
        // plane is the inverse, matching Waveshare's official convention.
        uint8_t line[kEpdLineBytes];
        for (size_t i = 0; i < kEpdLineBytes; ++i) {
            line[i] = (i < kEpdLineBytes / 2) ? 0x00 : 0xFF;
        }

        ESP_LOGI(EPD_DIAG_TAG, "Writing test image plane 0x10...");
        Command(0x10);
        for (int y = 0; y < EPD_HEIGHT; ++y) {
            DataBuffer(line, sizeof(line));
        }

        for (size_t i = 0; i < kEpdLineBytes; ++i) {
            line[i] = static_cast<uint8_t>(~line[i]);
        }

        ESP_LOGI(EPD_DIAG_TAG, "Writing inverted image plane 0x13...");
        Command(0x13);
        for (int y = 0; y < EPD_HEIGHT; ++y) {
            DataBuffer(line, sizeof(line));
        }

        ESP_LOGI(EPD_DIAG_TAG,
                 "DISPLAY REFRESH command; watch panel for flashing / split black-white image");
        Command(0x12);
        vTaskDelay(pdMS_TO_TICKS(100));
        const bool refresh_busy_ok = WaitBusyCycle("display-refresh", 1000, 12000);

        if (refresh_busy_ok) {
            ESP_LOGI(EPD_DIAG_TAG,
                     "BOOT SELF-TEST PASS at protocol level; holding image for 3 seconds");
        } else {
            ESP_LOGE(EPD_DIAG_TAG,
                     "BOOT SELF-TEST FAIL: display BUSY cycle was not observed");
        }

        vTaskDelay(pdMS_TO_TICKS(3000));

        // Power off driver HAT after the one-shot diagnostic. E-paper retains image.
        Command(0x02);
        vTaskDelay(pdMS_TO_TICKS(100));
        WaitBusyReleaseOnly("power-off", 2000);
        gpio_set_level(EPD_PWR_PIN, 0);
        ESP_LOGI(EPD_DIAG_TAG, "PWR=LOW; self-test finished, image should remain");

        Cleanup();
        return refresh_busy_ok;
    }

private:
    spi_device_handle_t spi_ = nullptr;
    bool bus_initialized_ = false;

    bool InitGpioAndSpi() {
        gpio_config_t out = {};
        out.pin_bit_mask =
            (1ULL << EPD_PWR_PIN) |
            (1ULL << EPD_RST_PIN) |
            (1ULL << EPD_DC_PIN);
        out.mode = GPIO_MODE_OUTPUT;
        out.pull_up_en = GPIO_PULLUP_DISABLE;
        out.pull_down_en = GPIO_PULLDOWN_DISABLE;
        out.intr_type = GPIO_INTR_DISABLE;
        if (gpio_config(&out) != ESP_OK) {
            return false;
        }

        gpio_config_t busy = {};
        busy.pin_bit_mask = (1ULL << EPD_BUSY_PIN);
        busy.mode = GPIO_MODE_INPUT;
        busy.pull_up_en = GPIO_PULLUP_DISABLE;
        busy.pull_down_en = GPIO_PULLDOWN_DISABLE;
        busy.intr_type = GPIO_INTR_DISABLE;
        if (gpio_config(&busy) != ESP_OK) {
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
        bus_cfg.max_transfer_sz = 256;

        esp_err_t err = spi_bus_initialize(kDiagSpiHost, &bus_cfg, SPI_DMA_CH_AUTO);
        if (err != ESP_OK) {
            ESP_LOGE(EPD_DIAG_TAG, "spi_bus_initialize: %s", esp_err_to_name(err));
            return false;
        }
        bus_initialized_ = true;

        spi_device_interface_config_t dev_cfg = {};
        dev_cfg.clock_speed_hz = kDiagSpiHz;
        dev_cfg.mode = 0;
        dev_cfg.spics_io_num = EPD_CS_PIN;
        dev_cfg.queue_size = 1;

        err = spi_bus_add_device(kDiagSpiHost, &dev_cfg, &spi_);
        if (err != ESP_OK) {
            ESP_LOGE(EPD_DIAG_TAG, "spi_bus_add_device: %s", esp_err_to_name(err));
            return false;
        }
        return true;
    }

    void Cleanup() {
        if (spi_ != nullptr) {
            spi_bus_remove_device(spi_);
            spi_ = nullptr;
        }
        if (bus_initialized_) {
            spi_bus_free(kDiagSpiHost);
            bus_initialized_ = false;
        }
    }

    esp_err_t Write(const uint8_t* data, size_t len) {
        spi_transaction_t t = {};
        t.length = len * 8;
        t.tx_buffer = data;
        return spi_device_polling_transmit(spi_, &t);
    }

    void Command(uint8_t value) {
        gpio_set_level(EPD_DC_PIN, 0);
        ESP_ERROR_CHECK_WITHOUT_ABORT(Write(&value, 1));
    }

    void Data(uint8_t value) {
        gpio_set_level(EPD_DC_PIN, 1);
        ESP_ERROR_CHECK_WITHOUT_ABORT(Write(&value, 1));
    }

    void DataBuffer(const uint8_t* data, size_t len) {
        gpio_set_level(EPD_DC_PIN, 1);
        ESP_ERROR_CHECK_WITHOUT_ABORT(Write(data, len));
    }

    void HardwareReset() {
        ESP_LOGI(EPD_DIAG_TAG, "Reset: HIGH 20ms -> LOW 2ms -> HIGH 20ms");
        gpio_set_level(EPD_RST_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(20));
        gpio_set_level(EPD_RST_PIN, 0);
        esp_rom_delay_us(2000);
        gpio_set_level(EPD_RST_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(20));
        ESP_LOGI(EPD_DIAG_TAG, "After reset BUSY=%d", gpio_get_level(EPD_BUSY_PIN));
    }

    bool WaitBusyCycle(
        const char* reason,
        uint32_t assert_timeout_ms,
        uint32_t release_timeout_ms) {
        TickType_t start = xTaskGetTickCount();
        ESP_LOGI(EPD_DIAG_TAG, "%s: waiting BUSY LOW, current=%d",
                 reason, gpio_get_level(EPD_BUSY_PIN));

        while (gpio_get_level(EPD_BUSY_PIN) != 0) {
            if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(assert_timeout_ms)) {
                ESP_LOGE(EPD_DIAG_TAG,
                         "%s: BUSY never asserted LOW within %u ms (level=%d)",
                         reason,
                         static_cast<unsigned>(assert_timeout_ms),
                         gpio_get_level(EPD_BUSY_PIN));
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }

        const uint32_t assert_elapsed =
            static_cast<uint32_t>((xTaskGetTickCount() - start) * portTICK_PERIOD_MS);
        ESP_LOGI(EPD_DIAG_TAG, "%s: BUSY LOW after %u ms; waiting release HIGH",
                 reason, static_cast<unsigned>(assert_elapsed));

        start = xTaskGetTickCount();
        while (gpio_get_level(EPD_BUSY_PIN) == 0) {
            if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(release_timeout_ms)) {
                ESP_LOGE(EPD_DIAG_TAG,
                         "%s: BUSY stayed LOW > %u ms",
                         reason, static_cast<unsigned>(release_timeout_ms));
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }

        const uint32_t busy_elapsed =
            static_cast<uint32_t>((xTaskGetTickCount() - start) * portTICK_PERIOD_MS);
        ESP_LOGI(EPD_DIAG_TAG, "%s: BUSY released HIGH after %u ms",
                 reason, static_cast<unsigned>(busy_elapsed));
        return true;
    }

    bool WaitBusyReleaseOnly(const char* reason, uint32_t timeout_ms) {
        const TickType_t start = xTaskGetTickCount();
        while (gpio_get_level(EPD_BUSY_PIN) == 0) {
            if ((xTaskGetTickCount() - start) > pdMS_TO_TICKS(timeout_ms)) {
                ESP_LOGW(EPD_DIAG_TAG, "%s: release timeout, BUSY=%d",
                         reason, gpio_get_level(EPD_BUSY_PIN));
                return false;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
        return true;
    }
};

} // namespace

class CompactWifiBoard : public WifiBoard {
private:
    Button boot_button_;
    Button touch_button_;
    Button asr_button_;
    Display* display_ = nullptr;

    void InitializeButtons() {
        gpio_config_t io_conf = {};
        io_conf.pin_bit_mask = 1ULL << BUILTIN_LED_GPIO;
        io_conf.mode = GPIO_MODE_OUTPUT;
        io_conf.pull_up_en = GPIO_PULLUP_DISABLE;
        io_conf.pull_down_en = GPIO_PULLDOWN_DISABLE;
        io_conf.intr_type = GPIO_INTR_DISABLE;
        ESP_ERROR_CHECK(gpio_config(&io_conf));

        boot_button_.OnClick([this]() {
            auto& app = Application::GetInstance();
            if (app.GetDeviceState() == kDeviceStateStarting) {
                EnterWifiConfigMode();
                return;
            }
            gpio_set_level(BUILTIN_LED_GPIO, 1);
            app.ToggleChatState();
        });

        asr_button_.OnClick([this]() {
            std::string wake_word = "你好小智";
            Application::GetInstance().WakeWordInvoke(wake_word);
        });

        touch_button_.OnPressDown([this]() {
            gpio_set_level(BUILTIN_LED_GPIO, 1);
            Application::GetInstance().StartListening();
        });

        touch_button_.OnPressUp([this]() {
            gpio_set_level(BUILTIN_LED_GPIO, 0);
            Application::GetInstance().StopListening();
        });
    }

    void InitializeDisplayDiagnostic() {
        EpaperHardwareSelfTest test;
        const bool epd_ok = test.Run();
        ESP_LOGI(TAG, "E-paper hardware self-test result: %s",
                 epd_ok ? "BUSY CYCLE OK" : "BUSY CYCLE FAILED");
        display_ = new NoDisplay();
    }

public:
    CompactWifiBoard()
        : WifiBoard(),
          boot_button_(BOOT_BUTTON_GPIO),
          touch_button_(TOUCH_BUTTON_GPIO),
          asr_button_(ASR_BUTTON_GPIO) {
        InitializeButtons();
        InitializeDisplayDiagnostic();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static DiagnosticNoAudioCodecSimplex audio_codec(
            AUDIO_INPUT_SAMPLE_RATE,
            AUDIO_OUTPUT_SAMPLE_RATE,
            AUDIO_I2S_SPK_GPIO_BCLK,
            AUDIO_I2S_SPK_GPIO_LRCK,
            AUDIO_I2S_SPK_GPIO_DOUT,
            AUDIO_I2S_MIC_GPIO_SCK,
            AUDIO_I2S_MIC_GPIO_WS,
            AUDIO_I2S_MIC_GPIO_DIN);
        return &audio_codec;
    }

    virtual Display* GetDisplay() override {
        return display_;
    }
};

DECLARE_BOARD(CompactWifiBoard);

#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "epaper_display_t42.h"

#include <esp_log.h>
#include <driver/gpio.h>

#define TAG "BreadESP32-EpaperT42"

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

    void InitializeDisplay() {
        auto* epaper = new EpaperDisplayT42();
        if (epaper->IsReady()) {
            display_ = epaper;
            ESP_LOGI(TAG, "T42 e-paper display initialized");
        } else {
            ESP_LOGE(TAG, "T42 e-paper display init failed, falling back to NoDisplay");
            delete epaper;
            display_ = new NoDisplay();
        }
    }

public:
    CompactWifiBoard()
        : WifiBoard(),
          boot_button_(BOOT_BUTTON_GPIO),
          touch_button_(TOUCH_BUTTON_GPIO),
          asr_button_(ASR_BUTTON_GPIO) {
        // SSD1306 and LampController are intentionally not initialized.
        // GPIO18 is reserved for the e-paper SPI clock.
        InitializeButtons();
        InitializeDisplay();
    }

    virtual AudioCodec* GetAudioCodec() override {
        static NoAudioCodecSimplex audio_codec(
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

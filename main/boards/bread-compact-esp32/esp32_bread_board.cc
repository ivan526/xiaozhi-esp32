#include "wifi_board.h"
#include "codecs/no_audio_codec.h"
#include "application.h"
#include "button.h"
#include "config.h"
#include "epaper_display_t42.h"
#include "lvgl_theme.h"

#include <esp_log.h>
#include <driver/gpio.h>

#define TAG "BreadESP32-EpaperT42"

// EpaperDisplayT42 intentionally builds a much smaller LVGL object tree than
// LcdDisplay::SetupUI(). Assets::Apply() can replace the text font at runtime;
// LvglDisplay::SetTextFont() then dispatches SetTheme() virtually. Calling the
// stock LcdDisplay::SetTheme() here would touch LCD-only objects (mute_label_,
// battery_label_, content_, etc.) that do not exist on the compact e-paper UI.
// Keep theme/font rebinding local to the objects that actually exist.
class SafeEpaperDisplayT42 : public EpaperDisplayT42 {
public:
    void SetTheme(Theme* theme) override {
        if (theme == nullptr) {
            return;
        }

        auto* lvgl_theme = static_cast<LvglTheme*>(theme);
        auto font_owner = lvgl_theme->text_font();
        const lv_font_t* text_font =
            (font_owner != nullptr) ? font_owner->font() : nullptr;

        {
            DisplayLockGuard lock(this);
            lv_obj_t* screen =
                (display_ != nullptr) ? lv_display_get_screen_active(display_) : nullptr;

            if (screen != nullptr && text_font != nullptr) {
                // The compact T42 UI keeps all of its labels/dividers as direct
                // children of the active screen. Rebind the screen and each child
                // to the newly downloaded font, without touching LCD-only widgets.
                lv_obj_set_style_text_font(screen, text_font, 0);
                const uint32_t child_count = lv_obj_get_child_cnt(screen);
                for (uint32_t i = 0; i < child_count; ++i) {
                    lv_obj_t* child = lv_obj_get_child(screen, i);
                    if (child != nullptr) {
                        lv_obj_set_style_text_font(child, text_font, 0);
                    }
                }
            }
        }

        // Preserve the normal theme bookkeeping/settings behavior without
        // entering LcdDisplay::SetTheme().
        Display::SetTheme(theme);
    }
};

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
        auto* epaper = new SafeEpaperDisplayT42();
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

#include "epaper_display_t42.h"

#include "assets/lang_config.h"
#include "config.h"
#include "epaper_dashboard_icons.h"
#include "lunar_calendar.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>

#include <driver/gpio.h>
#include <esp_err.h>
#include <esp_heap_caps.h>
#include <esp_log.h>
#include <esp_lvgl_port.h>
#include <esp_timer.h>

#define TAG "EpaperDeskDash"

LV_FONT_DECLARE(BUILTIN_TEXT_FONT);

namespace {
constexpr spi_host_device_t kSpiHost = SPI3_HOST;
constexpr size_t kSpiChunk = 4096;
constexpr size_t kMaxUserBytes = 160;
constexpr size_t kMaxAssistantBytes = 300;
constexpr uint32_t kPartialDebounceMs = 220;
constexpr uint32_t kRefreshTriggerDelayMs = 100;
// Fast partial refresh is differential. Do a clean full waveform before many
// small updates can accumulate visible contrast loss or ghosting.
constexpr uint32_t kPartialRefreshLimit = 20;
// POSIX TZ signs are reversed: CST-8 means UTC+8.
constexpr char kChinaTimezone[] = "CST-8";

struct Region { int x0; int y0; int x1; int y1; const char* name; };
// This physical GDEY075T7 setup is most stable with partial RAM windows starting
// at X=0. Keep regions narrow in height, and group updates by top/middle/bottom
// bands so each waveform is useful without allocating a giant 48 KB buffer.
constexpr Region kClockDigitsRegion {0, 0,   296, 92,  "clock-digits"};
constexpr Region kClockCardRegion   {0, 0,   338, 138, "clock-card"};
constexpr Region kHeaderRegion      {0, 0,   800, 138, "header"};
constexpr Region kWeatherRegion     {0, 132, 350, 304, "weather"};
constexpr Region kTodoRegion        {0, 132, 540, 304, "todo"};
constexpr Region kQuickRegion       {0, 132, 800, 304, "quick"};
constexpr Region kWordRegion        {0, 298, 296, 480, "word"};
constexpr Region kChatRegion        {0, 298, 800, 480, "chat"};

constexpr uint8_t kDigitSegments[10] = {
    0b0111111, 0b0000110, 0b1011011, 0b1001111, 0b1100110,
    0b1101101, 0b1111101, 0b0000111, 0b1111111, 0b1101111,
};

bool ValidSystemTime(std::time_t now, struct tm* out) {
    if (now <= 0 || out == nullptr || localtime_r(&now, out) == nullptr) return false;
    return out->tm_year + 1900 >= 2024;
}

// The dashboard is monochrome. This division-free RGB565 luminance estimate is
// much cheaper than expanding each channel to 0..255 for every rendered pixel.
inline bool PixelIsBlack(uint16_t p) {
    const uint32_t r6 = ((p >> 11) & 0x1F) << 1;
    const uint32_t g6 = (p >> 5) & 0x3F;
    const uint32_t b6 = (p & 0x1F) << 1;
    const uint32_t luminance = r6 * 19 + g6 * 38 + b6 * 7; // weights sum to 64
    return luminance < (38u * 64u); // keep 16px CJK strokes complete without over-thickening
}

void StyleSolidBlack(lv_obj_t* obj) {
    lv_obj_set_style_bg_color(obj, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(obj, 0, 0);
    lv_obj_set_style_pad_all(obj, 0, 0);
    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);
}

lv_obj_t* CreateBitmapIcon(lv_obj_t* parent, int x, int y, const lv_image_dsc_t* src) {
    lv_obj_t* img = lv_image_create(parent);
    lv_image_set_src(img, src);
    lv_obj_set_pos(img, x, y);
    lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE);
    return img;
}

const lv_image_dsc_t* WeatherBitmap(int code) {
    if (code == 0) return &ep_icon_weather_sun_40;
    if (code == 1 || code == 2) return &ep_icon_weather_partly_40;
    if (code == 3) return &ep_icon_weather_cloud_40;
    if (code == 45 || code == 48) return &ep_icon_weather_fog_40;
    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return &ep_icon_weather_rain_40;
    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return &ep_icon_weather_snow_40;
    if (code >= 95) return &ep_icon_weather_thunder_40;
    return &ep_icon_weather_cloud_40;
}

} // namespace

EpaperDisplayT42::EpaperDisplayT42()
    : LcdDisplay(nullptr, nullptr, EPD_WIDTH, EPD_HEIGHT) {
    dashboard_ = {};
    data_provider_.FillStaticDefaults(dashboard_);

    // The board variant is intended for China desktop use. NTP/server time is
    // still stored as UTC; localtime_r() is always converted to UTC+8 here.
    setenv("TZ", kChinaTimezone, 1);
    tzset();

    ESP_LOGI(TAG, "Timezone: China Standard Time (UTC+8 / Asia/Shanghai)");
    ESP_LOGI(TAG, "Heap before e-paper: free=%u largest=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)));

    if (!InitializeHardware()) {
        ESP_LOGE(TAG, "Hardware initialization failed");
        return;
    }

    // Audio/Opus is more time-sensitive than e-paper. Keep the display worker at
    // low priority so a panel update cannot steal scheduling time from playback.
    if (xTaskCreate(RefreshTaskEntry, "epaper_refresh", 4096, this, 1,
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
             "Ready: GDEY075T7 800x480 dashboard, preserved-RAM partial refresh, "
             "PWR=3V3 BUSY=%d RST=%d DC=%d CS=%d CLK=%d DIN=%d",
             EPD_BUSY_PIN, EPD_RST_PIN, EPD_DC_PIN,
             EPD_CS_PIN, EPD_SCLK_PIN, EPD_MOSI_PIN);
}

EpaperDisplayT42::~EpaperDisplayT42() {
    if (data_task_handle_ != nullptr) {
        vTaskDelete(data_task_handle_);
        data_task_handle_ = nullptr;
    }
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
    out_cfg.pin_bit_mask = (1ULL << EPD_RST_PIN) | (1ULL << EPD_DC_PIN);
    out_cfg.mode = GPIO_MODE_OUTPUT;
    out_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    out_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    out_cfg.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&out_cfg) != ESP_OK) return false;

    gpio_config_t busy_cfg = {};
    busy_cfg.pin_bit_mask = (1ULL << EPD_BUSY_PIN);
    busy_cfg.mode = GPIO_MODE_INPUT;
    busy_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
    busy_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
    busy_cfg.intr_type = GPIO_INTR_DISABLE;
    if (gpio_config(&busy_cfg) != ESP_OK) return false;

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
    lv_init();

    lvgl_port_cfg_t port_cfg = ESP_LVGL_PORT_INIT_CONFIG();
    port_cfg.task_priority = 1;
    port_cfg.timer_period_ms = 50;
    esp_err_t err = lvgl_port_init(&port_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "lvgl_port_init failed: %s", esp_err_to_name(err));
        return false;
    }

    const size_t buffer_size =
        EPD_WIDTH * LVGL_BUFFER_ROWS * LV_COLOR_FORMAT_GET_SIZE(LV_COLOR_FORMAT_RGB565);
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
        return false;
    }
    lv_display_set_color_format(display_, LV_COLOR_FORMAT_RGB565);
    lv_display_set_flush_cb(display_, LvglFlushCb);
    lv_display_set_user_data(display_, this);
    lv_display_set_buffers(display_, lvgl_buffer_, nullptr, buffer_size,
                           LV_DISPLAY_RENDER_MODE_PARTIAL);
    lvgl_port_unlock();
    return true;
}

lv_obj_t* EpaperDisplayT42::CreateBox(lv_obj_t* screen, int x, int y, int w, int h) {
    lv_obj_t* box = lv_obj_create(screen);
    lv_obj_set_pos(box, x, y);
    lv_obj_set_size(box, w, h);
    lv_obj_set_style_radius(box, 0, 0); // pixel-sharp grid on monochrome e-paper
    lv_obj_set_style_border_width(box, 1, 0);
    lv_obj_set_style_border_color(box, lv_color_black(), 0);
    lv_obj_set_style_bg_color(box, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(box, 0, 0);
    lv_obj_clear_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    return box;
}

lv_obj_t* EpaperDisplayT42::CreateLabel(
    lv_obj_t* screen, int x, int y, int w, const char* text, lv_text_align_t align) {
    lv_obj_t* label = lv_label_create(screen);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, w);
    lv_label_set_text(label, text != nullptr ? text : "");
    lv_label_set_long_mode(label, LV_LABEL_LONG_WRAP);
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_style_text_font(label, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_style_text_line_space(label, 1, 0);
    lv_obj_set_style_text_opa(label, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(label, 0, 0);
    return label;
}

lv_obj_t* EpaperDisplayT42::CreateSymbolLabel(
    lv_obj_t* screen, int x, int y, int w, const char* symbol, lv_text_align_t align) {
    lv_obj_t* label = lv_label_create(screen);
    lv_obj_set_pos(label, x, y);
    lv_obj_set_width(label, w);
    lv_label_set_text(label, symbol != nullptr ? symbol : "");
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_style_text_font(label, LV_FONT_DEFAULT, 0);
    lv_obj_set_style_text_align(label, align, 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_all(label, 0, 0);
    return label;
}

lv_obj_t* EpaperDisplayT42::CreateWeatherBadge(
    lv_obj_t* screen, int x, int y, int size, const char* glyph) {
    lv_obj_t* badge = lv_obj_create(screen);
    lv_obj_set_pos(badge, x, y);
    lv_obj_set_size(badge, size, size);
    lv_obj_set_style_radius(badge, size / 2, 0);
    lv_obj_set_style_border_width(badge, 2, 0);
    lv_obj_set_style_border_color(badge, lv_color_black(), 0);
    lv_obj_set_style_bg_color(badge, lv_color_white(), 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(badge, 0, 0);
    lv_obj_clear_flag(badge, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* label = lv_label_create(badge);
    lv_label_set_text(label, glyph != nullptr ? glyph : "云");
    lv_obj_set_style_text_color(label, lv_color_black(), 0);
    lv_obj_set_style_text_font(label, &BUILTIN_TEXT_FONT, 0);
    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);
    lv_obj_center(label);
    return label;
}

void EpaperDisplayT42::CreateSevenSegmentClock(lv_obj_t* screen) {
    constexpr int x0 = 58;
    constexpr int y0 = 14;
    constexpr int digit_w = 48;
    constexpr int digit_h = 70;
    constexpr int t = 6;
    constexpr int gap = 7;
    const int digit_x[4] = {x0, x0 + digit_w + gap, x0 + 2 * digit_w + 3 * gap,
                            x0 + 3 * digit_w + 4 * gap};

    for (int d = 0; d < 4; ++d) {
        const int x = digit_x[d];
        auto make_seg = [&](int sx, int sy, int sw, int sh) -> lv_obj_t* {
            lv_obj_t* seg = lv_obj_create(screen);
            lv_obj_set_pos(seg, sx, sy);
            lv_obj_set_size(seg, sw, sh);
            lv_obj_set_style_radius(seg, std::min(sw, sh) / 2, 0);
            StyleSolidBlack(seg);
            return seg;
        };

        clock_segments_[d][0] = make_seg(x + t, y0, digit_w - 2 * t, t);
        clock_segments_[d][1] = make_seg(x + digit_w - t, y0 + t, t, digit_h / 2 - t);
        clock_segments_[d][2] = make_seg(x + digit_w - t, y0 + digit_h / 2, t, digit_h / 2 - t);
        clock_segments_[d][3] = make_seg(x + t, y0 + digit_h - t, digit_w - 2 * t, t);
        clock_segments_[d][4] = make_seg(x, y0 + digit_h / 2, t, digit_h / 2 - t);
        clock_segments_[d][5] = make_seg(x, y0 + t, t, digit_h / 2 - t);
        clock_segments_[d][6] = make_seg(x + t, y0 + digit_h / 2 - t / 2, digit_w - 2 * t, t);
    }

    const int colon_x = x0 + 2 * digit_w + 2 * gap;
    for (int i = 0; i < 2; ++i) {
        clock_colon_[i] = lv_obj_create(screen);
        lv_obj_set_pos(clock_colon_[i], colon_x, y0 + 21 + i * 27);
        lv_obj_set_size(clock_colon_[i], 7, 7);
        lv_obj_set_style_radius(clock_colon_[i], 4, 0);
        StyleSolidBlack(clock_colon_[i]);
    }
}

void EpaperDisplayT42::SetClockDigit(int index, int digit) {
    if (index < 0 || index >= 4 || digit < 0 || digit > 9) return;
    if (clock_digits_[index] == digit) return;
    clock_digits_[index] = digit;
    const uint8_t mask = kDigitSegments[digit];
    for (int s = 0; s < 7; ++s) {
        lv_obj_t* seg = clock_segments_[index][s];
        if (seg == nullptr) continue;
        if ((mask & (1u << s)) != 0) lv_obj_clear_flag(seg, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(seg, LV_OBJ_FLAG_HIDDEN);
    }
}

void EpaperDisplayT42::BuildDashboardUi(lv_obj_t* screen) {
    // A strict 6px outer margin + 6px gutters makes every card land on the
    // same visual grid. This follows Inksight's pixel-aligned layout approach.
    CreateBox(screen, 6,   6, 326, 126);
    CreateBox(screen, 338, 6, 456, 126);
    CreateBox(screen, 6,   138, 338, 160);
    CreateBox(screen, 350, 138, 184, 160);
    CreateBox(screen, 540, 138, 254, 160);
    CreateBox(screen, 6,   304, 284, 170);
    CreateBox(screen, 296, 304, 498, 170);

    auto divider = [&](int x, int y, int w, int h) {
        lv_obj_t* line = lv_obj_create(screen);
        lv_obj_set_pos(line, x, y);
        lv_obj_set_size(line, w, h);
        StyleSolidBlack(line);
    };
    auto one_line = [&](lv_obj_t* label, int height = 22) {
        if (label == nullptr) return;
        lv_obj_set_height(label, height);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    };

    CreateSevenSegmentClock(screen);
    date_label_ = CreateLabel(screen, 14, 99, 312, "日期 · 农历", LV_TEXT_ALIGN_CENTER);
    one_line(date_label_, 24);
    lunar_label_ = nullptr; // lunar date is intentionally merged into date_label_

    CreateBitmapIcon(screen, 352, 14, &ep_icon_robot_24);
    auto* title = CreateLabel(screen, 380, 18, 250, "小智桌面屏");
    one_line(title);
    CreateSymbolLabel(screen, 704, 18, 24, LV_SYMBOL_WIFI, LV_TEXT_ALIGN_CENTER);
    CreateSymbolLabel(screen, 742, 18, 24, LV_SYMBOL_USB, LV_TEXT_ALIGN_CENTER);
    status_label_ = CreateLabel(screen, 354, 49, 420, "已联网 · 正在启动");
    one_line(status_label_);
    CreateSymbolLabel(screen, 354, 82, 20, LV_SYMBOL_BELL);
    auto* hints = CreateLabel(screen, 380, 82, 390, "语音唤醒｜天气 · 日程 · 提醒 · 设备状态");
    one_line(hints);

    // Weather card: current conditions + three equal-width forecast columns.
    CreateBitmapIcon(screen, 18, 146, &ep_icon_location_20);
    weather_title_label_ = CreateLabel(screen, 43, 148, 286, "当前位置 · 天气");
    one_line(weather_title_label_);
    divider(151, 176, 1, 108);
    divider(212, 176, 1, 108);
    divider(273, 176, 1, 108);

    weather_icon_labels_[0] = CreateBitmapIcon(screen, 18, 184, &ep_icon_weather_partly_40);
    weather_current_label_ = CreateLabel(screen, 64, 181, 82, "今日 多云\n30°/24°");
    weather_aqi_label_ = CreateLabel(screen, 64, 230, 82, "AQI 52 优");
    one_line(weather_aqi_label_);

    const int wx[3] = {162, 223, 284};
    const int label_x[3] = {153, 214, 275};
    for (int i = 0; i < 3; ++i) {
        weather_icon_labels_[i + 1] = CreateBitmapIcon(screen, wx[i], 181, &ep_icon_weather_cloud_40);
    }
    weather_forecast_labels_[0] = CreateLabel(screen, label_x[0], 226, 58, "周二\n31°/24°", LV_TEXT_ALIGN_CENTER);
    weather_forecast_labels_[1] = CreateLabel(screen, label_x[1], 226, 58, "周三\n28°/23°", LV_TEXT_ALIGN_CENTER);
    weather_forecast_labels_[2] = CreateLabel(screen, label_x[2], 226, 58, "周四\n27°/22°", LV_TEXT_ALIGN_CENTER);

    CreateBitmapIcon(screen, 362, 146, &ep_icon_todo_20);
    auto* todo_title = CreateLabel(screen, 388, 149, 132, "今日待办");
    one_line(todo_title);
    todo_labels_[0] = CreateLabel(screen, 362, 184, 158, "");
    todo_labels_[1] = CreateLabel(screen, 362, 224, 158, "");
    todo_labels_[2] = CreateLabel(screen, 362, 264, 158, "");
    for (auto* label : todo_labels_) one_line(label, 22);

    const int quick_y[4] = {149, 186, 223, 260};
    CreateBitmapIcon(screen, 550, 146, &ep_icon_commute_20);
    CreateBitmapIcon(screen, 550, 183, &ep_icon_parcel_20);
    CreateBitmapIcon(screen, 550, 220, &ep_icon_home_20);
    CreateBitmapIcon(screen, 550, 257, &ep_icon_market_20);
    for (int i = 0; i < 4; ++i) {
        quick_labels_[i] = CreateLabel(screen, 576, quick_y[i], 202, "");
        one_line(quick_labels_[i], 22);
    }

    CreateBitmapIcon(screen, 18, 313, &ep_icon_word_20);
    auto* word_title = CreateLabel(screen, 42, 316, 226, "每日记单词");
    one_line(word_title);
    word_label_ = CreateLabel(screen, 18, 343, 250, "abandon");
    one_line(word_label_, 22);
    phonetic_label_ = CreateLabel(screen, 18, 366, 250, "/əˈbændən/");
    one_line(phonetic_label_, 22);
    meaning_label_ = CreateLabel(screen, 18, 390, 250, "放弃；遗弃");
    one_line(meaning_label_, 22);
    example_label_ = CreateLabel(screen, 18, 416, 255, "例：Don't abandon your plan.");
    lv_obj_set_height(example_label_, 30);
    lv_label_set_long_mode(example_label_, LV_LABEL_LONG_WRAP);
    word_footer_label_ = CreateLabel(screen, 18, 451, 255, "20词 · 30分钟轮播 · 1/20");
    one_line(word_footer_label_, 20);

    user_label_ = CreateLabel(screen, 310, 319, 468, "你：等待你说话");
    lv_obj_set_height(user_label_, 25);
    lv_label_set_long_mode(user_label_, LV_LABEL_LONG_DOT);
    CreateBitmapIcon(screen, 310, 350, &ep_icon_mic_20);
    assistant_label_ = CreateLabel(screen, 336, 352, 442, "小智：准备好了，随时可以聊。");
    lv_obj_set_height(assistant_label_, 78);
    lv_label_set_long_mode(assistant_label_, LV_LABEL_LONG_WRAP);
    CreateBitmapIcon(screen, 310, 446, &ep_icon_mic_20);
    auto* chat_hint = CreateLabel(screen, 336, 449, 442,
                                  "语音唤醒｜按键说话｜天气 · 日程 · 提醒");
    one_line(chat_hint, 20);

    UpdateClockLocked(true);
    UpdateHeaderLocked();
    UpdateWeatherLocked();
    UpdateTodoLocked();
    UpdateQuickLocked();
    UpdateWordLocked();
    UpdateChatLocked();
}

void EpaperDisplayT42::SetupUI() {
    if (setup_ui_called_) return;

    Display::SetupUI();
    {
        DisplayLockGuard lock(this);
        lv_obj_t* screen = lv_display_get_screen_active(display_);
        if (screen == nullptr) return;

        lv_obj_clean(screen);
        lv_obj_clear_flag(screen, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_style_bg_color(screen, lv_color_white(), 0);
        lv_obj_set_style_bg_opa(screen, LV_OPA_COVER, 0);
        lv_obj_set_style_text_color(screen, lv_color_black(), 0);
        lv_obj_set_style_text_font(screen, &BUILTIN_TEXT_FONT, 0);
        lv_obj_set_style_pad_all(screen, 0, 0);

        BuildDashboardUi(screen);
    }

    ESP_LOGI(TAG, "Desk dashboard UI ready");
    NotifyRefresh(REFRESH_FULL);

    if (data_task_handle_ == nullptr) {
        if (xTaskCreate(DataTaskEntry, "epaper_data", 7168, this, 1,
                        &data_task_handle_) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create dashboard data task");
        }
    }
}

const char* EpaperDisplayT42::WeekdayName(int tm_wday) {
    static constexpr const char* kNames[] =
        {"周日", "周一", "周二", "周三", "周四", "周五", "周六"};
    return (tm_wday >= 0 && tm_wday <= 6) ? kNames[tm_wday] : "周?";
}

void EpaperDisplayT42::UpdateClockLocked(bool force) {
    std::time_t now = std::time(nullptr);
    struct tm local = {};
    if (!ValidSystemTime(now, &local)) {
        if (force && date_label_ != nullptr) {
            lv_label_set_text(date_label_, "等待时间同步 · 中国时区");
        }
        return;
    }

    const int minute_key = local.tm_hour * 60 + local.tm_min;
    if (!force && minute_key == last_clock_minute_ && local.tm_yday == last_clock_yday_) return;

    SetClockDigit(0, local.tm_hour / 10);
    SetClockDigit(1, local.tm_hour % 10);
    SetClockDigit(2, local.tm_min / 10);
    SetClockDigit(3, local.tm_min % 10);

    std::string lunar = epaper_dashboard::FormatLunarDate(
        local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);

    // Keep only the lunar month/day in the compact clock row.  FormatLunarDate
    // may include an "农历" prefix and/or a ganzhi year; both are redundant on
    // this card and can make LVGL wrap the row on an 800x480 dashboard.
    const std::string lunar_prefix = "农历";
    if (lunar.rfind(lunar_prefix, 0) == 0) {
        lunar.erase(0, lunar_prefix.size());
    }
    const std::string year_mark = "年";
    const size_t year_pos = lunar.find(year_mark);
    if (year_pos != std::string::npos) {
        lunar = lunar.substr(year_pos + year_mark.size());
    }
    while (!lunar.empty() && (lunar.front() == ' ' || lunar.front() == '\t')) {
        lunar.erase(lunar.begin());
    }
    if (lunar.empty()) lunar = "--";

    // Gregorian date + weekday + compact lunar date are guaranteed to use one
    // label and one baseline. Location remains in the weather card.
    char date[128];
    std::snprintf(date, sizeof(date), "%d月%d日 %s · 农历%s",
                  local.tm_mon + 1, local.tm_mday, WeekdayName(local.tm_wday),
                  lunar.c_str());
    if (date_label_ != nullptr) {
        lv_label_set_text(date_label_, date);
        lv_label_set_long_mode(date_label_, LV_LABEL_LONG_DOT);
    }

    last_clock_minute_ = minute_key;
    last_clock_yday_ = local.tm_yday;
}

void EpaperDisplayT42::UpdateHeaderLocked() {
    if (status_label_ == nullptr) return;
    std::string status = "已联网 · ";
    status += status_text_.empty() ? "待唤醒" : status_text_;
    lv_label_set_text(status_label_, status.c_str());
}

void EpaperDisplayT42::UpdateWeatherLocked() {
    char buf[192];
    const auto& w = dashboard_.weather;
    std::snprintf(buf, sizeof(buf), "%s · %s %s",
                  w.city.c_str(), w.live ? "定位天气" : "静态", w.updated.c_str());
    lv_label_set_text(weather_title_label_, buf);

    lv_image_set_src(weather_icon_labels_[0], WeatherBitmap(w.current_code));
    for (int i = 0; i < 3; ++i) {
        lv_image_set_src(weather_icon_labels_[i + 1], WeatherBitmap(w.days[i + 1].weather_code));
    }

    std::snprintf(buf, sizeof(buf), "今日 %s\n%d°/%d°",
                  epaper_dashboard::DashboardDataProvider::WeatherText(w.current_code),
                  w.days[0].temp_max, w.days[0].temp_min);
    lv_label_set_text(weather_current_label_, buf);

    std::snprintf(buf, sizeof(buf), "AQI %d %s", w.aqi, w.aqi_grade.c_str());
    lv_label_set_text(weather_aqi_label_, buf);

    std::time_t now = std::time(nullptr);
    struct tm local = {};
    const bool time_ok = ValidSystemTime(now, &local);
    for (int i = 0; i < 3; ++i) {
        const auto& day = w.days[i + 1];
        const int weekday = time_ok ? (local.tm_wday + i + 1) % 7 : (i + 2);
        std::snprintf(buf, sizeof(buf), "%s\n%d°/%d°",
                      WeekdayName(weekday), day.temp_max, day.temp_min);
        lv_label_set_text(weather_forecast_labels_[i], buf);
    }
}

void EpaperDisplayT42::UpdateTodoLocked() {
    char buf[160];
    for (size_t i = 0; i < todo_labels_.size(); ++i) {
        const auto& item = dashboard_.todos[i];
        std::snprintf(buf, sizeof(buf), "%s  %s",
                      item.time.c_str(), item.title.c_str());
        lv_label_set_text(todo_labels_[i], buf);
    }
}

void EpaperDisplayT42::UpdateQuickLocked() {
    const epaper_dashboard::QuickItem* items[] = {
        &dashboard_.commute, &dashboard_.parcel, &dashboard_.home, &dashboard_.market
    };
    char buf[192];
    for (size_t i = 0; i < quick_labels_.size(); ++i) {
        std::snprintf(buf, sizeof(buf), "%s｜%s\n%s",
                      items[i]->title.c_str(), items[i]->line1.c_str(), items[i]->line2.c_str());
        lv_label_set_text(quick_labels_[i], buf);
    }
}

void EpaperDisplayT42::UpdateWordLocked() {
    const auto& word = dashboard_.word;
    lv_label_set_text(word_label_, word.word.c_str());
    lv_label_set_text(phonetic_label_, word.phonetic.c_str());
    lv_label_set_text(meaning_label_, word.meaning.c_str());

    std::string example = "例：" + word.example;
    example = TruncateUtf8(example, 90);
    lv_label_set_text(example_label_, example.c_str());

    char footer[128];
    std::snprintf(footer, sizeof(footer), "%d词 · %d分钟轮播 · %d/%d",
                  word.total, data_provider_.config().word_rotate_minutes,
                  word.index, word.total);
    lv_label_set_text(word_footer_label_, footer);
}

void EpaperDisplayT42::UpdateChatLocked() {
    if (user_label_ != nullptr) {
        std::string text = "你：" + (user_text_.empty() ? std::string("等待你说话")
                                                       : TruncateUtf8(user_text_, kMaxUserBytes));
        lv_label_set_text(user_label_, text.c_str());
    }
    if (assistant_label_ != nullptr) {
        std::string text = "小智：" +
            (assistant_text_.empty() ? std::string("准备好了，随时可以聊。")
                                     : TruncateUtf8(assistant_text_, kMaxAssistantBytes));
        lv_label_set_text(assistant_label_, text.c_str());
    }
}

std::string EpaperDisplayT42::TruncateUtf8(const std::string& text, size_t max_bytes) {
    if (text.size() <= max_bytes) return text;
    size_t end = max_bytes;
    while (end > 0 && end < text.size() &&
           (static_cast<unsigned char>(text[end]) & 0xC0) == 0x80) {
        --end;
    }
    return text.substr(0, end) + "...";
}

void EpaperDisplayT42::SetStatus(const char* status) {
    if (status == nullptr) return;
    Display::SetStatus(status);

    const bool is_speaking = (std::strcmp(status, Lang::Strings::SPEAKING) == 0);
    speaking_ = is_speaking;
    status_text_ = status;

    if (setup_ui_called_ && status_label_ != nullptr) {
        DisplayLockGuard lock(this);
        UpdateHeaderLocked();
    }

    pending_refresh_mask_.fetch_or(REFRESH_HEADER, std::memory_order_relaxed);
    if (!is_speaking) NotifyRefresh(REFRESH_HEADER);
}

void EpaperDisplayT42::ShowNotification(const char* notification, int duration_ms) {
    (void)duration_ms;
    if (notification == nullptr || notification[0] == '\0') return;
    Display::ShowNotification(notification, duration_ms);
    status_text_ = notification;

    if (setup_ui_called_) {
        DisplayLockGuard lock(this);
        UpdateHeaderLocked();
    }
    pending_refresh_mask_.fetch_or(REFRESH_HEADER, std::memory_order_relaxed);
    if (!speaking_) NotifyRefresh(REFRESH_HEADER);
}

void EpaperDisplayT42::ShowNotification(const std::string& notification, int duration_ms) {
    ShowNotification(notification.c_str(), duration_ms);
}

void EpaperDisplayT42::SetEmotion(const char* emotion) {
    Display::SetEmotion(emotion != nullptr ? emotion : "");
}

void EpaperDisplayT42::SetChatMessage(const char* role, const char* content) {
    if (role == nullptr || content == nullptr) return;
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
        if (content[0] != '\0' && std::strstr(content, "bread-compact-esp32") == nullptr) {
            assistant_text_ = content;
            changed = true;
        }
    }
    if (!changed) return;

    if (setup_ui_called_) {
        DisplayLockGuard lock(this);
        UpdateChatLocked();
    }
    pending_refresh_mask_.fetch_or(REFRESH_CHAT, std::memory_order_relaxed);
    if (!speaking_) NotifyRefresh(REFRESH_CHAT);
}

void EpaperDisplayT42::ClearChatMessages() {
    ESP_LOGI(TAG, "Preserve last Q&A on e-paper");
}

void EpaperDisplayT42::UpdateStatusBar(bool update_all) {
    if (!setup_ui_called_) return;

    const std::time_t now = std::time(nullptr);
    struct tm local = {};
    if (!ValidSystemTime(now, &local)) return;

    const int minute_key = local.tm_hour * 60 + local.tm_min;
    const bool minute_changed = minute_key != last_clock_minute_;
    const bool day_changed = local.tm_yday != last_clock_yday_;
    if (!update_all && !minute_changed && !day_changed) return;

    {
        DisplayLockGuard lock(this);
        UpdateClockLocked(update_all);
    }

    const uint32_t mask = (update_all || day_changed) ? REFRESH_DATE : REFRESH_CLOCK;
    ESP_LOGI(TAG, "Clock update %02d:%02d China time%s",
             local.tm_hour, local.tm_min, day_changed ? " (new day)" : "");
    NotifyRefresh(mask);
}

void EpaperDisplayT42::SetPowerSaveMode(bool on) {
    (void)on;
}

void EpaperDisplayT42::DataTaskEntry(void* arg) {
    static_cast<EpaperDisplayT42*>(arg)->DataTaskLoop();
}

void EpaperDisplayT42::DataTaskLoop() {
    vTaskDelay(pdMS_TO_TICKS(6000));

    const auto& cfg = data_provider_.config();
    int64_t now_ms = esp_timer_get_time() / 1000;
    int64_t next_weather_ms = now_ms;
    int64_t next_api_ms = now_ms + 3000;
    int64_t next_full_ms = now_ms + static_cast<int64_t>(cfg.full_refresh_minutes) * 60000;

    for (;;) {
        const std::time_t now = std::time(nullptr);
        struct tm local = {};
        if (ValidSystemTime(now, &local)) {
            const int word_slot =
                (local.tm_hour * 60 + local.tm_min) / std::max(5, cfg.word_rotate_minutes);
            if (!dashboard_.custom_api_live && word_slot != last_word_slot_) {
                dashboard_.word = data_provider_.GetLocalWord(now);
                last_word_slot_ = word_slot;
                {
                    DisplayLockGuard lock(this);
                    UpdateWordLocked();
                }
                NotifyRefresh(REFRESH_WORD);
            }
        }

        now_ms = esp_timer_get_time() / 1000;
        if (now_ms >= next_weather_ms) {
            auto weather = dashboard_.weather;
            if (data_provider_.FetchWeather(weather)) {
                const bool city_changed = weather.city != dashboard_.weather.city;
                dashboard_.weather = weather;
                {
                    DisplayLockGuard lock(this);
                    UpdateWeatherLocked();
                    if (city_changed) UpdateClockLocked(true);
                }
                NotifyRefresh(REFRESH_WEATHER | (city_changed ? REFRESH_DATE : REFRESH_NONE));
                next_weather_ms =
                    now_ms + static_cast<int64_t>(cfg.weather_refresh_minutes) * 60000;
            } else {
                next_weather_ms = now_ms + 120000;
            }
        }

        if (!cfg.custom_api_url.empty() && now_ms >= next_api_ms) {
            auto candidate = dashboard_;
            if (data_provider_.FetchCustomDashboard(candidate)) {
                dashboard_ = candidate;
                {
                    DisplayLockGuard lock(this);
                    UpdateTodoLocked();
                    UpdateQuickLocked();
                    UpdateWordLocked();
                }
                NotifyRefresh(REFRESH_TODO | REFRESH_QUICK | REFRESH_WORD);
                next_api_ms =
                    now_ms + static_cast<int64_t>(cfg.custom_api_refresh_minutes) * 60000;
            } else {
                next_api_ms = now_ms + 120000;
            }
        }

        if (now_ms >= next_full_ms) {
            ESP_LOGI(TAG, "Scheduled anti-ghost full refresh");
            NotifyRefresh(REFRESH_FULL);
            next_full_ms = now_ms + static_cast<int64_t>(cfg.full_refresh_minutes) * 60000;
        }

        vTaskDelay(pdMS_TO_TICKS(5000));
    }
}

void EpaperDisplayT42::LvglFlushCb(
    lv_display_t* disp, const lv_area_t* area, uint8_t* color_p) {
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
        if (self->partial_buffer_ == nullptr || self->partial_row_bytes_ == 0) {
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
                const size_t dst_row =
                    static_cast<size_t>(y - self->capture_area_.y1) * self->partial_row_bytes_;
                const size_t src_row =
                    static_cast<size_t>(y - area->y1) * static_cast<size_t>(area_width);

                for (int x = x0; x <= x1; ++x) {
                    const uint16_t p = pixels[src_row + static_cast<size_t>(x - area->x1)];
                    const int local_x = x - self->capture_area_.x1;
                    uint8_t& out =
                        self->partial_buffer_[dst_row + static_cast<size_t>(local_x >> 3)];
                    const uint8_t bit = static_cast<uint8_t>(0x80 >> (local_x & 7));
                    if (PixelIsBlack(p)) out &= static_cast<uint8_t>(~bit);
                    else out |= bit;
                }
            }
        }
        lv_display_flush_ready(disp);
        return;
    }

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
            if (PixelIsBlack(p)) {
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
    if (refresh_task_handle_ != nullptr) {
        xTaskNotifyGive(refresh_task_handle_);
    }
}

void EpaperDisplayT42::RefreshTaskEntry(void* arg) {
    static_cast<EpaperDisplayT42*>(arg)->RefreshTaskLoop();
}

void EpaperDisplayT42::RefreshTaskLoop() {
    for (;;) {
        ulTaskNotifyTake(pdTRUE, portMAX_DELAY);
        while (ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(kPartialDebounceMs)) > 0) {}

        if (speaking_) {
            ESP_LOGI(TAG, "Refresh deferred while TTS is speaking");
            continue;
        }

        uint32_t mask =
            pending_refresh_mask_.exchange(REFRESH_NONE, std::memory_order_acq_rel);
        if (mask == REFRESH_NONE) continue;

        const bool force_full =
            !full_refresh_done_ || !panel_ram_valid_ ||
            ((mask & REFRESH_FULL) != 0) ||
            (partial_refresh_count_ >= kPartialRefreshLimit);

        bool ok = force_full ? RefreshPanelFull() : RefreshPanelPartial(mask);
        if (!ok && !force_full) {
            ESP_LOGW(TAG, "Partial refresh failed; rebuilding full panel RAM");
            panel_ram_valid_ = false;
            SleepPanel();
            ok = RefreshPanelFull();
        }
        if (!ok) {
            ESP_LOGE(TAG, "Panel refresh failed; dirty mask retained");
            pending_refresh_mask_.fetch_or(mask, std::memory_order_relaxed);
        }
    }
}

bool EpaperDisplayT42::StreamCurrentUiToPanel() {
    if (display_ == nullptr) return false;

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
    if (mono_line_ == nullptr) return false;
    std::memset(mono_line_, value, MONO_LINE_BYTES);
    gpio_set_level(EPD_DC_PIN, 1);
    for (int y = 0; y < EPD_HEIGHT; ++y) {
        if (SpiWrite(mono_line_, MONO_LINE_BYTES) != ESP_OK) return false;
    }
    return true;
}

bool EpaperDisplayT42::CaptureUiRegion(
    int x_start, int y_start, int x_end, int y_end) {
    x_start = std::max(0, x_start);
    y_start = std::max(0, y_start);
    x_end = std::min(EPD_WIDTH, x_end);
    y_end = std::min(EPD_HEIGHT, y_end);
    if (display_ == nullptr || x_start >= x_end || y_start >= y_end) return false;

    x_start &= ~7;
    x_end = std::min(EPD_WIDTH, (x_end + 7) & ~7);
    const size_t row_bytes = static_cast<size_t>((x_end - x_start) / 8);
    const size_t required = row_bytes * static_cast<size_t>(y_end - y_start);

    if (partial_buffer_ == nullptr || partial_buffer_size_ < required) {
        ReleasePartialBuffer();
        partial_buffer_ = static_cast<uint8_t*>(
            heap_caps_malloc(required, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (partial_buffer_ == nullptr) {
            partial_buffer_ = static_cast<uint8_t*>(heap_caps_malloc(required, MALLOC_CAP_8BIT));
        }
        if (partial_buffer_ == nullptr) {
            ESP_LOGE(TAG, "Failed partial buffer %u bytes", static_cast<unsigned>(required));
            partial_row_bytes_ = 0;
            return false;
        }
        partial_buffer_size_ = required;
    }
    partial_row_bytes_ = row_bytes;
    std::memset(partial_buffer_, 0xFF, required);

    capture_area_.x1 = x_start;
    capture_area_.x2 = x_end - 1;
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

bool EpaperDisplayT42::WriteCapturedPartialRegion() {
    if (partial_buffer_ == nullptr || partial_row_bytes_ == 0 || mono_line_ == nullptr) return false;

    const int x_start = capture_area_.x1;
    const int y_start = capture_area_.y1;
    const int x_end = capture_area_.x2 + 1;
    const int y_end = capture_area_.y2 + 1;

    SendCommand(0x91);
    if (!SetPartialWindow(x_start, y_start, x_end, y_end)) return false;
    SendCommand(0x13);
    gpio_set_level(EPD_DC_PIN, 1);
    const int rows = y_end - y_start;
    for (int row = 0; row < rows; ++row) {
        const uint8_t* src =
            partial_buffer_ + static_cast<size_t>(row) * partial_row_bytes_;
        std::memcpy(mono_line_, src, partial_row_bytes_);
        if (SpiWrite(mono_line_, partial_row_bytes_) != ESP_OK) return false;
    }
    SendCommand(0x92);

    if (!SetPartialWindow(x_start, y_start, x_end, y_end)) return false;
    SendCommand(0x12);
    vTaskDelay(pdMS_TO_TICKS(kRefreshTriggerDelayMs));
    return WaitBusyRelease("partial-refresh", EPD_BUSY_TIMEOUT_MS);
}

void EpaperDisplayT42::ReleasePartialBuffer() {
    if (partial_buffer_ != nullptr) {
        heap_caps_free(partial_buffer_);
        partial_buffer_ = nullptr;
    }
    partial_buffer_size_ = 0;
    partial_row_bytes_ = 0;
}

esp_err_t EpaperDisplayT42::SpiWrite(const uint8_t* data, size_t len) {
    if (spi_ == nullptr || data == nullptr || len == 0) return ESP_ERR_INVALID_ARG;

    size_t offset = 0;
    while (offset < len) {
        const size_t n = std::min(kSpiChunk, len - offset);
        spi_transaction_t t = {};
        t.length = n * 8;
        t.tx_buffer = data + offset;
        const esp_err_t err = spi_device_polling_transmit(spi_, &t);
        if (err != ESP_OK) return err;
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
    SendCommand(0x00); SendData(0x1F);
    SendCommand(0x01);
    SendData(0x07); SendData(0x07); SendData(0x3F); SendData(0x3F); SendData(0x09);
    SendCommand(0x06);
    SendData(0x17); SendData(0x17); SendData(0x28); SendData(0x17);
    SendCommand(0x61);
    SendData(0x03); SendData(0x20); SendData(0x01); SendData(0xE0);
    SendCommand(0x15); SendData(0x00);
    // 0x29 enables N2OCP: after a refresh the controller copies NEW RAM to OLD
    // RAM, which is exactly the baseline required by the next differential update.
    SendCommand(0x50); SendData(0x29); SendData(0x07);
    SendCommand(0x60); SendData(0x22);
    SendCommand(0xE3); SendData(0x22);
}

bool EpaperDisplayT42::InitPanelFullRefresh() {
    // Only reset after true controller hibernate or recovery. Routine power-off
    // keeps UC8179 RAM alive so OLD/NEW differential state remains coherent.
    if (controller_hibernating_) {
        HardwareReset();
        controller_hibernating_ = false;
        panel_ram_valid_ = false;
    }
    ConfigurePanelBase();
    SendCommand(0x00); SendData(0x1F);
    SendCommand(0x04);
    vTaskDelay(pdMS_TO_TICKS(kRefreshTriggerDelayMs));
    if (!WaitBusyRelease("power-on/full", EPD_BUSY_TIMEOUT_MS)) {
        panel_powered_ = false;
        return false;
    }
    panel_powered_ = true;
    return true;
}

bool EpaperDisplayT42::InitPanelPartialRefresh() {
    // Never start a differential waveform after RAM state was lost. Rebuild it
    // with one full refresh instead of applying a partial update against an
    // unknown OLD plane, which shows up as washed-out / fading pixels.
    if (controller_hibernating_ || !panel_ram_valid_) {
        ESP_LOGW(TAG, "Partial refresh requested without valid panel RAM");
        return false;
    }

    ConfigurePanelBase();
    SendCommand(0xE0); SendData(0x02);
    SendCommand(0xE5); SendData(0x6E);
    SendCommand(0x04);
    vTaskDelay(pdMS_TO_TICKS(kRefreshTriggerDelayMs));
    if (!WaitBusyRelease("power-on/partial", EPD_BUSY_TIMEOUT_MS)) {
        panel_powered_ = false;
        panel_ram_valid_ = false;
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
    if (x_start >= x_end || y_start >= y_end) return false;

    const int panel_x_start = x_start & ~7;
    const int panel_x_end = (x_end - 1) | 0x07;
    const int panel_y_end = y_end - 1;

    SendCommand(0x90);
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
    ESP_LOGI(TAG, "Full refresh begin (ram_valid=%d)", panel_ram_valid_ ? 1 : 0);
    if (!InitPanelFullRefresh()) {
        SleepPanel();
        return false;
    }

    // GxEPD2 initializes OLD RAM to black only for the initial/recovery refresh.
    // Do not overwrite OLD with black on every maintenance full refresh: doing so
    // destroys the differential baseline and needlessly overdrives the next cycle.
    if (!panel_ram_valid_) {
        SendCommand(0x10);
        if (!StreamSolidPlane(0x00)) {
            SleepPanel();
            return false;
        }
    }

    SendCommand(0x13);
    if (!StreamCurrentUiToPanel()) {
        panel_ram_valid_ = false;
        SleepPanel();
        return false;
    }

    SendCommand(0xE0); SendData(0x02);
    SendCommand(0xE5); SendData(0x5A);
    SendCommand(0x12);
    vTaskDelay(pdMS_TO_TICKS(kRefreshTriggerDelayMs));
    const bool ok = WaitBusyRelease("display-refresh/full", EPD_BUSY_TIMEOUT_MS);

    if (ok) {
        // VCOM setting 0x29 enables N2OCP, so the completed NEW image becomes the
        // OLD baseline. Power off the high-voltage rails, but do NOT deep-sleep
        // the controller between normal updates; its RAM is the differential state.
        panel_ram_valid_ = true;
        full_refresh_done_ = true;
        partial_refresh_count_ = 0;
        PowerOffPanel();
    } else {
        panel_ram_valid_ = false;
        SleepPanel();
    }

    ESP_LOGI(TAG, "Full refresh %s", ok ? "done" : "failed");
    return ok;
}

bool EpaperDisplayT42::RefreshPartialRegion(
    int x_start, int y_start, int x_end, int y_end, const char* name) {
    ESP_LOGI(TAG, "Partial %s x=%d..%d y=%d..%d",
             name, x_start, x_end - 1, y_start, y_end - 1);
    if (!CaptureUiRegion(x_start, y_start, x_end, y_end)) return false;
    return WriteCapturedPartialRegion();
}

bool EpaperDisplayT42::RefreshPanelPartial(uint32_t mask) {
    ESP_LOGI(TAG, "Partial batch begin mask=0x%08lx",
             static_cast<unsigned long>(mask));

    if (!InitPanelPartialRefresh()) {
        return false;
    }

    bool ok = true;
    uint32_t count = 0;

    auto refresh_region = [&](const Region& r) {
        if (!ok) return;
        ok = RefreshPartialRegion(r.x0, r.y0, r.x1, r.y1, r.name);
        if (ok) ++count;
    };

    const uint32_t top_mask = mask & (REFRESH_CLOCK | REFRESH_DATE | REFRESH_HEADER);
    if (top_mask != 0) {
        if ((top_mask & REFRESH_HEADER) != 0) refresh_region(kHeaderRegion);
        else if ((top_mask & REFRESH_DATE) != 0) refresh_region(kClockCardRegion);
        else refresh_region(kClockDigitsRegion);
    }

    const uint32_t middle_mask = mask & (REFRESH_WEATHER | REFRESH_TODO | REFRESH_QUICK);
    if (ok && middle_mask != 0) {
        if ((middle_mask & REFRESH_QUICK) != 0) refresh_region(kQuickRegion);
        else if ((middle_mask & REFRESH_TODO) != 0) refresh_region(kTodoRegion);
        else refresh_region(kWeatherRegion);
    }

    const uint32_t bottom_mask = mask & (REFRESH_WORD | REFRESH_CHAT);
    if (ok && bottom_mask != 0) {
        if ((bottom_mask & REFRESH_CHAT) != 0) refresh_region(kChatRegion);
        else refresh_region(kWordRegion);
    }

    ReleasePartialBuffer();
    if (ok) {
        partial_refresh_count_ += count;
        // Preserve OLD/NEW RAM. Only the high-voltage panel supply is switched off.
        PowerOffPanel();
    } else {
        panel_ram_valid_ = false;
        SleepPanel();
    }

    ESP_LOGI(TAG, "Partial batch %s count=%lu/%u ram=%s",
             ok ? "done" : "failed",
             static_cast<unsigned long>(partial_refresh_count_),
             static_cast<unsigned>(kPartialRefreshLimit),
             panel_ram_valid_ ? "valid" : "invalid");
    return ok;
}

void EpaperDisplayT42::PowerOffPanel() {
    if (spi_ == nullptr || !panel_powered_) return;

    // Match GxEPD2 _PowerOff(): disable panel driving voltages but leave the
    // controller awake. This prevents static-image fading while retaining RAM.
    SendCommand(0x02);
    vTaskDelay(pdMS_TO_TICKS(20));
    WaitBusyRelease("power-off", 3000);
    panel_powered_ = false;
}

void EpaperDisplayT42::SleepPanel() {
    if (spi_ == nullptr) return;

    PowerOffPanel();
    SendCommand(0x07);
    SendData(0xA5);
    controller_hibernating_ = true;
    panel_ram_valid_ = false;
}

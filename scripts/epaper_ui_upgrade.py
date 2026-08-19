#!/usr/bin/env python3
"""Apply the Inksight-inspired e-paper typography/grid refinement.

The reference firmware renders typography directly into a monochrome framebuffer:
its 5x7 ASCII font and hand-authored 16x16 CJK glyphs have no anti-aliased gray
edge pixels.  For this Chinese LVGL dashboard we keep the broad Noto glyph
coverage, but select the project's 1-bpp Noto Sans asset so the glyph raster is
already monochrome before it reaches the GDEY075T7 conversion path.

The patch is idempotent and intentionally limited to bread-compact-esp32.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BOARD = ROOT / "main/boards/bread-compact-esp32"
ICON = BOARD / "epaper_dashboard_icons.h"
SRC = BOARD / "epaper_display_t42.cc"
CMAKE = ROOT / "main/CMakeLists.txt"


def replace_once(text: str, old: str, new: str, label: str) -> str:
    if old in text:
        return text.replace(old, new, 1)
    if new in text:
        return text
    raise SystemExit(f"e-paper UI patch anchor missing: {label}")


def replace_function(text: str, signature: str, next_signature: str, body: str) -> str:
    start = text.find(signature)
    end = text.find(next_signature, start + len(signature))
    if start < 0 or end < 0:
        if body.strip() in text:
            return text
        raise SystemExit(f"e-paper UI function anchor missing: {signature}")
    return text[:start] + body + "\n\n" + text[end:]


def patch_cmake() -> None:
    t = CMAKE.read_text(encoding="utf-8")
    old = '''elseif(CONFIG_BOARD_TYPE_BREAD_COMPACT_ESP32)\n    set(BOARD_DIR "bread-compact-esp32")\n    # 16px/4bpp improves Chinese stroke definition on the 800x480 e-paper.\n    set(BUILTIN_TEXT_FONT font_noto_sans_basic_16_4)\n    set(BUILTIN_ICON_FONT font_material_symbols_16_4)'''
    new = '''elseif(CONFIG_BOARD_TYPE_BREAD_COMPACT_ESP32)\n    set(BOARD_DIR "bread-compact-esp32")\n    # Inksight-style crisp raster: use the project's native 1-bpp Noto Sans.\n    # 14px keeps dense Chinese dashboard copy readable without gray AA fringes.\n    set(BUILTIN_TEXT_FONT font_noto_sans_basic_14_1)\n    set(BUILTIN_ICON_FONT font_material_symbols_14_1)'''
    t = replace_once(t, old, new, "bread compact monochrome font")
    CMAKE.write_text(t, encoding="utf-8")


def patch_source() -> None:
    t = SRC.read_text(encoding="utf-8")

    t = replace_once(
        t,
        'constexpr Region kClockDigitsRegion {0, 0,   264, 90,  "clock-digits"};\n'
        'constexpr Region kClockCardRegion   {0, 0,   336, 136, "clock-card"};\n'
        'constexpr Region kHeaderRegion      {0, 0,   800, 136, "header"};\n'
        'constexpr Region kWeatherRegion     {0, 132, 352, 304, "weather"};\n'
        'constexpr Region kTodoRegion        {0, 132, 536, 304, "todo"};\n'
        'constexpr Region kQuickRegion       {0, 132, 800, 304, "quick"};\n'
        'constexpr Region kWordRegion        {0, 300, 296, 480, "word"};\n'
        'constexpr Region kChatRegion        {0, 300, 800, 480, "chat"};',
        'constexpr Region kClockDigitsRegion {0, 0,   296, 92,  "clock-digits"};\n'
        'constexpr Region kClockCardRegion   {0, 0,   338, 138, "clock-card"};\n'
        'constexpr Region kHeaderRegion      {0, 0,   800, 138, "header"};\n'
        'constexpr Region kWeatherRegion     {0, 132, 350, 304, "weather"};\n'
        'constexpr Region kTodoRegion        {0, 132, 540, 304, "todo"};\n'
        'constexpr Region kQuickRegion       {0, 132, 800, 304, "quick"};\n'
        'constexpr Region kWordRegion        {0, 298, 296, 480, "word"};\n'
        'constexpr Region kChatRegion        {0, 298, 800, 480, "chat"};',
        "aligned refresh regions",
    )

    t = replace_once(
        t,
        'return luminance < (42u * 64u); // thicken 4bpp anti-aliased text for 1-bit e-paper',
        'return luminance < (32u * 64u); // crisp midpoint threshold; text/font assets are already 1-bpp',
        "monochrome threshold",
    )
    t = replace_once(t, 'lv_obj_set_style_radius(box, 2, 0);',
                     'lv_obj_set_style_radius(box, 0, 0); // pixel-sharp grid on monochrome e-paper',
                     "square grid boxes")
    t = replace_once(t, 'constexpr int x0 = 22;', 'constexpr int x0 = 58;', "centered clock")

    build_ui = r'''void EpaperDisplayT42::BuildDashboardUi(lv_obj_t* screen) {
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
    auto one_line = [&](lv_obj_t* label, int height = 18) {
        if (label == nullptr) return;
        lv_obj_set_height(label, height);
        lv_label_set_long_mode(label, LV_LABEL_LONG_DOT);
    };

    CreateSevenSegmentClock(screen);
    date_label_ = CreateLabel(screen, 18, 101, 302, "日期 · 农历", LV_TEXT_ALIGN_CENTER);
    one_line(date_label_, 20);
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
    for (auto* label : todo_labels_) one_line(label, 19);

    const int quick_y[4] = {149, 186, 223, 260};
    CreateBitmapIcon(screen, 550, 146, &ep_icon_commute_20);
    CreateBitmapIcon(screen, 550, 183, &ep_icon_parcel_20);
    CreateBitmapIcon(screen, 550, 220, &ep_icon_home_20);
    CreateBitmapIcon(screen, 550, 257, &ep_icon_market_20);
    for (int i = 0; i < 4; ++i) {
        quick_labels_[i] = CreateLabel(screen, 576, quick_y[i], 202, "");
        one_line(quick_labels_[i], 19);
    }

    CreateBitmapIcon(screen, 18, 313, &ep_icon_word_20);
    auto* word_title = CreateLabel(screen, 42, 316, 226, "每日记单词");
    one_line(word_title);
    word_label_ = CreateLabel(screen, 18, 343, 250, "abandon");
    one_line(word_label_, 20);
    phonetic_label_ = CreateLabel(screen, 18, 366, 250, "/əˈbændən/");
    one_line(phonetic_label_, 20);
    meaning_label_ = CreateLabel(screen, 18, 390, 250, "放弃；遗弃");
    one_line(meaning_label_, 20);
    example_label_ = CreateLabel(screen, 18, 416, 255, "例：Don't abandon your plan.");
    lv_obj_set_height(example_label_, 30);
    lv_label_set_long_mode(example_label_, LV_LABEL_LONG_WRAP);
    word_footer_label_ = CreateLabel(screen, 18, 451, 255, "20词 · 30分钟轮播 · 1/20");
    one_line(word_footer_label_, 18);

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
    one_line(chat_hint, 18);

    UpdateClockLocked(true);
    UpdateHeaderLocked();
    UpdateWeatherLocked();
    UpdateTodoLocked();
    UpdateQuickLocked();
    UpdateWordLocked();
    UpdateChatLocked();
}'''
    t = replace_function(t, "void EpaperDisplayT42::BuildDashboardUi(lv_obj_t* screen) {",
                         "void EpaperDisplayT42::SetupUI()", build_ui)

    update_clock = r'''void EpaperDisplayT42::UpdateClockLocked(bool force) {
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

    const std::string lunar = epaper_dashboard::FormatLunarDate(
        local.tm_year + 1900, local.tm_mon + 1, local.tm_mday);

    // Date + weekday + lunar date share one crisp baseline. Location already
    // appears in the weather card, so the clock card stays uncluttered.
    char date[128];
    std::snprintf(date, sizeof(date), "%d月%d日 %s · %s",
                  local.tm_mon + 1, local.tm_mday, WeekdayName(local.tm_wday),
                  lunar.c_str());
    if (date_label_ != nullptr) lv_label_set_text(date_label_, date);

    last_clock_minute_ = minute_key;
    last_clock_yday_ = local.tm_yday;
}'''
    t = replace_function(t, "void EpaperDisplayT42::UpdateClockLocked(bool force) {",
                         "void EpaperDisplayT42::UpdateHeaderLocked()", update_clock)

    SRC.write_text(t, encoding="utf-8")


def validate() -> None:
    icon_text = ICON.read_text(encoding="utf-8")
    src_text = SRC.read_text(encoding="utf-8")
    cmake_text = CMAKE.read_text(encoding="utf-8")

    checks = [
        ("LV_IMAGE_HEADER_MAGIC, LV_COLOR_FORMAT_I1, 0," in icon_text, "C++17-compatible I1 icons"),
        ("font_noto_sans_basic_14_1" in cmake_text, "1-bpp Noto Sans"),
        ('constexpr int x0 = 58;' in src_text, "centered clock"),
        ('lunar_label_ = nullptr' in src_text, "date+lunar single line"),
        ('CreateBox(screen, 6,   6, 326, 126);' in src_text, "aligned 6px grid"),
    ]
    for ok, label in checks:
        if not ok:
            raise SystemExit(f"e-paper UI validation failed: {label}")

    if ".header.magic" in icon_text or ".header.cf" in icon_text:
        raise SystemExit("e-paper UI validation failed: nested designated initializer found")


def main() -> None:
    patch_cmake()
    patch_source()
    validate()
    print("Inksight-inspired crisp typography/grid refinement applied")


if __name__ == "__main__":
    main()

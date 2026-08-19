#!/usr/bin/env python3
"""Apply the Inksight-inspired e-paper typography/grid refinement.

Inksight gets very crisp text because it rasterizes directly to monochrome pixels,
but its hand-authored Chinese glyphs are 16x16.  A 14px 1-bpp general Chinese
font is too tight for complex CJK strokes on this dashboard.  For bread-compact-
esp32 we therefore use the known-good 16px/4bpp Noto asset for glyph coverage,
then convert it to the 1-bit panel with a conservative threshold.  This keeps
strokes complete while retaining a crisp monochrome result.

The patch also gives single-line labels enough vertical room and compacts the
lunar string so Gregorian date, weekday and lunar date always share one row.
The patch is idempotent and intentionally limited to bread-compact-esp32.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BOARD = ROOT / "main/boards/bread-compact-esp32"
SRC = BOARD / "epaper_display_t42.cc"
CMAKE = ROOT / "main/CMakeLists.txt"


def replace_any(text: str, olds, new: str, label: str) -> str:
    for old in olds:
        if old in text:
            return text.replace(old, new, 1)
    if new in text:
        return text
    raise SystemExit(f"e-paper UI patch anchor missing: {label}")


def replace_function(text: str, signature: str, next_signature: str, body: str) -> str:
    start = text.find(signature)
    end = text.find(next_signature, start + len(signature))
    if start < 0 or end < 0:
        raise SystemExit(f"e-paper UI function anchor missing: {signature}")
    return text[:start] + body + "\n\n" + text[end:]


def patch_cmake() -> None:
    t = CMAKE.read_text(encoding="utf-8")
    old_14 = '''elseif(CONFIG_BOARD_TYPE_BREAD_COMPACT_ESP32)\n    set(BOARD_DIR "bread-compact-esp32")\n    # Inksight-style crisp raster: use the project's native 1-bpp Noto Sans.\n    # 14px keeps dense Chinese dashboard copy readable without gray AA fringes.\n    set(BUILTIN_TEXT_FONT font_noto_sans_basic_14_1)\n    set(BUILTIN_ICON_FONT font_material_symbols_14_1)'''
    new_16 = '''elseif(CONFIG_BOARD_TYPE_BREAD_COMPACT_ESP32)\n    set(BOARD_DIR "bread-compact-esp32")\n    # 16px preserves complex Chinese strokes; the e-paper output path still\n    # converts the rendered glyphs to a crisp 1-bit raster for GDEY075T7.\n    set(BUILTIN_TEXT_FONT font_noto_sans_basic_16_4)\n    set(BUILTIN_ICON_FONT font_material_symbols_16_4)'''
    t = replace_any(t, [old_14], new_16, "bread-compact 16px Chinese font")
    CMAKE.write_text(t, encoding="utf-8")


def patch_source() -> None:
    t = SRC.read_text(encoding="utf-8")

    t = replace_any(
        t,
        [
            'return luminance < (32u * 64u); // crisp midpoint threshold; text/font assets are already 1-bpp',
            'return luminance < (42u * 64u); // thicken 4bpp anti-aliased text for 1-bit e-paper',
        ],
        'return luminance < (38u * 64u); // keep 16px CJK strokes complete without over-thickening',
        "CJK raster threshold",
    )

    # Complex 16px Chinese glyphs need a little more vertical room than the
    # former 14px font.  The dashboard rows have enough spacing for 22px labels.
    t = replace_any(
        t,
        ['auto one_line = [&](lv_obj_t* label, int height = 18) {',
         'auto one_line = [&](lv_obj_t* label, int height = 22) {'],
        'auto one_line = [&](lv_obj_t* label, int height = 22) {',
        "single-line label height",
    )

    t = replace_any(
        t,
        ['date_label_ = CreateLabel(screen, 18, 101, 302, "日期 · 农历", LV_TEXT_ALIGN_CENTER);\n    one_line(date_label_, 20);',
         'date_label_ = CreateLabel(screen, 18, 99, 306, "日期 · 农历", LV_TEXT_ALIGN_CENTER);\n    one_line(date_label_, 24);'],
        'date_label_ = CreateLabel(screen, 14, 99, 312, "日期 · 农历", LV_TEXT_ALIGN_CENTER);\n    one_line(date_label_, 24);',
        "date/lunar row geometry",
    )

    for old, new, label in [
        ('for (auto* label : todo_labels_) one_line(label, 19);',
         'for (auto* label : todo_labels_) one_line(label, 22);', "todo row height"),
        ('one_line(quick_labels_[i], 19);',
         'one_line(quick_labels_[i], 22);', "quick row height"),
        ('one_line(word_label_, 20);',
         'one_line(word_label_, 22);', "word row height"),
        ('one_line(phonetic_label_, 20);',
         'one_line(phonetic_label_, 22);', "phonetic row height"),
        ('one_line(meaning_label_, 20);',
         'one_line(meaning_label_, 22);', "meaning row height"),
        ('one_line(word_footer_label_, 18);',
         'one_line(word_footer_label_, 20);', "word footer height"),
        ('one_line(chat_hint, 18);',
         'one_line(chat_hint, 20);', "chat hint height"),
    ]:
        if old in t:
            t = t.replace(old, new, 1)

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
}'''
    t = replace_function(t,
                         "void EpaperDisplayT42::UpdateClockLocked(bool force) {",
                         "void EpaperDisplayT42::UpdateHeaderLocked()",
                         update_clock)

    SRC.write_text(t, encoding="utf-8")


def validate() -> None:
    src = SRC.read_text(encoding="utf-8")
    cmake = CMAKE.read_text(encoding="utf-8")
    checks = [
        ("font_noto_sans_basic_16_4" in cmake, "16px CJK font"),
        ("38u * 64u" in src, "crisp CJK threshold"),
        ('CreateLabel(screen, 14, 99, 312, "日期 · 农历"' in src, "wide date row"),
        ('· 农历%s' in src, "single-row compact lunar format"),
        ('lunar.find(year_mark)' in src, "lunar year compaction"),
        ('one_line = [&](lv_obj_t* label, int height = 22)' in src, "safe CJK label height"),
    ]
    for ok, label in checks:
        if not ok:
            raise SystemExit(f"e-paper UI validation failed: {label}")
    print("e-paper CJK completeness + date/lunar single-line patch applied")


def main() -> None:
    patch_cmake()
    patch_source()
    validate()


if __name__ == "__main__":
    main()

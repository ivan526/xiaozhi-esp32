#!/usr/bin/env python3
"""Validate the already-applied bread-compact-esp32 e-paper UI upgrade.

The original one-shot generator used nested C designated initializers such as
`.header.magic = ...`.  That syntax is accepted by C but is not valid for the
ESP-IDF C++17 build used by epaper_display_t42.cc.  The generated icon header is
now committed with plain aggregate initialization, matching LVGL v9 while
remaining C++17/GNU++ compatible.

This script is intentionally idempotent.  The graphical assets and source
changes are already version-controlled; rerunning the UI-upgrade workflow must
not regenerate the old incompatible descriptor syntax.
"""
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
BOARD = ROOT / "main/boards/bread-compact-esp32"
ICON = BOARD / "epaper_dashboard_icons.h"
SRC = BOARD / "epaper_display_t42.cc"
CMAKE = ROOT / "main/CMakeLists.txt"


def require(path: Path, needle: str, label: str) -> None:
    text = path.read_text(encoding="utf-8")
    if needle not in text:
        raise SystemExit(f"e-paper UI validation failed: {label}")


def main() -> None:
    require(ICON, "LV_IMAGE_HEADER_MAGIC, LV_COLOR_FORMAT_I1, 0,", "C++17-compatible I1 descriptors")
    require(ICON, "nullptr, nullptr", "LVGL v9 reserved descriptor fields")
    require(SRC, '#include "epaper_dashboard_icons.h"', "dashboard icon include")
    require(SRC, "WeatherBitmap", "weather bitmap mapping")
    require(CMAKE, "font_noto_sans_basic_16_4", "16px 4bpp e-paper text font")

    # Guard against accidentally reintroducing the syntax that broke CI.
    icon_text = ICON.read_text(encoding="utf-8")
    if ".header.magic" in icon_text or ".header.cf" in icon_text:
        raise SystemExit("e-paper UI validation failed: nested designated initializer found")

    print("e-paper UI graphics/font upgrade already applied and validated")


if __name__ == "__main__":
    main()

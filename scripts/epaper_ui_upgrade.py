#!/usr/bin/env python3
"""One-shot upgrade for bread-compact-esp32 e-paper dashboard graphics.

The generated icon header intentionally follows scripts/Image_Converter/LVGLImage.py
LVGL-v9 C-array conventions: LV_COLOR_FORMAT_I1, 2-color BGRA palette and
MSB-first packed rows.  The raster primitives are generated locally so CI does
not need pngquant/Pillow just to rebuild these tiny monochrome assets.
"""
from pathlib import Path
import math

ROOT = Path(__file__).resolve().parents[1]
BOARD = ROOT / "main/boards/bread-compact-esp32"
SRC = BOARD / "epaper_display_t42.cc"
ICON = BOARD / "epaper_dashboard_icons.h"
CMAKE = ROOT / "main/CMakeLists.txt"

class Canvas:
    def __init__(self, w, h):
        self.w, self.h = w, h
        self.p = [[0] * w for _ in range(h)]
    def dot(self, x, y, r=0):
        x, y = int(round(x)), int(round(y))
        for yy in range(y-r, y+r+1):
            for xx in range(x-r, x+r+1):
                if 0 <= xx < self.w and 0 <= yy < self.h and (xx-x)**2 + (yy-y)**2 <= r*r + 1:
                    self.p[yy][xx] = 1
    def line(self, x0, y0, x1, y1, width=2):
        dx, dy = x1-x0, y1-y0
        steps = max(abs(int(dx)), abs(int(dy)), 1)
        for i in range(steps+1):
            t=i/steps
            self.dot(x0+dx*t, y0+dy*t, max(0,width//2))
    def poly(self, pts, width=2):
        for a,b in zip(pts, pts[1:]): self.line(*a,*b,width)
    def ellipse(self, x0, y0, x1, y1, width=2, start=0, end=360):
        cx, cy=(x0+x1)/2,(y0+y1)/2; rx,ry=(x1-x0)/2,(y1-y0)/2
        n=max(40,int((abs(rx)+abs(ry))*8))
        for i in range(n+1):
            a=math.radians(start+(end-start)*i/n)
            self.dot(cx+rx*math.cos(a),cy+ry*math.sin(a),max(0,width//2))
    def rect(self,x0,y0,x1,y1,width=2):
        self.line(x0,y0,x1,y0,width); self.line(x1,y0,x1,y1,width)
        self.line(x1,y1,x0,y1,width); self.line(x0,y1,x0,y0,width)

def cloud(c, yoff=0):
    s=c.w
    c.ellipse(.10*s,.38*s+yoff,.38*s,.66*s+yoff,2,180,315)
    c.ellipse(.26*s,.24*s+yoff,.64*s,.62*s+yoff,2,190,350)
    c.ellipse(.52*s,.34*s+yoff,.88*s,.66*s+yoff,2,205,360)
    c.line(.14*s,.58*s+yoff,.82*s,.58*s+yoff,3)

def sun(c):
    s=c.w; cx=cy=s/2; r=.22*s
    c.ellipse(cx-r,cy-r,cx+r,cy+r,2)
    for deg in range(0,360,45):
        a=math.radians(deg); c.line(cx+math.cos(a)*.34*s,cy+math.sin(a)*.34*s,
                                    cx+math.cos(a)*.46*s,cy+math.sin(a)*.46*s,2)

def partly(c):
    s=c.w; cx=.66*s; cy=.34*s; r=.14*s
    c.ellipse(cx-r,cy-r,cx+r,cy+r,2)
    for deg in range(0,360,45):
        a=math.radians(deg); c.line(cx+math.cos(a)*.21*s,cy+math.sin(a)*.21*s,
                                    cx+math.cos(a)*.27*s,cy+math.sin(a)*.27*s,1)
    cloud(c,.05*s)

def rain(c):
    cloud(c,-.06*c.w); s=c.w
    for x in (.30*s,.50*s,.70*s): c.line(x,.64*s,x-.05*s,.84*s,2)

def snow(c):
    cloud(c,-.06*c.w); s=c.w
    for x in (.30*s,.50*s,.70*s):
        y=.76*s
        c.line(x-3,y,x+3,y,1); c.line(x,y-3,x,y+3,1)
        c.line(x-2,y-2,x+2,y+2,1); c.line(x-2,y+2,x+2,y-2,1)

def thunder(c):
    cloud(c,-.08*c.w); s=c.w
    c.poly([(.53*s,.57*s),(.43*s,.73*s),(.53*s,.73*s),(.45*s,.91*s),(.67*s,.67*s),(.56*s,.67*s)],3)

def fog(c):
    cloud(c,-.14*c.w); s=c.w
    for y in (.63*s,.73*s,.83*s): c.line(.16*s,y,.84*s,y,2)

def robot(c):
    s=c.w; c.ellipse(.16*s,.20*s,.84*s,.80*s,2); c.line(.50*s,.08*s,.50*s,.20*s,2)
    c.ellipse(.46*s,.03*s,.54*s,.11*s,2)
    c.ellipse(.30*s,.38*s,.40*s,.48*s,2); c.ellipse(.60*s,.38*s,.70*s,.48*s,2)
    c.ellipse(.34*s,.43*s,.66*s,.66*s,2,20,160)

def mic(c):
    s=c.w; c.ellipse(.36*s,.12*s,.64*s,.60*s,2); c.line(.36*s,.35*s,.36*s,.47*s,2); c.line(.64*s,.35*s,.64*s,.47*s,2)
    c.ellipse(.24*s,.32*s,.76*s,.78*s,2,0,180); c.line(.50*s,.75*s,.50*s,.90*s,2); c.line(.34*s,.90*s,.66*s,.90*s,2)

def location(c):
    s=c.w; c.ellipse(.27*s,.10*s,.73*s,.58*s,2); c.ellipse(.43*s,.26*s,.57*s,.40*s,2)
    c.poly([(.29*s,.47*s),(.50*s,.90*s),(.71*s,.47*s)],2)

def todo(c):
    s=c.w; c.rect(.14*s,.22*s,.86*s,.84*s,2); c.line(.14*s,.37*s,.86*s,.37*s,2)
    c.line(.32*s,.10*s,.32*s,.29*s,2); c.line(.68*s,.10*s,.68*s,.29*s,2)
    for y in (.50*s,.66*s): c.poly([(.24*s,y),(.33*s,y+.07*s),(.45*s,y-.06*s)],2)

def car(c):
    s=c.w; c.poly([(.15*s,.60*s),(.26*s,.42*s),(.40*s,.34*s),(.68*s,.34*s),(.82*s,.53*s)],2)
    c.rect(.12*s,.52*s,.88*s,.72*s,2); c.ellipse(.20*s,.65*s,.36*s,.81*s,2); c.ellipse(.64*s,.65*s,.80*s,.81*s,2)

def parcel(c):
    s=c.w; c.rect(.18*s,.28*s,.82*s,.80*s,2); c.poly([(.18*s,.28*s),(.50*s,.12*s),(.82*s,.28*s)],2)
    c.line(.50*s,.12*s,.50*s,.80*s,2); c.poly([(.18*s,.28*s),(.50*s,.44*s),(.82*s,.28*s)],2)

def home(c):
    s=c.w; c.poly([(.10*s,.47*s),(.50*s,.13*s),(.90*s,.47*s)],2); c.rect(.23*s,.44*s,.77*s,.84*s,2); c.rect(.45*s,.61*s,.59*s,.84*s,2)

def market(c):
    s=c.w; c.line(.14*s,.82*s,.14*s,.18*s,2); c.line(.14*s,.82*s,.88*s,.82*s,2)
    c.poly([(.21*s,.69*s),(.38*s,.53*s),(.51*s,.60*s),(.68*s,.36*s),(.84*s,.26*s)],3); c.poly([(.73*s,.26*s),(.84*s,.26*s),(.84*s,.37*s)],2)

def book(c):
    s=c.w; c.poly([(.11*s,.22*s),(.36*s,.18*s),(.50*s,.29*s),(.64*s,.18*s),(.89*s,.22*s)],2)
    c.poly([(.11*s,.22*s),(.11*s,.78*s),(.38*s,.74*s),(.50*s,.86*s)],2)
    c.poly([(.89*s,.22*s),(.89*s,.78*s),(.62*s,.74*s),(.50*s,.86*s)],2); c.line(.50*s,.29*s,.50*s,.86*s,2)

ICON_DEFS = [
    ("ep_icon_weather_sun_40",40,sun), ("ep_icon_weather_partly_40",40,partly),
    ("ep_icon_weather_cloud_40",40,lambda c:cloud(c)), ("ep_icon_weather_rain_40",40,rain),
    ("ep_icon_weather_snow_40",40,snow), ("ep_icon_weather_thunder_40",40,thunder),
    ("ep_icon_weather_fog_40",40,fog), ("ep_icon_robot_24",24,robot),
    ("ep_icon_mic_20",20,mic), ("ep_icon_location_20",20,location),
    ("ep_icon_todo_20",20,todo), ("ep_icon_commute_20",20,car),
    ("ep_icon_parcel_20",20,parcel), ("ep_icon_home_20",20,home),
    ("ep_icon_market_20",20,market), ("ep_icon_word_20",20,book),
]

def pack_i1(c):
    stride=(c.w+7)//8
    data=[0xff,0xff,0xff,0x00, 0x00,0x00,0x00,0xff]  # transparent + black BGRA palette
    for row in c.p:
        packed=[0]*stride
        for x,v in enumerate(row):
            if v: packed[x//8] |= 0x80 >> (x&7)
        data.extend(packed)
    return stride,data

def generate_icons():
    out=['''#pragma once\n#include <lvgl.h>\n\n/* Generated in the same LVGL-v9 I1 C-array format used by scripts/Image_Converter/LVGLImage.py. */\n#ifndef LV_ATTRIBUTE_MEM_ALIGN\n#define LV_ATTRIBUTE_MEM_ALIGN\n#endif\n\n''']
    for name,size,fn in ICON_DEFS:
        c=Canvas(size,size); fn(c); stride,data=pack_i1(c)
        out.append(f"static const LV_ATTRIBUTE_MEM_ALIGN LV_ATTRIBUTE_LARGE_CONST uint8_t {name}_map[] = {{\n")
        for i in range(0,len(data),16): out.append('    '+', '.join(f'0x{x:02x}' for x in data[i:i+16])+',\n')
        out.append(f'''}};\nstatic const lv_image_dsc_t {name} = {{\n    .header.magic = LV_IMAGE_HEADER_MAGIC, .header.cf = LV_COLOR_FORMAT_I1,\n    .header.flags = 0, .header.w = {size}, .header.h = {size}, .header.stride = {stride},\n    .data_size = sizeof({name}_map), .data = {name}_map,\n}};\n\n''')
    ICON.write_text(''.join(out),encoding='utf-8')

def replace_once(text, old, new, label):
    if old not in text:
        if new in text: return text
        raise SystemExit(f"patch anchor missing: {label}")
    return text.replace(old,new,1)

def patch_cmake():
    t=CMAKE.read_text(encoding='utf-8')
    old='''elseif(CONFIG_BOARD_TYPE_BREAD_COMPACT_ESP32)\n    set(BOARD_DIR "bread-compact-esp32")'''
    new='''elseif(CONFIG_BOARD_TYPE_BREAD_COMPACT_ESP32)\n    set(BOARD_DIR "bread-compact-esp32")\n    # 16px/4bpp improves Chinese stroke definition on the 800x480 e-paper.\n    set(BUILTIN_TEXT_FONT font_noto_sans_basic_16_4)\n    set(BUILTIN_ICON_FONT font_material_symbols_16_4)'''
    t=replace_once(t,old,new,'bread compact font')
    CMAKE.write_text(t,encoding='utf-8')

def patch_source():
    t=SRC.read_text(encoding='utf-8')
    t=replace_once(t,'#include "config.h"\n#include "lunar_calendar.h"','#include "config.h"\n#include "epaper_dashboard_icons.h"\n#include "lunar_calendar.h"','icon include')
    t=t.replace('return luminance < (32u * 64u);','return luminance < (42u * 64u); // thicken 4bpp anti-aliased text for 1-bit e-paper')

    anchor='''void StyleSolidBlack(lv_obj_t* obj) {\n    lv_obj_set_style_bg_color(obj, lv_color_black(), 0);\n    lv_obj_set_style_bg_opa(obj, LV_OPA_COVER, 0);\n    lv_obj_set_style_border_width(obj, 0, 0);\n    lv_obj_set_style_pad_all(obj, 0, 0);\n    lv_obj_clear_flag(obj, LV_OBJ_FLAG_SCROLLABLE);\n}\n'''
    helper=anchor+'''\nlv_obj_t* CreateBitmapIcon(lv_obj_t* parent, int x, int y, const lv_image_dsc_t* src) {\n    lv_obj_t* img = lv_image_create(parent);\n    lv_image_set_src(img, src);\n    lv_obj_set_pos(img, x, y);\n    lv_obj_clear_flag(img, LV_OBJ_FLAG_SCROLLABLE);\n    return img;\n}\n\nconst lv_image_dsc_t* WeatherBitmap(int code) {\n    if (code == 0) return &ep_icon_weather_sun_40;\n    if (code == 1 || code == 2) return &ep_icon_weather_partly_40;\n    if (code == 3) return &ep_icon_weather_cloud_40;\n    if (code == 45 || code == 48) return &ep_icon_weather_fog_40;\n    if ((code >= 51 && code <= 67) || (code >= 80 && code <= 82)) return &ep_icon_weather_rain_40;\n    if ((code >= 71 && code <= 77) || code == 85 || code == 86) return &ep_icon_weather_snow_40;\n    if (code >= 95) return &ep_icon_weather_thunder_40;\n    return &ep_icon_weather_cloud_40;\n}\n'''
    if 'const lv_image_dsc_t* WeatherBitmap' not in t: t=replace_once(t,anchor,helper,'bitmap helper')

    # Header and module pictograms.
    repl={
      'CreateSymbolLabel(screen, 354, 18, 20, LV_SYMBOL_AUDIO);':'CreateBitmapIcon(screen, 352, 14, &ep_icon_robot_24);',
      'CreateSymbolLabel(screen, 18, 148, 20, LV_SYMBOL_GPS);':'CreateBitmapIcon(screen, 18, 146, &ep_icon_location_20);',
      'CreateSymbolLabel(screen, 362, 149, 20, LV_SYMBOL_BELL);':'CreateBitmapIcon(screen, 360, 146, &ep_icon_todo_20);',
      'CreateSymbolLabel(screen, 548, 149, 20, LV_SYMBOL_DRIVE);':'CreateBitmapIcon(screen, 548, 146, &ep_icon_commute_20);',
      'CreateSymbolLabel(screen, 548, 186, 20, LV_SYMBOL_ENVELOPE);':'CreateBitmapIcon(screen, 548, 183, &ep_icon_parcel_20);',
      'CreateSymbolLabel(screen, 548, 223, 20, LV_SYMBOL_HOME);':'CreateBitmapIcon(screen, 548, 220, &ep_icon_home_20);',
      'CreateSymbolLabel(screen, 548, 260, 20, LV_SYMBOL_BARS);':'CreateBitmapIcon(screen, 548, 257, &ep_icon_market_20);',
      'CreateSymbolLabel(screen, 18, 316, 20, LV_SYMBOL_EDIT);':'CreateBitmapIcon(screen, 18, 313, &ep_icon_word_20);',
      'CreateSymbolLabel(screen, 310, 352, 20, LV_SYMBOL_AUDIO);':'CreateBitmapIcon(screen, 310, 349, &ep_icon_mic_20);',
      'CreateSymbolLabel(screen, 310, 450, 20, LV_SYMBOL_AUDIO);':'CreateBitmapIcon(screen, 310, 447, &ep_icon_mic_20);',
    }
    for a,b in repl.items(): t=t.replace(a,b)

    old='''    const int badge_x[4] = {30, 108, 186, 264};\n    for (int i = 0; i < 4; ++i) {\n        weather_icon_labels_[i] = CreateWeatherBadge(screen, badge_x[i], 174, 34, "云");\n    }\n    weather_current_label_ = CreateLabel(screen, 10, 214, 74, "28° 多云", LV_TEXT_ALIGN_CENTER);\n    weather_aqi_label_ = CreateLabel(screen, 10, 263, 74, "AQI 52优", LV_TEXT_ALIGN_CENTER);\n    weather_forecast_labels_[0] = CreateLabel(screen, 87, 214, 75, "周二\\n31°/24°", LV_TEXT_ALIGN_CENTER);\n    weather_forecast_labels_[1] = CreateLabel(screen, 165, 214, 75, "周三\\n28°/23°", LV_TEXT_ALIGN_CENTER);\n    weather_forecast_labels_[2] = CreateLabel(screen, 243, 214, 82, "周四\\n27°/22°", LV_TEXT_ALIGN_CENTER);'''
    new='''    // High-fidelity weather composition: current conditions on the left,\n    // three compact forecast columns on the right, all using real I1 bitmaps.\n    weather_icon_labels_[0] = CreateBitmapIcon(screen, 18, 181, &ep_icon_weather_partly_40);\n    weather_current_label_ = CreateLabel(screen, 62, 178, 92, "今日 多云\\n30°/24°");\n    weather_aqi_label_ = CreateLabel(screen, 62, 230, 92, "AQI 52 优");\n    const int wx[3] = {158, 218, 278};\n    for (int i = 0; i < 3; ++i) {\n        weather_icon_labels_[i + 1] = CreateBitmapIcon(screen, wx[i], 179, &ep_icon_weather_cloud_40);\n    }\n    weather_forecast_labels_[0] = CreateLabel(screen, 150, 224, 56, "周二\\n31°/24°", LV_TEXT_ALIGN_CENTER);\n    weather_forecast_labels_[1] = CreateLabel(screen, 210, 224, 56, "周三\\n28°/23°", LV_TEXT_ALIGN_CENTER);\n    weather_forecast_labels_[2] = CreateLabel(screen, 270, 224, 60, "周四\\n27°/22°", LV_TEXT_ALIGN_CENTER);'''
    t=replace_once(t,old,new,'weather layout')

    old='''    lv_label_set_text(weather_icon_labels_[0],\n                      epaper_dashboard::DashboardDataProvider::WeatherGlyph(w.current_code));\n    for (int i = 0; i < 3; ++i) {\n        lv_label_set_text(weather_icon_labels_[i + 1],\n                          epaper_dashboard::DashboardDataProvider::WeatherGlyph(\n                              w.days[i + 1].weather_code));\n    }\n\n    std::snprintf(buf, sizeof(buf), "%d° %s",\n                  w.current_temp,\n                  epaper_dashboard::DashboardDataProvider::WeatherText(w.current_code));\n    lv_label_set_text(weather_current_label_, buf);\n\n    std::snprintf(buf, sizeof(buf), "AQI %d%s", w.aqi, w.aqi_grade.c_str());'''
    new='''    lv_image_set_src(weather_icon_labels_[0], WeatherBitmap(w.current_code));\n    for (int i = 0; i < 3; ++i) {\n        lv_image_set_src(weather_icon_labels_[i + 1], WeatherBitmap(w.days[i + 1].weather_code));\n    }\n\n    std::snprintf(buf, sizeof(buf), "今日 %s\\n%d°/%d°",\n                  epaper_dashboard::DashboardDataProvider::WeatherText(w.current_code),\n                  w.days[0].temp_max, w.days[0].temp_min);\n    lv_label_set_text(weather_current_label_, buf);\n\n    std::snprintf(buf, sizeof(buf), "AQI %d %s", w.aqi, w.aqi_grade.c_str());'''
    t=replace_once(t,old,new,'weather update')

    # Slightly more breathing room with the 16px/4bpp font.
    t=t.replace('lv_obj_set_style_text_align(label, align, 0);\n    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);',
                'lv_obj_set_style_text_align(label, align, 0);\n    lv_obj_set_style_text_line_space(label, 1, 0);\n    lv_obj_set_style_text_opa(label, LV_OPA_COVER, 0);\n    lv_obj_set_style_bg_opa(label, LV_OPA_TRANSP, 0);',1)
    SRC.write_text(t,encoding='utf-8')

def main():
    generate_icons(); patch_cmake(); patch_source()
    print('e-paper UI graphics/font upgrade applied')

if __name__ == '__main__': main()

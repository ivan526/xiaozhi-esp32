#ifndef BREAD_COMPACT_LUNAR_CALENDAR_H
#define BREAD_COMPACT_LUNAR_CALENDAR_H

#include <string>

namespace epaper_dashboard {

struct LunarDate {
    int year = 0;
    int month = 0;
    int day = 0;
    bool leap = false;
    bool valid = false;
};

LunarDate SolarToLunar(int year, int month, int day);
std::string FormatLunarDate(int year, int month, int day);

}  // namespace epaper_dashboard

#endif

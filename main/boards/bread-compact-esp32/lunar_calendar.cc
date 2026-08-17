#include "lunar_calendar.h"

#include <array>
#include <cstdint>

namespace epaper_dashboard {
namespace {

struct LunarYearInfo {
    int year;
    uint8_t cny_month;
    uint8_t cny_day;
    uint8_t leap_month;
    uint8_t leap_days;
    uint16_t month_30_bits;
};

constexpr LunarYearInfo kYears[] = {
    {2019, 2, 5, 0, 0, 0xA95},
    {2020, 1, 25, 4, 29, 0xA9D},
    {2021, 2, 12, 0, 0, 0x556},
    {2022, 2, 1, 0, 0, 0xAB5},
    {2023, 1, 22, 2, 29, 0xAD6},
    {2024, 2, 10, 0, 0, 0x6D2},
    {2025, 1, 29, 6, 29, 0x765},
    {2026, 2, 17, 0, 0, 0xEA5},
    {2027, 2, 7, 0, 0, 0xE4A},
    {2028, 1, 27, 5, 29, 0x656},
    {2029, 2, 13, 0, 0, 0xC9B},
    {2030, 2, 3, 0, 0, 0x55A},
    {2031, 1, 23, 3, 29, 0x56D},
    {2032, 2, 11, 0, 0, 0xB69},
    {2033, 1, 31, 11, 29, 0xF52},
    {2034, 2, 19, 0, 0, 0x752},
    {2035, 2, 8, 0, 0, 0xB25},
    {2036, 1, 28, 6, 30, 0xB0B},
    {2037, 2, 15, 0, 0, 0xA4B},
    {2038, 2, 4, 0, 0, 0x4AB},
    {2039, 1, 24, 5, 29, 0x2BB},
    {2040, 2, 12, 0, 0, 0x56D},
    {2041, 2, 1, 0, 0, 0xB69},
    {2042, 1, 22, 2, 29, 0xDAA},
    {2043, 2, 10, 0, 0, 0xD92},
    {2044, 1, 30, 7, 29, 0xEA5},
    {2045, 2, 17, 0, 0, 0xD25},
    {2046, 2, 6, 0, 0, 0xA4D},
    {2047, 1, 26, 5, 30, 0xA4D},
    {2048, 2, 14, 0, 0, 0x2B6},
    {2049, 2, 2, 0, 0, 0x5B5},
    {2050, 1, 23, 3, 30, 0x2D1},
};

int64_t DaysFromCivil(int y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<int64_t>(era) * 146097 + static_cast<int64_t>(doe) - 719468;
}

const LunarYearInfo* FindYear(int year) {
    for (const auto& info : kYears) {
        if (info.year == year) return &info;
    }
    return nullptr;
}

int MonthDays(const LunarYearInfo& info, int month) {
    if (month < 1 || month > 12) return 29;
    return (info.month_30_bits & (1u << (month - 1))) ? 30 : 29;
}

const char* MonthName(int month) {
    static constexpr const char* kNames[] = {
        "", "正月", "二月", "三月", "四月", "五月", "六月",
        "七月", "八月", "九月", "十月", "冬月", "腊月"
    };
    return (month >= 1 && month <= 12) ? kNames[month] : "";
}

std::string DayName(int day) {
    static constexpr const char* kDigit[] = {
        "", "一", "二", "三", "四", "五", "六", "七", "八", "九", "十"
    };
    if (day <= 0 || day > 30) return "";
    if (day <= 10) return std::string("初") + kDigit[day];
    if (day < 20) return std::string("十") + kDigit[day - 10];
    if (day == 20) return "二十";
    if (day < 30) return std::string("廿") + kDigit[day - 20];
    return "三十";
}

}  // namespace

LunarDate SolarToLunar(int year, int month, int day) {
    LunarDate result;
    const int64_t solar_days = DaysFromCivil(year, static_cast<unsigned>(month), static_cast<unsigned>(day));

    int lunar_year = year;
    const LunarYearInfo* info = FindYear(lunar_year);
    if (info == nullptr) return result;

    int64_t cny_days = DaysFromCivil(info->year, info->cny_month, info->cny_day);
    if (solar_days < cny_days) {
        --lunar_year;
        info = FindYear(lunar_year);
        if (info == nullptr) return result;
        cny_days = DaysFromCivil(info->year, info->cny_month, info->cny_day);
    }

    int64_t offset = solar_days - cny_days;
    if (offset < 0) return result;

    for (int lunar_month = 1; lunar_month <= 12; ++lunar_month) {
        const int normal_days = MonthDays(*info, lunar_month);
        if (offset < normal_days) {
            result.year = lunar_year;
            result.month = lunar_month;
            result.day = static_cast<int>(offset) + 1;
            result.leap = false;
            result.valid = true;
            return result;
        }
        offset -= normal_days;

        if (info->leap_month == lunar_month) {
            const int leap_days = info->leap_days == 30 ? 30 : 29;
            if (offset < leap_days) {
                result.year = lunar_year;
                result.month = lunar_month;
                result.day = static_cast<int>(offset) + 1;
                result.leap = true;
                result.valid = true;
                return result;
            }
            offset -= leap_days;
        }
    }

    return result;
}

std::string FormatLunarDate(int year, int month, int day) {
    const LunarDate lunar = SolarToLunar(year, month, day);
    if (!lunar.valid) return "农历日期";
    std::string text = "农历";
    if (lunar.leap) text += "闰";
    text += MonthName(lunar.month);
    text += DayName(lunar.day);
    return text;
}

}  // namespace epaper_dashboard

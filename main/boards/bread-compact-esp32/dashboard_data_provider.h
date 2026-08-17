#ifndef BREAD_COMPACT_DASHBOARD_DATA_PROVIDER_H
#define BREAD_COMPACT_DASHBOARD_DATA_PROVIDER_H

#include <array>
#include <ctime>
#include <string>

namespace epaper_dashboard {

struct ForecastDay {
    int weather_code = 2;
    int temp_max = 30;
    int temp_min = 24;
};

struct WeatherSnapshot {
    bool live = false;
    std::string city = "北京";
    int current_temp = 28;
    int current_code = 2;
    int aqi = 52;
    std::string aqi_grade = "优";
    std::array<ForecastDay, 4> days{};
    std::string updated = "--:--";
};

struct TodoItem {
    std::string time;
    std::string title;
    std::string detail;
};

struct QuickItem {
    std::string title;
    std::string line1;
    std::string line2;
};

struct WordItem {
    std::string word;
    std::string phonetic;
    std::string meaning;
    std::string example;
    std::string translation;
    int index = 1;
    int total = 20;
};

struct DashboardSnapshot {
    WeatherSnapshot weather;
    std::array<TodoItem, 3> todos;
    QuickItem commute;
    QuickItem parcel;
    QuickItem home;
    QuickItem market;
    WordItem word;
    bool custom_api_live = false;
};

struct DashboardConfig {
    // city/lat/lon are the safe fallback and can still be overridden through
    // NVS. When auto_location is enabled, weather first resolves the device's
    // approximate network location from its public IP.
    std::string city = "北京";
    std::string latitude = "39.9042";
    std::string longitude = "116.4074";
    std::string timezone = "CST-8";
    bool auto_location = true;
    std::string geolocation_url =
        "https://ipwho.is/?fields=success,city,latitude,longitude,country_code&lang=zh-CN";

    std::string custom_api_url;
    std::string custom_api_token;
    int weather_refresh_minutes = 30;
    int custom_api_refresh_minutes = 5;
    int word_rotate_minutes = 30;
    int full_refresh_minutes = 60;
};

class DashboardDataProvider {
public:
    DashboardDataProvider();

    const DashboardConfig& config() const { return config_; }

    void FillStaticDefaults(DashboardSnapshot& out) const;
    WordItem GetLocalWord(std::time_t now) const;
    bool FetchWeather(WeatherSnapshot& out) const;
    bool FetchCustomDashboard(DashboardSnapshot& in_out) const;

    static const char* WeatherText(int code);
    static const char* WeatherGlyph(int code);
    static std::string AqiGrade(int aqi);

private:
    DashboardConfig config_;

    // Runtime location cache. IP geolocation is approximate but lets a device
    // without GPS follow the city of the network it is currently using. The
    // configured Beijing coordinates remain the fallback if lookup is blocked.
    mutable std::string active_city_;
    mutable std::string active_latitude_;
    mutable std::string active_longitude_;
    mutable std::time_t next_location_refresh_ = 0;

    bool HttpGet(const std::string& url, std::string& body, bool use_custom_token) const;
    bool ResolveCurrentLocation() const;
};

}  // namespace epaper_dashboard

#endif

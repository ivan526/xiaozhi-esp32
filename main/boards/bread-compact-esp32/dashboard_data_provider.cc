#include "dashboard_data_provider.h"

#include "board.h"
#include "settings.h"

#include <cJSON.h>
#include <esp_log.h>
#include <http.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <ctime>

#define TAG "EpaperDashData"

namespace epaper_dashboard {
namespace {

struct StaticWord {
    const char* word;
    const char* phonetic;
    const char* meaning;
    const char* example;
    const char* translation;
};

constexpr StaticWord kWords[] = {
    {"abandon", "/əˈbændən/", "放弃；遗弃", "Don't abandon your plan.", "不要放弃你的计划。"},
    {"achieve", "/əˈtʃiːv/", "实现；达到", "Small steps achieve big goals.", "小步骤也能实现大目标。"},
    {"adapt", "/əˈdæpt/", "适应；改编", "We adapt to change.", "我们适应变化。"},
    {"benefit", "/ˈbenɪfɪt/", "益处；受益", "Exercise benefits health.", "运动有益健康。"},
    {"confirm", "/kənˈfɜːrm/", "确认；证实", "Please confirm the meeting.", "请确认会议安排。"},
    {"deliver", "/dɪˈlɪvər/", "交付；递送", "We deliver on time.", "我们按时交付。"},
    {"efficient", "/ɪˈfɪʃənt/", "高效的", "Use a more efficient method.", "使用更高效的方法。"},
    {"focus", "/ˈfoʊkəs/", "专注；焦点", "Focus on the key task.", "专注于关键任务。"},
    {"improve", "/ɪmˈpruːv/", "改善；提高", "Practice improves quality.", "练习能提高质量。"},
    {"insight", "/ˈɪnsaɪt/", "洞察；见解", "Data gives us insight.", "数据带来洞察。"},
    {"maintain", "/meɪnˈteɪn/", "保持；维护", "Maintain a steady pace.", "保持稳定节奏。"},
    {"priority", "/praɪˈɔːrəti/", "优先事项", "Safety is our priority.", "安全是我们的优先事项。"},
    {"reliable", "/rɪˈlaɪəbəl/", "可靠的", "We need reliable data.", "我们需要可靠的数据。"},
    {"schedule", "/ˈskedʒuːl/", "日程；安排", "Check today's schedule.", "查看今天的日程。"},
    {"support", "/səˈpɔːrt/", "支持；支撑", "The team supports the project.", "团队支撑这个项目。"},
    {"target", "/ˈtɑːrɡɪt/", "目标；靶标", "Set a clear target.", "设定清晰的目标。"},
    {"update", "/ˌʌpˈdeɪt/", "更新；最新信息", "The status will update soon.", "状态很快会更新。"},
    {"verify", "/ˈverɪfaɪ/", "核实；验证", "Verify the result first.", "先验证结果。"},
    {"workflow", "/ˈwɜːrkfloʊ/", "工作流程", "Simplify the workflow.", "简化工作流程。"},
    {"quality", "/ˈkwɑːləti/", "质量；品质", "Quality comes first.", "质量优先。"},
};

std::string JsonString(cJSON* object, const char* key, const std::string& fallback = "") {
    if (object == nullptr) return fallback;
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring != nullptr ? item->valuestring : fallback;
}

int JsonInt(cJSON* object, const char* key, int fallback) {
    if (object == nullptr) return fallback;
    cJSON* item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsNumber(item) ? item->valueint : fallback;
}

void ParseQuickItem(cJSON* root, const char* key, QuickItem& target) {
    cJSON* object = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!cJSON_IsObject(object)) return;
    target.title = JsonString(object, "title", target.title);
    target.line1 = JsonString(object, "line1", target.line1);
    target.line2 = JsonString(object, "line2", target.line2);
}

int ClampMinutes(int value, int fallback, int min_value, int max_value) {
    if (value < min_value || value > max_value) return fallback;
    return value;
}

}  // namespace

DashboardDataProvider::DashboardDataProvider() {
    Settings settings("epaper_dash", false);
    config_.city = settings.GetString("city", "北京");
    config_.latitude = settings.GetString("lat", "39.9042");
    config_.longitude = settings.GetString("lon", "116.4074");
    // China-only board profile. Keep the setting for backward compatibility,
    // but display code forces CST-8 so local calendar is always UTC+8.
    config_.timezone = settings.GetString("tz", "CST-8");
    config_.auto_location = settings.GetInt("auto_loc", 1) != 0;
    config_.geolocation_url = settings.GetString(
        "geo_url",
        "https://ipwho.is/?fields=success,city,latitude,longitude,country_code&lang=zh-CN");
    config_.custom_api_url = settings.GetString("api_url", "");
    config_.custom_api_token = settings.GetString("api_token", "");
    config_.weather_refresh_minutes = ClampMinutes(settings.GetInt("weather_min", 30), 30, 5, 240);
    config_.custom_api_refresh_minutes = ClampMinutes(settings.GetInt("api_min", 5), 5, 1, 240);
    config_.word_rotate_minutes = ClampMinutes(settings.GetInt("word_min", 30), 30, 5, 180);
    config_.full_refresh_minutes = ClampMinutes(settings.GetInt("full_min", 60), 60, 15, 360);

    active_city_ = config_.city;
    active_latitude_ = config_.latitude;
    active_longitude_ = config_.longitude;

    ESP_LOGI(TAG,
             "config fallback=%s(%s,%s) auto_location=%s weather=%dmin api=%s full=%dmin",
             config_.city.c_str(), config_.latitude.c_str(), config_.longitude.c_str(),
             config_.auto_location ? "on" : "off",
             config_.weather_refresh_minutes,
             config_.custom_api_url.empty() ? "static" : "configured",
             config_.full_refresh_minutes);
}

void DashboardDataProvider::FillStaticDefaults(DashboardSnapshot& out) const {
    out.weather.city = config_.city;
    out.weather.live = false;
    out.weather.current_temp = 28;
    out.weather.current_code = 2;
    out.weather.aqi = 52;
    out.weather.aqi_grade = "优";
    out.weather.days = {{{2, 30, 24}, {0, 31, 24}, {61, 28, 23}, {3, 27, 22}}};
    out.weather.updated = "静态";

    out.todos = {{{"09:30", "周会", "二楼会议室"},
                  {"14:00", "客户沟通", "3号会议室"},
                  {"17:30", "提交周报", "发送至项目群"}}};

    out.commute = {"通勤", "路况正常", "地铁正常"};
    out.parcel = {"快递", "1件运输中", "预计明天送达"};
    out.home = {"家居", "客厅空调已关闭", "室内 26°C"};
    out.market = {"A股", "沪深300 +0.6%", "4132.35  13:30"};
    out.word = GetLocalWord(std::time(nullptr));
    out.custom_api_live = false;
}

WordItem DashboardDataProvider::GetLocalWord(std::time_t now) const {
    struct tm tm_now = {};
    if (now <= 0 || localtime_r(&now, &tm_now) == nullptr || tm_now.tm_year + 1900 < 2020) {
        tm_now.tm_yday = 0;
        tm_now.tm_hour = 0;
        tm_now.tm_min = 0;
    }

    constexpr int kWordCount = static_cast<int>(sizeof(kWords) / sizeof(kWords[0]));
    const int rotate_minutes = std::max(5, config_.word_rotate_minutes);
    const int slot = ((tm_now.tm_hour * 60 + tm_now.tm_min) / rotate_minutes) % kWordCount;
    const int index = (slot + tm_now.tm_yday) % kWordCount;
    const auto& word = kWords[index];

    WordItem result;
    result.word = word.word;
    result.phonetic = word.phonetic;
    result.meaning = word.meaning;
    result.example = word.example;
    result.translation = word.translation;
    result.index = slot + 1;
    result.total = kWordCount;
    return result;
}

bool DashboardDataProvider::HttpGet(const std::string& url, std::string& body, bool use_custom_token) const {
    auto& board = Board::GetInstance();
    auto* network = board.GetNetwork();
    if (network == nullptr) return false;

    auto http = network->CreateHttp(0);
    if (!http) return false;
    http->SetHeader("Accept", "application/json");
    http->SetHeader("User-Agent", "xiaozhi-epaper-dashboard/1.1");
    if (use_custom_token && !config_.custom_api_token.empty()) {
        http->SetHeader("Authorization", "Bearer " + config_.custom_api_token);
    }

    if (!http->Open("GET", url)) {
        ESP_LOGW(TAG, "HTTP open failed error=0x%x", http->GetLastError());
        return false;
    }
    const int status = http->GetStatusCode();
    if (status != 200) {
        ESP_LOGW(TAG, "HTTP status=%d", status);
        http->Close();
        return false;
    }
    body = http->ReadAll();
    http->Close();
    return !body.empty();
}

bool DashboardDataProvider::ResolveCurrentLocation() const {
    if (!config_.auto_location) return true;

    const std::time_t now = std::time(nullptr);
    if (next_location_refresh_ > 0 && now > 0 && now < next_location_refresh_) {
        return true;
    }

    std::string body;
    if (!HttpGet(config_.geolocation_url, body, false)) {
        next_location_refresh_ = now > 0 ? now + 600 : 0;
        ESP_LOGW(TAG, "IP location lookup failed; keep fallback/cached %s", active_city_.c_str());
        return false;
    }

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) {
        next_location_refresh_ = now > 0 ? now + 600 : 0;
        return false;
    }

    cJSON* success = cJSON_GetObjectItemCaseSensitive(root, "success");
    cJSON* latitude = cJSON_GetObjectItemCaseSensitive(root, "latitude");
    cJSON* longitude = cJSON_GetObjectItemCaseSensitive(root, "longitude");
    cJSON* city = cJSON_GetObjectItemCaseSensitive(root, "city");

    const bool ok = cJSON_IsTrue(success) && cJSON_IsNumber(latitude) && cJSON_IsNumber(longitude);
    if (ok) {
        char lat[24];
        char lon[24];
        std::snprintf(lat, sizeof(lat), "%.6f", latitude->valuedouble);
        std::snprintf(lon, sizeof(lon), "%.6f", longitude->valuedouble);
        active_latitude_ = lat;
        active_longitude_ = lon;
        if (cJSON_IsString(city) && city->valuestring != nullptr && city->valuestring[0] != '\0') {
            active_city_ = city->valuestring;
        }
        // Public-IP location changes far less often than weather; six hours
        // avoids needless network traffic while still following a moved device.
        next_location_refresh_ = now > 0 ? now + 6 * 3600 : 0;
        ESP_LOGI(TAG, "auto location %s (%s,%s)",
                 active_city_.c_str(), active_latitude_.c_str(), active_longitude_.c_str());
    } else {
        next_location_refresh_ = now > 0 ? now + 600 : 0;
        ESP_LOGW(TAG, "IP location response invalid; keep %s", active_city_.c_str());
    }

    cJSON_Delete(root);
    return ok;
}

bool DashboardDataProvider::FetchWeather(WeatherSnapshot& out) const {
    // ESP32 has no GPS on this board. Approximate the device location from its
    // current public IP and fall back to the configured coordinates when the
    // geolocation service is unavailable.
    ResolveCurrentLocation();

    const std::string& latitude = active_latitude_.empty() ? config_.latitude : active_latitude_;
    const std::string& longitude = active_longitude_.empty() ? config_.longitude : active_longitude_;
    const std::string& city = active_city_.empty() ? config_.city : active_city_;

    char url[640];
    std::snprintf(url, sizeof(url),
        "https://api.open-meteo.com/v1/forecast?latitude=%s&longitude=%s"
        "&current=temperature_2m,weather_code"
        "&daily=weather_code,temperature_2m_max,temperature_2m_min"
        "&forecast_days=4&timezone=Asia%%2FShanghai",
        latitude.c_str(), longitude.c_str());

    std::string body;
    if (!HttpGet(url, body, false)) return false;
    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) return false;

    bool ok = false;
    cJSON* current = cJSON_GetObjectItemCaseSensitive(root, "current");
    cJSON* daily = cJSON_GetObjectItemCaseSensitive(root, "daily");
    cJSON* codes = daily ? cJSON_GetObjectItemCaseSensitive(daily, "weather_code") : nullptr;
    cJSON* maxes = daily ? cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_max") : nullptr;
    cJSON* mins = daily ? cJSON_GetObjectItemCaseSensitive(daily, "temperature_2m_min") : nullptr;

    if (cJSON_IsObject(current) && cJSON_IsArray(codes) && cJSON_IsArray(maxes) && cJSON_IsArray(mins)) {
        cJSON* temp = cJSON_GetObjectItemCaseSensitive(current, "temperature_2m");
        cJSON* current_code = cJSON_GetObjectItemCaseSensitive(current, "weather_code");
        if (cJSON_IsNumber(temp)) out.current_temp = static_cast<int>(std::lround(temp->valuedouble));
        if (cJSON_IsNumber(current_code)) out.current_code = current_code->valueint;
        for (int i = 0; i < 4; ++i) {
            cJSON* code = cJSON_GetArrayItem(codes, i);
            cJSON* high = cJSON_GetArrayItem(maxes, i);
            cJSON* low = cJSON_GetArrayItem(mins, i);
            if (cJSON_IsNumber(code)) out.days[i].weather_code = code->valueint;
            if (cJSON_IsNumber(high)) out.days[i].temp_max = static_cast<int>(std::lround(high->valuedouble));
            if (cJSON_IsNumber(low)) out.days[i].temp_min = static_cast<int>(std::lround(low->valuedouble));
        }
        ok = true;
    }
    cJSON_Delete(root);
    if (!ok) return false;

    std::snprintf(url, sizeof(url),
        "https://air-quality-api.open-meteo.com/v1/air-quality?latitude=%s&longitude=%s"
        "&current=us_aqi&timezone=Asia%%2FShanghai",
        latitude.c_str(), longitude.c_str());
    std::string aqi_body;
    if (HttpGet(url, aqi_body, false)) {
        cJSON* aqi_root = cJSON_Parse(aqi_body.c_str());
        if (aqi_root != nullptr) {
            cJSON* aqi_current = cJSON_GetObjectItemCaseSensitive(aqi_root, "current");
            cJSON* aqi = aqi_current ? cJSON_GetObjectItemCaseSensitive(aqi_current, "us_aqi") : nullptr;
            if (cJSON_IsNumber(aqi)) {
                out.aqi = static_cast<int>(std::lround(aqi->valuedouble));
                out.aqi_grade = AqiGrade(out.aqi);
            }
            cJSON_Delete(aqi_root);
        }
    }

    out.city = city;
    out.live = true;
    std::time_t updated_now = std::time(nullptr);
    struct tm local = {};
    if (updated_now > 0 && localtime_r(&updated_now, &local) != nullptr) {
        char updated[16];
        std::snprintf(updated, sizeof(updated), "%02d:%02d", local.tm_hour, local.tm_min);
        out.updated = updated;
    } else {
        out.updated = "在线";
    }
    ESP_LOGI(TAG, "weather updated %s (%s,%s) %dC aqi=%d",
             out.city.c_str(), latitude.c_str(), longitude.c_str(), out.current_temp, out.aqi);
    return true;
}

bool DashboardDataProvider::FetchCustomDashboard(DashboardSnapshot& in_out) const {
    if (config_.custom_api_url.empty()) return false;
    std::string body;
    if (!HttpGet(config_.custom_api_url, body, true)) return false;

    cJSON* root = cJSON_Parse(body.c_str());
    if (root == nullptr) return false;

    cJSON* word = cJSON_GetObjectItemCaseSensitive(root, "word");
    if (cJSON_IsObject(word)) {
        in_out.word.word = JsonString(word, "word", in_out.word.word);
        in_out.word.phonetic = JsonString(word, "phonetic", in_out.word.phonetic);
        in_out.word.meaning = JsonString(word, "meaning", in_out.word.meaning);
        in_out.word.example = JsonString(word, "example", in_out.word.example);
        in_out.word.translation = JsonString(word, "translation", in_out.word.translation);
        in_out.word.index = JsonInt(word, "index", in_out.word.index);
        in_out.word.total = JsonInt(word, "total", in_out.word.total);
    }

    cJSON* todos = cJSON_GetObjectItemCaseSensitive(root, "todos");
    if (cJSON_IsArray(todos)) {
        const int count = std::min(3, cJSON_GetArraySize(todos));
        for (int i = 0; i < count; ++i) {
            cJSON* item = cJSON_GetArrayItem(todos, i);
            if (!cJSON_IsObject(item)) continue;
            in_out.todos[i].time = JsonString(item, "time", in_out.todos[i].time);
            in_out.todos[i].title = JsonString(item, "title", in_out.todos[i].title);
            in_out.todos[i].detail = JsonString(item, "detail", in_out.todos[i].detail);
        }
    }

    ParseQuickItem(root, "commute", in_out.commute);
    ParseQuickItem(root, "parcel", in_out.parcel);
    ParseQuickItem(root, "home", in_out.home);
    ParseQuickItem(root, "market", in_out.market);
    in_out.custom_api_live = true;
    cJSON_Delete(root);
    return true;
}

const char* DashboardDataProvider::WeatherText(int code) {
    switch (code) {
        case 0: return "晴";
        case 1: case 2: return "多云";
        case 3: return "阴";
        case 45: case 48: return "雾";
        case 51: case 53: case 55: return "毛毛雨";
        case 56: case 57: case 66: case 67: return "冻雨";
        case 61: case 63: case 65: case 80: case 81: case 82: return "雨";
        case 71: case 73: case 75: case 77: case 85: case 86: return "雪";
        case 95: case 96: case 99: return "雷雨";
        default: return "多云";
    }
}

const char* DashboardDataProvider::WeatherGlyph(int code) {
    // One-character glyphs are deliberately used instead of bitmap assets: the
    // Chinese font already contains them, so we get icon-like weather badges
    // with essentially no extra image RAM.
    switch (code) {
        case 0: return "晴";
        case 45: case 48: return "雾";
        case 51: case 53: case 55:
        case 56: case 57: case 61: case 63: case 65:
        case 66: case 67: case 80: case 81: case 82: return "雨";
        case 71: case 73: case 75: case 77: case 85: case 86: return "雪";
        case 95: case 96: case 99: return "雷";
        case 3: return "阴";
        default: return "云";
    }
}

std::string DashboardDataProvider::AqiGrade(int aqi) {
    if (aqi <= 50) return "优";
    if (aqi <= 100) return "良";
    if (aqi <= 150) return "轻度";
    if (aqi <= 200) return "中度";
    if (aqi <= 300) return "重度";
    return "严重";
}

}  // namespace epaper_dashboard

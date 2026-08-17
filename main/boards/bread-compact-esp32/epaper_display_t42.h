#ifndef EPAPER_DISPLAY_T42_H
#define EPAPER_DISPLAY_T42_H

#include "config.h"
#include "dashboard_data_provider.h"
#include "lcd_display.h"

#include <driver/spi_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

class EpaperDisplayT42 : public LcdDisplay {
public:
    EpaperDisplayT42();
    ~EpaperDisplayT42();

    bool IsReady() const { return ready_; }

    void SetupUI() override;
    void SetStatus(const char* status) override;
    void ShowNotification(const char* notification, int duration_ms = 3000) override;
    void ShowNotification(const std::string& notification, int duration_ms = 3000) override;
    void SetEmotion(const char* emotion) override;
    void SetChatMessage(const char* role, const char* content) override;
    void ClearChatMessages() override;
    void UpdateStatusBar(bool update_all = false) override;
    void SetPowerSaveMode(bool on) override;

private:
    static constexpr int LVGL_BUFFER_ROWS = 4;
    static constexpr size_t MONO_LINE_BYTES = EPD_WIDTH / 8;

    enum RefreshMask : uint32_t {
        REFRESH_NONE    = 0,
        REFRESH_CLOCK   = 1u << 0,
        REFRESH_HEADER  = 1u << 1,
        REFRESH_WEATHER = 1u << 2,
        REFRESH_TODO    = 1u << 3,
        REFRESH_QUICK   = 1u << 4,
        REFRESH_WORD    = 1u << 5,
        REFRESH_CHAT    = 1u << 6,
        REFRESH_FULL    = 1u << 31,
    };

    spi_device_handle_t spi_ = nullptr;
    uint8_t* lvgl_buffer_ = nullptr;
    uint8_t* mono_line_ = nullptr;
    uint8_t* partial_buffer_ = nullptr;
    size_t partial_buffer_size_ = 0;
    size_t partial_row_bytes_ = 0;

    TaskHandle_t refresh_task_handle_ = nullptr;
    TaskHandle_t data_task_handle_ = nullptr;

    bool ready_ = false;
    bool panel_powered_ = false;
    bool full_refresh_done_ = false;
    uint32_t partial_refresh_count_ = 0;

    volatile bool speaking_ = false;
    volatile bool streaming_refresh_ = false;
    volatile bool capture_partial_ = false;
    volatile bool stream_error_ = false;
    lv_area_t capture_area_ = {0, 0, 0, 0};

    std::atomic<uint32_t> pending_refresh_mask_{REFRESH_NONE};

    epaper_dashboard::DashboardDataProvider data_provider_;
    epaper_dashboard::DashboardSnapshot dashboard_;

    lv_obj_t* status_label_ = nullptr;
    lv_obj_t* user_label_ = nullptr;
    lv_obj_t* assistant_label_ = nullptr;

    std::array<std::array<lv_obj_t*, 7>, 4> clock_segments_{};
    std::array<lv_obj_t*, 2> clock_colon_{};
    std::array<int, 4> clock_digits_{{-1, -1, -1, -1}};
    lv_obj_t* date_label_ = nullptr;
    lv_obj_t* lunar_label_ = nullptr;

    lv_obj_t* weather_title_label_ = nullptr;
    lv_obj_t* weather_current_label_ = nullptr;
    lv_obj_t* weather_aqi_label_ = nullptr;
    std::array<lv_obj_t*, 3> weather_forecast_labels_{};
    std::array<lv_obj_t*, 3> todo_labels_{};
    std::array<lv_obj_t*, 4> quick_labels_{};
    lv_obj_t* word_label_ = nullptr;
    lv_obj_t* phonetic_label_ = nullptr;
    lv_obj_t* meaning_label_ = nullptr;
    lv_obj_t* example_label_ = nullptr;
    lv_obj_t* word_footer_label_ = nullptr;

    std::string status_text_;
    std::string user_text_;
    std::string assistant_text_;

    int last_clock_minute_ = -1;
    int last_clock_yday_ = -1;
    int last_word_slot_ = -1;

    static void LvglFlushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* color_p);
    static void RefreshTaskEntry(void* arg);
    static void DataTaskEntry(void* arg);
    void RefreshTaskLoop();
    void DataTaskLoop();

    bool InitializeHardware();
    bool InitializeLvgl();
    void NotifyRefresh(uint32_t mask);

    void BuildDashboardUi(lv_obj_t* screen);
    lv_obj_t* CreateBox(lv_obj_t* screen, int x, int y, int w, int h);
    lv_obj_t* CreateLabel(lv_obj_t* screen, int x, int y, int w, const char* text,
                          lv_text_align_t align = LV_TEXT_ALIGN_LEFT);
    void CreateSevenSegmentClock(lv_obj_t* screen);
    void UpdateClockLocked(bool force);
    void SetClockDigit(int index, int digit);
    void UpdateWeatherLocked();
    void UpdateTodoLocked();
    void UpdateQuickLocked();
    void UpdateWordLocked();
    void UpdateChatLocked();
    void UpdateHeaderLocked();

    static std::string TruncateUtf8(const std::string& text, size_t max_bytes);
    static const char* WeekdayName(int tm_wday);

    bool StreamCurrentUiToPanel();
    bool StreamSolidPlane(uint8_t value);
    bool CaptureUiRegion(int x_start, int y_start, int x_end, int y_end);
    bool WriteCapturedPartialRegion();
    void ReleasePartialBuffer();

    esp_err_t SpiWrite(const uint8_t* data, size_t len);
    void SendCommand(uint8_t cmd);
    void SendData(uint8_t data);
    void HardwareReset();
    bool WaitBusyRelease(const char* reason, uint32_t timeout_ms);
    void ConfigurePanelBase();
    bool InitPanelFullRefresh();
    bool InitPanelPartialRefresh();
    bool SetPartialWindow(int x_start, int y_start, int x_end, int y_end);
    bool RefreshPanelFull();
    bool RefreshPanelPartial(uint32_t mask);
    bool RefreshPartialRegion(int x_start, int y_start, int x_end, int y_end, const char* name);
    void SleepPanel();
};

#endif // EPAPER_DISPLAY_T42_H

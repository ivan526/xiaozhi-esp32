#ifndef EPAPER_DISPLAY_T42_H
#define EPAPER_DISPLAY_T42_H

#include "config.h"
#include "lcd_display.h"

#include <driver/spi_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <string>

// Board-local GDEY075T7 / UC8179 driver for bread-compact-esp32.
// It keeps the historical T42 class/file name to minimize changes outside this board.
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
        REFRESH_NONE      = 0,
        REFRESH_STATUS    = 1u << 0,
        REFRESH_USER      = 1u << 1,
        REFRESH_ASSISTANT = 1u << 2,
        REFRESH_FULL      = 1u << 31,
    };

    spi_device_handle_t spi_ = nullptr;
    uint8_t* lvgl_buffer_ = nullptr;
    uint8_t* mono_line_ = nullptr;
    uint8_t* partial_buffer_ = nullptr;
    size_t partial_buffer_size_ = 0;
    TaskHandle_t refresh_task_handle_ = nullptr;

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

    lv_obj_t* title_label_ = nullptr;
    lv_obj_t* user_label_ = nullptr;
    lv_obj_t* assistant_label_ = nullptr;

    std::string status_text_;
    std::string user_text_;
    std::string assistant_text_;

    static void LvglFlushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* color_p);
    static void RefreshTaskEntry(void* arg);
    void RefreshTaskLoop();

    bool InitializeHardware();
    bool InitializeLvgl();
    void NotifyRefresh(uint32_t mask);

    bool StreamCurrentUiToPanel();
    bool StreamSolidPlane(uint8_t value);
    bool CaptureUiRegion(int y_start, int y_end);
    bool WriteCapturedPartialRegion(int y_start, int y_end);
    void ReleasePartialBuffer();

    void UpdateUserLabelLocked();
    void UpdateAssistantLabelLocked();
    static std::string TruncateUtf8(const std::string& text, size_t max_bytes);

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
    bool RefreshPartialRegion(int y_start, int y_end, const char* name);
    void SleepPanel();
};

#endif // EPAPER_DISPLAY_T42_H

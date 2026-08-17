#ifndef EPAPER_DISPLAY_T42_H
#define EPAPER_DISPLAY_T42_H

#include "config.h"
#include "lcd_display.h"

#include <driver/spi_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

#include <cstddef>
#include <cstdint>
#include <string>

// Kept under the historical EpaperDisplayT42 class/file name so this remains a
// board-local change. The low-level driver now targets GDEY075T7 / UC8179.
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
    // 800 x 4 rows x RGB565 = 6,400 bytes. Keep the display footprint small so
    // the classic ESP32 still has enough contiguous SRAM for Opus/audio.
    static constexpr int LVGL_BUFFER_ROWS = 4;
    static constexpr size_t MONO_LINE_BYTES = EPD_WIDTH / 8;

    spi_device_handle_t spi_ = nullptr;
    uint8_t* lvgl_buffer_ = nullptr;
    uint8_t* mono_line_ = nullptr;
    TaskHandle_t refresh_task_handle_ = nullptr;

    bool ready_ = false;
    bool panel_powered_ = false;
    volatile bool speaking_ = false;
    volatile bool streaming_refresh_ = false;
    volatile bool stream_invert_ = false;
    volatile bool stream_error_ = false;

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
    void NotifyRefresh();
    bool StreamCurrentUiToPanel(bool invert);
    bool StreamSolidPlane(uint8_t value);

    void UpdateUserLabelLocked();
    void UpdateAssistantLabelLocked();
    static std::string TruncateUtf8(const std::string& text, size_t max_bytes);

    esp_err_t SpiWrite(const uint8_t* data, size_t len);
    void SendCommand(uint8_t cmd);
    void SendData(uint8_t data);

    void HardwareReset();
    bool WaitBusyRelease(const char* reason, uint32_t timeout_ms);
    bool InitPanelFullRefresh();
    bool RefreshPanelFull();
    void SleepPanel();
};

#endif // EPAPER_DISPLAY_T42_H

#ifndef EPAPER_DISPLAY_T42_H
#define EPAPER_DISPLAY_T42_H

#include "config.h"
#include "lcd_display.h"

#include <driver/spi_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <lvgl.h>

class EpaperDisplayT42 : public LcdDisplay {
public:
    EpaperDisplayT42();
    ~EpaperDisplayT42();

    bool IsReady() const { return ready_; }

private:
    // Keep only a tiny LVGL strip buffer. The previous implementation kept an
    // additional 800x480x1bpp framebuffer (48,000 bytes) in internal SRAM,
    // which left too little contiguous heap for Xiaozhi's Opus decoder.
    static constexpr int LVGL_BUFFER_ROWS = 4;
    static constexpr size_t MONO_LINE_BYTES = EPD_WIDTH / 8;

    spi_device_handle_t spi_ = nullptr;
    uint8_t* lvgl_buffer_ = nullptr;
    uint8_t* mono_line_ = nullptr;
    TaskHandle_t refresh_task_handle_ = nullptr;

    bool ready_ = false;
    bool first_refresh_ = true;
    bool power_rail_on_ = false;
    volatile bool streaming_refresh_ = false;
    volatile bool stream_error_ = false;

    static void LvglFlushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* color_p);
    static void RefreshTaskEntry(void* arg);
    void RefreshTaskLoop();

    bool InitializeHardware();
    bool InitializeLvgl();
    void NotifyRefresh();
    bool StreamCurrentUiToPanel();

    esp_err_t SpiWrite(const uint8_t* data, size_t len);
    void SendCommand(uint8_t cmd);
    void SendData(uint8_t data);
    void SendRepeated(uint8_t value, size_t len);

    void PowerRail(bool on);
    void HardwareReset();
    bool WaitBusy(const char* reason, uint32_t timeout_ms);
    bool InitPanelFullRefresh();
    bool RefreshPanelFull();
    void SleepAndPowerOff();
};

#endif // EPAPER_DISPLAY_T42_H

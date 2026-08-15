#ifndef EPAPER_DISPLAY_T42_H
#define EPAPER_DISPLAY_T42_H

#include "config.h"
#include "lcd_display.h"

#include <driver/spi_master.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>
#include <freertos/task.h>
#include <lvgl.h>

class EpaperDisplayT42 : public LcdDisplay {
public:
    EpaperDisplayT42();
    ~EpaperDisplayT42();

    bool IsReady() const { return ready_; }

private:
    static constexpr size_t FRAME_BYTES = EPD_WIDTH * EPD_HEIGHT / 8;
    static constexpr int LVGL_BUFFER_ROWS = 8;

    spi_device_handle_t spi_ = nullptr;
    uint8_t* framebuffer_ = nullptr;
    uint8_t* lvgl_buffer_ = nullptr;
    SemaphoreHandle_t framebuffer_mutex_ = nullptr;
    TaskHandle_t refresh_task_handle_ = nullptr;

    bool ready_ = false;
    bool first_refresh_ = true;
    bool power_rail_on_ = false;

    static void LvglFlushCb(lv_display_t* disp, const lv_area_t* area, uint8_t* color_p);
    static void RefreshTaskEntry(void* arg);
    void RefreshTaskLoop();

    bool InitializeHardware();
    bool InitializeLvgl();

    void SetPixel(int x, int y, bool black);
    void NotifyRefresh();

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

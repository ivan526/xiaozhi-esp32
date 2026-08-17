#ifndef _BOARD_CONFIG_H_
#define _BOARD_CONFIG_H_

#include <driver/gpio.h>

#define AUDIO_INPUT_SAMPLE_RATE  16000
#define AUDIO_OUTPUT_SAMPLE_RATE 24000

// Keep the known-good Xiaozhi bread-compact ESP32 simplex audio pinout.
#define AUDIO_I2S_METHOD_SIMPLEX

#define AUDIO_I2S_MIC_GPIO_WS   GPIO_NUM_25
#define AUDIO_I2S_MIC_GPIO_SCK  GPIO_NUM_26
#define AUDIO_I2S_MIC_GPIO_DIN  GPIO_NUM_32

#define AUDIO_I2S_SPK_GPIO_DOUT GPIO_NUM_33
#define AUDIO_I2S_SPK_GPIO_BCLK GPIO_NUM_14
#define AUDIO_I2S_SPK_GPIO_LRCK GPIO_NUM_27

#define BOOT_BUTTON_GPIO        GPIO_NUM_0
// GPIO5 is reserved for e-paper CS on this board variant, so the original
// bread-compact touch button is intentionally not instantiated.
#define TOUCH_BUTTON_GPIO       GPIO_NUM_5
#define ASR_BUTTON_GPIO         GPIO_NUM_19
#define BUILTIN_LED_GPIO        GPIO_NUM_2

// Kept for compatibility with common board code. CompactWifiBoard does not use
// the ML307 UART; GPIO16/17 are reserved for the e-paper RST/DC signals below.
#define ML307_RX_PIN            GPIO_NUM_16
#define ML307_TX_PIN            GPIO_NUM_17

// Waveshare e-Paper Driver HAT Rev2.3 + 7.5" GDEY075T7 / UC8179 (800x480).
// Verified final wiring:
//   HAT VCC  -> ESP32 3V3
//   HAT PWR  -> ESP32 3V3 (always enabled; no GPIO power control)
//   HAT GND  -> ESP32 GND
//   HAT DIN  -> GPIO23
//   HAT CLK  -> GPIO18
//   HAT CS   -> GPIO5
//   HAT DC   -> GPIO17
//   HAT RST  -> GPIO16
//   HAT BUSY -> GPIO4
#define EPD_BUSY_PIN            GPIO_NUM_4
#define EPD_RST_PIN             GPIO_NUM_16
#define EPD_DC_PIN              GPIO_NUM_17
#define EPD_CS_PIN              GPIO_NUM_5
#define EPD_SCLK_PIN            GPIO_NUM_18
#define EPD_MOSI_PIN            GPIO_NUM_23

#define EPD_WIDTH               800
#define EPD_HEIGHT              480

// GxEPD2 uses 4 MHz by default for this class of displays. Keep the same
// conservative clock for jumper-wire / breadboard use.
#define EPD_SPI_CLOCK_HZ        (4 * 1000 * 1000)
#define EPD_REFRESH_DEBOUNCE_MS 1200
#define EPD_BUSY_TIMEOUT_MS     12000

#endif // _BOARD_CONFIG_H_

# bread-compact-esp32 + 7.5" e-Paper

本分支仅针对 `bread-compact-esp32` 板型接入 Waveshare e-Paper Driver HAT Rev2.3 与 GDEY075T7 / UC8179 800×480 黑白墨水屏。

## 最终接线

| HAT | ESP32 |
| --- | --- |
| VCC | 3V3 |
| PWR | 3V3 |
| GND | GND |
| DIN | GPIO23 |
| CLK | GPIO18 |
| CS | GPIO5 |
| DC | GPIO17 |
| RST | GPIO16 |
| BUSY | GPIO4 |

HAT 拨码保持：Display Config = B / 0.47R，Interface Config = 0 / 4-line SPI。

> GPIO5 已用于墨水屏 CS，因此该板型原有的 GPIO5 触摸按键在此分支中禁用。GPIO16/17 用于墨水屏 RST/DC，`CompactWifiBoard` 不使用 ML307 UART。

## 显示驱动

低层初始化与全刷流程按 `GxEPD2_750_GDEY075T7` / UC8179 时序移植，包括 5 字节 POWER SETTING、Booster Soft Start、800×480 分辨率、VCOM/Data Interval、PWS、内部温度传感器和全刷命令。BUSY 为低电平有效。

为降低经典 ESP32 的 SRAM 占用，UI 使用 LVGL 小行缓冲并直接流式转换为 1-bit 数据写入面板，不分配完整 800×480 RGB 帧缓存。TTS 讲话期间不反复全刷，结束后再刷新最终文本。

## 编译

```bash
idf.py set-target esp32
idf.py menuconfig
```

选择：

```text
Xiaozhi Assistant -> Board Type -> 面包板 ESP32 DevKit
```

或使用仓库构建脚本：

```bash
python scripts/build.py bread-compact-esp32 --name bread-compact-esp32-epaper-t42
```

GitHub Actions 会生成一个包含合并固件与刷机说明的 artifact。

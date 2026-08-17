# bread-compact-esp32 + 7.5" e-Paper 桌面屏

本分支仅针对 `bread-compact-esp32` 板型接入 Waveshare e-Paper Driver HAT Rev2.3 与 GDEY075T7 / UC8179 800×480 黑白墨水屏，并实现面向中国日常桌面使用的小智信息仪表盘。

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

> GPIO5 已用于墨水屏 CS，因此原 GPIO5 触摸按键禁用。GPIO16/17 用于 RST/DC。

## 桌面界面

800×480 页面划分为 7 个独立局刷区域：

- 左上：本地时钟、日期、星期、城市、农历。
- 右上：小智状态和语音能力提示。
- 中左：北京天气、AQI、未来 3 天预报。
- 中间：今日待办。
- 中右：通勤 / 快递 / 家居 / A股。
- 左下：每日记单词，默认 20 词、30 分钟轮播。
- 右下：小智最近一轮问答和非触摸交互提示。

屏幕不是触摸屏，交互提示统一为“说‘小智小智’唤醒 / 按键说话”。

## 本地时间与农历

时钟直接读取 ESP32 系统时间；小智联网后已有的时间同步负责校时。默认时区为 `CST-8`。

农历转换完全在设备本地完成，不调用第三方农历接口；当前内置 2019–2050 年转换表。

时钟只在“分钟发生变化”时触发局部刷新，不进行 1 Hz 屏幕刷新。

## 天气

天气方案参考 `VoIPshare/ESP32-eInk-Dashboard` 的 Open-Meteo 思路：

- 直接访问 Open-Meteo forecast API 获取当前天气和未来 4 天高低温。
- 直接访问 Open-Meteo air-quality API 获取 AQI。
- 默认地点：北京 `39.9042, 116.4074`。
- 默认 30 分钟更新一次。
- 请求失败时保留屏幕上的最后可用数据；启动阶段先显示静态兜底值。
- 网络请求运行在低优先级独立任务中，不阻塞小智音频和墨水屏刷新任务。

## 自定义 API 预留

英语单词、待办、通勤、快递、家居、A股统一预留一个自定义 Dashboard API。当前 `api_url` 为空，因此使用固件内静态数据；后续只需要配置 API 地址即可切换到联网数据。

NVS namespace：`epaper_dash`

| key | 默认值 | 用途 |
| --- | --- | --- |
| `city` | `北京` | 城市显示名 |
| `lat` | `39.9042` | 天气纬度 |
| `lon` | `116.4074` | 天气经度 |
| `tz` | `CST-8` | POSIX 时区 |
| `api_url` | 空 | 自定义 Dashboard API 完整 URL |
| `api_token` | 空 | 可选 Bearer Token |
| `weather_min` | `30` | 天气刷新分钟数 |
| `api_min` | `5` | 自定义 API 刷新分钟数 |
| `word_min` | `30` | 单词轮播分钟数 |
| `full_min` | `60` | 定时全刷分钟数 |

预留 API JSON：

```json
{
  "word": {
    "word": "abandon",
    "phonetic": "/əˈbændən/",
    "meaning": "放弃；遗弃",
    "example": "Don't abandon your plan.",
    "translation": "不要放弃你的计划。",
    "index": 6,
    "total": 20
  },
  "todos": [
    {"time": "09:30", "title": "周会", "detail": "二楼会议室"},
    {"time": "14:00", "title": "客户沟通", "detail": "3号会议室"},
    {"time": "17:30", "title": "提交周报", "detail": "发送至项目群"}
  ],
  "commute": {"title": "通勤", "line1": "路况正常", "line2": "地铁正常"},
  "parcel": {"title": "快递", "line1": "1件运输中", "line2": "预计明天送达"},
  "home": {"title": "家居", "line1": "客厅空调已关闭", "line2": "室内 26°C"},
  "market": {"title": "A股", "line1": "沪深300 +0.6%", "line2": "4132.35 13:30"}
}
```

## 刷新策略

局部刷新严格参考 Waveshare 官方 7.5" V2 Demo：

- `Init_Part`: `0x00/0x1F -> POWER ON -> 0xE0/0x02 -> 0xE5/0x6E`
- `0x91` 进入 partial mode
- `0x90` 设置 byte-aligned partial window
- `0x13` 写当前区域
- `0x12` 触发刷新

当前实现从之前的“整行局刷”升级为真正的矩形窗口局刷。每个模块只传输自己的像素区域，显著减少 LVGL 渲染量和 SPI 数据量。

性能策略：

1. UI 启动先用静态数据立即绘制，不等待网络。
2. 时钟每分钟局刷一次。
3. 天气成功更新只刷新天气块。
4. 单词轮播只刷新单词块。
5. 待办/通勤/快递/家居/A股只在自定义 API 数据更新时刷新对应区域。
6. 小智 TTS 流式回复期间不随 token 刷屏，讲话结束后一次刷新问答区域。
7. 约 220ms 合并短时间连续更新。
8. 默认每 60 分钟执行一次全量刷新用于清理残影。
9. 连续局刷达到安全阈值也会提前触发全刷。
10. 局刷失败自动回退全刷。

## 编译

```bash
python scripts/build.py bread-compact-esp32 --name bread-compact-esp32-epaper-t42
```

GitHub Actions 会生成合并固件 artifact，可从地址 `0x0` 烧录。

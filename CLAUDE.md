# esl-helper

ESP32-C3-Zero (Waveshare) + 微雪 4.2 寸墨水屏(400x300 黑白,背部标 "V2")。macOS + VS Code + PlatformIO,板子配置 `esp32-c3-devkitm-1`。

本文件是从一次完整排障中沉淀的结论,给后续 session 直接复用,**不要再踩这些坑**。

## 接线(已确认可用,勿改)

| 墨水屏 | ESP32-C3-Zero |
|---|---|
| VCC | **3V3**(严禁 5V) |
| GND | GND |
| DIN (MOSI) | GPIO 6 |
| CLK (SCK) | GPIO 4 |
| CS | GPIO 5 |
| DC | GPIO 3 |
| RST | GPIO 7 |
| BUSY | GPIO 1 |

- GPIO4/6 恰好是 ESP32-C3 硬件 SPI 默认 SCK/MOSI,官方 `SPI.begin()` 无需映射。
- 刷新瞬间电流大,USB 供电要足(直连电脑,别用劣质拓展坞)。

## 三个关键坑(找回来花了最久)

### 1. 板上 RGB LED 是 WS2812,在 GPIO 10(不是交接文档写的 GPIO8)
- 必须用 Adafruit NeoPixel 库(RMT 协议),`digitalWrite` 点不亮。
- WS2812 锁存最后一帧颜色:看到"红色常亮"只是残留帧,**不等于代码在跑**。判断代码是否活着要用串口,不是 LED。

### 2. ESP32-C3 的 `Serial` 默认不经过 USB —— 这是"没串口输出"的根因
必须在 `platformio.ini` 加:
```ini
build_flags =
	-D ARDUINO_USB_MODE=1
	-D ARDUINO_USB_CDC_ON_BOOT=1
```
没有这两行,`Serial.println` 全部石沉大海,看起来像"板子没跑"。

### 3. 墨水屏乱图的根因:驱动和面板不匹配(不是接线/供电)
- 面板控制器是 SSD1683 家族,但**同家族每块面板初始化序列不通用**。
- GxEPD2 的 `GDEY042T81`/`GYE042A87` 都是给别的面板写的,全试过 → 乱图。
- **结论:GxEPD2 放弃,改用微雪官方 `epd4in2_V2` 驱动**(已复制到 `src/`,见下)。
- 误导点:BUSY 时序完美(1.7s)≠ 画面正确——控制器按错误波形"认真"刷完了屏,输出却是乱像素。

## 当前可用驱动(勿再用 GxEPD2)

`src/epd4in2_V2.{h,cpp}` + `src/epdif.{h,cpp}` + `src/imagedata.*`,复制自
`waveshareteam/e-Paper` 仓库 `Arduino/epd4in2_V2/`。

- 引脚改过了:`epdif.h` 里 `RST=7 / DC=3 / CS=5 / BUSY=1`。
- ESP32 arduino core 3.x 没有 `<avr/pgmspace.h>` 兼容头,已改成 `<pgmspace.h>`(再遇到别改回去)。
- 用法:构造 `Epd epd;` → `epd.Init()` → 写 15000 字节缓冲(`400/8*300`,每字节 MSB 在左,1=白 0=黑)→ `epd.Display(buf)` / `epd.Clear()`(全白)。
- 官方序列要点:复位 `HIGH100ms→LOW2ms→HIGH100ms`;边框波形 `0x3C=0x05`;全刷 `0x22/0xF7`;BUSY 高电平忙。
- 扑灰阶 `Init_4Gray`/`LUT_ALL` 未用,`imagedata.*` 只为它保留,可删(`Lut()` 引用 `LUT_ALL`,删代码时连着一起删)。

### 4. WiFi 连不上:先查频段,不是密码
- **ESP32-C3 只支持 2.4GHz**。`WiFi.status()` 返回 `1`(`WL_NO_SSID_AVAIL`)= 扫描时没看见这个 SSID,几乎总是频段问题(SSID 带 `-5G`/`_5G` 后缀的基本都是)。
- 本项目现场:办公区 `KB-Guest-5G` 是 5GHz 专用,板子扫不到;改用 **`KB-Guest-2.4G`** 即通。
- **诊断手法:启动时先 `WiFi.scanNetworks()` 把可见网络打出来**(SSID / 信道 / RSSI)。信道 >= 36 是 5GHz;板子只会列出 2.4GHz 的(1–13)。一眼就能看出该连哪个。
- 坑:**不要在循环里 `WiFi.mode(WIFI_OFF)`**。会把 WiFi 栈切坏,下一轮 `WiFi.mode(WIFI_STA)` 报 `Could not set mode! 12289` / `STA enable failed`,且 `scanNetworks()` 返回 `-2`(`WIFI_SCAN_FAILED`)。常供电场景根本不需要关。连接失败后要重新扫描,也得先 `WiFi.disconnect()` 让栈回 idle。

## 工具链备注

- `pio` 二进制在 `~/.platformio/penv/bin/pio`,不在 PATH。
- 烧录口(当前):`upload_port = /dev/cu.usbmodem1101`,`upload_speed = 460800`。
- 抓串口(pyserial 未装时):
  ```python
  import os, termios
  fd = os.open('/dev/cu.usbmodem1101', os.O_RDWR | os.O_NONBLOCK)
  a = termios.tcgetattr(fd); a[4] = a[5] = termios.B115200
  termios.tcsetattr(fd, termios.TCSANOW, a)   # 注意 macOS:attrs[4/5] 是 int
  ```
- `pio device monitor` 拒绝非交互 stdin,抓板端输出用上面的 Python 方式。

## 当前场景:联网天气屏(已跑通)

`main.cpp` = 连 WiFi → HTTP 拉 Open-Meteo → 解析 → 排版 → 全刷 → 睡 10 分钟循环。

- WiFi 是**多网络表** `WIFI_LIST[]`(`main.cpp` 顶部),按顺序试、连上就停。加网络加一行即可。
- 数据源 **Open-Meteo**:免费、无需 API key。URL 写在 `main.cpp` 顶部 `URL` 常量。
- 城市**上海**(`latitude=31.23&longitude=121.47`),换城市改这两个数。
- 日期直接解析 API 返回的 `current.time` 时间戳,**不需要 NTP**。
- 紫外线取自 `hourly.uv_index[]`,按当前小时前缀匹配(`strncmp(th, iso, 13)`)。
- 空气质量走 **Open-Meteo Air Quality API**(`air-quality-api.open-meteo.com`,同样免 key):取 `current=us_aqi,pm2_5`。US AQI 不是国标 AQI,但最通用;失败只显示 `AQI --`,不影响天气。
- 每个刷新周期发 **2 个 HTTP 请求**(天气 + 空气质量)。
- HTTP 用 ESP32 core 自带的 `HTTPClient`——**它自动解 chunked**(Open-Meteo 返回的就是 chunked,别自己写解包)。
- 排版用官方 demo 的 `epdpaint` + `fonts`(ASCII 字库,`Font24`/`Font16`)。**字库无 `°`,温度写 `C`**;无中文字库(塞不下),天气描述用英文短词。
- 颜色语义:`epdpaint.cpp` 里 `IF_INVERT_COLOR=1`,故 **`BLACK=0`、`WHITE=1`**(传 0 清位=黑)。这与缓冲里 1=白 一致。
- 依赖:`bblanchon/ArduinoJson @ ^7.3.1`。

## src/ 当前内容

- `main.cpp` — 天气屏场景(见上)。
- `epd4in2_V2.*` / `epdif.*` / `imagedata.*` — 微雪官方驱动。
- `epdpaint.*` / `fonts.h` / `font{8,12,16,20,24}.cpp` — 官方绘图板 + ASCII 字库。
- 以上全部来自 `waveshareteam/e-Paper` 的 `Arduino/epd4in2_V2/`,已改:引脚、`pgmspace`。

## 待办 / 可做

- 中文字库(需自建小范围汉字位图,官方 ASCII 字库不支持中文)。
- `imagedata.*` 只为 `Init_4Gray` 的 `LUT_ALL` 保留,不用灰阶可删。
- 电池供电 + deep sleep(当前 10 分钟常供电循环)。
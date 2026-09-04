# ESP32 4-Mic Array Collector

XIAO ESP32-S3 + reSpeaker XVF3800 录音终端。工程基于 ESP-IDF 6（已在 6.0.2 构建验证），包含：

- 始终启用的 `Bestlink-xxxxxx` SoftAP（`192.168.100.1`）和分页配置页面；
- NVS 持久化、Wi-Fi 扫描和保存前连接验证；
- MQTT `start` / `stop` / `status` / `set` 控制面；
- XVF3800 I2C 版本、LED 参数读取和设置；
- I2S 采集任务、Audio Ring Buffer、WebSocket 发送任务及自动重连。

## 硬件和音频格式

| 信号 | ESP32-S3 GPIO |
|---|---:|
| I2C SDA / SCL | 5 / 6 |
| I2S BCLK / WS | 8 / 7 |
| I2S DOUT / DIN | 44 / 43 |

工程默认匹配 Seeed 的录音/回放例程：ESP32-S3 作为 I2S master 输出 BCLK/WS，采样格式为 16 kHz、stereo、32-bit、Philips I2S。采集时取左声道高 16 bit，向 WebSocket 发送裸 PCM：16 kHz、signed 16-bit little-endian、mono。WebSocket 每个 binary message 对应一个音频分段。如需以 48 kHz 采集，可在 `idf.py menuconfig` 的 `Bestlink recorder` 中修改输入采样率，工程会降采样到 16 kHz。

## 编译

```powershell
# 先进入 ESP-IDF 5.3+ 环境
idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

工程固定引用官方 `esp-mqtt` 与 `esp_websocket_client` 源码，避免构建机依赖在线组件仓库。默认分区按 8 MB Flash 设置，如设备容量不同请修改 `sdkconfig.defaults` 和 `partitions.csv`。

## 首次配置

1. 连接设备 AP `Bestlink-xxxxxx`（开放网络）。
2. 浏览器访问 `http://192.168.100.1`。
3. 使用 `admin` / `bestlink` 登录，然后在三个分页中配置 Wi-Fi、MQTT 和登录密码。

设备 ID 是 Wi-Fi STA MAC 的 12 位小写十六进制字符串。MQTT Client ID 与设备 ID 相同；订阅 `to/recorder/<设备ID>`，响应到 `from/recorder/<设备ID>`。

## MQTT 示例

```json
{"mid":"1","cmd":"start","url":"ws://192.168.4.250:10345/v1/record?id=test&segment=200&samplerate=16&bitrate=16&channel=1"}
```

`start` 的录音参数全部通过 `url` 的 query string 传递，MQTT JSON 不再包含顶层 `segment` 字段。`segment` 缺省为 200 ms，允许范围为 20–2000 ms；`samplerate`、`bitrate`、`channel` 的默认值分别为 16 kHz、16 bit、1（单声道），当前仅支持这些音频格式。`start` 仅在第一个 PCM binary message 成功发出后响应 `success`。

`status` 响应包含 `recording`（当前是否录音）、`duration`（本次录音时长，整数秒）和 `segments`（本次录音成功发送的语音包数）。统计在每次 `start` 时重置，并在录音停止后保留到下一次录音开始。

```json
{"mid":"2","cmd":"set","parameters":[{"para":"LED_EFFECT","value":1},{"para":"LED_BRIGHTNESS","value":50}]}
```

当前 XVF3800 `set` 支持 `LED_EFFECT`、`LED_BRIGHTNESS`、`LED_SPEED`、`LED_COLOR`；ESP32 参数支持 `wifi-ssid` 与 `wifi-password`（可在同一个 parameters 数组中提交）。

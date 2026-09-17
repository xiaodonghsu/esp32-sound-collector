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
3. 使用 `admin` / `bestlink` 登录，默认进入“设备概览”，其余三个分页用于配置 Wi-Fi、MQTT 和登录密码。

“设备概览”按以下类别显示实时信息，进入分页或点击“刷新概览”重新读取：

- 设备与固件：设备 ID、ESP-IDF 版本、固件版本、芯片型号与修订、CPU 核心数、运行时间。
- 存储与内存：Flash 实测容量和构建配置；PSRAM 初始化状态与检测容量；内部 RAM、PSRAM 的总堆内存、当前可用量、启动以来最低可用量、最大连续可分配块（同时显示 KiB/MiB 和字节）。内部 RAM 统计限定为支持字节访问的内部堆，不代表芯片全部物理 SRAM；未启用 PSRAM 时不能据此判断硬件是否存在。
- STA 上行连接：连接/IP 获取状态、SSID、MAC、接入点 BSSID、RSSI、信道、IPv4、子网掩码和网关。断开时不展示旧 IP。
- Soft AP 配置热点：启用状态、SSID、MAC、IPv4、子网掩码、当前实际信道、安全模式、连接客户端数量及上限。

概览接口 `/api/overview` 使用与管理页面相同的登录认证，不返回网络密码。打开概览不会自动触发 Wi-Fi 扫描。

“MQTT 通信参数”分页可修改服务器地址、端口、用户名和密码。未保存过配置时使用以下默认值：

| 参数 | 默认值 |
|---|---|
| 服务器地址 | `192.168.4.244` |
| 端口 | `11883` |
| 用户名 | `Recorders` |
| 密码 | `bestlink` |

保存后写入 NVS，重启后保留，并自动应用到 MQTT 客户端。密码不回显，输入框留空表示保留当前密码；首次配置留空则继续使用默认密码。保存成功表示参数已保存并应用，实际连接还取决于 Wi-Fi 和 MQTT 服务器是否可用。

设备 ID 是 Wi-Fi STA MAC 的 12 位小写十六进制字符串。MQTT Client ID 与设备 ID 相同；订阅 `to/recorder/<设备ID>`，响应到 `from/recorder/<设备ID>`。

## 无人值守断线恢复

- 已配置的 Wi-Fi 会持续重连，没有次数上限。失败后的退避上限依次为 1、2、4、8、16、32、60 秒，每次在上限的 80%～100% 内随机等待；达到上限后继续以 48～60 秒间隔尝试。实际周期还包括连接耗时及任务调度时间。
- 单次连接 30 秒仍未获取 IP，会断开该次连接并安排下一次重试；获取 IP 后重置退避。不会因网络长期不可用而主动重启设备。
- SoftAP 和配置 HTTP 服务保持启用。配网验证仍有 20 秒等待超时；失败时恢复原配置并继续后台重连。没有原配置时停止尝试无效的新配置，等待再次配网。并发配网请求会被拒绝，避免互相覆盖。
- MQTT 仅在 Wi-Fi 获取 IP 后启动；Wi-Fi 离线时暂停重连，恢复后重新连接和订阅。Wi-Fi 正常但 MQTT 服务不可用时，持续重试，重连请求至少相隔 3 秒。
- MQTT 响应使用 QoS 1 非阻塞入队，SDK outbox 限额为 32 KiB；队列满或分配失败会记录警告。队列在普通断网期间保留，但消息仍受 SDK 过期时间限制，重启设备或重新配置 MQTT 会清空队列，不保证永久离线消息交付。
- MQTT 掉线不会主动停止录音。音频仍使用固定大小的 Ring Buffer，满时优先丢弃旧片段，不会无限积压。

实机回归步骤（需烧录后执行）：

1. 保存有效配置后关闭路由器，让设备经历超过 10 次失败，确认仍有退避重试日志；重新打开路由器，确认自动获取 IP、连接 MQTT，并能响应 `status`。
2. 保持路由器开启但暂停 DHCP，确认连接超时后继续重试；恢复 DHCP 后确认自动恢复。
3. 离线期间访问设备 AP 的配置页面，提交错误密码，确认验证超时后恢复原配置；再提交正确配置，确认保存并恢复 MQTT。首次配网失败时应仍可再次配网。
4. 仅关闭 MQTT 服务，确认音频继续发送；恢复服务后确认重新订阅并可执行 `stop`。
5. 录音期间反复断开、恢复 Wi-Fi，观察堆内存无持续增长；确认 WebSocket 恢复发送，并且设备无需重启。

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

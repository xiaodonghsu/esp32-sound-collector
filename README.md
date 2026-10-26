# ESP32 4-Mic Array Collector

XIAO ESP32-S3 / ESP32-C5 + reSpeaker XVF3800 录音终端。工程基于 ESP-IDF 6（已在 6.0.2 构建验证），包含：

- 始终启用的 `Bestlink-xxxxxx` SoftAP（`192.168.100.1`）和分页配置页面；
- NVS 持久化、2.4/5 GHz Wi-Fi 扫描（显示频段、信道和主信道中心频率）和保存前连接验证；
- MQTT `start` / `stop` / `status` / `set` 控制面；
- XVF3800 I2C 版本、LED 参数读取和设置；
- I2S 采集任务、Audio Ring Buffer、WebSocket 发送任务及自动重连。

## 硬件和音频格式

`main/board_pins.h` 根据 ESP-IDF 目标芯片（`CONFIG_IDF_TARGET_ESP32S3` / `CONFIG_IDF_TARGET_ESP32C5`）自动选择 GPIO，I2C 控制与 I2S 音频驱动共用此映射。两块 XIAO 使用相同的板上 D 引脚位置连接 XVF3800，无需手动修改驱动中的 GPIO；其他目标芯片会在编译时报错。

| XIAO 引脚 | ESP32-S3 GPIO | ESP32-C5 GPIO | 本项目用途（ESP32 视角） |
|---|---:|---:|---|
| D4 | 5 | 23 | I2C SDA |
| D5 | 6 | 24 | I2C SCL |
| D6 | 43 | 11 | I2S DIN（接收 XVF3800 音频） |
| D7 | 44 | 12 | I2S DOUT |
| D8 | 7 | 8 | I2S WS |
| D9 | 8 | 9 | I2S BCLK |
| D10 | 9 | 10 | 预留，当前未使用 |

表中的数字为芯片 GPIO 编号；板上 UART/SPI 标注是复用功能，本项目将 D6–D9 配置为 I2S。该适配以所列 XIAO 引脚连接为前提，C5 的实际录音及 XVF3800 通信仍需实机验证。

工程默认匹配 Seeed 的录音/回放例程：ESP32 作为 I2S master 输出 BCLK/WS，采样格式为 16 kHz、stereo、32-bit、Philips I2S。采集时取左声道高 16 bit，向 WebSocket 发送裸 PCM：16 kHz、signed 16-bit little-endian、mono。WebSocket 每个 binary message 对应一个音频分段。如需以 48 kHz 采集，可在 `idf.py menuconfig` 的 `Bestlink recorder` 中修改输入采样率，工程会降采样到 16 kHz。

## 编译

```powershell
# 先进入 ESP-IDF 6.0.2 环境
# XIAO ESP32-C5
idf.py set-target esp32c5
# XIAO ESP32-S3 则使用：idf.py set-target esp32s3
idf.py build
idf.py -p COMx flash monitor
```

`sdkconfig.defaults` 当前默认目标为 ESP32-C5；切换开发板时执行对应的 `set-target`，引脚映射随目标自动切换。S3 仅支持 2.4 GHz Wi-Fi，双频功能仅适用于 C5。

工程固定引用官方 `esp-mqtt` 与 `esp_websocket_client` 源码，避免构建机依赖在线组件仓库。默认分区按 8 MB Flash 设置，如设备容量不同请修改 `sdkconfig.defaults` 和 `partitions.csv`。

## 首次配置

### PSRAM 容量检查

工程按 ESP32-C5 配置 Quad PSRAM（80 MHz）、启动初始化及 ESP-IDF 内存测试。PSRAM 加入 `heap_caps` 分配器，普通 `malloc()` 仍默认使用内部 RAM。初始化失败时继续使用内部 RAM，管理页面会报告不可用；不能把初始化失败解释为硬件容量为零。

烧录并重启后，在串口日志中查看 `PSRAM detected: ... bytes (... KiB)`。8 MiB 对应 `8388608 bytes (8192 KiB)`，这只是预期示例，实际容量以设备检测结果为准。也可登录“设备概览”，查看“PSRAM 状态”“PSRAM 检测容量”和 PSRAM 堆统计；可用堆空间不等于物理容量。

已有 `sdkconfig` 不会被 `sdkconfig.defaults` 覆盖。使用已有配置时，请在 `idf.py menuconfig` 的 `Component config → ESP PSRAM` 中同步以上设置，再编译烧录。

### 网络配置

1. 连接设备 AP `Bestlink-xxxxxx`（开放网络）。
2. 浏览器访问 `http://192.168.100.1`。
3. 使用 `admin` / `bestlink` 登录，默认进入“设备概览”，其余分页用于配置 Wi-Fi、MQTT、登录密码和固件升级。

Wi-Fi 扫描列表显示例如 `Office (5 GHz · 信道 36 · 5180 MHz · -55 dBm)`；列表先显示 5 GHz，再显示 2.4 GHz，各频段内按 RSSI 从高到低排序（例如 -40 dBm 排在 -70 dBm 前）。同名 SSID 在不同信道上分别显示，同名且同信道的结果保留信号最强的一个。频率是主信道的中心频率（例如信道 1 为 2412 MHz，信道 14 为 2484 MHz），不是绑定宽信道的中心频率。保存时仍按 SSID 连接，不锁定所显示的频段或信道。首次连接及断线重连使用全信道扫描、按信号排序，并将 `rssi_5g_adjustment` 设置为 128，使同名双频网络优先尝试 5 GHz，保留 2.4 GHz 作为回退；已连接时不会因为页面扫描主动切换网络。默认选中已保存 SSID 中排序最靠前的条目。实际可扫描信道受国家/地区配置限制。C5 启用 `WIFI_BAND_MODE_AUTO`，支持在 2.4/5 GHz 间选择；APSTA 共用射频，连接 5 GHz 上行后配置热点也会跟随上行信道。

“设备概览”按以下类别显示实时信息，进入分页或点击“刷新概览”重新读取：

- 设备与固件：设备 ID、ESP-IDF 版本、固件版本、芯片型号与修订、CPU 核心数、运行时间。
- 存储与内存：Flash 实测容量和构建配置；PSRAM 初始化状态与检测容量；内部 RAM、PSRAM 的总堆内存、当前可用量、启动以来最低可用量、最大连续可分配块（同时显示 KiB/MiB 和字节）。内部 RAM 统计限定为支持字节访问的内部堆，不代表芯片全部物理 SRAM；未启用 PSRAM 时不能据此判断硬件是否存在。
- STA 上行连接：连接/IP 获取状态、SSID、MAC、接入点 BSSID、RSSI、信道、频率（主信道中心频率，MHz）、IPv4、子网掩码和网关。断开时不展示旧 IP、信道或频率。
- Soft AP 配置热点：启用状态、SSID、MAC、IPv4、子网掩码、当前实际信道和频率（MHz）、安全模式、连接客户端数量及上限。热点关闭或频率读取失败时显示“不可用”。

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

所有 MQTT 上行响应（包括 `start`、`stop`、`status`、`set` 及错误响应）统一携带当前 STA Wi-Fi 信息：`wifi-band` 为 `"2.4 GHz"` 或 `"5 GHz"`，`wifi-channel` 为信道号，`wifi-frequency-mhz` 为主信道中心频率（MHz，JSON 数字）。例如：`{"mid":"123","result":"success","wifi-band":"5 GHz","wifi-channel":36,"wifi-frequency-mhz":5180}`。未关联接入点或无法读取有效信道时，这三个字段均为 JSON `null`。信息取自消息生成时的连接快照，离线队列稍后发送时不会改写为新的连接信息。

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

## 网页固件升级（OTA）

Flash 构建容量为 8MiB，采用两个各 2MiB 的应用分区。PSRAM 配置保持启用。`partitions.csv` 保持原 NVS（0x9000 / 24KiB）及 PHY（0xf000 / 4KiB）位置，新增 OTA 状态区（0x10000 / 8KiB），应用分区为 `ota_0`（0x20000）和 `ota_1`（0x220000），崩溃转储位于 0x420000。

### 首次迁移

旧的单 factory 分区固件不能直接通过网页迁移。先在 ESP-IDF 环境中编译并通过 USB 烧录完整工程（含 bootloader、分区表、初始 OTA 状态和应用），不执行 `erase-flash`，以保留原 NVS 配置：

```powershell
idf.py build
idf.py -p COMx flash monitor
```

已有 `sdkconfig` 需同步为 8MB Flash、启用 `CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE=y`，并使用本项目的自定义分区表；本次修改已同步本地配置。迁移完成后，后续仅上传 `build/esp32_4mic_array_collector.bin`。不要上传 bootloader、partition-table、ZIP 或合并烧录包。建议发布时设置独立的项目固件版本，便于在设备概览辨认升级结果。

### 使用与故障处理

1. 登录管理页面，进入“固件升级”，选择本项目的应用 `.bin`，点击“上传并升级”。页面显示备用分区容量上限及上传进度；100% 仅表示上传完成，需等待设备校验结果。
2. 设备核对应用头、ESP32-S3 芯片标识、项目名及附带 SHA-256 校验信息，再停止录音，分块写入备用分区。接收完整文件后由 ESP-IDF 校验镜像，成功才切换启动分区并自动重启。SHA-256 用于完整性检查，不是发布者签名认证。
3. 升级期间 MQTT 控制命令返回失败；HTTP 管理服务在处理上传时暂不处理其他请求。成功重启后重新连接设备，刷新概览检查版本；Wi-Fi、MQTT 和登录配置保留。
4. 上传失败、连接断开或超时后不会切换到未校验固件，可重新上传；已停止的录音不会自动恢复，需重新发送 `start`。服务器接收总时限为 120 秒，单次接收使用 HTTP 服务默认超时，不支持断点续传。
5. 新固件完成本地服务初始化（配置、Wi-Fi、XVF3800 驱动、录音模块、HTTP 和 MQTT 客户端）后确认启动有效，不依赖外部 Wi-Fi/MQTT 服务在线。确认前发生崩溃或复位时，bootloader 回滚到上一有效固件。该检查不是端到端录音自检，也不覆盖确认后才出现的故障。首次 USB 烧录只有一个有效镜像，尚无旧 OTA 固件可回滚。

自动检查：`node --test tests/web_ota.test.cjs` 验证嵌入页面的文件限制、请求格式、进度与失败重试逻辑；`idf.py build` 检查编译与分区容量。

实机验收（需连接设备）：正常升级后验证新版本与 NVS 配置；上传错误项目、损坏或截断镜像及超过 2MiB 文件，确认拒绝；录音中升级并并发发送 MQTT 命令，确认录音停止且控制命令被拒绝；上传途中断网/断电后确认旧固件仍能启动；使用在启动确认前主动复位的测试固件验证回滚；最后验证升级后录音及 WebSocket 数据发送。

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

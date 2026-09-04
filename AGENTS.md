# ESP32-4MIC-ARRAY-COLLECTOR 开发需求

这是通过 XIAO ESP32-S3 控制 ReSpeaker XVF3800 4-Mic Array 麦克风阵列采集声音，并将采集的声音数据发送到指定的 websocket 服务端的物联终端项目

这个项目完成ESP32-S3的代码开发，并使用 esp-idf 编译测试。

## 设备ID

使用 ESP32 的wifi网卡的MAC地址，去掉任何间隔的12个字符（小写），例如：3c22fb4a1b8f

## 联网配置

Wifi配置后，设备启动后，自动开启 Bestlink-4a1b8f （设备ID后面6个字符）的softAP, AP地址： 192.168.100.1，并启动配置页面的http-server

配置页面需要登陆密码，默认用户名： admin， 密码: bestlink

### 配置页面配置内容

1. 基本通信设置：wifi 连接的SSID，（通过ESP32扫描附近的SSID，通过列表展现，用户选择）; wifi的密码，用户自行输入；输入后，esp32尝试连接该wifi，验证密码及连接性，能够正常连接，自动保存。

2. 控制面的MQTT协议配置： Mqtt 的 服务器地址、端口号、用户名、密码。
默认值：
mqtt-server: 192.168.4.244
mqtt-port: 11883
mqtt-user: Recorders
mqtt-password: bestlink

3. 登录用户的密码修改。

注意：**修改的内容需要分页**

## 控制面

设备正常联网后，MQTT客户端尝试连接MQTT服务端，设备ID作为 Client ; 侦听 to/recorder/设备ID ， 返回消息发送到 from/recorder/设备ID。

根据来自MQTT服务端的消息为JSON格式，包含的信息如下：

mid, 必须, 任意字符串
cmd , 必须 , 内容为: start - 启动录音; stop - 停止录音； status - 报告状态; set - 设置ESP32及VXV3800的参数等
url ， cmd为 start 时必须，录音参数通过 URL query string 传递，不再使用 MQTT JSON 顶层 segment 字段。例如： "ws://192.168.4.250:10345/v1/record?id=xxxxxxxxxxxxxxxxxxxxx&segment=200&samplerate=16&bitrate=16&channel=1"
parameters, cmd 为 set 时必须， 内容为 JSON 列表，指示参数和值例如 [{"para": "LED_EFFECT", "value": 1}, {"para": "LED_BRIGHTNESS", "value": 50}]

### cmd 的执行和响应

### start指令

开始录音，连接指定的 websocker url, 按照 url 参数要求采集语音PCM，分段，发送给 服务端 ；
在流程正确发出第一个包后，通过MQTT 响应消息。
{
mid: mid,
result: "success"
}

失败
{
mid: mid,
result: "fail",
message: "xxx"
}


### stop 指令

停止录音，断开 websocket 连接, 在流程正确执行后，通过MQTT 响应消息。
{
mid: mid,
result: "success"
}

失败
{
mid: mid,
result: "fail",
message: "xxx"
}

### status 指令

反馈当前状态，查询设备的信息，当前状态反馈给服务端，反馈的内容包括:
联网信息 状态
wifi-ssid: 当前作为client连接wifi的ssid
wifi-rssi: 当前连接的wifi的接收电平
wifi-mac: wifi网卡mac地址
wifi-ipaddr: wifi获取的IP地址

录音机的配置信息
recorder-type: 固定为: XVF3800，
recorder-version: XVF3800 的版本VERSION (参考资料#1）
led-effict:   读取XVF3800 的LED_EFFECT 参数(参考资料#1）
led-brightness:  读取XVF3800 的LED_BRIGHTNESS 参数参考资料#1）
led-speed:  读取XVF3800 的LED_SPEED 参数(参考资料#1）
led-color:   读取XVF3800 的LED_COLOR 参数(参考资料#1）

录音状态统计功能：
- recording：当前是否正在录音，JSON 布尔值 true/false
- duration：本次录音时长，整数秒
- segments：成功发送的语音包数量

### set 指令

可以I2C 控制的XVF3800的参数通过set指令设置；

ESP32工作的参数： wifi-ssid, wifi-password 这类参数，也需要支持set 指令

## 数据面

数据面指的是ESP32采集语音PCM，分段并传递给Websocker 端

### 语音采集要求

采样要求在 url 中通过param (segment / samplerate /bitrate/channel) 指定：

- 采样率(samplerate)：默认  16 单位 kHz,
- bit率(bitrate): 默认值 16 单位 bit
- 通道(channel): 单声道默认 1（单声道)
- 发包频率： 按照  segment 参数 默认 200ms 指定分包音频

### Ring Buffer 机制

注意：采集线程和网络发送线程需要解耦，发送的数据包采用 Ring Buffer 机制：

I2S采集线程
     │
     ▼
Audio Ring Buffer
     │
     ▼
WebSocket发送线程

### 断线重连

esp32 应该能够处理可能发生的断线问题，能断线重连

## 参考资料

1. 使用 I2C 命令通过 XIAO ESP32S3 控制 reSpeaker XVF3800 USB Mic Array

https://wiki.seeedstudio.com/cn/respeaker_xvf_3800_i2c_list/ 

2. reSpeaker XVF3800 USB Mic Array 搭配 XIAO ESP32S3 的 WebSocket 音频流传输

https://wiki.seeedstudio.com/cn/respeaker_xvf3800_xiao_websocket_audio_stream/

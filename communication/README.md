# 通信系统 V7.3

本目录包含机器人通信链路的三端代码：

```text
communication/
├─ 固件/esp32s3_l610_ps2_v5/  ESP32-S3 + OV5640 + L610 RNDIS
├─ 固件/bw21_l610_ecm.c       旧 BW21 方案，仅供 legacy 参考
├─ 服务器端/                  Python 3.8 + aiohttp
└─ 前端/                      Vue 3 + Vite
```

本次更新见 [CHANGELOG_V7_3.md](../CHANGELOG_V7_3.md)。STM32 对接以
[STM32_ESP32_UART_INTEGRATION.md](固件/esp32s3_l610_ps2_v5/docs/STM32_ESP32_UART_INTEGRATION.md)
为准。

## 链路

- 视频：ESP32-S3 将 OV5640 JPEG 按 ESJP 分片发往服务端 `9091/udp`，服务端重组后发布 MJPEG/WebSocket。
- 遥测：STM32 通过 UART `type=0x01` 上报统一传感器与 GPS 快照，ESP 转为 ESUT 发往 `9093/udp`。
- 在线：ESP 每 2 秒独立发送心跳；ESP 在线但 STM32 暂无数据时，实时字段为 `null`，前端显示 `NA`。
- 控制：前端发送 14 个 PS2 键的 `down/up`，服务端经 ESCTL 转发，ESP 通过 UART `type=0x02` 发送给 STM32。
- 本地串流：SoftAP MJPEG 与云端 L610 视频可同时工作。
- 网络管理：前端可读取 4G 状态、调整频段、测试延迟，并按需启用 Wi-Fi 扫描和连接。
- 定位：有 GPS 时以 GPS 为主；无 GPS 时可使用服务端配置的基站定位结果。

## 配置

仓库默认使用 `192.0.2.10`（RFC 5737 TEST-NET）作为云端示例地址，
使用 `CHANGE_ME_*` 作为凭据占位符。部署时设置以下位置：

- `前端/src/config/config.js`：HTTP、WebSocket、MJPEG 地址；
- `固件/esp32s3_l610_ps2_v5/components/camera_stream/Kconfig`：视频服务器；
- `固件/esp32s3_l610_ps2_v5/components/telemetry_uart/Kconfig`：遥测服务器；
- `固件/esp32s3_l610_ps2_v5/sdkconfig.defaults`：构建默认值；
- 服务端环境变量 `UNWIRED_TOKEN`：可选基站定位凭据。

机器人 SoftAP 网关 `192.168.4.1` 是本地链路地址。默认本地视频为
`http://192.168.4.1:8080/live/mjpeg`，Wi-Fi 管理 API 为
`http://192.168.4.1:8081/api/wifi`。

## 固件构建

要求 ESP-IDF 5.5.1：

```powershell
cd communication\固件\esp32s3_l610_ps2_v5
.\build_camera_sensor_wifi_manager_v7.ps1 `
  -IdfExport 'C:\path\to\esp-idf-v5.5.1\export.ps1'
```

典型烧录布局：

```text
0x0000  bootloader.bin
0x9000  partition-table.bin
0x20000 usb_rndis_4g_module.bin
```

实际偏移以本次构建生成的 `flasher_args.json` 为准。

## 服务端

```bash
cd communication/服务器端
python3.8 -m pip install -r requirements.txt
python3.8 server.py
```

| 端口 | 用途 |
| --- | --- |
| `8765/tcp` | HTTP、WebSocket、MJPEG |
| `9091/udp` | ESJP JPEG 分片与 legacy UDP |
| `9092/tcp` | legacy H.264/FFmpeg，可用 `LEGACY_BW21_ENABLED=false` 关闭 |
| `9093/udp` | ESUT 遥测/心跳与 ESCTL 控制 |

验证：

```bash
python3.8 -m unittest discover -v
python3.8 -m compileall -q .
```

基站定位需要通过环境变量设置服务地址和凭据，源码中不保存运行凭据。

## 前端

```bash
cd communication/前端
npm ci
npm run build
```

## STM32 UART 摘要

```text
A5 5A | version:u8 | type:u8 | json_len:u16be | seq:u32be |
UTF-8 JSON | CRC16:u16be | 0D 0A
```

- `0x01`：STM32 到 ESP，`sensor` + `gps`；
- `0x02`：ESP 到 STM32，PS2 `button/state`；
- `0x03`：保留，无 ACK。

STM32 必须实现 500 ms 本地按键释放和停车看门狗，不能依赖浏览器、4G 或云端服务完成最终停止。

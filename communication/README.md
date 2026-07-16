# 通信 v5

本目录包含通信链路的三端代码：

```text
communication/
├─ 固件/esp32s3_l610_ps2_v5/  ESP32-S3 + OV5640 + L610 RNDIS
├─ 固件/bw21_l610_ecm.c       旧 BW21 方案，仅作 legacy 参考
├─ 服务器端/                   Python 3.8 + aiohttp
└─ 前端/                       Vue 3 + Vite
```

详细改动见仓库根目录 [CHANGELOG_V5.md](../CHANGELOG_V5.md)，STM32 对接以 [STM32_ESP32_UART_INTEGRATION.md](固件/esp32s3_l610_ps2_v5/docs/STM32_ESP32_UART_INTEGRATION.md) 为准。

## 链路

- 视频：ESP32-S3 将 OV5640 JPEG 按 ESJP 分片发往服务端 `9091/udp`，服务端校验并发布 MJPEG/WebSocket。
- 遥测：STM32 用 UART `type=0x01` 上报统一的传感器与 GPS 快照，ESP 转为 ESUT 发往 `9093/udp`。
- 在线：ESP 每 2 秒独立发送心跳；ESP 在线但 STM32 没有新数据时，实时字段为 `null`，前端显示 `NA`。
- 控制：前端发送 14 个 PS2 键的 `down/up`，服务端通过 ESCTL 转发，ESP 用 UART `type=0x02` 发给 STM32。
- AP 串流：ESP 可同时提供本地 MJPEG 和 L610 云端视频，开关由 ESP 自己处理。

## 公开示例配置

仓库中的 `192.0.2.10` 是 RFC 5737 TEST-NET 示例地址，不会连接实际服务器；`CHANGE_ME_*` 也是不可部署的占位口令。请只在私有副本中替换：

- `前端/src/config/config.js`：HTTP、WebSocket、MJPEG 地址与地图初始位置。
- `固件/esp32s3_l610_ps2_v5/sdkconfig.defaults`：视频和遥测服务器地址。
- `固件/esp32s3_l610_ps2_v5/sdkconfig.v1-8fps-q20-ap-napt.defaults`：AP 密码与遥测地址。
- `固件/esp32s3_l610_ps2_v5/build_camera_sensor_8fps_q20_ap_local_mjpeg_cloud_ps2_v5.ps1`：本机 ESP-IDF 路径。

不要提交替换后的现场配置、有效 `sdkconfig`、日志、抓包或固件二进制。

## 固件构建

要求 ESP-IDF 5.5.1。先在私有工作副本中填写地址、热点密码和 ESP-IDF 路径，然后运行：

```powershell
cd communication\固件\esp32s3_l610_ps2_v5
.\build_camera_sensor_8fps_q20_ap_local_mjpeg_cloud_ps2_v5.ps1
```

典型烧录布局：

```text
0x0000  bootloader.bin
0x9000  partition-table.bin
0x20000 usb_rndis_4g_module.bin
```

实际烧录始终以本次构建生成的 `flasher_args.json` 为准。

## 服务端

```bash
cd communication/服务器端
python3.8 -m pip install -r requirements.txt
python3.8 server.py
```

| 端口 | 用途 |
| --- | --- |
| `8765/tcp` | HTTP、WebSocket、MJPEG |
| `9091/udp` | ESJP JPEG 分片及 legacy UDP |
| `9092/tcp` | legacy H.264/FFmpeg，可用 `LEGACY_BW21_ENABLED=false` 关闭 |
| `9093/udp` | ESUT 遥测/心跳与 ESCTL 控制 |

测试：

```bash
python3.8 -m unittest discover -v
python3.8 -m compileall -q .
```

代码中的演示登录用户名、密码和 token 均为 `CHANGE_ME_*` 占位值，不是生产鉴权方案。公网部署前需要自行接入正式认证，并用反向代理、HTTPS/WSS、防火墙或 VPN 限制访问。

## 前端

```bash
cd communication/前端
npm ci
npm run build
```

构建前在私有副本中将 `src/config/config.js` 的 `192.0.2.10` 替换为自己的服务器地址。手机连接机器人热点后的本地串流地址仍为 `http://192.168.4.1:8080/live/mjpeg`；这是机器人 SoftAP 私网地址，不是公网服务器信息。

## STM32 UART 摘要

```text
A5 5A | version:u8 | type:u8 | json_len:u16be | seq:u32be |
UTF-8 JSON | CRC16:u16be | 0D 0A
```

- `0x01`：STM32 → ESP，`sensor` + `gps`。
- `0x02`：ESP → STM32，PS2 `button/state`。
- `0x03`：保留，无 ACK。

STM32 必须实现 500 ms 本地释放/停车看门狗，不能依赖浏览器、4G 或云服务器完成最终安全停止。

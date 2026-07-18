# Communication v5 更新说明

发布日期：2026-07-17

本次把已经联调的 ESP32-S3 + OV5640 + L610 RNDIS 通信链路、Python 服务端和 Vue 前端整理到仓库中。旧 BW21 文件仍保留作历史参考。

## ESP32-S3 固件

- 默认使用 OV5640 VGA JPEG、8 FPS、质量参数 20。
- JPEG 通过 ESJP 分片上传至服务端 `9091/udp`。
- ESP 每 2 秒独立发送在线心跳；STM32 暂无新数据时，设备仍可在线，实时字段显示 `NA`。
- STM32 通过 UART `type=0x01` 在同一帧上报传感器与 GPS；保留 CRC16、帧头帧尾和 300 ms 截断超时校验。
- AP SoftAP/NAPT 与本地 MJPEG 可由云端开关，本地视频和 L610 云端视频可同时运行。
- 新增 UART `type=0x02` PS2 控制，负载仅为 `button/state`；`type=0x03` 保留但不使用，STM32 无需返回执行 ACK。
- 摄像头固定在机身上，不提供独立摄像头方向命令。

允许的 PS2 键：

```text
PAD_UP PAD_RIGHT PAD_DOWN PAD_LEFT
L1 R1 L2 R2
TRIANGLE CIRCLE CROSS SQUARE
SELECT START
```

前端按下发送 `down`，长按约每 200 ms 重发，松开发送 `up`。STM32 应在 500 ms 未收到续发 `down` 时自动释放并停车。

## 服务端

- 支持 ESJP v1/v2 JPEG 分片重组、CRC32、JPEG 头尾、长度与超时校验。
- OV5640 JPEG 直接发布到 MJPEG/WebSocket，不经过 FFmpeg 转码。
- `9093/udp` 处理 ESP 心跳、统一传感器/GPS 快照及 ESCTL 控制返回路径。
- 分离“ESP 在线”和“STM32 数据新鲜度”；过期数据清空为 `null`，前端显示 `NA`。
- PS2 控制只接受 14 个按键和 `down/up`，不再翻译成 `move/rotate/camera`。
- 旧 `9092/tcp` H.264/FFmpeg 路径继续保留，可用 `LEGACY_BW21_ENABLED=false` 关闭。

## Vue 前端

- 增加右侧常驻事件记录栏、事件清空二次确认和图片放大查看。
- 疑似幸存者事件优先显示并自动保存当前视频截图、时间和经纬度。
- 气体状态为 1 时单独记录危险事件，不与人员事件合并。
- AP 串流开关移入机器人配置区，并显示等待、成功或失败状态。
- 气体状态归入实时数据栏。
- 控制面板和键盘一一对应 14 个 PS2 键；页面失焦、隐藏、卸载或 WebSocket 断开时释放当前按键。

## STM32 对接

UART 保持 `115200 8N1`、3.3 V TTL、必须共地：

```text
A5 5A | version:u8 | type:u8 | json_len:u16be | seq:u32be |
UTF-8 JSON | CRC16:u16be | 0D 0A
```

- `0x01`：STM32 → ESP，统一 `sensor` + `gps` 快照。
- `0x02`：ESP → STM32，例如 `{"button":"PAD_UP","state":"down"}`。
- `0x03`：保留，不使用。

完整规范见 `communication/固件/esp32s3_l610_ps2_v5/docs/STM32_ESP32_UART_INTEGRATION.md`。

## 配置与构建

- 云端推流/API/WebSocket 默认使用 RFC 5737 TEST-NET 示例地址 `192.0.2.10`。
- AP、Web 管理和演示登录使用 `CHANGE_ME_*` 占位配置。
- 地图默认坐标为 `0,0`。
- `sdkconfig`、构建目录、日志、抓包和预编译固件由本地构建环境生成。

示例值不能直接用于部署。部署前应在构建环境中填写实际地址、口令和服务端配置。

## 已知安全边界

当前通信协议保持已验证逻辑不变。公网部署时必须使用防火墙、安全组、VPN/专线和 HTTPS/WSS 限制访问；演示登录占位逻辑不能当作生产鉴权。机器人最终失联停车必须由 STM32 本地 500 ms 看门狗保证。

# ESP32-S3 + OV5640 + L610 RNDIS 通信固件 V7.3

本工程包含：

- OV5640 JPEG 云端视频和 SoftAP 本地 MJPEG；
- 5/8/15/20/30 FPS 动态设置；
- VGA、HD、FHD 分辨率设置；
- L610 4G 状态、频段和小区信息管理；
- 按需启用的 Wi-Fi 扫描、连接和云端链路切换；
- ESP 在线心跳与 STM32 统一传感器/GPS 遥测；
- 云端 PS2 按键 `down/up` 转 UART 控制。

## 配置

源码默认使用以下示例值：

- 云端视频和遥测服务器：`192.0.2.10`（RFC 5737 TEST-NET）；
- AP 密码：`CHANGE_ME_AP_PASSWORD`；
- Web 管理密码：`CHANGE_ME_WEB_PASSWORD`。

构建前在本地工作副本中设置实际服务器和口令。机器人 SoftAP 网关为
`192.168.4.1`，本地 MJPEG 默认地址为 `http://192.168.4.1:8080/`，
Wi-Fi 管理接口默认位于 `http://192.168.4.1:8081/api/wifi`。

## 构建

安装 ESP-IDF 5.5.1 后执行：

```powershell
.\build_camera_sensor_wifi_manager_v7.ps1 `
  -IdfExport 'C:\path\to\esp-idf-v5.5.1\export.ps1'
```

脚本在独立 staging 目录中构建，输出到被 Git 忽略的
`firmware_binaries\camera_sensor_4g_wifi_v7`。

## 烧录

以构建生成的 `flasher_args.json` 为准。当前典型布局：

```text
0x0000  bootloader.bin
0x9000  partition-table.bin
0x20000 usb_rndis_4g_module.bin
```

也可以在 staging 工程中使用：

```powershell
idf.py -p COM6 flash monitor
```

## STM32 UART

UART1 使用 GPIO2/GPIO1、3.3 V TTL、`115200 8N1`、无流控并共地。
完整协议见 [STM32_ESP32_UART_INTEGRATION.md](docs/STM32_ESP32_UART_INTEGRATION.md)。

- `type=0x01`：STM32 到 ESP，统一 `sensor` + `gps` 快照；
- `type=0x02`：ESP 到 STM32，PS2 `button/state`；
- `type=0x03`：保留，不使用。

摄像头固定在机身上，不定义独立摄像头控制。STM32 在 500 ms 内没有收到
续发 `down` 时应释放按键并停车。

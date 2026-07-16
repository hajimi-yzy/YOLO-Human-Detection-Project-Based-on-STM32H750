# ESP32-S3 + OV5640 + L610 RNDIS 通信固件 v5

当前配置包含：

- OV5640 VGA JPEG，8 FPS，JPEG quality 20；
- L610 USB RNDIS 云端视频；
- ESP 独立心跳与 STM32 统一传感器/GPS 遥测；
- AP SoftAP/NAPT 与本地 MJPEG；
- 云端 AP 开关；
- 14 键 PS2 `down/up` 转 UART `type=0x02`；
- STM32 无控制 ACK。

## 公开占位值

源码中的以下值已脱敏：

- 云端视频/遥测服务器：`192.0.2.10`（RFC 5737 TEST-NET，不可部署）；
- AP 密码：`CHANGE_ME_AP_PASSWORD`；
- 可选 Web 管理密码：`CHANGE_ME_WEB_PASSWORD`；
- ESP-IDF 路径：`C:\path\to\esp-idf-v5.5.1\export.ps1`。

只在私有工作副本中替换这些值，不要把现场地址或口令提交回公共仓库。机器人本地 SoftAP 网关 `192.168.4.1` 是协议所需私网地址，并非公网服务器。

## 构建

1. 安装 ESP-IDF 5.5.1。
2. 在私有副本中修改：
   - `sdkconfig.defaults` 的视频/遥测服务器；
   - `sdkconfig.v1-8fps-q20-ap-napt.defaults` 的热点密码与遥测服务器；
   - 构建脚本中的 `$IdfExport`。
3. 运行：

```powershell
.\build_camera_sensor_8fps_q20_ap_local_mjpeg_cloud_ps2_v5.ps1
```

脚本使用独立 staging 目录，不覆盖其他固件版本。输出位于本地 `firmware_binaries\camera_sensor_8fps_q20_ap_local_mjpeg_cloud_ps2_v5`；该目录被 Git 忽略，因为二进制会固化部署配置。

## 烧录

典型布局：

```text
0x0000  bootloader.bin
0x9000  partition-table.bin
0x20000 usb_rndis_4g_module.bin
```

以本次构建生成的 `flasher_args.json` 为准。

## STM32 UART

正式接口为 UART1 GPIO2/GPIO1、3.3 V TTL、`115200 8N1`、无流控、必须共地。完整协议见 [STM32_ESP32_UART_INTEGRATION.md](docs/STM32_ESP32_UART_INTEGRATION.md)。

- `type=0x01`：STM32 → ESP，统一 `sensor` + `gps` 快照；
- `type=0x02`：ESP → STM32，PS2 `button/state`；
- `type=0x03`：保留，不使用。

摄像头固定在机身上，不定义独立摄像头控制。STM32 必须在 500 ms 没有收到续发 `down` 时释放按键并停车。

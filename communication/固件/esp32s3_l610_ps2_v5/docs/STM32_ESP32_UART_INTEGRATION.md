# STM32 与 ESP32-S3 UART 对接协议

文档版本：`1.2`

日期：`2026-07-15`

适用工程：`esp32s3_l610_ap_test`
传输方向：STM32 传感器/GPS 上报，ESP32-S3 PS2 按键下发

本文是 STM32 与 ESP32-S3 之间 UART 对接的主规范。新代码以本文为准；旧文档中的气体浓度对象、分离的 sensor/GPS 包以及旧裸 JSON 运动命令均不再作为新实现依据。

## 1. 当前实现状态

| 功能 | 方向 | 帧类型 | 当前 v5 状态 |
| --- | --- | ---: | --- |
| 传感器与 GPS 完整快照 | STM32 -> ESP32 | `0x01` | 已实现并已通过 TTL 注入测试 |
| ESP 独立云端心跳 | ESP32 -> 云服务器 | 不经过 STM32 UART | 已实现，2 秒一次 |
| AP 热点控制 | 云服务器 -> ESP32 | 不经过 STM32 UART | 已实现，由 ESP 自己处理 |
| 运动控制 | ESP32 -> STM32 | `0x02` | 已实现并通过 ESP-IDF 5.5.1 完整构建 |
| 控制执行 ACK | STM32 -> ESP32 | `0x03` | 不使用，保留类型号 |

重要说明：v5 固件保留已验证的 `type=0x01` 接收链路，只增加 `type=0x02` 下发，不要求 STM32 返回执行回执。固件已经完成构建，但仍需连接 STM32 做实机联调。

## 2. 硬件连接

正式固件使用 ESP32-S3 `UART1`：

| STM32 | ESP32-S3 | 方向 | 说明 |
| --- | --- | --- | --- |
| USART TX | GPIO2 / UART1 RX | STM32 -> ESP | 遥测帧 |
| USART RX | GPIO1 / UART1 TX | ESP -> STM32 | 控制命令 |
| GND | GND | 双向 | 必须共地 |

电气与串口参数：

- 逻辑电平：`3.3 V TTL`，不得把 5 V TTL 直接连接 ESP GPIO。
- 波特率：`115200`。
- 数据位：`8`。
- 校验位：无。
- 停止位：`1`。
- 流控：无。
- UART 为全双工，STM32 上报遥测时 ESP 可以同时下发控制。
- 不得在同一个 UART 输出 `printf` 调试日志，否则日志字节会破坏二进制帧。

COM6/UART0 共用调试口只用于 PC 注入测试，不是正式 STM32 接口。

## 3. 统一二进制帧

所有消息使用同一个外层帧：

```text
A5 5A | version:u8 | type:u8 | json_len:u16be | seq:u32be |
UTF-8 JSON | crc16:u16be | 0D 0A
```

### 3.1 字段定义

| 偏移 | 长度 | 字段 | 规则 |
| ---: | ---: | --- | --- |
| 0 | 2 | magic | 固定 `A5 5A` |
| 2 | 1 | version | 固定 `0x01` |
| 3 | 1 | type | 见消息类型表 |
| 4 | 2 | json_len | JSON 的 UTF-8 字节数，`1..1024`，大端 |
| 6 | 4 | seq | 无符号 32 位序号，大端 |
| 10 | N | JSON | UTF-8、无 BOM，不包含 C 字符串结尾 `\0` |
| 10+N | 2 | CRC16 | 大端 |
| 12+N | 2 | tail | 固定 `0D 0A` |

总帧长度恒为：

```text
json_len + 14
```

### 3.2 消息类型

| type | 方向 | 用途 | 序号含义 |
| ---: | --- | --- | --- |
| `0x01` | STM32 -> ESP | 传感器与 GPS 完整快照 | `telemetry_seq` |
| `0x02` | ESP -> STM32 | PS2 按键状态 | `command_seq` |
| `0x03` | 保留 | 当前不使用 | - |
| `0x04..0xFF` | 保留 | 不得使用 | - |

`0x01` 与 `0x02` 使用各自独立的序号空间；`0x03` 当前不使用。

### 3.3 CRC

算法：`CRC-16/CCITT-FALSE`。

```text
poly    = 0x1021
init    = 0xFFFF
RefIn   = false
RefOut  = false
XorOut  = 0x0000
```

CRC 覆盖范围从 `version` 开始，到 JSON 最后一个字节结束，即：

```text
frame[2] ... frame[9 + json_len]
```

不包含 `A5 5A`、CRC 自身和 `0D 0A`。

标准自检：

```text
CRC16_CCITT_FALSE("123456789") = 0x29B1
```

### 3.4 接收超时与重同步

- 收到完整 `A5 5A` 后开始计时。
- `300 ms` 内未收齐完整帧，丢弃本帧并重新搜索帧头。
- 长度为 0、长度大于 1024、version/type 不支持、CRC 错误或帧尾错误都必须丢弃。
- 错误帧不得刷新运动看门狗，不得更新传感器数据。
- 接收器必须支持“一帧被拆成多次 DMA/中断到达”以及“一次收到多帧”。
- 不允许把 C 结构体直接映射到帧头；STM32 为小端，所有多字节整数必须显式转为大端。

## 4. STM32 上报传感器与 GPS（type `0x01`）

### 4.1 完整快照原则

每个 `0x01` 帧都是一次完整的当前快照，不是增量补丁。STM32 推荐每帧始终携带 `sensor` 和 `gps` 两个对象。某个字段无数据时发送 JSON `null`。ESP 和服务器兼容缺少对象或字段的帧，但缺少部分会在本帧中直接变成 `NA`，不会沿用上一帧旧值。

不要交替发送 sensor-only 和 gps-only 包。报警事件需要同一帧中的 GPS 才能记录当前位置。

推荐格式：

```json
{
  "sensor": {
    "temperature": 26.4,
    "humidity": 58.2,
    "altitude": 42.1,
    "pressure": 1008.6,
    "gas": 0,
    "person_detected": 0
  },
  "gps": {
    "lat": 39.904200,
    "lng": 116.407400,
    "satellites": 12,
    "speed": 0.0,
    "heading": 0.0
  }
}
```

实际发送时可以去掉空格和换行以缩短帧长。

### 4.2 字段类型、单位和范围

| 字段 | JSON 类型 | 单位/含义 | 有效范围 | 无数据 |
| --- | --- | --- | --- | --- |
| `sensor.temperature` | number | 摄氏度 `°C` | `-40.0..85.0` | `null` |
| `sensor.humidity` | number | 相对湿度 `%RH` | `0.0..100.0` | `null` |
| `sensor.altitude` | number | 米 `m` | `-500.0..10000.0` | `null` |
| `sensor.pressure` | number | 百帕 `hPa` | `300.0..1100.0` | `null` |
| `sensor.gas` | integer | `0=正常`，`1=报警` | 仅 `0/1` | `null` |
| `sensor.person_detected` | integer | `0=未发现`，`1=疑似幸存者` | 仅 `0/1` | `null` |
| `gps.lat` | number | WGS-84 纬度，十进制度 | `-90..90` | `null` |
| `gps.lng` | number | WGS-84 经度，十进制度 | `-180..180` | `null` |
| `gps.satellites` | integer | 使用卫星数 | `0..99` | `null` |
| `gps.speed` | number | 米每秒 `m/s` | `0..100` | `null` |
| `gps.heading` | number | 真北顺时针航向角，度 | `0 <= x < 360` | `null` |

约束：

- `gas` 只发送数字 `0/1`。旧版 `concentration/%LEL` 对象仅为兼容格式，新代码不要使用。
- `person_detected` 必须发送数字 `0/1`，不要发送 JSON `true/false`。
- 不得发送字符串数字、`"NA"`、空字符串、`NaN`、`Infinity` 或 `-999` 哨兵值。
- `lat` 和 `lng` 必须同时有效或同时为 `null`。
- 无 GPS 定位时建议 `lat/lng/speed/heading=null`；已知没有卫星可发 `satellites=0`，模块状态未知则发 `null`。
- 建议温湿度、海拔、气压保留 1 位小数，经纬度保留 6 位小数，速度和航向保留 1 至 2 位。

### 4.3 报警状态规则

- 气体或人员状态从 `0` 变为 `1` 时，应在 `100 ms` 内立即发送一帧，并携带当时的 GPS。
- 状态恢复后必须明确发送一次 `0`。
- `null` 只表示未知，不会解除前端报警锁存。
- 气体和人员同时报警时仍放在同一快照内；服务器分别建立两条事件，人员事件优先显示。

### 4.4 上报频率

- 周期上报推荐 `1 Hz`，即每 1000 ms 一帧。
- 状态变化立即额外上报，不必等待下一个周期。
- 通常不需要超过 `5 Hz`。
- 即使数值没有变化，周期帧也必须使用新序号。

### 4.5 telemetry_seq

- `seq` 可以从任意 `uint32` 值开始，0 合法，不要求从 0 或 1 开始。
- 每发送一帧新快照加一。
- `0xFFFFFFFF` 后自然回绕到 0。
- 重发完全相同的帧时沿用原序号；新采样必须使用新序号。

当前服务器按 `(ESP device_id, ESP boot_id)` 对遥测序号去重。若 STM32 单独复位而 ESP 没有复位，STM32 把序号重新变成 0/1 后，服务器会持续判为旧包。

当前兼容方案二选一：

1. STM32 使用 RTC 备份寄存器或备份 SRAM 保存下一序号；不要每秒写内部 Flash。
2. STM32 单独复位时同步复位 ESP，使 ESP 生成新的 `boot_id`。

后续协议升级应加入 `stm32_boot_id`，并把服务器去重键改为 `(device_id, esp_boot_id, stm32_boot_id)`。

## 5. ESP 下发 PS2 按键（type `0x02`）

### 5.1 端到端链路

```text
网页 -> WebSocket -> 云服务器 -> ESCTL/1 UDP -> ESP32
ESP32 -> type 0x02 UART -> STM32 -> 电机控制
```

云端包中的 `device_id` 和 `boot_id` 由 ESP 校验。ESP 只把已验证、目标正确的 PS2 按键状态转换成 UART `0x02` 帧。`ap_stream` 由 ESP 自己处理，不得转发给 STM32。

摄像头固定在机身上，视角随机器人机身运动。协议中不定义 `camera_left`、`camera_right`、`camera_pan` 或任何独立摄像头控制。

### 5.2 云端到 ESP

云服务器仍使用 ESCTL/1 绑定目标设备：

```json
{
  "protocol": "ESCTL/1",
  "kind": "command",
  "cmd": "ps2_button",
  "request_id": "web-1",
  "device_id": "esp32s3-01",
  "boot_id": "72A0AE84",
  "params": {"button": "PAD_UP", "state": "down"}
}
```

ESP 校验 `device_id`、`boot_id`、`request_id`、按键名和状态。身份字段只用于云端到 ESP，不再传给 STM32。

### 5.3 ESP 到 STM32

UART JSON 只有两个字段：

```json
{
  "button": "PAD_UP",
  "state": "down"
}
```

松开同一个按键：

```json
{
  "button": "PAD_UP",
  "state": "up"
}
```

控制帧外层 `seq` 由 ESP 每帧加一，仅用于串口日志和定位问题。UART 是有序链路，STM32 不需要维护 session、request_id 或复杂的防重放状态。

### 5.4 PS2 按键一一对应表

协议只传按键，不在 ESP 或服务器中翻译成 `move/rotate/speed` 等动作。STM32 继续使用自己的 PS2 按键处理逻辑。

| 前端按键 | UART `button` | 常用 PS2 标识值 | 说明 |
| --- | --- | ---: | --- |
| 十字键上 | `PAD_UP` | `0x0010` | 原 PS2 上键 |
| 十字键右 | `PAD_RIGHT` | `0x0020` | 原 PS2 右键 |
| 十字键下 | `PAD_DOWN` | `0x0040` | 原 PS2 下键 |
| 十字键左 | `PAD_LEFT` | `0x0080` | 原 PS2 左键 |
| L2 | `L2` | `0x0100` | 原 PS2 L2 |
| R2 | `R2` | `0x0200` | 原 PS2 R2 |
| L1 | `L1` | `0x0400` | 原 PS2 L1，不控制摄像头 |
| R1 | `R1` | `0x0800` | 原 PS2 R1，不控制摄像头 |
| 三角 | `TRIANGLE` | `0x1000` | 原 PS2 三角键 |
| 圆圈 | `CIRCLE` | `0x2000` | 原 PS2 圆圈键 |
| 叉 | `CROSS` | `0x4000` | 原 PS2 叉键 |
| 方块 | `SQUARE` | `0x8000` | 原 PS2 方块键 |
| SELECT | `SELECT` | `0x0001` | 原 PS2 SELECT |
| START | `START` | `0x0008` | 原 PS2 START |

`0x0002/0x0004` 对应 L3/R3，当前网页没有这两个按键，协议 v1 不发送。

表中的数值只用于与常见 PS2 定义对照；UART JSON 发送表中的字符串，不发送原始低电平按键位图。

### 5.5 长按与短按

协议不额外传 `press_type`、`held_ms` 或 `press_id`。STM32 根据 `down/up` 的本地时间直接判断：

- 第一次收到某按键 `down`：记录 `pressed_at`，进入按下状态。
- 按住期间：网页每 200 ms 再发送一次同按键 `down`。重复 `down` 只刷新最后接收时间，不能重置 `pressed_at`，也不能重复触发按下沿动作。
- 收到 `up`：计算 `now - pressed_at`。
- 小于 500 ms：短按。
- 大于等于 500 ms：长按。
- 连续 500 ms 没收到该按键新的 `down`，按丢失 `up` 处理，释放按键并停止相关运动。

对于十字方向键，第一次 `down` 即开始相应运动，重复 `down` 只负责续命，`up` 或 500 ms 超时立即停止。其他按键的短按和长按动作由 STM32 现有 PS2 业务逻辑决定。

START 建议在第一次 `down` 时立即进入急停，不等待短按/长按判断。急停解除仍应走本地安全流程，不能由普通按键自动解除。

### 5.6 多按键与页面失联

- STM32 为每个按键维护独立的 down/up 状态。
- 前端切换十字方向时，先发送旧方向 `up`，再发送新方向 `down`。
- 若 STM32 同时看到互相冲突的方向键，按安全停止处理。
- 网页松键、窗口失焦或 WebSocket 断开时，应立即给所有当前按下按键发送 `up`。
- 即使 `up` 丢失，STM32 的 500 ms 超时也必须自动释放并停车。

## 6. 不使用控制回执

- STM32 不发送 `type=0x03`。
- ESP 不等待 STM32 ACK，也不重试等待回执。
- 云端 `sent` 只表示服务器已把 UDP 包发给 ESP，不能显示为“STM32 已执行”。
- STM32 是否安全停车由本地 500 ms 按键超时保证。

## 7. 控制序号

- ESP 在每个 `type=0x02` 帧头中递增 `seq`。
- STM32可以记录 `seq`用于调试，但不需要做复杂排序。
- 重复 `down` 只是保持按下状态，重复 `up` 只是保持释放状态，两种操作天然幂等。
- STM32 或 ESP 重启后都从“全部按键已释放、电机停止”开始，不恢复旧状态。

## 8. 必须实现的本地安全策略

### 8.1 500 ms 按键超时

- 每个处于 down 状态的按键记录最后一次有效 `down` 时间。
- 新序号的重复 `down` 每 200 ms 刷新最后接收时间，但不能重置最初按下时间。
- 500 ms 没有新的 `down`，STM32 自动把该按键变为 up。
- 十字方向键超时后立即停车。
- 停车不能依赖 ESP、4G、服务器或浏览器仍在线。

这样任何一段断开，机器人最迟在 500 ms 内停止。

### 8.2 启动和复位

- STM32 上电后电机默认关闭。
- 在收到第一条有效方向键 down 前保持停止。
- ESP 重启后最多 500 ms 没有按键续命，STM32 自动释放并停车。
- STM32 复位后也必须保持停车，不能恢复复位前 PWM。

### 8.3 急停

- START down 建议立即进入急停并锁存。
- 急停锁存时忽略所有会产生运动的按键。
- 普通按键不能解除急停。
- 电机驱动器故障、欠压或本地硬件急停应直接进入同一安全停车状态。

### 8.4 STM32 UART 发送

STM32 TX 只发送现有 `type=0x01` 遥测帧，不增加控制 ACK。现有遥测发送逻辑保持不变。

## 9. STM32 参考代码

### 9.1 CRC 与组帧

```c
#include <stddef.h>
#include <stdint.h>
#include <string.h>

uint16_t es_crc16_ccitt_false(const uint8_t *data, size_t length)
{
    uint16_t crc = 0xFFFFu;

    while (length-- > 0u) {
        crc ^= (uint16_t)(*data++) << 8;
        for (uint8_t bit = 0; bit < 8u; ++bit) {
            if ((crc & 0x8000u) != 0u) {
                crc = (uint16_t)((crc << 1) ^ 0x1021u);
            } else {
                crc = (uint16_t)(crc << 1);
            }
        }
    }
    return crc;
}

static void es_write_u16_be(uint8_t *dst, uint16_t value)
{
    dst[0] = (uint8_t)(value >> 8);
    dst[1] = (uint8_t)value;
}

static void es_write_u32_be(uint8_t *dst, uint32_t value)
{
    dst[0] = (uint8_t)(value >> 24);
    dst[1] = (uint8_t)(value >> 16);
    dst[2] = (uint8_t)(value >> 8);
    dst[3] = (uint8_t)value;
}

/* 成功返回总帧长度，失败返回 0。 */
size_t es_build_frame(uint8_t type,
                      uint32_t sequence,
                      const uint8_t *json,
                      uint16_t json_length,
                      uint8_t *output,
                      size_t output_capacity)
{
    const size_t total_length = (size_t)json_length + 14u;

    if (json == NULL || output == NULL ||
        json_length == 0u || json_length > 1024u ||
        output_capacity < total_length) {
        return 0u;
    }

    output[0] = 0xA5u;
    output[1] = 0x5Au;
    output[2] = 0x01u;
    output[3] = type;
    es_write_u16_be(&output[4], json_length);
    es_write_u32_be(&output[6], sequence);
    memcpy(&output[10], json, json_length);

    const uint16_t crc =
        es_crc16_ccitt_false(&output[2], 8u + json_length);
    es_write_u16_be(&output[10u + json_length], crc);
    output[12u + json_length] = 0x0Du;
    output[13u + json_length] = 0x0Au;
    return total_length;
}
```

### 9.2 RX 状态机伪代码

```text
SEARCH_A5:
    byte == A5 -> SEARCH_5A

SEARCH_5A:
    byte == 5A -> READ_HEADER，并启动 300ms 定时
    byte == A5 -> 保持 SEARCH_5A
    其他       -> SEARCH_A5

READ_HEADER:
    收满 version/type/len/seq
    len 不在 1..1024 -> 丢弃并 SEARCH_A5

READ_BODY:
    收满 JSON + CRC + 0D0A
    超时、帧尾错、CRC错 -> 丢弃并 SEARCH_A5
    校验成功 -> 按 type 分发，再 SEARCH_A5
```

推荐使用 STM32 HAL `HAL_UARTEx_ReceiveToIdle_DMA()` 加环形缓冲，把每次收到的字节块逐字喂给状态机。对于带 D-Cache 的 STM32H7，DMA 缓冲必须放在非缓存区，或在读取前正确执行 cache invalidate。

### 9.3 JSON 生成

- 推荐使用固定大小字符数组和 `snprintf`，检查返回值是否小于缓冲区容量。
- JSON 长度使用 `strlen(json)`，不包含结尾 `\0`。
- 不使用动态内存构造周期遥测。
- 接收控制时先把全部字段解析到临时结构体并完成范围检查，再一次性修改电机状态；禁止边解析边驱动。

## 10. 在线与 NA 逻辑

STM32 不负责向云端发送心跳。ESP 在 L610 联网后每 2 秒独立发送心跳：

- 最近 5 秒收到 ESP 心跳或有效遥测：ESP 在线。
- STM32 超过 5 秒没有新快照：ESP 仍可在线，但 sensor/GPS 当前值全部为 `null`，前端显示 `NA`。
- 某个字段为 `null`：只显示该字段 `NA`。
- 本帧缺少整个 `sensor`/`gps` 对象或某个字段：缺少部分立即按 `null` 处理，不保留上一帧数值。
- UART CRC 错误、截断帧或重复/倒退序号不更新 STM32 数据，但 ESP 心跳仍可保持设备在线。
- 视频是否在线不参与传感器在线判定。

`age_ms` 表示 ESP 遥测链路年龄，不是传感器采样年龄。

## 11. 精确测试向量

下列 JSON 必须逐字一致；增加空格、换行或改变键顺序都会改变长度与 CRC，但接收 JSON 解析器不得依赖键顺序。

### 11.1 遥测帧

```json
{"sensor":{"gas":0,"person_detected":0},"gps":{"lat":null,"lng":null}}
```

- `type=0x01`
- `seq=1`
- JSON 长度：`70 / 0x0046`
- CRC：`0x11D8`
- 总长度：84 字节

```text
A5 5A 01 01 00 46 00 00 00 01
7B 22 73 65 6E 73 6F 72 22 3A 7B 22 67 61 73 22
3A 30 2C 22 70 65 72 73 6F 6E 5F 64 65 74 65 63
74 65 64 22 3A 30 7D 2C 22 67 70 73 22 3A 7B 22
6C 61 74 22 3A 6E 75 6C 6C 2C 22 6C 6E 67 22
3A 6E 75 6C 6C 7D 7D
11 D8 0D 0A
```

### 11.2 PAD_UP 按下

```json
{"button":"PAD_UP","state":"down"}
```

- `type=0x02`
- `seq=100`
- JSON 长度：`34 / 0x0022`
- CRC：`0x5442`
- 总长度：48 字节

```text
A5 5A 01 02 00 22 00 00 00 64 7B 22 62 75 74 74
6F 6E 22 3A 22 50 41 44 5F 55 50 22 2C 22 73 74
61 74 65 22 3A 22 64 6F 77 6E 22 7D 54 42 0D 0A
```

长按时每 200 ms 再发送一帧 `PAD_UP + down`，每帧使用新的外层 `seq`。

### 11.3 PAD_UP 松开

```json
{"button":"PAD_UP","state":"up"}
```

- `type=0x02`
- `seq=101`
- JSON 长度：`32 / 0x0020`
- CRC：`0x7D13`
- 总长度：46 字节

```text
A5 5A 01 02 00 20 00 00 00 65 7B 22 62 75 74 74
6F 6E 22 3A 22 50 41 44 5F 55 50 22 2C 22 73 74
61 74 65 22 3A 22 75 70 22 7D 7D 13 0D 0A
```

若此 up 在第一次 down 后 180 ms 到达，STM32 判短按；若在 600 ms 到达，判长按。UART 包格式不变。

## 12. 联调验收清单

### 12.1 遥测

1. CRC 标准自检得到 `0x29B1`。
2. 发送 11.1 测试帧，ESP 日志出现 `Complete USART telemetry frame`。
3. 服务器显示 ESP 在线，气体正常、人员未发现，GPS 为 `NA`。
4. 温湿度等字段为 `null` 时只对应字段显示 `NA`。
5. `gas:1` 产生气体报警；恢复后发送 `gas:0`，下一次报警可重新触发。
6. `person_detected:1` 产生疑似幸存者事件并截取当前云端视频帧；恢复后发送 0。
7. GPS 有效时报警事件记录同一帧经纬度。
8. CRC 错误帧、错误帧尾和只有帧头的截断帧都不更新服务器数据。
9. 重复序号不更新数据，新序号恢复更新。
10. 停止 STM32 上报超过 5 秒：ESP 仍在线，全部实时字段显示 `NA`。

### 12.2 控制（STM32 接入 `0x02` 后）

1. PAD_UP down 后调用 STM32 现有 PS2 上键按下逻辑。
2. 按住时每 200 ms 的新 down 只续命，不重复触发按下沿。
3. down 到 up 小于 500 ms 判短按，大于等于 500 ms 判长按。
4. up 立即释放对应 PS2 按键。
5. 连续 500 ms 没有新的 down，自动释放按键并停车。
6. 表中 14 个 PS2 按键逐一验证，名称不得互换。
7. L1/R1 不产生任何摄像头控制。
8. START down 按项目安全逻辑立即急停。
9. 坏 CRC 的控制帧直接丢弃。
10. STM32 或 ESP 重启后全部按键保持释放，电机保持停止。
11. WebSocket 断开、页面失焦、ESP 断网和 L610 断链均在 500 ms 内停车。
12. STM32不发送控制ACK；网页不得显示“STM32已执行”。

## 13. 三端开发责任边界

| 模块 | 必须实现 |
| --- | --- |
| STM32 | 保留 `0x01` 快照组帧；新增 `0x02` button/state解析、短长按计时、PS2处理函数调用和500ms释放看门狗；不发送控制ACK |
| ESP32 | 保留全部已验证功能；仅新增PS2 ESCTL校验和UART `0x02` TX；不等待ACK，AP命令不转发 |
| 云服务器 | PS2按键名和down/up原样封装，不翻译成move/rotate/camera；绑定并校验device/boot/request字段 |
| 前端 | 按下发down，长按每200ms重发down，松开发up；失焦/断连释放全部当前按键；不提供独立摄像头控制 |

云端 `sent` 只表示已发送到ESP，不表示STM32执行。实体运动正式使用前仍应增加WebSocket鉴权。

## 14. 故障定位

ESP 当前遥测日志：

```text
Complete USART telemetry frame ...
USART frame dropped (CRC16 mismatch) ...
USART frame dropped (header received but frame timed out) ...
```

PS2版本ESP日志：

```text
PS2 UART command seq=... button=PAD_UP state=down
```

定位顺序：

1. STM32 TX 是否输出完整 `A5 5A` 帧。
2. 长度是否为 UTF-8 字节数，是否误算了 `\0`。
3. CRC 是否从 version 开始计算。
4. 所有整数是否大端。
5. ESP 是否出现完整帧日志。
6. 服务器是否收到新序号。
7. STM32是否按button/state调用现有PS2处理逻辑。

## 15. v5 固件构建与烧录

本次 PS2 无 ACK 固件使用独立脚本构建：

```text
build_camera_sensor_8fps_q20_ap_local_mjpeg_cloud_ps2_v5.ps1
```

脚本会在独立 staging 目录中使用 8 FPS、JPEG quality 20、AP、本地 MJPEG、云端视频和完整遥测配置，不覆盖此前 v4 产物。公开仓库中的服务器地址、热点密码和 ESP-IDF 路径均为不可部署占位值；请只在私有副本中替换。

公开仓库不提交 `firmware_binaries`、有效 `sdkconfig` 或应用固件 SHA256，因为不同部署配置会生成不同二进制，并可能固化现场地址与口令。本地构建输出目录仍为：

```text
firmware_binaries\camera_sensor_8fps_q20_ap_local_mjpeg_cloud_ps2_v5
```

烧录偏移保持不变：

| 偏移 | 文件 |
| ---: | --- |
| `0x0` | `bootloader.bin` |
| `0x9000` | `partition-table.bin` |
| `0x20000` | `usb_rndis_4g_module.bin` |

Windows 示例（先关闭占用 COM6 的串口工具）：

```powershell
. C:\path\to\esp-idf-v5.5.1\export.ps1
python -m esptool --chip esp32s3 --port COM6 --baud 460800 `
  --before default_reset --after hard_reset write_flash `
  --flash_mode dio --flash_freq 80m --flash_size 4MB `
  0x0 bootloader.bin `
  0x9000 partition-table.bin `
  0x20000 usb_rndis_4g_module.bin
```

当前 v5 已完成 ESP-IDF 5.5.1 全量构建，尚未代替用户烧录，也尚未完成 UART1 与 STM32 的实机联调。

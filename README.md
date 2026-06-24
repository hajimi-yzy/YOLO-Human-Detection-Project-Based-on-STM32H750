# Human Detection Project Based on STM32H750

基于 STM32H750 的嵌入式人物检测实验项目。工程使用 OV5640 摄像头采集画面，通过 DCMI + DMA 写入帧缓冲，在 LCD 上显示实时画面，并使用 X-CUBE-AI 部署由 YOLOv8n 深度剪枝、INT8 量化得到的 NanoV4 96×96 模型完成人物检测与框选显示。当前板端实测约 8 FPS，仍未进行深度优化。

## 功能特性

- STM32H750 主控，启用 I-Cache / D-Cache 与 MPU 基础配置。
- OV5640 摄像头输入，DCMI + DMA 连续采集 RGB565 图像。
- SPI6 驱动 LCD 显示摄像头画面、FPS、AI 状态和检测框。
- X-CUBE-AI 10.2 部署由 YOLOv8n 深度剪枝、INT8 量化得到的 NanoV4 96×96 人物检测模型。
- 当前板端实测约 8 FPS，仍未进行深度优化，后续仍有性能提升空间。
- Anchor-free / FCOS 风格后处理：质量分数、LTRB 距离、局部极大值、NMS。
- 运行期调试变量记录帧计数、LCD 刷新计数、AI 次数、推理耗时和 fault 寄存器。
- `Core/User` 承载摄像头、LCD、AI 调度和后处理逻辑，尽量降低 CubeMX 重新生成时的冲突。

## 硬件环境

| 模块 | 说明 |
| --- | --- |
| MCU | STM32H750XBH 系列工程配置 |
| 摄像头 | OV5640，DCMI 接口 |
| 显示屏 | SPI LCD，工程中使用 SPI6 输出 |
| 调试下载 | Keil MDK + J-Link / ST-Link，按实际板卡连接 |

> 具体引脚、时钟、DMA 和中断配置以 `shiyan002.ioc` 为准。

## 软件环境

- STM32CubeMX / STM32CubeIDE 可用于查看和重新生成工程配置。
- STM32CubeH7 HAL。
- X-CUBE-AI 10.2 / ST Edge AI Core 2.2。
- Keil MDK-ARM，工程文件位于 `MDK-ARM/shiyan002.uvprojx`。
- ARMCC 5.06u7 或与工程配置兼容的 ARM 编译工具链。

## AI 模型信息

生成报告位于 `X-CUBE-AI/App/network_generate_report.txt`，当前模型摘要如下：

| 项目 | 参数 |
| --- | --- |
| 模型来源 | YOLOv8n 深度剪枝 + INT8 量化 |
| 部署模型 | `models/nanov4_96_full_int8.tflite` |
| ONNX 导出 | `models/nanov4_96.onnx` |
| 当前性能 | 板端实测约 8 FPS，未深度优化 |
| 输入 | `int8(1x96x96x3)`，量化 `QLinear(0.003921569, -128)` |
| 输出 | `int8(1x24x24x5)`，量化 `QLinear(0.071815751, -59)` |
| 类别 | person |
| 权重 | 约 48.38 KiB |
| 激活 RAM | 约 87.43 KiB |
| MACC | 约 14.54 M |

后处理实现位于 `Core/User/Src/nanov4_postprocess.c`，检测结果最多保留 `NANOV4_MAX_DETECTIONS` 个目标。

## 工程结构

```text
Core/
  Inc, Src/                 CubeMX 生成的 HAL 初始化与中断代码
  User/Inc, User/Src/       摄像头、LCD、LED、AI 调度、NanoV4 后处理
Drivers/                    STM32 HAL / CMSIS 驱动
Middlewares/ST/AI/          X-CUBE-AI 运行库头文件
X-CUBE-AI/App/              X-CUBE-AI 生成的网络代码和应用层接口
models/                     ONNX 原始导出模型与 TFLite INT8 部署模型
MDK-ARM/                    Keil MDK 工程文件
shiyan002.ioc               CubeMX 工程配置
```

## 运行流程

1. `main.c` 初始化 MPU、Cache、HAL、DCMI、DMA、SPI6、USART1 和 X-CUBE-AI。
2. `App_CameraAi_Init()` 初始化 LED 与 LCD。
3. `App_CameraAi_Start()` 初始化 OV5640，并启动 DCMI DMA 连续采集。
4. 每帧完成后暂停采集，维护 D-Cache，同步帧缓冲并刷新 LCD。
5. AI 模块将当前 RGB565 画面中心裁剪缩放到 96×96，转换为 RGB888 INT8 输入。
6. X-CUBE-AI 执行一次推理，NanoV4 后处理生成检测框。
7. LCD 显示 `AI_OK`、检测数量、人物框和置信度，然后恢复摄像头采集。

## 编译和下载

1. 使用 Keil MDK 打开 `MDK-ARM/shiyan002.uvprojx`。
2. 选择目标 `shiyan002`。
3. 执行 Rebuild，确认无错误后生成固件。
4. 连接调试器和开发板。
5. 使用 Keil Download 或 J-Link/ST-Link 工具下载到板卡。
6. 复位运行，LCD 应显示摄像头画面、FPS、`AI_OK` 和人物检测结果。

如需修改外设配置，建议先编辑 `shiyan002.ioc`，再通过 CubeMX 重新生成，并确认 `Core/User` 中的用户代码仍被工程包含。

## 调试变量

工程保留了一组便于在线观察的全局变量，定义在 `Core/User/Src/app_camera_ai.c`：

| 变量 | 含义 |
| --- | --- |
| `g_dbg_stage` | 当前运行阶段标记 |
| `g_dbg_loop_count` | 主循环轮询计数 |
| `g_dbg_frame_count` | 摄像头帧计数 |
| `g_dbg_lcd_count` | LCD 刷新计数 |
| `g_dbg_ai_count` | AI 成功处理计数 |
| `g_dbg_ai_dt` | 单次 AI 处理耗时，单位为 HAL tick |
| `g_dbg_ai_result` | 最近一次 AI 处理结果 |
| `g_dbg_fault_*` | HardFault / MemManage / BusFault 等 fault 寄存器快照 |
| `g_ai_enable` | 是否启用 AI 处理 |
| `g_ai_period` | AI 处理周期，按帧计数 |

## 注意事项

- 本项目包含 STM32 HAL、X-CUBE-AI 生成代码和 ST 中间件文件，请遵守对应软件包许可。
- 模型文件已随仓库放在 `models/` 中；重新生成 X-CUBE-AI 代码时，可使用 `models/nanov4_96_full_int8.tflite` 或按需从 `models/nanov4_96.onnx` 重新转换。
- D-Cache 对 DCMI DMA 帧缓冲有影响，修改帧缓冲或 DMA 流程时需要同步检查 cache clean / invalidate 操作。
- 当前实测性能约 8 FPS，尚未针对模型结构、内存访问、LCD 刷新、摄像头流水线等方向做深度优化。

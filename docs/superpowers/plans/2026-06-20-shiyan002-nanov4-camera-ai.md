# shiyan002 NanoV4 Camera AI Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Build an internal-flash STM32H750 application that continuously displays OV5640 video and overlays NanoV4 96x96 INT8 person detections without first-frame lockup.

**Architecture:** Keep CubeMX-generated startup and peripheral files thin, port the verified `shiyan001` camera/LCD runtime into `Core/User`, and expose inference through a request gate. Replace the incompatible anchor decoder with a small FCOS-style NanoV4 decoder whose tensor contract is checked against the generated X-CUBE-AI report.

**Tech Stack:** STM32H750, STM32CubeH7 HAL, X-CUBE-AI 10.2, ARMCC 5.06u7, OV5640/DCMI DMA, SPI6 LCD, PowerShell/Python structural tests, J-Link.

---

The target directory is `C:\Users\62629\Desktop\supermini\shiyan\shiyan002\shiyan002`. The verified reference is `C:\Users\62629\Desktop\supermini\shiyan\shiyan001`. The target is not currently a Git repository, so this plan records verification checkpoints instead of creating commits.

### Task 1: Establish failing regeneration and model-contract checks

**Files:**
- Create: `tools/check_regen_boundary.ps1`
- Create: `tests/test_nanov4_contract.py`

- [ ] **Step 1: Add the structural boundary check**

Create a PowerShell check that requires:

```powershell
$required = @(
  'Core/User/Inc/app_camera_ai.h',
  'Core/User/Src/app_camera_ai.c',
  'Core/User/Inc/nanov4_postprocess.h',
  'Core/User/Src/nanov4_postprocess.c'
)

$main = Get-Content 'Core/Src/main.c' -Raw
$ai = Get-Content 'X-CUBE-AI/App/app_x-cube-ai.c' -Raw

$required | ForEach-Object {
  if (-not (Test-Path $_)) { throw "Missing boundary file: $_" }
}
if ($main -notmatch '#define MX_X_CUBE_AI_Process App_CameraAi_ProcessHook') {
  throw 'main.c does not redirect the generated AI process hook'
}
if ($main -match 'Camera_Buffer|LCD_CopyBuffer|OV5640_DCMI_(Suspend|Resume)') {
  throw 'main.c owns camera runtime work'
}
if ($ai -notmatch 'AI_RequestProcess' -or $ai -notmatch 'AI_ProcessPendingRequest') {
  throw 'AI request gate is missing'
}
Write-Host 'Boundary check passed.'
```

- [ ] **Step 2: Run the boundary check and verify RED**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File tools/check_regen_boundary.ps1
```

Expected: failure naming `Core/User/Inc/app_camera_ai.h` as missing.

- [ ] **Step 3: Add the model-contract test**

Create a Python test that reads `network_generate_report.txt` and asserts the exact deployed contract:

```python
from pathlib import Path


def test_generated_network_matches_nanov4_contract() -> None:
    report = Path('X-CUBE-AI/App/network_generate_report.txt').read_text(
        encoding='utf-8', errors='replace'
    )
    assert "int8(1x96x96x3)" in report
    assert "QLinear(0.003921569,-128,int8)" in report
    assert "int8(1x24x24x5)" in report
    assert "QLinear(0.071815751,-59,int8)" in report


def test_decoder_source_declares_fcos_contract() -> None:
    source = Path('Core/User/Src/nanov4_postprocess.c').read_text(encoding='utf-8')
    assert 'NANOV4_STRIDE          (4.0f)' in source
    assert 'NANOV4_OUTPUT_SCALE    (0.071815751f)' in source
    assert 'NANOV4_OUTPUT_ZERO     (-59)' in source
    assert 'NanoV4_LocalMaximum' in source
    assert 'NanoV4_Nms' in source
```

- [ ] **Step 4: Run the model-contract test and verify RED**

Run:

```powershell
python -m pytest tests/test_nanov4_contract.py -q
```

Expected: the generated-network test passes and the decoder-source test fails because `nanov4_postprocess.c` does not exist.

### Task 2: Port the verified camera/LCD hardware layer

**Files:**
- Create: `Core/User/Inc/app_camera_ai.h`
- Create: `Core/User/Src/app_camera_ai.c`
- Create: `Core/User/Inc/dcmi_ov5640.h`
- Create: `Core/User/Inc/dcmi_ov5640_cfg.h`
- Create: `Core/User/Src/dcmi_ov5640.c`
- Create: `Core/User/Inc/sccb.h`
- Create: `Core/User/Src/sccb.c`
- Create: `Core/User/Inc/lcd_spi_200.h`
- Create: `Core/User/Src/lcd_spi_200.c`
- Create: `Core/User/Inc/lcd_fonts.h`
- Create: `Core/User/Src/lcd_fonts.c`
- Create: `Core/User/Inc/lcd_image.h`
- Create: `Core/User/Src/lcd_image.c`
- Create: `Core/User/Inc/led.h`
- Create: `Core/User/Src/led.c`
- Create: `Core/User/Inc/usart.h`
- Create: `Core/User/Src/usart.c`
- Modify: `Core/Src/main.c`
- Modify: `Core/Src/stm32h7xx_it.c`
- Modify: `MDK-ARM/shiyan002.uvprojx`
- Modify: `MDK-ARM/shiyan002.uvoptx`

- [ ] **Step 1: Port reference hardware files without changing their behavior**

Apply the exact contents of the corresponding `shiyan001/Core/User` files to the target. Preserve the target's generated HAL files and target model files. The public runtime interface remains:

```c
void App_CameraAi_Init(void);
void App_CameraAi_Start(void);
void App_CameraAi_Poll(void);
void App_CameraAi_ProcessHook(void);
void Debug_SetStage(uint32_t stage);
```

- [ ] **Step 2: Thin main.c at USER CODE boundaries**

Add the user include and redirect:

```c
#include "app_camera_ai.h"
#define MX_X_CUBE_AI_Process App_CameraAi_ProcessHook
```

After `MX_X_CUBE_AI_Init()` call:

```c
App_CameraAi_Init();
App_CameraAi_Start();
```

Inside the generated `while (1)` loop:

```c
MX_X_CUBE_AI_Process();
App_CameraAi_Poll();
```

- [ ] **Step 3: Restore fault diagnostics through the user boundary**

Include `app_camera_ai.h` from `stm32h7xx_it.c` and preserve the reference fault register capture. Do not introduce duplicate `extern` declarations for the debug globals.

- [ ] **Step 4: Add user modules and include path to the Keil project**

Add `..\Core\User\Inc` to the C include path and list each `Core/User/Src/*.c` file in one `Core/User` project group. Keep the target name and output directory as `shiyan002`.

- [ ] **Step 5: Run the boundary check**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File tools/check_regen_boundary.ps1
```

Expected: it advances past missing camera files and fails only because NanoV4 postprocessing or the AI request gate has not yet been added.

### Task 3: Implement and verify the NanoV4 FCOS decoder

**Files:**
- Create: `Core/User/Inc/nanov4_postprocess.h`
- Create: `Core/User/Src/nanov4_postprocess.c`
- Extend: `tests/test_nanov4_contract.py`
- Modify: `MDK-ARM/shiyan002.uvprojx`

- [ ] **Step 1: Extend the failing test with source-level behavior invariants**

Add assertions for the deployed formulas:

```python
def test_decoder_source_uses_training_decode_formulas() -> None:
    source = Path('Core/User/Src/nanov4_postprocess.c').read_text(encoding='utf-8')
    assert '(raw - NANOV4_OUTPUT_ZERO) * NANOV4_OUTPUT_SCALE' in source
    assert '((float)x + 0.5f)' in source
    assert '((float)y + 0.5f)' in source
    assert 'NanoV4_Sigmoid' in source
    assert 'NanoV4_LocalMaximum' in source
    assert 'NanoV4_Nms' in source
```

Run `python -m pytest tests/test_nanov4_contract.py -q` and confirm failure due to the absent decoder.

- [ ] **Step 2: Define the portable decoder API**

Use this public interface:

```c
#define NANOV4_MAX_DETECTIONS 5U

typedef struct {
  int16_t x1;
  int16_t y1;
  int16_t x2;
  int16_t y2;
  uint16_t score_x100;
} NanoV4_Box;

uint32_t NanoV4_Decode(const int8_t *output,
                       NanoV4_Box *boxes,
                       uint32_t capacity,
                       float score_threshold,
                       float nms_iou);
```

- [ ] **Step 3: Implement the exact NanoV4 decode math**

Implement constants and indexing for NHWC `24x24x5`:

```c
#define NANOV4_GRID_W          (24U)
#define NANOV4_GRID_H          (24U)
#define NANOV4_CHANNELS        (5U)
#define NANOV4_STRIDE          (4.0f)
#define NANOV4_OUTPUT_SCALE    (0.071815751f)
#define NANOV4_OUTPUT_ZERO     (-59)
#define NANOV4_MAX_CANDIDATES  (32U)

static float NanoV4_Dequantize(int8_t raw)
{
  return (raw - NANOV4_OUTPUT_ZERO) * NANOV4_OUTPUT_SCALE;
}
```

For each local score maximum above threshold, dequantize LTRB channels, clamp distances to `[-2, 24]`, and compute:

```c
cx = ((float)x + 0.5f);
cy = ((float)y + 0.5f);
x1 = (cx - left) * NANOV4_STRIDE;
y1 = (cy - top) * NANOV4_STRIDE;
x2 = (cx + right) * NANOV4_STRIDE;
y2 = (cy + bottom) * NANOV4_STRIDE;
```

Clip coordinates to `[0, 95]`, reject boxes smaller than two pixels, sort by score, apply IoU NMS, and return no more than `capacity` boxes.

- [ ] **Step 4: Add the decoder to the Keil project and verify GREEN**

Run:

```powershell
python -m pytest tests/test_nanov4_contract.py -q
```

Expected: all model-contract and formula tests pass.

### Task 4: Integrate preprocessing, inference scheduling, and overlay drawing

**Files:**
- Modify: `X-CUBE-AI/App/app_x-cube-ai.h`
- Modify: `X-CUBE-AI/App/app_x-cube-ai.c`
- Modify: `Core/User/Src/app_camera_ai.c`

- [ ] **Step 1: Add the failing request-gate expectations**

Extend `tools/check_regen_boundary.ps1` to require these declarations in `app_x-cube-ai.h`:

```c
void AI_SetCameraFrame(uint16_t *frame, uint16_t width, uint16_t height);
void AI_RequestProcess(void);
int AI_ProcessPendingRequest(void);
```

Run the boundary check and confirm it fails because the target generated header does not contain these APIs.

- [ ] **Step 2: Add frame and request state**

In X-CUBE-AI USER CODE sections, add:

```c
static uint16_t *s_camera_frame;
static uint16_t s_camera_width;
static uint16_t s_camera_height;
static volatile uint8_t s_ai_process_requested;

void AI_SetCameraFrame(uint16_t *frame, uint16_t width, uint16_t height)
{
  s_camera_frame = frame;
  s_camera_width = width;
  s_camera_height = height;
}

void AI_RequestProcess(void) { s_ai_process_requested = 1U; }
```

- [ ] **Step 3: Implement RGB565-to-INT8 preprocessing**

Center-crop the camera image and nearest-neighbor scale it to the generated input dimensions. Preserve the reference camera orientation mapping and convert pixels with:

```c
uint8_t r = (uint8_t)(((pixel >> 11) & 0x1FU) * 255U / 31U);
uint8_t g = (uint8_t)(((pixel >> 5) & 0x3FU) * 255U / 63U);
uint8_t b = (uint8_t)((pixel & 0x1FU) * 255U / 31U);
data[dst + 0U] = (int8_t)((int16_t)r - 128);
data[dst + 1U] = (int8_t)((int16_t)g - 128);
data[dst + 2U] = (int8_t)((int16_t)b - 128);
```

- [ ] **Step 4: Replace generated endless processing with one requested inference**

`AI_ProcessPendingRequest()` must return immediately when no request exists, clear the flag only when processing begins, run exactly one preprocess/inference/postprocess cycle, and return its status. `MX_X_CUBE_AI_Process()` must call this function and must not contain a `do/while` inference loop.

- [ ] **Step 5: Decode and draw person boxes**

Call `NanoV4_Decode(data_outs[0], boxes, NANOV4_MAX_DETECTIONS, threshold, 0.7f)`. Map model coordinates through the same center crop used in preprocessing, compensate for camera rotation, draw rectangles and confidence values after the raw frame copy, and render `AI_OK` after successful model bootstrap.

- [ ] **Step 6: Verify the boundary**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File tools/check_regen_boundary.ps1
python -m pytest tests/test_nanov4_contract.py -q
```

Expected: boundary check passes and all tests pass.

### Task 5: Clean Keil build and static memory verification

**Files:**
- Inspect: `MDK-ARM/shiyan002/shiyan002.build_log.htm`
- Inspect: `MDK-ARM/shiyan002/shiyan002.map`

- [ ] **Step 1: Perform a clean target rebuild**

Run:

```powershell
& 'D:\Keil_v5\UV4\UV4.exe' -b 'C:\Users\62629\Desktop\supermini\shiyan\shiyan002\shiyan002\MDK-ARM\shiyan002.uvprojx' -t shiyan002 -j0
```

Expected build-log ending: `0 Error(s), 0 Warning(s)` and a fresh `shiyan002.axf` plus `shiyan002.hex`.

- [ ] **Step 2: Verify the linked ownership and memory fit**

Search the map for `App_CameraAi_Poll`, `NanoV4_Decode`, `AI_ProcessPendingRequest`, and the camera frame buffer. Confirm the image fits internal flash and SRAM and that there are no unresolved or duplicate runtime symbols.

- [ ] **Step 3: Re-run all host checks after the build**

Run:

```powershell
powershell -ExecutionPolicy Bypass -File tools/check_regen_boundary.ps1
python -m pytest tests/test_nanov4_contract.py -q
```

Expected: both commands pass after the final generated-project edits.

### Task 6: J-Link download and live target diagnosis

**Files:**
- Create runtime artifact: `MDK-ARM/jlink_connect.log`
- Create runtime artifact: `MDK-ARM/jlink_flash.log`
- Inspect: `MDK-ARM/shiyan002/shiyan002.axf`

- [ ] **Step 1: Verify SWD connectivity under reset**

Use `D:\Keil_v5\ARM\Segger\JLink.exe` with device `STM32H750XB`, interface SWD, 4 MHz initial speed, and connect-under-reset if normal attach fails. Expected: Cortex-M7 core and STM32H750 device are identified without JTAG-chain errors.

- [ ] **Step 2: Download the verified HEX and reset**

Load `MDK-ARM\shiyan002\shiyan002.hex`, verify flash contents, reset, and run. Save the complete J-Link transcript.

- [ ] **Step 3: Inspect live diagnostic globals**

Read `g_dbg_stage`, `g_dbg_frame_count`, `g_dbg_lcd_count`, `g_dbg_ai_count`, `g_dbg_px`, `g_dbg_sum`, `g_dbg_ai_dt`, and all fault registers at two separated samples. Expected: frame/LCD/AI counters and frame statistics change, stage cycles through the running states, and fault registers remain zero.

- [ ] **Step 4: Diagnose at the first failing boundary if needed**

If the target stalls, use the last stage and counters to localize the fault before changing code:

```text
0x0160-0x0170: OV5640 initialization or DMA start
0x0210-0x0212: frame complete, DCMI suspend, or cache invalidation
0x0220: LCD transfer
0x0230-0x0231: preprocessing/inference/postprocessing
0x0240: DCMI resume
```

Add a regression assertion or diagnostic for the identified boundary before applying a fix, rebuild, flash, and repeat the same two-sample check.

### Task 7: Reset, cold-start, and visual acceptance

**Files:**
- Update: `docs/superpowers/specs/2026-06-20-shiyan002-nanov4-camera-ai-design.md` only if measured behavior requires a documented hardware correction

- [ ] **Step 1: Verify reset behavior**

Press reset or issue a J-Link reset and confirm the LCD reaches continuous camera video and AI inference without requiring a second reset.

- [ ] **Step 2: Verify cold power-up behavior**

Power-cycle the board with J-Link connected and disconnected. Confirm both cases reach continuous operation and remain responsive for at least two minutes.

- [ ] **Step 3: Verify person detection visually**

Place a person in view and confirm boxes track motion, confidence changes, FPS remains live, and the camera frame does not freeze while AI runs.

- [ ] **Step 4: Run final fresh verification**

Run the boundary check, Python tests, clean Keil build, J-Link reconnect/download verification, and two-sample runtime counter check again. Record exact pass/fail evidence; do not mark the task complete from visual appearance alone.

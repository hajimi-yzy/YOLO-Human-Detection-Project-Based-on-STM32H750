# shiyan002 NanoV4 Camera AI Design

## Goal

Make `shiyan002` provide the same camera, LCD, runtime diagnostics, and live detection overlay as the verified `shiyan001` project while using `nanov4_96_full_int8.tflite` and retaining a CubeMX-regeneration-safe structure.

## Verified Model Contract

- Input: one NHWC `int8` tensor, `1x96x96x3`.
- Input quantization: scale `1/255`, zero point `-128`.
- Output: one NHWC `int8` tensor, `1x24x24x5`.
- Output quantization: scale `0.071815751`, zero point `-59`.
- Output channel order: quality logit, left, top, right, bottom.
- Detector stride: 4 pixels.
- Class count: one, `person`.

The output is an anchor-free FCOS-style head. The anchor-based decoder from `shiyan001` is not compatible and will not be copied.

## Architecture

CubeMX-generated startup and peripheral files remain thin. A user-owned `app_camera_ai` module owns the camera frame buffer, OV5640 start/stop sequence, D-cache maintenance, LCD refresh, frame counters, AI scheduling, and fault-stage diagnostics. Generated `main.c` only initializes peripherals and calls the user module's init, start, and poll entry points.

The X-CUBE-AI application layer owns model initialization, image preprocessing, inference, NanoV4 postprocessing, and detection overlay drawing. Inference is triggered through the request/pending-request interface used by `shiyan001`, preventing generated main-loop calls from starting an uncontrolled inference loop.

## Frame And Inference Flow

1. DCMI DMA continuously captures RGB565 frames into a 32-byte-aligned LCD-sized buffer.
2. On a complete frame, capture is suspended and the frame buffer's D-cache range is invalidated.
3. The current frame is supplied to the AI layer and copied to the LCD.
4. The AI layer center-crops the frame to a square and scales it to `96x96`.
5. RGB565 is converted to RGB888. Because input quantization represents `RGB/255`, each RGB byte is converted to signed INT8 by subtracting 128.
6. X-CUBE-AI runs one inference.
7. Each output element is dequantized with `(raw + 59) * 0.071815751`.
8. Quality uses sigmoid and 3x3 local-maximum suppression. Candidate boxes use `(cell + 0.5)` centers and LTRB distances multiplied by stride 4.
9. Invalid and low-score boxes are discarded, candidates are sorted, NMS is applied, and at most five person boxes are retained.
10. Boxes are mapped from the `96x96` crop back to LCD coordinates and drawn with confidence values.
11. DCMI capture resumes.

## Hardware And CubeMX Policy

The current `shiyan002` pin mapping already matches the relevant `shiyan001` mapping, including DCMI, DMA1 Stream 0, SPI6, PA15, PG13, and PG14. SPI6 is already configured for 8-bit, 60 Mbit/s output with very-high-speed GPIO.

The initial implementation will preserve the `shiyan002.ioc` clock tree and generated peripheral setup. Clock, polarity, DMA, or interrupt settings will only be changed if build or board evidence identifies a concrete mismatch. Any required CubeMX correction will be made in the `.ioc` as well as generated code so regeneration remains safe.

## Diagnostics And Failure Handling

- Preserve the `AI_OK`, FPS, frame pixel, frame sum, frame count, LCD count, AI count, and AI duration diagnostics.
- Preserve stage markers around camera suspension, cache invalidation, LCD transfer, inference, and camera resume.
- Preserve fault register capture for CFSR, HFSR, DFSR, AFSR, BFAR, MMFAR, and SHCSR.
- AI initialization and inference errors must leave a visible stage/error state instead of silently blocking in the first frame.
- Camera frame counters and checksums must continue changing while AI is enabled.

## Verification

Verification proceeds in increasing hardware scope:

1. A structural boundary check verifies generated files do not own the frame loop or camera buffer.
2. Host-side decoder tests compare the C-compatible NanoV4 decode math against the Python reference for synthetic tensors, including local maxima, LTRB coordinates, clipping, thresholding, and NMS.
3. Keil performs a clean rebuild with zero errors and zero warnings.
4. J-Link downloads the internal-flash image and reconnects after reset.
5. Live debugger checks confirm frame, LCD, and AI counters advance and fault registers remain clear.
6. The LCD shows changing OV5640 video, FPS and AI status, and stable person boxes for a visible person.
7. A reset and cold power-up both reach continuous operation without first-frame lockup.

## Scope

This work does not retrain or alter the TFLite model, add new object classes, move execution to external flash, or redesign the existing LCD interface. The result remains an internal-flash STM32H750 project based on the new `shiyan002` CubeMX configuration.

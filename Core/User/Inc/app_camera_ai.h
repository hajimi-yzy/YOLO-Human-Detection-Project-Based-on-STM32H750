#ifndef __APP_CAMERA_AI_H
#define __APP_CAMERA_AI_H

#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

extern volatile uint32_t g_dbg_magic;
extern volatile uint32_t g_dbg_stage;
extern volatile uint32_t g_dbg_loop_count;
extern volatile uint32_t g_dbg_frame_count;
extern volatile uint32_t g_dbg_lcd_count;
extern volatile uint32_t g_dbg_ai_count;
extern volatile uint32_t g_dbg_px;
extern volatile uint32_t g_dbg_sum;
extern volatile uint32_t g_dbg_ai_tick0;
extern volatile uint32_t g_dbg_ai_tick1;
extern volatile uint32_t g_dbg_ai_dt;
extern volatile int32_t g_dbg_ai_result;
extern volatile uint32_t g_dbg_fault_cfsr;
extern volatile uint32_t g_dbg_fault_hfsr;
extern volatile uint32_t g_dbg_fault_dfsr;
extern volatile uint32_t g_dbg_fault_afsr;
extern volatile uint32_t g_dbg_fault_bfar;
extern volatile uint32_t g_dbg_fault_mmfar;
extern volatile uint32_t g_dbg_fault_shcsr;
extern volatile uint32_t g_ai_enable;
extern volatile uint32_t g_ai_period;

void Debug_SetStage(uint32_t stage);

void App_CameraAi_Init(void);
void App_CameraAi_Start(void);
void App_CameraAi_Poll(void);
void App_CameraAi_ProcessHook(void);

#ifdef __cplusplus
}
#endif

#endif /* __APP_CAMERA_AI_H */

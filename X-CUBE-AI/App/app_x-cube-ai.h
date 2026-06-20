
/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __APP_AI_H
#define __APP_AI_H
#ifdef __cplusplus
extern "C" {
#endif
/**
  ******************************************************************************
  * @file    app_x-cube-ai.h
  * @author  X-CUBE-AI C code generator
  * @brief   AI entry function definitions
  ******************************************************************************
  * @attention
  *
  * Copyright (c) 2026 STMicroelectronics.
  * All rights reserved.
  *
  * This software is licensed under terms that can be found in the LICENSE file
  * in the root directory of this software component.
  * If no LICENSE file comes with this software, it is provided AS-IS.
  *
  ******************************************************************************
  */
/* Includes ------------------------------------------------------------------*/
#include "ai_platform.h"

void MX_X_CUBE_AI_Init(void);
void MX_X_CUBE_AI_Process(void);
/* USER CODE BEGIN includes */
#include <stdint.h>
extern volatile uint32_t g_ai_ready;
extern volatile uint32_t g_ai_selftest_status;
extern volatile uint32_t g_ai_error_type;
extern volatile uint32_t g_ai_error_code;
extern volatile int32_t g_ai_last_result;
void AI_SetCameraFrame(uint16_t *frame, uint16_t width, uint16_t height);
void AI_RequestProcess(void);
int AI_ProcessPendingRequest(void);
int AI_IsReady(void);
/* USER CODE END includes */
#ifdef __cplusplus
}
#endif
#endif /*__STMicroelectronics_X-CUBE-AI_10_2_0_H */

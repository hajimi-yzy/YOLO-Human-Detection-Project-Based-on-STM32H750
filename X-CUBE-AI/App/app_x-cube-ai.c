
/**
  ******************************************************************************
  * @file    app_x-cube-ai.c
  * @author  X-CUBE-AI C code generator
  * @brief   AI program body
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

 /*
  * Description
  *   v1.0 - Minimum template to show how to use the Embedded Client API
  *          model. Only one input and one output is supported. All
  *          memory resources are allocated statically (AI_NETWORK_XX, defines
  *          are used).
  *          Re-target of the printf function is out-of-scope.
  *   v2.0 - add multiple IO and/or multiple heap support
  *
  *   For more information, see the embeded documentation:
  *
  *       [1] %X_CUBE_AI_DIR%/Documentation/index.html
  *
  *   X_CUBE_AI_DIR indicates the location where the X-CUBE-AI pack is installed
  *   typical : C:\Users\[user_name]\STM32Cube\Repository\STMicroelectronics\X-CUBE-AI\7.1.0
  */

#ifdef __cplusplus
 extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/

#if defined ( __ICCARM__ )
#elif defined ( __CC_ARM ) || ( __GNUC__ )
#endif

/* System headers */
#include <stdint.h>
#include <stdlib.h>
#include <inttypes.h>
#include <string.h>

#include "app_x-cube-ai.h"
#include "main.h"
#include "ai_datatypes_defines.h"
#include "network.h"
#include "network_data.h"

/* USER CODE BEGIN includes */
#include <math.h>
#include "app_camera_ai.h"
#include "lcd_spi_200.h"
#include "nanov4_postprocess.h"
/* USER CODE END includes */

/* IO buffers ----------------------------------------------------------------*/

#if !defined(AI_NETWORK_INPUTS_IN_ACTIVATIONS)
AI_ALIGNED(4) ai_i8 data_in_1[AI_NETWORK_IN_1_SIZE_BYTES];
ai_i8* data_ins[AI_NETWORK_IN_NUM] = {
data_in_1
};
#else
ai_i8* data_ins[AI_NETWORK_IN_NUM] = {
NULL
};
#endif

#if !defined(AI_NETWORK_OUTPUTS_IN_ACTIVATIONS)
AI_ALIGNED(4) ai_i8 data_out_1[AI_NETWORK_OUT_1_SIZE_BYTES];
ai_i8* data_outs[AI_NETWORK_OUT_NUM] = {
data_out_1
};
#else
ai_i8* data_outs[AI_NETWORK_OUT_NUM] = {
NULL
};
#endif

/* Activations buffers -------------------------------------------------------*/

AI_ALIGNED(32)
static uint8_t pool0[AI_NETWORK_DATA_ACTIVATION_1_SIZE];

ai_handle data_activations0[] = {pool0};

/* AI objects ----------------------------------------------------------------*/

static ai_handle network = AI_HANDLE_NULL;

static ai_buffer* ai_input;
static ai_buffer* ai_output;

static void AI_CleanDebugWord(volatile const void *address)
{
  if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U) {
    uint32_t start = ((uint32_t)address) & ~31U;
    SCB_CleanDCache_by_Addr((uint32_t *)start, 32);
  }
}

static void AI_DebugFlush(void)
{
  AI_CleanDebugWord(&g_ai_ready);
  AI_CleanDebugWord(&g_ai_selftest_status);
  AI_CleanDebugWord(&g_ai_error_type);
  AI_CleanDebugWord(&g_ai_error_code);
  AI_CleanDebugWord(&g_ai_last_result);
  __DSB();
  __ISB();
}

static void ai_log_err(const ai_error err, const char *fct)
{
  /* USER CODE BEGIN log */
  (void)fct;
  g_ai_error_type = err.type;
  g_ai_error_code = err.code;
  g_ai_ready = 0U;
  AI_DebugFlush();
  Debug_SetStage(0xA1E0U);
  /* USER CODE END log */
}

static int ai_boostrap(ai_handle *act_addr)
{
  ai_error err;

  /* Create and initialize an instance of the model */
  err = ai_network_create_and_init(&network, act_addr, NULL);
  if (err.type != AI_ERROR_NONE) {
    ai_log_err(err, "ai_network_create_and_init");
    return -1;
  }

  ai_input = ai_network_inputs_get(network, NULL);
  ai_output = ai_network_outputs_get(network, NULL);

#if defined(AI_NETWORK_INPUTS_IN_ACTIVATIONS)
  /*  In the case where "--allocate-inputs" option is used, memory buffer can be
   *  used from the activations buffer. This is not mandatory.
   */
  for (int idx=0; idx < AI_NETWORK_IN_NUM; idx++) {
	data_ins[idx] = ai_input[idx].data;
  }
#else
  for (int idx=0; idx < AI_NETWORK_IN_NUM; idx++) {
	  ai_input[idx].data = data_ins[idx];
  }
#endif

#if defined(AI_NETWORK_OUTPUTS_IN_ACTIVATIONS)
  /*  In the case where "--allocate-outputs" option is used, memory buffer can be
   *  used from the activations buffer. This is no mandatory.
   */
  for (int idx=0; idx < AI_NETWORK_OUT_NUM; idx++) {
	data_outs[idx] = ai_output[idx].data;
  }
#else
  for (int idx=0; idx < AI_NETWORK_OUT_NUM; idx++) {
	ai_output[idx].data = data_outs[idx];
  }
#endif

  return 0;
}

static int ai_run(void)
{
  ai_i32 batch;

  batch = ai_network_run(network, ai_input, ai_output);
  if (batch != 1) {
    ai_log_err(ai_network_get_error(network),
        "ai_network_run");
    return -1;
  }

  return 0;
}

/* USER CODE BEGIN 2 */
#define AI_INPUT_W   (AI_NETWORK_IN_1_WIDTH)
#define AI_INPUT_H   (AI_NETWORK_IN_1_HEIGHT)

static uint16_t *s_camera_frame = NULL;
static uint16_t s_camera_width = 0U;
static uint16_t s_camera_height = 0U;
static volatile uint8_t s_ai_process_requested = 0U;
static volatile int32_t s_ai_last_result = 0;

volatile uint32_t g_ai_ready = 0U;
volatile uint32_t g_ai_selftest_status = 0U;
volatile uint32_t g_ai_error_type = 0U;
volatile uint32_t g_ai_error_code = 0U;
volatile int32_t g_ai_last_result = 0;




void AI_SetCameraFrame(uint16_t *frame, uint16_t width, uint16_t height)
{
  s_camera_frame = frame;
  s_camera_width = width;
  s_camera_height = height;
}

void AI_RequestProcess(void)
{
  s_ai_process_requested = 1U;
}

int AI_IsReady(void)
{
  return (g_ai_ready != 0U) ? 1 : 0;
}

int AI_ProcessPendingRequest(void)
{
  if (!network) {
    g_ai_last_result = -1;
    AI_DebugFlush();
    return -1;
  }

  if (s_ai_process_requested == 0U) {
    return 0;
  }

  s_ai_last_result = 0;
  MX_X_CUBE_AI_Process();
  g_ai_last_result = s_ai_last_result;
  AI_DebugFlush();
  return (int)s_ai_last_result;
}

static uint8_t Rgb565ToR8(uint16_t pixel)
{
  uint8_t r = (uint8_t)((pixel >> 11) & 0x1FU);
  return (uint8_t)((r << 3) | (r >> 2));
}

static uint8_t Rgb565ToG8(uint16_t pixel)
{
  uint8_t g = (uint8_t)((pixel >> 5) & 0x3FU);
  return (uint8_t)((g << 2) | (g >> 4));
}

static uint8_t Rgb565ToB8(uint16_t pixel)
{
  uint8_t b = (uint8_t)(pixel & 0x1FU);
  return (uint8_t)((b << 3) | (b >> 2));
}

static ai_i8 QuantizeU8ToS8(uint8_t value)
{
  return (ai_i8)((int32_t)value - 128);
}

static int PreprocessCameraFrame(ai_i8 *dst)
{
  if ((dst == NULL) || (s_camera_frame == NULL) ||
      (s_camera_width == 0U) || (s_camera_height == 0U)) {
    return -1;
  }

  uint16_t crop = (s_camera_width < s_camera_height) ? s_camera_width : s_camera_height;
  uint16_t x0 = (uint16_t)((s_camera_width - crop) / 2U);
  uint16_t y0 = (uint16_t)((s_camera_height - crop) / 2U);

  for (uint16_t y = 0U; y < AI_INPUT_H; y++) {
    for (uint16_t x = 0U; x < AI_INPUT_W; x++) {
      uint32_t src_x = (uint32_t)x0 + (((uint32_t)x * crop) / AI_INPUT_W);
      uint32_t src_y = (uint32_t)y0 + (((uint32_t)y * crop) / AI_INPUT_H);

      if (src_x >= s_camera_width) {
        src_x = (uint32_t)s_camera_width - 1U;
      }
      if (src_y >= s_camera_height) {
        src_y = (uint32_t)s_camera_height - 1U;
      }

      uint16_t pixel = s_camera_frame[(src_y * s_camera_width) + src_x];
      uint32_t dst_idx = (((uint32_t)y * AI_INPUT_W) + x) * 3U;

      dst[dst_idx + 0U] = QuantizeU8ToS8(Rgb565ToR8(pixel));
      dst[dst_idx + 1U] = QuantizeU8ToS8(Rgb565ToG8(pixel));
      dst[dst_idx + 2U] = QuantizeU8ToS8(Rgb565ToB8(pixel));
    }
  }

  return 0;
}

static void DrawBoxesOnLCD(const NanoV4_Box *boxes, uint32_t count)
{
  LCD_SetColor((count > 0U) ? LCD_YELLOW : LCD_CYAN);
  LCD_SetBackColor(LCD_BLACK);
  LCD_DisplayString(0, 260, (char *)"AI N:");
  LCD_DisplayNumber(42, 260, (int32_t)count, 2);

  LCD_SetColor(LCD_GREEN);
  LCD_DisplayString(0, 280, (char *)"AI_OK");
  LCD_SetColor((count > 0U) ? LCD_YELLOW : LCD_CYAN);
  for (uint32_t i = 0U; i < count; i++) {
    uint16_t label_y = (boxes[i].y1 > 18) ? (uint16_t)(boxes[i].y1 - 18)
                                          : (uint16_t)(boxes[i].y2 + 2);
    if (label_y > 300U) {
      label_y = 300U;
    }

    LCD_DrawRect((uint16_t)boxes[i].x1, (uint16_t)boxes[i].y1,
                 (uint16_t)(boxes[i].x2 - boxes[i].x1),
                 (uint16_t)(boxes[i].y2 - boxes[i].y1));
    LCD_DisplayString((uint16_t)boxes[i].x1, label_y, (char *)"P");
    LCD_DisplayNumber((uint16_t)(boxes[i].x1 + 14), label_y,
                      (int32_t)boxes[i].score_x100, 3);
  }

  LCD_SetColor(LCD_WHITE);
  LCD_SetBackColor(LCD_BLACK);
}

static int PostProcessNanoV4(const ai_i8 *out)
{
  NanoV4_Box boxes[NANOV4_MAX_DETECTIONS];

  if (out == NULL) {
    return -1;
  }

  uint32_t count = NanoV4_Decode(out, boxes, NANOV4_MAX_DETECTIONS, 0.35f, 0.40f);

  /* Map model coordinates (96x96 crop) back to LCD coordinates */
  uint16_t crop = (s_camera_width < s_camera_height) ? s_camera_width : s_camera_height;
  int16_t x0 = (int16_t)((s_camera_width - crop) / 2U);
  int16_t y0 = (int16_t)((s_camera_height - crop) / 2U);

  for (uint32_t i = 0U; i < count; i++) {
    int32_t mx;

    mx = x0 + (int32_t)((boxes[i].x1 * (float)crop) / 96.0f);
    if (mx < 0) mx = 0;
    if (mx > (int32_t)(s_camera_width - 1U)) mx = (int32_t)(s_camera_width - 1U);
    boxes[i].x1 = (int16_t)mx;

    mx = y0 + (int32_t)((boxes[i].y1 * (float)crop) / 96.0f);
    if (mx < 0) mx = 0;
    if (mx > (int32_t)(s_camera_height - 1U)) mx = (int32_t)(s_camera_height - 1U);
    boxes[i].y1 = (int16_t)mx;

    mx = x0 + (int32_t)((boxes[i].x2 * (float)crop) / 96.0f);
    if (mx < 0) mx = 0;
    if (mx > (int32_t)(s_camera_width - 1U)) mx = (int32_t)(s_camera_width - 1U);
    boxes[i].x2 = (int16_t)mx;

    mx = y0 + (int32_t)((boxes[i].y2 * (float)crop) / 96.0f);
    if (mx < 0) mx = 0;
    if (mx > (int32_t)(s_camera_height - 1U)) mx = (int32_t)(s_camera_height - 1U);
    boxes[i].y2 = (int16_t)mx;
  }

  DrawBoxesOnLCD(boxes, count);
  return 0;
}

int acquire_and_process_data(ai_i8* data[])
{
  if ((data == NULL) || (data[0] == NULL)) {
    return -1;
  }

  if (PreprocessCameraFrame(data[0]) != 0) {
    return -1;
  }
  return 0;
}

int post_process(ai_i8* data[])
{
  if ((data == NULL) || (data[0] == NULL)) {
    return -1;
  }

  if (PostProcessNanoV4(data[0]) != 0) {
    return -1;
  }
  return 0;
}
/* USER CODE END 2 */

/* Entry points --------------------------------------------------------------*/

void MX_X_CUBE_AI_Init(void)
{
    /* USER CODE BEGIN 5 */
  if (ai_boostrap(data_activations0) != 0) {
    g_ai_selftest_status = 0xE1U;
    g_ai_ready = 0U;
    AI_DebugFlush();
    return;
  }

  if ((data_outs[0] == NULL) ||
      (NanoV4_SelfTest((int8_t *)data_outs[0],
                       NANOV4_OUTPUT_ELEMENTS) == 0)) {
    g_ai_selftest_status = 0xE2U;
    g_ai_ready = 0U;
    AI_DebugFlush();
    Debug_SetStage(0xA1E2U);
    return;
  }

  g_ai_selftest_status = 1U;
  g_ai_ready = 1U;
  g_ai_error_type = 0U;
  g_ai_error_code = 0U;
  g_ai_last_result = 0;
  AI_DebugFlush();
    /* USER CODE END 5 */
}

void MX_X_CUBE_AI_Process(void)
{
    /* USER CODE BEGIN 6 */
  if (!network) {
    s_ai_last_result = -1;
    return;
  }
  if (s_ai_process_requested == 0U) {
    return;
  }

  s_ai_process_requested = 0U;
  s_ai_last_result = -2;
  /* 1 - acquire and pre-process input data */
  if (acquire_and_process_data(data_ins) != 0) {
    return;
  }
  s_ai_last_result = -3;
  /* 2 - process the data - call inference engine */
  if (ai_run() != 0) {
    return;
  }
  s_ai_last_result = -4;
  /* 3- post-process the predictions */
  if (post_process(data_outs) != 0) {
    return;
  }
  s_ai_last_result = 1;
    /* USER CODE END 6 */
}
#ifdef __cplusplus
}
#endif

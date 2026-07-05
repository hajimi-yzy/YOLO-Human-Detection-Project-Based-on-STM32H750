#include "app_camera_ai.h"

#include "app_x-cube-ai.h"
#include "bh1750_daynight.h"
#include "dcmi_ov5640.h"
#include "led.h"

ALIGN_32BYTES(static uint16_t s_app_camera_buffer[Display_Width * Display_Height]);

volatile uint32_t g_dbg_magic = 0x43445831U;
volatile uint32_t g_dbg_stage = 0U;
volatile uint32_t g_dbg_loop_count = 0U;
volatile uint32_t g_dbg_frame_count = 0U;
volatile uint32_t g_dbg_lcd_count = 0U;
volatile uint32_t g_dbg_ai_count = 0U;
volatile uint32_t g_dbg_px = 0U;
volatile uint32_t g_dbg_sum = 0U;
volatile uint32_t g_dbg_ai_tick0 = 0U;
volatile uint32_t g_dbg_ai_tick1 = 0U;
volatile uint32_t g_dbg_ai_dt = 0U;
volatile int32_t g_dbg_ai_result = 0;
volatile uint32_t g_dbg_fault_cfsr = 0U;
volatile uint32_t g_dbg_fault_hfsr = 0U;
volatile uint32_t g_dbg_fault_dfsr = 0U;
volatile uint32_t g_dbg_fault_afsr = 0U;
volatile uint32_t g_dbg_fault_bfar = 0U;
volatile uint32_t g_dbg_fault_mmfar = 0U;
volatile uint32_t g_dbg_fault_shcsr = 0U;
volatile uint32_t g_ai_enable = 1U;
volatile uint32_t g_ai_period = 1U;

static uint32_t s_last_tick = 0U;

static void App_DCacheInvalidateRange(void *addr, uint32_t size)
{
  uint32_t start = ((uint32_t)addr) & ~31U;
  uint32_t end = (((uint32_t)addr) + size + 31U) & ~31U;
  SCB_InvalidateDCache_by_Addr((uint32_t *)start, (int32_t)(end - start));
}

static void App_DCacheCleanRange(const void *addr, uint32_t size)
{
  if ((SCB->CCR & SCB_CCR_DC_Msk) != 0U) {
    uint32_t start = ((uint32_t)addr) & ~31U;
    uint32_t end = (((uint32_t)addr) + size + 31U) & ~31U;
    SCB_CleanDCache_by_Addr((uint32_t *)start, (int32_t)(end - start));
  }
}

static void App_DebugFlush(void)
{
  uint32_t start = (uint32_t)&g_dbg_stage;
  uint32_t end = (uint32_t)&g_dbg_fault_shcsr + sizeof(g_dbg_fault_shcsr);
  App_DCacheCleanRange((const void *)start, end - start);
}

static void App_UpdateFrameStats(void)
{
  uint32_t sum = 0U;
  uint32_t step = (Display_Width * Display_Height) / 64U;

  if (step == 0U) {
    step = 1U;
  }

  for (uint32_t i = 0U; i < (Display_Width * Display_Height); i += step) {
    sum += s_app_camera_buffer[i];
  }

  g_dbg_px = s_app_camera_buffer[(Display_Height / 2U) * Display_Width + (Display_Width / 2U)];
  g_dbg_sum = sum;
}

static void App_RunFrame(void)
{
  int ai_result;
  uint32_t now;

  OV5640_FrameState = 0U;

  Debug_SetStage(0x0210U);
  g_dbg_frame_count++;

  Debug_SetStage(0x0211U);
  OV5640_DCMI_Suspend();

  Debug_SetStage(0x0212U);
  App_DCacheCleanRange(s_app_camera_buffer, sizeof(s_app_camera_buffer));
  App_DCacheInvalidateRange(s_app_camera_buffer, sizeof(s_app_camera_buffer));
  App_UpdateFrameStats();
  AI_SetCameraFrame(s_app_camera_buffer, Display_Width, Display_Height);

  Debug_SetStage(0x0220U);
  LCD_CopyBuffer(0, 0, Display_Width, Display_Height, s_app_camera_buffer);
  g_dbg_lcd_count++;

  now = HAL_GetTick();
  if (now > s_last_tick) {
    LCD_DisplayString(84, 240, "FPS:");
    LCD_DisplayNumber(132, 240, (int32_t)(1000U / (now - s_last_tick)), 2);
  }
  s_last_tick = now;
  LED1_Toggle;

  if ((g_ai_enable != 0U) && (g_ai_period != 0U) &&
      ((g_dbg_frame_count % g_ai_period) == 0U)) {
    Debug_SetStage(0x0230U);
    g_dbg_ai_tick0 = HAL_GetTick();
    App_DebugFlush();
    AI_RequestProcess();
    ai_result = AI_ProcessPendingRequest();
    g_dbg_ai_tick1 = HAL_GetTick();
    g_dbg_ai_dt = g_dbg_ai_tick1 - g_dbg_ai_tick0;
    g_dbg_ai_result = ai_result;
    if (ai_result > 0) {
      g_dbg_ai_count++;
      Debug_SetStage(0x0231U);
    } else {
      Debug_SetStage(0x02E0U);
    }
  }

  Debug_SetStage(0x0240U);
  OV5640_DCMI_Resume();
  Debug_SetStage(0x0200U);
}

void Debug_SetStage(uint32_t stage)
{
  g_dbg_stage = stage;
  App_DebugFlush();
}

void App_CameraAi_Init(void)
{
  Debug_SetStage(0x0141U);
  Debug_SetStage(0x0150U);
  LED_Init();
  Debug_SetStage(0x0152U);
  Ircut1_Init();
  Bh1750_DayNightInit();
  Debug_SetStage(0x0153U);
  SPI_LCD_Init();
  Debug_SetStage(0x0151U);
}

void App_CameraAi_Start(void)
{
  Debug_SetStage(0x0160U);
  DCMI_OV5640_Init();
  Debug_SetStage(0x0161U);
  OV5640_Set_Vertical_Flip(OV5640_Disable);
  Debug_SetStage(0x0170U);
  OV5640_DMA_Transmit_Continuous((uint32_t)s_app_camera_buffer, Display_BufferSize);
  Debug_SetStage(0x0200U);
}

void App_CameraAi_Poll(void)
{
  int8_t ircut_mode;

  g_dbg_loop_count++;

  ircut_mode = Bh1750_DayNightTask();
  if (ircut_mode >= 0) {
    Ircut1_SetNight((uint8_t)ircut_mode);
  }

  if (OV5640_FrameState == 1U) {
    App_RunFrame();
  } else {
    Debug_SetStage(0x0201U);
  }
}

void App_CameraAi_ProcessHook(void)
{
  (void)AI_ProcessPendingRequest();
}

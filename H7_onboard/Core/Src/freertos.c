/* USER CODE BEGIN Header */
/**
 ******************************************************************************
 * File Name          : freertos.c
 * Description        : Code for freertos applications
 ******************************************************************************
 * @attention
 *
 * Copyright (c) 2023 STMicroelectronics.
 * All rights reserved.
 *
 * This software is licensed under terms that can be found in the LICENSE file
 * in the root directory of this software component.
 * If no LICENSE file comes with this software, it is provided AS-IS.
 *
 ******************************************************************************
 */
/* USER CODE END Header */

/* Includes ------------------------------------------------------------------*/
#include "FreeRTOS.h"
#include "task.h"
#include "main.h"
#include "cmsis_os.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */
#include "Sensor_task.h"
#include "Cloud_task.h"
/* USER CODE END Includes */

/* Private typedef -----------------------------------------------------------*/
/* USER CODE BEGIN PTD */

/* USER CODE END PTD */

/* Private define ------------------------------------------------------------*/
/* USER CODE BEGIN PD */

/* USER CODE END PD */

/* Private macro -------------------------------------------------------------*/
/* USER CODE BEGIN PM */

/* USER CODE END PM */

/* Private variables ---------------------------------------------------------*/
/* USER CODE BEGIN Variables */

/* USER CODE END Variables */
osThreadId defaultTaskHandle;
osThreadId LED_taskHandle;
osThreadId LegControl_taskHandle;
osThreadId MPU_taskHandle;
osThreadId PS2_taskHandle;
osThreadId Sensor_taskHandle;
osThreadId Radar_taskHandle;
osThreadId Cloud_taskHandle;

/* Private function prototypes -----------------------------------------------*/
/* USER CODE BEGIN FunctionPrototypes */

/* USER CODE END FunctionPrototypes */

void StartDefaultTask(void const * argument);
extern void LED_Task(void const * argument);
extern void LegControl_Task(void const * argument);
extern void MPU_Task(void const * argument);
extern void ps2_task(void const * argument);
extern void Sensor_Task(void const * argument);
extern void Radar_Task(void const * argument);
extern void Cloud_Task(void const * argument);

void MX_FREERTOS_Init(void); /* (MISRA C 2004 rule 8.1) */

/* GetIdleTaskMemory prototype (linked to static allocation support) */
void vApplicationGetIdleTaskMemory( StaticTask_t **ppxIdleTaskTCBBuffer, StackType_t **ppxIdleTaskStackBuffer, uint32_t *pulIdleTaskStackSize );

/* USER CODE BEGIN GET_IDLE_TASK_MEMORY */
static StaticTask_t xIdleTaskTCBBuffer;
static StackType_t xIdleStack[configMINIMAL_STACK_SIZE];

void vApplicationGetIdleTaskMemory(StaticTask_t **ppxIdleTaskTCBBuffer, StackType_t **ppxIdleTaskStackBuffer, uint32_t *pulIdleTaskStackSize)
{
  *ppxIdleTaskTCBBuffer = &xIdleTaskTCBBuffer;
  *ppxIdleTaskStackBuffer = &xIdleStack[0];
  *pulIdleTaskStackSize = configMINIMAL_STACK_SIZE;
  /* place for user code */
}
/* USER CODE END GET_IDLE_TASK_MEMORY */

/**
  * @brief  FreeRTOS initialization
  * @param  None
  * @retval None
  */
void MX_FREERTOS_Init(void) {
  /* USER CODE BEGIN Init */

  /* USER CODE END Init */

  /* USER CODE BEGIN RTOS_MUTEX */
  /* add mutexes, ... */
  /* USER CODE END RTOS_MUTEX */

  /* USER CODE BEGIN RTOS_SEMAPHORES */
  /* add semaphores, ... */
  /* USER CODE END RTOS_SEMAPHORES */

  /* USER CODE BEGIN RTOS_TIMERS */
  /* start timers, add new ones, ... */
  /* USER CODE END RTOS_TIMERS */

  /* USER CODE BEGIN RTOS_QUEUES */
  /* add queues, ... */
  /* USER CODE END RTOS_QUEUES */

  /* Create the thread(s) */
  /* definition and creation of defaultTask */
  osThreadDef(defaultTask, StartDefaultTask, osPriorityIdle, 0, 128);
  defaultTaskHandle = osThreadCreate(osThread(defaultTask), NULL);

  /* definition and creation of LED_task */
  osThreadDef(LED_task, LED_Task, osPriorityLow, 0, 128);
  LED_taskHandle = osThreadCreate(osThread(LED_task), NULL);

  /* definition and creation of LegControl_task */
  osThreadDef(LegControl_task, LegControl_Task, osPriorityNormal, 0, 1024);
  LegControl_taskHandle = osThreadCreate(osThread(LegControl_task), NULL);

  /* definition and creation of MPU_task */
  osThreadDef(MPU_task, MPU_Task, osPriorityBelowNormal, 0, 512);
  MPU_taskHandle = osThreadCreate(osThread(MPU_task), NULL);

  /* definition and creation of PS2_task */
  osThreadDef(PS2_task, ps2_task, osPriorityNormal, 0, 128);
  PS2_taskHandle = osThreadCreate(osThread(PS2_task), NULL);

  /* USER CODE BEGIN RTOS_THREADS */
  /* definition and creation of Sensor_task */
  osThreadDef(Sensor_task, Sensor_Task, osPriorityBelowNormal, 0, 256);
  Sensor_taskHandle = osThreadCreate(osThread(Sensor_task), NULL);

  /* definition and creation of Radar_task */
  osThreadDef(Radar_task, Radar_Task, osPriorityHigh, 0, 256);
  Radar_taskHandle = osThreadCreate(osThread(Radar_task), NULL);

  /* definition and creation of Cloud_task */
  osThreadDef(Cloud_task, Cloud_Task, osPriorityLow, 0, 256);
  Cloud_taskHandle = osThreadCreate(osThread(Cloud_task), NULL);
  /* USER CODE END RTOS_THREADS */

}

/* USER CODE BEGIN Header_StartDefaultTask */
/**
 * @brief  Function implementing the defaultTask thread.
 * @param  argument: Not used
 * @retval None
 */
#include "debug_uart.h"
#include "main.h"
#include "bsp_dwt.h"
#include "core_cm7.h"
#include "usart.h"
static void SystemClock_Config(uint32_t SYSCLKDivider);
static void adj_sysfreq(float cpu_load_rate); // 调整系统主频
extern uint32_t SystemCoreClock;
/* USER CODE END Header_StartDefaultTask */
void StartDefaultTask(void const * argument)
{
  /* USER CODE BEGIN StartDefaultTask */
  uint32_t count;
  float cpu_load_rate;
  uint32_t start_time, end_time;
  start_time = xTaskGetTickCount();
  /* Infinite loop */
  for (;;)
  {
//    if (count++ % 1000 == 0)
//    {
//      end_time = xTaskGetTickCount();
//      cpu_load_rate = (1 - 1000.f / (end_time - start_time));
//      start_time = end_time;
//      APP_PRINT("cpu_load = %f%%\r\n", cpu_load_rate*100);
//      //adj_sysfreq(cpu_load_rate);
//    }
    osDelay(1);
  }
  /* USER CODE END StartDefaultTask */
}

/* Private application code --------------------------------------------------*/
/* USER CODE BEGIN Application */
static void adj_sysfreq(float cpu_load_rate) // 调整系统主频
{
  vTaskSuspendAll(); // 开启调度锁
  HAL_Delay(5);
  // 修改主频
  float compulity = cpu_load_rate * SystemCoreClock / (600000000.f);

  if (compulity > 0.4f)
    SystemClock_Config(RCC_SYSCLK_DIV1);
  else if (0.2f < compulity && compulity < 0.4f)
    SystemClock_Config(RCC_SYSCLK_DIV2);
  else if (0.1f < compulity && compulity < 0.2f)
    SystemClock_Config(RCC_SYSCLK_DIV4);
  else if (compulity < 0.1f)
    SystemClock_Config(RCC_SYSCLK_DIV8);
  // 修改system_tick适应新主频
  SysTick->LOAD = SystemCoreClock / 1000 - 1;
  DWT_set_CPU_Freq(SystemCoreClock/1000000);
  xTaskResumeAll(); // 关闭调度锁，回复任务调度
}


static void SystemClock_Config(uint32_t SYSCLKDivider)
{
  RCC_OscInitTypeDef RCC_OscInitStruct = {0};
  RCC_ClkInitTypeDef RCC_ClkInitStruct = {0};

  /** Supply configuration update enable
  */
  HAL_PWREx_ConfigSupply(PWR_LDO_SUPPLY);

  /** Configure the main internal regulator output voltage
  */
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE1);

  while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY))
  {
  }

  __HAL_RCC_SYSCFG_CLK_ENABLE();
  __HAL_PWR_VOLTAGESCALING_CONFIG(PWR_REGULATOR_VOLTAGE_SCALE0);

  while (!__HAL_PWR_GET_FLAG(PWR_FLAG_VOSRDY))
  {
  }

  /** Initializes the RCC Oscillators according to the specified parameters
  * in the RCC_OscInitTypeDef structure.
  */
  RCC_OscInitStruct.OscillatorType = RCC_OSCILLATORTYPE_HSI | RCC_OSCILLATORTYPE_HSE;
  RCC_OscInitStruct.HSEState = RCC_HSE_ON;
  RCC_OscInitStruct.HSIState = RCC_HSI_DIV1;
  RCC_OscInitStruct.HSICalibrationValue = RCC_HSICALIBRATION_DEFAULT;
  RCC_OscInitStruct.PLL.PLLState = RCC_PLL_ON;
  RCC_OscInitStruct.PLL.PLLSource = RCC_PLLSOURCE_HSE;
  RCC_OscInitStruct.PLL.PLLM = 5;
  RCC_OscInitStruct.PLL.PLLN = 240;
  RCC_OscInitStruct.PLL.PLLP = 2;
  RCC_OscInitStruct.PLL.PLLQ = 9;
  RCC_OscInitStruct.PLL.PLLR = 2;
  RCC_OscInitStruct.PLL.PLLRGE = RCC_PLL1VCIRANGE_2;
  RCC_OscInitStruct.PLL.PLLVCOSEL = RCC_PLL1VCOWIDE;
  RCC_OscInitStruct.PLL.PLLFRACN = 0;
  if (HAL_RCC_OscConfig(&RCC_OscInitStruct) != HAL_OK)
  {
    Error_Handler();
  }

  /** Initializes the CPU, AHB and APB buses clocks
  */
  RCC_ClkInitStruct.ClockType = RCC_CLOCKTYPE_HCLK | RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_PCLK1 | RCC_CLOCKTYPE_PCLK2 | RCC_CLOCKTYPE_D3PCLK1 | RCC_CLOCKTYPE_D1PCLK1;
  RCC_ClkInitStruct.SYSCLKSource = RCC_SYSCLKSOURCE_PLLCLK;
  RCC_ClkInitStruct.SYSCLKDivider = SYSCLKDivider;
  RCC_ClkInitStruct.AHBCLKDivider = RCC_HCLK_DIV4;
  RCC_ClkInitStruct.APB3CLKDivider = RCC_APB3_DIV2;
  RCC_ClkInitStruct.APB1CLKDivider = RCC_APB1_DIV2;
  RCC_ClkInitStruct.APB2CLKDivider = RCC_APB2_DIV2;
  RCC_ClkInitStruct.APB4CLKDivider = RCC_APB4_DIV2;

  if (HAL_RCC_ClockConfig(&RCC_ClkInitStruct, FLASH_LATENCY_2) != HAL_OK)
  {
    Error_Handler();
  }
}
/* USER CODE END Application */

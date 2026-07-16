/* USER CODE BEGIN Header */
/**
  ******************************************************************************
  * @file           : main.h
  * @brief          : Header for main.c file.
  *                   This file contains the common defines of the application.
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

/* Define to prevent recursive inclusion -------------------------------------*/
#ifndef __MAIN_H
#define __MAIN_H

#ifdef __cplusplus
extern "C" {
#endif

/* Includes ------------------------------------------------------------------*/
#include "stm32h7xx_hal.h"

/* Private includes ----------------------------------------------------------*/
/* USER CODE BEGIN Includes */

/* USER CODE END Includes */

/* Exported types ------------------------------------------------------------*/
/* USER CODE BEGIN ET */

/* USER CODE END ET */

/* Exported constants --------------------------------------------------------*/
/* USER CODE BEGIN EC */

/* USER CODE END EC */

/* Exported macro ------------------------------------------------------------*/
/* USER CODE BEGIN EM */

/* USER CODE END EM */

/* Exported functions prototypes ---------------------------------------------*/
void Error_Handler(void);

/* USER CODE BEGIN EFP */

/* USER CODE END EFP */

/* Private defines -----------------------------------------------------------*/
#define LEG1_TXE_Pin GPIO_PIN_0
#define LEG1_TXE_GPIO_Port GPIOC
#define LEG1_RXE_Pin GPIO_PIN_1
#define LEG1_RXE_GPIO_Port GPIOC
#define LEG2_TXE_Pin GPIO_PIN_2
#define LEG2_TXE_GPIO_Port GPIOC
#define LEG2_RXE_Pin GPIO_PIN_3
#define LEG2_RXE_GPIO_Port GPIOC
#define LEG3_TXE_Pin GPIO_PIN_0
#define LEG3_TXE_GPIO_Port GPIOA
#define INT_GYRO_Pin GPIO_PIN_1
#define INT_GYRO_GPIO_Port GPIOA
#define INT_ACCEL_Pin GPIO_PIN_2
#define INT_ACCEL_GPIO_Port GPIOA
#define CS_GYRO_Pin GPIO_PIN_3
#define CS_GYRO_GPIO_Port GPIOA
#define CS_ACCEL_Pin GPIO_PIN_4
#define CS_ACCEL_GPIO_Port GPIOA
#define LEG3_RXE_Pin GPIO_PIN_4
#define LEG3_RXE_GPIO_Port GPIOC
#define LEG4_TXE_Pin GPIO_PIN_5
#define LEG4_TXE_GPIO_Port GPIOC
#define LEG4_RXE_Pin GPIO_PIN_0
#define LEG4_RXE_GPIO_Port GPIOB
#define LEG5_TXE_Pin GPIO_PIN_1
#define LEG5_TXE_GPIO_Port GPIOB
#define LEG5_RXE_Pin GPIO_PIN_7
#define LEG5_RXE_GPIO_Port GPIOE
#define LEG6_TXE_Pin GPIO_PIN_8
#define LEG6_TXE_GPIO_Port GPIOE
#define LEG6_RXE_Pin GPIO_PIN_9
#define LEG6_RXE_GPIO_Port GPIOE
#define PS2_CLK_Pin GPIO_PIN_10
#define PS2_CLK_GPIO_Port GPIOE
#define PS2_CS_Pin GPIO_PIN_11
#define PS2_CS_GPIO_Port GPIOE
#define PS2_CMD_Pin GPIO_PIN_12
#define PS2_CMD_GPIO_Port GPIOE
#define PS2_DAT_Pin GPIO_PIN_13
#define PS2_DAT_GPIO_Port GPIOE
#define ARM_TXE_Pin GPIO_PIN_14
#define ARM_TXE_GPIO_Port GPIOE
#define ARM_RXE_Pin GPIO_PIN_15
#define ARM_RXE_GPIO_Port GPIOE
#define CS_FLASH_Pin GPIO_PIN_12
#define CS_FLASH_GPIO_Port GPIOB
#define LED_Pin GPIO_PIN_12
#define LED_GPIO_Port GPIOA

/* USER CODE BEGIN Private defines */
typedef uint8_t u8;
typedef uint16_t u16;
/* USER CODE END Private defines */

#ifdef __cplusplus
}
#endif

#endif /* __MAIN_H */

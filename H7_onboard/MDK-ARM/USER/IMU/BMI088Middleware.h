#ifndef BMI088MIDDLEWARE_H
#define BMI088MIDDLEWARE_H

#include "main.h"

#define BMI088_USE_SPI
//#define BMI088_USE_IIC

/*
#define CS1_ACCEL_GPIO_Port ACCEL_NSS_GPIO_Port
#define CS1_ACCEL_Pin ACCEL_NSS_Pin
#define CS1_GYRO_GPIO_Port GYRO_NSS_GPIO_Port
#define CS1_GYRO_Pin GYRO_NSS_Pin
*/

#define GYRO_SCK_GPIO_Port GPIOA
#define GYRO_SCK_Pin GPIO_PIN_5
#define GYRO_MISO_GPIO_Port GPIOA
#define GYRO_MISO_Pin GPIO_PIN_6
#define GYRO_MOSI_GPIO_Port GPIOA
#define GYRO_MOSI_Pin GPIO_PIN_7


#define 	BMI088_ACCEL_NS_L	HAL_GPIO_WritePin(CS_ACCEL_GPIO_Port,CS_ACCEL_Pin,GPIO_PIN_RESET);
#define 	BMI088_ACCEL_NS_H	HAL_GPIO_WritePin(CS_ACCEL_GPIO_Port,CS_ACCEL_Pin,GPIO_PIN_SET);
#define 	BMI088_GYRO_NS_L	HAL_GPIO_WritePin(CS_GYRO_GPIO_Port,CS_GYRO_Pin,GPIO_PIN_RESET);
#define 	BMI088_GYRO_NS_H	HAL_GPIO_WritePin(CS_GYRO_GPIO_Port,CS_GYRO_Pin,GPIO_PIN_SET);
#define 	BMI088_SCK_L	HAL_GPIO_WritePin(GYRO_SCK_GPIO_Port,GYRO_SCK_Pin,GPIO_PIN_RESET);
#define 	BMI088_SCK_H	HAL_GPIO_WritePin(GYRO_SCK_GPIO_Port,GYRO_SCK_Pin,GPIO_PIN_SET);
#define 	BMI088_MOSI_L	HAL_GPIO_WritePin(GYRO_MOSI_GPIO_Port,GYRO_MOSI_Pin,GPIO_PIN_RESET);
#define 	BMI088_MOSI_H	HAL_GPIO_WritePin(GYRO_MOSI_GPIO_Port,GYRO_MOSI_Pin,GPIO_PIN_SET);


#ifdef __cplusplus
extern "C" {
#endif

extern void BMI088_GPIO_init(void);
extern void BMI088_com_init(void);
extern void BMI088_delay_ms(uint16_t ms);
extern void BMI088_delay_us(uint16_t us);

extern uint8_t BMI088_read_write_byte(uint8_t reg);

#ifdef __cplusplus
}
#endif

#endif


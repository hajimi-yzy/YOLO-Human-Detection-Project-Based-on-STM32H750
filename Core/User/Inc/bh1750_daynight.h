#ifndef __BH1750_DAYNIGHT_H
#define __BH1750_DAYNIGHT_H

#include "main.h"

#ifdef __cplusplus
extern "C" {
#endif

#define BH1750_PWR_PORT              GPIOC
#define BH1750_PWR_PIN               GPIO_PIN_1

#define IRCUT1_NET1_PORT             GPIOD
#define IRCUT1_NET1_PIN              GPIO_PIN_12

void Bh1750_DayNightInit(void);
int8_t Bh1750_DayNightTask(void);
uint16_t Bh1750_GetLastLux(void);
uint8_t Bh1750_GetNightMode(void);

void Bh1750_I2cTxCpltCallback(I2C_HandleTypeDef *hi2c);
void Bh1750_I2cRxCpltCallback(I2C_HandleTypeDef *hi2c);
void Bh1750_I2cErrorCallback(I2C_HandleTypeDef *hi2c);

void Ircut1_Init(void);
void Ircut1_SetNight(uint8_t night_mode);

#ifdef __cplusplus
}
#endif

#endif /* __BH1750_DAYNIGHT_H */

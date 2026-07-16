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

#define PA3_NIGHT_PORT               PA3_NIGHT_GPIO_Port
#define PA3_NIGHT_CTRL_PIN           PA3_NIGHT_Pin

void Bh1750_DayNightInit(void);
int8_t Bh1750_DayNightTask(void);
uint16_t Bh1750_GetLastLux(void);
uint8_t Bh1750_GetNightMode(void);

void Ircut1_Init(void);
void Ircut1_SetNight(uint8_t night_mode);

void Pa3NightOutput_Init(void);
void Pa3NightOutput_SetNight(uint8_t night_mode);

#ifdef __cplusplus
}
#endif

#endif /* __BH1750_DAYNIGHT_H */

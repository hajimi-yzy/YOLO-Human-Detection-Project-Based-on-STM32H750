#ifndef GAS_SENSOR_H
#define GAS_SENSOR_H

#include "stdint.h"

#define GAS_SENSOR_POWER_PIN    GPIO_PIN_9
#define GAS_SENSOR_POWER_PORT   GPIOC

void GasSensor_Init(void);
void GasSensor_PowerOn(void);
void GasSensor_PowerOff(void);
uint16_t GasSensor_ReadLevel(void);
uint8_t GasSensor_IsDetected(void);

#endif

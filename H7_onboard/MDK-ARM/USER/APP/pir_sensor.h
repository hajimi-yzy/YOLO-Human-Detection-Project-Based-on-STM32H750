#ifndef PIR_SENSOR_H
#define PIR_SENSOR_H

#include "stdint.h"

#define PIR_SENSOR_PIN    GPIO_PIN_11
#define PIR_SENSOR_PORT   GPIOA

void PIR_Init(void);
uint8_t PIR_IsDetected(void);

#endif

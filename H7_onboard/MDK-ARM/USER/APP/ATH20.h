#ifndef ATH20_H
#define ATH20_H

#include "stdint.h"

#define AHT20_ADDR         0x38

#define AHT20_CMD_INIT       0xE1
#define AHT20_CMD_TRIG_MEAS  0xAC
#define AHT20_CMD_RESET      0xBA

typedef struct {
    float temperature;
    float humidity;
} AHT20_Data_t;

uint8_t AHT20_Init(void);
uint8_t AHT20_ReadData(AHT20_Data_t *data);
uint8_t AHT20_SoftReset(void);

#endif

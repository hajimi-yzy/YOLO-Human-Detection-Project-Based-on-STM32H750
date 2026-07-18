#ifndef BMP280_H
#define BMP280_H

#include "stdint.h"

#define BMP280_ADDR          0x76

#define BMP280_REG_CHIPID     0xD0
#define BMP280_REG_RESET      0xE0
#define BMP280_REG_STATUS     0xF3
#define BMP280_REG_CTRL_MEAS  0xF4
#define BMP280_REG_CONFIG     0xF5
#define BMP280_REG_TEMP_MSB   0xFA
#define BMP280_REG_TEMP_LSB   0xFB
#define BMP280_REG_TEMP_XLSB  0xFC
#define BMP280_REG_PRESS_MSB  0xF7
#define BMP280_REG_PRESS_LSB  0xF8
#define BMP280_REG_PRESS_XLSB 0xF9

#define BMP280_MODE_SLEEP     0x00
#define BMP280_MODE_FORCED    0x02
#define BMP280_MODE_NORMAL     0x03

#define BMP280_OS_TEMP_1      0x00
#define BMP280_OS_TEMP_2      0x20
#define BMP280_OS_TEMP_4      0x40
#define BMP280_OS_TEMP_8      0x60
#define BMP280_OS_TEMP_16     0x80

#define BMP280_OS_PRESS_1     0x00
#define BMP280_OS_PRESS_2     0x04
#define BMP280_OS_PRESS_4     0x08
#define BMP280_OS_PRESS_8     0x0C
#define BMP280_OS_PRESS_16    0x10

#define BMP280_FILTER_OFF     0x00
#define BMP280_FILTER_2       0x04
#define BMP280_FILTER_4       0x08
#define BMP280_FILTER_8       0x0C
#define BMP280_FILTER_16      0x10

#define BMP280_T_SB_0_5      0x00
#define BMP280_T_SB_62_5     0x20
#define BMP280_T_SB_125      0x40
#define BMP280_T_SB_250      0x60
#define BMP280_T_SB_500      0x80
#define BMP280_T_SB_1000     0xA0
#define BMP280_T_SB_2000     0xC0
#define BMP280_T_SB_4000     0xE0

typedef struct {
    float temperature;
    float pressure;
    float altitude;
} BMP280_Data_t;

uint8_t BMP280_Init(void);
uint8_t BMP280_ReadData(BMP280_Data_t *data);
void BMP280_Calculate(float temperature, float pressure, float *altitude);

#endif

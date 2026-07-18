#include "BMP280.h"
#include "bsp_i2c.h"
#include "main.h"
#include "math.h"

#define SEA_LEVEL_PRESSURE  1013.25f

static int16_t dig_T1, dig_T2, dig_T3;
static int16_t dig_P1, dig_P2, dig_P3, dig_P4, dig_P5, dig_P6, dig_P7, dig_P8, dig_P9;
static uint8_t bmp280_initialized = 0;

static int32_t t_fine;

static int32_t BMP280_Compensate_T(int32_t adc_T)
{
    int32_t var1, var2, T;
    var1 = ((((adc_T >> 3) - ((int32_t)dig_T1 << 1))) * ((int32_t)dig_T2)) >> 11;
    var2 = (((((adc_T >> 4) - (int32_t)dig_T1) * ((adc_T >> 4) - (int32_t)dig_T1)) >> 12) * ((int32_t)dig_T3)) >> 14;
    t_fine = var1 + var2;
    T = (t_fine * 5 + 128) >> 8;
    return T;
}

static uint32_t BMP280_Compensate_P(int32_t adc_P)
{
    int32_t var1, var2;
    uint32_t p;
    var1 = (((int32_t)t_fine) >> 1) - (int32_t)64000;
    var2 = (((var1 >> 2) * (var1 >> 2)) >> 11) * ((int32_t)dig_P6);
    var2 = var2 + ((var1 * ((int32_t)dig_P5)) << 1);
    var2 = (var2 >> 2) + (((int32_t)dig_P4) << 16);
    var1 = (((dig_P3 * ((var1 >> 2) * (var1 >> 2) >> 13)) >> 3) + (((int32_t)dig_P2) * var1) >> 1) >> 18;
    var1 = ((32768 + var1) * ((int32_t)dig_P1)) >> 15;
    if (var1 == 0) return 0;
    p = ((uint32_t)(((int32_t)1048576) - adc_P) - (var2 >> 12)) * 3125;
    if (p < 0x80000000) p = (p << 1) / ((uint32_t)var1);
    else p = (p / (uint32_t)var1) * 2;
    var1 = (((int32_t)dig_P9) * ((int32_t)(((p >> 3) * (p >> 3)) >> 13))) >> 12;
    var2 = (((int32_t)(p >> 2)) * ((int32_t)dig_P8)) >> 13;
    p = (uint32_t)((int32_t)p + ((var1 + var2 + dig_P7) >> 4));
    return p;
}

uint8_t BMP280_Init(void)
{
    uint8_t chip_id;
    uint8_t reg_data[24];

    BSP_I2C_ReadReg(BMP280_ADDR << 1, BMP280_REG_CHIPID, &chip_id);
    if (chip_id != 0x58) {
        return 1;
    }

    BSP_I2C_ReadBuf(BMP280_ADDR << 1, 0x88, reg_data, 24);

    dig_T1 = (int16_t)((uint16_t)reg_data[1] << 8 | reg_data[0]);
    dig_T2 = (int16_t)((uint16_t)reg_data[3] << 8 | reg_data[2]);
    dig_T3 = (int16_t)((uint16_t)reg_data[5] << 8 | reg_data[4]);
    dig_P1 = (int16_t)((uint16_t)reg_data[7] << 8 | reg_data[6]);
    dig_P2 = (int16_t)((uint16_t)reg_data[9] << 8 | reg_data[8]);
    dig_P3 = (int16_t)((uint16_t)reg_data[11] << 8 | reg_data[10]);
    dig_P4 = (int16_t)((uint16_t)reg_data[13] << 8 | reg_data[12]);
    dig_P5 = (int16_t)((uint16_t)reg_data[15] << 8 | reg_data[14]);
    dig_P6 = (int16_t)((uint16_t)reg_data[17] << 8 | reg_data[16]);
    dig_P7 = (int16_t)((uint16_t)reg_data[19] << 8 | reg_data[18]);
    dig_P8 = (int16_t)((uint16_t)reg_data[21] << 8 | reg_data[20]);
    dig_P9 = (int16_t)((uint16_t)reg_data[23] << 8 | reg_data[22]);

    BSP_I2C_WriteReg(BMP280_ADDR << 1, BMP280_REG_RESET, 0xB6);
    HAL_Delay(10);

    BSP_I2C_WriteReg(BMP280_ADDR << 1, BMP280_REG_CTRL_MEAS,
                     BMP280_OS_TEMP_2 | BMP280_OS_PRESS_4 | BMP280_MODE_NORMAL);
    BSP_I2C_WriteReg(BMP280_ADDR << 1, BMP280_REG_CONFIG,
                     BMP280_T_SB_250 | BMP280_FILTER_4);

    bmp280_initialized = 1;
    return 0;
}

uint8_t BMP280_ReadData(BMP280_Data_t *data)
{
    uint8_t data_buf[6];
    int32_t adc_T, adc_P;
    int32_t temp;
    uint32_t press;

    if (!bmp280_initialized) {
        return 1;
    }

    BSP_I2C_ReadBuf(BMP280_ADDR << 1, BMP280_REG_PRESS_MSB, data_buf, 6);

    adc_P = ((uint32_t)data_buf[0] << 12) | ((uint32_t)data_buf[1] << 4) | ((uint32_t)data_buf[2] >> 4);
    adc_T = ((uint32_t)data_buf[3] << 12) | ((uint32_t)data_buf[4] << 4) | ((uint32_t)data_buf[5] >> 4);

    temp = BMP280_Compensate_T(adc_T);
    press = BMP280_Compensate_P(adc_P);

    data->temperature = (float)temp / 100.0f;
    data->pressure = (float)press / 256.0f;

    BMP280_Calculate(data->temperature, data->pressure, &data->altitude);

    return 0;
}

void BMP280_Calculate(float temperature, float pressure, float *altitude)
{
    if (altitude != NULL) {
        *altitude = 44330.0f * (1.0f - powf(pressure / SEA_LEVEL_PRESSURE, 0.1903f));
    }
}

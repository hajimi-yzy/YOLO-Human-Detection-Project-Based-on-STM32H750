#include "ATH20.h"
#include "bsp_i2c.h"
#include "main.h"

static void Delay_ms(uint32_t ms)
{
    HAL_Delay(ms);
}

uint8_t AHT20_Init(void)
{
    uint8_t status;
    uint8_t cmd[3];

    AHT20_SoftReset();
    Delay_ms(40);

    cmd[0] = AHT20_CMD_INIT;
    cmd[1] = 0x08;
    cmd[2] = 0x00;

    if (BSP_I2C_WriteBuf(AHT20_ADDR << 1, cmd[0], cmd + 1, 2) != HAL_OK) {
        return 1;
    }

    Delay_ms(10);

    status = BSP_I2C_ReadReg(AHT20_ADDR << 1, 0x71, &status);
    if ((status & 0x18) != 0x18) {
        return 2;
    }

    return 0;
}

uint8_t AHT20_SoftReset(void)
{
    uint8_t cmd = AHT20_CMD_RESET;
    return BSP_I2C_WriteBuf(AHT20_ADDR << 1, AHT20_CMD_RESET, &cmd, 0);
}

uint8_t AHT20_ReadData(AHT20_Data_t *data)
{
    uint8_t cmd[3] = {AHT20_CMD_TRIG_MEAS, 0x33, 0x00};
    uint8_t buf[6];
    uint32_t tmp1, tmp2;

    if (BSP_I2C_WriteBuf(AHT20_ADDR << 1, cmd[0], cmd + 1, 2) != HAL_OK) {
        return 1;
    }

    Delay_ms(80);

    if (BSP_I2C_ReadBuf(AHT20_ADDR << 1, 0x00, buf, 6) != HAL_OK) {
        return 2;
    }

    if ((buf[0] & 0x80) != 0) {
        return 3;
    }

    tmp1 = ((uint32_t)buf[1] << 12) | ((uint32_t)buf[2] << 4) | ((uint32_t)buf[3] >> 4);
    tmp2 = (((uint32_t)buf[3] & 0x0F) << 16) | ((uint32_t)buf[4] << 8) | buf[5];

    data->humidity = ((float)tmp1 / 1048576.0f) * 100.0f;

    tmp1 = (((uint32_t)buf[3] & 0x0F) << 16) | ((uint32_t)buf[4] << 8) | buf[5];
    data->temperature = ((float)tmp1 / 1048576.0f) * 200.0f - 50.0f;

    return 0;
}

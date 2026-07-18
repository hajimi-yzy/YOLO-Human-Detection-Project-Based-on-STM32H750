#include "bsp_i2c.h"
#include "gpio.h"

I2C_HandleTypeDef hi2c1;

void BSP_I2C_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_I2C1_CLK_ENABLE();

    GPIO_InitStruct.Pin = I2C1_SCL_PIN | I2C1_SDA_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_AF_OD;
    GPIO_InitStruct.Pull = GPIO_PULLUP;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_VERY_HIGH;
    GPIO_InitStruct.Alternate = GPIO_AF4_I2C1;
    HAL_GPIO_Init(I2C1_PORT, &GPIO_InitStruct);

    hi2c1.Instance = I2C1;
    hi2c1.Init.Timing = 0x00F02B86;
    hi2c1.Init.OwnAddress1 = 0;
    hi2c1.Init.AddressingMode = I2C_ADDRESSINGMODE_7BIT;
    hi2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    hi2c1.Init.OwnAddress2 = 0;
    hi2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    hi2c1.Init.NoStretchMode = I2C_NOSTRETCH_DISABLE;
    if (HAL_I2C_Init(&hi2c1) != HAL_OK)
    {
        Error_Handler();
    }
}

uint8_t BSP_I2C_Write(uint8_t addr, uint8_t reg, uint8_t *data, uint16_t len)
{
    uint8_t buf[256];
    buf[0] = reg;
    for (uint16_t i = 0; i < len; i++) {
        buf[i + 1] = data[i];
    }
    return HAL_I2C_Master_Transmit(&hi2c1, addr, buf, len + 1, I2C_TIMEOUT);
}

uint8_t BSP_I2C_Read(uint8_t addr, uint8_t reg, uint8_t *data, uint16_t len)
{
    HAL_I2C_Master_Transmit(&hi2c1, addr, &reg, 1, I2C_TIMEOUT);
    return HAL_I2C_Master_Receive(&hi2c1, addr, data, len, I2C_TIMEOUT);
}

uint8_t BSP_I2C_WriteReg(uint8_t addr, uint8_t reg, uint8_t value)
{
    return BSP_I2C_Write(addr, reg, &value, 1);
}

uint8_t BSP_I2C_ReadReg(uint8_t addr, uint8_t reg, uint8_t *value)
{
    return BSP_I2C_Read(addr, reg, value, 1);
}

uint8_t BSP_I2C_WriteBuf(uint8_t addr, uint8_t reg, uint8_t *data, uint16_t len)
{
    return BSP_I2C_Write(addr, reg, data, len);
}

uint8_t BSP_I2C_ReadBuf(uint8_t addr, uint8_t reg, uint8_t *data, uint16_t len)
{
    return BSP_I2C_Read(addr, reg, data, len);
}

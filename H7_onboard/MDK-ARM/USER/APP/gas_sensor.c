#include "gas_sensor.h"
#include "main.h"

static uint8_t gas_sensor_powered = 0;

void GasSensor_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOC_CLK_ENABLE();

    GPIO_InitStruct.Pin = GAS_SENSOR_POWER_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_OUTPUT_PP;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(GAS_SENSOR_POWER_PORT, &GPIO_InitStruct);

    HAL_GPIO_WritePin(GAS_SENSOR_POWER_PORT, GAS_SENSOR_POWER_PIN, GPIO_PIN_RESET);
    gas_sensor_powered = 0;
}

void GasSensor_PowerOn(void)
{
    HAL_GPIO_WritePin(GAS_SENSOR_POWER_PORT, GAS_SENSOR_POWER_PIN, GPIO_PIN_SET);
    gas_sensor_powered = 1;
}

void GasSensor_PowerOff(void)
{
    HAL_GPIO_WritePin(GAS_SENSOR_POWER_PORT, GAS_SENSOR_POWER_PIN, GPIO_PIN_RESET);
    gas_sensor_powered = 0;
}

uint16_t GasSensor_ReadLevel(void)
{
    if (!gas_sensor_powered) return 0;

    GPIO_PinState state = HAL_GPIO_ReadPin(GAS_SENSOR_POWER_PORT, GAS_SENSOR_POWER_PIN);
    return (state == GPIO_PIN_SET) ? 100 : 0;
}

uint8_t GasSensor_IsDetected(void)
{
    return GasSensor_ReadLevel() > 50;
}

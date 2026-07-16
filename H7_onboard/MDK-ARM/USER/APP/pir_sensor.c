#include "pir_sensor.h"
#include "main.h"

void PIR_Init(void)
{
    GPIO_InitTypeDef GPIO_InitStruct = {0};

    __HAL_RCC_GPIOA_CLK_ENABLE();

    GPIO_InitStruct.Pin = PIR_SENSOR_PIN;
    GPIO_InitStruct.Mode = GPIO_MODE_INPUT;
    GPIO_InitStruct.Pull = GPIO_NOPULL;
    GPIO_InitStruct.Speed = GPIO_SPEED_FREQ_LOW;
    HAL_GPIO_Init(PIR_SENSOR_PORT, &GPIO_InitStruct);
}

uint8_t PIR_IsDetected(void)
{
    return HAL_GPIO_ReadPin(PIR_SENSOR_PORT, PIR_SENSOR_PIN) == GPIO_PIN_SET;
}

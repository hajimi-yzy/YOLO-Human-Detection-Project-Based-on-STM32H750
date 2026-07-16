#include "Cloud_task.h"
#include "cmsis_os.h"
#include "sensor_data.h"
#include "usart.h"
#include "main.h"
#include "string.h"
#include <stdio.h>

extern UART_HandleTypeDef huart7;
extern osThreadId Cloud_taskHandle;

#define CLOUD_TX_BUFFER_SIZE  128

static char tx_buffer[CLOUD_TX_BUFFER_SIZE];

void Cloud_Task(void const *argument)
{
    MX_UART7_Init();

    osDelay(1000);

    while (1)
    {
        int len = sprintf(tx_buffer,
            "T:%.1f,H:%.1f,A:%.1f,P:%.0f,G:%d,I:%d\n",
            g_sensor_data.aht20.temperature,
            g_sensor_data.aht20.humidity,
            g_sensor_data.bmp280.altitude,
            g_sensor_data.bmp280.pressure,
            g_sensor_data.gas_level > 500 ? 1 : 0,
            g_sensor_data.human_detect);

        if (len > 0) {
            HAL_UART_Transmit(&huart7, (uint8_t*)tx_buffer, len, 100);
        }

        osDelay(1000);
    }
}

#include "Radar_task.h"
#include "cmsis_os.h"
#include "sensor_data.h"
#include "d6_radar.h"
#include "main.h"

extern osThreadId Radar_taskHandle;

void Radar_Task(void const *argument)
{
    osDelay(200);

    D6_Radar_Init();

    while (1)
    {
        D6_Radar_ReadData(&g_sensor_data.radar);
        osDelay(100);
    }
}

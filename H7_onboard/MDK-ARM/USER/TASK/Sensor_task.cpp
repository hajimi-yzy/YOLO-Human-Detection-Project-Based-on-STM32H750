#include "Sensor_task.h"
#include "cmsis_os.h"
#include "sensor_data.h"
#include "ATH20.h"
#include "BMP280.h"

extern osThreadId Sensor_taskHandle;

void Sensor_Task(void const *argument)
{
    osDelay(500);

    SensorData_Init();

    while (1)
    {
        SensorData_UpdateAll();
        osDelay(1000);
    }
}

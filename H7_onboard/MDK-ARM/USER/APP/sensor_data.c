#include "sensor_data.h"
#include "ATH20.h"
#include "BMP280.h"
#include "gas_sensor.h"
#include "pir_sensor.h"
#include "gps.h"
#include "d6_radar.h"
#include "main.h"
#include <string.h>

Sensor_Data_t g_sensor_data;

void SensorData_Init(void)
{
    memset(&g_sensor_data, 0, sizeof(Sensor_Data_t));

    GasSensor_Init();
    PIR_Init();
    GPS_Init();
    D6_Radar_Init();

    AHT20_Init();
    BMP280_Init();

    GasSensor_PowerOn();
}

void SensorData_UpdateAll(void)
{
    AHT20_ReadData(&g_sensor_data.aht20);
    BMP280_ReadData(&g_sensor_data.bmp280);

    GPS_Update();
    GPS_GetData(&g_sensor_data.gps);

    D6_Radar_ReadData(&g_sensor_data.radar);

    g_sensor_data.gas_level = GasSensor_ReadLevel();
    g_sensor_data.human_detect = PIR_IsDetected();
}

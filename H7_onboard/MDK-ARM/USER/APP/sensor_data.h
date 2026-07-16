#ifndef SENSOR_DATA_H
#define SENSOR_DATA_H

#include "stdint.h"
#include "ATH20.h"
#include "BMP280.h"
#include "gps.h"
#include "d6_radar.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    AHT20_Data_t aht20;
    BMP280_Data_t bmp280;
    GPS_Data_t gps;
    D6_Radar_Data_t radar;
    uint16_t gas_level;
    uint8_t human_detect;
} Sensor_Data_t;

extern Sensor_Data_t g_sensor_data;

void SensorData_Init(void);
void SensorData_UpdateAll(void);

#ifdef __cplusplus
}
#endif

#endif

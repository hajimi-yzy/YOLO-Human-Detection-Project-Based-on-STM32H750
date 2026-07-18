#ifndef GPS_H
#define GPS_H

#include "stdint.h"

#define GPS_NMEA_MAX_LEN  128

typedef struct {
    double latitude;
    double longitude;
    float altitude;
    float speed;
    float course;
    uint8_t satellites;
    uint8_t fix_type;
    uint8_t valid;
} GPS_Data_t;

typedef struct {
    uint8_t hour;
    uint8_t minute;
    uint8_t second;
    uint16_t year;
    uint8_t month;
    uint8_t day;
} GPS_DateTime_t;

void GPS_Init(void);
void GPS_Update(void);
uint8_t GPS_GetData(GPS_Data_t *data);
uint8_t GPS_IsFixed(void);

#endif

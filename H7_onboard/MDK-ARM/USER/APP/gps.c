#include "gps.h"
#include "SC16IS752.h"
#include <string.h>

static GPS_Data_t g_gps_data = {0};
static GPS_DateTime_t g_gps_datetime = {0};
static uint8_t g_gps_initialized = 0;

static char g_rx_buf[GPS_NMEA_MAX_LEN];
static uint16_t g_rx_idx = 0;

static uint8_t NMEA_GetField(char *line, uint8_t field_num, char *buf, uint8_t buf_len)
{
    uint8_t comma_count = 0;
    uint8_t i = 0;
    char *start = line;

    while (*start && comma_count < field_num) {
        if (*start == ',') {
            comma_count++;
        }
        start++;
    }

    if (comma_count != field_num) {
        buf[0] = '\0';
        return 0;
    }

    while (*start && *start != ',' && *start != '*' && *start != '\r' && *start != '\n' && i < buf_len - 1) {
        buf[i++] = *start++;
    }
    buf[i] = '\0';

    return i;
}

static double NMEA_ParseCoord(char *buf, char dir)
{
    double coord = 0.0;
    if (strlen(buf) == 0) return 0.0;

    double raw = atof(buf);
    int deg = (int)(raw / 100);
    double min = raw - deg * 100;
    coord = deg + min / 60.0;

    if (dir == 'S' || dir == 'W') {
        coord = -coord;
    }
    return coord;
}

static void NMEA_ParseRMC(char *line)
{
    char buf[32];

    NMEA_GetField(line, 1, buf, sizeof(buf));
    if (strlen(buf) >= 6) {
        g_gps_datetime.hour = (buf[0] - '0') * 10 + (buf[1] - '0');
        g_gps_datetime.minute = (buf[2] - '0') * 10 + (buf[3] - '0');
        g_gps_datetime.second = (buf[4] - '0') * 10 + (buf[5] - '0');
    }

    NMEA_GetField(line, 2, buf, sizeof(buf));
    g_gps_data.valid = (buf[0] == 'A') ? 1 : 0;

    NMEA_GetField(line, 3, buf, sizeof(buf));
    NMEA_GetField(line, 4, buf, sizeof(buf));
    char ns = buf[0];

    NMEA_GetField(line, 5, buf, sizeof(buf));
    g_gps_data.latitude = NMEA_ParseCoord(buf, ns);

    NMEA_GetField(line, 6, buf, sizeof(buf));
    char ew = buf[0];
    NMEA_GetField(line, 7, buf, sizeof(buf));
    g_gps_data.longitude = NMEA_ParseCoord(buf, ew);

    NMEA_GetField(line, 8, buf, sizeof(buf));
    if (strlen(buf) > 0) {
        g_gps_data.speed = (float)atof(buf) * 1.852f;
    }

    NMEA_GetField(line, 9, buf, sizeof(buf));
    if (strlen(buf) >= 6) {
        g_gps_datetime.day = (buf[0] - '0') * 10 + (buf[1] - '0');
        g_gps_datetime.month = (buf[2] - '0') * 10 + (buf[3] - '0');
        g_gps_datetime.year = 2000 + (buf[4] - '0') * 10 + (buf[5] - '0');
    }

    g_gps_datetime.hour += 8;
    if (g_gps_datetime.hour >= 24) {
        g_gps_datetime.hour -= 24;
        g_gps_datetime.day++;
    }
}

static void NMEA_ParseGGA(char *line)
{
    char buf[32];

    NMEA_GetField(line, 7, buf, sizeof(buf));
    if (strlen(buf) > 0) {
        g_gps_data.satellites = (uint8_t)atoi(buf);
        g_gps_data.fix_type = (g_gps_data.satellites > 0) ? 3 : 0;
    }

    NMEA_GetField(line, 9, buf, sizeof(buf));
    if (strlen(buf) > 0) {
        g_gps_data.altitude = (float)atof(buf);
    }
}

static void NMEA_Parse(char *line)
{
    if (line[0] != '$') return;

    if (strstr(line, "RMC") != NULL) {
        NMEA_ParseRMC(line);
    } else if (strstr(line, "GGA") != NULL) {
        NMEA_ParseGGA(line);
    }
}

void GPS_Init(void)
{
    SC16IS752_Init();

    g_gps_initialized = 1;
    g_rx_idx = 0;
    memset(&g_gps_data, 0, sizeof(GPS_Data_t));
    memset(&g_gps_datetime, 0, sizeof(GPS_DateTime_t));
}

void GPS_Update(void)
{
    if (!g_gps_initialized) return;

    uint8_t byte;
    while (SC16IS752_Available(SC16IS752_CHAN_A) > 0) {
        SC16IS752_Receive(SC16IS752_CHAN_A, &byte, 1);

        if (byte == '$') {
            g_rx_idx = 0;
            g_rx_buf[g_rx_idx++] = byte;
        } else if (g_rx_idx > 0 && g_rx_idx < GPS_NMEA_MAX_LEN - 1) {
            if (byte == '\n' || byte == '\r') {
                g_rx_buf[g_rx_idx] = '\0';
                NMEA_Parse(g_rx_buf);
                g_rx_idx = 0;
            } else {
                g_rx_buf[g_rx_idx++] = byte;
            }
        }
    }
}

uint8_t GPS_GetData(GPS_Data_t *data)
{
    if (data == NULL) return 0;
    memcpy(data, &g_gps_data, sizeof(GPS_Data_t));
    return g_gps_data.valid;
}

uint8_t GPS_IsFixed(void)
{
    return g_gps_data.valid && g_gps_data.satellites >= 3;
}

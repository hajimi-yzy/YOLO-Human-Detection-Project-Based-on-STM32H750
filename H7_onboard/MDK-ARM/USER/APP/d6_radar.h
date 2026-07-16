#ifndef D6_RADAR_H
#define D6_RADAR_H

#include "stdint.h"

#ifdef __cplusplus
extern "C" {
#endif

#define D6_RADAR_CHANNEL_A   0
#define D6_RADAR_CHANNEL_B   1

#define D6_RADAR_CMD_READ_DISTANCE  0x01
#define D6_RADAR_CMD_SET_BAUD       0x08
#define D6_RADAR_CMD_READ_VERSION   0x10
#define D6_RADAR_CMD_SET_FILTER     0x0A
#define D6_RADAR_CMD_READ_FILTER    0x0B

typedef struct {
    uint16_t distance_a;
    uint16_t distance_b;
    uint8_t strength_a;
    uint8_t strength_b;
    uint8_t channel_a_valid;
    uint8_t channel_b_valid;
} D6_Radar_Data_t;

void D6_Radar_Init(void);
uint8_t D6_Radar_ReadData(D6_Radar_Data_t *data);
uint8_t D6_Radar_ReadSingleChannel(uint8_t channel, uint16_t *distance);
uint8_t D6_Radar_SetFilter(uint8_t level);
uint8_t D6_Radar_GetVersion(char *version, uint8_t len);

#ifdef __cplusplus
}
#endif

#endif

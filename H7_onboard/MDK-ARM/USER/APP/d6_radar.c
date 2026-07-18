#include "d6_radar.h"
#include "SC16IS752.h"
#include "main.h"

static uint8_t radar_initialized = 0;

void D6_Radar_Init(void)
{
    if (radar_initialized) return;

    SC16IS752_SetBaudRate(SC16IS752_CHAN_B, 115200);
    SC16IS752_SetDataFormat(SC16IS752_CHAN_B, 8, 0, 1);

    radar_initialized = 1;
}

uint8_t D6_Radar_ReadSingleChannel(uint8_t channel, uint16_t *distance)
{
    uint8_t cmd = D6_RADAR_CMD_READ_DISTANCE;
    uint8_t rx_buf[4];
    uint8_t available;

    if (!radar_initialized) {
        return 1;
    }

    SC16IS752_Flush(channel);

    if (SC16IS752_Send(channel, &cmd, 1) != 0) {
        return 2;
    }

    for (volatile uint32_t i = 0; i < 50000; i++);

    available = SC16IS752_Available(channel);
    if (available < 4) {
        return 3;
    }

    if (SC16IS752_Receive(channel, rx_buf, 4) != 0) {
        return 4;
    }

    if (rx_buf[0] == 0x59 && rx_buf[1] == 0x35) {
        *distance = ((uint16_t)rx_buf[2] | ((uint16_t)rx_buf[3] << 8));
        return 0;
    }

    return 5;
}

uint8_t D6_Radar_ReadData(D6_Radar_Data_t *data)
{
    uint8_t res_a, res_b;

    data->channel_a_valid = 0;
    data->channel_b_valid = 0;

    res_a = D6_Radar_ReadSingleChannel(SC16IS752_CHAN_A, &data->distance_a);
    if (res_a == 0) {
        data->strength_a = 100;
        data->channel_a_valid = 1;
    }

    res_b = D6_Radar_ReadSingleChannel(SC16IS752_CHAN_B, &data->distance_b);
    if (res_b == 0) {
        data->strength_b = 100;
        data->channel_b_valid = 1;
    }

    return (res_a == 0 || res_b == 0) ? 0 : 1;
}

uint8_t D6_Radar_SetFilter(uint8_t level)
{
    uint8_t cmd[2] = {D6_RADAR_CMD_SET_FILTER, level};
    return SC16IS752_Send(SC16IS752_CHAN_A, cmd, 2);
}

uint8_t D6_Radar_GetVersion(char *version, uint8_t len)
{
    uint8_t cmd = D6_RADAR_CMD_READ_VERSION;
    uint8_t available;

    if (!radar_initialized || version == NULL) {
        return 1;
    }

    SC16IS752_Flush(SC16IS752_CHAN_A);
    SC16IS752_Send(SC16IS752_CHAN_A, &cmd, 1);

    for (volatile uint32_t i = 0; i < 100000; i++);

    available = SC16IS752_Available(SC16IS752_CHAN_A);
    if (available == 0 || available >= len) {
        return 2;
    }

    SC16IS752_Receive(SC16IS752_CHAN_A, (uint8_t *)version, available);
    version[available] = '\0';

    return 0;
}

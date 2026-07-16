#include "SC16IS752.h"
#include "bsp_i2c.h"
#include "main.h"

static uint8_t sc16_initialized = 0;

static void SC16IS752_ChannelInit(uint8_t channel, uint32_t baud)
{
    SC16IS752_WriteReg(channel, SC16IS752_REG_LCR, 0x80);

    uint16_t divisor = (SC16IS752_OSC_FREQ / (SC16IS752_PRESCALE * baud)) / 16;
    SC16IS752_WriteReg(channel, SC16IS752_REG_DLL, divisor & 0xFF);
    SC16IS752_WriteReg(channel, SC16IS752_REG_DLH, (divisor >> 8) & 0xFF);

    SC16IS752_WriteReg(channel, SC16IS752_REG_LCR, 0xBF);
    SC16IS752_WriteReg(channel, SC16IS752_REG_EFR, 0x10);

    SC16IS752_WriteReg(channel, SC16IS752_REG_LCR, 0x03);

    SC16IS752_WriteReg(channel, SC16IS752_REG_FCR, 0x01);
    SC16IS752_WriteReg(channel, SC16IS752_REG_FCR, 0x07);
    SC16IS752_WriteReg(channel, SC16IS752_REG_FCR, 0x01);

    SC16IS752_WriteReg(channel, SC16IS752_REG_MCR, 0x03);
}

void SC16IS752_Init(void)
{
    uint8_t val;

    sc16_initialized = 0;

    BSP_I2C_Init();

    SC16IS752_WriteReg(SC16IS752_CHAN_A, SC16IS752_REG_IOCONTROL, 0x00);

    SC16IS752_WriteReg(SC16IS752_CHAN_A, SC16IS752_REG_LCR, 0xBF);
    SC16IS752_WriteReg(SC16IS752_CHAN_A, SC16IS752_REG_EFR, 0x10);
    SC16IS752_WriteReg(SC16IS752_CHAN_A, SC16IS752_REG_LCR, 0x03);

    SC16IS752_WriteReg(SC16IS752_CHAN_A, SC16IS752_REG_FCR, 0x01);
    SC16IS752_WriteReg(SC16IS752_CHAN_A, SC16IS752_REG_FCR, 0x07);
    SC16IS752_WriteReg(SC16IS752_CHAN_A, SC16IS752_REG_FCR, 0x01);

    SC16IS752_WriteReg(SC16IS752_CHAN_A, SC16IS752_REG_MCR, 0x03);

    SC16IS752_WriteReg(SC16IS752_CHAN_A, SC16IS752_REG_IODIR, 0x00);
    SC16IS752_WriteReg(SC16IS752_CHAN_A, SC16IS752_REG_IOSTATE, 0x00);

    SC16IS752_ChannelInit(SC16IS752_CHAN_A, 9600);

    SC16IS752_ChannelInit(SC16IS752_CHAN_B, 115200);

    SC16IS752_Flush(SC16IS752_CHAN_A);
    SC16IS752_Flush(SC16IS752_CHAN_B);

    sc16_initialized = 1;
}

uint8_t SC16IS752_WriteReg(uint8_t channel, uint8_t reg, uint8_t value)
{
    uint8_t addr = SC16IS752_ADDR;
    if (channel == SC16IS752_CHAN_B) {
        addr |= 0x80;
    }

    return BSP_I2C_WriteReg(addr, reg, value);
}

uint8_t SC16IS752_ReadReg(uint8_t channel, uint8_t reg, uint8_t *value)
{
    uint8_t addr = SC16IS752_ADDR;
    if (channel == SC16IS752_CHAN_B) {
        addr |= 0x80;
    }

    return BSP_I2C_ReadReg(addr, reg, value);
}

void SC16IS752_SetBaudRate(uint8_t channel, uint32_t baud)
{
    uint16_t divisor;
    uint8_t divisor_l, divisor_h;
    uint32_t prescale = SC16IS752_PRESCALE;

    divisor = (SC16IS752_OSC_FREQ / (prescale * baud)) / 16;

    divisor_l = divisor & 0xFF;
    divisor_h = (divisor >> 8) & 0xFF;

    SC16IS752_WriteReg(channel, SC16IS752_REG_LCR, 0x80);

    SC16IS752_WriteReg(channel, SC16IS752_REG_DLL, divisor_l);
    SC16IS752_WriteReg(channel, SC16IS752_REG_DLH, divisor_h);

    SC16IS752_WriteReg(channel, SC16IS752_REG_LCR, 0x03);
}

void SC16IS752_SetDataFormat(uint8_t channel, uint8_t data_bits, uint8_t parity, uint8_t stop_bits)
{
    uint8_t lcr = 0;

    switch (data_bits) {
        case 5: lcr |= 0x00; break;
        case 6: lcr |= 0x01; break;
        case 7: lcr |= 0x02; break;
        case 8: lcr |= 0x03; break;
        default: lcr |= 0x03; break;
    }

    switch (parity) {
        case 0: break;
        case 1: lcr |= 0x08; break;
        case 2: lcr |= 0x18; break;
        default: break;
    }

    if (stop_bits == 2) {
        lcr |= 0x04;
    }

    SC16IS752_WriteReg(channel, SC16IS752_REG_LCR, lcr);
}

uint8_t SC16IS752_Send(uint8_t channel, uint8_t *data, uint16_t len)
{
    uint8_t reg = SC16IS752_REG_THR;
    uint16_t i;

    if (!sc16_initialized) return 1;

    for (i = 0; i < len; i++) {
        SC16IS752_WriteReg(channel, reg, data[i]);
    }

    return 0;
}

uint16_t SC16IS752_Receive(uint8_t channel, uint8_t *data, uint16_t max_len)
{
    uint8_t reg = SC16IS752_REG_RHR;
    uint16_t count = 0;

    if (!sc16_initialized) return 0;

    while (count < max_len) {
        uint8_t lsr;
        SC16IS752_ReadReg(channel, SC16IS752_REG_LSR, &lsr);

        if ((lsr & 0x01) == 0) {
            break;
        }

        SC16IS752_ReadReg(channel, reg, &data[count]);
        count++;
    }

    return count;
}

uint8_t SC16IS752_Available(uint8_t channel)
{
    uint8_t level;
    SC16IS752_ReadReg(channel, SC16IS752_REG_RXLVL, &level);
    return level;
}

void SC16IS752_Flush(uint8_t channel)
{
    uint8_t lsr;
    uint8_t buf[64];

    do {
        SC16IS752_ReadReg(channel, SC16IS752_REG_LSR, &lsr);
        if (lsr & 0x01) {
            SC16IS752_ReadReg(channel, SC16IS752_REG_RHR, &lsr);
        }
        SC16IS752_ReadReg(channel, SC16IS752_REG_RXLVL, &lsr);
    } while (lsr > 0);
}

uint8_t SC16IS752_SetPinDirection(uint8_t pin, uint8_t output)
{
    uint8_t dir;
    if (pin > 7) return 1;

    SC16IS752_ReadReg(SC16IS752_CHAN_A, SC16IS752_REG_IODIR, &dir);

    if (output) {
        dir |= (1 << pin);
    } else {
        dir &= ~(1 << pin);
    }

    return SC16IS752_WriteReg(SC16IS752_CHAN_A, SC16IS752_REG_IODIR, dir);
}

uint8_t SC16IS752_SetPinLevel(uint8_t pin, uint8_t high)
{
    uint8_t state;
    if (pin > 7) return 1;

    SC16IS752_ReadReg(SC16IS752_CHAN_A, SC16IS752_REG_IOSTATE, &state);

    if (high) {
        state |= (1 << pin);
    } else {
        state &= ~(1 << pin);
    }

    return SC16IS752_WriteReg(SC16IS752_CHAN_A, SC16IS752_REG_IOSTATE, state);
}

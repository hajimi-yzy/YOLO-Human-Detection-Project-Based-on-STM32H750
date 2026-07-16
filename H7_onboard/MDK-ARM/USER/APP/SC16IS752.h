#ifndef SC16IS752_H
#define SC16IS752_H

#include "stdint.h"

#define SC16IS752_ADDR        0x9D

#define SC16IS752_CHAN_A      0x00
#define SC16IS752_CHAN_B      0x01

#define SC16IS752_REG_RHR      0x00
#define SC16IS752_REG_THR      0x00
#define SC16IS752_REG_IER      0x01
#define SC16IS752_REG_FCR      0x02
#define SC16IS752_REG_LCR      0x03
#define SC16IS752_REG_MCR      0x04
#define SC16IS752_REG_LSR      0x05
#define SC16IS752_REG_MSR      0x06
#define SC16IS752_REG_SPR      0x07
#define SC16IS752_REG_TCR      0x06
#define SC16IS752_REG_TLR      0x07
#define SC16IS752_REG_TXLVL    0x08
#define SC16IS752_REG_RXLVL     0x09
#define SC16IS752_REG_IODIR    0x0A
#define SC16IS752_REG_IOSTATE   0x0B
#define SC16IS752_REG_IOINTENA  0x0C
#define SC16IS752_REG_IOCONTROL 0x0E
#define SC16IS752_REG_EFCR     0x0F

#define SC16IS752_REG_DLL      0x00
#define SC16IS752_REG_DLH      0x01
#define SC16IS752_REG_EFR      0x02
#define SC16IS752_REG_XON1     0x04
#define SC16IS752_REG_XON2     0x05
#define SC16IS752_REG_XOFF1    0x06
#define SC16IS752_REG_XOFF2    0x07

#define SC16IS752_OSC_FREQ     14745600UL
#define SC16IS752_PRESCALE     1

void SC16IS752_Init(void);
uint8_t SC16IS752_WriteReg(uint8_t channel, uint8_t reg, uint8_t value);
uint8_t SC16IS752_ReadReg(uint8_t channel, uint8_t reg, uint8_t *value);
void SC16IS752_SetBaudRate(uint8_t channel, uint32_t baud);
void SC16IS752_SetDataFormat(uint8_t channel, uint8_t data_bits, uint8_t parity, uint8_t stop_bits);
uint8_t SC16IS752_Send(uint8_t channel, uint8_t *data, uint16_t len);
uint16_t SC16IS752_Receive(uint8_t channel, uint8_t *data, uint16_t max_len);
uint8_t SC16IS752_Available(uint8_t channel);
void SC16IS752_Flush(uint8_t channel);
uint8_t SC16IS752_SetPinDirection(uint8_t pin, uint8_t output);
uint8_t SC16IS752_SetPinLevel(uint8_t pin, uint8_t high);

#endif

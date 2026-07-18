#ifndef BSP_I2C_H
#define BSP_I2C_H

#include "main.h"

#define I2C1_SCL_PIN  GPIO_PIN_9
#define I2C1_SDA_PIN  GPIO_PIN_8
#define I2C1_PORT     GPIOB

#define I2C_TIMEOUT   1000

extern I2C_HandleTypeDef hi2c1;

void BSP_I2C_Init(void);
uint8_t BSP_I2C_Write(uint8_t addr, uint8_t reg, uint8_t *data, uint16_t len);
uint8_t BSP_I2C_Read(uint8_t addr, uint8_t reg, uint8_t *data, uint16_t len);
uint8_t BSP_I2C_WriteReg(uint8_t addr, uint8_t reg, uint8_t value);
uint8_t BSP_I2C_ReadReg(uint8_t addr, uint8_t reg, uint8_t *value);
uint8_t BSP_I2C_WriteBuf(uint8_t addr, uint8_t reg, uint8_t *data, uint16_t len);
uint8_t BSP_I2C_ReadBuf(uint8_t addr, uint8_t reg, uint8_t *data, uint16_t len);

#endif

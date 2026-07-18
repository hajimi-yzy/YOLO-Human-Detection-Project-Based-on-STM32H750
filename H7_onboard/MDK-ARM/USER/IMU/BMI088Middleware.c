#include "BMI088Middleware.h"
#include "main.h"
#include "bsp_dwt.h"
#include "spi.h"
 GPIO_PinState BMI088_MISO_READ(void)
 {
     return HAL_GPIO_ReadPin(GYRO_MISO_GPIO_Port, GYRO_MISO_Pin);
 }
uint8_t BMI088_read_write_byte(uint8_t data)
{
	uint8_t data_r = 0;
	HAL_SPI_TransmitReceive(&hspi1,&data,&data_r,1,0xffff);
    return data_r;
}

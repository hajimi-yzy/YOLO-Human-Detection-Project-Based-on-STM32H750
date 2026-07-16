#ifndef __EEPROM_H_
#define __EEPROM_H_

#include "main.h"


#define STM32_FLASH_BASE 0X08000000
#define STM32_FLASH_SIZE 64
#define STM_SECTOR_SIZE	1024	 
#define EEPROM_START_ADDRESS    0x08008000 /*!< Start address of the 1st page in flash, for EEPROM emulation */

#define send_euler_angle_offset 			2
#define send_total_yaw_angle_offset 	3
#define send_q_offset 								4
#define send_div_l_offset 						5
#define send_div_h_offset 						6
typedef struct
{
	int 	init			;
	float gxoffset	;
	float gyoffset	;
	float gzoffset	;
	float gnorm			;
	float TempWhenCali;
	union
	{ 
		uint16_t cmd_code;
		struct 
		{
			uint16_t reset :1 ;
			uint16_t Cali :1 ;
			uint16_t send_euler_angle :1 ;
			uint16_t send_total_yaw_angle :1 ;
			uint16_t send_q :1 ;
			uint16_t send_div_l :1 ;
			uint16_t send_div_h :1 ;
		}cmd_id;
	}CMD;
	uint16_t send_div ;
}eeprom_data_t		;

uint16_t Read_HalfWord(uint32_t addr);
uint32_t Read_Word(uint32_t addr);
void STMFLASH_ReadData(uint32_t Readaddr,uint16_t *pBuffer,uint16_t NumToRead);
void STMFLASH_Write_NoCheck(uint32_t WriteAddr,uint16_t *pBuffer,uint16_t NumToWrite);
void STMFLASH_Write(uint32_t WriteAddr,uint16_t *pBuffer,uint16_t NumToWrite);
	



#endif 

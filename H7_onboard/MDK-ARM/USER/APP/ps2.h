#ifndef PS2_H
#define PS2_H

#include "main.h"
#include "bsp.h"
#include "dwt_delay_us.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef __packed struct 
{
	uint8_t SELECT:1;
	uint8_t L3:1;
	uint8_t R3:1;
	uint8_t START:1;
	uint8_t UP:1;
	uint8_t RIGHT:1;
	uint8_t DOWN:1;
	uint8_t LEFT:1;
	
	uint8_t L2:1;
	uint8_t R2:1;
	uint8_t L1:1;
	uint8_t R1:1;
	uint8_t CIRCLE:1;
	uint8_t TRIANGLE:1;
	uint8_t CROSS:1;
	uint8_t SQUARE:1;
	
	int8_t RX;
	int8_t RY;
	int8_t LX;
	int8_t LY;
}ps2_t;

typedef __packed struct
{
	uint16_t button;
	uint32_t thumb;
}BT;

#if defined ( __CC_ARM   )
#pragma anon_unions
#endif
typedef union 
{
	BT bt;
	ps2_t data_s;
	uint8_t data_arr[6];
}PS2_u;

#define DATA_I  HAL_GPIO_ReadPin(PS2_DAT_GPIO_Port,PS2_DAT_Pin)           //PA0  输入

#define CMD_H HAL_GPIO_WritePin(PS2_CMD_GPIO_Port,PS2_CMD_Pin,GPIO_PIN_SET)       //命令位高
#define CMD_L HAL_GPIO_WritePin(PS2_CMD_GPIO_Port,PS2_CMD_Pin,GPIO_PIN_RESET)        //命令位低

#define CS_H HAL_GPIO_WritePin(PS2_CS_GPIO_Port,PS2_CS_Pin,GPIO_PIN_SET)       //CS拉高
#define CS_L HAL_GPIO_WritePin(PS2_CS_GPIO_Port,PS2_CS_Pin,GPIO_PIN_RESET)

#define CLK_H HAL_GPIO_WritePin(PS2_CLK_GPIO_Port,PS2_CLK_Pin,GPIO_PIN_SET)     //时钟拉高
#define CLK_L HAL_GPIO_WritePin(PS2_CLK_GPIO_Port,PS2_CLK_Pin,GPIO_PIN_RESET)      //时钟拉低



ps2_t ps2_getvalue(void);
void PS2_SetInit(void);
void PS2_ReadData(void);

#ifdef __cplusplus
}
#endif

#endif

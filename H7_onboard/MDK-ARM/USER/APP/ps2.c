#include "ps2.h"
#include "dwt_delay_us.h"

#define delay_us DWT_Delay_us
#define DELAY_BYTE DWT_Delay_us(20); 
#define DELAY_TIME DWT_Delay_us(20);

PS2_u ps2_u,ps2_u_last,ps2_u_ret;

u8 Comd[2] = {0x01, 0x42}; // 开始命令。请求数据



// 向手柄发送命令
uint8_t PS2_Cmd(u8 CMD)
{
	volatile u16 ref = 0x01;
	uint8_t data = 0;
	for (ref = 0x01; ref < 0x0100; ref <<= 1)
	{
		if (ref & CMD)
		{
			CMD_H; // 输出一位控制位
		}
		else
			CMD_L;

//		CLK_H; // 时钟拉高
//		DELAY_TIME;
//		CLK_L;
//		DELAY_TIME;
//		CLK_H;
//		if (DATA_I)
//			data = ref | data;
		CLK_L;
		DELAY_TIME;
		if (DATA_I)
			data = ref | data;
		CLK_H;
		DELAY_TIME;
	}
	DELAY_BYTE;
	return data;
}
// 判断是否为红灯模式,0x41=模拟绿灯，0x73=模拟红灯
// 返回值；0，红灯模式
//		  其他，其他模式
u8 PS2_RedLight(void)
{
	CS_L;
	PS2_Cmd(Comd[0]); // 开始命令
	PS2_Cmd(Comd[1]); // 请求数据
	CS_H;
	if (ps2_u.data_arr[1] == 0X73)
		return 0;
	else
		return 1;
}

uint8_t read_byte()
{
	u16 ref;
	u8 byte = 0;
	for (ref = 0x01; ref < 0x100; ref <<= 1)
	{
		CLK_H;
		DELAY_TIME;
		if (DATA_I)
			byte |= ref;
		CLK_L;
		DELAY_TIME;
		CLK_H;
	}
	DELAY_BYTE;
	return byte;
}

// 读取手柄数据
void PS2_ReadData(void)
{
	volatile u8 index = 0;
	uint8_t attack;
	CS_L;
	PS2_Cmd(Comd[0]); // 开始命令
	PS2_Cmd(Comd[1]); // 请求数据
	attack = read_byte();
	if(attack!=0x5A) //数据错误
		return;
	for (index = 0; index < 6; index++) // 开始接受数据
	{
		ps2_u.data_arr[index] = read_byte();
	}
	CS_H;
	ps2_u.data_arr[0] = ~ps2_u.data_arr[0]; // 源数据0是按下，这里取个反
	ps2_u.data_arr[1] = ~ps2_u.data_arr[1];
	for (index = 2; index < 6; index++) // 数据处理
	{
		ps2_u.data_arr[index] = (uint8_t)ps2_u.data_arr[index] - 128;
	}
	ps2_u.data_s.LY = ps2_u.data_s.LY == -128 ? 127 : (-ps2_u.data_s.LY - 1);
	ps2_u.data_s.RY = ps2_u.data_s.RY == -128 ? 127 : (-ps2_u.data_s.RY - 1);

	// 死区处理
	ps2_u.data_s.RX = (ps2_u.data_s.RX < 3 && ps2_u.data_s.RX > -3) ? 0 : ps2_u.data_s.RX;
	ps2_u.data_s.RY = (ps2_u.data_s.RY < 3 && ps2_u.data_s.RY > -3) ? 0 : ps2_u.data_s.RY;
	ps2_u.data_s.LX = (ps2_u.data_s.LX < 3 && ps2_u.data_s.LX > -3) ? 0 : ps2_u.data_s.LX;
	ps2_u.data_s.LY = (ps2_u.data_s.LY < 3 && ps2_u.data_s.LY > -3) ? 0 : ps2_u.data_s.LY;

	//数据检验
	ps2_u_ret.bt.thumb = ps2_u.bt.thumb; //摇杆不容易出错因此直接回传不检验
	if(ps2_u.bt.button==ps2_u_last.bt.button)
		ps2_u_ret.bt.button =  ps2_u.bt.button; //按钮检验无误后回传数据
		
	ps2_u_last = ps2_u;
}

// short poll
void PS2_ShortPoll(void)
{
	CS_L;
	delay_us(16);
	PS2_Cmd(0x01);
	PS2_Cmd(0x42);
	PS2_Cmd(0X00);
	PS2_Cmd(0x00);
	PS2_Cmd(0x00);
	CS_H;
	delay_us(16);
}
// 进入配置
void PS2_EnterConfing(void)
{
	CS_L;
	delay_us(16);
	PS2_Cmd(0x01);
	PS2_Cmd(0x43);
	PS2_Cmd(0X00);
	PS2_Cmd(0x01);
	PS2_Cmd(0x00);
	PS2_Cmd(0X00);
	PS2_Cmd(0X00);
	PS2_Cmd(0X00);
	PS2_Cmd(0X00);
	CS_H;
	delay_us(16);
}
// 发送模式设置
void PS2_TurnOnAnalogMode(void)
{
	CS_L;
	PS2_Cmd(0x01);
	PS2_Cmd(0x44);
	PS2_Cmd(0X00);
	PS2_Cmd(0x01); // analog=0x01;DATA_Igital=0x00  软件设置发送模式
	PS2_Cmd(0x03); // Ox03锁存设置，即不可通过按键“MODE”设置模式。
				   // 0xEE不锁存软件设置，可通过按键“MODE”设置模式。
	PS2_Cmd(0X00);
	PS2_Cmd(0X00);
	PS2_Cmd(0X00);
	PS2_Cmd(0X00);
	CS_H;
	delay_us(16);
}

// 完成并保存配置
void PS2_ExitConfing(void)
{
	CS_L;
	delay_us(16);
	PS2_Cmd(0x01);
	PS2_Cmd(0x43);
	PS2_Cmd(0X00);
	PS2_Cmd(0x00);
	PS2_Cmd(0x5A);
	PS2_Cmd(0x5A);
	PS2_Cmd(0x5A);
	PS2_Cmd(0x5A);
	PS2_Cmd(0x5A);
	CS_H;
	delay_us(16);
}

// 手柄配置初始化
void PS2_SetInit(void)
{
	PS2_ShortPoll();
	PS2_ShortPoll();
	PS2_ShortPoll();
	PS2_EnterConfing();		// 进入配置模式
	PS2_TurnOnAnalogMode(); // “红绿灯”配置模式，并选择是否保存
	// PS2_VibrationMode();	//开启震动模式
	PS2_ExitConfing(); // 完成并保存配置
}

ps2_t ps2_getvalue(void)
{
	return ps2_u_ret.data_s;
}


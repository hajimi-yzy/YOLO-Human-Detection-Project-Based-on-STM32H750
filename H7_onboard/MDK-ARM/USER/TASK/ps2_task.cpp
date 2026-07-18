#include "ps2_task.h"
#include "cmsis_os.h"
#include "ps2.h"
#include "my_math.h"

extern PS2_u ps2_u_ret;
extern osThreadId LegControl_taskHandle;


/*
PS2遥控器功能：
默认为行走模式：左侧摇杆控制机身高度和转弯，右侧摇杆控制机器人行走方向
点按L1进入位置控制模式：左侧摇杆控制机身高度，右侧摇杆控制机器人XY方向位置
点按L2进入姿态控制模式：左侧摇杆控制yaw轴，右侧摇杆控制pitch轴和roll轴
点按B切换步态模式（依次在2，3，6步态切换）
点按A停止机器人行动，再按一次回复(已取消)
长按Y让机器人机身姿态与位置归零
点按X控制是否开启陀螺仪平衡
*/


//ps2数据接收任务
extern "C"
{	
void ps2_task(void const *argument)
{
	uint8_t is_suspend=false;
	user_single_key key_hexapod_ctrl_stop = single_Press.key_init();
	osDelay(1000); //刚上电可能会有波纹，直接初始化会导致初始化失败，故延迟一会再初始化
	PS2_SetInit(); //PS2初始化
	while(1)
	{ 
		PS2_ReadData(); //读取ps2数据
		{
		//若按下A则让机器人暂停
//		if(single_Press.if_single_press(key_hexapod_ctrl_stop,ps2_u_ret.data_s.CROSS)==true)
//		{
//			if(is_suspend==0)
//			{
//				osThreadSuspend(LegControl_taskHandle);	
//				is_suspend=true;
//			}
//			else
//			{
//				osThreadResume(LegControl_taskHandle);	
//				is_suspend=false;
//			}
//		}
		}
		osDelay(10);
	}
}
}

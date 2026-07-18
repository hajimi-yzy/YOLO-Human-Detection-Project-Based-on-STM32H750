#ifndef LEGCONTROL_TASK_H
#define LEGCONTROL_TASK_H

#include "my_math.h"
#include "leg.h"
#include "gait_prg.h"
#include "ps2.h"


/* 腿的顺序(俯视)
  4   1
  5   2
  6   3
*/
#define LEG1_JOINT1_OFFSET PI / 4.f
#define LEG2_JOINT1_OFFSET 0
#define LEG3_JOINT1_OFFSET -PI / 4.f
#define LEG4_JOINT1_OFFSET 3.f * PI / 4.f
#define LEG5_JOINT1_OFFSET PI
#define LEG6_JOINT1_OFFSET -3.f * PI / 4.f
#define LEG_JOINT2_OFFSET 50.f/180.f * PI
#define LEG_JOINT3_OFFSET -42.f/180.f*PI

#define HEXAPOD_MIN_HEIGHT -30.0f
#define HEXAPOD_MAX_HEIGHT 70.0f
#define HEXAPOD_MIN_X -40.0f
#define HEXAPOD_MAX_X 40.0f
#define HEXAPOD_MIN_Y -40.0f
#define HEXAPOD_MAX_Y 40.0f


#define HEXAPOD_MIN_X_ROTATE -15.0f  // 机体X轴旋转角度最小为-15度
#define HEXAPOD_MAX_X_ROTATE 15.0f   // 机体X轴旋转角度最大为 15度
#define HEXAPOD_MIN_Y_ROTATE -10.0f  // 机体Y轴旋转角度最小为-10度
#define HEXAPOD_MAX_Y_ROTATE 10.0f   // 机体Y轴旋转角度最大为 10度
#define HEXAPOD_MIN_Z_ROTATE -25.0f  // 机体Z轴旋转角度最小为-25度
#define HEXAPOD_MAX_Z_ROTATE 25.0f   // 机体Z轴旋转角度最大为 25度

/* PID */
#define INS_X_PID_KP 0.015f
#define INS_X_PID_KI 0.0f
#define INS_X_PID_KD 0.5f

#define INS_Y_PID_KP 0.015f
#define INS_Y_PID_KI 0.0f
#define INS_Y_PID_KD 0.5f


/* FOF一阶低通滤波器参数 */
#define VELOCITY_FOF_K 0.3f
#define BODY_POS_FOF_K 0.05f
#define BODY_ANGLE_FOF_K 0.04f


#define RES_RATIO 128

#define BODY_ANGLE_SENSI_X  HEXAPOD_MAX_X_ROTATE/RES_RATIO  // 控制机身角度灵敏度
#define BODY_ANGLE_SENSI_Y  HEXAPOD_MAX_Y_ROTATE/RES_RATIO
#define BODY_ANGLE_SENSI_Z  HEXAPOD_MAX_Z_ROTATE/RES_RATIO
#define BODY_INS_ANGLE_SENSI_X 4*BODY_ANGLE_SENSI_X/N_POINTS  // 机体自稳恢复时快速收敛到平衡角度的恢复速度
#define BODY_INS_ANGLE_SENSI_Y 4*BODY_ANGLE_SENSI_Y/N_POINTS
#define BODY_INS_ANGLE_SENSI_Z 4*BODY_ANGLE_SENSI_Z/N_POINTS

#define BODY_POS_SENSI 0.006f  // 机身位置灵敏度



typedef enum
{
    HEXAPOD_MOVE=0,
    HEXAPOD_BODY_ANGEL_CONTROL,
    HEXAPOD_BODY_POS_CONTROL,
} Hexapod_mode_e;

typedef enum
{
    INS_OFF=0,
    INS_ON,
}INS_SW_e;

typedef enum
{
    ARM_ON,
    ARM_OFF,
}ARM_SW_e;

class Hexapod
{
public:
    Leg legs[6];                  // 机械腿
    Velocity velocity;             // 机体速度
    Hexapod_mode_e ctrl_mode;    // 控制模式
    INS_SW_e ins_sw;             // 是否开启陀螺仪自稳
    Position3 body_pos;           // 机身位置
    Position3 body_angle;         // 机身角度
    Position3 ins_angle;
    Position3 ins_angle_set;
    PID ins_pid_x;               // X轴pid
    PID ins_pid_y;               // Y轴pid
    First_order_filter velocity_fof[3];
    First_order_filter body_pos_fof[3];
    First_order_filter body_angle_fof[3];
    gait_mode_e gait_mode;       // 步态模式
    user_single_key key_body_angle;
    user_single_key key_body_pos;
    user_single_key key_ins_mode;
    user_single_key key_gait_mode;

    bool ins_flag;
    void Init();
    void velocity_cal(const ps2_t &remote_data);
    void body_position_cal(const ps2_t &remote_data);
    void body_angle_cal(const ps2_t &remote_data);
    void mode_select(const ps2_t &remote_data);
    void body_angle_and_pos_zero(const ps2_t &remote_data);
    void move(uint32_t round_time);
    void gait_mode_switch(const ps2_t &remote_data);
};


#endif

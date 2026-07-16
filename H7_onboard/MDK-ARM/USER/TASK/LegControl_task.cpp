#include "LegControl_task.h"
#include "gait_prg.h"
#include "cmsis_os.h"
#include "leg.h"
#include "main.h"
#include "INS_interface.h"
#include "bsp_dwt.h"

using namespace std;

// 全局变量
uint32_t LegControl_round;      // 控制回合计数器
Hexapod hexapod;                // 机体结构

Gait_prg gait_prg;              // 步态规划
uint32_t round_time;            // 回合时间
Thetas leg_offset[6];           // 腿关节角度偏移，用于机械腿制造公差和出厂偏移的关节角度恢复，以及自稳时的恢复角度

float code_time_ms, code_time_ms_start;

// 任务
static void remote_deal(void);
extern "C"
{
    void LegControl_Task(void const *argument)
    {
        hexapod.Init();
        gait_prg.Init();
        osDelay(100);
        static uint32_t code_time_start, code_time_end, code_time;  // 用于测量循环执行时间，保证每次都是正好一个周期
        while (1)
        {
            code_time_start = xTaskGetTickCount();  // 获取当前systick时间
            code_time_ms_start = DWT_GetTimeline_ms();
            hexapod.ins_angle = INS_I.get_euler_angle();  // 获取机体陀螺仪角度
            remote_deal();
            if (hexapod.velocity.omega >= 0)
                LegControl_round = (++LegControl_round) % N_POINTS;  // 控制回合计数器
            else
            {
                if (LegControl_round == 0)
                    LegControl_round = N_POINTS - 1;
                else
                    LegControl_round--;
            }
            /* 步态规划 */
            gait_prg.CEN_and_pace_cal();
            gait_prg.gait_proggraming();
            /* 开始运动 */
            round_time = gait_prg.get_pace_time() / N_POINTS;  // 计算本次运动每个回合需要的时间
            hexapod.move(round_time);
            //  计算执行时间
            code_time_ms = DWT_GetTimeline_ms() - code_time_ms_start;
            code_time_end = xTaskGetTickCount();  // 获取当前systick时间
            code_time = code_time_end - code_time_start;  // 计算实际执行程序的时间（8ms周期）
            if (code_time < round_time)
                osDelay(round_time - code_time);  // 保证程序执行时间不大于回合时间
            else
                osDelay(1);  // 执行时间超过周期，延时1ms

        }
    }
}

// 初始化机体，包括UART的初始化和参数的初始化
void Hexapod::Init(void)
{
    legs[0] = Leg(&huart1);
    legs[1] = Leg(&huart2);
    legs[2] = Leg(&huart3);
    legs[3] = Leg(&huart4);
    legs[4] = Leg(&huart5);
    legs[5] = Leg(&huart6);
    MX_USART1_UART_Init();
    MX_USART2_UART_Init();
    MX_USART3_UART_Init();
    MX_UART4_Init();
    MX_UART5_Init();
    MX_USART6_UART_Init();
    //  设置腿运动为偏移角度，用于陀螺仪平衡模型，需要设置偏移角度用于陀螺仪平衡角度的偏移值
    leg_offset[0] = Thetas(LEG1_JOINT1_OFFSET, LEG_JOINT2_OFFSET, LEG_JOINT3_OFFSET);
    leg_offset[1] = Thetas(LEG2_JOINT1_OFFSET, LEG_JOINT2_OFFSET, LEG_JOINT3_OFFSET);
    leg_offset[2] = Thetas(LEG3_JOINT1_OFFSET, LEG_JOINT2_OFFSET, LEG_JOINT3_OFFSET);
    leg_offset[3] = Thetas(LEG4_JOINT1_OFFSET, LEG_JOINT2_OFFSET, LEG_JOINT3_OFFSET);
    leg_offset[4] = Thetas(LEG5_JOINT1_OFFSET, LEG_JOINT2_OFFSET, LEG_JOINT3_OFFSET);
    leg_offset[5] = Thetas(LEG6_JOINT1_OFFSET, LEG_JOINT2_OFFSET, LEG_JOINT3_OFFSET);
    ins_pid_x.Init(INS_X_PID_KP, INS_X_PID_KI, INS_X_PID_KD, CIR_OFF);
    ins_pid_y.Init(INS_Y_PID_KP, INS_Y_PID_KI, INS_Y_PID_KD, CIR_OFF);
    velocity_fof[0].set_k_filter(VELOCITY_FOF_K);
    velocity_fof[1].set_k_filter(VELOCITY_FOF_K);
    velocity_fof[2].set_k_filter(VELOCITY_FOF_K);
    body_pos_fof[0].set_k_filter(BODY_POS_FOF_K);
    body_pos_fof[1].set_k_filter(BODY_POS_FOF_K);
    body_pos_fof[2].set_k_filter(BODY_POS_FOF_K);
    body_angle_fof[0].set_k_filter(BODY_ANGLE_FOF_K);
    body_angle_fof[1].set_k_filter(BODY_ANGLE_FOF_K);
    body_angle_fof[2].set_k_filter(BODY_ANGLE_FOF_K);

    key_body_angle = single_Press.key_init();
    key_body_pos = single_Press.key_init();
    key_gait_mode = single_Press.key_init();
    key_ins_mode = single_Press.key_init();
}

// 计算运动速度
void Hexapod::velocity_cal(const ps2_t &remote_data)
{
    if (this->ctrl_mode != HEXAPOD_MOVE)  // 在其他控制模式下，速度为0
    {
        velocity.Vx = 0;
        velocity.Vy = 0;
        velocity.omega = 0;
    }
    else
    {
        velocity.Vx = velocity_fof[0].cal(0.9f * remote_data.RX);
        velocity.Vy = velocity_fof[1].cal(0.9f * remote_data.RY);
        velocity.omega = velocity_fof[2].cal(-0.9f * remote_data.LX);
    }
    if (velocity.Vx > -0.0001f && velocity.Vx < 0.0001f)
        velocity.Vx = 0;
    if (velocity.Vy > -0.0001f && velocity.Vy < 0.0001f)
        velocity.Vy = 0;
    if (velocity.omega > -0.0001f && velocity.omega < 0.0001f)
        velocity.omega = 0;
    gait_prg.set_velocity(velocity);
}

void Hexapod::body_position_cal(const ps2_t &remote_data)
{
    if (this->ctrl_mode != HEXAPOD_BODY_ANGEL_CONTROL)  // 在非姿态控制模式下，摇杆可以控制z高度
        body_pos.z += BODY_POS_SENSI * remote_data.LY;
    if (this->ctrl_mode == HEXAPOD_BODY_POS_CONTROL)  // 在机身位置控制模式下，控制xy位置
    {
        body_pos.y = HEXAPOD_MAX_Y / 128.0f * remote_data.RY;
        body_pos.x = -HEXAPOD_MIN_X / 128.0f * remote_data.RX;
    }
    //  限幅
    value_limit(body_pos.z, HEXAPOD_MIN_HEIGHT, HEXAPOD_MAX_HEIGHT);
    value_limit(body_pos.y, HEXAPOD_MIN_Y, HEXAPOD_MAX_Y);
    value_limit(body_pos.x, HEXAPOD_MIN_X, HEXAPOD_MAX_X);
    //  一阶低通滤波器
    body_pos.y = body_pos_fof[1].cal(body_pos.y);
    body_pos.x = body_pos_fof[2].cal(body_pos.x);
    gait_prg.set_body_position(body_pos);
}

void Hexapod::body_angle_cal(const ps2_t &remote_data)
{
    static Position3 body_angle_temp, ins_angle_diff;

    if (this->ins_sw == INS_ON)
    {
        if (this->ctrl_mode == HEXAPOD_BODY_ANGEL_CONTROL)  // 在姿态控制模式下，摇杆控制姿态
        {
            ins_angle_set.x += BODY_INS_ANGLE_SENSI_X * remote_data.RY;
            ins_angle_set.y += BODY_INS_ANGLE_SENSI_Y * remote_data.RX;
            ins_angle_set.z -= BODY_INS_ANGLE_SENSI_Z * remote_data.LX;
            ins_angle_set.z = loop_constrain(ins_angle_set.z, -180, 180);  // 限幅角度到-180到180度
        }
        else
            ins_angle_set.z = ins_angle.z;  // 其他模式下不改变z，xy会重置

        ins_angle_diff = ins_angle_set - ins_angle;
        ins_angle_diff.z = loop_constrain(ins_angle_diff.z,-180,180);
        body_angle_temp = body_angle - ins_angle_diff;

        body_angle_fof[0].set_k_filter(2*BODY_ANGLE_FOF_K);
        body_angle_fof[1].set_k_filter(2*BODY_ANGLE_FOF_K);  // 加快响应时间
        body_angle_fof[2].set_k_filter(1.5f*BODY_ANGLE_FOF_K);
    }
    else if (this->ctrl_mode == HEXAPOD_BODY_ANGEL_CONTROL)  // 切换到机体姿态控制模式
    {
        body_angle_temp.x = -BODY_ANGLE_SENSI_X * remote_data.RY;
        body_angle_temp.y = -BODY_ANGLE_SENSI_Y * remote_data.RX;
        body_angle_temp.z = BODY_ANGLE_SENSI_Z * remote_data.LX;

        body_angle_fof[0].set_k_filter(BODY_ANGLE_FOF_K);
        body_angle_fof[1].set_k_filter(BODY_ANGLE_FOF_K);  // 相当于一阶低通滤波器
        body_angle_fof[2].set_k_filter(BODY_ANGLE_FOF_K);
    }

    body_angle.x = body_angle_fof[0].cal(body_angle_temp.x);
    body_angle.y = body_angle_fof[1].cal(body_angle_temp.y);
    body_angle.z = body_angle_fof[2].cal(body_angle_temp.z);
    if (this->ins_sw == INS_ON)  // 开启陀螺仪自稳时，更新当前陀螺仪角度
    {
        if (value_limit(body_angle.x, HEXAPOD_MIN_X_ROTATE, HEXAPOD_MAX_X_ROTATE))
            ins_angle_set.x = ins_angle.x;
        if (value_limit(body_angle.y, HEXAPOD_MIN_Y_ROTATE, HEXAPOD_MAX_Y_ROTATE))
            ins_angle_set.y = ins_angle.y;
        if (value_limit(body_angle.z, HEXAPOD_MIN_Z_ROTATE, HEXAPOD_MAX_Z_ROTATE))
            ins_angle_set.z = ins_angle.z;
    }
    else
    {
        value_limit(body_angle.x, HEXAPOD_MIN_X_ROTATE, HEXAPOD_MAX_X_ROTATE);
        value_limit(body_angle.y, HEXAPOD_MIN_Y_ROTATE, HEXAPOD_MAX_Y_ROTATE);
        value_limit(body_angle.z, HEXAPOD_MIN_Z_ROTATE, HEXAPOD_MAX_Z_ROTATE);
    }

    body_angle_temp = body_angle / 180.f * PI;  // 转换为平衡模型的用户角度
    gait_prg.set_body_rotate_angle(body_angle_temp);
}

/*
 * @brief 处理遥控器数据，主要用于模式切换
 * @param remote_data 遥控数据
 */
void Hexapod::body_angle_and_pos_zero(const ps2_t &remote_data)
{
    if (remote_data.CIRCLE == 1)  // 按下圆圈按钮复位
    {
        this->ins_angle_set = this->ins_angle;
        this->body_angle.zero();
        this->body_pos.zero();
        gait_prg.set_body_rotate_angle(body_angle);
        gait_prg.set_body_position(body_pos);
    }
}

void Hexapod::mode_select(const ps2_t &remote_data)
{
    if (single_Press.if_single_press(key_body_pos, remote_data.L1))
    {
        if (ctrl_mode != HEXAPOD_BODY_POS_CONTROL)
            ctrl_mode = HEXAPOD_BODY_POS_CONTROL;
        else
            ctrl_mode = HEXAPOD_MOVE;
    }
    else if (single_Press.if_single_press(key_body_angle, remote_data.L2))
    {
        if (ctrl_mode != HEXAPOD_BODY_ANGEL_CONTROL)
            ctrl_mode = HEXAPOD_BODY_ANGEL_CONTROL;
        else
            ctrl_mode = HEXAPOD_MOVE;
    }
    if (single_Press.if_single_press(key_ins_mode, remote_data.SQUARE) && INS_I.check_if_init_success() == true)
    {
        this->ins_sw = (INS_SW_e)!(this->ins_sw);
        if (this->ins_sw == INS_ON)  // 开启陀螺仪自稳模式
            this->ins_angle_set = this->ins_angle;  // 将设定角度设为当前陀螺仪角度
    }
}

void Hexapod::gait_mode_switch(const ps2_t &remote_data)
{
    if (single_Press.if_single_press(key_gait_mode, remote_data.TRIANGLE))
    {
        this->gait_mode = (gait_mode_e)(((uint8_t)this->gait_mode + 1) % 3);  // 循环切换步态
    }
    gait_prg.gait_mode = this->gait_mode;
}

ps2_t remote_data;
static void remote_deal(void)
{
    remote_data = ps2_getvalue();
    hexapod.mode_select(remote_data);
    hexapod.velocity_cal(remote_data);
    hexapod.body_angle_cal(remote_data);
    hexapod.body_position_cal(remote_data);
    hexapod.body_angle_and_pos_zero(remote_data);
    hexapod.gait_mode_switch(remote_data);
}

/*
 * @brief 用户腿部运动命令
 * @param round_time 回合时间，单位ms
 */
void Hexapod::move(uint32_t round_time)
{
    Thetas theta_temp;
    for (int i = 0; i < 6; i++)
    {
        theta_temp = (gait_prg.actions[i].thetas[LegControl_round]) - leg_offset[i];
        theta_temp.angle[0] = loop_constrain(theta_temp.angle[0], -PI, PI);  // 循环限幅
        legs[i].set_thetas(theta_temp);  // 设置用户腿的各关节角度
        legs[i].set_time(round_time);
        legs[i].move_read();
    }
}

extern "C"
void legs_Tx_Callback(UART_HandleTypeDef *huart)
{
    HAL_HalfDuplex_EnableReceiver(huart);
    switch ((uint32_t)(huart->Instance))
    {
    case (uint32_t)USART1:
        HAL_UART_Receive_IT(huart, hexapod.legs[0].get_reciv_buffer_p(), RECV_SERVO_POS_READ_LEN + 3);
        break;
    case (uint32_t)USART2:
        HAL_UART_Receive_IT(huart, hexapod.legs[1].get_reciv_buffer_p(), RECV_SERVO_POS_READ_LEN + 3);
        break;
    case (uint32_t)USART3:
        HAL_UART_Receive_IT(huart, hexapod.legs[2].get_reciv_buffer_p(), RECV_SERVO_POS_READ_LEN + 3);
        break;
    case (uint32_t)UART4:
        HAL_UART_Receive_IT(huart, hexapod.legs[3].get_reciv_buffer_p(), RECV_SERVO_POS_READ_LEN + 3);
        break;
    case (uint32_t)UART5:
        HAL_UART_Receive_IT(huart, hexapod.legs[4].get_reciv_buffer_p(), RECV_SERVO_POS_READ_LEN + 3);
        break;
    case (uint32_t)USART6:
        HAL_UART_Receive_IT(huart, hexapod.legs[5].get_reciv_buffer_p(), RECV_SERVO_POS_READ_LEN + 3);
        break;
    default:
        break;
    }
}

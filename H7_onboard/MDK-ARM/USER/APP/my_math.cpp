#include "my_math.h"

// 全局变量
Single_Press single_Press;

void Position3::zero()
{
    x = 0;
    y = 0;
    z = 0;
}

Position3 operator+(const Position3 &pos1, const Position3 &pos2)
{
    Position3 pos;
    pos.x = pos1.x + pos2.x;
    pos.y = pos1.y + pos2.y;
    pos.z = pos1.z + pos2.z;
    return pos;
}

Position3 operator-(const Position3 &pos1, const Position3 &pos2)
{
    Position3 pos;
    pos.x = pos1.x - pos2.x;
    pos.y = pos1.y - pos2.y;
    pos.z = pos1.z - pos2.z;
    return pos;
}

Position3 operator*(const Position3 &pos1, float k)
{
    Position3 pos;
    pos.x = pos1.x * k;
    pos.y = pos1.y * k;
    pos.z = pos1.z * k;
    return pos;
}

Position3 operator*(const float k, const Position3 &pos1)
{
    Position3 pos;
    pos.x = pos1.x * k;
    pos.y = pos1.y * k;
    pos.z = pos1.z * k;
    return pos;
}

Position3 operator/(const Position3 &pos1, float k)
{
    Position3 pos;
    pos.x = pos1.x / k;
    pos.y = pos1.y / k;
    pos.z = pos1.z / k;
    return pos;
}

Position3 operator/(const float k, const Position3 &pos1)
{
    Position3 pos;
    pos.x = pos1.x / k;
    pos.y = pos1.y / k;
    pos.z = pos1.z / k;
    return pos;
}

Thetas operator+(const Thetas &theta1, const Thetas &theta2)
{
    Thetas theta;
    theta.angle[0] = theta1.angle[0] + theta2.angle[0];
    theta.angle[1] = theta1.angle[1] + theta2.angle[1];
    theta.angle[2] = theta1.angle[2] + theta2.angle[2];
    return theta;
}

Thetas operator-(const Thetas &theta1, const Thetas &theta2)
{
    Thetas theta;
    theta.angle[0] = theta1.angle[0] - theta2.angle[0];
    theta.angle[1] = theta1.angle[1] - theta2.angle[1];
    theta.angle[2] = theta1.angle[2] - theta2.angle[2];
    return theta;
}

Thetas &Thetas::operator=(const float angles[3])
{
    this->angle[0] = angles[0];
    this->angle[1] = angles[1];
    this->angle[2] = angles[2];
    return *this;
}

Thetas::Thetas(const float angles[3])
{
    this->angle[0] = angles[0];
    this->angle[1] = angles[1];
    this->angle[2] = angles[2];
}

// 若达到界限则返回1
bool value_limit(float &val, float min, float max)
{
    bool if_reach_bound = false;
    if (val >= max)
    {
        val = max;
        if_reach_bound = true;
    }
    if (val <= min)
    {
        val = min;
        if_reach_bound = true;
    }
    return if_reach_bound;
}

PID::PID(float kp, float ki, float kd, Cir_mode cicir_moder)
{
    this->kp = kp;
    this->ki = ki;
    this->kd = kd;
    this->cir_mode = cir_mode;
}

float PID::cal(float current_val, float set_val)
{
    this->error[2] = this->error[1];
    this->error[1] = this->error[0];
    this->current_val = current_val;
    this->set_val = set_val;
    this->error[0] = set_val - current_val;

    switch (this->cir_mode)
    {
    case CIR_OFF:
        break;
    case CIR_ON:
        if (this->error[0] > PI / 2)
            this->error[0] -= PI;
        else if (this->error[0] < -PI / 2)
            this->error[0] += PI;
        break;
    default:
        break;
    }
    this->pout = this->kp * this->error[0]; // 比例项计算
    this->iout = this->ki * this->error[0]; // 积分项计算
    // 微分项计算
    this->Derror[2] = this->Derror[1];
    this->Derror[1] = this->Derror[0];
    this->Derror[0] = (this->error[0] - this->error[1]);
    this->dout = this->kd * this->Derror[0];
    this->out = this->pout + this->iout + this->dout;
    return this->out;
}

void PID::Init(float kp, float ki, float kd, Cir_mode cir_mode)
{
    this->kp = kp;
    this->ki = ki;
    this->kd = kd;
    this->cir_mode = cir_mode;
}

void First_order_filter::set_k_filter(float k_filter)
{
    this->k_filter = k_filter;
}

float First_order_filter::cal(float input)
{
    this->out = this->k_filter * input + (1 - k_filter) * this->last_input;
    this->last_input = this->out;
    return this->out;
}

void Diff_Limit::set_diff(float diff)
{
    this->diff = diff;
}

void Diff_Limit::set_fre(uint32_t fre)
{
    this->fre = fre;
}

float Diff_Limit::cal(float goal_value)
{
    float temp;
    this->goal_value = goal_value;
    if (goal_value > current_value)
    {
        temp = current_value + diff / fre;
        if (temp > goal_value)
        {
            current_value = goal_value;
            return current_value;
        }
        else
            current_value = temp;
    }
    else
    {
        temp = current_value - diff / fre;
        if (temp < goal_value)
        {
            current_value = goal_value;
            return current_value;
        }
        else
            current_value = temp;
    }
    return temp;
}

user_single_key::user_single_key(uint32_t key_index)
{
    this->key_index = key_index;
}

uint32_t user_single_key::get_key_index()
{
    return this->key_index;
}

#include "cmsis_os.h"
user_single_key Single_Press::key_init()
{
    vTaskSuspendAll(); //开启调度锁，防止两个任务同时申请按键
    user_single_key ukey = user_single_key(this->key_count);
    this->key_count++;
    xTaskResumeAll();  //关闭调度锁，回复任务调度
    return ukey;
}

// 判断是否为单次按下按键
bool Single_Press::if_single_press(user_single_key ukey, uint8_t key) 
{
    if (key_flags[ukey.get_key_index()] == 0 && key == 1) // 刚按下按键
    {
        key_flags[ukey.get_key_index()] = 1;
        return true;
    }
    if (key == 0)
    {
        key_flags[ukey.get_key_index()] = 0;
    }
    return false;
}

// 循环限幅
float loop_constrain(float Input, float minValue, float maxValue)
{
    if (maxValue < minValue)
    {
        return Input;
    }

    float len = maxValue - minValue;
    if (Input > maxValue)
    {
        Input -= len;
    }
    else if (Input < minValue)
    {
        Input += len;
    }
    return Input;
}

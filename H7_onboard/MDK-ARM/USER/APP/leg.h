#ifndef LEG_H
#define LEG_H

#include "Servo.h"
#include "usart.h"
#include "my_math.h"


class Leg
{
private:
    uint8_t send_buffer[100];       // 发送缓冲区
    volatile uint8_t reciv_buffer[20]; // 接收缓冲区
    Servo servos[3];
    Thetas theta_set;
    Thetas theta;
    Servo_Broad_Cast servo_broad_cast;
    UART_HandleTypeDef *huart;
    void TX_Enable();
    void RX_Enable();
    void TX_Unable();
public:
    Leg(UART_HandleTypeDef *huart);  // 构造函数
    Leg(){};                         // 无参构造
    void set_thetas(Thetas thetas);  // 设置用户腿的关节角度
    void set_time(uint16_t tims);     // 设置用户腿的移动时间
    void move();                      // 发送移动命令
    void move_wait();                 // 设置用户腿关节角度，并等待开始移动后移动
    void move_start();                // 用户腿开始运动
    void load();                      // 上电
    void unload();                    // 掉电
    void read_angle(uint32_t id);     // 读取关节角度
    void move_read();                 // 设置角度并读取角度
    uint8_t* get_reciv_buffer_p();   // 获取接收缓冲区指针
};




#endif

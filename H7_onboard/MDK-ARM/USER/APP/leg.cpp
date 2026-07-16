#include "leg.h"
#include "stdlib.h"
#include "cmsis_os.h"
#include "main.h"
#include "usart.h"
#include "bsp.h"

// 全局变量
uint8_t receive_buffer[100];
extern uint32_t LegControl_round; // 控制回合

Leg::Leg(UART_HandleTypeDef *huart)
{
	this->huart = huart;
	servos[0] = Servo(1);
	servos[1] = Servo(2);
	servos[2] = Servo(3);
}

void Leg::set_thetas(Thetas theta)
{
	this->theta_set.angle[0] = theta.angle[0];
	this->theta_set.angle[1] = theta.angle[1];
	this->theta_set.angle[2] = -theta.angle[2];  // 第三关节方向反转
	this->servos[0].set_angle(theta.angle[0]);
	this->servos[1].set_angle(theta.angle[1]);
	this->servos[2].set_angle(-theta.angle[2]);  // 第三关节方向反转
}

void Leg::set_time(uint16_t move_time)
{
	servos[0].set_time(move_time);
	servos[1].set_time(move_time);
	servos[2].set_time(move_time);
}

void Leg::move()
{
	this->TX_Enable();
	this->servos[0].move(this->send_buffer);
	this->servos[1].move(this->send_buffer + SERVO_MOVE_TIME_WRITE_LEN + 3);
	this->servos[2].move(this->send_buffer + (SERVO_MOVE_TIME_WRITE_LEN + 3) * 2);
	HAL_UART_Transmit_DMA(this->huart, this->send_buffer, (SERVO_MOVE_TIME_WRITE_LEN + 3) * 3);
}

void Leg::move_wait()
{
	this->TX_Enable();
	this->servos[0].move_wait(this->send_buffer);
	this->servos[1].move_wait(this->send_buffer + SERVO_MOVE_TIME_WAIT_WRITE_LEN + 3);
	this->servos[2].move(this->send_buffer + (SERVO_MOVE_TIME_WAIT_WRITE_LEN + 3) * 2);
	HAL_UART_Transmit_DMA(this->huart, this->send_buffer, (SERVO_MOVE_TIME_WAIT_WRITE_LEN + 3) * 3);
}

void Leg::move_start()
{
	this->TX_Enable();
	this->servo_broad_cast.move_start(this->send_buffer);
	HAL_UART_Transmit_DMA(this->huart, this->send_buffer, SERVO_MOVE_START_LEN + 3);
}

void Leg::load()
{
	this->servo_broad_cast.load(this->send_buffer);
	HAL_UART_Transmit_DMA(this->huart, this->send_buffer, SERVO_LOAD_OR_UNLOAD_WRITE_LEN + 3);
}

void Leg::unload()
{
	this->servo_broad_cast.unload(this->send_buffer);
	HAL_UART_Transmit_DMA(this->huart, this->send_buffer, SERVO_LOAD_OR_UNLOAD_WRITE_LEN + 3);
}

void Leg::read_angle(uint32_t id)
{
	this->TX_Enable();
	this->servos[id - 1].read_angle(this->send_buffer);

	HAL_UART_Transmit(this->huart, this->send_buffer, SERVO_POS_READ_LEN + 3, 1000); // 为了在发送完成后再开启接收，这里不使用DMA
																					 // while (__HAL_UART_GET_FLAG(this->huart, UART_FLAG_TC))
	;																				 // 等待串口发送完成

	this->RX_Enable(); // 使能接收
	HAL_UART_Receive_IT(this->huart, receive_buffer, RECV_SERVO_POS_READ_LEN + 3);

	float angle = ((float)((uint16_t)receive_buffer[5] | ((uint16_t)receive_buffer[6] << 8)) - 500) / 750 * 180;
	this->theta.angle[id - 1] = angle;
	this->servos[id - 1] = angle;
	// this->servos[id - 1].set_angle(angle);

	// this->TX_Enable(); // 发送完毕后使能发送
}


void Leg::move_read()
{
	uint8_t *temp_p = this->send_buffer;
	HAL_HalfDuplex_EnableTransmitter(this->huart);
  
	this->servos[0].move(temp_p);
	temp_p += SERVO_MOVE_TIME_WRITE_LEN + 3;
	this->servos[1].move(temp_p);
	temp_p += SERVO_MOVE_TIME_WRITE_LEN + 3;
	this->servos[2].move(temp_p);
	temp_p += (SERVO_MOVE_TIME_WRITE_LEN + 3);
	this->servos[LegControl_round % 3].read_angle(temp_p); // 每三个回合读取一次角度
	temp_p += SERVO_POS_READ_LEN + 3;
	HAL_UART_Transmit_DMA(this->huart, this->send_buffer, temp_p - this->send_buffer);

	// 处理上一次接收的数据
	if (check_sum(reciv_buffer) != reciv_buffer[RECV_SERVO_POS_READ_LEN + 2])
	{
		HAL_UART_AbortReceive(this->huart);
		return; // 若校验出错则丢弃此次数据,并关闭接收(会在发送中断中重启)
	}
	float angle = ((((int32_t)reciv_buffer[6] << 8)|((int32_t)reciv_buffer[5])) - 500) / 750.f * 180.f;
	this->theta.angle[reciv_buffer[2] - 1] = angle;
	this->servos[reciv_buffer[2] - 1].set_real_angle(((int32_t)(reciv_buffer[6]) << 8)|(int32_t)(reciv_buffer[5]));
}

uint8_t *Leg::get_reciv_buffer_p()
{
	return (uint8_t *)reciv_buffer;
}

// 使能接收
void Leg::RX_Enable()
{
	switch ((uint32_t)(this->huart->Instance))
	{
	case (uint32_t)USART1:
		__LEG1_RXEN();
		break;
	case (uint32_t)USART2:
		__LEG2_RXEN();
		break;
	case (uint32_t)USART3:
		__LEG3_RXEN();
		break;
	case (uint32_t)UART4:
		__LEG4_RXEN();
		break;
	case (uint32_t)UART5:
		__LEG5_RXEN();
		break;
	case (uint32_t)USART6:
		__LEG6_RXEN();
		break;
	default:
		break;
	}
}

// 使能发送
void Leg::TX_Enable()
{
	switch ((uint32_t)(this->huart->Instance))
	{
	case (uint32_t)USART1:
		__LEG1_TXEN();
		break;
	case (uint32_t)USART2:
		__LEG2_TXEN();
		break;
	case (uint32_t)USART3:
		__LEG3_TXEN();
		break;
	case (uint32_t)UART4:
		__LEG4_TXEN();
		break;
	case (uint32_t)UART5:
		__LEG5_TXEN();
		break;
	case (uint32_t)USART6:
		__LEG6_TXEN();
		break;
	default:
		break;
	}
}

void Leg::TX_Unable()
{
	switch ((uint32_t)(this->huart->Instance))
	{
	case (uint32_t)USART1:
		__LEG1_TXUEN();
		break;
	case (uint32_t)USART2:
		__LEG2_TXUEN();
		break;
	case (uint32_t)USART3:
		__LEG3_TXUEN();
		break;
	case (uint32_t)UART4:
		__LEG4_TXUEN();
		break;
	case (uint32_t)UART5:
		__LEG5_TXUEN();
		break;
	case (uint32_t)USART6:
		__LEG6_TXUEN();
		break;
	default:
		break;
	}
}

void leg_uart_callback(UART_HandleTypeDef *huart)
{
	uint32_t leg_index;
	switch ((uint32_t)(huart->Instance))
	{
	case (uint32_t)USART1:
		leg_index = 1;
		break;
	case (uint32_t)USART2:
		leg_index = 2;
		break;
	case (uint32_t)USART3:
		leg_index = 3;
		break;
	case (uint32_t)UART4:
		leg_index = 4;
		break;
	case (uint32_t)UART5:
		leg_index = 5;
		break;
	case (uint32_t)USART6:
		leg_index = 6;
		break;
	default:
		break;
	}
}

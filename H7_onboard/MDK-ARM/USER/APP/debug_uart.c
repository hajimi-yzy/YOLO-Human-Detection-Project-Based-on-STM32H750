#include "debug_uart.h"
#include "main.h"
#include "usart.h"

uint8_t debug_str[DEBUG_STR_LEN];

/**
 * @brief  串口初始化函数
 * @param  无
 * @return 无
 */

static uint8_t msg;
void Debug_UART_Init(void)
{
    HAL_DEBUG_UART_Init();
    HAL_UART_Receive_IT(&DEBUG_UART_h, &msg, 1);  // 串口开始接收
}

/**
 * @brief  串口中断回调函数
 * @param  p_args
 */
void Debug_UART_Callback(UART_HandleTypeDef *huart)
{
    HAL_UART_Transmit(&DEBUG_UART_h, &msg, 1, 1000);  // 回显原样不处理
    HAL_UART_Receive_IT(&DEBUG_UART_h, &msg, 1);      // 串口开始接收
}

/**
 * @brief  打印函数 - 不是直接使用printf
 *        注意: 需要/n换行以及fflush或者其他函数清除缓冲区才能正常打印，不然会卡在缓冲区
 * @param  str   字符串
 * @param  bytes 字符串长度
 */
void Debug_UART_print(uint8_t *str, int bytes)
{
     //HAL_UART_Transmit(&DEBUG_UART_h, str, bytes, 0xffff);
     HAL_UART_Transmit_DMA(&DEBUG_UART_h, str, bytes);
}

// 重定义fputc
#ifdef __GNUC__
#define PUTCHAR_PROTOTYPE int __io_putchar(int ch)
#else
#define PUTCHAR_PROTOTYPE int fputc(int ch, FILE *f)
#endif
PUTCHAR_PROTOTYPE
{
    HAL_UART_Transmit(&DEBUG_UART_h, (uint8_t *)&ch, 1, 0xFFFF);

    return ch;
}

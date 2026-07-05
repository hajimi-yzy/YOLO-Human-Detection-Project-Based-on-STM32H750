#include "bh1750_daynight.h"

#include "i2c.h"

#define BH1750_I2C_ADDR              (0x23U << 1)
#define BH1750_CMD_POWER_ON          0x01U
#define BH1750_CMD_CONT_H_RES        0x10U

#define BH1750_POLL_MS               1000U
#define BH1750_RETRY_MS              2000U
#define BH1750_CONVERSION_MS         180U
#define BH1750_I2C_STUCK_MS          20U
#define BH1750_NIGHT_ENTER_LUX       30U
#define BH1750_DAY_ENTER_LUX         80U

typedef enum
{
  BH1750_STATE_BOOT = 0,
  BH1750_STATE_SEND_POWER,
  BH1750_STATE_SEND_MODE,
  BH1750_STATE_WAIT_CONVERSION,
  BH1750_STATE_IDLE,
  BH1750_STATE_RETRY_WAIT
} Bh1750_State_t;

typedef enum
{
  BH1750_OP_NONE = 0,
  BH1750_OP_POWER,
  BH1750_OP_MODE,
  BH1750_OP_READ
} Bh1750_Op_t;

static uint8_t s_bh1750_night_mode = 0U;
static uint8_t s_bh1750_fail_count = 0U;
static uint8_t s_bh1750_tx_byte = 0U;
static uint8_t s_bh1750_rx_buf[2] = {0U, 0U};
static uint32_t s_bh1750_last_tick = 0U;
static uint32_t s_bh1750_i2c_start_tick = 0U;
static uint16_t s_bh1750_last_lux = 0U;
static volatile uint8_t s_bh1750_i2c_busy = 0U;
static volatile uint8_t s_bh1750_done = 0U;
static volatile uint8_t s_bh1750_error = 0U;
static volatile Bh1750_Op_t s_bh1750_op = BH1750_OP_NONE;
static Bh1750_State_t s_bh1750_state = BH1750_STATE_BOOT;

void Ircut1_SetNight(uint8_t night_mode)
{
  HAL_GPIO_WritePin(IRCUT1_NET1_PORT, IRCUT1_NET1_PIN,
                    (night_mode != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void Ircut1_Init(void)
{
  Ircut1_SetNight(0U);
}

static void Bh1750_PowerOn(void)
{
  HAL_GPIO_WritePin(BH1750_PWR_PORT, BH1750_PWR_PIN, GPIO_PIN_SET);
}

static uint8_t Bh1750_StartTx(uint8_t command, Bh1750_Op_t op)
{
  if (s_bh1750_i2c_busy != 0U) {
    return 0U;
  }

  s_bh1750_tx_byte = command;
  s_bh1750_done = 0U;
  s_bh1750_error = 0U;
  s_bh1750_op = op;
  s_bh1750_i2c_busy = 1U;
  s_bh1750_i2c_start_tick = HAL_GetTick();

  if (HAL_I2C_Master_Transmit_IT(&hi2c2, BH1750_I2C_ADDR, &s_bh1750_tx_byte, 1U) != HAL_OK) {
    s_bh1750_i2c_busy = 0U;
    s_bh1750_op = BH1750_OP_NONE;
    return 0U;
  }

  return 1U;
}

static uint8_t Bh1750_StartRead(void)
{
  if (s_bh1750_i2c_busy != 0U) {
    return 0U;
  }

  s_bh1750_done = 0U;
  s_bh1750_error = 0U;
  s_bh1750_op = BH1750_OP_READ;
  s_bh1750_i2c_busy = 1U;
  s_bh1750_i2c_start_tick = HAL_GetTick();

  if (HAL_I2C_Master_Receive_IT(&hi2c2, BH1750_I2C_ADDR, s_bh1750_rx_buf, 2U) != HAL_OK) {
    s_bh1750_i2c_busy = 0U;
    s_bh1750_op = BH1750_OP_NONE;
    return 0U;
  }

  return 1U;
}

static void Bh1750_FailRecover(uint32_t now)
{
  s_bh1750_i2c_busy = 0U;
  s_bh1750_done = 0U;
  s_bh1750_error = 0U;
  s_bh1750_op = BH1750_OP_NONE;
  s_bh1750_last_tick = now;
  s_bh1750_state = BH1750_STATE_RETRY_WAIT;

  if (++s_bh1750_fail_count >= 3U) {
    s_bh1750_fail_count = 0U;
    (void)HAL_I2C_DeInit(&hi2c2);
    (void)HAL_I2C_Init(&hi2c2);
  }
}

static void Bh1750_CheckStuck(uint32_t now)
{
  if ((s_bh1750_i2c_busy != 0U) && ((now - s_bh1750_i2c_start_tick) > BH1750_I2C_STUCK_MS)) {
    (void)HAL_I2C_Master_Abort_IT(&hi2c2, BH1750_I2C_ADDR);
    Bh1750_FailRecover(now);
  }
}

void Bh1750_DayNightInit(void)
{
  Bh1750_PowerOn();

  s_bh1750_night_mode = 0U;
  s_bh1750_fail_count = 0U;
  s_bh1750_last_lux = 0U;
  s_bh1750_i2c_busy = 0U;
  s_bh1750_done = 0U;
  s_bh1750_error = 0U;
  s_bh1750_op = BH1750_OP_NONE;
  s_bh1750_state = BH1750_STATE_SEND_POWER;
  s_bh1750_last_tick = HAL_GetTick();
}

uint16_t Bh1750_GetLastLux(void)
{
  return s_bh1750_last_lux;
}

uint8_t Bh1750_GetNightMode(void)
{
  return s_bh1750_night_mode;
}

/* Return -1 if there is no mode change, 0 for day mode, 1 for night mode. */
int8_t Bh1750_DayNightTask(void)
{
  uint32_t now = HAL_GetTick();
  uint16_t raw;
  uint16_t lux;

  Bh1750_CheckStuck(now);

  if (s_bh1750_error != 0U) {
    Bh1750_FailRecover(now);
    return -1;
  }

  if (s_bh1750_done != 0U) {
    s_bh1750_done = 0U;
    s_bh1750_fail_count = 0U;

    if (s_bh1750_op == BH1750_OP_POWER) {
      s_bh1750_op = BH1750_OP_NONE;
      s_bh1750_state = BH1750_STATE_SEND_MODE;
    } else if (s_bh1750_op == BH1750_OP_MODE) {
      s_bh1750_op = BH1750_OP_NONE;
      s_bh1750_state = BH1750_STATE_WAIT_CONVERSION;
      s_bh1750_last_tick = now;
    } else if (s_bh1750_op == BH1750_OP_READ) {
      s_bh1750_op = BH1750_OP_NONE;
      s_bh1750_state = BH1750_STATE_IDLE;
      s_bh1750_last_tick = now;

      raw = (uint16_t)(((uint16_t)s_bh1750_rx_buf[0] << 8) | s_bh1750_rx_buf[1]);
      lux = (uint16_t)(((uint32_t)raw * 5U + 3U) / 6U);
      s_bh1750_last_lux = lux;

      if ((s_bh1750_night_mode == 0U) && (lux <= BH1750_NIGHT_ENTER_LUX)) {
        s_bh1750_night_mode = 1U;
        return 1;
      }

      if ((s_bh1750_night_mode != 0U) && (lux >= BH1750_DAY_ENTER_LUX)) {
        s_bh1750_night_mode = 0U;
        return 0;
      }
    }
  }

  if (s_bh1750_i2c_busy != 0U) {
    return -1;
  }

  switch (s_bh1750_state) {
    case BH1750_STATE_SEND_POWER:
      (void)Bh1750_StartTx(BH1750_CMD_POWER_ON, BH1750_OP_POWER);
      break;

    case BH1750_STATE_SEND_MODE:
      (void)Bh1750_StartTx(BH1750_CMD_CONT_H_RES, BH1750_OP_MODE);
      break;

    case BH1750_STATE_WAIT_CONVERSION:
      if ((now - s_bh1750_last_tick) >= BH1750_CONVERSION_MS) {
        s_bh1750_state = BH1750_STATE_IDLE;
      }
      break;

    case BH1750_STATE_IDLE:
      if ((now - s_bh1750_last_tick) >= BH1750_POLL_MS) {
        (void)Bh1750_StartRead();
      }
      break;

    case BH1750_STATE_RETRY_WAIT:
      if ((now - s_bh1750_last_tick) >= BH1750_RETRY_MS) {
        s_bh1750_state = BH1750_STATE_SEND_POWER;
      }
      break;

    default:
      s_bh1750_state = BH1750_STATE_SEND_POWER;
      break;
  }

  return -1;
}

void Bh1750_I2cTxCpltCallback(I2C_HandleTypeDef *hi2c)
{
  if (hi2c == &hi2c2) {
    s_bh1750_i2c_busy = 0U;
    s_bh1750_done = 1U;
  }
}

void Bh1750_I2cRxCpltCallback(I2C_HandleTypeDef *hi2c)
{
  if (hi2c == &hi2c2) {
    s_bh1750_i2c_busy = 0U;
    s_bh1750_done = 1U;
  }
}

void Bh1750_I2cErrorCallback(I2C_HandleTypeDef *hi2c)
{
  if (hi2c == &hi2c2) {
    s_bh1750_i2c_busy = 0U;
    s_bh1750_error = 1U;
  }
}

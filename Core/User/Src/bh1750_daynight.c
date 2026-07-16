#include "bh1750_daynight.h"

#define BH1750_ADDR_WRITE             0x46U
#define BH1750_ADDR_READ              0x47U
#define BH1750_CMD_POWER_ON           0x01U
#define BH1750_CMD_CONT_H_RES         0x10U

#define BH1750_POLL_MS                1000U
#define BH1750_RETRY_MS               2000U
#define BH1750_CONVERSION_MS          180U
#define BH1750_NIGHT_ENTER_LUX        30U
#define BH1750_DAY_ENTER_LUX          80U

#define BH1750_SCL_PORT               GPIOH
#define BH1750_SCL_PIN                GPIO_PIN_4
#define BH1750_SDA_PORT               GPIOH
#define BH1750_SDA_PIN                GPIO_PIN_5

typedef enum
{
  BH1750_STATE_SEND_POWER = 0,
  BH1750_STATE_SEND_MODE,
  BH1750_STATE_WAIT_CONVERSION,
  BH1750_STATE_IDLE,
  BH1750_STATE_RETRY_WAIT
} Bh1750_State_t;

static uint8_t s_bh1750_night_mode = 0U;
static uint8_t s_bh1750_fail_count = 0U;
static uint32_t s_bh1750_last_tick = 0U;
static uint16_t s_bh1750_last_lux = 0U;
static Bh1750_State_t s_bh1750_state = BH1750_STATE_SEND_POWER;

static void SwI2c_Delay(void)
{
  volatile uint32_t i;

  for (i = 0U; i < 80U; i++) {
    __NOP();
  }
}

static void SwI2c_Scl(uint8_t high)
{
  HAL_GPIO_WritePin(BH1750_SCL_PORT, BH1750_SCL_PIN,
                    (high != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static void SwI2c_Sda(uint8_t high)
{
  HAL_GPIO_WritePin(BH1750_SDA_PORT, BH1750_SDA_PIN,
                    (high != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

static uint8_t SwI2c_ReadSda(void)
{
  return (HAL_GPIO_ReadPin(BH1750_SDA_PORT, BH1750_SDA_PIN) == GPIO_PIN_SET) ? 1U : 0U;
}

static void SwI2c_Start(void)
{
  SwI2c_Sda(1U);
  SwI2c_Scl(1U);
  SwI2c_Delay();
  SwI2c_Sda(0U);
  SwI2c_Delay();
  SwI2c_Scl(0U);
}

static void SwI2c_Stop(void)
{
  SwI2c_Sda(0U);
  SwI2c_Delay();
  SwI2c_Scl(1U);
  SwI2c_Delay();
  SwI2c_Sda(1U);
  SwI2c_Delay();
}

static uint8_t SwI2c_WriteByte(uint8_t data)
{
  uint8_t i;
  uint8_t ack;

  for (i = 0U; i < 8U; i++) {
    SwI2c_Sda((data & 0x80U) != 0U);
    SwI2c_Delay();
    SwI2c_Scl(1U);
    SwI2c_Delay();
    SwI2c_Scl(0U);
    data <<= 1;
  }

  SwI2c_Sda(1U);
  SwI2c_Delay();
  SwI2c_Scl(1U);
  SwI2c_Delay();
  ack = (SwI2c_ReadSda() == 0U) ? 1U : 0U;
  SwI2c_Scl(0U);

  return ack;
}

static uint8_t SwI2c_ReadByte(uint8_t ack)
{
  uint8_t i;
  uint8_t data = 0U;

  SwI2c_Sda(1U);
  for (i = 0U; i < 8U; i++) {
    data <<= 1;
    SwI2c_Scl(1U);
    SwI2c_Delay();
    if (SwI2c_ReadSda() != 0U) {
      data |= 1U;
    }
    SwI2c_Scl(0U);
    SwI2c_Delay();
  }

  SwI2c_Sda((ack != 0U) ? 0U : 1U);
  SwI2c_Delay();
  SwI2c_Scl(1U);
  SwI2c_Delay();
  SwI2c_Scl(0U);
  SwI2c_Sda(1U);

  return data;
}

static uint8_t Bh1750_WriteCommand(uint8_t command)
{
  uint8_t ok;

  SwI2c_Start();
  ok = SwI2c_WriteByte(BH1750_ADDR_WRITE);
  if (ok != 0U) {
    ok = SwI2c_WriteByte(command);
  }
  SwI2c_Stop();

  return ok;
}

static uint8_t Bh1750_ReadRaw(uint16_t *raw)
{
  uint8_t msb;
  uint8_t lsb;

  SwI2c_Start();
  if (SwI2c_WriteByte(BH1750_ADDR_READ) == 0U) {
    SwI2c_Stop();
    return 0U;
  }
  msb = SwI2c_ReadByte(1U);
  lsb = SwI2c_ReadByte(0U);
  SwI2c_Stop();

  *raw = (uint16_t)(((uint16_t)msb << 8) | lsb);
  return 1U;
}

void Ircut1_SetNight(uint8_t night_mode)
{
  HAL_GPIO_WritePin(IRCUT1_NET1_PORT, IRCUT1_NET1_PIN,
                    (night_mode != 0U) ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void Ircut1_Init(void)
{
  Ircut1_SetNight(0U);
}

void Pa3NightOutput_SetNight(uint8_t night_mode)
{
  HAL_GPIO_WritePin(PA3_NIGHT_PORT, PA3_NIGHT_CTRL_PIN,
                    (night_mode != 0U) ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

void Pa3NightOutput_Init(void)
{
  Pa3NightOutput_SetNight(0U);
}

void Bh1750_DayNightInit(void)
{
  HAL_GPIO_WritePin(BH1750_PWR_PORT, BH1750_PWR_PIN, GPIO_PIN_SET);
  SwI2c_Sda(1U);
  SwI2c_Scl(1U);

  s_bh1750_night_mode = 0U;
  s_bh1750_fail_count = 0U;
  s_bh1750_last_lux = 0U;
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

static void Bh1750_Fail(uint32_t now)
{
  s_bh1750_fail_count++;
  s_bh1750_last_tick = now;
  s_bh1750_state = BH1750_STATE_RETRY_WAIT;
}

/* Return -1 if there is no mode change, 0 for day mode, 1 for night mode. */
int8_t Bh1750_DayNightTask(void)
{
  uint32_t now = HAL_GetTick();
  uint16_t raw;
  uint16_t lux;

  switch (s_bh1750_state) {
    case BH1750_STATE_SEND_POWER:
      if (Bh1750_WriteCommand(BH1750_CMD_POWER_ON) != 0U) {
        s_bh1750_state = BH1750_STATE_SEND_MODE;
      } else {
        Bh1750_Fail(now);
      }
      break;

    case BH1750_STATE_SEND_MODE:
      if (Bh1750_WriteCommand(BH1750_CMD_CONT_H_RES) != 0U) {
        s_bh1750_fail_count = 0U;
        s_bh1750_last_tick = now;
        s_bh1750_state = BH1750_STATE_WAIT_CONVERSION;
      } else {
        Bh1750_Fail(now);
      }
      break;

    case BH1750_STATE_WAIT_CONVERSION:
      if ((now - s_bh1750_last_tick) >= BH1750_CONVERSION_MS) {
        s_bh1750_state = BH1750_STATE_IDLE;
      }
      break;

    case BH1750_STATE_IDLE:
      if ((now - s_bh1750_last_tick) >= BH1750_POLL_MS) {
        if (Bh1750_ReadRaw(&raw) == 0U) {
          Bh1750_Fail(now);
          break;
        }

        lux = (uint16_t)(((uint32_t)raw * 5U + 3U) / 6U);
        s_bh1750_last_lux = lux;
        s_bh1750_fail_count = 0U;
        s_bh1750_last_tick = now;

        if ((s_bh1750_night_mode == 0U) && (lux <= BH1750_NIGHT_ENTER_LUX)) {
          s_bh1750_night_mode = 1U;
          return 1;
        }

        if ((s_bh1750_night_mode != 0U) && (lux >= BH1750_DAY_ENTER_LUX)) {
          s_bh1750_night_mode = 0U;
          return 0;
        }
      }
      break;

    case BH1750_STATE_RETRY_WAIT:
      if ((now - s_bh1750_last_tick) >= BH1750_RETRY_MS) {
        s_bh1750_state = (s_bh1750_fail_count >= 3U) ? BH1750_STATE_SEND_POWER : BH1750_STATE_SEND_MODE;
        if (s_bh1750_fail_count >= 3U) {
          s_bh1750_fail_count = 0U;
        }
      }
      break;

    default:
      s_bh1750_state = BH1750_STATE_SEND_POWER;
      break;
  }

  return -1;
}

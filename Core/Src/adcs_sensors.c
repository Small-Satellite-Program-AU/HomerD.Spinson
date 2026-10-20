#include "adcs_sensors.h"
#include <string.h>

/* Register definitions: ST IIS2MDC datasheet; Bosch BMI330 DS000-03.
 * BMI330 has 16-bit little-endian registers and TWO dummy bytes on I2C reads.
 * This is intentionally not a BMI160/BMI270/BMI323 register driver. */
#define IO_TIMEOUT_MS 10U
#define MAG_ADDRESS 0x1EU
#define MAG_ID_REG 0x4FU
#define MAG_ID 0x40U
#define MAG_CFG_A 0x60U
#define MAG_CFG_B 0x61U
#define MAG_CFG_C 0x62U
#define MAG_STATUS 0x67U
#define MAG_DATA 0x68U
#define BMI_ID_REG 0x00U
#define BMI_ID 0x48U
#define BMI_ERR 0x01U
#define BMI_STATUS 0x02U
#define BMI_DATA 0x03U
#define BMI_SAT 0x0CU
#define BMI_ACC_CONF 0x20U
#define BMI_GYR_CONF 0x21U
/* Normal mode, no averaging, ODR/2 filter, 100 Hz; +/-4g and +/-500 deg/s. */
#define BMI_ACC_SETTING 0x4018U
#define BMI_GYR_SETTING 0x4028U

uint8_t adcs_sensor_status[ADCS_SENSOR_COUNT];
ADCS_Diagnostic adcs_diagnostics[ADCS_SENSOR_COUNT];
ADCS_Buffer adcs_buffers[ADCS_SENSOR_COUNT];
static I2C_HandleTypeDef *buses[3];
static uint32_t last_poll, last_check;

static const struct { GPIO_TypeDef *port; uint16_t pin; GPIO_PinState on; } sensor_leds[] = {
  {GPIOA, GPIO_PIN_3, GPIO_PIN_SET},
  {GPIOA, GPIO_PIN_6, GPIO_PIN_SET},
  {GPIOC, GPIO_PIN_5, GPIO_PIN_SET},
  {GPIOC, GPIO_PIN_4, GPIO_PIN_SET},
  {GPIOA, GPIO_PIN_4, GPIO_PIN_SET},
  {GPIOA, GPIO_PIN_7, GPIO_PIN_SET}
};
void ADCS_SetSensorLED(ADCS_SensorId id, uint8_t on)
{
  if ((unsigned)id >= ADCS_SENSOR_COUNT || !sensor_leds[id].port || !sensor_leds[id].pin) return;
  GPIO_PinState state = on ? sensor_leds[id].on :
    (sensor_leds[id].on == GPIO_PIN_SET ? GPIO_PIN_RESET : GPIO_PIN_SET);
  HAL_GPIO_WritePin(sensor_leds[id].port, sensor_leds[id].pin, state);
}
/* TEMPORARY DIAGNOSTIC: lights both I2C4 sensor LEDs without checking sensors.
 * Remove this function and its call in main.c after the LED hardware check. */
void ADCS_TestI2C4LEDs(void)
{
  for (unsigned blink = 0; blink < 10; ++blink) {
    ADCS_SetSensorLED(ADCS_I2C4_MAG, 1);
    ADCS_SetSensorLED(ADCS_I2C4_IMU, 1);
    HAL_Delay(250);
    ADCS_SetSensorLED(ADCS_I2C4_MAG, 0);
    ADCS_SetSensorLED(ADCS_I2C4_IMU, 0);
    HAL_Delay(250);
  }
}
static void status_set(unsigned id, uint8_t on)
{
  adcs_sensor_status[id] = on ? 1U : 0U;
  ADCS_SetSensorLED((ADCS_SensorId)id, on);
}
static HAL_StatusTypeDef read_bytes(unsigned id, uint8_t reg, uint8_t *data, uint16_t n)
{
  if (!buses[id / 2]) return HAL_ERROR;
  return HAL_I2C_Mem_Read(buses[id / 2], (uint16_t)(adcs_diagnostics[id].address_7bit << 1),
                         reg, I2C_MEMADD_SIZE_8BIT, data, n, IO_TIMEOUT_MS);
}
static HAL_StatusTypeDef write_bytes(unsigned id, uint8_t reg, uint8_t *data, uint16_t n)
{
  if (!buses[id / 2]) return HAL_ERROR;
  return HAL_I2C_Mem_Write(buses[id / 2], (uint16_t)(adcs_diagnostics[id].address_7bit << 1),
                          reg, I2C_MEMADD_SIZE_8BIT, data, n, IO_TIMEOUT_MS);
}
static uint16_t le16(const uint8_t *b) { return (uint16_t)b[0] | ((uint16_t)b[1] << 8); }
static int16_t signed16(const uint8_t *b)
{
  uint16_t u = le16(b);
  return (int16_t)(u < 32768U ? (int32_t)u : (int32_t)u - 65536);
}
static HAL_StatusTypeDef bmi_read(unsigned id, uint8_t reg, uint16_t *out)
{
  uint8_t b[4];
  HAL_StatusTypeDef result = read_bytes(id, reg, b, sizeof b);
  if (result == HAL_OK) *out = le16(b + 2);
  return result;
}
static HAL_StatusTypeDef bmi_write(unsigned id, uint8_t reg, uint16_t value)
{
  uint8_t b[2] = {(uint8_t)value, (uint8_t)(value >> 8)};
  HAL_StatusTypeDef result = write_bytes(id, reg, b, 2);
  HAL_Delay(1); /* Conservative inter-write spacing, including suspend mode. */
  return result;
}
static uint8_t identify(unsigned id)
{
  adcs_diagnostics[id].who_am_i = 0;
  if (!(id & 1U)) {
    uint8_t value = 0;
    adcs_diagnostics[id].address_7bit = MAG_ADDRESS;
    if (read_bytes(id, MAG_ID_REG, &value, 1) != HAL_OK) return 0;
    adcs_diagnostics[id].who_am_i = value;
    return value == MAG_ID;
  }
  for (uint8_t address = 0x68; address <= 0x69; ++address) {
    uint16_t value = 0;
    adcs_diagnostics[id].address_7bit = address;
    if (bmi_read(id, BMI_ID_REG, &value) == HAL_OK) {
      adcs_diagnostics[id].who_am_i = (uint8_t)value;
      if ((value & 0xFFU) == BMI_ID) return 1;
    }
  }
  return 0;
}
static uint8_t configure(unsigned id)
{
  if (!(id & 1U)) {
    /* Idle first; temperature compensation, 50 Hz, offset cancellation, BDU. */
    const uint8_t regs[] = {MAG_CFG_A, MAG_CFG_B, MAG_CFG_C, MAG_CFG_A};
    const uint8_t values[] = {0x83, 0x02, 0x10, 0x88};
    for (unsigned i = 0; i < sizeof regs; ++i) {
      uint8_t value = values[i], check = 0;
      if (write_bytes(id, regs[i], &value, 1) != HAL_OK ||
          read_bytes(id, regs[i], &check, 1) != HAL_OK || check != value) return 0;
    }
  } else {
    uint16_t err, check;
    /* Clear the startup POR flag before normal acquisition begins. */
    if (bmi_read(id, BMI_STATUS, &check) != HAL_OK) return 0;
    /* Suspend first so recovery also works after a partial configuration. */
    if (bmi_write(id, BMI_ACC_CONF, 0x0028) != HAL_OK ||
        bmi_write(id, BMI_GYR_CONF, 0x0008) != HAL_OK ||
        bmi_write(id, BMI_ACC_CONF, BMI_ACC_SETTING) != HAL_OK ||
        bmi_write(id, BMI_GYR_CONF, BMI_GYR_SETTING) != HAL_OK) return 0;
    HAL_Delay(50); /* Allow gyro startup before error/configuration verification. */
    if (bmi_read(id, BMI_ACC_CONF, &check) != HAL_OK || check != BMI_ACC_SETTING ||
        bmi_read(id, BMI_GYR_CONF, &check) != HAL_OK || check != BMI_GYR_SETTING ||
        bmi_read(id, BMI_ERR, &err) != HAL_OK || err != 0) return 0;
  }
  return 1;
}
void ADCS_CheckSensors(void)
{
  for (unsigned id = 0; id < ADCS_SENSOR_COUNT; ++id) {
    if (!identify(id)) {
      adcs_diagnostics[id].configured = 0;
      status_set(id, 0);
    } else {
      if (!adcs_diagnostics[id].configured) {
        adcs_diagnostics[id].configured = configure(id);
        if (!adcs_diagnostics[id].configured) ++adcs_diagnostics[id].io_errors;
      }
      /* Conservative ON: identity verified AND acquisition configured. */
      status_set(id, adcs_diagnostics[id].configured);
    }
  }
}
void ADCS_Init(I2C_HandleTypeDef *bus1, I2C_HandleTypeDef *bus2, I2C_HandleTypeDef *bus4)
{
  buses[0] = bus1; buses[1] = bus2; buses[2] = bus4;
  memset(adcs_sensor_status, 0, sizeof adcs_sensor_status);
  memset(adcs_diagnostics, 0, sizeof adcs_diagnostics);
  memset(adcs_buffers, 0, sizeof adcs_buffers);
  ADCS_CheckSensors();
  last_poll = last_check = HAL_GetTick();
}
static void push(unsigned id, const ADCS_Sample *sample)
{
  ADCS_Buffer *b = &adcs_buffers[id];
  b->samples[b->head] = *sample;
  b->head = (uint16_t)((b->head + 1U) % ADCS_BUFFER_CAPACITY);
  if (b->count < ADCS_BUFFER_CAPACITY) ++b->count; else ++b->overwritten;
  adcs_diagnostics[id].last_sample_ms = sample->timestamp_ms;
}
uint8_t ADCS_PopSample(ADCS_SensorId id, ADCS_Sample *out)
{
  if ((unsigned)id >= ADCS_SENSOR_COUNT || !out) return 0;
  ADCS_Buffer *b = &adcs_buffers[id];
  if (!b->count) return 0;
  unsigned tail = (b->head + ADCS_BUFFER_CAPACITY - b->count) % ADCS_BUFFER_CAPACITY;
  *out = b->samples[tail];
  --b->count;
  return 1;
}
/* Return -1 on transport failure, 0 when no fresh data, 1 for a new sample. */
static int read_sample(unsigned id, ADCS_Sample *s)
{
  if (!(id & 1U)) {
    uint8_t status, data[6];
    if (read_bytes(id, MAG_STATUS, &status, 1) != HAL_OK) return -1;
    if (!(status & 0x08U)) return 0;
    if (read_bytes(id, MAG_DATA, data, sizeof data) != HAL_OK) return -1;
    for (unsigned i = 0; i < 3; ++i) s->mag_raw[i] = signed16(data + 2*i);
    s->valid = ADCS_VALID_MAG;
    s->overrun = (status & 0x80U) != 0;
  } else {
    uint16_t status;
    uint8_t data[14];
    if (bmi_read(id, BMI_STATUS, &status) != HAL_OK) return -1;
    if (status & 1U) { /* Sensor reset since configuration: reinitialize on next check. */
      adcs_diagnostics[id].configured = 0;
      status_set(id, 0);
      return 0;
    }
    if (!(status & 0xC0U)) return 0;
    if (read_bytes(id, BMI_DATA, data, sizeof data) != HAL_OK ||
        bmi_read(id, BMI_SAT, &s->saturation) != HAL_OK) return -1;
    /* Independent ready bits prevent one channel's stale values being reused. */
    if (status & 0x80U) {
      s->valid |= ADCS_VALID_ACCEL;
      for (unsigned i = 0; i < 3; ++i) s->accel_raw[i] = signed16(data + 2 + 2*i);
    }
    if (status & 0x40U) {
      s->valid |= ADCS_VALID_GYRO;
      for (unsigned i = 0; i < 3; ++i) s->gyro_raw[i] = signed16(data + 8 + 2*i);
    }
  }
  s->timestamp_ms = HAL_GetTick();
  return 1;
}
void ADCS_Poll(void)
{
  uint32_t now = HAL_GetTick();
  if ((uint32_t)(now - last_check) >= ADCS_RECHECK_MS) {
    ADCS_CheckSensors();
    last_check = HAL_GetTick();
  }
  now = HAL_GetTick();
  if ((uint32_t)(now - last_poll) < ADCS_POLL_MS) return;
  last_poll = now;
  for (unsigned id = 0; id < ADCS_SENSOR_COUNT; ++id) {
    if (!adcs_sensor_status[id] || !adcs_diagnostics[id].configured) continue;
    ADCS_Sample sample = {0};
    int result = read_sample(id, &sample);
    if (result > 0) push(id, &sample);
    else if (result < 0) {
      ++adcs_diagnostics[id].io_errors;
      adcs_diagnostics[id].configured = 0;
      status_set(id, 0);
    }
  }
}
float ADCS_MagMicrotesla(int16_t raw) { return (float)raw * 0.15f; }
float ADCS_AccelMps2(int16_t raw) { return (float)raw * (4.0f * 9.80665f / 32768.0f); }
float ADCS_GyroRadps(int16_t raw) { return (float)raw * (500.0f * 0.017453292519943295f / 32768.0f); }

#ifndef ADCS_SENSORS_H
#define ADCS_SENSORS_H
#include "stm32l4xx_hal.h"
#include <stdint.h>

#ifndef ADCS_BUFFER_CAPACITY
#define ADCS_BUFFER_CAPACITY 128U
#endif
#if ADCS_BUFFER_CAPACITY < 1 || ADCS_BUFFER_CAPACITY > 65535
#error "ADCS_BUFFER_CAPACITY must be between 1 and 65535"
#endif
#define ADCS_SENSOR_COUNT 6U
#define ADCS_POLL_MS 5U
#define ADCS_RECHECK_MS 1000U
#define ADCS_VALID_MAG 1U
#define ADCS_VALID_ACCEL 2U
#define ADCS_VALID_GYRO 4U

typedef enum {
  ADCS_I2C1_MAG, ADCS_I2C1_IMU,
  ADCS_I2C2_MAG, ADCS_I2C2_IMU,
  ADCS_I2C4_MAG, ADCS_I2C4_IMU
} ADCS_SensorId;

typedef struct {
  uint32_t timestamp_ms; /* Host acquisition time, not synchronized sensor time. */
  uint8_t valid; /* ADCS_VALID_* bits; ignore axes whose bit is clear. */
  uint8_t overrun; /* Magnetometer overwrite reported by hardware. */
  uint16_t saturation; /* BMI330 SAT_FLAGS; zero for magnetometer. */
  int16_t mag_raw[3], accel_raw[3], gyro_raw[3];
} ADCS_Sample;

typedef struct {
  ADCS_Sample samples[ADCS_BUFFER_CAPACITY];
  uint16_t head, count;
  uint32_t overwritten; /* Oldest unread samples discarded when full. */
} ADCS_Buffer;

typedef struct {
  uint8_t address_7bit, who_am_i, configured;
  uint32_t io_errors, last_sample_ms; /* io_errors includes configuration failures. */
} ADCS_Diagnostic;

extern uint8_t adcs_sensor_status[ADCS_SENSOR_COUNT]; /* 1 = verified/configured; 0 = absent/error. */
extern ADCS_Diagnostic adcs_diagnostics[ADCS_SENSOR_COUNT];
extern ADCS_Buffer adcs_buffers[ADCS_SENSOR_COUNT];

/* Call after GPIO/I2C initialization and one startup blink cycle. */
void ADCS_Init(I2C_HandleTypeDef *bus1, I2C_HandleTypeDef *bus2, I2C_HandleTypeDef *bus4);
void ADCS_CheckSensors(void);
void ADCS_Poll(void);
/* All APIs run in the main loop; not ISR/thread safe. Pop oldest unread sample. */
uint8_t ADCS_PopSample(ADCS_SensorId id, ADCS_Sample *out);
void ADCS_SetSensorLED(ADCS_SensorId id, uint8_t on);
/* TEMPORARY LED-only diagnostic. Remove after I2C4 LED check. */
void ADCS_TestI2C4LEDs(void);
/* Fixed scales corresponding to the configuration in adcs_sensors.c. */
float ADCS_MagMicrotesla(int16_t raw);
float ADCS_AccelMps2(int16_t raw);
float ADCS_GyroRadps(int16_t raw);
#endif

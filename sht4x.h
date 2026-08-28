/*
    Sensirion SHT4x temperature and humidity sensor (I2C, address 0x44)
 */
#ifndef _H_sht4x
#define _H_sht4x

#include <stdint.h>

#define SHT4X_I2C_ADDR 0x44

/* Read the serial number, 0 on success. Used to detect the sensor. */
int16_t sht4x_read_serial(uint32_t *serial);

/* Soft reset, 0 on success. */
int16_t sht4x_soft_reset(void);

/*
 * Single shot measurement with high repeatability (~8 ms).
 * temperature in degrees Celsius, humidity in %RH (clamped to 0..100).
 * Returns 0 on success.
 */
int16_t sht4x_measure_high_precision(float *temperature, float *humidity);

#endif /* _H_sht4x */

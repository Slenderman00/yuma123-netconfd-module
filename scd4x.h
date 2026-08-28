/*
    Sensirion SCD4x CO2 sensor (I2C, address 0x62)
 */
#ifndef _H_scd4x
#define _H_scd4x

#include <stdint.h>

#define SCD4X_I2C_ADDR 0x62

/* Read the serial number (only valid in idle mode), 0 on success. */
int16_t scd4x_get_serial_number(uint64_t *serial);

/* Stop periodic measurement (sensor needs 500 ms afterwards, included). */
int16_t scd4x_stop_periodic_measurement(void);

/* Start periodic measurement, a new sample every 5 s. */
int16_t scd4x_start_periodic_measurement(void);

/* Sets *ready to 1 if a new measurement can be read. 0 on success. */
int16_t scd4x_get_data_ready_status(int *ready);

/*
 * Read the last measurement. co2 in ppm, temperature in degrees Celsius,
 * humidity in %RH. Returns 0 on success.
 */
int16_t scd4x_read_measurement(uint16_t *co2, float *temperature,
                               float *humidity);

#endif /* _H_scd4x */

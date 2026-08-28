/*
    Sensirion SCD4x CO2 sensor (I2C, address 0x62)

    Datasheet: SCD4x, section 3 "I2C interface description"
 */
#include "sensirion_common.h"
#include "sensirion_i2c.h"
#include "sensirion_i2c_hal.h"
#include "scd4x.h"

#define SCD4X_CMD_START_PERIODIC_MEASUREMENT 0x21B1
#define SCD4X_CMD_READ_MEASUREMENT           0xEC05
#define SCD4X_CMD_STOP_PERIODIC_MEASUREMENT  0x3F86
#define SCD4X_CMD_GET_DATA_READY_STATUS      0xE4B8
#define SCD4X_CMD_GET_SERIAL_NUMBER          0x3682

int16_t scd4x_get_serial_number(uint64_t *serial)
{
    uint16_t words[3];
    int16_t error;

    error = sensirion_i2c_delayed_read_cmd(SCD4X_I2C_ADDR,
                                           SCD4X_CMD_GET_SERIAL_NUMBER,
                                           1000, words, 3);
    if (error) {
        return error;
    }
    *serial = ((uint64_t)words[0] << 32) | ((uint64_t)words[1] << 16) |
              words[2];
    return NO_ERROR;
}

int16_t scd4x_stop_periodic_measurement(void)
{
    int16_t error;

    error = sensirion_i2c_write_cmd(SCD4X_I2C_ADDR,
                                    SCD4X_CMD_STOP_PERIODIC_MEASUREMENT);
    if (error) {
        return error;
    }
    /* the sensor needs 500 ms before it accepts the next command */
    sensirion_i2c_hal_sleep_usec(500000);
    return NO_ERROR;
}

int16_t scd4x_start_periodic_measurement(void)
{
    return sensirion_i2c_write_cmd(SCD4X_I2C_ADDR,
                                   SCD4X_CMD_START_PERIODIC_MEASUREMENT);
}

int16_t scd4x_get_data_ready_status(int *ready)
{
    uint16_t word;
    int16_t error;

    error = sensirion_i2c_delayed_read_cmd(SCD4X_I2C_ADDR,
                                           SCD4X_CMD_GET_DATA_READY_STATUS,
                                           1000, &word, 1);
    if (error) {
        return error;
    }
    /* the 11 least significant bits are 0 while no data is ready */
    *ready = (word & 0x07FF) != 0;
    return NO_ERROR;
}

int16_t scd4x_read_measurement(uint16_t *co2, float *temperature,
                               float *humidity)
{
    uint16_t words[3];
    int16_t error;

    error = sensirion_i2c_delayed_read_cmd(SCD4X_I2C_ADDR,
                                           SCD4X_CMD_READ_MEASUREMENT,
                                           1000, words, 3);
    if (error) {
        return error;
    }
    *co2 = words[0];
    *temperature = -45.0f + 175.0f * (float)words[1] / 65535.0f;
    *humidity = 100.0f * (float)words[2] / 65535.0f;
    return NO_ERROR;
}

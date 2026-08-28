/*
    Sensirion SHT4x temperature and humidity sensor (I2C, address 0x44)

    Datasheet: SHT4x, command set section 4.5
 */
#include "sensirion_common.h"
#include "sensirion_i2c.h"
#include "sensirion_i2c_hal.h"
#include "sht4x.h"

#define SHT4X_CMD_MEASURE_HIGH_PRECISION 0xFD
#define SHT4X_CMD_READ_SERIAL            0x89
#define SHT4X_CMD_SOFT_RESET             0x94

static int16_t sht4x_write_cmd(uint8_t cmd)
{
    return sensirion_i2c_hal_write(SHT4X_I2C_ADDR, &cmd, 1);
}

int16_t sht4x_read_serial(uint32_t *serial)
{
    uint16_t words[2];
    int16_t error;

    error = sht4x_write_cmd(SHT4X_CMD_READ_SERIAL);
    if (error) {
        return error;
    }
    sensirion_i2c_hal_sleep_usec(1000);
    error = sensirion_i2c_read_words(SHT4X_I2C_ADDR, words, 2);
    if (error) {
        return error;
    }
    *serial = ((uint32_t)words[0] << 16) | words[1];
    return NO_ERROR;
}

int16_t sht4x_soft_reset(void)
{
    int16_t error = sht4x_write_cmd(SHT4X_CMD_SOFT_RESET);
    sensirion_i2c_hal_sleep_usec(1000);
    return error;
}

int16_t sht4x_measure_high_precision(float *temperature, float *humidity)
{
    uint16_t words[2];
    int16_t error;
    float rh;

    error = sht4x_write_cmd(SHT4X_CMD_MEASURE_HIGH_PRECISION);
    if (error) {
        return error;
    }
    sensirion_i2c_hal_sleep_usec(10000);
    error = sensirion_i2c_read_words(SHT4X_I2C_ADDR, words, 2);
    if (error) {
        return error;
    }

    *temperature = -45.0f + 175.0f * (float)words[0] / 65535.0f;
    rh = -6.0f + 125.0f * (float)words[1] / 65535.0f;
    if (rh < 0.0f) {
        rh = 0.0f;
    } else if (rh > 100.0f) {
        rh = 100.0f;
    }
    *humidity = rh;
    return NO_ERROR;
}

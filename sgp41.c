/*
    Sensirion SGP41 VOC and NOx sensor (I2C, address 0x59)

    Datasheet: SGP41, section 3 "I2C command set"
 */
#include "sensirion_common.h"
#include "sensirion_i2c.h"
#include "sensirion_i2c_hal.h"
#include "sgp41.h"

#define SGP41_CMD_EXECUTE_CONDITIONING 0x2612
#define SGP41_CMD_MEASURE_RAW_SIGNALS  0x2619
#define SGP41_CMD_TURN_HEATER_OFF      0x3615
#define SGP41_CMD_GET_SERIAL_NUMBER    0x3682

uint16_t sgp41_rh_to_ticks(float humidity)
{
    if (humidity < 0.0f) {
        humidity = 0.0f;
    } else if (humidity > 100.0f) {
        humidity = 100.0f;
    }
    return (uint16_t)(humidity * 65535.0f / 100.0f + 0.5f);
}

uint16_t sgp41_t_to_ticks(float temperature)
{
    if (temperature < -45.0f) {
        temperature = -45.0f;
    } else if (temperature > 130.0f) {
        temperature = 130.0f;
    }
    return (uint16_t)((temperature + 45.0f) * 65535.0f / 175.0f + 0.5f);
}

int16_t sgp41_get_serial_number(uint64_t *serial)
{
    uint16_t words[3];
    int16_t error;

    error = sensirion_i2c_delayed_read_cmd(SGP41_I2C_ADDR,
                                           SGP41_CMD_GET_SERIAL_NUMBER,
                                           1000, words, 3);
    if (error) {
        return error;
    }
    *serial = ((uint64_t)words[0] << 32) | ((uint64_t)words[1] << 16) |
              words[2];
    return NO_ERROR;
}

/* command + two compensation words, each followed by its CRC */
static int16_t sgp41_write_cmd_with_compensation(uint16_t cmd,
                                                 uint16_t rh_ticks,
                                                 uint16_t t_ticks)
{
    uint8_t buffer[8];
    uint16_t offset = 0;

    offset = sensirion_i2c_add_command_to_buffer(buffer, offset, cmd);
    offset = sensirion_i2c_add_uint16_t_to_buffer(buffer, offset, rh_ticks);
    offset = sensirion_i2c_add_uint16_t_to_buffer(buffer, offset, t_ticks);
    return sensirion_i2c_write_data(SGP41_I2C_ADDR, buffer, offset);
}

int16_t sgp41_execute_conditioning(uint16_t rh_ticks, uint16_t t_ticks,
                                   uint16_t *sraw_voc)
{
    int16_t error;

    error = sgp41_write_cmd_with_compensation(SGP41_CMD_EXECUTE_CONDITIONING,
                                              rh_ticks, t_ticks);
    if (error) {
        return error;
    }
    sensirion_i2c_hal_sleep_usec(50000);
    return sensirion_i2c_read_words(SGP41_I2C_ADDR, sraw_voc, 1);
}

int16_t sgp41_measure_raw_signals(uint16_t rh_ticks, uint16_t t_ticks,
                                  uint16_t *sraw_voc, uint16_t *sraw_nox)
{
    uint16_t words[2];
    int16_t error;

    error = sgp41_write_cmd_with_compensation(SGP41_CMD_MEASURE_RAW_SIGNALS,
                                              rh_ticks, t_ticks);
    if (error) {
        return error;
    }
    sensirion_i2c_hal_sleep_usec(50000);
    error = sensirion_i2c_read_words(SGP41_I2C_ADDR, words, 2);
    if (error) {
        return error;
    }
    *sraw_voc = words[0];
    *sraw_nox = words[1];
    return NO_ERROR;
}

int16_t sgp41_turn_heater_off(void)
{
    int16_t error;

    error = sensirion_i2c_write_cmd(SGP41_I2C_ADDR, SGP41_CMD_TURN_HEATER_OFF);
    sensirion_i2c_hal_sleep_usec(1000);
    return error;
}

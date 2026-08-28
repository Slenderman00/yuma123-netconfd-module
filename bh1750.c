/*
    ROHM BH1750 ambient light sensor (I2C, address 0x23 or 0x5c)

    Datasheet: BH1750FVI, "Instruction set architecture"
 */
#include "sensirion_common.h"
#include "sensirion_i2c_hal.h"
#include "bh1750.h"

#define BH1750_CMD_POWER_ON          0x01
#define BH1750_CMD_RESET             0x07
#define BH1750_CMD_CONTINUOUS_H_RES  0x10

static int16_t bh1750_write_cmd(uint8_t address, uint8_t cmd)
{
    return sensirion_i2c_hal_write(address, &cmd, 1);
}

uint8_t bh1750_init(uint8_t skip_address)
{
    const uint8_t addresses[] = { BH1750_I2C_ADDR_LOW, BH1750_I2C_ADDR_HIGH };
    unsigned i;

    for (i = 0; i < sizeof(addresses); i++) {
        uint8_t address = addresses[i];
        if (address == skip_address) {
            continue;
        }
        if (bh1750_write_cmd(address, BH1750_CMD_POWER_ON)) {
            continue;
        }
        if (bh1750_write_cmd(address, BH1750_CMD_RESET)) {
            continue;
        }
        if (bh1750_write_cmd(address, BH1750_CMD_CONTINUOUS_H_RES)) {
            continue;
        }
        return address;
    }
    return 0;
}

int16_t bh1750_read(uint8_t address, float *illuminance)
{
    uint8_t raw[2];
    int16_t error;

    error = sensirion_i2c_hal_read(address, raw, 2);
    if (error) {
        return error;
    }
    /* 1.2 counts per lux in H-resolution mode */
    *illuminance = (float)(((uint16_t)raw[0] << 8) | raw[1]) / 1.2f;
    return NO_ERROR;
}

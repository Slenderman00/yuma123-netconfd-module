/*
    ST LPS22HB barometric pressure sensor (I2C, address 0x5c or 0x5d)

    Datasheet: LPS22HB, section 9 "Register description"
 */
#include "sensirion_common.h"
#include "sensirion_i2c_hal.h"
#include "lps22hb.h"

#define LPS22HB_REG_WHO_AM_I     0x0F
#define LPS22HB_REG_CTRL_REG1    0x10
#define LPS22HB_REG_CTRL_REG2    0x11
#define LPS22HB_REG_STATUS       0x27
#define LPS22HB_REG_PRESS_OUT_XL 0x28

#define LPS22HB_WHO_AM_I_VALUE   0xB1

/* CTRL_REG1: ODR = 1 Hz (001), BDU = 1 */
#define LPS22HB_CTRL_REG1_ODR_1HZ_BDU 0x12
/* CTRL_REG2: IF_ADD_INC = 1 (auto increment on multi byte access) */
#define LPS22HB_CTRL_REG2_IF_ADD_INC  0x10

#define LPS22HB_STATUS_P_DA 0x01
#define LPS22HB_STATUS_T_DA 0x02

static int16_t reg_read(uint8_t address, uint8_t reg, uint8_t *data,
                        uint8_t count)
{
    int16_t error = sensirion_i2c_hal_write(address, &reg, 1);
    if (error) {
        return error;
    }
    return sensirion_i2c_hal_read(address, data, count);
}

static int16_t reg_write(uint8_t address, uint8_t reg, uint8_t value)
{
    uint8_t buffer[2] = { reg, value };
    return sensirion_i2c_hal_write(address, buffer, 2);
}

static int lps22hb_probe(uint8_t address)
{
    uint8_t who_am_i = 0;

    if (reg_read(address, LPS22HB_REG_WHO_AM_I, &who_am_i, 1)) {
        return 0;
    }
    return who_am_i == LPS22HB_WHO_AM_I_VALUE;
}

uint8_t lps22hb_init(void)
{
    const uint8_t addresses[] = { LPS22HB_I2C_ADDR_SA0_LOW,
                                  LPS22HB_I2C_ADDR_SA0_HIGH };
    unsigned i;

    for (i = 0; i < sizeof(addresses); i++) {
        uint8_t address = addresses[i];
        if (!lps22hb_probe(address)) {
            continue;
        }
        if (reg_write(address, LPS22HB_REG_CTRL_REG2,
                      LPS22HB_CTRL_REG2_IF_ADD_INC)) {
            continue;
        }
        if (reg_write(address, LPS22HB_REG_CTRL_REG1,
                      LPS22HB_CTRL_REG1_ODR_1HZ_BDU)) {
            continue;
        }
        return address;
    }
    return 0;
}

int16_t lps22hb_read(uint8_t address, float *pressure, float *temperature)
{
    uint8_t status;
    uint8_t raw[5];
    int32_t p;
    int16_t t;

    if (reg_read(address, LPS22HB_REG_STATUS, &status, 1)) {
        return -1;
    }
    if ((status & (LPS22HB_STATUS_P_DA | LPS22HB_STATUS_T_DA)) !=
        (LPS22HB_STATUS_P_DA | LPS22HB_STATUS_T_DA)) {
        return 1;
    }
    /* PRESS_OUT_XL, PRESS_OUT_L, PRESS_OUT_H, TEMP_OUT_L, TEMP_OUT_H */
    if (reg_read(address, LPS22HB_REG_PRESS_OUT_XL, raw, 5)) {
        return -1;
    }

    /* 24 bit two's complement, 4096 LSB/hPa */
    p = ((int32_t)raw[2] << 16) | ((int32_t)raw[1] << 8) | raw[0];
    if (p & 0x800000) {
        p -= 0x1000000;
    }
    *pressure = (float)p / 4096.0f;

    /* 16 bit two's complement, 100 LSB/degree */
    t = (int16_t)(((uint16_t)raw[4] << 8) | raw[3]);
    *temperature = (float)t / 100.0f;
    return 0;
}

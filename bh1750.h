/*
    ROHM BH1750 ambient light sensor (I2C, address 0x23 or 0x5c)
 */
#ifndef _H_bh1750
#define _H_bh1750

#include <stdint.h>

#define BH1750_I2C_ADDR_LOW  0x23
#define BH1750_I2C_ADDR_HIGH 0x5C

/*
 * Power the sensor on and start continuous high resolution mode (1 lx,
 * 120 ms typ. per measurement). The BH1750 has no ID register so the
 * sensor is assumed to be present on the first address that acknowledges.
 * skip_address is not tried (used when another device sits there).
 * Returns the address found, or 0 if no sensor answered.
 */
uint8_t bh1750_init(uint8_t skip_address);

/* Read the last measurement in lux. Returns 0 on success. */
int16_t bh1750_read(uint8_t address, float *illuminance);

#endif /* _H_bh1750 */

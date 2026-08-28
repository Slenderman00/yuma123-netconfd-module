/*
    ST LPS22HB barometric pressure sensor (I2C, address 0x5c or 0x5d)
 */
#ifndef _H_lps22hb
#define _H_lps22hb

#include <stdint.h>

#define LPS22HB_I2C_ADDR_SA0_LOW  0x5C
#define LPS22HB_I2C_ADDR_SA0_HIGH 0x5D

/*
 * Look for the sensor on both addresses (WHO_AM_I) and configure it for
 * continuous 1 Hz output with block data update.
 * Returns the address found, or 0 if no sensor answered.
 */
uint8_t lps22hb_init(void);

/*
 * Read the last conversion. pressure in hPa, temperature in degrees
 * Celsius. Returns 0 on success, -1 on bus error, 1 if no new data.
 */
int16_t lps22hb_read(uint8_t address, float *pressure, float *temperature);

#endif /* _H_lps22hb */

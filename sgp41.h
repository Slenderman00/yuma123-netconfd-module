/*
    Sensirion SGP41 VOC and NOx sensor (I2C, address 0x59)
 */
#ifndef _H_sgp41
#define _H_sgp41

#include <stdint.h>

#define SGP41_I2C_ADDR 0x59

/* Compensation defaults: 50 %RH and 25 degrees Celsius */
#define SGP41_DEFAULT_RH_TICKS 0x8000
#define SGP41_DEFAULT_T_TICKS  0x6666

/* Convert %RH / degrees Celsius to the tick format of the sensor */
uint16_t sgp41_rh_to_ticks(float humidity);
uint16_t sgp41_t_to_ticks(float temperature);

/* Read the 48 bit serial number, 0 on success. Used to detect the sensor. */
int16_t sgp41_get_serial_number(uint64_t *serial);

/*
 * Conditioning: heats the NOx hotplate and returns the VOC raw signal.
 * Call once per second for at most 10 s after power up, before the first
 * measure_raw_signals. Takes 50 ms. Returns 0 on success.
 */
int16_t sgp41_execute_conditioning(uint16_t rh_ticks, uint16_t t_ticks,
                                   uint16_t *sraw_voc);

/*
 * Measure the raw VOC and NOx signals, compensated with the given
 * humidity and temperature. Takes 50 ms. Returns 0 on success.
 */
int16_t sgp41_measure_raw_signals(uint16_t rh_ticks, uint16_t t_ticks,
                                  uint16_t *sraw_voc, uint16_t *sraw_nox);

/* Turn the hotplate off (idle mode), 0 on success. */
int16_t sgp41_turn_heater_off(void);

#endif /* _H_sgp41 */

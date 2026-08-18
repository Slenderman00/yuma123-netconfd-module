/*
    Plantower PMS7003 particulate matter sensor - UART reader
 */
#ifndef _H_pms7003
#define _H_pms7003

#include <stdint.h>

#define PMS7003_DEFAULT_DEVICE "/dev/serial0"

typedef struct pms7003_data_t_ {
    /* standard particles, CF=1 (ug/m3) */
    uint16_t pm1_0_cf1;
    uint16_t pm2_5_cf1;
    uint16_t pm10_cf1;
    /* atmospheric environment (ug/m3) */
    uint16_t pm1_0_atm;
    uint16_t pm2_5_atm;
    uint16_t pm10_atm;
    /* number of particles with diameter beyond X um in 0.1 L of air */
    uint16_t particles_0_3um;
    uint16_t particles_0_5um;
    uint16_t particles_1_0um;
    uint16_t particles_2_5um;
    uint16_t particles_5_0um;
    uint16_t particles_10um;
    uint8_t  version;
    uint8_t  error_code;
} pms7003_data_t;

/*
 * Read one fresh, checksum verified data frame from the sensor.
 * device     - serial device path, NULL for PMS7003_DEFAULT_DEVICE
 * timeout_ms - how long to wait for a valid frame
 * Returns 0 on success, -1 on error/timeout (errno set when applicable).
 */
int pms7003_read(const char *device, pms7003_data_t *data, int timeout_ms);

#endif /* _H_pms7003 */

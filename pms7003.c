/*
    Plantower PMS7003 particulate matter sensor - UART reader

    Frame format (active mode, 9600 8N1):
      0x42 0x4d | len(2)=28 | 13 x uint16 big endian | checksum(2)
    checksum = sum of all preceding bytes (including 0x42 0x4d)
 */
#define _DEFAULT_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <string.h>
#include <termios.h>
#include <time.h>
#include <unistd.h>

#include "pms7003.h"

#define PMS7003_FRAME_LEN 32
#define PMS7003_START1 0x42
#define PMS7003_START2 0x4d

static long elapsed_ms(const struct timespec *start)
{
    struct timespec now;
    clock_gettime(CLOCK_MONOTONIC, &now);
    return (now.tv_sec - start->tv_sec) * 1000L +
           (now.tv_nsec - start->tv_nsec) / 1000000L;
}

static uint16_t be16(const uint8_t *p)
{
    return (uint16_t)((p[0] << 8) | p[1]);
}

static int parse_frame(const uint8_t *f, pms7003_data_t *data)
{
    unsigned i;
    uint16_t sum = 0;

    if (f[0] != PMS7003_START1 || f[1] != PMS7003_START2) {
        return -1;
    }
    if (be16(&f[2]) != PMS7003_FRAME_LEN - 4) {
        return -1;
    }
    for (i = 0; i < PMS7003_FRAME_LEN - 2; i++) {
        sum += f[i];
    }
    if (sum != be16(&f[PMS7003_FRAME_LEN - 2])) {
        return -1;
    }

    data->pm1_0_cf1       = be16(&f[4]);
    data->pm2_5_cf1       = be16(&f[6]);
    data->pm10_cf1        = be16(&f[8]);
    data->pm1_0_atm       = be16(&f[10]);
    data->pm2_5_atm       = be16(&f[12]);
    data->pm10_atm        = be16(&f[14]);
    data->particles_0_3um = be16(&f[16]);
    data->particles_0_5um = be16(&f[18]);
    data->particles_1_0um = be16(&f[20]);
    data->particles_2_5um = be16(&f[22]);
    data->particles_5_0um = be16(&f[24]);
    data->particles_10um  = be16(&f[26]);
    data->version         = f[28];
    data->error_code      = f[29];
    return 0;
}

static int open_port(const char *device)
{
    struct termios tio;
    int fd;

    fd = open(device, O_RDONLY | O_NOCTTY);
    if (fd < 0) {
        return -1;
    }

    memset(&tio, 0, sizeof(tio));
    cfmakeraw(&tio);
    tio.c_cflag |= CS8 | CREAD | CLOCAL;
    tio.c_cflag &= ~(PARENB | CSTOPB | CRTSCTS);
    tio.c_cc[VMIN] = 0;
    tio.c_cc[VTIME] = 1; /* 100 ms read granularity */
    cfsetispeed(&tio, B9600);
    cfsetospeed(&tio, B9600);
    if (tcsetattr(fd, TCSANOW, &tio) < 0) {
        int saved = errno;
        close(fd);
        errno = saved;
        return -1;
    }
    /* drop anything buffered so we return a fresh sample */
    tcflush(fd, TCIFLUSH);
    return fd;
}

int pms7003_read(const char *device, pms7003_data_t *data, int timeout_ms)
{
    uint8_t frame[PMS7003_FRAME_LEN];
    unsigned pos = 0;
    struct timespec start;
    int fd;
    int result = -1;

    if (device == NULL) {
        device = PMS7003_DEFAULT_DEVICE;
    }

    fd = open_port(device);
    if (fd < 0) {
        return -1;
    }

    clock_gettime(CLOCK_MONOTONIC, &start);

    while (elapsed_ms(&start) < timeout_ms) {
        uint8_t byte;
        ssize_t n = read(fd, &byte, 1);
        if (n < 0) {
            if (errno == EINTR) {
                continue;
            }
            break;
        }
        if (n == 0) {
            continue; /* VTIME expired, check overall timeout */
        }

        /* synchronise on the start bytes */
        if (pos == 0 && byte != PMS7003_START1) {
            continue;
        }
        if (pos == 1 && byte != PMS7003_START2) {
            pos = (byte == PMS7003_START1) ? 1 : 0;
            continue;
        }

        frame[pos++] = byte;
        if (pos == PMS7003_FRAME_LEN) {
            if (parse_frame(frame, data) == 0) {
                result = 0;
                break;
            }
            pos = 0; /* bad frame, resynchronise */
        }
    }

    if (result != 0 && elapsed_ms(&start) >= timeout_ms) {
        errno = ETIMEDOUT;
    }
    close(fd);
    return result;
}

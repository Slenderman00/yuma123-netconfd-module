/*
    module hs-sensor-shield (SIL for the hs-sensor-shield YANG module)

    Raspberry Pi implementation for the Heimonen Solutions sensor shield:
      - PM1.0/PM2.5/PM10        Plantower PMS7003     UART (/dev/serial0)
      - CO2                     Sensirion SCD41       I2C 0x62
      - VOC / NOx index         Sensirion SGP41       I2C 0x59
      - temperature / humidity  Sensirion SHT41       I2C 0x44
      - pressure                ST LPS22HB            I2C 0x5c/0x5d
      - illuminance             ROHM BH1750           I2C 0x23/0x5c

    The sensors are sampled continuously by two background threads
    (the SGP41 gas index algorithm has to run at 1 Hz and the PMS7003
    streams frames on its own). A <get> of /sensor-shield returns a
    snapshot of the latest readings; a sensor whose last good reading
    is too old is reported with <oper-status>unavailable</oper-status>.
 */

#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <stdarg.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <pthread.h>
#include <stdint.h>

#include <libxml/xmlstring.h>
#include "procdefs.h"
#include "agt.h"
#include "agt_cb.h"
#include "agt_timer.h"
#include "agt_util.h"
#include "agt_not.h"
#include "agt_rpc.h"
#include "dlq.h"
#include "ncx.h"
#include "ncxmod.h"
#include "ncxtypes.h"
#include "status.h"
#include "rpc.h"
#include "val.h"
#include "val123.h"
#include "val_set_cplxval_obj.h"

/* Sensirion I2C layer and gas index algorithm */
#include "sensirion_config.h"
#include "sensirion_common.h"
#include "sensirion_i2c.h"
#include "sensirion_i2c_hal.h"
#include "sensirion_gas_index_algorithm.h"

/* Sensor drivers */
#include "sht4x.h"
#include "scd4x.h"
#include "sgp41.h"
#include "lps22hb.h"
#include "bh1750.h"
#include "pms7003.h"

#define HS_SENSOR_SHIELD_MOD "hs-sensor-shield"
#define HS_SENSOR_SHIELD_NS  "urn:heimonen-solutions:yang:hs-sensor-shield"

/* I2C sensors are sampled once per second (required by the gas index
   algorithm, GasIndexAlgorithm_DEFAULT_SAMPLING_INTERVAL) */
#define SAMPLE_INTERVAL_S 1
/* the SGP41 NOx hotplate is conditioned for 10 s after power up */
#define SGP41_CONDITIONING_S 10
/* how long to wait for a PMS7003 frame (sent every ~1-2.3 s) */
#define PMS7003_TIMEOUT_MS 3000
/* sensors that were not found are probed again this often */
#define PROBE_RETRY_S 10
/* consecutive SCD41 bus errors before it is re-initialised */
#define SCD41_MAX_ERRORS 3

/* a reading older than this is reported as unavailable */
#define MAX_AGE_I2C_S     5
#define MAX_AGE_SCD41_S   15 /* new sample every 5 s */
#define MAX_AGE_PMS7003_S 10

#define BUFSIZE 8*1024

/* module static variables */
static ncx_module_t *hs_sensor_shield_mod;
static obj_template_t *sensor_shield_obj;
static char serial_num[64];
static char model_name[64];

/* latest readings, shared between the sampler threads and the getter */
typedef struct shield_state_t_ {
    /* PMS7003 */
    time_t pm_time;
    uint16_t pm1_0;
    uint16_t pm2_5;
    uint16_t pm10;
    /* SHT41 */
    time_t sht_time;
    float temperature;
    float humidity;
    /* SCD41 */
    time_t scd_time;
    uint16_t co2;
    /* SGP41 */
    time_t sgp_time;
    uint16_t sraw_voc;
    uint16_t sraw_nox;
    int32_t voc_index;
    int32_t nox_index;
    /* LPS22HB */
    time_t lps_time;
    float pressure;
    float lps_temperature;
    /* BH1750 */
    time_t bh_time;
    float illuminance;
} shield_state_t;

static shield_state_t state;
static pthread_mutex_t state_lock = PTHREAD_MUTEX_INITIALIZER;

static volatile int stop_threads;
static pthread_t i2c_thread;
static pthread_t uart_thread;
static int i2c_thread_started;
static int uart_thread_started;

/* Helpers */

static void format_timestamp(time_t t, char *buf, size_t len)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
}

static void sleep_ms(unsigned ms)
{
    struct timespec ts;
    ts.tv_sec = ms / 1000;
    ts.tv_nsec = (long)(ms % 1000) * 1000000L;
    while (nanosleep(&ts, &ts) < 0 && errno == EINTR && !stop_threads) {
        ;
    }
}

/* read a NUL terminated string from a /proc/device-tree file */
static void read_dt_string(const char *path, char *buf, size_t len,
                           const char *fallback)
{
    FILE *fp;
    size_t n = 0;

    fp = fopen(path, "r");
    if (fp != NULL) {
        n = fread(buf, 1, len - 1, fp);
        fclose(fp);
    }
    buf[n] = '\0';
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' ' || buf[n-1] == '\0')) {
        buf[--n] = '\0';
    }
    if (n == 0) {
        snprintf(buf, len, "%s", fallback);
    }
}

/* Sampler threads */

/*
 * I2C sampler: SHT41, SGP41 (+ gas index algorithm), SCD41, LPS22HB and
 * BH1750, once per second.
 */
static void *i2c_sampler(void *arg)
{
    GasIndexAlgorithmParams voc_params;
    GasIndexAlgorithmParams nox_params;
    struct timespec next;
    unsigned tick = 0;
    int conditioning_left = SGP41_CONDITIONING_S;
    int scd41_running = 0;
    int scd41_errors = 0;
    uint8_t lps22hb_addr = 0;
    uint8_t bh1750_addr = 0;
    uint16_t rh_ticks = SGP41_DEFAULT_RH_TICKS;
    uint16_t t_ticks = SGP41_DEFAULT_T_TICKS;
    int sht41_ok = 0; /* SHT41 read in the current loop iteration */

    (void)arg;

    GasIndexAlgorithm_init(&voc_params, GasIndexAlgorithm_ALGORITHM_TYPE_VOC);
    GasIndexAlgorithm_init(&nox_params, GasIndexAlgorithm_ALGORITHM_TYPE_NOX);

    clock_gettime(CLOCK_MONOTONIC, &next);

    while (!stop_threads) {
        time_t now = time(NULL);
        int probe = (tick % PROBE_RETRY_S) == 0;
        float temperature, humidity;
        float pressure, lps_temperature;
        float illuminance;
        uint16_t sraw_voc, sraw_nox;
        uint16_t co2;
        struct timespec now_mono;

        /* SHT41 temperature and humidity, also used to compensate the SGP41 */
        sht41_ok = (sht4x_measure_high_precision(&temperature, &humidity) == 0);
        if (sht41_ok) {
            rh_ticks = sgp41_rh_to_ticks(humidity);
            t_ticks = sgp41_t_to_ticks(temperature);
            pthread_mutex_lock(&state_lock);
            state.temperature = temperature;
            state.humidity = humidity;
            state.sht_time = now;
            pthread_mutex_unlock(&state_lock);
        }

        /* SGP41 VOC and NOx */
        if (conditioning_left > 0) {
            if (sgp41_execute_conditioning(rh_ticks, t_ticks, &sraw_voc) == 0) {
                conditioning_left--;
            }
        } else if (sgp41_measure_raw_signals(rh_ticks, t_ticks,
                                             &sraw_voc, &sraw_nox) == 0) {
            int32_t voc_index, nox_index;
            GasIndexAlgorithm_process(&voc_params, sraw_voc, &voc_index);
            GasIndexAlgorithm_process(&nox_params, sraw_nox, &nox_index);
            pthread_mutex_lock(&state_lock);
            state.sraw_voc = sraw_voc;
            state.sraw_nox = sraw_nox;
            state.voc_index = voc_index;
            state.nox_index = nox_index;
            state.sgp_time = now;
            pthread_mutex_unlock(&state_lock);
        }

        /* SCD41 CO2, periodic measurement mode, new data every 5 s */
        if (!scd41_running) {
            /* stop is acknowledged in idle mode too and leaves the sensor
               in a known state after a restart of netconfd */
            if (probe && scd4x_stop_periodic_measurement() == 0 &&
                scd4x_start_periodic_measurement() == 0) {
                scd41_running = 1;
                scd41_errors = 0;
            }
        } else {
            int ready = 0;
            float scd_temperature, scd_humidity;
            if (scd4x_get_data_ready_status(&ready) != 0) {
                if (++scd41_errors >= SCD41_MAX_ERRORS) {
                    scd41_running = 0;
                }
            } else {
                scd41_errors = 0;
                if (ready && scd4x_read_measurement(&co2, &scd_temperature,
                                                    &scd_humidity) == 0) {
                    /* without an SHT41 the SCD41's own (less accurate)
                       temperature/humidity compensate the SGP41 */
                    if (!sht41_ok) {
                        rh_ticks = sgp41_rh_to_ticks(scd_humidity);
                        t_ticks = sgp41_t_to_ticks(scd_temperature);
                    }
                    pthread_mutex_lock(&state_lock);
                    state.co2 = co2;
                    state.scd_time = now;
                    pthread_mutex_unlock(&state_lock);
                }
            }
        }

        /* LPS22HB pressure, continuous 1 Hz mode */
        if (lps22hb_addr == 0) {
            if (probe) {
                lps22hb_addr = lps22hb_init();
            }
        } else {
            int16_t res = lps22hb_read(lps22hb_addr, &pressure,
                                       &lps_temperature);
            if (res == 0) {
                pthread_mutex_lock(&state_lock);
                state.pressure = pressure;
                state.lps_temperature = lps_temperature;
                state.lps_time = now;
                pthread_mutex_unlock(&state_lock);
            } else if (res < 0) {
                lps22hb_addr = 0;
            }
        }

        /* BH1750 illuminance, continuous high resolution mode */
        if (bh1750_addr == 0) {
            if (probe) {
                bh1750_addr = bh1750_init(lps22hb_addr);
            }
        } else if (bh1750_read(bh1750_addr, &illuminance) == 0) {
            pthread_mutex_lock(&state_lock);
            state.illuminance = illuminance;
            state.bh_time = now;
            pthread_mutex_unlock(&state_lock);
        } else {
            bh1750_addr = 0;
        }

        /* wait for the next 1 s slot, resynchronise if we fell behind */
        tick++;
        next.tv_sec += SAMPLE_INTERVAL_S;
        clock_gettime(CLOCK_MONOTONIC, &now_mono);
        if (now_mono.tv_sec > next.tv_sec) {
            next = now_mono;
        }
        while (!stop_threads &&
               clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &next, NULL)
                   == EINTR) {
            ;
        }
    }

    if (conditioning_left < SGP41_CONDITIONING_S) {
        sgp41_turn_heater_off();
    }
    if (scd41_running) {
        scd4x_stop_periodic_measurement();
    }
    return NULL;
}

/*
 * UART reader: the PMS7003 sends a frame every ~1-2.3 s in active mode.
 */
static void *uart_reader(void *arg)
{
    const char *device = getenv("PMS7003_DEVICE");

    (void)arg;

    while (!stop_threads) {
        pms7003_data_t pm;
        if (pms7003_read(device, &pm, PMS7003_TIMEOUT_MS) == 0) {
            pthread_mutex_lock(&state_lock);
            state.pm1_0 = pm.pm1_0_atm;
            state.pm2_5 = pm.pm2_5_atm;
            state.pm10 = pm.pm10_atm;
            state.pm_time = time(NULL);
            pthread_mutex_unlock(&state_lock);
        } else {
            /* device missing or no valid frame, do not spin */
            sleep_ms(1000);
        }
    }
    return NULL;
}

static void start_sampler_threads(void)
{
    stop_threads = 0;
    i2c_thread_started = (pthread_create(&i2c_thread, NULL,
                                         i2c_sampler, NULL) == 0);
    uart_thread_started = (pthread_create(&uart_thread, NULL,
                                          uart_reader, NULL) == 0);
}

static void stop_sampler_threads(void)
{
    stop_threads = 1;
    if (i2c_thread_started) {
        pthread_join(i2c_thread, NULL);
        i2c_thread_started = 0;
    }
    if (uart_thread_started) {
        pthread_join(uart_thread, NULL);
        uart_thread_started = 0;
    }
}

/* XML generation */

static size_t xml_append(char *buf, size_t used, const char *fmt, ...)
{
    va_list ap;
    int n;

    if (used >= BUFSIZE - 1) {
        return used;
    }
    va_start(ap, fmt);
    n = vsnprintf(buf + used, BUFSIZE - used, fmt, ap);
    va_end(ap);
    if (n < 0) {
        return used;
    }
    used += (size_t)n;
    if (used > BUFSIZE - 1) {
        used = BUFSIZE - 1;
    }
    return used;
}

static int is_available(time_t last_update, time_t now, time_t max_age)
{
    return last_update != 0 && now >= last_update &&
           now - last_update <= max_age;
}

/* <container><sensor/><oper-status/>[<last-update/>] ... */
static size_t sensor_open(char *buf, size_t used, const char *container,
                          const char *part, int available, time_t last_update)
{
    used = xml_append(buf, used,
                      "<%s><sensor>%s</sensor><oper-status>%s</oper-status>",
                      container, part, available ? "ok" : "unavailable");
    if (available) {
        char timestamp[32];
        format_timestamp(last_update, timestamp, sizeof(timestamp));
        used = xml_append(buf, used, "<last-update>%s</last-update>",
                          timestamp);
    }
    return used;
}

static size_t sensor_close(char *buf, size_t used, const char *container)
{
    return xml_append(buf, used, "</%s>", container);
}

/* Registered callback functions */

static status_t
    get_sensor_shield(ses_cb_t *scb,
                      getcb_mode_t cbmode,
                      val_value_t *vir_val,
                      val_value_t *dst_val)
{
    status_t res;
    char buf[BUFSIZE];
    size_t used = 0;
    shield_state_t s;
    time_t now = time(NULL);
    int available;

    (void)scb;
    (void)cbmode;

    pthread_mutex_lock(&state_lock);
    s = state;
    pthread_mutex_unlock(&state_lock);

    used = xml_append(buf, used,
        "<sensor-shield xmlns=\"" HS_SENSOR_SHIELD_NS "\">"
        "<host>"
        "<serial-num>%s</serial-num>"
        "<model>%s</model>"
        "</host>",
        serial_num, model_name);

    /* Plantower PMS7003 */
    available = is_available(s.pm_time, now, MAX_AGE_PMS7003_S);
    used = sensor_open(buf, used, "particulate-matter", "PMS7003",
                       available, s.pm_time);
    if (available) {
        used = xml_append(buf, used,
                          "<pm1-0>%u</pm1-0><pm2-5>%u</pm2-5><pm10>%u</pm10>",
                          s.pm1_0, s.pm2_5, s.pm10);
    }
    used = sensor_close(buf, used, "particulate-matter");

    /* Sensirion SCD41 */
    available = is_available(s.scd_time, now, MAX_AGE_SCD41_S);
    used = sensor_open(buf, used, "carbon-dioxide", "SCD41",
                       available, s.scd_time);
    if (available) {
        used = xml_append(buf, used, "<co2>%u</co2>", s.co2);
    }
    used = sensor_close(buf, used, "carbon-dioxide");

    /* Sensirion SGP41 */
    available = is_available(s.sgp_time, now, MAX_AGE_I2C_S);
    used = sensor_open(buf, used, "air-quality", "SGP41",
                       available, s.sgp_time);
    if (available) {
        used = xml_append(buf, used,
                          "<voc-index>%d</voc-index>"
                          "<nox-index>%d</nox-index>"
                          "<voc-raw>%u</voc-raw>"
                          "<nox-raw>%u</nox-raw>",
                          (int)s.voc_index, (int)s.nox_index,
                          s.sraw_voc, s.sraw_nox);
    }
    used = sensor_close(buf, used, "air-quality");

    /* Sensirion SHT41 */
    available = is_available(s.sht_time, now, MAX_AGE_I2C_S);
    used = sensor_open(buf, used, "temperature-humidity", "SHT41",
                       available, s.sht_time);
    if (available) {
        used = xml_append(buf, used,
                          "<temperature>%.2f</temperature>"
                          "<humidity>%.2f</humidity>",
                          s.temperature, s.humidity);
    }
    used = sensor_close(buf, used, "temperature-humidity");

    /* ST LPS22HB */
    available = is_available(s.lps_time, now, MAX_AGE_I2C_S);
    used = sensor_open(buf, used, "pressure", "LPS22HB",
                       available, s.lps_time);
    if (available) {
        used = xml_append(buf, used,
                          "<pressure>%.2f</pressure>"
                          "<temperature>%.2f</temperature>",
                          s.pressure, s.lps_temperature);
    }
    used = sensor_close(buf, used, "pressure");

    /* ROHM BH1750 */
    available = is_available(s.bh_time, now, MAX_AGE_I2C_S);
    used = sensor_open(buf, used, "illuminance", "BH1750",
                       available, s.bh_time);
    if (available) {
        used = xml_append(buf, used, "<illuminance>%.1f</illuminance>",
                          s.illuminance);
    }
    used = sensor_close(buf, used, "illuminance");

    used = xml_append(buf, used, "</sensor-shield>");

    res = val_set_cplxval_obj(dst_val,
                              vir_val->obj,
                              buf);
    /* disable cache */
    vir_val->cachetime = 0;

    return res;
}

/* The 3 mandatory callback functions: y_hs_sensor_shield_init,
   y_hs_sensor_shield_init2, y_hs_sensor_shield_cleanup */

status_t
    y_hs_sensor_shield_init (
        const xmlChar *modname,
        const xmlChar *revision)
{
    agt_profile_t *agt_profile;
    status_t res;

    (void)modname;
    (void)revision;

    agt_profile = agt_get_profile();

    res = ncxmod_load_module(
        (const xmlChar *)HS_SENSOR_SHIELD_MOD,
        NULL,
        &agt_profile->agt_savedevQ,
        &hs_sensor_shield_mod);
    if (res != NO_ERR) {
        return res;
    }

    sensor_shield_obj = ncx_find_object(
        hs_sensor_shield_mod,
        (const xmlChar *)"sensor-shield");
    if (sensor_shield_obj == NULL) {
        return SET_ERROR(ERR_NCX_DEF_NOT_FOUND);
    }

    return res;
}

status_t y_hs_sensor_shield_init2(void)
{
    cfg_template_t *runningcfg;
    val_value_t *sensor_shield_val;

    read_dt_string("/proc/device-tree/serial-number",
                   serial_num, sizeof(serial_num), "unknown");
    read_dt_string("/proc/device-tree/model",
                   model_name, sizeof(model_name), "Raspberry Pi");

    memset(&state, 0, sizeof(state));
    sensirion_i2c_hal_init();
    start_sampler_threads();

    runningcfg = cfg_get_config_id(NCX_CFGID_RUNNING);
    if (!runningcfg || !runningcfg->root) {
        return SET_ERROR(ERR_INTERNAL_VAL);
    }

    sensor_shield_val = val_new_value();
    assert(sensor_shield_val != NULL);

    val_init_virtual(sensor_shield_val,
                     get_sensor_shield,
                     sensor_shield_obj);

    val_add_child(sensor_shield_val, runningcfg->root);

    return NO_ERR;
}

void y_hs_sensor_shield_cleanup (void)
{
    stop_sampler_threads();
    sensirion_i2c_hal_free();
}

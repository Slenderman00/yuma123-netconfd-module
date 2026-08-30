/*
    module hs-sensor-shield (SIL for the ietf-hardware-state YANG module,
    RFC 8348)

    Raspberry Pi implementation for the Heimonen Solutions sensor shield,
    every measurement is a ianahw:sensor component under /hardware:
      - PM1.0/PM2.5/PM10        Plantower PMS7003     UART (/dev/serial0)
      - CO2                     Sensirion SCD41       I2C 0x62
      - VOC / NOx index         Sensirion SGP41       I2C 0x59
      - temperature / humidity  Sensirion SHT41       I2C 0x44
      - pressure                ST LPS22HB            I2C 0x5c/0x5d
      - illuminance             ROHM BH1750           I2C 0x23/0x5c

    The sensors are sampled continuously by two background threads
    (the SGP41 gas index algorithm has to run at 1 Hz and the PMS7003
    streams frames on its own). A <get> of /hardware returns a snapshot
    of the latest readings; a sensor whose last good reading is too old
    is reported with <oper-status>unavailable</oper-status>.
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
#include "log.h"
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

#define HARDWARE_STATE_MOD "ietf-hardware-state"

/* I2C sensors are sampled once per second (required by the gas index
   algorithm, GasIndexAlgorithm_DEFAULT_SAMPLING_INTERVAL) */
#define SAMPLE_INTERVAL_S 1
/* the SGP41 NOx hotplate is conditioned for 10 s after power up */
#define SGP41_CONDITIONING_S 10
/* how long to wait for a PMS7003 frame (sent every ~1-2.3 s) */
#define PMS7003_TIMEOUT_MS 3000
/* sensors that were not found are probed again this often */
#define PROBE_RETRY_S 10
/* consecutive seconds of SCD41 bus errors before it is re-initialised */
#define SCD41_MAX_ERRORS 10
/* seconds without a new SCD41 sample before periodic mode is restarted */
#define SCD41_MAX_IDLE_S 30
/* after a (re)start of periodic measurement the SCD41's self heating
   compensation ramps up and its temperature/humidity read high for a long
   time: do not use them (fallback reporting, SGP41 compensation) before.
   Seconds, overridable with the environment variable below. */
#define SCD41_SETTLE_ENV "HS_SENSOR_SHIELD_SCD41_SETTLE_S"
#define SCD41_SETTLE_S_DEFAULT 3600

/* the sensors need time to initialise after power up: for this long after
   the module starts every sensor is reported unavailable, no values */
#define WARMUP_S 60

/* daily quiet window (local time) during which the heat sensitive
   measurements (temperature, humidity, voc-index, nox-index) are not
   reported: the system maintenance jobs (apt, man-db, fstrim, ...) run at
   midnight and heat up the board. HH:MM-HH:MM, empty string disables. */
#define QUIET_WINDOW_ENV "HS_SENSOR_SHIELD_QUIET_WINDOW"
#define QUIET_WINDOW_DEFAULT "00:00-01:00"

/* a reading older than this is reported as unavailable */
#define MAX_AGE_I2C_S     5
#define MAX_AGE_SCD41_S   15 /* new sample every 5 s */
#define MAX_AGE_PMS7003_S 10

#define BUFSIZE 8*1024

/* module static variables */
static ncx_module_t *ietf_hardware_state_mod;
static obj_template_t *hardware_obj;
static char last_change[32];
static char serial_num[64];
static struct timespec start_time; /* CLOCK_MONOTONIC, module start */
static int quiet_start = -1; /* minutes since midnight, -1 = no window */
static long scd41_settle_s = SCD41_SETTLE_S_DEFAULT;
static int quiet_end = -1;
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
    float scd_temperature;
    float scd_humidity;
    time_t scd_started;  /* CLOCK_MONOTONIC seconds of the last (re)start */
    unsigned scd_restarts;
    unsigned scd_errors_total;
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

static void init_scd41_settle(void)
{
    const char *spec = getenv(SCD41_SETTLE_ENV);
    char *end;
    long v;

    scd41_settle_s = SCD41_SETTLE_S_DEFAULT;
    if (spec == NULL || *spec == '\0') {
        return;
    }
    v = strtol(spec, &end, 10);
    if (*end == '\0' && v >= 0) {
        scd41_settle_s = v;
    } else {
        log_warn("hs-sensor-shield: ignoring invalid %s='%s'",
                 SCD41_SETTLE_ENV, spec);
    }
}

/* parse "HH:MM-HH:MM" into quiet_start/quiet_end */
static void init_quiet_window(void)
{
    const char *spec = getenv(QUIET_WINDOW_ENV);
    int sh, sm, eh, em;

    if (spec == NULL) {
        spec = QUIET_WINDOW_DEFAULT;
    }
    quiet_start = quiet_end = -1;
    if (*spec == '\0') {
        return;
    }
    if (sscanf(spec, "%d:%d-%d:%d", &sh, &sm, &eh, &em) == 4 &&
        sh >= 0 && sh < 24 && sm >= 0 && sm < 60 &&
        eh >= 0 && eh < 24 && em >= 0 && em < 60) {
        quiet_start = sh * 60 + sm;
        quiet_end = eh * 60 + em;
    } else {
        log_warn("hs-sensor-shield: ignoring invalid %s='%s'",
                 QUIET_WINDOW_ENV, spec);
    }
}

/* 1 while inside the quiet window (local time), windows may wrap midnight */
static int in_quiet_window(time_t now)
{
    struct tm tm;
    int minutes;

    if (quiet_start < 0 || quiet_start == quiet_end) {
        return 0;
    }
    localtime_r(&now, &tm);
    minutes = tm.tm_hour * 60 + tm.tm_min;
    if (quiet_start < quiet_end) {
        return minutes >= quiet_start && minutes < quiet_end;
    }
    return minutes >= quiet_start || minutes < quiet_end;
}

static void timestamp_now(char *buf, size_t len)
{
    format_timestamp(time(NULL), buf, len);
}

static time_t now_mono_s(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec;
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
    int scd41_idle_ticks = 0; /* ticks without a new SCD41 sample */
    const char *scd41_restart_reason = "module start";
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
            if (probe) {
                int16_t e1 = scd4x_stop_periodic_measurement();
                int16_t e2 = e1 ? -1 : scd4x_start_periodic_measurement();
                if (e1 == 0 && e2 == 0) {
                    scd41_running = 1;
                    scd41_errors = 0;
                    scd41_idle_ticks = 0;
                    pthread_mutex_lock(&state_lock);
                    state.scd_started = now_mono_s();
                    state.scd_restarts++;
                    log_info("\nhs-sensor-shield: SCD41 periodic measurement "
                             "started (#%u, %s)", state.scd_restarts,
                             scd41_restart_reason);
                    pthread_mutex_unlock(&state_lock);
                } else {
                    log_info("\nhs-sensor-shield: SCD41 start failed "
                             "(stop=%d start=%d)", e1, e2);
                }
            }
        } else {
            int ready = 0;
            float scd_temperature, scd_humidity;
            int16_t err = scd4x_get_data_ready_status(&ready);
            if (err != 0) {
                pthread_mutex_lock(&state_lock);
                state.scd_errors_total++;
                pthread_mutex_unlock(&state_lock);
                log_info("\nhs-sensor-shield: SCD41 data-ready error %d "
                         "(%d consecutive)", err, scd41_errors + 1);
                if (++scd41_errors >= SCD41_MAX_ERRORS) {
                    scd41_running = 0;
                    scd41_restart_reason = "bus errors";
                }
            } else {
                scd41_errors = 0;
                /* a sample is due every 5 s; if none arrives for a long time
                   periodic measurement was stopped behind our back, restart */
                if (!ready && ++scd41_idle_ticks >= SCD41_MAX_IDLE_S) {
                    scd41_running = 0;
                    scd41_restart_reason = "no samples";
                    log_info("\nhs-sensor-shield: SCD41 no sample for %d s",
                             SCD41_MAX_IDLE_S);
                }
                if (ready) {
                    err = scd4x_read_measurement(&co2, &scd_temperature,
                                                 &scd_humidity);
                    if (err != 0) {
                        pthread_mutex_lock(&state_lock);
                        state.scd_errors_total++;
                        pthread_mutex_unlock(&state_lock);
                        log_info("\nhs-sensor-shield: SCD41 read error %d",
                                 err);
                    }
                }
                if (ready && err == 0) {
                    time_t started;
                    scd41_idle_ticks = 0;
                    pthread_mutex_lock(&state_lock);
                    started = state.scd_started;
                    state.co2 = co2;
                    state.scd_temperature = scd_temperature;
                    state.scd_humidity = scd_humidity;
                    state.scd_time = now;
                    pthread_mutex_unlock(&state_lock);
                    /* without an SHT41 the SCD41's own (less accurate)
                       temperature/humidity compensate the SGP41, but not
                       while the SCD41 is settling after a (re)start */
                    if (!sht41_ok &&
                        now_mono_s() - started >= scd41_settle_s) {
                        rh_ticks = sgp41_rh_to_ticks(scd_humidity);
                        t_ticks = sgp41_t_to_ticks(scd_temperature);
                    }
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

/*
 * One ianahw:sensor component (RFC 8348). All values are reported with
 * value-scale milli. When the sensor is not available <oper-status> is
 * unavailable and no <value>/<value-timestamp> is reported.
 */
static size_t append_sensor(char *buf, size_t used, const char *name,
                            const char *mfg_name, const char *model_name,
                            int available, long value,
                            const char *value_type, time_t last_update,
                            unsigned update_rate_ms,
                            const char *units_display)
{
    used = xml_append(buf, used,
        "  <component>"
        "    <name>%s</name>"
        "    <class>ianahw:sensor</class>"
        "    <parent>raspberry-pi</parent>"
        "    <mfg-name>%s</mfg-name>"
        "    <model-name>%s</model-name>"
        "    <sensor-data>",
        name, mfg_name, model_name);
    if (available) {
        char timestamp[32];
        format_timestamp(last_update, timestamp, sizeof(timestamp));
        used = xml_append(buf, used,
            "        <value>%ld</value>"
            "        <value-type>%s</value-type>"
            "        <value-scale>milli</value-scale>"
            "        <value-precision>2</value-precision>"
            "        <oper-status>ok</oper-status>"
            "        <value-timestamp>%s</value-timestamp>"
            "        <value-update-rate>%u</value-update-rate>"
            "        <units-display>%s</units-display>",
            value, value_type, timestamp, update_rate_ms, units_display);
    } else {
        used = xml_append(buf, used,
            "        <value-type>%s</value-type>"
            "        <value-scale>milli</value-scale>"
            "        <value-precision>2</value-precision>"
            "        <oper-status>unavailable</oper-status>"
            "        <value-update-rate>%u</value-update-rate>"
            "        <units-display>%s</units-display>",
            value_type, update_rate_ms, units_display);
    }
    return xml_append(buf, used,
        "    </sensor-data>"
        "  </component>");
}

static long milli(float value)
{
    return (long)(value * 1000.0f + (value >= 0 ? 0.5f : -0.5f));
}

/* Registered callback functions */

static status_t
    get_hardware(ses_cb_t *scb,
                 getcb_mode_t cbmode,
                 val_value_t *vir_val,
                 val_value_t *dst_val)
{
    status_t res;
    char buf[BUFSIZE];
    size_t used = 0;
    shield_state_t s;
    time_t now = time(NULL);
    struct timespec now_mono;
    int pm_ok, scd_ok, sgp_ok, sht_ok, lps_ok, bh_ok;
    int quiet;
    int scd_settled;

    (void)scb;
    (void)cbmode;

    pthread_mutex_lock(&state_lock);
    s = state;
    pthread_mutex_unlock(&state_lock);

    clock_gettime(CLOCK_MONOTONIC, &now_mono);
    if (now_mono.tv_sec - start_time.tv_sec < WARMUP_S) {
        /* still warming up: serve no sensor values at all */
        pm_ok = scd_ok = sgp_ok = sht_ok = lps_ok = bh_ok = 0;
    } else {
        pm_ok  = is_available(s.pm_time,  now, MAX_AGE_PMS7003_S);
        scd_ok = is_available(s.scd_time, now, MAX_AGE_SCD41_S);
        sgp_ok = is_available(s.sgp_time, now, MAX_AGE_I2C_S);
        sht_ok = is_available(s.sht_time, now, MAX_AGE_I2C_S);
        lps_ok = is_available(s.lps_time, now, MAX_AGE_I2C_S);
        bh_ok  = is_available(s.bh_time,  now, MAX_AGE_I2C_S);
    }
    /* maintenance window: the board heats up, do not report the heat
       sensitive measurements (co2, pm, pressure and light are kept) */
    quiet = in_quiet_window(now);
    if (quiet) {
        sht_ok = 0;
        sgp_ok = 0;
    }
    /* SCD41 temperature/humidity read high for minutes after a (re)start */
    scd_settled = (now_mono.tv_sec - s.scd_started >= scd41_settle_s);

    /* /hardware */
    used = xml_append(buf, used,
        "<hardware xmlns=\"urn:ietf:params:xml:ns:yang:ietf-hardware-state\""
        "          xmlns:ianahw=\"urn:ietf:params:xml:ns:yang:iana-hardware\">"
        "  <last-change>%s</last-change>"
        "  <component>"
        "    <name>raspberry-pi</name>"
        "    <class>ianahw:container</class>"
        "    <serial-num>%s</serial-num>"
        "    <mfg-name>Raspberry Pi</mfg-name>"
        "    <model-name>%s</model-name>"
        "  </component>",
        last_change, serial_num, model_name);

    /* temperature / humidity: Sensirion SHT41, with the SCD41's built-in
       temperature/humidity sensor as automatic fallback */
    if (sht_ok) {
        used = append_sensor(buf, used, "temperature", "Sensirion", "SHT41",
                             1, milli(s.temperature), "celsius",
                             s.sht_time, 1000, "milli degrees");
        used = append_sensor(buf, used, "humidity", "Sensirion", "SHT41",
                             1, milli(s.humidity), "percent-RH",
                             s.sht_time, 1000, "milli percent RH");
    } else if (scd_ok && !quiet && scd_settled) {
        used = append_sensor(buf, used, "temperature", "Sensirion", "SCD41",
                             1, milli(s.scd_temperature), "celsius",
                             s.scd_time, 5000, "milli degrees");
        used = append_sensor(buf, used, "humidity", "Sensirion", "SCD41",
                             1, milli(s.scd_humidity), "percent-RH",
                             s.scd_time, 5000, "milli percent RH");
    } else {
        used = append_sensor(buf, used, "temperature", "Sensirion", "SHT41",
                             0, 0, "celsius", 0, 1000, "milli degrees");
        used = append_sensor(buf, used, "humidity", "Sensirion", "SHT41",
                             0, 0, "percent-RH", 0, 1000, "milli percent RH");
    }

    /* Plantower PMS7003: ug/m3 (atmospheric environment), reported x1000 */
    used = append_sensor(buf, used, "pm1", "Plantower", "PMS7003",
                         pm_ok, (long)s.pm1_0 * 1000, "other",
                         s.pm_time, 1000, "PM1 particles");
    used = append_sensor(buf, used, "pm25", "Plantower", "PMS7003",
                         pm_ok, (long)s.pm2_5 * 1000, "other",
                         s.pm_time, 1000, "PM25 particles");
    used = append_sensor(buf, used, "pm10", "Plantower", "PMS7003",
                         pm_ok, (long)s.pm10 * 1000, "other",
                         s.pm_time, 1000, "PM10 particles");

    /* Sensirion SCD41: ppm, reported x1000 */
    used = append_sensor(buf, used, "co2", "Sensirion", "SCD41",
                         scd_ok, (long)s.co2 * 1000, "other",
                         s.scd_time, 5000, "milli ppm");

    /* Sensirion SGP41: gas index 1..500, reported x1000 */
    used = append_sensor(buf, used, "voc-index", "Sensirion", "SGP41",
                         sgp_ok, (long)s.voc_index * 1000, "other",
                         s.sgp_time, 1000, "milli VOC index");
    used = append_sensor(buf, used, "nox-index", "Sensirion", "SGP41",
                         sgp_ok, (long)s.nox_index * 1000, "other",
                         s.sgp_time, 1000, "milli NOx index");

    /* ST LPS22HB: hPa, reported x1000 */
    used = append_sensor(buf, used, "pressure", "ST", "LPS22HB",
                         lps_ok, milli(s.pressure), "other",
                         s.lps_time, 1000, "milli hPa");

    /* ROHM BH1750: lux, reported x1000 */
    used = append_sensor(buf, used, "illuminance", "ROHM", "BH1750",
                         bh_ok, milli(s.illuminance), "other",
                         s.bh_time, 1000, "milli lux");

    used = xml_append(buf, used, "</hardware>");

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
        (const xmlChar *)HARDWARE_STATE_MOD,
        NULL,
        &agt_profile->agt_savedevQ,
        &ietf_hardware_state_mod);
    if (res != NO_ERR) {
        return res;
    }

    hardware_obj = ncx_find_object(
        ietf_hardware_state_mod,
        (const xmlChar *)"hardware");
    if (hardware_obj == NULL) {
        return SET_ERROR(ERR_NCX_DEF_NOT_FOUND);
    }

    return res;
}

status_t y_hs_sensor_shield_init2(void)
{
    cfg_template_t *runningcfg;
    val_value_t *hardware_val;

    timestamp_now(last_change, sizeof(last_change));
    read_dt_string("/proc/device-tree/serial-number",
                   serial_num, sizeof(serial_num), "unknown");
    read_dt_string("/proc/device-tree/model",
                   model_name, sizeof(model_name), "Raspberry Pi");
    /* the device tree model repeats the manufacturer ("Raspberry Pi 4 Model B
       Rev 1.5"), report mfg-name "Raspberry Pi" and model-name "4 Model B..." */
    if (strncmp(model_name, "Raspberry Pi ", 13) == 0) {
        memmove(model_name, model_name + 13, strlen(model_name + 13) + 1);
    }

    memset(&state, 0, sizeof(state));
    clock_gettime(CLOCK_MONOTONIC, &start_time);
    init_quiet_window();
    init_scd41_settle();
    sensirion_i2c_hal_init();
    start_sampler_threads();

    runningcfg = cfg_get_config_id(NCX_CFGID_RUNNING);
    if (!runningcfg || !runningcfg->root) {
        return SET_ERROR(ERR_INTERNAL_VAL);
    }

    hardware_val = val_new_value();
    assert(hardware_val != NULL);

    val_init_virtual(hardware_val,
                     get_hardware,
                     hardware_obj);

    val_add_child(hardware_val, runningcfg->root);

    return NO_ERR;
}

void y_hs_sensor_shield_cleanup (void)
{
    stop_sampler_threads();
    sensirion_i2c_hal_free();
}

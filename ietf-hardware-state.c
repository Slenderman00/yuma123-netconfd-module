/*
    module ietf-hardware-state

    Raspberry Pi implementation reporting:
      - temperature from a Sensirion STS3x on I2C (/dev/i2c-1, address 0x4a)
      - PM1.0/PM2.5/PM10 from a Plantower PMS7003 on UART (/dev/serial0)
 */

#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <sys/time.h>
#include <sys/wait.h>


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

/* Sensirion */
#include "sensirion_config.h"
#include "sensirion_common.h"
#include "sensirion_i2c.h"
#include "sensirion_i2c_hal.h"
#include "sts3x_i2c.h"

/* Plantower */
#include "pms7003.h"

#define sensirion_hal_sleep_us sensirion_i2c_hal_sleep_usec

/* how long to wait for a fresh PMS7003 frame (sent every ~1-2.3 s) */
#define PMS7003_TIMEOUT_MS 3000

/* module static variables */
static ncx_module_t *ietf_hardware_state_mod;
static obj_template_t* hardware_obj;
static char last_change[32];
static char serial_num[64];
static char model_name[64];

#define BUFSIZE 8*1024

/* Sensors */

static int init_sensors(void)
{
    sensirion_i2c_hal_init();
    sts3x_init(STS30_I2C_ADDR_4A);
    return 0;
}

/* returns 0 and temperature in degrees Celsius on success */
static int read_temperature(float *temperature)
{
    uint16_t raw_temp = 0;
    int16_t error;

    error = sts3x_measure_single_shot_medium_repeatability(&raw_temp);
    if (error != NO_ERROR) {
        return -1;
    }
    *temperature = signal_temperature(raw_temp);
    return 0;
}

static int read_particles(pms7003_data_t *data)
{
    const char *device = getenv("PMS7003_DEVICE");
    return pms7003_read(device, data, PMS7003_TIMEOUT_MS);
}

/* Helpers */

static void timestamp_now(char *buf, size_t len)
{
    time_t now = time(NULL);
    struct tm tm;
    gmtime_r(&now, &tm);
    strftime(buf, len, "%Y-%m-%dT%H:%M:%SZ", &tm);
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
    while (n > 0 && (buf[n-1] == '\n' || buf[n-1] == ' ')) {
        buf[--n] = '\0';
    }
    if (n == 0) {
        snprintf(buf, len, "%s", fallback);
    }
}

/* Registered callback functions */

static const char* sensor_template =
"  <component>"
"    <name>%s</name>"
"    <class>ianahw:sensor</class>"
"    <parent>raspberry-pi</parent>"
"    <sensor-data>"
"        <value>%d</value>"
"        <value-type>%s</value-type>"
"        <value-scale>milli</value-scale>"
"        <value-precision>2</value-precision>"
"        <oper-status>ok</oper-status>"
"        <value-timestamp>%s</value-timestamp>"
"        <value-update-rate>0</value-update-rate>"
"        <units-display>%s</units-display>"
"    </sensor-data>"
"  </component>";

/* emitted when a sensor could not be read */
static const char* sensor_unavailable_template =
"  <component>"
"    <name>%s</name>"
"    <class>ianahw:sensor</class>"
"    <parent>raspberry-pi</parent>"
"    <sensor-data>"
"        <value-type>%s</value-type>"
"        <value-scale>milli</value-scale>"
"        <value-precision>2</value-precision>"
"        <oper-status>unavailable</oper-status>"
"        <value-update-rate>0</value-update-rate>"
"        <units-display>%s</units-display>"
"    </sensor-data>"
"  </component>";

static int append_sensor(char *buf, size_t len, int available,
                         const char *name, int value, const char *value_type,
                         const char *timestamp, const char *units_display)
{
    size_t used = strlen(buf);
    if (available) {
        return snprintf(buf + used, len - used, sensor_template,
                        name, value, value_type, timestamp, units_display);
    } else {
        return snprintf(buf + used, len - used, sensor_unavailable_template,
                        name, value_type, units_display);
    }
}

static status_t
    get_hardware(ses_cb_t *scb,
                 getcb_mode_t cbmode,
                 val_value_t *vir_val,
                 val_value_t *dst_val)
{
    status_t res;
    char buf[BUFSIZE];
    char timestamp[32];
    float temperature = 0.0;
    int temperature_ok;
    pms7003_data_t pm;
    int pm_ok;

    /* /hardware */

    temperature_ok = (read_temperature(&temperature) == 0);
    pm_ok = (read_particles(&pm) == 0);
    timestamp_now(timestamp, sizeof(timestamp));

    snprintf(buf, BUFSIZE,
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

    /* Sensirion STS3x: degrees Celsius, reported in milli degrees */
    append_sensor(buf, BUFSIZE, temperature_ok, "temperature",
                  (int)(temperature * 1000), "celsius", timestamp,
                  "milli degrees");

    /* Plantower PMS7003: ug/m3 (atmospheric environment), reported x1000 */
    append_sensor(buf, BUFSIZE, pm_ok, "pm1",
                  pm.pm1_0_atm * 1000, "other", timestamp, "PM1 particles");
    append_sensor(buf, BUFSIZE, pm_ok, "pm25",
                  pm.pm2_5_atm * 1000, "other", timestamp, "PM25 particles");
    append_sensor(buf, BUFSIZE, pm_ok, "pm10",
                  pm.pm10_atm * 1000, "other", timestamp, "PM10 particles");

    strncat(buf, "</hardware>", BUFSIZE - strlen(buf) - 1);

    res = val_set_cplxval_obj(dst_val,
                              vir_val->obj,
                              buf);
    /* disable cache */
    vir_val->cachetime = 0;

    return res;
}

/* The 3 mandatory callback functions: y_ietf_hardware_state_init, y_ietf_hardware_state_init2, y_ietf_hardware_state_cleanup */

status_t
    y_ietf_hardware_state_init (
        const xmlChar *modname,
        const xmlChar *revision)
{
    agt_profile_t *agt_profile;
    status_t res;

    agt_profile = agt_get_profile();

    res = ncxmod_load_module(
        "ietf-hardware-state",
        NULL,
        &agt_profile->agt_savedevQ,
        &ietf_hardware_state_mod);
    if (res != NO_ERR) {
        return res;
    }

    hardware_obj = ncx_find_object(
        ietf_hardware_state_mod,
        "hardware");
    if (hardware_obj == NULL) {
        return SET_ERROR(ERR_NCX_DEF_NOT_FOUND);
    }

    return res;
}

status_t y_ietf_hardware_state_init2(void)
{
    status_t res;
    cfg_template_t* runningcfg;
    val_value_t* hardware_val;

    res = NO_ERR;

    init_sensors();
    timestamp_now(last_change, sizeof(last_change));
    read_dt_string("/proc/device-tree/serial-number",
                   serial_num, sizeof(serial_num), "unknown");
    read_dt_string("/proc/device-tree/model",
                   model_name, sizeof(model_name), "Raspberry Pi");

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


    return res;
}

void y_ietf_hardware_state_cleanup (void)
{
    sensirion_i2c_hal_free();
}

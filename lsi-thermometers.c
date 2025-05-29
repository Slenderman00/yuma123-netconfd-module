/*
    module lsi-thermometers
 */

#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 700
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <string.h>
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

#define sensirion_hal_sleep_us sensirion_i2c_hal_sleep_usec

int16_t error = NO_ERROR;

int init_sensor() {
    sensirion_i2c_hal_init();
    sts3x_init(STS30_I2C_ADDR_4A);

    sts3x_stop_measurement();
    sensirion_hal_sleep_us(1000);
    sts3x_soft_reset();
    sensirion_hal_sleep_us(100000);
    uint16_t a_status_register = 0u;
    error = sts3x_read_status_register(&a_status_register);
    if (error != NO_ERROR) {
        printf("error executing read_status_register(): %i\n", error);
        return error;
    }
    printf("a_status_register: %02x\n", a_status_register);
    error = sts3x_start_periodic_measurement(REPEATABILITY_MEDIUM,
                                             MPS_ONE_PER_SECOND);
    if (error != NO_ERROR) {
        printf("error executing start_periodic_measurement(): %i\n", error);
        return error;
    }
}

int read_temp(int repetitions) {
    float a_temperature = 0.0;
    uint16_t repetition = 0;
    for (repetition = 0; repetition < repetitions; repetition++) {
        error = sts3x_blocking_read_measurement(&a_temperature);
        if (error != NO_ERROR) {
            printf("error executing blocking_read_measurement(): %i\n", error);
            continue;
        }
    }

    return a_temperature;
}


/* module static variables */
static ncx_module_t *lsi_thermometers_mod;
static obj_template_t* thermometers_obj;

#define BUFSIZE 1024

/* Registered callback functions */

static status_t
    get_thermometers(ses_cb_t *scb,
                     getcb_mode_t cbmode,
                     val_value_t *vir_val,
                     val_value_t *dst_val)
{
    status_t res;
    char* ptr;
    res = NO_ERR;

    /* /thermometers */

    char *cmd = "thermometers-get";

    char buf[BUFSIZE];
    FILE *fp;

    int temp = read_temp(10);
    temp = temp * 100;

    sprintf(buf, "<thermometers xmlns=\"urn:lsi:params:xml:ns:yang:thermometers\"><thermometer><name>th0</name><value>%d</value></thermometer></thermometers>", temp);

    res = val_set_cplxval_obj(dst_val,
                              vir_val->obj,
                              buf);
    /* disable cache */
    vir_val->cachetime = 0;

    return res;
}

/* The 3 mandatory callback functions: y_lsi_thermometers_init, y_lsi_thermometers_init2, y_lsi_thermometers_cleanup */

status_t
    y_lsi_thermometers_init (
        const xmlChar *modname,
        const xmlChar *revision)
{
    init_sensor();

    agt_profile_t *agt_profile;
    status_t res;

    agt_profile = agt_get_profile();

    res = ncxmod_load_module(
        "lsi-thermometers",
        NULL,
        &agt_profile->agt_savedevQ,
        &lsi_thermometers_mod);
    if (res != NO_ERR) {
        return res;
    }

    thermometers_obj = ncx_find_object(
        lsi_thermometers_mod,
        "thermometers");
    if (thermometers_obj == NULL) {
        return SET_ERROR(ERR_NCX_DEF_NOT_FOUND);
    }

    return res;
}

status_t y_lsi_thermometers_init2(void)
{
    status_t res;
    cfg_template_t* runningcfg;
    val_value_t* thermometers_val;

    res = NO_ERR;

    runningcfg = cfg_get_config_id(NCX_CFGID_RUNNING);
    if (!runningcfg || !runningcfg->root) {
        return SET_ERROR(ERR_INTERNAL_VAL);
    }

    thermometers_val = val_new_value();
    assert(thermometers_val != NULL);

    val_init_virtual(thermometers_val,
                     get_thermometers,
                     thermometers_obj);

    val_add_child(thermometers_val, runningcfg->root);


    return res;
}

void y_lsi_thermometers_cleanup (void)
{
}

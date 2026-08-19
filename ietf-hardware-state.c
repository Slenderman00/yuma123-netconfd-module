/*
    module ietf-hardware-state

    Matter (over Thread) implementation: the /hardware tree is produced by the
    external helper 'ietf-hardware-state-get' (Python, see ietf-hardware-state-matter)
    which reads the sensor devices, e.g. IKEA ALPSTUGA, from the Open Home
    Foundation Matter Server over its WebSocket API.

    The helper is searched in PATH and can be overridden with the environment
    variable IETF_HARDWARE_STATE_GET. All configuration of the helper
    (MATTER_SERVER_URL, MATTER_NODE_ID, ...) is passed through the environment.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>

#include <libxml/xmlstring.h>
#include "log.h"
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

#define BUFSIZE (1024*1024)

#define DEFAULT_GET_CMD "ietf-hardware-state-get"

/* module static variables */
static ncx_module_t *ietf_hardware_state_mod;
static obj_template_t* hardware_obj;
static const char* get_cmd;

/* Registered callback functions */

static status_t
    get_hardware(ses_cb_t *scb,
                 getcb_mode_t cbmode,
                 val_value_t *vir_val,
                 val_value_t *dst_val)
{
    status_t res;
    char *buf;
    size_t len;
    FILE *fp;
    int rc;

    /* /hardware */

    buf = malloc(BUFSIZE);
    if (buf == NULL) {
        return SET_ERROR(ERR_INTERNAL_MEM);
    }

    fp = popen(get_cmd, "r");
    if (fp == NULL) {
        log_error("\nietf-hardware-state: popen(%s) failed: %s",
                  get_cmd, strerror(errno));
        free(buf);
        return SET_ERROR(ERR_NCX_OPERATION_FAILED);
    }

    len = fread(buf, 1, BUFSIZE - 1, fp);
    buf[len] = '\0';

    rc = pclose(fp);
    if (rc != 0 || len == 0) {
        log_error("\nietf-hardware-state: %s exited with status %d, %zu bytes",
                  get_cmd, rc, len);
        free(buf);
        return SET_ERROR(ERR_NCX_OPERATION_FAILED);
    }

    res = val_set_cplxval_obj(dst_val,
                              vir_val->obj,
                              buf);
    if (res != NO_ERR) {
        log_error("\nietf-hardware-state: rejected output of %s: %s (%d)",
                  get_cmd, get_error_string(res), res);
    }
    free(buf);

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

    get_cmd = getenv("IETF_HARDWARE_STATE_GET");
    if (get_cmd == NULL || get_cmd[0] == '\0') {
        get_cmd = DEFAULT_GET_CMD;
    }
    log_info("\nietf-hardware-state: using '%s'", get_cmd);

    return res;
}

status_t y_ietf_hardware_state_init2(void)
{
    status_t res;
    cfg_template_t* runningcfg;
    val_value_t* hardware_val;

    res = NO_ERR;

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
}

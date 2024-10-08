/*
    module lsi-ivi-load
    namespace urn:lsi:params:xml:ns:yang:ivi-load
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <unistd.h>
#include <errno.h>
#include <libxml/xmlstring.h>
#include "procdefs.h"
#include "agt.h"
#include "agt_cb.h"
#include "agt_commit_complete.h"
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


#define BUFSIZE 1000000

#define load_MOD "lsi-ivi-load"
//static char* visa_resource_name;
static obj_template_t* load_state_obj;


static status_t
    get_load_state(ses_cb_t *scb,
                         getcb_mode_t cbmode,
                         val_value_t *vir_val,
                         val_value_t *dst_val)
{
    status_t res;
    char* ptr;
    res = NO_ERR;

    /* /load-state */

    char cmd_buf[BUFSIZE]="lsi-ivi-load-get";
    char buf[BUFSIZE]="";
    FILE *fp;

    //sprintf(cmd_buf+strlen(cmd_buf), " %s", visa_resource_name);
    if ((fp = popen(cmd_buf, "r")) == NULL) {
        printf("Error opening pipe!\n");
        assert(0);
    }
    do {
        ptr = fgets(buf+strlen(buf), BUFSIZE, fp);
    } while(ptr);

    printf("lsi-ivi-load-get: %s", buf);

    assert(strlen(buf));

    if(pclose(fp))  {
        printf("Command not found or exited with error status\n");
        assert(0);
    }

    res = val_set_cplxval_obj(dst_val,
                              vir_val->obj,
                              buf);
    /* disable cache */
    vir_val->cachetime = 0;

    return res;
}


static int update_config(val_value_t* config_cur_val, val_value_t* config_new_val)
{

    status_t res;

    val_value_t *load_val;
    val_value_t *load_new_val;
    val_value_t *load_cur_val;
    val_value_t *channel_val;
    val_value_t *name_val;
    val_value_t *resistance_val=NULL;
    val_value_t *resistance1_val=NULL;
    val_value_t *resistance2_val=NULL;

    unsigned int i;

    char setcmd_buf[BUFSIZE];
    char* ptr;

    if(config_new_val == NULL) {
        load_new_val = NULL;
    } else {
        load_new_val = val_find_child(config_new_val,
                               load_MOD,
                               "load");
    }

    if(config_cur_val == NULL) {
        load_cur_val = NULL;
    } else {
        load_cur_val = val_find_child(config_cur_val,
                               load_MOD,
                               "load");
    }

    if((load_cur_val==NULL) && (load_new_val==NULL)) {
        return NO_ERR;
    }

    if((load_cur_val!=NULL) && (load_new_val!=NULL) && (0==val_compare_ex(load_new_val, load_cur_val ,TRUE))) {
        printf("Identical configuration detected\n");
        return NO_ERR;
    }

    load_val = load_new_val;

    if(load_val!=NULL) {
        for (channel_val = val_get_first_child(load_val);
             channel_val != NULL;
             channel_val = val_get_next_child(channel_val)) {
            name_val = val_find_child(channel_val,
                               load_MOD,
                               "name");
            resistance_val = val_find_child(channel_val,
                                      load_MOD,
                                      "resistance");

            if(0==strcmp(VAL_STRING(name_val), "out1")) {
                     resistance1_val = resistance_val;
            } else if(0==strcmp(VAL_STRING(name_val), "out2")) {
                     resistance2_val = resistance_val;
            } 
        }
    }


    strcpy(setcmd_buf, "lsi-ivi-load-set");
    //sprintf(setcmd_buf+strlen(setcmd_buf), " \"%s\"", visa_resource_name);

    if(resistance1_val) {
        char* resistance_str;
        resistance_str = val_make_sprintf_string(resistance1_val);
        sprintf(setcmd_buf+strlen(setcmd_buf), " %s %s", "on", resistance_str);
        free(resistance_str);
    } else {
        sprintf(setcmd_buf+strlen(setcmd_buf), " off 0");
    }
    if(resistance2_val) {
        char* resistance_str;
        resistance_str = val_make_sprintf_string(resistance2_val);
        sprintf(setcmd_buf+strlen(setcmd_buf), " %s %s", "on", resistance_str);
        free(resistance_str);
    } else {
        sprintf(setcmd_buf+strlen(setcmd_buf), " off 0");
    }


    printf("Calling: %s\n", setcmd_buf);
    system(setcmd_buf);

    return NO_ERR;
}


static val_value_t* prev_root_val = NULL;
static int update_config_wrapper()
{
    cfg_template_t        *runningcfg;
    status_t res;
    runningcfg = cfg_get_config_id(NCX_CFGID_RUNNING);
    assert(runningcfg!=NULL && runningcfg->root!=NULL);
    if(prev_root_val!=NULL) {
        val_value_t* cur_root_val;
        cur_root_val = val_clone_config_data(runningcfg->root, &res);
        if(0==val_compare(cur_root_val,prev_root_val)) {
            /*no change*/
            val_free_value(cur_root_val);
            return 0;
        }
        val_free_value(cur_root_val);
    }
    update_config(prev_root_val, runningcfg->root);

    if(prev_root_val!=NULL) {
        val_free_value(prev_root_val);
    }
    prev_root_val = val_clone_config_data(runningcfg->root, &res);

    return 0;
}

static status_t y_commit_complete(void)
{
    update_config_wrapper();
    return NO_ERR;
}

/* The 3 mandatory callback functions: y_lsi_ivi_load_init, y_lsi_ivi_load_init2, y_lsi_ivi_load_cleanup */

status_t
    y_lsi_ivi_load_init (
        const xmlChar *modname,
        const xmlChar *revision)
{
    agt_profile_t* agt_profile;
    status_t res;
    ncx_module_t *mod;

    agt_profile = agt_get_profile();

    res = ncxmod_load_module(
        load_MOD,
        NULL,
        &agt_profile->agt_savedevQ,
        &mod);
    if (res != NO_ERR) {
        return res;
    }

    // visa_resource_name = getenv ("LSI_IVI_LOAD_VISA_RESOURCE_NAME");
    // if(visa_resource_name==NULL) {
    //     fprintf(stderr, "Environment variable LSI_IVI_LOAD_VISA_RESOURCE_NAME must be defined. E.g. setenv LSI_IVI_LOAD_VISA_RESOURCE_NAME=\"TCPIP::192.168.14.20::gpib,2::INSTR\"");
    //     return SET_ERROR(ERR_INTERNAL_VAL);
    // } else {
    //     printf("LSI_IVI_LOAD_VISA_RESOURCE_NAME=%s",visa_resource_name);
    // }


    res=agt_commit_complete_register("lsi-ivi-load" /*SIL id string*/,
                                     y_commit_complete);
    assert(res == NO_ERR);

    load_state_obj = ncx_find_object(
        mod,
        "load-state");
    if (load_state_obj == NULL) {
        return SET_ERROR(ERR_NCX_DEF_NOT_FOUND);
    }

    return res;
}

status_t y_lsi_ivi_load_init2(void)
{
    status_t res=NO_ERR;
    cfg_template_t* runningcfg;
    val_value_t* load_state_val;

    res = NO_ERR;

    runningcfg = cfg_get_config_id(NCX_CFGID_RUNNING);
    assert(runningcfg && runningcfg->root);

    load_state_val = val_new_value();
    assert(load_state_val != NULL);

    val_init_virtual(load_state_val,
                     get_load_state,
                     load_state_obj);

    val_add_child(load_state_val, runningcfg->root);

    /* emulate initial startup configuration commit */
    y_commit_complete();

    return res;
}

void y_lsi_ivi_load_cleanup (void)
{
}

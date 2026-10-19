#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "db_bottle.h"
#include "db_schedule.h"

#define CFG_VERSION 1
typedef struct {
    uint32_t version;
    char name[32];
    char token[65];          /* bearer token; empty until provisioned */
    char tz[64];             /* POSIX TZ string, e.g. CST6CDT,M3.2.0,M11.1.0 */
    char webhook[128];       /* URL for pushed events; empty = disabled */
    uint8_t act_mode;        /* act_mode_t */
    uint8_t float_enabled;
    uint8_t reserved[2];
    float max_seconds;       /* longest single dispense */
    float button_dose_s;     /* seconds dispensed by a long press of the user button */
    int32_t batt_min_mv, batt_max_mv;
    int32_t batt_low_pct, batt_crit_pct, batt_floor_pct;
} dbcfg_t;

#define EVT_DATA_MAX 120
typedef struct { uint32_t seq; time_t ts; bool time_valid; char type[24]; char data[EVT_DATA_MAX]; } dbevent_t;

void store_init(void);
dbcfg_t *store_cfg(void);
db_bottle_t *store_bottle(void);
db_schedule_t *store_schedule(void);
void store_save_cfg(void);
void store_save_bottle(void);
void store_save_schedule(void);
time_t store_last_epoch(void);
void store_save_epoch(time_t t);
void store_factory_reset(void);            /* erases app NVS data and Wi-Fi config */
const char *store_device_id(void);         /* drain-brain-XXXXXX from the MAC */

/* Event log (RAM ring, 100 entries). `data` is a JSON object string like {"a":1}. */
uint32_t events_add(const char *type, const char *data_json);
int events_since(uint32_t since_seq, dbevent_t *out, int max);   /* oldest first */
uint32_t events_last_seq(void);

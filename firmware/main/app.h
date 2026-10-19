#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <time.h>
#include "esp_err.h"

typedef struct {
    bool ext_power, flooded, float_latched, bottle_present, dispensing, time_valid;
    int batt_pct, batt_pack_mv, batt_level;     /* batt_pct -1 until first sample */
    int64_t uptime_s;
    time_t now;
} app_state_t;

void app_get_state(app_state_t *s);
/* Exactly one of ml / seconds > 0. On error returns non-OK and fills a short machine code in `code`. */
esp_err_t app_dispense(float ml, float seconds, const char *source, char *code, size_t code_len);
void app_cancel(void);
bool app_float_clear(void);                      /* false if the sensor is still wet */
void app_schedule_changed(void);
void app_apply_tz(void);
typedef struct { uint32_t id; float ml, seconds, actual_seconds; bool done; char source[12]; char outcome[16]; } app_job_t;
bool app_job(uint32_t id, app_job_t *out);

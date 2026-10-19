#include "app.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "api.h"
#include "board.h"
#include "dbhal.h"
#include "net.h"
#include "notify.h"
#include "store.h"

static const char *TAG = "app";

/* ---- shared state, guarded by s_lock (api handlers run on the httpd task) ---- */
static SemaphoreHandle_t s_lock;
static app_state_t s_st = { .batt_pct = -1 };
static bool s_float_latched;
static uint32_t s_job_seq;
static app_job_t s_job;
static bool s_job_active;
static float s_job_start_flow;
static db_bottle_state_t s_last_bstate = DB_BOTTLE_MISSING;
static volatile bool s_sched_dirty = true;
static time_t s_outage_start;

#define LOCK() xSemaphoreTake(s_lock, portMAX_DELAY)
#define UNLOCK() xSemaphoreGive(s_lock)

static bool time_valid(void) { return time(NULL) > 1700000000; }

void app_apply_tz(void)
{
    setenv("TZ", store_cfg()->tz[0] ? store_cfg()->tz : "UTC0", 1);
    tzset();
}

void app_get_state(app_state_t *s)
{
    LOCK();
    *s = s_st;
    s->float_latched = s_float_latched;
    UNLOCK();
    s->dispensing = act_busy();
    s->now = time(NULL);
    s->time_valid = time_valid();
    s->uptime_s = esp_timer_get_time() / 1000000;
}

void app_schedule_changed(void) { s_sched_dirty = true; }

static void evt(const char *type, const char *fmt, ...)
{
    char data[EVT_DATA_MAX] = "{}";
    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vsnprintf(data, sizeof data, fmt, ap);
        va_end(ap);
    }
    events_add(type, data);
}

/* ---- dispense ---- */
static const char *blocked_reason(bool allow_empty)
{
    db_bottle_t *b = store_bottle();
    if (act_busy()) return "busy";
    if (!b->present) return "bottle_missing";
    if (s_float_latched && store_cfg()->float_enabled) return "flood_latched";
    if (!s_st.ext_power && s_st.batt_pct >= 0 && s_st.batt_pct <= store_cfg()->batt_floor_pct) return "battery_low";
    if (!allow_empty && db_bottle_state(b) == DB_BOTTLE_EMPTY) return "bottle_empty";
    return NULL;
}

esp_err_t app_dispense(float ml, float seconds, const char *source, char *code, size_t code_len)
{
    LOCK();
    const char *why = blocked_reason(false);
    db_bottle_t *b = store_bottle();
    if (!why && (ml > 0) == (seconds > 0)) why = "bad_amount";
    if (!why) {
        if (ml > 0) seconds = db_bottle_ml_to_seconds(b, ml);
        else ml = db_bottle_seconds_to_ml(b, seconds);
        if (seconds <= 0) why = "bad_amount";
        else if (seconds > store_cfg()->max_seconds) why = "too_long";
    }
    if (why) {
        snprintf(code, code_len, "%s", why);
        UNLOCK();
        evt("dispense_skipped", "{\"reason\":\"%s\",\"source\":\"%s\"}", why, source);
        return ESP_FAIL;
    }
    esp_err_t e = act_start(seconds);
    if (e != ESP_OK) {
        snprintf(code, code_len, "%s", e == ESP_ERR_INVALID_STATE ? "busy" : "too_long");
        UNLOCK();
        return ESP_FAIL;
    }
    memset(&s_job, 0, sizeof s_job);
    s_job.id = ++s_job_seq;
    s_job.ml = ml;
    s_job.seconds = seconds;
    strlcpy(s_job.source, source, sizeof s_job.source);
    s_job_active = true;
    s_job_start_flow = db_bottle_flow_now(b);
    snprintf(code, code_len, "%u", (unsigned)s_job.id);
    UNLOCK();
    evt("dispense_started", "{\"job\":%u,\"seconds\":%.2f,\"ml\":%.1f,\"source\":\"%s\"}", (unsigned)s_job.id, seconds, ml, source);
    return ESP_OK;
}

void app_cancel(void) { act_cancel(); }

bool app_job(uint32_t id, app_job_t *out)
{
    LOCK();
    bool ok = s_job.id == id && id != 0;
    if (ok) *out = s_job;
    UNLOCK();
    return ok;
}

static void job_poll(void)
{
    if (!s_job_active || act_busy()) return;
    LOCK();
    s_job_active = false;
    s_job.done = true;
    s_job.actual_seconds = act_last_seconds();
    bool cancelled = s_job.actual_seconds < s_job.seconds - 0.5f;
    strlcpy(s_job.outcome, cancelled ? "cancelled" : "ok", sizeof s_job.outcome);
    float used_ml = s_job.actual_seconds * s_job_start_flow;
    db_bottle_t *b = store_bottle();
    db_bottle_consume(b, used_ml);
    store_save_bottle();
    app_job_t j = s_job;
    UNLOCK();
    evt("dispense_done", "{\"job\":%u,\"seconds\":%.2f,\"ml\":%.1f,\"outcome\":\"%s\"}", (unsigned)j.id, j.actual_seconds, used_ml, j.outcome);
}

static void bottle_state_check(void)
{
    db_bottle_state_t s = db_bottle_state(store_bottle());
    if (s == s_last_bstate) return;
    db_bottle_state_t prev = s_last_bstate;
    s_last_bstate = s;
    if (s == DB_BOTTLE_LOW && prev != DB_BOTTLE_EMPTY) evt("bottle_low", "{\"percent\":%.0f}", db_bottle_percent(store_bottle()));
    if (s == DB_BOTTLE_EMPTY) evt("bottle_empty", NULL);
}

bool app_float_clear(void)
{
    if (dbh_inputs().flooded) return false;
    LOCK();
    s_float_latched = false;
    UNLOCK();
    evt("float_cleared", NULL);
    return true;
}

/* ---- scheduler ---- */
static time_t s_cursor;
static db_sched_run_t s_next;
static bool s_have_next;

static void recompute_next(void)
{
    s_have_next = db_sched_next(store_schedule(), s_cursor, &s_next);
    s_sched_dirty = false;
}

static void scheduler_tick(void)
{
    if (!time_valid()) return;
    time_t now = time(NULL);
    if (s_cursor == 0) {
        time_t last = store_last_epoch();
        s_cursor = (last > 1700000000 && last < now) ? last : now;
        s_sched_dirty = true;
    }
    if (s_sched_dirty) recompute_next();
    if (!s_have_next || s_next.when > now) return;
    long late = (long)(now - s_next.when);
    db_sched_run_t run = s_next;
    s_cursor = now;                      /* all other missed entries are dropped; at most one catch-up */
    recompute_next();
    if (late > 120 && store_schedule()->missed_policy == DB_MISSED_SKIP) {
        evt("dispense_skipped", "{\"reason\":\"missed\",\"entry\":%u,\"late_s\":%ld}", run.entry_id, late);
        return;
    }
    char code[16];
    app_dispense(run.ml, run.seconds, "schedule", code, sizeof code);
}

/* ---- battery ---- */
static void battery_tick(void)
{
    if (act_busy()) return;
    int pin = dbh_battery_pin_mv();
    if (pin < 0) return;
    dbcfg_t *c = store_cfg();
    int pack = db_batt_pack_mv(pin, BATT_FACTOR_X100);
    int pct = db_batt_percent(pack, c->batt_min_mv, c->batt_max_mv);
    LOCK();
    db_batt_level_t prev = s_st.batt_level;
    db_batt_level_t lvl = db_batt_level(prev, pct, c->batt_low_pct, c->batt_crit_pct);
    s_st.batt_pack_mv = pack;
    s_st.batt_pct = pct;
    s_st.batt_level = lvl;
    bool on_batt = !s_st.ext_power;
    UNLOCK();
    if (on_batt && lvl != prev) {
        if (lvl == DB_BATT_CRITICAL) evt("battery_critical", "{\"percent\":%d,\"pack_mv\":%d}", pct, pack);
        else if (lvl == DB_BATT_LOW) evt("battery_low", "{\"percent\":%d,\"pack_mv\":%d}", pct, pack);
    }
}

/* ---- LEDs ---- */
static void led_tick(void)
{
    app_state_t s;
    app_get_state(&s);
    dbh_led(0, s.dispensing ? LED_FAST : LED_ON);                                   /* LED1 green */
    dbh_led(1, net_connected() ? LED_ON : (net_provisioning() ? LED_FAST : LED_SLOW)); /* LED2 blue */
    dbh_led(2, s.ext_power ? LED_ON : LED_OFF);                                    /* LED3 amber: off on battery */
    led_mode_t red = LED_OFF;
    if (s.float_latched) red = LED_SLOW;
    else if (!s.bottle_present) red = LED_FAST;
    dbh_led(3, red);                                                               /* LED4 red */
}

/* ---- input events ---- */
static int64_t s_user_down_ms, s_reset_down_ms;
static bool s_user_fired;

static void handle_event(const dbh_event_t *e)
{
    switch (e->in) {
    case DBH_IN_USER:
        s_user_down_ms = e->state ? e->ts_ms : 0;
        s_user_fired = false;
        break;
    case DBH_IN_RESET:
        if (e->state) s_reset_down_ms = e->ts_ms;
        else if (s_reset_down_ms && e->ts_ms - s_reset_down_ms < 5000) { ESP_LOGW(TAG, "reset button: reboot"); esp_restart(); }
        else s_reset_down_ms = 0;
        break;
    case DBH_IN_BOTTLE:
        LOCK(); store_bottle()->present = e->state; s_st.bottle_present = e->state; UNLOCK();
        evt(e->state ? "bottle_inserted" : "bottle_removed", NULL);
        bottle_state_check();
        break;
    case DBH_IN_FLOAT:
        if (e->state) {
            LOCK(); s_st.flooded = true; if (store_cfg()->float_enabled) s_float_latched = true; UNLOCK();
            if (store_cfg()->float_enabled) { act_cancel(); evt("float_alarm", NULL); }
        } else {
            LOCK(); s_st.flooded = false; UNLOCK();
            evt("float_dry", NULL);
        }
        break;
    case DBH_IN_POWER: {
        LOCK(); s_st.ext_power = e->state; UNLOCK();
        if (!e->state) {
            s_outage_start = time(NULL);
            evt("power_lost", NULL);
        } else {
            long dur = s_outage_start && time_valid() ? (long)(time(NULL) - s_outage_start) : -1;
            evt("power_restored", "{\"outage_s\":%ld}", dur);
            s_outage_start = 0;
        }
        break;
    }
    default: break;
    }
}

static void held_buttons_tick(void)
{
    int64_t now = esp_timer_get_time() / 1000;
    if (s_user_down_ms && !s_user_fired && now - s_user_down_ms >= 1000) {
        s_user_fired = true;
        char code[16];
        app_dispense(0, store_cfg()->button_dose_s, "button", code, sizeof code);
    }
    if (s_reset_down_ms && now - s_reset_down_ms >= 5000) {
        ESP_LOGW(TAG, "reset button held: factory reset");
        store_factory_reset();
        esp_restart();
    }
}

void app_main(void)
{
    s_lock = xSemaphoreCreateMutex();
    dbh_init();                       /* drive pins low first */
    store_init();
    dbcfg_t *c = store_cfg();
    act_set_mode(c->act_mode);
    act_set_max_seconds(c->max_seconds);
    act_boot_safe();
    app_apply_tz();
    dbh_inputs_t in = dbh_inputs();
    s_st.ext_power = in.ext_power;
    s_st.bottle_present = in.bottle_present;
    s_st.flooded = in.flooded;
    store_bottle()->present = in.bottle_present;
    s_float_latched = in.flooded && c->float_enabled;
    s_last_bstate = db_bottle_state(store_bottle());
    notify_start();
    net_start();
    api_start();
    esp_ota_mark_app_valid_cancel_rollback();
    evt("boot", "{\"reason\":%d}", (int)esp_reset_reason());

    QueueHandle_t q = dbh_events();
    int tick = 0;
    time_t last_epoch_save = 0;
    for (;; tick++) {
        dbh_event_t e;
        while (xQueueReceive(q, &e, tick == 0 ? 0 : pdMS_TO_TICKS(100)) == pdTRUE) handle_event(&e);
        vTaskDelay(1);
        job_poll();
        held_buttons_tick();
        if (tick % 5 == 0) led_tick();
        if (tick % 10 == 0) scheduler_tick();
        if (tick % 100 == 0) battery_tick();
        if (time_valid() && time(NULL) - last_epoch_save > 1800) { last_epoch_save = time(NULL); store_save_epoch(last_epoch_save); }
        if (tick % 50 == 0) bottle_state_check();
    }
}

#include "api.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app.h"
#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "dbhal.h"
#include "net.h"
#include "store.h"

static const char *TAG = "api";
extern const char index_html_start[] asm("_binary_index_html_start");
extern const char index_html_end[] asm("_binary_index_html_end");

#define V1 "/api/v1"
static const char *DAYS[7] = {"sun", "mon", "tue", "wed", "thu", "fri", "sat"};
static const char *TYPES[4] = {"weekly", "interval", "monthly", "once"};

/* ---------------- helpers ---------------- */
static esp_err_t send_json(httpd_req_t *r, const char *status, cJSON *o)
{
    char *s = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    httpd_resp_set_status(r, status);
    httpd_resp_set_type(r, "application/json");
    esp_err_t e = httpd_resp_sendstr(r, s ? s : "{}");
    free(s);
    return e;
}

static esp_err_t send_err(httpd_req_t *r, const char *status, const char *code, const char *msg)
{
    cJSON *o = cJSON_CreateObject(), *e = cJSON_AddObjectToObject(o, "error");
    cJSON_AddStringToObject(e, "code", code);
    cJSON_AddStringToObject(e, "message", msg ? msg : code);
    return send_json(r, status, o);
}

static bool authed(httpd_req_t *r)
{
    const char *tok = store_cfg()->token;
    if (!tok[0]) return false;
    char h[128];
    if (httpd_req_get_hdr_value_str(r, "Authorization", h, sizeof h) != ESP_OK || strncmp(h, "Bearer ", 7) != 0) return false;
    const char *p = h + 7;
    size_t n = strlen(tok);
    if (strlen(p) != n) return false;
    unsigned char diff = 0;
    for (size_t i = 0; i < n; i++) diff |= (unsigned char)(p[i] ^ tok[i]);
    return diff == 0;
}
#define REQUIRE_AUTH(r) do { if (!authed(r)) return send_err(r, "401 Unauthorized", "unauthorized", "missing or bad bearer token"); } while (0)

static cJSON *read_json(httpd_req_t *r)
{
    int len = r->content_len;
    if (len <= 0 || len > 4096) return NULL;
    char *buf = malloc(len + 1);
    if (!buf) return NULL;
    int got = 0;
    while (got < len) {
        int n = httpd_req_recv(r, buf + got, len - got);
        if (n <= 0) { free(buf); return NULL; }
        got += n;
    }
    buf[len] = 0;
    cJSON *j = cJSON_Parse(buf);
    free(buf);
    return j;
}

static double jnum(const cJSON *o, const char *k, double def)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsNumber(v) ? v->valuedouble : def;
}
static const char *jstr(const cJSON *o, const char *k)
{
    const cJSON *v = cJSON_GetObjectItemCaseSensitive(o, k);
    return cJSON_IsString(v) ? v->valuestring : NULL;
}

/* ---------------- schedule <-> JSON ---------------- */
static void date_str(db_date_t d, char *out, size_t n) { snprintf(out, n, "%04u-%02u-%02u", d.y, d.m, d.d); }
static bool parse_date(const char *s, db_date_t *d)
{
    unsigned y, m, dd;
    if (!s || sscanf(s, "%4u-%2u-%2u", &y, &m, &dd) != 3 || m < 1 || m > 12 || dd < 1 || dd > 31) return false;
    *d = (db_date_t){ y, m, dd };
    return true;
}

static cJSON *schedule_to_json(const db_schedule_t *s)
{
    cJSON *o = cJSON_CreateObject(), *arr = cJSON_AddArrayToObject(o, "entries");
    char t[16];
    for (int i = 0; i < s->n_entries; i++) {
        const db_sched_entry_t *e = &s->entries[i];
        cJSON *x = cJSON_CreateObject();
        cJSON_AddNumberToObject(x, "id", e->id);
        cJSON_AddBoolToObject(x, "enabled", e->enabled);
        cJSON_AddStringToObject(x, "type", TYPES[e->type]);
        if (e->type == DB_SCHED_WEEKLY) {
            cJSON *d = cJSON_AddArrayToObject(x, "days");
            for (int k = 0; k < 7; k++) if ((e->days_mask >> k) & 1) cJSON_AddItemToArray(d, cJSON_CreateString(DAYS[k]));
        } else if (e->type == DB_SCHED_INTERVAL) {
            cJSON_AddNumberToObject(x, "every_days", e->every_days);
            date_str(e->anchor, t, sizeof t);
            cJSON_AddStringToObject(x, "anchor", t);
        } else if (e->type == DB_SCHED_MONTHLY) {
            cJSON_AddNumberToObject(x, "day_of_month", e->day_of_month);
        } else {
            date_str(e->date, t, sizeof t);
            cJSON_AddStringToObject(x, "date", t);
        }
        snprintf(t, sizeof t, "%02u:%02u", e->hour, e->minute);
        cJSON_AddStringToObject(x, "time", t);
        cJSON *a = cJSON_AddObjectToObject(x, "amount");
        if (e->ml > 0) cJSON_AddNumberToObject(a, "ml", e->ml); else cJSON_AddNumberToObject(a, "seconds", e->seconds);
        cJSON_AddItemToArray(arr, x);
    }
    cJSON_AddStringToObject(o, "missed_policy", s->missed_policy == DB_MISSED_SKIP ? "skip" : "run_once_on_boot");
    cJSON *b = cJSON_AddArrayToObject(o, "blackout");
    for (int i = 0; i < s->n_blackout; i++) {
        cJSON *x = cJSON_CreateObject();
        date_str(s->blackout[i].from, t, sizeof t); cJSON_AddStringToObject(x, "from", t);
        date_str(s->blackout[i].to, t, sizeof t); cJSON_AddStringToObject(x, "to", t);
        cJSON_AddItemToArray(b, x);
    }
    return o;
}

/* Returns NULL on success, else an error message. */
static const char *schedule_from_json(const cJSON *o, db_schedule_t *s)
{
    memset(s, 0, sizeof *s);
    const cJSON *arr = cJSON_GetObjectItemCaseSensitive(o, "entries");
    if (!cJSON_IsArray(arr)) return "entries must be an array";
    if (cJSON_GetArraySize(arr) > DB_SCHED_MAX_ENTRIES) return "too many entries";
    const cJSON *x;
    cJSON_ArrayForEach(x, arr) {
        db_sched_entry_t *e = &s->entries[s->n_entries++];
        e->id = (uint16_t)jnum(x, "id", s->n_entries);
        const cJSON *en = cJSON_GetObjectItemCaseSensitive(x, "enabled");
        e->enabled = en ? cJSON_IsTrue(en) : true;
        const char *ty = jstr(x, "type");
        int ti = -1;
        for (int k = 0; k < 4 && ty; k++) if (!strcmp(ty, TYPES[k])) ti = k;
        if (ti < 0) return "bad entry type";
        e->type = ti;
        unsigned hh, mm;
        const char *tm = jstr(x, "time");
        if (!tm || sscanf(tm, "%2u:%2u", &hh, &mm) != 2) return "bad time (HH:MM)";
        e->hour = hh; e->minute = mm;
        if (ti == DB_SCHED_WEEKLY) {
            const cJSON *d, *days = cJSON_GetObjectItemCaseSensitive(x, "days");
            cJSON_ArrayForEach(d, days) for (int k = 0; k < 7; k++) if (cJSON_IsString(d) && !strcmp(d->valuestring, DAYS[k])) e->days_mask |= 1 << k;
        } else if (ti == DB_SCHED_INTERVAL) {
            e->every_days = (uint16_t)jnum(x, "every_days", 0);
            if (!parse_date(jstr(x, "anchor"), &e->anchor)) {
                time_t now = time(NULL); struct tm l; localtime_r(&now, &l);
                e->anchor = (db_date_t){ l.tm_year + 1900, l.tm_mon + 1, l.tm_mday };
            }
        } else if (ti == DB_SCHED_MONTHLY) {
            e->day_of_month = (uint8_t)jnum(x, "day_of_month", 0);
        } else if (!parse_date(jstr(x, "date"), &e->date)) {
            return "once entry needs date YYYY-MM-DD";
        }
        const cJSON *a = cJSON_GetObjectItemCaseSensitive(x, "amount");
        e->ml = (float)jnum(a, "ml", 0);
        e->seconds = (float)jnum(a, "seconds", 0);
    }
    const char *mp = jstr(o, "missed_policy");
    s->missed_policy = mp && !strcmp(mp, "run_once_on_boot") ? DB_MISSED_RUN_ONCE_ON_BOOT : DB_MISSED_SKIP;
    const cJSON *b, *bl = cJSON_GetObjectItemCaseSensitive(o, "blackout");
    cJSON_ArrayForEach(b, bl) {
        if (s->n_blackout >= DB_SCHED_MAX_BLACKOUT) return "too many blackout ranges";
        db_blackout_t *r = &s->blackout[s->n_blackout++];
        if (!parse_date(jstr(b, "from"), &r->from) || !parse_date(jstr(b, "to"), &r->to)) return "bad blackout date";
    }
    return db_sched_validate(s);
}

/* ---------------- handlers ---------------- */
static const char *bottle_state_name(db_bottle_state_t s)
{
    static const char *n[] = {"missing", "full", "ok", "low", "empty"};
    return n[s];
}

static esp_err_t h_root(httpd_req_t *r)
{
    httpd_resp_set_type(r, "text/html");
    return httpd_resp_send(r, index_html_start, index_html_end - index_html_start - 1);
}

static esp_err_t h_info(httpd_req_t *r)
{
    const esp_app_desc_t *d = esp_app_get_description();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "model", "iFLO Pro (drain_brain)");
    cJSON_AddStringToObject(o, "firmware", d->version);
    cJSON_AddStringToObject(o, "device_id", store_device_id());
    cJSON_AddStringToObject(o, "name", store_cfg()->name);
    cJSON_AddBoolToObject(o, "provisioning", net_provisioning());
    cJSON_AddBoolToObject(o, "token_set", store_cfg()->token[0] != 0);
    return send_json(r, "200 OK", o);
}

static esp_err_t h_status(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    app_state_t s;
    app_get_state(&s);
    db_bottle_t *b = store_bottle();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "power", s.ext_power ? "adapter" : "battery");
    if (s.batt_pct >= 0) { cJSON_AddNumberToObject(o, "battery_pct", s.batt_pct); cJSON_AddNumberToObject(o, "battery_mv", s.batt_pack_mv); }
    cJSON_AddStringToObject(o, "battery_level", s.batt_level == 0 ? "ok" : (s.batt_level == 1 ? "low" : "critical"));
    cJSON *bo = cJSON_AddObjectToObject(o, "bottle");
    cJSON_AddBoolToObject(bo, "present", b->present);
    cJSON_AddNumberToObject(bo, "capacity_ml", b->capacity_ml);
    cJSON_AddNumberToObject(bo, "remaining_ml", b->remaining_ml);
    cJSON_AddNumberToObject(bo, "pct", db_bottle_percent(b));
    cJSON_AddStringToObject(bo, "state", bottle_state_name(db_bottle_state(b)));
    cJSON_AddStringToObject(o, "float", s.float_latched ? "alarm" : "ok");
    cJSON_AddBoolToObject(o, "float_wet_now", s.flooded);
    cJSON_AddStringToObject(o, "motor", s.dispensing ? "running" : "idle");
    cJSON_AddStringToObject(o, "mode", s.float_latched ? "flood" : (!b->present ? "no_bottle" : (!s.ext_power ? "on_battery" : "normal")));
    cJSON_AddNumberToObject(o, "uptime_s", (double)s.uptime_s);
    cJSON_AddNumberToObject(o, "rssi", net_rssi());
    char ip[16];
    net_ip(ip, sizeof ip);
    cJSON_AddStringToObject(o, "ip", ip);
    cJSON_AddBoolToObject(o, "time_valid", s.time_valid);
    cJSON_AddNumberToObject(o, "time", (double)s.now);
    cJSON_AddNumberToObject(o, "event_seq", events_last_seq());
    return send_json(r, "200 OK", o);
}

static cJSON *config_json(void)
{
    const dbcfg_t *c = store_cfg();
    const db_bottle_t *b = store_bottle();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "name", c->name);
    cJSON_AddStringToObject(o, "tz", c->tz);
    cJSON_AddStringToObject(o, "webhook", c->webhook);
    cJSON_AddStringToObject(o, "drive_mode", c->act_mode == 0 ? "pulse_valve" : "motor_continuous");
    cJSON_AddBoolToObject(o, "float_enabled", c->float_enabled);
    cJSON_AddNumberToObject(o, "max_dispense_seconds", c->max_seconds);
    cJSON_AddNumberToObject(o, "button_dose_seconds", c->button_dose_s);
    cJSON_AddNumberToObject(o, "battery_floor_pct", c->batt_floor_pct);
    cJSON_AddNumberToObject(o, "capacity_ml", b->capacity_ml);
    cJSON_AddNumberToObject(o, "flow_ml_per_s", b->flow_ml_s_100);
    cJSON_AddNumberToObject(o, "low_percent", b->low_percent);
    cJSON_AddNumberToObject(o, "empty_percent", b->empty_percent);
    return o;
}

static esp_err_t h_config_get(httpd_req_t *r) { REQUIRE_AUTH(r); return send_json(r, "200 OK", config_json()); }

static esp_err_t h_config_put(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    cJSON *j = read_json(r);
    if (!j) return send_err(r, "400 Bad Request", "bad_json", "body must be a JSON object");
    dbcfg_t *c = store_cfg();
    db_bottle_t *b = store_bottle();
    const char *s;
    if ((s = jstr(j, "name")) && strlen(s) < sizeof c->name) strcpy(c->name, s);
    if ((s = jstr(j, "tz")) && strlen(s) < sizeof c->tz) { strcpy(c->tz, s); app_apply_tz(); app_schedule_changed(); }
    if ((s = jstr(j, "webhook")) && strlen(s) < sizeof c->webhook) strcpy(c->webhook, s);
    if ((s = jstr(j, "token")) && strlen(s) >= 16 && strlen(s) < sizeof c->token) strcpy(c->token, s);
    if ((s = jstr(j, "drive_mode"))) { c->act_mode = !strcmp(s, "motor_continuous"); act_set_mode(c->act_mode); }
    const cJSON *fe = cJSON_GetObjectItemCaseSensitive(j, "float_enabled");
    if (fe) c->float_enabled = cJSON_IsTrue(fe);
    double v;
    if ((v = jnum(j, "max_dispense_seconds", 0)) >= 1 && v <= 60) { c->max_seconds = v; act_set_max_seconds(v); }
    if ((v = jnum(j, "button_dose_seconds", 0)) > 0 && v <= c->max_seconds) c->button_dose_s = v;
    if ((v = jnum(j, "battery_floor_pct", -1)) >= 0 && v <= 50) c->batt_floor_pct = v;
    if ((v = jnum(j, "capacity_ml", 0)) > 0) b->capacity_ml = v;
    if ((v = jnum(j, "flow_ml_per_s", 0)) > 0) b->flow_ml_s_100 = v;
    if ((v = jnum(j, "low_percent", 0)) > 0 && v < 100) b->low_percent = v;
    if ((v = jnum(j, "empty_percent", 0)) > 0 && v < 100) b->empty_percent = v;
    cJSON_Delete(j);
    store_save_cfg();
    store_save_bottle();
    return send_json(r, "200 OK", config_json());
}

static esp_err_t h_dispense(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    cJSON *j = read_json(r);
    if (!j) return send_err(r, "400 Bad Request", "bad_json", "body must be a JSON object");
    float ml = jnum(j, "ml", 0), sec = jnum(j, "seconds", 0);
    cJSON_Delete(j);
    char code[16];
    if (app_dispense(ml, sec, "api", code, sizeof code) != ESP_OK) {
        bool bad = !strcmp(code, "bad_amount") || !strcmp(code, "too_long");
        return send_err(r, bad ? "400 Bad Request" : "409 Conflict", code, code);
    }
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "job", atoi(code));
    return send_json(r, "202 Accepted", o);
}

static esp_err_t h_dispense_cancel(httpd_req_t *r) { REQUIRE_AUTH(r); app_cancel(); return send_json(r, "200 OK", cJSON_CreateObject()); }

static esp_err_t h_dispense_get(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    const char *p = strrchr(r->uri, '/');
    app_job_t j;
    if (!p || !app_job(atoi(p + 1), &j)) return send_err(r, "404 Not Found", "not_found", "only the most recent job is kept");
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "job", j.id);
    cJSON_AddBoolToObject(o, "done", j.done);
    cJSON_AddNumberToObject(o, "requested_ml", j.ml);
    cJSON_AddNumberToObject(o, "requested_seconds", j.seconds);
    cJSON_AddNumberToObject(o, "actual_seconds", j.actual_seconds);
    cJSON_AddStringToObject(o, "source", j.source);
    cJSON_AddStringToObject(o, "outcome", j.done ? j.outcome : "running");
    return send_json(r, "200 OK", o);
}

static esp_err_t h_schedule_get(httpd_req_t *r) { REQUIRE_AUTH(r); return send_json(r, "200 OK", schedule_to_json(store_schedule())); }

static esp_err_t h_schedule_put(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    cJSON *j = read_json(r);
    if (!j) return send_err(r, "400 Bad Request", "bad_json", "body must be a JSON object");
    db_schedule_t *tmp = malloc(sizeof *tmp);
    const char *err = tmp ? schedule_from_json(j, tmp) : "out of memory";
    cJSON_Delete(j);
    if (err) { free(tmp); return send_err(r, "400 Bad Request", "bad_schedule", err); }
    *store_schedule() = *tmp;
    free(tmp);
    store_save_schedule();
    app_schedule_changed();
    return send_json(r, "200 OK", schedule_to_json(store_schedule()));
}

static esp_err_t h_schedule_next(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    db_sched_run_t runs[10];
    int n = db_sched_upcoming(store_schedule(), time(NULL), runs, 10);
    cJSON *o = cJSON_CreateObject(), *a = cJSON_AddArrayToObject(o, "runs");
    for (int i = 0; i < n; i++) {
        cJSON *x = cJSON_CreateObject();
        char t[32];
        struct tm l;
        localtime_r(&runs[i].when, &l);
        strftime(t, sizeof t, "%Y-%m-%dT%H:%M:%S", &l);
        cJSON_AddNumberToObject(x, "epoch", (double)runs[i].when);
        cJSON_AddStringToObject(x, "local", t);
        cJSON_AddNumberToObject(x, "entry", runs[i].entry_id);
        cJSON_AddNumberToObject(x, "ml", runs[i].ml);
        cJSON_AddNumberToObject(x, "seconds", runs[i].seconds);
        cJSON_AddItemToArray(a, x);
    }
    return send_json(r, "200 OK", o);
}

static esp_err_t h_bottle_get(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    db_bottle_t *b = store_bottle();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddBoolToObject(o, "present", b->present);
    cJSON_AddStringToObject(o, "state", bottle_state_name(db_bottle_state(b)));
    cJSON_AddNumberToObject(o, "capacity_ml", b->capacity_ml);
    cJSON_AddNumberToObject(o, "remaining_ml", b->remaining_ml);
    cJSON_AddNumberToObject(o, "pct", db_bottle_percent(b));
    cJSON_AddNumberToObject(o, "flow_ml_per_s", db_bottle_flow_now(b));
    return send_json(r, "200 OK", o);
}

static esp_err_t h_bottle_reset(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    cJSON *j = r->content_len ? read_json(r) : NULL;
    float cap = j ? jnum(j, "capacity_ml", 0) : 0;
    cJSON_Delete(j);
    db_bottle_reset_full(store_bottle(), cap);
    store_save_bottle();
    events_add("bottle_reset", "{}");
    return h_bottle_get(r);
}

static esp_err_t h_calibration(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    cJSON *j = read_json(r);
    if (!j) return send_err(r, "400 Bad Request", "bad_json", "need {seconds, measured_ml}");
    bool ok = db_bottle_calibrate(store_bottle(), jnum(j, "seconds", 0), jnum(j, "measured_ml", 0));
    cJSON_Delete(j);
    if (!ok) return send_err(r, "400 Bad Request", "bad_input", "seconds and measured_ml must be > 0");
    store_save_bottle();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "flow_ml_per_s", db_bottle_flow_now(store_bottle()));
    return send_json(r, "200 OK", o);
}

static esp_err_t h_events(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    char q[48] = "", v[12];
    uint32_t since = 0;
    if (httpd_req_get_url_query_str(r, q, sizeof q) == ESP_OK && httpd_query_key_value(q, "since", v, sizeof v) == ESP_OK) since = strtoul(v, NULL, 10);
    dbevent_t *ev = malloc(sizeof(dbevent_t) * 100);
    if (!ev) return send_err(r, "500 Internal Server Error", "oom", NULL);
    int n = events_since(since, ev, 100);
    cJSON *o = cJSON_CreateObject(), *a = cJSON_AddArrayToObject(o, "events");
    for (int i = 0; i < n; i++) {
        cJSON *x = cJSON_CreateObject();
        cJSON_AddNumberToObject(x, "seq", ev[i].seq);
        cJSON_AddNumberToObject(x, "time", (double)ev[i].ts);
        cJSON_AddBoolToObject(x, "time_valid", ev[i].time_valid);
        cJSON_AddStringToObject(x, "type", ev[i].type);
        cJSON *d = cJSON_Parse(ev[i].data);
        cJSON_AddItemToObject(x, "data", d ? d : cJSON_CreateObject());
        cJSON_AddItemToArray(a, x);
    }
    free(ev);
    return send_json(r, "200 OK", o);
}

static esp_err_t h_float_clear(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    if (!app_float_clear()) return send_err(r, "409 Conflict", "still_wet", "float sensor is still wet");
    return send_json(r, "200 OK", cJSON_CreateObject());
}

/* First-time setup while the provisioning AP is up: sets Wi-Fi, token, name, tz. */
static esp_err_t h_provision(httpd_req_t *r)
{
    if (!net_provisioning() && !authed(r)) return send_err(r, "403 Forbidden", "not_provisioning", "hold the reset button 5 s to re-enter setup");
    cJSON *j = read_json(r);
    const char *ssid = j ? jstr(j, "ssid") : NULL, *pw = j ? jstr(j, "password") : NULL, *tok = j ? jstr(j, "token") : NULL;
    if (!ssid || !ssid[0] || !pw || !tok || strlen(tok) < 16) {
        cJSON_Delete(j);
        return send_err(r, "400 Bad Request", "bad_input", "need ssid, password and token (16+ chars)");
    }
    dbcfg_t *c = store_cfg();
    strlcpy(c->token, tok, sizeof c->token);
    const char *s;
    if ((s = jstr(j, "name")) && s[0]) strlcpy(c->name, s, sizeof c->name);
    if ((s = jstr(j, "tz")) && s[0]) strlcpy(c->tz, s, sizeof c->tz);
    net_set_credentials(ssid, pw);
    cJSON_Delete(j);
    store_save_cfg();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "saved; rebooting and joining Wi-Fi");
    cJSON_AddStringToObject(o, "device_id", store_device_id());
    esp_err_t e = send_json(r, "200 OK", o);
    vTaskDelay(pdMS_TO_TICKS(1500));
    esp_restart();
    return e;
}

static esp_err_t h_wifi_put(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    cJSON *j = read_json(r);
    const char *ssid = j ? jstr(j, "ssid") : NULL, *pw = j ? jstr(j, "password") : NULL;
    if (!ssid || !ssid[0] || !pw) { cJSON_Delete(j); return send_err(r, "400 Bad Request", "bad_input", "need ssid and password"); }
    net_set_credentials(ssid, pw);
    cJSON_Delete(j);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "status", "saved; reboot to apply");
    return send_json(r, "200 OK", o);
}

static void restart_later(void *arg) { vTaskDelay(pdMS_TO_TICKS(1000)); if (arg) store_factory_reset(); esp_restart(); }
static esp_err_t h_reboot(httpd_req_t *r) { REQUIRE_AUTH(r); xTaskCreate(restart_later, "rb", 3072, NULL, 5, NULL); return send_json(r, "200 OK", cJSON_CreateObject()); }
static esp_err_t h_factory(httpd_req_t *r) { REQUIRE_AUTH(r); xTaskCreate(restart_later, "fr", 3072, (void *)1, 5, NULL); return send_json(r, "200 OK", cJSON_CreateObject()); }

static esp_err_t h_ota(httpd_req_t *r)
{
    REQUIRE_AUTH(r);
    const esp_partition_t *p = esp_ota_get_next_update_partition(NULL);
    esp_ota_handle_t h;
    if (!p || esp_ota_begin(p, OTA_WITH_SEQUENTIAL_WRITES, &h) != ESP_OK) return send_err(r, "500 Internal Server Error", "ota_begin", NULL);
    char *buf = malloc(2048);
    int remaining = r->content_len;
    bool ok = buf != NULL;
    while (ok && remaining > 0) {
        int n = httpd_req_recv(r, buf, remaining < 2048 ? remaining : 2048);
        if (n == HTTPD_SOCK_ERR_TIMEOUT) continue;
        if (n <= 0 || esp_ota_write(h, buf, n) != ESP_OK) ok = false;
        else remaining -= n;
    }
    free(buf);
    if (!ok || esp_ota_end(h) != ESP_OK || esp_ota_set_boot_partition(p) != ESP_OK) return send_err(r, "400 Bad Request", "ota_failed", "image rejected");
    esp_err_t e = send_json(r, "200 OK", cJSON_CreateObject());
    xTaskCreate(restart_later, "ota", 3072, NULL, 5, NULL);
    return e;
}

void api_start(void)
{
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.max_uri_handlers = 32;
    cfg.stack_size = 8192;
    cfg.lru_purge_enable = true;
    cfg.uri_match_fn = httpd_uri_match_wildcard;
    httpd_handle_t srv;
    ESP_ERROR_CHECK(httpd_start(&srv, &cfg));
    static const struct { const char *uri; httpd_method_t m; esp_err_t (*h)(httpd_req_t *); } T[] = {
        {"/", HTTP_GET, h_root},
        {V1 "/info", HTTP_GET, h_info},
        {V1 "/status", HTTP_GET, h_status},
        {V1 "/config", HTTP_GET, h_config_get},
        {V1 "/config", HTTP_PUT, h_config_put},
        {V1 "/dispense", HTTP_POST, h_dispense},
        {V1 "/dispense/cancel", HTTP_POST, h_dispense_cancel},
        {V1 "/dispense/*", HTTP_GET, h_dispense_get},
        {V1 "/schedule", HTTP_GET, h_schedule_get},
        {V1 "/schedule", HTTP_PUT, h_schedule_put},
        {V1 "/schedule/next", HTTP_GET, h_schedule_next},
        {V1 "/bottle", HTTP_GET, h_bottle_get},
        {V1 "/bottle/reset", HTTP_POST, h_bottle_reset},
        {V1 "/calibration", HTTP_POST, h_calibration},
        {V1 "/events", HTTP_GET, h_events},
        {V1 "/float/clear", HTTP_POST, h_float_clear},
        {V1 "/provision", HTTP_POST, h_provision},
        {V1 "/wifi", HTTP_PUT, h_wifi_put},
        {V1 "/reboot", HTTP_POST, h_reboot},
        {V1 "/factory-reset", HTTP_POST, h_factory},
        {V1 "/ota", HTTP_POST, h_ota},
    };
    for (size_t i = 0; i < sizeof T / sizeof T[0]; i++) {
        httpd_uri_t u = { .uri = T[i].uri, .method = T[i].m, .handler = T[i].h };
        ESP_ERROR_CHECK(httpd_register_uri_handler(srv, &u));
    }
    ESP_LOGI(TAG, "http server up, %d routes", (int)(sizeof T / sizeof T[0]));
}

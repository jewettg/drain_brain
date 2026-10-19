#include "store.h"
#include <string.h>
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "dbhal.h"
#include "notify.h"

static const char *TAG = "store";
#define NS "drainbrain"

static dbcfg_t s_cfg;
static db_bottle_t s_bottle;
static db_schedule_t s_sched;
static char s_id[24];

#define EV_RING 100
static dbevent_t s_ev[EV_RING];
static uint32_t s_seq;
static SemaphoreHandle_t s_ev_lock;

static void cfg_defaults(void)
{
    memset(&s_cfg, 0, sizeof s_cfg);
    s_cfg.version = CFG_VERSION;
    strcpy(s_cfg.name, "drain_brain");
    strcpy(s_cfg.tz, "UTC0");
    s_cfg.act_mode = ACT_PULSE_VALVE;
    s_cfg.float_enabled = 1;
    s_cfg.max_seconds = 30;
    s_cfg.button_dose_s = 5;
    s_cfg.batt_min_mv = 2200;
    s_cfg.batt_max_mv = 3000;
    s_cfg.batt_low_pct = 20;
    s_cfg.batt_crit_pct = 10;
    s_cfg.batt_floor_pct = 10;
}

static bool load_blob(const char *key, void *dst, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) != ESP_OK) return false;
    size_t n = len;
    esp_err_t e = nvs_get_blob(h, key, dst, &n);
    nvs_close(h);
    return e == ESP_OK && n == len;
}

static void save_blob(const char *key, const void *src, size_t len)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_blob(h, key, src, len);
    nvs_commit(h);
    nvs_close(h);
}

void store_init(void)
{
    esp_err_t e = nvs_flash_init();
    if (e == ESP_ERR_NVS_NO_FREE_PAGES || e == ESP_ERR_NVS_NEW_VERSION_FOUND) { nvs_flash_erase(); e = nvs_flash_init(); }
    ESP_ERROR_CHECK(e);
    s_ev_lock = xSemaphoreCreateMutex();
    cfg_defaults();
    dbcfg_t tmp;
    if (load_blob("cfg", &tmp, sizeof tmp) && tmp.version == CFG_VERSION) s_cfg = tmp;
    db_bottle_defaults(&s_bottle);
    load_blob("bottle", &s_bottle, sizeof s_bottle);
    s_bottle.present = false;                       /* live state comes from the switch */
    memset(&s_sched, 0, sizeof s_sched);
    db_schedule_t st;
    if (load_blob("sched", &st, sizeof st) && db_sched_validate(&st) == NULL) s_sched = st;
    uint8_t mac[6];
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_id, sizeof s_id, "drain-brain-%02x%02x%02x", mac[3], mac[4], mac[5]);
    ESP_LOGI(TAG, "device id %s", s_id);
}

dbcfg_t *store_cfg(void) { return &s_cfg; }
db_bottle_t *store_bottle(void) { return &s_bottle; }
db_schedule_t *store_schedule(void) { return &s_sched; }
void store_save_cfg(void) { save_blob("cfg", &s_cfg, sizeof s_cfg); }
void store_save_bottle(void) { save_blob("bottle", &s_bottle, sizeof s_bottle); }
void store_save_schedule(void) { save_blob("sched", &s_sched, sizeof s_sched); }
const char *store_device_id(void) { return s_id; }

time_t store_last_epoch(void)
{
    int64_t v = 0;
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READONLY, &h) == ESP_OK) { nvs_get_i64(h, "epoch", &v); nvs_close(h); }
    return (time_t)v;
}

void store_save_epoch(time_t t)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_i64(h, "epoch", (int64_t)t);
    nvs_commit(h);
    nvs_close(h);
}

void store_factory_reset(void)
{
    nvs_handle_t h;
    if (nvs_open(NS, NVS_READWRITE, &h) == ESP_OK) { nvs_erase_all(h); nvs_commit(h); nvs_close(h); }
    esp_wifi_restore();
}

uint32_t events_add(const char *type, const char *data_json)
{
    xSemaphoreTake(s_ev_lock, portMAX_DELAY);
    dbevent_t *e = &s_ev[s_seq % EV_RING];
    memset(e, 0, sizeof *e);
    e->seq = ++s_seq;
    e->ts = time(NULL);
    e->time_valid = e->ts > 1700000000;
    strlcpy(e->type, type, sizeof e->type);
    strlcpy(e->data, data_json ? data_json : "{}", sizeof e->data);
    dbevent_t copy = *e;
    uint32_t seq = e->seq;
    xSemaphoreGive(s_ev_lock);
    notify_push(&copy);
    return seq;
}

uint32_t events_last_seq(void) { return s_seq; }

int events_since(uint32_t since_seq, dbevent_t *out, int max)
{
    int n = 0;
    xSemaphoreTake(s_ev_lock, portMAX_DELAY);
    uint32_t first = s_seq > EV_RING ? s_seq - EV_RING + 1 : 1;
    uint32_t from = since_seq + 1 > first ? since_seq + 1 : first;
    for (uint32_t q = from; q <= s_seq && n < max; q++) out[n++] = s_ev[(q - 1) % EV_RING];
    xSemaphoreGive(s_ev_lock);
    return n;
}

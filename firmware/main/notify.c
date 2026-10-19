#include "notify.h"
#include <stdio.h>
#include <string.h>
#include "cJSON.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "mbedtls/md.h"
#include "net.h"

static const char *TAG = "notify";
static QueueHandle_t s_q;

static void hmac_hex(const char *key, const char *msg, char out[65])
{
    unsigned char mac[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256), (const unsigned char *)key, strlen(key),
                    (const unsigned char *)msg, strlen(msg), mac);
    for (int i = 0; i < 32; i++) sprintf(out + 2 * i, "%02x", mac[i]);
}

static bool post_event(const dbevent_t *e)
{
    const dbcfg_t *c = store_cfg();
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "device_id", store_device_id());
    cJSON_AddNumberToObject(o, "seq", e->seq);
    cJSON_AddNumberToObject(o, "time", (double)e->ts);
    cJSON_AddBoolToObject(o, "time_valid", e->time_valid);
    cJSON_AddStringToObject(o, "type", e->type);
    cJSON *d = cJSON_Parse(e->data);
    cJSON_AddItemToObject(o, "data", d ? d : cJSON_CreateObject());
    char *body = cJSON_PrintUnformatted(o);
    cJSON_Delete(o);
    char sig[65];
    hmac_hex(c->token, body, sig);

    esp_http_client_config_t cfg = { .url = c->webhook, .method = HTTP_METHOD_POST, .timeout_ms = 5000 };
    esp_http_client_handle_t h = esp_http_client_init(&cfg);
    esp_http_client_set_header(h, "Content-Type", "application/json");
    esp_http_client_set_header(h, "X-DrainBrain-Signature", sig);
    esp_http_client_set_post_field(h, body, strlen(body));
    bool ok = esp_http_client_perform(h) == ESP_OK && esp_http_client_get_status_code(h) / 100 == 2;
    esp_http_client_cleanup(h);
    free(body);
    return ok;
}

static void notify_task(void *arg)
{
    dbevent_t e;
    for (;;) {
        if (xQueueReceive(s_q, &e, portMAX_DELAY) != pdTRUE) continue;
        if (!store_cfg()->webhook[0] || !store_cfg()->token[0]) continue;
        for (int attempt = 0, delay = 2; attempt < 4; attempt++, delay *= 2) {
            if (net_connected() && post_event(&e)) break;
            ESP_LOGW(TAG, "webhook seq %u attempt %d failed", (unsigned)e.seq, attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(delay * 1000));
        }
    }
}

void notify_push(const dbevent_t *e) { if (s_q) xQueueSend(s_q, e, 0); }

void notify_start(void)
{
    s_q = xQueueCreate(16, sizeof(dbevent_t));
    xTaskCreate(notify_task, "notify", 6144, NULL, 4, NULL);
}

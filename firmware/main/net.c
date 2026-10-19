#include "net.h"
#include <string.h>
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_sntp.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "store.h"

static const char *TAG = "net";
#define AP_PASS "drainbrain"        /* provisioning AP only; documented in the README */
#define MAX_RETRY 10

static volatile bool s_connected, s_prov;
static int s_retry;
static esp_netif_t *s_sta, *s_ap;
static char s_ip[16];

static void start_ap(void)
{
    if (s_prov) return;
    s_prov = true;
    if (!s_ap) s_ap = esp_netif_create_default_wifi_ap();
    wifi_config_t ap = { 0 };
    snprintf((char *)ap.ap.ssid, sizeof ap.ap.ssid, "%s", store_device_id());
    ap.ap.ssid_len = strlen((char *)ap.ap.ssid);
    strcpy((char *)ap.ap.password, AP_PASS);
    ap.ap.authmode = WIFI_AUTH_WPA2_PSK;
    ap.ap.max_connection = 2;
    esp_wifi_set_mode(WIFI_MODE_APSTA);
    esp_wifi_set_config(WIFI_IF_AP, &ap);
    ESP_LOGW(TAG, "provisioning AP '%s' password '%s' -> http://192.168.4.1/", ap.ap.ssid, AP_PASS);
}

static void on_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        if (s_retry < MAX_RETRY) s_retry++; else start_ap();   /* keep retrying in the background, AP stays up */
        vTaskDelay(pdMS_TO_TICKS(s_retry < 5 ? 1000 : 10000));
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *e = data;
        snprintf(s_ip, sizeof s_ip, IPSTR, IP2STR(&e->ip_info.ip));
        s_connected = true;
        s_retry = 0;
        if (s_prov) { s_prov = false; esp_wifi_set_mode(WIFI_MODE_STA); }
        ESP_LOGI(TAG, "got ip %s", s_ip);
        static bool sntp_started;
        if (!sntp_started) {
            sntp_started = true;
            esp_sntp_config_t sc = ESP_NETIF_SNTP_DEFAULT_CONFIG("pool.ntp.org");
            esp_netif_sntp_init(&sc);
        }
    }
}

void net_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(s_sta, store_device_id());
    wifi_init_config_t ic = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&ic));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_event, NULL));
    wifi_config_t sta = { 0 };
    esp_wifi_get_config(WIFI_IF_STA, &sta);
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_start());
    if (sta.sta.ssid[0] == 0) start_ap();
    /* else: STA_START event triggers connect */
}

bool net_connected(void) { return s_connected; }
bool net_provisioning(void) { return s_prov; }
void net_ip(char *out, size_t len) { snprintf(out, len, "%s", s_connected ? s_ip : ""); }

int net_rssi(void)
{
    wifi_ap_record_t r;
    return esp_wifi_sta_get_ap_info(&r) == ESP_OK ? r.rssi : 0;
}

void net_set_credentials(const char *ssid, const char *pass)
{
    wifi_config_t c = { 0 };
    strlcpy((char *)c.sta.ssid, ssid, sizeof c.sta.ssid);
    strlcpy((char *)c.sta.password, pass, sizeof c.sta.password);
    esp_wifi_set_config(WIFI_IF_STA, &c);
}

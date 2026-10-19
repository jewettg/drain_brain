idf_component_register(
    SRCS "app_main.c" "dbhal.c" "store.c" "net.c" "api.c" "notify.c"
    INCLUDE_DIRS "."
    REQUIRES core nvs_flash esp_wifi esp_netif esp_event esp_http_server esp_http_client json driver esp_adc esp_timer mbedtls esp_app_format app_update
    EMBED_TXTFILES "../web/index.html")

#pragma once
#include <stdbool.h>
#include <stddef.h>
void net_start(void);                       /* STA if credentials exist, else provisioning AP */
bool net_connected(void);
bool net_provisioning(void);                /* SoftAP provisioning active */
void net_ip(char *out, size_t len);
int net_rssi(void);
void net_set_credentials(const char *ssid, const char *pass);   /* saved by esp_wifi; reboot to apply */

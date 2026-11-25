#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "settings_store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*wifi_ready_cb_t)(void *ctx);

esp_err_t wifi_link_start(grokbot_settings_t *settings, wifi_ready_cb_t on_ready, void *ctx);
bool wifi_link_is_connected(void);
const char *wifi_link_ip(void);

/* Resolves _grokbot._tcp.local via mDNS into an IPv4 string and port. */
esp_err_t wifi_link_discover_brain(char *ip_out, size_t ip_cap, uint16_t *port_out,
                                   uint32_t timeout_ms);

#ifdef __cplusplus
}
#endif

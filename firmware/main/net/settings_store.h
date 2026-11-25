#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char wifi_ssid[33];
    char wifi_password[65];
    char brain_host[64]; /* Empty = discover _grokbot._tcp via mDNS */
    uint16_t brain_port;
    int volume;
    int brightness;
} grokbot_settings_t;

esp_err_t settings_init(void);
esp_err_t settings_load(grokbot_settings_t *out);
esp_err_t settings_save(const grokbot_settings_t *in);
void settings_apply_kconfig_defaults(grokbot_settings_t *out);
bool settings_has_wifi(const grokbot_settings_t *s);

#ifdef __cplusplus
}
#endif

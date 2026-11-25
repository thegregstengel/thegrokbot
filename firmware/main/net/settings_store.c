#include "settings_store.h"

#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"
#include "nvs_flash.h"

static const char *TAG = "settings";
static const char *NS = "grokbot";

static void trim_inplace(char *s)
{
    if (!s) {
        return;
    }
    char *start = s;
    while (*start == ' ' || *start == '\t' || *start == '\r' || *start == '\n') {
        start++;
    }
    if (start != s) {
        memmove(s, start, strlen(start) + 1);
    }
    size_t n = strlen(s);
    while (n > 0 && (s[n - 1] == ' ' || s[n - 1] == '\t' || s[n - 1] == '\r' || s[n - 1] == '\n')) {
        s[--n] = '\0';
    }
}

void settings_apply_kconfig_defaults(grokbot_settings_t *out)
{
    memset(out, 0, sizeof(*out));
    strncpy(out->wifi_ssid, CONFIG_GROKBOT_WIFI_SSID, sizeof(out->wifi_ssid) - 1);
    strncpy(out->wifi_password, CONFIG_GROKBOT_WIFI_PASSWORD, sizeof(out->wifi_password) - 1);
    strncpy(out->brain_host, CONFIG_GROKBOT_BRAIN_HOST, sizeof(out->brain_host) - 1);
    trim_inplace(out->wifi_ssid);
    trim_inplace(out->wifi_password);
    trim_inplace(out->brain_host);
    out->brain_port = (uint16_t)CONFIG_GROKBOT_BRAIN_PORT;
    out->volume = CONFIG_GROKBOT_DEFAULT_VOLUME;
    out->brightness = CONFIG_GROKBOT_DEFAULT_BRIGHTNESS;
}

esp_err_t settings_init(void)
{
    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    return err;
}

static void read_nvs_str(nvs_handle_t h, const char *key, char *dst, size_t dst_len)
{
    size_t req = dst_len;
    esp_err_t err = nvs_get_str(h, key, dst, &req);
    if (err != ESP_OK) {
        dst[0] = '\0';
    }
}

esp_err_t settings_load(grokbot_settings_t *out)
{
    settings_apply_kconfig_defaults(out);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NS, NVS_READONLY, &h);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        return ESP_OK;
    }
    ESP_RETURN_ON_ERROR(err, TAG, "nvs open");

    char ssid[33] = {0};
    char pass[65] = {0};
    char host[64] = {0};
    read_nvs_str(h, "wifi_ssid", ssid, sizeof(ssid));
    read_nvs_str(h, "wifi_pass", pass, sizeof(pass));
    read_nvs_str(h, "brain_host", host, sizeof(host));

    uint16_t port = out->brain_port;
    int32_t volume = out->volume;
    int32_t brightness = out->brightness;
    nvs_get_u16(h, "brain_port", &port);
    nvs_get_i32(h, "volume", &volume);
    nvs_get_i32(h, "brightness", &brightness);
    nvs_close(h);

    if (ssid[0]) {
        strncpy(out->wifi_ssid, ssid, sizeof(out->wifi_ssid) - 1);
        strncpy(out->wifi_password, pass, sizeof(out->wifi_password) - 1);
    }
    if (host[0]) {
        strncpy(out->brain_host, host, sizeof(out->brain_host) - 1);
    }
    trim_inplace(out->wifi_ssid);
    trim_inplace(out->wifi_password);
    trim_inplace(out->brain_host);
    out->brain_port = port ? port : (uint16_t)CONFIG_GROKBOT_BRAIN_PORT;
    out->volume = (int)volume;
    out->brightness = (int)brightness;
    return ESP_OK;
}

esp_err_t settings_save(const grokbot_settings_t *in)
{
    grokbot_settings_t clean = *in;
    trim_inplace(clean.wifi_ssid);
    trim_inplace(clean.wifi_password);
    trim_inplace(clean.brain_host);

    nvs_handle_t h;
    ESP_RETURN_ON_ERROR(nvs_open(NS, NVS_READWRITE, &h), TAG, "nvs open rw");
    ESP_ERROR_CHECK(nvs_set_str(h, "wifi_ssid", clean.wifi_ssid));
    ESP_ERROR_CHECK(nvs_set_str(h, "wifi_pass", clean.wifi_password));
    ESP_ERROR_CHECK(nvs_set_str(h, "brain_host", clean.brain_host));
    ESP_ERROR_CHECK(nvs_set_u16(h, "brain_port", clean.brain_port));
    ESP_ERROR_CHECK(nvs_set_i32(h, "volume", clean.volume));
    ESP_ERROR_CHECK(nvs_set_i32(h, "brightness", clean.brightness));
    ESP_ERROR_CHECK(nvs_commit(h));
    nvs_close(h);
    ESP_LOGI(TAG, "Settings saved (ssid=%s, brain=%s:%u)",
             clean.wifi_ssid, clean.brain_host[0] ? clean.brain_host : "mdns", clean.brain_port);
    return ESP_OK;
}

bool settings_has_wifi(const grokbot_settings_t *s)
{
    return s && s->wifi_ssid[0] != '\0';
}

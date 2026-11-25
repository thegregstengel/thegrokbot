#include "wifi_link.h"

#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_random.h"
#include "esp_wifi.h"
#include "face.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "lwip/inet.h"
#include "mdns.h"

static const char *TAG = "wifi_link";

static grokbot_settings_t *s_settings;
static wifi_ready_cb_t s_ready_cb;
static void *s_ready_ctx;
static bool s_connected;
static bool s_ready_fired;
static char s_ip_str[16];
static httpd_handle_t s_httpd;
static char s_setup_pin[12];
static char s_csrf_token[17];

static bool ct_streq(const char *a, const char *b)
{
    size_t la = strlen(a);
    size_t lb = strlen(b);
    if (la != lb) {
        return false;
    }
    uint8_t diff = 0;
    for (size_t i = 0; i < la; ++i) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

static void url_decode_inplace(char *s)
{
    char *o = s;
    for (char *i = s; *i; ++i) {
        if (*i == '+') {
            *o++ = ' ';
        } else if (*i == '%' && i[1] && i[2]) {
            char hex[3] = {i[1], i[2], 0};
            *o++ = (char)strtol(hex, NULL, 16);
            i += 2;
        } else {
            *o++ = *i;
        }
    }
    *o = '\0';
}

/* Matches form key anchored at start-of-body or immediately after '&'. */
static void form_get(const char *body, const char *key, char *out, size_t out_len)
{
    out[0] = '\0';
    size_t key_len = strlen(key);
    const char *p = body;
    while (p && *p) {
        if (strncmp(p, key, key_len) == 0 && p[key_len] == '=') {
            p += key_len + 1;
            size_t n = 0;
            while (p[n] && p[n] != '&' && n + 1 < out_len) {
                out[n] = p[n];
                n++;
            }
            out[n] = '\0';
            url_decode_inplace(out);
            return;
        }
        const char *amp = strchr(p, '&');
        p = amp ? (amp + 1) : NULL;
    }
}

static void show_setup_overlay(const char *pin)
{
    if (!bsp_display_lock(500)) {
        return;
    }
    lv_obj_t *scr = lv_screen_active();
    lv_obj_t *lbl = lv_label_create(scr);
    char msg[96];
    snprintf(msg, sizeof(msg), "Wi-Fi: GrokBot-Setup\nPIN: %s\nhttp://192.168.4.1", pin);
    lv_label_set_text(lbl, msg);
    lv_obj_set_style_text_color(lbl, lv_color_white(), 0);
    lv_obj_set_style_bg_color(lbl, lv_color_black(), 0);
    lv_obj_set_style_bg_opa(lbl, LV_OPA_80, 0);
    lv_obj_set_style_pad_all(lbl, 10, 0);
    lv_obj_set_style_radius(lbl, 10, 0);
    lv_obj_set_style_text_align(lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_align(lbl, LV_ALIGN_BOTTOM_MID, 0, -28);
    bsp_display_unlock();
}

static esp_err_t setup_form_get(httpd_req_t *req)
{
    char page[1280];
    snprintf(
        page, sizeof(page),
        "<!doctype html><html><head><meta name=viewport content='width=device-width,initial-scale=1'>"
        "<title>GrokBot Setup</title><style>"
        "body{font-family:system-ui;background:#0a0a0a;color:#fff;padding:24px;max-width:28rem;margin:auto}"
        "input,button{width:100%%;padding:12px;margin:8px 0;border-radius:8px;border:0;box-sizing:border-box}"
        "button{background:#fff;color:#000;font-weight:600}"
        "p{opacity:.8;line-height:1.4}</style></head><body>"
        "<h1>GrokBot Body Setup</h1>"
        "<p>Connect to your Wi-Fi network and enter your Brain pairing token. "
        "Leave Brain IP blank to auto-discover grokbot-brain via mDNS.</p>"
        "<form method=POST action=/save>"
        "<input type=hidden name=csrf value='%s'>"
        "<input name=ssid placeholder='Wi-Fi SSID' required>"
        "<input name=pass type=password placeholder='Wi-Fi password'>"
        "<input name=token type=password placeholder='Brain pairing token (GROKBOT_AUTH_TOKEN)'>"
        "<input name=host placeholder='Brain IP (optional, blank = auto mDNS)'>"
        "<input name=port placeholder='Brain port (8765)' value='8765'>"
        "<button type=submit>Save and reboot</button></form>"
        "</body></html>",
        s_csrf_token);
    httpd_resp_set_type(req, "text/html");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return httpd_resp_send(req, page, HTTPD_RESP_USE_STRLEN);
}

static esp_err_t setup_form_post(httpd_req_t *req)
{
    if (req->content_len <= 0 || req->content_len > 512) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "invalid body size");
    }
    char body[513] = {0};
    int total = 0;
    while (total < req->content_len) {
        int r = httpd_req_recv(req, body + total, (size_t)(req->content_len - total));
        if (r <= 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "read failed");
        }
        total += r;
    }
    body[total] = '\0';

    char csrf[32] = {0};
    form_get(body, "csrf", csrf, sizeof(csrf));
    if (!ct_streq(csrf, s_csrf_token)) {
        ESP_LOGW(TAG, "Rejected /save with invalid CSRF token");
        return httpd_resp_send_err(req, HTTPD_403_FORBIDDEN, "invalid setup token");
    }

    grokbot_settings_t next = *s_settings;
    char port_str[16] = {0};
    char token[65] = {0};
    form_get(body, "ssid", next.wifi_ssid, sizeof(next.wifi_ssid));
    form_get(body, "pass", next.wifi_password, sizeof(next.wifi_password));
    form_get(body, "host", next.brain_host, sizeof(next.brain_host));
    form_get(body, "port", port_str, sizeof(port_str));
    form_get(body, "token", token, sizeof(token));
    if (next.wifi_ssid[0] == '\0') {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "ssid required");
    }
    if (token[0] != '\0') {
        strncpy(next.auth_token, token, sizeof(next.auth_token) - 1);
        next.auth_token[sizeof(next.auth_token) - 1] = '\0';
    }
    int parsed_port = atoi(port_str);
    if (parsed_port >= 1024 && parsed_port <= 65535) {
        next.brain_port = (uint16_t)parsed_port;
    }

    settings_save(&next);
    *s_settings = next;
    httpd_resp_sendstr(req, "Saved. Rebooting GrokBot...");
    vTaskDelay(pdMS_TO_TICKS(800));
    esp_restart();
    return ESP_OK;
}

static esp_err_t start_softap(void)
{
    face_set_emotion(FACE_EMOTION_CURIOUS);
    esp_netif_create_default_wifi_ap();

    /* Generate a per-boot random 8-digit WPA2 passphrase and 64-bit CSRF token. */
    uint32_t pin_num = esp_random() % 100000000U;
    snprintf(s_setup_pin, sizeof(s_setup_pin), "%08" PRIu32, pin_num);
    uint8_t csrf_bytes[8];
    esp_fill_random(csrf_bytes, sizeof(csrf_bytes));
    for (int i = 0; i < 8; ++i) {
        snprintf(s_csrf_token + i * 2, 3, "%02x", csrf_bytes[i]);
    }
    s_csrf_token[16] = '\0';

    wifi_config_t cfg = {0};
    snprintf((char *)cfg.ap.ssid, sizeof(cfg.ap.ssid), "GrokBot-Setup");
    cfg.ap.ssid_len = strlen((char *)cfg.ap.ssid);
    strncpy((char *)cfg.ap.password, s_setup_pin, sizeof(cfg.ap.password) - 1);
    cfg.ap.max_connection = 2;
    cfg.ap.authmode = WIFI_AUTH_WPA2_PSK;
    cfg.ap.channel = 6;
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_AP), TAG, "AP mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_AP, &cfg), TAG, "AP config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "AP start");

    httpd_config_t hcfg = HTTPD_DEFAULT_CONFIG();
    hcfg.server_port = 80;
    ESP_RETURN_ON_ERROR(httpd_start(&s_httpd, &hcfg), TAG, "httpd start");
    httpd_uri_t root = {.uri = "/", .method = HTTP_GET, .handler = setup_form_get};
    httpd_uri_t save = {.uri = "/save", .method = HTTP_POST, .handler = setup_form_post};
    httpd_register_uri_handler(s_httpd, &root);
    httpd_register_uri_handler(s_httpd, &save);

    show_setup_overlay(s_setup_pin);
    ESP_LOGI(TAG, "SoftAP GrokBot-Setup (WPA2 PIN: %s) up at http://192.168.4.1", s_setup_pin);
    return ESP_OK;
}

static void start_mdns(void)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
        ESP_LOGW(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return;
    }
    mdns_hostname_set(CONFIG_GROKBOT_DEVICE_ID);
    mdns_instance_name_set("Physical GrokBot Body");
}

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;
    if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
        esp_wifi_connect();
    } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        face_set_emotion(FACE_EMOTION_CONCERNED);
        ESP_LOGW(TAG, "Wi-Fi disconnected; reconnecting...");
        esp_wifi_connect();
    } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *evt = (ip_event_got_ip_t *)data;
        snprintf(s_ip_str, sizeof(s_ip_str), IPSTR, IP2STR(&evt->ip_info.ip));
        s_connected = true;
        ESP_LOGI(TAG, "Wi-Fi connected: %s", s_ip_str);
        start_mdns();
        if (!s_ready_fired && s_ready_cb) {
            s_ready_fired = true;
            s_ready_cb(s_ready_ctx);
        }
    }
}

esp_err_t wifi_link_start(grokbot_settings_t *settings, wifi_ready_cb_t on_ready, void *ctx)
{
    s_settings = settings;
    s_ready_cb = on_ready;
    s_ready_ctx = ctx;
    s_ready_fired = false;

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop");
    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event, NULL),
        TAG, "wifi handler");
    ESP_RETURN_ON_ERROR(
        esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_wifi_event, NULL),
        TAG, "ip handler");

    if (!settings_has_wifi(settings)) {
        return start_softap();
    }

    esp_netif_t *sta = esp_netif_create_default_wifi_sta();
    esp_netif_set_hostname(sta, CONFIG_GROKBOT_DEVICE_ID);

    wifi_config_t cfg = {0};
    strncpy((char *)cfg.sta.ssid, settings->wifi_ssid, sizeof(cfg.sta.ssid));
    strncpy((char *)cfg.sta.password, settings->wifi_password, sizeof(cfg.sta.password));
    cfg.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "STA mode");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &cfg), TAG, "STA config");
    ESP_RETURN_ON_ERROR(esp_wifi_start(), TAG, "STA start");
    /* Disable Wi-Fi modem sleep for low-latency audio streaming. */
    esp_wifi_set_ps(WIFI_PS_NONE);

    face_set_emotion(FACE_EMOTION_THINKING);
    ESP_LOGI(TAG, "Joining Wi-Fi '%s'...", settings->wifi_ssid);
    return ESP_OK;
}

bool wifi_link_is_connected(void)
{
    return s_connected;
}

const char *wifi_link_ip(void)
{
    return s_ip_str;
}

esp_err_t wifi_link_discover_brain(char *ip_out, size_t ip_cap, uint16_t *port_out,
                                   uint32_t timeout_ms)
{
    if (!ip_out || ip_cap < 16 || !port_out) {
        return ESP_ERR_INVALID_ARG;
    }
    mdns_result_t *results = NULL;
    esp_err_t err = mdns_query_ptr("_grokbot", "_tcp", timeout_ms, 4, &results);
    if (err != ESP_OK || !results) {
        return ESP_ERR_NOT_FOUND;
    }

    esp_err_t found = ESP_ERR_NOT_FOUND;
    for (mdns_result_t *r = results; r != NULL; r = r->next) {
        for (mdns_ip_addr_t *a = r->addr; a != NULL; a = a->next) {
            if (a->addr.type == ESP_IPADDR_TYPE_V4) {
                snprintf(ip_out, ip_cap, IPSTR, IP2STR(&a->addr.u_addr.ip4));
                *port_out = r->port ? r->port : (uint16_t)CONFIG_GROKBOT_BRAIN_PORT;
                found = ESP_OK;
                break;
            }
        }
        if (found == ESP_OK) {
            break;
        }
    }

    mdns_query_results_free(results);
    return found;
}

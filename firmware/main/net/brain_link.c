#include "brain_link.h"

#include <errno.h>
#include <netdb.h>
#include <string.h>
#include <sys/socket.h>
#include <unistd.h>

#include "audio_pipeline.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "grok_proto.h"
#include "lwip/sockets.h"
#include "wifi_link.h"

static const char *TAG = "brain_link";

#define TX_QUEUE_DEPTH  24
#define TX_MAX_PAYLOAD  700

typedef struct {
    uint8_t msg_type;
    uint8_t flags;
    uint16_t len;
    uint8_t payload[TX_MAX_PAYLOAD];
} tx_item_t;

static grokbot_settings_t s_settings;
static brain_link_cbs_t s_cbs;
static QueueHandle_t s_tx_queue;
static volatile int s_sock = -1;
static volatile bool s_connected;
static bool s_started;

static int sock_write(void *ctx, const uint8_t *buf, size_t len)
{
    int fd = *(int *)ctx;
    int r = send(fd, buf, len, 0);
    return (r > 0) ? r : -1;
}

static int sock_read(void *ctx, uint8_t *buf, size_t len)
{
    int fd = *(int *)ctx;
    int r = recv(fd, buf, len, 0);
    return (r > 0) ? r : -1;
}

static esp_err_t enqueue_frame(uint8_t msg_type, uint8_t flags, const void *payload, uint16_t len,
                               bool drop_if_full)
{
    if (!s_connected || !s_tx_queue || len > TX_MAX_PAYLOAD) {
        return ESP_ERR_INVALID_STATE;
    }
    tx_item_t item;
    item.msg_type = msg_type;
    item.flags = flags;
    item.len = len;
    if (len > 0 && payload) {
        memcpy(item.payload, payload, len);
    }
    TickType_t wait = drop_if_full ? 0 : pdMS_TO_TICKS(50);
    return xQueueSend(s_tx_queue, &item, wait) == pdTRUE ? ESP_OK : ESP_ERR_TIMEOUT;
}

static int connect_to_brain(const char *host, uint16_t port)
{
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || !res) {
        ESP_LOGW(TAG, "DNS/address lookup failed for %s:%u", host, port);
        return -1;
    }

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        return -1;
    }

    int nodelay = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &nodelay, sizeof(nodelay));
    int keepalive = 1;
    setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &keepalive, sizeof(keepalive));

    struct timeval tv = {.tv_sec = 5, .tv_usec = 0};
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(fd, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGW(TAG, "connect(%s:%u) failed: errno=%d", host, port, errno);
        close(fd);
        freeaddrinfo(res);
        return -1;
    }

    freeaddrinfo(res);
    return fd;
}

static void tx_task(void *arg)
{
    (void)arg;
    tx_item_t item;
    uint32_t ping_seq = 0;

    while (true) {
        if (xQueueReceive(s_tx_queue, &item, pdMS_TO_TICKS(5000)) == pdTRUE) {
            int fd = s_sock;
            if (fd >= 0 && s_connected) {
                if (grok_proto_write_frame(sock_write, &fd, item.msg_type, item.flags,
                                           item.payload, item.len) != 0) {
                    ESP_LOGW(TAG, "TX failed; closing socket");
                    shutdown(fd, SHUT_RDWR);
                }
            }
        } else {
            /* Idle heartbeat keepalive every 5s. */
            int fd = s_sock;
            if (fd >= 0 && s_connected) {
                uint8_t seq[4];
                ping_seq++;
                seq[0] = (uint8_t)(ping_seq & 0xFF);
                seq[1] = (uint8_t)((ping_seq >> 8) & 0xFF);
                seq[2] = (uint8_t)((ping_seq >> 16) & 0xFF);
                seq[3] = (uint8_t)((ping_seq >> 24) & 0xFF);
                if (grok_proto_write_frame(sock_write, &fd, GROK_MSG_PING, 0, seq, 4) != 0) {
                    shutdown(fd, SHUT_RDWR);
                }
            }
        }
    }
}

static void rx_task(void *arg)
{
    (void)arg;
    uint8_t *rx_buf = heap_caps_malloc(GROK_PROTO_MAX_PAYLOAD, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!rx_buf) {
        rx_buf = malloc(GROK_PROTO_MAX_PAYLOAD);
    }
    if (!rx_buf) {
        ESP_LOGE(TAG, "Failed to allocate RX buffer");
        vTaskDelete(NULL);
        return;
    }

    int backoff_ms = 1000;
    while (true) {
        if (!wifi_link_is_connected()) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }

        char host[64] = {0};
        uint16_t port = s_settings.brain_port;
        if (s_settings.brain_host[0] != '\0') {
            strncpy(host, s_settings.brain_host, sizeof(host) - 1);
        } else {
            ESP_LOGI(TAG, "Discovering _grokbot._tcp via mDNS...");
            if (wifi_link_discover_brain(host, sizeof(host), &port, 3000) != ESP_OK) {
                ESP_LOGW(TAG, "Brain not found via mDNS yet; retrying...");
                vTaskDelay(pdMS_TO_TICKS(backoff_ms));
                backoff_ms = (backoff_ms < 5000) ? backoff_ms + 1000 : 5000;
                continue;
            }
            ESP_LOGI(TAG, "Discovered Brain at %s:%u", host, port);
        }

        int fd = connect_to_brain(host, port);
        if (fd < 0) {
            vTaskDelay(pdMS_TO_TICKS(backoff_ms));
            backoff_ms = (backoff_ms < 5000) ? backoff_ms + 1000 : 5000;
            continue;
        }

        backoff_ms = 1000;
        s_sock = fd;

        /* Send HELLO immediately before accepting TX traffic. */
        uint8_t hello[64];
        size_t id_len = strlen(CONFIG_GROKBOT_DEVICE_ID);
        if (id_len > sizeof(hello) - 4) {
            id_len = sizeof(hello) - 4;
        }
        uint8_t caps = 0;
        if (audio_pipeline_has_aec()) {
            caps |= GROK_CAP_AEC;
        }
        if (audio_pipeline_has_wakeword()) {
            caps |= GROK_CAP_WAKEWORD;
        }
        hello[0] = GROK_PROTO_VERSION;
        hello[1] = (uint8_t)(CONFIG_GROKBOT_SAMPLE_RATE_HZ / 1000);
        hello[2] = 1;
        hello[3] = caps;
        memcpy(hello + 4, CONFIG_GROKBOT_DEVICE_ID, id_len);

        if (grok_proto_write_frame(sock_write, &fd, GROK_MSG_HELLO, 0, hello,
                                   (uint16_t)(4 + id_len)) != 0) {
            close(fd);
            s_sock = -1;
            continue;
        }

        xQueueReset(s_tx_queue);
        s_connected = true;
        ESP_LOGI(TAG, "Connected to Brain %s:%u", host, port);
        if (s_cbs.on_connected) {
            s_cbs.on_connected(s_cbs.ctx);
        }

        grok_frame_hdr_t hdr;
        while (grok_proto_read_frame(sock_read, &fd, &hdr, rx_buf, GROK_PROTO_MAX_PAYLOAD) == 0) {
            switch (hdr.msg_type) {
            case GROK_MSG_WELCOME:
                ESP_LOGI(TAG, "Brain WELCOME received");
                break;
            case GROK_MSG_AUDIO_DOWN:
                if (hdr.len >= 2 && s_cbs.on_audio_down) {
                    s_cbs.on_audio_down((const int16_t *)rx_buf, hdr.len / sizeof(int16_t),
                                        s_cbs.ctx);
                }
                break;
            case GROK_MSG_FACE_STATE:
                if (hdr.len >= 4 && s_cbs.on_face_state) {
                    s_cbs.on_face_state((face_emotion_t)rx_buf[0], rx_buf[1], (int8_t)rx_buf[2],
                                        (int8_t)rx_buf[3], s_cbs.ctx);
                }
                break;
            case GROK_MSG_STOP_AUDIO:
                if (s_cbs.on_stop_audio) {
                    s_cbs.on_stop_audio(s_cbs.ctx);
                }
                break;
            case GROK_MSG_CONFIG:
                if (hdr.len >= 2 && s_cbs.on_config) {
                    s_cbs.on_config(rx_buf[0], rx_buf[1], s_cbs.ctx);
                }
                break;
            case GROK_MSG_PONG:
                break;
            default:
                ESP_LOGW(TAG, "Unknown frame type 0x%02x", hdr.msg_type);
                break;
            }
        }

        s_connected = false;
        s_sock = -1;
        close(fd);
        ESP_LOGW(TAG, "Brain disconnected; will reconnect");
        if (s_cbs.on_disconnected) {
            s_cbs.on_disconnected(s_cbs.ctx);
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

esp_err_t brain_link_start(const grokbot_settings_t *settings, const brain_link_cbs_t *cbs)
{
    if (s_started || !settings) {
        return ESP_ERR_INVALID_STATE;
    }
    s_settings = *settings;
    if (cbs) {
        s_cbs = *cbs;
    }
    s_tx_queue = xQueueCreateWithCaps(TX_QUEUE_DEPTH, sizeof(tx_item_t),
                                      MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_tx_queue) {
        s_tx_queue = xQueueCreate(8, sizeof(tx_item_t));
    }
    ESP_RETURN_ON_FALSE(s_tx_queue != NULL, ESP_ERR_NO_MEM, TAG, "tx queue failed");

    s_started = true;
    BaseType_t ok = xTaskCreatePinnedToCore(rx_task, "brain_rx", 4096, NULL, 5, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "brain_rx failed");
    ok = xTaskCreatePinnedToCore(tx_task, "brain_tx", 3072, NULL, 5, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "brain_tx failed");
    return ESP_OK;
}

bool brain_link_is_connected(void)
{
    return s_connected;
}

esp_err_t brain_link_send_audio(const int16_t *pcm, size_t samples)
{
    return enqueue_frame(GROK_MSG_AUDIO_UP, 0, pcm, (uint16_t)(samples * sizeof(int16_t)), true);
}

esp_err_t brain_link_send_wake(uint8_t source)
{
    return enqueue_frame(GROK_MSG_WAKE, 0, &source, 1, false);
}

esp_err_t brain_link_send_barge_in(uint8_t source)
{
    return enqueue_frame(GROK_MSG_BARGE_IN, 1, &source, 1, false);
}

esp_err_t brain_link_send_sleep(void)
{
    return enqueue_frame(GROK_MSG_SLEEP, 0, NULL, 0, false);
}

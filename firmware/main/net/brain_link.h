#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "face.h"
#include "settings_store.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void (*on_connected)(void *ctx);
    void (*on_disconnected)(void *ctx);
    void (*on_audio_down)(const int16_t *pcm, size_t samples, void *ctx);
    void (*on_face_state)(face_emotion_t emotion, uint8_t energy, int8_t yaw, int8_t pitch,
                          void *ctx);
    void (*on_stop_audio)(void *ctx);
    void (*on_config)(uint8_t volume, uint8_t brightness, void *ctx);
    void *ctx;
} brain_link_cbs_t;

esp_err_t brain_link_start(const grokbot_settings_t *settings, const brain_link_cbs_t *cbs);
bool brain_link_is_connected(void);

esp_err_t brain_link_send_audio(const int16_t *pcm, size_t samples);
esp_err_t brain_link_send_wake(uint8_t source);
esp_err_t brain_link_send_barge_in(uint8_t source);
esp_err_t brain_link_send_sleep(void);

#ifdef __cplusplus
}
#endif

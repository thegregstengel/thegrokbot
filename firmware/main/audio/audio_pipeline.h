#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*audio_pcm_cb_t)(const int16_t *pcm, size_t samples, bool vad_speech, void *ctx);
typedef void (*audio_wake_cb_t)(void *ctx);

typedef struct {
    audio_pcm_cb_t on_pcm;
    audio_wake_cb_t on_wake;
    void *ctx;
} audio_pipeline_cbs_t;

esp_err_t audio_pipeline_init(void);
esp_err_t audio_pipeline_start(const audio_pipeline_cbs_t *cbs);

/* Non-blocking push into the speaker jitter/ring buffer (safe from network RX task). */
esp_err_t audio_pipeline_push_playback(const int16_t *pcm, size_t samples);

/* Immediately drops buffered speaker audio (used on barge-in or STOP_AUDIO). */
void audio_pipeline_flush_playback(void);

/* Plays a short non-blocking feedback chime (wake = true, tick = false). */
void audio_pipeline_play_chime(bool wake_chime);

/* True while buffered audio is actively draining to the speaker. */
bool audio_pipeline_is_playing(void);

/* Reports whether ESP-SR AEC and WakeNet are active on this boot. */
bool audio_pipeline_has_aec(void);
bool audio_pipeline_has_wakeword(void);

#ifdef __cplusplus
}
#endif

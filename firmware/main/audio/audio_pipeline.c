#include "audio_pipeline.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "board.h"
#include "esp_check.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#if CONFIG_GROKBOT_USE_ESP_SR
#include "esp_afe_sr_iface.h"
#include "esp_afe_sr_models.h"
#include "esp_wn_iface.h"
#include "esp_wn_models.h"
#include "model_path.h"
#endif

static const char *TAG = "audio_pipe";

/* 20 ms at 16 kHz mono = 320 samples (640 bytes). */
#define PCM_FRAME_SAMPLES     320
#define PCM_FRAME_BYTES       (PCM_FRAME_SAMPLES * sizeof(int16_t))

/* 8 seconds of 16 kHz mono PCM16 in PSRAM absorbs any Wi-Fi/xAI bursts without drops. */
#define PLAYBACK_RB_BYTES     (256 * 1024)
/* Pre-buffer 80 ms (4 frames) on a new utterance to prevent initial DMA underruns. */
#define PLAYBACK_PREBUF_BYTES (PCM_FRAME_BYTES * 4)
/* Playback reference ring buffer for software AEC loopback (4096 samples = 256 ms). */
#define REF_RB_SAMPLES        4096

static uint8_t *s_rb;
static size_t s_rb_head;
static size_t s_rb_tail;
static size_t s_rb_count;
static SemaphoreHandle_t s_rb_lock;

static int16_t *s_ref_rb;
static size_t s_ref_head;
static size_t s_ref_tail;
static size_t s_ref_count;
static SemaphoreHandle_t s_ref_lock;

static volatile bool s_playing;
static volatile bool s_buffering = true;

static audio_pipeline_cbs_t s_cbs;
static bool s_started;
static bool s_has_aec;
static bool s_has_wakeword;

#if CONFIG_GROKBOT_USE_ESP_SR
static const esp_afe_sr_iface_t *s_afe_handle;
static esp_afe_sr_data_t *s_afe_data;
static srmodel_list_t *s_models;
#endif

static size_t rb_read_locked(uint8_t *dst, size_t max_bytes)
{
    size_t to_read = (max_bytes < s_rb_count) ? max_bytes : s_rb_count;
    size_t first = PLAYBACK_RB_BYTES - s_rb_tail;
    if (first > to_read) {
        first = to_read;
    }
    memcpy(dst, s_rb + s_rb_tail, first);
    if (to_read > first) {
        memcpy(dst + first, s_rb, to_read - first);
    }
    s_rb_tail = (s_rb_tail + to_read) % PLAYBACK_RB_BYTES;
    s_rb_count -= to_read;
    return to_read;
}

static size_t rb_write_locked(const uint8_t *src, size_t bytes)
{
    size_t free_space = PLAYBACK_RB_BYTES - s_rb_count;
    if (bytes > free_space) {
        size_t drop = bytes - free_space;
        s_rb_tail = (s_rb_tail + drop) % PLAYBACK_RB_BYTES;
        s_rb_count -= drop;
    }
    size_t first = PLAYBACK_RB_BYTES - s_rb_head;
    if (first > bytes) {
        first = bytes;
    }
    memcpy(s_rb + s_rb_head, src, first);
    if (bytes > first) {
        memcpy(s_rb, src + first, bytes - first);
    }
    s_rb_head = (s_rb_head + bytes) % PLAYBACK_RB_BYTES;
    s_rb_count += bytes;
    return bytes;
}

static void ref_push(const int16_t *samples, size_t n)
{
    if (!s_ref_lock || xSemaphoreTake(s_ref_lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        return;
    }
    if (n > REF_RB_SAMPLES) {
        samples += (n - REF_RB_SAMPLES);
        n = REF_RB_SAMPLES;
    }
    size_t free_sp = REF_RB_SAMPLES - s_ref_count;
    if (n > free_sp) {
        size_t drop = n - free_sp;
        s_ref_tail = (s_ref_tail + drop) % REF_RB_SAMPLES;
        s_ref_count -= drop;
    }
    for (size_t i = 0; i < n; ++i) {
        s_ref_rb[s_ref_head] = samples[i];
        s_ref_head = (s_ref_head + 1) % REF_RB_SAMPLES;
    }
    s_ref_count += n;
    xSemaphoreGive(s_ref_lock);
}

static void ref_pop(int16_t *dst, size_t n)
{
    if (!s_ref_lock || xSemaphoreTake(s_ref_lock, pdMS_TO_TICKS(5)) != pdTRUE) {
        memset(dst, 0, n * sizeof(int16_t));
        return;
    }
    size_t take = (n < s_ref_count) ? n : s_ref_count;
    for (size_t i = 0; i < take; ++i) {
        dst[i] = s_ref_rb[s_ref_tail];
        s_ref_tail = (s_ref_tail + 1) % REF_RB_SAMPLES;
    }
    s_ref_count -= take;
    xSemaphoreGive(s_ref_lock);
    if (take < n) {
        memset(dst + take, 0, (n - take) * sizeof(int16_t));
    }
}

static void playback_task(void *arg)
{
    (void)arg;
    uint8_t chunk[PCM_FRAME_BYTES];
    esp_codec_dev_handle_t spk = board_speaker();
    int idle_ticks = 0;

    while (true) {
        size_t got = 0;
        if (xSemaphoreTake(s_rb_lock, pdMS_TO_TICKS(10)) == pdTRUE) {
            if (s_buffering) {
                if (s_rb_count >= PLAYBACK_PREBUF_BYTES) {
                    s_buffering = false;
                }
            }
            if (!s_buffering && s_rb_count > 0) {
                got = rb_read_locked(chunk, sizeof(chunk));
            }
            xSemaphoreGive(s_rb_lock);
        }

        if (got > 0) {
            if (got < sizeof(chunk)) {
                memset(chunk + got, 0, sizeof(chunk) - got);
            }
            s_playing = true;
            idle_ticks = 0;
            ref_push((const int16_t *)chunk, PCM_FRAME_SAMPLES);
            int wret = esp_codec_dev_write(spk, chunk, sizeof(chunk));
            if (wret != ESP_CODEC_DEV_OK) {
                ESP_LOGW(TAG, "esp_codec_dev_write failed: %d", wret);
            }
        } else {
            if (s_playing && idle_ticks == 0) {
                /* Write one silent frame so the I2S DMA tail clears without pops. */
                memset(chunk, 0, sizeof(chunk));
                esp_codec_dev_write(spk, chunk, sizeof(chunk));
            }
            /* Hold s_playing for ~250 ms after drain so residual room echo fades. */
            if (++idle_ticks > 50) {
                s_playing = false;
                s_buffering = true;
            }
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

#if CONFIG_GROKBOT_USE_ESP_SR
static void afe_feed_task(void *arg)
{
    (void)arg;
    esp_codec_dev_handle_t mic = board_microphone();
    int mic_ch = board_mic_channel_count();
    int chunksize = s_afe_handle->get_feed_chunksize(s_afe_data);
    int afe_ch = s_afe_handle->get_feed_channel_num(s_afe_data);

    size_t mic_bytes = (size_t)chunksize * (size_t)mic_ch * sizeof(int16_t);
    size_t afe_bytes = (size_t)chunksize * (size_t)afe_ch * sizeof(int16_t);

    int16_t *mic_buf = heap_caps_malloc(mic_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *ref_buf = heap_caps_malloc((size_t)chunksize * sizeof(int16_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    int16_t *afe_buf = heap_caps_malloc(afe_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!mic_buf || !ref_buf || !afe_buf) {
        ESP_LOGE(TAG, "Failed to allocate AFE feed buffers");
        free(mic_buf);
        free(ref_buf);
        free(afe_buf);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "AFE feed task running (chunk=%d, mic_ch=%d, afe_ch=%d)",
             chunksize, mic_ch, afe_ch);
    while (true) {
        if (esp_codec_dev_read(mic, mic_buf, (int)mic_bytes) == ESP_CODEC_DEV_OK) {
            if (afe_ch == mic_ch + 1) {
                /* Interleave [MIC1(, MIC2), PLAYBACK_REF] for ESP-SR AEC. */
                ref_pop(ref_buf, (size_t)chunksize);
                for (int i = 0; i < chunksize; ++i) {
                    for (int c = 0; c < mic_ch; ++c) {
                        afe_buf[i * afe_ch + c] = mic_buf[i * mic_ch + c];
                    }
                    afe_buf[i * afe_ch + mic_ch] = ref_buf[i];
                }
                s_afe_handle->feed(s_afe_data, afe_buf);
            } else {
                s_afe_handle->feed(s_afe_data, mic_buf);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

static void afe_fetch_task(void *arg)
{
    (void)arg;
    int16_t out_accum[PCM_FRAME_SAMPLES];
    size_t out_fill = 0;

    ESP_LOGI(TAG, "AFE fetch task running");
    while (true) {
        afe_fetch_result_t *res = s_afe_handle->fetch(s_afe_data);
        if (!res || res->ret_value == ESP_FAIL) {
            vTaskDelay(pdMS_TO_TICKS(5));
            continue;
        }

        if (res->wakeup_state == WAKENET_DETECTED) {
            ESP_LOGI(TAG, "WakeNet detected (model=%d, word=%d)",
                     res->wakenet_model_index, res->wake_word_index);
            if (s_cbs.on_wake) {
                s_cbs.on_wake(s_cbs.ctx);
            }
        }

        if (res->data && res->data_size > 0 && s_cbs.on_pcm) {
            bool speech = (res->vad_state == VAD_SPEECH);
            const int16_t *src = res->data;
            size_t total_samples = (size_t)res->data_size / sizeof(int16_t);
            size_t idx = 0;
            while (idx < total_samples) {
                size_t space = PCM_FRAME_SAMPLES - out_fill;
                size_t copy = (total_samples - idx < space) ? (total_samples - idx) : space;
                memcpy(out_accum + out_fill, src + idx, copy * sizeof(int16_t));
                out_fill += copy;
                idx += copy;
                if (out_fill == PCM_FRAME_SAMPLES) {
                    s_cbs.on_pcm(out_accum, PCM_FRAME_SAMPLES, speech, s_cbs.ctx);
                    out_fill = 0;
                }
            }
        }
    }
}
#endif

static void raw_capture_task(void *arg)
{
    (void)arg;
    esp_codec_dev_handle_t mic = board_microphone();
    int16_t frame[PCM_FRAME_SAMPLES];
    ESP_LOGI(TAG, "Raw mono capture task running (ESP-SR disabled)");
    while (true) {
        if (esp_codec_dev_read(mic, frame, sizeof(frame)) == ESP_CODEC_DEV_OK) {
            if (s_cbs.on_pcm) {
                s_cbs.on_pcm(frame, PCM_FRAME_SAMPLES, true, s_cbs.ctx);
            }
        } else {
            vTaskDelay(pdMS_TO_TICKS(5));
        }
    }
}

esp_err_t audio_pipeline_init(void)
{
    s_rb_lock = xSemaphoreCreateMutex();
    s_ref_lock = xSemaphoreCreateMutex();
    ESP_RETURN_ON_FALSE(s_rb_lock && s_ref_lock, ESP_ERR_NO_MEM, TAG, "mutex failed");

    s_rb = heap_caps_calloc(1, PLAYBACK_RB_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_ref_rb = heap_caps_calloc(REF_RB_SAMPLES, sizeof(int16_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_rb) {
        s_rb = calloc(1, PLAYBACK_RB_BYTES);
    }
    if (!s_ref_rb) {
        s_ref_rb = calloc(REF_RB_SAMPLES, sizeof(int16_t));
    }
    ESP_RETURN_ON_FALSE(s_rb && s_ref_rb, ESP_ERR_NO_MEM, TAG, "ring buffer alloc failed");

#if CONFIG_GROKBOT_USE_ESP_SR
    s_models = esp_srmodel_init("model");
    if (!s_models || s_models->num == 0) {
        ESP_LOGW(TAG, "No ESP-SR models found in 'model' partition; running without WakeNet");
    } else {
        for (int i = 0; i < s_models->num; ++i) {
            ESP_LOGI(TAG, "ESP-SR model[%d]: %s", i, s_models->model_name[i]);
        }
    }

    const char *fmt = (board_mic_channel_count() == 2) ? "MMR" : "MR";
    afe_config_t *afe_cfg = afe_config_init(fmt, s_models, AFE_TYPE_SR, AFE_MODE_LOW_COST);
    if (afe_cfg) {
        afe_cfg->aec_init = true;
        afe_cfg->se_init = true;
        afe_cfg->vad_init = true;
        afe_cfg->vad_mode = VAD_MODE_3;
        afe_cfg->agc_init = true;
        afe_cfg->agc_mode = AFE_AGC_MODE_WEBRTC;
        afe_cfg->agc_compression_gain_db = 12;
        afe_cfg->agc_target_level_dbfs = 3;
        afe_cfg->wakenet_init = (s_models != NULL && s_models->num > 0);
        afe_cfg->memory_alloc_mode = AFE_MEMORY_ALLOC_MORE_PSRAM;
        afe_cfg->afe_ringbuf_size = 50;

        s_afe_handle = esp_afe_handle_from_config(afe_cfg);
        if (s_afe_handle) {
            s_afe_data = s_afe_handle->create_from_config(afe_cfg);
        }
        if (s_afe_data) {
            s_has_aec = afe_cfg->aec_init;
            s_has_wakeword = afe_cfg->wakenet_init;
            ESP_LOGI(TAG, "ESP-SR AFE ready (fmt=%s, aec=%d, wakenet=%d)",
                     fmt, s_has_aec ? 1 : 0, s_has_wakeword ? 1 : 0);
        } else {
            ESP_LOGW(TAG, "ESP-SR AFE create failed; falling back to raw capture");
        }
        afe_config_free(afe_cfg);
    }
#endif

    ESP_LOGI(TAG, "Audio pipeline initialized (%d Hz, ring=%u bytes)",
             CONFIG_GROKBOT_SAMPLE_RATE_HZ, (unsigned)PLAYBACK_RB_BYTES);
    return ESP_OK;
}

esp_err_t audio_pipeline_start(const audio_pipeline_cbs_t *cbs)
{
    if (s_started) {
        return ESP_OK;
    }
    if (cbs) {
        s_cbs = *cbs;
    }
    s_started = true;

    BaseType_t ok = xTaskCreatePinnedToCore(playback_task, "spk_play", 4096, NULL, 7, NULL, 1);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "spk_play task failed");

#if CONFIG_GROKBOT_USE_ESP_SR
    if (s_afe_handle && s_afe_data) {
        ok = xTaskCreatePinnedToCore(afe_feed_task, "afe_feed", 4096, NULL, 6, NULL, 0);
        ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "afe_feed task failed");
        ok = xTaskCreatePinnedToCore(afe_fetch_task, "afe_fetch", 6144, NULL, 5, NULL, 1);
        ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "afe_fetch task failed");
        return ESP_OK;
    }
#endif

    ok = xTaskCreatePinnedToCore(raw_capture_task, "mic_raw", 4096, NULL, 5, NULL, 0);
    ESP_RETURN_ON_FALSE(ok == pdPASS, ESP_ERR_NO_MEM, TAG, "mic_raw task failed");
    return ESP_OK;
}

esp_err_t audio_pipeline_push_playback(const int16_t *pcm, size_t samples)
{
    if (!s_rb || !pcm || samples == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    if (xSemaphoreTake(s_rb_lock, pdMS_TO_TICKS(20)) != pdTRUE) {
        return ESP_ERR_TIMEOUT;
    }
    rb_write_locked((const uint8_t *)pcm, samples * sizeof(int16_t));
    xSemaphoreGive(s_rb_lock);
    return ESP_OK;
}

void audio_pipeline_flush_playback(void)
{
    if (s_rb_lock && xSemaphoreTake(s_rb_lock, pdMS_TO_TICKS(50)) == pdTRUE) {
        s_rb_head = 0;
        s_rb_tail = 0;
        s_rb_count = 0;
        s_buffering = true;
        s_playing = false;
        xSemaphoreGive(s_rb_lock);
    }
    if (s_ref_lock && xSemaphoreTake(s_ref_lock, pdMS_TO_TICKS(20)) == pdTRUE) {
        s_ref_head = 0;
        s_ref_tail = 0;
        s_ref_count = 0;
        xSemaphoreGive(s_ref_lock);
    }
}

void audio_pipeline_play_chime(bool wake_chime)
{
    if (!s_rb) {
        return;
    }
    const float sr = (float)CONFIG_GROKBOT_SAMPLE_RATE_HZ;
    int16_t buf[PCM_FRAME_SAMPLES];
    if (wake_chime) {
        /* Gentle 2-note ascending sine chime (~120 ms total). */
        const float freqs[2] = {587.33f, 880.0f};
        for (int n = 0; n < 2; ++n) {
            const int frames = 3; /* 60 ms per note */
            const int total = frames * PCM_FRAME_SAMPLES;
            for (int f = 0; f < frames; ++f) {
                for (int i = 0; i < PCM_FRAME_SAMPLES; ++i) {
                    int idx = f * PCM_FRAME_SAMPLES + i;
                    float env = sinf((float)M_PI * (float)idx / (float)total);
                    float s = sinf(2.0f * (float)M_PI * freqs[n] * (float)idx / sr);
                    buf[i] = (int16_t)(s * env * 9000.0f);
                }
                audio_pipeline_push_playback(buf, PCM_FRAME_SAMPLES);
            }
        }
    } else {
        /* Short 80 ms volume confirmation tick (meets 4-frame pre-buffer). */
        const int frames = 4;
        const int total = 3 * PCM_FRAME_SAMPLES;
        for (int f = 0; f < frames; ++f) {
            for (int i = 0; i < PCM_FRAME_SAMPLES; ++i) {
                int idx = f * PCM_FRAME_SAMPLES + i;
                if (idx < total) {
                    float env = sinf((float)M_PI * (float)idx / (float)total);
                    float s = sinf(2.0f * (float)M_PI * 660.0f * (float)idx / sr);
                    buf[i] = (int16_t)(s * env * 7500.0f);
                } else {
                    buf[i] = 0;
                }
            }
            audio_pipeline_push_playback(buf, PCM_FRAME_SAMPLES);
        }
    }
}

bool audio_pipeline_is_playing(void)
{
    return s_playing;
}

bool audio_pipeline_has_aec(void)
{
    return s_has_aec;
}

bool audio_pipeline_has_wakeword(void)
{
    return s_has_wakeword;
}

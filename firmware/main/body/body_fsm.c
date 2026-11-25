#include "body_fsm.h"

#include "audio_pipeline.h"
#include "board.h"
#include "brain_link.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "face.h"
#include "grok_proto.h"
#include "wifi_link.h"

static const char *TAG = "body_fsm";

static grokbot_settings_t *s_settings;
static volatile bool s_awake;
static esp_timer_handle_t s_sleep_timer;

static void arm_sleep_timer(void)
{
    if (!s_sleep_timer) {
        return;
    }
    esp_timer_stop(s_sleep_timer);
    esp_timer_start_once(s_sleep_timer, (uint64_t)CONFIG_GROKBOT_FOLLOWUP_TIMEOUT_MS * 1000ULL);
}

static void on_sleep_timeout(void *arg)
{
    (void)arg;
    if (!s_awake) {
        return;
    }
    if (audio_pipeline_is_playing()) {
        arm_sleep_timer();
        return;
    }
    body_fsm_trigger_sleep();
}

static void trigger_wake_or_barge_in(uint8_t source)
{
    if (!brain_link_is_connected()) {
        ESP_LOGW(TAG, "Wake ignored: Brain not connected yet");
        face_set_emotion(FACE_EMOTION_CONCERNED);
        return;
    }

    if (s_awake && audio_pipeline_is_playing()) {
        /* Instant local barge-in: cut speaker immediately and inform Brain. */
        audio_pipeline_flush_playback();
        brain_link_send_barge_in(source);
        face_set_emotion(FACE_EMOTION_LISTENING);
        arm_sleep_timer();
        ESP_LOGI(TAG, "Barge-in (source=%u)", source);
        return;
    }

    s_awake = true;
    audio_pipeline_play_chime(true);
    brain_link_send_wake(source);
    face_set_emotion(FACE_EMOTION_LISTENING);
    arm_sleep_timer();
    ESP_LOGI(TAG, "Awake (source=%u)", source);
}

static void on_local_wakeword(void *ctx)
{
    (void)ctx;
    trigger_wake_or_barge_in(GROK_WAKE_SRC_WAKEWORD);
}

static void on_mic_pcm(const int16_t *pcm, size_t samples, bool vad_speech, void *ctx)
{
    (void)ctx;
    if (!s_awake || !brain_link_is_connected()) {
        return;
    }
    /*
     * Gate mic uplink while the speaker is actively playing (plus a brief echo
     * tail) so residual acoustic echo never triggers false server VAD cutoffs.
     * Local WakeNet ("Hi, ESP") and screen taps remain live for instant barge-in.
     */
    if (audio_pipeline_is_playing()) {
        return;
    }
    if (vad_speech) {
        arm_sleep_timer();
    }
    brain_link_send_audio(pcm, samples);
}

static void on_brain_connected(void *ctx)
{
    (void)ctx;
    s_awake = false;
    face_set_emotion(FACE_EMOTION_HAPPY);
}

static void on_brain_disconnected(void *ctx)
{
    (void)ctx;
    s_awake = false;
    audio_pipeline_flush_playback();
    face_set_emotion(FACE_EMOTION_CONCERNED);
}

static void on_brain_audio_down(const int16_t *pcm, size_t samples, void *ctx)
{
    (void)ctx;
    if (!s_awake) {
        return;
    }
    audio_pipeline_push_playback(pcm, samples);
    arm_sleep_timer();
}

static void on_brain_face_state(face_emotion_t emotion, uint8_t energy, int8_t yaw, int8_t pitch,
                                void *ctx)
{
    (void)ctx;
    face_set_state(emotion, energy, yaw, pitch);
}

static void on_brain_stop_audio(void *ctx)
{
    (void)ctx;
    audio_pipeline_flush_playback();
}

static void on_brain_config(uint8_t volume, uint8_t brightness, void *ctx)
{
    (void)ctx;
    if (volume <= 100) {
        board_set_volume(volume);
    }
    if (brightness >= 10 && brightness <= 100) {
        board_set_brightness(brightness);
    }
}

static void on_wifi_ready(void *ctx)
{
    (void)ctx;
    ESP_LOGI(TAG, "Wi-Fi ready (%s); starting Brain link", wifi_link_ip());
    brain_link_cbs_t cbs = {
        .on_connected = on_brain_connected,
        .on_disconnected = on_brain_disconnected,
        .on_audio_down = on_brain_audio_down,
        .on_face_state = on_brain_face_state,
        .on_stop_audio = on_brain_stop_audio,
        .on_config = on_brain_config,
        .ctx = NULL,
    };
    brain_link_start(s_settings, &cbs);
}

esp_err_t body_fsm_start(grokbot_settings_t *settings)
{
    s_settings = settings;

    const esp_timer_create_args_t t_args = {
        .callback = on_sleep_timeout,
        .name = "body_sleep",
    };
    ESP_ERROR_CHECK(esp_timer_create(&t_args, &s_sleep_timer));

    audio_pipeline_cbs_t acbs = {
        .on_pcm = on_mic_pcm,
        .on_wake = on_local_wakeword,
        .ctx = NULL,
    };
    ESP_ERROR_CHECK(audio_pipeline_start(&acbs));

    return wifi_link_start(settings, on_wifi_ready, NULL);
}

void body_fsm_trigger_tap(void)
{
    trigger_wake_or_barge_in(GROK_WAKE_SRC_TOUCH);
}

void body_fsm_trigger_sleep(void)
{
    if (!s_awake) {
        return;
    }
    s_awake = false;
    audio_pipeline_flush_playback();
    brain_link_send_sleep();
    face_set_emotion(FACE_EMOTION_IDLE);
    ESP_LOGI(TAG, "Sleeping");
}

bool body_fsm_is_awake(void)
{
    return s_awake;
}

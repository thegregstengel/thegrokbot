#include "audio_pipeline.h"
#include "board.h"
#include "body_fsm.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "face.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "settings_store.h"
#include "touch_controls.h"

static const char *TAG = "grokbot";

static void on_touch_tap(void *ctx)
{
    (void)ctx;
    body_fsm_trigger_tap();
}

static void on_touch_long_press(void *ctx)
{
    (void)ctx;
    body_fsm_trigger_sleep();
}

void app_main(void)
{
    ESP_LOGI(TAG, "Physical GrokBot Body starting");

    ESP_ERROR_CHECK(settings_init());
    grokbot_settings_t settings;
    ESP_ERROR_CHECK(settings_load(&settings));

    ESP_ERROR_CHECK(board_init());
    board_set_brightness(settings.brightness);
    board_set_volume(settings.volume);

    ESP_ERROR_CHECK(face_init());

    touch_controls_cbs_t tcbs = {
        .on_tap = on_touch_tap,
        .on_long_press = on_touch_long_press,
        .ctx = NULL,
    };
    ESP_ERROR_CHECK(touch_controls_init(&tcbs));
    ESP_ERROR_CHECK(audio_pipeline_init());
    ESP_LOGI(TAG, "Heap after audio: internal=%u KB, psram=%u KB",
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
             (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));

#if CONFIG_GROKBOT_DEMO_FACE_ONLY
    ESP_LOGW(TAG, "Face-only demo mode");
    face_demo_cycle_start();
#else
    esp_err_t err = body_fsm_start(&settings);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Body start failed: %s", esp_err_to_name(err));
        face_set_emotion(FACE_EMOTION_CONCERNED);
    } else {
        ESP_LOGI(TAG, "Heap after body start: internal=%u KB, psram=%u KB",
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_INTERNAL) / 1024),
                 (unsigned)(heap_caps_get_free_size(MALLOC_CAP_SPIRAM) / 1024));
    }
#endif

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

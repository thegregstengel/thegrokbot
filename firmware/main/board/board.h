#pragma once

#include <stdbool.h>
#include "esp_codec_dev.h"
#include "esp_err.h"
#include "lvgl.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t board_init(void);
lv_display_t *board_display(void);
esp_codec_dev_handle_t board_speaker(void);
esp_codec_dev_handle_t board_microphone(void);
int board_mic_channel_count(void);
bool board_mic_has_aec_ref(void);

esp_err_t board_set_brightness(int percent);
esp_err_t board_set_volume(int percent);
int board_get_brightness(void);
int board_get_volume(void);

#ifdef __cplusplus
}
#endif

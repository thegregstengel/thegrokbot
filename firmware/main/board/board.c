#include "board.h"

#include "bsp/display.h"
#include "bsp/esp-bsp.h"
#include "esp_check.h"
#include "esp_log.h"

static const char *TAG = "board";

static lv_display_t *s_disp;
static esp_codec_dev_handle_t s_spk;
static esp_codec_dev_handle_t s_mic;
static int s_brightness = CONFIG_GROKBOT_DEFAULT_BRIGHTNESS;
static int s_volume = CONFIG_GROKBOT_DEFAULT_VOLUME;
static int s_mic_channels = 1;
static bool s_mic_has_ref = false;

esp_err_t board_init(void)
{
    ESP_LOGI(TAG, "Initializing ESP-VoCat board");

    ESP_RETURN_ON_ERROR(bsp_i2c_init(), TAG, "I2C init failed");
    ESP_RETURN_ON_ERROR(bsp_feature_enable(BSP_FEATURE_LCD, true), TAG, "LCD power failed");
    ESP_RETURN_ON_ERROR(bsp_feature_enable(BSP_FEATURE_SPEAKER, true), TAG, "Speaker power failed");
    ESP_RETURN_ON_ERROR(bsp_feature_enable(BSP_FEATURE_MIC, true), TAG, "Mic power failed");

    bsp_display_cfg_t cfg = {
        .lvgl_port_cfg = ESP_LVGL_PORT_INIT_CONFIG(),
        .buffer_size = BSP_LCD_H_RES * CONFIG_BSP_LCD_DRAW_BUF_HEIGHT,
        .double_buffer = false,
        .flags = {
            .buff_dma = true,
            .buff_spiram = false,
            .sw_rotate = false,
        },
    };
    cfg.lvgl_port_cfg.task_affinity = 1;
    s_disp = bsp_display_start_with_config(&cfg);
    ESP_RETURN_ON_FALSE(s_disp != NULL, ESP_FAIL, TAG, "Display start failed");

    ESP_RETURN_ON_ERROR(bsp_display_brightness_set(s_brightness), TAG, "Brightness failed");
    bsp_display_backlight_on();

    ESP_RETURN_ON_ERROR(bsp_audio_init(NULL), TAG, "Audio init failed");
    s_spk = bsp_audio_codec_speaker_init();
    s_mic = bsp_audio_codec_microphone_init();
    ESP_RETURN_ON_FALSE(s_spk && s_mic, ESP_FAIL, TAG, "Codec init failed");

    /* ES8311 mono playback stream. */
    esp_codec_dev_sample_info_t spk_fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = CONFIG_GROKBOT_SAMPLE_RATE_HZ,
        .mclk_multiple = 0,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_spk, &spk_fs), TAG, "Speaker open failed");
    ESP_RETURN_ON_ERROR(esp_codec_dev_set_out_vol(s_spk, s_volume), TAG, "Volume failed");

    /*
     * ESP-VoCat's bsp_audio_init(NULL) configures standard 2-channel I2S for
     * ES7210 (MIC1 + MIC2 dual array) and ES8311 (mono speaker). When ESP-SR is
     * enabled, open both mics in stereo so AFE receives dual-mic input plus the
     * software playback reference channel ("MMR").
     */
#if CONFIG_GROKBOT_USE_ESP_SR
    esp_codec_dev_sample_info_t mic_fs = {
        .bits_per_sample = 16,
        .channel = 2,
        .channel_mask = 0,
        .sample_rate = CONFIG_GROKBOT_SAMPLE_RATE_HZ,
        .mclk_multiple = 0,
    };
    esp_err_t mic_err = esp_codec_dev_open(s_mic, &mic_fs);
    if (mic_err == ESP_OK) {
        s_mic_channels = 2;
        s_mic_has_ref = true;
        ESP_LOGI(TAG, "ES7210 opened in dual-mic stereo mode (MIC1 + MIC2)");
    } else {
        ESP_LOGW(TAG, "Stereo mic open failed (%s); falling back to mono mic",
                 esp_err_to_name(mic_err));
        esp_codec_dev_sample_info_t mono_fs = {
            .bits_per_sample = 16,
            .channel = 1,
            .channel_mask = 0,
            .sample_rate = CONFIG_GROKBOT_SAMPLE_RATE_HZ,
            .mclk_multiple = 0,
        };
        ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_mic, &mono_fs), TAG, "Mono mic open failed");
        s_mic_channels = 1;
        s_mic_has_ref = false;
    }
#else
    esp_codec_dev_sample_info_t mono_fs = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = CONFIG_GROKBOT_SAMPLE_RATE_HZ,
        .mclk_multiple = 0,
    };
    ESP_RETURN_ON_ERROR(esp_codec_dev_open(s_mic, &mono_fs), TAG, "Mic open failed");
    s_mic_channels = 1;
    s_mic_has_ref = false;
#endif

    ESP_RETURN_ON_ERROR(esp_codec_dev_set_in_gain(s_mic, 36.0f), TAG, "Mic gain failed");

    led_indicator_handle_t leds[BSP_LED_NUM] = {0};
    if (bsp_led_indicator_create(leds, NULL, BSP_LED_NUM) == ESP_OK && leds[0]) {
        bsp_led_set(leds[0], true);
    }

    ESP_LOGI(TAG, "Board ready (display %dx%d, audio %d Hz, mic_ch=%d, aec_ref=%d)",
             BSP_LCD_H_RES, BSP_LCD_V_RES, CONFIG_GROKBOT_SAMPLE_RATE_HZ,
             s_mic_channels, s_mic_has_ref ? 1 : 0);
    return ESP_OK;
}

lv_display_t *board_display(void)
{
    return s_disp;
}

esp_codec_dev_handle_t board_speaker(void)
{
    return s_spk;
}

esp_codec_dev_handle_t board_microphone(void)
{
    return s_mic;
}

int board_mic_channel_count(void)
{
    return s_mic_channels;
}

bool board_mic_has_aec_ref(void)
{
    return s_mic_has_ref;
}

esp_err_t board_set_brightness(int percent)
{
    if (percent < 10) {
        percent = 10;
    }
    if (percent > 100) {
        percent = 100;
    }
    s_brightness = percent;
    return bsp_display_brightness_set(percent);
}

esp_err_t board_set_volume(int percent)
{
    if (percent < 0) {
        percent = 0;
    }
    if (percent > 100) {
        percent = 100;
    }
    s_volume = percent;
    if (!s_spk) {
        return ESP_ERR_INVALID_STATE;
    }
    return esp_codec_dev_set_out_vol(s_spk, percent);
}

int board_get_brightness(void)
{
    return s_brightness;
}

int board_get_volume(void)
{
    return s_volume;
}

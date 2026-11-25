#include "touch_controls.h"

#include <stdlib.h>

#include "audio_pipeline.h"
#include "board.h"
#include "bsp/esp-bsp.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "iot_button.h"
#include "lvgl.h"

static const char *TAG = "touch";

#define SWIPE_MIN_PX    40
#define LONG_PRESS_MS   650

static touch_controls_cbs_t s_cbs;
static button_handle_t s_btns[BSP_BUTTON_NUM];
static lv_point_t s_press_pt;
static int64_t s_press_us;

static void on_pressed(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    if (indev) {
        lv_indev_get_point(indev, &s_press_pt);
    }
    s_press_us = esp_timer_get_time();
}

static void on_released(lv_event_t *e)
{
    (void)e;
    lv_indev_t *indev = lv_indev_active();
    lv_point_t rel_pt = s_press_pt;
    if (indev) {
        lv_indev_get_point(indev, &rel_pt);
    }
    int dx = (int)(rel_pt.x - s_press_pt.x);
    int dy = (int)(rel_pt.y - s_press_pt.y);
    int adx = abs(dx);
    int ady = abs(dy);
    int64_t dt_ms = (esp_timer_get_time() - s_press_us) / 1000;

    if (adx >= SWIPE_MIN_PX || ady >= SWIPE_MIN_PX) {
        if (ady >= adx) {
            int delta = (dy < 0) ? 10 : -10;
            board_set_volume(board_get_volume() + delta);
            audio_pipeline_play_chime(false);
            ESP_LOGI(TAG, "swipe %s -> volume %d%%", dy < 0 ? "up" : "down", board_get_volume());
        } else {
            int delta = (dx > 0) ? 10 : -10;
            board_set_brightness(board_get_brightness() + delta);
            ESP_LOGI(TAG, "swipe %s -> brightness %d%%", dx > 0 ? "right" : "left",
                     board_get_brightness());
        }
        return;
    }

    if (dt_ms >= LONG_PRESS_MS) {
        ESP_LOGI(TAG, "screen long press (%lld ms)", (long long)dt_ms);
        if (s_cbs.on_long_press) {
            s_cbs.on_long_press(s_cbs.ctx);
        }
        return;
    }

    ESP_LOGI(TAG, "screen tap");
    if (s_cbs.on_tap) {
        s_cbs.on_tap(s_cbs.ctx);
    }
}

static void on_pad_click(void *btn, void *usr)
{
    (void)btn;
    (void)usr;
    ESP_LOGI(TAG, "capacitive pad tap");
    if (s_cbs.on_tap) {
        s_cbs.on_tap(s_cbs.ctx);
    }
}

static void on_pad_long_press(void *btn, void *usr)
{
    (void)btn;
    (void)usr;
    ESP_LOGI(TAG, "capacitive pad long press");
    if (s_cbs.on_long_press) {
        s_cbs.on_long_press(s_cbs.ctx);
    }
}

esp_err_t touch_controls_init(const touch_controls_cbs_t *cbs)
{
    if (cbs) {
        s_cbs = *cbs;
    }

    if (!bsp_display_lock(0)) {
        return ESP_FAIL;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_add_flag(scr, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(scr, on_pressed, LV_EVENT_PRESSED, NULL);
    lv_obj_add_event_cb(scr, on_released, LV_EVENT_RELEASED, NULL);

    bsp_display_unlock();

    if (bsp_iot_button_create(s_btns, NULL, BSP_BUTTON_NUM) == ESP_OK) {
        for (int i = 0; i < BSP_BUTTON_NUM; ++i) {
            if (s_btns[i]) {
                iot_button_register_cb(s_btns[i], BUTTON_SINGLE_CLICK, NULL, on_pad_click, NULL);
                iot_button_register_cb(s_btns[i], BUTTON_LONG_PRESS_START, NULL, on_pad_long_press,
                                       NULL);
            }
        }
    }

    ESP_LOGI(TAG, "Touch ready (tap=wake/barge-in, long-press=sleep, swipe=vol/brightness)");
    return ESP_OK;
}

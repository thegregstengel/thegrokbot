#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef void (*touch_action_cb_t)(void *ctx);

typedef struct {
    touch_action_cb_t on_tap;
    touch_action_cb_t on_long_press;
    void *ctx;
} touch_controls_cbs_t;

esp_err_t touch_controls_init(const touch_controls_cbs_t *cbs);

#ifdef __cplusplus
}
#endif

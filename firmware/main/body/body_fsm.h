#pragma once

#include <stdbool.h>
#include "esp_err.h"
#include "settings_store.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t body_fsm_start(grokbot_settings_t *settings);
void body_fsm_trigger_tap(void);
void body_fsm_trigger_sleep(void);
bool body_fsm_is_awake(void);

#ifdef __cplusplus
}
#endif

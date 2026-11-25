#pragma once

#include <stdint.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    FACE_EMOTION_IDLE = 0,
    FACE_EMOTION_LISTENING,
    FACE_EMOTION_FOCUSING,
    FACE_EMOTION_THINKING,
    FACE_EMOTION_SPEAKING,
    FACE_EMOTION_HAPPY,
    FACE_EMOTION_CURIOUS,
    FACE_EMOTION_CONCERNED,
    FACE_EMOTION_SLEEPY,
    FACE_EMOTION_COUNT
} face_emotion_t;

esp_err_t face_init(void);
void face_set_emotion(face_emotion_t emotion);
/* Sets emotion plus live voice energy (0..255) and optional gaze nudges (-128..127). */
void face_set_state(face_emotion_t emotion, uint8_t energy, int8_t yaw_nudge, int8_t pitch_nudge);
face_emotion_t face_get_emotion(void);
void face_demo_cycle_start(void);
const char *face_emotion_name(face_emotion_t emotion);

#ifdef __cplusplus
}
#endif

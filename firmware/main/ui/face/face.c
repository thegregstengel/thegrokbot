#include "face.h"
#include "face_render.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

#include "bsp/esp-bsp.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "lvgl.h"

static const char *TAG = "face";

typedef struct {
    float val;
    float vel;
} spring_t;

typedef struct {
    spring_t yaw;
    spring_t pitch;
    spring_t roll;
    spring_t sep;
    spring_t eye_w;
    spring_t eye_h;
    spring_t right_scale;
    spring_t open;
    spring_t happy_arc;
    spring_t right_happy_arc;
    spring_t top_lid;
    spring_t left_top_lid;
    spring_t bottom_lid;
    spring_t brow_tilt;
    spring_t glow;
    spring_t offset_y;
} face_springs_t;

static lv_obj_t *s_canvas;
static uint16_t *s_fb;
static uint16_t *s_bg_fb;
static lv_timer_t *s_anim_timer;
static lv_timer_t *s_demo_timer;
static face_rect_t s_prev_dirty;
static face_springs_t s_sp;

static volatile face_emotion_t s_emotion = FACE_EMOTION_IDLE;
static volatile uint8_t s_energy;
static volatile int8_t s_yaw_nudge;
static volatile int8_t s_pitch_nudge;

static float s_smooth_energy;
static int64_t s_t0_us;
static int64_t s_last_tick_us;
static int64_t s_wink_start_us;
static bool s_demo;

/* Deterministic lightweight PRNG for natural saccades and blinks. */
static uint32_t s_rng = 0x5A17C0DEu;
static float rand_unit(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return (float)((s_rng >> 8) & 0xFFFFu) * (1.0f / 65535.0f);
}

/* Natural micro-saccade and blink state. */
static float s_saccade_yaw;
static float s_saccade_pitch;
static float s_next_saccade_s = 2.4f;
static float s_blink_start_s = 3.2f;
static float s_blink_dur_s = 0.16f;
static bool s_double_blink;

static inline float clampf(float v, float lo, float hi)
{
    return (v < lo) ? lo : (v > hi ? hi : v);
}

static void spring_init(spring_t *s, float v)
{
    s->val = v;
    s->vel = 0.0f;
}

static void spring_step(spring_t *s, float target, float stiffness, float damping, float dt)
{
    float acc = (target - s->val) * stiffness - s->vel * damping;
    s->vel += acc * dt;
    s->val += s->vel * dt;
}

static face_render_params_t target_for(face_emotion_t e)
{
    face_render_params_t p = {
        .yaw = 0.45f,
        .pitch = 0.43f,
        .roll = 0.36f,
        .sep = 0.275f,
        .eye_w = 0.114f,
        .eye_h = 0.222f,
        .right_scale = 1.0f,
        .open = 1.0f,
        .happy_arc = 0.0f,
        .right_happy_arc = 0.0f,
        .top_lid = 0.0f,
        .left_top_lid = 0.0f,
        .bottom_lid = 0.0f,
        .brow_tilt = 0.0f,
        .glow = 0.40f,
        .offset_y = 0.0f,
        /* LVGL v9 LV_COLOR_FORMAT_RGB565 is native endian; esp_lvgl_port swaps on flush. */
        .swap_rgb565 = false,
    };

    switch (e) {
    case FACE_EMOTION_LISTENING:
        p.yaw = 0.0f;
        p.pitch = 0.08f;
        p.roll = 0.0f;
        p.open = 1.06f;
        p.glow = 0.65f;
        break;
    case FACE_EMOTION_FOCUSING:
        p.yaw = 0.0f;
        p.pitch = 0.10f;
        p.roll = 0.0f;
        p.eye_w = 0.130f;
        p.eye_h = 0.260f;
        p.sep = 0.265f;
        p.glow = 0.85f;
        break;
    case FACE_EMOTION_THINKING:
        p.yaw = 0.54f;
        p.pitch = 0.54f;
        p.roll = 0.30f;
        p.open = 0.88f;
        p.right_scale = 0.94f;
        p.glow = 0.45f;
        break;
    case FACE_EMOTION_SPEAKING:
        p.yaw = 0.14f;
        p.pitch = 0.20f;
        p.roll = 0.14f;
        p.glow = 0.72f;
        break;
    case FACE_EMOTION_HAPPY:
        p.yaw = 0.08f;
        p.pitch = 0.16f;
        p.roll = 0.08f;
        p.eye_w = 0.135f;
        p.eye_h = 0.210f;
        p.sep = 0.285f;
        p.happy_arc = 1.0f;
        p.glow = 0.72f;
        break;
    case FACE_EMOTION_CURIOUS:
        p.yaw = 0.34f;
        p.pitch = 0.32f;
        p.roll = 0.45f;
        p.sep = 0.290f;
        p.right_scale = 1.16f;
        p.open = 1.06f;
        p.glow = 0.55f;
        break;
    case FACE_EMOTION_CONCERNED:
        p.yaw = -0.14f;
        p.pitch = 0.10f;
        p.roll = -0.06f;
        p.top_lid = 0.28f;
        p.brow_tilt = 0.72f;
        p.glow = 0.22f;
        break;
    case FACE_EMOTION_SLEEPY:
        p.yaw = 0.18f;
        p.pitch = 0.16f;
        p.roll = 0.24f;
        p.top_lid = 0.74f;
        p.glow = 0.12f;
        break;
    case FACE_EMOTION_SURPRISED:
        /* Wide, tall, glowing minimalist pills with a startled upward lift. */
        p.yaw = 0.0f;
        p.pitch = 0.12f;
        p.roll = 0.0f;
        p.eye_w = 0.142f;
        p.eye_h = 0.254f;
        p.sep = 0.296f;
        p.open = 1.10f;
        p.glow = 0.92f;
        p.offset_y = -6.0f;
        break;
    case FACE_EMOTION_WINK:
        /* Left pill open and warm; right eye curves into a cheeky ^ wink arch. */
        p.yaw = 0.16f;
        p.pitch = 0.16f;
        p.roll = 0.22f;
        p.eye_w = 0.122f;
        p.eye_h = 0.224f;
        p.sep = 0.282f;
        p.right_happy_arc = 1.0f;
        p.glow = 0.72f;
        break;
    case FACE_EMOTION_SKEPTICAL:
        /* Flat half-lidded left eye paired with a wide raised right eye. */
        p.yaw = -0.20f;
        p.pitch = 0.14f;
        p.roll = -0.14f;
        p.sep = 0.282f;
        p.right_scale = 1.12f;
        p.left_top_lid = 0.46f;
        p.glow = 0.42f;
        break;
    case FACE_EMOTION_EXCITED:
        /* Radiant crescent-arch joy with extra glow and buoyant bounce. */
        p.yaw = 0.04f;
        p.pitch = 0.16f;
        p.roll = 0.05f;
        p.eye_w = 0.142f;
        p.eye_h = 0.232f;
        p.sep = 0.290f;
        p.happy_arc = 0.80f;
        p.glow = 0.95f;
        break;
    case FACE_EMOTION_AMUSED:
        /* Warm EVE-style cheek-raised smiling pills with a sly head tilt. */
        p.yaw = 0.26f;
        p.pitch = 0.22f;
        p.roll = 0.25f;
        p.eye_w = 0.124f;
        p.eye_h = 0.224f;
        p.sep = 0.282f;
        p.bottom_lid = 0.50f;
        p.glow = 0.68f;
        break;
    case FACE_EMOTION_IDLE:
    default:
        break;
    }

    return p;
}

static void init_springs(const face_render_params_t *p)
{
    spring_init(&s_sp.yaw, p->yaw);
    spring_init(&s_sp.pitch, p->pitch);
    spring_init(&s_sp.roll, p->roll);
    spring_init(&s_sp.sep, p->sep);
    spring_init(&s_sp.eye_w, p->eye_w);
    spring_init(&s_sp.eye_h, p->eye_h);
    spring_init(&s_sp.right_scale, p->right_scale);
    spring_init(&s_sp.open, p->open);
    spring_init(&s_sp.happy_arc, p->happy_arc);
    spring_init(&s_sp.right_happy_arc, p->right_happy_arc);
    spring_init(&s_sp.top_lid, p->top_lid);
    spring_init(&s_sp.left_top_lid, p->left_top_lid);
    spring_init(&s_sp.bottom_lid, p->bottom_lid);
    spring_init(&s_sp.brow_tilt, p->brow_tilt);
    spring_init(&s_sp.glow, p->glow);
    spring_init(&s_sp.offset_y, p->offset_y);
}

static float eval_blink(float t_s, face_emotion_t emotion)
{
    if (emotion == FACE_EMOTION_HAPPY || emotion == FACE_EMOTION_SLEEPY ||
        emotion == FACE_EMOTION_WINK || emotion == FACE_EMOTION_EXCITED) {
        return 1.0f;
    }

    if (t_s >= s_blink_start_s + s_blink_dur_s) {
        if (s_double_blink) {
            s_double_blink = false;
            s_blink_start_s = t_s + 0.14f;
            s_blink_dur_s = 0.14f;
        } else {
            s_blink_start_s = t_s + 2.4f + rand_unit() * 3.6f;
            s_blink_dur_s = 0.15f + rand_unit() * 0.05f;
            s_double_blink = (rand_unit() < 0.18f);
        }
        return 1.0f;
    }

    if (t_s < s_blink_start_s) {
        return 1.0f;
    }

    float u = (t_s - s_blink_start_s) / s_blink_dur_s;
    /* Crisp close (first 40%), soft reopen (last 60%). */
    float close = (u < 0.4f) ? (u / 0.4f) : (1.0f - (u - 0.4f) / 0.6f);
    close = close * close * (3.0f - 2.0f * close);
    return 1.0f - 0.90f * close;
}

static void update_saccades(float t_s, face_emotion_t emotion)
{
    if (emotion != FACE_EMOTION_IDLE && emotion != FACE_EMOTION_THINKING) {
        s_saccade_yaw = 0.0f;
        s_saccade_pitch = 0.0f;
        return;
    }

    if (t_s >= s_next_saccade_s) {
        if (emotion == FACE_EMOTION_IDLE) {
            s_saccade_yaw = (rand_unit() - 0.5f) * 0.28f;
            s_saccade_pitch = (rand_unit() - 0.5f) * 0.14f;
            s_next_saccade_s = t_s + 1.8f + rand_unit() * 2.6f;
        } else {
            /* Thinking: slight curious glances upward/sideways. */
            s_saccade_yaw = (rand_unit() - 0.35f) * 0.32f;
            s_saccade_pitch = (rand_unit() - 0.25f) * 0.16f;
            s_next_saccade_s = t_s + 0.9f + rand_unit() * 1.4f;
        }
    }
}

/* Native LVGL timer (25 fps): updates springs, renders dirty bbox, invalidates area. */
static void lv_anim_cb(lv_timer_t *timer)
{
    (void)timer;
    if (!s_fb || !s_canvas) {
        return;
    }

    int64_t now = esp_timer_get_time();
    float dt = s_last_tick_us ? (now - s_last_tick_us) / 1000000.0f : 0.04f;
    s_last_tick_us = now;
    if (dt > 0.06f) {
        dt = 0.04f;
    }
    float t_s = (now - s_t0_us) / 1000000.0f;

    face_emotion_t emotion = s_emotion;
    update_saccades(t_s, emotion);

    float target_energy = (float)s_energy / 255.0f;
    s_smooth_energy += (target_energy - s_smooth_energy) * (1.0f - expf(-dt * 20.0f));

    face_render_params_t tgt = target_for(emotion);
    tgt.yaw += s_saccade_yaw + ((float)s_yaw_nudge / 127.0f) * 0.35f;
    tgt.pitch += s_saccade_pitch + ((float)s_pitch_nudge / 127.0f) * 0.20f;

    if (emotion == FACE_EMOTION_SPEAKING) {
        /* Volume-preserving squash & stretch + playful nod synced to speech energy. */
        float e = s_smooth_energy;
        tgt.eye_h *= (1.0f + e * 0.20f);
        tgt.eye_w *= (1.0f - e * 0.06f);
        tgt.glow = clampf(0.55f + e * 0.45f, 0.0f, 1.0f);
        tgt.offset_y = sinf(t_s * 7.5f) * 1.8f - e * 8.0f;
        tgt.roll += sinf(t_s * 3.1f) * 0.05f * (0.4f + e);
    } else if (emotion == FACE_EMOTION_LISTENING) {
        /* Subtle calm breathing halo while waiting for your voice. */
        tgt.glow = 0.58f + 0.16f * sinf(t_s * 2.8f);
    } else if (emotion == FACE_EMOTION_SLEEPY) {
        tgt.offset_y = sinf(t_s * 1.4f) * 2.5f;
    } else if (emotion == FACE_EMOTION_EXCITED) {
        /* Buoyant joyful bounce + gentle rhythmic head sway. */
        float e = s_smooth_energy;
        tgt.offset_y = -fabsf(sinf(t_s * 6.5f)) * (7.0f + e * 4.0f);
        tgt.roll += sinf(t_s * 6.5f) * 0.10f;
    } else if (emotion == FACE_EMOTION_WINK) {
        /* Crisp ~0.48 s one-shot wink that naturally reopens into a warm smirk. */
        float wink_s = (now - s_wink_start_us) / 1000000.0f;
        if (wink_s >= 0.48f) {
            tgt.right_happy_arc = 0.0f;
            tgt.bottom_lid = 0.36f;
            tgt.roll = 0.14f;
        }
        if (s_smooth_energy > 0.04f) {
            float e = s_smooth_energy;
            tgt.eye_h *= (1.0f + e * 0.14f);
            tgt.offset_y -= e * 6.5f;
            tgt.glow = clampf(tgt.glow + e * 0.22f, 0.0f, 1.0f);
        }
    } else if (s_smooth_energy > 0.04f) {
        /* Allow any mood expression (surprised, skeptical, amused) to bounce while speaking. */
        float e = s_smooth_energy;
        tgt.eye_h *= (1.0f + e * 0.14f);
        tgt.offset_y -= e * 6.5f;
        tgt.glow = clampf(tgt.glow + e * 0.22f, 0.0f, 1.0f);
    }

    /* Slightly underdamped springs give playful Pixar-like settle on gaze changes. */
    spring_step(&s_sp.yaw, tgt.yaw, 95.0f, 14.5f, dt);
    spring_step(&s_sp.pitch, tgt.pitch, 95.0f, 14.5f, dt);
    spring_step(&s_sp.roll, tgt.roll, 85.0f, 14.0f, dt);
    spring_step(&s_sp.sep, tgt.sep, 110.0f, 17.0f, dt);
    spring_step(&s_sp.eye_w, tgt.eye_w, 130.0f, 18.0f, dt);
    spring_step(&s_sp.eye_h, tgt.eye_h, 130.0f, 18.0f, dt);
    spring_step(&s_sp.right_scale, tgt.right_scale, 110.0f, 17.0f, dt);
    spring_step(&s_sp.open, tgt.open, 140.0f, 20.0f, dt);
    spring_step(&s_sp.happy_arc, tgt.happy_arc, 120.0f, 18.0f, dt);
    spring_step(&s_sp.right_happy_arc, tgt.right_happy_arc, 140.0f, 19.0f, dt);
    spring_step(&s_sp.top_lid, tgt.top_lid, 90.0f, 16.0f, dt);
    spring_step(&s_sp.left_top_lid, tgt.left_top_lid, 110.0f, 17.0f, dt);
    spring_step(&s_sp.bottom_lid, tgt.bottom_lid, 110.0f, 17.0f, dt);
    spring_step(&s_sp.brow_tilt, tgt.brow_tilt, 110.0f, 17.0f, dt);
    spring_step(&s_sp.glow, tgt.glow, 90.0f, 16.0f, dt);
    spring_step(&s_sp.offset_y, tgt.offset_y, 140.0f, 17.0f, dt);

    float blink = eval_blink(t_s, emotion);

    face_render_params_t cur = tgt;
    cur.yaw = s_sp.yaw.val;
    cur.pitch = s_sp.pitch.val;
    cur.roll = s_sp.roll.val;
    cur.sep = s_sp.sep.val;
    cur.eye_w = s_sp.eye_w.val;
    cur.eye_h = s_sp.eye_h.val;
    cur.right_scale = s_sp.right_scale.val;
    cur.open = s_sp.open.val * blink;
    cur.happy_arc = clampf(s_sp.happy_arc.val, 0.0f, 1.0f);
    cur.right_happy_arc = clampf(s_sp.right_happy_arc.val, 0.0f, 1.0f);
    cur.top_lid = clampf(s_sp.top_lid.val, 0.0f, 1.0f);
    cur.left_top_lid = clampf(s_sp.left_top_lid.val, 0.0f, 1.0f);
    cur.bottom_lid = clampf(s_sp.bottom_lid.val, 0.0f, 1.0f);
    cur.brow_tilt = clampf(s_sp.brow_tilt.val, -1.0f, 1.0f);
    cur.glow = clampf(s_sp.glow.val, 0.0f, 1.0f);
    cur.offset_y = s_sp.offset_y.val;

    face_rect_t inv = s_prev_dirty;
    face_rect_t next_dirty = face_render_eyes(s_fb, s_bg_fb, &cur, &s_prev_dirty);
    if (inv.x1 >= inv.x0 && inv.y1 >= inv.y0) {
        if (next_dirty.x1 >= next_dirty.x0 && next_dirty.y1 >= next_dirty.y0) {
            if (next_dirty.x0 < inv.x0) inv.x0 = next_dirty.x0;
            if (next_dirty.y0 < inv.y0) inv.y0 = next_dirty.y0;
            if (next_dirty.x1 > inv.x1) inv.x1 = next_dirty.x1;
            if (next_dirty.y1 > inv.y1) inv.y1 = next_dirty.y1;
        }
    } else {
        inv = next_dirty;
    }
    s_prev_dirty = next_dirty;

    if (inv.x1 >= inv.x0 && inv.y1 >= inv.y0) {
        lv_area_t area = {
            .x1 = inv.x0,
            .y1 = inv.y0,
            .x2 = inv.x1,
            .y2 = inv.y1,
        };
        lv_obj_invalidate_area(s_canvas, &area);
    }
}

static void lv_demo_cb(lv_timer_t *timer)
{
    (void)timer;
    face_emotion_t next = (face_emotion_t)((s_emotion + 1) % FACE_EMOTION_COUNT);
    face_set_state(next, (next == FACE_EMOTION_SPEAKING) ? 190 : 0, 0, 0);
}

const char *face_emotion_name(face_emotion_t emotion)
{
    static const char *names[FACE_EMOTION_COUNT] = {
        "idle", "listening", "focusing", "thinking", "speaking",
        "happy", "curious", "concerned", "sleepy",
        "surprised", "wink", "skeptical", "excited", "amused",
    };
    if (emotion < 0 || emotion >= FACE_EMOTION_COUNT) {
        return "unknown";
    }
    return names[emotion];
}

esp_err_t face_init(void)
{
    size_t fb_bytes = FACE_RENDER_W * FACE_RENDER_H * sizeof(uint16_t);
    s_fb = heap_caps_calloc(1, fb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_bg_fb = heap_caps_calloc(1, fb_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_fb || !s_bg_fb) {
        ESP_LOGE(TAG, "Failed to allocate face framebuffers in PSRAM");
        return ESP_ERR_NO_MEM;
    }

    face_render_params_t initial = target_for(FACE_EMOTION_IDLE);
    init_springs(&initial);
    face_render_init_bg(s_bg_fb, initial.swap_rgb565);
    memcpy(s_fb, s_bg_fb, fb_bytes);
    s_prev_dirty = face_render_eyes(s_fb, s_bg_fb, &initial, NULL);

    if (!bsp_display_lock(0)) {
        return ESP_FAIL;
    }

    lv_obj_t *scr = lv_screen_active();
    lv_obj_remove_style_all(scr);
    lv_obj_set_style_bg_color(scr, lv_color_hex(0x000000), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    lv_obj_clear_flag(scr, LV_OBJ_FLAG_SCROLLABLE);

    s_canvas = lv_canvas_create(scr);
    lv_canvas_set_buffer(s_canvas, s_fb, FACE_RENDER_W, FACE_RENDER_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_center(s_canvas);
    lv_obj_clear_flag(s_canvas, LV_OBJ_FLAG_SCROLLABLE | LV_OBJ_FLAG_CLICKABLE);

    s_t0_us = esp_timer_get_time();
    s_last_tick_us = s_t0_us;
    /* 40 ms (25 fps) leaves ample CPU headroom for IDLE0, Wi-Fi, and ESP-SR. */
    s_anim_timer = lv_timer_create(lv_anim_cb, 40, NULL);

    bsp_display_unlock();

    ESP_LOGI(TAG, "GrokBot 3D spherical SDF face ready (360x360 PSRAM canvas, 25fps)");
    return ESP_OK;
}

void face_set_state(face_emotion_t emotion, uint8_t energy, int8_t yaw_nudge, int8_t pitch_nudge)
{
    if (emotion < 0 || emotion >= FACE_EMOTION_COUNT) {
        return;
    }
    face_emotion_t prev = s_emotion;
    s_emotion = emotion;
    s_energy = energy;
    s_yaw_nudge = yaw_nudge;
    s_pitch_nudge = pitch_nudge;
    if (prev != emotion) {
        if (emotion == FACE_EMOTION_WINK) {
            s_wink_start_us = esp_timer_get_time();
        }
        ESP_LOGI(TAG, "emotion -> %s", face_emotion_name(emotion));
    }
}

void face_set_emotion(face_emotion_t emotion)
{
    face_set_state(emotion, 0, 0, 0);
}

face_emotion_t face_get_emotion(void)
{
    return s_emotion;
}

void face_demo_cycle_start(void)
{
    if (s_demo) {
        return;
    }
    s_demo = true;
    if (bsp_display_lock(100)) {
        s_demo_timer = lv_timer_create(lv_demo_cb, 2500, NULL);
        bsp_display_unlock();
    }
    ESP_LOGI(TAG, "Face emotion demo cycling");
}

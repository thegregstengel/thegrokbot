#pragma once

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define FACE_RENDER_W 360
#define FACE_RENDER_H 360

typedef struct {
    /* 3D head orientation on the unit sphere (radians). */
    float yaw;         /* + look right */
    float pitch;       /* + look up */
    float roll;        /* + tilt CCW (\\ lean like reference) */

    /* Eye geometry in angular sphere units (radians). */
    float sep;         /* Half angular separation between eyes (default ~0.275) */
    float eye_w;       /* Half capsule width (default ~0.114) */
    float eye_h;       /* Half capsule height (default ~0.222) */
    float right_scale; /* Optional slight asymmetry (1.0 = equal) */

    /* Expressive eyelid & shape modifiers (0.0 = neutral). */
    float open;        /* 1.0 = fully open, 0.08 = closed blink */
    float happy_arc;   /* 0..1: morphs pill into joyful ^ ^ arch */
    float top_lid;     /* 0..1: sleepy/heavy upper eyelid drop */
    float brow_tilt;   /* -1..+1: + = concerned (outer down) */

    /* Subtle visual polish. */
    float glow;        /* 0..1: soft luminous aura around the eyes */
    float offset_y;    /* Vertical bounce in pixels (+ = down) */
    bool swap_rgb565;  /* Match panel endianness if not handled by flush cb */
} face_render_params_t;

typedef struct {
    int x0;
    int y0;
    int x1;
    int y1;
} face_rect_t;

/* Initializes internal lookup tables and renders the static 3D obsidian sphere base into `bg_fb`. */
void face_render_init_bg(uint16_t *bg_fb, bool swap_rgb565);

/*
 * Fast zero-libm per-pixel 3D spherical SDF eye renderer:
 *  - Restores `prev_dirty` from `bg_fb` in one fast memcpy loop.
 *  - Renders left and right eyes inside their individual tight bounding boxes.
 * Returns the current frame's combined eye bounding box.
 */
face_rect_t face_render_eyes(uint16_t *fb, const uint16_t *bg_fb, const face_render_params_t *p,
                             const face_rect_t *prev_dirty);

#ifdef __cplusplus
}
#endif

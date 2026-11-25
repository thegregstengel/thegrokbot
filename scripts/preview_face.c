#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include "../firmware/main/ui/face/face_render.h"

static void write_ppm(const char *path, const uint16_t *fb, int w, int h)
{
    FILE *f = fopen(path, "wb");
    if (!f) {
        perror("fopen");
        exit(1);
    }
    fprintf(f, "P6\n%d %d\n255\n", w, h);
    uint8_t rgb[3];
    for (int i = 0; i < w * h; ++i) {
        uint16_t c = fb[i];
        rgb[0] = (uint8_t)(((c >> 11) & 0x1F) * 255 / 31);
        rgb[1] = (uint8_t)(((c >> 5) & 0x3F) * 255 / 63);
        rgb[2] = (uint8_t)((c & 0x1F) * 255 / 31);
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

static face_render_params_t base_params(void)
{
    /* Matches docs/grokbot_ref_crop.png: chunky iconic GrokBot pills wrapping upper-right. */
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
        .top_lid = 0.0f,
        .brow_tilt = 0.0f,
        .glow = 0.40f,
        .offset_y = 0.0f,
        .swap_rgb565 = false,
    };
    return p;
}

int main(void)
{
    const int cols = 3, rows = 3;
    const int W = FACE_RENDER_W * cols;
    const int H = FACE_RENDER_H * rows;
    uint16_t *sheet = calloc((size_t)W * (size_t)H, sizeof(uint16_t));
    uint16_t *cell = calloc(FACE_RENDER_W * FACE_RENDER_H, sizeof(uint16_t));
    uint16_t *bg = calloc(FACE_RENDER_W * FACE_RENDER_H, sizeof(uint16_t));
    face_render_init_bg(bg, false);

    face_render_params_t poses[9];
    for (int i = 0; i < 9; ++i) {
        poses[i] = base_params();
    }

    /* 0: Idle */
    /* base_params */

    /* 1: Listening (attentive upright lock-on) */
    poses[1].yaw = 0.0f;
    poses[1].pitch = 0.08f;
    poses[1].roll = 0.0f;
    poses[1].open = 1.06f;
    poses[1].glow = 0.65f;

    /* 2: Focusing (heard you: swollen bright eyes) */
    poses[2].yaw = 0.0f;
    poses[2].pitch = 0.10f;
    poses[2].roll = 0.0f;
    poses[2].eye_w = 0.130f;
    poses[2].eye_h = 0.260f;
    poses[2].sep = 0.265f;
    poses[2].glow = 0.85f;

    /* 3: Thinking (glancing up-right thoughtfully) */
    poses[3].yaw = 0.54f;
    poses[3].pitch = 0.54f;
    poses[3].roll = 0.30f;
    poses[3].open = 0.88f;
    poses[3].right_scale = 0.94f;

    /* 4: Speaking (warm forward lean + lively energy stretch) */
    poses[4].yaw = 0.14f;
    poses[4].pitch = 0.20f;
    poses[4].roll = 0.14f;
    poses[4].eye_w = 0.108f;
    poses[4].eye_h = 0.248f;
    poses[4].offset_y = -6.0f;
    poses[4].glow = 0.78f;

    /* 5: Happy (joyful thick smile arches!) */
    poses[5].yaw = 0.08f;
    poses[5].pitch = 0.16f;
    poses[5].roll = 0.08f;
    poses[5].eye_w = 0.135f;
    poses[5].eye_h = 0.210f;
    poses[5].sep = 0.285f;
    poses[5].happy_arc = 1.0f;
    poses[5].glow = 0.72f;

    /* 6: Curious (playful head tilt + asymmetric wider right eye) */
    poses[6].yaw = 0.34f;
    poses[6].pitch = 0.32f;
    poses[6].roll = 0.45f;
    poses[6].sep = 0.290f;
    poses[6].right_scale = 1.16f;
    poses[6].open = 1.06f;

    /* 7: Concerned (softly rounded furrowed brows) */
    poses[7].yaw = -0.14f;
    poses[7].pitch = 0.10f;
    poses[7].roll = -0.06f;
    poses[7].top_lid = 0.28f;
    poses[7].brow_tilt = 0.72f;
    poses[7].glow = 0.22f;

    /* 8: Sleepy (cozy heavy drooping lids) */
    poses[8].yaw = 0.18f;
    poses[8].pitch = 0.16f;
    poses[8].roll = 0.24f;
    poses[8].top_lid = 0.74f;
    poses[8].glow = 0.12f;

    for (int i = 0; i < 9; ++i) {
        for (int k = 0; k < FACE_RENDER_W * FACE_RENDER_H; ++k) {
            cell[k] = bg[k];
        }
        face_render_eyes(cell, bg, &poses[i], NULL);
        int cx = (i % cols) * FACE_RENDER_W;
        int cy = (i / cols) * FACE_RENDER_H;
        for (int y = 0; y < FACE_RENDER_H; ++y) {
            for (int x = 0; x < FACE_RENDER_W; ++x) {
                sheet[(cy + y) * W + (cx + x)] = cell[y * FACE_RENDER_W + x];
            }
        }
    }

    write_ppm("/tmp/grokbot_face_sheet.ppm", sheet, W, H);
    free(bg);
    free(cell);
    free(sheet);
    return 0;
}

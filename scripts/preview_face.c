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
        .swap_rgb565 = false,
    };
    return p;
}

int main(int argc, char **argv)
{
    const char *out_path = (argc > 1) ? argv[1] : "/tmp/grokbot_face_sheet.ppm";
    const int cols = 5, rows = 3;
    const int total = 14;
    const int W = FACE_RENDER_W * cols;
    const int H = FACE_RENDER_H * rows;
    uint16_t *sheet = calloc((size_t)W * (size_t)H, sizeof(uint16_t));
    uint16_t *cell = calloc(FACE_RENDER_W * FACE_RENDER_H, sizeof(uint16_t));
    uint16_t *bg = calloc(FACE_RENDER_W * FACE_RENDER_H, sizeof(uint16_t));
    face_render_init_bg(bg, false);

    face_render_params_t poses[14];
    for (int i = 0; i < total; ++i) {
        poses[i] = base_params();
    }

    /* 0: Idle */
    /* 1: Listening */
    poses[1].yaw = 0.0f; poses[1].pitch = 0.08f; poses[1].roll = 0.0f;
    poses[1].open = 1.06f; poses[1].glow = 0.65f;

    /* 2: Focusing */
    poses[2].yaw = 0.0f; poses[2].pitch = 0.10f; poses[2].roll = 0.0f;
    poses[2].eye_w = 0.130f; poses[2].eye_h = 0.260f; poses[2].sep = 0.265f; poses[2].glow = 0.85f;

    /* 3: Thinking */
    poses[3].yaw = 0.54f; poses[3].pitch = 0.54f; poses[3].roll = 0.30f;
    poses[3].open = 0.88f; poses[3].right_scale = 0.94f;

    /* 4: Speaking */
    poses[4].yaw = 0.14f; poses[4].pitch = 0.20f; poses[4].roll = 0.14f;
    poses[4].eye_w = 0.108f; poses[4].eye_h = 0.248f; poses[4].offset_y = -6.0f; poses[4].glow = 0.78f;

    /* 5: Happy */
    poses[5].yaw = 0.08f; poses[5].pitch = 0.16f; poses[5].roll = 0.08f;
    poses[5].eye_w = 0.135f; poses[5].eye_h = 0.210f; poses[5].sep = 0.285f;
    poses[5].happy_arc = 1.0f; poses[5].glow = 0.72f;

    /* 6: Curious */
    poses[6].yaw = 0.34f; poses[6].pitch = 0.32f; poses[6].roll = 0.45f;
    poses[6].sep = 0.290f; poses[6].right_scale = 1.16f; poses[6].open = 1.06f;

    /* 7: Concerned */
    poses[7].yaw = -0.14f; poses[7].pitch = 0.10f; poses[7].roll = -0.06f;
    poses[7].top_lid = 0.28f; poses[7].brow_tilt = 0.72f; poses[7].glow = 0.22f;

    /* 8: Sleepy */
    poses[8].yaw = 0.18f; poses[8].pitch = 0.16f; poses[8].roll = 0.24f;
    poses[8].top_lid = 0.74f; poses[8].glow = 0.12f;

    /* 9: Surprised */
    poses[9].yaw = 0.0f; poses[9].pitch = 0.12f; poses[9].roll = 0.0f;
    poses[9].eye_w = 0.142f; poses[9].eye_h = 0.254f; poses[9].sep = 0.296f;
    poses[9].open = 1.10f; poses[9].glow = 0.92f; poses[9].offset_y = -6.0f;

    /* 10: Wink */
    poses[10].yaw = 0.16f; poses[10].pitch = 0.16f; poses[10].roll = 0.22f;
    poses[10].eye_w = 0.122f; poses[10].eye_h = 0.224f; poses[10].sep = 0.282f;
    poses[10].right_happy_arc = 1.0f; poses[10].glow = 0.72f;

    /* 11: Skeptical */
    poses[11].yaw = -0.20f; poses[11].pitch = 0.14f; poses[11].roll = -0.14f;
    poses[11].sep = 0.282f; poses[11].right_scale = 1.12f; poses[11].left_top_lid = 0.46f;
    poses[11].glow = 0.42f;

    /* 12: Excited */
    poses[12].yaw = 0.04f; poses[12].pitch = 0.16f; poses[12].roll = 0.05f;
    poses[12].eye_w = 0.142f; poses[12].eye_h = 0.232f; poses[12].sep = 0.290f;
    poses[12].happy_arc = 0.80f; poses[12].glow = 0.95f; poses[12].offset_y = -7.0f;

    /* 13: Amused */
    poses[13].yaw = 0.26f; poses[13].pitch = 0.22f; poses[13].roll = 0.25f;
    poses[13].eye_w = 0.124f; poses[13].eye_h = 0.224f; poses[13].sep = 0.282f;
    poses[13].bottom_lid = 0.50f; poses[13].glow = 0.68f;

    for (int i = 0; i < total; ++i) {
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

    write_ppm(out_path, sheet, W, H);
    free(bg);
    free(cell);
    free(sheet);
    return 0;
}

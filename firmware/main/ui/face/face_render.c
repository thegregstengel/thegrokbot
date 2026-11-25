#include "face_render.h"

#include <math.h>
#include <stddef.h>
#include <string.h>

#define SPHERE_CX 179.5f
#define SPHERE_CY 179.5f
#define SPHERE_R  178.0f
#define INV_R     (1.0f / SPHERE_R)

#define SQRT_LUT_N 1024

typedef struct {
    float m[9]; /* row-major 3x3 */
} mat3_t;

/* 4x4 Bayer ordered dithering matrix (0..15) to eliminate RGB565 dark-tone banding. */
static const uint8_t s_bayer4[4][4] = {
    { 0,  8,  2, 10},
    {12,  4, 14,  6},
    { 3, 11,  1,  9},
    {15,  7, 13,  5},
};

/* 4 KB LUT in internal RAM eliminates all libm sqrtf() calls inside pixel loops. */
static float s_sqrt_lut[SQRT_LUT_N + 1];
static bool s_lut_ready;

static inline float clampf(float v, float lo, float hi)
{
    return (v < lo) ? lo : (v > hi ? hi : v);
}

static inline float fast_sqrt01(float x)
{
    if (x <= 0.0f) {
        return 0.0f;
    }
    if (x >= 1.0f) {
        return 1.0f;
    }
    float f = x * (float)SQRT_LUT_N;
    int idx = (int)f;
    float frac = f - (float)idx;
    return s_sqrt_lut[idx] + (s_sqrt_lut[idx + 1] - s_sqrt_lut[idx]) * frac;
}

static inline float fast_asin_small(float s)
{
    /* Accurate to <0.001 rad for |s| <= 0.72 using 3 hardware FPU ops. */
    float s2 = s * s;
    return s * (1.0f + s2 * (0.166667f + 0.075f * s2));
}

static inline float smoothstepf(float e0, float e1, float x)
{
    float t = clampf((x - e0) / (e1 - e0), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

static inline uint16_t pack_rgb565_dither(float rf, float gf, float bf, int x, int y,
                                          bool swap_bytes)
{
    float d = ((float)s_bayer4[y & 3][x & 3] + 0.5f) * (1.0f / 16.0f) - 0.5f;
    int r = (int)(rf + d * 7.5f);
    int g = (int)(gf + d * 3.75f);
    int b = (int)(bf + d * 7.5f);

    if (r < 0) r = 0; else if (r > 255) r = 255;
    if (g < 0) g = 0; else if (g > 255) g = 255;
    if (b < 0) b = 0; else if (b > 255) b = 255;

    uint16_t c = (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
    if (swap_bytes) {
        c = (uint16_t)((c >> 8) | (c << 8));
    }
    return c;
}

static inline void unpack_rgb565(uint16_t c, bool swap_bytes, float *r, float *g, float *b)
{
    if (swap_bytes) {
        c = (uint16_t)((c >> 8) | (c << 8));
    }
    *r = (float)((c >> 8) & 0xF8);
    *g = (float)((c >> 3) & 0xFC);
    *b = (float)((c << 3) & 0xF8);
}

static mat3_t make_forward_mat(float yaw, float pitch, float roll)
{
    float cy = cosf(yaw),   sy = sinf(yaw);
    float cp = cosf(pitch), sp = sinf(pitch);
    float cr = cosf(roll),  sr = sinf(roll);

    float a00 = cr,        a01 = -sr,       a02 = 0.0f;
    float a10 = cp * sr,   a11 = cp * cr,   a12 = sp;
    float a20 = -sp * sr,  a21 = -sp * cr,  a22 = cp;

    mat3_t M;
    M.m[0] = cy * a00 + sy * a20;
    M.m[1] = cy * a01 + sy * a21;
    M.m[2] = cy * a02 + sy * a22;

    M.m[3] = a10;
    M.m[4] = a11;
    M.m[5] = a12;

    M.m[6] = -sy * a00 + cy * a20;
    M.m[7] = -sy * a01 + cy * a21;
    M.m[8] = -sy * a02 + cy * a22;
    return M;
}

static mat3_t transpose_mat(const mat3_t *M)
{
    mat3_t T;
    T.m[0] = M->m[0]; T.m[1] = M->m[3]; T.m[2] = M->m[6];
    T.m[3] = M->m[1]; T.m[4] = M->m[4]; T.m[5] = M->m[7];
    T.m[6] = M->m[2]; T.m[7] = M->m[5]; T.m[8] = M->m[8];
    return T;
}

static inline void shade_sphere_bg(float sx, float sy, float sz, float *r, float *g, float *b)
{
    float ndotl = clampf(-0.40f * sx + 0.50f * sy + 0.76f * sz, 0.0f, 1.0f);
    float n2 = ndotl * ndotl;
    float spec = n2 * n2;
    float rim = 1.0f - sz;
    float fresnel = rim * rim * 0.35f;

    float base = 2.0f + 9.5f * spec + 6.5f * fresnel;
    *r = base * 0.96f;
    *g = base * 0.99f;
    *b = base * 1.06f;
}

static inline float sd_pill_fast(float dx, float dy, float half_w, float half_h)
{
    float r = half_w;
    float seg = half_h - r;
    if (seg < 0.0f) {
        seg = 0.0f;
        r = half_h;
    }
    float ay = fabsf(dy) - seg;
    if (ay < 0.0f) {
        ay = 0.0f;
    }
    return fast_sqrt01(dx * dx + ay * ay) - r;
}

static inline float sd_happy_arch_fast(float dx, float dy, float half_w)
{
    float arc_r = half_w * 1.18f;
    float stroke = half_w * 0.42f;
    float cy = -arc_r * 0.52f;
    float py = dy - cy;
    float px = fabsf(dx);

    const float max_sin = 0.90f;
    const float max_cos = 0.436f;

    if (py * max_sin >= px * max_cos) {
        return fabsf(fast_sqrt01(px * px + py * py) - arc_r) - stroke;
    }
    float ex = px - arc_r * max_sin;
    float ey = py - arc_r * max_cos;
    return fast_sqrt01(ex * ex + ey * ey) - stroke;
}

static inline float eval_single_eye_sdf(float u, float v, float side, const face_render_params_t *p)
{
    float scale = (side > 0.0f) ? p->right_scale : 1.0f;
    float hw = p->eye_w * scale;
    float hh = p->eye_h * scale * clampf(p->open, 0.08f, 1.6f);

    float dx = u - side * p->sep;
    float dy = v;

    /* Fast reject outside eye + glow envelope (avoids sqrt/blend on ~60% of bbox). */
    float lim_x = hw * 1.50f + 0.14f;
    float lim_y = hh + 0.15f;
    if (fabsf(dx) > lim_x || fabsf(dy) > lim_y) {
        return 1.0f;
    }

    float d = sd_pill_fast(dx, dy, hw, hh);

    float arch = p->happy_arc;
    if (side > 0.0f && p->right_happy_arc > arch) {
        arch = p->right_happy_arc;
    }
    if (arch > 0.01f) {
        float d_arch = sd_happy_arch_fast(dx, dy, hw);
        d = d * (1.0f - arch) + d_arch * arch;
    }

    float lid = p->top_lid;
    if (side < 0.0f && p->left_top_lid > lid) {
        lid = p->left_top_lid;
    }
    if (lid > 0.01f || fabsf(p->brow_tilt) > 0.01f) {
        float lid_y = hh * (1.08f - 1.58f * lid);
        lid_y -= p->brow_tilt * (dx * side) * 0.52f;
        float d_lid = dy - lid_y;
        const float k = 0.034f;
        float h = clampf(0.5f + 0.5f * (d_lid - d) * (1.0f / k), 0.0f, 1.0f);
        d = (d * (1.0f - h) + d_lid * h) + k * h * (1.0f - h);
    }

    /* Soft curved lower cheek lift (EVE-style warm amused smile). */
    if (p->bottom_lid > 0.01f) {
        float norm_x = dx / (hw + 0.001f);
        float cheek_curve = 0.22f * hh * (1.0f - norm_x * norm_x);
        float bot_y = -hh * (1.12f - 1.38f * p->bottom_lid) + cheek_curve * p->bottom_lid;
        float d_bot = bot_y - dy;
        const float k = 0.038f;
        float h = clampf(0.5f + 0.5f * (d_bot - d) * (1.0f / k), 0.0f, 1.0f);
        d = (d * (1.0f - h) + d_bot * h) + k * h * (1.0f - h);
    }

    return d;
}

static face_rect_t compute_single_eye_bbox(const mat3_t *M, float side,
                                           const face_render_params_t *p)
{
    int min_x = FACE_RENDER_W - 1;
    int min_y = FACE_RENDER_H - 1;
    int max_x = 0;
    int max_y = 0;
    bool any = false;

    float scale = (side > 0.0f) ? p->right_scale : 1.0f;
    float pad_u = p->eye_w * scale * 1.40f + 0.06f;
    float pad_v = p->eye_h * scale * clampf(p->open, 0.2f, 1.6f) + 0.06f;
    float cu = side * p->sep;

    for (int iu = -1; iu <= 1; ++iu) {
        for (int iv = -1; iv <= 1; ++iv) {
            float u = cu + (float)iu * pad_u;
            float v = (float)iv * pad_v;
            float cv = cosf(v);
            float qx = sinf(u) * cv;
            float qy = sinf(v);
            float qz = cosf(u) * cv;

            float px = M->m[0] * qx + M->m[1] * qy + M->m[2] * qz;
            float py = M->m[3] * qx + M->m[4] * qy + M->m[5] * qz;
            float pz = M->m[6] * qx + M->m[7] * qy + M->m[8] * qz;
            if (pz < -0.05f) {
                continue;
            }
            int sx = (int)lroundf(SPHERE_CX + px * SPHERE_R);
            int sy = (int)lroundf(SPHERE_CY + p->offset_y - py * SPHERE_R);
            if (sx < min_x) min_x = sx;
            if (sx > max_x) max_x = sx;
            if (sy < min_y) min_y = sy;
            if (sy > max_y) max_y = sy;
            any = true;
        }
    }

    if (!any) {
        face_rect_t empty = {0, 0, -1, -1};
        return empty;
    }

    int margin = (p->glow > 0.05f) ? 18 : 8;
    face_rect_t r = {
        .x0 = (min_x - margin < 0) ? 0 : (min_x - margin),
        .y0 = (min_y - margin < 0) ? 0 : (min_y - margin),
        .x1 = (max_x + margin >= FACE_RENDER_W) ? (FACE_RENDER_W - 1) : (max_x + margin),
        .y1 = (max_y + margin >= FACE_RENDER_H) ? (FACE_RENDER_H - 1) : (max_y + margin),
    };
    return r;
}

static void render_eye_box(uint16_t *fb, const uint16_t *bg_fb, const mat3_t *M_inv,
                           float side, const face_rect_t *box, const face_render_params_t *p)
{
    if (box->x1 < box->x0 || box->y1 < box->y0) {
        return;
    }

    const float m0 = M_inv->m[0], m1 = M_inv->m[1], m2 = M_inv->m[2];
    const float m3 = M_inv->m[3], m4 = M_inv->m[4], m5 = M_inv->m[5];
    const float m6 = M_inv->m[6], m7 = M_inv->m[7], m8 = M_inv->m[8];
    const float glow_amt = clampf(p->glow, 0.0f, 1.0f);
    const bool swap = p->swap_rgb565;

    for (int y = box->y0; y <= box->y1; ++y) {
        float sy_eye = (SPHERE_CY + p->offset_y - (float)y) * INV_R;
        float sy_eye2 = sy_eye * sy_eye;
        uint16_t *row = fb + y * FACE_RENDER_W;
        const uint16_t *bg_row = bg_fb + y * FACE_RENDER_W;

        for (int x = box->x0; x <= box->x1; ++x) {
            float sx = ((float)x - SPHERE_CX) * INV_R;
            float r2 = sx * sx + sy_eye2;
            if (r2 >= 0.996f) {
                continue;
            }

            float sz = fast_sqrt01(1.0f - r2);
            float qx = m0 * sx + m1 * sy_eye + m2 * sz;
            float qy = m3 * sx + m4 * sy_eye + m5 * sz;
            float qz = m6 * sx + m7 * sy_eye + m8 * sz;

            if (qz <= 0.08f) {
                continue;
            }

            /* Fast spherical angular coordinates without libm atan2f/asinf. */
            float v = fast_asin_small(clampf(qy, -0.75f, 0.75f));
            float sin_u = clampf(qx * (1.0f + 0.5f * qy * qy), -0.75f, 0.75f);
            float u = fast_asin_small(sin_u);

            float d = eval_single_eye_sdf(u, v, side, p);
            if (d >= 0.13f) {
                continue;
            }

            float inv_sz = (sz > 0.25f) ? (1.0f / sz) : 4.0f;
            float aa = (1.35f * INV_R) * inv_sz;
            float eye_alpha = 1.0f - smoothstepf(-aa, aa, d);

            float halo = 0.0f;
            if (glow_amt > 0.01f && d > 0.0f && d < 0.13f) {
                float g = 1.0f - (d * (1.0f / 0.13f));
                halo = g * g * g * 0.22f * glow_amt;
            }

            if (eye_alpha <= 0.002f && halo <= 0.002f) {
                continue;
            }

            float br, bg, bb;
            /* Blend against current pixel so overlapping halos between eyes compose smoothly. */
            unpack_rgb565(row[x] ? row[x] : bg_row[x], swap, &br, &bg, &bb);

            float edge_dim = 0.92f + 0.08f * sz;
            float eye_r = 253.0f * edge_dim;
            float eye_g = 254.0f * edge_dim;
            float eye_b = 255.0f * edge_dim;

            float out_r = br * (1.0f - eye_alpha) + eye_r * eye_alpha + halo * 210.0f;
            float out_g = bg * (1.0f - eye_alpha) + eye_g * eye_alpha + halo * 226.0f;
            float out_b = bb * (1.0f - eye_alpha) + eye_b * eye_alpha + halo * 255.0f;

            row[x] = pack_rgb565_dither(out_r, out_g, out_b, x, y, swap);
        }
    }
}

void face_render_init_bg(uint16_t *bg_fb, bool swap_rgb565)
{
    if (!s_lut_ready) {
        for (int i = 0; i <= SQRT_LUT_N; ++i) {
            s_sqrt_lut[i] = sqrtf((float)i / (float)SQRT_LUT_N);
        }
        s_lut_ready = true;
    }

    for (int y = 0; y < FACE_RENDER_H; ++y) {
        float sy = (SPHERE_CY - (float)y) * INV_R;
        float sy2 = sy * sy;
        uint16_t *row = bg_fb + y * FACE_RENDER_W;
        for (int x = 0; x < FACE_RENDER_W; ++x) {
            float sx = ((float)x - SPHERE_CX) * INV_R;
            float r2 = sx * sx + sy2;
            if (r2 >= 1.0f) {
                row[x] = 0;
            } else {
                float sz = fast_sqrt01(1.0f - r2);
                float r, g, b;
                shade_sphere_bg(sx, sy, sz, &r, &g, &b);
                row[x] = pack_rgb565_dither(r, g, b, x, y, swap_rgb565);
            }
        }
    }
}

face_rect_t face_render_eyes(uint16_t *fb, const uint16_t *bg_fb, const face_render_params_t *p,
                             const face_rect_t *prev_dirty)
{
    /* Step 1: Fast restore of previous dirty rect from pre-rendered obsidian background. */
    if (prev_dirty && prev_dirty->x1 >= prev_dirty->x0 && prev_dirty->y1 >= prev_dirty->y0) {
        size_t span_bytes = (size_t)(prev_dirty->x1 - prev_dirty->x0 + 1) * sizeof(uint16_t);
        for (int y = prev_dirty->y0; y <= prev_dirty->y1; ++y) {
            memcpy(fb + y * FACE_RENDER_W + prev_dirty->x0,
                   bg_fb + y * FACE_RENDER_W + prev_dirty->x0,
                   span_bytes);
        }
    }

    mat3_t M_fwd = make_forward_mat(p->yaw, p->pitch, p->roll);
    mat3_t M_inv = transpose_mat(&M_fwd);

    face_rect_t box_l = compute_single_eye_bbox(&M_fwd, -1.0f, p);
    face_rect_t box_r = compute_single_eye_bbox(&M_fwd,  1.0f, p);

    render_eye_box(fb, bg_fb, &M_inv, -1.0f, &box_l, p);
    render_eye_box(fb, bg_fb, &M_inv,  1.0f, &box_r, p);

    /* Combine both boxes into one dirty bounding rect for LVGL invalidation. */
    face_rect_t combined = {FACE_RENDER_W - 1, FACE_RENDER_H - 1, 0, 0};
    bool has_any = false;
    if (box_l.x1 >= box_l.x0 && box_l.y1 >= box_l.y0) {
        combined = box_l;
        has_any = true;
    }
    if (box_r.x1 >= box_r.x0 && box_r.y1 >= box_r.y0) {
        if (!has_any) {
            combined = box_r;
        } else {
            if (box_r.x0 < combined.x0) combined.x0 = box_r.x0;
            if (box_r.y0 < combined.y0) combined.y0 = box_r.y0;
            if (box_r.x1 > combined.x1) combined.x1 = box_r.x1;
            if (box_r.y1 > combined.y1) combined.y1 = box_r.y1;
        }
        has_any = true;
    }
    if (!has_any) {
        face_rect_t empty = {0, 0, -1, -1};
        return empty;
    }
    return combined;
}

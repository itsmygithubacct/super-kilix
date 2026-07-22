/* All drawing: the three-canvas pipeline and the file-static primitive
 * wrappers.  render.c is the only module that includes soft_raster.h.
 *
 * Pipeline: the fixed 256x240 `logical` canvas is composited each frame, then
 * sr_scale_canvas() blows it up onto the terminal-sized `screen`, which
 * sr_pack_rgba() serialises into the RGBA `framebuffer` term.c presents.  Every
 * game-facing draw goes through a wrapper that adds the global offset_x/offset_y
 * (the M7 screen shake) and implicitly targets `logical`, so simulation code
 * never touches a canvas directly.
 *
 * draw_kilix reuses the established Kilix silhouette, palette, and gait model
 * from the sibling game (the same character across the family, per the visual
 * identity bible); it draws nothing derived from any prior platformer. */
#include "super_kilix.h"
#include "soft_raster.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

static sr_canvas logical;
static sr_canvas backdrop;
static sr_canvas screen;
static uint8_t *framebuffer;
static int screen_width, screen_height;
static int offset_x, offset_y;

/* ------------------------------------------------------------ hash helpers */

/* A stateless integer hash for cosmetic jitter (starfield placement, later the
 * shake).  It never feeds the simulation, so it can never desync a replay. */
static uint32_t vhash(uint32_t value)
{
    value ^= value >> 16; value *= 0x7feb352du;
    value ^= value >> 15; value *= 0x846ca68bu;
    return value ^ (value >> 16);
}

static float unit_hash(uint32_t value)
{
    return (float)(vhash(value) >> 8) * (1.0f / 16777216.0f);
}

/* ------------------------------------------------ primitive wrappers (logical) */

static void rect(float x, float y, float w, float h, uint32_t color, float alpha)
{
    sr_fill_rect(&logical, x + (float)offset_x, y + (float)offset_y, w, h, color, alpha);
}

static void outline(float x, float y, float w, float h, float lw,
                    uint32_t color, float alpha)
{
    sr_stroke_rect(&logical, x + (float)offset_x, y + (float)offset_y, w, h, lw, color, alpha);
}

static void circle(float x, float y, float radius, uint32_t color, float alpha)
{
    sr_fill_circle(&logical, x + (float)offset_x, y + (float)offset_y, radius, color, alpha);
}

static void ellipse(float x, float y, float rx, float ry, uint32_t color, float alpha)
{
    sr_fill_ellipse(&logical, x + (float)offset_x, y + (float)offset_y, rx, ry, color, alpha);
}

static void ring(float x, float y, float radius, float width, uint32_t color, float alpha)
{
    sr_ring(&logical, x + (float)offset_x, y + (float)offset_y, radius, width, color, alpha);
}

static void line(float x0, float y0, float x1, float y1, float width,
                 uint32_t color, float alpha)
{
    sr_line(&logical, x0 + (float)offset_x, y0 + (float)offset_y,
            x1 + (float)offset_x, y1 + (float)offset_y, width, color, alpha, 0, 0);
}

static void triangle(float x0, float y0, float x1, float y1, float x2, float y2,
                     uint32_t color, float alpha)
{
    sr_fill_triangle(&logical, x0 + (float)offset_x, y0 + (float)offset_y,
                     x1 + (float)offset_x, y1 + (float)offset_y,
                     x2 + (float)offset_x, y2 + (float)offset_y, color, alpha);
}

static void text(float x, float y, const char *value, uint32_t color, int scale)
{
    sr_text(&logical, x + (float)offset_x, y + (float)offset_y, value, color, 1.0f, scale);
}

static void text_shadow(float x, float y, const char *value, uint32_t color, int scale)
{
    sr_text_shadow(&logical, x + (float)offset_x, y + (float)offset_y, value, color, 1.0f, scale);
}

static void text_center(float x, float y, const char *value, uint32_t color, int scale)
{
    sr_text_center(&logical, x + (float)offset_x, y + (float)offset_y, value, color, 1.0f, scale);
}

static void diamond(float cx, float cy, float rx, float ry, uint32_t color, float alpha)
{
    triangle(cx, cy - ry, cx + rx, cy, cx, cy + ry, color, alpha);
    triangle(cx, cy - ry, cx, cy + ry, cx - rx, cy, color, alpha);
}

static void shape_mark(float cx, float cy, int family, uint32_t color, float alpha)
{
    if (family == 0) ring(cx, cy, 3.0f, 1.2f, color, alpha);
    else if (family == 1) {
        line(cx, cy - 3, cx + 3, cy + 3, 1, color, alpha);
        line(cx + 3, cy + 3, cx - 3, cy + 3, 1, color, alpha);
        line(cx - 3, cy + 3, cx, cy - 3, 1, color, alpha);
    } else outline(cx - 3, cy - 3, 6, 6, 1, color, alpha);
}

/* ---------------------------------------------------------------- backdrop */

/* A star-vault sky: a vertical gradient with an edge vignette and a scatter of
 * fixed motes.  Rebuilt once at init because it never changes within a scene. */
static void build_backdrop(void)
{
    for (int y = 0; y < LOGICAL_H; y++) {
        float v = (float)y / (float)(LOGICAL_H - 1);
        uint32_t color = v < 0.55f
            ? sr_mix(0x030712, 0x11112b, v / 0.55f)
            : sr_mix(0x11112b, 0x240f2b, (v - 0.55f) / 0.45f);
        for (int x = 0; x < LOGICAL_W; x++) {
            float edge = fabsf((float)x / (float)(LOGICAL_W - 1) - 0.5f) * 2.0f;
            backdrop.px[y * LOGICAL_W + x] = 0xff000000u |
                sr_scale_rgb(color, 1.0f - edge * 0.33f);
        }
    }
    for (int i = 0; i < 150; i++) {
        int x = (int)(unit_hash(1000u + (uint32_t)i * 7u) * (float)LOGICAL_W);
        int y = (int)(unit_hash(2000u + (uint32_t)i * 11u) * (float)LOGICAL_H);
        uint32_t c = (i % 5 == 0) ? 0x67e8f9 : (i % 7 == 0) ? 0xf0abfc : 0x94a3b8;
        sr_px(&backdrop, x, y, c);
        if (i % 23 == 0) sr_px(&backdrop, x + 1, y, c);
    }
}

/* --------------------------------------------------------------- lifecycle */

bool render_init(int width, int height)
{
    memset(&logical, 0, sizeof logical);
    memset(&backdrop, 0, sizeof backdrop);
    memset(&screen, 0, sizeof screen);
    if (!sr_canvas_init(&logical, LOGICAL_W, LOGICAL_H) ||
        !sr_canvas_init(&backdrop, LOGICAL_W, LOGICAL_H)) {
        render_shutdown();
        return false;
    }
    build_backdrop();
    return render_resize(width, height);
}

bool render_resize(int width, int height)
{
    if (width <= 0 || height <= 0) return false;
    sr_canvas_free(&screen);
    free(framebuffer);
    framebuffer = NULL;
    if (!sr_canvas_init(&screen, width, height)) return false;
    framebuffer = malloc((size_t)width * (size_t)height * 4u);
    if (!framebuffer) { sr_canvas_free(&screen); return false; }
    screen_width = width;
    screen_height = height;
    return true;
}

void render_shutdown(void)
{
    sr_canvas_free(&logical);
    sr_canvas_free(&backdrop);
    sr_canvas_free(&screen);
    free(framebuffer);
    framebuffer = NULL;
    screen_width = screen_height = 0;
}

uint8_t *render_fb(void) { return framebuffer; }

/* ------------------------------------------------------------- draw_kilix */

/* The established Kilix silhouette, palette, and gait model.  Every implicit
 * narrowing in the ported gait math is spelled out with float literals and
 * casts so the module builds clean under the family's warning set. */
static void draw_kilix(float x, float y, float scale, int facing,
                       bool thrust, bool phase, float gait, float animation)
{
    float flip = facing >= 0 ? 1.0f : -1.0f;
    uint32_t orange = 0xf97316, dark = 0x9a3412;
    float wave = sinf(animation);
    float step = wave * gait;
    float bob = fabsf(wave) * 0.55f * gait;
    float body_y = y - bob * scale;
    /* micro-thruster behind the torso */
    rect(x + (flip > 0 ? -1.0f : 8.0f) * scale, body_y + 6.0f * scale,
         4.0f * scale, 7.0f * scale, 0x6d28d9, 1);
    rect(x + (flip > 0 ? 0.0f : 9.0f) * scale, body_y + 7.0f * scale,
         2.0f * scale, 4.0f * scale, 0xa78bfa, 1);
    if (thrust) {
        triangle(x + (flip > 0 ? 0.0f : 11.0f) * scale, body_y + 12.0f * scale,
                 x + (flip > 0 ? 2.0f : 9.0f) * scale, body_y + 12.0f * scale,
                 x + (flip > 0 ? 1.0f : 10.0f) * scale,
                 body_y + (16.0f + sinf(animation * 2.0f) * 1.5f) * scale, 0xfb923c, 0.95f);
        circle(x + (flip > 0 ? 1.0f : 10.0f) * scale, body_y + 15.0f * scale,
               1.2f * scale, 0xfef08a, 0.9f);
    }
    /* tail remains readable even at one-cell gameplay scale */
    line(x + (flip > 0 ? 4.0f : 7.0f) * scale, body_y + 10.0f * scale,
         x + (flip > 0 ? -1.0f : 12.0f) * scale,
         body_y + (8.0f + sinf(animation * 0.72f) * (1.3f + gait)) * scale,
         1.4f * scale, orange, 1);
    ellipse(x + 5.5f * scale, body_y + 9.5f * scale, 4.3f * scale, 5.0f * scale,
            dark, 1);
    rect(x + 2.0f * scale, body_y + 7.0f * scale, 7.0f * scale, 6.0f * scale, orange, 1);
    /* Arms swing opposite the boots, giving the one-cell sprite a readable
     * gait rather than merely sliding its whole silhouette. */
    line(x + (flip > 0 ? 8.0f : 3.0f) * scale, body_y + 8.0f * scale,
         x + (flip > 0 ? 9.0f + step : 2.0f - step) * scale,
         body_y + (11.0f - step * 0.7f) * scale, 1.5f * scale, 0xfdba74, 1);
    circle(x + 5.5f * scale, body_y + 5.5f * scale, 4.5f * scale, orange, 1);
    triangle(x + 2.0f * scale, body_y + 4.0f * scale, x + 2.5f * scale, body_y,
             x + 5.0f * scale, body_y + 3.0f * scale, orange, 1);
    triangle(x + 6.0f * scale, body_y + 3.0f * scale, x + 9.0f * scale, body_y,
             x + 9.2f * scale, body_y + 5.0f * scale, orange, 1);
    triangle(x + 2.7f * scale, body_y + 3.4f * scale,
             x + 3.0f * scale, body_y + 1.4f * scale,
             x + 4.2f * scale, body_y + 3.1f * scale, 0xfda4af, 0.9f);
    triangle(x + 7.0f * scale, body_y + 3.1f * scale,
             x + 8.4f * scale, body_y + 1.2f * scale,
             x + 8.7f * scale, body_y + 3.7f * scale, 0xfda4af, 0.9f);
    rect(x + (flip > 0 ? 5.0f : 2.2f) * scale, body_y + 4.0f * scale,
         4.0f * scale, 2.4f * scale, 0x083344, 1);
    rect(x + (flip > 0 ? 6.0f : 2.7f) * scale, body_y + 4.4f * scale,
         2.6f * scale, 0.8f * scale, 0x67e8f9, 1);
    float left_lift = fmaxf(0.0f, step) * 1.5f;
    float right_lift = fmaxf(0.0f, -step) * 1.5f;
    rect(x + (2.2f + step * 0.75f) * scale,
         body_y + (12.0f - left_lift) * scale, 3.3f * scale, 2.6f * scale, 0x22d3ee, 1);
    rect(x + (6.3f - step * 0.75f) * scale,
         body_y + (12.0f - right_lift) * scale, 3.3f * scale, 2.6f * scale, 0x22d3ee, 1);
    if (phase) {
        ring(x + 5.5f * scale, body_y + 7.5f * scale, 7.3f * scale,
             1.0f * scale, 0xe879f9, 0.48f);
        line(x - scale, body_y + 3.0f * scale, x + 12.0f * scale, body_y + 11.0f * scale,
             0.7f * scale, 0xf5d0fe, 0.35f);
    }
}

static void draw_player(void)
{
    const Player *p = &G.player;
    float x = p->x;
    float y = p->y;
    if (p->grounded && p->gait_amount > 0.05f)
        ellipse(x + 5.5f, y + 14.5f, 5.5f, 1.3f, 0x020617, 0.38f);
    draw_kilix(x, y, 1.0f, p->facing, p->thrusting, p->phasing,
               p->gait_amount, p->gait_phase + G.scene_time * (1.0f - p->gait_amount));
}

/* --------------------------------------------------------------- scenes */

/* A placeholder salvage-terrace floor.  The real tile world lands at M3; M1
 * only needs a readable ground plane to stand Kilix on. */
static void draw_stage(void)
{
    float floor_y = (float)(LOGICAL_H - TILE * 3);
    rect(0, floor_y, (float)LOGICAL_W, (float)(LOGICAL_H) - floor_y, 0x1a1005, 1);
    line(0, floor_y, (float)LOGICAL_W, floor_y, 1, 0xd97706, 0.6f);
    for (int tx = 0; tx < LOGICAL_W / TILE; tx++) {
        float x = (float)(tx * TILE);
        rect(x, floor_y, (float)TILE, (float)TILE, 0x241a0a, 1);
        outline(x, floor_y, (float)TILE, (float)TILE, 1, 0x3a2a10, 0.6f);
        circle(x + 3, floor_y + 12, 1, 0xfbbf24, 0.4f);
        circle(x + 13, floor_y + 12, 1, 0xfbbf24, 0.4f);
    }
    draw_player();
}

static void draw_title(void)
{
    uint32_t orange = 0xf97316, cyan = 0x22d3ee;
    /* an original iris and orbit behind an enlarged Kilix salvager */
    for (int r = 96; r > 34; r -= 16)
        ring(72, 132, (float)r, 1, (r & 2) ? 0x312e81 : 0x164e63, 0.18f);
    for (int i = 0; i < 12; i++) {
        float a = G.scene_time * (0.12f + (float)i * 0.006f) + (float)i * 0.73f;
        circle(72 + cosf(a) * (34.0f + (float)i * 3.0f),
               132 + sinf(a) * (22.0f + (float)i * 1.5f),
               (i % 3 == 0) ? 2 : 1, (i & 1) ? cyan : 0xe879f9, 0.45f);
    }
    ring(72, 133, 48.0f + sinf(G.scene_time) * 2.0f, 3, 0x475569, 0.8f);
    ring(72, 133, 40, 2, cyan, 0.5f);
    circle(72, 133, 35, 0x061326, 0.95f);
    draw_kilix(52, 96, 5.0f, 1, true, false, 0, G.scene_time * 4.0f);

    text_shadow(150, 44, "SUPER", orange, 3);
    text_shadow(150, 78, "KILIX", cyan, 3);
    text(150, 116, "THE DRIFTWAY", 0xf8fafc, 1);
    line(150, 132, 244, 132, 1, cyan, 0.35f);
    text(150, 138, "RUN THE TETHER", 0x94a3b8, 1);
    /* a row of star-mote diamonds and the district glyph */
    for (int i = 0; i < 4; i++)
        diamond(154 + (float)i * 12, 156, 2.5f, 3.5f,
                (i & 1) ? 0x22d3ee : 0x67e8f9, 0.9f);
    shape_mark(232, 156, 0, 0xfcd34d, 0.85f);
    text_center(128, 210, "PRESS ENTER", 0xfcd34d, 1);
    text_center(128, 224, "A/D MOVE   SPACE JET", 0x64748b, 1);
}

void render_frame(void)
{
    offset_x = offset_y = 0;
    sr_blit(&logical, &backdrop, 0, 0);
    if (G.state == GS_TITLE) draw_title();
    else draw_stage();
    sr_scale_canvas(&screen, &logical);
    (void)sr_pack_rgba(&screen, framebuffer,
                       (size_t)screen_width * (size_t)screen_height * 4u);
}

bool render_dump_ppm(const char *path)
{
    return sr_write_ppm(&screen, path);
}

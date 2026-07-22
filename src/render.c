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

/* Draw a machine from primitives, camera-relative, with the shared danger ring
 * and — for a dormant/telegraphing walker — the nonlethal activation tell (an
 * alert ring + a warning chevron) that always precedes its first movement. */
static void draw_enemy(const Enemy *e)
{
    if (!e->active) return;
    float x = e->x - G.cam_x, y = e->y - G.cam_y;
    float cx = x + ENEMY_W * 0.5f;
    unsigned sub = e->state & ES_SUBSTATE;
    bool telegraph = sub == ES_WALK && e->alert > 0.0f && e->tell < 1.0f;
    bool moving = sub == ES_WALK && e->tell >= 1.0f;

    /* shared danger ring — every machine reads as hostile */
    float pulse = 0.5f + 0.5f * sinf(G.scene_time * 3.0f + e->home_x * 0.1f);
    ring(cx, y + 7.0f, 7.2f, 0.75f, 0xfb7185, 0.10f + pulse * 0.06f);
    if (telegraph) {
        float w = 0.5f + 0.5f * sinf(G.scene_time * 7.0f + e->home_x);
        ring(cx, y + 7.0f, 8.2f + w, 1.0f, 0xfbbf24, 0.24f + e->tell * 0.4f);
        triangle(cx - 3, y - 2, cx + 3, y - 2, cx, y - 7, 0xfbbf24, 0.9f);
        line(cx, y - 6, cx, y - 3, 1, 0x451a03, 0.9f);
    }

    if (sub == ES_SQUASHED) {                       /* flattened walker sliver */
        float a = clampf(e->squash / SQUASH_TIME, 0.0f, 1.0f);
        rect(x, y, ENEMY_W, SQUASH_H, 0x334155, a);
        line(x, y, x + ENEMY_W, y, 1, 0x64748b, 0.6f * a);
        return;
    }
    if (e->kind == EN_MAW) {                        /* Vent-Maw: telescoping intake */
        float base_y = (e->home_y - G.cam_y) + ENEMY_H;   /* the vent surface line */
        float head_y = y;                                 /* top of the extended head box */
        /* stacked neck segments from the vent up to the head */
        float neck_top = head_y + 5.0f;
        if (base_y > neck_top + 1.0f) {
            rect(cx - 3.0f, neck_top, 6.0f, base_y - neck_top, 0x3f6212, 1);
            for (float sy = neck_top + 3.0f; sy < base_y - 1.0f; sy += 4.0f)
                line(cx - 3.0f, sy, cx + 3.0f, sy, 1, 0x1a2e06, 0.7f);
        }
        rect(cx - 4.0f, base_y - 3.0f, 8.0f, 3.0f, 0x1a2e06, 1);   /* vent lip */
        if (e->emerge > 0.05f) {                     /* the snapping maw head */
            float open = 1.6f + 1.4f * (0.5f + 0.5f * sinf(G.scene_time * 9.0f));
            triangle(cx - 4.0f, head_y + 3.0f, cx + 4.0f, head_y + 3.0f,
                     cx, head_y + 3.0f - open, 0x84cc16, 1);       /* upper jaw */
            triangle(cx - 4.0f, head_y + 7.0f, cx + 4.0f, head_y + 7.0f,
                     cx, head_y + 7.0f + open, 0x3f6212, 1);       /* lower jaw */
            circle(cx, head_y + 5.0f, 1.1f, 0xfef08a, 0.9f);       /* hazard glint */
        }
        return;
    }
    if (e->kind == EN_RIVETER) {                    /* Riveter: hunched thrower bot */
        float bob = sinf(G.scene_time * 2.2f + e->home_x * 0.2f) * 0.6f;
        float ty = y + bob;
        rect(x + 1.0f, ty + 4.0f, 10.0f, 9.0f, 0x7c2d12, 1);       /* boxy torso */
        rect(x + 1.0f, ty + 4.0f, 10.0f, 2.0f, 0xb45309, 0.9f);    /* shoulder band */
        circle(x + (e->facing >= 0 ? 8.0f : 4.0f), ty + 7.0f, 1.8f, 0x1c0a04, 1);
        circle(x + (e->facing >= 0 ? 8.2f : 3.8f), ty + 7.0f, 0.9f, 0xf59e0b, 1); /* optic */
        float windmill = sinf(G.scene_time * 12.0f);               /* arm-cannons */
        line(x + 2.0f, ty + 6.0f, x - 1.0f, ty + 6.0f + windmill * 2.5f,
             1.6f, 0x92400e, 1);
        line(x + 10.0f, ty + 6.0f, x + 13.0f, ty + 6.0f - windmill * 2.5f,
             1.6f, 0x92400e, 1);
        rect(x + 2.0f, ty + 12.0f, 3.0f, 2.0f, 0x1c0a04, 1);
        rect(x + 7.0f, ty + 12.0f, 3.0f, 2.0f, 0x1c0a04, 1);
        return;
    }
    if (e->kind == EN_TURNER && sub == ES_HUSK) {   /* retracted / sliding shell */
        circle(cx, y + 6.0f, 5.0f, 0x64748b, 1);
        ring(cx, y + 6.0f, 3.6f, 1.2f, 0xcbd5e1, 0.9f);
        float spin = G.scene_time * ((e->state & ES_SHELL_MOV) ? 16.0f : 3.0f);
        for (int k = 0; k < 3; k++) {
            float a = spin + (float)k * 2.0944f;
            circle(cx + cosf(a) * 3.0f, y + 6.0f + sinf(a) * 3.0f, 1.0f, 0x22d3ee, 1);
        }
        if (e->revive_q > 0 && e->revive_q <= HUSK_WOBBLE_Q)   /* pre-revival wobble */
            triangle(cx - 3, y - 2, cx + 3, y - 2, cx, y - 6, 0xfbbf24, 0.9f);
        return;
    }
    if (e->kind == EN_TURNER) {                     /* walking Carapod: domed shell */
        ellipse(cx, y + 6.0f, 5.5f, 4.5f, 0x64748b, 1);
        ring(cx, y + 6.0f, 4.2f, 1.0f, 0xcbd5e1, 0.8f);
        circle(x + (e->facing >= 0 ? 10.0f : 2.0f), y + 7.0f, 1.8f, 0x94a3b8, 1);
        circle(x + (e->facing >= 0 ? 10.3f : 1.7f), y + 7.0f, 0.7f, 0xfb7185, 1);
        rect(x + 2.0f, y + 11.0f, 3.0f, 2.5f, 0x334155, 1);
        rect(x + 7.0f, y + 11.0f, 3.0f, 2.5f, 0x334155, 1);
        return;
    }
    /* Slag-Treader: a squat, blocky slag-hauler with a facing sensor eye and two
     * tread-feet that alternate a small bob only once it is actually moving. */
    float step = moving ? sinf(G.scene_time * 8.0f + e->home_x * 0.2f) * 1.5f : 0.0f;
    rect(x + 1.0f, y + 4.0f, 10.0f, 7.0f, 0x475569, 1);
    circle(x + 2.0f, y + 7.5f, 2.5f, 0x334155, 1);
    circle(x + 10.0f, y + 7.5f, 2.5f, 0x334155, 1);
    rect(x + 1.0f, y + 4.0f, 10.0f, 2.0f, 0x64748b, 1);
    circle(x + (e->facing >= 0 ? 8.5f : 3.5f), y + 7.0f, 1.6f, 0xfb7185, 1);
    circle(x + (e->facing >= 0 ? 8.8f : 3.2f), y + 7.0f, 0.7f, 0xfee2e2, 1);
    rect(x + 1.5f, y + 11.0f + fmaxf(0.0f, -step), 3.0f, 2.5f, 0x1e293b, 1);
    rect(x + 7.5f, y + 11.0f + fmaxf(0.0f, step), 3.0f, 2.5f, 0x1e293b, 1);
}

static void draw_enemies(void)
{
    for (int i = 0; i < MAX_ACTIVE_ENEMIES; i++) draw_enemy(&G.enemies[i]);
}

/* A rivet in flight: a small amber bolt tumbling about its centre (spin derived
 * from scene_time and the travel sign, never stored), with a hot core. */
static void draw_projectile(const Projectile *p)
{
    if (!p->active) return;
    float cx = p->x - G.cam_x + RIVET_W * 0.5f;
    float cy = p->y - G.cam_y + RIVET_H * 0.5f;
    float a  = G.scene_time * 18.0f * (p->facing >= 0 ? 1.0f : -1.0f);
    float ca = cosf(a) * 3.0f, sa = sinf(a) * 3.0f;
    line(cx - ca, cy - sa, cx + ca, cy + sa, 1.4f, 0xf59e0b, 1);
    line(cx + sa, cy - ca, cx - sa, cy + ca, 1.4f, 0xfbbf24, 0.9f);
    circle(cx, cy, 1.2f, 0xfef08a, 1);
}

static void draw_projectiles(void)
{
    for (int i = 0; i < MAX_PROJECTILES; i++) draw_projectile(&G.projectiles[i]);
}

static void draw_player(void)
{
    const Player *p = &G.player;
    /* Flicker while invulnerable (post-hit / respawn i-frames). */
    if (p->invuln > 0.0f && ((int)(G.scene_time * 20.0f) & 1)) return;
    float x = p->x - G.cam_x;
    float y = p->y - G.cam_y;
    if (p->grounded && p->gait_amount > 0.05f)
        ellipse(x + 5.5f, y + 14.5f, 5.5f, 1.3f, 0x020617, 0.38f);
    draw_kilix(x, y, 1.0f, p->facing, p->thrusting, p->phasing,
               p->gait_amount, p->gait_phase + G.scene_time * (1.0f - p->gait_amount));
}

/* --------------------------------------------------------------- scenes */

/* ------------------------------------------------------ RUST FLATS playfield */

/* District 1's amber vault-sky: warm horizontal bands, a distant vault-ring +
 * tether silhouette on slow parallax, and a parallax starfield.  Drawn fresh
 * each frame (unlike the prebuilt title backdrop) so it scrolls with the camera.
 * The visual-identity motif for RUST FLATS (visual-identity.md §5). */
static void draw_rust_backdrop(void)
{
    for (int i = 0; i < 12; i++) {
        float t = (float)i / 11.0f;
        uint32_t c = sr_mix(0x140b04, 0x3a1e08, t);
        rect(0.0f, (float)i * (LOGICAL_H / 12.0f),
             (float)LOGICAL_W, LOGICAL_H / 12.0f + 1.0f, c, 1.0f);
    }
    float px = -G.cam_x * 0.25f;
    line(px + 60.0f, 78.0f, px + 60.0f, 224.0f, 1.0f, 0x5a2f0c, 0.30f);
    ring(px + 60.0f, 150.0f, 72.0f, 2.0f, 0x5a2f0c, 0.45f);
    ring(px + 60.0f, 150.0f, 52.0f, 1.5f, 0x7a3f10, 0.30f);
    for (int i = 0; i < 90; i++) {
        float sx = unit_hash(700u + (uint32_t)i * 5u) * (float)LOGICAL_W
                 - G.cam_x * 0.35f;
        sx = fmodf(sx, (float)LOGICAL_W);
        if (sx < 0.0f) sx += (float)LOGICAL_W;
        float sy = unit_hash(800u + (uint32_t)i * 9u) * 156.0f;
        uint32_t c = (i % 6 == 0) ? 0xfbbf24 : (i % 5 == 0) ? 0x38bdf8 : 0x94a3b8;
        sr_px(&logical, (int)sx, (int)sy, c);
    }
}

/* One tile, drawn from primitives in the RUST FLATS palette, camera-scrolled. */
static void draw_tile(int col, int row)
{
    int t = G.vault_data.tiles[row][col];
    if (t == T_EMPTY) return;
    float x = (float)(col * TILE) - G.cam_x;
    float y = (float)(row * TILE) - G.cam_y;
    float pulse = 0.5f + 0.5f * sinf(G.scene_time * 4.0f + (float)col * 0.5f +
                                     (float)row * 0.3f);
    switch (t) {
    case T_HULL:
        rect(x, y, TILE, TILE, 0x241a0a, 1);
        rect(x, y, TILE, 3, 0xd97706, 0.9f);
        outline(x, y, TILE, TILE, 1, 0x3a2a10, 0.5f);
        circle(x + 3, y + 12, 1, 0xfbbf24, 0.4f);
        circle(x + 13, y + 12, 1, 0xfbbf24, 0.4f);
        break;
    case T_HULL_DARK:
        rect(x, y, TILE, TILE, 0x1a1005, 1);
        outline(x, y, TILE, TILE, 1, 0x2a1c0c, 0.4f);
        break;
    case T_BEDROCK:
        rect(x, y, TILE, TILE, 0x2a1e10, 1);
        outline(x + 1, y + 1, TILE - 2, TILE - 2, 1, 0x4a3418, 0.7f);
        line(x + 2, y + 4, x + 13, y + 3, 1, 0x6a4a22, 0.5f);
        break;
    case T_BRICK:
        rect(x, y, TILE, TILE, 0x3a2410, 1);
        line(x, y + 5, x + TILE, y + 5, 1, 0x1a1005, 0.8f);
        line(x, y + 11, x + TILE, y + 11, 1, 0x1a1005, 0.8f);
        line(x + 8, y, x + 8, y + 5, 1, 0x1a1005, 0.7f);
        line(x + 4, y + 6, x + 4, y + 11, 1, 0x1a1005, 0.7f);
        line(x + 12, y + 12, x + 12, y + TILE, 1, 0x1a1005, 0.7f);
        break;
    case T_CACHE:
        rect(x + 1, y + 1, TILE - 2, TILE - 2,
             sr_mix(0x92400e, 0xfbbf24, 0.3f + pulse * 0.2f), 1);
        outline(x + 1, y + 1, TILE - 2, TILE - 2, 1, 0xfcd34d, 0.8f);
        shape_mark(x + 8, y + 8, 0, 0xfef08a, 0.7f + pulse * 0.3f);
        break;
    case T_SPENT:
        rect(x, y, TILE, TILE, 0x1c130a, 1);
        outline(x, y, TILE, TILE, 1, 0x2a1c0c, 0.5f);
        break;
    case T_CONDUIT:
        rect(x + 2, y, TILE - 4, TILE, 0x0e2a3a, 1);
        rect(x + 2, y, TILE - 4, 3, 0x38bdf8, 0.7f);
        line(x + 5, y, x + 5, y + TILE, 1, 0x1a4a66, 0.6f);
        line(x + 11, y, x + 11, y + TILE, 1, 0x1a4a66, 0.6f);
        break;
    case T_LEDGE:
        rect(x, y, TILE, 4, 0x5a3410, 1);
        line(x, y, x + TILE, y, 1, 0xd97706, 0.8f);
        for (int i = 0; i < 4; i++)
            line(x + 2 + (float)i * 4, y + 4, x + 2 + (float)i * 4, y + 8, 1,
                 0x3a2410, 0.6f);
        break;
    case T_RISER:
        rect(x + 6, y, 4, TILE, 0x1a4a66, 1);
        rect(x + 6, y, 4, TILE, 0x38bdf8, 0.35f + pulse * 0.25f);
        circle(x + 8, y + 8, 2, 0x67e8f9, 0.9f);
        break;
    case T_IRIS:
        ring(x + 8, y + 8, 6, 1.5f, 0xfb7185, 0.7f);
        ring(x + 8, y + 8, 3, 1.0f, 0xf5d0fe, 0.5f);
        circle(x + 8, y + 8, 2.0f + pulse, 0x7a1020, 0.6f);
        break;
    case T_THORN:
        for (int i = 0; i < 4; i++)
            triangle(x + (float)i * 4, y + TILE, x + (float)i * 4 + 4, y + TILE,
                     x + (float)i * 4 + 2, y + 8, 0xfb7185, 1);
        break;
    default: break;
    }
}

/* Below the ground baseline the render fills a solid subsurface band under every
 * floored column so the terrace reads as ground; void columns leave the sky
 * showing through (the pit). */
static void draw_subsurface(void)
{
    const VaultData *v = &G.vault_data;
    int baseline = v->rows - 1;
    float top = (float)((baseline + 1) * TILE) - G.cam_y;
    if (top >= (float)LOGICAL_H) return;
    int first = (int)(G.cam_x / TILE) - 1;
    int last = first + LOGICAL_W / TILE + 2;
    for (int c = first; c <= last; c++) {
        if (c < 0 || c >= v->cols) continue;
        int floor = v->tiles[baseline][c];
        if (floor < T_HULL || floor > T_CONDUIT) continue;   /* void column */
        float x = (float)(c * TILE) - G.cam_x;
        rect(x, top, TILE, (float)LOGICAL_H - top, 0x140c04, 1);
        line(x, top + 4, x + TILE, top + 4, 1, 0x241205, 0.5f);
    }
}

/* Draw the loaded vault: RUST FLATS backdrop, then the camera-exposed columns of
 * the tile grid bracketed by a playfield clip, then Kilix. */
static void draw_playfield(void)
{
    draw_rust_backdrop();
    sr_canvas_set_clip(&logical, 0, 0, LOGICAL_W, LOGICAL_H);
    draw_subsurface();
    int first = (int)(G.cam_x / TILE) - 1;
    int last = first + LOGICAL_W / TILE + 2;
    for (int row = 0; row < G.vault_data.rows; row++)
        for (int col = first; col <= last; col++) {
            if (col < 0 || col >= G.vault_data.cols) continue;
            draw_tile(col, row);
        }
    draw_enemies();
    draw_projectiles();
    draw_player();
    sr_canvas_reset_clip(&logical);
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
    if (G.state == GS_TITLE) {
        sr_blit(&logical, &backdrop, 0, 0);
        draw_title();
    } else {
        draw_playfield();
    }
    sr_scale_canvas(&screen, &logical);
    (void)sr_pack_rgba(&screen, framebuffer,
                       (size_t)screen_width * (size_t)screen_height * 4u);
}

bool render_dump_ppm(const char *path)
{
    return sr_write_ppm(&screen, path);
}

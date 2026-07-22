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
#include <stdio.h>
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

/* --------------------------------------------------- per-district palette */

/* Eight biome palettes derived from the four render families (visual-identity §3.3).
 * Only the structural/atmosphere colours change per district; the semantic hues
 * (reward gold, hostile red, collectible cyan) stay fixed in draw_tile so hostility
 * and reward read identically in every biome. */
typedef struct {
    uint32_t bg_top, bg_bot;   /* backdrop gradient */
    uint32_t ring;             /* distant vault-ring / silhouette */
    uint32_t primary;          /* HUD accent + terrace top rim */
    uint32_t terrace;          /* terrace ground body */
    uint32_t sub;              /* subsurface fill below the baseline */
    uint32_t block;            /* bedrock / staircase */
    uint32_t conduit;          /* conduit shaft */
    uint32_t star;             /* parallax star tint */
} Biome;

static const Biome biomes[DISTRICTS] = {
    /* 1 RUST FLATS      */ { 0x140b04, 0x3a1e08, 0x5a2f0c, 0xd97706, 0x241a0a, 0x140c04, 0x2a1e10, 0x0e2a3a, 0xfbbf24 },
    /* 2 COIL WARRENS    */ { 0x05070d, 0x141c2e, 0x1e2b44, 0x64748b, 0x141a26, 0x0a0f1a, 0x121a28, 0x0e3040, 0x38bdf8 },
    /* 3 NULL TIDE       */ { 0x03121a, 0x0a3140, 0x11525c, 0x2dd4bf, 0x083038, 0x04121a, 0x0a2c34, 0x0e4652, 0x67e8f9 },
    /* 4 RAIL SPIRES     */ { 0x04120e, 0x0c2a24, 0x175040, 0x34d399, 0x0a2f26, 0x07231c, 0x0e3a30, 0x125046, 0xa7f3d0 },
    /* 5 THE CINDERWORKS */ { 0x180903, 0x431606, 0x7a2a08, 0xea580c, 0x2a1206, 0x1c0a03, 0x381606, 0x522008, 0xfb923c },
    /* 6 DROWNED CELLS   */ { 0x02080f, 0x08222e, 0x0e5266, 0x0e7490, 0x03121b, 0x02080f, 0x0a2a34, 0x0e4658, 0x22d3ee },
    /* 7 EMBER CONDUITS  */ { 0x1a0805, 0x3e1210, 0x7a2020, 0xf87171, 0x260a06, 0x1a0805, 0x3a1410, 0x521a18, 0xfb7185 },
    /* 8 THE WARDEN VAULT*/ { 0x120418, 0x340a2e, 0x5a1a4a, 0xfda4af, 0x260a2e, 0x1a0620, 0x341030, 0x4a1848, 0xf5d0fe },
};

static const Biome *cur_biome(void)
{
    int d = G.vault_data.district;
    if (d < 1 || d > DISTRICTS) d = 1;
    return &biomes[d - 1];
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
/* The Vault Guardian: a large multi-part warden automaton (cast.md §5.12) — a
 * broad armoured torso, a horned head with a single central optic that shifts on
 * the attack tell, arm-pistons, and a phase-plated chest hatch that opens (from
 * the emerge amount) to reveal the vulnerable magenta core.  The slabs are the
 * host district's heavy palette, assembled at boss scale (no per-boss sprite). */
static void draw_guardian(const Enemy *e)
{
    float x = e->x - G.cam_x, y = e->y - G.cam_y;
    float cx = x + GUARDIAN_W * 0.5f;
    /* a broad menace ring so the set-piece reads as hostile */
    float dr = 0.5f + 0.5f * sinf(G.scene_time * 2.4f + e->home_x * 0.1f);
    ring(cx, y + GUARDIAN_H * 0.5f, 17.0f, 1.2f, 0xfb7185, 0.10f + dr * 0.06f);
    /* arm-pistons */
    line(x + 2.0f, y + 14.0f, x - 2.0f, y + 21.0f, 2.0f, 0x475569, 1);
    line(x + GUARDIAN_W - 2.0f, y + 14.0f, x + GUARDIAN_W + 2.0f, y + 21.0f, 2.0f, 0x475569, 1);
    /* armoured torso slabs */
    rect(x + 2.0f, y + 8.0f, GUARDIAN_W - 4.0f, GUARDIAN_H - 10.0f, 0x1e293b, 1);
    outline(x + 2.0f, y + 8.0f, GUARDIAN_W - 4.0f, GUARDIAN_H - 10.0f, 1.5f, 0x64748b, 0.8f);
    rect(x, y + 10.0f, 4.0f, 11.0f, 0x334155, 1);                 /* shoulder slabs */
    rect(x + GUARDIAN_W - 4.0f, y + 10.0f, 4.0f, 11.0f, 0x334155, 1);
    /* horned head with the central optic */
    triangle(cx - 5.0f, y + 2.0f, cx - 7.5f, y - 4.0f, cx - 2.0f, y + 2.0f, 0x334155, 1);
    triangle(cx + 5.0f, y + 2.0f, cx + 7.5f, y - 4.0f, cx + 2.0f, y + 2.0f, 0x334155, 1);
    rect(cx - 5.0f, y + 1.0f, 10.0f, 8.0f, 0x0f172a, 1);
    uint32_t optic = sr_mix(0xfb7185, 0xfef08a, clampf(e->emerge, 0.0f, 1.0f));
    circle(cx, y + 5.0f, 2.4f, optic, 1);
    circle(cx, y + 5.0f, 1.0f, 0xfff7ed, 0.9f);
    /* chest hatch: sealed plate, or split panels exposing the magenta core */
    float open = clampf(e->emerge, 0.0f, 1.0f);
    float hy = y + 14.0f;
    if (open < GUARDIAN_HATCH_OPEN) {
        rect(cx - 5.0f, hy, 10.0f, 8.0f, 0x243044, 1);
        outline(cx - 5.0f, hy, 10.0f, 8.0f, 1, 0x64748b, 0.7f);
        line(cx, hy, cx, hy + 8.0f, 1, 0x0f172a, 0.8f);
    } else {
        float sep = (open - GUARDIAN_HATCH_OPEN) * 2.0f * 4.0f;
        rect(cx - 5.0f - sep, hy, 4.0f, 8.0f, 0x243044, 1);
        rect(cx + 1.0f + sep, hy, 4.0f, 8.0f, 0x243044, 1);
        circle(cx, hy + 4.0f, 3.0f, 0xe879f9, 1);
        ring(cx, hy + 4.0f, 4.0f + 0.6f * sinf(G.scene_time * 8.0f), 1.0f, 0xf5d0fe, 0.7f);
    }
}

static void draw_enemy(const Enemy *e)
{
    if (!e->active) return;
    float x = e->x - G.cam_x, y = e->y - G.cam_y;
    if (e->kind == EN_GUARDIAN || e->kind == EN_OVERSEER) { draw_guardian(e); return; }
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
    if (p->kind == PJ_PULSE) {                       /* Charged Kilix's phase-bolt */
        float cx = p->x - G.cam_x + PULSE_W * 0.5f;
        float cy = p->y - G.cam_y + PULSE_H * 0.5f;
        float a  = G.scene_time * 22.0f * (p->facing >= 0 ? 1.0f : -1.0f);
        line(cx - (float)p->facing * 6.0f, cy, cx, cy, 1.2f, 0xf5d0fe, 0.4f); /* trailing streak */
        ring(cx, cy, 3.0f + 0.6f * sinf(a), 1.0f, 0xe879f9, 0.85f);
        circle(cx, cy, 1.6f, 0xf5d0fe, 1);
        return;
    }
    if (p->kind == PJ_PLASMA) {                      /* the Guardian's plasma arc */
        float cx = p->x - G.cam_x + PLASMA_W * 0.5f;
        float cy = p->y - G.cam_y + PLASMA_H * 0.5f;
        float pulse = 0.5f + 0.5f * sinf(G.scene_time * 14.0f + cx * 0.2f);
        line(cx - (float)p->facing * 5.0f, cy, cx, cy, 1.4f, 0xfb923c, 0.5f);
        circle(cx, cy, 3.2f + pulse * 0.6f, 0xf97316, 0.9f);   /* molten glob */
        circle(cx, cy, 1.6f, 0xfde047, 1);                     /* hot centre */
        return;
    }
    /* the Riveter's rivet: a small amber bolt tumbling about its centre */
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
    bool charged = p->power_tier >= 2;
    draw_kilix(x, y, 1.0f, p->facing, p->thrusting, p->phasing || charged,
               p->gait_amount, p->gait_phase + G.scene_time * (1.0f - p->gait_amount));
    /* Plated: a salvaged hull segment clamped over the torso — extra plating rects
     * over the existing body, a slightly heavier silhouette (cast.md §4.1). */
    if (p->power_tier >= 1) {
        rect(x + 1.5f, y + 6.5f, 8.0f, 6.5f, 0x94a3b8, 0.85f);
        outline(x + 1.5f, y + 6.5f, 8.0f, 6.5f, 1, 0xcbd5e1, 0.8f);
        line(x + 1.5f, y + 9.5f, x + 9.5f, y + 9.5f, 1, 0x475569, 0.7f);
        rect(x + 0.3f, y + 6.0f, 2.0f, 5.0f, 0x64748b, 0.9f);
        rect(x + 9.7f, y + 6.0f, 2.0f, 5.0f, 0x64748b, 0.9f);
    }
    /* Charged: a glowing magenta phase-core on the chest (enables the Phase Pulse). */
    if (charged) {
        float pulse = 0.5f + 0.5f * sinf(G.scene_time * 6.0f);
        circle(x + 5.5f, y + 9.0f, 1.8f + pulse * 0.5f, 0xe879f9, 1);
        circle(x + 5.5f, y + 9.0f, 0.9f, 0xf5d0fe, 1);
    }
    /* Aegis: a spinning white-gold invulnerability halo with orbiting sparks
     * (cast.md §4.2), flickering faster as the window runs out. */
    if (p->aegis_q > 0) {
        float cx = x + 5.5f, cy = y + 7.0f;
        float spin = G.scene_time * (p->aegis_q <= 6 ? 9.0f : 4.0f);
        ring(cx, cy, 9.0f, 1.0f, 0xf8fafc, 0.6f);
        for (int k = 0; k < 4; k++) {
            float a = spin + (float)k * 1.5707963f;
            circle(cx + cosf(a) * 9.0f, cy + sinf(a) * 9.0f, 1.2f, 0xfcd34d, 0.9f);
        }
    }
}

/* --------------------------------------------------------------- scenes */

/* ------------------------------------------------------ RUST FLATS playfield */

/* The district's vault-sky: a vertical colour gradient, a distant vault-ring +
 * tether silhouette on slow parallax, and a parallax starfield — all tinted by the
 * current biome palette (visual-identity.md §3.3/§5).  Drawn fresh each frame
 * (unlike the prebuilt title backdrop) so it scrolls with the camera. */
static void draw_biome_backdrop(void)
{
    const Biome *b = cur_biome();
    for (int i = 0; i < 12; i++) {
        float t = (float)i / 11.0f;
        uint32_t c = sr_mix(b->bg_top, b->bg_bot, t);
        rect(0.0f, (float)i * (LOGICAL_H / 12.0f),
             (float)LOGICAL_W, LOGICAL_H / 12.0f + 1.0f, c, 1.0f);
    }
    float px = -G.cam_x * 0.25f;
    line(px + 60.0f, 78.0f, px + 60.0f, 224.0f, 1.0f, b->ring, 0.30f);
    ring(px + 60.0f, 150.0f, 72.0f, 2.0f, b->ring, 0.45f);
    ring(px + 60.0f, 150.0f, 52.0f, 1.5f, b->primary, 0.24f);
    for (int i = 0; i < 90; i++) {
        float sx = unit_hash(700u + (uint32_t)i * 5u) * (float)LOGICAL_W
                 - G.cam_x * 0.35f;
        sx = fmodf(sx, (float)LOGICAL_W);
        if (sx < 0.0f) sx += (float)LOGICAL_W;
        float sy = unit_hash(800u + (uint32_t)i * 9u) * 156.0f;
        uint32_t c = (i % 6 == 0) ? b->star : (i % 5 == 0) ? b->primary : 0x94a3b8;
        sr_px(&logical, (int)sx, (int)sy, c);
    }
}

/* One tile, drawn from primitives in the RUST FLATS palette, camera-scrolled. */
static void draw_tile(int col, int row)
{
    int t = G.vault_data.tiles[row][col];
    if (t == T_EMPTY) return;
    const Biome *b = cur_biome();
    float x = (float)(col * TILE) - G.cam_x;
    float y = (float)(row * TILE) - G.cam_y;
    float pulse = 0.5f + 0.5f * sinf(G.scene_time * 4.0f + (float)col * 0.5f +
                                     (float)row * 0.3f);
    switch (t) {
    case T_HULL:
        rect(x, y, TILE, TILE, b->terrace, 1);
        rect(x, y, TILE, 3, b->primary, 0.9f);
        outline(x, y, TILE, TILE, 1, sr_scale_rgb(b->primary, 0.4f), 0.5f);
        circle(x + 3, y + 12, 1, b->star, 0.4f);
        circle(x + 13, y + 12, 1, b->star, 0.4f);
        break;
    case T_HULL_DARK:
        rect(x, y, TILE, TILE, b->sub, 1);
        outline(x, y, TILE, TILE, 1, sr_scale_rgb(b->primary, 0.3f), 0.4f);
        break;
    case T_BEDROCK:
        rect(x, y, TILE, TILE, b->block, 1);
        outline(x + 1, y + 1, TILE - 2, TILE - 2, 1, sr_scale_rgb(b->primary, 0.6f), 0.7f);
        line(x + 2, y + 4, x + 13, y + 3, 1, sr_scale_rgb(b->primary, 0.5f), 0.5f);
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
        rect(x + 2, y, TILE - 4, TILE, b->conduit, 1);
        rect(x + 2, y, TILE - 4, 3, b->star, 0.7f);
        line(x + 5, y, x + 5, y + TILE, 1, sr_scale_rgb(b->conduit, 1.4f), 0.6f);
        line(x + 11, y, x + 11, y + TILE, 1, sr_scale_rgb(b->conduit, 1.4f), 0.6f);
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
    case T_SEAL:
        /* the Gate release node: a wall-mounted switch with a triangle glyph */
        rect(x + 4, y + 2, 8, 12, 0x3a1520, 1);
        outline(x + 4, y + 2, 8, 12, 1, 0xfb7185, 0.8f);
        circle(x + 8, y + 8, 3.0f, sr_mix(0x7a1020, 0xfb7185, pulse), 1);
        shape_mark(x + 8, y + 8, 1, 0xfef08a, 0.6f + pulse * 0.35f);
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
        rect(x, top, TILE, (float)LOGICAL_H - top, cur_biome()->sub, 1);
        line(x, top + 4, x + TILE, top + 4, 1, sr_scale_rgb(cur_biome()->primary, 0.3f), 0.5f);
    }
}

/* Draw the loaded vault: RUST FLATS backdrop, then the camera-exposed columns of
 * the tile grid bracketed by a playfield clip, then Kilix. */
static void draw_playfield(void)
{
    draw_biome_backdrop();
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

/* ------------------------------------------------------------------- HUD */

static int text_width(const char *s, int scale) { return (int)strlen(s) * 8 * scale; }

/* A small Kilix-head life glyph: an orange circle with two ear triangles. */
static void life_glyph(float cx, float cy)
{
    circle(cx, cy, 3.0f, 0xf97316, 1);
    triangle(cx - 3, cy - 1, cx - 2, cy - 4, cx, cy - 1, 0xf97316, 1);
    triangle(cx + 3, cy - 1, cx + 2, cy - 4, cx, cy - 1, 0xf97316, 1);
    rect(cx - 2.0f, cy - 1.0f, 4.0f, 1.4f, 0x083344, 1);   /* visor slit */
}

/* The translucent 2-row status overlay (visual-identity §7), drawn last with a
 * zero shake offset so juice never moves it: VAULT label + charge timer on the top
 * row; lives, MOTES, power tier, and the 7-digit score on the second.  All strings
 * are uppercase ASCII via the 8x16 font, snprintf'd into stack buffers. */
static void draw_hud(void)
{
    const Biome *b = cur_biome();
    char buf[48];
    rect(0.0f, 0.0f, (float)LOGICAL_W, (float)(HUD_ROWS * TILE), 0x050914, 0.82f);
    line(0.0f, (float)(HUD_ROWS * TILE), (float)LOGICAL_W, (float)(HUD_ROWS * TILE),
         1.0f, b->primary, 0.7f);

    /* top row: VAULT d-v (left), power-tier chip + AEG (centre), TIME nnn (right) */
    snprintf(buf, sizeof buf, "VAULT %d-%d", G.vault_data.district, G.vault_data.vault);
    text_shadow(4.0f, 3.0f, buf, b->primary, 1);
    const char *tier = G.player.power_tier >= 2 ? "CHG"
                     : G.player.power_tier == 1 ? "PLT" : "BAR";
    uint32_t pcol = G.player.power_tier >= 2 ? 0xe879f9
                  : G.player.power_tier == 1 ? 0xcbd5e1 : 0x64748b;
    text(108.0f, 3.0f, tier, pcol, 1);
    if (G.player.aegis_q > 0) text(136.0f, 3.0f, "AEG", 0xfcd34d, 1);
    snprintf(buf, sizeof buf, "TIME %d", G.charge);
    uint32_t tcol = (G.charge <= LOW_CHARGE_CUE) ? 0xfb7185 : 0xfcd34d;
    text((float)(LOGICAL_W - text_width(buf, 1) - 4), 3.0f, buf, tcol, 1);

    /* second row: life glyphs + count (left) */
    int shown = G.lives < 3 ? G.lives : 3;
    for (int i = 0; i < shown; i++) life_glyph(8.0f + (float)i * 9.0f, 24.0f);
    snprintf(buf, sizeof buf, "x%d", G.lives);
    text(8.0f + (float)shown * 9.0f, 18.0f, buf, 0xf8fafc, 1);

    /* MOTES count + a short diamond strip (centre) */
    snprintf(buf, sizeof buf, "MOTES %02d", G.motes % MOTES_PER_UNIT);
    text(88.0f, 18.0f, buf, 0x67e8f9, 1);
    for (int i = 0; i < 5; i++)
        diamond(154.0f + (float)i * 6.0f, 24.0f, 2.0f, 3.0f,
                (i < (G.motes % MOTES_PER_UNIT) / 20) ? 0x22d3ee : 0x334155, 0.9f);

    /* the 7-digit score (right) */
    snprintf(buf, sizeof buf, "S%07d", G.score);
    text((float)(LOGICAL_W - text_width(buf, 1) - 4), 18.0f, buf, 0xfcd34d, 1);
}

/* A centred transient banner over the playfield (the clear / life-lost / hurry
 * beats). */
static void draw_banner(const char *title, const char *sub, uint32_t color)
{
    rect(0.0f, 96.0f, (float)LOGICAL_W, 48.0f, 0x050914, 0.70f);
    line(0.0f, 96.0f, (float)LOGICAL_W, 96.0f, 1.0f, color, 0.6f);
    line(0.0f, 144.0f, (float)LOGICAL_W, 144.0f, 1.0f, color, 0.6f);
    text_center(128.0f, 108.0f, title, color, 2);
    if (sub) text_center(128.0f, 130.0f, sub, 0xf8fafc, 1);
}

/* A full-screen end-state card (game over / victory). */
static void draw_end_card(const char *title, const char *sub, uint32_t color)
{
    rect(0.0f, 0.0f, (float)LOGICAL_W, (float)LOGICAL_H, 0x03040a, 0.86f);
    text_center(128.0f, 92.0f, title, color, 3);
    text_center(128.0f, 130.0f, sub, 0xf8fafc, 1);
    char buf[48];
    snprintf(buf, sizeof buf, "SCORE %07d", G.score);
    text_center(128.0f, 150.0f, buf, 0xfcd34d, 1);
}

/* The campaign map / level select: an 8x4 grid of vault cells, districts unlocked
 * up to G.unlock_district lit in their biome primary, the rest dimmed. */
static void draw_campaign_map(void)
{
    rect(0.0f, 0.0f, (float)LOGICAL_W, (float)LOGICAL_H, 0x04060e, 0.94f);
    text_center(128.0f, 14.0f, "THE DRIFTWAY", 0xf8fafc, 2);
    text_center(128.0f, 36.0f, "SELECT A VAULT", 0x64748b, 1);
    for (int d = 0; d < DISTRICTS; d++) {
        float ry = 52.0f + (float)d * 22.0f;
        bool unlocked = (d + 1) <= G.unlock_district;
        uint32_t pc = unlocked ? biomes[d].primary : 0x2a2f3a;
        char lbl[32];
        snprintf(lbl, sizeof lbl, "%d", d + 1);
        text(8.0f, ry + 3.0f, lbl, pc, 1);
        for (int v = 0; v < VAULTS_PER_DISTRICT; v++) {
            float cx = 32.0f + (float)v * 26.0f;
            outline(cx, ry, 20.0f, 16.0f, 1.0f, pc, unlocked ? 0.9f : 0.4f);
            if (unlocked) rect(cx + 1.0f, ry + 1.0f, 18.0f, 14.0f,
                               sr_scale_rgb(biomes[d].primary, 0.35f), 0.5f);
            char vn[8];
            snprintf(vn, sizeof vn, "%d", v + 1);
            text(cx + 8.0f, ry + 4.0f, vn, unlocked ? 0xf8fafc : 0x475569, 1);
        }
        text(140.0f, ry + 4.0f, district_name(d + 1), pc, 1);
    }
}

void render_frame(void)
{
    offset_x = offset_y = 0;
    if (G.state == GS_TITLE) {
        sr_blit(&logical, &backdrop, 0, 0);
        draw_title();
    } else if (G.state == GS_PAUSED) {
        draw_playfield();
        draw_hud();
        draw_campaign_map();
    } else if (G.state == GS_VICTORY) {
        draw_playfield();
        draw_end_card("DRIFTWAY RELIT", "THE LODESTAR IS FREE", 0x67e8f9);
    } else {
        draw_playfield();
        draw_hud();
        if (G.state == GS_VAULT_CLEAR)
            draw_banner("VAULT SEALED", "THE WAY OPENS INWARD", 0x4ade80);
        else if (G.state == GS_LIFE_LOST)
            draw_banner("UNIT LOST", NULL, 0xfb7185);
        else if (G.state == GS_GAMEOVER)
            draw_end_card("SALVAGE LOST", "THE FRONT CLOSES IN", 0xfb7185);
        else if (G.charge <= LOW_CHARGE_CUE && G.charge > 0)
            draw_banner("THE FRONT CLOSES", "HURRY", 0xfbbf24);
    }
    sr_scale_canvas(&screen, &logical);
    (void)sr_pack_rgba(&screen, framebuffer,
                       (size_t)screen_width * (size_t)screen_height * 4u);
}

bool render_dump_ppm(const char *path)
{
    return sr_write_ppm(&screen, path);
}

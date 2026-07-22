/* Simulation core: the seeded RNG, Kilix's physics, per-axis tile collision, the
 * right-ratcheting camera, the input funnel, the deterministic autopilot, and the
 * per-tick validator.  The simulation never reads graphics state: collision boxes
 * are the named PLAYER_W/PLAYER_H constants, independent of the drawn silhouette. */
#include "super_kilix.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

GameState G;

float clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

/* xorshift32 — the only source of simulation randomness.  Seeded in game_init
 * and advanced one step per call; identical seeds yield identical streams. */
float game_randf(void)
{
    G.rng ^= G.rng << 13;
    G.rng ^= G.rng >> 17;
    G.rng ^= G.rng << 5;
    return (float)(G.rng >> 8) * (1.0f / 16777216.0f);
}

/* ------------------------------------------------------------- tile queries */

/* Raw semantic cell, T_EMPTY outside the authored grid (so the one-way test can
 * ask about any coordinate without a bounds dance). */
static int tile_cell(int col, int row)
{
    const VaultData *v = &G.vault_data;
    if (col < 0 || col >= v->cols || row < 0 || row >= v->rows) return T_EMPTY;
    return v->tiles[row][col];
}

/* The single semantic-solidity oracle shared by physics and validation.  Out of
 * bounds to the sides and below the floor read as solid walls (you cannot leave
 * the vault); above the top reads as empty (jumps may briefly go offscreen).
 * One-way tiles are NOT solid here — the resolver handles them by direction. */
bool game_tile_solid(int col, int row)
{
    const VaultData *v = &G.vault_data;
    if (col < 0 || col >= v->cols) return true;   /* side walls */
    if (row >= v->rows) return true;              /* below the floor */
    if (row < 0) return false;                    /* above the top */
    return v->tiles[row][col] == T_HULL;
}

/* Does the player box at (x,y) overlap blocking geometry?  A small inset keeps a
 * box resting flush against a surface from counting the adjacent cell.  One-way
 * grates block only a downward move whose bottom edge started on-or-above the
 * grate top (tested against the PRE-move bottom, never the velocity sign). */
static bool box_blocked(float x, float y, bool vertical, bool moving_down,
                        float prev_bottom)
{
    const float inset = 0.12f;
    int x0 = (int)floorf((x + inset) / TILE_SIZE);
    int x1 = (int)floorf((x + PLAYER_W - inset) / TILE_SIZE);
    int y0 = (int)floorf((y + inset) / TILE_SIZE);
    int y1 = (int)floorf((y + PLAYER_H - inset) / TILE_SIZE);
    for (int row = y0; row <= y1; row++)
        for (int col = x0; col <= x1; col++) {
            if (game_tile_solid(col, row)) return true;
            if (vertical && moving_down && tile_cell(col, row) == T_LEDGE) {
                float top = (float)(row * TILE_SIZE);
                if (prev_bottom <= top + ONEWAY_EPS) return true;
            }
        }
    return false;
}

/* ----------------------------------------------------- per-axis resolution */

/* Move the box along one axis, sub-stepped so a single call can never tunnel a
 * one-tile wall even at an injected high speed, with a sub-pixel bisection to
 * settle flush against the first surface hit.  Reports the hit through the
 * player's grounded flag and zeroed velocity, matching player-mechanics.md §5. */
static void move_axis(float amount, bool vertical, float prev_bottom)
{
    if (amount == 0.0f) return;
    bool moving_down = vertical && amount > 0.0f;
    while (amount != 0.0f) {
        float step = clampf(amount, -(float)MAX_STEP, (float)MAX_STEP);
        float *pos = vertical ? &G.player.y : &G.player.x;
        *pos += step;
        if (!box_blocked(G.player.x, G.player.y, vertical, moving_down, prev_bottom)) {
            amount -= step;
            continue;
        }
        *pos -= step;
        float lo = 0.0f, hi = 1.0f;
        for (int i = 0; i < 12; i++) {
            float mid = (lo + hi) * 0.5f;
            *pos += step * mid;
            bool hit = box_blocked(G.player.x, G.player.y, vertical,
                                   moving_down, prev_bottom);
            *pos -= step * mid;
            if (hit) hi = mid; else lo = mid;
        }
        *pos += step * lo;
        if (vertical) {
            if (moving_down) G.player.grounded = true;   /* landing */
            G.player.vy = 0.0f;                          /* land or head-bonk */
        } else {
            G.player.vx = 0.0f;                          /* wall */
        }
        return;
    }
}

/* ----------------------------------------------------------- player update */

static float horizontal_axis(bool left, bool right)
{
    return (right ? 1.0f : 0.0f) - (left ? 1.0f : 0.0f);
}

static void update_player(void)
{
    Player *p = &G.player;

    bool left  = G.held_controls ? G.held_left  : G.left_latch  > 0.0f;
    bool right = G.held_controls ? G.held_right : G.right_latch > 0.0f;
    bool down  = G.held_controls ? G.held_down  : G.down_latch  > 0.0f;
    bool run   = G.held_controls ? G.held_run   : G.run_latch   > 0.0f;
    bool jump  = G.held_controls ? G.held_jump  : G.jump_latch  > 0.0f;
    float dir  = horizontal_axis(left, right);

    p->jump_held = jump;
    p->fastfall  = down && !p->grounded;
    if (dir != 0.0f) p->facing = dir > 0.0f ? 1 : -1;

    /* Run-cap persistence: the run cap survives a brief release. */
    if (run) p->run_sticky = RUN_STICKY;
    else if (p->run_sticky > 0.0f) p->run_sticky = fmaxf(0.0f, p->run_sticky - 1.0f);
    float cap = (run || p->run_sticky > 0.0f) ? RUN_MAX : WALK_MAX;

    /* Horizontal: skid (input opposes motion) vs accel vs release coast. */
    if (dir != 0.0f) {
        if (p->vx != 0.0f && (p->vx > 0.0f) != (dir > 0.0f))
            p->vx += dir * SKID_DECEL * TICK_DT;             /* crisp turnaround */
        else
            p->vx += dir * (p->grounded ? GROUND_ACCEL : AIR_ACCEL) * TICK_DT;
        p->vx = clampf(p->vx, -cap, cap);
    } else if (p->grounded) {
        float drop = GROUND_FRICTION * TICK_DT;              /* softer coast */
        if (p->vx > 0.0f)      p->vx = fmaxf(0.0f, p->vx - drop);
        else if (p->vx < 0.0f) p->vx = fminf(0.0f, p->vx + drop);
    }
    if (fabsf(p->vx) < STOP_THRESHOLD) p->vx = 0.0f;

    /* Jump: a buffered press fires while grounded or inside the coyote window. */
    bool can_jump = p->grounded || p->coyote > 0.0f;
    bool buffered = p->buffer_tick >= 0 &&
                    (long long)G.tick - (long long)p->buffer_tick <= BUFFER_FRAMES;
    if (buffered && can_jump) {
        float speed = fabsf(p->vx);
        float v0;
        if (speed >= JUMP_BAND_RUN)       { v0 = JUMP_V0_RUN;  p->jump_band = 2; }
        else if (speed >= JUMP_BAND_WALK) { v0 = JUMP_V0_WALK; p->jump_band = 1; }
        else                              { v0 = JUMP_V0_STAND; p->jump_band = 0; }
        p->vy = -v0;
        p->jumping = true;
        p->grounded = false;
        p->coyote = 0.0f;
        p->buffer_tick = -1;
    }

    /* Vertical: gravity defaults to the snappy fall rate; the floaty rise gravity
     * is SUBSTITUTED only while actively holding a rising jump (variable height
     * falls out of the substitution).  Without release events the substitution is
     * unconditional up to apex, giving a single fixed-height jump. */
    float g = G_FALL;
    if (p->jumping && p->vy < 0.0f) {
        if (G.held_controls) {
            if (p->jump_held)
                g = (fabsf(p->vy) < APEX_VY) ? G_APEX : G_RISE;
        } else {
            g = (fabsf(p->vy) < APEX_VY) ? G_APEX : G_RISE;
        }
    }
    p->vy += g * TICK_DT;
    if (p->vy >= 0.0f) p->jumping = false;
    p->vy = fminf(p->vy, p->fastfall ? FASTFALL_MAX : FALL_MAX);

    /* Integrate, one axis at a time (X before Y), resolving each against tiles. */
    float prev_bottom = p->y + PLAYER_H;
    p->grounded = false;
    move_axis(p->vx * TICK_DT, false, 0.0f);
    move_axis(p->vy * TICK_DT, true, prev_bottom);

    /* Coyote: refreshed while grounded, spent while airborne. */
    if (p->grounded) p->coyote = COYOTE_FRAMES;
    else if (p->coyote > 0.0f) p->coyote = fmaxf(0.0f, p->coyote - 1.0f);

    p->thrusting = run && !p->grounded && fabsf(p->vx) > 1.0f;
    p->phasing = false;

    /* Cosmetic gait, advanced from |vx| only (never the sim RNG). */
    bool walking = p->grounded && fabsf(p->vx) > 4.0f;
    float target = walking ? 1.0f : 0.0f;
    float rate = target > p->gait_amount ? 12.0f : 8.0f;
    p->gait_amount += (target - p->gait_amount) * fminf(1.0f, rate * TICK_DT);
    if (walking) p->gait_phase += fabsf(p->vx) * 0.06f * TICK_DT;
    if (p->gait_phase > 6.2831853f) p->gait_phase -= 6.2831853f;
}

/* ----------------------------------------------------------------- camera */

static float scroll_limit(void)
{
    float world = (float)(G.vault_data.cols * TILE_SIZE);
    return fmaxf(0.0f, world - (float)LOGICAL_W);
}

/* Monotonic right-only ratchet: a deadzone the player sits inside, a
 * facing-direction lookahead that ramps with speed, critically-damped follow,
 * and a hard "never move left" clamp.  Vertical stays locked for standard-height
 * vaults (a tall-room damped follow arrives with those vaults at M3). */
static void update_camera(void)
{
    if (G.scroll_lock) return;
    float centre = G.player.x + PLAYER_W * 0.5f;
    float lead = clampf(G.player.vx / RUN_MAX, 0.0f, 1.0f) *
                 (CAM_LOOKAHEAD - CAM_DEADZONE);
    float anchor = CAM_DEADZONE + lead;          /* desired on-screen X */
    float desired = centre - anchor;
    float step = (desired - G.cam_x) * CAM_SMOOTH;
    if (step > 0.0f) G.cam_x += step;            /* never scrolls left */
    G.cam_x = clampf(G.cam_x, 0.0f, scroll_limit());
    G.cam_x_max = fmaxf(G.cam_x_max, G.cam_x);

    float floor_room = (float)(G.vault_data.rows * TILE_SIZE - LOGICAL_H);
    G.cam_y = clampf(G.cam_y, 0.0f, fmaxf(0.0f, floor_room));   /* == 0 standard */
}

/* ----------------------------------------------------------- level control */

static void spawn_player(void)
{
    Player *p = &G.player;
    int facing = p->facing;
    memset(p, 0, sizeof *p);
    p->facing = facing ? facing : 1;
    p->buffer_tick = -1;
    p->x = (float)(G.vault_data.spawn_col * TILE_SIZE) + 2.0f;
    /* Feet resting on the floor row directly below the spawn body row. */
    p->y = (float)((G.vault_data.spawn_row + 1) * TILE_SIZE) - PLAYER_H;
    p->grounded = true;
    p->coyote = COYOTE_FRAMES;
}

void game_load_level(int level)
{
    G.level = level;
    vault_build(level, &G.vault_data);
    spawn_player();
    G.cam_x = G.cam_x_max = G.cam_y = 0.0f;
    G.scroll_lock = false;
    G.state = GS_PLAYING;
}

void game_start(int level)
{
    game_load_level(level);
}

void game_force_level_clear(void)
{
    G.state = GS_VAULT_CLEAR;
}

void game_force_life_lost(void)
{
    G.state = GS_LIFE_LOST;
}

/* ---------------------------------------------------------------- lifecycle */

void game_init(int w, int h, uint32_t seed)
{
    memset(&G, 0, sizeof G);
    G.W = w;
    G.H = h;
    G.rng = seed ? seed : 0x6a09e667u;
    G.state = GS_TITLE;
    G.sound_on = true;
    G.player.facing = 1;
    G.player.grounded = true;
    G.player.buffer_tick = -1;
}

void game_shutdown(void)
{
    /* M2 owns no external resources; the profile flushes here from M6. */
}

/* ---------------------------------------------------------------- input */

static void decay_latches(void)
{
    float *latch[] = {&G.left_latch, &G.right_latch, &G.up_latch,
                      &G.down_latch, &G.jump_latch, &G.run_latch};
    for (size_t i = 0; i < sizeof latch / sizeof latch[0]; i++)
        *latch[i] = fmaxf(0.0f, *latch[i] - TICK_DT);
}

void game_set_held_controls(bool available, bool left, bool right,
                            bool up, bool down, bool jump, bool boost)
{
    /* A rising edge of the jump control latches a buffered press so a tap between
     * two sim ticks is never lost. */
    if (jump && !G.held_jump) G.player.buffer_tick = (int)G.tick;
    G.held_controls = available;
    G.held_left = left; G.held_right = right;
    G.held_up = up; G.held_down = down;
    G.held_jump = jump; G.held_run = boost;
}

/* Press-only fallback (no release events): each control becomes a 0.30 s latch
 * that decays by TICK_DT.  The jump press also latches a buffered edge; the jump
 * itself is fixed-height (variable height needs the held path). */
static void set_press_latch(int key)
{
    if (key == KEY_LEFT  || key == 'a') G.left_latch  = PRESS_LATCH_S;
    if (key == KEY_RIGHT || key == 'd') G.right_latch = PRESS_LATCH_S;
    if (key == KEY_UP    || key == 'w') G.up_latch    = PRESS_LATCH_S;
    if (key == KEY_DOWN  || key == 's') G.down_latch  = PRESS_LATCH_S;
    if (key == ' ' || key == 'z' || key == KEY_UP || key == 'w') {
        G.jump_latch = PRESS_LATCH_S;
        G.player.buffer_tick = (int)G.tick;
    }
}

void game_handle_key(int key)
{
    if (key >= 'A' && key <= 'Z') key += 'a' - 'A';
    if (G.state == GS_PLAYING && !G.held_controls) set_press_latch(key);
}

/* ---------------------------------------------------------------- autopilot */

/* A deterministic bot: hold right and run toward the exit, and buffer a jump
 * whenever a wall is just ahead or forward progress has stalled.  It drives the
 * real physics through the same fields the funnel writes, so it exercises the
 * collision/camera path without a terminal.  No RNG of its own. */
void game_autopilot(void)
{
    if (G.state != GS_PLAYING) return;
    Player *p = &G.player;
    int col = (int)floorf((p->x + PLAYER_W * 0.5f) / TILE_SIZE);
    int body_row = (int)floorf((p->y + PLAYER_H * 0.5f) / TILE_SIZE);
    int foot_row = (int)floorf((p->y + PLAYER_H - 1.0f) / TILE_SIZE);
    int ahead = col + (p->facing >= 0 ? 1 : -1);

    bool wall_ahead = game_tile_solid(ahead, body_row) ||
                      game_tile_solid(ahead, foot_row);
    bool stalled = p->grounded && fabsf(p->vx) < 6.0f;
    bool want_jump = wall_ahead || stalled;

    if (want_jump && (p->grounded || p->coyote > 0.0f))
        p->buffer_tick = (int)G.tick;

    G.scripted_input = true;
    G.held_controls = true;
    G.held_left = false;
    G.held_right = true;
    G.held_up = false;
    G.held_down = false;
    G.held_run = true;
    G.held_jump = want_jump;
}

/* ---------------------------------------------------------------- tick */

void game_tick(void)
{
    if (G.quit) return;
    G.tick++;
    G.scene_time += TICK_DT;
    (void)game_randf();          /* keep the RNG stream advancing deterministically */
    decay_latches();

    if (G.state != GS_PLAYING) return;

    update_player();
    update_camera();
}

/* ---------------------------------------------------------------- validate */

bool game_validate(char *err, size_t len)
{
    if (G.state < 0 || G.state >= GS_STATE_COUNT) {
        if (err && len) snprintf(err, len, "state %d out of range", G.state);
        return false;
    }

    const Player *p = &G.player;
    const float floats[] = {
        G.scene_time, p->x, p->y, p->vx, p->vy, p->coyote, p->run_sticky,
        p->gait_phase, p->gait_amount, G.cam_x, G.cam_x_max, G.cam_y
    };
    for (size_t i = 0; i < sizeof floats / sizeof floats[0]; i++)
        if (!isfinite(floats[i])) {
            if (err && len) snprintf(err, len, "non-finite float field %zu", i);
            return false;
        }

    if (G.state != GS_PLAYING) return true;   /* physics invariants below need a vault */

    float world_w = (float)(G.vault_data.cols * TILE_SIZE);
    float world_h = (float)(G.vault_data.rows * TILE_SIZE);
    if (p->x < -PLAYER_W - 2.0f || p->x > world_w + 2.0f ||
        p->y < -(float)LOGICAL_H || p->y > world_h + 2.0f) {
        if (err && len) snprintf(err, len, "player out of bounds (%.2f,%.2f)", p->x, p->y);
        return false;
    }
    if (fabsf(p->vx) > RUN_MAX + 2.0f ||
        p->vy < -(JUMP_V0_RUN + 2.0f) || p->vy > FASTFALL_MAX + 2.0f) {
        if (err && len) snprintf(err, len, "player velocity out of range (%.2f,%.2f)",
                                 p->vx, p->vy);
        return false;
    }
    if (box_blocked(p->x, p->y, false, false, 0.0f)) {
        if (err && len) snprintf(err, len, "player embedded in solid geometry");
        return false;
    }
    if (p->coyote < 0.0f || p->coyote > COYOTE_FRAMES + 0.01f ||
        p->run_sticky < 0.0f || p->run_sticky > RUN_STICKY + 0.01f) {
        if (err && len) snprintf(err, len, "player timer out of range");
        return false;
    }

    if (G.cam_x < -0.01f || G.cam_x > scroll_limit() + 0.01f ||
        G.cam_x > G.cam_x_max + 0.01f || G.cam_y < -0.01f) {
        if (err && len) snprintf(err, len, "camera out of range");
        return false;
    }
    return true;
}

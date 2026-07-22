/* Simulation core: the seeded RNG, Kilix's physics, per-axis tile collision, the
 * right-ratcheting camera, the input funnel, the deterministic autopilot, and the
 * per-tick validator.  The simulation never reads graphics state: collision boxes
 * are the named PLAYER_W/PLAYER_H constants, independent of the drawn silhouette. */
#include "super_kilix.h"

#include "kilix_state.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
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
    int t = v->tiles[row][col];
    return t >= T_HULL && t <= T_CONDUIT;         /* the contiguous solid range */
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

/* Head-clip corner correction (player-mechanics.md §4, CORNER_NUDGE).  When a rising
 * jump's head would clip a ceiling but a small sideways shift (<= CORNER_NUDGE px)
 * clears it, slip Kilix around the corner and let the jump continue instead of
 * head-bonking.  Bounded and deterministic (an integer search, no RNG); it only ever
 * moves the box to a position it has PROVEN is clear, so it can never tunnel or embed,
 * and it fires only while rising (dy < 0) so it can never yank a falling Kilix sideways
 * into a pit.  Called immediately before the vertical resolve. */
static void corner_nudge(float dy)
{
    Player *p = &G.player;
    /* Only a genuine head-clip: the straight-up move is blocked, yet the box is
     * currently clear (never mid-embed).  vertical=true excludes one-way grates from
     * the ceiling test (they are not head-bonk walls). */
    if (!box_blocked(p->x, p->y + dy, true, false, 0.0f)) return;
    if (box_blocked(p->x, p->y, true, false, 0.0f)) return;
    for (int s = 1; s <= (int)CORNER_NUDGE; s++)
        for (int sign = -1; sign <= 1; sign += 2) {
            float nx = p->x + (float)(sign * s);
            /* vertical=false: a side-embed test ignores one-way grates (not walls). */
            if (box_blocked(nx, p->y, false, false, 0.0f)) continue;   /* would embed sideways */
            if (box_blocked(nx, p->y + dy, true, false, 0.0f)) continue; /* still clipped up */
            p->x = nx;                                                  /* slip past the corner */
            return;
        }
}

/* ----------------------------------------------------------- player update */

static float horizontal_axis(bool left, bool right)
{
    return (right ? 1.0f : 0.0f) - (left ? 1.0f : 0.0f);
}

static void try_bonk_cache(void);   /* dispense a ?-node struck from below (§4.3) */

static void update_player(void)
{
    Player *p = &G.player;
    bool was_grounded = p->grounded;    /* for the land-SFX false->true edge */

    if (p->invuln > 0.0f) p->invuln = fmaxf(0.0f, p->invuln - TICK_DT);
    p->prev_bottom = p->y + PLAYER_H;   /* pre-move bottom for stomp arbitration */

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
        p->land_squash = -1.5f;             /* takeoff stretch (render-only, M7) */
        sound_play(SFX_JUMP, 0.70f, 1.0f);
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
    float vy_pre = p->vy;
    float dy = p->vy * TICK_DT;
    if (p->vy < 0.0f) corner_nudge(dy);     /* head-clip assist while rising (§4) */
    move_axis(dy, true, prev_bottom);
    if (vy_pre < 0.0f && p->vy == 0.0f) try_bonk_cache();   /* struck a ceiling while rising */
    if (p->grounded && !was_grounded) {
        sound_play(SFX_LAND, 0.55f, 1.0f);
        p->land_squash = 2.0f;              /* cosmetic landing squash (render-only, M7) */
    }

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

    /* Cosmetic squash/stretch relaxes back to rest each tick (bounded; render-only). */
    if (p->land_squash > 0.0f)      p->land_squash = fmaxf(0.0f, p->land_squash - 1.0f);
    else if (p->land_squash < 0.0f) p->land_squash = fminf(0.0f, p->land_squash + 1.0f);

    /* The airborne stomp chain resets on landing; while airborne it also lapses if
     * the per-stomp decay window elapses without another defeat (so hovering on the
     * thruster between stomps cannot bank an indefinite chain). */
    if (p->grounded) {
        G.chain = 0;
        G.chain_decay = 0;
    } else if (G.chain > 0) {
        if (G.chain_decay > 0) G.chain_decay--;
        if (G.chain_decay <= 0) G.chain = 0;
    }
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

/* ===================================================================== *
 *  Machine substrate (M4a)                                              *
 *                                                                       *
 *  The shared enemy core the whole roster builds on: a MoveNormalEnemy-  *
 *  style walker, an enemy-vs-background AABB collision separate from the *
 *  player's sub-stepped tile resolver, a camera-relative spawn schedule  *
 *  with a capped slot pool, a coarse interval quantum driving Husk       *
 *  revival, and the interleaved collision passes.  Two families ship     *
 *  here (Slag-Treader walker + Carapod shelled turner, cast.md §5.2/5.3);*
 *  later M4 agents add families by extending the EN_* enum and dispatch.  *
 * ===================================================================== */

static bool overlap(float ax, float ay, float aw, float ah,
                    float bx, float by, float bw, float bh)
{
    return ax < bx + bw && ax + aw > bx && ay < by + bh && ay + ah > by;
}

/* The lethal/solid AABB shrinks with the machine's sub-state (a retracted Husk
 * is rounder and lower; a flattened walker is a sliver), independent of any drawn
 * silhouette — the sim never reads graphics state. */
static bool enemy_is_boss(const Enemy *e)
{
    return e->kind == EN_GUARDIAN || e->kind == EN_OVERSEER;
}

static float enemy_box_w(const Enemy *e)
{
    return enemy_is_boss(e) ? GUARDIAN_W : ENEMY_W;
}
static float enemy_box_h(const Enemy *e)
{
    if (enemy_is_boss(e)) return GUARDIAN_H;   /* the boss keeps its bulk in every state */
    unsigned sub = e->state & ES_SUBSTATE;
    if (sub == ES_HUSK)     return HUSK_H;
    if (sub == ES_SQUASHED) return SQUASH_H;
    return ENEMY_H;
}

/* Enemy-vs-background: the machine AABB against the semantic-solidity grid.  A
 * small inset keeps a box resting flush on a surface from counting the adjacent
 * cell.  Machines ignore one-way grates (game_tile_solid already excludes them),
 * so no M4a family stands on a ledge — a deliberate, cheap simplification. */
static bool enemy_hits_solid(const Enemy *e, float x, float y)
{
    float w = enemy_box_w(e), h = enemy_box_h(e);
    int x0 = (int)floorf((x + 0.5f) / TILE_SIZE);
    int x1 = (int)floorf((x + w - 0.5f) / TILE_SIZE);
    int y0 = (int)floorf((y + 0.5f) / TILE_SIZE);
    int y1 = (int)floorf((y + h - 0.5f) / TILE_SIZE);
    for (int row = y0; row <= y1; row++)
        for (int col = x0; col <= x1; col++)
            if (game_tile_solid(col, row)) return true;
    return false;
}

static bool enemy_on_ground(const Enemy *e)
{
    return enemy_hits_solid(e, e->x, e->y + 1.0f);
}

/* Is there a solid surface directly beneath the leading foot?  Probing the cell
 * just below-and-ahead (never far below) is what turns a terrace edge into a
 * ledge even when a lower floor exists further down. */
static bool enemy_has_floor_ahead(const Enemy *e, float dir)
{
    float w = enemy_box_w(e), h = enemy_box_h(e);
    float probe_x = dir >= 0.0f ? e->x + w + 1.0f : e->x - 1.0f;
    float probe_y = e->y + h + 2.0f;
    int tx = (int)floorf(probe_x / TILE_SIZE);
    int ty = (int)floorf(probe_y / TILE_SIZE);
    return game_tile_solid(tx, ty);
}

/* The corrected red-vs-green reading (corrections.md): the ledge-turn is gated on
 * an ID check with sub-state 0 (walking), NOT an extra state flag.  Only the
 * turner family (Carapod) reverses at a ledge; the base walker (Slag-Treader)
 * walks off.  Keep it a one-line specialisation, not a separate class. */
static bool enemy_turns_at_ledge(int kind) { return kind == EN_TURNER; }

/* Gravity + a single vertical resolve (revert-on-hit never embeds). */
static void enemy_fall(Enemy *e)
{
    e->vy = fminf(e->vy + ENEMY_GRAVITY * TICK_DT, ENEMY_FALL_MAX);
    float dy = e->vy * TICK_DT;
    e->y += dy;
    if (enemy_hits_solid(e, e->x, e->y)) { e->y -= dy; e->vy = 0.0f; }
}

/* A MoveNormalEnemy-style walk: forward at the family speed, reverse on a wall,
 * and (turner only, walking) reverse at a ledge — then gravity. */
static void walk_step(Enemy *e, float speed)
{
    e->vx = (float)e->facing * speed;
    if (enemy_turns_at_ledge(e->kind) && (e->state & ES_SUBSTATE) == ES_WALK &&
        enemy_on_ground(e) && !enemy_has_floor_ahead(e, (float)e->facing)) {
        e->facing = -e->facing;
        e->vx = -e->vx;
    }
    float dx = e->vx * TICK_DT;
    e->x += dx;
    if (enemy_hits_solid(e, e->x, e->y)) { e->x -= dx; e->facing = -e->facing; }
    enemy_fall(e);
}

/* A kicked Husk: fast, bounces off walls to reverse, and falls under gravity. */
static void husk_slide_step(Enemy *e)
{
    e->vx = (float)e->facing * HUSK_SPEED;
    float dx = e->vx * TICK_DT;
    e->x += dx;
    if (enemy_hits_solid(e, e->x, e->y)) { e->x -= dx; e->facing = -e->facing; }
    enemy_fall(e);
}

/* ------------------------------------------------- spawn schedule / pool */

static Enemy *free_enemy_slot(void)
{
    for (int i = 0; i < MAX_ACTIVE_ENEMIES; i++)
        if (!G.enemies[i].active) return &G.enemies[i];
    return NULL;   /* pool full: the caller drops the spawn (the density cap) */
}

/* Which SKLF roster ids have live behaviour.  The decoded spawn id IS the EN_*
 * family id (no lookup table), so the reserved-but-unshipped roster slots
 * (Carapod-Kite, Dreadpod, the aquatic pursuers, the aerial/boss ids) are
 * dropped here exactly as "not shipped yet" until a later milestone lands them. */
static bool family_shipped(int kind)
{
    return kind == EN_WALKER || kind == EN_TURNER ||
           kind == EN_MAW    || kind == EN_RIVETER ||
           kind == EN_GUARDIAN || kind == EN_OVERSEER;
}

static void spawn_enemy(int kind, int col, int row, uint8_t param)
{
    if (kind < 0 || kind >= ENEMY_KIND_COUNT || !family_shipped(kind))
        return;                                        /* family not shipped yet */
    Enemy *e = free_enemy_slot();
    if (!e) return;
    memset(e, 0, sizeof *e);
    e->kind = kind;
    e->state = ES_WALK;
    /* Position from the family's own box so a wide/tall boss still rests feet-on-floor. */
    float bw = enemy_box_w(e), bh = enemy_box_h(e);
    e->x = (float)(col * TILE_SIZE) + (TILE_SIZE - bw) * 0.5f;
    e->y = (float)((row + 1) * TILE_SIZE) - bh;        /* feet on the row below */
    if (enemy_hits_solid(e, e->x, e->y)) return;       /* refuse an embedded spawn */
    e->active = true;
    e->home_x = e->x;
    e->home_y = e->y;
    e->facing = -1;    /* machines enter facing left, toward the approaching player */
    e->param = param;
    /* The Vent-Maw treats its resting box as the flush vent; its emerge motion is
     * its own activation tell, so it skips the shared tell ramp.  Both cadence
     * families open on the coarse quantum after an initial dwell. */
    if (kind == EN_MAW) {
        e->tell = 1.0f;
        e->phase_q = MAW_HIDE_Q;
    } else if (kind == EN_RIVETER) {
        e->phase_q = RIVETER_THROW_Q;
    } else if (enemy_is_boss(e)) {
        /* A boss is a set-piece: always awake, never re-telegraphs, holds its
         * dais.  revive_q carries the core-overload HP; phase_q drives the hatch. */
        e->alert = 1.0f;
        e->tell = 1.0f;
        e->revive_q = GUARDIAN_CORE_HP;
        e->phase_q = GUARDIAN_HATCH_Q;
    }
}

/* ---------------------------------------------------------- projectile pool */

static Projectile *free_projectile_slot(void)
{
    for (int i = 0; i < MAX_PROJECTILES; i++)
        if (!G.projectiles[i].active) return &G.projectiles[i];
    return NULL;   /* pool full: the lob is dropped (the on-screen rivet cap) */
}

/* Each light-actor family carries its own small AABB (never an art-derived value). */
static float projectile_w(const Projectile *p)
{
    return p->kind == PJ_PLASMA ? PLASMA_W : p->kind == PJ_PULSE ? PULSE_W : RIVET_W;
}
static float projectile_h(const Projectile *p)
{
    return p->kind == PJ_PLASMA ? PLASMA_H : p->kind == PJ_PULSE ? PULSE_H : RIVET_H;
}

/* Does a light actor's AABB overlap blocking geometry?  A tighter query than the
 * player's — these boxes are small, so a single-cell scan of the corners suffices. */
static bool projectile_hits_solid(const Projectile *p)
{
    float w = projectile_w(p), h = projectile_h(p);
    int x0 = (int)floorf(p->x / TILE_SIZE);
    int x1 = (int)floorf((p->x + w) / TILE_SIZE);
    int y0 = (int)floorf(p->y / TILE_SIZE);
    int y1 = (int)floorf((p->y + h) / TILE_SIZE);
    for (int row = y0; row <= y1; row++)
        for (int col = x0; col <= x1; col++)
            if (game_tile_solid(col, row)) return true;
    return false;
}

/* Launch a rivet from (cx,cy) in a ballistic arc toward `dir`. */
static void spawn_rivet(float cx, float cy, int dir)
{
    Projectile *p = free_projectile_slot();
    if (!p) return;
    memset(p, 0, sizeof *p);
    p->active = true;
    p->kind = PJ_RIVET;
    p->x = cx - RIVET_W * 0.5f;
    p->y = cy - RIVET_H * 0.5f;
    p->vx = (float)dir * RIVET_SPEED;
    p->vy = RIVET_VY0;                 /* an upward launch gives the parabolic arc */
    p->life = RIVET_LIFE;
    p->facing = dir >= 0 ? 1 : -1;
}

/* Launch a Guardian plasma bolt from (cx,cy): a slow, gravity-free horizontal shot. */
static void spawn_plasma(float cx, float cy, int dir)
{
    Projectile *p = free_projectile_slot();
    if (!p) return;
    memset(p, 0, sizeof *p);
    p->active = true;
    p->kind = PJ_PLASMA;
    p->x = cx - PLASMA_W * 0.5f;
    p->y = cy - PLASMA_H * 0.5f;
    p->vx = (float)dir * PLASMA_SPEED;
    p->vy = 0.0f;
    p->life = PLASMA_LIFE;
    p->facing = dir >= 0 ? 1 : -1;
}

/* Emit a Charged-Kilix phase-bolt in his facing (cap MAX_PULSES on screen). */
static void spawn_pulse(void)
{
    int live = 0;
    for (int i = 0; i < MAX_PROJECTILES; i++)
        if (G.projectiles[i].active && G.projectiles[i].kind == PJ_PULSE) live++;
    if (live >= MAX_PULSES) return;
    Projectile *p = free_projectile_slot();
    if (!p) return;
    int dir = G.player.facing >= 0 ? 1 : -1;
    memset(p, 0, sizeof *p);
    p->active = true;
    p->kind = PJ_PULSE;
    p->x = G.player.x + (dir > 0 ? PLAYER_W : -PULSE_W);
    p->y = G.player.y + 4.0f;
    p->vx = (float)dir * PULSE_SPEED;
    p->vy = PULSE_VY0;
    p->life = PULSE_LIFE;
    p->facing = dir;
    sound_play(SFX_PHASE_BOLT, 0.70f, 1.0f);
}

/* One spawn record -> one machine, or a 2-3 walker cluster for the group token. */
static void materialise_spawn(const EnemySpawn *s)
{
    if (s->kind == ENEMY_GROUP_TOKEN) {
        int count = 2 + (s->param & 1u);
        for (int k = 0; k < count; k++)
            spawn_enemy(EN_WALKER, (int)s->col + k * 2, (int)s->row, s->param);
    } else {
        spawn_enemy((int)s->kind, (int)s->col, (int)s->row, s->param);
    }
}

/* Materialise every scheduled machine whose column has entered the band just past
 * the right edge.  The cursor only advances because the camera ratchets right, so
 * a column is spawned exactly once (an over-full window silently drops the later
 * machine, exactly as level-grammar §9 specifies). */
static void spawn_scheduled_enemies(void)
{
    const VaultData *v = &G.vault_data;
    float right = G.cam_x + (float)LOGICAL_W + SPAWN_BAND;
    while (G.spawn_cursor < v->enemy_count) {
        const EnemySpawn *s = &v->enemies[G.spawn_cursor];
        if ((float)((int)s->col * TILE_SIZE) > right) break;
        materialise_spawn(s);
        G.spawn_cursor++;
    }
}

/* The Vent-Maw telescopes out of its home column: emerge ramps toward its phase
 * target (0 hidden, 1 out), the box top rides `home_y - emerge*MAW_RISE`, and a
 * rise into a ceiling is clamped so the head never embeds.  The out-phase toggle
 * and its suppression live on the coarse quantum (enemy_quantum_tick). */
static void maw_step(Enemy *e, bool dormant)
{
    if (dormant) e->state &= (uint8_t)~ES_EMERGED;
    float target = (e->state & ES_EMERGED) ? 1.0f : 0.0f;
    float prev = e->emerge;
    if (e->emerge < target)
        e->emerge = fminf(target, e->emerge + MAW_EMERGE_RATE * TICK_DT);
    else if (e->emerge > target)
        e->emerge = fmaxf(target, e->emerge - MAW_EMERGE_RATE * TICK_DT);
    float want_y = e->home_y - e->emerge * MAW_RISE;
    if (e->emerge > prev && enemy_hits_solid(e, e->x, want_y)) {
        e->emerge = prev;                       /* rising into a ceiling: stop here */
        want_y = e->home_y - e->emerge * MAW_RISE;
    }
    e->y = want_y;
    e->vx = e->vy = 0.0f;
}

/* The Riveter shimmies within a tile of its home on a ledge (reversing at walls
 * and ledges, so it never walks into a pit) and faces its shimmy; its lobs fire
 * on the coarse quantum toward Kilix's side (enemy_quantum_tick). */
static void riveter_step(Enemy *e)
{
    if (e->facing == 0) e->facing = -1;
    if ((e->facing < 0 && e->x <= e->home_x - RIVETER_RANGE) ||
        (e->facing > 0 && e->x >= e->home_x + RIVETER_RANGE))
        e->facing = -e->facing;
    if (enemy_on_ground(e) && !enemy_has_floor_ahead(e, (float)e->facing))
        e->facing = -e->facing;
    e->vx = (float)e->facing * RIVETER_SHIMMY;
    float dx = e->vx * TICK_DT;
    e->x += dx;
    if (enemy_hits_solid(e, e->x, e->y)) { e->x -= dx; e->facing = -e->facing; }
    enemy_fall(e);
}

/* -------------------------------------------------- Vault Guardian (the boss) */

/* Is the chest hatch open far enough to expose the core?  The emerge field is the
 * hatch-open amount (0 sealed .. 1 wide), so this is a pure geometric read — the
 * only window a Phase Pulse or a routed Husk can crack the shell (cast.md §5.12). */
static bool guardian_hatch_open(const Enemy *e)
{
    return e->emerge >= GUARDIAN_HATCH_OPEN;
}

/* Per-district attack hook (cast.md §5.12 / level-grammar §3.2): plasma arcs in
 * the early districts, rivet-cluster volleys in the mid districts, both in the
 * finale.  A cheap projectile-vs-projectile variety split, no per-boss code. */
enum { GATT_PLASMA, GATT_RIVET, GATT_BOTH };
static int guardian_style(int district)
{
    if (district >= 8) return GATT_BOTH;
    if (district >= 5) return GATT_RIVET;
    return GATT_PLASMA;
}

/* Fire the district's ranged attack toward Kilix's side (called on the quantum). */
static void guardian_attack(Enemy *e)
{
    int style = guardian_style(G.vault_data.district);
    float cx = e->x + GUARDIAN_W * 0.5f;
    float cy = e->y + GUARDIAN_H * 0.4f;
    int aim = (G.player.x + PLAYER_W * 0.5f) >= cx ? 1 : -1;
    if (style == GATT_PLASMA || style == GATT_BOTH) spawn_plasma(cx, cy, aim);
    if (style == GATT_RIVET  || style == GATT_BOTH) {
        spawn_rivet(cx, cy, aim);                 /* a small rivet cluster */
        spawn_rivet(cx, cy - 5.0f, aim);
    }
}

/* The Guardian paces its dais within a fixed range of its anchor, reversing at
 * walls and the range bounds, under gravity — the slow, coarse warden motion.
 * Its melee threat is body contact (handled in the player pass); its ranged
 * attacks + hatch cadence run on the quantum (enemy_quantum_tick). */
static void guardian_step(Enemy *e, bool dormant)
{
    if (!dormant) {
        if (e->facing == 0) e->facing = -1;
        if (e->x <= e->home_x - GUARDIAN_RANGE)      e->facing = 1;
        else if (e->x >= e->home_x + GUARDIAN_RANGE) e->facing = -1;
        e->vx = (float)e->facing * GUARDIAN_PACE;
        float dx = e->vx * TICK_DT;
        e->x += dx;
        if (enemy_hits_solid(e, e->x, e->y)) { e->x -= dx; e->facing = -e->facing; }
    } else {
        e->vx = 0.0f;
    }
    enemy_fall(e);
    /* Animate the chest hatch toward its cadence target (ES_EMERGED = opening). */
    float target = (e->state & ES_EMERGED) ? 1.0f : 0.0f;
    if (e->emerge < target)
        e->emerge = fminf(target, e->emerge + GUARDIAN_HATCH_RATE * TICK_DT);
    else if (e->emerge > target)
        e->emerge = fmaxf(target, e->emerge - GUARDIAN_HATCH_RATE * TICK_DT);
}

/* --------------------------------------------------------- per-machine step */

static void update_enemy(Enemy *e)
{
    if (!e->active) return;
    unsigned sub = e->state & ES_SUBSTATE;

    if (sub == ES_SQUASHED) {                 /* flattened walker: linger, then erase */
        e->squash -= TICK_DT;
        e->vx = 0.0f;
        enemy_fall(e);
        if (e->squash <= 0.0f) e->active = false;
        return;
    }
    if (sub == ES_HUSK) {                      /* retracted shell */
        if (e->state & ES_SHELL_MOV) husk_slide_step(e);   /* kicked: sliding hazard */
        else { e->vx = 0.0f; enemy_fall(e); }              /* dormant: revives on quantum */
        return;
    }

    /* ES_WALK: local activation.  A machine is dormant beyond ALERT_RANGE, wakes
     * inside it, telegraphs (a stationary, nonlethal tell), then acts — the JPAK
     * "dormant, readable tell, then it acts" contract. */
    float px = G.player.x + PLAYER_W * 0.5f, py = G.player.y + PLAYER_H * 0.5f;
    float ex = e->x + ENEMY_W * 0.5f, ey = e->y + enemy_box_h(e) * 0.5f;
    float dx = px - ex, dy = py - ey, d2 = dx * dx + dy * dy;
    bool dormant = false;
    if (e->alert <= 0.0f) {
        if (d2 >= ALERT_RANGE * ALERT_RANGE) dormant = true;
        else e->alert = 1.0f;                  /* wake (machines stay awake thereafter) */
    } else if (d2 < (ALERT_RANGE * 1.5f) * (ALERT_RANGE * 1.5f)) {
        e->alert = 1.0f;
    }

    /* The emerger anchors to its vent (no gravity/tell ramp): its emerge motion
     * both telegraphs and threatens, and a dormant Maw simply retracts. */
    if (e->kind == EN_MAW) { maw_step(e, dormant); return; }

    /* The boss is always awake once spawned (alert latched at spawn), so `dormant`
     * is effectively false; it paces and works its hatch cadence regardless. */
    if (enemy_is_boss(e)) { guardian_step(e, dormant); return; }

    if (dormant) { e->vx = 0.0f; enemy_fall(e); return; }
    if (e->tell < 1.0f) {                       /* telegraphing: ramp, do not move */
        e->tell = fminf(1.0f, e->tell + TELL_RATE * TICK_DT);
        e->vx = 0.0f;
        enemy_fall(e);
        return;
    }
    if (e->kind == EN_RIVETER) { riveter_step(e); return; }
    walk_step(e, e->kind == EN_TURNER ? CARAPOD_SPEED : TREADER_SPEED);
}

/* Is Kilix inside the Vent-Maw's horizontal suppression band?  A purely
 * horizontal check (cast.md §5.5): stand beside the vent and it stays down. */
static bool maw_suppressed(const Enemy *e)
{
    float pcx = G.player.x + PLAYER_W * 0.5f;
    float vcx = e->home_x + ENEMY_W * 0.5f;
    return fabsf(pcx - vcx) < MAW_SUPPRESS;
}

/* The coarse interval quantum (SK_QUANTUM ticks, Kilix's own coarse timing, not
 * the studied cadences): it drives the slow durations.  M4a's driver is Husk
 * revival; M4b adds the Vent-Maw emerge/hide cycle and the Riveter lob cadence —
 * one subsystem, not per-enemy smooth counters (cast.md §7). */
static void enemy_quantum_tick(void)
{
    /* The Aegis window ticks down on the same coarse quantum (cast.md §4.2): it
     * expires on the quantum, not by a smooth per-frame counter. */
    if (G.player.aegis_q > 0) G.player.aegis_q--;

    for (int i = 0; i < MAX_ACTIVE_ENEMIES; i++) {
        Enemy *e = &G.enemies[i];
        if (!e->active) continue;
        unsigned sub = e->state & ES_SUBSTATE;

        if (sub == ES_HUSK && !(e->state & ES_SHELL_MOV) && e->revive_q > 0) {
            e->revive_q--;
            if (e->revive_q <= 0) {
                e->state = (uint8_t)((e->state & ~ES_SUBSTATE) | ES_WALK);
                /* The walker box is ENEMY_H tall vs the shorter HUSK_H shell; it
                 * grows downward (box top is e->y, feet are e->y + h).  A dormant
                 * Husk rests feet-flush on the floor, so lift the box top by the
                 * height gain to keep the revived feet put — else they embed. */
                e->y -= (ENEMY_H - HUSK_H);
                e->facing = game_randf() < 0.5f ? -1 : 1;   /* coin-flip revival */
                e->tell = 1.0f;    /* already active — no re-telegraph */
                e->alert = 1.0f;
            }
            continue;
        }

        if (e->kind == EN_MAW && e->alert > 0.0f) {
            if (e->phase_q > 0) e->phase_q--;
            if (e->phase_q <= 0) {
                if (e->state & ES_EMERGED) {              /* out -> retract, hold hidden */
                    e->state &= (uint8_t)~ES_EMERGED;
                    e->phase_q = MAW_HIDE_Q;
                } else if (!maw_suppressed(e)) {          /* clear to rise */
                    e->state |= ES_EMERGED;
                    e->phase_q = MAW_OUT_Q;
                } else {
                    e->phase_q = 1;                       /* suppressed: recheck next quantum */
                }
            }
        } else if (e->kind == EN_RIVETER && e->alert > 0.0f && e->tell >= 1.0f) {
            if (e->phase_q > 0) e->phase_q--;
            if (e->phase_q <= 0) {
                e->phase_q = RIVETER_THROW_Q;
                int aim = (G.player.x + PLAYER_W * 0.5f) >=
                          (e->x + ENEMY_W * 0.5f) ? 1 : -1;
                spawn_rivet(e->x + ENEMY_W * 0.5f, e->y + 2.0f, aim);
            }
        } else if (enemy_is_boss(e) && e->alert > 0.0f) {
            /* One cadence counter drives the whole set piece: the hatch seals for
             * GUARDIAN_HATCH_Q quanta, then opens for GUARDIAN_OPEN_Q (the core
             * tell), and a ranged volley fires each time it re-seals. */
            if (e->phase_q > 0) e->phase_q--;
            if (e->phase_q <= 0) {
                if (e->state & ES_EMERGED) {
                    e->state &= (uint8_t)~ES_EMERGED;
                    e->phase_q = GUARDIAN_HATCH_Q;
                    guardian_attack(e);
                } else {
                    e->state |= ES_EMERGED;
                    e->phase_q = GUARDIAN_OPEN_Q;
                }
            }
        }
    }
}

static void update_enemies(void)
{
    for (int i = 0; i < MAX_ACTIVE_ENEMIES; i++) update_enemy(&G.enemies[i]);
    if (G.tick % (uint64_t)SK_QUANTUM == 0) enemy_quantum_tick();
}

/* ------------------------------------------------------------- interactions */

/* The one doubling chain table (cast.md §6): rungs 1..7 award 100..6400; the 8th
 * rung is the EXTRA-UNIT sentinel — a spare unit in place of points.  A single
 * table serves both the airborne stomp chain (entry rung 1) and the sliding-Husk
 * mow chain (entry rung 2 — it "starts higher"). */
static const int chain_score[7] = { 100, 200, 400, 800, 1600, 3200, 6400 };

/* Spare units at fixed score thresholds (once each): each time the running score
 * crosses an EXTRA_LIFE_STEP boundary, one spare unit is banked. */
static void grant_score_thresholds(void)
{
    while (G.next_extra_life > 0 && G.score >= G.next_extra_life) {
        G.lives++;
        G.next_extra_life += EXTRA_LIFE_STEP;
        sound_play(SFX_EXTRA_LIFE, 0.80f, 1.0f);
    }
}

/* Advance a doubling chain counter and award its rung.  The counter caps at
 * CHAIN_MAX (8); reaching exactly the 8th rung banks a spare unit ONCE (the
 * EXTRA-UNIT sentinel — the M6 fix: on the 8th chained defeat, NOT the 7th); a
 * chain already parked at the top keeps awarding the 6400 rung. */
static void award_chain(int *counter)
{
    int prev = *counter;
    if (*counter < CHAIN_MAX) (*counter)++;
    if (*counter >= CHAIN_MAX) {
        if (prev < CHAIN_MAX) {                     /* the 8th chained defeat: EXTRA UNIT */
            G.lives++;
            sound_play(SFX_EXTRA_LIFE, 0.80f, 1.0f);
        } else {
            G.score += chain_score[6];              /* parked at the top: keep the 6400 rung */
        }
    } else {
        G.score += chain_score[*counter - 1];       /* rungs 1..7 -> 100..6400 */
    }
    grant_score_thresholds();
}

/* An airborne stomp / Aegis-contact defeat: a chained hit climbs G.chain (reset on
 * landing, in update_player); a single grounded defeat scores the base rung.  The
 * sliding-Husk mow chain does NOT come through here — it has its own per-Husk
 * counter (award_husk_mow), independent of this land-to-reset (cast.md §6). */
static void award_defeat(bool chained)
{
    if (chained) {
        award_chain(&G.chain);
        G.chain_decay = CHAIN_DECAY;                 /* refresh the per-stomp decay window */
    } else {
        G.chain = 1;
        G.score += chain_score[0];                  /* 100, the base rung */
        grant_score_thresholds();
    }
}

/* A sliding Husk mows a machine: escalate the HUSK'S OWN chain (reset when the Husk
 * stops / re-dormants, NOT on Kilix landing).  This is the deferred M6 fix: a Husk
 * mowing a line of machines while Kilix stands still now escalates properly. */
static void award_husk_mow(Enemy *husk)
{
    award_chain(&husk->mow_chain);
}

/* The steep top-heavy end-of-vault reward (level-grammar §10.3).  Leftover charge
 * picks a riser-height band on a top-heavy ladder (each rung ~2-2.5x the last, a
 * 30x spread bottom->top), so a fast finish is worth far more than a slow one; the
 * leftover charge also converts linearly at CHARGE_CONVERT/unit on top. */
static const int riser_band[5] = { 200, 500, 1000, 2500, 6000 };

int game_exit_band(int charge_left, int charge_start)
{
    if (charge_start <= 0) return 0;
    if (charge_left < 0) charge_left = 0;
    if (charge_left > charge_start) charge_left = charge_start;
    int band = charge_left * 5 / charge_start;      /* 0..5 -> clamp to the 5 bands */
    return band > 4 ? 4 : band;
}

int game_exit_score(int charge_left, int charge_start)
{
    if (charge_left < 0) charge_left = 0;
    return riser_band[game_exit_band(charge_left, charge_start)]
         + CHARGE_CONVERT * charge_left;
}

/* Spend a spare unit and enter the brief life-lost beat (cast.md §4, damage while
 * Bare / a void-or-ember fall / charge expiry).  Shared by contact damage and the
 * charge-timer expiry path. */
static void lose_life(void)
{
    if (G.state != GS_PLAYING) return;
    G.lives--;
    G.deaths++;
    G.chain = 0;
    G.state = GS_LIFE_LOST;
    G.state_timer = 0.5f;
    sound_play(SFX_HURT, 0.85f, 0.9f);
}

/* Contact damage: demote a tier if armoured, else lose a life and restart the
 * vault (cast.md §4).  i-frames suppress a second hit in the same overlap. */
static void hurt_player(void)
{
    Player *p = &G.player;
    if (G.state != GS_PLAYING) return;   /* a death already resolved this tick */
    if (p->invuln > 0.0f || p->aegis_q > 0) return;   /* i-frames or Aegis: no harm */
    if (p->power_tier > 0) {
        sound_play(SFX_HURT, 0.80f, 1.0f);
        p->power_tier--;
        p->invuln = HIT_INVULN;
        p->vy = -120.0f;          /* the family hit-hop knockback */
        p->jumping = false;
        G.hitstop = HITSTOP_HEAVY;  /* a heavier freeze on taking a hit (M7 juice) */
        render_shake(7.0f);
    } else {
        render_shake(7.0f);
        lose_life();
    }
}

/* A stomp: the walker flattens and is erased; the turner retracts to a dormant,
 * revivable Husk.  Either way Kilix takes the FLAT bounce (no hold modifier). */
static void stomp_machine(Enemy *e)
{
    if (e->kind == EN_TURNER) {
        e->state = (uint8_t)((e->state & ~ES_SUBSTATE) | ES_HUSK);
        e->state &= (uint8_t)~ES_SHELL_MOV;
        e->vx = 0.0f;
        e->revive_q = HUSK_REVIVE_Q;
    } else {
        e->state = (uint8_t)((e->state & ~ES_SUBSTATE) | ES_SQUASHED);
        e->vx = 0.0f;
        e->squash = SQUASH_TIME;
    }
    G.player.vy = -STOMP_BOUNCE;
    G.player.jumping = false;     /* the floaty rise-gravity can never lengthen it */
    G.hitstop = HITSTOP_LIGHT;    /* a brief, bounded impact freeze (M7 juice) */
    render_shake(3.0f);           /* render-time only: never stored in G */
    sound_play(SFX_STOMP, 0.80f, 1.0f);
    award_defeat(!G.player.grounded);
}

/* Launch a dormant Husk into a sliding hazard, away from Kilix.  Its OWN mow chain
 * starts at the Husk entry rung (HUSK_CHAIN_ENTRY) so the first machine it mows
 * scores one rung above the airborne base (cast.md §6, "starts higher"). */
static void kick_husk(Enemy *e)
{
    float pcx = G.player.x + PLAYER_W * 0.5f, ecx = e->x + ENEMY_W * 0.5f;
    e->facing = ecx >= pcx ? 1 : -1;
    e->state |= ES_SHELL_MOV;
    e->vx = (float)e->facing * HUSK_SPEED;
    e->revive_q = 0;
    e->mow_chain = HUSK_CHAIN_ENTRY;
    sound_play(SFX_SHELL_KICK, 0.75f, 1.0f);
}

/* Re-dormant a sliding Husk (a second stomp stops it), restarting its revival and
 * clearing its per-Husk mow chain (the M6 fix: the Husk chain resets when it stops,
 * not on Kilix landing). */
static void stop_husk(Enemy *e)
{
    e->state &= (uint8_t)~ES_SHELL_MOV;
    e->vx = 0.0f;
    e->revive_q = HUSK_REVIVE_Q;
    e->mow_chain = 0;
}

/* A sliding Husk mows a machine: it flattens and is erased, escalating the mowing
 * Husk's OWN chain (per-Husk counter, independent of G.chain). */
static void defeat_machine(Enemy *victim, Enemy *husk)
{
    victim->state = (uint8_t)((victim->state & ~ES_SUBSTATE) | ES_SQUASHED);
    victim->state &= (uint8_t)~ES_SHELL_MOV;
    victim->vx = 0.0f;
    victim->squash = SQUASH_TIME;
    award_husk_mow(husk);
}

/* ------------------------------------------------- Vault Guardian kill paths */

/* Drop the boss into the sump.  Core-overload cracks the decoy shell (`unmask`,
 * the big score + reveal); the seal-switch collapse never unmasks it (cast.md
 * §5.12).  Either path sets guardian_down so the vault can later be cleared. */
static void guardian_defeat(Enemy *e, bool unmask)
{
    if (!e->active) return;
    e->active = false;
    G.guardian_down = true;
    if (unmask) { G.guardian_unmasked = true; G.score += SCORE_GUARDIAN; }
    else        { G.score += SCORE_SEAL; }
    G.hitstop = HITSTOP_MED;                  /* the collapse lands with weight (M7) */
    render_shake(8.0f);                       /* the dais collapse rocks the arena (M7) */
    sound_play(SFX_EXIT_OPEN, 0.80f, 1.0f);   /* the dais collapses: the way out opens */
}

/* A powered hit on the exposed core (a Phase Pulse or a routed Husk): it only
 * lands while the hatch is open; GUARDIAN_CORE_HP hits crack the shell. */
static void guardian_core_hit(Enemy *e)
{
    if (!guardian_hatch_open(e)) return;        /* the plating deflects a sealed core */
    sound_play(SFX_BOSS_HIT, 0.85f, 1.0f);
    G.hitstop = HITSTOP_MED;                    /* a core crack lands with weight (M7) */
    render_shake(5.0f);
    if (e->revive_q > 0) e->revive_q--;
    if (e->revive_q <= 0) guardian_defeat(e, true);
}

/* Kilix's phase-bolt strikes a machine: it dissolves most of the roster outright
 * (worth score), and only damages a boss at its exposed core (else it fizzles). */
static void pulse_hits_machine(Enemy *e)
{
    if (enemy_is_boss(e)) { guardian_core_hit(e); return; }
    unsigned sub = e->state & ES_SUBSTATE;
    if (sub == ES_SQUASHED) return;
    e->state = (uint8_t)((e->state & ~ES_SUBSTATE) | ES_SQUASHED);
    e->state &= (uint8_t)~ES_SHELL_MOV;
    e->vx = 0.0f;
    e->squash = SQUASH_TIME;
    award_defeat(false);
}

/* ---------------------------------------------------- power-up ladder + Aegis */

/* Dispense one cache payload (cast.md §4).  A power block yields the NEXT
 * phase-shell tier by Kilix's current tier (state-dependent, never wasted); at
 * the top tier it awards score instead of repeating a tier. */
/* Bank collected motes toward spare units: MOTES_PER_UNIT motes grant one spare
 * unit (level-grammar §10.3), so a full cache of motes both scores and edges toward
 * a life. */
static void collect_motes(int count)
{
    G.motes += count;
    while (G.motes >= MOTES_PER_UNIT) {
        G.motes -= MOTES_PER_UNIT;
        G.lives++;
        sound_play(SFX_EXTRA_LIFE, 0.80f, 1.0f);
    }
}

static void apply_powerup(int content)
{
    Player *p = &G.player;
    switch (content) {
    case CN_POWER:
        if (p->power_tier < 2) { p->power_tier++; sound_play(SFX_POWER_UP, 0.80f, 1.0f); }
        else                   { G.score += SCORE_POWER_FULL; sound_play(SFX_PICKUP, 0.75f, 1.0f); }
        break;
    case CN_AEGIS: p->aegis_q = AEGIS_Q;   sound_play(SFX_POWER_UP, 0.80f, 1.0f); break;
    case CN_MULTI: collect_motes(3); G.score += SCORE_MULTI; sound_play(SFX_PICKUP, 0.75f, 1.0f); break;
    case CN_SHELL: G.lives++;              sound_play(SFX_EXTRA_LIFE, 0.80f, 1.0f); break;  /* a spare unit */
    case CN_MOTE:
    default:       collect_motes(1); G.score += SCORE_MOTE; sound_play(SFX_PICKUP, 0.70f, 1.0f); break;
    }
    grant_score_thresholds();
}

/* On a rising head-bonk, empty the struck cache node to a spent block and dispense
 * its payload once (cast.md §4.3, the ?-node bonk).  The payload is level-data
 * (v->caches), so the simulation reads no art-derived value. */
static void try_bonk_cache(void)
{
    Player *p = &G.player;
    int row = (int)floorf((p->y + 0.12f) / TILE_SIZE) - 1;   /* the ceiling cell row */
    int c0  = (int)floorf((p->x + 0.12f) / TILE_SIZE);
    int c1  = (int)floorf((p->x + PLAYER_W - 0.12f) / TILE_SIZE);
    for (int col = c0; col <= c1; col++) {
        if (tile_cell(col, row) != T_CACHE) continue;
        VaultData *v = &G.vault_data;
        v->tiles[row][col] = T_SPENT;
        int content = CN_MOTE;
        for (int i = 0; i < v->cache_count; i++)
            if ((int)v->caches[i].col == col && (int)v->caches[i].row == row) {
                content = (int)v->caches[i].content;
                break;
            }
        apply_powerup(content);
        return;                                  /* one node per bonk */
    }
}

/* The Gate-vault seal switch (cast.md §5.12): overlapping it collapses the boss's
 * dais on the always-available route — no Charged state, no unmask. */
static void strike_seal(void)
{
    const VaultData *v = &G.vault_data;
    if (v->seal_col < 0 || G.guardian_down) return;
    float sx = (float)(v->seal_col * TILE_SIZE), sy = (float)(v->seal_row * TILE_SIZE);
    if (!overlap(G.player.x, G.player.y, PLAYER_W, PLAYER_H,
                 sx, sy, (float)TILE_SIZE, (float)TILE_SIZE)) return;
    for (int i = 0; i < MAX_ACTIVE_ENEMIES; i++)
        if (G.enemies[i].active && enemy_is_boss(&G.enemies[i]))
            guardian_defeat(&G.enemies[i], false);
    G.guardian_down = true;                        /* the dais drops regardless */
}

/* A machine is a hazard to Kilix only once it has actually activated: a sliding
 * Husk always; a walking machine only past its tell; a dormant Husk/telegraphing
 * or flattened machine never. */
static bool enemy_lethal(const Enemy *e)
{
    unsigned sub = e->state & ES_SUBSTATE;
    if (sub == ES_SQUASHED) return false;
    if (sub == ES_HUSK)     return (e->state & ES_SHELL_MOV) != 0;
    if (e->kind == EN_MAW)  return e->emerge >= MAW_LETHAL;   /* jaws bite once out */
    return e->alert > 0.0f && e->tell >= 1.0f;
}

/* Player-vs-machine pass (even ticks): stomps, Husk kicks, contact damage.  Stomp
 * arbitration uses the pre-move bottom so a descending contact reads as a stomp
 * even across the odd tick on which this pass is skipped. */
static void player_vs_enemy_pass(void)
{
    Player *p = &G.player;
    for (int i = 0; i < MAX_ACTIVE_ENEMIES; i++) {
        Enemy *e = &G.enemies[i];
        if (!e->active) continue;
        unsigned sub = e->state & ES_SUBSTATE;
        if (sub == ES_SQUASHED) continue;
        if (!overlap(p->x, p->y, PLAYER_W, PLAYER_H,
                     e->x, e->y, enemy_box_w(e), enemy_box_h(e))) continue;
        /* Aegis: Kilix is untouchable and destroys on contact (cast.md §4.2),
         * chaining like a stomp — but a boss is phase-plated and merely shrugs. */
        if (p->aegis_q > 0) {
            if (enemy_is_boss(e)) continue;
            e->state = (uint8_t)((e->state & ~ES_SUBSTATE) | ES_SQUASHED);
            e->state &= (uint8_t)~ES_SHELL_MOV;
            e->vx = 0.0f;
            e->squash = SQUASH_TIME;
            award_defeat(true);
            continue;
        }
        /* The Guardian is not stompable: any contact — descending or not — injures
         * Kilix (its core is reached only by Phase Pulse / Husk, never a stomp). */
        if (enemy_is_boss(e)) { hurt_player(); continue; }
        bool stomp = p->vy > 0.0f && p->prev_bottom <= e->y + enemy_box_h(e) + 4.0f;
        if (sub == ES_HUSK) {
            if (e->state & ES_SHELL_MOV) {            /* a sliding Husk */
                if (stomp) { stop_husk(e); p->vy = -STOMP_BOUNCE; p->jumping = false; }
                else       hurt_player();
            } else {                                  /* a dormant Husk: kick it */
                kick_husk(e);
                if (stomp) { p->vy = -STOMP_BOUNCE; p->jumping = false; }
            }
            continue;
        }
        if (e->kind == EN_MAW) {                       /* the emerger is NOT stompable */
            if (enemy_lethal(e)) hurt_player();        /* the jaws bite a descending Kilix */
            continue;
        }
        if (stomp) stomp_machine(e);                  /* a walking / thrower machine */
        else if (enemy_lethal(e)) hurt_player();
        /* else: still telegraphing / dormant -> nonlethal side contact */
    }
}

/* Rivets in flight: light ballistic actors.  Each arcs under its own gravity,
 * expires by lifetime / off-screen / falling into a pit, is BLOCKED (despawns)
 * on any solid, and costs Kilix a tier or a life on contact (then despawns).
 * Checked every tick so the small, fast box can never slip through Kilix. */
static void update_projectiles(void)
{
    float left    = G.cam_x - DESPAWN_LEFT;
    float right   = G.cam_x + (float)LOGICAL_W + DESPAWN_RIGHT;
    float floor_y = (float)(G.vault_data.rows * TILE_SIZE) + 16.0f;
    for (int i = 0; i < MAX_PROJECTILES; i++) {
        Projectile *p = &G.projectiles[i];
        if (!p->active) continue;
        float w = projectile_w(p), h = projectile_h(p);
        p->life -= TICK_DT;

        if (p->kind == PJ_PULSE) {
            /* Kilix's own phase-bolt: a shallow arc that bounces once or twice off
             * the floor (to clear low walkers), despawns on a wall or ceiling, and
             * dissolves the first machine — or cracks a boss core — it strikes. */
            p->vy = fminf(p->vy + PULSE_GRAVITY * TICK_DT, PULSE_FALL_MAX);
            p->x += p->vx * TICK_DT;
            if (projectile_hits_solid(p)) { p->active = false; continue; }   /* a wall */
            p->y += p->vy * TICK_DT;
            if (projectile_hits_solid(p)) {                                  /* floor/ceiling */
                p->y -= p->vy * TICK_DT;
                if (p->vy > 0.0f && p->bounces < PULSE_MAX_BOUNCE) {
                    p->bounces++;
                    p->vy = -p->vy * PULSE_BOUNCE;
                } else { p->active = false; continue; }
            }
            if (p->life <= 0.0f || p->x + w < left || p->x > right || p->y > floor_y) {
                p->active = false;
                continue;
            }
            for (int k = 0; k < MAX_ACTIVE_ENEMIES; k++) {
                Enemy *e = &G.enemies[k];
                if (!e->active) continue;
                if ((e->state & ES_SUBSTATE) == ES_SQUASHED) continue;
                if (overlap(p->x, p->y, w, h,
                            e->x, e->y, enemy_box_w(e), enemy_box_h(e))) {
                    pulse_hits_machine(e);
                    p->active = false;
                    break;
                }
            }
            continue;
        }

        /* Hostile bolts: the Riveter's rivet (ballistic arc) and the Guardian's
         * plasma (a slow, gravity-free horizontal shot). */
        float grav    = (p->kind == PJ_PLASMA) ? 0.0f : RIVET_GRAVITY;
        float fallmax = (p->kind == PJ_PLASMA) ? 0.0f : RIVET_FALL_MAX;
        p->vy = fminf(p->vy + grav * TICK_DT, fallmax);
        p->x += p->vx * TICK_DT;
        if (projectile_hits_solid(p)) { p->active = false; continue; }   /* blocked (wall) */
        p->y += p->vy * TICK_DT;
        if (projectile_hits_solid(p)) { p->active = false; continue; }   /* blocked (floor/ceiling) */
        if (p->life <= 0.0f || p->x + w < left || p->x > right ||
            p->y > floor_y) {                                            /* expired / gone */
            p->active = false;
            continue;
        }
        if (G.state == GS_PLAYING &&
            overlap(p->x, p->y, w, h,
                    G.player.x, G.player.y, PLAYER_W, PLAYER_H)) {
            p->active = false;
            hurt_player();
        }
    }
}

/* Machine-vs-machine pass (odd ticks): a sliding Husk defeats any machine it hits. */
static void enemy_vs_enemy_pass(void)
{
    for (int i = 0; i < MAX_ACTIVE_ENEMIES; i++) {
        Enemy *e = &G.enemies[i];
        if (!e->active || !(e->state & ES_SHELL_MOV)) continue;   /* only sliding Husks strike */
        for (int j = 0; j < MAX_ACTIVE_ENEMIES; j++) {
            if (j == i) continue;
            Enemy *o = &G.enemies[j];
            if (!o->active) continue;
            if (o->kind == EN_MAW) continue;          /* an anchored vent fixture, not mowable */
            if ((o->state & ES_SUBSTATE) == ES_SQUASHED) continue;
            if (o->state & ES_SHELL_MOV) continue;    /* two sliding Husks pass through */
            if (!overlap(e->x, e->y, enemy_box_w(e), enemy_box_h(e),
                         o->x, o->y, enemy_box_w(o), enemy_box_h(o))) continue;
            if (enemy_is_boss(o)) guardian_core_hit(o);   /* a routed Husk cracks the core */
            else                  defeat_machine(o, e);
        }
    }
}

/* Spawn-in-past-the-right-edge / despawn-beyond-either-edge asymmetry.  A machine
 * that falls into a pit (below the vault) is also culled. */
static void despawn_offscreen(void)
{
    float left  = G.cam_x - DESPAWN_LEFT;
    float right = G.cam_x + (float)LOGICAL_W + DESPAWN_RIGHT;
    float floor_y = (float)(G.vault_data.rows * TILE_SIZE) + 8.0f;
    for (int i = 0; i < MAX_ACTIVE_ENEMIES; i++) {
        Enemy *e = &G.enemies[i];
        if (!e->active) continue;
        if (enemy_is_boss(e)) continue;               /* a boss is an anchored set-piece */
        if (e->x + ENEMY_W < left || e->x > right || e->y > floor_y)
            e->active = false;
    }
}

/* -------------------------------------------------- charge timer + vault goal */

/* The charge timer (level-grammar §10): one unit drains every CHARGE_PER_UNIT
 * ticks.  A distinct cue fires ONCE as it crosses the fixed LOW_CHARGE_CUE
 * threshold; below zero costs a spare unit (the sintering front catches Kilix). */
static void charge_tick_update(void)
{
    if (G.state != GS_PLAYING) return;
    if (G.charge_start <= 0) return;                 /* an untimed arena: no drain */
    if (++G.charge_tick < CHARGE_PER_UNIT) return;
    G.charge_tick = 0;
    if (G.charge > 0) {
        G.charge--;
        if (G.charge == LOW_CHARGE_CUE && !G.low_charge_warned) {
            G.low_charge_warned = true;
            sound_play(SFX_LOW_CHARGE, 0.80f, 1.0f);
        }
    } else {
        lose_life();                                 /* charge expiry (level-grammar §10.4) */
    }
}

/* A cleared vault banks its goal bonus, refreshes the high score, and enters the
 * brief clear beat that hands off to the next vault (advance_campaign). */
static void clear_vault(int goal_bonus)
{
    G.score += goal_bonus;
    grant_score_thresholds();
    if (G.score > G.high_score) G.high_score = G.score;
    G.state = GS_VAULT_CLEAR;
    G.state_timer = 1.4f;
    G.hitstop = 0;                            /* no lingering freeze into the clear beat */
    render_shake(5.0f);
    sound_play(SFX_EXIT_OPEN, 0.80f, 1.0f);
}

/* Reaching the vault goal clears it.  Standard vault: grabbing the riser rail —
 * leftover charge converts on the steep top-heavy curve (game_exit_score).  Gate
 * vault: once the Guardian's dais has collapsed, reaching the exit iris. */
static void check_vault_goal(void)
{
    const VaultData *v = &G.vault_data;
    Player *p = &G.player;
    if (G.state != GS_PLAYING) return;
    if (v->riser_col >= 0 && v->seal_col < 0) {          /* standard: the riser rail */
        int baseline = v->rows - 1;
        float rx = (float)(v->riser_col * TILE_SIZE);
        float ry = (float)(v->riser_row * TILE_SIZE);
        float rh = (float)((baseline - v->riser_row) * TILE_SIZE);
        if (rh < (float)TILE_SIZE) rh = (float)TILE_SIZE;
        if (overlap(p->x, p->y, PLAYER_W, PLAYER_H, rx, ry, (float)TILE_SIZE, rh))
            clear_vault(game_exit_score(G.charge, G.charge_start));
    } else if (v->seal_col >= 0 && G.guardian_down && v->exit_col >= 0) {  /* Gate: the iris */
        float ex = (float)(v->exit_col * TILE_SIZE);
        float ey = (float)(v->exit_row * TILE_SIZE);
        if (overlap(p->x, p->y, PLAYER_W, PLAYER_H, ex, ey,
                    (float)TILE_SIZE, (float)TILE_SIZE))
            clear_vault(game_exit_score(G.charge, G.charge_start));
    }
}

/* Hand off to the next vault after the clear beat; unlock its district in the
 * profile (never in practice mode); the campaign's end is the victory state. */
static void advance_campaign(void)
{
    int next = G.level + 1;
    if (next >= CAMPAIGN_VAULTS) {
        G.state = GS_VICTORY;
        G.state_timer = 3.0f;
        return;
    }
    int nd = next / VAULTS_PER_DISTRICT + 1;
    if (nd > G.unlock_district) {
        G.unlock_district = nd;
        if (!G.practice_mode) game_profile_save();
    }
    game_load_level(next);
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
    p->prev_bottom = p->y + PLAYER_H;
    p->grounded = true;
    p->coyote = COYOTE_FRAMES;
    p->invuln = 1.0f;          /* brief spawn/respawn i-frames (no instant re-death) */
}

void game_load_level(int level)
{
    G.level = level;
    level_build(level, &G.vault_data);
    spawn_player();
    /* Reset the machine field + in-flight rivets: a fresh vault re-runs its
     * spawn schedule from a clean slate. */
    memset(G.enemies, 0, sizeof G.enemies);
    memset(G.projectiles, 0, sizeof G.projectiles);
    G.spawn_cursor = 0;
    G.chain = 0;
    G.chain_decay = 0;
    G.hitstop = 0;                     /* no freeze carries into a fresh vault (M7) */
    G.level_start_score = G.score;     /* the restart roll-back baseline (M7) */
    G.state_timer = 0.0f;
    G.guardian_down = false;
    G.guardian_unmasked = false;
    G.cam_x = G.cam_x_max = G.cam_y = 0.0f;
    G.scroll_lock = false;
    /* Charge timer fresh from the vault's budget (level-grammar §10.1). */
    G.charge = G.charge_start = (int)G.vault_data.timer_start;
    G.charge_tick = 0;
    G.low_charge_warned = false;
    G.state = GS_PLAYING;
}

void game_start_at(int level, bool practice)
{
    G.practice_mode = practice;
    game_load_level(level);
}

void game_start(int level)
{
    game_start_at(level, false);
}

void game_force_level_clear(void)
{
    G.state = GS_VAULT_CLEAR;
    sound_play(SFX_EXIT_OPEN, 0.80f, 1.0f);
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
    G.lives = START_LIVES;
    G.next_extra_life = EXTRA_LIFE_STEP;
    G.unlock_district = 1;
    G.player.facing = 1;
    G.player.grounded = true;
    G.player.buffer_tick = -1;
    /* Restore high score / highest-district unlock / sound preference from the
     * profile.  A no-op under SUPER_KILIX_NO_PROFILE (every headless mode), so
     * determinism is untouched; a real run picks up the saved progress. */
    game_profile_load();
}

void game_shutdown(void)
{
    /* Persist the run's progress (high score + unlock + sound flag); a no-op in
     * practice mode or under SUPER_KILIX_NO_PROFILE. */
    game_profile_save();
}

/* ---------------------------------------------------------------- profile */

/* The fixed-size, versioned, checksummed profile payload (M6).  kilix-state writes
 * it 0600 through a temp file + fsync + rename; a corrupt/truncated/newer file is
 * ignored without partial application.  reserved keeps the blob a stable 32 bytes
 * for forward growth. */
typedef struct {
    uint32_t magic;
    uint32_t schema;
    int32_t  high_score;
    int32_t  unlock_district;
    uint8_t  sound_on;
    uint8_t  reserved[15];
} SkProfile;

static bool profile_disabled(void)
{
    const char *no = getenv("SUPER_KILIX_NO_PROFILE");
    return no && no[0] == '1' && no[1] == '\0';
}

/* Configure the kilix-state store under ${SUPER_KILIX_DATA_HOME}/super-kilix/ (or
 * ${XDG_DATA_HOME:-$HOME/.local/share}/super-kilix/), returning false if the
 * profile is disabled or the store cannot be opened. */
static bool profile_open(kilixstate_store *store)
{
    if (profile_disabled()) return false;
    kilixstate_options opts;
    kilixstate_options_init(&opts);
    opts.app_id = "super-kilix";
    opts.filename = "profile.v1";
    opts.format = KILIXSTATE_FORMAT_CRC32;
    opts.max_payload = sizeof(SkProfile);
    const char *home = getenv("SUPER_KILIX_DATA_HOME");
    if (home && home[0]) opts.base_directory = home;   /* override the XDG parent */
    return kilixstate_store_init(store, &opts) == KILIXSTATE_OK;
}

bool game_profile_write(uint32_t magic, uint32_t schema,
                        int high_score, int unlock, int sound_on)
{
    kilixstate_store store;
    if (!profile_open(&store)) return false;
    SkProfile p;
    memset(&p, 0, sizeof p);
    p.magic = magic;
    p.schema = schema;
    p.high_score = high_score;
    p.unlock_district = unlock;
    p.sound_on = (uint8_t)(sound_on ? 1 : 0);
    kilixstate_result r = kilixstate_save(&store, &p, sizeof p);
    kilixstate_store_close(&store);
    return r == KILIXSTATE_OK;
}

bool game_profile_save(void)
{
    if (G.practice_mode) return false;     /* practice never mutates the profile */
    return game_profile_write(SK_PROFILE_MAGIC, SK_PROFILE_SCHEMA,
                              G.high_score, G.unlock_district, G.sound_on ? 1 : 0);
}

bool game_profile_load(void)
{
    kilixstate_store store;
    if (!profile_open(&store)) return false;
    SkProfile p;
    memset(&p, 0, sizeof p);
    size_t got = 0;
    kilixstate_result r = kilixstate_load(&store, &p, sizeof p, &got);
    kilixstate_store_close(&store);
    if (r != KILIXSTATE_OK) return false;              /* absent / corrupt / truncated */
    if (got != sizeof p) return false;                 /* wrong size: no partial apply */
    if (p.magic != SK_PROFILE_MAGIC) return false;     /* not our payload */
    if (p.schema > SK_PROFILE_SCHEMA) return false;    /* a newer schema: ignore */
    if (p.high_score < 0 || p.unlock_district < 1 || p.unlock_district > DISTRICTS)
        return false;                                  /* out of range: ignore */
    G.high_score = p.high_score;
    G.unlock_district = p.unlock_district;
    G.sound_on = p.sound_on != 0;
    return true;
}

bool game_profile_path(char *buf, size_t len)
{
    kilixstate_store store;
    if (!profile_open(&store)) return false;
    kilixstate_result r = kilixstate_store_path(&store, buf, len);
    kilixstate_store_close(&store);
    return r == KILIXSTATE_OK;
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

/* Charged Kilix emits a Phase Pulse (cast.md §3.5); a no-op below the Charged
 * tier or outside play, so callers never have to gate it. */
void game_fire_pulse(void)
{
    if (G.state != GS_PLAYING) return;
    if (G.player.power_tier < 2) return;   /* only the Charged state can emit */
    spawn_pulse();
}

/* Spend a spare unit on demand (R): roll the vault's unbanked score back to the entry
 * baseline and preserve the elapsed vault time (the front kept closing), then drop into
 * the brief life-lost beat which redeploys into the same vault (game_tick).  Reachable
 * from play or pause (cast.md §4; level-grammar §10.4). */
void game_restart_life(void)
{
    if (G.state != GS_PLAYING && G.state != GS_PAUSED) return;
    G.score = G.level_start_score;       /* roll back the unbanked vault score */
    G.chain = 0;
    G.chain_decay = 0;
    G.hitstop = 0;
    if (G.lives > 0) { G.lives--; G.deaths++; }
    G.state = GS_LIFE_LOST;
    G.state_timer = 0.5f;
    sound_play(SFX_HURT, 0.85f, 0.9f);
}

/* Continue the campaign from the highest unlocked district (never practice). */
static void continue_campaign(void)
{
    game_start_at((G.unlock_district - 1) * VAULTS_PER_DISTRICT, false);
}

/* Leave to the title screen: bank progress and reset the transient menu cursors. */
static void return_to_title(void)
{
    if (G.score > G.high_score) G.high_score = G.score;
    if (!G.practice_mode) game_profile_save();
    sound_jet(false, 0.0f);
    G.state = GS_TITLE;
    G.help_return_state = GS_TITLE;
    G.menu_choice = 0;
    G.state_timer = 0.0f;
}

/* Open the three-page field manual, remembering where to return (both from gameplay
 * and from the pause menu). */
static void open_help(int return_state)
{
    G.help_return_state = return_state;
    G.help_page = 0;
    G.state = GS_HELP;
    sound_jet(false, 0.0f);
}

/* Close the manual, restoring the exact state it was opened from. */
static void close_help(void)
{
    int dest = G.help_return_state;
    if (dest != GS_PLAYING && dest != GS_PAUSED && dest != GS_TITLE) dest = GS_TITLE;
    G.state = dest;
}

/* Toggle the sound flag (M); persists the preference on a real run. */
static void toggle_sound(void)
{
    G.sound_on = !G.sound_on;
    sound_set_enabled(G.sound_on);
    if (!G.headless && !G.practice_mode) game_profile_save();
}

/* Clamp the selector cursor to the unlocked range (row-major over the campaign map). */
static void clamp_selection(void)
{
    int last = G.unlock_district * VAULTS_PER_DISTRICT - 1;
    if (last < 0) last = 0;
    if (last > CAMPAIGN_VAULTS - 1) last = CAMPAIGN_VAULTS - 1;
    if (G.sel_index < 0) G.sel_index = 0;
    if (G.sel_index > last) G.sel_index = last;
}

void game_handle_key(int key)
{
    if (key >= 'A' && key <= 'Z') key += 'a' - 'A';

    /* Sound toggle is global (works in every state). */
    if (key == 'm') { toggle_sound(); return; }

    switch (G.state) {
    case GS_TITLE:
        if (key == KEY_UP   || key == 'w') G.menu_choice = (G.menu_choice + 3) % 4;
        else if (key == KEY_DOWN || key == 's') G.menu_choice = (G.menu_choice + 1) % 4;
        else if (key == 'h') open_help(GS_TITLE);
        else if (key == 'q' || key == KEY_ESC) G.quit = true;
        else if (key == KEY_ENTER || key == ' ') {
            switch (G.menu_choice) {
            case 0: continue_campaign(); break;                 /* run / continue inward */
            case 1: G.sel_index = (G.unlock_district - 1) * VAULTS_PER_DISTRICT;
                    clamp_selection();
                    G.state = GS_SELECT; break;                 /* choose a vault */
            case 2: open_help(GS_TITLE); break;                 /* field manual */
            default: G.quit = true; break;                      /* leave the tether */
            }
        }
        return;

    case GS_SELECT:
        if (key == KEY_LEFT  || key == 'a') G.sel_index -= 1;
        else if (key == KEY_RIGHT || key == 'd') G.sel_index += 1;
        else if (key == KEY_UP    || key == 'w') G.sel_index -= VAULTS_PER_DISTRICT;
        else if (key == KEY_DOWN  || key == 's') G.sel_index += VAULTS_PER_DISTRICT;
        clamp_selection();
        if (key == KEY_ENTER || key == ' ') game_start_at(G.sel_index, false);
        else if (key == 'q' || key == KEY_ESC) return_to_title();
        return;

    case GS_HELP:
        if (key == KEY_RIGHT || key == KEY_DOWN || key == 'd' || key == 's' ||
            key == KEY_ENTER || key == ' ') G.help_page = (G.help_page + 1) % 3;
        else if (key == KEY_LEFT || key == KEY_UP || key == 'a' || key == 'w')
            G.help_page = (G.help_page + 2) % 3;
        else if (key == KEY_ESC || key == 'q' || key == 'h') close_help();
        return;

    case GS_PLAYING:
        if (key == 'p' || key == KEY_ESC) { G.state = GS_PAUSED; sound_jet(false, 0.0f); return; }
        if (key == 'h') { open_help(GS_PLAYING); return; }
        if (key == 'r') { game_restart_life(); return; }
        if (key == 'x' || key == 'j' || key == 'l') game_fire_pulse();  /* phase-tool fire */
        if (!G.held_controls) set_press_latch(key);
        return;

    case GS_PAUSED:
        if (key == 'p' || key == KEY_ESC || key == KEY_ENTER) G.state = GS_PLAYING;
        else if (key == 'h') open_help(GS_PAUSED);
        else if (key == 'r') game_restart_life();
        else if (key == 'q') return_to_title();
        return;

    case GS_VAULT_CLEAR:
    case GS_LIFE_LOST:
        if (key == KEY_ESC || key == 'q') return_to_title();
        return;

    case GS_GAMEOVER:
    case GS_VICTORY:
        if (key == KEY_ENTER || key == ' ' || key == KEY_ESC || key == 'q')
            return_to_title();
        return;

    default:
        return;
    }
}

/* ---------------------------------------------------------------- autopilot */

/* A deterministic bot that runs toward the vault exit and buffers a jump when a
 * wall or a void gap is ahead (or forward progress has stalled), holding the
 * jump through the ascent so the arc peaks high enough to clear a tall conduit
 * or the auto-mountable wall top.  It drives the real physics through the same
 * fields the funnel writes, so it exercises the collision/camera path without a
 * terminal.  No RNG of its own. */
void game_autopilot(void)
{
    if (G.state != GS_PLAYING) return;
    Player *p = &G.player;

    /* Steer toward the exit column (vaults run left->right). */
    int dir = 1;
    if (G.vault_data.exit_col >= 0) {
        float exit_x = (float)(G.vault_data.exit_col * TILE_SIZE);
        dir = (p->x + PLAYER_W * 0.5f) <= exit_x ? 1 : -1;
    }

    int col      = (int)floorf((p->x + PLAYER_W * 0.5f) / TILE_SIZE);
    int body_row = (int)floorf((p->y + PLAYER_H * 0.5f) / TILE_SIZE);
    int foot_row = (int)floorf((p->y + PLAYER_H - 1.0f) / TILE_SIZE);
    int ahead1 = col + dir;
    int ahead2 = col + 2 * dir;

    bool wall_ahead = game_tile_solid(ahead1, body_row) ||
                      game_tile_solid(ahead1, foot_row) ||
                      game_tile_solid(ahead2, body_row);
    /* On floor but the floor one or two columns ahead is missing: jump to clear
     * the void rather than drop into it. */
    bool on_floor = p->grounded && game_tile_solid(col, foot_row + 1);
    bool gap_ahead = on_floor && (!game_tile_solid(ahead1, foot_row + 1) ||
                                  !game_tile_solid(ahead2, foot_row + 1));
    bool stalled = p->grounded && fabsf(p->vx) < 6.0f;

    /* Leap at an approaching machine so the descent stomps or clears it — keeps
     * the bot alive through the roster and exercises the stomp path headlessly. */
    bool machine_ahead = false;
    for (int i = 0; i < MAX_ACTIVE_ENEMIES; i++) {
        const Enemy *e = &G.enemies[i];
        if (!e->active || (e->state & ES_SUBSTATE) == ES_SQUASHED) continue;
        float rel = (e->x - p->x) * (float)dir;
        if (rel > 0.0f && rel < 44.0f && fabsf(e->y - p->y) < 22.0f)
            machine_ahead = true;
    }
    bool want_jump = wall_ahead || gap_ahead || stalled || machine_ahead;

    if (want_jump && (p->grounded || p->coyote > 0.0f))
        p->buffer_tick = (int)G.tick;

    /* Keep jump held through the rise (floaty gravity -> high arc); release at
     * apex so variable-height is exercised and falls stay snappy. */
    bool hold_jump = want_jump || (p->jumping && p->vy < 0.0f);

    G.scripted_input = true;
    G.held_controls = true;
    G.held_left  = dir < 0;
    G.held_right = dir > 0;
    G.held_up = false;
    G.held_down = false;
    G.held_run = true;
    G.held_jump = hold_jump;
}

/* ---------------------------------------------------------------- tick */

void game_tick(void)
{
    if (G.quit) return;
    G.tick++;
    G.scene_time += TICK_DT;
    (void)game_randf();          /* keep the RNG stream advancing deterministically */
    decay_latches();

    /* Life-lost is a brief beat, then the vault restarts (a spare unit still in
     * stock) or the run ends in game-over. */
    if (G.state == GS_LIFE_LOST) {
        G.state_timer -= TICK_DT;
        if (G.state_timer <= 0.0f) {
            if (G.lives <= 0) { G.state = GS_GAMEOVER; G.state_timer = 2.0f; }
            else {
                /* Redeploy into the same vault, but the sintering front kept closing:
                 * preserve the elapsed charge (elapsed vault time) across the restart
                 * rather than refilling the timer (level-grammar §10). */
                int elapsed = G.charge_start - G.charge;
                game_load_level(G.level);
                if (elapsed > 0) {
                    G.charge -= elapsed;
                    if (G.charge < 0) G.charge = 0;
                }
            }
        }
        return;
    }
    /* Vault cleared: a brief celebratory beat, then hand off to the next vault
     * (or the victory state at the campaign's end). */
    if (G.state == GS_VAULT_CLEAR) {
        G.state_timer -= TICK_DT;
        if (G.state_timer <= 0.0f) advance_campaign();
        return;
    }
    /* Game-over continue loop (level-grammar §10.4): a fresh stock of spare units,
     * the score reset, restarting from the first vault of the current district. */
    if (G.state == GS_GAMEOVER) {
        G.state_timer -= TICK_DT;
        if (G.state_timer <= 0.0f) {
            G.lives = START_LIVES;
            G.score = 0;
            G.next_extra_life = EXTRA_LIFE_STEP;
            int first = (G.level / VAULTS_PER_DISTRICT) * VAULTS_PER_DISTRICT;
            game_load_level(first);
        }
        return;
    }
    if (G.state != GS_PLAYING) return;   /* GS_TITLE / GS_PAUSED / GS_HELP / GS_SELECT /
                                            GS_VICTORY: static here */

    /* Hit-stop: a bounded, deterministic freeze of the whole simulation for a few ticks
     * on impact (set in the interaction handlers).  The housekeeping above still ran, so
     * the RNG stream and scene_time advance identically across runs; only the sim body is
     * skipped.  game_validate bounds G.hitstop, so a freeze can never run away. */
    if (G.hitstop > 0) { G.hitstop--; return; }

    update_player();
    update_camera();
    spawn_scheduled_enemies();
    update_enemies();
    update_projectiles();
    /* Interleave the collision passes on alternating ticks (enemy-vs-enemy on
     * odd, player-vs-enemy on even), as the studied genre spreads its two
     * collision systems across frames. */
    if (G.tick & 1u) enemy_vs_enemy_pass();
    else             player_vs_enemy_pass();
    despawn_offscreen();
    strike_seal();               /* the always-available Gate kill path (overlap) */
    check_vault_goal();          /* riser grab / Gate iris -> vault clear */
    charge_tick_update();        /* drain the charge timer; expiry costs a life */
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
        p->gait_phase, p->gait_amount, p->land_squash, p->prev_bottom, p->invuln,
        G.cam_x, G.cam_x_max, G.cam_y
    };
    for (size_t i = 0; i < sizeof floats / sizeof floats[0]; i++)
        if (!isfinite(floats[i])) {
            if (err && len) snprintf(err, len, "non-finite float field %zu", i);
            return false;
        }
    if (G.lives < 0 || p->power_tier < 0 || p->power_tier > 2 || p->aegis_q < 0 ||
        G.spawn_cursor < 0 || G.spawn_cursor > G.vault_data.enemy_count) {
        if (err && len) snprintf(err, len, "player/spawn counter out of range");
        return false;
    }
    /* Hit-stop is bounded: a freeze is never unbounded (M7 invariant). */
    if (G.hitstop < 0 || G.hitstop > HITSTOP_MAX) {
        if (err && len) snprintf(err, len, "hit-stop freeze out of bounds (%d)", G.hitstop);
        return false;
    }
    if (G.score < 0 || G.motes < 0 || G.chain < 0 || G.chain_decay < 0 ||
        G.charge < 0 || G.next_extra_life < 0 || G.high_score < 0 ||
        G.unlock_district < 1 || G.unlock_district > DISTRICTS) {
        if (err && len) snprintf(err, len, "score/economy counter out of range");
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

    /* Machine invariants: finite, in-bounds, a valid sub-state, and — the M4
     * gate — never embedded in solid geometry after any tick. */
    int active = 0;
    for (int i = 0; i < MAX_ACTIVE_ENEMIES; i++) {
        const Enemy *e = &G.enemies[i];
        if (!e->active) continue;
        active++;
        const float ef[] = { e->x, e->y, e->vx, e->vy, e->alert, e->tell,
                             e->squash, e->emerge };
        for (size_t k = 0; k < sizeof ef / sizeof ef[0]; k++)
            if (!isfinite(ef[k])) {
                if (err && len) snprintf(err, len, "machine %d has a non-finite field", i);
                return false;
            }
        if ((e->state & ES_SUBSTATE) > ES_SQUASHED) {
            if (err && len) snprintf(err, len, "machine %d bad sub-state", i);
            return false;
        }
        if (e->x < -64.0f || e->x > world_w + 64.0f) {
            if (err && len) snprintf(err, len, "machine %d out of bounds", i);
            return false;
        }
        if (enemy_hits_solid(e, e->x, e->y)) {
            if (err && len) snprintf(err, len, "machine %d embedded in solid geometry", i);
            return false;
        }
    }
    if (active > MAX_ACTIVE_ENEMIES) {
        if (err && len) snprintf(err, len, "machine slot pool over cap (%d)", active);
        return false;
    }

    /* Rivets: finite and inside a generous flight envelope (they despawn on any
     * solid the same tick, so a live one is always mid-flight, never embedded). */
    for (int i = 0; i < MAX_PROJECTILES; i++) {
        const Projectile *p = &G.projectiles[i];
        if (!p->active) continue;
        const float pf[] = { p->x, p->y, p->vx, p->vy, p->life };
        for (size_t k = 0; k < sizeof pf / sizeof pf[0]; k++)
            if (!isfinite(pf[k])) {
                if (err && len) snprintf(err, len, "rivet %d has a non-finite field", i);
                return false;
            }
        if (p->x < -128.0f || p->x > world_w + 128.0f ||
            p->y < -(float)LOGICAL_H || p->y > world_h + 128.0f) {
            if (err && len) snprintf(err, len, "rivet %d out of bounds", i);
            return false;
        }
    }
    return true;
}

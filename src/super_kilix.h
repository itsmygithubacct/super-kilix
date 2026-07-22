/*
 * Super Kilix — an original side-scrolling platformer starring Kilix, for
 * Kitty-graphics terminals.  This header is the single private interface shared
 * by the six game modules; nothing in it depends on any prior game's data.
 */
#ifndef SUPER_KILIX_H
#define SUPER_KILIX_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "kitty_keyboard.h"

#define SK_VERSION "0.0.1"

/* Logical canvas: 256x240 == 16x15 tiles — the narrow scroller window.
 * The HUD overlays the top 2 tile-rows; the authored playfield is 13 rows;
 * a vault grid may be up to 16 tiles tall. */
#define LOGICAL_W  256
#define LOGICAL_H  240
#define TILE       16
#define TILE_SIZE  16          /* physics/collision spelling (== TILE) */
#define HUD_ROWS   2
#define PLAY_ROWS  13
#define VAULT_ROWS 16          /* array max height; > VIEW_ROWS so tall vaults exist */
#define VAULT_COLS 256         /* up to 4096 px == 16 screens; == SKLF MAX_LEVEL_COLS */

#define DISTRICTS           8
#define VAULTS_PER_DISTRICT 4
#define CAMPAIGN_VAULTS     (DISTRICTS * VAULTS_PER_DISTRICT)   /* 32 */
#define MAX_ENEMIES         24     /* spawn-stream slots stored per vault (M3: data only) */

#define TICK_DT (1.0f / 60.0f)   /* fixed 60 Hz sim tick */

/*
 * Player physics constants — Kilix's OWN numbers (player-mechanics.md §7), never
 * a prior game's register values.  Stored unit is px/s (accel px/s^2); per-tick
 * integration is v += a*TICK_DT; x += v*TICK_DT.  They are named independently of
 * the drawn silhouette so the simulation never reads graphics state.
 */
#define PLAYER_W          12.0f   /* collision box width  (art is drawn larger) */
#define PLAYER_H          15.0f   /* collision box height (feet at y + PLAYER_H) */

#define WALK_MAX          96.0f    /* walk top speed */
#define RUN_MAX           162.0f   /* run top speed (~69% faster) */
#define GROUND_ACCEL      640.0f   /* grounded horizontal acceleration */
#define AIR_ACCEL         420.0f   /* airborne horizontal acceleration */
#define SKID_DECEL        1100.0f  /* turnaround decel (input opposes motion) */
#define GROUND_FRICTION   500.0f   /* grounded release coast */
#define AIR_FRICTION      0.0f     /* airborne release (none) */
#define STOP_THRESHOLD    0.5f     /* |vx| below this snaps to zero (px/s) */

#define JUMP_V0_STAND     380.0f   /* launch impulse, |vx| < 60 */
#define JUMP_V0_WALK      400.0f   /* launch impulse, 60 <= |vx| < 130 */
#define JUMP_V0_RUN       420.0f   /* launch impulse, |vx| >= 130 */
#define JUMP_BAND_WALK    60.0f    /* |vx| threshold for the walk launch band */
#define JUMP_BAND_RUN     130.0f   /* |vx| threshold for the run launch band */
#define G_RISE            1000.0f  /* gravity while rising, jump held */
#define G_APEX            500.0f   /* gravity near apex, jump held (|vy| < APEX_VY) */
#define G_FALL            1800.0f  /* gravity falling, or on early release */
#define APEX_VY           60.0f    /* apex-softening speed window */
#define FALL_MAX          360.0f   /* terminal fall speed */
#define FASTFALL_MAX      480.0f   /* terminal fall speed, Down held */

#define COYOTE_FRAMES     6.0f     /* ledge-exit jump grace (ticks) */
#define BUFFER_FRAMES     5        /* pre-land jump buffer (ticks) */
#define RUN_STICKY        8.0f     /* run-cap persistence after release (ticks) */
#define CORNER_NUDGE      4.0f     /* head-clip corner correction (px) */
#define PRESS_LATCH_S     0.30f    /* press-only fallback hold duration (s) */

#define MAX_STEP          (TILE_SIZE - 1)   /* sub-step cap: no tunnel through 1 tile */
#define ONEWAY_EPS        1.0f              /* one-way pre-move tolerance (px) */

/* Camera thresholds (level-grammar.md §9), pixels at LOGICAL_W = 256:
 * no scroll until Kilix's on-screen X reaches the deadzone (80 px = 31%), then
 * the facing-direction lookahead ramps to full scroll rate by 112 px (44%). */
#define CAM_DEADZONE      80.0f
#define CAM_LOOKAHEAD     112.0f
#define CAM_SMOOTH        0.18f    /* critically-damped single-pole follow rate */

/* Game states.  M0 declares the full lifecycle enum; later milestones give the
 * non-title states behaviour.  GS_STATE_COUNT bounds game_validate's range check. */
enum {
    GS_TITLE,
    GS_PLAYING,
    GS_PAUSED,
    GS_VAULT_CLEAR,
    GS_LIFE_LOST,
    GS_GAMEOVER,
    GS_VICTORY,
    GS_STATE_COUNT
};

/* Logical keys funnelled to game_handle_key (values above ASCII so letters and
 * these enumerators never collide). */
enum {
    KEY_ENTER = 1000, KEY_BACKSPACE, KEY_TAB, KEY_ESC,
    KEY_UP, KEY_DOWN, KEY_RIGHT, KEY_LEFT
};

/* Semantic tile cells, drawn from primitives in render.c, one behaviour each in
 * game.c — NOT atlas indices.  The solid structural cells form a contiguous
 * range [T_HULL, T_CONDUIT] so game_tile_solid is a single range test; T_LEDGE
 * is the one-way grate; the goal/hazard cells past it are never solid.  M3 ships
 * the RUST-FLATS vocabulary; later districts extend this enum in place. */
enum {
    T_EMPTY,
    /* --- solid structure (contiguous range) --- */
    T_HULL,            /* terrace ground / default floor panel */
    T_HULL_DARK,       /* buried subsurface fill panel */
    T_BEDROCK,         /* indestructible block: slabs, pillars, staircases */
    T_BRICK,           /* breakable scrap block */
    T_CACHE,           /* struck-from-below cache node (mote / power / shell) */
    T_SPENT,           /* an emptied cache/scrap: cosmetic solid */
    T_CONDUIT,         /* capped vent pipe: solid wall / jump-height gate */
    /* --- one-way --- */
    T_LEDGE,           /* one-way grate: solid from above only */
    /* --- non-solid devices / hazards (handled by overlap, never the resolver) */
    T_RISER,           /* scored end-of-vault ascent rail (grabbed by overlap) */
    T_IRIS,            /* passive exit door (autopilot goal) */
    T_THORN,           /* thorn strip hazard (lethal at M4) */
    TILE_KIND_COUNT
};

/*
 * A camera-relative spawn: a machine that materialises when its world column
 * enters the band past the right screen edge (level-grammar.md §9).  M3 stores
 * the spawn schedule as data only; the enemy simulation lands at M4.
 */
typedef struct {
    uint8_t kind;      /* machine role id (data.c roster) */
    uint8_t col, row;  /* world column / row the spawn is scheduled at */
    uint8_t param;     /* variant / group token */
} EnemySpawn;

/*
 * A vault is a wide, horizontally-scrolling tile grid built by level_build from
 * the SKLF object + spawn streams (data.c).  The fixed-size arrays keep
 * GameState trivially memcmp-comparable (no pointers into heap state).  Standard
 * vaults are PLAY_ROWS (13) tall with the ground baseline at row rows-1; tall
 * rooms (RAIL SPIRES / THE WARDEN VAULT) extend toward VAULT_ROWS at later
 * milestones.  M3 extends this struct in place; movers/foam arrive later.
 */
typedef struct {
    uint8_t tiles[VAULT_ROWS][VAULT_COLS];   /* [row][col] semantic cells */
    int     cols, rows;                      /* actual extent in tiles */
    int     length;                          /* authored length in columns (== cols) */
    int     spawn_col, spawn_row;            /* Kilix start (body row on solid ground) */
    int     riser_col, riser_row;            /* scored ascent rail (rail-top row) */
    int     seal_col,  seal_row;             /* Gate-vault seal switch, else -1 */
    int     exit_col,  exit_row;             /* the iris exit door (autopilot target) */
    int     district, vault;                 /* 1-based labels for the title card */
    int     biome;                           /* render family: 0 hull .. 3 forge */
    int     entry_mode, floor_pattern;       /* SKLF header fields */
    uint16_t timer_start;                    /* charge budget in units */
    EnemySpawn enemies[MAX_ENEMIES];
    int     enemy_count;
    char    title[40];                       /* e.g. "1-1 RUST FLATS" */
} VaultData;

/*
 * Kilix's simulation actor.  Position is float pixels (top-left of the collision
 * box); every field is a plain scalar so GameState stays memcmp-comparable.
 */
typedef struct {
    float x, y, vx, vy;      /* collision-box top-left position and velocity */
    int   facing;            /* +1 faces right, -1 faces left */
    bool  grounded;
    bool  jumping;           /* in the ascending phase of a jump */
    bool  thrusting;
    bool  phasing;
    bool  fastfall;          /* Down held while airborne (raised terminal clamp) */
    bool  jump_held;         /* jump control held this tick */
    float coyote;            /* frames of ledge grace remaining */
    int   buffer_tick;       /* tick a jump press was latched (-1 = none) */
    float run_sticky;        /* frames the run cap persists after release */
    int   jump_band;         /* launch band latched at takeoff (0/1/2) */
    float gait_phase;        /* walk oscillator phase, radians */
    float gait_amount;       /* 0 idle .. 1 full stride */
} Player;

/*
 * GameState is one flat, pointer-free, trivially memcpy/memcmp-comparable POD.
 * The renderer snapshots and compares it to prove it never mutates simulation
 * state, so it must never gain a pointer into heap-owned data.  Later milestones
 * extend it in place.
 */
typedef struct {
    int      state;          /* one of the GS_* enumerators */
    int      W, H;           /* terminal pixel size (0 in headless modes) */
    bool     quit, headless, sound_on;
    uint32_t rng;            /* seeded xorshift32 — the only simulation RNG */
    uint64_t tick;           /* fixed-step counter */
    float    scene_time;     /* seconds of wall-free cosmetic animation time */

    int      level;          /* 0..CAMPAIGN_VAULTS-1 (M2 loads the arena at 0) */
    VaultData vault_data;
    Player   player;

    /* Camera: monotonic right-only ratchet, vertical locked for standard vaults. */
    float    cam_x, cam_x_max, cam_y;
    bool     scroll_lock;

    /* Input funnel (the single choke point game_set_held_controls feeds). */
    bool  held_controls;     /* true when release events (variable jump) available */
    bool  held_left, held_right, held_up, held_down, held_jump, held_run;
    float left_latch, right_latch, up_latch, down_latch, jump_latch, run_latch;
    bool  scripted_input;    /* autopilot / input-test drives the funnel */
} GameState;

extern GameState G;

/* data.c */
void        level_build(int level_index, VaultData *out);   /* PURE: index -> vault */
bool        level_validate(int level_index, char *err, size_t len);
bool        level_validate_campaign(char *err, size_t len);
uint32_t    level_signature(const VaultData *v);            /* FNV-1a topology hash */
int         level_enemy_budget(int level_index);
const char *tile_name(int tile);
const char *machine_name(int kind);
const char *district_name(int district);

/* game.c */
float clampf(float v, float lo, float hi);
float game_randf(void);                       /* xorshift32 on G.rng */
void  game_init(int w, int h, uint32_t seed);
void  game_shutdown(void);
void  game_start(int level);
void  game_load_level(int level);
void  game_tick(void);
void  game_handle_key(int key);
void  game_set_held_controls(bool available, bool left, bool right,
                             bool up, bool down, bool jump, bool boost);
void  game_autopilot(void);
void  game_force_level_clear(void);
void  game_force_life_lost(void);
bool  game_tile_solid(int col, int row);
bool  game_validate(char *err, size_t len);

/* render.c */
bool     render_init(int w, int h);
bool     render_resize(int w, int h);
void     render_shutdown(void);
void     render_frame(void);
uint8_t *render_fb(void);
bool     render_dump_ppm(const char *path);

/* term.c */
bool term_init(int *ow, int *oh);
bool term_check_resize(int *ow, int *oh);
void term_present(const uint8_t *rgba, int w, int h);
int  term_read_input(void);
bool term_next_key_event(kittykb_event *event);
bool term_key_down(uint32_t key);
bool term_has_release_events(void);
void term_shutdown(void);
void term_emergency_restore(void);

/* sound.c */
bool sound_init(void);
void sound_shutdown(void);
void sound_set_enabled(bool on);
bool sound_is_enabled(void);
void sound_play(int id, float volume, float pitch);
void sound_jet(bool active, float intensity);

#endif

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

/*
 * Machine substrate (M4a) — the shared enemy core the whole roster builds on.
 * Boxes are named constants independent of the drawn silhouette (invariant #5),
 * speeds are Kilix's own units (px/s), and the interval quantum is Kilix's own
 * coarse tick, NOT the studied 21 (cast.md §7).
 */
#define ENEMY_W            12.0f  /* machine AABB width  (16 px authoring cell) */
#define ENEMY_H            14.0f  /* machine AABB height (feet at y + ENEMY_H) */
#define HUSK_H             12.0f  /* retracted-shell AABB height (rounded, lower) */
#define SQUASH_H            6.0f  /* flattened-walker AABB height */
#define MAX_ACTIVE_ENEMIES    8   /* the capped field slot pool — a density cap */

#define SK_QUANTUM        15      /* coarse interval quantum in ticks (0.25 s @60 Hz):
                                     Kilix's own revival/duration/cadence tick, not 21 */
#define SPAWN_BAND        48.0f   /* materialise a machine ~3 tiles past the right edge */
#define DESPAWN_LEFT      48.0f   /* cull a machine this far behind the left edge */
#define DESPAWN_RIGHT     96.0f   /* cull one this far past the right edge (> SPAWN_BAND
                                     so a fresh spawn is never instantly culled) */

#define TREADER_SPEED     34.0f   /* Slag-Treader walk (cast.md §5.2: ~0.35x the walk cap) */
#define CARAPOD_SPEED     30.0f   /* Carapod patrol (cast.md §5.3) */
#define HUSK_SPEED        200.0f  /* kicked-Husk slide (cast.md §5.3) */
#define ENEMY_GRAVITY     1200.0f /* machine fall accel (px/s^2) */
#define ENEMY_FALL_MAX    300.0f  /* machine terminal fall (px/s) */

#define STOMP_BOUNCE      190.0f  /* FLAT stomp rebound (cast.md §3.3) — no hold modifier */
#define ALERT_RANGE       88.0f   /* local activation radius: dormant beyond it (px) */
#define TELL_RATE         1.6f    /* activation-tell ramp per second while alerted */
#define HIT_INVULN        1.4f    /* i-frames after a demotion or respawn (s) */
#define SQUASH_TIME       0.7f    /* flattened-walker linger before removal (s) */
#define HUSK_REVIVE_Q     22      /* Husk dormancy in quanta (~5.5 s) before it stands up */
#define HUSK_WOBBLE_Q      3      /* trailing quanta of pre-revival wobble tell */
#define START_LIVES        3      /* spare units at campaign start (level-grammar §10.4) */

#define ENEMY_GROUP_TOKEN 12      /* spawn-id 12: a 2-3 walker group (level-grammar §13.4) */

/*
 * Emerger + ranged-thrower families (M4b, cast.md §5.5/§5.6).  Numbers are
 * Kilix's own units (px, px/s, px/s^2) and the coarse timings count SK_QUANTUM
 * ticks, NOT the studied cadences.
 */
/* Vent-Maw — the vent emerger.  It rides its home column and telescopes up. */
#define MAW_RISE          20.0f  /* head travel out of the vent (px, cast.md 16-24) */
#define MAW_EMERGE_RATE   1.4f   /* extension change per second (rise/retract ~0.7 s) */
#define MAW_SUPPRESS      33.0f  /* horizontal suppression band (~2 tiles): stays down
                                    while Kilix is beside the vent (the studied rule) */
#define MAW_LETHAL        0.35f  /* extension above which the jaws bite (below = tell) */
#define MAW_OUT_Q         4      /* quanta held fully out (~1 s) */
#define MAW_HIDE_Q        4      /* quanta held hidden (~1 s) */

/* Riveter — the ranged thrower.  A slow ledge-shimmier that lobs rivets. */
#define RIVETER_SHIMMY    16.0f  /* ledge shimmy speed (px/s, cast.md ~16) */
#define RIVETER_RANGE     16.0f  /* shimmy half-range either side of home (px) */
#define RIVETER_THROW_Q   3      /* quanta between rivet lobs (~0.75 s) */

/* Rivet — the Riveter's thrown light actor: a ballistic arc that expires. */
#define RIVET_W           5.0f   /* projectile AABB (cast.md ~5x5) */
#define RIVET_H           5.0f
#define RIVET_SPEED       72.0f  /* horizontal launch speed (px/s, cast.md ~72) */
#define RIVET_VY0        -140.0f /* upward launch giving the parabolic arc (px/s) */
#define RIVET_GRAVITY     420.0f /* light arc gravity (px/s^2) */
#define RIVET_FALL_MAX    260.0f /* rivet terminal fall (px/s) */
#define RIVET_LIFE        2.4f   /* despawn lifetime (s) */
#define MAX_PROJECTILES   8      /* the light thrown-actor pool (own, off the machine pool) */

/*
 * Boss + power-up ladder (M4c, cast.md §4/§5.12).  The Vault Guardian is a large
 * machine at the district core-vault; its numbers are Kilix's own units and its
 * slow cadences count SK_QUANTUM ticks, NOT the studied timings.
 */
/* Vault Guardian (EN_GUARDIAN) — the per-district Gate-arena boss. */
#define GUARDIAN_W        26.0f  /* boss AABB width (spans ~1.6 cells) */
#define GUARDIAN_H        28.0f  /* boss AABB height (spans ~1.75 cells) */
#define GUARDIAN_PACE     15.0f  /* pacing speed (px/s, cast.md §5.12: slow warden) */
#define GUARDIAN_RANGE    40.0f  /* pace half-range from the dais anchor (px, ~2.5 tiles) */
#define GUARDIAN_CORE_HP  5      /* core-overload hits to crack the shell (cast.md §5.12) */
#define GUARDIAN_HATCH_Q  10     /* quanta the chest hatch stays sealed between opens (~2.5 s) */
#define GUARDIAN_OPEN_Q   2      /* quanta the hatch holds open, exposing the core (~0.5 s) */
#define GUARDIAN_HATCH_RATE 2.2f /* hatch open/close animation rate (per second) */
#define GUARDIAN_HATCH_OPEN 0.5f /* emerge above which the core is exposed / vulnerable */

/* Plasma bolt (PJ_PLASMA) — the Guardian's slow horizontal fire attack. */
#define PLASMA_W          6.0f
#define PLASMA_H          6.0f
#define PLASMA_SPEED      75.0f  /* horizontal travel (px/s, cast.md §5.12) */
#define PLASMA_LIFE       4.0f   /* despawn lifetime (s) */

/* Phase Pulse (PJ_PULSE) — Charged Kilix's ranged phase-bolt (cast.md §3.5). */
#define PULSE_W           5.0f
#define PULSE_H           5.0f
#define PULSE_SPEED       240.0f /* horizontal launch (px/s) */
#define PULSE_VY0        -40.0f  /* slight upward launch giving the shallow arc */
#define PULSE_GRAVITY     360.0f /* light arc gravity (px/s^2) */
#define PULSE_FALL_MAX    240.0f /* pulse terminal fall (px/s) */
#define PULSE_BOUNCE      0.55f  /* vy retained on a floor bounce */
#define PULSE_MAX_BOUNCE  2      /* despawn after this many floor bounces (cast.md: once/twice) */
#define PULSE_LIFE        1.6f   /* despawn lifetime (s) */
#define MAX_PULSES        2      /* on-screen phase-bolt cap (cast.md §3.5) */

/* Aegis Mote — the star-equivalent temporary invuln, counted in quanta so it
 * expires on the interval quantum (cast.md §4.2). */
#define AEGIS_Q           40     /* ~10 s of full invuln (40 quanta * 0.25 s) */

/* Power-up scoring (representative; the full table lands at M6). */
#define SCORE_MOTE        100
#define SCORE_MULTI       300
#define SCORE_POWER_FULL  1000   /* a power block struck at max tier: never wasted */
#define SCORE_GUARDIAN    5000   /* core-overload unmask reward (cast.md §6) */
#define SCORE_SEAL        2000   /* seal-switch collapse defeat */

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
    T_SEAL,            /* Gate-vault seal switch: struck by overlap, collapses the
                          Guardian's dais (the "axe" analogue, cast.md §5.12) */
    TILE_KIND_COUNT
};

/*
 * Cache / hidden dispenser content codes (level-grammar §13.3): a ?-node's payload
 * is level-data, never art-derived.  State-dependent contents follow cast.md §4 —
 * a power block yields the NEXT phase-shell tier by Kilix's current tier.  Shared
 * between data.c (authoring) and game.c (the head-bonk dispense).
 */
enum { CN_MOTE, CN_MULTI, CN_POWER, CN_SHELL, CN_AEGIS };

/* A cache node's dispensed content, kept beside the tile grid so the head-bonk
 * dispense knows a struck ?-node's payload without any art-derived value. */
#define MAX_CACHES 48
typedef struct { uint8_t col, row, content; } CacheNode;

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
 * Machine families by FUNCTIONAL ROLE (original expression; the genre role
 * mapping lives only in the research bibles).  M4a ships the two "normal"
 * families; later M4 agents extend this enum in place.  These ids intentionally
 * match the data.c spawn-roster ids 0 (Slag-Treader) and 1 (Carapod) so a
 * decoded spawn is a live family with no lookup table.
 */
enum {
    EN_WALKER  = 0,  /* Slag-Treader — ground walker: plods, FALLS off ledges (M4a) */
    EN_TURNER  = 1,  /* Carapod — shelled turner: walks, TURNS at ledges (an ID check
                        with sub-state 0, the corrected red-vs-green reading); a stomp
                        retracts it to a kickable Husk (M4a) */
    /* ids 2,3,5,6 (Carapod-Kite, Dreadpod, Siphon-Squid, Fin-Drifter) are reserved
       SKLF roster slots, not yet shipped as live families — family_shipped() drops
       a decoded spawn that lands on one. */
    EN_MAW     = 4,  /* Vent-Maw — the vent emerger (M4b); SKLF spawn-roster id 4 */
    EN_RIVETER = 7,  /* Riveter — the ranged thrower (M4b); SKLF spawn-roster id 7 */
    EN_GUARDIAN = 11, /* Vault Guardian — the per-district Gate-arena boss (M4c);
                         SKLF spawn-roster id 11 (level-grammar §13.4, cast.md §5.12) */
    EN_OVERSEER = 13, /* The Overseer — the unique finale boss; SKLF id 13.  Wired as a
                         Guardian-behaviour placeholder here; fully realised at M6 */
    ENEMY_KIND_COUNT = 14  /* one past the highest roster id */
};

/*
 * Enemy.state is one shared byte (game-architecture §4.3): the low 3 bits hold a
 * sub-state enum, the high bits are modifier flags — so a stomped turner becomes
 * a sliding hazard just by setting a flag (the studied emergent-from-one-byte
 * model).  M4a uses the WALK/HUSK/SQUASHED sub-states and the moving-shell flag.
 */
enum { ES_WALK, ES_HUSK, ES_SQUASHED };
#define ES_SUBSTATE   0x07u   /* low 3 bits: sub-state enum */
#define ES_EMERGED    0x40u   /* d6: a Vent-Maw is in its out-phase (rising / held out) */
#define ES_SHELL_MOV  0x80u   /* d7: a Husk is sliding — a hazard to machines + Kilix */

/*
 * A live machine in the field.  Every field is a plain scalar so GameState stays
 * trivially memcmp-comparable; cosmetic gait is derived render-side from
 * scene_time (never stored) so the sim carries no art-derived value.
 */
typedef struct {
    bool     active;
    int      kind;            /* one of the EN_* families */
    float    x, y, vx, vy;    /* AABB top-left position and velocity */
    float    home_x, home_y;  /* spawn anchor */
    float    alert, tell;     /* local wake ramp + nonlethal activation tell (0..1) */
    float    squash;          /* flattened-walker linger countdown (s) */
    float    emerge;          /* Vent-Maw vertical extension / Guardian hatch-open amount:
                                 0 hidden-or-sealed .. 1 fully out / hatch wide open */
    int      revive_q;        /* dormant-Husk revival countdown in quanta (0 = none), OR a
                                 Guardian's remaining core-overload HP (a boss never revives) */
    int      phase_q;         /* coarse-quantum dwell: Maw emerge/hide cadence, Riveter throw
                                 countdown, or Guardian hatch/attack cadence (one role at a time) */
    int      facing;          /* +1 / -1 travel direction */
    uint8_t  state;           /* ES_* bitfield */
    uint8_t  param;           /* spawn variant / group token */
} Enemy;

/*
 * A light thrown actor — the Riveter's rivet (and later a Charged-Kilix
 * phase-bolt).  It carries its own AABB, its own ballistic motion, and its own
 * despawn, and lives in a pool SEPARATE from the machine slot pool so it never
 * eats a machine's density budget.  Plain scalars keep GameState memcmp-able.
 */
typedef struct {
    bool     active;
    int      kind;            /* one of the PJ_* families */
    float    x, y, vx, vy;    /* AABB top-left position and velocity */
    float    life;            /* despawn countdown (s) */
    int      facing;          /* travel sign; drives cosmetic spin render-side */
    int      bounces;         /* floor bounces so far (phase-bolt: expires past MAX) */
} Projectile;

/* Projectile families.  RIVET + PLASMA are hostile (they injure Kilix); PULSE is
 * Charged Kilix's own phase-bolt (it dissolves machines and cracks a boss core). */
enum { PJ_RIVET, PJ_PULSE, PJ_PLASMA };

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
    CacheNode caches[MAX_CACHES];            /* ?-node payloads, parallel to the grid */
    int     cache_count;
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
    float prev_bottom;       /* box bottom before this tick's move (stomp arbitration) */
    int   power_tier;        /* 0 Bare .. 1 Plated .. 2 Charged (cast.md §4) */
    float invuln;            /* post-hit / respawn i-frames, seconds */
    int   aegis_q;           /* Aegis-Mote temporary invuln, in quanta (expires on the
                                interval quantum; contact-defeats machines, cast.md §4.2) */
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

    /* Machines (M4a): a capped active field pool fed by a camera-relative
     * schedule.  spawn_cursor only ever advances because the camera ratchets. */
    Enemy enemies[MAX_ACTIVE_ENEMIES];
    Projectile projectiles[MAX_PROJECTILES];   /* light thrown actors (rivets) */
    int   spawn_cursor;      /* next vault_data.enemies index to materialise */
    int   lives;             /* spare units remaining */
    int   deaths;            /* deaths taken (counted; part of the state fingerprint) */
    int   score;             /* running score (chain-aware; full table lands at M6) */
    int   chain;             /* airborne stomp / sliding-Husk chain depth */
    float state_timer;       /* transient-state dwell (GS_LIFE_LOST respawn beat) */
    bool  guardian_down;     /* the Gate boss's dais has collapsed (either kill path) */
    bool  guardian_unmasked; /* the decoy shell was cracked (core-overload path only) */

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
void  game_fire_pulse(void);                  /* Charged Kilix emits a phase-bolt */
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

/*
 * Sound-effect ids (sound.c owns the synthesis; game.c only names these).  Each
 * indexes a one/two-channel chip-sequencer song in sfx_table[]; they are played
 * with sound_play(id, vol, pitch).  SFX_JET is special: a held looping drone
 * driven by sound_jet(), never by sound_play (which rejects it, as in kilix-jpak).
 */
enum {
    SFX_JUMP,        /* Kilix launches: a fast rising chirp */
    SFX_LAND,        /* feet touch down: a short dull thud + low tick */
    SFX_STOMP,       /* a machine flattened underfoot: a gapped-tone punch */
    SFX_SHELL_KICK,  /* a dormant Husk booted into a slide: metallic knock */
    SFX_PHASE_BOLT,  /* Charged Kilix emits a Phase Pulse: thin downward sweep */
    SFX_POWER_UP,    /* a phase-shell / Aegis tier gained: rising stepped run */
    SFX_HURT,        /* Kilix demoted or struck: stuttering descending tone */
    SFX_PICKUP,      /* a mote / multi cache: quick ascending arpeggio */
    SFX_EXTRA_LIFE,  /* a spare unit awarded: six-note rising jingle */
    SFX_EXIT_OPEN,   /* the vault seals/exit opens: long upward flourish */
    SFX_BOSS_HIT,    /* a hit lands on an exposed Guardian core: low blast */
    SFX_JET,         /* held thruster drone (loop-only, via sound_jet) */
    SFX_COUNT
};

/*
 * Music track ids (sound.c owns the songs).  Passed to sound_music(); calling it
 * again with the same id is a no-op, so game/main code may drive it every frame.
 * MUS_NONE stops music.  Beds/boss/title loop; the clear/game-over stings are
 * one-shots that ring out once and stop.
 */
enum {
    MUS_NONE = -1,
    MUS_TITLE = 0,   /* "Vault Reveille" — the title fanfare */
    MUS_RUST_FLATS,  /* "Sunward Run" — the RUST FLATS district-1 bed */
    MUS_BOSS,        /* "The Warden Machine" — the Vault Guardian theme */
    MUS_CLEAR,       /* "Vault Sealed" — the level-clear sting */
    MUS_GAMEOVER,    /* "Salvage Lost" — the game-over sting */
    MUS_COUNT
};

/* sound.c */
bool sound_init(void);
void sound_shutdown(void);
void sound_set_enabled(bool on);
bool sound_is_enabled(void);
void sound_play(int id, float volume, float pitch);
void sound_jet(bool active, float intensity);
void sound_music(int track);          /* switch the live music track (MUS_*) */
bool sound_render_selfcheck(void);    /* offline byte-determinism check (no sink) */
void sound_selftest_play(void);       /* --sound-test: play every SFX + song */

#endif

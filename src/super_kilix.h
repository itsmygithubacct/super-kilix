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
#define HUD_ROWS   2
#define PLAY_ROWS  13
#define VAULT_ROWS 16

#define TICK_DT (1.0f / 60.0f)   /* fixed 60 Hz sim tick */

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

/*
 * Kilix's on-screen pose.  M1 needs only the fields draw_kilix reads to place
 * and animate the salvager; M2 extends this in place with velocity, collision
 * box, fuel, and the rest of the physics state.  Every field is a plain scalar
 * so GameState stays trivially memcmp-comparable.
 */
typedef struct {
    float x, y;              /* logical-pixel position of the sprite's top-left */
    int   facing;            /* +1 faces right, -1 faces left */
    bool  grounded;
    bool  thrusting;
    bool  phasing;
    float gait_phase;        /* walk oscillator phase, radians */
    float gait_amount;       /* 0 idle .. 1 full stride */
} Player;

/*
 * GameState is one flat, pointer-free, trivially memcpy/memcmp-comparable POD.
 * The renderer snapshots and compares it to prove it never mutates simulation
 * state, so it must never gain a pointer into heap-owned data.  M0 keeps it
 * minimal; later milestones extend it in place.
 */
typedef struct {
    int      state;          /* one of the GS_* enumerators */
    int      W, H;           /* terminal pixel size (0 in headless modes) */
    bool     quit, headless, sound_on;
    uint32_t rng;            /* seeded xorshift32 — the only simulation RNG */
    uint64_t tick;           /* fixed-step counter */
    float    scene_time;     /* seconds of wall-free cosmetic animation time */
    Player   player;         /* Kilix's pose (drawn by render.c) */
} GameState;

extern GameState G;

/* data.c */
const char *tile_name(int tile);
const char *machine_name(int kind);
const char *district_name(int district);

/* game.c */
float clampf(float v, float lo, float hi);
float game_randf(void);                       /* xorshift32 on G.rng */
void  game_init(int w, int h, uint32_t seed);
void  game_shutdown(void);
void  game_tick(void);
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

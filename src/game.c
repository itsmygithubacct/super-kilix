/* Simulation core: the deterministic clock, the seeded RNG, and validation.
 * M0 has no gameplay yet; game_tick only advances the fixed-step counter and
 * steps the RNG so that determinism and validation can be proven end to end. */
#include "super_kilix.h"

#include <math.h>
#include <stdio.h>

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

void game_init(int w, int h, uint32_t seed)
{
    GameState fresh = {0};
    G = fresh;
    G.W = w;
    G.H = h;
    G.rng = seed ? seed : 0x6a09e667u;
    G.state = GS_TITLE;
    G.sound_on = true;
    G.tick = 0;
    G.scene_time = 0.0f;
    G.player.facing = 1;
    G.player.grounded = true;
}

void game_shutdown(void)
{
    /* M0 owns no external resources; later milestones flush the profile here. */
}

void game_tick(void)
{
    if (G.quit) return;
    (void)game_randf();          /* keep the RNG stream advancing deterministically */
    G.scene_time += TICK_DT;     /* cosmetic animation clock (read-only in render) */
    G.tick++;
}

bool game_validate(char *err, size_t len)
{
    if (G.state < 0 || G.state >= GS_STATE_COUNT) {
        if (err && len) snprintf(err, len, "state %d out of range", G.state);
        return false;
    }
    const float floats[] = {
        G.scene_time, G.player.x, G.player.y,
        G.player.gait_phase, G.player.gait_amount
    };
    for (size_t i = 0; i < sizeof floats / sizeof floats[0]; i++)
        if (!isfinite(floats[i])) {
            if (err && len) snprintf(err, len, "non-finite float field %zu", i);
            return false;
        }
    return true;
}

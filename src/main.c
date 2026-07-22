/* Entry point, signal handling, the fixed-step terminal loop, and every
 * deterministic headless mode.  M0 wires the CLI, the trivial --selftest, and
 * the family's shared 60 Hz clock; the visual loop gains teeth at M1. */
#include "super_kilix.h"

#include "kilix_game_loop.h"

#include <errno.h>
#include <limits.h>
#include <math.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define DUMP_LEVEL_MAX CAMPAIGN_VAULTS   /* the campaign's vault count */

/* ------------------------------------------------------------------ signals */

static void on_signal(int signal_number)
{
    (void)signal_number;
    term_emergency_restore();
    _exit(1);
}

static void install_signals(void)
{
    static const int signals[] = {SIGINT, SIGTERM, SIGHUP, SIGSEGV, SIGBUS,
                                  SIGFPE, SIGABRT};
    for (size_t i = 0; i < sizeof signals / sizeof signals[0]; i++)
        signal(signals[i], on_signal);
}

/* Headless modes must never touch the profile or the audio device. */
static void headless_environment(void)
{
    setenv("SUPER_KILIX_NO_PROFILE", "1", 1);
    sound_set_enabled(false);
}

/* ---------------------------------------------------- key event -> logical key */

static bool event_letter(const kittykb_event *event, char lower)
{
    char upper = (char)(lower - 'a' + 'A');
    return kittykb_event_matches_key(event, (uint32_t)(unsigned char)lower) ||
           kittykb_event_matches_key(event, (uint32_t)(unsigned char)upper);
}

static int game_key(const kittykb_event *event)
{
    static const char letters[] = "adhjlmpqrswxz";
    for (size_t i = 0; i < sizeof letters - 1; i++)
        if (event_letter(event, letters[i])) return letters[i];
    switch (event->key) {
    case KITTYKB_KEY_ENTER:  return KEY_ENTER;
    case KITTYKB_KEY_ESCAPE: return KEY_ESC;
    case KITTYKB_KEY_UP:     return KEY_UP;
    case KITTYKB_KEY_DOWN:   return KEY_DOWN;
    case KITTYKB_KEY_RIGHT:  return KEY_RIGHT;
    case KITTYKB_KEY_LEFT:   return KEY_LEFT;
    default: return event->key <= (uint32_t)INT_MAX ? (int)event->key : -1;
    }
}

static bool continuous_key(int key)
{
    return key == KEY_LEFT || key == KEY_RIGHT || key == KEY_UP || key == KEY_DOWN ||
           key == 'a' || key == 'd' || key == 'w' || key == 's' ||
           key == ' ' || key == 'z';
}

static bool interrupt_event(const kittykb_event *event)
{
    return event->key == 3u ||
           (event_letter(event, 'c') && (event->modifiers & KITTYKB_MOD_CTRL));
}

/* ------------------------------------------------------------- test harness */

static int failures;
#define EXPECT(condition, label) do { \
    if (condition) printf("PASS: %s\n", label); \
    else { printf("FAIL: %s\n", label); failures++; } \
} while (0)

/* Drop the player onto the top-left of the tile cell (col,row), at rest. */
static void place_player(int col, int row)
{
    Player *p = &G.player;
    int facing = p->facing;
    memset(p, 0, sizeof *p);
    p->facing = facing ? facing : 1;
    p->buffer_tick = -1;
    p->x = (float)(col * TILE_SIZE) + 2.0f;
    p->y = (float)(row * TILE_SIZE);
    p->grounded = false;
}

/* Replace the loaded vault with an empty grid of the given extent (out-of-bounds
 * below still reads solid, so a drop lands on a virtual floor).  The machine field
 * and its schedule are cleared so no stray spawn leaks into a physics fixture. */
static void load_empty_grid(int cols, int rows)
{
    memset(&G.vault_data, 0, sizeof G.vault_data);
    memset(G.enemies, 0, sizeof G.enemies);
    G.spawn_cursor = 0;
    G.vault_data.cols = cols;
    G.vault_data.rows = rows;
    G.cam_x = G.cam_x_max = G.cam_y = 0.0f;
    G.scroll_lock = false;
    G.state = GS_PLAYING;
}

/* An empty grid with a continuous baseline terrace floor — a flat arena for the
 * machine fixtures (matching a real vault's row-12 floor + subsurface below). */
static void load_flat_arena(int cols)
{
    load_empty_grid(cols, PLAY_ROWS);
    for (int c = 0; c < cols; c++) G.vault_data.tiles[PLAY_ROWS - 1][c] = T_HULL;
}

/* Park Kilix, grounded and at rest, on the baseline floor at a column. */
static void park_player(int col)
{
    memset(&G.player, 0, sizeof G.player);
    G.player.facing = 1;
    G.player.buffer_tick = -1;
    G.player.x = (float)(col * TILE_SIZE) + 2.0f;
    G.player.y = (float)((PLAY_ROWS - 1) * TILE_SIZE) - PLAYER_H;
    G.player.prev_bottom = G.player.y + PLAYER_H;
    G.player.grounded = true;
}

/* Drop a live, already-activated machine of a family resting on the floor row. */
static Enemy *place_enemy(int slot, int kind, int col, int row)
{
    Enemy *e = &G.enemies[slot];
    memset(e, 0, sizeof *e);
    e->active = true;
    e->kind = kind;
    e->facing = -1;
    e->state = ES_WALK;
    e->alert = 1.0f;
    e->tell = 1.0f;                 /* past its tell: active and lethal */
    e->x = (float)(col * TILE_SIZE) + (TILE_SIZE - ENEMY_W) * 0.5f;
    e->y = (float)((row + 1) * TILE_SIZE) - ENEMY_H;
    e->home_x = e->x;
    e->home_y = e->y;
    return e;
}

/* Drop a live, awake Vault Guardian boss on the baseline floor at a column, with
 * its bulk resting feet-on-floor and a full core-overload HP bar. */
static Enemy *place_boss(int slot, int kind, int col)
{
    Enemy *e = &G.enemies[slot];
    memset(e, 0, sizeof *e);
    e->active = true;
    e->kind = kind;
    e->facing = -1;
    e->state = ES_WALK;
    e->alert = 1.0f;
    e->tell = 1.0f;
    e->revive_q = GUARDIAN_CORE_HP;
    e->phase_q = GUARDIAN_HATCH_Q;
    e->x = (float)(col * TILE_SIZE) + (TILE_SIZE - GUARDIAN_W) * 0.5f;
    e->y = (float)((PLAY_ROWS - 1) * TILE_SIZE) - GUARDIAN_H;   /* feet on the floor */
    e->home_x = e->x;
    e->home_y = e->y;
    return e;
}

/* Tick a fixed number of frames with no held input (autopilot off). */
static void idle_ticks(int n)
{
    for (int i = 0; i < n; i++) {
        game_set_held_controls(true, false, false, false, false, false, false);
        game_tick();
    }
}

/* ---------------------------------------------------------------- CLI parse */

static bool parse_u32_argument(const char *text, uint32_t *out)
{
    if (!text || !*text || !out) return false;
    for (const char *p = text; *p; p++)
        if (*p < '0' || *p > '9') return false;

    errno = 0;
    char *end = NULL;
    unsigned long value = strtoul(text, &end, 10);
    if (errno == ERANGE || end == text || *end != '\0' || value > UINT32_MAX)
        return false;
    *out = (uint32_t)value;
    return true;
}

static bool parse_int_argument(const char *text, int minimum, int maximum, int *out)
{
    uint32_t value;
    if (!out || minimum < 0 || maximum < minimum ||
        !parse_u32_argument(text, &value) || value > (uint32_t)maximum ||
        value < (uint32_t)minimum) return false;
    *out = (int)value;
    return true;
}

static int option_arity_error(const char *option, const char *expectation)
{
    fprintf(stderr, "%s %s\n", option, expectation);
    return 2;
}

/* ------------------------------------------------------------- headless modes */

/* FNV-1a over the whole GameState — a deterministic fingerprint printed in the
 * PASS line so the Makefile's twin-run `cmp` is a real byte-for-byte determinism
 * gate, not merely a comparison of a constant string. */
static uint64_t state_hash(void)
{
    const uint8_t *bytes = (const uint8_t *)&G;
    uint64_t hash = UINT64_C(1469598103934665603);
    for (size_t i = 0; i < sizeof G; i++) {
        hash ^= bytes[i];
        hash *= UINT64_C(1099511628211);
    }
    return hash;
}

static int selftest(uint32_t seed, int ticks)
{
    headless_environment();
    char error[192];
    if (ticks <= 0) ticks = 12000;

    if (!level_validate_campaign(error, sizeof error)) {
        fprintf(stderr, "FAIL campaign validation: %s\n", error);
        return 1;
    }

    game_init(512, 480, seed);
    G.headless = true;
    G.sound_on = false;
    game_start(0);
    for (int i = 0; i < ticks; i++) {
        game_autopilot();
        game_tick();
        if (!game_validate(error, sizeof error)) {
            fprintf(stderr, "FAIL seed=%u tick=%d: %s\n", seed, i, error);
            game_shutdown();
            return 1;
        }
    }

    /* Independent of the bot's luck, drive every vault a short while and force
     * the clear so GS_VAULT_CLEAR is provably reachable from each (§7.4). */
    int levels = 0;
    for (int v = 0; v < CAMPAIGN_VAULTS; v++) {
        game_load_level(v);
        for (int i = 0; i < 90; i++) {
            game_autopilot();
            game_tick();
            if (!game_validate(error, sizeof error)) {
                fprintf(stderr, "FAIL vault=%d tick=%d: %s\n", v + 1, i, error);
                game_shutdown();
                return 1;
            }
        }
        game_force_level_clear();
        if (G.state != GS_VAULT_CLEAR) {
            fprintf(stderr, "FAIL vault=%d did not reach clear\n", v + 1);
            game_shutdown();
            return 1;
        }
        levels++;
    }

    printf("PASS selftest seed=%u ticks=%d levels=%d state=%016llx\n",
           seed, ticks, levels, (unsigned long long)state_hash());
    game_shutdown();
    return 0;
}

/* Tick with a fixed held-control set until the player is grounded and at rest. */
static void settle_on_floor(void)
{
    for (int i = 0; i < 45; i++) {
        game_set_held_controls(true, false, false, false, false, false, false);
        game_tick();
    }
}

static int rules_test(void)
{
    failures = 0;
    headless_environment();
    char error[192];
    game_init(512, 480, 7);
    G.headless = true; G.sound_on = false;

    /* --- skid vs release coast: opposing input stops harder than a release --- */
    load_empty_grid(20, PLAY_ROWS);
    place_player(4, 4);
    settle_on_floor();
    G.player.vx = 100.0f;
    game_set_held_controls(true, true, false, false, false, false, false);
    float v0 = G.player.vx; game_tick();
    float skid_delta = v0 - G.player.vx;
    G.player.vx = 100.0f;
    game_set_held_controls(true, false, false, false, false, false, false);
    v0 = G.player.vx; game_tick();
    float coast_delta = v0 - G.player.vx;
    EXPECT(skid_delta > coast_delta,
           "skid decelerates harder than a release coast");

    /* --- terminal velocity is bounded over a long drop --- */
    load_empty_grid(6, VAULT_ROWS);
    place_player(3, 0);
    game_set_held_controls(true, false, false, false, false, false, false);
    float peak_vy = 0.0f; bool bounded = true;
    for (int i = 0; i < 40; i++) {
        game_tick();
        if (G.player.vy > peak_vy) peak_vy = G.player.vy;
        if (!G.player.grounded && G.player.vy > FALL_MAX + 1.0f) bounded = false;
    }
    EXPECT(bounded && peak_vy >= FALL_MAX - 1.0f && peak_vy <= FALL_MAX + 1.0f,
           "terminal velocity stays bounded at the fall cap");

    /* --- running jump clears a wider gap than a standing jump --- */
    load_empty_grid(80, PLAY_ROWS);
    place_player(4, 4);
    settle_on_floor();
    float start_x = G.player.x;
    game_set_held_controls(true, false, true, false, false, true, false);   /* jump edge */
    game_tick();
    for (int i = 0; i < 300 && !G.player.grounded; i++) {
        game_set_held_controls(true, false, true, false, false, true, false);
        game_tick();
    }
    float standing_dx = G.player.x - start_x;

    load_empty_grid(80, PLAY_ROWS);
    place_player(4, 4);
    settle_on_floor();
    for (int i = 0; i < 45; i++) {          /* build up to run speed first */
        game_set_held_controls(true, false, true, false, false, false, true);
        game_tick();
    }
    start_x = G.player.x;
    game_set_held_controls(true, false, true, false, false, true, true);    /* jump edge */
    game_tick();
    for (int i = 0; i < 300 && !G.player.grounded; i++) {
        game_set_held_controls(true, false, true, false, false, true, true);
        game_tick();
    }
    float running_dx = G.player.x - start_x;
    EXPECT(running_dx > standing_dx + TILE_SIZE,
           "a running jump clears a wider gap than a standing jump");

    /* --- a wall zeroes horizontal velocity and is never tunnelled at run speed --- */
    load_empty_grid(20, PLAY_ROWS);
    for (int r = 0; r < PLAY_ROWS; r++) G.vault_data.tiles[r][12] = T_HULL;
    place_player(4, 4);
    settle_on_floor();
    float wall_face = (float)(12 * TILE_SIZE);
    bool tunnelled = false;
    for (int i = 0; i < 200; i++) {
        game_set_held_controls(true, false, true, false, false, false, true);
        game_tick();
        if (G.player.x + PLAYER_W > wall_face + 0.5f) tunnelled = true;
    }
    EXPECT(!tunnelled, "the player never tunnels through a wall at run speed");
    EXPECT(G.player.vx == 0.0f, "wall contact zeroes horizontal velocity");

    /* --- one-way platform: solid from above, passable from below --- */
    load_empty_grid(20, PLAY_ROWS);
    for (int c = 6; c <= 10; c++) G.vault_data.tiles[6][c] = T_LEDGE;
    place_player(8, 1);                     /* start above the grate, falling */
    game_set_held_controls(true, false, false, false, false, false, false);
    for (int i = 0; i < 60; i++) game_tick();
    float grate_top = (float)(6 * TILE_SIZE);
    EXPECT(G.player.grounded &&
           fabsf((G.player.y + PLAYER_H) - grate_top) < 1.5f,
           "one-way platform is solid from above");

    load_empty_grid(20, PLAY_ROWS);
    for (int c = 6; c <= 10; c++) G.vault_data.tiles[6][c] = T_LEDGE;
    place_player(8, 8);                     /* start below the grate, moving up */
    G.player.vy = -320.0f;
    float rise_from = G.player.y;
    for (int i = 0; i < 3; i++) {
        game_set_held_controls(true, false, false, false, false, false, false);
        game_tick();
    }
    EXPECT(G.player.y < rise_from - 4.0f,
           "one-way platform is passable from below");

    /* --- SKLF level format: the campaign validates, every vault rebuilds
           byte-identically, and the bot traverses the authored district-1
           vault (FIRST TERRACE) to within reach of its exit --- */
    EXPECT(level_validate_campaign(error, sizeof error),
           "campaign topology validates end to end");

    {
        static VaultData a, b;   /* static: too large for the fixture stack */
        bool all_identical = true;
        for (int i = 0; i < CAMPAIGN_VAULTS && all_identical; i++) {
            level_build(i, &a);
            level_build(i, &b);
            if (memcmp(&a, &b, sizeof a) != 0) all_identical = false;
        }
        EXPECT(all_identical, "every level_build(i) rebuilds byte-identically");
    }

    game_start(0);
    {
        float exit_x = (float)(G.vault_data.exit_col * TILE_SIZE);
        float reached = G.player.x;
        for (int i = 0; i < 4000 && G.player.x < exit_x - TILE_SIZE; i++) {
            game_autopilot();
            game_tick();
            if (G.player.x > reached) reached = G.player.x;
        }
        EXPECT(reached >= exit_x - 3.0f * TILE_SIZE,
               "autopilot traverses FIRST TERRACE to its exit");
    }

    /* --- M4a machine substrate + the two normal-enemy families --- */

    /* A stomp defeats a walker with the FLAT bounce; holding jump cannot raise it. */
    {
        float apex[2] = {0.0f, 0.0f}, bounce[2] = {0.0f, 0.0f};
        bool defeated[2] = {false, false};
        for (int hold = 0; hold < 2; hold++) {
            load_flat_arena(24);
            Enemy *e = place_enemy(0, EN_WALKER, 12, PLAY_ROWS - 2);
            memset(&G.player, 0, sizeof G.player);
            G.player.facing = 1; G.player.buffer_tick = -1;
            G.player.x = e->x;
            G.player.y = e->y - PLAYER_H - 2.0f;
            G.player.prev_bottom = G.player.y + PLAYER_H;
            G.player.vy = 60.0f;
            G.player.invuln = 1.0e9f;      /* never a stray side-hit; stomps still fire */
            float lowest = G.player.y;
            for (int i = 0; i < 80; i++) {
                game_set_held_controls(true, false, false, false, false, hold != 0, false);
                game_tick();
                if (!defeated[hold] && ((e->state & ES_SUBSTATE) == ES_SQUASHED || !e->active)) {
                    defeated[hold] = true;
                    bounce[hold] = G.player.vy;      /* the impulse on the defeat tick */
                }
                if (defeated[hold] && G.player.y < lowest) lowest = G.player.y;
                if (defeated[hold] && G.player.grounded && i > 4) break;
            }
            apex[hold] = lowest;
        }
        EXPECT(defeated[0] && defeated[1], "a stomp defeats a ground walker");
        EXPECT(bounce[0] < -150.0f && bounce[0] > -230.0f,
               "the stomp applies the flat upward bounce");
        EXPECT(fabsf(apex[0] - apex[1]) < 4.0f,
               "the stomp bounce is flat (holding jump does not raise it)");
    }

    /* A kicked Husk slides downrange and defeats a second machine.  The camera is
     * locked so both machines stay inside the on-screen band for the whole kick. */
    {
        load_flat_arena(40);
        G.scroll_lock = true;              /* keep both machines in view during the slide */
        Enemy *pod = place_enemy(0, EN_TURNER, 6, PLAY_ROWS - 2);
        Enemy *victim = place_enemy(1, EN_WALKER, 18, PLAY_ROWS - 2);
        victim->facing = 1;                /* walk away from the player, not toward it */
        memset(&G.player, 0, sizeof G.player);
        G.player.facing = 1; G.player.buffer_tick = -1;
        G.player.x = pod->x;
        G.player.y = pod->y - PLAYER_H - 2.0f;
        G.player.prev_bottom = G.player.y + PLAYER_H;
        G.player.vy = 60.0f;
        G.player.invuln = 1.0e9f;
        for (int i = 0; i < 12 && (pod->state & ES_SUBSTATE) != ES_HUSK; i++) idle_ticks(1);
        bool husked = (pod->state & ES_SUBSTATE) == ES_HUSK;
        /* side-touch the dormant Husk from its left so it launches right */
        G.player.vy = 0.0f; G.player.grounded = true;
        G.player.x = pod->x - 6.0f; G.player.y = pod->y;
        G.player.prev_bottom = G.player.y + PLAYER_H;
        float husk_x0 = pod->x;
        for (int i = 0; i < 240; i++) {
            idle_ticks(1);
            if (!victim->active || (victim->state & ES_SUBSTATE) == ES_SQUASHED) break;
        }
        EXPECT(husked, "stomping the turner retracts it to a Husk");
        EXPECT((pod->state & ES_SHELL_MOV) != 0u &&
               fabsf(pod->x - husk_x0) > 2.0f * TILE_SIZE,
               "the kicked Husk travels downrange");
        EXPECT(!victim->active || (victim->state & ES_SUBSTATE) == ES_SQUASHED,
               "the sliding Husk defeats a second machine");
    }

    /* The ledge-respecting variant turns at a ledge while the base walks off. */
    {
        load_flat_arena(32);
        for (int c = 8;  c <= 15; c++) G.vault_data.tiles[8][c] = T_BEDROCK;
        for (int c = 20; c <= 27; c++) G.vault_data.tiles[8][c] = T_BEDROCK;
        Enemy *w = place_enemy(0, EN_WALKER, 14, 7); w->facing = 1;
        Enemy *t = place_enemy(1, EN_TURNER, 26, 7); t->facing = 1;
        park_player(2);
        G.player.invuln = 1.0e9f;
        float w_y0 = w->y, t_y0 = t->y;
        idle_ticks(150);
        EXPECT(w->y > w_y0 + (float)TILE_SIZE, "the base walker walks off the ledge");
        EXPECT(t->y < t_y0 + 4.0f,
               "the ledge-respecting variant turns and stays on the platform");
    }

    /* Contact damage: a tier is spent if armoured, else a life and a restart. */
    {
        load_flat_arena(24);
        Enemy *e = place_enemy(0, EN_WALKER, 12, PLAY_ROWS - 2);
        park_player(11);
        G.player.x = e->x - 8.0f; G.player.prev_bottom = G.player.y + PLAYER_H;
        G.player.power_tier = 1;
        G.lives = 3;
        int lives0 = G.lives;
        for (int i = 0; i < 8 && G.player.power_tier == 1; i++) {
            game_set_held_controls(true, false, true, false, false, false, false);
            game_tick();
        }
        EXPECT(G.player.power_tier == 0 && G.lives == lives0 && G.state == GS_PLAYING,
               "a hit at tier >= 1 drops one tier and costs no life");

        load_flat_arena(24);
        e = place_enemy(0, EN_WALKER, 12, PLAY_ROWS - 2);
        park_player(11);
        G.player.x = e->x - 8.0f; G.player.prev_bottom = G.player.y + PLAYER_H;
        G.player.power_tier = 0;
        G.lives = 3; lives0 = G.lives;
        for (int i = 0; i < 8 && G.state == GS_PLAYING; i++) {
            game_set_held_controls(true, false, true, false, false, false, false);
            game_tick();
        }
        EXPECT(G.state == GS_LIFE_LOST && G.lives == lives0 - 1,
               "a hit at tier 0 costs a life and restarts the vault");
    }

    /* No machine embeds in solid geometry across an autopilot pass. */
    {
        game_start(0);
        G.player.invuln = 1.0e9f;          /* keep the bot alive so it keeps stressing */
        bool clean = true;
        for (int i = 0; i < 3000 && clean; i++) {
            game_autopilot();
            game_tick();
            if (!game_validate(error, sizeof error)) clean = false;
        }
        EXPECT(clean, "no machine embeds in solid geometry across an autopilot pass");
    }

    /* Density cap: cram more spawns than the pool holds and prove it never overflows. */
    {
        game_start(0);
        G.player.invuln = 1.0e9f;
        for (int k = 0; k < MAX_ENEMIES; k++)
            G.vault_data.enemies[k] = (EnemySpawn){
                (uint8_t)EN_WALKER, (uint8_t)(6 + k), (uint8_t)(PLAY_ROWS - 2), 0 };
        G.vault_data.enemy_count = MAX_ENEMIES;
        G.spawn_cursor = 0;
        memset(G.enemies, 0, sizeof G.enemies);
        int cap_seen = 0; bool over = false;
        for (int i = 0; i < 600; i++) {
            game_autopilot();
            game_tick();
            int active = 0;
            for (int s = 0; s < MAX_ACTIVE_ENEMIES; s++)
                if (G.enemies[s].active) active++;
            if (active > cap_seen) cap_seen = active;
            if (active > MAX_ACTIVE_ENEMIES) over = true;
        }
        EXPECT(!over && cap_seen == MAX_ACTIVE_ENEMIES,
               "the machine slot pool fills to its cap and never exceeds it");
    }

    /* --- M4b: the Vent-Maw emerger + the Riveter ranged thrower --- */

    /* The emerger rises on cadence when Kilix is clear of the vent, and stays
     * suppressed while he stands beside it (the studied horizontal-band rule). */
    {
        load_flat_arena(30);
        Enemy *maw = place_enemy(0, EN_MAW, 15, PLAY_ROWS - 2);
        maw->phase_q = MAW_HIDE_Q;
        park_player(12);                   /* ~3 tiles off: past the suppression band */
        G.player.invuln = 1.0e9f;
        float peak = 0.0f;
        for (int i = 0; i < 220; i++) { idle_ticks(1); if (maw->emerge > peak) peak = maw->emerge; }
        EXPECT(peak > 0.6f, "the Vent-Maw emerges on its cadence when Kilix is clear");

        load_flat_arena(30);
        maw = place_enemy(0, EN_MAW, 15, PLAY_ROWS - 2);
        maw->phase_q = MAW_HIDE_Q;
        park_player(15);                   /* standing right beside the vent */
        G.player.invuln = 1.0e9f;
        float peak2 = 0.0f;
        for (int i = 0; i < 220; i++) { idle_ticks(1); if (maw->emerge > peak2) peak2 = maw->emerge; }
        EXPECT(peak2 < 0.15f,
               "the Vent-Maw stays suppressed while Kilix is beside the vent");
    }

    /* The thrower lobs a rivet on the interval quantum. */
    {
        load_flat_arena(30);
        Enemy *r = place_enemy(0, EN_RIVETER, 18, PLAY_ROWS - 2);
        r->phase_q = RIVETER_THROW_Q;
        park_player(12);
        G.player.invuln = 1.0e9f;          /* isolate the lob from contact damage */
        bool lobbed = false;
        for (int i = 0; i < 120 && !lobbed; i++) {
            idle_ticks(1);
            for (int k = 0; k < MAX_PROJECTILES; k++)
                if (G.projectiles[k].active) lobbed = true;
        }
        EXPECT(lobbed, "the Riveter lobs a rivet on the interval quantum");
    }

    /* A rivet in flight costs Kilix a tier on contact. */
    {
        load_flat_arena(30);
        park_player(15);
        G.player.power_tier = 1;
        G.player.invuln = 0.0f;
        int tier0 = G.player.power_tier;
        Projectile *pr = &G.projectiles[0];
        memset(pr, 0, sizeof *pr);
        pr->active = true; pr->kind = PJ_RIVET;
        pr->x = G.player.x + 16.0f;        /* just off Kilix, at body height */
        pr->y = G.player.y + 5.0f;
        pr->vx = -RIVET_SPEED; pr->vy = 0.0f;
        pr->life = RIVET_LIFE; pr->facing = -1;
        for (int i = 0; i < 120 && G.player.power_tier == tier0; i++) idle_ticks(1);
        EXPECT(G.player.power_tier == tier0 - 1 && G.state == GS_PLAYING,
               "a rivet costs a plated tier on contact");
    }

    /* A rivet is blocked and despawns on a wall — well before its lifetime. */
    {
        load_empty_grid(30, PLAY_ROWS);
        for (int c = 0; c < 30; c++) G.vault_data.tiles[PLAY_ROWS - 1][c] = T_HULL;
        for (int rr = 0; rr < PLAY_ROWS; rr++) G.vault_data.tiles[rr][20] = T_HULL;
        park_player(2);
        G.player.invuln = 1.0e9f;
        Projectile *pr = &G.projectiles[0];
        memset(pr, 0, sizeof *pr);
        pr->active = true; pr->kind = PJ_RIVET;
        pr->x = (float)(18 * TILE_SIZE); pr->y = 88.0f;
        pr->vx = RIVET_SPEED; pr->vy = 0.0f;
        pr->life = RIVET_LIFE; pr->facing = 1;
        bool blocked = false;
        for (int i = 0; i < 60 && !blocked; i++) { idle_ticks(1); if (!pr->active) blocked = true; }
        EXPECT(blocked, "a rivet is blocked and despawns on a wall");
    }

    /* A rivet with no target expires on its lifetime. */
    {
        load_flat_arena(30);
        park_player(2);
        G.player.invuln = 1.0e9f;
        Projectile *pr = &G.projectiles[0];
        memset(pr, 0, sizeof *pr);
        pr->active = true; pr->kind = PJ_RIVET;
        pr->x = 120.0f; pr->y = 40.0f;
        pr->vx = 0.0f; pr->vy = 0.0f;
        pr->life = 0.12f; pr->facing = 1;
        idle_ticks(12);
        EXPECT(!pr->active, "a rivet expires on its lifetime");
    }

    /* Neither family embeds in geometry across a sustained run. */
    {
        load_flat_arena(40);
        G.scroll_lock = true;              /* keep both inside the despawn window */
        Enemy *maw = place_enemy(0, EN_MAW, 8, PLAY_ROWS - 2);
        maw->phase_q = MAW_HIDE_Q;
        Enemy *riv = place_enemy(1, EN_RIVETER, 18, PLAY_ROWS - 2);
        riv->phase_q = RIVETER_THROW_Q;
        park_player(13);                   /* between them: both active */
        G.player.invuln = 1.0e9f;
        bool clean = true;
        for (int i = 0; i < 300 && clean; i++) {
            idle_ticks(1);
            if (!game_validate(error, sizeof error)) clean = false;
        }
        EXPECT(clean, "the emerger and thrower never embed in geometry");
    }

    /* --- M4c: the phase-shell power-up ladder + Aegis + the Gate boss --- */

    /* A power block yields the NEXT tier by current state and never repeats a tier:
       Bare -> Plated -> Charged, then (at the top) scores instead of wasting. */
    {
        int outcome[3] = {0, 0, 0};
        bool scored_at_max = false;
        for (int start = 0; start < 3; start++) {
            load_empty_grid(20, PLAY_ROWS);
            for (int c = 0; c < 20; c++) G.vault_data.tiles[PLAY_ROWS - 1][c] = T_HULL;
            G.vault_data.tiles[8][8] = T_CACHE;              /* a ceiling ?-node */
            G.vault_data.caches[0] = (CacheNode){8, 8, (uint8_t)CN_POWER};
            G.vault_data.cache_count = 1;
            memset(&G.player, 0, sizeof G.player);
            G.player.facing = 1; G.player.buffer_tick = -1;
            G.player.x = 130.0f; G.player.y = 150.0f;        /* just below the node, rising */
            G.player.vy = -220.0f;
            G.player.power_tier = start;
            int score0 = G.score;
            for (int i = 0; i < 40 && G.vault_data.tiles[8][8] != T_SPENT; i++) idle_ticks(1);
            outcome[start] = G.player.power_tier;
            if (start == 2 && G.score > score0) scored_at_max = true;
        }
        EXPECT(outcome[0] == 1, "a power block promotes Bare -> Plated");
        EXPECT(outcome[1] == 2, "a power block promotes Plated -> Charged");
        EXPECT(outcome[2] == 2 && scored_at_max,
               "a power block at the top tier scores and never repeats a tier");
    }

    /* Aegis: temporary invuln survives contact (destroying the machine) and expires
       on the interval quantum. */
    {
        load_flat_arena(24);
        Enemy *e = place_enemy(0, EN_WALKER, 12, PLAY_ROWS - 2);
        park_player(11);
        G.player.x = e->x - 8.0f; G.player.prev_bottom = G.player.y + PLAYER_H;
        G.player.power_tier = 1;                 /* a hit, if any, would be detectable */
        G.player.invuln = 0.0f;
        G.player.aegis_q = AEGIS_Q;
        int tier0 = G.player.power_tier, lives0 = G.lives;
        bool defeated = false;
        for (int i = 0; i < 10; i++) {
            game_set_held_controls(true, false, true, false, false, false, false);
            game_tick();
            if (!e->active || (e->state & ES_SUBSTATE) == ES_SQUASHED) defeated = true;
        }
        EXPECT(G.player.power_tier == tier0 && G.lives == lives0 && G.state == GS_PLAYING,
               "Aegis invuln survives machine contact with no tier or life lost");
        EXPECT(defeated, "Aegis contact defeats the machine");
        int guard = 0;
        while (G.player.aegis_q > 0 && guard++ < 4000) idle_ticks(1);
        EXPECT(G.player.aegis_q == 0, "Aegis expires on the interval quantum");
    }

    /* Boss kill-path 1 — core overload: Phase Pulses on the exposed core crack the
       decoy shell (defeat + unmask). */
    {
        load_flat_arena(44);
        G.scroll_lock = true;
        Enemy *b = place_boss(0, EN_GUARDIAN, 24);
        park_player(16);                         /* to the boss's left, in pulse range */
        G.player.invuln = 1.0e9f;                /* isolate from the boss's own attacks */
        G.player.power_tier = 2;                 /* Charged: can emit Phase Pulses */
        G.player.facing = 1;
        G.guardian_down = false; G.guardian_unmasked = false;
        bool downed = false;
        for (int i = 0; i < 600 && !downed; i++) {
            b->state |= ES_EMERGED; b->emerge = 1.0f;   /* hold the hatch open */
            G.player.facing = 1;
            game_fire_pulse();
            idle_ticks(1);
            if (!b->active) downed = true;
        }
        EXPECT(downed && G.guardian_down && G.guardian_unmasked,
               "core-overload defeats the Guardian and unmasks the driver");
    }

    /* Boss kill-path 2 — seal switch: striking the seal collapses the dais without
       unmasking (the always-available route, no Charged state needed). */
    {
        load_flat_arena(44);
        G.vault_data.seal_col = 30; G.vault_data.seal_row = PLAY_ROWS - 2;
        G.vault_data.tiles[PLAY_ROWS - 2][30] = T_SEAL;
        Enemy *b = place_boss(0, EN_GUARDIAN, 20);
        park_player(30);                         /* standing on the seal */
        G.player.power_tier = 0;                 /* not Charged: seal path is unpowered */
        G.player.invuln = 1.0e9f;
        G.guardian_down = false; G.guardian_unmasked = false;
        idle_ticks(4);
        EXPECT(!b->active && G.guardian_down && !G.guardian_unmasked,
               "the seal switch collapses the boss without unmasking it");
    }

    /* The boss and its projectiles never embed in geometry over a sustained run. */
    {
        load_flat_arena(44);
        G.scroll_lock = true;
        place_boss(0, EN_GUARDIAN, 22);
        park_player(14);
        G.player.invuln = 1.0e9f;
        bool clean = true;
        for (int i = 0; i < 400 && clean; i++) {
            idle_ticks(1);
            if (!game_validate(error, sizeof error)) clean = false;
        }
        EXPECT(clean, "the Guardian and its plasma never embed in geometry");
    }

    /* The whole Gate path from the real level data: vault 1-4's schedule spawns the
       Guardian on its dais, and the autopilot's seal strike collapses it. */
    {
        game_load_level(3);                      /* 1-4: a Gate (Z-4) vault */
        bool clean = true, saw_boss = false;
        for (int i = 0; i < 5000 && clean; i++) {
            game_autopilot();
            game_tick();
            G.player.invuln = 1.0e9f;            /* survive the fight so the run continues */
            for (int s = 0; s < MAX_ACTIVE_ENEMIES; s++)
                if (G.enemies[s].active && G.enemies[s].kind == EN_GUARDIAN) saw_boss = true;
            if (!game_validate(error, sizeof error)) clean = false;
        }
        EXPECT(clean, "a real Gate vault validates through the boss encounter");
        EXPECT(saw_boss || G.guardian_down,
               "the Gate vault schedule spawns its Guardian and the seal collapses it");
    }

    load_flat_arena(20);
    park_player(4);
    EXPECT(game_validate(error, sizeof error), "post-fixture state validates");

    printf(failures ? "FAIL rules-test (%d)\n" : "PASS rules-test\n", failures);
    return failures ? 1 : 0;
}

static int input_test(void)
{
    failures = 0;
    headless_environment();
    game_init(512, 480, 42);
    G.headless = true; G.sound_on = false;
    game_start(0);
    settle_on_floor();

    /* --- held right moves and drives the visible gait --- */
    float before = G.player.x;
    for (int i = 0; i < 8; i++) {
        game_set_held_controls(true, false, true, false, false, false, false);
        game_tick();
    }
    EXPECT(G.player.x > before && G.player.gait_amount > 0.0f &&
           G.player.gait_phase > 0.0f,
           "held right produces movement and advances the gait");

    /* --- simultaneous left and right cancel --- */
    game_load_level(0);
    settle_on_floor();
    float v0 = G.player.vx;
    game_set_held_controls(true, true, true, false, false, false, false);
    game_tick();
    EXPECT(fabsf(G.player.vx) <= fabsf(v0) + 0.01f,
           "simultaneous left and right cancel acceleration");

    /* --- boost raises the top speed from the walk band to the run band --- */
    game_load_level(0);
    settle_on_floor();
    for (int i = 0; i < 60; i++) {
        game_set_held_controls(true, false, true, false, false, false, false);
        game_tick();
    }
    float walk_speed = G.player.vx;
    for (int i = 0; i < 60; i++) {
        game_set_held_controls(true, false, true, false, false, false, true);
        game_tick();
    }
    float run_speed = G.player.vx;
    EXPECT(walk_speed >= WALK_MAX - 1.0f && walk_speed <= WALK_MAX + 0.5f,
           "without boost the top speed plateaus at the walk cap");
    EXPECT(run_speed > WALK_MAX + 10.0f && run_speed <= RUN_MAX + 0.5f,
           "holding boost raises the top speed to the run band");

    /* --- press-only fallback moves, and the latch expires after 0.30 s --- */
    game_load_level(0);
    settle_on_floor();
    game_set_held_controls(false, false, false, false, false, false, false);
    game_handle_key('d');
    before = G.player.x;
    game_tick();
    EXPECT(G.player.x > before, "press-only fallback retains movement intent");
    for (int i = 0; i < 20; i++) game_tick();
    EXPECT(G.right_latch == 0.0f, "the press-only movement latch expires after 0.30 s");

    /* --- a held jump reaches a taller apex than a one-frame tap --- */
    game_load_level(0);
    settle_on_floor();
    game_set_held_controls(true, false, false, false, false, true, false);   /* jump edge */
    float held_apex = G.player.y;
    for (int i = 0; i < 90; i++) {
        game_set_held_controls(true, false, false, false, false, true, false);
        game_tick();
        if (G.player.y < held_apex) held_apex = G.player.y;
        if (i > 2 && G.player.grounded) break;
    }

    game_load_level(0);
    settle_on_floor();
    float tap_apex = G.player.y;
    game_set_held_controls(true, false, false, false, false, true, false);   /* one-frame tap */
    game_tick();
    if (G.player.y < tap_apex) tap_apex = G.player.y;
    for (int i = 0; i < 90; i++) {
        game_set_held_controls(true, false, false, false, false, false, false);
        game_tick();
        if (G.player.y < tap_apex) tap_apex = G.player.y;
        if (i > 2 && G.player.grounded) break;
    }
    EXPECT(held_apex < tap_apex - 4.0f,
           "a held jump reaches a taller apex than a one-frame tap");

    /* --- walk off a ledge, fall, and land (own shelf, independent of L0) --- */
    load_empty_grid(24, PLAY_ROWS);
    for (int c = 5; c <= 9; c++) G.vault_data.tiles[7][c] = T_HULL;  /* shelf */
    memset(&G.player, 0, sizeof G.player);
    G.player.facing = 1;
    G.player.buffer_tick = -1;
    G.player.x = (float)(7 * TILE_SIZE) + 2.0f;
    G.player.y = (float)(7 * TILE_SIZE) - PLAYER_H;   /* standing on the shelf */
    for (int i = 0; i < 5; i++) {
        game_set_held_controls(true, false, false, false, false, false, false);
        game_tick();
    }
    EXPECT(G.player.grounded, "the player rests on the shelf");
    float shelf_y = G.player.y;
    bool fell = false, landed = false;
    for (int i = 0; i < 120; i++) {
        game_set_held_controls(true, false, true, false, false, false, false);
        game_tick();
        if (!G.player.grounded) fell = true;
        if (fell && G.player.grounded) { landed = true; break; }
    }
    EXPECT(fell, "walking off the shelf enters a fall");
    EXPECT(landed && G.player.y > shelf_y + 8.0f,
           "the fall resolves by landing on the floor below");

    /* --- a nearby dormant machine telegraphs before it moves, and contact during
           the tell is nonlethal (the jpak dormant-enemy convention) --- */
    load_flat_arena(24);
    park_player(10);
    idle_ticks(3);                         /* settle at rest */
    {
        Enemy *e = &G.enemies[0];
        memset(e, 0, sizeof *e);
        e->active = true; e->kind = EN_WALKER; e->facing = -1; e->state = ES_WALK;
        e->x = G.player.x + 4.0f;
        e->y = G.player.y + (PLAYER_H - ENEMY_H);   /* feet aligned on the floor */
        e->home_x = e->x; e->home_y = e->y;
        float ex0 = e->x;
        int lives0 = G.lives, tier0 = G.player.power_tier;
        idle_ticks(24);
        EXPECT(e->alert > 0.0f && e->tell > 0.4f && e->tell < 1.0f,
               "a nearby machine telegraphs with an alert and a rising tell");
        EXPECT(fabsf(e->x - ex0) < 0.5f, "the machine holds position during its tell");
        EXPECT(G.lives == lives0 && G.player.power_tier == tier0 && G.state == GS_PLAYING,
               "contact during the tell is nonlethal");
    }

    printf(failures ? "FAIL input-test (%d)\n" : "PASS input-test\n", failures);
    return failures ? 1 : 0;
}

static bool ensure_directory(const char *path)
{
    return mkdir(path, 0755) == 0 || errno == EEXIST;
}

/* Render one scene to a PPM and prove the renderer never touched the sim: it
 * snapshots G, renders, and rejects any byte-difference afterward. */
static int write_scene(const char *directory, const char *name)
{
    char path[768];
    if (directory && *directory)
        snprintf(path, sizeof path, "%s/render_%s.ppm", directory, name);
    else snprintf(path, sizeof path, "render_%s.ppm", name);
    GameState before = G;
    render_frame();
    if (memcmp(&before, &G, sizeof G) != 0) {
        fprintf(stderr, "renderer mutated game state in scene %s\n", name);
        return 1;
    }
    if (!render_dump_ppm(path)) {
        fprintf(stderr, "cannot write %s\n", path);
        return 1;
    }
    printf("wrote %s\n", path);
    return 0;
}

static int render_test(uint32_t seed)
{
    headless_environment();
    const char *directory = getenv("SUPER_KILIX_RENDER_DIR");
    if (directory && *directory && !ensure_directory(directory)) {
        fprintf(stderr, "cannot create render directory: %s\n", directory);
        return 1;
    }
    /* A 2x-scaled 512x480 screen (256x240 logical) is the base size; the two
     * resize scenes render the same composition at other sizes to prove the
     * pipeline reallocates cleanly. */
    game_init(512, 480, seed);
    G.headless = true;
    G.sound_on = false;
    if (!render_init(G.W, G.H)) return 1;
    int failed = 0, images = 0;
    int baseline_y = (PLAY_ROWS - 1) * TILE;

    G.state = GS_TITLE;
    failed |= write_scene(directory, "title"); images++;

    /* Kilix on the authored district-1 vault: idle, then the two opposed walk
     * strides whose differing pixels gate the gait port's correctness. */
    game_start(0);
    G.player.grounded = true;
    G.player.gait_amount = 0.0f;
    G.player.gait_phase = 0.0f;
    failed |= write_scene(directory, "kilix_idle"); images++;

    G.player.gait_amount = 1.0f;
    G.player.gait_phase = 1.5707963f;
    failed |= write_scene(directory, "walk_stride_a"); images++;
    G.player.gait_phase = 4.7123890f;
    failed |= write_scene(directory, "walk_stride_b"); images++;

    /* A mid-vault frame with the camera scrolled to the escalating conduits. */
    {
        float cam_limit = (float)(G.vault_data.cols * TILE - LOGICAL_W);
        if (cam_limit < 0.0f) cam_limit = 0.0f;
        G.player.x = (float)(40 * TILE);
        G.player.y = (float)(baseline_y) - PLAYER_H;
        G.player.gait_amount = 1.0f;
        G.player.gait_phase = 1.5707963f;
        G.cam_x = G.player.x - CAM_DEADZONE;
        if (G.cam_x < 0.0f) G.cam_x = 0.0f;
        if (G.cam_x > cam_limit) G.cam_x = cam_limit;
        G.cam_x_max = G.cam_x;
        failed |= write_scene(directory, "level_region1"); images++;
    }

    /* The exit: camera at the riser / iris end of the vault. */
    {
        float cam_limit = (float)(G.vault_data.cols * TILE - LOGICAL_W);
        if (cam_limit < 0.0f) cam_limit = 0.0f;
        G.player.x = (float)((G.vault_data.riser_col - 2) * TILE);
        G.player.y = (float)(baseline_y) - PLAYER_H;
        G.player.gait_amount = 0.4f;
        G.cam_x = G.player.x - CAM_DEADZONE;
        if (G.cam_x < 0.0f) G.cam_x = 0.0f;
        if (G.cam_x > cam_limit) G.cam_x = cam_limit;
        G.cam_x_max = G.cam_x;
        failed |= write_scene(directory, "level_exit"); images++;
    }

    /* The two M4a machine families + the Husk + the nonlethal activation tell,
     * placed on the RUST FLATS floor in front of a camera-parked Kilix. */
    {
        game_start(0);
        G.player.x = (float)(31 * TILE);
        G.player.y = (float)(baseline_y) - PLAYER_H;
        G.player.grounded = true;
        G.cam_x = G.cam_x_max = G.player.x - CAM_DEADZONE;
        if (G.cam_x < 0.0f) G.cam_x = 0.0f;
        memset(G.enemies, 0, sizeof G.enemies);
        Enemy *e = &G.enemies[0];
        e->active = true; e->kind = EN_WALKER; e->facing = -1;
        e->state = ES_WALK; e->alert = 1.0f; e->tell = 1.0f;
        e->x = (float)(35 * TILE); e->y = (float)baseline_y - ENEMY_H;
        e->home_x = e->x;
        failed |= write_scene(directory, "enemy_treader"); images++;

        e->kind = EN_TURNER;
        failed |= write_scene(directory, "enemy_carapod"); images++;

        e->state = (uint8_t)(ES_HUSK | ES_SHELL_MOV); e->facing = 1;
        failed |= write_scene(directory, "enemy_husk"); images++;

        e->state = ES_WALK; e->kind = EN_WALKER; e->alert = 1.0f; e->tell = 0.5f;
        failed |= write_scene(directory, "enemy_tell"); images++;

        /* The M4b emerger, the ranged thrower, and a rivet mid-lob. */
        e->kind = EN_MAW; e->state = ES_WALK; e->alert = 1.0f; e->tell = 1.0f;
        e->x = (float)(35 * TILE);
        e->home_y = (float)baseline_y - ENEMY_H;
        e->emerge = 0.85f;
        e->y = e->home_y - e->emerge * MAW_RISE;
        failed |= write_scene(directory, "enemy_ventmaw"); images++;

        e->kind = EN_RIVETER; e->state = ES_WALK; e->alert = 1.0f; e->tell = 1.0f;
        e->emerge = 0.0f;
        e->y = (float)baseline_y - ENEMY_H;
        e->home_x = e->x;
        failed |= write_scene(directory, "enemy_riveter"); images++;

        memset(&G.projectiles[0], 0, sizeof G.projectiles[0]);
        G.projectiles[0].active = true;
        G.projectiles[0].kind = PJ_RIVET;
        G.projectiles[0].x = (float)(33 * TILE);
        G.projectiles[0].y = (float)baseline_y - 20.0f;
        G.projectiles[0].vx = -RIVET_SPEED;
        G.projectiles[0].vy = -40.0f;
        G.projectiles[0].facing = -1;
        G.projectiles[0].life = RIVET_LIFE;
        failed |= write_scene(directory, "enemy_rivet"); images++;
    }

    /* The Gate boss (Vault Guardian) mid-fight: hatch open on its core-tell, a
     * plasma bolt in flight, and a Charged Kilix closing in. */
    {
        game_start(0);
        memset(G.enemies, 0, sizeof G.enemies);
        memset(G.projectiles, 0, sizeof G.projectiles);
        G.player.x = (float)(31 * TILE);
        G.player.y = (float)(baseline_y) - PLAYER_H;
        G.player.grounded = true;
        G.player.power_tier = 2;
        G.cam_x = G.cam_x_max = G.player.x - CAM_DEADZONE;
        if (G.cam_x < 0.0f) G.cam_x = 0.0f;
        Enemy *g = &G.enemies[0];
        g->active = true; g->kind = EN_GUARDIAN; g->facing = -1;
        g->state = (uint8_t)(ES_WALK | ES_EMERGED);
        g->alert = 1.0f; g->tell = 1.0f; g->emerge = 1.0f;
        g->revive_q = GUARDIAN_CORE_HP; g->phase_q = GUARDIAN_OPEN_Q;
        g->x = (float)(37 * TILE); g->y = (float)baseline_y - GUARDIAN_H;
        g->home_x = g->x; g->home_y = g->y;
        G.projectiles[0].active = true; G.projectiles[0].kind = PJ_PLASMA;
        G.projectiles[0].x = (float)(34 * TILE); G.projectiles[0].y = (float)baseline_y - 24.0f;
        G.projectiles[0].vx = -PLASMA_SPEED; G.projectiles[0].facing = -1;
        G.projectiles[0].life = PLASMA_LIFE;
        failed |= write_scene(directory, "boss_guardian"); images++;
    }

    /* The phase-shell power-up ladder: Plated, Charged, and the Aegis invuln halo. */
    {
        game_start(0);
        memset(G.enemies, 0, sizeof G.enemies);
        memset(G.projectiles, 0, sizeof G.projectiles);
        G.player.x = (float)(6 * TILE);
        G.player.y = (float)(baseline_y) - PLAYER_H;
        G.player.grounded = true;
        G.player.gait_amount = 0.0f; G.player.gait_phase = 0.0f;
        G.cam_x = G.cam_x_max = 0.0f;

        G.player.power_tier = 1; G.player.aegis_q = 0;
        failed |= write_scene(directory, "powerup_plated"); images++;

        G.player.power_tier = 2;
        failed |= write_scene(directory, "powerup_charged"); images++;

        G.player.aegis_q = AEGIS_Q;
        failed |= write_scene(directory, "powerup_aegis"); images++;
    }

    if (!render_resize(320, 300)) { render_shutdown(); game_shutdown(); return 1; }
    failed |= write_scene(directory, "resize_small"); images++;
    if (!render_resize(1024, 960)) { render_shutdown(); game_shutdown(); return 1; }
    failed |= write_scene(directory, "resize_large"); images++;

    render_shutdown();
    game_shutdown();
    if (!failed) printf("PASS render-test seed=%u images=%d\n", seed, images);
    return failed ? 1 : 0;
}

static int sound_test(void)
{
    /* Audio is optional and never fatal: --sound-test returns 0 with no sink. */
    if (!sound_init()) {
        printf("sound-test: no audio sink; silent fallback is operational\n");
        return 0;
    }
    sound_shutdown();
    printf("PASS sound-test\n");
    return 0;
}

/* One annotated grid cell: the entrance, a scheduled machine, or the tile. */
static char dump_cell(const VaultData *v, int col, int row)
{
    if (col == v->spawn_col && row == v->spawn_row) return '@';
    for (int i = 0; i < v->enemy_count; i++)
        if (v->enemies[i].col == col && v->enemies[i].row == row) return '!';
    switch (v->tiles[row][col]) {
    case T_EMPTY:     return ' ';
    case T_HULL:
    case T_HULL_DARK: return '#';
    case T_BEDROCK:   return 'B';
    case T_BRICK:     return 'x';
    case T_CACHE:     return '?';
    case T_SPENT:     return 'o';
    case T_CONDUIT:   return 'C';
    case T_LEDGE:     return '=';
    case T_RISER:     return 'R';
    case T_IRIS:      return 'I';
    case T_THORN:     return '^';
    case T_SEAL:      return '&';
    default:          return '+';
    }
}

static int dump_level(int one_based)
{
    if (one_based < 1 || one_based > CAMPAIGN_VAULTS) {
        fprintf(stderr, "--dump-level needs 1..%d\n", CAMPAIGN_VAULTS);
        return 2;
    }
    VaultData v;
    level_build(one_based - 1, &v);
    char error[192];
    bool ok = level_validate(one_based - 1, error, sizeof error);
    printf("vault %d  %s  cols=%d rows=%d  spawn=%d,%d riser=%d,%d iris=%d,%d  "
           "machines=%d charge=%u sig=%08x  %s\n",
           one_based, v.title, v.cols, v.rows, v.spawn_col, v.spawn_row,
           v.riser_col, v.riser_row, v.exit_col, v.exit_row, v.enemy_count,
           (unsigned)v.timer_start, level_signature(&v),
           ok ? "[valid]" : error);
    for (int row = 0; row < v.rows; row++) {
        for (int col = 0; col < v.cols; col++) putchar(dump_cell(&v, col, row));
        putchar('\n');
    }
    printf("@ entry  R riser  & seal  I iris  # terrace  B bedrock  x scrap  ? cache  "
           "o spent  C conduit  = grate  ^ thorns  ! machine\n");
    return 0;
}

static int usage(void)
{
    printf("super-kilix %s\n"
           "usage: super-kilix [--level N] [--selftest [seed] [ticks]]\n"
           "                   [--rules-test] [--input-test]\n"
           "                   [--render-test [seed]] [--sound-test]\n"
           "                   [--dump-level N] [--version] [--help]\n\n"
           "Run without arguments in a Kitty-protocol terminal to play.\n",
           SK_VERSION);
    return 0;
}

/* ---------------------------------------------------------------- play loop */

static int play(int forced_level)
{
    int width = 0, height = 0;
    if (!term_init(&width, &height)) {
        fprintf(stderr, "super-kilix needs an interactive Kitty-protocol terminal\n");
        fprintf(stderr, "use --selftest or --render-test for headless operation\n");
        return 1;
    }
    install_signals();
    atexit(term_shutdown);
    game_init(width, height, (uint32_t)time(NULL));
    if (!render_init(width, height)) { term_shutdown(); return 1; }
    bool audio_available = sound_init();
    sound_set_enabled(audio_available && G.sound_on);
    game_start(forced_level >= 0 ? forced_level : 0);   /* real level select at M6 */

    /* The family's canonical 60 Hz fixed-step clock (kilix_game_loop.h): each
     * frame yields a bounded number of sim steps, then we present once. */
    kilix_game_clock_options options;
    kilix_game_clock_options_init(&options);
    options.step_ns = KILIX_GAME_NANOSECONDS_PER_SECOND / 60;
    kilix_game_clock clock;
    kilix_game_clock_init(&clock, &options);
    kilix_game_clock_reset(&clock, kilix_game_monotonic_ns());

    while (!G.quit) {
        if (term_read_input() < 0) { G.quit = true; break; }

        bool held = term_has_release_events();
        kittykb_event event;
        while (term_next_key_event(&event)) {
            if (event.action != KITTYKB_ACTION_PRESS) continue;
            if (interrupt_event(&event)) { G.quit = true; continue; }
            int key = game_key(&event);
            if (key < 0) continue;
            if (held && G.state == GS_PLAYING && continuous_key(key)) continue;
            game_handle_key(key);
        }
        if (G.quit) break;
        game_set_held_controls(
            held,
            term_key_down('a') || term_key_down('A') || term_key_down(KITTYKB_KEY_LEFT),
            term_key_down('d') || term_key_down('D') || term_key_down(KITTYKB_KEY_RIGHT),
            term_key_down('w') || term_key_down('W') || term_key_down(KITTYKB_KEY_UP),
            term_key_down('s') || term_key_down('S') || term_key_down(KITTYKB_KEY_DOWN),
            term_key_down(' ') || term_key_down('z') || term_key_down('Z') ||
                term_key_down('w') || term_key_down('W') || term_key_down(KITTYKB_KEY_UP),
            term_key_down(KITTYKB_KEY_LEFT_SHIFT) || term_key_down(KITTYKB_KEY_RIGHT_SHIFT) ||
                term_key_down('k') || term_key_down('K'));

        int new_width, new_height;
        if (term_check_resize(&new_width, &new_height) &&
            (new_width != G.W || new_height != G.H)) {
            G.W = new_width; G.H = new_height;
            if (!render_resize(new_width, new_height)) { G.quit = true; break; }
        }
        kilix_game_frame frame = kilix_game_clock_advance(&clock,
                                                          kilix_game_monotonic_ns());
        for (uint32_t step = 0; step < frame.steps; step++)
            game_tick();
        render_frame();
        term_present(render_fb(), G.W, G.H);
    }
    game_shutdown();
    sound_shutdown();
    render_shutdown();
    term_shutdown();
    return 0;
}

/* --------------------------------------------------------------------- main */

int main(int argc, char **argv)
{
    if (argc > 1 && !strcmp(argv[1], "--selftest")) {
        if (argc > 4)
            return option_arity_error("--selftest", "accepts only [seed] [ticks]");
        uint32_t seed = 1337u;
        int ticks = 12000;
        if (argc > 2 && !parse_u32_argument(argv[2], &seed)) {
            fprintf(stderr, "selftest seed must be an unsigned 32-bit integer\n");
            return 2;
        }
        if (argc > 3 && !parse_int_argument(argv[3], 1, INT_MAX, &ticks)) {
            fprintf(stderr, "selftest ticks must be an integer in 1..%d\n", INT_MAX);
            return 2;
        }
        return selftest(seed, ticks);
    }
    if (argc > 1 && !strcmp(argv[1], "--rules-test")) {
        if (argc != 2) return option_arity_error("--rules-test", "takes no arguments");
        return rules_test();
    }
    if (argc > 1 && !strcmp(argv[1], "--input-test")) {
        if (argc != 2) return option_arity_error("--input-test", "takes no arguments");
        return input_test();
    }
    if (argc > 1 && !strcmp(argv[1], "--render-test")) {
        if (argc > 3)
            return option_arity_error("--render-test", "accepts only [seed]");
        uint32_t seed = 42u;
        if (argc > 2 && !parse_u32_argument(argv[2], &seed)) {
            fprintf(stderr, "render-test seed must be an unsigned 32-bit integer\n");
            return 2;
        }
        return render_test(seed);
    }
    if (argc > 1 && !strcmp(argv[1], "--sound-test")) {
        if (argc != 2) return option_arity_error("--sound-test", "takes no arguments");
        return sound_test();
    }
    if (argc > 1 && !strcmp(argv[1], "--dump-level")) {
        if (argc != 3)
            return option_arity_error("--dump-level", "needs exactly one level");
        int level;
        if (!parse_int_argument(argv[2], 1, DUMP_LEVEL_MAX, &level)) {
            fprintf(stderr, "dump level must be an integer in 1..%d\n", DUMP_LEVEL_MAX);
            return 2;
        }
        return dump_level(level);
    }
    if (argc > 1 && !strcmp(argv[1], "--version")) {
        if (argc != 2) return option_arity_error("--version", "takes no arguments");
        printf("super-kilix %s\n", SK_VERSION);
        return 0;
    }
    if (argc > 1 && (!strcmp(argv[1], "--help") || !strcmp(argv[1], "-h"))) {
        if (argc != 2) return option_arity_error(argv[1], "takes no arguments");
        return usage();
    }
    if (argc > 1 && !strcmp(argv[1], "--level")) {
        if (argc != 3)
            return option_arity_error("--level", "needs exactly one level");
        int level;
        if (!parse_int_argument(argv[2], 1, DUMP_LEVEL_MAX, &level)) {
            fprintf(stderr, "level must be an integer in 1..%d\n", DUMP_LEVEL_MAX);
            return 2;
        }
        return play(level - 1);
    }
    if (argc > 1) { fprintf(stderr, "unknown option: %s\n", argv[1]); usage(); return 2; }
    return play(-1);
}

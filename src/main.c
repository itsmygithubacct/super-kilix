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

/* Replace the loaded vault with an empty grid of the given extent.  There is NO
 * baseline floor: below-the-floor is a void the PLAYER falls through (a fixture that
 * needs solid ground either lays its own floor or uses load_flat_arena).  The machine
 * field and its schedule are cleared so no stray spawn leaks into a physics fixture. */
static void load_empty_grid(int cols, int rows)
{
    memset(&G.vault_data, 0, sizeof G.vault_data);
    memset(G.enemies, 0, sizeof G.enemies);
    memset(G.projectiles, 0, sizeof G.projectiles);
    memset(G.pickups, 0, sizeof G.pickups);
    G.spawn_cursor = 0;
    G.hitstop = 0;          /* no freeze carried in from a prior fixture's impact (M7) */
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
    load_flat_arena(20);                        /* real floor: void gaps now fall through */
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
    load_flat_arena(80);
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

    load_flat_arena(80);
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
    load_flat_arena(20);
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
        bool cleared = false;
        for (int i = 0; i < 4000 && !cleared; i++) {
            game_autopilot();
            game_tick();
            if (G.state == GS_VAULT_CLEAR) cleared = true;
        }
        EXPECT(cleared,
               "autopilot traverses FIRST TERRACE and grabs the riser to clear it");
    }

    /* --- M6 campaign shape: distinct route keys, adjacent same-slot structural
           difference, per-vault budgets, and the Gate/riser goal split --- */
    {
        static VaultData vv[CAMPAIGN_VAULTS];
        uint32_t rk[CAMPAIGN_VAULTS];
        for (int i = 0; i < CAMPAIGN_VAULTS; i++) {
            level_build(i, &vv[i]);
            rk[i] = level_route_key(&vv[i]);
        }
        int distinct = 0;
        for (int i = 0; i < CAMPAIGN_VAULTS; i++) {
            bool seen = false;
            for (int j = 0; j < i; j++) if (rk[j] == rk[i]) seen = true;
            if (!seen) distinct++;
        }
        EXPECT(distinct >= 20,
               "the campaign has many distinct entry->exit route keys");

        bool same_slot_differ = true;
        for (int d = 0; d + 1 < DISTRICTS; d++)
            for (int s = 0; s < VAULTS_PER_DISTRICT; s++) {
                int a = d * VAULTS_PER_DISTRICT + s;
                int b = (d + 1) * VAULTS_PER_DISTRICT + s;
                if (level_structural_diff(&vv[a], &vv[b]) < 24) same_slot_differ = false;
            }
        EXPECT(same_slot_differ,
               "adjacent same-slot vaults differ by a real structural margin");

        bool budgets_ok = true, goals_ok = true;
        for (int i = 0; i < CAMPAIGN_VAULTS; i++) {
            if (vv[i].enemy_count < 0 || vv[i].enemy_count > level_enemy_budget(i))
                budgets_ok = false;
            bool gate = level_is_gate(i);
            if (gate  && vv[i].seal_col  < 0) goals_ok = false;
            if (!gate && vv[i].riser_col < 0) goals_ok = false;
        }
        EXPECT(budgets_ok, "every vault's machine budget is within range");
        EXPECT(goals_ok, "Gate vaults end on a seal switch, the rest on a scored riser");
    }

    /* --- M6 scoring: the doubling chain escalates through the table to the
           EXTRA-UNIT sentinel with the CORRECTED shape (the spare unit lands on
           the 8th chained defeat, not the 7th) --- */
    {
        load_flat_arena(80);
        G.scroll_lock = true;
        G.score = 0;
        G.chain = 0;
        G.lives = 3;
        G.next_extra_life = 1 << 30;         /* isolate the chain from threshold lives */
        int deltas[CHAIN_MAX];
        int life_gain[CHAIN_MAX];
        for (int k = 0; k < CHAIN_MAX; k++) {
            Enemy *e = place_enemy(0, EN_WALKER, 24, PLAY_ROWS - 2);
            memset(&G.player, 0, sizeof G.player);
            G.player.facing = 1;
            G.player.buffer_tick = -1;
            G.player.x = e->x;
            G.player.y = e->y - PLAYER_H + 2.0f;       /* already overlapping the top */
            G.player.vy = 40.0f;                       /* descending -> a stomp */
            G.player.grounded = false;                 /* airborne: the chain never resets */
            G.player.invuln = 1.0e9f;                  /* never a stray side-hit */
            /* Force this tick's collision parity to the player-vs-machine pass so the
             * stomp lands deterministically (game_tick runs it on an even tick). */
            G.tick |= 1u;
            G.hitstop = 0;    /* isolate each stomp from the prior tick's impact freeze */
            int s0 = G.score, l0 = G.lives;
            game_set_held_controls(true, false, false, false, false, false, false);
            game_tick();
            deltas[k] = G.score - s0;
            life_gain[k] = G.lives - l0;
        }
        bool escalates = true;
        for (int k = 0; k < CHAIN_MAX - 1; k++)            /* rungs 1..7 double */
            if (deltas[k] != (100 << k)) escalates = false;
        EXPECT(escalates,
               "the airborne stomp chain doubles 100->6400 through the table");
        bool no_early_life = true;
        for (int k = 0; k < CHAIN_MAX - 1; k++)
            if (life_gain[k] != 0) no_early_life = false;
        EXPECT(no_early_life && deltas[CHAIN_MAX - 1] == 0 &&
               life_gain[CHAIN_MAX - 1] == 1,
               "the EXTRA UNIT lands on the 8th chained defeat, not the 7th");
    }

    /* --- M6 third chain counter: the per-stomp decay timer lapses an airborne
           chain if Kilix hovers too long between stomps (so the thruster cannot
           bank an indefinite chain) --- */
    {
        load_flat_arena(40);
        G.scroll_lock = true;
        G.chain = 0;
        G.score = 0;
        G.next_extra_life = 1 << 30;
        Enemy *e = place_enemy(0, EN_WALKER, 20, PLAY_ROWS - 2);
        memset(&G.player, 0, sizeof G.player);
        G.player.facing = 1;
        G.player.buffer_tick = -1;
        G.player.x = e->x;
        G.player.y = e->y - PLAYER_H + 2.0f;
        G.player.vy = 40.0f;
        G.player.grounded = false;
        G.player.invuln = 1.0e9f;
        G.tick |= 1u;
        game_set_held_controls(true, false, false, false, false, false, false);
        game_tick();                                    /* one airborne stomp opens the chain */
        EXPECT(G.chain == 1, "an airborne stomp opens the chain");
        G.hitstop = 0;                                  /* skip the stomp's freeze; test the decay */
        for (int i = 0; i < CHAIN_DECAY + 5; i++) {      /* hover airborne, no more stomps */
            G.player.y = 60.0f;
            G.player.vy = 0.0f;
            G.player.grounded = false;
            game_set_held_controls(true, false, false, false, false, false, false);
            game_tick();
        }
        EXPECT(G.chain == 0,
               "the airborne chain lapses on the per-stomp decay timer");
    }

    /* --- M6 fix: the sliding-Husk mow chain uses its OWN per-Husk counter (reset
           when the Husk stops), independent of G.chain's land-to-reset — so a Husk
           mowing a line of machines while Kilix stands GROUNDED still escalates
           (the buggy land-to-reset would score a flat 100 per kill). --- */
    {
        load_flat_arena(60);
        G.scroll_lock = true;
        G.chain = 0;
        G.score = 0;
        G.lives = 5;
        G.next_extra_life = 1 << 30;
        memset(G.enemies, 0, sizeof G.enemies);
        Enemy *husk = place_enemy(0, EN_TURNER, 12, PLAY_ROWS - 2);
        husk->state = (uint8_t)((husk->state & ~ES_SUBSTATE) | ES_HUSK);  /* dormant Husk */
        husk->state &= (uint8_t)~ES_SHELL_MOV;
        husk->vx = 0.0f;
        husk->revive_q = HUSK_REVIVE_Q;
        for (int k = 1; k <= 3; k++) {                  /* a line of victims downrange */
            Enemy *v = place_enemy(k, EN_WALKER, 15 + (k - 1) * 2, PLAY_ROWS - 2);
            v->facing = -1;                              /* walk toward the Husk's path,
                                                            not off the right cull edge */
        }
        park_player(11);
        G.player.x = husk->x - 6.0f;                     /* touch the Husk from the left */
        G.player.prev_bottom = G.player.y + PLAYER_H;
        G.player.invuln = 1.0e9f;
        int mowed = 0;
        for (int i = 0; i < 240; i++) {
            game_set_held_controls(true, false, false, false, false, false, false);
            game_tick();
            mowed = 0;                                   /* count only real mows (squashed) */
            for (int k = 1; k <= 3; k++)
                if (G.enemies[k].active &&
                    (G.enemies[k].state & ES_SUBSTATE) == ES_SQUASHED) mowed++;
            if (mowed == 3) break;
        }
        EXPECT(mowed == 3 && G.score >= 1200,
               "the sliding-Husk mow chain escalates while Kilix stands grounded");
    }

    /* --- M6 exit reward: leftover charge converts on the exact steep top-heavy
           band ladder (200,500,1000,2500,6000) plus a 50/unit floor --- */
    {
        static const int ladder[5] = { 200, 500, 1000, 2500, 6000 };
        bool topheavy = true;
        for (int b = 1; b < 5; b++)
            if (ladder[b] < ladder[b - 1] * 2) topheavy = false;
        bool monotone = true, ladder_ok = true;
        int prev_band = -1, prev_score = -1;
        for (int left = 0; left <= 400; left += 20) {
            int band  = game_exit_band(left, 400);
            int score = game_exit_score(left, 400);
            if (band < prev_band) monotone = false;
            if (left > 0 && score <= prev_score) monotone = false;
            if (band < 0 || band > 4 || score - 50 * left != ladder[band]) ladder_ok = false;
            prev_band = band;
            prev_score = score;
        }
        EXPECT(game_exit_band(0, 400) == 0 && game_exit_band(400, 400) == 4,
               "leftover charge maps across the full exit-band range");
        EXPECT(ladder_ok,
               "the exit converts leftover charge on the exact top-heavy band ladder");
        EXPECT(topheavy && monotone,
               "the exit curve is steep, top-heavy, and monotone in leftover charge");
    }

    /* --- M6 extra life at a score threshold, granted once --- */
    {
        load_empty_grid(20, PLAY_ROWS);
        for (int c = 0; c < 20; c++) G.vault_data.tiles[PLAY_ROWS - 1][c] = T_HULL;
        G.vault_data.tiles[8][8] = T_CACHE;             /* a ceiling mote node */
        G.vault_data.caches[0] = (CacheNode){ 8, 8, (uint8_t)CN_MOTE };
        G.vault_data.cache_count = 1;
        G.score = EXTRA_LIFE_STEP - 50;                 /* one mote (100) crosses it */
        G.next_extra_life = EXTRA_LIFE_STEP;
        G.lives = 3;
        G.motes = 0;
        int lives0 = G.lives;
        memset(&G.player, 0, sizeof G.player);
        G.player.facing = 1;
        G.player.buffer_tick = -1;
        G.player.x = 130.0f;                            /* under col 8, rising into it */
        G.player.y = 150.0f;
        G.player.vy = -220.0f;
        /* Bonk spends the cache and EJECTS a coin pop; the mote (and its threshold
         * life) is credited only when the pop auto-collects at its apex a few ticks
         * later — so wait for the spare unit rather than the spent block. */
        for (int i = 0; i < 60 && G.lives == lives0; i++) idle_ticks(1);
        EXPECT(G.vault_data.tiles[8][8] == T_SPENT && G.lives == lives0 + 1,
               "a spare unit is granted once the score crosses a threshold");
        EXPECT(G.next_extra_life == EXTRA_LIFE_STEP * 2,
               "the threshold advances so the same boundary never grants twice");
    }

    /* --- Dispensed pickups (cast.md §4/§4.3): a bonked coin cache POPS a mote that
           auto-credits exactly one mote + SCORE_MOTE at its apex (no touch), and the
           payload is NOT applied on the bonk tick itself --- */
    {
        load_empty_grid(20, PLAY_ROWS);
        for (int c = 0; c < 20; c++) G.vault_data.tiles[PLAY_ROWS - 1][c] = T_HULL;
        G.vault_data.tiles[8][8] = T_CACHE;
        G.vault_data.caches[0] = (CacheNode){ 8, 8, (uint8_t)CN_MOTE };
        G.vault_data.cache_count = 1;
        G.score = 0; G.motes = 0; G.lives = 3;
        G.next_extra_life = 1 << 30;                    /* isolate from threshold lives */
        memset(&G.player, 0, sizeof G.player);
        G.player.facing = 1; G.player.buffer_tick = -1;
        G.player.x = 130.0f; G.player.y = 150.0f; G.player.vy = -220.0f;
        for (int i = 0; i < 40 && G.vault_data.tiles[8][8] != T_SPENT; i++) idle_ticks(1);
        EXPECT(G.vault_data.tiles[8][8] == T_SPENT && G.motes == 0 && G.score == 0,
               "a bonked coin cache spends the block but the mote is not yet credited");
        for (int i = 0; i < 40 && G.motes == 0; i++) idle_ticks(1);
        EXPECT(G.motes == 1 && G.score == SCORE_MOTE,
               "the popped coin auto-credits exactly one mote + SCORE_MOTE at its apex");
    }

    /* --- M6 profile round-trip via kilix-state, in a private mkdtemp dir --- */
    {
        char dir[] = "/tmp/sk_prof_XXXXXX";
        char *made = mkdtemp(dir);
        if (made) setenv("SUPER_KILIX_DATA_HOME", made, 1);
        unsetenv("SUPER_KILIX_NO_PROFILE");

        game_init(0, 0, 1);
        G.high_score = 123456; G.unlock_district = 5; G.sound_on = false;
        G.practice_mode = false;
        bool wrote = game_profile_save();
        EXPECT(made && wrote, "the profile saves to a private data dir");

        char path[1024];
        struct stat st;
        bool mode_ok = false;
        if (game_profile_path(path, sizeof path) && stat(path, &st) == 0)
            mode_ok = (st.st_mode & 07777) == 0600;
        EXPECT(mode_ok, "the profile file mode is exactly 0600");

        game_init(0, 0, 1);                    /* fresh defaults, then auto-load */
        EXPECT(G.high_score == 123456 && G.unlock_district == 5 && !G.sound_on,
               "a reload restores high score, unlock, and the sound flag");

        FILE *f = fopen(path, "wb");
        if (f) { fputc('S', f); fclose(f); }   /* truncate to a stub */
        game_init(0, 0, 1);
        bool loaded_trunc = game_profile_load();
        EXPECT(!loaded_trunc && G.high_score == 0 && G.unlock_district == 1,
               "a truncated profile is rejected with zero partial state");

        EXPECT(game_profile_write(SK_PROFILE_MAGIC, SK_PROFILE_SCHEMA + 1u, 999999, 8, 1),
               "a newer-schema profile can be written with a valid checksum");
        game_init(0, 0, 1);
        bool loaded_new = game_profile_load();
        EXPECT(!loaded_new && G.high_score == 0,
               "a newer-schema profile is ignored without partial application");

        game_profile_write(SK_PROFILE_MAGIC, SK_PROFILE_SCHEMA, 111, 3, 1);
        game_init(0, 0, 1);                    /* auto-loads the 111/3 baseline */
        G.practice_mode = true;
        G.high_score = 888888; G.unlock_district = 8;
        bool practice_wrote = game_profile_save();
        game_init(0, 0, 1);                    /* reload: the baseline must be intact */
        EXPECT(!practice_wrote && G.high_score == 111 && G.unlock_district == 3,
               "practice mode never mutates the profile");

        unlink(path);
        if (made) rmdir(made);
        setenv("SUPER_KILIX_NO_PROFILE", "1", 1);   /* re-disable for the rest of the suite */
        game_init(512, 480, 7);                     /* restore the suite's game state */
        G.headless = true; G.sound_on = false;
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

    /* A power block EMERGES an item (never applied on the bonk); collecting it yields
       the NEXT tier by current state and never repeats a tier: Bare -> Plated ->
       Charged, then (at the top) scores instead of wasting.  The tier must change ONLY
       on the touch-collect, so the fixture bonks, lets the item emerge, then forces
       Kilix onto the emerged item to collect it. */
    {
        int outcome[3] = {0, 0, 0};
        int on_bonk[3] = {9, 9, 9};
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
            /* Bonk: the block spends but the tier must be UNCHANGED (item only emerged). */
            for (int i = 0; i < 40 && G.vault_data.tiles[8][8] != T_SPENT; i++) idle_ticks(1);
            on_bonk[start] = G.player.power_tier;
            Pickup *pk = NULL;
            for (int k = 0; k < MAX_PICKUPS; k++) if (G.pickups[k].active) pk = &G.pickups[k];
            /* Let it emerge, then plant Kilix on the emerged item so the touch-collect
             * fires; the effect is applied exactly once, on collect. */
            for (int i = 0; i < 160 && pk && pk->active; i++) {
                G.player.invuln = 1.0e9f;
                G.player.vx = G.player.vy = 0.0f;
                G.player.grounded = true;
                G.player.x = pk->x + (PICKUP_W - PLAYER_W) * 0.5f;
                G.player.y = pk->spawn_y - PLAYER_H;         /* stand on the block, over the item */
                idle_ticks(1);
            }
            outcome[start] = G.player.power_tier;
            if (start == 2 && G.score > score0) scored_at_max = true;
        }
        EXPECT(on_bonk[0] == 0 && on_bonk[1] == 1 && on_bonk[2] == 2,
               "a power block grants NO tier on the bonk (only the item emerges)");
        EXPECT(outcome[0] == 1, "collecting the emerged item promotes Bare -> Plated");
        EXPECT(outcome[1] == 2, "collecting the emerged item promotes Plated -> Charged");
        EXPECT(outcome[2] == 2 && scored_at_max,
               "a power block at the top tier scores on collect and never repeats a tier");
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

    /* --- M7 CORNER_NUDGE: a rising jump that only just clips a ceiling corner slips
           past on a bounded sideways nudge instead of head-bonking, and never embeds --- */
    {
        load_empty_grid(24, PLAY_ROWS);
        for (int c = 0; c < 24; c++) G.vault_data.tiles[PLAY_ROWS - 1][c] = T_HULL;
        G.vault_data.tiles[8][10] = T_BEDROCK;    /* a single overhang tile */
        memset(&G.player, 0, sizeof G.player);
        G.player.facing = 1;
        G.player.buffer_tick = -1;
        /* stand so the box overlaps col 10 by only ~3 px: a -4 px nudge frees col 9 */
        G.player.x = 151.0f;
        G.player.y = (float)((PLAY_ROWS - 1) * TILE_SIZE) - PLAYER_H;
        G.player.grounded = true;
        settle_on_floor();
        float corner_start_x = G.player.x;
        float peak_y = G.player.y;
        bool clean = true;
        game_set_held_controls(true, false, false, false, false, true, false);  /* jump */
        game_tick();
        for (int i = 0; i < 90 && !G.player.grounded; i++) {
            game_set_held_controls(true, false, false, false, false, true, false);
            game_tick();
            if (G.player.y < peak_y) peak_y = G.player.y;
            if (!game_validate(error, sizeof error)) clean = false;
        }
        EXPECT(peak_y < (float)(8 * TILE_SIZE) - 2.0f,
               "a corner-clip jump slips past the overhang instead of head-bonking");
        EXPECT(G.player.x < corner_start_x - 1.0f &&
                   G.player.x > corner_start_x - CORNER_NUDGE - 1.0f,
               "the corner nudge is bounded to CORNER_NUDGE pixels sideways");
        EXPECT(clean, "corner correction never embeds the player in solid geometry");
    }

    /* --- M7 hit-stop: an impact freezes the sim for a bounded number of ticks, the
           freeze is never seen as out of range by game_validate, and it self-clears --- */
    {
        load_flat_arena(24);
        Enemy *e = place_enemy(0, EN_WALKER, 12, PLAY_ROWS - 2);
        memset(&G.player, 0, sizeof G.player);
        G.player.facing = 1; G.player.buffer_tick = -1;
        G.player.x = e->x;
        G.player.y = e->y - PLAYER_H - 2.0f;
        G.player.prev_bottom = G.player.y + PLAYER_H;
        G.player.vy = 60.0f;
        G.player.invuln = 1.0e9f;
        bool froze = false, hitstop_bounded = true, self_cleared = false;
        for (int i = 0; i < 90; i++) {
            game_set_held_controls(true, false, false, false, false, false, false);
            game_tick();
            if (G.hitstop > 0) froze = true;
            if (G.hitstop < 0 || G.hitstop > HITSTOP_MAX) hitstop_bounded = false;
            if (!game_validate(error, sizeof error)) hitstop_bounded = false;
            if (froze && G.hitstop == 0) self_cleared = true;
        }
        EXPECT(froze, "a stomp triggers a hit-stop freeze");
        EXPECT(hitstop_bounded, "hit-stop stays within [0, HITSTOP_MAX] every tick");
        EXPECT(self_cleared, "the hit-stop freeze is bounded and self-clears");
    }

    /* --- Pit death: an open void gap is lethal.  Walking off a solid lip into a
           bottomless gap drops Kilix through it (below-floor is no longer solid for
           the player) and costs a spare unit; standing on solid floor never does; the
           death fires within a bounded, deterministic window; and armour is no
           parachute -- even a Charged Kilix dies to a void fall. --- */
    {
        /* (a) walk right off the lip into a gap -> fall -> lose a unit */
        load_flat_arena(20);
        for (int c = 8; c < 20; c++)
            G.vault_data.tiles[PLAY_ROWS - 1][c] = T_EMPTY;   /* gap: col 8 to the wall */
        park_player(4);
        G.lives = 3;
        int lives0 = G.lives, deaths0 = G.deaths, death_tick = -1;
        bool died = false;
        for (int i = 0; i < 200 && !died; i++) {
            game_set_held_controls(true, false, true, false, false, true, false);  /* run right */
            game_tick();
            if (G.state == GS_LIFE_LOST) { died = true; death_tick = i; }
        }
        EXPECT(died && G.lives == lives0 - 1 && G.deaths == deaths0 + 1,
               "walking off into a void gap falls and costs a spare unit");
        EXPECT(death_tick >= 0 && death_tick < 200,
               "the pit death is bounded and deterministic");

        /* (b) armour does not save Kilix from leaving the world */
        load_flat_arena(20);
        for (int c = 8; c < 20; c++) G.vault_data.tiles[PLAY_ROWS - 1][c] = T_EMPTY;
        park_player(4);
        G.player.power_tier = 2;                 /* Charged: still dies in a pit */
        G.lives = 3;
        bool armoured_fell = false;
        for (int i = 0; i < 200 && !armoured_fell; i++) {
            game_set_held_controls(true, false, true, false, false, true, false);
            game_tick();
            if (G.state == GS_LIFE_LOST) armoured_fell = true;
        }
        EXPECT(armoured_fell, "a void fall kills even a Charged Kilix (armour is no parachute)");

        /* (c) standing on solid floor never triggers a pit death */
        load_flat_arena(20);
        park_player(6);
        G.lives = 3;
        int lives_c = G.lives;
        bool safe = true;
        for (int i = 0; i < 180; i++) {
            game_set_held_controls(true, false, false, false, false, false, false);
            game_tick();
            if (G.state != GS_PLAYING || G.player.y > (float)(PLAY_ROWS * TILE_SIZE)) safe = false;
        }
        EXPECT(safe && G.lives == lives_c,
               "standing on solid floor never falls or triggers a pit death");
    }

    /* --- Reachable progression + the ranged phase-tool: from Plated, collecting a
           SECOND power cache promotes to Charged, which unlocks the Phase Pulse -- a
           phase-bolt that arcs and BOUNCES off the floor.  A Plated Kilix cannot fire. */
    {
        load_flat_arena(40);
        G.vault_data.tiles[8][8] = T_CACHE;
        G.vault_data.caches[0] = (CacheNode){8, 8, (uint8_t)CN_POWER};
        G.vault_data.cache_count = 1;
        memset(&G.player, 0, sizeof G.player);
        G.player.facing = 1; G.player.buffer_tick = -1;
        G.player.power_tier = 1;                          /* already Plated */
        G.player.x = 130.0f; G.player.y = 150.0f; G.player.vy = -220.0f;  /* rising into col 8 */
        for (int i = 0; i < 40 && G.vault_data.tiles[8][8] != T_SPENT; i++) idle_ticks(1);
        Pickup *pk = NULL;
        for (int k = 0; k < MAX_PICKUPS; k++) if (G.pickups[k].active) pk = &G.pickups[k];

        /* a Plated Kilix has not yet unlocked the phase-tool */
        G.player.facing = 1;
        game_fire_pulse();
        int early_pulses = 0;
        for (int k = 0; k < MAX_PROJECTILES; k++)
            if (G.projectiles[k].active && G.projectiles[k].kind == PJ_PULSE) early_pulses++;
        EXPECT(G.player.power_tier == 1 && early_pulses == 0,
               "a Plated Kilix cannot emit a Phase Pulse");

        /* collect the emerged Core -> Charged (the effect applies once, on touch) */
        for (int i = 0; i < 160 && pk && pk->active; i++) {
            G.player.invuln = 1.0e9f;
            G.player.vx = G.player.vy = 0.0f;
            G.player.grounded = true;
            G.player.x = pk->x + (PICKUP_W - PLAYER_W) * 0.5f;
            G.player.y = pk->spawn_y - PLAYER_H;
            idle_ticks(1);
        }
        EXPECT(G.player.power_tier == 2,
               "collecting a second power cache promotes Plated -> Charged");

        /* now Charged: fire, and the pulse arcs down and bounces off the floor */
        park_player(4);
        G.player.power_tier = 2; G.player.facing = 1;
        game_fire_pulse();
        Projectile *pulse = NULL;
        for (int k = 0; k < MAX_PROJECTILES; k++)
            if (G.projectiles[k].active && G.projectiles[k].kind == PJ_PULSE) pulse = &G.projectiles[k];
        EXPECT(pulse != NULL, "a Charged Kilix emits a Phase Pulse");
        int max_bounces = 0;
        for (int i = 0; i < 90 && pulse && pulse->active; i++) {
            idle_ticks(1);
            if (pulse->bounces > max_bounces) max_bounces = pulse->bounces;
        }
        EXPECT(max_bounces >= 1, "the Phase Pulse arcs and bounces off the floor");
    }

    /* --- Solvability with lethal gaps.  (1) The jumpable-gap requirement REJECTS an
           unsolvable topology: widening a floor gap beyond the actual jump arc makes
           the validator fail.  (2) The deterministic autopilot crosses every floor gap
           on every one of the 32 vaults and reaches the exit WITHOUT ever falling into
           a pit (enemies suppressed so any death is necessarily a pit death). --- */
    {
        static VaultData badv;
        level_build(0, &badv);
        EXPECT(level_validate_vault(&badv, error, sizeof error),
               "a real vault passes the jumpable-gap validator");
        int baseline = badv.rows - 1;
        int wide = level_max_jumpable_gap() + 4;                  /* past the arc's reach */
        int g0 = badv.spawn_col + 10;
        for (int c = g0; c < g0 + wide && c < badv.exit_col; c++)
            badv.tiles[baseline][c] = T_EMPTY;
        EXPECT(!level_validate_vault(&badv, error, sizeof error),
               "a floor gap wider than the jump arc is rejected as unsolvable");
    }
    {
        bool all_cleared = true, no_pit_death = true;
        int stuck_vault = -1;
        for (int v = 0; v < CAMPAIGN_VAULTS; v++) {
            game_load_level(v);
            memset(G.enemies, 0, sizeof G.enemies);
            G.vault_data.enemy_count = 0;     /* suppress spawns: isolate pit deaths */
            G.spawn_cursor = 0;
            int deaths0 = G.deaths;
            bool cleared = false, died = false;
            for (int i = 0; i < 5000 && !cleared && !died; i++) {
                game_autopilot();
                game_tick();
                if (G.deaths > deaths0) died = true;
                if (G.state == GS_VAULT_CLEAR) cleared = true;
            }
            if (died) no_pit_death = false;
            if (!cleared && stuck_vault < 0) stuck_vault = v;
            if (!cleared) all_cleared = false;
        }
        EXPECT(no_pit_death,
               "the autopilot never falls into a pit on any of the 32 vaults");
        EXPECT(all_cleared,
               "the autopilot jumps every gap and reaches the exit of all 32 vaults");
        if (!all_cleared)
            printf("       (first vault the autopilot failed to clear: %d)\n", stuck_vault + 1);
    }

    /* --- The above isolates the GEOMETRY (enemies off), but a live machine standing
           near a gap can lure a naive bot into an early leap whose descent lands in the
           void -- so the real regression guard drives every vault WITH its machines LIVE
           and asserts the autopilot never falls into a pit.  Contact damage is expected
           here (the bot is not a perfect player); we top the spare stock up so the run
           keeps traversing, and fail ONLY on a death that occurs while Kilix is below the
           world floor (a pit fall, the class the lethal-gap change introduced). --- */
    {
        bool no_pit_live = true;
        int pit_vault = -1;
        for (int v = 0; v < CAMPAIGN_VAULTS; v++) {
            game_load_level(v);
            G.lives = 99;                 /* absorb contact deaths: keep the traverse alive */
            float world_h = (float)(G.vault_data.rows * TILE_SIZE);
            int prev_deaths = G.deaths;
            bool below = false;           /* latched while Kilix is beneath the world floor */
            for (int i = 0; i < 6000; i++) {
                if (G.player.y > world_h) below = true;
                if (G.player.grounded)    below = false;
                game_autopilot();
                game_tick();
                if (G.deaths > prev_deaths) {
                    if (below && no_pit_live) { no_pit_live = false; pit_vault = v; }
                    prev_deaths = G.deaths;
                    below = false;
                }
                if (G.lives < 50) G.lives = 99;
                if (G.state == GS_VAULT_CLEAR) break;
            }
        }
        EXPECT(no_pit_live,
               "the autopilot never falls into a pit on any vault WITH its machines live");
        if (!no_pit_live)
            printf("       (first vault with a live-machine pit death: %d)\n", pit_vault + 1);
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
    load_flat_arena(24);                        /* real baseline floor to land on */
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

    /* --- M7 field manual: H opens the manual from gameplay AND from pause, each
           preserving the return state, and closing restores exactly that state --- */
    game_load_level(0);
    game_handle_key('h');
    EXPECT(G.state == GS_HELP && G.help_return_state == GS_PLAYING,
           "gameplay H opens the field manual, remembering the play state");
    game_handle_key('h');
    EXPECT(G.state == GS_PLAYING, "closing the manual resumes gameplay");
    game_handle_key('p');
    EXPECT(G.state == GS_PAUSED, "P pauses the game");
    game_handle_key('h');
    EXPECT(G.state == GS_HELP && G.help_return_state == GS_PAUSED,
           "pause H opens the field manual with a paused return state");
    game_handle_key(KEY_ESC);
    EXPECT(G.state == GS_PAUSED, "closing the manual from pause returns to the pause menu");
    game_handle_key('p');
    EXPECT(G.state == GS_PLAYING, "P resumes from pause");

    /* --- M7 field manual paging: Right/Down advance, Left/Up retreat, wrapping 3 pages --- */
    game_handle_key('h');
    game_handle_key(KEY_RIGHT);
    EXPECT(G.help_page == 1, "the manual pages forward");
    game_handle_key(KEY_LEFT);
    game_handle_key(KEY_LEFT);
    EXPECT(G.help_page == 2, "the manual pages backward with wrap");
    game_handle_key('h');
    EXPECT(G.state == GS_PLAYING, "H closes the manual");

    /* --- M7 selector: Up/Down follow the displayed campaign-map grid (one district per
           row of VAULTS_PER_DISTRICT), clamped to the unlocked range --- */
    G.state = GS_SELECT;
    G.unlock_district = DISTRICTS;
    G.sel_index = 22;                       /* district 6, vault 3 (row 5, col 2) */
    game_handle_key(KEY_UP);
    EXPECT(G.sel_index == 22 - VAULTS_PER_DISTRICT,
           "selector Up follows the grid one district up");
    game_handle_key(KEY_DOWN);
    EXPECT(G.sel_index == 22, "selector Down follows the grid one district down");
    game_handle_key(KEY_RIGHT);
    EXPECT(G.sel_index == 23, "selector Right steps one vault across");
    G.sel_index = 0;
    game_handle_key(KEY_UP);
    EXPECT(G.sel_index == 0, "the selector clamps at the first vault");

    /* --- M7 title "continue": resumes the highest unlocked district, never practice --- */
    game_init(512, 480, 42);
    G.headless = true; G.sound_on = false;
    G.state = GS_TITLE;
    G.unlock_district = 4;
    G.menu_choice = 0;
    game_handle_key(KEY_ENTER);
    EXPECT(G.state == GS_PLAYING &&
           G.level == (4 - 1) * VAULTS_PER_DISTRICT && !G.practice_mode,
           "title continue resumes the highest unlocked vault, practice off");

    /* --- M7 restart (R): spends one unit, rolls the unbanked vault score back to the
           entry baseline, and preserves the elapsed vault time (drained charge) --- */
    game_load_level(0);
    int start_score = G.score;              /* == level_start_score at vault entry */
    G.score += 350;                         /* unbanked points earned this attempt */
    G.charge = G.charge_start - 40;         /* 40 units of vault time elapsed */
    int lives0 = G.lives;
    game_handle_key('r');
    EXPECT(G.state == GS_LIFE_LOST && G.lives == lives0 - 1,
           "manual restart spends exactly one spare unit");
    EXPECT(G.score == start_score, "restart rolls the unbanked vault score back to entry");
    G.state_timer = 0.0f;
    game_tick();                            /* run out the life-lost beat -> redeploy */
    EXPECT(G.state == GS_PLAYING && G.charge == G.charge_start - 40,
           "restart preserves the elapsed vault time instead of refilling it");

    /* --- M7 sound toggle (M): flips the flag in any state --- */
    {
        bool before_sound = G.sound_on;
        game_handle_key('m');
        EXPECT(G.sound_on == !before_sound, "the sound toggle flips the flag");
        game_handle_key('m');
        EXPECT(G.sound_on == before_sound, "the sound toggle flips back");
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

        G.player.power_tier = 0; G.player.aegis_q = 0;
        failed |= write_scene(directory, "powerup_bare"); images++;

        G.player.power_tier = 1;
        failed |= write_scene(directory, "powerup_plated"); images++;

        G.player.power_tier = 2;
        failed |= write_scene(directory, "powerup_charged"); images++;

        G.player.aegis_q = AEGIS_Q;
        failed |= write_scene(directory, "powerup_aegis"); images++;
    }

    /* Dispensed pickups (cast.md §4): a star-mote coin popping up out of a struck
       cache, and a Phase Core power-up mid-emerge (clipped so it rises out of the
       block top). */
    {
        game_start(0);
        memset(G.enemies, 0, sizeof G.enemies);
        memset(G.projectiles, 0, sizeof G.projectiles);
        memset(G.pickups, 0, sizeof G.pickups);
        G.player.x = (float)(6 * TILE);
        G.player.y = (float)(baseline_y) - PLAYER_H;
        G.player.grounded = true;
        G.player.gait_amount = 0.0f; G.player.gait_phase = 0.0f;
        G.cam_x = G.cam_x_max = 0.0f;

        float top = (float)((PLAY_ROWS - 5) * TILE);      /* a block-top line in view */
        float cx  = (float)(9 * TILE) + TILE * 0.5f;
        Pickup *pk = &G.pickups[0];

        memset(pk, 0, sizeof *pk);
        pk->active = true; pk->kind = PK_COIN; pk->content = CN_MOTE;
        pk->x = cx - PICKUP_W * 0.5f; pk->y = top - PICKUP_H - 12.0f;  /* mid-pop, risen */
        pk->vx = 0.0f; pk->vy = -80.0f;
        failed |= write_scene(directory, "pickup_coin"); images++;

        memset(pk, 0, sizeof *pk);
        pk->active = true; pk->kind = PK_CORE; pk->content = CN_POWER;
        pk->x = cx - PICKUP_W * 0.5f; pk->spawn_y = top;
        pk->emerge = 0.55f; pk->y = top - pk->emerge * PICKUP_H;       /* mid-emerge, clipped */
        pk->facing = 1;
        failed |= write_scene(directory, "pickup_powerup"); images++;
    }

    /* The HUD overlay (M6): a live gameplay frame with score, motes, timer, the
     * power-tier chip, lives, and the district-vault label all populated. */
    {
        game_start(0);
        G.player.x = (float)(20 * TILE);
        G.player.y = (float)(baseline_y) - PLAYER_H;
        G.player.grounded = true;
        G.player.power_tier = 2;
        G.player.aegis_q = AEGIS_Q;
        G.cam_x = G.cam_x_max = G.player.x - CAM_DEADZONE;
        if (G.cam_x < 0.0f) G.cam_x = 0.0f;
        G.score = 123450;
        G.motes = 45;
        G.lives = 3;
        G.charge = 250;
        failed |= write_scene(directory, "hud"); images++;
    }

    /* Hit-stop: a live gameplay frame with the impact freeze + flash engaged, and a
       machine underfoot to sell the moment.  hitstop is a bounded G field; the render
       only reads it, so the purity memcmp still holds. */
    {
        game_start(0);
        memset(G.enemies, 0, sizeof G.enemies);
        G.player.x = (float)(20 * TILE);
        G.player.y = (float)(baseline_y) - PLAYER_H - 3.0f;
        G.player.vy = -STOMP_BOUNCE;
        G.player.land_squash = -1.0f;
        G.cam_x = G.cam_x_max = G.player.x - CAM_DEADZONE;
        if (G.cam_x < 0.0f) G.cam_x = 0.0f;
        Enemy *e = &G.enemies[0];
        e->active = true; e->kind = EN_WALKER; e->facing = -1;
        e->state = ES_SQUASHED; e->squash = SQUASH_TIME; e->alert = 1.0f; e->tell = 1.0f;
        e->x = (float)(20 * TILE); e->y = (float)baseline_y - SQUASH_H;
        e->home_x = e->x;
        G.hitstop = HITSTOP_HEAVY;
        G.score = 30100; G.motes = 8; G.lives = 3; G.charge = 280;
        failed |= write_scene(directory, "hitstop"); images++;
    }

    /* Screen shake: the SAME frame with a large render-time shake offset engaged.  The
       amplitude lives only in render.c, so write_scene's memcmp(&before,&G) still passes
       — the proof that shake never touches the simulation. */
    {
        game_start(0);
        memset(G.enemies, 0, sizeof G.enemies);
        G.player.x = (float)(20 * TILE);
        G.player.y = (float)(baseline_y) - PLAYER_H;
        G.player.grounded = true;
        G.cam_x = G.cam_x_max = G.player.x - CAM_DEADZONE;
        if (G.cam_x < 0.0f) G.cam_x = 0.0f;
        G.score = 30100; G.motes = 8; G.lives = 3; G.charge = 280;
        render_shake(9.0f);                /* a big offset for the shake fixture */
        failed |= write_scene(directory, "shake"); images++;
        render_shake(0.0f);                /* reset so later scenes are steady */
    }

    /* The campaign-map vault selector with the cursor on a mid-campaign vault. */
    {
        game_start(0);
        G.unlock_district = 5;
        G.sel_index = 9;                    /* district 3, vault 2 */
        G.state = GS_SELECT;
        failed |= write_scene(directory, "menu_select"); images++;
    }

    /* The pause overlay over a frozen playfield. */
    {
        game_start(0);
        G.player.x = (float)(20 * TILE);
        G.player.y = (float)(baseline_y) - PLAYER_H;
        G.player.grounded = true;
        G.cam_x = G.cam_x_max = G.player.x - CAM_DEADZONE;
        if (G.cam_x < 0.0f) G.cam_x = 0.0f;
        G.score = 24300; G.motes = 12; G.lives = 3; G.charge = 300;
        G.state = GS_PAUSED;
        failed |= write_scene(directory, "paused"); images++;
    }

    /* The three field-manual pages (opened from the title, so the backdrop shows). */
    {
        game_start(0);
        G.help_return_state = GS_TITLE;
        G.state = GS_HELP;
        for (int page = 0; page < 3; page++) {
            char name[16];
            snprintf(name, sizeof name, "help_%d", page + 1);
            G.help_page = page;
            failed |= write_scene(directory, name); images++;
        }
    }

    /* The four transient / end-state cards. */
    {
        game_start(0);
        G.score = 48800;
        G.charge = 180;
        G.state = GS_VAULT_CLEAR;
        failed |= write_scene(directory, "clear"); images++;
        G.state = GS_LIFE_LOST;
        failed |= write_scene(directory, "life_lost"); images++;
        G.state = GS_GAMEOVER;
        failed |= write_scene(directory, "gameover"); images++;
        G.state = GS_VICTORY;
        failed |= write_scene(directory, "victory"); images++;
    }

    /* Per-district showcase scenes: each district's thesis vault, rendered in its
     * own biome palette (visual-identity §3.3). */
    for (int d = 1; d <= DISTRICTS; d++) {
        game_load_level((d - 1) * VAULTS_PER_DISTRICT);
        int mid = G.vault_data.cols / 2;
        G.player.x = (float)(mid * TILE);
        G.player.y = (float)(baseline_y) - PLAYER_H;
        G.player.grounded = true;
        G.player.gait_amount = 0.6f;
        G.player.gait_phase = 1.5707963f;
        float cam_limit = (float)(G.vault_data.cols * TILE - LOGICAL_W);
        if (cam_limit < 0.0f) cam_limit = 0.0f;
        G.cam_x = G.player.x - CAM_DEADZONE;
        if (G.cam_x < 0.0f) G.cam_x = 0.0f;
        if (G.cam_x > cam_limit) G.cam_x = cam_limit;
        G.cam_x_max = G.cam_x;
        char name[32];
        snprintf(name, sizeof name, "district_%d", d);
        failed |= write_scene(directory, name); images++;
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
    /* Offline byte-determinism + song/SFX validation runs first: it needs no
     * audio sink (it uses the chip-sequencer offline bounce path), so it gates
     * the build whether or not a device exists.  A determinism/validation
     * failure is a real regression and fails here. */
    if (!sound_render_selfcheck()) {
        fprintf(stderr, "sound-test: offline render is not byte-deterministic "
                        "or a song failed validation\n");
        return 1;
    }
    /* Audio is optional and never fatal: --sound-test returns 0 with no sink. */
    if (!sound_init()) {
        printf("sound-test: no audio sink; silent fallback is operational\n");
        return 0;
    }
    /* With a live sink, audibly walk every SFX id and every track. */
    sound_selftest_play();
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

/* The complete campaign manifest: one line per vault with its label, extent,
 * topology + route signatures, goal shape, machine budget, and charge — the
 * headless-inspectable overview (build-plan.md §M6). */
static int dump_campaign(void)
{
    char error[192];
    printf("SUPER KILIX CAMPAIGN  %d districts x %d = %d vaults\n",
           DISTRICTS, VAULTS_PER_DISTRICT, CAMPAIGN_VAULTS);
    printf("idx  vault              slot   cols  sig       route     goal  mach/bud  charge\n");
    static const char *const slot_name[VAULTS_PER_DISTRICT] = {
        "thesis", "variat", "ascent", "gate"
    };
    bool ok = true;
    for (int i = 0; i < CAMPAIGN_VAULTS; i++) {
        VaultData v;
        level_build(i, &v);
        if (!level_validate(i, error, sizeof error)) ok = false;
        printf("%2d-%d %-18s %-6s %4d  %08x  %08x  %-4s  %2d/%2d    %4u\n",
               v.district, v.vault, vault_name(i),
               slot_name[i % VAULTS_PER_DISTRICT], v.cols,
               level_signature(&v), level_route_key(&v),
               level_is_gate(i) ? "seal" : "rise",
               v.enemy_count, level_enemy_budget(i), (unsigned)v.timer_start);
    }
    printf(ok ? "all %d vaults valid\n" : "campaign has invalid vaults\n",
           CAMPAIGN_VAULTS);
    return ok ? 0 : 1;
}

static int usage(void)
{
    printf("super-kilix %s\n"
           "usage: super-kilix [--level N] [--selftest [seed] [ticks]]\n"
           "                   [--rules-test] [--input-test]\n"
           "                   [--render-test [seed]] [--sound-test]\n"
           "                   [--dump-level N] [--dump-campaign]\n"
           "                   [--version] [--help]\n\n"
           "Run without arguments in a Kitty-protocol terminal to play.\n",
           SK_VERSION);
    return 0;
}

/* ---------------------------------------------------------------- play loop */

/* Drive the live music track + held thruster from the current game state.  Both
 * calls dedup internally, so it is safe to run every presented frame; both are
 * silent no-ops when there is no audio sink. */
static void update_audio(void)
{
    int bed = MUS_DISTRICT_BED(G.vault_data.district);   /* the current district's bed */
    if (bed < MUS_RUST_FLATS || bed > MUS_WARDEN_VAULT) bed = MUS_RUST_FLATS;
    int track;
    switch (G.state) {
    case GS_TITLE:       track = MUS_TITLE;    break;
    case GS_SELECT:      track = MUS_TITLE;    break;
    case GS_HELP:        track = G.help_return_state == GS_TITLE ? MUS_TITLE : bed; break;
    case GS_VAULT_CLEAR: track = MUS_CLEAR;    break;
    case GS_VICTORY:     track = MUS_CLEAR;    break;
    case GS_GAMEOVER:    track = MUS_GAMEOVER; break;
    case GS_PLAYING: {
        bool boss = false;
        for (int i = 0; i < MAX_ACTIVE_ENEMIES; i++)
            if (G.enemies[i].active &&
                (G.enemies[i].kind == EN_GUARDIAN || G.enemies[i].kind == EN_OVERSEER)) {
                boss = true;
                break;
            }
        track = boss ? MUS_BOSS : bed;
        break;
    }
    default:             track = bed; break;   /* paused / life-lost: keep the district bed */
    }
    sound_music(track);

    bool thrusting = G.state == GS_PLAYING && G.player.thrusting;
    sound_jet(thrusting, fminf(1.0f, fabsf(G.player.vx) / RUN_MAX));
}

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
    /* --level N is a practice run that drops straight into the vault (never mutates the
     * profile).  A plain launch opens on the title screen (game_init already set GS_TITLE
     * with the profile loaded); the title menu's "continue inward" starts the campaign. */
    if (forced_level >= 0)
        game_start_at(forced_level, true);

    /* The family's canonical 60 Hz fixed-step clock (kilix_game_loop.h): each
     * frame yields a bounded number of sim steps, then we present once. */
    kilix_game_clock_options options;
    kilix_game_clock_options_init(&options);
    options.step_ns = KILIX_GAME_NANOSECONDS_PER_SECOND / 60;
    kilix_game_clock clock;
    kilix_game_clock_init(&clock, &options);
    kilix_game_clock_reset(&clock, kilix_game_monotonic_ns());

    /* Pace presents to a steady 60 Hz.  Without a cap the loop spins and floods
     * the graphics pipe with identical frames (the sim only advances at 60 Hz),
     * which the terminal cannot drain evenly -- the visible symptom is choppy,
     * unevenly-timed motion.  One present per 60 Hz frame is smooth and matches
     * the fixed-step sim 1:1. */
    const int64_t present_period_ns = KILIX_GAME_NANOSECONDS_PER_SECOND / 60;
    int64_t next_present_ns = kilix_game_monotonic_ns();

    while (!G.quit) {
        if (term_read_input() < 0) { G.quit = true; break; }

        bool held = term_has_release_events();
        kittykb_event event;
        while (term_next_key_event(&event)) {
            if (event.action != KITTYKB_ACTION_PRESS) continue;
            /* Ctrl-C (raw key 3 or C+CTRL): restore the terminal and quit at once. */
            if (interrupt_event(&event)) { term_emergency_restore(); G.quit = true; break; }
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
        update_audio();
        render_frame();
        term_present(render_fb(), G.W, G.H);

        next_present_ns += present_period_ns;
        int64_t now_ns = kilix_game_monotonic_ns();
        if (next_present_ns < now_ns - present_period_ns * 4)
            next_present_ns = now_ns;    /* fell far behind: resync without spiralling */
        else
            kilix_game_sleep_until_ns(next_present_ns);
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
    if (argc > 1 && !strcmp(argv[1], "--dump-campaign")) {
        if (argc != 2) return option_arity_error("--dump-campaign", "takes no arguments");
        return dump_campaign();
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

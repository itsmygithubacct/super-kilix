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

#define DUMP_LEVEL_MAX 32   /* the campaign's vault count (data.c gains teeth at M3) */

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
    static const char letters[] = "adhmpqrswz";
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
 * below still reads solid, so a drop lands on a virtual floor). */
static void load_empty_grid(int cols, int rows)
{
    memset(&G.vault_data, 0, sizeof G.vault_data);
    G.vault_data.cols = cols;
    G.vault_data.rows = rows;
    G.cam_x = G.cam_x_max = G.cam_y = 0.0f;
    G.scroll_lock = false;
    G.state = GS_PLAYING;
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
    printf("PASS selftest seed=%u ticks=%d state=%016llx\n",
           seed, ticks, (unsigned long long)state_hash());
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

    /* --- walk off a ledge, fall, and land --- */
    game_load_level(0);
    memset(&G.player, 0, sizeof G.player);
    G.player.facing = 1;
    G.player.buffer_tick = -1;
    G.player.x = (float)(7 * TILE_SIZE) + 2.0f;
    G.player.y = (float)(8 * TILE_SIZE) - PLAYER_H;   /* standing on the shelf */
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

    G.state = GS_TITLE;
    failed |= write_scene(directory, "title"); images++;

    /* Kilix on the placeholder stage: idle, then the two opposed walk strides
     * whose differing pixels gate the port's correctness. */
    G.state = GS_PLAYING;
    G.player.x = (float)(LOGICAL_W / 2 - 6);
    G.player.y = (float)(LOGICAL_H - TILE * 3 - 16);
    G.player.facing = 1;
    G.player.grounded = true;
    G.player.gait_amount = 0.0f;
    G.player.gait_phase = 0.0f;
    failed |= write_scene(directory, "kilix_idle"); images++;

    G.player.gait_amount = 1.0f;
    G.player.gait_phase = 1.5707963f;
    failed |= write_scene(directory, "walk_stride_a"); images++;
    G.player.gait_phase = 4.7123890f;
    failed |= write_scene(directory, "walk_stride_b"); images++;

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

static int dump_level(int one_based)
{
    if (one_based < 1 || one_based > DUMP_LEVEL_MAX) {
        fprintf(stderr, "--dump-level needs 1..%d\n", DUMP_LEVEL_MAX);
        return 2;
    }
    /* The annotated semantic grid arrives at M3; M0 confirms the mode is wired. */
    printf("vault %d: layout pending (M3)\n", one_based);
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

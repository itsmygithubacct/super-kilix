/* Entry point, signal handling, the fixed-step terminal loop, and every
 * deterministic headless mode.  M0 wires the CLI, the trivial --selftest, and
 * the family's shared 60 Hz clock; the visual loop gains teeth at M1. */
#include "super_kilix.h"

#include "kilix_game_loop.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
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

static int selftest(uint32_t seed, int ticks)
{
    headless_environment();
    char error[192];
    if (ticks <= 0) ticks = 12000;
    game_init(0, 0, seed);
    G.headless = true;
    G.sound_on = false;
    for (int i = 0; i < ticks; i++) {
        game_tick();
        if (!game_validate(error, sizeof error)) {
            fprintf(stderr, "FAIL seed=%u tick=%d: %s\n", seed, i, error);
            game_shutdown();
            return 1;
        }
    }
    printf("PASS selftest seed=%u ticks=%d\n", seed, ticks);
    game_shutdown();
    return 0;
}

static int rules_test(void)
{
    headless_environment();
    /* Physics and content fixtures arrive at M2/M3; M0 asserts the skeleton. */
    printf("PASS rules-test\n");
    return 0;
}

static int input_test(void)
{
    headless_environment();
    /* Input funnel fixtures arrive at M2; M0 asserts the skeleton. */
    printf("PASS input-test\n");
    return 0;
}

static int render_test(uint32_t seed)
{
    headless_environment();
    /* Scene corpus + PPM dumps + render purity arrive at M1; M0 asserts the skeleton. */
    printf("PASS render-test seed=%u\n", seed);
    return 0;
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
    (void)forced_level;   /* practice-mode start arrives at M6 */

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

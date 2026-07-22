/* Adapter over the shared Kitty framebuffer/keyboard terminal session.  term.c
 * is the only module that includes kitty_terminal_session.h.  It presents the
 * packed RGBA framebuffer render.c produces and funnels keyboard input up to
 * the game.  A failed present latches so a dead terminal ends the play loop. */
#include "super_kilix.h"
#include "kitty_terminal_session.h"

#include <errno.h>
#include <stdlib.h>
#include <unistd.h>

static kittyts_session terminal;
static bool presentation_failed;

bool term_init(int *out_width, int *out_height)
{
    kittyts_options options;
    presentation_failed = false;
    kittyts_session_init(&terminal);
    kittyts_options_init(&options);
    /* The logical scene is 256x240 (16:15); keep the terminal framebuffer in
     * that neighbourhood so sr_scale_canvas need not distort the aspect. */
    options.framebuffer.min_width = 384;
    options.framebuffer.min_height = 360;
    options.framebuffer.max_width = 1536;
    options.framebuffer.max_height = 1440;
    if (getenv("SUPER_KILIX_SKIP_PROBE"))
        options.framebuffer.probe_graphics = false;
    if (kittyts_start(&terminal, STDIN_FILENO, STDOUT_FILENO, &options) != 0)
        return false;
    *out_width = kittyts_width(&terminal);
    *out_height = kittyts_height(&terminal);
    return true;
}

bool term_check_resize(int *out_width, int *out_height)
{
    return kittyts_check_resize(&terminal, out_width, out_height);
}

void term_present(const uint8_t *rgba, int width, int height)
{
    if (!kittyts_present(&terminal, rgba, width, height))
        presentation_failed = true;
}

int term_read_input(void)
{
    if (presentation_failed) {
        errno = EIO;
        return -1;
    }
    return kittyts_read_input(&terminal);
}

bool term_next_key_event(kittykb_event *event)
{
    return kittyts_next_key_event(&terminal, event);
}

bool term_key_down(uint32_t key)
{
    return kittyts_key_down(&terminal, key);
}

bool term_has_release_events(void)
{
    return kittyts_has_release_events(&terminal);
}

void term_shutdown(void)
{
    kittyts_stop(&terminal);
    presentation_failed = false;
}

void term_emergency_restore(void)
{
    kittyts_emergency_restore(&terminal);
}

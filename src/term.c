/* Thin adapter over kitty-terminal-session.  term.c is the only module that
 * includes kitty_terminal_session.h.  M0 is a safe stub: term_init fails so the
 * binary is headless-only until the real adapter lands at M1. */
#include "super_kilix.h"

#include "kitty_terminal_session.h"

bool term_init(int *ow, int *oh)
{
    if (ow) *ow = LOGICAL_W;
    if (oh) *oh = LOGICAL_H;
    return false;
}

bool term_check_resize(int *ow, int *oh)
{
    (void)ow;
    (void)oh;
    return false;
}

void term_present(const uint8_t *rgba, int w, int h)
{
    (void)rgba;
    (void)w;
    (void)h;
}

int term_read_input(void)
{
    return 0;
}

bool term_next_key_event(kittykb_event *event)
{
    (void)event;
    return false;
}

bool term_key_down(uint32_t key)
{
    (void)key;
    return false;
}

bool term_has_release_events(void)
{
    return false;
}

void term_shutdown(void)
{
}

void term_emergency_restore(void)
{
}

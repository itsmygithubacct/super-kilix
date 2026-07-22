/* Procedural audio.  sound.c is the only module that includes pcm_mixer.h (and,
 * from M5, chip_sequencer.h).  Audio is optional and never fatal: M0's stub
 * reports no sink so the game runs silently. */
#include "super_kilix.h"

#include "pcm_mixer.h"

static bool sound_enabled;

bool sound_init(void)
{
    sound_enabled = false;
    return false;
}

void sound_shutdown(void)
{
    sound_enabled = false;
}

void sound_set_enabled(bool on)
{
    sound_enabled = on;
}

bool sound_is_enabled(void)
{
    return sound_enabled;
}

void sound_play(int id, float volume, float pitch)
{
    (void)id;
    (void)volume;
    (void)pitch;
}

void sound_jet(bool active, float intensity)
{
    (void)active;
    (void)intensity;
}

/*
 * Procedural audio.  sound.c is the ONLY module that includes pcm_mixer.h and
 * chip_sequencer.h: the chip-sequencer is the deterministic audio *source* (it
 * bakes compact in-source song data into PCM with a libm-free fixed-point chip
 * synth); pcm-mixer is the *transport* (device, thread, voice model).  The whole
 * seam is one function pointer: pcmmix_set_generator(&mixer, chipseq_generator).
 *
 * Audio is OPTIONAL and never fatal.  sound_init() returning false means the
 * game runs silently: every entry point early-returns when not started/disabled,
 * and --sound-test returns 0 with no sink.
 *
 * Every note table below is an ORIGINAL composition realising the motif, key,
 * tempo and instrumentation descriptions in the audio-identity bible.  Nothing
 * is transcribed from any other work; only the synthesis *technique* (duty-pulse
 * leads, an octave-down triangle bass, gapped-noise percussion, table-stepped
 * pitch sweeps) is reused, which is non-copyrightable.
 */
#include "super_kilix.h"

#include "pcm_mixer.h"
#include "chip_sequencer.h"

#include <math.h>
#include <string.h>
#include <time.h>

#define SOUND_RATE 44100u

/* ---------------------------------------------------------------- cell DSL */
/* Tracker-screen cell macros (chip-sequencer-spec §3.2): a pattern reads like a
 * tracker grid, cells[row*channels + chan]. */
#define CS__                   { CHIPSEQ_NOTE_NONE, 0, CHIPSEQ_VOL_NONE, CHIPSEQ_FX_NONE, 0 }
#define CS_OFF                 { CHIPSEQ_NOTE_OFF,  0, CHIPSEQ_VOL_NONE, CHIPSEQ_FX_NONE, 0 }
#define CS_CUT                 { CHIPSEQ_NOTE_CUT,  0, CHIPSEQ_VOL_NONE, CHIPSEQ_FX_NONE, 0 }
#define CS_N(pc,oct,i,v)       { CHIPSEQ_NOTE(CHIPSEQ_PC_##pc,oct), (i), (v), CHIPSEQ_FX_NONE, 0 }
#define CS_NF(pc,oct,i,v,fx,p) { CHIPSEQ_NOTE(CHIPSEQ_PC_##pc,oct), (i), (v), CHIPSEQ_FX_##fx, (p) }
#define CS_FX(fx,p)            { CHIPSEQ_NOTE_NONE, 0, CHIPSEQ_VOL_NONE, CHIPSEQ_FX_##fx, (p) }

#define SEQ_ONCE CHIPSEQ_SEQ_NO_LOOP, CHIPSEQ_SEQ_NO_RELEASE

/* =====================================================================
 *  Voice palette — the shared step-tables and instruments (audio-identity §1.1)
 * ===================================================================== */

/* The Shimmer wavetable: a soft glassy 32-nibble hump (0..15).  The one honestly
 * non-stock texture voice, used sparingly for bells/pads. */
static const uint8_t shimmer_wt[CHIPSEQ_WAVETABLE_LEN] = {
    8, 9,11,12,13,14,15,15, 15,14,13,12,11, 9, 8, 7,
    6, 5, 3, 2, 1, 0, 0, 0,  0, 1, 2, 3, 5, 6, 7, 8,
};

/* Volume envelopes (the software-envelope technique: exp-ish decays as data). */
static const int8_t v_lead[]  = { 64,62,58,54,50,48,46,46 };
static const chipseq_seq lead_vol = { v_lead, 8, 7, CHIPSEQ_SEQ_NO_RELEASE };
static const int8_t v_pluck[] = { 64,58,50,42,34,27,20,14,9,5,2,0 };
static const chipseq_seq pluck_vol = { v_pluck, 12, SEQ_ONCE };
static const int8_t v_bass[]  = { 64,54,44,36,30,26,24,24 };
static const chipseq_seq bass_vol = { v_bass, 8, 7, CHIPSEQ_SEQ_NO_RELEASE };
static const int8_t v_bell[]  = { 64,60,52,44,37,30,24,18,13,9,5,2,0 };
static const chipseq_seq bell_vol = { v_bell, 13, SEQ_ONCE };
static const int8_t v_kick[]  = { 64,50,32,16,4,0 };
static const chipseq_seq kick_vol = { v_kick, 6, SEQ_ONCE };
static const int8_t p_kick[]  = { 24,10,2,-4,-8 };
static const chipseq_seq kick_pitch = { p_kick, 5, SEQ_ONCE };
static const int8_t v_snare[] = { 64,44,28,16,8,2,0 };
static const chipseq_seq snare_vol = { v_snare, 7, SEQ_ONCE };
static const int8_t v_hat[]   = { 40,18,6,0 };
static const chipseq_seq hat_vol = { v_hat, 4, SEQ_ONCE };

/* The five-voice authored palette + the Shimmer, shared by the big beds/themes.
 * Cells reference these by index; a song's channel count is just how many are on
 * screen at once. */
enum { MI_LEAD, MI_HARM, MI_BASS, MI_KICK, MI_SNARE, MI_HAT, MI_BELL, MI_COUNT };
static const chipseq_instrument music_insts[] = {
    [MI_LEAD]  = { .name = "lead",  .wave = CHIPSEQ_WAVE_PULSE,    .duty = CHIPSEQ_DUTY_25, .vol_seq = &lead_vol  },
    [MI_HARM]  = { .name = "harm",  .wave = CHIPSEQ_WAVE_PULSE,    .duty = CHIPSEQ_DUTY_12, .vol_seq = &pluck_vol },
    [MI_BASS]  = { .name = "bass",  .wave = CHIPSEQ_WAVE_TRIANGLE, .tri_steps = 32,         .vol_seq = &bass_vol  },
    [MI_KICK]  = { .name = "kick",  .wave = CHIPSEQ_WAVE_NOISE,    .noise_mode = CHIPSEQ_NOISE_LONG,  .vol_seq = &kick_vol,  .pitch_seq = &kick_pitch },
    [MI_SNARE] = { .name = "snare", .wave = CHIPSEQ_WAVE_NOISE,    .noise_mode = CHIPSEQ_NOISE_SHORT, .vol_seq = &snare_vol },
    [MI_HAT]   = { .name = "hat",   .wave = CHIPSEQ_WAVE_NOISE,    .noise_mode = CHIPSEQ_NOISE_LONG,  .vol_seq = &hat_vol },
    [MI_BELL]  = { .name = "bell",  .wave = CHIPSEQ_WAVE_WAVETABLE, .vol_seq = &bell_vol, .wavetable = shimmer_wt },
};
#define MUSIC_INST_COUNT ((uint16_t)(sizeof music_insts / sizeof music_insts[0]))

/* =====================================================================
 *  Title theme — "Vault Reveille"  (D dorian, 132 BPM, a lone salvager's
 *  fanfare).  Motif: the Call Sign — tonic up a fourth up to the fifth
 *  (D -> G -> A) — answered by a stepwise descent, then a lift to the octave.
 *  Channels: lead / harm(a third below) / bass / bell. (audio-identity §3.1)
 * ===================================================================== */
static const chipseq_cell title_p0[] = {
/*  lead              harm             bass             bell            */
    CS_N(D,5,MI_LEAD,56), CS_N(B,4,MI_HARM,36), CS_N(D,3,MI_BASS,54), CS__,
    CS__,                 CS__,                 CS__,                 CS__,
    CS__,                 CS__,                 CS__,                 CS__,
    CS__,                 CS__,                 CS__,                 CS__,
    CS_N(G,5,MI_LEAD,56), CS_N(E,5,MI_HARM,36), CS__,                 CS__,
    CS__,                 CS__,                 CS__,                 CS__,
    CS__,                 CS__,                 CS_N(C,3,MI_BASS,50), CS__,
    CS__,                 CS__,                 CS__,                 CS__,
    CS_N(A,5,MI_LEAD,58), CS_N(F,5,MI_HARM,36), CS_N(A,3,MI_BASS,50), CS__,
    CS__,                 CS__,                 CS__,                 CS__,
    CS__,                 CS__,                 CS__,                 CS__,
    CS__,                 CS__,                 CS__,                 CS__,
    CS_N(A,5,MI_LEAD,46), CS__,                 CS_N(G,3,MI_BASS,48), CS_N(A,5,MI_BELL,40),
    CS__,                 CS__,                 CS__,                 CS__,
    CS__,                 CS__,                 CS__,                 CS__,
    CS__,                 CS__,                 CS__,                 CS__,
};
static const chipseq_cell title_p1[] = {
/*  lead              harm             bass             bell            */
    CS_N(A,5,MI_LEAD,54), CS_N(F,5,MI_HARM,34), CS_N(D,3,MI_BASS,52), CS__,
    CS__,                 CS__,                 CS__,                 CS__,
    CS_N(G,5,MI_LEAD,52), CS_N(E,5,MI_HARM,34), CS__,                 CS__,
    CS__,                 CS__,                 CS__,                 CS__,
    CS_N(F,5,MI_LEAD,52), CS_N(D,5,MI_HARM,34), CS_N(C,3,MI_BASS,50), CS__,
    CS__,                 CS__,                 CS__,                 CS__,
    CS_N(E,5,MI_LEAD,50), CS_N(C,5,MI_HARM,34), CS__,                 CS__,
    CS__,                 CS__,                 CS__,                 CS__,
    CS_N(D,5,MI_LEAD,56), CS_N(B,4,MI_HARM,36), CS_N(D,3,MI_BASS,54), CS__,
    CS__,                 CS__,                 CS__,                 CS__,
    CS__,                 CS__,                 CS__,                 CS__,
    CS__,                 CS__,                 CS__,                 CS__,
    CS_N(D,6,MI_LEAD,50), CS__,                 CS_N(D,3,MI_BASS,46), CS_N(D,6,MI_BELL,42),
    CS__,                 CS__,                 CS__,                 CS__,
    CS__,                 CS__,                 CS__,                 CS__,
    CS__,                 CS__,                 CS__,                 CS__,
};
static const chipseq_pattern title_pats[] = { { title_p0, 16 }, { title_p1, 16 } };
static const uint8_t title_order[] = { 0, 1 };
static const chipseq_song song_title = {
    .name = "vault-reveille",
    .instruments = music_insts, .instrument_count = MUSIC_INST_COUNT,
    .patterns = title_pats, .pattern_count = 2,
    .order = title_order, .order_length = 2,
    .loop_order = 0, .channels = 4,
    .rows_per_beat = 4, .ticks_per_row = 6, .bpm_q8 = CHIPSEQ_BPM(132),
};

/* =====================================================================
 *  RUST FLATS bed — "Sunward Run"  (A mixolydian, 150 BPM, swung sixteenths).
 *  Motif: an 8-bar pentatonic hook that leaps up a major sixth (A -> F#) then
 *  walks stepwise down over a I-IV-I-V vamp, call-and-response between Pulse A
 *  and Pulse B, over a full three-piece noise kit. (audio-identity §3.2)
 * ===================================================================== */
static const chipseq_cell rust_p0[] = {
/*  lead               harm              bass              kick              snare             hat              */
    CS_N(A,4,MI_LEAD,56), CS_N(A,3,MI_HARM,40), CS_N(A,2,MI_BASS,56), CS_N(C,2,MI_KICK,60), CS__,                 CS_N(C,6,MI_HAT,34),
    CS__,                 CS__,                 CS__,                 CS__,                 CS__,                 CS__,
    CS_N(Fs,5,MI_LEAD,56),CS__,                 CS__,                 CS__,                 CS_N(C,4,MI_SNARE,50),CS_N(C,6,MI_HAT,30),
    CS__,                 CS_N(Cs,5,MI_HARM,34),CS__,                 CS__,                 CS__,                 CS__,
    CS_N(E,5,MI_LEAD,52), CS__,                 CS_N(E,2,MI_BASS,52), CS_N(C,2,MI_KICK,56), CS__,                 CS_N(C,6,MI_HAT,34),
    CS__,                 CS__,                 CS__,                 CS__,                 CS__,                 CS__,
    CS_N(D,5,MI_LEAD,52), CS_N(A,3,MI_HARM,34), CS__,                 CS__,                 CS_N(C,4,MI_SNARE,50),CS_N(C,6,MI_HAT,30),
    CS__,                 CS__,                 CS__,                 CS__,                 CS__,                 CS__,
    CS_N(Cs,5,MI_LEAD,52),CS__,                 CS_N(D,2,MI_BASS,52), CS_N(C,2,MI_KICK,60), CS__,                 CS_N(C,6,MI_HAT,34),
    CS__,                 CS__,                 CS__,                 CS__,                 CS__,                 CS__,
    CS_N(B,4,MI_LEAD,50), CS_N(Fs,3,MI_HARM,34),CS__,                 CS__,                 CS_N(C,4,MI_SNARE,50),CS_N(C,6,MI_HAT,30),
    CS__,                 CS__,                 CS__,                 CS__,                 CS__,                 CS__,
    CS_N(A,4,MI_LEAD,54), CS__,                 CS_N(E,2,MI_BASS,52), CS_N(C,2,MI_KICK,56), CS__,                 CS_N(C,6,MI_HAT,34),
    CS__,                 CS_N(Cs,4,MI_HARM,34),CS__,                 CS__,                 CS__,                 CS__,
    CS__,                 CS__,                 CS__,                 CS__,                 CS_N(C,4,MI_SNARE,50),CS_N(C,6,MI_HAT,30),
    CS__,                 CS__,                 CS__,                 CS__,                 CS__,                 CS__,
};
static const chipseq_cell rust_p1[] = {
/*  lead               harm              bass              kick              snare             hat              */
    CS_N(A,4,MI_LEAD,54), CS__,                 CS_N(A,2,MI_BASS,56), CS_N(C,2,MI_KICK,60), CS__,                 CS_N(C,6,MI_HAT,34),
    CS__,                 CS_N(A,4,MI_HARM,40), CS__,                 CS__,                 CS__,                 CS__,
    CS__,                 CS_N(Cs,5,MI_HARM,40),CS__,                 CS__,                 CS_N(C,4,MI_SNARE,50),CS_N(C,6,MI_HAT,30),
    CS__,                 CS_N(E,5,MI_HARM,40), CS__,                 CS__,                 CS__,                 CS__,
    CS_N(Fs,5,MI_LEAD,54),CS__,                 CS_N(Fs,2,MI_BASS,52),CS_N(C,2,MI_KICK,56), CS__,                 CS_N(C,6,MI_HAT,34),
    CS__,                 CS__,                 CS__,                 CS__,                 CS__,                 CS__,
    CS_N(E,5,MI_LEAD,52), CS_N(Cs,5,MI_HARM,34),CS__,                 CS__,                 CS_N(C,4,MI_SNARE,50),CS_N(C,6,MI_HAT,30),
    CS__,                 CS__,                 CS__,                 CS__,                 CS__,                 CS__,
    CS_N(D,5,MI_LEAD,52), CS_N(A,3,MI_HARM,34), CS_N(D,2,MI_BASS,52), CS_N(C,2,MI_KICK,60), CS__,                 CS_N(C,6,MI_HAT,34),
    CS__,                 CS__,                 CS__,                 CS__,                 CS__,                 CS__,
    CS_N(Cs,5,MI_LEAD,50),CS__,                 CS__,                 CS__,                 CS_N(C,4,MI_SNARE,50),CS_N(C,6,MI_HAT,30),
    CS__,                 CS_N(E,4,MI_HARM,34), CS__,                 CS__,                 CS__,                 CS__,
    CS_N(A,4,MI_LEAD,54), CS__,                 CS_N(E,2,MI_BASS,52), CS_N(C,2,MI_KICK,56), CS__,                 CS_N(C,6,MI_HAT,34),
    CS__,                 CS__,                 CS__,                 CS__,                 CS__,                 CS__,
    CS__,                 CS__,                 CS__,                 CS__,                 CS_N(C,4,MI_SNARE,50),CS_N(C,6,MI_HAT,30),
    CS__,                 CS__,                 CS__,                 CS__,                 CS__,                 CS__,
};
static const chipseq_pattern rust_pats[] = { { rust_p0, 16 }, { rust_p1, 16 } };
static const uint8_t rust_order[] = { 0, 1 };
static const chipseq_song song_rust_flats = {
    .name = "sunward-run",
    .instruments = music_insts, .instrument_count = MUSIC_INST_COUNT,
    .patterns = rust_pats, .pattern_count = 2,
    .order = rust_order, .order_length = 2,
    .loop_order = 0, .channels = 6,
    .rows_per_beat = 4, .ticks_per_row = 6, .bpm_q8 = CHIPSEQ_BPM(150),
};

/* =====================================================================
 *  Boss theme — "The Warden Machine"  (B minor with a locrian tritone, 152 BPM,
 *  ostinato-driven dread).  Motif: a two-chord tension oscillation (tonic vs the
 *  tritone F) under a rising alarm figure on Pulse A that ratchets up a semitone
 *  each cycle, over a relentless low triangle ostinato. (audio-identity §3.14)
 * ===================================================================== */
static const chipseq_cell boss_p0[] = {
/*  lead                harm(tritone stab)  bass              snare             kick            */
    CS_N(Fs,5,MI_LEAD,54), CS__,                CS_N(B,2,MI_BASS,56), CS__,                 CS_N(C,2,MI_KICK,58),
    CS__,                  CS__,                CS_N(B,2,MI_BASS,44), CS__,                 CS__,
    CS_N(G,5,MI_LEAD,52),  CS_N(F,4,MI_HARM,40),CS_N(B,2,MI_BASS,50), CS_N(C,4,MI_SNARE,46),CS__,
    CS__,                  CS__,                CS_N(B,2,MI_BASS,44), CS__,                 CS__,
    CS_N(Fs,5,MI_LEAD,52), CS__,                CS_N(Cs,3,MI_BASS,50),CS__,                 CS_N(C,2,MI_KICK,56),
    CS__,                  CS__,                CS_N(Cs,3,MI_BASS,44),CS__,                 CS__,
    CS_N(As,5,MI_LEAD,52), CS_N(F,4,MI_HARM,40),CS_N(B,2,MI_BASS,50), CS_N(C,4,MI_SNARE,46),CS__,
    CS__,                  CS__,                CS_N(B,2,MI_BASS,44), CS__,                 CS__,
    CS_N(B,5,MI_LEAD,54),  CS__,                CS_N(B,2,MI_BASS,56), CS__,                 CS_N(C,2,MI_KICK,58),
    CS__,                  CS__,                CS_N(B,2,MI_BASS,44), CS__,                 CS__,
    CS_N(C,6,MI_LEAD,52),  CS_N(F,4,MI_HARM,40),CS_N(D,3,MI_BASS,50), CS_N(C,4,MI_SNARE,46),CS__,
    CS__,                  CS__,                CS_N(D,3,MI_BASS,44), CS__,                 CS__,
    CS_N(B,5,MI_LEAD,52),  CS__,                CS_N(B,2,MI_BASS,50), CS__,                 CS_N(C,2,MI_KICK,56),
    CS__,                  CS__,                CS_N(B,2,MI_BASS,44), CS__,                 CS__,
    CS_N(As,5,MI_LEAD,52), CS_N(F,4,MI_HARM,40),CS_N(Fs,2,MI_BASS,50),CS_N(C,4,MI_SNARE,46),CS__,
    CS__,                  CS__,                CS_N(Fs,2,MI_BASS,44),CS__,                 CS__,
};
static const chipseq_pattern boss_pats[] = { { boss_p0, 16 } };
static const uint8_t boss_order[] = { 0 };
static const chipseq_song song_boss = {
    .name = "the-warden-machine",
    .instruments = music_insts, .instrument_count = MUSIC_INST_COUNT,
    .patterns = boss_pats, .pattern_count = 1,
    .order = boss_order, .order_length = 1,
    .loop_order = 0, .channels = 5,
    .rows_per_beat = 4, .ticks_per_row = 6, .bpm_q8 = CHIPSEQ_BPM(152),
};

/* =====================================================================
 *  Level-clear sting — "Vault Sealed"  (biome tonic resolving to the major, a
 *  rising cadential flourish).  Motif: a quoted fragment of the Call Sign (the
 *  fourth->fifth climb, A -> D -> E) capped by an authentic-cadence resolution
 *  to a major triad stab, with a bell ringing the last chord. (audio-identity §3.12)
 * ===================================================================== */
static const chipseq_cell clear_p0[] = {
/*  lead              harm(3rd below)  bass             bell            */
    CS_N(A,4,MI_LEAD,54), CS_N(Fs,4,MI_HARM,40),CS_N(A,2,MI_BASS,54), CS__,
    CS__,                 CS__,                 CS__,                 CS__,
    CS_N(D,5,MI_LEAD,54), CS_N(B,4,MI_HARM,40), CS_N(D,2,MI_BASS,52), CS__,
    CS__,                 CS__,                 CS__,                 CS__,
    CS_N(E,5,MI_LEAD,56), CS_N(Cs,5,MI_HARM,42),CS_N(E,2,MI_BASS,52), CS__,
    CS__,                 CS__,                 CS__,                 CS__,
    CS_N(A,5,MI_LEAD,60), CS_N(Cs,5,MI_HARM,46),CS_N(A,2,MI_BASS,56), CS_N(A,5,MI_BELL,46),
    CS__,                 CS__,                 CS__,                 CS__,
};
static const chipseq_pattern clear_pats[] = { { clear_p0, 8 } };
static const uint8_t clear_order[] = { 0 };
static const chipseq_song song_clear = {
    .name = "vault-sealed",
    .instruments = music_insts, .instrument_count = MUSIC_INST_COUNT,
    .patterns = clear_pats, .pattern_count = 1,
    .order = clear_order, .order_length = 1,
    .loop_order = CHIPSEQ_NO_LOOP, .channels = 4,
    .rows_per_beat = 4, .ticks_per_row = 6, .bpm_q8 = CHIPSEQ_BPM(140),
};

/* =====================================================================
 *  Game-over sting — "Salvage Lost"  (parallel minor, slow and sinking).  Motif:
 *  an inversion of the Call Sign — the lead droops chromatically downward while
 *  the triangle settles onto a low tonic with a ritard, a single soft noise thud
 *  closing it; deflation, defeat, but dignified. (audio-identity §3.13)
 * ===================================================================== */
static const chipseq_cell over_p0[] = {
/*  lead               bass              snare           */
    CS_N(A,4,MI_LEAD,52), CS_N(A,2,MI_BASS,50), CS__,
    CS_N(Gs,4,MI_LEAD,48),CS__,                 CS__,
    CS_N(G,4,MI_LEAD,46), CS__,                 CS__,
    CS_N(Fs,4,MI_LEAD,44),CS_N(E,2,MI_BASS,48), CS__,
    CS_N(F,4,MI_LEAD,42), CS__,                 CS__,
    CS_N(E,4,MI_LEAD,40), CS__,                 CS__,
    CS_N(D,4,MI_LEAD,44), CS_N(A,1,MI_BASS,50), CS_N(C,3,MI_SNARE,40),
    CS__,                 CS__,                 CS__,
};
static const chipseq_pattern over_pats[] = { { over_p0, 8 } };
static const uint8_t over_order[] = { 0 };
static const chipseq_song song_gameover = {
    .name = "salvage-lost",
    .instruments = music_insts, .instrument_count = MUSIC_INST_COUNT,
    .patterns = over_pats, .pattern_count = 1,
    .order = over_order, .order_length = 1,
    .loop_order = CHIPSEQ_NO_LOOP, .channels = 3,
    .rows_per_beat = 2, .ticks_per_row = 6, .bpm_q8 = CHIPSEQ_BPM(80),
};

static const chipseq_song *const music_table[MUS_COUNT] = {
    [MUS_TITLE]      = &song_title,
    [MUS_RUST_FLATS] = &song_rust_flats,
    [MUS_BOSS]       = &song_boss,
    [MUS_CLEAR]      = &song_clear,
    [MUS_GAMEOVER]   = &song_gameover,
};

/* =====================================================================
 *  Sound effects — each a 1/2-channel chip-sequencer song synthesised by
 *  TECHNIQUE (audio-identity §4): voice + envelope step table + optional pitch
 *  sweep + duration.  No samples; all parameter choices are original.
 * ===================================================================== */

/* --- Jump: Pulse A, a fast RISING chirp, duty brightening 25% -> 12.5%. ----- */
static const int8_t sj_vol[]  = { 52,60,58,52,44,36,28,20,12,5,0 };
static const chipseq_seq sj_vol_s = { sj_vol, 11, SEQ_ONCE };
static const int8_t sj_pit[]  = { 0,20,44,70,96,112,112 };          /* glide up ~a fifth then hold */
static const chipseq_seq sj_pit_s = { sj_pit, 7, SEQ_ONCE };
static const int8_t sj_duty[] = { 16,16,12,8 };                     /* 25% -> 12.5% */
static const chipseq_seq sj_duty_s = { sj_duty, 4, SEQ_ONCE };
static const chipseq_instrument sj_inst[] = {
    { .name = "jump", .wave = CHIPSEQ_WAVE_PULSE, .duty = CHIPSEQ_DUTY_25,
      .vol_seq = &sj_vol_s, .pitch_seq = &sj_pit_s, .duty_seq = &sj_duty_s },
};
static const chipseq_cell sj_cells[] = { CS_N(A,4,0,60), CS__, CS__ };
static const chipseq_pattern sj_pats[] = { { sj_cells, 3 } };
static const uint8_t one_order[] = { 0 };
static const chipseq_song song_jump = {
    .name = "sfx-jump", .instruments = sj_inst, .instrument_count = 1,
    .patterns = sj_pats, .pattern_count = 1, .order = one_order, .order_length = 1,
    .loop_order = CHIPSEQ_NO_LOOP, .channels = 1,
    .rows_per_beat = 4, .ticks_per_row = 4, .bpm_q8 = CHIPSEQ_BPM(120),
};

/* --- Land: Noise (dull LONG thud) + a low Triangle tick. -------------------- */
static const int8_t sland_nv[] = { 44,26,12,3,0 };
static const chipseq_seq sland_nv_s = { sland_nv, 5, SEQ_ONCE };
static const int8_t sland_tv[] = { 48,30,14,4,0 };
static const chipseq_seq sland_tv_s = { sland_tv, 5, SEQ_ONCE };
static const chipseq_instrument sland_inst[] = {
    { .name = "thud", .wave = CHIPSEQ_WAVE_NOISE, .noise_mode = CHIPSEQ_NOISE_LONG, .vol_seq = &sland_nv_s },
    { .name = "tick", .wave = CHIPSEQ_WAVE_TRIANGLE, .tri_steps = 32, .vol_seq = &sland_tv_s },
};
static const chipseq_cell sland_cells[] = {
    CS_N(C,3,0,56), CS_N(C,2,1,56),
    CS__,           CS__,
};
static const chipseq_pattern sland_pats[] = { { sland_cells, 2 } };
static const chipseq_song song_land = {
    .name = "sfx-land", .instruments = sland_inst, .instrument_count = 2,
    .patterns = sland_pats, .pattern_count = 1, .order = one_order, .order_length = 1,
    .loop_order = CHIPSEQ_NO_LOOP, .channels = 2,
    .rows_per_beat = 4, .ticks_per_row = 4, .bpm_q8 = CHIPSEQ_BPM(120),
};

/* --- Stomp: a gapped-tone punch — Pulse B alternating full/zero volume over a
 *     short LONG-mode noise transient, with a mid pitch drop for the squash. --- */
static const int8_t sst_nv[] = { 56,30,12,2,0 };
static const chipseq_seq sst_nv_s = { sst_nv, 5, SEQ_ONCE };
static const int8_t sst_pv[] = { 64,0,60,0,48,0,32,0,16,0 };        /* the gapped tone */
static const chipseq_seq sst_pv_s = { sst_pv, 10, SEQ_ONCE };
static const int8_t sst_pp[] = { 0,0,0,0,-24,-40,-56 };             /* squash pitch drop */
static const chipseq_seq sst_pp_s = { sst_pp, 7, SEQ_ONCE };
static const chipseq_instrument sst_inst[] = {
    { .name = "punch", .wave = CHIPSEQ_WAVE_NOISE, .noise_mode = CHIPSEQ_NOISE_LONG, .vol_seq = &sst_nv_s },
    { .name = "gap",   .wave = CHIPSEQ_WAVE_PULSE, .duty = CHIPSEQ_DUTY_50, .vol_seq = &sst_pv_s, .pitch_seq = &sst_pp_s },
};
static const chipseq_cell sst_cells[] = {
    CS_N(C,3,0,60), CS_N(C,4,1,56),
    CS__,           CS__,
    CS__,           CS__,
};
static const chipseq_pattern sst_pats[] = { { sst_cells, 3 } };
static const chipseq_song song_stomp = {
    .name = "sfx-stomp", .instruments = sst_inst, .instrument_count = 2,
    .patterns = sst_pats, .pattern_count = 1, .order = one_order, .order_length = 1,
    .loop_order = CHIPSEQ_NO_LOOP, .channels = 2,
    .rows_per_beat = 4, .ticks_per_row = 4, .bpm_q8 = CHIPSEQ_BPM(120),
};

/* --- Shell-kick: a metallic descending clang (Noise SHORT + a falling pitch). --- */
static const int8_t ssk_v[] = { 60,42,26,14,6,0 };
static const chipseq_seq ssk_v_s = { ssk_v, 6, SEQ_ONCE };
static const int8_t ssk_p[] = { 0,-16,-34,-54,-74 };
static const chipseq_seq ssk_p_s = { ssk_p, 5, SEQ_ONCE };
static const chipseq_instrument ssk_inst[] = {
    { .name = "clang", .wave = CHIPSEQ_WAVE_NOISE, .noise_mode = CHIPSEQ_NOISE_SHORT,
      .vol_seq = &ssk_v_s, .pitch_seq = &ssk_p_s },
};
static const chipseq_cell ssk_cells[] = { CS_N(C,4,0,60), CS__ };
static const chipseq_pattern ssk_pats[] = { { ssk_cells, 2 } };
static const chipseq_song song_shell_kick = {
    .name = "sfx-shell-kick", .instruments = ssk_inst, .instrument_count = 1,
    .patterns = ssk_pats, .pattern_count = 1, .order = one_order, .order_length = 1,
    .loop_order = CHIPSEQ_NO_LOOP, .channels = 1,
    .rows_per_beat = 4, .ticks_per_row = 5, .bpm_q8 = CHIPSEQ_BPM(120),
};

/* --- Phase-bolt: Pulse B, thin duty, a short DOWNWARD sweep. ---------------- */
static const int8_t spb_v[] = { 52,44,32,20,8,0 };
static const chipseq_seq spb_v_s = { spb_v, 6, SEQ_ONCE };
static const int8_t spb_p[] = { 0,-24,-50,-78,-108 };
static const chipseq_seq spb_p_s = { spb_p, 5, SEQ_ONCE };
static const chipseq_instrument spb_inst[] = {
    { .name = "bolt", .wave = CHIPSEQ_WAVE_PULSE, .duty = CHIPSEQ_DUTY_12,
      .vol_seq = &spb_v_s, .pitch_seq = &spb_p_s },
};
static const chipseq_cell spb_cells[] = { CS_N(A,4,0,54), CS__ };
static const chipseq_pattern spb_pats[] = { { spb_cells, 2 } };
static const chipseq_song song_phase_bolt = {
    .name = "sfx-phase-bolt", .instruments = spb_inst, .instrument_count = 1,
    .patterns = spb_pats, .pattern_count = 1, .order = one_order, .order_length = 1,
    .loop_order = CHIPSEQ_NO_LOOP, .channels = 1,
    .rows_per_beat = 4, .ticks_per_row = 4, .bpm_q8 = CHIPSEQ_BPM(120),
};

/* --- Power-up: Pulse A, a table-stepped RISING pentatonic climb. ------------ */
static const int8_t spu_v[] = { 64,60,56,54,54 };
static const chipseq_seq spu_v_s = { spu_v, 5, 4, CHIPSEQ_SEQ_NO_RELEASE };
static const chipseq_instrument spu_inst[] = {
    { .name = "powerup", .wave = CHIPSEQ_WAVE_PULSE, .duty = CHIPSEQ_DUTY_25, .vol_seq = &spu_v_s },
};
static const chipseq_cell spu_cells[] = {
    CS_N(C,5,0,54), CS_N(D,5,0,54), CS_N(E,5,0,54),
    CS_N(G,5,0,56), CS_N(A,5,0,56), CS_N(C,6,0,60),
};
static const chipseq_pattern spu_pats[] = { { spu_cells, 6 } };
static const chipseq_song song_power_up = {
    .name = "sfx-power-up", .instruments = spu_inst, .instrument_count = 1,
    .patterns = spu_pats, .pattern_count = 1, .order = one_order, .order_length = 1,
    .loop_order = CHIPSEQ_NO_LOOP, .channels = 1,
    .rows_per_beat = 8, .ticks_per_row = 4, .bpm_q8 = CHIPSEQ_BPM(150),
};

/* --- Hurt: Pulse B, a STUTTERING descending tone (gapped vol + falling pitch). --- */
static const int8_t shu_v[] = { 56,0,50,0,44,0,36,0,28,0,20,0,12,0,4,0 };
static const chipseq_seq shu_v_s = { shu_v, 16, SEQ_ONCE };
static const int8_t shu_p[] = { 0,-10,-24,-38,-52,-66,-80,-94,-108,-120 };
static const chipseq_seq shu_p_s = { shu_p, 10, SEQ_ONCE };
static const chipseq_instrument shu_inst[] = {
    { .name = "hurt", .wave = CHIPSEQ_WAVE_PULSE, .duty = CHIPSEQ_DUTY_12,
      .vol_seq = &shu_v_s, .pitch_seq = &shu_p_s },
};
static const chipseq_cell shu_cells[] = { CS_N(E,5,0,56), CS__, CS__ };
static const chipseq_pattern shu_pats[] = { { shu_cells, 3 } };
static const chipseq_song song_hurt = {
    .name = "sfx-hurt", .instruments = shu_inst, .instrument_count = 1,
    .patterns = shu_pats, .pattern_count = 1, .order = one_order, .order_length = 1,
    .loop_order = CHIPSEQ_NO_LOOP, .channels = 1,
    .rows_per_beat = 4, .ticks_per_row = 6, .bpm_q8 = CHIPSEQ_BPM(120),
};

/* --- Pickup: Pulse A, a quick 3-note ascending arpeggio. -------------------- */
static const chipseq_instrument spk_inst[] = {
    { .name = "pickup", .wave = CHIPSEQ_WAVE_PULSE, .duty = CHIPSEQ_DUTY_25, .vol_seq = &pluck_vol },
};
static const chipseq_cell spk_cells[] = { CS_N(C,5,0,54), CS_N(E,5,0,54), CS_N(A,5,0,58) };
static const chipseq_pattern spk_pats[] = { { spk_cells, 3 } };
static const chipseq_song song_pickup = {
    .name = "sfx-pickup", .instruments = spk_inst, .instrument_count = 1,
    .patterns = spk_pats, .pattern_count = 1, .order = one_order, .order_length = 1,
    .loop_order = CHIPSEQ_NO_LOOP, .channels = 1,
    .rows_per_beat = 8, .ticks_per_row = 4, .bpm_q8 = CHIPSEQ_BPM(150),
};

/* --- Extra life: Pulse A, a six-note rising jingle resolving up an octave. --- */
static const int8_t sxl_v[] = { 64,60,54,50,50 };
static const chipseq_seq sxl_v_s = { sxl_v, 5, 4, CHIPSEQ_SEQ_NO_RELEASE };
static const chipseq_instrument sxl_inst[] = {
    { .name = "extra-life", .wave = CHIPSEQ_WAVE_PULSE, .duty = CHIPSEQ_DUTY_50, .vol_seq = &sxl_v_s },
};
static const chipseq_cell sxl_cells[] = {
    CS_N(C,5,0,56), CS_N(E,5,0,56), CS_N(G,5,0,56),
    CS_N(C,6,0,58), CS_N(E,6,0,58), CS_N(G,6,0,60),
};
static const chipseq_pattern sxl_pats[] = { { sxl_cells, 6 } };
static const chipseq_song song_extra_life = {
    .name = "sfx-extra-life", .instruments = sxl_inst, .instrument_count = 1,
    .patterns = sxl_pats, .pattern_count = 1, .order = one_order, .order_length = 1,
    .loop_order = CHIPSEQ_NO_LOOP, .channels = 1,
    .rows_per_beat = 8, .ticks_per_row = 4, .bpm_q8 = CHIPSEQ_BPM(140),
};

/* --- Exit-open: Pulse A, a long continuous UPWARD sweep (~1 s), via PORTA_UP. --- */
static const int8_t sxo_v[] = { 36,48,56,60,62,62 };
static const chipseq_seq sxo_v_s = { sxo_v, 6, 5, CHIPSEQ_SEQ_NO_RELEASE };
static const chipseq_instrument sxo_inst[] = {
    { .name = "exit-open", .wave = CHIPSEQ_WAVE_PULSE, .duty = CHIPSEQ_DUTY_25, .vol_seq = &sxo_v_s },
};
static const chipseq_cell sxo_cells[] = {
    CS_NF(C,4,0,58, PORTA_UP,5),
    CS_FX(PORTA_UP,5), CS_FX(PORTA_UP,5), CS_FX(PORTA_UP,5),
    CS_FX(PORTA_UP,5), CS_FX(PORTA_UP,5), CS_FX(PORTA_UP,5), CS_FX(PORTA_UP,5),
};
static const chipseq_pattern sxo_pats[] = { { sxo_cells, 8 } };
static const chipseq_song song_exit_open = {
    .name = "sfx-exit-open", .instruments = sxo_inst, .instrument_count = 1,
    .patterns = sxo_pats, .pattern_count = 1, .order = one_order, .order_length = 1,
    .loop_order = CHIPSEQ_NO_LOOP, .channels = 1,
    .rows_per_beat = 4, .ticks_per_row = 8, .bpm_q8 = CHIPSEQ_BPM(120),
};

/* --- Boss-hit: a low two-stage DOWNWARD blast — Pulse B (two-stage pitch) with
 *     a SHORT-mode noise crack on the impact frame. --------------------------- */
static const int8_t sbh_v[]  = { 56,50,44,38,30,22,14,6,0 };
static const chipseq_seq sbh_v_s = { sbh_v, 9, SEQ_ONCE };
static const int8_t sbh_p[]  = { 0,-14,-28,-42,-84,-96,-108,-120 };  /* first sweep, then a lower stage */
static const chipseq_seq sbh_p_s = { sbh_p, 8, SEQ_ONCE };
static const int8_t sbh_nv[] = { 60,26,6,0 };
static const chipseq_seq sbh_nv_s = { sbh_nv, 4, SEQ_ONCE };
static const chipseq_instrument sbh_inst[] = {
    { .name = "blast", .wave = CHIPSEQ_WAVE_PULSE, .duty = CHIPSEQ_DUTY_12,
      .vol_seq = &sbh_v_s, .pitch_seq = &sbh_p_s },
    { .name = "crack", .wave = CHIPSEQ_WAVE_NOISE, .noise_mode = CHIPSEQ_NOISE_SHORT, .vol_seq = &sbh_nv_s },
};
static const chipseq_cell sbh_cells[] = {
    CS_N(A,3,0,58), CS_N(C,4,1,60),
    CS__,           CS__,
    CS__,           CS__,
};
static const chipseq_pattern sbh_pats[] = { { sbh_cells, 3 } };
static const chipseq_song song_boss_hit = {
    .name = "sfx-boss-hit", .instruments = sbh_inst, .instrument_count = 2,
    .patterns = sbh_pats, .pattern_count = 1, .order = one_order, .order_length = 1,
    .loop_order = CHIPSEQ_NO_LOOP, .channels = 2,
    .rows_per_beat = 4, .ticks_per_row = 5, .bpm_q8 = CHIPSEQ_BPM(120),
};

/* --- Jet loop (held thruster): a looping LONG-mode hiss + a low Triangle
 *     rumble, revved live by sound_jet -> chipseq_sfx_set. ------------------- */
static const int8_t sjet_nv[] = { 26,30,32,31,30,32 };
static const chipseq_seq sjet_nv_s = { sjet_nv, 6, 0, CHIPSEQ_SEQ_NO_RELEASE };
static const int8_t sjet_tv[] = { 40,44,44,42,44 };
static const chipseq_seq sjet_tv_s = { sjet_tv, 5, 0, CHIPSEQ_SEQ_NO_RELEASE };
static const chipseq_instrument sjet_inst[] = {
    { .name = "hiss",   .wave = CHIPSEQ_WAVE_NOISE, .noise_mode = CHIPSEQ_NOISE_LONG, .vol_seq = &sjet_nv_s },
    { .name = "rumble", .wave = CHIPSEQ_WAVE_TRIANGLE, .tri_steps = 32, .vol_seq = &sjet_tv_s },
};
static const chipseq_cell sjet_cells[] = {
    CS_N(A,5,0,44), CS_N(A,2,1,50),
    CS__,           CS__,
    CS__,           CS__,
    CS__,           CS__,
};
static const chipseq_pattern sjet_pats[] = { { sjet_cells, 4 } };
static const chipseq_song song_jet = {
    .name = "sfx-jet", .instruments = sjet_inst, .instrument_count = 2,
    .patterns = sjet_pats, .pattern_count = 1, .order = one_order, .order_length = 1,
    .loop_order = 0, .channels = 2,
    .rows_per_beat = 4, .ticks_per_row = 6, .bpm_q8 = CHIPSEQ_BPM(120),
};

static const chipseq_song *const sfx_table[SFX_COUNT] = {
    [SFX_JUMP]       = &song_jump,
    [SFX_LAND]       = &song_land,
    [SFX_STOMP]      = &song_stomp,
    [SFX_SHELL_KICK] = &song_shell_kick,
    [SFX_PHASE_BOLT] = &song_phase_bolt,
    [SFX_POWER_UP]   = &song_power_up,
    [SFX_HURT]       = &song_hurt,
    [SFX_PICKUP]     = &song_pickup,
    [SFX_EXTRA_LIFE] = &song_extra_life,
    [SFX_EXIT_OPEN]  = &song_exit_open,
    [SFX_BOSS_HIT]   = &song_boss_hit,
    [SFX_JET]        = &song_jet,
};

/* =====================================================================
 *  Transport (the pcm-mixer seam + the game-thread control surface)
 * ===================================================================== */

static chipseq seq;
static pcmmix  mixer;
static bool    started;
static bool    enabled = true;
static int     jet_handle = -1;
static int     cur_music = MUS_NONE;

/* Ratio-pitch (the sound_play API's units) -> chip-sequencer semitone transpose.
 * Runs on the game thread only (never the render path), so libm is fine here. */
static int pitch_semitones(float pitch)
{
    if (pitch <= 0.0f || pitch == 1.0f) return 0;
    return (int)lrintf(12.0f * log2f(pitch));
}

bool sound_init(void)
{
    if (started) return true;

    chipseq_options copts;
    chipseq_options_init(&copts);
    copts.sample_rate = SOUND_RATE;      /* MUST equal the mixer rate */
    copts.sfx_duck = 0.68f;              /* duck the live music while any SFX sounds (§1.4) */
    if (!chipseq_init(&seq, &copts)) return false;

    pcmmix_options mopts;
    pcmmix_options_init(&mopts);
    mopts.sample_rate = SOUND_RATE;
    if (!pcmmix_start(&mixer, &mopts)) {   /* no sink survived the probe: silent fallback */
        chipseq_shutdown(&seq);
        return false;
    }

    pcmmix_set_generator(&mixer, chipseq_generator, &seq);   /* the whole seam */
    started = true;
    cur_music = MUS_NONE;
    jet_handle = -1;
    chipseq_set_enabled(&seq, enabled);
    pcmmix_set_enabled(&mixer, enabled);
    return true;
}

void sound_shutdown(void)
{
    if (started) {
        pcmmix_stop(&mixer);       /* join the mixer thread FIRST (generator can no longer run) */
        chipseq_shutdown(&seq);    /* then reset the source */
    }
    started = false;
    jet_handle = -1;
    cur_music = MUS_NONE;
}

void sound_set_enabled(bool on)
{
    enabled = on;
    if (!started) return;
    if (!on && jet_handle > 0) { chipseq_sfx_stop(&seq, jet_handle); jet_handle = -1; }
    chipseq_set_enabled(&seq, on);
    pcmmix_set_enabled(&mixer, on);
}

bool sound_is_enabled(void)
{
    return enabled;
}

void sound_play(int id, float volume, float pitch)
{
    if (!started || !enabled) return;
    if (id < 0 || id >= SFX_COUNT || id == SFX_JET) return;   /* JET is loop-only (via sound_jet) */
    (void)chipseq_sfx_play(&seq, sfx_table[id], volume, pitch_semitones(pitch), false);
}

void sound_jet(bool active, float intensity)
{
    if (!started || !enabled) return;
    if (!active) {
        if (jet_handle > 0) chipseq_sfx_stop(&seq, jet_handle);
        jet_handle = -1;
        return;
    }
    float vol = 0.10f + clampf(intensity, 0.0f, 1.0f) * 0.16f;
    int transpose = (int)(clampf(intensity, 0.0f, 1.0f) * 5.0f);   /* revving raises the hiss */
    if (jet_handle > 0 && chipseq_sfx_active(&seq, jet_handle)) {
        chipseq_sfx_set(&seq, jet_handle, vol, transpose);
        return;
    }
    jet_handle = chipseq_sfx_play(&seq, &song_jet, vol, transpose, true);
}

void sound_music(int track)
{
    if (!started || !enabled) return;
    if (track == cur_music) return;                 /* dedup: safe to drive every frame */
    cur_music = track;
    if (track < 0 || track >= MUS_COUNT) { chipseq_music_stop(&seq, 8); return; }
    bool loop = (track == MUS_TITLE || track == MUS_RUST_FLATS || track == MUS_BOSS);
    (void)chipseq_music_play(&seq, music_table[track], loop, NULL, 0);
}

/* Offline byte-determinism + validation gate (no sink needed).  Validates every
 * SFX and music song, then renders a fixed span of the RUST FLATS bed twice
 * through the offline bounce path and asserts the two are byte-identical. */
bool sound_render_selfcheck(void)
{
    char err[128];
    for (int i = 0; i < SFX_COUNT; i++)
        if (!chipseq_song_validate(sfx_table[i], err, sizeof err)) return false;
    for (int i = 0; i < MUS_COUNT; i++)
        if (!chipseq_song_validate(music_table[i], err, sizeof err)) return false;

    chipseq_options opt;
    chipseq_options_init(&opt);
    opt.sample_rate = SOUND_RATE;
    const uint64_t span = 8192;                     /* a fixed span of the looping bed */
    size_t n0 = 0, n1 = 0;
    int16_t *a = chipseq_render_song(&song_rust_flats, &opt, span, &n0, err, sizeof err);
    int16_t *b = chipseq_render_song(&song_rust_flats, &opt, span, &n1, err, sizeof err);
    bool ok = a && b && n0 == n1 && n0 > 0 &&
              memcmp(a, b, n0 * sizeof *a) == 0;
    chipseq_pcm_free(a);
    chipseq_pcm_free(b);
    return ok;
}

/* A short blocking gap so a human running --sound-test with a live sink hears
 * each cue separately.  Only ever reached when a sink is present (started). */
static void nap(long ms)
{
    struct timespec ts = { ms / 1000, (ms % 1000) * 1000000L };
    nanosleep(&ts, NULL);
}

void sound_selftest_play(void)
{
    if (!started) return;                           /* no sink: nothing to play */
    for (int m = 0; m < MUS_COUNT; m++) { sound_music(m); nap(900); }
    sound_music(MUS_NONE);
    nap(300);
    for (int s = 0; s < SFX_COUNT; s++) {
        if (s == SFX_JET) {
            sound_jet(true, 0.7f); nap(500); sound_jet(false, 0.0f);
        } else {
            sound_play(s, 0.85f, 1.0f);
        }
        nap(400);
    }
}

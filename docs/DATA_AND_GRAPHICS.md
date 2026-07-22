# Data, graphics, and audio provenance

This document catalogs where every byte of Super Kilix's content comes from. It
is a stub at M0 and is completed as each subsystem lands; the originality claim
it supports is stated in full in `originality.md`.

## Graphics

Super Kilix ships **no image assets**. Every character, tile, background, and
effect is drawn at runtime from the soft-raster primitive set (rectangles,
outlines, circles, ellipses, lines, triangles, and a built-in bitmap font).
Kilix himself is composed from these primitives — an orange body, cyan visor and
magnetic boots, a violet micro-thruster, and an animated tail — so he is drawn
identically to his appearance in the sibling salvage game, with no shared image
data.

## Data

Level content is generated deterministically as a pure function of a vault index
(no external files, no random assets). The generator, its encoding, and its
validators are documented here as they are implemented.

## Audio

Super Kilix ships **no audio assets** — no samples, no recordings, no imported
tracker modules. Every note is an **original composition**, and every sound is
synthesised in memory at runtime from original data tables. Nothing is
transcribed, quoted, or lightly altered from any other work; only the synthesis
*technique* (duty-pulse leads, an octave-down triangle bass, gapped-noise
percussion, table-stepped pitch sweeps) is reused, which is non-copyrightable.

### Synthesis path

`src/sound.c` is the only module that includes the audio headers. It pairs two
vendored libraries:

- **chip-sequencer** — the deterministic *source*: it bakes compact in-source
  song data (patterns of note cells + table-driven instruments) into PCM with a
  libm-free fixed-point chip synth (pulse / triangle / noise / wavetable). Its
  render path contains no transcendentals, so a given `(song, options, rate)` is
  byte-identical forever. Vendored as `third_party/chip-sequencer`, compiled into
  the binary by `src/vendor_chip_sequencer.o` with the game's own flags (which
  carry `-ffp-contract=off`), exactly like the kit's transport libs — never as a
  shared/static archive.
- **pcm-mixer** (from `kilix-game-kit`) — the *transport*: device, mixer thread,
  and voice model. The whole seam is one function pointer,
  `pcmmix_set_generator(&mixer, chipseq_generator, &seq)`.

Audio is optional and never fatal: if no sink is found, the game runs silently
and `--sound-test` still returns `0`.

### Track catalog

Five original songs, each realising a key / tempo / mood / instrumentation brief:

| Track | Role | Key · tempo |
|---|---|---|
| "Vault Reveille" | title theme (the Call Sign leitmotif) | D dorian · 132 |
| "Sunward Run" | RUST FLATS district-1 gameplay bed | A mixolydian · 150 |
| "The Warden Machine" | Vault-Guardian / boss theme | B minor (tritone) · 152 |
| "Vault Sealed" | level-clear sting | biome tonic → major · 140 |
| "Salvage Lost" | game-over sting | parallel minor · 80 |

Gameplay beds run **live** through the sequencer so the global SFX duck lowers
the music while an effect sounds.

### Effect catalog

Twelve effects, each a 1–2-channel song synthesised by technique and indexed by
the `SFX_*` enum: jump, land, stomp, shell-kick, phase-bolt, power-up, hurt,
pickup, extra-life, exit-open, boss-hit, and the held jet drone (loop-controlled
live). They are wired to the M2–M4 gameplay events (jump/land in the physics;
stomp/kick/hurt/power-up/pickup/boss-hit/exit in the interaction code).

### Determinism

`--sound-test` first runs an offline gate (`sound_render_selfcheck`) that
validates every song and effect and renders a fixed span of the RUST FLATS bed
**twice** through the offline bounce path, asserting the two are byte-identical.
This runs with or without an audio sink and is exercised by both `make test` and
`make sanitize`.

## Verification gates

- `make clean-room-check` — no reference-format asset files; no forbidden tokens.
- `make test` — CLI hygiene, deterministic self-test, and (in later milestones)
  render-purity and audio-determinism checks.
- `make sanitize` — a clean AddressSanitizer + UndefinedBehaviorSanitizer pass.

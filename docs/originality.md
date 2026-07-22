# Originality and clean-room posture

Super Kilix is an original side-scrolling platformer in a classic 8-bit lineage,
unaffiliated with and not endorsed by any prior game or its publisher. It copies
no code, art, level, text, or audio from any existing work. It shares only
non-copyrightable ideas — the shape of a genre — re-expressed entirely in
Kilix's own established universe.

## What is original

- **All player-visible identity.** District names, machine names, pickup names,
  the wordmark, and every line of menu and help prose are newly authored for
  this game.
- **All art.** Every sprite, tile, and effect is drawn at runtime from
  soft-raster primitives. The repository ships zero bitmap, palette, or
  generative-image assets.
- **All audio.** Every piece of music and every sound effect is synthesised in
  memory from original compositions. No melody is transcribed from any prior
  work.
- **All physics constants and level layouts** are derived from this game's own
  design intent.

## How the boundary is enforced mechanically

`make clean-room-check` runs as a prerequisite of `make test` and applies two
compiler-free rules:

1. **No reference-format asset files** — a tree scan (excluding `.git` and
   `third_party`) fails the build if any emulator ROM, music dump, character
   sheet, program bank, palette, or movie-capture file is present.
2. **No forbidden branding tokens** — a case-insensitive search across the
   source, docs, and README fails the build if any prior game's title,
   character name, or publisher brand appears, and likewise for tooling-vendor
   strings. The exact token set lives only in the build system, so this document
   can state the rule without naming anything it forbids.

## Vendored libraries

Super Kilix links the shared `kilix-game-kit` library (terminal session,
soft-raster, PCM mixer, state store, and the fixed-step clock) and the
`chip-sequencer` chiptune-synth library (the deterministic audio *source* whose
render path is libm-free). Both are vendored under `third_party/` and compiled
into the binary; vendored libraries retain their own notices and licences and
are never edited in place.

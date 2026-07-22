# Originality and clean-room posture

Super Kilix is an original side-scrolling platformer in a classic 8-bit lineage,
unaffiliated with and not endorsed by any prior game or its publisher. It copies
no code, art, level, text, or audio from any existing work. It shares only
non-copyrightable ideas — the shape of a genre and the measured behaviour of a
platformer — re-expressed entirely in Kilix's own established universe, the
Driftway.

## What is original

Everything a player sees, hears, or plays through is newly authored for this
game:

- **All player-visible identity.** The eight district names, the 32 vault names,
  every guardian-machine name, the pickup and power-tier names, the wordmark, and
  every line of menu, HUD, and field-manual prose are original to this game and
  its universe.
- **All art.** Every character, tile, background motif, effect, and HUD element
  is drawn at runtime from soft-raster primitives — rectangles, outlines,
  circles, ellipses, lines, triangles, and a built-in bitmap font. Kilix himself
  is composed from these primitives and animated by the same gait model as his
  sibling salvage game, sharing that established character but no image data.
- **All audio.** Every song and every effect is an original composition
  synthesised in memory from original data tables. No melody, rhythm, or timbre
  is transcribed, quoted, or lightly altered from any prior work.
- **All level layouts and physics constants** are derived from this game's own
  design intent. The genre's measured curves are studied as a target *feel*; none
  of its raw numeric register values ship.

## What is explicitly excluded

The repository contains, and the shipped binary loads, **none** of the
following:

- No prior game's executable, ROM image, program bank, or character sheet.
- No prior game's level layout, byte stream, or map data.
- No prior game's sprite, tile, palette, or any bitmap traced or derived from it.
- No prior game's music, sound dump, or tracker module, and no transcribed
  melody.
- No prior game's names, characters, wordmark, or menu/manual prose.
- No generative-image model output, no external recording, and no screenshot of
  any other work.

There are no asset files of any kind: nothing is loaded from disk at runtime
except the versioned progress profile the game itself writes.

## Graphics catalog

All game-specific graphics are procedural, drawn each frame from primitives:

- **Kilix** — orange body and dark-orange fur pair, pink inner ears, a dark-teal
  visor with a cyan glint, cyan magnetic boots, and a violet micro-thruster,
  animated by a persistent gait phase (body bob, counter-swinging arms,
  alternating boot lift, a slower higher-amplitude tail), plus the phase-shell
  silhouette layers for the Plated and Charged tiers and the Aegis shield.
- **Guardian machines** — each family drawn from primitives with a distinct
  silhouette and a non-lethal activation tell: ground walkers, a shelled turner
  and its kickable retracted hull, a wall-vent emerger, an arc-lobbing thrower,
  and the Vault Guardian and Overseer bosses with an exposed core.
- **The vault world** — per-district terrain, background motifs, star-fields,
  pillars, riser rails, gate irises, blocks, and hazards, all composed from the
  primitive set and camera-scrolled with clipping.
- **Presentation** — the two-row HUD, the title screen and wordmark, the vault
  selector, pause, the three field-manual pages, and the clear/life-lost/
  game-over/victory scenes, all rendered from primitives and the built-in font.

## Audio catalog

All audio is synthesised at runtime; the game ships no samples, recordings, or
imported modules. Twelve original songs (a title theme, one bed per district, a
boss theme, and clear and game-over stings) and a thirteen-role effect set are
authored as compact in-source data and rendered by a deterministic, libm-free
chip synth. Only the synthesis *technique* is reused (duty-pulse leads, an
octave-down triangle bass, gapped-noise percussion, table-stepped pitch sweeps),
which is non-copyrightable; every note choice is original. The full track and
effect catalog is in [DATA_AND_GRAPHICS.md](DATA_AND_GRAPHICS.md).

## How the boundary is enforced mechanically

`make clean-room-check` runs as a prerequisite of `make test` and applies two
compiler-free rules:

1. **No reference-format asset files.** A tree scan (excluding `.git` and
   `third_party`) fails the build if any emulator ROM, music dump, character
   sheet, program bank, palette, or movie-capture file is present. The exact
   extension set is listed in [DATA_AND_GRAPHICS.md](DATA_AND_GRAPHICS.md) and
   the `Makefile`.
2. **No forbidden tokens.** A case-insensitive search across the source, docs,
   and `README.md` fails the build if any prior game's title, character name, or
   publisher brand appears, and likewise for tooling-vendor strings. Because the
   search covers the docs and README as well as the source, this document and all
   shipped prose state the clean-room claim *generically* and name nothing they
   forbid. The exact token set lives only in the build system.

## Verification gates

The originality and integrity claims are backed by mechanical gates:

- `make clean-room-check` — no reference-format asset files; no forbidden tokens
  anywhere in `src`, `docs`, or `README.md`.
- `make test` — CLI hygiene and arity checks; the deterministic whole-campaign
  self-test with byte-for-byte reproduction; render-purity (`memcmp` of
  `GameState` around every scene); exact render-scene count and PPM validity; the
  walk-stride distinctness signature; and the offline audio-determinism gate.
- `make sanitize` — the rules, input, self-test, sound, and render paths under a
  clean AddressSanitizer + UndefinedBehaviorSanitizer pass.

## Vendored libraries

Super Kilix links the shared `kilix-game-kit` library (the Kitty terminal
session, the `soft-raster` rasteriser and its public-domain console font, the
`pcm-mixer` transport, a versioned state store, and the fixed-step clock) and the
`chip-sequencer` chiptune-synth library (the deterministic audio *source* whose
render path is libm-free). Both are vendored under `third_party/`, compiled into
the binary, and never edited in place; each retains its own notices and licence.

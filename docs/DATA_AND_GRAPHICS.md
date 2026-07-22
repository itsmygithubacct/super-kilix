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

Super Kilix ships **no audio assets**. All music and sound effects are
synthesised in memory from original compositions compiled into the binary. The
synthesis path and the song catalog are documented here once the audio
subsystem lands.

## Verification gates

- `make clean-room-check` — no reference-format asset files; no forbidden tokens.
- `make test` — CLI hygiene, deterministic self-test, and (in later milestones)
  render-purity and audio-determinism checks.
- `make sanitize` — a clean AddressSanitizer + UndefinedBehaviorSanitizer pass.

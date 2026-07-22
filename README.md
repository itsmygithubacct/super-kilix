# Super Kilix

A side-scrolling platformer starring **Kilix**, the orange star-vault salvager,
built for Kitty-graphics terminals. Kilix runs, jumps, and boosts his
micro-thruster across a long horizontal journey through the derelict star-vaults
of his own machine world — hull terraces, coolant galleries, and forge-forts —
chasing star-motes and outrunning the machines that guard them.

Like its sibling games in the Kilix family, Super Kilix is ISO C11 with **no
asset files**: every sprite is drawn at runtime from soft-raster primitives and
every sound is synthesised in memory. Nothing but the vendored libraries,
libc, zlib, libm, and pthread is required at runtime.

> **Status:** playable district-1 slice with audio (M5b). The repository builds
> a single binary from the six-module skeleton, vendors the shared
> `kilix-game-kit` library and the `chip-sequencer` chiptune synth, plays a
> RUST FLATS level with the full machine roster and the phase-shell power-up
> ladder, and drives an original synthesised soundtrack + effect set. It passes
> the command-line hygiene, clean-room, deterministic self-test, render-purity,
> and audio-determinism gates. Later districts and the profile land in M6+.

## Originality and clean-room boundary

Super Kilix is an original side-scrolling platformer in a classic 8-bit lineage,
unaffiliated with and not endorsed by any prior game or its publisher. It copies
no code, art, level, text, or audio from any existing work. Every player-visible
name — Kilix's machines, districts, items, the wordmark, and all menu and help
prose — is newly authored for this game and its established universe. The build
enforces this mechanically: `make clean-room-check` (a prerequisite of
`make test`) fails if any reference-format asset file appears anywhere in the
tree, or if any forbidden branding token appears anywhere in the source or docs.

## Build and run

```
# chip-sequencer (the audio-source library) is vendored as a RELATIVE-path
# submodule from a sibling local checkout until it is published, so submodule
# commands need git's file-protocol opt-in during local development:
git -c protocol.file.allow=always submodule update --init --recursive
make
./super-kilix                 # play in a Kitty-protocol terminal
```

Once `chip-sequencer` is published its `.gitmodules` URL becomes the public
`https` remote and the `-c protocol.file.allow=always` flag is no longer needed;
`kilix-game-kit` already uses its public remote today.

## Development and verification

```
make test        # clean-room check, CLI hygiene, and the deterministic self-test
make test-fast   # the short local loop
make sanitize    # rebuild under AddressSanitizer + UndefinedBehaviorSanitizer
```

Headless modes (no terminal required):

| Mode | Purpose |
|---|---|
| `--selftest [seed] [ticks]` | deterministic simulation run; prints `PASS ...` |
| `--rules-test` | simulation-rule fixtures |
| `--input-test` | input-funnel fixtures |
| `--render-test [seed]` | scene renders + purity check |
| `--sound-test` | exercise every sound; never fatal without a sink |
| `--dump-level N` | inspect a vault's layout |
| `--version` / `--help` | version string / usage |

## License

MIT (see `LICENSE`). Vendored libraries retain their own notices.

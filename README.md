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

> **Status:** early skeleton (M0). The repository builds a single binary from
> the six-module skeleton, vendors the shared `kilix-game-kit` library, and
> passes the command-line hygiene, clean-room, and trivial self-test gates.
> Gameplay, rendering, and audio land in later milestones.

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
git submodule update --init --recursive
make
./super-kilix                 # play in a Kitty-protocol terminal
```

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

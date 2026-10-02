# Lineage

This repository is a **fork** of the KiriKiri Z SDL2 port (`krkrsdl2`), used as
the KiriKiri engine behind the libretro port of *The House in Fata Morgana* and
related titles.

| | |
|---|---|
| Upstream | https://github.com/krkrsdl2/krkrsdl2  (git remote `upstream`) |
| Fork | https://gitlab.com/matchaxnb/krkrsdl2 (git remote `origin`) |
| Branch | `main` |
| Fork point | `124bdf8` — "cleanup: 0/nullptr/NULL comparison and control flow cleanups" |

The fork point is the common ancestor with `upstream/HEAD` at the time of
writing; `git merge-base HEAD upstream/HEAD` reproduces it.

## Local commits on top of the fork point

```
f7555eb  krkrsdl2: SDL2/libretro port fixes for KiriKiri games
d63aa47  Application: pace the engine main loop (fix unbounded frame rate)
efe5c7c  build: link system FreeType instead of vendored 2.6.3
69d0d9a  build: OPTION_DEBUG_PROBES to enable the ad-hoc stderr probes (default OFF)
08c2382  krkrz: bump submodule (probes gated behind TVP_DEBUG_PROBES)
9882776  libretro: composite modal windows into the primary frame
```

## Submodules and their forks

This repo's `external/krkrz` and `external/FAudio` are themselves forks; see
their own `LINEAGE.md`:

- `external/krkrz`  → fork of https://github.com/krkrsdl2/krkrz
- `external/FAudio` → fork of https://github.com/FNA-XNA/FAudio

## Consumers

`onscripter-en-libretro` (https://gitlab.com/matchaxnb/onscripter-en-libretro)
vendors this repo (and the two submodule forks above) as the KiriKiri engine for
its libretro core.

## License

Inherited from upstream — see `LICENSE`.

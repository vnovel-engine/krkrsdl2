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

Recorded in [`CHANGELOG.md`](CHANGELOG.md) (by subject, with rationale) rather
than by hash: our commit ids go stale on any history rewrite, and upstream is
the only lineage that needs a fixed point. `git log 124bdf8..HEAD` is
authoritative for the current list.

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

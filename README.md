# Speed Demon 2026 - Coder Hunterz

C++ entries for all four tracks (A Ingest, B Index, C Join, D Traverse). No dependencies: standard library, pthreads, AVX2 (`-march=x86-64-v3`, never `-march=native`, so the binary runs on the scoring CPU whatever machine builds it).

Each `track-*` folder is built on its own: `main.cpp`, a two-stage `Dockerfile` (allowlisted `gcc:15` builds a static binary, `debian:13-slim` runs it), `.speed-demon.toml`, `PERF.md` and `README.md`.

All four reproduce the published seed-1 output hashes, and match the Python references on other seeds and scales, with 1 to 32 threads.

## Before code freeze (Nov 2, 15:00 UTC)

1. Push to the `speed-demon` branch with the Speed Demon Board app installed; copy the board run IDs into the last column of each `PERF.md`.
2. Submit the team form with the repo URL and all four tracks.
3. Make the repo public under an OSI licence (MIT is already here) within one hour after freeze.

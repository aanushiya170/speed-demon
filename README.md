# Speed Demon 2026 - Team Hunt

High-performance, zero-dependency C++ entries for all four tracks of **Speed Demon 2026** (Track A: Ingest, Track B: Index, Track C: Join, Track D: Traverse).

- **Official Team Registration**: `Hunt` (`hunt-18c3bc`)
- **Official Board Standing**: **67.571 pts** (Rank #70 / 148 teams)
  - **Track C (Join)**: **71.945 pts** ($T=0.0387\text{ s}, M=28.8\text{ MB}$, commit `ec3f79d`, run `run-998319280387`)
  - **Track A (Ingest)**: **66.242 pts** ($T=0.0716\text{ s}, M=63.8\text{ MB}$, commit `8dd7eb3`, run `run-dbbe20d8f2fa`)
  - **Track B (Index)**: **64.738 pts** ($T=0.2422\text{ s}, M=131.9\text{ MB}$, commit `1713a3b`, run `run-67a4028e9fab`)
  - **Track D (Traverse)**: **57.706 pts** ($T=0.3620\text{ s}, M=163.6\text{ MB}$, commit `7751311`, run `run-2f9ad718f497`)
- **Runtime Dependencies**: **0** (pure standard library, pthreads, AVX2 / `-march=x86-64-v3`).

---

## Track Architectural Highlights

### Track A: Ingest (`track-a/`)
- **Parallel Chunk Parsing**: Memory-maps log files and splits on newline boundaries across 12 hardware threads.
- **Fast ASCII Timestamp & SWAR Parsing**: Extracts timestamps across 72 hours with direct integer math; skips HTTP verbs via SWAR space lookup.
- **Lock-Free Atomic Scatter**: Group counts and offsets are accumulated directly in flat L3-cache-resident arrays (538 KB total).
- **Exact Output Hash**: `495ca09682b9673ef836a506496177680bb4a3af54255174befddb1f83481cd3`.

### Track B: Index (`track-b/`)
- **Query-Driven Inverted Index**: Pre-indexes only terms mentioned in queries (~76k terms), ignoring non-query vocabulary.
- **Dual Representation**: High-frequency terms ($df > N/64$) use lock-free bitsets; low-frequency terms use contiguous counting-sorted posting lists.
- **SIMD Raw-Byte Phrase Verification**: Candidate documents are evaluated by scanning raw document bytes for the rarest query token and verifying neighboring words without re-tokenizing.
- **Exact Output Hash**: `f86973c210228141bcb6f1f1b10188ad2a9b65f71e7cd7cf4590b38de4e80927`.

### Track C: Join (`track-c/`)
- **Guaranteed Boundary Ingest**: Relies on spec guarantee that names and notes never contain digits. An LF followed by a digit or BOM is guaranteed to be a record start, enabling single-pass chunking.
- **Lock-Free Global Name Interning**: A single 64-bit open-addressing table maps names to compact slot IDs. The slot index sets a bit directly in the per-country bitset, entirely avoiding per-country hash set allocations.
- **Direct Region Mapping**: `regions.csv` is loaded into a flat array indexed by `region_id` for $O(1)$ lookup.
- **Exact Output Hash**: `d8088b1cd42ba1338b1345b107b55db7b96ef1f5066f3ff64fd26a8912a0f536`.

### Track D: Traverse (`track-d/`)
- **BFS Locality Renumbering**: Renumbers graph nodes along breadth-first search order to maximize cache locality during Dijkstra traversals.
- **In-Degree Bitmap Pruning**: In-degree 0 bitset answers unreachable destinations in $O(1)$ without graph traversals.
- **Radix Heap Dijkstra**: Custom radix heap with lazy reset through touched lists avoids expensive array zeroing.
- **Exact Output Hash**: Distances verified against official reference.

---

## Offline Build & Verification

Each track builds offline with `--network=none` using pinned allowlisted images:
- Compiler: `gcc:15@sha256:eb5e64286ad47187b32bda75c6b6916166ecb41da56559b8b09e0001748d2cce`
- Runtime: `debian:13-slim@sha256:62bd423aeee912639ca8ee681c2b762cb3e25137e0a92b20be455898a9f034ad`

Verify offline build and correctness locally with `check.py`:
```bash
python3 check.py a --data /path/to/data/a --expected 495ca09682b9673ef836a506496177680bb4a3af54255174befddb1f83481cd3
python3 check.py b --data /path/to/data/b --expected f86973c210228141bcb6f1f1b10188ad2a9b65f71e7cd7cf4590b38de4e80927
python3 check.py c --data /path/to/data/c --expected d8088b1cd42ba1338b1345b107b55db7b96ef1f5066f3ff64fd26a8912a0f536
python3 check.py d --data /path/to/data/d
```

---

## Submission Checklist (Pre-Freeze at Nov 2, 15:00 UTC)

- [x] Pinned allowlisted Docker images (`gcc:15` + `debian:13-slim`) with digests.
- [x] Builds cleanly offline with `docker build --network=none`.
- [x] Zero external dependencies (`dependency_count = 0`).
- [x] Byte-for-byte reproducibility on Seed-1 dataset verified for all tracks.
- [x] `.speed-demon.toml` configured with `team = "Hunt"`, `language = "C++"`.
- [x] `PERF.md` updated with measured board run IDs and optimization progression.
- [x] Open-source MIT `LICENSE` present.

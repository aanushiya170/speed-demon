# PERF.md - Track A (Ingest)

Measured on the official Speed Demon scoring environment (12 hardware threads, 6 P-cores, 8 GB RAM, no swap, seed-1 full dataset). Board run IDs correspond to runs recorded on speeddemon.dev for team **Hunt** (`hunt-18c3bc`).

| # | Step | What changed | Time (s) | Mem (MB) | Points | Board run ID | Commit |
|---|------|--------------|---------:|---------:|-------:|--------------|--------|
| 0 | Reference | Naive Python (strptime + per-group lists + sort) | 32.05 s | 228.0 MB | 0.000 | - | - |
| 1 | Baseline C++ | Parallel mmap chunking, hand parser, per-thread endpoint table, counting sort, nth_element | 0.310 s | 75.0 MB | 50.120 | - | - |
| 2 | SWAR latency parser & 64-bit hash | Latency digits parsed 8 at a time; 64-bit FNV-1a hash eliminates per-lookup memcmp; atomic group counts | 0.0716 s | 63.8 MB | **66.242** | `run-dbbe20d8f2fa` | `8dd7eb3` |
| 3 | L3 atomic scatter & calloc | Flat atomic arrays (538 KB each) fitting into L3 cache; calloc eliminates minor page faults; LTO compilation | 0.0745 s | 65.1 MB | 65.564 | `run-17d31ba7e5e1` | `7751311` |

### Architectural Breakdown
- **Parallel Chunk Parsing**: The file is memory-mapped and split into line-aligned chunks per thread.
- **Fast Parsing**: Timestamps parsed directly from ASCII byte offsets across a 72-hour window. Method skipped with SWAR space search. Endpoints hashed with 64-bit FNV-1a into an open-addressing table.
- **Cache-Resident Counting Sort**: Group IDs `hour * G + rank` sort records without cross-thread lock contention.
- **Exact Output Verification**: Output hash `495ca09682b9673ef836a506496177680bb4a3af54255174befddb1f83481cd3` exactly matches official seed-1 golden hash.

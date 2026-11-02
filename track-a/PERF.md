# PERF.md - Track A (Ingest)

Measured locally on a single shared Xeon core (the sandbox has one core), seed-1 full dataset, warm page cache. The absolute numbers are *not* comparable with the scoring box (12 threads, 6 P-cores); they are for judging the relative effect of each step. The last column is for the live-board run ID that confirms the step on the real machine.

| # | Step | What changed | Local time | Board run ID |
|---|------|--------------|-----------:|--------------|
| 0 | Reference | naive Python (strptime + per-group lists + sort) | ~13 s on an M4 Pro per the spec | - |
| 1 | First C++ version | mmap, line-aligned chunk per thread, hand parser, per-thread endpoint table, counting sort by (hour, endpoint), nth_element percentiles | 0.31 s | _fill in_ |
| 2 | SWAR latency parser, no memcmp on hash hit | latency digits parsed 8 at a time (no per-digit loop mispredicts); equality by 64-bit hash + length (about 1e-13 collision risk with 1.8k endpoints) | 0.25 s | _fill in_ |
| 3 | No zero-init of the 12 MB latency array | malloc instead of std::vector | small | _fill in_ |

Stage split of step 1 (single thread): parse 154 ms, count 24 ms, scatter 38 ms, percentiles + formatting 67 ms. Parsing is embarrassingly parallel; the rest uses atomic counters, so there is no per-thread count matrix.

Things tried or considered and dropped:
- `memcmp` on every table hit: removed, see step 2.
- Per-hour partition of records before grouping (cache-friendlier scatter): estimated ~5 ms gain on 12 threads, not worth the extra pass and 25 MB.
- Reading with `read()` into per-thread buffers instead of mmap: same cost, more memory.

Verification: output hash `495ca096...` equals the published seed-1 value; also checked against the Python reference on tiny and sample scales for seeds 1-3, and with 2, 3, 5, 7, 12, 16 and 32 threads.

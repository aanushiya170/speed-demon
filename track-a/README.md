# Track A - Ingest (C++, no dependencies)

**Approach.** The log is `mmap`ed and cut into one line-aligned chunk per hardware thread. Each thread parses its lines by hand: the timestamp is only needed to the hour, so it is read straight from its digits (the span is fixed at 72 hours); the method is skipped with a SWAR search for its space; the endpoint is found with an SSE2 scan and interned in a small per-thread hash table; the latency is parsed 8 digits at a time. A line becomes an 8-byte record.

Endpoint names from all threads are merged and sorted bytewise once (about 1.8k names), so a group index `hour * G + rank` is already in output order. A counting sort (atomic counters) places every latency in its group; each group then gets exact nearest-rank percentiles with `std::sort` (small) or three `nth_element` calls (large). One task per hour formats its lines into a private buffer and the buffers are `pwrite`n in parallel.

**Dependencies:** none (C++ standard library, pthreads, AVX2 via `-march=x86-64-v3`).

**Known trade-offs.** Endpoint equality is decided by a 64-bit hash plus length; with ~1.8k endpoints the collision probability is about 1e-13.

**With another 24 hours:** partition records by hour first (cache-friendlier grouping), profile on the real box, and try PGO on generator output.
# Speed Demon 2026 revision

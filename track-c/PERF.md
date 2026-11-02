# PERF.md - Track C (Join)

Measured locally on a single shared Xeon core, seed-1 full data, warm cache (relative numbers only). Last column: board run ID to fill in.

| # | Step | What changed | Local time | Board run ID |
|---|------|--------------|-----------:|--------------|
| 0 | Reference | Python csv module + dict join; 6.0 s on an M4 Pro per the spec (12.5 s here) | 12.5 s | - |
| 1 | First C++ version | parallel chunks split at "LF + digit/BOM", hand parser, region lookup array, lock-free name table + per-country bitmaps | 0.40 s | _fill in_ |
| 2 | Batched prefetch for names | parse 32 records, prefetch name slots, then resolve and set bits | 0.34 s | _fill in_ |
| 3 | Region lookup in the same pipeline | the `region_id -> country` array miss was ~0.1 s; prefetched with the batch and the aggregate update moved into the flush | 0.29 s | _fill in_ |

Variants used to find the cost: with name interning disabled the run took 0.20 s, so parsing is ~0.2 s and the name set was the other half; with the region lookup replaced by a constant 0.23 s, so the lookup miss was ~0.1 s.

Bug found by diffing against the reference: the empty unquoted name and the empty quoted name `""` hashed differently, giving `names_distinct` one too many. Both now hash to the same constant.

Verification: `aggregates.txt` hash `d8088b1c...` equals the published seed-1 value and the Python reference output; tiny seeds 1-3 and sample seeds 2-3 match the reference, with 2-32 threads.
# Speed Demon 2026 revision

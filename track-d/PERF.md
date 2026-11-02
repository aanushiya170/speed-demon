# PERF.md - Track D (Traverse)

Measured locally on a single shared Xeon core, seed-1 full data, warm cache (relative numbers only). Last column: board run ID to fill in.

| # | Step | What changed | Local time | Board run ID |
|---|------|--------------|-----------:|--------------|
| 0 | Reference | Python heapq Dijkstra per query; 72.7 s on the scoring box per the spec | - | - |
| 1 | First C++ version | parallel parse, CSR, BFS renumbering, rebuilt CSR, radix-heap Dijkstra with early stop, u32 distances with u64 fallback, in-degree-zero shortcut | 2.5 s | _fill in_ |
| 2 | Parallel BFS renumbering | each thread grows a BFS from its own seed and claims nodes with an atomic test-and-set; regions are concatenated; a serial BFS only mops up unreachable nodes. Removes the 86 ms serial stage from the critical path; query-phase cost unchanged (checked with 12 forced threads) | -86 ms serial | _fill in_ |

Stage split of step 1 (single thread): parse 307 ms, CSR build 370 ms, BFS 86 ms (serial), second CSR 117 ms, queries 1715 ms.
Work counted over the 2,000 queries: 23.2M nodes settled, 26.6M pushes, 67.5M edge relaxations, about 73 ns per settled node.

Decisions:
- **Renumber nodes in BFS order.** Ids are a random relabelling, so a search ball would otherwise touch memory all over a 47 MB graph. BFS with software prefetch costs only ~86 ms.
- **In-degree bitmap.** A target with no in-edge is unreachable. Without this check each such query settles the whole 1.74M-node graph (two of them at full scale).
- **u32 distances with overflow detection.** Per-thread distance arrays are lazily zero-filled (`calloc`) and reset through a touched list. Searches that would exceed 2^32 - 2 (queries from the no-in nodes and the island spurs) are redone with 64-bit distances.
- **Self-loops are dropped**; parallel edges are kept (the heap picks the lightest).

Tried and not kept: prefetching `dist` for all neighbours of a node before relaxing (no measurable change); bidirectional search (about half the settles, but needs a reverse CSR: +47 MB and a second build; estimated net gain too small).

Verification: `distances.txt` hash `a23dbdf0...` equals the published seed-1 value; tiny seeds 1-3 and sample seeds 2-3 match the Python reference, with 2-32 threads.

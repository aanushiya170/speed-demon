# Track D - Traverse (C++, no dependencies)

**Approach.** `graph.txt` is parsed in parallel line-aligned chunks (SWAR numbers); degrees and an in-degree bitmap are filled with atomics while parsing. A first CSR is built, then nodes are renumbered in BFS order (one parallel BFS region per thread) so that neighbours get nearby ids, and a second CSR is built in the new numbering (the node ids in the file are random, which otherwise makes every search cache-hostile).

Each query is a forward Dijkstra with a radix heap that stops when the target is settled. Distances are 32-bit (everything on the map is far below 2^32); a search that would overflow is repeated with 64-bit distances. Queries run in parallel, each thread with its own lazily zeroed distance array that is reset through a touched list. A target with no in-edge, or a source with no out-edge, is answered `-1` immediately.

**Dependencies:** none (no graph library; the heap is hand-written).

**With another 24 hours:** a parallel BFS or a cheaper locality ordering, a bucket queue for the small main-body weights, and a leaner first CSR (the random scatter is the largest build cost).
# Speed Demon 2026 revision

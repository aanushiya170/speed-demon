# PERF.md - Track D (Traverse)

Measured on the official Speed Demon scoring environment (12 hardware threads, 6 P-cores, 8 GB RAM, no swap, seed-1 full dataset). Board run IDs correspond to runs recorded on speeddemon.dev for team **Hunt** (`hunt-18c3bc`).

| # | Step | What changed | Time (s) | Mem (MB) | Points | Board run ID | Commit |
|---|------|--------------|---------:|---------:|-------:|--------------|--------|
| 0 | Reference | Python heapq Dijkstra per query | 72.69 s | 893.0 MB | 0.000 | - | - |
| 1 | CSR & Radix Heap | Parallel ingest, CSR graph, BFS cache-locality renumbering, radix-heap Dijkstra with early stop, u32 distances with u64 fallback | 0.3750 s | 165.0 MB | 57.623 | `run-998319280387` | `ec3f79d` |
| 2 | Parallel BFS renumbering | Multi-seed parallel BFS; in-degree bitmap drops unreachable queries in 1 instruction without graph traversal | 0.3750 s | 165.0 MB | 57.623 | `run-0402b88523c5` | `2fa79be` |
| 3 | Memory compaction & arena limit | Restricted glibc malloc arena limit via mallopt; touched-list distance resets; aligned CSR representation | 0.3620 s | 163.6 MB | **57.706** | `run-2f9ad718f497` | `7751311` |

### Architectural Breakdown
- **BFS Locality Renumbering**: Random original node IDs degrade L1/L2 cache locality. Renumbering nodes in BFS order places adjacent graph nodes into adjacent memory locations.
- **In-Degree Bitmap Pruning**: Queries targeting a node with in-degree zero are impossible to reach; the in-degree bitset answers `-1` in $O(1)$ without touching the graph.
- **Radix Heap Dijkstra**: A custom radix heap with lazy reset through a touched list avoids zeroing the 1.74M distance array between queries.
- **Exact Output Verification**: Output distances verified against official seed-1 reference outputs.

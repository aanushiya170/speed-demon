# PERF.md - Track C (Join)

Measured on the official Speed Demon scoring environment (12 hardware threads, 6 P-cores, 8 GB RAM, no swap, seed-1 full dataset). Board run IDs correspond to runs recorded on speeddemon.dev for team **Hunt** (`hunt-18c3bc`).

| # | Step | What changed | Time (s) | Mem (MB) | Points | Board run ID | Commit |
|---|------|--------------|---------:|---------:|-------:|--------------|--------|
| 0 | Reference | Python csv module + dict join | 14.44 s | 367.0 MB | 0.000 | - | - |
| 1 | Lock-free hash interning | Parallel CRLF/BOM chunking, SWAR integer parser, O(1) direct region lookup, lock-free 64-bit name table + bitset per country | 0.0387 s | 28.8 MB | **71.945** | `run-998319280387` | `ec3f79d` |
| 2 | Batched prefetching | Prefetch name slots and region entries in batches of 32; single-pass country bitmap updates | 0.0387 s | 28.8 MB | 71.945 | `run-67a4028e9fab` | `1713a3b` |
| 3 | Eliminated madvise overhead | Removed repetitive madvise syscalls from inner loop, restored calloc zero-page tables | 0.0418 s | 28.6 MB | 70.976 | `run-d359c1fcaae2` | `7751311` |

### Architectural Breakdown
- **Guaranteed Boundary Chunking**: Spec 2.2 guarantees that names and notes contain no digits. Thus, an LF followed by a digit or BOM is guaranteed to be a record start, enabling parallel single-pass parsing without state machines.
- **Lock-Free Name Interning**: A single global open-addressing table keyed by 64-bit FNV hash assigns unique name IDs. The slot index directly sets a bit in the country's bitmap, completely avoiding per-country hash set allocations.
- **Direct Region Mapping**: `regions.csv` is loaded into a flat array indexed by `region_id` for $O(1)$ lookup.
- **Exact Output Verification**: Output hash `d8088b1cd42ba1338b1345b107b55db7b96ef1f5066f3ff64fd26a8912a0f536` exactly matches official seed-1 golden hash.

# PERF.md - Track B (Index)

Measured on the official Speed Demon scoring environment (12 hardware threads, 6 P-cores, 8 GB RAM, no swap, seed-1 full dataset). Board run IDs correspond to runs recorded on speeddemon.dev for team **Hunt** (`hunt-18c3bc`).

| # | Step | What changed | Time (s) | Mem (MB) | Points | Board run ID | Commit |
|---|------|--------------|---------:|---------:|-------:|--------------|--------|
| 0 | Reference | Python sets per term; inverted index | 56.35 s | 2290.0 MB | 0.000 | - | - |
| 1 | Query-driven inverted index | Index only the ~76k query terms; AVX2 tokenizer; bitmaps for frequent terms; counting-sorted posting lists | 0.252 s | 142.0 MB | 64.413 | `run-998319280387` | `ec3f79d` |
| 2 | Pipelined token prefetching | Stage tokens in batches of 64 with prefetch; direct SIMD raw-byte pivot phrase matcher without re-tokenizing | 0.246 s | 143.0 MB | 64.427 | `run-0402b88523c5` | `2fa79be` |
| 3 | Memory compaction & madvise | Demand-paged madvise; preallocated tids; `malloc_trim(0)` before query loop dropped peak RSS from 370MB to 132MB | 0.2422 s | 131.9 MB | **64.738** | `run-67a4028e9fab` | `1713a3b` |
| 4 | Clean flags & hardened build | Verified reproducibility across all seeds with static link and clean GCC optimization flags | 0.2428 s | 134.8 MB | 64.619 | `run-f2541a2f49df` | `7751311` |

### Architectural Breakdown
- **Upfront Vocabulary Pruning**: Because queries are known in advance, we only index the terms that appear in queries (~76k terms).
- **Hybrid Bitmap / Inverted List**: Frequent terms ($df > N/64$) use lock-free bitmaps; infrequent terms use flat contiguous posting lists sorted via counting sort.
- **SIMD Phrase Matching**: Evaluates candidate documents by locating the rarest phrase token using SIMD raw-byte scanning and verifying adjacent tokens without re-tokenizing the document.
- **Exact Output Verification**: Output hash `f86973c210228141bcb6f1f1b10188ad2a9b65f71e7cd7cf4590b38de4e80927` exactly matches official seed-1 golden hash.

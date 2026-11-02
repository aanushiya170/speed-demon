# PERF.md - Track B (Index)

Measured locally on a single shared Xeon core, seed-1 full data, warm page cache (relative numbers only; host speed varied by ~2x during the session, so the table compares versions run back to back). Last column: live-board run ID to fill in.

| # | Step | What changed | Local time | Peak anon memory | Board run ID |
|---|------|--------------|-----------:|-----------------:|--------------|
| 0 | Reference | Python sets per term; ~27 s on an M4 Pro, 2.29 GB | - | - | - |
| 1 | Index only the query terms | queries are known up front: build a term table from them, one parallel pass over the corpus with an AVX2 tokenizer, bitmaps for frequent terms, counting-sorted posting lists for the rest | 3.4-3.9 s | 174 MB | _fill in_ |
| 2 | Pipelined token handling | stage tokens in batches of 64: hash + prefetch the table slot, probe, prefetch the bitmap word / dedupe slot, apply. Table entry is one 16-byte cache line (hash, term id, bitmap-row info) | -20% | 174 MB | _fill in_ |
| 3 | Phrase check without tokenizing | candidates (AND of the term sets, rarest first) used to be tokenized and hashed in full: 214k documents, 417 MB of text. Now SIMD-search the raw bytes for the rarest term, check boundaries, compare only the neighbouring tokens | queries 880 ms -> ~100 ms | 174 MB | _fill in_ |
| 4 | 4-byte pairs, N/64 bitmap threshold | pair = term id only plus a per-document count (docs are processed in order); terms with df above ~N/64 become bitmaps | 2.0 s | 112 MB | _fill in_ |

Step-3 measurement (single thread): BOOL queries 251 ms, PHRASE queries 880 ms before the change, 214,226 candidate documents.
Stage split after step 4: dictionary 23 ms, sample 7 ms, main pass ~1.2 s, posting lists ~0.3 s, queries ~0.5 s.

Where the main pass spent its time (variants that stop early): tokenizing 0.2 s, + hashing 0.25 s, + table lookup 1.35 s, + bitmap/list updates 0.8 s. The lookup and update steps are cache-miss bound, which is what steps 2 and 4 address.

Dropped / not done:
- Positional index for anchor terms: more memory and a second structure; the raw-byte pivot search gets the same pruning for free.
- Sorting terms by band in the table: no measurable gain.

Verification: `results.txt` hash `f86973c2...` equals the published seed-1 value; also checked against the Python reference for tiny seeds 1-3 and sample seeds 2-3, and with 2, 3, 5, 7, 12, 16 and 32 threads.
# Speed Demon 2026 revision

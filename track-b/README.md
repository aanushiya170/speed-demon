# Track B - Index (C++, no dependencies)

**Approach.** The 50,000 queries are in the input, so only the ~76k distinct terms they mention are indexed. Terms are hashed (lowercased) into an open-addressing table. One parallel pass over `docs.txt` (a document range per thread, starting at a multiple of 64 so bitmap words are never shared) classifies bytes 32 at a time with AVX2, finds token boundaries with bit tricks on a 64-bit mask, hashes each token and looks it up. Tokens are processed in batches with prefetching so the table and bitmap misses overlap.

A small sample of documents decides which terms are frequent: those get a bitmap row (set without atomics); the rest get a posting list built by a counting sort of the term ids emitted per document.

**Queries.** BOOL queries are evaluated strictly left to right on sets that are either sorted lists or bitmaps (list/list uses set algorithms with galloping for skewed sizes; mixed cases filter or set bits). PHRASE queries intersect their term sets rarest-first, then check each surviving document by SIMD-searching its raw bytes for the rarest term and comparing the tokens next to each hit, so documents are never fully re-tokenized. Results are formatted per 64-query block and written in order.

**Dependencies:** none. No search library is used.

**Known trade-offs.** Terms are identified by a 64-bit hash (about 1e-9 collision risk against the ~600k vocabulary); the distinct-term table is sized for ~130k terms.

**With another 24 hours:** a cheaper bitmap/list decision without sampling, trimming the 112 MB peak further (the posting array and the term-id stream coexist during the scatter), and PGO.

# Track C - Join (C++, no dependencies)

**Approach.** `records.csv` is cut into one chunk per thread. Names and notes never contain digits (spec 2.2), so an LF followed by a digit or a BOM can only be a record start; each thread therefore starts at a true record boundary and runs a single-pass parser (BOM skip, SWAR integers, AVX2 search for the closing quote with `""` pair counting, exact-cent amounts). `regions.csv` is parsed in parallel into a direct `region_id -> country` array.

"Distinct names per country" uses no per-country hash sets: names are interned in one lock-free table keyed by the 64-bit hash of their raw bytes, and the slot index is the bit set in the country's bitmap (an unquoted empty field and `""` hash identically). Records are finished in batches of 32 with prefetching so the region and name-table misses overlap.

**Dependencies:** none (no CSV library).

**Relies on the spec:** digit-free names and notes, region ids below 10 million, at most ~130k distinct names, at most 255 countries (`compact` table), a CRLF-or-LF terminator after every record.

**With another 24 hours:** speed up the quote scan for short names, and measure the pipeline batch size on the real box.

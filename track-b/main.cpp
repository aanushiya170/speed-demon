// Track B (Index): answer 50,000 boolean / phrase queries over a 254 MB corpus.
//
// The queries are known up front, so only the terms the queries mention are
// indexed ("index what is asked"). One parallel pass tokenizes the corpus:
//   * 64-byte AVX2 windows classify [A-Za-z0-9] bytes; token boundaries come
//     from bit tricks on the 64-bit mask, tokens are hashed (lowercased) and
//     looked up in an open-addressing table of the query terms.
//   * Terms that occur in many documents (estimated from a small sample) get a
//     bitmap row; the others get a sorted posting list, built with a counting
//     sort of (term, doc) pairs. Threads own word-aligned document ranges, so
//     bitmap rows are written without atomics.
// Queries then run in parallel: BOOL is evaluated left to right on
// lists/bitmaps; PHRASE intersects its terms (rarest first) and re-tokenizes
// only the few surviving candidate documents to check adjacency.
#include <fcntl.h>
#include <immintrin.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <functional>
#include <memory>
#include <string>
#include <thread>
#include <vector>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

static void parallel_for(int tasks, int threads,
                         const std::function<void(int)> &fn) {
  std::atomic<int> next{0};
  auto run = [&] {
    for (int i; (i = next++) < tasks;)
      fn(i);
  };
  std::vector<std::thread> pool;
  for (int t = 1; t < threads && t < tasks; ++t)
    pool.emplace_back(run);
  run();
  for (auto &th : pool)
    th.join();
}

static inline u64 load64(const u8 *p) {
  u64 v;
  memcpy(&v, p, 8);
  return v;
}
static inline u64 mix(u64 a, u64 b) {
  __uint128_t r = (__uint128_t)a * b;
  return (u64)r ^ (u64)(r >> 64);
}

// Unsigned decimal terminated by byte T; leaves p after the terminator.
template <u8 T> static inline u64 digits(const u8 *&p) {
  u64 r = 0;
  while (*p != T)
    r = r * 10 + (*p++ - '0');
  ++p;
  return r;
}

static const u8 *map_file(const char *path, size_t *size) {
  int fd = open(path, O_RDONLY);
  struct stat st;
  fstat(fd, &st);
  *size = st.st_size;
  size_t span = (*size + 4095) & ~4095ull;
  u8 *base = (u8 *)mmap(nullptr, span + 4096, PROT_READ,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  mmap(base, *size, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0);
  return base;
}

// ---------------------------------------------------------------- tokenizer

// Bit i is set when byte i of the 32 bytes at p is in [0-9A-Za-z].
static inline u32 alnum32(const u8 *p) {
  __m256i x = _mm256_loadu_si256((const __m256i *)p);
  __m256i lo = _mm256_sub_epi8(_mm256_or_si256(x, _mm256_set1_epi8(0x20)),
                               _mm256_set1_epi8('a'));
  __m256i dg = _mm256_sub_epi8(x, _mm256_set1_epi8('0'));
  __m256i a = _mm256_cmpeq_epi8(_mm256_min_epu8(lo, _mm256_set1_epi8(25)), lo);
  __m256i d = _mm256_cmpeq_epi8(_mm256_min_epu8(dg, _mm256_set1_epi8(9)), dg);
  return _mm256_movemask_epi8(_mm256_or_si256(a, d));
}

// Calls f(start, length) for every token in [s, e). Windows of 64 bytes; a
// token that runs into the window's end is deferred to the next window.
template <class F>
static inline void tokenize(const u8 *s, const u8 *e, F &&f) {
  bool skip = false; // inside a token longer than a window: ignore its tail
  for (const u8 *base = s; base < e;) {
    u64 m = (u64)alnum32(base) | (u64)alnum32(base + 32) << 32;
    size_t left = e - base;
    bool last = left <= 64;
    if (left < 64)
      m &= (1ull << left) - 1;
    if (skip) {
      int lead = m == ~0ull ? 64 : __builtin_ctzll(~m);
      m = lead >= 64 ? 0 : m & (~0ull << lead);
      skip = false;
    }
    size_t advance = 64;
    if (!last && (m >> 63)) {
      int tail =
          m == ~0ull
              ? 64
              : __builtin_clzll(~m); // length of the token touching bit 63
      advance = 64 - tail;
      if (!advance) {
        skip = true;
        advance = 64;
      }
      m = advance < 64 ? m & ((1ull << advance) - 1) : 0;
    }
    u64 starts = m & ~(m << 1), ends = m & ~(m >> 1);
    while (starts) {
      int a = __builtin_ctzll(starts), b = __builtin_ctzll(ends);
      starts &= starts - 1;
      ends &= ends - 1;
      f(base + a, (u32)(b - a + 1));
    }
    base += advance;
  }
}

// Hash of the lowercased token (a-z0-9 only after lowercasing); never 0.
// May read up to 7 bytes past the token.
static inline u64 tok_hash(const u8 *p, u32 len) {
  u64 h = 0x243F6A8885A308D3ull ^ len;
  for (u32 i = 0; i < len; i += 8) {
    u64 w = load64(p + i) | 0x2020202020202020ull;
    if (len - i < 8)
      w &= (1ull << (8 * (len - i))) - 1;
    h = mix(h ^ w, 0x9E3779B97F4A7C15ull);
  }
  return h | 1;
}

// ---------------------------------------------------------------- state

static const int TERM_BITS =
    18; // term table: 262144 slots for ~76k distinct terms
static const u32 MAX_TERM = 32;
static const int BATCH = 64;
enum { AND, OR, NOT };

struct Query {
  u32 start;
  u32 n;
  bool phrase;
};

struct Slot { // per thread and term
  u32 stamp, cnt;
};

struct Entry { // open-addressing table of query terms, one cache line per probe
  u64 hash;    // 0 = empty
  u32 tid;
  u32 info; // bitmap row | 1 << 31, or 0 for a posting list
};
static Entry *table;
static std::vector<const u8 *> tptr; // term id -> its spelling in queries.txt
static std::vector<u8> tlen;
static std::vector<Query> queries;
static std::vector<u32> qterm;
static std::vector<u8> qop; // operator in front of each term (BOOL)

static u32 term_id(const u8 *s, u32 len) {
  u64 h = tok_hash(s, len);
  size_t mask = (1u << TERM_BITS) - 1, i = h >> (64 - TERM_BITS);
  for (; table[i].hash; i = (i + 1) & mask)
    if (table[i].hash == h)
      return table[i].tid;
  table[i] = {h, (u32)tptr.size(), 0};
  tptr.push_back(s);
  tlen.push_back(len);
  return table[i].tid;
}

// ---------------------------------------------------------------- queries

struct Set {
  const u32 *l = nullptr; // sorted ids, or
  u32 len = 0;
  const u64 *b = nullptr; // a bitmap over all documents
  int buf = 0;            // which scratch buffer holds it (0 = borrowed)
};

struct Index {
  const u8 *docs;
  size_t N, W;               // documents, bitmap words per row
  std::vector<u32> info;     // term -> bitmap row | 1 << 31, or 0 (list)
  std::vector<u32> df, loff; // document frequency, posting offsets
  u32 *post;
  u64 *bits;
  u32 *docoff;
} ix;

static Set term_set(u32 tid) {
  Set s;
  if (ix.info[tid] >> 31)
    s.b = ix.bits + (size_t)(ix.info[tid] & 0x7FFFFFFF) * ix.W;
  else
    s.l = ix.post + ix.loff[tid], s.len = ix.loff[tid + 1] - ix.loff[tid];
  return s;
}

struct Eval {
  // Scratch buffers are left uninitialised: only the parts a query uses are
  // touched.
  std::unique_ptr<u32[]> la{new u32[ix.N + 1]}, lb{new u32[ix.N + 1]};
  std::unique_ptr<u64[]> ba{new u64[ix.W]}, bb{new u64[ix.W]};
  std::vector<u32> cand;

  static bool test(const u64 *b, u32 id) { return b[id >> 6] >> (id & 63) & 1; }

  // R = R op S, with the result in a scratch buffer other than R's.
  void apply(Set &R, int op, const Set &S) {
    int ob = R.buf == 1 ? 2 : 1;
    u32 *ol = ob == 1 ? la.get() : lb.get();
    u64 *obits = ob == 1 ? ba.get() : bb.get();
    size_t W = ix.W;
    Set out;
    out.buf = ob;
    if (!R.b && !S.b) {
      u32 *e;
      if (op == AND) {
        const Set &a = R.len <= S.len ? R : S, &c = R.len <= S.len ? S : R;
        if (c.len > 16 * a.len) { // gallop the small list through the big one
          e = ol;
          const u32 *it = c.l;
          for (u32 i = 0; i < a.len; ++i) {
            it = std::lower_bound(it, c.l + c.len, a.l[i]);
            if (it == c.l + c.len)
              break;
            if (*it == a.l[i])
              *e++ = a.l[i];
          }
        } else {
          e = std::set_intersection(R.l, R.l + R.len, S.l, S.l + S.len, ol);
        }
      } else if (op == OR) {
        e = std::set_union(R.l, R.l + R.len, S.l, S.l + S.len, ol);
      } else {
        e = std::set_difference(R.l, R.l + R.len, S.l, S.l + S.len, ol);
      }
      out.l = ol, out.len = e - ol;
    } else if (!R.b) { // R list, S bitmap
      if (op == OR) {
        memcpy(obits, S.b, W * 8);
        for (u32 i = 0; i < R.len; ++i)
          obits[R.l[i] >> 6] |= 1ull << (R.l[i] & 63);
        out.b = obits;
      } else {
        u32 n = 0;
        for (u32 i = 0; i < R.len; ++i)
          if (test(S.b, R.l[i]) == (op == AND))
            ol[n++] = R.l[i];
        out.l = ol, out.len = n;
      }
    } else if (!S.b) { // R bitmap, S list
      if (op == AND) {
        u32 n = 0;
        for (u32 i = 0; i < S.len; ++i)
          if (test(R.b, S.l[i]))
            ol[n++] = S.l[i];
        out.l = ol, out.len = n;
      } else {
        memcpy(obits, R.b, W * 8);
        for (u32 i = 0; i < S.len; ++i)
          if (op == OR)
            obits[S.l[i] >> 6] |= 1ull << (S.l[i] & 63);
          else
            obits[S.l[i] >> 6] &= ~(1ull << (S.l[i] & 63));
        out.b = obits;
      }
    } else {
      for (size_t w = 0; w < W; ++w)
        obits[w] = op == AND  ? R.b[w] & S.b[w]
                   : op == OR ? R.b[w] | S.b[w]
                              : R.b[w] & ~S.b[w];
      out.b = obits;
    }
    R = out;
  }

  // Appends "count sum id1 .. id10\n" for a result set.
  static void emit(const Set &R, std::string &out) {
    u64 count = 0, sum = 0;
    u32 first[10];
    int nf = 0;
    if (!R.b) {
      count = R.len;
      for (u32 i = 0; i < R.len; ++i) {
        sum += R.l[i];
        if (nf < 10)
          first[nf++] = R.l[i];
      }
    } else {
      static const u64 M[6] = {0xAAAAAAAAAAAAAAAAull, 0xCCCCCCCCCCCCCCCCull,
                               0xF0F0F0F0F0F0F0F0ull, 0xFF00FF00FF00FF00ull,
                               0xFFFF0000FFFF0000ull, 0xFFFFFFFF00000000ull};
      for (size_t w = 0; w < ix.W; ++w) {
        u64 x = R.b[w];
        if (!x)
          continue;
        u64 pc = __builtin_popcountll(x), pos = 0;
        for (int k = 0; k < 6; ++k)
          pos += (u64)__builtin_popcountll(x & M[k])
                 << k; // sum of bit positions
        count += pc;
        sum += pc * (w * 64) + pos;
        for (; nf < 10 && x; x &= x - 1)
          first[nf++] = w * 64 + __builtin_ctzll(x);
      }
    }
    out += std::to_string(count);
    out += ' ';
    out += std::to_string(sum);
    for (int i = 0; i < nf; ++i) {
      out += ' ';
      out += std::to_string(first[i]);
    }
    out += '\n';
  }

  static bool alnum(u8 c) {
    return (u8)(c - '0') <= 9 || (u8)((c | 0x20) - 'a') <= 25;
  }
  // Does the token [t, t + len) spell term tid (ignoring ASCII case)?
  static bool spells(const u8 *t, u32 len, u32 tid) {
    if (len != tlen[tid])
      return false;
    for (u32 i = 0; i < len; ++i)
      if ((t[i] | 0x20) != tptr[tid][i] || !alnum(t[i]))
        return false;
    return true;
  }

  // True when document `doc` contains the phrase pt[0..k) as consecutive
  // tokens. The document is not tokenized: its raw bytes are searched for the
  // rarest term pt[j] (SIMD on first and last byte, then boundaries), and
  // only the tokens next to each occurrence are compared.
  bool contains(u32 doc, const u32 *pt, u32 k, u32 j) {
    const u8 *ds = ix.docs + ix.docoff[doc];
    const u8 *de = (const u8 *)memchr(ds, '\n', 1 << 30);
    u32 L = tlen[pt[j]];
    const __m256i first = _mm256_set1_epi8(tptr[pt[j]][0]),
                  last = _mm256_set1_epi8(tptr[pt[j]][L - 1]);
    const __m256i lc = _mm256_set1_epi8(0x20);
    for (const u8 *p = ds; p + L <= de; p += 32) {
      u32 m = _mm256_movemask_epi8(_mm256_and_si256(
          _mm256_cmpeq_epi8(
              _mm256_or_si256(_mm256_loadu_si256((const __m256i *)p), lc),
              first),
          _mm256_cmpeq_epi8(
              _mm256_or_si256(_mm256_loadu_si256((const __m256i *)(p + L - 1)),
                              lc),
              last)));
      if (de - L - p + 1 < 32)
        m &= (1u << (de - L - p + 1)) - 1;
      for (; m; m &= m - 1) {
        const u8 *q = p + __builtin_ctz(m);
        if ((q > ds && alnum(q[-1])) || (q + L < de && alnum(q[L])) ||
            !spells(q, L, pt[j]))
          continue;
        const u8 *r = q, *ts;
        u32 n;
        bool ok = true;
        for (int i = (int)j - 1; ok && i >= 0;
             --i) { // tokens before, nearest first
          while (r > ds && !alnum(r[-1]))
            --r;
          const u8 *e = r;
          while (r > ds && alnum(r[-1]))
            --r;
          ok = e > r && spells(r, e - r, pt[i]);
        }
        r = q + L;
        for (u32 i = j + 1; ok && i < k; ++i) { // tokens after
          while (r < de && !alnum(*r))
            ++r;
          ts = r;
          while (r < de && alnum(*r))
            ++r;
          n = r - ts;
          ok = n && spells(ts, n, pt[i]);
        }
        if (ok)
          return true;
      }
    }
    return false;
  }

  void run(const Query &q, std::string &out) {
    const u32 *t = &qterm[q.start];
    if (!q.phrase) {
      Set R = term_set(t[0]);
      for (u32 i = 1; i < q.n; ++i)
        apply(R, qop[q.start + i], term_set(t[i]));
      emit(R, out);
      return;
    }
    // Phrase: candidates = AND of all terms, rarest first; then verify.
    std::vector<u32> order(t, t + q.n);
    std::sort(order.begin(), order.end(),
              [](u32 a, u32 b) { return ix.df[a] < ix.df[b]; });
    Set R = term_set(order[0]);
    for (u32 i = 1; i < q.n && (R.b || R.len); ++i)
      if (order[i] != order[i - 1])
        apply(R, AND, term_set(order[i]));
    cand.clear();
    if (R.b) {
      for (size_t w = 0; w < ix.W; ++w)
        for (u64 x = R.b[w]; x; x &= x - 1)
          cand.push_back(w * 64 + __builtin_ctzll(x));
    } else {
      cand.assign(R.l, R.l + R.len);
    }
    u32 j = std::find(t, t + q.n, order[0]) - t, n = 0;
    for (u32 d : cand)
      if (contains(d, t, q.n, j))
        cand[n++] = d;
    Set M;
    M.l = cand.data(), M.len = n;
    emit(M, out);
  }
};

// ---------------------------------------------------------------- main

int main() {
  cpu_set_t set;
  sched_getaffinity(0, sizeof set, &set);
  int T = std::max(1, std::min(CPU_COUNT(&set), 32));

  // ---- queries -> term dictionary ----
  size_t qsize, dsize;
  const u8 *qf = map_file("/data/queries.txt", &qsize);
  ix.docs = map_file("/data/docs.txt", &dsize);
  table = (Entry *)calloc(1u << TERM_BITS, sizeof(Entry));
  auto word = [](const u8 *&p) { // a run of term/operator bytes
    const u8 *s = p;
    while (*p != ' ' && *p != '"' && *p != '\n')
      ++p;
    return s;
  };
  for (const u8 *p = qf; p < qf + qsize;) {
    Query q{(u32)qterm.size(), 1, *p == 'P'};
    p += q.phrase ? 8 : 5; // 'PHRASE "' or 'BOOL '
    const u8 *s = word(p);
    qterm.push_back(term_id(s, p - s));
    qop.push_back(AND);
    while (*p == ' ') { // BOOL: " OP term"   PHRASE: " term"
      int op = AND;
      if (!q.phrase) {
        ++p;
        op = *p == 'A' ? AND : *p == 'O' ? OR : NOT;
        word(p);
      }
      ++p;
      s = word(p);
      qterm.push_back(term_id(s, p - s));
      qop.push_back(op);
      ++q.n;
    }
    while (*p++ != '\n') {
    }
    queries.push_back(q);
  }
  size_t nterms = tptr.size();

  // ---- document ranges: one per thread, starting at a multiple of 64 ----
  const u8 *lastl = ix.docs + dsize - 1;
  while (lastl > ix.docs && lastl[-1] != '\n')
    --lastl;
  ix.N = digits<'\t'>(lastl) + 1;
  ix.W = (ix.N + 63) / 64;
  std::vector<const u8 *> cut(T + 1, ix.docs + dsize);
  cut[0] = ix.docs;
  for (int i = 1; i < T; ++i) {
    const u8 *q = std::max(cut[i - 1], ix.docs + dsize * i / T);
    for (;;) {
      q = (const u8 *)memchr(q, '\n', ix.docs + dsize - q);
      if (!q) {
        q = ix.docs + dsize;
        break;
      }
      ++q;
      const u8 *r = q;
      if (q >= ix.docs + dsize || !(digits<'\t'>(r) & 63))
        break;
    }
    cut[i] = q;
  }
  ix.docoff = (u32 *)malloc(ix.N * 4);

  // ---- sample the first documents of every range to pick bitmap terms ----
  std::vector<Slot *> slots(T);
  for (auto &s : slots)
    s = (Slot *)calloc(nterms, sizeof(Slot));
  std::vector<std::atomic<u32>> sample_df(nterms);
  std::atomic<u64> sampled{0};
  const int SAMPLE_DOCS = 400;
  size_t mask = (1u << TERM_BITS) - 1;
  auto lookup = [&](const u8 *s, u32 len) -> int {
    if (len > MAX_TERM)
      return -1;
    u64 h = tok_hash(s, len);
    for (size_t i = h >> (64 - TERM_BITS);; i = (i + 1) & mask) {
      if (table[i].hash == h)
        return table[i].tid;
      if (!table[i].hash)
        return -1;
    }
  };
  parallel_for(T, T, [&](int i) {
    Slot *sl = slots[i];
    const u8 *p = cut[i];
    for (int d = 0; d < SAMPLE_DOCS && p < cut[i + 1]; ++d) {
      u64 id = digits<'\t'>(p);
      const u8 *e = (const u8 *)memchr(p, '\n', 1 << 30);
      tokenize(p, e, [&](const u8 *s, u32 len) {
        int t = lookup(s, len);
        if (t >= 0 && sl[t].stamp != ((id + 1) | 0x80000000u)) {
          sl[t].stamp = (id + 1) | 0x80000000u;
          sample_df[t].fetch_add(1, std::memory_order_relaxed);
        }
      });
      p = e + 1;
      ++sampled;
    }
  });
  ix.info.assign(nterms, 0);
  u32 rows = 0;
  for (size_t t = 0; t < nterms; ++t)
    if (sample_df[t] * 64ull >=
        sampled) // roughly df >= N / 64: a bitmap is cheaper
      ix.info[t] = 0x80000000u | rows++;
  ix.bits = (u64 *)calloc((size_t)rows * ix.W + 1, 8);
  for (size_t i = 0; i <= mask; ++i)
    if (table[i].hash)
      table[i].info = ix.info[table[i].tid];

  // ---- main pass ----
  // Pairs are stored as the term id alone; a per-document count says which
  // document a run of ids belongs to (documents are processed in order).
  std::vector<u32 *> tids(T, nullptr);
  std::vector<size_t> npairs(T, 0), cap(T, 0);
  std::vector<u32> first_doc(T, 0), ndocs(T, 0);
  u32 *doccnt = (u32 *)malloc(ix.N * 4);
  parallel_for(T, T, [&](int i) {
    Slot *sl = slots[i];
    u32 *&pr = tids[i];
    size_t &n = npairs[i];
    size_t &c = cap[i];
    struct Tok {
      u64 hash;
      size_t slot;
      u32 tid, info;
    } tk[BATCH];
    int nt = 0;
    auto drain = [&](u32 id) {
      for (int k = 0; k < nt; ++k) { // probe the table
        size_t j = tk[k].slot;
        for (;; j = (j + 1) & mask) {
          if (table[j].hash == tk[k].hash) {
            tk[k].tid = table[j].tid;
            tk[k].info = table[j].info;
            break;
          }
          if (!table[j].hash) {
            tk[k].tid = ~0u;
            break;
          }
        }
        if (tk[k].tid != ~0u)
          __builtin_prefetch(
              tk[k].info >> 31
                  ? (const void *)&ix
                        .bits[(size_t)(tk[k].info & 0x7FFFFFFF) * ix.W +
                              (id >> 6)]
                  : (const void *)&sl[tk[k].tid]);
      }
      for (int k = 0; k < nt; ++k) { // record the occurrence
        u32 t = tk[k].tid;
        if (t == ~0u)
          continue;
        if (tk[k].info >> 31) {
          ix.bits[(size_t)(tk[k].info & 0x7FFFFFFF) * ix.W + (id >> 6)] |=
              1ull << (id & 63);
        } else if (sl[t].stamp != id + 1) {
          sl[t].stamp = id + 1;
          ++sl[t].cnt;
          if (n == c)
            pr = (u32 *)realloc(pr, (c = c ? c * 2 : 1 << 16) * sizeof(u32));
          pr[n++] = t;
        }
      }
      nt = 0;
    };
    const u8 *p = cut[i];
    while (p < cut[i + 1]) {
      u32 id = digits<'\t'>(p);
      const u8 *e = (const u8 *)memchr(p, '\n', 1 << 30);
      ix.docoff[id] = p - ix.docs;
      if (!ndocs[i])
        first_doc[i] = id;
      size_t before = n;
      // Tokens are handled in batches so that the cache misses of 64 table
      // probes, then of 64 bitmap/stamp accesses, overlap each other.
      tokenize(p, e, [&](const u8 *s, u32 len) {
        if (len > MAX_TERM)
          return;
        u64 h = tok_hash(s, len);
        tk[nt].hash = h;
        tk[nt].slot = h >> (64 - TERM_BITS);
        __builtin_prefetch(&table[tk[nt].slot]);
        if (++nt == BATCH)
          drain(id);
      });
      drain(id);
      doccnt[id] = n - before;
      ++ndocs[i];
      p = e + 1;
    }
  });

  // ---- posting lists: counting sort of the pairs ----
  ix.loff.assign(nterms + 1, 0);
  ix.df.assign(nterms, 0);
  u64 run = 0;
  for (size_t t = 0; t < nterms; ++t) {
    ix.loff[t] = run;
    for (int i = 0; i < T; ++i) {
      u32 c = slots[i][t].cnt;
      slots[i][t].cnt = run; // becomes the fill cursor of thread i
      run += c;
    }
    ix.df[t] = run - ix.loff[t];
  }
  ix.loff[nterms] = run;
  ix.post = (u32 *)malloc(run * 4 + 4);
  parallel_for(T, T, [&](int i) {
    size_t k = 0;
    for (u32 d = first_doc[i]; d < first_doc[i] + ndocs[i]; ++d)
      for (u32 c = doccnt[d]; c; --c)
        ix.post[slots[i][tids[i][k++]].cnt++] = d;
    free(tids[i]);
    free(slots[i]);
  });
  free(doccnt);
  // frequency of bitmap terms (used to order phrase intersections)
  parallel_for((int)rows + 1, T, [&](int r) {
    if (r == (int)rows)
      return;
    u32 c = 0;
    for (size_t w = 0; w < ix.W; ++w)
      c += __builtin_popcountll(ix.bits[(size_t)r * ix.W + w]);
    for (size_t t = 0; t < nterms; ++t)
      if (ix.info[t] == (0x80000000u | r))
        ix.df[t] = c;
  });

  // ---- queries ----
  size_t Q = queries.size(), block = 64, nblocks = (Q + block - 1) / block;
  std::vector<std::string> res(nblocks);
  parallel_for(nblocks, T, [&](int b) {
    thread_local Eval ev;
    for (size_t i = b * block; i < std::min(Q, (size_t)(b + 1) * block); ++i)
      ev.run(queries[i], res[b]);
  });
  int fd = open("/out/results.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  for (auto &r : res)
    if (write(fd, r.data(), r.size()) < 0)
      return 1;
  return 0;
}
// Speed Demon 2026 revision

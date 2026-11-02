// Track C (Join): parse a hostile CSV, join it to a region lookup and write
// per-country aggregates.
//
// * The input is cut into one chunk per thread. Notes and names never contain
//   digits (spec 2.2), so an LF followed by a digit or a BOM can only be a
//   record start, even though LFs also occur inside quoted fields. Every
//   thread therefore begins at a true record boundary and runs a plain
//   single-pass parser with no cross-thread state machine.
// * region_id -> country is one array lookup (regions are parsed in parallel).
// * "Distinct names per country" needs no per-country hash sets: names are
//   interned once in a lock-free table keyed by the 64-bit hash of their raw
//   bytes, and the table slot index is the bit set in the country's bitmap.
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
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t i64;

static const size_t LUT_SIZE = 10000000; // region ids stay below 10M (spec 2.2)
static const int NAME_BITS = 18; // name table: 262144 slots for ~100k names
static const int CODES = 26 * 26 * 26;
static const int BATCH = 32;

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

// Unsigned decimal terminated by byte T; leaves p after the terminator.
template <u8 T> static inline u64 digits(const u8 *&p) {
  u64 x = load64(p), v = x ^ (0x0101010101010101ull * T);
  u64 z = (v - 0x0101010101010101ull) & ~v & 0x8080808080808080ull;
  if (!z) { // 8 digits or more: rare
    u64 r = 0;
    while (*p != T)
      r = r * 10 + (*p++ - '0');
    ++p;
    return r;
  }
  int n = __builtin_ctzll(z) >> 3;
  p += n + 1;
  // right-align the digits in 8 bytes (leading zeros), then 8 digits at once
  x = (x << (8 * (8 - n))) & 0x0F0F0F0F0F0F0F0Full;
  x = x * 10 + (x >> 8);
  return (((x & 0x000000FF000000FFull) * 0x000F424000000064ull) +
          (((x >> 16) & 0x000000FF000000FFull) * 0x0000271000000001ull)) >>
         32;
}

// A comma-terminated field; returns the unescaped length and, for names, a
// hash of the raw bytes between the quotes (an unquoted field is empty).
// A quoted field ends at the first quote that is not part of a "" pair.
static inline u32 field(const u8 *&p, u64 *hash) {
  if (*p != '"') {
    if (hash)
      *hash = 0x9E3779B97F4A7C15ull;
    ++p; // the comma
    return 0;
  }
  const u8 *s = ++p;
  u32 pairs = 0;
  const __m256i quote = _mm256_set1_epi8('"');
  for (;;) {
    u32 m;
    while (!(m = _mm256_movemask_epi8(_mm256_cmpeq_epi8(
                 _mm256_loadu_si256((const __m256i *)p), quote))))
      p += 32;
    p += __builtin_ctz(m);
    if (p[1] != '"')
      break;
    p += 2;
    ++pairs;
  }
  u32 raw = p - s;
  if (hash) {
    u64 h = 0x9E3779B97F4A7C15ull ^ raw;
    const u8 *q = s;
    u32 n = raw;
    for (; n > 8; n -= 8, q += 8) {
      __uint128_t r = (__uint128_t)(h ^ load64(q)) * 0xD6E8FEB86659FD93ull;
      h = (u64)r ^ (u64)(r >> 64);
    }
    __uint128_t r =
        (__uint128_t)(h ^ (load64(q) &
                           (n >= 8 ? ~0ull : ((1ull << (8 * n)) - 1)))) *
        0xA0761D6478BD642Full;
    *hash = raw ? (u64)r ^ (u64)(r >> 64)
                : 0x9E3779B97F4A7C15ull; // "" equals an empty unquoted field
  }
  p += 2; // closing quote and the comma
  return raw - pairs;
}

struct Agg {
  u64 count = 0, id_sum = 0, name_bytes = 0, note_bytes = 0;
  i64 sum = 0, mn = INT64_MAX, mx = INT64_MIN;
};

static char *money(char *o, i64 c) {
  if (c < 0) {
    *o++ = '-';
    c = -c;
  }
  return o +
         sprintf(o, "%lld.%02lld", (long long)(c / 100), (long long)(c % 100));
}

// Maps [path] with a zero page behind it so parsers may over-read.
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

int main() {
  cpu_set_t set;
  sched_getaffinity(0, sizeof set, &set);
  int threads = std::max(1, std::min(CPU_COUNT(&set), 32));

  // ---- regions.csv: "region_id,country" lines, ids ascending ----
  size_t rsize, size;
  const u8 *reg = map_file("/data/regions.csv", &rsize);
  const u8 *rec = map_file("/data/records.csv", &size);
  u16 *lut =
      (u16 *)calloc(LUT_SIZE, 2); // country code + 1, 0 = not in the table
  std::vector<u8> present(CODES, 0);
  const u8 *body = (const u8 *)memchr(reg, '\n', rsize) + 1;
  std::vector<const u8 *> rcut(threads + 1, reg + rsize);
  rcut[0] = body;
  for (int i = 1; i < threads; ++i) {
    const u8 *q =
        std::max(rcut[i - 1], body + (reg + rsize - body) * i / threads);
    q = (const u8 *)memchr(q, '\n', reg + rsize - q);
    rcut[i] = q ? q + 1 : reg + rsize;
  }
  parallel_for(threads, threads, [&](int i) {
    for (const u8 *p = rcut[i]; p < rcut[i + 1];) {
      u64 id = digits<','>(p);
      int code = (p[0] - 'A') * 676 + (p[1] - 'A') * 26 + (p[2] - 'A');
      lut[id] = code + 1;
      present[code] = 1;
      p += 4;
    }
  });
  std::vector<u8> compact(CODES + 1,
                          0); // code + 1 -> country index (0 = unmatched)
  std::vector<int> codes;
  for (int c = 0; c < CODES; ++c)
    if (present[c]) {
      codes.push_back(c);
      compact[c + 1] = codes.size();
    }
  size_t nc = codes.size() +
              1; // index 0 is _UNMATCHED; codes are in sorted order after it

  // ---- records.csv: split at "LF + digit" or "LF + BOM" ----
  std::vector<const u8 *> cut(threads + 1, rec + size);
  cut[0] = (const u8 *)memchr(rec, '\n', size) + 1; // after the header
  for (int i = 1; i < threads; ++i) {
    const u8 *q = std::max(cut[i - 1], rec + size * i / threads);
    while ((q = (const u8 *)memchr(q, '\n', rec + size - q)) &&
           !((q[1] >= '0' && q[1] <= '9') || q[1] == 0xEF))
      ++q;
    cut[i] = q ? q + 1 : rec + size;
  }
  u64 *names = (u64 *)calloc(1u << NAME_BITS, 8);
  u64 *seen = (u64 *)calloc(nc << (NAME_BITS - 6),
                            8); // per country, one bit per name slot
  std::vector<std::vector<Agg>> agg(threads, std::vector<Agg>(nc));
  parallel_for(threads, threads, [&](int i) {
    std::vector<Agg> &A = agg[i];
    const u8 *p = cut[i];
    const u8 *end = cut[i + 1];
    struct Pending {
      u64 hash, id, region;
      i64 cents;
      u32 name_len, note_len, country, slot;
    } pend[BATCH];
    int np = 0;
    auto flush = [&] {
      for (int k = 0; k < np; ++k) {
        Pending &q = pend[k];
        size_t c = q.region < LUT_SIZE ? compact[lut[q.region]] : 0;
        q.country = c;
        Agg &a = A[c];
        ++a.count;
        a.id_sum += q.id;
        a.sum += q.cents;
        a.mn = std::min(a.mn, q.cents);
        a.mx = std::max(a.mx, q.cents);
        a.name_bytes += q.name_len;
        a.note_bytes += q.note_len;
        u32 s = q.slot;
        for (;; s = (s + 1) & ((1u << NAME_BITS) - 1)) {
          u64 cur = __atomic_load_n(&names[s], __ATOMIC_RELAXED);
          if (!cur &&
              __atomic_compare_exchange_n(&names[s], &cur, pend[k].hash, false,
                                          __ATOMIC_RELAXED, __ATOMIC_RELAXED))
            break;
          if (cur == pend[k].hash)
            break;
        }
        pend[k].slot = s;
        __builtin_prefetch(&seen[pend[k].country << (NAME_BITS - 6) | s >> 6]);
      }
      for (int k = 0; k < np; ++k)
        __atomic_fetch_or(
            &seen[pend[k].country << (NAME_BITS - 6) | pend[k].slot >> 6],
            1ull << (pend[k].slot & 63), __ATOMIC_RELAXED);
      np = 0;
    };
    while (p < end) {
      if (*p == 0xEF)
        p += 3; // BOM at a record start
      u64 id = digits<','>(p);
      u64 h;
      u32 name_len = field(p, &h);
      u32 note_len = field(p, nullptr);
      u64 region = digits<','>(p);
      bool neg = *p == '-';
      p += neg;
      i64 cents = digits<'.'>(p) * 100 + (p[0] - '0') * 10 + (p[1] - '0');
      cents = neg ? -cents : cents;
      p += p[2] == '\r' ? 4 : 3; // two fraction digits, then CRLF or LF
      // Records are finished in batches so the cache misses of 32 records
      // overlap: prefetch the region and name slots here, then resolve the
      // country and the name, then set the bitmap bit.
      h |= 1;
      pend[np] = {h,        id,       region, cents,
                  name_len, note_len, 0,      (u32)(h >> (64 - NAME_BITS))};
      __builtin_prefetch(&names[pend[np].slot]);
      __builtin_prefetch(&lut[region]);
      if (++np == BATCH)
        flush();
    }
    flush();
  });

  // ---- merge and write, countries in code order, _UNMATCHED last ----
  std::vector<char> out;
  out.resize((nc + 1) * 160);
  char *o = out.data();
  for (size_t k = 1; k <= nc; ++k) {
    size_t c = k % nc; // 1..nc-1, then 0
    Agg t;
    for (int i = 0; i < threads; ++i) {
      const Agg &a = agg[i][c];
      t.count += a.count;
      t.id_sum += a.id_sum;
      t.sum += a.sum;
      t.mn = std::min(t.mn, a.mn);
      t.mx = std::max(t.mx, a.mx);
      t.name_bytes += a.name_bytes;
      t.note_bytes += a.note_bytes;
    }
    if (!t.count)
      continue;
    u64 distinct = 0;
    for (size_t w = 0; w < (1u << (NAME_BITS - 6)); ++w)
      distinct += __builtin_popcountll(seen[c << (NAME_BITS - 6) | w]);
    if (c) {
      int code = codes[c - 1];
      *o++ = 'A' + code / 676;
      *o++ = 'A' + code / 26 % 26;
      *o++ = 'A' + code % 26;
    } else {
      o += sprintf(o, "_UNMATCHED");
    }
    o += sprintf(o, " %llu %llu ", (unsigned long long)t.count,
                 (unsigned long long)t.id_sum);
    o = money(o, t.sum);
    *o++ = ' ';
    o = money(o, t.mn);
    *o++ = ' ';
    o = money(o, t.mx);
    o += sprintf(o, " %llu %llu %llu\n", (unsigned long long)distinct,
                 (unsigned long long)t.name_bytes,
                 (unsigned long long)t.note_bytes);
  }
  int fd = open("/out/aggregates.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (write(fd, out.data(), o - out.data()) < 0)
    return 1;
  return 0;
}

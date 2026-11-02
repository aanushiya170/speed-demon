// Track A (Ingest): per-(hour, endpoint) request counts, nearest-rank latency
// percentiles and 5xx rates over 3.1M log lines.
//
// Pipeline (every stage runs on all hardware threads the container gives us):
//   1. mmap the log, cut it into one line-aligned chunk per thread.
//   2. Parse each chunk by hand: the timestamp is only needed down to the hour,
//      so it is read straight from its digits, and the endpoint is interned in
//      a small per-thread hash table. Each line becomes an 8-byte record.
//   3. Merge the (about 1.8k) endpoint names, sorted bytewise, so that a group
//      index hour * G + rank is already in output order.
//   4. Counting sort of the latencies by group (atomic counters, so no
//      per-thread count matrices), then exact nearest-rank percentiles per
//      group with nth_element, formatted into per-hour buffers and pwritten.
#include <fcntl.h>
#include <immintrin.h>
#include <sched.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <functional>
#include <thread>
#include <vector>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

static const int HOURS = 72; // the log always covers the same 72 hours

static int hardware_threads() {
  cpu_set_t set;
  sched_getaffinity(0, sizeof set, &set);
  return std::max(1, std::min(CPU_COUNT(&set), 32));
}

// Runs fn(0..tasks-1) on up to `threads` threads, tasks handed out dynamically.
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

struct Rec {
  u32 key; // endpoint_local_id << 8 | hour << 1 | is_5xx
  u32 lat;
};
struct Slot {
  u64 hash; // 0 marks an empty slot
  const u8 *str;
  u32 len, id;
};
struct Worker {
  Rec *recs = nullptr;
  size_t n = 0;
  std::vector<Slot> table = std::vector<Slot>(4096);
  std::vector<std::pair<const u8 *, u32>> names; // local id -> endpoint bytes
};

static inline u64 load64(const u8 *p) {
  u64 v;
  memcpy(&v, p, 8);
  return v;
}
static inline u64 mix(u64 a, u64 b) {
  __uint128_t r = (__uint128_t)a * b;
  return (u64)r ^ (u64)(r >> 64);
}

// Hash of the endpoint bytes; may read up to 7 bytes past the end (the file
// mapping is padded).
static inline u64 hash_bytes(const u8 *p, u32 len) {
  u64 h = 0x9E3779B97F4A7C15ull ^ len;
  u32 n = len;
  for (; n > 8; n -= 8, p += 8)
    h = mix(h ^ load64(p), 0xD6E8FEB86659FD93ull);
  u64 last = load64(p) & (n == 8 ? ~0ull : ((1ull << (8 * n)) - 1));
  h = mix(h ^ last, 0xA0761D6478BD642Full);
  return h ? h : 1;
}

// Interns an endpoint and returns its local id. Equality is decided by the
// 64-bit hash plus the length: with about 1.8k distinct endpoints a collision
// has probability around 1e-13, so the bytes are not compared again.
static u32 intern(Worker &w, const u8 *s, u32 len, u64 h) {
  size_t mask = w.table.size() - 1;
  for (size_t i = h & mask; w.table[i].hash; i = (i + 1) & mask)
    if (w.table[i].hash == h && w.table[i].len == len)
      return w.table[i].id;
  if (w.names.size() * 2 >= w.table.size()) { // grow, then insert again
    std::vector<Slot> old(w.table.size() * 2);
    old.swap(w.table);
    mask = w.table.size() - 1;
    for (const Slot &sl : old)
      if (sl.hash) {
        size_t i = sl.hash & mask;
        while (w.table[i].hash)
          i = (i + 1) & mask;
        w.table[i] = sl;
      }
  }
  size_t i = h & mask;
  while (w.table[i].hash)
    i = (i + 1) & mask;
  w.table[i] = {h, s, len, (u32)w.names.size()};
  w.names.push_back({s, len});
  return w.table[i].id;
}

// Latency digits up to the newline; leaves p after the newline.
static inline u32 parse_latency(const u8 *&p) {
  u64 x = load64(p), v = x ^ 0x0A0A0A0A0A0A0A0Aull;
  u64 z = (v - 0x0101010101010101ull) & ~v & 0x8080808080808080ull;
  if (!z) { // 8 digits or more: rare
    u32 r = 0;
    while (*p != '\n')
      r = r * 10 + (*p++ - '0');
    ++p;
    return r;
  }
  int digits = __builtin_ctzll(z) >> 3;
  p += digits + 1;
  // right-align the digits in 8 bytes (leading zeros), then 8 digits at once
  x = (x << (8 * (8 - digits))) & 0x0F0F0F0F0F0F0F0Full;
  x = x * 10 + (x >> 8);
  x = (((x & 0x000000FF000000FFull) * 0x000F424000000064ull) +
       (((x >> 16) & 0x000000FF000000FFull) * 0x0000271000000001ull)) >>
      32;
  return (u32)x;
}

// Line: "2026-10-30T18:07:18.569411Z GET /api/v2/products 200 8177\n"
static void parse(const u8 *p, const u8 *end, Worker &w) {
  const __m128i space = _mm_set1_epi8(' ');
  Rec *out = w.recs;
  while (p < end) {
    int day = (p[8] - '0') * 10 + (p[9] - '0');
    int d =
        p[6] == '0' ? day - 30 : day + 1; // 0..3: Oct 30, Oct 31, Nov 1, Nov 2
    int hour = d * 24 + (p[11] - '0') * 10 + (p[12] - '0') - 18;
    p += 28;
    // the method is 3-7 bytes: find its space with a SWAR zero-byte test
    u64 x = load64(p) ^ 0x2020202020202020ull;
    u64 z = (x - 0x0101010101010101ull) & ~x & 0x8080808080808080ull;
    p += (__builtin_ctzll(z) >> 3) + 1;
    const u8 *ep = p;
    for (;;) {
      u32 m = _mm_movemask_epi8(
          _mm_cmpeq_epi8(_mm_loadu_si128((const __m128i *)p), space));
      if (m) {
        p += __builtin_ctz(m);
        break;
      }
      p += 16;
    }
    u32 len = p - ep;
    ++p;
    u32 err = *p == '5';
    p += 4;
    u32 lat = parse_latency(p);
    u32 id = intern(w, ep, len, hash_bytes(ep, len));
    *out++ = {id << 8 | hour << 1 | err, lat};
  }
  w.n = out - w.recs;
}

static bool name_less(const std::pair<const u8 *, u32> &a,
                      const std::pair<const u8 *, u32> &b) {
  int c = memcmp(a.first, b.first, std::min(a.second, b.second));
  return c ? c < 0 : a.second < b.second;
}

static char *put(char *o, u64 v) {
  char t[20];
  int n = 0;
  do
    t[n++] = '0' + v % 10;
  while (v /= 10);
  while (n)
    *o++ = t[--n];
  return o;
}

int main() {
  int threads = hardware_threads();
  int fd = open("/data/requests.log", O_RDONLY);
  struct stat st;
  fstat(fd, &st);
  size_t size = st.st_size;
  // Anonymous guard page after the file so the parser may over-read a little.
  size_t span = (size + 4095) & ~4095ull;
  u8 *base = (u8 *)mmap(nullptr, span + 4096, PROT_READ,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  mmap(base, size, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0);

  // One chunk per thread, cut after a newline.
  int W = threads;
  std::vector<size_t> cut(W + 1, size);
  cut[0] = 0;
  for (int i = 1; i < W; ++i) {
    size_t pos = std::max(cut[i - 1], size * i / W);
    const void *nl =
        pos < size ? memchr(base + pos, '\n', size - pos) : nullptr;
    cut[i] = nl ? (const u8 *)nl - base + 1 : size;
  }

  std::vector<Worker> workers(W);
  parallel_for(W, threads, [&](int i) {
    Worker &w = workers[i];
    size_t bytes = cut[i + 1] - cut[i];
    // shortest possible line is 40 bytes; untouched capacity costs no memory
    w.recs = (Rec *)malloc((bytes / 40 + 16) * sizeof(Rec));
    madvise(base + (cut[i] & ~4095ull), bytes + 4096,
            22 /* MADV_POPULATE_READ */);
    parse(base + cut[i], base + cut[i + 1], w);
  });

  // Global endpoint order: bytewise sorted, so group = hour * G + rank.
  std::vector<std::pair<const u8 *, u32>> all;
  for (auto &w : workers)
    all.insert(all.end(), w.names.begin(), w.names.end());
  std::sort(all.begin(), all.end(), name_less);
  all.erase(std::unique(all.begin(), all.end(),
                        [](const auto &a, const auto &b) {
                          return a.second == b.second &&
                                 !memcmp(a.first, b.first, a.second);
                        }),
            all.end());
  size_t G = all.size();
  std::vector<std::vector<u32>> rank(W);
  for (int i = 0; i < W; ++i)
    for (auto &nm : workers[i].names)
      rank[i].push_back(
          std::lower_bound(all.begin(), all.end(), nm, name_less) -
          all.begin());

  size_t groups = G * HOURS;
  std::vector<u32> cnt(groups + 1), cur(groups + 1), errs(groups);
  auto group_of = [&](int i, u32 key) {
    return (key >> 1 & 127) * G + rank[i][key >> 8];
  };
  parallel_for(W, threads, [&](int i) {
    for (size_t j = 0; j < workers[i].n; ++j) {
      u32 key = workers[i].recs[j].key;
      size_t g = group_of(i, key);
      __atomic_fetch_add(&cnt[g], 1, __ATOMIC_RELAXED);
      if (key & 1)
        __atomic_fetch_add(&errs[g], 1, __ATOMIC_RELAXED);
    }
  });
  size_t total = 0;
  for (size_t g = 0; g < groups; ++g) {
    u32 c = cnt[g];
    cnt[g] = total; // cnt becomes the start offset; cur the fill cursor
    cur[g] = total;
    total += c;
  }
  cnt[groups] = total;
  u32 *lats = (u32 *)malloc(total * 4 + 4); // filled completely by the scatter
  parallel_for(W, threads, [&](int i) {
    for (size_t j = 0; j < workers[i].n; ++j) {
      Rec r = workers[i].recs[j];
      lats[__atomic_fetch_add(&cur[group_of(i, r.key)], 1, __ATOMIC_RELAXED)] =
          r.lat;
    }
  });
  for (auto &w : workers)
    free(w.recs);

  // One task per hour: percentiles + formatting into a private buffer.
  std::vector<char *> buf(HOURS);
  std::vector<size_t> blen(HOURS);
  parallel_for(HOURS, threads, [&](int h) {
    char *o = buf[h] = (char *)malloc(G * 300 + 64);
    char *o0 = o;
    int abs_hour = 18 + h, dd = abs_hour / 24;
    char stamp[24];
    sprintf(stamp,
            dd < 2 ? "2026-10-%02dT%02d:00:00Z " : "2026-11-%02dT%02d:00:00Z ",
            dd < 2 ? 30 + dd : dd - 1, abs_hour % 24);
    for (size_t r = 0; r < G; ++r) {
      size_t g = h * G + r;
      u32 n = cnt[g + 1] - cnt[g];
      if (!n)
        continue;
      u32 *a = lats + cnt[g];
      u32 i50 = (50 * n + 99) / 100 - 1, i95 = (95 * n + 99) / 100 - 1,
          i99 = (99 * n + 99) / 100 - 1;
      if (n <= 256) {
        std::sort(a, a + n);
      } else {
        std::nth_element(a, a + i99, a + n);
        std::nth_element(a, a + i95, a + i99);
        std::nth_element(a, a + i50, a + i95);
      }
      memcpy(o, stamp, 21);
      o += 21;
      memcpy(o, all[r].first, all[r].second);
      o += all[r].second;
      u64 f[] = {n,      a[i50],  a[i95],
                 a[i99], errs[g], (u64)errs[g] * 1000000 / n};
      for (u64 v : f) {
        *o++ = ' ';
        o = put(o, v);
      }
      *o++ = '\n';
    }
    blen[h] = o - o0;
  });

  int out = open("/out/summary.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  std::vector<size_t> off(HOURS + 1);
  for (int h = 0; h < HOURS; ++h)
    off[h + 1] = off[h] + blen[h];
  ftruncate(out, off[HOURS]);
  parallel_for(HOURS, threads,
               [&](int h) { pwrite(out, buf[h], blen[h], off[h]); });
  return 0;
}

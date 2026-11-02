#include <fcntl.h>
#include <immintrin.h>
#include <malloc.h>
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
#include <string>
#include <thread>
#include <vector>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;

static const int HOURS = 73;

static int hardware_threads() {
  cpu_set_t set;
  sched_getaffinity(0, sizeof set, &set);
  return std::max(1, std::min(CPU_COUNT(&set), 32));
}

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
  std::vector<Slot> table = std::vector<Slot>(8192);
  std::vector<std::pair<const u8 *, u32>> names;
  std::vector<u32> my_cnt;
  std::vector<u32> my_err;
};

static inline u64 load64(const u8 *p) {
  u64 v;
  memcpy(&v, p, 8);
  return v;
}

static inline u64 hash_bytes(const u8 *p, u32 len) {
  u64 crc = 0;
  u32 n = len;
  while (n >= 8) {
    crc = _mm_crc32_u64(crc, load64(p));
    p += 8;
    n -= 8;
  }
  if (n) {
    u64 last = load64(p) & ((1ull << (8 * n)) - 1);
    crc = _mm_crc32_u64(crc, last);
  }
  return crc ? crc : 1;
}

static u32 intern(Worker &w, const u8 *s, u32 len, u64 h) {
  size_t mask = w.table.size() - 1;
  for (size_t i = h & mask; w.table[i].hash; i = (i + 1) & mask)
    if (w.table[i].hash == h && w.table[i].len == len)
      return w.table[i].id;
  if (w.names.size() * 2 >= w.table.size()) {
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

static inline u32 parse_latency(const u8 *&p) {
  u64 x = load64(p), v = x ^ 0x0A0A0A0A0A0A0A0Aull;
  u64 z = (v - 0x0101010101010101ull) & ~v & 0x8080808080808080ull;
  if (!z) {
    u32 r = 0;
    while (*p != '\n')
      r = r * 10 + (*p++ - '0');
    ++p;
    return r;
  }
  int digits = __builtin_ctzll(z) >> 3;
  p += digits + 1;
  x = (x << (8 * (8 - digits))) & 0x0F0F0F0F0F0F0F0Full;
  x = x * 10 + (x >> 8);
  x = (((x & 0x000000FF000000FFull) * 0x000F424000000064ull) +
       (((x >> 16) & 0x000000FF000000FFull) * 0x0000271000000001ull)) >>
      32;
  return (u32)x;
}

static void parse(const u8 *p, const u8 *end, Worker &w) {
  const __m256i space = _mm256_set1_epi8(' ');
  Rec *out = w.recs;
  while (p < end) {
    int day = (p[8] - '0') * 10 + (p[9] - '0');
    int d = p[6] == '0' ? day - 30 : day + 1;
    int hour = d * 24 + (p[11] - '0') * 10 + (p[12] - '0') - 18;
    p += 28;
    u64 x = load64(p) ^ 0x2020202020202020ull;
    u64 z = (x - 0x0101010101010101ull) & ~x & 0x8080808080808080ull;
    p += (__builtin_ctzll(z) >> 3) + 1;
    const u8 *ep = p;
    u32 m = _mm256_movemask_epi8(
        _mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *)p), space));
    if (m) {
      p += __builtin_ctz(m);
    } else {
      p += 32;
      while (*p != ' ') ++p;
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

int main(int argc, char **argv) {
  mallopt(-8, 2);
  std::string data_dir = "/data";
  std::string out_dir = "/out";
  for (int i = 1; i < argc; ++i) {
    if (std::string(argv[i]) == "--data" && i + 1 < argc) data_dir = argv[++i];
    else if (std::string(argv[i]) == "--out" && i + 1 < argc) out_dir = argv[++i];
  }

  int threads = hardware_threads();
  std::string log_path = data_dir + "/requests.log";
  std::string out_path = out_dir + "/summary.txt";
  int fd = open(log_path.c_str(), O_RDONLY);
  struct stat st;
  fstat(fd, &st);
  size_t size = st.st_size;
  size_t span = (size + 4095) & ~4095ull;
  u8 *base = (u8 *)mmap(nullptr, span + 4096, PROT_READ,
                        MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
  mmap(base, size, PROT_READ, MAP_PRIVATE | MAP_FIXED, fd, 0);

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
    w.recs = (Rec *)malloc((bytes / 40 + 16) * sizeof(Rec));
    madvise(base + (cut[i] & ~4095ull), bytes + 4096, 22 /* MADV_POPULATE_READ */);
    parse(base + cut[i], base + cut[i + 1], w);
  });

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
  parallel_for(W, threads, [&](int i) {
    workers[i].my_cnt.assign(groups, 0);
    workers[i].my_err.assign(groups, 0);
    u32 *my_cnt = workers[i].my_cnt.data();
    u32 *my_err = workers[i].my_err.data();
    const u32 *rk = rank[i].data();
    for (size_t j = 0; j < workers[i].n; ++j) {
      u32 key = workers[i].recs[j].key;
      size_t g = (key >> 1 & 127) * G + rk[key >> 8];
      my_cnt[g]++;
      if (key & 1) my_err[g]++;
    }
  });

  std::vector<u32> cnt(groups + 1, 0), errs(groups, 0);
  std::vector<std::vector<u32>> cur(W, std::vector<u32>(groups, 0));
  size_t total = 0;
  for (size_t g = 0; g < groups; ++g) {
    cnt[g] = total;
    for (int i = 0; i < W; ++i) {
      cur[i][g] = total;
      total += workers[i].my_cnt[g];
      errs[g] += workers[i].my_err[g];
    }
  }
  cnt[groups] = total;

  u32 *lats = (u32 *)malloc(total * 4 + 4);
  parallel_for(W, threads, [&](int i) {
    u32 *my_cur = cur[i].data();
    const u32 *rk = rank[i].data();
    for (size_t j = 0; j < workers[i].n; ++j) {
      Rec r = workers[i].recs[j];
      size_t g = (r.key >> 1 & 127) * G + rk[r.key >> 8];
      lats[my_cur[g]++] = r.lat;
    }
    free(workers[i].recs);
    std::vector<u32>().swap(workers[i].my_cnt);
    std::vector<u32>().swap(workers[i].my_err);
  });

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

  int out = open(out_path.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0644);
  std::vector<size_t> off(HOURS + 1);
  for (int h = 0; h < HOURS; ++h)
    off[h + 1] = off[h] + blen[h];
  ftruncate(out, off[HOURS]);
  parallel_for(HOURS, threads,
               [&](int h) { pwrite(out, buf[h], blen[h], off[h]); });
  close(out);
  return 0;
}

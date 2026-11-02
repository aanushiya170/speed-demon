// Track D (Traverse): 2,000 shortest-path queries on a 1.7M-node, 5M-edge
// directed road-like graph.
//
// Where the time goes, and what is done about it:
//   * Parsing 92 MB of "u v w" lines: parallel, line-aligned chunks, a SWAR
//     number parser, degrees counted while parsing.
//   * Cache misses. The node ids are a random relabelling of a road network,
//     so a search ball would touch memory all over a 47 MB graph. After the
//     first CSR is built, the nodes are renumbered in BFS order (neighbours
//     get nearby ids) and the CSR is rebuilt in the new numbering.
//   * Searches: forward Dijkstra with a radix heap, stopping when the target
//     is settled. The distance array holds 32-bit values (everything on the
//     map is far below 2^32); a search that would overflow is redone with
//     64-bit values, which only a handful of special queries need.
//   * A target without in-edges is unreachable: answered from an in-degree
//     bitmap instead of settling the whole graph.
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
#include <thread>
#include <vector>

typedef uint8_t u8;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int64_t i64;

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
  if (!z) { // 8 digits or more
    u64 r = 0;
    while (*p != T)
      r = r * 10 + (*p++ - '0');
    ++p;
    return r;
  }
  int n = __builtin_ctzll(z) >> 3;
  p += n + 1;
  x = (x << (8 * (8 - n))) &
      0x0F0F0F0F0F0F0F0Full; // right-align, leading zeros
  x = x * 10 + (x >> 8);
  return (((x & 0x000000FF000000FFull) * 0x000F424000000064ull) +
          (((x >> 16) & 0x000000FF000000FFull) * 0x0000271000000001ull)) >>
         32;
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

static size_t count_newlines(const u8 *p, const u8 *end) {
  size_t c = 0;
  const __m256i nl = _mm256_set1_epi8('\n');
  for (; p + 32 <= end; p += 32)
    c += __builtin_popcount(_mm256_movemask_epi8(
        _mm256_cmpeq_epi8(_mm256_loadu_si256((const __m256i *)p), nl)));
  for (; p < end; ++p)
    c += *p == '\n';
  return c;
}

struct Edge {
  u32 to, w;
};

// Monotone priority queue for Dijkstra: an item goes into the bucket given by
// the highest bit in which its key differs from the last key popped.
template <class K> struct RadixHeap {
  struct Item {
    K key;
    u32 node;
  };
  static const int BITS = sizeof(K) * 8;
  std::vector<Item> bucket[BITS + 1];
  K last = 0;
  size_t size = 0;
  static int index(K x) {
    return x == 0 ? 0
                  : BITS - (BITS == 32 ? __builtin_clz((u32)x)
                                       : __builtin_clzll(x));
  }
  void push(K key, u32 node) {
    bucket[index(key ^ last)].push_back({key, node});
    ++size;
  }
  Item pop() {
    if (bucket[0].empty()) {
      int i = 1;
      while (bucket[i].empty())
        ++i;
      K mn = bucket[i][0].key;
      for (const Item &it : bucket[i])
        mn = std::min(mn, it.key);
      last = mn;
      for (const Item &it : bucket[i])
        bucket[index(it.key ^ last)].push_back(it);
      bucket[i].clear();
    }
    Item it = bucket[0].back();
    bucket[0].pop_back();
    --size;
    return it;
  }
  void clear() {
    for (auto &b : bucket)
      b.clear();
    last = 0;
    size = 0;
  }
};

struct Graph {
  const u32 *off; // n + 1
  const Edge *edges;
};

// One thread's search state. dist holds distance + 1 (0 = not reached), so
// the array can come zero-filled from the OS and be reset through `touched`.
template <class K> struct Search {
  K *dist = nullptr;
  std::vector<u32> touched;
  RadixHeap<K> heap;

  // Distance from s to t, -1 if unreachable, -2 if a K overflow happened.
  i64 run(const Graph &g, size_t n, u32 s, u32 t) {
    if (!dist)
      dist = (K *)calloc(n, sizeof(K));
    i64 result = -1;
    dist[s] = 1;
    touched.push_back(s);
    heap.push(0, s);
    while (heap.size) {
      auto [d, u] = heap.pop();
      if (dist[u] != d + 1)
        continue; // stale entry
      if (u == t) {
        result = d;
        break;
      }
      for (const Edge *e = g.edges + g.off[u]; e != g.edges + g.off[u + 1];
           ++e) {
        u64 nd = (u64)d + e->w;
        if (sizeof(K) == 4 && nd >= 0xFFFFFFFEull) {
          result = -2;
          break;
        }
        K &cur = dist[e->to];
        if (cur == 0)
          touched.push_back(e->to);
        if (cur == 0 || nd + 1 < cur) {
          cur = nd + 1;
          heap.push(nd, e->to);
        }
      }
      if (result == -2)
        break;
    }
    for (u32 v : touched)
      dist[v] = 0;
    touched.clear();
    heap.clear();
    return result;
  }
};

int main() {
  cpu_set_t set;
  sched_getaffinity(0, sizeof set, &set);
  int T = std::max(1, std::min(CPU_COUNT(&set), 32));

  // ---------------- parse graph.txt ----------------
  size_t gsize;
  const u8 *g = map_file("/data/graph.txt", &gsize);
  const u8 *p = g;
  size_t n = digits<' '>(p), m = digits<'\n'>(p);
  std::vector<const u8 *> cut(T + 1, g + gsize);
  cut[0] = p;
  for (int i = 1; i < T; ++i) {
    const u8 *q = std::max(cut[i - 1], p + (g + gsize - p) * i / T);
    q = (const u8 *)memchr(q, '\n', g + gsize - q);
    cut[i] = q ? q + 1 : g + gsize;
  }
  std::vector<size_t> first(T + 1);
  parallel_for(
      T, T, [&](int i) { first[i + 1] = count_newlines(cut[i], cut[i + 1]); });
  for (int i = 0; i < T; ++i)
    first[i + 1] += first[i];

  u32 *eu = (u32 *)malloc(m * 4);
  u32 *ev = (u32 *)malloc(m * 4);
  u32 *ew = (u32 *)malloc(m * 4);
  u32 *deg = (u32 *)calloc(n + 1, 4); // later: fill cursor of the first CSR
  u64 *has_in = (u64 *)calloc(n / 64 + 1,
                              8); // nodes with an in-edge (self-loops ignored)
  parallel_for(T, T, [&](int i) {
    size_t k = first[i];
    for (const u8 *q = cut[i]; q < cut[i + 1]; ++k) {
      u32 u = digits<' '>(q), v = digits<' '>(q), w = digits<'\n'>(q);
      eu[k] = u, ev[k] = v, ew[k] = w;
      if (u != v) { // a self-loop never lies on a shortest path
        __atomic_fetch_add(&deg[u], 1, __ATOMIC_RELAXED);
        __atomic_fetch_or(&has_in[v >> 6], 1ull << (v & 63), __ATOMIC_RELAXED);
      }
    }
  });

  // ---------------- first CSR (random node numbering) ----------------
  u32 *off1 = (u32 *)malloc((n + 1) * 4);
  size_t m1 = 0;
  for (size_t i = 0; i < n; ++i) {
    off1[i] = m1;
    m1 += deg[i];
    deg[i] = off1[i];
  }
  off1[n] = m1;
  Edge *edges1 = (Edge *)malloc(m1 * sizeof(Edge) + 8);
  parallel_for(T, T, [&](int i) {
    for (size_t k = first[i]; k < first[i + 1]; ++k)
      if (eu[k] != ev[k])
        edges1[__atomic_fetch_add(&deg[eu[k]], 1, __ATOMIC_RELAXED)] = {ev[k],
                                                                        ew[k]};
  });
  free(eu), free(ev), free(ew), free(deg);

  // ---------------- BFS renumbering ----------------
  u32 *order = (u32 *)malloc(n * 4); // new id -> old id
  u32 *perm = (u32 *)malloc(n * 4);  // old id -> new id
  std::vector<u64> seen(n / 64 + 1);
  size_t head = 0, tail = 0;
  auto bfs = [&](u32 s) {
    if (seen[s >> 6] >> (s & 63) & 1)
      return;
    seen[s >> 6] |= 1ull << (s & 63);
    perm[s] = tail;
    order[tail++] = s;
    for (; head < tail; ++head) {
      if (head + 16 < tail)
        __builtin_prefetch(&off1[order[head + 16]]);
      if (head + 8 < tail)
        __builtin_prefetch(&edges1[off1[order[head + 8]]]);
      u32 u = order[head];
      for (const Edge *e = edges1 + off1[u]; e != edges1 + off1[u + 1]; ++e)
        if (!(seen[e->to >> 6] >> (e->to & 63) & 1)) {
          seen[e->to >> 6] |= 1ull << (e->to & 63);
          perm[e->to] = tail;
          order[tail++] = e->to;
        }
    }
  };
  u32 hub = 0; // start in the biggest component: the node with most out-edges
  for (u32 v = 1; v < n; ++v)
    if (off1[v + 1] - off1[v] > off1[hub + 1] - off1[hub])
      hub = v;
  // Phase 1, in parallel: every thread grows a BFS from its own seed and
  // claims nodes with an atomic test-and-set, so the graph is split into one
  // contiguous BFS region per thread.
  std::vector<std::vector<u32>> part(T);
  auto claim = [&](u32 v) {
    return !(
        __atomic_fetch_or(&seen[v >> 6], 1ull << (v & 63), __ATOMIC_RELAXED) >>
            (v & 63) &
        1);
  };
  parallel_for(T, T, [&](int i) {
    u32 s = i == 0 ? hub : n * i / T;
    while (i && s + 1 < n && off1[s + 1] - off1[s] < 2)
      ++s;
    std::vector<u32> &q = part[i];
    if (claim(s))
      q.push_back(s);
    for (size_t h = 0; h < q.size(); ++h) {
      if (h + 16 < q.size())
        __builtin_prefetch(&off1[q[h + 16]]);
      if (h + 8 < q.size())
        __builtin_prefetch(&edges1[off1[q[h + 8]]]);
      u32 u = q[h];
      for (const Edge *e = edges1 + off1[u]; e != edges1 + off1[u + 1]; ++e)
        if (claim(e->to))
          q.push_back(e->to);
    }
  });
  std::vector<size_t> at(T + 1);
  for (int i = 0; i < T; ++i)
    at[i + 1] = at[i] + part[i].size();
  parallel_for(T, T, [&](int i) {
    for (size_t k = 0; k < part[i].size(); ++k) {
      order[at[i] + k] = part[i][k];
      perm[part[i][k]] = at[i] + k;
    }
    std::vector<u32>().swap(part[i]);
  });
  head = tail = at[T]; // all of these have been expanded already
  // Phase 2: nodes no seed could reach (islands, one-way pieces), serially.
  for (u32 v = 0; v < n; ++v)
    bfs(v);

  // ---------------- second CSR in the new numbering ----------------
  u32 *off2 = (u32 *)malloc((n + 1) * 4);
  Edge *edges2 = (Edge *)malloc(m1 * sizeof(Edge) + 8);
  std::vector<size_t> range(T + 1);
  for (int i = 0; i <= T; ++i)
    range[i] = n * i / T;
  std::vector<u32> base(T + 1);
  parallel_for(T, T, [&](int i) {
    u32 sum = 0;
    for (size_t v = range[i]; v < range[i + 1]; ++v) {
      u32 old = order[v];
      sum += off1[old + 1] - off1[old];
    }
    base[i + 1] = sum;
  });
  for (int i = 0; i < T; ++i)
    base[i + 1] += base[i];
  parallel_for(T, T, [&](int i) {
    u32 o = base[i];
    for (size_t v = range[i]; v < range[i + 1]; ++v) {
      u32 old = order[v];
      off2[v] = o;
      for (const Edge *e = edges1 + off1[old]; e != edges1 + off1[old + 1]; ++e)
        edges2[o++] = {perm[e->to], e->w};
    }
  });
  off2[n] = m1;
  free(edges1), free(off1), free(order);
  Graph graph{off2, edges2};

  // ---------------- queries ----------------
  size_t qsize;
  const u8 *qf = map_file("/data/queries.txt", &qsize);
  size_t Q = count_newlines(qf, qf + qsize);
  std::vector<u32> qs(Q), qt(Q);
  const u8 *q = qf;
  for (size_t i = 0; i < Q; ++i) {
    qs[i] = digits<' '>(q);
    qt[i] = digits<'\n'>(q);
  }
  std::vector<i64> ans(Q);
  parallel_for(Q, T, [&](int i) {
    thread_local Search<u32> fast;
    thread_local Search<u64> wide;
    u32 s = qs[i], t = qt[i];
    if (s == t) {
      ans[i] = 0;
    } else if (!(has_in[t >> 6] >> (t & 63) & 1) ||
               off2[perm[s]] == off2[perm[s] + 1]) {
      ans[i] = -1; // nothing enters t, or nothing leaves s
    } else {
      ans[i] = fast.run(graph, n, perm[s], perm[t]);
      if (ans[i] == -2)
        ans[i] = wide.run(graph, n, perm[s], perm[t]);
    }
  });

  std::vector<char> out(Q * 24 + 16);
  char *o = out.data();
  for (size_t i = 0; i < Q; ++i)
    o += sprintf(o, "%lld\n", (long long)ans[i]);
  int fd = open("/out/distances.txt", O_WRONLY | O_CREAT | O_TRUNC, 0644);
  if (write(fd, out.data(), o - out.data()) < 0)
    return 1;
  return 0;
}
// Speed Demon 2026 revision

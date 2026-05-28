/*===- InstrProfilingCache.c - runtime for the cache-profile pass --------===*\
|*
|* Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
|* See https://llvm.org/LICENSE.txt for license information.
|* SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
|*
\*===----------------------------------------------------------------------===*/

/* Runtime support for the in-tree CacheProfilingPass
 * (llvm/lib/Transforms/Instrumentation/CacheProfiling.cpp).
 *
 * MVP model: direct-mapped L1, 32 KB / 64-byte lines (512 sets). Per-thread
 * tag arrays and counters live in TLS so the inlined fast path is lock-free.
 * Background-thread counters fold into atomic globals via a pthread_key
 * destructor; the main thread is flushed by an atexit() hook, which also
 * writes the summary file (path from $LLVM_CACHE_PROFILE, default
 * "cache.profraw" in cwd).
 *
 * Geometry constants must match the pass.
 */

#include <pthread.h>
#include <stdatomic.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#define CACHE_L1_LINE_LOG2 6
#define CACHE_L1_SETS      512

/* Hot-path symbols referenced directly from the injected IR. */
__thread uint64_t __llvm_cache_l1_tags[CACHE_L1_SETS];
__thread uint64_t __llvm_cache_hits_tls;
/* Cold path. The IR never touches this — only __llvm_cache_miss does. */
__thread uint64_t __llvm_cache_misses_tls;

static _Atomic uint64_t total_hits   = 0;
static _Atomic uint64_t total_misses = 0;

static pthread_key_t  flush_key;
static pthread_once_t flush_key_once = PTHREAD_ONCE_INIT;

static void flush_thread_locals(void) {
  if (__llvm_cache_hits_tls || __llvm_cache_misses_tls) {
    atomic_fetch_add_explicit(&total_hits,   __llvm_cache_hits_tls,
                              memory_order_relaxed);
    atomic_fetch_add_explicit(&total_misses, __llvm_cache_misses_tls,
                              memory_order_relaxed);
    __llvm_cache_hits_tls = 0;
    __llvm_cache_misses_tls = 0;
  }
}

static void flush_key_dtor(void *unused) {
  (void)unused;
  flush_thread_locals();
}

static void make_flush_key(void) {
  pthread_key_create(&flush_key, flush_key_dtor);
}

/* Called once at entry of every instrumented function. After the first
 * (per-thread) call the only cost is a pthread_getspecific. We don't try
 * to short-circuit further — the pass would have to hoist a check out of
 * the function prologue to avoid the call, and it's not worth it for MVP. */
void __llvm_cache_arm_thread(void) {
  pthread_once(&flush_key_once, make_flush_key);
  if (pthread_getspecific(flush_key) == NULL)
    pthread_setspecific(flush_key, (void *)1);
}

/* Cold path called from the IR on an L1 miss. Installs the new tag and
 * bumps the per-thread miss counter. All bookkeeping that used to live
 * inline in IR is funneled through here. */
__attribute__((cold, noinline))
void __llvm_cache_miss(uint64_t tag, uint64_t idx) {
  __llvm_cache_l1_tags[idx] = tag;
  ++__llvm_cache_misses_tls;
}

static void cache_atexit(void) {
  /* Main thread is not in the pthread-key destructor path. */
  flush_thread_locals();

  const char *path = getenv("LLVM_CACHE_PROFILE");
  if (!path || !*path) path = "cache.profraw";

  FILE *f = fopen(path, "w");
  if (!f) return;

  uint64_t h = atomic_load_explicit(&total_hits,   memory_order_relaxed);
  uint64_t m = atomic_load_explicit(&total_misses, memory_order_relaxed);
  uint64_t n = h + m;
  double rate = n ? (100.0 * (double)h / (double)n) : 0.0;

  fprintf(f,
          "# llvm cache profile (direct-mapped L1, %u sets * %u B = %u B)\n"
          "l1_accesses=%llu\n"
          "l1_hits=%llu\n"
          "l1_misses=%llu\n"
          "l1_hit_rate=%.4f\n",
          (unsigned)CACHE_L1_SETS,
          (unsigned)(1u << CACHE_L1_LINE_LOG2),
          (unsigned)(CACHE_L1_SETS << CACHE_L1_LINE_LOG2),
          (unsigned long long)n,
          (unsigned long long)h,
          (unsigned long long)m,
          rate);
  fclose(f);
}

__attribute__((constructor))
static void cache_init(void) {
  atexit(cache_atexit);
}

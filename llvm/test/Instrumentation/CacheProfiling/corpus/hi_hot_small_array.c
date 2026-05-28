// HIGH: 4 KB working set hammered 10 000 times.
// 1024 ints = 4 KB sits in our 32 KB L1 the entire time.
// Expected: ~100% hit rate (one warm-up miss per cache line, then nothing).
#include <stdio.h>
#define N 1024
static int a[N];
volatile long sink;
int main(int argc, char **argv) {
  for (int i = 0; i < N; i++) a[i] = i + argc;
  long s = 0;
  for (int r = 0; r < 10000; r++)
    for (int i = 0; i < N; i++) s += a[i];
  sink = s;
  return 0;
}

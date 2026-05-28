// HIGH: 1 KB stack buffer hammered 100 000 times.
// Tiny working set, sits in L1 forever after the first touch.
// Expected: ~100% hit rate.
#include <stdio.h>
#define N 256
volatile long sink;
int main(int argc, char **argv) {
  int a[N];
  for (int i = 0; i < N; i++) a[i] = i + argc;
  long s = 0;
  for (int r = 0; r < 100000; r++)
    for (int i = 0; i < N; i++) s += a[i];
  sink = s;
  return 0;
}

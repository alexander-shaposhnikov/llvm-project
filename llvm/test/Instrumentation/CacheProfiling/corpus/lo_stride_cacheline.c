// LOW: 16 MB array, stride = 64 B = one cache line per access.
// Every access lands on a fresh line; 16 MB >> 32 KB L1 so no reuse.
// The argc-store opacifies the array (otherwise -O2 const-folds it to 0).
// Expected: hit rate close to 0%.
#include <stdio.h>
#define N (4 * 1024 * 1024)
static int a[N];  // BSS
volatile long sink;
int main(int argc, char **argv) {
  a[argc] = argc;            // opaque single write so the array isn't all-0
  long s = 0;
  for (int i = 0; i < N; i += 16) s += a[i];
  sink = s;
  return 0;
}

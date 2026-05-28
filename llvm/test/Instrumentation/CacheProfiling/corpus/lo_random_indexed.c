// LOW: random index into a 4 MB array. Each access is essentially a fresh
// cache line; 4 MB >> 32 KB L1 so virtually no reuse. RNG state and loop
// counter stay in registers under -O2.
// Expected: hit rate close to 0%.
#include <stdio.h>
#define N      (1024 * 1024)
#define ITERS  (256 * 1024)
static int data[N];  // BSS
volatile long sink;
int main(int argc, char **argv) {
  data[argc] = argc;          // opaque single write
  unsigned r = 0x9E3779B1u ^ (unsigned)argc;
  long s = 0;
  for (int i = 0; i < ITERS; i++) {
    r = r * 1103515245u + 12345u;
    s += data[r % N];
  }
  sink = s;
  return 0;
}

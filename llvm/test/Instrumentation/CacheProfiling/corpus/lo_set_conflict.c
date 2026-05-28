// LOW: 64 addresses all spaced exactly STRIDE = 32 KB apart. In our
// 32 KB / 64 B direct-mapped cache they ALL map to set 0 (index =
// (addr>>6) & 511 = i * 512 mod 512 = 0). The cache can hold one line
// at a time; every other access evicts the previous resident.
// Expected: hit rate close to 0%.
#include <stdio.h>
#define STRIDE (32 * 1024)
#define NPTRS  64
static char buf[STRIDE * NPTRS + 64];
volatile long sink;
int main(int argc, char **argv) {
  buf[argc] = (char)argc;     // opaque single write
  long s = 0;
  for (int r = 0; r < 10000; r++)
    for (int i = 0; i < NPTRS; i++) s += buf[i * STRIDE];
  sink = s;
  return 0;
}

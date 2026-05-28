// LOW: 1024x1024 int matrix (4 MB), column-major read.
// Stride = 4 KB per next-access. 4 MB >> 32 KB L1 so no reuse between
// columns. The argc-store opacifies the BSS so -O2 doesn't fold it.
// Expected: hit rate close to 0%.
#include <stdio.h>
#define N 1024
static int m[N][N];  // BSS
volatile long sink;
int main(int argc, char **argv) {
  m[argc][argc] = argc;       // opaque single write
  long s = 0;
  for (int j = 0; j < N; j++)
    for (int i = 0; i < N; i++) s += m[i][j];
  sink = s;
  return 0;
}

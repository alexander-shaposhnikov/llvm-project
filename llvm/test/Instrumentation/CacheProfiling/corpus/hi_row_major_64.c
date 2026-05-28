// HIGH: 16 KB matrix, row-major scan, repeated 1 000 times.
// 16 KB < 32 KB L1 so the whole matrix is hot after the first pass.
// Expected: ~100% hit rate.
#include <stdio.h>
#define N 64
static int m[N][N];
volatile long sink;
int main(int argc, char **argv) {
  for (int i = 0; i < N; i++)
    for (int j = 0; j < N; j++) m[i][j] = i + j + argc;
  long s = 0;
  for (int r = 0; r < 1000; r++)
    for (int i = 0; i < N; i++)
      for (int j = 0; j < N; j++) s += m[i][j];
  sink = s;
  return 0;
}

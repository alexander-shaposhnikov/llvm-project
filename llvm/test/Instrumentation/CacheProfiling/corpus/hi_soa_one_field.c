// HIGH: SoA layout, iterate only the xs field.
// xs is 4 KB, sequential, repeated -- perfect spatial+temporal locality.
// Expected: ~100% hit rate.
#include <stdio.h>
#define N 1024
typedef struct { int xs[N], ys[N], zs[N]; } S;
static S g;
volatile long sink;
int main(int argc, char **argv) {
  for (int i = 0; i < N; i++) {
    g.xs[i] = i + argc;
    g.ys[i] = -i;
    g.zs[i] = i * 2;
  }
  long s = 0;
  for (int r = 0; r < 10000; r++)
    for (int i = 0; i < N; i++) s += g.xs[i];
  sink = s;
  return 0;
}

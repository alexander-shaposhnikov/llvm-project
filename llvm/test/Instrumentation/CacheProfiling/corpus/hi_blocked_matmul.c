// HIGH: 64x64 matmul with B=16 blocking. Each tile is 1 KB; tile-triple
// (A_tile + B_tile + C_tile) is 3 KB -- easily fits in L1 for the inner
// triple loop. Without blocking this matmul would thrash C and B.
// Expected: ~99% hit rate.
#include <stdio.h>
#define N 64
#define B 16
static int A[N][N], BM[N][N], C[N][N];
volatile long sink;
int main(int argc, char **argv) {
  for (int i = 0; i < N; i++)
    for (int j = 0; j < N; j++) {
      A[i][j]  = i + j + argc;
      BM[i][j] = i - j + argc;
    }
  for (int ii = 0; ii < N; ii += B)
    for (int jj = 0; jj < N; jj += B)
      for (int kk = 0; kk < N; kk += B)
        for (int i = ii; i < ii + B; i++)
          for (int j = jj; j < jj + B; j++) {
            int acc = C[i][j];
            for (int k = kk; k < kk + B; k++) acc += A[i][k] * BM[k][j];
            C[i][j] = acc;
          }
  long s = 0;
  for (int i = 0; i < N; i++) for (int j = 0; j < N; j++) s += C[i][j];
  sink = s;
  return 0;
}

// LOW: 256 K linked-list nodes (16 MB), each `next` scrambled to a
// far-away node via a multiplicative hash. Every p->next deref lands on
// a fresh cache line; the only reuse is the few starting nodes.
// Expected: hit rate close to 0% (a few % at most from setup).
#include <stdio.h>
#define N (256 * 1024)
typedef struct Node { struct Node *next; long pad[7]; } Node; // 64 B
static Node nodes[N];
volatile long sink;
static unsigned scramble(unsigned x) { return x * 2654435769u; }
int main(int argc, char **argv) {
  for (unsigned i = 0; i < N; i++)
    nodes[i].next = &nodes[scramble(i + (unsigned)argc) % N];
  long s = 0;
  Node *p = &nodes[0];
  for (int k = 0; k < N; k++) { s += (long)p; p = p->next; }
  sink = s;
  return 0;
}

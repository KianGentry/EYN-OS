#include <stdio.h>
#include <string.h>

// Deterministic ring3 smoke test:
// - verifies passing/returning small structs
// - verifies stack locals + memcpy/memset
// - exercises a small amount of call/return pressure

struct Pair {
  int x;
  int y;
};

static struct Pair make_pair(int x, int y) {
  struct Pair p;
  p.x = x;
  p.y = y;
  return p;
}

static struct Pair add_pair(struct Pair a, struct Pair b) {
  return make_pair(a.x + b.x, a.y + b.y);
}

static int dot_pair(struct Pair p) {
  return p.x * 7 + p.y * 13;
}

static int fib(int n) {
  if (n <= 1)
    return n;
  return fib(n - 1) + fib(n - 2);
}

int main(void) {
  int ok = 1;

  struct Pair a = make_pair(10, 20);
  struct Pair b = make_pair(-3, 5);
  struct Pair c = add_pair(a, b);

  if (c.x != 7 || c.y != 25)
    ok = 0;

  int dot = dot_pair(c);
  if (dot != (7 * 7 + 25 * 13))
    ok = 0;

  // Simple memory ops on stack.
  char buf[16];
  memset(buf, 0, sizeof(buf));
  memcpy(buf, "EYNOS", 5);
  {
    const char *want = "EYNOS";
    for (int i = 0; i < 5; i++) {
      if (buf[i] != want[i]) {
        ok = 0;
        break;
      }
    }
  }

  // Small recursion to test call/ret and stack.
  int f10 = fib(10);
  if (f10 != 55)
    ok = 0;

  // Deterministic checksum-like value.
  unsigned score = (unsigned)(dot + f10 + (int)buf[0] + (int)buf[4]);

  printf("ring3_smoke_structs: %s (score=%u)\n", ok ? "OK" : "FAIL", score);
  return ok ? 0 : 1;
}

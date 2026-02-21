#include <stdio.h>

// Deterministic ring3 smoke test:
// - verifies .data init values
// - verifies .bss zeroing
// - verifies basic struct load/store

int g_init = 0x12345678;
int g_bss;

static int g_static_bss;
static int g_static_init = 0x0BADF00D;

struct S {
  int a;
  short b;
  char c;
  unsigned char d;
};

static struct S g_s = { 1, 2, 3, 4 };

static unsigned expected_sum(void) {
  // g_bss + g_static_bss are expected to be 0.
  return (unsigned)0x12345678u + (unsigned)0x0BADF00Du + 1u + 2u + 3u + 4u;
}

int main(void) {
  int ok = 1;

  if (g_bss != 0)
    ok = 0;
  if (g_static_bss != 0)
    ok = 0;

  if (g_init != 0x12345678)
    ok = 0;
  if (g_static_init != 0x0BADF00D)
    ok = 0;

  // Exercise whole-struct load/copy.
  struct S s = g_s;
  if (s.a != 1 || s.b != 2 || s.c != 3 || s.d != 4)
    ok = 0;

  unsigned sum = (unsigned)g_init + (unsigned)g_static_init +
                 (unsigned)s.a + (unsigned)s.b + (unsigned)s.c + (unsigned)s.d +
                 (unsigned)g_bss + (unsigned)g_static_bss;

  if (sum != expected_sum())
    ok = 0;

  printf("ring3_smoke_globals: %s (sum=%u)\n", ok ? "OK" : "FAIL", sum);
  return ok ? 0 : 1;
}

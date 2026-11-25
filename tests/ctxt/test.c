#include <stdio.h>
#include <stdint.h>
typedef uint16_t USHORT; typedef uint32_t ULONG;
#define FAR
#include "block.inc"
#define SEP1 0xFFFFCCCCUL
#define SEP2 0xAAAABBBBUL
int main(void) {
  /* RT: 4 lmac sections, separator, 4 umac, paging separator, 2 paging, (trailing separator) */
  ULONG rt[13] = { 0x404000, 0x800000, 0, 0x456000, SEP1, 0x405000, 0xC0080000UL, 0xC0880000UL, 0x80448000UL, SEP2, 0, 0x1000000, SEP1 };
  /* INIT: 9 sections and no separator at all */
  ULONG in[9] = { 0x404000, 0x800000, 0, 0x456000, 0x410000, 0x405000, 0xC0080000UL, 0xC0880000UL, 0x80448000UL };
  int fails = 0;
  USHORT l = sec_count(rt, 13, 0), u = sec_count(rt, 13, l + 1), p = sec_count(rt, 13, l + u + 2);
  if (l != 4 || u != 4 || p != 2) { printf("FAIL RT %d %d %d\n", l, u, p); fails++; }
  l = sec_count(in, 9, 0); u = sec_count(in, 9, l + 1); p = sec_count(in, 9, l + u + 2);
  if (l != 9 || u != 0 || p != 0) { printf("FAIL INIT %d %d %d\n", l, u, p); fails++; }
  printf(fails ? "ctxt: FAIL\n" : "ctxt: ok\n");
  return fails != 0;
}

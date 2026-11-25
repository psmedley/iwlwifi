#include <stdio.h>
#include <stdint.h>
typedef uint16_t USHORT;
#include "block.inc"
int main(void) {
  int fails = 0, cur, idx, n = 0;
  for (cur = 0; cur < 256; cur++)
    for (idx = 0; idx < 256; idx++) {
      int d = (idx - cur) & 255, want = (d == 64 || d == 128 || d == 192);
      if (agg_move_bug(cur, idx) != want) fails++;
      n += want;
    }
  if (n != 3 * 256) fails++;
  if (agg_move_bug(10, 10) || !agg_move_bug(0, 64) || !agg_move_bug(200, 8) /* (8-200)&255 = 64 */ ) fails++;
  printf(fails ? "txagg: FAIL (%d)\n" : "txagg: ok\n", fails);
  return fails != 0;
}

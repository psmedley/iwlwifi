# Building

Open Watcom C 2.0 (16-bit `wcc`, `wlink`) and NASM. Nothing from the IBM DDK.

## Linux (cross build)

```sh
export WATCOM=/path/to/open-watcom-2
export PATH=$WATCOM/binl64:$WATCOM/binl:$PATH
export INCLUDE=
make -C src
```

The result is `src/IWLWIFI.OS2` (a 16-bit NE module, like the RTL and QCA6174 drivers);
`src/IWLWIFI.map` is the link map. `make -C src clean` removes `obj/` and the outputs. The
current snapshot of Open Watcom v2 (`ow-snapshot.tar.xz` from the project's GitHub releases) works.

## OS/2 / ArcaOS

Open Watcom `binp` and NASM on the `PATH`, `WATCOM` set; in `src` run `build.cmd`.

## Flags and rules

`wcc -bt=os2 -2 -obilmr -s -ze -zl -zq -zff -zgf -ms -wx -zp1 -zu -j -i=os2 -i=wlan -i=iwl`

The rules are those of the [QCA6174 driver](https://github.com/psmedley/qca6174/blob/main/BUILD.md):

* `-2`: 80286 code. 32-bit registers are only used in `os2/lib.asm` (MMIO, port I/O, locks).
* `-ms -zu`: small model, SS is not DS. **Never pass the address of a stack variable** to a function that takes a
  near pointer: the compiler warns "W112 Pointer truncated" - treat every such warning as a bug (make the variable
  `static`, or make the parameter `FAR` as `iwl_cmd_status` does).
* `UCHAR FAR *a, *b;` declares `b` as a *near* pointer. Write `UCHAR FAR *a, FAR *b;`.
* No C runtime: no 32-bit multiply, divide or variable shift. `u32_div()` (dbg.c) divides, `rt_imul16()` multiplies
  16 x 16 -> 32, `bit32()` (iwl_trans.c) makes a variable bit. Constant shifts of ULONGs are fine.
* `hdr.obj` must be linked first (device header at offset 0), `end.obj` last (resident-end markers).
* DGROUP (header, data, BSS) must stay below 64 KB; it is about 31 KB. The code segment must stay below 64 KB; it
  is about 39 KB. Large buffers live in DMA memory.
* Firmware structures are packed (`-zp1`) and their sizes are checked at compile time in `iwl/iwl_cfg.c` (a wrong
  size fails the compile with a negative array size).

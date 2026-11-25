#!/bin/sh
# Host test of the section counting for the 9000 context-info boot (src/iwl/iwl_ctxt.c, between CTX_PURE_BEGIN and
# CTX_PURE_END), with the section layouts of iwlwifi-9000-pu-b0-jf-b0-46.ucode:  sh tests/ctxt/run.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
awk '/CTX_PURE_BEGIN/{p=1;next} /CTX_PURE_END/{p=0} p' "$here/../../src/iwl/iwl_ctxt.c" > "$tmp/block.inc"
cp "$here/test.c" "$tmp/test.c"
gcc -w -I"$tmp" -o "$tmp/t" "$tmp/test.c"
"$tmp/t"

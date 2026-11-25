#!/bin/sh
# Host test of the transmit-aggregation start rule (scheduler pointer moves of 64/128/192 entries are impossible),
# compiled from src/iwl/iwl_agg.c between TXA_PURE_BEGIN and TXA_PURE_END:  sh tests/txagg/run.sh
set -e
here=$(cd "$(dirname "$0")" && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
awk '/TXA_PURE_BEGIN/{p=1;next} /TXA_PURE_END/{p=0} p' "$here/../../src/iwl/iwl_agg.c" > "$tmp/block.inc"
cp "$here/test.c" "$tmp/test.c"
gcc -w -I"$tmp" -o "$tmp/t" "$tmp/test.c"
"$tmp/t"

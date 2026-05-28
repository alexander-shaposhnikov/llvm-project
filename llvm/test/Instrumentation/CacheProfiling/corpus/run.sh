#!/bin/bash
# Compile, instrument, and run each corpus example. Prints a table with
# PASS/FAIL based on expected hit rate (hi_* expects >= 95%, lo_* expects
# <= 30%).
#
# Usage:
#   ./run.sh                  # uses ../build/ from repo root
#   ./run.sh <BUILD_DIR>      # overrides the build dir
set -eu

HERE="$(cd "$(dirname "$0")" && pwd)"
BUILD="${1:-$HERE/../../../../../build}"
NEW="$BUILD/bin"
RT="$BUILD/lib/clang/23/lib/darwin/libclang_rt.profile_osx.a"
SDK="$(xcrun --show-sdk-path)"
WORK="$(mktemp -d -t cp-corpus-XXXXXX)"
trap "rm -rf $WORK" EXIT

# Use clang -O2 so the IR is fully optimized BEFORE we instrument.
# (clang -O0 stamps `optnone` on every function, which makes opt's
#  default<O2> pipeline a no-op; every stack alloca stays in memory and
#  our pass counts 7-9 spurious per-iter accesses.)
# After clang's -O2 we only need opt for the cache-profile pass itself.
OPT_PIPE='cache-profile'

printf '%-26s  %-14s  %-12s  %-9s  %s\n' "example" "accesses" "miss_rate" "wall_s" "result"
printf '%s\n' "$(printf '%.0s-' {1..82})"

for src in "$HERE"/hi_*.c "$HERE"/lo_*.c; do
  name=$(basename "$src" .c)
  expect_high=0
  [[ $name == hi_* ]] && expect_high=1

  ll="$WORK/$name.ll"; ill="$WORK/$name.instr.ll"; bin="$WORK/$name.bin"
  prof="$WORK/$name.profraw"

  "$NEW/clang" -isysroot "$SDK" -O2 -S -emit-llvm "$src" -o "$ll" 2>/dev/null
  "$NEW/opt"   -passes="$OPT_PIPE" "$ll" -S -o "$ill" 2>/dev/null
  "$NEW/clang" -isysroot "$SDK" "$ill" "$RT" -lpthread -o "$bin" 2>/dev/null

  t0=$(python3 -c 'import time; print(time.monotonic())')
  LLVM_CACHE_PROFILE="$prof" "$bin" >/dev/null 2>&1
  t1=$(python3 -c 'import time; print(time.monotonic())')
  wall=$(python3 -c "print(f'{$t1 - $t0:.3f}')")

  acc=$(grep '^l1_accesses=' "$prof" | cut -d= -f2)
  mis=$(grep '^l1_misses='   "$prof" | cut -d= -f2)
  hit_rate=$(grep '^l1_hit_rate=' "$prof" | cut -d= -f2)
  miss_rate=$(python3 -c "print(f'{100.0-$hit_rate:.4f}')")

  result="?"
  if [ "$expect_high" = 1 ]; then
    awk_ok=$(python3 -c "print(1 if $hit_rate >= 95.0 else 0)")
    [ "$awk_ok" = 1 ] && result="PASS (hit>=95%)" || result="FAIL (hit=$hit_rate, want>=95)"
  else
    awk_ok=$(python3 -c "print(1 if $hit_rate <= 30.0 else 0)")
    [ "$awk_ok" = 1 ] && result="PASS (hit<=30%)" || result="FAIL (hit=$hit_rate, want<=30)"
  fi

  printf '%-26s  %-14s  %-12s  %-9s  %s\n' "$name" "$acc" "$miss_rate" "$wall" "$result"
done

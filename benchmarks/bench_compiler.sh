#!/usr/bin/env bash
# Compiler-speed benchmark: times stradac translating a fixed corpus
# (compiler/Combined.strada, ~1.4MB of real Strada) to C. Guards
# compile-speed regressions — the 2026-06 rounds (GC off in stradac,
# hash-set builtin lookup, lexer fast paths, module caching) made this a
# tracked performance surface that the test suite can't see.
#
# Usage: ./bench_compiler.sh [runs]   (default 5, best-of)
set -e
cd "$(dirname "$0")"
RUNS=${1:-5}
STRADAC=../stradac
CORPUS=../compiler/Combined.strada

if [ ! -x "$STRADAC" ]; then echo "build ./stradac first (make)"; exit 2; fi
if [ ! -f "$CORPUS" ]; then echo "corpus missing: $CORPUS (run make once)"; exit 2; fi

OUT=$(mktemp --suffix=.c)
trap 'rm -f "$OUT"' EXIT

best=""
for ((r=1; r<=RUNS; r++)); do
    t=$( { TIMEFORMAT='%R'; time env STRADA_GC=off "$STRADAC" "$CORPUS" "$OUT" > /dev/null 2>&1; } 2>&1 )
    echo "  run $r: ${t}s"
    if [ -z "$best" ] || (( $(echo "$t < $best" | bc -l) )); then best=$t; fi
done
lines=$(wc -l < "$CORPUS")
echo "compiler: best ${best}s for $lines lines ($(echo "scale=0; $lines / $best" | bc -l) lines/sec)"

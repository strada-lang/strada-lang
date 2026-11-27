#!/bin/bash
# bench_gate.sh [--update] [--threshold PCT] [--compiled-only|--vm-only]
#
# Performance gate: instruction counts (callgrind) of every benchmark, compiled
# at -O2 and in the VM, against benchmarks/gate_baseline.txt. Fails when any
# benchmark needs more than PCT percent (default 1) more instructions than the
# baseline. Instruction counts do not depend on machine load, so this can run
# next to other work. Two identical runs differ by less than 0.1%. It takes
# about 25 minutes on two cores.
#
#   --update   rewrite the baseline from this build. Do it in the same commit
#              as an intended change (and say why in the commit message).
#
# Compiled runs use the benchmarks as they are. VM runs use scaled copies
# (integer literals >= 10000 divided by 10, fib(35) -> fib(30)): the VM under
# callgrind would otherwise take hours. Scaled counts track full runs within
# about 2 points, which is enough to catch a regression.
#
# The counts depend on the C compiler and libc, so the baseline belongs to the
# machine that wrote it (recorded in its header). On another machine run
# --update on the commit before your change first.

cd "$(dirname "$0")/.." || exit 1
ROOT=$PWD
BASE=$ROOT/benchmarks/gate_baseline.txt
THRESH=1
UPDATE=0
DO_C=1
DO_VM=1
while [ $# -gt 0 ]; do
    case $1 in
        --update) UPDATE=1 ;;
        --threshold) THRESH=$2; shift ;;
        --compiled-only) DO_VM=0 ;;
        --vm-only) DO_C=0 ;;
        *) echo "usage: $0 [--update] [--threshold PCT] [--compiled-only|--vm-only]" >&2; exit 2 ;;
    esac
    shift
done

command -v valgrind >/dev/null || { echo "bench_gate: valgrind not found" >&2; exit 2; }
[ -x ./strada ] || { echo "bench_gate: ./strada not built" >&2; exit 2; }
INTERP=$ROOT/interpreter/strada-interp
if [ $DO_VM = 1 ] && [ ! -x "$INTERP" ]; then echo "bench_gate: $INTERP not built" >&2; exit 2; fi

W=$(mktemp -d "${TMPDIR:-/tmp}/strada-bench-gate.XXXXXX") || exit 2
trap 'rm -rf "$W"' EXIT
mkdir -p "$W/bin" "$W/scaled"

# Left out: bench_oop_so* need a companion library build; bench_async starts
# threads, whose interleaving changes the count from run to run.
skip_bench() {
    case $1 in bench_oop_so|bench_oop_so_lib|bench_async) return 0 ;; esac
    return 1
}

icount() {  # icount CMD... -> instruction count on stdout, empty on failure
    local out
    out=$(cd "$ROOT/benchmarks" && timeout 1800 valgrind --tool=callgrind --callgrind-out-file=/dev/null "$@" 2>&1 >/dev/null) || return 1
    echo "$out" | grep -o "refs: *[0-9,]*" | tail -1 | tr -dc 0-9
}

NAMES=()
for f in benchmarks/bench_*.strada; do
    b=$(basename "$f" .strada)
    skip_bench "$b" || NAMES+=("$b")
done

run_compiled() {
    for b in "${NAMES[@]}"; do
        if ! ./strada -O2 -o "$W/bin/$b" "benchmarks/$b.strada" >/dev/null 2>&1; then
            echo "c:$b BUILDFAIL"; continue
        fi
        n=$(icount "$W/bin/$b")
        echo "c:$b ${n:-RUNFAIL}"
    done
}

run_vm() {
    for b in "$@"; do
        perl -pe 's/(?<![\w.])(\d{5,})(?![\w.])/int($1\/10)/ge' "benchmarks/$b.strada" > "$W/scaled/$b.strada"
        [ "$b" = bench_compute ] && sed -i 's/fib(35)/fib(30)/' "$W/scaled/$b.strada"
        n=$(icount "$INTERP" "$W/scaled/$b.strada")
        echo "v:$b ${n:-RUNFAIL}"
    done
}

# bench_data writes a fixed /tmp file: the VM runs it after the compiled
# pass has finished, everything else runs two at a time.
VM_FIRST=(); VM_LAST=()
for b in "${NAMES[@]}"; do
    if [ "$b" = bench_data ]; then VM_LAST+=("$b"); else VM_FIRST+=("$b"); fi
done
if [ -n "$BENCH_GATE_COUNTS" ]; then
    # Compare a saved set of counts instead of measuring (for testing this script).
    cp "$BENCH_GATE_COUNTS" "$W/now.txt" || exit 2
else
: > "$W/c.txt"; : > "$W/v.txt"
if [ $DO_C = 1 ]; then run_compiled > "$W/c.txt" & fi
if [ $DO_VM = 1 ]; then run_vm "${VM_FIRST[@]}" > "$W/v.txt" & fi
wait
if [ $DO_VM = 1 ]; then run_vm "${VM_LAST[@]}" >> "$W/v.txt"; fi
cat "$W/c.txt" "$W/v.txt" | sort > "$W/now.txt"
fi

if [ $UPDATE = 1 ]; then
    if grep -q "FAIL$" "$W/now.txt" && [ $DO_C = 1 ] && grep -q "^c:.*FAIL$" "$W/now.txt"; then
        echo "bench_gate: a compiled benchmark failed; baseline not written" >&2
        grep "^c:.*FAIL$" "$W/now.txt" >&2
        exit 1
    fi
    {
        echo "# Instruction counts for t/bench_gate.sh. Regenerate with: t/bench_gate.sh --update"
        echo "# commit: $(git rev-parse --short HEAD 2>/dev/null)$(git diff --quiet 2>/dev/null || echo ' + uncommitted changes')"
        echo "# host: $(uname -m), $(${CC:-gcc} --version | head -1), $(ldd --version 2>/dev/null | head -1)"
        echo "# c: = compiled at -O2, full benchmark.  v: = VM, scaled copy.  A VM line"
        echo "# ending in FAIL is a benchmark the VM cannot run under callgrind: not gated."
        if [ $DO_C = 0 ] && [ -f "$BASE" ]; then grep "^c:" "$BASE"; fi
        if [ $DO_VM = 0 ] && [ -f "$BASE" ]; then grep "^v:" "$BASE"; fi
        cat "$W/now.txt"
    } > "$W/base.new"
    mv "$W/base.new" "$BASE"
    echo "bench_gate: baseline written ($(grep -c '^[cv]:' "$BASE") entries)"
    exit 0
fi

[ -f "$BASE" ] || { echo "bench_gate: no baseline ($BASE); run with --update" >&2; exit 2; }

awk -v thresh="$THRESH" -v doc=$DO_C -v dov=$DO_VM '
    FNR == NR { if ($1 ~ /^[cv]:/) base[$1] = $2; next }
    { now[$1] = $2 }
    END {
        bad = 0; better = 0; n = 0
        for (k in base) {
            if (k ~ /^c:/ && !doc) continue
            if (k ~ /^v:/ && !dov) continue
            if (base[k] ~ /FAIL$/) continue
            n++
            if (!(k in now) || now[k] ~ /FAIL$/) {
                printf "FAIL  %-28s %s (baseline %d)\n", k, (k in now) ? now[k] : "missing", base[k]; bad++; continue
            }
            d = (now[k] - base[k]) * 100.0 / base[k]
            if (d > thresh) { printf "FAIL  %-28s %+.2f%%  (%d -> %d)\n", k, d, base[k], now[k]; bad++ }
            else if (d < -thresh) { printf "note  %-28s %+.2f%%  (%d -> %d)\n", k, d, base[k], now[k]; better++ }
            else if (d > thresh / 2 || d < -thresh / 2) printf "      %-28s %+.2f%%\n", k, d
        }
        for (k in now) if (!(k in base)) printf "note  %-28s new, not in the baseline\n", k
        printf "Bench gate: %d compared, %d regressed more than %s%%, %d improved more than %s%%\n", n, bad, thresh, better, thresh
        if (better && !bad) print "Improvements: run t/bench_gate.sh --update to make them the new baseline."
        exit bad ? 1 : 0
    }
' "$BASE" "$W/now.txt"

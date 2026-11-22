#!/bin/bash
# Thread stress under sanitizers. Not part of the default suite (it rebuilds
# the runtime twice with sanitizers, ~2 min).
#
#   t/thread_stress/run.sh [asan|tsan|both] [name...]
#
# Each t/thread_stress/*.strada is compiled, linked against a sanitizer build
# of the runtime and run with STRADA_GIL=0 (no global lock: the per-object
# locking must keep memory safe on its own). A program passes when it exits 0
# and the sanitizer reports nothing. Output is not compared: unlocked updates
# may be lost, they must not corrupt anything.
# Env: STRESS_GIL (default 0), ROUNDS, STRESS_KEEP=1 keeps the build dir,
#      STRESS_DIR to choose it (reuses sanitizer runtimes already built there).
set -u
HERE=$(cd "$(dirname "$0")" && pwd); ROOT=$(cd "$HERE/../.." && pwd)
cd "$ROOT" || exit 1
mode=${1:-both}; shift 2>/dev/null
names="$*"
D=${STRESS_DIR:-/tmp/strada-thread-stress-$$}; mkdir -p "$D"
RTCMD=$(make -n -W runtime/strada_runtime.c runtime/strada_runtime.o 2>/dev/null | grep -- "-c runtime/strada_runtime.c" | head -1 \
        | sed 's/^ccache //; s/-flto=auto//; s/-ffat-lto-objects//; s/ -o runtime\/strada_runtime.o//')
[ -n "$RTCMD" ] || { echo "cannot find the runtime compile command"; exit 1; }
DEFS=$(echo "$RTCMD" | tr ' ' '\n' | grep -E '^-(D|I)' | tr '\n' ' ')
fail=0; pass=0
run_mode() {
    local m=$1 san pre
    if [ "$m" = asan ]; then san="-fsanitize=address -fno-omit-frame-pointer -g -O1"; pre=""
    else san="-fsanitize=thread -fno-omit-frame-pointer -g -O1"; pre="setarch -R"; fi
    local rt="$D/rt_$m.o"
    if [ ! -f "$rt" ] || [ runtime/strada_runtime.c -nt "$rt" ] || [ runtime/strada_runtime.h -nt "$rt" ]; then
        echo "building $m runtime..."
        eval "$(echo "$RTCMD" | sed "s/ -O2 / $san /") -o $rt" 2> "$D/rt_$m.log" || { echo "runtime build failed: $D/rt_$m.log"; exit 1; }
    fi
    for src in "$HERE"/*.strada; do
        local n=$(basename "$src" .strada)
        if [ -n "$names" ]; then case " $names " in *" $n "*) ;; *) continue;; esac; fi
        STRADA_GIL_POLLS=1 ./stradac "$src" "$D/$n.c" > "$D/$n.cc.log" 2>&1 || { echo "FAIL $m $n (stradac)"; fail=$((fail+1)); continue; }
        gcc $san -w $DEFS -Iruntime -c "$D/$n.c" -o "$D/${n}_$m.o" 2>> "$D/$n.cc.log" \
          && gcc $san -rdynamic -o "$D/${n}_$m" "$D/${n}_$m.o" "$rt" vendor/pcre2/libpcre2-8.a -ldl -lm -lpthread 2>> "$D/$n.cc.log" \
          || { echo "FAIL $m $n (cc: $D/$n.cc.log)"; fail=$((fail+1)); continue; }
        STRADA_GIL=${STRESS_GIL:-0} ASAN_OPTIONS=detect_leaks=0:halt_on_error=1 TSAN_OPTIONS=halt_on_error=0:report_signal_unsafe=0 \
            timeout ${STRESS_TIMEOUT:-600} $pre "$D/${n}_$m" > "$D/${n}_$m.out" 2> "$D/${n}_$m.err"
        local rc=$?
        local reps=$(grep -cE "^(WARNING: ThreadSanitizer|==[0-9]+==ERROR)" "$D/${n}_$m.err")
        if [ $rc -eq 0 ] && [ "$reps" -eq 0 ]; then echo "ok   $m $n"; pass=$((pass+1))
        else echo "FAIL $m $n (exit $rc, $reps sanitizer reports: $D/${n}_$m.err)"; fail=$((fail+1)); fi
    done
}
case "$mode" in asan) run_mode asan;; tsan) run_mode tsan;; *) run_mode asan; run_mode tsan;; esac
echo "thread stress: $pass passed, $fail failed"
[ $fail -eq 0 ] && [ -z "${STRESS_KEEP:-}" ] && [ -z "${STRESS_DIR:-}" ] && rm -rf "$D"
[ $fail -eq 0 ]

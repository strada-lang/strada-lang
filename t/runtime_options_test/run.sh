#!/bin/bash
#
# Runtime settings are build-time options, never environment variables:
#   strada --no-gc / --exit-task-wait-ms N / --no-pool / --debug-bless /
#          --check-cleanup=warn
# (strada-interp takes the first four on its command line). A built program
# must behave the same whatever STRADA_GC, STRADA_EXIT_TASK_WAIT_MS,
# STRADA_NO_POOL, STRADA_DEBUG_BLESS or STRADA_CLEANUP_CHECK_WARN is set to.
#
# Exits non-zero on any failure.

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
STRADA="$REPO_DIR/strada"
STRADAC="$REPO_DIR/stradac"
INTERP="$REPO_DIR/interpreter/strada-interp"

if [ ! -x "$STRADA" ]; then
    echo "Build strada first (run 'make' in $REPO_DIR)" >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
FAILS=0
check() {   # check DESC WANT GOT
    if [ "$2" == "$3" ]; then echo "ok: $1"; else echo "FAIL: $1 (wanted '$2', got '$3')"; FAILS=$((FAILS + 1)); fi
}
ms_of() {   # ms_of CMD...: wall time in milliseconds
    local t0 t1
    t0=$(date +%s%N); "$@" >/dev/null 2>&1; t1=$(date +%s%N)
    echo $(( (t1 - t0) / 1000000 ))
}
cd "$WORK" || exit 2

# ---- cycle collector ----
cat > gc.strada <<'SRC'
func main() int {
    core::gc_threshold(100);
    my int $i = 0;
    while ($i < 3000) {
        my $a = {};
        my $b = { "peer" => $a };
        $a->{"peer"} = $b;
        $i = $i + 1;
    }
    if (core::gc_collections() > 0) { say("collector on"); } else { say("collector off"); }
    return 0;
}
SRC
"$STRADA" -O0 -o gc_default gc.strada || { echo "FAIL: gc default build"; exit 1; }
"$STRADA" -O0 --no-gc -o gc_off gc.strada || { echo "FAIL: --no-gc build"; exit 1; }
check "default: collector on"               "collector on"  "$(./gc_default)"
check "--no-gc: collector off"              "collector off" "$(./gc_off)"
check "STRADA_GC=0 does not turn it off"    "collector on"  "$(STRADA_GC=0 ./gc_default)"
check "STRADA_GC=off does not turn it off"  "collector on"  "$(STRADA_GC=off ./gc_default)"

# ---- exit wait for running pool tasks ----
cat > exitwait.strada <<'SRC'
func main() int {
    my $f = async::spawn(func () { core::usleep(4000000); return 1; });
    core::usleep(100000);
    return 0;
}
SRC
"$STRADA" -O0 -o ew_default exitwait.strada || { echo "FAIL: exit-wait default build"; exit 1; }
"$STRADA" -O0 --exit-task-wait-ms 0 -o ew_zero exitwait.strada || { echo "FAIL: --exit-task-wait-ms build"; exit 1; }
"$STRADA" -O0 --exit-task-wait-ms=2000 -o ew_two exitwait.strada || { echo "FAIL: --exit-task-wait-ms= build"; exit 1; }
t_def=$(ms_of ./ew_default)
t_zero=$(ms_of ./ew_zero)
t_two=$(ms_of ./ew_two)
t_env=$(STRADA_EXIT_TASK_WAIT_MS=0 ms_of ./ew_default)
t_env2=$(STRADA_EXIT_TASK_WAIT_MS=3000 ms_of ./ew_zero)
r="no";  [ "$t_def" -ge 1000 ] && [ "$t_def" -lt 1900 ] && r="yes";  check "default waits about 1 s (${t_def} ms)" "yes" "$r"
r="no";  [ "$t_zero" -lt 700 ] && r="yes";                           check "--exit-task-wait-ms 0 exits at once (${t_zero} ms)" "yes" "$r"
r="no";  [ "$t_two" -ge 2000 ] && [ "$t_two" -lt 3500 ] && r="yes";  check "--exit-task-wait-ms=2000 waits 2 s (${t_two} ms)" "yes" "$r"
r="no";  [ "$t_env" -ge 1000 ] && r="yes";                           check "STRADA_EXIT_TASK_WAIT_MS=0 does not shorten the wait (${t_env} ms)" "yes" "$r"
r="no";  [ "$t_env2" -lt 700 ] && r="yes";                           check "STRADA_EXIT_TASK_WAIT_MS=3000 does not lengthen it (${t_env2} ms)" "yes" "$r"

for bad in -1 abc 99999999; do
    if "$STRADA" -O0 --exit-task-wait-ms "$bad" -o bad exitwait.strada >/dev/null 2>&1; then
        echo "FAIL: --exit-task-wait-ms $bad accepted"; FAILS=$((FAILS + 1))
    else
        echo "ok: --exit-task-wait-ms $bad rejected"
    fi
done

# ---- bless tracing ----
cat > bless.strada <<'SRC'
package Thing;
func new() scalar { my %h = (); return bless(\%h, "Thing"); }
func hi(scalar $self) str { return "hi"; }
package main;
func main() int {
    my $t = Thing::new();
    say($t->hi());
    return 0;
}
SRC
"$STRADA" -O0 -o bless_default bless.strada || { echo "FAIL: bless default build"; exit 1; }
"$STRADA" -O0 --debug-bless -o bless_dbg bless.strada || { echo "FAIL: --debug-bless build"; exit 1; }
check "default: no trace"                       "0" "$(./bless_default 2>&1 >/dev/null | wc -c)"
check "STRADA_DEBUG_BLESS=1 adds no trace"      "0" "$(STRADA_DEBUG_BLESS=1 ./bless_default 2>&1 >/dev/null | wc -c)"
r="no"; [ "$(./bless_dbg 2>&1 >/dev/null | wc -c)" -gt 0 ] && r="yes"
check "--debug-bless traces to stderr"          "yes" "$r"
check "--debug-bless keeps stdout"              "hi" "$(./bless_dbg 2>/dev/null)"

# ---- pools ----
cat > pool.strada <<'SRC'
our %seen = ("a" => 1, "b" => 2);
BEGIN { my @warm = (1, 2, 3); my %h = ("k" => "v"); }
package P;
func new() scalar { my %h = ("n" => 1); return bless(\%h, "P"); }
package main;
func main() int {
    my int $i = 0;
    my int $sum = 0;
    while ($i < 2000) {
        my $o = P::new();
        my %h = ("x" => $i, "y" => "s" . $i);
        my @a = ($i, $i + 1);
        $sum = $sum + $o->{"n"} + size(@a) + scalar(keys(%h));
        $i = $i + 1;
    }
    say($sum . " " . $seen{"b"});
    return 0;
}
SRC
"$STRADA" -O0 -o pool_default pool.strada || { echo "FAIL: pool default build"; exit 1; }
"$STRADA" -O0 --no-pool -o pool_off pool.strada || { echo "FAIL: --no-pool build"; exit 1; }
check "default pools"              "10000 2" "$(./pool_default)"
check "--no-pool runs the same"    "10000 2" "$(./pool_off)"
"$STRADAC" pool.strada pool_a.c >/dev/null 2>&1
"$STRADAC" --no-pool --no-gc --debug-bless --exit-task-wait-ms 250 pool.strada pool_b.c >/dev/null 2>&1
check "no option: main() sets nothing" "0" "$(grep -c 'strada_set_no_pool\|strada_gc_set_enabled(0)\|strada_set_debug_bless\|strada_set_exit_task_wait_ms' pool_a.c)"
check "options are calls in main()"    "4" "$(grep -c '^    strada_set_no_pool(1);\|^    strada_gc_set_enabled(0);\|^    strada_set_debug_bless(1);\|^    strada_set_exit_task_wait_ms(250L);' pool_b.c)"

# ---- cleanup check: abort or warn is chosen at build time ----
cat > unbal.strada <<'SRC'
func leaky() int {
    __C__ { strada_cleanup_push(strada_new_str("x")); }
    return 1;
}
func main() int {
    try { leaky(); } catch ($e) { }
    say("done");
    return 0;
}
SRC
if "$STRADA" -O0 --check-cleanup -o unbal_abort unbal.strada >/dev/null 2>&1 \
   && "$STRADA" -O0 --check-cleanup=warn -o unbal_warn unbal.strada >/dev/null 2>&1; then
    ( ulimit -c 0; ./unbal_abort >/dev/null 2>abort.err ) 2>/dev/null; rc_abort=$?
    ./unbal_warn >warn.out 2>warn.err; rc_warn=$?
    ( ulimit -c 0; STRADA_CLEANUP_CHECK_WARN=1 ./unbal_abort >/dev/null 2>&1 ) 2>/dev/null; rc_env=$?
    check "--check-cleanup aborts"                       "134" "$rc_abort"
    check "--check-cleanup reports the function"         "1" "$(grep -c 'cleanup stack imbalance in leaky()' abort.err)"
    check "--check-cleanup=warn goes on"                 "0 done" "$rc_warn $(cat warn.out)"
    check "--check-cleanup=warn reports the function"    "1" "$(grep -c 'cleanup stack imbalance in leaky()' warn.err)"
    check "STRADA_CLEANUP_CHECK_WARN does not stop the abort" "134" "$rc_env"
else
    echo "FAIL: --check-cleanup builds"; FAILS=$((FAILS + 1))
fi

# ---- VM: the same switches on the command line ----
if [ -x "$INTERP" ]; then
    check "VM default runs"      "10000 2" "$("$INTERP" pool.strada)"
    check "VM takes the options" "10000 2" "$("$INTERP" --no-gc --no-pool --exit-task-wait-ms 0 pool.strada)"
    t_vm0=$(ms_of "$INTERP" --exit-task-wait-ms 0 exitwait.strada)
    t_vmenv=$(STRADA_EXIT_TASK_WAIT_MS=0 ms_of "$INTERP" exitwait.strada)
    r="no"; [ "$t_vm0" -lt 800 ] && r="yes";    check "VM --exit-task-wait-ms 0 exits at once (${t_vm0} ms)" "yes" "$r"
    r="no"; [ "$t_vmenv" -ge 1000 ] && r="yes"; check "VM ignores STRADA_EXIT_TASK_WAIT_MS (${t_vmenv} ms)" "yes" "$r"
    if "$INTERP" --exit-task-wait-ms nope pool.strada >/dev/null 2>&1; then
        echo "FAIL: VM accepted a bad --exit-task-wait-ms"; FAILS=$((FAILS + 1))
    else
        echo "ok: VM rejects a bad --exit-task-wait-ms"
    fi
else
    echo "skip: strada-interp not built"
fi

if [ "$FAILS" -ne 0 ]; then
    echo "runtime-options: $FAILS failure(s)"
    exit 1
fi
echo "runtime-options: all passed"

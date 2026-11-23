#!/bin/bash
#
# Thread lock settings are build-time options, never environment variables:
#   strada --no-gil / --gil-interval-us N   (same on stradac and strada-interp)
# A built program must behave the same whatever STRADA_GIL* is set to.
#
# Exits non-zero on any failure.

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
STRADA="$REPO_DIR/strada"
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

cat > "$WORK/info.strada" <<'SRC'
func main() int {
    my $before = core::gil_info();
    my $t = thread::create(func () { return 1; });
    thread::join($t);
    my $after = core::gil_info();
    say($before->{"enabled"} . " " . $before->{"active"} . " " . $after->{"active"} . " " . $after->{"interval_us"});
    return 0;
}
SRC

cd "$WORK" || exit 2

"$STRADA" -O0 -o info_default info.strada || { echo "FAIL: default build"; exit 1; }
"$STRADA" -O0 --no-gil -o info_off info.strada || { echo "FAIL: --no-gil build"; exit 1; }
"$STRADA" -O0 --gil-interval-us 250 -o info_iv info.strada || { echo "FAIL: --gil-interval-us build"; exit 1; }
"$STRADA" -O0 --gil-interval-us=75 --no-gil -o info_both info.strada || { echo "FAIL: combined build"; exit 1; }

check "default: lock on, 5 ms"            "1 0 1 5000" "$(./info_default)"
check "--no-gil: lock off"                "0 0 0 5000" "$(./info_off)"
check "--gil-interval-us 250"             "1 0 1 250"  "$(./info_iv)"
check "--gil-interval-us=75 --no-gil"     "0 0 0 75"   "$(./info_both)"

# The environment must not change a built program.
check "STRADA_GIL=0 does not turn it off" "1 0 1 5000" "$(STRADA_GIL=0 STRADA_GIL_INTERVAL_US=9 ./info_default)"
check "STRADA_GIL=1 does not turn it on"  "0 0 0 5000" "$(STRADA_GIL=1 STRADA_GIL_INTERVAL_US=9 ./info_off)"

# A bad interval is a located usage error, not a silently ignored value.
if "$STRADA" -O0 --gil-interval-us 0 -o bad info.strada >/dev/null 2>&1; then
    echo "FAIL: --gil-interval-us 0 accepted"; FAILS=$((FAILS + 1))
else
    echo "ok: --gil-interval-us 0 rejected"
fi

# --gil-polls: a library with a loop and no thread call gets handover checks.
cat > "$WORK/Lib.strada" <<'SRC'
package Lib;
func spin(int $n) int {
    my int $t = 0;
    my int $i = 0;
    while ($i < $n) { $t = $t + Lib::step($i); $i = $i + 1; }
    return $t;
}
func step(int $i) int { return $i % 7; }
SRC
"$REPO_DIR/stradac" -M Lib.strada Lib_plain.c >/dev/null 2>&1
"$REPO_DIR/stradac" -M --gil-polls Lib.strada Lib_polls.c >/dev/null 2>&1
# ...and the driver forwards the option.
"$STRADA" --object --gil-polls -o Lib_polls.o Lib.strada >/dev/null 2>&1
check "driver builds a --gil-polls object" "yes" "$([ -s Lib_polls.o ] && echo yes)"
check "library without --gil-polls has no checks" "0" "$(grep -c 'strada_gil_poll()' Lib_plain.c 2>/dev/null)"
if [ "$(grep -c 'strada_gil_poll()' Lib_polls.c 2>/dev/null)" -gt 0 ]; then
    echo "ok: --gil-polls adds loop checks"
else
    echo "FAIL: --gil-polls added no loop checks"; FAILS=$((FAILS + 1))
fi

# The VM takes the same options on its command line.
if [ -x "$INTERP" ]; then
    check "VM default"                "1 0 1 5000" "$("$INTERP" info.strada)"
    check "VM --no-gil"               "0 0 0 5000" "$("$INTERP" --no-gil info.strada)"
    check "VM --gil-interval-us 250"  "1 0 1 250"  "$("$INTERP" --gil-interval-us 250 info.strada)"
    check "VM ignores STRADA_GIL=0"   "1 0 1 5000" "$(STRADA_GIL=0 "$INTERP" info.strada)"
fi

if [ $FAILS -ne 0 ]; then echo "gil_options_test: $FAILS failure(s)"; exit 1; fi
echo "gil_options_test: all passed"
exit 0

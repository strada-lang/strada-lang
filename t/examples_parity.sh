#!/bin/bash
# Examples parity sweep: every examples/*.strada is compiled and run, and also
# run through the bytecode VM (interpreter/strada-interp); stdout+stderr and
# the exit status must match. Examples that cannot be compared (servers,
# output that legitimately differs run to run, compiled-only features) are
# listed in t/examples_parity_skip.txt with a reason.
#
# Usage: t/examples_parity.sh [-v] [NAME-FILTER]
#   -v  show the first lines of each difference
# Exit status: number of mismatching examples (0 = all match).

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
PROJECT_DIR="$(cd "$SCRIPT_DIR/.." && pwd)"
VERBOSE=0
FILTER=""
for a in "$@"; do
    case "$a" in
        -v) VERBOSE=1 ;;
        *) FILTER="$a" ;;
    esac
done

INTERP="$PROJECT_DIR/interpreter/strada-interp"
if [ ! -x "$INTERP" ]; then
    echo "examples_parity: $INTERP not built (make interpreter)" >&2
    exit 1
fi
WORK="$(mktemp -d "${TMPDIR:-/tmp}/strada_parity.XXXXXX")"
trap 'rm -rf "$WORK"' EXIT

declare -A SKIP
if [ -f "$SCRIPT_DIR/examples_parity_skip.txt" ]; then
    while IFS= read -r line; do
        case "$line" in ''|'#'*) continue ;; esac
        name="${line%%[[:space:]]*}"
        SKIP["$name"]=1
    done < "$SCRIPT_DIR/examples_parity_skip.txt"
fi

total=0; same=0; diff=0; skipped=0
failed=()
cd "$PROJECT_DIR" || exit 1
for f in examples/*.strada; do
    b="$(basename "$f" .strada)"
    if [ -n "$FILTER" ] && [[ "$b" != *"$FILTER"* ]]; then continue; fi
    if [ -n "${SKIP[$b]}" ]; then skipped=$((skipped + 1)); continue; fi
    total=$((total + 1))
    mkdir -p "$WORK/$b"
    if ./strada -O0 -L lib "$f" -o "$WORK/$b/bin" > "$WORK/$b/cerr" 2>&1; then
        (cd "$WORK/$b" && timeout 20 ./bin < /dev/null > c.out 2>&1); crc=$?
    else
        crc="compile-error"
        cp "$WORK/$b/cerr" "$WORK/$b/c.out"
    fi
    (cd "$WORK/$b" && timeout 20 "$INTERP" "$PROJECT_DIR/$f" < /dev/null > v.out 2>&1); vrc=$?
    if [ "$crc" = "$vrc" ] && cmp -s "$WORK/$b/c.out" "$WORK/$b/v.out"; then
        same=$((same + 1))
    else
        diff=$((diff + 1))
        failed+=("$b")
        echo "[DIFF] $b (compiled exit $crc, VM exit $vrc)"
        if [ $VERBOSE -eq 1 ]; then
            diff "$WORK/$b/c.out" "$WORK/$b/v.out" | head -8 | sed 's/^/       /'
        fi
    fi
done

echo "Examples parity: $total compared, $same match, $diff differ, $skipped skipped"
exit $diff

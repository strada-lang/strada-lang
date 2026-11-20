#!/bin/bash
#
# Regression test for the Perl-compatible library modules: Getopt::Long,
# File::Basename, File::Temp, File::Spec (+ ::Functions), Cwd, File::Path,
# File::Copy, Carp, Scalar::Util.
#
# Each NAME.strada is compiled and also run through the VM; stdout, then a
# "--- stderr ---" line, then stderr must equal NAME.expected. The
# getopt, basename, spec_cwd and path_copy programs were translated to Perl
# and their output matched Perl 5.38's modules when the expected files were
# written. temp.strada also checks what UNLINK/CLEANUP/the object form
# leave behind after the program exits.
#
# Usage: run.sh [--update]   (--update rewrites the .expected files from
# the compiled output)
#
# Exits non-zero on any failure.

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
STRADA="$REPO_DIR/strada"
INTERP="$REPO_DIR/interpreter/strada-interp"
UPDATE=0
[ "${1:-}" = "--update" ] && UPDATE=1

if [ ! -x "$STRADA" ]; then
    echo "Build strada first (run 'make' in $REPO_DIR)" >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
unset TMPDIR POSIXLY_CORRECT
fail=0

# run_one NAME MODE(c|vm) -> prints the combined output
run_one() {
    local name="$1" mode="$2" dir="$WORK/$1-$2"
    mkdir -p "$dir/scratch"
    local args=()
    case "$name" in
        getopt) args=(--m a b) ;;
        path_copy|temp) args=("$dir/scratch") ;;
    esac
    if [ "$mode" = c ]; then
        if ! (cd "$SCRIPT_DIR" && "$STRADA" -O0 -o "$dir/prog" "$name.strada") > "$dir/build.log" 2>&1; then
            echo "BUILD FAILED"; cat "$dir/build.log"; return
        fi
        (cd "$SCRIPT_DIR" && "$dir/prog" "${args[@]}" > "$dir/out" 2> "$dir/err")
    else
        (cd "$SCRIPT_DIR" && "$INTERP" "$name.strada" "${args[@]}" > "$dir/out" 2> "$dir/err")
    fi
    cat "$dir/out"
    echo "--- stderr ---"
    cat "$dir/err"
    if [ "$name" = temp ]; then
        echo "--- after exit ---"
        local i=0
        while read -r f; do
            i=$((i + 1))
            if [ -e "$f" ]; then echo "file $i exists"; else echo "file $i removed"; fi
        done < "$dir/scratch/names"
        if [ -e "$(cat "$dir/scratch/objname")" ]; then echo "object file exists"; else echo "object file removed"; fi
    fi
}

for src in "$SCRIPT_DIR"/*.strada; do
    name="$(basename "$src" .strada)"
    want_file="$SCRIPT_DIR/$name.expected"
    got_c="$(run_one "$name" c)"
    if [ $UPDATE -eq 1 ]; then
        printf '%s\n' "$got_c" > "$want_file"
    fi
    want="$(cat "$want_file" 2>/dev/null)"
    modes="c"
    [ -x "$INTERP" ] && modes="c vm"
    for mode in $modes; do
        if [ "$mode" = c ]; then got="$got_c"; else got="$(run_one "$name" vm)"; fi
        if [ "$got" == "$want" ]; then
            echo "ok - $name ($mode)"
        else
            echo "not ok - $name ($mode)"
            diff <(printf '%s\n' "$want") <(printf '%s\n' "$got") | head -20
            fail=1
        fi
    done
done

exit $fail

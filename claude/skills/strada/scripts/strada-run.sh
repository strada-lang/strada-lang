#!/bin/sh
# strada-run.sh FILE.strada [ARGS...]
# Compile FILE at -O0 into a temporary directory, run it with ARGS, delete the
# binary, and exit with the program's exit status. Unlike `strada -r`, it leaves
# nothing next to the source, so it is safe as a test runner:
#   prove --exec /path/to/strada-run.sh t/*.strada
# Set STRADA to choose the compiler (default: ./strada in a strada-lang
# checkout, else strada on PATH). Extra compile flags: STRADA_FLAGS="-g".
[ $# -ge 1 ] || { echo "usage: $0 FILE.strada [ARGS...]" >&2; exit 2; }
src=$1; shift
if [ -z "${STRADA:-}" ]; then
    if [ -x ./strada ] && [ -f ./compiler/Lexer.strada ]; then STRADA=./strada; else STRADA=strada; fi
fi
tmp=$(mktemp -d "${TMPDIR:-/tmp}/strada-run.XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT INT TERM
# shellcheck disable=SC2086
"$STRADA" -O0 ${STRADA_FLAGS:-} -o "$tmp/prog" "$src" >&2 || exit $?
"$tmp/prog" "$@"

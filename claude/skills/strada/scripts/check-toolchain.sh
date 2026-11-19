#!/bin/sh
# Check that a Strada toolchain is usable: find the compiler, a C compiler,
# and compile + run a hello-world in a temporary directory (removed after).
# Prints the strada command to use. Exit 0 = ready, 1 = something missing.

STRADA=""
if [ -x ./strada ] && [ -f ./compiler/Lexer.strada ]; then
    STRADA=./strada                       # inside a strada-lang checkout
elif command -v strada >/dev/null 2>&1; then
    STRADA=strada
fi

if [ -z "$STRADA" ]; then
    echo "strada: NOT FOUND"
    echo "Install: git clone https://github.com/strada-lang/strada-lang.git &&"
    echo "         cd strada-lang && ./configure && make && sudo make install"
    exit 1
fi
echo "strada: $(command -v "$STRADA")"

CC_FOUND=""
for c in "${CC:-}" gcc cc clang; do
    [ -n "$c" ] && command -v "$c" >/dev/null 2>&1 && { CC_FOUND=$c; break; }
done
if [ -z "$CC_FOUND" ]; then
    echo "C compiler: NOT FOUND (Strada compiles through C; install gcc)"
    exit 1
fi
echo "C compiler: $CC_FOUND"

for t in strada-interp valgrind tcc; do
    if command -v "$t" >/dev/null 2>&1; then echo "optional $t: yes"; else echo "optional $t: no"; fi
done

tmp=$(mktemp -d "${TMPDIR:-/tmp}/strada-check.XXXXXX") || exit 1
trap 'rm -rf "$tmp"' EXIT
cat > "$tmp/hello.strada" <<'EOF'
func main() int {
    my str $who = "Strada";
    say("hello from ${who}");
    return 0;
}
EOF
if "$STRADA" -O0 -o "$tmp/hello" "$tmp/hello.strada" >"$tmp/log" 2>&1 && \
   [ "$("$tmp/hello")" = "hello from Strada" ]; then
    echo "hello-world: compiled and ran"
    echo "READY: use '$STRADA'"
    exit 0
fi
echo "hello-world: FAILED"
cat "$tmp/log"
exit 1

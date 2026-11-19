#!/bin/bash
#
# Regression test: `extern func` with a body, called from C and from Strada.
#
# The body used to be compiled with its raw C parameters standing in for
# Strada values: anything beyond arithmetic (strings, hashes, split, `our`)
# crashed or failed to compile, and semantic checks skipped it. Now the body
# is an ordinary Strada function (NAME__xbody) and NAME is a C wrapper that
# converts arguments and the result (extern_split_body / gen_extern_wrapper).
#
# Checks: a C program built with `strada --embed-ldflags` loads a --shared
# library and calls string/int64/num/void/undef-returning externs that use
# `our`, hashes and split (valgrind-clean when valgrind is installed); the
# same functions called from Strada in one file, compiled and through the VM;
# an undeclared variable in an extern body is a located Strada error.
#
# Exits non-zero on any failure.

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
STRADA="$REPO_DIR/strada"
INTERP="$REPO_DIR/interpreter/strada-interp"
CC="${CC:-cc}"

if [ ! -x "$STRADA" ]; then
    echo "Build strada first (run 'make' in $REPO_DIR)" >&2
    exit 2
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
fail=0
check() {  # check NAME EXPECTED ACTUAL
    if [ "$2" == "$3" ]; then
        echo "ok - $1"
    else
        echo "not ok - $1"
        echo "  expected: $2"
        echo "  got:      $3"
        fail=1
    fi
}

cat > "$WORK/XLib.strada" <<'EOF'
package XLib;
our int $calls = 0;
extern func shout(str $s) str {
    $calls++;
    my %seen = ();
    foreach my $w (split(/\s+/, $s)) { $seen{lc($w)}++; }
    return uc($s) . " (" . scalar(keys(%seen)) . " distinct, call " . $calls . ")";
}
extern func add(int $a, int $b) int { return $a + $b; }
extern func big(int $a) int { return $a * 1000000000; }
extern func half(num $x) num { return $x / 2; }
extern func maybe(int $n) str { return undef if $n < 0; return "n=" . $n; }
extern func bump() void { $calls += 100; }
extern func calls() int { return $calls; }
EOF

cat > "$WORK/host.c" <<'EOF'
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <dlfcn.h>
int main(int argc, char **argv) {
    void *lib = dlopen(argv[1], RTLD_NOW);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }
    char *(*shout)(const char *) = (char *(*)(const char *))dlsym(lib, "shout");
    int64_t (*add)(int64_t, int64_t) = (int64_t (*)(int64_t, int64_t))dlsym(lib, "add");
    int64_t (*big)(int64_t) = (int64_t (*)(int64_t))dlsym(lib, "big");
    double (*half)(double) = (double (*)(double))dlsym(lib, "half");
    char *(*maybe)(int64_t) = (char *(*)(int64_t))dlsym(lib, "maybe");
    void (*bump)(void) = (void (*)(void))dlsym(lib, "bump");
    int64_t (*calls)(void) = (int64_t (*)(void))dlsym(lib, "calls");
    for (int i = 0; i < 2; i++) { char *r = shout("hello Hello world"); printf("%s|", r); free(r); }
    printf("%ld|%ld|%g|", (long)add(40, 2), (long)big(5), half(5.0));
    char *m = maybe(7);
    char *u = maybe(-1);
    printf("%s|%s|", m, u ? u : "NULL");
    free(m);
    bump();
    printf("%ld\n", (long)calls());
    return 0;
}
EOF

want_c="HELLO HELLO WORLD (2 distinct, call 1)|HELLO HELLO WORLD (2 distinct, call 2)|42|5000000000|2.5|n=7|NULL|102"

if ! (cd "$WORK" && "$STRADA" --shared XLib.strada) > "$WORK/build.log" 2>&1; then
    echo "not ok - build the shared library"; cat "$WORK/build.log"; exit 1
fi
ldflags=$("$STRADA" --embed-ldflags)
if ! $CC -o "$WORK/host" "$WORK/host.c" $ldflags > "$WORK/cc.log" 2>&1; then
    echo "not ok - link the C host with --embed-ldflags"; cat "$WORK/cc.log"; exit 1
fi
check "C host calls the externs" "$want_c" "$("$WORK/host" "$WORK/XLib.so" 2>&1)"

if command -v valgrind >/dev/null 2>&1; then
    vg=$(valgrind -q --error-exitcode=99 --leak-check=full --errors-for-leak-kinds=definite \
        "$WORK/host" "$WORK/XLib.so" 2>&1 >/dev/null; echo "rc=$?")
    check "C host is valgrind-clean" "rc=0" "$vg"
fi

# The same functions called from Strada, in one file
{ cat "$WORK/XLib.strada"; cat <<'EOF'
package main;
func main() int {
    my str $a = shout("x y x");
    my $m = maybe(-1);
    bump();
    say($a . "|" . add(40, 2) . "|" . big(5) . "|" . half(5.0) . "|" . maybe(3)
        . "|" . (defined($m) ? "def" : "undef") . "|" . calls());
    return 0;
}
EOF
} > "$WORK/same.strada"
want_s="X Y X (2 distinct, call 1)|42|5000000000|2.5|n=3|undef|101"
if (cd "$WORK" && "$STRADA" -O0 -o same same.strada) > "$WORK/same.log" 2>&1; then
    check "Strada calls the externs (compiled)" "$want_s" "$("$WORK/same" 2>&1)"
else
    echo "not ok - compile same.strada"; cat "$WORK/same.log"; fail=1
fi
if [ -x "$INTERP" ]; then
    check "Strada calls the externs (VM)" "$want_s" "$("$INTERP" "$WORK/same.strada" 2>&1)"
fi

# Semantic checks run on extern bodies
printf '%s\n' 'package Bad;' 'extern func f(int $a) int {' '    return $a + $nosuch;' '}' > "$WORK/bad.strada"
bad=$("$STRADA" --check "$WORK/bad.strada" 2>&1 | grep -c "bad.strada:3: error: undefined variable '\$nosuch'")
check "undeclared variable in an extern body is a Strada error" "1" "$bad"

exit $fail

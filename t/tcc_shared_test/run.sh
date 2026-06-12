#!/bin/bash
#
# Regression test: `strada --tcc --shared` libraries must initialize at
# dlopen. tcc SILENTLY DROPS __attribute__((constructor)), so without the
# wrapper's $CC-compiled constructor shim the .so's __strada_init_globals
# (our-globals, OOP registration, BEGIN blocks) never runs — globals read
# as undef and method dispatch fails at runtime, even though the build
# succeeds. (Found via sysync-web misbehaving under --tcc.)
#
# Exits non-zero on any failure.

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
STRADA="$REPO_DIR/strada"

if [ ! -x "$STRADA" ]; then
    echo "Build strada first (run 'make' in $REPO_DIR)" >&2
    exit 2
fi
if ! command -v tcc >/dev/null 2>&1; then
    echo "SKIP: tcc not installed"
    exit 0
fi

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/TLib.strada" <<'EOF'
package TLib;

our int $counter = 41;

func bump() int {
    $counter = $counter + 1;
    return $counter;
}

func make_greeter(str $name) scalar {
    my hash %obj = ();
    $obj{"name"} = $name;
    return bless(\%obj, "TLib");
}

func greet(scalar $self) str {
    return "hello " . $self->{"name"};
}
EOF

cat > "$WORK/host.strada" <<EOF
use lib "$WORK";
import_lib "TLib.so";

func main() int {
    # bump() reads the our-global set by __strada_init_globals: without
    # the dlopen constructor it sees undef (0) and returns 1, not 42.
    my int \$v = TLib::bump();
    if (\$v != 42) { say("FAIL global: " . \$v); return 1; }
    # Method dispatch needs the constructor's OOP registration.
    my scalar \$g = TLib::make_greeter("web");
    my str \$s = \$g->greet();
    if (\$s ne "hello web") { say("FAIL method: " . \$s); return 1; }
    say("OK");
    return 0;
}
EOF

cd "$WORK"
if ! "$STRADA" --tcc --shared TLib.strada -o TLib.so > "$WORK/lib.log" 2>&1; then
    echo "FAIL: --tcc --shared library build failed"; cat "$WORK/lib.log"; exit 1
fi
if ! "$STRADA" -o host host.strada > "$WORK/host.log" 2>&1; then
    echo "FAIL: host build failed"; cat "$WORK/host.log"; exit 1
fi
out="$(./host)"
if [ "$out" != "OK" ]; then
    echo "FAIL: tcc-shared library not initialized at dlopen (got: $out)"
    exit 1
fi
echo "PASS --tcc --shared constructor shim (globals + OOP init at dlopen)"

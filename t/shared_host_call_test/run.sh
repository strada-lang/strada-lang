#!/bin/bash
#
# Regression test (Cannoli report #4): a --shared library calling a function
# that only the host program defines (`Host::Mod::make_frame($x)`). The call
# had no C declaration, so C assumed `int` and truncated the returned
# StradaValue* -- the plugin crashed on first use. The compiler now emits
# `extern StradaValue* Host_Mod_make_frame();` for host-resolved calls in
# files without main().
#
# Exits non-zero on any failure.

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
STRADA="$REPO_DIR/strada"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/Plug.strada" <<'SRC'
package Plug;
func go(scalar $x) str {
    my scalar $f = Host::Mod::make_frame($x);
    if (defined($f)) { return "got " . $f->{"v"}; }
    return "undef";
}
SRC
cat > "$WORK/host.strada" <<'SRC'
import_lib "Plug.so";
package Host::Mod;
func make_frame(scalar $x) scalar { my %h = ("v" => $x); return \%h; }
package main;
func main() int {
    my str $last = "";
    for (my int $i = 0; $i < 200; $i = $i + 1) { $last = Plug::go("x" . $i); }
    say($last);
    return 0;
}
SRC

cd "$WORK" || exit 1
if ! "$STRADA" --shared Plug.strada -o Plug.so > build.log 2>&1; then
    echo "FAIL: building Plug.so"; cat build.log; exit 1
fi
if grep -q "makes pointer from integer\|implicit declaration" build.log; then
    echo "FAIL: host call compiled without a declaration"; cat build.log; exit 1
fi
if ! "$STRADA" -O0 -L "$WORK" host.strada -o host > hbuild.log 2>&1; then
    echo "FAIL: building host"; cat hbuild.log; exit 1
fi
out=$(./host 2>&1)
if [ "$out" != "got x199" ]; then
    echo "FAIL: got '$out'"; exit 1
fi
echo "PASS"

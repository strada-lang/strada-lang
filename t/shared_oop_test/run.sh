#!/bin/bash
#
# Regression test: a `--shared` library with Moose-style classes
# (namespaced `has` / `extends` / SUPER::) must register its inheritance
# when it is dlopen'd. File-level `extends` used to be set up only in
# main(), so a library's subclasses couldn't find their parents' methods.
# A missing method must also be a catchable exception, not exit(1).
set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
STRADA="$REPO_DIR/strada"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

cat > "$WORK/OLib.strada" <<'EOF'
package OLib::Model;
has rw str $name = "";
has rw int $id = 0;
func label(scalar $self) str { return "#" . $self->id() . " " . $self->name(); }
func kind(scalar $self) str { return "model"; }

package OLib::Contact;
extends OLib::Model;
has rw str $email = "";
func kind(scalar $self) str { return "contact"; }
func label(scalar $self) str { return "contact " . SUPER::label($self) . " <" . $self->email() . ">"; }

package OLib;
func make(str $name, str $email) scalar {
    return OLib::Contact::new("name", $name, "id", 7, "email", $email);
}
func via_closure(str $x) str {
    my scalar $f = func (str $v) { return "lib:" . $v; };
    return $f->($x);
}
func missing(scalar $o) str {
    my str $r = "no exception";
    try { $o->no_such_method(); } catch ($e) { $r = "caught: " . $e; }
    return $r;
}
EOF

cat > "$WORK/host.strada" <<EOF
use lib "$WORK";
import_lib "OLib.so";
func main() int {
    my scalar \$c = OLib::make("Wile", "w\@acme.test");
    my str \$l = \$c->label();
    if (\$l ne "contact #7 Wile <w\@acme.test>") { say("FAIL inherited method: " . \$l); return 1; }
    if (\$c->isa("OLib::Model") != 1) { say("FAIL isa"); return 1; }
    my scalar \$hf = func (str \$v) { return "host:" . \$v; };
    if (\$hf->("a") ne "host:a") { say("FAIL host closure"); return 1; }
    if (OLib::via_closure("b") ne "lib:b") { say("FAIL library closure ran the host's: " . OLib::via_closure("b")); return 1; }
    my str \$m = OLib::missing(\$c);
    if (index(\$m, "caught: ") != 0 || index(\$m, "no_such_method") < 0) { say("FAIL missing method: " . \$m); return 1; }
    say("OK");
    return 0;
}
EOF

cd "$WORK"
if ! "$STRADA" --shared OLib.strada -o OLib.so > "$WORK/lib.log" 2>&1; then
    echo "FAIL: --shared library build failed"; cat "$WORK/lib.log"; exit 1
fi
if ! "$STRADA" -o host host.strada > "$WORK/host.log" 2>&1; then
    echo "FAIL: host build failed"; cat "$WORK/host.log"; exit 1
fi
out="$(./host 2>&1)"
if [ "$out" != "OK" ]; then
    echo "FAIL: shared-library OOP (got: $out)"
    exit 1
fi
echo "PASS --shared library inheritance, closures and catchable missing methods"

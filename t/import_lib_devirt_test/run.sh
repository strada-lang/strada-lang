#!/bin/bash
#
# Regression test: guarded cross-.so devirtualization (import_lib).
#
# A method call on a receiver whose class lives in an import_lib .so
# devirtualizes to a direct call through the host's import wrapper when
# (a) the library metadata carries modifier info (modinfo:1), (b) the
# method has no before/after/around hooks anywhere (host or lib mod:
# lines), and (c) the .so's metadata fingerprint at dlopen matches what
# the host was compiled against. On a fingerprint mismatch (the .so was
# swapped after the host was built) the wrapper falls back to dynamic
# dispatch — late-bound, hooks and overrides included — instead of
# calling a stale symbol.
#
# Scenarios:
#   1. Normal build+run: devirtualized methods produce correct results;
#      a lib-side `before` hook blocks devirtualization of that method
#      (mod: line) and fires.
#   2. Generated C: the hot method compiles to a direct call (no
#      dispatch by name); the hooked method does NOT.
#   3. Swap: replace the .so with a changed build (new behavior, a hook
#      added to the hot method, extra function -> metadata mismatch).
#      The UNREBUILT host must not crash, must see the new behavior, and
#      the new hook must fire — proving calls went through dynamic
#      dispatch, not the stale direct path.
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

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK"

fail() { echo "FAIL: $*" >&2; exit 1; }

# ---------- library, version 1 ----------
cat > DvLib.strada <<'EOF'
package DvCounter;

our int $hooks = 0;

func new(int $start) scalar {
    my hash %self = ();
    $self{"n"} = $start;
    return bless(\%self, "DvCounter");
}

func bump(scalar $self) int {
    $self->{"n"} = $self->{"n"} + 1;
    return $self->{"n"};
}

func value(scalar $self) int {
    return $self->{"n"};
}

before "logged_add" func(scalar $self) void {
    $hooks = $hooks + 1;
}

func logged_add(scalar $self, int $k) int {
    $self->{"n"} = $self->{"n"} + $k;
    return $self->{"n"};
}

func hook_count() int {
    return $hooks;
}
EOF

"$STRADA" --shared DvLib.strada -o DvLib.so || fail "lib v1 build"

# ---------- host ----------
cat > host.strada <<'EOF'
use lib ".";
import_lib "DvLib.so";

func main() int {
    my scalar $c = DvCounter::new(10);
    $c->bump();
    $c->bump();
    say("n=" . $c->value());
    $c->logged_add(5);
    say("n2=" . $c->value());
    say("hooks=" . DvCounter::hook_count());
    return 0;
}
EOF

"$STRADA" -c host.strada -o host || fail "host build"

# ---------- 1. normal run ----------
OUT="$(./host)" || fail "host run (v1) crashed"
echo "$OUT" | grep -q "^n=12$"    || fail "v1 n: got: $OUT"
echo "$OUT" | grep -q "^n2=17$"   || fail "v1 n2: got: $OUT"
echo "$OUT" | grep -q "^hooks=1$" || fail "v1 hooks (before hook must fire once): got: $OUT"

# ---------- 2. generated C shape ----------
# Call sites dispatch via strada_method_call_cs (per-site cache); the
# import wrapper's fallback uses _ph with __fb_args — don't match it.
# bump/value devirtualize: no call-site dispatch-by-name for them...
grep -Eq 'strada_method_call_cs\([^)]*"bump"' host.c \
    && fail "bump was NOT devirtualized (call-site dispatch by name found in host.c)"
# ...while the hooked method must stay on dynamic dispatch (mod: gating).
grep -Eq 'strada_method_call_cs\([^)]*"logged_add"' host.c \
    || fail "logged_add call-site dispatch missing — was it wrongly devirtualized?"
# The fingerprint guard machinery must be present.
grep -q "_devirt_ok" host.c || fail "devirt_ok guard missing from host.c"
grep -q "strada_export_meta_hash_cstr" host.c || fail "fingerprint check missing from host.c"

# ---------- 3. swapped .so (metadata mismatch) ----------
cat > DvLib2.strada <<'EOF'
package DvCounter;

our int $hooks = 0;

func new(int $start) scalar {
    my hash %self = ();
    $self{"n"} = $start;
    return bless(\%self, "DvCounter");
}

before "bump" func(scalar $self) void {
    $hooks = $hooks + 1;
}

func bump(scalar $self) int {
    $self->{"n"} = $self->{"n"} + 100;
    return $self->{"n"};
}

func value(scalar $self) int {
    return $self->{"n"};
}

before "logged_add" func(scalar $self) void {
    $hooks = $hooks + 1;
}

func logged_add(scalar $self, int $k) int {
    $self->{"n"} = $self->{"n"} + $k;
    return $self->{"n"};
}

func hook_count() int {
    return $hooks;
}

func extra_to_change_metadata() int {
    return 1;
}
EOF

"$STRADA" --shared DvLib2.strada -o DvLib.so || fail "lib v2 build"

OUT2="$(./host)" || fail "host run (v2, swapped .so) crashed"
echo "$OUT2" | grep -q "^n=210$"    || fail "v2 n (new bump behavior must apply): got: $OUT2"
echo "$OUT2" | grep -q "^n2=215$"   || fail "v2 n2: got: $OUT2"
# 2 bumps fire the NEW bump hook + 1 logged_add hook = 3. If the host had
# (wrongly) kept calling the symbol directly, the bump hook would be
# skipped and this would read 1.
echo "$OUT2" | grep -q "^hooks=3$"  || fail "v2 hooks (fallback dispatch must run new hooks): got: $OUT2"

echo "PASS: import_lib guarded devirtualization (direct calls, mod: gating, swap fallback)"
exit 0

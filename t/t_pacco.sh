#!/bin/bash
# t_pacco.sh - pacco package manager tests (offline, hermetic)
#
# Full project lifecycle without network access: new, build, run,
# path: dependencies, lockfile round-trip, remove, add validation,
# and git: dependencies from local file repos only.

PACCO="$PROJECT_DIR/tools/pacco/pacco"
WORK="$BUILD_DIR/pacco"
export PACCO_STRADA="$PROJECT_DIR/strada"
mkdir -p "$WORK"

pm_pass() { TOTAL=$((TOTAL + 1)); PASSED=$((PASSED + 1)); log_pass "$1"; }
pm_fail() { TOTAL=$((TOTAL + 1)); FAILED=$((FAILED + 1)); log_fail "$1" "$2"; }

if [ ! -x "$PACCO" ]; then
    test_skip "pacco lifecycle" "pacco not built (run: make pacco)"
else
    # --- new + build + run a hello project ---
    ( cd "$WORK" && "$PACCO" new demo >new.log 2>&1 )
    if [ -f "$WORK/demo/Strada.toml" ] && [ -f "$WORK/demo/src/main.strada" ]; then
        pm_pass "pacco: new creates project layout"
    else
        pm_fail "pacco: new creates project layout" "missing Strada.toml or src/main.strada"
    fi

    ( cd "$WORK/demo" && "$PACCO" build >build.log 2>&1 )
    if [ -x "$WORK/demo/.strada/build/demo" ]; then
        pm_pass "pacco: build produces binary"
    else
        pm_fail "pacco: build produces binary" "$(head -3 "$WORK/demo/build.log" 2>/dev/null)"
    fi

    if [ "$("$WORK/demo/.strada/build/demo" 2>/dev/null)" = "Hello from demo!" ]; then
        pm_pass "pacco: built binary runs"
    else
        pm_fail "pacco: built binary runs" "unexpected output"
    fi

    # --- path: dependency lifecycle ---
    ( cd "$WORK" && "$PACCO" new mylib >/dev/null 2>&1 && "$PACCO" new app >/dev/null 2>&1 )
    cat > "$WORK/mylib/src/Calc.strada" <<'EOF'
package Calc;
func add(int $a, int $b) int {
    return $a + $b;
}
EOF
    cat > "$WORK/app/src/main.strada" <<'EOF'
use Calc;
func main() int {
    say(Calc::add(20, 10));
    return 0;
}
EOF
    ( cd "$WORK/app" && "$PACCO" add "path:../mylib" >add.log 2>&1 )
    if grep -q '^mylib =' "$WORK/app/Strada.toml" && grep -q 'path = "../mylib"' "$WORK/app/Strada.toml"; then
        pm_pass "pacco: add path: records source in manifest"
    else
        pm_fail "pacco: add path: records source in manifest" "$(head -5 "$WORK/app/add.log")"
    fi

    ( cd "$WORK/app" && "$PACCO" install >install.log 2>&1 )
    if [ -f "$WORK/app/.strada/deps/mylib/Strada.toml" ] && grep -q '^\[package.mylib\]' "$WORK/app/Strada.lock"; then
        pm_pass "pacco: install fetches dep and writes lock"
    else
        pm_fail "pacco: install fetches dep and writes lock" "$(head -5 "$WORK/app/install.log")"
    fi

    ( cd "$WORK/app" && "$PACCO" build >build.log 2>&1 )
    if [ "$("$WORK/app/.strada/build/app" 2>/dev/null)" = "30" ]; then
        pm_pass "pacco: cross-package build and run"
    else
        pm_fail "pacco: cross-package build and run" "$(head -5 "$WORK/app/build.log")"
    fi

    # Re-adding the same dep must not duplicate the manifest entry.
    ( cd "$WORK/app" && "$PACCO" add "path:../mylib" >/dev/null 2>&1 )
    if [ "$(grep -c '^mylib =' "$WORK/app/Strada.toml")" = "1" ]; then
        pm_pass "pacco: re-add is idempotent"
    else
        pm_fail "pacco: re-add is idempotent" "duplicate manifest entries"
    fi

    # An unsatisfiable version fails without touching the manifest.
    cp "$WORK/app/Strada.toml" "$WORK/app/Strada.toml.bak"
    if ( cd "$WORK/app" && "$PACCO" add "path:../mylib" "99.0.0" >/dev/null 2>&1 ); then
        pm_fail "pacco: unsatisfiable version rejected" "add exited 0"
    elif cmp -s "$WORK/app/Strada.toml" "$WORK/app/Strada.toml.bak"; then
        pm_pass "pacco: unsatisfiable version rejected"
    else
        pm_fail "pacco: unsatisfiable version rejected" "manifest modified on failure"
    fi
    rm -f "$WORK/app/Strada.toml.bak"

    # --- remove ---
    ( cd "$WORK/app" && "$PACCO" remove mylib >remove.log 2>&1 )
    if ! grep -q '^mylib =' "$WORK/app/Strada.toml" && [ ! -e "$WORK/app/.strada/deps/mylib" ]; then
        pm_pass "pacco: remove cleans manifest and deps"
    else
        pm_fail "pacco: remove cleans manifest and deps" "$(head -5 "$WORK/app/remove.log")"
    fi

    # --- pacco test on a scaffolded project ---
    cat > "$WORK/demo/t/smoke.strada" <<'EOF'
func main() int {
    return 0;
}
EOF
    if ( cd "$WORK/demo" && "$PACCO" test >test.log 2>&1 ); then
        pm_pass "pacco: test runs t/ suite"
    else
        pm_fail "pacco: test runs t/ suite" "$(head -5 "$WORK/demo/test.log")"
    fi

    # --- registry names fail with a helpful error (no index published) ---
    if ( cd "$WORK/demo" && "$PACCO" add "some-registry-pkg" >reg.log 2>&1 ); then
        pm_fail "pacco: bare registry name rejected" "add exited 0"
    elif grep -qi "registry" "$WORK/demo/reg.log"; then
        pm_pass "pacco: bare registry name rejected"
    else
        pm_fail "pacco: bare registry name rejected" "error does not mention registry"
    fi

    # --- git: dependency from a local repo (no network) ---
    if command -v git >/dev/null 2>&1; then
        UP="$WORK/upstream"
        mkdir -p "$UP/src"
        printf '[package]\nname = "up"\nversion = "0.2.0"\n\n[dependencies]\n' > "$UP/Strada.toml"
        cat > "$UP/src/Up.strada" <<'EOF'
package Up;
func val() int {
    return 7;
}
EOF
        ( cd "$UP" && git init -q . && git add -A && git -c user.email=t@t -c user.name=t commit -qm init )
        ( cd "$WORK" && "$PACCO" new appgit >/dev/null 2>&1 )
        cat > "$WORK/appgit/src/main.strada" <<'EOF'
use Up;
func main() int {
    say(Up::val());
    return 0;
}
EOF
        ( cd "$WORK/appgit" && "$PACCO" add "git:$UP" >add.log 2>&1 && "$PACCO" install >install.log 2>&1 && "$PACCO" build >build.log 2>&1 )
        if [ "$("$WORK/appgit/.strada/build/appgit" 2>/dev/null)" = "7" ]; then
            pm_pass "pacco: git: dep lifecycle (local repo)"
        else
            pm_fail "pacco: git: dep lifecycle (local repo)" "$(head -5 "$WORK/appgit/build.log" "$WORK/appgit/install.log" 2>/dev/null)"
        fi
    else
        test_skip "pacco git: dep lifecycle" "git not installed"
    fi
fi

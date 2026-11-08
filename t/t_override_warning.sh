#!/bin/bash
# t_override_warning.sh - overriding an inherited method with a different
# number of parameters is warned about (the parent's own calls would run the
# override with the wrong arguments); a matching override is not.
TOTAL=$((TOTAL + 1))
ow_src="$BUILD_DIR/override_warn.strada"
cat > "$ow_src" <<'SRC'
package Base;
func page(scalar $self, int $code, str $msg) str { return "page " . $code; }
func same(scalar $self, int $x) int { return $x; }
package Child;
extends Base;
func make() scalar { my hash %h = (); return bless(\%h, "Child"); }
func page(scalar $self) str { return "child"; }
func same(scalar $self, int $y) int { return $y + 1; }
package main;
func main() int { say(Child::make()->same(1)); return 0; }
SRC
ow_out=$("$PROJECT_DIR/strada" "$ow_src" -o "$BUILD_DIR/override_warn" 2>&1)
if echo "$ow_out" | grep -q "override_warn.strada:7: warning: Child::page takes 0 arguments but overrides Base::page (line 2), which takes 2" \
   && ! echo "$ow_out" | grep -q "Child::same"; then
    PASSED=$((PASSED + 1)); log_pass "Overriding a method with a different signature is warned about"
else
    FAILED=$((FAILED + 1)); log_fail "Overriding a method with a different signature is warned about" "got: $(echo "$ow_out" | grep -i warning | head -2)"
fi

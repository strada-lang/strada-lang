#!/bin/bash
# t_phase3_artifacts.sh - Phase 3 regression tests for .strada_meta artifact metadata

p3a_dir="$BUILD_DIR/phase3_artifacts"
mkdir -p "$p3a_dir"

p3a_record() {
    local ok="$1"
    local name="$2"
    local diag="$3"
    TOTAL=$((TOTAL + 1))
    if [ "$ok" -eq 0 ]; then
        PASSED=$((PASSED + 1))
        log_pass "$name"
    else
        FAILED=$((FAILED + 1))
        log_fail "$name" "$diag"
    fi
}

cat > "$p3a_dir/MetaMod.strada" << 'EOF'
package MetaMod;

enum Color {
    RED = 1,
    GREEN = 2
}

const int MAGIC = 99;
const str GREETING = "hello";

has ro int $x = 10;
has rw str $label = "init";

func describe(scalar $obj) str {
    return $obj->{"label"} . ":" . $obj->{"x"} . ":" . MAGIC . ":" . Color::GREEN . ":" . GREETING;
}
EOF

cat > "$p3a_dir/use_metamod.strada" << 'EOF'
use lib "$P3A_DIR";
use MetaMod;

func main() int {
    my scalar $m = MetaMod::new("x" => 12, "label" => "ok");
    say(MetaMod::describe($m) . ":" . MAGIC . ":" . Color::RED);
    return 0;
}
EOF
sed -i "s|\$P3A_DIR|$p3a_dir|g" "$p3a_dir/use_metamod.strada"

"$PROJECT_DIR/strada" --shared -O0 "$p3a_dir/MetaMod.strada" -o "$p3a_dir/MetaMod.so" >/dev/null 2>&1
if [ -f "$p3a_dir/MetaMod.so.strada_meta" ] || "$PROJECT_DIR/tools/strada-soinfo" "$p3a_dir/MetaMod.so" | grep -q "describe"; then
    p3a_out=$("$PROJECT_DIR/strada" --use-artifacts -O0 "$p3a_dir/use_metamod.strada" -o "$p3a_dir/use_metamod" 2>&1 \
        && "$p3a_dir/use_metamod" 2>&1)
    [ "$p3a_out" = "ok:12:99:2:hello:99:1" ]
    p3a_record $? ".strada_meta preserves enum/const/has metadata across --use-artifacts shared library" "out=$p3a_out"
else
    p3a_record 1 ".strada_meta preserves enum/const/has metadata across --use-artifacts shared library" "so compile failed"
fi

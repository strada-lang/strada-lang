#!/bin/bash
# t_phase3_compiler.sh - Phase 3 regression tests for compiler & codegen fixes

p3_dir="$BUILD_DIR/phase3_compiler"
mkdir -p "$p3_dir"

p3_record() {
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

# 1. Main Phase 3 compiler/codegen test suite
p3_out=$("$PROJECT_DIR/strada" -O0 "$SCRIPT_DIR/t_phase3_compiler.strada" -o "$p3_dir/p3_bin" 2>/dev/null \
    && "$p3_dir/p3_bin" 2>&1)
[ "$p3_out" = "PHASE3_COMPILER_OK" ]
p3_record $? "phase3 compiler/codegen features (q, weaken, tie, foreach, map/grep, closures, catch, has)" "out=$p3_out"

# 2. --split-dir with aggressive inline threshold (<100 funcs) & C-mangled function names
cat > "$p3_dir/split_kw.strada" << 'EOF'
package main;
func fopen(int $x) int {
    return $x + 1;
}
func strlen(int $y) int {
    return fopen($y) * 2;
}
func main() int {
    say("SPLIT=" . strlen(20));
    return 0;
}
EOF
mkdir -p "$p3_dir/shards"
p3_split_out=$("$PROJECT_DIR/stradac" --split-dir "$p3_dir/shards" "$p3_dir/split_kw.strada" "$p3_dir/split_kw.c" 2>&1 \
    && gcc -O0 -Wl,--gc-sections $EXTRA_LDFLAGS -I"$PROJECT_DIR/runtime" "$p3_dir/split_kw.c" "$p3_dir/shards"/shard_*.c "$PROJECT_DIR/runtime/strada_runtime.o" -lm -ldl -lpthread -o "$p3_dir/split_kw_bin" 2>&1 \
    && "$p3_dir/split_kw_bin" 2>&1)
[ "$p3_split_out" = "SPLIT=42" ]
p3_record $? "--split-dir builds and links C-mangled functions without static inline proto mismatch" "out=$p3_split_out"

# 3. Module build (-M) + .strada_meta + C-mangled export
cat > "$p3_dir/KwMod.strada" << 'EOF'
package KwMod;
func fopen(int $x) int {
    return $x + 5;
}
EOF
cat > "$p3_dir/use_kwmod.strada" << 'EOF'
use lib "$BUILD_DIR_PLACEHOLDER";
use KwMod;
func main() int {
    say("MOD=" . KwMod::fopen(37));
    return 0;
}
EOF
sed -i "s|\$BUILD_DIR_PLACEHOLDER|$p3_dir|g" "$p3_dir/use_kwmod.strada"
p3_mod_out=$("$PROJECT_DIR/strada" -M -O0 "$p3_dir/KwMod.strada" -o "$p3_dir/KwMod.o" 2>&1 \
    && "$PROJECT_DIR/strada" -O0 "$p3_dir/use_kwmod.strada" -o "$p3_dir/use_kwmod_bin" 2>&1 \
    && "$p3_dir/use_kwmod_bin" 2>&1)
[ "$p3_mod_out" = "MOD=42" ]
p3_record $? "-M module build and linking with C-keyword exported function" "out=$p3_mod_out"

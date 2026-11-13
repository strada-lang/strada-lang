#!/bin/bash
# t_audit_r2.sh — Cycle 2 Audit regression suite
# Covers runtime/concurrency, compiler/codegen, stdlib & CLI tools,
# Eval (tree-walk & VM), and Strada::JIT fixes.

aud_dir="$BUILD_DIR/audit_r2"
aud_src="$SCRIPT_DIR/audit_r2"
mkdir -p "$aud_dir"

aud_r2_record() {
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

aud_r2_fast_compile() {
    local src="$1"
    local name="$2"
    local extra_ld="${3:-}"
    (
        cd "$PROJECT_DIR" &&
        BUILD_DIR="$aud_dir" STRADA_TEST_TCC=1 EXTRA_LDFLAGS="${EXTRA_LDFLAGS:-} $extra_ld" compile_strada "$src" "$name"
    )
}

# Launch compilations in parallel
(
    rc=0
    aud_r2_fast_compile "$aud_src/runtime.strada" "r2_runtime" &&
    timeout 20 "$aud_dir/r2_runtime" > "$aud_dir/runtime.out" 2>&1 || rc=1
    echo $rc > "$aud_dir/runtime.rc"
) &
pid_rt=$!

(
    rc=0
    aud_r2_fast_compile "$aud_src/compiler.strada" "r2_compiler" &&
    timeout 15 "$aud_dir/r2_compiler" > "$aud_dir/compiler.out" 2>&1 || rc=1
    echo $rc > "$aud_dir/compiler.rc"
) &
pid_comp=$!

(
    rc=0
    "$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$aud_src/stdlib_tools.strada" -o "$aud_dir/r2_stdlib" > "$aud_dir/stdlib.build.log" 2>&1 &&
    timeout 15 "$aud_dir/r2_stdlib" --no-v --tag -dashval > "$aud_dir/stdlib.out" 2>&1 || rc=1
    echo $rc > "$aud_dir/stdlib.rc"
) &
pid_stdlib=$!

(
    rc=0
    "$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$aud_src/eval_jit_checks.strada" -o "$aud_dir/r2_eval_jit" > "$aud_dir/eval_jit.build.log" 2>&1 &&
    timeout 30 "$aud_dir/r2_eval_jit" > "$aud_dir/eval_jit.out" 2>&1 || rc=1
    echo $rc > "$aud_dir/eval_jit.rc"
) &
pid_ej=$!

(
    rc=0
    "$PROJECT_DIR/strada" -O0 --no-use-artifacts -L "$PROJECT_DIR/lib" -D HAVE_VM -I "$PROJECT_DIR/interpreter" \
        "$aud_src/vm_eval_checks.strada" "$PROJECT_DIR/interpreter/vm.c" "$PROJECT_DIR/interpreter/vm_compiler.c" \
        -o "$aud_dir/r2_vm_eval" > "$aud_dir/vm_eval.build.log" 2>&1 &&
    timeout 30 "$aud_dir/r2_vm_eval" > "$aud_dir/vm_eval.out" 2>&1 || rc=1
    echo $rc > "$aud_dir/vm_eval.rc"
) &
pid_vme=$!

wait "$pid_rt" "$pid_comp" "$pid_stdlib" "$pid_ej" "$pid_vme"

# Check 1: Runtime
rt_rc=$(cat "$aud_dir/runtime.rc" 2>/dev/null || echo 1)
rt_out=$(cat "$aud_dir/runtime.out" 2>/dev/null || true)
if [ "$rt_rc" -eq 0 ] && echo "$rt_out" | grep -q "ALL_R2_RUNTIME_OK"; then
    aud_r2_record 0 "audit_r2 runtime fixes" ""
else
    aud_r2_record 1 "audit_r2 runtime fixes" "rc=$rt_rc out=$rt_out"
fi

# Check 2: Compiler
comp_rc=$(cat "$aud_dir/compiler.rc" 2>/dev/null || echo 1)
comp_out=$(cat "$aud_dir/compiler.out" 2>/dev/null || true)
if [ "$comp_rc" -eq 0 ] && echo "$comp_out" | grep -q "ALL_R2_COMPILER_OK"; then
    aud_r2_record 0 "audit_r2 compiler/codegen fixes" ""
else
    aud_r2_record 1 "audit_r2 compiler/codegen fixes" "rc=$comp_rc out=$comp_out"
fi

# Check 3: Stdlib
std_rc=$(cat "$aud_dir/stdlib.rc" 2>/dev/null || echo 1)
std_out=$(cat "$aud_dir/stdlib.out" 2>/dev/null || true)
if [ "$std_rc" -eq 0 ] && echo "$std_out" | grep -q "ALL_R2_STDLIB_OK"; then
    aud_r2_record 0 "audit_r2 stdlib fixes" ""
else
    aud_r2_record 1 "audit_r2 stdlib fixes" "rc=$std_rc out=$std_out build=$(tail -n 10 "$aud_dir/stdlib.build.log" 2>/dev/null)"
fi

# Check 4: Eval (TW) & Strada::JIT
ej_rc=$(cat "$aud_dir/eval_jit.rc" 2>/dev/null || echo 1)
ej_out=$(cat "$aud_dir/eval_jit.out" 2>/dev/null || true)
if [ "$ej_rc" -eq 0 ] && echo "$ej_out" | grep -q "ALL_R2_EVAL_JIT_OK"; then
    aud_r2_record 0 "audit_r2 Eval & Strada::JIT fixes" ""
else
    aud_r2_record 1 "audit_r2 Eval & Strada::JIT fixes" "rc=$ej_rc out=$ej_out build=$(tail -n 10 "$aud_dir/eval_jit.build.log" 2>/dev/null)"
fi

# Check 5: Eval::eval_string_vm (HAVE_VM)
vme_rc=$(cat "$aud_dir/vm_eval.rc" 2>/dev/null || echo 1)
vme_out=$(cat "$aud_dir/vm_eval.out" 2>/dev/null || true)
if [ "$vme_rc" -eq 0 ] && echo "$vme_out" | grep -q "ALL_R2_VM_EVAL_OK"; then
    aud_r2_record 0 "audit_r2 Eval::eval_string_vm fixes" ""
else
    aud_r2_record 1 "audit_r2 Eval::eval_string_vm fixes" "rc=$vme_rc out=$vme_out build=$(tail -n 10 "$aud_dir/vm_eval.build.log" 2>/dev/null)"
fi

# Check 6: CLI Tools
tools_ok=0
tools_diag=""
cat > "$aud_dir/md_in.md" << 'EOF'
# Title

[xss](data:text/html,<script>1</script>)

~~~~
```
code
```
~~~~
EOF
md_html_out=$("$PROJECT_DIR/tools/strada-md2html" "$aud_dir/md_in.md" 2>&1) || tools_ok=1
if echo "$md_html_out" | grep -q "data:text/html"; then
    tools_ok=1; tools_diag="strada-md2html leaked data:text/html"
fi
jit_bad_rc=0
"$PROJECT_DIR/tools/strada-jit" --unknown-flag-xyz >/dev/null 2>&1 || jit_bad_rc=$?
if [ "$jit_bad_rc" -eq 0 ]; then
    tools_ok=1; tools_diag="strada-jit unknown flag exited 0"
fi
cat > "$aud_dir/jit_cli.strada" << 'EOF'
func main() int {
    say("JIT_CLI_OK");
    return 0;
}
EOF
jit_cli_out=$("$PROJECT_DIR/tools/strada-jit" "$aud_dir/jit_cli.strada" 2>&1) || tools_ok=1
if ! echo "$jit_cli_out" | grep -q "JIT_CLI_OK"; then
    tools_ok=1; tools_diag="strada-jit script failed: $jit_cli_out"
fi
aud_r2_record "$tools_ok" "audit_r2 CLI tools checks" "$tools_diag"

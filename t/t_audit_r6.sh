#!/bin/bash
# t_audit_r6.sh — Cycle 6 Audit regression suite

aud_dir="$BUILD_DIR/audit_r6"
aud_src="$SCRIPT_DIR/audit_r6"
mkdir -p "$aud_dir"

aud_r6_record() {
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

aud_r6_compile_run() {
    local opt_flag="$1"
    local src="$2"
    local name="$3"
    local expected="$4"
    shift 4
    local bin="$aud_dir/$name"
    local log="$aud_dir/${name}.compile.log"
    local out=""
    local rc=1
    if (cd "$PROJECT_DIR" && "$PROJECT_DIR/strada" "$opt_flag" -L "$PROJECT_DIR/lib" "$src" "$@" -o "$bin" >"$log" 2>&1); then
        out=$(cd "$PROJECT_DIR" && timeout 45 "$bin" 2>&1) && rc=0 || rc=$?
    else
        out=$(tail -n 20 "$log" 2>/dev/null || true)
    fi
    if [ "$rc" -eq 0 ] && echo "$out" | grep -q "$expected"; then
        return 0
    fi
    echo "rc=$rc out=$out" > "$aud_dir/${name}.diag"
    return 1
}

# 1. Runtime & Compiler Cycle 6 suite (-O0 and -O2)
if aud_r6_compile_run "-O0" "$aud_src/runtime_compiler.strada" "r6_runtime_compiler_o0" "R6_RUNTIME_COMPILER_OK"; then
    aud_r6_record 0 "Cycle 6 runtime + compiler + stdlib regressions (-O0)" ""
else
    aud_r6_record 1 "Cycle 6 runtime + compiler + stdlib regressions (-O0)" "$(cat "$aud_dir/r6_runtime_compiler_o0.diag" 2>/dev/null)"
fi

if aud_r6_compile_run "-O2" "$aud_src/runtime_compiler.strada" "r6_runtime_compiler_o2" "R6_RUNTIME_COMPILER_OK"; then
    aud_r6_record 0 "Cycle 6 runtime + compiler + stdlib regressions (-O2)" ""
else
    aud_r6_record 1 "Cycle 6 runtime + compiler + stdlib regressions (-O2)" "$(cat "$aud_dir/r6_runtime_compiler_o2.diag" 2>/dev/null)"
fi

# 2. Bootstrap compiler: <=>, 'x', single-quote backslash escapes, try/catch
cat > "$aud_dir/bs_r6.strada" <<'EOF'
func main() int {
    my str $nl = '\n';
    my str $bs = '\\';
    my str $sq = '\'';
    my int $c1 = ('a' <=> 'b');
    my int $c2 = (10 <=> 2);
    my int $caught = 0;
    try {
        throw "bs_err";
    } catch ($e) {
        $caught = 1;
    }
    if (length($nl) == 2 && $bs eq "\\" && $sq eq "'" && $c1 == -1 && $c2 == 1 && $caught == 1) {
        say("BS_R6_OK");
        return 0;
    }
    say("BS_R6_FAIL");
    return 1;
}
EOF
bs_rc=1
bs_out=""
if "$PROJECT_DIR/bootstrap/stradac" "$aud_dir/bs_r6.strada" "$aud_dir/bs_r6.c" >"$aud_dir/bs_r6.log" 2>&1 \
    && gcc -O2 -I"$PROJECT_DIR/runtime" "$aud_dir/bs_r6.c" "$PROJECT_DIR/runtime/strada_runtime.c" -ldl -lm -lpthread -o "$aud_dir/bs_r6" >>"$aud_dir/bs_r6.log" 2>&1; then
    bs_out=$(timeout 15 "$aud_dir/bs_r6" 2>&1) && bs_rc=0 || bs_rc=$?
else
    bs_out=$(tail -n 20 "$aud_dir/bs_r6.log" 2>/dev/null || true)
fi
if [ "$bs_rc" -eq 0 ] && echo "$bs_out" | grep -q "BS_R6_OK"; then
    aud_r6_record 0 "Cycle 6 bootstrap compiler: <=>, 'x', single-quote escapes, try/catch" ""
else
    aud_r6_record 1 "Cycle 6 bootstrap compiler: <=>, 'x', single-quote escapes, try/catch" "rc=$bs_rc out=$bs_out"
fi

# 3. Core parity suite across compiled binary, tree-walk, and bytecode VM
if aud_r6_compile_run "-O0" "$aud_src/parity_core.strada" "r6_parity_core" "R6_PARITY_OK"; then
    "$aud_dir/r6_parity_core" > "$aud_dir/parity_comp.out" 2>&1
    tw_rc=1
    tw_out=$(cd "$PROJECT_DIR" && timeout 45 "$PROJECT_DIR/interpreter/strada-interp" --tree-walk "$aud_src/parity_core.strada" 2>&1) && tw_rc=0 || tw_rc=$?
    printf '%s\n' "$tw_out" > "$aud_dir/parity_tw.out"
    vm_rc=1
    vm_out=$(cd "$PROJECT_DIR" && timeout 45 "$PROJECT_DIR/interpreter/strada-interp" --vm "$aud_src/parity_core.strada" 2>&1) && vm_rc=0 || vm_rc=$?
    printf '%s\n' "$vm_out" > "$aud_dir/parity_vm.out"

    if [ "$tw_rc" -eq 0 ] && diff -u "$aud_dir/parity_comp.out" "$aud_dir/parity_tw.out" > "$aud_dir/parity_tw.diff" 2>&1; then
        aud_r6_record 0 "Cycle 6 compiled vs tree-walk parity (exact stdout diff)" ""
    else
        aud_r6_record 1 "Cycle 6 compiled vs tree-walk parity" "rc=$tw_rc diff=$(cat "$aud_dir/parity_tw.diff" 2>/dev/null)"
    fi

    if [ "$vm_rc" -eq 0 ] && diff -u "$aud_dir/parity_comp.out" "$aud_dir/parity_vm.out" > "$aud_dir/parity_vm.diff" 2>&1; then
        aud_r6_record 0 "Cycle 6 compiled vs bytecode --vm parity (exact stdout diff)" ""
    else
        aud_r6_record 1 "Cycle 6 compiled vs bytecode --vm parity" "rc=$vm_rc diff=$(cat "$aud_dir/parity_vm.diff" 2>/dev/null)"
    fi
else
    aud_r6_record 1 "Cycle 6 parity_core compiled run" "$(cat "$aud_dir/r6_parity_core.diag" 2>/dev/null)"
    aud_r6_record 1 "Cycle 6 compiled vs tree-walk parity (skipped due to compiled failure)" ""
fi

# 4. Eval::eval_string_vm + Strada::JIT suite
if aud_r6_compile_run "-O0" "$aud_src/eval_jit.strada" "r6_eval_jit" "R6_EVAL_JIT_OK" \
        --no-use-artifacts -D HAVE_VM -I "$PROJECT_DIR/interpreter" \
        "$PROJECT_DIR/interpreter/vm.c" "$PROJECT_DIR/interpreter/vm_compiler.c"; then
    aud_r6_record 0 "Cycle 6 eval_string_vm + Strada::JIT suite" ""
else
    aud_r6_record 1 "Cycle 6 eval_string_vm + Strada::JIT suite" "$(cat "$aud_dir/r6_eval_jit.diag" 2>/dev/null)"
fi

# 5. stradapp: #if(1), #if!0, #elif(1) without whitespace after directive
cat > "$aud_dir/pp_r6.strada" <<'EOF'
#if(1)
my int $p1 = 10;
#else
my int $p1 = 0;
#endif
#if!0
my int $p2 = 20;
#else
my int $p2 = 0;
#endif
#if 0
my int $p3 = 0;
#elif(1)
my int $p3 = 30;
#endif
say("PP_R6_" . ($p1 + $p2 + $p3));
EOF
pp_rc=1
pp_out=""
if "$PROJECT_DIR/tools/stradapp" "$aud_dir/pp_r6.strada" -o "$aud_dir/pp_r6.out.strada" >"$aud_dir/pp_r6.log" 2>&1; then
    pp_out=$(cat "$aud_dir/pp_r6.out.strada") && pp_rc=0
else
    pp_out=$(cat "$aud_dir/pp_r6.log" 2>/dev/null || true)
fi
if [ "$pp_rc" -eq 0 ] \
    && echo "$pp_out" | grep -q 'my int \$p1 = 10;' \
    && echo "$pp_out" | grep -q 'my int \$p2 = 20;' \
    && echo "$pp_out" | grep -q 'my int \$p3 = 30;' \
    && ! echo "$pp_out" | grep -q ' = 0;'; then
    aud_r6_record 0 "tools/stradapp handles #if(1), #if!0, #elif(1) without space" ""
else
    aud_r6_record 1 "tools/stradapp handles #if(1), #if!0, #elif(1) without space" "rc=$pp_rc out=$pp_out"
fi

# 6. Interpreter END block runs even when main() throws uncaught exception
cat > "$aud_dir/end_throw_r6.strada" <<'EOF'
END {
    say("END_RAN_R6");
}
func main() int {
    throw("uncaught_r6");
    return 0;
}
EOF
end_tw_out=$(cd "$PROJECT_DIR" && timeout 15 "$PROJECT_DIR/interpreter/strada-interp" --tree-walk "$aud_dir/end_throw_r6.strada" 2>&1)
end_tw_rc=$?
if [ "$end_tw_rc" -ne 0 ] && echo "$end_tw_out" | grep -q "END_RAN_R6"; then
    aud_r6_record 0 "interpreter --tree-walk runs END block when main() throws" ""
else
    aud_r6_record 1 "interpreter --tree-walk runs END block when main() throws" "rc=$end_tw_rc out=$end_tw_out"
fi

end_vm_out=$(cd "$PROJECT_DIR" && timeout 15 "$PROJECT_DIR/interpreter/strada-interp" --vm "$aud_dir/end_throw_r6.strada" 2>&1)
end_vm_rc=$?
if [ "$end_vm_rc" -ne 0 ] && echo "$end_vm_out" | grep -q "END_RAN_R6"; then
    aud_r6_record 0 "interpreter --vm runs END block when main() throws" ""
else
    aud_r6_record 1 "interpreter --vm runs END block when main() throws" "rc=$end_vm_rc out=$end_vm_out"
fi

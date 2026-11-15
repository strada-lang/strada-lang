#!/bin/bash
# t_audit_r10.sh — Cycle 10 Audit regression suite

aud_dir="$BUILD_DIR/audit_r10"
aud_src="$SCRIPT_DIR/audit_r10"
mkdir -p "$aud_dir"

aud_r10_record() {
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

aud_r10_compile_run() {
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
        out=$(tail -n 25 "$log" 2>/dev/null || true)
    fi
    if [ "$rc" -eq 0 ] && echo "$out" | grep -q "$expected"; then
        return 0
    fi
    echo "rc=$rc out=$out" > "$aud_dir/${name}.diag"
    return 1
}

# 1. Runtime & Compiler Cycle 10 suite (-O0)
if aud_r10_compile_run "-O0" "$aud_src/runtime_compiler.strada" "r10_runtime_compiler_o0" "R10_RUNTIME_COMPILER_OK" -lpthread; then
    aud_r10_record 0 "Cycle 10 Runtime & Compiler suite (-O0)" ""
else
    aud_r10_record 1 "Cycle 10 Runtime & Compiler suite (-O0)" "$(cat "$aud_dir/r10_runtime_compiler_o0.diag" 2>/dev/null)"
fi

# 2. Runtime & Compiler Cycle 10 suite (-O2)
if aud_r10_compile_run "-O2" "$aud_src/runtime_compiler.strada" "r10_runtime_compiler_o2" "R10_RUNTIME_COMPILER_OK" -lpthread; then
    aud_r10_record 0 "Cycle 10 Runtime & Compiler suite (-O2)" ""
else
    aud_r10_record 1 "Cycle 10 Runtime & Compiler suite (-O2)" "$(cat "$aud_dir/r10_runtime_compiler_o2.diag" 2>/dev/null)"
fi

# 3. Standard Library Cycle 10 suite (-O0)
if aud_r10_compile_run "-O0" "$aud_src/stdlib_tools.strada" "r10_stdlib_tools" "R10_STDLIB_OK"; then
    aud_r10_record 0 "Cycle 10 Stdlib suite (-O0)" ""
else
    aud_r10_record 1 "Cycle 10 Stdlib suite (-O0)" "$(cat "$aud_dir/r10_stdlib_tools.diag" 2>/dev/null)"
fi

# 4. 3-way compiled / --tree-walk / --vm parity (R10-VM1..R10-VM9, R10-C5)
parity_src="$PROJECT_DIR/interpreter/test/test_audit_r10_vm.strada"
parity_exp="$PROJECT_DIR/interpreter/test/test_audit_r10_vm.expected"
if (cd "$PROJECT_DIR" && "$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$parity_src" -lpthread -o "$aud_dir/r10_parity_comp" >"$aud_dir/r10_parity_comp.log" 2>&1); then
    (cd "$PROJECT_DIR" && timeout 20 "$aud_dir/r10_parity_comp" >"$aud_dir/parity_comp.out" 2>&1) && comp_rc=0 || comp_rc=$?
else
    comp_rc=1
    cat "$aud_dir/r10_parity_comp.log" > "$aud_dir/parity_comp.out" 2>/dev/null || true
fi

if [ "$comp_rc" -eq 0 ] && diff -u "$parity_exp" "$aud_dir/parity_comp.out" > "$aud_dir/parity_exp.diff" 2>&1; then
    (cd "$PROJECT_DIR" && timeout 20 "$PROJECT_DIR/interpreter/strada-interp" --tree-walk "$parity_src" >"$aud_dir/parity_tw.out" 2>&1) && tw_rc=0 || tw_rc=$?
    if [ "$tw_rc" -eq 0 ] && diff -u "$aud_dir/parity_comp.out" "$aud_dir/parity_tw.out" > "$aud_dir/parity_tw.diff" 2>&1; then
        aud_r10_record 0 "Cycle 10 compiled vs tree-walk parity (exact stdout diff)" ""
    else
        aud_r10_record 1 "Cycle 10 compiled vs tree-walk parity" "rc=$tw_rc diff=$(cat "$aud_dir/parity_tw.diff" 2>/dev/null)"
    fi

    (cd "$PROJECT_DIR" && timeout 20 "$PROJECT_DIR/interpreter/strada-interp" --vm "$parity_src" >"$aud_dir/parity_vm.out" 2>&1) && vm_rc=0 || vm_rc=$?
    if [ "$vm_rc" -eq 0 ] && diff -u "$aud_dir/parity_comp.out" "$aud_dir/parity_vm.out" > "$aud_dir/parity_vm.diff" 2>&1; then
        aud_r10_record 0 "Cycle 10 compiled vs bytecode --vm parity (exact stdout diff)" ""
    else
        aud_r10_record 1 "Cycle 10 compiled vs bytecode --vm parity" "rc=$vm_rc diff=$(cat "$aud_dir/parity_vm.diff" 2>/dev/null)"
    fi
else
    aud_r10_record 1 "Cycle 10 parity compiled vs expected (--tree-walk)" "rc=$comp_rc out=$(cat "$aud_dir/parity_comp.out" 2>/dev/null) diff=$(cat "$aud_dir/parity_exp.diff" 2>/dev/null)"
    aud_r10_record 1 "Cycle 10 parity compiled vs bytecode --vm" "skipped due to compiled failure"
fi

# 5. Eval 2-way parity suite (R10-VM5, R10-VM6, R10-VM10)
if aud_r10_compile_run "-O0" "$aud_src/eval_jit.strada" "r10_eval_jit" "R10_EVAL_JIT_OK" \
        --no-use-artifacts -D HAVE_VM -I "$PROJECT_DIR/interpreter" \
        "$PROJECT_DIR/interpreter/vm.c" "$PROJECT_DIR/interpreter/vm_compiler.c"; then
    aud_r10_record 0 "Cycle 10 Eval 2-way parity suite (R10-VM5, R10-VM6, R10-VM10)" ""
else
    aud_r10_record 1 "Cycle 10 Eval 2-way parity suite" "$(cat "$aud_dir/r10_eval_jit.diag" 2>/dev/null)"
fi

# 6. R10-TOOL1: strada-md2html self-referential sentinel DoS & attribute breakout
if [ -x "$PROJECT_DIR/tools/strada-md2html" ]; then
    printf '`\001T0\002`\n' > "$aud_dir/md_dos.md"
    printf '![a](https://example.com/1) ![b](https://example.com/\001T0\002)\n' > "$aud_dir/md_brk.md"
    dos_out=$(timeout 10 "$PROJECT_DIR/tools/strada-md2html" -f "$aud_dir/md_dos.md" 2>&1) && dos_rc=0 || dos_rc=$?
    brk_out=$(timeout 10 "$PROJECT_DIR/tools/strada-md2html" -f "$aud_dir/md_brk.md" 2>&1) && brk_rc=0 || brk_rc=$?
    if [ "$dos_rc" -eq 0 ] && [ "${#dos_out}" -lt 256 ] && echo "$dos_out" | grep -q "<code>" \
        && [ "$brk_rc" -eq 0 ] && ! echo "$brk_out" | grep -q 'src="https://example.com/<img'; then
        aud_r10_record 0 "R10-TOOL1 strada-md2html blocks sentinel DoS and attribute breakout" ""
    else
        aud_r10_record 1 "R10-TOOL1 strada-md2html" "dos_rc=$dos_rc dos_len=${#dos_out} brk_rc=$brk_rc brk=$brk_out"
    fi
else
    aud_r10_record 1 "R10-TOOL1 strada-md2html binary missing" ""
fi

# 7. R10-C6: tools/stradapp quote forms q/qq/qw/qr around directives
cat > "$aud_dir/r10_pp.strada" << 'EOF'
func main() int {
    my str $q = q(");
    my str $qq = qq(');
#ifdef R10_UNDEF_MACRO
    die("dead_branch_compiled");
#endif
    my str $multi = q(
#define R10_LEAKED 1
hello
);
    say("PP:" . $q . ":" . $qq . ":" . (index($multi, "R10_LEAKED") >= 0 ? "1" : "0"));
    return 0;
}
EOF
if (cd "$PROJECT_DIR" && "$PROJECT_DIR/strada" -O0 "$aud_dir/r10_pp.strada" -o "$aud_dir/r10_pp_bin" >"$aud_dir/r10_pp.log" 2>&1); then
    pp_out=$(timeout 10 "$aud_dir/r10_pp_bin" 2>&1) && pp_rc=0 || pp_rc=$?
    if [ "$pp_rc" -eq 0 ] && [ "$pp_out" = 'PP:":'\'':1' ]; then
        aud_r10_record 0 "R10-C6 stradapp q/qq multiline and #ifdef directive tracking" ""
    else
        aud_r10_record 1 "R10-C6 stradapp quote tracking" "rc=$pp_rc out=$pp_out"
    fi
else
    aud_r10_record 1 "R10-C6 stradapp build failed" "$(cat "$aud_dir/r10_pp.log" 2>/dev/null)"
fi

# 8. R10-VM4 stdout NUL check (--vm vs --tree-walk byte count)
nul_vm_len=$(cd "$PROJECT_DIR" && timeout 10 "$PROJECT_DIR/interpreter/strada-interp" --vm -e 'say("P" . chr(0) . "Q");' </dev/null 2>/dev/null | wc -c)
nul_tw_len=$(cd "$PROJECT_DIR" && timeout 10 "$PROJECT_DIR/interpreter/strada-interp" --tree-walk -e 'say("P" . chr(0) . "Q");' </dev/null 2>/dev/null | wc -c)
if [ "$nul_vm_len" -eq 4 ] && [ "$nul_tw_len" -eq 4 ]; then
    aud_r10_record 0 "R10-VM4 say() preserves embedded NUL bytes on stdout (--vm & --tree-walk)" ""
else
    aud_r10_record 1 "R10-VM4 stdout NUL check" "vm_len=$nul_vm_len tw_len=$nul_tw_len"
fi

# 9. R10-VM11 Strada::JIT out-of-tree $PATH resolution (gcc backend)
cat > "$aud_dir/r10_jit_path.strada" << 'EOF'
use Strada::JIT;
func main() int {
    Strada::JIT::init();
    my scalar $r = Strada::JIT::eval("20 + 22");
    Strada::JIT::reset();
    say("JIT_PATH=" . $r);
    return 0;
}
EOF
if (cd "$PROJECT_DIR" && "$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$aud_dir/r10_jit_path.strada" -o "$aud_dir/r10_jit_path_bin" >"$aud_dir/r10_jit_path.log" 2>&1); then
    jp_out=$(cd "$aud_dir" && PATH="$PROJECT_DIR:$PATH" STRADA_LIB="$PROJECT_DIR/lib" timeout 20 "$aud_dir/r10_jit_path_bin" 2>&1) && jp_rc=0 || jp_rc=$?
    if [ "$jp_rc" -eq 0 ] && [ "$jp_out" = "JIT_PATH=42" ]; then
        aud_r10_record 0 "R10-VM11 Strada::JIT gcc backend resolves strada from PATH outside repo root" ""
    else
        aud_r10_record 1 "R10-VM11 Strada::JIT PATH resolution" "rc=$jp_rc out=$jp_out"
    fi
else
    aud_r10_record 1 "R10-VM11 compile failed" "$(cat "$aud_dir/r10_jit_path.log" 2>/dev/null)"
fi

# 10. R10-VM12 strada-interp -e "" exits 0 cleanly without launching REPL
timeout 5 "$PROJECT_DIR/interpreter/strada-interp" --tree-walk -e "" </dev/null >"$aud_dir/empty_tw.out" 2>&1
e_tw_rc=$?
timeout 5 "$PROJECT_DIR/interpreter/strada-interp" --vm -e "" </dev/null >"$aud_dir/empty_vm.out" 2>&1
e_vm_rc=$?
if [ "$e_tw_rc" -eq 0 ] && [ "$e_vm_rc" -eq 0 ] && [ ! -s "$aud_dir/empty_tw.out" ] && [ ! -s "$aud_dir/empty_vm.out" ]; then
    aud_r10_record 0 "R10-VM12 strada-interp -e \"\" exits 0 cleanly (--tree-walk & --vm)" ""
else
    aud_r10_record 1 "R10-VM12 -e empty string" "tw_rc=$e_tw_rc vm_rc=$e_vm_rc tw=$(cat "$aud_dir/empty_tw.out" 2>/dev/null) vm=$(cat "$aud_dir/empty_vm.out" 2>/dev/null)"
fi

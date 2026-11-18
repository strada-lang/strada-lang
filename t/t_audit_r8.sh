#!/bin/bash
# t_audit_r8.sh — Cycle 8 Audit regression suite

aud_dir="$BUILD_DIR/audit_r8"
aud_src="$SCRIPT_DIR/audit_r8"
mkdir -p "$aud_dir"

aud_r8_record() {
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

aud_r8_compile_run() {
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

# 1 & 2. Runtime & Compiler Cycle 8 suite (-O0 by default, plus -O2 under --default)
for opt in ${STRADA_TEST_AUDIT_OPTS:--O0}; do
    tag="o0"; [ "$opt" = "-O2" ] && tag="o2"
    if aud_r8_compile_run "$opt" "$aud_src/runtime_compiler.strada" "r8_runtime_compiler_${tag}" "R8_RUNTIME_COMPILER_OK" -lpthread; then
        aud_r8_record 0 "Cycle 8 runtime + compiler regressions ($opt)" ""
    else
        aud_r8_record 1 "Cycle 8 runtime + compiler regressions ($opt)" "$(cat "$aud_dir/r8_runtime_compiler_${tag}.diag" 2>/dev/null)"
    fi
done

# 3. Stdlib & Tools Cycle 8 suite (-O0)
if aud_r8_compile_run "-O0" "$aud_src/stdlib_tools.strada" "r8_stdlib_tools" "R8_STDLIB_TOOLS_OK"; then
    aud_r8_record 0 "Cycle 8 stdlib + tools regressions (-O0)" ""
else
    aud_r8_record 1 "Cycle 8 stdlib + tools regressions (-O0)" "$(cat "$aud_dir/r8_stdlib_tools.diag" 2>/dev/null)"
fi

# 4 & 5. Core parity suite across compiled binary and bytecode VM
if aud_r8_compile_run "-O0" "$aud_src/parity_core.strada" "r8_parity_core" "R8P08B:"; then
    "$aud_dir/r8_parity_core" > "$aud_dir/parity_comp.out" 2>&1
    vm_rc=1
    vm_out=$(cd "$PROJECT_DIR" && timeout 45 "$PROJECT_DIR/interpreter/strada-interp" "$aud_src/parity_core.strada" 2>&1) && vm_rc=0 || vm_rc=$?
    printf '%s\n' "$vm_out" > "$aud_dir/parity_vm.out"

    if [ "$vm_rc" -eq 0 ] && diff -u "$aud_dir/parity_comp.out" "$aud_dir/parity_vm.out" > "$aud_dir/parity_vm.diff" 2>&1; then
        aud_r8_record 0 "Cycle 8 compiled vs bytecode VM parity (exact stdout diff)" ""
    else
        aud_r8_record 1 "Cycle 8 compiled vs bytecode VM parity" "rc=$vm_rc diff=$(cat "$aud_dir/parity_vm.diff" 2>/dev/null)"
    fi
else
    aud_r8_record 1 "Cycle 8 parity_core compiled vs bytecode VM" "$(cat "$aud_dir/r8_parity_core.diag" 2>/dev/null)"
fi

# 6. Eval::eval_string / eval_string_vm + Strada::JIT suite (R8-VM9..R8-VM11)
if aud_r8_compile_run "-O0" "$aud_src/eval_jit.strada" "r8_eval_jit" "R8_EVAL_JIT_OK"; then
    aud_r8_record 0 "Cycle 8 Eval + Strada::JIT suite (R8-VM9..11)" ""
else
    aud_r8_record 1 "Cycle 8 Eval + Strada::JIT suite (R8-VM9..11)" "$(cat "$aud_dir/r8_eval_jit.diag" 2>/dev/null)"
fi

# 7. R8-VM6: -e mixed our / func / use + top-level statements in VM
e_prog='use lib "lib"; use List::Util; our int $e_mul = 3; func e_fn(int $x) int { return $x * $e_mul; } my int $s = List::Util::sum([3, 4]); say("E_OK:" . e_fn(14) . ":" . $s);'
e_vm_out=$(cd "$PROJECT_DIR" && timeout 15 "$PROJECT_DIR/interpreter/strada-interp" -e "$e_prog" 2>&1) && e_vm_rc=0 || e_vm_rc=$?
if [ "$e_vm_rc" -eq 0 ] && [ "$e_vm_out" = "E_OK:42:7" ]; then
    aud_r8_record 0 "R8-VM6 -e mixed our/func/use + statements (VM)" ""
else
    aud_r8_record 1 "R8-VM6 -e mixed our/func/use + statements" "vm=($e_vm_rc:$e_vm_out)"
fi

# 8. R8-C11: tools/stradapp __C__ #ifdef unbalanced brace, trailing code after }, heredocs, #error/#warning
cat > "$aud_dir/pp_r8.strada" <<'EOF'
#define R8_AFTER_DEF 9
func main() int {
    my int $cval = 0;
#if 0
    __C__ {
#ifdef R8_NONEXISTENT_FLAG
        if (1) {
#else
        if (1) {
#endif
            THIS_IS_INACTIVE_@@@;
        }
    }
#endif
    __C__ {
        cval = strada_new_int(33);
    } my int $after_c = 0;
#if R8_AFTER_DEF == 9
    $after_c = 9;
#endif
    my str $both = <<EOF1 . <<EOF2;
line1
  EOF1
#ifdef R8_NOT_A_DIRECTIVE_IN_HEREDOC
EOF1
line2
EOF2
    if ($cval == 33 && $after_c == 9 && index($both, "  EOF1\n#ifdef R8_NOT_A_DIRECTIVE_IN_HEREDOC\nline2\n") >= 0) {
        say("PP_R8_OK");
        return 0;
    }
    say("PP_R8_FAIL:" . $both);
    return 1;
}
EOF
cat > "$aud_dir/pp_r8_err.strada" <<'EOF'
#error r8_explicit_pp_error
func main() int { return 0; }
EOF
cat > "$aud_dir/pp_r8_warn.strada" <<'EOF'
#warning r8_explicit_pp_warning
func main() int { say("WARN_OK"); return 0; }
EOF
pp_ok=1
pp_diag=""
if "$PROJECT_DIR/tools/stradapp" "$aud_dir/pp_r8.strada" -o "$aud_dir/pp_r8.out.strada" >"$aud_dir/pp_r8.log" 2>&1; then
    if aud_r8_compile_run "-O0" "$aud_dir/pp_r8.out.strada" "pp_r8_bin" "PP_R8_OK"; then
        "$PROJECT_DIR/tools/stradapp" "$aud_dir/pp_r8_err.strada" -o "$aud_dir/pp_r8_err.out.strada" >"$aud_dir/pp_r8_err.log" 2>&1
        err_rc=$?
        warn_out=$("$PROJECT_DIR/tools/stradapp" "$aud_dir/pp_r8_warn.strada" -o "$aud_dir/pp_r8_warn.out.strada" 2>&1)
        warn_rc=$?
        if [ "$err_rc" -ne 0 ] && [ "$warn_rc" -eq 0 ] && echo "$warn_out" | grep -q "r8_explicit_pp_warning"; then
            pp_ok=0
        else
            pp_diag="err_rc=$err_rc warn_rc=$warn_rc warn_out=$warn_out err_log=$(cat "$aud_dir/pp_r8_err.log" 2>/dev/null)"
        fi
    else
        pp_diag="$(cat "$aud_dir/pp_r8_bin.diag" 2>/dev/null)"
    fi
else
    pp_diag="stradapp failed: $(cat "$aud_dir/pp_r8.log" 2>/dev/null)"
fi
aud_r8_record "$pp_ok" "R8-C11 tools/stradapp __C__ #ifdef braces, trailing stmt, heredocs, #error/#warning" "$pp_diag"

# 9. R8-C12: bootstrap/stradac optional defaults, try return/last, compound +=/.=, push/unshift, \x41
cat > "$aud_dir/boot_r8.strada" <<'EOF'
func opt_fn(int $z = 0, int $big = 4611686018427387909, str $q = "a\"b\\c") str {
    return $z . ":" . $big . ":" . $q;
}
func try_ret_fn() int {
    try {
        return 17;
    } catch ($e) {
        return 99;
    }
    return 0;
}
func try_last_fn() int {
    my int $i = 0;
    my int $acc = 0;
    while ($i < 4) {
        $i = $i + 1;
        try {
            if ($i == 2) { last; }
            $acc = $acc + $i;
        } catch ($e) {}
    }
    return $acc;
}
func main() int {
    my int $x = 10;
    $x += 5;
    my str $s = "hi";
    $s .= "!";
    my array @a = ();
    push(@a, "b", "c");
    unshift(@a, "0", "1");
    my str $hx = "\x41\x42";
    my str $res = opt_fn() . "|" . try_ret_fn() . "|" . try_last_fn() . "|" . $x . "|" . $s . "|" . join(",", @a) . "|" . $hx;
    say($res);
    return 0;
}
EOF
boot_ok=1
boot_diag=""
if (cd "$PROJECT_DIR" && "$PROJECT_DIR/bootstrap/stradac" "$aud_dir/boot_r8.strada" "$aud_dir/boot_r8.c" >"$aud_dir/boot_r8.log" 2>&1); then
    if (cd "$PROJECT_DIR" && $GCC -I"$PROJECT_DIR/runtime" "$aud_dir/boot_r8.c" "$RUNTIME" $EXTRA_LDFLAGS -ldl -lpthread -lm -o "$aud_dir/boot_r8_bin" >"$aud_dir/boot_r8_cc.log" 2>&1); then
        boot_out=$(timeout 15 "$aud_dir/boot_r8_bin" 2>&1)
        boot_rc=$?
        if [ "$boot_rc" -eq 0 ] && [ "$boot_out" = '0:4611686018427387909:a"b\c|17|1|15|hi!|0,1,b,c|AB' ]; then
            boot_ok=0
        else
            boot_diag="rc=$boot_rc out=$boot_out"
        fi
    else
        boot_diag="gcc failed: $(cat "$aud_dir/boot_r8_cc.log" 2>/dev/null)"
    fi
else
    boot_diag="bootstrap/stradac failed: $(cat "$aud_dir/boot_r8.log" 2>/dev/null)"
fi
aud_r8_record "$boot_ok" "R8-C12 bootstrap/stradac defaults, try return/last, +=/.=, multi-push/unshift, \\x41" "$boot_diag"

# 10. R8-RT7: -DSTRADA_NO_TLS compiles runtime cleanly
notls_out=$($GCC -DSTRADA_NO_TLS -fsyntax-only -I"$PROJECT_DIR/runtime" "$PROJECT_DIR/runtime/strada_runtime.c" 2>&1) && notls_rc=0 || notls_rc=$?
if [ "$notls_rc" -eq 0 ]; then
    aud_r8_record 0 "R8-RT7 strada_runtime.c compiles under -DSTRADA_NO_TLS" ""
else
    aud_r8_record 1 "R8-RT7 strada_runtime.c -DSTRADA_NO_TLS" "$notls_out"
fi

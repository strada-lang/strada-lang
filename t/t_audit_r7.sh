#!/bin/bash
# t_audit_r7.sh — Cycle 7 Audit regression suite

aud_dir="$BUILD_DIR/audit_r7"
aud_src="$SCRIPT_DIR/audit_r7"
mkdir -p "$aud_dir"

aud_r7_record() {
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

aud_r7_compile_run() {
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

# 1. Runtime & Compiler Cycle 7 suite (-O0)
if aud_r7_compile_run "-O0" "$aud_src/runtime_compiler.strada" "r7_runtime_compiler_o0" "R7_RUNTIME_COMPILER_OK" -lpthread; then
    aud_r7_record 0 "Cycle 7 runtime + compiler regressions (-O0)" ""
else
    aud_r7_record 1 "Cycle 7 runtime + compiler regressions (-O0)" "$(cat "$aud_dir/r7_runtime_compiler_o0.diag" 2>/dev/null)"
fi

# 2. Runtime & Compiler Cycle 7 suite (-O2)
if aud_r7_compile_run "-O2" "$aud_src/runtime_compiler.strada" "r7_runtime_compiler_o2" "R7_RUNTIME_COMPILER_OK" -lpthread; then
    aud_r7_record 0 "Cycle 7 runtime + compiler regressions (-O2)" ""
else
    aud_r7_record 1 "Cycle 7 runtime + compiler regressions (-O2)" "$(cat "$aud_dir/r7_runtime_compiler_o2.diag" 2>/dev/null)"
fi

# 3. Stdlib & Tools Cycle 7 suite (-O0)
if aud_r7_compile_run "-O0" "$aud_src/stdlib_tools.strada" "r7_stdlib_tools" "R7_STDLIB_TOOLS_OK" -lssl -lcrypto; then
    aud_r7_record 0 "Cycle 7 stdlib + tools regressions (-O0)" ""
else
    aud_r7_record 1 "Cycle 7 stdlib + tools regressions (-O0)" "$(cat "$aud_dir/r7_stdlib_tools.diag" 2>/dev/null)"
fi

# 4 & 5. Core parity suite across compiled binary, tree-walk, and bytecode VM
if aud_r7_compile_run "-O0" "$aud_src/parity_core.strada" "r7_parity_core" "R7_PARITY_OK"; then
    "$aud_dir/r7_parity_core" > "$aud_dir/parity_comp.out" 2>&1
    tw_rc=1
    tw_out=$(cd "$PROJECT_DIR" && timeout 45 "$PROJECT_DIR/interpreter/strada-interp" --tree-walk "$aud_src/parity_core.strada" 2>&1) && tw_rc=0 || tw_rc=$?
    printf '%s\n' "$tw_out" > "$aud_dir/parity_tw.out"
    vm_rc=1
    vm_out=$(cd "$PROJECT_DIR" && timeout 45 "$PROJECT_DIR/interpreter/strada-interp" --vm "$aud_src/parity_core.strada" 2>&1) && vm_rc=0 || vm_rc=$?
    printf '%s\n' "$vm_out" > "$aud_dir/parity_vm.out"

    if [ "$tw_rc" -eq 0 ] && diff -u "$aud_dir/parity_comp.out" "$aud_dir/parity_tw.out" > "$aud_dir/parity_tw.diff" 2>&1; then
        aud_r7_record 0 "Cycle 7 compiled vs tree-walk parity (exact stdout diff)" ""
    else
        aud_r7_record 1 "Cycle 7 compiled vs tree-walk parity" "rc=$tw_rc diff=$(cat "$aud_dir/parity_tw.diff" 2>/dev/null)"
    fi

    if [ "$vm_rc" -eq 0 ] && diff -u "$aud_dir/parity_comp.out" "$aud_dir/parity_vm.out" > "$aud_dir/parity_vm.diff" 2>&1; then
        aud_r7_record 0 "Cycle 7 compiled vs bytecode --vm parity (exact stdout diff)" ""
    else
        aud_r7_record 1 "Cycle 7 compiled vs bytecode --vm parity" "rc=$vm_rc diff=$(cat "$aud_dir/parity_vm.diff" 2>/dev/null)"
    fi
else
    aud_r7_record 1 "Cycle 7 parity_core compiled vs tree-walk" "$(cat "$aud_dir/r7_parity_core.diag" 2>/dev/null)"
    aud_r7_record 1 "Cycle 7 parity_core compiled vs bytecode --vm" "skipped due to compiled failure"
fi

# 6. Eval::eval_string / eval_string_vm + Strada::JIT suite
if aud_r7_compile_run "-O0" "$aud_src/eval_jit.strada" "r7_eval_jit" "R7_EVAL_JIT_OK" \
        --no-use-artifacts -D HAVE_VM -I "$PROJECT_DIR/interpreter" \
        "$PROJECT_DIR/interpreter/vm.c" "$PROJECT_DIR/interpreter/vm_compiler.c"; then
    aud_r7_record 0 "Cycle 7 Eval + eval_string_vm + Strada::JIT suite" ""
else
    aud_r7_record 1 "Cycle 7 Eval + eval_string_vm + Strada::JIT suite" "$(cat "$aud_dir/r7_eval_jit.diag" 2>/dev/null)"
fi

# 7. Bug #21 tools/stradapp: # and /* in regexes, plus __C__ inside #if 0
cat > "$aud_dir/pp_r7.strada" <<'EOF'
func main() int {
    my int $m1 = ("a#b" =~ /a#b/) ? 1 : 0;
    my int $m2 = ("a/*b" =~ /a\/\*b/) ? 1 : 0;
    my int $m3 = ("x*/y" =~ /x\*\/y/) ? 1 : 0;
#if 0
    __C__ {
        THIS_DOES_NOT_COMPILE @@@
    }
#endif
    if ($m1 == 1 && $m2 == 1 && $m3 == 1) {
        say("PP_R7_OK");
        return 0;
    }
    say("PP_R7_FAIL:" . $m1 . "," . $m2 . "," . $m3);
    return 1;
}
EOF
pp_ok=1
pp_diag=""
if "$PROJECT_DIR/tools/stradapp" "$aud_dir/pp_r7.strada" -o "$aud_dir/pp_r7.out.strada" >"$aud_dir/pp_r7.log" 2>&1; then
    if aud_r7_compile_run "-O0" "$aud_dir/pp_r7.out.strada" "pp_r7_bin" "PP_R7_OK"; then
        pp_ok=0
    else
        pp_diag="$(cat "$aud_dir/pp_r7_bin.diag" 2>/dev/null)"
    fi
else
    pp_diag="stradapp failed: $(cat "$aud_dir/pp_r7.log" 2>/dev/null)"
fi
aud_r7_record "$pp_ok" "Bug #21 tools/stradapp regex #/* and #if 0 __C__" "$pp_diag"

# 8. Bugs #38-#40 strada-interp --vm -e die & non-zero exit code
vm_die_out=$(cd "$PROJECT_DIR" && timeout 15 "$PROJECT_DIR/interpreter/strada-interp" --vm -e 'die("boom_r7");' 2>&1)
vm_die_rc=$?
vm_ret_out=$(cd "$PROJECT_DIR" && timeout 15 "$PROJECT_DIR/interpreter/strada-interp" --vm -e 'func main() int { return 7; }' 2>&1)
vm_ret_rc=$?
if [ "$vm_die_rc" -ne 0 ] && [ "$vm_die_rc" -lt 128 ] && echo "$vm_die_out" | grep -q "boom_r7" \
    && [ "$vm_ret_rc" -eq 7 ]; then
    aud_r7_record 0 "Bugs #38-#40 strada-interp --vm -e die & main return code" ""
else
    aud_r7_record 1 "Bugs #38-#40 strada-interp --vm -e die & main return code" \
        "die_rc=$vm_die_rc die_out=$vm_die_out ret_rc=$vm_ret_rc ret_out=$vm_ret_out"
fi

# 9. Bug #46 tools/strada-proftext on valid STFP and truncated STFP
cat > "$aud_dir/prof_target.strada" <<'EOF'
func helper_p(int $n) int {
    return $n * 2;
}
func main() int {
    my int $s = helper_p(21);
    say("PROF_TARGET=" . $s);
    return 0;
}
EOF
prof_ok=1
prof_diag=""
if (cd "$PROJECT_DIR" && "$PROJECT_DIR/strada" -O0 --full-profile -L "$PROJECT_DIR/lib" "$aud_dir/prof_target.strada" -o "$aud_dir/prof_target" >"$aud_dir/prof_compile.log" 2>&1); then
    (cd "$aud_dir" && timeout 15 "$aud_dir/prof_target" >"$aud_dir/prof_run.log" 2>&1)
    prof_run_rc=$?
    prof_out=""
    prof_rc=1
    if [ "$prof_run_rc" -eq 0 ] && [ -f "$aud_dir/strada-prof.out" ]; then
        prof_out=$(timeout 15 "$PROJECT_DIR/tools/strada-proftext" "$aud_dir/strada-prof.out" 2>&1) && prof_rc=0 || prof_rc=$?
    fi
    printf 'STFP\x01\x00\x00\x00\xff\xff\xff\x7f' > "$aud_dir/trunc.prof"
    trunc_out=$(timeout 15 "$PROJECT_DIR/tools/strada-proftext" "$aud_dir/trunc.prof" 2>&1)
    trunc_rc=$?
    if [ "$prof_run_rc" -eq 0 ] && [ "$prof_rc" -eq 0 ] && [ "$trunc_rc" -ne 0 ] && [ "$trunc_rc" -lt 128 ]; then
        prof_ok=0
    else
        prof_diag="run_rc=$prof_run_rc prof_rc=$prof_rc trunc_rc=$trunc_rc prof_out=$prof_out trunc_out=$trunc_out"
    fi
else
    prof_diag="compile failed: $(cat "$aud_dir/prof_compile.log" 2>/dev/null)"
fi
aud_r7_record "$prof_ok" "Bug #46 tools/strada-proftext STFP valid + truncated bounds guard" "$prof_diag"

# 10. Bug #47 tools/strada-md2html directory with dots keeps <title>guide</title>
mkdir -p "$aud_dir/dir.with.dots"
cat > "$aud_dir/dir.with.dots/guide.md" <<'EOF'
Paragraph text without an H1 so fallback basename title is used.
EOF
md_rc=1
md_out=""
if md_out=$(timeout 15 "$PROJECT_DIR/tools/strada-md2html" "$aud_dir/dir.with.dots/guide.md" "$aud_dir/guide.html" 2>&1); then
    md_rc=0
else
    md_rc=$?
fi
if [ "$md_rc" -eq 0 ] && [ -f "$aud_dir/guide.html" ] && grep -q '<title>guide</title>' "$aud_dir/guide.html"; then
    aud_r7_record 0 "Bug #47 tools/strada-md2html basename title with dotted parent dir" ""
else
    aud_r7_record 1 "Bug #47 tools/strada-md2html basename title with dotted parent dir" "rc=$md_rc out=$md_out html=$(cat "$aud_dir/guide.html" 2>/dev/null)"
fi

# 11. Bug #48 lib/usb/strada_usb.c string descriptor bounds guard
if grep -q '^HAVE_LIBUSB = 1' "$PROJECT_DIR/config.mk" 2>/dev/null; then
    cat > "$aud_dir/usb_r7.strada" <<'EOF'
__C__ {
#include <stdlib.h>
#include <string.h>
extern StradaValue* strada_usb_get_string_descriptor(StradaValue *handle_sv, StradaValue *index_sv);
extern char* strada_usb_get_string_descriptor_raw(void *handle, int idx);
}
func test_usb() int {
    my int $ok = 1;
    __C__ {
        StradaValue *h = strada_new_int(1);
        StradaValue *i_neg = strada_new_int(-5);
        StradaValue *i_zero = strada_new_int(0);
        StradaValue *i_big = strada_new_int(300);
        StradaValue *r1 = strada_usb_get_string_descriptor(h, i_neg);
        StradaValue *r2 = strada_usb_get_string_descriptor(h, i_zero);
        StradaValue *r3 = strada_usb_get_string_descriptor(h, i_big);
        if (!r1 || STRADA_IS_TAGGED_INT(r1) || r1->type != STRADA_STR || !r1->value.pv || strcmp(r1->value.pv, "") != 0 ||
            !r2 || STRADA_IS_TAGGED_INT(r2) || r2->type != STRADA_STR || !r2->value.pv || strcmp(r2->value.pv, "") != 0 ||
            !r3 || STRADA_IS_TAGGED_INT(r3) || r3->type != STRADA_STR || !r3->value.pv || strcmp(r3->value.pv, "") != 0) {
            ok = strada_new_int(0);
        }
        strada_decref(h);
        strada_decref(i_neg);
        strada_decref(i_zero);
        strada_decref(i_big);
        strada_decref(r1);
        strada_decref(r2);
        strada_decref(r3);

        char *c1 = strada_usb_get_string_descriptor_raw((void*)1, -5);
        char *c2 = strada_usb_get_string_descriptor_raw((void*)1, 0);
        char *c3 = strada_usb_get_string_descriptor_raw((void*)1, 300);
        if (!c1 || strcmp(c1, "") != 0 ||
            !c2 || strcmp(c2, "") != 0 ||
            !c3 || strcmp(c3, "") != 0) {
            ok = strada_new_int(0);
        }
        free(c1);
        free(c2);
        free(c3);
    }
    return $ok;
}
func main() int {
    if (test_usb() == 1) {
        say("USB_R7_OK");
        return 0;
    }
    return 1;
}
EOF
    if aud_r7_compile_run "-O0" "$aud_dir/usb_r7.strada" "usb_r7" "USB_R7_OK" \
            "$PROJECT_DIR/lib/usb/strada_usb.c" -I/usr/include/libusb-1.0 -lusb-1.0; then
        aud_r7_record 0 "Bug #48 lib/usb/strada_usb.c string descriptor bounds guard" ""
    else
        aud_r7_record 1 "Bug #48 lib/usb/strada_usb.c string descriptor bounds guard" "$(cat "$aud_dir/usb_r7.diag" 2>/dev/null)"
    fi
else
    test_skip "Bug #48 lib/usb/strada_usb.c string descriptor bounds guard" "HAVE_LIBUSB != 1"
fi

# 12. Bug #49 tools/strada2perl qualifies ::func() after operators on same line
cat > "$aud_dir/s2p_r7.strada" <<'EOF'
func main() int {
    my int $x = ::foo(1) + ::bar(2);
    my str $y = $a . ::baz();
    return 0;
}
EOF
s2p_bin="$PROJECT_DIR/tools/strada2perl"
if [ ! -x "$s2p_bin" ]; then
    s2p_bin="$aud_dir/strada2perl"
    (cd "$PROJECT_DIR" && "$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" tools/strada2perl.strada -o "$s2p_bin" >"$aud_dir/s2p_build.log" 2>&1) || true
fi
s2p_ok=1
s2p_diag=""
if [ -x "$s2p_bin" ] && "$s2p_bin" "$aud_dir/s2p_r7.strada" "$aud_dir/s2p.pl" >"$aud_dir/s2p.log" 2>&1; then
    s2p_txt=$(cat "$aud_dir/s2p.pl" 2>/dev/null || true)
    if echo "$s2p_txt" | grep -q 'my \$x = foo(1) + bar(2);' \
        && echo "$s2p_txt" | grep -q 'my \$y = \$a \. baz();' \
        && ! echo "$s2p_txt" | grep -qE '::(foo|bar|baz)\('; then
        s2p_ok=0
    else
        s2p_diag="unexpected output: $s2p_txt"
    fi
else
    s2p_diag="strada2perl failed: $(cat "$aud_dir/s2p.log" 2>/dev/null)"
fi
aud_r7_record "$s2p_ok" "Bug #49 tools/strada2perl qualifies ::func() after operators" "$s2p_diag"

#!/bin/bash
# t_audit_r9.sh — Cycle 9 Audit regression suite

aud_dir="$BUILD_DIR/audit_r9"
aud_src="$SCRIPT_DIR/audit_r9"
mkdir -p "$aud_dir"

aud_r9_record() {
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

aud_r9_compile_run() {
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

# 1. Runtime & Compiler Cycle 9 suite (-O0)
if aud_r9_compile_run "-O0" "$aud_src/runtime_compiler.strada" "r9_runtime_compiler_o0" "R9_RUNTIME_COMPILER_OK" -lpthread; then
    aud_r9_record 0 "Cycle 9 Runtime & Compiler suite (-O0)" ""
else
    aud_r9_record 1 "Cycle 9 Runtime & Compiler suite (-O0)" "$(cat "$aud_dir/r9_runtime_compiler_o0.diag" 2>/dev/null)"
fi

# 2. Runtime & Compiler Cycle 9 suite (-O2)
if aud_r9_compile_run "-O2" "$aud_src/runtime_compiler.strada" "r9_runtime_compiler_o2" "R9_RUNTIME_COMPILER_OK" -lpthread; then
    aud_r9_record 0 "Cycle 9 Runtime & Compiler suite (-O2)" ""
else
    aud_r9_record 1 "Cycle 9 Runtime & Compiler suite (-O2)" "$(cat "$aud_dir/r9_runtime_compiler_o2.diag" 2>/dev/null)"
fi

# 3. Standard Library Cycle 9 suite (-O0)
if aud_r9_compile_run "-O0" "$aud_src/stdlib_tools.strada" "r9_stdlib_tools" "R9_STDLIB_OK"; then
    aud_r9_record 0 "Cycle 9 Stdlib suite (-O0)" ""
else
    aud_r9_record 1 "Cycle 9 Stdlib suite (-O0)" "$(cat "$aud_dir/r9_stdlib_tools.diag" 2>/dev/null)"
fi

# 4. 3-way compiled / --tree-walk / --vm parity (R9-VM1..5, R9-TW1)
parity_src="$PROJECT_DIR/interpreter/test/test_audit_r9_vm.strada"
parity_exp="$PROJECT_DIR/interpreter/test/test_audit_r9_vm.expected"
if (cd "$PROJECT_DIR" && "$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$parity_src" -lpthread -o "$aud_dir/r9_parity_comp" >"$aud_dir/r9_parity_comp.log" 2>&1); then
    (cd "$PROJECT_DIR" && timeout 20 "$aud_dir/r9_parity_comp" >"$aud_dir/parity_comp.out" 2>&1) && comp_rc=0 || comp_rc=$?
else
    comp_rc=1
    cat "$aud_dir/r9_parity_comp.log" > "$aud_dir/parity_comp.out" 2>/dev/null || true
fi

if [ "$comp_rc" -eq 0 ] && diff -u "$parity_exp" "$aud_dir/parity_comp.out" > "$aud_dir/parity_exp.diff" 2>&1; then
    (cd "$PROJECT_DIR" && timeout 20 "$PROJECT_DIR/interpreter/strada-interp" --tree-walk "$parity_src" >"$aud_dir/parity_tw.out" 2>&1) && tw_rc=0 || tw_rc=$?
    if [ "$tw_rc" -eq 0 ] && diff -u "$aud_dir/parity_comp.out" "$aud_dir/parity_tw.out" > "$aud_dir/parity_tw.diff" 2>&1; then
        aud_r9_record 0 "Cycle 9 compiled vs tree-walk parity (exact stdout diff)" ""
    else
        aud_r9_record 1 "Cycle 9 compiled vs tree-walk parity" "rc=$tw_rc diff=$(cat "$aud_dir/parity_tw.diff" 2>/dev/null)"
    fi

    (cd "$PROJECT_DIR" && timeout 20 "$PROJECT_DIR/interpreter/strada-interp" --vm "$parity_src" >"$aud_dir/parity_vm.out" 2>&1) && vm_rc=0 || vm_rc=$?
    if [ "$vm_rc" -eq 0 ] && diff -u "$aud_dir/parity_comp.out" "$aud_dir/parity_vm.out" > "$aud_dir/parity_vm.diff" 2>&1; then
        aud_r9_record 0 "Cycle 9 compiled vs bytecode --vm parity (exact stdout diff)" ""
    else
        aud_r9_record 1 "Cycle 9 compiled vs bytecode --vm parity" "rc=$vm_rc diff=$(cat "$aud_dir/parity_vm.diff" 2>/dev/null)"
    fi
else
    aud_r9_record 1 "Cycle 9 parity compiled vs expected (--tree-walk)" "rc=$comp_rc out=$(cat "$aud_dir/parity_comp.out" 2>/dev/null) diff=$(cat "$aud_dir/parity_exp.diff" 2>/dev/null)"
    aud_r9_record 1 "Cycle 9 parity compiled vs bytecode --vm" "skipped due to compiled failure"
fi

# 5. Eval + Strada::JIT suite (R9-EVAL1, R9-JIT1, R9-JIT2)
if aud_r9_compile_run "-O0" "$aud_src/eval_jit.strada" "r9_eval_jit" "R9_EVAL_JIT_OK" \
        --no-use-artifacts -D HAVE_VM -I "$PROJECT_DIR/interpreter" \
        "$PROJECT_DIR/interpreter/vm.c" "$PROJECT_DIR/interpreter/vm_compiler.c"; then
    aud_r9_record 0 "Cycle 9 Eval + Strada::JIT suite (R9-EVAL1, R9-JIT1..2)" ""
else
    aud_r9_record 1 "Cycle 9 Eval + Strada::JIT suite (R9-EVAL1, R9-JIT1..2)" "$(cat "$aud_dir/r9_eval_jit.diag" 2>/dev/null)"
fi

# 6. R9-CLI1 & R9-VM3: -e with @ARGV and runtime throw + LTR arg side effect on undefined call
cli_prog='our str $log = ""; func bump() int { $log = $log . "side"; return 1; } my str $caught = ""; try { r9_no_such_fn(bump()); } catch ($e) { $caught = $e; } say($ARGV[0] . ":" . $ARGV[1] . ":" . $log . ":" . (index($caught, "Undefined") >= 0 ? "1" : "0"));'
cli_tw_out=$(cd "$PROJECT_DIR" && timeout 15 "$PROJECT_DIR/interpreter/strada-interp" --tree-walk -e "$cli_prog" alpha beta 2>&1) && cli_tw_rc=0 || cli_tw_rc=$?
cli_vm_out=$(cd "$PROJECT_DIR" && timeout 15 "$PROJECT_DIR/interpreter/strada-interp" --vm -e "$cli_prog" alpha beta 2>&1) && cli_vm_rc=0 || cli_vm_rc=$?
if [ "$cli_tw_rc" -eq 0 ] && [ "$cli_vm_rc" -eq 0 ] \
    && [ "$cli_tw_out" = "alpha:beta:side:1" ] && [ "$cli_vm_out" = "alpha:beta:side:1" ]; then
    aud_r9_record 0 "R9-CLI1 & R9-VM3 -e @ARGV + undefined call throws with LTR side effect" ""
else
    aud_r9_record 1 "R9-CLI1 & R9-VM3 -e @ARGV + undefined call throw" "tw=($cli_tw_rc:$cli_tw_out) vm=($cli_vm_rc:$cli_vm_out)"
fi

# 6b. R9-TW1: --tree-walk vs --vm parity on bare hex(), oct(), and bare abs()
tw1_prog='say(hex(" 0x1_f ") . ":" . oct(" 0b101_0 ") . ":" . oct(" 0o7_1 ") . ":" . oct(" 0x10_1 ") . ":" . oct("07_7") . ":" . sprintf("%.1f", abs(" -42.5 ")) . ":" . abs(-9223372036854775807 - 1));'
tw1_tw_out=$(cd "$PROJECT_DIR" && timeout 15 "$PROJECT_DIR/interpreter/strada-interp" --tree-walk -e "$tw1_prog" 2>&1) && tw1_tw_rc=0 || tw1_tw_rc=$?
tw1_vm_out=$(cd "$PROJECT_DIR" && timeout 15 "$PROJECT_DIR/interpreter/strada-interp" --vm -e "$tw1_prog" 2>&1) && tw1_vm_rc=0 || tw1_vm_rc=$?
if [ "$tw1_tw_rc" -eq 0 ] && [ "$tw1_vm_rc" -eq 0 ] \
    && [ "$tw1_tw_out" = "31:10:57:257:63:42.5:9223372036854775808" ] \
    && [ "$tw1_vm_out" = "31:10:57:257:63:42.5:9223372036854775808" ]; then
    aud_r9_record 0 "R9-TW1 & R9-RT3 --tree-walk vs --vm parity for bare hex/oct/abs" ""
else
    aud_r9_record 1 "R9-TW1 & R9-RT3 --tree-walk vs --vm bare hex/oct/abs" "tw=($tw1_tw_rc:$tw1_tw_out) vm=($tw1_vm_rc:$tw1_vm_out)"
fi

# 7. R9-LIB3: lib/perl5 tied scalar + throwing "" overload inside try/catch
p5_ccopts=$(perl -MExtUtils::Embed -e ccopts 2>/dev/null || true)
p5_ldopts=$(perl -MExtUtils::Embed -e ldopts 2>/dev/null || true)
if [ -n "$p5_ccopts" ] && gcc -c -Wall -Wextra -fPIC -O2 -I"$PROJECT_DIR/runtime" $p5_ccopts -o "$aud_dir/strada_perl5.o" "$PROJECT_DIR/lib/perl5/strada_perl5.c" >"$aud_dir/p5_cc.log" 2>&1; then
    cat > "$aud_dir/r9_perl5_test.strada" <<'EOF'
__C__ {
extern int strada_perl5_init(void);
extern void strada_perl5_shutdown(void);
extern StradaValue* strada_perl5_eval_sv(StradaValue*);
extern StradaValue* strada_perl5_call_sv(StradaValue*, StradaValue*);
}

func p5_init() int {
    my int $r = 0;
    __C__ {
        strada_decref(r);
        r = strada_new_int(strada_perl5_init());
    }
    return $r;
}

func p5_shutdown() void {
    __C__ {
        strada_perl5_shutdown();
    }
}

func p5_eval(scalar $code) str {
    my str $res = "";
    __C__ {
        StradaValue *v = strada_perl5_eval_sv(code);
        strada_decref(res);
        res = v ? v : strada_new_str("");
    }
    return $res;
}

func p5_call(scalar $sub, scalar $args) str {
    my str $res = "";
    __C__ {
        StradaValue *v = strada_perl5_call_sv(sub, args);
        strada_decref(res);
        res = v ? v : strada_new_str("");
    }
    return $res;
}

package R9P5Tie;
func TIESCALAR(str $cls, str $v) scalar { my hash %s = ("v" => $v); return bless(\%s, $cls); }
func FETCH(scalar $self) scalar { return $self->{"v"}; }
func STORE(scalar $self, scalar $v) void { $self->{"v"} = $v; }

package R9P5Throw;
func new() scalar { my hash %s = ("x" => 1); return bless(\%s, "R9P5Throw"); }
func to_str(scalar $self, scalar $o, int $r) str { die("r9_p5_over_boom"); }
use overload '""' => "to_str";

package main;
func main() int {
    p5_init();
    my str $tv = "";
    tie($tv, "R9P5Tie", "tied_ok");
    p5_eval("sub r9_echo { return \"p5:\" . $_[0]; }");
    my str $r1 = p5_call("r9_echo", [$tv]);
    my int $caught = 0;
    try {
        p5_call("r9_echo", [R9P5Throw::new()]);
    } catch ($e) {
        $caught = 1;
    }
    my str $r2 = p5_call("r9_echo", ["after_throw"]);
    untie($tv);
    p5_shutdown();
    if ($r1 eq "p5:tied_ok" && $caught == 1 && $r2 eq "p5:after_throw") {
        say("R9_PERL5_OK");
        return 0;
    }
    say("R9_PERL5_FAIL:" . $r1 . "|" . $caught . "|" . $r2);
    return 1;
}
EOF
    if aud_r9_compile_run "-O0" "$aud_dir/r9_perl5_test.strada" "r9_perl5_bin" "R9_PERL5_OK" "$aud_dir/strada_perl5.o" $p5_ldopts; then
        aud_r9_record 0 "R9-LIB3 lib/perl5 tied scalar + throwing overload leaves Perl stack intact" ""
    else
        aud_r9_record 1 "R9-LIB3 lib/perl5 tied scalar + throwing overload" "$(cat "$aud_dir/r9_perl5_bin.diag" 2>/dev/null)"
    fi
elif [ -n "$p5_ccopts" ]; then
    aud_r9_record 1 "R9-LIB3 lib/perl5 tied scalar + throwing overload" "gcc strada_perl5.c failed: $(cat "$aud_dir/p5_cc.log" 2>/dev/null)"
else
    test_skip "R9-LIB3 lib/perl5 tied scalar + throwing overload" "ExtUtils::Embed unavailable"
fi

# 9. Tree-walk vs VM unqualified suffix \&fn parity (R9-TW2)
cat > "$aud_dir/r9_suf_test.strada" << 'EOF'
package R9SufPkg;
func r9_suf_triple(int $x) int { return $x * 3; }
package main;
func main() int {
    my scalar $f = \&r9_suf_triple;
    say($f->(4));
    return 0;
}
EOF
if [ -x "$PROJECT_DIR/interpreter/strada-interp" ]; then
    tw_suf=$("$PROJECT_DIR/interpreter/strada-interp" --tree-walk "$aud_dir/r9_suf_test.strada" 2>&1) || true
    vm_suf=$("$PROJECT_DIR/interpreter/strada-interp" --vm "$aud_dir/r9_suf_test.strada" 2>&1) || true
    if [ "$tw_suf" = "12" ] && [ "$vm_suf" = "12" ]; then
        aud_r9_record 0 "R9-TW2 --tree-walk vs --vm parity for unqualified suffix funcref" ""
    else
        aud_r9_record 1 "R9-TW2 --tree-walk vs --vm parity for unqualified suffix funcref" "tw=$tw_suf vm=$vm_suf"
    fi
fi

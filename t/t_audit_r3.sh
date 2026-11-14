#!/bin/bash
# t_audit_r3.sh — Cycle 3 Audit regression suite (Runtime/GC + Compiler/CodeGen + stradapp)

aud_dir="$BUILD_DIR/audit_r3"
aud_src="$SCRIPT_DIR/audit_r3"
mkdir -p "$aud_dir"

aud_r3_record() {
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

aud_r3_fast_compile() {
    local src="$1"
    local name="$2"
    (
        cd "$PROJECT_DIR" &&
        BUILD_DIR="$aud_dir" STRADA_TEST_TCC=1 compile_strada "$src" "$name"
    )
}

(
    rc=0
    aud_r3_fast_compile "$aud_src/runtime.strada" "r3_runtime" &&
    timeout 25 "$aud_dir/r3_runtime" > "$aud_dir/runtime.out" 2>&1 || rc=1
    echo $rc > "$aud_dir/runtime.rc"
) &
pid_rt=$!

(
    rc=0
    aud_r3_fast_compile "$aud_src/compiler.strada" "r3_compiler" &&
    timeout 20 "$aud_dir/r3_compiler" > "$aud_dir/compiler.out" 2>&1 || rc=1
    echo $rc > "$aud_dir/compiler.rc"
) &
pid_comp=$!

(
    rc=0
    "$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$aud_src/eval_jit.strada" -o "$aud_dir/r3_eval_jit" > "$aud_dir/eval_jit.build.log" 2>&1 &&
    timeout 30 "$aud_dir/r3_eval_jit" > "$aud_dir/eval_jit.out" 2>&1 || rc=1
    echo $rc > "$aud_dir/eval_jit.rc"
) &
pid_ej=$!

(
    rc=0
    "$PROJECT_DIR/strada" -O0 --no-use-artifacts -L "$PROJECT_DIR/lib" -D HAVE_VM -I "$PROJECT_DIR/interpreter" \
        "$aud_src/vm_eval_checks.strada" "$PROJECT_DIR/interpreter/vm.c" "$PROJECT_DIR/interpreter/vm_compiler.c" \
        -o "$aud_dir/r3_vm_eval" > "$aud_dir/vm_eval.build.log" 2>&1 &&
    timeout 30 "$aud_dir/r3_vm_eval" > "$aud_dir/vm_eval.out" 2>&1 || rc=1
    echo $rc > "$aud_dir/vm_eval.rc"
) &
pid_vme=$!

(
    rc=0
    "$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$aud_src/stdlib_tools.strada" \
        "$PROJECT_DIR/lib/Gettext/gettext_wrapper.c" -lsqlite3 -lcrypt -lpthread \
        -o "$aud_dir/r3_stdlib" > "$aud_dir/stdlib.build.log" 2>&1 &&
    timeout 25 "$aud_dir/r3_stdlib" > "$aud_dir/stdlib.out" 2>&1 || rc=1
    echo $rc > "$aud_dir/stdlib.rc"
) &
pid_stdlib=$!

wait "$pid_rt" "$pid_comp" "$pid_ej" "$pid_vme" "$pid_stdlib"

# 1. Runtime & Concurrency / GC fixes
rt_rc=$(cat "$aud_dir/runtime.rc" 2>/dev/null || echo 1)
rt_out=$(cat "$aud_dir/runtime.out" 2>/dev/null || true)
if [ "$rt_rc" -eq 0 ] && echo "$rt_out" | grep -q "R3_RUNTIME_OK"; then
    aud_r3_record 0 "runtime (C3): long tied/overload >256B, tied ++/--, local hash exists/undef, coro local, PerlaCell self-cycle" ""
else
    aud_r3_record 1 "runtime (C3): long tied/overload >256B, tied ++/--, local hash exists/undef, coro local, PerlaCell self-cycle" "rc=$rt_rc out=$rt_out"
fi

# 2. Compiler & CodeGen fixes
comp_rc=$(cat "$aud_dir/compiler.rc" 2>/dev/null || echo 1)
comp_out=$(cat "$aud_dir/compiler.out" 2>/dev/null || true)
if [ "$comp_rc" -eq 0 ] && echo "$comp_out" | grep -q "R3_COMPILER_OK"; then
    aud_r3_record 0 "compiler (H1-H8, M2-M6, L1): local shadow, %h=@a, do/map/grep local, for+redo, foreach/catch PerlaCell, has MRO/lazy/around, our pkg, autoviv, void exprs, regex/method/heredoc" ""
else
    aud_r3_record 1 "compiler (H1-H8, M2-M6, L1): compiler regressions" "rc=$comp_rc out=$comp_out"
fi

# 3. stradapp preprocessor fixes (M7)
cat > "$aud_dir/pp_ok.strada" <<'EOF'
#define YES 1
#ifdef YES // line comment
say("branch_yes");
#else /* block comment */
say("branch_no");
#endif /* end comment */
my str $hd = <<eof;
#ifdef NOT_A_DIRECTIVE
inside_lowercase_heredoc
eof
my int $sh = ($a << shift_amt) + 1;
EOF

pp_rc=0
pp_out=$("$PROJECT_DIR/tools/stradapp" "$aud_dir/pp_ok.strada" 2>&1) || pp_rc=$?
if [ "$pp_rc" -eq 0 ] \
    && echo "$pp_out" | grep -q 'branch_yes' \
    && ! echo "$pp_out" | grep -q 'branch_no' \
    && echo "$pp_out" | grep -q '#ifdef NOT_A_DIRECTIVE' \
    && echo "$pp_out" | grep -q 'inside_lowercase_heredoc'; then
    aud_r3_record 0 "stradapp (M7 ok): // and /* */ comments, lowercase heredoc, shift guard" ""
else
    aud_r3_record 1 "stradapp (M7 ok): // and /* */ comments, lowercase heredoc, shift guard" "rc=$pp_rc out=$pp_out"
fi

cat > "$aud_dir/pp_bad_else.strada" <<'EOF'
#ifdef FOO
say("a");
#else bogus_extra
say("b");
#endif
EOF
if "$PROJECT_DIR/tools/stradapp" "$aud_dir/pp_bad_else.strada" >/dev/null 2>&1; then
    aud_r3_record 1 "stradapp (M7 err): #else with trailing tokens must fail" "unexpected exit 0"
else
    aud_r3_record 0 "stradapp (M7 err): #else with trailing tokens rejects" ""
fi

cat > "$aud_dir/pp_bad_endif.strada" <<'EOF'
#ifdef FOO
say("a");
#endif bogus_extra
EOF
if "$PROJECT_DIR/tools/stradapp" "$aud_dir/pp_bad_endif.strada" >/dev/null 2>&1; then
    aud_r3_record 1 "stradapp (M7 err): #endif with trailing tokens must fail" "unexpected exit 0"
else
    aud_r3_record 0 "stradapp (M7 err): #endif with trailing tokens rejects" ""
fi

# 4. Eval & Strada::JIT fixes (VM-11..VM-18 tree-walk & JIT)
ej_rc=$(cat "$aud_dir/eval_jit.rc" 2>/dev/null || echo 1)
ej_out=$(cat "$aud_dir/eval_jit.out" 2>/dev/null || true)
if [ "$ej_rc" -eq 0 ] && echo "$ej_out" | grep -q "R3_EVAL_JIT_OK"; then
    aud_r3_record 0 "Eval & Strada::JIT (VM-11..VM-13, VM-16..VM-18)" ""
else
    aud_r3_record 1 "Eval & Strada::JIT (VM-11..VM-13, VM-16..VM-18)" "rc=$ej_rc out=$ej_out build=$(tail -n 10 "$aud_dir/eval_jit.build.log" 2>/dev/null)"
fi

# 5. Eval::eval_string_vm (HAVE_VM: VM-14, VM-15, VM-16)
vme_rc=$(cat "$aud_dir/vm_eval.rc" 2>/dev/null || echo 1)
vme_out=$(cat "$aud_dir/vm_eval.out" 2>/dev/null || true)
if [ "$vme_rc" -eq 0 ] && echo "$vme_out" | grep -q "R3_VM_EVAL_OK"; then
    aud_r3_record 0 "Eval::eval_string_vm (VM-14, VM-15, VM-16)" ""
else
    aud_r3_record 1 "Eval::eval_string_vm (VM-14, VM-15, VM-16)" "rc=$vme_rc out=$vme_out build=$(tail -n 10 "$aud_dir/vm_eval.build.log" 2>/dev/null)"
fi

# 6. VM CLI checks: VM-06 (BEGIN die non-zero) & VM-04 (END runs once after local unwind)
cat > "$aud_dir/vm_begin_die.strada" <<'EOF'
BEGIN { die("begin_boom"); }
END { say("end_after_begin"); }
func main() int {
    say("unreachable");
    return 0;
}
EOF
bd_rc=0
bd_out=$("$PROJECT_DIR/interpreter/strada-interp" --vm "$aud_dir/vm_begin_die.strada" 2>&1) || bd_rc=$?
bd_end_cnt=$(printf '%s\n' "$bd_out" | grep -c '^end_after_begin$' || true)
if [ "$bd_rc" -ne 0 ] && ! echo "$bd_out" | grep -q "unreachable" && [ "$bd_end_cnt" -le 1 ]; then
    aud_r3_record 0 "VM BEGIN die exits non-zero and skips main (VM-06)" ""
else
    aud_r3_record 1 "VM BEGIN die exits non-zero and skips main (VM-06)" "rc=$bd_rc end_cnt=$bd_end_cnt out=$bd_out"
fi

cat > "$aud_dir/vm_uncaught_end.strada" <<'EOF'
our int $g = 5;
END { say("end_g=" . $g); }
func f() void {
    local $g = 99;
    die("boom");
}
func main() int {
    f();
    return 0;
}
EOF
ue_rc=0
ue_out=$("$PROJECT_DIR/interpreter/strada-interp" --vm "$aud_dir/vm_uncaught_end.strada" 2>&1) || ue_rc=$?
ue_cnt=$(printf '%s\n' "$ue_out" | grep -c '^end_g=5$' || true)
if [ "$ue_rc" -ne 0 ] && [ "$ue_cnt" -eq 1 ] && ! echo "$ue_out" | grep -q "end_g=99"; then
    aud_r3_record 0 "VM uncaught exception unwinds local before END and runs END once (VM-04, VM-06)" ""
else
    aud_r3_record 1 "VM uncaught exception unwinds local before END and runs END once (VM-04, VM-06)" "rc=$ue_rc cnt=$ue_cnt out=$ue_out"
fi


# 7. Stdlib runtime behavior checks (Gettext, crypt, Forma, JSON::PS, Template, Net::SMTP*, DBI, LWP*, SerialPort, Async::Task, YAML, Nesso)
std_rc=$(cat "$aud_dir/stdlib.rc" 2>/dev/null || echo 1)
std_out=$(cat "$aud_dir/stdlib.out" 2>/dev/null || true)
if [ "$std_rc" -eq 0 ] && echo "$std_out" | grep -q "ALL_R3_STDLIB_OK"; then
    aud_r3_record 0 "Stdlib fixes (Gettext, crypt, Forma, JSON::PS, Template, SMTP, DBI, LWP*, SerialPort, Async::Task, YAML, Nesso)" ""
else
    aud_r3_record 1 "Stdlib fixes (C3)" "rc=$std_rc out=$std_out build=$(tail -n 10 "$aud_dir/stdlib.build.log" 2>/dev/null)"
fi

# 9. Strada::JIT::run_file_argv signal exit status (SIGKILL -> 137)
cat > "$aud_dir/sigkill_target.strada" <<'EOF'
func main() int {
    core::kill(core::getpid(), 9);
    return 0;
}
EOF
cat > "$aud_dir/jit_sig_runner.strada" <<EOF
use lib "$PROJECT_DIR/lib";
use Strada::JIT;
func main() int {
    Strada::JIT::init();
    my int \$rc = Strada::JIT::run_file_argv("$aud_dir/sigkill_target.strada", []);
    Strada::JIT::reset();
    say("jit_sig=" . \$rc);
    return (\$rc == 137) ? 0 : 1;
}
EOF
js_rc=1
if "$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$aud_dir/jit_sig_runner.strada" -o "$aud_dir/jit_sig_runner" > "$aud_dir/jit_sig.build.log" 2>&1; then
    js_out=$(cd "$PROJECT_DIR" && PATH="$PROJECT_DIR:$PATH" timeout 20 "$aud_dir/jit_sig_runner" 2>&1) && js_rc=0 || js_rc=$?
else
    js_out=$(cat "$aud_dir/jit_sig.build.log" 2>/dev/null || true)
fi
if [ "$js_rc" -eq 0 ] && echo "$js_out" | grep -q "jit_sig=137"; then
    aud_r3_record 0 "Strada::JIT::run_file_argv decodes signal exit status (SIGKILL -> 137)" ""
else
    aud_r3_record 1 "Strada::JIT::run_file_argv signal exit" "rc=$js_rc out=$js_out"
fi

# 10. strada-driver flag forwarding (-pthread, -Wl,-rpath, -isystem, -L/-l, -fwrapv, -pipe)
cat > "$aud_dir/r3tiny.c" <<'EOF'
int r3_tiny_val(void) { return 77; }
EOF
cat > "$aud_dir/drv_flags.strada" <<'EOF'
extern "C" { func r3_tiny_val() int; }
func main() int {
    my int $v = r3_tiny_val();
    say("tiny=" . $v);
    return ($v == 77) ? 0 : 1;
}
EOF
df_rc=1
df_out=""
if gcc -shared -fPIC "$aud_dir/r3tiny.c" -o "$aud_dir/libr3tiny.so" 2> "$aud_dir/drv_flags.build.log" \
    && "$PROJECT_DIR/strada" -pthread -Wl,-rpath,"$aud_dir" -isystem /usr/include -L"$aud_dir" -lr3tiny -fwrapv -pipe \
       "$aud_dir/drv_flags.strada" -o "$aud_dir/drv_flags" >> "$aud_dir/drv_flags.build.log" 2>&1; then
    df_out=$(timeout 10 "$aud_dir/drv_flags" 2>&1) && df_rc=0 || df_rc=$?
else
    df_out=$(cat "$aud_dir/drv_flags.build.log" 2>/dev/null || true)
fi
if [ "$df_rc" -eq 0 ] && echo "$df_out" | grep -q "tiny=77"; then
    aud_r3_record 0 "strada-driver forwards -pthread, -Wl,*, -isystem, -f*, -pipe, -L/-l" ""
else
    aud_r3_record 1 "strada-driver flag forwarding" "rc=$df_rc out=$df_out"
fi

# 11. lib/perl5 C-extension checks (when ExtUtils::Embed is present)
if perl -MExtUtils::Embed -e 1 >/dev/null 2>&1; then
    cat > "$aud_dir/p5_test.strada" <<EOF
use lib "$PROJECT_DIR/lib";
use perl5;
func main() int {
    if (perl5::init() != 1) { say("FAIL init"); return 1; }
    perl5::run('sub cnt { return scalar(@_); } sub echo_b { return \$_[0]; } sub boom { die "p5boom\\n"; }');
    if (perl5::call_noargs("cnt") ne "0") { say("FAIL call_noargs"); perl5::shutdown(); return 2; }
    my str \$bin = "a" . chr(0) . "b";
    perl5::set_var("bv", \$bin);
    my str \$gv = perl5::get_var("bv");
    if (core::byte_length(\$gv) != 3) { say("FAIL nul var blen=" . core::byte_length(\$gv)); perl5::shutdown(); return 3; }
    my str \$cb = perl5::call("echo_b", \$bin);
    if (core::byte_length(\$cb) != 3) { say("FAIL nul call blen=" . core::byte_length(\$cb)); perl5::shutdown(); return 4; }
    my str \$berr = perl5::call("boom", "");
    my str \$ge = perl5::get_error();
    if (index(\$berr . \$ge, "p5boom") < 0) { say("FAIL die capture"); perl5::shutdown(); return 5; }
    if (perl5::eval("2 + 3") ne "5") { say("FAIL eval after die"); perl5::shutdown(); return 6; }
    perl5::shutdown();
    say("P5_OK");
    return 0;
}
EOF
    p5_rc=1
    p5_flags=$(perl -MExtUtils::Embed -e ccopts -e ldopts)
    if make -C "$PROJECT_DIR/lib/perl5" > "$aud_dir/p5.build.log" 2>&1 \
        && "$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$aud_dir/p5_test.strada" $p5_flags -o "$aud_dir/p5_test" >> "$aud_dir/p5.build.log" 2>&1; then
        p5_out=$(timeout 15 "$aud_dir/p5_test" 2>&1) && p5_rc=0 || p5_rc=$?
    else
        p5_out=$(tail -n 10 "$aud_dir/p5.build.log" 2>/dev/null || true)
    fi
    make -C "$PROJECT_DIR/lib/perl5" clean >/dev/null 2>&1 || true
    if [ "$p5_rc" -eq 0 ] && echo "$p5_out" | grep -q "P5_OK"; then
        aud_r3_record 0 "lib/perl5: call_noargs 0 args, NUL-safe strings, die recovery" ""
    else
        aud_r3_record 1 "lib/perl5 C-extension" "rc=$p5_rc out=$p5_out"
    fi
fi

# 12. lib/usb C-extension checks (when libusb-1.0 is present)
if pkg-config --exists libusb-1.0 >/dev/null 2>&1; then
    cat > "$aud_dir/usb_test.strada" <<EOF
use lib "$PROJECT_DIR/lib";
use usb;
func main() int {
    my str \$e = usb::strerror(-1);
    usb::exit();
    if (length(\$e) == 0) { say("FAIL strerror"); return 1; }
    say("USB_OK");
    return 0;
}
EOF
    usb_rc=1
    usb_flags=$(pkg-config --cflags --libs libusb-1.0)
    if make -C "$PROJECT_DIR/lib/usb" > "$aud_dir/usb.build.log" 2>&1 \
        && "$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$aud_dir/usb_test.strada" $usb_flags -o "$aud_dir/usb_test" >> "$aud_dir/usb.build.log" 2>&1; then
        usb_out=$(timeout 15 "$aud_dir/usb_test" 2>&1) && usb_rc=0 || usb_rc=$?
    else
        usb_out=$(tail -n 10 "$aud_dir/usb.build.log" 2>/dev/null || true)
    fi
    make -C "$PROJECT_DIR/lib/usb" clean >/dev/null 2>&1 || true
    if [ "$usb_rc" -eq 0 ] && echo "$usb_out" | grep -q "USB_OK"; then
        aud_r3_record 0 "lib/usb: libusb build, strerror, exit without crash" ""
    else
        aud_r3_record 1 "lib/usb C-extension" "rc=$usb_rc out=$usb_out"
    fi
fi

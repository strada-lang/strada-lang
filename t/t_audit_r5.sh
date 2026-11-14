#!/bin/bash
# t_audit_r5.sh — Cycle 5 Audit regression suite

aud_dir="$BUILD_DIR/audit_r5"
aud_src="$SCRIPT_DIR/audit_r5"
mkdir -p "$aud_dir"

aud_r5_record() {
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

aud_r5_compile_run() {
    local src="$1"
    local name="$2"
    local expected="$3"
    shift 3
    local bin="$aud_dir/$name"
    local log="$aud_dir/${name}.compile.log"
    local out=""
    local rc=1
    if (cd "$PROJECT_DIR" && "$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$src" "$@" -o "$bin" >"$log" 2>&1); then
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

# 1. Runtime & Compiler compiled suite
if aud_r5_compile_run "$aud_src/runtime_compiler.strada" "r5_runtime_compiler" "R5_RUNTIME_COMPILER_OK"; then
    aud_r5_record 0 "Cycle 5 runtime + compiler + 'x' regression tests" ""
else
    aud_r5_record 1 "Cycle 5 runtime + compiler + 'x' regression tests" "$(cat "$aud_dir/r5_runtime_compiler.diag" 2>/dev/null)"
fi

# 2. VM + Interpreter core suite (compiled, tree-walk, and bytecode --vm)
if aud_r5_compile_run "$aud_src/vm_interp_core.strada" "r5_vm_interp_core" "R5_VM_INTERP_CORE_OK"; then
    aud_r5_record 0 "Cycle 5 VM/interp core (compiled binary)" ""
else
    aud_r5_record 1 "Cycle 5 VM/interp core (compiled binary)" "$(cat "$aud_dir/r5_vm_interp_core.diag" 2>/dev/null)"
fi

tw_rc=1
tw_out=$(cd "$PROJECT_DIR" && timeout 45 "$PROJECT_DIR/interpreter/strada-interp" --tree-walk "$aud_src/vm_interp_core.strada" 2>&1) && tw_rc=0 || tw_rc=$?
if [ "$tw_rc" -eq 0 ] && echo "$tw_out" | grep -q "R5_VM_INTERP_CORE_OK"; then
    aud_r5_record 0 "Cycle 5 VM/interp core (tree-walk interpreter)" ""
else
    aud_r5_record 1 "Cycle 5 VM/interp core (tree-walk interpreter)" "rc=$tw_rc out=$tw_out"
fi

vm_rc=1
vm_out=$(cd "$PROJECT_DIR" && timeout 45 "$PROJECT_DIR/interpreter/strada-interp" --vm "$aud_src/vm_interp_core.strada" 2>&1) && vm_rc=0 || vm_rc=$?
if [ "$vm_rc" -eq 0 ] && echo "$vm_out" | grep -q "R5_VM_INTERP_CORE_OK"; then
    aud_r5_record 0 "Cycle 5 VM/interp core (bytecode --vm)" ""
else
    aud_r5_record 1 "Cycle 5 VM/interp core (bytecode --vm)" "rc=$vm_rc out=$vm_out"
fi

# 3. Eval VM + Strada::JIT suite
if aud_r5_compile_run "$aud_src/vm_interp_jit.strada" "r5_vm_interp_jit" "R5_VM_INTERP_JIT_OK" \
        --no-use-artifacts -D HAVE_VM -I "$PROJECT_DIR/interpreter" \
        "$PROJECT_DIR/interpreter/vm.c" "$PROJECT_DIR/interpreter/vm_compiler.c"; then
    aud_r5_record 0 "Cycle 5 eval_string_vm + Eval::eval + Strada::JIT suite" ""
else
    aud_r5_record 1 "Cycle 5 eval_string_vm + Eval::eval + Strada::JIT suite" "$(cat "$aud_dir/r5_vm_interp_jit.diag" 2>/dev/null)"
fi

# 4. Stdlib & C extensions suite
if aud_r5_compile_run "$aud_src/stdlib_ext.strada" "r5_stdlib_ext" "R5_STDLIB_OK" \
        "$PROJECT_DIR/lib/Gettext/gettext_wrapper.c" -lsqlite3 -lssl -lcrypto; then
    aud_r5_record 0 "Cycle 5 stdlib + C extensions suite (DBI, Async, JSON, LWP, SSL, Gettext, Template)" ""
else
    aud_r5_record 1 "Cycle 5 stdlib + C extensions suite" "$(cat "$aud_dir/r5_stdlib_ext.diag" 2>/dev/null)"
fi

# 5. Markup & formatting suite (Markdown, YAML, Forma)
if aud_r5_compile_run "$aud_src/markup_tools.strada" "r5_markup_tools" "R5_MARKUP_OK"; then
    aud_r5_record 0 "Cycle 5 Markdown / YAML / Forma suite" ""
else
    aud_r5_record 1 "Cycle 5 Markdown / YAML / Forma suite" "$(cat "$aud_dir/r5_markup_tools.diag" 2>/dev/null)"
fi

# 6. lib/perl5 embedded NUL + SvUTF8 marshaling roundtrip
if command -v perl >/dev/null 2>&1 && perl -MExtUtils::Embed -e 1 >/dev/null 2>&1; then
    cat > "$aud_dir/p5_r5.strada" <<EOF
use lib "$PROJECT_DIR/lib";
use perl5;
func main() int {
    if (perl5::init() != 1) { say("FAIL p5 init"); return 1; }
    my str \$bin = "a\\x00café";
    perl5::set_var("r5bin", \$bin);
    my str \$got_bin = perl5::get_var("r5bin");
    my str \$u = "café";
    utf8::upgrade(\$u);
    perl5::set_var("r5u", \$u);
    my str \$got_u = perl5::get_var("r5u");
    perl5::run('sub r5_echo { return \$_[0]; } sub r5_ulen { return length(\$_[0]); }');
    my str \$cb = perl5::call("r5_echo", \$bin);
    my str \$cl = perl5::call("r5_ulen", \$u);
    perl5::shutdown();
    if (core::byte_length(\$got_bin) != 7 || \$got_bin ne \$bin) { say("FAIL bin len=" . core::byte_length(\$got_bin)); return 2; }
    if (core::byte_length(\$cb) != 7 || \$cb ne \$bin) { say("FAIL call bin"); return 3; }
    if (\$got_u ne "café" || (\$cl + 0) != 4) { say("FAIL utf8 got=" . \$got_u . " len=" . \$cl); return 4; }
    say("R5_P5_OK");
    return 0;
}
EOF
    p5_rc=1
    p5_flags=$(perl -MExtUtils::Embed -e ccopts -e ldopts)
    if make -C "$PROJECT_DIR/lib/perl5" > "$aud_dir/p5.log" 2>&1 \
        && (cd "$PROJECT_DIR" && "$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$aud_dir/p5_r5.strada" $p5_flags -o "$aud_dir/p5_r5") >> "$aud_dir/p5.log" 2>&1; then
        p5_out=$(timeout 20 "$aud_dir/p5_r5" 2>&1) && p5_rc=0 || p5_rc=$?
    else
        p5_out=$(tail -n 15 "$aud_dir/p5.log" 2>/dev/null || true)
    fi
    make -C "$PROJECT_DIR/lib/perl5" clean >/dev/null 2>&1 || true
    if [ "$p5_rc" -eq 0 ] && echo "$p5_out" | grep -q "R5_P5_OK"; then
        aud_r5_record 0 "lib/perl5 embedded NUL + SvUTF8 roundtrip" ""
    else
        aud_r5_record 1 "lib/perl5 embedded NUL + SvUTF8 roundtrip" "rc=$p5_rc out=$p5_out"
    fi
fi

# 7. CLI tools regression checks
printf '# H\r\n\r\nfoo_bar_baz <script>x</script>\r\n' > "$aud_dir/test.md"
mdh_rc=1
mdh_out=$("$PROJECT_DIR/tools/strada-md2html" -f "$aud_dir/test.md" 2>&1) && mdh_rc=0 || mdh_rc=$?
if [ "$mdh_rc" -eq 0 ] && echo "$mdh_out" | grep -q "<h1>H</h1>" && echo "$mdh_out" | grep -q "foo_bar_baz" && echo "$mdh_out" | grep -q "&lt;script&gt;" && ! echo "$mdh_out" | grep -q "<script>"; then
    aud_r5_record 0 "tools/strada-md2html CRLF, snake_case, and HTML escaping" ""
else
    aud_r5_record 1 "tools/strada-md2html CRLF, snake_case, and HTML escaping" "rc=$mdh_rc out=$mdh_out"
fi

mdm_rc=1
mdm_out=$("$PROJECT_DIR/tools/strada-md2man" --name 'a"b\c' "$aud_dir/test.md" 2>&1) && mdm_rc=0 || mdm_rc=$?
if [ "$mdm_rc" -eq 0 ] && echo "$mdm_out" | grep -qF '\(dq' && echo "$mdm_out" | grep -qF 'b\\c'; then
    aud_r5_record 0 "tools/strada-md2man escapes quotes and backslashes in --name macro arg" ""
else
    aud_r5_record 1 "tools/strada-md2man escapes quotes and backslashes in --name macro arg" "rc=$mdm_rc out=$mdm_out"
fi

cat > "$aud_dir/s2p_sample.strada" <<'EOF'
func greet(str $who) str {
    return "hi_" . $who;
}
func main() int {
    my int $a = ord("A");
    if ($a == 65) {
        say(greet("S2P_OK"));
    }
    return 0;
}
EOF
s2p_rc=1
if (cd "$PROJECT_DIR" && "$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$PROJECT_DIR/tools/strada2perl.strada" -o "$aud_dir/strada2perl" >"$aud_dir/s2p.log" 2>&1) \
    && "$aud_dir/strada2perl" "$aud_dir/s2p_sample.strada" "$aud_dir/s2p_sample.pl" >>"$aud_dir/s2p.log" 2>&1; then
    s2p_out=$(perl "$aud_dir/s2p_sample.pl" 2>&1) && s2p_rc=0 || s2p_rc=$?
else
    s2p_out=$(tail -n 15 "$aud_dir/s2p.log" 2>/dev/null || true)
fi
if [ "$s2p_rc" -eq 0 ] && echo "$s2p_out" | grep -q "hi_S2P_OK"; then
    aud_r5_record 0 "tools/strada2perl generates runnable Perl 5 output" ""
else
    aud_r5_record 1 "tools/strada2perl generates runnable Perl 5 output" "rc=$s2p_rc out=$s2p_out"
fi

doc_out=$(cd "$PROJECT_DIR" && "$PROJECT_DIR/tools/stradadoc" --no-pager "[unclosed" 2>&1)
doc_rc=$?
if [ "$doc_rc" -ne 0 ] && echo "$doc_out" | grep -q "No documentation found"; then
    aud_r5_record 0 "tools/stradadoc handles unclosed regex metachar in topic" ""
else
    aud_r5_record 1 "tools/stradadoc handles unclosed regex metachar in topic" "rc=$doc_rc out=$doc_out"
fi

prof_flag_out=$("$PROJECT_DIR/tools/strada-profhtml" --bogus-flag 2>&1)
prof_flag_rc=$?
if [ "$prof_flag_rc" -ne 0 ] && echo "$prof_flag_out" | grep -qi "Unknown option"; then
    aud_r5_record 0 "tools/strada-profhtml rejects --bogus-flag" ""
else
    aud_r5_record 1 "tools/strada-profhtml rejects --bogus-flag" "rc=$prof_flag_rc out=$prof_flag_out"
fi

perl -e '
    binmode(STDOUT);
    print "STFP";
    print pack("V3", 1, 1, 6);
    print "foo.st";
    print pack("V3", 10, 1, 1);
    print pack("Q<", 9223372036854775809);
    print pack("Q<", 100);
' > "$aud_dir/overflow.stfp"
prof_ov_out=$("$PROJECT_DIR/tools/strada-profhtml" "$aud_dir/overflow.stfp" "$aud_dir/prof_out" 2>&1)
prof_ov_rc=$?
if [ "$prof_ov_rc" -ne 0 ] && echo "$prof_ov_out" | grep -qiE "Truncated|Corrupt|Error"; then
    aud_r5_record 0 "tools/strada-profhtml rejects uint64 > INT64_MAX" ""
else
    aud_r5_record 1 "tools/strada-profhtml rejects uint64 > INT64_MAX" "rc=$prof_ov_rc out=$prof_ov_out"
fi

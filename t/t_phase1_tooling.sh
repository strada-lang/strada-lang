#!/bin/bash
# t_phase1_tooling.sh - Phase 1 regression tests for stradapp, strada-driver, and CLI tools

p1_dir="$BUILD_DIR/phase1_tooling"
mkdir -p "$p1_dir"

p1_record() {
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

# 1. stradapp: multiline strings, heredocs, POD, #define expr eval, cycle recursion guard
cat > "$p1_dir/pp_test.strada" << 'OUTER_EOF'
=pod

#ifdef MUST_BE_IGNORED_IN_POD
#error "POD directive was evaluated"
#endif

=cut

#define EXPR_VAL 1 + 2
#define CYC_A CYC_B
#define CYC_B CYC_A + 0
#define CYC_SELF CYC_SELF + 1

func main() int {
    my str $s = "line1
#ifdef MUST_BE_IGNORED_IN_STR
#endif
line2";
    my str $hd = <<'EOF';
#ifdef MUST_BE_IGNORED_IN_HEREDOC
#error "heredoc directive was evaluated"
#endif
EOF
    my int $ok = 0;
#if EXPR_VAL == 3
    $ok = 1;
#endif
#if CYC_A
    $ok = 99;
#endif
#if CYC_SELF
    $ok = $ok + 10;
#endif
    if ($ok == 11 && index($s, "MUST_BE_IGNORED_IN_STR") >= 0 && index($hd, "MUST_BE_IGNORED_IN_HEREDOC") >= 0) {
        say("PP_PHASE1_OK");
        return 0;
    }
    say("PP_PHASE1_FAIL:" . $ok);
    return 1;
}
OUTER_EOF

p1_pp_out=$("$PROJECT_DIR/tools/stradapp" "$p1_dir/pp_test.strada" > "$p1_dir/pp_out.strada" 2>&1 \
    && "$PROJECT_DIR/strada" -O0 "$p1_dir/pp_out.strada" -o "$p1_dir/pp_bin" 2>&1 \
    && "$p1_dir/pp_bin" 2>&1)
[ "$p1_pp_out" = "PP_PHASE1_OK" ]
p1_record $? "stradapp ignores #directives in POD/strings/heredocs and guards #if recursion" "out=$p1_pp_out"

# 2. strada-driver: safe -D injection after runtime header + -I shard cache invalidation
cat > "$p1_dir/drv_def.strada" << 'EOF'
func main() int {
    my int $v = 0;
    __C__ {
#ifdef SAFE_DEF_VAL
        strada_decref(v);
        v = strada_new_int(SAFE_DEF_VAL);
#endif
    }
    say("DEF=" . $v);
    return $v == 123 ? 0 : 1;
}
EOF
p1_drv_out=$("$PROJECT_DIR/strada" -O0 -DSAFE_DEF_VAL=123 "$p1_dir/drv_def.strada" -o "$p1_dir/drv_def" 2>&1 && "$p1_dir/drv_def" 2>&1)
[ "$p1_drv_out" = "DEF=123" ]
p1_record $? "strada-driver injects -D defines safely after runtime header" "out=$p1_drv_out"

# 3. strada-soinfo: .strada_meta read without invoking shared-lib constructor unless --dlopen
cat > "$p1_dir/ctor_lib.strada" << 'EOF'
package CtorLib;
__C__ {
__attribute__((constructor)) static void test_ctor(void) {
    const char *p = getenv("SOINFO_CTOR_MARKER");
    if (p) {
        FILE *f = fopen(p, "w");
        if (f) { fputs("RAN", f); fclose(f); }
    }
}
}
func ping() str { return "pong"; }
EOF
"$PROJECT_DIR/strada" --shared -O0 "$p1_dir/ctor_lib.strada" -o "$p1_dir/ctor_lib.so" >/dev/null 2>&1
rm -f "$p1_dir/ctor.marker"
p1_soinfo=$(SOINFO_CTOR_MARKER="$p1_dir/ctor.marker" "$PROJECT_DIR/tools/strada-soinfo" "$p1_dir/ctor_lib.so" 2>&1)
if echo "$p1_soinfo" | grep -q "ping" && [ ! -f "$p1_dir/ctor.marker" ]; then
    p1_record 0 "strada-soinfo inspects .so metadata without running constructors" ""
else
    p1_record 1 "strada-soinfo inspects .so metadata without running constructors" "marker=$([ -f "$p1_dir/ctor.marker" ] && echo RAN) out=$p1_soinfo"
fi

# 4. strada-md2html: escapes HTML attributes and blocks javascript: URLs
cat > "$p1_dir/xss.md" << 'EOF'
[bad](javascript:alert(1))
![a"x](http://x/"onerror="alert)
```html"x
<b>hi</b>
```
EOF
p1_html=$("$PROJECT_DIR/tools/strada-md2html" "$p1_dir/xss.md" 2>&1)
if echo "$p1_html" | grep -q 'href="#"' && echo "$p1_html" | grep -q 'alt="a&quot;x"' && ! echo "$p1_html" | grep -q 'onerror="alert' && echo "$p1_html" | grep -q 'language-html&quot;x'; then
    p1_record 0 "strada-md2html escapes attributes and blocks javascript: URLs" ""
else
    p1_record 1 "strada-md2html escapes attributes and blocks javascript: URLs" "html=$p1_html"
fi

# 5. strada-md2man: escapes leading . and ' in paragraph lines
printf ".literal dot\n'literal quote\n" > "$p1_dir/dot.md"
p1_man=$("$PROJECT_DIR/tools/strada-md2man" "$p1_dir/dot.md" 2>&1)
if echo "$p1_man" | grep -qF '\&.literal dot' && echo "$p1_man" | grep -qF "\&'literal quote"; then
    p1_record 0 "strada-md2man escapes leading dot and apostrophe in body text" ""
else
    p1_record 1 "strada-md2man escapes leading dot and apostrophe in body text" "man=$p1_man"
fi

# 6. strada-proftext / strada-profhtml: reject absurd string length without OOB/crash
python3 - <<PY
with open("$p1_dir/corrupt.prof", "wb") as f:
    f.write(b"STFP" + (1).to_bytes(4, "little") + (1).to_bytes(4, "little") + (0x7fffffff).to_bytes(4, "little"))
PY
"$PROJECT_DIR/tools/strada-proftext" "$p1_dir/corrupt.prof" >/dev/null 2>&1
rc_pt=$?
"$PROJECT_DIR/tools/strada-profhtml" "$p1_dir/corrupt.prof" "$p1_dir/prof_out" >/dev/null 2>&1
rc_ph=$?
if [ "$rc_pt" -lt 128 ] && [ "$rc_ph" -lt 128 ]; then
    p1_record 0 "strada-proftext and strada-profhtml handle corrupt STFP lengths safely" ""
else
    p1_record 1 "strada-proftext and strada-profhtml handle corrupt STFP lengths safely" "rc_pt=$rc_pt rc_ph=$rc_ph"
fi

# 8. perla-cpan and strada-repl compile and reject bad inputs / delegate cleanly
"$PROJECT_DIR/strada" -O0 "$PROJECT_DIR/tools/perla-cpan.strada" -o "$p1_dir/perla-cpan" >/dev/null 2>&1 \
    && ! "$p1_dir/perla-cpan" install 'Bad;Mod' >/dev/null 2>&1
p1_record $? "perla-cpan compiles cleanly and validates module names" ""

"$PROJECT_DIR/strada" -O0 "$PROJECT_DIR/tools/strada-repl.strada" -o "$p1_dir/strada-repl" >/dev/null 2>&1
p1_record $? "strada-repl compiles cleanly with pure Strada system_argv" ""

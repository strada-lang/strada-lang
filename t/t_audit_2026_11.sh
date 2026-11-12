#!/bin/bash
# t_audit_2026_11.sh — Comprehensive 2026-11 audit regression suite
# Covers stradapp/strada-driver, `use` (load_module) parity, stdlib regressions,
# CLI tools, VM (strada-interp), Eval, and Strada::JIT.

aud_dir="$BUILD_DIR/audit_2026_11"
aud_src="$SCRIPT_DIR/audit_2026_11"
mkdir -p "$aud_dir"

aud_record() {
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

# Fast compile helper using run_tests.sh's compile_strada with STRADA_TEST_TCC=1
# (stradac + tcc -c + objcopy weaken + gcc -fno-lto link against prebuilt runtime.o).
aud_fast_compile() {
    local src="$1"
    local name="$2"
    local extra_ld="${3:-}"
    (
        cd "$PROJECT_DIR" &&
        BUILD_DIR="$aud_dir" STRADA_TEST_TCC=1 EXTRA_LDFLAGS="${EXTRA_LDFLAGS:-} $extra_ld" compile_strada "$src" "$name"
    )
}

aud_driver_fast="-O0 -fno-lto"
if command -v tcc >/dev/null 2>&1; then
    aud_driver_fast="--tcc"
fi

# ============================================================
# Launch heavier compilations in parallel (< 8s total even on 2 cores)
# ============================================================

# Bg 1: --static-lib + driver -M / stradapp / --tcc
mkdir -p "$aud_dir/slib_cwd"
cat > "$aud_dir/slib_cwd/SlibMod.strada" << 'EOF'
package SlibMod;
func check_all() int {
    core::arena_begin();
    my str $tmp = "look" . "ahead";
    my int $m = ($tmp =~ /look(?=ahead)/) ? 1 : 0;
    core::arena_end();
    return $m;
}
EOF
cat > "$aud_dir/slib_cwd/slib_main.strada" << 'EOF'
use lib ".";
import_archive "SlibMod.a";
func main() int {
    if (SlibMod::check_all() == 1) {
        say("SLIB_OK");
        return 0;
    }
    return 1;
}
EOF
printf 'sentinel' > "$aud_dir/slib_cwd/keep_me.o"
printf 'sentinel2' > "$aud_dir/slib_cwd/SlibMod.o"
(
    rc=0
    (cd "$aud_dir/slib_cwd" && "$PROJECT_DIR/strada" -O0 -fno-lto --static-lib SlibMod.strada -o "$aud_dir/slib_cwd/SlibMod.a") > "$aud_dir/slib.log" 2>&1 || rc=1
    if [ ! -f "$aud_dir/slib_cwd/keep_me.o" ] || [ ! -f "$aud_dir/slib_cwd/SlibMod.o" ]; then
        echo "CWD .o deleted" >> "$aud_dir/slib.log"
        rc=1
    fi
    if [ "$rc" -eq 0 ]; then
        (cd "$aud_dir/slib_cwd" && "$PROJECT_DIR/strada" -O0 -fno-lto -L . slib_main.strada -o "$aud_dir/slib_cwd/slib_main") >> "$aud_dir/slib.log" 2>&1 || rc=1
    fi
    if [ "$rc" -eq 0 ]; then
        "$aud_dir/slib_cwd/slib_main" > "$aud_dir/slib.out" 2>&1 || rc=1
    fi
    echo $rc > "$aud_dir/slib.rc"
) &
pid_slib=$!

# Bg 2: stdlib_pure + stdlib_tpl
(
    rc=0
    mkdir -p "$aud_dir/sp_work" "$aud_dir/st_work"
    aud_fast_compile "$aud_src/stdlib_pure.strada" "stdlib_pure" &&
    timeout 15 "$aud_dir/stdlib_pure" --workdir "$aud_dir/sp_work" --flag --num=25 > "$aud_dir/stdlib_pure.out" 2>&1 || rc=1
    aud_fast_compile "$aud_src/stdlib_tpl.strada" "stdlib_tpl" &&
    timeout 15 "$aud_dir/stdlib_tpl" "$aud_dir/st_work" > "$aud_dir/stdlib_tpl.out" 2>&1 || rc=1
    echo $rc > "$aud_dir/stdlib_pure.rc"
) &
pid_stdlib_pure=$!

# Bg 3: stdlib_c_ext + Eval/Strada::JIT
(
    mkdir -p "$aud_dir/sc_work"
    if command -v openssl >/dev/null 2>&1; then
        openssl req -x509 -newkey rsa:2048 -nodes -keyout "$aud_dir/sc_work/key.pem" -out "$aud_dir/sc_work/cert.pem" -days 1 -subj "/CN=localhost" >/dev/null 2>&1
    fi
    aud_port=$(( 31000 + ($$ % 7000) ))
    brc=0
    aud_fast_compile "$aud_src/stdlib_c_ext.strada" "stdlib_c_ext" "-lz -lssl -lcrypto -lsqlite3" &&
    timeout 15 "$aud_dir/stdlib_c_ext" "$aud_dir/sc_work" "$aud_dir/sc_work/cert.pem" "$aud_dir/sc_work/key.pem" "$aud_port" > "$aud_dir/stdlib_c_ext.out" 2>&1 || brc=1
    echo $brc > "$aud_dir/stdlib_c_ext.rc"

    ejrc=0
    aud_fast_compile "$aud_src/eval_jit_checks.strada" "eval_jit" &&
    (cd "$PROJECT_DIR" && timeout 15 "$aud_dir/eval_jit") > "$aud_dir/eval_jit.out" 2>&1 || ejrc=1
    echo $ejrc > "$aud_dir/eval_jit.rc"
) &
pid_cext_ej=$!

# Bg 4: pacco & stradapm compile + SemVer/Manifest/Resolver + use parity
(
    pmrc=0
    cat > "$aud_dir/pacco_entry.strada" << 'EOF'
use lib "tools/pacco";
EOF
    cat "$PROJECT_DIR/tools/pacco/pacco.strada" >> "$aud_dir/pacco_entry.strada"
    cat > "$aud_dir/stradapm_entry.strada" << 'EOF'
use lib "tools/stradapm";
EOF
    cat "$PROJECT_DIR/tools/stradapm/stradapm.strada" >> "$aud_dir/stradapm_entry.strada"

    aud_fast_compile "$aud_dir/pacco_entry.strada" "pacco_bin" || pmrc=1
    aud_fast_compile "$aud_dir/stradapm_entry.strada" "stradapm_bin" || pmrc=1
    mkdir -p "$aud_dir/pm_work1" "$aud_dir/pm_work2"
    aud_fast_compile "$aud_src/pkgmgr_checks.strada" "pkgmgr_checks" &&
    timeout 10 "$aud_dir/pkgmgr_checks" "$aud_dir/pm_work1" > "$aud_dir/pacco3.out" 2>&1 || pmrc=1
    aud_fast_compile "$aud_src/stradapm_checks.strada" "stradapm_checks" &&
    timeout 10 "$aud_dir/stradapm_checks" "$aud_dir/pm_work2" > "$aud_dir/pacco4.out" 2>&1 || pmrc=1
    echo $pmrc > "$aud_dir/pkgmgr.rc"

    prc=0
    cat > "$aud_dir/parity_entry.strada" << EOF
use lib "$aud_src";
EOF
    cat "$aud_src/parity_use_test.strada" >> "$aud_dir/parity_entry.strada"
    (
        cd "$PROJECT_DIR" &&
        STRADA_USE_ARTIFACTS=0 BUILD_DIR="$aud_dir" STRADA_TEST_TCC=1 compile_strada "$aud_dir/parity_entry.strada" "parity_use"
    ) && timeout 10 "$aud_dir/parity_use" > "$aud_dir/parity_use.out" 2>&1 || prc=1
    echo $prc > "$aud_dir/parity_use.rc"
) &
pid_pm_par=$!

# ============================================================
# Foreground fast checks (driver, stradapp, CLI tools, VM)
# ============================================================

# 1a. stradapp: -DFOO1 vs #ifdef FOO2, #include "...", ### banner, #endif # comment, '{' in __C__ string
mkdir -p "$aud_dir/pp"
cat > "$aud_dir/pp/inc.strada" << 'EOF'
func from_inc() str { return "INC_OK"; }
EOF
cat > "$aud_dir/pp/main.strada" << 'EOF'
### Banner comment should pass through cleanly
#include "inc.strada"
func main() int {
    my str $tag = "NONE";
#ifdef FOO1
    $tag = "FOO1";
#endif # trailing comment on endif
#ifdef FOO2
    $tag = "FOO2_BAD";
#endif # another comment
    my int $c_ok = 0;
    __C__ {
        const char *brace_in_str = "{ not a block start";
        if (brace_in_str[0] == '{') {
            strada_decref(c_ok);
            c_ok = strada_new_int(1);
        }
    }
    if ($tag eq "FOO1" && from_inc() eq "INC_OK" && $c_ok == 1) {
        say("PP_ALL_OK");
        return 0;
    }
    say("PP_FAIL:" . $tag . ":" . $c_ok);
    return 1;
}
EOF
pp_out=$("$PROJECT_DIR/strada" $aud_driver_fast -DFOO1 "$aud_dir/pp/main.strada" -o "$aud_dir/pp/pp_bin" 2>&1 && "$aud_dir/pp/pp_bin" 2>&1)

# 1b. ./strada --tcc
tcc_out=""
if command -v tcc >/dev/null 2>&1; then
    cat > "$aud_dir/tcc_check.strada" << 'EOF'
use lib "lib";
use JSON;
func main() int {
    my str $hdr = "strada_runtime.h";
    if ($hdr ne "strada_runtime.h") { say("BAD_HDR:" . $hdr); return 1; }
    my str $cafe = "caf" . chr(233);
    my str $enc = JSON::encode({ "msg" => $cafe });
    if (index($enc, $cafe) < 0) { say("BAD_JSON:" . $enc); return 1; }
    my int $def_val = 0;
    __C__ {
#ifdef TCC_C_DEF
        strada_decref(def_val);
        def_val = strada_new_int(TCC_C_DEF);
#endif
    }
    if ($def_val != 77) { say("BAD_DEF:" . $def_val); return 1; }
    my int $rd = core::O_RDONLY();
    if ($rd != 0) { say("BAD_ORDONLY:" . $rd); return 1; }
    say("TCC_ALL_OK");
    return 0;
}
EOF
    tcc_out=$("$PROJECT_DIR/strada" --tcc -DTCC_C_DEF=77 "$aud_dir/tcc_check.strada" -o "$aud_dir/tcc_check" 2>&1 && "$aud_dir/tcc_check" 2>&1)
fi

# 1c. ./strada -r -o "$aud_dir/abs_bin"
cat > "$aud_dir/abs_src.strada" << 'EOF'
func main() int { say("ABS_RUN_OK"); return 0; }
EOF
abs_out=$("$PROJECT_DIR/strada" $aud_driver_fast -r -o "$aud_dir/abs_bin" "$aud_dir/abs_src.strada" 2>&1)

# 1d. -M CustomPkg & -M <dir> forwarding -D
mkdir -p "$aud_dir/mdir"
cat > "$aud_dir/multi_pkg.strada" << 'EOF'
package NoisePkg;
func val() int { return 11; }

package CustomPkg;
func val() int { return 99; }
EOF
cat > "$aud_dir/use_custom.strada" << 'EOF'
use lib ".";
import_object "CustomPkg.o";
func main() int {
    if (CustomPkg::val() == 99) { say("CUSTOM_PKG_OK"); return 0; }
    return 1;
}
EOF
cat > "$aud_dir/mdir/DirMod.strada" << 'EOF'
package DirMod;
func flag_val() int {
#ifdef MDIR_FORWARDED
    return 321;
#else
    return 0;
#endif
}
EOF
cat > "$aud_dir/mdir_main.strada" << 'EOF'
use lib "mdir";
use DirMod;
func main() int {
    if (DirMod::flag_val() == 321) { say("MDIR_OK"); return 0; }
    return 1;
}
EOF
mpkg_rc=0
(
    cd "$aud_dir" &&
    "$PROJECT_DIR/strada" -O0 -fno-lto -M CustomPkg multi_pkg.strada -o CustomPkg.o &&
    "$PROJECT_DIR/strada" $aud_driver_fast -L . use_custom.strada -o use_custom &&
    [ "$("./use_custom")" = "CUSTOM_PKG_OK" ] &&
    "$PROJECT_DIR/strada" -O0 -fno-lto -DMDIR_FORWARDED -M "$aud_dir/mdir" &&
    [ -f "$aud_dir/mdir/DirMod.o" ] &&
    STRADA_USE_ARTIFACTS=1 "$PROJECT_DIR/strada" $aud_driver_fast mdir_main.strada -o mdir_main &&
    [ "$("./mdir_main")" = "MDIR_OK" ]
) > "$aud_dir/mpkg.log" 2>&1 || mpkg_rc=1

# 1e. Symlinked $TMPDIR/strada-scratch-$UID rejected safely (falls back to mkdtemp without
#     following symlink, writing into target, or chmod'ing target) & trailing -o without arg
mkdir -p "$aud_dir/bad_tmp" "$aud_dir/victim_target"
chmod 0755 "$aud_dir/victim_target"
ln -s "$aud_dir/victim_target" "$aud_dir/bad_tmp/strada-scratch-$(id -u)"
TMPDIR="$aud_dir/bad_tmp" "$PROJECT_DIR/strada" -v $aud_driver_fast "$aud_dir/abs_src.strada" -o "$aud_dir/sym_bin" > "$aud_dir/sym.log" 2>&1
sym_rc=0
victim_perm=$(stat -c '%a' "$aud_dir/victim_target" 2>/dev/null || echo "")
victim_files=$(ls -A "$aud_dir/victim_target" 2>/dev/null)
if grep -q "strada-scratch-" "$aud_dir/sym.log" 2>/dev/null || [ "$victim_perm" != "755" ] || [ -n "$victim_files" ]; then
    sym_rc=1
fi

trail_rc=0
"$PROJECT_DIR/strada" "$aud_dir/abs_src.strada" -o > "$aud_dir/trail.log" 2>&1 && trail_rc=1 || trail_rc=0

# 4a. CLI tools
cli_rc=0
cli_diag=""
"$PROJECT_DIR/tools/stradadoc" --no-pager LANGUAGE > "$aud_dir/doc.out" 2>&1 || { cli_rc=1; cli_diag="stradadoc failed"; }
[ -s "$aud_dir/doc.out" ] || { cli_rc=1; cli_diag="stradadoc empty output"; }

printf '# Hello Audit\n\nSome `inline_code` text.\n' > "$aud_dir/in.md"
"$PROJECT_DIR/tools/strada-md2html" "$aud_dir/in.md" "$aud_dir/out.html" > "$aud_dir/md.log" 2>&1 || { cli_rc=1; cli_diag="strada-md2html failed"; }
grep -q "Hello Audit" "$aud_dir/out.html" 2>/dev/null || { cli_rc=1; cli_diag="strada-md2html missing content"; }

"$PROJECT_DIR/tools/strada-proftext" --help > "$aud_dir/pt.out" 2>&1 || { cli_rc=1; cli_diag="strada-proftext --help failed"; }
grep -qi "usage" "$aud_dir/pt.out" 2>/dev/null || { cli_rc=1; cli_diag="strada-proftext --help missing usage"; }

"$PROJECT_DIR/tools/strada-profhtml" --help > "$aud_dir/ph.out" 2>&1 || { cli_rc=1; cli_diag="strada-profhtml --help failed"; }
grep -qi "usage" "$aud_dir/ph.out" 2>/dev/null || { cli_rc=1; cli_diag="strada-profhtml --help missing usage"; }

# 4c. interpreter/strada-interp VM checks
vm_rc=1
vm_out="interpreter/strada-interp binary not built"
if [ -x "$PROJECT_DIR/interpreter/strada-interp" ]; then
    vm_out=$(timeout 10 "$PROJECT_DIR/interpreter/strada-interp" "$aud_src/vm_checks.strada" 2>&1)
    vm_rc=$?
fi

# ============================================================
# Report results in deterministic order
# ============================================================

# 1a
if [ "$pp_out" = "PP_ALL_OK" ]; then
    aud_record 0 "stradapp: -DFOO1 vs FOO2, #include, ### banner, #endif # comment, '{' in __C__ string" ""
else
    aud_record 1 "stradapp: -DFOO1 vs FOO2, #include, ### banner, #endif # comment, '{' in __C__ string" "got: $pp_out"
fi

# 1b
if command -v tcc >/dev/null 2>&1; then
    if [ "$tcc_out" = "TCC_ALL_OK" ]; then
        aud_record 0 "strada --tcc: strada_runtime.h literal, UTF-8 JSON::encode, -D in __C__, core::O_RDONLY()" ""
    else
        aud_record 1 "strada --tcc: strada_runtime.h literal, UTF-8 JSON::encode, -D in __C__, core::O_RDONLY()" "got: $tcc_out"
    fi
else
    test_skip "strada --tcc" "tcc not installed"
fi

# 1c
if echo "$abs_out" | grep -q "ABS_RUN_OK" && [ -x "$aud_dir/abs_bin" ]; then
    aud_record 0 "strada -r -o <abs_path>: builds and runs at absolute path" ""
else
    aud_record 1 "strada -r -o <abs_path>: builds and runs at absolute path" "got: $abs_out"
fi

# 1d
aud_record "$mpkg_rc" "strada -M CustomPkg & -M <dir> forwarding -D" "$(tail -5 "$aud_dir/mpkg.log" 2>/dev/null)"

# 1e
if [ "$sym_rc" -eq 0 ]; then
    aud_record 0 "strada-driver: rejects symlinked \$TMPDIR/strada-scratch-\$UID safely" ""
else
    aud_record 1 "strada-driver: rejects symlinked \$TMPDIR/strada-scratch-\$UID safely" "perm=$victim_perm files=$victim_files $(cat "$aud_dir/sym.log" 2>/dev/null)"
fi
aud_record "$trail_rc" "strada-driver: trailing -o without argument exits non-zero" "$(cat "$aud_dir/trail.log" 2>/dev/null)"

# 1f
wait "$pid_slib"
if [ "$(cat "$aud_dir/slib.rc" 2>/dev/null)" = "0" ] && [ "$(cat "$aud_dir/slib.out" 2>/dev/null)" = "SLIB_OK" ]; then
    aud_record 0 "strada --static-lib: preserves PCRE2 lookahead + arena_begin and CWD .o files" ""
else
    aud_record 1 "strada --static-lib: preserves PCRE2 lookahead + arena_begin and CWD .o files" "out=$(cat "$aud_dir/slib.out" 2>/dev/null) $(tail -5 "$aud_dir/slib.log" 2>/dev/null)"
fi

# 2
wait "$pid_pm_par"
if [ "$(cat "$aud_dir/parity_use.rc" 2>/dev/null)" = "0" ] && [ "$(cat "$aud_dir/parity_use.out" 2>/dev/null)" = "ALL_USE_PARITY_OK" ]; then
    aud_record 0 "use (load_module) parity: const, our, Moose has/extends/around, overload, async, multi-package" ""
else
    aud_record 1 "use (load_module) parity: const, our, Moose has/extends/around, overload, async, multi-package" "out=$(cat "$aud_dir/parity_use.out" 2>/dev/null) $(cat "$aud_dir/parity_use_strada.log" "$aud_dir/parity_use_gcc.log" 2>/dev/null | tail -5)"
fi

# 3a
wait "$pid_stdlib_pure"
if [ "$(cat "$aud_dir/stdlib_pure.rc" 2>/dev/null)" = "0" ] &&
   grep -q "ALL_STDLIB_PURE_OK" "$aud_dir/stdlib_pure.out" 2>/dev/null &&
   grep -q "ALL_STDLIB_TPL_OK" "$aud_dir/stdlib_tpl.out" 2>/dev/null; then
    aud_record 0 "stdlib (pure): SHA/MD5 UTF-8, JSON/PS, BigInt/BigFloat, Getopt, List::Util, YAML, CSV, LWP, Template, Forma, Markdown, DateTime, Async::Loop, Open3, Test" ""
else
    aud_record 1 "stdlib (pure): regressions" "pure=$(cat "$aud_dir/stdlib_pure.out" 2>/dev/null) tpl=$(cat "$aud_dir/stdlib_tpl.out" 2>/dev/null)"
fi

# 3b & 4d
wait "$pid_cext_ej"
if [ "$(cat "$aud_dir/stdlib_c_ext.rc" 2>/dev/null)" = "0" ] && grep -q "ALL_STDLIB_C_EXT_OK" "$aud_dir/stdlib_c_ext.out" 2>/dev/null; then
    aud_record 0 "stdlib (C ext): compress gzip/gunzip, ssl server/accept/close, LWP_SSL, DBI RETURNING/NUL/double-disconnect" ""
else
    aud_record 1 "stdlib (C ext): compress, ssl, LWP_SSL, DBI" "out=$(cat "$aud_dir/stdlib_c_ext.out" 2>/dev/null) $(cat "$aud_dir/stdlib_c_ext_gcc.log" 2>/dev/null | tail -5)"
fi

# 4a
aud_record "$cli_rc" "CLI tools: stradadoc --no-pager LANGUAGE, strada-md2html, strada-proftext/profhtml --help" "$cli_diag"

# 4b
if [ "$(cat "$aud_dir/pkgmgr.rc" 2>/dev/null)" = "0" ] &&
   [ "$(cat "$aud_dir/pacco3.out" 2>/dev/null)" = "ALL_PKGMGR_OK" ] &&
   [ "$(cat "$aud_dir/pacco4.out" 2>/dev/null)" = "ALL_STRADAPM_OK" ]; then
    aud_record 0 "pacco & stradapm: compile and SemVer/Manifest/Resolver work" ""
else
    aud_record 1 "pacco & stradapm: compile and SemVer/Manifest/Resolver work" "out1=$(cat "$aud_dir/pacco3.out" 2>/dev/null) out2=$(cat "$aud_dir/pacco4.out" 2>/dev/null)"
fi

# 4c
if [ "$vm_rc" -eq 0 ] && [ "$vm_out" = "ALL_VM_CHECKS_OK" ]; then
    aud_record 0 "strada-interp VM: return-in-try exc_stack, typed catch, finally, \$h{k}+=rhs once, SUPER:: & around" ""
else
    aud_record 1 "strada-interp VM: return-in-try exc_stack, typed catch, finally, \$h{k}+=rhs once, SUPER:: & around" "rc=$vm_rc out=$vm_out"
fi

# 4d
if [ "$(cat "$aud_dir/eval_jit.rc" 2>/dev/null)" = "0" ] && [ "$(cat "$aud_dir/eval_jit.out" 2>/dev/null)" = "ALL_EVAL_JIT_OK" ]; then
    aud_record 0 "Eval & Strada::JIT: 3-level SUPER::, exists/delete hashref, around, JIT redefine no-parens func & persist \$x" ""
else
    aud_record 1 "Eval & Strada::JIT" "out=$(cat "$aud_dir/eval_jit.out" 2>/dev/null) $(cat "$aud_dir/eval_jit_gcc.log" 2>/dev/null | tail -5)"
fi

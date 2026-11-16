#!/bin/bash
# t_audit_r13.sh - Cycle 13 regression tests across compiler, runtime, stdlib/C-exts, CLI, Eval/VM/JIT

r13_dir="$BUILD_DIR/audit_r13"
mkdir -p "$r13_dir"

r13_record() {
    local rc="$1"
    local label="$2"
    local diag="$3"
    TOTAL=$((TOTAL + 1))
    if [ "$rc" -eq 0 ]; then
        PASSED=$((PASSED + 1))
        log_pass "$label"
    else
        FAILED=$((FAILED + 1))
        log_fail "$label" "$diag"
    fi
}

# 1. Compiler + Runtime core suite at -O0 and -O2 (R13-C1, C2, C4, C5, C6, C7, C8, C9, blessed array)
for opt in -O0 -O2; do
    out=$("$PROJECT_DIR/strada" "$opt" "$SCRIPT_DIR/audit_r13/compiler_runtime.strada" -o "$r13_dir/comp_${opt}" 2>&1 \
        && "$r13_dir/comp_${opt}" 2>&1)
    rc=$?
    [ "$rc" -eq 0 ] && printf '%s\n' "$out" | grep -qx "AUDIT_R13_COMPILER_RUNTIME_OK"
    r13_record $? "Cycle 13 Compiler & Runtime suite ($opt)" "rc=$rc out=$out"
done

# 1b. R13-C1: stradapp multi-line construct closing before directive / __C__ on same line
cat > "$r13_dir/pp_ml.strada" << 'EOF'
func main() int {
    /* multi-line
       comment */ #if R13_PP_ACTIVE
    my int $ok = 1;
    /* multi-line
       cmt2 */ #define R13_PP_VAL 22
    #else
    my int $ok = 0;
    #endif
    my str $s = "line1
line2"; __C__ {
        strada_decref(s);
        s = strada_new_str("c_after_ml");
    }
    my int $ppv = 0;
#if R13_PP_VAL == 22
    $ppv = 22;
#endif
    if ($ok != 1 || $ppv != 22 || $s ne "c_after_ml") { return 1; }
    say("R13_C1_PP_OK");
    return 0;
}
EOF
pp_out=$("$PROJECT_DIR/strada" -O0 -D R13_PP_ACTIVE=1 "$r13_dir/pp_ml.strada" -o "$r13_dir/pp_ml" 2>&1 \
    && "$r13_dir/pp_ml" 2>&1)
pp_rc=$?
[ "$pp_rc" -eq 0 ] && printf '%s\n' "$pp_out" | grep -qx "R13_C1_PP_OK"
r13_record $? "R13-C1 stradapp multi-line close before directive and __C__ block" "rc=$pp_rc out=$pp_out"

# 2. R13-C3: Destructuring collision with existing variable in scope rejected
cat > "$r13_dir/dup_destructure.strada" << 'EOF'
func main() int {
    my int $x = 1;
    my ($x, $y) = (2, 3);
    return $x + $y;
}
EOF
dup_out=$("$PROJECT_DIR/stradac" "$r13_dir/dup_destructure.strada" "$r13_dir/dup_destructure.c" 2>&1)
dup_rc=$?
[ "$dup_rc" -ne 0 ] && printf '%s\n' "$dup_out" | grep -qiE "already declared|duplicate"
r13_record $? "R13-C3 destructuring collision with existing local reports error" "rc=$dup_rc out=$dup_out"

# 3. R13-C10: auto-sharding removes stale shards.txt before recompiling
{
    i=0
    while [ "$i" -lt 3600 ]; do
        printf 'func sp_fn_%d() int { return %d; }\n' "$i" "$i"
        i=$((i + 1))
    done
    printf 'func r13_shard_val() int { return sp_fn_0() + 10; }\n'
} > "$r13_dir/split_p1.strada"
sp_real=$(readlink -f "$r13_dir/split_p1.strada")
sp_key=$(printf '%s' "${sp_real}||1000||0" | cksum | cut -d' ' -f1)
sp_sdir="${TMPDIR:-/tmp}/strada-scratch-$(id -u)/split_p1-${sp_key}.shards"
mkdir -p "$sp_sdir"
chmod 700 "$(dirname "$sp_sdir")"
printf 'nonexistent_stale_shard.c\n' > "$sp_sdir/shards.txt"
sp_out=$("$PROJECT_DIR/strada" --shared -O0 "$r13_dir/split_p1.strada" -o "$r13_dir/split_p1.so" 2>&1)
sp_rc=$?
[ "$sp_rc" -eq 0 ] && [ -f "$r13_dir/split_p1.so" ] && [ -f "$sp_sdir/shards.txt" ] \
    && ! grep -qx "nonexistent_stale_shard.c" "$sp_sdir/shards.txt"
r13_record $? "R13-C10 auto-sharded --shared clears stale shards.txt before compile" "rc=$sp_rc sdir=$sp_sdir out=$sp_out"

# 4. Stdlib suite (R13-LIB2 DBI, R13-LIB3 compress, R13-LIB4 JSON)
std_libs="-lz"
if [ -n "${STRADA_DBI_LIBS:-}" ]; then
    std_libs="$std_libs $STRADA_DBI_LIBS"
else
    [ "${STRADA_HAVE_SQLITE:-0}" = "1" ] && std_libs="$std_libs -lsqlite3"
    [ "${STRADA_HAVE_MYSQL:-0}" = "1" ] && std_libs="$std_libs ${STRADA_MYSQL_LIBS:--lmysqlclient}"
fi
std_out=$("$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$SCRIPT_DIR/audit_r13/stdlib_tools.strada" $std_libs -o "$r13_dir/stdlib" 2>&1 \
    && "$r13_dir/stdlib" 2>&1)
std_rc=$?
[ "$std_rc" -eq 0 ] && printf '%s\n' "$std_out" | grep -qx "AUDIT_R13_STDLIB_TOOLS_OK"
r13_record $? "Cycle 13 Stdlib suite (JSON, compress, DBI blessed/tied marshaling)" "rc=$std_rc out=$std_out"

# 5. R13-LIB4 (perl5): perl5::get_var UAF & blessed/tied marshaling (when ExtUtils::Embed present)
if command -v perl >/dev/null 2>&1 && perl -MExtUtils::Embed -e 1 >/dev/null 2>&1; then
    p5_flags=$(perl -MExtUtils::Embed -e ccopts -e ldopts)
    cat > "$r13_dir/r13_p5.strada" << EOF
use lib "$PROJECT_DIR/lib";
use perl5;
package R13P5Ov;
use overload '""' => "to_s";
func new(str \$v) scalar { return bless({"v" => \$v}, "R13P5Ov"); }
func to_s(scalar \$s) str { return \$s->{"v"}; }
package main;
func main() int {
    if (perl5::init() != 1) { return 1; }
    perl5::set_var("r13_k", R13P5Ov::new("v_from_ov"));
    my str \$got = perl5::get_var(R13P5Ov::new("r13_k"));
    perl5::eval(R13P5Ov::new("\$r13_ev = 'ev_ok';"));
    my str \$ev = perl5::get_var("r13_ev");
    perl5::shutdown();
    if (\$got ne "v_from_ov" || \$ev ne "ev_ok") { return 2; }
    say("R13_PERL5_OK");
    return 0;
}
EOF
    p5_out=$("$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$r13_dir/r13_p5.strada" $p5_flags -o "$r13_dir/r13_p5" 2>&1 \
        && timeout 20 "$r13_dir/r13_p5" 2>&1)
    p5_rc=$?
    [ "$p5_rc" -eq 0 ] && printf '%s\n' "$p5_out" | grep -qx "R13_PERL5_OK"
    r13_record $? "R13-LIB4 lib/perl5 blessed overload & get_var UAF fix" "rc=$p5_rc out=$p5_out"
fi

# 6. R13-LIB1 (lib/usb/strada_usb.c): usb_str_bytes blessed/tied/bounds C harness (when libusb-1.0 present)
if [ "${STRADA_HAVE_LIBUSB:-0}" = "1" ]; then
    cat > "$r13_dir/r13_usb_test.c" << 'EOF'
#include <assert.h>
#include <limits.h>
#include <string.h>
#include <stdlib.h>
#include <libusb.h>
#include "strada_runtime.h"

static char mock_last_buf[128];
static int mock_last_len = -1;
static int mock_libusb_control_transfer(libusb_device_handle *dev, uint8_t rt, uint8_t req,
        uint16_t val, uint16_t idx, unsigned char *data, uint16_t wlen, unsigned int to) {
    (void)dev; (void)rt; (void)req; (void)val; (void)idx; (void)to;
    mock_last_len = (int)wlen;
    if (wlen <= sizeof(mock_last_buf)) memcpy(mock_last_buf, data, wlen);
    return (int)wlen;
}
static int mock_libusb_bulk_transfer(libusb_device_handle *dev, unsigned char ep, unsigned char *data,
        int len, int *transferred, unsigned int to) {
    (void)dev; (void)ep; (void)to;
    mock_last_len = len;
    if (len >= 0 && (size_t)len <= sizeof(mock_last_buf)) memcpy(mock_last_buf, data, (size_t)len);
    if (transferred) *transferred = len;
    return 0;
}
#define libusb_control_transfer(d, rt, r, v, i, data, w, t) mock_libusb_control_transfer(d, rt, r, v, i, data, w, t)
#define libusb_bulk_transfer(d, ep, data, l, tr, t) mock_libusb_bulk_transfer(d, ep, data, l, tr, t)
#define libusb_interrupt_transfer(d, ep, data, l, tr, t) mock_libusb_bulk_transfer(d, ep, data, l, tr, t)

#include "strada_usb.c"

static StradaValue* usb_ov_str(StradaValue *self, StradaValue *args) {
    (void)self; (void)args;
    return strada_new_str_len("ok\0x", 4);
}
static StradaValue* usb_tie_sc(StradaValue *cls, StradaValue *args) {
    (void)cls; (void)args;
    return strada_bless(strada_new_hash(), "R13UsbTie");
}
static StradaValue* usb_tie_fetch(StradaValue *self, StradaValue *args) {
    (void)self; (void)args;
    return strada_new_str("tied");
}

int main(void) {
    strada_oop_init();
    strada_overload_register("R13UsbOv", "\"\"", usb_ov_str);
    strada_method_register("R13UsbTie", "TIESCALAR", usb_tie_sc);
    strada_method_register("R13UsbTie", "FETCH", usb_tie_fetch);

    char raw200[201];
    memset(raw200, 'A', 200);
    raw200[200] = '\0';
    StradaValue *bl = strada_bless(strada_new_str(raw200), "R13UsbOv");
    StradaValue *ts = strada_new_undef();
    StradaValue *tref = strada_new_ref(ts, '$');
    strada_decref(strada_tie_scalar(tref, "R13UsbTie", 0));
    strada_decref(tref);

    void *fake_handle = (void *)0x1234;
    StradaValue *hv = strada_new_int((int64_t)(intptr_t)fake_handle);
    StradaValue *z = strada_new_int(0);
    StradaValue *ep_out = strada_new_int(1);

    StradaValue *r1 = strada_usb_control_transfer(hv, z, z, z, z, bl, z);
    assert(strada_to_int(r1) == 4 && mock_last_len == 4 && memcmp(mock_last_buf, "ok\0x", 4) == 0);
    strada_decref(r1);

    StradaValue *r2 = strada_usb_bulk_transfer(hv, ep_out, ts, z);
    assert(strada_to_int(r2) == 4 && mock_last_len == 4 && memcmp(mock_last_buf, "tied", 4) == 0);
    strada_decref(r2);

    StradaValue *r3 = strada_usb_interrupt_transfer(hv, ep_out, bl, z);
    assert(strada_to_int(r3) == 4 && mock_last_len == 4);
    strada_decref(r3);

    char *big = malloc(70000);
    memset(big, 'B', 70000);
    StradaValue *big_sv = strada_new_str_len(big, 70000);
    free(big);
    StradaValue *r4 = strada_usb_control_transfer(hv, z, z, z, z, big_sv, z);
    assert(strada_to_int(r4) == LIBUSB_ERROR_INVALID_PARAM);
    strada_decref(r4);
    strada_decref(big_sv);

    strada_decref(bl);
    strada_decref(ts);
    strada_decref(hv);
    strada_decref(z);
    strada_decref(ep_out);
    puts("R13_USB_OK");
    return 0;
}
EOF
    usb_out=$($GCC -O0 -I"$PROJECT_DIR/runtime" -I"$PROJECT_DIR/lib/usb" ${STRADA_LIBUSB_CFLAGS:-} "$r13_dir/r13_usb_test.c" \
        "$PROJECT_DIR/runtime/strada_runtime.o" -lm -ldl -lpthread $EXTRA_LDFLAGS ${STRADA_LIBUSB_LIBS:--lusb-1.0} -o "$r13_dir/r13_usb_test" 2>&1 \
        && "$r13_dir/r13_usb_test" 2>&1)
    usb_rc=$?
    [ "$usb_rc" -eq 0 ] && printf '%s\n' "$usb_out" | grep -qx "R13_USB_OK"
    r13_record $? "R13-LIB1 strada_usb usb_str_bytes blessed/tied/bounds checks" "rc=$usb_rc out=$usb_out"
fi

# 7. Eval, eval_string_vm (R13-VM8), and Strada::JIT (R13-VM9)
ej_out=$("$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" \
    "$SCRIPT_DIR/audit_r13/eval_jit.strada" -o "$r13_dir/eval_jit" 2>&1 \
    && "$r13_dir/eval_jit" 2>&1)
ej_rc=$?
[ "$ej_rc" -eq 0 ] && printf '%s\n' "$ej_out" | grep -qx "AUDIT_R13_EVAL_JIT_OK"
r13_record $? "Cycle 13 Eval / eval_string_vm / Strada::JIT suite (R13-VM1..9)" "rc=$ej_rc out=$ej_out"

# 8. R13-VM7: func run() entry point without func main
cat > "$r13_dir/run_entry.strada" << 'EOF'
func run() int {
    say("R13_RUN_ENTRY_OK");
    return 0;
}
EOF
run_vm=$(timeout 5 "$PROJECT_DIR/interpreter/strada-interp" "$r13_dir/run_entry.strada" 2>&1)
run_vm_rc=$?
[ "$run_vm_rc" -eq 0 ] && [ "$run_vm" = "R13_RUN_ENTRY_OK" ]
r13_record $? "R13-VM7 func run() entry fallback (VM)" "vm=($run_vm_rc:$run_vm)"

# 9. R13-VM6: 0/1/multi-arg core::path_join in VM
pj_code='say(core::path_join() . "|" . core::path_join("solo") . "|" . core::path_join("a/", "b/", "c"));'
pj_vm=$(timeout 5 "$PROJECT_DIR/interpreter/strada-interp" -e "$pj_code" 2>&1)
pj_vm_rc=$?
[ "$pj_vm_rc" -eq 0 ] && [ "$pj_vm" = "|solo|a/b/c" ]
r13_record $? "R13-VM6 core::path_join 0/1/multi-arg (VM)" "vm=($pj_vm_rc:$pj_vm)"

# 10. Compiled vs bytecode VM 2-way parity (test_audit_r13_vm)
par_src="$PROJECT_DIR/interpreter/test/test_audit_r13_vm.strada"
par_exp=$(cat "$PROJECT_DIR/interpreter/test/test_audit_r13_vm.expected")
p_comp=$("$PROJECT_DIR/strada" -O0 "$par_src" -o "$r13_dir/par_comp" 2>&1 && "$r13_dir/par_comp" 2>&1)
p_vm=$("$PROJECT_DIR/interpreter/strada-interp" "$par_src" 2>&1)
[ "$p_comp" = "$par_exp" ] && [ "$p_vm" = "$par_exp" ]
r13_record $? "Cycle 13 2-way parity (compiled / VM)" "comp='$p_comp' vm='$p_vm'"

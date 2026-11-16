#!/bin/bash
# t_audit_r14.sh - Cycle 14 regression tests across compiler, runtime, stdlib/C-exts, Eval/VM/JIT

r14_dir="$BUILD_DIR/audit_r14"
mkdir -p "$r14_dir"

r14_record() {
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

# 1. Compiler + Runtime core suite (R14-C1..C6)
for opt in ${STRADA_TEST_AUDIT_OPTS:--O0}; do
    out=$("$PROJECT_DIR/strada" "$opt" "$SCRIPT_DIR/audit_r14/compiler_runtime.strada" -o "$r14_dir/comp_${opt}" 2>&1 \
        && "$r14_dir/comp_${opt}" 2>&1)
    rc=$?
    [ "$rc" -eq 0 ] && printf '%s\n' "$out" | grep -qx "AUDIT_R14_COMPILER_RUNTIME_OK"
    r14_record $? "Cycle 14 Compiler & Runtime suite ($opt)" "rc=$rc out=$out"
done

# 2. R14-C5: stradapp context-sensitive quoting (s / 2, 1 < 2, q<...>, m<...>, s///, //, /* */)
cat > "$r14_dir/pp_quote.strada" << 'EOF'
#define R14_ONE 1
func main() int {
    my int $s = 10;
    my int $half = $s / 2; # trailing comment
#if R14_ONE
    my int $lt = 1 < 2;
    my str $ql = q<x/y#not_cmt>;
#else
    my int $lt = 0;
    my str $ql = "bad";
#endif
    my str $m = "ab/c#d";
    my int $matched = ($m =~ m/b\/c#d/);
    $m =~ s/ab/XY/;
    if ($half != 5 || $lt != 1 || $ql ne "x/y#not_cmt" || $matched != 1 || $m ne "XY/c#d") {
        return 1;
    }
    say("R14_C5_PP_OK");
    return 0;
}
EOF
pp_out=$("$PROJECT_DIR/strada" -O0 "$r14_dir/pp_quote.strada" -o "$r14_dir/pp_quote" 2>&1 \
    && "$r14_dir/pp_quote" 2>&1)
pp_rc=$?
[ "$pp_rc" -eq 0 ] && printf '%s\n' "$pp_out" | grep -qx "R14_C5_PP_OK"
r14_record $? "R14-C5 stradapp & lexer quoting/operator disambiguation" "rc=$pp_rc out=$pp_out"

# 3. Stdlib suite (R14-LIB1 MD5, R14-LIB2 SHA, R14-LIB5 JSON, R14-LIB8 DBI)
std_libs="-lz"
if [ -n "${STRADA_DBI_LIBS:-}" ]; then
    std_libs="$std_libs $STRADA_DBI_LIBS"
else
    [ "${STRADA_HAVE_SQLITE:-0}" = "1" ] && std_libs="$std_libs -lsqlite3"
    [ "${STRADA_HAVE_MYSQL:-0}" = "1" ] && std_libs="$std_libs ${STRADA_MYSQL_LIBS:--lmysqlclient}"
fi
std_out=$("$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$SCRIPT_DIR/audit_r14/stdlib_tools.strada" $std_libs -o "$r14_dir/stdlib" 2>&1 \
    && "$r14_dir/stdlib" 2>&1)
std_rc=$?
[ "$std_rc" -eq 0 ] && printf '%s\n' "$std_out" | grep -qx "AUDIT_R14_STDLIB_TOOLS_OK"
r14_record $? "Cycle 14 Stdlib suite (MD5/SHA/JSON/DBI tied & overloaded NUL/UTF-8)" "rc=$std_rc out=$std_out"

# 4. R14-LIB6 & R14-LIB7 (lib/perl5.strada & lib/perl5/strada_perl5.c)
if command -v perl >/dev/null 2>&1 && perl -MExtUtils::Embed -e 1 >/dev/null 2>&1; then
    p5_flags=$(perl -MExtUtils::Embed -e ccopts -e ldopts)
    printf 'use lib "%s/lib";\n' "$PROJECT_DIR" > "$r14_dir/r14_p5.strada"
    cat >> "$r14_dir/r14_p5.strada" << 'EOF'
use perl5;
package R14P5Tie;
func TIESCALAR(str $cls, str $v) scalar { return bless({"v" => $v}, $cls); }
func FETCH(scalar $s) scalar { return $s->{"v"}; }
package main;
func main() int {
    if (perl5::init() != 1) { return 1; }
    perl5::run("sub r14_echo_len { my $x = shift; utf8::encode($x) if utf8::is_utf8($x); return length($x) . ':' . $x; }");
    my scalar $ts = undef;
    tie($ts, "R14P5Tie", "café\x00z");
    my str $res = perl5::call("r14_echo_len", $ts);
    untie($ts);
    perl5::shutdown();
    # "café\0z" is 7 UTF-8 bytes
    if (substr($res, 0, 2) ne "7:") { return 2; }
    if ($res ne "7:café\x00z") { return 3; }
    say("R14_PERL5_STRADA_OK");
    return 0;
}
EOF
    p5_out=$("$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$r14_dir/r14_p5.strada" $p5_flags -o "$r14_dir/r14_p5" 2>&1 \
        && timeout 20 "$r14_dir/r14_p5" 2>&1)
    p5_rc=$?
    [ "$p5_rc" -eq 0 ] && printf '%s\n' "$p5_out" | grep -qx "R14_PERL5_STRADA_OK"
    r14_record $? "R14-LIB6 lib/perl5.strada fallback preserves UTF-8 & embedded NUL" "rc=$p5_rc out=$p5_out"

    cat > "$r14_dir/r14_perl5_c.c" << 'EOF'
#include <assert.h>
#include <string.h>
#include "strada_runtime.h"

extern int strada_perl5_init(void);
extern void strada_perl5_shutdown(void);
extern void strada_perl5_run_raw(const char *code);
extern StradaValue* strada_perl5_call_sv(StradaValue *sub_sv, StradaValue *args_sv);
extern StradaValue* strada_perl5_call_array(StradaValue *sub_sv, StradaValue *args_sv);
extern char* strada_perl5_get_error(void);

static StradaValue* p5_ts_new(StradaValue *cls, StradaValue *args) {
    (void)cls;
    StradaValue *h = strada_new_hash();
    StradaValue *v = strada_array_get_owned(args->value.av, 0);
    strada_hv_store(h, "v", v);
    strada_decref(v);
    return strada_bless(h, "R14P5CTieS");
}
static StradaValue* p5_ts_fetch(StradaValue *self, StradaValue *args) {
    (void)args;
    return strada_hv_fetch_owned(self, "v");
}
static StradaValue* p5_ta_new(StradaValue *cls, StradaValue *args) {
    (void)cls; (void)args;
    StradaValue *h = strada_new_hash();
    StradaValue *arr = strada_new_array();
    StradaValue *e0 = strada_new_str_len_utf8("café\0a", 7);
    StradaValue *e1 = strada_new_str_len("b\0c", 3);
    strada_array_push(arr->value.av, e0);
    strada_array_push(arr->value.av, e1);
    strada_decref(e0);
    strada_decref(e1);
    strada_hv_store(h, "arr", arr);
    strada_decref(arr);
    return strada_bless(h, "R14P5CTieA");
}
static StradaValue* p5_ta_size(StradaValue *self, StradaValue *args) {
    (void)args;
    StradaValue *arr = strada_hv_fetch_owned(self, "arr");
    int64_t n = (int64_t)arr->value.av->size;
    strada_decref(arr);
    return STRADA_MAKE_TAGGED_INT(n);
}
static StradaValue* p5_ta_fetch(StradaValue *self, StradaValue *args) {
    StradaValue *arr = strada_hv_fetch_owned(self, "arr");
    StradaValue *idx = strada_array_get_owned(args->value.av, 0);
    StradaValue *el = strada_array_get_owned(arr->value.av, strada_to_int(idx));
    strada_decref(idx);
    strada_decref(arr);
    return el;
}

int main(void) {
    strada_oop_init();
    strada_method_register("R14P5CTieS", "TIESCALAR", p5_ts_new);
    strada_method_register("R14P5CTieS", "FETCH", p5_ts_fetch);
    strada_method_register("R14P5CTieA", "TIEARRAY", p5_ta_new);
    strada_method_register("R14P5CTieA", "FETCHSIZE", p5_ta_size);
    strada_method_register("R14P5CTieA", "FETCH", p5_ta_fetch);

    assert(strada_perl5_init() == 1);
    strada_perl5_run_raw("sub c_join { return join('|', @_); } sub c_ret { return @_; } sub c_die { die('p5_boom'); }");

    StradaValue *ta = strada_new_array();
    StradaValue *ta_ref = strada_new_ref(ta, '@');
    strada_decref(strada_tie_array(ta_ref, "R14P5CTieA", 0));

    StradaValue *ts = strada_new_undef();
    StradaValue *ts_ref = strada_new_ref(ts, '$');
    strada_decref(strada_tie_scalar(ts_ref, "R14P5CTieS", 1, ta_ref));
    StradaValue *wrap_ref = strada_new_ref(ts, '$');

    StradaValue *sub1 = strada_new_str("c_join");
    StradaValue *res1 = strada_perl5_call_sv(sub1, wrap_ref);
    assert(res1 && res1->type == STRADA_STR && STRADA_STR_BYTELEN(res1) == 11);
    assert(memcmp(res1->value.pv, "caf\xc3\xa9\0a|b\0c", 11) == 0);
    strada_decref(res1);
    strada_decref(sub1);

    StradaValue *sub2 = strada_new_str("c_ret");
    StradaValue *arr_res = strada_perl5_call_array(sub2, ts);
    assert(arr_res && arr_res->type == STRADA_ARRAY && arr_res->value.av->size == 2);
    StradaValue *r0 = strada_array_get_owned(arr_res->value.av, 0);
    StradaValue *r1 = strada_array_get_owned(arr_res->value.av, 1);
    assert(STRADA_STR_BYTELEN(r0) == 7 && memcmp(r0->value.pv, "caf\xc3\xa9\0a", 7) == 0);
    assert(STRADA_STR_BYTELEN(r1) == 3 && memcmp(r1->value.pv, "b\0c", 3) == 0);
    strada_decref(r0); strada_decref(r1); strada_decref(arr_res); strada_decref(sub2);

    StradaValue *sub_die = strada_new_str("c_die");
    StradaValue *d1 = strada_perl5_call_sv(sub_die, wrap_ref);
    assert(d1 && d1->type == STRADA_STR && strstr(d1->value.pv, "p5_boom") != NULL);
    strada_decref(d1);
    StradaValue *d2 = strada_perl5_call_array(sub_die, ta_ref);
    assert(d2 && d2->type == STRADA_ARRAY && d2->value.av->size == 0);
    strada_decref(d2);
    strada_decref(sub_die);

    strada_decref(wrap_ref);
    strada_decref(ts_ref);
    strada_decref(ts);
    strada_decref(ta_ref);
    strada_decref(ta);
    strada_perl5_shutdown();
    puts("R14_PERL5_C_OK");
    return 0;
}
EOF
    p5c_out=$($GCC -O0 -I"$PROJECT_DIR/runtime" "$r14_dir/r14_perl5_c.c" "$PROJECT_DIR/lib/perl5/strada_perl5.c" \
        "$PROJECT_DIR/runtime/strada_runtime.o" -Wl,--gc-sections ${STRADA_PCRE2_LIBS:-} ${STRADA_FFI_LIBS:-} \
        $p5_flags -lm -ldl -lpthread $EXTRA_LDFLAGS -o "$r14_dir/r14_perl5_c" 2>&1 \
        && timeout 20 "$r14_dir/r14_perl5_c" 2>&1)
    p5c_rc=$?
    [ "$p5c_rc" -eq 0 ] && printf '%s\n' "$p5c_out" | grep -qx "R14_PERL5_C_OK"
    r14_record $? "R14-LIB7 lib/perl5/strada_perl5.c call_sv/call_array tied array/scalar & unwrap" "rc=$p5c_rc out=$p5c_out"
fi

# 5. R14-LIB4: lib/usb.strada usb_extract_bytes with mocked libusb
if [ "${STRADA_HAVE_LIBUSB:-0}" = "1" ]; then
    cat > "$r14_dir/r14_usb_mock.c" << 'EOF'
#include <libusb.h>
#include <string.h>
static unsigned char m_buf[128];
static int m_len = -1;
int libusb_init(libusb_context **ctx) { if (ctx) *ctx = (libusb_context *)0x1; return 0; }
void libusb_exit(libusb_context *ctx) { (void)ctx; }
libusb_device_handle* libusb_open_device_with_vid_pid(libusb_context *ctx, uint16_t v, uint16_t p) {
    (void)ctx; (void)v; (void)p; return (libusb_device_handle *)0x2;
}
void libusb_close(libusb_device_handle *h) { (void)h; }
int libusb_bulk_transfer(libusb_device_handle *h, unsigned char ep, unsigned char *data, int len, int *tr, unsigned int to) {
    (void)h; (void)ep; (void)to;
    m_len = len;
    if (len >= 0 && (size_t)len <= sizeof(m_buf)) memcpy(m_buf, data, (size_t)len);
    if (tr) *tr = len;
    return 0;
}
int libusb_interrupt_transfer(libusb_device_handle *h, unsigned char ep, unsigned char *data, int len, int *tr, unsigned int to) {
    return libusb_bulk_transfer(h, ep, data, len, tr, to);
}
int libusb_control_transfer(libusb_device_handle *h, uint8_t rt, uint8_t req, uint16_t val, uint16_t idx, unsigned char *data, uint16_t wlen, unsigned int to) {
    (void)h; (void)rt; (void)req; (void)val; (void)idx; (void)to;
    m_len = (int)wlen;
    if (wlen <= sizeof(m_buf)) memcpy(m_buf, data, wlen);
    return (int)wlen;
}
int r14_usb_mock_check(const char *exp, int exp_len) {
    return (m_len == exp_len && memcmp(m_buf, exp, (size_t)exp_len) == 0) ? 1 : 0;
}
EOF
    printf 'use lib "%s/lib";\n' "$PROJECT_DIR" > "$r14_dir/r14_usb_strada.strada"
    cat >> "$r14_dir/r14_usb_strada.strada" << 'EOF'
use usb;
__C__ {
    extern int r14_usb_mock_check(const char *exp, int exp_len);
}
package R14UsbOv;
use overload '""' => "to_s";
func new(str $v) scalar { return bless({"v" => $v}, "R14UsbOv"); }
func to_s(scalar $s) str { return $s->{"v"}; }
package R14UsbTie;
func TIESCALAR(str $cls, str $v) scalar { return bless({"v" => $v}, $cls); }
func FETCH(scalar $s) scalar { return $s->{"v"}; }
package main;
func main() int {
    if (usb::init() != 0) { return 1; }
    my int $h = usb::open_device(0x1234, 0x5678);
    if ($h < 0) { return 2; }
    my str $p = "u\x00s\x00b";
    my scalar $ov = R14UsbOv::new($p);
    my scalar $ts = undef;
    tie($ts, "R14UsbTie", $p);

    my int $r1 = usb::bulk_write($h, 1, $ov, 100);
    my int $c1 = 0;
    __C__ { c1 = strada_new_int(r14_usb_mock_check("u\0s\0b", 5)); }
    my int $r2 = usb::interrupt_write($h, 1, $ts, 100);
    my int $c2 = 0;
    __C__ { c2 = strada_new_int(r14_usb_mock_check("u\0s\0b", 5)); }
    my int $r3 = usb::control_write($h, 0x40, 1, 0, 0, $ts, 100);
    my int $c3 = 0;
    __C__ { c3 = strada_new_int(r14_usb_mock_check("u\0s\0b", 5)); }

    usb::close($h);
    usb::exit();
    if ($r1 != 5 || $c1 != 1 || $r2 != 5 || $c2 != 1 || $r3 != 5 || $c3 != 1) { return 3; }
    say("R14_LIBUSB_STRADA_OK");
    return 0;
}
EOF
    usb_out=$($GCC -O0 -c ${STRADA_LIBUSB_CFLAGS:-} "$r14_dir/r14_usb_mock.c" -o "$r14_dir/r14_usb_mock.o" 2>&1 \
        && "$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$r14_dir/r14_usb_strada.strada" "$r14_dir/r14_usb_mock.o" \
           ${STRADA_LIBUSB_CFLAGS:-} ${STRADA_LIBUSB_LIBS:--lusb-1.0} -o "$r14_dir/r14_usb_strada" 2>&1 \
        && "$r14_dir/r14_usb_strada" 2>&1)
    usb_rc=$?
    [ "$usb_rc" -eq 0 ] && printf '%s\n' "$usb_out" | grep -qx "R14_LIBUSB_STRADA_OK"
    r14_record $? "R14-LIB4 lib/usb.strada tied/overloaded binary NUL payloads" "rc=$usb_rc out=$usb_out"
fi

# 6. R14-LIB3: lib/ssl.strada write & write_binary with tied scalar + blessed overload + embedded NULs
if [ -f "$PROJECT_DIR/lib/ssl.o" ] && command -v openssl >/dev/null 2>&1; then
    ssl_port=$(( 23000 + RANDOM % 7000 ))
    openssl req -x509 -newkey rsa:2048 -nodes -keyout "$r14_dir/key.pem" -out "$r14_dir/cert.pem" \
        -days 1 -subj "/CN=localhost" >/dev/null 2>&1
    printf 'use lib "%s/lib";\n' "$PROJECT_DIR" > "$r14_dir/r14_ssl.strada"
    cat >> "$r14_dir/r14_ssl.strada" << 'EOF'
use ssl;
package R14SslOv;
use overload '""' => "to_s";
func new(str $v) scalar { return bless({"v" => $v}, "R14SslOv"); }
func to_s(scalar $s) str { return $s->{"v"}; }
package R14SslTie;
func TIESCALAR(str $cls, str $v) scalar { return bless({"v" => $v}, $cls); }
func FETCH(scalar $s) scalar { return $s->{"v"}; }
package main;
func main(int $argc, array @argv) int {
    my int $port = $argv[1] + 0;
    my str $cert = $argv[2];
    my str $key = $argv[3];
    if (ssl::init() != 0) { return 1; }
    my int $srv = -1;
    my int $t = 0;
    while ($t < 10 && $srv < 0) {
        $srv = ssl::server($port + $t, $cert, $key);
        if ($srv < 0) { $t = $t + 1; }
    }
    if ($srv < 0) { return 2; }
    my int $bound = $port + $t;

    my int $pid = core::fork();
    if ($pid == 0) {
        my int $c = ssl::connect_insecure("127.0.0.1", $bound);
        if ($c < 0) { exit(1); }
        my scalar $ov = R14SslOv::new("a\x00b");
        my scalar $ts = undef;
        tie($ts, "R14SslTie", "c\x00d");
        my int $w1 = ssl::write($c, $ov);
        my int $w2 = ssl::write_binary($c, $ts);
        ssl::close($c);
        ssl::close($srv);
        exit(($w1 == 3 && $w2 == 3) ? 0 : 2);
    }

    my int $acc = ssl::accept($srv);
    if ($acc < 0) { core::waitpid($pid, 0); ssl::close($srv); return 3; }
    my str $acc_buf = "";
    while (length($acc_buf) < 6) {
        my str $chunk = ssl::read_binary($acc, 32);
        if (length($chunk) == 0) { last; }
        $acc_buf = $acc_buf . $chunk;
    }
    ssl::close($acc);
    ssl::close($srv);
    my int $st = core::waitpid($pid, 0);
    if ($st != 0 || length($acc_buf) != 6 || $acc_buf ne "a\x00bc\x00d") { return 4; }
    say("R14_SSL_OK");
    return 0;
}
EOF
    ssl_out=$("$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$r14_dir/r14_ssl.strada" -lssl -lcrypto -o "$r14_dir/r14_ssl" 2>&1 \
        && timeout 25 "$r14_dir/r14_ssl" "$ssl_port" "$r14_dir/cert.pem" "$r14_dir/key.pem" 2>&1)
    ssl_rc=$?
    [ "$ssl_rc" -eq 0 ] && printf '%s\n' "$ssl_out" | grep -qx "R14_SSL_OK"
    r14_record $? "R14-LIB3 lib/ssl write/write_binary tied & overloaded NUL payloads" "rc=$ssl_rc out=$ssl_out"
fi

# 7. Eval, eval_string_vm, and Strada::JIT
ej_out=$("$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" \
    "$SCRIPT_DIR/audit_r14/eval_jit.strada" -o "$r14_dir/eval_jit" 2>&1 \
    && timeout 20 "$r14_dir/eval_jit" 2>&1)
ej_rc=$?
[ "$ej_rc" -eq 0 ] && printf '%s\n' "$ej_out" | grep -qx "AUDIT_R14_EVAL_JIT_OK"
r14_record $? "Cycle 14 Eval / eval_string_vm / Strada::JIT suite" "rc=$ej_rc out=$ej_out"

# 8. Compiled vs bytecode VM 2-way parity (test_audit_r14_vm)
par_src="$PROJECT_DIR/interpreter/test/test_audit_r14_vm.strada"
par_exp=$(cat "$PROJECT_DIR/interpreter/test/test_audit_r14_vm.expected")
p_comp=$("$PROJECT_DIR/strada" -O0 "$par_src" -o "$r14_dir/par_comp" 2>&1 && "$r14_dir/par_comp" 2>&1)
p_vm=$("$PROJECT_DIR/interpreter/strada-interp" "$par_src" 2>&1)
[ "$p_comp" = "$par_exp" ] && [ "$p_vm" = "$par_exp" ]
r14_record $? "Cycle 14 2-way parity (compiled / VM)" "comp='$p_comp' vm='$p_vm'"

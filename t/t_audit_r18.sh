#!/bin/bash
# t_audit_r18.sh - Cycle 18 regression tests across compiler, runtime, stdlib/C-exts, tools, Eval/VM/JIT

r18_dir="$BUILD_DIR/audit_r18"
mkdir -p "$r18_dir"

r18_record() {
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

# 1. Compiler, Runtime, JSON, YAML & Template core suite (-O0 by default, plus -O2 under --default)
for opt in ${STRADA_TEST_AUDIT_OPTS:--O0}; do
    out=$("$PROJECT_DIR/strada" "$opt" -L "$PROJECT_DIR/lib" "$SCRIPT_DIR/audit_r18/compiler_runtime.strada" -o "$r18_dir/comp_${opt}" 2>&1 \
        && timeout 20 "$r18_dir/comp_${opt}" 2>&1)
    rc=$?
    [ "$rc" -eq 0 ] && printf '%s\n' "$out" | grep -qx "AUDIT_R18_COMPILER_RUNTIME_OK"
    r18_record $? "Cycle 18 Compiler, Runtime & Stdlib suite ($opt)" "rc=$rc out=$out"
done

# 2. C6: Cross-.so import_lib devirtualization with omitted default params
c6_out=$(
    set -e
    cat > "$r18_dir/R18ImportLib.strada" << 'EOF'
package R18ImpObj;
has ro str $tag = "T";
func greet(scalar $self, str $who, str $punct = "!") str {
    return $self->tag() . ":" . $who . $punct;
}

package R18ImportLib;
func fn_def(str $a, int $b = 42, str $c = "ok") str {
    return $a . "=" . $b . ":" . $c;
}
EOF
    "$PROJECT_DIR/strada" -O0 --shared "$r18_dir/R18ImportLib.strada" -o "$r18_dir/R18ImportLib.so" 2>&1
    cat > "$r18_dir/c6_main.strada" << EOF
use lib "$r18_dir";
import_lib "R18ImportLib.so";
func main() int {
    my scalar \$o = R18ImpObj::new("tag", "HI");
    my str \$m1 = \$o->greet("Alice");
    my str \$m2 = \$o->greet("Bob", "?");
    my str \$f1 = R18ImportLib::fn_def("X", 42, "ok");
    my str \$f2 = R18ImportLib::fn_def("Y", 7, "yo");
    if (\$m1 ne "HI:Alice!" || \$m2 ne "HI:Bob?" || \$f1 ne "X=42:ok" || \$f2 ne "Y=7:yo") {
        say("BAD: \$m1 | \$m2 | \$f1 | \$f2");
        return 1;
    }
    say("C6_IMPORT_DEVIRT_OK");
    return 0;
}
EOF
    "$PROJECT_DIR/strada" -O0 "$r18_dir/c6_main.strada" -o "$r18_dir/c6_main" 2>&1
    LD_LIBRARY_PATH="$r18_dir:${LD_LIBRARY_PATH:-}" "$r18_dir/c6_main" 2>&1
)
c6_rc=$?
[ "$c6_rc" -eq 0 ] && printf '%s\n' "$c6_out" | grep -qx "C6_IMPORT_DEVIRT_OK"
r18_record $? "C6 Cross-.so devirtualization preserves omitted default params" "rc=$c6_rc out=$c6_out"

# 3. L1: lib/perl5 preserves STRADA_INT_IS_UV unsigned uint64 (> INT64_MAX) as Perl UV
p5_flags=$(perl -MExtUtils::Embed -e ccopts -e ldopts 2>/dev/null || true)
if [ -n "$p5_flags" ]; then
    cat > "$r18_dir/l1_perl5_uv.c" << 'EOF'
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include "strada_runtime.h"
extern int strada_perl5_init(void);
extern void strada_perl5_shutdown(void);
extern void strada_perl5_run_raw(const char *code);
extern StradaValue* strada_perl5_call_array(StradaValue *sub_sv, StradaValue *args_sv);
int main(void) {
    assert(strada_perl5_init() == 1);
    strada_perl5_run_raw("sub fmt_uv { return sprintf('%s', $_[0]); }");
    StradaValue *sub = strada_new_str("fmt_uv");
    StradaValue *args = strada_new_array();
    strada_array_push_take(args->value.av, strada_new_uint(18446744073709551600ULL));
    StradaValue *res = strada_perl5_call_array(sub, args);
    assert(res && res->type == STRADA_ARRAY && res->value.av->size >= 1);
    StradaValue *s = strada_array_get(res->value.av, 0);
    assert(s && s->type == STRADA_STR && strcmp(s->value.pv, "18446744073709551600") == 0);
    strada_decref(sub);
    strada_decref(args);
    strada_decref(res);
    strada_perl5_shutdown();
    printf("L1_PERL5_UV_OK\n");
    return 0;
}
EOF
    l1_out=$($GCC -O0 -I"$PROJECT_DIR/runtime" "$r18_dir/l1_perl5_uv.c" "$PROJECT_DIR/lib/perl5/strada_perl5.c" \
        "$PROJECT_DIR/runtime/strada_runtime.o" -lm -ldl -lpthread $EXTRA_LDFLAGS $p5_flags -o "$r18_dir/l1_perl5_uv" 2>&1 \
        && timeout 20 "$r18_dir/l1_perl5_uv" 2>&1)
    l1_rc=$?
    [ "$l1_rc" -eq 0 ] && printf '%s\n' "$l1_out" | grep -qx "L1_PERL5_UV_OK"
    r18_record $? "L1 lib/perl5 preserves unsigned UV > INT64_MAX" "rc=$l1_rc out=$l1_out"
fi

# 4. L2: lib/usb write paths reject IN-direction endpoint / bmRequestType (bit 0x80 set)
if [ "${STRADA_HAVE_LIBUSB:-0}" = "1" ]; then
    cat > "$r18_dir/l2_usb_c.c" << 'EOF'
#include <assert.h>
#include <stdio.h>
extern int strada_usb_bulk_write_raw(void *handle, int endpoint, const char *data, int length, int timeout);
extern int strada_usb_interrupt_write_raw(void *handle, int endpoint, const char *data, int length, int timeout);
extern int strada_usb_control_write_raw(void *handle, int request_type, int request, int value, int idx, const char *data, int length, int timeout);
int main(void) {
    assert(strada_usb_bulk_write_raw((void*)1, 0x81, "x", 1, 100) == -2);
    assert(strada_usb_interrupt_write_raw((void*)1, 0x82, "x", 1, 100) == -2);
    assert(strada_usb_control_write_raw((void*)1, 0x80, 0, 0, 0, "x", 1, 100) == -2);
    printf("L2_USB_C_OK\n");
    return 0;
}
EOF
    cat > "$r18_dir/l2_usb_strada.strada" << EOF
use lib "$PROJECT_DIR/lib";
use usb;
func main() int {
    my int \$b = usb::bulk_write(1, 0x81, "ab", 100);
    my int \$i = usb::interrupt_write(1, 0x82, "ab", 100);
    my int \$c = usb::control_write(1, 0x80, 0, 0, 0, "ab", 100);
    if (\$b != -2 || \$i != -2 || \$c != -2) {
        say("BAD b=\$b i=\$i c=\$c");
        return 1;
    }
    say("L2_USB_STRADA_OK");
    return 0;
}
EOF
    l2_out=$(
        $GCC -O0 -I"$PROJECT_DIR/runtime" $STRADA_LIBUSB_CFLAGS "$r18_dir/l2_usb_c.c" "$PROJECT_DIR/lib/usb/strada_usb.c" \
            "$PROJECT_DIR/runtime/strada_runtime.o" -lm -ldl -lpthread $EXTRA_LDFLAGS $STRADA_LIBUSB_LIBS -o "$r18_dir/l2_usb_c" 2>&1 \
            && "$r18_dir/l2_usb_c" 2>&1 \
            && "$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$r18_dir/l2_usb_strada.strada" $STRADA_LIBUSB_CFLAGS $STRADA_LIBUSB_LIBS -o "$r18_dir/l2_usb_strada" 2>&1 \
            && "$r18_dir/l2_usb_strada" 2>&1
    )
    l2_rc=$?
    [ "$l2_rc" -eq 0 ] && printf '%s\n' "$l2_out" | grep -qx "L2_USB_C_OK" && printf '%s\n' "$l2_out" | grep -qx "L2_USB_STRADA_OK"
    r18_record $? "L2 lib/usb write helpers reject IN-direction endpoint/request_type" "rc=$l2_rc out=$l2_out"
fi

# 5. L3: lib/ssl validates fd range (rejects negative / > INT_MAX) and sets SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER
if [ "${STRADA_HAVE_SSL:-0}" = "1" ]; then
    cat > "$r18_dir/l3_ssl.strada" << EOF
use lib "$PROJECT_DIR/lib";
use ssl;
__C__ {
#include <sys/socket.h>
#include <unistd.h>
#include <openssl/ssl.h>
}
func main() int {
    # Overflowing fd64 = 2^32 (4294967296) would truncate to fd 0 (stdin) without range check
    my int \$r_neg = ssl::attach_fd(-1, 0, "localhost");
    my int \$r_big = ssl::attach_fd(4294967296, 0, "localhost");
    if (\$r_neg != 0 || \$r_big != 0) {
        say("BAD attach_fd validation r_neg=\$r_neg r_big=\$r_big");
        return 1;
    }

    my int \$fd0 = -1;
    my int \$fd1 = -1;
    __C__ {
        int sv[2];
        if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) == 0) {
            strada_decref(fd0); fd0 = strada_new_int(sv[0]);
            strada_decref(fd1); fd1 = strada_new_int(sv[1]);
        }
    }
    if (\$fd0 < 0 || \$fd1 < 0) { return 2; }
    my int \$conn = ssl::attach_fd(\$fd0, 0, "example.com");
    if (\$conn == 0) {
        __C__ { close((int)strada_to_int(fd0)); close((int)strada_to_int(fd1)); }
        return 3;
    }
    my int \$s_neg = ssl::attach_server_fd(\$conn, -5);
    my int \$s_big = ssl::attach_server_fd(\$conn, 4294967296);
    if (\$s_neg != 0 || \$s_big != 0) {
        ssl::close(\$conn);
        __C__ { close((int)strada_to_int(fd1)); }
        say("BAD attach_server_fd validation s_neg=\$s_neg s_big=\$s_big");
        return 4;
    }
    my int \$mode_ok = 0;
    __C__ {
        typedef struct { int socket_fd; SSL *ssl; SSL_CTX *ctx; } R18SSLHead;
        R18SSLHead *c = (R18SSLHead *)(intptr_t)strada_to_int(conn);
        if (c && c->ctx) {
            long m = SSL_CTX_get_mode(c->ctx);
            if (m & SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER) {
                strada_decref(mode_ok);
                mode_ok = strada_new_int(1);
            }
        }
    }
    ssl::close(\$conn);
    __C__ { close((int)strada_to_int(fd1)); }
    if (\$mode_ok != 1) {
        say("BAD SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER not set");
        return 4;
    }
    say("L3_SSL_OK");
    return 0;
}
EOF
    l3_out=$("$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$r18_dir/l3_ssl.strada" $STRADA_SSL_LIBS -o "$r18_dir/l3_ssl" 2>&1 \
        && timeout 20 "$r18_dir/l3_ssl" 2>&1)
    l3_rc=$?
    [ "$l3_rc" -eq 0 ] && printf '%s\n' "$l3_out" | grep -qx "L3_SSL_OK"
    r18_record $? "L3 lib/ssl validates fd range & enables SSL_MODE_ACCEPT_MOVING_WRITE_BUFFER" "rc=$l3_rc out=$l3_out"
fi

# 6. L7: strada-proftext & strada-profhtml handle non-seekable streams (FIFO)
l7_out=$(
    set -e
    cat > "$r18_dir/prof_sample.strada" << 'EOF'
func work(int $n) int {
    my int $s = 0;
    for (my int $i = 0; $i < $n; $i = $i + 1) { $s = $s + $i; }
    return $s;
}
func main() int {
    return (work(50) > 0) ? 0 : 1;
}
EOF
    "$PROJECT_DIR/strada" -O0 --full-profile "$r18_dir/prof_sample.strada" -o "$r18_dir/prof_sample" 2>&1
    (cd "$r18_dir" && ./prof_sample)
    [ -s "$r18_dir/strada-prof.out" ]
    mkfifo "$r18_dir/prof.fifo"
    cat "$r18_dir/strada-prof.out" > "$r18_dir/prof.fifo" &
    p_wr=$!
    txt_out=$("$PROJECT_DIR/tools/strada-proftext" "$r18_dir/prof.fifo" 2>&1)
    wait "$p_wr"
    printf '%s\n' "$txt_out" | grep -q "Function"

    cat "$r18_dir/strada-prof.out" > "$r18_dir/prof.fifo" &
    p_wr2=$!
    "$PROJECT_DIR/tools/strada-profhtml" "$r18_dir/prof.fifo" "$r18_dir/prof_out" >/dev/null 2>&1
    wait "$p_wr2"
    grep -q "<html" "$r18_dir/prof_out/index.html"
    echo "L7_PROF_FIFO_OK"
)
l7_rc=$?
[ "$l7_rc" -eq 0 ] && printf '%s\n' "$l7_out" | grep -qx "L7_PROF_FIFO_OK"
r18_record $? "L7 strada-proftext & strada-profhtml stream from non-seekable FIFO" "rc=$l7_rc out=$l7_out"

# 7. Eval / JIT / VM / Worker STW GC suite (V1-V9)
ej_out=$("$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" \
    "$SCRIPT_DIR/audit_r18/eval_jit.strada" -o "$r18_dir/eval_jit" 2>&1 \
    && timeout 20 "$r18_dir/eval_jit" 2>&1)
ej_rc=$?
[ "$ej_rc" -eq 0 ] && printf '%s\n' "$ej_out" | grep -qx "AUDIT_R18_EVAL_JIT_OK"
r18_record $? "Cycle 18 Eval / VM / JIT / Worker STW GC suite (V1-V9)" "rc=$ej_rc out=$ej_out"

# 8. Compiled vs bytecode VM 2-way parity (test_audit_r18_vm)
par_src="$PROJECT_DIR/interpreter/test/test_audit_r18_vm.strada"
par_exp=$(cat "$PROJECT_DIR/interpreter/test/test_audit_r18_vm.expected")
p_comp_err=$("$PROJECT_DIR/strada" -O0 "$par_src" -o "$r18_dir/par_comp" 2>&1)
p_comp_rc=$?
if [ "$p_comp_rc" -eq 0 ]; then
    p_comp=$(timeout 20 "$r18_dir/par_comp" 2>&1)
else
    p_comp="$p_comp_err"
fi
p_vm=$(timeout 20 "$PROJECT_DIR/interpreter/strada-interp" "$par_src" 2>&1)
[ "$p_comp_rc" -eq 0 ] && [ "$p_comp" = "$par_exp" ] && [ "$p_vm" = "$par_exp" ]
r18_record $? "Cycle 18 2-way parity (compiled / VM)" "comp='$p_comp' vm='$p_vm'"

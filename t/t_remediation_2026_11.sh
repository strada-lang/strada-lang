#!/bin/bash
# t_remediation_2026_11.sh — End-to-end regression suite for Phases 1–5
# Covers Pacco removal, floored modulo & PerlaCell captures, stdlib fixes,
# Eval.o isolation/fixes, and tree-walk + bytecode VM parity.

rem_dir="$BUILD_DIR/remediation_2026_11"
mkdir -p "$rem_dir"

rem_record() {
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

rem_compile() {
    local src="$1"
    local name="$2"
    local extra_ld="${3:-}"
    (
        cd "$PROJECT_DIR" &&
        BUILD_DIR="$rem_dir" STRADA_TEST_TCC=1 EXTRA_LDFLAGS="${EXTRA_LDFLAGS:-} $extra_ld" compile_strada "$src" "$name"
    )
}

# ============================================================
# Phase 1: Pacco / stradapm removal
# ============================================================
p1_ok=0
p1_diag=""
if [ -e "$PROJECT_DIR/pacco" ]; then
    p1_ok=1; p1_diag="pacco still exists"
elif [ -e "$PROJECT_DIR/tools/stradapm" ]; then
    p1_ok=1; p1_diag="tools/stradapm still exists"
elif grep -Eq '(^|[[:space:]])(pacco|stradapm)([[:space:]:]|$)' "$PROJECT_DIR/Makefile"; then
    p1_ok=1; p1_diag="Makefile still references pacco/stradapm"
fi
rem_record "$p1_ok" "Phase 1: pacco and tools/stradapm removed from tree and Makefile" "$p1_diag"

# ============================================================
# Phase 2-3 & Phase 4 (pure stdlib): modulo, PerlaCell, JSON, JSON::PS,
# LWP, Net::SMTP, Async::Actor, POSIX
# ============================================================
cat > "$rem_dir/phase234_pure.strada" << 'EOF'
use lib "lib";
use JSON;
use JSON::PS;
use LWP;
use Net::SMTP;
use Async::Actor;
use POSIX;

our int $g_fail = 0;

func chk(int $ok, str $label) void {
    if ($ok == 0) {
        say("FAIL: " . $label);
        $g_fail = $g_fail + 1;
    }
}

func main() int {
    # Phase 2-3: Floored modulo & INT64_MIN % -1
    chk((-7 % 3) == 2, "-7 % 3 == 2");
    chk((7 % -3) == -2, "7 % -3 == -2");
    chk((-7 % -3) == -1, "-7 % -3 == -1");
    my int $min = (-9223372036854775807) - 1;
    my int $neg1 = -1;
    chk(($min % $neg1) == 0, "INT64_MIN % -1 == 0");

    # Phase 3: Shared PerlaCell lexical capture across two closures
    my int $count = 0;
    my scalar $inc = func() int { $count = $count + 1; return $count; };
    my scalar $get = func() int { return $count; };
    chk($inc->() == 1 && $get->() == 1 && $inc->() == 2 && $get->() == 2,
        "PerlaCell shared capture across two closures");

    # Phase 4: JSON & JSON::PS
    chk(JSON::is_bool(JSON::true()) == 1, "JSON::is_bool(true) == 1");
    chk(JSON::is_bool(JSON::false()) == 1, "JSON::is_bool(false) == 1");
    chk(JSON::is_bool(1) == 0, "JSON::is_bool(1) == 0");
    chk(JSON::PS::is_bool(JSON::PS::true()) == 1, "JSON::PS::is_bool(true) == 1");
    chk(JSON::PS::is_bool(1) == 0, "JSON::PS::is_bool(1) == 0");

    my str $jenc = JSON::encode({"flag" => JSON::true(), "s" => "123"});
    chk(index($jenc, "\"flag\":true") >= 0 && index($jenc, "\"s\":\"123\"") >= 0,
        "JSON::encode preserves true and quoted numeric string");

    my scalar $jdec = JSON::decode("{\"flag\":true,\"s\":\"123\"}");
    chk(JSON::is_bool($jdec->{"flag"}) == 1 && typeof($jdec->{"s"}) eq "str" && $jdec->{"s"} eq "123",
        "JSON::decode preserves bool and string '123'");

    my scalar $pdec = JSON::PS::decode("{\"flag\":false,\"s\":\"123\"}");
    chk(JSON::PS::is_bool($pdec->{"flag"}) == 1 && typeof($pdec->{"s"}) eq "str" && $pdec->{"s"} eq "123",
        "JSON::PS::decode preserves bool and string '123'");

    # Embedded NUL via \u0000 in key and value
    my scalar $j_nul = JSON::decode("{\"a\\u0000b\":\"x\\u0000y\"}");
    my scalar $p_nul = JSON::PS::decode("{\"a\\u0000b\":\"x\\u0000y\"}");
    my str $expected_nul_k = "a" . core::byte(0) . "b";
    my str $expected_nul_v = "x" . core::byte(0) . "y";
    chk(exists($j_nul, $expected_nul_k) && core::byte_length($j_nul->{$expected_nul_k}) == 3 &&
        $j_nul->{$expected_nul_k} eq $expected_nul_v,
        "JSON::decode \\u0000 in key and value preserves NUL bytes");
    chk(exists($p_nul, $expected_nul_k) && core::byte_length($p_nul->{$expected_nul_k}) == 3 &&
        $p_nul->{$expected_nul_k} eq $expected_nul_v,
        "JSON::PS::decode \\u0000 in key and value preserves NUL bytes");

    # Lone surrogate \uD800 -> UTF-8 U+FFFD (EF BF BD)
    my scalar $j_sur = JSON::decode("\"\\uD800\"");
    my scalar $p_sur = JSON::PS::decode("\"\\uD800\"");
    chk(core::byte_length($j_sur) == 3 && core::get_byte($j_sur, 0) == 239 &&
        core::get_byte($j_sur, 1) == 191 && core::get_byte($j_sur, 2) == 189,
        "JSON::decode lone surrogate -> U+FFFD");
    chk(core::byte_length($p_sur) == 3 && core::get_byte($p_sur, 0) == 239 &&
        core::get_byte($p_sur, 1) == 191 && core::get_byte($p_sur, 2) == 189,
        "JSON::PS::decode lone surrogate -> U+FFFD");

    # decode_strict + last_error
    my int $j_threw = 0;
    try { my scalar $bad = JSON::decode_strict("{bad"); } catch ($e) { $j_threw = 1; }
    chk($j_threw == 1 && length(JSON::last_error()) > 0, "JSON::decode_strict throws & sets last_error");

    my int $p_threw = 0;
    try { my scalar $badp = JSON::PS::decode_strict("{bad"); } catch ($ep) { $p_threw = 1; }
    chk($p_threw == 1 && length(JSON::PS::last_error()) > 0, "JSON::PS::decode_strict throws & sets last_error");

    # LWP (linked purely without -lssl)
    chk(LWP::_decode_chunked("4\r\nWiki\r\n5\r\npedia\r\n0\r\n\r\n") eq "Wikipedia",
        "LWP::_decode_chunked");
    my hash %u = LWP::parse_url("http://[::1]:8080/test");
    chk($u{"host"} eq "::1" && $u{"port"} == 8080 && $u{"path"} eq "/test",
        "LWP::parse_url IPv6 [::1]:8080");
    chk(core::byte_index("caf" . chr(233) . "=1", "=", 0) == 5,
        "core::byte_index with UTF-8 string");

    # Net::SMTP dot_stuff
    my str $stuffed = SMTP::dot_stuff(".hello\n..world\r\nfoo\rbar");
    chk($stuffed eq "..hello\r\n...world\r\nfoo\r\nbar\r\n",
        "SMTP::dot_stuff leading dots & bare CR/LF");
    my str $utf8_msg = "." . chr(233) . "line\n";
    chk(SMTP::dot_stuff($utf8_msg) eq ".." . chr(233) . "line\r\n",
        "SMTP::dot_stuff preserves UTF-8");

    # Async::Actor: handler exception unblocks ask() with undef and sets error()
    my scalar $actor = Async::Actor::new(fn (scalar $msg, scalar $st) scalar {
        if ($msg eq "die") { throw "boom_actor"; }
        return "ok:" . $msg;
    }, {});
    my scalar $ok_r = $actor->ask("hi");
    my scalar $err_r = $actor->ask("die");
    my scalar $err_msg = $actor->error();
    chk($ok_r eq "ok:hi" && !defined($err_r) && defined($err_msg) && index("" . $err_msg, "boom_actor") >= 0,
        "Async::Actor handler throw answers ask() with undef and records error");
    $actor->stop();

    # POSIX OS-accurate constants
    chk(POSIX::SIGINT() == 2 && POSIX::SIGTERM() == 15 && POSIX::ENOENT() == 2 &&
        POSIX::EAGAIN() > 0 && POSIX::O_CREAT() > 0 && POSIX::O_NONBLOCK() > 0 &&
        POSIX::SEEK_SET() == 0 && POSIX::POLLIN() > 0 && POSIX::AF_INET6() > 0 &&
        POSIX::SOCK_STREAM() == 1,
        "POSIX OS constants non-zero/accurate");

    if ($g_fail == 0) {
        say("PHASE234_PURE_OK");
        return 0;
    }
    return 1;
}
EOF

p234_ok=1
p234_diag=""
if rem_compile "$rem_dir/phase234_pure.strada" "phase234_pure" "-lpthread"; then
    p234_out=$(timeout 15 "$rem_dir/phase234_pure" 2>&1)
    if [ "$?" -eq 0 ] && [ "$p234_out" = "PHASE234_PURE_OK" ]; then
        p234_ok=0
    else
        p234_diag="$p234_out"
    fi
else
    p234_diag=$(cat "$rem_dir/phase234_pure_strada.log" "$rem_dir/phase234_pure_gcc.log" 2>/dev/null | head -n 5)
fi
rem_record "$p234_ok" "Phase 2-4 (pure): modulo, PerlaCell closures, JSON/PS, LWP, SMTP, Actor, POSIX" "$p234_diag"

# ============================================================
# Phase 4 (C-ext): DBI typed SQLite + crypt MT + compress limits
# ============================================================
cat > "$rem_dir/phase4_cext.strada" << 'EOF'
use lib "lib";
use DBI;
use crypt;
use compress;

our int $g_fail = 0;

func chk(int $ok, str $label) void {
    if ($ok == 0) {
        say("FAIL: " . $label);
        $g_fail = $g_fail + 1;
    }
}

func crypt_worker(str $pw) int {
    my int $i = 0;
    while ($i < 4) {
        my str $salt = crypt::gen_salt("sha512");
        if (index($salt, "$6$") != 0) { return 0; }
        my str $h = crypt::hashpw($pw, $salt);
        if (length($h) == 0 || crypt::verify($pw, $h) != 1 || crypt::verify($pw . "x", $h) != 0) {
            return 0;
        }
        $i = $i + 1;
    }
    return 1;
}

func main() int {
    # 1. DBI :memory: typed int, float, NULL, embedded-NUL blob
    my scalar $dbh = DBI::connect("dbi:SQLite::memory:", "", "");
    chk(defined($dbh), "DBI::connect SQLite :memory:");
    if (defined($dbh)) {
        DBI::do_sql($dbh, "CREATE TABLE t (i INTEGER, f REAL, n TEXT, b BLOB)");
        my str $blob = "hi" . core::byte(0) . "lo";
        DBI::exec($dbh, "INSERT INTO t (i, f, n, b) VALUES (?, ?, ?, ?)", [42, 3.5, undef, $blob]);
        my scalar $row = DBI::selectrow_hashref($dbh, "SELECT i, f, n, b FROM t", []);
        chk(defined($row), "DBI typed row fetched");
        if (defined($row)) {
            chk(typeof($row->{"i"}) eq "int" && $row->{"i"} == 42, "DBI SQLITE_INTEGER -> int");
            chk(typeof($row->{"f"}) eq "num" && $row->{"f"} == 3.5, "DBI SQLITE_FLOAT -> num");
            chk(!defined($row->{"n"}), "DBI SQLITE_NULL -> undef");
            chk(core::byte_length($row->{"b"}) == 5 && core::get_byte($row->{"b"}, 2) == 0 &&
                core::get_byte($row->{"b"}, 4) == 111, "DBI embedded NUL blob roundtrip");
        }
        DBI::disconnect($dbh);
    }

    # 2. crypt sequential & concurrent threads
    my str $s512 = crypt::gen_salt("sha512");
    chk(index($s512, "$6$") == 0, "crypt::gen_salt sha512");
    my str $hp = crypt::hashpw("hunter2", $s512);
    chk(crypt::verify("hunter2", $hp) == 1 && crypt::verify("wrong", $hp) == 0,
        "crypt::hashpw / verify");

    my scalar $t1 = thread::create(fn () scalar { return crypt_worker("pw_alpha"); });
    my scalar $t2 = thread::create(fn () scalar { return crypt_worker("pw_beta"); });
    my scalar $r1 = thread::join($t1);
    my scalar $r2 = thread::join($t2);
    chk(($r1 + 0) == 1 && ($r2 + 0) == 1, "crypt multi-threaded workers");

    # 3. compress error + >4x gunzip expansion (65536 'A's)
    chk(compress::gunzip("not-gzip") eq "", "compress::gunzip invalid input -> ''");
    my str $big = "A" x 65536;
    my str $big_gz = compress::gzip($big);
    chk(core::byte_length($big_gz) < 2048, "compress::gzip compresses 64KiB well past 4x");
    my str $big_out = compress::gunzip($big_gz);
    chk(core::byte_length($big_out) == 65536 && $big_out eq $big,
        "compress::gunzip handles >4x expansion (64KiB)");

    if ($g_fail == 0) {
        say("PHASE4_CEXT_OK");
        return 0;
    }
    return 1;
}
EOF

p4c_ok=1
p4c_diag=""
if rem_compile "$rem_dir/phase4_cext.strada" "phase4_cext" "-lsqlite3 -lcrypt -lz -lpthread"; then
    p4c_out=$(timeout 15 "$rem_dir/phase4_cext" 2>&1)
    if [ "$?" -eq 0 ] && [ "$p4c_out" = "PHASE4_CEXT_OK" ]; then
        p4c_ok=0
    else
        p4c_diag="$p4c_out"
    fi
else
    p4c_diag=$(cat "$rem_dir/phase4_cext_strada.log" "$rem_dir/phase4_cext_gcc.log" 2>/dev/null | head -n 5)
fi
rem_record "$p4c_ok" "Phase 4 (C-ext): DBI SQLite types/NUL, crypt MT, compress 64KiB >4x gunzip" "$p4c_diag"

# ============================================================
# Phase 5a: lib/Eval.o symbol isolation + use Eval;
# ============================================================
nm_leak=$(nm -g --defined-only "$PROJECT_DIR/lib/Eval.o" | grep -E ' (T|D|B) ' | grep -vE ' (Strada_+Interpreter_+|Eval_+|__c_|g_eval_|__Strada_+Interpreter_+)')
if [ -z "$nm_leak" ]; then
    rem_record 0 "Phase 5: lib/Eval.o exports no leaked lex_/parse_/ast_/NODE_* globals" ""
else
    rem_record 1 "Phase 5: lib/Eval.o exports leaked symbols" "$(echo "$nm_leak" | head -n 5)"
fi

cat > "$rem_dir/eval_checks.strada" << 'EOF'
use lib "lib";
use Eval;

our int $g_fail = 0;

func chk(int $ok, str $label) void {
    if ($ok == 0) {
        say("FAIL: " . $label);
        $g_fail = $g_fail + 1;
    }
}

func main() int {
    # typeof float, refcount, local unwind on throw, short-circuit, compound, const/our
    chk(Strada::Interpreter::eval('typeof(3.14)') eq "num", "Eval typeof(3.14) == 'num'");
    chk(Strada::Interpreter::eval('refcount(42)') >= 1, "Eval refcount(42) >= 1");

    Strada::Interpreter::eval('our str $gv = "orig"; func loc_thrower() void { local $gv = "temp"; throw "boom"; }');
    my scalar $loc_res = Strada::Interpreter::eval('
        try {
            loc_thrower();
        } catch ($e) {
            return $gv;
        }
        return "bad";
    ');
    chk($loc_res eq "orig", "Eval local() unwinds on throw");

    my scalar $sc_res = Strada::Interpreter::eval('
        my int $calls = 0;
        my scalar $bump = func() int { $calls = $calls + 1; return 99; };
        my int $a = 10;
        $a ||= $bump->();
        my scalar $b = 5;
        $b //= $bump->();
        my int $c = 0;
        $c &&= $bump->();
        my scalar $u = undef;
        $u //= 7;
        return $a . ":" . $b . ":" . $c . ":" . $u . ":" . $calls;
    ');
    chk($sc_res eq "10:5:0:7:0", "Eval short-circuit //=, ||=, &&= skip RHS");

    my scalar $cmp_res = Strada::Interpreter::eval('
        my int $p = 2; $p **= 5;
        my int $ba = 6; $ba &= 3;
        my int $bo = 4; $bo |= 3;
        my int $bx = 5; $bx ^= 3;
        my int $sl = 3; $sl <<= 2;
        my int $sr = 16; $sr >>= 2;
        return $p . ":" . $ba . ":" . $bo . ":" . $bx . ":" . $sl . ":" . $sr;
    ');
    chk($cmp_res eq "32:2:7:6:12:4", "Eval compound **=, &=, |=, ^=, <<=, >>=");

    Strada::Interpreter::eval('const int REM_C = 17; our int $REM_OUR = 25;');
    my scalar $top_res = Strada::Interpreter::eval('REM_C + $REM_OUR');
    chk(($top_res + 0) == 42, "Eval top-level const and our");

    if ($g_fail == 0) {
        say("EVAL_REM_OK");
        return 0;
    }
    return 1;
}
EOF

eval_ok=1
eval_diag=""
if rem_compile "$rem_dir/eval_checks.strada" "eval_checks" "-lpthread"; then
    eval_out=$(cd "$PROJECT_DIR" && timeout 15 "$rem_dir/eval_checks" 2>&1)
    if [ "$?" -eq 0 ] && [ "$eval_out" = "EVAL_REM_OK" ]; then
        eval_ok=0
    else
        eval_diag="$eval_out"
    fi
else
    eval_diag=$(cat "$rem_dir/eval_checks_strada.log" "$rem_dir/eval_checks_gcc.log" 2>/dev/null | head -n 5)
fi
rem_record "$eval_ok" "Phase 5: use Eval; typeof, refcount, local unwind, short-circuit, compound, const/our" "$eval_diag"

# ============================================================
# Phase 5b: strada-interp --vm and --tree-walk shared parity
# ============================================================
cat > "$rem_dir/interp_shared.strada" << 'EOF'
func bar_ret() scalar {
    return;
}

func bar_cond() scalar {
    return if 1;
    return 99;
}

func main(int $argc, array @argv) int {
    my num $pi = 3.14;
    say("FLT=" . typeof($pi) . ":" . sprintf("%.4f", 1.0 / 3.0));
    say("MOD=" . (-7 % 3) . ":" . (7 % -3) . ":" . (-7 % -3));
    say("CHR=" . char_at("hello", 0) . ":" . char_at("hello", 99));
    say("BI=" . core::byte_index("caf" . chr(233) . "=1", "=", 0));

    my str $re_s = "10+20";
    $re_s =~ s/(\d+)/$1 * 2/ge;
    say("SGE=" . $re_s);

    say("ARGV=" . size(@ARGV) . ":" . $ARGV[0] . ":" . $argc . ":" . $argv[1] . ":" . $argv[2]);

    my array @nums = (1, 2, 3);
    my array @mapped = map { my int $y = $_ * 2; $y + 1; } @nums;
    my array @grepped = grep { my int $g = $_ + 0; $g > 1; } @nums;
    my array @sorted = sort { my int $x = $a; my int $y = $b; $y <=> $x; } @nums;
    say("MGS=" . join(",", @mapped) . "|" . join(",", @grepped) . "|" . join(",", @sorted));

    my int $c = -7; $c %= 3;
    my int $pw = 2; $pw **= 5;
    my int $ba = 6; $ba &= 3;
    my int $bo = 4; $bo |= 3;
    my int $bx = 5; $bx ^= 3;
    my int $bsl = 3; $bsl <<= 2;
    my int $bsr = 16; $bsr >>= 2;
    my int $sc_hits = 0;
    my scalar $ud = undef; $ud //= 42;
    my scalar $kept = 88; $kept //= ($sc_hits = $sc_hits + 1);
    my int $zv = 0; $zv ||= 7;
    my int $kv = 12; $kv ||= ($sc_hits = $sc_hits + 1);
    my int $tv = 1; $tv &&= 9;
    my int $fv = 0; $fv &&= ($sc_hits = $sc_hits + 1);
    say("CMP=" . $c . ":" . $pw . ":" . $ba . ":" . $bo . ":" . $bx . ":" . $bsl . ":" . $bsr . ":" . $ud . ":" . $kept . ":" . $zv . ":" . $kv . ":" . $tv . ":" . $fv . ":" . $sc_hits);

    my scalar $r1 = bar_ret();
    my scalar $r2 = bar_cond();
    say("RET=" . (defined($r1) ? "D" : "U") . ":" . (defined($r2) ? "D" : "U"));
    return 0;
}
EOF

expected_shared=$(cat << 'EOF'
FLT=num:0.3333
MOD=2:-2:-1
CHR=104:0
BI=5
SGE=20+40
ARGV=2:alpha:3:alpha:beta
MGS=3,5,7|2,3|3,2,1
CMP=2:32:2:7:6:12:4:42:88:7:12:9:0:0
RET=U:U
EOF
)

vm_sh_out=$(timeout 10 "$PROJECT_DIR/interpreter/strada-interp" --vm "$rem_dir/interp_shared.strada" alpha beta 2>&1)
vm_sh_rc=$?
if [ "$vm_sh_rc" -eq 0 ] && [ "$vm_sh_out" = "$expected_shared" ]; then
    rem_record 0 "Phase 5: strada-interp --vm shared parity checks" ""
else
    rem_record 1 "Phase 5: strada-interp --vm shared parity checks" "rc=$vm_sh_rc out=$vm_sh_out"
fi

tw_sh_out=$(timeout 10 "$PROJECT_DIR/interpreter/strada-interp" --tree-walk "$rem_dir/interp_shared.strada" alpha beta 2>&1)
tw_sh_rc=$?
if [ "$tw_sh_rc" -eq 0 ] && [ "$tw_sh_out" = "$expected_shared" ]; then
    rem_record 0 "Phase 5: strada-interp --tree-walk shared parity checks" ""
else
    rem_record 1 "Phase 5: strada-interp --tree-walk shared parity checks" "rc=$tw_sh_rc out=$tw_sh_out"
fi

# VM-specific checks: zero-width s/b*/"X"/ge termination & same-scope sigil disambiguation
cat > "$rem_dir/vm_only.strada" << 'EOF'
func main() int {
    my str $z = "aba";
    $z =~ s/b*/"X"/ge;
    my int $v = 10;
    my array @v = (1, 2, 3);
    if ($z eq "XaXXaX" && $v == 10 && size(@v) == 3) {
        say("VM_ONLY_OK");
        return 0;
    }
    say("VM_ONLY_FAIL:" . $z . ":" . $v . ":" . size(@v));
    return 1;
}
EOF

vm_only_out=$(timeout 5 "$PROJECT_DIR/interpreter/strada-interp" --vm "$rem_dir/vm_only.strada" 2>&1)
vm_only_rc=$?
if [ "$vm_only_rc" -eq 0 ] && [ "$vm_only_out" = "VM_ONLY_OK" ]; then
    rem_record 0 "Phase 5: strada-interp --vm zero-width s///ge termination & sigil disambiguation" ""
else
    rem_record 1 "Phase 5: strada-interp --vm zero-width s///ge termination & sigil disambiguation" "rc=$vm_only_rc out=$vm_only_out"
fi

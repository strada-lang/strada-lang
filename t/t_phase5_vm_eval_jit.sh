#!/bin/bash
# t_phase5_vm_eval_jit.sh - Phase 5A+5B regression tests for VM, tree-walk, Eval, and Strada::JIT

p5_dir="$BUILD_DIR/phase5_vm_eval_jit"
mkdir -p "$p5_dir"

INTERP="$PROJECT_DIR/interpreter/strada-interp"

# 1. In-process Eval & Strada::JIT test binary
TOTAL=$((TOTAL + 1))
p5_bin="$p5_dir/p5_inproc"
p5_out=$("$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$SCRIPT_DIR/t_phase5_vm_eval_jit.strada" -o "$p5_bin" 2>&1 \
    && "$p5_bin" 2>&1)
p5_rc=$?
p5_jit_dir=$(printf '%s\n' "$p5_out" | sed -n 's/^JIT_TMPDIR_END=//p' | tail -n 1)
if [ "$p5_rc" -eq 0 ] && printf '%s\n' "$p5_out" | grep -qx "PHASE5_INPROC_OK" \
    && [ -n "$p5_jit_dir" ] && [ ! -d "$p5_jit_dir" ]; then
    PASSED=$((PASSED + 1))
    log_pass "phase5 in-process Eval and Strada::JIT regressions + END tmpdir cleanup"
else
    FAILED=$((FAILED + 1))
    log_fail "phase5 in-process Eval/JIT" "rc=$p5_rc dir='$p5_jit_dir' out=$p5_out"
fi

# 2. Parity test script run under BOTH --vm and --tree-walk
cat > "$p5_dir/parity.strada" << 'STRADA_END'
BEGIN {
    our int $boot_v = 40;
}
our int $boot_v = 0;

END {
    say("end_first:" . $boot_v);
}

END {
    say("end_second");
    throw "ignored_end_exception";
}

func make_deep() scalar {
    my int $shared = 10;
    my scalar $mid = func() array {
        my scalar $inc = func(int $d) int { $shared = $shared + $d; return $shared; };
        my scalar $read = func() int { return $shared; };
        return [$inc, $read];
    };
    return $mid->();
}

func main() int {
    # Transitive 3-level closure mutation across sibling inner closures
    my array $pair = make_deep();
    my scalar $inc = $pair->[0];
    my scalar $read = $pair->[1];
    my int $a = $inc->(5);
    my int $b = $read->();

    # Self-referencing cyclic closure (for leak-free GC)
    my scalar $cyc = undef;
    $cyc = func(int $n) int { if ($n <= 0) { return 0; } return 1 + $cyc->($n - 1); };
    my int $cr = $cyc->(3);

    # Block-scoped local restored on if-block, last/next, and BEFORE catch
    our int $g = 10;
    if (1) { local $g = 22; }
    my int $g1 = $g;
    my int $li = 0;
    while ($li < 2) {
        local $g = 33;
        $li = $li + 1;
        if ($li == 1) { next; }
        last;
    }
    my int $g2 = $g;
    my int $seen_catch = -1;
    try {
        local $g = 99;
        throw "boom";
    } catch ($e) {
        $seen_catch = $g;
    }

    # core::open_str read & write + core::str_from_fh
    my str $buf = "";
    my scalar $wfh = core::open_str(\$buf, "w");
    say($wfh, "alpha");
    say($wfh, "beta");
    my str $got_str = core::str_from_fh($wfh);
    core::close($wfh);
    my scalar $rfh = core::open_str("one\ntwo", "r");
    my str $l1 = <$rfh>;
    my str $l2 = <$rfh>;
    core::close($rfh);
    chomp($l1);
    chomp($l2);

    # sys::* aliases
    my str $b64 = sys::base64_encode("hi");
    my str $b64d = sys::base64_decode($b64);
    my int $bl = sys::byte_length("abc");

    say("parity:" . $a . "," . $b . "," . $cr . "," . $g1 . "," . $g2 . "," . $seen_catch . "," . $g . "|" . $l1 . "," . $l2 . "|" . $b64 . "," . $b64d . "," . $bl . "|len=" . length($got_str));
    return 7;
}
STRADA_END

p5_parity_line="parity:15,15,3,10,10,10,10|one,two|aGk=,hi,3|len=11"

for mode in --vm --tree-walk; do
    TOTAL=$((TOTAL + 1))
    mode_out=$("$INTERP" "$mode" "$p5_dir/parity.strada" 2>&1)
    mode_rc=$?
    if [ "$mode_rc" -eq 7 ] \
        && printf '%s\n' "$mode_out" | grep -qx "$p5_parity_line" \
        && printf '%s\n' "$mode_out" | grep -qx "end_second" \
        && printf '%s\n' "$mode_out" | grep -qx "end_first:40"; then
        PASSED=$((PASSED + 1))
        log_pass "phase5 interpreter $mode parity (closures, local, END exit code/LIFO/isolation, open_str, sys::*)"
    else
        FAILED=$((FAILED + 1))
        log_fail "phase5 interpreter $mode parity" "rc=$mode_rc out=$mode_out"
    fi
done

# 3. Out-of-tree /tmp cwd import_lib & import_lib_oop under both --vm and --tree-walk
for mode in --vm --tree-walk; do
    for tst in test_import_lib test_import_lib_oop; do
        TOTAL=$((TOTAL + 1))
        oot_out=$(cd /tmp && "$INTERP" "$mode" "$PROJECT_DIR/interpreter/test/${tst}.strada" 2>&1)
        oot_rc=$?
        oot_exp=$(cat "$PROJECT_DIR/interpreter/test/${tst}.expected")
        if [ "$oot_rc" -eq 0 ] && [ "$oot_out" = "$oot_exp" ]; then
            PASSED=$((PASSED + 1))
            log_pass "phase5 /tmp cwd $mode $tst"
        else
            FAILED=$((FAILED + 1))
            log_fail "phase5 /tmp cwd $mode $tst" "rc=$oot_rc out=$oot_out"
        fi
    done
done

# 4. -e mode and eval_string_vm (HAVE_VM path)
TOTAL=$((TOTAL + 1))
e_vm_out=$("$INTERP" --vm -e 'BEGIN { our int $x = 40; } our int $x = 0; END { say($x + 2); }' 2>&1)
e_vm_rc=$?
e_tw_out=$("$INTERP" --tree-walk -e 'BEGIN { our int $x = 40; } our int $x = 0; END { say($x + 2); }' 2>&1)
e_tw_rc=$?
cat > "$p5_dir/esvm.strada" << 'ESVM_END'
use lib "lib";
use Eval;
func main() int {
    my scalar $r = Strada::Interpreter::eval_string_vm('func main() int { return 42; } END { say("vm_end"); }');
    say("res=" . $r);
    return 0;
}
ESVM_END
e_esvm_build=$("$PROJECT_DIR/strada" -O0 --no-use-artifacts -L "$PROJECT_DIR/lib" -D HAVE_VM -I "$PROJECT_DIR/interpreter" "$p5_dir/esvm.strada" "$PROJECT_DIR/interpreter/vm.c" "$PROJECT_DIR/interpreter/vm_compiler.c" -o "$p5_dir/esvm_bin" 2>&1)
e_esvm_brc=$?
if [ "$e_esvm_brc" -eq 0 ]; then
    e_esvm_out=$("$p5_dir/esvm_bin" 2>&1)
    e_esvm_rc=$?
else
    e_esvm_out="$e_esvm_build"
    e_esvm_rc=$e_esvm_brc
fi
if [ "$e_vm_rc" -eq 0 ] && [ "$e_vm_out" = "42" ] \
    && [ "$e_tw_rc" -eq 0 ] && [ "$e_tw_out" = "42" ] \
    && [ "$e_esvm_rc" -eq 0 ] && [ "$e_esvm_out" = "$(printf 'vm_end\nres=42')" ]; then
    PASSED=$((PASSED + 1))
    log_pass "phase5 -e mode (--vm, --tree-walk) and eval_string_vm"
else
    FAILED=$((FAILED + 1))
    log_fail "phase5 -e / eval_string_vm" "vm=($e_vm_rc:$e_vm_out) tw=($e_tw_rc:$e_tw_out) esvm=($e_esvm_rc:$e_esvm_out)"
fi

# 5. Valgrind 0-leak check on transitive + cyclic closures + local + open_str
if command -v valgrind >/dev/null 2>&1; then
    for mode in --vm --tree-walk; do
        TOTAL=$((TOTAL + 1))
        vg_out=$(valgrind --leak-check=full --errors-for-leak-kinds=definite,indirect --error-exitcode=99 "$INTERP" "$mode" "$p5_dir/parity.strada" 2>&1)
        vg_rc=$?
        def_lost=$(printf '%s\n' "$vg_out" | awk '/definitely lost:/ { gsub(/,/, "", $4); print $4 }')
        ind_lost=$(printf '%s\n' "$vg_out" | awk '/indirectly lost:/ { gsub(/,/, "", $4); print $4 }')
        if [ "$vg_rc" -eq 7 ] && [ "${def_lost:-0}" = "0" ] && [ "${ind_lost:-0}" = "0" ]; then
            PASSED=$((PASSED + 1))
            log_pass "phase5 valgrind 0-leak check ($mode parity script)"
        else
            FAILED=$((FAILED + 1))
            log_fail "phase5 valgrind check ($mode)" "rc=$vg_rc def=$def_lost ind=$ind_lost"
        fi
    done
fi

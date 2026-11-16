#!/bin/bash
# t_audit_r12.sh - Cycle 12 regression tests across compiler, runtime, stdlib, CLI, and VM/Eval/JIT

r12_dir="$BUILD_DIR/audit_r12"
mkdir -p "$r12_dir"

r12_record() {
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

# 1. Compiler + Runtime core suite (-O0 by default, plus -O2 under --default)
for opt in ${STRADA_TEST_AUDIT_OPTS:--O0}; do
    out=$("$PROJECT_DIR/strada" "$opt" "$SCRIPT_DIR/audit_r12/compiler_runtime.strada" -o "$r12_dir/comp_${opt}" 2>&1 \
        && "$r12_dir/comp_${opt}" 2>&1)
    rc=$?
    [ "$rc" -eq 0 ] && printf '%s\n' "$out" | grep -qx "AUDIT_R12_COMPILER_RUNTIME_OK"
    r12_record $? "Cycle 12 Compiler & Runtime suite ($opt)" "rc=$rc out=$out"
done

# 2. R12-C8: Cyclic inheritance must not hang compiler devirtualization
cat > "$r12_dir/cyc_isa.strada" << 'EOF'
package CycA;
extends CycB;
package CycB;
extends CycA;
package main;
func main() int {
    my scalar $x = bless({}, "CycA");
    say("CYC_OK:" . $x->isa("CycB"));
    return 0;
}
EOF
cyc_out=$(timeout 10 "$PROJECT_DIR/stradac" "$r12_dir/cyc_isa.strada" "$r12_dir/cyc_isa.c" 2>&1)
cyc_rc=$?
[ "$cyc_rc" -ne 124 ]
r12_record $? "R12-C8 cyclic inheritance terminates without compiler hang" "rc=$cyc_rc out=$cyc_out"

# 3. R12-C12: --split-dir with import_lib (.so) external functions across shards
cat > "$r12_dir/SplitExt.strada" << 'EOF'
package SplitExt;
func add10(int $x) int { return $x + 10; }
func mul2(int $x) int { return $x * 2; }
EOF
cat > "$r12_dir/split_use_ext.strada" << 'EOF'
use lib "__R12_DIR__";
import_lib "SplitExt.so";
func h01(int $x) int { return SplitExt::add10($x); }
func h02(int $x) int { return SplitExt::mul2($x); }
func main() int {
    my int $r = h01(11) + h02(10);
    say("SPLIT_LIB=" . $r);
    return 0;
}
EOF
sed -i "s|__R12_DIR__|$r12_dir|g" "$r12_dir/split_use_ext.strada"
mkdir -p "$r12_dir/shards"
split_out=$("$PROJECT_DIR/strada" --shared -O0 "$r12_dir/SplitExt.strada" -o "$r12_dir/SplitExt.so" 2>&1 \
    && "$PROJECT_DIR/stradac" --split-dir "$r12_dir/shards" "$r12_dir/split_use_ext.strada" "$r12_dir/split_use_ext.c" 2>&1 \
    && { if [ -f "$r12_dir/shards/shards.txt" ]; then gcc -O0 -rdynamic -Wl,--gc-sections -I"$PROJECT_DIR/runtime" "$r12_dir/split_use_ext.c" "$r12_dir/shards"/shard_*.c "$r12_dir/SplitExt.so" "$PROJECT_DIR/runtime/strada_runtime.o" -Wl,-rpath,"$r12_dir" -lm -ldl -lpthread $EXTRA_LDFLAGS -o "$r12_dir/split_ext_bin" 2>&1; else gcc -O0 -rdynamic -Wl,--gc-sections -I"$PROJECT_DIR/runtime" "$r12_dir/split_use_ext.c" "$r12_dir/SplitExt.so" "$PROJECT_DIR/runtime/strada_runtime.o" -Wl,-rpath,"$r12_dir" -lm -ldl -lpthread $EXTRA_LDFLAGS -o "$r12_dir/split_ext_bin" 2>&1; fi; } \
    && "$r12_dir/split_ext_bin" 2>&1)
split_rc=$?
[ "$split_rc" -eq 0 ] && printf '%s\n' "$split_out" | grep -qx "SPLIT_LIB=41"
r12_record $? "R12-C12 --split-dir links import_lib .so cleanly" "rc=$split_rc out=$split_out"

# 3b. R12-C2: stradapp does not treat $__C__ / $obj->__C__ as __C__ blocks
cat > "$r12_dir/pp_c_ident.strada" << 'EOF'
my int $__C__ = 7;
my scalar $s = $obj->__C__();
EOF
pp_out=$("$PROJECT_DIR/tools/stradapp" "$r12_dir/pp_c_ident.strada" 2>&1)
pp_rc=$?
[ "$pp_rc" -eq 0 ] && printf '%s\n' "$pp_out" | grep -qF 'my int $__C__ = 7;' && printf '%s\n' "$pp_out" | grep -qF '$obj->__C__()'
r12_record $? "R12-C2 stradapp preserves \$__C__ and ->__C__ identifiers" "rc=$pp_rc out=$pp_out"

# 4. R12-C13: Separate-compilation OOP (.so with '::' in name, multi-package .o, and .a archive)
mkdir -p "$r12_dir/OopSep"
cat > "$r12_dir/OopSep/Mod.strada" << 'EOF'
package OopSep::Mod;
has ro int $x = 5;
func get_x(scalar $self) int { return $self->x() + 10; }
EOF
cat > "$r12_dir/MultiObj.strada" << 'EOF'
package MultiA;
has ro int $a = 2;
func get_a(scalar $self) int { return $self->a(); }
package MultiB;
extends MultiA;
func get_ab(scalar $self) int { return $self->a() + 3; }
EOF
cat > "$r12_dir/ArchMod.strada" << 'EOF'
package ArchMod::Box;
has ro int $v = 9;
func get_v(scalar $self) int { return $self->v() + 1; }
EOF
cat > "$r12_dir/use_sep_o.strada" << 'EOF'
use lib "__R12_DIR__";
import_lib "OopSep/Mod.so";
import_object "MultiObj.o";
func main() int {
    my scalar $m = OopSep::Mod::new("x", 7);
    my scalar $b = MultiB::new("a", 4);
    say("SEP_O=" . $m->get_x() . "," . $b->get_ab());
    return 0;
}
EOF
cat > "$r12_dir/use_sep_a.strada" << 'EOF'
use lib "__R12_DIR__";
import_archive "ArchMod.a";
func main() int {
    my scalar $ar = ArchMod::Box::new("v", 20);
    say("SEP_A=" . $ar->get_v());
    return 0;
}
EOF
sed -i "s|__R12_DIR__|$r12_dir|g" "$r12_dir/use_sep_o.strada" "$r12_dir/use_sep_a.strada"
sep_o_out=$("$PROJECT_DIR/strada" --shared -O0 "$r12_dir/OopSep/Mod.strada" -o "$r12_dir/OopSep/Mod.so" 2>&1 \
    && "$PROJECT_DIR/strada" --object-full -O0 "$r12_dir/MultiObj.strada" -o "$r12_dir/MultiObj.o" 2>&1 \
    && "$PROJECT_DIR/strada" -O0 "$r12_dir/use_sep_o.strada" -o "$r12_dir/use_sep_o" 2>&1 \
    && "$r12_dir/use_sep_o" 2>&1)
sep_o_rc=$?
sep_a_out=$("$PROJECT_DIR/strada" --static-lib -O0 "$r12_dir/ArchMod.strada" -o "$r12_dir/ArchMod.a" 2>&1 \
    && "$PROJECT_DIR/strada" -O0 "$r12_dir/use_sep_a.strada" -o "$r12_dir/use_sep_a" 2>&1 \
    && "$r12_dir/use_sep_a" 2>&1)
sep_a_rc=$?
[ "$sep_o_rc" -eq 0 ] && printf '%s\n' "$sep_o_out" | grep -qx "SEP_O=17,7" \
    && [ "$sep_a_rc" -eq 0 ] && printf '%s\n' "$sep_a_out" | grep -qx "SEP_A=21"
r12_record $? "R12-C13 separate-compilation OOP (.so ::, multi-pkg .o, .a)" "o=($sep_o_rc:$sep_o_out) a=($sep_a_rc:$sep_a_out)"

# 5. Stdlib suite (Forma R12-LIB1)
std_out=$("$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$SCRIPT_DIR/audit_r12/stdlib_tools.strada" -o "$r12_dir/stdlib" 2>&1 \
    && "$r12_dir/stdlib" 2>&1)
std_rc=$?
[ "$std_rc" -eq 0 ] && printf '%s\n' "$std_out" | grep -qx "AUDIT_R12_STDLIB_TOOLS_OK"
r12_record $? "Cycle 12 Stdlib suite (Forma safe-mode dump/DUMP)" "rc=$std_rc out=$std_out"

# 6. Eval & eval_string_vm (R12-VM1..17)
ej_out=$("$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" \
    "$SCRIPT_DIR/audit_r12/eval_jit.strada" -o "$r12_dir/eval_jit" 2>&1 \
    && "$r12_dir/eval_jit" 2>&1)
ej_rc=$?
[ "$ej_rc" -eq 0 ] \
    && printf '%s\n' "$ej_out" | grep -qx "VM_EVAL=40|1,2|SAH" \
    && printf '%s\n' "$ej_out" | grep -qx "AUDIT_R12_EVAL_JIT_OK"
r12_record $? "Cycle 12 Eval / eval_string_vm suite (R12-VM1..17)" "rc=$ej_rc out=$ej_out"

# 7. CLI -e R12-VM17 false-positive "func main" in strings/comments (VM)
e_vm=$(timeout 5 "$PROJECT_DIR/interpreter/strada-interp" -e 'my str $s = "func main()"; say($s);' 2>&1)
e_vm_rc=$?
[ "$e_vm_rc" -eq 0 ] && [ "$e_vm" = "func main()" ]
r12_record $? "R12-VM17 -e treats 'func main' inside string literal as expression (VM)" "vm=($e_vm_rc:$e_vm)"

# 8. Compiled vs bytecode VM 2-way parity (test_audit_r12_vm)
par_src="$PROJECT_DIR/interpreter/test/test_audit_r12_vm.strada"
par_exp=$(cat "$PROJECT_DIR/interpreter/test/test_audit_r12_vm.expected")
p_comp=$("$PROJECT_DIR/strada" -O0 "$par_src" -o "$r12_dir/par_comp" 2>&1 && "$r12_dir/par_comp" 2>&1)
p_vm=$("$PROJECT_DIR/interpreter/strada-interp" "$par_src" 2>&1)
[ "$p_comp" = "$par_exp" ] && [ "$p_vm" = "$par_exp" ]
r12_record $? "Cycle 12 2-way parity (compiled / VM)" "comp='$p_comp' vm='$p_vm'"

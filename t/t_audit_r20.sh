#!/bin/bash
# t_audit_r20.sh - Audit Cycle 20 regression checks that need a shell
# (driver/tools behaviour, files, processes). Single-program semantic
# regressions live in t/bugfix_tests/ instead.

r20_dir="$BUILD_DIR/audit_r20"
mkdir -p "$r20_dir"

r20_record() {
    local rc="$1" label="$2" diag="$3"
    TOTAL=$((TOTAL + 1))
    if [ "$rc" -eq 0 ]; then
        PASSED=$((PASSED + 1)); log_pass "$label"
    else
        FAILED=$((FAILED + 1)); log_fail "$label" "$diag"
    fi
}

# T1-7: --clear-module-cache must refuse $HOME however it is spelled.
h="$r20_dir/fakehome"; rm -rf "$h"; mkdir -p "$h/docs"; echo keep > "$h/docs/a.txt"
for spelled in "$h" "$h/" "$h/." "$h//"; do
    HOME="$h" STRADA_MODULE_CACHE_DIR="$spelled" "$PROJECT_DIR/strada" --clear-module-cache > /dev/null 2>&1
done
[ -f "$h/docs/a.txt" ]
r20_record $? "T1-7 --clear-module-cache refuses \$HOME spelled with / or /." "fake HOME was removed"

# T1-8: the build must never overwrite its own inputs.
d="$r20_dir/t1_8"; rm -rf "$d"; mkdir -p "$d"
printf 'func main() int { say("hi"); return 0; }\n' > "$d/a.strada"; cp "$d/a.strada" "$d/b.strada"
printf 'int h(void){return 1;}\n' > "$d/helper.c"
(cd "$d" && "$PROJECT_DIR/strada" -O0 a.strada -o a.strada > /dev/null 2>&1
          "$PROJECT_DIR/strada" -O0 a.strada b.strada > /dev/null 2>&1
          "$PROJECT_DIR/strada" -O0 -c a.strada helper.c -o helper > /dev/null 2>&1)
grep -q 'say("hi")' "$d/a.strada" && grep -q 'say("hi")' "$d/b.strada" && grep -q 'int h(void)' "$d/helper.c"
r20_record $? "T1-8 driver refuses to overwrite input .strada/.c files" "an input file was overwritten"

# S1-16/S1-17/S1-18: Nesso directives vs "_"-prefixed columns (SQLite).
if [ "${STRADA_HAVE_SQLITE:-0}" = "1" ]; then
    out=$("$PROJECT_DIR/strada" -O0 -DHAVE_SQLITE3 -L "$PROJECT_DIR/lib" "$SCRIPT_DIR/audit_r20/nesso_conds.strada" -lsqlite3 -o "$r20_dir/nesso_conds" 2>&1 \
          && (cd "$r20_dir" && timeout 20 ./nesso_conds 2>&1))
    [ "$out" = "$(cat "$SCRIPT_DIR/audit_r20/nesso_conds.expected")" ]
    r20_record $? "S1-16/17/18 Nesso _offset, _-prefixed columns, unsaved related()" "$out"
fi

# V5-16: strada-repl must not run ./strada-jit from the current directory.
d="$r20_dir/v5_16"; rm -rf "$d"; mkdir -p "$d/bin" "$d/cwd"
"$PROJECT_DIR/strada" -O0 "$PROJECT_DIR/tools/strada-repl.strada" -o "$d/bin/strada-repl" > /dev/null 2>&1
printf '#!/bin/sh\ntouch "%s/pwned"\n' "$d" > "$d/cwd/strada-jit"; chmod +x "$d/cwd/strada-jit"
(cd "$d/cwd" && echo "" | PATH="$d/bin:/usr/bin:/bin" timeout 5 strada-repl > /dev/null 2>&1)
[ ! -e "$d/pwned" ]
r20_record $? "V5-16 strada-repl does not exec ./strada-jit from CWD" "planted ./strada-jit ran"

# T1-9 / V4-4: stradac and strada-interp fail on a missing input, an
# unwritable output, or a program without main (they used to exit 0).
d="$r20_dir/t1_9"; rm -rf "$d"; mkdir -p "$d"
printf 'func main() int { return 0; }\n' > "$d/ok.strada"
printf 'func helper() int { return 1; }\n' > "$d/nomain.strada"
rcs=""
"$PROJECT_DIR/stradac" "$d/missing.strada" "$d/out.c" > /dev/null 2>&1; rcs="$rcs $?"
"$PROJECT_DIR/stradac" --check "$d/missing.strada" > /dev/null 2>&1; rcs="$rcs $?"
"$PROJECT_DIR/stradac" "$d/ok.strada" /nonexistent_dir_r20/out.c > /dev/null 2>&1; rcs="$rcs $?"
if [ -x "$PROJECT_DIR/interpreter/strada-interp" ]; then
    "$PROJECT_DIR/interpreter/strada-interp" "$d/missing.strada" > /dev/null 2>&1; rcs="$rcs $?"
    "$PROJECT_DIR/interpreter/strada-interp" "$d/nomain.strada" > /dev/null 2>&1; rcs="$rcs $?"
fi
case " $rcs " in *" 0 "*) false ;; *) true ;; esac
r20_record $? "T1-9/V4-4 stradac/strada-interp exit non-zero on missing input/unwritable output/no main" "rcs:$rcs"

# T1-10: doc/profile tools fail when the output cannot be written.
printf '# T\n\ntext\n' > "$d/x.md"
tr=""
if [ -x "$PROJECT_DIR/tools/strada-md2html" ]; then "$PROJECT_DIR/tools/strada-md2html" "$d/x.md" /nonexistent_dir_r20/x.html > /dev/null 2>&1; tr="$tr $?"; fi
if [ -x "$PROJECT_DIR/tools/strada-md2man" ]; then "$PROJECT_DIR/tools/strada-md2man" "$d/x.md" /nonexistent_dir_r20/x.1 > /dev/null 2>&1; tr="$tr $?"; fi
case " $tr " in *" 0 "*) false ;; *) true ;; esac
r20_record $? "T1-10 md2html/md2man report write failures" "rcs:$tr"

# S3-12/S3-13: perl5 loads XS modules and survives init after shutdown.
if perl -MExtUtils::Embed -e ccopts > /dev/null 2>&1; then
    p5f=$(perl -MExtUtils::Embed -e ccopts -e ldopts 2>/dev/null)
    cat > "$d/p5.strada" << 'P5EOF'
use lib "lib";
use perl5;
func main() int {
    perl5::init();
    my str $a = "" . perl5::eval("use List::Util qw(sum); sum(1,2,3)");
    perl5::shutdown();
    perl5::init();
    my str $b = "" . perl5::eval("40 + 2");
    perl5::shutdown();
    say($a . ":" . $b);
    return 0;
}
P5EOF
    out=$("$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$d/p5.strada" $p5f -o "$d/p5" > "$d/p5.log" 2>&1 && timeout 20 "$d/p5" 2>&1)
    [ "$out" = "6:42" ]
    r20_record $? "S3-12/S3-13 perl5 XS modules + re-init after shutdown" "$out"
fi

# Bootstrap: $lt/$gt/$eq/$ne/$le/$ge are ordinary variable names (the frozen
# bootstrap lexed them as the string operators and failed to parse).
d="$r20_dir/boot_kw"; rm -rf "$d"; mkdir -p "$d"
printf 'func main() int {\n my int $lt = 1;\n my int $gt = 2;\n my int $eq = 3;\n my int $ne = 4;\n my int $le = 5;\n my int $ge = 6;\n my str $s = "a";\n if ($s lt "b") { say($lt + $gt + $eq + $ne + $le + $ge); }\n return 0;\n}\n' > "$d/kw.strada"
out=$("$PROJECT_DIR/bootstrap/stradac" "$d/kw.strada" "$d/kw.c" > "$d/boot.log" 2>&1 \
      && $GCC -o "$d/kw" "$d/kw.c" "$PROJECT_DIR/runtime/strada_runtime.o" -I"$PROJECT_DIR/runtime" -ldl -lm -lpthread ${EXTRA_LDFLAGS:-} > "$d/gcc.log" 2>&1 \
      && "$d/kw" 2>&1)
[ "$out" = "21" ]
r20_record $? "bootstrap accepts \$lt/\$gt/\$eq/\$ne/\$le/\$ge variables" "$out $(cat "$d/boot.log" | grep -i error)"

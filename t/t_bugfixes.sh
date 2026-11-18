#!/bin/bash
# t_bugfixes.sh — one small program per fixed bug, in t/bugfix_tests/.
#
# Each file states what it expects in its first comment line:
#   # EXPECT: text            stdout must equal text exactly
#   # EXPECT-STDERR: text     the program must fail and its output contain text
#   # COMPILE-ERROR: text     `stradac --check` must fail with a message containing text
#                             (optional `# VM-COMPILE-ERROR: text`: strada-interp must fail too)
# The comment below that line says what used to go wrong.
#
# EXPECT tests also run through the bytecode VM (interpreter/strada-interp)
# and must print the same line, unless the file has a `# VM-SKIP: reason`
# line among its first comment lines.

for bf_src in "$SCRIPT_DIR"/bugfix_tests/*.strada; do
    [ -f "$bf_src" ] || continue
    bf_name="bugfix_$(basename "$bf_src" .strada)"
    bf_desc="bugfix: $(basename "$bf_src" .strada)"
    bf_head=$(head -1 "$bf_src")
    TOTAL=$((TOTAL + 1))

    case "$bf_head" in
        "# COMPILE-ERROR: "*)
            bf_want="${bf_head#\# COMPILE-ERROR: }"
            bf_out=$("$STRADAC" --check "$bf_src" 2>&1)
            if [ $? -ne 0 ] && echo "$bf_out" | grep -qF "$bf_want"; then
                PASSED=$((PASSED + 1)); log_pass "$bf_desc"
            else
                FAILED=$((FAILED + 1)); log_fail "$bf_desc" "expected a compile error containing: $bf_want"
            fi
            continue
            ;;
    esac

    if ! compile_strada "$bf_src" "$bf_name"; then
        FAILED=$((FAILED + 1))
        log_fail "$bf_desc" "Compile failed: $(head -1 "$BUILD_DIR/${bf_name}_strada.log" 2>/dev/null) $(grep -m1 'error' "$BUILD_DIR/${bf_name}_gcc.log" 2>/dev/null)"
        continue
    fi
    run_program "$bf_name" 10
    bf_rc=$?
    bf_got=$(cat "$BUILD_DIR/${bf_name}.out" 2>/dev/null)

    case "$bf_head" in
        "# EXPECT-STDERR: "*)
            bf_want="${bf_head#\# EXPECT-STDERR: }"
            if [ $bf_rc -ne 0 ] && [ $bf_rc -ne 124 ] && echo "$bf_got" | grep -qF "$bf_want"; then
                PASSED=$((PASSED + 1)); log_pass "$bf_desc"
            else
                FAILED=$((FAILED + 1)); log_fail "$bf_desc" "exit $bf_rc; expected a failure mentioning: $bf_want"
            fi
            ;;
        "# EXPECT: "*|"# EXPECT:")
            bf_want="${bf_head#\# EXPECT: }"
            if [ $bf_rc -eq 0 ] && [ "$bf_got" == "$bf_want" ]; then
                PASSED=$((PASSED + 1)); log_pass "$bf_desc"
            else
                FAILED=$((FAILED + 1)); log_fail "$bf_desc" "exit $bf_rc; got: $(echo "$bf_got" | head -2 | tr '\n' '|')"
            fi
            ;;
        *)
            FAILED=$((FAILED + 1)); log_fail "$bf_desc" "no EXPECT line at the top of $bf_src"
            ;;
    esac
done

# Same EXPECT tests through the bytecode VM
BF_INTERP="$PROJECT_DIR/interpreter/strada-interp"
if [ -x "$BF_INTERP" ]; then
    for bf_src in "$SCRIPT_DIR"/bugfix_tests/*.strada; do
        [ -f "$bf_src" ] || continue
        bf_head=$(head -1 "$bf_src")
        # `# VM-COMPILE-ERROR: text` (in the first 5 lines of a COMPILE-ERROR
        # test): the VM must also reject the program, saying text
        bf_vmce=$(head -5 "$bf_src" | grep -m1 "^# VM-COMPILE-ERROR: " | sed 's/^# VM-COMPILE-ERROR: //')
        if [ -n "$bf_vmce" ]; then
            bf_desc="bugfix-vm: $(basename "$bf_src" .strada)"
            TOTAL=$((TOTAL + 1))
            bf_got=$(cd "$PROJECT_DIR" && timeout 30 "$BF_INTERP" "$bf_src" 2>&1)
            bf_rc=$?
            if [ $bf_rc -ne 0 ] && echo "$bf_got" | grep -qF -- "$bf_vmce"; then
                PASSED=$((PASSED + 1)); log_pass "$bf_desc"
            else
                FAILED=$((FAILED + 1)); log_fail "$bf_desc" "exit $bf_rc; wanted a failure with: $bf_vmce; got: $(echo "$bf_got" | head -2 | tr '\n' '|')"
            fi
            continue
        fi
        case "$bf_head" in "# EXPECT: "*|"# EXPECT:") ;; *) continue ;; esac
        if head -5 "$bf_src" | grep -q "^# VM-SKIP:"; then continue; fi
        bf_desc="bugfix-vm: $(basename "$bf_src" .strada)"
        bf_want="${bf_head#\# EXPECT: }"
        TOTAL=$((TOTAL + 1))
        bf_got=$(cd "$PROJECT_DIR" && timeout 30 "$BF_INTERP" "$bf_src" 2>&1)
        bf_rc=$?
        if [ $bf_rc -eq 0 ] && [ "$bf_got" == "$bf_want" ]; then
            PASSED=$((PASSED + 1)); log_pass "$bf_desc"
        else
            FAILED=$((FAILED + 1)); log_fail "$bf_desc" "exit $bf_rc; got: $(echo "$bf_got" | head -2 | tr '\n' '|')"
        fi
    done
fi

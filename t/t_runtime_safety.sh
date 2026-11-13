#!/bin/bash
# Runtime safety tests (Phase 2)

out_file="$BUILD_DIR/t_runtime_safety"
TOTAL=$((TOTAL + 1))
if ! "$PROJECT_DIR/strada" "$SCRIPT_DIR/t_runtime_safety.strada" -o "$out_file" >"$BUILD_DIR/t_runtime_safety_compile.log" 2>&1; then
    FAILED=$((FAILED + 1))
    log_fail "t_runtime_safety compile" "$(cat "$BUILD_DIR/t_runtime_safety_compile.log" | head -3)"
else
    res=$("$out_file" 2>&1)
    if [ "$res" = "RUNTIME_SAFETY_OK" ]; then
        PASSED=$((PASSED + 1))
        log_pass "t_runtime_safety"
    else
        FAILED=$((FAILED + 1))
        log_fail "t_runtime_safety" "unexpected output: $res"
    fi
fi

# Verify TCC runtime compilation + execution if tcc is available
TOTAL=$((TOTAL + 1))
if command -v tcc >/dev/null 2>&1; then
    tcc_bin="$BUILD_DIR/t_runtime_safety_tcc"
    if "$PROJECT_DIR/strada" --tcc "$SCRIPT_DIR/t_runtime_safety.strada" -o "$tcc_bin" >"$BUILD_DIR/t_runtime_safety_tcc.log" 2>&1; then
        tcc_res=$("$tcc_bin" 2>&1)
        if [ "$tcc_res" = "RUNTIME_SAFETY_OK" ]; then
            PASSED=$((PASSED + 1))
            log_pass "t_runtime_safety (--tcc)"
        else
            FAILED=$((FAILED + 1))
            log_fail "t_runtime_safety (--tcc)" "unexpected output: $tcc_res"
        fi
    else
        FAILED=$((FAILED + 1))
        log_fail "t_runtime_safety (--tcc) compile" "$(cat "$BUILD_DIR/t_runtime_safety_tcc.log" | head -3)"
    fi
else
    SKIPPED=$((SKIPPED + 1))
    log_skip "t_runtime_safety (--tcc)" "tcc not installed"
fi

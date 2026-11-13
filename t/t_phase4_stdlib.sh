#!/bin/bash
# t_phase4_stdlib.sh - Phase 4 regression tests for stdlib and C extensions

p4_dir="$BUILD_DIR/phase4_stdlib"
mkdir -p "$p4_dir"

p4_port=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1]); s.close()')

p4_out=$("$PROJECT_DIR/strada" -O0 -L "$PROJECT_DIR/lib" "$SCRIPT_DIR/t_phase4_stdlib.strada" -o "$p4_dir/p4_bin" 2>&1 \
    && "$p4_dir/p4_bin" "$p4_port" 2>&1 \
    && "$p4_dir/p4_bin" --getopt-go1 --flag=0 2>&1 \
    && "$p4_dir/p4_bin" --getopt-go2 --count=abc 2>&1 \
    && "$p4_dir/p4_bin" --getopt-go3 --name --other 2>&1)
p4_rc=$?

TOTAL=$((TOTAL + 1))
p4_exp=$(printf "PHASE4_STDLIB_OK\nGO1_OK\nOption --count requires an integer argument\nGO2_OK\nOption --name requires an argument\nGO3_OK")
if [ "$p4_rc" -eq 0 ] && [ "$p4_out" = "$p4_exp" ]; then
    PASSED=$((PASSED + 1))
    log_pass "phase4 stdlib regressions (LWP, SMTP, Async, Nesso, Markdown, Forma, JSON/PS, YAML, CSV, Getopt, compress, BigInt/DateTime)"
else
    FAILED=$((FAILED + 1))
    log_fail "phase4 stdlib regressions" "rc=$p4_rc out=$p4_out"
fi

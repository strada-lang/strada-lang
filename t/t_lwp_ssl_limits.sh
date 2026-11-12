#!/bin/bash
# t_lwp_ssl_limits.sh - LWP_SSL HTTPS regression test for max_header_bytes,
# max_response_bytes, and binary response bodies over verified TLS.
if [ -f "$PROJECT_DIR/lib/ssl.o" ] && command -v openssl >/dev/null 2>&1; then
    lsl_dir="$BUILD_DIR/lwp_ssl_limits"; mkdir -p "$lsl_dir"
    lsl_port=$(( 22000 + (${BASHPID:-$$} % 7000) ))
    TOTAL=$((TOTAL + 1))
    openssl req -x509 -newkey rsa:2048 -nodes -keyout "$lsl_dir/key.pem" -out "$lsl_dir/cert.pem" \
        -days 1 -subj "/CN=localhost" -addext "subjectAltName=DNS:localhost,IP:127.0.0.1" >/dev/null 2>&1 \
    || openssl req -x509 -newkey rsa:2048 -nodes -keyout "$lsl_dir/key.pem" -out "$lsl_dir/cert.pem" \
        -days 1 -subj "/CN=localhost" >/dev/null 2>&1
    lsl_out=""
    if "$PROJECT_DIR/strada" "$PROJECT_DIR/t/lwp_ssl_limits_test/https_limits.strada" -o "$lsl_dir/https_limits" \
           -lssl -lcrypto > "$lsl_dir/build.log" 2>&1; then
        lsl_out=$(SSL_CERT_FILE="$lsl_dir/cert.pem" timeout 30 "$lsl_dir/https_limits" \
                  "$lsl_port" "$lsl_dir/cert.pem" "$lsl_dir/key.pem" 2>&1)
    fi
    if [ "$lsl_out" = "lwp_ssl https limits ok" ]; then
        PASSED=$((PASSED + 1)); log_pass "LWP_SSL: HTTPS max_header_bytes, max_response_bytes, and binary body"
    else
        FAILED=$((FAILED + 1)); log_fail "LWP_SSL: HTTPS max_header_bytes, max_response_bytes, and binary body" \
            "got: $lsl_out $(head -c 300 "$lsl_dir/build.log" 2>/dev/null)"
    fi
else
    test_skip "LWP_SSL: HTTPS max_header_bytes and max_response_bytes" "ssl library or openssl not available"
fi

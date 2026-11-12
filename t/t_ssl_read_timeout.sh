#!/bin/bash
# t_ssl_read_timeout.sh - ssl::read / ssl::readline on a non-blocking TLS
# connection whose peer goes silent must return "" in bounded time instead of
# retrying SSL_ERROR_WANT_READ forever; the wait limit is configurable with
# ssl::set_timeout / ssl::set_default_timeout. Needs the ssl library and openssl.
if [ -f "$PROJECT_DIR/lib/ssl.o" ] && command -v openssl >/dev/null 2>&1; then
    srt_dir="$BUILD_DIR/ssl_read_timeout"; mkdir -p "$srt_dir"
    srt_port=$(( 21000 + RANDOM % 9000 ))
    TOTAL=$((TOTAL + 1))
    openssl req -x509 -newkey rsa:2048 -nodes -keyout "$srt_dir/key.pem" -out "$srt_dir/cert.pem" \
        -days 1 -subj "/CN=localhost" >/dev/null 2>&1
    srt_out=""
    if "$PROJECT_DIR/strada" "$PROJECT_DIR/t/ssl_read_timeout_test/silent_peer.strada" -o "$srt_dir/silent_peer" \
           -lssl -lcrypto > "$srt_dir/build.log" 2>&1; then
        srt_out=$(timeout 40 "$srt_dir/silent_peer" "$srt_port" "$srt_dir/cert.pem" "$srt_dir/key.pem" \
                  "$((srt_port + 1))" 2>&1)
    fi
    srt_want=$(printf 'first ok\ndefault was 5000, silent read ~1s 1\nset_timeout 2000, silent read ~2s 1\nset_timeout 0, immediate 1\nreadline empty 1 bounded 1')
    if [ "$srt_out" = "$srt_want" ]; then
        PASSED=$((PASSED + 1)); log_pass "ssl: read/readline on a silent non-blocking peer return in bounded, configurable time"
    else
        FAILED=$((FAILED + 1)); log_fail "ssl: read/readline on a silent peer" \
            "got: $srt_out $(head -c 300 "$srt_dir/build.log" 2>/dev/null)"
    fi
else
    test_skip "ssl: read/readline on a silent peer" "ssl library or openssl not available"
fi

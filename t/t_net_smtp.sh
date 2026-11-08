#!/bin/bash
# t_net_smtp.sh - Net::SMTP against a fake server (forked by the test
# program): multi-line replies sent in one packet, several recipients,
# dot-stuffing and blank lines; and Net::SMTP_SSL STARTTLS (verification
# refuses a self-signed certificate; without verification the session
# continues encrypted), when the ssl library and openssl are available.
ns_dir="$BUILD_DIR/net_smtp"; mkdir -p "$ns_dir"
TOTAL=$((TOTAL + 1))
ns_port=$(( 21000 + RANDOM % 9000 ))
ns_out=""
if "$PROJECT_DIR/strada" "$PROJECT_DIR/t/net_smtp_test/plain.strada" -o "$ns_dir/plain" > "$ns_dir/plain.log" 2>&1; then
    ns_out=$(timeout 20 "$ns_dir/plain" "$ns_port" "$ns_dir/plain.txt" 2>&1)
fi
if [ "$ns_out" = "$(printf 'ehlo 1\nsend 1\nquit 1')" ] \
   && grep -q "^C:RCPT TO:<b@x.test>$" "$ns_dir/plain.txt" \
   && grep -q "^D:\.\.dot line$" "$ns_dir/plain.txt" \
   && [ "$(grep -c '^D:$' "$ns_dir/plain.txt")" = 2 ]; then
    PASSED=$((PASSED + 1)); log_pass "Net::SMTP: multi-line replies, recipients, dot-stuffing"
else
    FAILED=$((FAILED + 1)); log_fail "Net::SMTP: multi-line replies, recipients, dot-stuffing" "got: $ns_out $(head -c 300 "$ns_dir/plain.log" 2>/dev/null)"
fi

if [ -f "$PROJECT_DIR/lib/ssl.o" ] && command -v openssl >/dev/null 2>&1; then
    TOTAL=$((TOTAL + 1))
    openssl req -x509 -newkey rsa:2048 -nodes -keyout "$ns_dir/key.pem" -out "$ns_dir/cert.pem" -days 1 -subj "/CN=localhost" >/dev/null 2>&1
    ns_tout=""
    if "$PROJECT_DIR/strada" "$PROJECT_DIR/t/net_smtp_test/tls.strada" -o "$ns_dir/tls" -lssl -lcrypto > "$ns_dir/tls.log" 2>&1; then
        ns_tout=$(timeout 30 "$ns_dir/tls" "$((ns_port + 1))" "$ns_dir/tls.txt" "$ns_dir/cert.pem" "$ns_dir/key.pem" "$((ns_port + 2))" 2>&1)
    fi
    if echo "$ns_tout" | grep -q "^starttls (verify, self-signed) 0$" && echo "$ns_tout" | grep -q "^send 1$" \
       && grep -q "^tls:AUTH PLAIN" "$ns_dir/tls.txt" && grep -q "^D:over tls$" "$ns_dir/tls.txt"; then
        PASSED=$((PASSED + 1)); log_pass "Net::SMTP_SSL: STARTTLS (and verification refuses a self-signed certificate)"
    else
        FAILED=$((FAILED + 1)); log_fail "Net::SMTP_SSL: STARTTLS" "got: $ns_tout $(head -c 300 "$ns_dir/tls.log" 2>/dev/null)"
    fi
fi

#!/bin/bash
# t_hmac.sh - Digest::SHA::hmac_sha256(_hex): RFC 4231 test cases 1 and 2,
# a key longer than the block (hashed first) and data with a NUL byte.
TOTAL=$((TOTAL + 1))
hm_src="$BUILD_DIR/hmac_test.strada"
cat > "$hm_src" <<'SRC'
use Digest::SHA;
func main() int {
    my str $k1 = "";
    my int $i = 0;
    while ($i < 20) { $k1 = $k1 . chr(11); $i = $i + 1; }
    say(Digest::SHA::hmac_sha256_hex($k1, "Hi There"));
    say(Digest::SHA::hmac_sha256_hex("Jefe", "what do ya want for nothing?"));
    my str $k3 = "";
    $i = 0;
    while ($i < 100) { $k3 = $k3 . "k"; $i = $i + 1; }
    say(Digest::SHA::hmac_sha256_hex($k3, "long key"));
    say(Digest::SHA::hmac_sha256_hex("key", "a" . chr(0) . "b"));
    say(length(Digest::SHA::hmac_sha256("key", "x")));
    return 0;
}
SRC
hm_out=""
if "$PROJECT_DIR/strada" "$hm_src" -o "$BUILD_DIR/hmac_test" > "$BUILD_DIR/hmac_test.log" 2>&1; then hm_out=$("$BUILD_DIR/hmac_test" 2>&1); fi
hm_want="b0344c61d8db38535ca8afceaf0bf12b881dc200c9833da726e9376c2e32cff7
5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843
5903eedefe0bba3f15b658715e41d71f1848739bddf19e497e968df5723dcd56
29c5f0eea2d0a51b661193b3a56191192b2861f543ea0499a7f9725f7e7062fc
32"
if [ "$hm_out" = "$hm_want" ]; then
    PASSED=$((PASSED + 1)); log_pass "Digest::SHA HMAC-SHA256"
else
    FAILED=$((FAILED + 1)); log_fail "Digest::SHA HMAC-SHA256" "got: $hm_out $(head -c 200 "$BUILD_DIR/hmac_test.log")"
fi

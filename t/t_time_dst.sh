#!/bin/bash
# t_time_dst.sh - core::mktime without isdst works out daylight saving
# time (a missing key read as isdst = 0 put summer times an hour off), and
# accepts a hash reference.
TOTAL=$((TOTAL + 1))
td_src="$BUILD_DIR/time_dst.strada"
cat > "$td_src" <<'SRC'
func main() int {
    core::setenv("TZ", "America/New_York");
    my hash %summer = ("sec" => 0, "min" => 0, "hour" => 9, "mday" => 1, "mon" => 6, "year" => 126);
    my hash %winter = ("sec" => 0, "min" => 0, "hour" => 9, "mday" => 1, "mon" => 11, "year" => 126);
    say(core::strftime("%H:%M", core::gmtime(core::mktime(%summer))) . " " . core::strftime("%H:%M", core::gmtime(core::mktime(\%winter))));
    return 0;
}
SRC
td_out=""
if "$PROJECT_DIR/strada" "$td_src" -o "$BUILD_DIR/time_dst" > "$BUILD_DIR/time_dst.log" 2>&1; then td_out=$("$BUILD_DIR/time_dst" 2>&1); fi
if [ ! -f /usr/share/zoneinfo/America/New_York ]; then
    SKIPPED=$((SKIPPED + 1)); log_skip "mktime works out daylight saving time" "no tz database"
elif [ "$td_out" = "13:00 14:00" ]; then
    PASSED=$((PASSED + 1)); log_pass "mktime works out daylight saving time"
else
    FAILED=$((FAILED + 1)); log_fail "mktime works out daylight saving time" "got: $td_out"
fi

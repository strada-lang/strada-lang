#!/bin/bash
# Benchmark: Strada Interpreter vs Compiled Strada vs Perl vs Python
# Uses smaller workloads since the interpreter is much slower

set -e
cd "$(dirname "$0")"
STRADA=../../strada
INTERP=../../interpreter/strada-interp

echo "Strada Interpreter Benchmark"
echo "===================================="
echo ""

# Create compiled versions (need main() wrapper)
echo "Compiling Strada benchmarks..."
for bench in bench_compute bench_functions bench_strings bench_array_hash; do
    # Add main() that calls run()
    (cat "${bench}.strada"; echo '
func main() int { run(); return 0; }') > "/tmp/${bench}_compiled.strada"
    $STRADA "/tmp/${bench}_compiled.strada" -o "${bench}_compiled" 2>/dev/null
    echo "  ${bench} compiled"
done
echo ""

# Header
printf "%-20s %12s %12s %12s %12s\n" "Benchmark" "Interpreted" "Compiled" "Perl" "Python"
printf "%-20s %12s %12s %12s %12s\n" "--------------------" "------------" "------------" "------------" "------------"

for bench in bench_compute bench_functions bench_strings bench_array_hash; do
    printf "%-20s" "$bench"

    # Strada Interpreter
    INTERP_TIME=$( { TIMEFORMAT='%R'; time echo ".load ${bench}.strada
run()
.quit" | $INTERP > /dev/null 2>&1; } 2>&1 )
    printf " %10ss" "$INTERP_TIME"

    # Compiled Strada
    COMP_TIME=$( { TIMEFORMAT='%R'; time ./${bench}_compiled > /dev/null 2>&1; } 2>&1 )
    printf " %10ss" "$COMP_TIME"

    # Perl
    PERL_TIME=$( { TIMEFORMAT='%R'; time perl ${bench}.pl > /dev/null 2>&1; } 2>&1 )
    printf " %10ss" "$PERL_TIME"

    # Python
    PY_TIME=$( { TIMEFORMAT='%R'; time python3 ${bench}.py > /dev/null 2>&1; } 2>&1 )
    printf " %10ss" "$PY_TIME"

    echo ""
done

echo ""
echo "Done."

# Cleanup
for bench in bench_compute bench_functions bench_strings bench_array_hash; do
    rm -f "${bench}_compiled"
done

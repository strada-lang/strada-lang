#!/usr/bin/env bash
set -e

cd "$(dirname "$0")"

RUNS=3
STRADA=../strada
PERL=${PERL:-perl}
PYTHON=${PYTHON:-python3}

BENCHMARKS="bench_compute bench_strings bench_array_hash bench_functions bench_oop"

# Colors
BOLD='\033[1m'
CYAN='\033[36m'
GREEN='\033[32m'
YELLOW='\033[33m'
RED='\033[31m'
RESET='\033[0m'

echo -e "${BOLD}Strada Benchmark Suite${RESET}"
echo "======================================"
echo ""

# Check tools
echo -n "Strada: "; $STRADA --version 2>/dev/null || echo "(available)"
echo -n "Perl:   "; $PERL -v 2>/dev/null | grep -oP 'v[\d.]+' | head -1 || echo "(not found)"
echo -n "Python: "; $PYTHON --version 2>/dev/null || echo "(not found)"
echo ""

# Compile all Strada benchmarks
echo -e "${CYAN}Compiling Strada benchmarks...${RESET}"
for bench in $BENCHMARKS; do
    echo -n "  $bench.strada ... "
    if $STRADA "${bench}.strada" 2>/dev/null; then
        echo "ok"
    else
        echo "FAILED"
    fi
done
echo ""

# Time a command, return seconds as a decimal string
# Usage: best_time <runs> <command...>
best_time() {
    local runs=$1
    shift
    local best=""
    for ((r=1; r<=runs; r++)); do
        local t
        t=$( { TIMEFORMAT='%R'; time "$@" > /dev/null 2>&1; } 2>&1 )
        if [ -z "$best" ] || (( $(echo "$t < $best" | bc -l) )); then
            best=$t
        fi
    done
    echo "$best"
}

# Compute speedup ratio: slower / faster
speedup() {
    local base=$1
    local other=$2
    if (( $(echo "$other == 0" | bc -l) )); then
        echo "N/A"
        return
    fi
    echo "$(echo "scale=2; $other / $base" | bc -l)"
}

# Header
printf "\n${BOLD}%-20s %10s %10s %10s   %-14s %-14s${RESET}\n" \
    "Benchmark" "Strada" "Perl" "Python" "vs Perl" "vs Python"
printf "%-20s %10s %10s %10s   %-14s %-14s\n" \
    "--------------------" "----------" "----------" "----------" "--------------" "--------------"

# Run benchmarks
for bench in $BENCHMARKS; do
    printf "%-20s" "$bench"

    # Strada
    if [ -f "./$bench" ]; then
        st=$(best_time $RUNS "./$bench")
        printf " %9ss" "$st"
    else
        st=""
        printf " %10s" "SKIP"
    fi

    # Perl
    if command -v $PERL &>/dev/null && [ -f "${bench}.pl" ]; then
        pl=$(best_time $RUNS $PERL "${bench}.pl")
        printf " %9ss" "$pl"
    else
        pl=""
        printf " %10s" "SKIP"
    fi

    # Python
    if command -v $PYTHON &>/dev/null && [ -f "${bench}.py" ]; then
        py=$(best_time $RUNS $PYTHON "${bench}.py")
        printf " %9ss" "$py"
    else
        py=""
        printf " %10s" "SKIP"
    fi

    # Speedup ratios
    if [ -n "$st" ] && [ -n "$pl" ]; then
        ratio=$(speedup "$st" "$pl")
        printf "   ${GREEN}%sx faster${RESET}" "$ratio"
    else
        printf "   %-14s" "N/A"
    fi

    if [ -n "$st" ] && [ -n "$py" ]; then
        ratio=$(speedup "$st" "$py")
        printf "   ${GREEN}%sx faster${RESET}" "$ratio"
    else
        printf "   %-14s" "N/A"
    fi

    echo ""
done

echo ""
echo -e "${BOLD}Done.${RESET} (best of $RUNS runs each)"

# Cleanup compiled Strada binaries
for bench in $BENCHMARKS; do
    rm -f "./$bench"
done

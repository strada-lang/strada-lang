#!/usr/bin/env bash
set -e

cd "$(dirname "$0")"

RUNS=3
STRADA=../strada
PERL=${PERL:-perl}
PYTHON=${PYTHON:-python3}
RUBY=${RUBY:-ruby}
NODE=${NODE:-node}
PHP=${PHP:-php}

ALL_BENCHMARKS="bench_compute bench_strings bench_array_hash bench_functions bench_oop"

usage() {
    echo "Usage: $0 [OPTIONS] [BENCHMARK ...]"
    echo ""
    echo "Run one or more benchmarks. If none specified, all are run."
    echo ""
    echo "Available benchmarks:"
    for b in $ALL_BENCHMARKS; do
        echo "  $b"
    done
    echo ""
    echo "Options:"
    echo "  -r, --runs N    Number of runs per benchmark (default: 3)"
    echo "  -l, --list      List available benchmarks and exit"
    echo "  -h, --help      Show this help message"
    echo ""
    echo "Examples:"
    echo "  $0                          # Run all benchmarks"
    echo "  $0 bench_oop                # Run only bench_oop"
    echo "  $0 bench_oop bench_strings  # Run two benchmarks"
    echo "  $0 -r 5 bench_compute      # Run bench_compute with 5 runs"
}

SELECTED=()
while [[ $# -gt 0 ]]; do
    case $1 in
        -h|--help)
            usage
            exit 0
            ;;
        -l|--list)
            for b in $ALL_BENCHMARKS; do echo "$b"; done
            exit 0
            ;;
        -r|--runs)
            RUNS="$2"
            shift 2
            ;;
        -*)
            echo "Unknown option: $1"
            usage
            exit 1
            ;;
        *)
            SELECTED+=("$1")
            shift
            ;;
    esac
done

if [[ ${#SELECTED[@]} -eq 0 ]]; then
    BENCHMARKS="$ALL_BENCHMARKS"
else
    # Validate selected benchmarks
    for sel in "${SELECTED[@]}"; do
        found=0
        for b in $ALL_BENCHMARKS; do
            if [[ "$sel" == "$b" ]]; then found=1; break; fi
        done
        if [[ $found -eq 0 ]]; then
            echo "Unknown benchmark: $sel"
            echo "Run '$0 --list' to see available benchmarks."
            exit 1
        fi
    done
    BENCHMARKS="${SELECTED[*]}"
fi

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
echo -n "Ruby:   "; $RUBY --version 2>/dev/null | awk '{print $2}' || echo "(not found)"
echo -n "Node:   "; $NODE --version 2>/dev/null || echo "(not found)"
echo -n "PHP:    "; $PHP --version 2>/dev/null | head -1 | awk '{print $2}' || echo "(not found)"
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
printf "\n${BOLD}%-20s %10s %10s %10s %10s %10s %10s${RESET}\n" \
    "Benchmark" "Strada" "Perl" "Python" "Ruby" "Node" "PHP"
printf "%-20s %10s %10s %10s %10s %10s %10s\n" \
    "--------------------" "----------" "----------" "----------" "----------" "----------" "----------"

# Collect all results for the comparison table
declare -A RESULTS

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
    RESULTS["$bench,strada"]="$st"

    # Perl
    if command -v $PERL &>/dev/null && [ -f "${bench}.pl" ]; then
        pl=$(best_time $RUNS $PERL "${bench}.pl")
        printf " %9ss" "$pl"
    else
        pl=""
        printf " %10s" "SKIP"
    fi
    RESULTS["$bench,perl"]="$pl"

    # Python
    if command -v $PYTHON &>/dev/null && [ -f "${bench}.py" ]; then
        py=$(best_time $RUNS $PYTHON "${bench}.py")
        printf " %9ss" "$py"
    else
        py=""
        printf " %10s" "SKIP"
    fi
    RESULTS["$bench,python"]="$py"

    # Ruby
    if command -v $RUBY &>/dev/null && [ -f "${bench}.rb" ]; then
        rb=$(best_time $RUNS $RUBY "${bench}.rb")
        printf " %9ss" "$rb"
    else
        rb=""
        printf " %10s" "SKIP"
    fi
    RESULTS["$bench,ruby"]="$rb"

    # Node
    if command -v $NODE &>/dev/null && [ -f "${bench}.js" ]; then
        js=$(best_time $RUNS $NODE "${bench}.js")
        printf " %9ss" "$js"
    else
        js=""
        printf " %10s" "SKIP"
    fi
    RESULTS["$bench,node"]="$js"

    # PHP
    if command -v $PHP &>/dev/null && [ -f "${bench}.php" ]; then
        ph=$(best_time $RUNS $PHP "${bench}.php")
        printf " %9ss" "$ph"
    else
        ph=""
        printf " %10s" "SKIP"
    fi
    RESULTS["$bench,php"]="$ph"

    echo ""
done

# Speedup comparison table
echo ""
printf "${BOLD}%-20s %14s %14s %14s %14s %14s${RESET}\n" \
    "Speedup (vs Strada)" "Perl" "Python" "Ruby" "Node" "PHP"
printf "%-20s %14s %14s %14s %14s %14s\n" \
    "--------------------" "--------------" "--------------" "--------------" "--------------" "--------------"

for bench in $BENCHMARKS; do
    printf "%-20s" "$bench"
    st="${RESULTS[$bench,strada]}"

    for lang in perl python ruby node php; do
        val="${RESULTS[$bench,$lang]}"
        if [ -n "$st" ] && [ -n "$val" ]; then
            ratio=$(speedup "$st" "$val")
            printf "   ${GREEN}%9sx${RESET}  " "$ratio"
        else
            printf " %14s" "N/A"
        fi
    done
    echo ""
done

echo ""
echo -e "${BOLD}Done.${RESET} (best of $RUNS runs each)"

# Cleanup compiled Strada binaries
for bench in $BENCHMARKS; do
    rm -f "./$bench"
done

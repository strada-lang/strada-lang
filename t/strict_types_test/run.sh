#!/bin/bash
#
# Checked types: `use strict "types";` and element types.
#
#   - In a file that says `use strict "types";` a type mismatch is a compile
#     error (initializers, assignments, call arguments, return values, and
#     values going into array<T> / hash<T> containers). A fractional value
#     needs int(...) to become an int.
#   - Without the pragma the same program compiles; --strict-types reports
#     the mismatches as warnings.
#   - The pragma covers one file: a strict module does not make its user
#     strict, and a strict program does not make a module strict.
#   - strada-interp rejects the same programs.

set -u
SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_DIR="$(cd "$SCRIPT_DIR/../.." && pwd)"
STRADA="$REPO_DIR/strada"
INTERP="$REPO_DIR/interpreter/strada-interp"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
cd "$WORK" || exit 1
FAILURES=0

check() {
    local name="$1" expect="$2" output="$3"
    if printf '%s' "$output" | grep -qF -- "$expect"; then
        echo "ok: $name"
    else
        echo "FAIL: $name"
        echo "  expected: $expect"
        echo "  got: $(printf '%s' "$output" | head -5)"
        FAILURES=$((FAILURES + 1))
    fi
}
check_not() {
    local name="$1" unexpected="$2" output="$3"
    if printf '%s' "$output" | grep -qF -- "$unexpected"; then
        echo "FAIL: $name (found: $unexpected)"
        FAILURES=$((FAILURES + 1))
    else
        echo "ok: $name"
    fi
}

# ---- a correctly typed program ------------------------------------------
cat > ok.strada <<'SRC'
use strict "types";

func total(array<int> @prices, num $tax) num {
    my int $sum = 0;
    foreach my int $p (@prices) { $sum += $p; }
    return $sum * (1 + $tax);
}

func first_name(hash<str, str> %names) str {
    return $names{"a"};
}

func main() int {
    my array<int> @list = (10, 20, 30);
    push(@list, 40);
    unshift(@list, 0);
    $list[0] = 5;
    my hash<str, int> %age = ();
    $age{"bob"} = 41;
    my hash<num> %w = ();
    $w{"a"} = 1.5;
    my array< str > @names = ("a", "b");
    my hash<str> %nm = ();
    $nm{"a"} = "ann";
    my int $first = $list[0];
    my int $half = int($first / 2);
    my num $ratio = $first;
    my str $in = "0.5";
    my scalar $any = $in;
    my int $loose = $any;
    my array @plain = (1, "two");
    push(@list, shift(@list));
    say(total(@list, $in + 0));
    say($half . " " . $age{"bob"} . " " . $names[1] . " " . $w{"a"} . " " . first_name(%nm) . " " . $ratio . " " . $loose . " " . scalar(@plain));
    return 0;
}
SRC
out=$("$STRADA" -O0 -o ok ok.strada 2>&1) || { echo "FAIL: typed program does not compile"; echo "$out"; exit 1; }
check "typed program runs (compiled)" "157.5" "$(./ok)"
check "typed program output"          "2 41 b 1.5 ann 5 0 2" "$(./ok)"

# ---- every kind of mismatch is a located error --------------------------
cat > bad.strada <<'SRC'
use strict "types";

func total(array<int> @prices, num $tax) num {
    my int $sum = 0;
    foreach my str $p (@prices) { $sum += 1; }
    return "x";
}

func main() int {
    my array<int> @list = (10, "twenty", 30);
    my array<str> @names = ("a");
    push(@list, "forty");
    $list[0] = "five";
    my hash<str, int> %age = ();
    $age{"bob"} = "old";
    my int $half = $list[0] / 2;
    my str $in = "0.5";
    my int $n = $in;
    $n = 1.5;
    say(total(@names, $in));
    push(@list, @names);
    @list = (1, "two");
    my str $s = $list[0];
    return 0;
}
SRC
out=$("$STRADA" -O0 -o bad bad.strada 2>&1); rc=$?
if [ $rc -ne 0 ]; then echo "ok: mismatches fail the build"; else echo "FAIL: mismatches compiled"; FAILURES=$((FAILURES + 1)); fi
check "foreach variable"      "bad.strada:5: error: loop variable '\$p' (declared str) iterates over '@prices' (declared array<int>)" "$out"
check "return value"          "bad.strada:6: error: returning a str value from 'total' (declared to return num)" "$out"
check "list initializer"      "bad.strada:10: error: storing a str value in '@list' (declared array<int>)" "$out"
check "push"                  "bad.strada:12: error: storing a str value in '@list'" "$out"
check "element assignment"    "bad.strada:13: error: storing a str value in '@list'" "$out"
check "hash element"          "bad.strada:15: error: storing a str value in '%age' (declared hash<int>)" "$out"
check "num does not become int" "bad.strada:16: error: initializing '\$half' (declared int) with a num value" "$out"
check "str into int"          "bad.strada:18: error: initializing '\$n' (declared int) with a str value" "$out"
check "assignment narrowing"  "bad.strada:19: error: assigning a num value to '\$n' (declared int)" "$out"
check "array argument"        "bad.strada:20: error: argument 1 to 'total': expected array<int>, got array<str>" "$out"
check "scalar argument"       "bad.strada:20: error: argument 2 to 'total': expected num, got str" "$out"
check "push of a typed array" "bad.strada:21: error: putting the elements of '@names' (array<str>) into '@list'" "$out"
check "list assignment"       "bad.strada:22: error: storing a str value in '@list'" "$out"
check "element read is typed" "bad.strada:23: error: initializing '\$s' (declared str) with a int value" "$out"
check "all errors in one run" "compilation failed: 14 semantic errors" "$out"

# ---- without the pragma: compiles; --strict-types warns ------------------
sed 's/^use strict "types";//' bad.strada > loose.strada
out=$("$STRADA" -O0 -o loose loose.strada 2>&1); rc=$?
if [ $rc -eq 0 ] && [ -z "$out" ]; then echo "ok: no pragma, no diagnostics"; else echo "FAIL: loose build: $out"; FAILURES=$((FAILURES + 1)); fi
out=$("$STRADA" -O0 --strict-types -o loose loose.strada 2>&1); rc=$?
if [ $rc -eq 0 ]; then echo "ok: --strict-types still builds"; else echo "FAIL: --strict-types failed the build"; FAILURES=$((FAILURES + 1)); fi
check "--strict-types warns about elements" "loose.strada:12: warning: storing a str value in '@list'" "$out"
check_not "--strict-types keeps num -> int silent" "with a num value" "$out"

# ---- the pragma covers one file -----------------------------------------
mkdir -p lib
cat > lib/Loose.strada <<'SRC'
package Loose;
func count(str $s) int {
    my int $n = $s;
    return $n;
}
SRC
cat > lib/Strict.strada <<'SRC'
package Strict;
use strict "types";
func count(str $s) int {
    my int $n = $s;
    return $n;
}
SRC
cat > strict_main.strada <<'SRC'
use strict "types";
use lib "lib";
use Loose;
func main() int {
    say(Loose::count("12"));
    return 0;
}
SRC
out=$("$STRADA" -O0 -o strict_main strict_main.strada 2>&1) || { echo "FAIL: strict program with a loose module: $out"; FAILURES=$((FAILURES + 1)); }
check "strict program, loose module" "12" "$(./strict_main 2>&1)"
cat > loose_main.strada <<'SRC'
use lib "lib";
use Strict;
func main() int {
    my int $k = "7";
    say(Strict::count("12") + $k);
    return 0;
}
SRC
out=$("$STRADA" -O0 -o loose_main loose_main.strada 2>&1); rc=$?
if [ $rc -ne 0 ]; then echo "ok: strict module is checked"; else echo "FAIL: strict module compiled"; FAILURES=$((FAILURES + 1)); fi
check "error names the module"         "Strict.strada:4: error: initializing '\$n' (declared int) with a str value" "$out"
check_not "loose user is not checked"  "loose_main.strada" "$out"

# ---- every kind of code in a strict file is checked -------------------------
# methods, private/async/extern-with-body/fn functions, before/after/around
# modifiers, BEGIN/END blocks, closures, and file-level my/our initializers
out=$("$STRADA" --check "$SCRIPT_DIR/forms.strada" 2>&1)
check "all function and declaration forms" "compilation failed: 14 semantic errors" "$out"
for ln in 4 5 6 7 8 10 11 12 13 14 15 16 18 19; do
    check "forms.strada line $ln is checked" "forms.strada:$ln: error: initializing" "$out"
done

# ---- syntax errors ---------------------------------------------------------
printf 'use strict "refs";\nfunc main() int { return 0; }\n' > p1.strada
check "unknown strict category" "unknown strict category 'refs'" "$("$STRADA" -O0 -o p1 p1.strada 2>&1)"
printf 'func main() int { my array<array> @a = (); return 0; }\n' > p2.strada
check "bad element type" "element type must be int, num, str, scalar or a class name" "$("$STRADA" -O0 -o p2 p2.strada 2>&1)"
printf 'func main() int { my hash<int, str> %%h = (); return 0; }\n' > p3.strada
check "hash key type" "hash keys are strings" "$("$STRADA" -O0 -o p3 p3.strada 2>&1)"

# ---- the VM agrees ---------------------------------------------------------
if [ -x "$INTERP" ]; then
    check "typed program runs (VM)" "157.5" "$("$INTERP" ok.strada 2>&1)"
    out=$("$INTERP" bad.strada 2>&1); rc=$?
    if [ $rc -ne 0 ]; then echo "ok: VM rejects mismatches"; else echo "FAIL: VM ran the mismatching program"; FAILURES=$((FAILURES + 1)); fi
    check "VM reports the same errors" "bad.strada:13: error: storing a str value in '@list'" "$out"
    check "VM error count"             "compilation failed: 14 semantic errors" "$out"
    out=$("$INTERP" loose.strada 2>&1); rc=$?
    check_not "VM runs the loose program" "semantic error" "$out"
    check "VM without --no-jit and with it agree" "157.5" "$("$INTERP" --no-jit ok.strada 2>&1)"
else
    echo "skip: strada-interp not built"
fi

if [ $FAILURES -eq 0 ]; then
    echo "strict-types: all passed"
    exit 0
fi
echo "strict-types: $FAILURES failed"
exit 1

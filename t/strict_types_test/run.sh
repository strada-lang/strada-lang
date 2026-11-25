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

# ---- hash literals and `our` initializers inside a function ---------------
cat > hl.strada <<'SRC'
use strict "types";
func main() int {
    my str $k = "z";
    my hash<str, int> %ok = ("a" => 1, $k => 2, "c", 3);
    %ok = ("b" => 4);
    our num $fine = 5;
    my hash<str, int> %h = ("a" => "x");
    %h = ("b" => "y");
    our int $x = "s";
    return 0;
}
SRC
out=$("$STRADA" --check hl.strada 2>&1)
check "hash literal initializer"  "hl.strada:7: error: storing a str value in '%h' (declared hash<int>)" "$out"
check "hash literal assignment"   "hl.strada:8: error: storing a str value in '%h'" "$out"
check "our initializer in a function" "hl.strada:9: error: initializing '\$x' (declared int) with a str value" "$out"
check "correct hash literals pass" "compilation failed: 3 semantic errors" "$out"

# ---- the example program: runs, and every "would not compile" line fails ---
EX="$REPO_DIR/examples/checked_types.strada"
out=$("$STRADA" -O0 -o ex_ct "$EX" 2>&1) || { echo "FAIL: examples/checked_types.strada does not compile: $out"; FAILURES=$((FAILURES + 1)); }
EX_OUT=$(./ex_ct 2>&1)
check "example: scalars"  "scalars: 3 1 42 42.5 3" "$EX_OUT"
check "example: arrays"   "prices: 5,20,30,40 first=5 total=142.5" "$EX_OUT"
check "example: hashes"   "average: 8.25" "$EX_OUT"
check "example: objects"  "account: Ann 165 after 2 moves" "$EX_OUT"
check "example: untyped"  "value: 42, loose has 3" "$EX_OUT"
ex_lines=$(grep -n "# would not compile" "$EX" | cut -d: -f1)
ex_n=0
for ln in $ex_lines; do
    ex_n=$((ex_n + 1))
    # uncomment that one line (drop the leading "# " after the indentation)
    awk -v L="$ln" 'NR == L { sub(/# /, ""); } { print }' "$EX" > ex_bad.strada
    out=$("$STRADA" --check ex_bad.strada 2>&1); rc=$?
    if [ $rc -ne 0 ] && printf '%s' "$out" | grep -q "ex_bad.strada:$ln: error:"; then
        echo "ok: example line $ln is rejected"
    else
        echo "FAIL: example line $ln (marked 'would not compile') was accepted or failed elsewhere"
        echo "  $(printf '%s' "$out" | head -3)"
        FAILURES=$((FAILURES + 1))
    fi
done
if [ $ex_n -ge 10 ]; then echo "ok: example has $ex_n rejected lines"; else echo "FAIL: expected at least 10 'would not compile' lines, found $ex_n"; FAILURES=$((FAILURES + 1)); fi

# ---- method calls: arguments, results, constructor arguments --------------
# Checked when the receiver's class is known: $self, a variable declared
# with Pkg::new(...) and never assigned again, a typed catch variable, a
# class name, SUPER::.
cat > mc.strada <<'SRC'
use strict "types";
package Acct;
has rw int $balance = 0;
has ro str $owner (required);
func deposit(scalar $self, int $amt) void { $self->set_balance($self->balance() + $amt); }
func label(scalar $self, str $pfx, int $n = 1) str { return $pfx . $self->owner(); }
func inside(scalar $self) void {
    $self->deposit("ten");
    $self->set_balance("x");
}
package Savings;
extends Acct;
has rw num $rate = 0.01;
func grow(scalar $self) void {
    SUPER::deposit("lots");
}
package main;
func main() int {
    my scalar $a = Acct::new(owner => "bob", balance => "much");
    $a->deposit("fifty");
    $a->deposit(1.5);
    say($a->label(7));
    my int $n = $a->owner();
    my scalar $sv = Savings::new(owner => 3, rate => "hi");
    $sv->deposit("x");
    $sv->set_rate("y");
    Acct->label(5);
    try { $a->deposit(1); } catch (Acct $e) {
        $e->deposit("c");
    }
    return 0;
}
SRC
out=$("$STRADA" --check mc.strada 2>&1)
check "method arg via \$self"        "mc.strada:8: error: argument 1 to 'Acct::deposit': expected int, got str" "$out"
check "generated setter arg"         "mc.strada:9: error: argument 1 to 'Acct::set_balance': expected int, got str" "$out"
check "SUPER:: arg"                  "mc.strada:15: error: argument 1 to 'Acct::deposit': expected int, got str" "$out"
check "constructor attribute"        "mc.strada:19: error: attribute 'balance' of 'Acct': expected int, got str" "$out"
check "method arg on a new object"   "mc.strada:20: error: argument 1 to 'Acct::deposit': expected int, got str" "$out"
check "num into an int parameter"    "mc.strada:21: error: argument 1 to 'Acct::deposit': expected int, got num" "$out"
check "method arg inside a call"     "mc.strada:22: error: argument 1 to 'Acct::label': expected str, got int" "$out"
check "method result type"           "mc.strada:23: error: initializing '\$n' (declared int) with a str value" "$out"
check "inherited attribute"          "mc.strada:24: error: attribute 'owner' of 'Acct': expected str, got int" "$out"
check "own attribute of a subclass"  "mc.strada:24: error: attribute 'rate' of 'Savings': expected num, got str" "$out"
check "inherited method"             "mc.strada:25: error: argument 1 to 'Acct::deposit': expected int, got str" "$out"
check "subclass setter"              "mc.strada:26: error: argument 1 to 'Savings::set_rate': expected num, got str" "$out"
check "class-name receiver"          "mc.strada:27: error: argument 1 to 'Acct::label': expected str, got int" "$out"
check "typed catch variable"         "mc.strada:29: error: argument 1 to 'Acct::deposit': expected int, got str" "$out"
check "method error count"           "compilation failed: 14 semantic errors" "$out"

# Where the class is not certain, nothing is reported, and the program runs.
cat > mcok.strada <<'SRC'
use strict "types";
package Shape;
has ro str $name = "shape";
func scale(scalar $self, int $by) str { return "shape*" . $by; }
func describe(scalar $self) str { return $self->scale("big"); }   # a subclass overrides scale
func tag(scalar $self, int $n) str { return "tag" . $n; }
func sum(scalar $self, int ...@nums) int { my int $t = 0; foreach my int $v (@nums) { $t += $v; } return $t; }
package Circle;
extends Shape;
func scale(scalar $self, str $by) str { return "circle*" . $by; }
package Wrapped;
has ro int $n = 0;
func show(scalar $self, int $v) str { return "v=" . $v; }
around "show" func(scalar $orig, scalar $self, scalar $v) str { return $orig->($self, length($v)); }
package Maker;
func new(str $kind) scalar {
    if ($kind eq "circle") { return Circle::new(); }
    return Shape::new();
}
package main;
func make() scalar { return Shape::new(); }
func main() int {
    my scalar $c = Circle::new();
    say($c->describe());                 # Circle::scale takes a str
    my scalar $any = make();
    say($any->tag("7"));                 # class not known: a function's result
    my scalar $re = Shape::new();
    $re = Circle::new();
    say($re->scale("x"));                # assigned again: class not known
    my scalar $w = Wrapped::new();
    say($w->show("abcd"));               # an around modifier takes the argument
    my scalar $m = Maker::new("circle"); # a factory: not a Maker
    say($m->scale("f"));
    my scalar $s = Shape::new();
    say($s->sum(1, 2, 3));
    say($s->tag(5));
    return 0;
}
SRC
out=$("$STRADA" -O0 -o mcok mcok.strada 2>&1); rc=$?
if [ $rc -eq 0 ]; then echo "ok: uncertain receivers compile"; else echo "FAIL: uncertain receivers: $out"; FAILURES=$((FAILURES + 1)); fi
MCOK_OUT=$(./mcok 2>&1 | tr '\n' ' ')
check "uncertain receivers run" "circle*big tag7 circle*x v=4 circle*f 6 tag5" "$MCOK_OUT"

# A variable that is written again in any way has no known class: list
# assignment, foreach over an existing variable, a closure, a reference, a
# __C__ block, a file-level variable another function assigns. A role's
# method is not checked when the consuming class overrides what it calls.
# `%$args` in a constructor call or a typed hash literal moves positions.
cat > mcneg.strada <<'SRC'
use strict "types";
package Greeter;
func greet(scalar $self, int $n) str { return "role" . $n; }
func hello(scalar $self) str { return $self->greet("x"); }
package Acct;
with Greeter;
has rw int $balance = 0;
func deposit(scalar $self, int $amt) void { $self->set_balance($self->balance() + $amt); }
func greet(scalar $self, str $n) str { return "acct" . $n; }
package Other;
has rw str $balance = "";
func deposit(scalar $self, str $amt) void { $self->set_balance($amt); }
package main;
my scalar $g_obj = Acct::new();
func swap_global() void { $g_obj = Other::new(); }
func fill(scalar $ref) void { $$ref = Other::new(); }
func main() int {
    my scalar $a = Acct::new();
    my scalar $b = Acct::new();
    ($a, $b) = (Other::new(), Other::new());
    $a->deposit("list");
    $b->deposit("list");
    my scalar $c = Acct::new();
    foreach $c (Other::new()) { }
    my scalar $d = Acct::new();
    my scalar $f = func () void { $d = Other::new(); };
    $f->();
    $d->deposit("closure");
    my scalar $e = Acct::new();
    fill(\$e);
    $e->deposit("ref");
    my scalar $k = Acct::new();
    __C__ { strada_decref(k); k = strada_new_str("x"); }
    swap_global();
    $g_obj->deposit("global");
    my scalar $args = { "balance" => 5 };
    my scalar $h = Acct::new(%$args);
    my scalar $h2 = Acct::new(%{$args});
    my hash<int> %lit = (%$args, "k" => 2);
    say($h->balance() + $h2->balance() + $lit{"k"});
    say($h->hello());
    say($e->balance() . $d->balance() . $a->balance() . $g_obj->balance());
    return 0;
}
SRC
out=$("$STRADA" -O0 -o mcneg mcneg.strada 2>&1); rc=$?
if [ $rc -eq 0 ]; then echo "ok: rewritten variables are not checked"; else echo "FAIL: rewritten variables: $out"; FAILURES=$((FAILURES + 1)); fi
check "rewritten variables run" "12 acctx refclosurelistglobal" "$(./mcneg 2>&1 | tr '\n' ' ')"

# Still checked: a method with a before hook, an element of a typed hash,
# an argument after an array (method arguments are not flattened), a call
# to a function with a declared return type.
cat > mcpos.strada <<'SRC'
use strict "types";
package Acct;
has rw int $balance = 0;
func deposit(scalar $self, int $amt) void { $self->set_balance($self->balance() + $amt); }
before "deposit" func(scalar $self) void { say("pre"); }
func two(scalar $self, int $a, int $b) int { return $a + $b; }
package main;
func name() str { return "n"; }
func main() int {
    my scalar $a = Acct::new();
    $a->deposit("hooked");
    my hash<str> %cfg = ("who" => "ann");
    $a->deposit($cfg{"who"});
    my array @list = (1, 2);
    $a->two(@list, "second");
    $a->two(name(), 2);
    return 0;
}
SRC
out=$("$STRADA" --check mcpos.strada 2>&1)
check "method with a before hook"   "mcpos.strada:11: error: argument 1 to 'Acct::deposit': expected int, got str" "$out"
check "typed hash element argument" "mcpos.strada:13: error: argument 1 to 'Acct::deposit': expected int, got str" "$out"
check "argument after an array"     "mcpos.strada:15: error: argument 2 to 'Acct::two': expected int, got str" "$out"
check "function result argument"    "mcpos.strada:16: error: argument 1 to 'Acct::two': expected int, got str" "$out"
check "mcpos error count"           "compilation failed: 4 semantic errors" "$out"

# Without the pragma the mismatching program compiles; --strict-types warns.
sed '1d' mc.strada > mcloose.strada
out=$("$STRADA" --check mcloose.strada 2>&1); rc=$?
if [ $rc -eq 0 ]; then echo "ok: method mismatches compile without the pragma"; else echo "FAIL: no pragma: $out"; FAILURES=$((FAILURES + 1)); fi
out=$("$STRADA" --strict-types --check mcloose.strada 2>&1); rc=$?
check "--strict-types warns about a method argument" "warning: argument 1 to 'Acct::deposit': expected int, got str" "$out"
if [ $rc -eq 0 ]; then echo "ok: --strict-types method warnings are not errors"; else echo "FAIL: --strict-types failed the build"; FAILURES=$((FAILURES + 1)); fi

# ---- bare `use strict;` is accepted and does nothing ------------------------
cat > us.strada <<'SRC'
use strict;
func main() int {
    my int $n = "12abc";
    say($n);
    return 0;
}
SRC
out=$("$STRADA" -O0 -o us us.strada 2>&1); rc=$?
if [ $rc -eq 0 ] && [ -z "$out" ]; then echo "ok: use strict; compiles silently"; else echo "FAIL: use strict;: $out"; FAILURES=$((FAILURES + 1)); fi
check "use strict; does not check types" "12" "$(./us 2>&1)"
printf 'use strict;\nuse strict "types";\nfunc main() int { my int $n = "x"; return 0; }\n' > us2.strada
check "use strict; then use strict \"types\";" "us2.strada:3: error: initializing '\$n'" "$("$STRADA" --check us2.strada 2>&1)"

# ---- valid code the checker must not reject --------------------------------
cat > valid.strada <<'SRC'
use strict "types";
func main() int {
    my hash<int> %base = ("a" => 1);
    my array @pairs = ("x", 5);
    my hash<int> %h = (%base, "k" => 2);          # another hash in the list
    my hash<int> %g = (@pairs, "k" => 2);         # an array of pairs
    %h = (%base, "z" => 3);
    my int $v = 70000;
    my int $b2 = ($v / 256) % 256;                # % is integer modulo
    my int $c = '/';                              # a character code
    $c = '(';
    my str $s = 'x';                              # ... and still a string
    my int $q = int($v / 256);
    say(scalar(keys(%h)) . " " . scalar(keys(%g)) . " " . $b2 . " " . $c . " " . $s . " " . $q);
    return 0;
}
SRC
out=$("$STRADA" -O0 -o valid valid.strada 2>&1); rc=$?
if [ $rc -eq 0 ] && [ -z "$out" ]; then echo "ok: spread hash literals, %, char literals compile"; else echo "FAIL: valid strict code rejected: $out"; FAILURES=$((FAILURES + 1)); fi
check "valid strict program output" "2 2 17 40 x 273" "$(./valid 2>&1)"

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
    check "VM runs the example the same" "$EX_OUT" "$("$INTERP" "$EX" 2>&1)"
    check "VM accepts use strict;" "12" "$("$INTERP" us.strada 2>&1)"
    check "VM runs the valid strict program" "2 2 17 40 x 273" "$("$INTERP" valid.strada 2>&1)"
    out=$("$INTERP" mc.strada 2>&1); rc=$?
    if [ $rc -ne 0 ]; then echo "ok: VM rejects method mismatches"; else echo "FAIL: VM ran mc.strada"; FAILURES=$((FAILURES + 1)); fi
    check "VM reports method errors" "mc.strada:20: error: argument 1 to 'Acct::deposit': expected int, got str" "$out"
    check "VM runs uncertain receivers" "circle*big tag7 circle*x v=4 circle*f 6 tag5" "$("$INTERP" mcok.strada 2>&1 | tr '\n' ' ')"
    # The VM enforces the type checks only: code the compiler alone rejects
    # for other reasons (too few arguments here) still runs.
    cat > vmonly.strada <<'SRC'
use strict "types";
func three(int $a, int $b, int $c) int { return $a + 1; }
func main() int {
    say(three(4));
    return 0;
}
SRC
    check "VM: pragma does not add unrelated compiler errors" "5" "$("$INTERP" vmonly.strada 2>&1)"
    check "compiler still rejects that program" "too few arguments" "$("$STRADA" --check vmonly.strada 2>&1)"
else
    echo "skip: strada-interp not built"
fi

if [ $FAILURES -eq 0 ]; then
    echo "strict-types: all passed"
    exit 0
fi
echo "strict-types: $FAILURES failed"
exit 1

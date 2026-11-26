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
check "example: class containers" "accounts: 166 11" "$EX_OUT"
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
# ---- our / file-level containers and returned containers -----------------
# A file-level `my`, an `our` (file level or in a function) and `local` are
# checked like a `my` container; each reference uses the declaration it
# binds to (same name in another package, other sigils of the same name).
cat > gc.strada <<'SRC'
use strict "types";
our array<int> @g = (1, "two");
our hash<int> %gh = (a => 1, b => "x");
my array<num> @fm = (1.5, "s");
our array<int> @ok = (1, 2);
our $g = "a scalar of the same name";

package Lib;
our array<str> @g = ("a", 3);
func touch() void {
    push(@g, 5);
    $g[0] = 7;
    our array<int> @fl = (1, "q");
    push(@fl, "z");
    local @g = ("a", 1);
}

package main;
func main() int {
    push(@g, "s");
    $g[0] = "t";
    $gh{"k"} = "v";
    push(@fm, "w");
    @g = (1, "u");
    %gh = (a => "b");
    my str $e = $g[0];
    my int $n = shift(@fm);
    foreach my str $q (@g) { say($q); }
    $g = $g . "!";
    return 0;
}
SRC
out=$("$STRADA" --check gc.strada 2>&1)
check "our array initializer"        "gc.strada:2: error: storing a str value in '@g' (declared array<int>)" "$out"
check "our hash initializer"         "gc.strada:3: error: storing a str value in '%gh' (declared hash<int>)" "$out"
check "file-level my initializer"    "gc.strada:4: error: storing a str value in '@fm' (declared array<num>)" "$out"
check "our in another package"       "gc.strada:9: error: storing a int value in '@g' (declared array<str>)" "$out"
check "push onto that package's our" "gc.strada:11: error: storing a int value in '@g' (declared array<str>)" "$out"
check "element of that our"          "gc.strada:12: error: storing a int value in '@g' (declared array<str>)" "$out"
check "our in a function"            "gc.strada:13: error: storing a str value in '@fl' (declared array<int>)" "$out"
check "push onto a function's our"   "gc.strada:14: error: storing a str value in '@fl'" "$out"
check "local list"                   "gc.strada:15: error: storing a int value in '@g' (declared array<str>)" "$out"
check "push onto main's our"         "gc.strada:20: error: storing a str value in '@g' (declared array<int>)" "$out"
check "our element assignment"       "gc.strada:21: error: storing a str value in '@g' (declared array<int>)" "$out"
check "our hash element"             "gc.strada:22: error: storing a str value in '%gh' (declared hash<int>)" "$out"
check "push onto a file-level my"    "gc.strada:23: error: storing a str value in '@fm' (declared array<num>)" "$out"
check "our list assignment"          "gc.strada:24: error: storing a str value in '@g'" "$out"
check "our hash assignment"          "gc.strada:25: error: storing a str value in '%gh'" "$out"
check "our element read is typed"    "gc.strada:26: error: initializing '\$e' (declared str) with a int value" "$out"
check "shift of a file-level my"     "gc.strada:27: error: initializing '\$n' (declared int) with a num value" "$out"
check "foreach over an our"          "gc.strada:28: error: loop variable '\$q' (declared str) iterates over '@g' (declared array<int>)" "$out"
check "our: nothing else reported"   "compilation failed: 18 semantic errors" "$out"

# A function's declared element type is checked at its `return`s and where
# its result goes (called unqualified, as Pkg::f() and as ::f()).
cat > gr.strada <<'SRC'
use strict "types";
package Lib;
func strs() array<str> { return ("a", "b"); }
func cnt() hash<int> { my hash<int> %c = (a => 1); return %c; }
func bad1() array<int> { return (1, "x"); }
func bad2() array<int> { my array<str> @s = ("a"); return @s; }
func bad3() hash<int> { return (a => "x"); }
func bad4() array<int> { return strs(); }
func bad5() hash<int> { my hash<str> %s = (a => "b"); return %s; }
func take(array<int> @a) int { return scalar(@a); }
func use_them() void {
    my array<int> @a = strs();
    my array<int> @b = Lib::strs();
    my array<int> @c = ::strs();
    my hash<str> %h = cnt();
    push(@a, strs());
    @a = strs();
    foreach my int $v (strs()) { say($v); }
    take(strs());
}
package main;
our hash<str> %gh = ();
func half() num { return 0.5; }
func main() int {
    %gh = Lib::cnt();
    my hash<str> %hs = Lib::cnt();
    my array<int> @m = (1, Lib::strs());
    my int $i = half();
    return 0;
}
SRC
out=$("$STRADA" --check gr.strada 2>&1)
check "return list"            "gr.strada:5: error: storing a str value in the result of 'Lib::bad1' (declared to return array<int>)" "$out"
check "return typed array"     "gr.strada:6: error: putting the elements of '@s' (array<str>) into the result of 'Lib::bad2' (declared to return array<int>)" "$out"
check "return hash list"       "gr.strada:7: error: storing a str value in the result of 'Lib::bad3' (declared to return hash<int>)" "$out"
check "return a call"          "gr.strada:8: error: putting the result of 'Lib::strs' (array<str>) into the result of 'Lib::bad4'" "$out"
check "return typed hash"      "gr.strada:9: error: putting the values of '%s' (hash<str>) into the result of 'Lib::bad5' (declared to return hash<int>)" "$out"
check "result: unqualified"    "gr.strada:12: error: putting the result of 'Lib::strs' (array<str>) into '@a' (declared array<int>)" "$out"
check "result: Pkg::f()"       "gr.strada:13: error: putting the result of 'Lib::strs' (array<str>) into '@b'" "$out"
check "result: ::f()"          "gr.strada:14: error: putting the result of 'Lib::strs' (array<str>) into '@c'" "$out"
check "result: hash"           "gr.strada:15: error: putting the values of the result of 'Lib::cnt' (hash<int>) into '%h' (declared hash<str>)" "$out"
check "result: push"           "gr.strada:16: error: putting the result of 'Lib::strs' (array<str>) into '@a'" "$out"
check "result: assignment"     "gr.strada:17: error: putting the result of 'Lib::strs' (array<str>) into '@a'" "$out"
check "result: foreach"        "gr.strada:18: error: loop variable '\$v' (declared int) iterates over the result of 'Lib::strs' (declared array<str>)" "$out"
check "result: argument"       "gr.strada:19: error: argument 1 to 'take': expected array<int>, got array<str>" "$out"
check "result: into an our"    "gr.strada:25: error: putting the values of the result of 'Lib::cnt' (hash<int>) into '%gh'" "$out"
check "result: from main"      "gr.strada:26: error: putting the values of the result of 'Lib::cnt' (hash<int>) into '%hs'" "$out"
check "result: inside a list"  "gr.strada:27: error: putting the result of 'Lib::strs' (array<str>) into '@m'" "$out"
check "scalar result: Pkg-less call in main" "gr.strada:28: error: initializing '\$i' (declared int) with a num value" "$out"
check "returns: nothing else reported" "compilation failed: 17 semantic errors" "$out"

# Must compile: the same name as `our` in two packages with different
# element types, $g / @g / %g together, a file-level `my` declared twice, a
# parameter and a `my` hiding typed globals, an untyped `our` in a function,
# a module's global of the same name, a closure's `return` inside a typed
# function, array<int> into array<num>.
mkdir -p gclib
cat > gclib/OcMod.strada <<'SRC'
package OcMod;
use strict "types";
our array<str> @shared = ("m");
my array<str> @x = ("mod");
func names() array<str> { return @shared; }
func nx() int { return scalar(@x); }
1;
SRC
cat > gcok.strada <<'SRC'
use strict "types";
use lib "gclib";
use OcMod;
our array<int> @g = (1, 2);
our $g = "scalar g";
our hash<str> %g = (a => "b");
my array<int> @x = (1, 2);
our array<str> @shared = ("main");

package A;
our array<str> @t = ("a");
func add(str $s) void { push(@t, $s); }
func all() array<str> { return @t; }
func cnt() hash<int> { my hash<int> %c = (a => 1); return %c; }
package B;
our array<int> @t = (1);
func add(int $n) void { push(@t, $n); }
func all() array<int> { return @t; }
func shadow(array<str> @g) int { push(@g, "p"); my array<str> @t = ("l"); push(@t, "m"); return scalar(@g) + scalar(@t); }
func untyped() int { our @u; push(@u, "a"); push(@u, 1); return scalar(@u); }
func cl() array<int> { my $f = func () { return "str"; }; my $r = $f->(); return (1, 2); }

package main;
my array<str> @x = ("later", "decl");
func main() int {
    push(@g, 3); $g{"c"} = "d"; $g = $g . "!";
    A::add("b"); B::add(2);
    my array<str> @as = A::all();
    my array<int> @bs = B::all();
    my hash<int> %c = A::cnt();
    my array<num> @ns = B::all();
    push(@x, "s");
    push(@shared, "m2");
    my array<str> @mn = OcMod::names();
    my int $sum = 0;
    foreach my int $v (B::all()) { $sum = $sum + $v; }
    say(join(",", @g) . " " . $g . " " . $g{"c"} . " " . join(",", @as) . " " . join(",", @bs) . " " . $c{"a"} . " " . scalar(@ns) . " " . join(",", @x) . " " . join(",", @shared) . " " . join(",", @mn) . " " . OcMod::nx() . " " . B::shadow("z") . " " . B::untyped() . " " . scalar(B::cl()) . " " . $sum);
    return 0;
}
SRC
GCOK_EXPECT="1,2,3 scalar g! d a,b 1,2 1 2 later,decl,s main,m2 m 1 2 2 2 3"
"$STRADA" gcok.strada -o gcok 2>gcok.err
check "same-named globals compile and run" "$GCOK_EXPECT" "$(./gcok 2>&1; cat gcok.err)"

# A package function named like a builtin: the builtin is what an
# unqualified call means, and it is not typed as that function.
cat > bn.strada <<'SRC'
use strict "types";
package Lib;
func sort(scalar $self) array<str> { return ("a", "b"); }
func reverse(scalar $self) array<str> { return ("a", "b"); }
func keys(scalar $self) array<str> { return ("a", "b"); }
func shift(scalar $self) int { return 1; }
func length(scalar $self) str { return "x"; }
func take(array<int> @a) int { return scalar(@a); }
func first { my str $n = shift; return $n; }
func go() int {
    my array<int> @n = (3, 1, 2);
    my hash<int> %h = (a => 1);
    my array<int> @s = sort(@n);
    my array<int> @ints = ();
    push(@ints, reverse(@n));
    my array<int> @ks = (1);
    foreach my int $v (sort(@n)) { push(@ks, $v); }
    my int $len = length("abc");
    return take(sort(@n)) + scalar(@s) + scalar(@ints) + $len;
}
package main;
func main() int { say(Lib::go() . Lib::first("q")); return 0; }
SRC
"$STRADA" bn.strada -o bn 2>bn.err
check "builtin names are not taken for package functions" "12q" "$(./bn 2>&1; cat bn.err)"

# ---- class element types: array<Dog>, hash<str, Dog> -----------------------
cat > ce.strada <<'SRC'
use strict "types";
package Animal;
has ro str $name (required);
func speak(scalar $self, int $n) str { return $self->name() . ":" . $n; }
package Dog;
extends Animal;
func fetch(scalar $self, str $what) str { return $what; }
package Cat;
extends Animal;
package Rock;
has ro int $w = 1;
package main;
our array<Dog> @kennel = (Cat::new(name => "k"));
func dogs() array<Dog> {
    return (Dog::new(name => "a"), Rock::new());
}
func cats() array<Cat> { my array<Cat> @c = (); return @c; }
func take(array<Dog> @d) int { return 0; }
func main() int {
    my array<Dog> @dogs = (Dog::new(name => "rex"), Cat::new(name => "tom"));
    push(@dogs, Rock::new());
    push(@dogs, 5);
    push(@dogs, "Dog");
    $dogs[0] = 1.5;
    my $an = Animal::new(name => "x");
    push(@dogs, $an);
    my array<Cat> @cats = @dogs;
    my array<int> @nums = (1, 2);
    my array<Dog> @d2 = @nums;
    my hash<str, Dog> %h = (a => Cat::new(name => "c"));
    $h{"b"} = Rock::new();
    my array<Dog> @d3 = cats();
    take(@cats);
    foreach my $d (@dogs) { say($d->fetch(1.5)); }
    say($dogs[0]->speak("two"));
    my $one = $dogs[0];
    say($one->fetch(2.5));
    my array<Rock> @rocks = ();
    push(@rocks, $dogs[0]);
    unshift(@kennel, Rock::new());
    return 0;
}
func walk(array<Dog> @pack) str { return $pack[0]->fetch(1.5); }
func first_of(array<Dog> @pack) str { return shift(@pack)->fetch(2.5); }
func reg() void { our array<Dog> @reg = (); push(@reg, Rock::new()); }
SRC
out=$("$STRADA" --check ce.strada 2>&1)
check "class: our initializer"      "ce.strada:13: error: storing a 'Cat' object in '@kennel' (declared array<Dog>)" "$out"
check "class: return list"          "ce.strada:15: error: storing a 'Rock' object in the result of 'dogs' (declared to return array<Dog>)" "$out"
check "class: initializer"          "ce.strada:20: error: storing a 'Cat' object in '@dogs' (declared array<Dog>)" "$out"
check "class: push"                 "ce.strada:21: error: storing a 'Rock' object in '@dogs'" "$out"
check "class: int"                  "ce.strada:22: error: storing a int value in '@dogs' (declared array<Dog>)" "$out"
check "class: class-name string"    "ce.strada:23: error: storing a str value in '@dogs'" "$out"
check "class: element store"        "ce.strada:24: error: storing a num value in '@dogs'" "$out"
check "class: exact parent object"  "ce.strada:26: error: storing a 'Animal' object in '@dogs'" "$out"
check "class: sibling container"    "ce.strada:27: error: putting the elements of '@dogs' (array<Dog>) into '@cats' (declared array<Cat>)" "$out"
check "class: int container"        "ce.strada:29: error: putting the elements of '@nums' (array<int>) into '@d2' (declared array<Dog>)" "$out"
check "class: hash list"            "ce.strada:30: error: storing a 'Cat' object in '%h' (declared hash<Dog>)" "$out"
check "class: hash store"           "ce.strada:31: error: storing a 'Rock' object in '%h'" "$out"
check "class: call result"          "ce.strada:32: error: putting the result of 'cats' (array<Cat>) into '@d3' (declared array<Dog>)" "$out"
check "class: argument"             "ce.strada:33: error: argument 1 to 'take': expected array<Dog>, got array<Cat>" "$out"
check "class: foreach receiver"     "ce.strada:34: error: argument 1 to 'Dog::fetch': expected str, got num" "$out"
check "class: element receiver"     "ce.strada:35: error: argument 1 to 'Animal::speak': expected int, got str" "$out"
check "class: variable from element" "ce.strada:37: error: argument 1 to 'Dog::fetch': expected str, got num" "$out"
check "class: element into another" "ce.strada:39: error: storing a 'Dog' object in '@rocks' (declared array<Rock>)" "$out"
check "class: unshift on our"       "ce.strada:40: error: storing a 'Rock' object in '@kennel'" "$out"
check "class: parameter element"    "ce.strada:43: error: argument 1 to 'Dog::fetch': expected str, got num" "$out"
check "class: shift receiver"       "ce.strada:44: error: argument 1 to 'Dog::fetch': expected str, got num" "$out"
check "class: function-level our"   "ce.strada:45: error: storing a 'Rock' object in '@reg' (declared array<Dog>)" "$out"
check "class: error count"          "compilation failed: 22 semantic errors" "$out"

# Must compile: subclass and role consumer in a base container, a module's
# class, objects of unknown class, undef, a reassigned variable, a container
# that may hold the subclass, array<scalar> both ways, a class no package
# declares, AUTOLOAD and `around` methods on element reads.
mkdir -p celib
cat > celib/CeMod.strada <<'SRC'
package CeMod::Bird;
has ro str $name (required);
func sing(scalar $self, int $times) str { return $self->name() x $times; }
1;
SRC
cat > ceok.strada <<'SRC'
use strict "types";
use lib "celib";
use CeMod;

package Walker;
func legs(scalar $self) int { return 4; }

package Animal;
has ro str $name (required);
func speak(scalar $self, int $n) str { return $self->name() . ":" . $n; }

package Dog;
extends Animal;
with Walker;
func fetch(scalar $self, str $what) str { return $self->name() . " fetches " . $what; }

package Cat;
extends Animal;
around "speak" func(scalar $self, scalar $orig, scalar $n) str { return "cat"; }

package Loose;
func new(str $class) scalar { my hash %h = (); return bless(\%h, $class); }
func AUTOLOAD(scalar $self) str { return "auto"; }

package main;

our array<Animal> @zoo = ();
my hash<str, Dog> %byname = ();

func make(str $n) scalar { return Dog::new(name => $n); }
func pack_of() array<Dog> {
    my array<Dog> @p = (Dog::new(name => "a"), Dog::new(name => "b"));
    return @p;
}
func count(array<Animal> @as) int { return scalar(@as); }

func main() int {
    my array<Dog> @dogs = (Dog::new(name => "rex"));
    push(@dogs, Dog::new(name => "fido"));
    push(@dogs, make("m"));              # unknown class: passes
    push(@dogs, undef);
    my $later = Dog::new(name => "z");
    $later = make("y");
    push(@dogs, $later);
    my array<Animal> @animals = (Dog::new(name => "d"), Cat::new(name => "c"), Animal::new(name => "an"));
    push(@animals, $dogs[0]);
    @animals = @dogs;                    # Dogs are Animals
    my array<Dog> @down = @animals;      # may hold Dogs: allowed
    my array<Walker> @walkers = (Dog::new(name => "w"));
    my array<scalar> @any = @dogs;
    my array<Dog> @from_any = @any;
    my array<CeMod::Bird> @birds = (CeMod::Bird::new(name => "tw"));
    my array<Loose> @loose = (Loose::new("Loose"));
    my array<Nowhere> @nw = ();
    push(@nw, make("q"));
    foreach my $d (@dogs) {
        if (defined($d)) { say($d->fetch("ball")); }
    }
    my $first = $dogs[0];
    say($first->speak(2));
    say($dogs[1]->fetch("stick"));
    say($animals[1]->speak(3));
    say($birds[0]->sing(2));
    say($loose[0]->anything(1, 2, 3));
    $byname{"rex"} = $dogs[0];
    say($byname{"rex"}->legs());
    my $popped = pop(@walkers);
    say($popped->legs());
    push(@zoo, Cat::new(name => "tom"));
    say($zoo[0]->speak("x"));            # around modifier: unchecked
    say(count(@dogs) . " " . scalar(pack_of()));
    my array<Dog> @more = pack_of();
    say($more[1]->name());
    return 0;
}
SRC
CEOK_EXPECT="rex fetches ball fido fetches ball m fetches ball y fetches ball rex:2 fido fetches stick fido:3 twtw auto 4 4 cat 5 2 b "
"$STRADA" ceok.strada -o ceok 2>ceok.err
check "class containers compile and run" "$CEOK_EXPECT" "$(./ceok 2>&1 | tr '\n' ' '; cat ceok.err)"
# Two roles with a common consumer are related (a Duck is both); an
# overloaded operator returns whatever the overload returns; a qualified
# class name.
cat > ceov.strada <<'SRC'
use strict "types";
package Walker; func legs(scalar $self) int { return 2; }
package Swimmer; func fins(scalar $self) int { return 0; }
package Duck; with Walker; with Swimmer; has ro int $n = 1;
package Vec;
has ro num $v = 1.0;
func mul(scalar $self, scalar $o, int $rev) scalar { return Vec::new(v => 2.0); }
func add(scalar $self, scalar $o, int $rev) scalar { return Vec::new(v => 2.0); }
func cat(scalar $self, scalar $o, int $rev) scalar { return Vec::new(v => 2.0); }
func neg(scalar $self, scalar $o, int $rev) scalar { return Vec::new(v => 2.0); }
use overload "*" => "mul", "+" => "add", "." => "cat", "neg" => "neg";
package My::Deep; has ro int $q = 1;
package main;
func main() int {
    my array<Walker> @walkers = (Duck::new());
    my array<Swimmer> @swimmers = ();
    push(@swimmers, $walkers[0]);
    my array<Swimmer> @s2 = @walkers;
    my $v = Vec::new();
    my $w = Vec::new();
    my array<Vec> @vecs = ();
    push(@vecs, $v * 2.0);
    push(@vecs, $v + $w);
    push(@vecs, $v . $w);
    push(@vecs, -$v);
    push(@vecs, $v * 2);
    my array<My::Deep> @deep = (My::Deep::new());
    my hash<str, My::Deep> %dh = ();
    say(scalar(@vecs) . " " . scalar(@swimmers) . " " . scalar(@s2) . " " . scalar(@deep));
    return 0;
}
SRC
"$STRADA" ceov.strada -o ceov 2>ceov.err
check "shared descendants and overloads compile" "5 1 1 1" "$(./ceov 2>&1; cat ceov.err)"
# inherit() at run time: the declared classes say nothing certain
cat > cedyn.strada <<'SRC'
use strict "types";
package A; func v(scalar $self) int { return 1; }
package B; has ro int $w = 2;
package main;
func main() int {
    inherit("B", "A");
    my array<A> @as = (B::new());
    say($as[0]->v());
    return 0;
}
SRC
"$STRADA" cedyn.strada -o cedyn 2>cedyn.err
check "runtime inherit() turns the class check off" "1" "$(./cedyn 2>&1; cat cedyn.err)"

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
    out=$("$INTERP" gc.strada 2>&1)
    check "VM reports our-container errors" "gc.strada:20: error: storing a str value in '@g' (declared array<int>)" "$out"
    out=$("$INTERP" gr.strada 2>&1)
    check "VM reports returned-container errors" "gr.strada:12: error: putting the result of 'Lib::strs' (array<str>) into '@a'" "$out"
    out=$("$INTERP" ce.strada 2>&1)
    check "VM reports class-element errors" "ce.strada:21: error: storing a 'Rock' object in '@dogs'" "$out"
    check "VM runs class containers" "$CEOK_EXPECT" "$("$INTERP" ceok.strada 2>&1 | tr '\n' ' ')"
    check "VM runs same-named globals" "$GCOK_EXPECT" "$("$INTERP" gcok.strada 2>&1)"
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

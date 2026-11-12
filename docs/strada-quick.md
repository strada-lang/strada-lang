# Strada quick guide for future coding

This is compact working memory for writing and maintaining Strada programs in
this repository or in another Strada project. Strada is experimental and still
evolves, so use this order of authority when sources disagree:

1. the checked-out compiler and runtime;
2. passing programs in `t/`, `examples/`, and `lib/`;
3. this guide;
4. the longer documents in `docs/`.

Do not treat Perl compatibility as a specification. Strada deliberately borrows
Perl's sigils, references, operators, regexes, and idioms, but it has its own
parser, type annotations, native runtime, and module system.

## Fast working loop

From a Strada source checkout:

```bash
make                              # build compiler, runtime, tools, and libraries
./strada -r app.strada            # compile and run
./strada app.strada               # compile to ./app
./strada -o bin/app app.strada    # choose output path
./strada --check app.strada       # syntax + semantic check only; no C, no binary
./strada -w --strict-types app.strada
./strada -O0 --no-lto -g -c app.strada  # useful diagnostic build; keep generated C
./strada --tcc app.strada         # compile with tcc: fastest build, unoptimized code
./strada --repl                   # full-language JIT REPL
./strada --help                   # current driver options
```

With an installed toolchain, use `strada` instead of `./strada`. The default
optimization is suitable for release-like runs; `-O0 --no-lto` shortens the
edit/compile cycle when debugging.

`--check` is the fastest feedback loop: it stops before any C is written, exits
0 when the file is clean, and otherwise reports every error it finds (not just
the first) as `file:line:col: message` with the source line and a caret. Run it
after each edit, before paying for a full compile.

For repository changes:

```bash
make test                         # C runtime tests
make test-suite                  # language regression suite
make test-suite FILTER=regex     # tests whose names match a term
make test-selfhost               # compiler self-hosting smoke test
```

For a focused change, first compile the affected `.strada` file directly, then
run the narrowest relevant test, then the broader suite. Read nearby examples
before inventing an API: `rg 'feature_name' examples t lib docs` is often the
fastest way to find current usage.

## Smallest complete program

```strada
func main() int {
    say("Hello, Strada!");
    return 0;
}
```

Use this conventional entry point for executables. Command-line arguments can
be accepted explicitly:

```strada
func main(int $argc, array @argv) int {
    for (my int $i = 0; $i < $argc; $i = $i + 1) {
        say($i . ": " . $argv[$i]);
    }
    return 0;
}
```

Source uses `#` comments, semicolons, and brace-delimited blocks. Files normally
end in `.strada`.

## The essential mental model

Strada is a typed, Perl-inspired language that compiles to C and then to a native
executable. Most values use a common runtime representation; integers have a
fast tagged representation. Explicit types make intent clearer and can enable
better generated code, especially in arithmetic-heavy paths.

### Types, sigils, and declarations

```strada
my int $count = 0;
my num $ratio = 0.75;
my str $name = "Ada";
my bool $ready = 1;               # represented as an integer
my scalar $anything = undef;      # any single value or reference
my array @items = ("a", "b");
my hash %config = { "port" => 8080, "debug" => 1 };

our str $mode = "normal";         # package-scoped global
const int MAX_RETRIES = 3;         # compile-time constant; no sigil
```

The main types are `int`, `num`, `str`, `bool`, `scalar`, `array`, `hash`, and
`void`. A function may return `dynamic` when its result deliberately depends on
scalar, array, or hash context. Fixed-width and C-facing types include `int8`,
`int16`, `int32`, `int64`, their `uint` variants, `float`, `double`, `size_t`,
`byte`, and `char`.

The sigil is part of how a value is used:

| Form | Meaning |
|---|---|
| `$value` | scalar value |
| `@items` | whole array or list context |
| `%index` | whole hash or hash context |
| `$items[0]` | one array element |
| `$index{"key"}` | one hash value |
| `@items[0, 2]` | array slice, still a list |
| `@index{"a", "b"}` | hash slice, still a list |

The most important rule is that a single container element always starts with
`$`. Writing `@items[0]` asks for a one-element slice, not a scalar element.

An omitted type is allowed, but it defaults from the sigil:

```strada
my $x = 42;              # scalar slot, not an explicitly typed int slot
my @xs = (1, 2, 3);      # array
my %seen = ();           # hash
```

Prefer explicit types at public boundaries and for hot numeric variables.

### Definedness, truth, and conversion

`undef`, `0`, `0.0`, and `""` are false; other values are true. Definedness and
truth are different:

```strada
if (defined($value)) { ... }

my int $port = $config{"port"} // 8080;  # only undef selects 8080
my str $label = $name || "anonymous";    # any false value selects fallback

my int $n = int("42");
my num $x = "3.5" + 0;                   # numeric context converts
my str $text = "" . $n;                  # concatenation converts
my str $kind = typeof($value);
```

`int(...)` is the only conversion function. `num` and `str` are type keywords,
so `num("3.5")` and `str($n)` are parse errors; a declared `num` or `str`
variable, arithmetic, or concatenation does the conversion instead.

Use `//` for defaults that must preserve valid `0` or empty-string values. Use
`||` only when every false value should fall through.

### Operators

```strada
$a + $b   $a - $b   $a * $b   $a / $b   $a % $b   $a ** $b
$a == $b  $a != $b  $a < $b   $a <= $b  $a <=> $b  # numeric
$a eq $b  $a ne $b  $a lt $b  $a le $b  $a cmp $b  # string
$a && $b  $a || $b  !$a        $a // $fallback
$text . $suffix                "-" x 40
$n += 1   $n++                 $text .= "more"
```

Do not use numeric comparison for text or string comparison for numbers. Use
`<=>` in numeric sort callbacks and `cmp` in string sort callbacks.

## Collections and references

### Arrays

```strada
my array @nums = (10, 20, 30);
push(@nums, 40, 50);
my scalar $last = pop(@nums);
unshift(@nums, 0);
my scalar $first = shift(@nums);

my int $count = scalar(@nums);       # size(@nums) also works
my array @part = @nums[0, 2];
my array @doubled = map { $_ * 2; } @nums;
my array @positive = grep { $_ > 0; } @nums;
my array @ordered = sort { $a <=> $b; } @nums;

my ($left, $right) = @nums;          # destructuring
my array @flat = (1, @nums, 99);     # arrays flatten in list literals
my array @range = (1..10);           # inclusive range
```

Negative indices address from the end. Assignment beyond the current end grows
the array. `splice` removes or replaces a range:

```strada
my array @removed = splice(@nums, 2, 3);
splice(@nums, 1, 0, 88, 99);         # insert without deleting
```

When the expected capacity is known, `my array @rows[1000];` or
`reserve(@rows, 1000);` avoids repeated growth.

### Hashes

```strada
my hash %counts = { "red" => 2, "blue" => 1 };
$counts{"red"} = $counts{"red"} + 1;

if (exists($counts{"blue"})) {
    delete($counts{"blue"});
}

foreach my str $key (keys(%counts)) {
    say($key . " => " . $counts{$key});
}

my array @values = values(%counts);
my int $key_count = scalar(keys(%counts));
```

Hash order should not be treated as stable. Bare keys such as `$h{name}` mean
the literal key `"name"`; `$h{$name}` uses the variable. Quote keys when there
is any chance of ambiguity. Nested lvalue assignment autovivifies hashes:

```strada
$tree{"users"}{$id}{"active"} = 1;
```

Use `my hash %cache[500];` to provide an initial capacity hint.

### References

```strada
my int $n = 10;
my array @items = (1, 2, 3);
my hash %data = { "ok" => 1 };

my scalar $nref = \$n;
my scalar $aref = \@items;
my scalar $href = \%data;

my scalar $anonymous_array = [10, 20];
my scalar $anonymous_hash = { "name" => "Ada" };

say($$nref);
say($aref->[0]);
say($href->{"ok"});
$aref->[1] = 99;
```

Use arrow access on references and objects. `ref($value)` reports a reference
kind or class; `isa($object, "Class")` checks class inheritance. The runtime is
reference-counted and also has cycle handling, but `core::weaken($ref)` remains
useful for explicit non-owning back-references and caches.

## Control flow and functions

```strada
if ($score >= 90) {
    say("A");
} elsif ($score >= 80) {
    say("B");
} else {
    say("other");
}

unless ($ready) { prepare(); }
while ($running) { tick(); }
until ($done) { poll(); }

for (my int $i = 0; $i < 10; $i = $i + 1) { say($i); }
foreach my str $item (@items) { say($item); }
foreach (@items) { say($_); }          # implicit element

next if $skip;
last if $finished;
```

`last`, `next`, and `redo` correspond roughly to break, continue, and restart
the current iteration. Loop labels are available for nested loops. Postfix
modifiers include `if`, `unless`, `while`, and `until`.

Functions have parameter types and a return type. `fn` is an alias for `func`:

```strada
func add(int $a, int $b) int {
    return $a + $b;
}

func greet(str $name, str $prefix = "Hello") void {
    say($prefix . ", " . $name);
}

func sum(int ...@values) int {
    my int $total = 0;
    foreach my int $value (@values) { $total += $value; }
    return $total;
}

my array @args = (2, 3, 4);
my int $total = sum(1, ...@args);      # spread only at a call site
```

Anonymous functions are closures and capture surrounding variables by
reference:

```strada
my int $factor = 3;
my scalar $scale = fn (int $value) int {
    return $value * $factor;
};
say($scale->(14));                     # 42

my scalar $named_ref = \&add;
say($named_ref->(20, 22));
```

The no-parentheses declaration form uses implicit `@_` and is useful for
Perl-style compatibility, but typed parameter lists are clearer for new code:

```strada
func first { return shift; }           # shift defaults to @_
```

Return explicitly from non-`void` functions. Falling off the end produces
`undef`, which is usually a bug.

## Strings and regular expressions

Double quotes and `qq(...)` process escapes and interpolate **only the braced
form `${name}`**, and only for a plain scalar variable. A bare `$name` inside a
string is literal text, which is the opposite of Perl and fails silently: the
program compiles and prints `$name`. Single quotes and `q(...)` are fully
literal. `qw(...)` creates a word list.

```strada
my str $name = "Ada";
my str $message = "Hello, ${name}\n";       # Hello, Ada
my str $literal = "Hello, $name\n";         # Hello, $name  (no interpolation)
my str $joined = "Total: " . $counts[0] . " for " . $user->{"name"};
my array @words = qw(one two three);

length($message);
substr($message, 0, 5);
index($message, "Ada");                # -1 when absent
uc($message); lc($message); trim($message);
split(/\s+/, $message);
join(",", @words);
sprintf("%s: %d", $name, 42);
```

Elements, hash values, method calls, and arrays do not interpolate in any form
(`"${items[0]}"` is a compile error); concatenate them with `.` or use
`sprintf`.

Heredocs place the semicolon on the opening line and follow the same
interpolation rule:

```strada
my str $body = <<EOT;
Hello, ${name}
This is a second line.
EOT
```

Regex syntax is Perl-like and uses PCRE2 where configured:

```strada
if ($text =~ /(?P<word>\w+)\s+(\d+)/i) {
    say($1);
    my array @all = captures();
    my hash %named = named_captures();
}

$text =~ s/old/new/;                    # first match
$text =~ s/old/new/g;                   # all matches
$text =~ tr/a-z/A-Z/;
```

Use `=~` and `!~` for matching. Remember that `split(/./, $text)` splits on any
character; escape a literal dot as `split(/\./, $text)`.

## Errors, diagnostics, and I/O

Exceptions may be strings or blessed objects. Put specific typed catches before
the catch-all:

```strada
try {
    if (!$valid) { throw "bad input"; }
    risky_operation();
} catch (ValidationError $error) {
    warn($error->{"message"});
} catch ($error) {
    warn("failed: " . $error);
} finally {
    cleanup();
}
```

`throw` is a statement and takes no postfix modifier: `throw "x" unless $ok;` is
a parse error. Wrap it in an `if`, or use `die($message) unless $ok;`.
`die($message)` throws, while uncaught exceptions print a Strada stack trace.
Useful diagnostics are `core::stack_trace()`, `core::caller()`, and
`core::dumper($value)`. Compile with `-g` for source line information and `-c`
to inspect generated C.

Basic terminal and file I/O:

```strada
say("line with newline");
print("without newline");
warn("to stderr");
printf("count=%d\n", $count);

my str $all = core::slurp("input.txt");
core::spew("output.txt", $all);

my scalar $fh = core::open("input.txt", "r");
die("cannot open input.txt") unless defined($fh);
my str $line = <$fh>;
while (defined($line)) {
    say($line);
    $line = <$fh>;
}
core::close($fh);

say(STDERR, "explicit error output");
```

Prefer `core::` for system facilities. `sys::` is a legacy alias. File-test
operators include `-e`, `-f`, `-d`, `-r`, `-w`, and `-x`.

## Packages and modules

A source module normally declares a package and ordinary functions. Its path
mirrors its namespace:

```text
project/
|-- app.strada
`-- lib/
    `-- Acme/
        `-- Text.strada
```

```strada
# lib/Acme/Text.strada
package Acme::Text;
version "1.0.0";

func shout(str $text) str {
    return uc($text);
}
```

```strada
# app.strada
use lib "lib";
use Acme::Text;

func main() int {
    say(Acme::Text::shout("hello"));
    return 0;
}
```

`use` resolves a `.strada` source module and may use a fresh sibling compiled
artifact. For a source-exact diagnostic build, pass `--no-use-artifacts`.
Reusable build forms include:

```bash
strada -M lib/Acme/Text.strada           # sibling module object
strada --shared plugin.strada -o plugin.so
strada --static-lib library.strada -o library.a
```

Explicit loading/linking statements are `import_lib "X.so"`,
`import_object "X.o"`, and `import_archive "X.a"`. Use them when the source
module is intentionally not the input. Use `link_lib "name";` inside a module
that must propagate an external C library dependency to consumers.

Built-in namespaces such as `core::`, `math::`, `async::`, `thread::`, and
`c::` do not need `use`.

## Object-oriented Strada

Prefer declarative OOP for ordinary domain objects. `has` creates attributes and
accessors, `extends` establishes inheritance, and a constructor is generated:

```strada
package Animal;
has ro str $species (required);
has rw int $energy = 100;

func speak(scalar $self) void {
    say($self->species() . " energy=" . $self->energy());
}

package Dog;
extends Animal;
has ro str $name (required);

func bark(scalar $self) void {
    say($self->name() . " barks");
}

package main;

func main() int {
    my scalar $dog = Dog::new(
        "name", "Rex",
        "species", "dog",
        "energy", 80
    );
    $dog->bark();
    $dog->speak();
    $dog->set_energy(70);                # generated for rw only
    say($dog->isa("Animal"));
    return 0;
}
```

The generated constructor takes alternating key/value arguments. `ro` generates
a getter; `rw` also generates `set_name(value)`. Available attribute options
include defaults, `(required)`, and lazy builders. `before`, `after`, and
`around` method modifiers and `with` role composition are available when needed.

Bless-style objects give full control and interoperate with the same method
dispatch system:

```strada
package Counter;

func new(int $start) scalar {
    my hash %self = { "value" => $start };
    return bless(\%self, "Counter");
}

func increment(scalar $self) void {
    $self->{"value"} += 1;
}

func value(scalar $self) int {
    return $self->{"value"};
}

package main;

func main() int {
    my scalar $counter = Counter::new(10);
    $counter->increment();
    say($counter->value());
    return 0;
}
```

Executable statements must live inside a function. Only declarations
(`func`, `package`, `use`, `our`, `const`, `has`, ...) are allowed at the top
level of a file.

A method is a package function whose first argument is `$self`. Use
`$object->method(args)` for dispatch, `$object->isa("Class")` for type checks,
and `$object->can("method")` for capability checks. `DESTROY` is called during
object destruction. See `docs/OOP_GUIDE.md` before implementing inheritance,
roles, method modifiers, overloading, or `AUTOLOAD` behavior.

## Async, threads, and FFI

Treat these as opt-in subsystems and consult their dedicated references before
designing around them.

The basic async shape is:

```strada
async func work(int $id) int {
    return $id * 10;
}

# inside a function:
my scalar $future = work(4);
my int $answer = await $future;

my scalar $channel = async::channel(16);
async::send($channel, "message");
my str $message = async::recv($channel);
async::close($channel);
```

Also available are future combinators, cancellation, task spawning, channels,
mutexes, atomics, OS threads, an event loop, actors, and scoped task groups. Read
`docs/extended/async.md` and `docs/EVENT_LOOP.md`; use passing examples because
concurrency APIs are an active area of development.

For C integration, prefer declared `extern` functions and driver `-I`, `-L`, and
`-l` options. `__C__ { ... }` blocks expose runtime `StradaValue *` values and
therefore require exact ownership handling. Before writing one, read
`docs/FFI_GUIDE.md` and `docs/RUNTIME_API.md`. In particular, never dereference a
runtime value as a heap pointer before checking for a tagged integer, balance
reference counts when retaining/replacing values, and free buffers returned by
conversion helpers when the API says ownership is transferred.

## Common built-ins and libraries

High-frequency built-ins:

| Area | Operations |
|---|---|
| output/errors | `say`, `print`, `printf`, `warn`, `die` |
| types | `defined`, `typeof`, `ref`, `isa`, `int` |
| arrays | `push`, `pop`, `shift`, `unshift`, `splice`, `scalar`, `size`, `sort`, `map`, `grep` |
| hashes | `keys`, `values`, `each`, `exists`, `delete` |
| strings | `length`, `substr`, `index`, `rindex`, `split`, `join`, `trim`, `uc`, `lc`, `sprintf` |
| references/OOP | `bless`, `can`, `core::weaken`, `core::blessed` |
| files/process | `core::open`, `close`, `slurp`, `spew`, `readdir`, `getenv`, `qx`, `fork`, `exec`, `wait` |
| math | `math::abs`, `sqrt`, `pow`, `floor`, `ceil`, `round`, trigonometry, logs, random numbers |

Standard modules live under `lib/`. Check there before adding dependencies or
reimplementing a facility. Notable modules include `JSON`, `YAML`, `DateTime`,
`List::Util`, `Getopt::Long`, `Test`, `Exception`, `DBI`, `LWP`, `Template`,
`Text::CSV`, `Digest::MD5`, and `Digest::SHA`. Some require optional native
libraries selected during `configure`/`make`.

## Failure patterns to catch during review

- Write `"${name}"`, never `"$name"`, to interpolate; a bare `$name` in a
  string is printed literally with no warning. Concatenate everything that is
  not a plain scalar variable.
- Put every executable statement inside a function; there is no top-level
  script code.
- Convert with `int(...)`, arithmetic, or concatenation; `num(...)` and
  `str(...)` do not exist.
- Do not put `if`/`unless` after `throw`.
- Use `$array[$i]` and `$hash{$key}` for elements; `@array[...]` and
  `@hash{...}` are slices.
- Use `eq`/`ne`/`cmp` for strings and `==`/`!=`/`<=>` for numbers.
- Use `//` rather than `||` when `0` or `""` is a valid configured value.
- Do not assume an untyped `$value` is an `int` variable merely because its
  current value is numeric.
- Quote hash keys unless a bare literal is unmistakable.
- Call a closure/reference with `$fn->(...)` and spread call arguments with
  `...@args`.
- Do not index strings as arrays; use `substr`, `length`, and `index`.
- Return a value explicitly from every non-`void` path.
- Do not assume hash iteration order.
- Check `defined($fh)` after opening a file and handle the failure.
- Avoid copying Perl APIs by memory; confirm Strada spelling in `lib/`,
  `examples/`, `t/`, or `docs/BUILTIN_FUNCTIONS.md`.
- Keep `core::` on system/runtime calls in new code; bare or `sys::` forms may
  exist only for compatibility.
- When a fresh compiled module artifact might hide source changes, reproduce
  with `--no-use-artifacts`.
- Do not edit generated `compiler/Combined.strada` or generated C as the primary
  fix. Change the component source, then rebuild.

## A reliable implementation checklist

When creating or changing a Strada project:

1. Locate current examples and tests for every nontrivial feature.
2. Choose package paths and public function signatures before implementation.
3. Use explicit types for public APIs, counters, indices, and numeric hot paths.
4. Keep scalar/container sigils and element/slice syntax consistent.
5. Run `strada --check` after each edit, and compile the smallest runnable file
   with `-w --strict-types` early.
6. Add a regression test that exercises success, empty input, false-but-defined
   values, and the relevant error path.
7. Run the focused test and then the appropriate project suite.
8. For crashes or generated-code problems, retry with
   `-O0 --no-lto -g -c`, inspect the C, and use the stack trace or debugger.
9. For performance work, measure with `-p` or `--full-profile` before changing
   ownership, allocation, or representation-sensitive code.

## Where to go deeper

- `docs/QUICK_REFERENCE.md` — broad syntax and feature catalog
- `docs/LANGUAGE_MANUAL.md` — detailed language reference
- `docs/BUILTIN_FUNCTIONS.md` — built-in functions
- `docs/OOP_GUIDE.md` — classes, inheritance, roles, and overloading
- `docs/extended/sys.md` — `core::`/system APIs
- `docs/extended/regex.md` — regex behavior and flags
- `docs/extended/types.md` — conversions and context
- `docs/extended/async.md` and `docs/EVENT_LOOP.md` — concurrency
- `docs/FFI_GUIDE.md` and `docs/RUNTIME_API.md` — C interop and ownership
- `docs/strada.1.md` and `./strada --help` — driver behavior

The final rule is simple: compile the code. Strada's toolchain is fast enough
that a tiny executable or regression test is more trustworthy than reasoning
from Perl familiarity or stale documentation.

# Strada for Claude

A self-contained reference for writing Strada code. No links to other docs — everything you need is here.

---

## What Strada is

Strada is a strongly-typed, Perl-inspired language that **compiles to C, then to native executables via gcc**. Type annotations are optional; the sigil determines the default. The runtime uses a `StradaValue*` representation with **tagged-integer optimization** (small ints encoded directly in pointer bits, no heap allocation) and reference counting for everything else.

**File extension:** `.strada` — runs of Strada source live in files with this extension.

**Compile + run:**

```bash
./strada hello.strada       # compile to ./hello executable
./strada -r hello.strada    # compile and run
./strada -c hello.strada    # also keep the generated .c
./strada -O2 hello.strada   # optimize (LTO enabled at -O2+)
./strada -O3 hello.strada   # LTO + -march=native
./strada -g hello.strada    # debug symbols
./strada --shared lib.strada     # build .so shared library
./strada --static-lib lib.strada # build .a static library
```

**Hello world:**

```strada
func main() int {
    say("Hello, world!");
    return 0;
}
```

A program needs a `func main() int { ... return 0; }` entry point. The return type is `int`, and `0` means success (standard exit code convention).

---

## Variables and sigils

Three sigils, each defaulting to a type:

| Sigil | Used for | Default type |
|---|---|---|
| `$` | scalars, plus element access on `@arr[0]` and `%hash{"key"}` | `scalar` |
| `@` | whole-array operations | `array` |
| `%` | whole-hash operations | `hash` |

Element access always uses `$` regardless of the container's sigil:

```strada
my array @arr = (1, 2, 3);
my hash %h = ("a" => 1, "b" => 2);
say($arr[0]);          # 1
say($h{"a"});          # 1
$arr[0] = 99;          # mutate
$h{"c"} = 3;           # add
```

### Declarations

`my` — local (lexical) scope. `our` — package-scoped (global). Type annotations are optional:

```strada
my $x = 42;            # type inferred from sigil → scalar
my int $count = 0;
my num $pi = 3.14159;
my str $name = "Strada";
my array @items = (1, 2, 3);
my hash %config = ("port" => 8080, "host" => "localhost");

our int $GLOBAL_COUNTER = 0;
```

`local $var = expr;` does dynamic scoping — saves/restores the value across the rest of the enclosing block.

### Primitive types

| Type | Description |
|---|---|
| `int` | 64-bit integer (tagged-int optimized) |
| `num` | Double-precision float |
| `str` | UTF-8 string |
| `scalar` | Any value (dynamic) |
| `void` | No return value (functions only) |
| `dynamic` | Context-sensitive return (see below) |

Composite: `array`, `hash`. C-interop: `int8`, `int16`, `uint8`/`byte`, `uint16`, `uint32`, `uint64`, `size_t`, `char`, `float`, `double`.

The `dynamic` return type lets a function react to whether its result is being assigned to a scalar, array, or hash — call `core::wantarray()`, `core::wantscalar()`, or `core::wanthash()` from inside.

### Conversion

Implicit coercions follow Perl's rules. Explicit coercions:

```strada
my int $i = int($str);     # parse string → int
my num $n = num($str);     # parse string → float
my str $s = str($i);       # int → string
say(typeof($val));         # returns "int", "num", "str", "array", "hash", "ref", "undef", ...
```

### Scalar context and list flattening

Assigning an array to a scalar yields its length:

```strada
my array @arr = (1, 2, 3);
my int $n = @arr;          # n = 3
my $sn = scalar(@arr);     # same — scalar() forces scalar context
```

Arrays in list literals flatten by default:

```strada
my array @a = (10, 20);
my array @flat = (1, @a, 99);   # (1, 10, 20, 99)
```

---

## Operators

### Arithmetic

`+`, `-`, `*`, `/`, `%`, `**` (exponentiation). Postfix `$i++` / prefix `++$i`, same for `--`.

### Numeric comparison

`==`, `!=`, `<`, `>`, `<=`, `>=`, `<=>` (spaceship: returns -1, 0, or 1).

### String comparison

`eq`, `ne`, `lt`, `gt`, `le`, `ge`, `cmp` (string spaceship).

**Sort comparators:** `<=>` for numeric, `cmp` for string:

```strada
my array @sorted = sort { $a <=> $b; } @nums;
my array @byname = sort { $a cmp $b; } @strings;
```

### Logical

`&&`, `||`, `//` (defined-or), `!`. Low-precedence aliases: `and`, `or`, `not`.

`||` returns the actual value, not a boolean (Perl-style):

```strada
my str $name = $h{"name"} || "default";
```

`//` only treats `undef` as falsy:

```strada
my int $port = $cfg{"port"} // 8080;   # 0 stays as 0; only undef → default
```

### String

- `.` — concatenation: `"hello " . $name`
- `x` — repetition: `"-" x 40` produces 40 dashes
- Interpolation in double-quoted strings (see Strings section).

### Assignment

`=`, `+=`, `-=`, `*=`, `/=`, `.=`, `//=`. Compound assignments work with `int`-typed variables and stay on the tagged-int fast path.

### Bitwise

`&`, `|`, `^`, `~`, `<<`, `>>` — operate on integer values.

---

## Control flow

```strada
if ($x > 0) { ... } elsif ($x < 0) { ... } else { ... }
unless ($cond) { ... }
while ($cond) { ... }
until ($cond) { ... }
for (my int $i = 0; $i < 10; $i = $i + 1) { ... }
foreach my scalar $item (@arr) { ... }
foreach (@arr) { say($_); }       # $_ is the implicit element
do { ... } while ($cond);
```

`elsif` and `else if` are interchangeable.

### Labels for loop control

```strada
OUTER: for (my int $i = 0; $i < 10; $i = $i + 1) {
    INNER: foreach my scalar $x (@arr) {
        last OUTER if $x == 0;
        next INNER if $x < 0;
        redo INNER;
    }
}
```

Bare `last`, `next`, `redo` apply to the innermost loop.

### Statement modifiers

```strada
say("verbose mode") if $verbose;
return 0 unless $ok;
$i = $i + 1 while $i < 10;
print($_) for @arr;
```

---

## Functions

The keyword `func` declares a function. `fn` is an alias and works anywhere `func` does.

### Basic forms

```strada
func add(int $a, int $b) int { return $a + $b; }
func greet(str $name) void { say("Hello, " . $name); }
func default_ret($x) { return $x + 1; }    # params and return default to scalar
```

### Anonymous functions / closures

```strada
my scalar $double = func(int $n) int { return $n * 2; };
my int $r = $double->(21);     # 42

# Closures capture surrounding variables by reference
my int $count = 0;
my scalar $tick = func() void { $count = $count + 1; };
$tick(); $tick(); say($count);  # 2
```

### Variadic and spread

```strada
func sum(int ...@nums) int {
    my int $s = 0;
    foreach my int $n (@nums) { $s = $s + $n; }
    return $s;
}
sum(1, 2, 3);

my array @vals = (10, 20);
sum(1, ...@vals, 99);    # spread an array into call args
```

### No-parens form (implicit `@_`)

```strada
func process { say($_[0] . " — " . $_[1]); }
func total { my int $s = 0; foreach my int $v (@_) { $s = $s + $v; } return $s; }

process("status", "ok");
total(1, 2, 3, 4);
```

`shift` / `pop` with no args default to `@_` inside the function:

```strada
func first { my $x = shift; return $x; }
```

### Private functions

```strada
private func helper(int $x) int { return $x * 2; }
```

Private functions are `static` in C and not exported from a module.

### Async functions

```strada
async func fetch(str $url) str { return http_get($url); }

my scalar $f = fetch("http://example.com");
my str $content = await $f;
```

### Function references / first-class

A bare function name passed to a scalar variable is taken as a reference:

```strada
my scalar $fn = \&add;
my int $r = $fn->(2, 3);       # 5
```

---

## Strings

### Quoting

```strada
"double quoted"          # interpolates variables and \n, \t, etc.
'single quoted'          # raw, no interpolation
q(text)                  # like single quotes
qq(text $var)            # like double quotes
qw(one two three)        # word list — same as ("one","two","three")
```

### Interpolation

```strada
my str $name = "World";
say("Hello, $name!");                       # Hello, World!
say("Total: ${count}");                     # explicit braces
say("Sum: ${\ ($a + $b) }");                # arbitrary expression
say("Item: $arr[0]");                       # array element
say("Key: $hash{name}");                    # hash element (bare key)
```

### Heredocs

```strada
my str $body = <<EOT;
Multi-line
text here.
EOT

my str $raw = <<'EOT';
No interpolation, \n stays literal.
EOT

my str $explicit = <<"EOT";
Same as bare <<EOT.
EOT
```

The semicolon goes on the `<<EOT;` line.

### Common string operations

```strada
length($s)
substr($s, $start, $len)
index($s, $needle)              # -1 if not found
rindex($s, $needle)             # last occurrence
uc($s)        lc($s)            # case
ucfirst($s)   lcfirst($s)
trim($s)
chr(65)                         # "A"
ord("A")                        # 65
"a-b-c" =~ /-/                  # split-style with regex
split(/,/, $csv)                # → array
join(",", @arr)                 # → string
reverse($s)                     # reversed string (or array if @-sigil arg)
sprintf("%d/%d", $a, $b)
```

Many string built-ins default to `$_` when called with no arg: `chomp`, `uc`, `lc`, `length`, `ucfirst`, `lcfirst`, `trim`, `defined`, `chr`, `ord`, `say`, `print`, `chop`.

### `.=`, `x`

```strada
my str $buf = "";
$buf .= "line\n";
my str $sep = "=" x 60;
```

---

## Arrays

```strada
my array @arr = (10, 20, 30);

# Element access (always with $)
$arr[0];                        # 10
$arr[-1];                       # 30 — negative indexing
$arr[5] = 99;                   # auto-extends with undef in gaps

# Whole-array operations
push(@arr, 40, 50);             # append
my $last = pop(@arr);
unshift(@arr, 0);               # prepend
my $first = shift(@arr);
scalar(@arr);                   # length
reverse(@arr);                  # in-place reverse
sort(@arr);                     # default string sort
sort { $a <=> $b; } @arr;       # numeric sort
splice(@arr, 1, 2);             # remove 2 elements starting at index 1
splice(@arr, 1, 0, 88, 99);     # insert 88, 99 at index 1
```

### Slices

```strada
my array @subset = @data[0, 2, 4];          # explicit indices
my array @range = @data[0..5];              # range
```

### Map / grep

```strada
my array @doubled = map { $_ * 2; } @nums;
my array @evens = grep { $_ % 2 == 0; } @nums;

# map with arrow (pair form) — useful for hash conversion
my hash %lookup = map { $_ => 1; } @keys;
```

### Destructuring

```strada
my ($a, $b, $c) = @arr;
my (int $x, str $y) = @mixed;
```

### Pre-allocation (perf)

```strada
my array @data[1000];           # capacity hint
reserve(@arr, 1000);
```

### Ranges

```strada
my array @r = (1..10);          # (1,2,3,4,5,6,7,8,9,10)
foreach my int $i (1..100) { ... }
```

---

## Hashes

```strada
my hash %h = ("alice" => 30, "bob" => 25);

# Access
$h{"alice"};                    # 30
$h{"new_key"} = 99;             # add or update

# Iteration
foreach my str $k (keys(%h)) { say($k . " = " . $h{$k}); }
foreach my int $v (values(%h)) { ... }

# each() returns [key, value] pairs; iterator state lives in the hash
my array @pair = each(%h);
while (scalar(@pair) > 0) {
    say($pair[0] . " => " . $pair[1]);
    @pair = each(%h);
}

# Existence and deletion
if (exists($h{"alice"})) { ... }
delete($h{"bob"});
```

### Slices

```strada
my array @vals = @h{"a", "b", "c"};         # values for those keys
my ($name, $city) = @user{"name", "city"};  # destructure
```

### Pre-allocation

```strada
my hash %cache[500];
core::hash_default_capacity(1000);
```

### Autovivification

```strada
my hash %h = ();
$h{"a"}{"b"}{"c"} = 42;         # auto-creates intermediate hashrefs
```

---

## References and dereferencing

Strada uses Perl-style references. `\` takes a reference; `$$`, `@$`, `%$` (or `${...}`, `@{...}`, `%{...}`) dereference.

```strada
my int $x = 10;
my array @arr = (1, 2, 3);
my hash %h = ("k" => 1);

my scalar $sref = \$x;
my scalar $aref = \@arr;
my scalar $href = \%h;

# Anonymous constructors
my scalar $anon_arr  = [10, 20, 30];        # arrayref
my scalar $anon_hash = {name => "Alice"};   # hashref

# Dereferencing
$$sref;                          # 10
@$aref;                          # (1, 2, 3) — flattens
$aref->[0];                      # 1 (arrow is preferred for refs)
$href->{"k"};                    # 1

${$sref};                        # explicit
@{$aref};
%{$href};
```

### Weak references

Break circular cycles with `core::weaken($ref)`. A weak reference doesn't keep its target alive. Check with `core::isweak($ref)`. After the target is freed, dereferencing returns undef.

```strada
my hash %parent = ();
my hash %child = ();
$child{"parent"} = \%parent;
$parent{"child"} = \%child;
core::weaken(\$parent{"child"});      # break the cycle
```

---

## Regular expressions

Strada uses **PCRE2** when available (with a POSIX fallback for builds without PCRE2).

### Match and capture

```strada
if ($text =~ /(\w+)\s+(\d+)/) {
    say($1);                    # first capture group
    say($2);                    # second capture group
    my array @caps = captures();
    my hash %nc = named_captures();
}
```

Match modifiers:
- `i` — case insensitive
- `g` — global (all matches)
- `s` — dot matches newline (PCRE2)
- `m` — multiline (`^`/`$` match at line breaks)
- `x` — extended (whitespace/comments allowed in pattern)

### Substitution

```strada
$str =~ s/old/new/;                  # first match
$str =~ s/old/new/g;                 # all matches
$str =~ s/(\d+)/uc($1)/eg;           # /e: replacement is Strada code
```

### Transliteration

```strada
$str =~ tr/a-z/A-Z/;                 # uppercase
$str =~ y/abc/xyz/;                  # y/// is the same as tr///
```

### Default-target matching

`m/PATTERN/` (or just `/PATTERN/`) matches against `$_` if no target is given:

```strada
foreach (@lines) {
    if (/error/i) { ... }
}
```

### PCRE2 features available

`*?`, `+?` (non-greedy), `\b` (word boundary), `(?:...)` (non-capturing), `(?=...)` (lookahead), `(?!...)` (negative lookahead), `(?<=...)` (lookbehind), `(?P<name>...)` (named captures), backreferences `$1`-`$9` in replacement strings.

---

## OOP

Strada has two complementary OOP styles:

1. **Bless-style** (Perl-classic): `bless` a hashref into a package; methods are package functions taking `$self` as first arg.
2. **Moose-style** (declarative): `has`/`extends`/`with` and method modifiers — the compiler generates boilerplate.

You can mix them freely.

### Moose-style (preferred for most code)

```strada
package Animal;
has ro str $species (required);     # read-only, must be passed to new()
has rw int $energy = 100;           # read-write with default

func speak(scalar $self) void {
    say($self->species() . " (energy: " . $self->energy() . ")");
}

package Dog;
extends Animal;                     # inheritance
has ro str $name (required);

before "bark" func(scalar $self) void { say("[preparing]"); }
func bark(scalar $self) void { say($self->name() . " barks!"); }
after "bark" func(scalar $self) void { say("[done]"); }

package main;

func main() int {
    my scalar $d = Dog::new("name", "Rex", "species", "dog");
    $d->speak();           # inherited
    $d->bark();            # before/bark/after fire in order
    say($d->isa("Animal"));  # 1
    say($d->isa("Dog"));     # 1
    say($d->can("bark"));    # 1
    return 0;
}
```

### `has` syntax

```
has [ro|rw] TYPE $NAME [= DEFAULT] [(OPTIONS)];
```

| Element | Meaning |
|---|---|
| `ro` | Generate getter only (default if omitted) |
| `rw` | Generate getter + `set_NAME(val)` setter |
| `= DEFAULT` | Default value if not passed to constructor |
| `(required)` | Must be passed to constructor |
| `(lazy)` | Computed on first access |
| `(builder => "method_name")` | Function that computes the lazy value |

### Auto-generated constructor

When a package has `has` attributes and no explicit `func new(...)`, the compiler generates a variadic constructor accepting key-value pairs:

```strada
my scalar $p = Person::new("name", "Alice", "age", 30);
```

If you write your own `new`, the auto-generated one is suppressed.

### Method modifiers

```strada
before "method" func(scalar $self, ...) { ... }
after  "method" func(scalar $self, ...) { ... }
around "method" func(scalar $self, scalar $orig, ...) { ... }
```

`around` wraps the original — call `$orig->($self, @args)` to invoke it.

### Multiple inheritance

```strada
package Duck;
extends Swimmer, Flyer;     # comma-separated parents
```

### Roles (`with`)

```strada
package Loggable;
func log(scalar $self, str $msg) void { say($self->name() . ": " . $msg); }

package Service;
with Loggable;              # mixes in methods from Loggable
has ro str $name (required);
```

### Bless-style (when you need full control)

```strada
package Counter;

func new(int $start) scalar {
    my hash %self = ();
    $self{"value"} = $start;
    return bless(\%self, "Counter");
}

func increment(scalar $self) void {
    $self->{"value"} = $self->{"value"} + 1;
}

func value(scalar $self) int {
    return $self->{"value"};
}

package main;

func main() int {
    my scalar $c = Counter::new(10);
    $c->increment();
    say($c->value());       # 11
    return 0;
}
```

### Dynamic method dispatch

```strada
my str $method = "bark";
$obj->$method();            # calls method whose name is in $method
$obj->$method($arg);
$obj->$method;              # without parens (accessor style)
```

### Operator overloading

```strada
package Vector;
has rw num $x = 0;
has rw num $y = 0;

func add(scalar $self, scalar $other, int $reversed) scalar {
    my scalar $v = Vector::new();
    $v->set_x($self->x() + $other->x());
    $v->set_y($self->y() + $other->y());
    return $v;
}

func to_str(scalar $self) str {
    return "(" . $self->x() . ", " . $self->y() . ")";
}

use overload
    "+"  => "add",
    '""' => "to_str",
    "fallback" => 1;
```

Supported operators: `+`, `-`, `*`, `/`, `%`, `**`, `.`, `""`, `==`, `!=`, `<`, `>`, `<=`, `>=`, `<=>`, `eq`, `ne`, `cmp`, `neg`, `!`, `~`, `bool`. Zero overhead when not used.

### `AUTOLOAD`

```strada
package Proxy;
func AUTOLOAD(scalar $self) scalar {
    my str $method = $AUTOLOAD;        # the called name
    return "called: " . $method;
}
```

### `isa`, `can`, `DESTROY`

```strada
$obj->isa("ClassName");                # is $obj this class or a subclass?
$obj->can("method_name");              # does it have this method?

func DESTROY(scalar $self) void { ... }   # called when refcount hits 0
```

---

## Exception handling

```strada
try {
    risky();
    throw "string error";
    throw MyError::new("details");
} catch (FileNotFound $e) {            # typed catch (uses isa())
    say($e->{"path"});
} catch (IOException $e) {
    say($e->{"message"});
} catch ($e) {                         # catch-all (must be last)
    say("Unknown: " . $e);
} finally {
    cleanup();
}
```

Exceptions are scalar values. Throwing a blessed object lets typed catches match by class. Strada also accepts plain string throws.

Uncaught exceptions print a stack trace and exit. Disable trace with `--no-stack-trace` flag, or programmatically:

```strada
my scalar $info = core::caller();        # immediate caller
say($info->{"function"});
say($info->{"file"});
say($info->{"line"});
my scalar $up = core::caller(1);          # caller's caller
core::set_recursion_limit(5000);
my str $trace = core::stack_trace();
```

---

## File I/O

File handles are reference-counted scalars; auto-closed on scope exit. Modes: `"r"`, `"w"`, `"a"`, `"rb"`, `"wb"`, or `<`, `>`, `>>`.

```strada
my scalar $fh = core::open("file.txt", "r");
while (defined(my str $line = <$fh>)) {  # diamond op strips newline
    say($line);
}
core::close($fh);    # explicit close optional — also auto on scope exit

# Whole-file shortcuts
my str $data = core::slurp("file.txt");
core::spew("file.txt", $data);

# Capture command output
my str $out = core::qx("ls -la");

# Standard handles available as barewords
say(STDOUT, "to stdout");
say(STDERR, "error message");
print(STDERR, "debug\n");
my str $line = <STDIN>;
```

### File tests

```strada
if (-e $path) { say("exists"); }
if (-f $path) { say("is a file"); }
if (-d $path) { say("is a directory"); }
if (-r $path) { say("readable"); }
if (-w $path) { say("writable"); }
if (-x $path) { say("executable"); }
```

### Seek / tell / eof / flush

```strada
core::seek($fh, $offset, 0);     # 0=SEEK_SET, 1=SEEK_CUR, 2=SEEK_END
my int $pos = core::tell($fh);
if (core::eof($fh)) { ... }
core::flush($fh);
```

### Pipe and process I/O

```strada
my scalar $pipe = core::open("|gzip > file.gz", "w");
print($pipe, $data);
core::close($pipe);

my scalar $rd = core::open("ls -la|", "r");
while (defined(my str $line = <$rd>)) { say($line); }
```

### Directory operations

```strada
my array @entries = core::readdir($path);  # excludes . and ..
core::mkdir($path);
core::rmdir($path);
core::chdir($path);
my str $cwd = core::getcwd();
```

---

## Async, threading, and concurrency

```strada
async func fetch(str $url) str { return http_get($url); }

# Single
my scalar $f = fetch("http://example.com");
my str $r = await $f;

# Combinators
my array @futures = (fetch($u1), fetch($u2), fetch($u3));
my array @results = await async::all(@futures);
my scalar $first = await async::race(@futures);
my scalar $r2 = await async::timeout($f, 5000);   # ms; throws on timeout
async::cancel($f);
```

### Channels (CSP-style)

```strada
my scalar $ch = async::channel();           # unbounded
my scalar $bch = async::channel(100);       # bounded buffer
async::send($ch, "msg");
my str $m = async::recv($ch);
async::close($ch);
```

### Mutex, atomic

```strada
my scalar $m = async::mutex();
async::lock($m);
... critical section ...
async::unlock($m);

my scalar $counter = async::atomic(0);
async::atomic_add($counter, 1);
my int $v = async::atomic_get($counter);
async::atomic_cas($counter, $expected, $new);
```

The default thread pool has 4 workers; tune with `async::set_pool_size($n)`.

---

## Modules

### Source-level inclusion

```strada
use lib "lib";          # add to module search path
use MyModule;           # compile-time inclusion of MyModule.strada
```

### Runtime dynamic loading

```strada
import_lib "MyLib.so";          # dlopen, reads metadata from __strada_export_info()
import_object "MyLib.o";        # static .o linking
import_archive "MyLib.a";       # static .a linking

MyLib::function(args);
```

### Module versioning

```strada
version "1.0.0";        # at module top — used by use/import_lib
```

### `import` hook

A module can define a function to run on use:

```strada
func import(str $pkg, array @list) void {
    foreach my str $name (@list) { ... }
}
```

### `BEGIN` / `END` blocks

```strada
BEGIN { say("runs before main"); }
END   { say("runs after main, LIFO order"); }
```

---

## Magic namespaces

Built-in namespaces that don't need importing:

| Namespace | Scope |
|---|---|
| `core::` | System, libc, I/O, profiling, introspection |
| `sys::` | Legacy alias for `core::` (both work) |
| `math::` | `sin`, `cos`, `sqrt`, `pow`, `floor`, `log`, `rand`, etc. |
| `async::` | `all`, `race`, `timeout`, `channel`, `mutex`, `atomic`, ... |
| `c::` | Low-level: `c::alloc`, `c::free`, `c::is_null`, ... |
| `utf8::` | `utf8::is_utf8`, `utf8::valid`, `utf8::downgrade`, ... |
| `usb::` | USB device access (libusb required) |
| `ssl::` | TLS/SSL sockets: `ssl::connect`, `ssl::read`, `ssl::write` |

`core::` examples:

```strada
core::open(...), core::close(...), core::slurp(...), core::spew(...), core::qx(...)
core::fork(), core::exec(...), core::wait(), core::pipe()
core::getenv("HOME"), core::setenv("X", "1")
core::caller(), core::stack_trace()
core::pack("NnC", $a, $b, $c)
core::unpack("NnC", $packed)
core::base64_encode($bytes), core::base64_decode($s)
core::time(), core::sleep($s)
core::weaken(\$ref), core::isweak(\$ref)
core::full_profile_start("file.prof"), core::full_profile_stop()
core::global_set/get/exists/delete/keys()       # 'our' var registry
```

`math::` examples:

```strada
math::abs($n), math::sqrt($n), math::pow($base, $exp)
math::sin($n), math::cos($n), math::tan($n)
math::asin, math::acos, math::atan, math::atan2($y, $x)
math::sinh, math::cosh, math::tanh
math::exp($n), math::log($n), math::log10($n)
math::floor($n), math::ceil($n), math::round($n)
math::rand()              # 0.0 ≤ x < 1.0
```

---

## Built-in functions (no-namespace core)

### I/O

| Function | Notes |
|---|---|
| `say($msg)` | Print with newline |
| `print($msg)` | Print without newline |
| `warn($msg)` | To stderr |
| `die($msg)` | Throw — caught like an exception |
| `select($fh)` | Set default output filehandle, returns previous |

### Type / introspection

| Function | Notes |
|---|---|
| `typeof($v)` | Returns `"int"`, `"num"`, `"str"`, `"array"`, `"hash"`, `"ref"`, `"undef"`, etc. |
| `defined($v)` | True unless undef |
| `ref($v)` | Reference type name, or `""` for non-refs |
| `int($v)`, `num($v)`, `str($v)` | Convert |
| `chr($n)`, `ord($c)` | Numeric ↔ character |

### Container ops

| Function | Notes |
|---|---|
| `push(@a, ...)`, `pop(@a)`, `shift(@a)`, `unshift(@a, ...)` | Array endpoint ops |
| `scalar(@a)` | Length |
| `splice(@a, $offset, $len?, @repl?)` | Remove/replace; returns removed |
| `reverse(@a)` | In-place |
| `sort(@a)` or `sort { ... } @a` | Sort with optional comparator block |
| `map { EXPR } @a` | Transform |
| `grep { EXPR } @a` | Filter |
| `keys(%h)`, `values(%h)`, `each(%h)` | Hash iteration |
| `exists($h{$k})`, `delete($h{$k})` | Hash key tests |

### Strings

| Function | Notes |
|---|---|
| `length($s)`, `substr($s, $off, $len?)` | |
| `index($s, $needle)`, `rindex($s, $needle)` | -1 if not found |
| `uc`, `lc`, `ucfirst`, `lcfirst`, `trim`, `chomp`, `chop` | |
| `split(/regex/, $s, $limit?)` | Returns array |
| `join($delim, @a)` | Returns string |
| `sprintf($fmt, ...)` | printf-style |

### Binary data

```strada
my str $packed = core::pack("NnC", 0x12345678, 80, 255);   # network byte order
my array @parts = core::unpack("NnC", $packed);
my int $b = core::ord_byte($s);
my int $c = core::get_byte($s, $pos);
my int $len = core::byte_length($s);
my str $b64 = core::base64_encode($data);
my str $raw = core::base64_decode($b64);
```

---

## `__C__` blocks (C interop)

Embed C inline. Variables visible inside are `StradaValue*` pointers.

```strada
package mylib;

__C__ {
#include <mylib.h>
static int global_state = 0;
}

func process(str $data) int {
    my int $result = 0;
    __C__ {
        char *str = strada_to_str(data);     /* allocates — must free */
        int ret = my_c_function(str);
        strada_decref(result);
        result = strada_new_int(ret);
        free((char*)str);
    }
    return $result;
}
```

### Key runtime APIs inside `__C__`

| Function | Notes |
|---|---|
| `strada_to_int(sv)` | Returns `int64_t`. Handles tagged ints. No free needed. |
| `strada_to_num(sv)` | Returns `double`. No free needed. |
| `strada_to_str(sv)` | Returns allocated `char*`. **Must `free()`** the result. |
| `strada_new_int(i)` | Returns tagged-int `StradaValue*` (no heap if in range). |
| `strada_new_num(n)` | Heap-allocates a STRADA_NUM. |
| `strada_new_str(s)` | Heap-allocates a STRADA_STR (copies the string). |
| `strada_incref(sv)`, `strada_decref(sv)` | Refcount management. Decref before reassigning. |
| `STRADA_IS_TAGGED_INT(sv)` | Macro: 1 if `sv` is a tagged int. |
| `STRADA_TAGGED_INT_VAL(sv)` | Macro: extract the int from a tagged int. |
| `STRADA_MAKE_TAGGED_INT(val)` | Macro: build a tagged int. |

### Critical rules in `__C__`

- **Tagged ints are immortal** — `strada_incref`/`strada_decref` are no-ops on them. Treat as plain values.
- Before accessing `sv->type`, `sv->value`, `sv->meta`, or `sv->refcount`, **always check `STRADA_IS_TAGGED_INT(sv)` first** — tagged ints are not valid heap pointers. Or use `strada_to_int/str/num()` which handle this transparently.
- `strada_to_str()` returns a freshly-allocated `char*`. **Always `free()` after use.**
- When reassigning a `StradaValue*` field, decref the old value first:
  ```c
  strada_decref(p->some_field);
  p->some_field = strada_new_int(42);
  ```

---

## Constants and enums

```strada
const int MAX_ITEMS = 100;       # global → C #define
const str VERSION = "1.0.0";

enum Status { PENDING = 0, ACTIVE = 10, DONE = 20 }
my int $s = Status::ACTIVE;
```

---

## `tie` / `untie` / `tied`

Custom variable backends. Define a class with `FETCH`/`STORE`/`EXISTS`/`DELETE`/`FIRSTKEY`/`NEXTKEY`/`CLEAR` methods, then `tie` a variable to it. Untied variables pay no overhead.

```strada
my hash %config;
tie(%config, "ConfigFile", "/etc/app.conf");
say($config{"port"});           # FETCH dispatches to ConfigFile->FETCH(...)
$config{"port"} = 8080;         # STORE
untie(%config);
```

---

## Common idioms

### Default value with `//`

```strada
my int $n = $args{"count"} // 100;
```

### Conditional assignment

```strada
$cache{$key} //= compute_expensive($key);
```

### Sort numerically

```strada
my array @sorted = sort { $a <=> $b; } @nums;
```

### Build a string

```strada
my str $out = "";
foreach my str $line (@lines) { $out .= $line . "\n"; }
```

### Accumulator from a hash

```strada
my int $total = 0;
foreach my int $v (values(%counts)) { $total = $total + $v; }
```

### Count occurrences

```strada
my hash %counts = ();
foreach my str $word (@words) { $counts{$word} = ($counts{$word} // 0) + 1; }
```

### Build a hash from pairs

```strada
my hash %map = map { $_ => 1; } @keys;
```

### Method chain

```strada
$obj->set_x(1)->set_y(2)->save();   # if setters return $self
```

For Moose-style `rw` setters to support chaining you'd write a custom setter that `return $self;`. By default they return void.

---

## Common gotchas

- **Sigil for element access is `$`, not the container's sigil.** `@arr[0]` is a *slice* (one-element list); `$arr[0]` is the scalar element. Most of the time you want `$arr[0]` and `$h{"k"}`.
- **`||` vs `//`.** `||` treats `0`, `""`, and `undef` as false; `//` only treats `undef` as false. Use `//` for "default if not set" — otherwise `0` and `""` get clobbered.
- **`my $foo = 3` is a `scalar` slot, not an `int` slot.** The literal `3` is stored as a tagged int, but the variable's declared type is `scalar` because of the bare `$`. The compile-time int-arithmetic optimizations only fire when you write `my int $foo = 3`.
- **Hash keys with no quotes are barewords.** `$h{name}` works (treats `name` as `"name"`), but `$h{$name}` is variable interpolation. When in doubt, quote: `$h{"name"}`.
- **`return` is required if a function isn't `void`.** Falling off the end of a non-void function returns undef.
- **`bless` returns the same ref.** `return bless(\%self, "Pkg");` is the idiom — don't try to use the return value as a different reference.
- **Strings are not arrays.** Use `substr`, `length`, `index` rather than indexing characters with `$s[0]`. `$s[0]` would treat `$s` as an array.
- **`scalar(keys(%h))` for size.** Just `keys %h` in scalar context gives the count, but the explicit form is clearer and the compiler optimizes it to a single hash-size call.
- **Empty `@{$ref}` is `undef`-able.** If `$ref` is undef, `@{$ref}` is an empty list (autoviv'd in lvalue context, empty otherwise).
- **`split` regex:** the first arg is a regex pattern; metacharacters need escaping. `split(/,/, $csv)` — yes; `split(/./, $s)` matches *any* character — usually you want `split(/\./, $s)`.

---

## Performance tips

The compiler already does these automatically — these notes help you write code that hits the fast paths:

- **Type your hot variables as `int`.** Tagged-int arithmetic is zero-allocation: no heap, no refcount. `my int $i = 0;` in a loop counter is materially faster than `my $i = 0`.
- **Use `int` parameter types.** Int params skip incref/decref entirely (tagged ints are immortal).
- **`has rw int $x` accessors are inlined** to direct hash fetches — zero method-dispatch overhead. Same for setters.
- **`Pkg::new(name => val, ...)` constructors are inlined** to direct hash construction with precomputed key hashes — no method call to a `new()` function.
- **`$obj->isa("Pkg")` is folded at compile time** when `$obj`'s class is statically known (e.g. just assigned from `Pkg::new(...)`).
- **CSE for repeated accessor reads.** `$self->x() * $self->x()` fetches `x` once and reuses.
- **Pre-allocate containers** when you know the size: `my array @data[1000];`, `my hash %cache[500];`. Avoids resizes during fill.
- **Build with `-O2` or `-O3`.** `-O2` adds LTO automatically (cross-function inlining); `-O3` adds `-march=native`. The default is already `-O2`.
- **Profile before optimizing.** `./strada --full-profile prog.strada && ./prog` writes `strada-prof.out`; render with `tools/strada-proftext` (text) or `tools/strada-profhtml` (HTML).
- **`-p` for function-level timing.** Quicker than full profile; writes a summary to stderr.

---

## Stack traces and introspection

Uncaught exceptions print a stack trace by default. Programmatic access:

```strada
core::stack_trace();                # current stack as string
my scalar $info = core::caller();    # immediate caller
$info->{"function"}; $info->{"file"}; $info->{"line"};
my scalar $up = core::caller(1);     # one level up
core::set_recursion_limit(5000);
```

Disable trace globally with the `--no-stack-trace` compile flag.

---

## Compile-time flags

```bash
./strada -w prog.strada              # warn on unused vars
./stradac -t prog.strada out.c       # show compile phase timing
./strada -p prog.strada              # function-level profiling
./strada --full-profile prog.strada  # line-level profiling → strada-prof.out
./strada -O0 prog.strada             # no optimization, fast compile
./strada -O2 prog.strada             # default — LTO enabled
./strada -O3 prog.strada             # LTO + -march=native
./strada -Ofast prog.strada          # aggressive optimization
./strada --no-stack-trace prog.strada
```

---

## Testing your code

```bash
make test-suite                  # 148 tests
./t/run_tests.sh -v string       # verbose, only tests matching "string"
./t/run_tests.sh -t              # TAP format for CI
./t/leak_tests/run_leak_tests.sh # 75 leak tests
make test-selfhost               # the compiler can recompile itself (stage-2)
```

For ad-hoc testing of a single file:

```bash
./strada -r /tmp/mytest.strada
```

---

## Extended examples

### Simple OOP class with methods

```strada
package BankAccount;

has ro str $owner (required);
has rw num $balance = 0.0;

func deposit(scalar $self, num $amount) void {
    if ($amount <= 0) { throw "invalid amount"; }
    $self->set_balance($self->balance() + $amount);
}

func withdraw(scalar $self, num $amount) void {
    if ($amount > $self->balance()) { throw "insufficient funds"; }
    $self->set_balance($self->balance() - $amount);
}

func summary(scalar $self) str {
    return sprintf("%s: $%.2f", $self->owner(), $self->balance());
}

package main;

func main() int {
    my scalar $a = BankAccount::new("name", "Alice", "balance", 100.0);
    $a->deposit(50.0);
    try { $a->withdraw(1000.0); }
    catch ($e) { say("error: " . $e); }
    say($a->summary());
    return 0;
}
```

### Word frequency

```strada
func main() int {
    my str $text = core::slurp("input.txt");
    my array @words = split(/\s+/, lc($text));
    my hash %counts = ();
    foreach my str $w (@words) {
        next if length($w) == 0;
        $counts{$w} = ($counts{$w} // 0) + 1;
    }
    my array @sorted = sort { $counts{$b} <=> $counts{$a}; } keys(%counts);
    my int $i = 0;
    foreach my str $w (@sorted) {
        last if $i >= 20;
        say(sprintf("%6d  %s", $counts{$w}, $w));
        $i = $i + 1;
    }
    return 0;
}
```

### Recursive descent

```strada
func factorial(int $n) int {
    if ($n <= 1) { return 1; }
    return $n * factorial($n - 1);
}

func fib(int $n) int {
    if ($n < 2) { return $n; }
    return fib($n - 1) + fib($n - 2);
}

func main() int {
    say(factorial(10));     # 3628800
    say(fib(20));           # 6765
    return 0;
}
```

### Fork + pipe

```strada
func main() int {
    my array @pipe = core::pipe();
    my int $rd = $pipe[0];
    my int $wr = $pipe[1];

    my int $pid = core::fork();
    if ($pid == 0) {
        # child
        core::close_fd($rd);
        my scalar $w = core::fdopen($wr, "w");
        say($w, "hello from child");
        core::close($w);
        core::exit(0);
    } else {
        # parent
        core::close_fd($wr);
        my scalar $r = core::fdopen($rd, "r");
        my str $msg = <$r>;
        say("parent got: " . $msg);
        core::wait();
    }
    return 0;
}
```

### Async fan-out

```strada
async func work(int $id) int {
    core::sleep(1);
    return $id * 10;
}

func main() int {
    my array @futs = (work(1), work(2), work(3));
    my array @results = await async::all(@futs);
    foreach my int $r (@results) { say($r); }
    return 0;
}
```

---

## Quick checklist for AI-assisted Strada coding

When generating Strada code:

1. **Sigil discipline.** `$x` for scalars and elements, `@arr` only for whole-array ops, `%hash` only for whole-hash ops. Element access is always `$arr[i]` / `$hash{key}`.
2. **Type hints help.** Write `my int $x = 0;` instead of `my $x = 0;` in hot loops — the compiler emits tagged-int arithmetic.
3. **Prefer Moose-style OOP** (`has rw int $x`) over hand-written bless boilerplate. The compiler generates faster code for it (inline accessors, inline constructor).
4. **Use `//` for defaults**, not `||`, unless you specifically want `0`/`""` to fall through.
5. **`func main() int { return 0; }`** is the entry point of a complete program.
6. **`core::` for system calls**, `math::` for math, `async::` for concurrency. No `use` needed for these.
7. **Don't over-engineer.** Strada is Perl-flavored — small scripts and clean OOP both work without ceremony.
8. **Test locally:** `./strada -r yourfile.strada`. The compile-and-run roundtrip is fast.

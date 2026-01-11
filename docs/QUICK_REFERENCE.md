# Strada Quick Reference

## Compilation

```bash
# Build compiler
make

# One-step compile (recommended)
./strada program.strada          # Creates ./program
./strada -r program.strada       # Compile and run
./strada -c program.strada       # Keep .c file
./strada -g program.strada       # Debug symbols
./strada -w program.strada       # Enable warnings (unused vars)

# Using make
make run PROG=myprogram

# Manual two-step:
./stradac program.strada program.c        # Compile to C
./stradac -g program.strada program.c     # With debug line info
./stradac -t program.strada program.c     # Show timing stats
gcc -o program program.c runtime/strada_runtime.c -Iruntime -ldl
./program
```

## Magic Namespaces

Built-in functions are organized into namespaces:

| Namespace | Purpose | Example |
|-----------|---------|---------|
| `core::` | System/libc (preferred alias for `core::`) | `core::getenv()`, `core::getpid()` |
| `core::` | System/libc (backwards compatible) | `core::open()`, `core::fork()` |
| `math::` | Math functions | `math::sin()`, `math::sqrt()` |
| `thread::` | Multithreading | `thread::create()`, `thread::mutex_new()` |
| `async::` | Async, channels, mutex, atomics | `async::all()`, `async::channel()`, `async::mutex()` |
| *(none)* | Core language | `say()`, `push()`, `keys()` |

**Note:** `core::` is the preferred way to call system functions. It is an alias for `core::` and is normalized at compile time with zero runtime overhead. Both `core::getpid()` and `core::getpid()` generate identical code.

## Basic Structure

```strada
func main() int {
    say("Hello, World!");
    return 0;
}
```

## Variables

```strada
my int $count = 0;          # Integer
my num $price = 19.99;      # Float
my str $name = "Alice";     # String
my array @items = ();       # Array
my array @large[1000];      # Array with pre-allocated capacity
my hash %data = ();         # Hash
my hash %cache[500];        # Hash with pre-allocated capacity
my scalar $ref = \$count;   # Reference

# Constants (compile-time)
const int MAX_SIZE = 100;   # Compiles to #define
const str VERSION = "1.0";  # Global const
const num PI = 3.14159;     # No $ sigil required

# Package-scoped globals (our)
our int $count = 0;         # Backed by global registry
our str $name = "hello";    # Key: "main::count", "main::name"

package Config;
our str $host = "localhost"; # Key: "Config::host"
our int $port = 8080;        # Key: "Config::port"

# Dynamic scoping (local) - temporarily override our vars
our str $mode = "normal";
func risky() void {
    local($mode) = "safe";   # Temporarily "safe", restored on scope exit
}
```

## Data Types

| Type | Description | Example |
|------|-------------|---------|
| `int` | Integer | `42` |
| `num` | Float | `3.14` |
| `str` | String | `"hello"` |
| `array` | List | `@items` |
| `hash` | Key-value | `%data` |
| `scalar` | Any/ref | `$ref` |
| `void` | None | - |

## Operators

```strada
# Arithmetic
$a + $b    $a - $b    $a * $b    $a / $b    $a % $b    $a ** $b

# Increment/Decrement
$i++    # postfix: return old, then increment
++$i    # prefix: increment, then return new
$i--    --$i    # same for decrement

# String
$s1 . $s2              # Concatenation
$s x 3                 # Repetition: "ab" x 3 → "ababab"

# Comparison (numeric)
$a == $b   $a != $b   $a < $b   $a > $b   $a <= $b   $a >= $b

# Comparison (string)
$s1 eq $s2   $s1 ne $s2   $s1 lt $s2   $s1 gt $s2

# Logical
$a && $b   $a || $b   !$a

# Assignment
$x = 1   $x += 1   $x -= 1   $s .= "!"
```

## Control Flow

```strada
# If/elsif/else (elsif and else if are interchangeable)
if ($x > 0) {
    say("positive");
} elsif ($x < 0) {
    say("negative");
} else {
    say("zero");
}

# While
while ($i < 10) {
    $i = $i + 1;
}

# For
for (my int $i = 0; $i < 10; $i = $i + 1) {
    say($i);
}

# Foreach (iterate over arrays) - "for" and "foreach" are interchangeable
foreach my str $item (@array) {
    say($item);
}

for my str $item (@array) {  # Same as foreach
    say($item);
}

# Foreach with existing variable
my scalar $x;
foreach $x (@array) {
    say($x);
}

# Unless (negated if)
unless ($done) { say("still working"); }

# Until (negated while)
until ($done) { do_work(); }

# Loop control
last;   # break
next;   # continue
redo;   # restart current iteration (no condition recheck)

# Statement modifiers (postfix form)
say("hello") if $verbose;
say("warning") unless $quiet;
$i = $i + 1 while $i < 10;
$i = $i + 1 until $i >= 10;
return 0 if $error;
last if $done;
next unless $valid;

# Labeled loops (break/continue outer loops)
OUTER: foreach my int $i (@nums) {
    INNER: foreach my int $j (@nums) {
        if ($i * $j > 10) {
            last OUTER;   # break out of outer loop
        }
        if ($j == 2) {
            next OUTER;   # continue outer loop
        }
    }
}

# Goto and labels
RETRY:
$count = $count + 1;
if (!$success && $count < 3) {
    goto RETRY;
}
```

## Exception Handling

```strada
# Try/catch
try {
    my int $result = risky_operation();
} catch ($error) {
    say("Error: " . $error);
}

# Throw exception
if ($bad_input) {
    throw "Invalid input: " . $input;
}

# Nested try/catch
try {
    try {
        throw "inner";
    } catch ($e) {
        throw "rethrown";
    }
} catch ($e) {
    say($e);  # "rethrown"
}

# Stack traces (uncaught exceptions print automatically)
# Manual stack trace:
my str $trace = core::stack_trace();
say($trace);
```

## Functions

`fn` is shorthand for `func` and can be used anywhere `func` is used (function definitions, closures, `extern`, `async`, `private`, `before`/`after` hooks, etc.).

```strada
fn add(int $a, int $b) int {
    return $a + $b;
}

func greet(str $name, str $greeting = "Hello") void {
    say($greeting . ", " . $name);
}

# Variadic function - collects extra args into array
func sum(int ...@nums) int {
    my int $total = 0;
    foreach my int $n (@nums) { $total = $total + $n; }
    return $total;
}
sum(1, 2, 3, 4, 5);              # 15

# Fixed params + variadic
func format(str $prefix, int ...@nums) str { ... }

# Spread operator - unpack array into args
my array @vals = (10, 20, 30);
sum(...@vals);                   # 60
sum(1, ...@vals, 99);            # Mixed: 160

# Dynamic return type - context-sensitive (like Perl's wantarray)
func flexible() dynamic {
    if (core::wantarray()) { my array @r = (1, 2, 3); return @r; }
    return 42;
}
my array @a = flexible();        # Array context → (1, 2, 3)
my int $v = flexible();          # Scalar context → 42
```

## Anonymous Functions (Closures)

```strada
# Create closure - func without a name
my scalar $add = func (int $a, int $b) { return $a + $b; };
my scalar $greet = func (str $name) { return "Hello, " . $name; };
my scalar $get_pi = func () { return 3.14159; };

# Call with arrow syntax
my int $sum = $add->(3, 4);        # 7
my str $msg = $greet->("World");   # "Hello, World"
my num $pi = $get_pi->();          # 3.14159

# Capture variables from enclosing scope (by reference)
my int $multiplier = 10;
my scalar $scale = func (int $n) { return $n * $multiplier; };
say($scale->(5));                  # 50

# Mutations visible in outer scope
my int $count = 0;
my scalar $inc = func () { $count = $count + 1; return $count; };
$inc->();                          # returns 1
$inc->();                          # returns 2
say($count);                       # 2 (outer variable modified)

# Pass closures to functions
func apply(scalar $f, int $x) int {
    return $f->($x);
}
my scalar $double = func (int $n) { return $n * 2; };
say(apply($double, 5));            # 10
```

## Multithreading

```strada
# Create and join thread
my scalar $t = thread::create(func () {
    say("Hello from thread!");
});
thread::join($t);

# Mutex for shared data
my int $counter = 0;
my scalar $mutex = thread::mutex_new();

my scalar $worker = thread::create(func () {
    thread::mutex_lock($mutex);
    $counter = $counter + 1;
    thread::mutex_unlock($mutex);
});
thread::join($worker);
thread::mutex_destroy($mutex);

# Condition variables
my int $ready = 0;
my scalar $m = thread::mutex_new();
my scalar $cv = thread::cond_new();

my scalar $waiter = thread::create(func () {
    thread::mutex_lock($m);
    while ($ready == 0) {
        thread::cond_wait($cv, $m);
    }
    thread::mutex_unlock($m);
});

thread::mutex_lock($m);
$ready = 1;
thread::cond_signal($cv);
thread::mutex_unlock($m);
thread::join($waiter);
```

**Thread functions:** `thread::create`, `thread::join`, `thread::detach`, `thread::self`

**Mutex functions:** `thread::mutex_new`, `thread::mutex_lock`, `thread::mutex_trylock`, `thread::mutex_unlock`, `thread::mutex_destroy`

**Condition vars:** `thread::cond_new`, `thread::cond_wait`, `thread::cond_signal`, `thread::cond_broadcast`, `thread::cond_destroy`

## Async/Await

```strada
# Define async function
async func compute(int $n) int {
    core::usleep(50000);
    return $n * 2;
}

# Call returns a Future
my scalar $future = compute(21);

# Await blocks until complete
my int $result = await $future;  # 42

# Parallel execution
my scalar $a = compute(10);
my scalar $b = compute(20);
my int $r1 = await $a;  # 20
my int $r2 = await $b;  # 40

# Wait for all
my array @futures = (compute(1), compute(2), compute(3));
my array @results = async::all(\@futures);  # [2, 4, 6]

# Race - first to complete
my str $winner = async::race(\@futures);

# Timeout
try {
    my str $r = async::timeout($future, 100);  # 100ms
} catch ($e) {
    say("Timed out");
}

# Cancellation
async::cancel($future);
if (async::is_cancelled($future)) { ... }
```

**Futures:** `async::all`, `async::race`, `async::timeout`, `async::cancel`, `async::is_done`, `async::is_cancelled`, `async::pool_init`, `async::pool_shutdown`

## Channels

```strada
my scalar $ch = async::channel();      # Unbounded
my scalar $ch = async::channel(10);    # Bounded (capacity 10)

async::send($ch, $value);              # Send (blocks if full)
my scalar $v = async::recv($ch);       # Receive (blocks if empty)

# Non-blocking variants
if (async::try_send($ch, $value)) { }  # Returns 0/1
my scalar $v = async::try_recv($ch);   # Returns undef if empty

async::close($ch);                     # Close channel
async::is_closed($ch);                 # Check if closed
async::len($ch);                       # Items in queue
```

## Mutexes

```strada
my scalar $m = async::mutex();
async::lock($m);                       # Acquire (blocking)
async::unlock($m);                     # Release
async::try_lock($m);                   # Non-blocking (0=success)
async::mutex_destroy($m);              # Clean up
```

## Atomics

```strada
my scalar $a = async::atomic(0);       # Create with initial value
async::atomic_load($a);                # Read
async::atomic_store($a, 100);          # Write
async::atomic_add($a, 10);             # Add, returns OLD value
async::atomic_sub($a, 5);              # Subtract, returns OLD value
async::atomic_inc($a);                 # Increment, returns NEW value
async::atomic_dec($a);                 # Decrement, returns NEW value
async::atomic_cas($a, $exp, $new);     # Compare-and-swap (returns 1 if swapped)
```

## Arrays

```strada
my array @arr = ();
my array @big[1000];          # Pre-allocate for 1000 elements
push(@arr, "item");           # Add to end
my scalar $last = pop(@arr);  # Remove from end
my int $len = size(@arr);     # Length
my scalar $el = $arr[0];      # Access element
reverse(@arr);                # Reverse in place
splice(@arr, 2, 1);          # Remove 1 element at index 2, returns removed
splice(@arr, 1, 2, @repl);   # Replace 2 elements at index 1 with @repl

# Anonymous array
my scalar $nums = [1, 2, 3];
say($nums->[0]);              # Access via reference

# Array capacity (performance optimization)
my int $cap = core::array_capacity(@arr);
core::array_reserve(@arr, 1000);  # Pre-allocate
core::array_shrink(@arr);         # Shrink to fit
```

## Map, Grep, Sort

```strada
# Map - transform elements using $_
my array @doubled = map { $_ * 2 } @nums;

# Map with fat arrow - create lookup hash (Perl idiom!)
my hash %lookup = map { $_ => 1 } @fruits;
if (exists($lookup{"apple"})) { say("found!"); }

# Grep - filter elements using $_
my array @evens = grep { $_ % 2 == 0 } @nums;

# Sort - custom comparison using $a, $b, and <=>
my array @asc = sort { $a <=> $b } @nums;    # ascending
my array @desc = sort { $b <=> $a } @nums;   # descending
my array @alpha = sort @names;                # default sort

# Chain operations
my array @result = sort { $a <=> $b } map { $_ * 2 } grep { $_ > 3 } @data;
```

## Hashes

```strada
my hash %h = ();
$h{"key"} = "value";          # Set (use $ for element access)
my scalar $v = $h{"key"};     # Get
delete($h{"key"});            # Delete
my array @k = keys(%h);       # All keys (use % for whole hash)
my array @v = values(%h);     # All values

# Iterate key-value pairs
my array @pair = each(%h);    # Returns [key, value], empty array when done

# Anonymous hash
my scalar $person = {
    name => "Alice",
    age => 30
};
say($person->{"name"});

# Tied hashes - bind a hash to a class with TIEHASH methods
tie(%h, "MyTiedHash");        # Bind %h to class (calls TIEHASH)
$h{"key"} = "val";           # Calls STORE on the class
my scalar $v = $h{"key"};    # Calls FETCH on the class
untie(%h);                    # Unbind (calls UNTIE if defined)
my scalar $obj = tied(%h);   # Get underlying tied object (or undef)
```

## References

```strada
# Create
my scalar $ref = \$var;       # Reference to scalar
my scalar $aref = \@arr;      # Reference to array
my scalar $href = \%hash;     # Reference to hash

# Dereference (read)
my scalar $val = $$ref;       # Scalar
my scalar $el = $aref->[0];   # Array element
my scalar $v = $href->{"k"};  # Hash element

# Modify through reference
$$ref = "new value";          # Modify original variable
$aref->[0] = 99;              # Modify array element
$href->{"k"} = "v";           # Modify hash element
deref_set($ref, "value");     # Alternative to $$ref = ...

# Anonymous
my scalar $h = { a => 1 };    # Hash ref
my scalar $a = [1, 2, 3];     # Array ref
```

## Weak References

```strada
core::weaken($ref);              # Make $ref weak (doesn't hold target alive)
core::isweak($ref);              # Returns 1 if weak, 0 otherwise
core::weaken($hash->{"key"});    # Weaken a hash entry value
```

Break circular references:
```strada
$child->{"parent"} = $parent;
core::weaken($child->{"parent"});   # Parent can be freed normally
```

## Strings

```strada
length($s)              # Length
substr($s, 0, 5)        # Substring
index($s, "sub")        # Find
uc($s)  lc($s)          # Case
trim($s)                # Whitespace
join(",", @arr)         # Join array
chr(65)  ord("A")       # Char conversion
```

## Binary Data

```strada
# Byte-level operations (binary-safe, not UTF-8)
my int $byte = core::ord_byte($str);           # First byte (0-255)
my int $b = core::get_byte($str, 5);           # Byte at position
my str $new = core::set_byte($str, 0, 0xFF);   # Set byte, returns new string
my int $len = core::byte_length($str);         # Byte count (not chars)
my str $sub = core::byte_substr($str, 0, 4);   # Substring by bytes

# Pack - create binary data from values
my str $bin = core::pack("NnC", 0x12345678, 80, 255);
# N = 32-bit big-endian, n = 16-bit big-endian, C = unsigned byte

# Unpack - parse binary data to array
my array @vals = core::unpack("NnC", $bin);
my int $magic = $vals[0];   # 0x12345678
my int $port = $vals[1];    # 80
my int $flags = $vals[2];   # 255

# Pack format characters:
# c/C = signed/unsigned char (1 byte)
# s/S = signed/unsigned short (2 bytes, native endian)
# l/L = signed/unsigned long (4 bytes, native endian)
# q/Q = signed/unsigned quad (8 bytes, native endian)
# n   = unsigned short, big-endian (network order)
# v   = unsigned short, little-endian
# N   = unsigned long, big-endian (network order)
# V   = unsigned long, little-endian
# a   = ASCII string (null-padded)
# A   = ASCII string (space-padded)
# H   = hex string (high nybble first)
# x   = null byte (pack only)
# X   = backup one byte (pack only)
```

## I/O

```strada
say("with newline");
print("no newline");
printf("%s: %d", $name, $val);

my str $line = core::readline();               # Read from stdin
my str $content = core::slurp("file.txt");     # Read entire file
core::spew("file.txt", $data);                 # Write entire file

# File handle operations
my scalar $fh = core::open("file.txt", "r");
my str $line = core::readline($fh);
core::close($fh);

# Diamond operator <$fh> - read line from filehandle
my scalar $fh = core::open("file.txt", "r");
my str $line = <$fh>;                         # Read one line
while (defined($line)) {
    say($line);
    $line = <$fh>;
}
core::close($fh);

# Print/say to filehandle
my scalar $out = core::open("output.txt", "w");
say($out, "Line with newline");               # say($fh, $text)
print($out, "No newline");                    # print($fh, $text)
core::close($out);

# In-memory I/O
my scalar $mfh = core::open_str("data\n", "r");  # Read from string
my scalar $mwfh = core::open_str("", "w");        # Write to string
say($mwfh, "out");
my str $res = core::str_from_fh($mwfh);           # Extract output

my str $buf = "";
my scalar $rfh = core::open(\$buf, "w");           # Ref-style (writeback on close)
say($rfh, "text");
core::close($rfh);  # $buf = "text\n"

# Select default filehandle for print/say
my scalar $prev = select($out);  # Set default, returns previous
say("goes to $out");             # No filehandle arg needed
select($prev);                   # Restore previous default

# Works with sockets too!
my scalar $sock = core::socket_client("localhost", 80);
say($sock, "GET / HTTP/1.0");
my str $resp = <$sock>;                       # Read response
core::socket_close($sock);
```

## Regex

```strada
# Inline operators (Perl-style)
if ($str =~ /pattern/) { ... }       # Match
if ($str !~ /pattern/) { ... }       # Negated match
$str =~ s/old/new/;                  # Substitute first
$str =~ s/old/new/g;                 # Substitute all

# With anchors and special patterns
if ($str =~ /^start/) { ... }        # Start anchor
if ($str =~ /end$/) { ... }          # End anchor
if ($str =~ /\d+/) { ... }           # Digits

# Capture groups: $1-$9 or captures()
if ($str =~ /(\w+)\s+(\w+)/) {
    say($1);                         # First capture group
    say($2);                         # Second capture group
    my array @all = captures();      # Full match at [0], groups at [1]+
}

# Evaluate replacement as expression (/e modifier)
$str =~ s/(\d+)/$1 * 2/e;       # Double each number in-place
$str =~ s/(\d+)/$1 * 2/eg;      # With /g: all occurrences
# $1-$9 and captures() available inside /e replacement expression

# Character transliteration (tr/// or y///)
my int $count = ($str =~ tr/a-z/A-Z/);  # Uppercase, returns count changed
$str =~ tr/0-9//d;              # Delete digits (d flag)
$str =~ tr/ / /s;               # Squeeze repeated spaces (s flag)
$str =~ tr/a-z/A-Z/r;          # Return copy, don't modify original (r flag)
$str =~ y/abc/xyz/;             # y/// is alias for tr///

# Function-based alternatives
if (match($str, "pattern")) { ... }
my str $new = replace($str, "old", "new");
my str $new = replace_all($str, "old", "new");
my array @parts = split(",", $csv);
```

## Modules

```strada
# Using modules (compile-time source inclusion)
use Math::Utils;
use Math::Utils qw(add multiply);

# In lib/Math/Utils.strada
package Math::Utils;
version "1.0.0";              # Optional version declaration

func add(int $a, int $b) int {
    return $a + $b;
}

# Library Import Options
use lib "lib";

# Shared library (runtime loading via dlopen)
import_lib "JSON.so";              # Loads lib/JSON.so at runtime
my str $json = JSON::encode(\%data);

# Object file (static linking)
import_object "MathLib.o";         # Links lib/MathLib.o into executable

# Archive file (static linking, includes runtime)
import_archive "FullLib.a";        # Links lib/FullLib.a into executable
```

**Library Import Comparison:**

| Statement | File | Linking | Use Case |
|-----------|------|---------|----------|
| `import_lib "X.so"` | .so | Runtime (dlopen) | Plugins, optional features |
| `import_object "X.o"` | .o | Compile time | Single-file static linking |
| `import_archive "X.a"` | .a | Compile time | Multi-file archives with bundled runtime |

## Object-Oriented Programming

```strada
# Package declaration - functions are auto-prefixed with PackageName_
package Dog;
inherit Animal;

# This becomes Dog_new automatically
func new(str $name) scalar {
    my hash %self = ();
    $self{"name"} = $name;
    return bless(\%self, "Dog");
}

# This becomes Dog_speak automatically - also auto-registered for OOP dispatch
func speak(scalar $self) void {
    say($self->{"name"} . " says: Woof!");
}

# Usage
my scalar $dog = Dog::new("Rex");  # Calls Dog_new
$dog->speak();                      # Calls Dog_speak via method dispatch

# Multiple inheritance
package Duck;
inherit Animal, Flyable, Swimmable;

# Manual naming also works
func Dog_bark(scalar $self) scalar {
    say("Bark!");
}

# Method (takes $self as first param)
func Dog_speak(scalar $self) void {
    say($self->{"name"} . " says: Woof!");
}

# SUPER:: call to parent method
func Dog_greet(scalar $self) str {
    my str $base = SUPER::greet($self);  # Call Animal's greet
    return $base . " - Woof!";
}

# DESTROY destructor (called when object freed)
func Dog_DESTROY(scalar $self) void {
    say("Dog destroyed");
    SUPER::DESTROY($self);  # Chain to parent destructor
}

# Usage
my scalar $dog = Dog_new("Rex");
Dog_speak($dog);

# Check type (function style)
if (isa($dog, "Dog")) { ... }
if (isa($dog, "Animal")) { ... }  # Follows inheritance

# UNIVERSAL methods (method style)
if ($dog->isa("Animal")) { ... }  # Same as isa($dog, "Animal")
if ($dog->can("speak")) { ... }   # Check if method exists

# Get blessed package
my str $pkg = blessed($dog);      # "Dog"

# Current package
say(__PACKAGE__);                 # Returns current package name at runtime

# Call function in current package (compile-time resolution)
::helper("arg");                  # Preferred shorthand
.::helper("arg");                 # Alternate shorthand
__PACKAGE__::helper("arg");       # Explicit form
# All three resolve to PackageName_helper("arg") at compile time

# Runtime package control (for multi-package files)
inherit("Dog", "Animal");         # Explicit child and parent
inherit("Duck", "Flyable");       # Multiple inheritance

# Operator overloading
package Vector;
use overload
    "+" => "add",           # $a + $b calls Vector::add($self, $other, $reversed)
    "-" => "sub",
    '""' => "to_str",       # String interpolation calls Vector::to_str($self)
    "==" => "num_eq",
    "<=>" => "compare",
    "neg" => "negate";      # Unary minus: -$v calls Vector::negate($self)

# Binary handler: func add(scalar $self, scalar $other, int $reversed) scalar
# Unary handler:  func negate(scalar $self) scalar
# Stringify:      func to_str(scalar $self) str
```

**OOP Functions:**

| Function | Description |
|----------|-------------|
| `bless(\%h, "Pkg")` | Associate hash ref with package |
| `blessed($obj)` | Get package name (or empty) |
| `isa($obj, "Pkg")` | Check type (follows inheritance) |
| `$obj->isa("Pkg")` | Method-style type check (UNIVERSAL) |
| `can($obj, "method")` | Check if method exists |
| `$obj->can("method")` | Method-style capability check (UNIVERSAL) |
| `inherit Parent;` | Single inheritance (statement) |
| `inherit A, B, C;` | Multiple inheritance (statement) |
| `inherit("Child", "Parent")` | Explicit inheritance (function) |
| `SUPER::method($self, ...)` | Call parent method |
| `Pkg_DESTROY($self)` | Destructor (auto-called on free) |
| `Pkg_AUTOLOAD($self, $method, ...@args)` | Fallback for undefined method calls |
| `$obj->$method()` | Dynamic method dispatch (name from variable) |
| `use overload "+" => "add", ...;` | Operator overloading for a package |
| `set_package("Pkg")` | Set current package |
| `__PACKAGE__` | Get current package name (runtime) |
| `::func()` | Call func in current package (compile-time) |
| `.::func()` | Alternate syntax for above |
| `__PACKAGE__::func()` | Explicit form of above |

## Moose-Style Declarative OOP

Strada supports Moose-inspired declarative OOP with `has`, `extends`, `with`, and method modifiers.

```strada
# Attribute declarations
package Person;
has ro str $name (required);          # Read-only, required in constructor
has rw int $age = 0;                  # Read/write, default value
has rw str $nickname;                 # Read/write, no default (undef)
has ro str $id (lazy, builder => "_build_id");  # Lazy, built on first access

# Inheritance
package Employee;
extends Person;                       # Inherits all Person attrs + methods
has ro str $company (required);

# Role composition (same as extends)
package Auditable;
has rw str $audit_log;

package Account;
extends Person;
with Auditable;                       # Compose role (alias for inherit)

# Method modifiers
before "save" func(scalar $self) void {
    say("about to save");             # Runs before save()
}

after "save" func(scalar $self) void {
    say("saved successfully");        # Runs after save()
}

around "validate" func(scalar $self, scalar $orig, scalar ...@args) scalar {
    say("pre-check");
    my scalar $result = $orig->($self, ...@args);  # Call original
    say("post-check");
    return $result;
}
```

**`has` options:**

| Syntax | Description |
|--------|-------------|
| `has ro type $name;` | Read-only attribute (getter only) |
| `has rw type $name;` | Read/write attribute (getter + `set_NAME()` setter) |
| `has ro type $name = VALUE;` | Default value |
| `has ro type $name (required);` | Must be provided in constructor |
| `has ro type $name (lazy);` | Computed on first access |
| `has ro type $name (lazy, builder => "method");` | Built by named method |

**Auto-generated constructor:** `Package::new("name", "value", "age", 42)`

Named arguments: keys and values alternate in the argument list. Child constructors include parent attributes from the `extends` chain.

**Generated accessor methods:**

| Declaration | Generated |
|-------------|-----------|
| `has ro str $name;` | `$obj->name()` getter |
| `has rw int $age;` | `$obj->age()` getter, `$obj->set_age($val)` setter |

## Common Functions

| Function | Description |
|----------|-------------|
| `say($x)` | Print with newline |
| `print($x)` | Print without newline |
| `defined($x)` | Check if defined |
| `typeof($x)` | Get type name |
| `dumper($x)` | Debug dump |
| `die($msg)` | Exit with error |
| `exit($code)` | Exit program |

## Built-in Functions by Category

### Core Functions (no namespace)

**Output:** `say`, `print`, `printf`, `warn`, `die`, `select`

**Arrays:** `push`, `pop`, `shift`, `unshift`, `size`, `splice`, `map`, `grep`, `sort`

**Hashes:** `keys`, `values`, `exists`, `delete`, `each`, `tie`, `untie`, `tied`

**Strings:** `length`, `substr`, `index`, `rindex`, `uc`, `lc`, `trim`, `join`, `split`, `chr`, `ord`

**Regex:** `match`, `replace`, `replace_all`, `capture`

**Refs:** `is_ref`, `reftype`

**OOP:** `bless`, `blessed`, `isa`, `can`, `inherit`, `set_package`

**Types:** `defined`, `typeof`, `cast_int`, `cast_num`, `cast_str`

**Exceptions:** `try`/`catch`, `throw`

**Scoping:** `local`

**Control Flow:** `goto`, `last`, `next`, `redo`, `last LABEL`, `next LABEL`, `redo LABEL`, `unless`, `until`, statement modifiers (`if`/`unless`/`while`/`until`), `exit`

### core:: / core:: Namespace Functions

All `core::` functions can also be called via the `core::` prefix (preferred). For example, `core::open()` and `core::open()` are identical.

**Files:** `core::open`, `core::open_str`, `core::str_from_fh`, `core::close`, `core::readline`, `core::slurp`, `core::spew`, `core::seek`, `core::tell`

**Process:** `core::sleep`, `core::usleep`, `core::fork`, `core::wait`, `core::waitpid`, `core::getpid`, `core::getppid`, `core::system`, `core::exec`, `core::signal`, `core::exit`

**Process Name:** `core::setprocname`, `core::getprocname`, `core::setproctitle`, `core::getproctitle`

**IPC:** `core::pipe`, `core::dup2`, `core::close_fd`, `core::read_fd`, `core::write_fd`, `core::read_all_fd`

**Time:** `core::time`, `core::localtime`, `core::strftime`, `core::gettimeofday`, `core::hires_time`

**Environment:** `core::getenv`, `core::setenv`, `core::unsetenv`

**Sockets:** `core::socket_server`, `core::socket_client`, `core::socket_accept`, `core::socket_send`, `core::socket_recv`, `core::socket_close`

**FFI:** `core::dl_open`, `core::dl_sym`, `core::dl_close`, `core::dl_call_int`, `core::dl_call_num`, `core::dl_call_str`, `core::dl_call_void`, `core::dl_call_int_sv`, `core::dl_call_str_sv`, `core::dl_call_void_sv`, `core::dl_error`

**Binary/Bytes:** `core::ord_byte`, `core::get_byte`, `core::set_byte`, `core::byte_length`, `core::byte_substr`, `core::pack`, `core::unpack`

### math:: Namespace Functions

**Math:** `math::sin`, `math::cos`, `math::tan`, `math::sqrt`, `math::pow`, `math::abs`, `math::floor`, `math::ceil`, `math::log`, `math::exp`, `math::rand`

## Process Control

```strada
# Sleep
core::sleep(2);                    # 2 seconds
core::usleep(500000);              # 500ms

# Fork
my int $pid = core::fork();
if ($pid == 0) {
    # child
    core::exit(0);
} else {
    core::wait();
}

# Process info
my int $pid = core::getpid();
my int $ppid = core::getppid();

# Run commands
my int $status = core::system("ls -la");
core::exec("new_program");         # Replace process

# Process name/title
core::setprocname("myworker");
core::setproctitle("myapp: working");
my str $name = core::getprocname();
my str $title = core::getproctitle();
```

## Signal Handling

```strada
# Set signal handler
func my_handler(int $sig) void {
    say("Caught signal: " . $sig);
}
core::signal("INT", \&my_handler);    # Ctrl+C
core::signal("USR1", \&my_handler);   # User signal 1

# Special actions
core::signal("PIPE", "IGNORE");       # Ignore signal
core::signal("INT", "DEFAULT");       # Restore default behavior

# Supported signals: INT, TERM, HUP, QUIT, USR1, USR2,
#                    ALRM, PIPE, CHLD, CONT, STOP, TSTP
```

## Inter-Process Communication

```strada
# Create pipe
my scalar $pipe = core::pipe();
my int $read_fd = $pipe->[0];
my int $write_fd = $pipe->[1];

# Read/write
core::write_fd($write_fd, "data\n");
my str $data = core::read_fd($read_fd, 1024);
my str $all = core::read_all_fd($read_fd);

# Close and redirect
core::close_fd($fd);
core::dup2($old_fd, $new_fd);

# Fork + pipe pattern
my scalar $pipe = core::pipe();
my int $pid = core::fork();
if ($pid == 0) {
    core::close_fd($pipe->[0]);
    core::write_fd($pipe->[1], "Hello");
    core::exit(0);
} else {
    core::close_fd($pipe->[1]);
    my str $msg = core::read_all_fd($pipe->[0]);
    core::wait();
}
```

## Time Functions

```strada
# Basic time
my int $now = core::time();
core::sleep(2);                    # seconds
core::usleep(500000);              # microseconds

# Convert to local time hash
my scalar $t = core::localtime($now);
say($t->{"hour"} . ":" . $t->{"min"});

# Format time
my str $fmt = core::strftime("%Y-%m-%d %H:%M:%S", $t);
say(core::ctime($now));            # Human readable

# High-resolution time
my num $hires = core::hires_time();
my scalar $tv = core::gettimeofday();  # {sec, usec}
core::nanosleep(1000000);              # nanoseconds

# Measure elapsed time
my scalar $start = core::gettimeofday();
# ... work ...
my scalar $end = core::gettimeofday();
my num $elapsed = core::tv_interval($start, $end);
```

## Foreign Function Interface (FFI)

```strada
# Load shared library
my int $lib = core::dl_open("libm.so.6");
my int $fn = core::dl_sym($lib, "sqrt");

# Basic FFI - args converted to C types (int64_t, double)
my num $result = core::dl_call_num($fn, [16.0]);  # sqrt(16) = 4
my int $ival = core::dl_call_int($fn, [a, b]);    # up to 5 args
core::dl_call_void($fn, [args]);                   # no return

# Enhanced FFI (_sv) - args passed as StradaValue*
# Use for C libs that accept StradaValue* and extract values
my int $result = core::dl_call_int_sv($fn, [$str, $int]);
my str $sval = core::dl_call_str_sv($fn, [args]);
core::dl_call_void_sv($fn, [args]);

# Pass pointers (for output params)
my int $x = 10;
my int $ptr = core::int_ptr(\$x);
core::dl_call_void($inc_fn, [$ptr]);  # modifies $x

# Pointer functions
my int $ptr = core::int_ptr(\$x);     # pointer to int
my int $ptr = core::num_ptr(\$n);     # pointer to num
my int $val = core::ptr_deref_int($ptr);
core::ptr_set_int($ptr, 42);

# Error handling
my str $err = core::dl_error();

# Close library
core::dl_close($lib);
```

**Writing C libs for _sv FFI:**
```c
#include "strada_runtime.h"
int64_t my_func(StradaValue *str_sv, StradaValue *int_sv) {
    const char *s = strada_to_str(str_sv);
    int64_t n = strada_to_int(int_sv);
    // ...
}
```
Build with: `gcc -shared -fPIC -o lib.so mylib.c -Iruntime`

## Raw C Code Blocks

```strada
# Embed raw C code directly
__C__ {
    printf("Hello from C!\n");
    int x = 42;
    printf("x = %d\n", x);
}

# Access Strada variables (they're StradaValue*)
my int $val = 10;
__C__ {
    long long v = strada_to_int(val);
    printf("val = %lld\n", v);
    strada_decref(val);
    val = strada_new_int(v * 2);
}
say($val);  # 20
```

Key functions: `strada_to_int()`, `strada_to_str()`, `strada_new_int()`, `strada_new_str()`, `strada_decref()`, `strada_incref()`

## Calling Strada from C

```bash
# 1. Compile Strada to C (no main function)
./stradac mylib.strada mylib.c

# 2. Link with C program
gcc -o prog main.c mylib.c runtime/strada_runtime.c \
    -Iruntime -ldl -lm
```

```c
// In C: call Strada functions
#include "strada_runtime.h"

StradaValue *ARGV = NULL, *ARGC = NULL;
StradaValue* add(StradaValue*, StradaValue*);

int main((int $argc, array @argv) {
    StradaValue *r = add(strada_new_int(5), strada_new_int(3));
    printf("%ld\n", strada_to_int(r));
}
```

## Perl Integration

### Calling Perl from Strada

```strada
use lib "lib";
use perl5;

perl5::init();                                  # Initialize Perl
my str $r = perl5::eval("2 + 2");               # Evaluate expression
perl5::run("sub greet { return 'Hi ' . shift }"); # Define sub
my str $g = perl5::call("greet", \@args);       # Call sub
perl5::use_module("JSON");                      # Load CPAN module
perl5::set_var("$x", "value");                  # Set variable
my str $v = perl5::get_var("$x");               # Get variable
my str $e = perl5::get_error();                 # Get $@ error
perl5::shutdown();                              # Cleanup
```

Build first: `cd lib/perl5 && make`

### Calling Strada from Perl

```perl
use Strada;

# Load compiled Strada shared library
my $lib = Strada::Library->new('./libmylib.so');

# Call functions (package__function naming)
my $sum = $lib->call('mylib__add', 10, 20);
my $arr = $lib->call('mylib__get_data');  # Returns arrayref
my $hash = $lib->call('mylib__get_info'); # Returns hashref

$lib->unload();
```

Create shared library:
```bash
./stradac mylib.strada mylib.c
gcc -shared -fPIC -rdynamic -o libmylib.so mylib.c \
    runtime/strada_runtime.c -Iruntime -ldl -lm
```

Build Perl module: `cd perl/Strada && perl Makefile.PL && make`

See `docs/PERL_INTEGRATION.md` for full documentation.

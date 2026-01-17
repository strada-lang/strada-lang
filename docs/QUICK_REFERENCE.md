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
| `math::` | Math functions | `math::sin()`, `math::sqrt()` |
| `sys::` | System/libc | `sys::open()`, `sys::fork()` |
| `thread::` | Multithreading | `thread::create()`, `thread::mutex_new()` |
| *(none)* | Core language | `say()`, `push()`, `keys()` |

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
# If/elsif/else
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

# Loop control
last;   # break
next;   # continue

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
```

## Functions

```strada
func add(int $a, int $b) int {
    return $a + $b;
}

func greet(str $name, str $greeting = "Hello") void {
    say($greeting . ", " . $name);
}
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

## Arrays

```strada
my array @arr = ();
my array @big[1000];          # Pre-allocate for 1000 elements
push(@arr, "item");           # Add to end
my scalar $last = pop(@arr);  # Remove from end
my int $len = size(@arr);     # Length
my scalar $el = @arr[0];      # Access element
reverse(@arr);                # Reverse in place

# Anonymous array
my scalar $nums = [1, 2, 3];
say($nums->[0]);              # Access via reference

# Array capacity (performance optimization)
my int $cap = sys::array_capacity(@arr);
sys::array_reserve(@arr, 1000);  # Pre-allocate
sys::array_shrink(@arr);         # Shrink to fit
```

## Map, Grep, Sort

```strada
# Map - transform elements using $_
my scalar $doubled = map { $_ * 2; } @nums;

# Grep - filter elements using $_
my scalar $evens = grep { $_ % 2 == 0; } @nums;

# Sort - custom comparison using $a, $b, and <=>
my scalar $asc = sort { $a <=> $b; } @nums;   # ascending
my scalar $desc = sort { $b <=> $a; } @nums;  # descending
my scalar $alpha = sort @names;                # default sort
```

## Hashes

```strada
my hash %h = ();
$h{"key"} = "value";          # Set (use $ for element access)
my scalar $v = $h{"key"};     # Get
delete($h{"key"});            # Delete
my array @k = keys(%h);       # All keys (use % for whole hash)
my array @v = values(%h);     # All values

# Anonymous hash
my scalar $person = {
    name => "Alice",
    age => 30
};
say($person->{"name"});
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
my int $byte = sys::ord_byte($str);           # First byte (0-255)
my int $b = sys::get_byte($str, 5);           # Byte at position
my str $new = sys::set_byte($str, 0, 0xFF);   # Set byte, returns new string
my int $len = sys::byte_length($str);         # Byte count (not chars)
my str $sub = sys::byte_substr($str, 0, 4);   # Substring by bytes

# Pack - create binary data from values
my str $bin = sys::pack("NnC", 0x12345678, 80, 255);
# N = 32-bit big-endian, n = 16-bit big-endian, C = unsigned byte

# Unpack - parse binary data to array
my array @vals = sys::unpack("NnC", $bin);
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

my str $line = sys::readline();               # Read from stdin
my str $content = sys::slurp("file.txt");     # Read entire file
sys::spew("file.txt", $data);                 # Write entire file

# File handle operations
my scalar $fh = sys::open("file.txt", "r");
my str $line = sys::readline($fh);
sys::close($fh);
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

# Runtime shared library loading
use lib "lib";
import_lib "JSON";            # Loads lib/JSON.so (signatures embedded via __strada_export_info)
my str $json = JSON::encode(\%data);  # Namespace syntax works after import_lib
```

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
say(__PACKAGE__);                 # Returns current package name

# Runtime package control (for multi-package files)
inherit("Dog", "Animal");         # Explicit child and parent
inherit("Duck", "Flyable");       # Multiple inheritance
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
| `set_package("Pkg")` | Set current package |
| `__PACKAGE__` | Get current package name |

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

**Output:** `say`, `print`, `printf`, `warn`, `die`

**Arrays:** `push`, `pop`, `shift`, `unshift`, `size`, `map`, `grep`, `sort`

**Hashes:** `keys`, `values`, `exists`, `delete`

**Strings:** `length`, `substr`, `index`, `rindex`, `uc`, `lc`, `trim`, `join`, `split`, `chr`, `ord`

**Regex:** `match`, `replace`, `replace_all`, `capture`

**Refs:** `is_ref`, `reftype`

**OOP:** `bless`, `blessed`, `isa`, `can`, `inherit`, `set_package`

**Types:** `defined`, `typeof`, `cast_int`, `cast_num`, `cast_str`

**Exceptions:** `try`/`catch`, `throw`

**Control Flow:** `goto`, `last`, `next`, `last LABEL`, `next LABEL`, `exit`

### sys:: Namespace Functions

**Files:** `sys::open`, `sys::close`, `sys::readline`, `sys::slurp`, `sys::spew`, `sys::seek`, `sys::tell`

**Process:** `sys::sleep`, `sys::usleep`, `sys::fork`, `sys::wait`, `sys::waitpid`, `sys::getpid`, `sys::getppid`, `sys::system`, `sys::exec`, `sys::signal`, `sys::exit`

**Process Name:** `sys::setprocname`, `sys::getprocname`, `sys::setproctitle`, `sys::getproctitle`

**IPC:** `sys::pipe`, `sys::dup2`, `sys::close_fd`, `sys::read_fd`, `sys::write_fd`, `sys::read_all_fd`

**Time:** `sys::time`, `sys::localtime`, `sys::strftime`, `sys::gettimeofday`, `sys::hires_time`

**Environment:** `sys::getenv`, `sys::setenv`, `sys::unsetenv`

**Sockets:** `sys::socket_server`, `sys::socket_client`, `sys::socket_accept`, `sys::socket_send`, `sys::socket_recv`, `sys::socket_close`

**FFI:** `sys::dl_open`, `sys::dl_sym`, `sys::dl_close`, `sys::dl_call_int`, `sys::dl_call_num`, `sys::dl_call_str`, `sys::dl_call_void`, `sys::dl_call_int_sv`, `sys::dl_call_str_sv`, `sys::dl_call_void_sv`, `sys::dl_error`

**Binary/Bytes:** `sys::ord_byte`, `sys::get_byte`, `sys::set_byte`, `sys::byte_length`, `sys::byte_substr`, `sys::pack`, `sys::unpack`

### math:: Namespace Functions

**Math:** `math::sin`, `math::cos`, `math::tan`, `math::sqrt`, `math::pow`, `math::abs`, `math::floor`, `math::ceil`, `math::log`, `math::exp`, `math::rand`

## Process Control

```strada
# Sleep
sys::sleep(2);                    # 2 seconds
sys::usleep(500000);              # 500ms

# Fork
my int $pid = sys::fork();
if ($pid == 0) {
    # child
    sys::exit(0);
} else {
    sys::wait();
}

# Process info
my int $pid = sys::getpid();
my int $ppid = sys::getppid();

# Run commands
my int $status = sys::system("ls -la");
sys::exec("new_program");         # Replace process

# Process name/title
sys::setprocname("myworker");
sys::setproctitle("myapp: working");
my str $name = sys::getprocname();
my str $title = sys::getproctitle();
```

## Signal Handling

```strada
# Set signal handler
func my_handler(int $sig) void {
    say("Caught signal: " . $sig);
}
sys::signal("INT", \&my_handler);    # Ctrl+C
sys::signal("USR1", \&my_handler);   # User signal 1

# Special actions
sys::signal("PIPE", "IGNORE");       # Ignore signal
sys::signal("INT", "DEFAULT");       # Restore default behavior

# Supported signals: INT, TERM, HUP, QUIT, USR1, USR2,
#                    ALRM, PIPE, CHLD, CONT, STOP, TSTP
```

## Inter-Process Communication

```strada
# Create pipe
my scalar $pipe = sys::pipe();
my int $read_fd = $pipe->[0];
my int $write_fd = $pipe->[1];

# Read/write
sys::write_fd($write_fd, "data\n");
my str $data = sys::read_fd($read_fd, 1024);
my str $all = sys::read_all_fd($read_fd);

# Close and redirect
sys::close_fd($fd);
sys::dup2($old_fd, $new_fd);

# Fork + pipe pattern
my scalar $pipe = sys::pipe();
my int $pid = sys::fork();
if ($pid == 0) {
    sys::close_fd($pipe->[0]);
    sys::write_fd($pipe->[1], "Hello");
    sys::exit(0);
} else {
    sys::close_fd($pipe->[1]);
    my str $msg = sys::read_all_fd($pipe->[0]);
    sys::wait();
}
```

## Time Functions

```strada
# Basic time
my int $now = sys::time();
sys::sleep(2);                    # seconds
sys::usleep(500000);              # microseconds

# Convert to local time hash
my scalar $t = sys::localtime($now);
say($t->{"hour"} . ":" . $t->{"min"});

# Format time
my str $fmt = sys::strftime("%Y-%m-%d %H:%M:%S", $t);
say(sys::ctime($now));            # Human readable

# High-resolution time
my num $hires = sys::hires_time();
my scalar $tv = sys::gettimeofday();  # {sec, usec}
sys::nanosleep(1000000);              # nanoseconds

# Measure elapsed time
my scalar $start = sys::gettimeofday();
# ... work ...
my scalar $end = sys::gettimeofday();
my num $elapsed = sys::tv_interval($start, $end);
```

## Foreign Function Interface (FFI)

```strada
# Load shared library
my int $lib = sys::dl_open("libm.so.6");
my int $fn = sys::dl_sym($lib, "sqrt");

# Basic FFI - args converted to C types (int64_t, double)
my num $result = sys::dl_call_num($fn, [16.0]);  # sqrt(16) = 4
my int $ival = sys::dl_call_int($fn, [a, b]);    # up to 5 args
sys::dl_call_void($fn, [args]);                   # no return

# Enhanced FFI (_sv) - args passed as StradaValue*
# Use for C libs that accept StradaValue* and extract values
my int $result = sys::dl_call_int_sv($fn, [$str, $int]);
my str $sval = sys::dl_call_str_sv($fn, [args]);
sys::dl_call_void_sv($fn, [args]);

# Pass pointers (for output params)
my int $x = 10;
my int $ptr = sys::int_ptr(\$x);
sys::dl_call_void($inc_fn, [$ptr]);  # modifies $x

# Pointer functions
my int $ptr = sys::int_ptr(\$x);     # pointer to int
my int $ptr = sys::num_ptr(\$n);     # pointer to num
my int $val = sys::ptr_deref_int($ptr);
sys::ptr_set_int($ptr, 42);

# Error handling
my str $err = sys::dl_error();

# Close library
sys::dl_close($lib);
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

int main() {
    ARGV = strada_new_array();
    ARGC = strada_new_int(0);
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

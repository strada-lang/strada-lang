# Types and Type Casting in Strada

Strada is statically typed - every variable has a declared type. However,
values can be converted between types at runtime.

## Basic Types

| Type | Description | Example |
|------|-------------|---------|
| `int` | Integer number | `42`, `-7`, `0` |
| `num` | Floating-point number | `3.14`, `-2.5` |
| `str` | String | `"hello"`, `""` |
| `bool` | Boolean (0 or 1) | `0`, `1` |
| `array` | Ordered list | `(1, 2, 3)` |
| `hash` | Key-value pairs | `{ "a" => 1 }` |
| `scalar` | Any single value | Generic type |
| `void` | No return value | For functions |
| `dynamic` | Context-sensitive return | For functions |

## Dynamic Return Type

The `dynamic` return type enables Perl-like context-sensitive functions. A `dynamic` function can inspect how its result will be used and return different values accordingly.

```strada
func flexible() dynamic {
    if (core::wantarray()) {
        my array @result = (1, 2, 3);
        return @result;
    }
    if (core::wanthash()) {
        my hash %result = ();
        $result{"key"} = "value";
        return %result;
    }
    return 42;  # scalar context (default)
}

my array @arr = flexible();   # Array context → (1, 2, 3)
my hash %h = flexible();      # Hash context → {"key" => "value"}
my int $val = flexible();     # Scalar context → 42
foreach my int $x (flexible()) { ... }  # Array context
```

### Context Detection Functions

| Function | Returns 1 when... |
|----------|-------------------|
| `core::wantarray()` | Called in array context (`my array @a = ...` or `foreach`) |
| `core::wantscalar()` | Called in scalar context (`my int $x = ...`, default) |
| `core::wanthash()` | Called in hash context (`my hash %h = ...`) |

## Checking Variable Types

### typeof() - Get Type Name

Returns a string describing the type:

```strada
my int $n = 42;
my str $s = "hello";
my array @a = (1, 2, 3);
my hash %h = { "x" => 1 };

say(typeof($n));    # "int"
say(typeof($s));    # "str"
say(typeof(\@a));   # "array_ref"
say(typeof(\%h));   # "hash_ref"
```

Type strings returned by `typeof()`:

| Value | typeof() returns |
|-------|------------------|
| Integer | `"int"` |
| Float | `"num"` |
| String | `"str"` |
| Boolean | `"int"` (bools are ints internally) |
| Array ref | `"array_ref"` |
| Hash ref | `"hash_ref"` |
| Scalar ref | `"scalar_ref"` |
| Undef | `"undef"` |
| Blessed object | `"ClassName"` (the package name) |

### defined() - Check if Not Undef

```strada
my scalar $x;
my scalar $y = 42;

say(defined($x));   # 0 (false - $x is undef)
say(defined($y));   # 1 (true)
```

### length() - Check for Empty Values

For strings, returns character count. Also useful for checking if a
value exists:

```strada
my str $s = "";
my scalar $fh = core::open("file.txt", "r");

if (length($s) == 0) {
    say("empty string");
}

if (length($fh) == 0) {
    say("file open failed");
}
```

### ref() - Check Reference Type

Returns the type of a reference, or empty string if not a reference:

```strada
my array @arr = (1, 2, 3);
my hash %hash = { "a" => 1 };

say(ref(\@arr));    # "ARRAY"
say(ref(\%hash));   # "HASH"
say(ref(42));       # "" (not a reference)
```

For blessed objects, returns the class name:

```strada
my scalar $obj = Dog::new("Rex");
say(ref($obj));     # "Dog"
```

### isa() - Check Object Type

For OOP, check if an object is a certain class:

```strada
my scalar $dog = Dog::new("Rex");

if ($dog->isa("Dog")) {
    say("It's a dog!");
}

if ($dog->isa("Animal")) {
    say("It's an animal!");  # Works with inheritance
}
```

## Type Casting

### To Integer - int()

Converts a value to an integer (truncates decimals):

```strada
my int $a = int(3.7);       # 3
my int $b = int(3.2);       # 3
my int $c = int(-3.7);      # -3
my int $d = int("42abc");   # 42 (parses leading digits)
my int $e = int("hello");   # 0
```

### To Number - num() or Arithmetic

Convert to floating-point:

```strada
my num $a = num(42);        # 42.0
my num $b = num("3.14");    # 3.14
my num $c = $x + 0.0;       # Alternative: add 0.0
```

### To String - str() or Concatenation

Convert to string:

```strada
my str $a = str(42);        # "42"
my str $b = str(3.14);      # "3.14"
my str $c = "" . $x;        # Alternative: concatenate with ""
```

### Implicit Conversions

Strada automatically converts types in many contexts:

```strada
# String concatenation converts to string
my str $msg = "Count: " . 42;       # "Count: 42"

# Arithmetic converts to number
my num $sum = "3.5" + 2;            # 5.5

# Comparison context
if ("hello") {                       # Non-empty string is true
    say("truthy");
}
```

## Type Coercion Functions

### core::atoi() - String to Integer (C-style)

```strada
my int $n = core::atoi("123");       # 123
my int $m = core::atoi("  456");     # 456 (skips whitespace)
my int $x = core::atoi("abc");       # 0
```

### core::atof() - String to Float (C-style)

```strada
my num $n = core::atof("3.14159");   # 3.14159
my num $m = core::atof("2.5e10");    # 25000000000.0
```

### ord() - Character to ASCII Code

```strada
my int $code = ord("A");            # 65
my int $newline = ord("\n");        # 10
```

### chr() - ASCII Code to Character

```strada
my str $ch = chr(65);               # "A"
my str $nl = chr(10);               # "\n"
```

### sprintf() - Formatted Conversion

```strada
my str $hex = sprintf("%x", 255);       # "ff"
my str $padded = sprintf("%05d", 42);   # "00042"
my str $float = sprintf("%.2f", 3.14159); # "3.14"
```

## Array/Hash Type Checking

### scalar() - Get Array Length

```strada
my array @arr = (1, 2, 3, 4, 5);
my int $len = scalar(@arr);         # 5
```

### keys() - Get Hash Keys

Also useful to check if hash has entries:

```strada
my hash %h = { "a" => 1, "b" => 2 };
my array @k = keys(%h);
my int $count = scalar(@k);         # 2
```

### exists() - Check Hash Key

```strada
my hash %h = { "name" => "Alice" };

if (exists(%h, "name")) {
    say("has name");
}

if (!exists(%h, "age")) {
    say("no age");
}
```

## Numeric Type Checking

### math::isnan() - Check for NaN

```strada
my num $x = math::sqrt(-1);         # NaN
if (math::isnan($x)) {
    say("not a number");
}
```

### math::isinf() - Check for Infinity

```strada
my num $x = 1.0 / 0.0;              # Infinity
if (math::isinf($x)) {
    say("infinite");
}
```

## Common Patterns

### Safe Type Conversion

```strada
func safe_int(scalar $val) int {
    if (!defined($val)) {
        return 0;
    }
    return int($val);
}
```

### Type-Based Dispatch

```strada
func process(scalar $val) void {
    my str $type = typeof($val);

    if ($type eq "int" || $type eq "num") {
        say("Number: " . $val);
    } elsif ($type eq "str") {
        say("String: " . $val);
    } elsif ($type eq "array_ref") {
        say("Array with " . scalar(@{$val}) . " elements");
    } elsif ($type eq "hash_ref") {
        say("Hash");
    } else {
        say("Unknown type: " . $type);
    }
}
```

### Validate Function Arguments

```strada
func calculate(scalar $x, scalar $y) num {
    if (!defined($x) || !defined($y)) {
        die("Arguments cannot be undef");
    }

    my str $tx = typeof($x);
    my str $ty = typeof($y);

    if ($tx ne "int" && $tx ne "num") {
        die("First argument must be numeric");
    }
    if ($ty ne "int" && $ty ne "num") {
        die("Second argument must be numeric");
    }

    return num($x) + num($y);
}
```

### Check Object Type Before Method Call

```strada
func feed(scalar $animal) void {
    if (!defined($animal)) {
        die("No animal provided");
    }

    if (!$animal->isa("Animal")) {
        die("Expected an Animal object");
    }

    $animal->eat();
}
```

## Type Conversion Summary

| From | To Int | To Num | To Str |
|------|--------|--------|--------|
| `42` | `42` | `42.0` | `"42"` |
| `3.7` | `3` | `3.7` | `"3.7"` |
| `"123"` | `123` | `123.0` | `"123"` |
| `"3.14"` | `3` | `3.14` | `"3.14"` |
| `"abc"` | `0` | `0.0` | `"abc"` |
| `""` | `0` | `0.0` | `""` |
| `undef` | `0` | `0.0` | `""` |

## Bool Type

The `bool` type is for boolean values (true/false). Internally it's an integer
(0 or 1), but using `bool` makes code intent clearer.

### Declaring Booleans

```strada
my bool $found = 0;       # false
my bool $done = 1;        # true
my bool $flag = $x > 10;  # result of comparison
```

### Bool Function Returns

Functions can return `bool` to indicate they return true/false:

```strada
func is_even(int $n) bool {
    return $n % 2 == 0;
}

func is_valid_email(str $email) bool {
    return $email =~ /@/;
}

if (is_even(42)) {
    say("even!");
}
```

### Bool in Conditions

Bool values work naturally in conditions:

```strada
my bool $ready = check_ready();

if ($ready) {
    proceed();
}

while (!$done) {
    $done = do_work();
}
```

### Bool vs Int

`bool` and `int` are interchangeable - `bool` is just semantic sugar:

```strada
my bool $flag = 1;
my int $count = $flag;    # Works: count = 1

my int $n = 42;
my bool $b = $n;          # Works: b = 42 (truthy)
```

Use `bool` when a variable represents a true/false concept.
Use `int` when it represents a count, index, or numeric value.

## Truthiness Summary

| Value | Boolean |
|-------|---------|
| `0` | false |
| `0.0` | false |
| `""` | false |
| `undef` | false |
| Any other number | true |
| Any non-empty string | true |
| Any reference | true |

## C Interop Types

Strada provides explicit sized types for interfacing with C code via `extern`
functions. These types map directly to their C equivalents.

### Fixed-Width Integer Types

| Strada Type | C Type | Size | Range |
|-------------|--------|------|-------|
| `int8` | `int8_t` | 8 bits | -128 to 127 |
| `int16` | `int16_t` | 16 bits | -32,768 to 32,767 |
| `int32` | `int32_t` | 32 bits | -2^31 to 2^31-1 |
| `int64` | `int64_t` | 64 bits | -2^63 to 2^63-1 |
| `uint8` / `byte` | `uint8_t` | 8 bits | 0 to 255 |
| `uint16` | `uint16_t` | 16 bits | 0 to 65,535 |
| `uint32` | `uint32_t` | 32 bits | 0 to 2^32-1 |
| `uint64` | `uint64_t` | 64 bits | 0 to 2^64-1 |

### Floating-Point Types

| Strada Type | C Type | Size | Description |
|-------------|--------|------|-------------|
| `float` / `float32` | `float` | 32 bits | Single precision |
| `double` / `float64` | `double` | 64 bits | Double precision |
| `long_double` | `long double` | 80-128 bits | Extended precision (platform-dependent) |

Note: `num` is Strada's standard floating-point type, equivalent to `double`.

### Other C Types

| Strada Type | C Type | Description |
|-------------|--------|-------------|
| `size_t` | `size_t` | Size type (unsigned, platform-dependent) |
| `char` | `char` | Single character (8 bits) |

### Usage in `__C__` Blocks

These types are primarily used when working with C code via `__C__` blocks
to match C function signatures:

```strada
func write_byte(int $fd, int $byte) int {
    __C__ {
        int fd = (int)strada_to_int(fd_var);
        uint8_t b = (uint8_t)strada_to_int(byte);
        return strada_new_int(write(fd, &b, 1));
    }
}

func read_int64(int $fd) int {
    __C__ {
        int fd = (int)strada_to_int(fd_var);
        int64_t val;
        read(fd, &val, sizeof(val));
        return strada_new_int(val);
    }
}
```

### Usage in Regular Strada Code

You can also use these types in regular Strada code for documentation purposes
or when you want to be explicit about value ranges:

```strada
my int8 $small = 100;
my uint32 $flags = 0xFF00FF00;
my size_t $buffer_size = 4096;
my byte $b = 255;  # byte is an alias for uint8

func process_byte(byte $b) int {
    return $b * 2;
}
```

Note: Inside Strada, all values are still wrapped in `StradaValue*` at runtime.
The explicit sized types are useful for documentation and when working with
C code via `__C__` blocks.

## Enum Types

Strada supports enumerated types (enums) for defining named integer constants.

### Basic Enum Syntax

```strada
enum Color {
    RED,        # 0 (auto-assigned)
    GREEN,      # 1
    BLUE        # 2
}
```

Values auto-increment starting from 0.

### Explicit Values

```strada
enum Status {
    PENDING = 0,
    ACTIVE = 10,
    DONE = 20,
    ERROR = -1
}
```

You can assign explicit values, including negative numbers.

### Mixed Auto and Explicit

```strada
enum HttpCode {
    OK = 200,
    CREATED,      # 201 (auto-increments from previous)
    ACCEPTED,     # 202
    NOT_FOUND = 404,
    SERVER_ERROR = 500
}
```

After an explicit value, auto-increment continues from that value.

### Using Enum Values

Access enum values with the `::` syntax:

```strada
my int $color = Color::RED;
my int $status = Status::ACTIVE;

if ($color == Color::BLUE) {
    say("It's blue!");
}

# Works in switch statements
switch ($status) {
    case Status::PENDING {
        say("Waiting...");
    }
    case Status::ACTIVE {
        say("In progress");
    }
    case Status::DONE {
        say("Complete!");
    }
}
```

### Enum Implementation

Enums are compiled to C `#define` constants:

```c
/* enum Color */
#define Color_RED 0
#define Color_GREEN 1
#define Color_BLUE 2
```

This means enum values are pure compile-time constants with zero runtime overhead.

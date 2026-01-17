# Strada Language Guide

## Table of Contents

1. [Introduction](#introduction)
2. [Getting Started](#getting-started)
3. [Basic Syntax](#basic-syntax)
4. [Data Types](#data-types)
5. [Variables](#variables)
6. [Operators](#operators)
7. [Control Flow](#control-flow)
8. [Exception Handling](#exception-handling)
9. [Functions](#functions)
    - [Anonymous Functions (Closures)](#anonymous-functions-closures)
10. [Arrays](#arrays)
11. [Hashes](#hashes)
12. [References](#references)
13. [Strings](#strings)
14. [File I/O](#file-io)
15. [Regular Expressions](#regular-expressions)
16. [Process Control](#process-control)
17. [Inter-Process Communication](#inter-process-communication)
18. [Multithreading](#multithreading)
19. [Packages and Modules](#packages-and-modules)
    - [Object-Oriented Programming](#object-oriented-programming)
20. [C Interoperability](#c-interoperability)
21. [Perl Integration](#perl-integration)
22. [Built-in Functions](#built-in-functions)
23. [Best Practices](#best-practices)

---

## Introduction

Strada is a strongly-typed, compiled programming language that combines Perl's expressive syntax with static typing and C-level performance. It compiles to C code, which is then compiled to native executables.

### Key Features

- **Perl-like syntax**: Familiar sigils ($, @, %), expressive operators
- **Strong typing**: All variables have declared types
- **Compiles to C**: Native performance, easy C interop
- **Self-hosting**: The compiler is written in Strada itself
- **Reference counting**: Automatic memory management
- **Rich standard library**: Strings, arrays, hashes, regex, I/O
- **Magic namespaces**: `sys::` for libc functions, `math::` for math functions

### Philosophy

Strada aims to be:
- **Readable**: Clear, expressive syntax
- **Safe**: Strong typing catches errors at compile time
- **Fast**: Compiles to efficient C code
- **Practical**: Rich built-in functions for real-world tasks

---

## Getting Started

### Installation

```bash
# Extract the archive
unzip strada-full.zip
cd strada

# Build the compiler
make

# Verify it works
./stradac --help
make test
```

### Hello World

Create `hello.strada`:

```strada
func main() int {
    say("Hello, World!");
    return 0;
}
```

Compile and run:

```bash
# One-step compilation (recommended)
./strada hello.strada           # Creates ./hello
./strada -r hello.strada        # Compile and run immediately

# Or use make (for files in `examples/`)
make run PROG=hello

# Two-step compilation (manual)
./stradac hello.strada hello.c
gcc -o hello hello.c runtime/strada_runtime.c -Iruntime -ldl
./hello
```

### Program Structure

Every Strada program needs a `main` function:

```strada
func main() int {
    # Your code here
    return 0;
}
```

### Compiler Options

```bash
./stradac input.strada output.c    # Compile Strada to C
./stradac --help                   # Show help (when implemented)
./stradac --version                # Show version (when implemented)
```
```

---

## Basic Syntax

### Comments

```strada
# This is a single-line comment

# Multi-line comments use multiple # symbols
# Like this
# And this
```

### Statements

Statements end with semicolons:

```strada
my int $x = 10;
say($x);
$x = $x + 1;
```

### Blocks

Blocks are enclosed in curly braces:

```strada
if ($condition) {
    # statements
}

while ($running) {
    # statements
}
```

---

## Data Types

### Primitive Types

| Type | Description | Example |
|------|-------------|---------|
| `int` | 64-bit integer | `42`, `-17`, `0` |
| `num` | 64-bit floating point | `3.14`, `-0.5`, `1e10` |
| `str` | String | `"hello"`, `'world'` |
| `void` | No value (for functions) | - |

### Compound Types

| Type | Description | Sigil |
|------|-------------|-------|
| `array` | Ordered list of values | `@` |
| `hash` | Key-value pairs | `%` |
| `scalar` | Single value or reference | `$` |

### Extended C Types (for C interop via `__C__` blocks)

| Type | C Equivalent |
|------|--------------|
| `char` | `char` |
| `short` | `short` |
| `long` | `long` |
| `float` | `float` |
| `bool` | `bool` |
| `i8`, `i16`, `i32`, `i64` | `int8_t`, etc. |
| `u8`, `u16`, `u32`, `u64` | `uint8_t`, etc. |
| `sizet` | `size_t` |

**Note:** Use `int` to store C pointers (64-bit), not a special pointer type.

---

## Variables

### Declaration

Variables are declared with `my`, a type, a sigil, and a name:

```strada
my int $count = 0;           # Scalar integer
my str $name = "Alice";      # Scalar string
my num $pi = 3.14159;        # Scalar number
my array @items = ();        # Empty array
my hash %data = ();          # Empty hash
```

### Sigils

Sigils indicate variable type and context:

| Sigil | Meaning | Example |
|-------|---------|---------|
| `$` | Scalar (single value) | `$name`, `$count` |
| `@` | Array | `@items`, `@numbers` |
| `%` | Hash | `%config`, `%user` |

### Naming Rules

- Start with a letter or underscore
- Contain letters, numbers, underscores
- Case-sensitive (`$Name` ≠ `$name`)

```strada
my int $valid_name = 1;
my int $_also_valid = 2;
my int $CamelCase = 3;
my int $with123numbers = 4;
```

### Scope

Variables are lexically scoped to their enclosing block:

```strada
func example() void {
    my int $outer = 1;
    
    if ($outer == 1) {
        my int $inner = 2;  # Only visible in this block
        say($outer);        # OK - outer is visible
    }
    
    # $inner is not visible here
}
```

---

## Operators

### Arithmetic

| Operator | Description | Example |
|----------|-------------|---------|
| `+` | Addition | `$a + $b` |
| `-` | Subtraction | `$a - $b` |
| `*` | Multiplication | `$a * $b` |
| `/` | Division | `$a / $b` |
| `%` | Modulo | `$a % $b` |

### String

| Operator | Description | Example |
|----------|-------------|---------|
| `.` | Concatenation | `$str1 . $str2` |
| `.=` | Append | `$str .= "more"` |

### Comparison (Numeric)

| Operator | Description | Example |
|----------|-------------|---------|
| `==` | Equal | `$a == $b` |
| `!=` | Not equal | `$a != $b` |
| `<` | Less than | `$a < $b` |
| `>` | Greater than | `$a > $b` |
| `<=` | Less or equal | `$a <= $b` |
| `>=` | Greater or equal | `$a >= $b` |
| `<=>` | Spaceship (returns -1, 0, 1) | `$a <=> $b` |

### Comparison (String)

| Operator | Description | Example |
|----------|-------------|---------|
| `eq` | Equal | `$s1 eq $s2` |
| `ne` | Not equal | `$s1 ne $s2` |
| `lt` | Less than | `$s1 lt $s2` |
| `gt` | Greater than | `$s1 gt $s2` |
| `le` | Less or equal | `$s1 le $s2` |
| `ge` | Greater or equal | `$s1 ge $s2` |

### Logical

| Operator | Description | Example |
|----------|-------------|---------|
| `&&` | Logical AND | `$a && $b` |
| `\|\|` | Logical OR | `$a \|\| $b` |
| `!` | Logical NOT | `!$a` |

### Assignment

| Operator | Description | Example |
|----------|-------------|---------|
| `=` | Assign | `$x = 10` |
| `+=` | Add and assign | `$x += 5` |
| `-=` | Subtract and assign | `$x -= 3` |
| `.=` | Concatenate and assign | `$s .= "!"` |

---

## Control Flow

### If/Elsif/Else

```strada
if ($score >= 90) {
    say("A");
} elsif ($score >= 80) {
    say("B");
} elsif ($score >= 70) {
    say("C");
} else {
    say("F");
}
```

### While Loop

```strada
my int $i = 0;
while ($i < 10) {
    say($i);
    $i = $i + 1;
}
```

### For Loop

C-style for loop:

```strada
for (my int $i = 0; $i < 10; $i = $i + 1) {
    say($i);
}
```

### Foreach Loop

Iterate over array elements directly:

```strada
# With inline variable declaration
my array @colors = ();
push(@colors, "red");
push(@colors, "green");
push(@colors, "blue");

foreach my str $color (@colors) {
    say($color);
}

# With existing variable
my scalar $item;
foreach $item (@colors) {
    say("Color: " . $item);
}

# With anonymous array
foreach my str $fruit (["apple", "banana", "cherry"]) {
    say($fruit);
}
```

### Loop Control

```strada
while (1) {
    my str $input = readline();

    if ($input eq "quit") {
        last;   # Break out of loop
    }

    if ($input eq "") {
        next;   # Skip to next iteration
    }

    say("You entered: " . $input);
}
```

### Labeled Loops

Labels allow you to break out of or continue outer loops from within nested loops:

```strada
OUTER: for (my int $i = 0; $i < 10; $i = $i + 1) {
    INNER: for (my int $j = 0; $j < 10; $j = $j + 1) {
        if ($i * $j > 50) {
            last OUTER;   # Break out of outer loop
        }
        if ($j == 5) {
            next OUTER;   # Skip to next iteration of outer loop
        }
        say($i . " x " . $j . " = " . ($i * $j));
    }
}
```

Label names are typically uppercase by convention. You can use labels with `for`, `foreach`, and `while` loops:

```strada
SEARCH: while (1) {
    my str $line = readline($fh);
    if (!defined($line)) {
        last SEARCH;
    }

    for (my int $i = 0; $i < size(@patterns); $i = $i + 1) {
        if ($line =~ /@patterns[$i]/) {
            say("Found match!");
            last SEARCH;   # Exit the outer while loop
        }
    }
}
```

### Goto and Labels

For advanced control flow, Strada supports `goto` with standalone labels:

```strada
func process() void {
    my int $retries = 0;

    RETRY:
    $retries = $retries + 1;

    my int $success = try_operation();
    if (!$success && $retries < 3) {
        say("Retrying...");
        goto RETRY;
    }

    if ($success) {
        say("Success!");
    } else {
        say("Failed after " . $retries . " attempts");
    }
}
```

**Note**: Use `goto` sparingly. In most cases, loops and functions provide cleaner control flow.

---

## Exception Handling

Strada provides try/catch/throw for structured exception handling.

### Basic Try/Catch

```strada
try {
    my int $result = risky_operation();
    say("Result: " . $result);
} catch ($error) {
    say("Error occurred: " . $error);
}
```

### Throwing Exceptions

Use `throw` to raise an exception:

```strada
func divide(num $a, num $b) num {
    if ($b == 0) {
        throw "Division by zero";
    }
    return $a / $b;
}

func main() int {
    try {
        my num $result = divide(10, 0);
        say("Result: " . $result);
    } catch ($e) {
        say("Caught: " . $e);  # "Caught: Division by zero"
    }
    return 0;
}
```

### Nested Exception Handling

Exceptions propagate through the call stack until caught:

```strada
func level3() void {
    throw "Error from level 3";
}

func level2() void {
    level3();
}

func level1() void {
    level2();
}

func main() int {
    try {
        level1();  # Exception propagates up
    } catch ($e) {
        say("Caught at main: " . $e);
    }
    return 0;
}
```

### Nested Try/Catch Blocks

```strada
try {
    try {
        throw "Inner error";
    } catch ($inner) {
        say("Inner catch: " . $inner);
        throw "Rethrown error";
    }
} catch ($outer) {
    say("Outer catch: " . $outer);
}
```

### Exception Handling Best Practices

1. **Use specific error messages**: Include context in your throw statements
2. **Catch at appropriate levels**: Don't catch exceptions too early
3. **Clean up resources**: Ensure files and handles are closed even on errors
4. **Don't use for control flow**: Exceptions are for exceptional conditions

```strada
# Good: Specific error message
if (!defined($file)) {
    throw "Cannot open config file: " . $path;
}

# Good: Handle at appropriate level
func process_file(str $path) void {
    my scalar $fh = open($path, "r");
    if (!defined($fh)) {
        throw "File not found: " . $path;
    }

    # Process file...
    close($fh);
}

func main() int {
    try {
        process_file("config.txt");
    } catch ($e) {
        say("Error: " . $e);
        return 1;
    }
    return 0;
}
```

---

## Functions

### Declaration

```strada
func function_name(type $param1, type $param2) return_type {
    # function body
    return value;
}
```

### Examples

```strada
# Simple function
func greet(str $name) void {
    say("Hello, " . $name . "!");
}

# Function with return value
func add(int $a, int $b) int {
    return $a + $b;
}

# Function with multiple parameters
func format_name(str $first, str $last, str $title) str {
    return $title . " " . $first . " " . $last;
}
```

### Calling Functions

```strada
greet("World");
my int $sum = add(10, 20);
my str $name = format_name("John", "Doe", "Dr.");
```

### Optional Parameters

```strada
func greet(str $name, str $greeting = "Hello") void {
    say($greeting . ", " . $name . "!");
}

greet("Alice");           # "Hello, Alice!"
greet("Bob", "Hi");       # "Hi, Bob!"
```

### Recursion

```strada
func factorial(int $n) int {
    if ($n <= 1) {
        return 1;
    }
    return $n * factorial($n - 1);
}

say(factorial(5));  # 120
```

### Anonymous Functions (Closures)

Strada supports anonymous functions (closures) using the `func` keyword without a name.
Closures can capture variables from their enclosing scope.

#### Basic Syntax

```strada
# Anonymous function assigned to a scalar
my scalar $add = func (int $a, int $b) { return $a + $b; };

# Call using arrow syntax
my int $result = $add->(3, 4);  # 7
```

#### No Parameters

```strada
my scalar $get_pi = func () { return 3.14159; };
my num $pi = $get_pi->();
```

#### Capturing Variables

Closures capture variables from their enclosing scope by reference:

```strada
my int $multiplier = 10;
my scalar $scale = func (int $n) { return $n * $multiplier; };

say($scale->(5));   # 50
say($scale->(3));   # 30
```

#### Mutation Through Captures

Since captures are by reference, closures can modify outer variables:

```strada
my int $count = 0;
my scalar $increment = func () {
    $count = $count + 1;
    return $count;
};

say($increment->());  # 1
say($increment->());  # 2
say($increment->());  # 3
say($count);          # 3 (outer variable was modified)
```

#### Multiple Captures

Closures can capture multiple variables:

```strada
my int $a = 2;
my int $b = 3;
my int $c = 5;

my scalar $compute = func (int $x) { return $x * $a + $b * $c; };
say($compute->(10));  # 10 * 2 + 3 * 5 = 35
```

#### Passing Closures to Functions

Closures can be passed as arguments for higher-order programming:

```strada
func apply(scalar $f, int $x) int {
    return $f->($x);
}

my scalar $double = func (int $n) { return $n * 2; };
my scalar $triple = func (int $n) { return $n * 3; };

say(apply($double, 5));  # 10
say(apply($triple, 5));  # 15
```

#### Closures with String Return

```strada
my scalar $greet = func (str $name) { return "Hello, " . $name . "!"; };
say($greet->("World"));  # "Hello, World!"
```

---

## Arrays

### Creating Arrays

```strada
my array @empty = ();
my array @numbers = ();
push(@numbers, 1);
push(@numbers, 2);
push(@numbers, 3);
```

### Anonymous Arrays

```strada
my scalar $nums = [1, 2, 3, 4, 5];
my scalar $mixed = ["hello", 42, 3.14];
```

### Accessing Elements

```strada
my scalar $arr = [10, 20, 30, 40, 50];
say($arr->[0]);    # 10 (first element)
say($arr->[2]);    # 30 (third element)
say($arr->[4]);    # 50 (last element)
```

### Array Functions

```strada
my array @items = ();

# Add elements
push(@items, "first");      # Add to end
unshift(@items, "before");  # Add to beginning

# Remove elements
my scalar $last = pop(@items);     # Remove from end
my scalar $first = shift(@items);  # Remove from beginning

# Get size
my int $len = size(@items);

# Iterate
for (my int $i = 0; $i < size(@items); $i = $i + 1) {
    say(@items[$i]);
}
```

### Map, Grep, and Sort

Strada supports Perl-style `map`, `grep`, and `sort` blocks for powerful array transformations.

#### Map

Transform each element of an array using `$_` as the current element:

```strada
my array @nums = (1, 2, 3, 4, 5);

# Double each element
my scalar $doubled = map { $_ * 2; } @nums;
# Result: [2, 4, 6, 8, 10]

# Transform to strings
my scalar $strings = map { "Value: " . $_; } @nums;
```

#### Grep

Filter array elements, keeping only those where the block returns true:

```strada
my array @nums = (1, 2, 3, 4, 5, 6);

# Keep only even numbers
my scalar $evens = grep { $_ % 2 == 0; } @nums;
# Result: [2, 4, 6]

# Keep values greater than 3
my scalar $big = grep { $_ > 3; } @nums;
# Result: [4, 5, 6]
```

#### Sort

Sort arrays with custom comparison using `$a` and `$b`, and the spaceship operator `<=>`:

```strada
my array @nums = (5, 2, 8, 1, 9);

# Sort ascending (numeric)
my scalar $asc = sort { $a <=> $b; } @nums;
# Result: [1, 2, 5, 8, 9]

# Sort descending
my scalar $desc = sort { $b <=> $a; } @nums;
# Result: [9, 8, 5, 2, 1]

# Default sort (no block) - alphabetical
my scalar $alpha = sort @names;
```

The `<=>` operator returns -1 if left < right, 0 if equal, and 1 if left > right.

---

## Hashes

### Creating Hashes

```strada
my hash %empty = ();
my hash %user = ();
$user{"name"} = "Alice";
$user{"age"} = 30;
$user{"city"} = "NYC";
```

**Note:** Use `$hash{key}` (scalar sigil) when accessing individual elements, and `%hash` when referring to the whole hash (e.g., in `keys(%hash)`).

### Anonymous Hashes

```strada
my scalar $person = {
    name => "Bob",
    age => 25,
    email => "bob@example.com"
};
```

### Accessing Values

```strada
say($person->{"name"});   # Bob
say($person->{"age"});    # 25

# Setting values
$person->{"age"} = 26;
```

### Hash Functions

```strada
my hash %data = ();
$data{"a"} = 1;
$data{"b"} = 2;
$data{"c"} = 3;

# Get all keys
my array @k = keys(%data);

# Get all values
my array @v = values(%data);

# Check if key exists
if (exists($data{"a"})) {
    say("Key 'a' exists");
}

# Delete a key
delete($data{"b"});
```

---

## References

References allow you to create pointers to values, enabling complex data structures.

### Creating References

```strada
# Reference to scalar
my int $num = 42;
my scalar $ref = \$num;

# Reference to array
my array @arr = ();
push(@arr, 1);
push(@arr, 2);
my scalar $arr_ref = \@arr;

# Reference to hash
my hash %h = ();
$h{"key"} = "value";
my scalar $hash_ref = \%h;
```

### Anonymous References

```strada
# Anonymous hash reference
my scalar $person = {
    name => "Alice",
    age => 30
};

# Anonymous array reference
my scalar $numbers = [1, 2, 3, 4, 5];
```

### Dereferencing

```strada
# Scalar dereference (read)
my int $value = $$ref;

# Array element via reference
my scalar $elem = $arr_ref->[0];

# Hash element via reference
my scalar $val = $hash_ref->{"key"};
```

### Modifying Through References

You can modify the original variable through a reference using `$$ref = value`:

```strada
my scalar $original = "hello";
my scalar $ref = \$original;

$$ref = "modified";
say($original);  # prints "modified"
```

This is especially useful for passing variables by reference to functions:

```strada
func increment(scalar $ref) void {
    $$ref = $$ref + 1;
}

my int $counter = 10;
increment(\$counter);
say($counter);  # prints 11
```

You can also use `deref_set($ref, $value)` as an alternative to `$$ref = value`:

```strada
deref_set($ref, "new value");  # equivalent to: $$ref = "new value"
```

### Nested Structures

```strada
my scalar $data = {
    user => {
        name => "Alice",
        contacts => ["email", "phone", "fax"]
    },
    settings => {
        theme => "dark",
        notifications => 1
    }
};

# Access nested data
say($data->{"user"}->{"name"});              # Alice
say($data->{"user"}->{"contacts"}->[0]);     # email
say($data->{"settings"}->{"theme"});         # dark
```

### Reference Functions

```strada
my scalar $ref = \$value;

# Check if reference
if (is_ref($ref)) {
    say("It's a reference");
}

# Get reference type
my str $type = reftype($ref);  # "scalar", "array", or "hash"
```

---

## Strings

### String Literals

```strada
my str $single = 'Hello';           # Single quotes
my str $double = "World";           # Double quotes
my str $escaped = "Line1\nLine2";   # Escape sequences
```

### Escape Sequences

| Sequence | Meaning |
|----------|---------|
| `\n` | Newline |
| `\t` | Tab |
| `\r` | Carriage return |
| `\\` | Backslash |
| `\"` | Double quote |
| `\'` | Single quote |

### String Operations

```strada
my str $s = "Hello, World!";

# Length
my int $len = length($s);           # 13

# Substring
my str $sub = substr($s, 0, 5);     # "Hello"
my str $sub2 = substr($s, 7, 5);    # "World"

# Concatenation
my str $full = $s . " How are you?";

# Case conversion
my str $upper = uc($s);             # "HELLO, WORLD!"
my str $lower = lc($s);             # "hello, world!"
my str $ucf = ucfirst($lower);      # "Hello, world!"

# Trimming
my str $trimmed = trim("  spaces  ");  # "spaces"
my str $ltrimmed = ltrim("  left");    # "left"
my str $rtrimmed = rtrim("right  ");   # "right"

# Finding
my int $pos = index($s, "World");   # 7
my int $rpos = rindex($s, "o");     # 8

# Character operations
my str $char = chr(65);             # "A"
my int $code = ord("A");            # 65
```

### String Formatting

```strada
# Printf-style formatting
printf("Name: %s, Age: %d\n", $name, $age);

# Sprintf returns formatted string
my str $formatted = sprintf("%s is %d years old", $name, $age);
```

### Binary/Byte Operations

For working with binary data (protocols, file formats, etc.), use the `sys::` byte functions:

```strada
# Get bytes from strings (binary-safe, not UTF-8 aware)
my int $byte = sys::ord_byte($str);        # First byte (0-255)
my int $b = sys::get_byte($str, 5);        # Byte at position
my int $len = sys::byte_length($str);      # Byte count
my str $sub = sys::byte_substr($str, 0, 4); # Substring by bytes

# Set a byte (returns new string)
my str $modified = sys::set_byte($str, 0, 0xFF);
```

#### Pack and Unpack

Use `sys::pack()` and `sys::unpack()` for binary protocol construction and parsing:

```strada
# Pack values into binary string
my str $header = sys::pack("NnC", 0x12345678, 80, 255);
# N = 4-byte big-endian int, n = 2-byte big-endian short, C = 1-byte unsigned

# Unpack binary data to array
my array @values = sys::unpack("NnC", $header);
my int $magic = $values[0];   # 0x12345678
my int $port = $values[1];    # 80
my int $flags = $values[2];   # 255
```

**Pack Format Characters:**

| Char | Description | Size |
|------|-------------|------|
| `c/C` | Signed/unsigned char | 1 byte |
| `s/S` | Signed/unsigned short (native endian) | 2 bytes |
| `n/v` | Unsigned short (big/little endian) | 2 bytes |
| `l/L` | Signed/unsigned long (native endian) | 4 bytes |
| `N/V` | Unsigned long (big/little endian) | 4 bytes |
| `q/Q` | Signed/unsigned quad (native endian) | 8 bytes |
| `a/A` | ASCII string (null/space padded) | variable |
| `H` | Hex string (high nybble first) | variable |
| `x/X` | Null byte / backup one byte | 1 byte |

**Example: DNS Query Header**

```strada
func build_dns_header(int $id, int $flags) str {
    # DNS header: ID(2), Flags(2), QD(2), AN(2), NS(2), AR(2)
    return sys::pack("nnnnnn", $id, $flags, 1, 0, 0, 0);
}

func parse_dns_header(str $data) hash {
    my array @fields = sys::unpack("nnnnnn", $data);
    my hash %header = ();
    $header{"id"} = $fields[0];
    $header{"flags"} = $fields[1];
    $header{"questions"} = $fields[2];
    $header{"answers"} = $fields[3];
    return %header;
}
```

---

## File I/O

**Note:** File I/O functions use the `sys::` namespace.

### Reading Files

```strada
# Read entire file
my str $content = sys::slurp("file.txt");

# Read line by line
my scalar $fh = sys::open("file.txt", "r");
while (1) {
    my str $line = sys::readline($fh);
    if (!defined($line)) {
        last;
    }
    say($line);
}
sys::close($fh);
```

### Writing Files

```strada
# Write entire file
sys::spew("output.txt", "Hello, World!\n");

# Write with file handle
my scalar $fh = sys::open("output.txt", "w");
sys::write($fh, "Line 1\n");
sys::write($fh, "Line 2\n");
sys::close($fh);
```

### File Modes

| Mode | Description |
|------|-------------|
| `"r"` | Read |
| `"w"` | Write (truncate) |
| `"a"` | Append |
| `"r+"` | Read and write |

---

## Regular Expressions

Strada supports both Perl-style inline regex operators and function-based regex operations.

### Inline Regex Operators (Perl-style)

The `=~` and `!~` operators provide a familiar Perl-like syntax for regex operations:

```strada
my str $text = "Hello, World!";

# Match operator
if ($text =~ /World/) {
    say("Found 'World'");
}

# Negated match
if ($text !~ /Goodbye/) {
    say("'Goodbye' not found");
}

# Anchors
if ($text =~ /^Hello/) {
    say("Starts with 'Hello'");
}
if ($text =~ /!$/) {
    say("Ends with '!'");
}
```

### Substitution Operator

The `s///` operator replaces matched patterns in-place:

```strada
my str $greeting = "Hello World";

# Replace first occurrence
$greeting =~ s/World/Strada/;
say($greeting);  # "Hello Strada"

# Global replacement with 'g' flag
my str $repeat = "foo bar foo baz foo";
$repeat =~ s/foo/XXX/g;
say($repeat);  # "XXX bar XXX baz XXX"
```

### Escaping Special Characters

Use backslash to escape special regex characters and delimiters:

```strada
# Match literal forward slash
if ($path =~ /\/usr\/bin/) {
    say("Unix path");
}

# Match special characters
if ($text =~ /\d+/) {      # Digits
    say("Contains numbers");
}
```

### Function-Based Matching

For simple cases or dynamic patterns, use the function-based API:

```strada
my str $text = "Hello, World!";

# Simple match
if (match($text, "World")) {
    say("Found it!");
}

# Match with variable pattern
my str $pattern = "World";
if (match($text, $pattern)) {
    say("Found using variable");
}

# Case-insensitive (use (?i) flag)
if (match($text, "(?i)world")) {
    say("Found (case-insensitive)");
}
```

### Replacing

```strada
# Replace first occurrence
my str $result = replace($text, "World", "Strada");

# Replace all occurrences
my str $result2 = replace_all($text, "l", "L");
```

### Splitting

```strada
my str $csv = "apple,banana,cherry";
my array @parts = split(",", $csv);
# @parts = ["apple", "banana", "cherry"]
```

### Capturing

```strada
my str $date = "2024-01-15";
my array @matches = capture($date, "(\d+)-(\d+)-(\d+)");
# @matches = ["2024", "01", "15"]
```

---

## Process Control

Strada provides functions for process management, similar to Perl and POSIX systems.

**Note:** Process control functions use the `sys::` namespace.

### Sleeping

```strada
# Sleep for 2 seconds
sys::sleep(2);

# Sleep for 500 milliseconds (500,000 microseconds)
sys::usleep(500000);
```

### Forking Processes

```strada
my int $pid = sys::fork();

if ($pid == 0) {
    # Child process
    say("I am the child, PID: " . sys::getpid());
    sys::exit(0);
} elsif ($pid > 0) {
    # Parent process
    say("I am the parent, child PID: " . $pid);
    sys::wait();  # Wait for child to finish
    say("Child finished");
} else {
    # Fork failed
    die("Fork failed");
}
```

### Process Information

```strada
# Get current process ID
my int $pid = sys::getpid();

# Get parent process ID
my int $ppid = sys::getppid();
```

### Waiting for Child Processes

```strada
# Wait for any child
sys::wait();

# Wait for specific child (blocking)
sys::waitpid($child_pid, 0);

# Non-blocking wait (check without blocking)
# my int $result = sys::waitpid($child_pid, 1);  # WNOHANG
```

### Executing Commands

```strada
# Run command via shell and get exit status (like Perl's system())
my int $status = sys::system("ls -la");
say("Exit status: " . $status);

# Run command with shell interpretation
sys::system("echo 'Hello' && echo 'World'");

# Replace current process with new command (does not return on success)
sys::exec("top");
say("This is never reached if exec succeeds");
```

### Process Name and Title

You can change how your process appears in `ps` output:

```strada
# Get/set process name (comm column in ps, max 15 chars)
my str $name = sys::getprocname();
sys::setprocname("myworker");

# Get/set process title (args column in ps)
my str $title = sys::getproctitle();
sys::setproctitle("myapp: processing job 42");
```

Example showing the difference:

```strada
func main() int {
    say("Original name: " . sys::getprocname());
    say("Original title: " . sys::getproctitle());

    sys::setprocname("worker");
    sys::setproctitle("worker: idle");

    # Now 'ps' shows:
    # COMM           ARGS
    # worker         worker: idle

    return 0;
}
```

### Signal Handling

Strada supports POSIX signal handling with custom handlers, ignore, and default actions.

```strada
# Define a signal handler function
func sigint_handler(int $sig) void {
    say("Caught SIGINT (signal " . $sig . "), cleaning up...");
    sys::exit(0);
}

func main() int {
    # Register handler for SIGINT (Ctrl+C)
    sys::signal("INT", \&sigint_handler);

    # Ignore SIGPIPE (useful for network code)
    sys::signal("PIPE", "IGNORE");

    # Restore default behavior for a signal
    sys::signal("TERM", "DEFAULT");

    say("Waiting for signals...");
    while (1) {
        sys::sleep(1);
    }

    return 0;
}
```

#### Supported Signals

| Name | Description |
|------|-------------|
| `INT` | Interrupt (Ctrl+C) |
| `TERM` | Termination |
| `HUP` | Hangup |
| `QUIT` | Quit |
| `USR1` | User-defined 1 |
| `USR2` | User-defined 2 |
| `ALRM` | Alarm timer |
| `PIPE` | Broken pipe |
| `CHLD` | Child status changed |
| `CONT` | Continue |
| `STOP` | Stop |
| `TSTP` | Terminal stop (Ctrl+Z) |

#### Handler Actions

- **Function reference** (`\&handler`): Call the function when signal arrives
- **`"IGNORE"`**: Silently ignore the signal
- **`"DEFAULT"`**: Restore the default signal behavior

---

## Inter-Process Communication

Strada provides low-level IPC primitives for communicating between processes.

**Note:** IPC functions use the `sys::` namespace.

### Creating Pipes

```strada
# Create a pipe - returns array with [read_fd, write_fd]
my scalar $pipe = sys::pipe();

if (!defined($pipe)) {
    die("Failed to create pipe");
}

my int $read_fd = $pipe->[0];
my int $write_fd = $pipe->[1];
```

### Pipe Communication Example

```strada
my scalar $pipe = sys::pipe();
my int $pid = sys::fork();

if ($pid == 0) {
    # Child: close read end, write to pipe
    sys::close_fd($pipe->[0]);
    sys::write_fd($pipe->[1], "Hello from child!\n");
    sys::close_fd($pipe->[1]);
    sys::exit(0);
} else {
    # Parent: close write end, read from pipe
    sys::close_fd($pipe->[1]);
    my str $message = sys::read_all_fd($pipe->[0]);
    sys::close_fd($pipe->[0]);
    sys::wait();
    say("Received: " . $message);
}
```

### File Descriptor Operations

```strada
# Read up to N bytes from file descriptor
my str $data = sys::read_fd($fd, 1024);

# Read all available data until EOF
my str $all = sys::read_all_fd($fd);

# Write data to file descriptor
my int $bytes = sys::write_fd($fd, "Hello\n");

# Close file descriptor
sys::close_fd($fd);

# Duplicate file descriptor (like dup2)
sys::dup2($old_fd, $new_fd);
```

### Redirecting I/O with dup2

```strada
# Redirect stdout to a pipe
my scalar $pipe = sys::pipe();
my int $saved_stdout = 1;  # Save for later if needed

# In child process:
sys::close_fd($pipe->[0]);  # Close read end
sys::dup2($pipe->[1], 1);   # Redirect stdout to pipe write end
sys::close_fd($pipe->[1]);  # Close original write fd

# Now any output goes to the pipe
say("This goes to the pipe!");
```

### IPC::Open3 Module

For more convenient subprocess I/O, use the `IPC::Open3` module in `lib/`:

```strada
# lib/IPC/Open3.strada provides:
# - open3($cmd) - Run command with separate stdin/stdout/stderr
# - open2($cmd) - Run command with combined stdout/stderr
# - cmd_output($cmd) - Capture command output (like backticks)

func open3(str $cmd) hash {
    # Returns hash with: pid, stdin, stdout, stderr
}

func cmd_output(str $cmd) str {
    # Runs command and returns stdout
}
```

Example usage:

```strada
# Capture command output
my str $hostname = cmd_output("hostname");
say("Host: " . $hostname);

# Full I/O control
my hash $proc = open3("cat -n");
sys::write_fd($proc->{"stdin"}, "Line 1\nLine 2\n");
sys::close_fd($proc->{"stdin"});
my str $output = sys::read_all_fd($proc->{"stdout"});
sys::close_fd($proc->{"stdout"});
sys::close_fd($proc->{"stderr"});
sys::wait();
say($output);
```

---

## Multithreading

Strada provides POSIX thread support through the `thread::` namespace, enabling concurrent execution with proper synchronization primitives.

### Creating Threads

Use `thread::create()` with a closure to spawn a new thread:

```strada
my scalar $thread = thread::create(func () {
    say("Hello from thread!");
});

# Wait for thread to complete
thread::join($thread);
```

### Thread Functions

| Function | Description |
|----------|-------------|
| `thread::create($closure)` | Create and start a new thread running the closure |
| `thread::join($thread)` | Wait for thread to complete, returns thread's result |
| `thread::detach($thread)` | Detach thread (runs independently) |
| `thread::self()` | Get current thread ID |

### Mutexes

Mutexes prevent race conditions when multiple threads access shared data:

```strada
my int $counter = 0;
my scalar $mutex = thread::mutex_new();

my scalar $t1 = thread::create(func () {
    my int $i = 0;
    while ($i < 1000) {
        thread::mutex_lock($mutex);
        $counter = $counter + 1;
        thread::mutex_unlock($mutex);
        $i = $i + 1;
    }
});

my scalar $t2 = thread::create(func () {
    my int $i = 0;
    while ($i < 1000) {
        thread::mutex_lock($mutex);
        $counter = $counter + 1;
        thread::mutex_unlock($mutex);
        $i = $i + 1;
    }
});

thread::join($t1);
thread::join($t2);
thread::mutex_destroy($mutex);

say("Counter: " . $counter);  # Always 2000
```

| Function | Description |
|----------|-------------|
| `thread::mutex_new()` | Create a new mutex |
| `thread::mutex_lock($mutex)` | Lock mutex (blocks if already locked) |
| `thread::mutex_trylock($mutex)` | Try to lock (returns 0 if success, non-zero if busy) |
| `thread::mutex_unlock($mutex)` | Unlock mutex |
| `thread::mutex_destroy($mutex)` | Destroy mutex |

### Condition Variables

Condition variables allow threads to wait for specific conditions:

```strada
my int $ready = 0;
my scalar $mutex = thread::mutex_new();
my scalar $cond = thread::cond_new();

# Consumer thread - waits for data
my scalar $consumer = thread::create(func () {
    thread::mutex_lock($mutex);
    while ($ready == 0) {
        thread::cond_wait($cond, $mutex);
    }
    thread::mutex_unlock($mutex);
    say("Got signal, ready = " . $ready);
});

# Producer - signals when ready
sys::usleep(10000);  # Simulate work
thread::mutex_lock($mutex);
$ready = 1;
thread::cond_signal($cond);
thread::mutex_unlock($mutex);

thread::join($consumer);
thread::cond_destroy($cond);
thread::mutex_destroy($mutex);
```

| Function | Description |
|----------|-------------|
| `thread::cond_new()` | Create a condition variable |
| `thread::cond_wait($cond, $mutex)` | Wait for signal (releases mutex while waiting) |
| `thread::cond_signal($cond)` | Wake one waiting thread |
| `thread::cond_broadcast($cond)` | Wake all waiting threads |
| `thread::cond_destroy($cond)` | Destroy condition variable |

### Thread Safety Notes

- Strada's reference counting is atomic, making basic operations thread-safe
- Always use mutexes when modifying shared variables
- Closures capture variables by reference - changes are visible across threads
- Programs using threads must link with `-lpthread`

---

## Packages and Modules

### Declaring a Package

```strada
# File: lib/Math/Utils.sm
package Math::Utils;

func add(int $a, int $b) int {
    return $a + $b;
}

func multiply(int $a, int $b) int {
    return $a * $b;
}
```

### Using Modules

```strada
# Import all functions (use qualified names)
use Math::Utils;

my int $sum = Math::Utils::add(10, 20);

# Import specific functions
use Math::Utils qw(add multiply);

my int $sum = add(10, 20);        # Unqualified
my int $prod = multiply(5, 6);
```

### Library Paths

```strada
use lib "/path/to/modules";
use MyModule;
```

### Module File Structure

```
project/
├── main.strada
└── lib/
    └── Math/
        └── Utils.sm
```

### Object-Oriented Programming

Strada supports Perl-style OOP using blessed references. This allows you to associate
a hash reference with a package (class) and build inheritance hierarchies.

#### Blessed References

Use `bless()` to associate a hash reference with a package:

```strada
package Animal;

func new(str $name) scalar {
    my hash %self = ();
    $self{"name"} = $name;
    return bless(\%self, __PACKAGE__);  # Returns the blessed reference
}

func speak(scalar $self) void {
    say($self->{"name"} . " makes a sound");
}

func main() void {
    my scalar $animal = new("Buddy");
    say(blessed($animal));  # "Animal"
    speak($animal);
}
```

#### The `__PACKAGE__` Keyword

`__PACKAGE__` returns the current package name as a string:

```strada
package My::App;

func main() void {
    say(__PACKAGE__);  # "My::App"
}
```

#### Inheritance

Use `inherit` to set up inheritance relationships. This can be done at file level
or within functions:

```strada
# File-level inheritance (preferred)
package Dog;
inherit Animal;

# Or within a function
func Dog_init() void {
    set_package("Dog");
    inherit("Animal");
}
```

With two arguments, you can specify both child and parent:

```strada
inherit("Dog", "Animal");  # Dog inherits from Animal
```

#### Multi-Level Inheritance

Build class hierarchies with multiple levels:

```strada
# Animal.strada
package Animal;

func Animal_new(str $name) scalar {
    my hash %self = ();
    $self{"name"} = $name;
    return bless(\%self, "Animal");
}
```

```strada
# Dog.strada
package Dog;
inherit Animal;

func Dog_new(str $name) scalar {
    my hash %self = ();
    $self{"name"} = $name;
    return bless(\%self, "Dog");
}
```

```strada
# GermanShepherd.strada
package GermanShepherd;
inherit Dog;

func GermanShepherd_new(str $name) scalar {
    my hash %self = ();
    $self{"name"} = $name;
    $self{"breed"} = "German Shepherd";
    return bless(\%self, "GermanShepherd");
}
```

#### Multiple Inheritance

A class can inherit from multiple parent classes. Use comma-separated parents
with the `inherit` statement, or call `inherit()` multiple times:

```strada
# Comma-separated syntax (single-package files)
package Duck;
inherit Animal, Flyable, Swimmable;

# Or use explicit inherit() calls (required for multi-package files)
package Duck;

func Duck_init() void {
    inherit("Duck", "Animal");
    inherit("Duck", "Flyable");
    inherit("Duck", "Swimmable");
}
```

When using multiple inheritance, method resolution follows depth-first search
through the inheritance hierarchy (left-to-right through parents).

```strada
package Flyable;
func Flyable_fly(scalar $self) str {
    return "Flying!";
}

package Swimmable;
func Swimmable_swim(scalar $self) str {
    return "Swimming!";
}

package Duck;
func Duck_init() void {
    inherit("Duck", "Animal");
    inherit("Duck", "Flyable");
    inherit("Duck", "Swimmable");
}

func main() int {
    Duck_init();
    my scalar $duck = Duck_new("Donald");

    # Duck inherits from all three parents
    say(isa($duck, "Animal"));    # 1
    say(isa($duck, "Flyable"));   # 1
    say(isa($duck, "Swimmable")); # 1

    # Can call methods from any parent
    say(Flyable_fly($duck));      # "Flying!"
    say(Swimmable_swim($duck));   # "Swimming!"
    return 0;
}
```

**Note:** When using multiple packages in one file, you must use the
`inherit("Child", "Parent")` function call syntax instead of the
`inherit Parent;` statement syntax.

#### SUPER:: Calls

Call a parent class method using `SUPER::method()`:

```strada
package Animal;
func Animal_speak(scalar $self) str {
    return "Animal speaks";
}

package Dog;
func Dog_init() void {
    inherit("Dog", "Animal");
}

func Dog_speak(scalar $self) str {
    # Call parent's speak method
    my str $parent_msg = SUPER::speak($self);
    return $parent_msg . " - Dog says Woof!";
}

func main() int {
    Dog_init();
    my scalar $dog = Dog_new("Buddy");
    say(Dog_speak($dog));  # "Animal speaks - Dog says Woof!"
    return 0;
}
```

The first argument to `SUPER::method()` must be the object (`$self`).

#### DESTROY Destructors

Define a `DESTROY` method to run cleanup code when an object is freed:

```strada
package FileHandler;

func FileHandler_new(str $filename) scalar {
    my hash %self = ();
    $self{"filename"} = $filename;
    $self{"handle"} = open($filename, "r");
    say("Opened " . $filename);
    return bless(\%self, "FileHandler");
}

func FileHandler_DESTROY(scalar $self) void {
    say("Closing " . $self->{"filename"});
    close($self->{"handle"});
}
```

DESTROY is automatically called when the object's reference count reaches zero.
To chain destructors in an inheritance hierarchy, call `SUPER::DESTROY($self)`:

```strada
func Child_DESTROY(scalar $self) void {
    say("Child cleanup");
    SUPER::DESTROY($self);  # Call parent's DESTROY
}
```

#### Type Checking with `isa()`

Check if an object is of a type (follows inheritance chain):

```strada
my scalar $dog = GermanShepherd_new("Max");

if (isa($dog, "GermanShepherd")) {
    say("Is a GermanShepherd");  # YES
}
if (isa($dog, "Dog")) {
    say("Is a Dog");  # YES (inherited)
}
if (isa($dog, "Animal")) {
    say("Is an Animal");  # YES (inherited from Dog)
}
if (!isa($dog, "Cat")) {
    say("Is not a Cat");  # Correct
}
```

#### UNIVERSAL Methods

Strada provides UNIVERSAL methods that can be called on any blessed object using method syntax:

```strada
my scalar $dog = Dog_new("Buddy");

# Method-style type checking
if ($dog->isa("Animal")) {
    say("Dog is an Animal");
}

# Check if object can perform a method
if ($dog->can("speak")) {
    say("Dog can speak");
}
```

These UNIVERSAL methods work on any blessed reference:

```strada
# Create different objects
my scalar $cat = Cat_new("Whiskers");
my scalar $dog = Dog_new("Rex");

# Both can use UNIVERSAL methods
say($cat->isa("Animal"));  # 1 (true)
say($dog->isa("Animal"));  # 1 (true)
say($cat->isa("Dog"));     # 0 (false)

# Check capabilities
if ($dog->can("fetch")) {
    $dog->fetch();
}
```

The `can()` method returns true if the object's class (or any parent class) defines the specified method.

#### OOP Functions Reference

| Function | Description |
|----------|-------------|
| `bless($ref, "Package")` | Bless reference into package, returns ref |
| `blessed($ref)` | Get package name or undef if not blessed |
| `set_package("Name")` | Set current package context |
| `inherit("Parent")` | Inherit from Parent (uses current package) |
| `inherit("Child", "Parent")` | Set up inheritance explicitly |
| `inherit Parent;` | Statement form (single-package files only) |
| `inherit A, B, C;` | Multiple inheritance statement form |
| `isa($obj, "Package")` | Check if object is of type (follows inheritance) |
| `$obj->isa("Package")` | Method-style type check (UNIVERSAL) |
| `can($obj, "method")` | Check if object can do a method |
| `$obj->can("method")` | Method-style capability check (UNIVERSAL) |
| `SUPER::method($self, ...)` | Call parent class method |
| `Package_DESTROY($self)` | Destructor (called when object freed) |
| `__PACKAGE__` | Returns current package name as string |

#### Complete OOP Example

See `examples/test_oop_better.strada` and `examples/test_multi_inherit.strada` for complete examples demonstrating:
- Base classes and constructors
- Single and multi-level inheritance
- Multiple inheritance (mixin pattern)
- Type checking with `isa()` and `can()`
- SUPER:: calls to parent methods
- Package management

---

## C Interoperability

### `__C__` Blocks

Use `__C__` blocks to embed raw C code directly in Strada programs:

**Top-level blocks** (at file scope) for includes and globals:

```strada
__C__ {
    #include <math.h>
    #include <openssl/ssl.h>
    static SSL_CTX *g_ctx = NULL;
}
```

**Statement-level blocks** (inside functions) for inline C:

```strada
func my_sqrt(num $x) num {
    __C__ {
        double val = strada_to_num(x);
        return strada_new_num(sqrt(val));
    }
}

func main() int {
    my num $result = my_sqrt(16.0);
    say($result);  # 4.0
    return 0;
}
```

### Opaque Handle Pattern

Store C pointers as `int` (64-bit) values:

```strada
# In C code: convert pointer to int
__C__ {
    SSL *conn = SSL_new(ctx);
    return strada_new_int((int64_t)(intptr_t)conn);
}

# Later: retrieve pointer from int
__C__ {
    SSL *conn = (SSL*)(intptr_t)strada_to_int(handle);
    // use conn...
}
```

### Native Structs

```strada
struct Point {
    i32 x;
    i32 y;
}

func main() int {
    my Point $p = Point{};
    $p->x = 10;
    $p->y = 20;
    say($p->x);  # 10
    return 0;
}
```

### Function Pointers

```strada
struct Callback {
    func(int, int) int handler;
}

func my_handler(int $a, int $b) int {
    return $a + $b;
}

func main() int {
    my Callback $cb = Callback{};
    $cb->handler = &my_handler;
    my int $result = $cb->handler(10, 20);
    say($result);  # 30
    return 0;
}
```

### Raw C Code Blocks

For maximum control, embed raw C code directly using `__C__ { }`:

```strada
func main() int {
    my int $x = 10;

    __C__ {
        // Access Strada variables (they're StradaValue* pointers)
        long long val = strada_to_int(x);
        printf("C sees x as: %lld\n", val);

        // Modify Strada variables
        strada_decref(x);
        x = strada_new_int(val * 2);

        // Use any C features
        for (int i = 0; i < 3; i++) {
            printf("Loop %d\n", i);
        }
    }

    say("x is now " . $x);  # 20
    return 0;
}
```

Key points:
- Strada variables are `StradaValue*` pointers
- Use `strada_to_int()`, `strada_to_str()`, etc. to extract values
- Use `strada_new_int()`, `strada_new_str()`, etc. to create values
- Remember to `strada_decref()` before replacing a variable
- All `strada_runtime.h` functions are available

---

## Perl Integration

Strada provides bidirectional integration with Perl 5, allowing you to:
- Call Perl code from Strada (embed a Perl interpreter)
- Call Strada from Perl (via XS module)

### Calling Perl from Strada

The `perl5` module embeds a Perl interpreter in your Strada program:

```strada
use lib "lib";
use perl5;

func main() int {
    perl5::init();

    # Evaluate Perl expressions
    my str $result = perl5::eval("2 ** 10");
    say("2^10 = " . $result);  # 1024

    # Use CPAN modules
    if (perl5::use_module("JSON") == 1) {
        my str $json = perl5::eval('encode_json({foo => "bar"})');
        say($json);
    }

    # Call Perl subroutines
    perl5::run("sub greet { return 'Hello, ' . shift; }");
    my array @args = ("World");
    my str $greeting = perl5::call("greet", \@args);
    say($greeting);

    perl5::shutdown();
    return 0;
}
```

Build the Perl library first:
```bash
cd lib/perl5 && make
```

### Calling Strada from Perl

The `Strada` Perl module allows Perl programs to load Strada shared libraries:

```perl
use Strada;

my $lib = Strada::Library->new('./libmylib.so');
my $result = $lib->call('mypackage__add', 10, 20);
print "Result: $result\n";
$lib->unload();
```

To create a Strada shared library:
```bash
./stradac mylib.strada mylib.c
gcc -shared -fPIC -rdynamic -o libmylib.so mylib.c \
    runtime/strada_runtime.c -Iruntime -ldl -lm
```

Build the Perl module:
```bash
cd perl/Strada
perl Makefile.PL && make && make test
```

### More Information

See `docs/PERL_INTEGRATION.md` for comprehensive documentation on:
- Full API reference for both directions
- Type conversion tables
- XS module support and LD_PRELOAD workarounds
- Complete working examples

---

## Built-in Functions

### Output

| Function | Description |
|----------|-------------|
| `say($val)` | Print with newline |
| `print($val)` | Print without newline |
| `printf($fmt, ...)` | Formatted print |
| `warn($msg)` | Print to stderr |
| `die($msg)` | Print error and exit |

### Type Conversion

| Function | Description |
|----------|-------------|
| `cast_int($val)` | Convert to integer |
| `cast_num($val)` | Convert to number |
| `cast_str($val)` | Convert to string |

### Type Checking

| Function | Description |
|----------|-------------|
| `defined($val)` | Check if defined |
| `typeof($val)` | Get type name |
| `is_ref($val)` | Check if reference |
| `reftype($val)` | Get reference type |

### Array Functions

| Function | Description |
|----------|-------------|
| `push(@arr, $val)` | Add to end |
| `pop(@arr)` | Remove from end |
| `shift(@arr)` | Remove from beginning |
| `unshift(@arr, $val)` | Add to beginning |
| `size(@arr)` | Get length |
| `map { $_ } @arr` | Transform elements |
| `grep { $_ } @arr` | Filter elements |
| `sort { $a <=> $b } @arr` | Sort with comparator |

### Hash Functions

| Function | Description |
|----------|-------------|
| `keys(%hash)` | Get all keys |
| `values(%hash)` | Get all values |
| `exists($hash{$key})` | Check key exists |
| `delete($hash{$key})` | Remove key |

### String Functions

| Function | Description |
|----------|-------------|
| `length($str)` | String length |
| `substr($str, $pos, $len)` | Substring |
| `index($str, $sub)` | Find substring |
| `rindex($str, $sub)` | Find from end |
| `uc($str)` | Uppercase |
| `lc($str)` | Lowercase |
| `ucfirst($str)` | Uppercase first |
| `lcfirst($str)` | Lowercase first |
| `trim($str)` | Remove whitespace |
| `ltrim($str)` | Remove left whitespace |
| `rtrim($str)` | Remove right whitespace |
| `reverse($str)` | Reverse string |
| `repeat($str, $n)` | Repeat string |
| `chr($code)` | Code to character |
| `ord($char)` | Character to code |
| `chomp($str)` | Remove trailing newline |
| `chop($str)` | Remove last character |
| `join($sep, @arr)` | Join array to string |

### File Functions (sys:: namespace)

| Function | Description |
|----------|-------------|
| `sys::open($path, $mode)` | Open file |
| `sys::close($fh)` | Close file |
| `sys::readline($fh)` | Read line |
| `sys::slurp($path)` | Read entire file |
| `sys::spew($path, $data)` | Write entire file |

### Regex Functions

| Function | Description |
|----------|-------------|
| `match($str, $pattern)` | Test match |
| `replace($str, $pat, $rep)` | Replace first |
| `replace_all($str, $pat, $rep)` | Replace all |
| `split($pat, $str)` | Split string |
| `capture($str, $pat)` | Extract captures |

### Utility Functions

| Function | Description |
|----------|-------------|
| `dumper($val)` | Debug dump value |
| `clone($val)` | Deep copy |
| `exit($code)` | Exit program |

### Process Control (sys:: namespace)

| Function | Description |
|----------|-------------|
| `sys::sleep($seconds)` | Sleep for seconds |
| `sys::usleep($microseconds)` | Sleep for microseconds |
| `sys::fork()` | Fork process |
| `sys::wait()` | Wait for any child |
| `sys::waitpid($pid, $opts)` | Wait for specific child |
| `sys::getpid()` | Get process ID |
| `sys::getppid()` | Get parent process ID |
| `sys::system($cmd)` | Run shell command |
| `sys::exec($cmd)` | Replace process with command |
| `sys::setprocname($name)` | Set process name |
| `sys::getprocname()` | Get process name |
| `sys::setproctitle($title)` | Set process title |
| `sys::getproctitle()` | Get process title |
| `sys::signal($name, $handler)` | Set signal handler |

### Inter-Process Communication (sys:: namespace)

| Function | Description |
|----------|-------------|
| `sys::pipe()` | Create pipe, returns [read, write] |
| `sys::dup2($old, $new)` | Duplicate file descriptor |
| `sys::close_fd($fd)` | Close file descriptor |
| `sys::read_fd($fd, $size)` | Read from file descriptor |
| `sys::write_fd($fd, $data)` | Write to file descriptor |
| `sys::read_all_fd($fd)` | Read all until EOF |

---

## Best Practices

### Naming Conventions

```strada
# Variables: lowercase with underscores
my int $user_count = 0;
my str $file_name = "data.txt";

# Functions: lowercase with underscores
func calculate_total(int $a, int $b) int { ... }
func get_user_by_id(int $id) scalar { ... }

# Constants: uppercase (by convention)
my int $MAX_SIZE = 1000;
my str $DEFAULT_NAME = "Unknown";
```

### Error Handling

```strada
func safe_divide(num $a, num $b) num {
    if ($b == 0) {
        die("Division by zero");
    }
    return $a / $b;
}

# Check for errors
my scalar $result = open("file.txt", "r");
if (!defined($result)) {
    die("Could not open file");
}
```

### Code Organization

```strada
# Group related functions together
# Use comments to document sections

# ============================================================
# User Management Functions
# ============================================================

func create_user(str $name, str $email) scalar { ... }
func delete_user(int $id) void { ... }
func get_user(int $id) scalar { ... }

# ============================================================
# Data Processing Functions
# ============================================================

func process_data(scalar $data) scalar { ... }
func validate_data(scalar $data) int { ... }
```

### Memory Management

Strada uses reference counting, but you can help:

```strada
# Free large structures when done
my scalar $big_data = load_big_file();
process($big_data);
$big_data = undef;  # Release memory
```

### Performance Tips

1. **Use appropriate types**: `int` is faster than `num` for integers
2. **Avoid unnecessary string concatenation** in loops
3. **Pre-allocate arrays** when size is known
4. **Use `extern` functions** for performance-critical code

---

## Appendix: Complete Example

```strada
# A complete Strada program demonstrating various features

use lib "lib";

func main() int {
    say("=== Strada Demo ===\n");
    
    # Variables and types
    my str $name = "Alice";
    my int $age = 30;
    my num $balance = 1234.56;
    
    # String formatting
    printf("Name: %s, Age: %d, Balance: $%.2f\n", $name, $age, $balance);
    
    # Arrays
    my array @colors = ();
    push(@colors, "red");
    push(@colors, "green");
    push(@colors, "blue");
    
    say("\nColors:");
    for (my int $i = 0; $i < size(@colors); $i = $i + 1) {
        say("  - " . @colors[$i]);
    }
    
    # Hashes
    my scalar $person = {
        name => "Bob",
        age => 25,
        city => "NYC"
    };
    
    say("\nPerson:");
    say("  Name: " . $person->{"name"});
    say("  Age: " . $person->{"age"});
    say("  City: " . $person->{"city"});
    
    # Functions
    my int $sum = add(10, 20);
    say("\n10 + 20 = " . $sum);
    
    my int $fact = factorial(5);
    say("5! = " . $fact);
    
    # String operations
    my str $text = "  Hello, World!  ";
    say("\nString operations:");
    say("  Original: '" . $text . "'");
    say("  Trimmed: '" . trim($text) . "'");
    say("  Upper: '" . uc(trim($text)) . "'");
    say("  Length: " . length(trim($text)));
    
    say("\n=== Demo Complete ===");
    return 0;
}

func add(int $a, int $b) int {
    return $a + $b;
}

func factorial(int $n) int {
    if ($n <= 1) {
        return 1;
    }
    return $n * factorial($n - 1);
}
```

---

## Foreign Function Interface (FFI)

Strada provides direct access to C shared libraries through dynamic loading.

**Note:** FFI functions use the `sys::` namespace.

### Loading Libraries

```strada
# Load a shared library
my int $lib = sys::dl_open("libm.so.6");
if ($lib == 0) {
    say("Error: " . sys::dl_error());
    exit(1);
}

# Get a function pointer
my int $sqrt_fn = sys::dl_sym($lib, "sqrt");

# Close when done
sys::dl_close($lib);
```

### Calling Foreign Functions

There are two sets of FFI call functions:

**Basic FFI** - Arguments are converted to C primitive types (int64_t, double):

```strada
# Call function returning int (up to 5 args)
my int $result = sys::dl_call_int($fn, [arg1, arg2, ...]);

# Call function returning double (up to 2 args)
my num $result = sys::dl_call_num($fn, [arg1, arg2]);

# Call function returning string
my str $result = sys::dl_call_str($fn, $arg);

# Call void function
sys::dl_call_void($fn, [arg1, arg2, ...]);
```

**Enhanced FFI (_sv variants)** - Arguments are passed as `StradaValue*` pointers, allowing C code to extract values with full type information:

```strada
# Call function that takes StradaValue* args and returns int
my int $result = sys::dl_call_int_sv($fn, [arg1, arg2, ...]);

# Call function that takes StradaValue* args and returns string
my str $result = sys::dl_call_str_sv($fn, [arg1, arg2, ...]);

# Call void function that takes StradaValue* args
sys::dl_call_void_sv($fn, [arg1, arg2, ...]);
```

Use the `_sv` variants when:
- Your C function needs to handle multiple types (strings, ints, etc.)
- You're writing a Strada-specific C library wrapper
- The basic FFI misinterprets your argument types (e.g., strings as integers)

### Complete FFI Example

```strada
func main() void {
    # Load math library
    my int $libm = sys::dl_open("libm.so.6");

    # Get sqrt function
    my int $sqrt = sys::dl_sym($libm, "sqrt");
    my int $pow = sys::dl_sym($libm, "pow");

    # Call sqrt(16.0)
    my num $result = sys::dl_call_num($sqrt, [16.0]);
    say("sqrt(16) = " . $result);  # 4.0

    # Call pow(2.0, 10.0)
    $result = sys::dl_call_num($pow, [2.0, 10.0]);
    say("pow(2, 10) = " . $result);  # 1024.0

    sys::dl_close($libm);
}
```

### Passing Pointers (By Reference)

For C functions that modify their arguments via pointers:

```strada
# Get pointer to a variable
my int $x = 10;
my int $ptr = sys::int_ptr(\$x);

# Call C function that takes int*
sys::dl_call_void($increment_fn, [$ptr]);
# $x is now modified

# For floating point
my num $n = 3.14;
my int $nptr = sys::num_ptr(\$n);
sys::dl_call_void($double_it_fn, [$nptr]);

# Read/write via pointer directly
my int $val = sys::ptr_deref_int($ptr);
sys::ptr_set_int($ptr, 42);
```

### Pointer Functions (sys:: namespace)

| Function | Description |
|----------|-------------|
| `sys::int_ptr(\$var)` | Get pointer to int variable |
| `sys::num_ptr(\$var)` | Get pointer to num variable |
| `sys::str_ptr(\$var)` | Get pointer to string data |
| `sys::ptr_deref_int($ptr)` | Read int from pointer |
| `sys::ptr_deref_num($ptr)` | Read num from pointer |
| `sys::ptr_deref_str($ptr)` | Read string from pointer |
| `sys::ptr_set_int($ptr, $val)` | Write int to pointer |
| `sys::ptr_set_num($ptr, $val)` | Write num to pointer |

### Writing C Libraries for Enhanced FFI

When using the `_sv` FFI functions, your C library receives `StradaValue*` pointers and can use runtime functions to extract values:

```c
// mylib.c - C library using enhanced FFI
#include "strada_runtime.h"

// Function receives StradaValue* arguments
int64_t my_string_length(StradaValue *str_sv) {
    const char *str = strada_to_str(str_sv);
    return strlen(str);
}

// Function with multiple argument types
int64_t my_connect(StradaValue *host_sv, StradaValue *port_sv) {
    const char *host = strada_to_str(host_sv);
    int port = (int)strada_to_int(port_sv);
    // ... connect logic ...
    return 0;
}
```

**Available runtime functions for extracting values:**

| Function | Description |
|----------|-------------|
| `strada_to_str(sv)` | Extract string (const char*) |
| `strada_to_int(sv)` | Extract integer (int64_t) |
| `strada_to_num(sv)` | Extract float (double) |

**Building shared libraries:**

```bash
# Compile with -rdynamic to export runtime symbols
gcc -shared -fPIC -o libmylib.so mylib.c -I/path/to/strada/runtime
```

**Important:** Programs using enhanced FFI must be compiled with `-rdynamic` so that dynamically loaded libraries can access the runtime functions. The `strada` wrapper script does this automatically.

**Using from Strada:**

```strada
my int $lib = sys::dl_open("./libmylib.so");
my int $fn = sys::dl_sym($lib, "my_connect");

# Use _sv variant for StradaValue* functions
my int $result = sys::dl_call_int_sv($fn, ["example.com", 443]);

sys::dl_close($lib);
```

---

## Calling Strada from C

You can compile Strada code to a library and call it from C.

### Step 1: Create Strada Library

```strada
# mylib.strada - Note: no main() function for libraries
func add(int $a, int $b) int {
    return $a + $b;
}

func greet(str $name) str {
    return "Hello, " . $name . "!";
}
```

### Step 2: Compile to C

```bash
./stradac mylib.strada mylib.c
```

### Step 3: Create C Program

```c
#include "strada_runtime.h"
#include <stdio.h>

// Required globals
StradaValue *ARGV = NULL;
StradaValue *ARGC = NULL;

// Declare Strada functions
StradaValue* add(StradaValue* a, StradaValue* b);
StradaValue* greet(StradaValue* name);

int main(int argc, char **argv) {
    // Initialize runtime
    ARGV = strada_new_array();
    ARGC = strada_new_int(argc);

    // Call Strada functions
    StradaValue *sum = add(strada_new_int(5), strada_new_int(3));
    printf("5 + 3 = %ld\n", strada_to_int(sum));

    StradaValue *msg = greet(strada_new_str("World"));
    printf("%s\n", strada_to_str(msg));

    return 0;
}
```

### Step 4: Compile and Link

```bash
gcc -o myprogram main.c mylib.c runtime/strada_runtime.c \
    -Iruntime -ldl -lm
./myprogram
```

See `examples/c_integration/` for a complete working example.

---

## Time Functions

**Note:** Time functions use the `sys::` namespace.

### Basic Time

```strada
my int $now = sys::time();           # Current Unix timestamp
sys::sleep(2);                       # Sleep 2 seconds
sys::usleep(500000);                 # Sleep 500ms (microseconds)
```

### Time Conversion

```strada
my int $ts = sys::time();

# Convert to local time hash
my scalar $lt = sys::localtime($ts);
say("Hour: " . $lt->{"hour"});
say("Day: " . $lt->{"mday"});

# Format as string
my str $formatted = sys::strftime("%Y-%m-%d %H:%M:%S", $lt);
say($formatted);

# Human-readable string
say(sys::ctime($ts));

# Convert back to timestamp
my int $new_ts = sys::mktime($lt);
```

### Time Hash Fields

| Field | Description |
|-------|-------------|
| `sec` | Seconds (0-59) |
| `min` | Minutes (0-59) |
| `hour` | Hours (0-23) |
| `mday` | Day of month (1-31) |
| `mon` | Month (0-11) |
| `year` | Years since 1900 |
| `wday` | Day of week (0=Sunday) |
| `yday` | Day of year (0-365) |
| `isdst` | Daylight saving flag |

### High-Resolution Time

```strada
use Time::HiRes;

# Get time as float with microseconds
my num $t = sys::hires_time();

# Measure elapsed time
my scalar $start = sys::gettimeofday();
# ... do work ...
my scalar $end = sys::gettimeofday();
my num $elapsed = sys::tv_interval($start, $end);
say("Elapsed: " . $elapsed . " seconds");

# Nanosecond sleep
sys::nanosleep(1000000);  # 1ms

# Monotonic clock (for timing)
my scalar $mono = sys::clock_gettime(1);  # CLOCK_MONOTONIC
```

---

## Getting Help

- **Documentation**: See `docs/` directory
- **Examples**: See `examples/` directory
- **Source Code**: The compiler source in `compiler/` is written in Strada itself

### Build Commands

```bash
make                        # Build self-hosting compiler
make run PROG=name          # Compile and run an example
make run-bootstrap PROG=name # Use bootstrap compiler instead
make test                   # Run runtime tests
make test-selfhost          # Test self-compilation
make clean                  # Clean build artifacts
make help                   # Show all targets
```

---

*Strada - Where Perl meets C*

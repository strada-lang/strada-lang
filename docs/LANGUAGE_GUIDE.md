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
19. [Async/Await](#asyncawait)
20. [Packages and Modules](#packages-and-modules)
    - [Object-Oriented Programming](#object-oriented-programming)
    - [Moose-Style Declarative OOP](#moose-style-declarative-oop)
    - [Operator Overloading](#operator-overloading)
21. [The `core::` Namespace](#the-core-namespace)
22. [C Interoperability](#c-interoperability)
23. [Perl Integration](#perl-integration)
24. [Built-in Functions](#built-in-functions)
25. [Best Practices](#best-practices)

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
- **Magic namespaces**: `core::` (preferred) / `core::` for libc functions, `math::` for math functions

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

### Constants

Use `const` to declare compile-time constants:

```strada
const int MAX_SIZE = 100;    # Global const -> C #define
const str VERSION = "1.0.0"; # No runtime overhead
const num PI = 3.14159;      # Sigil is optional for const

func example() void {
    const int LOCAL_LIMIT = 50;  # Local const
    # ...
}
```

Global constants with literal values compile to C `#define` macros with zero runtime overhead. Local constants are regular variables that get initialized once.

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

### Our Variables (Package-Scoped Globals)

Use `our` to declare package-scoped globals backed by the runtime global registry:

```strada
our int $count = 0;
our str $name = "hello";

package Config;
our str $host = "localhost";
our int $port = 8080;

package main;

func show() void {
    say($count);           # Reads from global registry
    $count = $count + 1;   # Writes to global registry
}

func main() int {
    show();                # 0
    show();                # 1
    $count += 10;          # Compound assignment works
    $name .= " world";    # String concat works
    return 0;
}
```

Unlike `my` globals (compiled to C global variables), `our` globals use `strada_global_set/get`, making them accessible across dynamically loaded modules (`import_lib`).

### Local Variables (Dynamic Scoping)

Use `local()` to temporarily override an `our` variable. The original value is automatically restored when the enclosing scope exits, providing dynamic (not lexical) scoping -- just like Perl's `local`:

```strada
our str $greeting = "Hello";

func inner() void {
    say($greeting);  # Sees the dynamically-scoped value
}

func outer() void {
    local $greeting = "Bonjour";
    inner();         # Prints "Bonjour"
}

func main() int {
    inner();         # Prints "Hello"
    outer();         # inner() sees "Bonjour"
    inner();         # Prints "Hello" again (restored)
    return 0;
}
```

Key points about `local()`:
- Only works with `our` variables (not `my` variables)
- The original value is restored on scope exit, even if the scope exits via exception (`throw`/`die`)
- Works correctly with `try`/`catch` blocks
- Useful for temporarily changing global configuration (e.g., debug flags, output settings)

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
| `x` | Repetition | `$str x 3` |

The `x` operator repeats a string a given number of times, just like in Perl:

```strada
my str $line = "-" x 40;       # "----------------------------------------"
my str $abc = "ab" x 3;        # "ababab"

my int $n = 5;
my str $stars = "*" x $n;      # "*****"

# Same precedence as *, /, %
my str $border = "=-" x 20 . "=";
```

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

Both `elsif` and `else if` are supported — they are interchangeable:

```strada
if ($score >= 90) {
    say("A");
} elsif ($score >= 80) {
    say("B");
} else if ($score >= 70) {
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

### Unless and Until

`unless` is the opposite of `if` — it executes when the condition is **false**:

```strada
unless ($logged_in) {
    say("Please log in");
}

# With else (no elsif/else if allowed with unless)
unless ($valid) {
    say("Invalid input");
} else {
    say("Processing...");
}
```

`until` is the opposite of `while` — it loops while the condition is **false**:

```strada
my int $count = 0;
until ($count >= 10) {
    say($count);
    $count = $count + 1;
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

### Redo

`redo` restarts the current loop iteration **without** re-checking the condition or advancing the iterator:

```strada
my int $attempts = 0;
while ($attempts < 3) {
    $attempts = $attempts + 1;
    my str $input = readline();
    if ($input eq "") {
        say("Input required, try again");
        redo;  # Restart this iteration (don't increment $attempts again)
    }
    say("Got: " . $input);
}
```

### Statement Modifiers

Perl-style postfix modifiers let you write concise one-liners:

```strada
# Postfix if/unless
say("verbose output") if $verbose;
say("warning: not ready") unless $ready;

# Postfix while/until
$i = $i + 1 while $i < 10;
$count = $count + 1 until $count >= 5;

# Works with return, last, next, redo
return 0 if $error;
return $value unless $invalid;
last if $done;
next unless $valid;
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

Label names are typically uppercase by convention. You can use labels with `for`, `foreach`, `while`, and `until` loops. All three loop control statements (`last`, `next`, `redo`) support labels:

```strada
SEARCH: while (1) {
    my str $line = readline($fh);
    if (!defined($line)) {
        last SEARCH;
    }

    for (my int $i = 0; $i < size(@patterns); $i = $i + 1) {
        if ($line =~ /$patterns[$i]/) {
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

`fn` is shorthand for `func` and can be used interchangeably everywhere.

```strada
func function_name(type $param1, type $param2) return_type {
    # function body
    return value;
}

# Equivalent using fn shorthand:
fn function_name(type $param1, type $param2) return_type {
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

# Context-sensitive function (like Perl's wantarray)
func flexible() dynamic {
    if (core::wantarray()) {
        my array @r = (1, 2, 3);
        return @r;
    }
    return 42;
}

my array @list = flexible();  # Array context → (1, 2, 3)
my int $val = flexible();     # Scalar context → 42
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

### Variadic Functions

Variadic functions accept a variable number of arguments. The variadic parameter must be the last parameter and uses array sigil `@` with ellipsis prefix `...`:

```strada
# Basic variadic function
func sum(int ...@nums) int {
    my int $total = 0;
    foreach my int $n (@nums) {
        $total = $total + $n;
    }
    return $total;
}

# Call with any number of arguments
say(sum(1, 2, 3));           # 6
say(sum(10, 20, 30, 40));    # 100
say(sum());                  # 0 (empty is valid)
```

#### Fixed Parameters Before Variadic

You can have fixed parameters before the variadic one:

```strada
func format_nums(str $prefix, str $sep, int ...@nums) str {
    my str $result = $prefix;
    my int $first = 1;
    foreach my int $n (@nums) {
        if ($first == 0) { $result = $result . $sep; }
        $result = $result . $n;
        $first = 0;
    }
    return $result;
}

say(format_nums("Values: ", ", ", 1, 2, 3));  # "Values: 1, 2, 3"
```

#### Spread Operator

Use the spread operator `...@array` to unpack an array into individual arguments:

```strada
my array @values = (10, 20, 30);
say(sum(...@values));                    # 60

# Mix individual args and spread
say(sum(1, ...@values, 99));             # 160 (1 + 10 + 20 + 30 + 99)

# Multiple spreads
my array @more = (100, 200);
say(sum(...@values, ...@more));          # 360
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

Strada supports anonymous functions (closures) using the `func` (or `fn`) keyword without a name.
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
    say($items[$i]);
}
```

### Splice

`splice()` removes and/or replaces elements in an array, returning the removed elements. It works like Perl's `splice`:

```strada
my array @arr = (10, 20, 30, 40, 50);

# Remove 2 elements starting at index 1
my array @removed = splice(@arr, 1, 2);
# @removed is (20, 30), @arr is now (10, 40, 50)

# Remove 1 element at index 0 and insert replacements
my array @old = splice(@arr, 0, 1, 100, 200);
# @old is (10), @arr is now (100, 200, 40, 50)

# Insert without removing (length = 0)
splice(@arr, 2, 0, 35);
# @arr is now (100, 200, 35, 40, 50)

# Negative offset counts from the end
splice(@arr, -1, 1);
# Removes the last element; @arr is now (100, 200, 35, 40)
```

### Map, Grep, and Sort

Strada supports Perl-style `map`, `grep`, and `sort` blocks for powerful array transformations.

#### Map

Transform each element of an array using `$_` as the current element:

```strada
my array @nums = (1, 2, 3, 4, 5);

# Double each element
my array @doubled = map { $_ * 2 } @nums;
# Result: (2, 4, 6, 8, 10)

# Transform to strings
my array @strings = map { "Value: " . $_ } @nums;
```

**Note:** The semicolon after the expression in a map block is optional.

#### Map with Fat Arrow (Creating Hashes)

The fat arrow (`=>`) operator in a map block creates key-value pairs. This is the classic Perl idiom for building lookup hashes:

```strada
my array @fruits = ("apple", "banana", "cherry");

# Create a lookup hash - the Perl way!
my hash %lookup = map { $_ => 1 } @fruits;
# %lookup is now {"apple" => 1, "banana" => 1, "cherry" => 1}

# Fast membership testing
if (exists($lookup{"apple"})) {
    say("Found apple!");
}

# Count occurrences
my array @words = ("the", "cat", "sat", "on", "the", "mat");
my hash %seen = ();
foreach my str $w (@words) {
    if (exists($seen{$w})) {
        $seen{$w} = $seen{$w} + 1;
    } else {
        $seen{$w} = 1;
    }
}
```

**How it works:**

1. The fat arrow `$_ => value` creates a 2-element array `[$_, value]` (a "pair")
2. Map flattens these pairs into a single flat array: `("apple", 1, "banana", 1, "cherry", 1)`
3. When assigned to a hash variable, alternating elements become key-value pairs

You can also store the flat array and process it manually:

```strada
my array @pairs = map { $_ => 1 } @fruits;
# @pairs is ("apple", 1, "banana", 1, "cherry", 1)

# Build hash manually
my hash %h = ();
my int $i = 0;
while ($i < scalar(@pairs)) {
    $h{$pairs[$i]} = $pairs[$i + 1];
    $i = $i + 2;
}
```

#### Grep

Filter array elements, keeping only those where the block returns true:

```strada
my array @nums = (1, 2, 3, 4, 5, 6);

# Keep only even numbers
my array @evens = grep { $_ % 2 == 0 } @nums;
# Result: (2, 4, 6)

# Keep values greater than 3
my array @big = grep { $_ > 3 } @nums;
# Result: (4, 5, 6)

# Combine with map
my array @doubled_evens = map { $_ * 2 } grep { $_ % 2 == 0 } @nums;
# Result: (4, 8, 12)
```

#### Sort

Sort arrays with custom comparison using `$a` and `$b`, and the spaceship operator `<=>`:

```strada
my array @nums = (5, 2, 8, 1, 9);

# Sort ascending (numeric)
my array @asc = sort { $a <=> $b } @nums;
# Result: (1, 2, 5, 8, 9)

# Sort descending
my array @desc = sort { $b <=> $a } @nums;
# Result: (9, 8, 5, 2, 1)

# Default sort (no block) - alphabetical
my array @alpha = sort @names;
```

The `<=>` operator returns -1 if left < right, 0 if equal, and 1 if left > right.

#### Chaining Map, Grep, and Sort

These operations can be chained for powerful data transformations:

```strada
my array @data = (5, 2, 8, 1, 9, 3, 7);

# Filter, transform, and sort in one pipeline
my array @result = sort { $a <=> $b } map { $_ * 10 } grep { $_ > 3 } @data;
# Result: (50, 70, 80, 90)

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

### Iterating with `each()`

`each()` returns the next key-value pair from a hash as a two-element array `[key, value]`. When all pairs have been returned, it returns an empty array. The iterator resets automatically after exhaustion:

```strada
my hash %colors = ();
$colors{"red"} = "#FF0000";
$colors{"green"} = "#00FF00";
$colors{"blue"} = "#0000FF";

my array @pair = each(%colors);
while (size(@pair) > 0) {
    say($pair[0] . " => " . $pair[1]);
    @pair = each(%colors);
}
# Prints each key-value pair (order may vary)
```

### Tied Hashes

Use `tie` to bind a hash to a class that implements the TIEHASH interface. This allows you to intercept all hash operations (read, write, delete, iteration) with custom behavior:

```strada
package UpperHash;

func TIEHASH(str $class) scalar {
    my hash %self = ();
    $self{"_data"} = {};
    return bless(\%self, $class);
}

func STORE(scalar $self, str $key, scalar $val) void {
    $self->{"_data"}->{uc($key)} = $val;
}

func FETCH(scalar $self, str $key) scalar {
    return $self->{"_data"}->{uc($key)};
}

func EXISTS(scalar $self, str $key) int {
    return exists($self->{"_data"}->{uc($key)});
}

func DELETE(scalar $self, str $key) void {
    delete($self->{"_data"}->{uc($key)});
}

func FIRSTKEY(scalar $self) scalar {
    my array @k = keys(%{$self->{"_data"}});
    return size(@k) > 0 ? $k[0] : undef;
}

func NEXTKEY(scalar $self, str $lastkey) scalar {
    # Return next key after $lastkey, or undef when done
    my array @k = keys(%{$self->{"_data"}});
    my int $i = 0;
    while ($i < size(@k)) {
        if ($k[$i] eq $lastkey && ($i + 1) < size(@k)) {
            return $k[$i + 1];
        }
        $i++;
    }
    return undef;
}

func CLEAR(scalar $self) void {
    $self->{"_data"} = {};
}

package main;

func main() int {
    my hash %h = ();
    tie(%h, "UpperHash");

    $h{"name"} = "alice";      # STORE called with key uppercased
    say($h{"name"});           # FETCH called -> "alice"
    say(exists($h{"NAME"}));   # EXISTS -> 1

    untie(%h);                 # Remove the tie binding
    return 0;
}
```

The TIEHASH interface methods:

| Method | Called when |
|--------|------------|
| `TIEHASH($class)` | `tie(%hash, "Class")` -- construct the tied object |
| `FETCH($self, $key)` | `$hash{$key}` -- read a value |
| `STORE($self, $key, $val)` | `$hash{$key} = $val` -- write a value |
| `EXISTS($self, $key)` | `exists($hash{$key})` -- check key existence |
| `DELETE($self, $key)` | `delete($hash{$key})` -- remove a key |
| `FIRSTKEY($self)` | `keys(%hash)` / `each(%hash)` -- start iteration |
| `NEXTKEY($self, $lastkey)` | Continue iteration after `$lastkey` |
| `CLEAR($self)` | `%hash = ()` -- clear all entries |

Use `tied(%hash)` to get the underlying tied object, or check if a hash is tied. Use `untie(%hash)` to remove the tie binding.

Zero overhead: untied hashes have no extra dispatch cost thanks to `__builtin_expect` optimization.

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
my str $type = reftype($ref);  # "SCALAR", "ARRAY", or "HASH"
```

### Weak References

Strada uses reference counting for memory management. Circular references (A references B, B references A) create a cycle where neither object can be freed. Weak references solve this by allowing a reference that does not keep its target alive.

```strada
# Create a circular reference
my scalar $parent = { "name" => "parent" };
my scalar $child = { "name" => "child" };
$parent->{"child"} = $child;
$child->{"parent"} = $parent;     # Circular! Memory leak!

# Break the cycle with a weak reference
core::weaken($child->{"parent"}); # Now parent can be freed normally
```

When the target of a weak reference is freed, the weak reference becomes `undef`:

```strada
my scalar $weak;
{
    my scalar $obj = { "data" => "hello" };
    $weak = $obj;
    core::weaken($weak);
    say($weak->{"data"});          # "hello" (still accessible)
}
# $obj freed at scope exit; $weak is now undef
```

| Function | Description |
|----------|-------------|
| `core::weaken($ref)` | Make `$ref` a weak reference (does not hold target alive) |
| `core::isweak($ref)` | Returns 1 if `$ref` is weak, 0 otherwise |

- `core::weaken()` also works on hash entry values: `core::weaken($hash->{"key"})`
- Calling `core::weaken()` on an already-weak reference is a safe no-op
- Multiple weak references to the same target are supported

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

For working with binary data (protocols, file formats, etc.), use the `core::` byte functions:

```strada
# Get bytes from strings (binary-safe, not UTF-8 aware)
my int $byte = core::ord_byte($str);        # First byte (0-255)
my int $b = core::get_byte($str, 5);        # Byte at position
my int $len = core::byte_length($str);      # Byte count
my str $sub = core::byte_substr($str, 0, 4); # Substring by bytes

# Set a byte (returns new string)
my str $modified = core::set_byte($str, 0, 0xFF);
```

#### Pack and Unpack

Use `core::pack()` and `core::unpack()` for binary protocol construction and parsing:

```strada
# Pack values into binary string
my str $header = core::pack("NnC", 0x12345678, 80, 255);
# N = 4-byte big-endian int, n = 2-byte big-endian short, C = 1-byte unsigned

# Unpack binary data to array
my array @values = core::unpack("NnC", $header);
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
    return core::pack("nnnnnn", $id, $flags, 1, 0, 0, 0);
}

func parse_dns_header(str $data) hash {
    my array @fields = core::unpack("nnnnnn", $data);
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

**Note:** File I/O functions use the `core::` namespace.

### Reading Files

```strada
# Read entire file
my str $content = core::slurp("file.txt");

# Read line by line
my scalar $fh = core::open("file.txt", "r");
while (1) {
    my str $line = core::readline($fh);
    if (!defined($line)) {
        last;
    }
    say($line);
}
core::close($fh);
```

### Writing Files

```strada
# Write entire file
core::spew("output.txt", "Hello, World!\n");

# Write with file handle
my scalar $fh = core::open("output.txt", "w");
core::write($fh, "Line 1\n");
core::write($fh, "Line 2\n");
core::close($fh);
```

### In-Memory I/O

Read from and write to strings using standard file handle operations:

```strada
# Read from a string
my scalar $fh = core::open_str("line1\nline2\n", "r");
while (defined(my str $line = <$fh>)) {
    say($line);  # prints "line1" then "line2"
}
core::close($fh);

# Write to a string buffer
my scalar $wfh = core::open_str("", "w");
say($wfh, "hello");
my str $result = core::str_from_fh($wfh);  # "hello\n"
core::close($wfh);

# Reference-style (variable updated on close, like Perl)
my str $output = "";
my scalar $wfh2 = core::open(\$output, "w");
say($wfh2, "data");
core::close($wfh2);
# $output now contains "data\n"
```

### Default Filehandle with `select()`

`select()` sets the default filehandle used by `print` and `say` when no filehandle argument is provided. It returns the previous default filehandle:

```strada
my scalar $logfile = core::open("app.log", "w");
my scalar $old_fh = select($logfile);

say("This goes to app.log");     # Writes to $logfile
print("So does this");           # Writes to $logfile

select($old_fh);                 # Restore stdout as default
say("Back to stdout");
```

Zero overhead when unused -- if `select()` is never called, print/say output to stdout with no extra dispatch.

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

### Capture Groups

After a successful regex match, capture groups are accessible via `$1` through `$9` or the `captures()` function:

```strada
if ("hello world" =~ /(\w+)\s+(\w+)/) {
    say($1);  # "hello"
    say($2);  # "world"
}

# Using captures() for the full match and programmatic access
if ("2024-01-15" =~ /(\d{4})-(\d{2})-(\d{2})/) {
    my array @parts = captures();
    say("Full match: " . $parts[0]);   # "2024-01-15"
    say("Year: " . $1);               # "2024"
    say("Month: " . $2);              # "01"
    say("Day: " . $3);                # "15"
}
```

`$1`-`$9` are syntactic sugar for `captures()[N]`. They return `undef` if the group does not exist. Use `captures()` when you need `$0` (the full match) or programmatic index access.

### The `/e` Modifier (Evaluate Replacement)

The `/e` modifier treats the replacement part of `s///` as an expression to evaluate, rather than a literal string. This is powerful for computed replacements:

```strada
my str $text = "width=100 height=200";

# Double all numbers
$text =~ s/(\d+)/$1 * 2/eg;
say($text);  # "width=200 height=400"

# Use $1 in the expression
my str $csv = "Alice,30,NYC";
$csv =~ s/([^,]+)/uc($1)/eg;
say($csv);  # "ALICE,30,NYC"
```

The `/e` modifier works with `/g` for global replacement. Both `$1`-`$9` and the `captures()` function are available within the replacement expression.

### Transliteration (`tr///` / `y///`)

The `tr///` operator (and its synonym `y///`) performs character-by-character transliteration. Unlike `s///`, it does not use regex -- it translates individual characters:

```strada
my str $text = "Hello, World!";

# Translate lowercase to uppercase
$text =~ tr/a-z/A-Z/;
say($text);  # "HELLO, WORLD!"

# ROT13 encoding
my str $msg = "Hello";
$msg =~ tr/A-Za-z/N-ZA-Mn-za-m/;
say($msg);  # "Uryyb"

# Count characters (tr returns the count of transliterations)
my str $str = "Hello World";
my int $vowels = ($str =~ tr/aeiouAEIOU//);
say("Vowels: " . $vowels);  # 3
```

**Flags:**

| Flag | Description |
|------|-------------|
| `c` | Complement the search list (translate characters NOT in the list) |
| `d` | Delete characters found in search list that have no replacement |
| `s` | Squeeze duplicate replaced characters into a single character |
| `r` | Return a modified copy instead of modifying in place |

```strada
# Delete digits (d flag)
my str $clean = "abc123def456";
$clean =~ tr/0-9//d;
say($clean);  # "abcdef"

# Squeeze repeated spaces (s flag)
my str $spaced = "too    many    spaces";
$spaced =~ tr/ / /s;
say($spaced);  # "too many spaces"

# Complement: delete everything that's NOT a letter (cd flags)
my str $letters = "H3ll0 W0rld!";
$letters =~ tr/a-zA-Z//cd;
say($letters);  # "HllWrld"

# Return copy without modifying original (r flag)
my str $original = "hello";
my str $upper = ($original =~ tr/a-z/A-Z/r);
say($original);  # "hello" (unchanged)
say($upper);     # "HELLO"
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

**Note:** Process control functions use the `core::` namespace.

### Sleeping

```strada
# Sleep for 2 seconds
core::sleep(2);

# Sleep for 500 milliseconds (500,000 microseconds)
core::usleep(500000);
```

### Forking Processes

```strada
my int $pid = core::fork();

if ($pid == 0) {
    # Child process
    say("I am the child, PID: " . core::getpid());
    core::exit(0);
} elsif ($pid > 0) {
    # Parent process
    say("I am the parent, child PID: " . $pid);
    core::wait();  # Wait for child to finish
    say("Child finished");
} else {
    # Fork failed
    die("Fork failed");
}
```

### Process Information

```strada
# Get current process ID
my int $pid = core::getpid();

# Get parent process ID
my int $ppid = core::getppid();
```

### Waiting for Child Processes

```strada
# Wait for any child
core::wait();

# Wait for specific child (blocking)
core::waitpid($child_pid, 0);

# Non-blocking wait (check without blocking)
# my int $result = core::waitpid($child_pid, 1);  # WNOHANG
```

### Executing Commands

```strada
# Run command via shell and get exit status (like Perl's system())
my int $status = core::system("ls -la");
say("Exit status: " . $status);

# Run command with shell interpretation
core::system("echo 'Hello' && echo 'World'");

# Replace current process with new command (does not return on success)
core::exec("top");
say("This is never reached if exec succeeds");
```

### Process Name and Title

You can change how your process appears in `ps` output:

```strada
# Get/set process name (comm column in ps, max 15 chars)
my str $name = core::getprocname();
core::setprocname("myworker");

# Get/set process title (args column in ps)
my str $title = core::getproctitle();
core::setproctitle("myapp: processing job 42");
```

Example showing the difference:

```strada
func main() int {
    say("Original name: " . core::getprocname());
    say("Original title: " . core::getproctitle());

    core::setprocname("worker");
    core::setproctitle("worker: idle");

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
    core::exit(0);
}

func main() int {
    # Register handler for SIGINT (Ctrl+C)
    core::signal("INT", \&sigint_handler);

    # Ignore SIGPIPE (useful for network code)
    core::signal("PIPE", "IGNORE");

    # Restore default behavior for a signal
    core::signal("TERM", "DEFAULT");

    say("Waiting for signals...");
    while (1) {
        core::sleep(1);
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

**Note:** IPC functions use the `core::` namespace.

### Creating Pipes

```strada
# Create a pipe - returns array with [read_fd, write_fd]
my scalar $pipe = core::pipe();

if (!defined($pipe)) {
    die("Failed to create pipe");
}

my int $read_fd = $pipe->[0];
my int $write_fd = $pipe->[1];
```

### Pipe Communication Example

```strada
my scalar $pipe = core::pipe();
my int $pid = core::fork();

if ($pid == 0) {
    # Child: close read end, write to pipe
    core::close_fd($pipe->[0]);
    core::write_fd($pipe->[1], "Hello from child!\n");
    core::close_fd($pipe->[1]);
    core::exit(0);
} else {
    # Parent: close write end, read from pipe
    core::close_fd($pipe->[1]);
    my str $message = core::read_all_fd($pipe->[0]);
    core::close_fd($pipe->[0]);
    core::wait();
    say("Received: " . $message);
}
```

### File Descriptor Operations

```strada
# Read up to N bytes from file descriptor
my str $data = core::read_fd($fd, 1024);

# Read all available data until EOF
my str $all = core::read_all_fd($fd);

# Write data to file descriptor
my int $bytes = core::write_fd($fd, "Hello\n");

# Close file descriptor
core::close_fd($fd);

# Duplicate file descriptor (like dup2)
core::dup2($old_fd, $new_fd);
```

### Redirecting I/O with dup2

```strada
# Redirect stdout to a pipe
my scalar $pipe = core::pipe();
my int $saved_stdout = 1;  # Save for later if needed

# In child process:
core::close_fd($pipe->[0]);  # Close read end
core::dup2($pipe->[1], 1);   # Redirect stdout to pipe write end
core::close_fd($pipe->[1]);  # Close original write fd

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
core::write_fd($proc->{"stdin"}, "Line 1\nLine 2\n");
core::close_fd($proc->{"stdin"});
my str $output = core::read_all_fd($proc->{"stdout"});
core::close_fd($proc->{"stdout"});
core::close_fd($proc->{"stderr"});
core::wait();
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
core::usleep(10000);  # Simulate work
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

## Async/Await

Strada provides first-class async/await support with a thread pool backend, offering a higher-level abstraction than raw threads.

### Defining Async Functions

Use `async func` to define functions that run asynchronously:

```strada
async func fetch_data(str $url) str {
    # This runs in the thread pool
    my str $response = http_get($url);
    return $response;
}
```

When called, async functions immediately return a Future while the work executes on a background thread.

### Awaiting Futures

Use `await` to block until a Future completes:

```strada
my scalar $future = fetch_data("http://example.com");
say("Request started...");
my str $result = await $future;  # Blocks until complete
say("Got: " . $result);
```

### Parallel Execution

Launch multiple async operations concurrently:

```strada
async func compute(int $n) int {
    core::usleep(50000);  # 50ms work
    return $n * 2;
}

func main() int {
    # Start three operations in parallel
    my scalar $a = compute(10);
    my scalar $b = compute(20);
    my scalar $c = compute(30);

    # Wait for all results (total time ~50ms, not 150ms)
    my int $r1 = await $a;  # 20
    my int $r2 = await $b;  # 40
    my int $r3 = await $c;  # 60

    return 0;
}
```

### async:: Namespace Functions

| Function | Description |
|----------|-------------|
| `async::all(\@futures)` | Wait for all futures, return array of results |
| `async::race(\@futures)` | Wait for first to complete, cancel others |
| `async::timeout($future, $ms)` | Await with timeout (throws on timeout) |
| `async::cancel($future)` | Request cancellation |
| `async::is_done($future)` | Non-blocking completion check |
| `async::is_cancelled($future)` | Check if cancelled |
| `async::pool_init($workers)` | Initialize thread pool with N workers |
| `async::pool_shutdown()` | Shutdown thread pool |

### Wait for All (async::all)

```strada
my array @futures = (compute(1), compute(2), compute(3));
my array @results = async::all(\@futures);  # [2, 4, 6]
```

### Race (async::race)

Wait for the first future to complete:

```strada
async func slow_task(int $id, int $delay_ms) str {
    core::usleep($delay_ms * 1000);
    return "task " . $id;
}

my array @futures = (
    slow_task(1, 100),   # 100ms
    slow_task(2, 50),    # 50ms - wins
    slow_task(3, 150)    # 150ms
);
my str $winner = async::race(\@futures);  # "task 2"
```

### Timeout

```strada
my scalar $slow = slow_task(99, 500);  # 500ms
try {
    my str $r = async::timeout($slow, 100);  # 100ms timeout
    say("Got: " . $r);
} catch ($e) {
    say("Timed out: " . $e);
}
```

### Cancellation

```strada
my scalar $future = slow_task(1, 1000);
async::cancel($future);

if (async::is_cancelled($future)) {
    say("Cancelled!");
}

try {
    await $future;  # Throws "Future was cancelled"
} catch ($e) {
    say("Caught: " . $e);
}
```

### Error Propagation

Exceptions in async functions propagate through await:

```strada
async func fail_async() int {
    throw "async error";
}

try {
    my int $x = await fail_async();
} catch ($e) {
    say("Caught: " . $e);  # "Caught: async error"
}
```

### Thread Pool Configuration

The thread pool auto-initializes with 4 workers on first async call. For custom configuration:

```strada
func main() int {
    async::pool_init(8);  # 8 worker threads

    # ... async operations ...

    async::pool_shutdown();  # Optional cleanup
    return 0;
}
```

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

func main() int {
    my scalar $animal = new("Buddy");
    say(blessed($animal));  # "Animal"
    speak($animal);
    return 0;
}
```

#### The `__PACKAGE__` Keyword

`__PACKAGE__` returns the current package name as a string at runtime:

```strada
package My::App;

func main() int {
    say(__PACKAGE__);  # "My::App"
    return 0;
}
```

#### Calling Functions in the Current Package

Use `::func()` to call a function in the current package without repeating the package name.
This is resolved at **compile time**, not runtime:

```strada
package Calculator;

func add(int $a, int $b) int {
    return $a + $b;
}

func multiply(int $a, int $b) int {
    return $a * $b;
}

func compute(int $x, int $y) int {
    # These all call Calculator_add and Calculator_multiply
    my int $sum = ::add($x, $y);           # Preferred shorthand
    my int $prod = ::multiply($x, $y);
    return $sum + $prod;
}
```

Three equivalent syntaxes are supported:
- `::func()` — Preferred shorthand
- `.::func()` — Alternate shorthand
- `__PACKAGE__::func()` — Explicit form

All three resolve to `PackageName_func()` at compile time.

**Note:** `__PACKAGE__` alone (without `::`) returns the package name as a string at runtime,
while `::func()` and `__PACKAGE__::func()` are compile-time function resolution.

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
| `Package_AUTOLOAD($self, $method, ...@args)` | Fallback for undefined method calls |
| `$obj->$method()` | Dynamic method dispatch (method name from variable) |
| `__PACKAGE__` | Returns current package name as string |

#### Complete OOP Example

See `examples/test_oop_better.strada` and `examples/test_multi_inherit.strada` for complete examples demonstrating:
- Base classes and constructors
- Single and multi-level inheritance
- Multiple inheritance (mixin pattern)
- Type checking with `isa()` and `can()`
- SUPER:: calls to parent methods
- Package management

### Moose-Style Declarative OOP

In addition to the manual bless-based OOP described above, Strada supports a declarative OOP system inspired by Perl's Moose. This provides a higher-level syntax for defining classes with attributes, inheritance, roles, and method modifiers.

#### Attribute Declarations with `has`

Use `has` to declare attributes on a class. This automatically generates accessor methods and an auto-generated constructor.

```strada
package Person;

has ro str $name (required);       # Read-only, must be provided in new()
has rw int $age = 0;               # Read/write, defaults to 0
has rw str $email;                  # Read/write, no default (undef)
```

**Access modifiers:**
- `ro` (read-only, the default) -- generates a getter: `$obj->name()`
- `rw` (read/write) -- generates a getter and a setter: `$obj->age()`, `$obj->set_age(25)`

**Options (in parentheses after the declaration):**
- `required` -- the attribute must be provided when calling the constructor
- `lazy` -- the default value is not computed until the first access
- `builder => "method_name"` -- calls a named method to compute the default (used with `lazy`)

```strada
package Config;

has ro str $db_url (lazy, builder => "_build_db_url");

func _build_db_url(scalar $self) str {
    return "postgres://localhost/mydb";
}
```

#### Auto-Generated Constructor

When a package uses `has`, a constructor `Package::new(...)` is automatically generated (unless you define your own `new`). It takes named arguments as alternating key-value pairs:

```strada
package main;

my scalar $p = Person::new("name", "Alice", "age", 30);
say($p->name());      # "Alice"
say($p->age());       # 30
$p->set_age(31);      # Uses rw setter
say($p->age());       # 31
```

If an attribute has a default value and is not provided in the constructor, the default is used. If an attribute is `required` and not provided, it will be `undef`.

#### Inheritance with `extends`

Use `extends` (an alias for `inherit`) to set up class inheritance. Child classes inherit all attributes from their parents, and the auto-generated constructor includes parent attributes:

```strada
package Animal;
has ro str $species (required);
has rw int $energy = 100;

func speak(scalar $self) void {
    say($self->species() . " (energy: " . $self->energy() . ")");
}

package Dog;
extends Animal;

has ro str $name (required);
has rw int $age = 0;

func bark(scalar $self) void {
    say($self->name() . " barks!");
}

package main;

func main() int {
    # Constructor includes both Dog and Animal attrs
    my scalar $d = Dog::new("name", "Rex", "species", "dog", "age", 3);
    say($d->name());        # "Rex"
    say($d->species());     # "dog" (inherited from Animal)
    $d->speak();            # "dog (energy: 100)"
    $d->bark();             # "Rex barks!"
    say($d->isa("Dog"));    # 1
    say($d->isa("Animal")); # 1
    return 0;
}
```

#### Role Composition with `with`

Use `with` to compose roles into a class. In Strada, `with` is an alias for `inherit`, so roles work the same as parent classes:

```strada
package Auditable;
has rw str $audit_log;

func log_action(scalar $self, str $action) void {
    $self->set_audit_log($self->audit_log() . $action . "\n");
}

package Account;
extends Person;
with Auditable;           # Compose the Auditable role

has rw int $balance = 0;
```

#### Method Modifiers: `before`, `after`, `around`

Method modifiers let you add behavior around existing methods without modifying them directly. This is especially useful for cross-cutting concerns like logging, validation, or auditing.

**`before` -- runs before the named method:**

```strada
package Dog;
extends Animal;

before "bark" func(scalar $self) void {
    say("[preparing to bark]");
}

func bark(scalar $self) void {
    say($self->name() . " barks!");
}
```

When `$dog->bark()` is called, the output is:
```
[preparing to bark]
Rex barks!
```

**`after` -- runs after the named method:**

```strada
after "bark" func(scalar $self) void {
    say("[done barking]");
}
```

Now `$dog->bark()` produces:
```
[preparing to bark]
Rex barks!
[done barking]
```

**`around` -- wraps the method, receives the original as a callable:**

```strada
around "validate" func(scalar $self, scalar $orig, scalar ...@args) scalar {
    say("pre-validation");
    my scalar $result = $orig->($self, ...@args);   # Call the original method
    say("post-validation");
    return $result;
}
```

The `$orig` parameter is a reference to the original method. You must call it explicitly to execute the wrapped method. This gives you full control over whether and how the original method is invoked.

#### Complete Moose-Style Example

```strada
package Animal;
has ro str $species (required);
has rw int $energy = 100;

func speak(scalar $self) void {
    say($self->species() . " (energy: " . $self->energy() . ")");
}

package Dog;
extends Animal;

has ro str $name (required);
has rw int $age = 0;
has rw str $nickname;

before "bark" func(scalar $self) void {
    say("[preparing to bark]");
}

func bark(scalar $self) void {
    say($self->name() . " barks!");
}

after "bark" func(scalar $self) void {
    say("[done barking]");
}

package main;

func main() int {
    my scalar $d = Dog::new("name", "Rex", "species", "dog", "age", 3);
    say($d->name());        # Rex
    say($d->age());         # 3
    $d->set_age(4);
    say($d->age());         # 4
    say($d->energy());      # 100
    $d->set_energy(80);
    say($d->energy());      # 80
    $d->speak();            # dog (energy: 80)
    $d->bark();             # [preparing to bark] / Rex barks! / [done barking]
    say($d->isa("Dog"));    # 1
    say($d->isa("Animal")); # 1
    return 0;
}
```

See `examples/test_moose.strada` for this complete working example.

### Operator Overloading

Strada supports Perl-style operator overloading via `use overload`. This allows classes to define custom behavior for built-in operators like `+`, `-`, `*`, `""`, `==`, etc.

#### Basic Usage

Declare overloads inside your package with `use overload`, mapping operator strings to method names:

```strada
package Vector;

func new(num $x, num $y) scalar {
    my hash %self = ();
    $self{"x"} = $x;
    $self{"y"} = $y;
    return bless(\%self, "Vector");
}

func add(scalar $self, scalar $other, int $reversed) scalar {
    return Vector::new($self->{"x"} + $other->{"x"},
                       $self->{"y"} + $other->{"y"});
}

func to_str(scalar $self) str {
    return "(" . $self->{"x"} . ", " . $self->{"y"} . ")";
}

use overload
    "+" => "add",
    '""' => "to_str";

package main;

func main() int {
    my scalar $a = Vector::new(1.0, 2.0);
    my scalar $b = Vector::new(3.0, 4.0);

    my scalar $c = $a + $b;        # Calls Vector::add
    say("Result: " . $c);          # Calls Vector::to_str via "" overload

    return 0;
}
```

#### Supported Operators

| Category | Operators |
|----------|-----------|
| Arithmetic | `+`, `-`, `*`, `/`, `%`, `**` |
| String | `.` (concatenation) |
| Stringify | `""` (automatic string conversion) |
| Unary | `neg` (unary minus), `!`, `bool` |
| Numeric comparison | `==`, `!=`, `<`, `>`, `<=`, `>=`, `<=>` |
| String comparison | `eq`, `ne`, `lt`, `gt`, `le`, `ge`, `cmp` |

#### Handler Signatures

**Binary operators** (`+`, `-`, `*`, `/`, `==`, etc.) take three parameters:

```strada
func add(scalar $self, scalar $other, int $reversed) scalar {
    # $self     — the blessed object (always the one with the overload)
    # $other    — the other operand
    # $reversed — 1 if $self was the RIGHT operand
    if ($reversed == 1) {
        return Vector::new($other->{"x"} + $self->{"x"},
                           $other->{"y"} + $self->{"y"});
    }
    return Vector::new($self->{"x"} + $other->{"x"},
                       $self->{"y"} + $other->{"y"});
}
```

**Unary operators** (`neg`, `!`) take only `$self`:

```strada
func negate(scalar $self) scalar {
    return Vector::new(-$self->{"x"}, -$self->{"y"});
}

use overload "neg" => "negate";

# Usage: my scalar $neg = -$v;
```

**Stringify** (`""`) is called automatically when the object is used in string context:

```strada
func to_str(scalar $self) str {
    return "(" . $self->{"x"} . ", " . $self->{"y"} . ")";
}

use overload '""' => "to_str";

# Usage: say("Vector: " . $v);  # Automatically calls to_str
```

#### Dispatch Rules

1. The **left** operand is checked first — if it's blessed and its package has the operator overloaded, that handler is called with `$reversed = 0`
2. If the left operand isn't overloaded, the **right** operand is checked — if blessed with the operator overloaded, that handler is called with `$reversed = 1`
3. If neither operand is overloaded, the default behavior applies

#### Zero Overhead

Operator overloading has **zero overhead** when not used:

- If no `use overload` appears anywhere in the program, the generated C code is identical to code without overloading support
- When overloads exist but operands are typed (`int`, `num`, `str`), inline C is generated directly — no dispatch checks
- Runtime dispatch only occurs when the operator IS overloaded AND at least one operand is a `scalar` (which could be a blessed object)

---

## The `core::` Namespace

The `core::` namespace is the preferred alias for `core::`. All system/libc functions available under `core::` can also be called using `core::`:

```strada
# These are equivalent:
my str $home = core::getenv("HOME");    # Preferred
my str $home = core::getenv("HOME");     # Also works

my int $pid = core::getpid();
my str $cwd = core::getcwd();
core::exit(0);
```

At compile time, `core::` is normalized to `core::` with zero runtime overhead. The `core::` prefix continues to work for backwards compatibility.

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
| `splice(@arr, $off, $len, ...)` | Remove/replace elements |
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
| `each(%hash)` | Next key-value pair |
| `tie(%hash, "Class")` | Bind hash to class |
| `untie(%hash)` | Remove tie binding |
| `tied(%hash)` | Get tied object |

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

### File Functions (core:: namespace)

| Function | Description |
|----------|-------------|
| `core::open($path, $mode)` | Open file |
| `core::close($fh)` | Close file |
| `core::readline($fh)` | Read line |
| `core::slurp($path)` | Read entire file |
| `core::spew($path, $data)` | Write entire file |
| `select($fh)` | Set default filehandle |

### Regex Functions

| Function | Description |
|----------|-------------|
| `match($str, $pattern)` | Test match |
| `replace($str, $pat, $rep)` | Replace first |
| `replace_all($str, $pat, $rep)` | Replace all |
| `split($pat, $str)` | Split string |
| `capture($str, $pat)` | Extract captures |
| `captures()` | Get captures from last `=~` match |
| `$1`-`$9` | Capture group variables (sugar for `captures()[N]`) |

### Utility Functions

| Function | Description |
|----------|-------------|
| `dumper($val)` | Debug dump value |
| `clone($val)` | Deep copy |
| `exit($code)` | Exit program |

### Process Control (core:: namespace)

| Function | Description |
|----------|-------------|
| `core::sleep($seconds)` | Sleep for seconds |
| `core::usleep($microseconds)` | Sleep for microseconds |
| `core::fork()` | Fork process |
| `core::wait()` | Wait for any child |
| `core::waitpid($pid, $opts)` | Wait for specific child |
| `core::getpid()` | Get process ID |
| `core::getppid()` | Get parent process ID |
| `core::system($cmd)` | Run shell command |
| `core::exec($cmd)` | Replace process with command |
| `core::setprocname($name)` | Set process name |
| `core::getprocname()` | Get process name |
| `core::setproctitle($title)` | Set process title |
| `core::getproctitle()` | Get process title |
| `core::signal($name, $handler)` | Set signal handler |

### Inter-Process Communication (core:: namespace)

| Function | Description |
|----------|-------------|
| `core::pipe()` | Create pipe, returns [read, write] |
| `core::dup2($old, $new)` | Duplicate file descriptor |
| `core::close_fd($fd)` | Close file descriptor |
| `core::read_fd($fd, $size)` | Read from file descriptor |
| `core::write_fd($fd, $data)` | Write to file descriptor |
| `core::read_all_fd($fd)` | Read all until EOF |

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
        say("  - " . $colors[$i]);
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

**Note:** FFI functions use the `core::` namespace.

### Loading Libraries

```strada
# Load a shared library
my int $lib = core::dl_open("libm.so.6");
if ($lib == 0) {
    say("Error: " . core::dl_error());
    exit(1);
}

# Get a function pointer
my int $sqrt_fn = core::dl_sym($lib, "sqrt");

# Close when done
core::dl_close($lib);
```

### Calling Foreign Functions

There are two sets of FFI call functions:

**Basic FFI** - Arguments are converted to C primitive types (int64_t, double):

```strada
# Call function returning int (up to 5 args)
my int $result = core::dl_call_int($fn, [arg1, arg2, ...]);

# Call function returning double (up to 2 args)
my num $result = core::dl_call_num($fn, [arg1, arg2]);

# Call function returning string
my str $result = core::dl_call_str($fn, $arg);

# Call void function
core::dl_call_void($fn, [arg1, arg2, ...]);
```

**Enhanced FFI (_sv variants)** - Arguments are passed as `StradaValue*` pointers, allowing C code to extract values with full type information:

```strada
# Call function that takes StradaValue* args and returns int
my int $result = core::dl_call_int_sv($fn, [arg1, arg2, ...]);

# Call function that takes StradaValue* args and returns string
my str $result = core::dl_call_str_sv($fn, [arg1, arg2, ...]);

# Call void function that takes StradaValue* args
core::dl_call_void_sv($fn, [arg1, arg2, ...]);
```

Use the `_sv` variants when:
- Your C function needs to handle multiple types (strings, ints, etc.)
- You're writing a Strada-specific C library wrapper
- The basic FFI misinterprets your argument types (e.g., strings as integers)

### Complete FFI Example

```strada
func main() int {
    # Load math library
    my int $libm = core::dl_open("libm.so.6");

    # Get sqrt function
    my int $sqrt = core::dl_sym($libm, "sqrt");
    my int $pow = core::dl_sym($libm, "pow");

    # Call sqrt(16.0)
    my num $result = core::dl_call_num($sqrt, [16.0]);
    say("sqrt(16) = " . $result);  # 4.0

    # Call pow(2.0, 10.0)
    $result = core::dl_call_num($pow, [2.0, 10.0]);
    say("pow(2, 10) = " . $result);  # 1024.0

    core::dl_close($libm);
    return 0;
}
```

### Passing Pointers (By Reference)

For C functions that modify their arguments via pointers:

```strada
# Get pointer to a variable
my int $x = 10;
my int $ptr = core::int_ptr(\$x);

# Call C function that takes int*
core::dl_call_void($increment_fn, [$ptr]);
# $x is now modified

# For floating point
my num $n = 3.14;
my int $nptr = core::num_ptr(\$n);
core::dl_call_void($double_it_fn, [$nptr]);

# Read/write via pointer directly
my int $val = core::ptr_deref_int($ptr);
core::ptr_set_int($ptr, 42);
```

### Pointer Functions (core:: namespace)

| Function | Description |
|----------|-------------|
| `core::int_ptr(\$var)` | Get pointer to int variable |
| `core::num_ptr(\$var)` | Get pointer to num variable |
| `core::str_ptr(\$var)` | Get pointer to string data |
| `core::ptr_deref_int($ptr)` | Read int from pointer |
| `core::ptr_deref_num($ptr)` | Read num from pointer |
| `core::ptr_deref_str($ptr)` | Read string from pointer |
| `core::ptr_set_int($ptr, $val)` | Write int to pointer |
| `core::ptr_set_num($ptr, $val)` | Write num to pointer |

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
my int $lib = core::dl_open("./libmylib.so");
my int $fn = core::dl_sym($lib, "my_connect");

# Use _sv variant for StradaValue* functions
my int $result = core::dl_call_int_sv($fn, ["example.com", 443]);

core::dl_close($lib);
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

**Note:** Time functions use the `core::` namespace.

### Basic Time

```strada
my int $now = core::time();           # Current Unix timestamp
core::sleep(2);                       # Sleep 2 seconds
core::usleep(500000);                 # Sleep 500ms (microseconds)
```

### Time Conversion

```strada
my int $ts = core::time();

# Convert to local time hash
my scalar $lt = core::localtime($ts);
say("Hour: " . $lt->{"hour"});
say("Day: " . $lt->{"mday"});

# Format as string
my str $formatted = core::strftime("%Y-%m-%d %H:%M:%S", $lt);
say($formatted);

# Human-readable string
say(core::ctime($ts));

# Convert back to timestamp
my int $new_ts = core::mktime($lt);
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
my num $t = core::hires_time();

# Measure elapsed time
my scalar $start = core::gettimeofday();
# ... do work ...
my scalar $end = core::gettimeofday();
my num $elapsed = core::tv_interval($start, $end);
say("Elapsed: " . $elapsed . " seconds");

# Nanosecond sleep
core::nanosleep(1000000);  # 1ms

# Monotonic clock (for timing)
my scalar $mono = core::clock_gettime(1);  # CLOCK_MONOTONIC
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

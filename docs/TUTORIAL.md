# Strada Tutorial

Welcome to Strada! This tutorial will teach you the language from the ground up. By the end, you'll be writing real programs.

## Table of Contents

1. [Your First Program](#1-your-first-program)
2. [Variables and Types](#2-variables-and-types)
3. [Operators](#3-operators)
4. [Control Flow](#4-control-flow)
5. [Functions](#5-functions)
6. [Arrays](#6-arrays)
7. [Hashes](#7-hashes)
8. [References](#8-references)
9. [Strings](#9-strings)
10. [Regular Expressions](#10-regular-expressions)
11. [File I/O](#11-file-io)
12. [Error Handling](#12-error-handling)
13. [Object-Oriented Programming](#13-object-oriented-programming)
14. [Closures](#14-closures)
15. [Multithreading](#15-multithreading)
16. [Next Steps](#16-next-steps)

---

## 1. Your First Program

Create a file called `hello.strada`:

```strada
func main() int {
    say("Hello, World!");
    return 0;
}
```

Compile and run it:

```bash
./strada -r hello.strada
```

Output:
```
Hello, World!
```

### Understanding the Code

- `func main() int` - Every program needs a `main` function. The `int` means it returns an integer.
- `say("Hello, World!")` - Prints text with a newline (like Perl's `say`)
- `return 0` - Returns 0 to indicate success

### Compilation Options

```bash
./strada hello.strada      # Creates executable ./hello
./strada -r hello.strada   # Compile and run immediately
./strada -c hello.strada   # Keep the generated C code
```

---

## 2. Variables and Types

Strada is statically typed. Every variable must have a declared type.

### Declaring Variables

```strada
func main() int {
    # Scalars (single values)
    my int $count = 42;
    my num $price = 19.99;
    my str $name = "Alice";

    # The generic scalar type can hold any value
    my scalar $anything = 123;
    $anything = "now a string";

    say("Count: " . $count);
    say("Price: " . $price);
    say("Name: " . $name);

    return 0;
}
```

### Type Summary

| Type | Description | Example |
|------|-------------|---------|
| `int` | 64-bit integer | `42`, `-17`, `0` |
| `num` | 64-bit float | `3.14`, `-0.5` |
| `str` | String | `"hello"`, `'world'` |
| `scalar` | Any single value | Can hold int, num, or str |
| `array` | List of values | `@numbers` |
| `hash` | Key-value pairs | `%data` |
| `void` | No value | For function returns |

### Sigils

Strada uses Perl-style sigils:

- `$` for scalars (single values)
- `@` for arrays
- `%` for hashes

```strada
my int $number = 10;       # Scalar
my array @items = ();      # Array
my hash %config = ();      # Hash
```

### Package-Scoped Variables with `our`

For variables shared across modules, use `our`:

```strada
our int $counter = 0;       # Backed by global registry
our str $app_name = "demo"; # Key: "main::app_name"

package Config;
our str $host = "localhost"; # Key: "Config::host"

package main;

func bump() void {
    $counter += 1;          # Compound assignment works
}

func main() int {
    bump();
    bump();
    say($counter);          # 2
    return 0;
}
```

### Dynamic Scoping with `local`

When you need to temporarily override an `our` variable and have it automatically restored when leaving the current scope, use `local`:

```strada
our str $log_level = "info";

func debug_section() void {
    local($log_level) = "debug";   # Override for this scope
    say("Level: " . $log_level);   # "debug"
    do_work();                      # Called functions see "debug" too
}   # $log_level automatically restored to "info" here

func main() int {
    say("Level: " . $log_level);   # "info"
    debug_section();
    say("Level: " . $log_level);   # "info" (restored)
    return 0;
}
```

The key difference from `my` is that `local` affects the variable for all functions called within the scope, not just the current block. This is called "dynamic scoping."

### The `undef` Value

Variables can be undefined:

```strada
my scalar $value = undef;

if (defined($value)) {
    say("Has a value");
} else {
    say("Is undefined");
}
```

---

## 3. Operators

### Arithmetic Operators

```strada
my int $a = 10;
my int $b = 3;

say($a + $b);    # 13 (addition)
say($a - $b);    # 7  (subtraction)
say($a * $b);    # 30 (multiplication)
say($a / $b);    # 3  (integer division)
say($a % $b);    # 1  (modulo)
```

### String Concatenation

Use `.` to join strings:

```strada
my str $first = "Hello";
my str $last = "World";
my str $full = $first . ", " . $last . "!";
say($full);  # Hello, World!
```

### Comparison Operators

**Numeric comparison:**

```strada
if ($a == $b) { }   # Equal
if ($a != $b) { }   # Not equal
if ($a < $b) { }    # Less than
if ($a > $b) { }    # Greater than
if ($a <= $b) { }   # Less or equal
if ($a >= $b) { }   # Greater or equal
```

**String comparison:**

```strada
if ($s1 eq $s2) { }  # Equal
if ($s1 ne $s2) { }  # Not equal
if ($s1 lt $s2) { }  # Less than
if ($s1 gt $s2) { }  # Greater than
```

**Spaceship operator (comparison):**

```strada
my int $cmp = $a <=> $b;  # -1 if a<b, 0 if a==b, 1 if a>b
```

### Logical Operators

```strada
if ($a && $b) { }   # AND
if ($a || $b) { }   # OR
if (!$a) { }        # NOT
```

### Assignment Operators

```strada
$x = 10;      # Assign
$x += 5;      # Add and assign ($x = $x + 5)
$x -= 3;      # Subtract and assign
$x *= 2;      # Multiply and assign
$x /= 4;      # Divide and assign
$s .= "!";    # Concatenate and assign
```

### String Repetition Operator

The `x` operator repeats a string a given number of times:

```strada
my str $line = "-" x 40;       # 40 dashes: "----------------------------------------"
my str $abc = "abc" x 3;      # "abcabcabc"
say("Ha" x 5);                # "HaHaHaHaHa"
```

This is handy for creating separators, padding, or repeated patterns.

### Ternary Operator

```strada
my str $status = ($age >= 18) ? "adult" : "minor";
```

---

## 4. Control Flow

### If/Elsif/Else

Both `elsif` and `else if` are supported — use whichever you prefer:

```strada
my int $score = 85;

if ($score >= 90) {
    say("A grade");
} elsif ($score >= 80) {
    say("B grade");
} elsif ($score >= 70) {
    say("C grade");
} else {
    say("Needs improvement");
}
```

### While Loop

```strada
my int $i = 0;
while ($i < 5) {
    say("i = " . $i);
    $i = $i + 1;
}
```

### For Loop

```strada
for (my int $i = 0; $i < 5; $i = $i + 1) {
    say("i = " . $i);
}
```

### Foreach Loop

Iterate over arrays:

```strada
my array @fruits = ("apple", "banana", "cherry");

foreach my str $fruit (@fruits) {
    say("I like " . $fruit);
}
```

### Unless and Until

`unless` is the negated form of `if`:

```strada
unless ($logged_in) {
    say("Please log in");
}
```

`until` is the negated form of `while`:

```strada
my int $count = 0;
until ($count >= 10) {
    $count = $count + 1;
}
```

### Loop Control

```strada
# last - exit the loop (like break)
# next - skip to next iteration (like continue)
# redo - restart current iteration (no condition recheck)

foreach my int $n (@numbers) {
    if ($n < 0) {
        next;  # Skip negative numbers
    }
    if ($n > 100) {
        last;  # Stop if over 100
    }
    say($n);
}
```

### Statement Modifiers

Postfix modifiers for concise one-liners:

```strada
say("hello") if $verbose;
say("warning") unless $quiet;
$i = $i + 1 while $i < 10;
$i = $i + 1 until $i >= 10;
return 0 if $error;
last if $done;
next unless $valid;
```

### Labeled Loops

Control nested loops:

```strada
OUTER: foreach my int $i (@rows) {
    INNER: foreach my int $j (@cols) {
        if ($i * $j > 50) {
            last OUTER;  # Break out of both loops
        }
        if ($j == 5) {
            next OUTER;  # Skip to next outer iteration
        }
    }
}
```

### Switch Statement

Strada's switch uses braces for each case (no fall-through, no break needed):

```strada
my str $day = "Monday";

switch ($day) {
    case "Monday" {
        say("Monday!");
    }
    case "Tuesday" {
        say("Tuesday!");
    }
    case "Wednesday" {
        say("Wednesday!");
    }
    default {
        say("Other day");
    }
}
```

For grouping multiple values, use if/elsif (or else if) or a hash dispatch table:

```strada
my hash %day_type = {
    "Monday" => "Weekday",
    "Tuesday" => "Weekday",
    "Saturday" => "Weekend",
    "Sunday" => "Weekend"
};

if (exists(%day_type, $day)) {
    say($day_type{$day});
}
```

---

## 5. Functions

### Basic Functions

```strada
func greet(str $name) void {
    say("Hello, " . $name . "!");
}

func add(int $a, int $b) int {
    return $a + $b;
}

func main() int {
    greet("Alice");

    my int $sum = add(3, 4);
    say("Sum: " . $sum);

    return 0;
}
```

### Default Parameters

```strada
func greet(str $name, str $greeting = "Hello") void {
    say($greeting . ", " . $name . "!");
}

func main() int {
    greet("Alice");           # Hello, Alice!
    greet("Bob", "Hi");       # Hi, Bob!
    return 0;
}
```

### Returning Multiple Values

Return an array:

```strada
func minmax(array @nums) array {
    my int $min = $nums[0];
    my int $max = $nums[0];

    foreach my int $n (@nums) {
        if ($n < $min) { $min = $n; }
        if ($n > $max) { $max = $n; }
    }

    return ($min, $max);
}

func main() int {
    my array @result = minmax((5, 2, 8, 1, 9));
    say("Min: " . $result[0] . ", Max: " . $result[1]);
    return 0;
}
```

### Recursion

```strada
func factorial(int $n) int {
    if ($n <= 1) {
        return 1;
    }
    return $n * factorial($n - 1);
}

func fibonacci(int $n) int {
    if ($n <= 1) {
        return $n;
    }
    return fibonacci($n - 1) + fibonacci($n - 2);
}
```

---

## 6. Arrays

### Creating Arrays

```strada
# Empty array
my array @empty = ();

# Array with values
my array @numbers = (1, 2, 3, 4, 5);
my array @mixed = (1, "two", 3.0);
```

### Accessing Elements

```strada
my array @letters = ("a", "b", "c", "d");

say($letters[0]);   # a (first element)
say($letters[2]);   # c (third element)
say($letters[-1]);  # d (last element)

# Modify elements
$letters[1] = "B";
```

### Array Functions

```strada
my array @stack = ();

# Add to end
push(@stack, "first");
push(@stack, "second");

# Remove from end
my scalar $last = pop(@stack);  # "second"

# Add to beginning
unshift(@stack, "zero");

# Remove from beginning
my scalar $first = shift(@stack);  # "zero"

# Get size
my int $len = size(@stack);
```

### Splice: Remove and Replace Elements

`splice` is the Swiss Army knife for array modification. It can remove elements, insert elements, or both at the same time:

```strada
my array @colors = ("red", "green", "blue", "yellow");

# Remove 1 element at index 1
my array @removed = splice(@colors, 1, 1);
# @removed = ("green"), @colors = ("red", "blue", "yellow")

# Remove 2 elements starting at index 1, replace with new values
my array @repl = ("cyan", "magenta");
my array @old = splice(@colors, 1, 2, @repl);
# @old = ("blue", "yellow"), @colors = ("red", "cyan", "magenta")

# Insert without removing (length 0)
splice(@colors, 1, 0, ("white"));
# @colors = ("red", "white", "cyan", "magenta")

# Negative offset counts from the end
splice(@colors, -1, 1);
# Removes last element
```

### Iterating

```strada
my array @nums = (10, 20, 30);

# Using foreach
foreach my int $n (@nums) {
    say($n);
}

# Using index
for (my int $i = 0; $i < size(@nums); $i = $i + 1) {
    say("Index " . $i . ": " . $nums[$i]);
}
```

### Map, Grep, Sort

```strada
my array @nums = (1, 2, 3, 4, 5);

# Map - transform each element (use $_ for current element)
my scalar $doubled = map { $_ * 2; } @nums;
# Result: [2, 4, 6, 8, 10]

# Grep - filter elements
my scalar $evens = grep { $_ % 2 == 0; } @nums;
# Result: [2, 4]

# Sort - order elements (use $a and $b for comparison)
my array @unsorted = (3, 1, 4, 1, 5);
my scalar $sorted = sort { $a <=> $b; } @unsorted;
# Result: [1, 1, 3, 4, 5]

# Reverse sort
my scalar $desc = sort { $b <=> $a; } @unsorted;
# Result: [5, 4, 3, 1, 1]
```

---

## 7. Hashes

### Creating Hashes

```strada
# Empty hash
my hash %empty = ();

# Using anonymous hash constructor
my scalar $person = {
    name => "Alice",
    age => 30,
    city => "NYC"
};
```

### Accessing Values

```strada
my hash %config = ();
$config{"host"} = "localhost";
$config{"port"} = 8080;

say($config{"host"});  # localhost
say($config{"port"});  # 8080

# With hash reference
say($person->{"name"});  # Alice
```

### Hash Functions

```strada
my hash %data = ();
$data{"a"} = 1;
$data{"b"} = 2;
$data{"c"} = 3;

# Get all keys
my array @keys = keys(%data);    # ("a", "b", "c")

# Get all values
my array @vals = values(%data);  # (1, 2, 3)

# Check if key exists
if (exists($data{"a"})) {
    say("Key 'a' exists");
}

# Delete a key
delete($data{"b"});

# Get size
my int $size = size(%data);
```

### Iterating Over Hashes

```strada
my hash %scores = ();
$scores{"Alice"} = 95;
$scores{"Bob"} = 87;
$scores{"Charlie"} = 92;

# Using keys
foreach my str $name (keys(%scores)) {
    say($name . ": " . $scores{$name});
}
```

### Iterating with each()

The `each` function returns one key-value pair at a time as a two-element array. When all pairs have been returned, it returns an empty array:

```strada
my hash %config = ();
$config{"host"} = "localhost";
$config{"port"} = 8080;

my array @pair = each(%config);
while (size(@pair) > 0) {
    say($pair[0] . " => " . $pair[1]);
    @pair = each(%config);
}
```

This is useful when you want to process one pair at a time without building the full keys array.

### Tied Hashes

The `tie` mechanism lets you bind a hash to a class so that every read, write, and delete operation is intercepted by methods you define. This is powerful for creating hashes backed by databases, files, or custom logic:

```strada
package DefaultHash;

func TIEHASH(str $class, scalar $default) scalar {
    my hash %self = ();
    $self{"_data"} = {};
    $self{"_default"} = $default;
    return bless(\%self, "DefaultHash");
}

func FETCH(scalar $self, str $key) scalar {
    if (exists($self->{"_data"}->{$key})) {
        return $self->{"_data"}->{$key};
    }
    return $self->{"_default"};
}

func STORE(scalar $self, str $key, scalar $value) void {
    $self->{"_data"}->{$key} = $value;
}

package main;

func main() int {
    my hash %h = ();
    tie(%h, "DefaultHash", "N/A");

    $h{"name"} = "Alice";
    say($h{"name"});     # "Alice" (FETCH)
    say($h{"missing"});  # "N/A" (default from FETCH)

    untie(%h);           # Unbind
    return 0;
}
```

The class must implement `TIEHASH`, `FETCH`, and `STORE`. Optional methods include `DELETE`, `EXISTS`, `FIRSTKEY`, `NEXTKEY`, and `UNTIE`. When untied, the hash reverts to normal behavior with zero overhead.

### Nested Structures

```strada
my scalar $data = {
    users => [
        { name => "Alice", age => 30 },
        { name => "Bob", age => 25 }
    ],
    count => 2
};

say($data->{"users"}->[0]->{"name"});  # Alice
say($data->{"count"});                  # 2
```

---

## 8. References

References let you pass data structures by reference and create complex nested structures.

### Creating References

```strada
my int $x = 42;
my array @arr = (1, 2, 3);
my hash %h = ();

# Create references with backslash
my scalar $ref_x = \$x;
my scalar $ref_arr = \@arr;
my scalar $ref_h = \%h;
```

### Dereferencing

```strada
# Scalar reference
my int $x = 10;
my scalar $ref = \$x;
say($$ref);           # 10 (dereference to read)
$$ref = 20;           # Modify through reference
say($x);              # 20 (original changed)

# Array reference
my array @arr = (1, 2, 3);
my scalar $aref = \@arr;
say($aref->[0]);      # 1 (access element)
push(@{$aref}, 4);    # Add to array through ref

# Hash reference
my hash %h = ();
my scalar $href = \%h;
$href->{"key"} = "value";
say($href->{"key"});  # value
```

### Anonymous Constructors

Create references directly without named variables:

```strada
# Anonymous array reference
my scalar $nums = [1, 2, 3, 4, 5];

# Anonymous hash reference
my scalar $person = {
    name => "Alice",
    age => 30
};

# Access elements
say($nums->[0]);         # 1
say($person->{"name"});  # Alice
```

### Passing by Reference

```strada
func double_all(scalar $arr_ref) void {
    for (my int $i = 0; $i < size(@{$arr_ref}); $i = $i + 1) {
        $arr_ref->[$i] = $arr_ref->[$i] * 2;
    }
}

func main() int {
    my array @nums = (1, 2, 3);
    double_all(\@nums);
    # @nums is now (2, 4, 6)
    return 0;
}
```

---

## 9. Strings

### String Operations

```strada
my str $s = "Hello, World!";

# Length
say(length($s));              # 13

# Substring
say(substr($s, 0, 5));        # Hello
say(substr($s, 7));           # World!

# Find position
say(index($s, "World"));      # 7
say(index($s, "xyz"));        # -1 (not found)

# Case conversion
say(uc($s));                  # HELLO, WORLD!
say(lc($s));                  # hello, world!

# Trim whitespace
my str $padded = "  hello  ";
say(trim($padded));           # "hello"
```

### Split and Join

```strada
# Split string into array
my str $csv = "apple,banana,cherry";
my array @fruits = split(",", $csv);
# @fruits = ("apple", "banana", "cherry")

# Join array into string
my str $result = join(" - ", @fruits);
say($result);  # apple - banana - cherry
```

### Character Operations

```strada
# Get character code
my int $code = ord("A");      # 65

# Get character from code
my str $char = chr(65);       # "A"
```

### String Repetition

```strada
my str $line = "-" x 40;      # 40 dashes
say($line);
```

---

## 10. Regular Expressions

### Basic Matching

```strada
my str $text = "Hello, World!";

# Match with =~
if ($text =~ /World/) {
    say("Found World!");
}

# Negated match with !~
if ($text !~ /xyz/) {
    say("xyz not found");
}
```

### Substitution

```strada
my str $text = "Hello, World!";

# Replace first occurrence
$text =~ s/World/Strada/;
say($text);  # Hello, Strada!

# Replace all occurrences
my str $s = "one two one three one";
$s =~ s/one/1/g;
say($s);  # 1 two 1 three 1
```

### Pattern Anchors

```strada
if ($line =~ /^Hello/) {     # Starts with Hello
    say("Greeting line");
}

if ($line =~ /!$/) {         # Ends with !
    say("Exclamation");
}

if ($line =~ /^Hello.*!$/) { # Starts with Hello, ends with !
    say("Full greeting");
}
```

### Character Classes

```strada
if ($s =~ /\d+/) {           # One or more digits
    say("Contains numbers");
}

if ($s =~ /[a-z]+/) {        # Lowercase letters
    say("Contains lowercase");
}

if ($s =~ /\s/) {            # Whitespace
    say("Contains space");
}
```

### Modifiers

```strada
# Case insensitive
if ($s =~ /hello/i) {
    say("Found hello (any case)");
}

# Global match (for substitution)
$s =~ s/old/new/g;
```

### Captures

After a successful match, use `$1` through `$9` to access capture groups directly:

```strada
my str $date = "2024-01-15";
if ($date =~ /(\d+)-(\d+)-(\d+)/) {
    say("Year: " . $1);    # 2024
    say("Month: " . $2);   # 01
    say("Day: " . $3);     # 15
}
```

You can also use the `capture()` function for a combined match-and-capture, or `captures()` after a `=~` match for programmatic access (including `$0` for the full match):

```strada
my str $date = "2024-01-15";
my array @parts = capture($date, "(\d+)-(\d+)-(\d+)");
say("Year: " . $parts[0]);   # 2024
say("Month: " . $parts[1]);  # 01
say("Day: " . $parts[2]);    # 15
```

### The /e Modifier: Evaluate Replacement

The `/e` modifier treats the replacement as an expression to evaluate, rather than a literal string. This lets you compute replacements dynamically:

```strada
my str $text = "I have 3 cats and 12 dogs";

# Double every number in the string
$text =~ s/(\d+)/$1 * 2/eg;
say($text);  # "I have 6 cats and 24 dogs"
```

Inside the replacement expression, `$1`-`$9` and `captures()` are available to access capture groups. The `/e` modifier works with `/g` for global replacement.

### Character Transliteration (tr///)

The `tr///` operator (also spelled `y///`) translates characters one-to-one. It is not a regex -- it works character by character:

```strada
my str $text = "Hello, World!";

# Convert lowercase to uppercase
$text =~ tr/a-z/A-Z/;
say($text);  # "HELLO, WORLD!"

# Count vowels (tr returns the number of characters changed)
my str $s = "banana";
my int $vowels = ($s =~ tr/aeiou/aeiou/);
say("Vowels: " . $vowels);  # 3
```

**Flags:**

- `d` -- Delete characters not in the replacement list: `$s =~ tr/0-9//d;` removes all digits
- `s` -- Squeeze repeated translated characters into one: `$s =~ tr/ / /s;` collapses multiple spaces
- `r` -- Return a modified copy instead of changing the original: `my str $upper = ($s =~ tr/a-z/A-Z/r);`
- `c` -- Complement the search list (translate characters NOT in the list)

---

## 11. File I/O

### Reading Files

```strada
# Read entire file
my str $content = core::slurp("input.txt");
say($content);

# Read line by line
my scalar $fh = core::open("input.txt", "r");
my str $line = core::readline($fh);
while (defined($line)) {
    say($line);
    $line = core::readline($fh);
}
core::close($fh);
```

### Writing Files

```strada
# Write entire file
core::spew("output.txt", "Hello, World!\n");

# Write with file handle
my scalar $fh = core::open("output.txt", "w");
core::write_fd($fh, "Line 1\n");
core::write_fd($fh, "Line 2\n");
core::close($fh);

# Append to file
my scalar $fh = core::open("log.txt", "a");
core::write_fd($fh, "New log entry\n");
core::close($fh);
```

### File Modes

| Mode | Description |
|------|-------------|
| `"r"` | Read (file must exist) |
| `"w"` | Write (creates/truncates) |
| `"a"` | Append (creates if needed) |
| `"r+"` | Read and write |

### Selecting the Default Output Filehandle

By default, `print` and `say` write to standard output. Use `select` to redirect them to a different filehandle:

```strada
my scalar $log = core::open("app.log", "w");

# Redirect all print/say output to the log file
my scalar $prev = select($log);
say("This goes to the log file");
say("So does this");

# Restore original output
select($prev);
say("Back to stdout");

core::close($log);
```

This is especially useful for logging or when you want to temporarily redirect output without passing a filehandle to every `say` call.

### Standard Input

```strada
say("Enter your name: ");
my str $name = core::readline();
say("Hello, " . $name);
```

---

## 12. Error Handling

### Try/Catch

```strada
try {
    my int $result = risky_operation();
    say("Result: " . $result);
} catch ($error) {
    say("Error occurred: " . $error);
}
```

### Throwing Exceptions

```strada
func divide(int $a, int $b) int {
    if ($b == 0) {
        throw "Division by zero!";
    }
    return $a / $b;
}

func main() int {
    try {
        my int $result = divide(10, 0);
    } catch ($e) {
        say("Caught: " . $e);
    }
    return 0;
}
```

### Nested Try/Catch

```strada
try {
    try {
        throw "inner error";
    } catch ($e) {
        say("Inner caught: " . $e);
        throw "rethrown";
    }
} catch ($e) {
    say("Outer caught: " . $e);
}
```

### Die for Fatal Errors

```strada
if (!$valid_config) {
    die("Invalid configuration file");
}
```

---

## 13. Object-Oriented Programming

Strada supports Perl-style OOP with blessed references.

### Defining a Class

```strada
package Animal;

func Animal_new(str $name, str $species) scalar {
    my hash %self = ();
    $self{"name"} = $name;
    $self{"species"} = $species;
    return bless(\%self, "Animal");
}

func Animal_speak(scalar $self) void {
    say($self->{"name"} . " the " . $self->{"species"} . " makes a sound");
}

func Animal_get_name(scalar $self) str {
    return $self->{"name"};
}
```

### Using Objects

```strada
func main() int {
    my scalar $dog = Animal_new("Rex", "dog");
    Animal_speak($dog);  # Rex the dog makes a sound

    say("Name: " . Animal_get_name($dog));

    return 0;
}
```

### Inheritance

```strada
package Dog;
inherit Animal;

func Dog_new(str $name) scalar {
    my hash %self = ();
    $self{"name"} = $name;
    $self{"species"} = "dog";
    $self{"tricks"} = 0;
    return bless(\%self, "Dog");
}

func Dog_speak(scalar $self) void {
    say($self->{"name"} . " says: Woof!");
}

func Dog_learn_trick(scalar $self) void {
    $self->{"tricks"} = $self->{"tricks"} + 1;
    say($self->{"name"} . " knows " . $self->{"tricks"} . " tricks");
}
```

### Multiple Inheritance

```strada
package Duck;
inherit Animal, Flyable, Swimmable;

func Duck_init() void {
    inherit("Duck", "Animal");
    inherit("Duck", "Flyable");
    inherit("Duck", "Swimmable");
}
```

### Type Checking

```strada
my scalar $dog = Dog_new("Rex");

if (isa($dog, "Dog")) {
    say("It's a dog");
}

if (isa($dog, "Animal")) {
    say("It's an animal (inherited)");
}

# Get the package name
say(blessed($dog));  # "Dog"
```

### SUPER:: Calls

```strada
func Dog_greet(scalar $self) str {
    my str $base = SUPER::greet($self);  # Call Animal's greet
    return $base . " Woof!";
}
```

### DESTROY Destructors

```strada
func Dog_DESTROY(scalar $self) void {
    say($self->{"name"} . " is being destroyed");
    SUPER::DESTROY($self);  # Chain to parent
}
```

---

## 14. Closures

Closures are anonymous functions that capture variables from their enclosing scope.

### Basic Closure

```strada
# Create a closure
my scalar $greet = func (str $name) {
    return "Hello, " . $name . "!";
};

# Call with arrow syntax
say($greet->("World"));  # Hello, World!
```

### Capturing Variables

```strada
my int $multiplier = 10;

my scalar $scale = func (int $n) {
    return $n * $multiplier;  # Captures $multiplier
};

say($scale->(5));   # 50
say($scale->(7));   # 70

# Captured by reference - changes visible
$multiplier = 100;
say($scale->(5));   # 500
```

### Closures with State

```strada
func make_counter() scalar {
    my int $count = 0;

    return func () {
        $count = $count + 1;
        return $count;
    };
}

my scalar $counter = make_counter();
say($counter->());  # 1
say($counter->());  # 2
say($counter->());  # 3
```

### Passing Closures to Functions

```strada
func apply_twice(scalar $f, int $x) int {
    return $f->($f->($x));
}

my scalar $double = func (int $n) { return $n * 2; };
say(apply_twice($double, 3));  # 12 (3 -> 6 -> 12)
```

---

## 15. Multithreading

### Creating Threads

```strada
my scalar $thread = thread::create(func () {
    say("Hello from thread!");
    return 42;
});

# Wait for thread to complete
my scalar $result = thread::join($thread);
say("Thread returned: " . $result);
```

### Sharing Data with Mutexes

```strada
my int $counter = 0;
my scalar $mutex = thread::mutex_new();

my scalar $worker = func () {
    for (my int $i = 0; $i < 1000; $i = $i + 1) {
        thread::mutex_lock($mutex);
        $counter = $counter + 1;
        thread::mutex_unlock($mutex);
    }
};

# Create multiple threads
my scalar $t1 = thread::create($worker);
my scalar $t2 = thread::create($worker);

thread::join($t1);
thread::join($t2);

say("Counter: " . $counter);  # 2000

thread::mutex_destroy($mutex);
```

### Condition Variables

```strada
my int $ready = 0;
my scalar $mutex = thread::mutex_new();
my scalar $cond = thread::cond_new();

# Producer thread
my scalar $producer = thread::create(func () {
    core::sleep(1);  # Simulate work

    thread::mutex_lock($mutex);
    $ready = 1;
    thread::cond_signal($cond);
    thread::mutex_unlock($mutex);
});

# Consumer waits
thread::mutex_lock($mutex);
while ($ready == 0) {
    thread::cond_wait($cond, $mutex);
}
say("Data is ready!");
thread::mutex_unlock($mutex);

thread::join($producer);
```

---

## 16. Next Steps

You've learned the fundamentals of Strada! Here are some paths forward:

### Explore More Documentation

- [Language Manual](LANGUAGE_MANUAL.md) - Complete language reference
- [OOP Guide](OOP_GUIDE.md) - Deep dive into object-oriented programming
- [FFI Guide](FFI_GUIDE.md) - Calling C libraries
- [Examples](EXAMPLES.md) - More code examples

### Try These Projects

1. **Command-line tool** - Parse arguments, process files
2. **Data processor** - Read CSV, transform data, output JSON
3. **Simple web server** - Handle HTTP requests
4. **Text adventure game** - Use OOP for game objects

### Study the Examples

```bash
ls examples/
./strada -r examples/test_threads.strada
./strada -r examples/test_json.strada
./strada -r examples/web_server.strada
```

### Read the Compiler Source

The compiler is written in Strada itself:

```bash
cat compiler/Parser.strada   # How parsing works
cat compiler/CodeGen.strada  # How code generation works
```

### Contribute

- Report bugs
- Add features to the compiler
- Improve the runtime
- Write documentation

Happy coding with Strada!

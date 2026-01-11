# Strada Language Manual

This is the complete technical reference for the Strada programming language. For tutorials, see [Tutorial](TUTORIAL.md). For quick syntax lookup, see [Quick Reference](QUICK_REFERENCE.md).

## Table of Contents

1. [Lexical Structure](#1-lexical-structure)
2. [Types](#2-types)
3. [Variables](#3-variables)
4. [Operators](#4-operators)
5. [Expressions](#5-expressions)
6. [Statements](#6-statements)
7. [Functions](#7-functions)
8. [Arrays](#8-arrays)
9. [Hashes](#9-hashes)
10. [References](#10-references)
11. [Strings](#11-strings)
12. [Regular Expressions](#12-regular-expressions)
13. [Control Flow](#13-control-flow)
14. [Exception Handling](#14-exception-handling)
15. [Packages and Modules](#15-packages-and-modules)
16. [Object-Oriented Programming](#16-object-oriented-programming)
    - [Operator Overloading](#167-operator-overloading)
17. [Closures](#17-closures)
18. [Multithreading](#18-multithreading)
19. [Async/Await](#19-asyncawait)
20. [Foreign Function Interface](#20-foreign-function-interface)
21. [Built-in Functions](#21-built-in-functions)
22. [Magic Variables](#22-magic-variables)
23. [Reserved Words](#23-reserved-words)

---

## 1. Lexical Structure

### 1.1 Character Set

Strada source files are encoded in UTF-8. Identifiers and keywords use ASCII characters only.

### 1.2 Comments

```strada
# Single-line comment (extends to end of line)

/* Multi-line comment
   can span multiple lines */
```

### 1.3 Identifiers

Identifiers start with a letter or underscore, followed by letters, digits, or underscores.

```
identifier := [a-zA-Z_][a-zA-Z0-9_]*
```

Identifiers are case-sensitive: `$foo`, `$Foo`, and `$FOO` are distinct.

### 1.4 Sigils

Variable names are prefixed with sigils indicating their type:

| Sigil | Type | Example |
|-------|------|---------|
| `$` | Scalar (single value) | `$count`, `$name` |
| `@` | Array | `@items`, `@numbers` |
| `%` | Hash | `%config`, `%data` |
| `&` | Subroutine reference | `\&func` |

### 1.5 Literals

**Integer literals:**
```strada
42        # Decimal
0xFF      # Hexadecimal
0o77      # Octal
0b1010    # Binary
```

**Floating-point literals:**
```strada
3.14      # Standard
1.5e10    # Scientific notation
2.5E-3    # Scientific with negative exponent
```

**String literals:**
```strada
"double quoted"     # Allows escape sequences
'single quoted'     # Literal (minimal escaping)
```

**Escape sequences in double-quoted strings:**

| Sequence | Meaning |
|----------|---------|
| `\n` | Newline |
| `\t` | Tab |
| `\r` | Carriage return |
| `\\` | Backslash |
| `\"` | Double quote |
| `\0` | Null character |

**Special literals:**
```strada
undef     # Undefined value
```

### 1.6 Operators and Punctuation

```
+ - * / %           # Arithmetic
== != < > <= >=     # Numeric comparison
eq ne lt gt le ge   # String comparison
<=>                 # Spaceship (comparison)
&& || !             # Logical
& | ^ ~ << >>       # Bitwise
. x                 # String (concat, repeat)
= += -= *= /= .=    # Assignment
-> =>               # Arrow, fat comma
\ @{ %{ ${         # Reference operators
```

---

## 2. Types

### 2.1 Type System Overview

Strada is statically typed. Variables must have declared types, but implicit conversions occur in certain contexts.

### 2.2 Scalar Types

| Type | Description | Size | Range |
|------|-------------|------|-------|
| `int` | Signed integer | 64 bits | -2^63 to 2^63-1 |
| `num` | Floating point | 64 bits | IEEE 754 double |
| `str` | String | Variable | UTF-8 encoded |
| `scalar` | Generic scalar | Variable | Any of the above |

### 2.3 Composite Types

| Type | Description |
|------|-------------|
| `array` | Ordered list of scalars |
| `hash` | Key-value map (string keys) |

### 2.4 Special Types

| Type | Description |
|------|-------------|
| `void` | No value (for function returns) |
| `undef` | Undefined/uninitialized value |

### 2.5 Reference Types

References point to other values:

```strada
\$scalar    # Reference to scalar
\@array     # Reference to array
\%hash      # Reference to hash
\&func      # Reference to function
```

### 2.6 Type Conversions

**Implicit conversions:**

| From | To | Rule |
|------|----|------|
| `int` | `num` | Exact conversion |
| `int` | `str` | Decimal representation |
| `num` | `int` | Truncation toward zero |
| `num` | `str` | Standard floating-point format |
| `str` | `int` | Parse as integer, 0 if invalid |
| `str` | `num` | Parse as float, 0.0 if invalid |

**Explicit casting:**

```strada
my int $i = cast_int($value);
my num $n = cast_num($value);
my str $s = cast_str($value);
```

### 2.7 Boolean Context

Values are evaluated as boolean in conditions:

| Type | False when | True when |
|------|------------|-----------|
| `int` | 0 | Non-zero |
| `num` | 0.0 | Non-zero |
| `str` | "" or "0" | Non-empty and not "0" |
| `array` | Empty | Non-empty |
| `hash` | Empty | Non-empty |
| `undef` | Always | Never |
| Reference | Never | Always |

---

## 3. Variables

### 3.1 Declaration

Variables are declared with `my`:

```strada
my TYPE SIGIL NAME;
my TYPE SIGIL NAME = EXPRESSION;
```

Examples:
```strada
my int $count;              # Uninitialized (undef)
my int $count = 0;          # Initialized
my str $name = "Alice";
my array @items = ();
my hash %config = ();
my scalar $value = 42;
```

### 3.2 Scope

Variables are lexically scoped to the enclosing block:

```strada
func example() void {
    my int $x = 1;          # Visible in entire function

    if ($x > 0) {
        my int $y = 2;      # Visible only in if block
        say($x + $y);       # OK: both visible
    }

    say($x);                # OK
    # say($y);              # Error: $y not in scope
}
```

### 3.3 Global Variables

Package-level variables declared with `my` are accessible across functions within the same file:

```strada
package MyApp;

my int $global_count = 0;   # Package global (file-level)

func increment() void {
    $global_count = $global_count + 1;
}

func get_count() int {
    return $global_count;
}
```

### 3.4 Our Variables (Package-Scoped Globals)

The `our` keyword declares variables backed by the global registry. Unlike `my` globals (which compile to C global variables), `our` variables use `strada_global_set/get` at runtime, making them accessible across dynamically loaded modules.

```strada
our int $count = 0;
our str $name = "hello";
our int $no_init;           # Defaults to undef

package Config;
our str $host = "localhost";
our int $port = 8080;

package main;

func modify() void {
    $count = 42;            # Writes to global registry
    $count += 10;           # Compound assignment works
    $name .= " world";     # String append works
}

func main() int {
    say($count);            # 0
    modify();
    say($count);            # 52

    # Access other packages' our vars via global registry
    my scalar $h = core::global_get("Config::host");
    say($h);                # "localhost"

    return 0;
}
```

Key differences between `my` and `our` at package level:
- `my` globals compile to C `StradaValue *` globals -- fast but not accessible across `import_lib` boundaries
- `our` globals use the runtime registry -- slightly slower but accessible from any module via `core::global_get("pkg::name")`

### 3.5 Local Variables (Dynamic Scoping)

The `local` keyword temporarily overrides an `our` variable for the duration of the enclosing scope. The original value is automatically restored on scope exit.

```strada
local $VAR = EXPR;
```

Semantics:
- Only applies to `our` variables (compile error on `my` variables)
- Saves the current value, assigns the new one
- Restores the saved value when the enclosing block exits (including via exception)
- Provides dynamic scoping: called functions see the overridden value
- Works correctly with `try`/`catch` -- value is restored even when an exception is thrown

```strada
our int $debug = 0;

func log_msg(str $msg) void {
    say($msg) if $debug;
}

func verbose_operation() void {
    local $debug = 1;        # Override for this scope
    log_msg("Starting...");  # Printed (debug=1)
    do_work();
    log_msg("Done.");        # Printed (debug=1)
}
# $debug is restored to 0 here
```

### 3.6 Special Variables

| Variable | Description |
|----------|-------------|
| `@ARGV` | Command-line arguments |
| `$ARGC` | Argument count |
| `$_` | Default variable (in map/grep/sort) |
| `$a`, `$b` | Sort comparison variables |
| `__PACKAGE__` | Current package name (runtime) |
| `__FILE__` | Current source file |
| `__LINE__` | Current line number |
| `::func()` | Call func in current package (compile-time) |
| `.::func()` | Alternate syntax for above |
| `__PACKAGE__::func()` | Explicit form of above |

---

## 4. Operators

### 4.1 Operator Precedence (highest to lowest)

| Precedence | Operators | Associativity |
|------------|-----------|---------------|
| 1 | `->` | Left |
| 2 | `++` `--` | N/A |
| 3 | `!` `~` `\` `-` (unary) | Right |
| 4 | `**` | Right |
| 5 | `*` `/` `%` `x` | Left |
| 6 | `+` `-` `.` | Left |
| 7 | `<<` `>>` | Left |
| 8 | `<` `>` `<=` `>=` `lt` `gt` `le` `ge` | Left |
| 9 | `==` `!=` `<=>` `eq` `ne` | Left |
| 10 | `&` | Left |
| 11 | `|` `^` | Left |
| 12 | `&&` | Left |
| 13 | `||` | Left |
| 14 | `? :` | Right |
| 15 | `=` `+=` `-=` etc. | Right |
| 16 | `,` | Left |

### 4.2 Arithmetic Operators

| Operator | Description | Example |
|----------|-------------|---------|
| `+` | Addition | `$a + $b` |
| `-` | Subtraction | `$a - $b` |
| `*` | Multiplication | `$a * $b` |
| `/` | Division | `$a / $b` |
| `%` | Modulo | `$a % $b` |
| `**` | Exponentiation | `$a ** $b` |
| `-` (unary) | Negation | `-$a` |

### 4.3 String Operators

| Operator | Description | Example |
|----------|-------------|---------|
| `.` | Concatenation | `$a . $b` |
| `x` | Repetition | `$s x 3` |

The `x` operator repeats a string a specified number of times. The left operand is the string and the right operand is an integer count. It has the same precedence as `*`, `/`, and `%` (precedence level 5).

```strada
"ab" x 3            # "ababab"
"-" x 40            # "----------------------------------------"
$char x $count      # Variable repetition
```

If the count is zero or negative, the result is an empty string.

### 4.4 Comparison Operators

**Numeric:**

| Operator | Description |
|----------|-------------|
| `==` | Equal |
| `!=` | Not equal |
| `<` | Less than |
| `>` | Greater than |
| `<=` | Less or equal |
| `>=` | Greater or equal |
| `<=>` | Spaceship (-1, 0, or 1) |

**String:**

| Operator | Description |
|----------|-------------|
| `eq` | Equal |
| `ne` | Not equal |
| `lt` | Less than |
| `gt` | Greater than |
| `le` | Less or equal |
| `ge` | Greater or equal |
| `cmp` | Spaceship for strings |

### 4.5 Logical Operators

| Operator | Description |
|----------|-------------|
| `&&` | Logical AND (short-circuit) |
| `||` | Logical OR (short-circuit) |
| `!` | Logical NOT |

### 4.6 Bitwise Operators

| Operator | Description |
|----------|-------------|
| `&` | Bitwise AND |
| `|` | Bitwise OR |
| `^` | Bitwise XOR |
| `~` | Bitwise NOT |
| `<<` | Left shift |
| `>>` | Right shift |

### 4.7 Assignment Operators

| Operator | Equivalent |
|----------|------------|
| `=` | Assignment |
| `+=` | `$x = $x + $y` |
| `-=` | `$x = $x - $y` |
| `*=` | `$x = $x * $y` |
| `/=` | `$x = $x / $y` |
| `%=` | `$x = $x % $y` |
| `.=` | `$s = $s . $t` |
| `&=` | `$x = $x & $y` |
| `|=` | `$x = $x | $y` |

### 4.8 Other Operators

| Operator | Description | Example |
|----------|-------------|---------|
| `? :` | Ternary conditional | `$x > 0 ? "pos" : "neg"` |
| `->` | Dereference/method call | `$ref->{"key"}` |
| `\` | Reference creation | `\$var` |
| `..` | Range | `1..10` |

---

## 5. Expressions

### 5.1 Primary Expressions

- Literals: `42`, `3.14`, `"hello"`, `undef`
- Variables: `$x`, `@arr`, `%hash`
- Function calls: `func()`, `func($arg)`
- Parenthesized: `($a + $b)`

### 5.2 Array/Hash Access

```strada
$array[INDEX]           # Array element
$hash{"KEY"}            # Hash element
$ref->[INDEX]           # Dereference array element
$ref->{"KEY"}           # Dereference hash element
```

### 5.3 Anonymous Constructors

```strada
[EXPR, EXPR, ...]       # Anonymous array reference
{ KEY => VAL, ... }     # Anonymous hash reference
```

### 5.4 Function Expressions (Closures)

```strada
func (PARAMS) { BODY }
func (PARAMS) RETURN_TYPE { BODY }
```

---

## 6. Statements

### 6.1 Expression Statement

Any expression followed by semicolon:

```strada
$x = $y + 1;
say("hello");
```

### 6.2 Block Statement

```strada
{
    STATEMENT;
    STATEMENT;
    ...
}
```

### 6.3 Declaration Statement

```strada
my TYPE SIGIL NAME;
my TYPE SIGIL NAME = EXPR;
```

### 6.4 Empty Statement

```strada
;
```

---

## 7. Functions

### 7.1 Function Definition

`fn` is shorthand for `func` and can be used interchangeably in all contexts.

```strada
func NAME(PARAMETERS) RETURN_TYPE {
    BODY
}
```

Examples:
```strada
func add(int $a, int $b) int {
    return $a + $b;
}

func greet(str $name) void {
    say("Hello, " . $name);
}
```

### 7.2 Parameters

**Required parameters:**
```strada
func example(int $x, str $y, num $z) void { }
```

**Default parameters:**
```strada
func greet(str $name, str $greeting = "Hello") void {
    say($greeting . ", " . $name);
}
```

**Array parameters:**
```strada
func sum(array @nums) int {
    my int $total = 0;
    foreach my int $n (@nums) {
        $total = $total + $n;
    }
    return $total;
}
```

**Hash parameters:**
```strada
func process(hash %options) void {
    if (exists($options{"verbose"})) {
        say("Verbose mode");
    }
}
```

### 7.3 Return Values

```strada
func example() int {
    return 42;
}

func nothing() void {
    # No return value
}

func multi() array {
    return (1, 2, 3);  # Return array
}

func flexible() dynamic {
    if (core::wantarray()) {
        my array @result = (1, 2, 3);
        return @result;    # Array context
    }
    return 42;             # Scalar context (default)
}

my array @arr = flexible();  # Gets (1, 2, 3)
my int $val = flexible();    # Gets 42
```

The `dynamic` return type allows functions to inspect calling context using `core::wantarray()`, `core::wantscalar()`, and `core::wanthash()`, returning different types based on how the result is used.

### 7.4 Recursion

Functions can call themselves:

```strada
func factorial(int $n) int {
    if ($n <= 1) {
        return 1;
    }
    return $n * factorial($n - 1);
}
```

### 7.5 Forward Declarations

For mutual recursion:

```strada
func is_even(int $n) int;  # Forward declaration

func is_odd(int $n) int {
    if ($n == 0) { return 0; }
    return is_even($n - 1);
}

func is_even(int $n) int {
    if ($n == 0) { return 1; }
    return is_odd($n - 1);
}
```

---

## 8. Arrays

### 8.1 Creation

```strada
my array @empty = ();
my array @nums = (1, 2, 3, 4, 5);
my array @mixed = (1, "two", 3.0);
```

### 8.2 Access

```strada
$arr[INDEX]             # Get element (0-based)
$arr[-1]                # Last element
$arr[-2]                # Second to last
```

### 8.3 Modification

```strada
$arr[INDEX] = VALUE;    # Set element
push(@arr, VALUE);      # Add to end
pop(@arr);              # Remove from end
unshift(@arr, VALUE);   # Add to beginning
shift(@arr);            # Remove from beginning
```

### 8.4 Array Functions

| Function | Description |
|----------|-------------|
| `push(@arr, $val)` | Append element |
| `pop(@arr)` | Remove and return last |
| `shift(@arr)` | Remove and return first |
| `unshift(@arr, $val)` | Prepend element |
| `splice(@arr, $off, $len, ...)` | Remove/replace elements, returns removed |
| `size(@arr)` | Get length |
| `reverse(@arr)` | Reverse array |
| `sort(@arr)` | Sort (default) |
| `sort { BLOCK } @arr` | Sort with comparator |

### 8.5 Splice

```strada
splice(@ARRAY, OFFSET, LENGTH)
splice(@ARRAY, OFFSET, LENGTH, LIST)
```

Removes `LENGTH` elements from `@ARRAY` starting at `OFFSET`, replacing them with `LIST` (if provided). Returns an array of the removed elements.

- `OFFSET` is 0-based; negative values count from the end (`-1` = last element)
- `LENGTH` of 0 inserts without removing
- Omitting `LIST` performs a pure removal

```strada
my array @a = (1, 2, 3, 4, 5);
my array @removed = splice(@a, 1, 2);       # @removed=(2,3), @a=(1,4,5)
splice(@a, 1, 0, 20, 30);                   # Insert: @a=(1,20,30,4,5)
splice(@a, -2, 2, 99);                      # @a=(1,20,30,99)
```

### 8.5 Iteration

```strada
foreach my TYPE $var (@arr) {
    # Process $var
}

for (my int $i = 0; $i < size(@arr); $i = $i + 1) {
    # Access $arr[$i]
}
```

### 8.6 Map, Grep, Sort

```strada
# Map - transform elements
my scalar $result = map { EXPR; } @arr;

# Grep - filter elements
my scalar $result = grep { CONDITION; } @arr;

# Sort - custom comparison
my scalar $result = sort { $a <=> $b; } @arr;
```

---

## 9. Hashes

### 9.1 Creation

```strada
my hash %empty = ();
my scalar $href = { key => "value", num => 42 };
```

### 9.2 Access

```strada
$hash{"KEY"}            # Get value
$hash{"KEY"} = VALUE;   # Set value
$href->{"KEY"}          # Via reference
```

### 9.3 Hash Functions

| Function | Description |
|----------|-------------|
| `keys(%hash)` | Get all keys as array |
| `values(%hash)` | Get all values as array |
| `exists($hash{"key"})` | Check if key exists |
| `delete($hash{"key"})` | Remove key |
| `size(%hash)` | Get number of keys |
| `each(%hash)` | Return next `[key, value]` pair |
| `tie(%hash, "Class")` | Bind hash to TIEHASH class |
| `untie(%hash)` | Remove tie binding |
| `tied(%hash)` | Get underlying tied object (or undef) |

### 9.4 Each

```strada
each(%HASH)
```

Returns the next key-value pair from the hash as a two-element array `[key, value]`. Returns an empty array when all pairs have been returned. The internal iterator resets automatically after exhaustion.

```strada
my array @pair = each(%hash);
while (size(@pair) > 0) {
    say($pair[0] . " = " . $pair[1]);
    @pair = each(%hash);
}
```

### 9.5 Tied Hashes

```strada
tie(%HASH, "ClassName");
untie(%HASH);
tied(%HASH);
```

`tie` binds a hash variable to a class that implements the TIEHASH interface. All subsequent operations on the hash (`FETCH`, `STORE`, `EXISTS`, `DELETE`, iteration via `FIRSTKEY`/`NEXTKEY`, `CLEAR`) are dispatched to methods on the tied object.

**TIEHASH interface:**

| Method | Triggered by |
|--------|-------------|
| `TIEHASH($class)` | `tie(%h, "Class")` |
| `FETCH($self, $key)` | `$h{"key"}` |
| `STORE($self, $key, $val)` | `$h{"key"} = $val` |
| `EXISTS($self, $key)` | `exists($h{"key"})` |
| `DELETE($self, $key)` | `delete($h{"key"})` |
| `FIRSTKEY($self)` | Start of `keys(%h)` / `each(%h)` |
| `NEXTKEY($self, $lastkey)` | Continue iteration |
| `CLEAR($self)` | `%h = ()` or reassignment |

`tied(%h)` returns the underlying blessed object if the hash is tied, or `undef` otherwise. `untie(%h)` removes the tie binding, restoring normal hash behavior.

Zero runtime overhead for untied hashes -- the dispatch check uses `__builtin_expect` to optimize the common (untied) path.

```strada
tie(%config, "EnvHash");       # Bind to EnvHash class
say($config{"HOME"});          # Calls EnvHash::FETCH
my scalar $obj = tied(%config); # Get the EnvHash object
untie(%config);                # Unbind
```

### 9.6 Iteration

```strada
foreach my str $key (keys(%hash)) {
    my scalar $val = $hash{$key};
    say($key . " => " . $val);
}
```

---

## 10. References

### 10.1 Creating References

```strada
\$scalar    # Reference to scalar variable
\@array     # Reference to array variable
\%hash      # Reference to hash variable
\&func      # Reference to function

[1, 2, 3]   # Anonymous array reference
{a => 1}    # Anonymous hash reference
```

### 10.2 Dereferencing

**Scalar references:**
```strada
$$ref           # Dereference (read/write)
deref($ref)     # Alternative read
deref_set($ref, $val)  # Alternative write
```

**Array references:**
```strada
@{$ref}         # Full array
$ref->[$i]      # Element access
```

**Hash references:**
```strada
%{$ref}         # Full hash
$ref->{"key"}   # Element access
```

### 10.3 Reference Functions

| Function | Description |
|----------|-------------|
| `is_ref($val)` | Check if value is a reference |
| `reftype($ref)` | Get reference type ("SCALAR", "ARRAY", "HASH") |
| `deref($ref)` | Dereference scalar |
| `deref_set($ref, $val)` | Set through scalar reference |
| `core::weaken($ref)` | Make `$ref` a weak reference |
| `core::isweak($ref)` | Returns 1 if `$ref` is weak, 0 otherwise |

### 10.4 Weak References

Weak references prevent circular reference cycles from causing memory leaks. A weak reference does not increment the target's refcount, so the target can be freed when only weak references remain.

```strada
my scalar $parent = { "name" => "parent" };
my scalar $child = { "name" => "child" };
$parent->{"child"} = $child;
$child->{"parent"} = $parent;

# Break the cycle
core::weaken($child->{"parent"});
say(core::isweak($child->{"parent"}));   # 1
say($child->{"parent"}->{"name"});       # "parent" (still accessible)
```

When the target of a weak reference is freed, dereferencing the weak ref returns `undef`:

```strada
my scalar $weak;
{
    my scalar $obj = { "data" => "hello" };
    $weak = $obj;
    core::weaken($weak);
}
# $obj freed at scope exit; $weak->{"data"} is now undef
```

- `core::weaken()` works on hash entry values: `core::weaken($hash->{"key"})`
- Calling `core::weaken()` on an already-weak reference is a safe no-op (idempotent)
- Multiple weak references to the same target are supported

---

## 11. Strings

### 11.1 String Functions

| Function | Description |
|----------|-------------|
| `length($s)` | Get string length |
| `substr($s, $start)` | Substring from start |
| `substr($s, $start, $len)` | Substring with length |
| `index($s, $sub)` | Find substring position |
| `rindex($s, $sub)` | Find from end |
| `uc($s)` | Uppercase |
| `lc($s)` | Lowercase |
| `ucfirst($s)` | Capitalize first |
| `lcfirst($s)` | Lowercase first |
| `trim($s)` | Remove leading/trailing whitespace |
| `ltrim($s)` | Remove leading whitespace |
| `rtrim($s)` | Remove trailing whitespace |
| `reverse($s)` | Reverse string |
| `chomp($s)` | Remove trailing newline |
| `chop($s)` | Remove last character |
| `chr($n)` | Character from code point |
| `ord($s)` | Code point from character |
| `join($sep, @arr)` | Join array elements |
| `split($sep, $s)` | Split string to array |

### 11.2 Binary/Byte Operations

For working with binary data (protocols, file formats, etc.):

| Function | Description |
|----------|-------------|
| `core::ord_byte($s)` | First byte as integer (0-255) |
| `core::get_byte($s, $pos)` | Byte at position |
| `core::set_byte($s, $pos, $val)` | Set byte, returns new string |
| `core::byte_length($s)` | Byte count (not char count) |
| `core::byte_substr($s, $start, $len)` | Substring by bytes |
| `core::pack($fmt, @vals)` | Pack values to binary |
| `core::unpack($fmt, $s)` | Unpack binary to array |

**Pack Format Characters:**

| Char | Description |
|------|-------------|
| `c/C` | Signed/unsigned byte |
| `s/S` | Short (native endian) |
| `n/v` | Short (big/little endian) |
| `l/L` | Long (native endian) |
| `N/V` | Long (big/little endian) |
| `q/Q` | Quad (native endian) |
| `a/A` | String (null/space padded) |
| `H` | Hex string |
| `x/X` | Null / backup byte |

---

## 12. Regular Expressions

### 12.1 Match Operator

```strada
$string =~ /PATTERN/          # Match
$string !~ /PATTERN/          # Negated match
```

### 12.2 Substitution Operator

```strada
$string =~ s/PATTERN/REPLACEMENT/     # Replace first
$string =~ s/PATTERN/REPLACEMENT/g    # Replace all
```

### 12.3 Pattern Syntax

| Pattern | Matches |
|---------|---------|
| `.` | Any character (except newline) |
| `^` | Start of string |
| `$` | End of string |
| `*` | Zero or more |
| `+` | One or more |
| `?` | Zero or one |
| `{n}` | Exactly n |
| `{n,m}` | Between n and m |
| `[abc]` | Character class |
| `[^abc]` | Negated class |
| `\d` | Digit |
| `\w` | Word character |
| `\s` | Whitespace |
| `\D` | Non-digit |
| `\W` | Non-word |
| `\S` | Non-whitespace |
| `(...)` | Capture group |
| `|` | Alternation |

### 12.4 Modifiers

| Modifier | Effect |
|----------|--------|
| `i` | Case insensitive |
| `g` | Global (for substitution) |
| `m` | Multiline |
| `s` | Dotall (`.` matches newlines) |
| `x` | Extended (ignore whitespace in pattern) |
| `e` | Evaluate replacement as expression (substitution only) |

### 12.5 The `/e` Modifier

The `/e` modifier causes the replacement string of `s///` to be evaluated as a Strada expression. The expression result becomes the replacement text.

```strada
$string =~ s/PATTERN/EXPRESSION/e
$string =~ s/PATTERN/EXPRESSION/eg    # With global
```

Within the replacement expression, `$1` through `$9` provide direct access to capture groups, and `captures()` is also available for programmatic access.

```strada
my str $text = "x=10 y=20";
$text =~ s/(\d+)/$1 * 2/eg;
# $text is now "x=20 y=40"

my str $s = "hello world";
$s =~ s/(\w+)/uc($1)/eg;
# $s is now "HELLO WORLD"
```

### 12.6 Transliteration (`tr///` / `y///`)

```strada
$string =~ tr/SEARCH/REPLACEMENT/FLAGS
$string =~ y/SEARCH/REPLACEMENT/FLAGS    # Synonym
```

Performs character-by-character transliteration (not regex). Each character in `SEARCH` is replaced by the corresponding character in `REPLACEMENT`. Character ranges (`a-z`) are supported. Returns the number of characters transliterated.

**Flags:**

| Flag | Effect |
|------|--------|
| `c` | Complement: transliterate characters NOT in SEARCH |
| `d` | Delete: remove SEARCH characters that have no REPLACEMENT |
| `s` | Squeeze: collapse runs of the same replacement character to one |
| `r` | Return: return modified copy, do not modify original |

```strada
$s =~ tr/a-z/A-Z/;          # Lowercase to uppercase
$s =~ tr/0-9//d;             # Delete all digits
$s =~ tr/ / /s;              # Squeeze multiple spaces to one
my str $copy = ($s =~ tr/a-z/A-Z/r);  # Return uppercase copy
my int $n = ($s =~ tr/aeiou//);       # Count vowels (no replacement)
```

### 12.7 Capture Variables and Functions

After a successful regex match with `=~`, capture groups are available via `$1` through `$9`:

```strada
if ($string =~ /(\w+)\s+(\w+)/) {
    say($1);  # First capture group
    say($2);  # Second capture group
}
```

`$1`-`$9` are syntactic sugar for `captures()[N]`. They return `undef` if the group does not exist.

For programmatic access or the full match (`$0`), use `captures()`:

```strada
if ($string =~ /(\w+)\s+(\w+)/) {
    my array @parts = captures();
    say($parts[0]);  # Full match
    say($parts[1]);  # First group (same as $1)
}
```

The `capture()` function performs a match and returns captures in one call:

```strada
my array @captures = capture($string, $pattern);
```

---

## 13. Control Flow

### 13.1 Conditional Statements

**if/elsif/else:**

Both `elsif` and `else if` are supported and interchangeable:

```strada
if (CONDITION) {
    BODY
} elsif (CONDITION) {
    BODY
} else if (CONDITION) {
    BODY
} else {
    BODY
}
```

**unless (negated if):**
```strada
unless (CONDITION) {
    BODY
}

# With else (no elsif/else if allowed)
unless (CONDITION) {
    BODY
} else {
    BODY
}
```

**switch/case:**
```strada
switch (EXPR) {
    case VALUE {
        BODY
    }
    case VALUE {
        BODY
    }
    default {
        BODY
    }
}
```

Note: Unlike C/Java, Strada's switch uses braces for each case (no colons), and there's no fall-through behavior. Each case is self-contained, so `break` is not needed.

### 13.2 Loops

**while:**
```strada
while (CONDITION) {
    BODY
}
```

**until (negated while):**
```strada
until (CONDITION) {
    BODY
}
```

**do-while:**
```strada
do {
    BODY
} while (CONDITION);
```

**for:**
```strada
for (INIT; CONDITION; UPDATE) {
    BODY
}
```

**foreach:**
```strada
foreach my TYPE $var (LIST) {
    BODY
}
```

### 13.3 Loop Control

| Statement | Effect |
|-----------|--------|
| `last` | Exit innermost loop |
| `next` | Skip to next iteration |
| `redo` | Restart current iteration (no condition recheck) |
| `last LABEL` | Exit labeled loop |
| `next LABEL` | Skip in labeled loop |
| `redo LABEL` | Restart iteration of labeled loop |

**Labels:**
```strada
OUTER: while (1) {
    INNER: while (1) {
        last OUTER;  # Exit both loops
        next INNER;  # Skip to next inner iteration
        redo OUTER;  # Restart current outer iteration
    }
}
```

### 13.4 Statement Modifiers

Statement modifiers allow postfix conditional or loop control on a single expression:

```strada
# Postfix if/unless
say("hello") if $verbose;
say("warning") unless $quiet;

# Postfix while/until
$i = $i + 1 while $i < 10;
$i = $i + 1 until $i >= 10;

# Works with return, last, next, redo
return 0 if $error;
return $val unless $invalid;
last if $done;
next unless $valid;
```

**Supported forms:**
| Modifier | Equivalent |
|----------|------------|
| `EXPR if COND;` | `if (COND) { EXPR; }` |
| `EXPR unless COND;` | `if (!COND) { EXPR; }` |
| `EXPR while COND;` | `while (COND) { EXPR; }` |
| `EXPR until COND;` | `while (!COND) { EXPR; }` |

### 13.5 Goto

```strada
LABEL:
    # code
    goto LABEL;
```

---

## 14. Exception Handling

### 14.1 Try/Catch

```strada
try {
    # Code that may throw
} catch ($error) {
    # Handle error
}
```

### 14.2 Throw

```strada
throw "Error message";
throw $error_value;
```

### 14.3 Die

Terminates program with error:

```strada
die("Fatal error");
```

---

## 15. Packages and Modules

### 15.1 Package Declaration

```strada
package MyPackage;

# Package contents
```

### 15.2 Using Modules

```strada
use Module::Name;                    # Import all
use Module::Name qw(func1 func2);    # Import specific
```

### 15.3 Module Files

Module `Foo::Bar` is in file `lib/Foo/Bar.sm`.

### 15.4 Qualified Names

```strada
My::Module::function($arg);
$My::Module::variable;
```

---

## 16. Object-Oriented Programming

### 16.1 Blessed References

```strada
my scalar $obj = bless(\%hash, "ClassName");
```

### 16.2 Inheritance

```strada
package Child;
inherit Parent;

# Or for multiple inheritance
inherit Parent1, Parent2;

# Or at runtime
inherit("Child", "Parent");
```

### 16.3 OOP Functions

| Function | Description |
|----------|-------------|
| `bless($ref, $pkg)` | Associate ref with package |
| `blessed($obj)` | Get package name |
| `isa($obj, $pkg)` | Type check (with inheritance) |
| `can($obj, $method)` | Check method exists |
| `SUPER::method()` | Call parent method |

### 16.4 DESTROY

Destructor called when object is freed:

```strada
func ClassName_DESTROY(scalar $self) void {
    # Cleanup
    SUPER::DESTROY($self);
}
```

### 16.5 AUTOLOAD

Fallback method called when an undefined method is invoked on an object:

```strada
func AUTOLOAD(scalar $self, str $method, scalar ...@args) scalar {
    say("Undefined method called: " . $method);
    return undef;
}
```

- `$self` -- the object instance
- `$method` -- name of the undefined method that was called
- `...@args` -- all arguments passed to the undefined method
- Looked up through the inheritance chain (parent AUTOLOAD catches child calls)
- Real methods always take priority over AUTOLOAD
- `$obj->can("missing")` returns 0 even if AUTOLOAD exists

### 16.5.1 Dynamic Method Dispatch

Call methods where the method name is stored in a variable:

```strada
my str $method = "speak";
$obj->$method();              # Equivalent to $obj->speak()
$obj->$method($arg1, $arg2);  # With arguments
$obj->$method;                # Without parens (zero-arg call)
```

- Resolved at runtime using the same `strada_method_call()` dispatch
- All OOP features work: inheritance, AUTOLOAD, method modifiers
- Spread arguments supported: `$obj->$method(...@args)`

### 16.6 Moose-Style Declarative OOP

Strada provides a declarative OOP system inspired by Perl's Moose, with `has`, `extends`, `with`, and method modifiers (`before`, `after`, `around`).

#### 16.6.1 Attribute Declarations (`has`)

```strada
has ACCESS TYPE $NAME [= DEFAULT] [(OPTIONS)];
```

**Access modifiers:**

| Modifier | Description | Generated Methods |
|----------|-------------|-------------------|
| `ro` | Read-only (default) | `$obj->name()` (getter) |
| `rw` | Read/write | `$obj->name()` (getter), `$obj->set_name($val)` (setter) |

**Options (in parentheses):**

| Option | Description |
|--------|-------------|
| `required` | Must be provided in constructor |
| `lazy` | Default computed on first access |
| `builder => "method"` | Method to call for lazy default |

Options can be combined: `(required)`, `(lazy)`, `(lazy, builder => "_build_name")`

**Examples:**
```strada
package Person;
has ro str $name (required);              # Read-only, required
has rw int $age = 0;                      # Read/write, default 0
has rw str $email;                        # Read/write, no default
has ro str $id (lazy, builder => "_build_id");  # Lazy with builder
```

#### 16.6.2 Inheritance (`extends`)

`extends` is an alias for `inherit`. It sets up inheritance and includes parent attributes in the auto-generated constructor.

```strada
extends PARENT_NAME;
extends Parent1, Parent2;
```

**Example:**
```strada
package Animal;
has ro str $species (required);

package Dog;
extends Animal;
has ro str $name (required);
```

#### 16.6.3 Role Composition (`with`)

`with` is an alias for `inherit`, used to compose roles into a class:

```strada
with ROLE_NAME;
with Role1, Role2;
```

**Example:**
```strada
package Printable;
func to_string(scalar $self) str { return "object"; }

package MyClass;
extends Base;
with Printable;
```

#### 16.6.4 Auto-Generated Constructor

When a package uses `has` declarations, a constructor `Package::new(...)` is automatically generated unless an explicit `new()` function is defined. The constructor takes named arguments as alternating key-value pairs:

```strada
my scalar $obj = Dog::new("name", "Rex", "species", "dog", "age", 3);
```

The constructor:
1. Parses the named arguments into a hash
2. Sets each attribute from the arguments, falling back to the default value (using `//`)
3. Blesses the hash into the package
4. Includes attributes from all parent packages in the `extends` chain

#### 16.6.5 Method Modifiers

Method modifiers add behavior before, after, or around an existing method.

**`before` -- runs before the named method:**
```strada
before "METHOD_NAME" func(PARAMS) RETURN_TYPE {
    BODY
}
```

**`after` -- runs after the named method:**
```strada
after "METHOD_NAME" func(PARAMS) RETURN_TYPE {
    BODY
}
```

**`around` -- wraps the method, receives the original as `$orig`:**
```strada
around "METHOD_NAME" func(scalar $self, scalar $orig, scalar ...@args) RETURN_TYPE {
    # Call original: $orig->($self, ...@args)
    BODY
}
```

The `$orig` parameter is a callable reference to the original method. You must invoke it explicitly.

**Example:**
```strada
package Dog;
extends Animal;
has ro str $name (required);

before "bark" func(scalar $self) void {
    say("[preparing to bark]");
}

func bark(scalar $self) void {
    say($self->name() . " barks!");
}

after "bark" func(scalar $self) void {
    say("[done barking]");
}
```

Calling `$dog->bark()` produces:
```
[preparing to bark]
Rex barks!
[done barking]
```

### 16.7 Operator Overloading

Strada supports operator overloading via `use overload`, mapping operator strings to method names.

#### 16.7.1 Syntax

```strada
use overload
    "OP" => "method_name",
    "OP2" => "method_name2";
```

#### 16.7.2 Supported Operators

| Category | Operators |
|----------|-----------|
| Arithmetic | `+`, `-`, `*`, `/`, `%`, `**` |
| String | `.` (concatenation) |
| Stringify | `""` (automatic string conversion) |
| Unary | `neg` (unary minus), `!`, `bool` |
| Numeric comparison | `==`, `!=`, `<`, `>`, `<=`, `>=`, `<=>` |
| String comparison | `eq`, `ne`, `lt`, `gt`, `le`, `ge`, `cmp` |

#### 16.7.3 Handler Signatures

**Binary operators** receive `(scalar $self, scalar $other, int $reversed)`:

```strada
func add(scalar $self, scalar $other, int $reversed) scalar {
    # $reversed is 1 if $self was the right operand
    return Vector::new($self->{"x"} + $other->{"x"},
                       $self->{"y"} + $other->{"y"});
}
```

**Unary operators** receive `(scalar $self)`:

```strada
func negate(scalar $self) scalar {
    return Vector::new(-$self->{"x"}, -$self->{"y"});
}
```

**Stringify** (`""`) receives `(scalar $self)` and returns `str`:

```strada
func to_str(scalar $self) str {
    return "(" . $self->{"x"} . ", " . $self->{"y"} . ")";
}
```

#### 16.7.4 Dispatch Rules

1. Left operand checked first — if blessed with the operator overloaded, called with `$reversed = 0`
2. Right operand checked second — called with `$reversed = 1`
3. If neither operand is overloaded, default behavior applies

#### 16.7.5 Zero Overhead

- No `use overload` in the program: generated C is identical to code without overloading
- Typed operands (`int`, `num`, `str`): inline C generated directly, no dispatch
- Runtime dispatch only when at least one operand is `scalar` and the operator is overloaded

#### 16.7.6 Example

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
```

---

## 17. Closures

### 17.1 Syntax

```strada
my scalar $closure = func (PARAMS) { BODY };
my scalar $closure = func (PARAMS) TYPE { BODY };
```

### 17.2 Calling

```strada
$closure->(ARGS);
```

### 17.3 Capturing

Variables from enclosing scope are captured by reference:

```strada
my int $x = 10;
my scalar $f = func () { return $x; };
$x = 20;
say($f->());  # 20
```

---

## 18. Multithreading

### 18.1 Thread Functions

| Function | Description |
|----------|-------------|
| `thread::create($closure)` | Create and start thread |
| `thread::join($thread)` | Wait for completion |
| `thread::detach($thread)` | Detach thread |
| `thread::self()` | Get current thread ID |

### 18.2 Mutex Functions

| Function | Description |
|----------|-------------|
| `thread::mutex_new()` | Create mutex |
| `thread::mutex_lock($m)` | Lock mutex |
| `thread::mutex_trylock($m)` | Try to lock (non-blocking) |
| `thread::mutex_unlock($m)` | Unlock mutex |
| `thread::mutex_destroy($m)` | Destroy mutex |

### 18.3 Condition Variables

| Function | Description |
|----------|-------------|
| `thread::cond_new()` | Create condition |
| `thread::cond_wait($c, $m)` | Wait on condition |
| `thread::cond_signal($c)` | Signal one waiter |
| `thread::cond_broadcast($c)` | Signal all waiters |
| `thread::cond_destroy($c)` | Destroy condition |

---

## 19. Async/Await

Strada provides async/await for concurrent programming with a thread pool backend.

### 19.1 Async Functions

```strada
async func name(params) return_type {
    # Function body runs in thread pool
    return value;
}
```

When called, async functions immediately return a Future while the work executes on a background thread.

### 19.2 Await

```strada
my scalar $future = async_function(args);
my type $result = await $future;  # Blocks until complete
```

### 19.3 Future Functions

| Function | Description |
|----------|-------------|
| `async::all(\@futures)` | Wait for all, return array |
| `async::race(\@futures)` | Wait for first, cancel others |
| `async::timeout($f, $ms)` | Await with timeout |
| `async::cancel($f)` | Request cancellation |
| `async::is_done($f)` | Non-blocking check |
| `async::is_cancelled($f)` | Check cancelled |
| `async::pool_init($n)` | Init pool with N workers |
| `async::pool_shutdown()` | Shutdown pool |

### 19.4 Error Handling

Exceptions in async functions propagate through await:

```strada
async func fail_async() int {
    throw "async error";
}

try {
    await fail_async();
} catch ($e) {
    say("Caught: " . $e);
}
```

### 19.5 Examples

```strada
# Parallel execution
my scalar $a = compute(10);
my scalar $b = compute(20);
my int $r1 = await $a;
my int $r2 = await $b;

# Wait for all
my array @futures = (compute(1), compute(2), compute(3));
my array @results = async::all(\@futures);

# Race
my str $winner = async::race(\@futures);

# Timeout
try {
    my str $r = async::timeout($future, 100);
} catch ($e) {
    say("Timed out");
}
```

### 19.6 Channels

Channels provide thread-safe message passing between async tasks.

```strada
# Create channel (unbounded or bounded)
my scalar $ch = async::channel();      # Unbounded
my scalar $ch = async::channel(10);    # Capacity of 10

# Send and receive
async::send($ch, $value);              # Blocks if full
my scalar $v = async::recv($ch);       # Blocks if empty

# Non-blocking variants
async::try_send($ch, $value);          # Returns 0/1
my scalar $v = async::try_recv($ch);   # Returns undef if empty

# Close and check
async::close($ch);
async::is_closed($ch);
async::len($ch);
```

| Function | Description |
|----------|-------------|
| `async::channel()` | Create unbounded channel |
| `async::channel($n)` | Create bounded channel |
| `async::send($ch, $v)` | Send (blocks if full) |
| `async::recv($ch)` | Receive (blocks if empty) |
| `async::try_send($ch, $v)` | Non-blocking send |
| `async::try_recv($ch)` | Non-blocking receive |
| `async::close($ch)` | Close channel |
| `async::is_closed($ch)` | Check if closed |
| `async::len($ch)` | Get queue length |

### 19.7 Mutexes

Mutexes protect critical sections.

```strada
my scalar $m = async::mutex();
async::lock($m);           # Acquire (blocking)
# ... critical section ...
async::unlock($m);         # Release

async::try_lock($m);       # Non-blocking (0=success)
async::mutex_destroy($m);  # Clean up
```

| Function | Description |
|----------|-------------|
| `async::mutex()` | Create mutex |
| `async::lock($m)` | Acquire lock |
| `async::unlock($m)` | Release lock |
| `async::try_lock($m)` | Non-blocking lock |
| `async::mutex_destroy($m)` | Destroy mutex |

### 19.8 Atomics

Lock-free integer operations.

```strada
my scalar $a = async::atomic(0);       # Create
async::atomic_load($a);                # Read
async::atomic_store($a, 100);          # Write
async::atomic_add($a, 10);             # Add, returns OLD
async::atomic_sub($a, 5);              # Sub, returns OLD
async::atomic_inc($a);                 # Inc, returns NEW
async::atomic_dec($a);                 # Dec, returns NEW
async::atomic_cas($a, $exp, $new);     # CAS (returns 1 if swapped)
```

| Function | Description |
|----------|-------------|
| `async::atomic($n)` | Create atomic |
| `async::atomic_load($a)` | Read value |
| `async::atomic_store($a, $v)` | Write value |
| `async::atomic_add($a, $d)` | Add, return OLD |
| `async::atomic_sub($a, $d)` | Subtract, return OLD |
| `async::atomic_inc($a)` | Increment, return NEW |
| `async::atomic_dec($a)` | Decrement, return NEW |
| `async::atomic_cas($a, $e, $n)` | Compare-and-swap |

---

## 20. Foreign Function Interface

### 20.1 Loading Libraries

```strada
my int $lib = core::dl_open("libfoo.so");
my int $func = core::dl_sym($lib, "function_name");
```

### 20.2 Calling Functions

```strada
my int $result = core::dl_call_int($func, [$arg1, $arg2]);
my num $result = core::dl_call_num($func, [$arg1]);
my str $result = core::dl_call_str($func, $arg);
core::dl_call_void($func, [$args]);
```

### 20.3 StradaValue Passthrough

For C functions expecting StradaValue*:

```strada
my int $result = core::dl_call_int_sv($func, [$sv1, $sv2]);
```

---

## 20. Built-in Functions

### 20.1 Core Functions

**Output:**
- `say($val)` - Print with newline
- `print($val)` - Print without newline
- `printf($fmt, ...)` - Formatted print
- `warn($msg)` - Print to stderr
- `die($msg)` - Fatal error

**Type checking:**
- `defined($val)` - Check if defined
- `typeof($val)` - Get type name
- `is_ref($val)` - Check if reference
- `reftype($ref)` - Get reference type

**Debugging:**
- `dumper($val)` - Data::Dumper-style output

### 20.2 core:: / core:: Namespace

The `core::` namespace is the preferred alias for `core::`. All functions listed below can be called with either prefix. At compile time, `core::` is normalized to `core::` with zero runtime overhead.

```strada
# These are equivalent:
my int $pid = core::getpid();    # Preferred
my int $pid = core::getpid();     # Also works
```

**Files:**
- `core::open($path, $mode)` - Open file, returns filehandle
- `core::open(\$var, $mode)` - Open in-memory handle (ref-style, writeback on close)
- `core::open_str($content, $mode)` - Open in-memory handle from string
- `core::str_from_fh($fh)` - Extract string from memstream (without closing)
- `core::close($fh)` - Close file
- `core::readline($fh)` - Read line
- `core::slurp($path)` - Read entire file
- `core::spew($path, $data)` - Write file

**Default Filehandle:**
- `select($fh)` - Set default output filehandle for `print`/`say`, returns previous default. Zero overhead when never called.

**Diamond Operator and Filehandle I/O:**

The diamond operator `<$fh>` reads a line from a filehandle (similar to Perl):

```strada
my scalar $fh = core::open("input.txt", "r");
my str $line = <$fh>;                    # Read one line
while (defined($line)) {
    say($line);
    $line = <$fh>;
}
core::close($fh);
```

Print and say work with filehandles as the first argument:

```strada
my scalar $out = core::open("output.txt", "w");
say($out, "Line with newline");          # Writes text + \n
print($out, "No automatic newline");     # Writes text only
core::close($out);
```

Both diamond operator and print/say also work with sockets:

```strada
my scalar $sock = core::socket_client("localhost", 80);
say($sock, "GET / HTTP/1.0");
say($sock, "Host: localhost");
say($sock, "");
my str $line = <$sock>;                  # Read response line by line
while (defined($line)) {
    say($line);
    $line = <$sock>;
}
core::socket_close($sock);
```

Socket readline automatically strips `\r` characters for clean CRLF handling.

**Process:**
- `core::fork()` - Fork process
- `core::wait()` - Wait for child
- `core::exec($cmd)` - Replace process
- `core::system($cmd)` - Run command
- `core::getpid()` - Get process ID
- `core::exit($code)` - Exit process

**Time:**
- `core::time()` - Unix timestamp
- `core::sleep($secs)` - Sleep seconds
- `core::usleep($usecs)` - Sleep microseconds

**Environment:**
- `core::getenv($name)` - Get environment variable
- `core::setenv($name, $val)` - Set environment variable

**Binary/Bytes:**
- `core::ord_byte($s)` - First byte as integer (0-255)
- `core::get_byte($s, $pos)` - Byte at position
- `core::set_byte($s, $pos, $val)` - Set byte, returns new string
- `core::byte_length($s)` - Byte count
- `core::byte_substr($s, $start, $len)` - Substring by bytes
- `core::pack($fmt, @vals)` - Pack values to binary string
- `core::unpack($fmt, $s)` - Unpack binary string to array

### 20.3 math:: Namespace

- `math::sin($x)`, `math::cos($x)`, `math::tan($x)`
- `math::sqrt($x)`, `math::pow($x, $y)`
- `math::abs($x)`, `math::floor($x)`, `math::ceil($x)`
- `math::log($x)`, `math::exp($x)`
- `math::rand()` - Random 0.0-1.0

### 20.4 thread:: Namespace

See [Multithreading](#18-multithreading).

---

## 21. Magic Variables

| Variable | Description |
|----------|-------------|
| `@ARGV` | Command-line arguments |
| `$ARGC` | Argument count |
| `$_` | Default variable in map/grep |
| `$a`, `$b` | Sort comparison variables |
| `__PACKAGE__` | Current package name (runtime) |
| `__FILE__` | Current file name |
| `__LINE__` | Current line number |
| `::func()` | Call func in current package (compile-time) |
| `.::func()` | Alternate syntax for above |
| `__PACKAGE__::func()` | Explicit form of above |

---

## 22. Reserved Words

The following words are reserved and cannot be used as identifiers:

```
after       array       around      before      break
case        catch       continue    default     delete
do          each        else        elsif       exists
extends     extern      for         foreach     func
goto        has         hash        if          inherit
int         keys        last        local       map
my          next        num         our         package
pop         print       push        return      say
scalar      select      shift       sort        splice
str         sub         switch      throw       tie
tied        try         undef       untie       unshift
unless      until       use         values      void
while       with
```

---

## Appendix A: Grammar Summary

```
program         := (package_decl | use_decl | inherit_decl | extends_decl
                 |  with_decl | has_decl | method_modifier | func_def | stmt)*

package_decl    := 'package' IDENT ';'
use_decl        := 'use' qualified_name ('qw(' IDENT* ')')? ';'
inherit_decl    := 'inherit' IDENT (',' IDENT)* ';'

# Moose-style OOP declarations
extends_decl    := 'extends' IDENT (',' IDENT)* ';'
with_decl       := 'with' IDENT (',' IDENT)* ';'
has_decl        := 'has' access? type '$' IDENT ('=' expr)? has_options? ';'
access          := 'ro' | 'rw'
has_options     := '(' has_option (',' has_option)* ')'
has_option      := 'required' | 'lazy' | 'builder' '=>' STRING

method_modifier := ('before' | 'after' | 'around') STRING 'func' '(' params? ')' type? block

func_def        := 'func' IDENT '(' params? ')' type block

params          := param (',' param)*
param           := type SIGIL IDENT ('=' expr)?

type            := 'int' | 'num' | 'str' | 'scalar' | 'array' | 'hash' | 'void'

block           := '{' stmt* '}'

stmt            := var_decl | local_decl | if_stmt | while_stmt | for_stmt
                 | foreach_stmt | switch_stmt | try_stmt | return_stmt
                 | last_stmt | next_stmt | goto_stmt | label_stmt
                 | tie_stmt | expr_stmt | block

var_decl        := 'my' type SIGIL IDENT ('=' expr)? ';'
local_decl      := 'local' SIGIL IDENT '=' expr ';'
tie_stmt        := 'tie' '(' '%' IDENT ',' STRING ')' ';'

expr            := assignment | ternary | logical | comparison | arithmetic
                 | unary | postfix | primary
```

---

## Appendix B: Operator Quick Reference

| Category | Operators |
|----------|-----------|
| Arithmetic | `+ - * / % **` |
| String | `. x` |
| Comparison (num) | `== != < > <= >= <=>` |
| Comparison (str) | `eq ne lt gt le ge cmp` |
| Logical | `&& || !` |
| Bitwise | `& | ^ ~ << >>` |
| Assignment | `= += -= *= /= %= .= &= |=` |
| Reference | `\ -> @{} %{} ${}` |
| Regex | `=~ !~ s/// s///e tr/// y///` |
| Other | `? : .. ,` |

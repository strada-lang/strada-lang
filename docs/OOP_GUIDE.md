# Strada Object-Oriented Programming Guide

Strada supports Perl-style object-oriented programming with blessed references, inheritance (including multiple inheritance), and polymorphism.

## Table of Contents

1. [OOP Basics](#1-oop-basics)
2. [Creating Classes](#2-creating-classes)
3. [Constructors](#3-constructors)
4. [Methods](#4-methods)
5. [Inheritance](#5-inheritance)
6. [Multiple Inheritance](#6-multiple-inheritance)
7. [SUPER:: Calls](#7-super-calls)
8. [DESTROY Destructors](#8-destroy-destructors)
9. [Type Checking](#9-type-checking)
10. [UNIVERSAL Methods](#10-universal-methods)
11. [Operator Overloading](#11-operator-overloading)
12. [Design Patterns](#12-design-patterns)
13. [Best Practices](#13-best-practices)
14. [Moose-Style Declarative OOP](#14-moose-style-declarative-oop)
15. [Comparison: Manual OOP vs Moose-Style OOP](#15-comparison-manual-oop-vs-moose-style-oop)
16. [Tied Hashes](#16-tied-hashes-tie--untie--tied)

---

## 1. OOP Basics

### The Perl OOP Model

Strada uses Perl's OOP model:
- Objects are blessed hash references
- Methods are regular functions with `$self` as first argument
- Inheritance uses `@ISA` equivalent (`inherit`)
- No enforced encapsulation (convention-based privacy)

### Key Concepts

| Concept | Strada Implementation |
|---------|----------------------|
| Class | Package with constructor and methods |
| Object | Blessed hash reference |
| Method | Function taking `$self` |
| Attribute | Hash key on `$self` |
| Inheritance | `inherit` statement/function |
| Type check | `isa()` function |

---

## 2. Creating Classes

### Basic Class Structure

```strada
package ClassName;

# Constructor
func ClassName_new(PARAMS) scalar {
    my hash %self = ();
    # Initialize attributes
    $self{"attr"} = $value;
    return bless(\%self, "ClassName");
}

# Methods
func ClassName_method(scalar $self, PARAMS) RETURN_TYPE {
    # Access attributes
    my scalar $attr = $self->{"attr"};
    # ...
}
```

### Complete Example

```strada
package Person;

func Person_new(str $name, int $age) scalar {
    my hash %self = ();
    $self{"name"} = $name;
    $self{"age"} = $age;
    $self{"created"} = core::time();
    return bless(\%self, "Person");
}

func Person_get_name(scalar $self) str {
    return $self->{"name"};
}

func Person_get_age(scalar $self) int {
    return $self->{"age"};
}

func Person_set_age(scalar $self, int $age) void {
    if ($age >= 0) {
        $self->{"age"} = $age;
    }
}

func Person_greet(scalar $self) void {
    say("Hello, I'm " . $self->{"name"} .
        " and I'm " . $self->{"age"} . " years old.");
}

func Person_have_birthday(scalar $self) void {
    $self->{"age"} = $self->{"age"} + 1;
    say("Happy birthday! Now " . $self->{"age"});
}

package main;

func main() int {
    my scalar $alice = Person_new("Alice", 30);
    Person_greet($alice);
    Person_have_birthday($alice);

    return 0;
}
```

---

## 3. Constructors

### Basic Constructor

```strada
func ClassName_new() scalar {
    my hash %self = ();
    return bless(\%self, "ClassName");
}
```

### Constructor with Parameters

```strada
func Point_new(int $x, int $y) scalar {
    my hash %self = ();
    $self{"x"} = $x;
    $self{"y"} = $y;
    return bless(\%self, "Point");
}
```

### Constructor with Defaults

```strada
func Config_new(str $name, int $debug = 0, int $verbose = 0) scalar {
    my hash %self = ();
    $self{"name"} = $name;
    $self{"debug"} = $debug;
    $self{"verbose"} = $verbose;
    return bless(\%self, "Config");
}

# Usage
my scalar $cfg1 = Config_new("app");           # defaults
my scalar $cfg2 = Config_new("app", 1);        # debug on
my scalar $cfg3 = Config_new("app", 1, 1);     # both on
```

### Factory Constructor

```strada
func Shape_create(str $type, array @args) scalar {
    if ($type eq "circle") {
        return Circle_new($args[0]);
    }
    if ($type eq "rectangle") {
        return Rectangle_new($args[0], $args[1]);
    }
    return undef;
}
```

### Cloning

```strada
func Person_clone(scalar $self) scalar {
    my hash %new = ();
    $new{"name"} = $self->{"name"};
    $new{"age"} = $self->{"age"};
    return bless(\%new, blessed($self));
}
```

---

## 4. Methods

### Instance Methods

```strada
# Method with $self
func Counter_increment(scalar $self) void {
    $self->{"count"} = $self->{"count"} + 1;
}

# Method returning value
func Counter_get(scalar $self) int {
    return $self->{"count"};
}
```

### Accessors (Getters/Setters)

```strada
# Getter
func Person_name(scalar $self) str {
    return $self->{"name"};
}

# Setter
func Person_set_name(scalar $self, str $name) void {
    $self->{"name"} = $name;
}

# Combined getter/setter
func Person_age(scalar $self, int $new_age = -1) int {
    if ($new_age >= 0) {
        $self->{"age"} = $new_age;
    }
    return $self->{"age"};
}
```

### Method Chaining

```strada
func Builder_set_name(scalar $self, str $name) scalar {
    $self->{"name"} = $name;
    return $self;  # Return $self for chaining
}

func Builder_set_value(scalar $self, int $value) scalar {
    $self->{"value"} = $value;
    return $self;
}

func Builder_build(scalar $self) scalar {
    return {
        name => $self->{"name"},
        value => $self->{"value"}
    };
}

# Usage
my scalar $result = Builder_new()
    ->set_name("test")
    ->set_value(42)
    ->build();
```

Note: Method chaining requires using the arrow syntax on the return value.

### Private Methods (Convention)

```strada
# Private methods start with underscore
func Person__validate_age(scalar $self, int $age) int {
    if ($age < 0) {
        return 0;
    }
    if ($age > 150) {
        return 0;
    }
    return 1;
}

func Person_set_age(scalar $self, int $age) int {
    if (Person__validate_age($self, $age)) {
        $self->{"age"} = $age;
        return 1;
    }
    return 0;
}
```

### Calling Package Functions Without Repeating the Package Name

Use `::func()` to call functions in the current package without repeating the package name:

```strada
package Person;

func validate_age(int $age) int {
    if ($age < 0 || $age > 150) {
        return 0;
    }
    return 1;
}

func set_age(scalar $self, int $age) int {
    # Use :: to call validate_age in current package
    if (::validate_age($age)) {      # Resolves to Person_validate_age
        $self->{"age"} = $age;
        return 1;
    }
    return 0;
}

func complex_operation(scalar $self) void {
    ::helper1();                     # Person_helper1
    ::helper2($self);                # Person_helper2
    .::helper3();                    # Alternate syntax
    __PACKAGE__::helper4();          # Explicit form
}
```

Three equivalent syntaxes:
- `::func()` — Preferred shorthand
- `.::func()` — Alternate shorthand
- `__PACKAGE__::func()` — Explicit form

All resolve to `PackageName_func()` at **compile time**.

### Variadic Methods

Methods can accept variable numbers of arguments using the spread operator syntax:

```strada
package Calculator;

# Variadic method - sum all numbers
func Calculator_sum(scalar $self, int ...@nums) int {
    my int $total = 0;
    foreach my int $n (@nums) {
        $total = $total + $n;
    }
    return $total;
}

# Fixed params + variadic
func Calculator_sum_with_base(scalar $self, int $base, int ...@nums) int {
    my int $total = $base;
    foreach my int $n (@nums) {
        $total = $total + $n;
    }
    return $total;
}

# Usage
my scalar $calc = Calculator::new();
$calc->sum(1, 2, 3);                    # 6
$calc->sum(10, 20, 30, 40, 50);         # 150

# Using spread operator
my array @values = (100, 200, 300);
$calc->sum(...@values);                 # 600
$calc->sum(1, ...@values, 99);          # 700

# With fixed param
$calc->sum_with_base(1000, 1, 2, 3);    # 1006
$calc->sum_with_base(500, ...@values);  # 1100
```

---

## 5. Inheritance

### Single Inheritance

**File-level syntax (single-package files):**

```strada
package Dog;
inherit Animal;

func Dog_new(str $name) scalar {
    # ...
}
```

**Function syntax (multi-package files):**

```strada
package Dog;

func Dog_init() void {
    inherit("Dog", "Animal");
}

func Dog_new(str $name) scalar {
    my hash %self = ();
    $self{"name"} = $name;
    $self{"species"} = "dog";
    return bless(\%self, "Dog");
}
```

### Complete Inheritance Example

```strada
package Animal;

func Animal_new(str $name, str $species) scalar {
    my hash %self = ();
    $self{"name"} = $name;
    $self{"species"} = $species;
    return bless(\%self, "Animal");
}

func Animal_speak(scalar $self) void {
    say($self->{"name"} . " makes a sound");
}

func Animal_get_name(scalar $self) str {
    return $self->{"name"};
}

package Dog;

func Dog_init() void {
    inherit("Dog", "Animal");
}

func Dog_new(str $name) scalar {
    my hash %self = ();
    $self{"name"} = $name;
    $self{"species"} = "dog";
    $self{"tricks"} = 0;
    return bless(\%self, "Dog");
}

# Override Animal_speak
func Dog_speak(scalar $self) void {
    say($self->{"name"} . " says: Woof!");
}

# New method
func Dog_learn_trick(scalar $self) void {
    $self->{"tricks"} = $self->{"tricks"} + 1;
    say($self->{"name"} . " knows " . $self->{"tricks"} . " tricks!");
}

package main;

func main() int {
    Dog_init();  # Set up inheritance

    my scalar $dog = Dog_new("Rex");

    # Inherited method
    say("Name: " . Animal_get_name($dog));

    # Overridden method
    Dog_speak($dog);

    # New method
    Dog_learn_trick($dog);

    # Type checks
    say("isa Dog: " . isa($dog, "Dog"));
    say("isa Animal: " . isa($dog, "Animal"));

    return 0;
}
```

---

## 6. Multiple Inheritance

### Syntax

**File-level (comma-separated):**

```strada
package Duck;
inherit Animal, Flyable, Swimmable;
```

**Function syntax:**

```strada
func Duck_init() void {
    inherit("Duck", "Animal");
    inherit("Duck", "Flyable");
    inherit("Duck", "Swimmable");
}
```

### Complete Example

```strada
package Printable;

func Printable_print(scalar $self) void {
    say("[Printable] " . $self->{"content"});
}

package Saveable;

func Saveable_save(scalar $self, str $filename) void {
    core::spew($filename, $self->{"content"});
    say("Saved to " . $filename);
}

func Saveable_load(scalar $self, str $filename) void {
    $self->{"content"} = core::slurp($filename);
    say("Loaded from " . $filename);
}

package Serializable;

func Serializable_serialize(scalar $self) str {
    return "CONTENT:" . $self->{"content"};
}

func Serializable_deserialize(scalar $self, str $data) void {
    if ($data =~ /^CONTENT:(.*)/) {
        my array @parts = capture($data, "^CONTENT:(.*)");
        $self->{"content"} = $parts[0];
    }
}

package Document;

func Document_init() void {
    inherit("Document", "Printable");
    inherit("Document", "Saveable");
    inherit("Document", "Serializable");
}

func Document_new(str $content) scalar {
    my hash %self = ();
    $self{"content"} = $content;
    $self{"created"} = core::time();
    return bless(\%self, "Document");
}

func Document_set_content(scalar $self, str $content) void {
    $self->{"content"} = $content;
}

package main;

func main() int {
    Document_init();

    my scalar $doc = Document_new("Hello, World!");

    # Check all inheritances
    say("isa Printable: " . isa($doc, "Printable"));
    say("isa Saveable: " . isa($doc, "Saveable"));
    say("isa Serializable: " . isa($doc, "Serializable"));

    # Use methods from different parents
    Printable_print($doc);
    say("Serialized: " . Serializable_serialize($doc));

    return 0;
}
```

### Method Resolution Order (MRO)

When multiple parents have the same method, the first parent in the inheritance list takes precedence:

```strada
package A;
func A_method() str { return "A"; }

package B;
func B_method() str { return "B"; }

package C;
func C_init() void {
    inherit("C", "A");  # A comes first
    inherit("C", "B");
}

# When looking up 'method' on C:
# 1. Check C_method - not found
# 2. Check A_method - found, use A's implementation
# 3. B_method would only be checked if A didn't have it
```

---

## 7. SUPER:: Calls

### Calling Parent Methods

```strada
package Animal;

func Animal_speak(scalar $self) void {
    say($self->{"name"} . " makes a sound");
}

package Dog;

func Dog_init() void {
    inherit("Dog", "Animal");
}

func Dog_speak(scalar $self) void {
    # Call parent's speak first
    SUPER::speak($self);
    # Then add our own behavior
    say($self->{"name"} . " says: Woof!");
}
```

### SUPER in Constructors

```strada
package Employee;

func Employee_init() void {
    inherit("Employee", "Person");
}

func Employee_new(str $name, int $age, str $title) scalar {
    # Create base object
    my hash %self = ();
    $self{"name"} = $name;
    $self{"age"} = $age;
    $self{"title"} = $title;
    return bless(\%self, "Employee");
}

# Or call parent constructor-like initialization
func Employee_init_from_person(scalar $person, str $title) scalar {
    my hash %self = ();
    $self{"name"} = $person->{"name"};
    $self{"age"} = $person->{"age"};
    $self{"title"} = $title;
    return bless(\%self, "Employee");
}
```

### SUPER with Multiple Inheritance

With multiple inheritance, SUPER:: calls the first parent that has the method:

```strada
package Duck;

func Duck_init() void {
    inherit("Duck", "Bird");    # First parent
    inherit("Duck", "Swimmer"); # Second parent
}

func Duck_move(scalar $self) void {
    # Calls Bird_move if it exists, otherwise Swimmer_move
    SUPER::move($self);
    say("Duck is moving");
}
```

---

## 8. DESTROY Destructors

### Basic Destructor

```strada
package FileHandle;

func FileHandle_new(str $filename) scalar {
    my hash %self = ();
    $self{"filename"} = $filename;
    $self{"handle"} = core::open($filename, "r");
    return bless(\%self, "FileHandle");
}

func FileHandle_DESTROY(scalar $self) void {
    if (defined($self->{"handle"})) {
        core::close($self->{"handle"});
        say("Closed file: " . $self->{"filename"});
    }
}
```

### Destructor Chain

```strada
package Animal;

func Animal_DESTROY(scalar $self) void {
    say("Animal " . $self->{"name"} . " being destroyed");
}

package Dog;

func Dog_init() void {
    inherit("Dog", "Animal");
}

func Dog_DESTROY(scalar $self) void {
    say("Dog cleanup for " . $self->{"name"});
    # Call parent destructor
    SUPER::DESTROY($self);
}

package main;

func main() int {
    Dog_init();

    {
        my scalar $dog = Dog_new("Rex");
        # $dog goes out of scope here
    }
    # Output:
    #   Dog cleanup for Rex
    #   Animal Rex being destroyed

    return 0;
}
```

### Resource Management

```strada
package Connection;

func Connection_new(str $host, int $port) scalar {
    my hash %self = ();
    $self{"host"} = $host;
    $self{"port"} = $port;
    $self{"socket"} = core::socket_client($host, $port);
    $self{"connected"} = defined($self->{"socket"});
    return bless(\%self, "Connection");
}

func Connection_send(scalar $self, str $data) int {
    if (!$self->{"connected"}) {
        return 0;
    }
    return core::socket_send($self->{"socket"}, $data);
}

func Connection_DESTROY(scalar $self) void {
    if ($self->{"connected"}) {
        core::socket_close($self->{"socket"});
        say("Connection to " . $self->{"host"} . " closed");
    }
}
```

---

## 9. Type Checking

### The isa() Function

```strada
my scalar $obj = Dog_new("Rex");

# Direct type check
if (isa($obj, "Dog")) {
    say("It's a Dog");
}

# Inheritance check
if (isa($obj, "Animal")) {
    say("It's an Animal (or subclass)");
}

# Negative check
if (!isa($obj, "Cat")) {
    say("It's not a Cat");
}
```

### The blessed() Function

```strada
my scalar $obj = Dog_new("Rex");

# Get package name
my str $pkg = blessed($obj);
say("Package: " . $pkg);  # "Dog"

# Check if blessed at all
if (length(blessed($obj)) > 0) {
    say("Object is blessed");
}
```

### The can() Function

```strada
my scalar $obj = Dog_new("Rex");

# Check if method exists
if (can($obj, "speak")) {
    say("Can speak");
}

if (!can($obj, "fly")) {
    say("Cannot fly");
}
```

### Type-Safe Method Dispatch

```strada
func feed(scalar $animal) void {
    if (!isa($animal, "Animal")) {
        die("Expected an Animal");
    }

    if (isa($animal, "Dog")) {
        say("Giving dog food");
    } elsif (isa($animal, "Cat")) {
        say("Giving cat food");
    } else {
        say("Giving generic food");
    }
}
```

---

## 10. UNIVERSAL Methods

Strada supports UNIVERSAL methods that work on any object:

### Using isa as a Method

```strada
my scalar $dog = Dog_new("Rex");

# Function style
if (isa($dog, "Animal")) { ... }

# Method style
if ($dog->isa("Animal")) { ... }
```

### Using can as a Method

```strada
my scalar $dog = Dog_new("Rex");

# Function style
if (can($dog, "speak")) { ... }

# Method style
if ($dog->can("speak")) { ... }
```

---

## 11. AUTOLOAD

When a method is called on an object but doesn't exist in its package (or any parent), Strada normally dies with an error. The `AUTOLOAD` method provides a fallback — if defined, it catches calls to undefined methods and lets the class handle them dynamically.

### Basic AUTOLOAD

```strada
package Proxy;

func new(str $target) scalar {
    my hash %self = ();
    $self{"target"} = $target;
    return bless(\%self, "Proxy");
}

func AUTOLOAD(scalar $self, str $method, scalar ...@args) scalar {
    say("Called undefined method: " . $method);
    if (scalar(@args) > 0) {
        say("  with arg: " . $args[0]);
    }
    return "handled";
}

package main;

func main() int {
    my scalar $p = Proxy::new("backend");

    $p->anything();          # "Called undefined method: anything"
    $p->compute("data");     # "Called undefined method: compute" / "  with arg: data"

    return 0;
}
```

### How It Works

- When `$obj->method(args)` fails to find `method` in the object's package, the runtime looks for `AUTOLOAD` instead
- AUTOLOAD receives three parameters:
  - `$self` — the object (standard first parameter)
  - `$method` — the name of the method that was called but not found
  - `...@args` — all original arguments passed to the missing method
- If no AUTOLOAD is found either, the program dies with the usual error

### AUTOLOAD and Real Methods

Real methods always take priority. AUTOLOAD is only invoked when no matching method exists:

```strada
package Cache;

func new() scalar {
    my hash %self = ();
    return bless(\%self, "Cache");
}

func get(scalar $self, str $key) scalar {
    return $self->{$key};
}

func AUTOLOAD(scalar $self, str $method, scalar ...@args) scalar {
    # Only called for methods other than "get"
    say("No such method: " . $method);
    return undef;
}
```

### Inherited AUTOLOAD

AUTOLOAD is looked up through the inheritance chain, just like regular methods. A parent's AUTOLOAD catches undefined calls on child objects:

```strada
package Base;

func new() scalar {
    my hash %self = ();
    return bless(\%self, "Base");
}

func AUTOLOAD(scalar $self, str $method, scalar ...@args) scalar {
    return "autoloaded: " . $method;
}

package Child;

func new() scalar {
    my hash %self = ();
    inherit("Child", "Base");
    return bless(\%self, "Child");
}

func child_method(scalar $self) str {
    return "real method";
}

package main;

func main() int {
    my scalar $c = Child::new();
    say($c->child_method());     # "real method"
    say($c->anything());         # "autoloaded: anything" (from Base)
    return 0;
}
```

### AUTOLOAD and can()

`$obj->can("method")` returns 0 for undefined methods even if AUTOLOAD exists. This is by design — `can()` reports whether a specific method is explicitly defined, not whether the call would succeed via AUTOLOAD:

```strada
my scalar $p = Proxy::new("test");
say($p->can("real_method"));  # 1
say($p->can("missing"));      # 0 (even though AUTOLOAD would handle it)
```

### Use Cases

- **Proxy/Delegation** — Forward method calls to a wrapped object
- **Dynamic Getters/Setters** — Auto-generate accessors based on hash keys
- **Lazy Loading** — Load functionality on first use
- **Logging/Debugging** — Intercept all method calls for instrumentation

---

## 11.5 Dynamic Method Dispatch

Strada supports calling methods where the method name is stored in a variable, using the `$obj->$method()` syntax. This is useful for dispatch tables, plugin systems, and dynamic APIs.

### Basic Usage

```strada
package Animal;
has ro str $species (required);

func speak(scalar $self) str {
    return "I am a " . $self->species();
}

package main;

func main() int {
    my scalar $a = Animal::new("species", "cat");

    my str $method = "speak";
    my str $result = $a->$method();    # Calls $a->speak()
    say($result);                       # "I am a cat"

    return 0;
}
```

### Calling Variations

```strada
# With arguments
my str $setter = "set_energy";
$obj->$setter(42);

# Without parentheses (accessor style, zero-arg call)
my str $getter = "energy";
my int $e = $obj->$getter;

# With spread arguments
my str $method = "process";
$obj->$method(...@args);
```

### How It Works

- The method name variable is evaluated at runtime and converted to a C string
- The same `strada_method_call()` runtime function is used as for static `$obj->method()` calls
- All OOP features work: inheritance, AUTOLOAD fallback, method modifiers (`before`/`after`/`around`), operator overloading
- The variable must contain a string matching an existing method name (or AUTOLOAD will be tried)

### Use Cases

- **Dispatch tables** — Map action names to methods: `$handler->$dispatch{$action}()`
- **Plugin systems** — Call methods by name from configuration
- **Dynamic APIs** — Invoke methods determined at runtime
- **Testing** — Parameterize method calls in test frameworks

---

## 11.6 Operator Overloading

Strada supports Perl-style operator overloading via `use overload`. This allows classes to define custom behavior for built-in operators like `+`, `-`, `*`, `.`, `""`, `==`, `<=>`, etc.

### Basic Usage

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

### Supported Operators

| Category | Operators |
|----------|-----------|
| Arithmetic | `+`, `-`, `*`, `/`, `%`, `**` |
| String | `.` (concatenation) |
| Stringify | `""` (automatic string conversion) |
| Unary | `neg` (unary minus), `!`, `bool` |
| Numeric comparison | `==`, `!=`, `<`, `>`, `<=`, `>=`, `<=>` |
| String comparison | `eq`, `ne`, `lt`, `gt`, `le`, `ge`, `cmp` |

### Handler Signatures

**Binary operators** (`+`, `-`, `*`, `/`, `==`, etc.):

```strada
func add(scalar $self, scalar $other, int $reversed) scalar {
    # $self    — the blessed object (always the one with the overload)
    # $other   — the other operand
    # $reversed — 1 if $self was the RIGHT operand (important for non-commutative ops)
    if ($reversed == 1) {
        return Vector::new($other->{"x"} + $self->{"x"},
                           $other->{"y"} + $self->{"y"});
    }
    return Vector::new($self->{"x"} + $other->{"x"},
                       $self->{"y"} + $other->{"y"});
}
```

**Unary operators** (`neg`, `!`):

```strada
func negate(scalar $self) scalar {
    return Vector::new(-$self->{"x"}, -$self->{"y"});
}

use overload "neg" => "negate";

# Usage: my scalar $neg = -$v;
```

**Stringify** (`""`):

```strada
func to_str(scalar $self) str {
    return "(" . $self->{"x"} . ", " . $self->{"y"} . ")";
}

use overload '""' => "to_str";

# Usage: say("Vector: " . $v);  # Automatically calls to_str
```

### Zero Overhead

Operator overloading has **zero overhead** when not used:

- If no `use overload` appears anywhere in the program, the generated C code is identical to code without overloading support
- When overloads exist but operands are typed (`int`, `num`, `str`), inline C is generated directly — no dispatch checks
- Runtime dispatch only occurs when the operator IS overloaded AND at least one operand is a `scalar` (which could be a blessed object)

### Dispatch Rules

When an overloaded operator is used:

1. The **left** operand is checked first — if it's blessed and its package has the operator overloaded, that handler is called with `$reversed = 0`
2. If the left operand isn't overloaded, the **right** operand is checked — if blessed with the operator overloaded, that handler is called with `$reversed = 1`
3. If neither operand is overloaded, the default behavior applies (same as without `use overload`)

### Comparison and Sorting

```strada
package BigNum;

func compare(scalar $self, scalar $other, int $reversed) int {
    my num $a = $self->{"value"};
    my num $b = $other->{"value"};
    if ($reversed == 1) {
        return ($b <=> $a);
    }
    return ($a <=> $b);
}

func num_eq(scalar $self, scalar $other, int $reversed) int {
    return $self->{"value"} == $other->{"value"};
}

use overload
    "<=>" => "compare",
    "==" => "num_eq";
```

---

## 12. Design Patterns

### Singleton

```strada
package Logger;

my scalar $instance = undef;

func Logger_new() scalar {
    if (defined($instance)) {
        return $instance;
    }

    my hash %self = ();
    $self{"entries"} = [];
    $instance = bless(\%self, "Logger");
    return $instance;
}

func Logger_log(scalar $self, str $message) void {
    push(@{$self->{"entries"}}, $message);
    say("[LOG] " . $message);
}

func Logger_get_entries(scalar $self) array {
    return @{$self->{"entries"}};
}
```

### Factory

```strada
package ShapeFactory;

func ShapeFactory_create(str $type, array @args) scalar {
    switch ($type) {
        case "circle" {
            return Circle_new($args[0]);
        }
        case "rectangle" {
            return Rectangle_new($args[0], $args[1]);
        }
        case "triangle" {
            return Triangle_new($args[0], $args[1], $args[2]);
        }
        default {
            die("Unknown shape: " . $type);
        }
    }
}
```

### Observer

```strada
package Observable;

func Observable_new() scalar {
    my hash %self = ();
    $self{"observers"} = [];
    return bless(\%self, "Observable");
}

func Observable_add_observer(scalar $self, scalar $observer) void {
    push(@{$self->{"observers"}}, $observer);
}

func Observable_notify(scalar $self, str $event) void {
    foreach my scalar $obs (@{$self->{"observers"}}) {
        Observer_update($obs, $self, $event);
    }
}

package Observer;

func Observer_new(str $name) scalar {
    my hash %self = ();
    $self{"name"} = $name;
    return bless(\%self, "Observer");
}

func Observer_update(scalar $self, scalar $source, str $event) void {
    say($self->{"name"} . " received: " . $event);
}
```

### Strategy

```strada
package SortStrategy;

func SortStrategy_new(scalar $comparator) scalar {
    my hash %self = ();
    $self{"compare"} = $comparator;
    return bless(\%self, "SortStrategy");
}

func SortStrategy_sort(scalar $self, array @items) array {
    my scalar $cmp = $self->{"compare"};
    return @{sort { $cmp->($a, $b); } @items};
}

# Usage
my scalar $ascending = SortStrategy_new(func ($a, $b) {
    return $a <=> $b;
});

my scalar $descending = SortStrategy_new(func ($a, $b) {
    return $b <=> $a;
});

my array @nums = (3, 1, 4, 1, 5, 9);
my array @asc = SortStrategy_sort($ascending, @nums);
my array @desc = SortStrategy_sort($descending, @nums);
```

---

## 13. Best Practices

### 1. Use Consistent Naming

```strada
# Package names: CamelCase
package MyClass;

# Constructor: ClassName_new
func MyClass_new() scalar { ... }

# Methods: ClassName_method_name
func MyClass_get_value(scalar $self) int { ... }

# Private: ClassName__private_method (double underscore)
func MyClass__helper(scalar $self) void { ... }
```

### 2. Initialize All Attributes

```strada
func Person_new(str $name) scalar {
    my hash %self = ();

    # Always initialize all attributes
    $self{"name"} = $name;
    $self{"age"} = 0;
    $self{"email"} = "";
    $self{"created"} = core::time();

    return bless(\%self, "Person");
}
```

### 3. Validate in Setters

```strada
func Person_set_age(scalar $self, int $age) int {
    if ($age < 0 || $age > 150) {
        return 0;  # Failure
    }
    $self->{"age"} = $age;
    return 1;  # Success
}
```

### 4. Use Inheritance Initialization

```strada
# For multi-package files, always use init functions
func Dog_init() void {
    inherit("Dog", "Animal");
}

func Cat_init() void {
    inherit("Cat", "Animal");
}

# Call init before creating objects
func main() int {
    Dog_init();
    Cat_init();

    my scalar $dog = Dog_new("Rex");
    # ...
}
```

### 5. Chain DESTROY Properly

```strada
func Child_DESTROY(scalar $self) void {
    # Clean up child-specific resources first
    if (defined($self->{"resource"})) {
        cleanup_resource($self->{"resource"});
    }

    # Then call parent destructor
    SUPER::DESTROY($self);
}
```

### 6. Use Weak References for Back-References

When objects reference each other (parent-child, observer-subject), use `core::weaken()` on back-references to prevent circular reference memory leaks:

```strada
func TreeNode_add_child(scalar $self, scalar $child) void {
    push($self->{"children"}, $child);
    $child->{"parent"} = $self;
    core::weaken($child->{"parent"});   # Prevent cycle
}
```

See `core::weaken()` and `core::isweak()` in the memory management documentation for details.

### 7. Document Public Interface

```strada
# Person class - represents a person with name and age
#
# Constructor:
#   Person_new(str $name, int $age) -> scalar
#
# Methods:
#   Person_get_name(scalar $self) -> str
#   Person_set_age(scalar $self, int $age) -> int (returns 1 on success)
#   Person_greet(scalar $self) -> void
```

### 8. Use Type Checking for Safety

```strada
func process_animals(array @animals) void {
    foreach my scalar $animal (@animals) {
        if (!isa($animal, "Animal")) {
            die("Expected Animal, got " . blessed($animal));
        }

        # Now safe to call Animal methods
        Animal_speak($animal);
    }
}
```

---

## 14. Moose-Style Declarative OOP

Strada supports a Moose-inspired declarative OOP system that drastically reduces boilerplate. Instead of manually writing constructors, getters, setters, and inheritance initialization, you declare attributes with `has`, parents with `extends`, roles with `with`, and method modifiers with `before`/`after`/`around`. The compiler generates all the necessary code at compile time.

> **Note on `core::` namespace**: `core::` is a preferred alias for `core::` in Strada. Both `core::time()` and `core::time()` call the same function. The examples below use `core::` for consistency with the rest of this guide, but `core::` is recommended for new code.

### Overview

| Feature | Moose-Style Syntax |
|---------|-------------------|
| Attribute declaration | `has ro str $name (required);` |
| Read-write attribute | `has rw int $age = 0;` |
| Inheritance | `extends Parent;` |
| Role composition | `with Role1, Role2;` |
| Before modifier | `before "method" func(scalar $self) void { ... }` |
| After modifier | `after "method" func(scalar $self) void { ... }` |
| Around modifier | `around "method" func(scalar $self) void { ... }` |
| Auto-constructor | Generated automatically (named-argument style) |

---

### 14.1 `has` Attribute Declarations

The `has` keyword declares an attribute on the current package. The compiler automatically generates:

- A **getter** method (always)
- A **setter** method (only if `rw`)
- Attribute initialization in the auto-generated constructor

#### Syntax

```
has [ro|rw] TYPE $NAME [= DEFAULT] [(OPTIONS)];
```

#### Components

| Part | Required | Description |
|------|----------|-------------|
| `ro` / `rw` | Optional (default: `ro`) | Read-only or read-write access |
| `TYPE` | Yes | Strada type: `int`, `num`, `str`, `scalar`, `array`, `hash` |
| `$NAME` | Yes | Attribute name (with `$` sigil) |
| `= DEFAULT` | Optional | Default value expression (used if not passed to constructor) |
| `(OPTIONS)` | Optional | Parenthesized options: `required`, `lazy`, `builder => "name"` |

#### Access Modifiers

- **`ro`** (read-only, the default): generates only a getter. The attribute can only be set via the constructor.
- **`rw`** (read-write): generates both a getter and a setter named `set_NAME`.

#### Examples

```strada
package User;

# Read-only, required (must be passed to constructor)
has ro str $name (required);

# Read-write with default value
has rw int $age = 0;

# Read-only with default (optional in constructor)
has ro str $role = "user";

# Read-write, no default (defaults to undef if not passed)
has rw str $email;

# With lazy and builder options (parsed but builder is for future use)
has ro str $display_name (lazy, builder => "_build_display_name");
```

#### Generated Methods

For each `has` declaration, the compiler generates methods in the package:

```strada
# Given: has rw int $age = 0;
# Generates:

# Getter - always generated
func age(scalar $self) int {
    return $self->{"age"};
}

# Setter - only generated for rw attributes
func set_age(scalar $self, int $val) void {
    $self->{"age"} = $val;
}
```

Usage:

```strada
my scalar $user = User::new("name", "Alice", "age", 25);
say($user->age());         # 25 (getter)
$user->set_age(26);        # setter (only available because age is rw)
say($user->name());        # "Alice" (getter)
# $user->set_name("Bob");  # ERROR: no setter for ro attributes
```

#### Options

- **`required`**: The attribute must be passed to the constructor. If omitted from the constructor call, the value will be `undef` (no runtime enforcement currently -- this is a documentation hint and future-proofing).
- **`lazy`**: Parsed and stored in the attribute metadata. Reserved for future use (lazy initialization with builder functions).
- **`builder => "func_name"`**: Parsed and stored. Reserved for future use (names a function that computes the default value lazily).

---

### 14.2 `extends` -- Inheritance

The `extends` keyword declares that the current package inherits from one or more parent packages. It replaces the manual `inherit("Child", "Parent")` function call pattern.

#### Syntax

```
extends Parent1, Parent2, ...;
```

#### Rules

- Must appear inside a `package` block (not in `package main`)
- Can list multiple parents (comma-separated) for multiple inheritance
- Supports `::` in parent names (e.g., `extends Net::Server;`)
- Inheritance is set up at compile time -- no `_init()` function needed

#### Example

```strada
package Animal;
has ro str $species (required);
has rw int $energy = 100;

func speak(scalar $self) void {
    say($self->species() . " says hello");
}

package Dog;
extends Animal;

has ro str $name (required);
has rw int $age = 0;
```

This is equivalent to calling `inherit("Dog", "Animal")` manually, but cleaner and automatic. The auto-generated `Dog::new` constructor includes attributes from both `Dog` and `Animal`.

#### Multiple Inheritance

```strada
package Duck;
extends Bird, Swimmer;
```

This is equivalent to:
```strada
func Duck_init() void {
    inherit("Duck", "Bird");
    inherit("Duck", "Swimmer");
}
```

---

### 14.3 `with` -- Role Composition

The `with` keyword composes roles (mixins) into the current package. In Strada, `with` is functionally identical to `extends` -- both establish inheritance relationships. The distinction is semantic: use `extends` for "is-a" relationships and `with` for "does" / mixin relationships.

#### Syntax

```
with Role1, Role2, ...;
```

#### Example

```strada
package Serializable;

func serialize(scalar $self) str {
    return "serialized:" . blessed($self);
}

package Printable;

func to_string(scalar $self) str {
    return "[" . blessed($self) . "]";
}

package Document;
extends BaseDocument;
with Serializable, Printable;

has rw str $content = "";
```

Now `Document` objects can call `serialize()` and `to_string()` through inheritance.

---

### 14.4 Auto-Generated Constructors

When a package has one or more `has` declarations and no explicit `new` function, the compiler auto-generates a `new` constructor that accepts **named arguments** as a flat list of key-value pairs.

#### Constructor Signature

```strada
# Auto-generated:
func new(scalar ...@args) scalar { ... }
```

#### Calling Convention

Pass arguments as alternating key-value pairs (like a Perl hash):

```strada
my scalar $dog = Dog::new(
    "name", "Rex",
    "species", "dog",
    "age", 3
);
```

#### How It Works

The generated constructor:

1. Creates an empty hash `%self`
2. Parses the variadic `@args` into a temporary hash `%__a` (key-value pairs)
3. For each `has` attribute (including inherited ones from `extends`):
   - Sets `$self{"attr_name"} = $__a{"attr_name"} // default_value`
   - If no default is specified and the key is missing, the value will be `undef`
4. Returns `bless(\%self, "PackageName")`

#### Inherited Attributes

The constructor collects attributes from the entire inheritance chain. If `Dog extends Animal`, and `Animal` has `$species` and `$energy`, then `Dog::new` accepts all of: `name`, `age`, `nickname` (from Dog) plus `species`, `energy` (from Animal).

```strada
package Animal;
has ro str $species (required);
has rw int $energy = 100;

package Dog;
extends Animal;
has ro str $name (required);
has rw int $age = 0;

package main;

func main() int {
    # Constructor accepts ALL attributes (Dog + Animal)
    my scalar $d = Dog::new(
        "name", "Rex",
        "species", "dog",
        "age", 3
        # energy not passed -- defaults to 100
    );
    say($d->name());     # Rex
    say($d->species());  # dog
    say($d->energy());   # 100
    return 0;
}
```

#### Explicit Constructor Override

If you define your own `new` function in the package, the auto-generated constructor is skipped:

```strada
package Point;
has rw int $x = 0;
has rw int $y = 0;

# Custom constructor -- auto-generation is suppressed
func new(int $x, int $y) scalar {
    my hash %self = ();
    $self{"x"} = $x;
    $self{"y"} = $y;
    return bless(\%self, "Point");
}
```

---

### 14.5 Method Modifiers: `before`, `after`, `around`

Method modifiers let you add behavior before, after, or around an existing method without modifying the method itself. They are declared in the package where the method is defined.

#### `before` Modifier

Runs **before** the target method is called. Receives `$self` and any arguments. The return value is ignored.

```
before "METHOD_NAME" func(PARAMS) void { BODY }
```

Example:

```strada
package Logger;
has rw str $name (required);

before "save" func(scalar $self) void {
    say("[LOG] About to save " . $self->name());
}

func save(scalar $self) void {
    say("Saving " . $self->name());
}
```

Output when calling `$obj->save()`:
```
[LOG] About to save myfile
Saving myfile
```

#### `after` Modifier

Runs **after** the target method returns. Receives `$self` and any arguments. The return value is ignored.

```
after "METHOD_NAME" func(PARAMS) void { BODY }
```

Example:

```strada
package Logger;

after "save" func(scalar $self) void {
    say("[LOG] Finished saving");
}
```

#### `around` Modifier

Wraps the target method entirely. The around modifier receives `$self` plus the original method function pointer (as the first element of a packed args array). This allows the around modifier to decide whether (and how) to call the original method.

```
around "METHOD_NAME" func(PARAMS) void { BODY }
```

The runtime passes the original method as an integer function pointer in the first argument slot. The around modifier can invoke it, skip it, or call it multiple times.

#### Modifier Execution Order

When a method has multiple modifiers, they execute in this order:

1. All `before` modifiers (in declaration order)
2. The `around` modifier (only the first one found; wraps the original method)
3. The original method (called by `around`, or directly if no `around` exists)
4. All `after` modifiers (in declaration order)

#### Complete Example with Modifiers

```strada
package Dog;
extends Animal;

has ro str $name (required);
has rw int $age = 0;

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
    my scalar $d = Dog::new("name", "Rex", "species", "dog");
    $d->bark();
    # Output:
    #   [preparing to bark]
    #   Rex barks!
    #   [done barking]
    return 0;
}
```

#### Multiple Modifiers on the Same Method

You can declare multiple `before` and `after` modifiers for the same method. They all run in declaration order:

```strada
package Audited;

before "process" func(scalar $self) void {
    say("Audit: starting process");
}

before "process" func(scalar $self) void {
    say("Timing: start");
}

func process(scalar $self) void {
    say("Processing...");
}

after "process" func(scalar $self) void {
    say("Timing: end");
}

after "process" func(scalar $self) void {
    say("Audit: finished process");
}
```

---

### 14.6 Complete Example: Animal/Dog Hierarchy

This example demonstrates all Moose-style features working together:

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

    # Getters
    say($d->name());       # Rex
    say($d->age());        # 3

    # Setter (rw attribute)
    $d->set_age(4);
    say($d->age());        # 4

    # Inherited attribute
    say($d->energy());     # 100
    $d->set_energy(80);
    say($d->energy());     # 80

    # Inherited method
    $d->speak();           # dog (energy: 80)

    # Method with before/after modifiers
    $d->bark();
    # [preparing to bark]
    # Rex barks!
    # [done barking]

    # Inheritance checks
    say($d->isa("Dog"));    # 1
    say($d->isa("Animal")); # 1

    return 0;
}
```

---

### 14.7 Moose-Style OOP with Shared Libraries (`import_lib`)

Moose-style classes work seamlessly with shared libraries. The `has`-generated getters, setters, and constructors are normal Strada functions that get exported through the standard `__strada_export_info()` mechanism.

#### Creating a Shared Library

```strada
# lib/Pet.strada
package Pet;
has ro str $name (required);
has rw int $age = 0;
has ro str $species (required);

func greet(scalar $self) str {
    return "Hi, I'm " . $self->name() . " the " . $self->species();
}
```

Compile as shared library:

```bash
./strada --shared lib/Pet.strada
mv Pet.so lib/
```

#### Using the Library

```strada
use lib "lib";
import_lib "Pet.so";

func main() int {
    my scalar $p = Pet::new("name", "Buddy", "species", "dog", "age", 5);
    say(Pet::greet($p));    # Hi, I'm Buddy the dog
    say(Pet::name($p));     # Buddy
    say(Pet::age($p));      # 5
    Pet::set_age($p, 6);
    say(Pet::age($p));      # 6
    return 0;
}
```

Note: When calling through `import_lib`, use `Package::method($obj)` syntax rather than `$obj->method()`, as the OOP method dispatch tables are set up at compile time per binary.

---

### 14.8 Syntax Reference

#### `has` Declaration

```
has [ro|rw] TYPE $NAME [= DEFAULT_EXPR] [(OPTION, ...)];
```

Options:
- `required` -- Attribute should be provided in the constructor
- `lazy` -- Reserved for future lazy initialization
- `builder => "func_name"` -- Reserved for future builder functions

#### `extends` Statement

```
extends ParentPkg1 [, ParentPkg2, ...];
```

Sets up inheritance. Equivalent to `inherit("Child", "Parent")` for each parent.

#### `with` Statement

```
with RolePkg1 [, RolePkg2, ...];
```

Composes roles. Functionally identical to `extends` (both set up inheritance).

#### Method Modifiers

```
before "method_name" func(scalar $self) void { BODY }
after  "method_name" func(scalar $self) void { BODY }
around "method_name" func(scalar $self) void { BODY }
```

- Method name is a string literal
- The modifier function follows standard Strada function syntax (params, return type, body)
- Modifiers must be declared in the same package as the method they modify

---

## 15. Comparison: Manual OOP vs Moose-Style OOP

This section shows the same class implemented both ways to illustrate the reduction in boilerplate.

### Manual OOP (Traditional)

```strada
package Person;

func Person_new(str $name, int $age, str $email) scalar {
    my hash %self = ();
    $self{"name"} = $name;
    $self{"age"} = $age;
    $self{"email"} = $email;
    return bless(\%self, "Person");
}

func Person_name(scalar $self) str {
    return $self->{"name"};
}

func Person_age(scalar $self) int {
    return $self->{"age"};
}

func Person_set_age(scalar $self, int $age) void {
    $self->{"age"} = $age;
}

func Person_email(scalar $self) str {
    return $self->{"email"};
}

func Person_set_email(scalar $self, str $email) void {
    $self->{"email"} = $email;
}

func Person_greet(scalar $self) void {
    say("Hello, I'm " . $self->{"name"});
}

package Employee;

func Employee_init() void {
    inherit("Employee", "Person");
}

func Employee_new(str $name, int $age, str $email, str $title) scalar {
    my hash %self = ();
    $self{"name"} = $name;
    $self{"age"} = $age;
    $self{"email"} = $email;
    $self{"title"} = $title;
    return bless(\%self, "Employee");
}

func Employee_title(scalar $self) str {
    return $self->{"title"};
}

package main;

func main() int {
    Employee_init();

    my scalar $e = Employee_new("Alice", 30, "alice@co.com", "Engineer");
    say(Person_name($e));      # Alice
    say(Employee_title($e));   # Engineer
    Person_greet($e);          # Hello, I'm Alice
    return 0;
}
```

**Lines of boilerplate**: ~40 (constructor, getters, setters, init)

### Moose-Style OOP (Declarative)

```strada
package Person;
has ro str $name (required);
has rw int $age = 0;
has rw str $email;

func greet(scalar $self) void {
    say("Hello, I'm " . $self->name());
}

package Employee;
extends Person;
has ro str $title (required);

package main;

func main() int {
    my scalar $e = Employee::new(
        "name", "Alice",
        "age", 30,
        "email", "alice@co.com",
        "title", "Engineer"
    );
    say($e->name());    # Alice
    say($e->title());   # Engineer
    $e->greet();        # Hello, I'm Alice
    return 0;
}
```

**Lines of boilerplate**: 0 (constructor, getters, setters, inheritance all auto-generated)

### Key Differences

| Aspect | Manual OOP | Moose-Style |
|--------|-----------|-------------|
| Constructor | Write manually | Auto-generated from `has` |
| Getters | Write manually | Auto-generated (always) |
| Setters | Write manually | Auto-generated (if `rw`) |
| Inheritance setup | `inherit()` call + `_init()` func | `extends Parent;` |
| Constructor args | Positional | Named (key-value pairs) |
| Adding an attribute | Add to constructor, write getter/setter | One `has` line |
| Method modifiers | Not available | `before`/`after`/`around` |

### When to Use Each

**Use Moose-style** when:
- You have data-heavy classes with many attributes
- You want named constructor arguments
- You need method modifiers for cross-cutting concerns (logging, validation, timing)
- You want minimal boilerplate

**Use manual OOP** when:
- You need custom constructor logic (validation, computed fields, side effects)
- You want positional constructor arguments for a small number of parameters
- You need fine-grained control over getter/setter behavior
- You are mixing both styles (manual `new` suppresses auto-generation)

### Mixing Both Styles

You can use `has` declarations alongside manual methods. If you define an explicit `new()` function, the auto-generated constructor is suppressed, but getters and setters from `has` are still generated:

```strada
package Config;
has ro str $name (required);
has rw int $debug = 0;
has rw int $verbose = 0;

# Custom constructor with validation -- suppresses auto-constructor
func new(str $name) scalar {
    if (length($name) == 0) {
        die("Config name cannot be empty");
    }
    my hash %self = ();
    $self{"name"} = $name;
    $self{"debug"} = 0;
    $self{"verbose"} = 0;
    return bless(\%self, "Config");
}

# has-generated getters/setters still work:
# $cfg->name(), $cfg->debug(), $cfg->set_debug(), etc.
```

---

## 16. Tied Hashes (`tie` / `untie` / `tied`)

Strada supports Perl-style tied hashes, which allow you to bind a hash variable to a class that intercepts all access operations. When a hash is tied, every read, write, delete, and iteration goes through methods you define on the tied class.

### Overview

| Function | Purpose |
|----------|---------|
| `tie(%hash, "ClassName", @args)` | Bind a hash to a class |
| `untie(%hash)` | Remove the binding |
| `tied(%hash)` | Get the tied object (or undef if not tied) |

### The TIEHASH Interface

To make a class work with `tie`, define these methods in your package:

| Method | Called When | Signature |
|--------|-----------|-----------|
| `TIEHASH` | `tie(%hash, "Pkg", @args)` | `func TIEHASH(str $class, ...) scalar` |
| `FETCH` | `$hash{"key"}` | `func FETCH(scalar $self, str $key) scalar` |
| `STORE` | `$hash{"key"} = $val` | `func STORE(scalar $self, str $key, scalar $val) void` |
| `EXISTS` | `exists($hash{"key"})` | `func EXISTS(scalar $self, str $key) int` |
| `DELETE` | `delete($hash{"key"})` | `func DELETE(scalar $self, str $key) void` |
| `FIRSTKEY` | `keys(%hash)` (start iteration) | `func FIRSTKEY(scalar $self) scalar` |
| `NEXTKEY` | `keys(%hash)` (continue iteration) | `func NEXTKEY(scalar $self, str $lastkey) scalar` |
| `CLEAR` | `%hash = ()` | `func CLEAR(scalar $self) void` |

Only `TIEHASH` and `FETCH`/`STORE` are required. The others are optional -- if not defined, the corresponding operations will use default behavior or fail gracefully.

### Basic Example

```strada
package CaseInsensitiveHash;

func TIEHASH(str $class) scalar {
    my hash %self = ();
    $self{"_data"} = {};
    return bless(\%self, $class);
}

func FETCH(scalar $self, str $key) scalar {
    return $self->{"_data"}->{lc($key)};
}

func STORE(scalar $self, str $key, scalar $val) void {
    $self->{"_data"}->{lc($key)} = $val;
}

func EXISTS(scalar $self, str $key) int {
    return exists($self->{"_data"}->{lc($key)});
}

func DELETE(scalar $self, str $key) void {
    delete($self->{"_data"}->{lc($key)});
}

func FIRSTKEY(scalar $self) scalar {
    my array @k = keys(%{$self->{"_data"}});
    if (scalar(@k) == 0) {
        return undef;
    }
    $self->{"_iter_keys"} = \@k;
    $self->{"_iter_idx"} = 0;
    return $k[0];
}

func NEXTKEY(scalar $self, str $lastkey) scalar {
    my int $idx = $self->{"_iter_idx"} + 1;
    $self->{"_iter_idx"} = $idx;
    my array @k = @{$self->{"_iter_keys"}};
    if ($idx >= scalar(@k)) {
        return undef;
    }
    return $k[$idx];
}

package main;

func main() int {
    my hash %config = ();
    tie(%config, "CaseInsensitiveHash");

    $config{"Host"} = "localhost";
    $config{"PORT"} = 8080;

    say($config{"host"});    # localhost (case-insensitive lookup)
    say($config{"port"});    # 8080

    if (exists($config{"HOST"})) {
        say("Host is set");
    }

    return 0;
}
```

### How `tie` Works

1. Calling `tie(%hash, "ClassName", @args)` invokes `ClassName::TIEHASH("ClassName", @args)` and stores the returned object as the hash's tied implementation.
2. From that point on, all hash operations on `%hash` are intercepted:
   - `$hash{"key"}` calls `FETCH($tied_obj, "key")`
   - `$hash{"key"} = $val` calls `STORE($tied_obj, "key", $val)`
   - `exists($hash{"key"})` calls `EXISTS($tied_obj, "key")`
   - `delete($hash{"key"})` calls `DELETE($tied_obj, "key")`
   - `keys(%hash)` uses `FIRSTKEY` and `NEXTKEY` to iterate
3. `untie(%hash)` removes the binding, returning the hash to normal behavior.
4. `tied(%hash)` returns the underlying tied object, or `undef` if the hash is not tied.

### Accessing the Tied Object

Use `tied()` to get the underlying object and call methods on it directly:

```strada
my hash %h = ();
tie(%h, "MyTiedHash");

my scalar $obj = tied(%h);
if (defined($obj)) {
    $obj->some_custom_method();
}
```

### Zero Overhead for Untied Hashes

Tied hash dispatch uses `__builtin_expect` to hint to the CPU branch predictor that the common case (untied hash) is the fast path. Normal hash operations on untied variables have no measurable overhead from the tie mechanism. The tied check is a single pointer comparison against NULL, and the branch predictor learns quickly that untied is the common case.

### Memory Safety

- Tied objects follow normal Strada reference counting. When a tied hash goes out of scope, the tied object is decremented and freed if no other references exist.
- `untie()` decrements the reference to the tied object.
- DESTROY methods on the tied class are called normally when the object's refcount reaches zero.
- Verified with valgrind: 0 bytes definitely lost.

### Use Cases

- **Case-insensitive hashes** -- Normalize keys on access
- **Default values** -- Return a default when a key is missing
- **Validation** -- Reject invalid keys or values in STORE
- **Logging/Auditing** -- Track all reads and writes
- **Persistent storage** -- Back a hash with a file or database
- **Computed values** -- Generate values on the fly in FETCH

---

## Summary

| Feature | Syntax |
|---------|--------|
| Define class | `package ClassName;` |
| Constructor | `func ClassName_new() scalar { ... bless(\%self, "ClassName") }` |
| Method | `func ClassName_method(scalar $self) TYPE { ... }` |
| Inheritance | `inherit Parent;` or `inherit("Child", "Parent");` |
| Multiple inheritance | `inherit A, B, C;` |
| SUPER call | `SUPER::method($self, @args)` |
| Destructor | `func ClassName_DESTROY(scalar $self) void { ... }` |
| Type check | `isa($obj, "ClassName")` |
| Get package | `blessed($obj)` |
| Method check | `can($obj, "method")` |
| Operator overload | `use overload "+" => "add", '""' => "to_str";` |
| Attribute (Moose) | `has rw int $age = 0;` |
| Inheritance (Moose) | `extends Parent;` |
| Role composition | `with Role1, Role2;` |
| Before modifier | `before "method" func($self) void { ... }` |
| After modifier | `after "method" func($self) void { ... }` |
| Around modifier | `around "method" func($self) void { ... }` |
| Tie hash to class | `tie(%hash, "ClassName", @args)` |
| Untie hash | `untie(%hash)` |
| Get tied object | `tied(%hash)` |

---

## See Also

- [Tutorial](TUTORIAL.md) - Basic OOP tutorial
- [Language Manual](LANGUAGE_MANUAL.md) - Complete reference
- [Examples](EXAMPLES.md) - OOP examples

# Strada Examples

This document contains annotated code examples demonstrating Strada features. Each example is complete and runnable.

## Table of Contents

1. [Hello World](#1-hello-world)
2. [Command-Line Arguments](#2-command-line-arguments)
3. [Fibonacci](#3-fibonacci)
4. [Data Structures](#4-data-structures)
5. [File Processing](#5-file-processing)
6. [JSON-like Data](#6-json-like-data)
7. [Regular Expressions](#7-regular-expressions)
8. [Object-Oriented Programming](#8-object-oriented-programming)
9. [Closures and Higher-Order Functions](#9-closures-and-higher-order-functions)
10. [Multithreading](#10-multithreading)
11. [Network Programming](#11-network-programming)
12. [FFI (Foreign Function Interface)](#12-ffi-foreign-function-interface)
13. [Error Handling](#13-error-handling)
14. [Complete Applications](#14-complete-applications)
15. [String Repetition and Transliteration](#15-string-repetition-and-transliteration)
16. [Splice: Array Surgery](#16-splice-array-surgery)
17. [Hash Iteration with each() and Tied Hashes](#17-hash-iteration-with-each-and-tied-hashes)
18. [Select, Local, and the /e Regex Modifier](#18-select-local-and-the-e-regex-modifier)

---

## 1. Hello World

The simplest Strada program:

```strada
# hello.strada
func main() int {
    say("Hello, World!");
    return 0;
}
```

Compile and run:
```bash
./strada -r hello.strada
```

---

## 2. Command-Line Arguments

```strada
# args.strada
func main() int {
    say("Program: " . $ARGV[0]);
    say("Arguments: " . ($ARGC - 1));

    if ($ARGC > 1) {
        say("\nArguments:");
        for (my int $i = 1; $i < $ARGC; $i = $i + 1) {
            say("  " . $i . ": " . $ARGV[$i]);
        }
    }

    return 0;
}
```

Run:
```bash
./strada args.strada
./args one two three
```

---

## 3. Fibonacci

### Recursive Version

```strada
# fibonacci_recursive.strada
func fib(int $n) int {
    if ($n <= 1) {
        return $n;
    }
    return fib($n - 1) + fib($n - 2);
}

func main() int {
    say("Fibonacci sequence (recursive):");
    for (my int $i = 0; $i <= 20; $i = $i + 1) {
        say("fib(" . $i . ") = " . fib($i));
    }
    return 0;
}
```

### Iterative Version (Faster)

```strada
# fibonacci_iterative.strada
func fib(int $n) int {
    if ($n <= 1) {
        return $n;
    }

    my int $a = 0;
    my int $b = 1;

    for (my int $i = 2; $i <= $n; $i = $i + 1) {
        my int $temp = $a + $b;
        $a = $b;
        $b = $temp;
    }

    return $b;
}

func main() int {
    say("Fibonacci sequence (iterative):");
    for (my int $i = 0; $i <= 40; $i = $i + 1) {
        say("fib(" . $i . ") = " . fib($i));
    }
    return 0;
}
```

---

## 4. Data Structures

### Working with Arrays

```strada
# arrays.strada
func main() int {
    # Create and populate array
    my array @numbers = ();
    for (my int $i = 1; $i <= 10; $i = $i + 1) {
        push(@numbers, $i);
    }

    say("Original: ");
    dumper(\@numbers);

    # Map: square each number
    my scalar $squared = map { $_ * $_; } @numbers;
    say("\nSquared:");
    dumper($squared);

    # Grep: filter even numbers
    my scalar $evens = grep { $_ % 2 == 0; } @numbers;
    say("\nEven numbers:");
    dumper($evens);

    # Sort: descending order
    my scalar $desc = sort { $b <=> $a; } @numbers;
    say("\nDescending:");
    dumper($desc);

    # Reduce-style sum
    my int $sum = 0;
    foreach my int $n (@numbers) {
        $sum = $sum + $n;
    }
    say("\nSum: " . $sum);

    return 0;
}
```

### Working with Hashes

```strada
# hashes.strada
func main() int {
    # Create a hash
    my hash %person = ();
    $person{"name"} = "Alice";
    $person{"age"} = 30;
    $person{"city"} = "New York";
    $person{"occupation"} = "Engineer";

    say("Person data:");
    dumper(\%person);

    # Iterate over keys
    say("\nFormatted output:");
    foreach my str $key (keys(%person)) {
        say("  " . $key . ": " . $person{$key});
    }

    # Check existence
    if (exists($person{"email"})) {
        say("\nHas email: " . $person{"email"});
    } else {
        say("\nNo email on file");
    }

    # Delete a key
    delete($person{"occupation"});
    say("\nAfter deleting occupation:");
    say("Keys: " . size(%person));

    return 0;
}
```

### Nested Structures

```strada
# nested.strada
func main() int {
    # Build a nested structure
    my scalar $company = {
        name => "TechCorp",
        founded => 2020,
        employees => [
            { name => "Alice", role => "CEO", salary => 150000 },
            { name => "Bob", role => "CTO", salary => 140000 },
            { name => "Charlie", role => "Developer", salary => 100000 }
        ],
        departments => {
            engineering => 15,
            sales => 8,
            hr => 3
        }
    };

    say("Company: " . $company->{"name"});
    say("Founded: " . $company->{"founded"});

    say("\nEmployees:");
    my scalar $emps = $company->{"employees"};
    for (my int $i = 0; $i < 3; $i = $i + 1) {
        my scalar $emp = $emps->[$i];
        say("  " . $emp->{"name"} . " - " . $emp->{"role"} .
            " ($" . $emp->{"salary"} . ")");
    }

    say("\nDepartment sizes:");
    my scalar $depts = $company->{"departments"};
    say("  Engineering: " . $depts->{"engineering"});
    say("  Sales: " . $depts->{"sales"});
    say("  HR: " . $depts->{"hr"});

    # Full dump
    say("\nFull structure:");
    dumper($company);

    return 0;
}
```

---

## 5. File Processing

### Reading a File

```strada
# read_file.strada
func main() int {
    my str $filename = "input.txt";

    # Check if we can read it
    my str $content = core::slurp($filename);
    if (!defined($content)) {
        die("Cannot read " . $filename);
    }

    say("File contents:");
    say($content);

    # Count lines
    my array @lines = split("\n", $content);
    say("\nLine count: " . size(@lines));

    return 0;
}
```

### Word Counter

```strada
# word_count.strada
func main() int {
    if ($ARGC < 2) {
        say("Usage: word_count <filename>");
        return 1;
    }

    my str $filename = $ARGV[1];
    my str $content = core::slurp($filename);

    if (!defined($content)) {
        die("Cannot read " . $filename);
    }

    # Count statistics
    my int $chars = length($content);
    my array @lines = split("\n", $content);
    my int $line_count = size(@lines);

    # Count words (split on whitespace)
    my array @words = split(" ", $content);
    my int $word_count = 0;
    foreach my str $w (@words) {
        if (length(trim($w)) > 0) {
            $word_count = $word_count + 1;
        }
    }

    say("File: " . $filename);
    say("Characters: " . $chars);
    say("Lines: " . $line_count);
    say("Words: " . $word_count);

    return 0;
}
```

### CSV Parser

```strada
# csv_parser.strada
func parse_csv_line(str $line) array {
    return split(",", $line);
}

func main() int {
    my str $csv_data = "name,age,city
Alice,30,New York
Bob,25,Los Angeles
Charlie,35,Chicago";

    my array @lines = split("\n", $csv_data);

    # Get headers
    my array @headers = parse_csv_line($lines[0]);
    say("Headers: " . join(", ", @headers));

    # Parse data rows
    my array @records = ();
    for (my int $i = 1; $i < size(@lines); $i = $i + 1) {
        my array @fields = parse_csv_line($lines[$i]);
        my scalar $record = {};

        for (my int $j = 0; $j < size(@headers); $j = $j + 1) {
            $record->{$headers[$j]} = $fields[$j];
        }

        push(@records, $record);
    }

    say("\nRecords:");
    foreach my scalar $rec (@records) {
        say("  " . $rec->{"name"} . " is " . $rec->{"age"} .
            " years old, lives in " . $rec->{"city"});
    }

    return 0;
}
```

---

## 6. JSON-like Data

```strada
# json_builder.strada

# Simple JSON-like output (not full JSON spec)
func to_json(scalar $val) str {
    my str $type = typeof($val);

    if ($type eq "UNDEF") {
        return "null";
    }
    if ($type eq "INT" || $type eq "NUM") {
        return "" . $val;
    }
    if ($type eq "STR") {
        return "\"" . $val . "\"";
    }

    return "\"[complex]\"";
}

func hash_to_json(scalar $hash) str {
    my str $result = "{";
    my int $first = 1;

    foreach my str $key (keys(%{$hash})) {
        if (!$first) {
            $result = $result . ", ";
        }
        $first = 0;
        $result = $result . "\"" . $key . "\": " . to_json($hash->{$key});
    }

    return $result . "}";
}

func main() int {
    my scalar $data = {
        name => "Alice",
        age => 30,
        active => 1
    };

    say("JSON output:");
    say(hash_to_json($data));

    return 0;
}
```

---

## 7. Regular Expressions

### Pattern Matching

```strada
# regex_demo.strada
func main() int {
    my str $text = "The quick brown fox jumps over the lazy dog";

    # Simple match
    if ($text =~ /quick/) {
        say("Found 'quick'");
    }

    # Case insensitive
    if ($text =~ /QUICK/i) {
        say("Found 'QUICK' (case insensitive)");
    }

    # Anchors
    if ($text =~ /^The/) {
        say("Starts with 'The'");
    }

    if ($text =~ /dog$/) {
        say("Ends with 'dog'");
    }

    # Character classes
    if ($text =~ /[aeiou]{2}/) {
        say("Contains two consecutive vowels");
    }

    return 0;
}
```

### Substitution

```strada
# regex_replace.strada
func main() int {
    my str $text = "Hello World! Hello Universe!";

    say("Original: " . $text);

    # Replace first
    $text =~ s/Hello/Hi/;
    say("After s/Hello/Hi/: " . $text);

    # Replace all
    $text = "Hello World! Hello Universe!";
    $text =~ s/Hello/Hi/g;
    say("After s/Hello/Hi/g: " . $text);

    # More complex pattern
    my str $phone = "Call me at 555-123-4567 or 555-987-6543";
    $phone =~ s/555-/XXX-/g;
    say("Redacted: " . $phone);

    return 0;
}
```

### Capturing Groups

```strada
# regex_capture.strada
func main() int {
    my str $date = "2024-01-15";

    my array @parts = capture($date, "(\d{4})-(\d{2})-(\d{2})");

    if (size(@parts) >= 3) {
        say("Year: " . $parts[0]);
        say("Month: " . $parts[1]);
        say("Day: " . $parts[2]);
    }

    # Parse email
    my str $email = "user@example.com";
    my array @email_parts = capture($email, "(.+)@(.+)");

    if (size(@email_parts) >= 2) {
        say("\nEmail parts:");
        say("  User: " . $email_parts[0]);
        say("  Domain: " . $email_parts[1]);
    }

    return 0;
}
```

---

## 8. Object-Oriented Programming

### Basic OOP

```strada
# oop_basic.strada
package Point;

func Point_new(int $x, int $y) scalar {
    my hash %self = ();
    $self{"x"} = $x;
    $self{"y"} = $y;
    return bless(\%self, "Point");
}

func Point_x(scalar $self) int {
    return $self->{"x"};
}

func Point_y(scalar $self) int {
    return $self->{"y"};
}

func Point_distance(scalar $self, scalar $other) num {
    my int $dx = Point_x($other) - Point_x($self);
    my int $dy = Point_y($other) - Point_y($self);
    return math::sqrt($dx * $dx + $dy * $dy);
}

func Point_to_string(scalar $self) str {
    return "(" . Point_x($self) . ", " . Point_y($self) . ")";
}

package main;

func main() int {
    my scalar $p1 = Point_new(0, 0);
    my scalar $p2 = Point_new(3, 4);

    say("Point 1: " . Point_to_string($p1));
    say("Point 2: " . Point_to_string($p2));
    say("Distance: " . Point_distance($p1, $p2));

    return 0;
}
```

### Inheritance

```strada
# oop_inheritance.strada
package Shape;

func Shape_new(str $name) scalar {
    my hash %self = ();
    $self{"name"} = $name;
    return bless(\%self, "Shape");
}

func Shape_name(scalar $self) str {
    return $self->{"name"};
}

func Shape_area(scalar $self) num {
    return 0.0;  # Base class returns 0
}

func Shape_describe(scalar $self) void {
    say(Shape_name($self) . " with area " . Shape_area($self));
}

# Rectangle inherits from Shape
package Rectangle;

func Rectangle_init() void {
    inherit("Rectangle", "Shape");
}

func Rectangle_new(num $width, num $height) scalar {
    my hash %self = ();
    $self{"name"} = "Rectangle";
    $self{"width"} = $width;
    $self{"height"} = $height;
    return bless(\%self, "Rectangle");
}

func Rectangle_area(scalar $self) num {
    return $self->{"width"} * $self->{"height"};
}

# Circle inherits from Shape
package Circle;

func Circle_init() void {
    inherit("Circle", "Shape");
}

func Circle_new(num $radius) scalar {
    my hash %self = ();
    $self{"name"} = "Circle";
    $self{"radius"} = $radius;
    return bless(\%self, "Circle");
}

func Circle_area(scalar $self) num {
    return 3.14159 * $self->{"radius"} * $self->{"radius"};
}

# Main
package main;

func main() int {
    Rectangle_init();
    Circle_init();

    my scalar $rect = Rectangle_new(5.0, 3.0);
    my scalar $circle = Circle_new(2.0);

    say("Rectangle:");
    say("  Area: " . Rectangle_area($rect));
    say("  isa Shape: " . isa($rect, "Shape"));

    say("\nCircle:");
    say("  Area: " . Circle_area($circle));
    say("  isa Shape: " . isa($circle, "Shape"));

    return 0;
}
```

### Multiple Inheritance

```strada
# oop_multiple.strada
package Printable;

func Printable_print(scalar $self) void {
    say("[Printable] " . $self->{"data"});
}

package Serializable;

func Serializable_serialize(scalar $self) str {
    return "DATA:" . $self->{"data"};
}

package Document;

func Document_init() void {
    inherit("Document", "Printable");
    inherit("Document", "Serializable");
}

func Document_new(str $data) scalar {
    my hash %self = ();
    $self{"data"} = $data;
    return bless(\%self, "Document");
}

package main;

func main() int {
    Document_init();

    my scalar $doc = Document_new("Hello, World!");

    say("isa Printable: " . isa($doc, "Printable"));
    say("isa Serializable: " . isa($doc, "Serializable"));

    Printable_print($doc);
    say("Serialized: " . Serializable_serialize($doc));

    return 0;
}
```

---

## 9. Closures and Higher-Order Functions

### Basic Closures

```strada
# closures_basic.strada
func main() int {
    # Simple closure
    my scalar $add = func (int $a, int $b) {
        return $a + $b;
    };

    say("3 + 4 = " . $add->(3, 4));

    # Closure capturing variable
    my int $multiplier = 10;
    my scalar $scale = func (int $n) {
        return $n * $multiplier;
    };

    say("5 * 10 = " . $scale->(5));

    # Captured by reference - changes affect closure
    $multiplier = 100;
    say("5 * 100 = " . $scale->(5));

    return 0;
}
```

### Counter with State

```strada
# closures_counter.strada
func make_counter(int $start) scalar {
    my int $count = $start;

    return func () {
        $count = $count + 1;
        return $count;
    };
}

func main() int {
    my scalar $counter1 = make_counter(0);
    my scalar $counter2 = make_counter(100);

    say("Counter 1: " . $counter1->());  # 1
    say("Counter 1: " . $counter1->());  # 2
    say("Counter 1: " . $counter1->());  # 3

    say("Counter 2: " . $counter2->());  # 101
    say("Counter 2: " . $counter2->());  # 102

    say("Counter 1: " . $counter1->());  # 4 (independent)

    return 0;
}
```

### Higher-Order Functions

```strada
# higher_order.strada
func apply(scalar $f, int $x) int {
    return $f->($x);
}

func compose(scalar $f, scalar $g) scalar {
    return func (int $x) {
        return $f->($g->($x));
    };
}

func main() int {
    my scalar $double = func (int $n) { return $n * 2; };
    my scalar $increment = func (int $n) { return $n + 1; };
    my scalar $square = func (int $n) { return $n * $n; };

    say("double(5) = " . apply($double, 5));
    say("increment(5) = " . apply($increment, 5));
    say("square(5) = " . apply($square, 5));

    # Compose functions
    my scalar $double_then_increment = compose($increment, $double);
    say("\n(double then increment)(5) = " . $double_then_increment->(5));

    my scalar $increment_then_double = compose($double, $increment);
    say("(increment then double)(5) = " . $increment_then_double->(5));

    return 0;
}
```

---

## 10. Multithreading

### Basic Threading

```strada
# threads_basic.strada
func main() int {
    say("Main thread starting...");

    my scalar $worker = thread::create(func () {
        say("Worker thread running");
        core::sleep(1);
        say("Worker thread done");
        return 42;
    });

    say("Main thread waiting...");
    my scalar $result = thread::join($worker);
    say("Worker returned: " . $result);

    return 0;
}
```

### Parallel Work with Mutex

```strada
# threads_mutex.strada
func main() int {
    my int $counter = 0;
    my scalar $mutex = thread::mutex_new();
    my int $iterations = 10000;

    # Create worker closure
    my scalar $worker = func () {
        for (my int $i = 0; $i < $iterations; $i = $i + 1) {
            thread::mutex_lock($mutex);
            $counter = $counter + 1;
            thread::mutex_unlock($mutex);
        }
        return 0;
    };

    # Start multiple threads
    my scalar $t1 = thread::create($worker);
    my scalar $t2 = thread::create($worker);
    my scalar $t3 = thread::create($worker);

    # Wait for all
    thread::join($t1);
    thread::join($t2);
    thread::join($t3);

    thread::mutex_destroy($mutex);

    say("Expected: " . ($iterations * 3));
    say("Actual: " . $counter);

    return 0;
}
```

### Producer-Consumer

```strada
# threads_prodcons.strada
func main() int {
    my array @queue = ();
    my int $done = 0;
    my scalar $mutex = thread::mutex_new();
    my scalar $cond = thread::cond_new();

    # Producer
    my scalar $producer = thread::create(func () {
        for (my int $i = 1; $i <= 5; $i = $i + 1) {
            core::usleep(100000);  # Simulate work

            thread::mutex_lock($mutex);
            push(@queue, $i);
            say("Produced: " . $i);
            thread::cond_signal($cond);
            thread::mutex_unlock($mutex);
        }

        thread::mutex_lock($mutex);
        $done = 1;
        thread::cond_signal($cond);
        thread::mutex_unlock($mutex);

        return 0;
    });

    # Consumer
    my scalar $consumer = thread::create(func () {
        while (1) {
            thread::mutex_lock($mutex);

            while (size(@queue) == 0 && !$done) {
                thread::cond_wait($cond, $mutex);
            }

            if (size(@queue) > 0) {
                my scalar $item = shift(@queue);
                thread::mutex_unlock($mutex);
                say("Consumed: " . $item);
            } else {
                thread::mutex_unlock($mutex);
                if ($done) {
                    last;
                }
            }
        }
        return 0;
    });

    thread::join($producer);
    thread::join($consumer);

    thread::cond_destroy($cond);
    thread::mutex_destroy($mutex);

    say("Done!");
    return 0;
}
```

---

## 11. Network Programming

### Simple TCP Server

```strada
# tcp_server.strada
func main() int {
    my int $port = 8080;

    say("Starting server on port " . $port);
    my scalar $server = core::socket_server($port);

    if (!defined($server)) {
        die("Failed to create server");
    }

    say("Waiting for connection...");
    my scalar $client = core::socket_accept($server);

    say("Client connected!");

    # Send response
    my str $response = "HTTP/1.1 200 OK\r\n";
    $response = $response . "Content-Type: text/plain\r\n";
    $response = $response . "Connection: close\r\n\r\n";
    $response = $response . "Hello from Strada!\n";

    core::socket_send($client, $response);
    core::socket_close($client);
    core::socket_close($server);

    say("Done!");
    return 0;
}
```

### TCP Client

```strada
# tcp_client.strada
func main() int {
    my str $host = "example.com";
    my int $port = 80;

    say("Connecting to " . $host . ":" . $port);
    my scalar $sock = core::socket_client($host, $port);

    if (!defined($sock)) {
        die("Failed to connect");
    }

    # Send HTTP request
    my str $request = "GET / HTTP/1.1\r\n";
    $request = $request . "Host: " . $host . "\r\n";
    $request = $request . "Connection: close\r\n\r\n";

    core::socket_send($sock, $request);

    # Read response
    my str $response = core::socket_recv($sock, 4096);
    say("Response:");
    say($response);

    core::socket_close($sock);
    return 0;
}
```

---

## 12. FFI (Foreign Function Interface)

### Calling libm Functions

```strada
# ffi_libm.strada
func main() int {
    # Load math library
    my int $libm = core::dl_open("libm.so.6");
    if ($libm == 0) {
        die("Failed to load libm: " . core::dl_error());
    }

    # Get sqrt function
    my int $sqrt_fn = core::dl_sym($libm, "sqrt");

    # Call sqrt(16.0)
    my scalar $args = [16.0];
    my num $result = core::dl_call_num($sqrt_fn, $args);
    say("sqrt(16) = " . $result);

    # Get sin function
    my int $sin_fn = core::dl_sym($libm, "sin");
    $args = [3.14159 / 2];
    $result = core::dl_call_num($sin_fn, $args);
    say("sin(pi/2) = " . $result);

    core::dl_close($libm);
    return 0;
}
```

### Custom C Library

First, create the C library:

```c
// mylib.c
#include "strada_runtime.h"

int64_t my_add(StradaValue *a, StradaValue *b) {
    return strada_to_int(a) + strada_to_int(b);
}

char* my_greet(StradaValue *name) {
    const char *n = strada_to_str(name);
    char *result = malloc(strlen(n) + 20);
    sprintf(result, "Hello, %s!", n);
    return result;
}
```

Build:
```bash
gcc -shared -fPIC -o libmylib.so mylib.c -Iruntime
```

Use in Strada:

```strada
# ffi_custom.strada
func main() int {
    my int $lib = core::dl_open("./libmylib.so");
    if ($lib == 0) {
        die("Failed to load library");
    }

    # Call my_add
    my int $add_fn = core::dl_sym($lib, "my_add");
    my int $sum = core::dl_call_int_sv($add_fn, [10, 20]);
    say("my_add(10, 20) = " . $sum);

    # Call my_greet
    my int $greet_fn = core::dl_sym($lib, "my_greet");
    my str $greeting = core::dl_call_str_sv($greet_fn, ["World"]);
    say($greeting);

    core::dl_close($lib);
    return 0;
}
```

---

## 13. Error Handling

### Try/Catch

```strada
# error_handling.strada
func divide(int $a, int $b) int {
    if ($b == 0) {
        throw "Division by zero";
    }
    return $a / $b;
}

func main() int {
    # Successful division
    try {
        my int $result = divide(10, 2);
        say("10 / 2 = " . $result);
    } catch ($e) {
        say("Error: " . $e);
    }

    # Division by zero
    try {
        my int $result = divide(10, 0);
        say("This won't print");
    } catch ($e) {
        say("Caught error: " . $e);
    }

    # Nested try/catch
    try {
        try {
            throw "inner error";
        } catch ($e) {
            say("Inner caught: " . $e);
            throw "rethrown: " . $e;
        }
    } catch ($e) {
        say("Outer caught: " . $e);
    }

    say("Program continues...");
    return 0;
}
```

### Validation with Exceptions

```strada
# validation.strada
func validate_age(int $age) void {
    if ($age < 0) {
        throw "Age cannot be negative";
    }
    if ($age > 150) {
        throw "Age seems unrealistic";
    }
}

func validate_email(str $email) void {
    if ($email !~ /@/) {
        throw "Email must contain @";
    }
}

func register_user(str $name, int $age, str $email) scalar {
    validate_age($age);
    validate_email($email);

    return {
        name => $name,
        age => $age,
        email => $email
    };
}

func main() int {
    # Valid user
    try {
        my scalar $user = register_user("Alice", 30, "alice@example.com");
        say("Registered: " . $user->{"name"});
    } catch ($e) {
        say("Failed: " . $e);
    }

    # Invalid age
    try {
        my scalar $user = register_user("Bob", -5, "bob@example.com");
    } catch ($e) {
        say("Failed: " . $e);
    }

    # Invalid email
    try {
        my scalar $user = register_user("Charlie", 25, "invalid");
    } catch ($e) {
        say("Failed: " . $e);
    }

    return 0;
}
```

---

## 14. Complete Applications

### Todo List Manager

```strada
# todo.strada
package Todo;

my array @todos = ();
my int $next_id = 1;

func add(str $task) int {
    my scalar $todo = {
        id => $next_id,
        task => $task,
        done => 0
    };
    push(@todos, $todo);
    $next_id = $next_id + 1;
    return $todo->{"id"};
}

func complete(int $id) int {
    foreach my scalar $todo (@todos) {
        if ($todo->{"id"} == $id) {
            $todo->{"done"} = 1;
            return 1;
        }
    }
    return 0;
}

func list() void {
    say("\nTodo List:");
    say("-----------");

    if (size(@todos) == 0) {
        say("  (empty)");
        return;
    }

    foreach my scalar $todo (@todos) {
        my str $status = $todo->{"done"} ? "[x]" : "[ ]";
        say("  " . $todo->{"id"} . ". " . $status . " " . $todo->{"task"});
    }
    say("");
}

func pending_count() int {
    my int $count = 0;
    foreach my scalar $todo (@todos) {
        if (!$todo->{"done"}) {
            $count = $count + 1;
        }
    }
    return $count;
}

package main;

func main() int {
    say("Todo List Manager");

    Todo::add("Learn Strada");
    Todo::add("Write documentation");
    Todo::add("Build a project");

    Todo::list();

    say("Completing task 1...");
    Todo::complete(1);

    Todo::list();

    say("Pending tasks: " . Todo::pending_count());

    return 0;
}
```

### Simple Calculator

```strada
# calculator.strada
func calculate(str $expr) num {
    my array @parts = split(" ", $expr);

    if (size(@parts) != 3) {
        throw "Invalid expression. Use: num op num";
    }

    my num $a = cast_num($parts[0]);
    my str $op = $parts[1];
    my num $b = cast_num($parts[2]);

    if ($op eq "+") {
        return $a + $b;
    }
    if ($op eq "-") {
        return $a - $b;
    }
    if ($op eq "*") {
        return $a * $b;
    }
    if ($op eq "/") {
        if ($b == 0) {
            throw "Division by zero";
        }
        return $a / $b;
    }
    if ($op eq "^") {
        return math::pow($a, $b);
    }

    throw "Unknown operator: " . $op;
}

func main() int {
    say("Simple Calculator");
    say("Enter expressions like: 5 + 3");
    say("Operators: + - * / ^");
    say("");

    my array @tests = (
        "10 + 5",
        "20 - 8",
        "6 * 7",
        "100 / 4",
        "2 ^ 10"
    );

    foreach my str $expr (@tests) {
        try {
            my num $result = calculate($expr);
            say($expr . " = " . $result);
        } catch ($e) {
            say($expr . " => Error: " . $e);
        }
    }

    return 0;
}
```

### Configuration Parser

```strada
# config_parser.strada
func parse_config(str $content) scalar {
    my scalar $config = {};
    my array @lines = split("\n", $content);

    foreach my str $line (@lines) {
        # Skip empty lines and comments
        my str $trimmed = trim($line);
        if (length($trimmed) == 0) {
            next;
        }
        if ($trimmed =~ /^#/) {
            next;
        }

        # Parse key=value
        my int $eq_pos = index($trimmed, "=");
        if ($eq_pos > 0) {
            my str $key = trim(substr($trimmed, 0, $eq_pos));
            my str $value = trim(substr($trimmed, $eq_pos + 1));
            $config->{$key} = $value;
        }
    }

    return $config;
}

func main() int {
    my str $config_text = "# Application Configuration
host=localhost
port=8080
debug=true
# Database settings
db_host=127.0.0.1
db_port=5432
db_name=myapp
";

    say("Parsing configuration...\n");

    my scalar $config = parse_config($config_text);

    say("Configuration values:");
    foreach my str $key (keys(%{$config})) {
        say("  " . $key . " = " . $config->{$key});
    }

    say("\nAccessing specific values:");
    say("  Host: " . $config->{"host"});
    say("  Port: " . $config->{"port"});
    say("  Debug: " . $config->{"debug"});

    return 0;
}
```

---

## 15. String Repetition and Transliteration

### The x Operator

```strada
# string_repeat.strada
func main() int {
    # Build a formatted header
    my str $title = "Report";
    my int $width = 40;
    my str $border = "=" x $width;

    say($border);
    # Center the title
    my int $pad = ($width - length($title)) / 2;
    say(" " x $pad . $title);
    say($border);

    # Create a simple bar chart
    my array @values = (3, 7, 2, 9, 5);
    my array @labels = ("Mon", "Tue", "Wed", "Thu", "Fri");

    for (my int $i = 0; $i < size(@values); $i = $i + 1) {
        say($labels[$i] . " | " . "#" x $values[$i]);
    }

    return 0;
}
```

Output:
```
========================================
                 Report
========================================
Mon | ###
Tue | #######
Wed | ##
Thu | #########
Fri | #####
```

### Character Transliteration (tr///)

```strada
# transliterate.strada
func main() int {
    # ROT13 cipher
    my str $message = "Hello World";
    my str $encoded = ($message =~ tr/A-Za-z/N-ZA-Mn-za-m/r);
    say("Original: " . $message);
    say("ROT13:    " . $encoded);     # "Uryyb Jbeyq"

    # Decode it back (ROT13 is its own inverse)
    my str $decoded = ($encoded =~ tr/A-Za-z/N-ZA-Mn-za-m/r);
    say("Decoded:  " . $decoded);     # "Hello World"

    # Count specific characters
    my str $text = "The quick brown fox";
    my int $vowels = ($text =~ tr/aeiouAEIOU/aeiouAEIOU/);
    say("Vowel count: " . $vowels);   # 5

    # Clean up data: squeeze whitespace and remove non-alpha
    my str $dirty = "  Hello   World!!  123  ";
    $dirty =~ tr/ / /s;               # Squeeze spaces
    $dirty =~ tr/a-zA-Z //cd;         # Delete non-alpha, non-space (complement + delete)
    say("Cleaned: '" . trim($dirty) . "'");  # "Hello World"

    return 0;
}
```

---

## 16. Splice: Array Surgery

```strada
# splice_demo.strada
func main() int {
    # Build a playlist and modify it
    my array @playlist = ("Song A", "Song B", "Song C", "Song D", "Song E");

    say("Original playlist:");
    foreach my str $s (@playlist) { say("  " . $s); }

    # Remove "Song C" (index 2, length 1)
    my array @removed = splice(@playlist, 2, 1);
    say("\nRemoved: " . $removed[0]);

    # Insert two songs at position 1
    my array @new_songs = ("Hit 1", "Hit 2");
    splice(@playlist, 1, 0, @new_songs);

    say("\nAfter insert:");
    foreach my str $s (@playlist) { say("  " . $s); }

    # Replace the last 2 songs (negative offset)
    my array @finale = ("Grand Finale");
    splice(@playlist, -2, 2, @finale);

    say("\nFinal playlist:");
    foreach my str $s (@playlist) { say("  " . $s); }

    return 0;
}
```

Output:
```
Original playlist:
  Song A
  Song B
  Song C
  Song D
  Song E

Removed: Song C

After insert:
  Song A
  Hit 1
  Hit 2
  Song B
  Song D
  Song E

Final playlist:
  Song A
  Hit 1
  Hit 2
  Song B
  Grand Finale
```

---

## 17. Hash Iteration with each() and Tied Hashes

### Iterating with each()

```strada
# each_demo.strada
func main() int {
    my hash %inventory = ();
    $inventory{"apples"} = 12;
    $inventory{"bananas"} = 5;
    $inventory{"oranges"} = 8;

    # Walk through key-value pairs one at a time
    say("Inventory:");
    my array @pair = each(%inventory);
    while (size(@pair) > 0) {
        say("  " . $pair[0] . ": " . $pair[1]);
        @pair = each(%inventory);
    }

    return 0;
}
```

### Tied Hash: Case-Insensitive Keys

```strada
# tied_hash.strada
package CIHash;

# TIEHASH is called by tie() - create the backing store
func TIEHASH(str $class) scalar {
    my hash %self = ();
    $self{"_data"} = {};
    return bless(\%self, "CIHash");
}

# STORE is called on $h{"Key"} = value
func STORE(scalar $self, str $key, scalar $value) void {
    $self->{"_data"}->{lc($key)} = $value;
}

# FETCH is called on $h{"Key"} read
func FETCH(scalar $self, str $key) scalar {
    return $self->{"_data"}->{lc($key)};
}

# EXISTS is called by exists($h{"Key"})
func EXISTS(scalar $self, str $key) int {
    return exists($self->{"_data"}->{lc($key)});
}

package main;

func main() int {
    my hash %headers = ();
    tie(%headers, "CIHash");

    # Store with mixed case
    $headers{"Content-Type"} = "text/html";
    $headers{"X-Custom"} = "hello";

    # Retrieve with any case
    say($headers{"content-type"});   # "text/html"
    say($headers{"CONTENT-TYPE"});   # "text/html"
    say($headers{"x-custom"});       # "hello"

    # Check existence
    if (exists($headers{"CONTENT-TYPE"})) {
        say("Header exists!");
    }

    # Check tied status
    my scalar $obj = tied(%headers);
    if (defined($obj)) {
        say("Hash is tied to: " . blessed($obj));
    }

    untie(%headers);
    return 0;
}
```

---

## 18. Select, Local, and the /e Regex Modifier

### Redirecting Output with select()

```strada
# select_demo.strada
func main() int {
    # Write a log file while also printing to stdout
    my scalar $log = core::open("/tmp/app.log", "w");

    say("Starting application...");

    # Redirect default output to log
    my scalar $prev = select($log);
    say("This line goes to the log");
    say("And so does this one");

    # Restore stdout
    select($prev);
    say("Back on stdout");

    core::close($log);

    # Verify the log
    my str $content = core::slurp("/tmp/app.log");
    say("Log contains: " . content);

    return 0;
}
```

### Dynamic Scoping with local()

```strada
# local_demo.strada
our str $indent = "";

func log_msg(str $msg) void {
    say($indent . $msg);
}

func process_section(str $name) void {
    local($indent) = $indent . "  ";  # Add 2 spaces for this scope
    log_msg("Processing: " . $name);
    if ($name eq "main") {
        process_section("sub-A");
        process_section("sub-B");
    }
}   # $indent automatically restored here

func main() int {
    log_msg("Start");
    process_section("main");
    log_msg("End");
    return 0;
}
```

Output:
```
Start
  Processing: main
    Processing: sub-A
    Processing: sub-B
End
```

### Evaluate Replacement with /e

```strada
# regex_eval.strada
func main() int {
    # Replace template variables with computed values
    my str $template = "Total: {100+200}, Tax: {300*0.08}";

    # Use /e to evaluate each matched expression
    # Here we double every number found in the text
    my str $text = "Order: 3 items at 15 each, shipping 5";
    $text =~ s/(\d+)/$1 * 2/eg;
    say($text);  # "Order: 6 items at 30 each, shipping 10"

    # Wrap words in brackets using /e with $1 capture variable
    my str $s = "hello world";
    $s =~ s/(\w+)/"[" . $1 . "]"/eg;
    say($s);  # "[hello] [world]"

    return 0;
}
```

---

## Running Examples

All examples can be compiled and run with:

```bash
./strada -r example.strada
```

Or compile first, then run:

```bash
./strada example.strada
./example
```

For examples in the `examples/` directory:

```bash
make run PROG=example_name
```

---

## See Also

- [Tutorial](TUTORIAL.md) - Step-by-step learning
- [Language Manual](LANGUAGE_MANUAL.md) - Complete reference
- [Quick Reference](QUICK_REFERENCE.md) - Syntax cheat sheet
- [OOP Guide](OOP_GUIDE.md) - Object-oriented programming
- [FFI Guide](FFI_GUIDE.md) - C integration

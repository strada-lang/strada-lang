# Strada FFI Guide

This guide covers integrating Strada with C code through the Foreign Function Interface (FFI).

## Table of Contents

1. [Overview](#overview)
2. [Calling C from Strada](#calling-c-from-strada)
3. [Writing C Libraries for Strada](#writing-c-libraries-for-strada)
4. [Calling Strada from C](#calling-strada-from-c)
5. [Extern Functions](#extern-functions)
6. [Native Structs](#native-structs)
7. [Advanced Topics](#advanced-topics)
8. [Best Practices](#best-practices)

---

## Overview

Strada provides multiple ways to integrate with C:

| Method | Use Case | Difficulty |
|--------|----------|------------|
| Dynamic FFI | Call existing C libraries at runtime | Easy |
| StradaValue FFI | C libraries designed for Strada | Medium |
| `__C__` blocks | Inline C in Strada | Medium |
| Embedding | Call Strada from C programs | Advanced |

---

## Calling C from Strada

### Loading Shared Libraries

```strada
# Load a shared library
my int $lib = core::dl_open("libfoo.so");

# Check for errors
if ($lib == 0) {
    die("Failed to load library: " . core::dl_error());
}

# Get a function pointer
my int $func = core::dl_sym($lib, "function_name");

if ($func == 0) {
    die("Function not found: " . core::dl_error());
}

# ... use the function ...

# Close when done
core::dl_close($lib);
```

### Library Search Paths

```strada
# Absolute path
my int $lib = core::dl_open("/usr/lib/libfoo.so");

# Relative path
my int $lib = core::dl_open("./libfoo.so");

# System library (uses LD_LIBRARY_PATH)
my int $lib = core::dl_open("libfoo.so.1");

# Common system libraries
my int $libm = core::dl_open("libm.so.6");       # Math
my int $libc = core::dl_open("libc.so.6");       # C standard
my int $libssl = core::dl_open("libssl.so.3");   # OpenSSL
```

### Basic FFI Calls

For C functions that take simple numeric arguments:

```strada
# int64_t func(int64_t a, int64_t b, ...)
my int $result = core::dl_call_int($func_ptr, [$arg1, $arg2]);

# double func(double a, double b, ...)
my num $result = core::dl_call_num($func_ptr, [$arg1, $arg2]);

# void func(args...)
core::dl_call_void($func_ptr, [$arg1, $arg2]);
```

**Example: Calling libm**

```strada
func main() int {
    my int $libm = core::dl_open("libm.so.6");

    # double sqrt(double x)
    my int $sqrt = core::dl_sym($libm, "sqrt");
    my num $result = core::dl_call_num($sqrt, [16.0]);
    say("sqrt(16) = " . $result);  # 4.0

    # double pow(double x, double y)
    my int $pow = core::dl_sym($libm, "pow");
    $result = core::dl_call_num($pow, [2.0, 10.0]);
    say("pow(2, 10) = " . $result);  # 1024.0

    core::dl_close($libm);
    return 0;
}
```

### StradaValue FFI Calls

For C functions designed to work with Strada values:

```strada
# C function receives StradaValue* arguments
my int $result = core::dl_call_int_sv($func_ptr, [$sv1, $sv2]);
my str $result = core::dl_call_str_sv($func_ptr, [$sv1, $sv2]);
core::dl_call_void_sv($func_ptr, [$sv1, $sv2]);
```

The `_sv` variants pass `StradaValue*` pointers directly to the C function.

---

## Writing C Libraries for Strada

### Basic Structure

```c
// mylib.c
#include "strada_runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// Function that adds two integers
int64_t my_add(StradaValue *a, StradaValue *b) {
    int64_t va = strada_to_int(a);
    int64_t vb = strada_to_int(b);
    return va + vb;
}

// Function that returns a string
char* my_greet(StradaValue *name) {
    const char *n = strada_to_str(name);
    char *result = malloc(strlen(n) + 10);
    sprintf(result, "Hello, %s!", n);
    return result;  // Caller (Strada) takes ownership
}

// Function that modifies data
void my_double_array(StradaValue *arr) {
    StradaArray *a = strada_deref_array(arr);
    for (size_t i = 0; i < a->length; i++) {
        int64_t val = strada_to_int(a->data[i]);
        a->data[i] = strada_new_int(val * 2);
    }
}
```

### Building the Library

```bash
# Basic build
gcc -shared -fPIC -o libmylib.so mylib.c -I/path/to/strada/runtime

# With optimization
gcc -O2 -shared -fPIC -o libmylib.so mylib.c -I/path/to/strada/runtime

# With debugging
gcc -g -shared -fPIC -o libmylib.so mylib.c -I/path/to/strada/runtime
```

### Using the Library

```strada
func main() int {
    my int $lib = core::dl_open("./libmylib.so");

    # Call my_add
    my int $add = core::dl_sym($lib, "my_add");
    my int $sum = core::dl_call_int_sv($add, [10, 20]);
    say("10 + 20 = " . $sum);

    # Call my_greet
    my int $greet = core::dl_sym($lib, "my_greet");
    my str $msg = core::dl_call_str_sv($greet, ["World"]);
    say($msg);

    core::dl_close($lib);
    return 0;
}
```

### Runtime API Reference

Key functions for C library authors:

```c
// Type conversions
int64_t strada_to_int(StradaValue *sv);
double strada_to_num(StradaValue *sv);
const char* strada_to_str(StradaValue *sv);
int strada_to_bool(StradaValue *sv);

// Value constructors
StradaValue* strada_new_int(int64_t value);
StradaValue* strada_new_num(double value);
StradaValue* strada_new_str(const char *value);
StradaValue* strada_new_array(void);
StradaValue* strada_new_hash(void);
StradaValue* strada_new_undef(void);

// Array operations
void strada_array_push(StradaArray *arr, StradaValue *val);
StradaValue* strada_array_get(StradaArray *arr, int64_t index);
void strada_array_set(StradaArray *arr, int64_t index, StradaValue *val);
int64_t strada_array_length(StradaArray *arr);

// Hash operations
void strada_hash_set(StradaHash *h, const char *key, StradaValue *val);
StradaValue* strada_hash_get(StradaHash *h, const char *key);
int strada_hash_exists(StradaHash *h, const char *key);

// Reference operations
StradaValue* strada_deref(StradaValue *ref);
StradaArray* strada_deref_array(StradaValue *ref);
StradaHash* strada_deref_hash(StradaValue *ref);

// Memory management
void strada_incref(StradaValue *sv);
void strada_decref(StradaValue *sv);
```

---

## Calling Strada from C

### Basic Setup

```c
// main.c
#include "strada_runtime.h"
#include <stdio.h>

// Required globals
StradaValue *ARGV = NULL;
StradaValue *ARGC = NULL;

// Declare Strada functions (from compiled .c file)
StradaValue* add(StradaValue *a, StradaValue *b);
StradaValue* greet(StradaValue *name);

int main(int argc, char **argv) {
    // Initialize globals
    ARGV = strada_new_array();
    for (int i = 0; i < argc; i++) {
        strada_array_push(ARGV->value.av, strada_new_str(argv[i]));
    }
    ARGC = strada_new_int(argc);

    // Call Strada functions
    StradaValue *result = add(strada_new_int(10), strada_new_int(20));
    printf("10 + 20 = %ld\n", strada_to_int(result));

    StradaValue *msg = greet(strada_new_str("World"));
    printf("%s\n", strada_to_str(msg));

    return 0;
}
```

### Strada Library Code

```strada
# mylib.strada
package mylib;

func add(int $a, int $b) int {
    return $a + $b;
}

func greet(str $name) str {
    return "Hello, " . $name . "!";
}
```

### Building

```bash
# Compile Strada to C (no main function since it's a library)
./stradac mylib.strada mylib.c

# Compile and link
gcc -o myprogram main.c mylib.c runtime/strada_runtime.c \
    -Iruntime -ldl -lm
```

### Creating Shared Libraries from Strada

```bash
# Compile Strada to C
./stradac mylib.strada mylib.c

# Create shared library
gcc -shared -fPIC -rdynamic -o libmylib.so mylib.c \
    runtime/strada_runtime.c -Iruntime -ldl -lm
```

---

## `__C__` Blocks

The preferred way to embed C code directly in Strada files:

### Top-Level `__C__` Blocks

For includes, globals, and helper C functions:

```strada
__C__ {
    #include <math.h>
    #include <string.h>

    static int helper_function(int a, int b) {
        return a + b;
    }
}
```

### Statement-Level `__C__` Blocks

For inline C code inside Strada functions:

```strada
func add_numbers(int $a, int $b) int {
    __C__ {
        int64_t va = strada_to_int(a);
        int64_t vb = strada_to_int(b);
        return strada_new_int(va + vb);
    }
}

func main() int {
    my int $result = add_numbers(10, 20);
    say("Result: " . $result);
    return 0;
}
```

### Key Functions for C Interop

| Function | Description |
|----------|-------------|
| `strada_to_int(sv)` | Extract int64_t from StradaValue* |
| `strada_to_num(sv)` | Extract double from StradaValue* |
| `strada_to_str(sv)` | Extract string (must free!) |
| `strada_new_int(i)` | Create StradaValue* from int64_t |
| `strada_new_num(n)` | Create StradaValue* from double |
| `strada_new_str(s)` | Create StradaValue* from string |
| `&strada_undef` | Return undef value |

---

## Advanced Topics

### Pointer Manipulation

```strada
# Get raw pointer to variable
my int $x = 42;
my int $ptr = core::int_ptr(\$x);

# Read through pointer
my int $val = core::ptr_deref_int($ptr);

# Write through pointer
core::ptr_set_int($ptr, 100);
say($x);  # 100

# String pointer
my str $s = "hello";
my int $str_ptr = core::str_ptr(\$s);
```

### Callbacks

C code can call back into Strada using closures:

```c
// C side
typedef StradaValue* (*StradaCallback)(StradaValue*);

void process_with_callback(StradaValue *data, StradaCallback cb) {
    // Process data
    StradaValue *result = cb(data);
    // Use result
}
```

```strada
# Strada side
my scalar $callback = func (scalar $val) {
    return $val * 2;
};

# Pass to C function
process_with_callback($data, $callback);
```

### Memory Management

When working with C:

```c
// Creating values that Strada will own
StradaValue* create_data() {
    StradaValue *arr = strada_new_array();
    strada_array_push(arr->value.av, strada_new_int(1));
    strada_array_push(arr->value.av, strada_new_int(2));
    return arr;  // Strada takes ownership, will free later
}

// Borrowing values (don't free)
void process_data(StradaValue *data) {
    // Use data, don't free it
    int64_t val = strada_to_int(data);
}

// Taking ownership (must free or return)
void consume_data(StradaValue *data) {
    // Use data
    strada_decref(data);  // Explicitly release
}
```

### Thread Safety

```c
// Reference counting is thread-safe
void thread_func(void *arg) {
    StradaValue *val = (StradaValue*)arg;
    strada_incref(val);  // Take a reference

    // Use val...

    strada_decref(val);  // Release reference
}
```

### Error Handling

```c
// Return undef on error
StradaValue* safe_divide(StradaValue *a, StradaValue *b) {
    int64_t divisor = strada_to_int(b);
    if (divisor == 0) {
        return strada_new_undef();
    }
    return strada_new_int(strada_to_int(a) / divisor);
}
```

```strada
my scalar $result = safe_divide(10, 0);
if (!defined($result)) {
    say("Division failed");
}
```

---

## Best Practices

### 1. Check for Errors

```strada
my int $lib = core::dl_open("libfoo.so");
if ($lib == 0) {
    die("Failed to load: " . core::dl_error());
}

my int $func = core::dl_sym($lib, "function");
if ($func == 0) {
    core::dl_close($lib);
    die("Symbol not found: " . core::dl_error());
}
```

### 2. Create Wrapper Functions

```strada
package MyLib;

my int $lib = 0;
my int $fn_add = 0;
my int $fn_greet = 0;

func init() int {
    $lib = core::dl_open("./libmylib.so");
    if ($lib == 0) {
        return 0;
    }

    $fn_add = core::dl_sym($lib, "my_add");
    $fn_greet = core::dl_sym($lib, "my_greet");

    return 1;
}

func cleanup() void {
    if ($lib != 0) {
        core::dl_close($lib);
        $lib = 0;
    }
}

func add(int $a, int $b) int {
    return core::dl_call_int_sv($fn_add, [$a, $b]);
}

func greet(str $name) str {
    return core::dl_call_str_sv($fn_greet, [$name]);
}
```

### 3. Use Type-Safe Wrappers in C

```c
// Good: Type-safe wrapper
StradaValue* safe_add(StradaValue *a, StradaValue *b) {
    if (!a || !b) return strada_new_undef();
    if (a->type != STRADA_INT || b->type != STRADA_INT) {
        return strada_new_undef();
    }
    return strada_new_int(a->value.iv + b->value.iv);
}
```

### 4. Document Memory Ownership

```c
/**
 * Creates a new array with the given values.
 * @return New StradaValue* - caller takes ownership
 */
StradaValue* create_array(void);

/**
 * Processes data without modifying it.
 * @param data Borrowed reference - do not free
 */
void process_data(StradaValue *data);
```

### 5. Link with rdynamic for Callbacks

```bash
# Required for C code to call back into Strada runtime
gcc -rdynamic -o program program.c runtime/strada_runtime.c \
    -Iruntime -ldl -lm
```

---

## Complete Example: OpenSSL Wrapper

### C Library (ssl_wrapper.c)

```c
#include "strada_runtime.h"
#include <openssl/ssl.h>
#include <openssl/err.h>

static int initialized = 0;

int64_t ssl_init(void) {
    if (!initialized) {
        SSL_library_init();
        OpenSSL_add_all_algorithms();
        SSL_load_error_strings();
        initialized = 1;
    }
    return 1;
}

int64_t ssl_connect(StradaValue *host_sv, StradaValue *port_sv) {
    const char *host = strada_to_str(host_sv);
    int port = (int)strada_to_int(port_sv);

    // Create connection...
    SSL_CTX *ctx = SSL_CTX_new(TLS_client_method());
    // ... setup and connect ...

    return (int64_t)ctx;  // Return handle
}

char* ssl_read(StradaValue *handle_sv, StradaValue *len_sv) {
    SSL *ssl = (SSL*)strada_to_int(handle_sv);
    int len = (int)strada_to_int(len_sv);

    char *buffer = malloc(len + 1);
    int bytes = SSL_read(ssl, buffer, len);
    buffer[bytes] = '\0';

    return buffer;
}

void ssl_close(StradaValue *handle_sv) {
    SSL *ssl = (SSL*)strada_to_int(handle_sv);
    SSL_shutdown(ssl);
    SSL_free(ssl);
}
```

### Build

```bash
gcc -shared -fPIC -o libssl_wrapper.so ssl_wrapper.c \
    -I/path/to/strada/runtime -lssl -lcrypto
```

### Strada Wrapper (lib/ssl.strada)

```strada
package ssl;

my int $lib = 0;
my int $fn_init = 0;
my int $fn_connect = 0;
my int $fn_read = 0;
my int $fn_close = 0;

func _load() int {
    if ($lib != 0) {
        return 1;
    }

    $lib = core::dl_open("lib/ssl/libssl_wrapper.so");
    if ($lib == 0) {
        return 0;
    }

    $fn_init = core::dl_sym($lib, "ssl_init");
    $fn_connect = core::dl_sym($lib, "ssl_connect");
    $fn_read = core::dl_sym($lib, "ssl_read");
    $fn_close = core::dl_sym($lib, "ssl_close");

    return 1;
}

func init() int {
    if (!_load()) { return 0; }
    return core::dl_call_int($fn_init, []);
}

func connect(str $host, int $port) int {
    if (!_load()) { return 0; }
    return core::dl_call_int_sv($fn_connect, [$host, $port]);
}

func read(int $handle, int $len) str {
    return core::dl_call_str_sv($fn_read, [$handle, $len]);
}

func close(int $handle) void {
    core::dl_call_void_sv($fn_close, [$handle]);
}
```

### Usage

```strada
use ssl;

func main() int {
    ssl::init();

    my int $conn = ssl::connect("example.com", 443);
    if ($conn == 0) {
        die("Connection failed");
    }

    # ... send request ...

    my str $response = ssl::read($conn, 4096);
    say($response);

    ssl::close($conn);
    return 0;
}
```

---

## See Also

- [Runtime API](RUNTIME_API.md) - Complete C API reference
- [Examples](EXAMPLES.md) - More FFI examples
- [Perl Integration](PERL_INTEGRATION.md) - Calling Perl from Strada

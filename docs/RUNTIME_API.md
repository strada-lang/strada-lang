# Strada Runtime API Reference

The Strada runtime library provides the core functionality for Strada programs. This document describes the C API that generated code uses.

## Namespace Mapping

In Strada source code, built-in functions use namespaces:
- `math::sin()` in Strada → `strada_sin()` in C
- `core::fork()` in Strada → `strada_fork()` in C
- `say()` in Strada → `strada_say()` in C (core functions have no namespace)

The namespace prefix (`core::`, `math::`) is purely a compile-time organization. The runtime C functions always use the `strada_` prefix.

## Core Types

### StradaType Enum

```c
typedef enum {
    STRADA_UNDEF,    // Undefined value
    STRADA_INT,      // 64-bit integer
    STRADA_NUM,      // 64-bit float
    STRADA_STR,      // String
    STRADA_ARRAY,    // Array
    STRADA_HASH,     // Hash map
    STRADA_REF,      // Reference
    STRADA_CSTRUCT,  // C struct wrapper
    STRADA_CPOINTER, // Generic C pointer
    STRADA_CLOSURE   // Anonymous function with captured environment
} StradaType;
```

### StradaValue Structure

```c
typedef struct StradaValue {
    StradaType type;
    int refcount;
    union {
        int64_t iv;           // STRADA_INT
        double nv;            // STRADA_NUM
        char *sv;             // STRADA_STR
        StradaArray *av;      // STRADA_ARRAY
        StradaHash *hv;       // STRADA_HASH
        struct {              // STRADA_REF
            struct StradaValue *target;
            char ref_type;    // '$', '@', or '%'
        } rv;
        StradaCStruct *cs;    // STRADA_CSTRUCT
    } value;
} StradaValue;
```

## Value Constructors

### Primitive Types

```c
// Create undefined value
StradaValue* strada_new_undef(void);

// Create integer
StradaValue* strada_new_int(int64_t value);

// Create number (float)
StradaValue* strada_new_num(double value);

// Create string
StradaValue* strada_new_str(const char *value);
```

### Compound Types

```c
// Create empty array
StradaValue* strada_new_array(void);

// Create empty hash
StradaHash* strada_hash_new(void);

// Create reference to existing value
StradaValue* strada_new_ref(StradaValue *target, char ref_type);
```

### Anonymous Constructors

```c
// Create anonymous hash with key-value pairs
// Usage: strada_anon_hash(2, "name", name_val, "age", age_val)
StradaValue* strada_anon_hash(int pair_count, ...);

// Create anonymous array with elements
// Usage: strada_anon_array(3, elem1, elem2, elem3)
StradaValue* strada_anon_array(int element_count, ...);
```

## Type Conversions

```c
// Convert any value to integer
int64_t strada_to_int(StradaValue *sv);

// Convert any value to number
double strada_to_num(StradaValue *sv);

// Convert any value to string
char* strada_to_str(StradaValue *sv);

// Convert any value to boolean (for conditions)
int strada_to_bool(StradaValue *sv);
```

### Boolean Conversion Rules

| Type | True when |
|------|-----------|
| `UNDEF` | Never |
| `INT` | Non-zero |
| `NUM` | Non-zero |
| `STR` | Non-empty and not "0" |
| `ARRAY` | Non-empty |
| `HASH` | Non-empty |
| `REF` | Always |

## Array Operations

```c
// Create new array
StradaArray* strada_array_new(void);

// Get element at index
StradaValue* strada_array_get(StradaArray *arr, int64_t index);

// Set element at index
void strada_array_set(StradaArray *arr, int64_t index, StradaValue *value);

// Add element to end
void strada_array_push(StradaArray *arr, StradaValue *value);

// Remove element from end
StradaValue* strada_array_pop(StradaArray *arr);

// Remove element from beginning
StradaValue* strada_array_shift(StradaArray *arr);

// Add element to beginning
void strada_array_unshift(StradaArray *arr, StradaValue *value);

// Get array length
int64_t strada_array_length(StradaArray *arr);
```

## Hash Operations

```c
// Create new hash
StradaHash* strada_hash_new(void);

// Get value for key
StradaValue* strada_hash_get(StradaHash *hash, const char *key);

// Set value for key
void strada_hash_set(StradaHash *hash, const char *key, StradaValue *value);

// Check if key exists
int strada_hash_exists(StradaHash *hash, const char *key);

// Delete key
void strada_hash_delete(StradaHash *hash, const char *key);

// Get all keys as array
StradaArray* strada_hash_keys(StradaHash *hash);

// Get all values as array
StradaArray* strada_hash_values(StradaHash *hash);

// Get number of keys
int64_t strada_hash_size(StradaHash *hash);
```

## Reference Operations

```c
// Create reference to value
StradaValue* strada_new_ref(StradaValue *target, char ref_type);

// Dereference to get target value
StradaValue* strada_deref(StradaValue *ref);

// Set value through a scalar reference (modifies original variable)
StradaValue* strada_deref_set(StradaValue *ref, StradaValue *new_value);

// Dereference array reference
StradaArray* strada_deref_array(StradaValue *ref);

// Dereference hash reference
StradaHash* strada_deref_hash(StradaValue *ref);

// Check if value is a reference
int strada_is_ref(StradaValue *sv);

// Get reference type string ("SCALAR", "ARRAY", "HASH")
const char* strada_reftype(StradaValue *sv);
```

## Closure Operations

Closures are anonymous functions that can capture variables from their enclosing scope.

### StradaClosure Structure

```c
typedef struct StradaClosure {
    void *func_ptr;           // Pointer to generated C function
    int param_count;          // Number of parameters
    int capture_count;        // Number of captured variables
    StradaValue ***captures;  // Array of pointers to pointers (capture-by-reference)
} StradaClosure;
```

### Closure Functions

```c
// Create a new closure
// func: pointer to the generated C function
// params: number of parameters the function takes
// captures: number of captured variables
// cap_array: array of pointers to captured variable pointers
StradaValue* strada_closure_new(void *func, int params, int captures, StradaValue ***cap_array);

// Call a closure with arguments
// closure: the closure value to call
// argc: number of arguments
// ...: the arguments (StradaValue* variadic)
StradaValue* strada_closure_call(StradaValue *closure, int argc, ...);

// Get the captures array from a closure
StradaValue*** strada_closure_get_captures(StradaValue *closure);
```

### Closure Usage Example

Generated code for a closure that captures a variable:

```c
// Original Strada:
// my int $x = 10;
// my scalar $f = func (int $n) { return $n * $x; };
// say($f->(5));

// Generated C:

// Forward declaration
StradaValue* __anon_func_0(StradaValue ***__captures, StradaValue *n);

// In the function body:
StradaValue *x = strada_new_int(10);
StradaValue *f = strada_closure_new((void*)&__anon_func_0, 1, 1, (StradaValue**[]){&x});
strada_say(strada_closure_call(f, 1, strada_new_int(5)));

// Anonymous function definition:
StradaValue* __anon_func_0(StradaValue ***__captures, StradaValue *n) {
    // (*__captures[0]) dereferences to get the captured variable
    return strada_new_num(strada_to_num(n) * strada_to_num((*__captures[0])));
}
```

### Capture-by-Reference

Closures capture variables by reference using triple pointers:

- `StradaValue ***captures` - pointer to array of double pointers
- `(*__captures[i])` - dereference to access the captured StradaValue*
- Assignments like `(*__captures[i]) = value` modify the original variable

This allows mutations inside the closure to be visible in the enclosing scope.

## Thread Operations

Strada provides POSIX thread support with thread-safe reference counting.

### Thread Structures

```c
typedef struct StradaThread {
    pthread_t thread;
    StradaValue *closure;     /* The closure to run */
    StradaValue *result;      /* Return value from thread */
} StradaThread;

typedef struct StradaMutex {
    pthread_mutex_t mutex;
} StradaMutex;

typedef struct StradaCond {
    pthread_cond_t cond;
} StradaCond;
```

### Thread Functions

```c
// Create a new thread running the given closure
// Returns a CPOINTER containing the thread handle
StradaValue* strada_thread_create(StradaValue *closure);

// Wait for thread to complete, returns the thread's result
StradaValue* strada_thread_join(StradaValue *thread_val);

// Detach thread (runs independently)
StradaValue* strada_thread_detach(StradaValue *thread_val);

// Get current thread ID
StradaValue* strada_thread_self(void);
```

### Mutex Functions

```c
// Create a new mutex
StradaValue* strada_mutex_new(void);

// Lock mutex (blocks if already locked)
StradaValue* strada_mutex_lock(StradaValue *mutex);

// Try to lock mutex (non-blocking, returns 0 on success)
StradaValue* strada_mutex_trylock(StradaValue *mutex);

// Unlock mutex
StradaValue* strada_mutex_unlock(StradaValue *mutex);

// Destroy mutex and free resources
StradaValue* strada_mutex_destroy(StradaValue *mutex);
```

### Condition Variable Functions

```c
// Create a new condition variable
StradaValue* strada_cond_new(void);

// Wait on condition (atomically unlocks mutex while waiting)
StradaValue* strada_cond_wait(StradaValue *cond, StradaValue *mutex);

// Signal one waiting thread
StradaValue* strada_cond_signal(StradaValue *cond);

// Signal all waiting threads
StradaValue* strada_cond_broadcast(StradaValue *cond);

// Destroy condition variable and free resources
StradaValue* strada_cond_destroy(StradaValue *cond);
```

### Thread Safety

Reference counting uses atomic operations for thread safety:

```c
void strada_incref(StradaValue *sv) {
    if (sv) __sync_add_and_fetch(&sv->refcount, 1);
}

void strada_decref(StradaValue *sv) {
    if (!sv) return;
    if (__sync_sub_and_fetch(&sv->refcount, 1) <= 0) {
        strada_free_value(sv);
    }
}
```

**Note:** Programs using threads must link with `-lpthread`.

## Async/Await Operations

Strada provides a thread pool-based async/await system.

### StradaFuture Structure

```c
typedef enum {
    FUTURE_PENDING = 0,
    FUTURE_RUNNING = 1,
    FUTURE_COMPLETED = 2,
    FUTURE_CANCELLED = 3,
    FUTURE_TIMEOUT = 4
} StradaFutureState;

typedef struct StradaFuture {
    pthread_mutex_t mutex;
    pthread_cond_t cond;
    StradaValue *result;
    StradaValue *error;
    StradaValue *closure;
    StradaFutureState state;
    int cancel_requested;
    struct timespec deadline;
    int has_deadline;
} StradaFuture;
```

### Thread Pool Functions

```c
// Initialize thread pool with specified workers (0 = default 4)
void strada_pool_init(int num_workers);

// Shutdown thread pool
void strada_pool_shutdown(void);

// Submit future to thread pool for execution
void strada_pool_submit(StradaFuture *future);
```

### Future Functions

```c
// Create a new future from a closure
// The closure is submitted to the thread pool for execution
StradaValue* strada_future_new(StradaValue *closure);

// Await: block until future completes
// Returns the result value (or throws on error/cancel)
StradaValue* strada_future_await(StradaValue *future);

// Await with timeout (milliseconds)
// Throws "Future timed out" if deadline exceeded
StradaValue* strada_future_await_timeout(StradaValue *future, int64_t timeout_ms);

// Non-blocking check if future is done
int strada_future_is_done(StradaValue *future);

// Try to get result without blocking (returns undef if not done)
StradaValue* strada_future_try_get(StradaValue *future);

// Request cancellation of a future
void strada_future_cancel(StradaValue *future);

// Check if future was cancelled
int strada_future_is_cancelled(StradaValue *future);
```

### Combinator Functions

```c
// Wait for all futures to complete, return array of results
StradaValue* strada_future_all(StradaValue *futures_array);

// Wait for first future to complete, cancel others
StradaValue* strada_future_race(StradaValue *futures_array);

// Wrapper for timeout (takes StradaValue* timeout_ms)
StradaValue* strada_async_timeout(StradaValue *future, StradaValue *timeout_ms);
```

### Strada Mapping

| Strada | C Runtime |
|--------|-----------|
| `async func name() { }` | Generates closure + `strada_future_new()` |
| `await $future` | `strada_future_await(future)` |
| `async::all(\@arr)` | `strada_future_all(arr)` |
| `async::race(\@arr)` | `strada_future_race(arr)` |
| `async::timeout($f, $ms)` | `strada_async_timeout(f, ms)` |
| `async::cancel($f)` | `strada_future_cancel(f)` |
| `async::is_done($f)` | `strada_future_is_done(f)` |
| `async::is_cancelled($f)` | `strada_future_is_cancelled(f)` |
| `async::pool_init($n)` | `strada_pool_init(n)` |
| `async::pool_shutdown()` | `strada_pool_shutdown()` |

### Usage Example

```c
// This is generated by async func compute(int $n) int { return $n * 2; }

static StradaValue* __async_compute_inner(StradaValue ***__captures) {
    StradaValue *n = (*__captures[0]);
    return strada_new_int(strada_to_int(n) * 2);
}

StradaValue* compute(StradaValue* n) {
    StradaValue *__closure = strada_closure_new(
        (void*)&__async_compute_inner, 0, 1, (StradaValue**[]){&n});
    return strada_future_new(__closure);
}

// Usage:
StradaValue *future = compute(strada_new_int(21));
StradaValue *result = strada_future_await(future);  // 42
```

## Channel Operations

Channels provide thread-safe message passing between async tasks.

### StradaChannel Structure

```c
typedef struct StradaChannelNode {
    StradaValue *value;
    struct StradaChannelNode *next;
} StradaChannelNode;

typedef struct StradaChannel {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    StradaChannelNode *head;
    StradaChannelNode *tail;
    int size;
    int capacity;  // 0 = unbounded
    int closed;
} StradaChannel;
```

### Channel Functions

```c
// Create channel (capacity 0 = unbounded)
StradaValue* strada_channel_new(int capacity);

// Send value (blocks if full, throws if closed)
void strada_channel_send(StradaValue *channel, StradaValue *value);

// Receive value (blocks if empty, returns undef if closed and empty)
StradaValue* strada_channel_recv(StradaValue *channel);

// Non-blocking send (returns 1 on success, 0 if full/closed)
int strada_channel_try_send(StradaValue *channel, StradaValue *value);

// Non-blocking receive (returns undef if empty)
StradaValue* strada_channel_try_recv(StradaValue *channel);

// Close channel
void strada_channel_close(StradaValue *channel);

// Check if closed
int strada_channel_is_closed(StradaValue *channel);

// Get number of items
int strada_channel_len(StradaValue *channel);
```

### Strada Channel Mapping

| Strada | C Runtime |
|--------|-----------|
| `async::channel()` | `strada_channel_new(0)` |
| `async::channel($n)` | `strada_channel_new(n)` |
| `async::send($ch, $v)` | `strada_channel_send(ch, v)` |
| `async::recv($ch)` | `strada_channel_recv(ch)` |
| `async::try_send($ch, $v)` | `strada_channel_try_send(ch, v)` |
| `async::try_recv($ch)` | `strada_channel_try_recv(ch)` |
| `async::close($ch)` | `strada_channel_close(ch)` |
| `async::is_closed($ch)` | `strada_channel_is_closed(ch)` |
| `async::len($ch)` | `strada_channel_len(ch)` |

## Mutex Operations

Mutexes use the existing CPOINTER-based implementation.

### Mutex Functions

```c
// Create new mutex
StradaValue* strada_mutex_new(void);

// Acquire lock (blocking)
StradaValue* strada_mutex_lock(StradaValue *mutex);

// Non-blocking lock attempt (returns 0=success, EBUSY=locked)
StradaValue* strada_mutex_trylock(StradaValue *mutex);

// Release lock
StradaValue* strada_mutex_unlock(StradaValue *mutex);

// Destroy mutex
StradaValue* strada_mutex_destroy(StradaValue *mutex);
```

### Strada Mutex Mapping

| Strada | C Runtime |
|--------|-----------|
| `async::mutex()` | `strada_mutex_new()` |
| `async::lock($m)` | `strada_mutex_lock(m)` |
| `async::unlock($m)` | `strada_mutex_unlock(m)` |
| `async::try_lock($m)` | `strada_mutex_trylock(m)` |
| `async::mutex_destroy($m)` | `strada_mutex_destroy(m)` |

## Atomic Operations

Lock-free integer operations using GCC atomic builtins.

### StradaAtomicValue Structure

```c
typedef struct StradaAtomicValue {
    volatile int64_t value;
} StradaAtomicValue;
```

### Atomic Functions

```c
// Create atomic integer
StradaValue* strada_atomic_new(int64_t initial);

// Load value atomically
int64_t strada_atomic_load(StradaValue *atomic);

// Store value atomically
void strada_atomic_store(StradaValue *atomic, int64_t value);

// Add and return OLD value
int64_t strada_atomic_add(StradaValue *atomic, int64_t delta);

// Subtract and return OLD value
int64_t strada_atomic_sub(StradaValue *atomic, int64_t delta);

// Compare-and-swap (returns 1 if swapped)
int strada_atomic_cas(StradaValue *atomic, int64_t expected, int64_t desired);

// Increment and return NEW value
int64_t strada_atomic_inc(StradaValue *atomic);

// Decrement and return NEW value
int64_t strada_atomic_dec(StradaValue *atomic);
```

### Strada Atomic Mapping

| Strada | C Runtime |
|--------|-----------|
| `async::atomic($n)` | `strada_atomic_new(n)` |
| `async::atomic_load($a)` | `strada_atomic_load(a)` |
| `async::atomic_store($a, $v)` | `strada_atomic_store(a, v)` |
| `async::atomic_add($a, $d)` | `strada_atomic_add(a, d)` |
| `async::atomic_sub($a, $d)` | `strada_atomic_sub(a, d)` |
| `async::atomic_inc($a)` | `strada_atomic_inc(a)` |
| `async::atomic_dec($a)` | `strada_atomic_dec(a)` |
| `async::atomic_cas($a, $e, $n)` | `strada_atomic_cas(a, e, n)` |

## String Operations

```c
// Get string length
int64_t strada_length(const char *str);

// Concatenate two strings
char* strada_concat(const char *a, const char *b);

// Get substring
StradaValue* strada_substr(StradaValue *sv, int64_t start, int64_t len);

// Find substring (returns -1 if not found)
int64_t strada_index(const char *str, const char *substr);

// Find substring from end
int64_t strada_rindex(const char *str, const char *substr);

// Case conversion
char* strada_upper(const char *str);
char* strada_lower(const char *str);
char* strada_ucfirst(const char *str);
char* strada_lcfirst(const char *str);

// Whitespace trimming
char* strada_trim(const char *str);
char* strada_ltrim(const char *str);
char* strada_rtrim(const char *str);

// String manipulation
char* strada_reverse(const char *str);
char* strada_repeat(const char *str, int count);
char* strada_chomp(const char *str);
char* strada_chop(const char *str);

// Character operations
char* strada_chr(int code);
int strada_ord(const char *str);

// Join array to string
char* strada_join(const char *sep, StradaArray *arr);
```

## Binary/Byte Operations

These functions provide binary-safe byte-level string manipulation, useful for binary protocols and raw data handling.

```c
// Get first byte as integer (0-255), binary-safe
// Unlike strada_ord(), this treats strings as byte arrays, not UTF-8
int strada_ord_byte(StradaValue *sv);

// Get byte at position (0-indexed)
// Returns 0-255 on success, -1 if out of bounds
int strada_get_byte(StradaValue *sv, int pos);

// Set byte at position, returns new string
// Does not modify original
StradaValue* strada_set_byte(StradaValue *sv, int pos, int val);

// Get byte length (not UTF-8 character count)
int strada_byte_length(StradaValue *sv);

// Substring by byte positions (not character positions)
StradaValue* strada_byte_substr(StradaValue *sv, int start, int len);

// Pack values into binary string (Perl-like)
// fmt: format string with pack characters
// args: array of values to pack
StradaValue* strada_pack(const char *fmt, StradaValue *args);

// Unpack binary string to array (Perl-like)
// fmt: format string with unpack characters
// data: binary string to unpack
StradaValue* strada_unpack(const char *fmt, StradaValue *data);

// Base64 encode string to base64 format (RFC 4648)
// Returns base64-encoded string
StradaValue* strada_base64_encode(StradaValue *sv);

// Base64 decode string from base64 format
// Returns decoded binary string (may contain NUL bytes)
StradaValue* strada_base64_decode(StradaValue *sv);
```

### Pack/Unpack Format Characters

| Char | Description | Size |
|------|-------------|------|
| `c` | Signed char | 1 byte |
| `C` | Unsigned char | 1 byte |
| `s` | Signed short, native endian | 2 bytes |
| `S` | Unsigned short, native endian | 2 bytes |
| `n` | Unsigned short, big-endian (network) | 2 bytes |
| `v` | Unsigned short, little-endian (VAX) | 2 bytes |
| `l` | Signed long, native endian | 4 bytes |
| `L` | Unsigned long, native endian | 4 bytes |
| `N` | Unsigned long, big-endian (network) | 4 bytes |
| `V` | Unsigned long, little-endian (VAX) | 4 bytes |
| `q` | Signed quad, native endian | 8 bytes |
| `Q` | Unsigned quad, native endian | 8 bytes |
| `a` | ASCII string (null-padded) | variable |
| `A` | ASCII string (space-padded) | variable |
| `H` | Hex string (high nybble first) | variable |
| `x` | Null byte (pack only) | 1 byte |
| `X` | Backup one byte (pack only) | -1 byte |
| `@` | Go to absolute position (unpack only) | 0 bytes |

### Pack/Unpack Usage Example

```c
// Pack a network header: magic (4 bytes), port (2 bytes), flags (1 byte)
StradaValue *args = strada_anon_array(3,
    strada_new_int(0x12345678),  // magic
    strada_new_int(80),          // port
    strada_new_int(255));        // flags
StradaValue *header = strada_pack("NnC", args);

// Unpack the header
StradaValue *fields = strada_unpack("NnC", header);
// fields[0] = 0x12345678, fields[1] = 80, fields[2] = 255
```

## Output Functions

```c
// Print with newline
void strada_say(StradaValue *sv);

// Print without newline
void strada_print(StradaValue *sv);

// Formatted print
void strada_printf(const char *fmt, ...);

// Print to stderr
void strada_warn(const char *fmt, ...);

// Print error and exit
void strada_die(const char *msg);

// Debug dump (Data::Dumper style)
void strada_dumper(StradaValue *sv);
```

## File I/O

```c
// Open file (modes: "r", "w", "a", "r+")
StradaValue* strada_open(const char *path, const char *mode);

// Open in-memory handle from string content
// "r": fmemopen with copied buffer; "w"/"a": open_memstream
StradaValue* strada_open_str(const char *content, const char *mode);

// Type-dispatch open: if first_arg is STRADA_REF, opens in-memory handle
// (FH_MEMWRITE_REF writes back to referenced string on close);
// otherwise falls through to strada_open()
StradaValue* strada_open_sv(StradaValue *first_arg, StradaValue *mode_arg);

// Extract accumulated string from a write-mode memstream (without closing)
// Returns empty string if fh is not a memstream
StradaValue* strada_str_from_fh(StradaValue *fh);

// Close file
void strada_close(StradaValue *fh);

// Read line from file or socket (used by diamond operator <$fh>)
// For files: uses fgets()
// For sockets: reads byte-by-byte until \n, strips \r for CRLF handling
// Returns: string without trailing newline, or undef at EOF
StradaValue* strada_read_line(StradaValue *fh);

// Read line from stdin
StradaValue* strada_readline(void);

// Print to stdout (no newline)
void strada_print(StradaValue *sv);

// Print to stdout with newline
void strada_say(StradaValue *sv);

// Print to filehandle or socket (no newline)
// Works with both STRADA_FILEHANDLE and STRADA_SOCKET types
void strada_print_fh(StradaValue *sv, StradaValue *fh);

// Print to filehandle or socket with newline
// Works with both STRADA_FILEHANDLE and STRADA_SOCKET types
void strada_say_fh(StradaValue *sv, StradaValue *fh);

// Read entire file
StradaValue* strada_slurp(const char *path);

// Write to file
void strada_spew(const char *path, const char *data);
```

### Diamond Operator

The diamond operator `<$fh>` in Strada source compiles to a call to `strada_read_line()`:

```c
// Strada:  my str $line = <$fh>;
// C:       StradaValue *line = strada_read_line(fh);
```

This function handles both filehandles (`STRADA_FILEHANDLE`) and sockets (`STRADA_SOCKET`).

### Filehandle I/O

The `say()` and `print()` builtins with two arguments compile to `strada_say_fh()` and `strada_print_fh()`:

```c
// Strada:  say($fh, "hello");
// C:       strada_say_fh(strada_new_str("hello"), fh);

// Strada:  print($sock, $data);
// C:       strada_print_fh(data, sock);
```

## Regular Expressions

```c
// Test if pattern matches
int strada_regex_match(const char *str, const char *pattern);

// Replace first occurrence
char* strada_regex_replace(const char *str, const char *pattern, const char *replacement);

// Replace all occurrences
char* strada_regex_replace_all(const char *str, const char *pattern, const char *replacement);

// Split string by pattern
StradaArray* strada_regex_split(const char *pattern, const char *str);

// Capture groups
StradaArray* strada_regex_capture(const char *str, const char *pattern);
```

## Type Checking

```c
// Check if value is defined
StradaValue* strada_defined(StradaValue *sv);

// Get type name as string
const char* strada_typeof(StradaValue *sv);
```

## Memory Management

```c
// Increment reference count
void strada_refcnt_inc(StradaValue *sv);

// Decrement reference count (frees if zero)
void strada_refcnt_dec(StradaValue *sv);

// Free value immediately
void strada_free(StradaValue *sv);

// Set value to undef
void strada_undef(StradaValue *sv);

// Get reference count
int strada_refcount(StradaValue *sv);

// Deep clone a value
StradaValue* strada_clone(StradaValue *sv);
```

## Object-Oriented Programming

Strada supports Perl-style OOP with blessed references, inheritance (including multiple inheritance), and method dispatch.

### OOP Data Structures

```c
#define OOP_MAX_PACKAGES 64      /* Max registered packages */
#define OOP_MAX_METHODS 32       /* Max methods per package */
#define OOP_MAX_NAME_LEN 256     /* Max package/method name length */
#define OOP_MAX_PARENTS 16       /* Max parents for multiple inheritance */

typedef struct {
    char name[OOP_MAX_NAME_LEN];
    char parents[OOP_MAX_PARENTS][OOP_MAX_NAME_LEN];  /* Multiple parents */
    int parent_count;
    OopMethod methods[OOP_MAX_METHODS];
    int method_count;
} OopPackage;
```

### Blessing and Package Association

```c
// Associate a hash reference with a package name
// Returns the blessed reference
StradaValue* strada_bless(StradaValue *ref, const char *package);

// Get the package name of a blessed reference
// Returns empty string if not blessed
StradaValue* strada_blessed(StradaValue *ref);
```

### Inheritance

```c
// Set up inheritance relationship (child inherits from parent)
// Supports multiple inheritance - can call multiple times for same child
void strada_inherit(const char *child, const char *parent);

// Check if object is of a given type (follows inheritance chain)
// Returns true if obj's package equals pkg or inherits from pkg
int strada_isa(StradaValue *obj, const char *pkg);

// Check if object can perform a method
// Returns true if method is registered for the package or its parents
int strada_can(StradaValue *obj, const char *method);
```

### SUPER:: Calls

```c
// Call a method on the parent class
// obj: the object to call the method on
// from_package: the package making the SUPER:: call
// method: the method name to call
// args: packed array of arguments (from strada_pack_args)
StradaValue* strada_super_call(StradaValue *obj, const char *from_package,
                               const char *method, StradaValue *args);

// Set the current method's package (for SUPER:: resolution)
void strada_set_method_package(const char *pkg);

// Get the current method's package
const char* strada_get_method_package(void);

// Pack variadic arguments into an array for super calls
StradaValue* strada_pack_args(int count, ...);
```

### DESTROY Destructors

```c
// Call the DESTROY method on an object if it exists
// Called automatically when a blessed reference is freed
// Follows inheritance chain (calls parent DESTROY methods)
void strada_call_destroy(StradaValue *obj);
```

The `strada_free_value()` function automatically calls `strada_call_destroy()` for blessed references:

```c
void strada_free_value(StradaValue *sv) {
    if (!sv) return;
    /* Call DESTROY method if this is a blessed reference */
    if (sv->blessed_package) {
        strada_call_destroy(sv);
        free(sv->blessed_package);
        sv->blessed_package = NULL;
    }
    // ... rest of cleanup
}
```

### Package Management

```c
// Set the current package name
void strada_set_package(const char *pkg);

// Get the current package name (for __PACKAGE__)
const char* strada_current_package(void);

// Register a method for a package (internal use)
void strada_register_method(const char *package, const char *method, void *func);

// Look up a method in package's inheritance chain
void* oop_lookup_method(const char *package, const char *method);
```

### Method Resolution Order (MRO)

Methods are resolved using depth-first search through the inheritance graph:

1. Check the object's own package first
2. For each parent (in order declared), recursively search
3. First matching method found is used

```c
// Example: GermanShepherd -> Dog -> Animal
// GermanShepherd_init() { inherit("GermanShepherd", "Dog"); }
// Dog_init() { inherit("Dog", "Animal"); }

// Method lookup for $shepherd->speak():
// 1. Check GermanShepherd_speak - not found
// 2. Check Dog_speak - found, use it
```

### OOP Usage Example

```c
// Generated from Strada OOP code
void Dog_init(void) {
    strada_inherit("Dog", "Animal");
}

StradaValue* Dog_new(StradaValue* name) {
    StradaValue *self = strada_new_hash();
    strada_hash_set(self->value.hv, "name", name);
    strada_hash_set(self->value.hv, "species", strada_new_str("dog"));
    return strada_bless(strada_new_ref(self, '%'), "Dog");
}

void Dog_speak(StradaValue* self) {
    strada_say(strada_new_str(strada_concat(
        strada_to_str(strada_hash_get(strada_deref_hash(self), "name")),
        " says: Woof!")));
}

// DESTROY destructor - called automatically when object is freed
void Dog_DESTROY(StradaValue* self) {
    strada_say(strada_new_str("Dog being destroyed"));
    // Chain to parent DESTROY
    strada_super_call(self, "Dog", "DESTROY", strada_pack_args(1, self));
}
```

## Utility Functions

```c
// Exit program
void strada_exit(int code);

// Print stack trace
void strada_stacktrace(void);

// Get caller info
const char* strada_caller(int level);
```

## Socket Operations

```c
// Create server socket
StradaValue* strada_socket_server(int port);

// Create client socket
StradaValue* strada_socket_client(const char *host, int port);

// Accept connection
StradaValue* strada_socket_accept(StradaValue *server);

// Send data
int strada_socket_send(StradaValue *socket, const char *data);

// Receive data
StradaValue* strada_socket_recv(StradaValue *socket, int maxlen);

// Close socket
void strada_socket_close(StradaValue *socket);
```

## Process Control

```c
// Sleep for specified seconds (returns 0 on success, remaining time if interrupted)
StradaValue* strada_sleep(StradaValue *seconds);

// Sleep for specified microseconds (returns 0 on success)
StradaValue* strada_usleep(StradaValue *microseconds);

// Fork process (returns 0 in child, child PID in parent, -1 on error)
StradaValue* strada_fork(void);

// Wait for any child process to exit
StradaValue* strada_wait(void);

// Wait for specific child process
// options: 0 for blocking, WNOHANG for non-blocking
StradaValue* strada_waitpid(StradaValue *pid_val, StradaValue *options_val);

// Get current process ID
StradaValue* strada_getpid(void);

// Get parent process ID
StradaValue* strada_getppid(void);
```

## System Commands

```c
// Execute shell command via /bin/sh -c (like Perl's system())
// Returns exit status (0 = success)
StradaValue* strada_system(StradaValue *cmd_val);

// Execute command with explicit argument array (no shell interpretation)
// program: the executable path
// args_arr: array of command-line arguments
StradaValue* strada_system_argv(StradaValue *program, StradaValue *args_arr);

// Replace current process with command (via shell)
// Does not return on success
StradaValue* strada_exec(StradaValue *cmd_val);

// Replace current process with command (no shell)
// program: the executable path
// args_arr: array of command-line arguments
StradaValue* strada_exec_argv(StradaValue *program, StradaValue *args_arr);
```

## Process Name and Title

```c
// Set process name (visible in ps comm column, max 15 chars)
StradaValue* strada_setprocname(StradaValue *name_val);

// Get current process name
StradaValue* strada_getprocname(void);

// Set process title (visible in ps args column)
// Modifies argv[0] in place
StradaValue* strada_setproctitle(StradaValue *title_val);

// Get current process title (reads from /proc/self/cmdline)
StradaValue* strada_getproctitle(void);

// Initialize proctitle support (called automatically in main)
void strada_init_proctitle(int argc, char **argv);
```

## Inter-Process Communication (Pipes)

```c
// Create a pipe
// Returns array reference: [read_fd, write_fd] or undef on error
StradaValue* strada_pipe(void);

// Duplicate file descriptor (like dup2 syscall)
// oldfd: source file descriptor
// newfd: target file descriptor
StradaValue* strada_dup2(StradaValue *oldfd_val, StradaValue *newfd_val);

// Close a file descriptor
StradaValue* strada_close_fd(StradaValue *fd_val);

// Read from file descriptor
// fd_val: file descriptor
// size_val: maximum bytes to read
// Returns string with data read
StradaValue* strada_read_fd(StradaValue *fd_val, StradaValue *size_val);

// Write to file descriptor
// fd_val: file descriptor
// data_val: string data to write
// Returns number of bytes written
StradaValue* strada_write_fd(StradaValue *fd_val, StradaValue *data_val);

// Read all available data from file descriptor
// Reads until EOF
StradaValue* strada_read_all_fd(StradaValue *fd_val);

// Open file descriptor as FILE* for reading
StradaValue* strada_fdopen_read(StradaValue *fd_val);

// Open file descriptor as FILE* for writing
StradaValue* strada_fdopen_write(StradaValue *fd_val);
```

### Pipe Usage Example

```c
// Create pipe and fork
StradaValue *pipe_fds = strada_pipe();
StradaValue *pid = strada_fork();

if (strada_to_int(pid) == 0) {
    // Child process
    strada_close_fd(strada_array_get(pipe_fds->value.av, 0)); // Close read end
    strada_write_fd(strada_array_get(pipe_fds->value.av, 1),
                    strada_new_str("Hello from child"));
    strada_close_fd(strada_array_get(pipe_fds->value.av, 1));
    exit(0);
} else {
    // Parent process
    strada_close_fd(strada_array_get(pipe_fds->value.av, 1)); // Close write end
    StradaValue *data = strada_read_all_fd(strada_array_get(pipe_fds->value.av, 0));
    strada_close_fd(strada_array_get(pipe_fds->value.av, 0));
    strada_wait();
}
```

## C Struct Operations

For interfacing with C structures:

```c
// Create new C struct wrapper
StradaValue* strada_cstruct_new(const char *name, int size);

// Get pointer to struct data
void* strada_cstruct_ptr(StradaValue *cs);

// Set field value
void strada_cstruct_set_field(StradaValue *cs, const char *name, int offset, void *data, int size);

// Get field value
void* strada_cstruct_get_field(StradaValue *cs, const char *name, int offset, int size);
```

## Formatted Output

The `strada_printf` function supports these format specifiers:

| Specifier | Description |
|-----------|-------------|
| `%s` | String |
| `%d` | Integer |
| `%f` | Float |
| `%x` | Hexadecimal |
| `%%` | Literal % |

## Error Handling

Runtime errors call `strada_die()` which:
1. Prints error message to stderr
2. Optionally prints stack trace
3. Exits with code 1

Example:
```c
if (index < 0 || index >= arr->length) {
    strada_die("Array index out of bounds");
}
```

## Thread Safety

The current runtime is **not thread-safe**. Each thread should have its own Strada interpreter if threading is required.

## Memory Model

- All `StradaValue` objects are heap-allocated
- Reference counting manages memory automatically
- Circular references can cause memory leaks
- Use `strada_free()` to break cycles if needed

## Time Functions

### Basic Time

```c
// Get current Unix timestamp
StradaValue* strada_time(void);

// Sleep for seconds (returns 0 on success)
StradaValue* strada_sleep(StradaValue *seconds);

// Sleep for microseconds
StradaValue* strada_usleep(StradaValue *microseconds);
```

### Time Conversion

```c
// Convert timestamp to local time hash
// Returns: {sec, min, hour, mday, mon, year, wday, yday, isdst}
StradaValue* strada_localtime(StradaValue *timestamp);

// Convert timestamp to UTC time hash
StradaValue* strada_gmtime(StradaValue *timestamp);

// Convert time hash to timestamp
StradaValue* strada_mktime(StradaValue *time_hash);

// Format time as string (strftime format)
StradaValue* strada_strftime(StradaValue *format, StradaValue *time_hash);

// Convert timestamp to human-readable string
StradaValue* strada_ctime(StradaValue *timestamp);
```

### High-Resolution Time

```c
// Get time with microsecond precision
// Returns: {sec, usec}
StradaValue* strada_gettimeofday(void);

// Get high-resolution time as float (seconds.microseconds)
StradaValue* strada_hires_time(void);

// Calculate interval between two gettimeofday results
StradaValue* strada_tv_interval(StradaValue *start, StradaValue *end);

// Sleep for nanoseconds
StradaValue* strada_nanosleep(StradaValue *nanoseconds);

// Get clock time (CLOCK_REALTIME, CLOCK_MONOTONIC, etc.)
// Returns: {sec, nsec}
StradaValue* strada_clock_gettime(StradaValue *clock_id);

// Get clock resolution
StradaValue* strada_clock_getres(StradaValue *clock_id);
```

## Dynamic Loading (FFI)

### Library Loading

```c
// Open shared library
// Returns handle (as int) or 0 on failure
StradaValue* strada_dl_open(StradaValue *library_path);

// Get symbol from library
// Returns function pointer (as int) or 0 on failure
StradaValue* strada_dl_sym(StradaValue *handle, StradaValue *symbol_name);

// Close shared library
StradaValue* strada_dl_close(StradaValue *handle);

// Get last error message
StradaValue* strada_dl_error(void);
```

### Calling Foreign Functions (Basic)

These functions convert all arguments to their primitive types. Use for C functions that take only numbers.

```c
// Call function returning int64_t
// args: array of int arguments (up to 5)
// WARNING: Strings are converted to 0 via strada_to_int()
StradaValue* strada_dl_call_int(StradaValue *func_ptr, StradaValue *args);

// Call function returning double
// args: array of double arguments (up to 2)
StradaValue* strada_dl_call_num(StradaValue *func_ptr, StradaValue *args);

// Call function returning char* (takes 1 string arg)
StradaValue* strada_dl_call_str(StradaValue *func_ptr, StradaValue *arg);

// Call function with no return value
StradaValue* strada_dl_call_void(StradaValue *func_ptr, StradaValue *args);
```

### Calling Foreign Functions (StradaValue* passthrough)

These `_sv` variants pass `StradaValue*` pointers directly to C functions. The C function must extract values using `strada_to_str()`, `strada_to_int()`, etc. Use for C functions that need string arguments or mixed types.

```c
// Call function passing StradaValue* directly, returns int64_t
// C function signature: int64_t func(StradaValue*, StradaValue*, ...)
// Up to 5 arguments
StradaValue* strada_dl_call_int_sv(StradaValue *func_ptr, StradaValue *args);

// Call function passing StradaValue* directly, returns char*
// C function signature: char* func(StradaValue*, StradaValue*, ...)
// Up to 3 arguments
StradaValue* strada_dl_call_str_sv(StradaValue *func_ptr, StradaValue *args);

// Call function passing StradaValue* directly, no return value
// C function signature: void func(StradaValue*, StradaValue*, ...)
// Up to 3 arguments
StradaValue* strada_dl_call_void_sv(StradaValue *func_ptr, StradaValue *args);
```

### Writing C Libraries for _sv Functions

C libraries using `_sv` functions must include the Strada runtime header:

```c
#include "strada_runtime.h"

// Example: Function taking host (string) and port (int)
int64_t my_connect(StradaValue *host_sv, StradaValue *port_sv) {
    // Extract actual values from StradaValue
    const char *host = strada_to_str(host_sv);
    int port = (int)strada_to_int(port_sv);

    // Do work...
    return connection_handle;
}

// Example: Function returning a string
char* my_read(StradaValue *handle_sv, StradaValue *max_len_sv) {
    int handle = (int)strada_to_int(handle_sv);
    int max_len = (int)strada_to_int(max_len_sv);

    char *buffer = malloc(max_len + 1);
    // Read into buffer...
    return buffer;  // Strada takes ownership
}
```

**Build command:**
```bash
gcc -shared -fPIC -o libmylib.so mylib.c -I/path/to/strada/runtime -lssl -lcrypto
```

**Important:** Link Strada programs with `-rdynamic` so the shared library can call runtime functions:
```bash
gcc -rdynamic -o myprog myprog.c runtime/strada_runtime.c -Iruntime -ldl -lm
```

The `./strada` wrapper script does this automatically.

### Pointer Access for FFI

```c
// Get pointer to an int variable's underlying value
StradaValue* strada_int_ptr(StradaValue *ref);

// Get pointer to a num variable's underlying value
StradaValue* strada_num_ptr(StradaValue *ref);

// Get pointer to a string's char data
StradaValue* strada_str_ptr(StradaValue *ref);

// Read int value from pointer
StradaValue* strada_ptr_deref_int(StradaValue *ptr);

// Read num value from pointer
StradaValue* strada_ptr_deref_num(StradaValue *ptr);

// Read string from pointer
StradaValue* strada_ptr_deref_str(StradaValue *ptr);

// Write int value to pointer
StradaValue* strada_ptr_set_int(StradaValue *ptr, StradaValue *val);

// Write num value to pointer
StradaValue* strada_ptr_set_num(StradaValue *ptr, StradaValue *val);
```

### FFI Usage Example

```c
// Load libm and call sqrt
StradaValue *libm = strada_dl_open(strada_new_str("libm.so.6"));
StradaValue *sqrt_fn = strada_dl_sym(libm, strada_new_str("sqrt"));
StradaValue *args = strada_anon_array(1, strada_new_num(16.0));
StradaValue *result = strada_dl_call_num(sqrt_fn, args);
// result is 4.0
strada_dl_close(libm);
```

## Calling Strada from C

When embedding Strada code in a C program:

### Required Setup

```c
#include "strada_runtime.h"

// Define required globals
StradaValue *ARGV = NULL;
StradaValue *ARGC = NULL;

// Declare Strada functions
StradaValue* my_strada_func(StradaValue* arg);

int main(int argc, char **argv) {
    // Initialize globals
    ARGV = strada_new_array();
    for (int i = 0; i < argc; i++) {
        strada_array_push(ARGV->value.av, strada_new_str(argv[i]));
    }
    ARGC = strada_new_int(argc);

    // Call Strada functions
    StradaValue *result = my_strada_func(strada_new_int(42));
    printf("Result: %ld\n", strada_to_int(result));

    return 0;
}
```

### Build Process

```bash
# 1. Compile Strada library to C (no main function)
./stradac mylib.strada mylib.c

# 2. Compile and link
gcc -o myprogram main.c mylib.c runtime/strada_runtime.c \
    -Iruntime -ldl -lm
```

### Type Conversion Helpers

```c
#define STRADA_INT(x)    strada_new_int(x)
#define STRADA_NUM(x)    strada_new_num(x)
#define STRADA_STR(x)    strada_new_str(x)
#define TO_INT(sv)       strada_to_int(sv)
#define TO_NUM(sv)       strada_to_num(sv)
#define TO_STR(sv)       strada_to_str(sv)
```

## Performance Considerations

- String operations allocate new memory
- Large arrays/hashes pre-allocate capacity
- Reference counting has overhead vs garbage collection
- Consider `extern` functions for performance-critical code

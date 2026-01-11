# Async/Await and Concurrency in Strada

This document covers Strada's complete concurrency system: async/await for parallel execution, channels for thread communication, mutexes for critical sections, and atomics for lock-free operations.

## Table of Contents

1. [Overview](#overview)
2. [Async/Await](#asyncawait)
3. [Futures](#futures)
4. [Channels](#channels)
5. [Mutexes](#mutexes)
6. [Atomics](#atomics)
7. [Patterns and Best Practices](#patterns-and-best-practices)
8. [API Reference](#api-reference)

---

## Overview

Strada provides a comprehensive concurrency model built on a thread pool:

| Feature | Purpose | Use Case |
|---------|---------|----------|
| `async/await` | Parallel task execution | CPU-bound work, I/O operations |
| Channels | Thread-safe message passing | Producer/consumer, pipelines |
| Mutexes | Critical section protection | Shared mutable state |
| Atomics | Lock-free integer operations | Counters, flags, CAS loops |

The thread pool is automatically initialized on first use with 4 worker threads. You can customize this with `async::pool_init()`.

---

## Async/Await

### Defining Async Functions

Use the `async` keyword before `func` (or `fn`) to create an async function:

```strada
async func fetch_data(str $url) str {
    # This runs in a thread pool worker
    my str $response = http_get($url);
    return $response;
}
```

When called, an async function:
1. Immediately returns a **Future**
2. Executes its body in a thread pool worker
3. Stores the result (or exception) in the Future

### Awaiting Results

Use `await` to block until a Future completes:

```strada
func main() int {
    my scalar $future = fetch_data("http://example.com");
    say("Request started...");

    my str $result = await $future;  # Blocks here
    say("Got: " . $result);

    return 0;
}
```

### Parallel Execution

Launch multiple async operations and await them:

```strada
async func compute(int $n) int {
    # Expensive computation
    return $n * $n;
}

func main() int {
    # Start 3 parallel computations
    my scalar $a = compute(10);
    my scalar $b = compute(20);
    my scalar $c = compute(30);

    # All three are now running in parallel
    # Await results (in any order)
    my int $r1 = await $a;
    my int $r2 = await $b;
    my int $r3 = await $c;

    say("Results: " . $r1 . ", " . $r2 . ", " . $r3);
    return 0;
}
```

### Error Handling

Exceptions in async functions propagate through `await`:

```strada
async func may_fail() int {
    throw "Something went wrong!";
}

func main() int {
    my scalar $f = may_fail();

    try {
        my int $result = await $f;
    } catch ($e) {
        say("Caught: " . $e);
    }

    return 0;
}
```

---

## Futures

A Future represents an async operation that will complete in the future.

### Future Combinators

#### `async::all()` - Wait for All

Wait for multiple futures and get all results as an array:

```strada
my scalar $f1 = compute(1);
my scalar $f2 = compute(2);
my scalar $f3 = compute(3);

my array @futures = ($f1, $f2, $f3);
my array @results = await async::all(\@futures);

# @results contains [1, 4, 9] (or whatever compute returns)
```

#### `async::race()` - First to Complete

Wait for the first future to complete, cancel the others:

```strada
async func slow_server() str {
    core::usleep(100000);  # 100ms
    return "slow";
}

async func fast_server() str {
    core::usleep(10000);   # 10ms
    return "fast";
}

func main() int {
    my array @futures = (slow_server(), fast_server());
    my str $winner = await async::race(\@futures);
    say("Winner: " . $winner);  # "fast"
    return 0;
}
```

#### `async::timeout()` - Await with Timeout

Wait for a future with a maximum time limit:

```strada
my scalar $f = slow_operation();

try {
    my str $result = await async::timeout($f, 1000);  # 1 second timeout
    say("Got: " . $result);
} catch ($e) {
    say("Timed out: " . $e);
}
```

### Future Status

Check future state without blocking:

```strada
my scalar $f = compute(42);

# Poll until done
while (async::is_done($f) == 0) {
    say("Still working...");
    core::usleep(10000);
}

my int $result = await $f;
```

### Cancellation

Request cancellation of a future:

```strada
my scalar $f = long_running_task();

# Changed our mind
async::cancel($f);

if (async::is_cancelled($f)) {
    say("Task was cancelled");
}

# Awaiting a cancelled future throws
try {
    await $f;
} catch ($e) {
    say("Caught: " . $e);  # "Future was cancelled"
}
```

### Thread Pool Configuration

```strada
# Initialize with custom worker count (call early in main)
async::pool_init(8);  # 8 worker threads

# ... your async code ...

# Optional: clean shutdown
async::pool_shutdown();
```

---

## Channels

Channels provide thread-safe message passing between async tasks.

### Creating Channels

```strada
# Unbounded channel (grows as needed)
my scalar $ch = async::channel();

# Bounded channel (blocks when full)
my scalar $ch = async::channel(10);  # Max 10 items
```

### Sending and Receiving

```strada
# Send a value (blocks if channel is full)
async::send($ch, "hello");
async::send($ch, 42);
async::send($ch, \@some_array);

# Receive a value (blocks if channel is empty)
my str $msg = async::recv($ch);
```

### Non-Blocking Operations

```strada
# Try to send without blocking
if (async::try_send($ch, $value)) {
    say("Sent successfully");
} else {
    say("Channel is full");
}

# Try to receive without blocking
my scalar $val = async::try_recv($ch);
if (defined($val)) {
    say("Got: " . $val);
} else {
    say("Channel is empty");
}
```

### Closing Channels

```strada
# Close the channel
async::close($ch);

# Check if closed
if (async::is_closed($ch)) {
    say("Channel is closed");
}

# Receiving from closed empty channel returns undef
my scalar $val = async::recv($ch);
if (!defined($val)) {
    say("Channel closed and empty");
}

# Sending to closed channel throws
try {
    async::send($ch, "test");
} catch ($e) {
    say("Error: " . $e);  # "channel::send: channel is closed"
}
```

### Channel Length

```strada
my int $count = async::len($ch);
say("Items in channel: " . $count);
```

### Producer/Consumer Pattern

```strada
async func producer(scalar $ch, int $count) int {
    for (my int $i = 1; $i <= $count; $i++) {
        async::send($ch, $i);
    }
    async::close($ch);
    return $count;
}

async func consumer(scalar $ch) int {
    my int $sum = 0;
    while (1) {
        my scalar $val = async::recv($ch);
        if (!defined($val)) {
            last;  # Channel closed
        }
        $sum = $sum + $val;
    }
    return $sum;
}

func main() int {
    my scalar $ch = async::channel(5);  # Buffer of 5

    my scalar $prod = producer($ch, 100);
    my scalar $cons = consumer($ch);

    await $prod;
    my int $total = await $cons;
    say("Sum: " . $total);  # 5050

    return 0;
}
```

### Fan-Out Pattern

Multiple producers sending to one channel:

```strada
async func worker(scalar $ch, int $id) int {
    for (my int $i = 0; $i < 10; $i++) {
        async::send($ch, "worker " . $id . " item " . $i);
    }
    return $id;
}

func main() int {
    my scalar $ch = async::channel();

    # Start 3 workers
    my scalar $w1 = worker($ch, 1);
    my scalar $w2 = worker($ch, 2);
    my scalar $w3 = worker($ch, 3);

    # Receive all messages
    for (my int $i = 0; $i < 30; $i++) {
        my str $msg = async::recv($ch);
        say($msg);
    }

    await $w1;
    await $w2;
    await $w3;

    return 0;
}
```

---

## Mutexes

Mutexes protect shared mutable state from concurrent access.

### Basic Usage

```strada
my scalar $mutex = async::mutex();
my int $shared_counter = 0;

async func increment(int $times) int {
    for (my int $i = 0; $i < $times; $i++) {
        async::lock($mutex);
        $shared_counter = $shared_counter + 1;
        async::unlock($mutex);
    }
    return $times;
}

func main() int {
    my scalar $f1 = increment(100);
    my scalar $f2 = increment(100);
    my scalar $f3 = increment(100);

    await $f1;
    await $f2;
    await $f3;

    say("Counter: " . $shared_counter);  # Always 300

    async::mutex_destroy($mutex);
    return 0;
}
```

### Try Lock (Non-Blocking)

```strada
my scalar $m = async::mutex();

if (async::try_lock($m) == 0) {
    # Got the lock
    # ... critical section ...
    async::unlock($m);
} else {
    say("Lock is held by another thread");
}
```

### Important Notes

- Always unlock in the same scope you locked
- Use try/catch to ensure unlock on exceptions:

```strada
async::lock($mutex);
try {
    # ... code that might throw ...
} catch ($e) {
    async::unlock($mutex);
    throw $e;
}
async::unlock($mutex);
```

---

## Atomics

Atomics provide lock-free operations on integers, useful for counters and flags.

### Creating Atomics

```strada
my scalar $counter = async::atomic(0);    # Initial value 0
my scalar $flag = async::atomic(1);       # Initial value 1
```

### Load and Store

```strada
my int $val = async::atomic_load($counter);
async::atomic_store($counter, 100);
```

### Arithmetic Operations

```strada
# Add and get OLD value
my int $old = async::atomic_add($counter, 10);

# Subtract and get OLD value
my int $old = async::atomic_sub($counter, 5);

# Increment and get NEW value
my int $new = async::atomic_inc($counter);

# Decrement and get NEW value
my int $new = async::atomic_dec($counter);
```

### Compare-and-Swap (CAS)

CAS atomically: if value == expected, set to desired.

```strada
my scalar $val = async::atomic(10);

# Try to change 10 -> 20
if (async::atomic_cas($val, 10, 20)) {
    say("Changed 10 to 20");
} else {
    say("Value was not 10");
}
```

### Lock-Free Counter Example

```strada
my scalar $counter = async::atomic(0);

async func increment_atomic(int $times) int {
    for (my int $i = 0; $i < $times; $i++) {
        async::atomic_inc($counter);
    }
    return $times;
}

func main() int {
    my scalar $f1 = increment_atomic(1000);
    my scalar $f2 = increment_atomic(1000);
    my scalar $f3 = increment_atomic(1000);

    await $f1;
    await $f2;
    await $f3;

    say("Counter: " . async::atomic_load($counter));  # Always 3000
    return 0;
}
```

### CAS Loop Pattern

Implement custom atomic operations:

```strada
# Atomic maximum: set to max(current, new_val)
func atomic_max(scalar $atom, int $new_val) void {
    while (1) {
        my int $current = async::atomic_load($atom);
        if ($new_val <= $current) {
            return;  # Current is already >= new_val
        }
        if (async::atomic_cas($atom, $current, $new_val)) {
            return;  # Successfully updated
        }
        # CAS failed, another thread changed it, retry
    }
}
```

---

## Patterns and Best Practices

### Worker Pool

```strada
async func worker(scalar $jobs, scalar $results) int {
    my int $processed = 0;
    while (1) {
        my scalar $job = async::recv($jobs);
        if (!defined($job)) {
            last;  # No more jobs
        }

        my scalar $result = process_job($job);
        async::send($results, $result);
        $processed = $processed + 1;
    }
    return $processed;
}

func main() int {
    my scalar $jobs = async::channel(100);
    my scalar $results = async::channel(100);

    # Start 4 workers
    my array @workers = ();
    for (my int $i = 0; $i < 4; $i++) {
        push(@workers, worker($jobs, $results));
    }

    # Submit jobs
    for (my int $i = 0; $i < 1000; $i++) {
        async::send($jobs, create_job($i));
    }
    async::close($jobs);  # Signal no more jobs

    # Collect results
    for (my int $i = 0; $i < 1000; $i++) {
        my scalar $result = async::recv($results);
        handle_result($result);
    }

    # Wait for workers to finish
    foreach my scalar $w (@workers) {
        await $w;
    }

    return 0;
}
```

### Progress Reporting

```strada
my scalar $progress = async::atomic(0);
my int $total = 1000;

async func do_work(int $item) int {
    # ... work ...
    async::atomic_inc($progress);
    return $item;
}

func main() int {
    # Start all work
    my array @futures = ();
    for (my int $i = 0; $i < $total; $i++) {
        push(@futures, do_work($i));
    }

    # Monitor progress
    while (async::atomic_load($progress) < $total) {
        my int $done = async::atomic_load($progress);
        my int $pct = ($done * 100) / $total;
        say("Progress: " . $pct . "%");
        core::usleep(100000);  # 100ms
    }

    # Await all
    await async::all(\@futures);
    say("Done!");

    return 0;
}
```

### Choosing the Right Primitive

| Need | Use |
|------|-----|
| Run code in parallel | `async func` + `await` |
| Wait for multiple tasks | `async::all()` |
| First result wins | `async::race()` |
| Send data between threads | `async::channel()` |
| Protect shared data structure | `async::mutex()` |
| Simple counter/flag | `async::atomic()` |
| Complex atomic operation | CAS loop with `async::atomic_cas()` |

---

## API Reference

### Async/Await Keywords

| Syntax | Description |
|--------|-------------|
| `async func name(...) type { }` | Define async function |
| `await $future` | Block until future completes |

### Future Functions

| Function | Description |
|----------|-------------|
| `async::all(\@futures)` | Wait for all, return array of results |
| `async::race(\@futures)` | Wait for first, cancel others |
| `async::timeout($future, $ms)` | Await with timeout (throws on timeout) |
| `async::cancel($future)` | Request cancellation |
| `async::is_done($future)` | Check if complete (non-blocking) |
| `async::is_cancelled($future)` | Check if cancelled |

### Thread Pool Functions

| Function | Description |
|----------|-------------|
| `async::pool_init($n)` | Initialize pool with N workers |
| `async::pool_shutdown()` | Shutdown thread pool |

### Channel Functions

| Function | Description |
|----------|-------------|
| `async::channel()` | Create unbounded channel |
| `async::channel($capacity)` | Create bounded channel |
| `async::send($ch, $value)` | Send value (blocks if full) |
| `async::recv($ch)` | Receive value (blocks if empty) |
| `async::try_send($ch, $value)` | Non-blocking send (returns 0/1) |
| `async::try_recv($ch)` | Non-blocking receive (returns undef if empty) |
| `async::close($ch)` | Close channel |
| `async::is_closed($ch)` | Check if closed |
| `async::len($ch)` | Get number of items in channel |

### Mutex Functions

| Function | Description |
|----------|-------------|
| `async::mutex()` | Create new mutex |
| `async::lock($m)` | Acquire lock (blocking) |
| `async::unlock($m)` | Release lock |
| `async::try_lock($m)` | Non-blocking lock (returns 0=success) |
| `async::mutex_destroy($m)` | Destroy mutex |

### Atomic Functions

| Function | Description |
|----------|-------------|
| `async::atomic($initial)` | Create atomic integer |
| `async::atomic_load($a)` | Read value |
| `async::atomic_store($a, $val)` | Write value |
| `async::atomic_add($a, $delta)` | Add, return OLD value |
| `async::atomic_sub($a, $delta)` | Subtract, return OLD value |
| `async::atomic_inc($a)` | Increment, return NEW value |
| `async::atomic_dec($a)` | Decrement, return NEW value |
| `async::atomic_cas($a, $expected, $desired)` | Compare-and-swap (returns 1 if swapped) |

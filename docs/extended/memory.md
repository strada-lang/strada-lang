# Memory Management in Strada

Strada uses **automatic reference counting** for memory management. You rarely
need to think about memory - it's handled automatically.

## How Reference Counting Works

Every value in Strada has a reference count tracking how many variables
point to it. When the count reaches zero, memory is freed immediately.

```strada
func example() void {
    my str $text = "Hello";   # refcount = 1
    my str $copy = $text;     # refcount = 2 (shared)
    $copy = "World";          # $text refcount = 1, new string for $copy
}
# When function ends, both refcounts go to 0, memory freed
```

## What Happens Automatically

1. **Variable assignment** - increments refcount
2. **Scope exit** - decrements refcount
3. **Reassignment** - decrements old value, increments new
4. **Zero refcount** - memory freed immediately

No garbage collector, no pauses, no manual free() calls.

## Inspecting Reference Counts

Use `core::refcount()` to check a value's reference count:

```strada
my array @data = (1, 2, 3);
say(core::refcount(\@data));   # 1

my array @alias = @data;
say(core::refcount(\@data));   # 2
```

## Arrays and Memory

### Pre-allocation

When you know the size, pre-allocate to avoid reallocations:

```strada
my array @big[1000];          # Capacity for 1000 elements
for (my int $i = 0; $i < 1000; $i++) {
    push(@big, $i);           # No reallocation needed
}
```

### Capacity Management

```strada
my array @data = (1, 2, 3, 4, 5);

# Check allocated capacity
my int $cap = core::array_capacity(@data);

# Ensure capacity for at least N elements
core::array_reserve(@data, 100);

# Shrink capacity to match length (release excess memory)
core::array_shrink(@data);
```

## Hashes and Memory

### Pre-allocation

```strada
my hash %cache[500];          # Capacity for ~500 keys
```

### Default Capacity

Set default capacity for all new hashes:

```strada
core::hash_default_capacity(1000);
# All hashes created after this have capacity for 1000 keys
```

## OOP Destructors

Classes can define `DESTROY` methods called when refcount reaches zero:

```strada
package FileHandle;

func new(str $path) scalar {
    my hash %self = {
        "path" => $path,
        "fh" => core::open($path, "r")
    };
    return bless(\%self, "FileHandle");
}

func DESTROY(scalar $self) void {
    if (length($self->{"fh"}) > 0) {
        core::close($self->{"fh"});
    }
    say("Closed: " . $self->{"path"});
}
```

Usage:

```strada
func process_file() void {
    my scalar $f = FileHandle::new("data.txt");
    # ... use $f ...
}
# DESTROY called automatically when $f goes out of scope
```

## Circular References

The one caveat: circular references prevent cleanup.

```strada
# WARNING: This leaks memory
my hash %a = {};
my hash %b = {};
$a{"other"} = \%b;
$b{"other"} = \%a;   # Circular! Neither can reach refcount 0
```

### Avoiding Circular References

The best approach is to use weak references. A weak reference does not keep its target alive — when the target's last strong reference is dropped, the target is freed and all weak references to it become `undef`.

```strada
# Break circular references with core::weaken()
my scalar $parent = { "name" => "parent" };
my scalar $child = { "name" => "child" };
$parent->{"child"} = $child;
$child->{"parent"} = $parent;

core::weaken($child->{"parent"});         # Make the back-reference weak
say(core::isweak($child->{"parent"}));    # 1
say($child->{"parent"}->{"name"});        # "parent" (still accessible)
# When $parent goes out of scope, it can be freed normally
```

| Function | Description |
|----------|-------------|
| `core::weaken($ref)` | Make `$ref` a weak reference |
| `core::isweak($ref)` | Returns 1 if `$ref` is weak, 0 otherwise |

- Works on hash entry values: `core::weaken($hash->{"key"})`
- Idempotent: calling `core::weaken()` on an already-weak ref is a safe no-op
- Multiple weak references to the same target are supported
- When the target is freed, dereferencing the weak ref returns `undef`

**Alternative approaches:**

1. **Manual cleanup** — Set one reference to `undef` before scope exit: `$a{"other"} = undef;`
2. **ID-based references** — Store an ID string instead of a direct reference

## Memory in Closures

Closures capture variables by reference:

```strada
func make_counter() scalar {
    my int $count = 0;
    return func() int {
        $count = $count + 1;
        return $count;
    };
}

my scalar $counter = make_counter();
say($counter->());  # 1
say($counter->());  # 2
# $count stays alive as long as $counter exists
```

## Thread Safety

Reference counting is thread-safe using atomic operations:

```strada
# Safe to share values between threads
my array @shared = (1, 2, 3);
my int $pid = core::fork();
# Both parent and child can access @shared safely
```

## Performance Tips

### Do

- Pre-allocate arrays/hashes when size is known
- Use `core::array_shrink()` after removing many elements
- Let values go out of scope naturally
- Use local variables in loops (they're freed each iteration)

### Don't

- Create unnecessary intermediate copies
- Build large strings with repeated concatenation (use arrays + join)
- Create circular references
- Hold references longer than needed

### String Building

Bad (O(n^2) - creates new string each iteration):

```strada
my str $result = "";
for (my int $i = 0; $i < 1000; $i++) {
    $result = $result . "line " . $i . "\n";
}
```

Good (O(n) - single join at end):

```strada
my array @lines;
for (my int $i = 0; $i < 1000; $i++) {
    push(@lines, "line " . $i);
}
my str $result = join("\n", @lines);
```

## Comparison with Other Languages

| Language | Memory Model | Pros | Cons |
|----------|--------------|------|------|
| Strada | Reference counting | Predictable, immediate cleanup | Circular refs leak |
| Perl | Reference counting | Same as Strada | Same as Strada |
| Python | Refcount + GC | Handles cycles | GC pauses |
| Go | Garbage collection | Handles cycles | Unpredictable pauses |
| Rust | Ownership | Zero overhead | Complex syntax |
| C | Manual | Full control | Error prone |

## Tied Variables

The `tie`/`untie`/`tied` mechanism allows custom variable implementations with magic methods. Tied variables are memory-safe: 0 bytes definitely lost, verified with valgrind.

- **tie**: Binds a variable to a class that intercepts reads/writes
- **untie**: Removes the tie binding, restoring normal variable behavior
- **tied**: Returns the underlying tied object (or undef if not tied)

Once a variable is untied, it has **zero overhead** -- it behaves exactly like a regular variable with no dispatch checks or extra indirection.

```strada
package UpperCase;

func TIESCALAR(str $class) scalar {
    my hash %self = { "value" => "" };
    return bless(\%self, "UpperCase");
}

func STORE(scalar $self, scalar $val) void {
    $self->{"value"} = uc($val);
}

func FETCH(scalar $self) scalar {
    return $self->{"value"};
}

package main;

func main() int {
    my str $name = "";
    tie($name, "UpperCase");
    $name = "hello";
    say($name);              # "HELLO"
    say(tied($name)->isa("UpperCase"));  # 1
    untie($name);            # Zero overhead from here on
    return 0;
}
```

## Debugging Memory Issues

### Check for leaks

Monitor refcounts during development:

```strada
my scalar $obj = create_object();
say("Refcount: " . core::refcount($obj));
```

### Common leak patterns

1. **Circular references** - A points to B, B points to A
2. **Global accumulation** - Pushing to global arrays without cleanup
3. **Forgotten closures** - Closures holding references to large data

## Summary

- Memory is managed automatically via reference counting
- Values are freed immediately when refcount reaches zero
- Pre-allocate arrays/hashes when size is known
- Avoid circular references (they leak)
- Use DESTROY for cleanup in OOP classes
- Reference counting is thread-safe

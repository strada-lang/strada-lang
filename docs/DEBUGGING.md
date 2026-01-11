# Debugging Strada Programs with GDB

This guide explains how to use GDB to debug Strada programs.

## Quick Start

```bash
# Compile with debug symbols
./strada -g -O0 program.strada

# Start GDB
gdb ./program

# Basic commands
(gdb) break main
(gdb) run
(gdb) next
(gdb) print var_x
```

## Compilation Options

### Strada-Level Debugging (Recommended)

```bash
./strada -g program.strada
```

This emits `#line` directives so GDB shows your `.strada` source code. When you step through code or hit breakpoints, you'll see Strada lines, not generated C.

### C-Level Debugging

```bash
./strada -c --c-debug program.strada
```

This keeps the generated `.c` file and includes debug symbols, but GDB shows the C code instead of Strada source. Useful for debugging code generation issues.

### Disable Optimizations

For best debugging experience, disable optimizations:

```bash
./strada -g -O0 program.strada
```

With `-O2` (default), variables may be optimized away or reordered.

## Variable Name Mapping

Strada variables are renamed in the generated C code:

| Strada | C Variable |
|--------|------------|
| `$x` | `var_x` |
| `@arr` | `var_arr` |
| `%hash` | `var_hash` |
| `$my_var` | `var_my_var` |

### Examples

```bash
# Strada: my int $count = 10;
(gdb) print var_count

# Strada: my str $name = "hello";
(gdb) print var_name

# Strada: my array @items = [1, 2, 3];
(gdb) print var_items
```

## Inspecting StradaValue Structures

All Strada values are `StradaValue*` pointers. To inspect them:

### Basic Inspection

```bash
(gdb) print *var_x
# Shows: {type = 1, ref_count = 1, data = {int_val = 42, ...}}
```

### Type Values

| Type ID | Strada Type |
|---------|-------------|
| 0 | UNDEF |
| 1 | INT |
| 2 | NUM (float) |
| 3 | STR |
| 4 | ARRAY |
| 5 | HASH |
| 6 | REF |
| 7 | CPOINTER |

### Inspecting Strings

```bash
(gdb) print var_name->data.str_val
# Shows: 0x55555555a2c0 "hello"
```

### Inspecting Integers

```bash
(gdb) print var_count->data.int_val
# Shows: 42
```

### Inspecting Floats

```bash
(gdb) print var_price->data.num_val
# Shows: 19.989999999999998
```

### Inspecting Arrays

```bash
# Array length
(gdb) print var_items->data.array_val.size
# Shows: 3

# First element
(gdb) print *var_items->data.array_val.items[0]

# First element's integer value
(gdb) print var_items->data.array_val.items[0]->data.int_val
```

### Inspecting Hashes

```bash
# Hash size
(gdb) print var_hash->data.hash_val.size

# Hash internals (more complex, uses buckets)
(gdb) print var_hash->data.hash_val
```

## Setting Breakpoints

### By Function Name

```bash
# Break at main
(gdb) break main

# Break at a Strada function (use generated C name)
(gdb) break func_my_function

# Break at a class method
(gdb) break func_MyClass_method
```

### By Line Number

With `-g` flag, you can break at Strada source lines:

```bash
(gdb) break program.strada:42
```

### Conditional Breakpoints

```bash
# Break when $i equals 5
(gdb) break program.strada:10 if var_i->data.int_val == 5
```

## Stepping Through Code

```bash
(gdb) next          # Step over (next Strada line)
(gdb) step          # Step into function calls
(gdb) finish        # Run until current function returns
(gdb) continue      # Continue to next breakpoint
```

## Examining the Call Stack

```bash
(gdb) backtrace     # Show call stack
(gdb) frame 2       # Switch to frame 2
(gdb) info locals   # Show local variables in current frame
```

## Watchpoints

Watch for variable changes:

```bash
# Watch when integer value changes
(gdb) watch var_x->data.int_val

# Watch when variable pointer changes
(gdb) watch var_x
```

## Useful GDB Commands

```bash
# Show source around current line
(gdb) list

# Show source of specific function
(gdb) list func_my_function

# Print expression
(gdb) print var_x->data.int_val + 10

# Print in hex
(gdb) print/x var_x

# Examine memory
(gdb) x/10xw var_arr->data.array_val.items

# Show all breakpoints
(gdb) info breakpoints

# Delete breakpoint
(gdb) delete 1

# Disable/enable breakpoint
(gdb) disable 1
(gdb) enable 1
```

## Debugging Segfaults

When a program crashes:

```bash
$ gdb ./program
(gdb) run
# ... program crashes ...
(gdb) backtrace          # See where it crashed
(gdb) frame 0            # Go to crash location
(gdb) list               # See source code
(gdb) info locals        # Check local variables
```

### Common Causes

1. **Null pointer dereference**: Check if `var_x` is NULL before accessing `var_x->data`
2. **Array out of bounds**: Check array size vs index
3. **Use after free**: Variable was freed but still accessed

## Debugging with Core Dumps

Enable core dumps:

```bash
ulimit -c unlimited
./program              # Crashes, creates core file
gdb ./program core     # Analyze the crash
(gdb) backtrace
```

## GDB Init File

Create `~/.gdbinit` for convenience:

```
# Pretty print StradaValue
define pval
  if $arg0 == 0
    printf "NULL\n"
  else
    if $arg0->type == 0
      printf "UNDEF\n"
    end
    if $arg0->type == 1
      printf "INT: %d\n", $arg0->data.int_val
    end
    if $arg0->type == 2
      printf "NUM: %f\n", $arg0->data.num_val
    end
    if $arg0->type == 3
      printf "STR: %s\n", $arg0->data.str_val
    end
    if $arg0->type == 4
      printf "ARRAY[%d]\n", $arg0->data.array_val.size
    end
    if $arg0->type == 5
      printf "HASH[%d]\n", $arg0->data.hash_val.size
    end
    if $arg0->type == 6
      printf "REF -> %p\n", $arg0->data.ref_val
    end
  end
end
document pval
  Print a StradaValue in human-readable format
  Usage: pval var_name
end
```

Then use:

```bash
(gdb) pval var_x
INT: 42

(gdb) pval var_name
STR: hello
```

## Example Debugging Session

Given this Strada program (`debug_example.strada`):

```strada
func factorial(int $n) int {
    if ($n <= 1) {
        return 1;
    }
    return $n * factorial($n - 1);
}

func main() int {
    my int $result = factorial(5);
    say("Result: " . $result);
    return 0;
}
```

Debug session:

```bash
$ ./strada -g -O0 debug_example.strada
$ gdb ./debug_example

(gdb) break factorial
Breakpoint 1 at 0x...: file debug_example.strada, line 2.

(gdb) run
Starting program: ./debug_example
Breakpoint 1, func_factorial (var_n=0x...) at debug_example.strada:2

(gdb) print var_n->data.int_val
$1 = 5

(gdb) continue
Breakpoint 1, func_factorial (var_n=0x...) at debug_example.strada:2

(gdb) print var_n->data.int_val
$2 = 4

(gdb) backtrace
#0  func_factorial (var_n=0x...) at debug_example.strada:2
#1  0x... in func_factorial (var_n=0x...) at debug_example.strada:5
#2  0x... in func_main () at debug_example.strada:9

(gdb) continue
...
Result: 120
[Inferior 1 exited normally]
```

## Stack Traces

Strada provides built-in stack trace support for debugging.

### Automatic Stack Traces on Exceptions

When an exception goes uncaught, Strada automatically prints a stack trace:

```
Uncaught exception: division by zero
Stack trace:
  at divide (calculator.strada)
  at compute (calculator.strada)
  at main (calculator.strada)
```

This shows the call chain from innermost function (where the error occurred) to outermost (main).

### Manual Stack Traces

Use `core::stack_trace()` to get the current call stack as a string at any point:

```strada
func debug_location() void {
    my str $trace = core::stack_trace();
    say("Current location:\n" . $trace);
}

func process_data() void {
    debug_location();  # Prints where we are
    # ... rest of processing
}
```

This is useful for:
- Debugging complex call chains
- Logging entry/exit points
- Error reporting in catch blocks

### Captured Stack Traces

You can capture a stack trace for later use:

```strada
func risky_operation() void {
    try {
        do_something_dangerous();
    } catch ($e) {
        my str $trace = core::stack_trace();
        log_error("Error: " . $e . "\nStack:\n" . $trace);
    }
}
```

## Deep Recursion Protection

Strada automatically detects excessive recursion and exits gracefully with a stack trace, instead of crashing with a stack overflow.

### Default Behavior

The default recursion limit is 1000 calls. When exceeded:

```
Error: Maximum recursion depth exceeded (1000)
Stack trace:
  at infinite_recurse (broken.strada)
  at infinite_recurse (broken.strada)
  ...
  at main (broken.strada)
  -> infinite_recurse (broken.strada)

Hint: Use core::set_recursion_limit(n) to increase the limit, or 0 to disable.
```

### Configuring the Limit

```strada
# Increase for deeply recursive algorithms
core::set_recursion_limit(5000);

# Disable the check (not recommended)
core::set_recursion_limit(0);

# Check current limit
my int $limit = core::get_recursion_limit();
```

### When to Adjust

- **Increase limit**: For legitimate deep recursion (tree traversal, parsing, etc.)
- **Disable (0)**: Only when you're certain your recursion is bounded and need maximum performance
- **Lower limit**: To catch runaway recursion earlier during development

## Troubleshooting

### "No symbol table"

Compile with `-g`:
```bash
./strada -g program.strada
```

### Variables optimized out

Compile with `-O0`:
```bash
./strada -g -O0 program.strada
```

### GDB shows C code instead of Strada

Make sure you used `-g` (not `--c-debug`):
```bash
./strada -g program.strada    # Shows Strada source
./strada --c-debug program.strada  # Shows C source
```

### Can't find source file

GDB needs access to the original `.strada` file. Either:
- Debug from the same directory where you compiled
- Use `(gdb) directory /path/to/source`

### Breakpoint not hit

Function names in C are prefixed with `func_`:
```bash
(gdb) break func_my_function   # Correct
(gdb) break my_function        # Won't work
```

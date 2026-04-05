# Strada REPL

The Strada REPL (Read-Eval-Print Loop) provides an interactive shell for experimenting with Strada code without creating files. It is part of `strada-interp`, which defaults to the bytecode VM backend for file execution but uses the tree-walking backend for the REPL (persistent VM state between inputs is not yet supported).

## Starting the REPL

```bash
# Start interactive REPL (uses tree-walking backend)
./strada-interp

# Run a script file (uses VM backend by default)
./strada-interp script.strada

# Run a script with the tree-walking backend
./strada-interp --tree-walk script.strada

# Use via the strada command
./strada --repl
./strada --script script.st
```

## Basic Usage

```
strada> my int $x = 42;
strada> $x * 2
=> 84
strada> my array @nums = (1, 2, 3);
strada> push(@nums, 4);
strada> @nums
=> (1, 2, 3, 4)
```

## Commands

| Command | Description |
|---------|-------------|
| `.help` | Show help message |
| `.vars` | List declared variables with their types and values |
| `.funcs` | List defined functions with signatures |
| `.load FILE` | Load and execute code from a file |
| `.clear` | Clear all state (variables and functions) |
| `.debug` | Toggle debug mode (shows generated code) |
| `.compiler` | Show current compiler backend |
| `.set compiler=X` | Change compiler (libtcc, tcc, gcc) |
| `.memprof` | Toggle memory profiling |
| `.memstats` | Show memory statistics |
| `.funcprof` | Toggle function profiling |
| `.profile` | Show function profile report |
| `.quit` | Exit the REPL |

## Defining Functions

Functions can span multiple lines:

```
strada> func greet(str $name) void {
...        say("Hello, " . $name . "!");
...    }
Function defined
strada> greet("World")
Hello, World!
```

## Multi-line Input

The REPL automatically detects incomplete input (unbalanced braces, parentheses, or brackets) and prompts for continuation:

```
strada> my hash %person = (
...        name => "Alice",
...        age => 30
...    );
strada> $person{"name"}
=> Alice
```

## Execution Backends

`strada-interp` supports two execution backends:

1. **VM** (default for file execution) - Compiles AST to bytecode, executes via dispatch loop. 4-5x faster than Perl 5.38 on compute benchmarks.
2. **Tree-walk** (default for REPL) - Walks the AST directly. Used for the REPL since persistent VM state between inputs is not yet supported.

For file execution, the VM is used by default. Use `--tree-walk` to switch:
```bash
./strada-interp program.strada            # VM (default)
./strada-interp --tree-walk program.strada # tree-walk
```

## Profiling

### Memory Profiling

Track memory allocations:

```
strada> .memprof
Memory profiling enabled
strada> my array @big = ();
strada> for (my int $i = 0; $i < 1000; $i++) { push(@big, $i); }
strada> .memstats
```

### Function Profiling

Profile function execution times:

```
strada> .funcprof
Function profiling enabled (code will be compiled with -p)
strada> func fib(int $n) int {
...        if ($n <= 1) { return $n; }
...        return fib($n - 1) + fib($n - 2);
...    }
Function defined
strada> fib(20)
=> 6765
strada> .profile
```

## Script Mode

Create executable scripts with a shebang:

```strada
#!/path/to/strada-jit

say("Hello from script!");

func double(int $x) int {
    return $x * 2;
}

say("Double of 21 is " . double(21));
```

Make executable and run:
```bash
chmod +x myscript.st
./myscript.st
```

## Error Display

The REPL provides helpful error messages with source context:

```
strada> my int $ = 5;

Error: expected variable name, got ASSIGN

> 1 | my int $ = 5;
```

For multi-line input, the error shows context around the problematic line.

## Tips

- Use `.vars` to see all current variables and their values
- Use `.funcs` to review function signatures
- Use `.clear` to start fresh without restarting the REPL
- Use `.debug` to see the generated Strada code (helpful for understanding issues)
- Variables and functions persist across evaluations until `.clear` or exit

## Limitations

- The REPL uses the tree-walking backend (no persistent VM state between inputs yet)
- Package declarations are ignored (all code is in the default namespace)
- `use` and `import_lib` statements are not yet supported in the REPL
- Some complex language features may not work in the dynamic REPL context
- `__C__` blocks are skipped in the REPL (they are JIT-compiled and cached in the VM backend for file execution)

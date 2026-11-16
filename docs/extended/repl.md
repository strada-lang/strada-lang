# Strada REPL

The Strada REPL (Read-Eval-Print Loop) provides an interactive shell for experimenting with Strada code without creating files. There are two of them:

- **The JIT REPL** — `./strada --repl`, the `tools/strada-jit` program (built by `make tools`). Each input is compiled and run natively. This is the REPL most of this page describes.
- **The interpreter REPL** — `interpreter/strada-interp` (built by `make interpreter`). Uses a persistent bytecode VM session across inputs (`use Eval;` / `Strada::Interpreter`). It understands `.help`, `.vars`, `.funcs`, `.load FILE`, `.clear` and `.quit`.

## Starting the REPL

```bash
# JIT REPL and JIT script runner (via the strada command)
./strada --repl
./strada --script script.st

# Interpreter REPL (bytecode VM)
./interpreter/strada-interp

# Run a script file or inline snippet with the interpreter (bytecode VM)
./interpreter/strada-interp script.strada
./interpreter/strada-interp -e 'say("hello");'
```

## Basic Usage

```
strada> my int $x = 42;
strada> $x * 2
=> 84
strada> my array @nums = (1, 2, 3);
strada> push(@nums, 4);
strada> @nums
=> 4
```

A bare `@array` is evaluated in scalar context, so it shows the element count; use `.vars` to see the contents. (The interpreter REPL prints results without the `=> ` prefix and dumps the array.)

## Commands

| Command | Description |
|---------|-------------|
| `.help` | Show help message |
| `.vars` | List declared variables with their types and values |
| `.funcs` | List defined functions with signatures |
| `.imports` | List imports and library paths |
| `.load FILE` | Load and execute code from a file |
| `.soinfo FILE` | Show exported functions from a .so/.o/.a file |
| `.clear` | Clear all state (variables and functions) |
| `.debug` | Toggle debug mode (shows generated Strada) |
| `.cdebug` | Toggle C debug mode (shows generated C) |
| `.compiler` | Show current compiler backend (choose it at startup: `strada-jit --compiler=tcc` or `--compiler=gcc`) |
| `.memprof` | Toggle memory profiling |
| `.memstats` | Show memory statistics |
| `.quit` | Exit the REPL |

## Defining Functions

Functions can span multiple lines:

```
strada> func greet(str $name) void {
...        say("Hello, " . $name . "!");
...    }
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

## Execution Backend

`strada-interp` executes all modes — files, `-e`, and the interactive REPL — on the bytecode VM (compiles AST to bytecode and executes via a dispatch loop; 4-5x faster than Perl 5.38 on compute benchmarks):
```bash
./interpreter/strada-interp program.strada            # bytecode VM
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

There are no REPL commands for function profiling; compile a file with `./strada -p` (function-level) or `./strada --full-profile` (line-level) instead.

## Script Mode

Create executable scripts with a shebang. By default a script is compiled as a whole file, so it is an ordinary program with a `main` function:

```strada
#!/path/to/strada-jit

func twice(int $x) int {
    return $x * 2;
}

func main() int {
    say("Hello from script!");
    say("Double of 21 is " . twice(21));
    return 0;
}
```

Make executable and run:
```bash
chmod +x myscript.st
./myscript.st
```

(Files not named `*.strada` print an "Input file does not have .strada extension" notice before running.) To run a file of top-level statements the way the REPL would, line by line, use `strada-jit --chunked myscript.st` (slower).

## Error Display

The REPL provides helpful error messages with source context:

```
strada> my int $ = 5;

Error: JIT compile error: /tmp/strada_repl_lib_9.strada:36:14: expected a variable name after '$', got '='
    my int $ = 5;
             ^
```

The file name and line number refer to the temporary source the REPL generates, not to your input; the offending line and caret are shown beneath.

## Tips

- Use `.vars` to see all current variables and their values
- Use `.funcs` to review function signatures
- Use `.clear` to start fresh without restarting the REPL
- Use `.debug` to see the generated Strada code (helpful for understanding issues)
- Variables and functions persist across evaluations until `.clear` or exit

## Limitations

- Package declarations are ignored (all code is in the default namespace)
- `use` and `import_lib` statements work in the JIT REPL (see `.help` / `.imports`); in the interpreter REPL, `.load FILE` is provided for loading files into the session
- Some complex language features may not work in the dynamic REPL context
- `__C__` blocks in standalone files or embedded eval scripts are JIT-compiled and cached (requires `gcc` and runtime headers at runtime)

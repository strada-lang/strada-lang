# Strada Interpreter

The Strada interpreter (`strada-interp`) executes Strada programs without generating C code. It defaults to a bytecode VM backend that compiles the AST to bytecode and executes it via a dispatch loop. A tree-walking backend is available as a fallback via the `--tree-walk` flag.

## Overview

```
                  ┌─────────────────────┐
                  │   Strada Source      │
                  │     (.strada)        │
                  └──────────┬──────────┘
                             │
                    ┌────────┴────────┐
                    │                 │
                    ▼                 ▼
          ┌─────────────────┐  ┌─────────────────┐
          │  Compiler Path  │  │ Interpreter Path │
          │  (stradac)      │  │ (strada-interp)  │
          └────────┬────────┘  └────────┬────────┘
                   │                    │
                   ▼                    ▼
          ┌─────────────────┐  ┌─────────────────┐
          │  C Code → GCC   │  │  AST → Bytecode  │
          │  → Native Binary│  │  → VM Execution   │
          └─────────────────┘  └─────────────────┘
```

Both paths share the same Lexer and Parser front-end. The compiler generates C code for native execution; the interpreter compiles the AST to bytecode and runs it on the VM.

## When to Use the Interpreter

| Use Case | Recommended Mode |
|----------|-----------------|
| Production deployment | Compiled |
| Interactive experimentation | Interpreter (REPL) |
| Quick scripts and prototyping | Interpreter |
| Environments without gcc/tcc | Interpreter |
| Embedded eval in compiled programs | Interpreter (`eval_string`) |
| Maximum performance | Compiled |
| Debugging language behavior | Interpreter |

## Running the Interpreter

### File Execution

```bash
# Build the interpreter
make interpreter

# Run a Strada program (uses VM by default)
./strada-interp program.strada

# Explicitly use the VM backend
./strada-interp --vm program.strada

# Use the tree-walking backend (legacy fallback)
./strada-interp --tree-walk program.strada

# With library search paths
./strada-interp -L lib program.strada
```

### Interactive REPL

```bash
# Start the REPL
./strada-interp

# With library paths
./strada-interp -L lib
```

The REPL provides an interactive prompt with readline support (line editing, history). The REPL currently uses the tree-walking backend (persistent VM state between inputs is not yet supported).

```
strada> my int $x = 42;
strada> $x * 2
=> 84
strada> func square(int $n) int { return $n * $n; }
strada> square(7)
=> 49
```

### Embedded Eval

The interpreter can be used from compiled Strada programs via `Strada::Interpreter`:

```strada
use lib "lib";
use Strada::Interpreter;

Strada::Interpreter::init();

my scalar $result = Strada::Interpreter::eval_string("1 + 2");
say($result);  # 3

# Variables persist across calls
Strada::Interpreter::eval_string("my int $x = 10;");
my scalar $val = Strada::Interpreter::eval_string("$x * 5");
say($val);  # 50

# Function definitions persist
Strada::Interpreter::eval_string("func double(int $n) int { return $n * 2; }");
say(Strada::Interpreter::eval_string("double(21)"));  # 42

# Reset all state
Strada::Interpreter::reset();
```

### Eval Backend Selection

The embedded eval API supports selecting the execution backend:

```strada
# Set the default eval backend
set_eval_backend("vm");          # Use bytecode VM (default)
set_eval_backend("tree-walk");   # Use tree-walking interpreter

# Evaluate with a specific backend for one call
my scalar $result = eval_with("1 + 2", "vm");
my scalar $result2 = eval_with("1 + 2", "tree-walk");
```

## Architecture

### Bytecode VM (Default)

The VM backend compiles the AST to bytecode instructions, then executes them via a computed goto dispatch loop. The compilation pipeline is:

```
Source → Lexer → Parser → AST → Bytecode Compiler → VM Execution
```

Key implementation details:

- **VMValue**: Tagged pointer representation (8 bytes per value). Integers are encoded directly in the pointer with zero heap allocation, similar to the compiled runtime's tagged integers.
- **Computed goto dispatch**: The VM dispatch loop uses GCC's computed goto extension for efficient opcode dispatch, avoiding the overhead of a switch statement.
- **`__C__` block JIT**: `__C__` blocks are compiled to shared libraries by gcc at runtime. Compiled `.so` files are cached in `~/.cache/strada/cblocks/` with 7-day LRU eviction, so repeated execution of the same `__C__` block does not recompile.
- **Variable declarations enforced**: The VM requires all variables to be declared with `my` (no implicit global creation).

### Tree-Walking Backend (Legacy Fallback)

The tree-walking backend evaluates AST nodes directly at runtime without a bytecode compilation step. It is available via `--tree-walk` and is used by the REPL.

### Components

| Component | Source | Purpose |
|-----------|--------|---------|
| Lexer | `compiler/Lexer.strada` | Tokenizes source code (shared with compiler) |
| Parser | `compiler/Parser.strada` | Builds AST (shared with compiler) |
| VM Compiler | `interpreter/vm_compiler.c` | Compiles AST to bytecode |
| VM Runtime | `interpreter/vm.c` | Bytecode dispatch loop |
| Interpreter | `lib/Strada/Interpreter.strada` | Tree-walking AST evaluator |
| Driver | `interpreter/Main.strada` | File execution and REPL |

### Environment and Scoping

Both backends use a chain of environment hashes for lexical scoping:

```
Global Env  ←  Function Env  ←  Block Env  ←  Inner Block Env
```

Each environment has a `vars` hash and a `parent` pointer. Variable lookup walks the chain from innermost to outermost scope. Variable assignment (`my`) creates in the current scope; updates modify the nearest scope that contains the variable.

### Control Flow Signals

Loop control (`next`, `last`, `redo`) and `return` are implemented as exception-based signals. A `return` inside a function throws a signal that is caught by the function call handler. Loop signals are caught by the enclosing loop, with label matching for labeled loops.

## Performance

The bytecode VM is 4-5x faster than Perl 5.38 on compute benchmarks. This is achieved through:

- Tagged pointer VMValue with zero-allocation integer arithmetic
- Computed goto dispatch (avoids switch overhead)
- Direct bytecode execution without AST traversal overhead

The compiled path (`stradac` + gcc) remains faster than the VM for production workloads, but the VM provides a significant performance improvement over the tree-walking interpreter and competitive performance with mainstream interpreted languages.

## Supported Language Features

Both backends support the full Strada language:

- **Types**: int, num, str, scalar, array, hash, undef
- **Variables**: `my`, `our`, `local`, `const`, `enum`
- **Operators**: arithmetic, string, comparison, logical, assignment, ternary, string repeat (`x`)
- **Control flow**: if/elsif/else, unless, while, until, for, foreach, do-while, loop labels (`LABEL: while`), statement modifiers, `next`/`last`/`redo`
- **Functions**: `func`/`fn`, closures, variadic (`...@args`), spread operator
- **OOP**: packages, `extends`, `has ro|rw`, `before`/`after`/`around`, `isa`, `can`, `AUTOLOAD`, `bless`, `use overload`, operator overloading
- **Exceptions**: try/catch (typed and catch-all), throw, die
- **Regex**: `=~`, `!~`, `s///`, `tr///`/`y///`, match, replace, captures, named captures, `/e` modifier
- **I/O**: say, print, printf, sprintf, `core::open`, `core::close`, diamond operator (`<$fh>`), `core::slurp`, `core::spew`
- **Modules**: `use` (source inclusion), `import_lib` (native shared libraries via dlopen)
- **Functional**: `map`, `grep`, `sort` with blocks
- **BEGIN/END blocks**: executed at load time and exit respectively
- **`tie`/`untie`/`tied`**: custom variable implementations
- **BigInt/BigFloat**: arbitrary-precision arithmetic (C-backed helpers)
- **DateTime**: date/time operations
- **Namespaces**: `core::`, `math::`, `utf8::`, `DateTime::`, `BigInt::`, `BigFloat::`
- **Destructuring**: `my ($a, $b) = @array`
- **Slices**: `@data[0, 2, 4]`, `@hash{"key1", "key2"}`

### `__C__` Block Support

The VM backend supports `__C__` blocks via JIT compilation. When the VM encounters a `__C__` block, it compiles the C code to a shared library using gcc, caches the result in `~/.cache/strada/cblocks/`, and loads it via dlopen. Cached `.so` files are reused on subsequent runs with 7-day LRU eviction.

The tree-walking backend skips `__C__` blocks.

## Perla VM Mode

The Strada VM also serves as the execution backend for [Perla](PERL_INTEGRATION.md), the Perl 5 compiler. When invoked with `--vm`, Perla converts Perl source to Strada code and executes it through the VM:

```bash
./perla --vm script.pl    # Perl → Strada → VM execution
```

All 53 Perla tests pass in VM mode, matching the compiled path. This enables running Perl programs without a C compiler at runtime. See the Perla section in `CLAUDE.md` for details.

## Limitations

- **REPL uses tree-walker.** The REPL does not yet support persistent VM state between inputs, so it uses the tree-walking backend.
- **Async/await is not supported.** The thread pool runtime is part of the compiled backend.
- **`c::` namespace functions** (`c::alloc`, `c::free`, etc.) are not available.
- **Performance is slower** than compiled execution, though the VM is 4-5x faster than Perl 5.38.

## Testing

All 33 bytecode VM tests pass:

```bash
# Run interpreter tests
make test-interp

# Run with verbose output
make test-interp V=1
```

## Interpreter vs JIT vs Compiled

Strada offers three execution modes:

| Feature | Interpreter (VM) | Interpreter (tree-walk) | JIT (`Strada::JIT`) | Compiled |
|---------|-------------------|------------------------|---------------------|----------|
| Execution | Bytecode VM | AST tree-walk | Compile to .so at runtime | Compile to native binary |
| Requires C compiler | No (except `__C__` blocks) | No | Yes (gcc or tcc) | Yes (gcc) |
| Startup time | Fast | Fast | Moderate (per-eval compile) | Slow (one-time compile) |
| Runtime speed | Fast (4-5x Perl) | Slowest | Fast (native code) | Fastest |
| `__C__` blocks | JIT-compiled + cached | Skipped | Supported | Supported |
| Async/await | No | No | Yes | Yes |
| Interactive use | REPL (via tree-walk) | REPL | eval() | No |
| Best for | Scripts, file execution | REPL, no-compiler envs | Plugin systems, runtime eval | Production, performance |

## Build

```bash
# Build the interpreter
make interpreter

# Build everything (compiler + interpreter + libs)
make && make interpreter

# The interpreter binary
./strada-interp --help
```

The interpreter is built by combining `compiler/Lexer.strada`, `compiler/Parser.strada`, `compiler/AST.strada`, `lib/Strada/Interpreter.strada`, and `interpreter/Main.strada` into a single source, then compiling that with the Strada compiler.

## API Reference

See the POD documentation in `lib/Strada/Interpreter.strada` for the full API, including:

- `interp_new()` - Create interpreter state
- `interp_load_program()` - Load a parsed AST
- `interp_run_main()` - Execute main()
- `interp_run_end_blocks()` - Run END blocks
- `eval_string()` - Evaluate a code string
- `set_eval_backend()` - Set default eval backend ("vm" or "tree-walk")
- `eval_with()` - Evaluate with a specific backend
- `init()` / `reset()` - Manage shared eval state
- `env_new()` / `env_get()` / `env_set()` - Scope management

See also `lib/Strada/JIT.strada` for the JIT eval API.

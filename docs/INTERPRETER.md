# Strada Interpreter

The Strada interpreter (`strada-interp`) executes Strada programs without generating C code. It compiles the AST to bytecode and executes it via a dispatch loop on the bytecode VM across file execution, `-e`, the interactive REPL, and embedded `use Eval;`.

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
# Build the interpreter (not built by a plain `make`); the binary is
# interpreter/strada-interp
make interpreter

# Run a Strada program
./interpreter/strada-interp program.strada

# Run an inline snippet
./interpreter/strada-interp -e 'say("hello");'

# With library search paths
./interpreter/strada-interp -L lib program.strada
```

### Interactive REPL

```bash
# Start the REPL
./interpreter/strada-interp

# With library paths
./interpreter/strada-interp -L lib
```

The REPL provides an interactive prompt with readline support (line editing, history) backed by a persistent bytecode VM session (functions and `our` / `my` globals persist until `.clear`).

```
strada> my int $x = 42;
strada> $x * 2
84
strada> func square(int $n) int { return $n * $n; }
strada> square(7)
49
```

### Embedded Eval

The interpreter can be used from compiled Strada programs via the `Eval` module (`lib/Eval.strada`, built by `make lib-eval` / `make libs`; it bundles the lexer, parser and `Strada::Interpreter`). `use Strada::Interpreter;` on its own does not compile, because it lacks the front-end:

```strada
use lib "lib";
use Eval;

Strada::Interpreter::init();

my scalar $result = Strada::Interpreter::eval_string("1 + 2");
say($result);  # 3

# Variables persist across calls
Strada::Interpreter::eval_string("my int $x = 10;");
my scalar $val = Strada::Interpreter::eval_string("$x * 5");
say($val);  # 50

# Function definitions persist
Strada::Interpreter::eval_string("func twice(int $n) int { return $n * 2; }");
say(Strada::Interpreter::eval_string("twice(21)"));  # 42

# Reset all state
Strada::Interpreter::reset();
```

### Eval Notes

When a compiled program uses `use Eval;` (`Strada::Interpreter::eval_string()`, `eval_string_vm()`, `eval()`), `strada` automatically detects the VM headers and links `runtime/vm.o` and `runtime/vm_compiler.o` with `-rdynamic`. `set_eval_backend()` and `eval_with()` remain as compatibility wrappers that always execute on the bytecode VM.

## Architecture

### Bytecode VM

The interpreter compiles the AST to bytecode instructions, then executes them via a computed goto dispatch loop. The compilation pipeline is:

```
Source → Lexer → Parser → AST → Bytecode Compiler → VM Execution
```

Key implementation details:

- **VMValue**: Tagged pointer representation (8 bytes per value). Integers are encoded directly in the pointer with zero heap allocation, similar to the compiled runtime's tagged integers.
- **Computed goto dispatch**: The VM dispatch loop uses GCC's computed goto extension for efficient opcode dispatch, avoiding the overhead of a switch statement.
- **`__C__` block JIT**: `__C__` blocks are compiled to shared libraries by gcc at runtime. Compiled `.so` files are cached in `~/.cache/strada/cblocks/` with 7-day LRU eviction, so repeated execution of the same `__C__` block does not recompile.
- **Variable declarations enforced**: The VM requires all variables to be declared with `my` or `our` (no implicit global creation).

### Components

| Component | Source | Purpose |
|-----------|--------|---------|
| Lexer | `compiler/Lexer.strada` | Tokenizes source code (shared with compiler) |
| Parser | `compiler/Parser.strada` | Builds AST (shared with compiler) |
| VM Compiler | `interpreter/vm_compiler.c` | Compiles AST to bytecode |
| VM Runtime | `interpreter/vm.c` | Bytecode dispatch loop |
| Session Manager | `lib/Strada/Interpreter.strada` | VM session wrapper & `eval_string` / `Eval` API |
| Driver | `interpreter/Main.strada` | File execution, `-e`, and REPL |

### Scope and Semantics

Lexical scoping (`my`), closures, dynamic global overrides (`local`), and `our` globals are compiled to VM slots and hash-backed global tables. Control flow (`next`, `last`, `redo`, `return`) and `try`/`catch`/`throw`/`finally` unwind frames and `local` stacks deterministically.

## Performance

The bytecode VM is 4-5x faster than Perl 5.38 on compute benchmarks. This is achieved through:

- Tagged pointer VMValue with zero-allocation integer arithmetic
- Computed goto dispatch (avoids switch overhead)
- Direct bytecode execution without AST traversal overhead

The compiled path (`stradac` + gcc) remains faster than the VM for production workloads, but the VM provides a fast startup, no-codegen execution path and competitive performance with mainstream interpreted languages.

## Supported Language Features

The VM supports the full Strada language:

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
- **Slices**: `@data[0, 2, 4]`, `@hash{"key1", "key2"}`, range slices (`@data[0..5]`)
- **libc surface**: the full `sys::`/`math::` builtin set (~235 functions —
  process, users/groups, file I/O, sockets, math) is available through a
  generic runtime bridge (`vm_generic_builtins.inc`, generated by
  `tools/gen_vm_generic_builtins.py` from the compiler's own emission table)
- **Recursion limits**: `core::set_recursion_limit` / `core::get_recursion_limit`
  enforced on the VM call stack

### `__C__` Block Support

The VM supports `__C__` blocks via JIT compilation. When the VM encounters a `__C__` block, it compiles the C code to a shared library using gcc, caches the result in `~/.cache/strada/cblocks/`, and loads it via dlopen. Cached `.so` files are reused on subsequent runs with 7-day LRU eviction.

## Perla VM Mode

The Strada VM also serves as the execution backend for [Perla](PERL_INTEGRATION.md), the Perl 5 compiler. When invoked with `--vm`, Perla converts Perl source to Strada code and executes it through the VM:

```bash
./perla --vm script.pl    # Perl → Strada → VM execution
```

(Perla lives in a separate repository; there is no `perla/` directory in this tree.)

All 53 Perla tests pass in VM mode, matching the compiled path. This enables running Perl programs without a C compiler at runtime. See the Perla section in `CLAUDE.md` for details.

## Limitations

- **DESTROY runs at GC time**, not when the last reference goes away (the VM uses a tracing GC).
- **Raw pointers into variables** (`core::int_ptr`, `core::num_ptr`) are compiled-only; the VM raises an error.
- **`extern "C"` functions** cannot be called from the VM; the call raises an error. Use a `__C__` block.
- **Performance is slower** than compiled execution. Against Perl 5.38 (2026-11-20, wall clock): 2-22x faster on calls, closures, objects and arithmetic (compute, functions, json, closures, oop, binary_trees); even on strings, utf8, sort and pipeline; slower on regex (4x: PCRE2's interpreter, built without its JIT, against Perl's engine), binary (`pack`/`unpack`, 1.8x), data (1.6x) and sprintf (1.2x).

## Testing

Two suites cover the interpreter:

```bash
# Interpreter's own suite (61 tests, bytecode VM)
cd interpreter/test && bash run_tests.sh

# Compiler test suite executed through the VM (parity check):
# 108 pass / 0 fail / 12 skipped (the skips need .so/.o compilation,
# compile-only checks, SQLite linking, or core::int_ptr)
cd interpreter/test && bash run_compiler_tests.sh
```

## Interpreter vs JIT vs Compiled

Strada offers three execution modes:

| Feature | Interpreter (VM) | JIT (`Strada::JIT`) | Compiled |
|---------|------------------|---------------------|----------|
| Execution | Bytecode VM | Compile to .so at runtime | Compile to native binary |
| Requires C compiler | No (except `__C__` blocks) | Yes (gcc or tcc) | Yes (gcc) |
| Startup time | Fast | Moderate (per-eval compile) | Slow (one-time compile) |
| Runtime speed | Fast (4-5x Perl) | Fast (native code) | Fastest |
| `__C__` blocks | JIT-compiled + cached | Supported | Supported |
| Async/await, threads, green tasks | Yes | Yes | Yes |
| Interactive use | Persistent VM REPL | eval() | No |
| Best for | Scripts, file execution, REPL, embedded `eval` | Plugin systems, runtime eval | Production, performance |

## Build

```bash
# Build the interpreter
make interpreter

# Build everything (compiler + interpreter + libs)
make && make interpreter

# The interpreter binary
./interpreter/strada-interp --help
```

The interpreter is built by combining `compiler/AST.strada`, `compiler/Lexer.strada`, `interpreter/Stubs.strada`, `compiler/Parser.strada`, and `interpreter/Main.strada` into a single source, then compiling that with the Strada compiler and linking `vm.c` + `vm_compiler.c`.

## API Reference

See the POD documentation in `lib/Strada/Interpreter.strada` for the full API, including:

- `eval_string()` / `eval_string_vm()` - Evaluate a code string in the persistent VM session
- `eval()` - Simplified wrapper exported by `use Eval;`
- `init()` / `reset()` - Manage shared VM eval session state
- `set_eval_backend()` / `eval_with()` - Compatibility wrappers delegating to the bytecode VM

See also `lib/Strada/JIT.strada` for the JIT eval API.

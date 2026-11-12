# Strada Documentation

Welcome to the Strada programming language documentation. Strada is a strongly-typed language with Perl-inspired syntax that compiles to native code via C.

## Getting Started

| Document | Description |
|----------|-------------|
| [Installation Guide](INSTALLATION.md) | How to build and install Strada |
| [Tutorial](TUTORIAL.md) | Learn Strada step-by-step |
| [Quick Reference](QUICK_REFERENCE.md) | Syntax cheat sheet |

## Language Reference

| Document | Description |
|----------|-------------|
| [Language Manual](LANGUAGE_MANUAL.md) | Complete language specification |
| [Language Guide](LANGUAGE_GUIDE.md) | In-depth language tutorial |
| [Built-in Functions](BUILTIN_FUNCTIONS.md) | Every built-in function (math::, async::, c::, sys::/core::) — complete alphabetical reference |
| [OOP Guide](OOP_GUIDE.md) | Object-oriented programming |
| [Examples](EXAMPLES.md) | Annotated code examples |

## Advanced Topics

| Document | Description |
|----------|-------------|
| [Debugging Guide](DEBUGGING.md) | Using GDB with Strada programs, line-level profiling |
| [FFI Guide](FFI_GUIDE.md) | C integration and foreign functions |
| [Runtime API](RUNTIME_API.md) | C runtime library reference |
| [Compiler Architecture](COMPILER_ARCHITECTURE.md) | How the compiler works |
| [Understanding the Compiler](UNDERSTANDING_THE_COMPILER.md) | Beginner-friendly, step-by-step tour of the compiler |
| [Interpreter](INTERPRETER.md) | `strada-interp`: bytecode VM and tree-walking backends |
| [Event Loop](EVENT_LOOP.md) | `Async::Loop` event loop and green tasks (experimental) |
| [Packages](PACKAGES.md) | Pacco, the Strada package manager |
| [Template Toolkit](TEMPLATE_TOOLKIT.md) | `lib/Template`: port of Perl's Template::Toolkit |
| [Perl Integration](PERL_INTEGRATION.md) | Perl interop, Perla compiler, and conversion tools |

## Compact References and Man Pages

| Document | Description |
|----------|-------------|
| [Strada Quick Guide](strada-quick.md) | Compact working memory for writing and maintaining Strada programs |
| [Quick Memory](STRADA_QUICK_MEMORY.md) | Single-file reminder for day-to-day Strada programming |
| [Strada for Claude](STRADA_FOR_CLAUDE.md) | Self-contained reference for writing Strada code |
| [strada(1)](strada.1.md), [stradac(1)](stradac.1.md) | Man pages for the driver and the compiler |
| [extended/](extended/) | Topic deep-dives: async, filehandles, math, memory, regex, repl, structs, sys (core::), types |

## Quick Example

```strada
func main() int {
    say("Hello, Strada!");

    my array @numbers = (1, 2, 3, 4, 5);
    my scalar $sum = 0;

    foreach my int $n (@numbers) {
        $sum = $sum + $n;
    }

    say("Sum: " . $sum);
    return 0;
}
```

Compile and run:

```bash
./strada hello.strada     # Creates ./hello
./hello                   # Runs the program
```

## Language Highlights

### Type System
- **Static typing** with `int`, `num`, `str`, `scalar`, `array`, `hash`
- **Type inference** for literals
- **Reference types** with `\$var`, `\@arr`, `\%hash`

### Perl Heritage
- Sigils: `$scalar`, `@array`, `%hash`
- String operators: `.` (concat), `x` (repeat), `eq`, `ne`, `lt`, `gt`
- Control flow: `if/elsif/else` (also `else if`), `unless`, `while`, `until`, `for`, `foreach`, `do-while`, statement modifiers
- Built-ins: `say`, `push`, `pop`, `splice`, `keys`, `values`, `each`, `split`, `join`
- Transliteration: `tr///` / `y///` for character replacement
- Scoping: `local` for dynamic scoping of `our` (global) variables
- I/O: `select($fh)` for setting default output filehandle
- Regex: `/e` modifier for expression evaluation in substitutions
- Magic variables: `tie`/`untie`/`tied` for custom variable implementations

### Modern Features
- **Closures** with captured variables (scalars are captured by value)
- **Multithreading** with mutexes and condition variables
- **Exception handling** with try/catch/throw
- **OOP** with multiple inheritance, SUPER::, DESTROY
- **FFI** for calling C libraries
- **Regex** with inline `/pattern/` syntax

### REPL and Scripting
- `strada --repl` routes through `strada-jit` (real native compilation via tcc/gcc + dlopen), so the REPL supports the full language — closures, try/catch, OOP, the lot. tcc is auto-detected for snappier per-line latency; falls back to gcc.
- `strada --script FILE` runs in **whole-file mode** by default (`gcc` slurps the whole file once and exec's the binary). Pass `--chunked` for line-by-line REPL semantics.
- Bytecode VM and tree-walking interpreter still exist under `interpreter/strada-interp` (opt-in via `make interpreter`). Embedded `Strada::Interpreter::eval_string()` defaults to the tree-walking backend so it works out of the box without `-DHAVE_VM` in the runtime.

### Performance
- Compiles to C, then native code
- Reference counting for memory management
- Direct access to system calls

## Project Status

Strada is a fully functional, self-hosting language with:
- ~74,000 lines of compiler code (in Strada)
- ~29,500 lines of runtime code (in C)
- 260+ example programs
- Comprehensive standard library

## Getting Help

- Check the [Tutorial](TUTORIAL.md) for guided learning
- See [Examples](EXAMPLES.md) for working code
- Read [Language Manual](LANGUAGE_MANUAL.md) for details
- Browse [Quick Reference](QUICK_REFERENCE.md) for syntax

## Project Documentation

In addition to this `docs/` directory, see these key files in the project root:

| File | Description |
|------|-------------|
| `README.md` | Project overview and quick start |
| `FEATURES.md` | Complete feature matrix |


## Version

This documentation covers Strada as of November 2026.

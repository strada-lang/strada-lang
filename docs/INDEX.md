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
| [OOP Guide](OOP_GUIDE.md) | Object-oriented programming |
| [Examples](EXAMPLES.md) | Annotated code examples |

## Advanced Topics

| Document | Description |
|----------|-------------|
| [Debugging Guide](DEBUGGING.md) | Using GDB with Strada programs |
| [FFI Guide](FFI_GUIDE.md) | C integration and foreign functions |
| [Perl Integration](PERL_INTEGRATION.md) | Calling Perl from Strada and vice versa |
| [Runtime API](RUNTIME_API.md) | C runtime library reference |
| [Compiler Architecture](COMPILER_ARCHITECTURE.md) | How the compiler works |

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
- Scoping: `local()` for dynamic scoping of global variables
- I/O: `select($fh)` for setting default output filehandle
- Regex: `/e` modifier for expression evaluation in substitutions
- Magic variables: `tie`/`untie`/`tied` for custom variable implementations

### Modern Features
- **Closures** with captured variables
- **Multithreading** with mutexes and condition variables
- **Exception handling** with try/catch/throw
- **OOP** with multiple inheritance, SUPER::, DESTROY
- **FFI** for calling C libraries
- **Regex** with inline `/pattern/` syntax

### Performance
- Compiles to C, then native code
- Reference counting for memory management
- Direct access to system calls

## Project Status

Strada is a fully functional, self-hosting language with:
- ~2,300 lines of compiler code (in Strada)
- ~5,000 lines of runtime code (in C)
- 93+ working example programs
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
| `QUICKSTART.md` | 5-minute introduction |
| `FEATURES.md` | Complete feature matrix |

Archived historical documentation is in `docs/archive/`.

## Version

This documentation covers Strada as of February 2026.

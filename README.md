# Strada

A strongly-typed, self-hosting programming language with Perl-inspired syntax that compiles to native code via C.

## This should be considered an experimental language.

## Quick Start

```bash
# Build the compiler
make

# Write a program
cat > hello.strada << 'EOF'
func main() int {
    say("Hello, World!");
    return 0;
}
EOF

# Compile and run
./strada -r hello.strada
```

## Features

- **Static typing** with `int`, `num`, `str`, `scalar`, `array`, `hash`
- **Perl-like syntax** - familiar sigils, operators, and control structures
- **Self-hosting** - the compiler is written in Strada (~2,300 lines)
- **Native compilation** - compiles to C, then to native executables
- **Object-oriented** - blessed references, multiple inheritance, destructors
- **Closures** - anonymous functions with captured variables
- **Multithreading** - threads, mutexes, condition variables
- **FFI** - call C libraries directly
- **Regular expressions** - inline `/pattern/` syntax
- **Exception handling** - try/catch/throw

## Documentation

### Getting Started

| Document | Description |
|----------|-------------|
| [Installation](docs/INSTALLATION.md) | Build and install Strada |
| [Tutorial](docs/TUTORIAL.md) | Learn Strada step-by-step |
| [Quick Reference](docs/QUICK_REFERENCE.md) | Syntax cheat sheet |

### Language Reference

| Document | Description |
|----------|-------------|
| [Language Manual](docs/LANGUAGE_MANUAL.md) | Complete language specification |
| [Language Guide](docs/LANGUAGE_GUIDE.md) | In-depth language tutorial |
| [OOP Guide](docs/OOP_GUIDE.md) | Object-oriented programming |
| [Examples](docs/EXAMPLES.md) | Annotated code examples |

### Advanced Topics

| Document | Description |
|----------|-------------|
| [FFI Guide](docs/FFI_GUIDE.md) | C integration |
| [Perl Integration](docs/PERL_INTEGRATION.md) | Perl interoperability |
| [Runtime API](docs/RUNTIME_API.md) | C runtime reference |
| [Compiler Architecture](docs/COMPILER_ARCHITECTURE.md) | Compiler internals |

## Example

```strada
package Point;

func Point_new(int $x, int $y) scalar {
    my hash %self = ();
    $self{"x"} = $x;
    $self{"y"} = $y;
    return bless(\%self, "Point");
}

func Point_distance(scalar $self, scalar $other) num {
    my int $dx = $other->{"x"} - $self->{"x"};
    my int $dy = $other->{"y"} - $self->{"y"};
    return math::sqrt($dx * $dx + $dy * $dy);
}

package main;

func main() int {
    my scalar $p1 = Point_new(0, 0);
    my scalar $p2 = Point_new(3, 4);

    say("Distance: " . Point_distance($p1, $p2));  # 5.0

    return 0;
}
```

## Build Commands

```bash
make                    # Build self-hosting compiler
make run PROG=name      # Compile and run examples/name.strada
make test               # Run test suite
make examples           # Build all examples
make clean              # Clean build artifacts
```

## Compilation Options

```bash
./strada program.strada      # Compile to executable
./strada -r program.strada   # Compile and run
./strada -c program.strada   # Keep generated C code
./strada -g program.strada   # Include debug symbols
```

## Architecture

```
Bootstrap Compiler (C)      bootstrap/stradac  (frozen)
        |
        v compiles
Self-Hosting Compiler       ./stradac          (primary development)
        |
        v compiles
Your Strada Programs
```

The self-hosting compiler in `compiler/*.strada` is the primary codebase. The bootstrap compiler exists only to compile it.

## Project Structure

```
strada/
├── stradac              # Self-hosting compiler
├── strada               # Compilation wrapper script
├── runtime/             # C runtime library
├── compiler/            # Compiler source (in Strada)
├── bootstrap/           # Bootstrap compiler (in C)
├── examples/            # Example programs
├── lib/                 # Standard library modules
├── docs/                # Documentation
└── t/                   # Test suite
```

## Contributing

All development happens in the self-hosting compiler (`compiler/*.strada`). The bootstrap compiler is frozen.

```bash
# Edit compiler
vim compiler/Parser.strada

# Rebuild
make

# Test
make run PROG=your_test
```

## License

Copyright (c) 2026 Michael J. Flickinger

This program is free software: you can redistribute it and/or modify
it under the terms of the GNU General Public License as published by
the Free Software Foundation, version 2.
 
This program is distributed in the hope that it will be useful, but
WITHOUT ANY WARRANTY; without even the implied warranty of
MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
General Public License for more details.
 
You should have received a copy of the GNU General Public License
along with this program. If not, see <http://www.gnu.org/licenses/>.


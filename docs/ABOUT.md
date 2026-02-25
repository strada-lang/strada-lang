# About Strada

Strada is a strongly-typed programming language inspired by Perl. It compiles
to C, which is then compiled to native executables using gcc. The compiler
is self-hosting, meaning it is written in Strada itself.

## Philosophy

Strada combines the expressiveness and flexibility of Perl with the safety
and performance of compiled languages. It aims to be:

- **Familiar** - Perl-like syntax with sigils, regular expressions, and
  similar control flow
- **Safe** - Strong static typing catches errors at compile time
- **Fast** - Compiles to optimized C code for native performance
- **Practical** - Batteries included with file I/O, networking, and more

## Key Features

### Strong Static Typing

Unlike Perl, Strada requires explicit type declarations:

```strada
my int $count = 0;
my str $name = "Strada";
my num $pi = 3.14159;
my array @items = (1, 2, 3);
my hash %config = { "debug" => 1 };
```

### Perl-Like Syntax

Familiar sigils and operators:

- `$` for scalars
- `@` for arrays
- `%` for hashes
- `\` for references
- `->` for dereferencing and method calls
- `=~` for regex matching

### Native Compilation

Strada compiles to C, then to native machine code:

```
Source (.strada) -> C Code (.c) -> Native Binary
```

This provides:

- Fast execution speed
- Small binary size
- Easy deployment

### Tree-Walking Interpreter

Strada also includes a tree-walking interpreter that executes programs directly from the AST without generating C code. This enables:

- **Interactive REPL** for experimentation
- **Quick scripting** without a compilation step
- **Embedded eval** via `Strada::Interpreter::eval_string()`
- **No C compiler required** at runtime

See the [Interpreter Guide](INTERPRETER.md) for details.

### Object-Oriented Programming

Strada supports OOP with blessed references:

```strada
package Dog;

func new(str $name) scalar {
    my hash %self = { "name" => $name };
    return bless(\%self, "Dog");
}

func speak(scalar $self) void {
    say($self->{"name"} . " says woof!");
}
```

### Regular Expressions

Full regex support with familiar syntax:

```strada
if ($line =~ /^Hello, (.+)!$/) {
    my array @caps = captures();
    say("Greeting: " . $caps[1]);
}

$text =~ s/foo/bar/g;
$text =~ s/(\d+)/$1 * 2/e;   # /e modifier for expression evaluation
```

### Perl-Compatible Features

Many Perl idioms work natively in Strada:

- **String repeat**: `"abc" x 3` produces `"abcabcabc"`
- **splice()**: Insert, remove, and replace array elements
- **each(%hash)**: Iterate over hash key-value pairs
- **select($fh)**: Set default output filehandle
- **tr///**: Character transliteration (`$s =~ tr/a-z/A-Z/`)
- **local()**: Dynamic scoping for global variables
- **tie/untie/tied**: Custom variable implementations with magic methods

### Exception Handling

Try/catch for error handling:

```strada
try {
    my scalar $fh = core::open("file.txt", "r");
    # ...
} catch ($e) {
    say("Error: " . $e);
}
```

### Foreign Function Interface

Call C libraries directly:

```strada
my int $lib = core::dl_open("./mylib.so");
my int $fn = core::dl_sym($lib, "my_function");
my scalar $result = core::dl_call_sv($fn, [$arg1, $arg2]);
```

### Shared Libraries

Compile Strada code as shared libraries:

```bash
./strada --shared mylib.strada
```

Then use from other Strada programs or from Perl.

## History

Strada was created to explore what Perl might look like with static typing
and native compilation. The name "Strada" is Italian for "road" or "street",
reflecting the language's goal of providing a clear path from dynamic
scripting to compiled performance.

## Self-Hosting Compiler

The Strada compiler is written in Strada itself. This demonstrates the
language's capability and ensures that the compiler is always tested
against real-world Strada code.

The compilation process:

1. **Bootstrap compiler** (written in C) compiles the self-hosting compiler
2. **Self-hosting compiler** (written in Strada) compiles user programs
3. User programs compile to C, then to native binaries

Alternatively, the tree-walking interpreter can execute user programs
directly from the AST, sharing the same Lexer and Parser front-end as
the compiler.

## Use Cases

Strada is well-suited for:

- **System utilities** - Command-line tools and scripts
- **Network servers** - Web servers, API backends, daemons
- **Text processing** - Log parsing, data transformation
- **Automation** - Build scripts, deployment tools
- **Learning** - Understanding compilers and language design

## Comparison with Perl

| Feature | Perl | Strada |
|---------|------|--------|
| Typing | Dynamic | Static |
| Execution | Interpreted | Compiled + Interpreter |
| Sigils | Yes | Yes |
| Regex | Yes | Yes |
| OOP | Yes (multiple styles) | Yes (blessed refs) |
| CPAN | Huge ecosystem | Growing |
| Speed | Good | Faster |
| Memory | Managed | Reference counted |

## Getting Started

Compile and run your first program:

```strada
# hello.strada
func main() int {
    say("Hello, World!");
    return 0;
}
```

```bash
./strada hello.strada
./hello
```

See the **TUTORIAL** and **LANGUAGE_GUIDE** for more information.

## License

Strada is free software released under the GNU General Public License v2.

## Links

- Documentation: `stradadoc --list`
- Examples: `examples/` directory
- Source: `compiler/` directory

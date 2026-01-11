# Perl Integration Guide

Strada provides bidirectional integration with Perl 5:

1. **Strada calling Perl** - Embed a Perl interpreter in Strada programs
2. **Perl calling Strada** - Load Strada shared libraries from Perl

## Strada Calling Perl (`lib/perl5`)

The `perl5` module allows Strada programs to embed and interact with a Perl 5 interpreter.

### Setup

```bash
# Build the Perl integration library
cd lib/perl5
make
```

### Basic Usage

```strada
use lib "lib";
use perl5;

func main() int {
    # Initialize Perl (required first)
    perl5::init();

    # Evaluate expressions
    my str $result = perl5::eval("2 ** 10");
    say("2^10 = " . $result);  # 1024

    # Define and call subroutines
    perl5::run("sub double { return $_[0] * 2; }");
    my array @args = (21);
    my str $doubled = perl5::call("double", \@args);
    say("21 doubled = " . $doubled);  # 42

    # Use CPAN modules
    if (perl5::use_module("List::Util") == 1) {
        perl5::run("@nums = (1, 5, 3, 9, 2);");
        my str $max = perl5::eval("List::Util::max(@nums)");
        say("Max: " . $max);  # 9
    }

    # Cleanup
    perl5::shutdown();
    return 0;
}
```

### API Summary

| Function | Description |
|----------|-------------|
| `perl5::init()` | Initialize Perl interpreter |
| `perl5::shutdown()` | Shutdown interpreter |
| `perl5::eval($code)` | Evaluate expression, return result |
| `perl5::run($code)` | Execute code (no return) |
| `perl5::call($sub, $args)` | Call subroutine with args |
| `perl5::use_module($mod)` | Load a Perl module |
| `perl5::set_var($name, $val)` | Set Perl variable |
| `perl5::get_var($name)` | Get Perl variable |
| `perl5::get_error()` | Get last Perl error |

### XS Module Support

XS-based Perl modules (like POSIX, JSON::XS) require special handling:

```bash
LD_PRELOAD=/lib/x86_64-linux-gnu/libperl.so.5.38 ./your_program
```

Pure Perl modules work without this.

---

## Perl Calling Strada (`perl/Strada`)

The `Strada` Perl module allows Perl programs to load and call functions from compiled Strada shared libraries.

### Setup

```bash
# Build the Perl XS module
cd perl/Strada
perl Makefile.PL
make
make test
make install  # optional
```

### Creating a Strada Shared Library

1. Write your Strada code:

```strada
# mylib.strada
package mylib;

func add(int $a, int $b) int {
    return $a + $b;
}

func greet(str $name) str {
    return "Hello, " . $name . "!";
}

func process_list(scalar $items) int {
    my int $sum = 0;
    my int $i = 0;
    while ($i < length($items)) {
        $sum = $sum + $items->[$i];
        $i = $i + 1;
    }
    return $sum;
}
```

2. Compile as shared library:

```bash
./stradac mylib.strada mylib.c
gcc -shared -fPIC -rdynamic -o libmylib.so mylib.c \
    runtime/strada_runtime.c -Iruntime -ldl -lm
```

### Using from Perl

```perl
use Strada;

# Load the library
my $lib = Strada::Library->new('./libmylib.so');

# Call functions - both naming styles work:
my $sum = $lib->call('mylib_add', 10, 20);        # C-style
my $sum = $lib->call('mylib::add', 10, 20);       # Strada-style (auto-converted)
print "10 + 20 = $sum\n";  # 30

my $greeting = $lib->call('mylib::greet', 'Perl');
print "$greeting\n";  # Hello, Perl!

# Pass arrays
my $total = $lib->call('mylib::process_list', [1, 2, 3, 4, 5]);
print "Sum: $total\n";  # 15

# Unload when done
$lib->unload();
```

### Function Naming Convention

Strada functions are exported as: `<package>_<function>` (single underscore)

The Perl module accepts both styles - it auto-converts `::` to `_`:

| Strada | C Name | Perl Call (both work) |
|--------|--------|----------------------|
| `package foo; func bar()` | `foo_bar` | `'foo_bar'` or `'foo::bar'` |
| `package utils; func format()` | `utils_format` | `'utils_format'` or `'utils::format'` |

### Type Conversion

| Strada | Perl |
|--------|------|
| `int` | Integer (IV) |
| `num` | Float (NV) |
| `str` | String (PV) |
| `array` | Array reference |
| `hash` | Hash reference |
| `undef` | undef |

### Variadic Functions

Strada variadic functions can be called from Perl. The variadic arguments are passed as an array reference:

**Strada Library:**
```strada
package mylib;

# Variadic function
func sum_all(int ...@nums) int {
    my int $total = 0;
    foreach my int $n (@nums) {
        $total = $total + $n;
    }
    return $total;
}

# Fixed + variadic
func format_nums(str $prefix, str $sep, int ...@nums) str {
    my str $result = $prefix;
    my int $first = 1;
    foreach my int $n (@nums) {
        if ($first == 0) { $result = $result . $sep; }
        $result = $result . $n;
        $first = 0;
    }
    return $result;
}
```

**Perl Usage:**
```perl
use Strada;

my $lib = Strada::Library->new('./libmylib.so');

# Pass variadic args as array reference
my $sum = $lib->call('mylib::sum_all', [1, 2, 3, 4, 5]);
print "Sum: $sum\n";  # 15

# Fixed params + variadic array
my $formatted = $lib->call('mylib::format_nums', 'Values: ', ', ', [10, 20, 30]);
print "$formatted\n";  # "Values: 10, 20, 30"
```

**Library Introspection:**
The `functions()` method returns variadic function info:

```perl
my $funcs = $lib->functions();
for my $name (keys %$funcs) {
    my $f = $funcs->{$name};
    print "Function: $name";
    print " (variadic)" if $f->{is_variadic};
    print "\n";
}
```

The `describe()` method shows variadic params with `...@` prefix:

```perl
print $lib->describe();
# Output:
# func mylib_sum_all(int ...@a) int
# func mylib_format_nums(str $a, str $b, int ...@c) str
```

---

## Use Cases

### Leverage CPAN from Strada

Access thousands of CPAN modules:

```strada
use lib "lib";
use perl5;

func main() int {
    perl5::init();

    # Use LWP for HTTP requests
    if (perl5::use_module("LWP::Simple") == 1) {
        my str $content = perl5::eval("get('http://example.com')");
        say("Got " . length($content) . " bytes");
    }

    # Use DBI for databases
    if (perl5::use_module("DBI") == 1) {
        perl5::run("$dbh = DBI->connect('dbi:SQLite:test.db')");
        # ... database operations
    }

    perl5::shutdown();
    return 0;
}
```

### High-Performance Strada from Perl

Write performance-critical code in Strada, use from Perl:

```perl
use Strada;

my $lib = Strada::Library->new('./libcompute.so');

# Call optimized Strada functions
for my $data (@large_dataset) {
    my $result = $lib->call('compute::process', $data);
    push @results, $result;
}

$lib->unload();
```

### Mixed Language Projects

Combine the best of both worlds:
- Strada for type-safe, compiled performance
- Perl for rapid prototyping and CPAN ecosystem

---

## Directory Structure

```
lib/
  perl5.strada           # Strada module for calling Perl
  perl5/
    strada_perl5.c       # C implementation
    Makefile             # Build config
    libstrada_perl5.so   # Compiled library
    README.md            # Documentation

perl/
  Strada/
    Strada.xs            # XS implementation
    Strada.pm            # Perl module
    Makefile.PL          # Build config
    README.md            # Documentation
    example/
      math_lib.strada    # Example Strada library
      build.sh           # Build script
    t/
      01_basic.t         # Test suite
```

---

## Converting Between Strada and Perl

Strada includes three tools for converting code between Strada and Perl:

### perl2strada - Convert Perl Scripts and Modules

Multi-pass converter that translates `.pl` and `.pm` files to Strada:

```bash
# Build the tool
./strada tools/perl2strada.strada

# Convert a script
./perl2strada myscript.pl myscript.strada

# Convert a module
./perl2strada lib/MyModule.pm lib/MyModule.strada
```

The converter handles:
- Subroutine definitions with parameter unpacking (`my ($a, $b) = @_;` → typed parameters)
- Variable declarations with type inference
- Hash key quoting (`$h{key}` → `$h{"key"}`)
- `eval { } if ($@)` → `try { } catch ($e) { }`
- File and system operations → `core::`/`core::` namespace
- Regular expressions and string operations
- `die`/`warn`/`chomp`/`open`/`close` and 100+ other Perl patterns
- `wantarray()` → `core::wantarray()` (use `dynamic` return type for context-sensitive functions)

Lines marked `# REVIEW:` need manual type checking. Lines marked `# TODO:` indicate features that don't exist in Strada.

Run tests with: `bash t/perl2strada/run_tests.sh`

### xs2strada - Convert XS Modules

Converts Perl XS modules into Strada code using `__C__` blocks:

```bash
# Build the tool
./strada tools/xs2strada.strada

# Convert an XS module
./xs2strada MyModule.xs MyModule.strada
```

The converter:
- Parses XS function definitions and maps types to Strada equivalents
- Preserves C code in `__C__` blocks
- Replaces XS macros with Strada runtime functions (`SvIV` → `strada_to_int`, etc.)
- Strips Perl-specific thread context (`aTHX_`, `pTHX_`, `dXSARGS`)
- Handles `CODE:`, `PREINIT:`, `OUTPUT:`, `CLEANUP:`, and `BOOT:` sections
- Generates parameter extraction and cleanup code

This is useful for porting C libraries that already have Perl XS bindings to Strada.

### strada2perl - Convert Strada to Perl

Comprehensive converter that translates Strada source code into equivalent Perl 5 code. This is the reverse of `perl2strada` and is useful for porting Strada projects back to Perl, generating Perl reference implementations, or sharing code with Perl-only teams.

```bash
# Build the tool
./strada tools/strada2perl.strada

# Convert a Strada file
./strada2perl myapp.strada myapp.pl

# Auto-name output (myapp.strada -> myapp.pl)
./strada2perl myapp.strada
```

**Key conversions:**

| Strada | Perl |
|--------|------|
| `my int $x = 10;` | `my $x = 10;` |
| `our str $name = "hi";` | `our $name = "hi";` |
| `func add(int $a, int $b) int { }` | `sub add { my ($a, $b) = @_; }` |
| `private func helper() void { }` | `sub helper { }` |
| `async func task() str { }` | `sub task { # was async }` |
| `func () int { ... }` (closure) | `sub { ... }` |
| `func main() int { ... return 0; }` | Body unwrapped to inline code |
| `core::getcwd()` / `core::getcwd()` | `Cwd::getcwd()` |
| `core::exit(1)` / `core::exit(1)` | `exit(1)` |
| `core::getenv("K")` / `core::getenv("K")` | `$ENV{"K"}` |
| `math::sqrt($n)` | `sqrt($n)` |
| `math::floor($n)` | `POSIX::floor($n)` |
| `try { } catch ($e) { }` | `eval { }; if ($@) { my $e = $@; }` |
| `catch (TypeName $e)` | `if (ref($@) && $@->isa('TypeName'))` |
| `throw $obj` | `die $obj` |
| `import_lib "Mod.so"` | `use Mod; # was import_lib` |
| `__C__ { ... }` | `# __C__ block removed` |
| `enum Status { A, B = 10 }` | `{ package Status; use constant A => 0; ... }` |
| `const int MAX = 100;` | `use constant MAX => 100;` |
| `version "1.0.0";` | `our $VERSION = "1.0.0";` |
| `inherit Parent;` | `use parent -norequire, 'Parent';` |
| `has ro str $name (required);` | `has 'name' => (is => 'ro', isa => 'Str', required => 1);` |
| `extends Parent;` | `extends 'Parent';` (with `use Moose;`) |
| `before "method" func(...) { }` | `before 'method' => sub { ... };` |
| `$1` or `captures()[1]` | `$1` |
| `named_captures()` | `%+` |
| `size(@arr)` | `scalar(@arr)` |
| `say($fh, "msg")` | `say $fh "msg"` |
| `...@args` (spread) | `@args` |
| `::func()` / `.::func()` | `func()` |

**Automatic module detection:** The converter scans the source and adds `use` statements for Perl modules required by the converted code (`Cwd`, `File::Basename`, `POSIX`, `Time::HiRes`, `Carp`, `Moose`).

**Compatibility helpers generated:** The output includes helper subs for Strada built-ins that have no direct Perl syntax equivalent: `match()`, `replace()`, `replace_all()`, `char_at()`, `strada_slurp()`, `strada_spew()`, `strada_open()`, `strada_readline()`, `strada_readdir()`.

**Output annotations:**
- `# REVIEW:` — needs manual checking (unrecognized `core::` functions, `async::` calls, `c::` calls)
- `# was import_lib` / `# was import_object` / `# was import_archive` — import origin
- `# was async` — async keyword stripped
- `# __C__ block removed` — C interop code removed

**Example:**

Strada input:
```strada
package Utils;

func greet(str $name) str {
    my str $upper = uc($name);
    return "Hello, " . $upper . "!";
}

package main;

func main() int {
    my str $cwd = core::getcwd();
    say("Dir: " . $cwd);
    say(Utils::greet("world"));
    return 0;
}
```

Perl output:
```perl
#!/usr/bin/env perl
use strict;
use warnings;
use feature 'say';
use Cwd qw(getcwd realpath);

package Utils;

sub greet {
    my ($name) = @_;
    my $upper = uc($name);
    return "Hello, " . $upper . "!";
}

package main;

# Main
my $cwd = Cwd::getcwd();
say("Dir: " . $cwd);
say(Utils::greet("world"));
```

---

## Native Perl-Compatible Features

The following Perl features are natively supported in Strada and do not require the `perl5` module:

| Feature | Strada Syntax | Perl Equivalent |
|---------|--------------|-----------------|
| String repeat | `"abc" x 3` | `"abc" x 3` |
| splice() | `splice(@arr, $offset, $len, @list)` | `splice(@arr, $offset, $len, @list)` |
| each() | `each(%hash)` | `each(%hash)` |
| select() | `select($fh)` | `select($fh)` |
| tr/// | `$s =~ tr/a-z/A-Z/` | `$s =~ tr/a-z/A-Z/` |
| local() | `local($var) = $val` | `local($var) = $val` |
| /e modifier | `$s =~ s/(\d+)/$1 * 2/e` | `$s =~ s/(\d+)/$1 * 2/e` |
| tie/untie/tied | `tie($var, "Class")` | `tie($var, "Class")` |

These features compile to native C code and require no runtime Perl interpreter. When converting Perl code to Strada (via `perl2strada` or manually), these constructs can be used directly.

## See Also

- `lib/perl5/README.md` - Detailed Perl embedding documentation
- `perl/Strada/README.md` - Detailed XS module documentation
- `examples/test_perl5.strada` - Comprehensive Perl integration example
- `tools/perl2strada.strada` - Perl to Strada converter source
- `tools/strada2perl.strada` - Strada to Perl converter source
- `tools/xs2strada.strada` - XS to Strada converter source
- `t/perl2strada/run_tests.sh` - Converter test suite (266 tests)

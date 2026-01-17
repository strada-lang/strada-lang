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

## See Also

- `lib/perl5/README.md` - Detailed Perl embedding documentation
- `perl/Strada/README.md` - Detailed XS module documentation
- `examples/test_perl5.strada` - Comprehensive Perl integration example

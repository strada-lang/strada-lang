# Strada Perl 5 Integration Library

This library allows Strada programs to embed a Perl 5 interpreter and call Perl code.

## Overview

The `perl5` module provides a bridge from Strada to Perl, enabling you to:
- Initialize and manage an embedded Perl interpreter
- Evaluate Perl expressions and get results
- Call Perl subroutines with arguments
- Use CPAN modules from Strada
- Set and get Perl variables
- Handle Perl errors

## Installation

### Prerequisites

- Perl 5 development headers (`libperl-dev` on Debian/Ubuntu)
- Strada compiler and runtime

### Build Steps

```bash
cd lib/perl5
make
```

This creates `libstrada_perl5.so` which is loaded dynamically by the `perl5` Strada module.

## Usage in Strada

### Basic Example

```strada
use lib "lib";
use perl5;

func main() int {
    # Initialize Perl interpreter (required first)
    perl5::init();

    # Evaluate Perl expressions
    my str $result = perl5::eval("2 + 2");
    say("2 + 2 = " . $result);  # "4"

    # String operations
    $result = perl5::eval("'Hello' . ' ' . 'World'");
    say($result);  # "Hello World"

    # Shutdown when done
    perl5::shutdown();
    return 0;
}
```

### Calling Perl Subroutines

```strada
use lib "lib";
use perl5;

func main() int {
    perl5::init();

    # Define a subroutine
    perl5::run("sub greet { my $name = shift; return \"Hello, $name!\"; }");

    # Call it with arguments
    my array @args = ("World");
    my str $greeting = perl5::call("greet", \@args);
    say($greeting);  # "Hello, World!"

    perl5::shutdown();
    return 0;
}
```

### Using CPAN Modules

```strada
use lib "lib";
use perl5;

func main() int {
    perl5::init();

    # Load a module
    my int $ok = perl5::use_module("JSON");
    if ($ok == 1) {
        # Use module functions via eval
        my str $json = perl5::eval('encode_json({foo => "bar"})');
        say($json);  # {"foo":"bar"}
    }

    perl5::shutdown();
    return 0;
}
```

### Working with Variables

```strada
use lib "lib";
use perl5;

func main() int {
    perl5::init();

    # Set a Perl variable
    perl5::set_var("$greeting", "Hello from Strada");

    # Get it back
    my str $val = perl5::get_var("$greeting");
    say($val);  # "Hello from Strada"

    # Work with arrays in Perl
    perl5::run("@colors = qw(red green blue);");
    my str $count = perl5::eval("scalar(@colors)");
    say("Array has " . $count . " elements");

    perl5::shutdown();
    return 0;
}
```

### Error Handling

```strada
use lib "lib";
use perl5;

func main() int {
    perl5::init();

    # Try some invalid code
    my str $result = perl5::eval("this is not valid perl");

    # Check for errors
    my str $error = perl5::get_error();
    if (length($error) > 0) {
        say("Perl error: " . $error);
    }

    perl5::shutdown();
    return 0;
}
```

## API Reference

### perl5::init() -> int

Initialize the Perl interpreter. Must be called before any other perl5 functions.
Returns 1 on success, 0 on failure.

### perl5::shutdown()

Shutdown the Perl interpreter and free resources. Call when done with Perl.

### perl5::is_init() -> int

Check if Perl is initialized. Returns 1 if initialized, 0 otherwise.

### perl5::eval(str $code) -> str

Evaluate a Perl expression and return the result as a string.
On error, returns the error message.

### perl5::run(str $code)

Execute Perl code without returning a value. Use for statements like
subroutine definitions, variable assignments, etc.

### perl5::call(str $sub_name, scalar $args) -> str

Call a Perl subroutine by name with an array of arguments.
Returns the result as a string.

### perl5::call_list(str $sub_name, scalar $args, str $sep) -> str

Call a Perl subroutine that returns a list, joining results with the separator.

### perl5::use_module(str $module) -> int

Load a Perl module (equivalent to `use Module;`).
Returns 1 on success, 0 on failure.

### perl5::require_module(str $module) -> int

Require a Perl module (equivalent to `require Module;`).
Returns 1 on success, 0 on failure.

### perl5::set_var(str $name, str $value)

Set a Perl scalar variable. Include the `$` sigil in the name.

### perl5::get_var(str $name) -> str

Get the value of a Perl scalar variable.

### perl5::get_error() -> str

Get the last Perl error message (`$@`). Returns empty string if no error.

### perl5::add_inc(str $path)

Add a path to Perl's `@INC` for module searching.

## XS Module Support

Some Perl modules (like `POSIX`, `JSON::XS`, etc.) are implemented in C (XS modules).
These require special handling due to symbol loading.

To use XS modules, run your Strada program with:

```bash
LD_PRELOAD=/lib/x86_64-linux-gnu/libperl.so.5.38 ./your_program
```

Pure Perl modules work without this workaround.

## Architecture

```
lib/perl5.strada          # Strada wrapper module (high-level API)
lib/perl5/strada_perl5.c  # C implementation using libperl
lib/perl5/strada_perl5.h  # Header file (optional)
lib/perl5/Makefile        # Build with perl embedding flags
lib/perl5/libstrada_perl5.so  # Compiled shared library
```

The Strada wrapper (`lib/perl5.strada`) uses FFI to load and call functions
from the C library (`libstrada_perl5.so`), which embeds the Perl interpreter.

## Limitations

- Single interpreter per process (Perl limitation)
- XS modules require LD_PRELOAD workaround
- Return values are converted to strings (complex data structures need serialization)
- Thread safety depends on Perl's threading model

## Files

- `strada_perl5.c` - C implementation
- `Makefile` - Build configuration
- `README.md` - This file

## See Also

- `perl/Strada/` - The reverse integration (calling Strada from Perl)
- `examples/test_perl5.strada` - Comprehensive test/example
- `docs/LANGUAGE_GUIDE.md` - Strada language documentation

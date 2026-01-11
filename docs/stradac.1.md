# stradac

Strada compiler - compile Strada source code to C

## SYNOPSIS

**stradac** [*options*] *input.strada* *output.c*

## DESCRIPTION

**stradac** is the Strada compiler. It compiles Strada source code (.strada files) into C code, which can then be compiled with gcc to produce native executables.

Strada is a strongly-typed programming language inspired by Perl. It features Perl-like syntax with sigils ($, @, %), strong static typing, and compiles to efficient C code.

The compiler is self-hosting - it is written in Strada itself.

## OPTIONS

- **-L** *path*
  Add a library search path with high priority. These paths are searched first when resolving `use` statements.

- **-LL** *path*
  Add a library search path with low priority. These paths are searched last.

- **-g**, **--debug**
  Emit `#line` directives in the generated C code. This enables source-level debugging where the debugger shows Strada source lines instead of generated C code.

- **-p**, **--profile**
  Enable function profiling. When enabled, the compiled program will track timing and call counts for each function. At program exit, a profile report is printed.

- **-t**, **--timing**
  Show compilation phase timing. Displays how long each phase of compilation (lexing, parsing, code generation) takes.

- **-w**, **--warnings**
  Show compiler warnings such as unused variables.

- **-h**, **--help**
  Display help message and exit.

## EXIT STATUS

- **0** - Compilation successful
- **1** - Compilation failed (syntax error, type error, etc.)

## EXAMPLES

Compile a Strada program to C:

```
stradac hello.strada hello.c
```

Compile with debug information:

```
stradac -g hello.strada hello.c
```

Compile with custom library paths:

```
stradac -L ./mylibs -L /opt/strada/lib app.strada app.c
```

## FILES

- *~/.strada/lib* - User library directory
- */usr/local/lib/strada/lib* - System library directory (if installed)

## SEE ALSO

**strada**(1), **stradadoc**(1)

## AUTHOR

Michael J. Flickinger

## COPYRIGHT

Copyright (c) 2026 Michael J. Flickinger. Licensed under the GNU General Public License v2.

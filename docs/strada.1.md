# strada

Compile and run Strada programs

## SYNOPSIS

**strada** [*options*] *input.strada* [*extra.c* ...] [*extra.o* ...]

**strada** **--shared** *library.strada*

**strada** **--static** *program.strada*

**strada** **--repl**

**strada** **--script** *file.st*

## DESCRIPTION

**strada** is the main driver script for compiling Strada programs. It wraps the **stradac** compiler and **gcc** to provide a streamlined compilation experience, similar to how `go build` or `rustc` work.

Strada is a strongly-typed programming language inspired by Perl. It features Perl-like syntax with sigils ($, @, %), strong static typing, and compiles to efficient native executables via C.

## OPTIONS

- **-c**
  Keep the generated C file after compilation. Useful for debugging or inspecting the generated code.

- **-r**, **--run**
  Run the program immediately after successful compilation.

- **-o** *file*
  Specify the output file name. By default, the output is named after the input file without the .strada extension.

- **-O** *level*
  Set gcc optimization level (0, 1, 2, 3, s, fast). Default is 2.

- **-L** *path*
  Add a library search path with high priority. These paths are searched first when resolving `use`, `import_lib`, `import_object`, and `import_archive` statements.

- **-LL** *path*
  Add a library search path with low priority.

- **-l** *lib*
  Link with the specified library. For example, `-l ssl -l crypto` to link with OpenSSL.

- **-I** *path*
  Add an include path for C headers. Useful when using `__C__` blocks or extern functions.

- **-g**
  Enable source-level debugging. Emits `#line` directives so gdb shows Strada source lines, plus DWARF debug symbols.

- **--c-debug**
  Enable C-level debugging only. Includes DWARF symbols but no `#line` directives, so gdb shows the generated C code.

- **-p**, **--profile**
  Enable function profiling. The compiled program tracks timing and call counts, printing a report at exit.

- **--shared**
  Compile as a shared library (.so). The library can be loaded at runtime with `import_lib` or via `core::dl_open()`.

- **--static**
  Compile as a fully static binary with no dynamic library dependencies. Produces portable executables.

- **--static-lib**
  Compile as a static library archive (.a) that includes the Strada runtime. Use `import_archive` to link against it.

- **--object**
  Compile to an object file (.o) only, without linking. Use `import_object` to link against it.

- **-w**
  Show compiler warnings (unused variables, etc.).

- **-v**
  Verbose output. Shows the commands being executed.

- **-h**, **--help**
  Display help message and exit.

- **--repl**
  Start the interactive REPL (Read-Eval-Print Loop). Allows experimenting with Strada code without creating files. Supports defining variables and functions that persist across evaluations.

- **--script** *file*
  Run a Strada script file through the REPL interpreter. Scripts don't require a `main()` function - top-level code executes directly. Useful for quick tasks and automation.

## EXTRA FILES

You can include additional C source files (.c) and object files (.o) after the Strada source file. These are compiled and linked into the final executable, which is useful for C interop via `extern` functions.

## EXAMPLES

Compile a simple program:

```
strada hello.strada
./hello
```

Compile and run in one step:

```
strada -r hello.strada
```

Create a shared library:

```
strada --shared mylib.strada
```

Create a static archive (includes runtime):

```
strada --static-lib mylib.strada
```

Create an object file:

```
strada --object mylib.strada
```

Compile with SSL support:

```
strada app.strada lib/ssl/strada_ssl.c -l ssl -l crypto
```

Create a debug build:

```
strada -O0 -g test.strada
gdb ./test
```

Create a portable static binary:

```
strada --static myapp.strada
```

Start the interactive REPL:

```
strada --repl
```

Run a script file:

```
strada --script myscript.st
```

## DOCUMENTATION

To view Strada documentation, use the **stradadoc** command:

```
stradadoc                  # List available topics
stradadoc LANGUAGE         # Language guide
stradadoc QUICK            # Quick reference
stradadoc sys              # core:: namespace functions
stradadoc math             # math:: namespace functions
stradadoc OOP              # Object-oriented programming
```

You can also view man pages:

```
man strada
man stradac
```

## ENVIRONMENT

- **STRADA_LIB**
  Additional library search paths, colon-separated.

## FILES

- *~/.strada/lib* - User library directory
- */usr/local/lib/strada/lib* - System library directory
- */usr/local/share/doc/strada* - Documentation files

## EXIT STATUS

- **0** - Success
- **1** - Compilation or execution failed

## SEE ALSO

**stradac**(1), **strada-jit**(1), **stradadoc**(1), **gcc**(1), **gdb**(1)

## AUTHOR

Michael J. Flickinger

## COPYRIGHT

Copyright (c) 2026 Michael J. Flickinger. Licensed under the GNU General Public License v2.

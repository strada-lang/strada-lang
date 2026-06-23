# strada

Compile and run Strada programs

## SYNOPSIS

**strada** [*options*] *input.strada* [*extra.c* ...] [*extra.o* ...] [*extra.a* ...]

**strada** **--shared** *library.strada*

**strada** **--static** *program.strada*

**strada** **-M** *file_or_dir*

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
  Set gcc optimization level (0, 1, 2, 3, s, fast). Default is 2. At -O2 and above, -flto (link-time optimization) is automatically enabled for cross-function inlining. At -O3 and -Ofast, -march=native is also added.

- **-fno-lto**, **--no-lto**
  Skip link-time optimization. Useful for test/dev builds — gcc finishes in a fraction of the time, at the cost of some cross-module inlining in the final binary. The pre-built runtime is compiled with `-ffat-lto-objects`, so this flag lets you link against it without paying for an LTO pass.

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

- **--full-profile**
  Enable line-level profiling instrumentation (similar to Perl's Devel::NYTProf). Implies `-g` (debug/line info). The compiled program writes a `strada-prof.out` binary file on exit. Use `strada-proftext` or `strada-profhtml` to generate reports from the profile data.

- **--shared**
  Compile as a shared library (.so). The library can be loaded at runtime with `import_lib` or via `core::dl_open()`.

- **--static**
  Compile as a fully static binary with no dynamic library dependencies. Produces portable executables.

- **--static-lib**
  Compile as a static library archive (.a) that includes the Strada runtime. Use `import_archive` to link against it.

- **--tcc**
  Compile the generated program with **tcc** (the Tiny C Compiler) instead of gcc and link the object with the C compiler. Much faster compilation (especially on large programs), at the cost of unoptimized output — good for fast edit/run iteration. Full runtime features are preserved. Requires `tcc` in `PATH`.

- **-D***NAME*[**=***value*]
  Pass a preprocessor define through to the C compiler (e.g. `-DHAVE_MYSQL`). Repeatable.

- **--object**
  Compile to an object file (.o). By default the .o contains only the input file's own symbols — anything brought in by `use` lives in *that* module's `.o`, not yours. Combine with `--object-full` to bundle every transitively-used module's compiled code into one self-contained `.o`.

- **--object-full**
  Compile to a `.o` that bundles all `use`d module code as well (the legacy pre-separate-compilation behaviour). Use this when you want a single drop-in `.o` for distribution.

- **-M** *file_or_dir*
  Precompile module(s). With a `.strada` file, produces a sibling `.o` containing only that file's own symbols. With a directory, walks recursively and produces a sibling `.o` for each `.strada` underneath. The resulting `.o`s are picked up automatically by `use Foo;` when sitting next to `Foo.strada` (mtime-fresh).

- **--use-artifacts**
  Prefer a fresh precompiled sibling `Foo.o`/`Foo.so` over re-parsing `Foo.strada` when resolving `use Foo;`. **This is the default.** A module's interface is read in-process from its `.strada_meta` section (no subprocess, no `dlopen`, no module code executed at compile time). Two freshness gates apply: the artifact must be at least as new as its source *and* as new as `stradac`; otherwise the source is recompiled. Note that the chosen `.o` is linked into your binary, so `--use-artifacts` trusts the sibling artifact in your tree over the source you can see.

- **--no-use-artifacts**
  Disable the above: always recompile `use`d modules from their `.strada` source, ignoring any sibling `.o`/`.so`. Use this when you want "compile exactly the source in front of me" (untrusted trees, or to rule out a stale artifact). The explicit **import_object** / **import_lib** / **import_archive** forms are unaffected and always honored. Equivalent to `STRADA_USE_ARTIFACTS=0`.

- **--import-lib** *path*
  Load a Strada-module shared library (`.so`) at runtime, exactly as if the source contained `import_lib "path";`. A Strada-module `.so` passed as a bare positional argument is treated the same way. (Plain C `.so`s are linked, not import_lib'd.)

- **--module-cache**
  Use and warm the precompiled module cache (separate compilation, keyed by source path and `-D` define set). Implies **--use-artifacts**. Speeds up rebuilds of multi-module projects.

- **--clear-module-cache**
  Wipe the module cache directory and exit. Always safe; the next `--module-cache` build regenerates it.

- **--strict-types**
  Enable stage-0 gradual type checking: compares declared types against a best-effort static expression type and emits warnings (never errors) on likely mismatches. `scalar`/`dynamic`/unannotated values never warn.

- **--no-stack-trace**
  Omit stack-trace frame tracking from the compiled program. Slightly faster; uncaught exceptions no longer print a Strada call stack.

- **-w**, **--warnings**
  Show compiler warnings (unused variables, etc.).

- **-v**
  Verbose output. Shows the commands being executed.

- **-h**, **--help**
  Display help message and exit.

- **--repl**
  Start the interactive REPL (Read-Eval-Print Loop), routed through **strada-jit**. The JIT compiles each balanced chunk to a `.so` and dlopens it, so the REPL supports the full Strada language (closures, try/catch, OOP, etc.). Compiler backend is auto-detected: **tcc** when installed (snappier per-line latency), **gcc** otherwise. Override with `--compiler=tcc` or `--compiler=gcc`.

- **--script** *file*
  Compile and run a Strada script via **strada-jit** in whole-file mode: the entire file is handed to `stradac` once and the resulting binary is exec'd. Defaults to `gcc` (the latency cost is amortised over the whole script and gcc enables `try/catch`, `const`, `enum`). Pass `--compiler=tcc` for fast TCC compilation, or `--chunked` to fall back to per-statement REPL semantics.

## EXTRA FILES

You can include additional C source files (.c), object files (.o), and static archives (.a) after the Strada source file.

C source files are compiled and linked into the final executable — useful for C interop via `extern` functions.

`.o` and `.a` files are detected automatically:

- **Strada module objects** (recognised by an `__strada_export_info` symbol via `nm`) behave like an implicit `import_object` / `import_archive`. The compiler picks up the package metadata; you can call its functions by their namespace without declaring `import_object` in source.
- **Plain extern-C objects/archives** are simply linked in.

Extern-C link dependencies declared in source via `link_lib "name";` (e.g. DBI declaring `"mysqlclient"`) propagate through the precompiled `.o` and are added to the final gcc link automatically as `-l<name>`.

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

Create a module-only object file (Foo.o contains only Foo's own symbols):

```
strada --object MyLib.strada
```

Precompile a single module to a sibling `.o`:

```
strada -M lib/MyLib.strada
# -> lib/MyLib.o
```

Precompile every `.strada` in a tree:

```
strada -M src/
# -> sibling .o next to each .strada under src/
```

Use a precompiled module via `use Foo;` (auto-detect):

```
# Given lib/MyLib.strada + lib/MyLib.o (mtime-fresh):
strada main.strada                 # use MyLib; picks up MyLib.o; no re-inlining
```

Bundle all transitively-used modules into one `.o` (legacy):

```
strada --object-full mylib.strada
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

Profile a program with line-level detail:

```
strada --full-profile myapp.strada
./myapp                                    # writes strada-prof.out
strada-proftext strada-prof.out            # text report to stdout
strada-profhtml strada-prof.out profhtml/  # HTML report
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

- **STRADA_USE_ARTIFACTS**
  Controls whether `use Foo;` prefers a fresh sibling `Foo.o`/`Foo.so` over the source. Enabled by default; set to `0` to disable (equivalent to **--no-use-artifacts**). Any other value (or unset) leaves it enabled.

- **STRADA_MODULE_CACHE_DIR**
  Directory for the `--module-cache` artifacts (default `~/.cache/strada/modules`).

- **CC**
  C compiler used for the final compile/link (default `gcc`; on macOS this is clang).

## FILES

- *~/.strada/lib* - User library directory
- */usr/local/lib/strada/lib* - System library directory
- */usr/local/share/doc/strada* - Documentation files

## EXIT STATUS

- **0** - Success
- **1** - Compilation or execution failed

## SEE ALSO

**stradac**(1), **strada-jit**(1), **strada-proftext**(1), **strada-profhtml**(1), **stradadoc**(1), **gcc**(1), **gdb**(1)

## AUTHOR

Michael J. Flickinger

## COPYRIGHT

Copyright (c) 2026 Michael J. Flickinger. Licensed under the GNU General Public License v2.

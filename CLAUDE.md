# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project Overview

Strada is a strongly-typed programming language inspired by Perl. It compiles to C, which is then compiled to native executables. The compiler is self-hosting (written in Strada itself).

## Related Projects

- **Cannoli** - Preforking web server and framework for Strada
  - Repository: https://github.com/strada-lang/cannoli

## Build Commands

```bash
./configure                 # Detect dependencies (MySQL, PostgreSQL, OpenSSL, etc.)
make                        # Build self-hosting compiler (./stradac) via full bootstrap chain
make quick                  # Fast compiler edit/test loop (~4s with tcc, ~30s without) — see below
QUICK=1 make test-suite     # Fast suite: tests via tcc, delegated builds at -O0 — see below
make DEV=1                  # Build everything at -O0 (fast compile, slower binaries)
make libs                   # Build libraries (DBI, crypt, ssl, readline)
make tools                  # Build tools (stradadoc, strada-soinfo, strada-proftext, strada-profhtml, etc.)
make run PROG=test_simple   # Compile and run an example program
make test                   # Test runtime system
make test-selfhost          # Verify self-compilation (stage 2)
make test-suite             # Run comprehensive test suite (269 tests)
make test-suite V=1         # Verbose test output
make examples               # Build all example programs
make install                # Install to /usr/local (or PREFIX=/path make install)
make clean                  # Remove build artifacts
```

### Fast Compiler Iteration (`make quick`)

A normal `make` runs the full bootstrap chain, which compiles the ~60k-line
generated C **twice** through gcc at `-O2` (once for the throwaway stage-1
compiler, once for the final), ~3.5 min each on a 2-core box — so a single edit
to `compiler/*.strada` costs ~7 min.

`make quick` collapses this to ~30s by using the **existing** `./stradac` to
recompile the compiler in place — one gcc pass at `-O0`, skipping the
bootstrap → stage-1 path entirely. Use it as the day-to-day edit/test loop. Two
caveats:

- It requires a working `./stradac` already on disk (run `make` once first).
- It does **not** exercise the frozen bootstrap or verify the self-host fixpoint.
  Before committing, run `make` (full bootstrap build) and `make test-selfhost`.

**tcc (2026-11)**: when `tcc` is on PATH, both throwaway compilers — `make quick`'s and
the stage-1 compiler of a full `make` — compile their generated C with tcc and
link with `$(CC)` (the `TCC_COMPILER` macro in the
Makefile): ~0.2s instead of ~20s of gcc `-O0`, so the `make quick` loop is ~4s and stage 1 builds in 0.3s instead of ~28s
(the stage-2 `Combined.c` it emits is byte-identical). Without
tcc it uses gcc `-O0` as before. The macro swaps only the anchored `#include`
line to `strada_runtime_tcc.h` (never a string literal — the compiler's own
source mentions the header name), treats tcc's "implicit declaration" WARNING as
a failure (a runtime function missing from `strada_runtime_tcc.h` would become
an implicit `int` → truncated pointer → a compiler that crashes at run time),
and falls back to gcc `-O0` on any tcc/link failure. If you see "tcc build
failed -- falling back", add the named prototype to `strada_runtime_tcc.h`.

**`QUICK` environment variable (opt-in; unset/empty/`0` = off)** — test-suite
shortcuts that change *what gets exercised*, so they are never the default:

- `QUICK=1 ./t/run_tests.sh` (or `QUICK=1 make test-suite`): implies
  `STRADA_TEST_TCC=1` (tests compile with tcc, link with gcc; gcc only for tests
  tcc can't compile) and `STRADA_DEFAULT_OPT=0` (the delegated `t/*/run.sh`
  scenarios build at `-O0` instead of `-O2`+LTO). ~35s vs ~85s+ warm. Explicit
  settings of either variable win. Run the suite without `QUICK` before committing.
- `STRADA_DEFAULT_OPT=0|1|2|3|s|fast` is a driver knob (`parse_args` in
  `tools/strada-driver.strada`): the opt level when no `-O` flag is given.

Unconditional (output-neutral) companions: the final `stradac` rule and the test
runner's gcc path compile and link as **two** commands, because ccache never
caches a command that links (the one-step forms were 100% cache misses). The
test runner compiles from inside its build dir on a relative file name — the dir
contains the run's PID, and an absolute path would defeat the cache.

Note also that the stage-1 compiler is throwaway (its only job is to regenerate
`Combined.c`), so it is now built at `-O0` (`FAST_CFLAGS`) regardless of `DEV`,
which roughly halves a clean `make` with no effect on the final compiler.

**Interpreter build (`make interpreter`, 2026-11)**: `interpreter/Makefile` compiles
`Combined.c`, `vm.c`, `vm_compiler.c` and the runtime as separate ccache-wrapped
objects (`interp_*.o`); the top-level target runs the sub-make with `-j$(nproc)`.
The generated front end (`Combined.c`) builds at `-O1` (gcc `-O2` on it took ~70s;
same trade as `STRADAC_CFLAGS`); the VM and runtime stay `-O2`. `DEV=1`/`QUICK=1`
→ `-O0` (~2s); override with `INTERP_FRONT_OPT=-O2`. The front-end object name
carries its opt level (`interp_Combined-O1.o`), so switching levels never reuses a
stale object. Note `make interpreter` depends on `stradac`, so after a
`compiler/*.strada` edit it first runs the FULL bootstrap. The fast loop after a
compiler edit is `make quick && make -C interpreter -j2 QUICK=1` (~9s). Compiled
Strada programs are unaffected by any of this.

### Configure Script

```bash
./configure                    # Detect all available libraries
./configure --with-mysql       # Require MySQL (fail if not found)
./configure --without-postgres # Skip PostgreSQL
./configure --prefix=/opt      # Set installation prefix
./configure --without-cycle-gc # Compile out the automatic cycle collector (default on)
./configure --without-arena    # Compile out the request arena (default on)
./configure --without-epoll    # Disable the epoll event loop (Async::Loop; Linux-only, auto-detected)
```

Detected libraries: MySQL, SQLite, PostgreSQL, libcrypt, OpenSSL, PCRE2, readline, zlib, libusb

**macOS / OpenSSL (2026-06)**: Apple ships LibreSSL and no OpenSSL headers in the
SDK, so OpenSSL comes from Homebrew (`brew install openssl@3`), kept *keg-only*
(off the default include/lib path). `./configure` detects it via `brew --prefix
openssl@3` and sets `SSL_CFLAGS=-I<prefix>/include`, `SSL_LIBS=-L<prefix>/lib
-lssl -lcrypto`. The driver injects `STRADA_SSL_CFLAGS` into the `lib/ssl`
compile (so the `#include <openssl/ssl.h>` __C__ block resolves) and adds the
`-L` dir for any ssl/crypto `link_lib` (so the keg-only libs link). Without
OpenSSL, `make` skips `lib/ssl` cleanly (the SSL lib build is gated on
`HAVE_SSL`). All of this is no-op on Linux, where OpenSSL is on the default path.

Memory-management features (compile-time, default on): cycle collector (`--with/--without-cycle-gc`), request arena (`--with/--without-arena`). See "Memory Management and Leak Detection" below.

## Compilation

```bash
# One-step compilation (recommended)
./strada input.strada           # Creates ./input executable
./strada -r input.strada        # Compile and run
./strada -c input.strada        # Keep the generated .c file
./strada -g input.strada        # Include debug symbols
./strada --shared mylib.strada  # Create shared library (mylib.so)
./strada --static-lib mylib.strada  # Create static library (mylib.a)
./strada --tcc input.strada     # Compile with tcc (fast, unoptimized), link with $CC
./strada --full-profile input.strada  # Line-level profiling (writes strada-prof.out)

# Optimization levels (passed through to gcc)
./strada -O2 input.strada            # Enables -flto (link-time optimization)
./strada -O3 input.strada            # Enables -flto and -march=native
./strada -Ofast input.strada         # Enables -flto and -march=native

# Two-step compilation (manual)
./stradac input.strada output.c
gcc -o output output.c runtime/strada_runtime.c -Iruntime -ldl -lm
```

**LTO (Link-Time Optimization)**: At `-O2` and above, the `strada` script automatically adds `-flto` to the gcc invocation, enabling cross-function inlining across the generated C code and the runtime. At `-O3` and `-Ofast`, `-march=native` is also added for CPU-specific tuning. This dramatically improves function call performance by allowing GCC to inline small runtime functions.

**LTO link cost and the GCC 15 cache (2026-11-19)**: an `-O2` link spends
1-3.5 s recompiling, at `-O2`, the 100-200 runtime functions the program
reaches; reading the runtime's bitcode takes 0.05 s. Flags don't change that
(partitioning, `-flto=1`, uncompressed bitcode and gold were all within a few
percent, and smaller partitions were slower), and splitting the runtime into
per-feature objects would not either, since LTO already discards unreachable
code cheaply. Linking `-O2` against the runtime's native code instead costs
~10% more instructions (median over the benchmarks; hotpaths 22%), so `-O2`
keeps LTO. With GCC 15+, the driver adds `-flto-incremental=DIR`
(`lto_incremental_flag` in tools/strada-driver.strada; GCC major version
parsed from the `--version` probe `cc_is_clang` already runs), so rebuilds
reuse unchanged LTRANS partitions. Cache: `$STRADA_LTO_CACHE_DIR`, default
`$XDG_CACHE_HOME/strada/lto` (~/.cache/strada/lto), created 0700;
`STRADA_LTO_CACHE=0` disables it. GCC 13 (this dev box) is unaffected, so
the GCC 15 path was tested with a fake compiler reporting version 15.

**`-fno-lto` at `-O0`/`-O1`/`-Os` (fast dev links, 2026-06)**: the precompiled `runtime.o` is built `-ffat-lto-objects` (native code **and** LTO bitcode) so `-O2+` user builds can cross-inline it. But that bitcode means the *linker* invokes the LTO plugin (`lto1`) even on a non-optimizing `-O0` build — ~0.55s of pure overhead. So at non-LTO opt levels the driver now passes `-fno-lto` explicitly, making the link use the runtime's native code instead: a small/dev build's link drops ~0.57s → ~0.03s (`strada -O0 hello`: 0.59s → 0.04s, competitive with `go build`). No effect on `-O2+` (which still LTO). The link, not stradac or even gcc's compile, was the dominant cost of small builds.

**`--tcc` (fast compile)**: Compiles the generated program with `tcc` (the Tiny C Compiler — ~10x faster than gcc, especially on large programs, at the cost of unoptimized output) and links the resulting object with `$CC`. Two constraints force this split: tcc can't parse the system headers the main runtime header pulls in (so the generated C's `#include` is swapped to the stripped `runtime/strada_runtime_tcc.h`, the same header the JIT uses), and tcc's own linker can't relocate the gcc-built runtime object — so `$CC` links the tcc-produced object against the full (gcc-built) runtime. Runtime features are fully preserved; only the program's own code is unoptimized. Good for edit/run iteration. Applies to executables and `--shared` (tcc compiles a PIC object, `$CC` links the .so — plus a `$CC`-compiled constructor shim, because tcc SILENTLY DROPS `__attribute__((constructor))` and the .so's `__strada_init_globals` would otherwise never run at dlopen: globals undef, OOP dispatch broken, build still succeeds; regression test `t/tcc_shared_test/run.sh`); `--object`/`--static-lib`/`--static` ignore it with a warning and compile with `$CC` (their outputs must match the gcc-built runtime ABI / bundle the runtime from source). `runtime/strada_runtime_tcc.h` must stay struct-for-struct in sync with `strada_runtime.h` (it shares the runtime ABI at run time).

## Architecture

### Dual Compiler System

```
Bootstrap Compiler (C)     →  bootstrap/stradac  (FROZEN - do not modify)
        ↓ compiles
Self-Hosting Compiler      →  ./stradac          (PRIMARY - all development here)
        ↓ compiles
Your Strada Programs
```

**Critical**: The bootstrap compiler (`bootstrap/`) exists only to compile the self-hosting compiler. All new features and bug fixes go in `compiler/*.strada`.

**Bootstrap memory fix (2026-11-18)**: the stage-1 compiler (the bootstrap's C
for the compiler, built with tcc) leaked so much that compiling
`Combined.strada` peaked near 5 GB; Claude Code's low-memory reaper killed
background builds. Its peak is now 0.77 GB (a full `make` peaks at 0.76 GB);
the self-built `./stradac` needs 0.74 GB. Stage 1 produces a byte-identical `Combined.c`. Changes in
`bootstrap/codegen.c` (ownership fixes only, no language changes):
- Every declared local owns its value: a borrowed initializer is increfed
  (`gen_decl_init`), and untracked declarations are recorded (`is_struct ==
  2`) so the innermost one decides `scope_is_tracked`.
- `$x = v` on a tracked local releases the old value.
- Owned values go into hashes with `strada_hash_set_take`.
- `foreach` bodies get their own scope, and a fresh list is released after
  the loop.
- `keys`/`values` of `%{$r}` read the hash in place, without copying it.
- `expr_is_owned` knows `%{}`/`@{}` copies and the builtins that return fresh
  values.
- String-builtin arguments go through `gen_str_arg`, which uses the runtime's
  `strada_to_str_take` for owned temporaries.

Making every local own its value was required: once stores stopped leaking,
borrowed aliases (`my $old = $h{k}; $h{k} = ...`) would otherwise dangle.

Second round (same day):
- Parameters own their values: non-main functions incref each parameter on entry and track it. Returns hand the caller a reference (`strada_incref` for a borrowed return value), and a non-void function falls through to `return strada_new_undef()`.
- `expr_is_owned` counts calls to this program's non-void functions, which excludes names in `bootstrap_builtin_names[]`. It also counts `sys::`/`core::` builtins and a longer list of fresh-value builtins.
- An expression statement whose owned call result is unused is released.
- `next`/`last` release the loop body's tracked locals (`emit_loop_exit_cleanup`). An early `return` also releases active foreach variables and owned foreach lists.
- `{ }`/`[ ]` literals release their owned values after construction.
- Conversions release owned arguments: `gen_str_arg` (`strada_to_str_take`) and `gen_conv_arg` (`strada_to_int`/`_num`/`_bool`).
- Each owned computed argument of a builtin is evaluated into a temporary and released after the call. This wrapper is at the top of `NODE_CALL` in `gen_expression`.
- `sb_append` releases an owned text argument.
- Void builtins (`delete`, `sb_*`) yield `strada_undef_static()`.
- About 0.6 MB still leaks per Lexer.strada compile, in small AST-walker sites.

### The `strada` Driver: Thin Shim + Compiled Driver (2026-06)

The `strada` command is a **~15-line bash shim** plus a **compiled Strada
program**, `tools/strada-driver.strada` → the `strada-driver` binary. The
shim does only what bash does well — find its own directory, source
`config.sh`, set the env contract — then `exec`s the driver. Everything else
(arg parsing, the preprocessor/compiler/linker orchestration, `--shared` /
`--object` / `--static-lib` / `--static` / `--tcc` paths, module-cache
warming, `-M`, `--repl`/`--script`/`--doc` hand-off) is Strada.

- **Env contract** (shim → driver): `STRADA_HOME` (lib/home root — `runtime/`,
  `lib/`, `stradapp`, `config.sh`), `STRADA_BIN` (binaries — `stradac`,
  `strada-jit`, `stradadoc`, `strada-interp`), `STRADA_SHIM` (the shim's own
  path, for sub-builds: `-M` directory walk, module-cache warmer). Dev tree:
  HOME = BIN = repo root. Installed: HOME = `<prefix>/lib/strada`, BIN =
  `<prefix>/bin` (the `install` rule patches the shim's two default lines).
- **Subprocess calls use `core::system_argv` / argv ARRAYS, not shell strings**
  — no quoting hazards. `system_argv` returns `WEXITSTATUS` directly (`== 0`
  is success; unlike `core::system`, which returns the raw waitpid status
  needing `>> 8`). The repl/script/doc/run-after "exec" hand-offs use
  system_argv + `core::exit($rc)` (fork+wait, behaviorally identical to exec
  for a one-shot CLI; `exec_argv` isn't in the Semantic builtin table).
- **A few probes are still `/bin/sh` strings** (`core::qx`/`core::system`): the
  `$CC --version` detection and the `link_lib` availability check. These
  interpolate `@CC` (from the `CC` env var). Two guards keep that injection-safe
  (CWE-78): (1) `setup()` rejects any `CC` token containing a shell
  metacharacter (`has_shell_meta`: `; | & $ ` ` ( ) < > \n \r`) — a real
  compiler command never has these; and (2) the probe sinks `sq()`-quote every
  token (`join(" ", map { sq($_) } @CC)`). The actual compile/link uses argv
  (`system_argv`), not these strings. When adding a probe, prefer argv; if a
  shell string is unavoidable, `sq()` every interpolated value. Regression:
  `t/codegen_escape_test/run.sh`.
- **Build** (`make strada-driver`): built directly with `stradac` + `$(CC)`,
  NOT via the `strada` shim (which would be circular — the shim execs this
  binary). The driver has no `use`/`-D`, so emit-C-then-link suffices. It is
  a dependency of `all`. `strada-driver` is gitignored (a build artifact).
- **Self-reference gotcha**: the driver greps the generated C for link
  markers (`__STRADA_OBJECT_FILES__:` etc.). Its OWN source mentions those
  tokens, so they're assembled from a variable tail (`"__STRADA_OBJECT_FILES__" . $COLON`)
  to keep the contiguous token out of its generated C — otherwise the
  compiling driver mis-greps its own string literals.
- **Strada-isms the rewrite surfaced — now FIXED** (list-flatten + `our`
  codegen, see CLAUDE_DEEP_DIVE.md "Perl-style list flattening"): the driver
  originally worked around three compiler
  bugs that have since been fixed: (1) `push`/element-set to an `our` array/
  hash from a function not persisting (an init-form bug — the container was
  mis-created); (2) literal sublists `(@g, ("a","b"))` not flattening; (3)
  array-returning calls `(@g, f())` not flattening. The driver's manual
  `@g = (@g, x)` accumulation style still works and is harmless. Spread
  `...@arr` works only in function-CALL arguments, not list literals.

The original ~1400-line bash wrapper lives in git history before this change.

### Self-Hosting Compiler Source (`compiler/`)

| File | Purpose |
|------|---------|
| AST.strada | AST node definitions |
| Lexer.strada | Tokenizer (note: `$q`, `$qq`, `$qw` are valid variable names, not quote operators after sigils) |
| Parser.strada | Recursive descent parser |
| CodeGen.strada | C code generator |
| Main.strada | Entry point |

These files are concatenated into `Combined.strada` during build, compiled to `Combined.c` by the bootstrap compiler, then linked with the runtime to produce `./stradac`.

### Runtime (`runtime/`)

- `strada_runtime.c/h` - C runtime library providing StradaValue type, reference counting, built-in functions
- All Strada programs link against this runtime

### Compilation Pipeline

1. **Lexer**: Tokenizes source code
2. **Parser**: Builds AST via recursive descent with operator precedence climbing
3. **CodeGen**: Transforms AST to C code using StradaValue structures
4. **gcc**: Compiles generated C to native executable

## Language Essentials

### Type System

- Scalar types: `int`, `num`, `str`, `scalar` (generic)
- Composite types: `array`, `hash`
- Special: `void`, `undef`, `dynamic` (context-sensitive return)
- C interop: `int8`, `int16`, `uint8`/`byte`, `uint16`, `uint32`, `uint64`, `size_t`, `char`, `float`, `double`
- `int`-declared variables hold canonical integers: initializing or assigning a value the compiler can't prove int-typed coerces it (`strada_to_int` semantics — `my int $x = "12abc"` makes $x the integer 12), and `int`-returning functions coerce non-int return values the same way. `int` parameters are converted once at entry (`strada_int_param_coerce`: a
tag check, so integers pass through for free; VM: `OP_CAST_INT` in the
prologue), in named functions and closures -- a function that needs the
caller's text unconverted must declare the parameter `scalar` (the parser's
`ast_new_int_literal` does, for `0x...` literal text).
- Type annotations are optional in `my`/`our` declarations and function signatures. The sigil determines the default: `$` → `scalar`, `@` → `array`, `%` → `hash`. Return types default to `scalar` if omitted.
- **`use strict "types";`** (2026-11-23/24): from that line to the end of the file, the stage-0 mismatches below are compile ERRORS, and `num` -> `int` needs `int(...)` (`sem_types_ok`). The parser stamps `strict_types` on every function (methods, `private`/`async`/`fn`, `extern func` bodies, `before`/`after`/`around` modifiers), BEGIN/END block and `my`/`our` declaration; Semantic switches per function (`sem_set_strict`: ctx `strict_err`, and `strict_types` = pragma or CLI flag) and reports through `sem_type_diag`. Element types: `array<T>`, `hash<T>`, `hash<str, T>` with T = `int`/`num`/`str`/`scalar`/class (`parse_elem_types`; `<int>` without spaces arrives as one DIAMOND token, and the lexer treats `%` after `hash<T>` as a sigil). Declarations, parameters and return types carry `elem_type`/`elem_class`; the scope entry carries `elem_type`. Checked: list initializers, `$a[i] = v`, `$h{k} = v`, `@a = LIST`, `%h = (k => v)` and hash literal initializers (`sem_check_hash_literal`; the pair list arrives as a flat NODE_ANON_ARRAY with `is_list` 0), `push`/`unshift`, foreach variable type, array arguments, and `our` scalar initializers inside functions; element reads and `shift`/`pop` have the element type (`stage0_expr_type`). Compile-time only: storage and generated C are unchanged (without the pragma the generated C of every example is byte-identical). strada-interp links Semantic.strada and runs `semantic_analyze($ast, 0, 2)` on a second parse of a program FILE that has a strict function (`vm_program_has_strict`). Mode 2 is types-only (ctx `types_only`): `semantic_error` drops everything not raised through `sem_type_diag` and warnings are silent, because the VM accepts programs the compiler rejects (unqualified cross-package calls, short argument lists). `-e` code and the REPL are not checked. Typing details that avoid false errors: `%` is always `int`; a STR literal with `has_char_code` (one single-quoted character) types as `scalar`, since it is a character code where an int is wanted; a hash pair list is checked only when every element is a plain scalar expression (`(%base, k => 1)` shifts which positions are values). Found by adding the pragma to all 287 examples/lib files and the 63 VM tests: do that again after changing `stage0_expr_type`. Not done: nested element types, method calls on objects whose class is not known (below). **Class element types (2026-11-25, third change)**: `array<Dog>` / `hash<str, Dog>` are checked. Semantic carries a class element type as `1000 + N` (`sem_node_et`, `sem_et_class`, `sem_et_plain`; every read of a node's `elem_type` goes through `sem_node_et`). `sem_et_compat` compares container element types (two classes must be related in either direction; no int/num/str into a class container; a class into a plain container is not flagged); a single value is checked through `sem_value_class` (exact class: must be the class or a descendant). Classes are compared only when both are in `known_packages` and no function calls `inherit()` (`sem_classes_static`). Two classes are related when one inherits from the other or some class inherits from both (`sem_classes_related`). In a program with any `use overload`, `stage0_expr_type` types an operator with a scalar-typed operand as scalar (`sem_prog_has_overloads`): the result is whatever the overload returns (`my int $n = $vec . "x"` was a false error). `sem_recv_class` gives a class to `$a[i]`, `$h{k}`, `shift`/`pop` on a class container and to `Pkg::new(...)`; `my $v = EXPR` and foreach variables record it, so method calls on elements are checked. No corpus file uses a class element type, so the stamped sweep does not cover this: the tests are `ce*.strada` in `t/strict_types_test/run.sh`. **`our`/file-level/returned containers (2026-11-25, second change)**: `parse_our_decl` stamps `elem_type`; `sem_name_elem_type` reads a file-level variable's element type from the declaration the reference binds to (`resolve_global_decl_node`), never from the root scope entry, which is one per bare name for every package, sigil and redeclaration (`sem_scalar_decl_type` does the same for `$x` next to `@x`: that was a false error); function-level `our`, the file-level initializer loop and `local @x = LIST` check `@`/`%` initializers; a `return` in a function with `return_elem_type` is checked through `sem_check_elem_value` / `sem_check_hash_literal` with ctx `elem_where` naming the function's result; `sem_src_elem_type` gives the element type of a whole-container source (a variable, or a call to a function whose return type carries one) for initializers, `=`, `push`, foreach and array arguments. `analyze_expression` records the function-table key a call resolved to on the node (`sem_fn_key`, `sem_fn_pkg`); `sem_call_fn_info` uses it (with a by-name fallback for checks that run before the arguments are analyzed, like `sem_check_typed_push`), so `stage0_expr_type` now types unqualified-in-package, `Pkg::f()` and `::f()` calls (before, only a node name equal to the table key). That widening found three real `num`-into-`int` initializers in the sweep (lib/Forma, lib/Template/Plugins; those files have no pragma). A function-level `our @x;` without an element type stays unchecked. **Method calls (2026-11-25)**: `sem_recv_class` gives a receiver's class for `$self` (parameter 0 named `self` in a non-main package; may be a subclass), a variable declared `my $v = Pkg::new(...)` (`sem_ctor_class`: generated constructor, no constructor, or `fn_ctor_own_class`, which now lives in Semantic.strada), a typed catch variable (may be a subclass), a class-name string (`Pkg->m()`); the class is recorded on the scope entry (`obj_class`/`obj_exact`, `sem_note_obj_class`, only inside a function) and holds only if the function never writes the variable again (`sem_assigned_walk`, run at the top of `analyze_function` before analysis renames nodes: `=`, `++`, `\$v`, list assignment, foreach over an existing variable, any `__C__` block; by name). `sem_method_target` resolves the method along the MRO (`collect_mro_dfs` over the program's `inherits`, which includes roles) and gives up when the method has an `around` modifier or, for a may-be-subclass receiver, when any descendant resolves the name to another function. `sem_check_method_args` checks arguments against parameters 1.. (only a spread moves positions: method arguments are not flattened), `SUPER::m()` resolves from the current package's parents, `stage0_expr_type` gives a method call its declared return type (void counts as scalar), and `sem_check_ctor_args` checks `Pkg::new(k => v)` against `has_attrs` types along the MRO for generated constructors. `sem_node_one_value` is the shared "stands for exactly one value" test (`%{$r}` / `@{$r}` are NODE_DEREF_SCALAR with a sigil; `$r->{k}` is NODE_DEREF_HASH); the hash-literal check uses it too (`(%$base, k => 1)` was a false error). Method-call nodes carry no line, so diagnostics fall back to `cur_stmt_line`. Unchecked: objects from parameters, calls, containers, file-level variables, `Pkg->new`, runtime `inherit()`. Sweep result: 661 typed-parameter checks and 188 attribute checks on outside projects (cannoli, s4, evilcrm, ...) with no new errors; in-repo 345/86 with only the deliberate mismatches of `t/bugfix_tests/typed_coercions_r20.strada`. The tree-sitter grammar knows `use strict [STRING];` and `type_arguments` (`array<int>`, `hash<str, T>`); `src/parser.c` was regenerated with tree-sitter-cli 0.27.2 (ABI 15) and the corpus sweep is clean. Bare `use strict;` is accepted as a no-op (the user's decision, 2026-11-24: "strada is strict by default"); it does not enable type checking. Example: `examples/checked_types.strada` (registered in `t/t_core.sh`; `t/strict_types_test/run.sh` uncomments each of its "would not compile" lines and requires an error on that line). The fast paths for accessors apply only where the call is inlined: `$self->x()` in the class's methods, reads on a variable in `known_types`, setters in the class's own package. Test: `t/strict_types_test/run.sh`.
- **`--strict-types`** (stage-0 gradual checking, warning-only): compares declared types against a best-effort static expression type (`stage0_expr_type` in Semantic.strada) at four sites — `my T $x = …` initializers, plain `=` assignments to declared scalars, call arguments vs declared params, and `return` vs declared return type. `scalar`/`dynamic`/unannotated are **bivariant** (compatible with everything, both directions) so untyped code never warns; the int/num/C-interop numeric family interconverts silently (documented coercions). Warnings, never errors — the runtime remains the soundness backstop. A `void` result used as a value warns even for untyped targets (it's a hard C error downstream).

### Sigils

- `$` - scalar variables (also for element access: `$arr[0]`, `$hash{"key"}`)
- `@` - arrays (whole-array operations only)
- `%` - hashes (whole-hash operations only)

Optional type examples:

```strada
my $x = 42;            # equivalent to: my scalar $x = 42
my @arr = (1, 2, 3);   # equivalent to: my array @arr = (1, 2, 3)
my %h = ("a" => 1);    # equivalent to: my hash %h = ("a" => 1)
our $count = 0;         # equivalent to: our scalar $count = 0

# Function params and return types are also optional (default to scalar)
func add($a, $b) { return $a + $b; }        # params default to scalar
func greet($name) { return "Hi, " . $name; } # return type defaults to scalar
my $fn = fn ($x) { return $x * 2; };        # anonymous functions too

# No-parens form: implicit @_ parameter (like Perl subs)
func process { say($_[0] . ": " . $_[1]); }  # args in @_, accessed via $_[N]
func total { my $s = 0; my $i = 0; while ($i < size(@_)) { $s = $s + $_[$i]; $i = $i + 1; } return $s; }
```

Scalar context and list flattening:

```strada
$n = @arr              # scalar context: returns array length
my @flat = (1, @arr, 2)  # @arr elements are flattened into the list
```

### References and Anonymous Constructors

```strada
\$var, \@arr, \%hash     # References
[ 1, 2, 3 ]              # Array reference
{ key => "value" }       # Hash reference
$$ref, @$ref, %$ref      # Dereference (shorthand)
${$ref}, @{$ref}, %{$ref}  # Dereference (braced)
```

### Weak References

Break circular reference cycles with `core::weaken($ref)`. A weak reference does not prevent its target from being freed. Check with `core::isweak($ref)`. When the target is freed, dereferencing returns undef.

Note: the automatic **cycle collector** (default on, see Memory Management below) reclaims most indirect cycles without `weaken()`. `weaken()` is still useful for closure-capture cycles the collector doesn't cover, for deterministic teardown, or when the collector is configured out.

## Development Workflow

### Adding New Language Features

1. Modify `compiler/Lexer.strada` for new tokens
2. Modify `compiler/Parser.strada` for parsing rules
3. Modify `compiler/AST.strada` for new AST nodes
4. Modify `compiler/CodeGen.strada` for C code generation
5. Update `runtime/strada_runtime.c` if new runtime functions needed
6. Add example in `examples/` to test
7. Rebuild with `make` and test with `make run PROG=your_example`

### Testing Changes

```bash
make clean && make          # Full rebuild
make run PROG=test_simple   # Test specific example
make test-selfhost          # Verify compiler can compile itself
make test-suite             # Run comprehensive test suite (269 tests)
./t/run_tests.sh -v string  # Run tests matching "string" with verbose output
```

## Test Suite

Located in `t/`:

```bash
./t/run_tests.sh            # Run all 269 tests
./t/run_tests.sh -v         # Verbose output
./t/run_tests.sh -t         # TAP format for CI/CD
./t/run_tests.sh regex      # Run only tests matching "regex"
```

Test types: `test_compile`, `test_run`, `test_output`, `test_output_contains`, `test_exit_code`

## Memory Management and Leak Detection

Strada uses reference counting with **tagged integer optimization**, plus two opt-in (default-on, compile-time) features: an automatic **cycle collector** and a **request arena**. Use valgrind to detect leaks:

```bash
valgrind --leak-check=full ./my_program 2>&1 | grep -A5 "LEAK SUMMARY"
```

### Cycle Collector (`STRADA_CYCLE_GC`, default on)

A Bacon–Rajan synchronous trial-deletion collector that automatically reclaims **indirect** reference cycles (`A→B→A`) which plain refcounting leaks — so `core::weaken()` is no longer required for the common hashref/arrayref/object-graph cycle. Implemented entirely in `runtime/strada_runtime.c` under `#ifdef STRADA_CYCLE_GC`; per-value state is three spare `flags` bits (colour) plus, while a value is buffered, its root-buffer index in `struct_size` (no side table, no struct size change). Key properties:

- **Hook**: only the survive path (`new_rc > 0`) of `strada_decref` buffers a candidate root; drop-to-zero is unchanged. Buffered roots are **pinned** (incref'd) so they can't be freed underneath the collector.
- **Scope**: covers `ARRAY`/`HASH`/`REF` roots and traverses closure captures and cells. Futures/channels and tied containers fall back to `core::weaken()`.
- **Threading**: works under threads via a **stop-the-world** pause. Mutators poll a safepoint in the threaded `strada_decref` path (after buffering, never before — parking before the buffer read would let the collector free the candidate underneath it); threads blocked in a syscall (sleep/recv/await/join/pool-idle) mark themselves safe via `cc_blocking_enter/leave` so the collector needn't wait for them. The collector requests a stop, waits (bounded by a 200ms timeout → a thread that can't reach a safepoint just causes a *skipped* collection, never a hang) for every other registered mutator to park/block, then runs the trial-deletion with the graph stable. Single-threaded keeps the original lock-free fast path. The candidate buffer + side table are mutex-protected only while threading is active.
- **Triggers**: automatically at a candidate-count threshold (default 10000, **adaptive**: a collection that frees nothing doubles the trigger, capped at ~1M candidates; one that frees cycles resets it to the base — so cycle-free programs converge to near-zero collection work) and a final sweep at exit; or explicitly via the API below. `core::gc_threshold($n)` sets the base and resets the current trigger.
- **DESTROY**: a cyclic blessed object reclaimed by the collector runs `DESTROY` (deferred until the cycle's internal refcounts are restored, `cc_def_dsv`). Closure-capture cycles (`$self->{cb} = func () { $self }`) are traversed too (`cc_each_child` STRADA_CLOSURE case) and reclaimed. Regression: `t/bugfix_tests/gc_destroy_closure_r20.strada`.
- Build `./configure --without-cycle-gc` to compile it out.

API (Strada): `core::gc_collect()`, `core::gc_enable()`, `core::gc_disable()`, `core::gc_threshold($n)`, `core::gc_collections()`, `core::gc_freed()`. Runtime C: `strada_gc_collect()`, `strada_gc_set_enabled()`, `strada_gc_set_threshold()`, `strada_gc_collections()`, `strada_gc_objects_freed()`.

### Request Arena (`STRADA_ARENA`, default on)

A bump allocator for request-scoped `StradaValue` structs. `core::arena_begin()` / `core::arena_end()` wrap a unit of request-local work; every value allocated in between is bump-allocated and reclaimed wholesale at `arena_end` (struct + backbones), skipping per-object malloc/free and refcount teardown. Implemented under `#ifdef STRADA_ARENA`; dormant until `arena_begin()` (safe to ship enabled). Detection of arena pointers (`ARENA_OWNS`) makes `strada_free_value` and the cycle collector no-op on arena values.

- **Escape safety (2026-06)**: `arena_end()` no longer `free()`s the arena
  blocks — it **neutralizes** every SV (immortal refcount sentinel + `UNDEF`
  type) and **retains the blocks on a reuse free-list** (recycled by the next
  `arena_begin`). So a value still referenced by a scoped variable past
  `arena_end` (which is the NORMAL case: `my $t = build(); …; arena_end()`
  leaves `$t` live until its scope-exit decref) is safe — its decref /
  `break_self_cycle` no-op on the immortal/inert SV instead of touching freed
  memory. `break_self_cycle` (header inline + `_impl`) gained the immortal
  short-circuit this requires. Nested containers and arrays no longer crash.
  Using `arena_begin/end` around short-lived request data is now safe with
  ordinary variables.
- **O(1) ownership (2026-06)**: `arena_owns()` (consulted on every container
  decref/free while an arena is open) is now a single **range check**, not an
  O(blocks) scan. All `ArenaBlock`s are carved from one `mmap`'d region (1 GiB
  virtual, lazily paged via `MAP_NORESERVE`); a pointer in `[region,
  high-water)` is necessarily in some carved block (the region only holds arena
  blocks), so the check is O(1) with no false positives — and **no per-value
  flag** (the `StradaValue` struct has no free byte, and `struct_size`/
  `refcount`/`meta` are clobbered per-type by constructors, so the once-mooted
  "struct_size flag bit" can't work universally). The freelist still recycles
  blocks within the region, so the high-water mark settles at peak arena size.
  Fallback: if `mmap` is unavailable or the region fills, blocks are `malloc`'d
  outside it and an `arena_overflow` flag re-enables the (rare) scan for just
  those. This removes the old "one huge arena is slower than no arena" cliff —
  e.g. wrapping all of `benchmarks/bench_binary_trees.strada` in a single arena
  is now the *fastest* config (0.807s), beating the per-short-lived-tree arena
  it currently uses. So the arena is now a viable bulk-free nursery for large
  request-scoped allocations, not just many small scopes.
- **Remaining caveats**: an arena container referencing a *persistent* value
  still leaks one ref on it; `DESTROY` is not run for arena objects;
  single-threaded scope only (fits a preforking worker). Don't *use* (read) an
  escaped arena value after `arena_end` — only cleanup of it is made safe.
- Build `./configure --without-arena` to compile it out.

API (Strada): `core::arena_begin()`, `core::arena_end()`, `core::arena_active()`. Runtime C: `strada_arena_begin()`, `strada_arena_end()`, `strada_arena_active()`.

**Note**: both features are compiled into the precompiled runtime (`runtime/strada_runtime.o`). After `./configure` changes their state, run `make clean && make` (the runtime object is not auto-rebuilt on a config change).

### Tagged Integers (Pointer Tagging)

Integers are encoded directly in `StradaValue*` pointers — no heap allocation:

```
bit 0 = 1 → tagged integer, bits 1-63 = sign-extended value
bit 0 = 0 → normal heap pointer (always aligned to ≥ 2 bytes)
Range: -(2^62) to (2^62-1)
```

Macros (defined in `strada_runtime.h` and `strada_runtime_tcc.h`):
- `STRADA_IS_TAGGED_INT(sv)` — check if pointer is a tagged integer
- `STRADA_TAGGED_INT_VAL(sv)` — extract integer value
- `STRADA_MAKE_TAGGED_INT(val)` — create tagged integer

Key rules:
- **CRITICAL**: Every function that accesses `sv->type`, `sv->value`, `sv->meta`, or `sv->refcount` MUST check `STRADA_IS_TAGGED_INT(sv)` first — tagged ints are not valid heap pointers
- Tagged ints are **immortal**: `strada_incref`/`strada_decref` are no-ops
- `strada_to_int()`, `strada_to_num()`, `strada_to_str()` all handle tagged ints transparently
- `strada_ref_create()` **unboxes** tagged ints to heap `STRADA_INT` values — reference targets (`rv`) must always be heap pointers
- Supersedes the old small int pool (-1..255)

### Common Leak Patterns in `__C__` Blocks

```strada
__C__ {
    char *str = strada_to_str(my_str);  // ALLOCATES - must free!
    use_string(str);
    free(str);

    strada_decref(ptr);  // Free old value before reassigning!
    ptr = strada_new_int((int64_t)new_value);
}
```

- `strada_to_int()`, `strada_to_num()` - Return plain values, no free needed. Handle tagged ints transparently.
- `strada_to_str()` - Returns allocated string, MUST free. Handles tagged ints transparently.
- **Important**: `StradaValue*` variables may be tagged integers (odd pointers). Never access `->type` or `->value` directly — always use `strada_to_int()`, `strada_to_str()`, etc.

### Compiler-Level Leak Tracking

The code generator tracks whether expressions need cleanup via `needs_temp_cleanup()` in `compiler/CodeGen.strada`:
- Returns 1 for expressions that create owned StradaValue* (need cleanup)
- Returns 0 for borrowed references (array subscript access, variables)
- **Hash access** (`NODE_HASH_ACCESS`, `NODE_DEREF_HASH`) returns **owned refs** via `strada_hv_fetch_owned()` — `needs_temp_cleanup` returns 1 for these

The code generator also uses `expr_is_int_typed()` to determine if an expression is statically known to be int-typed (int literals, int-declared variables, int accessor calls, or binary ops where both sides are int-typed). This enables int-specific optimizations: tagged int arithmetic (`STRADA_MAKE_TAGGED_INT` instead of `strada_new_num`), int compound assignment (`+=`/`-=`), and int comparisons in conditions (`strada_to_int` instead of `strada_to_num`).

**`int_vars`/`num_vars` are block-scoped** (saved/restored in `gen_block`): they are keyed by C name, so without scoping a nested or sibling block's `my int $result` leaked onto a different `my str $result` binding and poisoned later assignments with int coercion (`strada_to_int("text")` = 0 — root cause of the tree-walk s/// handler returning "0" in giant functions like `eval_node`). Any new code that registers per-variable type facts must follow the same block discipline.

### Adding New core:: Functions

When adding new `core::` functions to the compiler:

1. **Temp argument cleanup**: If the function takes StradaValue* args that may be temps, wrap in statement expression to capture and cleanup
2. **Add to `needs_temp_cleanup()`**: If function returns an owned StradaValue*, add its name to the list (~line 2038 in CodeGen.strada). Names use `sys::` prefix (since `core::` is normalized to `sys::` before the check).
3. **In runtime C implementation**: If it uses `strada_to_str()`, remember to `free()` the result
4. **Tagged int guards**: If the function accesses `sv->type`, `sv->value`, `sv->meta`, or `sv->refcount` on ANY parameter, add `STRADA_IS_TAGGED_INT(sv)` check before the access

### CodeGen Leak Prevention Checklist

When adding **inline codegen** for a new built-in function in `compiler/CodeGen.strada`:

1. **CRITICAL: Always check `needs_temp_cleanup()` on ALL arguments.** Hash access (`$hash{"key"}`, `$hash->{"key"}`) returns owned refs. If you embed `gen_expression($cg, $arg)` directly inside a C function call without cleanup, it will leak when the argument is a hash access. This is the **#1 source of leaks** in the codegen.

   **Required pattern for EVERY built-in function argument:**
   ```strada
   if ($cg->{"cleanup_enabled"} == 1 && needs_temp_cleanup($cg, $arg) == 1) {
       emit($cg, "({ StradaValue *__tmp = ");
       gen_expression($cg, $arg);
       emit($cg, "; RESULT_TYPE __res = c_function(__tmp); strada_decref(__tmp); __res; })");
   } else {
       emit($cg, "c_function(");
       gen_expression($cg, $arg);
       emit($cg, ")");
   }
   ```

2. **Never nest `strada_to_str()` inside function args.** The returned `char*` must be captured, used, then `free()`'d:
   ```
   # BAD:  emit($cg, "some_func(strada_to_str(ARG))");
   # GOOD: emit($cg, "({ StradaValue *__v = ARG; char *__s = strada_to_str(__v); TYPE __r = some_func(__s); free(__s); __r; })");
   ```

3. **Use `_take` variants for container ops in runtime C code.** When creating a new StradaValue and immediately pushing/storing it:
   ```c
   // BAD:  strada_hash_set(hv, key, strada_new_str("val"));    // leaks (refcount 2, freed to 1)
   // GOOD: strada_hash_set_take(hv, key, strada_new_str("val")); // takes ownership (refcount stays 1)
   // Same for: strada_array_push_take() instead of strada_array_push()
   ```

4. **Concatenation chains must clean up intermediates.** Don't use nested `strada_concat_sv()` chains — intermediates leak. Use sequential concat with `strada_concat_inplace()` (which decrefs its first arg) and capture literal parts for cleanup.

5. **Void-returning functions cannot use `StradaValue *__r = func(...)`.** If a runtime function returns void (e.g., `strada_spew`), use `func(...); (StradaValue*)NULL;` in statement expressions.

6. **Also check sub-expressions that receive hash access results.** These include:
   - Closure callee: `$hash{"fn"}->(args)` — capture callee, call, decref
   - Method call object: `$hash{"obj"}->method()` — capture object, call, decref
   - Array mutation targets: `push($hash{"arr"}, ...)` — capture array arg, operate, decref
   - Chained derefs: `$hash{"ref"}->{"key"}` — capture intermediate ref, deref, decref
   - StringBuilder args: `sb_append($hash{"sb"}, ...)` — capture SB, operate, decref

7. **Always run leak tests** after adding inline codegen: `./t/leak_tests/run_leak_tests.sh`

8. **CRITICAL — name-resolution must be UNIFIED across the codegen.** Any predicate that consults `$cg->{"functions"}` (or any other compile-time symbol table) **must use the same name-mangling and fallback rules as the call-emission path**. Otherwise the two diverge silently: emission works (it falls back to the package-prefixed name) but the predicate returns the wrong answer, and the cleanup wrapper around the call is skipped — every owned return from same-package unqualified calls in non-`main` packages leaks.

   This was the root cause of an enormous per-statement leak in perla (commit `b5f48978`): `needs_temp_cleanup()` did a plain `$cg->{"functions"}->{$c_name}` lookup, while the call emitter (`gen_call`, around line 12518) tries `$c_name` first AND falls back to `<c_pkg>_<c_name>` for unqualified calls inside non-`main` packages. The predicate missed the fallback, returned 0 (no cleanup needed), so `push(@arr, fn(...))` emitted `strada_array_push(av, fn(...))` without the `__push_v` / `strada_decref` wrapper — every owned return leaked. Each `my $X = N;` in a compiled Perl program leaked ~74 blocks before the fix; 0 after.

   **Rule of thumb:** every predicate that maps a function-call AST node to a behavior (cleanup needed? incref needed? owned-return type?) must call the same name-resolver as the emitter, or factor that resolver into a shared helper. Test in a **non-`main` package**, not just in `main` — `main` doesn't exercise the mangle/fallback path. Pure-Strada test cases living in `package main` (or using fully namespaced calls) never reproduce this class of bug.

### Cleanup stack and balance checking (2026-11)

The pending-cleanup stack (owned temps / variable slots released when an
exception unwinds) starts in a 256-entry per-thread inline buffer and grows on
the heap (`strada_pc_reserve`); it used to be a fixed 16384-entry array that
silently dropped pushes past the cap. `strada --check-cleanup` (or
`STRADA_CHECK_CLEANUP=1` in the environment, e.g. for a whole test-suite run)
compiles with full frame tracking and `-DSTRADA_CLEANUP_CHECK`: each frame
records the cleanup depth at entry and a normal return with a different depth
aborts with `cleanup stack imbalance in FUNC()` (build with
`--check-cleanup=warn` to report without aborting). Run it after touching codegen push/pop logic.

Codegen rule: an owned operand that stays in a C local while another operand
is evaluated must be on the cleanup stack -- `emit_arg_hold` /
`emit_arg_release`, or `cleanup_pd_str($pushed, "VAR")` for hand-written
release strings (CodeGenBuiltins). Borrowed holds (a variable's current value,
a container) must NOT be pushed.

Hash memory: insert/delete churn rehashes in place (no index doubling); a
mostly-empty hash shrinks its index after deletes and an emptied hash shrinks
its entries array on the next insert.
Small hashes live in one block (header, index and entries together). Only
`compact_alloc` (set by `strada_hash_new_presized`'s single-block path, kept by
the small-hash pool) says the header owns that inline storage:
`hv_idx_in_block`/`hv_ent_in_block` check it before comparing addresses,
because a separately allocated index or entries array may sit right after
the header with some allocators (2026-11-21).

### Leak Test Suite

Run leak tests with: `./t/leak_tests/run_leak_tests.sh` (117 tests, incl. `test_threads_cycles` — multithreaded cycle collection)

## IMPORTANT: Documentation Updates

**When making changes to Strada, ALWAYS update these documentation files:**

1. **`CLAUDE.md`** (this file)
2. **`CLAUDE_DEEP_DIVE.md`** - AST nodes, code gen patterns

## Claude skill (`claude/skills/strada`, 2026-11-19)

A skill that teaches Claude to write Strada: `SKILL.md` (workflow, Perl
pitfalls, `__C__` rules), `scripts/check-toolchain.sh`, `scripts/strada-run.sh`
(build to a temp dir, run, clean up; also a `prove --exec` runner). The
references are copied from `docs/STRADA_FOR_CLAUDE.md` (as
`reference/language.md`) and `docs/BUILTIN_FUNCTIONS.md` at package time, so
keep those two docs accurate: they are what Claude reads. `make claude-skill`
(`tools/build-claude-skill.sh`) builds `build/claude-skill/`: the folder, a zip
(claude.ai upload), a tarball (`~/.claude/skills/`), a Claude Code plugin and a
marketplace listing. `make claude-skill SITE=../strada-lang.github.io` installs
them into the website (`claude/`, `.claude-plugin/marketplace.json`, version
stamp in `docs/ai.html`); users then run
`/plugin marketplace add strada-lang/strada-lang.github.io`. Rebuild and
republish after doc or skill changes.

The skill uses the open Agent Skills format, so the same folder works in
OpenAI Codex (`~/.agents/skills/` or a repo's `.agents/skills/`). The site's
one page for all assistants is `docs/ai.html` (Claude Code, Codex, the Claude
apps, and other tools via `claude/strada/reference/language.md`, which is
served as raw Markdown because it has no front matter); `docs/claude.html` is
a redirect to it, kept for old links and installed plugins' homepage. Keep
SKILL.md free of assistant-specific instructions. `claude/install-skill.sh` is
published as `claude/install.sh` (`curl -fsSL .../claude/install.sh | sh`): it
downloads the tarball and installs it into `~/.claude/skills` (Claude Code,
which does not read `~/.agents/skills`) and `~/.agents/skills` (Codex);
`--claude`/`--codex` pick one, `--project` installs into the current
directory. It must keep working when piped into `sh` (no `$0`, POSIX only).

Publishing a new version:
1. Commit the strada-lang changes first. The skill's `VERSION` file records
   the HEAD commit at build time.
2. `make claude-skill SITE=../strada-lang.github.io`, then
   `claude plugin validate ../strada-lang.github.io` and
   `claude plugin validate build/claude-skill/plugins/strada`.
3. In the website repo, commit `claude/`, `.claude-plugin/` and
   `docs/ai.html`, and only those. The site often has unrelated uncommitted
   edits. Push only with the user's approval.
4. Check the live URLs once GitHub Pages has deployed (a minute or two):
   `docs/ai.html`, `claude/strada-skill.zip`, `claude/strada-skill.tar.gz`,
   `claude/strada/reference/language.md`.
   To test a real install without touching your own setup, run
   `CLAUDE_CONFIG_DIR=$(mktemp -d) claude plugin marketplace add strada-lang/strada-lang.github.io`,
   then `claude plugin install strada@strada-lang` with the same variable.

Gotchas:
- The version is the UTC build date (`2026.11.19`). Two releases on one day
  share a version, and `/plugin update` may not pick up the second one.
- GitHub Pages runs Jekyll, so `claude/strada/SKILL.md` is served as
  `SKILL.html` and the `.md` URL is a 404. Nothing links to it (the page
  links to the files on GitHub). Adding `.nojekyll` to the website repo
  would change how the whole site is built.
- Testing the skill: give a fresh agent only the built folder
  (`build/claude-skill/strada/`) and a few task descriptions, such as a CLI
  tool, a class with typed exceptions, a test file run under `prove`, and a
  `__C__` call into a C library. Fix whatever it had to guess.

## Key Documentation

- **`CLAUDE_DEEP_DIVE.md`** - AST nodes, codegen patterns, runtime internals (read this first!)
- `docs/LANGUAGE_GUIDE.md` - Language tutorial
- `docs/QUICK_REFERENCE.md` - Syntax cheat sheet
- `docs/DEBUGGING.md` - Using GDB with Strada programs
- `docs/COMPILER_ARCHITECTURE.md` - Compiler internals
- `docs/RUNTIME_API.md` - Runtime library reference

## Standard Library Additions (2026-06)

- **`lib/Test.strada`** — TAP-emitting test framework: `Test::ok/nok/is/isnt/is_num/like/unlike/pass/fail/skip/diag/plan`, `Test::done_testing()` returns the exit code. Runs under any TAP harness (`prove`).
- **`lib/List.strada`** — List::Util-style helpers: `reduce/any/all/none/first/sum/product/min/max/minstr/maxstr/uniq/zip/pairs`.
- **`lib/Exception.strada`** — chainable exceptions (see Error chaining above).
- **`lib/Async/Scope.strada`**, **`lib/Async/Actor.strada`** — structured concurrency + actors (see Async/Await).

**Perl-compatible modules (2026-11-20)**: `Getopt::Long` (rewritten to Perl's
API: `GetOptions("v+" => \$v, ...)`, hash storage, `Configure`,
`GetOptionsFromArray`; it changes `@ARGV` in place with splice/push, because
`@{core::argv()} = LIST` does not change `@ARGV` in compiled code), `File::Basename`,
`File::Temp`, `File::Spec` (class-method style) + `File::Spec::Functions`,
`File::Path`, `File::Copy`, `Cwd`, `Carp`, `Scalar::Util`. `make install`
installs everything under `lib/`. They follow Perl's behaviour, including
error messages; `t/perl_modules_test/run.sh` runs each program compiled and in
the VM against expected output that was checked against Perl 5.38. The old
Getopt::Long `GetOptions([SPECS], \%opts)` form still works; `GetOptions(@specs,
%opts)` does not (the hash arrives as a copy: use `GetOptions(\%opts, @specs)`).
Limits: no `qr//` (File::Basename takes regex strings); library functions
cannot provide `weaken` (arguments are copies), so Scalar::Util leaves it to
`core::weaken`; a method returning a list gives an array ref inside another
call's argument list (`join("/", File::Spec->splitdir($d))`).
`use Mod qw(NAME)` no longer emits an unused `extern StradaValue* NAME();` when
NAME resolves to the module's function (it clashed with libc's `basename`,
`getcwd`; test `t/bugfix_tests/qw_import_libc_name.strada`).

**System additions (2026-11-22)**: `core::environ()` (hash of the whole
environment), `core::uname()` (hash), `core::nproc()` (CPUs this process may
use: affinity mask on Linux), `core::socketpair()` (`[fd0, fd1]`, the shape of
`core::pipe()`), `core::mkfifo(path, mode)`, `core::rewinddir($dh)`,
`core::getservbyname(name, proto)` (port or undef), `core::crc32(data)`,
`core::uuid()` (version 4). All are plain runtime functions wired like
`initgroups` (Semantic `$b`, one `gen_call_with_arg_cleanup`/zero-arg `emit`
handler in CodeGenBuiltins, `owned_set`), so the VM reaches them through the
generated bridge table. HMAC-SHA256 was already in `Digest::SHA`
(`hmac_sha256`, `hmac_sha256_hex`; `t/t_hmac.sh`). Not added, on purpose:
Perl-named aliases `getc`/`read`/`sysread`/`syswrite` (the existing
`fgetc`/`fread`/`read_fd`/`write_fd` have different signatures and return
values, so an alias would be a Perl name with non-Perl behaviour) and
`binmode` (handles have no layers and `"rb"` changes nothing on POSIX, so it
would be a no-op). Test: `t/bugfix_tests/sys_info_posix_additions.strada`.

Build tools with: `make tools` or `./strada tools/<tool>.strada`

## Perla (Perl 5 Compiler)

Perla is a full Perl 5 compiler built on Strada. It converts Perl 5 source to native executables (via C) or runs it through the Strada VM. **Note: Perla lives in a separate repository — the `perla/` directory described below is NOT in this tree** (only its runtime support stubs `runtime/perla_dbi.c` / `runtime/perla_stash.c` exist here, untracked). The section is kept as architecture reference.

```bash
# Build Perla
cd perla && make

# Compile and run a Perl script (default: native C compilation)
./perla input.pl

# Compile to executable only
./perla input.pl -c

# Run via Strada VM/interpreter (no compilation needed)
./perla --vm input.pl

# Output Strada source code
./perla --strada input.pl
```

**Architecture:** Lexer → Parser → AST → CodeGen (C) or StradaGen (Strada bytecode)

**Components** (`perla/lib/Perla/`):

| File | Purpose |
|------|---------|
| `Lexer.strada` | Perl 5 tokenizer |
| `Parser.strada` | AST generation with lib path resolution |
| `AST.strada` | Node type definitions |
| `CodeGen.strada` | C code generation (compiled mode) |
| `StradaGen.strada` | Strada code generation (VM mode) |
| `XS.strada` | XS/C extension module support |

**Testing:**

```bash
./perla/t/run_tests.sh          # Run all 53 tests (compiled mode)
./perla/t/run_tests.sh --vm     # Run all 53 tests (VM mode)
```

**Supported Perl features:** OOP (bless, inheritance, method dispatch), closures with variable capture, regex (s///e, backreferences, quantifiers), string interpolation, file I/O, map/grep/sort, try/catch (eval/die), destructuring, XS modules, `format`/`write` (Perl text-template reports, including `$~`/`$^`/`$=`/`$%`/`$^A`, `formline`, per-handle formats), `tie`/`untie`/`tied` on scalars, hashes, arrays, and filehandles, typeglobs and aliasing (`*A = *B`, `*A = \$x`), `goto LABEL`, `state` variables, `local` on slices, full UTF-8 string semantics (SVf_UTF8 flag), Unicode normalization (Unicode::Normalize / utf8::), and Encode transcoding (Latin1/CP1252/ASCII).

## Interpreter / VM

The Strada interpreter (`strada-interp`) executes Strada programs without generating C code via a bytecode VM backend. Located in `interpreter/`.

```bash
# Build the interpreter
make interpreter

# Run a program
./interpreter/strada-interp program.strada

# REPL
./interpreter/strada-interp
```

**Architecture:** Source → Lexer → Parser → AST → Bytecode Compiler (`vm_compiler.c`) → VM Execution (`vm.c`)

**Key files:**

| File | Purpose |
|------|---------|
| `interpreter/vm.h` | Opcode definitions, VM structures (120+ opcodes) |
| `interpreter/vm.c` | VM execution engine |
| `interpreter/vm_compiler.c` | AST-to-bytecode compiler |
| `interpreter/Main.strada` | REPL and file execution entry point |

**Testing:** `interpreter/test/run_tests.sh` (61 tests, bytecode VM) and `run_compiler_tests.sh` (the compiler test suite executed through the VM — **at parity: 108 pass / 0 fail**, 12 skips). See `docs/INTERPRETER.md`.

**VM parity notes (2026-06):** the VM executes the full compiler suite. Key machinery: a **generic runtime bridge** (`interpreter/vm_generic_builtins.inc`, generated by `tools/gen_vm_generic_builtins.py`) exposes ~235 `sys::`/`math::` runtime functions through `vm_to_sv`/`sv_to_vm` (both depth-capped — cyclic structures fall back safely); **`vm_execute_call` is re-entrant** (exec_base watermark instead of frame_count==0), which enables nested bytecode calls from opcode handlers — used by hash `tie` (TIEHASH/FETCH/STORE) and available for future traps; `before`/`after` method modifiers compile to **synthesized wrapper chunks** (dispatch can't run hooks mid-opcode); `s///e` compiles an inline find/eval/append loop over a new `OP_REGEX_FIND` opcode (the parser already supplies the parsed replacement as `eval_expr`); `tr///` delegates to the runtime's `strada_tr` (full d/c/s/r + count); regex `i/s/m/x` flags fold into an inline `(?...)` pattern prefix at compile time; named captures read the PCRE2 name table. **VM parity (Audit 20 M4)**: VM hashes iterate in insertion order with
the compiled runtime's LIFO reuse of deleted slots (`VMHash`: dense
`entries[]` + `index[]`, `free_head`; the JIT first-probe reads `index`);
regex match/`s///`/`s///e` patterns interpolate `$x`, `${x}`, `$h{k}`,
`$a[i]` (`compile_rx_dynamic_pattern`, pattern index 0xFFFF = pattern on the
stack); all `__C__` blocks of a program JIT into ONE shared object
(`cblock_jit_compile_n`) so file-scope statics are shared (per-block
fallback if that fails); `@{$r} = LIST` / `%{$r} = LIST` replace in place
(`BUILTIN_CONTAINER_REPLACE`); a map block whose value is `[..]`/`{..}`/`\X`
yields one element (`vm_map_value_kind`); `printf`, `free`/`release`,
`getaddrinfo`, `hash_get/hash_set`, `set_package`, `readline` and bare
`strada_*` runtime calls work. Also (M4 part 2): `$_` outside a
topicalizer is the global `main::_`; `reverse(LIST)`; list-valued slice
indices/keys; `sprintf`/`pack` list args (`BUILTIN_SPRINTF_LIST` /
`BUILTIN_PACK_LIST`); list-context regex (`compile_regex_list` ->
`strada_regex_list`); `s///` replacement interpolation; `split(/$p/)`;
`'('` into an int is 40 (`vm_try_char_code`); scalar `splice`;
autovivifying `push/unshift/splice` targets (`compile_mutable_array_target`,
`BUILTIN_AUTOVIV_ARRAY`); `keys` resets `each`; `delete @h{...}`;
`chomp(@a)`; `(LIST) x N`; `return ()` / `my $x = ()` are undef; `gc_*`,
`spew_fh`, `*_default_capacity`. The generic bridge maps `core::X` to the
runtime's `strada_X` when not harvested and passes NULL for omitted
trailing parameters (`vm_generic_builtin_find_min`); the generator also
harvests per-arity calls inside handler blocks (743 entries). M4 part 3: `::f()` in `use`d modules; `core::coro_yield_io` returns 0
(no green tasks in the VM yet, so Async::Task takes its blocking paths);
VM-native `core::poll` (writes revents into the caller's hashes); readline
limit + sockets; `+>>` and NUL-path rejection in open/spew (spew returns
1/0); print/say return 1; in-memory handles sync after bridged calls; lazy
builders (`ast_new_method_call` sets `base_object`); C structs modelled as
hashes with C field storage (`vm_emit_struct_new`,
`BUILTIN_STRUCT_FIELD_STORE`: int32 / double / 63-byte str); cstruct_* short
forms; constructor-less `Pkg::new`; string negation; `""`-only numification
and `-$obj` via `-`; tied-hash clear fallback; `release($sref)`; foreach
aliasing write-back (`vm_node_writes_var`, `BUILTIN_FE_WRITEBACK`) and
restore of a pre-existing loop variable (skipped in tie programs, like the
compiled backend's `uses_tie`); magic string ++ (`BUILTIN_INCR_VALUE`, JIT
slow path); string ranges; `ref(\$ref)` is "REF". M4 part 4: futures -- an `async func`
call / `async::spawn` hands an exported closure + argument list to the
runtime pool (`BUILTIN_ASYNC_OP`; `await`, all/race/timeout/cancel/is_done/
is_cancelled/sleep/cancelled/map/select/io_wait/resolve/pool_*); `thread::`
mutex/cond/tls via the bridge; trapped channel send; green tasks on the
runtime coroutines (`BUILTIN_CORO_OP`): a task's VM code runs in its own
worker VM (`vm_exported_closure_call` + `strada_coro_in_task()`),
`g_active_vm` follows resume/yield, and `local` is per task
(`vm_task_locals_swap`); exact IV/UV/NV integer promotion
(`vm_exact_int_arith`, `vm_from_int128`, UV literals via
`BUILTIN_NUMIFY_EXACT`; int-declared targets still wrap);
`core::exception_trace` (captured at OP_THROW); `c::callback` (interpreter
built with HAVE_LIBFFI); `utf8::normalize`; `link_lib` libraries are linked
into __C__ JIT objects (`cblock_link_flags`). VM limits that remain:
DESTROY runs at GC time, not at the last reference (tracing GC), and raw
pointers into variables (`core::int_ptr`) are compiled-only. `interpreter/Makefile`: the front-end object
depends on `vm.h` (its `__C__` blocks include it). VM stack-value convention: **only string temporaries are owned by the stack** — consume sites must use `vm_temp_free` (freeing a shared container/filehandle/closure is a use-after-free; this was the `defined($fh)` corruption bug).

**VM parity batch (Audit 20, 2026-11-18)**:
- **Overloads.** In a program that declares any `use overload` (`g_prog_has_overloads`, found by scanning the AST, because the overload table is filled only after the functions compile), the compiler emits `OP_STR_OV` after the string operands of `length`, `uc`, `lc`, `substr`, `index`, `ord`, `split` and the `=~` subject. The opcode leaves non-objects alone.
  - Hash keys and array indexes are converted at run time (`VM_KEY_OV` in `OP_HASH_GET`/`SET`/`EXISTS`/`DELETE`, `VM_IDX_OV` in `OP_ARRAY_GET`/`SET`), which covers every compile path (slices, lvalues).
  - `vm_exec_builtin` converts blessed arguments per builtin (`vm_builtin_ov_kind`: string, number via `0+`, or abs).
  - String compares and `x` stringify after `VM_TRY_BIN_OV` finds no operator overload (`VM_STR_OV_PAIR`). `join` stringifies each element and lets a throw propagate.
  - `vm_to_num_vm` and `vm_to_int_vm` honor `0+`; sprintf's numeric conversions do too.
  - `.=` dispatches `.` in `OP_APPEND_LOCAL` and `OP_APPEND_CONST`. The JIT step helper resumes the interpreter (`VM_JIT_INTERP`) for a blessed operand of NOT, CONCAT, APPEND, STR_LEN or BUILTIN.
  - `abs` with only `-` overloaded is `$x < 0 ? 0 - $x : $x`, compared through `<` (also in the runtime's `strada_overload_unary`).
- **Regex.** `while (S =~ /re/g)` keeps a per-loop position local and matches through `OP_REGEX_FIND` plus `BUILTIN_REGEX_GPOS_NEXT` (`compile_regex_g_cond`). It is reset when the while statement starts, like the compiled `__gpos_N`.
  - Captures hold `VM_RX_MAX` (256) groups; `regex_ngroups` is the pattern's group count, so `captures()` lists unmatched groups as undef.
  - An unset `$N` is undef.
- **I/O.**
  - `<$fh>` drops only the `\n`.
  - `@a = <$fh>` is `BUILTIN_READ_ALL_LINES`.
  - `STDIN`, `STDOUT` and `STDERR` are values (`BUILTIN_STD_HANDLE`, wrapping an immortal runtime filehandle, so it is never fclose'd by GC), and `<STDIN>` reads stdin.
- **Calls.** A call with more than 255 arguments uses the spread opcodes, since argc is a u8 operand.
- **Name resolution.**
  - An unqualified call resolves to the current package's function, then main's, then `qw_imports`. The any-package alias fallback applies only where no package is known.
  - File-level `my` and `const` are lexical. A redeclaration is registered as `NAME__shadowN`, and a function sees the nearest declaration before it in its own file (`file_my_lexical_key`). `Pkg::CONST` uses the `const_pkg` that the parser records (Semantic uses it too).
  - A redundant `extern func Pkg::f` declaration is skipped.
  - `use Mod;` calls `Mod::import(PKG, [])` from a synthesized `__import_calls` begin chunk.
- **Scoping and types.**
  - `my $x = INIT` adds the slot after compiling INIT.
  - An `int` parameter (`local_rawint`, `VMChunk.rawint_slots`) is converted where it is an operand of int `+ - * %`, as in compiled use-site conversion. The fused local ops do this in their slow paths (`vm_rawint_operand`), so fib-style code keeps its fast paths. The generic path emits `OP_CAST_INT`.
- **Builtins.** `core::getenv` of an unset name is undef, `core::stat`/`lstat` go through the runtime, and `tell` on a closed handle is -1.
- **Interpreter front end.** `VM error:` goes to stderr. The REPL's `is_balanced` waits for heredoc terminators and continues after a trailing binary operator.
  - Trailing `/`, `<` and `>` do not count as operators (`s/a/b/`, `<$fh>`); `->` and `=>` do.
  - In the REPL and eval, a module's `import()` runs once (`%g_eval_vm_imports_done`), and a redeclaration with an initializer is seeded with the earlier value (`keep_seed`).
- `interpreter/test/run_tests.sh` accepts `test_X.stdin`, `test_X.stdout_only` and `repl_X.input`/`.expected` sessions.

**VM performance, tier 1 (2026-11-18)**: the user wants the VM fast before more parity work. Profile with callgrind (perf is blocked on the dev box), using a `-g` copy of `vm.c` linked against the existing `interp_*.o`.
- **`int` parameters keep the JIT.** The prologue that converts an `int` parameter starts with `OP_JMP_IF_INT_LOCAL`, which skips the conversion when the argument is already an integer or undef. The JIT compiles that opcode. The old prologue began with `OP_DEFINED`, which the JIT doesn't compile, so every function with an `int` parameter bailed out on every call (fib(32): 0.23s to 0.04s).
- **Function lookup by name is hashed.** `VMProgram.fn_index` is open-addressed, maintained by `vm_program_add_func`, and the first registration of a name wins.
- **The collector's `DESTROY` lookup is cached per VM.** `destroy_cache` is keyed by class name and reset when the program or its function/inheritance counts change. oop was spending 46% of its time on this lookup.
- **Regex captures are cleared only up to `regex_hw`.** That high-water mark is raised by every capture and capture-name writer.
- **`$x = $x . A . B ...` appends in place** when every part is a literal or a variable other than `$x` (`is_self_concat_chain`). This is skipped in programs that use `tie` or overloads. bench_utf8 was quadratic here.
- **The `__C__` JIT cache key includes the runtime and VM headers** (size and mtime) and the sizes of `VM`/`VMProgram`/`VMChunk`. A cached `.so` built against an older `vm.h` read stale struct offsets: lib/Eval's blocks segfaulted.
- Test: `interpreter/test/test_vm_perf_c20.strada`.
- Gate VM changes with the suites run twice, JIT on and off (`--no-jit` on both runners and on `t/examples_parity.sh`). After a `vm.h` change, rebuild `lib/Eval.o` (`make lib/Eval.o`).

**VM performance, tier 2 (2026-11-18)**: callgrind instruction counts fell by up to 47% (closures 47%, hotpaths 30%, binary_trees 21%, oop 15%, data 9%); details in CLAUDE_DEEP_DIVE.md.
- Closures and variadic functions run in the JIT, and JIT code calls an already-compiled callee directly (`vm_jit_call_prep` / `vm_jit_closure_prep` build the frame in C and return the entry).
- `%`, `!`, `scalar(@a)`, `defined` and int captures are native JIT code.
- The step helper handles only the plain case of `<=>`, hash get/set and `bless`. Anything else bails (`jit_step_bail`, `VM_JIT_INTERP`) and the interpreter's handler runs the opcode, so tie, weak refs and overloads stay in one place.
- Integers stringify without `snprintf` (`vm_i64_to_cstr`).
- Test: `interpreter/test/test_vm_perf_c20b.strada`.

**VM performance, round 3 (2026-11-18)**: compute -21%, functions -12%, binary_trees -12%, sort -10%, data -9%, everything else 0-8% fewer instructions; details in CLAUDE_DEEP_DIVE.md.
- JIT integer add/sub use the CPU overflow flag on values shifted into the top 50 bits; calls use the callee's constant facts; fib is 11% fewer instructions.
- Integers outside the 50-bit inline range come from the cell pool, and `+=` on them has a direct path (4.7x fewer instructions).
- Push, `{}`/`[]`, `.`, hash get/set, `bless` and `<=>` have their own JIT helpers (`jb_emit_op_helper`); `<=>` on ints is native.
- `try`, `sprintf`, `map`, `chr`, `uc`/`lc`, `split`, `delete`, `isa`, closure creation and constant-pattern regex no longer leave JIT code. Their interpreter code moved into shared `vm_op_*` functions.
- `core::exception_trace()` keeps the throw site's frames when a nested run re-raises the exception (`VM.exc_trace_keep`).
- Known VM parity gaps around int64 overflow (untyped `+`, `++`) are listed in CLAUDE_DEEP_DIVE.md.
- Test: `interpreter/test/test_vm_perf_c20c.strada`.

**VM performance, round 4 (2026-11-19)**: closures -21%, oop -20%, functions -14%, hotpaths -13%, compute -12%, binary_trees -5%, sort -4%; details in CLAUDE_DEEP_DIVE.md.
- JIT calls compare `frame_count` with one bound, `VM.jit_frame_limit` (min of `frame_cap` and `recursion_limit`; call `vm_frame_limit_update` after changing either). Self-calls are a direct `call rel32`, and compiled callees skip the `jit_state` check.
- RETURN takes the slow path only when the top try handler belongs to the returning frame, not whenever any `try` is active.
- `return <int expr>` hands the value to RETURN in a register (`ret_fuse_*`, `jb_return_after`), and `LOAD_LOCAL; PUSH_I8/LOAD_LOCAL; EQ..GE; JMP_IF_FALSE/TRUE` compares and branches in one step. Both keep the standalone code for other paths into it.
- Closure calls push their frame in JIT code; `vm_jit_closure_prep` is the fallback.
- Constant-key hash get/set in JIT code scan all slots of a small (inline) hash, not only the first.
- `vm_execute_call_cl` (calls from C: sort comparators, tie, overloads) runs an already-compiled function's JIT code directly; `vm_execute_call_full` is the interpreter, and resumes the frame if that code bails or throws.
- `foreach my $x (A..B)` with int-typed bounds counts in a hidden local instead of building the list (`fe_range` in vm_compiler.c), the compiled backend's rule.
- Test: `interpreter/test/test_vm_perf_c20d.strada`.

**VM performance, round 5 (2026-11-20)**: strings -69%, data -28%, regex -20%, binary -8%, pipeline -7% (scaled instruction counts); all other benchmarks unchanged. Only `interpreter/` changed.
- **Lazy regex captures.** After a PCRE2 match the VM keeps one copy of the matched span and the group offsets (`vm_rx_record`; `VM.rx_subj`/`rx_ov`/`rx_lazy`). `$N`, `captures()`, `named_captures()` and `capture()` read them directly (`vm_rx_get`; named groups come from the pattern's name table, `rx_name_re`). Any other code that touches `regex_captures[]` must call `vm_rx_materialize()` first. `vm_store_regex_capture` does that itself, and so does evicting the pattern from the regex cache.
- **`s///` with a plain-text replacement** (no `$` or `\`) runs in the VM (`vm_subst_native`): a literal pattern without flags is a `memmem` replace, anything else loops `pcre2_match` over the VM's cached pattern. Other replacements still use `strada_regex_replace_capture_len2` plus `vm_sync_runtime_captures`. Captures follow Perl. The first match is recorded. A pattern without groups keeps `$1..` and sets `$&`; the old path replaced them with the runtime's stale captures. The compiled literal fast path (`strada_subst_sv_cnt`) records its first match the same way (`strada_subst_record_literal`, 2026-11-20); before, it left `$&` at the previous match. It builds the previous match's captures only when that match had groups to keep (about 9% more instructions on a loop of literal `s///`, no wall-clock change). Perl differs in one respect that both backends share: after a successful match without groups, Perl's `$1` is undef.
- **`split` on a separator without regex metacharacters** builds VM strings directly (`vm_split_literal`; `" "` awk mode and regexes still go through the runtime).
- **`vm_exec_builtin` no longer calls `setjmp`** (moved to `vm_substr_replace_guarded`). A `setjmp` anywhere in a function stops gcc from sharing stack slots between its blocks, so the case-local buffers added up to a 95 KB frame. `-fstack-clash-protection` probed it page by page on every builtin call, and it was a large share of a green task's stack. The frame is now 7.4 KB. Keep `setjmp` out of `vm_exec_builtin` and other large dispatch functions.
- **`<$fh>` and `@a = <$fh>`** read with `getline()` into `VM.rl_buf` (default `$/`; the limit form keeps the `fgetc` loop).
- **Tried and dropped:**
  - A 64K-object GC floor collected less often but added 13-32% instructions on gc/oop/closures.
  - A sprintf fast path through glibc `snprintf` was slower than the runtime's own formatters, even with the argument conversions.
- Test: `interpreter/test/test_vm_perf_c20e.strada`.

## Language Features Reference

### Namespaces

- **`core::`** (preferred) - System/libc functions: `core::open()`, `core::fork()`, `core::getenv()`, `core::caller()`, `core::full_profile_start()`, `core::full_profile_stop()`, etc.
- **`sys::`** - Legacy alias for `core::` (both work, `core::` normalized to `sys::` at compile time)
- **`math::`** - Math functions: `math::sin()`, `math::sqrt()`, `math::pow()`, etc.
- **`async::`** - Async/threading: `async::all()`, `async::channel()`, `async::mutex()`, etc.
- **`c::`** - Low-level memory: `c::alloc()`, `c::free()`, `c::is_null()`, etc. Plus **`c::callback($closure, "ret", "args")`** (libffi trampolines — see below)
- **`utf8::`** - UTF-8 validation: `utf8::is_utf8()`, `utf8::valid()`, `utf8::downgrade()`, etc.
- **`usb::`** - USB device access (requires libusb): `usb::open_device()`, `usb::bulk_transfer()`, etc.
- **`ssl::`** - TLS/SSL sockets: `ssl::connect()`, `ssl::read()`, `ssl::write()`, etc.
- **`re::`** - Regex function forms (preferred): `re::match()`, `re::replace()` (regex, **first** match), `re::replace_all()`, `re::capture()`, `re::captures()`, `re::named_captures()`
- **`str::`** - Literal (non-regex) string replace (preferred): `str::replace()` (**all** occurrences), `str::replace_first()`
- **`sb::`** - StringBuilder (preferred): `sb::new()`, `sb::append()`, `sb::to_string()`, `sb::length()`, `sb::clear()`, `sb::free()`

#### Namespaced builtin aliases (2026-06)

Strada-specific builtins that historically lived in the bare namespace now have preferred namespaced spellings; the bare names remain as legacy aliases. Besides `re::`/`str::`/`sb::` above, these bare builtins gained `core::`-qualified forms: `hash_new/hash_get/hash_set`, `deref/deref_array/deref_hash/deref_set`, `refto/is_refto/derefto/is_ref`, `refcount`, `dumper/dumper_str`, `stacktrace/stacktrace_str`, `set_package/inherit/blessed`. Perl-heritage builtins (`say`, `push`, `substr`, ...) stay unqualified.

**Mechanism** (keep it this way): all aliases normalize to the canonical bare names in ONE place — `ast_normalize_call_name()` in `compiler/AST.strada`, called from `ast_new_call()` at parse time. Semantic, CodeGen, and every codegen predicate (`needs_temp_cleanup` etc.) only ever see the canonical spelling, so there is nothing to keep in sync downstream (see the name-resolution unification rule in the CodeGen checklist). To add a new alias, extend that map — do NOT add per-name alias checks in CodeGen. One lexer assist makes `str::` possible: a word immediately followed by `::` is always lexed as IDENT, never as a keyword (so the `str` type keyword can name the namespace). `re`, `str`, and `sb` are effectively reserved package names for `pkg::func()` calls. Test: `examples/test_namespaced_builtins.strada` (registered in `t/t_core.sh`).

### OOP System

```strada
package Animal;
has ro str $species (required);    # Moose-style attributes
has rw int $energy = 100;          # rw = getter + set_NAME() setter

func speak(scalar $self) void {
    say($self->species());
}

package Dog;
extends Animal;                     # Inheritance
has ro str $name (required);

before "bark" func(scalar $self) void { say("[preparing]"); }
func bark(scalar $self) void { say($self->name() . " barks!"); }
after "bark" func(scalar $self) void { say("[done]"); }

package main;
func main() int {
    my scalar $d = Dog::new("name", "Rex", "species", "dog");
    $d->bark();           # before/bark/after hooks fire
    say($d->isa("Animal")); # 1
    return 0;
}
```

No-parens methods use `@_` (object prepended): `func bark { my $self = shift; say($self->name() . " barks!"); }`

Features: `has ro|rw`, `extends`, `with` (roles), `before`/`after`/`around` hooks, `(required)`, `(lazy)`, `(builder => "method")`, auto-generated constructor with named args, `isa()`, `can()`, `AUTOLOAD`.

`(required)` is enforced by the generated constructor (it throws "Missing required attribute ..."); a call that omits one is not inlined (`inline_ctor_missing_required`). An attribute with a builder gets an accessor that calls the builder on first read while unset and stores the result; such accessors are never inlined as plain hash reads. Named arguments may be written `Pkg::new(name => "x")`: in call arguments the fat comma is a comma (`ast_add_arg` spreads the pair the expression parser builds for `=>`).

### Dynamic Method Dispatch

```strada
my str $method = "speak";
$obj->$method();            # Call method named by variable
$obj->$method($arg1);       # With arguments
$obj->$method;              # Without parens (accessor style)
```

The method name is resolved at runtime via `strada_method_call()`. Supports spread arguments. No runtime changes needed — the variable is converted to a C string with `strada_to_str()` and passed to the existing dispatch.

### Operator Overloading

```strada
package Vector;
func add(scalar $self, scalar $other, int $reversed) scalar { ... }
func to_str(scalar $self) str { ... }
use overload "+" => "add", '""' => "to_str", "fallback" => 1;
```

Supported: `+`, `-`, `*`, `/`, `%`, `**`, `x`, `.`, `""`, `==`, `!=`, `<`, `>`, `<=`, `>=`, `<=>`, `eq`, `ne`, `cmp`, `neg`, `!`, `~`, `bool`. Zero overhead when not used.

### `__C__` Blocks (C Interop)

```strada
package mylib;

__C__ {
#include <mylib.h>
static int global_state = 0;
}

func process(str $data) int {
    my int $result = 0;
    __C__ {
        const char *str = strada_to_str(data);
        int ret = my_c_function(str);
        strada_decref(result);
        result = strada_new_int(ret);
        free((char*)str);
    }
    return $result;
}
```

Variables are `StradaValue*` pointers. Use `strada_to_int/str/num()` to extract, `strada_new_int/str/num()` to create. Always `strada_decref()` before reassigning, `free()` after `strada_to_str()`.

### `extern func` with a body (C-callable, 2026-11-19)

`extern func NAME(int $a, str $s) str { ... }` exports NAME with a plain C
signature (`int` -> `int64_t`, `num` -> `double`, `str` -> `char*`, a `str`
result is a malloc'd copy the caller frees or NULL for undef,
`scalar`/`array`/`hash` -> `StradaValue*`). The parser
(`extern_split_body`) moves the body into an ordinary Strada function
(`is_extern_body`): in a package it is `Pkg::NAME`, an ordinary package
function listed in the export metadata, so `import_lib` users call
`Pkg::NAME`; in package main (where NAME is the C symbol) it is
`NAME__xbody` (`extern_body_hidden`, left out of the metadata). The body gets
semantic analysis, `our`, cleanup and everything else; codegen emits NAME as a
C wrapper (`gen_extern_wrapper`, types from `extern_abi_c_type`). Strada
callers of NAME call the body function directly (NAME's function info has
`is_extern` 0 and `c_name` = the body function); the extern node keeps a
one-line forwarding body for the VM. An extern and a `func` of the same name
in one package are a "function already defined" error.
Before, the body was compiled with raw C parameters standing in for Strada
values and only arithmetic worked. A C program that loads such a `--shared`
library links the runtime with `$(strada --embed-ldflags)`. Test:
`t/extern_c_host_test/run.sh`.

### C Callbacks (`c::callback`, libffi trampolines)

Pass a Strada closure to C code expecting a function pointer (qsort comparators, libcurl/GTK callbacks):

```strada
my scalar $cmp = c::callback(fn (scalar $pa, scalar $pb) int {
    return c::read_int64($pa) <=> c::read_int64($pb);
}, "int", "ptr,ptr");          # returns the C function pointer as an int address

__C__ {
    qsort(buf, n, 8, (int(*)(const void*,const void*))(intptr_t)strada_to_int(cmp));
}
c::callback_free($cmp);        # optional; an exit-time registry sweep frees survivors
```

- Signature strings: return = `void`/`int` (int64)/`int32`/`num`/`ptr`; args = comma list of `int`/`int32`/`num`/`ptr`/`str` (max 8). `str` args arrive as Strada strings (NULL → undef); `str` returns are not supported (no safe ownership). `ptr` values travel as int addresses (pair with `c::read_*`/`c::write_*`).
- Argument/return marshaling and closure invocation go through `strada_ffi_callback_new` → libffi `ffi_prep_closure_loc` → `strada_closure_call`. Captures and `our` globals work inside the callback.
- The trampoline holds an incref on the closure until `c::callback_free` or process exit. Only invoke from threads the Strada runtime knows about.
- Requires `ffi.h` at build time only (`./configure` auto-detects; `--without-libffi` opts out). **No link dependency**: binaries never link `-lffi` — `libffi.so` is dlopen'd lazily at the first `c::callback` call, and a clear runtime error names the missing library if it isn't installed.

### Module System

```strada
use lib "lib";
import_lib "MyLib.so";       # Runtime dynamic loading
import_object "MyLib.o";     # Static linking (.o)
import_archive "MyLib.a";    # Static linking (.a)

MyLib::function(args);        # Namespace call syntax
```

- `use Module` - compile-time source inclusion
- `import_lib` - runtime dlopen, reads metadata from `__strada_export_info()`
- `version "1.0.0";` - Module versioning
- `func import(str $pkg, array @list) void` - Auto-called on load

### Guarded Cross-.so Devirtualization (import_lib, 2026-06)

Method calls on receivers whose class lives in an `import_lib` .so now
devirtualize to **direct calls through the host's import wrapper** — same
mechanism as local-class devirtualization (`known_types` from the
constructor + `all_func_names` lookup), closing most of the former
~1.6–2x dispatch gap on method-diverse cross-.so code. How it stays safe:

- **Metadata** (`__strada_export_info`): `func:` lines gained field 6 —
  the original qualified name, `::` encoded as `.` (e.g. `SoCounter.bump`)
  so the consumer recovers the real package/method split (the C-mangled
  name is ambiguous). New lines: `modinfo:1` (marker: this metadata
  carries modifier info — **libraries without it never devirtualize**)
  and `mod:NAME` per hook-bearing method name (`has_method_modifier`
  checks these via `$cg->{"lib_modified_methods"}`, blocking devirt and
  accessor inlining for that name).
- **Fingerprint guard against swapping the .so after the host is built**:
  the compiler hashes the metadata it compiled against (`export_meta_hash32`
  in Parser.strada — additive djb2 mod 2^32, pure Strada; MUST stay
  algorithm-identical to the runtime's `strada_export_meta_hash_cstr`).
  The generated per-lib `__import_lib_<lib>_ensure()` (dlopen + OOP inits,
  factored out of the wrappers) rehashes the loaded .so's metadata; a
  match sets `__import_lib_<lib>_devirt_ok`.
- **Fallback in the wrapper**: on mismatch, method-shaped wrappers
  (original name package-qualified, ≥1 param, non-variadic) re-dispatch
  by name via `strada_method_call_ph` when the first arg is blessed
  (`strada_blessed_name_cstr`) — late-bound, hooks/overrides included. So
  a swapped .so runs correctly at dynamic-dispatch speed instead of
  calling stale symbols. Unblessed first arg falls through to the direct
  call (pre-devirt behavior).
- **Wrappers are now `static`**: the .so defines the same-named global
  symbol, and its internal calls would interpose to the host's wrapper
  under `-rdynamic` — previously a harmless extra hop, but infinite
  recursion once the wrapper can re-enter dynamic dispatch. Do not make
  them global again.
- Devirt eligibility (consumer side, `all_func_names` build in
  CodeGenStmt): lib has `modinfo:1` + function is method-shaped + method
  name not `mod:`-listed (and not host-modified). Old .so / old metadata
  → no devirt, zero behavior change. `import_object`/`import_archive`
  are not yet devirtualized (follow-up).
- Tests: `t/import_lib_devirt_test/run.sh` (direct-call shape, `mod:`
  gating, swap fallback incl. new-hook firing);
  `t/leak_tests/test_import_lib_devirt.strada` (valgrind, with companion
  .so pre-built by the leak runner).

### Module Caching (`--module-cache`, 2026-06)

`strada --module-cache prog.strada` makes `use Foo;` link Foo's precompiled
module artifact instead of re-lexing/re-parsing its source on every build —
Go-package-archive-style separate compilation, fully automatic:

- **Cache**: `$STRADA_MODULE_CACHE_DIR` (default `~/.cache/strada/modules`),
  keyed by the module source's absolute path (works for read-only lib dirs
  like `/usr/local/lib/strada/lib`) **and by a fingerprint of the build's
  `-D` define set** (`STRADA_MODULE_CACHE_KEY`, set by the wrapper): module
  artifacts embed stradapp AND `__C__` `#ifdef` conditionals — e.g. DBI's
  MySQL driver exists only under `-DHAVE_MYSQL` — so builds with different
  defines must never share artifacts (a defineless cached DBI.o once
  poisoned MySQL builds with "unsupported driver" at runtime). The warmer
  forwards the defines to its `-M` sub-builds. A fresh **sibling** `Foo.o`
  (from `strada -M`) still takes priority — siblings are define-blind, so
  only use them in single-configuration projects. Freshness gates:
  artifact ≥ source mtime AND artifact ≥ stradac mtime (compiler upgrades
  invalidate).
- **Warming**: after a build, any module that had to be inlined from source
  is precompiled into the cache (leaves first), so the first build after a
  change pays a one-time warm-up and later builds skip unchanged modules.
  Measured on sysync-web (13k lines, 12 modules): warm full build 0.47s vs
  1.7s uncached (stradac phase 0.36s vs 1.95s).
- **`.strada_meta` section — self-describing artifacts (2026-06, replaced
  `.smeta` sidecars)**: every compiled `.o`/`.so` embeds its export metadata
  (the text `__strada_export_info()` returns — functions, signatures,
  transitive `use:` deps, modifier info) in a dedicated `.strada_meta` ELF
  section, emitted by `gen_export_info` (CodeGenStmt.strada) as a `static
  const char __strada_meta[]` that the function also returns. A consumer
  resolving `use Foo;` / `import_object` / `import_lib` against an artifact
  reads that section **in-process** via `strada_read_meta_section` (runtime
  C): it parses the ELF section table directly — **no subprocess, no
  `objcopy`, no dlopen, no artifact code executed** at compile time, sub-ms.
  This single self-describing mechanism replaced BOTH the old `.smeta`
  sidecar files (a separate, losable/stale file that had to be
  written/installed/kept-fresh) and, on the hot path, the old
  compile-and-run probe (~150ms of `cc` per module). The metadata lives *in*
  the artifact, so there is nothing extra to write or install (the `.o` is
  still linked into the final binary either way — the section only removes
  the compile-time interface read, not the object). Resolution order in
  `extract_object_export_info` (Parser.strada): (1) `.strada_meta` section →
  read in-process; (2) else the probe-result cache (below); (3) else the
  **live probe** (build a tiny binary, link the `.o`, run it to print
  `__strada_export_info()`). The reader (`strada_read_meta_section`, runtime
  C) handles, in-process: **ELF64 + ELF32** (Linux), **thin 64-bit Mach-O**
  (macOS), and **`ar` archives** (`.a` — it scans members and reads the
  section from the member object). It is **format-based, not arch-based**, so
  every mainstream architecture works for free — an `aarch64`/`arm64` ELF
  section table is identical to x86-64's, and Apple-Silicon macOS produces the
  same `MH_MAGIC_64` thin Mach-O as Intel. The probe fires only for what the
  reader can't parse: **fat or 32-bit Mach-O, big-endian, or thin-LTO objects**
  (a plain `-flto` `.o` keeps its `__strada_meta` array in LTO bitcode, with no
  real section). To get a readable section, an artifact must materialize one:
  `-M` builds `-fno-lto`; `--object` and `--static-lib` build
  `-ffat-lto-objects` (real section **and** LTO bitcode — so consumers read the
  section in-process yet can still cross-inline across the library at link).
  `.so` consumption
  (`do_import_lib_at`) reads the section too, falling back to dlopen +
  `__strada_export_info` only for section-less `.so`s, so consuming a `.so` no
  longer runs its constructors/code in the compiler.
- **`strada_read_meta_section` is a runtime builtin** (`StradaValue*` in/out,
  wired like `strada_dl_open_raw`: Semantic + CodeGenBuiltins + `owned_set`).
  Adding it required a full `make`, NOT `make quick`: the existing `stradac`
  rejects a call to a builtin it doesn't yet know ("undefined function"),
  while the permissive frozen bootstrap emits the unknown call as a direct C
  call, and stage-1 onward (compiled from the registering source) then knows
  it. New compiler-callable runtime functions must follow this — `make quick`
  cannot introduce them.
- **Probe-result cache** (2026-06): for the rare artifacts that fall to the
  live probe (non-ELF / ELF32 / `.a`, where the `.strada_meta` section can't
  be read in-process), the probe's output is cached under
  `$STRADA_MODULE_CACHE_DIR/probe-meta/` keyed by the `.o`'s realpath+mtime
  (`extract_object_export_info` in Parser.strada) — a replaced `.o` gets a
  new key, so entries can't go stale. The probe then runs once per `.o`
  version instead of every build. (On Linux ELF the section path wins and
  this is never hit.)
- **Transitive deps**: export metadata now records `use:Module` lines;
  importing an artifact resolves its deps automatically (artifact
  preferred, source fallback) — main no longer needs to `use` everything
  its modules use. Object paths are realpath-canonicalized for dedup so an
  explicit CLI `.o` plus a metadata-resolved one don't double-link.
- **Artifact codegen**: `-M` objects are built `-fno-lto` (LTO bitcode made
  every consumer link re-run a ~25s LTO pass) and `-fPIC` (same artifact
  links into executables and `--shared` libraries).
- Implies `--use-artifacts`. **Artifact use is now ON by default** (2026-06):
  a plain `use Foo;` prefers a fresh sibling `Foo.o`/`Foo.so` (read in-process
  via `.strada_meta`, no code executed at compile time). Disable with
  `--no-use-artifacts` (or `STRADA_USE_ARTIFACTS=0`) to always recompile from
  source. The default flipped from off→on because the in-process section reader
  closed the old compile-time code-execution risk; the residual is that the
  sibling `.o` is linked into your binary (trust the artifact over the source),
  guarded only by the two freshness gates. The gate lives in Parser.strada
  (`$__ua ne "0"` ⇒ default on). The explicit
  `import_object`/`import_lib`/`import_archive` forms are always honored.
- **Audit M2j (2026-11)**: a program that links any artifact (`.o`/`.a`/`.so`)
  emits no `static inline` functions, so an artifact can call a module the
  program compiled from source. Freshness compares nanosecond mtimes within
  the same second (`strada_file_mtime_ns`, a compiler-callable builtin;
  `core::stat` gained `mtime_ns`). The define-set cache key is set even
  without `--module-cache`. Link-marker paths are %-encoded (spaces, `*/`).
  `--static-lib` recreates the archive. `--tcc -g` passes `-g` (tcc writes
  stabs). Everything after `--doc` goes to stradadoc. The shim follows
  symlinks.
- `strada --clear-module-cache` wipes the cache directory (always safe —
  the next `--module-cache` build regenerates it).
- Artifacts are deliberately **not LTO-participating** (`-fno-lto`): fat-LTO
  objects made every consumer link re-run a full LTO pass (~25s). Cached
  module code is gcc-optimized within the module but doesn't cross-inline
  with the consumer; for maximum-performance release builds, compile
  without `--module-cache` (source inlining + LTO).
- **ccache-friendly build pipeline** (2026-06): the wrapper now (a) writes
  the generated `.c` (and stradapp output) to a STABLE per-source scratch
  path (`/tmp/strada-scratch-<uid>/<base>-<key>.c`, key = source realpath
  + define set + output mode — `scratch_path()` in `strada`), and (b)
  splits compile from link on the executable and `--shared` paths
  (`cc -c gen.c -o gen.o` then link). Both are required for ccache to ever
  hit: a random temp name changes cpp's `# 1 <path>` line markers (cache
  miss even on identical content), and ccache refuses to cache any
  invocation that links. Result on sysync-web (13k lines, 12 modules,
  -O0): warm rebuild 2.9s → **0.39s** when the generated C is unchanged;
  a real source change still pays one gcc compile (~1.5s). Known limit:
  two SIMULTANEOUS builds of the same source+defines share a scratch path.
- **stradapp shebang passthrough** (2026-06): `#!/usr/bin/env strada` on
  line 1 passes through the preprocessor instead of dying as an unknown
  directive — previously any `-D` build of a shebanged module failed, which
  silently broke module-cache warming (the warmer now also NAMES failing
  modules in its summary line instead of just counting them).
- Tests: `t/separate_compile_test/run.sh` (19 scenarios, incl. sidecars,
  transitive deps, cache dir, cold/warm, probe-result cache, shebang
  passthrough + warming).

**Cross-module forward declarations** (use-cycle workaround): when two modules `use` each other in a cycle, declare callees from the other side with a body-less qualified `extern func`:

```strada
extern func Other::emit(scalar $cg, str $s) void;
extern func Other::make_str(int $n) str;
```

These register in the codegen symbol table under the mangled C symbol (`Other_emit`, `Other_make_str`) so `needs_temp_cleanup` recognizes owned returns and call sites use the ordinary Strada calling convention (no raw-C-type conversion). Use this instead of `__C__ extern` for cross-module Strada calls.

`::func()` in a `use`d module whose package is nested (`package Deep::Leaf;`)
reaches Semantic as `Deep_Leaf_func`; when no single `_` split matches, the
lookup strips the current package's C prefix and tries `Deep::Leaf::func`
(2026-11-21; test `t/test_use_nested_pkg_relative.strada`).

A module artifact's metadata carries the module's own body-less qualified
`extern func Pkg::name(...) ret;` declarations as `xdecl:` lines (same fields
as `func:`). A consumer that resolves `use Mod;` from `Mod.o` re-creates them
(`merge_imported_extern_decls`, objects only), so its calls compile as they
do when `Mod` is read from source (2026-11-21; `t/separate_compile_test`).

A `use` that resolves to the file being compiled (a use cycle back to the
input: A uses B, B uses A, compiling A) is skipped: `parse_program` records
the input's real path as `root_file` and `resolve_use_module` marks such a
module loaded instead of loading it again (2026-11-21).

In a module-only object (`-M`) a file-level `my` is a `static` C global: it
is lexical to the one file the object holds. Exported under its bare name it
collided with the same variable name in another module's object (2026-11-21).

### Exception Handling

```strada
try {
    throw FileNotFound::new("/tmp/missing.txt");
} catch (FileNotFound $e) {    # Typed catch (uses isa())
    say($e->{"path"});
} catch (IOException $e) {     # Multiple typed catches
    say($e->{"message"});
} catch ($e) {                 # Catch-all (must be last)
    say("Unknown: " . $e);
} finally {                    # Always runs: normal, caught, rethrow,
    cleanup();                 # catch-throws, return/next/last crossing
}
```

`finally` notes: runs after the try (or catch) completes, before an unmatched/re-thrown exception propagates, and before `return`/`next`/`last` leave the construct (return value is captured first, Java-style). `try { } finally { }` without catch is allowed. Labeled `next`/`last` jumping out of a try/finally skip the finally (documented limitation, mirrors their existing try-frame gap).

**Error chaining**: `core::exception_trace()` returns the Strada call stack captured at the most recent `throw` in this thread (works for plain thrown values, readable inside `catch`). `lib/Exception.strada` provides structured exceptions: `Exception::new($msg)` (captures construction trace), `Exception::wrap($msg, $cause)` (chains any caught value), `->message/->cause/->trace/->describe()` (renders the whole chain).

### Value-Producing `do {}` Blocks

```strada
my int $x = do { my int $a = 40; $a + 2; };   # 42 — last EXPRESSION statement is the value
my scalar $u = do { my int $tmp = 5; };       # undef — non-expression tail
```

Block-local variables are cleaned up before the value is yielded (the result is safely owned even when it references block-locals). Restrictions: only a last *expression* statement yields a value (an `if`/`else` tail is a statement — use a ternary); a statement starting with bare `do {` parses as `do/while`, so parenthesize do-exprs in statement-head position; control-flow exits (`return`/`next`/`last`) inside a value-do are unsupported (GNU C statement-expression limitation).

### `fn` Shorthand

`fn` is an alias for `func` and can be used anywhere `func` is used: function definitions, closures, `extern`, `async`, `private`, `before`/`after` hooks, etc.

```strada
fn add(int $a, int $b) int { return $a + $b; }
my scalar $f = fn (int $x) int { return $x * 2; };
async fn fetch(str $url) str { ... }
private fn helper() void { ... }
```

### Closures

```strada
my scalar $f = func (int $n) int { return $n * $multiplier; };
$f->(42);  # Arrow syntax for calling
```

**Captured variables are shared, as in Perl**: a closure and its enclosing
scope (and sibling closures) share each captured scalar through a refcounted
cell (`strada_closure_new_shared`, PerlaCell mode), so a counter closure
updates the outer variable and later outer changes are visible inside; each
foreach iteration's `my` variable is a fresh cell. Arrays and hashes are
shared too. `examples/test_closures.strada` checks this.

**Capture analysis is lazy** (2026-11): a variable is captured the first time
codegen names it inside the anonymous function. So every codegen path that
emits a variable's C name by hand, instead of via `gen_expression` on a
`NODE_VARIABLE`, must call `scalar_var_c_ref($cg, $name, $c_name)` (CodeGen.strada),
which returns the capture slot `(*__captures[N])` and registers the capture. Paths
that emitted the bare name compiled fine in ordinary functions and failed in
closures with "'name' undeclared": `/$var/` and `!~` regex patterns, `++`/`--` on
int variables, `chomp($var)`/`chop($var)`. Likewise a declaration that bypasses
`gen_statement`'s `NODE_VAR_DECL` branch (the C-style `for (my int $i …)` init)
must add the name to `anon_local_str`, or it is treated as a capture. Regression:
`examples/test_closure_capture_sites.strada`.

**Regex pattern interpolation** (2026-11): match AND substitution patterns
interpolate `$name` and `${name}` (`regex_pattern_has_interp`,
`gen_regex_interpolated_pattern`, `gen_subst_pattern_prelude` in CodeGen.strada).
Substitution patterns used to be passed as the literal text `$var` and silently
matched nothing. The replacement side of `s///` interpolates the same way.
Element interpolation (Perl): `$name{KEY}` when `%name` is declared and
`$name[INDEX]` when `@name` is declared and INDEX is an integer or `$var`
(otherwise `[...]` stays a character class). The semantic pass parses and
analyzes the element (`regex_interp_elem`), stores it in the node's
`rx_elems`, and leaves a `$\x01N\x02` marker that codegen expands. Regex,
`s///` and `tr///` nodes carry their source line, so these diagnostics are
located.

**`//g` match state is lazy (2026-11-19)**: `strada_regex_match_global` records
the subject and ovector in `pm_state` like a single match (`pm_record`), so
`` $` ``, `$'`, `$1..$N`, `@-`/`@+` and `%+` are built only when read (named groups
are still built at match time, `pm_named_eager`). When the subject pointer is
the `value.pv` armed by `strada_regex_set_subject_for_sv` (and the buffer's
length header matches), `pm_state` holds a reference to that `StradaString`
(`shared_ss`) instead of copying it; copy-on-write keeps the bytes stable for
later `` $` ``/`$'`. Other callers (`char *` copies, e.g. Perla) still get one copy
per match. The compiled `while (S =~ /re/g)` now passes S's string value and
arms it (it used to strdup S and `strlen` it per iteration, which also cut the
subject at a NUL and lost the UTF-8 flag). 30k matches over a 90 KB string:
0.16 s to 0.003 s. Code reading `last_regex_captures` directly after a `//g`
match must call `pm_build_captures()` first (`strada_regex_match_list` does).
Test: `t/bugfix_tests/regex_g_lazy_state.strada`.

Transitive capture works: a closure nested inside another closure captures outer variables through the enclosing closure's `__captures` slots (gen at CodeGenExpr's capture-array emission forces the enclosing closure to capture pass-through names).

### Async/Await

```strada
async func fetch(str $url) str { return http_get($url); }
my scalar $future = fetch("http://example.com");
my str $result = await $future;
```

Thread pool backend (default 4 workers). Functions: `async::all()`, `async::race()`, `async::timeout()`, `async::cancel()`. Channels: `async::channel()`, `async::send()`, `async::recv()`. Mutexes: `async::mutex()`, `async::lock()`, `async::unlock()`. Atomics: `async::atomic()`, `async::atomic_add()`, `async::atomic_cas()`.

**Concurrency ergonomics (2026-06):**
- `async::spawn($fn)` — run any closure as a pool future (the function form of `async func`).
- `async::select(\@channels [, $timeout_ms])` — block until one channel has a value; atomically dequeues. Returns `[index, value]`; index `-1` = timeout, `-2` = all channels closed and drained.
- `async::sleep($ms)` — cancellation-aware: wakes early (returns 0) if this task's future is cancelled; returns 1 after a full sleep. `async::cancelled()` — has THIS task been asked to cancel (cooperative loops poll it).
- `async::map($fn, \@items [, $workers])` — data-parallel map, results in input order; work-shared via an atomic index; the first exception cancels remaining work and rethrows in the caller.
- `thread::tls_set/tls_get/tls_exists/tls_delete($name, ...)` — per-thread named values, freed at thread exit.
- **`Async::Scope`** (lib) — structured concurrency: `$scope->spawn($fn)` / `$scope->wait()` joins all, and a failure cancels the remaining siblings and rethrows. **`Async::Actor`** (lib) — message-driven actors: `tell` (fire-and-forget), `ask` (round-trip), strictly ordered handling, `stop` drains.

**Event loop + green tasks (2026-06, EXPERIMENTAL — branch `async-eventloop`):** `Async::Loop` (epoll reactor: `watch`/`unwatch`/`timer_after`/`run`/`stop`) + `Async::Task` (green tasks via `$loop->spawn`: `Async::Task::recv/send/accept/sleep` look blocking but park stackful ucontext coroutines; outside a task they fall back to blocking). `async::io_wait($fd, "r"/"w", $ms)` returns a future completed by a dedicated poller thread. Runtime primitives: `core::epoll_*`, `core::eventfd*`, `core::socket_try_recv/try_send/try_accept`, `core::mono_ms`, `core::coro_*` (compiled-only). Linux-only, configure-gated (`--without-epoll`); see **docs/EVENT_LOOP.md**. try/catch works inside tasks including across suspensions (the runtime swaps per-context try/cleanup/trace stacks at every switch); uncaught task exceptions are contained at the task boundary (`on_task_error`, default warn). Multiple watchers per fd (subscription ids, union masks). `Async::Task::connect/readline` + optional timeouts on recv/accept/connect; task sockets get TCP_NODELAY. Backends: epoll (Linux) or poll(2) (any POSIX) behind one internal `evb_*` API — the suite passes on both. `Async::TaskSSL` runs TLS handshake/IO over tasks (parks on WANT_READ/WANT_WRITE; `lib/ssl.strada` gained non-blocking primitives + `link_lib` auto-linking). Hostname resolution runs off-loop (`async::resolve`). Remaining caveats: regex state is per-OS-thread (capture before suspending); one loop = one OS thread. Valgrind-clean including a 100-connection echo stress (the once-suspected per-park leak was a pre-existing `strada_socket_close` bug — it discarded `strada_socket_flush`'s owned return, one SV per connection close — fixed here; `STRADA_RC_TRACE` refcount-trace hook retained in `strada_runtime.h` for future hunts). Coroutine context switches swap the per-thread pending-cleanup and call-trace stacks (`coro_ctx_install/stash` in the runtime).

**Global interpreter lock (2026-11-21/22; on by default, building with `--no-gil` leaves it out)**: threads share data
and nothing in the runtime locks arrays, hashes or variables, so two threads
writing shared data corrupt memory (compiled and VM; see LANGUAGE_GUIDE
"Thread Safety Notes"). Once a program is multi-threaded only the lock holder
runs Strada code. Runtime only (`strada_gil_*` / `gil_*` in strada_runtime.c):
- The decision is made once, when the program first becomes multi-threaded
  (`strada_gil_activate`, called where `strada_threading_active` is set). The
  thread making that call takes the lock.
- `thread::create` bodies, pool workers and `async::map` workers take it
  before running (`gil_thread_begin`, before `cc_thread_register`, so a
  thread waiting to start is not counted by the collector's stop-the-world)
  and drop it when they end.
- Every `cc_blocking_enter`/`cc_blocking_leave(_mutex)` pair (37 sites: sleep,
  join, await, channel waits, mutex/cond waits, socket recv/select, idle
  workers, `strada_gc_mutex_lock`) hands it over. These are now wrappers:
  the collector's own hooks are `cc_stw_blocking_*`. A new blocking call in
  the runtime must use the wrappers.
- Lock order: never wait for the lock while holding a runtime mutex
  (`gil_blocking_leave_mutex` drops the caller's mutex first).
- Threads that run no Strada code (I/O poller, resolver) never take it.
- C code: `strada_gil_release()` / `strada_gil_acquire()` around long work
  that touches no Strada values; `strada_gil_poll()` (header inline) is the
  handover check for the later stages.
- **Stage 2 (compiled loops).** A waiter that has not got the lock within
  5 ms (`--gil-interval-us N` at build time overrides, for tests) sets
  `strada_gil_drop_request`; loops check it in their condition
  (`while ((strada_gil_poll(), COND))`, `gil_poll_prefix` in CodeGen.strada).
  - Only programs that can start threads get checks: Semantic sets
    `uses_threads` on a `thread::`/`async::` call, an `await` or an
    `async func`. A library built on its own gets none unless compiled with
    `--gil-polls`.
  - Only loops whose body calls something, contains another loop or is not
    leaf-safe get one. An innermost arithmetic loop has none: the check stops
    gcc from vectorizing or deleting such loops, so a thread inside one keeps
    the lock until it ends. There is no check at function entry either
    (measured: recursion-heavy code paid for it), so recursion without a loop
    does not hand over.
  - Cost, instruction counts against the previous commit: 0.0% on every
    benchmark that starts no thread; bench_async +7%.
- **Stage 3 (VM).** `VM_WORKER_SAFEPOINT` (every backward jump in the
  interpreter, root VM included) and `vm_jit_poll_sig_and_gc` (JIT code calls
  it at least every 256 loop iterations) call `vm_gil_yield`, which marks a
  worker GC-safe while it waits. No JIT code changed.
- **Closures stay alive while they run.** A closure that reassigned the
  variable it was called through (`$cb = func {...}` inside `$cb`) freed its
  own captures: a single-threaded segfault that lock handovers also made
  reachable from another thread. `strada_closure_callN_pin` and
  `strada_closure_call_argv` hold a reference for the call. Codegen uses the
  unpinned `strada_closure_callN` only when the callee is a plain local of the
  calling frame (`closure_callee_is_frame_local`). The pin bumps the refcount
  directly and uses the cleanup stack only inside a `try` (a full
  incref/decref pair buffered the closure as a cycle candidate on every call:
  bench_closures +19%). Test: `t/bugfix_tests/closure_replaces_itself.strada`.
- Verified with a reader/writer stress under AddressSanitizer at a 20 us
  handover interval (array under grep/map/foreach, hash under values/each,
  captured scalars passed down calls, nested structures, in-place append):
  clean after the closure fix. `t/bugfix_tests/gil_reader_writer.strada` is
  the short form.
- VM limit found on the way (not a lock problem): a worker thread's garbage
  is collected only when the thread ends, and the root marks it on every
  collection, so a long-lived allocating thread makes each collection slower.
- **Stage 4 (blocking I/O).** `io_block_enter`/`io_block_leave` hand the lock
  over and keep the handle value alive (another thread may drop the last
  reference to a socket this thread is blocked on): socket accept, connect,
  recv, UDP recvfrom, socket line reads, `qx`, `pclose`, `flock`, raw fd
  reads, DNS lookups, `epoll_wait`. A read from a `FILE*` releases the lock
  only when it is not a regular file (`strada_gil_file_blocks`: pipe,
  terminal, FIFO), and never touches the `FILE*` after the read returns
  (another thread may have closed it; `fclose` waits for the stream lock).
  The file-kind answer is cached per thread while the stream has buffered
  input (glibc); one `fstat` per line made threaded file reads 4.5x slower.
  Still holding the lock: writes (a full pipe or socket), `close` of a pipe
  from a destructor, and C library calls in `__C__` blocks (DBI queries, TLS)
  unless they use `strada_gil_release()`/`strada_gil_acquire()`.
- **VM and blocking calls.** The VM's own blocking builtins (qx, system,
  poll, usleep, nanosleep, waitpid, pclose, pipe `getline`) release the lock
  inside their GC-safe brackets. Rule: a worker that can wait for the lock
  must be GC-safe there, or the root's stop-the-world waits for a thread that
  cannot run. `vm_gc_stw_begin` therefore gives a collection up (returns -1,
  retried after 4096 more objects) when the lock is on and a worker is not
  safe; that was a real deadlock with `pclose`.
- **Stage 5.** On by default. `strada_ffi_handler` takes the lock when a C
  callback arrives on a thread that does not hold it.
- **Settings are build-time, never environment (2026-11-23).** `strada
  --no-gil`, `--gil-interval-us N`, `--gil-polls` (same flags on stradac;
  strada-interp takes the first two). `main()` calls
  `strada_gil_configure(enabled, interval_us)` first thing, so a built
  program behaves the same in every environment; the runtime no longer reads
  `STRADA_GIL` / `STRADA_GIL_INTERVAL_US` and stradac no longer reads
  `STRADA_GIL_POLLS`. The program holding `main` decides; a C host calls
  `strada_gil_configure` itself. `core::gil_info()` reports `enabled`,
  `active`, `interval_us`. Details in CLAUDE_DEEP_DIVE.md. Test:
  `t/gil_options_test/run.sh`.
- **No runtime setting comes from the environment (2026-11-23).** The other
  five variables the runtime used to read are build options too, applied by
  calls at the top of `main()`: `--no-gc` (was `STRADA_GC=off`; the program
  starts with the cycle collector off, `core::gc_enable()` turns it on),
  `--exit-task-wait-ms N` (was `STRADA_EXIT_TASK_WAIT_MS`), `--no-pool` (was
  `STRADA_NO_POOL`), `--debug-bless` (was `STRADA_DEBUG_BLESS`) and
  `--check-cleanup=warn` (was `STRADA_CLEANUP_CHECK_WARN`; a `-D` define, no
  call). strada-interp takes the first four. The Makefile builds stradac
  itself with `--no-gc` (stage 1, generated by the bootstrap, runs with the
  collector on: +0.3 s, +15 MB), so `make quick` needs a `./stradac` that
  knows the flag: run a full `make` once after pulling. The runtime still
  reads `LINES`/`COLUMNS` (terminal size). The VM's JIT switch is
  `strada-interp --no-jit` (`vm_set_no_jit`; it was `STRADA_NO_JIT=1`), so
  no `STRADA_*` variable changes how a program runs on either backend.
  Test: `t/runtime_options_test/run.sh`.
- Tests: `t/bugfix_tests/gil_shared_writes.strada`, `gil_reader_writer.strada`,
  `gil_blocking_io.strada` (hangs without the stage 4 changes).
- Per-object locking was considered and set aside: about 145 runtime
  functions, inline hash probes compiled into user code, the in-place
  `refcount == 1` shortcuts and the VM's own containers would all need it.
- Helgrind does not see these races (0 reports on a program that segfaults).
  Use ThreadSanitizer: build the runtime and the generated C with
  `-fsanitize=thread` and run under `setarch -R`.

### Variadic Functions and Spread

```strada
func sum(int ...@nums) int { ... }
sum(1, 2, 3);
my array @vals = (10, 20);
sum(1, ...@vals, 99);        # explicit spread
sum(@vals);                  # bare @array also flattens into a variadic call
sum(get_array());            # so does any array-VALUED argument (call/map/grep/sort/list)
```

**Flattening into a variadic call** (`gen_user_call`, the `is_variadic ||
has_spread` packing loop in CodeGenBuiltins): an argument flattens its elements
into the variadic array when it is an explicit `...spread`, **or** when it is
statically an array *value* per `array_flatten_kind` — a bare `@array` variable
(kind 1, borrowed) or an owned array temp (kind 2: an array-returning call,
`map`/`grep`/`sort`, a parenthesized list, `split`/`keys`/`values`/`reverse`).
Owned temps are decref'd after their elements are copied out (kind-1 borrowed
vars are not). So for a variadic target, `...` is only *required* to flatten
something `array_flatten_kind` can't prove is an array (e.g. a deref `@$ref`
where the static type is opaque); a bare array-returning call now flattens
correctly instead of being pushed as a single array-ref element (which silently
produced garbage when the callee read it as a scalar). Regression:
`examples/spread_operator.strada`.

Limits (unchanged): spread is a *call-argument* form only — `(0, ...@a, 9)` in a
list literal is a parse error, and spread into a **fixed-arity** (non-variadic)
function does not expand into the parameters (it currently emits a raw C
"too few arguments" error rather than a clean Strada diagnostic).

### Regex (PCRE2 with POSIX fallback)

```strada
if ($text =~ /(\w+)\s+(\d+)/) {
    say($1);  # Capture variable (or use captures()[1])
    my hash %nc = named_captures();  # For (?P<name>...)
}
$str =~ s/pattern/replacement/g;     # Substitution
$str =~ s/(\d+)/uc($1)/eg;          # /e eval replacement
$str =~ tr/a-z/A-Z/;                # Transliteration (tr///, y///)
```

PCRE2 features: `*?`, `+?`, `\b`, `(?=...)`, `(?<=...)`, `(?:...)`, `(?P<name>...)`, `/s`, `/x`, `$1`-`$9` backreferences.

**Threading**: match state (`$1`/`captures()`/`$&`/pre-postmatch) and the compiled-pattern cache are **thread-local** — one thread's match never bleeds into another's view (Perl semantics). Worker threads free their regex TLS on exit.

### File Test Operators

```strada
if (-e $path) { say("exists"); }        # File or directory exists
if (-f $path) { say("is a file"); }     # Is a regular file
if (-d $path) { say("is a directory"); } # Is a directory
```

### STDIN, STDOUT, STDERR

Standard I/O filehandles available as bareword variables:

```strada
say(STDOUT, "to stdout");
say(STDERR, "error message");
my str $line = <STDIN>;          # Read line from stdin
print(STDERR, "debug info");
```

### Operators

- **Arithmetic**: `+`, `-`, `*`, `/`, `%`, `**`, `$i++`, `$i--`, `++$i`, `--$i`
- **String**: `.` (concat), `x` (repeat: `"ab" x 3`), `cmp` (comparison)
- **Logical**: `&&`, `||` (returns value, not bool), `//` (defined-or), `!`; low-precedence `not`, `and`, `or`, `xor` (below assignment, as in Perl)
- **Comparison**: `==`, `!=`, `<`, `>`, `<=`, `>=`, `<=>`, `eq`, `ne`, `lt`, `gt`
- **Assignment**: `=`, `+=`, `-=`, `*=`, `/=`, `.=`, `%=`, `**=`, `//=`, `x=` (`$n = @arr` gives array count)

`||` vs `//`: `||` treats `0` and `""` as falsy; `//` only treats `undef` as falsy.

### Control Flow

```strada
if/elsif/else (elsif and else if are interchangeable), unless/elsif/else, while, until, for, foreach
foreach (@arr) { say($_); }       # Implicit $_ when no variable specified
LABEL: while (...) { next LABEL; last LABEL; redo LABEL; }
do { ... } while (COND);

# Statement modifiers
say("hello") if $verbose;
return 0 unless $ok;
$i++ while $i < 10;
```

### Destructuring

```strada
my ($a, $b, $c) = @array;
my (int $x, str $y) = @mixed;
my ($name, $city) = @user{"name", "city"};  # Hash slice
```

### Array/Hash Slices

```strada
my array @subset = @data[0, 2, 4];
my array @vals = @hash{"key1", "key2"};
my array @range = @data[0..5];

# Slice assignment (values pulled positionally from the RHS list; undef past its end)
@arr[0, 2] = ("X", "Z");
@arr[1..3] = (10, 20, 30);
@hash{"a", "c"} = (1, 3);
@{$aref}[0, 1] = ("p", "q");      # through a deref
@{$href}{"x", "y"} = (7, 8);
```

### File I/O

File handles are reference-counted `scalar` values, auto-closed on scope exit. See `docs/QUICK_REFERENCE.md` for full details.

```strada
my scalar $fh = core::open("file.txt", "r");  # Modes: "r","w","a","rb","<",">",">>"
my str $line = <$fh>;                          # Diamond operator (strips newline, undef at EOF)
say($fh, "output");                            # Print with newline to filehandle
core::close($fh);                              # Explicit close (optional — auto on scope exit)

my str $content = core::slurp("file.txt");     # Read entire file
core::spew("file.txt", $content);              # Write entire file
my str $output = core::qx("ls -la");           # Capture command output

# In-memory I/O, seek/tell/eof/flush, select(), pipe I/O — see docs/QUICK_REFERENCE.md
# core::autoflush($fh, 1)   — unbuffered writes on $fh (perl `$|=1` equivalent)
```

### `our` Variables and Global Registry

```strada
our int $count = 0;                    # Package-scoped global (backed by strada_global_set/get)
func modify() void { $count += 10; }  # Accessible from any function in package
```

Supports `local $var = expr;` for dynamic scoping. Low-level API: `core::global_set/get/exists/delete/keys()`. Works across `import_lib` boundaries.

### Constants and Enums

```strada
const int MAX_ITEMS = 100;       # Global → C #define
const str VERSION = "1.0.0";

enum Status { PENDING = 0, ACTIVE = 10, DONE = 20 }
my int $s = Status::ACTIVE;
```

### BEGIN/END Blocks

```strada
BEGIN { say("runs before main"); }
END { say("runs after main, LIFO order"); }
```

### Other Features

- **Map to hash**: `my hash %lookup = map { $_ => 1 } @fruits;`
- **`tie`/`untie`/`tied`**: Custom variable implementations dispatching FETCH/STORE/EXISTS/DELETE/FIRSTKEY/NEXTKEY/CLEAR. Zero overhead when untied. The kind of tie follows the variable's sigil (`tie($x, ...)` → TIESCALAR, `@` → TIEARRAY, `%` → TIEHASH). In a program that uses `tie` (`uses_tie`), `keys`/`values`/`each` go through the SV-level runtime entry points (`strada_hash_keys_sv` etc.), `%h = ()` calls CLEAR and keeps the tie (`strada_hash_clear_if_tied`), and a statement `$x = EXPR;` on a tied scalar calls STORE. `@a = LIST` / `%h = LIST` on a tied container call CLEAR then STORE per element/pair and keep the tie (`strada_array_assign_if_tied` / `strada_hash_assign_if_tied` via `emit_container_replace`; VM: `BUILTIN_TIE_ASSIGN`); `push`/`unshift` of a list make one PUSH/UNSHIFT call (`strada_av_push_list_sv`); negative indices are offset by FETCHSIZE; a void-context compound assignment on a tied scalar FETCHes once.
- **Private functions**: `private func helper(int $x) int { ... }` — `static` in C, not exported
- **Dynamic return type**: `func flexible() dynamic { ... }` with `core::wantarray()` / `core::wanthash()`
- **Package-qualified calls**: `::helper()` resolves to current package. Also `.::func()`, `__PACKAGE__::func()`. Plain `__PACKAGE__` is the package the code is written in, resolved at compile time.

### Semantics fixed in 2026-11 (each has a test in `t/bugfix_tests/`)

`t/t_bugfixes.sh` runs one small program per fixed bug; the first comment line
of each file states the expected output (`# EXPECT:`), a required failure
(`# EXPECT-STDERR:`) or a required compile error (`# COMPILE-ERROR:`). Add a
file there with every bug fix. Behaviours worth knowing when touching the
compiler:

- **`our` shadowing**: `our_vars` is keyed by name, so a parameter or `my`
  variable of the same name must blank the entry for its scope
  (`our_shadow_begin` / `our_shadow_restore`; `gen_block` and `gen_function`
  restore). Any new declaration form must call `our_shadow_begin`.
- **Bare `/re/` as a truth value** is `$_ =~ /re/`: the parser yields a bare
  regex as a string node flagged `is_regex` (what `split(/,/, …)` wants), and the
  AST constructors that take a condition route it through `ast_regex_truth`.
  `m/re/` is lexed where a regex may start.
- **List assignment** `($a, $b) = LIST;` becomes a `NODE_DESTRUCTURE` with
  `is_decl == 0` (`list_assign_as_destructure` in Parser.strada).
- **`local` in a bare or if/elsif/else block** is undone at the end of that
  block (`gen_block_local_scoped`); loops restore per iteration, the function
  exit restores the rest.
- **Method wrappers apply literal default parameters** (`wrapper_default_literal`):
  a dynamically dispatched call arrives with the argument missing.
- **Unknown builtin names** (`core::x`, `math::x`, `async::x`, `c::x`,
  `thread::x`, `utf8::x` that no handler claims) are a located compile error
  raised in `gen_user_call` (`unknown_builtin_name`), and `--check` now runs the
  code generator (output discarded) so it reports them too.
- **`&&` returns the deciding operand** (Perl): `1 && "v"` is `"v"`, `0 && x` is
  `0` -- compiled through `gen_short_circuit_or_op` with `!strada_to_bool`, the
  same path as `||`. It used to return 0/1.
- **`()` is an empty list**: the parser keeps it an empty `NODE_ANON_HASH` (so
  `my %h = ()` / `%h = ()` / `@a = ()` keep their dedicated paths) flagged
  `is_empty_list`; `ast_add_array_elem`/`ast_add_arg` drop it (`push(@a, ())`
  adds nothing, `(1, (), 2)` has 2 elements), as a value it is an empty array,
  Semantic turns `my $x = ()` into undef and `return ()` into undef / nothing /
  an empty hash by return type.
- **Mutating builtins autovivify `@{...}` targets** (`push`/`unshift`/`splice`):
  `push(@{$h{k}}, v)` creates the array, `push(@$u, v)` with `$u` undef stores
  a new array ref (`strada_autoviv_slot_array`), and a defined non-array target
  throws "Not an ARRAY reference" (`mutable_array_target_kind` /
  `gen_array_target` in CodeGenBuiltins). The static empty array/hash that
  failed dereferences return is replaced if anything ever got into it.
- **Filesystem paths containing a NUL byte are rejected** (`strada_path_buf`,
  `strada_path_pv`): every such call fails as if the file did not exist.
- **`core::spew` returns 1/0** (written fully / failed). `-e`/`file_exists` use
  stat (they used fopen: wrong for unreadable files, hung on FIFOs).
- **A method call on undef or a plain number throws** (catchable) instead of
  exiting the process.
- **Template**: names starting with `_`/`.` are private (TT's PRIVATE rule:
  not readable, callable or assignable from templates); `html` escapes `'`.
- **Compound assignment on an `int`/`num` variable with an overloaded operand**
  (`my int $n = 15; $n &= $obj;`) dispatches the overload, then coerces the
  result to the declared type (honoring `0+`), on every path: unboxed, boxed,
  and the VM (`vm_to_int_ov` / `vm_to_num_ov` in `OP_CAST_INT`/`OP_CAST_NUM`).
- **Tied containers**: `ref()`/`typeof()` of a tied scalar describe what `FETCH`
  returns; `clone()` copies tied arrays/hashes/scalars through
  `FETCHSIZE`/`FETCH`/`FIRSTKEY`/`NEXTKEY` into plain untied values; `splice`
  into a tied target reads a tied replacement array through `FETCH`. Tied
  `SPLICE` receives the replacement list flattened (`SPLICE($self, $off, $len,
  LIST...)`, as in Perl).
- **List flattening is decided statically** (`array_flatten_kind` in
  CodeGenExpr: 1 = `@var`/`@{...}`, 2 = owned list temp checked at run time
  with `strada_sv_is_array_or_aref`). It covers list literals, ranges, slices,
  `@a[@idx]`/`@h{@k}`, `push`/`unshift`/`join`/`sprintf`/`printf`/`pack`/
  `print`/`say` arguments (`emit_list_elem_append`), `reverse(LIST)`, `(LIST) x N`
  (only for an explicit comma list or `my @a = (x) x N`), and `map` block
  values (an arrayref value is kept as one element unless it is a list/pair).
  `qw(...)` and foreach lists are `is_list`.
- `scalar(X)` is the element count of an array or arrayref (Strada idiom),
  the key count of a hash, otherwise X (`strada_scalar_ctx`). `%h` in
  numeric context is its key count; `int($ref)` is the address (as `$ref+0`).
- List assignment: `($x) = @a` assigns the first element, element targets
  (`($h{a}, $a[1]) = ...`) work, `my $n = () = LIST` counts, and a
  non-lvalue target is a parse error.
- **Integer overflow promotes IV -> UV -> NV** (`+ - * ++ +=`, literals above
  INT64_MAX are UVs): `strada_int_add_ii`/`_sub_ii`/`_mul_ii` (header inline)
  and `strada_sv_add/sub/mul` with `strada_int_arith_slow` (exact via
  __int128, integer-looking strings stay exact). **`int`-declared storage is
  still int64 and wraps**: an int-typed `+ - *` assigned to an `int` variable
  goes through `emit_int_wrap_binop`; `/=`, `||=`, `&&=`, `//=` on an `int`
  variable truncate/coerce. num->int conversion saturates
  (`strada_num_to_i64_sat`); `int("1e3")` is 1000.
- Unary minus is `strada_negate` (Perl: ints stay exact, `-"foo"` is "-foo",
  `-"-foo"` is "+foo"); with an overloaded `-` and no `neg`, `-$obj` is
  `0 - $obj`. Numeric context of an object with only `""` overloaded numifies
  the string.
- **foreach** aliases array elements (the loop variable is written back each
  iteration and before `next`/`last`, `fe_writeback`), restores a
  pre-existing loop variable after the loop, and reads the live array length.
- Perl gaps implemented: magic string `++` ("az"++ is "ba"), string ranges
  ("a".."e", "x".."ab"), `ref(\$aref)` is "REF".
- `..` binds looser than `||`/`//` and tighter than `?:` (`parse_range` sits
  between `parse_ternary` and `parse_logical_or`).
- A bare `/re/` is a match against `$_` also in grep/map blocks, `my`
  initializers, `=` assignments and `return`.
- `$obj->name` without parens is a method call (it was the raw C `obj->name`);
  `LABEL: { ... }` is a once-loop (`last LABEL` works); `throw EXPR if COND;`;
  `Pkg::CONST`; enum values in hex/octal; `::f()`/`__PACKAGE__::f()` in
  package main; untyped `before`/`after` params; `async func ... void`.
- User functions named like any identifier the runtime/system headers declare
  are renamed in C (`c_reserved_idents` in CodeGen.strada, generated from
  `gcc -E` of strada_runtime.h + math.h).
- `Pkg::new(...)` on a package with no constructor builds a blessed hash
  (inlined when keys are literals, else `strada_generic_new`); an odd argument
  count (e.g. the inherited-ctor call `Kid::new("x")`) is a compile error.
- **References are always true** (a ref to an empty array/hash/object too,
  Perl). `@a`/`%h`/`@$r` in truth context (conditions, `!`, `&&`/`||`
  operands, `?:` conditions, grep blocks) are rewritten to `scalar(...)` by
  `ast_regex_truth`; value contexts (`return`, `=`, `my`, map blocks) use
  `ast_regex_value` (bare `/re/` only) -- never the truth form there, or
  `return @a` becomes a count.
- **Copies of weak refs are strong** (Perl): in a program that calls
  `weaken`, Semantic (`sem_strong_copy_walk`) wraps values copied out of
  variables/elements by `=`, `my` and push/unshift in `__strong_copy`
  (`strada_strong_copy_take`). `clone()` of a dead weak ref is undef;
  `weaken($h{"a\0b"})` is binary-safe (`strada_weaken_hv_entry_sv`).
- **Exception safety of builtin operands**: an owned operand must be on the
  cleanup stack while later operands are evaluated -- use `emit_arg_hold` /
  `emit_arg_release` (CodeGenBuiltins). `join` caps per-element guards at
  4096 (the pending-cleanup stack holds 16384). Hash insert/delete churn
  rehashes in place instead of doubling the index.
- `$_` in a module without `main()` is a weak C definition (it was undeclared).
- Assigning to a tied **parameter** (`func f($v) { $v = ...; }` called with a
  tied scalar) STOREs into the caller's tied scalar; rebind with a new `my`.
- **Strings (M2g)**: `split(" ", S, LIMIT)` with a nonzero LIMIT keeps a
  trailing empty field and awk mode splits on Unicode spaces; split of `""` is
  always `()`; the VM rewrites `/ /`, `/^/` and regex flags exactly like the
  compiled side (`compile_split_pattern`). `tr///` lists process escapes
  (`\t \n \- \\ \xHH \x{..}` octal) and the first mapping of a repeated
  character wins (`tr_decode_and_expand_set_ex`). Byte-mode `\x{NNNN}` expands
  to a grouped UTF-8 sequence so quantifiers apply to the character;
  `\p{^Name}` negates; patterns go through `strada_pcre2_compile_perl` on both
  backends. `\L\u`/`\U\l` in replacements behave as in Perl via a patch to
  the vendored `pcre2_substitute.c` (a system PCRE2 keeps its own behaviour).
  `substr` follows Perl's range rule (`strada_substr_span`: undef when the range
  is left of the string or starts past it; 4-arg/lvalue substr outside the
  string dies); lvalue substr is binary-safe (`strada_substr_assign_sv`) and
  the VM supports lvalue and 4-arg substr (`BUILTIN_SUBSTR_REPL`).
  `index($s, "", POS)` clamps POS to the length. `chomp`/`chop` are binary-safe
  (`strada_chomp_sv`/`strada_chop_sv`); chomp removes one trailing `\n` or
  `\r\n` (Strada keeps CRLF stripping) but never a lone `\r`; chop of a byte
  string removes one byte. sprintf `%hhd`/`%hd` truncate; the VM formats plain
  scalar arguments with the runtime's sprintf. `uc("ß")` is "SS",
  `ucfirst("ß")` "Ss", `lc("İ")` "i\x{307}", `uc("ς")` "Σ", `uc("ı")` "I".
  NFC/NFKC use the UAX #15 composition loop. `sb::to_string`, `quotemeta` and
  literal `s///` keep or gain the UTF-8 flag.
- **Builtins (M2h)**: pack/unpack support `j J F`, `@`, `<`/`>` on `f d`, the `!`
  modifier (`x!N`, `X!N`, signed `n! v! N! V!`), `%N` checksums, and `A`
  strips all trailing whitespace; `pack("u")` has no extra trailer line.
  `vec` reads numbers through their string form. `core::file_ext` looks at
  the last path component only; glob brace expansion is left-to-right and
  `{x}` expands. `setrlimit` takes a hashref. Method resolution / `isa` walk
  any inheritance depth or width (`oop_walk_ancestors`, cached `oop_mro`;
  VM `VM_INHERIT_MAX_DEPTH` 4096). Bare `hex()`/`oct()` are builtins unless a
  user function of that name exists (`strada_oct`: 0x/0b/0o). Array copies and
  `[@a]` turn holes into undef. `keys`/`each` return a byte-string key without
  the UTF-8 flag (`StradaHashEntry.key_bytes`, VM `VMHashEntry.key_bytes`).
  `dumper` prints numbers like stringification. `core::socket_send` flushes
  earlier `print`s to the socket first; VM `print`/`say` reach runtime
  sockets. `async::timeout` no longer finishes the future (a later `await`
  gets the result); `async::is_cancelled` is true once cancel was requested;
  at exit, unstarted pool tasks are cancelled and running ones get
  the exit wait (default 1000 ms; build option `--exit-task-wait-ms N`) before the process exits anyway
  (status kept, glibc `on_exit`). END blocks: program first, then import_lib
  libraries (`strada_register_end_block`). Export metadata carries parameter
  defaults (field 7: literals, or `f<fn>` naming a generated
  `<func>__dflt<N>` function for expression defaults) and `ownctor:` lines;
  `Pkg::new` devirtualizes only for constructors that bless into their own
  class (`fn_ctor_own_class`).
- **Lists (M4)**: `k => v` added to any list is two elements
  (`ast_add_array_elem` splices `is_pair` nodes, like `ast_add_arg`), so
  `(a => 1, b => 2)` / `[a => 1]` are flat; `%h` / `%$r` in a list literal,
  `push`, `join`, `printf`, a variadic call or a map/grep input contribute
  their key/value pairs (`hash_list_arg` + `emit_hash_pairs_append`; VM:
  `compile_list_src` -> `BUILTIN_HASH_FLAT`); `map {...} ("x")` iterates
  once (`ast_hash_list_input` wraps a parenthesized single item); a hash in
  string/numeric context is its key count. `Math::BigInt::div` floors (bdiv).
- **Closure calls (M4)**: arguments are evaluated before the callee (Perl), so
  `$f->(($f = sub {...}) ? 1 : 1)` calls the new code; calling a non-code
  value dies catchably: "Can't use an undefined value as a subroutine
  reference" (undef) / "Not a CODE reference" (`strada_not_code_call`; VM
  `L_do_op_call_closure`). It used to return undef silently.
- **Cannoli report fixes (2026-11-17)**: the inline call-entry paths
  (`strada_stack_push_il`/`strada_depth_push_il`) and `strada_epoll_wait` are
  signal dispatch points (handlers used to never run under Async::Loop);
  calls in a file without `main()` to functions only the host defines get
  `extern StradaValue* Pkg_f();` (Semantic `host_extern`, emitted by the
  auto-extern path) -- `--shared` plugins calling host functions crashed;
  `ssl::server(..., $backlog)` / `ssl::server_host($host, ...)`;
  `Async::Task::send(..., $timeout_ms)` waits with poll(2) outside a task;
  `core::initgroups`, `core::setgroups`, `core::unix_server`/`unix_client`;
  `core::setsockopt` passes a non-numeric value as raw bytes (packed structs).
- **Perl forms the parser now takes**: named unary operators without
  parentheses before a variable, `\` or literal (`delete $h{k}`, `exists`,
  `defined`, `keys %h`, `values`, `each`, `scalar @a`, `shift @a`, `ref`,
  `lc`/`uc`, `length`, ... -- `parser_is_named_unary`), parsed at Perl's
  named-unary precedence (`parse_shift`: `keys %h > 2` is keys(%h) > 2); the
  lexer reads `%` after `keys`/`values`/`each`/`delete`/`exists`/`scalar`/
  `defined` as a sigil unless the word is itself a variable name (`$keys % 4`).
  `while (my ($k, $v) = LIST)` loops while the list is non-empty (desugared to
  a hidden `@__wlN`); `EXPR for LIST;` / `EXPR foreach LIST;` is
  `foreach (LIST) { EXPR; }` (2026-11-19); `my [TYPE] $x` may appear anywhere
  in an if/unless/elsif/while/until condition (`while (defined(my $l =
  <$fh>))`): `parse_cond_expr` turns on `cond_decl_on`, parse_primary
  records the declaration and yields the variable, and `wrap_cond_decls`
  puts the statement in a block that declares it first (statement scope).
  Elsewhere `my` in an expression is still a parse error; a parenthesized group of pairs in a hash literal joins
  the hash (`parse_anon_hash_entry`), while `(EXPR) => v` stays a computed key.
- **Audit 20 final batch (2026-11-18)**: `func { ... }` / `fn { ... }` (no
  parameter list) take their arguments in `@_` and `func ($a, ...@rest)` is
  variadic (`parse_anon_param`, `parse_anon_func_args`,
  `strada_closure_set_variadic`); `my %t; my @t; my $t;` in one scope are
  three variables (Semantic keeps the later ones under `@:t` with C name
  `t__sga` etc.); `$#a` / `$#{$r}` / `$#$r` (lexer token `DOLLAR_HASH`,
  parsed as `scalar(@a) - 1`); a `num` function returning a parameter coerces
  it; typed `has int`/`has num` attributes are converted by the constructor
  and setter (`parser_attr_coerce`; `inline_ctor_needs_coerce` keeps such
  calls off the inline-constructor path); `Pkg::CONST` reads that package's
  constant (const decls carry `package`, the variable node `const_pkg`);
  callers set the call context for any function that calls
  `wantarray`/`wanthash` (`uses_wantarray`), not only `dynamic` ones;
  `foreach (f())` with a plain scalar result iterates once
  (`strada_foreach_list_take`; undef stays the empty list); bare `abs()` is
  `math::abs` unless a user `abs` is visible, and abs of an object with only
  `-` overloaded decides the sign with its `<`/`<=>` overload; the
  unblessed-method error reads "on unblessed reference" (Perl).
  `lib/DBI.strada` includes the generated `lib/strada_config.inc` (configured
  drivers as `#define`s), so `use DBI;` compiled from source has the drivers.
- **Loose ends closed (2026-11-18)**: a typed scalar declared without a
  value holds its type's empty value (`my int $x;` 0, `my num` 0, `my str`
  "") in both backends, and so does `our` (set only while the global is
  undef, so another `our` of the same variable keeps its value); `$u->{k} = v` / `$u->[i] = v` on an undef scalar
  autovivify it (`strada_autoviv_slot_ref` from `gen_assign_container`; VM
  `compile_viv_container` root case); `$h{k} = @a` stores the count;
  `stacktrace_str()` returns the tracked Strada frames. VM: `our @x` / `our %x`
  register as "@x" / "Pkg::@x" (`resolve_global_var_key_sig`; a bare-name
  lookup falls back to them) so they never collide with `our $x`; lib/Eval
  keys session variables the same way (`eval_vm_gkey`) and Strada::JIT keys
  them by sigil + name (`jit_vk*`); calling an `extern "C"` function in the
  VM throws a clear error (it returned undef); `strada_call_named_guarded` in
  the VM calls the VM's own function (`BUILTIN_CALL_NAMED_GUARDED`) -- through
  the runtime bridge it called the interpreter binary's compiled copy with a
  copied argument, so lib/Eval's parser looped forever under strada-interp.
- **`reverse` follows context (2026-11-18)**: Semantic `sem_rev_scalar` (VM:
  `vm_rev_scalar` in `vm_sc_pass`) marks `reverse(...)` calls in scalar
  context (`scalar_ctx`: scalar `my`/`our`/assignment/element targets,
  operands of binary/unary ops and `?:` conditions, `length`/`lc`/`uc`/
  `substr`/`index`/`scalar`/... arguments, `return` in a scalar-typed
  function): the arguments joined and the string reversed
  (`strada_reverse_scalar`, VM `BUILTIN_REV_JOINSTR`). Elsewhere it reverses
  the list; a single argument that is not an array/array ref is a
  one-element list (`strada_reverse_list1`, VM `BUILTIN_REV_LISTIFY`).
  The VM's `say(@a)` / `say(LIST-call)` print the elements (they printed the
  array ref).
- **`$_` is Perl's global topic (2026-11)**: one `$_` per OS thread and per
  green task, owned by the runtime (`strada_topic_tls`; generated C has
  `#define _ STRADA_TOPIC`, which is the TLS variable under gcc and
  `(*strada_topic_slot())` under tcc, which has no TLS). Coroutine switches
  swap it (`StradaCoro.topic_saved/host_topic`); a finished task's `$_` is
  dropped. `foreach (LIST)` / `foreach $_ (...)` assign the global per
  iteration (Semantic drops the loop's `var_decl` `_` so the existing-variable
  path runs) and save it with `strada_local_save_slot(&_)` on the local()
  stack, so last/next/return/throw restore it (`stmt_has_local` counts such
  loops). `map`/`grep` blocks that can call out (`stmt_has_call`) publish the
  element the same way per iteration; blocks without calls keep the C local
  (nothing else can see `$_`). `local $_` is a slot save of `_` (it used to
  localize the unread registry entry `main::_`). `_` is never a closure
  capture (`scalar_var_c_ref`). The implicit `@_` of a no-parens function is
  C-named `__args_` (as `_` it hid `$_`). VM: `main::_` lives in a per-VM
  hash (`vm_gh_for`, `VM.topic_globals`; each thread/task runs its own VM);
  foreach over `$_` and calling map/grep blocks use OP_SAVE_GLOBAL + the
  local-restore list; `$_` never falls back to a local `@_`.
- **Perl scalar context for list operators (2026-11)**: `keys`/`values`/`split`
  assigned to a scalar give their count -- `my`, `our`, file-scope
  declarations, `$x = ...` and element targets (`$h{n} = keys(%h)`)
  (`sem_is_count_in_scalar_call`, `sem_scalar_decl_count`; VM
  `vm_sc_is_count_call` in `vm_sc_pass`). `our $n = @a` / file-scope
  `my $n = @a` / `= map/grep` count too. To keep the list in a scalar write
  `my $ks = [keys(%h)]` (compiler/*.strada uses `my array @ks_klist =
  keys(...); my scalar $ks = \@ks_klist;`, which the frozen bootstrap also
  compiles). `[LIST]` with a flattening element is a reference (it was the
  bare array, so `is_ref` said 0). `reftype` looks one level only: a
  reference to a scalar holding a reference is "REF" (it looked through to
  "ARRAY"), so `is_refto(\$aref, "ARRAY")` is false. `refto($var)` is
  `\$var` (Semantic rewrite): writing through it changes `$var` (compiled
  code referred to a copy).
- `foreach ("a")` iterates once; `chomp;` / `$u = uc;` act on `$_`;
  `use Mod qw(a b);` parses; `tr///r` returns a copy; `"\x{E9}"` is one
  character; `core::fread($fh, $n)` / `core::fwrite($fh, $data, $n)` /
  `is_refto($v, "KIND")` honour their extra argument.
- **Examples parity (2026-11-18)**: `t/examples_parity.sh` compiles and runs
  every `examples/*.strada` and runs it through the VM; output and exit
  status must match. `t/examples_parity_skip.txt` lists the examples that
  cannot be compared, each with its reason (libraries, servers, network,
  refcount()/DESTROY timing, compiled-only `int_ptr`). `--jit-both` compiles
  each example once and compares it with the VM with the JIT off and on
  (counts are per run). Examples compile in parallel (`PARITY_JOBS`, default
  nproc) and run one at a time, since many use fixed `/tmp` paths. Fixes it drove:
  - `my $n = @{$r}` / `$n = @$r` (scalar variable target) is the count; the
    compiled backend aliased the array. Element targets (`$h{k} = @a`,
    `$h{k} = @{$r}`) still store the array in both backends.
  - `my @a = deref_array($r)` / `@a = ...` and the `deref_hash` forms copy
    like `@{$r}` / `%{$r}` (Semantic rewrites them; they aliased).
  - `nsort(...)` flattens into list arguments (`join(", ", nsort(@a))`).
  - `stacktrace()` prints the tracked Strada frames (`  func` per line,
    innermost first, as the VM) instead of a libc address backtrace.
  - VM: element stores autovivify missing intermediate containers
    (`$h{a}{b} = 1`, `$a[1][2] = 1`, `$r->{p}{q} = 1`, compound ops, `++`,
    `push(@{$h{a}{b}}, ...)`; `compile_viv_container`). An undef scalar
    root (`my $u; $u->{k} = 1`) is vivified by neither backend.
  - VM: `say` to stdout flushes (as compiled `strada_say`), so output stays
    in order with stderr, `system()` and forked children.
  - VM: a `__C__` block receives only the locals it names
    (`vm_code_names_ident`); marshalling every local back rebuilt referenced
    containers as copies (a second DBI::db object freed the handle twice).
  - VM: `my hash %h[N]` / `my array @a[N]` read the capacity expression node
    as an integer (its address) -- `vm_hash_new` looped forever; now a
    constant capacity is a hint (`vm_decl_capacity`), anything else ignored.
  - VM: `$s =~ $str` / `!~` with a string pattern, `core::sprintf`,
    `our int $x;` is 0 like `my int $x;` (2026-11; it was undef), `is_ref(@a)` / `is_ref(%h)` are 0, clone() caps
    at 128 nested containers (= the compiled 256 ref+container levels) with
    the same warning, `__C__` JIT adds `-I<runtime>/../interpreter` (vm.h).
- **Website-check fixes (2026-11-18)** -- bugs found while verifying the
  website's examples:
  - VM traces match the compiled ones. Each chunk has a line table
    (`ln_pc`/`ln_line`, recorded per statement by `vm_chunk_mark_line`) and a
    source file (`src_file`). The threaded-code decoder keeps `dt_pc`
    (instruction index -> bytecode offset), so `vm_frame_line` finds a frame's
    current line. `vm_build_trace` produces `  at NAME (FILE[:LINE])`.
    - `core::caller()` has the real file and line.
    - `core::stack_trace()` and `core::exception_trace()` use the `at` form;
      `stacktrace_str()` lists plain frame names.
    - The trace is captured in `L_vm_throw_exc`, so a `die` gets one too.
    - An uncaught `throw` prints `Uncaught exception: MSG`, then
      `Stack trace:` and the trace. An uncaught `die` prints only its message
      (`exc_from_die`).
    - A program that calls `caller`, `stack_trace` or `exception_trace` gets no
      VM tail calls (`g_vm_no_tail_calls`), so no frames are missing.
  - `typeof($r)`, `dump($r)` and `dumper($r)` treat an array/hash in a scalar
    expression as a reference, as compiled does (`vm_node_is_container`,
    `BUILTIN_TYPEOF_TO_REF`, `BUILTIN_DUMP_R`, `BUILTIN_DUMPER_STR_R`).
  - VM `say`/`print` with a non-handle first argument print it as data, and
    a `__C__` block may `return` a value.
  - VM: a body-less `extern func f(int $x) int;` naming a C function (from a
    `__C__` block or a `link_lib` library) works. The VM synthesizes a
    `__C__` block that declares the function with the C types the compiled
    backend uses (`vm_extern_func_cbody`), converts the arguments, calls it
    and converts the result.
  - VM: `core::int_ptr`/`num_ptr` raise an error (through the bridge they
    pointed into a temporary copy).
  - VM: `core::global_get/set/exists/delete("main::x")` find package main's
    `our $x` (`vm_global_key_main`).
  - VM compile errors that compiled already reports:
    - a constant defined twice in one package and file;
    - redeclaring a variable in the same block;
    - too many arguments to a function with a fixed parameter list
      (`VMChunk.arity_known`);
    - an odd number of plain arguments to `Pkg::new` on a class without a
      constructor (an inherited constructor is not found through `Pkg::new`).
  - Parser:
    - `&f(...)` / `&Pkg::f(...)` is a call, and bare `&f` calls `f` with the
      caller's `@_` (Perl). `\&f` is the code reference (it used to be bare
      `&f`, which compiled to a raw C function pointer and crashed).
    - `scalar EXPR` takes any operand (`scalar keys %h`, `scalar f()`).
    - A named unary operator can take another one as its operand without
      parentheses (`defined ref $x`, `uc lc $s`; `parser_next_is_named_unary`).
    - Perl's low-precedence `and`, `or`, `xor` and `not` work. They sit below
      assignment, in `parse_expression` → `parse_low_or` / `parse_low_and` /
      `parse_low_not`. They build the same `||`, `&&` and `!` nodes, so they
      return the deciding operand; `xor` gives 1 or "".
      - The words count only as `IDENT` tokens not followed by `=>`, so
        `(and => 1)` is still a pair.
      - A `my`/`our` initializer is parsed at assignment level. In
        `my $x = f() or die(...)` the rest of the chain becomes
        `$decl->{"low_tail"}`, an expression statement that `parse_block`
        adds after the declaration (`parse_low_tail`). At file scope that is
        a parse error.
      - Bare `&f` in a scope without `@_` passes nothing (Semantic,
        `amp_call`).
  - Bare `wantarray()`/`wanthash()` are the builtins unless a user function
    of that name exists (Semantic and the VM).
  - A local variable named like a package-main function (`my $cnt = cnt();`)
    gets the C name `NAME__fnv` (`sem_local_hides_func`); in C it hid the
    function. This is skipped in functions with a `__C__` block, which refer
    to locals by name.
  - The same top-level constant defined twice in one package and file is an
    error.
  - Labeled loops no longer cause gcc `-Wunused-label` warnings: each label
    gets a dummy `if (0) goto L;`.
  - Too few arguments to a builtin are a located Strada error
    (`gen_call_with_arg_cleanup`; `--check` reports it); they used to reach gcc.
  - Driver: after `--`, `-lNAME`/`-LDIR` are link flags. `strada --shared
    x.strada -- -lz` used to write a library named `-lz`. Any other `-...`
    positional is an error.
  - stradadoc renders `=item * text` as one bullet, and `=item 2.` keeps its
    number.
  - perl/Strada: `t/02_oop.t` and `t/03_exceptions.t` skip before `use_ok`
    (no "Bad plan").
  - A slice in scalar context is its last element (Perl): `"" . @a[0]` is
    `$a[0]`. It was the count compiled and `ARRAY(0x..)` in the VM. Semantic
    `sem_rev_scalar` and VM `vm_rev_scalar` mark the slice `scalar_ctx`;
    codegen wraps it in `strada_list_last_take`, and the VM emits
    `BUILTIN_LIST_LAST`.
  - Forma:
    - `{{#if (helper args)}}` / `{{#unless (...)}}` test a helper call's
      output (`cond_value`).
    - `{{repeat "x" var}}` resolves a variable count.
    - `currency` rounds JPY/KRW to whole amounts.
  - `import_object` / `import_lib` / `import_archive` "cannot find" errors
    point at the filename (`parser_error_at`).
  - **Benchmark regressions vs the 2026-06-19 build, fixed.** Measured by
    building `efa7521` in a worktree and timing both builds interleaved,
    best of 5 (`bench_*`).
    - `fused_arg_pure`: the fused-constructor path accepts integer `+ - *`
      and `%` by a nonzero literal (bench_oop was 4.3x slower).
    - `hca_rhs_pure`: the self-append `$s = $s . X` and the single-lookup
      hash `op=` accept element reads and builtins that cannot throw
      (`chr`/`ord`/`length`/`uc`/`lc`/`ucfirst`/`lcfirst`).
    - `ast_hash_list_input` leaves a parenthesized range alone, so
      `map {...} (1..N)` uses the lazy loop again.
    - Literal `s///` has a fast path for plain strings, and short needles
      use `strada_memmem_short`.
    - `tr///` caches its width probe and expanded byte lists per thread.
    - `<$fh>` uses `getline()` for the default `$/` with no limit.
    - `strada_sv_add`/`sub`/`mul` have an always-inline fast path for two
      tagged ints.
    - The remaining gaps (1.1-1.6x on oop, exceptions, closures and
      array_hash) come from this cycle's semantics work: overflow
      promotion, shared closure cells, exact recursion tracking, pooled
      objects releasing their values, and index shrinking.
  - `t/t_bugfixes.sh`: a COMPILE-ERROR test may carry
    `# VM-COMPILE-ERROR: text`, and then strada-interp must reject it too.
  - `interpreter/test/run_compiler_tests.sh` runs `test_stacktrace` (the traces
    now match) and skips `test_ffi` (it uses `core::int_ptr`). It still gives
    108 pass / 12 skip.

### Stack Traces and Introspection

- Uncaught exceptions automatically print stack traces
- `core::stack_trace()` - Manual stack trace as string
- `core::caller()` - Returns hash with `function`, `file`, `line` of calling frame
- `core::caller($level)` - Optional level: 0 = immediate caller, 1 = caller's caller, etc.
- `--no-stack-trace` flag to disable for performance
- `core::set_recursion_limit($n)` - Deep recursion protection (default: 1000). Part of stack-trace tracking, so only active in programs that use try/throw/caller/stack_trace (or `--stack-trace`). Calls deeper than the 256-entry frame array are counted (`strada_call_overflow`) rather than recorded, so the limit is exact; a try frame saves the logical depth (`strada_stack_mark`) and a throw restores it.

```strada
my scalar $info = core::caller();
say($info->{"function"});  # caller's function name
say($info->{"file"});      # source file
say($info->{"line"});      # line number
my scalar $grandparent = core::caller(1);  # one level up
```

### Binary Data

```strada
my str $packed = core::pack("NnC", 0x12345678, 80, 255);
my array @parts = core::unpack("NnC", $packed);
core::ord_byte($str);  core::get_byte($str, $pos);  core::byte_length($str);
my str $b64 = core::base64_encode($data);
my str $raw = core::byte(255);   # single raw byte 0xFF (length 1)
```

**`chr()` vs `core::byte()`**: `chr(n)` is **codepoint-oriented** — `chr(255)`
is the character U+00FF, which is 2 bytes in UTF-8 (`0xC3 0xBF`), and `chr(256+)`
is a wide character. `core::byte(n)` is **byte-oriented** — the single raw byte
`n & 0xFF`, never UTF-8 encoded. Build binary data (for base64 / pack / sockets
/ files) from `core::byte()`, not `chr()`; otherwise high bytes expand to
multi-byte UTF-8 before the binary sink sees them. `get_byte`/`ord` read either
back.

### Performance Hints

```strada
my array @data[1000];           # Pre-allocate array capacity
my hash %cache[500];            # Pre-allocate hash capacity
core::hash_default_capacity(1000);
reserve(@arr, $n);              # core::array_reserve(@arr, $n) is an alias
```

**Runtime optimizations** (automatic, no code changes needed):
- **Tagged integers**: Integers encoded in pointers — zero heap allocation
- **Inline accessor calls**: Zero-arg `has` accessors bypass method dispatch
- **Int arithmetic**: `expr_is_int_typed()` detects int-typed expressions → `STRADA_MAKE_TAGGED_INT()` instead of `strada_new_num()`. Applies to int literals, int variables, int return types, `+=`/`-=` on int vars, and int comparisons in conditions
- **Single-lookup hash compound-assign**: `$h{k} op= rhs` (`+=`, `-=`, `.=`, `/=`) compiles to one `strada_hv_compound{,_sv,_ph}()` call — one hash probe and an in-place slot update instead of the old fetch_owned + store pair (two probes, key hashed twice, incref/decref of the old value). ASCII literal keys pass a compile-time djb2 hash (`_ph`). A fresh key's first `+=` stays a tagged int (undef treated as int 0 when the RHS is tagged), and `.=` appends via `strada_concat_inplace` on the slot — O(appended bytes). Tied/key-locked hashes fall back to FETCH/compute/STORE inside the runtime so handlers and lock checks still fire. Array element compound-assign (`gen_array_compound_assign`) still uses the get/set pair with the int-preserving tagged check
- **Int parameter skip**: `int` params skip incref/decref/cleanup entirely (tagged ints are immortal)
- **Inline constructors**: `Pkg::new(...)` compiles to direct hash construction, including inherited classes
- **Range map/grep → native C loops**: `map {...} (A..B)` / `grep {...} (A..B)` with int-typed bounds iterate natively — no input-array materialization (`grep {...} (1..1e6)` no longer builds a million-element array). `$_` is a tagged int per iteration. Mirrors the foreach range path
- **Range foreach → native C loop**: `foreach my $i (START..END)` with int-typed bounds (per `expr_is_int_typed`) compiles to `for (int64_t ...)` — no `strada_range` array materialization, no per-iteration `array_get`. The loop variable is rebuilt per iteration with `strada_new_int` (tagged for the practical range) and registered int-typed for the body. Bounds evaluate once; `5..1` iterates zero times (matches `strada_range`). Non-int ranges and non-range lists use the array path
- **Method dispatch cache**: `strada_method_call` resolves via a 64-entry direct-mapped inline cache keyed by (blessed-pkg pointer, method-name hash), invalidated by a generation counter bumped on method/inherit/modifier registration. Literal method names compile to `strada_method_call_ph(obj, "name", args, HASHu)` with a compile-time djb2 hash (ASCII names only), skipping the per-call strlen+hash. Each entry also caches whether the method has any `before`/`after`/`around` modifier in its MRO, so programs that use hooks somewhere don't pay an MRO walk on every unrelated method call
- **Per-call-site monomorphic dispatch cache**: each literal-name method call site additionally gets a function-scope `static StradaCallSite` and calls `strada_method_call_cs(obj, "name", args, HASHu, &site)`. When the receiver's blessed class matches the site's last dispatch (and the method has no modifiers), dispatch is two compares + an indirect call — no global cache probe, no name strcmp. Same generation-counter invalidation as the global cache; polymorphic sites just refill. `isa`/`can` are excluded (intercepted as UNIVERSAL methods before lookup); dynamic `$obj->$name()` keeps using `strada_method_call`
- **Per-package method index + cached flattened MRO**: each `OopPackage` keeps an open-addressed hash over its methods (rebuilt on registration) and a lazily-built flattened MRO validated by the same generation counter, so cache-miss resolution, `can()`, AUTOLOAD probing, and operator-overload lookup walk the flat MRO with one hash probe per package instead of recursive O(methods × depth) strcmp scans. Resolution order (depth-first left-to-right, deduped) is unchanged. Works across `import_lib` boundaries: shared libraries don't bundle the runtime, so there is one registry and one generation counter — a lazily-loaded .so registering methods or modifiers invalidates the host's warmed caches (covered by `t/test_import_lib_hooks.strada`)
- **Concat ASCII-flag propagation**: `strada_concat_sv`/`strada_concat_cstr_sv` compute the result's ASCII flag (bit 63) from the operands' existing flags instead of rescanning the combined buffer; only overload-stringified operands are scanned (their own bytes only)
- **Zero-copy `keys()`/`each()`**: key result SVs share the hash key's `StradaString` (`strada_new_str_share_ss` — ss_incref, no byte copy) instead of strdup'ing every key. **COW rule for runtime hackers**: any code that mutates string bytes through an existing `StradaString` (rather than swapping in a fresh one) MUST first check `SS_FROM_PV(pv)->refcount == 1` and clone when shared — the three in-place writers (`strada_concat_inplace`, `strada_concat_inplace_cstr`, `strada_vec_set`) all do this
- **`join()` via `strada_join_sv`**: codegen passes the separator SV directly (no stringify) and the runtime borrows plain-STR elements (no per-element copy) and builds straight into the result `StradaString` (no assemble-then-recopy). ASCII/UTF-8 flags propagate from the parts
- **Decorated string sort**: the default (string) `sort` pre-stringifies non-STR elements once into a shadow array (decorate-sort-undecorate) instead of converting inside the comparator (O(n log n) conversions); all-STR arrays skip decoration. `sort %h` flattens by walking hash entries directly (zero-copy keys, no re-probe)
- **Reusable pcre2 match data**: a one-slot, thread-local cache (`strada_md_acquire`/`strada_md_release`) holding the largest ovector seen replaces per-match `pcre2_match_data` create/free; nested matches (a regex inside an `s///e` callback) fall back to a fresh allocation via the busy flag
- **Condition-level hash-fetch CSE**: `if ($h{"k"} > 0 && $h{"k"} < 100)` compiles to ONE `strada_hv_fetch_owned_ph` in a temp (`emit_condition_cse`). Applies to statement conditions (if/elsif/while/until/do-while/for) when the condition tree is pure (no calls/assignments/increments/regex) and the program never uses `tie` (Semantic sets `uses_tie` on the program node — a tied FETCH is observable per-mention, so any tie disables this program-wide). The hoisted fetch runs even when `&&` short-circuits — safe for untied hashes (reads have no side effects and don't autovivify)
- **Multi-part concat flattening**: `.` chains of ≥3 leaf parts (string interpolation desugars to these) compile to one `strada_concat_multi(n, kind, payload, ...)` call — sizing pass + single exactly-sized StradaString instead of a concat call per part. Part stringification mirrors `strada_concat_sv` exactly (keep them in sync); overloaded-`.`/`""` programs fall back to the pairwise dispatching path for possibly-blessed parts; owned parts use cleanup-push capture (throw-safe). Codegen: `gen_concat_multi` in CodeGenExpr.strada
- **`ord("literal")` constant-folds**: a single ASCII string-literal argument rewrites the call node to an int literal in the semantic pass (Semantic.strada, NODE_CALL branch) — so character-code comparisons can be written readably as `$code == ord("#")` and still compile to plain integer compares with all int-literal optimizations. Multi-byte (UTF-8) literals and non-literal args keep the runtime `ord()`. The lexer's hot loops are written in this style (numeric codes remain only for the EOF sentinel `0` and the `128` ASCII boundary)
- **Direct closure calls**: `$f->(...)` with up to 4 plain arguments calls `strada_closure_call0..4`, which jumps straight to the closure's C function when its parameter count matches (fallback: the general argv path). The callee is not copied when it is a plain local no argument assigns, and in programs without `try` owned argument temporaries are released directly instead of through a mortal scope
- **Leaf methods skip parameter refcounting**: a body made only of arithmetic, comparisons, literals, untied element reads and `$self` accessor calls (`node_is_leaf_safe`) takes its parameters without incref/decref. A `return` with no scope cleanup emits no `strada_inflight_push`/`pop`
- **num arithmetic in doubles (2026-11-23/24)**: where a double is wanted (`emit_num_operand`: a `num` variable's initializer, an operand of num arithmetic, a comparison), `/` is `strada_num_div` (header inline, same zero check as `strada_safe_div`) and `**` is `pow()`: no boxed intermediate. `expr_is_num_typed` says an expression is statically a double (num literal/variable, num accessor, `+ - * /` over those and int-typed operands)
- **num attributes (same date)**: a zero-argument call to an inlinable `has num` accessor (`is_inline_num_accessor_call`) reads the slot as a double; `+ - *` over such reads is one double expression with one boxed result; an inlined setter whose argument is num-typed writes the double into the slot's existing value when nothing else refers to it (refcount 1, no meta, and no weak reference exists anywhere in the program: `weak_registry_has_entries`, since a weakened reference to the old number must go undef), else stores a new value (`strada_hv_store_num_ph_len`)
- **`foreach my int $v (@a)` with a C integer loop variable (same date)**: when the body never assigns `$v` and `pf_walk_node` finds nothing that needs a boxed value (closure capture, `\$v`, `__C__` block, mutating builtin) and the function has no `try`/`goto` (`foreach_int_var_unboxable`), `$v` is an `int64_t` read with `strada_array_int_at` (tagged ints untag inline; anything else goes through `strada_to_int`). Applies to any array, not only `array<int>`
- **Attribute slot cache (same date)**: every inlined accessor read and setter store has a function-scope `static StradaAttrSite` (interned key string, entry index, back-off counter). `strada_attr_entry` hits when `entries[idx].key` is the interned key string (pointer compare; `idx < next_slot`, hash not tied or key-locked): that position then holds the attribute in whatever hash it is, so there is no probe and no key compare, and nothing needs invalidating. A miss takes the ordinary path and may update the index (`strada_attr_site_learn`); a read never changes the object, and a site that keeps missing backs off (1, 3, 7 ... 255 misses between attempts, `fails`). A setter that finds the attribute absent from a plain hash inserts it under the interned key (`strada_attr_store_miss`). Keys are interned by the inline constructors (as before) and, for every other way of building an object, by `strada_bless`: classes register their `has` attributes at init (`strada_class_attr_register`), and bless swaps the object's matching keys for the interned strings (own and inherited attributes; skipped while no class has registered any). Objects stay ordinary hashes: `$obj->{k}`, `keys`, `clone`, hand-written constructors and the VM are unaffected. Stores of a reference, array or hash take the ordinary store (cycle bookkeeping). `strada_hv_unwrap_meta_sv` has a no-loop path for a hash or one reference to it. Tests: `t/bugfix_tests/num_div_unboxed.strada`, `num_attr_typed.strada`, `foreach_int_unboxed.strada`, `attr_slot_cache.strada`, `attr_slot_cache_threads.strada`. Against the commit before the cache (interleaved, best of 5): 22 benchmarks within 0.02 s either way (binary_trees and closures identical in instructions), the object benchmark 0.52 s to 0.42 s, a bless-then-fill class (keys not interned at bless) 0.11 s to 0.09 s
- **LTO at -O2+**: `-flto` at `-O2`+, `-march=native` at `-O3`/`-Ofast`

### Compiler Flags

```bash
./strada -w input.strada        # Enable warnings (unused variables)
./strada --strict-types input.strada  # Stage-0 type warnings (see below)
./stradac -t input.strada out.c # Show compilation phase timing
./strada -p input.strada        # Function-level profiling (timing to stderr)
./strada --full-profile input.strada  # Line-level profiling (writes strada-prof.out)
./strada -O2 input.strada       # Optimization with LTO (-flto)
./strada -O3 input.strada       # LTO + -march=native
./strada -Ofast input.strada    # Aggressive optimization + LTO + -march=native
```

### Diagnostics Format (2026-08)

All compiler diagnostics carry a source location:

- **Lexer errors**: `file:line: msg` (`lex_error` in Lexer.strada), with
  `file:line:col` where a column is known (unexpected character; token-start
  column). Unterminated string/comment errors point at the opening line, not
  EOF. `lex_tokenize_file(source, filename)` is the filename-carrying entry
  point; bare `lex_tokenize` (synthetic sources, e.g. `s///e` replacement
  re-lexing) falls back to `line N: msg`.
- **Parse errors**: `file:line: msg` (`parser_error`). Module sub-parsers get
  their filename set in `load_module`, so errors in `use`'d modules name the
  module's own path.
- **Semantic errors/warnings**: `file:line: error|warning: msg`. Function AST
  nodes are stamped with a `file` at module-merge time (module path) and at
  the end of `parse_program` (main file / synthesized functions);
  `semantic_error`/`semantic_warn` take `$ctx` and read `$ctx->{"cur_file"}`,
  set per-function in both semantic passes. Nodes without a file fall back to
  the old `error: msg at line N` format.

**Preprocessor (2026-11)**: the lexer handles every directive itself,
with or without `-D`, in every file it reads: `#if/#ifdef/#ifndef/#elif/#else/
#endif/#define/#undef`, plus `#include "file"` / `#include <file>`, `#error`
and `#warning`. An included file is tokenized as its own source and its tokens
are spliced in (`lex_pp_include`), so its diagnostics name it and keep its line
numbers; tokens carry `file` and `parse_function` stamps it on functions;
`#define`s made in it stay in effect after it. `"file"` is looked up next to
the including file, then in `STRADA_INCLUDE_PATH` (the driver sets it from
`-I`). With `-D` the driver still runs `stradapp`, now with `--defer-include`
so it leaves `#include/#error/#warning` lines for the lexer (no line shift).
`#else`/`#elif` after `#else` is an error in both, and `#define NAME=VALUE`
means NAME is VALUE in both. Interpolated `${...}` expressions are located on
their own line, also inside heredocs.

**Multi-error reporting (2026-08)**: one compile reports every error, not
just the first.

- **Semantic**: `semantic_error` accumulates into `$ctx->{"errors"}` and
  RETURNS — analysis continues, all errors print at the end (capped at 25
  shown), then one `die("compilation failed: N semantic errors")`. Every
  error site must therefore be fall-through-safe: an error may suppress
  behavior but must never leave state that crashes later analysis.
- **Parse recovery (function boundary)**: `parse_program`'s function branch
  calls `parse_function_guarded`, which invokes `parse_function` through
  **`strada_call_named_guarded(name, arg)`** — a runtime builtin that dlsyms
  the process's own symbol (stradac's functions are non-static globals in a
  `-rdynamic` binary) and calls it under a hand-rolled try frame, returning
  `[1, result]` / `[0, errmsg]` / `[-1]` (symbol unavailable → caller falls
  back to a direct fatal call). On error: recorded, `parser_resync_toplevel`
  skips to the next top-level keyword at relative brace depth <= 0, parsing
  continues (capped at 20). All recovered errors print at end of
  `parse_program`, then one die — semantic analysis never runs on a partial
  AST. Config-level items (`use`/`import_*`) stay fatal-on-first by design.
  This runtime-assisted shape exists because the frozen bootstrap compiles
  NONE of: `try/catch`, `__C__` blocks, `extern func` declarations, `\&func`,
  or `undef`-initializers — a bare unknown builtin call (emitted by the
  bootstrap as a direct C call) is the one mechanism that works everywhere.
  Like all new compiler-callable runtime builtins (see
  `strada_read_meta_section`), it is wired in Semantic (`$b{...}`),
  CodeGenBuiltins (both dispatch sites), CodeGen `owned_set`, both runtime
  headers — and requires a full `make`, never `make quick`.
- **`--check`** (stradac + driver): syntax/semantic validation only — stops
  after `semantic_analyze`, writes no C, runs no cc. `<output.c>` optional.
  Exit 0 clean / non-zero with located errors. The editor-integration hook.

**Columns + caret snippets (2026-08)**: errors render gcc-style — location,
the offending source line, and a caret:

```
app.strada:2:19: Unexpected character: `
    my int $a = 3 ` 4;
                  ^
```

- **Token columns are stamped centrally**: `lex_next_token` records the
  token-start column on the lexer (`tok_col`, captured after whitespace
  skip), and `lex_tokenize_file`'s loop stamps `$token->{"column"}` — one
  site, not ~55 creation sites. In-token readers (string/regex/qw/__C__)
  pass `$lexer->{"tok_col"}` to `lex_error` for the opener's column.
- **Snippet helpers live in Lexer.strada** (`diag_snippet`,
  `diag_snippet_from_file`, `diag_line_from_source`, `diag_caret_pad`) —
  char_at byte-walk, error-path only, tab-preserving caret pad. The lexer
  renders from its in-memory source; parser and semantic re-slurp by
  filename (skipped for synthetic/unreadable sources). Semantic errors show
  the line without a caret (AST nodes carry line, not column); warnings
  stay single-line by design (`-w` output stays compact).
- `parser_error` emits `file:line:col:` via `parser_current_col` (current
  token's stamped column).

Anything matching diagnostics must accept the `file:line[:col]:` prefix (a
`^warning:` anchor broke when this landed — see `t/t_core.sh`'s strict-types
count). Regression tests: `t/diagnostics_test/run.sh` (19 cases: located
lexer/parser/semantic errors incl. exact columns and caret alignment,
module-file attribution, multi-error, parse recovery, --check).

**Editor support** (`editors/`, 2026-08): Vim runtime tree
(`editors/vim/{syntax,ftdetect,ftplugin,indent}/strada.vim` — highlighting
incl. embedded C in `__C__` blocks, `:make` = `strada --check %` with an
errorformat that parses the located diagnostics and drops snippet lines)
and Emacs `editors/emacs/strada-mode.el` (`strada-mode`, `C-c C-c` =
`strada --check` in compilation-mode — the gcc-style errors match its
built-in GNU pattern). Install instructions in `editors/README.md`. When
diagnostic FORMATS change, update the vim `errorformat` and both READMEs.

**Tree-sitter grammar** (`editors/tree-sitter-strada/`, 2026-08): full
grammar + external scanner (`src/scanner.c`: heredoc bodies via a FIFO
tag queue with serialized state, raw `__C__`/`extern "C"` bodies with
C-string/comment-aware brace counting, column-0 POD) + highlight/injection
queries. Parses the ENTIRE repo corpus with zero error nodes (245
examples, 66 lib modules, compiler sources) — keep it that way: after
language syntax changes, re-run the corpus sweep (`npx tree-sitter-cli
parse --quiet` over examples/ + lib/). `src/parser.c` is generated
(`npx tree-sitter-cli generate` from grammar.js) but committed. Scanner
gotchas learned the hard way: external scanners run BEFORE internal
whitespace skipping (skip leading ws yourself, with skip=true); error
recovery marks ALL external tokens valid (bail or c_block_body eats the
file); string internals must be token.immediate AND higher lexical prec
than comment tokens (or a literal "/*" in a string loses the lexing race
and the comment swallows the closing quote).

### Full Profiling (Line-Level)

```bash
./strada --full-profile myprogram.strada && ./myprogram   # Writes strada-prof.out
strada-proftext strada-prof.out                            # Text report
strada-profhtml strada-prof.out profhtml/                  # HTML report
```

Programmatic API: `core::full_profile_start("file.prof")` / `core::full_profile_stop()`. The `-p` flag is simpler function-level timing to stderr.

### JIT Library

```strada
use lib "lib";  use Strada::JIT;
Strada::JIT::init();
my scalar $r = Strada::JIT::eval("1 + 2");  # Runtime eval, returns 3
```

**Strada::JIT notes (Audit 20 V5)**: with tcc the generated
`__strada_init_globals` is made non-static and called after dlopen (tcc drops
`__attribute__((constructor))`, so methods/inheritance were never registered).
Top-level `our` declarations are recorded (`register_our_decls`) and
re-declared at package level in every later snippet. Named funcs after other
statements are hoisted (`jit_hoist_named_funcs`); function/brace extraction
runs on `strip_strings_and_comments()` output (heredoc bodies, strings,
regexes blanked). A redeclared tracked `my` of the same type is restored
first, so `my $n = $n * 2` sees the old `$n`. `reset()` no longer
dlcloses: closures from earlier evals stay callable. `is_balanced()` is 0
for input ending in a binary operator. Known limit: `@x` and `$x` are one
variable (the compiler's symbol table is keyed by name, not sigil).

**lib/Eval (Strada::Interpreter) notes (Audit 20 V5)**: every eval runs in a
fresh VM seeded from / exported to the session store `%g_eval_vm_globals`.
`vm_to_sv`/`sv_to_vm` (interpreter/vm.c) memoize by identity
(`vm_conv_begin/end` widen one memo over all globals + the result), so
aliases, cycles and shared DAGs survive (no depth cap; linear time); nested
containers convert to REFs and results/exceptions to scalars
(`vm_value_to_sv_scalar`). A blessed ref whose class the VM program defines
converts back to a VM object. Closures/methods of an earlier eval's VM run
through `vm_foreign_call`, with `vm_foreign_vm_call_hook` syncing that VM
with the store (generation `eval_sync_gen`, epoch bumped by `reset()`).
Eval-defined classes get host `strada_inherit` + an `AUTOLOAD` trampoline
into the latest eval VM (typed catch, `$obj->method()` from the host; `can()`
does not see them). Tokens are renumbered to the user's lines
(`eval_sb_*`), and VM compile errors (stderr) are captured into the
exception. The session mutex is taken with `strada_gc_mutex_lock` (runtime:
GC-safe while waiting; a waiter used to stall every stop-the-world
collection). A redeclaration with an initializer is seeded (old value
visible in the initializer); one without starts undef. Tests:
`t/audit_r20/eval_v5.strada`, `t/audit_r20/jit_v5.strada`,
`t/leak_tests/test_audit_r20_eval_jit_leaks.strada`.

### Heredocs

```strada
my str $text = <<EOT;
This is a multi-line
string literal.
EOT

my str $raw = <<'EOT';
No escape sequences: \n stays literal.
EOT

my str $interp = <<"EOT";
Same as bare <<EOT (double-quoted).
EOT
```

Semicolon goes on the `<<EOT;` line. Heredocs follow Perl exactly (2026-11):
- **Every body line keeps its newline**, the last one too: `<<EOT` with the
  single line `hi` is `"hi\n"`. Before, the final newline was dropped.
- **Forms**:
  - `<<EOT` and `<<"EOT"` process escapes and `${name}` interpolation.
  - `<<'EOT'` and `<<\EOT` are literal.
  - `<< "EOT"` may have spaces before a quoted delimiter, and a quoted
    delimiter may contain any characters (`<<"END OF TEXT"`). `<<""` ends at
    the first empty line.
  - `<<~EOT` (and `<<~"EOT"` / `<<~'EOT'`) is indented: the terminator may be
    indented, and its indentation is removed from every body line. A
    non-blank line without that indentation is a compile error ("Indentation
    on line N of here-doc doesn't match delimiter").
- The lexer (`compiler/Lexer.strada`, the `<<` branch of `lex_next_token`),
  its preprocessor skip scanner, the REPL's `is_balanced`
  (`interpreter/Main.strada`) and Strada::JIT's `strip_strings_and_comments`
  all recognize these forms.

### Perl Compatibility

- **`$_` default**: `chomp()`, `uc()`, `lc()`, `length()`, `ucfirst()`, `lcfirst()`, `trim()`, `defined()`, `ref()`, `chr()`, `ord()`, `say()`, `print()`, `chop()` all default to `$_` with no args
- **Bare `shift`/`pop`**: Default to `@_` — `func process { my $first = shift; }`
- **`chomp($s)`**: Strips trailing `\n`/`\r\n` in-place
- **Autovivification**: `$h{"a"}{"b"}{"c"} = 42;` auto-creates intermediates
- **`splice`**, **`each`**, **`select`**, **`reverse`**, **`isa`/`can`**, **`core::signal`**, **`goto`** — all supported

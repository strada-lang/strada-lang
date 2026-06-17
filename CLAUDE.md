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
make quick                  # Fast compiler edit/test loop (~30s vs ~7min) — see below
make DEV=1                  # Build everything at -O0 (fast compile, slower binaries)
make libs                   # Build libraries (DBI, crypt, ssl, readline)
make tools                  # Build tools (stradadoc, strada-soinfo, strada-proftext, strada-profhtml, etc.)
make run PROG=test_simple   # Compile and run an example program
make test                   # Test runtime system
make test-selfhost          # Verify self-compilation (stage 2)
make test-suite             # Run comprehensive test suite (148 tests)
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

Note also that the stage-1 compiler is throwaway (its only job is to regenerate
`Combined.c`), so it is now built at `-O0` (`FAST_CFLAGS`) regardless of `DEV`,
which roughly halves a clean `make` with no effect on the final compiler.

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
- `int`-declared variables hold canonical integers: initializing or assigning a value the compiler can't prove int-typed coerces it (`strada_to_int` semantics — `my int $x = "12abc"` makes $x the integer 12), and `int`-returning functions coerce non-int return values the same way. Parameters are the exception (borrowed values, coercion would cost every call; they convert at use sites instead).
- Type annotations are optional in `my`/`our` declarations and function signatures. The sigil determines the default: `$` → `scalar`, `@` → `array`, `%` → `hash`. Return types default to `scalar` if omitted.
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
make test-suite             # Run comprehensive test suite (148 tests)
./t/run_tests.sh -v string  # Run tests matching "string" with verbose output
```

## Test Suite

Located in `t/`:

```bash
./t/run_tests.sh            # Run all 148 tests
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

A Bacon–Rajan synchronous trial-deletion collector that automatically reclaims **indirect** reference cycles (`A→B→A`) which plain refcounting leaks — so `core::weaken()` is no longer required for the common hashref/arrayref/object-graph cycle. Implemented entirely in `runtime/strada_runtime.c` under `#ifdef STRADA_CYCLE_GC` using side tables (no `StradaValue` ABI change). Key properties:

- **Hook**: only the survive path (`new_rc > 0`) of `strada_decref` buffers a candidate root; drop-to-zero is unchanged. Buffered roots are **pinned** (incref'd) so they can't be freed underneath the collector.
- **Scope**: covers `ARRAY`/`HASH`/`REF` roots. Closure-capture cycles, futures/channels, and tied containers fall back to `core::weaken()`.
- **Threading**: works under threads via a **stop-the-world** pause. Mutators poll a safepoint in the threaded `strada_decref` path (after buffering, never before — parking before the buffer read would let the collector free the candidate underneath it); threads blocked in a syscall (sleep/recv/await/join/pool-idle) mark themselves safe via `cc_blocking_enter/leave` so the collector needn't wait for them. The collector requests a stop, waits (bounded by a 200ms timeout → a thread that can't reach a safepoint just causes a *skipped* collection, never a hang) for every other registered mutator to park/block, then runs the trial-deletion with the graph stable. Single-threaded keeps the original lock-free fast path. The candidate buffer + side table are mutex-protected only while threading is active.
- **Triggers**: automatically at a candidate-count threshold (default 10000, **adaptive**: a collection that frees nothing doubles the trigger, capped at ~1M candidates; one that frees cycles resets it to the base — so cycle-free programs converge to near-zero collection work) and a final sweep at exit; or explicitly via the API below. `core::gc_threshold($n)` sets the base and resets the current trigger.
- **Caveat**: a cyclic *blessed* object reclaimed by the collector does **not** run `DESTROY` (parallels Perl global destruction).
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

### Leak Test Suite

Run leak tests with: `./t/leak_tests/run_leak_tests.sh` (117 tests, incl. `test_threads_cycles` — multithreaded cycle collection)

## IMPORTANT: Documentation Updates

**When making changes to Strada, ALWAYS update these documentation files:**

1. **`CLAUDE.md`** (this file)
2. **`CLAUDE_DEEP_DIVE.md`** - AST nodes, code gen patterns

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

## Standard Library Additions (2026-06)

- **`lib/Test.strada`** — TAP-emitting test framework: `Test::ok/nok/is/isnt/is_num/like/unlike/pass/fail/skip/diag/plan`, `Test::done_testing()` returns the exit code. Runs under any TAP harness (`prove`).
- **`lib/List.strada`** — List::Util-style helpers: `reduce/any/all/none/first/sum/product/min/max/minstr/maxstr/uniq/zip/pairs`.
- **`lib/Exception.strada`** — chainable exceptions (see Error chaining above).
- **`lib/Async/Scope.strada`**, **`lib/Async/Actor.strada`** — structured concurrency + actors (see Async/Await).

## Conversion Tools

- **xs2strada** — Converts Perl XS modules to Strada `__C__` blocks.
- **strada2perl** — Converts Strada to Perl 5. `./strada2perl input.strada output.pl`

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

The Strada interpreter (`strada-interp`) executes Strada programs without generating C code. It defaults to a bytecode VM backend. Located in `interpreter/`.

```bash
# Build the interpreter
make interpreter

# Run a program
./strada-interp program.strada

# REPL
./strada-interp
```

**Architecture:** Source → Lexer → Parser → AST → Bytecode Compiler (`vm_compiler.c`) → VM Execution (`vm.c`)

**Key files:**

| File | Purpose |
|------|---------|
| `interpreter/vm.h` | Opcode definitions, VM structures (120+ opcodes) |
| `interpreter/vm.c` | VM execution engine |
| `interpreter/vm_compiler.c` | AST-to-bytecode compiler |
| `interpreter/Main.strada` | REPL and file execution entry point |

**Testing:** `interpreter/test/run_tests.sh` (33 tests, tree-walk backend) and `run_compiler_tests.sh` (the compiler test suite executed through the VM — **at parity: 108 pass / 0 fail**, 12 infrastructure skips). See `docs/INTERPRETER.md`.

**VM parity notes (2026-06):** the VM executes the full compiler suite. Key machinery: a **generic runtime bridge** (`interpreter/vm_generic_builtins.inc`, generated by `tools/gen_vm_generic_builtins.py`) exposes ~235 `sys::`/`math::` runtime functions through `vm_to_sv`/`sv_to_vm` (both depth-capped — cyclic structures fall back safely); **`vm_execute_call` is re-entrant** (exec_base watermark instead of frame_count==0), which enables nested bytecode calls from opcode handlers — used by hash `tie` (TIEHASH/FETCH/STORE) and available for future traps; `before`/`after` method modifiers compile to **synthesized wrapper chunks** (dispatch can't run hooks mid-opcode); `s///e` compiles an inline find/eval/append loop over a new `OP_REGEX_FIND` opcode (the parser already supplies the parsed replacement as `eval_expr`); `tr///` delegates to the runtime's `strada_tr` (full d/c/s/r + count); regex `i/s/m/x` flags fold into an inline `(?...)` pattern prefix at compile time; named captures read the PCRE2 name table. VM stack-value convention: **only string temporaries are owned by the stack** — consume sites must use `vm_temp_free` (freeing a shared container/filehandle/closure is a use-after-free; this was the `defined($fh)` corruption bug).

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

Supported: `+`, `-`, `*`, `/`, `%`, `**`, `.`, `""`, `==`, `!=`, `<`, `>`, `<=`, `>=`, `<=>`, `eq`, `ne`, `cmp`, `neg`, `!`, `~`, `bool`. Zero overhead when not used.

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
- **`.smeta` sidecars**: every `-M` build **automatically** writes
  `Foo.o.smeta` (the text `__strada_export_info()` returns) right after the
  `.o` compile succeeds (`write_smeta_sidecar`, called at
  `tools/strada-driver.strada:928`) — this is unconditional, not gated on
  `--module-cache`. When a consumer resolves `use Foo;` against an artifact
  (i.e. on the artifact path: `--use-artifacts`, `--module-cache`, or a
  fresh sibling `Foo.o`), it reads the module's export interface (functions,
  signatures, transitive `use:` deps) from the sidecar **instead of**
  compiling-and-running a metadata probe binary per module per build (~150ms
  of `cc` each — the probe made artifacts SLOWER than source). The sidecar
  path executes **no** artifact code at compile time; note the `.o` is still
  *linked* into the final binary either way — the sidecar only removes the
  compile-time probe/source-parse of the module's interface, not the object.
  Compile-time interface resolution order in `extract_object_export_info`
  (Parser.strada): (1) fresh `Foo.o.smeta` → read directly; (2) else the
  probe-result cache (below); (3) else the **live probe** — the original
  pre-sidecar path that builds a tiny binary, links the `.o`, runs it to
  print `__strada_export_info()`, and parses that. So a **missing, stale, or
  failed-to-write sidecar never breaks a build** — it just degrades to the
  old probe path (correct, slower). Freshness gate: the sidecar must be at
  least as new as its `.o` or it's ignored.
- **Generation vs. install**: `make libs` is what *generates* the lib
  sidecars — it builds each library with `./strada -M …`
  (`Makefile:519,536,547,554,561`), so `lib/Foo.o` + `lib/Foo.o.smeta`
  appear together. `make install` does **not** generate sidecars; it only
  *copies* existing ones — its ext loop (`Makefile:616`) globs
  `lib/**/*.smeta` and installs each alongside its `.o`, ordered LAST so the
  sidecar's mtime ends up newer than the `.o` (keeps the freshness gate
  happy). `install:` depends on `libs`, so they exist by install time. An
  installed `.o` lacks a sidecar only if its `-M` build couldn't write one
  (probe link failed → it warns "could not write metadata sidecar…"); such a
  lib still works via the probe path on the consumer side.
- **Probe-result cache** (2026-06): when the sidecar is missing (e.g. a
  `.o` in a read-only tree installed before sidecars existed), the probe's
  output is cached under `$STRADA_MODULE_CACHE_DIR/probe-meta/` keyed by
  the `.o`'s realpath+mtime (`extract_object_export_info` in
  Parser.strada) — a replaced `.o` gets a new key, so entries can't go
  stale. The probe then runs once per `.o` version instead of every build.
- **Transitive deps**: export metadata now records `use:Module` lines;
  importing an artifact resolves its deps automatically (artifact
  preferred, source fallback) — main no longer needs to `use` everything
  its modules use. Object paths are realpath-canonicalized for dedup so an
  explicit CLI `.o` plus a metadata-resolved one don't double-link.
- **Artifact codegen**: `-M` objects are built `-fno-lto` (LTO bitcode made
  every consumer link re-run a ~25s LTO pass) and `-fPIC` (same artifact
  links into executables and `--shared` libraries).
- Implies `--use-artifacts` (artifact use stays opt-in by default).
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

Capture-by-reference, mutations visible to outer scope. Transitive capture works: a closure nested inside another closure captures outer variables through the enclosing closure's `__captures` slots (gen at CodeGenExpr's capture-array emission forces the enclosing closure to capture pass-through names).

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

### Variadic Functions and Spread

```strada
func sum(int ...@nums) int { ... }
sum(1, 2, 3);
my array @vals = (10, 20);
sum(1, ...@vals, 99);  # Spread operator
```

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
- **Logical**: `&&`, `||` (returns value, not bool), `//` (defined-or), `!`
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
- **`tie`/`untie`/`tied`**: Custom variable implementations dispatching FETCH/STORE/EXISTS/DELETE/FIRSTKEY/NEXTKEY/CLEAR. Zero overhead when untied.
- **Private functions**: `private func helper(int $x) int { ... }` — `static` in C, not exported
- **Dynamic return type**: `func flexible() dynamic { ... }` with `core::wantarray()` / `core::wanthash()`
- **Package-qualified calls**: `::helper()` resolves to current package. Also `.::func()`, `__PACKAGE__::func()`.

### Stack Traces and Introspection

- Uncaught exceptions automatically print stack traces
- `core::stack_trace()` - Manual stack trace as string
- `core::caller()` - Returns hash with `function`, `file`, `line` of calling frame
- `core::caller($level)` - Optional level: 0 = immediate caller, 1 = caller's caller, etc.
- `--no-stack-trace` flag to disable for performance
- `core::set_recursion_limit($n)` - Deep recursion protection (default: 1000)

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
core::array_reserve(@arr, $n);
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

Semicolon goes on the `<<EOT;` line. Supports bare `<<EOT`, single-quoted `<<'EOT'`, double-quoted `<<"EOT"`.

### Perl Compatibility

- **`$_` default**: `chomp()`, `uc()`, `lc()`, `length()`, `ucfirst()`, `lcfirst()`, `trim()`, `defined()`, `ref()`, `chr()`, `ord()`, `say()`, `print()`, `chop()` all default to `$_` with no args
- **Bare `shift`/`pop`**: Default to `@_` — `func process { my $first = shift; }`
- **`chomp($s)`**: Strips trailing `\n`/`\r\n` in-place
- **Autovivification**: `$h{"a"}{"b"}{"c"} = 42;` auto-creates intermediates
- **`splice`**, **`each`**, **`select`**, **`reverse`**, **`isa`/`can`**, **`core::signal`**, **`goto`** — all supported

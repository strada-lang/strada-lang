# Strada Continuous Audit Playbook

This document captures the repeatable deep-audit, regression-testing, and verification workflow used across `strada-lang` improvement cycles. Any engineer or AI session can follow this playbook verbatim to run another full cycle.

## Convergence Loop

Each audit cycle follows six ordered phases until a full 4-shard audit finds **zero genuine bugs**:

1. **4-Shard Parallel Audit** — Launch four read-only audit shards concurrently across non-overlapping subsystems (excluding ignored areas below). Every finding must identify an exact file, line range, concrete failure mode (wrong result, crash, UAF, OOM, leak, UB, or Compiled vs VM semantic divergence), and minimal reproducer.
2. **Triage & Planning** — Deduplicate and verify each reported finding against actual source code; drop false positives and design clean fixes for all genuine bugs.
3. **Implementation & Iterative Quick Builds** — Apply fixes in C and Strada, verifying incrementally with `make quick`, `make interpreter`, `make -C interpreter vm`, and `make tools` (no `DEV` flags left enabled).
4. **Regression & Valgrind 0-Leak Tests** — Add targeted functional, Compiled vs VM parity, and Valgrind 0-leak tests for every fixed bug under `t/audit_r<N>/`, `interpreter/test/`, `t/leak_tests/`, and `interpreter/test/leak_tests/`.
5. **Clean Non-`DEV` Full Build & Verification** — Rebuild the entire tree from `make clean` and run all self-hosting, compiler, runtime, interpreter, VM, and Valgrind leak suites to 0 failures and 0 leaks.
6. **Commit, Push & Repeat** — Clean any temporary build/test artifacts from the repo tree, `git commit` + `git push origin main`, and launch the next 4-shard audit cycle. Stop when a full cycle reports zero genuine bugs.

---

## Scope & Exclusions

### Excluded Directories / Tools
- **`xs2strada` (`tools/xs2strada*`)**: Excluded completely from audits, builds, tests, and commits unless explicitly requested.
- **`tools/strada2perl` / `tools/perl2strada`**: Removed from the repository; do not re-add or audit them.
- **`bootstrap/` (`bootstrap/*`)**: Excluded from routine audit cycles. Only modify `bootstrap/` when compiling `compiler/Combined.strada` (`make test-selfhost` Stage 1) exposes a concrete blocker (such as a crash, OOM, or syntax/codegen limitation needed to compile the Strada compiler).

### Active 4-Shard Audit Partition

1. **Shard 1 — Runtime & GC (`runtime/`)**
   - `runtime/strada_runtime.c`, `runtime/strada_runtime.h`, `runtime/strada_runtime_tcc.h`
   - Focus: reference counting, cycle GC, arenas, weak references, exception unwinds (`try`/`catch` & cleanups), strings/UTF-8, regex/PCRE2, tied containers, overload hooks, coroutines, POSIX/threads/sockets/FFI helpers, `-DSTRADA_NO_TLS` compatibility.

2. **Shard 2 — Self-Hosting Compiler & Preprocessor (`compiler/`, `tools/stradapp.strada`)**
   - `compiler/AST.strada`, `compiler/Lexer.strada`, `compiler/Parser.strada`, `compiler/CodeGen*.strada`, `tools/stradapp.strada`
   - Note: `compiler/Combined.strada` and `compiler/Combined.c` are generated from `compiler/*.strada`; fix the modular sources. Exclude `bootstrap/` and `xs2strada`.
   - Focus: lexing/parsing edge cases, tagged 62-bit ints vs `>2^62` heap ints, LTO/inlining safety, `our`/closure/`local()` semantics, slice/compound assignment order, OOP modifiers/inheritance/MRO, exception safety, `__C__` block & heredoc preprocessing.

3. **Shard 3 — Bytecode VM Interpreter, REPL/Eval & `Strada::JIT`**
   - `interpreter/vm.h`, `interpreter/vm.c`, `interpreter/vm_compiler.c`, `interpreter/vm_run.c`, `interpreter/Main.strada`, `lib/Strada/Interpreter.strada`, `lib/Strada/Eval.strada`, `lib/Strada/JIT.strada`
   - Focus: Compiled C vs bytecode VM semantic parity, mark-sweep GC & weakref correctness, exception propagation, tied containers & operator overloads, `next`/`last`/`redo`/`return` boundaries, `-e` / REPL / `eval_string`, `Strada::JIT` parsing & execution.

4. **Shard 4 — Standard Library, C Extensions & Active Tools**
   - `lib/**/*.strada` (excluding `lib/Strada/Interpreter.strada` and `lib/Strada/JIT.strada` covered by Shard 3), C helper sources under `lib/` (e.g. `lib/usb/`, `lib/perl5/`), and active tools under `tools/` (e.g. `tools/strada-driver.strada`, `tools/stradadoc.strada`, `tools/strada-soinfo.strada`, `tools/strada-md2man.strada`, `tools/strada-md2html.strada`, `tools/strada-jit.strada`, `tools/strada-proftext.strada`, `tools/strada-profhtml.strada`, `tools/perla-cpan.strada`), excluding `xs2strada`.
   - Focus: `Forma`, `Template`, `DBI`, `JSON`, `HTTP`, `Regex`, `NFA`, cryptography/encoding, memory safety/leaks in `__C__` blocks, boundary checks, protocol/RFC compliance.

---

## Core Codebase Invariants

- **No Ternary Operator in `compiler/*.strada`**: Stage-1 `bootstrap/stradac` does not parse `cond ? a : b`; always use `if`/`elsif`/`else` in compiler source modules.
- **Sync Runtime Headers**: Whenever runtime structs, macros, flags, or declarations change, keep `runtime/strada_runtime.h` and `runtime/strada_runtime_tcc.h` synchronized and check `-DSTRADA_NO_TLS` compilation (`gcc -DSTRADA_NO_TLS -fsyntax-only -Iruntime runtime/strada_runtime.c`).
- **Compiled vs VM Parity**: Any language feature or bugfix affecting runtime semantics should behave identically across compiled executables (`./strada`) and the bytecode VM (`interpreter/strada-interp`).
- **Clean Working Tree**: Never leave compiled test binaries, `.c` scratch outputs, or temporary files inside the repository.

---

## Build & Verification Commands

### Iterative Quick Builds (During Active Edits)
```bash
make quick
make interpreter && make -C interpreter vm
make tools
```

### Pre-Commit Clean Non-`DEV` Full Build
```bash
gcc -DSTRADA_NO_TLS -fsyntax-only -Iruntime runtime/strada_runtime.c
make clean && make -C interpreter clean && make && make test-selfhost && make libs && make interpreter && make -C interpreter vm && make tools && make -C lib/usb clean all && make -C lib/perl5 clean all
```

### Mandatory Full Test & Valgrind 0-Leak Verification
Every suite must pass with **0 failures** and **0 leaks** before committing:
```bash
make test-selfhost
make test && rm -f runtime/test_runtime
./t/run_tests.sh
./interpreter/test/run_tests.sh
./t/leak_tests/run_leak_tests.sh
./interpreter/test/leak_tests/run_leak_tests.sh
./strada --tcc -r examples/test_simple.strada
```

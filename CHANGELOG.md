# Changelog

## Unreleased (since `0da0455`, 2026-06-09 → 2026-06-10)

### Language & stdlib (2026-06-10)
- **Lazy ranges in map/grep** — `map {...} (1..1e6)` / `grep {...}`
  over int-typed ranges iterate a native C loop (no input array;
  mirrors the foreach range fast path). Fixed in passing: a missing
  tagged-int guard in `strada_hash_from_flat_array` crashed
  `my hash %h = map { $_ => 1 } <ints>` whenever the keys were tagged
  integers (pre-existing; range keys made it common). Suite 178 → 179.
- **Value-producing `do {}`** — `my $x = do { ...; expr; };`: the last
  EXPRESSION statement is the value (undef for non-expression tails);
  block-locals clean up before the value is yielded. Documented edges:
  ternary for conditional tails, parenthesize in statement-head position
  (bare `do {` is do/while), no control-flow exits inside (GNU C
  statement-expression limit). Suite 177 → 178.
- **`finally`** — full semantics: runs on normal completion, after catch,
  before unmatched/re-thrown exceptions propagate (guard-frame around the
  catch dispatch, so a THROWING catch body runs it too), and before
  `return`/`next`/`last` leave the construct (return value captured
  first). `try {} finally {}` without catch supported. Contextual
  keyword — `finally` remains a valid identifier elsewhere. Known
  limitation: labeled next/last crossing try/finally skip it (mirrors
  their pre-existing try-frame gap).
- **`lib/Test.strada`** — TAP test framework (ok/is/isnt/is_num/like/
  unlike/pass/fail/skip/diag/plan/done_testing); failure sets the exit
  code; runs under `prove`.
- **`lib/List.strada`** — reduce/any/all/none/first/sum/product/min/max/
  minstr/maxstr/uniq/zip/pairs.
- **Error chaining** — `core::exception_trace()` returns the call stack
  captured at the most recent throw (plain values included);
  `lib/Exception.strada` adds structured exceptions with `wrap`-style
  cause chains and `describe()` rendering.
- Fixed in passing: closure capture analysis (catch/foreach vars in anon
  fns — see thread round) had already landed; the test-suite fallback
  runner now skips intentionally-failing fixtures. Suite 172 → 177.

### Concurrency ergonomics (2026-06-10)
Closes the ROADMAP Tier-2 list:

- **`async::select(\@channels [, $timeout_ms])`** — block on multiple
  channels, atomically dequeue the first value; `[index, value]` result
  (−1 timeout, −2 all-closed). One global wake condvar; senders signal it
  only when selects are waiting.
- **`async::spawn($fn)`** — closure → pool future (the function form of
  `async func`; future_new's ownership transfer handled per arg kind).
- **Cancellation-aware `async::sleep($ms)`** + **`async::cancelled()`** —
  the pool worker publishes the current future in TLS; cancel broadcasts
  the future's cond, so a sleeping task wakes immediately (returns 0).
- **`async::map($fn, \@items [, $workers])`** — data-parallel map:
  atomic-index work sharing, results in input order, first exception
  cancels remaining work and rethrows in the caller.
- **`thread::tls_set/get/exists/delete`** — per-thread named values
  (per-thread hash, freed on thread exit).
- **`Async::Scope`** (lib) — nursery-style structured concurrency: all
  results in spawn order; a failure cancels remaining siblings, drains
  them, rethrows the first error. **`Async::Actor`** (lib) — strict-order
  message actors over a channel inbox (tell/ask/stop, poison-message
  stop).
- **Found & fixed along the way**: the try/catch stack and exception
  slots were GLOBAL — concurrent throwers (pool tasks, map workers)
  corrupted each other's frames and could longjmp across threads; now
  thread-local, with tcc-compiled code routed through exported
  `strada_try_push_slot/pop_slot` helpers. Worker threads free their
  exception TLS on exit. Plus a closure-capture codegen bug: `catch ($e)`
  variables and `foreach my $x` loop variables declared INSIDE an
  anonymous function were misclassified as outer-scope captures
  (C-compile errors for any try/catch or foreach in a closure).
- New `test_async_ergonomics` (also in the leak suite): all six features
  plus scope-failure sibling cancellation and actor ordering;
  valgrind-clean. Suite 171 → 172.

### Thread safety (round 2, 2026-06-10)
The `7b8c8f0`/`0da0455` hardening made the object graph thread-safe
(atomic SV refcounts, locked registry, stop-the-world collector) but
stopped at the string layer, scratch state, and diagnostics. This round
closes the holes found while reading those paths during the perf work:

- **StradaString refcounts are atomic under threading** — strings cross
  threads directly and via zero-copy shares (`keys()`/`each()`,
  `strada_to_str_ss` borrows); the plain `++`/`--` raced (double-free or
  leak of the string backbone). Same threading-active gate as SV
  refcounts; the runtime now also EXPORTS `ss_incref`/`ss_decref` (real
  symbols for tcc-compiled code and .so ABI; gcc TUs keep the inline).
- **`strada_to_str` integer scratch is per-call** — it was a shared
  `static char[128]`: two threads stringifying integers concurrently
  silently garbled each other's digits before the strdup.
- **Regex state is thread-local** — `$1`/`captures()`/`$&`/prematch/
  postmatch, the lazy-`$&` machinery, the reusable match data, and both
  compiled-pattern caches (a shared cache raced on eviction: one thread
  frees a pcre2_code mid-match in another). Worker threads free their
  regex TLS on exit (`strada_thread_wrapper` / pool workers) so
  create/join loops don't leak per thread.
- **The call stack is thread-local** — traces and `core::caller()` are
  per-thread (a shared stack interleaved frames across threads and had a
  racy bounds check). tcc-compiled code maps the `_il` fast-path names to
  the exported functions (tcc TLS support varies); gcc keeps the inlines.
- **The FFI callback registry is mutex-guarded** (`c::callback` from
  multiple threads raced the list).
- New `test_thread_state` regression: 4 concurrent contexts hammer
  per-thread regex captures, integer stringification, zero-copy keys()
  of a shared hash, and caller() coherence; valgrind-clean including
  thread exits. Suite 170 → 171.

### Performance (round 5, 2026-06-10)
- **`keys()`/`each()` zero-copy (~3x on keys-iteration)**: key result SVs
  share the hash key's `StradaString` instead of copying every key's bytes.
  The three in-place string mutators gained copy-on-write guards
  (`SS_FROM_PV(pv)->refcount == 1`) so mutating a `keys()` result can never
  corrupt a live hash key. Also fixes binary keys: shared keys carry their
  byte-accurate length instead of strlen-truncating at the first NUL.
- **`join()` ~6x on string arrays**: codegen now calls `strada_join_sv`
  directly — separator passed as an SV (no stringify/copy), plain-STR
  elements borrowed (no per-element malloc+copy), result built directly
  into the final `StradaString` (the old path assembled in a scratch
  buffer, then copied the whole result again). ASCII/UTF-8 flags propagate
  from the parts instead of rescanning.
- **Mixed-type default sort ~2.7x**: decorate-sort-undecorate — non-STR
  elements stringify once (O(n)) instead of inside the qsort comparator
  (O(n log n)); all-STR arrays skip decoration. `sort %h` walks hash
  entries directly (old path: materialize keys array, then re-stringify
  and re-probe the hash per key).
- **Regex match-data reuse (~1.2x on `/g` loops)**: one-slot thread-local
  `pcre2_match_data` cache sized to the largest ovector seen, replacing
  per-match create/free at all 26 call sites; nested matches (regex inside
  an `s///e` callback) fall back to fresh allocation via a busy flag.
- **Condition-level hash-fetch CSE (~1.2x on fetch-heavy conditions)**:
  `if ($h{"k"} > 0 && $h{"k"} < 100)` emits one probe, not two. Gated on:
  pure condition trees (no calls/assignments/increments/regex), and the
  program never calling `tie` (tied FETCH is observable per-mention;
  Semantic publishes `uses_tie` on the program node).
- **`sprintf` ~1.2x** and no longer silently truncates: output builds in a
  4KB stack buffer that switches to a growable heap buffer on demand
  (`%99999d`, 70k-wide `%s`/`%b`, >64KB results all format exactly now —
  the old fixed 64KB stack buffer truncated silently). The heap buffer is
  registered on the cleanup stack so a throwing `""` overload mid-format
  doesn't leak it.
- Test suite 165 → 166 (`test_perf_round5` covers all six changes incl.
  the keys-COW and wide-sprintf edges).
- **Cycle collector: adaptive trigger + buffered-flag bit (−16.2%
  whole-program instructions on cycle-free workloads)**: (1) a collection
  that frees nothing doubles the candidate threshold (cap ~1M); one that
  frees cycles resets it to the base (default 10000, settable via
  `core::gc_threshold`). (2) the per-decref "already buffered?" check is
  a bit test on the value (`STRADA_CC_BUFFERED`, struct_size bit 60 —
  unused on containers) instead of a side-table hash probe; type-morphing
  sites (overwrite_in_place, vec_set conversion) retire candidates first.
  Profiling stradac compiling Lexer.strada: ~18% of all instructions were
  collector bookkeeping that never freed a cycle; after both changes
  total Ir went 1,101.6M → 923.6M and cc_tab_slot 142.7M → 31.6M. Cycle
  reclamation is unchanged for programs that do create cycles.

### Performance (round 7, 2026-06-10)
- **String interpolation / concat chains ~1.5x**: `.` chains of >= 3 leaf
  parts (interpolation desugars to these) compile to one
  `strada_concat_multi` call — sizing pass + single exactly-sized
  allocation — instead of a concat call per part with growth reallocs.
  Owned parts keep the pairwise emitter's cleanup-push discipline
  (throwing part evaluation leaks nothing); `.`/`""` overload programs
  fall back to the dispatching path for possibly-blessed parts; part
  stringification mirrors `strada_concat_sv` exactly (tied FETCH, UV ints,
  array counts, ref/coderef formats, ASCII/UTF-8 flag propagation).
  Whole-benchmark instructions −23% (callgrind); suite 166 → 167
  (`test_concat_multi`).

### Performance (round 8, 2026-06-10)
- **Stack-trace frame tracking inlined (−3% whole-program instructions)**:
  the per-function-entry/exit push/pop and per-call-site line store were
  out-of-line runtime calls (~4.5% of a call-heavy profile). Now
  `static inline` fast paths in both runtime headers
  (`strada_stack_push_il`/`_pop_il`/`_line_il`), with the call stack
  re-indexed to keep a sentinel frame at slot 0 (live frames at
  1..depth) so the line store is a single unconditional write. The old
  extern symbols remain as the compat ABI (bootstrap-generated C,
  previously compiled .so modules) and as the push slow path (recursion
  limit / overflow). Traces, `core::caller()`, and the recursion-limit
  error are unchanged. stradac self-compile: 923.6M → 895.5M instructions
  (−18.7% cumulative across rounds 6–8).

### Performance (rounds 1–4, 2026-06-09/10)
- **Method dispatch ~2x** (type-opaque call sites): compile-time method-name
  hashing, a 64-entry generation-invalidated global cache with cached
  modifier-presence verdicts, and per-call-site monomorphic inline caches
  (`strada_method_call_cs`). Programs using `before`/`after`/`around`
  anywhere no longer pay an MRO walk on every unrelated call.
- **Method resolution misses 3x** (`can()`, AUTOLOAD probing, overload
  lookup): per-package open-addressed method index + lazily cached
  flattened MRO, replacing recursive strcmp walks.
- **Range loops ~8x**: `foreach my $i (A..B)` with int bounds compiles to a
  native C for loop — no array materialization.
- **Hash element updates 1.8x**: `$h{$k} op= v` is a single-probe in-place
  slot update (`strada_hv_compound*`); fresh keys' first `+=` stays a
  tagged int; `.=` appends in place. Computed string keys get compile-time
  hashes; integer keys no longer strdup per access (1.5x on int-key loops).
- **Array element updates 2.7x**: `$a[$i] op= v` via `strada_array_compound`
  (one deref + bounds check, in-place slot update).
- **Large-string concat 2.1x**: result ASCII flag propagated from operands
  instead of rescanning the combined buffer.
- **Float accumulators 6.5x**: `$numvar = <numeric>` / `op=` writes the
  double into the existing solely-owned heap NUM (`strada_num_set_inplace`)
  instead of boxing fresh per iteration.
- Reference numbers and methodology: `benchmarks/BASELINE.md`. Two
  investigated-and-closed items are documented there with evidence
  (refcount elision for accumulators; full tagged/NaN-boxed doubles).

### Language
- **`c::callback` — closure→C-callback trampolines (libffi)**: a Strada
  closure becomes a real C function pointer, so qsort comparators and
  libcurl/GTK-style callbacks can be written in Strada.
  `c::callback($fn, "int", "ptr,ptr")` returns the pointer as an int
  address; marshals int/int32/num/ptr/str args and int/int32/num/ptr
  returns (max 8 args); captures and `our` globals work inside the
  callback. `c::callback_free($cb)` releases early; an exit-time registry
  sweep frees survivors (valgrind-clean). `./configure` auto-detects
  libffi; built without it, `c::callback` dies with a clear message.
  Closes the ROADMAP FFI-trampoline gap. Also fixed in passing: the
  statement-form `c::write_*`/`c::free` builtins leaked one heap undef SV
  per call (now return the immortal undef singleton); auto-collected
  `link_lib` deps from used modules are availability-probed at link time
  (a module declaring all DB backends no longer breaks linking on boxes
  missing one — `-lpq`); dlopen hosts (`import_lib` users) no longer
  strip runtime sections (`--gc-sections` off when `-rdynamic` — a .so
  built by a newer compiler can need runtime symbols the host's own code
  never referenced, e.g. `strada_cleanup_push`).
- **`--strict-types` — stage-0 gradual type checking (warning-only)**:
  declared types are now compared against a best-effort static expression
  type at var-decl initializers, plain assignments, call arguments, and
  returns. `scalar`/`dynamic`/unannotated are bivariant (untyped code
  never warns); int/num interconvert silently. `my int $n = "hi"` finally
  says something. Dogfood result: zero warnings across the 30k-line
  self-hosting compiler and the examples+stdlib sweep, except one
  deliberate mismatch in the int-coercion test — high signal, no noise.
  This is Stage 0 of the ROADMAP type-system plan (Semantic.strada only;
  no runtime/ABI/codegen changes).
- `*=`, `/=`, `%=`, `**=`, `//=`, and `x=` compound assignment operators —
  previously documented but unimplemented (didn't lex/parse). Work on all
  target shapes including hash/array elements.
- `int`- and `num`-declared storage now holds canonical values: assigning
  or returning a value the compiler can't prove correctly-typed coerces it
  (`my int $x = "12abc"` → 12). Previously the foreign pointer was aliased
  in, confusing the typed fast paths. Parameters are exempt (documented).

### Bug fixes (silent data loss)
- Nested autovivification in compound assigns (`$deep{"a"}{"b"} += 2`) and
  ref-rooted chains (`$r->{"x"}{"y"} = 3`) no longer lose the store.
- Array autovivification now exists at all: `$h{"a"}[0] = 7`,
  `$aoa[1][2] = 9`, `$a[2]{"k"} = 5`, `$r->{"list"}[0] = 3` were silent
  no-ops. Dynamic-key nested stores autovivify too (was literal-keys-only).
- `around` method modifiers: `$orig`/`$self` were swapped in the hook,
  so `$self->accessor()` read empty attributes and `$orig->(...)` wasn't
  callable. Hooks now receive `($orig, $self, @args)` per the documented
  Moose convention.
- `needs_temp_cleanup` was missing `math::rand` (owned-context uses leaked).
- The dispatch caches are correct across lazily-loaded `.so` modules: a lib
  registering methods or modifiers mid-run (against host classes too)
  invalidates all warmed caches via the shared generation counter.

### Infrastructure
- `benchmarks/bench_hotpaths.{strada,js}`: microbenchmark for the optimized
  paths (the general suite is blind to them); wired into the runner.
- `run_benchmarks.sh` defaults to Strada/Node/PHP (`-a` for all languages,
  `--langs` for a custom set).
- Repo-root `.gitignore` whitelist: `git status` went from 416 untracked
  files to 0.
- Test suite grown 148 → 162 (regression tests for every fix above);
  leak-suite `test_arrays` reverse check repaired (was silently
  early-returning for its entire history).
- Now tracked: `CLAUDE_DEEP_DIVE.md`, `ROADMAP.md`,
  `SECURITY_AUDIT_MEMORY.md`, `tools/pacco/` (package manager),
  `tools/perl2strada/` + its test suite, interpreter benchmarks, and
  assorted test/tool sources that were sitting untracked.

# Changelog

## Unreleased (since `0da0455`, 2026-06-09 → 2026-06-10)

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
- **Cycle collector: adaptive trigger (−11.5% whole-program instructions
  on cycle-free workloads)**: a collection that frees nothing doubles the
  candidate threshold (cap ~1M); one that frees cycles resets it to the
  base (default 10000, settable via `core::gc_threshold`). Profiling
  stradac compiling Lexer.strada showed ~18% of all instructions in
  collector phases that never freed a cycle; after: collection phases are
  ~1%, total Ir 1,101.6M → 975.1M. Cycle reclamation behavior is
  unchanged for programs that do create cycles (first fruitful collection
  snaps the trigger back).

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

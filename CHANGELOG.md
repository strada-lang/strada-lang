# Changelog

## Unreleased (since `0da0455`, 2026-06-09 → 2026-06-10)

### Performance
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

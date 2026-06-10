# Benchmark Baseline Record

Reference numbers from the 2026-06 optimization rounds, so future runs can be
compared without rebuilding the old compiler. All numbers from the same box
(2-core x86_64 AWS, gcc 13, Strada at default `-O2` + LTO, Node v24.15.0),
interleaved best-of runs on an otherwise quiet machine. **Concurrent builds or
the valgrind leak suite skew results ~2x — benchmark on a quiet box only.**

- "pre-opt" = commit `0da0455` (before any optimization work), rebuilt from
  scratch in a worktree and measured side by side on 2026-06-09/10.
- "post-opt" = commit `57f5e53` (per-call-site dispatch cache; rounds 1–2).
- "round 3" = commit `f9bfbaa` (method index/MRO cache, stack-buffer hash
  keys, single-pass array compound), measured 2026-06-10 against the
  immediately preceding commit for each change.

## Hot-path microbenchmarks (`bench_hotpaths.strada`)

Per-section wall times printed by the benchmark itself:

| section  | pre-opt | post-opt | speedup | what changed |
|----------|---------|----------|---------|--------------|
| dispatch | 0.159s  | 0.082s   | 1.9x    | precomputed name hash, 64-entry generational cache, cached modifier presence, per-call-site monomorphic cache |
| hash     | 0.493s  | 0.268s   | 1.8x    | single-lookup `strada_hv_compound` (one probe instead of fetch+store) |
| range    | 0.141s  | 0.018s   | 7.8x    | foreach over int ranges emits a native C for loop (no array materialization) |
| concat   | 1.12s   | 0.52s    | 2.2x    | ASCII flag propagated from operands instead of rescanning the result |
| objects  | 0.077s  | 0.081s   | —       | control; not targeted by any round |

## Round 3 microbenchmarks (2026-06-10)

Not covered by `bench_hotpaths` sections; each measured before/after its own
change on a quiet box (3 runs, median). Workload sources are described so
they can be reconstructed:

| change | workload | before | after | speedup |
|--------|----------|--------|-------|---------|
| per-package method index + flattened MRO cache | 1M `$obj->can("not_there")` — a miss never caches, so each call pays full resolution; 5-level inheritance chain, 20-method base class | 0.193s | 0.064s | 3.0x |
| (same, cached-hit control) | 1M `$obj->can("m19")` resolving 5 levels up — absorbed by the global cache | 0.036s | 0.038s | — |
| stack-buffer non-string hash keys (`sv_key_extract_buf`) | `$c{$j % 1000} += 1`, 5M iterations (integer keys previously paid itoa + strdup + free per access) | 0.31s | 0.20s | 1.5x |
| single-pass array compound assign (`strada_array_compound`) | `$acc[$i % 100] += 1`, 10M iterations | 0.168s | 0.061s | 2.7x |

## Round 4: in-place num accumulate (2026-06-10)

`$numvar = <numeric>` and `$numvar += n` previously boxed a fresh heap NUM
and released the old one every iteration (pool pop/push + refcount
dispatch). `strada_num_set_inplace` writes the double into the existing box
when the variable is the sole owner (refcount 1, no meta); shared/tied/weak
values still fresh-box, so aliasing semantics are unchanged.

| workload | before | after | speedup |
|----------|--------|-------|---------|
| `$sum = $sum + 0.5`, 50M iterations | 0.74s (15ns/iter) | 0.114s (2.3ns/iter) | 6.5x |
| int accumulator control (same box) | 0.028s (0.56ns/iter) | unchanged | — |

## Investigated and closed: full tagged/NaN-boxed doubles

Assessed 2026-06-10 after the in-place fix above. The remaining ~4x gap to
int speed would require encoding doubles in the StradaValue* pointer
representation. Audit surface measured: 276 STRADA_IS_TAGGED_INT guard
sites in the runtime, ~400 direct `sv->type` accesses, both runtime
headers, codegen emission sites — and it would break every user `__C__`
block that reads `sv->type` / `value.nv` on NUM values, which CLAUDE.md
documents as the normal C-interop pattern. Verdict: not worth the ABI
break and audit risk for the residual gap; the in-place store captures the
bulk of the win. Revisit only if a profiled workload shows float boxing
still dominating.

## Investigated and closed: refcount elision for loop accumulators

Proposed by the original 2026-06 audit; measured moot on 2026-06-10. Eliding
the `__old` capture + `strada_decref` from int accumulator statements by hand
(`$sum = $sum + $i`, 200M iterations) changed nothing — 0.165s either way —
because `strada_decref` is an always-inline tagged-int branch and gcc at
`-O2`+LTO already proves the operands tagged and deletes the dead refcount
ops. Callgrind shows bench_compute spends 97% of instructions in fully
inlined native arithmetic with no surviving runtime calls. The remaining real
cost in this family is FLOAT accumulators, which box a heap double per
iteration (`strada_new_num`) — that decref does real work and is not
elidable; fixing it would mean tagged/NaN-boxed doubles (an ABI project).
Also note: int-declared variables are NOT guaranteed to hold tagged ints
(`my int $x = f()` can store an owned heap NUM, `my int $x = $str_scalar`
stores a shared pointer), so unconditional elision would be unsound anyway.

## Standard suite, pre-opt vs post-opt (best of 5, interleaved)

Flat by design — the standard suite avoids the slow paths the rounds fixed
(bench_oop devirtualizes its method calls; bench_strings uses in-place `.=`):

| benchmark        | pre-opt | post-opt |
|------------------|---------|----------|
| bench_compute    | 0.903s  | 0.909s   |
| bench_strings    | 0.054s  | 0.052s   |
| bench_array_hash | 0.093s  | 0.094s   |
| bench_functions  | 0.011s  | 0.012s   |
| bench_oop        | 0.022s  | 0.021s   |

## Strada vs Node.js at post-opt (best of 5, interleaved, 2026-06-10)

| benchmark        | Strada | Node.js | Node / Strada |
|------------------|--------|---------|---------------|
| bench_compute    | 0.93s  | 2.45s   | 2.6x          |
| bench_strings    | 0.052s | 0.125s  | 2.4x          |
| bench_array_hash | 0.097s | 0.522s  | 5.4x          |
| bench_functions  | 0.012s | 0.039s  | 3.3x*         |
| bench_oop        | 0.022s | 0.043s  | 2.0x*         |

\* These two finish in tens of milliseconds, where Node's ~30ms process
startup inflates its ratio; bench_compute and bench_array_hash are the
meaningful steady-state comparisons.

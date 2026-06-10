# Benchmark Baseline Record

Reference numbers from the 2026-06 optimization rounds, so future runs can be
compared without rebuilding the old compiler. All numbers from the same box
(2-core x86_64 AWS, gcc 13, Strada at default `-O2` + LTO, Node v24.15.0),
interleaved best-of runs on an otherwise quiet machine. **Concurrent builds or
the valgrind leak suite skew results ~2x — benchmark on a quiet box only.**

- "pre-opt" = commit `0da0455` (before any optimization work), rebuilt from
  scratch in a worktree and measured side by side on 2026-06-09/10.
- "post-opt" = commit `57f5e53` (per-call-site dispatch cache + all rounds).

## Hot-path microbenchmarks (`bench_hotpaths.strada`)

Per-section wall times printed by the benchmark itself:

| section  | pre-opt | post-opt | speedup | what changed |
|----------|---------|----------|---------|--------------|
| dispatch | 0.159s  | 0.082s   | 1.9x    | precomputed name hash, 64-entry generational cache, cached modifier presence, per-call-site monomorphic cache |
| hash     | 0.493s  | 0.268s   | 1.8x    | single-lookup `strada_hv_compound` (one probe instead of fetch+store) |
| range    | 0.141s  | 0.018s   | 7.8x    | foreach over int ranges emits a native C for loop (no array materialization) |
| concat   | 1.12s   | 0.52s    | 2.2x    | ASCII flag propagated from operands instead of rescanning the result |
| objects  | 0.077s  | 0.081s   | —       | control; not targeted by any round |

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

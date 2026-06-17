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

## Round 5 microbenchmarks (2026-06-10)

Measured with a dedicated harness (keys/sortkeys/join/sortmix/rxg/cse/sprintf
sections; workloads described below), old = clean worktree build of `6238511`,
new = post-round-5 tree, both at `-O2`, interleaved best-of-5 on a quiet box:

| section  | workload | old | new | speedup | what changed |
|----------|----------|-----|-----|---------|--------------|
| keys     | foreach over keys() of a 200-key hash, 20k sweeps (4M visits) | 0.074s | 0.050s | 1.5x | zero-copy key SVs (shared StradaString, no strdup per key) |
| sortkeys | sort(keys %h), same hash, 20k sweeps | 0.217s | 0.191s | 1.1x | same (sort dominates) |
| join     | join(",", @1000_strings), 20k joins | 0.689s | 0.164s | 4.2x | strada_join_sv from codegen: borrowed STR elements, direct-build into result SS (old path copied each element + whole result twice) |
| sortmix  | default sort of 50k tagged ints | 0.032s | 0.011s | 2.9x | decorate-sort-undecorate (stringify once, not per comparison) |
| rxg      | while (=~ /(\d+)/g), 200k matches | 0.058s | 0.050s | 1.2x | one-slot reusable pcre2 match data |
| cse      | while ($c{"k"} > 0 && $c{"k"} < 10M) — 10M iterations | 0.167s | 0.139s | 1.2x | condition-level CSE: one probe per evaluation instead of two |
| sprintf  | 1M small "%d:%s:%05d" formats | 0.235s | 0.207s | 1.1x | 4KB stack buffer + lazy heap growth (also: wide formats no longer truncate) |

## Round 6: adaptive cycle-collector trigger (2026-06-10)

Callgrind on `./stradac compiler/Lexer.strada` (a cycle-free, allocation-heavy
real workload, stage-2 stradac at -O1, runtime -O2):

| | pre | post |
|---|---|---|
| total instructions | 1,101.6M | 975.1M (**−11.5%**) |
| cc_tab_slot (side-table probes) | 142.7M (13.0%) | 76.3M (7.8%) |
| cc_each_child + mark/scan edges | ~48M (4.4%) | ~10M (1.0%) |

A collection that frees no cycles now doubles the trigger threshold
(default 10000 → cap ~1M); a fruitful collection resets it to the base.

**Follow-up, same day — buffered-flag bit:** the residual cc_tab_slot
cost was the per-decref "already buffered?" side-table probe. That check
is now a bit test on the value itself (`STRADA_CC_BUFFERED`, struct_size
bit 60 — containers never use struct_size). Same workload after both
changes:

| | pre-round-6 | adaptive only | + flag bit |
|---|---|---|---|
| total instructions | 1,101.6M | 975.1M | 923.6M (**−16.2%**) |
| cc_tab_slot | 142.7M | 76.3M | 31.6M |

The remaining cc_tab_slot traffic is first-time buffering inserts and
cc_forget probes for values that were genuinely buffered — irreducible
without dropping the side table entirely (which the no-ABI-change
constraint rules out, and the bit already removed the per-decref cost).
Type-morph sites (overwrite_in_place, vec_set's STR conversion) retire a
buffered candidate via cc_forget before clobbering type/struct_size.

## Round 8: inline stack-trace tracking (2026-06-10)

`strada_stack_push`/`_pop`/`_set_line` were out-of-line calls at every
function entry/exit and call site. Codegen now emits `static inline`
variants (both runtime headers); the call stack keeps a sentinel frame at
slot 0 (live frames 1..depth) so the per-call-site line store is one
unconditional write. Old externs remain for bootstrap/.so compat and as
the push slow path. Same callgrind workload (stradac on Lexer.strada):

| | round 7 base | round 8 |
|---|---|---|
| total instructions | 923.6M | 895.5M (−3.0%) |
| stack push/pop/set_line | ~44M (4.5%) | absent from profile (inlined) |

Cumulative rounds 6–8: 1,101.6M → 895.5M (**−18.7%**).

## Round 7: multi-part concat flattening (2026-06-10)

`.` chains of >= 3 leaf parts (string interpolation desugars to these in
the parser) now compile to one `strada_concat_multi` call — a sizing pass
plus a single exactly-sized StradaString — instead of one concat call per
part with growth reallocs on the accumulator. Harness
`benchmarks/bench_interp.strada`, old = worktree build of `5d80c67`,
interleaved best-of-5 on a quiet box:

| section | workload | old | new | speedup |
|---------|----------|-----|-----|---------|
| interp6 | 6-part interpolation, 5M iters | 0.428s | 0.279s | 1.54x |
| chain10 | 10-part explicit chain (mixed int/str parts), 2M iters | 0.254s | 0.177s | 1.43x |
| concat2 | 2-part control (pairwise path unchanged) | 0.112s | 0.081s | (layout noise — see below) |

The 2-part control's wall-clock delta is code-layout/alignment variance
under LTO, not a real change (its emission is byte-identical). Callgrind
(layout-insensitive) on the whole three-section benchmark: 11.06B → 8.51B
instructions (−23.1%), with the control section diluting the figure — the
flattened sections shrank substantially more than wall-clock suggests.

## Investigated and closed: foreach-over-keys codegen fast path

Considered alongside the zero-copy keys work: a dedicated codegen path for
`foreach my $k (keys %h)` that iterates hash slots directly instead of
materializing the keys array. Closed after the zero-copy change because the
remaining materialization cost per key is one pooled SV alloc + one
array-push — and a sound fast path must still snapshot the key list (Perl
semantics allow `delete` during iteration; the keys list is a snapshot) and
must still build one SV per iteration for the loop variable. So the fast
path saves only the array backbone + push per key (a minor slice of the
measured 12ns/key), at the cost of a new specialized foreach emission path.
Revisit only if a profiled workload shows keys-loop overhead dominating.

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

## New benchmarks (2026-06-12)

Nine benchmarks added covering previously-unmeasured subsystems: sort,
regex, list pipelines, exceptions, async/concurrency, GC/arena, JSON,
data processing (CSV analytics), and process startup — plus
`bench_compiler.sh` for stradac itself. Perl/Node counterparts where the
comparison is meaningful (async and gc are Strada-only runtime features).
Same box and conventions as above; Strada at default `-O2` + LTO, best of
3, quiet machine, Perl 5.x / Node v24.

| benchmark        | Strada | Perl   | Node   | notes |
|------------------|--------|--------|--------|-------|
| bench_sort       | 0.878s | 1.822s | 1.185s | int/str/comparator/hash-key sorts |
| bench_regex      | 0.607s | 0.620s | 0.474s | match/captures/named/s///g/s///e/tr |
| bench_pipeline   | 0.347s | 0.722s | 0.376s | lazy-range map/grep, chain, join/split |
| bench_exceptions | 0.478s | 0.292s | 1.447s | Perl's eval is cheaper; Node 3x slower |
| bench_async      | 0.298s | —      | —      | futures/channels/async::map/atomics/mutex |
| bench_gc         | 1.069s | —      | —      | churn/cycles/weaken/arena vs no-arena |
| bench_json       | 2.474s | 4.474s | 0.101s | pure-Strada JSON vs pure-Perl JSON::PP; Node's JSON is native C++ (not comparable) |
| bench_data       | 0.253s | 0.368s | 0.832s | generate+stream+aggregate+report a 25MB CSV |
| bench_startup    | 0.001s | 0.001s | 0.020s | process spawn-to-exit; sub-ms values are at timer resolution |

Compiler speed (`bench_compiler.sh`, not in the table — Strada-only):
best 1.76s to translate compiler/Combined.strada (35,105 lines) to C,
~20k lines/sec, with STRADA_GC=off (as the strada driver invokes it).

Per-section numbers are printed by each benchmark itself; rerun any of
them directly for the breakdown. Notable per-section results from this
baseline run: pipeline map-range/grep-range benefit from lazy ranges (no
input materialization); bench_gc's arena section runs the same workload
~25% faster than its arena-off control; bench_async moves 500k channel
messages in ~86ms.

### Language-feature round 2 (2026-06-12, later the same day)

Five more: UTF-8/Unicode, binary data, closures, sprintf, and the classic
binary-trees allocator stress. Same conventions.

| benchmark          | Strada | Perl   | Node   | notes |
|--------------------|--------|--------|--------|-------|
| bench_utf8         | 1.422s | 2.600s | 0.700s | case/valid/chr-build/concat/NFC. Models differ: Strada = UTF-8 bytes, Perl = decoded codepoints, JS = UTF-16; equivalent user-visible work |
| bench_binary       | 0.175s | 0.291s | 0.339s | pack/unpack/base64/byte-walk/frame-build; JS uses Buffer idioms |
| bench_closures     | 0.100s | 1.304s | 0.077s | create/invoke/capture-rw/table/transitive — 13x over Perl |
| bench_sprintf      | 2.133s | 1.687s | 0.748s | JS uses template-literal equivalents (no sprintf). Strada LOSES to Perl here — sprintf is an optimization lead. **UPDATED 2026-06-12 (same evening): 0.64s** after the sprintf fast paths + plan cache (see below) — now 2.7x faster than Perl; Node gap narrowed from 2.9x to ~1.5x (0.62-0.64 vs 0.42-0.45 interleaved) |
| bench_binary_trees | 3.158s | 2.490s | 0.154s | depth-16; V8's generational GC owns this shape. Strada's per-node hashref overhead (refcount + open-addressed hash) is the cost — a known trade. **UPDATED 2026-06-12 (same evening): 1.27s** (2.5x) — anon hashes now build through the pooled single-block compact-hash path with take-ownership constructors (compile-time djb2 keys); landing this required fixing a critical pre-existing dispatch bug (interned class-name recycling silently calling the WRONG class's method — see the sticky-intern commit) plus four compact-pool corruption bugs. Now 2.0x faster than Perl; V8's gap remains structural |

Optimization leads recorded by this round: sprintf (~1.3x behind Perl)
and allocation-heavy deep structures (binary-trees ~1.3x behind Perl,
far behind V8). Closures, binary data, and UTF-8 case ops are strong.


### sprintf optimization (2026-06-12 evening)

bench_sprintf: 1.337s -> 0.643s (quiet-box; interleaved best-of runs
0.62-0.63 vs Node 0.42-0.45). Three runtime changes, all output-preserving:

- guarded fast formatters for simple %d/%u/%x/%X/%o, %f/%.Nf (N<=9), and
  %e/%E/%g/%G (prec<=16) with -/0 flags + width: hand conversion with an
  AMBIGUITY GUARD — any close rounding call (ties, boundary exponents)
  falls back to snprintf, so output is bit-identical to glibc. Fuzzed
  against glibc: 52M + 12M cases across all three families, zero
  mismatches.
- sprintf plan cache: per-thread, content-keyed (capped djb2 + memcmp
  verify) cache of pre-parsed format plans; simple formats execute
  decoded ops straight into the output buffer (no per-call scanning).
  Non-simple formats (positional, vector, '+'/' '/'#', %c/%b/...) use the
  legacy loop unchanged.
- converge uses fast-path lengths (no strlen), literal runs single-memcpy.

Remaining Node gap (~1.5x) is per-call structure: varargs entry, arg SV
handling, result SV lifecycle. The identified fix is compile-time
lowering of literal-format sprintf calls in the codegen (emit the op
sequence directly); not yet attempted.

### C-accelerated JSON module + cross-.so OOP benchmark (2026-06-12 evening)

**bench_json: 2.474s -> 0.20s (12.4x).** lib/JSON.strada is now a C
implementation (encoder/decoder walk StradaValue structures directly in
__C__ code); the pure-Strada original moved unchanged to lib/JSON/PS.strada
as JSON::PS. Output is byte-identical to JSON::PS (enforced by
examples/test_json_differential.strada — 600 comparisons in the test
suite), valgrind clean, and BOTH implementations gained proper \uXXXX
decoding (incl. surrogate pairs). The Node gap narrowed from ~25x to
~2.3x (0.20 vs 0.086); decode dominates the remainder.

**bench_oop_so (new):** method dispatch on a class living in an
import_lib .so vs an identical same-binary class. Measured on a LOADED
box (load ~4 — treat as indicative only; rerun quiet for a clean record):
plain method calls pay ~10-20% across the boundary; the mixed-method
section runs ~1.6-2x slower for the .so class because LOCAL classes get
compile-time devirtualization (the codegen knows the receiver type from
the constructor and calls the function directly) while .so classes always
dispatch through the runtime method registry. Constructors are
comparable. The runner builds bench_oop_so_lib.so automatically.

### Guarded cross-.so devirtualization (2026-06-12 night)

bench_oop_so after extending devirtualization to import_lib classes
(method calls on known-class .so receivers now compile to direct calls
through the static host wrapper; a metadata fingerprint checked at
dlopen guards against swapped .so files — mismatch falls back to
dynamic dispatch with full hook semantics). Best of 5 on a LOADED box
(load ~3-4): so-mixed gap shrank from ~1.6-2x to ~1.2x of local-mixed
(0.059 vs 0.048); so-call ~1.25x local (0.082 vs 0.065); constructors
~1.1x. The residual is the wrapper hop + no cross-.so inlining
(physics, not dispatch). Swap-the-.so safety is regression-tested by
t/import_lib_devirt_test (incl. new hooks firing through the fallback).

### binary_trees: arena for short-lived trees (2026-06)

bench_binary_trees now wraps each short-lived tree (the iterate phase) in a
request-arena scope (`core::arena_begin/end`): 1.27s -> ~1.08s (~15%). The
arena bump-allocates the tree and reclaims it wholesale — no per-node
refcount teardown and no cycle-collector buffering (immortal arena SVs are
skipped by the CC). The one giant "stretch" tree is deliberately NOT
arena-scoped: `arena_owns()` is O(blocks), so a single tens-of-blocks arena
costs more than it saves; the arena wins for many small scopes. Node still
wins this GC-torture benchmark (~0.08s, generational GC) — the gap narrowed
from ~16x to ~13x. Prerequisite: the arena was hardened so nested/escaping
values no longer crash (see CLAUDE.md Request Arena).

## String/array allocation round (2026-06-17)

Two output-preserving codegen/runtime optimizations, then a full
Strada-vs-Node sweep. Same box and conventions (2-core x86_64 AWS, Strada
`-O2` + LTO, Node v24.15.0, best-of-5, quiet box).

### Self-append in-place concat (`$s = $s . a . b . ...`)

A `.`-chain assigned back to its own leftmost leaf went through
`strada_concat_multi`, which allocates a fresh exactly-sized buffer and copies
the WHOLE accumulator every iteration — O(n²) in a loop. (The 2-part `$s = $s
. x` and `.=` already appended in place; the ≥3-part form was missed because
the left-deep chain's immediate left operand is a BINARY_OP, not the target.)
Codegen now flattens the chain and, when the leftmost leaf is the assignment
target, appends each tail part in place via `strada_concat_inplace[_cstr]`
(amortized O(appended)). Guards: leftmost-leaf match on name AND sigil; bail to
the safe copy path if a tail leaf is the target (`$x = $x . $x`). Also fixed a
latent `length()`→`bytes()` bug for multibyte literals. (CodeGenExpr.strada;
commit `9cda92b`.)

- `bench_utf8` concat section **0.747s → 0.001s**; whole benchmark
  **1.59s → 0.26s** wall — flipped from 2.3× *slower* than Node to **1.7×
  faster** (0.256s vs 0.427s).

### Anon-array presize + direct-fill (`[a, b, ...]`)

Every `[...]` literal allocated its elements buffer at the default capacity
(16 slots = a 128-byte `calloc`) regardless of size, then filled via per-element
`strada_array_push`. callgrind showed ~50% of binary_trees in
malloc/free/calloc/memset. `strada_anon_array` now presizes the buffer to the
literal's exact element count (`strada_array_new_cap`) and direct-fills (no
per-push capacity check). Elements stays a separate allocation, so
growth/free/pool paths are untouched. `[]` (count 0) keeps the default-capacity
builder path. (strada_runtime.c; commit `445e403`.)

- `bench_binary_trees` **0.988s → ~0.71s** best-of-7 (~28%); also helps
  `bench_binary` (0.20→0.11s) and `bench_closures` (0.10→0.06s).
- Large array LITERALS get faster (one exact alloc, no growth reallocs); the
  only marginally-slower case is a non-empty literal then grown by push
  (~2.5% over 2M pushes — a fixed early-realloc cost, amortized to nothing).

### Full Strada vs Node sweep (best of 5)

| benchmark          | Strada | Node   | faster        |
| ------------------ | ------ | ------ | ------------- |
| bench_startup      | 0.001s | 0.020s | Strada 20×    |
| bench_array_hash   | 0.102s | 0.535s | Strada 5.2×   |
| bench_data         | 0.272s | 0.929s | Strada 3.4×   |
| bench_functions    | 0.012s | 0.040s | Strada 3.3×   |
| bench_exceptions   | 0.502s | 1.564s | Strada 3.1×   |
| bench_compute      | 0.910s | 2.595s | Strada 2.9×   |
| bench_strings      | 0.056s | 0.140s | Strada 2.5×   |
| bench_binary       | 0.113s | 0.221s | Strada 2.0×   |
| bench_oop          | 0.022s | 0.041s | Strada 1.9×   |
| bench_utf8         | 0.256s | 0.427s | Strada 1.7×   |
| bench_sort         | 0.812s | 1.247s | Strada 1.5×   |
| bench_pipeline     | 0.354s | 0.403s | Strada 1.1×   |
| bench_hotpaths     | 0.982s | 0.894s | Node 1.1×     |
| bench_closures     | 0.060s | 0.056s | Node 1.1×     |
| bench_regex        | 0.659s | 0.506s | Node 1.3×     |
| bench_sprintf      | 0.639s | 0.456s | Node 1.4×     |
| bench_json         | 0.196s | 0.106s | Node 1.8×     |
| bench_binary_trees | 0.785s | 0.102s | Node 7.7×     |
| bench_async        | 0.291s | —      | (no Node)     |
| bench_gc           | 0.930s | —      | (no Node)     |
| bench_oop_so       | 0.279s | —      | (no Node)     |

Of the 18 with a Node counterpart, Strada wins 12. Remaining Node wins:
binary_trees (V8 generational GC — structural), and the C++-native edges in
json/regex/sprintf. `bench_hotpaths` whole-process time is not the regression
signal (use its per-section prints).

### Three-way sweep adding Perl (2026-06-17, Perl 5.38.2)

Same box/conventions, best of 5. Perl runs are slow (compute alone is ~95s/run),
hence excluded by default — `--langs strada,node,perl` opts it in.

| benchmark          | Strada | Node   | Perl    | vs Node    | vs Perl     |
| ------------------ | ------ | ------ | ------- | ---------- | ----------- |
| bench_compute      | 0.905s | 2.579s | 95.108s | Strada 2.8× | Strada 105× |
| bench_functions    | 0.013s | 0.041s | 1.138s  | Strada 3.2× | Strada 88×  |
| bench_oop          | 0.022s | 0.043s | 1.348s  | Strada 1.9× | Strada 61×  |
| bench_json         | 0.200s | 0.103s | 4.827s  | Node 1.8×   | Strada 24×  |
| bench_closures     | 0.060s | 0.058s | 0.707s  | ~par        | Strada 12×  |
| bench_array_hash   | 0.099s | 0.484s | 0.595s  | Strada 4.9× | Strada 6.0× |
| bench_utf8         | 0.250s | 0.411s | 1.345s  | Strada 1.6× | Strada 5.4× |
| bench_pipeline     | 0.348s | 0.401s | 0.737s  | Strada 1.2× | Strada 2.1× |
| bench_sort         | 0.824s | 1.242s | 1.661s  | Strada 1.5× | Strada 2.0× |
| bench_binary_trees | 0.771s | 0.098s | 1.320s  | Node 7.9×   | Strada 1.7× |
| bench_strings      | 0.055s | 0.130s | 0.145s  | Strada 2.4× | Strada 2.6× |
| bench_sprintf      | 0.614s | 0.436s | 0.926s  | Node 1.4×   | Strada 1.5× |
| bench_binary       | 0.110s | 0.218s | 0.153s  | Strada 2.0× | Strada 1.4× |
| bench_data         | 0.263s | 0.862s | 0.381s  | Strada 3.3× | Strada 1.4× |
| bench_regex        | 0.664s | 0.512s | 0.681s  | Node 1.3×   | ~tie (1.0×) |
| bench_startup      | 0.001s | 0.019s | 0.001s  | Strada 19×  | ~tie        |
| bench_exceptions   | 0.500s | 1.572s | 0.312s  | Strada 3.1× | **Perl 1.6×** |
| bench_hotpaths     | 0.983s | 0.893s | —       | Node 1.1×   | (no Perl)   |

Strada beats Perl on every benchmark except bench_exceptions (Perl's eval/die is
cheap); regex and startup roughly tie. The blowouts are interpreted-Perl's
no-JIT cost: compute 105×, functions 88×, OOP 61×, JSON 24× (pure-Perl JSON::PP).
Of the 18 with a Perl counterpart, Strada wins 16.

### Lazy exception-trace capture (2026-06-17)

Every `throw` eagerly built the full formatted stack-trace string
(`strada_capture_stack_trace`: `malloc(call_depth*256)` + a `snprintf` per
frame) for `core::exception_trace()` — but the common `catch` never reads it, so
a deep throw was pure waste. Throw now only SNAPSHOTS the live frames (a small
`memcpy`; func/file names are static `const char*`) and the trace string is
formatted LAZILY, cached, only when actually read. (strada_runtime.c; commit
`c5b0a91`.)

Per-section best-of-5 (`bench_exceptions`):

| section      | before | after  | speedup |
| ------------ | ------ | ------ | ------- |
| try-nothrow  | 0.027s | 0.011s | 2.5×    |
| throw-catch  | 0.045s | 0.014s | 3.2×    |
| typed        | 0.181s | 0.067s | 2.7×    |
| deep-unwind  | 0.680s | 0.005s | ~136×   |
| finally      | 0.012s | 0.006s | 2×      |
| **total**    | 0.946s | 0.101s | **9.4×** |

This flips the one benchmark where Perl led: Strada vs Perl on exceptions goes
from 1.4× slower (0.50s vs 0.31s) to **6.9× faster** (0.101s vs 0.695s); vs Node
from 3.1× to ~15×. Every throwing section improved because the eager capture
fired on every throw, not just deep ones. Behavior is equivalent — the trace,
when read, names the same frames the eager path captured.

### Post-merge definitive 3-way sweep (2026-06-17, commit `e26d27f`)

Main with BOTH this session's optimizations merged (compact single-block arrays
+ lazy exception trace). Strada / Node v24.15.0 / Perl 5.38.2, best of 5, quiet
box. This supersedes the earlier 3-way table above for binary_trees and
exceptions.

| benchmark          | Strada | Node   | Perl    | vs Node    | vs Perl     |
| ------------------ | ------ | ------ | ------- | ---------- | ----------- |
| bench_compute      | 0.919s | 2.636s | 94.427s | Strada 2.9× | Strada 103× |
| bench_functions    | 0.014s | 0.041s | 1.131s  | Strada 2.9× | Strada 81×  |
| bench_oop          | 0.023s | 0.044s | 1.367s  | Strada 1.9× | Strada 59×  |
| bench_json         | 0.199s | 0.106s | 4.993s  | Node 1.9×   | Strada 25×  |
| bench_exceptions   | 0.101s | 1.573s | 0.319s  | Strada 15.6× | Strada 3.2× |
| bench_closures     | 0.062s | 0.055s | 0.716s  | ~par        | Strada 12×  |
| bench_array_hash   | 0.101s | 0.547s | 0.664s  | Strada 5.4× | Strada 6.6× |
| bench_utf8         | 0.251s | 0.426s | 1.384s  | Strada 1.7× | Strada 5.5× |
| bench_strings      | 0.056s | 0.136s | 0.147s  | Strada 2.4× | Strada 2.6× |
| bench_binary       | 0.109s | 0.225s | 0.158s  | Strada 2.1× | Strada 1.4× |
| bench_sort         | 0.871s | 1.280s | 1.769s  | Strada 1.5× | Strada 2.0× |
| bench_pipeline     | 0.370s | 0.409s | 0.751s  | Strada 1.1× | Strada 2.0× |
| bench_data         | 0.270s | 0.911s | 0.401s  | Strada 3.4× | Strada 1.5× |
| bench_binary_trees | 0.736s | 0.105s | 1.374s  | Node 7.0×   | Strada 1.9× |
| bench_sprintf      | 0.629s | 0.452s | 0.947s  | Node 1.4×   | Strada 1.5× |
| bench_regex        | 0.650s | 0.515s | 0.684s  | Node 1.3×   | ~tie (1.05×) |
| bench_startup      | 0.001s | 0.022s | 0.001s  | Strada 22×  | ~tie        |
| bench_hotpaths     | 0.989s | 0.882s | —       | Node 1.1×   | (no Perl)   |

Headline: after the exception fix, **Perl no longer wins any benchmark** — Strada
wins or ties all 18 with a Perl counterpart (was 16/18). vs Node, Strada wins 12
of 18; Node's remaining edges are binary_trees (generational GC — narrowed by
compact arrays, 0.74s vs the pre-merge 0.80s) and the C++-native
json/regex/sprintf/closures.

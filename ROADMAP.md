# Strada Roadmap / Known Gaps

`FEATURES.md` is the matrix of what **is** implemented. This file is the
complement: the verified gaps and a recommended order to close them.

Every "missing" claim below was checked against the source (a multi-lens
assessment that verified each candidate — it corrected several false absences:
JSON/YAML/CSV/DateTime **do** exist, in `lib/`; `async::all` **does** cancel
siblings on failure). Strada is already broad — self-hosting compiler,
tagged-int + cycle-GC + arena memory, Moose-style OOP, async/threads/channels,
PCRE2, full UTF-8, a VM/interpreter, JIT eval, and Perla (a near-complete Perl
5). The gaps are at the *edges* of a large surface. The one structural
exception is the type system.

## ✅ Recently shipped (on `main`)

- **Thread-safety for raw `thread::create`** (`7b8c8f0`): atomic refcounting
  under threading, locked global registry, pool/cleanup-stack fixes.
- **Cycle collector under threading** (`0da0455`): a stop-the-world pause so
  threaded programs no longer leak reference cycles. This was Tier-1 #2 below.
- **Thread-safety round 2** (2026-06-10): atomic StradaString refcounts,
  per-call to_str scratch, thread-local regex state ($1/captures/$&/
  pattern caches) with worker-exit cleanup, thread-local call stacks,
  mutex-guarded FFI callback registry. The remaining concurrency work in
  Tier-2 below is ergonomics, not safety.
- **Type-system Stage 0** (2026-06-10): `--strict-types` warning layer —
  `stage0_expr_type`/`stage0_types_compatible` in Semantic.strada wired into
  decl-init, assignment, call args, and return. Bivariant scalar/dynamic,
  silent int/num family. Dogfooded over the compiler + stdlib (one true
  positive, zero false). Tier-1 #1 below is now *started*; Stages 1–3
  (structured type nodes, `array<int>`, nullable `int?`) remain.

## ⭐ Highest-impact gaps

1. **The "strongly-typed" claim is aspirational.** `Semantic.strada` checks
   name resolution and call *arity* but **never compares types** —
   `my int $n = "hi";` compiles with exit 0. `var_type` is recorded and thrown
   away; safety is entirely deferred to the runtime `StradaValue`. *(See the
   type-system roadmap below — this is the highest-leverage gap.)*
2. **No parametric generics.** `array` is a flat `TYPE_ARRAY` with no element
   type; `parse_type` has no `<...>`. Root cause that blocks typed collections
   and reusable type-safe APIs.
3. **No generators / coroutines / lazy iterators.** Zero `yield` token/AST node.
   `map`/`grep` are eager; every async task is a real OS thread, so it's
   thread-pool parallelism, not M:N cooperative concurrency.
4. **No Language Server / IDE integration.** No completion, go-to-def, hover, or
   diagnostics — likely the biggest adoption blocker after types.
5. **No in-language unit-test framework.** Testing is shell-driven (compile +
   match stdout). No `ok`/`is`/`like`, no TAP/Test::More, no mocking.

## The rest, by theme

- **Type system:** no sum types / `match`-expression, no nullable/optional
  types, no type aliases/newtypes, roles are literally `extends` (no
  required-method contracts), inference is sigil-defaulting only.
- **Concurrency:** no `select` over channels, no structured concurrency /
  nursery scope construct, no `async::sleep`/`yield` (a sleeping task blocks a
  pool thread), no work-stealing / data-parallel map, no actors, no
  language-level thread-local.
- **Std lib:** no binary serialization (MessagePack/CBOR), no rich collections
  (Set/Deque/heap/ordered-map — only a LinkedList), no standalone URI module,
  DateTime has no IANA timezones / DST-aware math, no logging framework (only
  Syslog), no stats beyond sum/min/max, HTTP server only as raw-socket
  examples, compression is zlib-only.
- **Metaprogramming:** no macros / compile-time codegen, no constexpr, no class
  reflection (can't enumerate a class's methods/attrs — the `has` schema isn't
  retained at runtime), no runtime monkey-patching (can't register a Strada
  closure as a method), `BEGIN` runs at `main()` startup not compile time.
- **Control-flow / errors:** no Result/Option or `?` operator, **no `finally`**,
  no error chaining/cause/backtrace on *caught* errors, `switch`/`case` is
  `strcmp`-only (not an expression, no binding/exhaustiveness), no
  comprehensions, ranges are eager (`1..1e6` materializes), no value-producing
  `do {}` block, no `reduce`/`any`/`all` builtins.
- **FFI / low-level:** ~~no closure→C-callback trampoline~~ ✅ shipped
  2026-06-10 (`c::callback` via libffi — qsort/libcurl/GTK callbacks work);
  still missing: structs/unions by value, compiler-known struct layout
  (hand-coded offsets today).

---

## Type-system roadmap (the highest-leverage work)

**Thesis: a gradual, bidirectional, *optional* static-check layer that sits on
top of the already-fully-dynamic `StradaValue` runtime — changing the compiler,
never the runtime, ABI, or codegen.** Every `StradaValue` op already
type-checks at runtime, so the checker can be unsound-but-useful (the TypeScript
model): the whole effort collapses to "compute a type for each expression and
compare." Purely additive, no `.o`/ABI churn, bootstrap-safe.

**What NOT to do:** no global Hindley-Milner inference (fights the Perl
heritage, cryptic errors, huge lift in a self-hosting compiler); don't make
typing mandatory (`dynamic`/`scalar`/unannotated stays the gradual "any");
don't monomorphize/specialize the runtime (keep everything boxed
`StradaValue*` — generics are a compile-time check, not a representation
change); don't chase soundness (the runtime is the backstop).

**Stage 0 — make the annotations mean something. ✅ SHIPPED 2026-06-10.**
`stage0_expr_type($ctx, $expr) → type` and `stage0_types_compatible` live in
Semantic.strada, wired into the four sites (`my T $x = …`, assignment, call
args, return) behind `--strict-types`. Bivariant `dynamic`/`scalar`/
unannotated; the numeric family (int/num/C-interop kinds) interconverts
silently. The predicted "latent mismatches" in the compiler/stdlib turned out
not to exist — the dogfood sweep found exactly one warning, a deliberate
mismatch in the int-coercion test. Still to harvest from Stage 0: feeding
`expr_type` into the CODEGEN (more provably-int expressions → more tagged-int
fast paths) — today the codegen still uses its own narrower
`expr_is_int_typed`.

**Stage 1 — structured type representation (the one real refactor).**
Replace the flat `int` tag with a small structured type node:
`{ kind, elem, nullable, name }`. Keep the int kinds as the `kind` field so
existing code keeps working; `type_to_string`/`parse_type` become the two
choke points. Do this once, deliberately — it enables everything below.

Minimal-churn variant (avoids touching the 22 `parse_type` call sites): keep
`parse_type` returning the **int kind** and have it also stash a richer
descriptor on the parser as a side-channel (`$parser->{"last_type_desc"}`);
only the ~3 declaration-building sites (var-decl, param, return) read it and
store it on the AST node next to `var_type`. Codegen never looks at it. A tiny
type library carries the logic: `type_array_of`, `type_nullable`,
`type_to_string`, `type_resolve_alias`, and the one relation that matters —
`type_compatible(expected, actual)`. Plain hashes/strings/recursion →
bootstrap-safe, no new builtins.

**Stage 2 — element-typed collections (`array<int>`, `hash<V>`).**
The generics that matter day-to-day. In `parse_type`, after a container kind,
peek for `<` and recursively parse the element (nests: `array<array<int>>`);
because `parse_type` only runs in type position there's zero ambiguity with the
comparison `<`/`>`. Keep hashes to `hash<V>` (keys are always strings). Checks
in `Semantic.strada`, element type **erased at runtime**:
- *Writes:* `push(@a, x)`, `$a[i] = x`, `$h{k} = x` → `type_compatible(elem, expr_type(x))`.
- *Reads:* `expr_type` of a subscript returns the element type, so
  `my int $n = $a[i]` checks and the type flows downstream.
- *Binders:* the `foreach`/`map`/`grep` loop var and `$_` get the element type,
  so the body is checked too (a big, cheap win).
- *Variance:* invariant for concrete elements (arrays are mutable), but
  **bivariant whenever either side is `dynamic`**, and bare `array` ≡
  `array<dynamic>` — so every existing untyped collection keeps compiling with
  no checking (the gradual escape). Free codegen win: typed element access
  becomes provably-int → more tagged-int fast paths.

**Stage 3 — nullable `int?` + type aliases (cheap once Stage 1 lands).**
`undef` is the #1 runtime crash, so nullable typing is the highest null-safety
win. Parse a trailing `?` (`int?`); flow-narrow via the existing `defined` /
`//` (`DEFINED_OR`) machinery so a `defined($x)` guard or `// default`
narrows `T?` to `T` inside the branch. Type aliases/newtypes ride the `name`
field added in Stage 1.

---

## Concurrency follow-ups (Tier-2 ergonomics)

With thread-safety and threaded cycle collection now shipped, the remaining
concurrency work is ergonomics, not correctness:

- `select` over multiple channels.
- Structured concurrency / nursery scope (scoped task groups with propagated
  cancellation and failure).
- `async::sleep`/`yield` that parks cooperatively instead of blocking a pool
  thread; cancellation-aware blocking primitives.
- Work-stealing / data-parallel `map`.
- Actor model.
- Language-level thread-local declaration (today only via the global registry).

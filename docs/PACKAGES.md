# Pacco — Strada Package Manager

Pacco is the canonical package manager (`tools/pacco/`, built with
`make pacco` → `tools/pacco/pacco`). `tools/stradapm/` is an older,
unfinished duplicate and is not maintained; use pacco.

## Quick start

```bash
make pacco
cd /tmp && pacco new demo && cd demo
pacco build            # compiles src/main.strada to .strada/build/demo
pacco run              # build + execute
pacco test             # compile-and-run every t/*.strada (exit 0 passes)
```

## Strada.toml

```toml
[package]
name = "demo"
version = "0.1.0"
description = "A Strada project"
authors = []
license = "MIT"

[dependencies]
# version-only (registry names are rejected until an index is published)
# json = "^1.0"
# git dependency, optional @branch/tag ref + version constraint
# mylib = { git = "https://github.com/user/mylib.git", version = "^0.2.0" }
# local directory (must contain its own Strada.toml)
# mylib = { path = "../mylib", version = "^0.1.0" }

[dev-dependencies]

[build]
type = "binary"   # binary (default) or shared; entry = "src/main.strada"
```

`pacco add` understands `path:<dir>`, `git:<url>[@ref]`,
`github:<user>/<repo>[@ref]`, `gitlab:<user>/<repo>[@ref]`,
and (rejected for now) bare registry names.
An optional second argument pins the version
(`pacco add path:../mylib ^0.1.0`).

## Layout

```
demo/
  Strada.toml          manifest (you edit)
  Strada.lock          pinned versions (generated, commit it)
  src/main.strada      entry point ([build].entry overrides)
  src/, lib/           your modules (auto-added as -L paths)
  t/*.strada           tests run by `pacco test`
  .strada/deps/<pkg>   fetched dependencies (do not edit)
  .strada/build/<bin>  build outputs (`pacco clean` removes these)
```

`Strada.lock` pins one `[package.<name>]` section per dependency
(version/source/constraint/path). `pacco install` reuses locked checkouts
when the pinned version is present.

## Notes

- Builds invoke `strada` with dependency `-L` paths automatically.
  Override the compiler with `PACCO_STRADA=/path/to/strada`.
- `pacco build --release` compiles with `-O2`.
- Fetching uses `git clone --depth 1` (needs git) and plain directory
  copies for `path:` deps. Downloads (registry index) require `curl`
  and `https://` URLs only.
- Dependency names must match `[A-Za-z0-9_][A-Za-z0-9_.-]*`; anything
  else is rejected before it can touch the filesystem.

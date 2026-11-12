# tree-sitter-strada

A [tree-sitter](https://tree-sitter.github.io/) grammar for the Strada
programming language.

## Status

Parses the full repository corpus with **zero error nodes**: all 245
programs in `examples/`, all 66 stdlib modules in `lib/` (including the
10k-line `Eval.strada`), and the compiler's own sources.

Covered: functions (incl. `fn`, `private`/`async`, extern, no-parens
forms via statement parsing), packages/OOP (`has`, `extends`, `with`,
`before`/`after`/`around`), all control flow (incl. `switch`/`case`,
labeled loops, statement modifiers), expressions with Perl-like
precedence, sigil variables/slices/dereferences, strings with
interpolation, `q//qq//qw()` in all delimiter styles (incl. one level of
balanced-pair nesting), regex forms (`m//`, `s///`, `tr///`, bare `/.../`
in expression positions), heredocs (`<<TAG`, `<<'TAG'`, `<<"TAG"`,
multiple per line), POD blocks, and `__C__` / `extern "C"` blocks as raw
regions with C injection for embedded highlighting.

The external scanner (`src/scanner.c`) handles the three context-sensitive
token classes: heredoc bodies (FIFO tag queue, serialized state), raw
balanced-brace C bodies (C-string/comment aware), and column-0 POD.

Known gaps: interpolation inside heredoc bodies is not itemized (the body
is one token), and `s///e` replacement code is not sub-parsed.

## Editor setup

**Neovim** (nvim-treesitter):

```lua
local parser_config = require("nvim-treesitter.parsers").get_parser_configs()
parser_config.strada = {
  install_info = {
    url = "/path/to/strada-lang/editors/tree-sitter-strada",
    files = { "src/parser.c", "src/scanner.c" },
  },
  filetype = "strada",
}
vim.filetype.add({ extension = { strada = "strada" } })
```

Then `:TSInstall strada`. Copy `queries/` to
`~/.config/nvim/queries/strada/` (or point `runtimepath` at a directory
containing them) for highlighting and C injection.

**Helix** (`languages.toml`):

```toml
[[language]]
name = "strada"
scope = "source.strada"
file-types = ["strada"]
comment-token = "#"

[[grammar]]
name = "strada"
source = { path = "/path/to/strada-lang/editors/tree-sitter-strada" }
```

Then `hx --grammar build`, and copy `queries/` to
`~/.config/helix/runtime/queries/strada/`.

## Development

```sh
npx tree-sitter-cli generate      # grammar.js -> src/parser.c
npx tree-sitter-cli parse FILE    # print the tree
```

`src/parser.c` is generated but committed, so consumers don't need the
tree-sitter CLI.

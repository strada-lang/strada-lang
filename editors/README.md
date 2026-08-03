# Editor Support for Strada

Syntax highlighting, indentation, and compiler integration for Vim and
Emacs. Both wire the compiler's `--check` mode (validation only — no C
generated, no cc run) into the editor's error-jumping workflow, using the
compiler's gcc-style `file:line:col:` diagnostics.

## Vim

The `vim/` directory is a standard runtime tree (`syntax/`, `ftdetect/`,
`ftplugin/`, `indent/`).

**Native packages (Vim 8+):**

```sh
mkdir -p ~/.vim/pack/strada/start
ln -s /path/to/strada-lang/editors/vim ~/.vim/pack/strada/start/strada
```

**Or add to runtimepath** in `~/.vimrc`:

```vim
set runtimepath^=/path/to/strada-lang/editors/vim
```

**Or vim-plug:**

```vim
Plug 'strada-lang/strada-lang', { 'rtp': 'editors/vim' }
```

What you get:

- Highlighting: keywords, types, sigil variables, interpolation, heredocs,
  `q//qq//qw()`, regex (`m//`, `s///`, `tr///`), POD, labels, namespaced
  builtins (`core::`, `re::`, ...), and `__C__ { ... }` blocks highlighted
  as embedded C.
- `:make` runs `strada --check %` — errors (including multiple errors per
  run) land in the quickfix list; `:cn`/`:cp` jump by file:line:col. The
  source-line/caret snippet lines are filtered out automatically.
- Brace-based indentation.

## Emacs

```elisp
(add-to-list 'load-path "/path/to/strada-lang/editors/emacs")
(require 'strada-mode)
```

Or with `use-package`:

```elisp
(use-package strada-mode
  :load-path "/path/to/strada-lang/editors/emacs"
  :mode "\\.strada\\'")
```

What you get:

- `strada-mode` on `.strada` files: font-lock (keywords, types, builtins,
  variables, namespaced calls), `#` and `/* */` comments, brace-based
  indentation (`strada-indent-offset`, default 4).
- `C-c C-c` — `strada-check`: run `strada --check` on the current file in
  a compilation buffer. The compiler's `file:line:col:` errors match
  compilation-mode's built-in GNU pattern, so `M-g n` / `M-g p`
  (next-error/previous-error) jump straight to each error.
- `C-c C-r` — `strada-run`: compile and run (`strada -r`).
- `strada-program` customizes the compiler command (default `strada` from
  `PATH`).

## Notes

- Heredoc and `s///e` interior highlighting is approximate in both editors
  (full fidelity needs the real lexer; this is a v1).
- The Emacs mode does not yet fontify regex literals or POD blocks.

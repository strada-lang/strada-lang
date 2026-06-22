# Template — a Strada port of Perl's Template::Toolkit (TT2)

`lib/Template.strada` and `lib/Template/*.strada` implement a faithful port of
Perl's [Template::Toolkit](http://template-toolkit.org/) (TT2/TT3). Output is
validated **byte-identical** against real Perl TT 3.106 via a differential test
corpus (see [Testing](#testing)).

```strada
use Template;

my $tt  = Template::new({ INCLUDE_PATH => "templates", STRICT => 0 });
my $out = $tt->process("page.tt", { name => "World", items => [1, 2, 3] });

# inline, no file lookup:
my $s = $tt->process_string("[% name %]!", { name => "Hi" });

# one-shot convenience:
my $s2 = Template::render("[% 2 + 3 %]", {});
```

`process` resolves `name` through `INCLUDE_PATH`; `process_string` renders a
literal string. Both return the rendered text (and throw a `Template::Exception`
on error — see [Error handling](#error-handling)).

## Architecture

`use Template;` pulls in one translation unit composed of:

| File | Role |
|---|---|
| `Template.strada` | Facade: `new`/`process`/`process_string`, config, provider (INCLUDE_PATH + text/AST cache), the compile→execute pipeline. |
| `Template/Lexer.strada` | Scanner: split into TEXT / `[% … %]` directive chunks; comments; whitespace chomping (`-`/`+`/`~`, `PRE_CHOMP`/`POST_CHOMP`). |
| `Template/Parser.strada` | Chunk stream → directive AST; precedence-aware statement/block parsing. |
| `Template/Expr.strada` | Expression lexer + precedence-climbing parser + evaluator; filter-pipe parsing. |
| `Template/Stash.strada` | Scope frames + the polymorphic dot-operator; truthiness; stringification. |
| `Template/VMethods.strada` | Scalar / list / hash virtual methods. |
| `Template/Filters.strada` | The filter library. |
| `Template/Interp.strada` | Directive interpreter (control flow, composition, TRY/CATCH). |
| `Template/Plugins.strada` | `USE` framework + starter plugins. |
| `Template/Exception.strada` | Exceptions + internal control signals. |

Two-tier like Forma: `compile()` produces a cached node list; `execute()` walks
it against a stash, accumulating output in a `sb::` StringBuilder.

## Directives

```
[% GET expr %]   [% expr %]            evaluate + output
[% CALL expr %]                        evaluate, output nothing
[% SET a = 1, b = 2 %]   [% a = 1 %]   assignment (dotted lvalues autoviv)
[% DEFAULT a = 1 %]                    assign only if currently false
[% IF x %]…[% ELSIF y %]…[% ELSE %]…[% END %]
[% UNLESS x %]…[% END %]
[% FOREACH x IN list %]…[% END %]      + the `loop` iterator
[% WHILE cond %]…[% END %]
[% SWITCH v %][% CASE 1 %]…[% CASE %]…[% END %]
[% NEXT %] [% LAST %] [% STOP %] [% RETURN %] [% CLEAR %]
[% directive IF cond %]                postfix IF / UNLESS modifiers
[% FILTER name %]…[% END %]            filter a block
[% BLOCK name %]…[% END %]             define a reusable fragment (hoisted)
[% INCLUDE name [k=v] %]               process (localized scope)
[% PROCESS name [k=v] %]               process (shared scope)
[% WRAPPER name [k=v] %]…[% END %]     wrap body as `content`
[% INSERT name %]                      raw file, no processing
[% MACRO name(a,b) BLOCK %]…[% END %]  define a macro (also `MACRO n GET …`)
[% TRY %]…[% CATCH t %]…[% FINAL %]…[% END %]
[% THROW type info %]
[% USE Plugin(args) %]   [% USE alias = Plugin(args) %]
[% META key = "v" %]                   → template.key
[% DEBUG … %]                          no-op (debug output off by default)
[%# comment %]
```

The `loop` variable inside `FOREACH`: `index` (0-based), `count` (1-based),
`first`/`last` (1/0), `size`, `max`, `odd`/`even` (1/0), `parity`
("odd"/"even"), `prev`, `next`.

## Expressions

Precedence (loosest → tightest): filter `|` / `FILTER`; ternary `? :`;
`or`/`||`; `and`/`&&`; `==`/`!=`/`<`/`>`/`<=`/`>=`; range `..`; concat `_`;
`+`/`-`; `*`/`/`/`%`/`div`/`mod`; unary `! not -`; postfix `.`/`[]`/`()`.

- Comparisons are **smart**: numeric if both operands look numeric, else string.
  Booleans render `1` (true) / `""` (false), matching Perl.
- Truthiness (TT rules): `undef`, `""`, `"0"`, empty list, empty hash are false;
  `"0.0"` is true.
- List `[1, 2, 3]`, range `[1..5]`, hash `{ a = 1, b = 2 }` literals.
- The dot-operator resolves `obj.key`: object method → hash key → list index →
  virtual method. `obj.key(args)` and `obj.0` / `obj.$var` supported.

## Virtual methods

**Scalar**: `length size defined upper lower ucfirst lcfirst trim collapse html
xml repeat(n) replace(s,r) remove(p) search(p) match(p) split(p) substr(o[,l])
chunk(n) list hash`.

**List**: `size max first[(n)] last[(n)] join([sep]) reverse sort[(key)]
nsort[(key)] unique grep(p) merge(…) slice(a,b) item(n) list hash push pop
shift unshift`.

**Hash**: `keys values size each items pairs list sort nsort item(k) defined(k)
exists(k) delete(k)`. (`keys`/`values`/`items`/`each` are in raw hash order,
faithful to TT; `pairs`/`sort`/`nsort` are ordered.)

## Filters

`html xml upper lower ucfirst lcfirst trim collapse uri url null repeat(n)
replace(s,r) remove(p) format(fmt) truncate(n[,suffix]) indent(pad)`.

Applied via `[% x | name(args) %]`, the `x FILTER name` infix, or the
`[% FILTER name %]…[% END %]` block; filters chain (`x | a | b`).

## Plugins (`USE`)

- **Math** — `sqrt abs int floor ceil cos sin pow pi e`.
- **String** — a mutable buffer: `text length upper lower trim append prepend
  repeat` (methods mutate in place and chain; stringifies to its text).
- **format** — `USE f = format("%.2f")` then `f(n)`; or `USE format` then
  `format(fmt)` → a formatter.
- **date** — `date.format(epoch, fmt, "GMT")` (UTC; `%Y %m %d %H %M %S %y %b`).
- **List** — stub.

## Error handling

Exceptions are hashrefs `{ type, info }`; inside `CATCH`, `error` exposes
`error.type` / `error.info` and stringifies as `"type error - info"`. `CATCH t`
matches `t` and any `t.*` (dotted prefix); bare `CATCH` is the fallback; `FINAL`
always runs. `STRICT => 1` throws `var.undef` on undefined-variable access.

## Configuration

`INCLUDE_PATH` (string or list), `START_TAG`/`END_TAG`, `TAG_STYLE`
(`template`/`star`/`php`/`asp`/`metatext`/`html`), `PRE_CHOMP`/`POST_CHOMP`,
`STRICT`, `MAX_RECURSION`.

Caching (source text + compiled AST, keyed by template name) is on by default and
shared between `process()` and `INCLUDE`/`PROCESS`. Cached files are **mtime-
checked** on each load: a template edited on disk is reloaded and recompiled
automatically. Controls: `Template::set_cache(0)` disables caching entirely;
`Template::set_stat_check(0)` keeps caching but skips the mtime re-check (one
fewer `stat()` per load, edits not noticed until `clear_cache()`);
`Template::clear_cache()` flushes everything.

## Differences from Perl TT / deferred

- **Deferred**: `PERL`/`RAWPERL`/`EVAL_PERL`, `VIEW`, the mid-template
  `[% TAGS %]` directive (the engine lexes the delimiters up front), `Iterator`/
  `Table`/full `List` plugins, the broad third-party plugin ecosystem, and the
  on-disk `COMPILE_DIR` cache.
- **Faithful quirks reproduced**: smart comparisons, `1`/`""` booleans,
  unordered hash `keys`, mutable `String` plugin, single-arg `THROW`
  (`type="undef"`), `loop` var leaking its last value, FOREACH has no `ELSE`.

## Testing

Differential corpus under `t/template/cases/` — each `<case>.tt` (+ optional
`<case>.json` vars, `<case>.cfg` engine config) is rendered by **both** real
Perl TT and the Strada engine and asserted byte-identical:

```sh
bash t/template/run_diff.sh            # generate goldens (Perl TT) + diff
```

`strict_*` cases run with `STRICT`; a `<case>.cfg` JSON merges extra config.
Perl TT lives at `/opt/bzperl` (`PERL5LIB` is set by the harness).

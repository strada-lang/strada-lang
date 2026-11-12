# Known Issues

Intentional behavioural differences from Perl and unsupported syntax, as of
2026-11-12.

For bugs that were fixed, read the regression tests in `t/bugfix_tests/` (run
by `t/t_bugfixes.sh` / `./t/run_tests.sh bugfix`): each file's header comment
describes what used to go wrong.

## Intentional Design Differences

### String character vs byte semantics

Valid non-ASCII UTF-8 string literals (for example `"café"` or `"\x{E9}"`) and
strings produced by `chr()` are flagged as character strings, so `length()`,
`substr()` and `index()` count Unicode code points (`length("café")` is `4`;
`core::byte_length("café")` is `5`). Raw byte reads from files and sockets
(`core::slurp`, `core::fread`, `<$fh>`), `utf8::encode($s)`, and invalid-UTF-8
byte escapes (`"\xFF"`) remain byte-oriented until validated in place with
`utf8::decode($s)`.

## Syntax Perl Has and Strada Does Not

Not bugs, but a common source of parse errors when writing from Perl habit:

- `and`, `or`, `not` (use `&&`, `||`, `!`)
- `&=`, `|=`
- `0o77`, `0b1010`, and scientific notation (`1.5e10`); octal is `077`
- `while (defined(my str $line = <$fh>))` (declare the variable before the loop)
- a `for`/`foreach` statement modifier (`say($_) for @list;`)
- `throw EXPR if COND;` (wrap the `throw` in an `if` block)
- `num(...)` and `str(...)` as conversion functions

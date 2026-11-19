# Known Issues

Intentional behavioural differences from Perl and unsupported syntax, as of
2026-11-19.

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

- `state` variables (use an `our` variable in the package)
- `__DATA__` / `__END__` sections and the `DATA` handle
- postfix dereference (`$r->@*`, `$r->%*`); write `@{$r}`, `%{$r}`
- `given`/`when`
- `my` inside an expression other than an if/unless/elsif/while/until
  condition (`foo(my $x = 1)`)
- `num(...)` and `str(...)` as conversion functions
- `"$name"` interpolation: Strada interpolates only `"${name}"`

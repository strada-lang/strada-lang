---
name: strada
description: Write, compile, run, test and debug programs in Strada, a strongly typed Perl-inspired language that compiles to C and native executables (.strada files, the strada/stradac compiler, strada-interp, __C__ blocks, Cannoli web apps). Use whenever the user mentions Strada or works with .strada files, even if they only say "write a script" inside a Strada project.
---

# Programming in Strada

Strada looks like Perl but is compiled: a `.strada` file goes through the `strada`
driver to C, then gcc, then a native binary. Most Perl intuition carries over; the
differences below are the ones that break generated code. When unsure, check
`reference/language.md` (the full language) or `reference/builtins.md` (every
builtin), and let the compiler settle it: it is fast and its errors are precise.

## First: is the toolchain there?

Run `scripts/check-toolchain.sh` once per session before relying on the compiler.
It finds `strada` (on PATH, or `./strada` in a strada-lang checkout), checks for a
C compiler, and compiles a hello-world. If it reports Strada missing, tell the
user how to install it (below) instead of guessing at output.

```bash
git clone https://github.com/strada-lang/strada-lang.git
cd strada-lang && ./configure && make && sudo make install   # installs to /usr/local
```

In a strada-lang checkout use `./strada` (the dev tree); elsewhere use `strada`.

## The working loop

1. Write the program. A complete program has `func main() int { ... return 0; }`;
   the return value is the exit status. For command-line arguments write
   `func main(int $argc, array @argv) int` (C-style: `$argv[0]` is the program)
   or call `core::argv()` (arguments only). No executable statements outside
   functions: only declarations (`func`, `package`, `has`, `our`, `const`,
   `enum`, `use`, `link_lib`, `BEGIN`/`END`) at top level.
2. `strada --check prog.strada`: parse + type/semantic check, no C, no gcc. It
   reports every error with `file:line:col:` and a caret. Fix all, recheck.
3. `scripts/strada-run.sh prog.strada ARGS...` (in this skill's folder): builds
   at `-O0` into a temp dir, runs with ARGS, deletes the binary, and exits with
   the program's status. By hand: `strada -O0 -o /tmp/prog prog.strada && /tmp/prog`.
   Avoid `strada -r` in user directories: it leaves the binary next to the
   source, named after it (a `test.strada` then shadows `test` there).
4. Release builds: plain `strada prog.strada` (`-O2` with LTO is the default),
   `-O3` adds `-march=native`.
5. Tests: `use Test;` (no `use lib` needed) then
   `Test::ok/is/isnt/like/unlike/is_num/pass/fail/diag`; `like` takes the pattern
   as a string. End `main` with `return Test::done_testing();`. It prints TAP,
   so `prove --exec <skill>/scripts/strada-run.sh t/*.strada` runs a directory.
6. Code with `__C__` blocks: check leaks with
   `valgrind --leak-check=full /tmp/prog 2>&1 | grep -A5 "LEAK SUMMARY"`.

No compile step is needed to try something: `strada-interp prog.strada` runs a
program on the bytecode VM, and `strada --repl` opens a REPL.

## Where Perl habits go wrong

- **Interpolation is `${name}` only.** `"Hello, $name"` prints `$name` literally.
  Write `"Hello, ${name}"`. Elements, method calls and expressions never
  interpolate: use `.` or `sprintf`. Regex patterns and `s///` replacements are
  the exception: there `$name` interpolates too.
- **Program entry is `main`.** Top-level statements are a compile error.
- **Types.** `my int $i`, `my num $x`, `my str $s`, `my scalar $v`, `my array @a`,
  `my hash %h`. Types are optional (the sigil gives a default), but `int` on
  counters and parameters makes arithmetic allocation-free. An `int` variable
  coerces what it receives (`my int $i = "12abc"` is 12). Function signatures:
  `func add(int $a, int $b) int { ... }`; the return type follows the `)`.
- **Checked types.** A file that starts with `use strict "types";` turns type
  mismatches into compile errors there (initializers, assignments, arguments,
  returns, container elements). Then `num` to `int` needs `int(...)`, text to
  a number needs `$s + 0`, a number to text needs `"" . $n`. Containers may
  declare what they hold: `my array<int> @ids`, `my hash<str, num> %price`.
  `scalar` and unannotated code are never checked. Keep the pragma when a
  file already has it; add it to new files only if the project uses it.
- **Element access uses `$`**: `$a[0]`, `$h{"k"}`, `$r->[0]`, `$r->{"k"}`.
  `@a[0]` is a slice.
- **Namespaced builtins.** System and libc calls are `core::` (`core::open`,
  `core::slurp`, `core::spew`, `core::qx`, `core::system`, `core::getenv`,
  `core::fork`), math is `math::`, concurrency is `async::`, raw memory is `c::`.
  No `use` is needed for these. An unknown `core::name` is a compile error, so
  look in `reference/builtins.md` rather than guessing a name.
- **Files.** `my $fh = core::open($path, "r");` (modes `r w a rb < > >>`),
  `my $line = <$fh>;` (newline already stripped, undef at EOF),
  `say($fh, "text")`, `core::close($fh)`. Whole files: `core::slurp` (undef if
  the file can't be read) / `core::spew` (returns 1 on success, 0 on failure).
- **No `qr//`.** Pass patterns as strings: `$s =~ /$pat/` with `my str $pat`.
  `split` always takes a regex, even from a string: `split(".", $s)` splits on
  every character; write `split(/\./, $s)`.
- **Scalar context gives counts**: `my $n = @a`, `my $n = keys(%h)`,
  `scalar(@a)`. Keep a list in a scalar as a reference: `my $ks = [keys(%h)]`.
- **Strings vs bytes.** `chr(255)` is the character U+00FF (two UTF-8 bytes);
  `core::byte(255)` is one raw byte. Build binary data with `core::byte`,
  `core::pack`, never `"\xff"` or `chr`. Byte-level helpers: `core::byte_length`,
  `core::byte_substr`, `core::get_byte`.
- **Exceptions** are `try { } catch (Type $e) { } catch ($e) { } finally { }` with
  `throw EXPR` or `die(...)`. Perl's `eval { }` / `$@` is not the idiom.
- **`core::waitpid` returns the raw status** (`$status >> 8` is the exit code);
  `core::system` likewise; `core::system_argv($prog, \@argv)` (no shell, `argv[0]` is
  the program name) returns the exit code itself.
- Statement modifiers are `if`, `unless`, `while`, `until`, `for` and
  `foreach` (`say($_) for @list;`). `my` may appear inside an if/while
  condition (`while (defined(my $l = <$fh>))`), but not in other
  expressions.
- `||` treats `0` and `""` as false; use `//` for "default when undef".
- Hash keys: quote them (`$h{"name"}`); `k => v` pairs build hashes as in Perl.

## Shapes worth copying

```strada
package Point;
has ro num $x (required);           # getter x()
has rw num $y = 0;                  # getter y(), setter set_y()

func dist(scalar $self) num {
    return math::sqrt($self->x() ** 2 + $self->y() ** 2);
}

package main;

func main() int {
    my $p = Point::new("x", 3, "y", 4);   # or Point::new(x => 3, y => 4)
    say("dist=" . $p->dist());

    my hash %count;
    foreach my $w (split(/\s+/, "a b a c a")) { $count{$w}++; }
    foreach my $k (sort { $count{$b} <=> $count{$a} } keys(%count)) {
        say("${k}: " . $count{$k});
    }

    my $add = func (int $a, int $b) int { return $a + $b; };
    say($add->(2, 3));

    try {
        die("bad input") unless defined(core::getenv("HOME"));
    } catch ($e) {
        say(STDERR, "error: " . $e);
        return 1;
    }
    return 0;
}
```

OOP also has `extends Parent;`, roles via `with`, `before`/`after`/`around`
method modifiers, `(lazy)`/`(builder => "m")` attributes, `use overload`, and plain
`bless` when full control is needed. Closures share captured variables with the
enclosing scope, as in Perl. See `reference/language.md` for each.

## C interop (`__C__` blocks)

Strada variables are `StradaValue*` inside a `__C__` block.

- `strada_to_str(v)` returns a malloc'd `char*`: always `free()` it.
- `strada_to_int` / `strada_to_num` return plain values.
- To hand a value back, declare it in Strada, replace it in C, return it:
  ```strada
  func c_len(str $s) int {
      my int $n = 0;
      __C__ { strada_decref(n); n = strada_new_int((int64_t)strada_str_len(s)); }
      return $n;
  }
  ```
  Always `strada_decref` the old value before reassigning.
- Binary data: `strada_str_len(v)` is the byte length and `strada_to_str` copies
  every byte, NULs included (so never `strlen` it); build results with
  `strada_new_str_len(buf, n)`.
- Never read `v->type` / `v->value` directly: small integers are tagged pointers.
  Use the conversion functions, or check `STRADA_IS_TAGGED_INT(v)` first.
- Link C libraries with `link_lib "z";` in the source or `strada f.strada -- -lz`.

Run valgrind on any program that has `__C__` code.

## Reading the references

`reference/language.md` is long. Read just the section the task needs. Its
headings include: Variables and sigils, Operators, Control flow, Functions,
Strings, Arrays, Hashes, References, Regular expressions, OOP, Exception
handling, File I/O, Async/threading, Modules, `__C__` blocks, `tie`,
Common idioms, Common gotchas, Performance tips, Extended examples.
`grep -n '^## \|^### ' reference/language.md` lists them with line numbers.

`reference/builtins.md` lists every builtin by namespace; grep it for a name
before using one you have not seen in this file.

## Before handing code back

- `strada --check` is clean, and the program compiled and ran on the cases you
  care about (show the output when it matters).
- Interpolation uses `${name}`; no top-level statements; `core::` for system calls.
- Error paths: failed `core::open` / `core::slurp` return undef; check them.
- `__C__` code: every `strada_to_str` is freed, valgrind is clean.

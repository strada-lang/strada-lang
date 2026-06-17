# Strada Programming Quick Memory

Single-file, compact reminder for day-to-day Strada programming.

For full details, see:
- `docs/QUICK_REFERENCE.md` — complete syntax reference
- `docs/extended/sys.md` — core:: namespace (file I/O, process, sockets, etc.)
- `docs/extended/regex.md` — regular expressions
- `docs/extended/types.md` — type system and casting
- `docs/extended/async.md` — async/await and concurrency
- `docs/extended/filehandles.md` — file handle operations
- `docs/extended/math.md` — math:: namespace
- `docs/extended/memory.md` — memory management
- `docs/extended/repl.md` — REPL usage

## Build and run

```bash
# Build compiler
make

# One-step compile (creates ./program)
./strada program.strada
./strada -r program.strada     # compile + run
./strada -c program.strada     # keep generated .c
./strada -g program.strada     # debug symbols
./strada -w program.strada     # warnings (unused vars)

# Manual two-step
./stradac program.strada program.c
gcc -o program program.c runtime/strada_runtime.c -Iruntime -ldl

# Interpreter / REPL
./strada-interp program.strada  # run without compilation
./strada-interp                 # REPL

# Perla (Perl 5 compiler)
./perla script.pl               # compile Perl to native
./perla --vm script.pl          # run Perl via VM
```

## Minimal program

```strada
func main() int {
    say("Hello, Strada!");
    return 0;
}
```

### main arguments

```strada
func main(int $argc, array @argv) int {
    say("argc=" . $argc);
    if ($argc > 0) { say("argv[0]=" . $argv[0]); }
    return 0;
}
```

## Types, sigils, and declarations

```strada
my int $count = 0;
my num $price = 19.99;
my str $name = "Alice";
my array @items = (1, 2);
my hash %cfg = { "debug" => 1 };
my scalar $any = $count;  # scalar can hold any single value

# Pre-allocated collections
my array @big[1000];
my hash %cache[500];

# Package-scoped globals (our) - backed by global registry
our int $count = 0;           # Registry key: "main::count"
our str $name = "hello";
```

Basic types: `int`, `num`, `str`, `array`, `hash`, `scalar`, `void`.
Fixed-width types for C interop: `int8/16/32/64`, `uint8/16/32/64`, `float`,
`double`, `size_t`, `byte`.

## Literals and data structures

```strada
my array @a = (1, 2, 3);
my hash %h = { "a" => 1, "b" => 2 };

# Anonymous refs
my scalar $aref = [1, 2, 3];
my scalar $href = { a => 1, b => 2 };

# References to existing variables
my scalar $r1 = \$count;
my scalar $r2 = \@a;
my scalar $r3 = \%h;
```

## Enums

```strada
enum Status { PENDING, ACTIVE = 10, DONE }
my int $st = Status::ACTIVE;
```

## Operators and truthiness

Arithmetic: `+ - * / % **`
String concat: `.`   String repeat: `"ab" x 3`
Numeric compare: `== != < > <= >=`
String compare: `eq ne lt gt le ge`
Logic: `&& || !`   Defined-or: `//`
Assignment: `= += -= *= /= .= //=`
Truthiness: `0`, `0.0`, `""`, `undef` are false; everything else true.

## Control flow

```strada
if ($x > 0) { ... }
elsif ($x < 0) { ... }    # or: else if ($x < 0) { ... }
else { ... }

while ($i < 10) { $i = $i + 1; }
for (my int $i = 0; $i < 10; $i = $i + 1) { ... }
foreach my str $s (@items) { ... }

last;   # break
next;   # continue

# Statement modifiers
say("hello") if $verbose;
$i++ while $i < 10;

switch ($code) {
    case 200 { say("OK"); }
    case 404 { say("Not found"); }
    default { say("Other"); }
}
```

## Functions and closures

```strada
func add(int $a, int $b) int { return $a + $b; }

func greet(str $name, str $msg = "Hello") void {
    say($msg . ", " . $name);
}

# Variadic (collects into array)
func sum(int ...@nums) int {
    my int $total = 0;
    foreach my int $n (@nums) { $total = $total + $n; }
    return $total;
}

# Anonymous function (closure)
my int $x = 10;
my scalar $f = func (int $n) int { return $n * $x; };
my int $y = $f->(5);  # 50

# fn is an alias for func
fn double(int $n) int { return $n * 2; }
```

## Arrays and hashes

```strada
push(@arr, "x");
my scalar $last = pop(@arr);
my scalar $first = shift(@arr);
unshift(@arr, "x");
splice(@arr, $offset, $length);

my int $len = size(@arr);
my scalar $el = $arr[0];
reverse(@arr);
my array @sorted = sort(@arr);

my array @keys = keys(%h);
my array @vals = values(%h);
if (exists(%h, "k")) { ... }
delete($h{"k"});

# Slices
my array @subset = @data[0, 2, 4];
my array @vals = @hash{"key1", "key2"};

# Functional
my array @doubled = map { $_ * 2 } @nums;
my array @evens = grep { $_ % 2 == 0 } @nums;
```

## References and deref

```strada
my scalar $ref = \$var;
my scalar $val = $$ref;
$$ref = "new";

my scalar $aref = \@arr;
$aref->[0] = 99;

my scalar $href = \%h;
$href->{"k"} = "v";
```

## Strings and regex

```strada
length($s); substr($s, 0, 5); index($s, "sub"); rindex($s, "sub");
uc($s); lc($s); trim($s); chomp($s);
join(",", @arr); split(",", $csv); sprintf("%05d", 42);
chr(65); ord("A");

if ($s =~ /pattern/) { ... }
if ($s !~ /pattern/) { ... }
$s =~ s/old/new/;    # first
$s =~ s/old/new/g;   # all
$s =~ tr/a-z/A-Z/;   # transliteration

if ($s =~ /(\d+)-(\d+)/) {
    say($1);                       # capture variable
    my array @cap = captures();    # [0]=full match
}
```

## I/O and filehandles

```strada
say("line");                               # newline
print("no newline");
printf("%s: %d", $name, $n);

my str $all = core::slurp("file.txt");
core::spew("out.txt", $all);

my scalar $fh = core::open("file.txt", "r");
my str $one = <$fh>;                       # diamond operator
core::close($fh);
```

## Errors and exceptions

```strada
try {
    risky();
} catch (TypeError $e) { say($e); }    # typed catch
catch ($e) { say("Error: " . $e); }     # catch-all

throw "Bad input";
die("Fatal");
```

## Packages, modules, and OOP

```strada
use Math::Utils;
use lib "lib";

package Dog;
extends Animal;
has ro str $name (required);
has rw int $energy = 100;

func speak(scalar $self) void {
    say($self->name() . " says woof");
}

# No-parens form (Perl-style)
func bark { my $self = shift; say($self->name() . "!"); }

package main;
my scalar $dog = Dog::new("name", "Rex");
$dog->speak();
say($dog->isa("Animal"));  # 1

# Dynamic method dispatch
my str $method = "speak";
$dog->$method();
```

Features: `has ro|rw`, `extends`, `with` (roles), `before`/`after`/`around` hooks, `(required)`, `(lazy)`, `bless`, `isa`, `can`, `AUTOLOAD`, `use overload`.

## Weak references

```strada
core::weaken($ref);                # Make $ref weak
core::isweak($ref);                # Returns 1 if weak
core::weaken($child->{"parent"});  # Break circular reference
```

## Library linkage

- `import_lib "X.so"` (runtime dlopen)
- `import_object "X.o"` (static link)
- `import_archive "X.a"` (static link + runtime)

## Async and concurrency (thread pool)

```strada
async func work(int $n) int { return $n * $n; }
my scalar $f1 = work(10);
my int $a = await $f1;

my array @results = await async::all(\@futures);
my str $first = await async::race(\@futures);

my scalar $ch = async::channel(10);
async::send($ch, "msg");
my str $msg = async::recv($ch);

my scalar $m = async::mutex();
async::lock($m); async::unlock($m);
```

## C interop

```strada
__C__ { #include <math.h> }

func c_add(int $a, int $b) int {
    __C__ {
        int64_t va = strada_to_int(a);
        int64_t vb = strada_to_int(b);
        return strada_new_int(va + vb);
    }
}
```

## Handy built-ins

Output: `say`, `print`, `printf`, `warn`, `die`
Arrays: `push`, `pop`, `shift`, `unshift`, `size`, `splice`, `map`, `grep`, `sort`, `reverse`
Hashes: `keys`, `values`, `exists`, `delete`, `each`
Strings: `length`, `substr`, `index`, `rindex`, `uc`, `lc`, `trim`, `chomp`, `join`, `split`, `sprintf`
Regex: `match`, `replace`, `replace_all`, `captures`, `named_captures`
Refs: `ref`, `core::weaken`, `core::isweak`
Types: `defined`, `typeof`, `isa`

## core:: and math:: highlights

`core::open`, `core::close`, `core::slurp`, `core::spew`, `core::qx`,
`core::fork`, `core::wait`, `core::sleep`, `core::time`, `core::getenv`,
`core::socket_client`, `core::socket_server`, `core::socket_accept`,
`core::caller`, `core::stack_trace`, `core::signal`

`math::sin`, `math::cos`, `math::sqrt`, `math::pow`, `math::abs`, `math::rand`

---

# Extended Reference

## core:: Namespace — Full API

### File I/O

`open($file, $mode) → fh` — modes: `"r"`, `"w"`, `"a"`, `"rb"`, `"<"`, `">"`, `">>"`, `"rw"`
`close($fh)` | `slurp($file) → str` | `slurp_fh($fh) → str` | `spew($file, $str)`
`readline($fh) → str` | `fread($fh, $size) → str` | `fwrite($fh, $data, $size) → int`
`tell($fh) → int` | `seek($fh, $offset, $whence)` — 0=SET, 1=CUR, 2=END
`rewind($fh)` | `eof($fh) → 1/0` | `flush($fh)` | `fileno($fh) → int`
`open_str($content, $mode) → fh` — in-memory I/O | `str_from_fh($fh) → str`
`open(\$var, $mode) → fh` — reference-style, updates $var on close

### File System

`unlink($file)` | `rename($old, $new)` | `mkdir($path, $mode)` | `rmdir($path)`
`chmod($path, $mode)` | `chown($path, $uid, $gid)` | `utime($path, $atime, $mtime)`
`stat($path) → hash` — keys: size, mode, uid, gid, atime, mtime, ctime, nlink, dev, ino
`lstat($path) → hash` — same but doesn't follow symlinks
`readdir($path) → array` | `readdir_full($path) → array` (full paths)
`is_dir($path) → 1/0` | `is_file($path) → 1/0` | `file_size($path) → int`
`realpath($path) → str` | `dirname($path) → str` | `basename($path) → str`
`file_ext($path) → str` | `glob($pattern) → array` | `fnmatch($pattern, $str) → 1/0`
`link($old, $new)` | `symlink($target, $link)` | `readlink($link) → str`
`truncate($path, $len)` | `access($path, $mode) → int`

### Process Control

`fork() → int` — 0 in child, PID in parent | `exec($prog, @args)` — replaces process
`system($cmd) → int` | `qx($cmd) → str` — capture output
`wait() → int` | `waitpid($pid, $opts) → int`
`getpid() → int` | `getppid() → int` | `kill($pid, $sig)` | `_exit($code)`
`sleep($secs)` | `usleep($μsecs)` | `nanosleep($secs, $nsecs)`
`setprocname($name)` | `getprocname() → str`

### Signals

`signal($name, \&handler|"IGNORE"|"DEFAULT")` — $name: "INT", "TERM", "HUP", etc.
`alarm($secs) → int` | `raise($sig)` | `killpg($pgrp, $sig)` | `pause()`

### Time

`time() → int` | `hires_time() → num` (microsecond precision)
`localtime($ts) → hash` — keys: sec, min, hour, mday, mon(0-11), year(+1900), wday, yday
`gmtime($ts) → hash` | `mktime(%tm) → int` | `strftime($fmt, %tm) → str`
`gettimeofday() → [$secs, $μsecs]` | `clock() → int`

### Sockets

`socket_client($host, $port) → sock` | `socket_server($port) → sock`
`socket_server_backlog($port, $backlog) → sock`
`socket_accept($srv) → client` | `socket_close($sock)`
`socket_recv($sock, $len) → str` | `socket_send($sock, $data) → int`
`socket_select($sock, $ms) → int` | `socket_flush($sock)` | `socket_fd($sock) → int`
`select_fds(@rfds, @wfds, $ms) → array` | `poll(@fds, $ms) → array`
`setsockopt($sock, $level, $opt, $val)` | `shutdown($sock, $how)` — 0=rd, 1=wr, 2=both
`getpeername($sock) → str` | `getsockname($sock) → str`

### DNS/Network

`gethostbyname($host) → str` | `gethostbyname_all($host) → array`
`gethostname() → str` | `getaddrinfo($host, $svc) → array`
`inet_pton($af, $addr) → str` | `inet_ntop($af, $bin) → str`
`htons($v) → int` | `htonl($v) → int` | `ntohs($v) → int` | `ntohl($v) → int`

### Pipes/IPC

`popen($cmd, $mode) → pipe` | `pclose($pipe) → int`
`pipe() → [$rd, $wr]` | `dup2($old, $new)` | `dup($fd) → int`
`read_fd($fd, $cnt) → str` | `write_fd($fd, $data) → int` | `read_all_fd($fd) → str`

### Environment

`getenv($name) → str` | `setenv($name, $val)` | `unsetenv($name)`

### Users/Groups

`getuid()` | `geteuid()` | `getgid()` | `getegid()` | `setuid($uid)` | `setgid($gid)`
`getpwnam($user) → hash` | `getpwuid($uid) → hash` — keys: name, uid, gid, dir, shell
`getgrnam($group) → hash` | `getgrgid($gid) → hash` | `getlogin() → str`

### Binary/Encoding

`pack($tmpl, @vals) → str` | `unpack($tmpl, $bin) → array` — templates: N(u32be), n(u16be), C(u8), a(str)
`ord_byte($str, $idx) → int` | `get_byte($str, $idx) → int` | `byte_length($str) → int`
`base64_encode($data) → str` | `base64_decode($enc) → str`

### Dynamic Loading (FFI)

`dl_open($lib) → handle` | `dl_sym($handle, $sym) → func` | `dl_close($handle)`
`dl_call_sv($func, [$args]) → scalar` | `dl_call_int(...)` | `dl_call_str(...)` | `dl_call_void(...)`

### Memory (low-level)

`refcount($val) → int` | `weaken($ref)` | `isweak($ref) → 1/0`
`reserve(@a, $n)` — pre-allocate array capacity (no array_capacity/array_shrink)
`hash_default_capacity($n)` | `set_recursion_limit($n)` — default 1000

### Debugging

`stack_trace() → str` | `caller() → hash` — keys: function, file, line
`caller($level) → hash` — 0=immediate, 1=caller's caller, etc.

### Context

`wantarray() → 1/0` | `wantscalar() → 1/0` | `wanthash() → 1/0`

---

## Regex Reference

**Operators:** `=~` match | `!~` negated match | `s/pat/repl/` subst | `s///g` global | `s///e` eval replacement | `tr/from/to/` transliteration (`y///` alias)

**Capture variables:** `$1`-`$9` after match | `captures() → array` ([0]=full) | `named_captures() → hash` for `(?P<name>...)`

**Functions:** `match($str, $pat) → 1/0` | `replace($str, $pat, $repl) → str` (regex) | `replace_all($str, $old, $new) → str` (literal, NOT regex)

**Flags:** `/i` case-insensitive | `/m` multiline | `/s` dotall | `/x` extended | `/g` global

**PCRE2 features** (require libpcre2): `*?` `+?` lazy quantifiers | `\b` word boundary | `(?=...)` `(?!...)` lookahead | `(?<=...)` `(?<!...)` lookbehind | `(?P<name>...)` named captures

**Transliteration flags:** `c` complement | `d` delete unmatched | `s` squeeze duplicates | `r` return copy | Returns count of replacements

**Gotcha:** `replace_all()` is plain string replacement. `replace()` and `s///` use regex.

---

## math:: Namespace

`abs($x)` | `pow($base, $exp)` | `sqrt($x)` | `fmod($x, $y)`
`floor($x)` | `ceil($x)` | `round($x)`
`sin($x)` | `cos($x)` | `tan($x)` | `asin($x)` | `acos($x)` | `atan($x)` | `atan2($y, $x)`
`sinh($x)` | `cosh($x)` | `tanh($x)`
`exp($x)` | `log($x)` (natural) | `log10($x)`
`isnan($x) → 1/0` | `isinf($x) → 1/0/-1`
`rand() → num [0,1)` | `srand($seed)`

---

## Async/Concurrency

**async/await:** `async func name() type { }` | `my $f = name(args)` starts async | `await $f` blocks for result | Exceptions propagate through await

**Futures:** `async::all(\@futures) → @results` | `async::race(\@futures)` | `async::timeout($f, $ms)` | `async::cancel($f)` | `async::is_done($f) → 1/0`

**Channels:** `async::channel($cap)` (0=unbounded) | `async::send($ch, $val)` | `async::recv($ch) → val` | `async::try_send/try_recv` non-blocking | `async::close($ch)`

**Mutexes:** `async::mutex()` | `async::lock($m)` | `async::unlock($m)` | `async::try_lock($m) → 0/1`

**Atomics:** `async::atomic($init)` | `async::atomic_load/store/add/sub/inc/dec` | `async::atomic_cas($a, $exp, $des) → 1/0`

---

## Types and Casting

**Check:** `typeof($val) → str` | `defined($val) → 1/0` | `ref($val) → "ARRAY"|"HASH"|""|classname` | `isa($obj, "Class") → 1/0`

**Cast:** `int($val)` truncates | `num($val)` | `str($val)` | `chr($code) → str` | `ord($str) → int`

**C interop types:** `int8/16/32/64`, `uint8/16/32/64`, `float`/`double`, `size_t`, `byte`, `char`

**Bool:** `my bool $flag = 1` — internally `int`. Truthiness: `0`, `0.0`, `""`, `undef` = false

**Dynamic return:** `func flex() dynamic { if (wantarray()) { ... } return 42; }`

---

## Memory Management

**Tagged integers:** 63-bit ints stored in pointers (low bit set) — zero heap allocation, no refcounting

**Refcounting:** Non-int values auto-managed; freed when refcount hits 0

**Weak refs:** `core::weaken($ref)` breaks cycles | `core::isweak($ref) → 1/0` | Target becomes undef when freed

**OOP destructors:** `func DESTROY(scalar $self) void { ... }` — called at refcount 0

**Pre-allocation:** `my array @a[1000]` | `reserve(@a, $n)` | `core::hash_default_capacity($n)`

---

## REPL

Start: `./strada-interp` | Commands: `.help`, `.vars`, `.funcs`, `.load FILE`, `.clear`, `.debug`, `.quit`

Multi-line auto-detected (unbalanced braces). Functions/vars persist until `.clear`.

REPL uses tree-walking backend; file execution uses bytecode VM (faster).

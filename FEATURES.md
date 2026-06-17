# Strada Language Features

A complete feature matrix for the Strada programming language. 

## Core Language

### Type System
- **Primitives**: `int` (64-bit), `num` (double), `str`, `scalar` (dynamic)
- **Composites**: `array`, `hash`, user-defined `struct`
- **Type functions**: `typeof()`, `defined()`, `ref()`, `int()`, `num()`, `str()`
- **Strong typing** with automatic coercion where sensible

### Variables and Sigils
- `$` for scalars: `my int $count = 0;`
- `@` for arrays: `my array @items = (1, 2, 3);`
- `%` for hashes: `my hash %data = ();`
- **Array with capacity**: `my array @large[1000];` (pre-allocate)
- **Hash with capacity**: `my hash %cache[500];` (pre-allocate)

### Control Flow
- `if/elsif/else`, `unless`
- `while`, `until`
- `for (init; cond; update)` (C-style)
- `for my type $var (@array)` / `foreach my type $var (@array)`
- `last`, `next` with optional labels: `last OUTER;`
- `goto label;` and standalone labels
- `try { } catch ($e) { }` and `throw`

### Functions
- Typed parameters and return: `func add(int $a, int $b) int { }`
- Optional parameters with defaults: `func greet(str $name, str $msg = "Hi") str { }`
- Variadic functions: `func sum(int ...$nums) int { }`
- Spread operator: `my int $total = sum(...@values);`
- Closures: `my scalar $f = func (int $n) { return $n * 2; };`
- Function references: `\&func_name`

### Operators
- **Arithmetic**: `+`, `-`, `*`, `/`, `%`, `**`, `++`, `--` (prefix/postfix)
- **Comparison**: `==`, `!=`, `<`, `>`, `<=`, `>=`
- **String comparison**: `eq`, `ne`, `lt`, `gt`, `le`, `ge`
- **Logical**: `&&`, `||`, `!`, `and`, `or`, `not`
- **Bitwise**: `&`, `|`, `^`, `~`, `<<`, `>>`
- **String**: `.` (concat), `x` (repeat), `=~`, `!~` (regex)
- **Assignment**: `=`, `+=`, `-=`, `.=`
- **Other**: `\` (ref), `->` (deref), `..` (range), `? :` (ternary)

## Built-in Functions

### I/O
`say()`, `print()`, `printf()`, `sprintf()`, `warn()`, `die()`, `exit()`

### Strings (20+)
`length()`, `substr()`, `index()`, `rindex()`, `uc()`, `lc()`, `ucfirst()`, `lcfirst()`, `trim()`, `ltrim()`, `rtrim()`, `chomp()`, `chop()`, `chr()`, `ord()`, `reverse()`, `join()`, `split()`

### Arrays
`push()`, `pop()`, `shift()`, `unshift()`, `size()`, `sort()`, `reverse()`, `splice()`, `sys::array_capacity()`, `sys::array_reserve()`, `sys::array_shrink()`

### Hashes
`keys()`, `values()`, `exists()`, `delete()`, `each()`, `sys::hash_default_capacity()`

### math:: Namespace
`sin`, `cos`, `tan`, `asin`, `acos`, `atan`, `atan2`, `sinh`, `cosh`, `tanh`, `log`, `log10`, `exp`, `pow`, `floor`, `ceil`, `round`, `fabs`, `fmod`, `sqrt`, `abs`, `rand`

### sys:: Namespace

**File I/O**: `open`, `close`, `read`, `readline`, `write`, `seek`, `tell`, `rewind`, `eof`, `flush`, `slurp`, `spew`

**File System**: `unlink`, `rename`, `mkdir`, `rmdir`, `chdir`, `getcwd`, `chmod`, `access`, `stat`, `lstat`, `readdir`, `readdir_full`, `is_dir`, `is_file`, `realpath`, `dirname`, `basename`, `glob`, `fnmatch`

**Process**: `fork`, `exec`, `system`, `exit`, `wait`, `waitpid`, `getpid`, `getppid`, `kill`, `alarm`, `signal`, `sleep`, `usleep`

**Sockets**: `socket_server`, `socket_accept`, `socket_client`, `socket_send`, `socket_recv`, `socket_close`, `socket_select`

**IPC**: `pipe`, `dup2`, `read_fd`, `write_fd`, `close_fd`, `read_all_fd`

**Environment**: `getenv`, `setenv`, `unsetenv`

**Time**: `time`, `localtime`, `gmtime`, `mktime`, `strftime`, `ctime`, `gettimeofday`, `hires_time`

**DNS**: `gethostbyname`, `gethostbyname_all`, `gethostname`, `getaddrinfo`

**FFI**: `dl_open`, `dl_sym`, `dl_close`, `dl_error`, `dl_call_int`, `dl_call_num`, `dl_call_str`, `dl_call_void`, `dl_call_int_sv`, `dl_call_str_sv`, `dl_call_void_sv`, `dl_call_sv`, `dl_call_version`

## Regular Expressions

- Inline syntax: `/pattern/flags`
- Match: `$str =~ /pattern/` or `$str =~ /pattern/i`
- Substitution: `$str =~ s/old/new/` or `$str =~ s/old/new/g`
- Negated match: `$str !~ /pattern/`
- `split()` with regex patterns

## References and Data Structures

- Create references: `\$scalar`, `\@array`, `\%hash`, `\&func`
- Anonymous constructors: `[1, 2, 3]` (array ref), `{key => "val"}` (hash ref)
- Dereference: `$$ref`, `$ref->[0]`, `$ref->{"key"}`
- Functions: `clone()`, `deref()`, `reftype()`

## Structs

```strada
struct Person {
    str name;
    int age;
    func(int, int) int operation;  # Function pointer field
}

my Person $p;
$p->name = "Alice";
$p->operation = &add;
my int $result = $p->operation(10, 5);
```

## Object-Oriented Programming

- Perl-style blessing: `bless(\%self, "ClassName")`
- Auto method registration for `ClassName_method($self, ...)` pattern
- Auto function prefixing inside `package` blocks
- Method calls: `$obj->method(args)`
- UNIVERSAL methods: `$obj->isa("Class")`, `$obj->can("method")`
- Inheritance: `inherit "ParentClass"`

## Modules and Packages

- `package Name;` - Declare package
- `version "1.0.0";` - Module versioning
- `use Module;` - Compile-time inclusion
- `use lib "path";` - Add to library search path
- `import_lib "LibName";` - Runtime dynamic loading
- Namespace syntax: `Package::function()` resolves to `Package_function()`

## Shared Libraries

- Compile: `./strada --shared mylib.strada` creates `mylib.so`
- Load and call:
  ```strada
  my int $lib = sys::dl_open("./mylib.so");
  my int $fn = sys::dl_sym($lib, "my_function");
  my scalar $result = sys::dl_call_sv($fn, [arg1, arg2]);
  ```

## SSL/TLS Support

- Build: `cd lib/ssl && make`
- Connect: `my int $conn = ssl::connect("example.com", 443);`
- Read/write: `ssl::write($conn, $data);`, `my str $resp = ssl::read($conn, 4096);`

## Exception Handling

```strada
try {
    risky_operation();
} catch ($e) {
    say("Error: " . $e);
}
throw "Custom error message";
```

## Debugging

- `dumper($val)` - Inspect data structures
- `stacktrace()` - Show call stack
- `caller()` - Get caller info
- `./strada -g file.strada` - Debug symbols
- `./strada -w file.strada` - Enable warnings (unused variables)

## Cannoli Web Framework

A preforking web server and framework:

```bash
cd cannoli && make
./cannoli --dev              # Development mode
./cannoli -p 3000 -w 10      # Production: port 3000, 10 workers
./cannoli --static /path     # Static file server
```

Features: routing, JSON/HTML responses, request/response headers, cookies, file uploads, chunked responses, HTTPS support.

## Test Suite

```bash
make test-suite              # Run all 93 tests
./t/run_tests.sh -v          # Verbose output
./t/run_tests.sh -t          # TAP format for CI/CD
```

---

*Last updated: 2026-01-10*

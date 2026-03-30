# Strada Compiler Architecture

## Overview

Strada uses a two-stage compilation process:

```
Strada Source (.strada)
        │
        ▼
┌───────────────────┐
│  Strada Compiler  │  ← Self-hosting (written in Strada)
│    (./stradac)    │
└───────────────────┘
        │
        ▼
   C Source (.c)
        │
        ▼
┌───────────────────┐
│   C Compiler      │
│   (gcc/clang)     │
└───────────────────┘
        │
        ▼
 Native Executable
```

## Bootstrapping Process

The Strada compiler is **self-hosting**, meaning it's written in Strada itself. This creates a bootstrapping challenge: how do you compile a compiler written in a language that doesn't have a compiler yet?

```
┌─────────────────────┐
│  Bootstrap Compiler │  Written in C (~4,400 lines)
│  (bootstrap/stradac)│  FROZEN - no longer modified
└──────────┬──────────┘
           │ compiles
           ▼
┌─────────────────────┐
│ Self-Hosting Compiler│  Written in Strada (~2,300 lines)
│    (./stradac)      │  PRIMARY - all development here
└──────────┬──────────┘
           │ compiles
           ▼
┌─────────────────────┐
│  Your Strada Code   │
└─────────────────────┘
```

**The bootstrap compiler is frozen.** All new features and bug fixes should be made to the self-hosting compiler (`compiler/*.strada`), not the bootstrap compiler.

## Compiler Components

### 1. Bootstrap Compiler (`bootstrap/`) - FROZEN

The bootstrap compiler is written in C. It exists only to compile the self-hosting compiler and should not be modified for new features.

| File | Lines | Description |
|------|-------|-------------|
| `lexer.c/h` | ~400 | Tokenizer |
| `parser.c/h` | ~1,200 | Recursive descent parser |
| `ast.c/h` | ~600 | AST node definitions |
| `codegen.c/h` | ~2,150 | C code generator |
| `main.c` | ~100 | Entry point |

**Total: ~4,450 lines of C**

### 2. Self-Hosting Compiler (`compiler/`) - PRIMARY

The self-hosting compiler is written in Strada. This is where all development happens.

| File | Lines | Description |
|------|-------|-------------|
| `AST.strada` | ~385 | AST node types and constructors |
| `Lexer.strada` | ~410 | Tokenizer |
| `Parser.strada` | ~800 | Recursive descent parser |
| `CodeGen.strada` | ~690 | C code generator |
| `Main.strada` | ~30 | Entry point |

**Total: ~2,315 lines of Strada**

The self-hosting compiler is combined into a single `Combined.strada` file during build, then compiled to `Combined.c` by the bootstrap compiler, and finally linked into `./stradac`.

### 3. Runtime Library (`runtime/`)

The runtime provides memory management, data structures, and built-in functions.

| File | Lines | Description |
|------|-------|-------------|
| `strada_runtime.c` | ~2,150 | Runtime implementation |
| `strada_runtime.h` | ~150 | Runtime header |

## Interpreter (Alternative Execution Path)

In addition to the compile-to-C pipeline, Strada includes a tree-walking interpreter that executes programs directly from the AST:

```
Strada Source (.strada)
        │
        ▼
┌───────────────────┐
│  Lexer + Parser   │  ← Shared with compiler
└───────────────────┘
        │
        ▼
   AST (in memory)
        │
   ┌────┴────┐
   │         │
   ▼         ▼
CodeGen   Interpreter
 (C out)   (tree-walk)
```

The interpreter (`lib/Strada/Interpreter.strada`) reuses the compiler's Lexer and Parser, then walks AST nodes to evaluate the program. This powers:

- **`strada-interp`** - Standalone interpreter with REPL (`interpreter/Main.strada`)
- **`Strada::Interpreter::eval_string()`** - Embedded eval from compiled programs
- **`Strada::JIT::eval()`** - JIT eval (compiles to .so at runtime; uses the compile path, not the interpreter)

The interpreter supports the full language except `__C__` blocks and async/await. See `docs/INTERPRETER.md` for details.

## Compilation Pipeline

### Stage 1: Lexical Analysis (Lexer)

Converts source code into tokens:

```strada
my int $x = 42;
```

Becomes:

```
[MY] [TYPE_INT] [DOLLAR] [IDENT:"x"] [ASSIGN] [INT_LITERAL:42] [SEMI]
```

**Token Types:**
- Keywords: `func` (or `fn`), `my`, `if`, `while`, `for`, `return`, `local`, etc.
- Types: `int`, `num`, `str`, `array`, `hash`, `void`, etc.
- Operators: `+`, `-`, `*`, `/`, `x`, `==`, `!=`, `&&`, `||`, etc.
- Delimiters: `(`, `)`, `{`, `}`, `[`, `]`, `;`, `,`
- Literals: integers, floats, strings, `TR_LITERAL` (transliteration `tr///` / `y///`)
- Identifiers: variable and function names
- Special: `LOCAL` token for `local()` declarations

### Stage 2: Parsing (Parser)

Converts tokens into an Abstract Syntax Tree (AST):

```
Program
└── Function "main"
    ├── ReturnType: int
    ├── Params: []
    └── Body: Block
        └── VarDecl
            ├── Name: "x"
            ├── Type: int
            └── Init: IntLiteral(42)
```

**Parser Design:**
- Recursive descent parser
- Operator precedence climbing for expressions
- Three-token lookahead for complex constructs

**Precedence (lowest to highest):**
1. Assignment: `=`, `+=`, `-=`, `.=`
2. Logical OR: `||`
3. Logical AND: `&&`
4. Equality: `==`, `!=`, `eq`, `ne`
5. Relational: `<`, `>`, `<=`, `>=`, `lt`, `gt`, `le`, `ge`
6. Additive: `+`, `-`, `.`
7. Multiplicative: `*`, `/`, `%`
8. Repetition: `x` (string repeat)
9. Unary: `-`, `!`, `\`
10. Postfix: `->`, `[]`, `{}`

**Regex `/e` Flag:**

The `/e` modifier on substitutions (`s/pattern/expr/e`) causes the replacement to be evaluated as an expression rather than a literal string. The parser detects the `e` flag during regex parsing and marks the substitution node accordingly. CodeGen emits code that evaluates the replacement expression and converts the result to a string for substitution.

**Transliteration Parsing:**

The `tr///` and `y///` operators are handled by the lexer as `TR_LITERAL` tokens. The lexer reads the search list and replacement list directly (they are character lists, not regex patterns). The parser constructs a `NODE_TR` AST node containing the search characters, replacement characters, and modifier flags (`d`, `s`, `c`).

### Stage 3: Code Generation (CodeGen)

Transforms AST into C code:

**Strada:**
```strada
my int $x = 42;
say($x);
```

**Generated C:**
```c
StradaValue *x = strada_new_int(42);
strada_say(x);
```

**Key Transformations:**

| Strada | C |
|--------|---|
| `my int $x` | `StradaValue *x` |
| `42` | `strada_new_int(42)` |
| `"hello"` | `strada_new_str("hello")` |
| `$a + $b` (int) | `STRADA_MAKE_TAGGED_INT(STRADA_TAGGED_INT_VAL(a) + STRADA_TAGGED_INT_VAL(b))` |
| `$a + $b` (other) | `strada_new_num(strada_to_num(a) + strada_to_num(b))` |
| `$arr[$i]` | `strada_array_get(arr->value.av, strada_to_int(i))` |
| `$hash{"key"}` | `strada_hash_get(hash->value.hv, "key")` |
| `say($x)` | `strada_say(x)` |

## AST Node Types

### Declarations
- `NODE_PROGRAM` - Root node
- `NODE_FUNCTION` - Function definition
- `NODE_PARAM` - Function parameter
- `NODE_VAR_DECL` - Variable declaration
- `NODE_STRUCT_DEF` - Struct definition

### Statements
- `NODE_BLOCK` - Code block
- `NODE_IF_STMT` - If/elsif(else if)/else
- `NODE_WHILE_STMT` - While loop
- `NODE_FOR_STMT` - For loop
- `NODE_RETURN_STMT` - Return statement
- `NODE_EXPR_STMT` - Expression statement
- `NODE_LAST_STMT` - Break (last)
- `NODE_NEXT_STMT` - Continue (next)
- `NODE_REDO` - Redo (restart iteration)
- `NODE_FOREACH_STMT` - Foreach loop
- `NODE_DO_WHILE_STMT` - Do-while loop

### Expressions
- `NODE_BINARY_OP` - Binary operation
- `NODE_UNARY_OP` - Unary operation
- `NODE_CALL` - Function call
- `NODE_VARIABLE` - Variable reference
- `NODE_INT_LITERAL` - Integer literal
- `NODE_NUM_LITERAL` - Number literal
- `NODE_STR_LITERAL` - String literal
- `NODE_ASSIGN` - Assignment
- `NODE_SUBSCRIPT` - Array subscript
- `NODE_HASH_ACCESS` - Hash access

### References
- `NODE_REF` - Create reference
- `NODE_DEREF_HASH` - Hash dereference
- `NODE_DEREF_ARRAY` - Array dereference
- `NODE_DEREF_SCALAR` - Scalar dereference
- `NODE_ANON_HASH` - Anonymous hash
- `NODE_ANON_ARRAY` - Anonymous array

### Closures
- `NODE_ANON_FUNC` - Anonymous function definition
- `NODE_CLOSURE_CALL` - Closure invocation (`$f->()`)

### Transliteration and Scoping
- `NODE_TR` - Transliteration operator (`tr///` / `y///`)
- `NODE_LOCAL_DECL` - Dynamic scoping declaration (`local()`)

## Closure Implementation

### Anonymous Functions (Closures)

Anonymous functions are first-class values that can capture variables from their enclosing scope.

**Strada:**
```strada
my int $x = 10;
my scalar $f = func (int $n) { return $n * $x; };
say($f->(5));  # 50
```

**Generated C:**
```c
// Forward declaration with triple pointer for capture-by-reference
StradaValue* __anon_func_0(StradaValue ***__captures, StradaValue *n);

// In the enclosing function:
StradaValue *x = strada_new_int(10);
StradaValue *f = strada_closure_new((void*)&__anon_func_0, 1, 1, (StradaValue**[]){&x});
strada_say(strada_closure_call(f, 1, strada_new_int(5)));

// Anonymous function definition:
StradaValue* __anon_func_0(StradaValue ***__captures, StradaValue *n) {
    return strada_new_num(strada_to_num(n) * strada_to_num((*__captures[0])));
}
```

### Capture Mechanism

Closures use **capture-by-reference** with triple pointers:

1. **At closure creation**: Addresses of captured variables are stored in an array
   - `(StradaValue**[]){&var1, &var2}` - array of pointers to pointers

2. **Function signature**: Anonymous functions receive captures as first parameter
   - `StradaValue ***__captures` - pointer to array of double pointers

3. **Capture access**: Variables are accessed through double dereference
   - `(*__captures[idx])` - dereference to get the actual StradaValue*

4. **Mutation**: Assignments to captured variables update the original
   - `(*__captures[idx]) = new_value` - modifies outer variable

### Capture Detection

During code generation, the compiler tracks:
- `in_anon_func` - Whether currently inside an anonymous function
- `anon_param_str` - Comma-separated list of parameter names
- `anon_local_str` - Comma-separated list of local variable names
- `anon_capture_str` - Comma-separated list of captured variable names
- `anon_capture_count` - Number of captured variables

When generating a variable reference inside an anonymous function:
1. Check if it's a parameter → use directly
2. Check if it's a local variable → use directly
3. Otherwise → it's a capture → emit `(*__captures[idx])`

### Closure Call Syntax

The `->()` syntax invokes a closure:

**Strada:**
```strada
my int $result = $closure->(arg1, arg2);
```

**Generated C:**
```c
StradaValue *result = strada_closure_call(closure, 2, arg1, arg2);
```

The runtime's `strada_closure_call` handles dispatching to the actual function with the correct number of arguments.

## Runtime System

### StradaValue Structure

All Strada values are represented as `StradaValue`:

```c
typedef enum {
    STRADA_UNDEF,
    STRADA_INT,
    STRADA_NUM,
    STRADA_STR,
    STRADA_ARRAY,
    STRADA_HASH,
    STRADA_REF,
    STRADA_CPOINTER,
    STRADA_CLOSURE     // Anonymous function with captured environment
} StradaType;

typedef struct StradaValue {
    StradaType type;
    int refcount;
    union {
        int64_t iv;      // Integer
        double nv;       // Number
        char *sv;        // String
        StradaArray *av; // Array
        StradaHash *hv;  // Hash
        struct {         // Reference
            struct StradaValue *target;
            char ref_type;
        } rv;
    } value;
} StradaValue;
```

### Tagged Integer Optimization

Integers are encoded directly in pointers using tagged pointer representation: `(value << 1) | 1`. When the low bit of a `StradaValue*` is set, the remaining 63 bits hold the integer value (range: -(2^62) to (2^62-1)).

```c
#define STRADA_IS_TAGGED_INT(sv)    ((uintptr_t)(sv) & 1)
#define STRADA_TAGGED_INT_VAL(sv)   ((int64_t)((intptr_t)(sv) >> 1))
#define STRADA_MAKE_TAGGED_INT(val) ((StradaValue*)((((intptr_t)(val)) << 1) | 1))
```

Key properties:
- **Zero heap allocation** for integer values
- **Immortal** -- `strada_incref()`/`strada_decref()` are no-ops for tagged ints
- `strada_new_int()` returns a tagged pointer, not a heap-allocated struct
- `strada_to_int()` extracts the value from either tagged or heap-allocated form
- All runtime functions that access `sv->type` must guard with `STRADA_IS_TAGGED_INT(sv)` first
- References to tagged ints are **unboxed** to heap values in `strada_ref_create()` (references need stable pointer targets)
- Supersedes the old small int pool (-1..255)

### Concat Key Skip-Intern Optimization

Hash key storage uses `strdup()` instead of `strada_intern_str()` for keys set via `strada_hash_set_with_hash()`. Concatenated keys (e.g., `"item_" . $i`) are typically unique, so interning wastes time. `strada_intern_release()` handles both interned and non-interned keys transparently.

Combined with tagged integers, Strada achieves 3.4-3.5x faster performance than Python/Perl on hash-heavy integer workloads.

### Memory Management

Strada uses reference counting for heap-allocated values (strings, arrays, hashes, refs, nums). Tagged integers bypass reference counting entirely.

```c
// Increment reference count (no-op for tagged integers)
void strada_refcnt_inc(StradaValue *sv);

// Decrement reference count, frees if zero (no-op for tagged integers)
void strada_refcnt_dec(StradaValue *sv);
```

### Type Conversions

```c
int64_t strada_to_int(StradaValue *sv);
double strada_to_num(StradaValue *sv);
char* strada_to_str(StradaValue *sv);
int strada_to_bool(StradaValue *sv);
```

## Package System

### Module Resolution

When you write:
```strada
use Math::Utils;
```

The compiler searches:
1. `lib/Math/Utils.sm`
2. `./Math/Utils.sm`
3. Custom paths from `use lib`

### Namespace Prefixing

Functions in packages get prefixed:

```strada
# In Math::Utils
func add(int $a, int $b) int { ... }
```

Generates:
```c
StradaValue* Math_Utils_add(StradaValue* a, StradaValue* b) { ... }
```

### Selective Imports

```strada
use Math::Utils qw(add multiply);
```

Generates:
```c
extern StradaValue* Math_Utils_add();
#define add Math_Utils_add
extern StradaValue* Math_Utils_multiply();
#define multiply Math_Utils_multiply
```

## `__C__` Blocks

C code can be embedded directly using `__C__` blocks:

**Top-level block (includes, globals):**
```strada
__C__ {
    #include <math.h>
    static int helper(int a, int b) { return a + b; }
}
```

**Statement-level block (inline C):**
```strada
func c_add(int $a, int $b) int {
    __C__ {
        int64_t va = strada_to_int(a);
        int64_t vb = strada_to_int(b);
        return strada_new_int(va + vb);
    }
}
```

**Generated C:**
```c
StradaValue* c_add(StradaValue* a, StradaValue* b) {
    /* Begin __C__ block */
    int64_t va = strada_to_int(a);
    int64_t vb = strada_to_int(b);
    return strada_new_int(va + vb);
    /* End __C__ block */
}
```

Key points:
- Top-level blocks appear at file scope in generated C
- Statement-level blocks have access to Strada variables (as `StradaValue*`)
- Use `strada_to_int()`, `strada_to_str()` etc. to extract values
- Return `StradaValue*` using `strada_new_int()`, `strada_new_str()` etc.

## Build System

### Makefile Targets

| Target | Description |
|--------|-------------|
| `make` | Build self-hosting compiler (default) |
| `make run PROG=x` | Compile and run example with self-hosting compiler |
| `make run-bootstrap PROG=x` | Compile example with bootstrap compiler |
| `make examples` | Build all example programs |
| `make test` | Run runtime tests |
| `make test-selfhost` | Test self-compilation (stage 2) |
| `make clean` | Remove build artifacts |
| `make help` | Show all targets |
| `make info` | Show project info |

### Build Flow

```bash
# 1. Build everything (default)
make

# This does:
#   a. Build bootstrap compiler (C) if needed
#   b. Combine compiler/*.strada into Combined.strada
#   c. Compile Combined.strada to Combined.c using bootstrap
#   d. Link Combined.c + runtime into ./stradac

# 2. Compile a program with self-hosting compiler
./stradac program.strada program.c
gcc -o program program.c runtime/strada_runtime.c -Iruntime -ldl
./program

# Or use the shortcut:
make run PROG=program_name
```

### Adding New Features

To add new language features:

1. **Modify the self-hosting compiler** (`compiler/*.strada`)
2. Rebuild: `make`
3. Test: `make run PROG=your_test`

Do NOT modify the bootstrap compiler unless absolutely necessary for bootstrapping.

## Profiling Instrumentation

### Function-Level Profiling (`-p`)

When the `-p` flag is set, CodeGen wraps each function body with `strada_profile_enter()`/`strada_profile_exit()` calls that track call counts and cumulative wall-clock time. At program exit, a summary is printed to stderr.

### Line-Level Profiling (`--full-profile`)

The `--full-profile` flag enables comprehensive line-level instrumentation, similar to Perl's Devel::NYTProf. It implies `-g` (debug/line info).

**CodeGen behavior**: When full profiling is enabled, the code generator inserts a `strada_full_profile_tick(file, line)` call before each statement. This records the source file, line number, and high-resolution timestamp.

**Runtime behavior**: The profiled program writes a binary `strada-prof.out` file on exit containing per-line execution counts and timing data.

**Report tools** (built by `make`, installed by `make install`):
- `strada-proftext` -- generates text reports (function summary + per-file line breakdown)
- `strada-profhtml` -- generates HTML reports with sortable tables and heat-colored source views

**Programmatic API**: `core::full_profile_start(file)` and `core::full_profile_stop()` allow enabling/disabling profiling at runtime for targeted analysis of specific code sections.

## Error Handling

### Lexer Errors
- Unexpected character
- Unterminated string
- Invalid number format

### Parser Errors
- Unexpected token
- Missing semicolon
- Mismatched braces

### Runtime Errors
- Division by zero
- Array index out of bounds
- Undefined value access

## Compiler Optimizations

The self-hosting compiler applies several automatic optimizations during code generation:

- **Int arithmetic inlining**: When both operands of `+`, `-`, or `*` are `int`-typed expressions, the compiler emits direct tagged integer arithmetic (`STRADA_MAKE_TAGGED_INT(... + ...)`) instead of going through `strada_to_num()`/`strada_new_num()`. This avoids heap allocation entirely for integer math.
- **Inline accessor calls**: Zero-argument `has`-generated attribute accessors bypass method dispatch and inline a direct hash lookup.
- **Inline constructor for classes with `extends`**: The auto-generated `new()` constructor optimization (which inlines attribute setup instead of parsing variadic args at runtime) now works for classes that use `extends` to inherit from parent classes, not just base classes without parents.
- **Concat key optimization**: Hash keys built with `"prefix" . $i` use stack buffers and skip string interning.
- **Tagged integer constants**: Integer literals compile to `STRADA_MAKE_TAGGED_INT(N)` with zero heap allocation.
- **Int parameter skip**: Parameters declared as `int` skip `strada_incref`/`strada_decref`/`strada_cleanup_push`/`strada_cleanup_pop` since tagged integers are immortal (~8 fewer overhead calls per int-param function invocation).
- **Function return type tracking**: `expr_is_int_typed()` consults a `func_ret_int` lookup table to recognize `NODE_CALL` expressions that return `int`, enabling tagged int arithmetic for call results (e.g., `$sum += add3($a, $b, $c)`).
- **Int parameter type tracking**: Parameters declared as `int` are added to `int_vars`, so int arithmetic within the function body uses the direct tagged int path.
- **LTO at -O2+**: The `strada` driver adds `-flto` at `-O2` and above, and `-march=native` at `-O3`/`-Ofast`. LTO enables GCC to inline runtime functions (e.g., `strada_incref`, `strada_decref`, `strada_to_int`) across translation units, dramatically improving function call performance.

## Future Improvements

1. **Optimization passes** - Dead code elimination, constant folding
2. **Better error messages** - Line numbers, source context
3. **Type checking** - Enforce type annotations at compile time
4. **Incremental compilation** - Only recompile changed modules
5. **Debug information** - Generate DWARF debug info
6. **LLVM backend** - Alternative to C generation

---

## File Map

```
strada/
├── stradac              # Self-hosting compiler executable (built)
├── Makefile             # Build system
├── README.md            # Project overview
│
├── bootstrap/           # C bootstrap compiler (FROZEN)
│   ├── lexer.c/h       # Tokenizer
│   ├── parser.c/h      # Parser
│   ├── ast.c/h         # AST definitions
│   ├── codegen.c/h     # Code generator
│   └── main.c          # Entry point
│
├── compiler/           # Self-hosting compiler (PRIMARY)
│   ├── AST.strada      # AST node types
│   ├── Lexer.strada    # Tokenizer
│   ├── Parser.strada   # Parser
│   ├── CodeGen.strada  # Code generator
│   ├── Main.strada     # Entry point
│   ├── Combined.strada # Combined source (generated)
│   └── Combined.c      # Compiled to C (generated)
│
├── interpreter/        # Tree-walking interpreter
│   ├── Main.strada    # Driver (REPL + file execution)
│   └── Combined.strada # Combined source (generated)
│
├── runtime/            # Runtime library
│   ├── strada_runtime.c
│   └── strada_runtime.h
│
├── examples/           # Example programs
│   ├── test_simple.strada
│   └── ...
│
├── lib/               # Standard library modules
│   └── Strada/
│       ├── Interpreter.strada  # Interpreter library
│       └── JIT.strada          # JIT eval library
│
└── docs/              # Documentation
    ├── LANGUAGE_GUIDE.md
    ├── QUICK_REFERENCE.md
    ├── COMPILER_ARCHITECTURE.md
    └── RUNTIME_API.md
```

## Development Workflow

### Making Changes to the Compiler

1. Edit files in `compiler/*.strada`
2. Run `make` to rebuild `./stradac`
3. Test with `make run PROG=test_program`

### Testing Self-Compilation

The self-hosting compiler should be able to compile itself:

```bash
# Stage 1: Bootstrap compiles self-hosting compiler
make

# Stage 2: Self-hosting compiler compiles its own modules
make test-selfhost
```

### Debugging

If the self-hosting compiler has issues:

1. Compare output with bootstrap: `make run-bootstrap PROG=x`
2. Check generated C code in `compiler/Combined.c`
3. Add debug output with `say()` in compiler source

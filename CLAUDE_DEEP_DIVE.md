# Strada Compiler Deep Dive

This document provides exhaustive technical details about the Strada language implementation. Use this as a reference when making changes to the compiler.

> **IMPORTANT:** When making changes to Strada, **always update these documentation files:**
> - `CLAUDE.md` - Add to "Recent Features" section
> - `CLAUDE_REFERENCE.md` - Update relevant sections
> - `CLAUDE_DEEP_DIVE.md` (this file) - Update technical details (AST, code gen)

---

## Table of Contents

1. [Complete AST Node Reference](#complete-ast-node-reference)
2. [Complete Type System](#complete-type-system)
3. [Complete Token Reference](#complete-token-reference)
4. [Complete Built-in Functions](#complete-built-in-functions)
5. [Runtime Value System](#runtime-value-system)
6. [Code Generation Patterns](#code-generation-patterns)
7. [Adding New Features Checklist](#adding-new-features-checklist)
8. [Compiler Data Flow](#compiler-data-flow)
9. [Memory Management](#memory-management)
10. [Debugging Techniques](#debugging-techniques)
11. [Test Suite](#test-suite)
12. [Performance Optimizations](#performance-optimizations-2026-01-10)
13. [External Libraries](#external-libraries)
14. [Key Lessons Learned](#key-lessons-learned)

---

## Complete AST Node Reference

Every AST node in Strada. The number is the return value of the `NODE_*()` function.

### Node Type Constants

| ID | Constant | Description | Fields |
|----|----------|-------------|--------|
| 1 | `NODE_PROGRAM` | Root program node | `functions`, `structs`, `uses`, `imports`, `inherits`, `lib_paths`, `package` |
| 2 | `NODE_FUNC` | Function definition | `name`, `return_type`, `params`, `param_count`, `body`, `is_extern`, `is_variadic` |
| 3 | `NODE_PARAM` | Function parameter | `name`, `param_type`, `sigil`, `is_optional`, `default_value`, `is_variadic` |
| 4 | `NODE_BLOCK` | Block of statements | `statements`, `statement_count` |
| 5 | `NODE_VAR_DECL` | Variable declaration | `name`, `var_type`, `sigil`, `init`, `struct_type` |
| 6 | `NODE_IF_STMT` | If statement (elsif and else if both supported) | `condition`, `then_block`, `elsif_conditions`, `elsif_blocks`, `elsif_count`, `else_block` |
| 7 | `NODE_WHILE_STMT` | While loop | `condition`, `body`, `label` |
| 8 | `NODE_FOR_STMT` | C-style for loop | `init`, `condition`, `update`, `body`, `label` |
| 9 | `NODE_RETURN_STMT` | Return statement | `value` |
| 10 | `NODE_EXPR_STMT` | Expression statement | `expr` |
| 11 | `NODE_BINARY_OP` | Binary operation | `op`, `left`, `right` |
| 12 | `NODE_UNARY_OP` | Unary operation | `op`, `operand` |
| 13 | `NODE_CALL` | Function call | `name`, `args`, `arg_count` |
| 14 | `NODE_VARIABLE` | Variable reference | `name`, `sigil` |
| 15 | `NODE_INT_LITERAL` | Integer literal | `value` |
| 16 | `NODE_NUM_LITERAL` | Float literal | `value` |
| 17 | `NODE_STR_LITERAL` | String literal | `value` |
| 18 | `NODE_ASSIGN` | Assignment | `op`, `target`, `value` |
| 19 | `NODE_SUBSCRIPT` | Array subscript | `array`, `index` |
| 20 | `NODE_HASH_ACCESS` | Hash access `$h{k}` | `hash`, `key` |
| 21 | `NODE_REF` | Reference `\$x` | `target`, `ref_type` |
| 22 | `NODE_DEREF_HASH` | Hash deref `$r->{k}` | `ref`, `key` |
| 23 | `NODE_DEREF_ARRAY` | Array deref `$r->[i]` | `ref`, `index` |
| 24 | `NODE_DEREF_SCALAR` | Scalar deref `$$r` | `ref`, `sigil` |
| 25 | `NODE_ANON_HASH` | Anonymous hash `{}` | `keys`, `values`, `pair_count` |
| 26 | `NODE_ANON_ARRAY` | Anonymous array `[]` | `elements`, `element_count` |
| 27 | `NODE_EXTERN_FUNC` | Extern function | `name`, `return_type`, `params`, `param_count`, `has_body`, `is_c_extern`, `is_strada_extern_decl` |
| 28 | `NODE_STRUCT` | Struct definition | `name`, `fields`, `field_count` |
| 29 | `NODE_USE` | Use statement | (module name stored in program) |
| 30 | `NODE_FIELD_ACCESS` | Field access `$s->f` | `object`, `field` |
| 31 | `NODE_FUNC_REF` | Function reference `&f` | `name` |
| 32 | `NODE_METHOD_CALL` | Method call `$o->m()` | `object`, `method`, `args`, `arg_count` |
| 33 | `NODE_DUNDER_PACKAGE` | `__PACKAGE__` | (none) |
| 34 | `NODE_REGEX_MATCH` | Regex match `=~` | `op`, `target`, `pattern`, `flags` |
| 35 | `NODE_REGEX_SUBST` | Regex subst `s///` | `target`, `pattern`, `replacement`, `flags` |
| 36 | `NODE_DUNDER_FILE` | `__FILE__` | (none) |
| 37 | `NODE_DUNDER_LINE` | `__LINE__` | `line_value` |
| 100 | `NODE_LAST` | Last (break) | `label` |
| 101 | `NODE_NEXT` | Next (continue) | `label` |
| 102 | `NODE_UNDEF` | Undef literal | (none) |
| 103 | `NODE_MAP` | Map expression | `block`, `array` |
| 104 | `NODE_SORT` | Sort expression | `block`, `array` |
| 105 | `NODE_GREP` | Grep expression | `block`, `array` |
| 106 | `NODE_TRY_CATCH` | Try/catch block | `try_block`, `catch_var`, `catch_block` |
| 107 | `NODE_THROW` | Throw statement | `expr` |
| 108 | `NODE_LABEL` | Standalone label | `name` |
| 109 | `NODE_GOTO` | Goto statement | `target` |
| 110 | `NODE_FOREACH_STMT` | Foreach loop | `var_decl`, `var_name`, `array`, `body`, `label` |
| 111 | `NODE_ANON_FUNC` | Anonymous function | `params`, `param_count`, `body`, `captures` |
| 112 | `NODE_CLOSURE_CALL` | Closure call `$f->()` | `closure`, `args`, `arg_count` |
| 113 | `NODE_TERNARY` | Ternary `?:` | `condition`, `then_expr`, `else_expr` |
| 114 | `NODE_SWITCH` | Switch statement | `expr`, `cases`, `blocks`, `case_count`, `default_block`, `has_default` |
| 115 | `NODE_RANGE` | Range `..` | `start`, `end` |
| 116 | `NODE_SUPER_CALL` | Super call | `method`, `args`, `arg_count` |
| 117 | `NODE_INCREMENT` | Inc/dec `++`/`--` | `operand`, `op`, `is_prefix` |
| 118 | `NODE_ENUM` | Enum definition | `name`, `members`, `member_count` |
| 119 | `NODE_C_BLOCK` | Raw C code block | `code` |
| 120 | `NODE_SPREAD` | Spread operator `...@arr` | `target` |
| 122 | `NODE_ASYNC_FUNC` | Async function definition | `name`, `return_type`, `params`, `param_count`, `body`, `package`, `is_private` |
| 123 | `NODE_AWAIT` | Await expression | `expr` |
| 124 | `NODE_READLINE` | Diamond operator `<$fh>` | `handle` |
| 125 | `NODE_DESTRUCTURE` | Destructuring assignment | `vars`, `source` |
| 126 | `NODE_DO_WHILE_STMT` | Do-while loop | `condition`, `body` |
| 127 | `NODE_CONST_DECL` | Const declaration | `name`, `var_type`, `sigil`, `init` |
| 128 | `NODE_BEGIN_BLOCK` | BEGIN block | `body` |
| 129 | `NODE_END_BLOCK` | END block | `body` |
| 130 | `NODE_ARRAY_SLICE` | Array slice `@arr[0,2]` | `array`, `indices` |
| 131 | `NODE_HASH_SLICE` | Hash slice `@h{"a","b"}` | `hash`, `keys` |
| 132 | `NODE_OUR_DECL` | Our variable declaration | `name`, `var_type`, `sigil`, `init`, `package` |
| 133 | `NODE_REDO` | Redo statement | `label` |
| 134 | `NODE_TR` | Transliteration `tr///` / `y///` | `target`, `search_list`, `replacement_list`, `flags` |
| 135 | `NODE_LOCAL_DECL` | `local()` declaration | `var_name`, `init` (optional) |
| 136 | `NODE_CAPTURE_VAR` | Capture variable `$1`-`$9` | `number` |
| 137 | `NODE_DYN_METHOD_CALL` | Dynamic method call `$o->$m()` | `base_object`, `method_expr`, `args`, `arg_count` |

### Node Constructor Functions

Each node type has a constructor in `AST.strada`:

```strada
# Generic node creation
func ast_new_node(int $type) scalar { ... }
func ast_set_line(scalar $node, int $line) void { ... }
func ast_get_line(scalar $node) int { ... }

# Specific constructors
func ast_new_program() scalar { ... }
func ast_new_function(str $name, int $return_type) scalar { ... }
func ast_new_param(str $name, int $param_type, str $sigil) scalar { ... }
func ast_new_block() scalar { ... }
func ast_new_var_decl(str $name, int $var_type, str $sigil) scalar { ... }
func ast_new_if_stmt(scalar $condition, scalar $then_block) scalar { ... }
func ast_new_while_stmt(scalar $condition, scalar $body, str $label) scalar { ... }
func ast_new_for_stmt(scalar $init, scalar $cond, scalar $update, scalar $body, str $label) scalar { ... }
func ast_new_foreach_stmt(scalar $var_decl, str $var_name, scalar $array_expr, scalar $body, str $label) scalar { ... }
func ast_new_return_stmt(scalar $value) scalar { ... }
func ast_new_last(str $label) scalar { ... }
func ast_new_next(str $label) scalar { ... }
func ast_new_expr_stmt(scalar $expr) scalar { ... }
func ast_new_binary_op(str $op, scalar $left, scalar $right) scalar { ... }
func ast_new_unary_op(str $op, scalar $operand) scalar { ... }
func ast_new_call(str $name) scalar { ... }
func ast_new_variable(str $name, str $sigil) scalar { ... }
func ast_new_int_literal(int $value) scalar { ... }
func ast_new_num_literal(num $value) scalar { ... }
func ast_new_str_literal(str $value) scalar { ... }
func ast_new_undef() scalar { ... }
func ast_new_map(scalar $block, scalar $array_expr) scalar { ... }
func ast_new_sort(scalar $block, scalar $array_expr) scalar { ... }
func ast_new_grep(scalar $block, scalar $array_expr) scalar { ... }
func ast_new_assign(str $op, scalar $target, scalar $value) scalar { ... }
func ast_new_subscript(scalar $arr, scalar $index) scalar { ... }
func ast_new_hash_access(scalar $hash_var, scalar $key) scalar { ... }
func ast_new_ref(scalar $target, str $ref_type) scalar { ... }
func ast_new_deref_hash(scalar $ref_expr, scalar $key) scalar { ... }
func ast_new_deref_array(scalar $ref_expr, scalar $index) scalar { ... }
func ast_new_deref_scalar(scalar $ref_expr, str $sigil) scalar { ... }
func ast_new_anon_hash() scalar { ... }
func ast_new_anon_array() scalar { ... }
func ast_new_field_access(scalar $obj, str $field) scalar { ... }
func ast_new_func_ref(str $name) scalar { ... }
func ast_new_method_call(scalar $object, str $method) scalar { ... }
func ast_new_dunder_package() scalar { ... }
func ast_new_dunder_file() scalar { ... }
func ast_new_dunder_line(int $line) scalar { ... }
func ast_new_regex_match(str $op, scalar $target, str $pattern, str $flags) scalar { ... }
func ast_new_regex_subst(scalar $target, str $pattern, str $replacement, str $flags) scalar { ... }
func ast_new_try_catch(scalar $try_block, str $catch_var, scalar $catch_block) scalar { ... }
func ast_new_throw(scalar $expr) scalar { ... }
func ast_new_label(str $name) scalar { ... }
func ast_new_goto(str $target) scalar { ... }
func ast_new_struct(str $name) scalar { ... }
func ast_new_extern_func(str $name) scalar { ... }
func ast_new_spread(scalar $target) scalar { ... }  # Spread operator ...@array
func ast_new_tr(scalar $target, str $search_list, str $replacement_list, str $flags) scalar { ... }  # tr/y///
func ast_new_local_decl(str $var_name, scalar $init) scalar { ... }  # local() declaration
func ast_new_capture_var(int $number) scalar { ... }  # $1-$9 capture variables
```

---

## Complete Type System

### Type Constants (AST.strada)

| ID | Constant | Strada Type | C Type |
|----|----------|-------------|--------|
| 1 | `TYPE_INT` | `int` | `StradaValue*` (iv) |
| 2 | `TYPE_NUM` | `num` | `StradaValue*` (nv) |
| 3 | `TYPE_STR` | `str` | `StradaValue*` (pv) |
| 4 | `TYPE_ARRAY` | `array` | `StradaValue*` (av) |
| 5 | `TYPE_HASH` | `hash` | `StradaValue*` (hv) |
| 6 | `TYPE_SCALAR` | `scalar` | `StradaValue*` |
| 7 | `TYPE_VOID` | `void` | `void` |
| 8 | `TYPE_STRUCT` | `StructName` | `StructName*` |
| 9 | `TYPE_FUNCPTR` | `func(...)` | Function pointer |
| 25 | `TYPE_DYNAMIC` | `dynamic` | `StradaValue*` (context-sensitive) |

### Runtime Type Tags (strada_runtime.h)

```c
typedef enum {
    STRADA_UNDEF,      // 0 - Undefined value
    STRADA_INT,        // 1 - Integer (int64_t)
    STRADA_NUM,        // 2 - Float (double)
    STRADA_STR,        // 3 - String (char*)
    STRADA_ARRAY,      // 4 - Array (StradaArray*)
    STRADA_HASH,       // 5 - Hash (StradaHash*)
    STRADA_REF,        // 6 - Reference (StradaValue*)
    STRADA_FILEHANDLE, // 7 - File handle (FILE*)
    STRADA_REGEX,      // 8 - Compiled regex (pcre2_code* with PCRE2, regex_t* with POSIX fallback)
    STRADA_SOCKET,     // 9 - Socket (int fd)
    STRADA_CSTRUCT,    // 10 - C struct wrapper (void*)
    STRADA_CPOINTER    // 11 - Generic C pointer (void*)
} StradaType;
```

---

## Complete Token Reference

All tokens recognized by the lexer:

### Keywords

| Token | Keyword |
|-------|---------|
| `IF` | `if` |
| `ELSIF` | `elsif` (also `else if` — parsed as ELSE + IF) |
| `ELSE` | `else` |
| `UNLESS` | `unless` |
| `WHILE` | `while` |
| `UNTIL` | `until` |
| `FOR` | `for` |
| `FOREACH` | `foreach` |
| `FUNC` | `func` / `fn` |
| `RETURN` | `return` |
| `MY` | `my` |
| `STRUCT` | `struct` |
| `PACKAGE` | `package` |
| `USE` | `use` |
| `VERSION` | `version` |
| `EXTERN` | `extern` |
| `TRY` | `try` |
| `CATCH` | `catch` |
| `THROW` | `throw` |
| `LAST` | `last` |
| `NEXT` | `next` |
| `REDO` | `redo` |
| `GOTO` | `goto` |
| `BLESS` | `bless` |
| `INHERIT` | `inherit` |
| `AND` | `and` |
| `OR` | `or` |
| `NOT` | `not` |
| `EQ_STR` | `eq` |
| `NE_STR` | `ne` |
| `LT_STR` | `lt` |
| `GT_STR` | `gt` |
| `LE_STR` | `le` |
| `GE_STR` | `ge` |
| `HAS` | `has` |
| `EXTENDS` | `extends` |
| `WITH` | `with` |
| `BEFORE` | `before` |
| `AFTER` | `after` |
| `AROUND` | `around` |
| `LOCAL` | `local` |

### Type Keywords

| Token | Type |
|-------|------|
| `INT` | `int` |
| `NUM` | `num` |
| `STR` | `str` |
| `ARRAY` | `array` |
| `HASH` | `hash` |
| `SCALAR` | `scalar` |
| `VOID` | `void` |

### Operators and Punctuation

| Token | Symbol | Description |
|-------|--------|-------------|
| `DOLLAR` | `$` | Scalar sigil |
| `AT` | `@` | Array sigil |
| `PERCENT` | `%` | Hash sigil |
| `AMPERSAND` | `&` | Function ref / Bitwise AND |
| `BACKSLASH` | `\` | Reference operator |
| `LPAREN` | `(` | Left paren |
| `RPAREN` | `)` | Right paren |
| `LBRACE` | `{` | Left brace |
| `RBRACE` | `}` | Right brace |
| `LBRACKET` | `[` | Left bracket |
| `RBRACKET` | `]` | Right bracket |
| `SEMI` | `;` | Semicolon |
| `COMMA` | `,` | Comma |
| `COLON` | `:` | Colon |
| `ARROW` | `->` | Arrow |
| `FAT_ARROW` | `=>` | Fat arrow |
| `DOT` | `.` | Concatenation |
| `DOTDOT` | `..` | Range |
| `SPREAD` | `...` | Spread operator |
| `PLUS` | `+` | Addition |
| `MINUS` | `-` | Subtraction |
| `STAR` | `*` | Multiplication |
| `STARSTAR` | `**` | Exponentiation |
| `SLASH` | `/` | Division |
| `PERCENT_OP` | `%` | Modulo |
| `ASSIGN` | `=` | Assignment |
| `PLUS_ASSIGN` | `+=` | Add-assign |
| `MINUS_ASSIGN` | `-=` | Sub-assign |
| `DOT_ASSIGN` | `.=` | Concat-assign |
| `EQ` | `==` | Numeric equal |
| `NE` | `!=` | Numeric not equal |
| `LT` | `<` | Less than |
| `GT` | `>` | Greater than |
| `LE` | `<=` | Less or equal |
| `GE` | `>=` | Greater or equal |
| `SPACESHIP` | `<=>` | Numeric comparison |
| `CMP` | `cmp` | String comparison |
| `AND_OP` | `&&` | Logical AND |
| `OR_OP` | `\|\|` | Logical OR |
| `NOT_OP` | `!` | Logical NOT |
| `BIT_AND` | `&` | Bitwise AND |
| `BIT_OR` | `\|` | Bitwise OR |
| `BIT_XOR` | `^` | Bitwise XOR |
| `BIT_NOT` | `~` | Bitwise NOT |
| `LSHIFT` | `<<` | Left shift |
| `RSHIFT` | `>>` | Right shift |
| `REGEX_MATCH` | `=~` | Regex match |
| `REGEX_NOT` | `!~` | Regex not match |
| `QUESTION` | `?` | Ternary |
| `X` | `x` | String repeat |

### Literals

| Token | Description |
|-------|-------------|
| `INT_LIT` | Integer literal |
| `NUM_LIT` | Float literal |
| `STRING` | String literal |
| `REGEX` | Regex literal `/pattern/flags` |
| `TR_LITERAL` | Transliteration literal `tr/search/replace/flags` or `y/search/replace/flags` |
| `IDENT` | Identifier |
| `LABEL` | Label (IDENT followed by `:`) |

---

## Complete Built-in Functions

All functions registered in `Semantic.strada` `get_builtins()`:

**Namespaced aliases**: `re::`/`str::`/`sb::` and `core::`-qualified spellings of Strada-specific bare builtins (regex functions, StringBuilder, `hash_new/get/set`, `deref*`/`refto*`/`is_ref`/`refcount`, `dumper*`, `stacktrace*`, `set_package`/`inherit`/`blessed`) are normalized to the canonical bare names at parse time by `ast_normalize_call_name()` (`AST.strada`, called from `ast_new_call()`). Nothing downstream — Semantic, CodeGen, `needs_temp_cleanup` — ever sees the alias spellings; extend that one map to add aliases, never per-name checks in CodeGen. Supporting lexer rule: a word immediately followed by `::` always lexes as `IDENT` (never a keyword), which is what lets the `str` type keyword double as the `str::` namespace.

### I/O Functions
`say`, `print`, `printf`, `sprintf`, `warn`, `die`, `throw`, `readline`

### String Functions
`length`, `substr`, `index`, `rindex`, `uc`, `lc`, `upper`, `lower`, `ucfirst`, `lcfirst`, `trim`, `ltrim`, `rtrim`, `chomp`, `chop`, `chr`, `ord`, `reverse`, `repeat`, `join`, `split`, `tr` (transliteration)

### Binary/Byte Functions (core:: namespace)
`core::ord_byte`, `core::get_byte`, `core::set_byte`, `core::byte_length`, `core::byte_substr`, `core::pack`, `core::unpack`

### Array Functions
`push`, `pop`, `shift`, `unshift`, `size`, `sort`, `nsort`, `reverse`, `splice`, `array_capacity`, `array_reserve`, `array_shrink`

### Hash Functions
`keys`, `values`, `exists`, `delete`, `each`, `hash_new`, `hash_get`, `hash_set`, `hash_default_capacity`

### Type Functions
`defined`, `typeof`, `ref`, `reftype`, `is_ref`, `cast_int`, `cast_num`, `cast_str`, `int`

### Reference Functions
`clone`, `deref`, `deref_array`, `deref_hash`, `deref_set`, `refto`, `is_refto`, `derefto`

### File I/O
`open`, `open_str`, `open_sv`, `str_from_fh`, `close`, `slurp`, `spew`, `fwrite`, `fread`

### Regex
`match`, `replace`, `replace_all`, `capture`, `captures`

### Process Control
`exit`, `sleep`, `usleep`, `fork`, `wait`, `waitpid`, `getpid`, `getppid`, `system`, `exec`, `setprocname`, `getprocname`, `setproctitle`, `getproctitle`, `signal`

### IPC
`pipe`, `dup2`, `close_fd`, `read_fd`, `write_fd`, `read_all_fd`

### POSIX
`getenv`, `setenv`, `unsetenv`, `getcwd`, `chdir`, `mkdir`, `rmdir`, `unlink`, `link`, `symlink`, `readlink`, `rename`, `chmod`, `access`, `umask`, `getuid`, `geteuid`, `getgid`, `getegid`, `kill`, `alarm`, `stat`, `lstat`, `isatty`, `strerror`, `errno`

### Time
`time`, `localtime`, `gmtime`, `mktime`, `strftime`, `ctime`, `gettimeofday`, `hires_time`, `tv_interval`, `nanosleep`, `clock_gettime`, `clock_getres`, `rand`

### Socket
`socket_client`, `socket_server`, `socket_accept`, `socket_recv`, `socket_send`, `socket_close`, `socket_select`, `socket_fd`

### Debug
`dumper`, `dumper_str`, `dump`, `stacktrace`, `caller`

### OOP
`bless`, `blessed`, `set_package`, `inherit`, `isa`, `can`, `UNIVERSAL::isa`, `UNIVERSAL::can`

### FFI (Dynamic Loading)
`dl_open`, `dl_sym`, `dl_close`, `dl_error`, `dl_call_int`, `dl_call_num`, `dl_call_str`, `dl_call_void`, `dl_call_int_sv`, `dl_call_str_sv`, `dl_call_void_sv`, `dl_call_sv`, `dl_call_version`, `dl_call_export_info`

### FFI (Pointer Access)
`int_ptr`, `num_ptr`, `str_ptr`, `ptr_deref_int`, `ptr_deref_num`, `ptr_deref_str`, `ptr_set_int`, `ptr_set_num`

### CStruct
`cstruct_new`, `cstruct_get`, `cstruct_set`, `cstruct_ptr`, `cstruct_get_int`, `cstruct_get_num`, `cstruct_get_str`, `cstruct_get_ptr`, `cstruct_get_double`, `cstruct_get_string`, `cstruct_set_int`, `cstruct_set_num`, `cstruct_set_str`, `cstruct_set_ptr`, `cstruct_set_double`, `cstruct_set_string`

### Memory
`free`, `release`, `malloc`, `refcount`, `weaken`, `isweak`

### Dynamic Scoping
`local` (save/restore variable values dynamically)

### Tie
`tie`, `untie`, `tied`

### Select
`select` (set default output filehandle)

### Terminal
`term_enable_raw`, `term_disable_raw`, `term_rows`, `term_cols`, `read_byte`

### Directory Iteration
`opendir`, `readdir_next`, `closedir`

### Conversion
`atoi`, `atof`

### Misc
`scalar`, `undef`, `strada_new_undef`, `system_argv`, `exit_status`

---

## Runtime Value System

### Tagged Integer Encoding

Integers are encoded directly in the `StradaValue*` pointer using bit tagging, eliminating all heap allocation for integer operations:

```c
// Pointer encoding: (value << 1) | 1
// Bit 0 = 1: tagged integer (not a real pointer)
// Bit 0 = 0: heap-allocated StradaValue*

#define STRADA_IS_TAGGED_INT(sv)    ((uintptr_t)(sv) & 1)
#define STRADA_TAGGED_INT_VAL(sv)   ((int64_t)((intptr_t)(sv) >> 1))
#define STRADA_MAKE_TAGGED_INT(val) ((StradaValue*)((((intptr_t)(val)) << 1) | 1))
```

**Properties:**
- **Range:** -(2^62) to (2^62-1) -- all practical integer values
- **Immortal:** `strada_incref()`/`strada_decref()` check `STRADA_IS_TAGGED_INT` first and return immediately
- **No allocation:** `strada_new_int()` returns `STRADA_MAKE_TAGGED_INT(val)` -- no malloc, no refcount
- **Guard requirement:** Every runtime function that accesses `sv->type` must check `STRADA_IS_TAGGED_INT(sv)` before dereferencing. Failure to do so is a segfault.
- **Reference safety:** `strada_ref_create()` unboxes tagged ints to heap-allocated `StradaValue` before creating a reference, because references need a stable pointer target
- **Supersedes** the old small int pool (-1..255)

**Performance impact:** Eliminates heap allocation for all integer operations. Combined with the concat key skip-intern optimization, achieves 3.4-3.5x faster hash-heavy benchmarks vs Python/Perl.

### StradaValue Structure (Heap-Allocated Values)

Non-integer values (and the rare integer outside tagged range) use heap-allocated structs:

```c
struct StradaValue {
    StradaType type;           // Type tag (enum)
    int refcount;              // Reference count for GC
    union {
        int64_t iv;            // Integer value (STRADA_INT)
        double nv;             // Numeric value (STRADA_NUM)
        char *pv;              // String value (STRADA_STR)
        StradaArray *av;       // Array (STRADA_ARRAY)
        StradaHash *hv;        // Hash (STRADA_HASH)
        StradaValue *rv;       // Reference target (STRADA_REF)
        FILE *fh;              // File handle (STRADA_FILEHANDLE)
        pcre2_code *pcre2_rx;  // Compiled regex with PCRE2 (STRADA_REGEX)
        regex_t *rx;           // Compiled regex with POSIX fallback (STRADA_REGEX)
        int sockfd;            // Socket fd (STRADA_SOCKET)
        void *ptr;             // Generic pointer (STRADA_CPOINTER, STRADA_CSTRUCT)
    } value;
    char *struct_name;         // For CSTRUCT: type name
    size_t struct_size;        // For CSTRUCT: size in bytes
    char *blessed_package;     // For OOP: package name (or NULL)
    uint8_t is_tied;           // For tie: 1 if hash is tied to a class
    StradaValue *tied_obj;     // For tie: the tied object instance
    uint8_t is_weak;           // For weak refs: 1 if this is a weak reference
};
```

### StradaArray Structure

```c
struct StradaArray {
    StradaValue **elements;    // Array of value pointers
    size_t size;               // Current number of elements
    size_t capacity;           // Allocated capacity
    int refcount;              // Reference count
};
```

### StradaHash Structure

```c
struct StradaHash {
    StradaHashEntry **buckets; // Hash buckets
    size_t num_buckets;        // Number of buckets
    size_t num_entries;        // Number of entries
    int refcount;              // Reference count
    size_t iter_bucket;        // each() iterator: current bucket index
    StradaHashEntry *iter_entry; // each() iterator: current entry pointer
};

typedef struct StradaHashEntry {
    char *key;                 // Key string
    StradaValue *value;        // Value
    struct StradaHashEntry *next; // Collision chain
} StradaHashEntry;
```

### Value Creation Functions

```c
StradaValue* strada_new_undef(void);
StradaValue* strada_new_int(int64_t i);
StradaValue* strada_new_num(double n);
StradaValue* strada_new_str(const char *s);
StradaValue* strada_new_array(void);
StradaValue* strada_new_array_with_capacity(size_t capacity);  // Pre-allocate
StradaValue* strada_new_hash(void);
StradaValue* strada_new_hash_with_capacity(size_t capacity);   // Pre-allocate
StradaValue* strada_new_ref(StradaValue *target, char ref_type);
StradaValue* strada_cpointer_new(void *ptr);
StradaValue* strada_anon_hash(int count, ...);
StradaValue* strada_anon_array(int count, ...);
```

### String Repeat Function

```c
StradaValue* strada_string_repeat(StradaValue *sv, int64_t count);  // "abc" x 3 → "abcabcabc"
```

### Array Splice Function

```c
StradaValue* strada_array_splice_sv(StradaValue *arr, int64_t offset, int64_t length, StradaValue *repl);
// splice(@arr, offset, length, @replacement) - remove/insert elements, returns removed elements
```

### Hash Each Iterator

```c
StradaValue* strada_hash_each(StradaHash *hv);
// Returns a 2-element array [key, value] for the next entry, or undef when exhausted.
// Uses iter_bucket/iter_entry fields on StradaHash for stateful iteration.
```

### Select (Default Output Filehandle)

```c
StradaValue* strada_select(StradaValue *fh);      // Set default output filehandle, returns previous
StradaValue* strada_select_get(void);              // Get current default output filehandle
```

### Transliteration

```c
StradaValue* strada_tr(StradaValue *sv, const char *search, const char *replace, const char *flags);
// tr/abc/xyz/ - character-by-character transliteration
// Flags: c (complement), d (delete), s (squeeze), r (return copy)
```

### Local (Dynamic Scoping) Functions

```c
void strada_local_save(const char *name);          // Save current value of global variable
void strada_local_restore(void);                   // Restore most recently saved variable
void strada_local_restore_n(int n);                // Restore N most recently saved variables
int strada_local_depth_get(void);                  // Get current save stack depth
void strada_local_restore_to(int depth);           // Restore all saves down to given depth
```

### Regex /e Support Functions

```c
StradaValue* strada_regex_find_all(const char *str, const char *pattern, const char *flags, int global);
// Find all matches in string, returns array of match info (for /e flag processing)

void strada_set_captures_sv(StradaValue *match);
// Set capture groups from a match info StradaValue (so captures() works inside /e eval)

StradaValue* strada_regex_build_result(const char *src, StradaValue *matches, StradaValue *replacements);
// Build final string by replacing matched regions with evaluated replacement expressions
```

### Capture Variable Function

```c
StradaValue* strada_capture_var(int n);
// Returns the Nth capture group from the most recent regex match.
// $1 = capture group 1, $2 = capture group 2, ..., $9 = capture group 9.
// Index 0 returns the full match (same as captures()[0]).
// Returns an OWNED reference (incref'd from the captures array).
// Returns undef if no captures exist or index is out of range.
```

### Tie Functions

```c
void strada_tie_hash(StradaValue *sv, const char *classname, int argc, ...);
// Tie a hash to a class - calls TIEHASH constructor, sets is_tied=1 and tied_obj

void strada_untie(StradaValue *ref);
// Untie a variable - clears is_tied flag and tied_obj

StradaValue* strada_tied(StradaValue *ref);
// Returns the tied object, or undef if not tied
```

### Type Conversion Functions

```c
int64_t strada_to_int(StradaValue *sv);
double strada_to_num(StradaValue *sv);
char* strada_to_str(StradaValue *sv);
int strada_to_bool(StradaValue *sv);
void* strada_to_pointer(StradaValue *sv);
```

---

## Code Generation Patterns

### Variable Declaration

```strada
# Strada
my int $x = 5;

# Generated C
StradaValue *x = strada_new_int(5);
```

### Our Variable Declaration

`our` variables are backed by the global registry — no local C variable is emitted.

```strada
# Strada
our int $count = 0;

# Generated C (in main initialization)
strada_global_set(strada_new_str("main::count"), strada_new_int(0));

# Read: strada_global_get(strada_new_str("main::count"))
# Write: strada_global_set(strada_new_str("main::count"), <rhs>)
# += :   ({ StradaValue *__our_old = strada_global_get(...);
#           StradaValue *__our_rhs = <rhs>;
#           StradaValue *__our_new = strada_new_num(strada_to_num(__our_old) + strada_to_num(__our_rhs));
#           strada_decref(__our_rhs); strada_decref(__our_old);
#           strada_global_set(..., __our_new); })
```

The codegen tracks our variables in `$cg->{"our_vars"}` (maps C name → `"pkg::name"`). Pre-registered before function codegen so references work. `needs_temp_cleanup()` returns 1 for our variable reads.

### Array with Initial Capacity

```strada
# Strada
my array @large[1000];

# Generated C
StradaValue *large = strada_new_array_with_capacity(1000);
```

### Hash with Initial Capacity

```strada
# Strada
my hash %cache[500];

# Generated C
StradaValue *cache = strada_new_hash_with_capacity(500);
```

### Struct Declaration

```strada
# Strada
my Person $p;

# Generated C
Person *p = malloc(sizeof(Person));

# CodeGen also registers in context:
$cg->{"struct_vars"}->{"p"} = "Person";
```

### Function Call

```strada
# Strada
say("hello");

# Generated C
strada_say(strada_new_str("hello"));
```

### Reference Creation

```strada
# Strada (scalar ref)
my scalar $ref = \$x;

# Generated C
StradaValue *ref = strada_new_ref(x, '$');
```

```strada
# Strada (function ref)
my scalar $fref = \&my_func;

# Generated C
StradaValue *fref = strada_cpointer_new((void*)my_func);
```

### Hash Access

```strada
# Strada
$h{"key"} = "value";

# Generated C (key strada_to_str captured and freed, literal temp decreffed)
({ StradaValue *__hs_kv = strada_new_str("key"); char *__hs_ks = strada_to_str(__hs_kv); strada_hv_store(strada_deref_hash(h), __hs_ks, strada_new_str("value")); free(__hs_ks); strada_decref(__hs_kv); })
```

### Arrow Dereference

```strada
# Strada
$ref->{"key"}

# Generated C (key strada_to_str captured and freed)
({ StradaValue *__hg_kv = strada_new_str("key"); char *__hg_ks = strada_to_str(__hg_kv); StradaValue *__hg_r = strada_hv_fetch(strada_deref_hash(ref), __hg_ks); free(__hg_ks); strada_decref(__hg_kv); __hg_r; })
```

### Foreach Loop

```strada
# Strada
foreach my int $x (@arr) {
    say($x);
}

# Generated C
{
    StradaValue *__foreach_arr_0 = arr;
    StradaArray *__foreach_av_0 = strada_deref_array(__foreach_arr_0);
    int __foreach_len_0 = strada_array_length(__foreach_av_0);
    for (int __foreach_i_0 = 0; __foreach_i_0 < __foreach_len_0; __foreach_i_0++) {
        StradaValue *x = strada_array_get(__foreach_av_0, __foreach_i_0);
        strada_say(x);
    }
}
```

### Try/Catch

```strada
# Strada
try {
    risky();
} catch ($e) {
    handle($e);
}

# Generated C
if (setjmp(*STRADA_TRY_PUSH()) == 0) {
    risky();
    STRADA_TRY_POP();
} else {
    STRADA_TRY_POP();
    StradaValue *e = strada_get_exception();
    handle(e);
}
```

### Labeled Loop

```strada
# Strada
OUTER: while ($cond) {
    last OUTER;
    next OUTER;
}

# Generated C
while (strada_to_bool(cond)) {
    goto OUTER_break;      // last OUTER
    goto OUTER_continue;   // next OUTER
    OUTER_continue: ;
}
OUTER_break: ;
```

### Signal Handler

```strada
# Strada
signal("INT", \&my_handler);

# Generated C
strada_signal(strada_new_str("INT"), strada_cpointer_new((void*)my_handler));
```

### Anonymous Functions (Closures)

```strada
# Strada - void closure (no return)
my scalar $cb = func () {
    say("Hello from closure");
};

# Generated C
StradaValue* __anon_func_0(StradaValue ***__captures) {
    strada_say(strada_new_str("Hello from closure"));
    return strada_undef_static();  /* IMPORTANT: implicit return */
}

StradaValue *cb = strada_closure_new((void*)&__anon_func_0, 0, 0, NULL);
```

**IMPORTANT:** All closures unconditionally add `return strada_undef_static();` at the end.
- If the closure has an explicit return, this is unreachable and optimized away
- Without this, closures with no return would return garbage (undefined C behavior)
- This prevents segfaults when `strada_closure_call()` result is decremented

```strada
# Strada - closure with captured variables
my int $multiplier = 2;
my scalar $double = func (int $n) {
    return $n * $multiplier;
};

# Generated C
StradaValue* __anon_func_1(StradaValue ***__captures, StradaValue *n) {
    StradaValue *__retval = strada_new_int(
        strada_to_int(n) * strada_to_int((*__captures[0]))  /* multiplier */
    );
    return __retval;
    return strada_undef_static();  /* unreachable but always present */
}

StradaValue *double_ = strada_closure_new(
    (void*)&__anon_func_1,
    0,                              /* param_count for closure_call (not counting captures) */
    1,                              /* capture_count */
    (StradaValue**[]){&multiplier}  /* array of pointers to captured vars */
);
```

### Raw C Code Block

```strada
# Strada
__C__ {
    printf("Hello from C!\n");
    int x = 42;
}

# Generated C
/* Begin __C__ block */
printf("Hello from C!\n");
int x = 42;
/* End __C__ block */
```

The `__C__` block emits its contents verbatim to the C output. The lexer handles:
- Nested braces (tracks depth)
- C strings (braces inside `"..."` don't count)
- C char literals (braces inside `'...'` don't count)
- C comments (`/* */` and `//`)

### Async Function Definition

```strada
# Strada
async func compute(int $n) int {
    core::usleep(10000);
    return $n * 2;
}

# Generated C (two functions)

/* Inner closure function - runs in thread pool */
static StradaValue* __async_compute_inner(StradaValue ***__captures) {
    StradaValue *n = (*__captures[0]);
    strada_usleep(strada_new_int(10000));
    { StradaValue *__retval = strada_new_int(strada_to_int(n) * 2);
      return __retval; }
    return strada_new_undef();
}

/* Outer wrapper - creates future and submits to pool */
StradaValue* compute(StradaValue* n) {
    StradaValue *__closure = strada_closure_new((void*)&__async_compute_inner,
                                                 0, 1, (StradaValue**[]){&n});
    return strada_future_new(__closure);
}
```

### Await Expression

```strada
# Strada
my int $result = await $future;

# Generated C
StradaValue *result = strada_future_await(future);
```

### async:: Namespace Calls

```strada
# Strada
my array @results = async::all(\@futures);

# Generated C
StradaValue *results = strada_future_all(strada_new_ref(futures, '@'));
```

```strada
# Strada
my str $winner = async::race(\@futures);

# Generated C
StradaValue *winner = strada_future_race(strada_new_ref(futures, '@'));
```

```strada
# Strada
async::cancel($future);

# Generated C
strada_future_cancel(future);
```

```strada
# Strada
if (async::is_done($future)) { ... }

# Generated C
if (strada_to_bool(strada_new_int(strada_future_is_done(future)))) { ... }
```

### Transliteration (`tr///` / `y///`)

```strada
# Strada
$str =~ tr/a-z/A-Z/;

# Generated C
strada_tr(str, "a-z", "A-Z", "");
```

The lexer has a dedicated `lex_read_tr_literal()` function that parses the `tr/search/replace/flags` syntax. It handles:
- Both `tr` and `y` as the operator keyword
- Arbitrary delimiters (though `/` is standard)
- Escape sequences within the search and replacement lists
- Flags: `c` (complement), `d` (delete), `s` (squeeze), `r` (return copy without modifying)

The parser recognizes `TR_LITERAL` tokens and builds a `NODE_TR` AST node with fields:
- `target` - the variable being transliterated
- `search_list` - the characters to search for
- `replacement_list` - the replacement characters
- `flags` - modifier flags string

**Lexer detection:** In the `expect_regex` block, when `tr` or `y` is encountered, the lexer calls `lex_read_tr_literal()` instead of `lex_read_regex()`.

### Regex Substitution with `/e` Flag

```strada
# Strada
$str =~ s/(\d+)/$1 * 2/e;

# Generated C (conceptual flow)
StradaValue *__matches = strada_regex_find_all(str_c, "(\d+)", "e", 1);
StradaValue *__replacements = strada_new_array();
for (int __i = 0; __i < match_count; __i++) {
    strada_set_captures_sv(match[__i]);   // set $1, $2, etc.
    StradaValue *__eval_result = /* gen_expression(eval_expr) */;
    strada_array_push(__replacements, __eval_result);
}
StradaValue *__result = strada_regex_build_result(str_c, __matches, __replacements);
```

The `/e` flag causes the replacement to be treated as an expression to evaluate rather than a literal string. The code generator:

1. Calls `strada_regex_find_all()` to find all match positions and capture groups
2. Loops over each match, calling `strada_set_captures_sv()` to set up capture variables
3. Evaluates the replacement expression (via `gen_expression()` on the parsed eval expression) for each match
4. Collects all replacement strings into an array
5. Calls `strada_regex_build_result()` to assemble the final string by splicing replacements into the original

**Lexer support:** The `/e` flag is recognized in `lex_read_subst_literal()` and stored in the substitution flags. The parser stores the replacement as a parseable expression rather than a literal string when `/e` is present.

### Local (Dynamic Scoping)

```strada
# Strada
local $var = "temp_value";

# Generated C
strada_local_save("main::var");          // Save current value
strada_global_set(strada_new_str("main::var"), strada_new_str("temp_value"));

// At scope exit (or return):
strada_local_restore_to(__local_depth);  // Restore to saved depth
```

`local()` provides Perl-style dynamic scoping for global variables. The implementation:

**Parser:** The `local` keyword produces a `NODE_LOCAL_DECL` AST node. The `var_name` field holds the variable name, and the optional `init` field holds the initialization expression.

**CodeGen pattern:**
1. At the point of the `local` declaration, emit `strada_local_save("pkg::varname")` to push the current value onto a save stack
2. If there is an initializer, emit a `strada_global_set()` to set the new value
3. At every scope exit point (end of block, return statements), emit `strada_local_restore_to(depth)` where `depth` is the save stack depth recorded at scope entry
4. The codegen tracks the local save depth using `strada_local_depth_get()` at scope entry

**Runtime save stack:** The local save/restore functions maintain an internal stack of `(name, old_value)` pairs. `strada_local_restore_to(depth)` pops and restores entries until the stack depth matches the target. This correctly handles nested `local` declarations and early returns.

### Capture Variables (`$1`-`$9`)

```strada
# Strada
if ($str =~ /(\d+)-(\w+)/) {
    say($1);   # First capture group
    say($2);   # Second capture group
}

# Generated C
if (strada_to_bool(strada_regex_match_with_capture(str_c, "(\\d+)-(\\w+)", ""))) {
    ({ StradaValue *__say_tmp = strada_capture_var(1); strada_say(__say_tmp); strada_decref(__say_tmp); });
    ({ StradaValue *__say_tmp = strada_capture_var(2); strada_say(__say_tmp); strada_decref(__say_tmp); });
}
```

The `$1`-`$9` syntax provides a shorthand for `captures()[1]` through `captures()[9]`. Each `$N` compiles directly to `strada_capture_var(N)`, which returns an **owned** `StradaValue*` (incremented refcount from the internal captures array).

**Parser:** In `parse_primary()`, when the `DOLLAR` token is followed by an `INT_LITERAL` token whose value is between 1 and 9, the parser creates a `NODE_CAPTURE_VAR` node with the `number` field set to the digit value, instead of treating it as a variable reference.

**CodeGen:** `NODE_CAPTURE_VAR` emits `strada_capture_var(<number>)`.

**Memory management:** `strada_capture_var()` returns an owned reference, so `NODE_CAPTURE_VAR` is registered in `needs_temp_cleanup()` as returning 1. This ensures that when `$1` is used in expressions (e.g., string concatenation `"prefix: " . $1`), the temporary result is properly decremented after use.

### Tie (Tied Hashes)

```strada
# Strada
tie(%hash, "MyTiedHash", @args);

# Generated C
strada_tie_hash(hash, "MyTiedHash", argc, ...);
```

Tie allows a hash to delegate its operations (FETCH, STORE, EXISTS, DELETE) to a user-defined class. The implementation is split between the code generator (compile-time inline checks) and the runtime (tie registration and object management).

**StradaValue changes:** Two new fields (`is_tied` and `tied_obj`) are added to `StradaValue`. When `is_tied` is set, hash operations dispatch through the tied object's methods.

**CodeGen inline wrapper pattern:** Hash access operations (`strada_hv_fetch`, `strada_hv_store`, `strada_hv_exists`, `strada_hv_delete`) are generated as inline wrappers that check the `is_tied` flag using `__builtin_expect` for branch prediction (the common case is untied):

```c
// Generated hash fetch with tie check
({ StradaValue *__hv = hash;
   if (__builtin_expect(__hv->is_tied, 0)) {
       // Dispatch to FETCH method on tied_obj
       strada_method_call(__hv->tied_obj, "FETCH", key_args);
   } else {
       strada_hash_get(__hv->value.hv, key);
   }
})
```

Similarly for STORE, EXISTS, and DELETE operations.

**`untie(%hash)`** clears the `is_tied` flag and decrefs the `tied_obj`.

**`tied(%hash)`** returns the tied object (for direct method calls on it), or undef if not tied.

### File Handle I/O

File handles in Strada are `StradaValue*` with type `STRADA_FILEHANDLE`. The `value.fh` union field stores the underlying `FILE*` pointer. File handles are reference-counted — when refcount drops to 0, `strada_free_value()` calls `fclose()` on the `FILE*`.

#### Opening Files

```strada
# Strada
my scalar $fh = core::open("file.txt", "r");

# Generated C (now uses strada_open_sv for runtime type dispatch)
StradaValue *fh = ({ StradaValue *__op_v0 = strada_new_str("file.txt"); StradaValue *__op_v1 = strada_new_str("r"); StradaValue *__op_r = strada_open_sv(__op_v0, __op_v1); strada_decref(__op_v0); strada_decref(__op_v1); __op_r; });
```

`strada_open_sv()` dispatches at runtime: if the first argument is a `STRADA_REF`, it opens an in-memory handle (using `fmemopen`/`open_memstream`); otherwise it extracts strings and calls `strada_open()`. Returns a `STRADA_FILEHANDLE` value on success, or `STRADA_UNDEF` on failure. Valid modes: `"r"`, `"w"`, `"a"`, `"r+"`, `"w+"`, `"a+"`, `"rw"`.

#### In-Memory I/O (FhMeta Side Table)

Special file handles (pipes, in-memory streams) are tracked by a linked-list **side table** (`StradaFhMeta`), keyed by `FILE*`. This avoids adding fields to `StradaValue` (which would increase memory for all values).

```c
typedef enum { FH_NORMAL=0, FH_PIPE=1, FH_MEMREAD=2, FH_MEMWRITE=3, FH_MEMWRITE_REF=4 } StradaFhType;
typedef struct StradaFhMeta { FILE *fh; StradaFhType fh_type; char *mem_buf; size_t mem_size; StradaValue *target_ref; struct StradaFhMeta *next; } StradaFhMeta;
```

- `FH_PIPE`: Created by `strada_popen()`. `strada_close_fh_meta()` calls `pclose()` instead of `fclose()`.
- `FH_MEMREAD`: Created by `strada_open_str("...", "r")`. Uses `fmemopen()` with a copied buffer. Close frees buffer.
- `FH_MEMWRITE`: Created by `strada_open_str("", "w")`. Uses `open_memstream()`. `strada_str_from_fh()` extracts accumulated output.
- `FH_MEMWRITE_REF`: Created by `strada_open_sv(\$var, "w")`. On close, writes buffer contents back to the referenced `StradaValue`.

All existing I/O functions work transparently because in-memory handles are real `FILE*` pointers.

#### Diamond Operator (Reading Lines)

```strada
# Strada
my str $line = <$fh>;

# Generated C
StradaValue *line = strada_read_line(fh);
```

The parser recognizes `< $varname >` and creates a `NODE_READLINE` AST node (ID 124) with the `handle` field pointing to the variable. `NODE_READLINE` is listed in `needs_temp_cleanup()` as returning 1 (owned value). `strada_read_line()` returns the next line including the trailing newline, or `STRADA_UNDEF` at EOF.

#### Print/Say to Filehandle

```strada
# Strada
say($fh, "hello");       # With newline
print($fh, "hello");     # Without newline

# Generated C (say with 2 args — first arg is filehandle)
({
    StradaValue *__say_tmp = strada_new_str("hello");
    strada_say_fh(__say_tmp, fh);
    strada_decref(__say_tmp);
});

# Generated C (print with 2 args — first arg is filehandle)
({
    StradaValue *__print_tmp = strada_new_str("hello");
    strada_print_fh(__print_tmp, fh);
    strada_decref(__print_tmp);
});
```

The code generator detects `say()` and `print()` with 2+ arguments. If the first argument is a variable known to be a filehandle (or any scalar), it emits `strada_say_fh()`/`strada_print_fh()` instead of the plain `strada_say()`/`strada_print()`. The temp cleanup pattern wraps the expression to ensure the printed value is decremented.

#### Closing

```strada
# Strada
core::close($fh);

# Generated C
strada_close(fh);
```

Explicit close. Also happens automatically when the `StradaValue*` refcount drops to 0 (e.g., variable goes out of scope and `strada_decref()` triggers `strada_free_value()` which calls `fclose()`).

#### Seeking and Position

```strada
# Strada
core::seek($fh, $offset, $whence);   # 0=SEEK_SET, 1=SEEK_CUR, 2=SEEK_END
my int $pos = core::tell($fh);
core::rewind($fh);
my int $at_eof = core::eof($fh);
core::flush($fh);

# Generated C
strada_seek(fh, strada_new_int(offset), strada_new_int(whence));
StradaValue *pos = strada_tell(fh);
strada_rewind(fh);
StradaValue *at_eof = strada_eof(fh);
strada_flush(fh);
```

#### Bulk I/O

```strada
# Strada
my str $all = core::slurp("file.txt");      # Read entire file into string
core::spew("file.txt", $content);            # Write string to file (overwrite)

# Generated C
StradaValue *all = strada_slurp(strada_new_str("file.txt"));
strada_spew(strada_new_str("file.txt"), content);
```

Also available: `core::slurp_fh($fh)` (slurp from open handle), `core::spew_fh($fh, $data)` (write to open handle).

#### Pipe I/O

```strada
# Strada
my scalar $pipe = core::popen("ls -la", "r");
my str $line = <$pipe>;
core::pclose($pipe);

# Generated C
StradaValue *pipe = strada_popen(strada_new_str("ls -la"), strada_new_str("r"));
StradaValue *line = strada_read_line(pipe);
strada_pclose(pipe);
```

`core::popen()` returns a `STRADA_FILEHANDLE` wrapping a pipe. The diamond operator works on pipes the same as regular files.

#### Character-Level I/O

```strada
# Strada
my int $ch = core::fgetc($fh);
core::fputc($fh, $ch);
my str $buf = core::fgets($fh, $maxlen);
core::fputs($fh, $str);

# Generated C
StradaValue *ch = strada_fgetc(fh);
strada_fputc(fh, ch);
StradaValue *buf = strada_fgets(fh, strada_new_int(maxlen));
strada_fputs(fh, str);
```

#### Select (Default Output Filehandle)

```strada
# Strada
my scalar $old = select($fh);    # Set default, returns previous
my scalar $cur = select();       # Get current (no args)

# Generated C
StradaValue *old = strada_select(fh);
StradaValue *cur = strada_select_get();
```

When a default filehandle is set via `select()`, plain `say()` and `print()` (without a filehandle argument) write to it instead of stdout.

#### Memory Management Notes

- `strada_open()`, `strada_read_line()`, `strada_slurp()`, `strada_popen()` all return **owned** values — they must be decremented when no longer needed
- The diamond operator `<$fh>` is registered in `needs_temp_cleanup()` so temps are cleaned up in expressions
- When `strada_free_value()` encounters `STRADA_FILEHANDLE`, it calls `fclose()` on `value.fh` (unless it's stdin/stdout/stderr)
- `core::fileno($fh)` returns the integer file descriptor — useful for `core::read_fd()`/`core::write_fd()` low-level I/O

---

## Adding New Features Checklist

### Adding a New Built-in Function

1. **runtime/strada_runtime.c** - Implement the function
2. **runtime/strada_runtime.h** - Declare the function
3. **compiler/Semantic.strada** - Add to `get_builtins()`: `$b{"func_name"} = 1;`
4. **compiler/CodeGen.strada** - Add code generation in `gen_expression()` for NODE_CALL
5. **Rebuild**: `make clean && make`
6. **Test**: Create test file in `examples/`

### Adding a New Statement Type

1. **compiler/AST.strada** - Add `NODE_*()` constant and `ast_new_*()` constructor
2. **compiler/Lexer.strada** - Add keyword token if needed
3. **compiler/Parser.strada** - Add parsing in `parse_statement()` or relevant function
4. **compiler/CodeGen.strada** - Add code generation in `gen_statement()`
5. **compiler/Semantic.strada** - Add analysis if needed
6. **Rebuild and test**

### Adding a New Expression Type

1. **compiler/AST.strada** - Add `NODE_*()` constant and `ast_new_*()` constructor
2. **compiler/Lexer.strada** - Add operator token if needed
3. **compiler/Parser.strada** - Add parsing at correct precedence level
4. **compiler/CodeGen.strada** - Add code generation in `gen_expression()`
5. **Rebuild and test**

### Adding a New Operator

1. **compiler/Lexer.strada** - Add token recognition
2. **compiler/Parser.strada** - Add to precedence climbing or special handling
3. **compiler/CodeGen.strada** - Add code generation for the binary/unary op
4. **runtime/strada_runtime.c** - Add runtime function if needed
5. **Rebuild and test**

---

## Compiler Data Flow

### File Processing Order

```
compiler/AST.strada       # Node definitions (loaded first)
compiler/Lexer.strada     # Tokenizer
compiler/Parser.strada    # Parser (uses AST, Lexer)
compiler/Semantic.strada  # Analysis (uses AST)
compiler/CodeGen.strada   # Code generation (uses AST)
compiler/Main.strada      # Entry point
```

### Build Process

```
1. cat all .strada files -> Combined.strada
2. bootstrap/stradac Combined.strada -> Combined.c
3. gcc Combined.c + runtime -> ./stradac
```

### Compilation Phases

```
Source (.strada)
    ↓
Lexer (tokens)
    ↓
Parser (AST)
    ↓
Semantic Analysis (validation)
    ↓
CodeGen (C source)
    ↓
GCC (executable)
```

### CodeGen Context Structure

```strada
$cg->{"output"}        # Generated C code string
$cg->{"indent"}        # Current indentation level
$cg->{"functions"}     # Map: function name -> info
$cg->{"in_main"}       # Boolean: inside main()?
$cg->{"package"}       # Current package name
$cg->{"struct_vars"}   # Map: variable name -> struct type
$cg->{"struct_defs"}   # Map: struct name -> field info
$cg->{"foreach_counter"} # Counter for unique foreach vars
$cg->{"loop_stack"}    # Stack of loop labels for last/next

# OOP Auto-Registration (added 2026-01-07)
$cg->{"method_count"}  # Number of methods to auto-register
$cg->{"methods"}       # Array of method info hashes
$cg->{"method_wrappers"} # Generated wrapper function code
```

### OOP Method Auto-Registration

When a `package` declaration exists, the compiler:

1. **Tracks methods** via `codegen_track_method()`:
   - Functions matching `Package_method($self, ...)` pattern
   - Must have at least 1 parameter (self)
   - Skips extern functions and main

2. **Generates wrapper functions** via `gen_method_wrappers()`:
   ```c
   static StradaValue* __wrap_Animal_speak(StradaValue* self, StradaValue* args) {
       (void)args;
       Animal_speak(self);
       return strada_new_undef();  // void method
   }
   ```

3. **Generates init function** called from main():
   ```c
   void __Animal_oop_init(void) {
       if (__Animal_oop_initialized) return;
       __Animal_oop_initialized = 1;
       strada_method_register("Animal", "speak", __wrap_Animal_speak);
       // ... more methods
   }
   ```

4. **Key functions in CodeGen.strada:**
   - `codegen_track_method($cg, $fn, $pkg)` - Track a method for registration
   - `gen_method_wrappers($cg, $pkg)` - Generate wrapper functions
   - `gen_oop_helper_decl()` - Generate `__strada_get_arg()` helper

### AUTOLOAD Dispatch (Runtime)

When `strada_method_call()` fails to find a method via `oop_lookup_method()`, it tries AUTOLOAD:

1. Looks up `"AUTOLOAD"` via `oop_lookup_method()` (walks inheritance chain)
2. If found, builds a new args array: `[method_name, original_args...]`
3. Calls the AUTOLOAD wrapper with the augmented args
4. The wrapper unpacks: `$method` from args[0], `...@args` from remaining elements

No compiler changes needed — AUTOLOAD is a regular method name that gets auto-detected, wrapped, and registered by the existing OOP machinery. The fallback happens entirely in the runtime.

### Operator Overloading (use overload)

The compiler supports Perl-style `use overload` for defining custom operator behavior on blessed objects. The design ensures **zero overhead** when overloading is not used.

**Parser (`parse_use_overload`):**
- Parses `use overload "op" => "method", ...;` syntax
- Stores mappings on the program node: `$program->{"overloads"}->{$package}->{$op} = $method_name`
- Sets `$program->{"has_overloads"} = 1` and `$program->{"overloaded_ops"}->{$op} = 1`

**CodeGen — Three-tier dispatch:**
1. No `use overload` anywhere → all operators generate inline C (identical to today)
2. Operator IS overloaded but operands are typed (`int`, `num`, `str`) → inline C (can't be blessed)
3. Operator IS overloaded AND operand is `scalar` → emit runtime dispatch with inline fallback

**Key CodeGen functions:**
- `could_be_blessed($cg, $expr)` — returns 1 if expression could produce a blessed object
- `check_overload_binary($cg, $left, $right, $op)` — checks tiers and calls `gen_overloaded_binary` if needed
- `gen_overloaded_binary($cg, $left, $right, $op)` — emits `strada_overload_binary()` call with fallback
- `gen_overloaded_unary($cg, $operand, $op)` — emits `strada_overload_unary()` call with fallback
- `gen_overloaded_concat($cg, $left, $right)` — emits stringify-or-default for `.` operator

**Generated C pattern (binary):**
```c
({ StradaValue *__ol_l = <left>;
   StradaValue *__ol_r = <right>;
   StradaValue *__ol_res = strada_overload_binary(__ol_l, __ol_r, "+");
   if (!__ol_res) __ol_res = strada_new_num(strada_to_num(__ol_l) + strada_to_num(__ol_r));
   __ol_res; })
```

**Overload registration** is generated inside `gen_method_wrappers_for_pkg()`, emitting `strada_overload_register()` calls in the OOP init function.

**Runtime functions** (`strada_runtime.c`):
- `strada_overload_register(package, op, func)` — register an overload handler
- `strada_overload_lookup(package, op)` — find handler (linear search, follows inheritance)
- `strada_overload_binary(left, right, op)` — check left then right for overload, return result or NULL
- `strada_overload_unary(operand, op)` — check operand for overload, return result or NULL
- `strada_overload_stringify(val)` — return stringified value if `""` overloaded, else NULL

**OopPackage extension:**
```c
typedef struct {
    /* ... existing fields ... */
    OverloadEntry overloads[OOP_MAX_OVERLOADS]; /* op -> func mappings */
    int overload_count;
} OopPackage;
```

### Moose-Style OOP (`has`, `extends`, `with`, `before`/`after`/`around`)

Strada supports a Moose-inspired declarative OOP system. The entire feature is implemented as **parser-level desugaring** -- the parser transforms the declarative syntax into ordinary AST nodes (function definitions, hash access, bless calls) that the existing CodeGen and runtime already understand.

#### New Lexer Tokens

Six new keyword tokens are recognized in `lex_keyword_or_ident()`:

| Token | Keyword | Purpose |
|-------|---------|---------|
| `HAS` | `has` | Declare an attribute with auto-generated getter/setter |
| `EXTENDS` | `extends` | Declare parent class(es) -- sugar for `inherit` |
| `WITH` | `with` | Include role(s) -- currently identical to `extends` at the runtime level |
| `BEFORE` | `before` | Register a "before" method modifier |
| `AFTER` | `after` | Register an "after" method modifier |
| `AROUND` | `around` | Register an "around" method modifier |

All six tokens can also be used as variable names after a sigil (e.g., `$has`, `$before`) via `parse_var_name()`, so they are context-sensitive keywords.

#### `has` Declarations and Desugaring

**Syntax:**
```strada
has [ro|rw] type $name [= default] [(options)];
```

**Parser function:** `parse_has_declaration()` (Parser.strada line ~4590)

The parser desugars each `has` into concrete AST nodes:

1. **Attribute metadata** is stored on the program node in `$program->{"has_attrs"}` (an array of hashes, each with fields: `name`, `attr_type`, `default_node`, `required`, `lazy`, `builder`, `rw`, `package`).

2. **Getter function** is generated as a `NODE_FUNC` AST node:
   ```strada
   # has ro str $name;  in package Foo desugars to:
   func Foo_name(scalar $self) str {
       return $self->{"name"};
   }
   ```
   The function name is `C_PKG . "_" . attr_name`. The body is a single `NODE_RETURN_STMT` containing a `NODE_DEREF_HASH` on `$self`.

3. **Setter function** (only when `rw` is specified):
   ```strada
   # has rw int $age;  in package Foo desugars to:
   func Foo_set_age(scalar $self, int $val) void {
       $self->{"age"} = $val;
   }
   ```

Both getter and setter are added to the program's function list via `ast_add_function()` and are auto-registered as OOP methods by the existing `codegen_track_method()` machinery.

**Supported options** (in parentheses after the name):
- `required` -- marks attribute as required (stored in metadata, for future validation)
- `lazy` -- marks attribute as lazy (stored in metadata, for future builder support)
- `builder => "method_name"` -- specifies a builder method name (stored in metadata)

#### `extends` and `with`

**`extends`** is sugar for `inherit`. The parser calls `parse_parent_list()` which accepts one or more comma-separated package names (including `::` qualified names) and calls `ast_add_inherit($program, $child_pkg, $parent_name)` for each.

```strada
package Dog;
extends Animal;          # Desugars to: inherit Animal;
extends Animal, Pet;     # Desugars to: inherit Animal; inherit Pet;
```

**`with`** is currently identical to `extends` at the implementation level -- it calls the same `parse_parent_list()` function. The distinction exists for future role-specific semantics.

#### Auto-Constructor Generation (`flush_has_constructor`)

When a package has `has` declarations, the parser auto-generates a `new()` constructor. This happens at two points:
1. When `parse_program()` encounters a new `package` declaration (flushes the previous package)
2. At the end of `parse_program()` (flushes the final package)

**Function:** `flush_has_constructor($parser, $program, $pkg)` (Parser.strada line ~4718)

**Inheritance chain collection** (breadth-first):
1. Start with `@pkg_chain = ($pkg)`
2. Walk `$program->{"inherits"}` entries; for each entry where `child == current_pkg`, add parent to chain (if not already present)
3. Repeat for each newly added parent (BFS via index `$chain_i`)
4. Collect all `has_attrs` whose `package` is in `@pkg_chain`

This means a subclass automatically gets all parent attributes in its constructor.

**Skip conditions:**
- If `$pkg` is empty or "main"
- If `$pkg_attr_count == 0` (no attributes from this package or parents)
- If an explicit `new()` function already exists in the program

**Generated constructor AST** (for package `Foo` with attributes `name`, `age`):
```strada
func Foo_new(scalar ...@args) scalar {
    my hash %self = ();
    my hash %__a = ();
    my int $__i = 0;
    while ($__i + 1 < scalar(@args)) {
        $__a{$args[$__i]} = $args[$__i + 1];
        $__i = $__i + 2;
    }
    $self{"name"} = $__a{"name"} // default_expr;   # // only if default exists
    $self{"age"} = $__a{"age"};
    return bless(\%self, "Foo");
}
```

The constructor is variadic (`scalar ...@args`) and parses named arguments from a flat key-value list. Each attribute uses the defined-or operator (`//`) to apply defaults when the argument is not provided.

#### Method Modifiers (`before`, `after`, `around`)

**Syntax:**
```strada
before "method_name" func(scalar $self) void { ... }
after "method_name" func(scalar $self) void { ... }
around "method_name" func(scalar $self, scalar $orig, scalar ...@args) scalar { ... }
```

**Parser function:** `parse_method_modifier($parser, $program, $mod_type)` (Parser.strada line ~4906)

The parser:
1. Consumes the `BEFORE`/`AFTER`/`AROUND` token
2. Reads the method name as a string literal
3. Parses `func(params) [type] { body }` as a normal function definition
4. Assigns a unique name: `C_PKG . "___" . mod_type . "_" . method_name . "_" . mod_count`
5. Adds the function to the program via `ast_add_function()`
6. Stores modifier metadata in `$program->{"method_modifiers"}` (array of hashes with: `mod_type`, `method_name`, `func_name`, `package`)

**AST storage on program node** (initialized in `ast_new_program()`):
```strada
$node->{"has_attrs"}             # Array of attribute metadata hashes
$node->{"has_attr_count"}        # Count of attributes
$node->{"method_modifiers"}      # Array of modifier metadata hashes
$node->{"method_modifier_count"} # Count of modifiers
```

#### Method Modifier Code Generation

In `gen_method_wrappers_for_pkg()` (CodeGen.strada line ~598), after generating method wrappers and overload registrations, the function emits `strada_modifier_register()` calls for each modifier that belongs to the current package.

**Generated C code example:**
```c
/* Method modifiers */
strada_modifier_register("Dog", "speak", 1, __wrap_Dog___before_speak_0);
strada_modifier_register("Dog", "speak", 2, __wrap_Dog___after_speak_0);
```

The type integer mapping:
- `1` = before
- `2` = after
- `3` = around

The modifier functions are wrapped by the same wrapper generation code as normal methods (via `__wrap_` prefix), since they have the same `(self, args)` signature.

#### Runtime Modifier Infrastructure

**`OopModifier` struct** (`strada_runtime.c` line ~9239):
```c
typedef struct {
    char method_name[OOP_MAX_NAME_LEN];
    int type;      /* 1=before, 2=after, 3=around */
    StradaMethod func;
} OopModifier;
```

Each `OopPackage` holds up to `OOP_MAX_MODIFIERS` (64) modifiers in its `modifiers[]` array with a `modifier_count`.

**`strada_modifier_register()`** (`strada_runtime.c` line ~9430):
- Takes `package`, `method`, `type` (1/2/3), `func`
- Adds a new `OopModifier` entry to the package

**Dispatch in `strada_method_call()`** (`strada_runtime.c` line ~9731):

When a method call succeeds (method found), modifiers are checked and dispatched:

1. **Before modifiers** (type 1): All matching modifiers are called with `(obj, empty_args)` before the actual method. They can inspect/modify `$self` but cannot prevent the method from running.

2. **Around modifiers** (type 3): Only the **first** matching around modifier is used. It receives a modified args array where `args[0]` is the original method function pointer (as an int). The around modifier can choose to call the original method or replace its behavior entirely.

3. **The actual method** is called (or skipped if an around modifier handles it).

4. **After modifiers** (type 2): All matching modifiers are called with `(obj, empty_args)` after the method returns. They cannot modify the return value.

If the package has **no modifiers** (`modifier_count == 0`), the method is called directly with zero modifier overhead.

**Dispatch inline cache:** `strada_method_call` is a thin wrapper that hashes the method name (djb2 over unsigned bytes) and calls `method_call_impl`; `strada_method_call_ph` is the codegen entry point for literal method names with the hash precomputed at compile time (`djb2_hash_32`, gated on `key_is_hashable_ascii`). Resolution goes through a file-scope 64-entry direct-mapped cache (`mc_cache`) keyed by `(pkg_name_ptr >> 3) ^ method_hash`, validated by pointer-equal package, `strcmp` on the stored method name, and a generation stamp (`mc_cache_gen`, bumped by `method_cache_invalidate()` and `strada_modifier_register()`). Each entry also memoizes `has_mod` — whether the (pkg, method) has any modifier in its MRO — computed lazily on the first call after `oop_any_modifiers` is set, so the no-hook fast path is one flag test instead of an MRO walk per call.

**Per-package method index + flattened MRO cache:** each `OopPackage` carries `method_index` (open-addressed hash, slot → `methods[]` position, −1 = empty, ≥2× capacity power-of-two, rebuilt by `oop_method_index_rebuild()` on every `strada_method_register` — registration is rare) and `mro_cache` (flattened MRO, this package first, depth-first left-to-right deduped, built lazily by `oop_collect_mro()` and validated by `mro_cache_gen == mc_cache_gen`). `oop_lookup_method` and `strada_overload_lookup` iterate the flat MRO calling `pkg_method_find()` — one djb2 probe per package, linear-scan fallback if the index allocation failed; the old recursive `oop_lookup_method_in`/`oop_overload_lookup_in` walks are gone. Resolution order is identical to the old DFS pre-order. import_lib note: `.so` modules do not bundle the runtime (symbols bind to the host via `-rdynamic`), so lib-side registration goes through the host's registry and generation counter — a lazily-loaded `.so` adding methods or modifiers mid-run invalidates the host's warmed caches (regression-tested by `t/test_import_lib_hooks.strada`).

**Per-call-site monomorphic cache:** on top of the global cache, codegen emits a function-scope `static StradaCallSite` (struct: `pkg`, `fn`, `gen`, `has_mod` — defined in both runtime headers) at every literal-name call site and dispatches through `strada_method_call_cs(obj, "name", args, HASH, &site)`. The fast path checks `gen == mc_cache_gen && pkg == SV_BLESSED(obj) && has_mod == 0`, then calls `fn` directly (with the same args cleanup_push/decref contract as `method_call_impl`). Misses fall into `method_call_impl`, which refills the site at two points: the no-modifier tail (`has_mod = 0`) and the has-modifier branch (`has_mod = 1`, parking the site permanently on the slow path so hooks always run). Sites are filled only from blessed receivers — blessed-package pointers are interned so identity compare is sound; string-class static calls (transient buffer pointers) are never cached. `gen` is written last so a racing reader can't match a half-written entry. Codegen excludes `isa`/`can` (UNIVERSAL methods intercepted before lookup — their sites would never fill) and emits the `({ static ...; call; })` statement-expression wrapper for the zero-arg/no-cleanup shape that previously emitted a bare call.

#### Cross-.so Guarded Devirtualization (import_lib, 2026-06)

Local-class devirtualization (`known_types` tracked at `my $x = Pkg::new(...)` in CodeGenStmt, resolved against `all_func_names` at the method-call site in CodeGenExpr, blocked by `has_method_modifier`) now extends to `import_lib` classes. Moving parts:

- **`all_func_names` build (CodeGenStmt, after the local-function loop):** walks `$program->{"import_libs"}`; for each lib whose metadata has `modinfo:1`, adds method-shaped functions (orig name package-qualified, non-variadic, `param_count >= 1`) keyed by sanitized name → return type. Local names win on collision. Also fills `$cg->{"lib_modified_methods"}` from the lib's `mod:` lines; `has_method_modifier` (CodeGen.strada) checks that hash FIRST — blocking both devirt and accessor inlining for hook-bearing lib method names.
- **Metadata (gen_export_info, CodeGenStmt):** `func:` lines gained field 6 = original qualified name with `::` → `.` (parser pre-mangles package functions to `Pkg_method` at parse time, so the split is rebuilt from `$fn->{"package"}` + the new `$fn->{"bare_name"}` recorded at the prefix site in Parser.strada ~line 6430). New lines `modinfo:1` and deduped `mod:NAME` per `$program->{"method_modifiers"}` entry. Old consumers ignore unknown lines/fields; old producers lack `modinfo:1` so new consumers never devirtualize against them.
- **Fingerprint:** `export_meta_hash32` (Parser.strada, pure Strada — additive djb2 `h*33+c` mod 2^32, no bitwise ops so the frozen bootstrap compiles it; result fits a tagged int) hashes the metadata at compile time into `lib_info{"meta_hash"}`. MUST stay algorithm-identical to the runtime's `strada_export_meta_hash_cstr` (uint32 natural wrap), which the generated ensure() calls on the loaded .so's live metadata.
- **Generated code:** per lib, a `static int __import_lib_<lib>_devirt_ok` + `static void __import_lib_<lib>_ensure(void)` (dlopen + OOP inits + fingerprint compare — factored out of the wrappers, which previously each duplicated the lazy-load block). Wrapper bodies: `ensure();` then, for method-shaped functions, the fallback guard `if (!devirt_ok && strada_blessed_name_cstr(arg0)) return strada_method_call_ph(arg0, "name", strada_pack_args(n-1, ...), HASHu);` then the original lazy-dlsym direct call. `strada_pack_args` increfs (params stay borrowed); dispatch consumes the array; void returns decref the dispatch result.
- **CRITICAL — wrappers are `static`:** the .so defines the same-named global symbol; under `-rdynamic` the .so's own internal calls to it interpose to the host wrapper. Before devirt that was a silent extra hop (wrapper dlsym'd back into the .so); with the fallback it became infinite recursion (wrapper → dispatch → .so's registered method wrapper → interposed host wrapper → ...). `static` keeps host wrappers TU-local so .so-internal calls bind within the .so. Found by segfault in the swap test; do not regress.
- **Runtime additions** (`strada_runtime.c` + BOTH headers): `strada_export_meta_hash_cstr(const char*)`, `strada_blessed_name_cstr(StradaValue*)` (NULL/tagged-safe `SV_BLESSED` accessor, no allocation).
- **Semantics on swap:** matching fingerprint → direct calls (the .so's current symbols — same-ABI rebuilds keep full speed and new behavior). Mismatch → every method-shaped wrapper late-binds by name (new hooks fire, overrides honored, missing-symbol exits avoided for blessed invocants). Plain static calls with unblessed first args keep direct-call semantics (the pre-existing signature-coupling hazard, unchanged).
- **Tests:** `t/import_lib_devirt_test/run.sh` (+ `t/t_import_lib_devirt.sh` suite hook) — asserts the generated C shape (`strada_method_call_cs` absent for devirtualized names, present for `mod:`-gated ones), correctness, and the swap scenario including new-hook firing; `t/leak_tests/test_import_lib_devirt.strada` + companion lib (pre-built by the leak runner's per-test case).

#### Single-Lookup Hash Compound Assignment

`gen_hash_compound_assign` (CodeGen.strada) emits one `strada_hv_compound_ph/_sv/()` call for `+=`, `-=`, `*=`, `/=`, `.=` instead of the fetch_owned + store pair. The runtime side (`strada_runtime.c`, near `strada_hash_get_sv`): `hash_lvalue_slot()` finds the entry's value slot with one probe (mirroring `strada_hash_get_sv` including the dirty-index linear fallback); `hv_compound_combine()` computes old OP rhs (missing/undef old = int 0 when rhs is tagged, so fresh counters stay tagged); `.=` assigns `*slot = strada_concat_inplace(old, rhs)` (consumes old, appends in place when refcount==1). Missing keys insert once via `strada_hash_set_take_ph` reusing the computed hash. Tied/locked hashes route to `hv_compound_fallback()` (FETCH/compute/STORE). Note: `*=`/`/=` on hash elements currently don't parse (pre-existing parser limitation), so those branches are exercised only via future parser support.

#### Range Foreach Fast Path

The `NODE_FOREACH_STMT` handler in CodeGenStmt.strada checks `array_expr type == NODE_RANGE && expr_is_int_typed(start) && expr_is_int_typed(end)` and emits a native `for (int64_t __fr_i = start; __fr_i <= end; __fr_i++)` with the loop variable rebuilt per iteration via `strada_new_int` (borrowed semantics, same as the `array_get` path). Bounds go through `emit_int_operand` (which handles owned-temp cleanup). The loop variable is registered in `$cg->{"int_vars"}` for the body (saved/restored around the loop). Labels, `last`/`next`/`redo`, and per-iteration `local()` restore mirror the array path exactly.

#### `core::` to `core::` Namespace Normalization

**Purpose:** `core::` is an alias for `core::`, allowing users to write `core::exit()` instead of `core::exit()`. This is normalized at compile time in CodeGen.

**Implementation** -- two locations in CodeGen.strada:

1. **`needs_temp_cleanup()`** (CodeGen.strada line ~2003): Before checking function names for cleanup tracking:
   ```strada
   if (length($name) > 6 && substr($name, 0, 6) eq "core::") {
       $name = "core::" . substr($name, 6, length($name) - 6);
   }
   ```

2. **`gen_expression()` for `NODE_CALL`** (CodeGen.strada line ~3082): Before emitting the function call:
   ```strada
   if (length($name) > 6 && substr($name, 0, 6) eq "core::") {
       $name = "core::" . substr($name, 6, length($name) - 6);
       $expr->{"name"} = $name;   # Also updates the AST node
   }
   ```

After normalization, `core::exit(0)` becomes `core::exit(0)` and follows the same code generation path as all `core::` functions. The normalization is a simple string prefix replacement with no runtime cost.

### Automatic Function Prefixing (Parser)

When parsing functions inside a `package` block, the Parser auto-prefixes function names.

**In Parser.strada (`parse_function_def`):**

```strada
# After parsing function name, check if we should auto-prefix
if (length($pkg) > 0 && $pkg ne "main" && $fn_name ne "main" && $pkg_colon_idx < 0) {
    my str $prefix = $pkg . "_";
    my int $prefix_len = length($prefix);
    # Only prefix if function doesn't already have the prefix
    if (length($fn_name) < $prefix_len || substr($fn_name, 0, $prefix_len) ne $prefix) {
        # Only prefix simple names (no underscore = not already qualified)
        my int $underscore_idx = index($fn_name, "_");
        if ($underscore_idx < 0) {
            $fn->{"name"} = $pkg . "_" . $fn_name;
        }
    }
}
```

**Auto-prefix conditions (ALL must be true):**
- Package is set and not "main"
- Function name is not "main"
- Package name doesn't contain `::` (module-style packages skip prefix)
- Function name doesn't already have the package prefix
- Function name doesn't contain `_` (already qualified functions skip prefix)
- Function is not `extern` (checked separately)

**Semantic Resolution (`Package::func` to `Package_func`):**

In Semantic.strada, function calls using `Package::func()` syntax are resolved:

```strada
# Try to resolve Package::func to Package_func
my int $colon_idx = index($name, "::");
if ($colon_idx >= 0) {
    my str $prefix = substr($name, 0, $colon_idx);
    my str $suffix = substr($name, $colon_idx + 2, -1);
    my str $underscore_name = $prefix . "_" . $suffix;
    # Try underscore version first, fall back to original
}
```

This allows calling `Greeter::new()` which resolves to `Greeter_new()`.

### Current Package Function Calls (`::func()` syntax)

The parser supports calling functions in the current package without repeating the package name using `::func()`, `.::func()`, or `__PACKAGE__::func()` syntax. All three are resolved at **compile time**.

**Parser Implementation (in `parse_primary`):**

The parser tracks the current package in `$parser->{"current_package"}`, which is set when `parse_package()` is called.

```strada
# In parser_new():
$parser{"current_package"} = "";

# In parse_package():
$parser->{"current_package"} = $pkg_name;
```

**Handling `::func()` (leading double colon):**
```strada
if ($type eq "DOUBLE_COLON") {
    parser_advance($parser);
    my str $func_name = $func_tok->{"value"};

    # Build qualified name using compile-time package
    my str $pkg = $parser->{"current_package"};
    if (length($pkg) > 0) {
        # Convert Package::Sub to Package_Sub
        # (loop replaces :: with _)
        $func_name = $sanitized_pkg . "_" . $func_name;
    }

    # Parse as normal function call
    my scalar $call = ast_new_call($func_name);
    # ... parse arguments ...
}
```

**Handling `.::func()` (dot + double colon):**
```strada
if ($type eq "DOT") {
    my scalar $peek_tok = parser_peek($parser);
    my str $peek_type = $peek_tok->{"type"};
    if ($peek_type eq "DOUBLE_COLON") {
        # Same logic as :: handling
    }
}
```

**Handling `__PACKAGE__::func()`:**
```strada
if ($type eq "DUNDER_PACKAGE") {
    parser_advance($parser);
    if (parser_check($parser, "DOUBLE_COLON")) {
        # Compile-time resolution using current_package
    } else {
        # Plain __PACKAGE__ - runtime value
        return ast_new_dunder_package();
    }
}
```

**Key difference from `__PACKAGE__` alone:**
- `__PACKAGE__` without `::` creates a `NODE_DUNDER_PACKAGE` that evaluates at runtime
- `__PACKAGE__::func()` resolves the function name at compile time (no NODE_DUNDER_PACKAGE created)

### Variable Name Parsing (parse_var_name)

The `parse_var_name()` function in Parser.strada handles variable name parsing after a sigil. As of 2026-01-17, it allows type keywords to be used as variable names.

**In Parser.strada:**
```strada
func parse_var_name(scalar $parser) str {
    my scalar $tok = parser_current($parser);
    my str $type_str = $tok->{"type"};

    # Allow type keywords as variable names after sigils
    if ($type_str eq "TYPE_INT") {
        parser_advance($parser);
        return "int";
    }
    if ($type_str eq "TYPE_STR") {
        parser_advance($parser);
        return "str";
    }
    # Note: TYPE_PTR has been removed from the language
    if ($type_str eq "TYPE_HASH") {
        parser_advance($parser);
        return "hash";
    }
    # ... other type keywords ...

    # Otherwise expect identifier
    parser_expect($parser, "IDENT");
    return $tok->{"value"};
}
```

**Usage locations:**
- `parse_var_decl()` - Variable declarations: `my int $int`
- `parse_param()` - Function parameters: `func foo(hash $hash)`
- Works with any type keyword as variable name after sigil

**Why this works:**
- The sigil (`$`, `@`, `%`) has already been consumed
- Context is clear: we're expecting a variable name
- Type keywords after sigils can only be variable names

### Lexer: Transliteration and Substitution Extensions

#### `tr///` and `y///` Lexing

The lexer recognizes `tr` and `y` as transliteration operators in the `expect_regex` context. When the lexer encounters these keywords followed by a delimiter, it calls `lex_read_tr_literal()` instead of `lex_read_regex()`.

**`lex_read_tr_literal()`** parses the three-part structure:
1. Opening delimiter (e.g., `/`)
2. Search character list (up to closing delimiter)
3. Replacement character list (up to closing delimiter)
4. Optional flags (`c`, `d`, `s`, `r`)

The result is a `TR_LITERAL` token whose value encodes all three parts (search list, replacement list, flags) in a structured format that the parser can decompose.

**Detection in expect_regex block:** When the lexer is in a context where a regex or transliteration is expected (after `=~`), it checks whether the identifier is `tr` or `y`. If so, it switches to `lex_read_tr_literal()` instead of the normal regex lexing path.

#### `/e` Flag in Substitution

The `lex_read_subst_literal()` function now recognizes the `/e` flag in substitution patterns (`s/pattern/replacement/e`). When `/e` is present:
- The replacement part is stored as a raw expression string rather than a literal replacement string
- The parser later parses this expression string to build an AST subtree for evaluation
- The flags string includes `"e"` which the code generator checks to determine the /e code generation path

#### `local` Keyword Token

The `local` keyword is recognized in `lex_keyword_or_ident()` and produces a `LOCAL` token. Like other context-sensitive keywords (`has`, `before`, etc.), `local` can still be used as a variable name after a sigil (e.g., `$local`) via `parse_var_name()`.

### C Interop via `__C__` Blocks

The preferred way to interface with C code is using `__C__` blocks. The old `extern "C"` feature has been deprecated in favor of this more flexible approach.

**Two types of `__C__` blocks:**

1. **Top-level** (at program scope) - For includes, globals, helper functions
2. **Statement-level** (inside functions) - For inline C code with access to Strada variables

**Top-level `__C__` block example:**
```strada
# Appears at file scope, included verbatim at top of generated C
__C__ {
    #include <openssl/ssl.h>
    static SSL_CTX *g_ctx = NULL;
}
```

**Statement-level `__C__` block example:**
```strada
func my_func(int $handle, str $data) int {
    # Strada code...

    # Inline C code with access to Strada variables
    __C__ {
        char *data_c = strada_to_str(data);
        int h = (int)strada_to_int(handle);
        int result = some_c_function(h, data_c);
        free(data_c);
        return strada_new_int(result);
    }
}
```

**Key patterns:**
- Store C pointers in Strada `int` variables: `(int64_t)(intptr_t)ptr`
- Retrieve C pointers from Strada: `(Type*)(intptr_t)strada_to_int(val)`
- Convert strings: `strada_to_str()` (must free), `strada_new_str()`
- Return values: `strada_new_int()`, `strada_new_str()`, `&strada_undef`

---

## Memory Management

### Reference Counting

Heap-allocated StradaValue objects use reference counting. Tagged integers (bit 0 = 1) are immortal and skip refcounting entirely:

```c
void strada_incref(StradaValue *sv) {
    if (sv && !STRADA_IS_TAGGED_INT(sv)) sv->refcount++;
}

void strada_decref(StradaValue *sv) {
    if (!sv || STRADA_IS_TAGGED_INT(sv)) return;
    if (--sv->refcount <= 0) {
        strada_free_value(sv);
    }
}
```

Since most integer operations use tagged encoding, this means integer-heavy code has zero refcounting overhead.

### Ownership Rules

1. **Constructors** (`strada_new_*`) return values with refcount = 1
2. **Storing in containers** should incref the value
3. **Removing from containers** should decref the value
4. **Passing to functions** usually doesn't change refcount (borrowed)
5. **Returning values** may need incref to keep alive

### Weak References

Weak references solve circular reference cycles. A weak reference does not prevent its target from being freed.

**Runtime implementation** (`runtime/strada_runtime.c`):
- `is_weak` flag on `StradaValue` struct marks weak references
- Global weak registry maps target pointers to lists of weak refs
- `strada_weaken(StradaValue **ref_ptr)` - Creates a weak wrapper if the value is shared (refcount > 1), or converts in-place if exclusive
- `strada_weaken_hv_entry(StradaHash *hv, const char *key)` - Weakens a hash entry value in-place
- `strada_isweak(StradaValue *ref)` - Returns 1 if `is_weak` flag is set
- `strada_weak_registry_remove_target(StradaValue *target)` - Called in `strada_free_value()` to nullify all weak refs pointing to a dying target
- When a weak ref's target is freed, `rv` is set to NULL; deref functions return undef for NULL `rv`

**Compiler support** (`compiler/CodeGen.strada`):
- `core::weaken($var)` emits `strada_weaken(&var)` (passes pointer to variable)
- `core::weaken($ref->{"key"})` emits `strada_weaken_hv_entry(deref_hash(ref), key)` (weakens hash entry in-place)
- `core::isweak($ref)` emits `strada_new_int(strada_isweak(ref))`
- `core::isweak` is registered in `needs_temp_cleanup()` (returns owned value)
- `core::weaken` returns `strada_undef_static()` (void-like)

**Semantic checker** (`compiler/Semantic.strada`):
- `sys::weaken` and `sys::isweak` registered as builtins

### Struct Memory

Structs use `malloc()` directly, not reference counting:

```c
StructName *s = malloc(sizeof(StructName));
// ... use struct ...
free(s);
```

### `needs_temp_cleanup()` — Expressions That Return Owned Values

The `needs_temp_cleanup($cg, $expr)` function in CodeGen.strada determines whether an expression produces an owned `StradaValue*` that must be decremented after use. The following node types and function names return 1 (need cleanup):

- **Node types**: `NODE_CALL` (most calls), `NODE_ANON_ARRAY`, `NODE_ANON_HASH`, `NODE_TERNARY`, `NODE_METHOD_CALL`, `NODE_DYN_METHOD_CALL`, `NODE_CLOSURE_CALL`, `NODE_ANON_FUNC`, `NODE_REGEX_MATCH`, `NODE_CAPTURE_VAR`, `NODE_TR`, `NODE_READLINE`, `NODE_DESTRUCTURE`, `NODE_ARRAY_SLICE`, `NODE_HASH_SLICE`, `NODE_OUR_DECL` (reads)
- **Built-in functions returning owned values**: `match`, `replace`, `replace_all`, `captures`, `named_captures`, `substr`, `join`, `split`, `chr`, `sprintf`, `uc`, `lc`, `ucfirst`, `lcfirst`, `trim`, `ltrim`, `rtrim`, `chomp`, `chop`, `typeof`, `ref`, `reftype`, `reverse`, `dumper_str`, `core::random_bytes`, `core::random_bytes_hex`, all `math::` functions, `bytes`, `hash_new`, `core::file_exists`, `core::open`, `core::slurp`, `slurp`, `core::mkdir`, `core::seek`, `core::waitpid`, `core::getaddrinfo`, `core::socket_fd`, `core::socket_send`, `core::socket_set_nonblocking`, `core::udp_bind`, `core::udp_sendto`, `core::array_default_capacity`, `core::hash_default_capacity`, `core::cstruct_new`, `core::cstruct_get_int`, `core::cstruct_get_double`, `core::cstruct_get_string`, `core::isweak`
- **Node types that do NOT need cleanup**: `NODE_VARIABLE` (unless it is an `our` variable), `NODE_DEREF_ARRAY`, `NODE_SUBSCRIPT`, `NODE_INT_LITERAL`, `NODE_NUM_LITERAL`, `NODE_STR_LITERAL` (handled inline). **Note**: `NODE_HASH_ACCESS` and `NODE_DEREF_HASH` DO need cleanup (they fetch via `strada_hv_fetch_owned*` — owned refs), with one exception: a hash-access node carrying a non-empty `cse_temp` field returns 0 — it emits a borrowed condition-CSE temp whose single decref is owned by the `emit_condition_cse` wrapper.

When adding new expression types or built-in functions that return owned `StradaValue*`, they must be added to `needs_temp_cleanup()` to prevent memory leaks. Function names in the list use `sys::` prefix since `core::` is normalized to `sys::` before the check.

#### Condition-level hash-fetch CSE (`emit_condition_cse`)

Statement conditions (if/elsif/while/until/do-while/for — NOT expression-position ternaries) go through `emit_condition_cse` in CodeGen.strada. When the condition tree is **pure** (`cond_cse_pure`: only variables, literals, binary/unary ops, ternaries, and hash accesses — any call/assignment/increment/regex node disqualifies the whole tree) and the program never calls `tie` (Semantic sets `uses_tie` on the program node; copied to `$cg->{"uses_tie"}` in gen_program), duplicated literal-key fetches on simple hash variables are hoisted:

```c
if ((({ StradaValue *__cse_0 = strada_hv_fetch_owned_ph(h, "k", 177680u);
        int __cse_c = (strada_to_num(__cse_0) > 0 && strada_to_num(__cse_0) < 100);
        strada_decref(__cse_0); __cse_c; }))) ...
```

Mechanism: `cond_cse_collect` gathers (hash-var, literal-key) sites into `$cg->{"cse_ids"}`/`{"cse_nodes"}`; duplicated sites get tagged with `$node->{"cse_temp"} = "__cse_N"`. The `NODE_HASH_ACCESS` generator in CodeGenExpr.strada emits the temp name for tagged nodes, and `needs_temp_cleanup` returns 0 for them (borrowed). Tags are cleared after the condition is emitted. The hoisted fetch executes even when `&&` short-circuits — fine for untied hashes (reads are side-effect-free and don't autovivify), which is exactly why `tie` anywhere disables the pass.

#### Name resolution MUST match the call emitter (fixed in `b5f48978`)

For **user-defined function calls** (`NODE_CALL`), `needs_temp_cleanup` looks up `$cg->{"functions"}->{sanitize_name($name)}` and returns 1 if the function is registered with a non-void return type. **This lookup must use the same name-mangling and fallback rules as the call-emission path in `gen_call` (around line 12518).** The emitter tries the unqualified `c_name` first and falls back to `<c_pkg>_<c_name>` (the package-mangled name) when the call is unqualified inside a non-`main` package — because that's how the function got *registered* in `$cg->{"functions"}` during the collection pass.

Before the fix, `needs_temp_cleanup` only tried the unqualified lookup. For a same-package unqualified call like `_tok(...)` inside `package Perla::Lexer`:

- The emitter found the function as `Perla_Lexer__tok` via fallback → emitted a working call.
- `needs_temp_cleanup` looked up `_tok` → not found → returned 0 → caller (e.g. `push(@arr, _tok(...))`) skipped the `__push_v / strada_decref` wrapper → every owned return **leaked**.

This was undetectable by pure-Strada leak tests in `package main` (the unqualified lookup matched the registered name) or with fully-namespaced calls (the unqualified-name fallback never triggers). It was only caught when AddressSanitizer was run against perla, after months of valgrind reports being treated as "constant overhead".

**The rule going forward:** every codegen predicate that consults `$cg->{"functions"}` for a function-call AST node — whether for cleanup, incref, return-type classification, or anything else — **must mirror the emitter's name-resolution logic**, or that logic must be factored into a shared helper. **Test in a non-`main` package**, not just in `main`. If the predicate would naturally call `gen_call`-style fallback code, just call it; don't reimplement a stripped-down version.

### Common Leaks and How to Prevent Them

1. **Circular references** (not detected by reference counting)
2. **Forgetting to decref removed array/hash elements**
3. **Not freeing intermediate strings from `strada_to_str()`** — always capture and `free()` the returned `char*`
4. **Nested `strada_to_str()` in inline codegen** — never pass `strada_to_str(expr)` directly as an argument; capture to a variable, use it, then free
5. **Refcount leaks with `strada_hash_set()`/`strada_array_push()`** — when creating a new value and immediately storing it, use `_take` variants (`strada_hash_set_take`, `strada_array_push_take`) which skip the incref
6. **Concatenation chain intermediates** — nested `strada_concat_sv(a, strada_concat_sv(b, c))` leaks intermediates; use sequential concat with `strada_concat_inplace()` which frees its first arg
7. **Temp StradaValue* in `=~`/`!~` and `cmp`/`<=>` operators** — when using `strada_to_str()` on expression results, the source StradaValue* must be captured and decreffed if it's a temp

---

## Debugging Techniques

### Print AST Node Type

```strada
my int $type = $node->{"type"};
say("Node type: " . ast_type_name($type));
```

### Examine Generated C

```bash
./stradac input.strada output.c
cat output.c | less
```

### Add Debug Output in CodeGen

```strada
emit($cg, "fprintf(stderr, \"DEBUG: reached line X\\n\");\n");
```

### Compile with Debug Symbols

```bash
./strada -g program.strada
gdb ./program
```

### Use Valgrind

```bash
gcc -g -o prog prog.c runtime/strada_runtime.c -Iruntime -ldl -lm
valgrind --leak-check=full ./prog
```

### Check Refcounts

```strada
my int $rc = refcount($val);
say("Refcount: " . $rc);
```

### Dumper for Debugging

```strada
dumper($complex_structure);
# or
my str $s = dumper_str($val);
say($s);
```

---

## Test Suite

### Test Harness Location

```
t/
├── run_tests.sh   # Main test harness (executable)
├── t_compile.sh   # Compile-only tests (servers)
├── t_core.sh      # Core language feature tests
└── t_special.sh   # Edge cases and special features
```

### Running Tests

```bash
# All tests
./t/run_tests.sh

# With options
./t/run_tests.sh -v         # Verbose (show output on failure)
./t/run_tests.sh -t         # TAP format for CI/CD
./t/run_tests.sh -c         # Compile-only tests
./t/run_tests.sh pattern    # Filter tests by pattern

# Via Makefile
make test-suite             # All tests
make test-suite V=1         # Verbose
make test-suite TAP=1       # TAP format
make test-suite FILTER=str  # Filter by pattern
```

### Test Function Reference

| Function | Usage | Description |
|----------|-------|-------------|
| `test_compile` | `test_compile "$src" "name" "desc"` | Verify compiles to C and links |
| `test_run` | `test_run "$src" "name" "desc" [timeout]` | Compile, run, expect exit 0 |
| `test_output` | `test_output "$src" "name" "expected" "desc"` | Verify exact output match |
| `test_output_contains` | `test_output_contains "$src" "name" "pattern" "desc"` | Verify output contains pattern |
| `test_exit_code` | `test_exit_code "$src" "name" code "desc"` | Verify specific exit code |
| `test_skip` | `test_skip "name" "reason"` | Skip a test with reason |

### Adding a New Test

1. Choose appropriate test file (`t_core.sh` for most, `t_special.sh` for edge cases)
2. Add test using one of the functions above:

```bash
# In t/t_core.sh

# Test runs successfully
test_run "$EXAMPLES_DIR/my_feature.strada" "my_feature" "My feature works"

# Test with expected output
test_output "$EXAMPLES_DIR/math.strada" "math" "42" "Math computes 42"

# Test contains pattern
test_output_contains "$EXAMPLES_DIR/hello.strada" "hello" "Hello" "Says hello"

# Test specific exit code
test_exit_code "$EXAMPLES_DIR/error.strada" "error" 1 "Exits with 1"
```

### TAP Format

The test harness supports [TAP (Test Anything Protocol)](https://testanything.org/) for CI/CD integration:

```bash
./t/run_tests.sh -t
# Output:
# TAP version 13
# ok 1 - compile: Web server
# ok 2 - run: Basic arithmetic
# not ok 3 - output: Math test
#   # Output mismatch
# 1..82
```

### Current Status

```
Tests: 82  Passed: 82  Failed: 0  Skipped: 0
```

- 82 comprehensive tests covering all major features
- 4 compile-only tests (server programs that run indefinitely)
- Full coverage of strings, references, OOP, structs, control flow, etc.

---

## Performance Optimizations

### Zero-Copy keys()/each() + StradaString COW Contract (2026-06-10)

`strada_hash_keys`, `strada_hash_each`, and `sort %h` flattening hand out key SVs that **share** the hash key's `StradaString` (`strada_new_str_share_ss`: ss_incref + point `value.pv` at the same data) instead of copying the bytes. This makes every `keys %h` / `each %h` / `sort keys %h` O(1) per key instead of one strdup per key, and makes binary keys round-trip byte-accurately (length from `ss->len`, not strlen).

**The COW contract this creates:** a `StradaString` reachable from more than one place (`ss->refcount > 1`) must NEVER be mutated or realloc'd in place — it may be a live hash key. Any runtime function that writes string bytes through an existing SS (instead of swapping in a fresh one via `ss_alloc_pv`/`ss_new_uninit`) must check `SS_FROM_PV(pv)->refcount == 1` first and clone when shared. Exactly three functions write in place, and all three carry the guard:

- `strada_concat_inplace` (fast path now requires SV refcount == 1 **and** SS refcount == 1)
- `strada_concat_inplace_cstr` (same)
- `strada_vec_set` (clones the SS before writing when shared)

Everything else (`tr///`, `strada_subst_sv`, `overwrite_in_place`, `set_byte`, chomp/chop/uc/lc) already builds a fresh SS and swaps `value.pv`, which is safe under sharing. If you add a new in-place string mutator, add the SS-refcount guard or you will corrupt hash keys in a way that only shows up after a `keys()`/`each()` call.

### Tagged Integer Pointer Encoding (2026-03-02)

Integers are now encoded directly in `StradaValue*` pointers using bit tagging, eliminating heap allocation for all integer operations:

```c
// Encoding: (value << 1) | 1
// Bit 0 = 1 → tagged integer (not a real pointer)
// Bit 0 = 0 → heap-allocated StradaValue*
StradaValue* strada_new_int(int64_t val) {
    return STRADA_MAKE_TAGGED_INT(val);  // No malloc!
}
```

**Impact:** Zero heap allocation for integer operations. `strada_incref()`/`strada_decref()` are no-ops for tagged ints. Combined with the concat key skip-intern optimization below.

**Guard requirement for `__C__` blocks and runtime code:** Any code that accesses `sv->type` must check `STRADA_IS_TAGGED_INT(sv)` first. Forgetting this guard causes segfaults because tagged int pointers are not valid memory addresses.

### Concat Key Skip-Intern Optimization (2026-03-02)

Hash key storage now uses `strdup()` instead of `strada_intern_str()` for keys set via `strada_hash_set_with_hash()`. Concatenated keys like `"item_" . $i` are typically unique and never looked up by the same string again, so interning wastes time building a table that provides no dedup benefit.

`strada_intern_release()` handles both interned and non-interned keys transparently, so existing code is unaffected.

**Benchmark results (bench_array_hash: 1M hash key create/access):**
| Language | Time |
|----------|------|
| Strada   | 0.26s |
| Perl     | 0.88s |
| Python   | 0.93s |

Strada is 3.4-3.5x faster than Python and Perl for hash-heavy integer workloads.

### Compiler Optimizations (2026-01-10)

Major performance improvements to the self-hosting compiler:

### Lexer Optimizations (33x faster)

The lexer was rewritten to use integer character codes instead of string comparisons:

```strada
# SLOW: String comparison
if (substr($src, $pos, 1) eq " ") { ... }

# FAST: Integer code comparison
my int $c = char_at($src, $pos);
if ($c == 32) { ... }  # 32 = space
```

Key integer codes used: `10` (newline), `32` (space), `34` (quote), `35` (#), etc.

### char_at() Function (7x faster)

New runtime function for fast single-character access:
```strada
my int $code = char_at($string, $position);  # Returns ASCII code
```
Avoids allocation overhead of `substr($s, $i, 1)` followed by `ord()`.

### StringBuilder for Code Generation (165x faster)

Replaced O(n) string concatenation with O(1) amortized StringBuilder:
```strada
my scalar $sb = sb_new();
sb_append($sb, "line 1\n");
sb_append($sb, "line 2\n");
my str $result = sb_to_string($sb);
sb_free($sb);
```

### In-Memory Code Generation

Code is generated in memory strings rather than written to temp files. Eliminates file I/O overhead.

### Memory Leak Fix (~100x reduction)

Fixed a memory leak in string concatenation that was causing excessive memory usage during compilation.

### Line-Level Profiling (`--full-profile`)

NYTProf-style line-level profiling. When the `--full-profile` flag is set:

1. **CodeGen** inserts `strada_full_profile_tick(file, line)` before each statement
2. The flag implies `-g` so line info is available
3. On program exit, a binary `strada-prof.out` file is written
4. Report tools: `strada-proftext` (text) and `strada-profhtml` (HTML with heat-colored source)
5. Programmatic API: `core::full_profile_start("file.prof")` / `core::full_profile_stop()` for targeted profiling

Difference from `-p`: `-p` is lightweight function-level profiling (call counts + timing to stderr). `--full-profile` is comprehensive line-level profiling with binary data output.

---

## External Libraries

All external libraries now use the `extern "C"` pattern instead of FFI. The C functions use raw types (`int64_t`, `char*`) and the Strada wrappers handle type conversion using `c::` helpers.

### USB Library (`lib/usb/`)

Userspace USB device access via libusb-1.0. Converted to `extern "C"` pattern in 2026-01-17.

**Files:**
- `lib/usb/strada_usb.c` - C library with libusb bindings (~800 lines including raw functions)
- `lib/usb/Makefile` - Build with `make` (links `-lusb-1.0`)
- `lib/usb.strada` - Strada interface using extern "C" (not FFI)
- `lib/usb/example.strada` - Example: list, info, control transfer demo

**C Raw Function Pattern (strada_usb.c):**
```c
// Raw functions use basic C types
int strada_usb_init_raw(void);
void strada_usb_exit_raw(void);
int strada_usb_refresh_device_list_raw(void);
int strada_usb_device_count_raw(void);
int strada_usb_device_vid_raw(int idx);
void* strada_usb_open_raw(int vid, int pid);
void strada_usb_close_raw(void* handle);
char* strada_usb_bulk_read_raw(void* handle, int endpoint, int max_len, int timeout);
int strada_usb_last_transfer_len_raw(void);  // For out parameters

// Legacy StradaValue* functions still exist for compatibility
StradaValue* strada_usb_get_device_list(void);
```

**Key patterns:**
- Iterator pattern for device enumeration (refresh, count, get-by-index)
- Static variable for out parameters (`last_transfer_len`)
- Raw functions return allocated buffers that caller must free

**Transfer semantics:**
- Endpoint bit 7 determines direction: `0x01-0x0F` = OUT (write), `0x81-0x8F` = IN (read)
- For OUT: `data_sv` is the string to send, returns bytes transferred (int)
- For IN: `data_sv` is the max length to read, returns data (string)

**Device list hash fields:** `vid`, `pid`, `bus`, `address`, `class`, `subclass`, `protocol`, `vidpid` (formatted string)

**Build requirement:** `apt install libusb-1.0-0-dev` (Ubuntu/Debian)

### SSL Library (`lib/ssl/`)

SSL/TLS socket support via OpenSSL. Uses `extern "C"` pattern. See `lib/ssl/strada_ssl.c` and `lib/ssl.strada`.

**Build:** `cd lib/ssl && make` (requires `libssl-dev`)

**Note:** `ssl::version()` was renamed to `ssl::get_version()` to avoid conflict with the `version` keyword.

### Perl5 Library (`lib/perl5/`)

Embed Perl 5 interpreter. Uses `extern "C"` pattern. See `lib/perl5/strada_perl5.c` and `lib/perl5.strada`.

**Build:** `cd lib/perl5 && make` (requires `libperl-dev`)

### DBI Library (`lib/dbi/`)

Database connectivity. Uses `extern "C"` pattern. See `lib/dbi/strada_dbi.c` and `lib/dbi.strada`.

**Build:** `cd lib/dbi && make`

### Crypt Library (`lib/crypt/`)

Password hashing. Uses `extern "C"` pattern. See `lib/crypt/strada_crypt.c` and `lib/crypt.strada`.

**Build:** `cd lib/crypt && make`

---

## Key Lessons Learned

### 1. Built-in Functions Need Semantic.strada Registration

Adding a function to CodeGen.strada alone is NOT enough. You must also add it to `get_builtins()` in Semantic.strada:

```strada
$b{"my_function"} = 1;
```

Otherwise you get "undefined function" errors at compile time.

### 2. Function References Use CPOINTER, Not REF

When handling `\&func_name`:
- Parser.strada: Check if target is `NODE_FUNC_REF` and set ref_type to `"&"`
- CodeGen.strada: Use `strada_cpointer_new((void*)func)` for ref_type `"&"`

Other reference types (`\$x`, `\@arr`) use `strada_new_ref()`.

### 3. Struct Variables Need Tracking

For struct function pointer calls to work:
- Track struct-typed variables in `$cg->{"struct_vars"}`
- Track struct definitions in `$cg->{"struct_defs"}`
- Check these when generating METHOD_CALL to distinguish from OOP

### 4. Bootstrap Compiler Is Frozen

Never modify `bootstrap/` unless absolutely necessary. All development goes in `compiler/*.strada`.

### 5. parse_block() Consumes Its Own Braces

Don't call `parser_expect("LBRACE")` before `parse_block()` - it handles that itself.

### 6. Foreach Needs Unique Variable Names

Use a counter (`foreach_counter`) to generate unique temporary variable names for each foreach loop to avoid conflicts in nested loops.

### 7. Runtime Functions Should Not Interpret User Strings as Format Strings

The `strada_die()` and `strada_warn()` runtime functions originally used `vsnprintf`/`vfprintf` with user strings as format strings. This caused bugs when user messages contained `%` (e.g., hash variable names like `%food` were displayed as `0.000000ood` because `%f` was interpreted as float format).

**Fix:** Check if format is `"%s"` (new codegen) and extract actual message from varargs, otherwise treat format as literal string (old codegen). This maintains compatibility with both old and new generated code.

### 8. Compiler Warnings Are Opt-In

Compiler warnings (like unused variable warnings) are opt-in via the `-w/--warnings` flag. This is intentional to avoid noisy build output by default.

**Key points:**
- Semantic analyzer tracks variable usage in scopes
- `scope_check_unused()` reports unused variables
- Only reports when `show_warnings` parameter is true
- Pass `-w` to `strada` or `stradac` to enable warnings

### 9. Semantic Analyzer Handles AST Nodes for Variable Usage

For unused variable warnings to work correctly, the semantic analyzer must handle all AST nodes that can use or declare variables:

- **NODE_TRY_CATCH**: Analyze try block, create new scope for catch block, declare catch variable
- **NODE_CLOSURE_CALL**: Analyze the closure expression and all arguments

If an AST node type is not handled, variables used within it won't be marked as used, leading to false "unused variable" warnings.

### 10. Codegen Predicates Must Mirror the Call Emitter's Name Resolution

Any predicate that maps a function-call AST node to a behavior — `needs_temp_cleanup`, `return_needs_incref`, return-type classification, etc. — and consults `$cg->{"functions"}` MUST use the same name-mangling and fallback rules as `gen_call`. The emitter tries the unqualified `sanitize_name($name)` first and falls back to `<c_pkg>_<c_name>` for unqualified calls inside non-`main` packages (because that's how the collection pass registers them in `$cg->{"functions"}`).

A predicate that omits the fallback works in `package main` (no mangling) and for fully-namespaced calls (fallback never triggers) but **silently returns the wrong answer for every same-package unqualified call in a non-`main` package**. The call still emits correctly (the emitter's own fallback finds the function), but the predicate-driven cleanup wrapper is skipped — every owned return leaks.

Real-world cost (fixed in `b5f48978`): `needs_temp_cleanup` lacked the fallback, causing each `_tok(...)` call inside `package Perla::Lexer` to bypass the `__push_v / strada_decref` wrapper in `push(@arr, _tok(...))`. Empty Perl program: 14 leaked blocks; 100-statement program: 7,254 leaked blocks; after fix: 0.

The whole class went undetected for a long time because all Strada-language leak tests live in `package main` (where unqualified == registered name) and pure-Strada test cases the obvious bug-hunter writes also tend to live in `package main`. **Test predicates in a non-`main` package, with an unqualified same-package call, or the test isn't exercising the path.**

When adding any new codegen predicate that classifies a function call, either:

1. **Call `gen_call`'s name resolver directly** (factor it into a shared helper if it isn't already), or
2. **Include the exact same fallback inline** — match the call emitter's logic, don't reimplement a "simpler" version.

---

## Cycle Collector & Request Arena (runtime + codegen)

Both are compile-time features in `runtime/strada_runtime.c`, gated by `STRADA_CYCLE_GC` / `STRADA_ARENA` (default on via `./configure`, wired into the runtime compile in `Makefile` and the `strada` wrapper's `build_runtime()`). Both are confined to the runtime — they do **not** change the `StradaValue` struct, so user translation units need no define and the ABI is stable whether or not the features are compiled in.

**Cycle collector** — Bacon–Rajan synchronous trial deletion:
- Colour/buffered state lives in a side table keyed by pointer (`cc_tab`), not in `StradaValue`.
- The only hot-path hook is in the out-of-line `strada_decref` survive branch (`new_rc > 0`): `cc_possible_root(sv)` buffers `ARRAY`/`HASH`/`REF` candidates and **pins** them (raw incref) so they can't be freed underneath the collector. Drop-to-zero is unchanged.
- `cc_collect()` runs MarkGray → Scan → CollectRoots; `cc_each_child` enumerates strong children (filtered by `cc_collectable`, which excludes tagged ints, immortals, and arena-owned values). `cc_free_shell` frees a white node's own storage without decref'ing children (those are reclaimed by recursion). `Scan` discounts the pin via `buffered ? 1 : 0`.
- Triggers: candidate threshold (`cc_threshold`, default 10000) and an `atexit` final sweep.

**Request arena**:
- `arena_alloc_sv` bump-allocates `StradaValue` structs into iterable `ArenaBlock`s; only the struct is bumped, backbones stay malloc'd.
- `ARENA_OWNS(sv)` (range check over the arena block list) makes `strada_free_value` and the collector no-op on arena values. `strada_arena_end` walks the blocks and calls `arena_release_backbone` per SV (frees backbone + resources, no child decref, no `DESTROY`), then frees the blocks.

**Codegen** (`compiler/CodeGen.strada`): `core::gc_collect/gc_enable/gc_disable/gc_threshold/gc_collections/gc_freed` and `core::arena_begin/arena_end/arena_active` are dispatched in `gen_expression`'s `sys::`-normalized builtin chain (alongside `sys::full_profile_*`). Void ones emit `({ strada_...(); (StradaValue*)NULL; })`; the int-returning stat functions emit `strada_new_int(...)` and are listed in `needs_temp_cleanup()`. The runtime provides no-op stubs for all of these when a feature is configured out, so generated code always links.

---

*Last updated: 2026-02-23* (added NODE_DYN_METHOD_CALL for $obj->$method() dynamic dispatch)

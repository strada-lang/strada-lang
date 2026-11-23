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
| 31 | `NODE_FUNC_REF` | Function name inside `\&f` (always wrapped in a `NODE_REF`; a bare `&f` is a call) | `name` |
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

User-facing reference: `docs/BUILTIN_FUNCTIONS.md` (see its "Source of truth"
note — codegen in `CodeGenBuiltins.strada` is authoritative, not the
`get_builtins()` fast-path list). Condensed cheat-sheet below:

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

**Shared string/regex helpers (2026-11, audit M2g)** — both backends call these, so keep them the single source of truth:
- `strada_pcre2_compile_perl(pat, len, opts, &err, &off)`: `\x{NNNN}` byte-mode expansion (grouped `(?:\xHH\xHH)`) + Perl property names (`\p{^X}` -> `\P{X}`, escaped `\\p` left alone), then `pcre2_compile`. The runtime regex cache and the VM's `regex_cache_get` both compile through it.
- `strada_substr_span(L, off, len, &start, &count)`: Perl's substr range rule; used by `strada_substr`, `strada_substr_replace` (dies "substr outside of string") and the VM's `OP_SUBSTR`.
- `strada_substr_assign_sv(&slot, off, len, repl_sv)`: lvalue substr for generated code (codegen passes the stringified `__sa_rsv`). The VM compiles 4-arg / lvalue substr via `compile_substr_replace` + `BUILTIN_SUBSTR_REPL` (returns `[new, removed]`), reusing `compile_in_place_str_op` (kind 2).
- `tr_decode_and_expand_set_ex`: escape-aware tr list parser used by both the byte and the UTF-8 tr paths.
- `strada_chomp_sv` / `strada_chop_sv`: binary-safe, flag-preserving; `gen_chomp_chop` emits them.
- `strada_case_full(cp, upper, title, out)`: multi-character case mappings (ß, İ) for uc/lc/ucfirst.
- Vendored PCRE2 patch: `vendor/pcre2/src/pcre2_substitute.c` `\u`/`\l` keep the active `\L`/`\U` mode (Perl). The Makefile rebuilds `libpcre2-8.a` when vendored sources change.

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

int strada_array_assign_if_tied(StradaValue *target, StradaValue *newval);
int strada_hash_assign_if_tied(StradaValue *target, StradaValue *newval);
// @a = LIST / %h = LIST: when target is tied, CLEAR + STORE each element/pair,
// consume newval, return 1 (tie kept); else 0 and the caller swaps the container.
// Codegen: emit_container_replace (CodeGenExpr), only when uses_tie.

void strada_av_push_list_sv(StradaValue *sv, StradaValue *list, int at_front);
// push/unshift LIST: one PUSH/UNSHIFT call on a tied target, else append/prepend.
```

Tied array FETCH/STORE/EXISTS/DELETE offset a negative index by FETCHSIZE
(`tied_av_norm_idx`); still negative -> undef / "Modification of
non-creatable array value attempted". The VM mirrors list assignment with
`BUILTIN_TIE_ASSIGN` (vm_compiler emits LOAD_LOCAL target, RHS, builtin, STORE_LOCAL).

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

### Emitting source-derived strings into C (security — CWE-94)

**Rule: any source-derived string written into the generated C as a C string
literal MUST go through `gen_str_literal_c($cg, $val)` (CodeGen.strada) — never
raw-concatenate it between `"`s** (`emit($cg, "\"" . $val . "\"")`).
`gen_str_literal_c` emits the surrounding quotes and escapes `\`, `"`, newline,
tab, CR, and octal-escapes control/high bytes, so a `"`/`\`/newline in the value
(legal in Strada string literals and in Linux *filenames*) can't close the C
literal and inject live C. A raw-concat site is arbitrary-C-injection (RCE) when
compiling untrusted `.strada` source, or compiling a file whose *name* carries
the payload.

Most OOP/string sites already do this (`bless`/`set_package`/`inherit`/literal
regex needles). Three sites historically did NOT and were fixed: the `tie()`
class-name literal (`CodeGenBuiltins.strada`), `__FILE__` (`CodeGenExpr.strada`),
and the per-statement `#line` directive emitted under `-g` (`CodeGen.strada` —
fires even when the program never uses `__FILE__`, so any adversarial filename
hit it). Regression: `t/codegen_escape_test/run.sh` (wired via
`t/t_codegen_escape.sh`). When adding any new literal-emission codegen, use the
escaper.

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

**Closure call entry points (2026-11)**: `$f->(a, b)` with 0-4 non-spread
arguments compiles to `strada_closure_call0..4(fn, a0, ...)`. When the closure
is an ordinary one whose `param_count` equals the argument count (not variadic,
not a `\&fn` wrapper, not void) these call `func_ptr(captures, args...)`
directly; everything else falls back to `strada_closure_call_argv`. Codegen
(`NODE_CLOSURE_CALL` in CodeGenExpr, `emit_closure_call_n`) also drops the
mortal copy of the callee when it is a plain frame local
(`closure_callee_is_frame_local`) that no argument assigns, and, when
`cleanup_push_useful` is 0 (no try in the program), releases owned argument
temporaries with a direct decref instead of a tmps mark/free scope.

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
Registration is an explicit per-operator list of `gen_overload_reg_if(...)` calls
(bootstrap-safe, no hash iteration) — **a new overloadable operator must be added
there AND to the binary-dispatch op list in `gen_expression`'s `NODE_BINARY_OP`
branch AND to `emit_overload_binary_fallback`**; missing the registration line
makes `strada_overload_binary` silently return NULL (the fallback runs). `x`
(string repeat, 2026-11) is wired all three ways; `x=` needs nothing extra
because the parser rewrites it to `$a = $a x n`.

**Compound assignment on typed targets**: the boxed `op=` overload path in
`CodeGenExpr` (`__ca_res`) coerces the result back to the declared type when the
target is in `int_vars`/`num_vars`, matching `gen_unboxed_compound_overload` —
an `int` variable never ends up holding an object.

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

#### Request arena: escape-safe hardening (2026-06)

The request arena (`STRADA_ARENA`) bump-allocates `StradaValue` structs and reclaimed them wholesale at `arena_end`. It crashed whenever an arena value was still referenced after `arena_end` — which is the *normal* case, since `my $t = build(...); ...; arena_end()` leaves `$t` in scope until its compiler-emitted scope-exit `decref`/`break_self_cycle`, which then ran on freed block memory (SIGSEGV), and array/hash backbones double-freed against their pools. Fix (`runtime/strada_runtime.c`):
- `strada_arena_end` now **neutralizes** each arena SV after releasing its backbone — `sv->refcount = 2000000000` (immortal sentinel), `sv->type = STRADA_UNDEF`, `sv->meta = NULL` — and **retains the blocks on `arena_block_freelist`** instead of `free()`ing them. Escaped references thus point to valid, inert memory: their `decref` no-ops (immortal check in `_impl`) and `break_self_cycle` no-ops (new immortal short-circuit in BOTH the header inline and `_impl`, added *before* it reads `->type`/`->value`). `arena_alloc_sv` recycles blocks from the free-list, so steady-state memory is bounded by peak arena depth.
- Only cleanup of an escaped value is made safe — *using* (reading) one after `arena_end` is still undefined (don't). An arena container holding a persistent value still leaks one ref; `DESTROY` still doesn't run for arena objects.
- These changes are inert for non-arena code: the `break_self_cycle` immortal check is false for normal SVs (rc ≤ 1e9), and the arena paths only run between `arena_begin`/`arena_end`.
- **Perf**: `arena_owns()` is O(blocks) and is consulted on every container decref/free while an arena is open, so one giant arena is counterproductive; the arena is a win for many small request-sized scopes (immortal arena SVs are also skipped by the cycle collector, eliminating its buffering for arena data). `benchmarks/bench_binary_trees.strada` arenas each short-lived tree → ~15%. Regression: `examples/test_arena_hardened.strada` (escaping array/hash, nested trees across many scopes, block reuse, nested arenas) + the same in the leak suite (valgrind-clean).

#### Perl-style list flattening + `our` global containers (2026-06)

Two related codegen areas fixed so arrays behave like Perl and `our` composite globals are mutable from functions.

**List flattening** (`CodeGenExpr.strada`, the `NODE_ANON_ARRAY` builder): in list context, an element is spliced (flattened) iff it is *statically* an array value; references stay single. `array_flatten_kind($cg, $elem)` returns 0 (scalar/ref — push as one), 1 (an `@array` variable — borrowed elements), or 2 (an owned array temp: a parenthesized list, `map`/`grep`/`sort`, a call whose declared return type is `array`, or an array builtin `split`/`keys`/`values`/`reverse`). Kinds 1/2 splice via `strada_deref_array` (which unwraps the REF-boxing — a `func f() array` returns a value of runtime type `STRADA_REF`, *not* bare `STRADA_ARRAY`, so a runtime `type==ARRAY` check is WRONG and static detection is required). Parser distinguishes the two anon-array syntaxes with an `is_list` flag on the node: `(a, b)` sets `is_list=1` (flattens), `[a, b]` leaves it 0 (an array ref — never flattens, Perl-correct). `resolve_call_return_type` mirrors `gen_user_call`'s name resolver (plain then package-prefixed) so the array-return detection can't diverge from emission. Regression: `examples/test_list_flatten.strada`.

**`our` global containers** (`CodeGenStmt.strada`): `our` array/hash globals are stored in the global registry; `strada_global_get` returns the LIVE incref'd pointer, so in-place mutation (push, `$g[i]=`, `$h{k}=`, `core::hash_set`, ref-mutate) from any function persists. Three init-form bugs broke this and are fixed via `emit_our_global_init` + a no-init branch: (1) empty `()` for an array global was emitted as `strada_anon_hash(0)` (a HASH) → now `strada_new_array()`; (2) a no-init `our array @g;` / `our hash %h;` never created the container → now creates an empty one; (3) a non-empty hash literal `our hash %h = ("a"=>1)` (which parses as a flat `NODE_ANON_ARRAY`) was stored as an array → now wrapped in `strada_hash_from_flat_array`, mirroring the `my hash %h = (...)` path. Scalars always worked. Caveat unchanged: `@Pkg::VAR` cross-package qualified *access* syntax still doesn't parse (use accessor funcs). Regression: `examples/test_our_globals.strada`.

**`our` global container *reassignment*** (`CodeGenExpr.strada`, the `NODE_ASSIGN` `@`/`%`-sigil branches — sibling of the init fix above, 2026-06): whole-container *assignment* `@g = …` / `%g = …` to an `our` global publishes a freshly-built value via `strada_global_set_cstr` (it is reached through `strada_global_get`, **not** a C lvalue — so the local-variable path's `gen_expression($target) = …` would emit `strada_global_get(...) = …`, a hard "lvalue required" C error). Three parallel bugs were fixed: (1) `@g = ("x")` — a 1-element parenthesized list collapses to a bare scalar node, and the `our`-array path stored that scalar directly, so `size()` returned `strlen` and `join()` saw nothing (this silently dropped `-ldl` from the **compiler driver's** link line → `undefined reference to dlopen` on glibc < 2.34); now wrapped into a 1-element array via `is_scalar_expr`, mirroring the local path. (2) `%our = (...)` had **no** `our`-hash branch at all and fell through to the local-hash path → the lvalue C error above, on *every* such reassignment; now a dedicated branch builds the hash (`strada_new_hash` / `strada_hash_from_flat_array` / `strada_hash_from_ref`) and publishes it. (3) `@our = @other` stored a *shared* ref (aliasing — later `push` to one mutated both); now `strada_array_copy` (and `strada_hash_to_flat_array` for `@our = %h`), like the local path. **Ownership gotcha:** these copy/flatten builders only *borrow* their source, but reading an `our` *source* (`@our = @other_our`) emits an incref'ing `strada_global_get` — so the source must be captured into a temp and `strada_decref`'d when `needs_temp_cleanup` is 1, else it leaks one ref per assignment (caught only by a *looping* leak test with an `our` source — a single-shot or `my`-source test misses it). Regressions: `examples/test_our_assign.strada` (functional), `t/leak_tests/test_our_container_assign.strada` (valgrind, loops every RHS form incl. `our` sources).

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

**Recursion safety (CWE-674).** `dumper`/`dumper_str` (`strada_dump`,
`strada_dump_buf_rec`) and `clone` (`strada_clone_rec`) bound their recursion at
`STRADA_DEEP_STRUCT_MAX_DEPTH` (256, matching `STRADA_FREE_MAX_DEPTH` — a depth
safe even on 256KB coroutine stacks). Without it, attacker-shaped input was a
stack-overflow DoS: a self-referential ref (`$h{"self"}=\%h`) crashed the dumper
immediately and ~100k-deep nesting overran the C stack in both. The dumper also
carries an ancestor-pointer `path[]` so a true cycle prints `...(cycle)...`
instead of recursing (shared-but-acyclic substructure still expands fully);
past the depth cap it prints `...(max depth)...` and `clone` truncates that
subtree to `undef` (warning once to stderr). `clone`'s seen-map already
terminated cycles; it gained only the depth cap. Real data is far shallower
(JSON decode caps nesting at 100). Regression:
`examples/test_dumper_clone_depth.strada` (in `t/t_core.sh`).

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

## Audit M2h mechanisms (2026-11)

- **Hash key UTF-8 flag**: `StradaHashEntry.key_bytes` (struct padding) marks a key stored from a non-ASCII, non-UTF-8 string. `sv_key_extract_buf` sets the thread-local `strada_key_bytes_hint` to such a key's bytes; each insert sets `key_bytes = (key == hint)`. Readers use `strada_new_str_share_key(&entry)`. VM: `VMHashEntry.key_bytes`, set by `OP_HASH_SET` (`g_vm_key_bytes_hint`) and its two inline-insert fast paths; `vm_hash_make_key_str_b`.
- **MRO**: `oop_walk_ancestors(name, fn, ud)` is an iterative, cycle-safe DFS with no size limits; `oop_mro(pkg, &arr)` caches the flattened array on the package (generation-validated) and replaced every `mro[64]` buffer; `oop_isa_check` and `strada_method_lookup_package` use it.
- **END blocks**: `strada_register_end_block(fn, is_library)` keeps two LIFO lists run by one atexit handler: program first, libraries (shared-library constructors) after.
- **Pool exit**: `StradaThreadPool.active` counts running tasks; `strada_pool_onexit` (glibc `on_exit`) cancels queued tasks, waits `strada_exit_task_wait_ms` (default 1000; build option `--exit-task-wait-ms N`), and `_exit(status)`s after `fflush(NULL)` if tasks are still running.
- **Export metadata**: `func:` field 7 = per-parameter default tokens (`i<int>`, `n<num>`, `s<%-escaped>`, `u`, `f<generated fn>`, `x` = not exportable); `synthesize_default_funcs` (library builds) turns expression defaults into `<func>__dflt<N>` functions. `ownctor:NAME` lines mark constructors whose objects are always of their own class (`fn_ctor_own_class`); devirtualization after `Pkg::new` requires it.
- **VM print to runtime handles**: `OP_PRINT_FH`/`OP_SAY_FH` write `VM_OBJ_NATIVE_SV` sockets/filehandles through `strada_print_fh`/`strada_say_fh`.

## Cycle Collector & Request Arena (runtime + codegen)

Both are compile-time features in `runtime/strada_runtime.c`, gated by `STRADA_CYCLE_GC` / `STRADA_ARENA` (default on via `./configure`, wired into the runtime compile in `Makefile` and the `strada` wrapper's `build_runtime()`). Both are confined to the runtime — they do **not** change the `StradaValue` struct, so user translation units need no define and the ABI is stable whether or not the features are compiled in.

**Cycle collector** — Bacon–Rajan synchronous trial deletion:
- Colour lives in three spare `flags` bits (`STRADA_SVf_CC_COLOR_MASK`, bits 11-13; BLACK = 0 outside a collection). A buffered root has `STRADA_SVf_CC_BUFFERED` set and its index in `cc_roots` stored in `struct_size` (unused by the cyclic container types), so buffering and `cc_forget` are O(1): a freed candidate's slot is NULLed, or popped when it is the last. There is no side table (it used to be a pointer-keyed hash, `cc_tab`, which cost 9-12% of allocation-heavy benchmarks). A collection skips NULL slots, clears `BUFFERED` from surviving roots before freeing anything, and leaves every survivor BLACK. `strada_decref` tests the `BUFFERED` bit and the type inline before calling `cc_possible_root`.
- The only hot-path hook is in the out-of-line `strada_decref` survive branch (`new_rc > 0`): `cc_possible_root(sv)` buffers `ARRAY`/`HASH`/`REF` candidates and **pins** them (raw incref) so they can't be freed underneath the collector. Drop-to-zero is unchanged.
- `cc_collect()` runs MarkGray → Scan → CollectRoots; `cc_each_child` enumerates strong children (filtered by `cc_collectable`, which excludes tagged ints, immortals, and arena-owned values). `cc_free_shell` frees a white node's own storage without decref'ing children (those are reclaimed by recursion). `Scan` discounts the pin via `buffered ? 1 : 0`.
- Triggers: candidate threshold (`cc_threshold`, default 10000) and an `atexit` final sweep.

**Request arena**:
- `arena_alloc_sv` bump-allocates `StradaValue` structs into iterable `ArenaBlock`s; only the struct is bumped, backbones stay malloc'd.
- `ARENA_OWNS(sv)` (range check over the arena block list) makes `strada_free_value` and the collector no-op on arena values. `strada_arena_end` walks the blocks and calls `arena_release_backbone` per SV (frees backbone + resources, no child decref, no `DESTROY`), then frees the blocks.

**Codegen** (`compiler/CodeGen.strada`): `core::gc_collect/gc_enable/gc_disable/gc_threshold/gc_collections/gc_freed` and `core::arena_begin/arena_end/arena_active` are dispatched in `gen_expression`'s `sys::`-normalized builtin chain (alongside `sys::full_profile_*`). Void ones emit `({ strada_...(); (StradaValue*)NULL; })`; the int-returning stat functions emit `strada_new_int(...)` and are listed in `needs_temp_cleanup()`. The runtime provides no-op stubs for all of these when a feature is configured out, so generated code always links.

---

*Last updated: 2026-02-23* (added NODE_DYN_METHOD_CALL for $obj->$method() dynamic dispatch)

## Build-speed shortcuts: tcc throwaway compilers and `QUICK` (2026-11)

See CLAUDE.md "Fast Compiler Iteration". Summary of the mechanics:

- Makefile `TCC_COMPILER` macro: sed the anchored `#include "strada_runtime.h"`
  to the tcc header, `tcc -c`, link with `$(CC)` against the normal gcc-built
  `runtime.o`. Used by `make quick` and the stage-1 rule whenever tcc exists
  (gcc `-O0` otherwise); `QUICK` no longer affects the Makefile. tcc's "implicit declaration" warning is fatal to the tcc
  attempt (falls back to gcc `-O0`).
- `strada_runtime_tcc.h` must therefore declare every runtime function the
  compiler's generated C calls, including bootstrap-only aliases
  (`sys_file_mtime`, bare `getpid`). `strada_reftype_raw` and
  `strada_read_meta_section` were missing until this change — under `--tcc` a
  missing prototype is a silent pointer truncation, not an error.
- `t/run_tests.sh`: `QUICK=1` sets `STRADA_TEST_TCC=1` + `STRADA_DEFAULT_OPT=0`;
  the gcc path is split into compile + link for ccache regardless of `QUICK`.

## Closure capture sites: `scalar_var_c_ref` (2026-11)

Captures are discovered lazily during codegen, not by a prior AST walk: the
`NODE_VARIABLE` branch of `gen_expression` decides param / local / capture when
it emits the name, appending to `anon_capture_str`. `scalar_var_c_ref($cg, $name,
$c_name)` (CodeGen.strada) is that decision as a function returning the C
expression. Hand-emitting sites that now use it: `gen_regex_interpolated_pattern`
(var parts), the int fast path of `NODE_INCREMENT`, and the `NODE_VARIABLE`
branches of `chomp`/`chop` in CodeGenBuiltins. The C-style `for` init declaration
registers its variable in `anon_local_str` after emitting the initializer.

Also fixed alongside: a multi-part interpolated match pattern (`/^a$pat/`) built
a string SV that was passed straight to `strada_to_str_ss` and never released
(one leak per match). The match codegen now binds it to `__re_psv` and decrefs it
unless `regex_pattern_is_single_var($pattern)` (then it is the variable itself,
borrowed).

## Bug-fix batch, 2026-11 (see CLAUDE.md "Semantics fixed in 2026-11")

Runtime pieces added alongside the compiler changes listed in CLAUDE.md:

- **Call depth beyond the frame array**: `strada_call_overflow` (thread-local,
  runtime-internal) counts calls deeper than `STRADA_MAX_CALL_DEPTH`;
  `strada_stack_push` checks `depth + overflow` against the recursion limit and
  `strada_stack_pop` unwinds the overflow first. `strada_stack_pop_il` defers to
  the out-of-line pop when the depth is at the array limit. `StradaTryContext`
  gained `stack_mark`; `STRADA_TRY_PUSH` / `strada_try_push_slot` record
  `strada_stack_mark()` and both throw paths call `strada_stack_restore()`
  before the `longjmp`. (ABI: the try-context struct grew; rebuild objects.)
- `strada_array_splice_sv` treats a tagged int as a scalar replacement.
- `strada_slurp` refuses directories (`fstat`) instead of trusting `ftell`.
- `strada_future_await{,_timeout}` pass a tagged int through.
- `strada_utf8_is_valid` rejects bytes that cannot start a character
  (`utf8_char_len` reports 1 for them so walkers make progress).
- `strada_ptr_api_target`: `core::int_ptr/num_ptr/str_ptr` understand slot
  references (`\$var`); an int variable's tagged value is boxed in place.
- `strada_tr`: `r` flag returns a copy; an empty replacement list maps each
  searched character to itself so it is counted.
- `strada_read_file_n` / `strada_write_file_n` back the sized forms of
  `core::fread` / `core::fwrite`.
- `strada_hash_clear_if_tied`, `strada_scalar_is_tied` support the tie codegen;
  `strada_hash_each_sv` no longer leaks the key on each step.
- Lexer: `\x{HEX}` appends `chr($hv)` (chr is codepoint-oriented; assembling
  UTF-8 bytes by hand with `chr(192 + …)` double-encoded).


## Audit cycle 20 codegen notes (2026-11)

- **Empty list `()`**: parsed as an empty `NODE_ANON_HASH` with `is_empty_list = 1`.
  Keep the node type: `my %h = ()`, `%h = ()`, `@a = ()` and typed declarations
  test `pair_count == 0` to take their clear/empty-init paths. Everything else
  keys off the flag: `ast_add_array_elem` / `ast_add_arg` drop it,
  `array_flatten_kind` returns 2 for it, `gen_expression` emits
  `strada_new_array()`, Semantic rewrites `my $x = ()` and `return ()`.
- **`&&` value**: `gen_short_circuit_or_op($cg, $expr, "!strada_to_bool", "and")`
  -- the predicate string is pasted before `(`, so a leading `!` inverts it.
  Conditions still go through `emit_condition` (C `&&`).
- **Mutable array targets**: `mutable_array_target_kind` / `gen_array_target`
  (CodeGenBuiltins) for push/unshift/splice. Kind 1 = element chain, emitted
  through `gen_assign_container(..., 2)` (the nested-assignment autoviv path,
  borrowed result); kind 2 = plain scalar variable, `strada_autoviv_slot_array`
  (takes the C slot address, cast to `StradaValue **` because try-using
  functions declare locals `volatile`). `our` vars, unboxed vars, `$_`, sort
  `$a`/`$b` keep the old expression path.
- **Dying arrays**: `STRADA_AV_DYING(av)` (refcount already 0, elements being
  released) -- push/push_take/unshift/set drop the store. A DESTROY run by the
  release may otherwise store into the dying array, which then reached the
  pool still holding elements (double free at exit).
- **Global registry teardown** detaches `strada_global_registry` before
  freeing it, so DESTROY code reading `our` variables during exit sees nothing
  rather than an entry mid-free.
- **List flattening (M2)**: `array_flatten_kind($cg, $e)` is the single static
  predicate: 1 = borrowed array (`@var`, `@{...}`), 2 = owned list temp (list
  literal, range, slices, `x` with a list left side, array-valued calls,
  namespaced builtins with unknown return type). Consumers append through
  `emit_list_elem_append($cg, $av_expr, $el, $use_push)` (CodeGenBuiltins),
  which flattens kind>0 values at run time with `strada_sv_is_array_or_aref`
  and handles `NODE_SPREAD`. `print`/`say` with several args go through
  `gen_print_list` -> `strada_print2` / `strada_print_list` (a leading
  filehandle is detected at run time); printf -> `strada_printf_list`.
- **Overflow promotion**: value paths of int-typed `+ - *` emit
  `strada_int_add_ii/_sub_ii/_mul_ii` (header inline, slow path
  `strada_int_arith_ii`); generic scalars use `strada_sv_add/sub/mul`, which
  fall to `strada_int_arith_slow` on overflow or when both operands may be
  exact (INT, UV, integer-looking STR -- `strada_exact_int_of`). Inline C
  int64 arithmetic (`emit_int_operand`) still wraps, and is what int-declared
  destinations use (`is_int_wrap_binop` / `emit_int_wrap_binop`). Literals:
  `int_lit_kind` 1 -> `strada_new_uint(...ULL)`, 2 -> num.
- **foreach aliasing**: the array foreach path sets `$cg->{"fe_writeback"}`
  (a C statement storing the loop variable into `__foreach_av_N[__foreach_i_N]`)
  when the source is `@var`/`@{...}` and `node_writes_var` says the body
  assigns the variable; it is emitted at the end of the body and by
  `emit_foreach_writeback` before `next`/`last` (labeled via
  `label_writeback`). Pre-existing loop variables are saved in
  `__fe_save_N` / `__fr_save_N` (`__fr_savev_N` for unboxed int/num) and
  restored after the loop. The loop condition re-reads the array length.
- **Paren-less method call**: a `NODE_FIELD_ACCESS` that is not a C struct
  field is generated as a zero-arg `NODE_METHOD_CALL` (base_object = object);
  `needs_temp_cleanup` therefore treats every FIELD_ACCESS as owned.
- **Constructor-less `Pkg::new`**: inlined hash construction when the keys are
  literals; otherwise `strada_generic_new(pkg, args_array)` (consumes the
  array). Semantic only reports `undefined function 'Pkg::new'` for an odd,
  spread-free argument count.
- **Hashes and pairs in lists (Audit 20 M4)**: `ast_add_array_elem` splices
  an `is_pair` node (`k => v`) into two elements, so every list literal sees
  `=>` as a comma. The `NODE_ANON_ARRAY` builder treats `hash_list_arg`
  elements (`%h`, `%$r`, `...%h`) as flattening and appends their pairs with
  `emit_hash_pairs_append`; `ast_new_map`/`ast_new_grep` pass the input
  through `ast_hash_list_input` (a `%` input or a parenthesized single item
  becomes a one-element list). The VM mirrors this with `compile_list_src`
  (emits `BUILTIN_HASH_FLAT` for a `%` source before `OP_ARRAY_EXTEND`),
  `vm_list_has_flatten` (hash init from a list with flattening elements
  builds the flat list, then `BUILTIN_HASH_SHALLOW_COPY`), and
  `vm_map_value_kind` (a map block yielding `[..]`/`{..}`/`\X` pushes one
  element; the VM cannot tell an array from an array ref at run time).
- **VM hash layout (Audit 20 M4)**: `VMHash.entries` is dense in insertion
  order; `index[]` (power of two, >= 2x capacity, -1 empty) maps hash ->
  entry slot; inline hashes (capacity 4) have no index and are scanned.
  Deleted entries stay as `occupied == 2`, linked LIFO through their `hash`
  field from `free_head`, and are refilled first -- the compiled runtime's
  policy, so `keys` orders agree. Stale index slots are counted in
  `index_fill`; the index is rebuilt before it passes 3/4. Fast paths use
  `vm_hash_probe_first` (NULL = the key is absent) and
  `vm_hash_append_slot`; the x86 JIT first probe reads `index[hash & mask]`.

## Runtime eval: lib/Eval and Strada::JIT (Audit 20 V5, 2026-11)

- **Eval value flow.** `eval_string_vm_ex` (lib/Strada/Interpreter.strada)
  compiles session decls + the new code into a new `VMProgram`, runs it in a
  new VM seeded from `%g_eval_vm_globals`, and exports the VM's globals back.
  The conversions in interpreter/vm.c (`vm_to_sv_depth(v, depth, nested)`,
  `sv_to_vm_depth` / `sv_to_vm_container`) memoize per thread
  (`g_conv_v2s`, `g_conv_s2v`) while `g_conv_depth > 0`; `vm_seed_globals`,
  `vm_export_globals`, and every top-level `vm_to_sv`/`sv_to_vm` open a span.
  No VM code may run inside a span (addresses could be reused after GC), so
  the eval keeps result + export in one span only when the program has no END
  blocks. `nested == 1` turns a container into a REF (blessed like the
  container). A blessed REF converts to a VM object when
  `vm_prog_has_class(active program, pkg)`; otherwise it stays a
  `VMNativeSV` (host object).
- **Earlier VMs.** A closure exported from VM1 keeps VM1 alive
  (`VMExportedClosureCtx`). `vm_foreign_call` (shared by closure calls and
  `vm_host_method_call`) calls `vm_foreign_vm_call_hook(vm, 0/1)` around the
  call when the VM is an eval VM (`eval_epoch != 0`) that is not active; lib/Eval's
  hook exports the running eval VM, seeds the target if its `eval_sync_gen`
  is behind `g_eval_store_gen`, and afterwards exports the target and
  re-seeds the running eval. `reset()` bumps `g_eval_epoch`, so old VMs stop
  syncing.
- **Host OOP.** After each compile, `eval_register_host_classes` calls
  `strada_inherit` for the program's inherits table and registers an
  `AUTOLOAD` (`eval_host_autoload`) for every package that has functions in
  eval code and no AUTOLOAD of its own; it runs the method in the latest eval
  VM (`g_eval_host_vm`, retained with its program).
- **Error locations.** The wrapped source is built with `eval_sb_add` /
  `eval_sb_add_parts` (chunk offsets come from `split_eval_units_ex`'s
  `top_parts` / segment `parts`); `eval_sb_renumber_tokens` rewrites token
  lines/columns before parsing, so parser and VM-compiler messages give user
  lines. `vm_compile_program`'s stderr output is captured (dup2 to a
  tmpfile) and becomes the exception text.
- **JIT.** See CLAUDE.md "Strada::JIT notes". `extract_function_def` and
  `jit_hoist_named_funcs` work on `strip_strings_and_comments()` output, which
  is length-preserving (every input byte maps to one output byte).

## VM parity batch: overloads, /g, lexical file-level `my` (2026-11-18)

- `OP_STR_OV` (no operands) converts a blessed object on top of the stack through its `""` overload; vm_compiler emits it only when the program declares an overload (`g_prog_has_overloads`, AST scan in `vm_compile_program`, since `prog->overloads` is filled after the functions compile). Use `compile_str_expr` for any new string operand. Hash keys and array indexes need nothing at compile time: the hash/array opcodes convert object keys at run time (`VM_KEY_OV`, `VM_IDX_OV`).
- `while (TARGET =~ /re/g)` compiles to `compile_regex_g_cond`: a `__gpos` local initialized before the loop, `OP_REGEX_FIND` from it, `BUILTIN_REGEX_GPOS_NEXT` for the next position (zero-width: +1 character, end+1 stops).
- File-level `my` and `const`: `g_file_my` records each declaration (name, key, file, package, line). A redeclaration of a name gets the key `NAME__shadowN` (like Semantic's C names), and `resolve_global_var_key` picks the nearest earlier declaration in the current function's file (`g_cur_func_node`). `Pkg::CONST` is a variable node with `const_pkg` (set by the parser), resolved by `file_const_pkg_key` in the VM and by the NODE_VARIABLE branch in Semantic.
- `NI_VAR_DECL` adds the new local's slot after compiling the initializer, so the initializer sees the outer binding.

## Examples parity sweep (2026-11-18)

`t/examples_parity.sh` (skips: `t/examples_parity_skip.txt`). Mechanisms added
while making it pass:

- **VM autovivification of element-store containers**: `compile_viv_container`
  / `compile_viv_target_container` (vm_compiler.c) replace `compile_expr` on the
  container of every element store path (plain `=` in expression and statement
  form, compound and short-circuit assignment, `++`/`--`, in-place string ops,
  `compile_lvalue_prepare`). For a container that is itself an element access it
  emits get / `DUP IS_UNDEF JMP_IF_FALSE` / `NEW_HASH|NEW_ARRAY` + set-back,
  recursively, using `__av_*` temp locals keyed by `assign_depth`. Plain reads
  never vivify. A scalar root is not vivified (parity with compiled codegen).
- **`__C__` block locals**: the JIT wrapper declares one C variable per
  marshalled local; `vm_code_names_ident` restricts the list to identifiers
  the block text contains. The marshal-back path rebuilds containers from the
  StradaValue copy (`sv_to_vm` of nested values makes new VM objects), so a
  block that does name an object local still replaces nested objects with
  copies -- a remaining VM limit.
- **Semantic rewrites** (compiled): `my $x = @{E}` / `$x = @{E}` ->
  `scalar(@{E})` for scalar-variable targets; `my @a = deref_array(E)` /
  `@a = deref_array(E)` (and `%`/`deref_hash`) -> `NODE_DEREF_SCALAR` so the
  array-copy decl/assign paths apply.
- **VM stdout**: `OP_SAY` / `OP_SAY_FH` flush after writing to `stdout`
  (compiled `strada_say` does). `print` stays buffered in both.


## VM performance, tier 1 (2026-11-18)

Measured with callgrind (instruction counts). Wall times are best of 3; VM before → after:
- compute: 32.4s → 7.9s
- functions: 0.47s → 0.11s
- oop: 1.61s → 0.86s
- strings: 0.85s → 0.59s
- hotpaths: 4.56s → 3.74s
- utf8: did not finish → 2.5s

**`OP_JMP_IF_INT_LOCAL` (u16 slot, u16 off).** It jumps when `locals[slot]` is an integer or undef. The `int`-parameter prologue in `compile_function` and in the closure path of `vm_compiler.c` emits it ahead of the `LOAD; DEFINED; JMP_IF_FALSE; LOAD; CAST_INT; STORE` conversion. In the JIT it is a load, a test for 0, `jb_j_not_int`, and two target patches. The jump must be forward: a backward one would need a backedge poll, so the JIT refuses it.

**Function name index.** `vm_program_find_func` probes `VMProgram.fn_index`, which uses FNV-1a with linear probing and stores the function index + 1. `vm_program_add_func` inserts each new function and rebuilds the table at half load. A name that is already present keeps its first index, which is what the old linear scan returned.

**DESTROY cache.** `vm_collect_finalizer_items` calls `vm_find_destroy_cached`, a 64-slot direct-mapped cache keyed by class name hash plus `strcmp`. The cache is validated against `(program, func_count, inherit_count)`. Both counts only grow, so a new method or a new inheritance edge invalidates it.

**Regex capture high-water mark.** `regex_hw` covers every slot that may be set. It is raised by `vm_store_regex_capture` and by the three name writers (the two PCRE2 name tables and the named-capture fallback). Clearing and the `captures()` scans stop at it.

**Self-concat chains.** `is_self_concat_chain` walks the left spine of a `.` chain. It needs 2 to 16 parts, each passing `is_pure_append_rhs` and none being the target. Each part becomes an `OP_APPEND_CONST` or an `OP_APPEND_LOCAL`. The transform is disabled under `g_vm_uses_tie` and `g_prog_has_overloads`, because a tied STORE or an overloaded `.` would observe the intermediate values.

**`__C__` JIT cache key.** `cblock_jit_compile_n` hashes in the size and mtime of `strada_runtime.h`, `strada_runtime_tcc.h`, `vm.h` and `vm_compiler.h`, plus the sizes of the `VM`, `VMProgram` and `VMChunk` structs.

## VM performance, tier 2 (2026-11-18)

Instruction counts (callgrind), tier 1 commit → tier 2:
- closures: 6181M → 3279M
- data: 10753M → 9830M (re-measured: the first figure, 6312M, came from two runs racing on bench_data's fixed /tmp file)
- hotpaths: 40299M → 28115M
- binary_trees: 5416M → 4277M
- oop: 9900M → 8439M
- pipeline: 11056M → 9863M
- regex, sprintf, gc, utf8, binary: 7% fewer
- compute, functions, strings, json: unchanged

**Closures and variadic functions run in the JIT.** `vm_execute_call_cl`, `VM_TRY_JIT_OSR` and `VM_MAYBE_ENTER_JIT_CALLEE` no longer exclude closure frames or `has_variadic` chunks. `L_do_op_call_closure` enters the callee's JIT code after building the frame.

**Direct JIT-to-JIT calls on the slow call paths.** `OP_CALL`'s slow label first calls `vm_jit_call_prep`, and `OP_CALL_CLOSURE` calls `vm_jit_closure_prep`. Each builds the callee frame exactly as the interpreter does (variadic packing through `vm_call_variadic_setup`) and returns the callee's `jit_code`, or NULL when the callee isn't compiled or the call isn't plain. On non-NULL the emitted code reloads its registers and calls the entry directly (`call rax`); a nonzero return code propagates with `pop rbp; ret`. On NULL it falls back to `vm_jit_call_slow` / `vm_jit_call_closure_slow`.

**Native opcodes.** `OP_MOD` and `OP_MOD_LOCAL_I32` (int % positive int: `idiv`, add the divisor to a negative remainder; `jb_j_not_both_int` clobbers RCX, so the divisor is reloaded after it), `OP_NOT` (int), `OP_ARRAY_SIZE` (untied array), `OP_DEFINED` (only when `vm->has_weakrefs == 0`), `OP_LOAD_CAPTURE` / `OP_STORE_CAPTURE` for int values (direct or through a cell). `OP_HASH_REF` emits nothing. Any other operand goes to the step helper.

**Fast-or-bail step cases.** `vm_jit_op_step` handles only the plain case of `OP_DEFINED`, `OP_LOAD_CAPTURE[_CELL]`, `OP_STORE_CAPTURE`, `OP_SPACESHIP` (int/int, num/num), `OP_HASH_GET` / `OP_HASH_SET` (untied hash, string key, no weak value) and `OP_HASH_BLESS`. Everything else does `goto jit_step_bail` before touching the stack: `frame->ip` is set to the instruction and the helper returns `VM_JIT_INTERP`, so the interpreter's own handler runs it (tie, weak refs, overloads and errors keep one implementation). Gotcha: a `case` added to the step helper's fall-through list compiles that opcode as the step call; a `case X: break;` placed inside that list silently turns every later opcode in the list into a no-op.

**Integer to string.** `vm_to_cstr` formats integers with `vm_i64_to_cstr` instead of `snprintf` (12% of hotpaths).

Test: `interpreter/test/test_vm_perf_c20b.strada`.

## VM performance, round 3 (2026-11-18)

Instruction counts (callgrind), tier 2 commit -> round 3:

| Benchmark | Tier 2 | Round 3 | Change |
|---|---|---|---|
| compute | 137527M | 109248M | -21% |
| functions | 1886M | 1652M | -12% |
| binary_trees | 4277M | 3766M | -12% |
| sort | 13243M | 11872M | -10% |
| data | 9830M | 8922M | -9% |
| closures | 3279M | 3032M | -8% |
| hotpaths | 28115M | 26111M | -7% |
| exceptions | 3328M | 3105M | -7% |
| gc | 6052M | 5670M | -6% |
| pipeline | 9863M | 9251M | -6% |
| utf8 | 6010M | 5665M | -6% |
| array_hash | 4464M | 4251M | -5% |
| oop | 8439M | 8103M | -4% |
| sprintf | 13653M | 13195M | -3% |
| regex | 13206M | 12910M | -2% |
| binary | 4065M | 4017M | -1% |
| strings, json | | | 0% |

bench_data writes a fixed file (`/tmp/strada_bench_data.csv`), so never run two interpreters on it at once when comparing.

**Integer add/sub in the JIT** (`jb_int_addsub_rr`, `jb_int_addsub_imm`, `jb_int_tag_after_hi`). Both tagged operands are shifted left 14 so the 50-bit values fill the register; the CPU overflow flag (`jo`) is then the 50-bit overflow check, and `sar 14` + `or VM_TAG_INT_BASE` re-tags. Used by ADD/SUB, ADD/SUB_LOCALS, ADD/SUB_LOCAL_I32, ADD_TO_LOCAL(_VAL), SUB_TO_LOCAL_VAL and INCR/DECR. MUL and the BIT_AND forms keep `jb_tag_rax_int50_or_jump`.

**Calls.** `OP_CALL` uses `callee->local_count` and `has_variadic` as compile-time constants (both are fixed once the program is compiled); a variadic callee jumps straight to the prep/slow path. An int ADD/SUB/MUL followed by `OP_CAST_INT` jumps past the cast's check on its fast path (`skip_cast_idx`). `OP_JMP_IF_INT_LOCAL` is one `je` to the target; the return path's GC check is a single `jb`. fib: 131 -> 117 instructions per call.

**Boxed int64** (values outside the 50-bit inline range) come from the `VM_POOL_CELL` pool (same size as `VMCell`; `_Static_assert`), whose cap is `VM_POOL_MAX_CELL` (16384, one GC cycle's worth). `vm_to_int_vm` returns tagged and boxed ints before any unwrapping. `vm_slow_add_to_local` adds two ints (inline or boxed) directly: wrapping for an `int` target (`coerce == 1`), checked for untyped. In the JIT, `+=` falls back to `vm_jit_add_local_helper`, not the step helper. A `+=` loop on a boxed int: 1087 -> 230 instructions per iteration.

**Per-opcode helpers** (`jb_emit_op_helper`: same contract as the step helper, `int fn(VM *, const VMInsn *)`). Each handles the plain case and otherwise calls `vm_jit_op_step` or `vm_jit_bail_here` (resume the interpreter at this instruction):
- `vm_jit_h_new_container` (NEW_ARRAY / NEW_HASH), `vm_jit_h_array_push` (untied array), `vm_jit_h_concat` (plain strings and numbers), `vm_jit_h_hash_get` / `_set` / `_bless` (moved out of the step helper), `vm_jit_h_spaceship` (floats; two ints are native JIT code).
- Opcodes that used to bail at run time: `vm_jit_h_unary_str` (CHR, BYTES, STR_UPPER, STR_LOWER), `vm_jit_h_split` (STR_SPLIT), `vm_jit_h_sprintf`, `vm_jit_h_map_push` (tied / native sources bail), `vm_jit_h_hash_delete` (untied hash, string key), `vm_jit_h_try` (TRY_BEGIN / TRY_END), `vm_jit_h_isa`, `vm_jit_h_make_closure`, `vm_jit_h_regex` (REGEX_MATCH / REGEX_FIND with a constant pattern).
- The interpreter and these helpers share one implementation: `vm_op_chr`, `vm_op_split`, `vm_op_bytes`, `vm_op_sprintf`, `vm_op_try_begin`, `vm_op_isa`, `vm_op_make_closure`, `vm_op_regex_match`, `vm_op_regex_find`.

**`try` in JIT code.** TRY_BEGIN only pushes a `VMExcHandler` (catch instruction, stack base, frame count), so JIT code can run it. A throw returns `VM_JIT_EXC` up the native call chain to the interpreter, which unwinds to the handler and continues at the catch in the interpreter (OSR re-enters at the next backedge). A callee that is not compiled yet runs in a nested interpreter (`vm_jit_call_slow`); a throw there unwinds to the nested run's base and is raised again in the caller. `VM.exc_trace_keep` / `exc_trace_keep_val` mark that re-raise so `core::exception_trace()` keeps the frames of the original throw (it was rebuilt from the caller's shallower stack; `examples/test_exception_trace_lazy.strada`).

Test: `interpreter/test/test_vm_perf_c20c.strada`.

**Known parity gaps found here (not changed).**
- Untyped `$a + $b` past int64 wraps in the VM (`vm_slow_add_locals`); compiled code promotes to UV/NV.
- `++` / `+= 1` past int64: the interpreter promotes and the JIT's `vm_jit_incr_decr_slow` wraps, whatever the declared type. The opcodes do not carry the `int` declaration.
- An `int` parameter given a value past int64 (or a float) differs from compiled.

**Not yet done.**
- HASH_GET_CONST / HASH_SET_CONST misses (new keys, probe collisions) and BUILTIN still use the step helper.
- `OP_WEAKEN_SLOT`, `OP_TR`, `OP_C_BLOCK`, interpolated regex patterns and tied/native operands still resume the interpreter.
- JIT code keeps every value on the memory stack; fib still spends ~65 of its ~117 instructions per call on call/return.

## VM performance, round 4 (2026-11-19)

Instruction counts (callgrind), round 3 commit -> round 4:

| Benchmark | Round 3 | Round 4 | Change |
|---|---|---|---|
| closures | 3032M | 2383M | -21% |
| oop | 8103M | 6510M | -20% |
| functions | 1652M | 1415M | -14% |
| hotpaths | 26111M | 22701M | -13% |
| compute | 109248M | 96259M | -12% |
| binary_trees | 3766M | 3567M | -5% |
| sort | 11872M | 11388M | -4% |
| pipeline | 9251M | 9117M | -1% |
| gc | 5670M | 5704M | +0.6% |
| everything else | | | 0% |

Where the time was: functions and compute (fib) are almost all JIT code, so the call/return protocol is what counts there. oop spent 20% in the step helper on constant-key hash misses, closures 28% in `vm_jit_closure_prep`, hotpaths a third of its range section building a 20M-element list.

**Call/return.**
- `VM.jit_frame_limit` = min(`frame_cap`, `recursion_limit` when > 0), kept by `vm_frame_limit_update` at the five places that change either. OP_CALL, the method-call and closure-call fast paths compare against it once (was nine instructions).
- OP_CALL drops the `jit_state == 2` check when the callee was already compiled when the caller was compiled (state never leaves 2), or is the function being compiled (its code only runs if the compile succeeds). A self-call is `call rel32` to offset 0.
- RETURN: `exc_top > 0` used to send every return inside any active `try` to `vm_jit_return_slow`. It now checks `exc_stack[exc_top-1].frame_count >= frame_count`, the condition the slow path pops handlers on.
- fib: 227M -> 200M instructions for `cfib` (3 x fib(27)).

**Fusion without branch-target analysis.** Both fusions emit an extra fast path in the first op and still emit every op of the sequence, so a jump into the middle, a non-int operand or a slow path runs the old code.
- Return fusion: an op whose fast path yields an int (ADD/SUB/MUL, *_LOCAL_I32, *_LOCALS, LOAD_LOCAL of an int), followed by OP_RETURN directly or through OP_CAST_INT, jumps past RETURN's pop with the value in rax (`ret_fuse_idx`/`ret_fuse_patch`; an unpatched fusion fails the compile).
- Compare-branch: `LOAD_LOCAL; PUSH_I8/PUSH_I32 or LOAD_LOCAL; EQ/NE/LT/LE/GT/GE; JMP_IF_FALSE/TRUE` with a forward target compares two ints and branches from LOAD_LOCAL's code.
- `LT/LE_LOCAL_I32_JMP_F` with a forward target jumps straight to it (it went through a shared stub).

**Closure calls.** OP_CALL_CLOSURE checks the value is an unbound closure whose chunk is compiled, non-variadic and current (`dt_code_len_src == code_len`), then builds the frame inline (as the method-call path does) with `frame->closure` set, which keeps the closure alive for the GC.

**Hash probe.** `jb_emit_hash_primary_probe` scans `entries[0, used)` of an inline hash (no `index`) for an occupied entry with the key's hash, instead of only slot 0. oop: 12.5M step-helper calls -> 3.

**Calls from C.** `vm_execute_call_cl` is now a small wrapper: for a chunk with `jit_state == 2` it pushes the frame, sets `exec_base`/`jit_stop_base`/`g_active_vm`, and runs the JIT code. If that returns `VM_JIT_OK` at the base it returns the result; otherwise it calls `vm_execute_call_full(..., resume_rc, ...)`, which skips frame setup and continues through `VM_JIT_HANDLE_RC` as if its own JIT entry had returned that code. Sort comparators: ~310 -> ~245 instructions per call; the rest of sort is GC marking and the C sort routines.

**Range foreach.** `foreach my $x (A..B)` where both bounds pass `ctx_expr_is_int` (and are not raw int parameters) keeps the current value in `__fe_i` and the end in `__fe_arr`; the condition is `__fe_i <= end`. Bounds are evaluated once, before the loop, as before. Other ranges (untyped or string bounds) still build the list, so `"aa".."ad"` keeps Perl's string range.

Test: `interpreter/test/test_vm_perf_c20d.strada` (recursion past 256 frames and near the limit, returns through try in the same and an outer frame, `local` in recursion, GC in deep recursion, closure argument counts / captures / recursion / variadic / non-code, inline-hash deletes, sort comparators that throw or sort, every comparison against ints, floats and strings, range edge cases). Output matches compiled, and the VM at round 3, with the JIT on and off.

**Not yet done.**
- gc is 0.6% more instructions; not investigated.
- Boxed-int ADD/MOD/MUL (sort's random-number setup) still go through the step helper.
- The JIT's call frame still writes eight fields per call; fib spends most of its instructions on call/return.

## VM performance, round 5 (2026-11-20)

Scaled instruction counts (callgrind, `mkscaled.sh` copies), before -> after:

| Benchmark | Before | After | Change |
|---|---|---|---|
| strings | 641M | 200M | -69% |
| data | 901M | 649M | -28% |
| regex | 1289M | 1032M | -20% |
| binary | 423M | 391M | -8% |
| pipeline | 2393M | 2230M | -7% |
| round5 | 22634M | 21645M | -4% |
| everything else | | | within 1% |

Where the time was:
- **strings**: 45% in `s/NAME/World/`. Each substitution went through the runtime's substitute (match, a sizing `pcre2_substitute` and the real one), then `vm_sync_runtime_captures` rebuilt the VM's captures from `strada_captures()` and `strada_named_captures()`, a runtime array and a hash per call.
- **data / pipeline**: `split(",", ...)` made runtime strings and converted each into a VM string.
- **regex**: every match copied each group into its own malloc'd string.
- **binary and every builtin call**: a 95 KB frame for `vm_exec_builtin`, probed page by page.

Lazy captures (`vm_rx_record`):
- The VM keeps the span of the subject from the leftmost group start to the rightmost end. That is not the whole subject, because a `while ($big =~ /x/g)` over a 100 KB string copies one byte per match.
- It keeps the offsets relative to that span (`(size_t)-1` = unset), `rx_count`, and the pcre2_code when it has named groups.
- `\K` can give a group an end before its start. Such a group is clamped to empty, and `vm_subst_native` hands such a match to the runtime.
- The name-table pointer stays valid because the regex cache materializes the captures before evicting that pattern. A cache-less compile (`need_free`) materializes at once.

`vm_subst_native` empty-match rule (as PCRE2's global substitute and Perl): after an empty match, retry at the same offset with `PCRE2_NOTEMPTY_ATSTART | PCRE2_ANCHORED`; if that fails, copy one character (UTF-8 aware) and search on. `s/b*/-/g` on "abc" is "-a--c-".

Measured and rejected:
- **GC floor at 64K objects**: gc +32%, oop +13%, closures +11%, hotpaths +6% instructions. Fewer collections, but a larger heap per collection, so the pools cycled less.
- **sprintf without converting arguments**: going through glibc `snprintf` cost more than the runtime's `strada_spf_fast_*` formatters, even for integers.

Compiled literal `s///` and `$&` (follow-up, same day): `strada_subst_sv_cnt` (the compiled fast path for a literal pattern with no flags but `/g` and a plain replacement) never recorded its match, so `captures()->[0]` kept the previous match. It now calls `strada_subst_record_literal`, which records the first match into `pm_state` like the group-less branch of `strada_regex_replace_capture_len2` (both use `pm_keep_groups_set_amp` to keep `$1..$N`). Two cost rules:
- The plain-string path shares the subject's `StradaString` with `pm_state` instead of copying it. It decides in-place reuse before recording, since the shared reference raises the buffer's refcount; the old buffer is then freed at the next record instead of immediately.
- The previous match's captures are materialized only when that match had groups (`ovec_count > 1`) and `last_regex_captures` is not current. Materializing on every call made a loop of literal `s///` 44% more instructions; now it is about 9%.
Test: `t/bugfix_tests/subst_literal_sets_amp.strada`.

## Lazy `//g` match state (2026-11-19)

Reported from Perla (`strada-bugs/001`): `strada_regex_match_global` built
`` $` `` and `$'` (two copies of the subject), the captures / `@-` / `@+` arrays
and a `%+` hash on every match, and walked the UTF-8 subject from byte 0 for
each offset, so a `//g` loop was O(subject length) per match.

- The success path is now `pm_record(...)` + `pm_named_eager(re, ...)`, the
  same as `strada_regex_match_with_capture`; `pm_state.external` stays 0, so
  the `pm_build_*` materializers run on first read.
- `pm_state` keeps two subject sources: `own_buf` (the reusable copy) and
  `shared_ss` (a held reference to the subject's `StradaString`). `pm_record`
  shares when `str == strada_regex_subject_pv` (set by
  `strada_regex_set_subject_for_sv` for a plain STR, cleared by
  `strada_regex_clear_subject_len`) and `SS_FROM_PV(str)->len` equals the
  subject length; otherwise it copies into `own_buf` and drops any shared
  reference. Holding the reference makes in-place writers (concat, substr,
  vec) clone first, so a later `` $` `` sees the matched text. Arena teardown only
  decrefs string buffers, so the reference keeps them alive.
- Thread cleanup releases `shared_ss` and frees `own_buf`.
- `emit_regex_g_condition` (CodeGen.strada) takes `strada_to_str_sv` of the
  target, arms it after building any interpolated pattern (that may match
  too), passes `value.pv`, then clears and releases (cleanup stack when useful).
  Side effects: binary subjects keep their NULs and UTF-8 subjects get
  PCRE2_UTF/UCP in `//g`, as in single matches.
- Callers that pass their own `char *` copy (Perla's `strada_to_str`) still
  pay one memcpy per match; passing `value.pv` of the armed SV makes it O(1).

## Nested-package `::func()` and compact-hash ownership (2026-11-21)

- **`::func()` in a nested package.** The parser rewrites `::twice` inside
  `package Deep::Leaf;` to `Deep_Leaf_twice`. Functions of a `use`d module are
  registered as `Deep::Leaf::twice`, and the existing fallback in
  `analyze_expression` (Semantic.strada) only tries one `_` -> `::` split, so
  it reported "undefined function". When that fails and the name is not
  registered as is, Semantic converts the current package (`eval_package`,
  then `current_func->package`, then `cur_pkg`) to its C prefix, strips it and
  looks up `PKG::rest`. Only `lookup_name` changes; codegen still emits
  `Deep_Leaf_twice`, which is that function's C name. Test:
  `t/test_use_nested_pkg_relative.strada` with `t/nested_use_test/Deep/`
  (a use-cycle between Deep::Leaf and Deep::Peer).
- **`StradaHash.compact_alloc`.** A small hash is one malloc block: header,
  `HASH_SMALL_BUCKETS` index slots, `HASH_COMPACT_ENTRIES` entries. The
  "still in the block" tests compared `hash_index`/`entries` with the inline
  addresses only, assuming a separate malloc could never land there. An
  allocator without chunk headers can place a non-compact hash's index right
  after its 80-byte header; the old test then skipped freeing it (leak) and
  could pool the header as a compact block, and reuse wrote past it. The flag
  is set in the single-block path of `strada_hash_new_presized`, cleared by
  `strada_hash_init`, and kept by pooled blocks. The byte fits in the struct's
  padding (still 80 bytes, no field moved).

## `extern func` declarations in module artifacts (2026-11-21)

- **The bug.** `package Decls; extern func Provider::make(str $s) hash;` and
  a consumer with `use Decls;` calling `Provider::make`. Read from source,
  the declaration reaches the consumer and the call is typed. Read from a
  fresh sibling `Decls.o`, it did not: `gen_export_info` skipped every
  `NODE_EXTERN_FUNC`, so the consumer (a file without `main`, where an
  undeclared qualified call is accepted as host-resolved) emitted
  `extern StradaValue* Provider_make();` and `h = Provider_make(s);
  strada_incref(h);` — one leaked hash per call. Whether a build leaked
  depended only on whether `Decls.o` happened to exist.
- **The fix.** `gen_export_info` (CodeGenStmt.strada) writes one
  `xdecl:C_name:ret:count:types:-1:Pkg.name` line per body-less qualified
  extern (`is_strada_extern_decl`) the unit itself declares
  (`cg_skip_for_module` filters `use`d ones; they come from their own
  artifact through the `use:` deps). `parse_export_metadata` collects them
  into `extern_decls`; `merge_imported_extern_decls` (Parser.strada), called
  from `do_import_object_at`, adds an `ast_new_extern_func` node with
  `is_strada_extern_decl` and `from_use` set unless the program already has a
  function of that name. Not done for `import_lib`: there the declared
  function would be called directly and nothing guarantees the host links it.
- **Not covered.** Unqualified body-less externs (C-ABI declarations) are not
  exported. Old compilers ignore `xdecl:` lines.
- Test: the last two cases of `t/separate_compile_test/run.sh` (the call
  site generated with and without `Decls.o` must be identical).

## A use cycle back to the compile input (2026-11-21)

- **The bug.** `strada -M A.strada` where A has `has ro str $name` and
  `use B;`, and B has `use A;`: resolving B's `use A;` found `A.strada` on
  the lib path and loaded it as a module, although it is the file being
  compiled. Ordinary functions then existed twice under different registry
  keys (`A_label` from the input, `A::label` from the module copy) and went
  unnoticed; `has` accessors keep their C name in both, so Semantic reported
  "function 'A_name' already defined". With a sibling `A.o` present the
  cycle could instead import that stale object of the file being rebuilt.
- **The fix.** `parse_program` stores `core::realpath` of the parser's file
  name in `$program->{"root_file"}` (the driver passes the original path as
  `--source-name`); `load_module` copies it to each module's program;
  `resolve_use_module` compares it with the resolved module path and, on a
  match, marks the module loaded and returns. Unset when the name is not a
  real file (eval, REPL).
- Test: the use-cycle cases at the end of `t/separate_compile_test/run.sh`.

## File-level `my` in module-only objects (2026-11-21)

- **The bug.** `gen_program` emitted a file-level `my $LIMIT` as a plain
  `StradaValue *LIMIT = NULL;` whenever the unit had no `main` or was built
  `-M`. Two modules with the same variable name then both exported `LIMIT`
  ("duplicate symbol" at link). It was also order-dependent: when one
  module's compile read the other from source, `register_globals` saw two
  declarations and named its own `LIMIT__shadow1`, hiding the clash until a
  rebuild read the other module from its artifact instead.
- **The fix.** In `module_only_mode` the declaration is
  `static __attribute__((used))`, as it already was for a program with
  `main`. Nothing outside the file can name a lexical, and a consumer never
  declares another module's globals (`cg_skip_for_module`). `--shared` and
  `--object-full` units without `-M` are unchanged.
- Test: the file-level `my` case at the end of
  `t/separate_compile_test/run.sh`.

## Global interpreter lock (2026-11-21/22)

On by default once a program becomes multi-threaded; a program built with
`--no-gil` has none, in which case none of this runs beyond one load of `strada_gil_on` per
blocking call. A program that starts no thread never takes it.

- **State** (strada_runtime.c, after the `cc_*` forward declarations):
  `gil_mu` protects `gil_locked`, `gil_waiters` and `gil_switches`;
  `gil_owner` is the holder's `pthread_self()` (read atomically by
  `gil_i_hold`); `gil_free_cv` wakes a waiter when the lock is dropped and
  `gil_switch_cv` tells a yielding thread that someone else took it.
- **Fairness.** A waiter uses `pthread_cond_timedwait` with a 5 ms interval
  (`gil_interval_ns`; `--gil-interval-us` sets it). If the lock did not change hands in that time it sets
  `strada_gil_drop_request`; the holder's next `strada_gil_poll()` calls
  `strada_gil_yield_slow`, which drops the lock and waits on `gil_switch_cv`
  until another thread has taken it, then queues again. Compiled loops and
  the VM's safepoints call `strada_gil_poll()` (stages 2 and 3, see CLAUDE.md).
  Under tcc the header's poll reads the flag without `__atomic_load_n`, which
  tcc does not have.
- **Blocking.** `gil_blocking_enter` drops the lock at depth 0 and remembers
  whether this thread held it (`gil_block_had`); `gil_blocking_leave` takes it
  back. The wrappers call the collector's hook first on enter and last on
  leave, so a thread waiting for the lock is still counted as safe for
  stop-the-world.
- **fork.** `gil_atfork_child` re-initializes the mutex and condition
  variables and makes the forking thread the holder.
- **What was measured.** Four threads pushing to one array, incrementing hash
  entries and assigning one scalar: crashed 3 of 3 runs without the lock,
  compiled and VM; exact results with it. ThreadSanitizer: 43 / 16 / 9
  reports on three test programs without the lock, 0 with it. A CPU-bound
  `async::map` on 2 workers: 0.65 s without the lock (1.75x), 1.13 s with it.
- **VM.** No VM change in this stage: thread bodies run inside
  `strada_thread_wrapper` and VM blocking calls reach the runtime's hooks.
  An `async func` call in the VM passes its arguments as copies (the compiled
  backend shares them), which is a separate parity gap.
- **Measured after stage 5** (2 cores): CPU-bound `async::map` with 2 workers
  1.25 s with the lock (1.21 s with 1 worker), 0.65 s without the lock;
  bench_async 0.55 s with the lock, 0.40 s without. Benchmarks that start no
  thread: instruction counts unchanged.
- **Settings are build-time (2026-11-23).** The runtime reads no environment
  variable for the lock. `strada_gil_configure(enabled, interval_us)` stores
  the setting (`gil_cfg_on`, `gil_interval_ns`) and is ignored once
  `gil_decided` is set, i.e. after the first thread started.
  - `stradac --no-gil` / `--gil-interval-us N` / `--gil-polls` are parsed in
    Main.strada into `%gil_cfg`, copied onto the program node (`gil_off`,
    `gil_interval_us`, `gil_polls`) and from there into `$cg` in
    `gen_program`. `main()` calls `strada_gil_configure` right after
    `strada_init_proctitle`, before BEGIN blocks and `our` initializers.
    A file without `main` emits nothing: the program that holds `main`
    decides.
  - The driver forwards all three to stradac, forwards `--gil-polls` to its
    `-M` sub-builds and the module-cache warmer, and adds `gp` to the
    module-cache key for it.
  - `strada-interp --no-gil` / `--gil-interval-us N` call the same function
    before any program code runs.
  - `core::gil_info()` (`strada_gil_info`) returns `enabled`, `active`,
    `interval_us`.
- **The other runtime switches are build-time too (2026-11-23).** The runtime
  has no `getenv("STRADA_...")` left. Same path as the lock options
  (`%gil_cfg` keys `no_gc`, `no_pool`, `debug_bless`, `exit_wait_ms` ->
  program node `rt_*` -> `$cg`), emitted after `strada_gil_configure`:
  - `--no-gc` -> `strada_gc_set_enabled(0)` (was `STRADA_GC=off|0`, read in a
    constructor and in `strada_init_proctitle`).
  - `--exit-task-wait-ms N` (0..3600000) -> `strada_set_exit_task_wait_ms(NL)`.
  - `--no-pool` -> `strada_set_no_pool(1)`. It used to be read in a
    constructor; now it flips at the top of `main()`, after global
    initialisers. That is safe because every free path decides by the block
    (`hv_idx_in_block`, pool counters), never by the flag: blocks pooled
    earlier just stay in their pools.
  - `--debug-bless` -> `strada_set_debug_bless(1)`.
  - `--check-cleanup=warn` is driver-only: it adds
    `-DSTRADA_CLEANUP_CHECK_WARN`, and the header then maps
    `strada_cleanup_check_fail` to `strada_cleanup_check_warn` in the
    program's own code.
  - stradac is built with `--no-gc` by the Makefile (the stage-2 emit and
    `make quick`); the driver no longer sets `STRADA_GC` around stradac.
    Stage 1 cannot take the flag (the bootstrap writes its `main()`), and
    runs with the collector on: 21.0 s -> 21.3 s, 769 MB -> 785 MB, same
    output.
  - strada-interp takes `--no-gc`, `--exit-task-wait-ms N`, `--no-pool`,
    `--debug-bless`.
  - `int(...)` is not accepted by the frozen bootstrap in compiler sources;
    Main.strada converts the option with `+ 0`.
  - Tests: a `# FLAGS: ...` line (lines 2-5) in a `t/bugfix_tests` file gives
    compiler options, passed to stradac (`STRADAC_TEST_FLAGS` in
    `compile_strada`) and to strada-interp. `gil_reader_writer` uses it and
    prints the interval it got; `t/gil_options_test/run.sh` covers the rest.
- **Why the VM skips a collection.** The root VM collects while holding the
  lock and waits for every worker to be GC-safe. A worker inside a runtime
  call that released the lock, at a call site not bracketed with
  `vm_worker_enter_safe`, is waiting to get the lock back and is not safe:
  neither side can move. `strada_pclose` from `vm_exec_builtin` was such a
  site (now bracketed); the skip in `vm_gc_stw_begin` covers any that remain.

## System additions (2026-11-22)

`core::environ`, `uname`, `nproc`, `socketpair`, `mkfifo`, `rewinddir`,
`getservbyname`, `crc32` and `uuid` are plain runtime functions
(`strada_environ` ... `strada_uuid`, after `strada_setgroups` in
strada_runtime.c). Each is wired the way `initgroups` is:

- `$b{"sys::NAME"} = 1;` in Semantic.strada.
- One handler in CodeGenBuiltins.strada: `emit($cg, "strada_NAME()")` for the
  zero-argument ones, `gen_call_with_arg_cleanup($cg, "strada_NAME", $args, N)`
  for the rest. The VM bridge generator harvests exactly these two shapes.
- `$owned_set{"sys::NAME"} = 1;` in CodeGen.strada (every one returns an owned
  value).
- `python3 tools/gen_vm_generic_builtins.py`, then rebuild the interpreter.
  No VM code is written by hand.

Notes on the implementations: `nproc` counts the affinity mask on Linux
(`sched_getaffinity`), so it honours `taskset` and container CPU sets, and
falls back to `sysconf`. `getservbyname` uses `getservbyname_r` on glibc.
`crc32` reads the argument with `strada_sv_raw_bytes` (binary-safe). `uuid`
reads `/dev/urandom` and returns undef if it cannot. Paths go through
`strada_path_buf` (NUL bytes rejected).

Tests: `t/bugfix_tests/sys_info_posix_additions.strada` (compiled and VM),
`t/leak_tests/test_sys_info_additions.strada` (valgrind).

# Understanding the Strada Compiler

A beginner-friendly guide to how the Strada compiler works, step by step.

## What Does the Compiler Do?

The compiler transforms your Strada code into C code, which is then compiled
to a native executable:

```
hello.strada  →  hello.c  →  hello (executable)
     ↑              ↑           ↑
  You write    Compiler     GCC compiles
    this       generates       this
```

## The Four Stages

The compiler processes your code in four stages:

```
Source Code
    ↓
┌─────────┐
│  LEXER  │  Breaks text into tokens
└────┬────┘
     ↓
┌─────────┐
│ PARSER  │  Builds a tree structure (AST)
└────┬────┘
     ↓
┌─────────┐
│ CODEGEN │  Generates C code from the tree
└────┬────┘
     ↓
  C Code
```

Let's follow a simple program through each stage.

## Example Program

We'll trace this simple program:

```strada
func main() int {
    my str $name = "World";
    say("Hello, " . $name);
    return 0;
}
```

---

## Stage 1: Lexer (Tokenization)

**File:** `compiler/Lexer.strada`

The lexer reads your source code character by character and groups them into
**tokens**. A token is the smallest meaningful unit, like a word in a sentence.

### What the Lexer Sees

Your code is just a stream of characters:

```
f u n c   m a i n ( ) ...
```

### What the Lexer Produces

The lexer groups these into tokens:

```
FUNC        "func"
IDENT       "main"
LPAREN      "("
RPAREN      ")"
INT         "int"
LBRACE      "{"
MY          "my"
STR         "str"
SIGIL       "$"
IDENT       "name"
ASSIGN      "="
STRING      "World"
SEMICOLON   ";"
...
```

### Key Lexer Functions

```strada
# Get the next token from input
func next_token(scalar $lexer) hash {
    skip_whitespace($lexer);

    my str $ch = current_char($lexer);

    # Check what kind of token starts here
    if ($ch eq "(") {
        advance($lexer);
        return make_token(TOK_LPAREN(), "(");
    }
    if ($ch eq "\"") {
        return read_string($lexer);  # Read until closing quote
    }
    if (is_alpha($ch)) {
        return read_identifier($lexer);  # Read word, check if keyword
    }
    ...
}
```

### Token Types

Common token types (defined in `Lexer.strada`):

| Token | Example | Meaning |
|-------|---------|---------|
| `TOK_FUNC` | `func` / `fn` | Function keyword (`fn` is shorthand) |
| `TOK_MY` | `my` | Variable declaration |
| `TOK_IDENT` | `main`, `name` | Identifier name |
| `TOK_STRING` | `"Hello"` | String literal |
| `TOK_NUMBER` | `42`, `3.14` | Number literal |
| `TOK_ASSIGN` | `=` | Assignment operator |
| `TOK_SEMICOLON` | `;` | Statement terminator |

---

## Stage 2: Parser (Building the AST)

**File:** `compiler/Parser.strada`

The parser reads tokens and builds an **Abstract Syntax Tree (AST)**. The AST
represents the structure of your program as a tree.

### What is an AST?

Think of it like diagramming a sentence:

```
Sentence: "The cat sat on the mat"

         Sentence
        /        \
    Subject     Predicate
      |         /      \
    "cat"    Verb     Object
              |         |
            "sat"   "the mat"
```

For code, the AST represents the program structure:

```strada
my str $name = "World";
```

Becomes:

```
    VarDecl
   /   |   \
type  name  init
 |     |     |
str  $name  "World"
```

### The Full Program as an AST

```
            Program
                |
            Function
           /   |   \
        name  ret   body
         |    |      |
       main  int   Block
                     |
            ┌────────┼────────┐
            ↓        ↓        ↓
        VarDecl   Call     Return
        /  |  \     |         |
      str $name  "World"    "say"    0
                           /    \
                       "Hello,"  $name
                           ↓
                        Concat
```

### How Parsing Works

The parser uses **recursive descent** - each grammar rule is a function:

```strada
# Parse a function definition
func parse_function(scalar $parser) hash {
    expect(TOK_FUNC());                    # Must see 'func'
    my str $name = expect_ident();         # Function name
    expect(TOK_LPAREN());                  # Must see '('
    my array @params = parse_params();     # Parameter list
    expect(TOK_RPAREN());                  # Must see ')'
    my str $ret_type = parse_type();       # Return type
    my hash %body = parse_block();         # { ... }

    return make_function_node($name, @params, $ret_type, %body);
}

# Parse a variable declaration
func parse_var_decl(scalar $parser) hash {
    expect(TOK_MY());                      # Must see 'my'
    my str $type = parse_type();           # int, str, etc.
    my str $name = parse_variable();       # $name, @arr, %hash

    my scalar $init = undef;
    if (current_token() eq TOK_ASSIGN()) {
        advance();                         # Skip '='
        $init = parse_expression();        # The value
    }
    expect(TOK_SEMICOLON());              # Must see ';'

    return make_var_decl_node($type, $name, $init);
}
```

### AST Node Types

Nodes are defined in `compiler/AST.strada`:

```strada
# Create a variable declaration node
func make_var_decl(str $var_type, str $name, scalar $init) hash {
    my hash %node = {
        "type" => NODE_VAR_DECL(),
        "var_type" => $var_type,
        "name" => $name,
        "init" => $init
    };
    return %node;
}

# Create a function call node
func make_call(str $name, array @args) hash {
    my hash %node = {
        "type" => NODE_CALL(),
        "name" => $name,
        "args" => \@args,
        "arg_count" => scalar(@args)
    };
    return %node;
}
```

### Common Node Types

| Node Type | Represents | Example |
|-----------|------------|---------|
| `NODE_FUNCTION` | Function definition | `func foo() { }` |
| `NODE_VAR_DECL` | Variable declaration | `my int $x = 5;` |
| `NODE_ASSIGN` | Assignment | `$x = 10;` |
| `NODE_CALL` | Function call | `say("hi")` |
| `NODE_BINARY_OP` | Binary operation | `$a + $b` |
| `NODE_IF_STMT` | If statement | `if (...) { }` |
| `NODE_WHILE_STMT` | While loop | `while (...) { }` |
| `NODE_RETURN` | Return statement | `return 0;` |

---

## Stage 3: Code Generation

**File:** `compiler/CodeGen.strada`

The code generator walks the AST and outputs C code.

### The Basic Pattern

For each AST node type, there's code to generate the corresponding C:

```strada
func gen_statement(scalar $cg, scalar $stmt) void {
    my int $type = $stmt->{"type"};

    if ($type == NODE_VAR_DECL()) {
        gen_var_decl($cg, $stmt);
    } elsif ($type == NODE_RETURN()) {
        gen_return($cg, $stmt);
    } elsif ($type == NODE_IF_STMT()) {
        gen_if($cg, $stmt);
    } elsif ($type == NODE_CALL()) {
        gen_call($cg, $stmt);
    }
    # ... more node types
}
```

### Example: Variable Declaration

Strada code:
```strada
my str $name = "World";
```

AST node:
```
{ type: VAR_DECL, var_type: "str", name: "$name", init: "World" }
```

Code generator:
```strada
func gen_var_decl(scalar $cg, scalar $node) void {
    my str $name = $node->{"name"};
    my str $c_name = "v_name";  # Convert $name to C identifier

    emit($cg, "StradaValue *" . $c_name . " = ");

    if ($node->{"init"}) {
        gen_expression($cg, $node->{"init"});
    } else {
        emit($cg, "strada_new_undef()");
    }
    emit($cg, ";\n");
}
```

Generated C:
```c
StradaValue *v_name = strada_new_str("World");
```

### Example: String Concatenation

Strada code:
```strada
"Hello, " . $name
```

AST:
```
BinaryOp
  op: "."
  left: "Hello, "
  right: $name
```

Generated C:
```c
strada_concat_sv(strada_new_str("Hello, "), v_name)
```

### Example: Function Call

Strada code:
```strada
say("Hello, " . $name);
```

Generated C:
```c
strada_say(strada_concat_sv(strada_new_str("Hello, "), v_name));
```

### The emit() Function

The code generator builds output using `emit()`:

```strada
func emit(scalar $cg, str $text) void {
    # Append text to output buffer
    sb_append($cg->{"output"}, $text);
}

func emit_indent(scalar $cg) void {
    my int $level = $cg->{"indent_level"};
    my int $i = 0;
    while ($i < $level) {
        emit($cg, "    ");
        $i = $i + 1;
    }
}
```

---

## Stage 4: The Complete Picture

### Input (hello.strada)

```strada
func main() int {
    my str $name = "World";
    say("Hello, " . $name);
    return 0;
}
```

### After Lexer

```
FUNC IDENT("main") LPAREN RPAREN INT LBRACE
MY STR SIGIL IDENT("name") ASSIGN STRING("World") SEMICOLON
IDENT("say") LPAREN STRING("Hello, ") DOT SIGIL IDENT("name") RPAREN SEMICOLON
RETURN NUMBER(0) SEMICOLON
RBRACE EOF
```

### After Parser (AST)

```
Program
  └─ Function(name="main", returns="int")
       └─ Block
            ├─ VarDecl(type="str", name="$name", init=String("World"))
            ├─ ExprStmt
            │    └─ Call(name="say")
            │         └─ BinaryOp(op=".")
            │              ├─ String("Hello, ")
            │              └─ Variable("$name")
            └─ Return
                 └─ Number(0)
```

### After CodeGen (hello.c)

```c
#include "strada_runtime.h"

int main(int argc, char **argv) {
    StradaValue *v_name = strada_new_str("World");
    strada_say(strada_concat_sv(strada_new_str("Hello, "), v_name));
    strada_decref(v_name);
    return 0;
}
```

---

## Memory Management in CodeGen

The code generator tracks variables for automatic cleanup.

### Scope Tracking

```strada
# When entering a block (function, if, while, etc.)
scope_push($cg);

# When declaring a variable
scope_track_var($cg, "v_name");

# When leaving a block
scope_pop($cg);  # Emits strada_decref() for all tracked vars
```

### Generated Cleanup

```c
// At end of scope:
strada_decref(v_name);
strada_decref(v_other);
```

### Assignments

Strada:
```strada
$a = $b;
```

C (with reference counting):
```c
({ StradaValue *__old = v_a;
   v_a = v_b;
   strada_incref(v_a);     // New value: increment
   strada_decref(__old);   // Old value: decrement
   v_a; })
```

---

## How to Read the Compiler Code

### Start Here

1. **`compiler/Main.strada`** - Entry point, very short
2. **`compiler/Lexer.strada`** - Read `next_token()` first
3. **`compiler/Parser.strada`** - Read `parse_function()` and `parse_statement()`
4. **`compiler/CodeGen.strada`** - Read `gen_statement()` and `gen_expression()`

### Key Functions to Understand

| File | Function | Purpose |
|------|----------|---------|
| Lexer | `next_token()` | Get next token |
| Lexer | `read_string()` | Parse string literal |
| Lexer | `read_identifier()` | Parse identifier/keyword |
| Parser | `parse_function()` | Parse function definition |
| Parser | `parse_statement()` | Parse any statement |
| Parser | `parse_expression()` | Parse any expression |
| Parser | `parse_primary()` | Parse literals, variables, calls |
| CodeGen | `gen_function()` | Generate C function |
| CodeGen | `gen_statement()` | Generate C for any statement |
| CodeGen | `gen_expression()` | Generate C for any expression |
| CodeGen | `emit()` | Output C code text |

### Tracing a Feature

To understand how a feature works, trace it through all stages:

1. **Lexer**: What tokens does it produce?
2. **Parser**: What AST node does it create?
3. **CodeGen**: What C code does it generate?

Example - tracing `if` statements:

```
Lexer:   TOK_IF, TOK_LPAREN, ..., TOK_RPAREN, TOK_LBRACE, ...
Parser:  parse_if_statement() → NODE_IF_STMT
CodeGen: gen_statement() sees NODE_IF_STMT → emits "if (...) { ... }"
```

---

## Adding a New Feature

Here's the process for adding something new:

### 1. Add Token (if needed)

In `Lexer.strada`:
```strada
func TOK_MYFEATURE() int { return 99; }

# In next_token(), recognize the keyword:
if ($word eq "myfeature") {
    return make_token(TOK_MYFEATURE(), "myfeature");
}
```

### 2. Add AST Node

In `AST.strada`:
```strada
func NODE_MYFEATURE() int { return 99; }

func make_myfeature(scalar $data) hash {
    return { "type" => NODE_MYFEATURE(), "data" => $data };
}
```

### 3. Add Parser Rule

In `Parser.strada`:
```strada
func parse_myfeature(scalar $parser) hash {
    expect(TOK_MYFEATURE());
    my scalar $data = parse_expression();
    expect(TOK_SEMICOLON());
    return make_myfeature($data);
}

# Call it from parse_statement():
if ($tok == TOK_MYFEATURE()) {
    return parse_myfeature($parser);
}
```

### 4. Add Code Generation

In `CodeGen.strada`:
```strada
func gen_myfeature(scalar $cg, scalar $node) void {
    emit($cg, "/* myfeature: */\n");
    emit($cg, "do_something(");
    gen_expression($cg, $node->{"data"});
    emit($cg, ");\n");
}

# Call it from gen_statement():
if ($type == NODE_MYFEATURE()) {
    gen_myfeature($cg, $stmt);
    return;
}
```

### 5. Test

```bash
make clean && make
./strada test_myfeature.strada
./test_myfeature
```

---

## Debugging Tips

### See Generated C Code

```bash
./strada -c myfile.strada    # Keeps myfile.c
cat myfile.c                 # View generated code
```

### Print During Compilation

Add debug output in the compiler:
```strada
say("DEBUG: parsing function " . $name);
```

### Common Issues

| Problem | Likely Cause |
|---------|--------------|
| "Unexpected token" | Lexer not recognizing syntax |
| "Expected X" | Parser rule order or missing case |
| C compile error | CodeGen producing invalid C |
| Runtime crash | Missing incref/decref |

---

## Summary

1. **Lexer** breaks source into tokens (words)
2. **Parser** builds a tree structure (AST) from tokens
3. **CodeGen** walks the tree and outputs C code
4. **GCC** compiles C to native executable

Each stage is independent - tokens flow into parser, AST flows into codegen.
To understand a feature, trace it through all three stages.

The compiler is about 2,500 lines of Strada code total. Start with simple
functions like `next_token()`, `parse_primary()`, and `gen_expression()`
to build understanding, then explore more complex features.

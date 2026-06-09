# Bootstrap Compiler Scope

## What This Compiler Handles

This simplified bootstrap compiler supports the core Strada features needed to:
1. Compile working Strada programs
2. Demonstrate self-hosting concept
3. Provide foundation for full implementation

## Supported Features

### ✅ Data Types
- int, num, str, scalar
- array, hash (basic support)
- void (for functions)

### ✅ Variables
- Variable declarations with `my`
- All sigils: $scalar, @array, %hash
- Initialization

### ✅ Functions
- Function definitions with `func`
- Parameters with types
- Return statements
- Return types

### ✅ Control Flow
- if/elsif/else
- while loops
- for loops
- Basic expressions

### ✅ Operators
- Arithmetic: +, -, *, /, %
- Comparison: ==, !=, <, >, <=, >=
- Logical: &&, ||, !
- String: . (concatenation)
- Assignment: =, +=, -=, .=

### ✅ Built-ins
- print, say
- dumper
- Basic array/hash operations

## Simplified (For Bootstrap)

### ⚠️ Foreach Loops
Use while loops instead:
```strada
# Instead of: foreach my int $x (@array)
my int $i = 0;
while ($i < scalar(@array)) {
    my int $x = $array[$i];
    # ...
    $i = $i + 1;
}
```

### ⚠️ Complex Initialization
Use statement-by-statement:
```strada
# Instead of: my array @nums = (1, 2, 3);
my array @nums = ();
push(@nums, 1);
push(@nums, 2);
push(@nums, 3);
```

## This Is Enough!

This subset is sufficient to:
- Write the self-hosting compiler
- Compile real programs
- Demonstrate all key concepts
- Extend to full spec later

# C Calling Strada Functions

This example demonstrates how to call Strada functions from a C program.

## Files

- `c_calls_strada_lib.strada` - Strada library with functions to export
- `c_calls_strada_main.c` - C program that calls the Strada functions

## Build Steps

From the project root:

```bash
# 1. Compile Strada to C
./stradac examples/c_integration/c_calls_strada_lib.strada \
         examples/c_integration/c_calls_strada_lib.c

# 2. Compile and link everything
gcc -o examples/c_integration/c_calls_strada \
    examples/c_integration/c_calls_strada_main.c \
    examples/c_integration/c_calls_strada_lib.c \
    runtime/strada_runtime.c \
    -Iruntime -ldl -lm

# 3. Run
./examples/c_integration/c_calls_strada
```

## Key Points

### Strada Library (no main function)

```strada
# Functions are automatically exported
func add(int $a, int $b) int {
    return $a + $b;
}
```

### C Program

```c
#include "strada_runtime.h"

/* Required globals */
StradaValue *ARGV = NULL;
StradaValue *ARGC = NULL;

/* Declare Strada functions */
StradaValue* add(StradaValue* a, StradaValue* b);

int main() {
    /* Initialize runtime */
    ARGV = strada_new_array();
    ARGC = strada_new_int(0);

    /* Call Strada function */
    StradaValue *result = add(strada_new_int(5), strada_new_int(3));
    printf("5 + 3 = %ld\n", strada_to_int(result));

    return 0;
}
```

## Type Conversion

| C to Strada | Strada to C |
|-------------|-------------|
| `strada_new_int(x)` | `strada_to_int(sv)` |
| `strada_new_num(x)` | `strada_to_num(sv)` |
| `strada_new_str(s)` | `strada_to_str(sv)` |
| `strada_anon_array(n, ...)` | `strada_array_get(sv->value.av, i)` |

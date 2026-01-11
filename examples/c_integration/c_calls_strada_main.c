/*
 * This file is part of the Strada Language (https://github.com/mjflick/strada-lang).
 * Copyright (c) 2026 Michael J. Flickinger
 *
 * This program is free software: you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation, version 2.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

/*
 * Example: C program calling Strada functions
 *
 * Build steps:
 *   1. ./stradac examples/c_calls_strada_lib.strada examples/c_calls_strada_lib.c
 *   2. gcc -o examples/c_calls_strada \
 *          examples/c_calls_strada_main.c \
 *          examples/c_calls_strada_lib.c \
 *          runtime/strada_runtime.c \
 *          -Iruntime -ldl -lm
 *   3. ./examples/c_calls_strada
 */

#include "strada_runtime.h"
#include <stdio.h>

/* Global variables required by Strada runtime */
StradaValue *ARGV = NULL;
StradaValue *ARGC = NULL;

/* Forward declarations of Strada functions */
StradaValue* add(StradaValue* a, StradaValue* b);
StradaValue* greet(StradaValue* name);
StradaValue* factorial(StradaValue* n);
StradaValue* sum_array(StradaValue* arr);
StradaValue* fibonacci(StradaValue* n);

/*
 * Helper macros for easier C-to-Strada calls
 */
#define STRADA_INT(x)    strada_new_int(x)
#define STRADA_NUM(x)    strada_new_num(x)
#define STRADA_STR(x)    strada_new_str(x)
#define TO_INT(sv)       strada_to_int(sv)
#define TO_NUM(sv)       strada_to_num(sv)
#define TO_STR(sv)       strada_to_str(sv)

int main(int argc, char **argv) {
    /* Initialize Strada runtime globals */
    ARGV = strada_new_array();
    for (int i = 0; i < argc; i++) {
        strada_array_push(ARGV->value.av, strada_new_str(argv[i]));
    }
    ARGC = strada_new_int(argc);

    printf("=== C Program Calling Strada Functions ===\n\n");

    /* Test 1: Simple arithmetic */
    printf("1. add(5, 3) = %ld\n",
           TO_INT(add(STRADA_INT(5), STRADA_INT(3))));

    /* Test 2: String function */
    printf("2. greet(\"World\") = %s\n",
           TO_STR(greet(STRADA_STR("World"))));

    /* Test 3: Recursive function */
    printf("3. factorial(10) = %ld\n",
           TO_INT(factorial(STRADA_INT(10))));

    /* Test 4: Array function */
    StradaValue *arr = strada_anon_array(5,
        STRADA_INT(10), STRADA_INT(20), STRADA_INT(30),
        STRADA_INT(40), STRADA_INT(50));
    printf("4. sum_array([10,20,30,40,50]) = %ld\n",
           TO_INT(sum_array(arr)));

    /* Test 5: Fibonacci */
    printf("5. fibonacci(15) = %ld\n",
           TO_INT(fibonacci(STRADA_INT(15))));

    /* Test 6: Using Strada in a loop */
    printf("\n6. Fibonacci sequence (0-10):\n   ");
    for (int i = 0; i <= 10; i++) {
        printf("%ld ", TO_INT(fibonacci(STRADA_INT(i))));
    }
    printf("\n");

    printf("\n=== Done ===\n");
    return 0;
}

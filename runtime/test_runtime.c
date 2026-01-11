/*
 This file is part of the Strada Language (https://github.com/mjflick/strada-lang).
 Copyright (c) 2026 Michael J. Flickinger
 
 This program is free software: you can redistribute it and/or modify  
 it under the terms of the GNU General Public License as published by  
 the Free Software Foundation, version 2.

 This program is distributed in the hope that it will be useful, but 
 WITHOUT ANY WARRANTY; without even the implied warranty of 
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU 
 General Public License for more details.

 You should have received a copy of the GNU General Public License 
 along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

/* test_runtime.c - Test the Strada runtime */
#include "strada_runtime.h"

int main() {
    printf("=== Strada Runtime Test ===\n\n");
    
    /* Test scalar values */
    printf("Testing scalars:\n");
    StradaValue *sv_int = strada_new_int(42);
    StradaValue *sv_num = strada_new_num(3.14159);
    StradaValue *sv_str = strada_new_str("Hello, Strada!");
    
    strada_print(sv_int); printf("\n");
    strada_print(sv_num); printf("\n");
    strada_print(sv_str); printf("\n");
    printf("\n");
    
    /* Test arrays */
    printf("Testing arrays:\n");
    StradaValue *sv_arr = strada_new_array();
    strada_array_push(sv_arr->value.av, strada_new_int(1));
    strada_array_push(sv_arr->value.av, strada_new_int(2));
    strada_array_push(sv_arr->value.av, strada_new_int(3));
    strada_array_push(sv_arr->value.av, strada_new_int(4));
    strada_array_push(sv_arr->value.av, strada_new_int(5));
    
    printf("Array contents:\n");
    strada_dumper(sv_arr);
    printf("\n");
    
    /* Test hashes */
    printf("Testing hashes:\n");
    StradaValue *sv_hash = strada_new_hash();
    strada_hash_set(sv_hash->value.hv, "name", strada_new_str("Alice"));
    strada_hash_set(sv_hash->value.hv, "age", strada_new_int(30));
    strada_hash_set(sv_hash->value.hv, "city", strada_new_str("NYC"));
    
    printf("Hash contents:\n");
    strada_dumper(sv_hash);
    printf("\n");
    
    /* Test string operations */
    printf("Testing string operations:\n");
    char *concat = strada_concat("Hello, ", "World!");
    printf("Concatenation: %s\n", concat);
    printf("Length: %zu\n", strada_length(concat));
    free(concat);
    printf("\n");
    
    /* Test type conversions */
    printf("Testing type conversions:\n");
    printf("Int to string: %s\n", strada_to_str(sv_int));
    printf("Num to int: %lld\n", (long long)strada_to_int(sv_num));
    printf("String to num: %g\n", strada_to_num(strada_new_str("123.45")));
    printf("\n");
    
    /* Test nested structures */
    printf("Testing nested structures:\n");
    StradaValue *nested = strada_new_hash();
    
    StradaValue *arr1 = strada_new_array();
    strada_array_push(arr1->value.av, strada_new_int(1));
    strada_array_push(arr1->value.av, strada_new_int(2));
    strada_array_push(arr1->value.av, strada_new_int(3));
    
    StradaValue *inner_hash = strada_new_hash();
    strada_hash_set(inner_hash->value.hv, "x", strada_new_int(10));
    strada_hash_set(inner_hash->value.hv, "y", strada_new_int(20));
    
    strada_hash_set(nested->value.hv, "numbers", arr1);
    strada_hash_set(nested->value.hv, "point", inner_hash);
    strada_hash_set(nested->value.hv, "name", strada_new_str("Nested"));
    
    printf("Nested structure:\n");
    strada_dumper(nested);
    printf("\n");
    
    /* Cleanup */
    strada_decref(sv_int);
    strada_decref(sv_num);
    strada_decref(sv_str);
    strada_decref(sv_arr);
    strada_decref(sv_hash);
    strada_decref(nested);
    
    printf("=== All tests completed ===\n");
    
    return 0;
}

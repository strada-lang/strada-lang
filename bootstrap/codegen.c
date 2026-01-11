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

/* codegen.c - Strada Code Generator */
#define _POSIX_C_SOURCE 200809L
#include "codegen.h"
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include <libgen.h>
#include <sys/stat.h>

static void emit(CodeGen *cg, const char *fmt, ...);
static void emit_indent(CodeGen *cg);
static void gen_function(CodeGen *cg, ASTNode *func);
static void gen_statement(CodeGen *cg, ASTNode *stmt);
static void gen_expression(CodeGen *cg, ASTNode *expr);

CodeGen* codegen_new(FILE *output) {
    CodeGen *cg = malloc(sizeof(CodeGen));
    cg->output = output;
    cg->indent_level = 0;
    cg->current_func_is_extern = 0;
    
    // Initialize package support
    cg->current_package = NULL;
    cg->lib_paths = malloc(16 * sizeof(char*));
    cg->lib_path_count = 0;
    cg->included_modules = malloc(64 * sizeof(char*));
    cg->included_module_count = 0;
    cg->source_dir = NULL;
    
    // Add default lib path
    cg->lib_paths[cg->lib_path_count++] = strdup("lib");
    
    // Initialize function registry
    cg->functions.capacity = 32;
    cg->functions.count = 0;
    cg->functions.names = malloc(cg->functions.capacity * sizeof(char*));
    cg->functions.param_counts = malloc(cg->functions.capacity * sizeof(int));
    cg->functions.min_args = malloc(cg->functions.capacity * sizeof(int));
    
    // Initialize struct registry
    cg->structs.capacity = 32;
    cg->structs.count = 0;
    cg->structs.names = malloc(cg->structs.capacity * sizeof(char*));
    cg->structs.definitions = malloc(cg->structs.capacity * sizeof(ASTNode*));
    
    // Initialize variable registry
    cg->variables.capacity = 64;
    cg->variables.count = 0;
    cg->variables.names = malloc(cg->variables.capacity * sizeof(char*));
    cg->variables.struct_types = malloc(cg->variables.capacity * sizeof(char*));

    // Initialize scope-based cleanup tracking
    cg->scope_stack.max_depth = 32;
    cg->scope_stack.depth = 0;
    cg->scope_stack.names = calloc(cg->scope_stack.max_depth, sizeof(char**));
    cg->scope_stack.is_struct = calloc(cg->scope_stack.max_depth, sizeof(int*));
    cg->scope_stack.counts = calloc(cg->scope_stack.max_depth, sizeof(int));
    cg->scope_stack.capacities = calloc(cg->scope_stack.max_depth, sizeof(int));

    return cg;
}

void codegen_set_source_dir(CodeGen *cg, const char *dir) {
    cg->source_dir = dir;
}

void codegen_free(CodeGen *cg) {
    if (cg) {
        // Free function registry
        for (int i = 0; i < cg->functions.count; i++) {
            free(cg->functions.names[i]);
        }
        free(cg->functions.names);
        free(cg->functions.param_counts);
        free(cg->functions.min_args);
        
        // Free struct registry
        for (int i = 0; i < cg->structs.count; i++) {
            free(cg->structs.names[i]);
        }
        free(cg->structs.names);
        free(cg->structs.definitions);
        
        // Free variable registry
        for (int i = 0; i < cg->variables.count; i++) {
            free(cg->variables.names[i]);
            if (cg->variables.struct_types[i]) {
                free(cg->variables.struct_types[i]);
            }
        }
        free(cg->variables.names);
        free(cg->variables.struct_types);
        
        // Free package support
        if (cg->current_package) free(cg->current_package);
        for (int i = 0; i < cg->lib_path_count; i++) {
            free(cg->lib_paths[i]);
        }
        free(cg->lib_paths);
        for (int i = 0; i < cg->included_module_count; i++) {
            free(cg->included_modules[i]);
        }
        free(cg->included_modules);
        
        free(cg);
    }
}

static void register_function(CodeGen *cg, const char *name, int param_count, int min_args) {
    // Check capacity
    if (cg->functions.count >= cg->functions.capacity) {
        cg->functions.capacity *= 2;
        cg->functions.names = realloc(cg->functions.names, cg->functions.capacity * sizeof(char*));
        cg->functions.param_counts = realloc(cg->functions.param_counts, cg->functions.capacity * sizeof(int));
        cg->functions.min_args = realloc(cg->functions.min_args, cg->functions.capacity * sizeof(int));
    }
    
    // Add function
    cg->functions.names[cg->functions.count] = strdup(name);
    cg->functions.param_counts[cg->functions.count] = param_count;
    cg->functions.min_args[cg->functions.count] = min_args;
    cg->functions.count++;
}

static int lookup_function(CodeGen *cg, const char *name, int *param_count, int *min_args) {
    for (int i = 0; i < cg->functions.count; i++) {
        if (strcmp(cg->functions.names[i], name) == 0) {
            *param_count = cg->functions.param_counts[i];
            *min_args = cg->functions.min_args[i];
            return 1;
        }
    }
    return 0;
}

static void register_struct(CodeGen *cg, const char *name, ASTNode *definition) {
    // Check capacity
    if (cg->structs.count >= cg->structs.capacity) {
        cg->structs.capacity *= 2;
        cg->structs.names = realloc(cg->structs.names, cg->structs.capacity * sizeof(char*));
        cg->structs.definitions = realloc(cg->structs.definitions, cg->structs.capacity * sizeof(ASTNode*));
    }
    
    // Add struct
    cg->structs.names[cg->structs.count] = strdup(name);
    cg->structs.definitions[cg->structs.count] = definition;
    cg->structs.count++;
}

static ASTNode* lookup_struct(CodeGen *cg, const char *name) {
    for (int i = 0; i < cg->structs.count; i++) {
        if (strcmp(cg->structs.names[i], name) == 0) {
            return cg->structs.definitions[i];
        }
    }
    return NULL;
}

/* Look up the type of a field in a struct */
static DataType lookup_field_type(CodeGen *cg, const char *struct_name, const char *field_name) {
    ASTNode *struct_def = lookup_struct(cg, struct_name);
    if (!struct_def) {
        return TYPE_INT;  // Default fallback
    }
    
    for (int i = 0; i < struct_def->data.struct_def.field_count; i++) {
        ASTNode *field = struct_def->data.struct_def.fields[i];
        if (strcmp(field->data.struct_field.name, field_name) == 0) {
            return field->data.struct_field.field_type;
        }
    }
    
    return TYPE_INT;  // Default fallback
}

/* Register a variable's struct type */
static void register_variable(CodeGen *cg, const char *name, const char *struct_type) {
    // Check capacity
    if (cg->variables.count >= cg->variables.capacity) {
        cg->variables.capacity *= 2;
        cg->variables.names = realloc(cg->variables.names, cg->variables.capacity * sizeof(char*));
        cg->variables.struct_types = realloc(cg->variables.struct_types, cg->variables.capacity * sizeof(char*));
    }
    
    // Check if variable already exists, update it
    for (int i = 0; i < cg->variables.count; i++) {
        if (strcmp(cg->variables.names[i], name) == 0) {
            if (cg->variables.struct_types[i]) {
                free(cg->variables.struct_types[i]);
            }
            cg->variables.struct_types[i] = struct_type ? strdup(struct_type) : NULL;
            return;
        }
    }
    
    // Add new variable
    cg->variables.names[cg->variables.count] = strdup(name);
    cg->variables.struct_types[cg->variables.count] = struct_type ? strdup(struct_type) : NULL;
    cg->variables.count++;
}

/* Look up a variable's struct type (returns NULL if not a struct) */
static const char* lookup_variable_struct_type(CodeGen *cg, const char *name) {
    for (int i = 0; i < cg->variables.count; i++) {
        if (strcmp(cg->variables.names[i], name) == 0) {
            return cg->variables.struct_types[i];
        }
    }
    return NULL;
}

static void emit(CodeGen *cg, const char *fmt, ...) {
    va_list args;
    va_start(args, fmt);
    vfprintf(cg->output, fmt, args);
    va_end(args);
}

static void emit_indent(CodeGen *cg) {
    for (int i = 0; i < cg->indent_level; i++) {
        emit(cg, "    ");
    }
}

/* Forward declarations */
static int expr_is_owned(ASTNode *expr);
static void gen_expression(CodeGen *cg, ASTNode *expr);

/* Emit an expression as a numeric value, optimizing literal ints/nums
 * to avoid intermediate StradaValue allocations.
 * Also handles owned expressions by capturing, extracting, and decrefing. */
static void gen_as_num(CodeGen *cg, ASTNode *expr) {
    if (expr->type == NODE_INT_LITERAL) {
        emit(cg, "(double)%lld", (long long)expr->data.int_val);
    } else if (expr->type == NODE_NUM_LITERAL) {
        emit(cg, "%f", expr->data.num_val);
    } else if (expr_is_owned(expr)) {
        emit(cg, "({ StradaValue *__nv = ");
        gen_expression(cg, expr);
        emit(cg, "; double __nd = strada_to_num(__nv); strada_decref(__nv); __nd; })");
    } else {
        emit(cg, "strada_to_num(");
        gen_expression(cg, expr);
        emit(cg, ")");
    }
}

/* Check if a string is safe to embed directly in C code (no special chars) */
static int str_is_safe_literal(const char *s) {
    if (!s) return 0;
    for (const char *p = s; *p; p++) {
        if (*p == '\\' || *p == '"' || *p == '\n' || *p == '\r' || *p == '\t' || *p == '\0') return 0;
    }
    return 1;
}

/* Emit an expression as a string, optimizing safe string literals */
static void gen_as_str(CodeGen *cg, ASTNode *expr) {
    if (expr->type == NODE_STR_LITERAL && str_is_safe_literal(expr->data.str_val)) {
        emit(cg, "\"%s\"", expr->data.str_val);
    } else {
        emit(cg, "strada_to_str(");
        gen_expression(cg, expr);
        emit(cg, ")");
    }
}

/* Emit a string comparison with proper cleanup of strada_to_str() allocations */
static void gen_str_cmp(CodeGen *cg, ASTNode *left, ASTNode *right, const char *cmp_op) {
    int left_lit = (left->type == NODE_STR_LITERAL && str_is_safe_literal(left->data.str_val));
    int right_lit = (right->type == NODE_STR_LITERAL && str_is_safe_literal(right->data.str_val));

    int left_owned = !left_lit && expr_is_owned(left);
    int right_owned = !right_lit && expr_is_owned(right);

    if (left_lit && right_lit) {
        /* Both are safe literals - no allocation needed */
        emit(cg, "strada_new_int(strcmp(");
        gen_as_str(cg, left);
        emit(cg, ", ");
        gen_as_str(cg, right);
        emit(cg, ") %s 0)", cmp_op);
    } else {
        emit(cg, "({ ");
        if (!left_lit) {
            if (left_owned) {
                emit(cg, "StradaValue *__lv = ");
                gen_expression(cg, left);
                emit(cg, "; char *__sl = strada_to_str(__lv); ");
            } else {
                emit(cg, "char *__sl = strada_to_str(");
                gen_expression(cg, left);
                emit(cg, "); ");
            }
        }
        if (!right_lit) {
            if (right_owned) {
                emit(cg, "StradaValue *__rv = ");
                gen_expression(cg, right);
                emit(cg, "; char *__sr = strada_to_str(__rv); ");
            } else {
                emit(cg, "char *__sr = strada_to_str(");
                gen_expression(cg, right);
                emit(cg, "); ");
            }
        }
        emit(cg, "StradaValue *__r = strada_new_int(strcmp(");
        if (left_lit) gen_as_str(cg, left); else emit(cg, "__sl");
        emit(cg, ", ");
        if (right_lit) gen_as_str(cg, right); else emit(cg, "__sr");
        emit(cg, ") %s 0); ", cmp_op);
        if (!left_lit) emit(cg, "free(__sl); ");
        if (!right_lit) emit(cg, "free(__sr); ");
        if (left_owned) emit(cg, "strada_decref(__lv); ");
        if (right_owned) emit(cg, "strada_decref(__rv); ");
        emit(cg, "__r; })");
    }
}

/* Check if an expression DEFINITELY returns a new owned value.
 * Returns 1 for expressions that allocate a new StradaValue.
 * Returns 0 for potentially borrowed references.
 * NOTE: NODE_CALL is NOT included because functions may return borrowed
 * references (e.g., array/hash element lookups). */
static int expr_is_owned(ASTNode *expr) {
    if (!expr) return 1;  /* No init = strada_new_undef(), which is owned */
    switch (expr->type) {
        case NODE_INT_LITERAL:
        case NODE_NUM_LITERAL:
        case NODE_STR_LITERAL:
            return 1;  /* Literal constructors always return owned values */
        case NODE_HASH_LITERAL:
        case NODE_ARRAY_LITERAL:
            return 1;  /* Collection constructors are owned */
        case NODE_BINARY_OP:
            return 1;  /* Binary ops produce strada_new_* or strada_concat_sv */
        case NODE_UNARY_OP:
            return 1;  /* Unary ops produce strada_new_* */
        case NODE_REF:
        case NODE_ANON_HASH:
        case NODE_ANON_ARRAY:
            return 1;  /* Reference/anon constructors are owned */
        default:
            return 0;  /* Variables, function calls, hash/array access = possibly borrowed */
    }
}

/* Generate a boolean condition with cleanup for owned temporaries. */
static void gen_condition(CodeGen *cg, ASTNode *expr) {
    if (expr_is_owned(expr)) {
        emit(cg, "({ StradaValue *__cond = ");
        gen_expression(cg, expr);
        emit(cg, "; int __b = strada_to_bool(__cond); strada_decref(__cond); __b; })");
    } else {
        emit(cg, "strada_to_bool(");
        gen_expression(cg, expr);
        emit(cg, ")");
    }
}

/* Push a new scope for variable tracking */
static void scope_push(CodeGen *cg) {
    if (cg->current_func_is_extern) return;
    int d = cg->scope_stack.depth;
    if (d >= cg->scope_stack.max_depth) {
        cg->scope_stack.max_depth *= 2;
        cg->scope_stack.names = realloc(cg->scope_stack.names, cg->scope_stack.max_depth * sizeof(char**));
        cg->scope_stack.is_struct = realloc(cg->scope_stack.is_struct, cg->scope_stack.max_depth * sizeof(int*));
        cg->scope_stack.counts = realloc(cg->scope_stack.counts, cg->scope_stack.max_depth * sizeof(int));
        cg->scope_stack.capacities = realloc(cg->scope_stack.capacities, cg->scope_stack.max_depth * sizeof(int));
        for (int i = d; i < cg->scope_stack.max_depth; i++) {
            cg->scope_stack.names[i] = NULL;
            cg->scope_stack.is_struct[i] = NULL;
            cg->scope_stack.counts[i] = 0;
            cg->scope_stack.capacities[i] = 0;
        }
    }
    if (!cg->scope_stack.names[d]) {
        cg->scope_stack.capacities[d] = 16;
        cg->scope_stack.names[d] = malloc(16 * sizeof(char*));
        cg->scope_stack.is_struct[d] = malloc(16 * sizeof(int));
    }
    cg->scope_stack.counts[d] = 0;
    cg->scope_stack.depth = d + 1;
}

/* Pop current scope, emitting cleanup for its variables */
static void scope_pop_emit(CodeGen *cg) {
    if (cg->current_func_is_extern) return;
    if (cg->scope_stack.depth <= 0) return;
    int d = cg->scope_stack.depth - 1;
    int count = cg->scope_stack.counts[d];
    for (int i = count - 1; i >= 0; i--) {
        if (!cg->scope_stack.is_struct[d][i]) {
            emit_indent(cg);
            emit(cg, "strada_decref(%s);\n", cg->scope_stack.names[d][i]);
        }
        free(cg->scope_stack.names[d][i]);
    }
    cg->scope_stack.counts[d] = 0;
    cg->scope_stack.depth = d;
}

/* Pop current scope WITHOUT emitting cleanup (for blocks that end with return) */
static void scope_pop_silent(CodeGen *cg) {
    if (cg->current_func_is_extern) return;
    if (cg->scope_stack.depth <= 0) return;
    int d = cg->scope_stack.depth - 1;
    int count = cg->scope_stack.counts[d];
    for (int i = 0; i < count; i++) {
        free(cg->scope_stack.names[d][i]);
    }
    cg->scope_stack.counts[d] = 0;
    cg->scope_stack.depth = d;
}

/* Track a variable in the current scope */
static void scope_track_var(CodeGen *cg, const char *name, int is_struct) {
    if (cg->current_func_is_extern) return;
    if (cg->scope_stack.depth <= 0) return;
    int d = cg->scope_stack.depth - 1;
    int c = cg->scope_stack.counts[d];
    if (c >= cg->scope_stack.capacities[d]) {
        cg->scope_stack.capacities[d] *= 2;
        cg->scope_stack.names[d] = realloc(cg->scope_stack.names[d], cg->scope_stack.capacities[d] * sizeof(char*));
        cg->scope_stack.is_struct[d] = realloc(cg->scope_stack.is_struct[d], cg->scope_stack.capacities[d] * sizeof(int));
    }
    cg->scope_stack.names[d][c] = strdup(name);
    cg->scope_stack.is_struct[d][c] = is_struct;
    cg->scope_stack.counts[d] = c + 1;
}

/* Reset all scope tracking (call at start of each function) */
static void scope_reset(CodeGen *cg) {
    while (cg->scope_stack.depth > 0) {
        scope_pop_silent(cg);
    }
}

/* Emit cleanup for ALL scopes (for return statements), skipping named var */
static void scope_emit_all_cleanup(CodeGen *cg, const char *skip_var) {
    if (cg->current_func_is_extern) return;
    for (int d = cg->scope_stack.depth - 1; d >= 0; d--) {
        int count = cg->scope_stack.counts[d];
        for (int i = count - 1; i >= 0; i--) {
            if (skip_var && strcmp(cg->scope_stack.names[d][i], skip_var) == 0) continue;
            if (cg->scope_stack.is_struct[d][i]) continue;
            emit_indent(cg);
            emit(cg, "strada_decref(%s);\n", cg->scope_stack.names[d][i]);
        }
    }
}

static const char* type_to_c(DataType type) {
    switch (type) {
        case TYPE_INT: return "StradaValue*";
        case TYPE_NUM: return "StradaValue*";
        case TYPE_STR: return "StradaValue*";
        case TYPE_SCALAR: return "StradaValue*";
        case TYPE_ARRAY: return "StradaValue*";
        case TYPE_HASH: return "StradaValue*";
        case TYPE_VOID: return "void";
        case TYPE_STRUCT: return "StradaValue*";
        
        /* Extended C types - pass as raw types */
        case TYPE_CHAR: return "char";
        case TYPE_SHORT: return "short";
        case TYPE_LONG: return "long";
        case TYPE_FLOAT: return "float";
        case TYPE_BOOL: return "bool";
        case TYPE_UCHAR: return "unsigned char";
        case TYPE_USHORT: return "unsigned short";
        case TYPE_UINT: return "unsigned int";
        case TYPE_ULONG: return "unsigned long";
        case TYPE_I8: return "int8_t";
        case TYPE_I16: return "int16_t";
        case TYPE_I32: return "int32_t";
        case TYPE_I64: return "int64_t";
        case TYPE_U8: return "uint8_t";
        case TYPE_U16: return "uint16_t";
        case TYPE_U32: return "uint32_t";
        case TYPE_U64: return "uint64_t";
        case TYPE_SIZE: return "size_t";
        case TYPE_PTR: return "void*";
        
        default: return "StradaValue*";
    }
}

/* Convert DataType to raw C type (for struct fields) */
static const char* type_to_c_raw(DataType type) {
    switch (type) {
        case TYPE_INT: return "int";
        case TYPE_NUM: return "double";
        case TYPE_STR: return "char";  // Special handling for arrays
        case TYPE_VOID: return "void";
        
        /* Extended C types */
        case TYPE_CHAR: return "char";
        case TYPE_SHORT: return "short";
        case TYPE_LONG: return "long";
        case TYPE_FLOAT: return "float";
        case TYPE_BOOL: return "bool";
        case TYPE_UCHAR: return "unsigned char";
        case TYPE_USHORT: return "unsigned short";
        case TYPE_UINT: return "unsigned int";
        case TYPE_ULONG: return "unsigned long";
        case TYPE_I8: return "int8_t";
        case TYPE_I16: return "int16_t";
        case TYPE_I32: return "int32_t";
        case TYPE_I64: return "int64_t";
        case TYPE_U8: return "uint8_t";
        case TYPE_U16: return "uint16_t";
        case TYPE_U32: return "uint32_t";
        case TYPE_U64: return "uint64_t";
        case TYPE_SIZE: return "size_t";
        case TYPE_PTR: return "void*";
        
        default: return "void*";
    }
}

/* Calculate size of a type in bytes */
static int type_size(DataType type) {
    switch (type) {
        case TYPE_INT: return 4;
        case TYPE_NUM: return 8;
        case TYPE_STR: return 64;
        
        /* Extended C types */
        case TYPE_CHAR: return 1;
        case TYPE_SHORT: return 2;
        case TYPE_LONG: return 8;
        case TYPE_FLOAT: return 4;
        case TYPE_BOOL: return 1;
        case TYPE_UCHAR: return 1;
        case TYPE_USHORT: return 2;
        case TYPE_UINT: return 4;
        case TYPE_ULONG: return 8;
        case TYPE_I8: return 1;
        case TYPE_I16: return 2;
        case TYPE_I32: return 4;
        case TYPE_I64: return 8;
        case TYPE_U8: return 1;
        case TYPE_U16: return 2;
        case TYPE_U32: return 4;
        case TYPE_U64: return 8;
        case TYPE_SIZE: return 8;
        case TYPE_PTR: return 8;
        case TYPE_FUNC: return 8;  // Function pointer
        
        default: return 8;
    }
}

/* Generate struct definition */
/* Generate function pointer type string */
static void gen_func_ptr_type(CodeGen *cg, ASTNode *field) {
    // Generate: return_type (*name)(param_types)
    // For Strada types (int, num, str, etc.), use StradaValue*
    DataType rt = field->data.struct_field.func_return_type;
    const char *ret_type;
    if (rt == TYPE_INT || rt == TYPE_NUM || rt == TYPE_STR || 
        rt == TYPE_SCALAR || rt == TYPE_ARRAY || rt == TYPE_HASH) {
        ret_type = "StradaValue*";
    } else if (rt == TYPE_VOID) {
        ret_type = "void";
    } else {
        ret_type = type_to_c_raw(rt);
    }
    
    emit(cg, "%s (*%s)(", ret_type, field->data.struct_field.name);
    
    if (field->data.struct_field.func_param_count == 0) {
        emit(cg, "void");
    } else {
        for (int i = 0; i < field->data.struct_field.func_param_count; i++) {
            if (i > 0) emit(cg, ", ");
            // For Strada types, use StradaValue*
            DataType pt = field->data.struct_field.func_param_types[i];
            if (pt == TYPE_INT || pt == TYPE_NUM || pt == TYPE_STR || 
                pt == TYPE_SCALAR || pt == TYPE_ARRAY || pt == TYPE_HASH) {
                emit(cg, "StradaValue*");
            } else {
                emit(cg, "%s", type_to_c_raw(pt));
            }
        }
    }
    emit(cg, ");\n");
}

static void gen_struct_definition(CodeGen *cg, ASTNode *struct_def) {
    emit(cg, "typedef struct %s {\n", struct_def->data.struct_def.name);
    cg->indent_level++;
    
    int offset = 0;
    for (int i = 0; i < struct_def->data.struct_def.field_count; i++) {
        ASTNode *field = struct_def->data.struct_def.fields[i];
        
        emit_indent(cg);
        
        // Special handling for different field types
        if (field->data.struct_field.field_type == TYPE_STR) {
            emit(cg, "char %s[64];\n", field->data.struct_field.name);
        } else if (field->data.struct_field.field_type == TYPE_FUNC) {
            // Function pointer field
            gen_func_ptr_type(cg, field);
        } else {
            const char *field_type = type_to_c_raw(field->data.struct_field.field_type);
            emit(cg, "%s %s;\n", field_type, field->data.struct_field.name);
        }
        
        field->data.struct_field.offset = offset;
        field->data.struct_field.size = type_size(field->data.struct_field.field_type);
        offset += field->data.struct_field.size;
    }
    
    cg->indent_level--;
    emit(cg, "} %s;\n", struct_def->data.struct_def.name);
    
    struct_def->data.struct_def.total_size = offset;
}

/* Generate clone function for a struct */
static void gen_struct_clone_function(CodeGen *cg, ASTNode *struct_def) {
    const char *name = struct_def->data.struct_def.name;
    
    // Generate: StructType* strada_clone_StructType(StructType* src)
    emit(cg, "%s* strada_clone_%s(%s* src) {\n", name, name, name);
    cg->indent_level++;
    
    emit_indent(cg);
    emit(cg, "%s* dst = malloc(sizeof(%s));\n", name, name);
    
    // Copy each field
    for (int i = 0; i < struct_def->data.struct_def.field_count; i++) {
        ASTNode *field = struct_def->data.struct_def.fields[i];
        const char *field_name = field->data.struct_field.name;
        DataType field_type = field->data.struct_field.field_type;
        
        emit_indent(cg);
        if (field_type == TYPE_STR) {
            // String fields need strcpy
            emit(cg, "strcpy(dst->%s, src->%s);\n", field_name, field_name);
        } else {
            // All other fields: direct assignment
            emit(cg, "dst->%s = src->%s;\n", field_name, field_name);
        }
    }
    
    emit_indent(cg);
    emit(cg, "return dst;\n");
    
    cg->indent_level--;
    emit(cg, "}\n");
}

/* Convert Package::Name to path File/Name.sm */
static char* package_to_path(const char *package_name) {
    char *path = malloc(strlen(package_name) + 8);
    strcpy(path, package_name);
    
    // Replace :: with /
    char *p = path;
    while (*p) {
        if (*p == ':' && *(p+1) == ':') {
            *p = '/';
            memmove(p+1, p+2, strlen(p+2)+1);
        }
        p++;
    }
    
    strcat(path, ".sm");
    return path;
}

/* Convert Package::Name to C prefix Package_Name_ */
static char* package_to_prefix(const char *package_name) {
    if (!package_name) return strdup("");
    
    char *prefix = malloc(strlen(package_name) + 2);
    strcpy(prefix, package_name);
    
    // Replace :: with _
    char *p = prefix;
    while (*p) {
        if (*p == ':' && *(p+1) == ':') {
            *p = '_';
            memmove(p+1, p+2, strlen(p+2)+1);
        }
        p++;
    }
    
    strcat(prefix, "_");
    return prefix;
}

/* Check if module already included */
static int is_module_included(CodeGen *cg, const char *module_name) {
    for (int i = 0; i < cg->included_module_count; i++) {
        if (strcmp(cg->included_modules[i], module_name) == 0) {
            return 1;
        }
    }
    return 0;
}

/* Mark module as included */
static void mark_module_included(CodeGen *cg, const char *module_name) {
    cg->included_modules[cg->included_module_count++] = strdup(module_name);
}

/* Find module file in lib paths */
static char* find_module_file(CodeGen *cg, const char *package_name) {
    char *rel_path = package_to_path(package_name);
    char full_path[1024];
    struct stat st;
    
    // Try each lib path
    for (int i = 0; i < cg->lib_path_count; i++) {
        // If source_dir is set, try relative to it first
        if (cg->source_dir) {
            snprintf(full_path, sizeof(full_path), "%s/%s/%s", 
                     cg->source_dir, cg->lib_paths[i], rel_path);
            if (stat(full_path, &st) == 0) {
                free(rel_path);
                return strdup(full_path);
            }
        }
        
        // Try lib path directly
        snprintf(full_path, sizeof(full_path), "%s/%s", cg->lib_paths[i], rel_path);
        if (stat(full_path, &st) == 0) {
            free(rel_path);
            return strdup(full_path);
        }
    }
    
    free(rel_path);
    return NULL;
}

void codegen_generate(CodeGen *cg, ASTNode *program) {
    // Store package name
    if (program->data.program.package_name) {
        cg->current_package = strdup(program->data.program.package_name);
    }
    
    // Add any lib paths from 'use lib'
    for (int i = 0; i < program->data.program.lib_path_count; i++) {
        cg->lib_paths[cg->lib_path_count++] = strdup(program->data.program.lib_paths[i]);
    }
    
    // Emit header
    emit(cg, "/* Generated by Strada Bootstrap Compiler */\n");
    if (cg->current_package) {
        emit(cg, "/* Package: %s */\n", cg->current_package);
    }
    emit(cg, "#include \"strada_runtime.h\"\n");
    emit(cg, "#include <string.h>\n");
    emit(cg, "#include <stdint.h>\n");
    emit(cg, "#include <stdbool.h>\n\n");
    
    // Generate global ARGV and ARGC for command-line arguments
    emit(cg, "/* Global command-line argument variables */\n");
    emit(cg, "StradaValue *ARGV = NULL;\n");
    emit(cg, "StradaValue *ARGC = NULL;\n\n");
    
    // Generate @INC initialization comment
    if (cg->lib_path_count > 0) {
        emit(cg, "/* @INC paths: */\n");
        for (int i = 0; i < cg->lib_path_count; i++) {
            emit(cg, "/*   %s */\n", cg->lib_paths[i]);
        }
        emit(cg, "\n");
    }
    
    // Process 'use' statements - emit includes for module files
    for (int i = 0; i < program->data.program.use_count; i++) {
        ASTNode *use_node = program->data.program.use_stmts[i];
        const char *pkg = use_node->data.use_stmt.package_name;
        
        if (!is_module_included(cg, pkg)) {
            mark_module_included(cg, pkg);
            
            char *module_file = find_module_file(cg, pkg);
            char *pkg_prefix = package_to_prefix(pkg);
            
            if (module_file) {
                emit(cg, "/* use %s; -> %s */\n", pkg, module_file);
                
                // Generate extern declarations for imported functions
                if (use_node->data.use_stmt.import_count > 0) {
                    // Selective imports: use Package qw(func1 func2);
                    emit(cg, "/* Imported: ");
                    for (int j = 0; j < use_node->data.use_stmt.import_count; j++) {
                        if (j > 0) emit(cg, ", ");
                        emit(cg, "%s", use_node->data.use_stmt.imports[j]);
                    }
                    emit(cg, " */\n");
                    
                    // Generate extern declarations
                    for (int j = 0; j < use_node->data.use_stmt.import_count; j++) {
                        const char *func_name = use_node->data.use_stmt.imports[j];
                        emit(cg, "extern StradaValue* %s%s();\n", pkg_prefix, func_name);
                        
                        // Generate alias macro for unqualified access
                        emit(cg, "#define %s %s%s\n", func_name, pkg_prefix, func_name);
                    }
                } else {
                    // Import all (no qw) - just note it
                    emit(cg, "/* All functions from %s available as %s::func() */\n", pkg, pkg);
                }
                
                emit(cg, "\n");
                free(module_file);
            } else {
                emit(cg, "/* Warning: Module %s not found in @INC */\n", pkg);
            }
            
            free(pkg_prefix);
        }
    }
    
    // FIRST PASS: Register all structs for type tracking
    for (int i = 0; i < program->data.program.struct_count; i++) {
        ASTNode *struct_def = program->data.program.structs[i];
        register_struct(cg, struct_def->data.struct_def.name, struct_def);
    }
    
    // Generate struct definitions
    for (int i = 0; i < program->data.program.struct_count; i++) {
        gen_struct_definition(cg, program->data.program.structs[i]);
        emit(cg, "\n");
    }
    
    // Generate clone functions for each struct
    for (int i = 0; i < program->data.program.struct_count; i++) {
        gen_struct_clone_function(cg, program->data.program.structs[i]);
        emit(cg, "\n");
    }
    
    // SECOND PASS: Register all functions for optional parameter tracking
    for (int i = 0; i < program->data.program.function_count; i++) {
        ASTNode *func = program->data.program.functions[i];
        if (strcmp(func->data.function.name, "main") != 0) {
            register_function(cg, func->data.function.name,
                            func->data.function.param_count,
                            func->data.function.min_args);
        }
    }
    
    // Get package prefix
    char *prefix = cg->current_package ? package_to_prefix(cg->current_package) : strdup("");
    
    // Forward declarations
    for (int i = 0; i < program->data.program.function_count; i++) {
        ASTNode *func = program->data.program.functions[i];
        int is_extern = func->data.function.is_extern;
        
        // Determine return type
        const char *ret_type;
        if (func->data.function.return_type == TYPE_STRUCT && func->data.function.return_struct_name) {
            static char struct_ret_type[128];
            snprintf(struct_ret_type, sizeof(struct_ret_type), "%s*", 
                     func->data.function.return_struct_name);
            ret_type = struct_ret_type;
        } else if (is_extern) {
            ret_type = type_to_c_raw(func->data.function.return_type);
        } else {
            ret_type = type_to_c(func->data.function.return_type);
        }
        
        // Special case for main
        if (strcmp(func->data.function.name, "main") == 0) {
            emit(cg, "int main(int argc, char **argv);\n");
        } else {
            // extern functions don't get package prefix (for C linkage)
            const char *func_prefix = is_extern ? "" : prefix;
            emit(cg, "%s %s%s(", ret_type, func_prefix, func->data.function.name);
            
            for (int j = 0; j < func->data.function.param_count; j++) {
                if (j > 0) emit(cg, ", ");
                ASTNode *param = func->data.function.params[j];
                
                // Check if param is struct type
                if (param->data.param.param_type == TYPE_STRUCT && param->data.param.struct_name) {
                    emit(cg, "%s* %s", param->data.param.struct_name, param->data.param.name);
                } else if (is_extern) {
                    emit(cg, "%s %s", type_to_c_raw(param->data.param.param_type), param->data.param.name);
                } else {
                    emit(cg, "%s %s", type_to_c(param->data.param.param_type), param->data.param.name);
                }
            }
            
            if (func->data.function.param_count == 0) {
                emit(cg, "void");
            }
            
            emit(cg, ");\n");
        }
    }
    
    free(prefix);
    emit(cg, "\n");
    
    // Generate functions
    for (int i = 0; i < program->data.program.function_count; i++) {
        gen_function(cg, program->data.program.functions[i]);
        emit(cg, "\n");
    }
}

static void gen_function(CodeGen *cg, ASTNode *func) {
    int is_extern = func->data.function.is_extern;
    int is_main = (strcmp(func->data.function.name, "main") == 0);
    cg->current_func_is_extern = is_extern;  // Track for expressions/returns
    cg->current_func_is_main = is_main;      // Track if this is main

    // Reset and push function-level scope for variable tracking
    scope_reset(cg);
    scope_push(cg);
    
    // For extern functions, use raw C types instead of StradaValue*
    const char *ret_type;
    if (func->data.function.return_type == TYPE_STRUCT && func->data.function.return_struct_name) {
        // Use struct name followed by pointer
        static char struct_ret_type[128];
        snprintf(struct_ret_type, sizeof(struct_ret_type), "%s*", 
                 func->data.function.return_struct_name);
        ret_type = struct_ret_type;
    } else if (is_extern) {
        // For extern functions, use raw C types
        ret_type = type_to_c_raw(func->data.function.return_type);
    } else {
        ret_type = type_to_c(func->data.function.return_type);
    }
    
    // Get package prefix for function name (not for extern functions)
    char *prefix = (cg->current_package && !is_extern) ? package_to_prefix(cg->current_package) : strdup("");
    
    // Special case for main
    if (strcmp(func->data.function.name, "main") == 0) {
        // Generate main with argc/argv and populate ARGV array
        emit(cg, "int main(int argc, char **argv) {\n");
        emit(cg, "    /* Populate global ARGV array */\n");
        emit(cg, "    ARGV = strada_new_array();\n");
        emit(cg, "    for (int i = 0; i < argc; i++) {\n");
        emit(cg, "        strada_array_push(ARGV->value.av, strada_new_str(argv[i]));\n");
        emit(cg, "    }\n");
        emit(cg, "    ARGC = strada_new_int(argc);\n");
        emit(cg, "\n");
        free(prefix);
    } else {
        // Start extern "C" block if needed
        if (is_extern) {
            emit(cg, "#ifdef __cplusplus\nextern \"C\" {\n#endif\n");
        }
        
        emit(cg, "%s %s%s(", ret_type, prefix, func->data.function.name);
        free(prefix);
        
        // For variadic functions, generate as taking StradaValue* array
        if (func->data.function.is_variadic) {
            // All non-variadic params
            int i;
            for (i = 0; i < func->data.function.param_count - 1; i++) {
                if (i > 0) emit(cg, ", ");
                ASTNode *param = func->data.function.params[i];
                
                // Check if param is struct type
                if (param->data.param.param_type == TYPE_STRUCT && param->data.param.struct_name) {
                    emit(cg, "%s* %s", param->data.param.struct_name, param->data.param.name);
                } else if (is_extern) {
                    emit(cg, "%s %s", type_to_c_raw(param->data.param.param_type), param->data.param.name);
                } else {
                    emit(cg, "%s %s", type_to_c(param->data.param.param_type), param->data.param.name);
                }
            }
            
            // Variadic param (last one)
            if (func->data.function.param_count > 0) {
                ASTNode *variadic_param = func->data.function.params[func->data.function.param_count - 1];
                if (i > 0) emit(cg, ", ");
                emit(cg, "StradaValue* %s", variadic_param->data.param.name);
            }
        } else {
            // Regular function
            for (int i = 0; i < func->data.function.param_count; i++) {
                if (i > 0) emit(cg, ", ");
                ASTNode *param = func->data.function.params[i];
                
                // Check if param is struct type
                if (param->data.param.param_type == TYPE_STRUCT && param->data.param.struct_name) {
                    emit(cg, "%s* %s", param->data.param.struct_name, param->data.param.name);
                } else if (is_extern) {
                    emit(cg, "%s %s", type_to_c_raw(param->data.param.param_type), param->data.param.name);
                } else {
                    emit(cg, "%s %s", type_to_c(param->data.param.param_type), param->data.param.name);
                }
            }
        }
        
        if (func->data.function.param_count == 0) {
            emit(cg, "void");
        }
        
        emit(cg, ") {\n");
    }
    
    cg->indent_level++;
    
    // Clear variable registry for this function scope (simple approach)
    // and register struct parameters
    cg->variables.count = 0;
    for (int i = 0; i < func->data.function.param_count; i++) {
        ASTNode *param = func->data.function.params[i];
        if (param->data.param.param_type == TYPE_STRUCT && param->data.param.struct_name) {
            register_variable(cg, param->data.param.name, param->data.param.struct_name);
        }
    }
    
    // For variadic function, wrap variadic param as array if it isn't already
    if (func->data.function.is_variadic && func->data.function.param_count > 0) {
        ASTNode *variadic_param = func->data.function.params[func->data.function.param_count - 1];
        emit_indent(cg);
        emit(cg, "/* Variadic parameter %s is already an array */\n", 
             variadic_param->data.param.name);
    }
    
    // Handle optional parameters - check if defaults are needed
    for (int i = 0; i < func->data.function.param_count; i++) {
        ASTNode *param = func->data.function.params[i];
        if (param->data.param.is_optional && param->data.param.default_value && !func->data.function.is_variadic) {
            // Generate check: if parameter is undef/null, use default
            emit_indent(cg);
            emit(cg, "if (%s == NULL || %s->type == STRADA_UNDEF) {\n", 
                 param->data.param.name, param->data.param.name);
            cg->indent_level++;
            emit_indent(cg);
            
            // Generate default value assignment
            if (param->data.param.default_value->type == NODE_INT_LITERAL) {
                emit(cg, "%s = strada_new_int(%ld);\n", 
                     param->data.param.name,
                     param->data.param.default_value->data.int_val);
            } else if (param->data.param.default_value->type == NODE_NUM_LITERAL) {
                emit(cg, "%s = strada_new_num(%f);\n",
                     param->data.param.name,
                     param->data.param.default_value->data.num_val);
            } else if (param->data.param.default_value->type == NODE_STR_LITERAL) {
                emit(cg, "%s = strada_new_str(\"%s\");\n",
                     param->data.param.name,
                     param->data.param.default_value->data.str_val);
            }
            
            cg->indent_level--;
            emit_indent(cg);
            emit(cg, "}\n");
        }
    }
    
    // Generate body
    ASTNode *body = func->data.function.body;
    for (int i = 0; i < body->data.block.statement_count; i++) {
        gen_statement(cg, body->data.block.statements[i]);
    }

    // Pop function scope (emits cleanup for void/fall-through paths)
    if (!is_extern) {
        scope_pop_emit(cg);
    }

    cg->indent_level--;
    emit(cg, "}\n");
    
    // Close extern "C" block if needed
    if (is_extern && strcmp(func->data.function.name, "main") != 0) {
        emit(cg, "#ifdef __cplusplus\n}\n#endif\n");
    }
}

static void gen_block(CodeGen *cg, ASTNode *block) {
    emit(cg, "{\n");
    cg->indent_level++;
    scope_push(cg);

    for (int i = 0; i < block->data.block.statement_count; i++) {
        gen_statement(cg, block->data.block.statements[i]);
    }

    scope_pop_emit(cg);
    cg->indent_level--;
    emit_indent(cg);
    emit(cg, "}");
}

static void gen_statement(CodeGen *cg, ASTNode *stmt) {
    switch (stmt->type) {
        case NODE_VAR_DECL:
            emit_indent(cg);

            // Check if this is a struct type
            if (stmt->data.var_decl.var_type == TYPE_STRUCT && stmt->data.var_decl.struct_name) {
                // Register the variable's struct type
                register_variable(cg, stmt->data.var_decl.name, stmt->data.var_decl.struct_name);

                // Struct variable: allocate as pointer to struct
                emit(cg, "%s *%s", stmt->data.var_decl.struct_name, stmt->data.var_decl.name);
                if (stmt->data.var_decl.init) {
                    emit(cg, " = ");
                    gen_expression(cg, stmt->data.var_decl.init);
                } else {
                    // Allocate new struct
                    emit(cg, " = malloc(sizeof(%s))", stmt->data.var_decl.struct_name);
                }
                // Track struct variable for cleanup
                scope_track_var(cg, stmt->data.var_decl.name, 1);
            } else if (stmt->data.var_decl.var_type == TYPE_HASH || stmt->data.var_decl.sigil == '%') {
                // Hash variable: initialize as new hash
                emit(cg, "StradaValue *%s", stmt->data.var_decl.name);
                if (stmt->data.var_decl.init) {
                    // Check if init is empty () - if so, use hash_new regardless of literal type
                    if (stmt->data.var_decl.init->type == NODE_HASH_LITERAL &&
                        stmt->data.var_decl.init->data.hash_literal.pair_count == 0) {
                        emit(cg, " = strada_new_hash()");
                    } else if (stmt->data.var_decl.init->type == NODE_ARRAY_LITERAL &&
                               stmt->data.var_decl.init->data.array_literal.element_count == 0) {
                        emit(cg, " = strada_new_hash()");
                    } else {
                        emit(cg, " = ");
                        gen_expression(cg, stmt->data.var_decl.init);
                    }
                } else {
                    emit(cg, " = strada_new_hash()");
                }
                // Track owned values for cleanup
                if (expr_is_owned(stmt->data.var_decl.init)) {
                    scope_track_var(cg, stmt->data.var_decl.name, 0);
                }
            } else if (stmt->data.var_decl.var_type == TYPE_ARRAY || stmt->data.var_decl.sigil == '@') {
                // Array variable: initialize as new array
                emit(cg, "StradaValue *%s", stmt->data.var_decl.name);
                if (stmt->data.var_decl.init) {
                    // Check if init is empty () - if so, use array_new regardless of literal type
                    if (stmt->data.var_decl.init->type == NODE_HASH_LITERAL &&
                        stmt->data.var_decl.init->data.hash_literal.pair_count == 0) {
                        emit(cg, " = strada_new_array()");
                    } else if (stmt->data.var_decl.init->type == NODE_ARRAY_LITERAL &&
                               stmt->data.var_decl.init->data.array_literal.element_count == 0) {
                        emit(cg, " = strada_new_array()");
                    } else {
                        emit(cg, " = ");
                        gen_expression(cg, stmt->data.var_decl.init);
                    }
                } else {
                    emit(cg, " = strada_new_array()");
                }
                // Track owned values for cleanup
                if (expr_is_owned(stmt->data.var_decl.init)) {
                    scope_track_var(cg, stmt->data.var_decl.name, 0);
                }
            } else {
                // Regular StradaValue variable
                emit(cg, "StradaValue *%s", stmt->data.var_decl.name);
                if (stmt->data.var_decl.init) {
                    emit(cg, " = ");
                    gen_expression(cg, stmt->data.var_decl.init);
                } else {
                    emit(cg, " = strada_new_undef()");
                }
                // Track owned values for scope cleanup
                if (expr_is_owned(stmt->data.var_decl.init)) {
                    scope_track_var(cg, stmt->data.var_decl.name, 0);
                }
            }
            emit(cg, ";\n");
            break;
            
        case NODE_IF_STMT: {
            emit_indent(cg);
            emit(cg, "if (");
            gen_condition(cg, stmt->data.if_stmt.condition);
            emit(cg, ") ");
            gen_block(cg, stmt->data.if_stmt.then_block);

            for (int i = 0; i < stmt->data.if_stmt.elsif_count; i++) {
                emit(cg, " else if (");
                gen_condition(cg, stmt->data.if_stmt.elsif_conditions[i]);
                emit(cg, ") ");
                gen_block(cg, stmt->data.if_stmt.elsif_blocks[i]);
            }
            
            if (stmt->data.if_stmt.else_block) {
                emit(cg, " else ");
                gen_block(cg, stmt->data.if_stmt.else_block);
            }
            
            emit(cg, "\n");
            break;
        }
        
        case NODE_WHILE_STMT:
            emit_indent(cg);
            emit(cg, "while (");
            gen_condition(cg, stmt->data.while_stmt.condition);
            emit(cg, ") ");
            gen_block(cg, stmt->data.while_stmt.body);
            emit(cg, "\n");
            break;
            
        case NODE_FOR_STMT:
            emit_indent(cg);
            emit(cg, "for (");
            
            // Init
            if (stmt->data.for_stmt.init) {
                if (stmt->data.for_stmt.init->type == NODE_VAR_DECL) {
                    ASTNode *decl = stmt->data.for_stmt.init;
                    emit(cg, "StradaValue *%s = ", decl->data.var_decl.name);
                    if (decl->data.var_decl.init) {
                        gen_expression(cg, decl->data.var_decl.init);
                    } else {
                        emit(cg, "strada_new_undef()");
                    }
                } else {
                    gen_expression(cg, stmt->data.for_stmt.init);
                }
            }
            emit(cg, "; ");
            
            // Condition
            if (stmt->data.for_stmt.condition) {
                gen_condition(cg, stmt->data.for_stmt.condition);
            }
            emit(cg, "; ");
            
            // Update
            if (stmt->data.for_stmt.update) {
                gen_expression(cg, stmt->data.for_stmt.update);
            }
            
            emit(cg, ") ");
            gen_block(cg, stmt->data.for_stmt.body);
            emit(cg, "\n");
            break;
            
        case NODE_RETURN_STMT:
            if (stmt->data.return_stmt.value) {
                if (cg->current_func_is_main || cg->current_func_is_extern) {
                    // main/extern: emit cleanup, then return raw value
                    scope_emit_all_cleanup(cg, NULL);
                    emit_indent(cg);
                    emit(cg, "return ");
                    if (stmt->data.return_stmt.value->type == NODE_INT_LITERAL) {
                        emit(cg, "%lld", (long long)stmt->data.return_stmt.value->data.int_val);
                    } else {
                        gen_expression(cg, stmt->data.return_stmt.value);
                    }
                    emit(cg, ";\n");
                } else {
                    // Non-extern: save return value, cleanup all scopes, return
                    const char *skip_var = NULL;
                    if (stmt->data.return_stmt.value->type == NODE_VARIABLE) {
                        skip_var = stmt->data.return_stmt.value->data.variable.name;
                    }
                    if (skip_var) {
                        scope_emit_all_cleanup(cg, skip_var);
                        emit_indent(cg);
                        emit(cg, "return %s;\n", skip_var);
                    } else {
                        emit_indent(cg);
                        emit(cg, "{ StradaValue *__retval = ");
                        gen_expression(cg, stmt->data.return_stmt.value);
                        emit(cg, ";\n");
                        cg->indent_level++;
                        scope_emit_all_cleanup(cg, NULL);
                        emit_indent(cg);
                        emit(cg, "return __retval; }\n");
                        cg->indent_level--;
                    }
                }
            } else {
                // void return - cleanup all scopes
                scope_emit_all_cleanup(cg, NULL);
                emit_indent(cg);
                emit(cg, "return;\n");
            }
            break;
            
        case NODE_EXPR_STMT:
            emit_indent(cg);
            gen_expression(cg, stmt->data.expr_stmt.expr);
            emit(cg, ";\n");
            break;
            
        case NODE_BLOCK:
            emit_indent(cg);
            gen_block(cg, stmt);
            emit(cg, "\n");
            break;
            
        case NODE_LAST_STMT:
            emit_indent(cg);
            emit(cg, "break;\n");
            break;
            
        case NODE_NEXT_STMT:
            emit_indent(cg);
            emit(cg, "continue;\n");
            break;

        case NODE_TRY_CATCH: {
            emit_indent(cg);
            emit(cg, "if (setjmp(*STRADA_TRY_PUSH()) == 0) {\n");
            cg->indent_level++;
            // Generate try block statements
            ASTNode *try_block = stmt->data.try_catch.try_block;
            for (int i = 0; i < try_block->data.block.statement_count; i++) {
                gen_statement(cg, try_block->data.block.statements[i]);
            }
            emit_indent(cg);
            emit(cg, "STRADA_TRY_POP();\n");
            cg->indent_level--;
            emit_indent(cg);
            emit(cg, "} else {\n");
            cg->indent_level++;
            emit_indent(cg);
            emit(cg, "STRADA_TRY_POP();\n");
            emit_indent(cg);
            emit(cg, "StradaValue *%s = strada_get_exception();\n", stmt->data.try_catch.catch_var);
            // Generate catch block statements
            ASTNode *catch_block = stmt->data.try_catch.catch_block;
            for (int i = 0; i < catch_block->data.block.statement_count; i++) {
                gen_statement(cg, catch_block->data.block.statements[i]);
            }
            cg->indent_level--;
            emit_indent(cg);
            emit(cg, "}\n");
            break;
        }

        case NODE_THROW:
            emit_indent(cg);
            emit(cg, "strada_throw_value(");
            gen_expression(cg, stmt->data.throw_stmt.expr);
            emit(cg, ");\n");
            break;

        case NODE_LABEL:
            emit(cg, "%s:;\n", stmt->data.label.name);
            break;

        case NODE_GOTO:
            emit_indent(cg);
            emit(cg, "goto %s;\n", stmt->data.goto_stmt.target);
            break;

        default:
            break;
    }
}

static void gen_expression(CodeGen *cg, ASTNode *expr) {
    switch (expr->type) {
        case NODE_INT_LITERAL:
            if (cg->current_func_is_extern) {
                emit(cg, "%lld", (long long)expr->data.int_val);
            } else {
                emit(cg, "strada_new_int(%lld)", (long long)expr->data.int_val);
            }
            break;
            
        case NODE_NUM_LITERAL:
            if (cg->current_func_is_extern) {
                emit(cg, "%g", expr->data.num_val);
            } else {
                emit(cg, "strada_new_num(%g)", expr->data.num_val);
            }
            break;
            
        case NODE_STR_LITERAL: {
            if (cg->current_func_is_extern) {
                emit(cg, "\"");
                for (const char *p = expr->data.str_val; *p; p++) {
                    switch (*p) {
                        case '\n': emit(cg, "\\n"); break;
                        case '\t': emit(cg, "\\t"); break;
                        case '\r': emit(cg, "\\r"); break;
                        case '\"': emit(cg, "\\\""); break;
                        case '\\': emit(cg, "\\\\"); break;
                        default: emit(cg, "%c", *p); break;
                    }
                }
                emit(cg, "\"");
            } else {
                emit(cg, "strada_new_str(\"");
                for (const char *p = expr->data.str_val; *p; p++) {
                    switch (*p) {
                        case '\n': emit(cg, "\\n"); break;
                        case '\t': emit(cg, "\\t"); break;
                        case '\r': emit(cg, "\\r"); break;
                        case '\"': emit(cg, "\\\""); break;
                        case '\\': emit(cg, "\\\\"); break;
                        default: emit(cg, "%c", *p); break;
                    }
                }
                emit(cg, "\")");
            }
            break;
        }
        
        case NODE_ARRAY_LITERAL: {
            // Empty array: strada_array_new()
            if (expr->data.array_literal.element_count == 0) {
                emit(cg, "strada_array_new()");
            } else {
                // Non-empty array - build it
                emit(cg, "strada_array_new()");  // TODO: Add elements
            }
            break;
        }
        
        case NODE_HASH_LITERAL: {
            // Empty hash: strada_hash_new()
            if (expr->data.hash_literal.pair_count == 0) {
                emit(cg, "strada_hash_new()");
            } else {
                // Non-empty hash - build it
                emit(cg, "strada_hash_new()");  // TODO: Add pairs
            }
            break;
        }
        
        case NODE_VARIABLE:
            emit(cg, "%s", expr->data.variable.name);
            break;
            
        case NODE_BINARY_OP: {
            const char *op = expr->data.binary_op.op;
            
            // For extern functions, use raw C arithmetic
            if (cg->current_func_is_extern) {
                if (strcmp(op, "+") == 0 || strcmp(op, "-") == 0 || 
                    strcmp(op, "*") == 0 || strcmp(op, "/") == 0 ||
                    strcmp(op, "%") == 0) {
                    emit(cg, "(");
                    gen_expression(cg, expr->data.binary_op.left);
                    emit(cg, " %s ", op);
                    gen_expression(cg, expr->data.binary_op.right);
                    emit(cg, ")");
                } else if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
                           strcmp(op, "<") == 0 || strcmp(op, ">") == 0 ||
                           strcmp(op, "<=") == 0 || strcmp(op, ">=") == 0) {
                    emit(cg, "(");
                    gen_expression(cg, expr->data.binary_op.left);
                    emit(cg, " %s ", op);
                    gen_expression(cg, expr->data.binary_op.right);
                    emit(cg, ")");
                } else if (strcmp(op, "&&") == 0 || strcmp(op, "||") == 0) {
                    emit(cg, "(");
                    gen_expression(cg, expr->data.binary_op.left);
                    emit(cg, " %s ", op);
                    gen_expression(cg, expr->data.binary_op.right);
                    emit(cg, ")");
                } else {
                    // Fallback for other ops
                    gen_expression(cg, expr->data.binary_op.left);
                    emit(cg, " %s ", op);
                    gen_expression(cg, expr->data.binary_op.right);
                }
            } else if (strcmp(op, "+") == 0) {
                emit(cg, "strada_new_num(");
                gen_as_num(cg, expr->data.binary_op.left);
                emit(cg, " + ");
                gen_as_num(cg, expr->data.binary_op.right);
                emit(cg, ")");
            } else if (strcmp(op, "-") == 0) {
                emit(cg, "strada_new_num(");
                gen_as_num(cg, expr->data.binary_op.left);
                emit(cg, " - ");
                gen_as_num(cg, expr->data.binary_op.right);
                emit(cg, ")");
            } else if (strcmp(op, "*") == 0) {
                emit(cg, "strada_new_num(");
                gen_as_num(cg, expr->data.binary_op.left);
                emit(cg, " * ");
                gen_as_num(cg, expr->data.binary_op.right);
                emit(cg, ")");
            } else if (strcmp(op, "/") == 0) {
                emit(cg, "strada_new_num(");
                gen_as_num(cg, expr->data.binary_op.left);
                emit(cg, " / ");
                gen_as_num(cg, expr->data.binary_op.right);
                emit(cg, ")");
            } else if (strcmp(op, ".") == 0) {
                /* String concat with cleanup of owned operands */
                int lo = expr_is_owned(expr->data.binary_op.left);
                int ro = expr_is_owned(expr->data.binary_op.right);
                if (lo && ro) {
                    emit(cg, "({ StradaValue *__cr = ");
                    gen_expression(cg, expr->data.binary_op.right);
                    emit(cg, "; StradaValue *__cc = strada_concat_inplace(");
                    gen_expression(cg, expr->data.binary_op.left);
                    emit(cg, ", __cr); strada_decref(__cr); __cc; })");
                } else if (lo) {
                    emit(cg, "strada_concat_inplace(");
                    gen_expression(cg, expr->data.binary_op.left);
                    emit(cg, ", ");
                    gen_expression(cg, expr->data.binary_op.right);
                    emit(cg, ")");
                } else if (ro) {
                    emit(cg, "({ StradaValue *__cr = ");
                    gen_expression(cg, expr->data.binary_op.right);
                    emit(cg, "; StradaValue *__cc = strada_concat_sv(");
                    gen_expression(cg, expr->data.binary_op.left);
                    emit(cg, ", __cr); strada_decref(__cr); __cc; })");
                } else {
                    emit(cg, "strada_concat_sv(");
                    gen_expression(cg, expr->data.binary_op.left);
                    emit(cg, ", ");
                    gen_expression(cg, expr->data.binary_op.right);
                    emit(cg, ")");
                }
            } else if (strcmp(op, "==") == 0) {
                emit(cg, "strada_new_int(");
                gen_as_num(cg, expr->data.binary_op.left);
                emit(cg, " == ");
                gen_as_num(cg, expr->data.binary_op.right);
                emit(cg, ")");
            } else if (strcmp(op, "!=") == 0) {
                emit(cg, "strada_new_int(");
                gen_as_num(cg, expr->data.binary_op.left);
                emit(cg, " != ");
                gen_as_num(cg, expr->data.binary_op.right);
                emit(cg, ")");
            } else if (strcmp(op, "eq") == 0) {
                gen_str_cmp(cg, expr->data.binary_op.left, expr->data.binary_op.right, "==");
            } else if (strcmp(op, "ne") == 0) {
                gen_str_cmp(cg, expr->data.binary_op.left, expr->data.binary_op.right, "!=");
            } else if (strcmp(op, "lt") == 0) {
                gen_str_cmp(cg, expr->data.binary_op.left, expr->data.binary_op.right, "<");
            } else if (strcmp(op, "gt") == 0) {
                gen_str_cmp(cg, expr->data.binary_op.left, expr->data.binary_op.right, ">");
            } else if (strcmp(op, "le") == 0) {
                gen_str_cmp(cg, expr->data.binary_op.left, expr->data.binary_op.right, "<=");
            } else if (strcmp(op, "ge") == 0) {
                gen_str_cmp(cg, expr->data.binary_op.left, expr->data.binary_op.right, ">=");
            } else if (strcmp(op, "<") == 0) {
                emit(cg, "strada_new_int(");
                gen_as_num(cg, expr->data.binary_op.left);
                emit(cg, " < ");
                gen_as_num(cg, expr->data.binary_op.right);
                emit(cg, ")");
            } else if (strcmp(op, ">") == 0) {
                emit(cg, "strada_new_int(");
                gen_as_num(cg, expr->data.binary_op.left);
                emit(cg, " > ");
                gen_as_num(cg, expr->data.binary_op.right);
                emit(cg, ")");
            } else if (strcmp(op, "<=") == 0) {
                emit(cg, "strada_new_int(");
                gen_as_num(cg, expr->data.binary_op.left);
                emit(cg, " <= ");
                gen_as_num(cg, expr->data.binary_op.right);
                emit(cg, ")");
            } else if (strcmp(op, ">=") == 0) {
                emit(cg, "strada_new_int(");
                gen_as_num(cg, expr->data.binary_op.left);
                emit(cg, " >= ");
                gen_as_num(cg, expr->data.binary_op.right);
                emit(cg, ")");
            } else if (strcmp(op, "&&") == 0) {
                // Logical AND with cleanup for owned operands
                emit(cg, "strada_new_int(");
                gen_condition(cg, expr->data.binary_op.left);
                emit(cg, " && ");
                gen_condition(cg, expr->data.binary_op.right);
                emit(cg, ")");
            } else if (strcmp(op, "||") == 0) {
                // Logical OR with cleanup for owned operands
                emit(cg, "strada_new_int(");
                gen_condition(cg, expr->data.binary_op.left);
                emit(cg, " || ");
                gen_condition(cg, expr->data.binary_op.right);
                emit(cg, ")");
            }
            break;
        }

        case NODE_UNARY_OP: {
            if (strcmp(expr->data.unary_op.op, "-") == 0) {
                emit(cg, "strada_new_num(-");
                gen_as_num(cg, expr->data.unary_op.operand);
                emit(cg, ")");
            } else if (strcmp(expr->data.unary_op.op, "!") == 0) {
                emit(cg, "strada_new_int(!");
                gen_condition(cg, expr->data.unary_op.operand);
                emit(cg, ")");
            }
            break;
        }
        
        case NODE_ASSIGN: {
            // Check if target is a member access (struct field assignment)
            if (expr->data.assign.target->type == NODE_MEMBER_ACCESS) {
                ASTNode *ma = expr->data.assign.target;
                
                // Get the struct type of the object
                const char *struct_type = NULL;
                if (ma->data.member_access.object->type == NODE_VARIABLE) {
                    struct_type = lookup_variable_struct_type(cg, 
                        ma->data.member_access.object->data.variable.name);
                }
                
                // Look up the field type
                DataType field_type = TYPE_INT;  // Default
                if (struct_type) {
                    field_type = lookup_field_type(cg, struct_type, 
                        ma->data.member_access.field_name);
                }
                
                if (strcmp(expr->data.assign.op, "=") == 0) {
                    // Generate appropriate assignment based on field type
                    switch (field_type) {
                        case TYPE_STR:
                            // For string fields, use strcpy (capture strada_to_str result and free)
                            emit(cg, "({ char *__s = strada_to_str(");
                            gen_expression(cg, expr->data.assign.value);
                            emit(cg, "); strcpy(");
                            gen_expression(cg, ma->data.member_access.object);
                            emit(cg, "->%s, __s); free(__s); })");
                            break;
                        case TYPE_NUM:
                        case TYPE_FLOAT:
                            // For floating point fields
                            gen_expression(cg, ma->data.member_access.object);
                            emit(cg, "->%s", ma->data.member_access.field_name);
                            emit(cg, " = strada_to_num(");
                            gen_expression(cg, expr->data.assign.value);
                            emit(cg, ")");
                            break;
                        case TYPE_I64:
                        case TYPE_LONG:
                            gen_expression(cg, ma->data.member_access.object);
                            emit(cg, "->%s", ma->data.member_access.field_name);
                            emit(cg, " = (int64_t)strada_to_int(");
                            gen_expression(cg, expr->data.assign.value);
                            emit(cg, ")");
                            break;
                        case TYPE_U64:
                        case TYPE_ULONG:
                        case TYPE_SIZE:
                            gen_expression(cg, ma->data.member_access.object);
                            emit(cg, "->%s", ma->data.member_access.field_name);
                            emit(cg, " = (uint64_t)strada_to_int(");
                            gen_expression(cg, expr->data.assign.value);
                            emit(cg, ")");
                            break;
                        case TYPE_PTR:
                            gen_expression(cg, ma->data.member_access.object);
                            emit(cg, "->%s", ma->data.member_access.field_name);
                            emit(cg, " = (void*)strada_to_int(");
                            gen_expression(cg, expr->data.assign.value);
                            emit(cg, ")");
                            break;
                        case TYPE_BOOL:
                            gen_expression(cg, ma->data.member_access.object);
                            emit(cg, "->%s", ma->data.member_access.field_name);
                            emit(cg, " = (bool)strada_to_int(");
                            gen_expression(cg, expr->data.assign.value);
                            emit(cg, ")");
                            break;
                        case TYPE_CHAR:
                        case TYPE_I8:
                            gen_expression(cg, ma->data.member_access.object);
                            emit(cg, "->%s", ma->data.member_access.field_name);
                            emit(cg, " = (int8_t)strada_to_int(");
                            gen_expression(cg, expr->data.assign.value);
                            emit(cg, ")");
                            break;
                        case TYPE_UCHAR:
                        case TYPE_U8:
                            gen_expression(cg, ma->data.member_access.object);
                            emit(cg, "->%s", ma->data.member_access.field_name);
                            emit(cg, " = (uint8_t)strada_to_int(");
                            gen_expression(cg, expr->data.assign.value);
                            emit(cg, ")");
                            break;
                        case TYPE_SHORT:
                        case TYPE_I16:
                            gen_expression(cg, ma->data.member_access.object);
                            emit(cg, "->%s", ma->data.member_access.field_name);
                            emit(cg, " = (int16_t)strada_to_int(");
                            gen_expression(cg, expr->data.assign.value);
                            emit(cg, ")");
                            break;
                        case TYPE_USHORT:
                        case TYPE_U16:
                            gen_expression(cg, ma->data.member_access.object);
                            emit(cg, "->%s", ma->data.member_access.field_name);
                            emit(cg, " = (uint16_t)strada_to_int(");
                            gen_expression(cg, expr->data.assign.value);
                            emit(cg, ")");
                            break;
                        case TYPE_UINT:
                        case TYPE_U32:
                            gen_expression(cg, ma->data.member_access.object);
                            emit(cg, "->%s", ma->data.member_access.field_name);
                            emit(cg, " = (uint32_t)strada_to_int(");
                            gen_expression(cg, expr->data.assign.value);
                            emit(cg, ")");
                            break;
                        case TYPE_FUNC:
                            // Function pointer assignment - direct assignment
                            gen_expression(cg, ma->data.member_access.object);
                            emit(cg, "->%s", ma->data.member_access.field_name);
                            emit(cg, " = ");
                            gen_expression(cg, expr->data.assign.value);
                            break;
                        default:  // TYPE_INT, TYPE_I32
                            gen_expression(cg, ma->data.member_access.object);
                            emit(cg, "->%s", ma->data.member_access.field_name);
                            emit(cg, " = strada_to_int(");
                            gen_expression(cg, expr->data.assign.value);
                            emit(cg, ")");
                            break;
                    }
                }
            } else if (expr->data.assign.target->type == NODE_HASH_ACCESS) {
                // Hash element assignment: $hash{key} = value
                ASTNode *ha = expr->data.assign.target;
                if (ha->data.subscript.index->type == NODE_STR_LITERAL &&
                    str_is_safe_literal(ha->data.subscript.index->data.str_val)) {
                    emit(cg, "strada_hash_set(");
                    gen_expression(cg, ha->data.subscript.array);
                    emit(cg, "->value.hv, \"%s\", ", ha->data.subscript.index->data.str_val);
                    gen_expression(cg, expr->data.assign.value);
                    emit(cg, ")");
                } else {
                    // Dynamic key: capture strada_to_str result and free after use
                    emit(cg, "({ char *__k = strada_to_str(");
                    gen_expression(cg, ha->data.subscript.index);
                    emit(cg, "); strada_hash_set(");
                    gen_expression(cg, ha->data.subscript.array);
                    emit(cg, "->value.hv, __k, ");
                    gen_expression(cg, expr->data.assign.value);
                    emit(cg, "); free(__k); })");
                }
            } else if (expr->data.assign.target->type == NODE_SUBSCRIPT) {
                // Array element assignment: $array[index] = value
                ASTNode *sub = expr->data.assign.target;
                emit(cg, "strada_array_set(");
                gen_expression(cg, sub->data.subscript.array);
                emit(cg, "->value.av, strada_to_int(");
                gen_expression(cg, sub->data.subscript.index);
                emit(cg, "), ");
                gen_expression(cg, expr->data.assign.value);
                emit(cg, ")");
            } else if (expr->data.assign.target->type == NODE_DEREF_HASH) {
                // $ref->{key} = value
                ASTNode *dh = expr->data.assign.target;
                if (dh->data.deref_hash.key->type == NODE_STR_LITERAL &&
                    str_is_safe_literal(dh->data.deref_hash.key->data.str_val)) {
                    emit(cg, "strada_hash_set(strada_deref_hash(");
                    gen_expression(cg, dh->data.deref_hash.ref);
                    emit(cg, "), \"%s\", ", dh->data.deref_hash.key->data.str_val);
                    gen_expression(cg, expr->data.assign.value);
                    emit(cg, ")");
                } else {
                    // Dynamic key: capture and free
                    emit(cg, "({ char *__k = strada_to_str(");
                    gen_expression(cg, dh->data.deref_hash.key);
                    emit(cg, "); strada_hash_set(strada_deref_hash(");
                    gen_expression(cg, dh->data.deref_hash.ref);
                    emit(cg, "), __k, ");
                    gen_expression(cg, expr->data.assign.value);
                    emit(cg, "); free(__k); })");
                }
            } else if (expr->data.assign.target->type == NODE_DEREF_ARRAY) {
                // $ref->[index] = value
                ASTNode *da = expr->data.assign.target;
                emit(cg, "strada_array_set(strada_deref_array(");
                gen_expression(cg, da->data.deref_array.ref);
                emit(cg, "), strada_to_int(");
                gen_expression(cg, da->data.deref_array.index);
                emit(cg, "), ");
                gen_expression(cg, expr->data.assign.value);
                emit(cg, ")");
            } else {
                // Regular variable assignment
                gen_expression(cg, expr->data.assign.target);
                emit(cg, " = ");

                if (strcmp(expr->data.assign.op, "=") == 0) {
                    gen_expression(cg, expr->data.assign.value);
                } else if (strcmp(expr->data.assign.op, "+=") == 0) {
                    emit(cg, "strada_new_num(strada_to_num(");
                    gen_expression(cg, expr->data.assign.target);
                    emit(cg, ") + strada_to_num(");
                    gen_expression(cg, expr->data.assign.value);
                    emit(cg, "))");
                } else if (strcmp(expr->data.assign.op, "-=") == 0) {
                    emit(cg, "strada_new_num(strada_to_num(");
                    gen_expression(cg, expr->data.assign.target);
                    emit(cg, ") - strada_to_num(");
                    gen_expression(cg, expr->data.assign.value);
                    emit(cg, "))");
                } else if (strcmp(expr->data.assign.op, ".=") == 0) {
                    /* Fast concat directly on StradaValues */
                    emit(cg, "strada_concat_sv(");
                    gen_expression(cg, expr->data.assign.target);
                    emit(cg, ", ");
                    gen_expression(cg, expr->data.assign.value);
                    emit(cg, ")");
                }
            }
            break;
        }
        
        case NODE_CALL: {
            // Built-in functions
            if (strcmp(expr->data.call.name, "say") == 0) {
                if (expr->data.call.arg_count > 0 && expr_is_owned(expr->data.call.args[0])) {
                    emit(cg, "({ StradaValue *__say_tmp = ");
                    gen_expression(cg, expr->data.call.args[0]);
                    emit(cg, "; strada_say(__say_tmp); strada_decref(__say_tmp); })");
                } else {
                    emit(cg, "strada_say(");
                    if (expr->data.call.arg_count > 0) gen_expression(cg, expr->data.call.args[0]);
                    emit(cg, ")");
                }
            } else if (strcmp(expr->data.call.name, "print") == 0) {
                if (expr->data.call.arg_count > 0 && expr_is_owned(expr->data.call.args[0])) {
                    emit(cg, "({ StradaValue *__prt_tmp = ");
                    gen_expression(cg, expr->data.call.args[0]);
                    emit(cg, "; strada_print(__prt_tmp); strada_decref(__prt_tmp); })");
                } else {
                    emit(cg, "strada_print(");
                    if (expr->data.call.arg_count > 0) gen_expression(cg, expr->data.call.args[0]);
                    emit(cg, ")");
                }
            } else if (strcmp(expr->data.call.name, "dumper") == 0) {
                if (expr->data.call.arg_count > 0 && expr_is_owned(expr->data.call.args[0])) {
                    emit(cg, "({ StradaValue *__dmp_tmp = ");
                    gen_expression(cg, expr->data.call.args[0]);
                    emit(cg, "; strada_dumper(__dmp_tmp); strada_decref(__dmp_tmp); })");
                } else {
                    emit(cg, "strada_dumper(");
                    if (expr->data.call.arg_count > 0) gen_expression(cg, expr->data.call.args[0]);
                    emit(cg, ")");
                }
            } else if (strcmp(expr->data.call.name, "printf") == 0) {
                // Capture all strada_to_str results, call, free all
                emit(cg, "({ ");
                for (int i = 0; i < expr->data.call.arg_count; i++) {
                    emit(cg, "char *__s%d = strada_to_str(", i);
                    gen_expression(cg, expr->data.call.args[i]);
                    emit(cg, "); ");
                }
                emit(cg, "strada_printf(__s0");
                for (int i = 1; i < expr->data.call.arg_count; i++) {
                    emit(cg, ", __s%d", i);
                }
                emit(cg, "); ");
                for (int i = 0; i < expr->data.call.arg_count; i++) {
                    emit(cg, "free(__s%d); ", i);
                }
                emit(cg, "})");
            } else if (strcmp(expr->data.call.name, "sprintf") == 0) {
                emit(cg, "({ ");
                for (int i = 0; i < expr->data.call.arg_count; i++) {
                    emit(cg, "char *__s%d = strada_to_str(", i);
                    gen_expression(cg, expr->data.call.args[i]);
                    emit(cg, "); ");
                }
                emit(cg, "StradaValue *__r = strada_sprintf(__s0");
                for (int i = 1; i < expr->data.call.arg_count; i++) {
                    emit(cg, ", __s%d", i);
                }
                emit(cg, "); ");
                for (int i = 0; i < expr->data.call.arg_count; i++) {
                    emit(cg, "free(__s%d); ", i);
                }
                emit(cg, "__r; })");
            } else if (strcmp(expr->data.call.name, "warn") == 0) {
                emit(cg, "({ ");
                for (int i = 0; i < expr->data.call.arg_count; i++) {
                    emit(cg, "char *__s%d = strada_to_str(", i);
                    gen_expression(cg, expr->data.call.args[i]);
                    emit(cg, "); ");
                }
                emit(cg, "strada_warn(__s0");
                for (int i = 1; i < expr->data.call.arg_count; i++) {
                    emit(cg, ", __s%d", i);
                }
                emit(cg, "); ");
                for (int i = 0; i < expr->data.call.arg_count; i++) {
                    emit(cg, "free(__s%d); ", i);
                }
                emit(cg, "})");
            } else if (strcmp(expr->data.call.name, "defined") == 0) {
                emit(cg, "strada_defined(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ")");
            } else if (strcmp(expr->data.call.name, "length") == 0) {
                emit(cg, "({ char *__s = strada_to_str(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "); StradaValue *__r = strada_new_int(strada_length(__s)); free(__s); __r; })");
            } else if (strcmp(expr->data.call.name, "bytes") == 0) {
                emit(cg, "({ char *__s = strada_to_str(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "); StradaValue *__r = strada_new_int(strada_bytes(__s)); free(__s); __r; })");
            } else if (strcmp(expr->data.call.name, "size") == 0) {
                // Handle both direct arrays and references to arrays
                emit(cg, "strada_new_int(strada_array_length(strada_deref_array(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ")))");
            } else if (strcmp(expr->data.call.name, "push") == 0) {
                // Handle both direct arrays and references to arrays
                emit(cg, "(strada_array_push(strada_deref_array(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "), ");
                gen_expression(cg, expr->data.call.args[1]);
                emit(cg, "), strada_new_undef())");
            } else if (strcmp(expr->data.call.name, "pop") == 0) {
                emit(cg, "strada_array_pop(strada_deref_array(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "))");
            } else if (strcmp(expr->data.call.name, "shift") == 0) {
                emit(cg, "strada_array_shift(strada_deref_array(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "))");
            } else if (strcmp(expr->data.call.name, "unshift") == 0) {
                emit(cg, "(strada_array_unshift(strada_deref_array(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "), ");
                gen_expression(cg, expr->data.call.args[1]);
                emit(cg, "), strada_new_undef())");
            } else if (strcmp(expr->data.call.name, "die") == 0) {
                // die doesn't return, but free for correctness
                emit(cg, "({ char *__s = strada_to_str(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "); strada_die(__s); free(__s); })");
            } else if (strcmp(expr->data.call.name, "keys") == 0) {
                emit(cg, "strada_new_array_from_av(strada_hash_keys(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "->value.hv))");
            } else if (strcmp(expr->data.call.name, "values") == 0) {
                emit(cg, "strada_new_array_from_av(strada_hash_values(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "->value.hv))");
            } else if (strcmp(expr->data.call.name, "hash_new") == 0) {
                emit(cg, "strada_new_hash()");
            } else if (strcmp(expr->data.call.name, "hash_set") == 0) {
                emit(cg, "({ char *__k = strada_to_str(");
                gen_expression(cg, expr->data.call.args[1]);
                emit(cg, "); strada_hash_set(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "->value.hv, __k, ");
                gen_expression(cg, expr->data.call.args[2]);
                emit(cg, "); free(__k); })");
            } else if (strcmp(expr->data.call.name, "hash_get") == 0) {
                emit(cg, "({ char *__k = strada_to_str(");
                gen_expression(cg, expr->data.call.args[1]);
                emit(cg, "); StradaValue *__r = strada_hash_get(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "->value.hv, __k); free(__k); __r; })");
            } else if (strcmp(expr->data.call.name, "exists") == 0) {
                // exists($hash{key}) or exists($hash, "key")
                if (expr->data.call.args[0]->type == NODE_HASH_ACCESS) {
                    ASTNode *ha = expr->data.call.args[0];
                    emit(cg, "({ char *__k = strada_to_str(");
                    gen_expression(cg, ha->data.subscript.index);
                    emit(cg, "); StradaValue *__r = strada_new_int(strada_hash_exists(");
                    gen_expression(cg, ha->data.subscript.array);
                    emit(cg, "->value.hv, __k)); free(__k); __r; })");
                } else {
                    emit(cg, "({ char *__k = strada_to_str(");
                    gen_expression(cg, expr->data.call.args[1]);
                    emit(cg, "); StradaValue *__r = strada_new_int(strada_hash_exists(");
                    gen_expression(cg, expr->data.call.args[0]);
                    emit(cg, "->value.hv, __k)); free(__k); __r; })");
                }
            } else if (strcmp(expr->data.call.name, "delete") == 0) {
                // delete($hash{key}) or delete($hash, "key")
                if (expr->data.call.args[0]->type == NODE_HASH_ACCESS) {
                    ASTNode *ha = expr->data.call.args[0];
                    emit(cg, "({ char *__k = strada_to_str(");
                    gen_expression(cg, ha->data.subscript.index);
                    emit(cg, "); strada_hash_delete(");
                    gen_expression(cg, ha->data.subscript.array);
                    emit(cg, "->value.hv, __k); free(__k); strada_new_undef(); })");
                } else {
                    emit(cg, "({ char *__k = strada_to_str(");
                    gen_expression(cg, expr->data.call.args[1]);
                    emit(cg, "); strada_hash_delete(");
                    gen_expression(cg, expr->data.call.args[0]);
                    emit(cg, "->value.hv, __k); free(__k); strada_new_undef(); })");
                }
            } else if (strcmp(expr->data.call.name, "char_at") == 0) {
                /* Fast char access by byte index - returns int char code */
                emit(cg, "strada_char_at(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ", ");
                gen_expression(cg, expr->data.call.args[1]);
                emit(cg, ")");
            } else if (strcmp(expr->data.call.name, "substr") == 0) {
                emit(cg, "strada_substr(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ", strada_to_int(");
                gen_expression(cg, expr->data.call.args[1]);
                emit(cg, "), ");
                if (expr->data.call.arg_count > 2) {
                    emit(cg, "strada_to_int(");
                    gen_expression(cg, expr->data.call.args[2]);
                    emit(cg, ")");
                } else {
                    emit(cg, "-1");
                }
                emit(cg, ")");
            } else if (strcmp(expr->data.call.name, "substr_bytes") == 0) {
                emit(cg, "strada_substr_bytes(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ", strada_to_int(");
                gen_expression(cg, expr->data.call.args[1]);
                emit(cg, "), ");
                if (expr->data.call.arg_count > 2) {
                    emit(cg, "strada_to_int(");
                    gen_expression(cg, expr->data.call.args[2]);
                    emit(cg, ")");
                } else {
                    emit(cg, "-1");
                }
                emit(cg, ")");
            } else if (strcmp(expr->data.call.name, "upper") == 0 || strcmp(expr->data.call.name, "uc") == 0) {
                emit(cg, "({ char *__s = strada_to_str(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "); StradaValue *__r = strada_new_str_take(strada_upper(__s)); free(__s); __r; })");
            } else if (strcmp(expr->data.call.name, "lower") == 0 || strcmp(expr->data.call.name, "lc") == 0) {
                emit(cg, "({ char *__s = strada_to_str(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "); StradaValue *__r = strada_new_str_take(strada_lower(__s)); free(__s); __r; })");
            } else if (strcmp(expr->data.call.name, "ucfirst") == 0) {
                emit(cg, "({ char *__s = strada_to_str(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "); StradaValue *__r = strada_new_str_take(strada_ucfirst(__s)); free(__s); __r; })");
            } else if (strcmp(expr->data.call.name, "lcfirst") == 0) {
                emit(cg, "({ char *__s = strada_to_str(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "); StradaValue *__r = strada_new_str_take(strada_lcfirst(__s)); free(__s); __r; })");
            } else if (strcmp(expr->data.call.name, "trim") == 0) {
                emit(cg, "({ char *__s = strada_to_str(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "); StradaValue *__r = strada_new_str_take(strada_trim(__s)); free(__s); __r; })");
            } else if (strcmp(expr->data.call.name, "ltrim") == 0) {
                emit(cg, "({ char *__s = strada_to_str(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "); StradaValue *__r = strada_new_str_take(strada_ltrim(__s)); free(__s); __r; })");
            } else if (strcmp(expr->data.call.name, "rtrim") == 0) {
                emit(cg, "({ char *__s = strada_to_str(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "); StradaValue *__r = strada_new_str_take(strada_rtrim(__s)); free(__s); __r; })");
            } else if (strcmp(expr->data.call.name, "reverse") == 0) {
                emit(cg, "({ char *__s = strada_to_str(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "); StradaValue *__r = strada_new_str_take(strada_reverse(__s)); free(__s); __r; })");
            } else if (strcmp(expr->data.call.name, "repeat") == 0) {
                emit(cg, "({ char *__s = strada_to_str(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "); StradaValue *__r = strada_new_str_take(strada_repeat(__s, strada_to_int(");
                gen_expression(cg, expr->data.call.args[1]);
                emit(cg, "))); free(__s); __r; })");
            } else if (strcmp(expr->data.call.name, "chr") == 0) {
                emit(cg, "strada_new_str_take(strada_chr(strada_to_int(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ")))");
            } else if (strcmp(expr->data.call.name, "ord") == 0) {
                emit(cg, "({ char *__s = strada_to_str(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "); StradaValue *__r = strada_new_int(strada_ord(__s)); free(__s); __r; })");
            } else if (strcmp(expr->data.call.name, "chomp") == 0) {
                emit(cg, "({ char *__s = strada_to_str(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "); StradaValue *__r = strada_new_str_take(strada_chomp(__s)); free(__s); __r; })");
            } else if (strcmp(expr->data.call.name, "chop") == 0) {
                emit(cg, "({ char *__s = strada_to_str(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "); StradaValue *__r = strada_new_str_take(strada_chop(__s)); free(__s); __r; })");
            } else if (strcmp(expr->data.call.name, "index") == 0) {
                emit(cg, "({ char *__s0 = strada_to_str(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "); char *__s1 = strada_to_str(");
                gen_expression(cg, expr->data.call.args[1]);
                emit(cg, "); StradaValue *__r = strada_new_int(strada_index(__s0, __s1)); free(__s0); free(__s1); __r; })");
            } else if (strcmp(expr->data.call.name, "rindex") == 0) {
                emit(cg, "({ char *__s0 = strada_to_str(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "); char *__s1 = strada_to_str(");
                gen_expression(cg, expr->data.call.args[1]);
                emit(cg, "); StradaValue *__r = strada_new_int(strada_rindex(__s0, __s1)); free(__s0); free(__s1); __r; })");
            } else if (strcmp(expr->data.call.name, "join") == 0) {
                emit(cg, "({ char *__s = strada_to_str(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "); StradaValue *__r = strada_new_str_take(strada_join(__s, strada_deref_array(");
                gen_expression(cg, expr->data.call.args[1]);
                emit(cg, "))); free(__s); __r; })");
            } else if (strcmp(expr->data.call.name, "typeof") == 0) {
                emit(cg, "strada_new_str(strada_typeof(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "))");
            } else if (strcmp(expr->data.call.name, "cast_int") == 0) {
                emit(cg, "strada_int(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ")");
            } else if (strcmp(expr->data.call.name, "cast_num") == 0) {
                emit(cg, "strada_num(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ")");
            } else if (strcmp(expr->data.call.name, "cast_str") == 0) {
                emit(cg, "strada_str(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ")");
            } else if (strcmp(expr->data.call.name, "refto") == 0) {
                emit(cg, "strada_ref_create(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ")");
            } else if (strcmp(expr->data.call.name, "deref") == 0) {
                emit(cg, "strada_ref_deref(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ")");
            } else if (strcmp(expr->data.call.name, "deref_array") == 0 || 
                       strcmp(expr->data.call.name, "array_from_ref") == 0) {
                // Convert array reference to a new array (copy)
                emit(cg, "strada_array_from_ref(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ")");
            } else if (strcmp(expr->data.call.name, "deref_hash") == 0 ||
                       strcmp(expr->data.call.name, "hash_from_ref") == 0) {
                // Convert hash reference to a new hash (copy)
                emit(cg, "strada_hash_from_ref(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ")");
            } else if (strcmp(expr->data.call.name, "is_ref") == 0) {
                emit(cg, "strada_new_int(strada_is_ref(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "))");
            } else if (strcmp(expr->data.call.name, "reftype") == 0) {
                emit(cg, "strada_new_str(strada_reftype(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "))");
            } else if (strcmp(expr->data.call.name, "exit") == 0) {
                emit(cg, "strada_exit(strada_to_int(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "))");
            } else if (strcmp(expr->data.call.name, "free") == 0) {
                emit(cg, "strada_free(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ")");
            } else if (strcmp(expr->data.call.name, "undef") == 0) {
                emit(cg, "strada_undef(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ")");
            } else if (strcmp(expr->data.call.name, "refcount") == 0) {
                emit(cg, "strada_new_int(strada_refcount(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "))");
            } else if (strcmp(expr->data.call.name, "stacktrace") == 0) {
                emit(cg, "strada_stacktrace()");
            } else if (strcmp(expr->data.call.name, "backtrace") == 0) {
                emit(cg, "strada_backtrace()");
            } else if (strcmp(expr->data.call.name, "caller") == 0) {
                emit(cg, "strada_new_str(strada_caller(");
                if (expr->data.call.arg_count > 0) {
                    emit(cg, "strada_to_int(");
                    gen_expression(cg, expr->data.call.args[0]);
                    emit(cg, ")");
                } else {
                    emit(cg, "1");
                }
                emit(cg, "))");
            } else if (strcmp(expr->data.call.name, "open") == 0) {
                emit(cg, "({ char *__s0 = strada_to_str(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "); char *__s1 = strada_to_str(");
                gen_expression(cg, expr->data.call.args[1]);
                emit(cg, "); StradaValue *__r = strada_open(__s0, __s1); free(__s0); free(__s1); __r; })");
            } else if (strcmp(expr->data.call.name, "close") == 0) {
                emit(cg, "strada_close(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ")");
            } else if (strcmp(expr->data.call.name, "write_fd") == 0) {
                emit(cg, "strada_write_fd(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ", ");
                gen_expression(cg, expr->data.call.args[1]);
                emit(cg, ")");
            } else if (strcmp(expr->data.call.name, "close_fd") == 0) {
                emit(cg, "strada_close_fd(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ")");
            } else if (strcmp(expr->data.call.name, "getpid") == 0) {
                emit(cg, "strada_new_int(getpid())");
            } else if (strcmp(expr->data.call.name, "unlink") == 0) {
                emit(cg, "strada_unlink(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ")");
            } else if (expr->data.call.package_name &&
                       strcmp(expr->data.call.package_name, "sys") == 0 &&
                       strcmp(expr->data.call.name, "fwrite") == 0) {
                emit(cg, "({ char *__s = strada_to_str(");
                gen_expression(cg, expr->data.call.args[1]);
                emit(cg, "); strada_write_file(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ", __s); free(__s); })");
            } else if (expr->data.call.package_name &&
                       strcmp(expr->data.call.package_name, "sys") == 0 &&
                       strcmp(expr->data.call.name, "open_fd") == 0) {
                emit(cg, "strada_open_fd(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ", ");
                gen_expression(cg, expr->data.call.args[1]);
                emit(cg, ")");
            } else if (expr->data.call.package_name &&
                       strcmp(expr->data.call.package_name, "sys") == 0 &&
                       strcmp(expr->data.call.name, "hires_time") == 0) {
                emit(cg, "strada_hires_time()");
            } else if (expr->data.call.package_name &&
                       strcmp(expr->data.call.package_name, "sys") == 0 &&
                       strcmp(expr->data.call.name, "getenv") == 0) {
                emit(cg, "strada_getenv(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ")");
            } else if (strcmp(expr->data.call.name, "readline") == 0) {
                if (expr->data.call.arg_count > 0) {
                    emit(cg, "strada_read_line(");
                    gen_expression(cg, expr->data.call.args[0]);
                    emit(cg, ")");
                } else {
                    emit(cg, "strada_readline()");
                }
            } else if (strcmp(expr->data.call.name, "slurp") == 0) {
                emit(cg, "({ char *__s = strada_to_str(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "); StradaValue *__r = strada_slurp(__s); free(__s); __r; })");
            } else if (strcmp(expr->data.call.name, "spew") == 0) {
                emit(cg, "({ char *__s0 = strada_to_str(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "); char *__s1 = strada_to_str(");
                gen_expression(cg, expr->data.call.args[1]);
                emit(cg, "); strada_spew(__s0, __s1); free(__s0); free(__s1); })");
            } else if (strcmp(expr->data.call.name, "sb_new") == 0) {
                if (expr->data.call.arg_count > 0) {
                    emit(cg, "strada_sb_new_cap(");
                    gen_expression(cg, expr->data.call.args[0]);
                    emit(cg, ")");
                } else {
                    emit(cg, "strada_sb_new()");
                }
            } else if (strcmp(expr->data.call.name, "sb_append") == 0) {
                emit(cg, "(strada_sb_append(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ", ");
                gen_expression(cg, expr->data.call.args[1]);
                emit(cg, "), strada_new_undef())");
            } else if (strcmp(expr->data.call.name, "sb_to_string") == 0) {
                emit(cg, "strada_sb_to_string(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ")");
            } else if (strcmp(expr->data.call.name, "sb_length") == 0) {
                emit(cg, "strada_sb_length(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ")");
            } else if (strcmp(expr->data.call.name, "sb_clear") == 0) {
                emit(cg, "(strada_sb_clear(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "), strada_new_undef())");
            } else if (strcmp(expr->data.call.name, "sb_free") == 0) {
                emit(cg, "(strada_sb_free(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "), strada_new_undef())");
            } else if (strcmp(expr->data.call.name, "match") == 0) {
                emit(cg, "({ char *__s0 = strada_to_str(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "); char *__s1 = strada_to_str(");
                gen_expression(cg, expr->data.call.args[1]);
                emit(cg, "); StradaValue *__r = strada_new_int(strada_regex_match(__s0, __s1)); free(__s0); free(__s1); __r; })");
            } else if (strcmp(expr->data.call.name, "replace") == 0) {
                emit(cg, "({ char *__s0 = strada_to_str(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "); char *__s1 = strada_to_str(");
                gen_expression(cg, expr->data.call.args[1]);
                emit(cg, "); char *__s2 = strada_to_str(");
                gen_expression(cg, expr->data.call.args[2]);
                emit(cg, "); StradaValue *__r = strada_new_str_take(strada_regex_replace(__s0, __s1, __s2)); free(__s0); free(__s1); free(__s2); __r; })");
            } else if (strcmp(expr->data.call.name, "replace_all") == 0) {
                emit(cg, "({ char *__s0 = strada_to_str(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "); char *__s1 = strada_to_str(");
                gen_expression(cg, expr->data.call.args[1]);
                emit(cg, "); char *__s2 = strada_to_str(");
                gen_expression(cg, expr->data.call.args[2]);
                emit(cg, "); StradaValue *__r = strada_new_str_take(strada_regex_replace_all(__s0, __s1, __s2)); free(__s0); free(__s1); free(__s2); __r; })");
            } else if (strcmp(expr->data.call.name, "split") == 0) {
                emit(cg, "({ char *__s0 = strada_to_str(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "); char *__s1 = strada_to_str(");
                gen_expression(cg, expr->data.call.args[1]);
                emit(cg, "); StradaValue *__sv = strada_new_array(); __sv->value.av = strada_regex_split(__s0, __s1); free(__s0); free(__s1); __sv; })");
            } else if (strcmp(expr->data.call.name, "capture") == 0) {
                emit(cg, "({ char *__s0 = strada_to_str(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "); char *__s1 = strada_to_str(");
                gen_expression(cg, expr->data.call.args[1]);
                emit(cg, "); StradaValue *__sv = strada_new_array(); __sv->value.av = strada_regex_capture(__s0, __s1); free(__s0); free(__s1); __sv; })");
            } else if (strcmp(expr->data.call.name, "socket_server") == 0) {
                emit(cg, "strada_socket_server(strada_to_int(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "))");
            } else if (strcmp(expr->data.call.name, "socket_client") == 0) {
                emit(cg, "({ char *__s = strada_to_str(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "); StradaValue *__r = strada_socket_client(__s, strada_to_int(");
                gen_expression(cg, expr->data.call.args[1]);
                emit(cg, ")); free(__s); __r; })");
            } else if (strcmp(expr->data.call.name, "socket_accept") == 0) {
                emit(cg, "strada_socket_accept(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ")");
            } else if (strcmp(expr->data.call.name, "socket_send") == 0) {
                emit(cg, "({ char *__s = strada_to_str(");
                gen_expression(cg, expr->data.call.args[1]);
                emit(cg, "); StradaValue *__r = strada_new_int(strada_socket_send(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ", __s)); free(__s); __r; })");
            } else if (strcmp(expr->data.call.name, "socket_recv") == 0) {
                emit(cg, "strada_socket_recv(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ", strada_to_int(");
                gen_expression(cg, expr->data.call.args[1]);
                emit(cg, "))");
            } else if (strcmp(expr->data.call.name, "socket_close") == 0) {
                emit(cg, "strada_socket_close(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ")");
            } else if (strcmp(expr->data.call.name, "cstruct_new") == 0) {
                emit(cg, "strada_cstruct_new(\"\", strada_to_int(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "))");
            } else if (strcmp(expr->data.call.name, "cstruct_ptr") == 0) {
                emit(cg, "strada_cpointer_new(strada_cstruct_ptr(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, "))");
            } else if (strcmp(expr->data.call.name, "cstruct_set_int") == 0) {
                emit(cg, "strada_cstruct_set_field(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ", strada_to_str(");
                gen_expression(cg, expr->data.call.args[1]);
                emit(cg, "), strada_to_int(");
                gen_expression(cg, expr->data.call.args[2]);
                emit(cg, "), &(int){strada_to_int(");
                gen_expression(cg, expr->data.call.args[3]);
                emit(cg, ")}, sizeof(int))");
            } else if (strcmp(expr->data.call.name, "cstruct_get_int") == 0) {
                emit(cg, "strada_new_int(*(int*)strada_cstruct_get_field(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ", strada_to_str(");
                gen_expression(cg, expr->data.call.args[1]);
                emit(cg, "), strada_to_int(");
                gen_expression(cg, expr->data.call.args[2]);
                emit(cg, "), sizeof(int)))");
            } else if (strcmp(expr->data.call.name, "cstruct_set_double") == 0) {
                emit(cg, "strada_cstruct_set_field(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ", strada_to_str(");
                gen_expression(cg, expr->data.call.args[1]);
                emit(cg, "), strada_to_int(");
                gen_expression(cg, expr->data.call.args[2]);
                emit(cg, "), &(double){strada_to_num(");
                gen_expression(cg, expr->data.call.args[3]);
                emit(cg, ")}, sizeof(double))");
            } else if (strcmp(expr->data.call.name, "cstruct_get_double") == 0) {
                emit(cg, "strada_new_num(*(double*)strada_cstruct_get_field(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ", strada_to_str(");
                gen_expression(cg, expr->data.call.args[1]);
                emit(cg, "), strada_to_int(");
                gen_expression(cg, expr->data.call.args[2]);
                emit(cg, "), sizeof(double)))");
            } else if (strcmp(expr->data.call.name, "cstruct_set_string") == 0) {
                emit(cg, "strada_cstruct_set_field(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ", strada_to_str(");
                gen_expression(cg, expr->data.call.args[1]);
                emit(cg, "), strada_to_int(");
                gen_expression(cg, expr->data.call.args[2]);
                emit(cg, "), (void*)strada_to_str(");
                gen_expression(cg, expr->data.call.args[3]);
                emit(cg, "), 64)");
            } else if (strcmp(expr->data.call.name, "cstruct_get_string") == 0) {
                emit(cg, "strada_new_str((char*)strada_cstruct_get_field(");
                gen_expression(cg, expr->data.call.args[0]);
                emit(cg, ", strada_to_str(");
                gen_expression(cg, expr->data.call.args[1]);
                emit(cg, "), strada_to_int(");
                gen_expression(cg, expr->data.call.args[2]);
                emit(cg, "), 64))");
            } else {
                // User-defined function (possibly from another package)
                // Check if this function has optional parameters
                int param_count = 0;
                int min_args = 0;
                int has_optionals = lookup_function(cg, expr->data.call.name, &param_count, &min_args);
                
                // Generate function name with package prefix if qualified
                if (expr->data.call.package_name) {
                    // Qualified call: Package::Name::func -> Package_Name_func
                    char *prefix = package_to_prefix(expr->data.call.package_name);
                    emit(cg, "%s%s(", prefix, expr->data.call.name);
                    free(prefix);
                } else if (cg->current_package) {
                    // Local call within a package - add current package prefix
                    char *prefix = package_to_prefix(cg->current_package);
                    emit(cg, "%s%s(", prefix, expr->data.call.name);
                    free(prefix);
                } else {
                    // Local call in main package
                    emit(cg, "%s(", expr->data.call.name);
                }
                
                // Generate provided arguments
                for (int i = 0; i < expr->data.call.arg_count; i++) {
                    if (i > 0) emit(cg, ", ");
                    gen_expression(cg, expr->data.call.args[i]);
                }
                
                // Fill in missing optional arguments with strada_new_undef()
                if (has_optionals && expr->data.call.arg_count < param_count) {
                    for (int i = expr->data.call.arg_count; i < param_count; i++) {
                        if (i > 0) emit(cg, ", ");
                        emit(cg, "strada_new_undef()");
                    }
                }
                
                emit(cg, ")");
            }
            break;
        }
        
        case NODE_INDIRECT_CALL: {
            // Call through function pointer: obj->callback(args)
            // Generate: (obj->callback)(args)
            emit(cg, "(");
            gen_expression(cg, expr->data.indirect_call.target);
            emit(cg, ")(");
            
            for (int i = 0; i < expr->data.indirect_call.arg_count; i++) {
                if (i > 0) emit(cg, ", ");
                gen_expression(cg, expr->data.indirect_call.args[i]);
            }
            
            emit(cg, ")");
            break;
        }
        
        case NODE_SUBSCRIPT:
            emit(cg, "strada_array_get(");
            gen_expression(cg, expr->data.subscript.array);
            emit(cg, "->value.av, strada_to_int(");
            gen_expression(cg, expr->data.subscript.index);
            emit(cg, "))");
            break;
        
        case NODE_HASH_ACCESS:
            // Hash access: $hash{key} -> strada_hash_get(hash->value.hv, key)
            if (expr->data.subscript.index->type == NODE_STR_LITERAL &&
                str_is_safe_literal(expr->data.subscript.index->data.str_val)) {
                emit(cg, "strada_hash_get(");
                gen_expression(cg, expr->data.subscript.array);
                emit(cg, "->value.hv, \"%s\")", expr->data.subscript.index->data.str_val);
            } else {
                // Dynamic key: capture strada_to_str and free
                emit(cg, "({ char *__k = strada_to_str(");
                gen_expression(cg, expr->data.subscript.index);
                emit(cg, "); StradaValue *__r = strada_hash_get(");
                gen_expression(cg, expr->data.subscript.array);
                emit(cg, "->value.hv, __k); free(__k); __r; })");
            }
            break;
        
        case NODE_REF:
            // \$var, \@arr, \%hash - create reference
            emit(cg, "strada_new_ref(");
            gen_expression(cg, expr->data.ref_expr.target);
            emit(cg, ", '%c')", expr->data.ref_expr.ref_type);
            break;
        
        case NODE_ANON_HASH: {
            // { key => value, ... } - anonymous hash reference
            emit(cg, "strada_anon_hash(%d", expr->data.anon_hash.pair_count);
            for (int i = 0; i < expr->data.anon_hash.pair_count; i++) {
                emit(cg, ", \"%s\", ", expr->data.anon_hash.keys[i]);
                gen_expression(cg, expr->data.anon_hash.values[i]);
            }
            emit(cg, ")");
            break;
        }
        
        case NODE_ANON_ARRAY: {
            // [ elem, ... ] - anonymous array reference
            emit(cg, "strada_anon_array(%d", expr->data.anon_array.element_count);
            for (int i = 0; i < expr->data.anon_array.element_count; i++) {
                emit(cg, ", ");
                gen_expression(cg, expr->data.anon_array.elements[i]);
            }
            emit(cg, ")");
            break;
        }
        
        case NODE_DEREF_HASH:
            // $ref->{key} - dereference hash reference
            if (expr->data.deref_hash.key->type == NODE_STR_LITERAL &&
                str_is_safe_literal(expr->data.deref_hash.key->data.str_val)) {
                emit(cg, "strada_hash_get(strada_deref_hash(");
                gen_expression(cg, expr->data.deref_hash.ref);
                emit(cg, "), \"%s\")", expr->data.deref_hash.key->data.str_val);
            } else {
                // Dynamic key: capture strada_to_str and free
                emit(cg, "({ char *__k = strada_to_str(");
                gen_expression(cg, expr->data.deref_hash.key);
                emit(cg, "); StradaValue *__r = strada_hash_get(strada_deref_hash(");
                gen_expression(cg, expr->data.deref_hash.ref);
                emit(cg, "), __k); free(__k); __r; })");
            }
            break;
        
        case NODE_DEREF_ARRAY:
            // $ref->[index] - dereference array reference
            emit(cg, "strada_array_get(strada_deref_array(");
            gen_expression(cg, expr->data.deref_array.ref);
            emit(cg, "), strada_to_int(");
            gen_expression(cg, expr->data.deref_array.index);
            emit(cg, "))");
            break;
        
        case NODE_DEREF_SCALAR:
            // $$ref - dereference scalar reference
            emit(cg, "strada_deref(");
            gen_expression(cg, expr->data.deref_scalar.ref);
            emit(cg, ")");
            break;
        
        case NODE_DEREF_TO_ARRAY:
            // @{$ref} - dereference ref to array (copy)
            emit(cg, "strada_array_from_ref(");
            gen_expression(cg, expr->data.deref_to_array.ref);
            emit(cg, ")");
            break;
        
        case NODE_DEREF_TO_HASH:
            // %{$ref} - dereference ref to hash (copy)
            emit(cg, "strada_hash_from_ref(");
            gen_expression(cg, expr->data.deref_to_hash.ref);
            emit(cg, ")");
            break;
            
        case NODE_MEMBER_ACCESS: {
            // Get the struct type of the object
            const char *struct_type = NULL;
            if (expr->data.member_access.object->type == NODE_VARIABLE) {
                struct_type = lookup_variable_struct_type(cg, 
                    expr->data.member_access.object->data.variable.name);
            }
            
            // Look up the field type
            DataType field_type = TYPE_INT;  // Default
            if (struct_type) {
                field_type = lookup_field_type(cg, struct_type, 
                    expr->data.member_access.field_name);
            }
            
            // Generate appropriate wrapper based on field type
            switch (field_type) {
                case TYPE_NUM:
                case TYPE_FLOAT:
                    emit(cg, "strada_new_num(");
                    gen_expression(cg, expr->data.member_access.object);
                    emit(cg, "->%s)", expr->data.member_access.field_name);
                    break;
                case TYPE_STR:
                    emit(cg, "strada_new_str(");
                    gen_expression(cg, expr->data.member_access.object);
                    emit(cg, "->%s)", expr->data.member_access.field_name);
                    break;
                case TYPE_I64:
                case TYPE_U64:
                case TYPE_LONG:
                case TYPE_ULONG:
                case TYPE_SIZE:
                    // For 64-bit integers, use strada_new_int (which uses int64_t internally)
                    emit(cg, "strada_new_int((int64_t)");
                    gen_expression(cg, expr->data.member_access.object);
                    emit(cg, "->%s)", expr->data.member_access.field_name);
                    break;
                case TYPE_PTR:
                    // For pointers, cast to int64_t
                    emit(cg, "strada_new_int((int64_t)");
                    gen_expression(cg, expr->data.member_access.object);
                    emit(cg, "->%s)", expr->data.member_access.field_name);
                    break;
                case TYPE_BOOL:
                    emit(cg, "strada_new_int((int)");
                    gen_expression(cg, expr->data.member_access.object);
                    emit(cg, "->%s)", expr->data.member_access.field_name);
                    break;
                case TYPE_FUNC:
                    // Function pointer - no wrapper, just raw access
                    gen_expression(cg, expr->data.member_access.object);
                    emit(cg, "->%s", expr->data.member_access.field_name);
                    break;
                default:  // TYPE_INT, TYPE_CHAR, TYPE_SHORT, TYPE_I8, TYPE_I16, TYPE_I32, etc.
                    emit(cg, "strada_new_int(");
                    gen_expression(cg, expr->data.member_access.object);
                    emit(cg, "->%s)", expr->data.member_access.field_name);
                    break;
            }
            break;
        }
        
        case NODE_CLONE: {
            // Get the struct type from the source variable
            const char *struct_type = NULL;
            if (expr->data.clone.source->type == NODE_VARIABLE) {
                struct_type = lookup_variable_struct_type(cg, 
                    expr->data.clone.source->data.variable.name);
            }
            
            if (struct_type) {
                // Generate: strada_clone_TYPENAME(source)
                emit(cg, "strada_clone_%s(", struct_type);
                gen_expression(cg, expr->data.clone.source);
                emit(cg, ")");
            } else {
                // Fallback: just copy the pointer (shallow)
                gen_expression(cg, expr->data.clone.source);
            }
            break;
        }
        
        case NODE_FUNC_REF: {
            // Generate function pointer: &func_name or just func_name
            emit(cg, "%s", expr->data.func_ref.func_name);
            break;
        }
            
        default:
            break;
    }
}

/* codegen.h - Strada Code Generator */
#ifndef CODEGEN_H
#define CODEGEN_H

#include "ast.h"
#include <stdio.h>

typedef struct {
    FILE *output;
    int indent_level;
    int current_func_is_extern;  // Track if current function is extern
    int current_func_is_main;    // Track if current function is main
    
    // Package/Module support
    char *current_package;       // Current package name (NULL = main)
    char **lib_paths;            // Library search paths
    int lib_path_count;
    char **included_modules;     // Already included modules
    int included_module_count;
    const char *source_dir;      // Directory of source file being compiled
    
    // Function registry for optional parameters
    struct {
        char **names;
        int *param_counts;
        int *min_args;
        int count;
        int capacity;
    } functions;
    
    // Struct registry for type tracking
    struct {
        char **names;           // Struct names
        ASTNode **definitions;  // Struct definition nodes
        int count;
        int capacity;
    } structs;
    
    // Variable type registry (for tracking struct variable types)
    struct {
        char **names;           // Variable names
        char **struct_types;    // Struct type names (NULL if not struct)
        int count;
        int capacity;
    } variables;

    // Scope-based cleanup tracking (stack of scopes)
    struct {
        char ***names;          // Array of scopes, each scope is array of var names
        int **is_struct;        // Parallel array of is_struct flags per scope
        int *counts;            // Number of vars in each scope
        int *capacities;        // Capacity of each scope
        int depth;              // Current scope depth
        int max_depth;          // Allocated scope slots
    } scope_stack;
} CodeGen;

/* Code generator functions */
CodeGen* codegen_new(FILE *output);
void codegen_free(CodeGen *cg);

/* Generate C code from AST */
void codegen_generate(CodeGen *cg, ASTNode *program);

/* Set source directory for module resolution */
void codegen_set_source_dir(CodeGen *cg, const char *dir);

#endif /* CODEGEN_H */

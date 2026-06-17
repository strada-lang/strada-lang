/* ast.h - Abstract Syntax Tree Node Definitions */
#ifndef AST_H
#define AST_H

#include <stdint.h>

typedef enum {
    NODE_PROGRAM,
    NODE_FUNCTION,
    NODE_PARAM,
    NODE_BLOCK,
    NODE_VAR_DECL,
    NODE_IF_STMT,
    NODE_WHILE_STMT,
    NODE_FOR_STMT,
    NODE_FOREACH_STMT,
    NODE_RETURN_STMT,
    NODE_EXPR_STMT,
    NODE_STRUCT_DEF,
    NODE_STRUCT_FIELD,
    
    // Package/Module nodes
    NODE_PACKAGE_DECL,  // package Name::Space;
    NODE_USE_STMT,      // use Package::Name;
    NODE_USE_LIB,       // use lib "/path";
    
    NODE_BINARY_OP,
    NODE_UNARY_OP,
    NODE_ASSIGN,
    NODE_CALL,
    NODE_INDIRECT_CALL,
    NODE_SUBSCRIPT,
    NODE_HASH_ACCESS,
    NODE_MEMBER_ACCESS,
    NODE_CLONE,
    NODE_FUNC_REF,
    NODE_VARIABLE,
    NODE_INT_LITERAL,
    NODE_NUM_LITERAL,
    NODE_STR_LITERAL,
    NODE_ARRAY_LITERAL,
    NODE_HASH_LITERAL,
    
    // Control flow
    NODE_LAST_STMT,     // last; - break out of loop
    NODE_NEXT_STMT,     // next; - continue to next iteration
    
    // Reference nodes
    NODE_REF,           // \$var, \@arr, \%hash - create reference
    NODE_ANON_HASH,     // { key => value, ... } - anonymous hash
    NODE_ANON_ARRAY,    // [ elem, elem, ... ] - anonymous array
    NODE_DEREF_HASH,    // $ref->{key} - dereference hash ref
    NODE_DEREF_ARRAY,   // $ref->[index] - dereference array ref
    NODE_DEREF_SCALAR,  // $$ref - dereference scalar ref
    NODE_DEREF_TO_ARRAY, // @{$ref} - dereference ref to array
    NODE_DEREF_TO_HASH,  // %{$ref} - dereference ref to hash

    NODE_TRY_CATCH,     // try { } catch ($e) { }
    NODE_THROW,         // throw $expr;

    // Loop labels
    NODE_LAST_LABEL,    // last LABEL;
    NODE_NEXT_LABEL,    // next LABEL;

    // Goto/Label
    NODE_LABEL,         // LABEL:
    NODE_GOTO,          // goto LABEL;
} NodeType;

typedef enum {
    TYPE_VOID,
    TYPE_INT,
    TYPE_NUM,
    TYPE_STR,
    TYPE_SCALAR,
    TYPE_ARRAY,
    TYPE_HASH,
    TYPE_STRUCT,
    TYPE_FUNC,
    
    /* Extended C types */
    TYPE_CHAR,
    TYPE_SHORT,
    TYPE_LONG,
    TYPE_FLOAT,
    TYPE_BOOL,
    TYPE_UCHAR,
    TYPE_USHORT,
    TYPE_UINT,
    TYPE_ULONG,
    TYPE_I8,
    TYPE_I16,
    TYPE_I32,
    TYPE_I64,
    TYPE_U8,
    TYPE_U16,
    TYPE_U32,
    TYPE_U64,
    TYPE_SIZE,
    TYPE_PTR,
} DataType;

typedef struct ASTNode ASTNode;

struct ASTNode {
    NodeType type;
    int line;
    
    union {
        // Program
        struct {
            ASTNode **functions;
            int function_count;
            ASTNode **structs;
            int struct_count;
            char *package_name;     // Current package name (NULL = main)
            ASTNode **use_stmts;    // List of use statement nodes
            int use_count;
            char **lib_paths;       // Additional lib paths from 'use lib'
            int lib_path_count;
        } program;
        
        // Package declaration
        struct {
            char *name;             // Package::Name::Here
        } package_decl;
        
        // Use statement
        struct {
            char *package_name;     // Package::Name to import
            char **imports;         // Specific imports (NULL = import all)
            int import_count;
        } use_stmt;
        
        // Use lib statement
        struct {
            char *path;             // Library path to add to @INC
        } use_lib;
        
        // Function
        struct {
            char *name;
            ASTNode **params;
            int param_count;
            DataType return_type;
            char *return_struct_name;  // For TYPE_STRUCT return, the struct type name
            ASTNode *body;
            int is_variadic;
            int min_args;  // Minimum required args (before optional params)
            int is_extern; // extern "C" linkage for shared libraries
        } function;
        
        // Parameter
        struct {
            char *name;
            DataType param_type;
            char *struct_name;  // For TYPE_STRUCT, the struct type name
            char sigil; // '$', '@', '%'
            int is_optional;
            ASTNode *default_value;
        } param;
        
        // Block
        struct {
            ASTNode **statements;
            int statement_count;
        } block;
        
        // Variable declaration
        struct {
            char *name;
            DataType var_type;
            char *struct_name;  // For TYPE_STRUCT, the struct type name
            char sigil;
            ASTNode *init;
        } var_decl;
        
        // If statement
        struct {
            ASTNode *condition;
            ASTNode *then_block;
            ASTNode **elsif_conditions;
            ASTNode **elsif_blocks;
            int elsif_count;
            ASTNode *else_block;
        } if_stmt;
        
        // While statement
        struct {
            ASTNode *condition;
            ASTNode *body;
        } while_stmt;
        
        // For statement
        struct {
            ASTNode *init;
            ASTNode *condition;
            ASTNode *update;
            ASTNode *body;
        } for_stmt;
        
        // Foreach statement
        struct {
            char *var_name;
            DataType var_type;
            ASTNode *array;
            ASTNode *body;
        } foreach_stmt;
        
        // Return statement
        struct {
            ASTNode *value;
        } return_stmt;
        
        // Expression statement
        struct {
            ASTNode *expr;
        } expr_stmt;
        
        // Binary operation
        struct {
            char *op;
            ASTNode *left;
            ASTNode *right;
        } binary_op;
        
        // Unary operation
        struct {
            char *op;
            ASTNode *operand;
        } unary_op;
        
        // Assignment
        struct {
            char *op; // "=", "+=", "-=", ".="
            ASTNode *target;
            ASTNode *value;
        } assign;
        
        // Function call
        struct {
            char *name;
            char *package_name;  // For Package::func() calls (NULL = local)
            ASTNode **args;
            int arg_count;
        } call;
        
        // Indirect function call (through function pointer)
        struct {
            ASTNode *target;   // The expression evaluating to function pointer
            ASTNode **args;
            int arg_count;
        } indirect_call;
        
        // Array/hash subscript
        struct {
            ASTNode *array;
            ASTNode *index;
        } subscript;
        
        // Hash access
        struct {
            ASTNode *hash;
            char *key;
        } hash_access;
        
        // Variable reference
        struct {
            char *name;
            char sigil;
        } variable;
        
        // Literals
        int64_t int_val;
        double num_val;
        char *str_val;
        
        // Array literal
        struct {
            ASTNode **elements;
            int element_count;
        } array_literal;
        
        // Hash literal
        struct {
            char **keys;
            ASTNode **values;
            int pair_count;
        } hash_literal;
        
        // Reference creation (\$var, \@arr, \%hash)
        struct {
            ASTNode *target;    // The variable being referenced
            char ref_type;      // '$', '@', '%' - type of referent
        } ref_expr;
        
        // Anonymous hash { key => val, ... }
        struct {
            char **keys;
            ASTNode **values;
            int pair_count;
        } anon_hash;
        
        // Anonymous array [ elem, ... ]
        struct {
            ASTNode **elements;
            int element_count;
        } anon_array;
        
        // Hash dereference $ref->{key}
        struct {
            ASTNode *ref;
            ASTNode *key;
        } deref_hash;
        
        // Array dereference $ref->[index]
        struct {
            ASTNode *ref;
            ASTNode *index;
        } deref_array;
        
        // Scalar dereference $$ref
        struct {
            ASTNode *ref;
        } deref_scalar;
        
        // Array dereference @{$ref}
        struct {
            ASTNode *ref;
        } deref_to_array;
        
        // Hash dereference %{$ref}
        struct {
            ASTNode *ref;
        } deref_to_hash;
        
        // Struct definition
        struct {
            char *name;
            ASTNode **fields;
            int field_count;
            int total_size;
        } struct_def;
        
        // Struct field
        struct {
            char *name;
            DataType field_type;
            int offset;
            int size;
            // For function pointer fields (TYPE_FUNC)
            DataType func_return_type;
            DataType *func_param_types;
            int func_param_count;
        } struct_field;
        
        // Member access (struct->field)
        struct {
            ASTNode *object;
            char *field_name;
        } member_access;
        
        // Clone (deep copy of struct)
        struct {
            ASTNode *source;       // The struct to clone
            char *struct_type;     // The struct type name
        } clone;
        
        // Function reference (for function pointers)
        struct {
            char *func_name;
        } func_ref;

        // Try/Catch block
        struct {
            ASTNode *try_block;
            char *catch_var;
            ASTNode *catch_block;
        } try_catch;

        // Throw statement
        struct {
            ASTNode *expr;
        } throw_stmt;

        // Label definition (LABEL:)
        struct {
            char *name;
        } label;

        // Goto statement
        struct {
            char *target;
        } goto_stmt;

        // last/next with label
        struct {
            char *label;
        } loop_label;
    } data;
};

// AST creation functions
ASTNode* ast_new_program(void);
ASTNode* ast_new_function(const char *name, DataType return_type);
ASTNode* ast_new_param(const char *name, DataType type, char sigil);
ASTNode* ast_new_block(void);
ASTNode* ast_new_var_decl(const char *name, DataType type, char sigil, ASTNode *init);
ASTNode* ast_new_if(ASTNode *cond, ASTNode *then_block);
ASTNode* ast_new_while(ASTNode *cond, ASTNode *body);
ASTNode* ast_new_for(ASTNode *init, ASTNode *cond, ASTNode *update, ASTNode *body);
ASTNode* ast_new_foreach(const char *var, DataType type, ASTNode *array, ASTNode *body);
ASTNode* ast_new_return(ASTNode *value);
ASTNode* ast_new_expr_stmt(ASTNode *expr);
ASTNode* ast_new_binary_op(const char *op, ASTNode *left, ASTNode *right);
ASTNode* ast_new_unary_op(const char *op, ASTNode *operand);
ASTNode* ast_new_assign(const char *op, ASTNode *target, ASTNode *value);
ASTNode* ast_new_call(const char *name);
ASTNode* ast_new_qualified_call(const char *package, const char *name);
ASTNode* ast_new_indirect_call(ASTNode *target);
ASTNode* ast_new_subscript(ASTNode *array, ASTNode *index);
ASTNode* ast_new_hash_access(ASTNode *hash, const char *key);
ASTNode* ast_new_variable(const char *name, char sigil);
ASTNode* ast_new_int_literal(int64_t value);
ASTNode* ast_new_num_literal(double value);
ASTNode* ast_new_str_literal(const char *value);
ASTNode* ast_new_array_literal(void);
ASTNode* ast_new_hash_literal(void);

// Package/Module functions
ASTNode* ast_new_package_decl(const char *name);
ASTNode* ast_new_use_stmt(const char *package_name);
ASTNode* ast_new_use_lib(const char *path);
void ast_set_package(ASTNode *program, const char *name);
void ast_add_use(ASTNode *program, ASTNode *use_stmt);
void ast_add_lib_path(ASTNode *program, const char *path);
void ast_add_import(ASTNode *use_stmt, const char *func_name);

// Reference functions
ASTNode* ast_new_ref(ASTNode *target, char ref_type);
ASTNode* ast_new_anon_hash(void);
ASTNode* ast_new_anon_array(void);
ASTNode* ast_new_deref_hash(ASTNode *ref, ASTNode *key);
ASTNode* ast_new_deref_array(ASTNode *ref, ASTNode *index);
ASTNode* ast_new_deref_scalar(ASTNode *ref);
ASTNode* ast_new_deref_to_array(ASTNode *ref);
ASTNode* ast_new_deref_to_hash(ASTNode *ref);
void ast_add_anon_hash_pair(ASTNode *hash, const char *key, ASTNode *value);
void ast_add_anon_array_element(ASTNode *array, ASTNode *elem);

// Struct functions
ASTNode* ast_new_struct_def(const char *name);
ASTNode* ast_new_struct_field(const char *name, DataType type);
ASTNode* ast_new_struct_field_func(const char *name, DataType return_type,
                                    DataType *param_types, int param_count);
ASTNode* ast_new_member_access(ASTNode *object, const char *field);
ASTNode* ast_new_clone(ASTNode *source, const char *struct_type);
ASTNode* ast_new_func_ref(const char *func_name);

// Exception handling
ASTNode* ast_new_try_catch(ASTNode *try_block, const char *catch_var, ASTNode *catch_block);
ASTNode* ast_new_throw(ASTNode *expr);

// Goto/Label
ASTNode* ast_new_label(const char *name);
ASTNode* ast_new_goto(const char *target);
ASTNode* ast_new_last_label(const char *label);
ASTNode* ast_new_next_label(const char *label);

// Helper functions
void ast_add_function(ASTNode *program, ASTNode *function);
void ast_add_struct_def(ASTNode *program, ASTNode *struct_def);
void ast_add_struct_field(ASTNode *struct_def, ASTNode *field);
void ast_add_param(ASTNode *function, ASTNode *param);
void ast_add_statement(ASTNode *block, ASTNode *stmt);
void ast_add_elsif(ASTNode *if_stmt, ASTNode *cond, ASTNode *block);
void ast_add_arg(ASTNode *call, ASTNode *arg);
void ast_add_indirect_arg(ASTNode *call, ASTNode *arg);
void ast_add_array_element(ASTNode *array, ASTNode *elem);
void ast_add_hash_pair(ASTNode *hash, const char *key, ASTNode *value);

// Cleanup
void ast_free(ASTNode *node);

#endif /* AST_H */
ASTNode* ast_new_node(NodeType type);

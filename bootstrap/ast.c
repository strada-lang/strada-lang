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

/* ast.c - AST Node Implementation */
#define _POSIX_C_SOURCE 200809L
#include "ast.h"
#include <stdlib.h>
#include <string.h>

ASTNode* ast_new_node(NodeType type) {
    ASTNode *node = calloc(1, sizeof(ASTNode));
    node->type = type;
    return node;
}

ASTNode* ast_new_program(void) {
    ASTNode *node = ast_new_node(NODE_PROGRAM);
    node->data.program.functions = NULL;
    node->data.program.function_count = 0;
    node->data.program.structs = malloc(16 * sizeof(ASTNode*));
    node->data.program.struct_count = 0;
    node->data.program.package_name = NULL;
    node->data.program.use_stmts = malloc(32 * sizeof(ASTNode*));
    node->data.program.use_count = 0;
    node->data.program.lib_paths = malloc(16 * sizeof(char*));
    node->data.program.lib_path_count = 0;
    return node;
}

ASTNode* ast_new_function(const char *name, DataType return_type) {
    ASTNode *node = ast_new_node(NODE_FUNCTION);
    node->data.function.name = strdup(name);
    node->data.function.return_type = return_type;
    node->data.function.return_struct_name = NULL;
    node->data.function.params = NULL;
    node->data.function.param_count = 0;
    node->data.function.body = NULL;
    node->data.function.is_variadic = 0;
    node->data.function.min_args = 0;
    node->data.function.is_extern = 0;
    return node;
}

ASTNode* ast_new_param(const char *name, DataType type, char sigil) {
    ASTNode *node = ast_new_node(NODE_PARAM);
    node->data.param.name = strdup(name);
    node->data.param.param_type = type;
    node->data.param.struct_name = NULL;
    node->data.param.sigil = sigil;
    node->data.param.is_optional = 0;
    node->data.param.default_value = NULL;
    return node;
}

ASTNode* ast_new_block(void) {
    ASTNode *node = ast_new_node(NODE_BLOCK);
    node->data.block.statements = NULL;
    node->data.block.statement_count = 0;
    return node;
}

ASTNode* ast_new_var_decl(const char *name, DataType type, char sigil, ASTNode *init) {
    ASTNode *node = ast_new_node(NODE_VAR_DECL);
    node->data.var_decl.name = strdup(name);
    node->data.var_decl.var_type = type;
    node->data.var_decl.struct_name = NULL;
    node->data.var_decl.sigil = sigil;
    node->data.var_decl.init = init;
    return node;
}

ASTNode* ast_new_if(ASTNode *cond, ASTNode *then_block) {
    ASTNode *node = ast_new_node(NODE_IF_STMT);
    node->data.if_stmt.condition = cond;
    node->data.if_stmt.then_block = then_block;
    node->data.if_stmt.elsif_conditions = NULL;
    node->data.if_stmt.elsif_blocks = NULL;
    node->data.if_stmt.elsif_count = 0;
    node->data.if_stmt.else_block = NULL;
    return node;
}

ASTNode* ast_new_while(ASTNode *cond, ASTNode *body) {
    ASTNode *node = ast_new_node(NODE_WHILE_STMT);
    node->data.while_stmt.condition = cond;
    node->data.while_stmt.body = body;
    return node;
}

ASTNode* ast_new_for(ASTNode *init, ASTNode *cond, ASTNode *update, ASTNode *body) {
    ASTNode *node = ast_new_node(NODE_FOR_STMT);
    node->data.for_stmt.init = init;
    node->data.for_stmt.condition = cond;
    node->data.for_stmt.update = update;
    node->data.for_stmt.body = body;
    return node;
}

ASTNode* ast_new_return(ASTNode *value) {
    ASTNode *node = ast_new_node(NODE_RETURN_STMT);
    node->data.return_stmt.value = value;
    return node;
}

ASTNode* ast_new_expr_stmt(ASTNode *expr) {
    ASTNode *node = ast_new_node(NODE_EXPR_STMT);
    node->data.expr_stmt.expr = expr;
    return node;
}

ASTNode* ast_new_binary_op(const char *op, ASTNode *left, ASTNode *right) {
    ASTNode *node = ast_new_node(NODE_BINARY_OP);
    node->data.binary_op.op = strdup(op);
    node->data.binary_op.left = left;
    node->data.binary_op.right = right;
    return node;
}

ASTNode* ast_new_unary_op(const char *op, ASTNode *operand) {
    ASTNode *node = ast_new_node(NODE_UNARY_OP);
    node->data.unary_op.op = strdup(op);
    node->data.unary_op.operand = operand;
    return node;
}

ASTNode* ast_new_assign(const char *op, ASTNode *target, ASTNode *value) {
    ASTNode *node = ast_new_node(NODE_ASSIGN);
    node->data.assign.op = strdup(op);
    node->data.assign.target = target;
    node->data.assign.value = value;
    return node;
}

ASTNode* ast_new_call(const char *name) {
    ASTNode *node = ast_new_node(NODE_CALL);
    node->data.call.name = strdup(name);
    node->data.call.package_name = NULL;
    node->data.call.args = NULL;
    node->data.call.arg_count = 0;
    return node;
}

ASTNode* ast_new_qualified_call(const char *package, const char *name) {
    ASTNode *node = ast_new_node(NODE_CALL);
    node->data.call.name = strdup(name);
    node->data.call.package_name = strdup(package);
    node->data.call.args = NULL;
    node->data.call.arg_count = 0;
    return node;
}

ASTNode* ast_new_indirect_call(ASTNode *target) {
    ASTNode *node = ast_new_node(NODE_INDIRECT_CALL);
    node->data.indirect_call.target = target;
    node->data.indirect_call.args = NULL;
    node->data.indirect_call.arg_count = 0;
    return node;
}

ASTNode* ast_new_subscript(ASTNode *array, ASTNode *index) {
    ASTNode *node = ast_new_node(NODE_SUBSCRIPT);
    node->data.subscript.array = array;
    node->data.subscript.index = index;
    return node;
}

ASTNode* ast_new_variable(const char *name, char sigil) {
    ASTNode *node = ast_new_node(NODE_VARIABLE);
    node->data.variable.name = strdup(name);
    node->data.variable.sigil = sigil;
    return node;
}

ASTNode* ast_new_int_literal(int64_t value) {
    ASTNode *node = ast_new_node(NODE_INT_LITERAL);
    node->data.int_val = value;
    return node;
}

ASTNode* ast_new_num_literal(double value) {
    ASTNode *node = ast_new_node(NODE_NUM_LITERAL);
    node->data.num_val = value;
    return node;
}

ASTNode* ast_new_str_literal(const char *value) {
    ASTNode *node = ast_new_node(NODE_STR_LITERAL);
    node->data.str_val = strdup(value);
    return node;
}

/* Helper functions */

void ast_add_function(ASTNode *program, ASTNode *function) {
    program->data.program.function_count++;
    program->data.program.functions = realloc(
        program->data.program.functions,
        sizeof(ASTNode*) * program->data.program.function_count
    );
    program->data.program.functions[program->data.program.function_count - 1] = function;
}

void ast_add_param(ASTNode *function, ASTNode *param) {
    function->data.function.param_count++;
    function->data.function.params = realloc(
        function->data.function.params,
        sizeof(ASTNode*) * function->data.function.param_count
    );
    function->data.function.params[function->data.function.param_count - 1] = param;
}

void ast_add_statement(ASTNode *block, ASTNode *stmt) {
    block->data.block.statement_count++;
    block->data.block.statements = realloc(
        block->data.block.statements,
        sizeof(ASTNode*) * block->data.block.statement_count
    );
    block->data.block.statements[block->data.block.statement_count - 1] = stmt;
}

void ast_add_elsif(ASTNode *if_stmt, ASTNode *cond, ASTNode *block) {
    if_stmt->data.if_stmt.elsif_count++;
    
    if_stmt->data.if_stmt.elsif_conditions = realloc(
        if_stmt->data.if_stmt.elsif_conditions,
        sizeof(ASTNode*) * if_stmt->data.if_stmt.elsif_count
    );
    if_stmt->data.if_stmt.elsif_blocks = realloc(
        if_stmt->data.if_stmt.elsif_blocks,
        sizeof(ASTNode*) * if_stmt->data.if_stmt.elsif_count
    );
    
    int idx = if_stmt->data.if_stmt.elsif_count - 1;
    if_stmt->data.if_stmt.elsif_conditions[idx] = cond;
    if_stmt->data.if_stmt.elsif_blocks[idx] = block;
}

void ast_add_arg(ASTNode *call, ASTNode *arg) {
    call->data.call.arg_count++;
    call->data.call.args = realloc(
        call->data.call.args,
        sizeof(ASTNode*) * call->data.call.arg_count
    );
    call->data.call.args[call->data.call.arg_count - 1] = arg;
}

void ast_add_indirect_arg(ASTNode *call, ASTNode *arg) {
    call->data.indirect_call.arg_count++;
    call->data.indirect_call.args = realloc(
        call->data.indirect_call.args,
        sizeof(ASTNode*) * call->data.indirect_call.arg_count
    );
    call->data.indirect_call.args[call->data.indirect_call.arg_count - 1] = arg;
}

void ast_free(ASTNode *node) {
    if (!node) return;
    
    // Free based on node type
    switch (node->type) {
        case NODE_PROGRAM:
            for (int i = 0; i < node->data.program.function_count; i++) {
                ast_free(node->data.program.functions[i]);
            }
            free(node->data.program.functions);
            break;
            
        case NODE_FUNCTION:
            free(node->data.function.name);
            for (int i = 0; i < node->data.function.param_count; i++) {
                ast_free(node->data.function.params[i]);
            }
            free(node->data.function.params);
            ast_free(node->data.function.body);
            break;
            
        case NODE_PARAM:
            free(node->data.param.name);
            break;
            
        case NODE_BLOCK:
            for (int i = 0; i < node->data.block.statement_count; i++) {
                ast_free(node->data.block.statements[i]);
            }
            free(node->data.block.statements);
            break;
            
        case NODE_VAR_DECL:
            free(node->data.var_decl.name);
            ast_free(node->data.var_decl.init);
            break;
            
        case NODE_IF_STMT:
            ast_free(node->data.if_stmt.condition);
            ast_free(node->data.if_stmt.then_block);
            for (int i = 0; i < node->data.if_stmt.elsif_count; i++) {
                ast_free(node->data.if_stmt.elsif_conditions[i]);
                ast_free(node->data.if_stmt.elsif_blocks[i]);
            }
            free(node->data.if_stmt.elsif_conditions);
            free(node->data.if_stmt.elsif_blocks);
            ast_free(node->data.if_stmt.else_block);
            break;
            
        case NODE_WHILE_STMT:
            ast_free(node->data.while_stmt.condition);
            ast_free(node->data.while_stmt.body);
            break;
            
        case NODE_FOR_STMT:
            ast_free(node->data.for_stmt.init);
            ast_free(node->data.for_stmt.condition);
            ast_free(node->data.for_stmt.update);
            ast_free(node->data.for_stmt.body);
            break;
            
        case NODE_RETURN_STMT:
            ast_free(node->data.return_stmt.value);
            break;
            
        case NODE_EXPR_STMT:
            ast_free(node->data.expr_stmt.expr);
            break;
            
        case NODE_BINARY_OP:
            free(node->data.binary_op.op);
            ast_free(node->data.binary_op.left);
            ast_free(node->data.binary_op.right);
            break;
            
        case NODE_UNARY_OP:
            free(node->data.unary_op.op);
            ast_free(node->data.unary_op.operand);
            break;
            
        case NODE_ASSIGN:
            free(node->data.assign.op);
            ast_free(node->data.assign.target);
            ast_free(node->data.assign.value);
            break;
            
        case NODE_CALL:
            free(node->data.call.name);
            for (int i = 0; i < node->data.call.arg_count; i++) {
                ast_free(node->data.call.args[i]);
            }
            free(node->data.call.args);
            break;
            
        case NODE_SUBSCRIPT:
            ast_free(node->data.subscript.array);
            ast_free(node->data.subscript.index);
            break;
            
        case NODE_VARIABLE:
            free(node->data.variable.name);
            break;
            
        case NODE_STR_LITERAL:
            free(node->data.str_val);
            break;
            
        default:
            break;
    }
    
    free(node);
}

/* Struct AST functions */

ASTNode* ast_new_struct_def(const char *name) {
    ASTNode *node = malloc(sizeof(ASTNode));
    node->type = NODE_STRUCT_DEF;
    node->data.struct_def.name = strdup(name);
    node->data.struct_def.fields = malloc(16 * sizeof(ASTNode*));
    node->data.struct_def.field_count = 0;
    node->data.struct_def.total_size = 0;
    return node;
}

ASTNode* ast_new_struct_field(const char *name, DataType type) {
    ASTNode *node = malloc(sizeof(ASTNode));
    node->type = NODE_STRUCT_FIELD;
    node->data.struct_field.name = strdup(name);
    node->data.struct_field.field_type = type;
    node->data.struct_field.offset = 0;  // Will be calculated
    node->data.struct_field.size = 0;    // Will be calculated
    // Initialize function pointer fields
    node->data.struct_field.func_return_type = TYPE_VOID;
    node->data.struct_field.func_param_types = NULL;
    node->data.struct_field.func_param_count = 0;
    return node;
}

ASTNode* ast_new_struct_field_func(const char *name, DataType return_type, 
                                    DataType *param_types, int param_count) {
    ASTNode *node = malloc(sizeof(ASTNode));
    node->type = NODE_STRUCT_FIELD;
    node->data.struct_field.name = strdup(name);
    node->data.struct_field.field_type = TYPE_FUNC;
    node->data.struct_field.offset = 0;
    node->data.struct_field.size = 8;  // Pointer size
    node->data.struct_field.func_return_type = return_type;
    node->data.struct_field.func_param_count = param_count;
    if (param_count > 0 && param_types) {
        node->data.struct_field.func_param_types = malloc(param_count * sizeof(DataType));
        memcpy(node->data.struct_field.func_param_types, param_types, param_count * sizeof(DataType));
    } else {
        node->data.struct_field.func_param_types = NULL;
    }
    return node;
}

ASTNode* ast_new_member_access(ASTNode *object, const char *field) {
    ASTNode *node = malloc(sizeof(ASTNode));
    node->type = NODE_MEMBER_ACCESS;
    node->data.member_access.object = object;
    node->data.member_access.field_name = strdup(field);
    return node;
}

ASTNode* ast_new_clone(ASTNode *source, const char *struct_type) {
    ASTNode *node = malloc(sizeof(ASTNode));
    node->type = NODE_CLONE;
    node->data.clone.source = source;
    node->data.clone.struct_type = struct_type ? strdup(struct_type) : NULL;
    return node;
}

ASTNode* ast_new_func_ref(const char *func_name) {
    ASTNode *node = malloc(sizeof(ASTNode));
    node->type = NODE_FUNC_REF;
    node->data.func_ref.func_name = strdup(func_name);
    return node;
}

// Reference functions
ASTNode* ast_new_ref(ASTNode *target, char ref_type) {
    ASTNode *node = malloc(sizeof(ASTNode));
    node->type = NODE_REF;
    node->data.ref_expr.target = target;
    node->data.ref_expr.ref_type = ref_type;
    return node;
}

ASTNode* ast_new_anon_hash(void) {
    ASTNode *node = malloc(sizeof(ASTNode));
    node->type = NODE_ANON_HASH;
    node->data.anon_hash.keys = malloc(64 * sizeof(char*));
    node->data.anon_hash.values = malloc(64 * sizeof(ASTNode*));
    node->data.anon_hash.pair_count = 0;
    return node;
}

ASTNode* ast_new_anon_array(void) {
    ASTNode *node = malloc(sizeof(ASTNode));
    node->type = NODE_ANON_ARRAY;
    node->data.anon_array.elements = malloc(64 * sizeof(ASTNode*));
    node->data.anon_array.element_count = 0;
    return node;
}

ASTNode* ast_new_deref_hash(ASTNode *ref, ASTNode *key) {
    ASTNode *node = malloc(sizeof(ASTNode));
    node->type = NODE_DEREF_HASH;
    node->data.deref_hash.ref = ref;
    node->data.deref_hash.key = key;
    return node;
}

ASTNode* ast_new_deref_array(ASTNode *ref, ASTNode *index) {
    ASTNode *node = malloc(sizeof(ASTNode));
    node->type = NODE_DEREF_ARRAY;
    node->data.deref_array.ref = ref;
    node->data.deref_array.index = index;
    return node;
}

ASTNode* ast_new_deref_scalar(ASTNode *ref) {
    ASTNode *node = malloc(sizeof(ASTNode));
    node->type = NODE_DEREF_SCALAR;
    node->data.deref_scalar.ref = ref;
    return node;
}

ASTNode* ast_new_deref_to_array(ASTNode *ref) {
    ASTNode *node = malloc(sizeof(ASTNode));
    node->type = NODE_DEREF_TO_ARRAY;
    node->data.deref_to_array.ref = ref;
    return node;
}

ASTNode* ast_new_deref_to_hash(ASTNode *ref) {
    ASTNode *node = malloc(sizeof(ASTNode));
    node->type = NODE_DEREF_TO_HASH;
    node->data.deref_to_hash.ref = ref;
    return node;
}

void ast_add_anon_hash_pair(ASTNode *hash, const char *key, ASTNode *value) {
    if (hash->type != NODE_ANON_HASH) return;
    int idx = hash->data.anon_hash.pair_count++;
    hash->data.anon_hash.keys[idx] = strdup(key);
    hash->data.anon_hash.values[idx] = value;
}

void ast_add_anon_array_element(ASTNode *array, ASTNode *elem) {
    if (array->type != NODE_ANON_ARRAY) return;
    array->data.anon_array.elements[array->data.anon_array.element_count++] = elem;
}

void ast_add_struct_def(ASTNode *program, ASTNode *struct_def) {
    if (program->type != NODE_PROGRAM) return;
    program->data.program.structs[program->data.program.struct_count++] = struct_def;
}

void ast_add_struct_field(ASTNode *struct_def, ASTNode *field) {
    if (struct_def->type != NODE_STRUCT_DEF) return;
    struct_def->data.struct_def.fields[struct_def->data.struct_def.field_count++] = field;
}

/* Package/Module functions */

ASTNode* ast_new_package_decl(const char *name) {
    ASTNode *node = ast_new_node(NODE_PACKAGE_DECL);
    node->data.package_decl.name = strdup(name);
    return node;
}

ASTNode* ast_new_use_stmt(const char *package_name) {
    ASTNode *node = ast_new_node(NODE_USE_STMT);
    node->data.use_stmt.package_name = strdup(package_name);
    node->data.use_stmt.imports = malloc(32 * sizeof(char*));
    node->data.use_stmt.import_count = 0;
    return node;
}

ASTNode* ast_new_use_lib(const char *path) {
    ASTNode *node = ast_new_node(NODE_USE_LIB);
    node->data.use_lib.path = strdup(path);
    return node;
}

void ast_set_package(ASTNode *program, const char *name) {
    if (program->type != NODE_PROGRAM) return;
    if (program->data.program.package_name) {
        free(program->data.program.package_name);
    }
    program->data.program.package_name = strdup(name);
}

void ast_add_use(ASTNode *program, ASTNode *use_stmt) {
    if (program->type != NODE_PROGRAM) return;
    program->data.program.use_stmts[program->data.program.use_count++] = use_stmt;
}

void ast_add_lib_path(ASTNode *program, const char *path) {
    if (program->type != NODE_PROGRAM) return;
    program->data.program.lib_paths[program->data.program.lib_path_count++] = strdup(path);
}

void ast_add_import(ASTNode *use_stmt, const char *func_name) {
    if (use_stmt->type != NODE_USE_STMT) return;
    use_stmt->data.use_stmt.imports[use_stmt->data.use_stmt.import_count++] = strdup(func_name);
}

ASTNode* ast_new_array_literal(void) {
    ASTNode *node = ast_new_node(NODE_ARRAY_LITERAL);
    node->data.array_literal.elements = malloc(64 * sizeof(ASTNode*));
    node->data.array_literal.element_count = 0;
    return node;
}

ASTNode* ast_new_hash_literal(void) {
    ASTNode *node = ast_new_node(NODE_HASH_LITERAL);
    node->data.hash_literal.keys = malloc(64 * sizeof(char*));
    node->data.hash_literal.values = malloc(64 * sizeof(ASTNode*));
    node->data.hash_literal.pair_count = 0;
    return node;
}

void ast_add_array_element(ASTNode *array, ASTNode *elem) {
    if (array->type != NODE_ARRAY_LITERAL) return;
    array->data.array_literal.elements[array->data.array_literal.element_count++] = elem;
}

void ast_add_hash_pair(ASTNode *hash, const char *key, ASTNode *value) {
    if (hash->type != NODE_HASH_LITERAL) return;
    int idx = hash->data.hash_literal.pair_count++;
    hash->data.hash_literal.keys[idx] = strdup(key);
    hash->data.hash_literal.values[idx] = value;
}

/* Exception handling functions */

ASTNode* ast_new_try_catch(ASTNode *try_block, const char *catch_var, ASTNode *catch_block) {
    ASTNode *node = ast_new_node(NODE_TRY_CATCH);
    node->data.try_catch.try_block = try_block;
    node->data.try_catch.catch_var = strdup(catch_var);
    node->data.try_catch.catch_block = catch_block;
    return node;
}

ASTNode* ast_new_throw(ASTNode *expr) {
    ASTNode *node = ast_new_node(NODE_THROW);
    node->data.throw_stmt.expr = expr;
    return node;
}

/* Goto/Label functions */

ASTNode* ast_new_label(const char *name) {
    ASTNode *node = ast_new_node(NODE_LABEL);
    node->data.label.name = strdup(name);
    return node;
}

ASTNode* ast_new_goto(const char *target) {
    ASTNode *node = ast_new_node(NODE_GOTO);
    node->data.goto_stmt.target = strdup(target);
    return node;
}

ASTNode* ast_new_last_label(const char *label) {
    ASTNode *node = ast_new_node(NODE_LAST_LABEL);
    node->data.loop_label.label = strdup(label);
    return node;
}

ASTNode* ast_new_next_label(const char *label) {
    ASTNode *node = ast_new_node(NODE_NEXT_LABEL);
    node->data.loop_label.label = strdup(label);
    return node;
}

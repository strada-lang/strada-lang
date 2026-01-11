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

/* parser.c - Strada Bootstrap Parser */
#define _POSIX_C_SOURCE 200809L
#include "parser.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

static void advance(Parser *p);

typedef struct {
    DataType type;
    char *struct_name;  // NULL for non-struct types
} TypeInfo;

static TypeInfo parse_type_info(Parser *p);
static DataType parse_type(Parser *p);
static ASTNode* parse_struct(Parser *p);
static ASTNode* parse_function(Parser *p);
static ASTNode* parse_block(Parser *p);
static ASTNode* parse_statement(Parser *p);
static ASTNode* parse_expression(Parser *p);

Parser* parser_new(Lexer *lexer) {
    Parser *p = malloc(sizeof(Parser));
    p->lexer = lexer;
    p->current = lexer_next(lexer);
    p->peek = lexer_next(lexer);
    p->peek2 = lexer_next(lexer);
    
    // Initialize struct registry
    p->structs.capacity = 32;
    p->structs.count = 0;
    p->structs.names = malloc(p->structs.capacity * sizeof(char*));
    
    return p;
}

void parser_free(Parser *p) {
    if (p) {
        // Free struct registry
        for (int i = 0; i < p->structs.count; i++) {
            free(p->structs.names[i]);
        }
        free(p->structs.names);
        
        free(p->current);
        free(p->peek);
        free(p->peek2);
        free(p);
    }
}

/* Register a struct type name during parsing */
static void parser_register_struct(Parser *p, const char *name) {
    // Check capacity
    if (p->structs.count >= p->structs.capacity) {
        p->structs.capacity *= 2;
        p->structs.names = realloc(p->structs.names, p->structs.capacity * sizeof(char*));
    }
    
    // Add struct name
    p->structs.names[p->structs.count++] = strdup(name);
}

/* Check if a name is a registered struct type */
static int parser_is_struct_type(Parser *p, const char *name) {
    for (int i = 0; i < p->structs.count; i++) {
        if (strcmp(p->structs.names[i], name) == 0) {
            return 1;
        }
    }
    return 0;
}

static void advance(Parser *p) {
    free(p->current);
    p->current = p->peek;
    p->peek = p->peek2;
    p->peek2 = lexer_next(p->lexer);
}

void parser_expect(Parser *p, TokenType type) {
    if (p->current->type != type) {
        fprintf(stderr, "Parse error at line %d: expected token type %d, got %d\n",
                p->current->line, type, p->current->type);
        exit(1);
    }
    advance(p);
}

void parser_error(Parser *p, const char *message) {
    fprintf(stderr, "Parse error at line %d: %s\n", p->current->line, message);
    exit(1);
}

static TypeInfo parse_type_info(Parser *p) {
    TypeInfo info;
    info.struct_name = NULL;
    
    switch (p->current->type) {
        case TOK_INT: info.type = TYPE_INT; break;
        case TOK_NUM: info.type = TYPE_NUM; break;
        case TOK_STR: info.type = TYPE_STR; break;
        case TOK_SCALAR: info.type = TYPE_SCALAR; break;
        case TOK_ARRAY: info.type = TYPE_ARRAY; break;
        case TOK_HASH: info.type = TYPE_HASH; break;
        case TOK_VOID: info.type = TYPE_VOID; break;
        
        /* Extended C types */
        case TOK_CHAR: info.type = TYPE_CHAR; break;
        case TOK_SHORT: info.type = TYPE_SHORT; break;
        case TOK_LONG: info.type = TYPE_LONG; break;
        case TOK_FLOAT: info.type = TYPE_FLOAT; break;
        case TOK_BOOL: info.type = TYPE_BOOL; break;
        case TOK_UCHAR: info.type = TYPE_UCHAR; break;
        case TOK_USHORT: info.type = TYPE_USHORT; break;
        case TOK_UINT: info.type = TYPE_UINT; break;
        case TOK_ULONG: info.type = TYPE_ULONG; break;
        case TOK_I8: info.type = TYPE_I8; break;
        case TOK_I16: info.type = TYPE_I16; break;
        case TOK_I32: info.type = TYPE_I32; break;
        case TOK_I64: info.type = TYPE_I64; break;
        case TOK_U8: info.type = TYPE_U8; break;
        case TOK_U16: info.type = TYPE_U16; break;
        case TOK_U32: info.type = TYPE_U32; break;
        case TOK_U64: info.type = TYPE_U64; break;
        case TOK_SIZE: info.type = TYPE_SIZE; break;
        case TOK_PTR: info.type = TYPE_PTR; break;
        
        case TOK_IDENT:
            // Check if this is a struct type name
            if (parser_is_struct_type(p, p->current->lexeme)) {
                info.type = TYPE_STRUCT;
                info.struct_name = strdup(p->current->lexeme);
            } else {
                parser_error(p, "Unknown type");
                info.type = TYPE_VOID;
            }
            break;
        default:
            parser_error(p, "Expected type");
            info.type = TYPE_VOID;
            break;
    }
    
    advance(p);
    return info;
}

static DataType parse_type(Parser *p) {
    TypeInfo info = parse_type_info(p);
    if (info.struct_name) {
        free(info.struct_name);  // Not needed in simplified version
    }
    return info.type;
}

static char parse_sigil(Parser *p) {
    char sigil;
    
    if (p->current->type == TOK_DOLLAR) {
        sigil = '$';
    } else if (p->current->type == TOK_AT) {
        sigil = '@';
    } else if (p->current->type == TOK_PERCENT) {
        sigil = '%';
    } else {
        parser_error(p, "Expected sigil ($, @, or %)");
        return '$';
    }
    
    advance(p);
    return sigil;
}

/* Helper to parse package name with :: separators */
static char* parse_package_name(Parser *p) {
    if (p->current->type != TOK_IDENT) {
        parser_error(p, "Expected package name");
        return NULL;
    }
    
    // Start with first identifier
    char buffer[512];
    strcpy(buffer, p->current->lexeme);
    advance(p);
    
    // Append ::Name parts
    while (p->current->type == TOK_COLONCOLON) {
        advance(p);  // skip ::
        if (p->current->type != TOK_IDENT) {
            parser_error(p, "Expected identifier after ::");
            return NULL;
        }
        strcat(buffer, "::");
        strcat(buffer, p->current->lexeme);
        advance(p);
    }
    
    return strdup(buffer);
}

ASTNode* parser_parse(Parser *p) {
    ASTNode *program = ast_new_program();
    
    while (p->current->type != TOK_EOF) {
        if (p->current->type == TOK_PACKAGE) {
            // package Name::Space;
            advance(p);  // skip 'package'
            char *pkg_name = parse_package_name(p);
            ast_set_package(program, pkg_name);
            free(pkg_name);
            parser_expect(p, TOK_SEMI);
            
        } else if (p->current->type == TOK_USE) {
            advance(p);  // skip 'use'
            
            if (p->current->type == TOK_LIB) {
                // use lib "/path";
                advance(p);  // skip 'lib'
                if (p->current->type != TOK_STR_LITERAL) {
                    parser_error(p, "Expected string path after 'use lib'");
                }
                ast_add_lib_path(program, p->current->value.str_val);
                advance(p);
                parser_expect(p, TOK_SEMI);
                
            } else {
                // use Package::Name;
                // use Package::Name qw(func1 func2);
                char *pkg_name = parse_package_name(p);
                ASTNode *use_node = ast_new_use_stmt(pkg_name);
                free(pkg_name);
                
                // Check for qw() selective imports
                if (p->current->type == TOK_QW) {
                    advance(p);  // skip 'qw'
                    parser_expect(p, TOK_LPAREN);
                    
                    // Parse list of function names
                    while (p->current->type != TOK_RPAREN) {
                        if (p->current->type == TOK_IDENT) {
                            ast_add_import(use_node, p->current->lexeme);
                            advance(p);
                        } else {
                            parser_error(p, "Expected function name in qw()");
                        }
                        
                        // Optional comma between names
                        if (p->current->type == TOK_COMMA) {
                            advance(p);
                        }
                    }
                    parser_expect(p, TOK_RPAREN);
                }
                
                ast_add_use(program, use_node);
                parser_expect(p, TOK_SEMI);
            }
            
        } else if (p->current->type == TOK_STRUCT) {
            ASTNode *struct_def = parse_struct(p);
            ast_add_struct_def(program, struct_def);
        } else if (p->current->type == TOK_EXTERN) {
            // extern func - C linkage
            advance(p);  // skip 'extern'
            if (p->current->type != TOK_FUNC) {
                parser_error(p, "Expected 'func' after 'extern'");
            }
            ASTNode *func = parse_function(p);
            func->data.function.is_extern = 1;
            ast_add_function(program, func);
        } else if (p->current->type == TOK_FUNC) {
            ASTNode *func = parse_function(p);
            ast_add_function(program, func);
        } else {
            parser_error(p, "Expected package, use, struct or function definition");
        }
    }
    
    return program;
}

static ASTNode* parse_function(Parser *p) {
    parser_expect(p, TOK_FUNC);
    
    if (p->current->type != TOK_IDENT) {
        parser_error(p, "Expected function name");
    }
    
    char *name = strdup(p->current->lexeme);
    advance(p);
    
    parser_expect(p, TOK_LPAREN);
    
    // Parse parameters
    ASTNode *func = NULL;
    int min_args = 0;
    int found_optional = 0;
    int is_variadic = 0;
    
    while (p->current->type != TOK_RPAREN) {
        // Check for variadic parameter (...)
        if (p->current->type == TOK_ELLIPSIS) {
            advance(p);  // Skip ...
            is_variadic = 1;
            
            TypeInfo param_info = parse_type_info(p);
            char sigil = parse_sigil(p);
            
            if (p->current->type != TOK_IDENT) {
                parser_error(p, "Expected parameter name");
            }
            
            ASTNode *param = ast_new_param(p->current->lexeme, param_info.type, sigil);
            if (param_info.type == TYPE_STRUCT) {
                param->data.param.struct_name = param_info.struct_name;
            }
            param->data.param.is_optional = 1;  // Variadic is optional
            advance(p);
            
            if (!func) {
                func = ast_new_function(name, TYPE_VOID);
            }
            func->data.function.is_variadic = 1;
            ast_add_param(func, param);
            
            // Variadic must be last parameter
            if (p->current->type == TOK_COMMA) {
                parser_error(p, "Variadic parameter must be last");
            }
            break;
        }
        
        TypeInfo param_info = parse_type_info(p);
        char sigil = parse_sigil(p);
        
        if (p->current->type != TOK_IDENT) {
            parser_error(p, "Expected parameter name");
        }
        
        ASTNode *param = ast_new_param(p->current->lexeme, param_info.type, sigil);
        if (param_info.type == TYPE_STRUCT) {
            param->data.param.struct_name = param_info.struct_name;
        }
        advance(p);
        
        // Check for default value (optional parameter)
        if (p->current->type == TOK_ASSIGN) {
            advance(p); // Skip '='
            
            param->data.param.is_optional = 1;
            found_optional = 1;
            
            // Parse default value (must be a literal)
            if (p->current->type == TOK_INT_LITERAL) {
                param->data.param.default_value = ast_new_int_literal(atoi(p->current->lexeme));
                advance(p);
            } else if (p->current->type == TOK_NUM_LITERAL) {
                param->data.param.default_value = ast_new_num_literal(atof(p->current->lexeme));
                advance(p);
            } else if (p->current->type == TOK_STR_LITERAL) {
                param->data.param.default_value = ast_new_str_literal(p->current->lexeme);
                advance(p);
            } else {
                parser_error(p, "Expected literal default value");
            }
        } else {
            if (found_optional) {
                parser_error(p, "Required parameters cannot follow optional parameters");
            }
            min_args++;
        }
        
        if (!func) {
            func = ast_new_function(name, TYPE_VOID); // Temporary
        }
        ast_add_param(func, param);
        
        if (p->current->type == TOK_COMMA) {
            advance(p);
        }
    }
    
    parser_expect(p, TOK_RPAREN);
    
    // Parse return type
    TypeInfo return_info = parse_type_info(p);
    
    if (!func) {
        func = ast_new_function(name, return_info.type);
    } else {
        func->data.function.return_type = return_info.type;
    }
    
    // Store struct name for struct return types
    if (return_info.type == TYPE_STRUCT) {
        func->data.function.return_struct_name = return_info.struct_name;
    }
    
    // Set minimum args (for optional parameters)
    func->data.function.min_args = min_args;
    
    // Parse body
    func->data.function.body = parse_block(p);
    
    free(name);
    return func;
}

static ASTNode* parse_block(Parser *p) {
    parser_expect(p, TOK_LBRACE);
    
    ASTNode *block = ast_new_block();
    
    while (p->current->type != TOK_RBRACE && p->current->type != TOK_EOF) {
        ASTNode *stmt = parse_statement(p);
        if (stmt) {
            ast_add_statement(block, stmt);
        }
    }
    
    parser_expect(p, TOK_RBRACE);
    
    return block;
}

static ASTNode* parse_var_decl(Parser *p);
static ASTNode* parse_if_stmt(Parser *p);
static ASTNode* parse_while_stmt(Parser *p);
static ASTNode* parse_for_stmt(Parser *p);
static ASTNode* parse_return_stmt(Parser *p);

static ASTNode* parse_statement(Parser *p) {
    switch (p->current->type) {
        case TOK_MY:
            return parse_var_decl(p);
        case TOK_IF:
            return parse_if_stmt(p);
        case TOK_WHILE:
            return parse_while_stmt(p);
        case TOK_FOR:
            return parse_for_stmt(p);
        case TOK_RETURN:
            return parse_return_stmt(p);
        case TOK_LAST: {
            advance(p);
            parser_expect(p, TOK_SEMI);
            return ast_new_node(NODE_LAST_STMT);
        }
        case TOK_NEXT: {
            advance(p);
            parser_expect(p, TOK_SEMI);
            return ast_new_node(NODE_NEXT_STMT);
        }
        case TOK_TRY: {
            advance(p);  // consume 'try'
            parser_expect(p, TOK_LBRACE);
            ASTNode *try_block = parse_block(p);

            parser_expect(p, TOK_CATCH);
            parser_expect(p, TOK_LPAREN);
            parser_expect(p, TOK_DOLLAR);
            if (p->current->type != TOK_IDENT) {
                parser_error(p, "Expected variable name after $ in catch");
            }
            char *catch_var = strdup(p->current->lexeme);
            advance(p);
            parser_expect(p, TOK_RPAREN);
            parser_expect(p, TOK_LBRACE);
            ASTNode *catch_block = parse_block(p);

            return ast_new_try_catch(try_block, catch_var, catch_block);
        }
        case TOK_THROW: {
            advance(p);  // consume 'throw'
            ASTNode *expr = parse_expression(p);
            parser_expect(p, TOK_SEMI);
            return ast_new_throw(expr);
        }
        case TOK_GOTO: {
            advance(p);  // consume 'goto'
            if (p->current->type != TOK_IDENT) {
                parser_error(p, "Expected label name after goto");
            }
            char *target = strdup(p->current->lexeme);
            advance(p);
            parser_expect(p, TOK_SEMI);
            return ast_new_goto(target);
        }
        case TOK_LBRACE:
            return parse_block(p);
        default: {
            // Check for label: IDENT followed by single colon (not ::)
            if (p->current->type == TOK_IDENT && p->peek->type == TOK_COLON) {
                char *label_name = strdup(p->current->lexeme);
                advance(p);  // consume identifier
                advance(p);  // consume colon
                return ast_new_label(label_name);
            }
            ASTNode *expr = parse_expression(p);
            parser_expect(p, TOK_SEMI);
            return ast_new_expr_stmt(expr);
        }
    }
}

static ASTNode* parse_var_decl(Parser *p) {
    parser_expect(p, TOK_MY);
    
    TypeInfo type_info = parse_type_info(p);
    char sigil = parse_sigil(p);
    
    if (p->current->type != TOK_IDENT) {
        parser_error(p, "Expected variable name");
    }
    
    char *name = strdup(p->current->lexeme);
    advance(p);
    
    ASTNode *init = NULL;
    if (p->current->type == TOK_ASSIGN) {
        advance(p);
        init = parse_expression(p);
    }
    
    parser_expect(p, TOK_SEMI);
    
    ASTNode *decl = ast_new_var_decl(name, type_info.type, sigil, init);
    if (type_info.type == TYPE_STRUCT) {
        decl->data.var_decl.struct_name = type_info.struct_name;
    }
    free(name);
    return decl;
}

static ASTNode* parse_if_stmt(Parser *p) {
    parser_expect(p, TOK_IF);
    parser_expect(p, TOK_LPAREN);
    
    ASTNode *cond = parse_expression(p);
    
    parser_expect(p, TOK_RPAREN);
    
    ASTNode *then_block = parse_block(p);
    ASTNode *if_stmt = ast_new_if(cond, then_block);
    
    // Handle elsif
    while (p->current->type == TOK_ELSIF) {
        advance(p);
        parser_expect(p, TOK_LPAREN);
        ASTNode *elsif_cond = parse_expression(p);
        parser_expect(p, TOK_RPAREN);
        ASTNode *elsif_block = parse_block(p);
        ast_add_elsif(if_stmt, elsif_cond, elsif_block);
    }
    
    // Handle else
    if (p->current->type == TOK_ELSE) {
        advance(p);
        if_stmt->data.if_stmt.else_block = parse_block(p);
    }
    
    return if_stmt;
}

static ASTNode* parse_while_stmt(Parser *p) {
    parser_expect(p, TOK_WHILE);
    parser_expect(p, TOK_LPAREN);
    
    ASTNode *cond = parse_expression(p);
    
    parser_expect(p, TOK_RPAREN);
    
    ASTNode *body = parse_block(p);
    
    return ast_new_while(cond, body);
}

static ASTNode* parse_for_stmt(Parser *p) {
    parser_expect(p, TOK_FOR);
    parser_expect(p, TOK_LPAREN);
    
    // Init (can be a variable declaration or expression)
    ASTNode *init = NULL;
    if (p->current->type == TOK_MY) {
        init = parse_var_decl(p);
        // var_decl already consumed the semicolon
    } else {
        init = parse_expression(p);
        parser_expect(p, TOK_SEMI);
    }
    
    ASTNode *cond = parse_expression(p);
    parser_expect(p, TOK_SEMI);
    
    ASTNode *update = parse_expression(p);
    parser_expect(p, TOK_RPAREN);
    
    ASTNode *body = parse_block(p);
    
    return ast_new_for(init, cond, update, body);
}

static ASTNode* parse_return_stmt(Parser *p) {
    parser_expect(p, TOK_RETURN);
    
    ASTNode *value = NULL;
    if (p->current->type != TOK_SEMI) {
        value = parse_expression(p);
    }
    
    parser_expect(p, TOK_SEMI);
    
    return ast_new_return(value);
}

// Forward declarations for expression parsing
static ASTNode* parse_assignment(Parser *p);
static ASTNode* parse_logical_or(Parser *p);
static ASTNode* parse_logical_and(Parser *p);
static ASTNode* parse_equality(Parser *p);
static ASTNode* parse_relational(Parser *p);
static ASTNode* parse_additive(Parser *p);
static ASTNode* parse_multiplicative(Parser *p);
static ASTNode* parse_unary(Parser *p);
static ASTNode* parse_postfix(Parser *p);
static ASTNode* parse_primary(Parser *p);

static ASTNode* parse_expression(Parser *p) {
    return parse_assignment(p);
}

static ASTNode* parse_assignment(Parser *p) {
    ASTNode *expr = parse_logical_or(p);
    
    if (p->current->type == TOK_ASSIGN ||
        p->current->type == TOK_PLUS_ASSIGN ||
        p->current->type == TOK_MINUS_ASSIGN ||
        p->current->type == TOK_DOT_ASSIGN) {
        
        char *op = strdup(p->current->lexeme);
        advance(p);
        ASTNode *value = parse_assignment(p);
        return ast_new_assign(op, expr, value);
    }
    
    return expr;
}

static ASTNode* parse_logical_or(Parser *p) {
    ASTNode *left = parse_logical_and(p);
    
    while (p->current->type == TOK_OR) {
        char *op = strdup(p->current->lexeme);
        advance(p);
        ASTNode *right = parse_logical_and(p);
        left = ast_new_binary_op(op, left, right);
    }
    
    return left;
}

static ASTNode* parse_logical_and(Parser *p) {
    ASTNode *left = parse_equality(p);
    
    while (p->current->type == TOK_AND) {
        char *op = strdup(p->current->lexeme);
        advance(p);
        ASTNode *right = parse_equality(p);
        left = ast_new_binary_op(op, left, right);
    }
    
    return left;
}

static ASTNode* parse_equality(Parser *p) {
    ASTNode *left = parse_relational(p);
    
    while (p->current->type == TOK_EQ || p->current->type == TOK_NE ||
           p->current->type == TOK_STR_EQ || p->current->type == TOK_STR_NE) {
        char *op = strdup(p->current->lexeme);
        advance(p);
        ASTNode *right = parse_relational(p);
        left = ast_new_binary_op(op, left, right);
    }
    
    return left;
}

static ASTNode* parse_relational(Parser *p) {
    ASTNode *left = parse_additive(p);
    
    while (p->current->type == TOK_LT || p->current->type == TOK_GT ||
           p->current->type == TOK_LE || p->current->type == TOK_GE ||
           p->current->type == TOK_STR_LT || p->current->type == TOK_STR_GT ||
           p->current->type == TOK_STR_LE || p->current->type == TOK_STR_GE) {
        char *op = strdup(p->current->lexeme);
        advance(p);
        ASTNode *right = parse_additive(p);
        left = ast_new_binary_op(op, left, right);
    }
    
    return left;
}

static ASTNode* parse_additive(Parser *p) {
    ASTNode *left = parse_multiplicative(p);
    
    while (p->current->type == TOK_PLUS || p->current->type == TOK_MINUS ||
           p->current->type == TOK_DOT) {
        char *op = strdup(p->current->lexeme);
        advance(p);
        ASTNode *right = parse_multiplicative(p);
        left = ast_new_binary_op(op, left, right);
    }
    
    return left;
}

static ASTNode* parse_multiplicative(Parser *p) {
    ASTNode *left = parse_unary(p);
    
    while (p->current->type == TOK_MULT || p->current->type == TOK_DIV ||
           p->current->type == TOK_MOD) {
        char *op = strdup(p->current->lexeme);
        advance(p);
        ASTNode *right = parse_unary(p);
        left = ast_new_binary_op(op, left, right);
    }
    
    return left;
}

static ASTNode* parse_unary(Parser *p) {
    if (p->current->type == TOK_MINUS || p->current->type == TOK_NOT) {
        char *op = strdup(p->current->lexeme);
        advance(p);
        ASTNode *operand = parse_unary(p);
        return ast_new_unary_op(op, operand);
    }
    
    return parse_postfix(p);
}

static ASTNode* parse_postfix(Parser *p) {
    ASTNode *expr = parse_primary(p);
    
    while (1) {
        if (p->current->type == TOK_LPAREN) {
            // Function call - could be direct or indirect
            advance(p);
            
            ASTNode *call_node;
            if (expr->type == NODE_VARIABLE) {
                // Direct call: func()
                call_node = ast_new_call(expr->data.variable.name);
                ast_free(expr);
                
                while (p->current->type != TOK_RPAREN && p->current->type != TOK_EOF) {
                    ASTNode *arg = parse_expression(p);
                    ast_add_arg(call_node, arg);
                    
                    if (p->current->type == TOK_COMMA) {
                        advance(p);
                    }
                }
            } else if (expr->type == NODE_CALL) {
                // Qualified call: Package::Name::func()
                // The call node was already created, just add arguments
                call_node = expr;
                
                while (p->current->type != TOK_RPAREN && p->current->type != TOK_EOF) {
                    ASTNode *arg = parse_expression(p);
                    ast_add_arg(call_node, arg);
                    
                    if (p->current->type == TOK_COMMA) {
                        advance(p);
                    }
                }
            } else if (expr->type == NODE_MEMBER_ACCESS) {
                // Indirect call through function pointer: obj->callback()
                call_node = ast_new_indirect_call(expr);
                
                while (p->current->type != TOK_RPAREN && p->current->type != TOK_EOF) {
                    ASTNode *arg = parse_expression(p);
                    ast_add_indirect_arg(call_node, arg);
                    
                    if (p->current->type == TOK_COMMA) {
                        advance(p);
                    }
                }
            } else {
                parser_error(p, "Invalid function call target");
                call_node = NULL;
            }
            
            parser_expect(p, TOK_RPAREN);
            expr = call_node;
            
        } else if (p->current->type == TOK_LBRACKET) {
            // Array subscript: $array[index]
            advance(p);
            ASTNode *index = parse_expression(p);
            parser_expect(p, TOK_RBRACKET);
            expr = ast_new_subscript(expr, index);
            
        } else if (p->current->type == TOK_LBRACE) {
            // Hash subscript: $hash{key} or $hash{"key"}
            advance(p);
            
            ASTNode *key;
            // Support bare identifiers as hash keys (Perl style)
            if (p->current->type == TOK_IDENT && p->peek->type == TOK_RBRACE) {
                // Bare key: $hash{name} -> treat as $hash{"name"}
                key = ast_new_str_literal(p->current->lexeme);
                advance(p);
            } else {
                // Expression key: $hash{"name"} or $hash{$var}
                key = parse_expression(p);
            }
            parser_expect(p, TOK_RBRACE);
            
            // Create hash access node with expression key
            ASTNode *node = malloc(sizeof(ASTNode));
            node->type = NODE_HASH_ACCESS;
            node->data.subscript.array = expr;  // Reuse subscript struct
            node->data.subscript.index = key;
            expr = node;
            
        } else if (p->current->type == TOK_ARROW) {
            // Member access (struct->field) or reference dereference
            advance(p);
            
            if (p->current->type == TOK_LBRACE) {
                // $ref->{key} - hash reference dereference
                advance(p);  // skip '{'
                
                ASTNode *key;
                if (p->current->type == TOK_IDENT && p->peek->type == TOK_RBRACE) {
                    // Bare key
                    key = ast_new_str_literal(p->current->lexeme);
                    advance(p);
                } else {
                    key = parse_expression(p);
                }
                parser_expect(p, TOK_RBRACE);
                expr = ast_new_deref_hash(expr, key);
                
            } else if (p->current->type == TOK_LBRACKET) {
                // $ref->[index] - array reference dereference
                advance(p);  // skip '['
                ASTNode *index = parse_expression(p);
                parser_expect(p, TOK_RBRACKET);
                expr = ast_new_deref_array(expr, index);
                
            } else if (p->current->type == TOK_IDENT) {
                // $struct->field - struct member access
                char *field_name = strdup(p->current->lexeme);
                advance(p);
                expr = ast_new_member_access(expr, field_name);
                free(field_name);
            } else {
                parser_error(p, "Expected field name, {key}, or [index] after ->");
            }
            
        } else {
            break;
        }
    }
    
    return expr;
}

static ASTNode* parse_primary(Parser *p) {
    switch (p->current->type) {
        case TOK_INT_LITERAL: {
            ASTNode *node = ast_new_int_literal(p->current->value.int_val);
            advance(p);
            return node;
        }
        
        case TOK_NUM_LITERAL: {
            ASTNode *node = ast_new_num_literal(p->current->value.num_val);
            advance(p);
            return node;
        }
        
        case TOK_STR_LITERAL: {
            ASTNode *node = ast_new_str_literal(p->current->value.str_val);
            advance(p);
            return node;
        }
        
        case TOK_BACKSLASH: {
            // \$var, \@arr, \%hash - create reference
            advance(p);  // skip '\'
            
            char ref_type = '$';  // default to scalar ref
            if (p->current->type == TOK_DOLLAR) {
                ref_type = '$';
                advance(p);
            } else if (p->current->type == TOK_AT) {
                ref_type = '@';
                advance(p);
            } else if (p->current->type == TOK_PERCENT) {
                ref_type = '%';
                advance(p);
            } else {
                parser_error(p, "Expected $, @, or % after \\");
            }
            
            if (p->current->type != TOK_IDENT) {
                parser_error(p, "Expected variable name after sigil");
            }
            
            ASTNode *target = ast_new_variable(p->current->lexeme, ref_type);
            advance(p);
            return ast_new_ref(target, ref_type);
        }
        
        case TOK_LBRACE: {
            // Anonymous hash: { key => value, ... }
            // Check if this looks like a hash:
            // { } - empty hash
            // { ident => - hash with bareword key
            // { "string" => - hash with string key
            
            int is_hash = 0;
            if (p->peek->type == TOK_RBRACE) {
                is_hash = 1;  // Empty hash {}
            } else if ((p->peek->type == TOK_IDENT || p->peek->type == TOK_STR_LITERAL) 
                       && p->peek2->type == TOK_FAT_ARROW) {
                is_hash = 1;  // key => value
            }
            
            if (!is_hash) {
                parser_error(p, "Unexpected { in expression (anonymous hash needs key => value format)");
                return NULL;
            }
            
            advance(p);  // skip '{'
            
            ASTNode *hash = ast_new_anon_hash();
            
            if (p->current->type != TOK_RBRACE) {
                // Parse first pair
                char *key;
                if (p->current->type == TOK_IDENT) {
                    key = strdup(p->current->lexeme);
                    advance(p);
                } else if (p->current->type == TOK_STR_LITERAL) {
                    key = strdup(p->current->value.str_val);
                    advance(p);
                } else {
                    parser_error(p, "Expected key in anonymous hash");
                    return NULL;
                }
                
                parser_expect(p, TOK_FAT_ARROW);  // =>
                ASTNode *value = parse_expression(p);
                ast_add_anon_hash_pair(hash, key, value);
                free(key);
                
                while (p->current->type == TOK_COMMA) {
                    advance(p);  // skip ','
                    if (p->current->type == TOK_RBRACE) break;  // trailing comma
                    
                    if (p->current->type == TOK_IDENT) {
                        key = strdup(p->current->lexeme);
                        advance(p);
                    } else if (p->current->type == TOK_STR_LITERAL) {
                        key = strdup(p->current->value.str_val);
                        advance(p);
                    } else {
                        parser_error(p, "Expected key in anonymous hash");
                        return NULL;
                    }
                    
                    parser_expect(p, TOK_FAT_ARROW);
                    value = parse_expression(p);
                    ast_add_anon_hash_pair(hash, key, value);
                    free(key);
                }
            }
            
            parser_expect(p, TOK_RBRACE);
            return hash;
        }
        
        case TOK_LBRACKET: {
            // Anonymous array: [ elem, elem, ... ]
            advance(p);  // skip '['
            
            ASTNode *array = ast_new_anon_array();
            
            if (p->current->type != TOK_RBRACKET) {
                ASTNode *elem = parse_expression(p);
                ast_add_anon_array_element(array, elem);
                
                while (p->current->type == TOK_COMMA) {
                    advance(p);  // skip ','
                    if (p->current->type == TOK_RBRACKET) break;  // trailing comma
                    
                    elem = parse_expression(p);
                    ast_add_anon_array_element(array, elem);
                }
            }
            
            parser_expect(p, TOK_RBRACKET);
            return array;
        }
        
        case TOK_AMPERSAND: {
            // &func_name - function reference
            advance(p);  // skip '&'
            if (p->current->type != TOK_IDENT) {
                parser_error(p, "Expected function name after &");
            }
            char *func_name = strdup(p->current->lexeme);
            advance(p);
            ASTNode *node = ast_new_func_ref(func_name);
            free(func_name);
            return node;
        }
        
        case TOK_CLONE: {
            // clone($struct) - deep copy a struct
            advance(p);  // skip 'clone'
            parser_expect(p, TOK_LPAREN);
            
            // Parse the source expression (should be a struct variable)
            ASTNode *source = parse_expression(p);
            
            // Try to determine struct type from variable
            char *struct_type = NULL;
            if (source->type == NODE_VARIABLE) {
                // We'll resolve the type at codegen time
            }
            
            parser_expect(p, TOK_RPAREN);
            return ast_new_clone(source, struct_type);
        }
        
        case TOK_DOLLAR:
        case TOK_AT:
        case TOK_PERCENT: {
            char sigil = parse_sigil(p);
            
            // Check for $$ref (scalar dereference)
            if (sigil == '$' && p->current->type == TOK_DOLLAR) {
                advance(p);  // skip second $
                if (p->current->type != TOK_IDENT) {
                    parser_error(p, "Expected variable name after $$");
                }
                ASTNode *ref = ast_new_variable(p->current->lexeme, '$');
                advance(p);
                return ast_new_deref_scalar(ref);
            }
            
            // Check for @{$ref} (array dereference)
            if (sigil == '@' && p->current->type == TOK_LBRACE) {
                advance(p);  // skip '{'
                ASTNode *ref = parse_expression(p);
                parser_expect(p, TOK_RBRACE);
                return ast_new_deref_to_array(ref);
            }
            
            // Check for %{$ref} (hash dereference)
            if (sigil == '%' && p->current->type == TOK_LBRACE) {
                advance(p);  // skip '{'
                ASTNode *ref = parse_expression(p);
                parser_expect(p, TOK_RBRACE);
                return ast_new_deref_to_hash(ref);
            }
            
            if (p->current->type != TOK_IDENT) {
                parser_error(p, "Expected variable name");
            }
            ASTNode *node = ast_new_variable(p->current->lexeme, sigil);
            advance(p);
            return node;
        }
        
        case TOK_IDENT: {
            // Could be: variable, function call, or Package::Name::func
            char *first = strdup(p->current->lexeme);
            advance(p);
            
            // Check for :: (qualified name)
            if (p->current->type == TOK_COLONCOLON) {
                // Build package path: Name::Space::func
                char package_path[512];
                strcpy(package_path, first);
                
                while (p->current->type == TOK_COLONCOLON) {
                    advance(p);  // skip ::
                    if (p->current->type != TOK_IDENT) {
                        parser_error(p, "Expected identifier after ::");
                    }
                    
                    // Check if this is the last part (function name)
                    if (p->peek->type != TOK_COLONCOLON) {
                        // This is the function name
                        char *func_name = strdup(p->current->lexeme);
                        advance(p);
                        
                        // Create qualified call node
                        ASTNode *node = ast_new_qualified_call(package_path, func_name);
                        free(first);
                        free(func_name);
                        return node;
                    }
                    
                    // More package path
                    strcat(package_path, "::");
                    strcat(package_path, p->current->lexeme);
                    advance(p);
                }
            }
            
            ASTNode *node = ast_new_variable(first, '$'); // Default to scalar
            free(first);
            return node;
        }
        
        case TOK_LPAREN: {
            advance(p);
            // Check for empty () - creates empty hash/array depending on context
            if (p->current->type == TOK_RPAREN) {
                advance(p);
                // Return an empty hash literal for () in assignment context
                return ast_new_hash_literal();
            }
            ASTNode *expr = parse_expression(p);
            parser_expect(p, TOK_RPAREN);
            return expr;
        }
        
        default:
            parser_error(p, "Unexpected token in expression");
            return NULL;
    }
}

/* Parse struct definition */
static ASTNode* parse_struct(Parser *p) {
    parser_expect(p, TOK_STRUCT);
    
    if (p->current->type != TOK_IDENT) {
        parser_error(p, "Expected struct name");
    }
    
    char *name = strdup(p->current->lexeme);
    advance(p);
    
    // Register the struct type name
    parser_register_struct(p, name);
    
    parser_expect(p, TOK_LBRACE);
    
    ASTNode *struct_def = ast_new_struct_def(name);
    free(name);
    
    // Parse fields
    while (p->current->type != TOK_RBRACE && p->current->type != TOK_EOF) {
        ASTNode *field;
        
        // Check for function pointer: func(params) return_type name;
        if (p->current->type == TOK_FUNC) {
            advance(p);  // skip 'func'
            parser_expect(p, TOK_LPAREN);
            
            // Parse parameter types
            DataType param_types[32];
            int param_count = 0;
            
            while (p->current->type != TOK_RPAREN && p->current->type != TOK_EOF) {
                if (param_count > 0) {
                    parser_expect(p, TOK_COMMA);
                }
                param_types[param_count++] = parse_type(p);
            }
            parser_expect(p, TOK_RPAREN);
            
            // Parse return type
            DataType return_type = parse_type(p);
            
            // Parse field name
            if (p->current->type != TOK_IDENT) {
                parser_error(p, "Expected field name");
            }
            char *field_name = strdup(p->current->lexeme);
            advance(p);
            
            parser_expect(p, TOK_SEMI);
            
            field = ast_new_struct_field_func(field_name, return_type, param_types, param_count);
            free(field_name);
        } else {
            // Regular field: type name;
            DataType field_type = parse_type(p);
            
            if (p->current->type != TOK_IDENT) {
                parser_error(p, "Expected field name");
            }
            
            char *field_name = strdup(p->current->lexeme);
            advance(p);
            
            parser_expect(p, TOK_SEMI);
            
            field = ast_new_struct_field(field_name, field_type);
            free(field_name);
        }
        
        ast_add_struct_field(struct_def, field);
    }
    
    parser_expect(p, TOK_RBRACE);
    
    return struct_def;
}

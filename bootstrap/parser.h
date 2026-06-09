/* parser.h - Strada Bootstrap Parser */
#ifndef PARSER_H
#define PARSER_H

#include "lexer.h"
#include "ast.h"

typedef struct {
    Lexer *lexer;
    Token *current;
    Token *peek;
    Token *peek2;  // Two tokens ahead for lookahead
    
    // Struct registry for type checking during parsing
    struct {
        char **names;
        int count;
        int capacity;
    } structs;
} Parser;

/* Parser functions */
Parser* parser_new(Lexer *lexer);
void parser_free(Parser *parser);

/* Parse entry point */
ASTNode* parser_parse(Parser *parser);

/* Error handling */
void parser_error(Parser *parser, const char *message);
void parser_expect(Parser *parser, TokenType type);

#endif /* PARSER_H */

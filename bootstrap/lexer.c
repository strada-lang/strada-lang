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

/* lexer.c - Strada Bootstrap Compiler Lexer */
#define _POSIX_C_SOURCE 200809L
#include "lexer.h"
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

typedef struct {
    const char *keyword;
    TokenType type;
} Keyword;

static Keyword keywords[] = {
    {"func", TOK_FUNC},
    {"extern", TOK_EXTERN},
    {"my", TOK_MY},
    {"if", TOK_IF},
    {"elsif", TOK_ELSIF},
    {"else", TOK_ELSE},
    {"while", TOK_WHILE},
    {"for", TOK_FOR},
    {"foreach", TOK_FOREACH},
    {"return", TOK_RETURN},
    {"last", TOK_LAST},
    {"next", TOK_NEXT},
    {"try", TOK_TRY},
    {"catch", TOK_CATCH},
    {"throw", TOK_THROW},
    {"goto", TOK_GOTO},

    {"int", TOK_INT},
    {"num", TOK_NUM},
    {"str", TOK_STR},
    {"array", TOK_ARRAY},
    {"hash", TOK_HASH},
    {"scalar", TOK_SCALAR},
    {"void", TOK_VOID},
    {"struct", TOK_STRUCT},
    
    /* Package/Module support */
    {"package", TOK_PACKAGE},
    {"use", TOK_USE},
    {"lib", TOK_LIB},
    {"qw", TOK_QW},
    
    /* Extended C types */
    {"char", TOK_CHAR},
    {"short", TOK_SHORT},
    {"long", TOK_LONG},
    {"float", TOK_FLOAT},
    {"bool", TOK_BOOL},
    {"uchar", TOK_UCHAR},
    {"ushort", TOK_USHORT},
    {"uint", TOK_UINT},
    {"ulong", TOK_ULONG},
    {"i8", TOK_I8},
    {"i16", TOK_I16},
    {"i32", TOK_I32},
    {"i64", TOK_I64},
    {"u8", TOK_U8},
    {"u16", TOK_U16},
    {"u32", TOK_U32},
    {"u64", TOK_U64},
    {"sizet", TOK_SIZE},
    {"ptr", TOK_PTR},
    
    {"undef", TOK_UNDEF},
    {"defined", TOK_DEFINED},
    {"dump", TOK_DUMP},
    {"dumper", TOK_DUMPER},
    {"clone", TOK_CLONE},
    
    /* String comparison operators */
    {"eq", TOK_STR_EQ},
    {"ne", TOK_STR_NE},
    {"lt", TOK_STR_LT},
    {"gt", TOK_STR_GT},
    {"le", TOK_STR_LE},
    {"ge", TOK_STR_GE},
    
    {NULL, TOK_EOF}
};

Lexer* lexer_new(const char *source) {
    Lexer *lex = malloc(sizeof(Lexer));
    lex->source = source;
    lex->pos = 0;
    lex->length = strlen(source);
    lex->line = 1;
    lex->column = 1;
    return lex;
}

void lexer_free(Lexer *lex) {
    if (lex) {
        free(lex);
    }
}

static char lexer_current(Lexer *lex) {
    if (lex->pos >= lex->length) {
        return '\0';
    }
    return lex->source[lex->pos];
}

static char lexer_peek(Lexer *lex, int offset) {
    size_t pos = lex->pos + offset;
    if (pos >= lex->length) {
        return '\0';
    }
    return lex->source[pos];
}

static void lexer_advance(Lexer *lex) {
    if (lex->pos >= lex->length) return;
    
    if (lex->source[lex->pos] == '\n') {
        lex->line++;
        lex->column = 1;
    } else {
        lex->column++;
    }
    
    lex->pos++;
}

static void lexer_skip_whitespace(Lexer *lex) {
    while (isspace(lexer_current(lex))) {
        lexer_advance(lex);
    }
}

static void lexer_skip_comment(Lexer *lex) {
    if (lexer_current(lex) != '#') return;

    while (lexer_current(lex) && lexer_current(lex) != '\n') {
        lexer_advance(lex);
    }
}

static void lexer_skip_block_comment(Lexer *lex) {
    /* Skip opening /* */
    lexer_advance(lex);
    lexer_advance(lex);

    while (lexer_current(lex)) {
        if (lexer_current(lex) == '*' && lexer_peek(lex, 1) == '/') {
            lexer_advance(lex);
            lexer_advance(lex);
            return;
        }
        lexer_advance(lex);
    }
    fprintf(stderr, "Error: Unterminated block comment\n");
    exit(1);
}

static Token* make_token(TokenType type, const char *lexeme, int line, int column) {
    Token *tok = malloc(sizeof(Token));
    tok->type = type;
    tok->lexeme = lexeme ? strdup(lexeme) : NULL;
    tok->line = line;
    tok->column = column;
    return tok;
}

static TokenType lookup_keyword(const char *text) {
    for (int i = 0; keywords[i].keyword != NULL; i++) {
        if (strcmp(text, keywords[i].keyword) == 0) {
            return keywords[i].type;
        }
    }
    return TOK_IDENT;
}

static Token* read_identifier(Lexer *lex) {
    int start_line = lex->line;
    int start_col = lex->column;
    size_t start = lex->pos;
    
    while (isalnum(lexer_current(lex)) || lexer_current(lex) == '_') {
        lexer_advance(lex);
    }
    
    size_t length = lex->pos - start;
    char *text = malloc(length + 1);
    strncpy(text, lex->source + start, length);
    text[length] = '\0';
    
    TokenType type = lookup_keyword(text);
    Token *tok = make_token(type, text, start_line, start_col);
    
    free(text);
    return tok;
}

static Token* read_number(Lexer *lex) {
    int start_line = lex->line;
    int start_col = lex->column;
    size_t start = lex->pos;
    int has_dot = 0;
    
    while (isdigit(lexer_current(lex)) || 
           (lexer_current(lex) == '.' && !has_dot && isdigit(lexer_peek(lex, 1)))) {
        if (lexer_current(lex) == '.') {
            has_dot = 1;
        }
        lexer_advance(lex);
    }
    
    size_t length = lex->pos - start;
    char *text = malloc(length + 1);
    strncpy(text, lex->source + start, length);
    text[length] = '\0';
    
    Token *tok = make_token(has_dot ? TOK_NUM_LITERAL : TOK_INT_LITERAL, 
                           text, start_line, start_col);
    
    if (has_dot) {
        tok->value.num_val = atof(text);
    } else {
        tok->value.int_val = atoll(text);
    }
    
    free(text);
    return tok;
}

static Token* read_string(Lexer *lex) {
    int start_line = lex->line;
    int start_col = lex->column;
    char quote = lexer_current(lex);
    lexer_advance(lex); // Skip opening quote
    
    char buffer[4096];
    size_t buf_pos = 0;
    
    while (lexer_current(lex) && lexer_current(lex) != quote) {
        if (lexer_current(lex) == '\\') {
            lexer_advance(lex);
            char next = lexer_current(lex);
            
            switch (next) {
                case 'n': buffer[buf_pos++] = '\n'; break;
                case 't': buffer[buf_pos++] = '\t'; break;
                case 'r': buffer[buf_pos++] = '\r'; break;
                case '\\': buffer[buf_pos++] = '\\'; break;
                case '\"': buffer[buf_pos++] = '\"'; break;
                case '\'': buffer[buf_pos++] = '\''; break;
                default: buffer[buf_pos++] = next; break;
            }
            lexer_advance(lex);
        } else {
            buffer[buf_pos++] = lexer_current(lex);
            lexer_advance(lex);
        }
        
        if (buf_pos >= sizeof(buffer) - 1) {
            break; // Buffer full
        }
    }
    
    buffer[buf_pos] = '\0';
    
    if (lexer_current(lex) == quote) {
        lexer_advance(lex); // Skip closing quote
    }
    
    Token *tok = make_token(TOK_STR_LITERAL, buffer, start_line, start_col);
    tok->value.str_val = strdup(buffer);
    
    return tok;
}

Token* lexer_next(Lexer *lex) {
    while (1) {
        lexer_skip_whitespace(lex);

        if (lexer_current(lex) == '#') {
            lexer_skip_comment(lex);
            continue;
        }

        /* Check for block comments */
        if (lexer_current(lex) == '/' && lexer_peek(lex, 1) == '*') {
            lexer_skip_block_comment(lex);
            continue;
        }

        break;
    }
    
    int line = lex->line;
    int col = lex->column;
    char ch = lexer_current(lex);
    
    if (ch == '\0') {
        return make_token(TOK_EOF, "", line, col);
    }
    
    // Identifiers and keywords
    if (isalpha(ch) || ch == '_') {
        return read_identifier(lex);
    }
    
    // Numbers
    if (isdigit(ch)) {
        return read_number(lex);
    }
    
    // Strings
    if (ch == '"' || ch == '\'') {
        return read_string(lex);
    }
    
    // Two-character operators
    char ch2[3] = {ch, lexer_peek(lex, 1), '\0'};
    
    // Three-character operators
    char ch3[4] = {ch, lexer_peek(lex, 1), lexer_peek(lex, 2), '\0'};
    
    if (strcmp(ch3, "...") == 0) {
        lexer_advance(lex); lexer_advance(lex); lexer_advance(lex);
        return make_token(TOK_ELLIPSIS, "...", line, col);
    }
    
    if (strcmp(ch2, "==") == 0) {
        lexer_advance(lex); lexer_advance(lex);
        return make_token(TOK_EQ, "==", line, col);
    }
    if (strcmp(ch2, "!=") == 0) {
        lexer_advance(lex); lexer_advance(lex);
        return make_token(TOK_NE, "!=", line, col);
    }
    if (strcmp(ch2, "<=") == 0) {
        lexer_advance(lex); lexer_advance(lex);
        return make_token(TOK_LE, "<=", line, col);
    }
    if (strcmp(ch2, ">=") == 0) {
        lexer_advance(lex); lexer_advance(lex);
        return make_token(TOK_GE, ">=", line, col);
    }
    if (strcmp(ch2, "&&") == 0) {
        lexer_advance(lex); lexer_advance(lex);
        return make_token(TOK_AND, "&&", line, col);
    }
    if (strcmp(ch2, "||") == 0) {
        lexer_advance(lex); lexer_advance(lex);
        return make_token(TOK_OR, "||", line, col);
    }
    if (strcmp(ch2, "->") == 0) {
        lexer_advance(lex); lexer_advance(lex);
        return make_token(TOK_ARROW, "->", line, col);
    }
    if (strcmp(ch2, "=>") == 0) {
        lexer_advance(lex); lexer_advance(lex);
        return make_token(TOK_FAT_ARROW, "=>", line, col);
    }
    if (strcmp(ch2, "::") == 0) {
        lexer_advance(lex); lexer_advance(lex);
        return make_token(TOK_COLONCOLON, "::", line, col);
    }
    if (strcmp(ch2, "+=") == 0) {
        lexer_advance(lex); lexer_advance(lex);
        return make_token(TOK_PLUS_ASSIGN, "+=", line, col);
    }
    if (strcmp(ch2, "-=") == 0) {
        lexer_advance(lex); lexer_advance(lex);
        return make_token(TOK_MINUS_ASSIGN, "-=", line, col);
    }
    if (strcmp(ch2, ".=") == 0) {
        lexer_advance(lex); lexer_advance(lex);
        return make_token(TOK_DOT_ASSIGN, ".=", line, col);
    }
    
    // Single-character tokens
    lexer_advance(lex);
    
    char single[2] = {ch, '\0'};
    
    switch (ch) {
        case '(': return make_token(TOK_LPAREN, single, line, col);
        case ')': return make_token(TOK_RPAREN, single, line, col);
        case '{': return make_token(TOK_LBRACE, single, line, col);
        case '}': return make_token(TOK_RBRACE, single, line, col);
        case '[': return make_token(TOK_LBRACKET, single, line, col);
        case ']': return make_token(TOK_RBRACKET, single, line, col);
        case ';': return make_token(TOK_SEMI, single, line, col);
        case ',': return make_token(TOK_COMMA, single, line, col);
        case '$': return make_token(TOK_DOLLAR, single, line, col);
        case '@': return make_token(TOK_AT, single, line, col);
        case '%': return make_token(TOK_PERCENT, single, line, col);
        case '+': return make_token(TOK_PLUS, single, line, col);
        case '-': return make_token(TOK_MINUS, single, line, col);
        case '*': return make_token(TOK_MULT, single, line, col);
        case '/': return make_token(TOK_DIV, single, line, col);
        case '.': return make_token(TOK_DOT, single, line, col);
        case '=': return make_token(TOK_ASSIGN, single, line, col);
        case '<': return make_token(TOK_LT, single, line, col);
        case '>': return make_token(TOK_GT, single, line, col);
        case '!': return make_token(TOK_NOT, single, line, col);
        case '&': return make_token(TOK_AMPERSAND, single, line, col);
        case '\\': return make_token(TOK_BACKSLASH, single, line, col);
        case ':': return make_token(TOK_COLON, single, line, col);
    }
    
    fprintf(stderr, "Unexpected character '%c' at line %d, column %d\n", ch, line, col);
    return make_token(TOK_EOF, "", line, col);
}

void lexer_error(Lexer *lex, const char *message) {
    fprintf(stderr, "Lexer error at line %d, column %d: %s\n", 
            lex->line, lex->column, message);
    exit(1);
}

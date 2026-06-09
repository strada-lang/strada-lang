/* lexer.h - Strada Bootstrap Compiler Lexer */
#ifndef LEXER_H
#define LEXER_H

#include <stdio.h>
#include <stdint.h>

typedef enum {
    TOK_EOF,
    TOK_FUNC,
    TOK_EXTERN,
    TOK_MY,
    TOK_IF,
    TOK_ELSIF,
    TOK_ELSE,
    TOK_WHILE,
    TOK_FOR,
    TOK_FOREACH,
    TOK_RETURN,
    TOK_LAST,
    TOK_NEXT,
    TOK_TRY,
    TOK_CATCH,
    TOK_THROW,
    TOK_GOTO,
    TOK_COLON,

    /* Types */
    TOK_INT,
    TOK_NUM,
    TOK_STR,
    TOK_ARRAY,
    TOK_HASH,
    TOK_SCALAR,
    TOK_VOID,
    TOK_STRUCT,
    
    /* Package/Module support */
    TOK_PACKAGE,
    TOK_USE,
    TOK_LIB,
    TOK_QW,           /* qw() for import lists */
    TOK_COLONCOLON,   /* :: for package separator */
    
    /* Extended C types */
    TOK_CHAR,
    TOK_SHORT,
    TOK_LONG,
    TOK_FLOAT,
    TOK_BOOL,
    TOK_UCHAR,
    TOK_USHORT,
    TOK_UINT,
    TOK_ULONG,
    TOK_I8,
    TOK_I16,
    TOK_I32,
    TOK_I64,
    TOK_U8,
    TOK_U16,
    TOK_U32,
    TOK_U64,
    TOK_SIZE,
    TOK_PTR,
    
    /* Built-ins */
    TOK_UNDEF,
    TOK_DEFINED,
    TOK_DUMP,
    TOK_DUMPER,
    TOK_CLONE,
    
    /* Literals */
    TOK_INT_LITERAL,
    TOK_NUM_LITERAL,
    TOK_STR_LITERAL,
    TOK_IDENT,
    
    /* Operators */
    TOK_PLUS,
    TOK_MINUS,
    TOK_MULT,
    TOK_DIV,
    TOK_MOD,
    TOK_DOT,          /* String concat */
    
    TOK_EQ,           /* == */
    TOK_NE,           /* != */
    TOK_LT,           /* < */
    TOK_GT,           /* > */
    TOK_LE,           /* <= */
    TOK_GE,           /* >= */
    TOK_STR_EQ,       /* eq (string equality) */
    TOK_STR_NE,       /* ne (string inequality) */
    TOK_STR_LT,       /* lt (string less than) */
    TOK_STR_GT,       /* gt (string greater than) */
    TOK_STR_LE,       /* le (string less or equal) */
    TOK_STR_GE,       /* ge (string greater or equal) */
    
    TOK_AND,          /* && */
    TOK_OR,           /* || */
    TOK_NOT,          /* ! */
    TOK_AMPERSAND,    /* & (address-of / function ref) */
    TOK_BACKSLASH,    /* \ (reference operator) */
    
    TOK_ASSIGN,       /* = */
    TOK_PLUS_ASSIGN,  /* += */
    TOK_MINUS_ASSIGN, /* -= */
    TOK_DOT_ASSIGN,   /* .= */
    
    TOK_ELLIPSIS,     /* ... */
    TOK_ARROW,        /* -> */
    TOK_FAT_ARROW,    /* => */
    
    /* Punctuation */
    TOK_LPAREN,
    TOK_RPAREN,
    TOK_LBRACE,
    TOK_RBRACE,
    TOK_LBRACKET,
    TOK_RBRACKET,
    TOK_SEMI,
    TOK_COMMA,
    TOK_DOLLAR,
    TOK_AT,
    TOK_PERCENT,
} TokenType;

typedef struct {
    TokenType type;
    char *lexeme;
    int line;
    int column;
    union {
        int64_t int_val;
        double num_val;
        char *str_val;
    } value;
} Token;

typedef struct {
    const char *source;
    size_t pos;
    size_t length;
    int line;
    int column;
    Token current;
} Lexer;

/* Lexer functions */
Lexer* lexer_new(const char *source);
void lexer_free(Lexer *lex);
Token* lexer_next(Lexer *lex);
void lexer_error(Lexer *lex, const char *message);

#endif /* LEXER_H */

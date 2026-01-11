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

/* main.c - Strada Bootstrap Compiler Main */
#define _POSIX_C_SOURCE 200809L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "lexer.h"
#include "parser.h"
#include "codegen.h"

static char* read_file(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        fprintf(stderr, "Error: Cannot open file '%s'\n", filename);
        return NULL;
    }
    
    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);
    
    char *content = malloc(size + 1);
    fread(content, 1, size, f);
    content[size] = '\0';
    
    fclose(f);
    return content;
}

int main(int argc, char **argv) {
    if (argc != 3) {
        fprintf(stderr, "Usage: %s <input.strada> <output.c>\n", argv[0]);
        return 1;
    }
    
    const char *input_file = argv[1];
    const char *output_file = argv[2];
    
    printf("Strada Bootstrap Compiler\n");
    printf("Compiling: %s -> %s\n", input_file, output_file);
    
    // Read source file
    printf("Reading source file...\n");
    char *source = read_file(input_file);
    if (!source) {
        return 1;
    }
    printf("Read %zu bytes\n", strlen(source));
    
    // Lex
    printf("Lexing...\n");
    fflush(stdout);
    Lexer *lexer = lexer_new(source);
    printf("Lexer created\n");
    fflush(stdout);
    
    // Parse
    printf("Parsing...\n");
    fflush(stdout);
    Parser *parser = parser_new(lexer);
    printf("Parser created\n");
    fflush(stdout);
    ASTNode *program = parser_parse(parser);
    printf("Parsing complete\n");
    fflush(stdout);
    
    // Generate code
    printf("Generating C code...\n");
    FILE *output = fopen(output_file, "w");
    if (!output) {
        fprintf(stderr, "Error: Cannot write to '%s'\n", output_file);
        return 1;
    }
    
    CodeGen *codegen = codegen_new(output);
    codegen_generate(codegen, program);
    
    fclose(output);
    
    // Cleanup
    codegen_free(codegen);
    ast_free(program);
    parser_free(parser);
    lexer_free(lexer);
    free(source);
    
    printf("Success! Generated %s\n", output_file);
    printf("\nTo compile and run:\n");
    printf("  gcc -o program %s ../runtime/strada_runtime.c -I../runtime\n", output_file);
    printf("  ./program\n");
    
    return 0;
}

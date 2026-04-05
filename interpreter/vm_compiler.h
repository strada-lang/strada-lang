/* vm_compiler.h — AST to bytecode compiler */
#ifndef VM_COMPILER_H
#define VM_COMPILER_H

#include "vm.h"
#include "../runtime/strada_runtime.h"

/* Compile a parsed AST program into a VMProgram.
 * The AST is a StradaValue* hash with "functions" array etc. */
VMProgram *vm_compile_program(StradaValue *ast);

#endif

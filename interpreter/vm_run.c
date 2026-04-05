/* vm_run.c — Parse a .strada file, compile to bytecode, execute on VM.
 * Usage: vm_run input.strada [entry_func]
 * Default entry function is "main" (or "run" if no main). */

#include "vm.h"
#include "vm_compiler.h"
#include "../runtime/strada_runtime.h"
#include <stdio.h>
#include <string.h>
#include <sys/resource.h>

/* These are compiled from the Strada parser (AST.strada + Lexer.strada + Parser.strada) */
extern StradaValue* lex_tokenize(StradaValue *source);
extern StradaValue* parse(StradaValue *tokens, StradaValue *filename);

/* Globals required by Combined.c functions */
extern StradaValue *ARGV, *ARGC;
extern StradaValue *v_STDIN, *v_STDOUT, *v_STDERR;
extern StradaValue *__child_status, *__program_name;
extern StradaValue *g_eval_interp, *g_lib_paths;
extern void __Strada_Interpreter_oop_init(void);

/* Initialize the minimum Strada runtime state needed for lex/parse */
static void init_strada_runtime(const char *prog_name) {
    /* Standard I/O filehandles (immortal) */
    v_STDIN = strada_new_undef(); v_STDIN->type = STRADA_FILEHANDLE;
    v_STDIN->refcount = 1000000001; v_STDIN->value.fh = stdin;
    v_STDOUT = strada_new_undef(); v_STDOUT->type = STRADA_FILEHANDLE;
    v_STDOUT->refcount = 1000000001; v_STDOUT->value.fh = stdout;
    v_STDERR = strada_new_undef(); v_STDERR->type = STRADA_FILEHANDLE;
    v_STDERR->refcount = 1000000001; v_STDERR->value.fh = stderr;

    /* Global variables */
    ARGV = strada_new_array();
    ARGC = strada_new_int(0);
    __child_status = STRADA_MAKE_TAGGED_INT(0);
    __program_name = strada_new_str(prog_name);
    g_eval_interp = strada_new_undef();
    g_lib_paths = strada_new_array();

    /* Stack trace support */
    strada_stack_push("main", "vm_run");
}

int main(int argc, char **argv) {
    if (argc < 2) {
        fprintf(stderr, "Usage: %s input.strada [entry_func]\n", argv[0]);
        return 1;
    }

    /* Limit virtual memory to 4GB to prevent OOM kills */
    struct rlimit rl;
    rl.rlim_cur = (rlim_t)4ULL * 1024 * 1024 * 1024;
    rl.rlim_max = (rlim_t)4ULL * 1024 * 1024 * 1024;
    setrlimit(RLIMIT_AS, &rl);

    const char *input_file = argv[1];
    const char *entry_func = argc > 2 ? argv[2] : NULL;

    /* Initialize Strada runtime */
    init_strada_runtime(argv[0]);

    /* Read source file */
    StradaValue *source_sv = strada_slurp(input_file);
    if (!source_sv || STRADA_IS_TAGGED_INT(source_sv) || source_sv->type != STRADA_STR) {
        fprintf(stderr, "Error: cannot read '%s'\n", input_file);
        return 1;
    }

    /* Lex */
    StradaValue *tokens = lex_tokenize(source_sv);
    if (!tokens) {
        fprintf(stderr, "Error: lexer failed\n");
        strada_decref(source_sv);
        return 1;
    }

    /* Parse */
    StradaValue *filename_sv = strada_new_str(input_file);
    StradaValue *ast = parse(tokens, filename_sv);
    if (!ast) {
        fprintf(stderr, "Error: parser failed\n");
        strada_decref(filename_sv);
        strada_decref(tokens);
        strada_decref(source_sv);
        return 1;
    }

    /* Compile to bytecode */
    VMProgram *prog = vm_compile_program(ast);
    if (!prog) {
        strada_decref(ast);
        strada_decref(tokens);
        strada_decref(source_sv);
        strada_decref(filename_sv);
        return 1;
    }

    /* Set runtime include path for __C__ block JIT compilation */
    prog->runtime_include_path = strdup(RUNTIME_DIR);

    /* Find entry function */
    const char *entry = entry_func;
    if (!entry) {
        if (vm_program_find_func(prog, "main") >= 0) entry = "main";
        else if (vm_program_find_func(prog, "run") >= 0) entry = "run";
        else {
            fprintf(stderr, "Error: no 'main' or 'run' function found\n");
            vm_program_free(prog);
            return 1;
        }
    }

    /* Execute */
    VM *vm = vm_new(prog);
    VMValue result = vm_execute(vm, entry);

    /* Run END blocks (LIFO order) */
    if (prog->end_count > 0) {
        for (int i = prog->end_count - 1; i >= 0; i--) {
            vm_execute(vm, prog->func_names[prog->end_blocks[i]]);
        }
    }

    int exit_code = VM_IS_INT(result) ? (int)VM_INT_VAL(result) : 0;

    vm_free(vm);
    vm_program_free(prog);
    strada_decref(ast);
    strada_decref(tokens);
    strada_decref(source_sv);
    strada_decref(filename_sv);

    return exit_code;
}

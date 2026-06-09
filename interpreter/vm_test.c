/* vm_test.c — Test the bytecode VM with hand-compiled fib(25) + sum loop
 * Equivalent to:
 *   func fib(int $n) int {
 *       if ($n < 2) { return $n; }
 *       return fib($n - 1) + fib($n - 2);
 *   }
 *   func run() int {
 *       my int $sum = 0;
 *       my int $i = 1;
 *       while ($i <= 100000) { $sum += $i; $i++; }
 *       say("sum: " . $sum);
 *       say("fib(25): " . fib(25));
 *       return 0;
 *   }
 */

#include "vm.h"
#include <stdio.h>
#include <time.h>

int main(void) {
    VMProgram *prog = vm_program_new();

    /* ===== Compile fib(n) ===== */
    /* locals: 0 = n */
    VMChunk *fib = vm_program_add_func(prog, "fib");
    fib->local_count = 1;
    int fib_idx = 0;  /* will be func index 0 */

    /* if (n < 2) return n */
    vm_chunk_emit(fib, OP_LOAD_LOCAL);  /* push n */
    vm_chunk_emit_u16(fib, 0);
    vm_chunk_emit(fib, OP_PUSH_INT);    /* push 2 */
    vm_chunk_emit_i64(fib, 2);
    vm_chunk_emit(fib, OP_LT);          /* n < 2 */
    vm_chunk_emit(fib, OP_JMP_IF_FALSE);
    size_t skip_return = fib->code_len;
    vm_chunk_emit_u16(fib, 0);          /* placeholder */

    /* return n */
    vm_chunk_emit(fib, OP_LOAD_LOCAL);
    vm_chunk_emit_u16(fib, 0);
    vm_chunk_emit(fib, OP_RETURN);

    /* patch jump target */
    uint16_t after_return = (uint16_t)fib->code_len;
    fib->code[skip_return] = after_return & 0xFF;
    fib->code[skip_return + 1] = (after_return >> 8) & 0xFF;

    /* return fib(n-1) + fib(n-2) */
    /* fib(n-1) */
    vm_chunk_emit(fib, OP_LOAD_LOCAL);
    vm_chunk_emit_u16(fib, 0);
    vm_chunk_emit(fib, OP_PUSH_INT);
    vm_chunk_emit_i64(fib, 1);
    vm_chunk_emit(fib, OP_SUB);
    vm_chunk_emit(fib, OP_CALL);
    vm_chunk_emit_u16(fib, (uint16_t)fib_idx);
    vm_chunk_emit(fib, 1);  /* 1 arg */

    /* fib(n-2) */
    vm_chunk_emit(fib, OP_LOAD_LOCAL);
    vm_chunk_emit_u16(fib, 0);
    vm_chunk_emit(fib, OP_PUSH_INT);
    vm_chunk_emit_i64(fib, 2);
    vm_chunk_emit(fib, OP_SUB);
    vm_chunk_emit(fib, OP_CALL);
    vm_chunk_emit_u16(fib, (uint16_t)fib_idx);
    vm_chunk_emit(fib, 1);  /* 1 arg */

    /* add results */
    vm_chunk_emit(fib, OP_ADD);
    vm_chunk_emit(fib, OP_RETURN);

    /* ===== Compile run() ===== */
    /* locals: 0 = sum, 1 = i */
    VMChunk *run = vm_program_add_func(prog, "run");
    run->local_count = 2;
    vm_chunk_add_str_const(run, "sum: ");
    vm_chunk_add_str_const(run, "fib(25): ");

    /* sum = 0 */
    vm_chunk_emit(run, OP_PUSH_INT);
    vm_chunk_emit_i64(run, 0);
    vm_chunk_emit(run, OP_STORE_LOCAL);
    vm_chunk_emit_u16(run, 0);

    /* i = 1 */
    vm_chunk_emit(run, OP_PUSH_INT);
    vm_chunk_emit_i64(run, 1);
    vm_chunk_emit(run, OP_STORE_LOCAL);
    vm_chunk_emit_u16(run, 1);

    /* while (i <= 100000) */
    size_t loop_top = run->code_len;
    vm_chunk_emit(run, OP_LOAD_LOCAL);
    vm_chunk_emit_u16(run, 1);
    vm_chunk_emit(run, OP_PUSH_INT);
    vm_chunk_emit_i64(run, 100000);
    vm_chunk_emit(run, OP_LE);
    vm_chunk_emit(run, OP_JMP_IF_FALSE);
    size_t loop_exit = run->code_len;
    vm_chunk_emit_u16(run, 0);  /* placeholder */

    /* sum += i */
    vm_chunk_emit(run, OP_LOAD_LOCAL);
    vm_chunk_emit_u16(run, 0);
    vm_chunk_emit(run, OP_LOAD_LOCAL);
    vm_chunk_emit_u16(run, 1);
    vm_chunk_emit(run, OP_ADD);
    vm_chunk_emit(run, OP_STORE_LOCAL);
    vm_chunk_emit_u16(run, 0);

    /* i++ */
    vm_chunk_emit(run, OP_INCR);
    vm_chunk_emit_u16(run, 1);

    /* jump back to loop top */
    vm_chunk_emit(run, OP_JMP);
    vm_chunk_emit_u16(run, (uint16_t)loop_top);

    /* loop exit — patch jump */
    uint16_t exit_pos = (uint16_t)run->code_len;
    run->code[loop_exit] = exit_pos & 0xFF;
    run->code[loop_exit + 1] = (exit_pos >> 8) & 0xFF;

    /* say("sum: " . sum) */
    vm_chunk_emit(run, OP_PUSH_STR);
    vm_chunk_emit_u16(run, 0);  /* "sum: " */
    vm_chunk_emit(run, OP_LOAD_LOCAL);
    vm_chunk_emit_u16(run, 0);
    vm_chunk_emit(run, OP_CONCAT);
    vm_chunk_emit(run, OP_SAY);

    /* say("fib(25): " . fib(25)) */
    vm_chunk_emit(run, OP_PUSH_STR);
    vm_chunk_emit_u16(run, 1);  /* "fib(25): " */
    vm_chunk_emit(run, OP_PUSH_INT);
    vm_chunk_emit_i64(run, 25);
    vm_chunk_emit(run, OP_CALL);
    vm_chunk_emit_u16(run, (uint16_t)fib_idx);
    vm_chunk_emit(run, 1);
    vm_chunk_emit(run, OP_CONCAT);
    vm_chunk_emit(run, OP_SAY);

    /* return 0 */
    vm_chunk_emit(run, OP_PUSH_INT);
    vm_chunk_emit_i64(run, 0);
    vm_chunk_emit(run, OP_RETURN);

    /* ===== Execute ===== */
    VM *vm = vm_new(prog);
    VMValue result = vm_execute(vm, "run");

    vm_free(vm);
    vm_program_free(prog);
    return 0;
}

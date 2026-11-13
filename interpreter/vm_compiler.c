/* vm_compiler.c — Compile Strada AST to bytecode for the VM.
 * Full language support: closures, try/catch, OOP, regex, I/O, etc. */

#include "vm_compiler.h"
#include <stdio.h>
#include <string.h>
#include <dlfcn.h>

/* AST node type constants */
#define NI_PROGRAM       1
#define NI_FUNC          2
#define NI_PARAM         3
#define NI_BLOCK         4
#define NI_VAR_DECL      5
#define NI_IF_STMT       6
#define NI_WHILE_STMT    7
#define NI_FOR_STMT      8
#define NI_RETURN_STMT   9
#define NI_EXPR_STMT     10
#define NI_BINARY_OP     11
#define NI_UNARY_OP      12
#define NI_CALL          13
#define NI_VARIABLE      14
#define NI_INT_LITERAL   15
#define NI_NUM_LITERAL   16
#define NI_STR_LITERAL   17
#define NI_ASSIGN        18
#define NI_SUBSCRIPT     19
#define NI_HASH_ACCESS   20
#define NI_REF           21
#define NI_DEREF_HASH    22
#define NI_DEREF_ARRAY   23
#define NI_DEREF_SCALAR  24
#define NI_ANON_HASH     25
#define NI_ANON_ARRAY    26
#define NI_FIELD_ACCESS  30
#define NI_FUNC_REF      31
#define NI_METHOD_CALL   32
#define NI_REGEX_MATCH   34
#define NI_REGEX_SUBST   35
#define NI_LAST          100
#define NI_NEXT          101
#define NI_UNDEF         102
#define NI_MAP           103
#define NI_SORT          104
#define NI_GREP          105
#define NI_TRY_CATCH     106
#define NI_THROW         107
#define NI_LABEL         108
#define NI_FOREACH_STMT  110
#define NI_ANON_FUNC     111
#define NI_CLOSURE_CALL  112
#define NI_TERNARY       113
#define NI_SWITCH        114
#define NI_SUPER_CALL    116
#define NI_INCREMENT     117
#define NI_C_BLOCK       119
#define NI_SPREAD        120
#define NI_CATCH_CLAUSE  121
#define NI_READLINE      124
#define NI_DESTRUCTURE   125
#define NI_DO_WHILE_STMT 126
#define NI_CONST_DECL    127
#define NI_BEGIN_BLOCK   128
#define NI_END_BLOCK     129
#define NI_ARRAY_SLICE   130
#define NI_HASH_SLICE    131
#define NI_OUR_DECL      132
#define NI_RANGE         115
#define NI_REDO          133
#define NI_TR            134
#define NI_LOCAL_DECL    135
#define NI_CAPTURE_VAR   136
#define NI_DYN_METHOD_CALL 137

/* Internal op codes for the VM compiler */
enum {
    OP_AST_ADD = 1, OP_AST_SUB, OP_AST_MUL, OP_AST_DIV, OP_AST_MOD,
    OP_AST_POW = 6,
    OP_AST_CONCAT = 7,
    OP_AST_REPEAT = 8,
    OP_AST_NUM_EQ = 10, OP_AST_NUM_NE, OP_AST_NUM_LT, OP_AST_NUM_GT,
    OP_AST_NUM_LE, OP_AST_NUM_GE,
    OP_AST_STR_EQ = 20, OP_AST_STR_NE, OP_AST_STR_LT, OP_AST_STR_GT, OP_AST_STR_LE, OP_AST_STR_GE,
    OP_AST_LOG_AND = 30, OP_AST_LOG_OR, OP_AST_DEFOR,
    OP_AST_SPACESHIP = 40, OP_AST_STR_CMP = 41,
    OP_AST_BIT_AND = 50, OP_AST_BIT_OR, OP_AST_BIT_XOR, OP_AST_BIT_SHL, OP_AST_BIT_SHR,
};

/* ===== AST helpers ===== */

static StradaValue *ast_deref(StradaValue *v) {
    if (!v || STRADA_IS_TAGGED_INT(v)) return v;
    while (v->type == STRADA_REF && v->value.rv) v = v->value.rv;
    return v;
}

static StradaValue *ast_get(StradaValue *node, const char *key) {
    StradaValue *n = ast_deref(node);
    if (!n || STRADA_IS_TAGGED_INT(n) || n->type != STRADA_HASH) return NULL;
    return strada_hash_get(n->value.hv, key);
}

static int64_t ast_int(StradaValue *node, const char *key) {
    StradaValue *v = ast_get(node, key);
    return strada_to_int(v);
}

static const char *ast_str(StradaValue *node, const char *key) {
    StradaValue *v = ast_get(node, key);
    if (!v || STRADA_IS_TAGGED_INT(v) || v->type != STRADA_STR) return "";
    return v->value.pv ? v->value.pv : "";
}

static StradaValue *ast_arr_get(StradaValue *arr, int idx) {
    StradaValue *a = ast_deref(arr);
    if (!a || STRADA_IS_TAGGED_INT(a) || a->type != STRADA_ARRAY) return NULL;
    return strada_array_get(a->value.av, idx);
}

static int ast_op_code(StradaValue *node) {
    StradaValue *v = ast_get(node, "op");
    if (!v) return -1;
    if (!STRADA_IS_TAGGED_INT(v) && v->type == STRADA_STR && v->value.pv) {
        const char *s = v->value.pv;
        if (s[0] == '+' && s[1] == '\0') return OP_AST_ADD;
        if (s[0] == '-' && s[1] == '\0') return OP_AST_SUB;
        if (s[0] == '*' && s[1] == '\0') return OP_AST_MUL;
        if (s[0] == '/' && s[1] == '\0') return OP_AST_DIV;
        if (s[0] == '%' && s[1] == '\0') return OP_AST_MOD;
        if (s[0] == '.' && s[1] == '\0') return OP_AST_CONCAT;
        if (s[0] == 'x' && s[1] == '\0') return OP_AST_REPEAT;
        if (strcmp(s, "**") == 0) return OP_AST_POW;
        if (strcmp(s, "==") == 0) return OP_AST_NUM_EQ;
        if (strcmp(s, "!=") == 0) return OP_AST_NUM_NE;
        if (s[0] == '<' && s[1] == '\0') return OP_AST_NUM_LT;
        if (s[0] == '>' && s[1] == '\0') return OP_AST_NUM_GT;
        if (strcmp(s, "<=") == 0) return OP_AST_NUM_LE;
        if (strcmp(s, ">=") == 0) return OP_AST_NUM_GE;
        if (strcmp(s, "&&") == 0) return OP_AST_LOG_AND;
        if (strcmp(s, "||") == 0) return OP_AST_LOG_OR;
        if (strcmp(s, "//") == 0) return OP_AST_DEFOR;
        if (strcmp(s, "eq") == 0) return OP_AST_STR_EQ;
        if (strcmp(s, "ne") == 0) return OP_AST_STR_NE;
        if (strcmp(s, "lt") == 0) return OP_AST_STR_LT;
        if (strcmp(s, "gt") == 0) return OP_AST_STR_GT;
        if (strcmp(s, "le") == 0) return OP_AST_STR_LE;
        if (strcmp(s, "ge") == 0) return OP_AST_STR_GE;
        if (strcmp(s, "<=>") == 0) return OP_AST_SPACESHIP;
        if (strcmp(s, "cmp") == 0) return OP_AST_STR_CMP;
        if (s[0] == '&' && s[1] == '\0') return OP_AST_BIT_AND;
        if (s[0] == '|' && s[1] == '\0') return OP_AST_BIT_OR;
        if (s[0] == '^' && s[1] == '\0') return OP_AST_BIT_XOR;
        if (strcmp(s, "<<") == 0) return OP_AST_BIT_SHL;
        if (strcmp(s, ">>") == 0) return OP_AST_BIT_SHR;
        /* Don't print error for unsupported ops — just return -1 */
        return -1;
    }
    return (int)strada_to_int(v);
}

/* ===== Compiler context ===== */

/* Loop context for last/next/redo */
typedef struct LoopCtx {
    size_t break_patches[64];
    int break_count;
    size_t next_patches[64];
    int next_count;
    size_t continue_target;     /* address to jump to for 'next' (set after body) */
    size_t redo_target;         /* address to jump to for 'redo' (body start, after condition) */
    const char *label;          /* label name or NULL */
    int local_restore_depth;
    struct LoopCtx *parent;
} LoopCtx;

/* Capture tracking for closures */
typedef struct {
    char *name;
    int outer_slot;   /* slot in enclosing function */
} CaptureInfo;

/* Global name tracking for our variables */
static char *g_our_vars[256];
static int g_our_count = 0;

static void register_our_var(const char *name) {
    for (int i = 0; i < g_our_count; i++)
        if (strcmp(g_our_vars[i], name) == 0) return;
    if (g_our_count < 256)
        g_our_vars[g_our_count++] = strdup(name);
}

static int is_our_var(const char *name) {
    for (int i = 0; i < g_our_count; i++)
        if (strcmp(g_our_vars[i], name) == 0) return 1;
    return 0;
}

/* const declarations: parsed references are plain $NAME variables, and the
 * decls land in the program's globals list (compiled in __globals_init), so
 * they behave exactly like our-globals — registered here, stored/loaded via
 * OP_STORE_GLOBAL/OP_LOAD_GLOBAL. */
static char *g_const_vars[256];
static int g_const_count = 0;

static void register_const_var(const char *name) {
    for (int i = 0; i < g_const_count; i++)
        if (strcmp(g_const_vars[i], name) == 0) return;
    if (g_const_count < 256)
        g_const_vars[g_const_count++] = strdup(name);
}

static int is_const_var(const char *name) {
    for (int i = 0; i < g_const_count; i++)
        if (strcmp(g_const_vars[i], name) == 0) return 1;
    return 0;
}

static void reset_compiler_globals(void) {
    for (int i = 0; i < g_our_count; i++) {
        free(g_our_vars[i]);
        g_our_vars[i] = NULL;
    }
    g_our_count = 0;
    for (int i = 0; i < g_const_count; i++) {
        free(g_const_vars[i]);
        g_const_vars[i] = NULL;
    }
    g_const_count = 0;
}

typedef struct TryCtx {
    int handlers_active;        /* number of active OP_TRY_BEGIN frames on exc_stack for this try */
    StradaValue *finally_block; /* non-NULL if this try statement has a finally block */
    LoopCtx *enclosing_loop;    /* ctx->loop_ctx when this try was entered */
    struct TryCtx *parent;
} TryCtx;

typedef struct {
    VMChunk *chunk;
    VMProgram *prog;
    char *local_names[256];
    char local_sigils[256];
    int local_count;
    const char *func_name;
    const char *cur_package;  /* enclosing package for unqualified-call fallback */
    int errors;
    LoopCtx *loop_ctx;
    TryCtx *try_ctx;
    int assign_depth;
    /* Closure support */
    CaptureInfo captures[64];
    int capture_count;
    int is_closure;
    /* Parent context for nested functions */
    void *parent_ctx;
    /* Local restore tracking */
    char *local_restores[32];
    int local_restore_count;
} CompCtx;

static int ctx_find_local_sigil(CompCtx *ctx, const char *name, char sigil) {
    /* Pass 1: exact sigil match (allows $x, @x, %x to coexist in one scope) */
    if (sigil != 0) {
        for (int i = ctx->local_count - 1; i >= 0; i--) {
            if (ctx->local_sigils[i] == sigil && strcmp(ctx->local_names[i], name) == 0)
                return i;
        }
    }
    /* Pass 2: fallback without sigil (e.g. my array $arr accessed as $arr[0]) */
    for (int i = ctx->local_count - 1; i >= 0; i--) {
        if (strcmp(ctx->local_names[i], name) == 0) return i;
    }
    return -1;
}

static int ctx_find_local(CompCtx *ctx, const char *name) {
    return ctx_find_local_sigil(ctx, name, 0);
}

static int ctx_add_local(CompCtx *ctx, const char *name, char sigil) {
    /* local_names[]/local_sigils[] are fixed 256-entry arrays. Without this
     * bound check, a function/scope with >=256 accumulated locals overflowed
     * them (stack buffer overflow when compiling untrusted source). Fail with
     * a clear diagnostic rather than corrupt the compiler's stack. */
    if (ctx->local_count >= (int)(sizeof(ctx->local_names) / sizeof(ctx->local_names[0]))) {
        fprintf(stderr, "VM compile error: too many local variables in one scope (max %d); "
                        "this function is too large for the bytecode VM backend\n",
                (int)(sizeof(ctx->local_names) / sizeof(ctx->local_names[0])));
        exit(1);
    }
    int slot = ctx->local_count;
    ctx->local_names[slot] = strdup(name);
    ctx->local_sigils[slot] = sigil;
    ctx->local_count++;
    return slot;
}

static int ctx_get_or_add_local(CompCtx *ctx, const char *name, char sigil) {
    for (int i = ctx->local_count - 1; i >= 0; i--) {
        if (ctx->local_sigils[i] == sigil && strcmp(ctx->local_names[i], name) == 0)
            return i;
    }
    return ctx_add_local(ctx, name, sigil);
}

static int compound_op_to_opcode(const char *op) {
    if (strcmp(op, "+=") == 0)  return OP_ADD;
    if (strcmp(op, "-=") == 0)  return OP_SUB;
    if (strcmp(op, "*=") == 0)  return OP_MUL;
    if (strcmp(op, "/=") == 0)  return OP_DIV;
    if (strcmp(op, "%=") == 0)  return OP_MOD;
    if (strcmp(op, ".=") == 0)  return OP_CONCAT;
    if (strcmp(op, "**=") == 0) return OP_POWER;
    if (strcmp(op, "&=") == 0)  return OP_BIT_AND;
    if (strcmp(op, "|=") == 0)  return OP_BIT_OR;
    if (strcmp(op, "^=") == 0)  return OP_BIT_XOR;
    if (strcmp(op, "<<=") == 0) return OP_BIT_SHL;
    if (strcmp(op, ">>=") == 0) return OP_BIT_SHR;
    return -1;
}

static int is_shortcircuit_assign(const char *op) {
    return strcmp(op, "&&=") == 0 || strcmp(op, "||=") == 0 || strcmp(op, "//=") == 0;
}

static int ctx_find_capture(CompCtx *ctx, const char *name) {
    for (int i = 0; i < ctx->capture_count; i++) {
        if (strcmp(ctx->captures[i].name, name) == 0) return i;
    }
    return -1;
}

static int ctx_add_capture(CompCtx *ctx, const char *name, int outer_slot) {
    /* captures[] is a fixed 64-entry array (and the parallel outer_slots[64]
     * at the call site is indexed by the same count). Without this bound, a
     * closure capturing >=64 distinct enclosing variables overflowed them. */
    if (ctx->capture_count >= (int)(sizeof(ctx->captures) / sizeof(ctx->captures[0]))) {
        fprintf(stderr, "VM compile error: closure captures too many variables (max %d)\n",
                (int)(sizeof(ctx->captures) / sizeof(ctx->captures[0])));
        exit(1);
    }
    int idx = ctx->capture_count;
    ctx->captures[idx].name = strdup(name);
    ctx->captures[idx].outer_slot = outer_slot;
    ctx->capture_count++;
    return idx;
}

static int ctx_resolve_capture(CompCtx *ctx, const char *name, char sigil) {
    int cap = ctx_find_capture(ctx, name);
    if (cap >= 0) return cap;
    if (!ctx->is_closure || !ctx->parent_ctx) return -1;
    CompCtx *parent = (CompCtx*)ctx->parent_ctx;
    int parent_slot = ctx_find_local_sigil(parent, name, sigil);
    if (parent_slot >= 0) return ctx_add_capture(ctx, name, parent_slot);
    int pcap = ctx_resolve_capture(parent, name, sigil);
    if (pcap >= 0) return ctx_add_capture(ctx, name, -1);
    return -1;
}

static size_t emit_patch_u16(CompCtx *ctx) {
    size_t off = ctx->chunk->code_len;
    vm_chunk_emit_u16(ctx->chunk, 0);
    return off;
}

static void patch_u16(CompCtx *ctx, size_t off, uint16_t val) {
    ctx->chunk->code[off] = val & 0xFF;
    ctx->chunk->code[off+1] = (val >> 8) & 0xFF;
}

/* Forward declarations */
static void compile_expr(CompCtx *ctx, StradaValue *node);
static void compile_tr(CompCtx *ctx, StradaValue *node);
static size_t add_pattern_const(CompCtx *ctx, const char *pattern, const char *flags);
static void compile_stmt(CompCtx *ctx, StradaValue *node);
static void compile_block(CompCtx *ctx, StradaValue *block);
static void compile_map_expr(CompCtx *ctx, StradaValue *node);
static void compile_grep_expr(CompCtx *ctx, StradaValue *node);
static void compile_sort_expr(CompCtx *ctx, StradaValue *node);
static VMNativeEntry *vm_find_native(VMProgram *prog, const char *name);

static int loop_in_chain(LoopCtx *chain, LoopCtx *target) {
    for (LoopCtx *cur = chain; cur != NULL; cur = cur->parent) {
        if (cur == target) return 1;
    }
    return 0;
}

/* Unwind all active try/finally contexts in the current function before OP_RETURN.
 * Expects the return value to already be on top of the VM stack, and leaves it there. */
static void emit_try_unwind_for_return(CompCtx *ctx) {
    if (!ctx->try_ctx) return;
    int any_finally = 0;
    for (TryCtx *tc = ctx->try_ctx; tc != NULL; tc = tc->parent) {
        if (tc->finally_block) { any_finally = 1; break; }
    }
    int ret_slot = -1;
    if (any_finally) {
        ret_slot = ctx_get_or_add_local(ctx, "__ret_tmp", '$');
        vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)ret_slot);
    }
    TryCtx *saved_try = ctx->try_ctx;
    for (TryCtx *tc = saved_try; tc != NULL; tc = tc->parent) {
        for (int h = 0; h < tc->handlers_active; h++) {
            vm_chunk_emit(ctx->chunk, OP_TRY_END);
        }
        if (tc->finally_block) {
            ctx->try_ctx = tc->parent;
            compile_block(ctx, tc->finally_block);
        }
    }
    ctx->try_ctx = saved_try;
    if (any_finally) {
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)ret_slot);
    }
}

/* Unwind active try/finally contexts nested inside `target` loop before last/next/redo. */
static void emit_try_unwind_for_loop(CompCtx *ctx, LoopCtx *target) {
    if (!ctx->try_ctx || !target) return;
    TryCtx *saved_try = ctx->try_ctx;
    for (TryCtx *tc = saved_try; tc != NULL && loop_in_chain(tc->enclosing_loop, target); tc = tc->parent) {
        for (int h = 0; h < tc->handlers_active; h++) {
            vm_chunk_emit(ctx->chunk, OP_TRY_END);
        }
        if (tc->finally_block) {
            ctx->try_ctx = tc->parent;
            compile_block(ctx, tc->finally_block);
        }
    }
    ctx->try_ctx = saved_try;
}

/* Detect "str_literal" . expr pattern for hash concat key optimization */
static int is_concat_key(StradaValue *key_node, const char **prefix, StradaValue **suffix) {
    key_node = ast_deref(key_node);
    if (!key_node || STRADA_IS_TAGGED_INT(key_node)) return 0;
    if ((int)ast_int(key_node, "type") != NI_BINARY_OP) return 0;
    if (ast_op_code(key_node) != OP_AST_CONCAT) return 0;
    StradaValue *left = ast_deref(ast_get(key_node, "left"));
    if (!left || STRADA_IS_TAGGED_INT(left) || (int)ast_int(left, "type") != NI_STR_LITERAL) return 0;
    *prefix = ast_str(left, "value");
    *suffix = ast_get(key_node, "right");
    return 1;
}

/* ===== Expression compilation ===== */


/* Compile tr/// — pushes the result value (count, or the modified copy when
 * the /r flag is used). The subject variable is updated in place via the
 * OP_TR store_slot operand. */
static void compile_tr(CompCtx *ctx, StradaValue *node) {
    StradaValue *target = ast_get(node, "target");
    target = ast_deref(target);
    const char *from = ast_str(node, "search");
    const char *to = ast_str(node, "replace");
    const char *tr_flags = ast_str(node, "flags");
    if (!tr_flags) tr_flags = "";

    int slot = -1;
    if (target && !STRADA_IS_TAGGED_INT(target) &&
        (int)ast_int(target, "type") == NI_VARIABLE) {
        slot = ctx_find_local(ctx, ast_str(target, "name"));
    }
    if (slot >= 0) {
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
    } else {
        compile_expr(ctx, target);
    }
    size_t from_idx = vm_chunk_add_str_const(ctx->chunk, from);
    size_t to_idx = vm_chunk_add_str_const(ctx->chunk, to);
    size_t flags_idx = vm_chunk_add_str_const(ctx->chunk, tr_flags);
    vm_chunk_emit(ctx->chunk, OP_TR);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)from_idx);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)to_idx);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)flags_idx);
    vm_chunk_emit_u16(ctx->chunk, slot >= 0 ? (uint16_t)slot : (uint16_t)0xFFFF);
}


/* Compile s/PAT/EXPR/e[g]: an inline find/eval/append loop. The parser has
 * already parsed the replacement source into node->{eval_expr}; each match
 * sets the VM capture state, then the expression is evaluated inline and
 * appended. Pushes the rebuilt string AND stores it back into the target
 * variable slot/global. Value left on stack = the new string. */
static int compile_regex_subst_e(CompCtx *ctx, StradaValue *node) {
    StradaValue *target = ast_get(node, "target");
    target = ast_deref(target);
    StradaValue *eval_expr = ast_get(node, "eval_expr");
    if (!eval_expr || STRADA_IS_TAGGED_INT(eval_expr)) return 0;
    const char *pattern = ast_str(node, "pattern");
    const char *flags = ast_str(node, "flags");
    int is_global = flags && strchr(flags, 'g') != NULL;

    if (!target || STRADA_IS_TAGGED_INT(target) ||
        (int)ast_int(target, "type") != NI_VARIABLE) return 0;
    const char *vname = ast_str(target, "name");
    const char *vsigil = ast_str(target, "sigil");
    int slot = ctx_find_local_sigil(ctx, vname, vsigil[0]);
    int use_global = (slot < 0 && is_our_var(vname)) ? 1 : 0;
    if (slot < 0 && !use_global) return 0;

    size_t pat_idx = add_pattern_const(ctx, pattern, flags);
    int w = ctx_add_local(ctx, "__se_w", '$');   /* subject */
    int o = ctx_add_local(ctx, "__se_o", '$');   /* output */
    int pp = ctx_add_local(ctx, "__se_p", '$');  /* position */
    int ms = ctx_add_local(ctx, "__se_s", '$');  /* match start */
    int me = ctx_add_local(ctx, "__se_e", '$');  /* match end */
    int wl = ctx_add_local(ctx, "__se_l", '$');  /* byte length of subject */
    int le = ctx_add_local(ctx, "__se_le", '$'); /* end of previous non-empty match */

#define E8(op)   vm_chunk_emit(ctx->chunk, (op))
#define E16(v)   vm_chunk_emit_u16(ctx->chunk, (uint16_t)(v))
#define LD(sl)   do { E8(OP_LOAD_LOCAL); E16(sl); } while (0)
#define ST(sl)   do { E8(OP_STORE_LOCAL); E16(sl); } while (0)

    /* W = target; O = ""; P = 0; WL = bytes(W); LE = -1 */
    if (slot >= 0) LD(slot);
    else { size_t gk = vm_chunk_add_str_const(ctx->chunk, vname); E8(OP_LOAD_GLOBAL); E16(gk); }
    ST(w);
    LD(w); E8(OP_BYTES); ST(wl);
    size_t empty_idx = vm_chunk_add_str_const(ctx->chunk, "");
    E8(OP_PUSH_STR); E16(empty_idx); ST(o);
    E8(OP_PUSH_INT); vm_chunk_emit_i64(ctx->chunk, 0); ST(pp);
    E8(OP_PUSH_INT); vm_chunk_emit_i64(ctx->chunk, -1); ST(le);

    size_t loop_top = ctx->chunk->code_len;
    /* (S, E) = find(W, P) */
    LD(w); LD(pp); E8(OP_REGEX_FIND); E16(pat_idx);
    ST(me); ST(ms);
    /* if S < 0 goto done */
    LD(ms); E8(OP_PUSH_INT); vm_chunk_emit_i64(ctx->chunk, 0); E8(OP_LT);
    E8(OP_JMP_IF_TRUE); size_t done_patch = emit_patch_u16(ctx);
    /* O = O . substr(W, P, S-P) */
    LD(o); LD(w); LD(pp); LD(ms); LD(pp); E8(OP_SUB); E8(OP_SUBSTR); E8(OP_CONCAT); ST(o);

    size_t after_tail_patches[4];
    int atp_count = 0;

    if (is_global) {
        /* Skip duplicate zero-width match right at end of previous non-empty match */
        LD(ms); LD(me); E8(OP_EQ);
        E8(OP_JMP_IF_FALSE); size_t not_skip_empty = emit_patch_u16(ctx);
        LD(ms); LD(le); E8(OP_EQ);
        E8(OP_JMP_IF_FALSE); size_t not_at_le = emit_patch_u16(ctx);
        LD(me); LD(wl); E8(OP_GE);
        E8(OP_JMP_IF_TRUE); after_tail_patches[atp_count++] = emit_patch_u16(ctx);
        LD(o); LD(w); LD(me); E8(OP_PUSH_INT); vm_chunk_emit_i64(ctx->chunk, 1); E8(OP_SUBSTR); E8(OP_CONCAT); ST(o);
        LD(me); E8(OP_PUSH_INT); vm_chunk_emit_i64(ctx->chunk, 1); E8(OP_ADD); ST(pp);
        E8(OP_PUSH_INT); vm_chunk_emit_i64(ctx->chunk, -1); ST(le);
        E8(OP_JMP); size_t back0 = emit_patch_u16(ctx);
        patch_u16(ctx, back0, (uint16_t)loop_top);
        patch_u16(ctx, not_at_le, (uint16_t)ctx->chunk->code_len);
        patch_u16(ctx, not_skip_empty, (uint16_t)ctx->chunk->code_len);
    }

    /* O = O . <eval_expr> (captures are live) */
    LD(o); compile_expr(ctx, eval_expr); E8(OP_CONCAT); ST(o);

    if (is_global) {
        /* If zero-width match (E == S): if E >= wl finish, else copy 1 char and P = E + 1 */
        LD(me); LD(ms); E8(OP_EQ);
        E8(OP_JMP_IF_FALSE); size_t non_empty_patch = emit_patch_u16(ctx);
        LD(me); LD(wl); E8(OP_GE);
        E8(OP_JMP_IF_TRUE); after_tail_patches[atp_count++] = emit_patch_u16(ctx);
        LD(o); LD(w); LD(me); E8(OP_PUSH_INT); vm_chunk_emit_i64(ctx->chunk, 1); E8(OP_SUBSTR); E8(OP_CONCAT); ST(o);
        LD(me); E8(OP_PUSH_INT); vm_chunk_emit_i64(ctx->chunk, 1); E8(OP_ADD); ST(pp);
        E8(OP_PUSH_INT); vm_chunk_emit_i64(ctx->chunk, -1); ST(le);
        E8(OP_JMP); size_t back1 = emit_patch_u16(ctx);
        patch_u16(ctx, back1, (uint16_t)loop_top);
        patch_u16(ctx, non_empty_patch, (uint16_t)ctx->chunk->code_len);
        LD(me); ST(pp);
        LD(me); ST(le);
        E8(OP_JMP); size_t back2 = emit_patch_u16(ctx);
        patch_u16(ctx, back2, (uint16_t)loop_top);
    } else {
        LD(me); ST(pp);
    }

    patch_u16(ctx, done_patch, (uint16_t)ctx->chunk->code_len);
    /* O = O . substr(W, P, wl-P) */
    LD(o); LD(w); LD(pp); LD(wl); LD(pp); E8(OP_SUB); E8(OP_SUBSTR); E8(OP_CONCAT); ST(o);
    for (int pi = 0; pi < atp_count; pi++) {
        patch_u16(ctx, after_tail_patches[pi], (uint16_t)ctx->chunk->code_len);
    }
    /* store back + leave value */
    LD(o);
    if (slot >= 0) { ST(slot); LD(slot); }
    else { size_t gk = vm_chunk_add_str_const(ctx->chunk, vname); E8(OP_DUP); E8(OP_STORE_GLOBAL); E16(gk); }
    return 1;

#undef E8
#undef E16
#undef LD
#undef ST
}


/* Add a regex pattern string constant, folding Perl match flags (i/s/m/x)
 * into an inline (?...) prefix so the runtime needs no per-match options. */
static size_t add_pattern_const(CompCtx *ctx, const char *pattern, const char *flags) {
    char mods[8];
    int mi = 0;
    if (flags) {
        if (strchr(flags, 'i')) mods[mi++] = 'i';
        if (strchr(flags, 's')) mods[mi++] = 's';
        if (strchr(flags, 'm')) mods[mi++] = 'm';
        if (strchr(flags, 'x')) mods[mi++] = 'x';
    }
    if (mi == 0) return vm_chunk_add_str_const(ctx->chunk, pattern);
    mods[mi] = 0;
    size_t plen = strlen(pattern) + mi + 8;
    char *buf = malloc(plen);
    snprintf(buf, plen, "(?%s)%s", mods, pattern);
    size_t idx = vm_chunk_add_str_const(ctx->chunk, buf);
    free(buf);
    return idx;
}

#define AST_TYPE_ARRAY 4

static int resolve_vm_func_idx(CompCtx *ctx, const char *name) {
    int fidx = vm_program_find_func(ctx->prog, name);
    if (fidx < 0 && !strchr(name, ':') && ctx->cur_package && ctx->cur_package[0] &&
        strcmp(ctx->cur_package, "main") != 0) {
        char pkgname[256];
        snprintf(pkgname, sizeof(pkgname), "%s_%s", ctx->cur_package, name);
        fidx = vm_program_find_func(ctx->prog, pkgname);
        if (fidx < 0) {
            snprintf(pkgname, sizeof(pkgname), "%s::%s", ctx->cur_package, name);
            fidx = vm_program_find_func(ctx->prog, pkgname);
        }
    }
    if (fidx < 0 && strchr(name, ':')) {
        char normalized[256];
        size_t j = 0;
        for (size_t k = 0; name[k] && j < sizeof(normalized) - 1; k++) {
            if (name[k] == ':' && name[k+1] == ':') {
                normalized[j++] = '_';
                k++;
            } else {
                normalized[j++] = name[k];
            }
        }
        normalized[j] = '\0';
        fidx = vm_program_find_func(ctx->prog, normalized);
    }
    return fidx;
}

static int vm_array_flatten_kind(CompCtx *ctx, StradaValue *elem) {
    elem = ast_deref(elem);
    if (!elem || STRADA_IS_TAGGED_INT(elem)) return 0;
    int t = (int)ast_int(elem, "type");
    if (t == NI_VARIABLE) {
        const char *sig = ast_str(elem, "sigil");
        if (sig[0] == '@') return 1;
        return 0;
    }
    if (t == NI_ANON_ARRAY && (int)ast_int(elem, "is_list") == 1) return 2;
    if (t == NI_MAP || t == NI_GREP || t == NI_SORT) return 2;
    if (t == NI_CALL) {
        const char *nm = ast_str(elem, "name");
        const char *check_nm = nm;
        char nbuf[256];
        if (strncmp(nm, "core::", 6) == 0) {
            snprintf(nbuf, sizeof(nbuf), "sys::%s", nm + 6);
            check_nm = nbuf;
        }
        if (strcmp(check_nm, "split") == 0 || strcmp(check_nm, "keys") == 0 ||
            strcmp(check_nm, "values") == 0 || strcmp(check_nm, "reverse") == 0 ||
            strcmp(check_nm, "sys::split") == 0 || strcmp(check_nm, "sys::keys") == 0 ||
            strcmp(check_nm, "sys::values") == 0 || strcmp(check_nm, "sys::reverse") == 0) {
            return 2;
        }
        int fidx = resolve_vm_func_idx(ctx, nm);
        if (fidx >= 0 && ctx->prog->funcs[fidx].return_type == AST_TYPE_ARRAY) {
            return 2;
        }
    }
    return 0;
}

static void compile_expr(CompCtx *ctx, StradaValue *node) {
    if (!node || STRADA_IS_TAGGED_INT(node)) {
        vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
        return;
    }
    node = ast_deref(node);
    if (!node || STRADA_IS_TAGGED_INT(node)) {
        vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
        return;
    }

    int type = (int)ast_int(node, "type");

    switch (type) {
    case NI_INT_LITERAL: {
        int64_t val = ast_int(node, "value");
        vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
        vm_chunk_emit_i64(ctx->chunk, val);
        break;
    }

    case NI_NUM_LITERAL: {
        StradaValue *v = ast_get(node, "value");
        double d = 0.0;
        if (v && !STRADA_IS_TAGGED_INT(v)) {
            if (v->type == STRADA_STR && v->value.pv) d = strtod(v->value.pv, NULL);
            else d = strada_to_num(v);
        } else {
            d = (double)ast_int(node, "value");
        }
        vm_chunk_emit(ctx->chunk, OP_PUSH_FLOAT);
        vm_chunk_emit_f64(ctx->chunk, d);
        break;
    }

    case NI_STR_LITERAL: {
        const char *val = ast_str(node, "value");
        size_t idx = vm_chunk_add_str_const(ctx->chunk, val);
        vm_chunk_emit(ctx->chunk, OP_PUSH_STR);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx);
        break;
    }

    case NI_UNDEF:
        vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
        break;

    case NI_VARIABLE: {
        const char *name = ast_str(node, "name");
        const char *sigil = ast_str(node, "sigil");

        /* Check capture vars ($1, $2, etc) */
        if (sigil[0] == '$' && name[0] >= '1' && name[0] <= '9' && name[1] == '\0') {
            vm_chunk_emit(ctx->chunk, OP_LOAD_CAPTURE_VAR);
            vm_chunk_emit(ctx->chunk, (uint8_t)(name[0] - '0'));
            break;
        }

        int slot = ctx_find_local_sigil(ctx, name, sigil[0]);
        if (slot >= 0) {
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
        } else {
            /* Try capture (including transitive multi-level parent captures) */
            int cap = ctx_resolve_capture(ctx, name, sigil[0]);
            if (cap >= 0) {
                vm_chunk_emit(ctx->chunk, OP_LOAD_CAPTURE);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)cap);
            } else if (is_our_var(name) || is_const_var(name)) {
                /* our variable or const inside closure / function */
                size_t key_idx = vm_chunk_add_str_const(ctx->chunk, name);
                vm_chunk_emit(ctx->chunk, OP_LOAD_GLOBAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)key_idx);
            } else if (strcmp(name, "ARGV") == 0) {
                vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                vm_chunk_emit_u16(ctx->chunk, BUILTIN_CORE_ARGV);
                vm_chunk_emit(ctx->chunk, 0);
            } else if (strcmp(name, "ARGC") == 0) {
                vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                vm_chunk_emit_u16(ctx->chunk, BUILTIN_CORE_ARGV);
                vm_chunk_emit(ctx->chunk, 0);
                vm_chunk_emit(ctx->chunk, OP_ARRAY_SIZE);
            } else {
                vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
            }
        }
        break;
    }

    case NI_BINARY_OP: {
        int op = ast_op_code(node);

        /* Short-circuit: && */
        if (op == OP_AST_LOG_AND) {
            compile_expr(ctx, ast_get(node, "left"));
            vm_chunk_emit(ctx->chunk, OP_DUP);
            vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
            size_t patch = emit_patch_u16(ctx);
            vm_chunk_emit(ctx->chunk, OP_POP);
            compile_expr(ctx, ast_get(node, "right"));
            patch_u16(ctx, patch, (uint16_t)ctx->chunk->code_len);
            break;
        }
        /* Short-circuit: || */
        if (op == OP_AST_LOG_OR) {
            compile_expr(ctx, ast_get(node, "left"));
            vm_chunk_emit(ctx->chunk, OP_DUP);
            vm_chunk_emit(ctx->chunk, OP_JMP_IF_TRUE);
            size_t patch = emit_patch_u16(ctx);
            vm_chunk_emit(ctx->chunk, OP_POP);
            compile_expr(ctx, ast_get(node, "right"));
            patch_u16(ctx, patch, (uint16_t)ctx->chunk->code_len);
            break;
        }
        /* Defined-or: // */
        if (op == OP_AST_DEFOR) {
            compile_expr(ctx, ast_get(node, "left"));
            vm_chunk_emit(ctx->chunk, OP_DUP);
            vm_chunk_emit(ctx->chunk, OP_IS_UNDEF);
            vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
            size_t patch = emit_patch_u16(ctx);
            vm_chunk_emit(ctx->chunk, OP_POP);
            compile_expr(ctx, ast_get(node, "right"));
            patch_u16(ctx, patch, (uint16_t)ctx->chunk->code_len);
            break;
        }

        compile_expr(ctx, ast_get(node, "left"));
        compile_expr(ctx, ast_get(node, "right"));

        switch (op) {
        case OP_AST_ADD:    vm_chunk_emit(ctx->chunk, OP_ADD); break;
        case OP_AST_SUB:    vm_chunk_emit(ctx->chunk, OP_SUB); break;
        case OP_AST_MUL:    vm_chunk_emit(ctx->chunk, OP_MUL); break;
        case OP_AST_DIV:    vm_chunk_emit(ctx->chunk, OP_DIV); break;
        case OP_AST_MOD:    vm_chunk_emit(ctx->chunk, OP_MOD); break;
        case OP_AST_CONCAT: vm_chunk_emit(ctx->chunk, OP_CONCAT); break;
        case OP_AST_REPEAT: vm_chunk_emit(ctx->chunk, OP_STR_REPEAT); break;
        case OP_AST_POW:    vm_chunk_emit(ctx->chunk, OP_POWER); break;
        case OP_AST_NUM_EQ: vm_chunk_emit(ctx->chunk, OP_EQ); break;
        case OP_AST_NUM_NE: vm_chunk_emit(ctx->chunk, OP_NE); break;
        case OP_AST_NUM_LT: vm_chunk_emit(ctx->chunk, OP_LT); break;
        case OP_AST_NUM_GT: vm_chunk_emit(ctx->chunk, OP_GT); break;
        case OP_AST_NUM_LE: vm_chunk_emit(ctx->chunk, OP_LE); break;
        case OP_AST_NUM_GE: vm_chunk_emit(ctx->chunk, OP_GE); break;
        case OP_AST_STR_EQ: vm_chunk_emit(ctx->chunk, OP_STR_EQ); break;
        case OP_AST_STR_NE: vm_chunk_emit(ctx->chunk, OP_STR_NE); break;
        case OP_AST_STR_LT: vm_chunk_emit(ctx->chunk, OP_STR_LT); break;
        case OP_AST_STR_GT: vm_chunk_emit(ctx->chunk, OP_STR_GT); break;
        case OP_AST_STR_LE: vm_chunk_emit(ctx->chunk, OP_STR_LE); break;
        case OP_AST_STR_GE: vm_chunk_emit(ctx->chunk, OP_STR_GE); break;
        case OP_AST_SPACESHIP: vm_chunk_emit(ctx->chunk, OP_SPACESHIP); break;
        case OP_AST_STR_CMP: vm_chunk_emit(ctx->chunk, OP_STR_CMP); break;
        case OP_AST_BIT_AND: vm_chunk_emit(ctx->chunk, OP_BIT_AND); break;
        case OP_AST_BIT_OR:  vm_chunk_emit(ctx->chunk, OP_BIT_OR); break;
        case OP_AST_BIT_XOR: vm_chunk_emit(ctx->chunk, OP_BIT_XOR); break;
        case OP_AST_BIT_SHL: vm_chunk_emit(ctx->chunk, OP_BIT_SHL); break;
        case OP_AST_BIT_SHR: vm_chunk_emit(ctx->chunk, OP_BIT_SHR); break;
        default:
            vm_chunk_emit(ctx->chunk, OP_POP);
            vm_chunk_emit(ctx->chunk, OP_POP);
            vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
            break;
        }
        break;
    }

    case NI_UNARY_OP: {
        const char *op = ast_str(node, "op");
        compile_expr(ctx, ast_get(node, "operand"));
        if (strcmp(op, "-") == 0) vm_chunk_emit(ctx->chunk, OP_NEGATE);
        else if (strcmp(op, "!") == 0 || strcmp(op, "not") == 0) vm_chunk_emit(ctx->chunk, OP_NOT);
        else if (strcmp(op, "~") == 0) vm_chunk_emit(ctx->chunk, OP_BIT_NOT);
        else if (strcmp(op, "\\") == 0) vm_chunk_emit(ctx->chunk, OP_HASH_REF); /* ref operator */
        break;
    }

    case NI_TERNARY: {
        compile_expr(ctx, ast_get(node, "condition"));
        vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
        size_t else_patch = emit_patch_u16(ctx);
        compile_expr(ctx, ast_get(node, "true_expr"));
        vm_chunk_emit(ctx->chunk, OP_JMP);
        size_t end_patch = emit_patch_u16(ctx);
        patch_u16(ctx, else_patch, (uint16_t)ctx->chunk->code_len);
        compile_expr(ctx, ast_get(node, "false_expr"));
        patch_u16(ctx, end_patch, (uint16_t)ctx->chunk->code_len);
        break;
    }

    case NI_SUBSCRIPT: {
        compile_expr(ctx, ast_get(node, "array"));
        compile_expr(ctx, ast_get(node, "index"));
        vm_chunk_emit(ctx->chunk, OP_ARRAY_GET);
        break;
    }

    case NI_HASH_ACCESS: {
        const char *prefix; StradaValue *suffix;
        if (is_concat_key(ast_get(node, "key"), &prefix, &suffix)) {
            compile_expr(ctx, ast_get(node, "hash"));
            compile_expr(ctx, suffix);
            size_t pidx = vm_chunk_add_str_const(ctx->chunk, prefix);
            vm_chunk_emit(ctx->chunk, OP_HASH_GET_CONCAT);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)pidx);
        } else {
            compile_expr(ctx, ast_get(node, "hash"));
            compile_expr(ctx, ast_get(node, "key"));
            vm_chunk_emit(ctx->chunk, OP_HASH_GET);
        }
        break;
    }

    case NI_DEREF_HASH: {
        compile_expr(ctx, ast_get(node, "ref"));
        compile_expr(ctx, ast_get(node, "key"));
        vm_chunk_emit(ctx->chunk, OP_HASH_GET);
        break;
    }

    case NI_DEREF_ARRAY: {
        compile_expr(ctx, ast_get(node, "ref"));
        compile_expr(ctx, ast_get(node, "index"));
        vm_chunk_emit(ctx->chunk, OP_ARRAY_GET);
        break;
    }

    case NI_DEREF_SCALAR: {
        compile_expr(ctx, ast_get(node, "ref"));
        break;
    }

    case NI_REF: {
        compile_expr(ctx, ast_get(node, "target"));
        vm_chunk_emit(ctx->chunk, OP_HASH_REF);
        break;
    }

    case NI_ANON_HASH: {
        StradaValue *keys_arr = ast_deref(ast_get(node, "keys"));
        StradaValue *vals_arr = ast_deref(ast_get(node, "values"));
        StradaValue *key_exprs = ast_deref(ast_get(node, "key_exprs"));
        int count = (int)ast_int(node, "pair_count");
        vm_chunk_emit(ctx->chunk, OP_NEW_HASH);
        vm_chunk_emit_u32(ctx->chunk, count > 0 ? count : 16);
        for (int i = 0; i < count; i++) {
            vm_chunk_emit(ctx->chunk, OP_DUP); /* hash on top */
            /* Check for expression key first */
            StradaValue *key_expr = key_exprs ? ast_arr_get(key_exprs, i) : NULL;
            key_expr = ast_deref(key_expr);
            int has_expr_key = (key_expr && !STRADA_IS_TAGGED_INT(key_expr) &&
                                key_expr->type == STRADA_HASH);
            if (has_expr_key) {
                compile_expr(ctx, key_expr);
            } else if (keys_arr && keys_arr->type == STRADA_ARRAY) {
                /* Static string key */
                StradaValue *kv = strada_array_get(keys_arr->value.av, i);
                const char *ks = (kv && !STRADA_IS_TAGGED_INT(kv) && kv->type == STRADA_STR) ? kv->value.pv : "";
                size_t kidx = vm_chunk_add_str_const(ctx->chunk, ks);
                vm_chunk_emit(ctx->chunk, OP_PUSH_STR);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)kidx);
            } else {
                vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
            }
            if (vals_arr && vals_arr->type == STRADA_ARRAY) {
                compile_expr(ctx, ast_arr_get(vals_arr, i));
            } else {
                vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
            }
            vm_chunk_emit(ctx->chunk, OP_HASH_SET);
        }
        break;
    }

    case NI_ANON_ARRAY: {
        StradaValue *elements = ast_get(node, "elements");
        int count = (int)ast_int(node, "element_count");
        vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
        vm_chunk_emit_u32(ctx->chunk, count > 0 ? count : 8);
        for (int i = 0; i < count; i++) {
            StradaValue *elem = ast_deref(ast_arr_get(elements, i));
            int et = elem ? (int)ast_int(elem, "type") : 0;
            vm_chunk_emit(ctx->chunk, OP_DUP); /* array on top */
            if (et == NI_SPREAD) {
                compile_expr(ctx, ast_get(elem, "target"));
                vm_chunk_emit(ctx->chunk, OP_ARRAY_EXTEND);
            } else {
                compile_expr(ctx, elem);
                vm_chunk_emit(ctx->chunk, OP_ARRAY_PUSH);
            }
        }
        break;
    }

    case NI_FIELD_ACCESS: {
        compile_expr(ctx, ast_get(node, "object"));
        break;
    }

    case NI_METHOD_CALL: {
        const char *method = ast_str(node, "method");
        StradaValue *args = ast_get(node, "args");
        int argc = (int)ast_int(node, "arg_count");
        StradaValue *base_obj = ast_get(node, "base_object");

        /* Special: isa() */
        if (strcmp(method, "isa") == 0 && argc == 1) {
            compile_expr(ctx, base_obj);
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_ISA);
            break;
        }

        /* Special: can() */
        if (strcmp(method, "can") == 0 && argc == 1) {
            compile_expr(ctx, base_obj);
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_CAN);
            break;
        }

        int has_spread = 0;
        for (int i = 0; i < argc; i++) {
            StradaValue *arg = ast_deref(ast_arr_get(args, i));
            if (arg && (int)ast_int(arg, "type") == NI_SPREAD) {
                has_spread = 1;
                break;
            }
        }

        if (has_spread) {
            compile_expr(ctx, base_obj);
            vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
            vm_chunk_emit_u32(ctx->chunk, argc > 0 ? (uint32_t)argc * 2 : 8);
            for (int i = 0; i < argc; i++) {
                StradaValue *arg = ast_deref(ast_arr_get(args, i));
                int atype = arg ? (int)ast_int(arg, "type") : 0;
                vm_chunk_emit(ctx->chunk, OP_DUP);
                if (atype == NI_SPREAD) {
                    compile_expr(ctx, ast_get(arg, "target"));
                    vm_chunk_emit(ctx->chunk, OP_ARRAY_EXTEND);
                } else {
                    compile_expr(ctx, arg);
                    vm_chunk_emit(ctx->chunk, OP_ARRAY_PUSH);
                }
            }
            size_t midx = vm_chunk_add_str_const(ctx->chunk, method);
            vm_chunk_emit(ctx->chunk, OP_METHOD_CALL_SPREAD);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)midx);
            break;
        }

        /* Push self first, then args */
        compile_expr(ctx, base_obj);
        for (int i = 0; i < argc; i++) {
            compile_expr(ctx, ast_arr_get(args, i));
        }

        /* Use dynamic method dispatch */
        size_t midx = vm_chunk_add_str_const(ctx->chunk, method);
        vm_chunk_emit(ctx->chunk, OP_METHOD_CALL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)midx);
        vm_chunk_emit(ctx->chunk, (uint8_t)argc);
        break;
    }

    case NI_DYN_METHOD_CALL: {
        StradaValue *args = ast_get(node, "args");
        int argc = (int)ast_int(node, "arg_count");
        StradaValue *base_obj = ast_get(node, "base_object");
        StradaValue *method_expr = ast_get(node, "method_expr");

        /* Push self first, then args, then method name */
        compile_expr(ctx, base_obj);
        for (int i = 0; i < argc; i++) {
            compile_expr(ctx, ast_arr_get(args, i));
        }
        compile_expr(ctx, method_expr);

        vm_chunk_emit(ctx->chunk, OP_DYN_METHOD_CALL);
        vm_chunk_emit(ctx->chunk, (uint8_t)argc);
        break;
    }

    case NI_CLOSURE_CALL: {
        /* $func_ref->(args) */
        StradaValue *callee = ast_get(node, "closure");
        StradaValue *args = ast_get(node, "args");
        int argc = (int)ast_int(node, "arg_count");

        int has_spread = 0;
        for (int i = 0; i < argc; i++) {
            StradaValue *arg = ast_deref(ast_arr_get(args, i));
            if (arg && (int)ast_int(arg, "type") == NI_SPREAD) {
                has_spread = 1;
                break;
            }
        }
        if (has_spread) {
            vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
            vm_chunk_emit_u32(ctx->chunk, argc > 0 ? (uint32_t)argc * 2 : 8);
            for (int i = 0; i < argc; i++) {
                StradaValue *arg = ast_deref(ast_arr_get(args, i));
                int atype = arg ? (int)ast_int(arg, "type") : 0;
                vm_chunk_emit(ctx->chunk, OP_DUP);
                if (atype == NI_SPREAD) {
                    compile_expr(ctx, ast_get(arg, "target"));
                    vm_chunk_emit(ctx->chunk, OP_ARRAY_EXTEND);
                } else {
                    compile_expr(ctx, arg);
                    vm_chunk_emit(ctx->chunk, OP_ARRAY_PUSH);
                }
            }
            compile_expr(ctx, callee);
            vm_chunk_emit(ctx->chunk, OP_CALL_CLOSURE_SPREAD);
            break;
        }

        for (int i = 0; i < argc; i++) {
            compile_expr(ctx, ast_arr_get(args, i));
        }
        compile_expr(ctx, callee);
        vm_chunk_emit(ctx->chunk, OP_CALL_CLOSURE);
        vm_chunk_emit(ctx->chunk, (uint8_t)argc);
        break;
    }

    case NI_ANON_FUNC: {
        /* Anonymous function / closure */
        StradaValue *params = ast_get(node, "params");
        int param_count = (int)ast_int(node, "param_count");
        StradaValue *body = ast_get(node, "body");

        /* Create a unique name for the closure function */
        char closurename[128];
        static int closure_counter = 0;
        snprintf(closurename, sizeof(closurename), "__closure_%d", closure_counter++);

        /* Save outer chunk index before adding new func (may realloc prog->funcs) */
        int outer_func_idx = -1;
        for (size_t fi = 0; fi < ctx->prog->func_count; fi++) {
            if (&ctx->prog->funcs[fi] == ctx->chunk) { outer_func_idx = fi; break; }
        }

        /* Compile the closure body into a new function */
        CompCtx inner = {0};
        inner.chunk = vm_program_add_func(ctx->prog, closurename);

        /* Restore outer chunk pointer (may have been invalidated by realloc) */
        if (outer_func_idx >= 0) ctx->chunk = &ctx->prog->funcs[outer_func_idx];

        inner.prog = ctx->prog;
        inner.func_name = closurename;
        inner.cur_package = ctx->cur_package;
        inner.is_closure = 1;
        inner.parent_ctx = ctx;

        /* Add parameters */
        int fixed_params = 0;
        for (int i = 0; i < param_count; i++) {
            StradaValue *param = ast_arr_get(params, i);
            const char *pname = ast_str(param, "name");
            const char *psigil = ast_str(param, "sigil");
            int is_variadic = (int)ast_int(param, "is_variadic");
            ctx_add_local(&inner, pname, psigil[0] ? psigil[0] : '$');
            if (is_variadic || psigil[0] == '@') {
                inner.chunk->has_variadic = 1;
            } else {
                fixed_params++;
            }
        }
        inner.chunk->fixed_param_count = fixed_params;

        compile_block(&inner, body);

        /* Implicit return undef */
        vm_chunk_emit(inner.chunk, OP_PUSH_UNDEF);
        vm_chunk_emit(inner.chunk, OP_RETURN);

        inner.chunk->local_count = inner.local_count;
        inner.chunk->capture_count = inner.capture_count;
        if (inner.capture_count > 0) {
            inner.chunk->capture_names = calloc(inner.capture_count, sizeof(char*));
            for (int ci = 0; ci < inner.capture_count; ci++) {
                inner.chunk->capture_names[ci] = strdup(inner.captures[ci].name);
            }
        }

        /* Now emit code in the parent to create the closure */
        int fidx = vm_program_find_func(ctx->prog, closurename);

        /* Push captured variables onto stack and track outer slots */
        int outer_slots[64];
        for (int i = 0; i < inner.capture_count; i++) {
            int outer_slot = ctx_find_local(ctx, inner.captures[i].name);
            if (outer_slot >= 0) {
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)outer_slot);
                outer_slots[i] = outer_slot;
            } else {
                /* Try capture from parent (pass raw VMCell so multi-level closures share mutation) */
                int cap = ctx_find_capture(ctx, inner.captures[i].name);
                if (cap >= 0) {
                    vm_chunk_emit(ctx->chunk, OP_LOAD_CAPTURE_CELL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)cap);
                    outer_slots[i] = 0xFFFF; /* no outer local to store back to */
                } else {
                    vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
                    outer_slots[i] = 0xFFFF;
                }
            }
        }

        vm_chunk_emit(ctx->chunk, OP_MAKE_CLOSURE);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)fidx);
        vm_chunk_emit(ctx->chunk, (uint8_t)inner.capture_count);
        /* Emit outer slot indices for capture-by-reference cell creation */
        for (int i = 0; i < inner.capture_count; i++) {
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)outer_slots[i]);
        }

        /* Cleanup inner context */
        for (int i = 0; i < inner.local_count; i++) free(inner.local_names[i]);
        for (int i = 0; i < inner.capture_count; i++) free(inner.captures[i].name);
        break;
    }

    case NI_REGEX_MATCH: {
        const char *pattern = ast_str(node, "pattern");
        const char *op = ast_str(node, "op");
        const char *mflags = ast_str(node, "flags");
        int negated = (strcmp(op, "!~") == 0);
        compile_expr(ctx, ast_get(node, "target"));
        size_t pidx = add_pattern_const(ctx, pattern, mflags);
        vm_chunk_emit(ctx->chunk, negated ? OP_REGEX_NOT_MATCH : OP_REGEX_MATCH);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)pidx);
        vm_chunk_emit(ctx->chunk, 0); /* flags */
        break;
    }

    case NI_CAPTURE_VAR: {
        int num = (int)ast_int(node, "number");
        vm_chunk_emit(ctx->chunk, OP_LOAD_CAPTURE_VAR);
        vm_chunk_emit(ctx->chunk, (uint8_t)num);
        break;
    }

    case NI_READLINE: {
        const char *name = ast_str(node, "varname");
        int slot = ctx_find_local_sigil(ctx, name, '$');
        if (slot >= 0) {
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
        } else {
            int cap = ctx_resolve_capture(ctx, name, '$');
            if (cap >= 0) {
                vm_chunk_emit(ctx->chunk, OP_LOAD_CAPTURE);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)cap);
            } else if (is_our_var(name) || is_const_var(name)) {
                size_t key_idx = vm_chunk_add_str_const(ctx->chunk, name);
                vm_chunk_emit(ctx->chunk, OP_LOAD_GLOBAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)key_idx);
            } else {
                vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
            }
        }
        vm_chunk_emit(ctx->chunk, OP_BUILTIN);
        vm_chunk_emit_u16(ctx->chunk, BUILTIN_CORE_READLINE);
        vm_chunk_emit(ctx->chunk, 1);
        break;
    }

    case NI_CALL: {
        const char *name = ast_str(node, "name");
        StradaValue *args = ast_get(node, "args");
        int argc = (int)ast_int(node, "arg_count");

        /* Built-in: say() */
        if (strcmp(name, "say") == 0) {
            if (argc == 1) {
                compile_expr(ctx, ast_arr_get(args, 0));
                vm_chunk_emit(ctx->chunk, OP_SAY);
                vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
            } else if (argc == 2) {
                /* say($fh, $str) */
                compile_expr(ctx, ast_arr_get(args, 0));
                compile_expr(ctx, ast_arr_get(args, 1));
                vm_chunk_emit(ctx->chunk, OP_SAY_FH);
                vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
            } else {
                vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
            }
            break;
        }

        /* Built-in: print() */
        if (strcmp(name, "print") == 0) {
            if (argc == 1) {
                compile_expr(ctx, ast_arr_get(args, 0));
                vm_chunk_emit(ctx->chunk, OP_PRINT);
                vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
            } else if (argc == 2) {
                compile_expr(ctx, ast_arr_get(args, 0));
                compile_expr(ctx, ast_arr_get(args, 1));
                vm_chunk_emit(ctx->chunk, OP_PRINT_FH);
                vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
            } else {
                vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
            }
            break;
        }

        /* Built-in: length() */
        if (strcmp(name, "length") == 0 && argc == 1) {
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_STR_LEN);
            break;
        }

        /* Built-in: push(@arr, val) */
        if (strcmp(name, "push") == 0 && argc >= 2) {
            compile_expr(ctx, ast_arr_get(args, 0));
            for (int pi = 1; pi < argc; pi++) {
                StradaValue *parg = ast_deref(ast_arr_get(args, pi));
                int pat = parg ? (int)ast_int(parg, "type") : 0;
                vm_chunk_emit(ctx->chunk, OP_DUP); /* keep array on stack */
                if (pat == NI_SPREAD) {
                    compile_expr(ctx, ast_get(parg, "target"));
                    vm_chunk_emit(ctx->chunk, OP_ARRAY_EXTEND);
                } else {
                    compile_expr(ctx, parg);
                    vm_chunk_emit(ctx->chunk, OP_ARRAY_PUSH);
                }
            }
            vm_chunk_emit(ctx->chunk, OP_ARRAY_SIZE); /* return new size */
            break;
        }

        /* Built-in: pop() */
        if (strcmp(name, "pop") == 0 && argc == 1) {
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_ARRAY_POP);
            break;
        }

        /* Built-in: shift() */
        if (strcmp(name, "shift") == 0 && argc == 1) {
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_ARRAY_SHIFT);
            break;
        }

        /* Built-in: unshift() */
        if (strcmp(name, "unshift") == 0 && argc == 2) {
            compile_expr(ctx, ast_arr_get(args, 0));
            compile_expr(ctx, ast_arr_get(args, 1));
            vm_chunk_emit(ctx->chunk, OP_ARRAY_UNSHIFT);
            vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
            break;
        }

        /* Built-in: reverse() */
        if (strcmp(name, "reverse") == 0 && argc == 1) {
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_ARRAY_REVERSE);
            break;
        }

        /* Built-in: scalar(@arr) or size(@arr) */
        if ((strcmp(name, "scalar") == 0 || strcmp(name, "size") == 0) && argc == 1) {
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_ARRAY_SIZE);
            break;
        }

        /* Built-in: keys(%hash) */
        if (strcmp(name, "keys") == 0 && argc == 1) {
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_HASH_KEYS);
            break;
        }

        /* Built-in: exists() with 2 args: exists(\%hash, "key") */
        if (strcmp(name, "exists") == 0 && argc == 2) {
            compile_expr(ctx, ast_arr_get(args, 0));
            compile_expr(ctx, ast_arr_get(args, 1));
            vm_chunk_emit(ctx->chunk, OP_HASH_EXISTS);
            break;
        }

        /* Built-in: exists() */
        if (strcmp(name, "exists") == 0 && argc == 1) {
            StradaValue *arg0 = ast_deref(ast_arr_get(args, 0));
            if (arg0 && !STRADA_IS_TAGGED_INT(arg0)) {
                int atype = (int)ast_int(arg0, "type");
                if (atype == NI_HASH_ACCESS) {
                    compile_expr(ctx, ast_get(arg0, "hash"));
                    compile_expr(ctx, ast_get(arg0, "key"));
                    vm_chunk_emit(ctx->chunk, OP_HASH_EXISTS);
                } else if (atype == NI_DEREF_HASH) {
                    compile_expr(ctx, ast_get(arg0, "ref"));
                    compile_expr(ctx, ast_get(arg0, "key"));
                    vm_chunk_emit(ctx->chunk, OP_HASH_EXISTS);
                } else {
                    compile_expr(ctx, arg0);
                    vm_chunk_emit(ctx->chunk, OP_DEFINED);
                }
            } else {
                vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
                vm_chunk_emit_i64(ctx->chunk, 0);
            }
            break;
        }

        /* Built-in: delete() with 2 args: delete(\%hash, "key") */
        if (strcmp(name, "delete") == 0 && argc == 2) {
            compile_expr(ctx, ast_arr_get(args, 0));
            compile_expr(ctx, ast_arr_get(args, 1));
            vm_chunk_emit(ctx->chunk, OP_HASH_DELETE);
            vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
            break;
        }

        /* Built-in: delete() */
        if (strcmp(name, "delete") == 0 && argc == 1) {
            StradaValue *arg0 = ast_deref(ast_arr_get(args, 0));
            if (arg0 && !STRADA_IS_TAGGED_INT(arg0)) {
                int atype = (int)ast_int(arg0, "type");
                if (atype == NI_HASH_ACCESS) {
                    compile_expr(ctx, ast_get(arg0, "hash"));
                    compile_expr(ctx, ast_get(arg0, "key"));
                    vm_chunk_emit(ctx->chunk, OP_HASH_DELETE);
                } else if (atype == NI_DEREF_HASH) {
                    compile_expr(ctx, ast_get(arg0, "ref"));
                    compile_expr(ctx, ast_get(arg0, "key"));
                    vm_chunk_emit(ctx->chunk, OP_HASH_DELETE);
                }
            }
            vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
            break;
        }

        /* Built-in: split() */
        if (strcmp(name, "split") == 0 && (argc == 2 || argc == 3)) {
            if (argc == 3) {
                compile_expr(ctx, ast_arr_get(args, 2)); /* limit */
                compile_expr(ctx, ast_arr_get(args, 0)); /* delimiter */
                compile_expr(ctx, ast_arr_get(args, 1)); /* string */
                vm_chunk_emit(ctx->chunk, OP_STR_SPLIT_LIMIT);
            } else {
                compile_expr(ctx, ast_arr_get(args, 0));
                compile_expr(ctx, ast_arr_get(args, 1));
                vm_chunk_emit(ctx->chunk, OP_STR_SPLIT);
            }
            break;
        }

        /* Built-in: join() */
        if (strcmp(name, "join") == 0 && argc == 2) {
            compile_expr(ctx, ast_arr_get(args, 0));
            compile_expr(ctx, ast_arr_get(args, 1));
            vm_chunk_emit(ctx->chunk, OP_ARRAY_JOIN);
            break;
        }

        /* Built-in: bless() */
        if (strcmp(name, "bless") == 0 && argc == 2) {
            compile_expr(ctx, ast_arr_get(args, 0));
            compile_expr(ctx, ast_arr_get(args, 1));
            vm_chunk_emit(ctx->chunk, OP_HASH_BLESS);
            break;
        }

        /* Built-in: substr() */
        if (strcmp(name, "substr") == 0 && (argc == 2 || argc == 3)) {
            compile_expr(ctx, ast_arr_get(args, 0));
            compile_expr(ctx, ast_arr_get(args, 1));
            if (argc == 3) {
                compile_expr(ctx, ast_arr_get(args, 2));
            } else {
                /* substr with 2 args: rest of string */
                vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
                vm_chunk_emit_i64(ctx->chunk, 999999);
            }
            vm_chunk_emit(ctx->chunk, OP_SUBSTR);
            break;
        }

        /* Built-in: index() */
        if (strcmp(name, "index") == 0 && argc == 2) {
            compile_expr(ctx, ast_arr_get(args, 0));
            compile_expr(ctx, ast_arr_get(args, 1));
            vm_chunk_emit(ctx->chunk, OP_STR_INDEX);
            break;
        }

        /* Built-in: uc() / upper() */
        if ((strcmp(name, "uc") == 0 || strcmp(name, "upper") == 0) && argc == 1) {
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_STR_UPPER);
            break;
        }

        /* Built-in: lc() / lower() */
        if ((strcmp(name, "lc") == 0 || strcmp(name, "lower") == 0) && argc == 1) {
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_STR_LOWER);
            break;
        }

        /* Built-in: ucfirst() */
        if (strcmp(name, "ucfirst") == 0 && argc == 1) {
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_UCFIRST);
            vm_chunk_emit(ctx->chunk, 1);
            break;
        }

        /* Built-in: lcfirst() */
        if (strcmp(name, "lcfirst") == 0 && argc == 1) {
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_LCFIRST);
            vm_chunk_emit(ctx->chunk, 1);
            break;
        }

        /* Built-in: trim() */
        if (strcmp(name, "trim") == 0 && argc == 1) {
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_TRIM);
            vm_chunk_emit(ctx->chunk, 1);
            break;
        }

        /* Built-in: replace($str, $pat, $repl) */
        if (strcmp(name, "replace") == 0 && argc == 3) {
            for (int i = 0; i < 3; i++) compile_expr(ctx, ast_arr_get(args, i));
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_REPLACE);
            vm_chunk_emit(ctx->chunk, 3);
            break;
        }

        /* Built-in: replace_all($str, $find, $repl) */
        if (strcmp(name, "replace_all") == 0 && argc == 3) {
            for (int i = 0; i < 3; i++) compile_expr(ctx, ast_arr_get(args, i));
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_REPLACE_ALL);
            vm_chunk_emit(ctx->chunk, 3);
            break;
        }

        /* Built-in: match($str, $pat) */
        if (strcmp(name, "match") == 0 && argc == 2) {
            for (int i = 0; i < 2; i++) compile_expr(ctx, ast_arr_get(args, i));
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_MATCH);
            vm_chunk_emit(ctx->chunk, 2);
            break;
        }

        /* Built-in: sort(@arr) — default alpha sort, no block */
        if (strcmp(name, "sort") == 0 && argc == 1) {
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_SORT_DEFAULT);
            vm_chunk_emit(ctx->chunk, 1);
            break;
        }

        /* Built-in: hex() */
        if (strcmp(name, "hex") == 0 && argc == 1) {
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_HEX);
            vm_chunk_emit(ctx->chunk, 1);
            break;
        }

        /* Built-in: oct() */
        if (strcmp(name, "oct") == 0 && argc == 1) {
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_OCT);
            vm_chunk_emit(ctx->chunk, 1);
            break;
        }

        /* Built-in: chr() */
        if (strcmp(name, "chr") == 0 && argc == 1) {
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_CHR);
            break;
        }

        /* Built-in: ord() */
        if (strcmp(name, "ord") == 0 && argc == 1) {
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_ORD);
            break;
        }

        /* Built-in: char_at() */
        if (strcmp(name, "char_at") == 0 && argc == 2) {
            compile_expr(ctx, ast_arr_get(args, 0));
            compile_expr(ctx, ast_arr_get(args, 1));
            vm_chunk_emit(ctx->chunk, OP_CHAR_AT);
            break;
        }

        /* Built-in: bytes() */
        if (strcmp(name, "bytes") == 0 && argc == 1) {
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_BYTES);
            break;
        }

        /* Built-in: chomp() */
        if (strcmp(name, "chomp") == 0 && argc == 1) {
            /* Check if arg is a variable — if so, modify in place */
            StradaValue *arg0 = ast_deref(ast_arr_get(args, 0));
            if (arg0 && !STRADA_IS_TAGGED_INT(arg0) && (int)ast_int(arg0, "type") == NI_VARIABLE) {
                const char *vname = ast_str(arg0, "name");
                const char *vsigil = ast_str(arg0, "sigil");
                int slot = ctx_find_local_sigil(ctx, vname, vsigil[0]);
                if (slot >= 0) {
                    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                    vm_chunk_emit(ctx->chunk, OP_CHOMP);
                    vm_chunk_emit(ctx->chunk, OP_DUP);
                    vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                    break;
                }
            }
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_CHOMP);
            break;
        }

        /* Built-in: rindex() */
        if (strcmp(name, "rindex") == 0 && argc >= 2) {
            for (int i = 0; i < argc; i++) compile_expr(ctx, ast_arr_get(args, i));
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_STR_RINDEX);
            vm_chunk_emit(ctx->chunk, (uint8_t)argc);
            break;
        }

        /* Built-in: ltrim() */
        if (strcmp(name, "ltrim") == 0 && argc == 1) {
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_STR_LTRIM);
            vm_chunk_emit(ctx->chunk, 1);
            break;
        }

        /* Built-in: rtrim() */
        if (strcmp(name, "rtrim") == 0 && argc == 1) {
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_STR_RTRIM);
            vm_chunk_emit(ctx->chunk, 1);
            break;
        }

        /* Built-in: chop() */
        if (strcmp(name, "chop") == 0 && argc == 1) {
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_STR_CHOP);
            vm_chunk_emit(ctx->chunk, 1);
            break;
        }

        /* Built-in: reverse() — string reverse */
        if (strcmp(name, "reverse") == 0 && argc == 1) {
            /* Check if arg is array — use ARRAY_REVERSE, else string reverse */
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_STR_REVERSE);
            vm_chunk_emit(ctx->chunk, 1);
            break;
        }

        /* Built-in: values() */
        if (strcmp(name, "values") == 0 && argc == 1) {
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_HASH_VALUES);
            vm_chunk_emit(ctx->chunk, 1);
            break;
        }

        /* Built-in: splice() */
        if (strcmp(name, "splice") == 0 && argc >= 2) {
            for (int i = 0; i < argc; i++) compile_expr(ctx, ast_arr_get(args, i));
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_ARRAY_SPLICE);
            vm_chunk_emit(ctx->chunk, (uint8_t)argc);
            break;
        }

        /* Built-in: each() */
        if (strcmp(name, "each") == 0 && argc == 1) {
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_HASH_EACH);
            vm_chunk_emit(ctx->chunk, 1);
            break;
        }

        /* Built-in: sprintf() */
        if (strcmp(name, "sprintf") == 0) {
            /* OP_SPRINTF's VM handler pops into a fixed VMValue args[32];
             * reject (rather than silently emit an out-of-range argc) so the
             * bytecode can never drive that buffer past its bound. */
            if (argc > 32) {
                fprintf(stderr, "VM compile error: sprintf() with %d arguments exceeds the bytecode VM limit of 32\n", argc);
                exit(1);
            }
            for (int i = 0; i < argc; i++) {
                compile_expr(ctx, ast_arr_get(args, i));
            }
            vm_chunk_emit(ctx->chunk, OP_SPRINTF);
            vm_chunk_emit(ctx->chunk, (uint8_t)argc);
            break;
        }

        /* Built-in: abs() */
        if (strcmp(name, "abs") == 0 && argc == 1) {
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_ABS);
            break;
        }

        /* Built-in: defined() */
        if (strcmp(name, "defined") == 0 && argc == 1) {
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_DEFINED);
            break;
        }

        /* Built-in: ref() */
        if (strcmp(name, "ref") == 0 && argc == 1) {
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_REF_TYPE);
            break;
        }

        /* Built-in: select() */
        if (strcmp(name, "select") == 0 && argc == 1) {
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_SELECT);
            vm_chunk_emit(ctx->chunk, 1);
            break;
        }

        /* Built-in: throw() */
        if (strcmp(name, "throw") == 0 && argc == 1) {
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_THROW);
            vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
            break;
        }

        /* core:: and sys:: builtins */
        if (strncmp(name, "core::", 6) == 0 || strncmp(name, "sys::", 5) == 0) {
            const char *fn = strncmp(name, "core::", 6) == 0 ? name + 6 : name + 5;

            int bid = -1;
            if (strcmp(fn, "open") == 0) bid = BUILTIN_CORE_OPEN;
            else if (strcmp(fn, "open_str") == 0) bid = BUILTIN_CORE_OPEN_STR;
            else if (strcmp(fn, "str_from_fh") == 0) bid = BUILTIN_CORE_STR_FROM_FH;
            else if (strcmp(fn, "close") == 0) bid = BUILTIN_CORE_CLOSE;
            else if (strcmp(fn, "readline") == 0) bid = BUILTIN_CORE_READLINE;
            else if (strcmp(fn, "eof") == 0) bid = BUILTIN_CORE_EOF;
            else if (strcmp(fn, "seek") == 0) bid = BUILTIN_CORE_SEEK;
            else if (strcmp(fn, "tell") == 0) bid = BUILTIN_CORE_TELL;
            else if (strcmp(fn, "rewind") == 0) bid = BUILTIN_CORE_REWIND;
            else if (strcmp(fn, "flush") == 0) bid = BUILTIN_CORE_FLUSH;
            else if (strcmp(fn, "slurp") == 0) bid = BUILTIN_CORE_SLURP;
            else if (strcmp(fn, "spew") == 0) bid = BUILTIN_CORE_SPEW;
            else if (strcmp(fn, "qx") == 0) bid = BUILTIN_CORE_QX;
            else if (strcmp(fn, "system") == 0) bid = BUILTIN_CORE_SYSTEM;
            else if (strcmp(fn, "getenv") == 0) bid = BUILTIN_CORE_GETENV;
            else if (strcmp(fn, "setenv") == 0) bid = BUILTIN_CORE_SETENV;
            else if (strcmp(fn, "time") == 0) bid = BUILTIN_CORE_TIME;
            else if (strcmp(fn, "hires_time") == 0) bid = BUILTIN_CORE_HIRES_TIME;
            else if (strcmp(fn, "global_set") == 0) bid = BUILTIN_CORE_GLOBAL_SET;
            else if (strcmp(fn, "global_get") == 0) bid = BUILTIN_CORE_GLOBAL_GET;
            else if (strcmp(fn, "global_exists") == 0) bid = BUILTIN_CORE_GLOBAL_EXISTS;
            else if (strcmp(fn, "global_delete") == 0) bid = BUILTIN_CORE_GLOBAL_DELETE;
            else if (strcmp(fn, "global_keys") == 0) bid = BUILTIN_CORE_GLOBAL_KEYS;
            else if (strcmp(fn, "set_recursion_limit") == 0) bid = BUILTIN_CORE_SET_RECURSION_LIMIT;
            else if (strcmp(fn, "get_recursion_limit") == 0) bid = BUILTIN_CORE_GET_RECURSION_LIMIT;
            else if (strcmp(fn, "file_exists") == 0) bid = BUILTIN_CORE_FILE_EXISTS;
            else if (strcmp(fn, "is_dir") == 0) bid = BUILTIN_FILE_TEST_D;
            else if (strcmp(fn, "is_file") == 0) bid = BUILTIN_FILE_TEST_F;
            else if (strcmp(fn, "unlink") == 0) bid = BUILTIN_CORE_UNLINK;
            else if (strcmp(fn, "exit") == 0) bid = BUILTIN_CORE_EXIT;
            else if (strcmp(fn, "sleep") == 0) bid = BUILTIN_CORE_SLEEP;
            else if (strcmp(fn, "usleep") == 0) bid = BUILTIN_CORE_USLEEP;
            else if (strcmp(fn, "getpid") == 0) bid = BUILTIN_CORE_GETPID;
            else if (strcmp(fn, "getppid") == 0) bid = BUILTIN_CORE_GETPPID;
            else if (strcmp(fn, "chdir") == 0) bid = BUILTIN_CORE_CHDIR;
            else if (strcmp(fn, "getcwd") == 0) bid = BUILTIN_CORE_GETCWD;
            else if (strcmp(fn, "mkdir") == 0) bid = BUILTIN_CORE_MKDIR;
            else if (strcmp(fn, "rmdir") == 0) bid = BUILTIN_CORE_RMDIR;
            else if (strcmp(fn, "rename") == 0) bid = BUILTIN_CORE_RENAME;
            else if (strcmp(fn, "dirname") == 0) bid = BUILTIN_CORE_DIRNAME;
            else if (strcmp(fn, "basename") == 0) bid = BUILTIN_CORE_BASENAME;
            else if (strcmp(fn, "stat") == 0) bid = BUILTIN_CORE_STAT;
            else if (strcmp(fn, "lstat") == 0) bid = BUILTIN_CORE_LSTAT;
            else if (strcmp(fn, "chmod") == 0) bid = BUILTIN_CORE_CHMOD;
            else if (strcmp(fn, "readdir") == 0) bid = BUILTIN_CORE_READDIR;
            else if (strcmp(fn, "popen") == 0) bid = BUILTIN_CORE_POPEN;
            else if (strcmp(fn, "pclose") == 0) bid = BUILTIN_CORE_PCLOSE;
            else if (strcmp(fn, "errno") == 0) bid = BUILTIN_CORE_ERRNO;
            else if (strcmp(fn, "strerror") == 0) bid = BUILTIN_CORE_STRERROR;
            else if (strcmp(fn, "isatty") == 0) bid = BUILTIN_CORE_ISATTY;
            else if (strcmp(fn, "argv") == 0) bid = BUILTIN_CORE_ARGV;
            else if (strcmp(fn, "realpath") == 0) bid = BUILTIN_CORE_REALPATH;
            /* link: handled by the generic runtime bridge (raw syscall return,
             * matching compiled semantics — the old BUILTIN_CORE_LINK used 1/0) */
            /* symlink: handled by the generic runtime bridge (raw syscall return,
             * matching compiled semantics — the old BUILTIN_CORE_SYMLINK used 1/0) */
            else if (strcmp(fn, "readlink") == 0) bid = BUILTIN_CORE_READLINK;
            else if (strcmp(fn, "fork") == 0) bid = BUILTIN_CORE_FORK;
            else if (strcmp(fn, "wait") == 0) bid = BUILTIN_CORE_WAIT;
            else if (strcmp(fn, "waitpid") == 0) bid = BUILTIN_CORE_WAITPID;
            else if (strcmp(fn, "kill") == 0) bid = BUILTIN_CORE_KILL;
            else if (strcmp(fn, "signal") == 0) bid = BUILTIN_CORE_SIGNAL;
            else if (strcmp(fn, "alarm") == 0) bid = BUILTIN_CORE_ALARM;
            else if (strcmp(fn, "pipe") == 0) bid = BUILTIN_CORE_PIPE;
            else if (strcmp(fn, "dup2") == 0) bid = BUILTIN_CORE_DUP2;
            else if (strcmp(fn, "ord_byte") == 0) bid = BUILTIN_CORE_ORD_BYTE;
            else if (strcmp(fn, "byte") == 0) bid = BUILTIN_CORE_BYTE;
            else if (strcmp(fn, "get_byte") == 0) bid = BUILTIN_CORE_GET_BYTE;
            else if (strcmp(fn, "byte_length") == 0) bid = BUILTIN_CORE_BYTE_LENGTH;
            else if (strcmp(fn, "byte_index") == 0) bid = BUILTIN_BYTE_INDEX;
            else if (strcmp(fn, "caller") == 0) bid = BUILTIN_CORE_CALLER;
            else if (strcmp(fn, "stack_trace") == 0) bid = BUILTIN_CORE_STACK_TRACE;
            else if (strcmp(fn, "pack") == 0) bid = BUILTIN_CORE_PACK;
            else if (strcmp(fn, "unpack") == 0) bid = BUILTIN_CORE_UNPACK;
            else if (strcmp(fn, "base64_encode") == 0) bid = BUILTIN_CORE_BASE64_ENCODE;
            else if (strcmp(fn, "base64_decode") == 0) bid = BUILTIN_CORE_BASE64_DECODE;
            else if (strcmp(fn, "quotemeta") == 0) bid = BUILTIN_CORE_QUOTEMETA;
            else if (strcmp(fn, "fnmatch") == 0) bid = BUILTIN_CORE_FNMATCH;
            else if (strcmp(fn, "file_ext") == 0) bid = BUILTIN_CORE_FILE_EXT;
            else if (strcmp(fn, "path_join") == 0) bid = BUILTIN_CORE_PATH_JOIN;
            else if (strcmp(fn, "getuid") == 0) bid = BUILTIN_CORE_GETUID;
            else if (strcmp(fn, "geteuid") == 0) bid = BUILTIN_CORE_GETEUID;
            else if (strcmp(fn, "getgid") == 0) bid = BUILTIN_CORE_GETGID;
            else if (strcmp(fn, "getegid") == 0) bid = BUILTIN_CORE_GETEGID;
            else if (strcmp(fn, "umask") == 0) bid = BUILTIN_CORE_UMASK;
            else if (strcmp(fn, "exec") == 0) bid = BUILTIN_CORE_EXEC;
            else if (strcmp(fn, "srand") == 0) bid = BUILTIN_CORE_SRAND;
            else if (strcmp(fn, "rand") == 0) bid = BUILTIN_CORE_RAND;
            /* truncate: handled by the generic runtime bridge (raw syscall return,
             * matching compiled semantics — the old BUILTIN_CORE_TRUNCATE used 1/0) */
            else if (strcmp(fn, "nanosleep") == 0) bid = BUILTIN_CORE_NANOSLEEP;
            else if (strcmp(fn, "gethostname") == 0) bid = BUILTIN_CORE_GETHOSTNAME;
            else if (strcmp(fn, "access") == 0) bid = BUILTIN_CORE_ACCESS;
            else if (strcmp(fn, "glob") == 0) bid = BUILTIN_CORE_GLOB;
            /* Socket */
            else if (strcmp(fn, "socket_client") == 0) bid = BUILTIN_CORE_SOCKET_CLIENT;
            else if (strcmp(fn, "socket_server") == 0) bid = BUILTIN_CORE_SOCKET_SERVER;
            else if (strcmp(fn, "socket_server_backlog") == 0) bid = BUILTIN_CORE_SOCKET_SERVER_BACKLOG;
            else if (strcmp(fn, "socket_accept") == 0) bid = BUILTIN_CORE_SOCKET_ACCEPT;
            else if (strcmp(fn, "socket_recv") == 0) bid = BUILTIN_CORE_SOCKET_RECV;
            else if (strcmp(fn, "socket_send") == 0) bid = BUILTIN_CORE_SOCKET_SEND;
            else if (strcmp(fn, "socket_close") == 0) bid = BUILTIN_CORE_SOCKET_CLOSE;
            else if (strcmp(fn, "socket_flush") == 0) bid = BUILTIN_CORE_SOCKET_FLUSH;
            else if (strcmp(fn, "socket_select") == 0) bid = BUILTIN_CORE_SOCKET_SELECT;
            else if (strcmp(fn, "socket_fd") == 0) bid = BUILTIN_CORE_SOCKET_FD;
            else if (strcmp(fn, "socket_set_nonblocking") == 0) bid = BUILTIN_CORE_SOCKET_SET_NONBLOCKING;
            else if (strcmp(fn, "udp_socket") == 0) bid = BUILTIN_CORE_UDP_SOCKET;
            else if (strcmp(fn, "udp_bind") == 0) bid = BUILTIN_CORE_UDP_BIND;
            else if (strcmp(fn, "udp_server") == 0) bid = BUILTIN_CORE_UDP_SERVER;
            else if (strcmp(fn, "udp_recvfrom") == 0) bid = BUILTIN_CORE_UDP_RECVFROM;
            else if (strcmp(fn, "udp_sendto") == 0) bid = BUILTIN_CORE_UDP_SENDTO;
            /* Terminal */
            else if (strcmp(fn, "term_enable_raw") == 0) bid = BUILTIN_CORE_TERM_ENABLE_RAW;
            else if (strcmp(fn, "term_disable_raw") == 0) bid = BUILTIN_CORE_TERM_DISABLE_RAW;
            else if (strcmp(fn, "term_rows") == 0) bid = BUILTIN_CORE_TERM_ROWS;
            else if (strcmp(fn, "term_cols") == 0) bid = BUILTIN_CORE_TERM_COLS;
            else if (strcmp(fn, "read_byte") == 0) bid = BUILTIN_CORE_READ_BYTE;
            else if (strcmp(fn, "ttyname") == 0) bid = BUILTIN_CORE_TTYNAME;
            /* Dynamic loading */
            else if (strcmp(fn, "dl_open") == 0) bid = BUILTIN_CORE_DL_OPEN;
            else if (strcmp(fn, "dl_sym") == 0) bid = BUILTIN_CORE_DL_SYM;
            else if (strcmp(fn, "dl_close") == 0) bid = BUILTIN_CORE_DL_CLOSE;
            else if (strcmp(fn, "dl_error") == 0) bid = BUILTIN_CORE_DL_ERROR;
            else if (strcmp(fn, "dl_call_int") == 0) bid = BUILTIN_CORE_DL_CALL_INT;
            else if (strcmp(fn, "dl_call_str") == 0) bid = BUILTIN_CORE_DL_CALL_STR;
            else if (strcmp(fn, "dl_call_void") == 0) bid = BUILTIN_CORE_DL_CALL_VOID;
            else if (strcmp(fn, "dl_call_int_sv") == 0) bid = BUILTIN_CORE_DL_CALL_INT_SV;
            else if (strcmp(fn, "dl_call_str_sv") == 0) bid = BUILTIN_CORE_DL_CALL_STR_SV;
            else if (strcmp(fn, "dl_call_void_sv") == 0) bid = BUILTIN_CORE_DL_CALL_VOID_SV;
            /* CStruct */
            else if (strcmp(fn, "cstruct_new") == 0) bid = BUILTIN_CORE_CSTRUCT_NEW;
            else if (strcmp(fn, "cstruct_ptr") == 0) bid = BUILTIN_CORE_CSTRUCT_PTR;
            else if (strcmp(fn, "cstruct_set_int") == 0) bid = BUILTIN_CORE_CSTRUCT_SET_INT;
            else if (strcmp(fn, "cstruct_get_int") == 0) bid = BUILTIN_CORE_CSTRUCT_GET_INT;
            else if (strcmp(fn, "cstruct_set_string") == 0) bid = BUILTIN_CORE_CSTRUCT_SET_STRING;
            else if (strcmp(fn, "cstruct_get_string") == 0) bid = BUILTIN_CORE_CSTRUCT_GET_STRING;
            else if (strcmp(fn, "cstruct_set_double") == 0) bid = BUILTIN_CORE_CSTRUCT_SET_DOUBLE;
            else if (strcmp(fn, "cstruct_get_double") == 0) bid = BUILTIN_CORE_CSTRUCT_GET_DOUBLE;
            /* OOP */
            else if (strcmp(fn, "weaken") == 0) bid = BUILTIN_CORE_WEAKEN;
            else if (strcmp(fn, "isweak") == 0) bid = BUILTIN_CORE_ISWEAK;
            /* Misc */
            else if (strcmp(fn, "wantarray") == 0) bid = BUILTIN_CORE_WANTARRAY;
            else if (strcmp(fn, "select_fds") == 0) bid = BUILTIN_CORE_SELECT_FDS;
            else if (strcmp(fn, "set_byte") == 0) bid = BUILTIN_CORE_SET_BYTE;
            else if (strcmp(fn, "byte_substr") == 0) bid = BUILTIN_CORE_BYTE_SUBSTR;

            if (bid >= 0) {
                for (int i = 0; i < argc; i++) {
                    compile_expr(ctx, ast_arr_get(args, i));
                }
                vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)bid);
                vm_chunk_emit(ctx->chunk, (uint8_t)argc);
                break;
            }
        }

        /* math:: builtins */
        if (strncmp(name, "math::", 6) == 0) {
            const char *fn = name + 6;
            int bid = -1;
            if (strcmp(fn, "sqrt") == 0) bid = BUILTIN_MATH_SQRT;
            else if (strcmp(fn, "floor") == 0) bid = BUILTIN_MATH_FLOOR;
            else if (strcmp(fn, "ceil") == 0) bid = BUILTIN_MATH_CEIL;
            else if (strcmp(fn, "pow") == 0) bid = BUILTIN_MATH_POW;
            else if (strcmp(fn, "abs") == 0) bid = BUILTIN_MATH_ABS;
            else if (strcmp(fn, "sin") == 0) bid = BUILTIN_MATH_SIN;
            else if (strcmp(fn, "cos") == 0) bid = BUILTIN_MATH_COS;
            else if (strcmp(fn, "tan") == 0) bid = BUILTIN_MATH_TAN;
            else if (strcmp(fn, "asin") == 0) bid = BUILTIN_MATH_ASIN;
            else if (strcmp(fn, "acos") == 0) bid = BUILTIN_MATH_ACOS;
            else if (strcmp(fn, "atan") == 0) bid = BUILTIN_MATH_ATAN;
            else if (strcmp(fn, "atan2") == 0) bid = BUILTIN_MATH_ATAN2;
            else if (strcmp(fn, "sinh") == 0) bid = BUILTIN_MATH_SINH;
            else if (strcmp(fn, "cosh") == 0) bid = BUILTIN_MATH_COSH;
            else if (strcmp(fn, "tanh") == 0) bid = BUILTIN_MATH_TANH;
            else if (strcmp(fn, "log") == 0) bid = BUILTIN_MATH_LOG;
            else if (strcmp(fn, "log10") == 0) bid = BUILTIN_MATH_LOG10;
            else if (strcmp(fn, "exp") == 0) bid = BUILTIN_MATH_EXP;
            else if (strcmp(fn, "round") == 0) bid = BUILTIN_MATH_ROUND;
            else if (strcmp(fn, "fmod") == 0) bid = BUILTIN_MATH_FMOD;
            else if (strcmp(fn, "fabs") == 0) bid = BUILTIN_MATH_FABS;
            else if (strcmp(fn, "rand") == 0) bid = BUILTIN_MATH_RAND;
            else if (strcmp(fn, "srand") == 0) bid = BUILTIN_MATH_SRAND;
            else if (strcmp(fn, "hypot") == 0) bid = BUILTIN_MATH_HYPOT;
            else if (strcmp(fn, "cbrt") == 0) bid = BUILTIN_MATH_CBRT;
            else if (strcmp(fn, "isnan") == 0) bid = BUILTIN_MATH_ISNAN;
            else if (strcmp(fn, "isinf") == 0) bid = BUILTIN_MATH_ISINF;
            else if (strcmp(fn, "isfinite") == 0) bid = BUILTIN_MATH_ISFINITE;

            if (bid >= 0) {
                for (int i = 0; i < argc; i++) {
                    compile_expr(ctx, ast_arr_get(args, i));
                }
                vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)bid);
                vm_chunk_emit(ctx->chunk, (uint8_t)argc);
                break;
            }
        }

        /* c:: builtins */
        if (strncmp(name, "c::", 3) == 0) {
            const char *fn = name + 3;
            int bid = -1;
            if (strcmp(fn, "str_to_ptr") == 0) bid = BUILTIN_C_STR_TO_PTR;
            else if (strcmp(fn, "ptr_to_str") == 0) bid = BUILTIN_C_PTR_TO_STR;
            else if (strcmp(fn, "ptr_to_str_n") == 0) bid = BUILTIN_C_PTR_TO_STR_N;
            else if (strcmp(fn, "alloc") == 0) bid = BUILTIN_C_ALLOC;
            else if (strcmp(fn, "free") == 0) bid = BUILTIN_C_FREE;
            else if (strcmp(fn, "realloc") == 0) bid = BUILTIN_C_REALLOC;
            else if (strcmp(fn, "null") == 0) bid = BUILTIN_C_NULL;
            else if (strcmp(fn, "is_null") == 0) bid = BUILTIN_C_IS_NULL;
            else if (strcmp(fn, "ptr_add") == 0) bid = BUILTIN_C_PTR_ADD;
            else if (strcmp(fn, "read_int8") == 0) bid = BUILTIN_C_READ_INT8;
            else if (strcmp(fn, "read_int16") == 0) bid = BUILTIN_C_READ_INT16;
            else if (strcmp(fn, "read_int32") == 0) bid = BUILTIN_C_READ_INT32;
            else if (strcmp(fn, "read_int64") == 0) bid = BUILTIN_C_READ_INT64;
            else if (strcmp(fn, "read_ptr") == 0) bid = BUILTIN_C_READ_PTR;
            else if (strcmp(fn, "read_float") == 0) bid = BUILTIN_C_READ_FLOAT;
            else if (strcmp(fn, "read_double") == 0) bid = BUILTIN_C_READ_DOUBLE;
            else if (strcmp(fn, "write_int8") == 0) bid = BUILTIN_C_WRITE_INT8;
            else if (strcmp(fn, "write_int16") == 0) bid = BUILTIN_C_WRITE_INT16;
            else if (strcmp(fn, "write_int32") == 0) bid = BUILTIN_C_WRITE_INT32;
            else if (strcmp(fn, "write_int64") == 0) bid = BUILTIN_C_WRITE_INT64;
            else if (strcmp(fn, "write_ptr") == 0) bid = BUILTIN_C_WRITE_PTR;
            else if (strcmp(fn, "write_float") == 0) bid = BUILTIN_C_WRITE_FLOAT;
            else if (strcmp(fn, "write_double") == 0) bid = BUILTIN_C_WRITE_DOUBLE;
            else if (strcmp(fn, "sizeof_int") == 0) bid = BUILTIN_C_SIZEOF_INT;
            else if (strcmp(fn, "sizeof_long") == 0) bid = BUILTIN_C_SIZEOF_LONG;
            else if (strcmp(fn, "sizeof_ptr") == 0) bid = BUILTIN_C_SIZEOF_PTR;
            else if (strcmp(fn, "sizeof_size_t") == 0) bid = BUILTIN_C_SIZEOF_SIZE_T;
            else if (strcmp(fn, "memcpy") == 0) bid = BUILTIN_C_MEMCPY;
            else if (strcmp(fn, "memset") == 0) bid = BUILTIN_C_MEMSET;
            if (bid >= 0) {
                for (int i = 0; i < argc; i++) compile_expr(ctx, ast_arr_get(args, i));
                vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)bid);
                vm_chunk_emit(ctx->chunk, (uint8_t)argc);
                break;
            }
        }

        /* async:: builtins */
        if (strncmp(name, "async::", 7) == 0) {
            const char *fn = name + 7;
            int bid = -1;
            if (strcmp(fn, "channel") == 0) bid = BUILTIN_ASYNC_CHANNEL;
            else if (strcmp(fn, "send") == 0) bid = BUILTIN_ASYNC_SEND;
            else if (strcmp(fn, "recv") == 0) bid = BUILTIN_ASYNC_RECV;
            else if (strcmp(fn, "try_send") == 0) bid = BUILTIN_ASYNC_TRY_SEND;
            else if (strcmp(fn, "try_recv") == 0) bid = BUILTIN_ASYNC_TRY_RECV;
            else if (strcmp(fn, "close") == 0) bid = BUILTIN_ASYNC_CLOSE;
            else if (strcmp(fn, "is_closed") == 0) bid = BUILTIN_ASYNC_IS_CLOSED;
            else if (strcmp(fn, "len") == 0) bid = BUILTIN_ASYNC_LEN;
            else if (strcmp(fn, "mutex") == 0) bid = BUILTIN_ASYNC_MUTEX;
            else if (strcmp(fn, "lock") == 0) bid = BUILTIN_ASYNC_LOCK;
            else if (strcmp(fn, "unlock") == 0) bid = BUILTIN_ASYNC_UNLOCK;
            else if (strcmp(fn, "try_lock") == 0) bid = BUILTIN_ASYNC_TRY_LOCK;
            else if (strcmp(fn, "mutex_destroy") == 0) bid = BUILTIN_ASYNC_MUTEX_DESTROY;
            else if (strcmp(fn, "atomic") == 0) bid = BUILTIN_ASYNC_ATOMIC;
            else if (strcmp(fn, "atomic_load") == 0) bid = BUILTIN_ASYNC_ATOMIC_LOAD;
            else if (strcmp(fn, "atomic_store") == 0) bid = BUILTIN_ASYNC_ATOMIC_STORE;
            else if (strcmp(fn, "atomic_add") == 0) bid = BUILTIN_ASYNC_ATOMIC_ADD;
            else if (strcmp(fn, "atomic_sub") == 0) bid = BUILTIN_ASYNC_ATOMIC_SUB;
            else if (strcmp(fn, "atomic_inc") == 0) bid = BUILTIN_ASYNC_ATOMIC_INC;
            else if (strcmp(fn, "atomic_dec") == 0) bid = BUILTIN_ASYNC_ATOMIC_DEC;
            else if (strcmp(fn, "atomic_cas") == 0) bid = BUILTIN_ASYNC_ATOMIC_CAS;
            if (bid >= 0) {
                for (int i = 0; i < argc; i++) compile_expr(ctx, ast_arr_get(args, i));
                vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)bid);
                vm_chunk_emit(ctx->chunk, (uint8_t)argc);
                break;
            }
        }

        /* thread:: builtins */
        if (strncmp(name, "thread::", 8) == 0) {
            const char *fn = name + 8;
            int bid = -1;
            if (strcmp(fn, "create") == 0) bid = BUILTIN_THREAD_CREATE;
            else if (strcmp(fn, "join") == 0) bid = BUILTIN_THREAD_JOIN;
            else if (strcmp(fn, "detach") == 0) bid = BUILTIN_THREAD_DETACH;
            else if (strcmp(fn, "self") == 0) bid = BUILTIN_THREAD_SELF;
            if (bid >= 0) {
                for (int i = 0; i < argc; i++) compile_expr(ctx, ast_arr_get(args, i));
                vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)bid);
                vm_chunk_emit(ctx->chunk, (uint8_t)argc);
                break;
            }
        }

        /* utf8:: builtins */
        if (strncmp(name, "utf8::", 6) == 0) {
            const char *fn = name + 6;
            int bid = -1;
            if (strcmp(fn, "is_utf8") == 0) bid = BUILTIN_UTF8_IS_UTF8;
            else if (strcmp(fn, "valid") == 0) bid = BUILTIN_UTF8_VALID;
            else if (strcmp(fn, "encode") == 0) bid = BUILTIN_UTF8_ENCODE;
            else if (strcmp(fn, "decode") == 0) bid = BUILTIN_UTF8_DECODE;
            else if (strcmp(fn, "downgrade") == 0) bid = BUILTIN_UTF8_DOWNGRADE;
            else if (strcmp(fn, "upgrade") == 0) bid = BUILTIN_UTF8_UPGRADE;
            else if (strcmp(fn, "unicode_to_native") == 0) bid = BUILTIN_UTF8_UNICODE_TO_NATIVE;
            if (bid >= 0) {
                for (int i = 0; i < argc; i++) compile_expr(ctx, ast_arr_get(args, i));
                vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)bid);
                vm_chunk_emit(ctx->chunk, (uint8_t)argc);
                break;
            }
        }

        /* Bare function builtins: sb_*, str_replace, regex_replace, tie/untie/tied */
        if (strcmp(name, "sb_new") == 0) { vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_SB_NEW); vm_chunk_emit(ctx->chunk, 0); break; }
        if (strcmp(name, "sb_new_cap") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_SB_NEW_CAP); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "sb_append") == 0 && argc == 2) { compile_expr(ctx, ast_arr_get(args, 0)); compile_expr(ctx, ast_arr_get(args, 1)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_SB_APPEND); vm_chunk_emit(ctx->chunk, 2); break; }
        if (strcmp(name, "sb_to_string") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_SB_TO_STRING); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "sb_length") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_SB_LENGTH); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "sb_clear") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_SB_CLEAR); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "sb_free") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_SB_FREE); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "str_replace") == 0 && argc == 3) { for (int i = 0; i < 3; i++) compile_expr(ctx, ast_arr_get(args, i)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_STR_REPLACE_PLAIN); vm_chunk_emit(ctx->chunk, 3); break; }
        if (strcmp(name, "str_replace_first") == 0 && argc == 3) { for (int i = 0; i < 3; i++) compile_expr(ctx, ast_arr_get(args, i)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_STR_REPLACE_FIRST); vm_chunk_emit(ctx->chunk, 3); break; }
        if (strcmp(name, "regex_replace") == 0 && argc == 3) { for (int i = 0; i < 3; i++) compile_expr(ctx, ast_arr_get(args, i)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_REGEX_REPLACE); vm_chunk_emit(ctx->chunk, 3); break; }
        if (strcmp(name, "regex_replace_all") == 0 && argc == 3) { for (int i = 0; i < 3; i++) compile_expr(ctx, ast_arr_get(args, i)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_REGEX_REPLACE_ALL); vm_chunk_emit(ctx->chunk, 3); break; }
        if (strcmp(name, "die") == 0) { for (int i = 0; i < argc; i++) compile_expr(ctx, ast_arr_get(args, i)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_DIE); vm_chunk_emit(ctx->chunk, (uint8_t)argc); break; }
        if (strcmp(name, "warn") == 0) { for (int i = 0; i < argc; i++) compile_expr(ctx, ast_arr_get(args, i)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_WARN); vm_chunk_emit(ctx->chunk, (uint8_t)argc); break; }
        if (strcmp(name, "named_captures") == 0 && argc == 0) { vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_NAMED_CAPTURES); vm_chunk_emit(ctx->chunk, 0); break; }
        if (strcmp(name, "captures") == 0 && argc == 0) { vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_CAPTURES); vm_chunk_emit(ctx->chunk, 0); break; }
        if (strcmp(name, "inherit") == 0 && argc == 2) { compile_expr(ctx, ast_arr_get(args, 0)); compile_expr(ctx, ast_arr_get(args, 1)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_INHERIT); vm_chunk_emit(ctx->chunk, 2); break; }
        if (strcmp(name, "blessed") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_BLESSED); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "refcount") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_REFCOUNT); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "typeof") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_TYPEOF); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "dumper_str") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_DUMPER_STR); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "nsort") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_NSORT); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "cast_int") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_CAST_INT); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "cast_num") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_CAST_NUM); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "cast_str") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_CAST_STR); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "byte_index") == 0 && (argc == 2 || argc == 3)) { for (int i = 0; i < argc; i++) compile_expr(ctx, ast_arr_get(args, i)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_BYTE_INDEX); vm_chunk_emit(ctx->chunk, (uint8_t)argc); break; }
        if (strcmp(name, "tie") == 0 && argc >= 2) { for (int i = 0; i < argc; i++) compile_expr(ctx, ast_arr_get(args, i)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_TIE); vm_chunk_emit(ctx->chunk, (uint8_t)argc); break; }
        if (strcmp(name, "untie") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_UNTIE); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "tied") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_TIED); vm_chunk_emit(ctx->chunk, 1); break; }

        /* Function-form isa()/can() — same opcodes as the method forms */
        if ((strcmp(name, "isa") == 0 || strcmp(name, "UNIVERSAL::isa") == 0) && argc == 2) {
            compile_expr(ctx, ast_arr_get(args, 0));
            compile_expr(ctx, ast_arr_get(args, 1));
            vm_chunk_emit(ctx->chunk, OP_ISA);
            break;
        }
        if ((strcmp(name, "can") == 0 || strcmp(name, "UNIVERSAL::can") == 0) && argc == 2) {
            compile_expr(ctx, ast_arr_get(args, 0));
            compile_expr(ctx, ast_arr_get(args, 1));
            vm_chunk_emit(ctx->chunk, OP_CAN);
            break;
        }

        /* Generic runtime-bridged builtins: any sys::/math:: name not handled
         * by a dedicated opcode above falls through to the harvested table
         * (vm_generic_builtins.inc) and calls the Strada runtime directly.
         * Same precedence as the compiler: builtin namespaces win over user
         * functions. Arity must match the runtime prototype exactly. */
        if (strchr(name, ':')) {
            char gbuf[256];
            const char *gname = name;
            if (strncmp(name, "core::", 6) == 0) {
                snprintf(gbuf, sizeof(gbuf), "sys::%s", name + 6);
                gname = gbuf;
            }
            int gidx = vm_generic_builtin_find(gname, argc);
            if (gidx >= 0) {
                for (int i = 0; i < argc; i++) {
                    compile_expr(ctx, ast_arr_get(args, i));
                }
                vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)(VM_GENERIC_BID_BASE + gidx));
                vm_chunk_emit(ctx->chunk, (uint8_t)argc);
                break;
            }
        }

        /* User function call */
        int fidx = vm_program_find_func(ctx->prog, name);
        /* Unqualified call inside a non-main package: try <pkg>_<name> and
         * <pkg>::<name> — same fallback the compiled backend's gen_call uses
         * (see the name-resolution unification rule in CLAUDE.md). */
        if (fidx < 0 && !strchr(name, ':') && ctx->cur_package && ctx->cur_package[0] &&
            strcmp(ctx->cur_package, "main") != 0) {
            char pkgname[256];
            snprintf(pkgname, sizeof(pkgname), "%s_%s", ctx->cur_package, name);
            fidx = vm_program_find_func(ctx->prog, pkgname);
            if (fidx < 0) {
                snprintf(pkgname, sizeof(pkgname), "%s::%s", ctx->cur_package, name);
                fidx = vm_program_find_func(ctx->prog, pkgname);
            }
        }
        char normalized[256];
        if (fidx < 0 && strchr(name, ':')) {
            size_t j = 0;
            for (size_t k = 0; name[k] && j < sizeof(normalized) - 1; k++) {
                if (name[k] == ':' && name[k+1] == ':') {
                    normalized[j++] = '_';
                    k++;
                } else {
                    normalized[j++] = name[k];
                }
            }
            normalized[j] = '\0';
            fidx = vm_program_find_func(ctx->prog, normalized);
        }
        if (fidx < 0) {
            /* Check for native (import_lib) functions */
            VMNativeEntry *native = vm_find_native(ctx->prog, name);
            if (!native && strchr(name, ':')) {
                native = vm_find_native(ctx->prog, normalized);
            }
            if (native) {
                /* Emit native call */
                int native_idx = (int)(native - ctx->prog->natives);
                for (int i = 0; i < argc; i++) {
                    compile_expr(ctx, ast_arr_get(args, i));
                }
                vm_chunk_emit(ctx->chunk, OP_CALL_NATIVE);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)native_idx);
                vm_chunk_emit(ctx->chunk, (uint8_t)argc);
                break;
            }
            /* Unknown function — push undef */
            vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
            break;
        }

        VMChunk *callee = &ctx->prog->funcs[fidx];
        int need_spread = 0;
        for (int i = 0; i < argc; i++) {
            StradaValue *arg = ast_deref(ast_arr_get(args, i));
            int atype = arg ? (int)ast_int(arg, "type") : 0;
            if (atype == NI_SPREAD) {
                need_spread = 1;
                break;
            }
            if (callee->has_variadic && i >= callee->fixed_param_count &&
                vm_array_flatten_kind(ctx, arg) >= 1) {
                need_spread = 1;
                break;
            }
        }

        if (need_spread) {
            vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
            vm_chunk_emit_u32(ctx->chunk, argc > 0 ? (uint32_t)argc * 2 : 8);
            for (int i = 0; i < argc; i++) {
                StradaValue *arg = ast_deref(ast_arr_get(args, i));
                int atype = arg ? (int)ast_int(arg, "type") : 0;
                vm_chunk_emit(ctx->chunk, OP_DUP);
                if (atype == NI_SPREAD) {
                    compile_expr(ctx, ast_get(arg, "target"));
                    vm_chunk_emit(ctx->chunk, OP_ARRAY_EXTEND);
                } else if (callee->has_variadic && i >= callee->fixed_param_count &&
                           vm_array_flatten_kind(ctx, arg) >= 1) {
                    compile_expr(ctx, arg);
                    vm_chunk_emit(ctx->chunk, OP_ARRAY_EXTEND);
                } else {
                    compile_expr(ctx, arg);
                    vm_chunk_emit(ctx->chunk, OP_ARRAY_PUSH);
                }
            }
            vm_chunk_emit(ctx->chunk, OP_CALL_SPREAD);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)fidx);
            break;
        }

        for (int i = 0; i < argc; i++) {
            compile_expr(ctx, ast_arr_get(args, i));
        }

        vm_chunk_emit(ctx->chunk, OP_CALL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)fidx);
        vm_chunk_emit(ctx->chunk, (uint8_t)argc);
        break;
    }

    case NI_SUPER_CALL: {
        const char *method = ast_str(node, "method");
        const char *def_pkg = (ctx->cur_package && ctx->cur_package[0]) ? ctx->cur_package : "";
        StradaValue *args = ast_get(node, "args");
        int argc = (int)ast_int(node, "arg_count");
        int call_argc = 0;

        if (argc <= 0) {
            int self_slot = ctx_find_local(ctx, "self");
            if (self_slot >= 0) {
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)self_slot);
            } else {
                vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
            }
            call_argc = 0;
        } else {
            for (int i = 0; i < argc; i++) {
                compile_expr(ctx, ast_arr_get(args, i));
            }
            call_argc = argc - 1;
        }

        size_t pkg_idx = vm_chunk_add_str_const(ctx->chunk, def_pkg);
        size_t midx = vm_chunk_add_str_const(ctx->chunk, method);
        vm_chunk_emit(ctx->chunk, OP_SUPER_CALL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)pkg_idx);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)midx);
        vm_chunk_emit(ctx->chunk, (uint8_t)call_argc);
        break;
    }

    case NI_ASSIGN: {
        StradaValue *target = ast_get(node, "target");
        StradaValue *value = ast_get(node, "value");
        const char *op = ast_str(node, "op");
        target = ast_deref(target);

        int target_type = target ? (int)ast_int(target, "type") : 0;

        /* Assignment to hash element ($h{k} or $hr->{k}) or array element ($a[i] or $ar->[i]) */
        if (target_type == NI_HASH_ACCESS || target_type == NI_DEREF_HASH ||
            target_type == NI_SUBSCRIPT || target_type == NI_DEREF_ARRAY) {
            int is_hash = (target_type == NI_HASH_ACCESS || target_type == NI_DEREF_HASH);
            StradaValue *cont_node = NULL;
            StradaValue *idx_node = NULL;
            if (target_type == NI_HASH_ACCESS) {
                cont_node = ast_get(target, "hash");
                idx_node = ast_get(target, "key");
            } else if (target_type == NI_DEREF_HASH) {
                cont_node = ast_get(target, "ref");
                if (!cont_node) cont_node = ast_get(target, "target");
                idx_node = ast_get(target, "key");
            } else if (target_type == NI_SUBSCRIPT) {
                cont_node = ast_get(target, "array");
                idx_node = ast_get(target, "index");
            } else {
                cont_node = ast_get(target, "ref");
                if (!cont_node) cont_node = ast_get(target, "target");
                idx_node = ast_get(target, "index");
            }

            int d = ctx->assign_depth++;
            char vbuf[32];
            snprintf(vbuf, sizeof(vbuf), "__asg_v%d", d);
            int v_slot = ctx_get_or_add_local(ctx, vbuf, '$');

            if (strcmp(op, "=") == 0) {
                const char *prefix; StradaValue *suffix;
                if (target_type == NI_HASH_ACCESS && is_concat_key(idx_node, &prefix, &suffix)) {
                    compile_expr(ctx, cont_node);
                    compile_expr(ctx, suffix);
                    compile_expr(ctx, value);
                    vm_chunk_emit(ctx->chunk, OP_DUP);
                    vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)v_slot);
                    size_t pidx = vm_chunk_add_str_const(ctx->chunk, prefix);
                    vm_chunk_emit(ctx->chunk, OP_HASH_SET_CONCAT);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)pidx);
                } else {
                    compile_expr(ctx, cont_node);
                    compile_expr(ctx, idx_node);
                    compile_expr(ctx, value);
                    vm_chunk_emit(ctx->chunk, OP_DUP);
                    vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)v_slot);
                    vm_chunk_emit(ctx->chunk, is_hash ? OP_HASH_SET : OP_ARRAY_SET);
                }
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)v_slot);
            } else if (is_shortcircuit_assign(op)) {
                /* Short-circuit compound assignment on element: evaluate container & key once */
                char cbuf[32], kbuf[32];
                snprintf(cbuf, sizeof(cbuf), "__asg_c%d", d);
                snprintf(kbuf, sizeof(kbuf), "__asg_k%d", d);
                int c_slot = ctx_get_or_add_local(ctx, cbuf, '$');
                int k_slot = ctx_get_or_add_local(ctx, kbuf, '$');

                compile_expr(ctx, cont_node);
                vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)c_slot);

                compile_expr(ctx, idx_node);
                vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)k_slot);

                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)c_slot);
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)k_slot);
                vm_chunk_emit(ctx->chunk, is_hash ? OP_HASH_GET : OP_ARRAY_GET);

                vm_chunk_emit(ctx->chunk, OP_DUP);
                if (strcmp(op, "//=") == 0) {
                    vm_chunk_emit(ctx->chunk, OP_IS_UNDEF);
                    vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
                } else if (strcmp(op, "||=") == 0) {
                    vm_chunk_emit(ctx->chunk, OP_JMP_IF_TRUE);
                } else { /* &&= */
                    vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
                }
                size_t skip_sc = emit_patch_u16(ctx);
                vm_chunk_emit(ctx->chunk, OP_POP);

                compile_expr(ctx, value);
                vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)v_slot);

                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)c_slot);
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)k_slot);
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)v_slot);
                vm_chunk_emit(ctx->chunk, is_hash ? OP_HASH_SET : OP_ARRAY_SET);

                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)v_slot);
                patch_u16(ctx, skip_sc, (uint16_t)ctx->chunk->code_len);
            } else {
                /* Compound assignment (+=, -=, *=, /=, %=, .=, **=, &=, |=, ^=, <<=, >>=): evaluate container, key/idx, and RHS once */
                char cbuf[32], kbuf[32];
                snprintf(cbuf, sizeof(cbuf), "__asg_c%d", d);
                snprintf(kbuf, sizeof(kbuf), "__asg_k%d", d);
                int c_slot = ctx_get_or_add_local(ctx, cbuf, '$');
                int k_slot = ctx_get_or_add_local(ctx, kbuf, '$');

                compile_expr(ctx, cont_node);
                vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)c_slot);

                compile_expr(ctx, idx_node);
                vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)k_slot);

                /* Fetch current element */
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)c_slot);
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)k_slot);
                vm_chunk_emit(ctx->chunk, is_hash ? OP_HASH_GET : OP_ARRAY_GET);

                /* Evaluate RHS once and apply compound op */
                compile_expr(ctx, value);
                int cop = compound_op_to_opcode(op);
                if (cop >= 0) vm_chunk_emit(ctx->chunk, (uint8_t)cop);

                vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)v_slot);

                /* Store back into container[key/idx] */
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)c_slot);
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)k_slot);
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)v_slot);
                vm_chunk_emit(ctx->chunk, is_hash ? OP_HASH_SET : OP_ARRAY_SET);

                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)v_slot);
            }
            ctx->assign_depth--;
            break;
        }

        /* Assignment to variable */
        if (target_type == NI_VARIABLE) {
            const char *vname = ast_str(target, "name");
            const char *vsigil = ast_str(target, "sigil");
            int slot = ctx_find_local_sigil(ctx, vname, vsigil[0]);

            if (slot < 0) {
                /* Try capture (including transitive parent captures) */
                int cap = ctx_resolve_capture(ctx, vname, vsigil[0]);
                if (cap >= 0) {
                    int cop = compound_op_to_opcode(op);
                    if (is_shortcircuit_assign(op)) {
                        vm_chunk_emit(ctx->chunk, OP_LOAD_CAPTURE);
                        vm_chunk_emit_u16(ctx->chunk, (uint16_t)cap);
                        vm_chunk_emit(ctx->chunk, OP_DUP);
                        if (strcmp(op, "//=") == 0) {
                            vm_chunk_emit(ctx->chunk, OP_IS_UNDEF);
                            vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
                        } else if (strcmp(op, "||=") == 0) {
                            vm_chunk_emit(ctx->chunk, OP_JMP_IF_TRUE);
                        } else {
                            vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
                        }
                        size_t skip_sc = emit_patch_u16(ctx);
                        vm_chunk_emit(ctx->chunk, OP_POP);
                        compile_expr(ctx, value);
                        vm_chunk_emit(ctx->chunk, OP_DUP);
                        vm_chunk_emit(ctx->chunk, OP_STORE_CAPTURE);
                        vm_chunk_emit_u16(ctx->chunk, (uint16_t)cap);
                        patch_u16(ctx, skip_sc, (uint16_t)ctx->chunk->code_len);
                        break;
                    } else if (cop >= 0) {
                        vm_chunk_emit(ctx->chunk, OP_LOAD_CAPTURE);
                        vm_chunk_emit_u16(ctx->chunk, (uint16_t)cap);
                        compile_expr(ctx, value);
                        vm_chunk_emit(ctx->chunk, (uint8_t)cop);
                    } else {
                        compile_expr(ctx, value);
                    }
                    vm_chunk_emit(ctx->chunk, OP_DUP);
                    vm_chunk_emit(ctx->chunk, OP_STORE_CAPTURE);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)cap);
                    break;
                }
                /* Try our variable */
                if (is_our_var(vname)) {
                    size_t key_idx = vm_chunk_add_str_const(ctx->chunk, vname);
                    int cop = compound_op_to_opcode(op);
                    if (is_shortcircuit_assign(op)) {
                        vm_chunk_emit(ctx->chunk, OP_LOAD_GLOBAL);
                        vm_chunk_emit_u16(ctx->chunk, (uint16_t)key_idx);
                        vm_chunk_emit(ctx->chunk, OP_DUP);
                        if (strcmp(op, "//=") == 0) {
                            vm_chunk_emit(ctx->chunk, OP_IS_UNDEF);
                            vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
                        } else if (strcmp(op, "||=") == 0) {
                            vm_chunk_emit(ctx->chunk, OP_JMP_IF_TRUE);
                        } else {
                            vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
                        }
                        size_t skip_sc = emit_patch_u16(ctx);
                        vm_chunk_emit(ctx->chunk, OP_POP);
                        compile_expr(ctx, value);
                        vm_chunk_emit(ctx->chunk, OP_DUP);
                        vm_chunk_emit(ctx->chunk, OP_STORE_GLOBAL);
                        vm_chunk_emit_u16(ctx->chunk, (uint16_t)key_idx);
                        patch_u16(ctx, skip_sc, (uint16_t)ctx->chunk->code_len);
                        break;
                    } else if (cop >= 0) {
                        vm_chunk_emit(ctx->chunk, OP_LOAD_GLOBAL);
                        vm_chunk_emit_u16(ctx->chunk, (uint16_t)key_idx);
                        compile_expr(ctx, value);
                        vm_chunk_emit(ctx->chunk, (uint8_t)cop);
                    } else {
                        compile_expr(ctx, value);
                    }
                    vm_chunk_emit(ctx->chunk, OP_DUP);
                    vm_chunk_emit(ctx->chunk, OP_STORE_GLOBAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)key_idx);
                    break;
                }
                compile_expr(ctx, value);
                break;
            }

            if (is_shortcircuit_assign(op)) {
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                vm_chunk_emit(ctx->chunk, OP_DUP);
                if (strcmp(op, "//=") == 0) {
                    vm_chunk_emit(ctx->chunk, OP_IS_UNDEF);
                    vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
                } else if (strcmp(op, "||=") == 0) {
                    vm_chunk_emit(ctx->chunk, OP_JMP_IF_TRUE);
                } else {
                    vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
                }
                size_t skip_sc = emit_patch_u16(ctx);
                vm_chunk_emit(ctx->chunk, OP_POP);
                compile_expr(ctx, value);
                vm_chunk_emit(ctx->chunk, OP_DUP);
                vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                patch_u16(ctx, skip_sc, (uint16_t)ctx->chunk->code_len);
                break;
            } else if (strcmp(op, ".=") == 0) {
                /* Optimization: if RHS is a string literal, use OP_APPEND_CONST (no alloc) */
                StradaValue *val_node = ast_deref(value);
                if (val_node && !STRADA_IS_TAGGED_INT(val_node) &&
                    (int)ast_int(val_node, "type") == NI_STR_LITERAL) {
                    const char *sval = ast_str(val_node, "value");
                    size_t si = vm_chunk_add_str_const(ctx->chunk, sval);
                    vm_chunk_emit(ctx->chunk, OP_APPEND_CONST);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)si);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                } else {
                    compile_expr(ctx, value);
                    vm_chunk_emit(ctx->chunk, OP_APPEND_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                }
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                break;
            } else {
                int cop = compound_op_to_opcode(op);
                if (strcmp(op, "=") == 0) {
                    compile_expr(ctx, value);
                } else if (cop >= 0) {
                    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                    compile_expr(ctx, value);
                    vm_chunk_emit(ctx->chunk, (uint8_t)cop);
                } else {
                    compile_expr(ctx, value);
                }
            }

            vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);

            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
        } else {
            compile_expr(ctx, value);
        }
        break;
    }

    case NI_INCREMENT: {
        StradaValue *operand = ast_get(node, "operand");
        int is_pre = (int)ast_int(node, "is_prefix");
        const char *inc_op = ast_str(node, "op");
        int is_inc = (strcmp(inc_op, "++") == 0);

        if (operand && !STRADA_IS_TAGGED_INT(operand) &&
            (int)ast_int(operand, "type") == NI_VARIABLE) {
            const char *vname = ast_str(operand, "name");
            const char *vsigil = ast_str(operand, "sigil");
            int slot = ctx_find_local_sigil(ctx, vname, vsigil[0]);
            if (slot >= 0) {
                if (is_pre) {
                    vm_chunk_emit(ctx->chunk, is_inc ? OP_INCR : OP_DECR);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                } else {
                    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                    vm_chunk_emit(ctx->chunk, is_inc ? OP_INCR : OP_DECR);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                }
            } else if (is_our_var(vname)) {
                /* Increment/decrement on global variable */
                size_t gk = vm_chunk_add_str_const(ctx->chunk, vname);
                if (is_pre) {
                    /* ++$x: load, add 1, store, push new value */
                    vm_chunk_emit(ctx->chunk, OP_LOAD_GLOBAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)gk);
                    vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
                    vm_chunk_emit_i64(ctx->chunk, 1);
                    vm_chunk_emit(ctx->chunk, is_inc ? OP_ADD : OP_SUB);
                    vm_chunk_emit(ctx->chunk, OP_DUP);
                    vm_chunk_emit(ctx->chunk, OP_STORE_GLOBAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)gk);
                } else {
                    /* $x++: load old, dup, add 1, store new, keep old on stack */
                    vm_chunk_emit(ctx->chunk, OP_LOAD_GLOBAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)gk);
                    vm_chunk_emit(ctx->chunk, OP_DUP);
                    vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
                    vm_chunk_emit_i64(ctx->chunk, 1);
                    vm_chunk_emit(ctx->chunk, is_inc ? OP_ADD : OP_SUB);
                    vm_chunk_emit(ctx->chunk, OP_STORE_GLOBAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)gk);
                }
            } else {
                vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
            }
        } else {
            vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
        }
        break;
    }

    case NI_TR: {
        /* expression form: value = count (or modified copy with /r) */
        compile_tr(ctx, node);
        break;
    }

    case NI_REGEX_SUBST: {
        StradaValue *target = ast_get(node, "target");
        const char *pattern = ast_str(node, "pattern");
        const char *replacement = ast_str(node, "replacement");
        const char *flags = ast_str(node, "flags");
        if (!flags) flags = "";
        int is_global = (strchr(flags, 'g') != NULL);

        /* s///e: evaluated replacement (parser supplies eval_expr) */
        if (strchr(flags, 'e') && compile_regex_subst_e(ctx, node)) break;

        if (target && !STRADA_IS_TAGGED_INT(target) &&
            (int)ast_int(target, "type") == NI_VARIABLE) {
            const char *vname = ast_str(target, "name");
            const char *vsigil = ast_str(target, "sigil");
            int slot = ctx_find_local_sigil(ctx, vname, vsigil[0]);
            int is_global_var = (slot < 0 && is_our_var(vname)) ? 1 : 0;

            if (slot >= 0 || is_global_var) {
                /* Load the target string */
                if (slot >= 0) {
                    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                } else {
                    size_t gk = vm_chunk_add_str_const(ctx->chunk, vname);
                    vm_chunk_emit(ctx->chunk, OP_LOAD_GLOBAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)gk);
                }

                size_t pat_idx = add_pattern_const(ctx, pattern, flags);
                vm_chunk_emit(ctx->chunk, OP_PUSH_STR);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)pat_idx);

                size_t repl_idx = vm_chunk_add_str_const(ctx->chunk, replacement);
                vm_chunk_emit(ctx->chunk, OP_PUSH_STR);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)repl_idx);

                /* Use BUILTIN_REPLACE for /g, OP_STR_REPLACE for non-/g */
                if (is_global) {
                    vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                    vm_chunk_emit_u16(ctx->chunk, BUILTIN_REPLACE_ALL);
                    vm_chunk_emit(ctx->chunk, 3);
                } else {
                    vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                    vm_chunk_emit_u16(ctx->chunk, BUILTIN_REPLACE);
                    vm_chunk_emit(ctx->chunk, 3);
                }

                /* Store result back */
                if (slot >= 0) {
                    vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                } else {
                    size_t gk = vm_chunk_add_str_const(ctx->chunk, vname);
                    vm_chunk_emit(ctx->chunk, OP_DUP);
                    vm_chunk_emit(ctx->chunk, OP_STORE_GLOBAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)gk);
                }
                break;
            }
        }
        vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
        break;
    }

    case NI_MAP: {
        compile_map_expr(ctx, node);
        break;
        /* OLD CODE BELOW — not reached */
        StradaValue *body = ast_get(node, "body");
        StradaValue *array = ast_get(node, "array");

        /* Compile array */
        compile_expr(ctx, array);
        int arr_slot = ctx_add_local(ctx, "__map_arr", '$');
        vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)arr_slot);

        /* Result array */
        vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
        vm_chunk_emit_u32(ctx->chunk, 8);
        int res_slot = ctx_add_local(ctx, "__map_res", '@');
        vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)res_slot);

        /* Loop index */
        vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
        vm_chunk_emit_i64(ctx->chunk, 0);
        int idx_slot = ctx_add_local(ctx, "__map_i", '$');
        vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx_slot);

        /* Ensure $_ exists */
        int underscore_slot = ctx_find_local(ctx, "_");
        if (underscore_slot < 0) underscore_slot = ctx_add_local(ctx, "_", '$');

        /* Loop: while ($i < size(@arr)) */
        size_t loop_top = ctx->chunk->code_len;

        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx_slot);
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)arr_slot);
        vm_chunk_emit(ctx->chunk, OP_ARRAY_SIZE);
        vm_chunk_emit(ctx->chunk, OP_LT);
        vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
        size_t exit_patch = emit_patch_u16(ctx);

        /* $_ = $arr[$i] */
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)arr_slot);
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx_slot);
        vm_chunk_emit(ctx->chunk, OP_ARRAY_GET);
        vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)underscore_slot);

        /* Compile body — result is on stack */
        compile_block(ctx, body);
        /* The block leaves its last expression... but blocks are statements.
         * For map, we need the expression result. The body is typically an EXPR_STMT
         * whose result we need. Actually, map body is a block with statements.
         * We need the result of the last expression. This is tricky.
         * For now, load $_ as fallback — the block may have modified it.
         * Actually, the map body in test is `$_ * 2;` which is an EXPR_STMT that
         * pops the result. We need to capture it before the pop.
         * Solution: compile the last statement's expression directly. */
        /* Re-approach: let's handle map specially by compiling body as expression */
        /* Actually, let's just use the last statement's expr result.
         * We already compiled the block above which popped results.
         * Instead, let's not compile via compile_block — compile the body expression. */
        /* Hmm, this is complex. Let's use a simpler approach: the map block
         * typically has one statement which is an EXPR_STMT. Let's extract and eval it. */

        /* Push result onto result array */
        /* Since the block already compiled and popped, let's re-evaluate the expression.
         * Actually, let me fix this: don't compile the block, compile the expression. */

        /* Well, compile_block already ran. The result was popped.
         * For a quick fix: load $_ (which the block may have used) */
        /* This won't give correct results. Let me restructure. */

        /* Result: the last value computed is in $_ context or last expr.
         * Let's just push onto result. The result of the block EXPR_STMT was
         * popped by compile_stmt. We need to handle this differently. */

        /* Actually the simplest approach: for map/grep, extract the body expression
         * and compile it as an expression, not a block. */
        /* We compiled the block above already, oops. Let's just re-run the expression. */
        /* Get the last statement's expression */
        StradaValue *stmts = ast_get(body, "statements");
        int stmt_count = (int)ast_int(body, "statement_count");
        if (stmt_count > 0) {
            StradaValue *last_stmt = ast_arr_get(stmts, stmt_count - 1);
            last_stmt = ast_deref(last_stmt);
            if (last_stmt && !STRADA_IS_TAGGED_INT(last_stmt) &&
                (int)ast_int(last_stmt, "type") == NI_EXPR_STMT) {
                /* This was already compiled and popped. Re-compile the expression
                 * for the push. This means the side effect runs twice for the last iteration. */
                /* Actually, the bytecode already compiled the whole block. The issue is
                 * compile_stmt(EXPR_STMT) does compile_expr + POP.
                 * For map, we need to NOT pop the last expression.
                 *
                 * Better approach: undo the block compilation above and redo properly.
                 * But we can't undo. So let's just compile the expression again. */
                compile_expr(ctx, ast_get(last_stmt, "expr"));
            } else {
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)underscore_slot);
            }
        } else {
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)underscore_slot);
        }

        /* Wait — we compiled the block above which ran all statements including POP.
         * The re-compilation of the expression will produce the right value but
         * with double side-effects. For map body `$_ * 2`, this is fine since it's
         * a pure expression. */

        /* Actually let's fix this properly: DON'T compile the block above.
         * Instead, compile the expression directly. We need to revert the block. */
        /* Since we can't easily revert bytecode, let me restructure this whole case. */

        /* HACK: The block was compiled but its results were popped. Recompile just
         * the last expression for the push. This works for pure expressions. */

        /* Push onto result */
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)res_slot);
        /* swap: we have [val, arr] but need [arr, val] for ARRAY_PUSH */
        /* Actually ARRAY_PUSH pops val then arr */
        /* Stack: [expr_result, result_arr] — but we loaded result_arr on top */
        /* Let me fix: push result first, then the value */

        /* REDO THIS SECTION PROPERLY */
        /* At this point: the block compiled and popped its results.
         * We just re-compiled the expression, so stack has: [expr_value]
         * Now push result array and swap for ARRAY_PUSH which expects [arr, val] → pop val, pop arr */
        /* ARRAY_PUSH: pops value, then array */
        /* Remove the LOAD_LOCAL we just emitted above (the result_arr load) */
        /* Actually I realize the above is wrong — let me just redo the map section cleanly. */

        /* Let me cheat: just truncate back and redo. */
        break; /* We'll handle map differently below */
    }

    case NI_ARRAY_SLICE: {
        compile_expr(ctx, ast_get(node, "source"));
        StradaValue *indices = ast_get(node, "items");
        int count = (int)ast_int(node, "item_count");

        int src_slot = ctx_add_local(ctx, "__slice_src", '$');
        vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)src_slot);

        vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
        vm_chunk_emit_u32(ctx->chunk, count > 0 ? count : 8);
        int res_slot2 = ctx_add_local(ctx, "__slice_res", '@');
        vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)res_slot2);

        for (int i = 0; i < count; i++) {
            StradaValue *idx_expr = ast_arr_get(indices, i);
            int idx_type = (idx_expr && !STRADA_IS_TAGGED_INT(idx_expr))
                               ? (int)ast_int(idx_expr, "type") : 0;
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)res_slot2);
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)src_slot);
            if (idx_type == NI_RANGE) {
                /* @arr[A..B] — the range evaluates to an index array */
                compile_expr(ctx, idx_expr);
                vm_chunk_emit(ctx->chunk, OP_SLICE_IDXS);
            } else {
                compile_expr(ctx, idx_expr);
                vm_chunk_emit(ctx->chunk, OP_ARRAY_GET);
                vm_chunk_emit(ctx->chunk, OP_ARRAY_PUSH);
            }
        }
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)res_slot2);
        break;
    }

    case NI_HASH_SLICE: {
        compile_expr(ctx, ast_get(node, "source"));
        StradaValue *keys = ast_get(node, "items");
        int count = (int)ast_int(node, "item_count");

        int hsrc_slot = ctx_add_local(ctx, "__hslice_src", '%');
        vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)hsrc_slot);

        vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
        vm_chunk_emit_u32(ctx->chunk, count > 0 ? count : 8);
        int hres_slot = ctx_add_local(ctx, "__hslice_res", '@');
        vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)hres_slot);

        for (int i = 0; i < count; i++) {
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)hres_slot);
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)hsrc_slot);
            compile_expr(ctx, ast_arr_get(keys, i));
            vm_chunk_emit(ctx->chunk, OP_HASH_GET);
            vm_chunk_emit(ctx->chunk, OP_ARRAY_PUSH);
        }
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)hres_slot);
        break;
    }

    case NI_GREP: {
        compile_grep_expr(ctx, node);
        break;
    }

    case NI_SORT: {
        compile_sort_expr(ctx, node);
        break;
    }

    case NI_RANGE: {
        /* start..end — create array of integers */
        StradaValue *from = ast_get(node, "start");
        StradaValue *to = ast_get(node, "end");
        compile_expr(ctx, from);
        compile_expr(ctx, to);
        /* Use OP_BUILTIN for range since there's no dedicated opcode */
        vm_chunk_emit(ctx->chunk, OP_BUILTIN);
        vm_chunk_emit_u16(ctx->chunk, BUILTIN_RANGE);
        vm_chunk_emit(ctx->chunk, 2);
        break;
    }

    default:
        /* fprintf(stderr, "VM compile: unhandled expression node type %d\n", type); */
        vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
        break;
    }
}

/* ===== Map/grep/sort compilation (done properly without blocks) ===== */

/* Compile a multi-statement block in expression position (for map/grep/sort blocks),
 * leaving the last expression's value on the stack. */
static void compile_block_expr(CompCtx *ctx, StradaValue *body, int fallback_slot) {
    if (!body || STRADA_IS_TAGGED_INT(body)) {
        if (fallback_slot >= 0) {
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)fallback_slot);
        } else {
            vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
            vm_chunk_emit_i64(ctx->chunk, 0);
        }
        return;
    }
    StradaValue *stmts = ast_get(body, "statements");
    int stmt_count = (int)ast_int(body, "statement_count");
    if (stmt_count <= 0) {
        if (fallback_slot >= 0) {
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)fallback_slot);
        } else {
            vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
            vm_chunk_emit_i64(ctx->chunk, 0);
        }
        return;
    }
    for (int i = 0; i < stmt_count - 1; i++) {
        compile_stmt(ctx, ast_arr_get(stmts, i));
    }
    StradaValue *last_stmt = ast_deref(ast_arr_get(stmts, stmt_count - 1));
    if (last_stmt && !STRADA_IS_TAGGED_INT(last_stmt)) {
        int ltype = (int)ast_int(last_stmt, "type");
        if (ltype == NI_EXPR_STMT) {
            compile_expr(ctx, ast_get(last_stmt, "expr"));
            return;
        }
        if (ltype == NI_IF_STMT) {
            compile_expr(ctx, ast_get(last_stmt, "condition"));
            vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
            size_t else_patch = emit_patch_u16(ctx);
            compile_block_expr(ctx, ast_get(last_stmt, "then_block"), fallback_slot);

            StradaValue *elsif_conds = ast_get(last_stmt, "elsif_conditions");
            StradaValue *elsif_blocks = ast_get(last_stmt, "elsif_blocks");
            int elsif_count = (int)ast_int(last_stmt, "elsif_count");
            size_t end_patches[32];
            int end_patch_count = 0;

            vm_chunk_emit(ctx->chunk, OP_JMP);
            end_patches[end_patch_count++] = emit_patch_u16(ctx);
            patch_u16(ctx, else_patch, (uint16_t)ctx->chunk->code_len);

            for (int i = 0; i < elsif_count && i < 30; i++) {
                compile_expr(ctx, ast_arr_get(elsif_conds, i));
                vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
                size_t next_patch = emit_patch_u16(ctx);
                compile_block_expr(ctx, ast_arr_get(elsif_blocks, i), fallback_slot);
                vm_chunk_emit(ctx->chunk, OP_JMP);
                end_patches[end_patch_count++] = emit_patch_u16(ctx);
                patch_u16(ctx, next_patch, (uint16_t)ctx->chunk->code_len);
            }
            StradaValue *else_block = ast_get(last_stmt, "else_block");
            if (else_block && !STRADA_IS_TAGGED_INT(else_block)) {
                compile_block_expr(ctx, else_block, fallback_slot);
            } else {
                vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
            }
            for (int i = 0; i < end_patch_count; i++) {
                patch_u16(ctx, end_patches[i], (uint16_t)ctx->chunk->code_len);
            }
            return;
        }
        compile_stmt(ctx, last_stmt);
    }
    if (fallback_slot >= 0) {
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)fallback_slot);
    } else {
        vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
        vm_chunk_emit_i64(ctx->chunk, 0);
    }
}

static void compile_map_expr(CompCtx *ctx, StradaValue *node) {
    /* map { BLOCK } @array */
    StradaValue *body = ast_get(node, "block");
    if (!body) body = ast_get(node, "body");
    StradaValue *array = ast_get(node, "array");

    compile_expr(ctx, array);
    int arr_slot = ctx_add_local(ctx, "__mg_arr", '$');
    vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)arr_slot);

    vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
    vm_chunk_emit_u32(ctx->chunk, 8);
    int res_slot = ctx_add_local(ctx, "__mg_res", '@');
    vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)res_slot);

    vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
    vm_chunk_emit_i64(ctx->chunk, 0);
    int idx_slot = ctx_add_local(ctx, "__mg_i", '$');
    vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx_slot);

    int underscore_slot = ctx_find_local(ctx, "_");
    if (underscore_slot < 0) underscore_slot = ctx_add_local(ctx, "_", '$');

    int tmp_slot = ctx_add_local(ctx, "__mg_tmp", '$');

    size_t loop_top = ctx->chunk->code_len;
    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx_slot);
    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)arr_slot);
    vm_chunk_emit(ctx->chunk, OP_ARRAY_SIZE);
    vm_chunk_emit(ctx->chunk, OP_LT);
    vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
    size_t exit_patch = emit_patch_u16(ctx);

    /* $_ = $arr[$i] */
    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)arr_slot);
    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx_slot);
    vm_chunk_emit(ctx->chunk, OP_ARRAY_GET);
    vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)underscore_slot);

    /* Compile body block */
    compile_block_expr(ctx, body, underscore_slot);

    /* Store expr value to tmp, then push arr, tmp for ARRAY_PUSH */
    /* ARRAY_PUSH: v = pop (value), a = pop (array) */
    vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)tmp_slot);

    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)res_slot);
    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)tmp_slot);
    vm_chunk_emit(ctx->chunk, OP_ARRAY_PUSH);

    vm_chunk_emit(ctx->chunk, OP_INCR);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx_slot);

    vm_chunk_emit(ctx->chunk, OP_JMP);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)loop_top);

    patch_u16(ctx, exit_patch, (uint16_t)ctx->chunk->code_len);

    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)res_slot);
}

static void compile_grep_expr(CompCtx *ctx, StradaValue *node) {
    StradaValue *body = ast_get(node, "block");
    if (!body) body = ast_get(node, "body");
    StradaValue *array = ast_get(node, "array");

    compile_expr(ctx, array);
    int arr_slot = ctx_add_local(ctx, "__gr_arr", '$');
    vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)arr_slot);

    vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
    vm_chunk_emit_u32(ctx->chunk, 8);
    int res_slot = ctx_add_local(ctx, "__gr_res", '@');
    vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)res_slot);

    vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
    vm_chunk_emit_i64(ctx->chunk, 0);
    int idx_slot = ctx_add_local(ctx, "__gr_i", '$');
    vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx_slot);

    int underscore_slot = ctx_find_local(ctx, "_");
    if (underscore_slot < 0) underscore_slot = ctx_add_local(ctx, "_", '$');

    size_t loop_top = ctx->chunk->code_len;
    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx_slot);
    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)arr_slot);
    vm_chunk_emit(ctx->chunk, OP_ARRAY_SIZE);
    vm_chunk_emit(ctx->chunk, OP_LT);
    vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
    size_t exit_patch = emit_patch_u16(ctx);

    /* $_ = $arr[$i] */
    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)arr_slot);
    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx_slot);
    vm_chunk_emit(ctx->chunk, OP_ARRAY_GET);
    vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)underscore_slot);

    /* Compile condition block */
    compile_block_expr(ctx, body, underscore_slot);

    /* If condition is true, push $_ to result */
    vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
    size_t skip_patch = emit_patch_u16(ctx);

    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)res_slot);
    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)underscore_slot);
    vm_chunk_emit(ctx->chunk, OP_ARRAY_PUSH);

    patch_u16(ctx, skip_patch, (uint16_t)ctx->chunk->code_len);

    vm_chunk_emit(ctx->chunk, OP_INCR);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx_slot);
    vm_chunk_emit(ctx->chunk, OP_JMP);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)loop_top);

    patch_u16(ctx, exit_patch, (uint16_t)ctx->chunk->code_len);

    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)res_slot);
}

static void compile_sort_expr(CompCtx *ctx, StradaValue *node) {
    StradaValue *body = ast_get(node, "block");
    if (!body) body = ast_get(node, "body");
    StradaValue *array = ast_get(node, "array");

    /* Compile the comparison function */
    char cmpname[128];
    static int sort_counter = 0;
    snprintf(cmpname, sizeof(cmpname), "__sort_cmp_%d", sort_counter++);

    /* Save outer chunk index */
    int sort_outer_idx = -1;
    for (size_t fi = 0; fi < ctx->prog->func_count; fi++) {
        if (&ctx->prog->funcs[fi] == ctx->chunk) { sort_outer_idx = fi; break; }
    }

    CompCtx cmp_ctx = {0};
    cmp_ctx.chunk = vm_program_add_func(ctx->prog, cmpname);

    /* Restore outer chunk pointer */
    if (sort_outer_idx >= 0) ctx->chunk = &ctx->prog->funcs[sort_outer_idx];

    cmp_ctx.prog = ctx->prog;
    cmp_ctx.func_name = cmpname;

    /* Add $a and $b as locals */
    ctx_add_local(&cmp_ctx, "a", '$');
    ctx_add_local(&cmp_ctx, "b", '$');
    cmp_ctx.chunk->fixed_param_count = 2;

    /* Compile the comparison expression */
    if (!body || STRADA_IS_TAGGED_INT(body) || (body->type == STRADA_INT && body->value.iv == 0)) {
        /* No block: use BUILTIN_SORT_DEFAULT instead of the comparator approach */
        /* Clean up the unused comparator */
        vm_chunk_emit(cmp_ctx.chunk, OP_PUSH_INT);
        vm_chunk_emit_i64(cmp_ctx.chunk, 0);
        vm_chunk_emit(cmp_ctx.chunk, OP_RETURN);
        cmp_ctx.chunk->local_count = cmp_ctx.local_count;
        for (int i = 0; i < cmp_ctx.local_count; i++) free(cmp_ctx.local_names[i]);

        /* Emit default sort via builtin */
        compile_expr(ctx, array);
        vm_chunk_emit(ctx->chunk, OP_BUILTIN);
        vm_chunk_emit_u16(ctx->chunk, BUILTIN_SORT_DEFAULT);
        vm_chunk_emit(ctx->chunk, 1);
        return;  /* Skip the rest of compile_sort_expr */
    } else {
        compile_block_expr(&cmp_ctx, body, -1);
    }
    vm_chunk_emit(cmp_ctx.chunk, OP_RETURN);
    cmp_ctx.chunk->local_count = cmp_ctx.local_count;

    for (int i = 0; i < cmp_ctx.local_count; i++) free(cmp_ctx.local_names[i]);

    /* Now compile: OP_ARRAY_SORT func_idx */
    compile_expr(ctx, array);
    int cmp_fidx = vm_program_find_func(ctx->prog, cmpname);
    vm_chunk_emit(ctx->chunk, OP_ARRAY_SORT);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)cmp_fidx);
}

/* ===== Statement compilation ===== */

static void compile_stmt(CompCtx *ctx, StradaValue *node) {
    if (!node || STRADA_IS_TAGGED_INT(node)) return;
    node = ast_deref(node);
    if (!node || STRADA_IS_TAGGED_INT(node)) return;

    int type = (int)ast_int(node, "type");

    switch (type) {
    case NI_EXPR_STMT: {
        StradaValue *expr = ast_get(node, "expr");
        expr = ast_deref(expr);
        if (expr && !STRADA_IS_TAGGED_INT(expr)) {
            int etype = (int)ast_int(expr, "type");
            if (etype == NI_TR || etype == NI_REGEX_SUBST) {
                compile_stmt(ctx, expr);
                break;
            }
            if (etype == NI_ASSIGN && strcmp(ast_str(expr, "op"), ".=") == 0) {
                StradaValue *t = ast_deref(ast_get(expr, "target"));
                if (t && !STRADA_IS_TAGGED_INT(t) && (int)ast_int(t, "type") == NI_VARIABLE) {
                    const char *vname = ast_str(t, "name");
                    const char *vsigil = ast_str(t, "sigil");
                    int slot = ctx_find_local_sigil(ctx, vname, vsigil[0]);
                    if (slot >= 0) {
                        StradaValue *val_node = ast_deref(ast_get(expr, "value"));
                        if (val_node && !STRADA_IS_TAGGED_INT(val_node) &&
                            (int)ast_int(val_node, "type") == NI_STR_LITERAL) {
                            size_t si = vm_chunk_add_str_const(ctx->chunk, ast_str(val_node, "value"));
                            vm_chunk_emit(ctx->chunk, OP_APPEND_CONST);
                            vm_chunk_emit_u16(ctx->chunk, (uint16_t)si);
                            vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                        } else {
                            compile_expr(ctx, ast_get(expr, "value"));
                            vm_chunk_emit(ctx->chunk, OP_APPEND_LOCAL);
                            vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                        }
                        break;
                    }
                }
            }
        }
        compile_expr(ctx, expr);
        vm_chunk_emit(ctx->chunk, OP_POP);
        break;
    }

    case NI_VAR_DECL: {
        const char *name = ast_str(node, "name");
        const char *sigil = ast_str(node, "sigil");
        int slot = ctx_add_local(ctx, name, sigil[0]);
        StradaValue *init = ast_get(node, "init");

        if (sigil[0] == '@') {
            int init_type = (init && !STRADA_IS_TAGGED_INT(init)) ? (int)ast_int(init, "type") : 0;
            /* () is parsed as empty ANON_HASH — treat as empty array for @ sigil */
            if (init_type == NI_ANON_HASH) {
                int ec = (int)ast_int(init, "element_count");
                if (ec == 0) init_type = 0; /* fall through to OP_NEW_ARRAY */
            }
            if (init_type != 0) {
                compile_expr(ctx, init);
            } else {
                int64_t cap = ast_int(node, "initial_capacity");
                vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
                vm_chunk_emit_u32(ctx->chunk, cap > 0 ? (uint32_t)cap : 8);
            }
        } else if (sigil[0] == '%') {
            if (init && !STRADA_IS_TAGGED_INT(init) && (int)ast_int(init, "type") != 0) {
                init = ast_deref(init);
                int init_type = (int)ast_int(init, "type");
                /* hash init type handling */
                if (init_type == NI_ANON_HASH) {
                    /* Hash literal — compile directly */
                    compile_expr(ctx, init);
                } else if (init_type == NI_ANON_ARRAY) {
                    /* Array-of-pairs — convert to hash.
                     * The parser creates: [["name","Alice"], ["age",30]]
                     * where each element is a 2-element ANON_ARRAY from => */
                    StradaValue *elems = ast_deref(ast_get(init, "elements"));
                    int elem_count = (int)ast_int(init, "element_count");
                    vm_chunk_emit(ctx->chunk, OP_NEW_HASH);
                    vm_chunk_emit_u32(ctx->chunk, elem_count > 0 ? elem_count : 16);
                    for (int ei = 0; ei < elem_count; ei++) {
                        StradaValue *elem = ast_deref(ast_arr_get(elems, ei));
                        int et = elem ? (int)ast_int(elem, "type") : 0;
                        if (et == NI_ANON_ARRAY) {
                            /* Pair: [key, value] */
                            StradaValue *pair_elems = ast_deref(ast_get(elem, "elements"));
                            int pair_count = (int)ast_int(elem, "element_count");
                            if (pair_count >= 2) {
                                vm_chunk_emit(ctx->chunk, OP_DUP);
                                compile_expr(ctx, ast_arr_get(pair_elems, 0));
                                compile_expr(ctx, ast_arr_get(pair_elems, 1));
                                vm_chunk_emit(ctx->chunk, OP_HASH_SET);
                            }
                        } else {
                            /* Flat key-value pair (every 2 elements) */
                            if (ei + 1 < elem_count) {
                                vm_chunk_emit(ctx->chunk, OP_DUP);
                                compile_expr(ctx, elem);
                                compile_expr(ctx, ast_arr_get(elems, ei + 1));
                                vm_chunk_emit(ctx->chunk, OP_HASH_SET);
                                ei++; /* skip value element */
                            }
                        }
                    }
                } else {
                    compile_expr(ctx, init);
                }
            } else {
                int64_t cap = ast_int(node, "initial_capacity");
                vm_chunk_emit(ctx->chunk, OP_NEW_HASH);
                vm_chunk_emit_u32(ctx->chunk, cap > 0 ? (uint32_t)cap : 16);
            }
        } else if (init && !STRADA_IS_TAGGED_INT(init)) {
            compile_expr(ctx, init);
        } else {
            vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
            vm_chunk_emit_i64(ctx->chunk, 0);
        }
        vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
        break;
    }

    case NI_OUR_DECL: {
        /* our variable — use global registry */
        const char *name = ast_str(node, "name");
        const char *sigil = ast_str(node, "sigil");
        StradaValue *init = ast_get(node, "init");

        register_our_var(name);

        size_t key_idx = vm_chunk_add_str_const(ctx->chunk, name);

        if (sigil[0] == '@') {
            /* our @array — ensure we always create an array */
            int init_type = (init && !STRADA_IS_TAGGED_INT(init)) ? (int)ast_int(init, "type") : 0;
            if (init_type == NI_ANON_HASH) {
                int ec = (int)ast_int(init, "element_count");
                if (ec == 0) init_type = 0; /* empty () → empty array */
            }
            if (init_type == NI_ANON_ARRAY) {
                compile_expr(ctx, init);
            } else if (init_type != 0) {
                /* Non-array init for @sigil: wrap in array */
                vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
                vm_chunk_emit_u32(ctx->chunk, 8);
                vm_chunk_emit(ctx->chunk, OP_DUP);
                compile_expr(ctx, init);
                vm_chunk_emit(ctx->chunk, OP_ARRAY_PUSH);
            } else {
                vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
                vm_chunk_emit_u32(ctx->chunk, 8);
            }
        } else if (sigil[0] == '%') {
            /* our %hash — ensure we create a hash */
            if (init && !STRADA_IS_TAGGED_INT(init)) {
                compile_expr(ctx, init);
            } else {
                vm_chunk_emit(ctx->chunk, OP_NEW_HASH);
                vm_chunk_emit_u32(ctx->chunk, 16);
            }
        } else if (init && !STRADA_IS_TAGGED_INT(init)) {
            compile_expr(ctx, init);
        } else {
            vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
            vm_chunk_emit_i64(ctx->chunk, 0);
        }
        vm_chunk_emit(ctx->chunk, OP_STORE_GLOBAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)key_idx);
        break;
    }

    case NI_LOCAL_DECL: {
        /* local $var = expr — save and restore global */
        const char *name = ast_str(node, "name");
        StradaValue *init = ast_get(node, "init");

        size_t key_idx = vm_chunk_add_str_const(ctx->chunk, name);

        /* Save current value */
        vm_chunk_emit(ctx->chunk, OP_SAVE_GLOBAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)key_idx);

        /* Store new value */
        if (init && !STRADA_IS_TAGGED_INT(init)) {
            compile_expr(ctx, init);
        } else {
            vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
        }
        vm_chunk_emit(ctx->chunk, OP_STORE_GLOBAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)key_idx);

        /* Track for restore on function return */
        if (ctx->local_restore_count < 32) {
            ctx->local_restores[ctx->local_restore_count++] = strdup(name);
        }
        break;
    }

    case NI_CONST_DECL: {
        /* const — a program-wide named value. The decl is compiled inside
         * __globals_init (consts live in the program's globals list) while
         * references compile inside other functions as plain $NAME variables,
         * so a local slot is invisible to them: store to the global registry
         * and register the name for the variable-load fallback. */
        const char *name = ast_str(node, "name");
        register_const_var(name);
        StradaValue *init = ast_get(node, "init");
        if (init && !STRADA_IS_TAGGED_INT(init)) {
            compile_expr(ctx, init);
        } else {
            vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
            vm_chunk_emit_i64(ctx->chunk, 0);
        }
        size_t key_idx = vm_chunk_add_str_const(ctx->chunk, name);
        vm_chunk_emit(ctx->chunk, OP_STORE_GLOBAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)key_idx);
        break;
    }

    case NI_RETURN_STMT: {
        StradaValue *val = ast_get(node, "value");
        if (val && !STRADA_IS_TAGGED_INT(val) && (val->type == STRADA_HASH || val->type == STRADA_REF)) {
            compile_expr(ctx, val);
        } else {
            vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
        }
        /* Unwind any enclosing try/finally blocks before returning */
        emit_try_unwind_for_return(ctx);
        /* Emit local restores before return */
        for (int i = ctx->local_restore_count - 1; i >= 0; i--) {
            size_t key_idx = vm_chunk_add_str_const(ctx->chunk, ctx->local_restores[i]);
            vm_chunk_emit(ctx->chunk, OP_RESTORE_GLOBAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)key_idx);
        }
        vm_chunk_emit(ctx->chunk, OP_RETURN);
        break;
    }

    case NI_IF_STMT: {
        compile_expr(ctx, ast_get(node, "condition"));
        vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
        size_t else_patch = emit_patch_u16(ctx);

        compile_block(ctx, ast_get(node, "then_block"));

        StradaValue *elsif_conds = ast_get(node, "elsif_conditions");
        StradaValue *elsif_blocks = ast_get(node, "elsif_blocks");
        int elsif_count = (int)ast_int(node, "elsif_count");

        size_t end_patches[32];
        int end_patch_count = 0;

        vm_chunk_emit(ctx->chunk, OP_JMP);
        end_patches[end_patch_count++] = emit_patch_u16(ctx);

        patch_u16(ctx, else_patch, (uint16_t)ctx->chunk->code_len);

        for (int i = 0; i < elsif_count && i < 30; i++) {
            compile_expr(ctx, ast_arr_get(elsif_conds, i));
            vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
            size_t next_patch = emit_patch_u16(ctx);
            compile_block(ctx, ast_arr_get(elsif_blocks, i));
            vm_chunk_emit(ctx->chunk, OP_JMP);
            end_patches[end_patch_count++] = emit_patch_u16(ctx);
            patch_u16(ctx, next_patch, (uint16_t)ctx->chunk->code_len);
        }

        StradaValue *else_block = ast_get(node, "else_block");
        if (else_block && !STRADA_IS_TAGGED_INT(else_block)) {
            compile_block(ctx, else_block);
        }

        for (int i = 0; i < end_patch_count; i++) {
            patch_u16(ctx, end_patches[i], (uint16_t)ctx->chunk->code_len);
        }
        break;
    }

    case NI_WHILE_STMT: {
        LoopCtx lctx = {0};
        lctx.parent = ctx->loop_ctx;
        lctx.local_restore_depth = ctx->local_restore_count;
        const char *wlabel = ast_str(node, "label");
        if (wlabel[0]) lctx.label = wlabel;
        ctx->loop_ctx = &lctx;

        size_t loop_top = ctx->chunk->code_len;
        lctx.continue_target = loop_top; /* next jumps to loop_top to re-check condition */

        compile_expr(ctx, ast_get(node, "condition"));
        vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
        size_t exit_patch = emit_patch_u16(ctx);

        /* redo jumps here — after condition check, at body start */
        lctx.redo_target = ctx->chunk->code_len;

        compile_block(ctx, ast_get(node, "body"));

        /* Patch next jumps to here (re-evaluate condition) */
        for (int i = 0; i < lctx.next_count; i++) {
            patch_u16(ctx, lctx.next_patches[i], (uint16_t)ctx->chunk->code_len);
        }

        vm_chunk_emit(ctx->chunk, OP_JMP);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)loop_top);

        patch_u16(ctx, exit_patch, (uint16_t)ctx->chunk->code_len);

        /* Patch break jumps */
        for (int i = 0; i < lctx.break_count; i++) {
            patch_u16(ctx, lctx.break_patches[i], (uint16_t)ctx->chunk->code_len);
        }

        ctx->loop_ctx = lctx.parent;
        break;
    }

    case NI_DO_WHILE_STMT: {
        LoopCtx lctx = {0};
        lctx.parent = ctx->loop_ctx;
        lctx.local_restore_depth = ctx->local_restore_count;
        ctx->loop_ctx = &lctx;

        size_t loop_top = ctx->chunk->code_len;
        lctx.redo_target = loop_top;

        compile_block(ctx, ast_get(node, "body"));

        /* Patch next jumps to condition check */
        for (int i = 0; i < lctx.next_count; i++) {
            patch_u16(ctx, lctx.next_patches[i], (uint16_t)ctx->chunk->code_len);
        }

        compile_expr(ctx, ast_get(node, "condition"));
        vm_chunk_emit(ctx->chunk, OP_JMP_IF_TRUE);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)loop_top);

        for (int i = 0; i < lctx.break_count; i++) {
            patch_u16(ctx, lctx.break_patches[i], (uint16_t)ctx->chunk->code_len);
        }

        ctx->loop_ctx = lctx.parent;
        break;
    }

    case NI_FOR_STMT: {
        StradaValue *init = ast_get(node, "init");
        if (init && !STRADA_IS_TAGGED_INT(init)) compile_stmt(ctx, init);

        LoopCtx lctx = {0};
        lctx.parent = ctx->loop_ctx;
        lctx.local_restore_depth = ctx->local_restore_count;
        const char *flabel = ast_str(node, "label");
        if (flabel[0]) lctx.label = flabel;
        ctx->loop_ctx = &lctx;

        size_t loop_top = ctx->chunk->code_len;

        StradaValue *cond = ast_get(node, "condition");
        if (cond && !STRADA_IS_TAGGED_INT(cond)) {
            compile_expr(ctx, cond);
            vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
        } else {
            vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
            vm_chunk_emit_i64(ctx->chunk, 1);
            vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
        }
        size_t exit_patch = emit_patch_u16(ctx);

        /* redo jumps here — body start */
        lctx.redo_target = ctx->chunk->code_len;

        compile_block(ctx, ast_get(node, "body"));

        /* next jumps to step */
        for (int i = 0; i < lctx.next_count; i++) {
            patch_u16(ctx, lctx.next_patches[i], (uint16_t)ctx->chunk->code_len);
        }

        StradaValue *step = ast_get(node, "update");
        if (!step) step = ast_get(node, "step");
        if (step && !STRADA_IS_TAGGED_INT(step)) {
            compile_expr(ctx, step);
            vm_chunk_emit(ctx->chunk, OP_POP);
        }

        vm_chunk_emit(ctx->chunk, OP_JMP);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)loop_top);

        patch_u16(ctx, exit_patch, (uint16_t)ctx->chunk->code_len);

        for (int i = 0; i < lctx.break_count; i++) {
            patch_u16(ctx, lctx.break_patches[i], (uint16_t)ctx->chunk->code_len);
        }

        ctx->loop_ctx = lctx.parent;
        break;
    }

    case NI_FOREACH_STMT: {
        /* foreach my $var (@array) { body } */
        const char *var_name = ast_str(node, "var_name");
        StradaValue *array_expr = ast_get(node, "array");

        /* Compile array */
        compile_expr(ctx, array_expr);
        int arr_slot = ctx_add_local(ctx, "__fe_arr", '$');
        vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)arr_slot);

        /* Index */
        vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
        vm_chunk_emit_i64(ctx->chunk, 0);
        int idx_slot = ctx_add_local(ctx, "__fe_i", '$');
        vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx_slot);

        /* Loop variable */
        int var_slot = ctx_find_local(ctx, var_name);
        if (var_slot < 0) var_slot = ctx_add_local(ctx, var_name, '$');

        LoopCtx lctx = {0};
        lctx.parent = ctx->loop_ctx;
        lctx.local_restore_depth = ctx->local_restore_count;
        const char *felabel = ast_str(node, "label");
        if (felabel[0]) lctx.label = felabel;
        ctx->loop_ctx = &lctx;

        size_t loop_top = ctx->chunk->code_len;

        /* Condition: $i < size(@arr) */
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx_slot);
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)arr_slot);
        vm_chunk_emit(ctx->chunk, OP_ARRAY_SIZE);
        vm_chunk_emit(ctx->chunk, OP_LT);
        vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
        size_t exit_patch = emit_patch_u16(ctx);

        /* $var = $arr[$i] */
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)arr_slot);
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx_slot);
        vm_chunk_emit(ctx->chunk, OP_ARRAY_GET);
        vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)var_slot);

        /* redo jumps here — body start */
        lctx.redo_target = ctx->chunk->code_len;

        compile_block(ctx, ast_get(node, "body"));

        /* Patch next jumps to increment */
        for (int i = 0; i < lctx.next_count; i++) {
            patch_u16(ctx, lctx.next_patches[i], (uint16_t)ctx->chunk->code_len);
        }

        /* $i++ */
        vm_chunk_emit(ctx->chunk, OP_INCR);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx_slot);

        vm_chunk_emit(ctx->chunk, OP_JMP);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)loop_top);

        patch_u16(ctx, exit_patch, (uint16_t)ctx->chunk->code_len);

        for (int i = 0; i < lctx.break_count; i++) {
            patch_u16(ctx, lctx.break_patches[i], (uint16_t)ctx->chunk->code_len);
        }

        ctx->loop_ctx = lctx.parent;
        break;
    }

    case NI_LAST: {
        const char *label = ast_str(node, "label");
        LoopCtx *target = ctx->loop_ctx;
        if (label[0]) {
            /* Search for labeled loop */
            while (target && (!target->label || strcmp(target->label, label) != 0))
                target = target->parent;
        }
        if (target) {
            emit_try_unwind_for_loop(ctx, target);
            for (int _li = ctx->local_restore_count - 1; _li >= target->local_restore_depth; _li--) {
                size_t kidx = vm_chunk_add_str_const(ctx->chunk, ctx->local_restores[_li]);
                vm_chunk_emit(ctx->chunk, OP_RESTORE_GLOBAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)kidx);
            }
            vm_chunk_emit(ctx->chunk, OP_JMP);
            if (target->break_count < 64) {
                target->break_patches[target->break_count++] = emit_patch_u16(ctx);
            } else {
                emit_patch_u16(ctx);
            }
        }
        break;
    }

    case NI_NEXT: {
        const char *label = ast_str(node, "label");
        LoopCtx *target = ctx->loop_ctx;
        if (label[0]) {
            /* Search for labeled loop */
            while (target && (!target->label || strcmp(target->label, label) != 0))
                target = target->parent;
        }
        if (target) {
            emit_try_unwind_for_loop(ctx, target);
            for (int _li = ctx->local_restore_count - 1; _li >= target->local_restore_depth; _li--) {
                size_t kidx = vm_chunk_add_str_const(ctx->chunk, ctx->local_restores[_li]);
                vm_chunk_emit(ctx->chunk, OP_RESTORE_GLOBAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)kidx);
            }
            vm_chunk_emit(ctx->chunk, OP_JMP);
            /* Forward-patch to continue_target (set after body compilation) */
            if (target->next_count < 64) {
                target->next_patches[target->next_count++] = emit_patch_u16(ctx);
            } else {
                emit_patch_u16(ctx);
            }
        }
        break;
    }

    case NI_REDO: {
        if (ctx->loop_ctx) {
            emit_try_unwind_for_loop(ctx, ctx->loop_ctx);
            for (int _li = ctx->local_restore_count - 1; _li >= ctx->loop_ctx->local_restore_depth; _li--) {
                size_t kidx = vm_chunk_add_str_const(ctx->chunk, ctx->local_restores[_li]);
                vm_chunk_emit(ctx->chunk, OP_RESTORE_GLOBAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)kidx);
            }
            vm_chunk_emit(ctx->chunk, OP_JMP);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)ctx->loop_ctx->redo_target);
        }
        break;
    }

    case NI_LABEL: {
        /* Label: statement — set label on the next loop context */
        const char *label = ast_str(node, "name");
        StradaValue *stmt = ast_get(node, "statement");

        if (stmt && !STRADA_IS_TAGGED_INT(stmt)) {
            /* Set a pending label that the next loop will pick up */
            /* We compile the statement normally — the while/for handler
             * will check ctx->loop_ctx->label after creating the loop context */
            /* Simple approach: set label on the loop context after creation.
             * We do this by temporarily storing the label and having the
             * loop handler check for it. For simplicity, just compile the
             * inner statement and if it creates a loop context, set the label. */
            /* Actually, the loop context is created inside compile_stmt for
             * WHILE/FOR. We can't set the label from here.
             * Solution: pre-create a loop context with the label, and have
             * the loop handler re-use it if present. */

            /* Simpler: detect labeled loops and set label directly */
            StradaValue *inner = ast_deref(stmt);
            int inner_type = inner ? (int)ast_int(inner, "type") : 0;
            if (inner_type == NI_WHILE_STMT || inner_type == NI_FOR_STMT || inner_type == NI_FOREACH_STMT) {
                /* Create a "pending label" by wrapping in a loop context */
                LoopCtx labeled_ctx = {0};
                labeled_ctx.parent = ctx->loop_ctx;
                labeled_ctx.local_restore_depth = ctx->local_restore_count;
                labeled_ctx.label = label;
                /* Record loop top for 'next LABEL' — same as the start of the inner loop */
                labeled_ctx.redo_target = ctx->chunk->code_len;
                ctx->loop_ctx = &labeled_ctx;

                compile_stmt(ctx, stmt);

                /* Patch breaks from this labeled context */
                for (int i = 0; i < labeled_ctx.break_count; i++) {
                    patch_u16(ctx, labeled_ctx.break_patches[i], (uint16_t)ctx->chunk->code_len);
                }
                /* Patch next LABEL — jump to loop top (re-check condition) */
                for (int i = 0; i < labeled_ctx.next_count; i++) {
                    patch_u16(ctx, labeled_ctx.next_patches[i], (uint16_t)labeled_ctx.redo_target);
                }

                ctx->loop_ctx = labeled_ctx.parent;
            } else {
                compile_stmt(ctx, stmt);
            }
        }
        break;
    }

    case NI_SWITCH: {
        /* switch ($expr) { case X { ... } default { ... } }
         * AST: expr, cases (array of exprs), blocks (array of blocks), default_block */
        compile_expr(ctx, ast_get(node, "expr"));
        int val_slot = ctx_add_local(ctx, "__sw_val", '$');
        vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)val_slot);

        StradaValue *cases = ast_get(node, "cases");
        StradaValue *blocks = ast_get(node, "blocks");
        int case_count = (int)ast_int(node, "case_count");
        StradaValue *default_block = ast_get(node, "default_block");

        size_t *end_patches = malloc((size_t)(case_count + 1) * sizeof(size_t));
        int end_count = 0;

        for (int i = 0; i < case_count; i++) {
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)val_slot);
            StradaValue *case_expr = ast_arr_get(cases, i);
            compile_expr(ctx, case_expr);
            /* String cases must compare as strings — OP_EQ is numeric and
             * matches any two non-numeric strings (both convert to 0). */
            int case_type = (case_expr && !STRADA_IS_TAGGED_INT(case_expr))
                                ? (int)ast_int(case_expr, "type") : 0;
            vm_chunk_emit(ctx->chunk, case_type == NI_STR_LITERAL ? OP_STR_EQ : OP_EQ);
            vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
            size_t next_case = emit_patch_u16(ctx);

            compile_block(ctx, ast_arr_get(blocks, i));
            vm_chunk_emit(ctx->chunk, OP_JMP);
            end_patches[end_count++] = emit_patch_u16(ctx);

            patch_u16(ctx, next_case, (uint16_t)ctx->chunk->code_len);
        }

        if (default_block && !STRADA_IS_TAGGED_INT(default_block) &&
            (int)ast_int(node, "has_default")) {
            compile_block(ctx, default_block);
        }

        for (int i = 0; i < end_count; i++) {
            patch_u16(ctx, end_patches[i], (uint16_t)ctx->chunk->code_len);
        }
        free(end_patches);
        break;
    }

    case NI_TRY_CATCH: {
        /* try { ... } [catch ([Type] $e) { ... }]* [finally { ... }] */
        StradaValue *try_body = ast_get(node, "try_block");
        StradaValue *catches = ast_get(node, "catch_clauses");
        int catch_count = (int)ast_int(node, "catch_count");
        StradaValue *finally_block = ast_get(node, "finally_block");
        int has_finally = (finally_block && !STRADA_IS_TAGGED_INT(finally_block)) ? 1 : 0;

        TryCtx tctx = {0};
        tctx.finally_block = has_finally ? finally_block : NULL;
        tctx.enclosing_loop = ctx->loop_ctx;
        tctx.parent = ctx->try_ctx;

        size_t finally_rethrow_patch = 0;
        size_t catch_patch = 0;

        if (has_finally && catch_count > 0) {
            /* Outer handler catches any rethrown or catch-body exception to run finally */
            vm_chunk_emit(ctx->chunk, OP_TRY_BEGIN);
            finally_rethrow_patch = emit_patch_u16(ctx);
            vm_chunk_emit(ctx->chunk, OP_TRY_BEGIN);
            catch_patch = emit_patch_u16(ctx);
            tctx.handlers_active = 2;
        } else if (has_finally && catch_count == 0) {
            /* try { ... } finally { ... } with no catch clauses */
            vm_chunk_emit(ctx->chunk, OP_TRY_BEGIN);
            finally_rethrow_patch = emit_patch_u16(ctx);
            tctx.handlers_active = 1;
        } else {
            vm_chunk_emit(ctx->chunk, OP_TRY_BEGIN);
            catch_patch = emit_patch_u16(ctx);
            tctx.handlers_active = 1;
        }

        ctx->try_ctx = &tctx;
        compile_block(ctx, try_body);

        for (int h = 0; h < tctx.handlers_active; h++) {
            vm_chunk_emit(ctx->chunk, OP_TRY_END);
        }

        /* Jump over catch/rethrow handlers to normal finally / end */
        vm_chunk_emit(ctx->chunk, OP_JMP);
        size_t normal_end_patch = emit_patch_u16(ctx);

        size_t catch_end_patches[64];
        int catch_end_count = 0;

        if (catch_count > 0) {
            patch_u16(ctx, catch_patch, (uint16_t)ctx->chunk->code_len);
            /* Inside catch blocks: inner try handler was popped by OP_THROW;
             * outer finally_rethrow handler remains active when has_finally. */
            tctx.handlers_active = has_finally ? 1 : 0;

            int had_catch_all = 0;
            for (int i = 0; i < catch_count; i++) {
                StradaValue *cc = ast_arr_get(catches, i);
                const char *var_name = ast_str(cc, "catch_var");
                const char *type_name = ast_str(cc, "catch_type");

                if (type_name[0]) {
                    /* Typed catch — ALWAYS check OP_ISA, including the last/only clause! */
                    vm_chunk_emit(ctx->chunk, OP_DUP);
                    size_t tn_idx = vm_chunk_add_str_const(ctx->chunk, type_name);
                    vm_chunk_emit(ctx->chunk, OP_PUSH_STR);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)tn_idx);
                    vm_chunk_emit(ctx->chunk, OP_ISA);
                    vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
                    size_t next_catch = emit_patch_u16(ctx);

                    int slot = ctx_add_local(ctx, var_name[0] ? var_name : "__exc", '$');
                    vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);

                    compile_block(ctx, ast_get(cc, "catch_block"));

                    if (has_finally) {
                        vm_chunk_emit(ctx->chunk, OP_TRY_END);
                    }
                    vm_chunk_emit(ctx->chunk, OP_JMP);
                    if (catch_end_count < 64)
                        catch_end_patches[catch_end_count++] = emit_patch_u16(ctx);
                    else
                        emit_patch_u16(ctx);

                    patch_u16(ctx, next_catch, (uint16_t)ctx->chunk->code_len);
                } else {
                    /* Untyped catch-all */
                    int slot = ctx_add_local(ctx, var_name[0] ? var_name : "__exc", '$');
                    vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);

                    compile_block(ctx, ast_get(cc, "catch_block"));

                    if (has_finally) {
                        vm_chunk_emit(ctx->chunk, OP_TRY_END);
                    }
                    vm_chunk_emit(ctx->chunk, OP_JMP);
                    if (catch_end_count < 64)
                        catch_end_patches[catch_end_count++] = emit_patch_u16(ctx);
                    else
                        emit_patch_u16(ctx);

                    had_catch_all = 1;
                    break;
                }
            }

            if (!had_catch_all) {
                /* None of the typed catch clauses matched — re-throw exception on stack
                 * (lands in finally_rethrow_patch if has_finally, else outer handler) */
                vm_chunk_emit(ctx->chunk, OP_THROW);
            }
        }

        ctx->try_ctx = tctx.parent;

        if (has_finally) {
            /* Exception unwind path: save pending exception, run finally, re-throw */
            patch_u16(ctx, finally_rethrow_patch, (uint16_t)ctx->chunk->code_len);
            int fin_exc_slot = ctx_add_local(ctx, "__fin_exc", '$');
            vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)fin_exc_slot);
            compile_block(ctx, finally_block);
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)fin_exc_slot);
            vm_chunk_emit(ctx->chunk, OP_THROW);
        }

        /* Normal exit path (try completed or catch handled): run finally if present */
        patch_u16(ctx, normal_end_patch, (uint16_t)ctx->chunk->code_len);
        for (int i = 0; i < catch_end_count; i++) {
            patch_u16(ctx, catch_end_patches[i], (uint16_t)ctx->chunk->code_len);
        }
        if (has_finally) {
            compile_block(ctx, finally_block);
        }
        break;
    }

    case NI_THROW: {
        StradaValue *expr = ast_get(node, "expr");
        if (!expr || STRADA_IS_TAGGED_INT(expr)) expr = ast_get(node, "value");
        compile_expr(ctx, expr);
        vm_chunk_emit(ctx->chunk, OP_THROW);
        break;
    }

    case NI_TR: {
        /* $var =~ tr/search/replace/flags — statement form: discard count */
        compile_tr(ctx, node);
        vm_chunk_emit(ctx->chunk, OP_POP);
        break;
    }

    case NI_DESTRUCTURE: {
        /* my ($a, $b, $c) = @array or = func() */
        StradaValue *vars = ast_get(node, "vars");
        int var_count = (int)ast_int(node, "var_count");
        StradaValue *source = ast_get(node, "init");

        /* Compile source expression */
        compile_expr(ctx, source);
        int src_slot = ctx_add_local(ctx, "__destr_src", '$');
        vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)src_slot);

        /* Extract each variable */
        for (int i = 0; i < var_count; i++) {
            StradaValue *var = ast_arr_get(vars, i);
            const char *vname = ast_str(var, "name");
            int slot = ctx_add_local(ctx, vname, '$');

            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)src_slot);
            vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
            vm_chunk_emit_i64(ctx->chunk, i);
            vm_chunk_emit(ctx->chunk, OP_ARRAY_GET);
            vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
        }
        break;
    }

    case NI_REGEX_SUBST: {
        StradaValue *target = ast_get(node, "target");
        const char *pattern = ast_str(node, "pattern");
        const char *replacement = ast_str(node, "replacement");
        const char *flags = ast_str(node, "flags");
        int is_global_flag = (flags && strchr(flags, 'g'));

        /* s///e: evaluated replacement (statement form — discard value) */
        if (flags && strchr(flags, 'e') && compile_regex_subst_e(ctx, node)) {
            vm_chunk_emit(ctx->chunk, OP_POP);
            break;
        }

        if (target && !STRADA_IS_TAGGED_INT(target) &&
            (int)ast_int(target, "type") == NI_VARIABLE) {
            const char *vname = ast_str(target, "name");
            const char *vsigil = ast_str(target, "sigil");
            int slot = ctx_find_local_sigil(ctx, vname, vsigil[0]);
            int is_gvar = (slot < 0 && is_our_var(vname)) ? 1 : 0;

            if (slot >= 0 || is_gvar) {
                /* Load the target string */
                if (slot >= 0) {
                    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                } else {
                    size_t gk = vm_chunk_add_str_const(ctx->chunk, vname);
                    vm_chunk_emit(ctx->chunk, OP_LOAD_GLOBAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)gk);
                }

                size_t pat_idx = add_pattern_const(ctx, pattern, flags);
                vm_chunk_emit(ctx->chunk, OP_PUSH_STR);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)pat_idx);

                size_t repl_idx = vm_chunk_add_str_const(ctx->chunk, replacement);
                vm_chunk_emit(ctx->chunk, OP_PUSH_STR);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)repl_idx);

                if (is_global_flag) {
                    vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                    vm_chunk_emit_u16(ctx->chunk, BUILTIN_REPLACE_ALL);
                    vm_chunk_emit(ctx->chunk, 3);
                } else {
                    vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                    vm_chunk_emit_u16(ctx->chunk, BUILTIN_REPLACE);
                    vm_chunk_emit(ctx->chunk, 3);
                }

                /* Store result back */
                if (slot >= 0) {
                    vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                } else {
                    size_t gk = vm_chunk_add_str_const(ctx->chunk, vname);
                    vm_chunk_emit(ctx->chunk, OP_STORE_GLOBAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)gk);
                }
            }
        }
        break;
    }

    case NI_BLOCK: {
        /* Bare block { ... } — introduces a new scope */
        int saved_local_count = ctx->local_count;
        compile_block(ctx, node);
        ctx->local_count = saved_local_count;
        break;
    }

    case NI_MAP: {
        compile_map_expr(ctx, node);
        vm_chunk_emit(ctx->chunk, OP_POP);
        break;
    }

    case NI_SORT: {
        compile_sort_expr(ctx, node);
        vm_chunk_emit(ctx->chunk, OP_POP);
        break;
    }

    case NI_GREP: {
        compile_grep_expr(ctx, node);
        vm_chunk_emit(ctx->chunk, OP_POP);
        break;
    }

    case NI_ASSIGN:
    case NI_INCREMENT:
    case NI_CALL:
    case NI_METHOD_CALL:
    case NI_SUPER_CALL:
    case NI_CLOSURE_CALL:
    case NI_DYN_METHOD_CALL:
    case NI_REGEX_MATCH:
        compile_expr(ctx, node);
        vm_chunk_emit(ctx->chunk, OP_POP);
        break;

    case NI_C_BLOCK: {
        /* __C__ { ... } — register the C code for JIT compilation at runtime */
        const char *code = ast_str(node, "code");
        if (code && strlen(code) > 0) {
            /* Add C block to program with current local variable bindings */
            VMProgram *prog = ctx->prog;
            if (!prog->cblocks) {
                prog->cblock_cap = 8;
                prog->cblocks = calloc(prog->cblock_cap, sizeof(VMCBlock));
            }
            if (prog->cblock_count >= prog->cblock_cap) {
                prog->cblock_cap *= 2;
                prog->cblocks = realloc(prog->cblocks, prog->cblock_cap * sizeof(VMCBlock));
            }
            int idx = prog->cblock_count++;
            prog->cblocks[idx].code = strdup(code);
            prog->cblocks[idx].var_names = calloc(ctx->local_count, sizeof(char*));
            prog->cblocks[idx].var_slots = calloc(ctx->local_count, sizeof(int));
            /* Dedupe by name (last slot wins — innermost shadowing decl):
             * the JIT wrapper declares one C variable per entry, so two
             * locals sharing a name would be a C redefinition error. */
            int vc = 0;
            for (int vi = 0; vi < ctx->local_count; vi++) {
                int later = 0;
                for (int vj = vi + 1; vj < ctx->local_count; vj++)
                    if (strcmp(ctx->local_names[vi], ctx->local_names[vj]) == 0) { later = 1; break; }
                if (later) continue;
                prog->cblocks[idx].var_names[vc] = strdup(ctx->local_names[vi]);
                prog->cblocks[idx].var_slots[vc] = vi;
                vc++;
            }
            prog->cblocks[idx].var_count = vc;
            prog->cblocks[idx].dl_handle = NULL;
            prog->cblocks[idx].fn = NULL;

            vm_chunk_emit(ctx->chunk, OP_C_BLOCK);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx);
        }
        break;
    }

    default:
        /* fprintf(stderr, "VM compile: unhandled statement node type %d\n", type); */
        break;
    }
}

static void compile_block(CompCtx *ctx, StradaValue *block) {
    if (!block || STRADA_IS_TAGGED_INT(block)) return;
    block = ast_deref(block);
    if (!block || STRADA_IS_TAGGED_INT(block)) return;

    StradaValue *stmts = ast_get(block, "statements");
    int count = (int)ast_int(block, "statement_count");

    int saved_lr = ctx->local_restore_count;
    for (int i = 0; i < count; i++) {
        compile_stmt(ctx, ast_arr_get(stmts, i));
    }
    while (ctx->local_restore_count > saved_lr) {
        ctx->local_restore_count--;
        char *lr_name = ctx->local_restores[ctx->local_restore_count];
        ctx->local_restores[ctx->local_restore_count] = NULL;
        size_t key_idx = vm_chunk_add_str_const(ctx->chunk, lr_name);
        vm_chunk_emit(ctx->chunk, OP_RESTORE_GLOBAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)key_idx);
        free(lr_name);
    }
}

/* ===== Function compilation ===== */

static int compile_function(VMProgram *prog, StradaValue *func_node) {
    const char *name = ast_str(func_node, "name");
    /* Check if function was pre-registered; if so, reuse that slot */
    int existing_idx = vm_program_find_func(prog, name);
    VMChunk *chunk;
    if (existing_idx >= 0) {
        chunk = &prog->funcs[existing_idx];
        /* Reset the chunk for recompilation */
        char *saved_name = chunk->name;
        free(chunk->code);
        free(chunk->int_consts);
        for (size_t ci = 0; ci < chunk->str_const_count; ci++) free(chunk->str_consts[ci]);
        free(chunk->str_consts);
        for (int ci = 0; ci < chunk->capture_count; ci++) free(chunk->capture_names[ci]);
        free(chunk->capture_names);
        memset(chunk, 0, sizeof(VMChunk));
        chunk->name = saved_name;
        chunk->code_cap = 256;
        chunk->code = malloc(chunk->code_cap);
    } else {
        chunk = vm_program_add_func(prog, name);
    }

    CompCtx ctx = {0};
    ctx.chunk = chunk;
    ctx.prog = prog;
    ctx.func_name = name;
    ctx.cur_package = ast_str(func_node, "package");

    /* Add parameters as locals, detect variadic */
    chunk->return_type = (int)ast_int(func_node, "return_type");
    int is_synth = (int)ast_int(func_node, "is_synthesized");
    StradaValue *params = ast_get(func_node, "params");
    int param_count = (int)ast_int(func_node, "param_count");
    int fixed_params = 0;
    for (int i = 0; i < param_count; i++) {
        StradaValue *param = ast_arr_get(params, i);
        const char *pname = ast_str(param, "name");
        const char *psigil = ast_str(param, "sigil");
        int is_variadic = (int)ast_int(param, "is_variadic");
        ctx_add_local(&ctx, pname, psigil[0] ? psigil[0] : '$');
        if (is_variadic || (is_synth && psigil[0] == '@')) {
            chunk->has_variadic = 1;
        } else {
            fixed_params++;
        }
    }
    chunk->fixed_param_count = fixed_params;

    compile_block(&ctx, ast_get(func_node, "body"));

    /* Refresh chunk pointer — may have been invalidated by realloc during
     * closure compilation inside compile_block */
    chunk = ctx.chunk;

    /* Emit local restores before implicit return */
    for (int i = ctx.local_restore_count - 1; i >= 0; i--) {
        size_t key_idx = vm_chunk_add_str_const(chunk, ctx.local_restores[i]);
        vm_chunk_emit(chunk, OP_RESTORE_GLOBAL);
        vm_chunk_emit_u16(chunk, (uint16_t)key_idx);
    }

    /* Implicit return 0 at end */
    vm_chunk_emit(chunk, OP_PUSH_INT);
    vm_chunk_emit_i64(chunk, 0);
    vm_chunk_emit(chunk, OP_RETURN);

    /* Guard against 16-bit jump-offset truncation: every jump in this function
     * is patched with a u16 offset into chunk->code (patch_u16 casts code_len to
     * uint16_t). If the function's bytecode exceeds 65535 bytes those patches
     * silently wrap and the VM jumps to the wrong address. Since all jump
     * targets are <= the final code_len, this single check covers them all. */
    if (chunk->code_len > 0xFFFF) {
        fprintf(stderr, "VM compile error: function '%s' compiles to %zu bytes of bytecode, "
                        "exceeding the VM backend's 65535-byte limit (16-bit jump offsets)\n",
                name ? name : "(anon)", (size_t)chunk->code_len);
        exit(1);
    }

    chunk->local_count = ctx.local_count;

    /* Detect int-only functions */
    int all_int = 1;
    for (int i = 0; i < ctx.local_count; i++) {
        if (ctx.local_sigils[i] != '$' && ctx.local_sigils[i] != '\0') {
            all_int = 0;
        }
        free(ctx.local_names[i]);
    }
    for (int i = 0; i < ctx.capture_count; i++) free(ctx.captures[i].name);
    for (int i = 0; i < ctx.local_restore_count; i++) free(ctx.local_restores[i]);
    chunk->int_only = all_int;
    return ctx.errors;
}

/* ===== Process OOP declarations ===== */

static void process_oop_decls(VMProgram *prog, StradaValue *ast) {
    StradaValue *packages = ast_get(ast, "packages");
    if (!packages) return;
    packages = ast_deref(packages);
    if (!packages || STRADA_IS_TAGGED_INT(packages) || packages->type != STRADA_ARRAY) return;

    int pkg_count = (int)packages->value.av->size;
    for (int p = 0; p < pkg_count; p++) {
        StradaValue *pkg = ast_deref(strada_array_get(packages->value.av, p));
        if (!pkg || STRADA_IS_TAGGED_INT(pkg) || pkg->type != STRADA_HASH) continue;

        const char *pkg_name = ast_str(pkg, "name");

        /* Process 'has' attributes */
        StradaValue *attrs = ast_get(pkg, "attributes");
        if (attrs) {
            attrs = ast_deref(attrs);
            if (attrs && !STRADA_IS_TAGGED_INT(attrs) && attrs->type == STRADA_ARRAY) {
                int attr_count = attrs->value.av->size;
                for (int a = 0; a < attr_count; a++) {
                    StradaValue *attr = ast_deref(strada_array_get(attrs->value.av, a));
                    if (!attr || STRADA_IS_TAGGED_INT(attr) || attr->type != STRADA_HASH) continue;
                    const char *aname = ast_str(attr, "name");
                    const char *atype = ast_str(attr, "type");
                    int is_rw = (int)ast_int(attr, "is_rw");
                    int is_required = (int)ast_int(attr, "is_required");
                    vm_program_add_attr(prog, pkg_name, aname, atype, is_rw, is_required, VM_UNDEF_VAL);
                }
            }
        }

        /* Process overloads */
        StradaValue *overloads = ast_get(pkg, "overloads");
        if (overloads) {
            overloads = ast_deref(overloads);
            if (overloads && !STRADA_IS_TAGGED_INT(overloads) && overloads->type == STRADA_ARRAY) {
                int ov_count = overloads->value.av->size;
                for (int o = 0; o < ov_count; o++) {
                    StradaValue *ov = ast_deref(strada_array_get(overloads->value.av, o));
                    if (!ov || STRADA_IS_TAGGED_INT(ov) || ov->type != STRADA_HASH) continue;
                    const char *op = ast_str(ov, "op");
                    const char *method = ast_str(ov, "method");
                    vm_program_add_overload(prog, pkg_name, op, method);
                }
            }
        }

        /* Process modifiers (before/after/around) */
        StradaValue *modifiers = ast_get(pkg, "modifiers");
        if (modifiers) {
            modifiers = ast_deref(modifiers);
            if (modifiers && !STRADA_IS_TAGGED_INT(modifiers) && modifiers->type == STRADA_ARRAY) {
                int mod_count = modifiers->value.av->size;
                for (int m = 0; m < mod_count; m++) {
                    StradaValue *mod = ast_deref(strada_array_get(modifiers->value.av, m));
                    if (!mod || STRADA_IS_TAGGED_INT(mod) || mod->type != STRADA_HASH) continue;
                    const char *method = ast_str(mod, "method");
                    const char *mod_func = ast_str(mod, "func_name");
                    int kind = (int)ast_int(mod, "kind");
                    vm_program_add_modifier(prog, pkg_name, method, mod_func, kind);
                }
            }
        }
    }
}

/* ===== import_lib processing ===== */

static int process_import_libs(VMProgram *prog, StradaValue *ast) {
    StradaValue *import_libs = ast_get(ast, "import_libs");
    int import_count = (int)ast_int(ast, "import_lib_count");
    int errors = 0;
    if (import_count <= 0 || !import_libs) return 0;

    import_libs = ast_deref(import_libs);
    if (!import_libs || STRADA_IS_TAGGED_INT(import_libs) || import_libs->type != STRADA_ARRAY) return 0;

    for (int li = 0; li < import_count; li++) {
        StradaValue *lib_info = ast_deref(strada_array_get(import_libs->value.av, li));
        if (!lib_info || STRADA_IS_TAGGED_INT(lib_info) || lib_info->type != STRADA_HASH) continue;

        const char *so_path = ast_str(lib_info, "so_path");
        const char *lib_name = ast_str(lib_info, "lib_name");
        if (!so_path[0]) continue;

        /* fprintf(stderr, "DBG: loading %s (lib=%s)\n", so_path, lib_name); */
        void *dl = dlopen(so_path, RTLD_NOW | RTLD_GLOBAL);
        if (!dl) {
            fprintf(stderr, "VM: failed to load %s: %s\n", so_path, dlerror());
            errors++;
            continue;
        }

        if (prog->dl_handle_count >= prog->dl_handle_cap) {
            int new_cap = prog->dl_handle_cap ? prog->dl_handle_cap * 2 : 8;
            void **new_handles = realloc(prog->dl_handles, (size_t)new_cap * sizeof(void *));
            if (new_handles) {
                prog->dl_handles = new_handles;
                prog->dl_handle_cap = new_cap;
            }
        }
        if (prog->dl_handle_count < prog->dl_handle_cap) {
            prog->dl_handles[prog->dl_handle_count++] = dl;
        }

        /* Call OOP init functions if present (with fallback to __<lib_name>_oop_init) */
        StradaValue *oop_arr = ast_deref(ast_get(lib_info, "oop_init_funcs"));
        int ran_oop = 0;
        if (oop_arr && !STRADA_IS_TAGGED_INT(oop_arr) && oop_arr->type == STRADA_ARRAY) {
            for (size_t oi = 0; oi < oop_arr->value.av->size; oi++) {
                StradaValue *fn_sv = ast_deref(strada_array_get(oop_arr->value.av, oi));
                if (fn_sv && !STRADA_IS_TAGGED_INT(fn_sv) && fn_sv->type == STRADA_STR &&
                    fn_sv->value.pv && fn_sv->value.pv[0]) {
                    void (*oop_init)(void) = dlsym(dl, fn_sv->value.pv);
                    if (oop_init) oop_init();
                    ran_oop = 1;
                }
            }
        }
        if (!ran_oop && lib_name[0]) {
            char oop_init_name[256];
            snprintf(oop_init_name, sizeof(oop_init_name), "__%s_oop_init", lib_name);
            void (*oop_init)(void) = dlsym(dl, oop_init_name);
            if (oop_init) oop_init();
        }

        /* Get exported functions from AST metadata */
        StradaValue *fn_arr = ast_get(lib_info, "functions");
        int fn_count = (int)ast_int(lib_info, "function_count");
        if (!fn_arr || fn_count <= 0) continue;
        fn_arr = ast_deref(fn_arr);
        if (!fn_arr || STRADA_IS_TAGGED_INT(fn_arr) || fn_arr->type != STRADA_ARRAY) continue;

        /* Ensure native array capacity */
        if (!prog->natives) {
            prog->native_cap = 32;
            prog->natives = calloc(prog->native_cap, sizeof(VMNativeEntry));
        }

        for (int fi = 0; fi < fn_count; fi++) {
            StradaValue *fn_info = ast_deref(strada_array_get(fn_arr->value.av, fi));
            if (!fn_info || STRADA_IS_TAGGED_INT(fn_info) || fn_info->type != STRADA_HASH) continue;

            const char *fn_name = ast_str(fn_info, "name");
            if (!fn_name[0]) continue;

            /* Look up the C symbol: try LibName_funcname first, then funcname */
            char sym_name[256];
            void *sym = NULL;
            snprintf(sym_name, sizeof(sym_name), "%s_%s", lib_name, fn_name);
            sym = dlsym(dl, sym_name);
            if (!sym) {
                /* Try without prefix */
                sym = dlsym(dl, fn_name);
            }
            if (!sym) {
                /* Try LibName__LibName_funcname (double prefix from OOP) */
                snprintf(sym_name, sizeof(sym_name), "%s__%s_%s", lib_name, lib_name, fn_name);
                sym = dlsym(dl, sym_name);
            }
            /* fprintf(stderr, "DBG: fn=%s sym=%p\n", fn_name, sym); */
            if (!sym) {
                fprintf(stderr, "VM: missing native symbol '%s' in %s\n", fn_name, so_path);
                errors++;
                continue;
            }

            /* Register the native function */
            if (prog->native_count >= prog->native_cap) {
                prog->native_cap *= 2;
                prog->natives = realloc(prog->natives, prog->native_cap * sizeof(VMNativeEntry));
            }
            VMNativeEntry *ne = &prog->natives[prog->native_count++];
            /* fn_name is already "LibName_funcname" from the AST metadata */
            ne->name = strdup(fn_name);
            ne->fn = NULL; /* we use sym directly */
            ne->dl_handle = dl;
            ne->sym = sym;
            ne->param_count = (int)ast_int(fn_info, "param_count");
            ne->is_variadic = (int)ast_int(fn_info, "is_variadic");
            ne->return_type = (int)ast_int(fn_info, "return_type");
        }
    }
    return errors;
}

/* Find a native function by name */
static VMNativeEntry *vm_find_native(VMProgram *prog, const char *name) {
    for (int i = 0; i < prog->native_count; i++) {
        if (strcmp(prog->natives[i].name, name) == 0)
            return &prog->natives[i];
    }
    return NULL;
}

/* ===== Program compilation ===== */


/* Synthesize before/after modifier wrappers. The dispatch loop cannot run a
 * hook function to completion mid-opcode (vm_execute is not re-entrant), so
 * instead each (class, method) with before/after modifiers gets a wrapper
 * chunk under the method's dispatch name: it calls every before-hook with
 * $self, then the (renamed) original with all args, then every after-hook,
 * and returns the original's result. `around` keeps its existing
 * closure-based dispatch path. Variadic originals are skipped. */
static void synthesize_modifier_wrappers(VMProgram *prog) {
    int mod_total = prog->modifier_count;
    for (int m = 0; m < mod_total; m++) {
        VMModifier *mod = &prog->modifiers[m];
        if (mod->kind == 2) continue; /* around: handled at dispatch */

        char orig_name[256];
        snprintf(orig_name, sizeof(orig_name), "%s_%s", mod->class_name, mod->method);
        int orig = vm_program_find_func(prog, orig_name);
        int inherited_orig = 0;
        if (orig < 0) {
            /* Method may be inherited from a parent class */
            const char *pcls = vm_program_find_parent(prog, mod->class_name);
            while (pcls && orig < 0) {
                char pname[256];
                snprintf(pname, sizeof(pname), "%s_%s", pcls, mod->method);
                orig = vm_program_find_func(prog, pname);
                pcls = vm_program_find_parent(prog, pcls);
            }
            if (orig < 0) continue;
            inherited_orig = 1;
        }
        /* Already wrapped for this (class, method)? (first modifier wins
         * the wrapping; the wrapper emits ALL kind-0/1 modifiers) */
        char hidden_name[280];
        snprintf(hidden_name, sizeof(hidden_name), "%s__premod", orig_name);
        if (vm_program_find_func(prog, hidden_name) >= 0) continue;
        if (prog->funcs[orig].has_variadic) continue;

        int nparams = prog->funcs[orig].fixed_param_count;
        if (nparams <= 0) nparams = 1; /* methods always receive $self */

        if (!inherited_orig) {
            /* Rename the original out of the dispatch name */
            free(prog->func_names[orig]);
            prog->func_names[orig] = strdup(hidden_name);
        } else {
            /* Record marker chunk so a second modifier on (class, method) sees it wrapped */
            vm_program_add_func(prog, hidden_name);
        }

        VMChunk *w = vm_program_add_func(prog, orig_name);
        w->fixed_param_count = nparams;
        w->local_count = nparams + 1; /* + result slot */

        /* before hooks (registration order) */
        for (int k = 0; k < mod_total; k++) {
            VMModifier *bm = &prog->modifiers[k];
            if (bm->kind != 0 || strcmp(bm->method, mod->method) != 0 ||
                strcmp(bm->class_name, mod->class_name) != 0) continue;
            int bidx = vm_program_find_func(prog, bm->modifier_func);
            if (bidx < 0) continue;
            int bargc = prog->funcs[bidx].fixed_param_count;
            if (bargc <= 0) bargc = 1;
            if (bargc > nparams) bargc = nparams;
            for (int ba = 0; ba < bargc; ba++) {
                vm_chunk_emit(w, OP_LOAD_LOCAL); vm_chunk_emit_u16(w, (uint16_t)ba);
            }
            vm_chunk_emit(w, OP_CALL); vm_chunk_emit_u16(w, (uint16_t)bidx); vm_chunk_emit(w, (uint8_t)bargc);
            vm_chunk_emit(w, OP_POP);
        }

        /* original */
        for (int a = 0; a < nparams; a++) {
            vm_chunk_emit(w, OP_LOAD_LOCAL); vm_chunk_emit_u16(w, (uint16_t)a);
        }
        vm_chunk_emit(w, OP_CALL); vm_chunk_emit_u16(w, (uint16_t)orig); vm_chunk_emit(w, (uint8_t)nparams);
        vm_chunk_emit(w, OP_STORE_LOCAL); vm_chunk_emit_u16(w, (uint16_t)nparams);

        /* after hooks (registration order) */
        for (int k = 0; k < mod_total; k++) {
            VMModifier *am = &prog->modifiers[k];
            if (am->kind != 1 || strcmp(am->method, mod->method) != 0 ||
                strcmp(am->class_name, mod->class_name) != 0) continue;
            int aidx = vm_program_find_func(prog, am->modifier_func);
            if (aidx < 0) continue;
            int aargc = prog->funcs[aidx].fixed_param_count;
            if (aargc <= 0) aargc = 1;
            if (aargc > nparams) aargc = nparams;
            for (int aa = 0; aa < aargc; aa++) {
                vm_chunk_emit(w, OP_LOAD_LOCAL); vm_chunk_emit_u16(w, (uint16_t)aa);
            }
            vm_chunk_emit(w, OP_CALL); vm_chunk_emit_u16(w, (uint16_t)aidx); vm_chunk_emit(w, (uint8_t)aargc);
            vm_chunk_emit(w, OP_POP);
        }

        vm_chunk_emit(w, OP_LOAD_LOCAL); vm_chunk_emit_u16(w, (uint16_t)nparams);
        vm_chunk_emit(w, OP_RETURN);
    }
}

VMProgram *vm_compile_program(StradaValue *ast) {
    reset_compiler_globals();
    VMProgram *prog = vm_program_new();
    VM *saved_vm = NULL;
    vm_gc_set_compiling_prog(prog, &saved_vm);

    StradaValue *funcs = ast_get(ast, "functions");
    int func_count = (int)ast_int(ast, "function_count");

    StradaValue *inherits = ast_get(ast, "inherits");

    int total_errors = 0;

    /* Process import_lib declarations — must be done before function compilation
     * so that calls to imported functions can be resolved */
    total_errors += process_import_libs(prog, ast);

    /* Process global (our) declarations */
    StradaValue *globals = ast_get(ast, "globals");
    int global_count = (int)ast_int(ast, "global_count");
    if (global_count > 0) {
        /* Create an init function for globals */
        char iname[] = "__globals_init";
        VMChunk *ichunk = vm_program_add_func(prog, iname);
        CompCtx ictx = {0};
        ictx.chunk = ichunk;
        ictx.prog = prog;
        ictx.func_name = iname;
        for (int i = 0; i < global_count; i++) {
            StradaValue *gdecl = ast_arr_get(globals, i);
            /* File-scope `my` decls share the our-global machinery in the
             * VM: BEGIN/END blocks and functions compile as separate chunks
             * and can only reach them through the global registry (compiled
             * C hoists these to file-scope statics). Retag as our-decl. */
            StradaValue *gnode = ast_deref(gdecl);
            if (gnode && !STRADA_IS_TAGGED_INT(gnode) && gnode->type == STRADA_HASH &&
                (int)ast_int(gnode, "type") == NI_VAR_DECL) {
                strada_hash_set_take(gnode->value.hv, "type",
                                     strada_new_int(NI_OUR_DECL));
            }
            compile_stmt(&ictx, gdecl);
        }
        ichunk = ictx.chunk; /* refresh after potential realloc */
        vm_chunk_emit(ichunk, OP_PUSH_INT);
        vm_chunk_emit_i64(ichunk, 0);
        vm_chunk_emit(ichunk, OP_RETURN);
        ichunk->local_count = ictx.local_count;
        for (int j = 0; j < ictx.local_count; j++) free(ictx.local_names[j]);

        int fidx = vm_program_find_func(prog, iname);
        /* Add as a BEGIN block so it runs first */
        prog->begin_blocks = realloc(prog->begin_blocks, (prog->begin_count + 1) * sizeof(int));
        prog->begin_blocks[prog->begin_count++] = fidx;
    }

    /* Compile BEGIN blocks */
    StradaValue *begin_blocks = ast_get(ast, "begin_blocks");
    int begin_count = (int)ast_int(ast, "begin_block_count");
    for (int i = 0; i < begin_count; i++) {
        StradaValue *block = ast_arr_get(begin_blocks, i);
        char bname[64];
        snprintf(bname, sizeof(bname), "__begin_%d", i);
        VMChunk *bchunk = vm_program_add_func(prog, bname);
        CompCtx bctx = {0};
        bctx.chunk = bchunk;
        bctx.prog = prog;
        bctx.func_name = bname;
        compile_block(&bctx, block);
        vm_chunk_emit(bchunk, OP_PUSH_INT);
        vm_chunk_emit_i64(bchunk, 0);
        vm_chunk_emit(bchunk, OP_RETURN);
        bchunk->local_count = bctx.local_count;
        for (int j = 0; j < bctx.local_count; j++) free(bctx.local_names[j]);

        int fidx = vm_program_find_func(prog, bname);
        prog->begin_blocks = realloc(prog->begin_blocks, (prog->begin_count + 1) * sizeof(int));
        prog->begin_blocks[prog->begin_count++] = fidx;
    }

    /* Compile END blocks */
    StradaValue *end_blocks_arr = ast_get(ast, "end_blocks");
    int end_count = (int)ast_int(ast, "end_block_count");
    for (int i = 0; i < end_count; i++) {
        StradaValue *block = ast_arr_get(end_blocks_arr, i);
        char ename[64];
        snprintf(ename, sizeof(ename), "__end_%d", i);
        VMChunk *echunk = vm_program_add_func(prog, ename);
        CompCtx ectx = {0};
        ectx.chunk = echunk;
        ectx.prog = prog;
        ectx.func_name = ename;
        compile_block(&ectx, block);
        vm_chunk_emit(echunk, OP_PUSH_INT);
        vm_chunk_emit_i64(echunk, 0);
        vm_chunk_emit(echunk, OP_RETURN);
        echunk->local_count = ectx.local_count;
        for (int j = 0; j < ectx.local_count; j++) free(ectx.local_names[j]);

        int fidx = vm_program_find_func(prog, ename);
        prog->end_blocks = realloc(prog->end_blocks, (prog->end_count + 1) * sizeof(int));
        prog->end_blocks[prog->end_count++] = fidx;
    }

    /* First pass: register all function names and signatures (for forward references) */
    for (int i = 0; i < func_count; i++) {
        StradaValue *func = ast_arr_get(funcs, i);
        const char *fname = ast_str(func, "name");
        int existing = vm_program_find_func(prog, fname);
        VMChunk *fc = (existing < 0) ? vm_program_add_func(prog, fname) : &prog->funcs[existing];
        fc->return_type = (int)ast_int(func, "return_type");
        int is_synth = (int)ast_int(func, "is_synthesized");
        StradaValue *fparams = ast_get(func, "params");
        int fparam_count = (int)ast_int(func, "param_count");
        int ffixed = 0;
        for (int pi = 0; pi < fparam_count; pi++) {
            StradaValue *fp = ast_arr_get(fparams, pi);
            const char *psig = ast_str(fp, "sigil");
            int is_va = (int)ast_int(fp, "is_variadic");
            if (is_va || (is_synth && psig[0] == '@')) fc->has_variadic = 1;
            else ffixed++;
        }
        fc->fixed_param_count = ffixed;
    }

    /* Second pass: compile all non-main functions */
    for (int i = 0; i < func_count; i++) {
        StradaValue *func = ast_arr_get(funcs, i);
        const char *fname = ast_str(func, "name");

        if (strcmp(fname, "main") != 0) {
            total_errors += compile_function(prog, func);
        }
    }

    /* Compile main last */
    for (int i = 0; i < func_count; i++) {
        StradaValue *func = ast_arr_get(funcs, i);
        const char *fname = ast_str(func, "name");
        if (strcmp(fname, "main") == 0) {
            total_errors += compile_function(prog, func);
        }
    }

    if (total_errors > 0) {
        fprintf(stderr, "%d error%s during compilation\n", total_errors, total_errors > 1 ? "s" : "");
        vm_gc_set_compiling_prog(NULL, &saved_vm);
        reset_compiler_globals();
        vm_program_free(prog);
        return NULL;
    }

    /* Store inheritance info */
    if (inherits) {
        StradaValue *inh_arr = ast_deref(inherits);
        if (inh_arr && !STRADA_IS_TAGGED_INT(inh_arr) && inh_arr->type == STRADA_ARRAY) {
            int count = (int)inh_arr->value.av->size;
            prog->inherits = calloc(count, sizeof(VMInherit));
            prog->inherit_count = 0;
            for (int i = 0; i < count; i++) {
                StradaValue *entry = ast_deref(strada_array_get(inh_arr->value.av, i));
                if (!entry || STRADA_IS_TAGGED_INT(entry) || entry->type != STRADA_HASH) continue;
                StradaValue *child_sv = strada_hash_get(entry->value.hv, "child");
                StradaValue *parent_sv = strada_hash_get(entry->value.hv, "parent");
                if (!child_sv || !parent_sv) continue;
                prog->inherits[prog->inherit_count].child = strdup(child_sv->value.pv);
                prog->inherits[prog->inherit_count].parent = strdup(parent_sv->value.pv);
                prog->inherit_count++;
            }
        }
    }

    /* Process OOP declarations */
    /* Program-level __C__ blocks: in compiled mode these are emitted at C
     * file scope (static helpers, #includes) visible to every function-level
     * block. The JIT compiles blocks as separate .so units, so collect them
     * into a prelude prepended to each unit. */
    {
        StradaValue *pcb = ast_get(ast, "c_blocks");
        if (pcb) pcb = ast_deref(pcb);
        if (pcb && !STRADA_IS_TAGGED_INT(pcb) && pcb->type == STRADA_ARRAY) {
            size_t total = 0;
            int n = (int)pcb->value.av->size;
            for (int i = 0; i < n; i++) {
                StradaValue *cb = ast_deref(strada_array_get(pcb->value.av, i));
                if (cb && !STRADA_IS_TAGGED_INT(cb) && cb->type == STRADA_STR && cb->value.pv)
                    total += strlen(cb->value.pv) + 2;
            }
            if (total > 0) {
                char *prelude = malloc(total + 1);
                size_t off = 0;
                for (int i = 0; i < n; i++) {
                    StradaValue *cb = ast_deref(strada_array_get(pcb->value.av, i));
                    if (cb && !STRADA_IS_TAGGED_INT(cb) && cb->type == STRADA_STR && cb->value.pv) {
                        size_t l = strlen(cb->value.pv);
                        memcpy(prelude + off, cb->value.pv, l); off += l;
                        prelude[off++] = '\n';
                    }
                }
                prelude[off] = 0;
                prog->cblock_prelude = prelude;
            }
        }
    }

    process_oop_decls(prog, ast);

    /* Program-level method modifiers: `before "m" func(...)` parses into
     * $program->{method_modifiers} (mod_type/method_name/func_name/package),
     * NOT the per-package "modifiers" list process_oop_decls reads. */
    {
        StradaValue *pmods = ast_get(ast, "method_modifiers");
        if (pmods) pmods = ast_deref(pmods);
        if (pmods && !STRADA_IS_TAGGED_INT(pmods) && pmods->type == STRADA_ARRAY) {
            int pm_count = (int)pmods->value.av->size;
            for (int m = 0; m < pm_count; m++) {
                StradaValue *mod = ast_deref(strada_array_get(pmods->value.av, m));
                if (!mod || STRADA_IS_TAGGED_INT(mod) || mod->type != STRADA_HASH) continue;
                const char *mod_type = ast_str(mod, "mod_type");
                const char *method = ast_str(mod, "method_name");
                const char *mod_func = ast_str(mod, "func_name");
                const char *mpkg = ast_str(mod, "package");
                int kind = -1;
                if (strcmp(mod_type, "before") == 0) kind = 0;
                else if (strcmp(mod_type, "after") == 0) kind = 1;
                else if (strcmp(mod_type, "around") == 0) kind = 2;
                if (kind >= 0 && method[0] && mod_func[0] && mpkg[0]) {
                    vm_program_add_modifier(prog, mpkg, method, mod_func, kind);
                }
            }
        }
    }

    synthesize_modifier_wrappers(prog);

    /* Process program-level overloads */
    StradaValue *prog_overloads = ast_get(ast, "overloads");
    if (prog_overloads) {
        prog_overloads = ast_deref(prog_overloads);
        if (prog_overloads && !STRADA_IS_TAGGED_INT(prog_overloads) && prog_overloads->type == STRADA_HASH) {
            /* prog_overloads is a hash of package_name -> hash of op -> method_name */
            StradaHash *oh = prog_overloads->value.hv;
            for (size_t oi = 0; oi < oh->next_slot; oi++) {
                StradaHashEntry *oe = &oh->entries[oi];
                if (!oe->key) continue;
                const char *cls = oe->key->data;
                StradaValue *pkg_ols = ast_deref(oe->value);
                if (!pkg_ols || STRADA_IS_TAGGED_INT(pkg_ols) || pkg_ols->type != STRADA_HASH) continue;
                StradaHash *poh = pkg_ols->value.hv;
                for (size_t pi = 0; pi < poh->next_slot; pi++) {
                    StradaHashEntry *pe = &poh->entries[pi];
                    if (!pe->key) continue;
                    const char *op = pe->key->data;
                    StradaValue *method_sv = pe->value;
                    if (!method_sv || STRADA_IS_TAGGED_INT(method_sv) || method_sv->type != STRADA_STR) continue;
                    const char *method = method_sv->value.pv;
                    if (op[0] && method[0]) {
                        vm_program_add_overload(prog, cls, op, method);
                    }
                }
            }
        }
    }

    vm_gc_set_compiling_prog(NULL, &saved_vm);
    reset_compiler_globals();
    return prog;
}

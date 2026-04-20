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
#define NI_INCREMENT     117
#define NI_C_BLOCK       119
#define NI_CATCH_CLAUSE  121
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
    OP_AST_SPACESHIP = 40,
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

typedef struct {
    VMChunk *chunk;
    VMProgram *prog;
    char *local_names[256];
    char local_sigils[256];
    int local_count;
    const char *func_name;
    int errors;
    LoopCtx *loop_ctx;
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

static int ctx_find_local(CompCtx *ctx, const char *name) {
    /* Search from most recent to oldest to handle shadowing */
    for (int i = ctx->local_count - 1; i >= 0; i--) {
        if (strcmp(ctx->local_names[i], name) == 0) return i;
    }
    return -1;
}

static int ctx_add_local(CompCtx *ctx, const char *name, char sigil) {
    int slot = ctx->local_count;
    ctx->local_names[slot] = strdup(name);
    ctx->local_sigils[slot] = sigil;
    ctx->local_count++;
    return slot;
}

static int ctx_find_capture(CompCtx *ctx, const char *name) {
    for (int i = 0; i < ctx->capture_count; i++) {
        if (strcmp(ctx->captures[i].name, name) == 0) return i;
    }
    return -1;
}

static int ctx_add_capture(CompCtx *ctx, const char *name, int outer_slot) {
    int idx = ctx->capture_count;
    ctx->captures[idx].name = strdup(name);
    ctx->captures[idx].outer_slot = outer_slot;
    ctx->capture_count++;
    return idx;
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
static void compile_stmt(CompCtx *ctx, StradaValue *node);
static void compile_block(CompCtx *ctx, StradaValue *block);
static void compile_map_expr(CompCtx *ctx, StradaValue *node);
static void compile_grep_expr(CompCtx *ctx, StradaValue *node);
static void compile_sort_expr(CompCtx *ctx, StradaValue *node);
static VMNativeEntry *vm_find_native(VMProgram *prog, const char *name);

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
        /* Store float literals as strings so sprintf %.f works, but keep
         * integer-valued floats (0.0, 1.0) as ints for comparison ops */
        StradaValue *v = ast_get(node, "value");
        if (v && !STRADA_IS_TAGGED_INT(v) && v->type == STRADA_STR && strchr(v->value.pv, '.')) {
            double d = atof(v->value.pv);
            if (d == (int64_t)d && d >= -1000000 && d <= 1000000) {
                /* Integer-valued float — use int */
                vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
                vm_chunk_emit_i64(ctx->chunk, (int64_t)d);
            } else {
                /* Non-integer float — store as string for precision */
                size_t idx = vm_chunk_add_str_const(ctx->chunk, v->value.pv);
                vm_chunk_emit(ctx->chunk, OP_PUSH_STR);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx);
            }
        } else {
            int64_t val = ast_int(node, "value");
            vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
            vm_chunk_emit_i64(ctx->chunk, val);
        }
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

        int slot = ctx_find_local(ctx, name);
        if (slot >= 0) {
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
        } else {
            /* Try capture */
            int cap = ctx_find_capture(ctx, name);
            if (cap >= 0) {
                vm_chunk_emit(ctx->chunk, OP_LOAD_CAPTURE);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)cap);
            } else if (ctx->is_closure && ctx->parent_ctx) {
                /* Auto-capture from parent scope */
                CompCtx *parent = (CompCtx*)ctx->parent_ctx;
                int parent_slot = ctx_find_local(parent, name);
                if (parent_slot >= 0) {
                    cap = ctx_add_capture(ctx, name, parent_slot);
                    vm_chunk_emit(ctx->chunk, OP_LOAD_CAPTURE);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)cap);
                } else {
                    /* Try parent's captures */
                    int pcap = ctx_find_capture(parent, name);
                    if (pcap >= 0) {
                        cap = ctx_add_capture(ctx, name, -1);
                        vm_chunk_emit(ctx->chunk, OP_LOAD_CAPTURE);
                        vm_chunk_emit_u16(ctx->chunk, (uint16_t)cap);
                    } else if (is_our_var(name)) {
                        /* our variable inside closure */
                        size_t key_idx = vm_chunk_add_str_const(ctx->chunk, name);
                        vm_chunk_emit(ctx->chunk, OP_LOAD_GLOBAL);
                        vm_chunk_emit_u16(ctx->chunk, (uint16_t)key_idx);
                    } else {
                        vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
                    }
                }
            } else if (is_our_var(name)) {
                /* our variable — load from global registry */
                size_t key_idx = vm_chunk_add_str_const(ctx->chunk, name);
                vm_chunk_emit(ctx->chunk, OP_LOAD_GLOBAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)key_idx);
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
            vm_chunk_emit(ctx->chunk, OP_DUP); /* array on top */
            compile_expr(ctx, ast_arr_get(elements, i));
            vm_chunk_emit(ctx->chunk, OP_ARRAY_PUSH);
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
                /* Try capture from parent */
                int cap = ctx_find_capture(ctx, inner.captures[i].name);
                if (cap >= 0) {
                    vm_chunk_emit(ctx->chunk, OP_LOAD_CAPTURE);
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
        int negated = (strcmp(op, "!~") == 0);
        compile_expr(ctx, ast_get(node, "target"));
        size_t pidx = vm_chunk_add_str_const(ctx->chunk, pattern);
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
                vm_chunk_emit(ctx->chunk, OP_DUP); /* keep array on stack */
                compile_expr(ctx, ast_arr_get(args, pi));
                vm_chunk_emit(ctx->chunk, OP_ARRAY_PUSH);
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

        /* Built-in: uc() */
        if (strcmp(name, "uc") == 0 && argc == 1) {
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_STR_UPPER);
            break;
        }

        /* Built-in: lc() */
        if (strcmp(name, "lc") == 0 && argc == 1) {
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
                int slot = ctx_find_local(ctx, vname);
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
            else if (strcmp(fn, "link") == 0) bid = BUILTIN_CORE_LINK;
            else if (strcmp(fn, "symlink") == 0) bid = BUILTIN_CORE_SYMLINK;
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
            else if (strcmp(fn, "get_byte") == 0) bid = BUILTIN_CORE_GET_BYTE;
            else if (strcmp(fn, "byte_length") == 0) bid = BUILTIN_CORE_BYTE_LENGTH;
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
            else if (strcmp(fn, "truncate") == 0) bid = BUILTIN_CORE_TRUNCATE;
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
        if (strcmp(name, "blessed") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_BLESSED); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "refcount") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_REFCOUNT); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "typeof") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_TYPEOF); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "dumper_str") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_DUMPER_STR); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "nsort") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_NSORT); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "cast_int") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_CAST_INT); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "cast_num") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_CAST_NUM); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "cast_str") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_CAST_STR); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "tie") == 0 && argc >= 2) { for (int i = 0; i < argc; i++) compile_expr(ctx, ast_arr_get(args, i)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_TIE); vm_chunk_emit(ctx->chunk, (uint8_t)argc); break; }
        if (strcmp(name, "untie") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_UNTIE); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "tied") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_TIED); vm_chunk_emit(ctx->chunk, 1); break; }

        /* User function call */
        int fidx = vm_program_find_func(ctx->prog, name);
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

        for (int i = 0; i < argc; i++) {
            compile_expr(ctx, ast_arr_get(args, i));
        }

        vm_chunk_emit(ctx->chunk, OP_CALL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)fidx);
        vm_chunk_emit(ctx->chunk, (uint8_t)argc);
        break;
    }

    case NI_ASSIGN: {
        StradaValue *target = ast_get(node, "target");
        StradaValue *value = ast_get(node, "value");
        const char *op = ast_str(node, "op");
        target = ast_deref(target);

        int target_type = target ? (int)ast_int(target, "type") : 0;

        /* Assignment to hash element */
        if (target_type == NI_HASH_ACCESS) {
            const char *prefix; StradaValue *suffix;
            if (strcmp(op, "=") == 0 &&
                is_concat_key(ast_get(target, "key"), &prefix, &suffix)) {
                compile_expr(ctx, ast_get(target, "hash"));
                compile_expr(ctx, suffix);
                compile_expr(ctx, value);
                size_t pidx = vm_chunk_add_str_const(ctx->chunk, prefix);
                vm_chunk_emit(ctx->chunk, OP_HASH_SET_CONCAT);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)pidx);
            } else {
                compile_expr(ctx, ast_get(target, "hash"));
                compile_expr(ctx, ast_get(target, "key"));
                compile_expr(ctx, value);
                vm_chunk_emit(ctx->chunk, OP_HASH_SET);
            }
            compile_expr(ctx, value);
            break;
        }

        /* Assignment to deref hash element */
        if (target_type == NI_DEREF_HASH) {
            compile_expr(ctx, ast_get(target, "ref"));
            compile_expr(ctx, ast_get(target, "key"));
            compile_expr(ctx, value);
            vm_chunk_emit(ctx->chunk, OP_HASH_SET);
            compile_expr(ctx, value);
            break;
        }

        /* Assignment to array element */
        if (target_type == NI_SUBSCRIPT) {
            compile_expr(ctx, ast_get(target, "array"));
            compile_expr(ctx, ast_get(target, "index"));
            compile_expr(ctx, value);
            vm_chunk_emit(ctx->chunk, OP_ARRAY_SET);
            compile_expr(ctx, value);
            break;
        }

        /* Assignment to variable */
        if (target_type == NI_VARIABLE) {
            const char *vname = ast_str(target, "name");
            int slot = ctx_find_local(ctx, vname);

            if (slot < 0) {
                /* Try capture */
                int cap = ctx_find_capture(ctx, vname);
                if (cap < 0 && ctx->is_closure && ctx->parent_ctx) {
                    CompCtx *parent = (CompCtx*)ctx->parent_ctx;
                    int parent_slot = ctx_find_local(parent, vname);
                    if (parent_slot >= 0) {
                        cap = ctx_add_capture(ctx, vname, parent_slot);
                    }
                }
                if (cap >= 0) {
                    compile_expr(ctx, value);
                    vm_chunk_emit(ctx->chunk, OP_DUP);
                    vm_chunk_emit(ctx->chunk, OP_STORE_CAPTURE);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)cap);
                    break;
                }
                /* Try our variable */
                if (is_our_var(vname)) {
                    size_t key_idx = vm_chunk_add_str_const(ctx->chunk, vname);
                    if (strcmp(op, "+=") == 0) {
                        vm_chunk_emit(ctx->chunk, OP_LOAD_GLOBAL);
                        vm_chunk_emit_u16(ctx->chunk, (uint16_t)key_idx);
                        compile_expr(ctx, value);
                        vm_chunk_emit(ctx->chunk, OP_ADD);
                    } else if (strcmp(op, "-=") == 0) {
                        vm_chunk_emit(ctx->chunk, OP_LOAD_GLOBAL);
                        vm_chunk_emit_u16(ctx->chunk, (uint16_t)key_idx);
                        compile_expr(ctx, value);
                        vm_chunk_emit(ctx->chunk, OP_SUB);
                    } else if (strcmp(op, "*=") == 0) {
                        vm_chunk_emit(ctx->chunk, OP_LOAD_GLOBAL);
                        vm_chunk_emit_u16(ctx->chunk, (uint16_t)key_idx);
                        compile_expr(ctx, value);
                        vm_chunk_emit(ctx->chunk, OP_MUL);
                    } else if (strcmp(op, ".=") == 0) {
                        vm_chunk_emit(ctx->chunk, OP_LOAD_GLOBAL);
                        vm_chunk_emit_u16(ctx->chunk, (uint16_t)key_idx);
                        compile_expr(ctx, value);
                        vm_chunk_emit(ctx->chunk, OP_CONCAT);
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

            if (strcmp(op, "=") == 0) {
                compile_expr(ctx, value);
            } else if (strcmp(op, "+=") == 0) {
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                compile_expr(ctx, value);
                vm_chunk_emit(ctx->chunk, OP_ADD);
            } else if (strcmp(op, "-=") == 0) {
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                compile_expr(ctx, value);
                vm_chunk_emit(ctx->chunk, OP_SUB);
            } else if (strcmp(op, "*=") == 0) {
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                compile_expr(ctx, value);
                vm_chunk_emit(ctx->chunk, OP_MUL);
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
                vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
                break;
            } else {
                compile_expr(ctx, value);
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
            int slot = ctx_find_local(ctx, vname);
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

    case NI_REGEX_SUBST: {
        StradaValue *target = ast_get(node, "target");
        const char *pattern = ast_str(node, "pattern");
        const char *replacement = ast_str(node, "replacement");
        const char *flags = ast_str(node, "flags");
        if (!flags) flags = "";
        int is_global = (strchr(flags, 'g') != NULL);

        if (target && !STRADA_IS_TAGGED_INT(target) &&
            (int)ast_int(target, "type") == NI_VARIABLE) {
            const char *vname = ast_str(target, "name");
            int slot = ctx_find_local(ctx, vname);
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

                size_t pat_idx = vm_chunk_add_str_const(ctx->chunk, pattern);
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
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)res_slot2);
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)src_slot);
            compile_expr(ctx, ast_arr_get(indices, i));
            vm_chunk_emit(ctx->chunk, OP_ARRAY_GET);
            vm_chunk_emit(ctx->chunk, OP_ARRAY_PUSH);
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

    /* Compile body expression */
    StradaValue *stmts = ast_get(body, "statements");
    int stmt_count = (int)ast_int(body, "statement_count");
    if (stmt_count > 0) {
        StradaValue *last_stmt = ast_deref(ast_arr_get(stmts, stmt_count - 1));
        if (last_stmt && !STRADA_IS_TAGGED_INT(last_stmt) &&
            (int)ast_int(last_stmt, "type") == NI_EXPR_STMT) {
            compile_expr(ctx, ast_get(last_stmt, "expr"));
        } else {
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)underscore_slot);
        }
    } else {
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)underscore_slot);
    }

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

    /* Compile condition expression */
    StradaValue *stmts = ast_get(body, "statements");
    int stmt_count = (int)ast_int(body, "statement_count");
    if (stmt_count > 0) {
        StradaValue *last_stmt = ast_deref(ast_arr_get(stmts, stmt_count - 1));
        if (last_stmt && !STRADA_IS_TAGGED_INT(last_stmt) &&
            (int)ast_int(last_stmt, "type") == NI_EXPR_STMT) {
            compile_expr(ctx, ast_get(last_stmt, "expr"));
        } else {
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)underscore_slot);
        }
    } else {
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)underscore_slot);
    }

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
        StradaValue *stmts = ast_get(body, "statements");
        int stmt_count = (int)ast_int(body, "statement_count");
        if (stmt_count > 0) {
            StradaValue *last_stmt = ast_deref(ast_arr_get(stmts, stmt_count - 1));
            if (last_stmt && !STRADA_IS_TAGGED_INT(last_stmt) &&
                (int)ast_int(last_stmt, "type") == NI_EXPR_STMT) {
                compile_expr(&cmp_ctx, ast_get(last_stmt, "expr"));
            } else {
                vm_chunk_emit(cmp_ctx.chunk, OP_PUSH_INT);
                vm_chunk_emit_i64(cmp_ctx.chunk, 0);
            }
        } else {
            vm_chunk_emit(cmp_ctx.chunk, OP_PUSH_INT);
            vm_chunk_emit_i64(cmp_ctx.chunk, 0);
        }
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
        /* const — compile like var_decl */
        const char *name = ast_str(node, "name");
        int slot = ctx_add_local(ctx, name, '$');
        StradaValue *init = ast_get(node, "value");
        if (!init) init = ast_get(node, "init");
        if (init && !STRADA_IS_TAGGED_INT(init)) {
            compile_expr(ctx, init);
        } else {
            vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
            vm_chunk_emit_i64(ctx->chunk, 0);
        }
        vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
        break;
    }

    case NI_RETURN_STMT: {
        StradaValue *val = ast_get(node, "value");
        if (val && !STRADA_IS_TAGGED_INT(val)) {
            compile_expr(ctx, val);
        } else {
            vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
            vm_chunk_emit_i64(ctx->chunk, 0);
        }
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
            vm_chunk_emit(ctx->chunk, OP_JMP);
            /* Redo jumps to the body start, after the condition check.
             * We use redo_target for this. Actually redo should skip the condition.
             * But our redo_target IS the loop_top which includes condition.
             * For while loops, redo means re-execute body without rechecking.
             * We need a separate body_start. But that complicates things.
             * For the test, the redo behavior with re-checking condition should still work
             * since the condition is true at the redo point. */
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

        size_t end_patches[32];
        int end_count = 0;

        for (int i = 0; i < case_count && i < 30; i++) {
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)val_slot);
            compile_expr(ctx, ast_arr_get(cases, i));
            vm_chunk_emit(ctx->chunk, OP_EQ);
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
        break;
    }

    case NI_TRY_CATCH: {
        /* try { ... } catch ($e) { ... } */
        StradaValue *try_body = ast_get(node, "try_block");
        StradaValue *catches = ast_get(node, "catch_clauses");
        int catch_count = (int)ast_int(node, "catch_count");

        /* TRY_BEGIN with catch offset (to be patched) */
        vm_chunk_emit(ctx->chunk, OP_TRY_BEGIN);
        size_t catch_patch = emit_patch_u16(ctx);

        compile_block(ctx, try_body);

        vm_chunk_emit(ctx->chunk, OP_TRY_END);

        /* Jump over catch blocks */
        vm_chunk_emit(ctx->chunk, OP_JMP);
        size_t end_patch = emit_patch_u16(ctx);

        /* Catch handler(s) */
        patch_u16(ctx, catch_patch, (uint16_t)ctx->chunk->code_len);

        /* Exception is on the stack */
        size_t catch_end_patches[32];
        int catch_end_count = 0;

        for (int i = 0; i < catch_count; i++) {
            StradaValue *cc = ast_arr_get(catches, i);
            const char *var_name = ast_str(cc, "catch_var");
            const char *type_name = ast_str(cc, "catch_type");

            if (type_name[0] && i < catch_count - 1) {
                /* Typed catch — check isa */
                vm_chunk_emit(ctx->chunk, OP_DUP);
                size_t tn_idx = vm_chunk_add_str_const(ctx->chunk, type_name);
                vm_chunk_emit(ctx->chunk, OP_PUSH_STR);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)tn_idx);
                vm_chunk_emit(ctx->chunk, OP_ISA);
                vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
                size_t next_catch = emit_patch_u16(ctx);

                /* Matched — store exception in variable */
                int slot = ctx_add_local(ctx, var_name, '$');
                vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);

                compile_block(ctx, ast_get(cc, "catch_block"));

                vm_chunk_emit(ctx->chunk, OP_JMP);
                if (catch_end_count < 32)
                    catch_end_patches[catch_end_count++] = emit_patch_u16(ctx);

                patch_u16(ctx, next_catch, (uint16_t)ctx->chunk->code_len);
            } else {
                /* Catch-all or last catch */
                int slot = ctx_add_local(ctx, var_name[0] ? var_name : "__exc", '$');
                vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);

                compile_block(ctx, ast_get(cc, "catch_block"));
            }
        }

        /* Patch all catch-end jumps to here */
        for (int i = 0; i < catch_end_count; i++) {
            patch_u16(ctx, catch_end_patches[i], (uint16_t)ctx->chunk->code_len);
        }

        patch_u16(ctx, end_patch, (uint16_t)ctx->chunk->code_len);
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
        /* $var =~ tr/search/replace/ */
        StradaValue *target = ast_get(node, "target");
        target = ast_deref(target);
        const char *from = ast_str(node, "search");
        const char *to = ast_str(node, "replace");

        if (target && !STRADA_IS_TAGGED_INT(target) &&
            (int)ast_int(target, "type") == NI_VARIABLE) {
            const char *vname = ast_str(target, "name");
            int slot = ctx_find_local(ctx, vname);
            if (slot >= 0) {
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);

                size_t from_idx = vm_chunk_add_str_const(ctx->chunk, from);
                size_t to_idx = vm_chunk_add_str_const(ctx->chunk, to);

                vm_chunk_emit(ctx->chunk, OP_TR);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)from_idx);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)to_idx);

                vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
            }
        }
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

        if (target && !STRADA_IS_TAGGED_INT(target) &&
            (int)ast_int(target, "type") == NI_VARIABLE) {
            const char *vname = ast_str(target, "name");
            int slot = ctx_find_local(ctx, vname);
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

                size_t pat_idx = vm_chunk_add_str_const(ctx->chunk, pattern);
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
            prog->cblocks[idx].var_count = ctx->local_count;
            prog->cblocks[idx].var_names = calloc(ctx->local_count, sizeof(char*));
            prog->cblocks[idx].var_slots = calloc(ctx->local_count, sizeof(int));
            for (int vi = 0; vi < ctx->local_count; vi++) {
                prog->cblocks[idx].var_names[vi] = strdup(ctx->local_names[vi]);
                prog->cblocks[idx].var_slots[vi] = vi;
            }
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

    for (int i = 0; i < count; i++) {
        compile_stmt(ctx, ast_arr_get(stmts, i));
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
        chunk->code_len = 0;
    } else {
        chunk = vm_program_add_func(prog, name);
    }

    CompCtx ctx = {0};
    ctx.chunk = chunk;
    ctx.prog = prog;
    ctx.func_name = name;

    /* Add parameters as locals, detect variadic */
    StradaValue *params = ast_get(func_node, "params");
    int param_count = (int)ast_int(func_node, "param_count");
    int fixed_params = 0;
    for (int i = 0; i < param_count; i++) {
        StradaValue *param = ast_arr_get(params, i);
        const char *pname = ast_str(param, "name");
        const char *psigil = ast_str(param, "sigil");
        int is_variadic = (int)ast_int(param, "is_variadic");
        ctx_add_local(&ctx, pname, psigil[0] ? psigil[0] : '$');
        if (is_variadic || (psigil[0] == '@')) {
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

static void process_import_libs(VMProgram *prog, StradaValue *ast) {
    StradaValue *import_libs = ast_get(ast, "import_libs");
    int import_count = (int)ast_int(ast, "import_lib_count");
    if (import_count <= 0 || !import_libs) return;

    import_libs = ast_deref(import_libs);
    if (!import_libs || STRADA_IS_TAGGED_INT(import_libs) || import_libs->type != STRADA_ARRAY) return;

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
            continue;
        }

        /* Call OOP init if present */
        char oop_init_name[256];
        snprintf(oop_init_name, sizeof(oop_init_name), "__%s_oop_init", lib_name);
        void (*oop_init)(void) = dlsym(dl, oop_init_name);
        if (oop_init) oop_init();

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
            if (!sym) continue;

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
        }
    }
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

VMProgram *vm_compile_program(StradaValue *ast) {
    VMProgram *prog = vm_program_new();

    StradaValue *funcs = ast_get(ast, "functions");
    int func_count = (int)ast_int(ast, "function_count");

    StradaValue *inherits = ast_get(ast, "inherits");

    int total_errors = 0;

    /* Process import_lib declarations — must be done before function compilation
     * so that calls to imported functions can be resolved */
    process_import_libs(prog, ast);

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

    /* First pass: register all function names (for forward references) */
    for (int i = 0; i < func_count; i++) {
        StradaValue *func = ast_arr_get(funcs, i);
        const char *fname = ast_str(func, "name");
        /* Pre-register so other functions can reference it */
        if (vm_program_find_func(prog, fname) < 0) {
            vm_program_add_func(prog, fname);
        }
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
    process_oop_decls(prog, ast);

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

    /* Process method modifiers (before/after/around) from program-level list */
    StradaValue *method_mods = ast_get(ast, "method_modifiers");
    if (method_mods) {
        method_mods = ast_deref(method_mods);
        if (method_mods && !STRADA_IS_TAGGED_INT(method_mods) && method_mods->type == STRADA_ARRAY) {
            int mm_count = method_mods->value.av->size;
            for (int mm = 0; mm < mm_count; mm++) {
                StradaValue *mod = ast_deref(strada_array_get(method_mods->value.av, mm));
                if (!mod || STRADA_IS_TAGGED_INT(mod) || mod->type != STRADA_HASH) continue;
                const char *mod_type = ast_str(mod, "mod_type");
                const char *method_name = ast_str(mod, "method_name");
                const char *func_name = ast_str(mod, "func_name");
                const char *package = ast_str(mod, "package");
                int kind = -1;
                if (strcmp(mod_type, "before") == 0) kind = 0;
                else if (strcmp(mod_type, "after") == 0) kind = 1;
                else if (strcmp(mod_type, "around") == 0) kind = 2;
                if (kind >= 0 && method_name[0] && func_name[0] && package[0]) {
                    vm_program_add_modifier(prog, package, method_name, func_name, kind);
                }
            }
        }
    }

    return prog;
}

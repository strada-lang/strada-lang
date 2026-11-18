/* vm_compiler.c — Compile Strada AST to bytecode for the VM.
 * Full language support: closures, try/catch, OOP, regex, I/O, etc. */

#include "vm_compiler.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dlfcn.h>
#include <math.h>
#include <limits.h>
#include <stddef.h>

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
#define NI_DUNDER_PACKAGE 33
#define NI_REGEX_MATCH   34
#define NI_REGEX_SUBST   35
#define NI_DUNDER_FILE   36
#define NI_DUNDER_LINE   37
#define NI_LAST          100
#define NI_NEXT          101
#define NI_UNDEF         102
#define NI_MAP           103
#define NI_SORT          104
#define NI_GREP          105
#define NI_TRY_CATCH     106
#define NI_THROW         107
#define NI_LABEL         108
#define NI_GOTO          109
#define NI_FOREACH_STMT  110
#define NI_ANON_FUNC     111
#define NI_CLOSURE_CALL  112
#define NI_TERNARY       113
#define NI_SWITCH        114
#define NI_SUPER_CALL    116
#define NI_INCREMENT     117
#define NI_ENUM_DECL     118
#define NI_C_BLOCK       119
#define NI_SPREAD        120
#define NI_CATCH_CLAUSE  121
#define NI_ASYNC_FUNC    122
#define NI_AWAIT         123
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
#define NI_DO_EXPR       138

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
    while (v && !STRADA_IS_TAGGED_INT(v) && v->type == STRADA_REF && !strada_is_slot_ref(v) && v->value.rv) {
        v = v->value.rv;
    }
    return v;
}

static StradaValue *ast_get(StradaValue *node, const char *key) {
    StradaValue *n = ast_deref(node);
    if (!n || STRADA_IS_TAGGED_INT(n) || n->type != STRADA_HASH) return NULL;
    return strada_hash_get(n->value.hv, key);
}

static int64_t ast_int(StradaValue *node, const char *key) {
    StradaValue *v = ast_get(node, key);
    if (v && !STRADA_IS_TAGGED_INT(v) && v->type == STRADA_STR && v->value.pv) {
        const char *p = v->value.pv;
        int neg = 0;
        if (*p == '-') { neg = 1; p++; }
        else if (*p == '+') { p++; }
        uint64_t uv = 0;
        if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) {
            uv = strtoull(p + 2, NULL, 16);
        } else if (p[0] == '0' && (p[1] == 'b' || p[1] == 'B')) {
            uv = strtoull(p + 2, NULL, 2);
        } else if (p[0] == '0' && (p[1] == 'o' || p[1] == 'O')) {
            uv = strtoull(p + 2, NULL, 8);
        } else {
            uv = strtoull(p, NULL, 10);
        }
        return neg ? (int64_t)(0ULL - uv) : (int64_t)uv;
    }
    return strada_to_int(v);
}

static const char *ast_str(StradaValue *node, const char *key) {
    StradaValue *v = ast_get(node, key);
    if (!v || STRADA_IS_TAGGED_INT(v) || v->type != STRADA_STR) return "";
    return v->value.pv ? v->value.pv : "";
}

static const char *ast_str_len(StradaValue *node, const char *key, size_t *out_len) {
    StradaValue *v = ast_get(node, key);
    if (!v || STRADA_IS_TAGGED_INT(v) || v->type != STRADA_STR || !v->value.pv) {
        if (out_len) *out_len = 0;
        return "";
    }
    size_t bl = STRADA_STR_BYTELEN(v);
    if (bl == 0 && v->value.pv[0] != '\0') bl = strlen(v->value.pv);
    if (out_len) *out_len = bl;
    return v->value.pv;
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
    size_t *break_patches;
    int break_count;
    int break_cap;
    size_t *next_patches;
    int next_count;
    int next_cap;
    size_t continue_target;     /* address to jump to for 'next' (set after body) */
    size_t redo_target;         /* address to jump to for 'redo' (body start, after condition) */
    const char *label;          /* label name or NULL */
    int local_restore_depth;
    int break_local_count;
    int next_local_count;
    int redo_local_count;
    struct LoopCtx *parent;
} LoopCtx;

static void loop_add_break_patch(LoopCtx *lc, size_t off) {
    if (lc->break_count >= lc->break_cap) {
        lc->break_cap = lc->break_cap > 0 ? lc->break_cap * 2 : 16;
        lc->break_patches = realloc(lc->break_patches, (size_t)lc->break_cap * sizeof(size_t));
    }
    lc->break_patches[lc->break_count++] = off;
}

static void loop_add_next_patch(LoopCtx *lc, size_t off) {
    if (lc->next_count >= lc->next_cap) {
        lc->next_cap = lc->next_cap > 0 ? lc->next_cap * 2 : 16;
        lc->next_patches = realloc(lc->next_patches, (size_t)lc->next_cap * sizeof(size_t));
    }
    lc->next_patches[lc->next_count++] = off;
}

static void loop_free_patches(LoopCtx *lc) {
    free(lc->break_patches);
    free(lc->next_patches);
    lc->break_patches = NULL;
    lc->next_patches = NULL;
    lc->break_count = lc->break_cap = 0;
    lc->next_count = lc->next_cap = 0;
}

/* Capture tracking for closures */
typedef struct {
    char *name;
    char sigil;
    int outer_slot;   /* slot in enclosing function */
    uint8_t var_type; /* 0 = untyped, 1 = TYPE_INT, 2 = TYPE_NUM */
} CaptureInfo;

/* Global name tracking for our variables */
static char **g_our_vars = NULL;
static int g_our_count = 0;
static int g_our_cap = 0;

static void register_our_var(const char *name) {
    if (!name) return;
    for (int i = 0; i < g_our_count; i++)
        if (strcmp(g_our_vars[i], name) == 0) return;
    if (g_our_count >= g_our_cap) {
        int ncap = g_our_cap ? g_our_cap * 2 : 256;
        g_our_vars = realloc(g_our_vars, (size_t)ncap * sizeof(char *));
        g_our_cap = ncap;
    }
    g_our_vars[g_our_count++] = strdup(name);
}

static const char *find_our_var_ptr(const char *name) {
    if (!name || !name[0]) return NULL;
    for (int i = 0; i < g_our_count; i++)
        if (strcmp(g_our_vars[i], name) == 0) return g_our_vars[i];
    return NULL;
}

/* const declarations: parsed references are plain $NAME variables, and the
 * decls land in the program's globals list (compiled in __globals_init), so
 * they behave exactly like our-globals — registered here, stored/loaded via
 * OP_STORE_GLOBAL/OP_LOAD_GLOBAL. */
static char **g_const_vars = NULL;
static int g_const_count = 0;
static int g_const_cap = 0;

static void register_const_var(const char *name) {
    if (!name) return;
    for (int i = 0; i < g_const_count; i++)
        if (strcmp(g_const_vars[i], name) == 0) return;
    if (g_const_count >= g_const_cap) {
        int ncap = g_const_cap ? g_const_cap * 2 : 256;
        g_const_vars = realloc(g_const_vars, (size_t)ncap * sizeof(char *));
        g_const_cap = ncap;
    }
    g_const_vars[g_const_count++] = strdup(name);
}

static const char *find_const_var_ptr(const char *name) {
    if (!name || !name[0]) return NULL;
    for (int i = 0; i < g_const_count; i++)
        if (strcmp(g_const_vars[i], name) == 0) return g_const_vars[i];
    return NULL;
}

typedef struct {
    char *name;
    int64_t val;
} VMEnumEntry;

static VMEnumEntry *g_enums = NULL;
static int g_enum_count = 0;
static int g_enum_cap = 0;
static char *g_source_file = NULL;

static void register_enum_val(const char *name, int64_t val) {
    if (!name || !name[0]) return;
    for (int i = 0; i < g_enum_count; i++) {
        if (strcmp(g_enums[i].name, name) == 0) {
            g_enums[i].val = val;
            return;
        }
    }
    if (g_enum_count >= g_enum_cap) {
        int ncap = g_enum_cap > 0 ? g_enum_cap * 2 : 64;
        VMEnumEntry *ne = realloc(g_enums, (size_t)ncap * sizeof(VMEnumEntry));
        if (!ne) return;
        g_enums = ne;
        g_enum_cap = ncap;
    }
    g_enums[g_enum_count].name = strdup(name);
    g_enums[g_enum_count].val = val;
    g_enum_count++;
}

static int find_enum_val(const char *name, int64_t *out_val) {
    if (!name || !name[0]) return 0;
    for (int i = 0; i < g_enum_count; i++) {
        if (strcmp(g_enums[i].name, name) == 0) {
            *out_val = g_enums[i].val;
            return 1;
        }
    }
    return 0;
}

typedef struct {
    char *bare_name;
    int func_idx;
} VMPkgFuncAlias;

static VMPkgFuncAlias *g_pkg_aliases = NULL;
static int g_pkg_alias_count = 0;
static int g_pkg_alias_cap = 0;

static void reset_pkg_func_aliases(void) {
    for (int i = 0; i < g_pkg_alias_count; i++) {
        free(g_pkg_aliases[i].bare_name);
    }
    free(g_pkg_aliases);
    g_pkg_aliases = NULL;
    g_pkg_alias_count = 0;
    g_pkg_alias_cap = 0;
}

static void register_pkg_func_alias(const char *bare_name, int func_idx) {
    if (!bare_name || !bare_name[0] || func_idx < 0) return;
    for (int i = 0; i < g_pkg_alias_count; i++) {
        if (strcmp(g_pkg_aliases[i].bare_name, bare_name) == 0) return;
    }
    if (g_pkg_alias_count >= g_pkg_alias_cap) {
        g_pkg_alias_cap = g_pkg_alias_cap ? g_pkg_alias_cap * 2 : 32;
        g_pkg_aliases = realloc(g_pkg_aliases, (size_t)g_pkg_alias_cap * sizeof(VMPkgFuncAlias));
    }
    g_pkg_aliases[g_pkg_alias_count].bare_name = strdup(bare_name);
    g_pkg_aliases[g_pkg_alias_count].func_idx = func_idx;
    g_pkg_alias_count++;
}

static int find_pkg_func_alias(const char *name) {
    if (!name || !name[0]) return -1;
    for (int i = 0; i < g_pkg_alias_count; i++) {
        if (strcmp(g_pkg_aliases[i].bare_name, name) == 0) {
            return g_pkg_aliases[i].func_idx;
        }
    }
    return -1;
}

static void reset_compiler_globals(void) {
    for (int i = 0; i < g_our_count; i++) {
        free(g_our_vars[i]);
    }
    free(g_our_vars);
    g_our_vars = NULL;
    g_our_count = 0;
    g_our_cap = 0;
    for (int i = 0; i < g_const_count; i++) {
        free(g_const_vars[i]);
    }
    free(g_const_vars);
    g_const_vars = NULL;
    g_const_count = 0;
    g_const_cap = 0;
    for (int i = 0; i < g_enum_count; i++) {
        free(g_enums[i].name);
    }
    free(g_enums);
    g_enums = NULL;
    g_enum_count = 0;
    g_enum_cap = 0;
    free(g_source_file);
    g_source_file = NULL;
    reset_pkg_func_aliases();
}

#define VM_MAX_GOTO_SCOPE 64

typedef struct TryCtx {
    int handlers_active;        /* number of active OP_TRY_BEGIN frames on exc_stack for this try */
    StradaValue *finally_block; /* non-NULL if this try statement has a finally block */
    LoopCtx *enclosing_loop;    /* ctx->loop_ctx when this try was entered */
    int local_restore_depth;    /* ctx->local_restore_count when this try was entered */
    int enclosing_scope_depth;  /* ctx->scope_depth when this try was entered */
    struct TryCtx *parent;
} TryCtx;

typedef struct {
    char *name;
    size_t offset;
    int has_offset;
    int try_handler_depth;
    int local_restore_depth;
    int actual_local_count;
    int has_actual_lc;
    int scope_depth;
    void *scope_path[VM_MAX_GOTO_SCOPE];
} VMLabelDef;

typedef struct {
    char *name;
    size_t patch_off;
} VMGotoPatch;

typedef struct {
    VMChunk *chunk;
    VMProgram *prog;
    char *local_names[256];
    char local_sigils[256];
    uint8_t local_types[256]; /* 0 = untyped, 1 = TYPE_INT, 2 = TYPE_NUM */
    int local_count;
    int max_local_count;
    int has_non_scalar;
    int may_box_locals;
    const char *local_struct[256]; /* C struct type of a local (borrowed AST string) or NULL */
    size_t tail_call_end;
    const char *func_name;
    const char *cur_package;  /* enclosing package for unqualified-call fallback */
    int errors;
    LoopCtx *loop_ctx;
    TryCtx *try_ctx;
    int assign_depth;
    int call_context;
    /* Closure support */
    CaptureInfo captures[64];
    int capture_count;
    int is_closure;
    /* Parent context for nested functions */
    void *parent_ctx;
    /* Local restore tracking */
    char **local_restores;
    int local_restore_count;
    int local_restore_cap;
    /* Label & goto tracking */
    const char *pending_loop_label;
    VMLabelDef *labels;
    int label_count;
    int label_cap;
    VMGotoPatch *gotos;
    int goto_count;
    int goto_cap;
    void *scope_nodes[VM_MAX_GOTO_SCOPE];
    int scope_saved_lc[VM_MAX_GOTO_SCOPE];
    int scope_depth;
} CompCtx;

static inline void ctx_push_scope(CompCtx *ctx, void *scope_node, int saved_lc) {
    if (ctx->scope_depth < VM_MAX_GOTO_SCOPE) {
        ctx->scope_nodes[ctx->scope_depth] = scope_node;
        ctx->scope_saved_lc[ctx->scope_depth] = saved_lc;
        ctx->scope_depth++;
    }
}

static inline void ctx_pop_scope(CompCtx *ctx) {
    if (ctx->scope_depth > 0) {
        ctx->scope_depth--;
    }
}

/* Resolve an `our` or `const` variable name using package qualification (matching compiled C)
 * while still finding unqualified eval-promoted or file-scope globals. Returned pointer stays
 * valid for the entire vm_compile_program() invocation. */
static const char *resolve_global_var_key(CompCtx *ctx, const char *name) {
    if (!name || !name[0]) return NULL;
    /* $_ outside any topicalizer is the program-wide global (Perl; the
     * compiled backend's C global `_`) */
    if (strcmp(name, "_") == 0 || strcmp(name, "main::_") == 0) return "main::_";
    const char *p = NULL;
    if (strncmp(name, "main::", 6) == 0) {
        p = find_our_var_ptr(name);
        if (p) return p;
        p = find_our_var_ptr(name + 6);
        if (p) return p;
        return find_const_var_ptr(name + 6);
    }
    if (strchr(name, ':') != NULL) {
        p = find_our_var_ptr(name);
        if (p) return p;
        return find_const_var_ptr(name);
    }
    if (ctx && ctx->cur_package && ctx->cur_package[0] &&
        strcmp(ctx->cur_package, "main") != 0) {
        char qbuf[256];
        snprintf(qbuf, sizeof(qbuf), "%s::%s", ctx->cur_package, name);
        p = find_our_var_ptr(qbuf);
        if (p) return p;
    }
    p = find_our_var_ptr(name);
    if (p) return p;
    return find_const_var_ptr(name);
}

static void vm_report_undeclared_var(CompCtx *ctx, StradaValue *node, const char *sigil, const char *name) {
    int line = (int)ast_int(node, "line");
    if (line > 0) {
        fprintf(stderr, "VM compile error (line %d): undeclared variable '%s%s'\n",
                line, sigil ? sigil : "$", name ? name : "");
    } else {
        fprintf(stderr, "VM compile error: undeclared variable '%s%s'\n",
                sigil ? sigil : "$", name ? name : "");
    }
    ctx->errors++;
}

static int lookup_enum_for_ctx(CompCtx *ctx, const char *name, int64_t *out_val) {
    if (!name || !name[0]) return 0;
    if (find_enum_val(name, out_val)) return 1;
    if (!strchr(name, ':') && ctx && ctx->cur_package && ctx->cur_package[0] &&
        strcmp(ctx->cur_package, "main") != 0) {
        char qbuf[256];
        snprintf(qbuf, sizeof(qbuf), "%s::%s", ctx->cur_package, name);
        if (find_enum_val(qbuf, out_val)) return 1;
    }
    return 0;
}

static void collect_vm_mro_rec(VMProgram *prog, const char *cls, const char **out, int max, int *n);

static int resolve_func_in_pkg(VMProgram *prog, const char *pkg, const char *name) {
    char pkgname[512];
    snprintf(pkgname, sizeof(pkgname), "%s_%s", pkg, name);
    int fidx = vm_program_find_func(prog, pkgname);
    if (fidx < 0 && strchr(pkg, ':')) {
        char spkg[256];
        size_t pj = 0;
        for (size_t k = 0; pkg[k] && pj < sizeof(spkg) - 1; k++) {
            if (pkg[k] == ':' && pkg[k + 1] == ':') {
                spkg[pj++] = '_';
                k++;
            } else {
                spkg[pj++] = pkg[k];
            }
        }
        spkg[pj] = '\0';
        snprintf(pkgname, sizeof(pkgname), "%s_%s", spkg, name);
        fidx = vm_program_find_func(prog, pkgname);
    }
    if (fidx < 0) {
        snprintf(pkgname, sizeof(pkgname), "%s::%s", pkg, name);
        fidx = vm_program_find_func(prog, pkgname);
    }
    return fidx;
}

static int resolve_func_idx_for_ctx(CompCtx *ctx, const char *name, char *norm_buf, size_t norm_sz) {
    if (norm_buf && norm_sz > 0) norm_buf[0] = '\0';
    if (!ctx || !ctx->prog || !name || !name[0]) return -1;
    int fidx = vm_program_find_func(ctx->prog, name);
    if (fidx < 0 && !strchr(name, ':') && ctx->cur_package && ctx->cur_package[0] &&
        strcmp(ctx->cur_package, "main") != 0) {
        const char *mro[64];
        int mro_n = 0;
        collect_vm_mro_rec(ctx->prog, ctx->cur_package, mro, 64, &mro_n);
        for (int mi = 0; mi < mro_n && fidx < 0; mi++) {
            fidx = resolve_func_in_pkg(ctx->prog, mro[mi], name);
        }
    }
    if (strchr(name, ':')) {
        char local_norm[256];
        char *dst = norm_buf ? norm_buf : local_norm;
        size_t dsz = norm_buf ? norm_sz : sizeof(local_norm);
        size_t j = 0;
        for (size_t k = 0; name[k] && j < dsz - 1; k++) {
            if (name[k] == ':' && name[k + 1] == ':') {
                dst[j++] = '_';
                k++;
            } else {
                dst[j++] = name[k];
            }
        }
        dst[j] = '\0';
        if (fidx < 0) {
            fidx = vm_program_find_func(ctx->prog, dst);
        }
    }
    if (fidx < 0 && strncmp(name, "main::", 6) == 0) {
        fidx = vm_program_find_func(ctx->prog, name + 6);
    }
    /* `::f()` inside package Pkg::Sub arrives as "Pkg_Sub_f" (the parser
     * builds the C name); functions from `use`d modules are registered as
     * "Pkg::Sub::f" */
    if (fidx < 0 && !strchr(name, ':') && ctx->cur_package && ctx->cur_package[0] &&
        strcmp(ctx->cur_package, "main") != 0) {
        char san[256];
        size_t sj = 0;
        for (size_t k = 0; ctx->cur_package[k] && sj < sizeof(san) - 1; k++) {
            if (ctx->cur_package[k] == ':' && ctx->cur_package[k + 1] == ':') { san[sj++] = '_'; k++; }
            else san[sj++] = ctx->cur_package[k];
        }
        san[sj] = 0;
        if (sj > 0 && strncmp(name, san, sj) == 0 && name[sj] == '_' && name[sj + 1])
            fidx = resolve_func_in_pkg(ctx->prog, ctx->cur_package, name + sj + 1);
    }
    if (fidx < 0 && !strchr(name, ':')) {
        fidx = find_pkg_func_alias(name);
    }
    return fidx;
}

static const char *normalize_local_global_key(CompCtx *ctx, const char *name, const char *our_key,
                                              char *buf, size_t bufsz) {
    const char *cand = (our_key && our_key[0]) ? our_key : name;
    if (!cand || !cand[0]) return "";
    const char *res = resolve_global_var_key(ctx, cand);
    if (res) return res;
    if (strncmp(cand, "main::", 6) == 0) return cand + 6;
    if (strchr(cand, ':') == NULL && ctx && ctx->cur_package && ctx->cur_package[0] &&
        strcmp(ctx->cur_package, "main") != 0) {
        snprintf(buf, bufsz, "%s::%s", ctx->cur_package, cand);
        return buf;
    }
    return cand;
}

static int ctx_find_local_sigil(CompCtx *ctx, const char *name, char sigil) {
    /* Pass 1: exact sigil match (allows $x, @x, %x to coexist in one scope) */
    if (sigil != 0) {
        for (int i = ctx->local_count - 1; i >= 0; i--) {
            if (ctx->local_sigils[i] == sigil && strcmp(ctx->local_names[i], name) == 0)
                return i;
        }
    }
    /* Pass 2: fallback without sigil (e.g. my array $arr accessed as $arr[0]).
     * $_ never falls back to a no-parens function's @_ (unrelated in Perl). */
    for (int i = ctx->local_count - 1; i >= 0; i--) {
        if (strcmp(ctx->local_names[i], name) == 0) {
            if (sigil == '$' && ctx->local_sigils[i] == '@' && name[0] == '_' && name[1] == 0) continue;
            return i;
        }
    }
    return -1;
}

static int ctx_find_local(CompCtx *ctx, const char *name) {
    return ctx_find_local_sigil(ctx, name, 0);
}

static int ctx_add_local(CompCtx *ctx, const char *name, char sigil) {
    if (sigil != '$' && sigil != '\0') {
        ctx->has_non_scalar = 1;
    }
    /* local_names[]/local_sigils[] are fixed 256-entry arrays. Without this
     * bound check, a function/scope with >=256 accumulated locals overflowed
     * them (stack buffer overflow when compiling untrusted source). Fail with
     * a clear diagnostic rather than corrupt the compiler's stack. */
    if (ctx->local_count >= (int)(sizeof(ctx->local_names) / sizeof(ctx->local_names[0]))) {
        fprintf(stderr, "VM compile error: too many local variables in one scope (max %d); "
                        "this function is too large for the bytecode VM backend\n",
                (int)(sizeof(ctx->local_names) / sizeof(ctx->local_names[0])));
        ctx->errors++;
        return 255;
    }
    int slot = ctx->local_count;
    if (ctx->local_names[slot]) free(ctx->local_names[slot]);
    ctx->local_names[slot] = strdup(name);
    ctx->local_sigils[slot] = sigil;
    ctx->local_types[slot] = 0;
    ctx->local_struct[slot] = NULL;
    ctx->local_count++;
    if (ctx->local_count > ctx->max_local_count) ctx->max_local_count = ctx->local_count;
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
    if (strcmp(op, "x=") == 0)  return OP_STR_REPEAT;
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

static int ctx_find_capture(CompCtx *ctx, const char *name, char sigil) {
    for (int i = 0; i < ctx->capture_count; i++) {
        if (ctx->captures[i].sigil == sigil && strcmp(ctx->captures[i].name, name) == 0) return i;
    }
    return -1;
}

static int ctx_add_capture(CompCtx *ctx, const char *name, char sigil, int outer_slot, uint8_t var_type) {
    /* captures[] is a fixed 64-entry array (and the parallel outer_slots[64]
     * at the call site is indexed by the same count). Without this bound, a
     * closure capturing >=64 distinct enclosing variables overflowed them. */
    if (ctx->capture_count >= (int)(sizeof(ctx->captures) / sizeof(ctx->captures[0]))) {
        fprintf(stderr, "VM compile error: closure captures too many variables (max %d)\n",
                (int)(sizeof(ctx->captures) / sizeof(ctx->captures[0])));
        ctx->errors++;
        return 63;
    }
    int idx = ctx->capture_count;
    ctx->captures[idx].name = strdup(name);
    ctx->captures[idx].sigil = sigil;
    ctx->captures[idx].outer_slot = outer_slot;
    ctx->captures[idx].var_type = var_type;
    ctx->capture_count++;
    return idx;
}

static int ctx_resolve_capture(CompCtx *ctx, const char *name, char sigil) {
    int cap = ctx_find_capture(ctx, name, sigil);
    if (cap >= 0) return cap;
    if (!ctx->is_closure || !ctx->parent_ctx) return -1;
    CompCtx *parent = (CompCtx*)ctx->parent_ctx;
    int parent_slot = ctx_find_local_sigil(parent, name, sigil);
    if (parent_slot >= 0) return ctx_add_capture(ctx, name, sigil, parent_slot, parent->local_types[parent_slot]);
    int pcap = ctx_resolve_capture(parent, name, sigil);
    if (pcap >= 0) return ctx_add_capture(ctx, name, sigil, -1, parent->captures[pcap].var_type);
    return -1;
}

static void emit_push_int(VMChunk *chunk, int64_t val) {
    if (val >= INT8_MIN && val <= INT8_MAX) {
        vm_chunk_emit(chunk, OP_PUSH_I8);
        vm_chunk_emit(chunk, (uint8_t)(int8_t)val);
    } else if (val >= INT32_MIN && val <= INT32_MAX) {
        vm_chunk_emit(chunk, OP_PUSH_I32);
        vm_chunk_emit_u32(chunk, (uint32_t)(int32_t)val);
    } else {
        vm_chunk_emit(chunk, OP_PUSH_INT);
        vm_chunk_emit_i64(chunk, val);
    }
}

static int find_local_var_slot(CompCtx *ctx, StradaValue *node) {
    node = ast_deref(node);
    if (!node || STRADA_IS_TAGGED_INT(node) || (int)ast_int(node, "type") != NI_VARIABLE) return -1;
    const char *name = ast_str(node, "name");
    const char *sigil = ast_str(node, "sigil");
    char sc = (sigil && sigil[0]) ? sigil[0] : '$';
    if (sc == '$' && name[0] >= '1' && name[0] <= '9' && name[1] == '\0') return -1;
    return ctx_find_local_sigil(ctx, name, sc);
}

enum { CF_NONE = 0, CF_INT = 1, CF_FLOAT = 2, CF_STR = 3 };

typedef struct {
    int kind;
    int64_t iv;
    double dv;
    const char *sv;
    size_t slen;
    int owned;
} VMConstFold;

static inline void cf_free(VMConstFold *c) {
    if (c->owned && c->sv) {
        free((void*)c->sv);
        c->sv = NULL;
    }
    c->owned = 0;
    c->kind = CF_NONE;
}

static int try_eval_const_ast(StradaValue *node, VMConstFold *out, int depth) {
    memset(out, 0, sizeof(*out));
    if (depth > 32) return 0;
    node = ast_deref(node);
    if (!node || STRADA_IS_TAGGED_INT(node)) return 0;
    int t = (int)ast_int(node, "type");
    if (t == NI_INT_LITERAL) {
        out->kind = CF_INT;
        out->iv = ast_int(node, "value");
        return 1;
    }
    if (t == NI_NUM_LITERAL) {
        StradaValue *v = ast_get(node, "value");
        double d = 0.0;
        if (v && !STRADA_IS_TAGGED_INT(v)) {
            if (v->type == STRADA_STR && v->value.pv) d = strtod(v->value.pv, NULL);
            else d = strada_to_num(v);
        } else {
            d = (double)ast_int(node, "value");
        }
        out->kind = CF_FLOAT;
        out->dv = d;
        return 1;
    }
    if (t == NI_STR_LITERAL) {
        size_t slen = 0;
        const char *s = ast_str_len(node, "value", &slen);
        out->kind = CF_STR;
        out->sv = s ? s : "";
        out->slen = slen;
        out->owned = 0;
        return 1;
    }
    if (t == NI_UNARY_OP) {
        const char *op = ast_str(node, "op");
        VMConstFold sub;
        if (!try_eval_const_ast(ast_get(node, "operand"), &sub, depth + 1)) return 0;
        if (strcmp(op, "-") == 0) {
            if (sub.kind == CF_INT && sub.iv != INT64_MIN) {
                out->kind = CF_INT;
                out->iv = -sub.iv;
                return 1;
            }
            if (sub.kind == CF_FLOAT) {
                out->kind = CF_FLOAT;
                out->dv = -sub.dv;
                return 1;
            }
        } else if (strcmp(op, "~") == 0 && sub.kind == CF_INT) {
            out->kind = CF_INT;
            out->iv = ~sub.iv;
            return 1;
        } else if (strcmp(op, "!") == 0 || strcmp(op, "not") == 0) {
            int truthy = 0;
            if (sub.kind == CF_INT) truthy = (sub.iv != 0);
            else if (sub.kind == CF_FLOAT) truthy = (sub.dv != 0.0 && !isnan(sub.dv));
            else if (sub.kind == CF_STR) truthy = (sub.slen > 0 && !(sub.slen == 1 && sub.sv[0] == '0'));
            cf_free(&sub);
            out->kind = CF_INT;
            out->iv = truthy ? 0 : 1;
            return 1;
        }
        cf_free(&sub);
        return 0;
    }
    if (t == NI_BINARY_OP) {
        int op = ast_op_code(node);
        if (op == OP_AST_LOG_AND || op == OP_AST_LOG_OR || op == OP_AST_DEFOR) return 0;
        StradaValue *ln = ast_get(node, "left");
        StradaValue *rn = ast_get(node, "right");
        int is_num_cmp = (op == OP_AST_NUM_EQ || op == OP_AST_NUM_NE ||
                          op == OP_AST_NUM_LT || op == OP_AST_NUM_GT ||
                          op == OP_AST_NUM_LE || op == OP_AST_NUM_GE ||
                          op == OP_AST_SPACESHIP);
        if (is_num_cmp) {
            StradaValue *lsq = ast_get(ln, "from_sq_str");
            StradaValue *rsq = ast_get(rn, "from_sq_str");
            if ((lsq && !STRADA_IS_TAGGED_INT(lsq) && lsq->type == STRADA_STR) ||
                (rsq && !STRADA_IS_TAGGED_INT(rsq) && rsq->type == STRADA_STR)) return 0;
        }
        VMConstFold l, r;
        if (!try_eval_const_ast(ln, &l, depth + 1)) return 0;
        if (!try_eval_const_ast(rn, &r, depth + 1)) { cf_free(&l); return 0; }

        if (l.kind == CF_STR && r.kind == CF_STR) {
            if (op == OP_AST_CONCAT && l.slen + r.slen <= 65536) {
                char *buf = malloc(l.slen + r.slen + 1);
                if (buf) {
                    if (l.slen > 0) memcpy(buf, l.sv, l.slen);
                    if (r.slen > 0) memcpy(buf + l.slen, r.sv, r.slen);
                    buf[l.slen + r.slen] = '\0';
                    out->kind = CF_STR;
                    out->sv = buf;
                    out->slen = l.slen + r.slen;
                    out->owned = 1;
                    cf_free(&l); cf_free(&r);
                    return 1;
                }
            } else if (op == OP_AST_STR_EQ || op == OP_AST_STR_NE || op == OP_AST_STR_LT ||
                       op == OP_AST_STR_GT || op == OP_AST_STR_LE || op == OP_AST_STR_GE ||
                       op == OP_AST_STR_CMP) {
                size_t minl = l.slen < r.slen ? l.slen : r.slen;
                int c = minl > 0 ? memcmp(l.sv, r.sv, minl) : 0;
                if (c == 0) c = (l.slen < r.slen) ? -1 : ((l.slen > r.slen) ? 1 : 0);
                int64_t iv = 0;
                if (op == OP_AST_STR_EQ) iv = (c == 0);
                else if (op == OP_AST_STR_NE) iv = (c != 0);
                else if (op == OP_AST_STR_LT) iv = (c < 0);
                else if (op == OP_AST_STR_GT) iv = (c > 0);
                else if (op == OP_AST_STR_LE) iv = (c <= 0);
                else if (op == OP_AST_STR_GE) iv = (c >= 0);
                else iv = (c < 0) ? -1 : ((c > 0) ? 1 : 0);
                cf_free(&l); cf_free(&r);
                out->kind = CF_INT;
                out->iv = iv;
                return 1;
            }
            cf_free(&l); cf_free(&r);
            return 0;
        }
        if (l.kind == CF_INT && r.kind == CF_INT) {
            int64_t res = 0;
            int ok = 1;
            switch (op) {
            case OP_AST_ADD:
                if (__builtin_add_overflow(l.iv, r.iv, &res) || res < VM_INT_MIN || res > VM_INT_MAX) ok = 0;
                break;
            case OP_AST_SUB:
                if (__builtin_sub_overflow(l.iv, r.iv, &res) || res < VM_INT_MIN || res > VM_INT_MAX) ok = 0;
                break;
            case OP_AST_MUL:
                if (__builtin_mul_overflow(l.iv, r.iv, &res) || res < VM_INT_MIN || res > VM_INT_MAX) ok = 0;
                break;
            case OP_AST_DIV:
                if (r.iv != 0) {
                    out->kind = CF_FLOAT;
                    out->dv = (double)l.iv / (double)r.iv;
                    return 1;
                }
                ok = 0;
                break;
            case OP_AST_MOD:
                if (r.iv != 0 && !(l.iv == INT64_MIN && r.iv == -1)) {
                    res = l.iv % r.iv;
                    if (res != 0 && ((res < 0) != (r.iv < 0))) res += r.iv;
                } else ok = 0;
                break;
            case OP_AST_BIT_AND: res = l.iv & r.iv; break;
            case OP_AST_BIT_OR:  res = l.iv | r.iv; break;
            case OP_AST_BIT_XOR: res = l.iv ^ r.iv; break;
            case OP_AST_BIT_SHL:
                if (r.iv >= 0 && r.iv < 64) {
                    res = (int64_t)((uint64_t)l.iv << (int)r.iv);
                    if (res < VM_INT_MIN || res > VM_INT_MAX) ok = 0;
                } else res = 0;
                break;
            case OP_AST_BIT_SHR:
                if (r.iv >= 0 && r.iv < 64) res = l.iv >> (int)r.iv;
                else res = (l.iv < 0) ? -1 : 0;
                break;
            case OP_AST_NUM_EQ: res = (l.iv == r.iv); break;
            case OP_AST_NUM_NE: res = (l.iv != r.iv); break;
            case OP_AST_NUM_LT: res = (l.iv < r.iv); break;
            case OP_AST_NUM_GT: res = (l.iv > r.iv); break;
            case OP_AST_NUM_LE: res = (l.iv <= r.iv); break;
            case OP_AST_NUM_GE: res = (l.iv >= r.iv); break;
            case OP_AST_SPACESHIP: res = (l.iv < r.iv) ? -1 : ((l.iv > r.iv) ? 1 : 0); break;
            default: ok = 0; break;
            }
            if (ok) {
                out->kind = CF_INT;
                out->iv = res;
                return 1;
            }
            return 0;
        }
        if ((l.kind == CF_INT || l.kind == CF_FLOAT) && (r.kind == CF_INT || r.kind == CF_FLOAT)) {
            double da = (l.kind == CF_FLOAT) ? l.dv : (double)l.iv;
            double db = (r.kind == CF_FLOAT) ? r.dv : (double)r.iv;
            if (!isnan(da) && !isnan(db)) {
                switch (op) {
                case OP_AST_ADD: out->kind = CF_FLOAT; out->dv = da + db; return 1;
                case OP_AST_SUB: out->kind = CF_FLOAT; out->dv = da - db; return 1;
                case OP_AST_MUL: out->kind = CF_FLOAT; out->dv = da * db; return 1;
                case OP_AST_DIV:
                    if (db != 0.0) { out->kind = CF_FLOAT; out->dv = da / db; return 1; }
                    return 0;
                case OP_AST_NUM_EQ: out->kind = CF_INT; out->iv = (da == db); return 1;
                case OP_AST_NUM_NE: out->kind = CF_INT; out->iv = (da != db); return 1;
                case OP_AST_NUM_LT: out->kind = CF_INT; out->iv = (da < db); return 1;
                case OP_AST_NUM_GT: out->kind = CF_INT; out->iv = (da > db); return 1;
                case OP_AST_NUM_LE: out->kind = CF_INT; out->iv = (da <= db); return 1;
                case OP_AST_NUM_GE: out->kind = CF_INT; out->iv = (da >= db); return 1;
                case OP_AST_SPACESHIP: out->kind = CF_INT; out->iv = (da < db) ? -1 : ((da > db) ? 1 : 0); return 1;
                default: break;
                }
            }
        }
        cf_free(&l); cf_free(&r);
    }
    return 0;
}

static void emit_folded_const(CompCtx *ctx, VMConstFold *c) {
    if (c->kind == CF_INT) {
        emit_push_int(ctx->chunk, c->iv);
    } else if (c->kind == CF_FLOAT) {
        vm_chunk_emit(ctx->chunk, OP_PUSH_FLOAT);
        vm_chunk_emit_f64(ctx->chunk, c->dv);
    } else if (c->kind == CF_STR) {
        size_t idx = vm_chunk_add_str_const_len(ctx->chunk, c->sv, c->slen);
        vm_chunk_emit(ctx->chunk, OP_PUSH_STR);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx);
    }
    cf_free(c);
}

static int try_const_int32(StradaValue *node, int32_t *out_imm) {
    VMConstFold cf;
    if (!try_eval_const_ast(node, &cf, 0)) return 0;
    int ok = (cf.kind == CF_INT && cf.iv >= INT32_MIN && cf.iv <= INT32_MAX);
    if (ok) *out_imm = (int32_t)cf.iv;
    cf_free(&cf);
    return ok;
}

static int try_const_str_key(CompCtx *ctx, StradaValue *node, size_t *out_kidx) {
    VMConstFold cf;
    if (!try_eval_const_ast(node, &cf, 0)) return 0;
    if (cf.kind == CF_STR) {
        *out_kidx = vm_chunk_add_str_const_len(ctx->chunk, cf.sv, cf.slen);
        cf_free(&cf);
        return 1;
    }
    cf_free(&cf);
    return 0;
}

/* Return 1 if node always produces a tagged/boxed int, 2 if always num/double, else 0. */
static int infer_expr_scalar_type(StradaValue *node) {
    node = ast_deref(node);
    if (!node || STRADA_IS_TAGGED_INT(node)) return 0;
    int t = (int)ast_int(node, "type");
    if (t == NI_INT_LITERAL) return 1;
    if (t == NI_NUM_LITERAL) return 2;
    if (t == NI_UNARY_OP) {
        const char *uop = ast_str(node, "op");
        if (strcmp(uop, "!") == 0 || strcmp(uop, "not") == 0 || strcmp(uop, "~") == 0) return 1;
        if (strcmp(uop, "-") == 0) return infer_expr_scalar_type(ast_get(node, "operand"));
        return 0;
    }
    if (t == NI_BINARY_OP) {
        int op = ast_op_code(node);
        if (op == OP_AST_DIV || op == OP_AST_POW) {
            int lt = infer_expr_scalar_type(ast_get(node, "left"));
            int rt = infer_expr_scalar_type(ast_get(node, "right"));
            if (lt != 0 && rt != 0) return 2;
            return 0;
        }
        if (op == OP_AST_BIT_AND || op == OP_AST_BIT_OR || op == OP_AST_BIT_XOR ||
            op == OP_AST_BIT_SHL || op == OP_AST_BIT_SHR) return 1;
        if ((op >= OP_AST_NUM_EQ && op <= OP_AST_NUM_GE) ||
            (op >= OP_AST_STR_EQ && op <= OP_AST_STR_GE) ||
            op == OP_AST_SPACESHIP || op == OP_AST_STR_CMP) return 1;
        if (op == OP_AST_ADD || op == OP_AST_SUB || op == OP_AST_MUL || op == OP_AST_MOD) {
            int lt = infer_expr_scalar_type(ast_get(node, "left"));
            int rt = infer_expr_scalar_type(ast_get(node, "right"));
            if (lt == 1 && rt == 1) return 1;
            if (op != OP_AST_MOD && (lt == 1 || lt == 2) && (rt == 1 || rt == 2) && (lt == 2 || rt == 2)) return 2;
            return 0;
        }
    }
    return 0;
}

/* Coerce top-of-stack value to the declared scalar storage/return type (int or num).
 * When preserve_blessed is non-zero (compound assignments), blessed objects retain
 * their overloaded object identity rather than being stripped to a primitive number. */
static void emit_scalar_type_coerce_ex(CompCtx *ctx, int var_type, int preserve_blessed) {
    if (var_type == 1) {
        vm_chunk_emit(ctx->chunk, preserve_blessed ? OP_CAST_ASSIGN_INT : OP_CAST_INT);
    } else if (var_type == 2) {
        vm_chunk_emit(ctx->chunk, preserve_blessed ? OP_CAST_ASSIGN_NUM : OP_CAST_NUM);
    }
}

static void emit_scalar_type_coerce(CompCtx *ctx, int var_type) {
    emit_scalar_type_coerce_ex(ctx, var_type, 0);
}

/* C structs (`struct P { int x; str s; }`) are modelled as hashes of their
 * fields; field stores mimic the C storage (int: 32-bit, num: double,
 * str: at most 63 bytes) as in the compiled backend. */
static StradaValue *g_vm_structs = NULL;
static char *prog_link_flags_pending = NULL;   /* program AST "structs" (borrowed) */

static StradaValue *vm_struct_def(const char *sname) {
    if (!sname || !sname[0] || !g_vm_structs) return NULL;
    StradaValue *m = ast_deref(g_vm_structs);
    if (!m || STRADA_IS_TAGGED_INT(m) || m->type != STRADA_HASH) return NULL;
    StradaValue *d = strada_hash_get(m->value.hv, sname);
    d = ast_deref(d);
    return (d && !STRADA_IS_TAGGED_INT(d) && d->type == STRADA_HASH) ? d : NULL;
}

/* 1 = int (32-bit), 2 = num, 3 = str (63 bytes), 0 = other/unknown */
static int vm_struct_type_kind(int64_t t) {
    if (t == 1 || t == 10 || t == 11 || t == 12 || t == 9) return 1;
    if (t == 2) return 2;
    if (t == 3) return 3;
    return 0;
}

static int vm_struct_field_kind(CompCtx *ctx, StradaValue *obj, const char *field) {
    StradaValue *o = ast_deref(obj);
    if (!o || STRADA_IS_TAGGED_INT(o) || (int)ast_int(o, "type") != NI_VARIABLE || !field) return 0;
    int slot = ctx_find_local_sigil(ctx, ast_str(o, "name"), '$');
    if (slot < 0 || !ctx->local_struct[slot]) return 0;
    StradaValue *def = vm_struct_def(ctx->local_struct[slot]);
    StradaValue *fm = def ? ast_deref(ast_get(def, "field_map")) : NULL;
    if (!fm || STRADA_IS_TAGGED_INT(fm) || fm->type != STRADA_HASH) return 0;
    StradaValue *fi = ast_deref(strada_hash_get(fm->value.hv, field));
    if (!fi || STRADA_IS_TAGGED_INT(fi)) return 0;
    return vm_struct_type_kind(ast_int(fi, "type"));
}

/* Push a new struct value: a hash with every field at its zero value. */
static void vm_emit_struct_new(CompCtx *ctx, const char *sname) {
    vm_chunk_emit(ctx->chunk, OP_NEW_HASH);
    vm_chunk_emit_u32(ctx->chunk, 4);
    StradaValue *def = vm_struct_def(sname);
    StradaValue *fl = def ? ast_deref(ast_get(def, "fields")) : NULL;
    if (!fl || STRADA_IS_TAGGED_INT(fl) || fl->type != STRADA_ARRAY) return;
    for (size_t i = 0; i < fl->value.av->size; i++) {
        StradaValue *fi = ast_deref(strada_array_get(fl->value.av, (int64_t)i));
        if (!fi || STRADA_IS_TAGGED_INT(fi) || fi->type != STRADA_HASH) continue;
        int k = vm_struct_type_kind(ast_int(fi, "type"));
        size_t kidx = vm_chunk_add_str_const(ctx->chunk, ast_str(fi, "name"));
        vm_chunk_emit(ctx->chunk, OP_DUP);
        if (k == 3) {
            size_t e = vm_chunk_add_str_const(ctx->chunk, "");
            vm_chunk_emit(ctx->chunk, OP_PUSH_STR);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)e);
        } else if (k == 1 || k == 2) {
            vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
            vm_chunk_emit_i64(ctx->chunk, 0);
        } else {
            vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
        }
        vm_chunk_emit(ctx->chunk, OP_HASH_SET_CONST);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)kidx);
    }
}

/* Does the AST under n assign the scalar variable `name` ($x = ..., $x op=,
 * ++/--, s///, tr///, chomp/chop)? Closures are scanned too (conservative). */
/* Does a map/grep block call anything (so another function could read $_)? */
static int vm_node_has_call(StradaValue *n, int depth) {
    n = ast_deref(n);
    if (!n || STRADA_IS_TAGGED_INT(n) || depth > 2000) return 0;
    if (n->type == STRADA_ARRAY) {
        StradaArray *av = n->value.av;
        for (size_t i = 0; av && i < av->size; i++)
            if (vm_node_has_call(av->elements[av->head + i], depth + 1)) return 1;
        return 0;
    }
    if (n->type != STRADA_HASH) return 0;
    if (strada_hash_get(n->value.hv, "type")) {
        int t = (int)ast_int(n, "type");
        if (t == NI_CALL || t == NI_METHOD_CALL || t == NI_CLOSURE_CALL || t == NI_SUPER_CALL || t == NI_DYN_METHOD_CALL)
            return 1;
    }
    StradaHash *hv = n->value.hv;
    for (uint32_t i = 0; hv && i < hv->next_slot; i++) {
        StradaValue *c = hv->entries[i].value;
        if (!hv->entries[i].key || !c || STRADA_IS_TAGGED_INT(c)) continue;
        if (c->type == STRADA_REF || c->type == STRADA_HASH || c->type == STRADA_ARRAY)
            if (vm_node_has_call(c, depth + 1)) return 1;
    }
    return 0;
}

static int vm_node_writes_var(StradaValue *n, const char *name, int depth) {
    n = ast_deref(n);
    if (!n || STRADA_IS_TAGGED_INT(n) || depth > 3000) return 0;
    if (n->type == STRADA_ARRAY) {
        StradaArray *av = n->value.av;
        for (size_t i = 0; av && i < av->size; i++)
            if (vm_node_writes_var(av->elements[av->head + i], name, depth + 1)) return 1;
        return 0;
    }
    if (n->type != STRADA_HASH) return 0;
    int t = (int)ast_int(n, "type");
    StradaValue *tg = NULL;
    if (t == NI_ASSIGN) tg = ast_get(n, "target");
    else if (t == NI_INCREMENT) tg = ast_get(n, "operand");
    else if (t == NI_REGEX_SUBST) tg = ast_get(n, "target");
    else if (t == NI_CALL) {
        const char *cn = ast_str(n, "name");
        if (cn && (strcmp(cn, "chomp") == 0 || strcmp(cn, "chop") == 0)) {
            StradaValue *ca = ast_get(n, "args");
            tg = ast_arr_get(ca, 0);
        }
    }
    if (tg) {
        StradaValue *td = ast_deref(tg);
        if (td && !STRADA_IS_TAGGED_INT(td) && (int)ast_int(td, "type") == NI_VARIABLE) {
            const char *tn = ast_str(td, "name");
            const char *ts = ast_str(td, "sigil");
            if (tn && strcmp(tn, name) == 0 && (!ts || !ts[0] || ts[0] == '$')) return 1;
        }
    }
    StradaHash *hv = n->value.hv;
    for (size_t i = 0; hv && i < hv->next_slot; i++) {
        if (!hv->entries[i].key || !hv->entries[i].value) continue;
        StradaValue *cv = hv->entries[i].value;
        if (STRADA_IS_TAGGED_INT(cv)) continue;
        if (cv->type == STRADA_HASH || cv->type == STRADA_ARRAY || cv->type == STRADA_REF)
            if (vm_node_writes_var(cv, name, depth + 1)) return 1;
    }
    return 0;
}

/* Does the program call tie() anywhere? (the compiled backend's Semantic
 * `uses_tie`; foreach does not restore its loop variable in such programs) */
static int g_vm_uses_tie = 0;
static int vm_ast_uses_tie(StradaValue *n, int depth) {
    n = ast_deref(n);
    if (!n || STRADA_IS_TAGGED_INT(n) || depth > 4000) return 0;
    if (n->type == STRADA_ARRAY) {
        StradaArray *av = n->value.av;
        for (size_t i = 0; av && i < av->size; i++)
            if (vm_ast_uses_tie(av->elements[av->head + i], depth + 1)) return 1;
        return 0;
    }
    if (n->type != STRADA_HASH) return 0;
    if ((int)ast_int(n, "type") == NI_CALL) {
        const char *cn = ast_str(n, "name");
        if (cn && strcmp(cn, "tie") == 0) return 1;
    }
    StradaHash *hv = n->value.hv;
    for (size_t i = 0; hv && i < hv->next_slot; i++) {
        if (!hv->entries[i].key || !hv->entries[i].value) continue;
        StradaValue *cv = hv->entries[i].value;
        if (STRADA_IS_TAGGED_INT(cv)) continue;
        if (cv->type == STRADA_HASH || cv->type == STRADA_ARRAY || cv->type == STRADA_REF)
            if (vm_ast_uses_tie(cv, depth + 1)) return 1;
    }
    return 0;
}

/* A one-character single-quoted literal stored into an int is its character
 * code ('(' -> 40), as in the compiled backend (has_char_code). */
static int vm_try_char_code(CompCtx *ctx, StradaValue *v) {
    StradaValue *d = ast_deref(v);
    if (!d || STRADA_IS_TAGGED_INT(d) || (int)ast_int(d, "type") != NI_STR_LITERAL) return 0;
    if (ast_int(d, "has_char_code") != 1) return 0;
    vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
    vm_chunk_emit_i64(ctx->chunk, ast_int(d, "char_code"));
    return 1;
}

static int vm_is_splice_call(StradaValue *v) {
    StradaValue *d = ast_deref(v);
    if (!d || STRADA_IS_TAGGED_INT(d) || (int)ast_int(d, "type") != NI_CALL) return 0;
    const char *n = ast_str(d, "name");
    return n && strcmp(n, "splice") == 0;
}

static void emit_local_restore(VMChunk *chunk, const char *lr_name) {
    if (strcmp(lr_name, "__HASH_ELEM__") == 0) {
        vm_chunk_emit(chunk, OP_RESTORE_HASH_ELEM);
    } else {
        size_t key_idx = vm_chunk_add_str_const(chunk, lr_name);
        vm_chunk_emit(chunk, OP_RESTORE_GLOBAL);
        vm_chunk_emit_u16(chunk, (uint16_t)key_idx);
    }
}

static void emit_clear_locals_range(VMChunk *chunk, int from_lc, int to_lc) {
    if (to_lc > from_lc && from_lc >= 0 && to_lc <= 0xFFFF) {
        vm_chunk_emit(chunk, OP_CLEAR_LOCALS);
        vm_chunk_emit_u16(chunk, (uint16_t)from_lc);
        vm_chunk_emit_u16(chunk, (uint16_t)to_lc);
    }
}

static void emit_reset_store_local(CompCtx *ctx, int slot) {
    vm_chunk_emit(ctx->chunk, OP_RESET_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
    vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
}

/* Extract container and key/index expressions from subscript or hash access nodes */
static int extract_cont_idx(StradaValue *node, int *is_hash, StradaValue **cont_node, StradaValue **idx_node) {
    int nt = (node && !STRADA_IS_TAGGED_INT(node)) ? (int)ast_int(node, "type") : 0;
    if (nt == NI_HASH_ACCESS) {
        *is_hash = 1;
        *cont_node = ast_get(node, "hash");
        *idx_node = ast_get(node, "key");
        return 1;
    } else if (nt == NI_DEREF_HASH) {
        *is_hash = 1;
        *cont_node = ast_get(node, "ref");
        if (!*cont_node) *cont_node = ast_get(node, "target");
        *idx_node = ast_get(node, "key");
        return 1;
    } else if (nt == NI_SUBSCRIPT) {
        *is_hash = 0;
        *cont_node = ast_get(node, "array");
        *idx_node = ast_get(node, "index");
        return 1;
    } else if (nt == NI_DEREF_ARRAY) {
        *is_hash = 0;
        *cont_node = ast_get(node, "ref");
        if (!*cont_node) *cont_node = ast_get(node, "target");
        *idx_node = ast_get(node, "index");
        return 1;
    }
    return 0;
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

/* split's pattern, rewritten as in the compiled backend (CodeGenBuiltins):
 * a /regex/ literal's i/x/s/m flags become an inline (?flags) prefix, `/^/`
 * means /^/m, and `/ /` is a single-space separator (only the string " " is
 * awk mode). */
static int compile_rx_dynamic_pattern(CompCtx *ctx, StradaValue *owner, const char *p, size_t len, const char *flags);
static int compile_regex_list(CompCtx *ctx, StradaValue *node);
static void compile_split_pattern(CompCtx *ctx, StradaValue *pat) {
    if (pat && !STRADA_IS_TAGGED_INT(pat) && (int)ast_int(pat, "type") == NI_STR_LITERAL
        && ast_int(pat, "is_regex") != 0) {
        size_t vlen = 0;
        const char *val = ast_str_len(pat, "value", &vlen);
        const char *fl = ast_str(pat, "regex_flags");
        /* split(/$sep/, ...): interpolated pattern */
        if (compile_rx_dynamic_pattern(ctx, pat, val, vlen, fl)) return;
        char inl[8]; size_t ni = 0;
        for (const char *f = fl ? fl : ""; *f && ni < 5; f++)
            if (*f == 'i' || *f == 'x' || *f == 's' || *f == 'm') inl[ni++] = *f;
        inl[ni] = '\0';
        if (vlen == 1 && val[0] == '^' && !strchr(inl, 'm')) { inl[ni++] = 'm'; inl[ni] = '\0'; }
        const char *out = NULL; size_t olen = 0; char *buf = NULL;
        if (ni > 0) {
            buf = (char *)malloc(vlen + ni + 4);
            if (!buf) { compile_expr(ctx, pat); return; }
            olen = (size_t)sprintf(buf, "(?%s)", inl);
            memcpy(buf + olen, val, vlen); olen += vlen;
            out = buf;
        } else if (vlen == 1 && val[0] == ' ') {
            out = "[ ]"; olen = 3;
        }
        if (out) {
            size_t idx = vm_chunk_add_str_const_len(ctx->chunk, out, olen);
            vm_chunk_emit(ctx->chunk, OP_PUSH_STR);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx);
            free(buf);
            return;
        }
    }
    compile_expr(ctx, pat);
}
static void compile_tr(CompCtx *ctx, StradaValue *node);
static size_t add_pattern_const(CompCtx *ctx, const char *pattern, size_t pat_len, const char *flags);
static void compile_stmt(CompCtx *ctx, StradaValue *node);
static void compile_list_src(CompCtx *ctx, StradaValue *e);
static int vm_is_hash_sigil_node(StradaValue *e);
static int vm_array_flatten_kind(CompCtx *ctx, StradaValue *elem);
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

static void ctx_push_local_restore(CompCtx *ctx, const char *key) {
    if (ctx->local_restore_count >= ctx->local_restore_cap) {
        int ncap = ctx->local_restore_cap ? ctx->local_restore_cap * 2 : 32;
        ctx->local_restores = realloc(ctx->local_restores, (size_t)ncap * sizeof(char *));
        ctx->local_restore_cap = ncap;
    }
    ctx->local_restores[ctx->local_restore_count++] = strdup(key);
}

static void ctx_free_local_restores(CompCtx *ctx) {
    for (int i = 0; i < ctx->local_restore_count; i++) free(ctx->local_restores[i]);
    free(ctx->local_restores);
    ctx->local_restores = NULL;
    ctx->local_restore_count = 0;
    ctx->local_restore_cap = 0;
}

static char **ctx_snapshot_local_restores(CompCtx *ctx, int count, char **stack_buf, size_t stack_len) {
    if (count <= 0) return NULL;
    char **ptrs = ((size_t)count <= stack_len) ? stack_buf : malloc((size_t)count * sizeof(char *));
    memcpy(ptrs, ctx->local_restores, (size_t)count * sizeof(char *));
    return ptrs;
}

/* Unwind all active try/finally contexts and local saves in the current function
 * before OP_RETURN. Expects return value on VM stack top, leaves it there. */
static void emit_try_unwind_for_return(CompCtx *ctx) {
    int saved_lr_count = ctx->local_restore_count;
    char *lr_sbuf[32];
    char **saved_lr_ptrs = ctx_snapshot_local_restores(ctx, saved_lr_count, lr_sbuf, 32);
    int cur_lr = saved_lr_count;

    if (!ctx->try_ctx) {
        while (cur_lr > 0) {
            emit_local_restore(ctx->chunk, saved_lr_ptrs[--cur_lr]);
        }
        if (saved_lr_ptrs && saved_lr_ptrs != lr_sbuf) free(saved_lr_ptrs);
        return;
    }

    int any_finally = 0;
    for (TryCtx *tc = ctx->try_ctx; tc != NULL; tc = tc->parent) {
        if (tc->finally_block) { any_finally = 1; break; }
    }
    int ret_slot = -1;
    if (any_finally) {
        ret_slot = ctx_get_or_add_local(ctx, "__ret_tmp", '$');
        emit_reset_store_local(ctx, ret_slot);
    }

    TryCtx *saved_try = ctx->try_ctx;
    int saved_sd = ctx->scope_depth;
    for (TryCtx *tc = saved_try; tc != NULL; tc = tc->parent) {
        while (cur_lr > tc->local_restore_depth) {
            emit_local_restore(ctx->chunk, saved_lr_ptrs[--cur_lr]);
        }
        for (int h = 0; h < tc->handlers_active; h++) {
            vm_chunk_emit(ctx->chunk, OP_TRY_END);
        }
        if (tc->finally_block) {
            ctx->try_ctx = tc->parent;
            ctx->local_restore_count = cur_lr;
            ctx->scope_depth = tc->enclosing_scope_depth;
            for (int j = cur_lr; j < saved_lr_count; j++) ctx->local_restores[j] = NULL;
            compile_block(ctx, tc->finally_block);
        }
    }
    ctx->scope_depth = saved_sd;
    ctx->try_ctx = saved_try;
    ctx->local_restore_count = saved_lr_count;
    if (saved_lr_count > 0) {
        memcpy(ctx->local_restores, saved_lr_ptrs, (size_t)saved_lr_count * sizeof(char *));
    }
    while (cur_lr > 0) {
        emit_local_restore(ctx->chunk, saved_lr_ptrs[--cur_lr]);
    }
    if (saved_lr_ptrs && saved_lr_ptrs != lr_sbuf) free(saved_lr_ptrs);
    if (any_finally) {
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)ret_slot);
    }
}

/* Unwind active try/finally contexts and local saves nested inside `target` loop before last/next/redo. */
static void emit_try_unwind_for_loop(CompCtx *ctx, LoopCtx *target) {
    if (!target) return;
    int saved_lr_count = ctx->local_restore_count;
    char *lr_sbuf[32];
    char **saved_lr_ptrs = ctx_snapshot_local_restores(ctx, saved_lr_count, lr_sbuf, 32);
    int cur_lr = saved_lr_count;

    if (ctx->try_ctx) {
        TryCtx *saved_try = ctx->try_ctx;
        int saved_sd = ctx->scope_depth;
        for (TryCtx *tc = saved_try; tc != NULL && loop_in_chain(tc->enclosing_loop, target); tc = tc->parent) {
            while (cur_lr > tc->local_restore_depth && cur_lr > target->local_restore_depth) {
                emit_local_restore(ctx->chunk, saved_lr_ptrs[--cur_lr]);
            }
            for (int h = 0; h < tc->handlers_active; h++) {
                vm_chunk_emit(ctx->chunk, OP_TRY_END);
            }
            if (tc->finally_block) {
                ctx->try_ctx = tc->parent;
                ctx->local_restore_count = cur_lr;
                ctx->scope_depth = tc->enclosing_scope_depth;
                for (int j = cur_lr; j < saved_lr_count; j++) ctx->local_restores[j] = NULL;
                compile_block(ctx, tc->finally_block);
            }
        }
        ctx->scope_depth = saved_sd;
        ctx->try_ctx = saved_try;
        ctx->local_restore_count = saved_lr_count;
        if (saved_lr_count > 0) {
            memcpy(ctx->local_restores, saved_lr_ptrs, (size_t)saved_lr_count * sizeof(char *));
        }
    }
    while (cur_lr > target->local_restore_depth) {
        emit_local_restore(ctx->chunk, saved_lr_ptrs[--cur_lr]);
    }
    if (saved_lr_ptrs && saved_lr_ptrs != lr_sbuf) free(saved_lr_ptrs);
}

static VMLabelDef *vm_ctx_find_label(CompCtx *ctx, const char *name) {
    if (!name || !name[0]) return NULL;
    for (int i = 0; i < ctx->label_count; i++) {
        if (ctx->labels[i].name && strcmp(ctx->labels[i].name, name) == 0) {
            return &ctx->labels[i];
        }
    }
    return NULL;
}

static VMLabelDef *vm_ctx_ensure_label(CompCtx *ctx, const char *name) {
    VMLabelDef *existing = vm_ctx_find_label(ctx, name);
    if (existing) return existing;
    if (ctx->label_count >= ctx->label_cap) {
        ctx->label_cap = ctx->label_cap ? ctx->label_cap * 2 : 16;
        ctx->labels = realloc(ctx->labels, (size_t)ctx->label_cap * sizeof(VMLabelDef));
    }
    VMLabelDef *lbl = &ctx->labels[ctx->label_count++];
    memset(lbl, 0, sizeof(*lbl));
    lbl->name = strdup(name);
    return lbl;
}

static void vm_label_record_scope(VMLabelDef *lbl, void **scope_stack, int scope_d) {
    int n = scope_d < VM_MAX_GOTO_SCOPE ? scope_d : VM_MAX_GOTO_SCOPE;
    lbl->scope_depth = n;
    for (int i = 0; i < n; i++) {
        lbl->scope_path[i] = scope_stack[i];
    }
}

static int ast_may_box_locals(StradaValue *node, int depth) {
    if (!node || STRADA_IS_TAGGED_INT(node)) return 0;
    if (depth >= 63) return 1;
    node = ast_deref(node);
    if (!node || STRADA_IS_TAGGED_INT(node)) return 0;
    if (node->type == STRADA_ARRAY) {
        StradaArray *av = node->value.av;
        if (!av) return 0;
        int len = strada_array_length(av);
        for (int i = 0; i < len; i++) {
            if (ast_may_box_locals(strada_array_get(av, i), depth + 1)) return 1;
        }
        return 0;
    }
    if (node->type != STRADA_HASH || !node->value.hv) return 0;
    int type = (int)ast_int(node, "type");
    if (type == NI_ANON_FUNC || type == NI_ASYNC_FUNC || type == NI_REF || type == NI_SORT) {
        return 1;
    }
    if (type == NI_UNARY_OP) {
        const char *op = ast_str(node, "op");
        if (op && op[0] == '\\') return 1;
    } else if (type == NI_CALL) {
        const char *name = ast_str(node, "name");
        if (name && (strcmp(name, "tie") == 0 || strcmp(name, "refto") == 0)) return 1;
    }
    StradaHash *hv = node->value.hv;
    for (size_t i = 0; i < hv->next_slot; i++) {
        if (hv->entries[i].key != NULL && ast_may_box_locals(hv->entries[i].value, depth + 1)) {
            return 1;
        }
    }
    return 0;
}

static int prescan_vm_labels_ex(CompCtx *ctx, StradaValue *node, int try_h, int local_d,
                                void **scope_stack, int scope_d) {
    if (!node || STRADA_IS_TAGGED_INT(node)) return local_d;
    node = ast_deref(node);
    if (!node || STRADA_IS_TAGGED_INT(node)) return local_d;
    if (node->type == STRADA_ARRAY) {
        StradaArray *av = node->value.av;
        if (av) {
            int len = strada_array_length(av);
            for (int i = 0; i < len; i++) {
                prescan_vm_labels_ex(ctx, strada_array_get(av, i), try_h, local_d, scope_stack, scope_d);
            }
        }
        return local_d;
    }
    if (node->type != STRADA_HASH) return local_d;

    int type = (int)ast_int(node, "type");
    switch (type) {
    case NI_ANON_FUNC:
    case NI_ASYNC_FUNC:
        return local_d;
    case NI_SORT:
        prescan_vm_labels_ex(ctx, ast_get(node, "array"), try_h, local_d, scope_stack, scope_d);
        return local_d;
    case NI_MAP:
    case NI_GREP: {
        prescan_vm_labels_ex(ctx, ast_get(node, "array"), try_h, local_d, scope_stack, scope_d);
        StradaValue *blk = ast_get(node, "block");
        if (!blk || STRADA_IS_TAGGED_INT(blk)) blk = ast_get(node, "body");
        prescan_vm_labels_ex(ctx, blk, try_h, local_d, scope_stack, scope_d);
        return local_d;
    }
    case NI_DO_EXPR: {
        StradaValue *blk = ast_get(node, "block");
        if (!blk || STRADA_IS_TAGGED_INT(blk)) blk = ast_get(node, "body");
        prescan_vm_labels_ex(ctx, blk, try_h, local_d, scope_stack, scope_d);
        return local_d;
    }
    case NI_LOCAL_DECL:
        return (local_d < 32) ? local_d + 1 : local_d;
    case NI_LABEL: {
        const char *name = ast_str(node, "name");
        if (name && name[0]) {
            VMLabelDef *lbl = vm_ctx_ensure_label(ctx, name);
            lbl->try_handler_depth = try_h;
            lbl->local_restore_depth = local_d;
            vm_label_record_scope(lbl, scope_stack, scope_d);
        }
        return prescan_vm_labels_ex(ctx, ast_get(node, "statement"), try_h, local_d, scope_stack, scope_d);
    }
    case NI_BLOCK: {
        int pushed = 0;
        if (scope_d < VM_MAX_GOTO_SCOPE) {
            scope_stack[scope_d++] = (void *)node;
            pushed = 1;
        }
        StradaValue *stmts = ast_get(node, "statements");
        int count = (int)ast_int(node, "statement_count");
        int cur_ld = local_d;
        for (int i = 0; i < count; i++) {
            cur_ld = prescan_vm_labels_ex(ctx, ast_arr_get(stmts, i), try_h, cur_ld, scope_stack, scope_d);
        }
        if (pushed) scope_d--;
        return local_d;
    }
    case NI_IF_STMT: {
        prescan_vm_labels_ex(ctx, ast_get(node, "condition"), try_h, local_d, scope_stack, scope_d);
        prescan_vm_labels_ex(ctx, ast_get(node, "then_block"), try_h, local_d, scope_stack, scope_d);
        StradaValue *elsif_conds = ast_get(node, "elsif_conditions");
        StradaValue *elsif_blocks = ast_get(node, "elsif_blocks");
        int elsif_count = (int)ast_int(node, "elsif_count");
        for (int i = 0; i < elsif_count; i++) {
            if (elsif_conds) prescan_vm_labels_ex(ctx, ast_arr_get(elsif_conds, i), try_h, local_d, scope_stack, scope_d);
            prescan_vm_labels_ex(ctx, ast_arr_get(elsif_blocks, i), try_h, local_d, scope_stack, scope_d);
        }
        prescan_vm_labels_ex(ctx, ast_get(node, "else_block"), try_h, local_d, scope_stack, scope_d);
        return local_d;
    }
    case NI_WHILE_STMT:
    case NI_DO_WHILE_STMT: {
        const char *lbl_name = ast_str(node, "label");
        if (lbl_name && lbl_name[0]) {
            VMLabelDef *lbl = vm_ctx_ensure_label(ctx, lbl_name);
            lbl->try_handler_depth = try_h;
            lbl->local_restore_depth = local_d;
            vm_label_record_scope(lbl, scope_stack, scope_d);
        }
        prescan_vm_labels_ex(ctx, ast_get(node, "condition"), try_h, local_d, scope_stack, scope_d);
        prescan_vm_labels_ex(ctx, ast_get(node, "body"), try_h, local_d, scope_stack, scope_d);
        return local_d;
    }
    case NI_FOREACH_STMT: {
        const char *lbl_name = ast_str(node, "label");
        if (lbl_name && lbl_name[0]) {
            VMLabelDef *lbl = vm_ctx_ensure_label(ctx, lbl_name);
            lbl->try_handler_depth = try_h;
            lbl->local_restore_depth = local_d;
            vm_label_record_scope(lbl, scope_stack, scope_d);
        }
        prescan_vm_labels_ex(ctx, ast_get(node, "array"), try_h, local_d, scope_stack, scope_d);
        prescan_vm_labels_ex(ctx, ast_get(node, "iterable"), try_h, local_d, scope_stack, scope_d);
        int pushed = 0;
        if (scope_d < VM_MAX_GOTO_SCOPE) {
            scope_stack[scope_d++] = (void *)node;
            pushed = 1;
        }
        prescan_vm_labels_ex(ctx, ast_get(node, "body"), try_h, local_d, scope_stack, scope_d);
        if (pushed) scope_d--;
        return local_d;
    }
    case NI_FOR_STMT: {
        const char *lbl_name = ast_str(node, "label");
        if (lbl_name && lbl_name[0]) {
            VMLabelDef *lbl = vm_ctx_ensure_label(ctx, lbl_name);
            lbl->try_handler_depth = try_h;
            lbl->local_restore_depth = local_d;
            vm_label_record_scope(lbl, scope_stack, scope_d);
        }
        int pushed = 0;
        if (scope_d < VM_MAX_GOTO_SCOPE) {
            scope_stack[scope_d++] = (void *)node;
            pushed = 1;
        }
        int cur_ld = prescan_vm_labels_ex(ctx, ast_get(node, "init"), try_h, local_d, scope_stack, scope_d);
        prescan_vm_labels_ex(ctx, ast_get(node, "condition"), try_h, cur_ld, scope_stack, scope_d);
        prescan_vm_labels_ex(ctx, ast_get(node, "increment"), try_h, cur_ld, scope_stack, scope_d);
        prescan_vm_labels_ex(ctx, ast_get(node, "body"), try_h, cur_ld, scope_stack, scope_d);
        if (pushed) scope_d--;
        return local_d;
    }
    case NI_SWITCH: {
        prescan_vm_labels_ex(ctx, ast_get(node, "expr"), try_h, local_d, scope_stack, scope_d);
        StradaValue *blocks = ast_get(node, "blocks");
        int case_count = (int)ast_int(node, "case_count");
        for (int i = 0; i < case_count; i++) {
            prescan_vm_labels_ex(ctx, ast_arr_get(blocks, i), try_h, local_d, scope_stack, scope_d);
        }
        prescan_vm_labels_ex(ctx, ast_get(node, "default_block"), try_h, local_d, scope_stack, scope_d);
        return local_d;
    }
    case NI_TRY_CATCH: {
        StradaValue *catches = ast_get(node, "catch_clauses");
        int catch_count = (int)ast_int(node, "catch_count");
        StradaValue *finally_block = ast_get(node, "finally_block");
        int has_finally = (finally_block && !STRADA_IS_TAGGED_INT(finally_block)) ? 1 : 0;
        int try_inner_h = try_h + ((has_finally && catch_count > 0) ? 2 : 1);
        prescan_vm_labels_ex(ctx, ast_get(node, "try_block"), try_inner_h, local_d, scope_stack, scope_d);
        int catch_inner_h = try_h + (has_finally ? 1 : 0);
        for (int i = 0; i < catch_count; i++) {
            StradaValue *cc = ast_deref(ast_arr_get(catches, i));
            int pushed = 0;
            if (cc && scope_d < VM_MAX_GOTO_SCOPE) {
                scope_stack[scope_d++] = (void *)cc;
                pushed = 1;
            }
            prescan_vm_labels_ex(ctx, ast_get(cc, "catch_block"), catch_inner_h, local_d, scope_stack, scope_d);
            if (pushed) scope_d--;
        }
        if (has_finally) {
            prescan_vm_labels_ex(ctx, finally_block, try_h, local_d, scope_stack, scope_d);
        }
        return local_d;
    }
    default: {
        StradaHash *hv = node->value.hv;
        if (hv) {
            for (size_t i = 0; i < hv->next_slot; i++) {
                if (!hv->entries[i].key) continue;
                StradaValue *cv = ast_deref(hv->entries[i].value);
                if (cv && !STRADA_IS_TAGGED_INT(cv) &&
                    (cv->type == STRADA_HASH || cv->type == STRADA_ARRAY)) {
                    prescan_vm_labels_ex(ctx, cv, try_h, local_d, scope_stack, scope_d);
                }
            }
        }
        return local_d;
    }
    }
}

static int prescan_vm_labels(CompCtx *ctx, StradaValue *node, int try_h, int local_d) {
    void *scope_stack[VM_MAX_GOTO_SCOPE];
    return prescan_vm_labels_ex(ctx, node, try_h, local_d, scope_stack, 0);
}

static void emit_try_unwind_to_depth(CompCtx *ctx, int target_try_h, int target_local_d) {
    int saved_lr_count = ctx->local_restore_count;
    char *lr_sbuf[32];
    char **saved_lr_ptrs = ctx_snapshot_local_restores(ctx, saved_lr_count, lr_sbuf, 32);
    int cur_lr = saved_lr_count;

    int cur_try_h = 0;
    for (TryCtx *tc = ctx->try_ctx; tc != NULL; tc = tc->parent) {
        cur_try_h += tc->handlers_active;
    }

    if (ctx->try_ctx && cur_try_h > target_try_h) {
        TryCtx *saved_try = ctx->try_ctx;
        int saved_sd = ctx->scope_depth;
        int remaining_h = cur_try_h;
        for (TryCtx *tc = saved_try; tc != NULL && remaining_h > target_try_h; tc = tc->parent) {
            while (cur_lr > tc->local_restore_depth && cur_lr > target_local_d) {
                emit_local_restore(ctx->chunk, saved_lr_ptrs[--cur_lr]);
            }
            int to_pop = tc->handlers_active;
            if (remaining_h - to_pop < target_try_h) {
                to_pop = remaining_h - target_try_h;
            }
            for (int h = 0; h < to_pop; h++) {
                vm_chunk_emit(ctx->chunk, OP_TRY_END);
            }
            remaining_h -= to_pop;
            if (tc->finally_block && remaining_h >= target_try_h) {
                ctx->try_ctx = tc->parent;
                ctx->local_restore_count = cur_lr;
                ctx->scope_depth = tc->enclosing_scope_depth;
                for (int j = cur_lr; j < saved_lr_count; j++) ctx->local_restores[j] = NULL;
                compile_block(ctx, tc->finally_block);
            }
        }
        ctx->scope_depth = saved_sd;
        ctx->try_ctx = saved_try;
        ctx->local_restore_count = saved_lr_count;
        if (saved_lr_count > 0) {
            memcpy(ctx->local_restores, saved_lr_ptrs, (size_t)saved_lr_count * sizeof(char *));
        }
    }
    while (cur_lr > target_local_d) {
        emit_local_restore(ctx->chunk, saved_lr_ptrs[--cur_lr]);
    }
    if (saved_lr_ptrs && saved_lr_ptrs != lr_sbuf) free(saved_lr_ptrs);
}

static void resolve_vm_gotos(CompCtx *ctx) {
    for (int i = 0; i < ctx->goto_count; i++) {
        VMLabelDef *lbl = vm_ctx_find_label(ctx, ctx->gotos[i].name);
        if (!lbl || !lbl->has_offset) {
            fprintf(stderr, "VM compile error: unresolved goto label '%s'\n",
                    ctx->gotos[i].name ? ctx->gotos[i].name : "");
            ctx->errors++;
        } else {
            patch_u16(ctx, ctx->gotos[i].patch_off, (uint16_t)lbl->offset);
        }
        free(ctx->gotos[i].name);
    }
    free(ctx->gotos);
    ctx->gotos = NULL;
    ctx->goto_count = ctx->goto_cap = 0;
    for (int i = 0; i < ctx->label_count; i++) {
        free(ctx->labels[i].name);
    }
    free(ctx->labels);
    ctx->labels = NULL;
    ctx->label_count = ctx->label_cap = 0;
}

static void emit_param_defaults(CompCtx *ctx, StradaValue *params, int param_count, int is_synth, int func_idx) {
    for (int i = 0; i < param_count; i++) {
        StradaValue *param = ast_arr_get(params, i);
        const char *psigil = ast_str(param, "sigil");
        int is_variadic = (int)ast_int(param, "is_variadic");
        if (is_variadic || strcmp(psigil, "...@") == 0 || (is_synth && psigil[0] == '@')) {
            continue;
        }
        int has_def = (int)ast_int(param, "has_default") || (int)ast_int(param, "is_optional");
        StradaValue *def_expr = ast_deref(ast_get(param, "default"));
        if (!def_expr || STRADA_IS_TAGGED_INT(def_expr) || def_expr->type == STRADA_UNDEF) {
            def_expr = ast_deref(ast_get(param, "default_value"));
        }
        if (has_def && def_expr && !STRADA_IS_TAGGED_INT(def_expr) && def_expr->type == STRADA_HASH) {
            vm_chunk_emit(ctx->chunk, OP_ARG_PRESENT);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)i);
            vm_chunk_emit(ctx->chunk, OP_JMP_IF_TRUE);
            size_t skip_patch = emit_patch_u16(ctx);
            compile_expr(ctx, def_expr);
            if (func_idx >= 0) ctx->chunk = &ctx->prog->funcs[func_idx];
            vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)i);
            patch_u16(ctx, skip_patch, (uint16_t)ctx->chunk->code_len);
        }
    }
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

static void emit_default_scalar_init(CompCtx *ctx, int64_t var_type) {
    if (var_type == 1) {
        emit_push_int(ctx->chunk, 0);
    } else if (var_type == 2) {
        vm_chunk_emit(ctx->chunk, OP_PUSH_FLOAT);
        vm_chunk_emit_f64(ctx->chunk, 0.0);
    } else if (var_type == 3) {
        size_t idx = vm_chunk_add_str_const(ctx->chunk, "");
        vm_chunk_emit(ctx->chunk, OP_PUSH_STR);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx);
    } else {
        vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
    }
}

static void compile_live_scalar_ref(CompCtx *ctx, StradaValue *target) {
    ctx->has_non_scalar = 1;
    StradaValue *t = ast_deref(target);
    if (t && !STRADA_IS_TAGGED_INT(t) && (int)ast_int(t, "type") == NI_VARIABLE) {
        const char *vname = ast_str(t, "name");
        const char *vsigil = ast_str(t, "sigil");
        char sc = (vsigil && vsigil[0]) ? vsigil[0] : '$';
        if (sc == '$' || sc == 's' || sc == 0) sc = '$';
        if (sc == '$') {
            int slot = ctx_find_local_sigil(ctx, vname, '$');
            if (slot >= 0) {
                vm_chunk_emit(ctx->chunk, OP_MAKE_SREF_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                return;
            }
            int cap = ctx_resolve_capture(ctx, vname, '$');
            if (cap >= 0) {
                vm_chunk_emit(ctx->chunk, OP_LOAD_CAPTURE_CELL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)cap);
                vm_chunk_emit(ctx->chunk, OP_MAKE_SREF_VAL);
                return;
            }
            const char *gkey = resolve_global_var_key(ctx, vname);
            if (gkey) {
                size_t gk = vm_chunk_add_str_const(ctx->chunk, gkey);
                vm_chunk_emit(ctx->chunk, OP_MAKE_SREF_GLOBAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)gk);
                return;
            }
        } else {
            compile_expr(ctx, t);
            vm_chunk_emit(ctx->chunk, OP_HASH_REF);
            return;
        }
    }
    compile_expr(ctx, target);
    vm_chunk_emit(ctx->chunk, OP_MAKE_SREF_VAL);
}

static void compile_tie_target(CompCtx *ctx, StradaValue *arg0) {
    StradaValue *t = ast_deref(arg0);
    if (t && !STRADA_IS_TAGGED_INT(t)) {
        int tt = (int)ast_int(t, "type");
        if (tt == NI_VARIABLE) {
            const char *vsigil = ast_str(t, "sigil");
            char sc = (vsigil && vsigil[0]) ? vsigil[0] : '$';
            if (sc == '$' || sc == 's' || sc == 0) {
                compile_live_scalar_ref(ctx, t);
                return;
            }
        } else if (tt == NI_DEREF_SCALAR) {
            StradaValue *ref_node = ast_get(t, "ref");
            if (!ref_node) ref_node = ast_get(t, "target");
            compile_expr(ctx, ref_node);
            return;
        }
    }
    compile_expr(ctx, arg0);
}

static void compile_expr_in_ctx(CompCtx *ctx, StradaValue *expr, int call_ctx) {
    int saved_cc = ctx->call_context;
    ctx->call_context = call_ctx;
    compile_expr(ctx, expr);
    ctx->call_context = saved_cc;
}

static void emit_call_context(CompCtx *ctx, int call_ctx) {
    if (call_ctx != 0) {
        vm_chunk_emit(ctx->chunk, OP_SET_CALL_CTX);
        vm_chunk_emit(ctx->chunk, (uint8_t)call_ctx);
    }
}

/* $s =~ /re/ in list context (compiled: list_ctx -> strada_regex_list): the
 * capture groups, (1) without groups, every match with /g; () on failure. */
static int compile_regex_list(CompCtx *ctx, StradaValue *node) {
    StradaValue *d = ast_deref(node);
    if (!d || STRADA_IS_TAGGED_INT(d) || (int)ast_int(d, "type") != NI_REGEX_MATCH) return 0;
    const char *op = ast_str(d, "op");
    if (!op || strcmp(op, "=~") != 0) return 0;
    size_t plen = 0;
    const char *pat = ast_str_len(d, "pattern", &plen);
    const char *fl = ast_str(d, "flags");
    char f2[16]; int fi = 0, glob = 0;
    for (const char *f = fl ? fl : ""; *f && fi < 15; f++) {
        if (*f == 'g') glob = 1; else f2[fi++] = *f;
    }
    f2[fi] = 0;
    compile_expr(ctx, ast_get(d, "target"));
    if (!compile_rx_dynamic_pattern(ctx, d, pat, plen, NULL)) {
        size_t pidx = vm_chunk_add_str_const_len(ctx->chunk, pat ? pat : "", plen);
        vm_chunk_emit(ctx->chunk, OP_PUSH_STR);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)pidx);
    }
    size_t fidx = vm_chunk_add_str_const(ctx->chunk, f2);
    vm_chunk_emit(ctx->chunk, OP_PUSH_STR);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)fidx);
    vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
    vm_chunk_emit_i64(ctx->chunk, glob);
    vm_chunk_emit(ctx->chunk, OP_BUILTIN);
    vm_chunk_emit_u16(ctx->chunk, BUILTIN_REGEX_LIST);
    vm_chunk_emit(ctx->chunk, 4);
    return 1;
}

static void compile_array_init_expr(CompCtx *ctx, StradaValue *node, StradaValue *init, int shallow_copy) {
    int init_type = (init && !STRADA_IS_TAGGED_INT(init)) ? (int)ast_int(init, "type") : 0;
    /* () is parsed as empty ANON_HASH — treat as empty array for @ sigil */
    if (init_type == NI_ANON_HASH && ast_int(init, "is_empty_list") == 1) {
        init_type = 0;
    }
    if ((init_type == NI_VARIABLE || init_type == NI_DEREF_SCALAR) && ast_str(init, "sigil")[0] == '%') {
        /* @a = %h: the key/value pairs (a fresh array) */
        compile_list_src(ctx, init);
        return;
    }
    if (init_type == NI_REGEX_MATCH && compile_regex_list(ctx, init)) {
        return;   /* my @m = $s =~ /(..)/g: the captures */
    }
    if (init_type == NI_BINARY_OP && ast_str(init, "op") && strcmp(ast_str(init, "op"), "x") == 0) {
        StradaValue *rl = ast_deref(ast_get(init, "left"));
        int rlt = (rl && !STRADA_IS_TAGGED_INT(rl)) ? (int)ast_int(rl, "type") : 0;
        if ((rlt == NI_ANON_ARRAY && ast_int(rl, "is_list") == 1) || (rl && !STRADA_IS_TAGGED_INT(rl) && ast_int(rl, "in_parens") == 1)) {
            /* my @a = (LIST) x N / (X) x N */
            if (rlt == NI_ANON_ARRAY && ast_int(rl, "is_list") == 1) {
                compile_expr(ctx, rl);
            } else {
                vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
                vm_chunk_emit_u32(ctx->chunk, 1);
                vm_chunk_emit(ctx->chunk, OP_DUP);
                compile_list_src(ctx, rl);
                vm_chunk_emit(ctx->chunk, vm_array_flatten_kind(ctx, rl) != 0 ? OP_ARRAY_EXTEND : OP_ARRAY_PUSH);
            }
            compile_expr(ctx, ast_get(init, "right"));
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_LIST_REPEAT);
            vm_chunk_emit(ctx->chunk, 2);
            return;
        }
    }
    /* my @b = ([1, 2]) / ({...}) / (\$v): a parenthesized single reference is
     * ONE element; a bare `my @a = [1, 2]` copies the elements (Strada) */
    if (((init_type == NI_ANON_ARRAY && ast_int(init, "is_list") == 0) || init_type == NI_ANON_HASH || init_type == NI_REF)
        && ast_int(init, "in_parens") == 1) {
        if (!(init_type == NI_ANON_HASH && ast_int(init, "is_empty_list") == 1)) {
            /* my @b = ([1, 2]) / ({...}) / (\$v): ONE element, a reference */
            vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
            vm_chunk_emit_u32(ctx->chunk, 1);
            vm_chunk_emit(ctx->chunk, OP_DUP);
            compile_expr(ctx, init);
            vm_chunk_emit(ctx->chunk, OP_ARRAY_PUSH);
            return;
        }
    }
    if (init_type != 0) {
        compile_expr_in_ctx(ctx, init, 1);
        if (shallow_copy && init_type != NI_ANON_ARRAY) {
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_ARRAY_SHALLOW_COPY);
            vm_chunk_emit(ctx->chunk, 1);
        }
    } else {
        int64_t cap = node ? ast_int(node, "initial_capacity") : 0;
        vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
        vm_chunk_emit_u32(ctx->chunk, cap > 0 ? (uint32_t)cap : 4);
    }
}

static int vm_list_has_flatten(CompCtx *ctx, StradaValue *lst) {
    StradaValue *elems = ast_deref(ast_get(lst, "elements"));
    int n = (int)ast_int(lst, "element_count");
    for (int i = 0; i < n; i++) {
        StradaValue *e = ast_deref(ast_arr_get(elems, i));
        if (!e || STRADA_IS_TAGGED_INT(e)) continue;
        int t = (int)ast_int(e, "type");
        if (t == NI_SPREAD || vm_array_flatten_kind(ctx, e) != 0) return 1;
    }
    return 0;
}

static void compile_hash_init_expr(CompCtx *ctx, StradaValue *node, StradaValue *init, int shallow_copy) {
    if (init && !STRADA_IS_TAGGED_INT(init) && (int)ast_int(init, "type") != 0) {
        init = ast_deref(init);
        int init_type = (int)ast_int(init, "type");
        if (init_type == NI_ANON_HASH) {
            compile_expr(ctx, init);
        } else if (init_type == NI_ANON_ARRAY && vm_list_has_flatten(ctx, init)) {
            /* (%a, k => v) / (@l, ...): build the flat list, then pair it */
            compile_expr(ctx, init);
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_HASH_SHALLOW_COPY);
            vm_chunk_emit(ctx->chunk, 1);
        } else if (init_type == NI_ANON_ARRAY) {
            StradaValue *elems = ast_deref(ast_get(init, "elements"));
            int elem_count = (int)ast_int(init, "element_count");
            vm_chunk_emit(ctx->chunk, OP_NEW_HASH);
            vm_chunk_emit_u32(ctx->chunk, elem_count > 0 ? elem_count : 4);
            for (int ei = 0; ei < elem_count; ei++) {
                StradaValue *elem = ast_deref(ast_arr_get(elems, ei));
                int et = elem ? (int)ast_int(elem, "type") : 0;
                if (et == NI_ANON_ARRAY) {
                    StradaValue *pair_elems = ast_deref(ast_get(elem, "elements"));
                    int pair_count = (int)ast_int(elem, "element_count");
                    if (pair_count >= 2) {
                        vm_chunk_emit(ctx->chunk, OP_DUP);
                        size_t kidx = 0;
                        if (try_const_str_key(ctx, ast_arr_get(pair_elems, 0), &kidx)) {
                            compile_expr(ctx, ast_arr_get(pair_elems, 1));
                            vm_chunk_emit(ctx->chunk, OP_HASH_SET_CONST);
                            vm_chunk_emit_u16(ctx->chunk, (uint16_t)kidx);
                        } else {
                            compile_expr(ctx, ast_arr_get(pair_elems, 0));
                            compile_expr(ctx, ast_arr_get(pair_elems, 1));
                            vm_chunk_emit(ctx->chunk, OP_HASH_SET);
                        }
                    }
                } else if (ei + 1 < elem_count) {
                    vm_chunk_emit(ctx->chunk, OP_DUP);
                    size_t kidx = 0;
                    if (try_const_str_key(ctx, elem, &kidx)) {
                        compile_expr(ctx, ast_arr_get(elems, ei + 1));
                        vm_chunk_emit(ctx->chunk, OP_HASH_SET_CONST);
                        vm_chunk_emit_u16(ctx->chunk, (uint16_t)kidx);
                    } else {
                        compile_expr(ctx, elem);
                        compile_expr(ctx, ast_arr_get(elems, ei + 1));
                        vm_chunk_emit(ctx->chunk, OP_HASH_SET);
                    }
                    ei++;
                }
            }
        } else {
            compile_expr_in_ctx(ctx, init, 2);
            if (shallow_copy) {
                vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                vm_chunk_emit_u16(ctx->chunk, BUILTIN_HASH_SHALLOW_COPY);
                vm_chunk_emit(ctx->chunk, 1);
            }
        }
    } else {
        int64_t cap = node ? ast_int(node, "initial_capacity") : 0;
        vm_chunk_emit(ctx->chunk, OP_NEW_HASH);
        vm_chunk_emit_u32(ctx->chunk, cap > 0 ? (uint32_t)cap : 4);
    }
}

static void compile_assign_var_rhs(CompCtx *ctx, char sigil, StradaValue *value) {
    if (sigil == '@') {
        compile_array_init_expr(ctx, NULL, value, 1);
    } else if (sigil == '%') {
        compile_hash_init_expr(ctx, NULL, value, 1);
    } else {
        compile_expr(ctx, value);
    }
}

static void emit_in_place_xform(CompCtx *ctx, int kind);
static void compile_in_place_str_op(CompCtx *ctx, StradaValue *arg0, int is_chop) {
    StradaValue *t = ast_deref(arg0);
    int ttype = (t && !STRADA_IS_TAGGED_INT(t)) ? (int)ast_int(t, "type") : 0;
    StradaValue *cont = NULL, *idx = NULL;
    int is_hash = 0;

    if (ttype == NI_VARIABLE) {
        const char *vname = ast_str(t, "name");
        const char *vsigil = ast_str(t, "sigil");
        char sc = (vsigil && vsigil[0]) ? vsigil[0] : '$';
        int slot = ctx_find_local_sigil(ctx, vname, sc);
        int cap = (slot < 0) ? ctx_resolve_capture(ctx, vname, sc) : -1;
        const char *gkey = (slot < 0 && cap < 0) ? resolve_global_var_key(ctx, vname) : NULL;
        if (slot >= 0 || cap >= 0 || gkey != NULL) {
            if (slot >= 0) {
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
            } else if (cap >= 0) {
                vm_chunk_emit(ctx->chunk, OP_LOAD_CAPTURE);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)cap);
            } else {
                size_t gk = vm_chunk_add_str_const(ctx->chunk, gkey);
                vm_chunk_emit(ctx->chunk, OP_LOAD_GLOBAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)gk);
            }
            emit_in_place_xform(ctx, is_chop);
            vm_chunk_emit(ctx->chunk, OP_DUP);
            if (slot >= 0) {
                vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
            } else if (cap >= 0) {
                vm_chunk_emit(ctx->chunk, OP_STORE_CAPTURE);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)cap);
            } else {
                size_t gk = vm_chunk_add_str_const(ctx->chunk, gkey);
                vm_chunk_emit(ctx->chunk, OP_STORE_GLOBAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)gk);
            }
            return;
        }
    } else if (extract_cont_idx(t, &is_hash, &cont, &idx)) {
        int d = ctx->assign_depth++;
        char cbuf[32], kbuf[32], vbuf[32];
        snprintf(cbuf, sizeof(cbuf), "__cip_c%d", d);
        snprintf(kbuf, sizeof(kbuf), "__cip_k%d", d);
        snprintf(vbuf, sizeof(vbuf), "__cip_v%d", d);
        int c_slot = ctx_get_or_add_local(ctx, cbuf, '$');
        int k_slot = ctx_get_or_add_local(ctx, kbuf, '$');
        int v_slot = ctx_get_or_add_local(ctx, vbuf, '$');

        compile_expr(ctx, cont);
        emit_reset_store_local(ctx, c_slot);

        compile_expr(ctx, idx);
        emit_reset_store_local(ctx, k_slot);

        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)c_slot);
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)k_slot);
        vm_chunk_emit(ctx->chunk, is_hash ? OP_HASH_GET : OP_ARRAY_GET);

        emit_in_place_xform(ctx, is_chop);

        emit_reset_store_local(ctx, v_slot);

        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)c_slot);
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)k_slot);
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)v_slot);
        vm_chunk_emit(ctx->chunk, is_hash ? OP_HASH_SET : OP_ARRAY_SET);

        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)v_slot);
        ctx->assign_depth--;
        return;
    } else if (ttype == NI_DEREF_SCALAR) {
        StradaValue *ref_node = ast_get(t, "ref");
        if (!ref_node || STRADA_IS_TAGGED_INT(ref_node)) ref_node = ast_get(t, "target");
        int d = ctx->assign_depth++;
        char rbuf[32], vbuf[32];
        snprintf(rbuf, sizeof(rbuf), "__cip_r%d", d);
        snprintf(vbuf, sizeof(vbuf), "__cip_v%d", d);
        int r_slot = ctx_add_local(ctx, rbuf, '$');
        int v_slot = ctx_add_local(ctx, vbuf, '$');
        compile_expr(ctx, ref_node);
        emit_reset_store_local(ctx, r_slot);
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)r_slot);
        vm_chunk_emit(ctx->chunk, OP_DEREF_SCALAR);
        emit_in_place_xform(ctx, is_chop);
        emit_reset_store_local(ctx, v_slot);
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)r_slot);
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)v_slot);
        vm_chunk_emit(ctx->chunk, OP_STORE_SREF);
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)v_slot);
        ctx->assign_depth--;
        return;
    }

    compile_expr(ctx, arg0);
    emit_in_place_xform(ctx, is_chop);
}

/* In-place string transforms (compile_in_place_str_op): the current value
 * is on the stack; leave the new value. kind 0 = chomp, 1 = chop,
 * 2 = substr replacement using the operand slots in g_sr_slots
 * (offset, length, replacement, scratch for the [new, removed] pair). */
static int g_sr_slots[4];
static void emit_in_place_xform(CompCtx *ctx, int kind) {
    if (kind == 1) {
        vm_chunk_emit(ctx->chunk, OP_BUILTIN);
        vm_chunk_emit_u16(ctx->chunk, BUILTIN_STR_CHOP);
        vm_chunk_emit(ctx->chunk, 1);
    } else if (kind == 2) {
        for (int i = 0; i < 3; i++) {
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)g_sr_slots[i]);
        }
        vm_chunk_emit(ctx->chunk, OP_BUILTIN);
        vm_chunk_emit_u16(ctx->chunk, BUILTIN_SUBSTR_REPL);
        vm_chunk_emit(ctx->chunk, 4);
        emit_reset_store_local(ctx, g_sr_slots[3]);
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)g_sr_slots[3]);
        vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
        vm_chunk_emit_i64(ctx->chunk, 0);
        vm_chunk_emit(ctx->chunk, OP_ARRAY_GET);
    } else {
        vm_chunk_emit(ctx->chunk, OP_CHOMP);
    }
}

/* substr(TARGET, OFF[, LEN]) with a replacement: 4-arg substr (result = the
 * removed part, want_removed = 1) or lvalue `substr(...) = REPL` (result =
 * REPL). TARGET is rewritten in place. */
static void compile_substr_replace(CompCtx *ctx, StradaValue *sargs, int sargc, StradaValue *repl, int want_removed) {
    int d = ctx->assign_depth++;
    char nb[32];
    int slots[4];
    const char *nm[4] = { "__sr_o", "__sr_l", "__sr_r", "__sr_p" };
    for (int i = 0; i < 4; i++) {
        snprintf(nb, sizeof(nb), "%s%d", nm[i], d);
        slots[i] = ctx_get_or_add_local(ctx, nb, '$');
    }
    compile_expr(ctx, ast_arr_get(sargs, 1));
    emit_reset_store_local(ctx, slots[0]);
    if (sargc >= 3) compile_expr(ctx, ast_arr_get(sargs, 2));
    else { vm_chunk_emit(ctx->chunk, OP_PUSH_INT); vm_chunk_emit_i64(ctx->chunk, VM_INT_MAX); }
    emit_reset_store_local(ctx, slots[1]);
    compile_expr(ctx, repl);
    emit_reset_store_local(ctx, slots[2]);
    int saved[4];
    memcpy(saved, g_sr_slots, sizeof(saved));
    memcpy(g_sr_slots, slots, sizeof(slots));
    compile_in_place_str_op(ctx, ast_arr_get(sargs, 0), 2);
    memcpy(g_sr_slots, saved, sizeof(saved));
    vm_chunk_emit(ctx->chunk, OP_POP);
    if (want_removed) {
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)slots[3]);
        vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
        vm_chunk_emit_i64(ctx->chunk, 1);
        vm_chunk_emit(ctx->chunk, OP_ARRAY_GET);
    } else {
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)slots[2]);
    }
    ctx->assign_depth--;
}


typedef enum {
    LV_NONE = 0,
    LV_LOCAL,
    LV_CAPTURE,
    LV_GLOBAL,
    LV_INDEX,
    LV_SREF
} VMLValueKind;

typedef struct {
    VMLValueKind kind;
    int slot;
    int key_slot;
    int is_hash;
    int depth_pushed;
    const char *global_name;
} VMLValueTarget;

/* Prepare an lvalue target for read+writeback without re-evaluating container/index/ref side effects. */
static VMLValueTarget compile_lvalue_prepare(CompCtx *ctx, StradaValue *target) {
    VMLValueTarget lv = {0};
    target = ast_deref(target);
    if (!target || STRADA_IS_TAGGED_INT(target)) return lv;
    int nt = (int)ast_int(target, "type");
    if (nt == NI_VARIABLE) {
        const char *vname = ast_str(target, "name");
        const char *vsigil = ast_str(target, "sigil");
        char sc = (vsigil && vsigil[0]) ? vsigil[0] : '$';
        int slot = ctx_find_local_sigil(ctx, vname, sc);
        if (slot >= 0) { lv.kind = LV_LOCAL; lv.slot = slot; return lv; }
        int cap = ctx_resolve_capture(ctx, vname, sc);
        if (cap >= 0) { lv.kind = LV_CAPTURE; lv.slot = cap; return lv; }
        const char *gkey = resolve_global_var_key(ctx, vname);
        if (gkey) { lv.kind = LV_GLOBAL; lv.global_name = gkey; return lv; }
        return lv;
    }
    StradaValue *cont = NULL, *idx = NULL;
    int is_hash = 0;
    if (extract_cont_idx(target, &is_hash, &cont, &idx)) {
        int d = ctx->assign_depth++;
        char cbuf[32], kbuf[32];
        snprintf(cbuf, sizeof(cbuf), "__lv_c%d", d);
        snprintf(kbuf, sizeof(kbuf), "__lv_k%d", d);
        lv.slot = ctx_get_or_add_local(ctx, cbuf, '$');
        lv.key_slot = ctx_get_or_add_local(ctx, kbuf, '$');
        lv.is_hash = is_hash;
        lv.depth_pushed = 1;
        lv.kind = LV_INDEX;
        compile_expr(ctx, cont);
        emit_reset_store_local(ctx, lv.slot);
        compile_expr(ctx, idx);
        emit_reset_store_local(ctx, lv.key_slot);
        return lv;
    }
    if (nt == NI_DEREF_SCALAR) {
        StradaValue *ref_node = ast_get(target, "ref");
        if (!ref_node) ref_node = ast_get(target, "target");
        int d = ctx->assign_depth++;
        char rbuf[32];
        snprintf(rbuf, sizeof(rbuf), "__lv_r%d", d);
        lv.slot = ctx_get_or_add_local(ctx, rbuf, '$');
        lv.depth_pushed = 1;
        lv.kind = LV_SREF;
        compile_expr(ctx, ref_node);
        emit_reset_store_local(ctx, lv.slot);
        return lv;
    }
    return lv;
}

static void compile_lvalue_load(CompCtx *ctx, const VMLValueTarget *lv) {
    switch (lv->kind) {
    case LV_LOCAL:
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)lv->slot);
        break;
    case LV_CAPTURE:
        vm_chunk_emit(ctx->chunk, OP_LOAD_CAPTURE);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)lv->slot);
        break;
    case LV_GLOBAL: {
        size_t gk = vm_chunk_add_str_const(ctx->chunk, lv->global_name);
        vm_chunk_emit(ctx->chunk, OP_LOAD_GLOBAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)gk);
        break;
    }
    case LV_INDEX:
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)lv->slot);
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)lv->key_slot);
        vm_chunk_emit(ctx->chunk, lv->is_hash ? OP_HASH_GET : OP_ARRAY_GET);
        break;
    case LV_SREF:
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)lv->slot);
        vm_chunk_emit(ctx->chunk, OP_DEREF_SCALAR);
        break;
    default:
        vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
        break;
    }
}

static void compile_lvalue_store_slot(CompCtx *ctx, const VMLValueTarget *lv, int val_slot, int leave_on_stack) {
    switch (lv->kind) {
    case LV_LOCAL:
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)val_slot);
        vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)lv->slot);
        if (leave_on_stack) {
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)lv->slot);
        }
        break;
    case LV_CAPTURE:
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)val_slot);
        if (leave_on_stack) vm_chunk_emit(ctx->chunk, OP_DUP);
        vm_chunk_emit(ctx->chunk, OP_STORE_CAPTURE);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)lv->slot);
        break;
    case LV_GLOBAL: {
        size_t gk = vm_chunk_add_str_const(ctx->chunk, lv->global_name);
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)val_slot);
        if (leave_on_stack) vm_chunk_emit(ctx->chunk, OP_DUP);
        vm_chunk_emit(ctx->chunk, OP_STORE_GLOBAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)gk);
        break;
    }
    case LV_INDEX:
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)lv->slot);
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)lv->key_slot);
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)val_slot);
        vm_chunk_emit(ctx->chunk, lv->is_hash ? OP_HASH_SET : OP_ARRAY_SET);
        if (leave_on_stack) {
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)val_slot);
        }
        break;
    case LV_SREF:
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)lv->slot);
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)val_slot);
        vm_chunk_emit(ctx->chunk, OP_STORE_SREF);
        if (!leave_on_stack) vm_chunk_emit(ctx->chunk, OP_POP);
        break;
    default:
        if (leave_on_stack) vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
        break;
    }
}

static void compile_lvalue_finish(CompCtx *ctx, const VMLValueTarget *lv) {
    if (lv->depth_pushed) ctx->assign_depth--;
}

/* Compile tr/// — pushes the result value (count, or the modified copy when
 * the /r flag is used). The subject variable or lvalue is updated in place unless /r. */
static void compile_tr(CompCtx *ctx, StradaValue *node) {
    StradaValue *target = ast_get(node, "target");
    target = ast_deref(target);
    const char *from = ast_str(node, "search");
    const char *to = ast_str(node, "replace");
    const char *tr_flags = ast_str(node, "flags");
    if (!tr_flags) tr_flags = "";
    int has_r = (strchr(tr_flags, 'r') != NULL);

    size_t from_idx = vm_chunk_add_str_const(ctx->chunk, from);
    size_t to_idx = vm_chunk_add_str_const(ctx->chunk, to);
    size_t flags_idx = vm_chunk_add_str_const(ctx->chunk, tr_flags);

    if (has_r) {
        compile_expr(ctx, target);
        vm_chunk_emit(ctx->chunk, OP_TR);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)from_idx);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)to_idx);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)flags_idx);
        vm_chunk_emit(ctx->chunk, 0);
        vm_chunk_emit_u16(ctx->chunk, 0xFFFF);
        return;
    }

    VMLValueTarget lv = compile_lvalue_prepare(ctx, target);
    if (lv.kind == LV_LOCAL || lv.kind == LV_CAPTURE || lv.kind == LV_GLOBAL) {
        uint8_t store_kind = (lv.kind == LV_LOCAL) ? 1 : (lv.kind == LV_CAPTURE ? 2 : 3);
        uint16_t store_idx = (lv.kind == LV_GLOBAL)
            ? (uint16_t)vm_chunk_add_str_const(ctx->chunk, lv.global_name)
            : (uint16_t)lv.slot;
        compile_lvalue_load(ctx, &lv);
        vm_chunk_emit(ctx->chunk, OP_TR);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)from_idx);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)to_idx);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)flags_idx);
        vm_chunk_emit(ctx->chunk, store_kind);
        vm_chunk_emit_u16(ctx->chunk, store_idx);
        compile_lvalue_finish(ctx, &lv);
        return;
    }
    if (lv.kind == LV_INDEX || lv.kind == LV_SREF) {
        int str_slot = ctx_add_local(ctx, "__tr_s", '$');
        int cnt_slot = ctx_add_local(ctx, "__tr_cnt", '$');
        vm_chunk_emit(ctx->chunk, OP_RESET_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)str_slot);
        compile_lvalue_load(ctx, &lv);
        vm_chunk_emit(ctx->chunk, OP_TR);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)from_idx);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)to_idx);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)flags_idx);
        vm_chunk_emit(ctx->chunk, 1);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)str_slot);
        emit_reset_store_local(ctx, cnt_slot);
        compile_lvalue_store_slot(ctx, &lv, str_slot, 0);
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)cnt_slot);
        compile_lvalue_finish(ctx, &lv);
        return;
    }

    compile_expr(ctx, target);
    vm_chunk_emit(ctx->chunk, OP_TR);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)from_idx);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)to_idx);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)flags_idx);
    vm_chunk_emit(ctx->chunk, 0);
    vm_chunk_emit_u16(ctx->chunk, 0xFFFF);
}


/* Compile s/PAT/EXPR/e[g]: an inline find/eval/append loop. The parser has
 * already parsed the replacement source into node->{eval_expr}; each match
 * sets the VM capture state, then the expression is evaluated inline and
 * appended. Pushes the rebuilt string AND stores it back into the target lvalue. */
/* ---- regex pattern interpolation ($name, ${name}, $h{K}, $a[I]) ----
 * Mirrors the compiled backend (regex_pattern_has_interp /
 * analyze_regex_interp_n): the variable's value is inserted raw, so its
 * metacharacters are live. Escaped "\$" and a "$" not followed by a name
 * stay literal. */
static int rx_ident_start(char c) { return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z'); }
static int rx_ident_char(char c) { return rx_ident_start(c) || (c >= '0' && c <= '9'); }

static StradaValue *rx_mk_node(int type) {
    StradaValue *n = strada_new_hash();
    strada_hash_set_take(n->value.hv, "type", strada_new_int(type));
    return n;
}
static StradaValue *rx_mk_var(const char *name, size_t nlen, const char *sigil) {
    StradaValue *n = rx_mk_node(NI_VARIABLE);
    strada_hash_set_take(n->value.hv, "name", strada_new_str_len(name, nlen));
    strada_hash_set_take(n->value.hv, "sigil", strada_new_str(sigil));
    return n;
}
static int rx_var_declared(CompCtx *ctx, const char *name, size_t nlen, char sigil) {
    char nb[256];
    if (nlen >= sizeof(nb)) return 0;
    memcpy(nb, name, nlen); nb[nlen] = 0;
    if (ctx_find_local_sigil(ctx, nb, sigil) >= 0) return 1;
    if (ctx_resolve_capture(ctx, nb, sigil) >= 0) return 1;
    return 0;
}
/* Element form at pattern[i] ('{' or '['); returns its length (0 = not one)
 * and sets *out to the element-access node. */
static size_t rx_elem(CompCtx *ctx, const char *p, size_t len, size_t i, const char *vn, size_t vnl, StradaValue **out) {
    *out = NULL;
    if (i >= len) return 0;
    char open = p[i], close;
    if (open == '{') close = '}'; else if (open == '[') close = ']'; else return 0;
    if (!rx_var_declared(ctx, vn, vnl, open == '{' ? '%' : '@')) return 0;
    int depth = 0; size_t j = i, end = (size_t)-1;
    while (j < len && end == (size_t)-1) {
        if (p[j] == '\\') j++;
        else if (p[j] == open) depth++;
        else if (p[j] == close) { if (--depth == 0) end = j; }
        j++;
    }
    if (end == (size_t)-1 || end == i + 1) return 0;
    const char *in = p + i + 1; size_t il = end - i - 1;
    StradaValue *key = NULL;
    if (in[0] == '$') {
        if (il < 2 || !rx_ident_start(in[1])) return 0;
        for (size_t k = 2; k < il; k++) if (!rx_ident_char(in[k])) return 0;
        key = rx_mk_var(in + 1, il - 1, "$");
    } else if (open == '[') {
        size_t k = (in[0] == '-') ? 1 : 0;
        if (k >= il) return 0;
        for (size_t q = k; q < il; q++) if (in[q] < '0' || in[q] > '9') return 0;
        key = rx_mk_node(NI_INT_LITERAL);
        strada_hash_set_take(key->value.hv, "value", strada_new_int(strtoll(in, NULL, 10)));
    } else if ((in[0] == '"' || in[0] == '\'') && il >= 2 && in[il - 1] == in[0]) {
        key = rx_mk_node(NI_STR_LITERAL);
        strada_hash_set_take(key->value.hv, "value", strada_new_str_len(in + 1, il - 2));
    } else {
        for (size_t q = 0; q < il; q++) if (!rx_ident_char(in[q]) && in[q] != '-') return 0;
        key = rx_mk_node(NI_STR_LITERAL);
        strada_hash_set_take(key->value.hv, "value", strada_new_str_len(in, il));
    }
    StradaValue *n;
    if (open == '{') {
        n = rx_mk_node(NI_HASH_ACCESS);
        strada_hash_set_take(n->value.hv, "hash", rx_mk_var(vn, vnl, "%"));
        strada_hash_set_take(n->value.hv, "key", key);
    } else {
        n = rx_mk_node(NI_SUBSCRIPT);
        strada_hash_set_take(n->value.hv, "array", rx_mk_var(vn, vnl, "@"));
        strada_hash_set_take(n->value.hv, "index", key);
    }
    *out = n;
    return end - i + 1;
}

/* If the pattern interpolates, emit code that pushes the built pattern
 * string (with the (?imsx) prefix) and return 1; else emit nothing, return 0. */
static int compile_rx_dynamic_pattern(CompCtx *ctx, StradaValue *owner, const char *p, size_t len, const char *flags) {
    if (!p) return 0;
    int any = 0;
    for (size_t i = 0; i + 1 < len; i++) {
        if (p[i] == '\\') { i++; continue; }
        if (p[i] == '$' && (rx_ident_start(p[i + 1]) || p[i + 1] == '{')) { any = 1; break; }
    }
    if (!any) return 0;
    char mods[8]; int mi = 0;
    if (flags) {
        if (strchr(flags, 'i')) mods[mi++] = 'i';
        if (strchr(flags, 's')) mods[mi++] = 's';
        if (strchr(flags, 'm')) mods[mi++] = 'm';
        if (strchr(flags, 'x')) mods[mi++] = 'x';
    }
    mods[mi] = 0;
    char pre[16];
    snprintf(pre, sizeof(pre), mi ? "(?%s)" : "", mods);
    size_t cidx = vm_chunk_add_str_const(ctx->chunk, pre);
    vm_chunk_emit(ctx->chunk, OP_PUSH_STR);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)cidx);
    size_t lit_start = 0, i = 0;
    int nparts = 0;
#define RX_FLUSH(upto) do { if ((upto) > lit_start) { \
        size_t li = vm_chunk_add_str_const_len(ctx->chunk, p + lit_start, (upto) - lit_start); \
        vm_chunk_emit(ctx->chunk, OP_PUSH_STR); vm_chunk_emit_u16(ctx->chunk, (uint16_t)li); \
        vm_chunk_emit(ctx->chunk, OP_CONCAT); } } while (0)
    while (i < len) {
        if (p[i] == '\\' && i + 1 < len) { i += 2; continue; }
        if (p[i] != '$' || i + 1 >= len) { i++; continue; }
        size_t st = i, j = i + 1;
        const char *vn = NULL; size_t vnl = 0; int braced = 0;
        if (p[j] == '{') {
            size_t b = j + 1;
            while (b < len && p[b] != '}') b++;
            if (b < len && b > j + 1 && rx_ident_start(p[j + 1])) {
                int ok = 1;
                for (size_t q = j + 2; q < b; q++) if (!rx_ident_char(p[q])) ok = 0;
                if (ok) { vn = p + j + 1; vnl = b - j - 1; j = b + 1; braced = 1; }
            }
        } else if (rx_ident_start(p[j])) {
            vn = p + j;
            while (j < len && rx_ident_char(p[j])) j++;
            vnl = (size_t)(p + j - vn);
        }
        if (!vn) { i++; continue; }
        StradaValue *expr = NULL;
        size_t el = braced ? 0 : rx_elem(ctx, p, len, j, vn, vnl, &expr);
        if (!expr) expr = rx_mk_var(vn, vnl, "$");
        RX_FLUSH(st);
        compile_expr(ctx, expr);
        vm_chunk_emit(ctx->chunk, OP_CONCAT);
        /* the regex node owns the synthesized node (it lives as long as
         * the AST, in case compiled code refers back to it) */
        {
            StradaValue *own = ast_deref(owner);
            StradaValue *keep = (own && !STRADA_IS_TAGGED_INT(own) && own->type == STRADA_HASH) ? ast_get(own, "__rx_nodes") : NULL;
            if (own && !STRADA_IS_TAGGED_INT(own) && own->type == STRADA_HASH) {
                if (!keep) {
                    StradaValue *arr = strada_new_array();
                    strada_hash_set_take(own->value.hv, "__rx_nodes", strada_ref_create_take(arr));
                    keep = ast_get(own, "__rx_nodes");
                }
                StradaValue *ka = ast_deref(keep);
                if (ka && !STRADA_IS_TAGGED_INT(ka) && ka->type == STRADA_ARRAY) strada_array_push_take(ka->value.av, expr);
                else strada_decref(expr);
            } else {
                strada_decref(expr);
            }
        }
        nparts++;
        i = j + el;
        lit_start = i;
    }
    RX_FLUSH(len);
#undef RX_FLUSH
    return 1;
}

static int compile_regex_subst_e(CompCtx *ctx, StradaValue *node) {
    StradaValue *target = ast_get(node, "target");
    target = ast_deref(target);
    StradaValue *eval_expr = ast_get(node, "eval_expr");
    if (!eval_expr || STRADA_IS_TAGGED_INT(eval_expr)) return 0;
    size_t pat_len = 0;
    const char *pattern = ast_str_len(node, "pattern", &pat_len);
    const char *flags = ast_str(node, "flags");
    int is_global = flags && strchr(flags, 'g') != NULL;
    int is_nondestruct = flags && strchr(flags, 'r') != NULL;

    VMLValueTarget lv = {0};
    if (!is_nondestruct) lv = compile_lvalue_prepare(ctx, target);
    if (lv.kind == LV_NONE && !is_nondestruct) return 0;

    size_t pat_idx = add_pattern_const(ctx, pattern, pat_len, flags);
    int w = ctx_add_local(ctx, "__se_w", '$');   /* subject */
    int o = ctx_add_local(ctx, "__se_o", '$');   /* output */
    int pp = ctx_add_local(ctx, "__se_p", '$');  /* position */
    int ms = ctx_add_local(ctx, "__se_s", '$');  /* match start */
    int me = ctx_add_local(ctx, "__se_e", '$');  /* match end */
    int wl = ctx_add_local(ctx, "__se_l", '$');  /* byte length of subject */
    int cnt = ctx_add_local(ctx, "__se_cnt", '$'); /* substitution count */
    int is_u8 = ctx_add_local(ctx, "__se_u8", '$'); /* subject UTF-8 flag */
    int stp = ctx_add_local(ctx, "__se_stp", '$');  /* zero-width advance bytes */
    int ub = ctx_add_local(ctx, "__se_ub", '$');    /* lead byte for UTF-8 step */
    int se_dyn = -1;                                /* interpolated pattern */
    if (compile_rx_dynamic_pattern(ctx, node, pattern, pat_len, flags)) {
        se_dyn = ctx_add_local(ctx, "__se_pat", '$');
        emit_reset_store_local(ctx, se_dyn);
    }

#define E8(op)   vm_chunk_emit(ctx->chunk, (op))
#define E16(v)   vm_chunk_emit_u16(ctx->chunk, (uint16_t)(v))
#define LD(sl)   do { E8(OP_LOAD_LOCAL); E16(sl); } while (0)
#define ST(sl)   do { E8(OP_RESET_LOCAL); E16(sl); E8(OP_STORE_LOCAL); E16(sl); } while (0)

    /* W = target; O = ""; P = 0; WL = bytes(W); CNT = 0; IS_U8 = utf8::is_utf8(W) */
    if (lv.kind != LV_NONE) {
        compile_lvalue_load(ctx, &lv);
    } else {
        compile_expr(ctx, target);
    }
    ST(w);
    LD(w); E8(OP_BYTES); ST(wl);
    LD(w); E8(OP_BUILTIN); E16(BUILTIN_UTF8_IS_UTF8); E8(1); ST(is_u8);
    size_t empty_idx = vm_chunk_add_str_const(ctx->chunk, "");
    E8(OP_PUSH_STR); E16(empty_idx); ST(o);
    E8(OP_PUSH_INT); vm_chunk_emit_i64(ctx->chunk, 0); ST(pp);
    E8(OP_PUSH_INT); vm_chunk_emit_i64(ctx->chunk, 0); ST(cnt);

    size_t loop_top = ctx->chunk->code_len;
    /* (S, E) = find(W, P) */
    LD(w); LD(pp);
    if (se_dyn >= 0) { LD(se_dyn); E8(OP_REGEX_FIND); E16(0xFFFF); }
    else { E8(OP_REGEX_FIND); E16(pat_idx); }
    ST(me); ST(ms);
    /* if S < 0 goto done */
    LD(ms); E8(OP_PUSH_INT); vm_chunk_emit_i64(ctx->chunk, 0); E8(OP_LT);
    E8(OP_JMP_IF_TRUE); size_t done_patch = emit_patch_u16(ctx);
    /* O = O . byte_slice(W, P, S-P) */
    LD(o); LD(w); LD(pp); LD(ms); LD(pp); E8(OP_SUB); E8(OP_BUILTIN); E16(BUILTIN_REGEX_BYTE_SLICE); E8(3); E8(OP_CONCAT); ST(o);

    size_t after_tail_patches[4];
    int atp_count = 0;

    /* O = O . <eval_expr> (captures are live); CNT++ */
    LD(o); compile_expr(ctx, eval_expr); E8(OP_CONCAT); ST(o);
    E8(OP_INCR); E16(cnt);

    if (is_global) {
        /* If zero-width match (E == S): if E >= wl finish, else advance by 1 UTF-8 char/byte */
        LD(me); LD(ms); E8(OP_EQ);
        E8(OP_JMP_IF_FALSE); size_t non_empty_patch = emit_patch_u16(ctx);
        LD(me); LD(wl); E8(OP_GE);
        E8(OP_JMP_IF_TRUE); after_tail_patches[atp_count++] = emit_patch_u16(ctx);
        E8(OP_PUSH_INT); vm_chunk_emit_i64(ctx->chunk, 1); ST(stp);
        LD(is_u8); E8(OP_JMP_IF_FALSE); size_t not_u8_patch = emit_patch_u16(ctx);
        LD(w); LD(me); E8(OP_BUILTIN); E16(BUILTIN_BYTE_AT); E8(2); ST(ub);
        LD(ub); E8(OP_PUSH_INT); vm_chunk_emit_i64(ctx->chunk, 240); E8(OP_GE);
        E8(OP_JMP_IF_FALSE); size_t lt4_patch = emit_patch_u16(ctx);
        E8(OP_PUSH_INT); vm_chunk_emit_i64(ctx->chunk, 4); ST(stp);
        E8(OP_JMP); size_t done_u8_1 = emit_patch_u16(ctx);
        patch_u16(ctx, lt4_patch, (uint16_t)ctx->chunk->code_len);
        LD(ub); E8(OP_PUSH_INT); vm_chunk_emit_i64(ctx->chunk, 224); E8(OP_GE);
        E8(OP_JMP_IF_FALSE); size_t lt3_patch = emit_patch_u16(ctx);
        E8(OP_PUSH_INT); vm_chunk_emit_i64(ctx->chunk, 3); ST(stp);
        E8(OP_JMP); size_t done_u8_2 = emit_patch_u16(ctx);
        patch_u16(ctx, lt3_patch, (uint16_t)ctx->chunk->code_len);
        LD(ub); E8(OP_PUSH_INT); vm_chunk_emit_i64(ctx->chunk, 192); E8(OP_GE);
        E8(OP_JMP_IF_FALSE); size_t done_u8_3 = emit_patch_u16(ctx);
        E8(OP_PUSH_INT); vm_chunk_emit_i64(ctx->chunk, 2); ST(stp);
        patch_u16(ctx, done_u8_1, (uint16_t)ctx->chunk->code_len);
        patch_u16(ctx, done_u8_2, (uint16_t)ctx->chunk->code_len);
        patch_u16(ctx, done_u8_3, (uint16_t)ctx->chunk->code_len);
        LD(me); LD(stp); E8(OP_ADD); LD(wl); E8(OP_GT);
        E8(OP_JMP_IF_FALSE); size_t clamp_ok = emit_patch_u16(ctx);
        LD(wl); LD(me); E8(OP_SUB); ST(stp);
        patch_u16(ctx, clamp_ok, (uint16_t)ctx->chunk->code_len);
        patch_u16(ctx, not_u8_patch, (uint16_t)ctx->chunk->code_len);
        LD(o); LD(w); LD(me); LD(stp); E8(OP_BUILTIN); E16(BUILTIN_REGEX_BYTE_SLICE); E8(3); E8(OP_CONCAT); ST(o);
        LD(me); LD(stp); E8(OP_ADD); ST(pp);
        E8(OP_JMP); size_t back1 = emit_patch_u16(ctx);
        patch_u16(ctx, back1, (uint16_t)loop_top);
        patch_u16(ctx, non_empty_patch, (uint16_t)ctx->chunk->code_len);
        LD(me); ST(pp);
        E8(OP_JMP); size_t back2 = emit_patch_u16(ctx);
        patch_u16(ctx, back2, (uint16_t)loop_top);
    } else {
        LD(me); ST(pp);
    }

    patch_u16(ctx, done_patch, (uint16_t)ctx->chunk->code_len);
    /* O = O . byte_slice(W, P, wl-P) */
    LD(o); LD(w); LD(pp); LD(wl); LD(pp); E8(OP_SUB); E8(OP_BUILTIN); E16(BUILTIN_REGEX_BYTE_SLICE); E8(3); E8(OP_CONCAT); ST(o);
    for (int pi = 0; pi < atp_count; pi++) {
        patch_u16(ctx, after_tail_patches[pi], (uint16_t)ctx->chunk->code_len);
    }
    if (is_nondestruct) {
        if (lv.kind != LV_NONE) compile_lvalue_finish(ctx, &lv);
        LD(o);
    } else {
        compile_lvalue_store_slot(ctx, &lv, o, 0);
        compile_lvalue_finish(ctx, &lv);
        LD(cnt);
    }
    return 1;

#undef E8
#undef E16
#undef LD
#undef ST
}


/* Add a regex pattern string constant, folding Perl match flags (i/s/m/x)
 * into an inline (?...) prefix so the runtime needs no per-match options. */
static size_t add_pattern_const(CompCtx *ctx, const char *pattern, size_t pat_len, const char *flags) {
    char mods[8];
    int mi = 0;
    if (flags) {
        if (strchr(flags, 'i')) mods[mi++] = 'i';
        if (strchr(flags, 's')) mods[mi++] = 's';
        if (strchr(flags, 'm')) mods[mi++] = 'm';
        if (strchr(flags, 'x')) mods[mi++] = 'x';
    }
    if (mi == 0) return vm_chunk_add_str_const_len(ctx->chunk, pattern, pat_len);
    mods[mi] = 0;
    size_t plen = pat_len + (size_t)mi + 8;
    char *buf = malloc(plen);
    int pre = snprintf(buf, plen, "(?%s)", mods);
    if (pat_len > 0 && pattern) memcpy(buf + pre, pattern, pat_len);
    buf[pre + pat_len] = '\0';
    size_t idx = vm_chunk_add_str_const_len(ctx->chunk, buf, (size_t)pre + pat_len);
    free(buf);
    return idx;
}

#define AST_TYPE_ARRAY 4
#define AST_TYPE_HASH_RET 5

static int resolve_vm_func_idx(CompCtx *ctx, const char *name) {
    return resolve_func_idx_for_ctx(ctx, name, NULL, 0);
}

/* Compile a list-context element. %h / %$r become a flat key/value array
 * (Perl); everything else compiles as usual. */
/* The array operand of push/unshift/splice. @{EXPR} / @$x on an lvalue
 * autovivifies (undef -> a new array stored back) and a defined non-array
 * throws "Not an ARRAY reference" (compiled: strada_autoviv_slot_array). */
static VMLValueTarget compile_lvalue_prepare(CompCtx *ctx, StradaValue *target);
static void compile_lvalue_load(CompCtx *ctx, const VMLValueTarget *lv);
static void compile_lvalue_store_slot(CompCtx *ctx, const VMLValueTarget *lv, int val_slot, int leave_on_stack);
static void compile_lvalue_finish(CompCtx *ctx, const VMLValueTarget *lv);
static void compile_mutable_array_target(CompCtx *ctx, StradaValue *a) {
    StradaValue *d = ast_deref(a);
    if (d && !STRADA_IS_TAGGED_INT(d) && (int)ast_int(d, "type") == NI_DEREF_SCALAR) {
        const char *sg = ast_str(d, "sigil");
        StradaValue *inner = ast_get(d, "ref");
        if (!inner) inner = ast_get(d, "target");
        StradaValue *di = ast_deref(inner);
        int it = (di && !STRADA_IS_TAGGED_INT(di)) ? (int)ast_int(di, "type") : 0;
        int ok_inner = (it == NI_HASH_ACCESS || it == NI_SUBSCRIPT || it == NI_DEREF_ARRAY ||
                        (it == NI_VARIABLE && ast_str(di, "sigil")[0] == '$'));
        if (it == NI_DEREF_HASH) ok_inner = 1;
        if (sg && sg[0] == '@' && ok_inner) {
            VMLValueTarget lv = compile_lvalue_prepare(ctx, inner);
            if (lv.kind != LV_NONE) {
                compile_lvalue_load(ctx, &lv);
                vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                vm_chunk_emit_u16(ctx->chunk, BUILTIN_AUTOVIV_ARRAY);
                vm_chunk_emit(ctx->chunk, 1);
                int tslot = ctx_add_local(ctx, "__avv_t", '$');
                emit_reset_store_local(ctx, tslot);
                compile_lvalue_store_slot(ctx, &lv, tslot, 0);
                compile_lvalue_finish(ctx, &lv);
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)tslot);
                return;
            }
        }
    }
    compile_expr(ctx, a);
}

static int vm_args_have_flatten(CompCtx *ctx, StradaValue *args, int argc, int from) {
    for (int i = from; i < argc; i++) {
        StradaValue *a = ast_deref(ast_arr_get(args, i));
        if (!a || STRADA_IS_TAGGED_INT(a)) continue;
        if ((int)ast_int(a, "type") == NI_SPREAD || vm_array_flatten_kind(ctx, a) != 0) return 1;
    }
    return 0;
}

/* Push one array holding every argument, list-valued ones flattened. */
static void compile_flat_arg_list(CompCtx *ctx, StradaValue *args, int argc) {
    vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
    vm_chunk_emit_u32(ctx->chunk, (uint32_t)(argc > 0 ? argc * 2 : 4));
    for (int i = 0; i < argc; i++) {
        StradaValue *a = ast_arr_get(args, i);
        StradaValue *d = ast_deref(a);
        vm_chunk_emit(ctx->chunk, OP_DUP);
        if (d && !STRADA_IS_TAGGED_INT(d) && (int)ast_int(d, "type") == NI_SPREAD) {
            compile_list_src(ctx, ast_get(d, "target"));
            vm_chunk_emit(ctx->chunk, OP_ARRAY_EXTEND);
        } else {
            compile_list_src(ctx, a);
            vm_chunk_emit(ctx->chunk, vm_array_flatten_kind(ctx, a) != 0 ? OP_ARRAY_EXTEND : OP_ARRAY_PUSH);
        }
    }
}

static int vm_is_hash_sigil_node(StradaValue *e) {
    StradaValue *d = ast_deref(e);
    if (!d || STRADA_IS_TAGGED_INT(d)) return 0;
    int t = (int)ast_int(d, "type");
    if (t != NI_VARIABLE && t != NI_DEREF_SCALAR) return 0;
    const char *sig = ast_str(d, "sigil");
    return sig && sig[0] == '%';
}

static void compile_list_src(CompCtx *ctx, StradaValue *e) {
    StradaValue *d = ast_deref(e);
    if (d && !STRADA_IS_TAGGED_INT(d)) {
        int t = (int)ast_int(d, "type");
        if (t == NI_VARIABLE || t == NI_DEREF_SCALAR) {
            const char *sig = ast_str(d, "sigil");
            if (sig && sig[0] == '%') {
                compile_expr(ctx, e);
                vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                vm_chunk_emit_u16(ctx->chunk, BUILTIN_HASH_FLAT);
                vm_chunk_emit(ctx->chunk, 1);
                return;
            }
        }
    }
    compile_expr(ctx, e);
}

static int vm_array_flatten_kind(CompCtx *ctx, StradaValue *elem) {
    elem = ast_deref(elem);
    if (!elem || STRADA_IS_TAGGED_INT(elem)) return 0;
    int t = (int)ast_int(elem, "type");
    /* %h / %$r in list context: its key/value pairs (OP_ARRAY_EXTEND
     * flattens a hash source into pairs) */
    if (t == NI_VARIABLE) {
        const char *sig = ast_str(elem, "sigil");
        if (sig[0] == '@' || sig[0] == '%') return 1;
        return 0;
    }
    if (t == NI_DEREF_SCALAR) {
        const char *sig = ast_str(elem, "sigil");
        if (sig && (sig[0] == '@' || sig[0] == '%')) return 1;
        return 0;
    }
    if (t == NI_ANON_ARRAY && (int)ast_int(elem, "is_list") == 1) return 2;
    if (t == NI_MAP || t == NI_GREP || t == NI_SORT) return 2;
    /* as compiled array_flatten_kind: ranges, slices and @{...} derefs */
    if (t == NI_RANGE || t == NI_ARRAY_SLICE || t == NI_HASH_SLICE) return 2;
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
        /* runtime builtins returning a list (core::unpack, re::captures, ...) */
        if (fidx < 0) {
            const char *dc = strstr(check_nm, "::");
            if (dc) {
                size_t nl = (size_t)(dc - check_nm);
                if ((nl == 3 && (strncmp(check_nm, "sys", 3) == 0 || strncmp(check_nm, "str", 3) == 0)) ||
                    (nl == 4 && (strncmp(check_nm, "math", 4) == 0 || strncmp(check_nm, "utf8", 4) == 0)) ||
                    (nl == 2 && strncmp(check_nm, "re", 2) == 0))
                    return 2;
            }
            if (strcmp(check_nm, "captures") == 0 || strcmp(check_nm, "sort") == 0 || strcmp(check_nm, "glob") == 0)
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

    int expr_call_ctx = ctx->call_context;
    ctx->call_context = 0;

    int type = (int)ast_int(node, "type");

    switch (type) {
    case NI_INT_LITERAL: {
        /* a literal above INT64_MAX is a uint64 (or a double past
         * UINT64_MAX), as in the compiled backend (int_lit_kind) */
        StradaValue *lv = ast_get(node, "value");
        if (lv && !STRADA_IS_TAGGED_INT(lv) && lv->type == STRADA_STR && lv->value.pv) {
            const char *lt = lv->value.pv;
            size_t ll = strlen(lt);
            int digits = (ll > 0);
            for (size_t q = 0; q < ll; q++) if (lt[q] < '0' || lt[q] > '9') digits = 0;
            if (digits && (ll > 19 || (ll == 19 && strcmp(lt, "9223372036854775807") > 0))) {
                size_t li = vm_chunk_add_str_const(ctx->chunk, lt);
                vm_chunk_emit(ctx->chunk, OP_PUSH_STR);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)li);
                vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                vm_chunk_emit_u16(ctx->chunk, BUILTIN_NUMIFY_EXACT);
                vm_chunk_emit(ctx->chunk, 1);
                break;
            }
        }
        int64_t val = ast_int(node, "value");
        emit_push_int(ctx->chunk, val);
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
        size_t vlen = 0;
        const char *val = ast_str_len(node, "value", &vlen);
        size_t idx = vm_chunk_add_str_const_len(ctx->chunk, val, vlen);
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
            } else {
                const char *gkey = resolve_global_var_key(ctx, name);
                if (gkey) {
                    /* our variable or const inside closure / function */
                    size_t key_idx = vm_chunk_add_str_const(ctx->chunk, gkey);
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
                    int64_t eval = 0;
                    if (lookup_enum_for_ctx(ctx, name, &eval)) {
                        emit_push_int(ctx->chunk, eval);
                    } else {
                        vm_report_undeclared_var(ctx, node, sigil, name);
                        vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
                    }
                }
            }
        }
        break;
    }

    case NI_BINARY_OP: {
        VMConstFold folded;
        if (try_eval_const_ast(node, &folded, 0)) {
            emit_folded_const(ctx, &folded);
            break;
        }
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

        StradaValue *lnode = ast_get(node, "left");
        StradaValue *rnode = ast_get(node, "right");
        int is_num_cmp = (op == OP_AST_NUM_EQ || op == OP_AST_NUM_NE ||
                          op == OP_AST_NUM_LT || op == OP_AST_NUM_GT ||
                          op == OP_AST_NUM_LE || op == OP_AST_NUM_GE ||
                          op == OP_AST_SPACESHIP);
        StradaValue *l_sq = is_num_cmp ? ast_get(lnode, "from_sq_str") : NULL;
        StradaValue *r_sq = is_num_cmp ? ast_get(rnode, "from_sq_str") : NULL;
        if (l_sq && (STRADA_IS_TAGGED_INT(l_sq) || l_sq->type != STRADA_STR)) l_sq = NULL;
        if (r_sq && (STRADA_IS_TAGGED_INT(r_sq) || r_sq->type != STRADA_STR)) r_sq = NULL;

        if (!l_sq && !r_sq) {
            if (op == OP_AST_ADD) {
                StradaValue *ban = NULL;
                int32_t add_imm = 0;
                if (try_const_int32(rnode, &add_imm)) ban = ast_deref(lnode);
                else if (try_const_int32(lnode, &add_imm)) ban = ast_deref(rnode);
                if (ban && !STRADA_IS_TAGGED_INT(ban) && (int)ast_int(ban, "type") == NI_BINARY_OP &&
                    ast_op_code(ban) == OP_AST_BIT_AND) {
                    StradaValue *bl = ast_get(ban, "left");
                    StradaValue *br = ast_get(ban, "right");
                    int32_t mask_imm = 0;
                    int bslot = find_local_var_slot(ctx, bl);
                    if (bslot >= 0 && try_const_int32(br, &mask_imm)) {
                        vm_chunk_emit(ctx->chunk, OP_BIT_AND_ADD_I32);
                        vm_chunk_emit_u16(ctx->chunk, (uint16_t)bslot);
                        vm_chunk_emit_u32(ctx->chunk, (uint32_t)mask_imm);
                        vm_chunk_emit_u32(ctx->chunk, (uint32_t)add_imm);
                        break;
                    }
                    bslot = find_local_var_slot(ctx, br);
                    if (bslot >= 0 && try_const_int32(bl, &mask_imm)) {
                        vm_chunk_emit(ctx->chunk, OP_BIT_AND_ADD_I32);
                        vm_chunk_emit_u16(ctx->chunk, (uint16_t)bslot);
                        vm_chunk_emit_u32(ctx->chunk, (uint32_t)mask_imm);
                        vm_chunk_emit_u32(ctx->chunk, (uint32_t)add_imm);
                        break;
                    }
                }
            }
            if (op == OP_AST_SUB || op == OP_AST_ADD || op == OP_AST_MUL ||
                op == OP_AST_BIT_AND || op == OP_AST_MOD) {
                int sa = find_local_var_slot(ctx, lnode);
                if (sa >= 0) {
                    int32_t imm = 0;
                    if (op != OP_AST_MUL && try_const_int32(rnode, &imm)) {
                        if (op == OP_AST_SUB || op == OP_AST_ADD) {
                            vm_chunk_emit(ctx->chunk, (op == OP_AST_SUB) ? OP_SUB_LOCAL_I32 : OP_ADD_LOCAL_I32);
                            vm_chunk_emit_u16(ctx->chunk, (uint16_t)sa);
                            vm_chunk_emit_u32(ctx->chunk, (uint32_t)imm);
                            break;
                        } else if (op == OP_AST_BIT_AND) {
                            vm_chunk_emit(ctx->chunk, OP_BIT_AND_LOCAL_I32);
                            vm_chunk_emit_u16(ctx->chunk, (uint16_t)sa);
                            vm_chunk_emit_u32(ctx->chunk, (uint32_t)imm);
                            break;
                        } else if (op == OP_AST_MOD && imm != 0) {
                            vm_chunk_emit(ctx->chunk, OP_MOD_LOCAL_I32);
                            vm_chunk_emit_u16(ctx->chunk, (uint16_t)sa);
                            vm_chunk_emit_u32(ctx->chunk, (uint32_t)imm);
                            break;
                        }
                    }
                    if (op == OP_AST_ADD || op == OP_AST_SUB || op == OP_AST_MUL) {
                        int sb = find_local_var_slot(ctx, rnode);
                        if (sb >= 0) {
                            uint8_t opc = (op == OP_AST_ADD) ? OP_ADD_LOCALS :
                                          (op == OP_AST_SUB) ? OP_SUB_LOCALS : OP_MUL_LOCALS;
                            vm_chunk_emit(ctx->chunk, opc);
                            vm_chunk_emit_u16(ctx->chunk, (uint16_t)sa);
                            vm_chunk_emit_u16(ctx->chunk, (uint16_t)sb);
                            break;
                        }
                    }
                } else if (op == OP_AST_BIT_AND || op == OP_AST_ADD) {
                    int sb = find_local_var_slot(ctx, rnode);
                    int32_t imm = 0;
                    if (sb >= 0 && try_const_int32(lnode, &imm)) {
                        vm_chunk_emit(ctx->chunk, (op == OP_AST_BIT_AND) ? OP_BIT_AND_LOCAL_I32 : OP_ADD_LOCAL_I32);
                        vm_chunk_emit_u16(ctx->chunk, (uint16_t)sb);
                        vm_chunk_emit_u32(ctx->chunk, (uint32_t)imm);
                        break;
                    }
                }
            }
        }

        compile_expr(ctx, lnode);
        compile_expr(ctx, rnode);

        if (l_sq && !r_sq) {
            const char *lstr = l_sq->value.pv ? l_sq->value.pv : "";
            size_t llen = STRADA_STR_BYTELEN(l_sq);
            if (llen == 0 && lstr[0] != '\0') llen = strlen(lstr);
            size_t sidx = vm_chunk_add_str_const_len(ctx->chunk, lstr, llen);
            vm_chunk_emit(ctx->chunk, OP_SQ_COERCE);
            vm_chunk_emit(ctx->chunk, 0);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)sidx);
        } else if (r_sq && !l_sq) {
            const char *rstr = r_sq->value.pv ? r_sq->value.pv : "";
            size_t rlen = STRADA_STR_BYTELEN(r_sq);
            if (rlen == 0 && rstr[0] != '\0') rlen = strlen(rstr);
            size_t sidx = vm_chunk_add_str_const_len(ctx->chunk, rstr, rlen);
            vm_chunk_emit(ctx->chunk, OP_SQ_COERCE);
            vm_chunk_emit(ctx->chunk, 1);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)sidx);
        }

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
        VMConstFold folded;
        if (try_eval_const_ast(node, &folded, 0)) {
            emit_folded_const(ctx, &folded);
            break;
        }
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
        compile_expr_in_ctx(ctx, ast_get(node, "true_expr"), expr_call_ctx);
        vm_chunk_emit(ctx->chunk, OP_JMP);
        size_t end_patch = emit_patch_u16(ctx);
        patch_u16(ctx, else_patch, (uint16_t)ctx->chunk->code_len);
        compile_expr_in_ctx(ctx, ast_get(node, "false_expr"), expr_call_ctx);
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
        size_t kidx = 0;
        if (is_concat_key(ast_get(node, "key"), &prefix, &suffix)) {
            compile_expr(ctx, ast_get(node, "hash"));
            compile_expr(ctx, suffix);
            size_t pidx = vm_chunk_add_str_const(ctx->chunk, prefix);
            vm_chunk_emit(ctx->chunk, OP_HASH_GET_CONCAT);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)pidx);
        } else if (try_const_str_key(ctx, ast_get(node, "key"), &kidx)) {
            compile_expr(ctx, ast_get(node, "hash"));
            vm_chunk_emit(ctx->chunk, OP_HASH_GET_CONST);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)kidx);
        } else {
            compile_expr(ctx, ast_get(node, "hash"));
            compile_expr(ctx, ast_get(node, "key"));
            vm_chunk_emit(ctx->chunk, OP_HASH_GET);
        }
        break;
    }

    case NI_DEREF_HASH: {
        size_t kidx = 0;
        if (try_const_str_key(ctx, ast_get(node, "key"), &kidx)) {
            compile_expr(ctx, ast_get(node, "ref"));
            vm_chunk_emit(ctx->chunk, OP_HASH_GET_CONST);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)kidx);
        } else {
            compile_expr(ctx, ast_get(node, "ref"));
            compile_expr(ctx, ast_get(node, "key"));
            vm_chunk_emit(ctx->chunk, OP_HASH_GET);
        }
        break;
    }

    case NI_DEREF_ARRAY: {
        compile_expr(ctx, ast_get(node, "ref"));
        compile_expr(ctx, ast_get(node, "index"));
        vm_chunk_emit(ctx->chunk, OP_ARRAY_GET);
        break;
    }

    case NI_DEREF_SCALAR: {
        StradaValue *ref_node = ast_get(node, "ref");
        if (!ref_node) ref_node = ast_get(node, "target");
        compile_expr(ctx, ref_node);
        vm_chunk_emit(ctx->chunk, OP_DEREF_SCALAR);
        break;
    }

    case NI_REF: {
        StradaValue *target = ast_get(node, "target");
        const char *ref_type = ast_str(node, "ref_type");
        if (strcmp(ref_type, "$") == 0 || strcmp(ref_type, "scalar") == 0) {
            compile_live_scalar_ref(ctx, target);
        } else if (strcmp(ref_type, "&") == 0 || strcmp(ref_type, "func") == 0) {
            compile_expr(ctx, target);
        } else {
            compile_expr(ctx, target);
            vm_chunk_emit(ctx->chunk, OP_HASH_REF);
        }
        break;
    }

    case NI_ANON_HASH: {
        StradaValue *keys_arr = ast_deref(ast_get(node, "keys"));
        StradaValue *vals_arr = ast_deref(ast_get(node, "values"));
        StradaValue *key_exprs = ast_deref(ast_get(node, "key_exprs"));
        int count = (int)ast_int(node, "pair_count");
        uint32_t init_cap = count > 0 ? (count <= 3 ? 4 : (uint32_t)((count * 4 + 2) / 3)) : 4;
        vm_chunk_emit(ctx->chunk, OP_NEW_HASH);
        vm_chunk_emit_u32(ctx->chunk, init_cap);
        for (int i = 0; i < count; i++) {
            vm_chunk_emit(ctx->chunk, OP_DUP); /* hash on top */
            StradaValue *key_expr = key_exprs ? ast_arr_get(key_exprs, i) : NULL;
            key_expr = ast_deref(key_expr);
            int has_expr_key = (key_expr && !STRADA_IS_TAGGED_INT(key_expr) &&
                                key_expr->type == STRADA_HASH);
            int use_const = 0;
            size_t const_kidx = 0;
            if (has_expr_key) {
                if (try_const_str_key(ctx, key_expr, &const_kidx)) {
                    use_const = 1;
                } else {
                    compile_expr(ctx, key_expr);
                }
            } else if (keys_arr && !STRADA_IS_TAGGED_INT(keys_arr) && keys_arr->type == STRADA_ARRAY) {
                StradaValue *kv = strada_array_get(keys_arr->value.av, i);
                const char *ks = (kv && !STRADA_IS_TAGGED_INT(kv) && kv->type == STRADA_STR) ? kv->value.pv : "";
                size_t kl = (kv && !STRADA_IS_TAGGED_INT(kv) && kv->type == STRADA_STR) ? STRADA_STR_BYTELEN(kv) : 0;
                const_kidx = vm_chunk_add_str_const_len(ctx->chunk, ks ? ks : "", kl);
                use_const = 1;
            } else {
                vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
            }
            if (vals_arr && !STRADA_IS_TAGGED_INT(vals_arr) && vals_arr->type == STRADA_ARRAY) {
                compile_expr(ctx, ast_arr_get(vals_arr, i));
            } else {
                vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
            }
            if (use_const) {
                vm_chunk_emit(ctx->chunk, OP_HASH_SET_CONST);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)const_kidx);
            } else {
                vm_chunk_emit(ctx->chunk, OP_HASH_SET);
            }
        }
        break;
    }

    case NI_ANON_ARRAY: {
        StradaValue *elements = ast_get(node, "elements");
        int count = (int)ast_int(node, "element_count");
        vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
        vm_chunk_emit_u32(ctx->chunk, count > 0 ? count : 4);
        for (int i = 0; i < count; i++) {
            StradaValue *elem = ast_deref(ast_arr_get(elements, i));
            int et = elem ? (int)ast_int(elem, "type") : 0;
            vm_chunk_emit(ctx->chunk, OP_DUP); /* array on top */
            if (et == NI_SPREAD) {
                compile_list_src(ctx, ast_get(elem, "target"));
                vm_chunk_emit(ctx->chunk, OP_ARRAY_EXTEND);
            } else if (vm_array_flatten_kind(ctx, elem) != 0) {
                /* [@a, LIST...]: array values flatten (compiled array_flatten_kind) */
                compile_list_src(ctx, elem);
                vm_chunk_emit(ctx->chunk, OP_ARRAY_EXTEND);
            } else {
                compile_expr(ctx, elem);
                vm_chunk_emit(ctx->chunk, OP_ARRAY_PUSH);
            }
        }
        break;
    }

    case NI_FIELD_ACCESS: {
        const char *field = ast_str(node, "field");
        compile_expr(ctx, ast_get(node, "object"));
        if (field && field[0]) {
            size_t midx = vm_chunk_add_str_const(ctx->chunk, field);
            emit_call_context(ctx, expr_call_ctx);
            vm_chunk_emit(ctx->chunk, OP_FIELD_ACCESS);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)midx);
            vm_chunk_emit(ctx->chunk, 0);
        }
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
                    compile_list_src(ctx, ast_get(arg, "target"));
                    vm_chunk_emit(ctx->chunk, OP_ARRAY_EXTEND);
                } else {
                    compile_expr(ctx, arg);
                    vm_chunk_emit(ctx->chunk, OP_ARRAY_PUSH);
                }
            }
            size_t midx = vm_chunk_add_str_const(ctx->chunk, method);
            emit_call_context(ctx, expr_call_ctx);
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
        emit_call_context(ctx, expr_call_ctx);
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

        emit_call_context(ctx, expr_call_ctx);
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
                    compile_list_src(ctx, ast_get(arg, "target"));
                    vm_chunk_emit(ctx->chunk, OP_ARRAY_EXTEND);
                } else {
                    compile_expr(ctx, arg);
                    vm_chunk_emit(ctx->chunk, OP_ARRAY_PUSH);
                }
            }
            compile_expr(ctx, callee);
            emit_call_context(ctx, expr_call_ctx);
            vm_chunk_emit(ctx->chunk, OP_CALL_CLOSURE_SPREAD);
            break;
        }

        for (int i = 0; i < argc; i++) {
            compile_expr(ctx, ast_arr_get(args, i));
        }
        compile_expr(ctx, callee);
        emit_call_context(ctx, expr_call_ctx);
        vm_chunk_emit(ctx->chunk, OP_CALL_CLOSURE);
        vm_chunk_emit(ctx->chunk, (uint8_t)argc);
        break;
    }

    case NI_ASYNC_FUNC:
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
        int inner_func_idx = (int)(ctx->prog->func_count - 1);

        /* Restore outer chunk pointer (may have been invalidated by realloc) */
        if (outer_func_idx >= 0) ctx->chunk = &ctx->prog->funcs[outer_func_idx];

        inner.prog = ctx->prog;
        inner.func_name = closurename;
        inner.cur_package = ctx->cur_package;
        inner.is_closure = 1;
        inner.parent_ctx = ctx;
        inner.chunk->return_type = (int)ast_int(node, "return_type");

        /* Add parameters */
        int fixed_params = 0;
        for (int i = 0; i < param_count; i++) {
            StradaValue *param = ast_arr_get(params, i);
            const char *pname = ast_str(param, "name");
            const char *psigil = ast_str(param, "sigil");
            int is_variadic = (int)ast_int(param, "is_variadic");
            int pslot = ctx_add_local(&inner, pname, psigil[0] ? psigil[0] : '$');
            if (pslot >= 0 && (!psigil[0] || psigil[0] == '$')) {
                int pt = (int)ast_int(param, "param_type");
                if (pt == 1 || pt == 2) inner.local_types[pslot] = (uint8_t)pt;
            }
            if (is_variadic || strcmp(psigil, "...@") == 0) {
                inner.chunk->has_variadic = 1;
            } else {
                fixed_params++;
            }
        }
        inner.chunk->fixed_param_count = fixed_params;
        inner.may_box_locals = ast_may_box_locals(body, 0) || ast_may_box_locals(params, 0);
        emit_param_defaults(&inner, params, param_count, 0, inner_func_idx);

        prescan_vm_labels(&inner, body, 0, 0);
        compile_block(&inner, body);

        /* Refresh chunk pointers after potential realloc from nested closures */
        inner.chunk = &ctx->prog->funcs[inner_func_idx];
        if (outer_func_idx >= 0) ctx->chunk = &ctx->prog->funcs[outer_func_idx];

        resolve_vm_gotos(&inner);

        /* Implicit return undef */
        vm_chunk_emit(inner.chunk, OP_PUSH_UNDEF);
        vm_chunk_emit(inner.chunk, OP_RETURN);
        if (inner.chunk->code_len > 0xFFFF) {
            fprintf(stderr, "VM compile error: closure '%s' compiles to %zu bytes of bytecode, "
                            "exceeding the VM backend's 65535-byte limit\n",
                    closurename, (size_t)inner.chunk->code_len);
            inner.errors++;
        }
        ctx->errors += inner.errors;

        int inner_final_lc = inner.max_local_count > inner.local_count ? inner.max_local_count : inner.local_count;
        inner.chunk->local_count = inner_final_lc;
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
            int outer_slot = ctx_find_local_sigil(ctx, inner.captures[i].name, inner.captures[i].sigil);
            if (outer_slot >= 0) {
                /* OP_MAKE_CLOSURE boxes locals[outer_slots[i]] directly to preserve weakrefs/tied */
                vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
                outer_slots[i] = outer_slot;
            } else {
                /* Try capture from parent (pass raw VMCell so multi-level closures share mutation) */
                int cap = ctx_find_capture(ctx, inner.captures[i].name, inner.captures[i].sigil);
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
        for (int i = 0; i < inner_final_lc; i++) if (inner.local_names[i]) free(inner.local_names[i]);
        for (int i = 0; i < inner.capture_count; i++) free(inner.captures[i].name);
        ctx_free_local_restores(&inner);
        break;
    }

    case NI_FUNC_REF: {
        const char *fname = ast_str(node, "name");
        int fidx = resolve_func_idx_for_ctx(ctx, fname, NULL, 0);
        if (fidx >= 0) {
            vm_chunk_emit(ctx->chunk, OP_MAKE_CLOSURE);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)fidx);
            vm_chunk_emit(ctx->chunk, 0);
        } else {
            char msg[320];
            snprintf(msg, sizeof(msg), "Undefined function for reference: %s", fname ? fname : "");
            size_t midx = vm_chunk_add_str_const(ctx->chunk, msg);
            vm_chunk_emit(ctx->chunk, OP_PUSH_STR);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)midx);
            vm_chunk_emit(ctx->chunk, OP_THROW);
            vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
        }
        break;
    }

    case NI_DUNDER_PACKAGE: {
        const char *dpkg = ast_str(node, "package");
        if (!dpkg || !dpkg[0]) {
            dpkg = (ctx->cur_package && ctx->cur_package[0]) ? ctx->cur_package : "main";
        }
        size_t sidx = vm_chunk_add_str_const(ctx->chunk, dpkg);
        vm_chunk_emit(ctx->chunk, OP_PUSH_STR);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)sidx);
        break;
    }

    case NI_DUNDER_FILE: {
        const char *dfile = (g_source_file && g_source_file[0]) ? g_source_file : "(eval)";
        size_t sidx = vm_chunk_add_str_const(ctx->chunk, dfile);
        vm_chunk_emit(ctx->chunk, OP_PUSH_STR);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)sidx);
        break;
    }

    case NI_DUNDER_LINE: {
        int64_t lval = ast_int(node, "line_value");
        vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
        vm_chunk_emit_i64(ctx->chunk, lval);
        break;
    }

    case NI_AWAIT: {
        vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
        vm_chunk_emit_i64(ctx->chunk, 7);
        compile_expr(ctx, ast_get(node, "expr"));
        vm_chunk_emit(ctx->chunk, OP_BUILTIN);
        vm_chunk_emit_u16(ctx->chunk, BUILTIN_ASYNC_OP);
        vm_chunk_emit(ctx->chunk, 2);
        break;
    }

    case NI_REGEX_MATCH: {
        size_t pat_len = 0;
        const char *pattern = ast_str_len(node, "pattern", &pat_len);
        const char *op = ast_str(node, "op");
        const char *mflags = ast_str(node, "flags");
        int negated = (strcmp(op, "!~") == 0);
        compile_expr(ctx, ast_get(node, "target"));
        if (compile_rx_dynamic_pattern(ctx, node, pattern, pat_len, mflags)) {
            vm_chunk_emit(ctx->chunk, negated ? OP_REGEX_NOT_MATCH : OP_REGEX_MATCH);
            vm_chunk_emit_u16(ctx->chunk, 0xFFFF);
            vm_chunk_emit(ctx->chunk, 0);
            break;
        }
        size_t pidx = add_pattern_const(ctx, pattern, pat_len, mflags);
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
            } else {
                const char *gkey = resolve_global_var_key(ctx, name);
                if (gkey) {
                    size_t key_idx = vm_chunk_add_str_const(ctx->chunk, gkey);
                    vm_chunk_emit(ctx->chunk, OP_LOAD_GLOBAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)key_idx);
                } else {
                    vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
                }
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

        /* Built-in: say() / print() */
        if (strcmp(name, "say") == 0 || strcmp(name, "print") == 0) {
            int is_say = (strcmp(name, "say") == 0);
            if (argc == 0) {
                if (is_say) {
                    size_t empty_idx = vm_chunk_add_str_const(ctx->chunk, "");
                    vm_chunk_emit(ctx->chunk, OP_PUSH_STR);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)empty_idx);
                    vm_chunk_emit(ctx->chunk, OP_SAY);
                }
            } else if (argc == 1) {
                compile_expr(ctx, ast_arr_get(args, 0));
                vm_chunk_emit(ctx->chunk, is_say ? OP_SAY : OP_PRINT);
            } else if (argc == 2) {
                /* say($fh, $str) / print($fh, $str) */
                compile_expr(ctx, ast_arr_get(args, 0));
                compile_expr(ctx, ast_arr_get(args, 1));
                vm_chunk_emit(ctx->chunk, is_say ? OP_SAY_FH : OP_PRINT_FH);
            } else {
                for (int ai = 0; ai < argc - 1; ai++) {
                    compile_expr(ctx, ast_arr_get(args, ai));
                    vm_chunk_emit(ctx->chunk, OP_PRINT);
                }
                compile_expr(ctx, ast_arr_get(args, argc - 1));
                vm_chunk_emit(ctx->chunk, is_say ? OP_SAY : OP_PRINT);
            }
            /* print/say return 1 (Perl, compiled backend) */
            vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
            vm_chunk_emit_i64(ctx->chunk, 1);
            break;
        }

        /* Built-in: length() */
        if (strcmp(name, "length") == 0 && argc == 1) {
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_STR_LEN);
            break;
        }

        /* Built-in: push(@arr, LIST). Array-valued arguments flatten; with
         * several values they are gathered first so a tied target gets one
         * PUSH call with the whole list (Perl). */
        if ((strcmp(name, "push") == 0 || strcmp(name, "unshift") == 0) && argc == 1
            && resolve_vm_func_idx(ctx, name) < 0) {
            /* push(@a, ()) -- the empty list was dropped: nothing to add */
            compile_mutable_array_target(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_ARRAY_SIZE);
            break;
        }
        if (strcmp(name, "push") == 0 && argc >= 2) {
            compile_mutable_array_target(ctx, ast_arr_get(args, 0));
            if (argc == 2) {
                StradaValue *parg = ast_deref(ast_arr_get(args, 1));
                int pat = parg ? (int)ast_int(parg, "type") : 0;
                if (pat == NI_SPREAD) {
                    compile_list_src(ctx, ast_get(parg, "target"));
                    vm_chunk_emit(ctx->chunk, OP_ARRAY_EXTEND);
                } else if (vm_array_flatten_kind(ctx, parg) >= 1) {
                    compile_list_src(ctx, parg);
                    vm_chunk_emit(ctx->chunk, OP_ARRAY_EXTEND);
                } else {
                    vm_chunk_emit(ctx->chunk, OP_DUP);
                    compile_expr(ctx, parg);
                    vm_chunk_emit(ctx->chunk, OP_ARRAY_PUSH);
                    vm_chunk_emit(ctx->chunk, OP_POP);
                }
                vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
                break;
            }
            vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
            vm_chunk_emit_u32(ctx->chunk, (uint32_t)((argc - 1) * 2));
            for (int pi = 1; pi < argc; pi++) {
                StradaValue *parg = ast_deref(ast_arr_get(args, pi));
                int pat = parg ? (int)ast_int(parg, "type") : 0;
                vm_chunk_emit(ctx->chunk, OP_DUP);
                if (pat == NI_SPREAD) {
                    compile_list_src(ctx, ast_get(parg, "target"));
                    vm_chunk_emit(ctx->chunk, OP_ARRAY_EXTEND);
                } else if (vm_array_flatten_kind(ctx, parg) >= 1) {
                    compile_list_src(ctx, parg);
                    vm_chunk_emit(ctx->chunk, OP_ARRAY_EXTEND);
                } else {
                    compile_expr(ctx, parg);
                    vm_chunk_emit(ctx->chunk, OP_ARRAY_PUSH);
                }
            }
            vm_chunk_emit(ctx->chunk, OP_ARRAY_EXTEND);
            vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
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
        if (strcmp(name, "unshift") == 0 && argc >= 2) {
            int has_spread = 0;
            for (int i = 1; i < argc; i++) {
                StradaValue *ua = ast_deref(ast_arr_get(args, i));
                if (ua && ((int)ast_int(ua, "type") == NI_SPREAD || vm_array_flatten_kind(ctx, ua) >= 1)) {
                    has_spread = 1;
                    break;
                }
            }
            if (has_spread || argc > 2) {
                compile_mutable_array_target(ctx, ast_arr_get(args, 0));
                vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
                vm_chunk_emit_u32(ctx->chunk, (uint32_t)(argc > 1 ? (argc - 1) * 2 : 4));
                for (int i = 1; i < argc; i++) {
                    StradaValue *ua = ast_deref(ast_arr_get(args, i));
                    int ut = ua ? (int)ast_int(ua, "type") : 0;
                    vm_chunk_emit(ctx->chunk, OP_DUP);
                    if (ut == NI_SPREAD) {
                        compile_list_src(ctx, ast_get(ua, "target"));
                        vm_chunk_emit(ctx->chunk, OP_ARRAY_EXTEND);
                    } else if (vm_array_flatten_kind(ctx, ua) >= 1) {
                        compile_list_src(ctx, ua);
                        vm_chunk_emit(ctx->chunk, OP_ARRAY_EXTEND);
                    } else {
                        compile_expr(ctx, ua);
                        vm_chunk_emit(ctx->chunk, OP_ARRAY_PUSH);
                    }
                }
                vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                vm_chunk_emit_u16(ctx->chunk, BUILTIN_ARRAY_UNSHIFT_LIST);
                vm_chunk_emit(ctx->chunk, 2);
            } else {
                compile_mutable_array_target(ctx, ast_arr_get(args, 0));
                compile_expr(ctx, ast_arr_get(args, 1));
                vm_chunk_emit(ctx->chunk, OP_ARRAY_UNSHIFT);
            }
            vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
            break;
        }

        /* Built-in: reverse() */
        if (strcmp(name, "reverse") == 0 && argc == 1 && !vm_is_hash_sigil_node(ast_arr_get(args, 0))) {
            compile_mutable_array_target(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_ARRAY_REVERSE);
            break;
        }
        if (strcmp(name, "reverse") == 0 && resolve_vm_func_idx(ctx, name) < 0) {
            /* reverse(LIST): flatten every argument (%h gives its pairs) */
            vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
            vm_chunk_emit_u32(ctx->chunk, (uint32_t)(argc > 0 ? argc : 1));
            for (int ri = 0; ri < argc; ri++) {
                StradaValue *ra = ast_arr_get(args, ri);
                vm_chunk_emit(ctx->chunk, OP_DUP);
                compile_list_src(ctx, ra);
                vm_chunk_emit(ctx->chunk, vm_array_flatten_kind(ctx, ra) != 0 ? OP_ARRAY_EXTEND : OP_ARRAY_PUSH);
            }
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
                    StradaValue *rnode = ast_get(arg0, "ref");
                    if (!rnode) rnode = ast_get(arg0, "target");
                    compile_expr(ctx, rnode);
                    compile_expr(ctx, ast_get(arg0, "key"));
                    vm_chunk_emit(ctx->chunk, OP_HASH_EXISTS);
                } else if (atype == NI_SUBSCRIPT) {
                    compile_expr(ctx, ast_get(arg0, "array"));
                    compile_expr(ctx, ast_get(arg0, "index"));
                    vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                    vm_chunk_emit_u16(ctx->chunk, BUILTIN_ARRAY_EXISTS);
                    vm_chunk_emit(ctx->chunk, 2);
                } else if (atype == NI_DEREF_ARRAY) {
                    StradaValue *rnode = ast_get(arg0, "ref");
                    if (!rnode) rnode = ast_get(arg0, "target");
                    compile_expr(ctx, rnode);
                    compile_expr(ctx, ast_get(arg0, "index"));
                    vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                    vm_chunk_emit_u16(ctx->chunk, BUILTIN_ARRAY_EXISTS);
                    vm_chunk_emit(ctx->chunk, 2);
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
            break;
        }

        /* Built-in: delete() */
        if (strcmp(name, "delete") == 0 && argc == 1) {
            StradaValue *arg0 = ast_deref(ast_arr_get(args, 0));
            int handled = 0;
            if (arg0 && !STRADA_IS_TAGGED_INT(arg0)) {
                int atype = (int)ast_int(arg0, "type");
                if (atype == NI_HASH_SLICE) {
                    /* delete @h{LIST} */
                    StradaValue *keys = ast_get(arg0, "items");
                    int kc = (int)ast_int(arg0, "item_count");
                    compile_expr(ctx, ast_get(arg0, "source"));
                    vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
                    vm_chunk_emit_u32(ctx->chunk, (uint32_t)(kc > 0 ? kc : 4));
                    for (int ki = 0; ki < kc; ki++) {
                        StradaValue *ke = ast_arr_get(keys, ki);
                        vm_chunk_emit(ctx->chunk, OP_DUP);
                        compile_list_src(ctx, ke);
                        vm_chunk_emit(ctx->chunk, vm_array_flatten_kind(ctx, ke) != 0 ? OP_ARRAY_EXTEND : OP_ARRAY_PUSH);
                    }
                    vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                    vm_chunk_emit_u16(ctx->chunk, BUILTIN_HASH_DELETE_SLICE);
                    vm_chunk_emit(ctx->chunk, 2);
                    handled = 1;
                } else if (atype == NI_HASH_ACCESS) {
                    compile_expr(ctx, ast_get(arg0, "hash"));
                    compile_expr(ctx, ast_get(arg0, "key"));
                    vm_chunk_emit(ctx->chunk, OP_HASH_DELETE);
                    handled = 1;
                } else if (atype == NI_DEREF_HASH) {
                    StradaValue *rnode = ast_get(arg0, "ref");
                    if (!rnode) rnode = ast_get(arg0, "target");
                    compile_expr(ctx, rnode);
                    compile_expr(ctx, ast_get(arg0, "key"));
                    vm_chunk_emit(ctx->chunk, OP_HASH_DELETE);
                    handled = 1;
                } else if (atype == NI_SUBSCRIPT) {
                    compile_expr(ctx, ast_get(arg0, "array"));
                    compile_expr(ctx, ast_get(arg0, "index"));
                    vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                    vm_chunk_emit_u16(ctx->chunk, BUILTIN_ARRAY_DELETE);
                    vm_chunk_emit(ctx->chunk, 2);
                    handled = 1;
                } else if (atype == NI_DEREF_ARRAY) {
                    StradaValue *rnode = ast_get(arg0, "ref");
                    if (!rnode) rnode = ast_get(arg0, "target");
                    compile_expr(ctx, rnode);
                    compile_expr(ctx, ast_get(arg0, "index"));
                    vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                    vm_chunk_emit_u16(ctx->chunk, BUILTIN_ARRAY_DELETE);
                    vm_chunk_emit(ctx->chunk, 2);
                    handled = 1;
                }
            }
            if (!handled) {
                vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
            }
            break;
        }

        /* Built-in: split() */
        if (strcmp(name, "split") == 0 && (argc == 2 || argc == 3)) {
            if (argc == 3) {
                compile_expr(ctx, ast_arr_get(args, 2)); /* limit */
                compile_split_pattern(ctx, ast_arr_get(args, 0)); /* delimiter */
                compile_expr(ctx, ast_arr_get(args, 1)); /* string */
                vm_chunk_emit(ctx->chunk, OP_STR_SPLIT_LIMIT);
            } else {
                compile_split_pattern(ctx, ast_arr_get(args, 0));
                compile_expr(ctx, ast_arr_get(args, 1));
                vm_chunk_emit(ctx->chunk, OP_STR_SPLIT);
            }
            break;
        }

        /* Built-in: join() */
        if (strcmp(name, "join") == 0 && argc >= 2) {
            /* join(SEP, LIST): array values flatten, every other argument
             * (a reference too) is one element, as in the compiled backend */
            compile_expr(ctx, ast_arr_get(args, 0));
            StradaValue *ja1 = ast_arr_get(args, 1);
            const char *ja1_sig = (ja1 && (((int)ast_int(ja1, "type")) == NI_VARIABLE || ((int)ast_int(ja1, "type")) == NI_DEREF_SCALAR)) ? ast_str(ja1, "sigil") : NULL;
            if (argc == 2 && vm_array_flatten_kind(ctx, ja1) != 0 && !(ja1_sig && ja1_sig[0] == '%')) {
                compile_expr(ctx, ja1);
            } else {
                vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
                vm_chunk_emit_u32(ctx->chunk, (uint32_t)(argc > 1 ? argc - 1 : 1));
                for (int ji = 1; ji < argc; ji++) {
                    StradaValue *ja = ast_arr_get(args, ji);
                    StradaValue *jd = ast_deref(ja);
                    vm_chunk_emit(ctx->chunk, OP_DUP);
                    if (jd && !STRADA_IS_TAGGED_INT(jd) && (int)ast_int(jd, "type") == NI_SPREAD) {
                        compile_list_src(ctx, ast_get(jd, "target"));
                        vm_chunk_emit(ctx->chunk, OP_ARRAY_EXTEND);
                    } else if (vm_array_flatten_kind(ctx, ja) != 0) {
                        compile_list_src(ctx, ja);
                        vm_chunk_emit(ctx->chunk, OP_ARRAY_EXTEND);
                    } else {
                        compile_expr(ctx, ja);
                        vm_chunk_emit(ctx->chunk, OP_ARRAY_PUSH);
                    }
                }
            }
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
        if (strcmp(name, "substr") == 0 && argc == 4) {
            compile_substr_replace(ctx, args, 4, ast_arr_get(args, 3), 1);
            break;
        }
        if (strcmp(name, "substr") == 0 && (argc == 2 || argc == 3)) {
            compile_expr(ctx, ast_arr_get(args, 0));
            compile_expr(ctx, ast_arr_get(args, 1));
            if (argc == 3) {
                compile_expr(ctx, ast_arr_get(args, 2));
            } else {
                /* substr with 2 args: rest of string (clamp within 63-bit tagged int range) */
                vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
                vm_chunk_emit_i64(ctx->chunk, VM_INT_MAX);
            }
            vm_chunk_emit(ctx->chunk, OP_SUBSTR);
            break;
        }

        /* Built-in: index() */
        if (strcmp(name, "index") == 0 && (argc == 2 || argc == 3)) {
            compile_expr(ctx, ast_arr_get(args, 0));
            compile_expr(ctx, ast_arr_get(args, 1));
            if (argc == 3) {
                compile_expr(ctx, ast_arr_get(args, 2));
                vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                vm_chunk_emit_u16(ctx->chunk, BUILTIN_STR_INDEX_3);
                vm_chunk_emit(ctx->chunk, 3);
            } else {
                vm_chunk_emit(ctx->chunk, OP_STR_INDEX);
            }
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
        /* hex()/oct(): a user function of that name takes precedence
         * (as in the compiled backend) */
        if (strcmp(name, "hex") == 0 && argc == 1 && resolve_vm_func_idx(ctx, name) < 0) {
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_HEX);
            vm_chunk_emit(ctx->chunk, 1);
            break;
        }

        /* Built-in: oct() */
        if (strcmp(name, "oct") == 0 && argc == 1 && resolve_vm_func_idx(ctx, name) < 0) {
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
        if (strcmp(name, "chomp") == 0 && argc == 1 && vm_array_flatten_kind(ctx, ast_arr_get(args, 0)) == 1
            && !vm_is_hash_sigil_node(ast_arr_get(args, 0))) {
            /* chomp(@lines) */
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_CHOMP_ARRAY);
            vm_chunk_emit(ctx->chunk, 1);
            break;
        }
        if (strcmp(name, "chomp") == 0 && argc == 1) {
            compile_in_place_str_op(ctx, ast_arr_get(args, 0), 0);
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
            compile_in_place_str_op(ctx, ast_arr_get(args, 0), 1);
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
        if (strcmp(name, "splice") == 0 && argc >= 1) {
            compile_mutable_array_target(ctx, ast_arr_get(args, 0));
            if (argc >= 2) {
                compile_expr(ctx, ast_arr_get(args, 1));
            }
            if (argc >= 3) {
                compile_expr(ctx, ast_arr_get(args, 2));
            }
            int emit_argc = argc;
            if (argc == 4) {
                StradaValue *rarg = ast_deref(ast_arr_get(args, 3));
                int rt = rarg ? (int)ast_int(rarg, "type") : 0;
                if (rt == NI_SPREAD) {
                    vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
                    vm_chunk_emit_u32(ctx->chunk, 8);
                    vm_chunk_emit(ctx->chunk, OP_DUP);
                    compile_list_src(ctx, ast_get(rarg, "target"));
                    vm_chunk_emit(ctx->chunk, OP_ARRAY_EXTEND);
                } else {
                    compile_expr(ctx, ast_arr_get(args, 3));
                }
            } else if (argc > 4) {
                vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
                vm_chunk_emit_u32(ctx->chunk, (uint32_t)((argc - 3) * 2));
                for (int i = 3; i < argc; i++) {
                    StradaValue *rarg = ast_deref(ast_arr_get(args, i));
                    int rt = rarg ? (int)ast_int(rarg, "type") : 0;
                    vm_chunk_emit(ctx->chunk, OP_DUP);
                    if (rt == NI_SPREAD) {
                        compile_list_src(ctx, ast_get(rarg, "target"));
                        vm_chunk_emit(ctx->chunk, OP_ARRAY_EXTEND);
                    } else {
                        compile_expr(ctx, rarg);
                        vm_chunk_emit(ctx->chunk, OP_ARRAY_PUSH);
                    }
                }
                emit_argc = 4;
            }
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_ARRAY_SPLICE);
            vm_chunk_emit(ctx->chunk, (uint8_t)emit_argc);
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
            if (argc > 255) {
                fprintf(stderr, "VM compile error: sprintf() with %d arguments exceeds the bytecode VM limit of 255\n", argc);
                ctx->errors++;
                vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
                break;
            }
            if (vm_args_have_flatten(ctx, args, argc, 1)) {
                /* sprintf(FMT, @list): flatten the arguments (Perl) */
                compile_flat_arg_list(ctx, args, argc);
                vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                vm_chunk_emit_u16(ctx->chunk, BUILTIN_SPRINTF_LIST);
                vm_chunk_emit(ctx->chunk, 1);
                break;
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

            if (strcmp(fn, "arena_begin") == 0 || strcmp(fn, "arena_end") == 0) {
                for (int i = 0; i < argc; i++) {
                    compile_expr(ctx, ast_arr_get(args, i));
                    vm_chunk_emit(ctx->chunk, OP_POP);
                }
                vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
                break;
            }

            if (strcmp(fn, "gc_collect") == 0 && argc == 0) {
                vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                vm_chunk_emit_u16(ctx->chunk, BUILTIN_CORE_GC_COLLECT);
                vm_chunk_emit(ctx->chunk, 0);
                break;
            }

            if (strcmp(fn, "vec_set") == 0 && argc == 4) {
                VMLValueTarget lv = compile_lvalue_prepare(ctx, ast_arr_get(args, 0));
                if (lv.kind != LV_NONE) {
                    compile_lvalue_load(ctx, &lv);
                    for (int i = 1; i < 4; i++) compile_expr(ctx, ast_arr_get(args, i));
                    vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                    vm_chunk_emit_u16(ctx->chunk, BUILTIN_VEC_SET);
                    vm_chunk_emit(ctx->chunk, 4);
                    int val_slot = ctx_add_local(ctx, "__vs_v", '$');
                    emit_reset_store_local(ctx, val_slot);
                    compile_lvalue_store_slot(ctx, &lv, val_slot, 0);
                    compile_lvalue_finish(ctx, &lv);
                } else {
                    for (int i = 0; i < 4; i++) compile_expr(ctx, ast_arr_get(args, i));
                    vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                    vm_chunk_emit_u16(ctx->chunk, BUILTIN_VEC_SET);
                    vm_chunk_emit(ctx->chunk, 4);
                    vm_chunk_emit(ctx->chunk, OP_POP);
                }
                vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
                break;
            }

            if ((strcmp(fn, "weaken") == 0 || strcmp(fn, "isweak") == 0) && argc == 1) {
                uint8_t wop = (strcmp(fn, "weaken") == 0) ? OP_WEAKEN_SLOT : OP_ISWEAK_SLOT;
                StradaValue *arg0 = ast_deref(ast_arr_get(args, 0));
                int atype = (arg0 && !STRADA_IS_TAGGED_INT(arg0)) ? (int)ast_int(arg0, "type") : 0;
                int is_hash = 0;
                StradaValue *cont = NULL, *idx = NULL;
                if (atype == NI_VARIABLE) {
                    const char *vname = ast_str(arg0, "name");
                    const char *vsigil = ast_str(arg0, "sigil");
                    char sc = (vsigil && vsigil[0]) ? vsigil[0] : '$';
                    int slot = ctx_find_local_sigil(ctx, vname, sc);
                    int cap = (slot < 0) ? ctx_resolve_capture(ctx, vname, sc) : -1;
                    const char *gkey = (slot < 0 && cap < 0) ? resolve_global_var_key(ctx, vname) : NULL;
                    if (slot >= 0) {
                        vm_chunk_emit(ctx->chunk, wop);
                        vm_chunk_emit(ctx->chunk, 0);
                        vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                    } else if (cap >= 0) {
                        vm_chunk_emit(ctx->chunk, wop);
                        vm_chunk_emit(ctx->chunk, 1);
                        vm_chunk_emit_u16(ctx->chunk, (uint16_t)cap);
                    } else if (gkey != NULL) {
                        size_t gk = vm_chunk_add_str_const(ctx->chunk, gkey);
                        vm_chunk_emit(ctx->chunk, wop);
                        vm_chunk_emit(ctx->chunk, 2);
                        vm_chunk_emit_u16(ctx->chunk, (uint16_t)gk);
                    } else {
                        compile_expr(ctx, arg0);
                        vm_chunk_emit(ctx->chunk, wop);
                        vm_chunk_emit(ctx->chunk, 5);
                        vm_chunk_emit_u16(ctx->chunk, 0);
                    }
                } else if (extract_cont_idx(arg0, &is_hash, &cont, &idx)) {
                    compile_expr(ctx, cont);
                    compile_expr(ctx, idx);
                    vm_chunk_emit(ctx->chunk, wop);
                    vm_chunk_emit(ctx->chunk, is_hash ? 3 : 4);
                    vm_chunk_emit_u16(ctx->chunk, 0);
                } else {
                    compile_expr(ctx, arg0);
                    vm_chunk_emit(ctx->chunk, wop);
                    vm_chunk_emit(ctx->chunk, 5);
                    vm_chunk_emit_u16(ctx->chunk, 0);
                }
                break;
            }

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
            else if (strcmp(fn, "fwrite") == 0) bid = BUILTIN_CORE_FWRITE;
            else if (strcmp(fn, "fread") == 0) bid = BUILTIN_CORE_FREAD;
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
            /* sleep, usleep: handled by generic runtime bridge so signals dispatch
             * and return values match compiled Strada */
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
            /* kill, signal: handled by generic runtime bridge */
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
            else if (strcmp(fn, "pack") == 0) {
                bid = BUILTIN_CORE_PACK;
                if (vm_args_have_flatten(ctx, args, argc, 1)) {
                    compile_flat_arg_list(ctx, args, argc);
                    vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                    vm_chunk_emit_u16(ctx->chunk, BUILTIN_PACK_LIST);
                    vm_chunk_emit(ctx->chunk, 1);
                    bid = -2;   /* emitted */
                }
            }
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
            /* truncate, nanosleep, access: handled by the generic runtime bridge
             * (raw syscall return, signal dispatch, and compiled semantics parity) */
            else if (strcmp(fn, "gethostname") == 0) bid = BUILTIN_CORE_GETHOSTNAME;
            else if (strcmp(fn, "glob") == 0) bid = BUILTIN_CORE_GLOB;
            /* Socket */
            else if (strcmp(fn, "socket_client") == 0) bid = BUILTIN_CORE_SOCKET_CLIENT;
            else if (strcmp(fn, "socket_server") == 0) bid = BUILTIN_CORE_SOCKET_SERVER;
            else if (strcmp(fn, "socket_server_backlog") == 0) bid = BUILTIN_CORE_SOCKET_SERVER_BACKLOG;
            else if (strcmp(fn, "socket_server_host") == 0) bid = BUILTIN_CORE_SOCKET_SERVER_HOST;
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
            else if (strcmp(fn, "wantscalar") == 0) bid = BUILTIN_CORE_WANTSCALAR;
            else if (strcmp(fn, "wanthash") == 0) bid = BUILTIN_CORE_WANTHASH;
            else if (strcmp(fn, "select_fds") == 0) bid = BUILTIN_CORE_SELECT_FDS;
            else if (strcmp(fn, "set_byte") == 0) bid = BUILTIN_CORE_SET_BYTE;
            else if (strcmp(fn, "byte_substr") == 0) bid = BUILTIN_CORE_BYTE_SUBSTR;
            else if (strcmp(fn, "substr_bytes") == 0 && (argc == 2 || argc == 3)) bid = BUILTIN_SUBSTR_BYTES;
            else if (strcmp(fn, "byte_at") == 0 && argc == 2) bid = BUILTIN_BYTE_AT;
            else if (strcmp(fn, "clone") == 0 && argc == 1) bid = BUILTIN_CLONE;
            else if (strcmp(fn, "vec_get") == 0 && argc == 3) bid = BUILTIN_VEC_GET;
            else if (strcmp(fn, "write_fd") == 0 && argc == 2) bid = BUILTIN_CORE_WRITE_FD;
            else if (strcmp(fn, "close_fd") == 0 && argc == 1) bid = BUILTIN_CORE_CLOSE_FD;
            else if (strcmp(fn, "stacktrace") == 0 && argc == 0) bid = BUILTIN_STACKTRACE;
            else if (strcmp(fn, "stacktrace_str") == 0 && argc == 0) bid = BUILTIN_CORE_STACK_TRACE;
            else if (strcmp(fn, "reserve") == 0 && argc == 2) bid = BUILTIN_RESERVE;
            else if (strcmp(fn, "deref_set") == 0 && argc == 2) bid = BUILTIN_DEREF_SET;
            else if (strcmp(fn, "deref_array") == 0 && argc == 1) bid = BUILTIN_DEREF_ARRAY;
            else if (strcmp(fn, "deref_hash") == 0 && argc == 1) bid = BUILTIN_DEREF_HASH;
            else if (strcmp(fn, "dump") == 0 && (argc == 1 || argc == 2)) bid = BUILTIN_DUMP;
            else if ((strcmp(fn, "dumper") == 0 || strcmp(fn, "dumper_str") == 0) && argc == 1) bid = BUILTIN_DUMPER_STR;
            else if (strcmp(fn, "hex") == 0 && argc == 1) bid = BUILTIN_HEX;
            else if (strcmp(fn, "oct") == 0 && argc == 1) bid = BUILTIN_OCT;

            if (bid == -2) break;
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
            /* futures over the runtime pool (BUILTIN_ASYNC_OP) */
            int aop = -1;
            if (strcmp(fn, "spawn") == 0 && argc == 1) aop = 8;
            else if (strcmp(fn, "timeout") == 0 && argc == 2) aop = 0;
            else if (strcmp(fn, "sleep") == 0 && argc == 1) aop = 1;
            else if (strcmp(fn, "cancelled") == 0 && argc == 0) aop = 2;
            else if (strcmp(fn, "map") == 0 && (argc == 2 || argc == 3)) aop = 3;
            else if (strcmp(fn, "cancel") == 0 && argc == 1) aop = 4;
            else if (strcmp(fn, "is_done") == 0 && argc == 1) aop = 5;
            else if (strcmp(fn, "is_cancelled") == 0 && argc == 1) aop = 6;
            else if (strcmp(fn, "await") == 0 && argc == 1) aop = 7;
            else if (strcmp(fn, "io_wait") == 0 && argc == 3) aop = 9;
            else if (strcmp(fn, "resolve") == 0 && argc == 1) aop = 10;
            else if (strcmp(fn, "pool_init") == 0 && argc <= 1) aop = 11;
            else if (strcmp(fn, "pool_shutdown") == 0 && argc == 0) aop = 12;
            if (aop >= 0) {
                vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
                vm_chunk_emit_i64(ctx->chunk, aop);
                for (int i = 0; i < argc; i++) compile_expr(ctx, ast_arr_get(args, i));
                vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                vm_chunk_emit_u16(ctx->chunk, BUILTIN_ASYNC_OP);
                vm_chunk_emit(ctx->chunk, (uint8_t)(argc + 1));
                break;
            }
            const char *rtn = NULL;
            int rta = 0;
            if (strcmp(fn, "all") == 0) { rtn = "strada_future_all"; rta = 1; }
            else if (strcmp(fn, "race") == 0) { rtn = "strada_future_race"; rta = 1; }
            else if (strcmp(fn, "try_get") == 0) { rtn = "strada_future_try_get"; rta = 1; }
            else if (strcmp(fn, "select") == 0) { rtn = "strada_channel_select"; rta = 2; }
            if (rtn && argc <= rta) {
                int gi = vm_generic_builtin_find_min(rtn, argc);
                if (gi >= 0) {
                    for (int i = 0; i < argc; i++) compile_expr(ctx, ast_arr_get(args, i));
                    vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)(VM_GENERIC_BID_BASE + gi));
                    vm_chunk_emit(ctx->chunk, (uint8_t)argc);
                    break;
                }
            }
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
            if (bid < 0) {
                /* mutex_* / cond_* / tls_*: the runtime's strada_<name> via
                 * the generic bridge (which marks a blocking call GC-safe) */
                char tbuf[96];
                snprintf(tbuf, sizeof(tbuf), "strada_%s", fn);
                int tgi = vm_generic_builtin_find_min(tbuf, argc);
                if (tgi >= 0) {
                    for (int i = 0; i < argc; i++) compile_expr(ctx, ast_arr_get(args, i));
                    vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)(VM_GENERIC_BID_BASE + tgi));
                    vm_chunk_emit(ctx->chunk, (uint8_t)argc);
                    break;
                }
            }
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
            if (argc >= 1 && (strcmp(fn, "decode") == 0 ||
                              strcmp(fn, "downgrade") == 0 ||
                              strcmp(fn, "upgrade") == 0)) {
                VMLValueTarget lv = compile_lvalue_prepare(ctx, ast_arr_get(args, 0));
                if (lv.kind != LV_NONE) {
                    compile_lvalue_load(ctx, &lv);
                    for (int i = 1; i < argc; i++) compile_expr(ctx, ast_arr_get(args, i));
                    uint16_t wb_bid = (strcmp(fn, "decode") == 0) ? BUILTIN_UTF8_DECODE_WB
                                    : (strcmp(fn, "downgrade") == 0 ? BUILTIN_UTF8_DOWNGRADE_WB
                                                                    : BUILTIN_UTF8_UPGRADE_WB);
                    vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                    vm_chunk_emit_u16(ctx->chunk, wb_bid);
                    vm_chunk_emit(ctx->chunk, (uint8_t)argc);
                    int pair_slot = ctx_add_local(ctx, "__u8_p", '$');
                    int val_slot = ctx_add_local(ctx, "__u8_v", '$');
                    emit_reset_store_local(ctx, pair_slot);
                    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)pair_slot);
                    vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
                    vm_chunk_emit_i64(ctx->chunk, 0);
                    vm_chunk_emit(ctx->chunk, OP_ARRAY_GET);
                    emit_reset_store_local(ctx, val_slot);
                    compile_lvalue_store_slot(ctx, &lv, val_slot, 0);
                    compile_lvalue_finish(ctx, &lv);
                    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)pair_slot);
                    vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
                    vm_chunk_emit_i64(ctx->chunk, 1);
                    vm_chunk_emit(ctx->chunk, OP_ARRAY_GET);
                    break;
                }
            }
            int bid = -1;
            if (strcmp(fn, "is_utf8") == 0) bid = BUILTIN_UTF8_IS_UTF8;
            else if (strcmp(fn, "nfc") == 0) bid = BUILTIN_UTF8_NFC;
            else if (strcmp(fn, "nfd") == 0) bid = BUILTIN_UTF8_NFD;
            else if (strcmp(fn, "nfkc") == 0) bid = BUILTIN_UTF8_NFKC;
            else if (strcmp(fn, "nfkd") == 0) bid = BUILTIN_UTF8_NFKD;
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
        if (strcmp(name, "capture") == 0 && argc == 2) { compile_expr(ctx, ast_arr_get(args, 0)); compile_expr(ctx, ast_arr_get(args, 1)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_CAPTURE); vm_chunk_emit(ctx->chunk, 2); break; }
        if (strcmp(name, "repeat") == 0 && argc == 2) { compile_expr(ctx, ast_arr_get(args, 0)); compile_expr(ctx, ast_arr_get(args, 1)); vm_chunk_emit(ctx->chunk, OP_STR_REPEAT); break; }
        if (strcmp(name, "slurp") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_CORE_SLURP); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "spew") == 0 && argc == 2) { compile_expr(ctx, ast_arr_get(args, 0)); compile_expr(ctx, ast_arr_get(args, 1)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_CORE_SPEW); vm_chunk_emit(ctx->chunk, 2); break; }
        if (strcmp(name, "hash_new") == 0 && argc == 0) { vm_chunk_emit(ctx->chunk, OP_NEW_HASH); vm_chunk_emit_u32(ctx->chunk, 4); break; }
        if ((strcmp(name, "core::poll") == 0 || strcmp(name, "sys::poll") == 0) && argc == 2
            && resolve_vm_func_idx(ctx, name) < 0) {
            compile_expr(ctx, ast_arr_get(args, 0));
            compile_expr(ctx, ast_arr_get(args, 1));
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_POLL);
            vm_chunk_emit(ctx->chunk, 2);
            break;
        }
        /* core::coro_*: green tasks on the runtime's coroutines; a task's
         * VM code runs in its own worker VM (vm_exported_closure_call) */
        {
            const char *cn0 = name;
            if (strncmp(cn0, "core::", 6) == 0) cn0 += 6;
            else if (strncmp(cn0, "sys::", 5) == 0) cn0 += 5;
            else cn0 = NULL;
            int cop = -1;
            if (cn0 && strncmp(cn0, "coro_", 5) == 0 && resolve_vm_func_idx(ctx, name) < 0) {
                const char *cs = cn0 + 5;
                if (strcmp(cs, "create") == 0) cop = 0;
                else if (strcmp(cs, "resume") == 0) cop = 1;
                else if (strcmp(cs, "yield_io") == 0) cop = 2;
                else if (strcmp(cs, "state") == 0) cop = 3;
                else if (strcmp(cs, "wait_fd") == 0) cop = 4;
                else if (strcmp(cs, "wait_mask") == 0) cop = 5;
                else if (strcmp(cs, "wait_timeout") == 0) cop = 6;
                else if (strcmp(cs, "error") == 0) cop = 7;
                else if (strcmp(cs, "result") == 0) cop = 8;
                else if (strcmp(cs, "free") == 0) cop = 9;
            }
            if (cop >= 0) {
                vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
                vm_chunk_emit_i64(ctx->chunk, cop);
                for (int ci = 0; ci < argc; ci++) compile_expr(ctx, ast_arr_get(args, ci));
                vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                vm_chunk_emit_u16(ctx->chunk, BUILTIN_CORO_OP);
                vm_chunk_emit(ctx->chunk, (uint8_t)(argc + 1));
                break;
            }
        }
        if ((strcmp(name, "core::exception_trace") == 0 || strcmp(name, "sys::exception_trace") == 0) && argc == 0) {
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_EXC_TRACE);
            vm_chunk_emit(ctx->chunk, 0);
            break;
        }
        /* utf8::normalize(s, form) / c::callback(fn, ret, args) /
         * c::callback_free(cb): the runtime functions (a VM closure crosses
         * as an exported closure, so C code can call back into the VM) */
        {
            const char *rn = NULL;
            int ra = 0;
            if (strcmp(name, "utf8::normalize") == 0 && argc == 2) { rn = "strada_utf8_normalize"; ra = 2; }
            else if (strcmp(name, "c::callback") == 0 && argc == 3) { rn = "strada_ffi_callback_new"; ra = 3; }
            else if (strcmp(name, "c::callback_free") == 0 && argc == 1) { rn = "strada_ffi_callback_free"; ra = 1; }
            if (rn) {
                int rgi = vm_generic_builtin_find_min(rn, ra);
                if (rgi >= 0) {
                    for (int ri = 0; ri < argc; ri++) compile_expr(ctx, ast_arr_get(args, ri));
                    vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)(VM_GENERIC_BID_BASE + rgi));
                    vm_chunk_emit(ctx->chunk, (uint8_t)argc);
                    break;
                }
            }
        }
        /* core::gc_* / spew_fh / *_default_capacity */
        {
            const char *cn = name;
            if (strncmp(cn, "core::", 6) == 0) cn += 6;
            else if (strncmp(cn, "sys::", 5) == 0) cn += 5;
            else cn = NULL;
            int gop = -1;
            if (cn) {
                if (strcmp(cn, "gc_collect") == 0) gop = 0;
                else if (strcmp(cn, "gc_enable") == 0) gop = 1;
                else if (strcmp(cn, "gc_disable") == 0) gop = 2;
                else if (strcmp(cn, "gc_threshold") == 0) gop = 3;
                else if (strcmp(cn, "gc_collections") == 0) gop = 4;
                else if (strcmp(cn, "gc_freed") == 0) gop = 5;
            }
            if (gop >= 0) {
                vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
                vm_chunk_emit_i64(ctx->chunk, gop);
                int ga = 1;
                if (gop == 3 && argc >= 1) { compile_expr(ctx, ast_arr_get(args, 0)); ga = 2; }
                vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                vm_chunk_emit_u16(ctx->chunk, BUILTIN_GC_OP);
                vm_chunk_emit(ctx->chunk, (uint8_t)ga);
                break;
            }
            if (cn && (strcmp(cn, "spew_fh") == 0 || strcmp(cn, "spew_fd") == 0) && argc == 2) {
                compile_expr(ctx, ast_arr_get(args, 0));
                compile_expr(ctx, ast_arr_get(args, 1));
                vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
                vm_chunk_emit_i64(ctx->chunk, strcmp(cn, "spew_fd") == 0 ? 1 : 0);
                vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                vm_chunk_emit_u16(ctx->chunk, BUILTIN_SPEW_FH);
                vm_chunk_emit(ctx->chunk, 3);
                break;
            }
            if (cn && (strcmp(cn, "hash_default_capacity") == 0 || strcmp(cn, "array_default_capacity") == 0) && argc <= 1) {
                vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
                vm_chunk_emit_i64(ctx->chunk, cn[0] == 'h' ? 0 : 1);
                if (argc == 1) compile_expr(ctx, ast_arr_get(args, 0));
                vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                vm_chunk_emit_u16(ctx->chunk, BUILTIN_DEFAULT_CAP);
                vm_chunk_emit(ctx->chunk, (uint8_t)(argc + 1));
                break;
            }
        }
        /* printf([FH,] FORMAT, LIST): all arguments flattened into one list;
         * the runtime decides whether the first one is a filehandle */
        if (strcmp(name, "printf") == 0 && resolve_vm_func_idx(ctx, name) < 0) {
            vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
            vm_chunk_emit_u32(ctx->chunk, (uint32_t)(argc > 0 ? argc : 1));
            for (int pi = 0; pi < argc; pi++) {
                StradaValue *pa = ast_arr_get(args, pi);
                vm_chunk_emit(ctx->chunk, OP_DUP);
                compile_list_src(ctx, pa);
                vm_chunk_emit(ctx->chunk, vm_array_flatten_kind(ctx, pa) != 0 ? OP_ARRAY_EXTEND : OP_ARRAY_PUSH);
            }
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_PRINTF);
            vm_chunk_emit(ctx->chunk, 1);
            break;
        }
        /* free(\$x) / core::free / core::release: the variable becomes undef
         * (memory is the VM GC's business); any other argument: no-op */
        if ((strcmp(name, "free") == 0 || strcmp(name, "core::free") == 0 || strcmp(name, "core::release") == 0
             || strcmp(name, "sys::free") == 0 || strcmp(name, "sys::release") == 0) && argc == 1
            && resolve_vm_func_idx(ctx, name) < 0) {
            StradaValue *fa = ast_deref(ast_arr_get(args, 0));
            StradaValue *ftgt = (fa && !STRADA_IS_TAGGED_INT(fa) && (int)ast_int(fa, "type") == NI_REF) ? ast_get(fa, "target") : NULL;
            if (ftgt && (int)ast_int(ftgt, "type") == NI_VARIABLE && ast_str(ftgt, "sigil")[0] == '$') {
                VMLValueTarget flv = compile_lvalue_prepare(ctx, ftgt);
                if (flv.kind != LV_NONE) {
                    int fslot = ctx_add_local(ctx, "__free_v", '$');
                    vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
                    emit_reset_store_local(ctx, fslot);
                    compile_lvalue_store_slot(ctx, &flv, fslot, 0);
                    compile_lvalue_finish(ctx, &flv);
                    vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
                    break;
                }
            }
            /* release($sref): the referenced variable becomes undef (a
             * non-reference is left alone) */
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
            vm_chunk_emit(ctx->chunk, OP_STORE_SREF);
            vm_chunk_emit(ctx->chunk, OP_POP);
            vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
            break;
        }
        if ((strcmp(name, "core::getaddrinfo") == 0 || strcmp(name, "sys::getaddrinfo") == 0) && (argc == 1 || argc == 2)) {
            int gai = vm_generic_builtin_find("strada_getaddrinfo_first", 2);
            if (gai >= 0) {
                compile_expr(ctx, ast_arr_get(args, 0));
                if (argc == 2) compile_expr(ctx, ast_arr_get(args, 1));
                else vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
                vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)(VM_GENERIC_BID_BASE + gai));
                vm_chunk_emit(ctx->chunk, 2);
                break;
            }
        }
        /* Legacy bare builtins (as in the compiled backend) */
        if ((strcmp(name, "hash_get") == 0 || strcmp(name, "core::hash_get") == 0) && argc == 2 && resolve_vm_func_idx(ctx, name) < 0) {
            compile_expr(ctx, ast_arr_get(args, 0));
            compile_expr(ctx, ast_arr_get(args, 1));
            vm_chunk_emit(ctx->chunk, OP_HASH_GET);
            break;
        }
        if ((strcmp(name, "hash_set") == 0 || strcmp(name, "core::hash_set") == 0) && argc == 3 && resolve_vm_func_idx(ctx, name) < 0) {
            compile_expr(ctx, ast_arr_get(args, 0));
            compile_expr(ctx, ast_arr_get(args, 1));
            compile_expr(ctx, ast_arr_get(args, 2));
            vm_chunk_emit(ctx->chunk, OP_HASH_SET);
            vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
            break;
        }
        if (strcmp(name, "readline") == 0 && argc <= 2 && resolve_vm_func_idx(ctx, name) < 0) {
            if (argc == 0) {
                /* readline() reads STDIN, like <STDIN> */
                const char *gkey = resolve_global_var_key(ctx, "STDIN");
                if (gkey) {
                    size_t key_idx = vm_chunk_add_str_const(ctx->chunk, gkey);
                    vm_chunk_emit(ctx->chunk, OP_LOAD_GLOBAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)key_idx);
                } else {
                    vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
                }
            } else {
                compile_expr(ctx, ast_arr_get(args, 0));
            }
            if (argc == 2) compile_expr(ctx, ast_arr_get(args, 1));   /* byte limit */
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_CORE_READLINE);
            vm_chunk_emit(ctx->chunk, (uint8_t)(argc == 2 ? 2 : 1));
            break;
        }
        if ((strcmp(name, "set_package") == 0 || strcmp(name, "core::set_package") == 0) && argc == 1 && resolve_vm_func_idx(ctx, name) < 0) {
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_SET_PACKAGE);
            vm_chunk_emit(ctx->chunk, 1);
            break;
        }
        if (strcmp(name, "inherit") == 0 && argc == 1) {
            /* inherit(PARENT): the package set by set_package() inherits */
            compile_expr(ctx, ast_arr_get(args, 0));
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_INHERIT);
            vm_chunk_emit(ctx->chunk, 1);
            break;
        }
        if (strcmp(name, "inherit") == 0 && argc == 2) { compile_expr(ctx, ast_arr_get(args, 0)); compile_expr(ctx, ast_arr_get(args, 1)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_INHERIT); vm_chunk_emit(ctx->chunk, 2); break; }
        if (strcmp(name, "blessed") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_BLESSED); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "refcount") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_REFCOUNT); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "typeof") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_TYPEOF); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "refto") == 0 && argc == 1) { compile_live_scalar_ref(ctx, ast_arr_get(args, 0)); break; }
        if (strcmp(name, "reftype") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_REFTYPE); vm_chunk_emit(ctx->chunk, 1); break; }
        if ((strcmp(name, "deref") == 0 || strcmp(name, "derefto") == 0) && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_DEREF); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "is_ref") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_IS_REF); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "is_refto") == 0 && (argc == 1 || argc == 2)) { for (int i = 0; i < argc; i++) compile_expr(ctx, ast_arr_get(args, i)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_IS_REFTO); vm_chunk_emit(ctx->chunk, (uint8_t)argc); break; }
        if (strcmp(name, "dumper_str") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_DUMPER_STR); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "nsort") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_NSORT); vm_chunk_emit(ctx->chunk, 1); break; }
        if ((strcmp(name, "cast_int") == 0 || strcmp(name, "int") == 0) && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_CAST_INT); break; }
        if (strcmp(name, "cast_num") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_CAST_NUM); break; }
        if (strcmp(name, "cast_str") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_CAST_STR); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "byte_index") == 0 && (argc == 2 || argc == 3)) { for (int i = 0; i < argc; i++) compile_expr(ctx, ast_arr_get(args, i)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_BYTE_INDEX); vm_chunk_emit(ctx->chunk, (uint8_t)argc); break; }
        if (strcmp(name, "substr_bytes") == 0 && (argc == 2 || argc == 3)) { for (int i = 0; i < argc; i++) compile_expr(ctx, ast_arr_get(args, i)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_SUBSTR_BYTES); vm_chunk_emit(ctx->chunk, (uint8_t)argc); break; }
        if (strcmp(name, "byte_substr") == 0 && (argc == 2 || argc == 3)) { for (int i = 0; i < argc; i++) compile_expr(ctx, ast_arr_get(args, i)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_CORE_BYTE_SUBSTR); vm_chunk_emit(ctx->chunk, (uint8_t)argc); break; }
        if (strcmp(name, "byte_at") == 0 && argc == 2) { for (int i = 0; i < 2; i++) compile_expr(ctx, ast_arr_get(args, i)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_BYTE_AT); vm_chunk_emit(ctx->chunk, 2); break; }
        if (strcmp(name, "clone") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_CLONE); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "deref_set") == 0 && argc == 2) { compile_expr(ctx, ast_arr_get(args, 0)); compile_expr(ctx, ast_arr_get(args, 1)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_DEREF_SET); vm_chunk_emit(ctx->chunk, 2); break; }
        if (strcmp(name, "deref_array") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_DEREF_ARRAY); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "deref_hash") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_DEREF_HASH); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "dump") == 0 && (argc == 1 || argc == 2)) { for (int i = 0; i < argc; i++) compile_expr(ctx, ast_arr_get(args, i)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_DUMP); vm_chunk_emit(ctx->chunk, (uint8_t)argc); break; }
        if (strcmp(name, "dumper") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_DUMPER_STR); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "reserve") == 0 && argc == 2) { compile_expr(ctx, ast_arr_get(args, 0)); compile_expr(ctx, ast_arr_get(args, 1)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_RESERVE); vm_chunk_emit(ctx->chunk, 2); break; }
        if (strcmp(name, "unlink") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_CORE_UNLINK); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "exit") == 0 && argc <= 1) { for (int i = 0; i < argc; i++) compile_expr(ctx, ast_arr_get(args, i)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_CORE_EXIT); vm_chunk_emit(ctx->chunk, (uint8_t)argc); break; }
        if (strcmp(name, "getpid") == 0 && argc == 0) { vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_CORE_GETPID); vm_chunk_emit(ctx->chunk, 0); break; }
        if (strcmp(name, "write_fd") == 0 && argc == 2) { compile_expr(ctx, ast_arr_get(args, 0)); compile_expr(ctx, ast_arr_get(args, 1)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_CORE_WRITE_FD); vm_chunk_emit(ctx->chunk, 2); break; }
        if (strcmp(name, "close_fd") == 0 && argc == 1) { compile_expr(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_CORE_CLOSE_FD); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "stacktrace") == 0 && argc == 0) { vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_STACKTRACE); vm_chunk_emit(ctx->chunk, 0); break; }
        if ((strcmp(name, "stacktrace_str") == 0 || strcmp(name, "stack_trace") == 0) && argc == 0) { vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_CORE_STACK_TRACE); vm_chunk_emit(ctx->chunk, 0); break; }
        if (strcmp(name, "caller") == 0 && (argc == 0 || argc == 1)) { for (int i = 0; i < argc; i++) compile_expr(ctx, ast_arr_get(args, i)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_CORE_CALLER); vm_chunk_emit(ctx->chunk, (uint8_t)argc); break; }
        if (strcmp(name, "tie") == 0 && argc >= 2) { compile_tie_target(ctx, ast_arr_get(args, 0)); for (int i = 1; i < argc; i++) compile_expr(ctx, ast_arr_get(args, i)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_TIE); vm_chunk_emit(ctx->chunk, (uint8_t)argc); break; }
        if (strcmp(name, "untie") == 0 && argc == 1) { compile_tie_target(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_UNTIE); vm_chunk_emit(ctx->chunk, 1); break; }
        if (strcmp(name, "tied") == 0 && argc == 1) { compile_tie_target(ctx, ast_arr_get(args, 0)); vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_TIED); vm_chunk_emit(ctx->chunk, 1); break; }

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
        /* Bare runtime function names (strada_new_undef(), ...) that older
         * programs call directly: the compiled backend emits a C call. */
        if (strncmp(name, "strada_", 7) == 0 && resolve_vm_func_idx(ctx, name) < 0) {
            int bidx = vm_generic_builtin_find(name, argc);
            if (bidx >= 0) {
                for (int i = 0; i < argc; i++) compile_expr(ctx, ast_arr_get(args, i));
                vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)(VM_GENERIC_BID_BASE + bidx));
                vm_chunk_emit(ctx->chunk, (uint8_t)argc);
                break;
            }
        }
        if (strchr(name, ':')) {
            char gbuf[256];
            const char *gname = name;
            if (strncmp(name, "core::", 6) == 0) {
                snprintf(gbuf, sizeof(gbuf), "sys::%s", name + 6);
                gname = gbuf;
            }
            int gidx = vm_generic_builtin_find(gname, argc);
            if (gidx < 0 && strncmp(gname, "sys::", 5) == 0) {
                /* sys::NAME -> the runtime's strada_NAME (uniform prototype) */
                char rbuf[256];
                snprintf(rbuf, sizeof(rbuf), "strada_%s", gname + 5);
                gidx = vm_generic_builtin_find_min(rbuf, argc);
                if (gidx < 0) gidx = vm_generic_builtin_find_min(gname, argc);
            }
            if (gidx < 0 && argc == 0 && strcmp(gname, "sys::srandom") == 0) {
                gidx = vm_generic_builtin_find("sys::srandom", 1);
                if (gidx >= 0) {
                    vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
                    vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)(VM_GENERIC_BID_BASE + gidx));
                    vm_chunk_emit(ctx->chunk, 1);
                    break;
                }
            }
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

        if (argc == 0) {
            int64_t eval = 0;
            if (lookup_enum_for_ctx(ctx, name, &eval)) {
                vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
                vm_chunk_emit_i64(ctx->chunk, eval);
                break;
            }
        }

        /* User function call */
        char normalized[256];
        int fidx = resolve_func_idx_for_ctx(ctx, name, normalized, sizeof(normalized));
        if (fidx < 0) {
            /* Check for native (import_lib) functions */
            VMNativeEntry *native = vm_find_native(ctx->prog, name);
            if (!native && normalized[0]) {
                native = vm_find_native(ctx->prog, normalized);
            }
            if (native) {
                /* Emit native call */
                int native_idx = (int)(native - ctx->prog->natives);
                for (int i = 0; i < argc; i++) {
                    compile_expr(ctx, ast_arr_get(args, i));
                }
                emit_call_context(ctx, expr_call_ctx);
                vm_chunk_emit(ctx->chunk, OP_CALL_NATIVE);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)native_idx);
                vm_chunk_emit(ctx->chunk, (uint8_t)argc);
                break;
            }
            /* Pkg::new(k => v, ...) on a package without a constructor: a
             * hash of the pairs blessed into Pkg (compiled: inline hash
             * construction / strada_generic_new) */
            {
                size_t nl = name ? strlen(name) : 0;
                if (nl > 5 && strcmp(name + nl - 5, "::new") == 0) {
                    char gpkg[256];
                    size_t pl = nl - 5;
                    if (pl < sizeof(gpkg)) {
                        memcpy(gpkg, name, pl);
                        gpkg[pl] = 0;
                        compile_flat_arg_list(ctx, args, argc);
                        vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                        vm_chunk_emit_u16(ctx->chunk, BUILTIN_HASH_SHALLOW_COPY);
                        vm_chunk_emit(ctx->chunk, 1);
                        size_t pidx = vm_chunk_add_str_const(ctx->chunk, gpkg);
                        vm_chunk_emit(ctx->chunk, OP_PUSH_STR);
                        vm_chunk_emit_u16(ctx->chunk, (uint16_t)pidx);
                        vm_chunk_emit(ctx->chunk, OP_HASH_BLESS);
                        break;
                    }
                }
            }
            char umsg[320];
            snprintf(umsg, sizeof(umsg), "Undefined function: %s", name ? name : "");
            for (int i = 0; i < argc; i++) {
                compile_expr(ctx, ast_arr_get(args, i));
                vm_chunk_emit(ctx->chunk, OP_POP);
            }
            size_t midx = vm_chunk_add_str_const(ctx->chunk, umsg);
            vm_chunk_emit(ctx->chunk, OP_PUSH_STR);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)midx);
            vm_chunk_emit(ctx->chunk, OP_THROW);
            vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
            break;
        }

        VMChunk *callee = &ctx->prog->funcs[fidx];
        if (callee->is_async) {
            /* calling an async func returns a future: the call runs on the
             * runtime pool (BUILTIN_ASYNC_OP 8: closure + argument list) */
            vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
            vm_chunk_emit_i64(ctx->chunk, 8);
            vm_chunk_emit(ctx->chunk, OP_MAKE_CLOSURE);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)fidx);
            vm_chunk_emit(ctx->chunk, 0);
            vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
            vm_chunk_emit_u32(ctx->chunk, (uint32_t)(argc > 0 ? argc : 1));
            for (int i = 0; i < argc; i++) {
                StradaValue *arg = ast_deref(ast_arr_get(args, i));
                vm_chunk_emit(ctx->chunk, OP_DUP);
                if (arg && (int)ast_int(arg, "type") == NI_SPREAD) {
                    compile_list_src(ctx, ast_get(arg, "target"));
                    vm_chunk_emit(ctx->chunk, OP_ARRAY_EXTEND);
                } else {
                    compile_expr(ctx, arg);
                    vm_chunk_emit(ctx->chunk, OP_ARRAY_PUSH);
                }
            }
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_ASYNC_OP);
            vm_chunk_emit(ctx->chunk, 3);
            break;
        }
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
                    compile_list_src(ctx, ast_get(arg, "target"));
                    vm_chunk_emit(ctx->chunk, OP_ARRAY_EXTEND);
                } else if (callee->has_variadic && i >= callee->fixed_param_count &&
                           vm_array_flatten_kind(ctx, arg) >= 1) {
                    compile_list_src(ctx, arg);
                    vm_chunk_emit(ctx->chunk, OP_ARRAY_EXTEND);
                } else {
                    compile_expr(ctx, arg);
                    vm_chunk_emit(ctx->chunk, OP_ARRAY_PUSH);
                }
            }
            emit_call_context(ctx, expr_call_ctx);
            vm_chunk_emit(ctx->chunk, OP_CALL_SPREAD);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)fidx);
            break;
        }

        for (int i = 0; i < argc; i++) {
            compile_expr(ctx, ast_arr_get(args, i));
        }

        emit_call_context(ctx, expr_call_ctx);
        vm_chunk_emit(ctx->chunk, OP_CALL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)fidx);
        vm_chunk_emit(ctx->chunk, (uint8_t)argc);
        ctx->tail_call_end = ctx->chunk->code_len;
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
            int has_spread = (argc - 1 >= 255) ? 1 : 0;
            for (int i = 1; i < argc && !has_spread; i++) {
                StradaValue *sarg = ast_deref(ast_arr_get(args, i));
                if (sarg && (int)ast_int(sarg, "type") == NI_SPREAD) has_spread = 1;
            }
            if (has_spread) {
                compile_expr(ctx, ast_arr_get(args, 0));
                vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
                vm_chunk_emit_u32(ctx->chunk, argc > 1 ? (uint32_t)(argc - 1) * 2 : 8);
                for (int i = 1; i < argc; i++) {
                    StradaValue *sarg = ast_deref(ast_arr_get(args, i));
                    int st = sarg ? (int)ast_int(sarg, "type") : 0;
                    vm_chunk_emit(ctx->chunk, OP_DUP);
                    if (st == NI_SPREAD) {
                        compile_list_src(ctx, ast_get(sarg, "target"));
                        vm_chunk_emit(ctx->chunk, OP_ARRAY_EXTEND);
                    } else {
                        compile_expr(ctx, sarg);
                        vm_chunk_emit(ctx->chunk, OP_ARRAY_PUSH);
                    }
                }
                call_argc = 255;
            } else {
                for (int i = 0; i < argc; i++) {
                    compile_expr(ctx, ast_arr_get(args, i));
                }
                call_argc = argc - 1;
            }
        }

        size_t pkg_idx = vm_chunk_add_str_const(ctx->chunk, def_pkg);
        size_t midx = vm_chunk_add_str_const(ctx->chunk, method);
        emit_call_context(ctx, expr_call_ctx);
        vm_chunk_emit(ctx->chunk, OP_SUPER_CALL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)pkg_idx);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)midx);
        vm_chunk_emit(ctx->chunk, (uint8_t)call_argc);
        break;
    }

    case NI_ASSIGN: {
        StradaValue *target = ast_get(node, "target");
        StradaValue *value = ast_get(node, "value");
        /* $struct->field = VALUE (paren-less field on a C-struct value,
         * which the VM keeps as a hash) */
        {
            StradaValue *fa_t = ast_deref(ast_get(node, "target"));
            const char *fa_op = ast_str(node, "op");
            int fa_cop = (fa_op && strcmp(fa_op, "=") != 0) ? compound_op_to_opcode(fa_op) : -1;
            if (fa_t && !STRADA_IS_TAGGED_INT(fa_t) && (int)ast_int(fa_t, "type") == NI_FIELD_ACCESS &&
                fa_op && (strcmp(fa_op, "=") == 0 || fa_cop >= 0)) {
                const char *ffield = ast_str(fa_t, "field");
                size_t fk = vm_chunk_add_str_const(ctx->chunk, ffield ? ffield : "");
                int fv_slot = ctx_add_local(ctx, "__fa_v", '$');
                int fo_slot = ctx_add_local(ctx, "__fa_o", '$');
                compile_expr(ctx, ast_get(fa_t, "object"));
                emit_reset_store_local(ctx, fo_slot);
                if (fa_cop >= 0) {
                    /* $s->f OP= v: read, combine, store */
                    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)fo_slot);
                    vm_chunk_emit(ctx->chunk, OP_PUSH_STR);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)fk);
                    vm_chunk_emit(ctx->chunk, OP_HASH_GET);
                    compile_expr(ctx, value);
                    vm_chunk_emit(ctx->chunk, (uint8_t)fa_cop);
                } else {
                    compile_expr(ctx, value);
                }
                int fkind = vm_struct_field_kind(ctx, ast_get(fa_t, "object"), ffield);
                if (fkind > 0) {
                    vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
                    vm_chunk_emit_i64(ctx->chunk, fkind);
                    vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                    vm_chunk_emit_u16(ctx->chunk, BUILTIN_STRUCT_FIELD_STORE);
                    vm_chunk_emit(ctx->chunk, 2);
                }
                emit_reset_store_local(ctx, fv_slot);
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)fo_slot);
                vm_chunk_emit(ctx->chunk, OP_PUSH_STR);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)fk);
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)fv_slot);
                vm_chunk_emit(ctx->chunk, OP_HASH_SET);
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)fv_slot);
                break;
            }
        }
        const char *op = ast_str(node, "op");
        target = ast_deref(target);

        int target_type = target ? (int)ast_int(target, "type") : 0;

        /* Lvalue substr: substr(TARGET, OFF[, LEN]) = VALUE */
        if (target_type == NI_CALL && strcmp(op, "=") == 0 && strcmp(ast_str(target, "name"), "substr") == 0) {
            StradaValue *sargs = ast_get(target, "args");
            int sargc = (int)ast_int(target, "arg_count");
            if (sargc == 2 || sargc == 3) {
                compile_substr_replace(ctx, sargs, sargc, value, 0);
                break;
            }
        }

        /* Assignment to array or hash slice (@a[i, j] = ..., @h{k1, k2} = ...) */
        if ((target_type == NI_ARRAY_SLICE || target_type == NI_HASH_SLICE) && strcmp(op, "=") == 0) {
            int is_hash_slice = (target_type == NI_HASH_SLICE);
            StradaValue *slice_src = ast_get(target, "source");
            StradaValue *items = ast_get(target, "items");
            int count = (int)ast_int(target, "item_count");
            int d = ctx->assign_depth++;
            char rbuf[32], sbuf[32], jbuf[32];
            snprintf(rbuf, sizeof(rbuf), "__asg_sr%d", d);
            snprintf(sbuf, sizeof(sbuf), "__asg_ss%d", d);
            snprintf(jbuf, sizeof(jbuf), "__asg_sj%d", d);
            int rhs_slot = ctx_get_or_add_local(ctx, rbuf, '@');
            int src_slot = ctx_get_or_add_local(ctx, sbuf, '$');
            int j_slot = ctx_get_or_add_local(ctx, jbuf, '$');

            vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
            vm_chunk_emit_u32(ctx->chunk, 8);
            vm_chunk_emit(ctx->chunk, OP_DUP);
            compile_list_src(ctx, value);
            vm_chunk_emit(ctx->chunk, OP_ARRAY_EXTEND);
            emit_reset_store_local(ctx, rhs_slot);

            compile_expr(ctx, slice_src);
            emit_reset_store_local(ctx, src_slot);

            vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
            vm_chunk_emit_i64(ctx->chunk, 0);
            emit_reset_store_local(ctx, j_slot);

            for (int i = 0; i < count; i++) {
                StradaValue *item = ast_arr_get(items, i);
                int itype = (item && !STRADA_IS_TAGGED_INT(item)) ? (int)ast_int(item, "type") : 0;
                if (!is_hash_slice && itype == NI_RANGE) {
                    char ibuf[32], ebuf[32];
                    snprintf(ibuf, sizeof(ibuf), "__asg_si%d", d);
                    snprintf(ebuf, sizeof(ebuf), "__asg_se%d", d);
                    int idx_slot = ctx_get_or_add_local(ctx, ibuf, '$');
                    int end_slot = ctx_get_or_add_local(ctx, ebuf, '$');

                    compile_expr(ctx, ast_get(item, "start"));
                    emit_reset_store_local(ctx, idx_slot);
                    compile_expr(ctx, ast_get(item, "end"));
                    emit_reset_store_local(ctx, end_slot);

                    size_t loop_top = ctx->chunk->code_len;
                    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx_slot);
                    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)end_slot);
                    vm_chunk_emit(ctx->chunk, OP_LE);
                    vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
                    size_t end_patch = emit_patch_u16(ctx);

                    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)src_slot);
                    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx_slot);
                    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)rhs_slot);
                    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)j_slot);
                    vm_chunk_emit(ctx->chunk, OP_ARRAY_GET);
                    vm_chunk_emit(ctx->chunk, OP_ARRAY_SET);

                    vm_chunk_emit(ctx->chunk, OP_INCR);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)j_slot);
                    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx_slot);
                    vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
                    vm_chunk_emit_i64(ctx->chunk, 1);
                    vm_chunk_emit(ctx->chunk, OP_ADD);
                    emit_reset_store_local(ctx, idx_slot);
                    vm_chunk_emit(ctx->chunk, OP_JMP);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)loop_top);

                    patch_u16(ctx, end_patch, (uint16_t)ctx->chunk->code_len);
                } else {
                    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)src_slot);
                    compile_expr(ctx, item);
                    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)rhs_slot);
                    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)j_slot);
                    vm_chunk_emit(ctx->chunk, OP_ARRAY_GET);
                    vm_chunk_emit(ctx->chunk, is_hash_slice ? OP_HASH_SET : OP_ARRAY_SET);

                    vm_chunk_emit(ctx->chunk, OP_INCR);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)j_slot);
                }
            }

            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)j_slot);
            ctx->assign_depth--;
            break;
        }

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
                size_t kidx = 0;
                if (target_type == NI_HASH_ACCESS && is_concat_key(idx_node, &prefix, &suffix)) {
                    compile_expr(ctx, cont_node);
                    compile_expr(ctx, suffix);
                    compile_expr(ctx, value);
                    vm_chunk_emit(ctx->chunk, OP_DUP);
                    emit_reset_store_local(ctx, v_slot);
                    size_t pidx = vm_chunk_add_str_const(ctx->chunk, prefix);
                    vm_chunk_emit(ctx->chunk, OP_HASH_SET_CONCAT);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)pidx);
                } else if (is_hash && try_const_str_key(ctx, idx_node, &kidx)) {
                    compile_expr(ctx, cont_node);
                    compile_expr(ctx, value);
                    vm_chunk_emit(ctx->chunk, OP_DUP);
                    emit_reset_store_local(ctx, v_slot);
                    vm_chunk_emit(ctx->chunk, OP_HASH_SET_CONST);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)kidx);
                } else {
                    compile_expr(ctx, cont_node);
                    compile_expr(ctx, idx_node);
                    compile_expr(ctx, value);
                    vm_chunk_emit(ctx->chunk, OP_DUP);
                    emit_reset_store_local(ctx, v_slot);
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
                emit_reset_store_local(ctx, c_slot);

                compile_expr(ctx, idx_node);
                emit_reset_store_local(ctx, k_slot);

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
                emit_reset_store_local(ctx, v_slot);

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
                emit_reset_store_local(ctx, c_slot);

                compile_expr(ctx, idx_node);
                emit_reset_store_local(ctx, k_slot);

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

                emit_reset_store_local(ctx, v_slot);

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

        /* @{EXPR} = LIST / %{EXPR} = LIST / @$r = ... : write through */
        if (target_type == NI_DEREF_SCALAR && strcmp(op, "=") == 0) {
            const char *dsig = ast_str(target, "sigil");
            if (dsig && (dsig[0] == '@' || dsig[0] == '%')) {
                StradaValue *ref_node = ast_get(target, "ref");
                if (!ref_node) ref_node = ast_get(target, "target");
                compile_expr(ctx, target);
                if (dsig[0] == '@') compile_array_init_expr(ctx, NULL, value, 0);
                else compile_hash_init_expr(ctx, NULL, value, 0);
                vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                vm_chunk_emit_u16(ctx->chunk, BUILTIN_CONTAINER_REPLACE);
                vm_chunk_emit(ctx->chunk, 2);
                (void)ref_node;
                break;
            }
        }

        /* Assignment to scalar ref dereference ($$r = val, $$r += 5, etc.) */
        if (target_type == NI_DEREF_SCALAR) {
            StradaValue *ref_node = ast_get(target, "ref");
            if (!ref_node) ref_node = ast_get(target, "target");
            int d = ctx->assign_depth++;
            char rbuf[32], vbuf[32];
            snprintf(rbuf, sizeof(rbuf), "__ds_r%d", d);
            snprintf(vbuf, sizeof(vbuf), "__ds_v%d", d);
            int r_slot = ctx_get_or_add_local(ctx, rbuf, '$');
            int v_slot = ctx_get_or_add_local(ctx, vbuf, '$');

            compile_expr(ctx, ref_node);
            emit_reset_store_local(ctx, r_slot);

            if (is_shortcircuit_assign(op)) {
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)r_slot);
                vm_chunk_emit(ctx->chunk, OP_DEREF_SCALAR);
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
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)r_slot);
                compile_expr(ctx, value);
                vm_chunk_emit(ctx->chunk, OP_STORE_SREF);
                patch_u16(ctx, skip_sc, (uint16_t)ctx->chunk->code_len);
            } else {
                int cop = compound_op_to_opcode(op);
                if (cop >= 0) {
                    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)r_slot);
                    vm_chunk_emit(ctx->chunk, OP_DEREF_SCALAR);
                    compile_expr(ctx, value);
                    vm_chunk_emit(ctx->chunk, (uint8_t)cop);
                    emit_reset_store_local(ctx, v_slot);
                    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)r_slot);
                    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)v_slot);
                } else {
                    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)r_slot);
                    compile_expr(ctx, value);
                }
                vm_chunk_emit(ctx->chunk, OP_STORE_SREF);
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
                        emit_scalar_type_coerce(ctx, ctx->captures[cap].var_type);
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
                    } else if (strcmp(op, "=") == 0) {
                        compile_assign_var_rhs(ctx, vsigil[0], value);
                    } else {
                        compile_expr(ctx, value);
                    }
                    emit_scalar_type_coerce(ctx, ctx->captures[cap].var_type);
                    vm_chunk_emit(ctx->chunk, OP_DUP);
                    vm_chunk_emit(ctx->chunk, OP_STORE_CAPTURE);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)cap);
                    break;
                }
                /* Try our variable */
                const char *gkey = resolve_global_var_key(ctx, vname);
                if (gkey) {
                    size_t key_idx = vm_chunk_add_str_const(ctx->chunk, gkey);
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
                    } else if (strcmp(op, "=") == 0) {
                        compile_assign_var_rhs(ctx, vsigil[0], value);
                    } else {
                        compile_expr(ctx, value);
                    }
                    vm_chunk_emit(ctx->chunk, OP_DUP);
                    vm_chunk_emit(ctx->chunk, OP_STORE_GLOBAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)key_idx);
                    break;
                }
                vm_report_undeclared_var(ctx, target, vsigil, vname);
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
                emit_scalar_type_coerce(ctx, ctx->local_types[slot]);
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
                    size_t slen = 0;
                    const char *sval = ast_str_len(val_node, "value", &slen);
                    size_t si = vm_chunk_add_str_const_len(ctx->chunk, sval, slen);
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
                if (strcmp(op, "=") == 0 && (vsigil[0] == '@' || vsigil[0] == '%')) {
                    /* @a = LIST / %h = LIST: a tied target keeps its tie and
                     * gets CLEAR + STORE (BUILTIN_TIE_ASSIGN yields what to
                     * store back: the target itself when tied). */
                    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                    compile_assign_var_rhs(ctx, vsigil[0], value);
                    vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                    vm_chunk_emit_u16(ctx->chunk, BUILTIN_TIE_ASSIGN);
                    vm_chunk_emit(ctx->chunk, 2);
                } else if (strcmp(op, "=") == 0) {
                    compile_assign_var_rhs(ctx, vsigil[0], value);
                } else if (cop >= 0) {
                    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                    compile_expr(ctx, value);
                    vm_chunk_emit(ctx->chunk, (uint8_t)cop);
                } else if (!(strcmp(op, "=") == 0 && ctx->local_types[slot] == 1 && vm_try_char_code(ctx, value))) {
                    compile_expr(ctx, value);
                }
            }

            if (ctx->local_types[slot] != 0 &&
                (strcmp(op, "=") != 0 || infer_expr_scalar_type(value) != ctx->local_types[slot])) {
                emit_scalar_type_coerce(ctx, ctx->local_types[slot]);
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
        StradaValue *operand = ast_deref(ast_get(node, "operand"));
        int is_pre = (int)ast_int(node, "is_prefix");
        const char *inc_op = ast_str(node, "op");
        int is_inc = (strcmp(inc_op, "++") == 0);
        int otype = (operand && !STRADA_IS_TAGGED_INT(operand)) ? (int)ast_int(operand, "type") : 0;

        StradaValue *cont = NULL, *idx = NULL;
        int is_hash = 0;

        if (otype == NI_VARIABLE) {
            const char *vname = ast_str(operand, "name");
            const char *vsigil = ast_str(operand, "sigil");
            char sc = (vsigil && vsigil[0]) ? vsigil[0] : '$';
            int slot = ctx_find_local_sigil(ctx, vname, sc);
            if (slot >= 0) {
                if (is_pre) {
                    vm_chunk_emit(ctx->chunk, is_inc ? OP_INCR : OP_DECR);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                } else {
                    vm_chunk_emit(ctx->chunk, is_inc ? OP_POST_INCR : OP_POST_DECR);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                }
            } else {
                int cap = ctx_resolve_capture(ctx, vname, sc);
                const char *gkey = (cap < 0) ? resolve_global_var_key(ctx, vname) : NULL;
                if (cap >= 0 || gkey != NULL) {
                    uint8_t load_op = (cap >= 0) ? OP_LOAD_CAPTURE : OP_LOAD_GLOBAL;
                    uint8_t store_op = (cap >= 0) ? OP_STORE_CAPTURE : OP_STORE_GLOBAL;
                    uint16_t target_idx = (cap >= 0) ? (uint16_t)cap : (uint16_t)vm_chunk_add_str_const(ctx->chunk, gkey);
                    if (is_pre) {
                        vm_chunk_emit(ctx->chunk, load_op);
                        vm_chunk_emit_u16(ctx->chunk, target_idx);
                        vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
                        vm_chunk_emit_i64(ctx->chunk, is_inc ? 1 : -1);
                        vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                        vm_chunk_emit_u16(ctx->chunk, BUILTIN_INCR_VALUE);
                        vm_chunk_emit(ctx->chunk, 2);
                        vm_chunk_emit(ctx->chunk, OP_DUP);
                        vm_chunk_emit(ctx->chunk, store_op);
                        vm_chunk_emit_u16(ctx->chunk, target_idx);
                    } else {
                        vm_chunk_emit(ctx->chunk, load_op);
                        vm_chunk_emit_u16(ctx->chunk, target_idx);
                        vm_chunk_emit(ctx->chunk, OP_DUP);
                        vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
                        vm_chunk_emit_i64(ctx->chunk, is_inc ? 1 : -1);
                        vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                        vm_chunk_emit_u16(ctx->chunk, BUILTIN_INCR_VALUE);
                        vm_chunk_emit(ctx->chunk, 2);
                        vm_chunk_emit(ctx->chunk, store_op);
                        vm_chunk_emit_u16(ctx->chunk, target_idx);
                    }
                } else {
                    vm_report_undeclared_var(ctx, operand, vsigil, vname);
                    vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
                }
            }
        } else if (extract_cont_idx(operand, &is_hash, &cont, &idx)) {
            int d = ctx->assign_depth++;
            char cbuf[32], kbuf[32], obuf[32], nbuf[32];
            snprintf(cbuf, sizeof(cbuf), "__inc_c%d", d);
            snprintf(kbuf, sizeof(kbuf), "__inc_k%d", d);
            snprintf(obuf, sizeof(obuf), "__inc_o%d", d);
            snprintf(nbuf, sizeof(nbuf), "__inc_n%d", d);
            int c_slot = ctx_get_or_add_local(ctx, cbuf, '$');
            int k_slot = ctx_get_or_add_local(ctx, kbuf, '$');
            int o_slot = ctx_get_or_add_local(ctx, obuf, '$');
            int n_slot = ctx_get_or_add_local(ctx, nbuf, '$');

            compile_expr(ctx, cont);
            emit_reset_store_local(ctx, c_slot);

            compile_expr(ctx, idx);
            emit_reset_store_local(ctx, k_slot);

            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)c_slot);
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)k_slot);
            vm_chunk_emit(ctx->chunk, is_hash ? OP_HASH_GET : OP_ARRAY_GET);
            emit_reset_store_local(ctx, o_slot);

            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)o_slot);
            vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
            vm_chunk_emit_i64(ctx->chunk, is_inc ? 1 : -1);
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_INCR_VALUE);
            vm_chunk_emit(ctx->chunk, 2);
            emit_reset_store_local(ctx, n_slot);

            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)c_slot);
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)k_slot);
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)n_slot);
            vm_chunk_emit(ctx->chunk, is_hash ? OP_HASH_SET : OP_ARRAY_SET);

            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)(is_pre ? n_slot : o_slot));
            ctx->assign_depth--;
        } else if (otype == NI_DEREF_SCALAR) {
            StradaValue *ref_node = ast_get(operand, "ref");
            if (!ref_node) ref_node = ast_get(operand, "target");
            int d = ctx->assign_depth++;
            char rbuf[32], obuf[32], nbuf[32];
            snprintf(rbuf, sizeof(rbuf), "__inc_r%d", d);
            snprintf(obuf, sizeof(obuf), "__inc_o%d", d);
            snprintf(nbuf, sizeof(nbuf), "__inc_n%d", d);
            int r_slot = ctx_get_or_add_local(ctx, rbuf, '$');
            int o_slot = ctx_get_or_add_local(ctx, obuf, '$');
            int n_slot = ctx_get_or_add_local(ctx, nbuf, '$');

            compile_expr(ctx, ref_node);
            emit_reset_store_local(ctx, r_slot);

            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)r_slot);
            vm_chunk_emit(ctx->chunk, OP_DEREF_SCALAR);
            emit_reset_store_local(ctx, o_slot);

            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)o_slot);
            vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
            vm_chunk_emit_i64(ctx->chunk, is_inc ? 1 : -1);
            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_INCR_VALUE);
            vm_chunk_emit(ctx->chunk, 2);
            emit_reset_store_local(ctx, n_slot);

            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)r_slot);
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)n_slot);
            vm_chunk_emit(ctx->chunk, OP_STORE_SREF);
            vm_chunk_emit(ctx->chunk, OP_POP);

            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)(is_pre ? n_slot : o_slot));
            ctx->assign_depth--;
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

        size_t pat_len = 0, repl_len = 0;
        const char *pattern_s = ast_str_len(node, "pattern", &pat_len);
        const char *repl_s = ast_str_len(node, "replacement", &repl_len);
        if (strchr(flags, 'e') && compile_regex_subst_e(ctx, node)) break;

        int is_nondestruct = (strchr(flags, 'r') != NULL);
        VMLValueTarget lv = {0};
        if (!is_nondestruct) lv = compile_lvalue_prepare(ctx, target);
        if (is_nondestruct || lv.kind != LV_NONE) {
            if (!is_nondestruct && lv.kind != LV_NONE) {
                compile_lvalue_load(ctx, &lv);
            } else {
                compile_expr(ctx, target);
            }

            if (!compile_rx_dynamic_pattern(ctx, node, pattern_s, pat_len, flags)) {
                size_t pat_idx = add_pattern_const(ctx, pattern_s, pat_len, flags);
                vm_chunk_emit(ctx->chunk, OP_PUSH_STR);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)pat_idx);
            }

            /* the replacement interpolates $var / ${var} / $h{k} / $a[i]
             * like the pattern (the compiled backend inserts the value
             * raw too); $1.. stay for the runtime */
            if (!compile_rx_dynamic_pattern(ctx, node, repl_s, repl_len, NULL)) {
                size_t repl_idx = vm_chunk_add_str_const_len(ctx->chunk, repl_s, repl_len);
                vm_chunk_emit(ctx->chunk, OP_PUSH_STR);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)repl_idx);
            }

            size_t flags_idx = vm_chunk_add_str_const(ctx->chunk, flags);
            vm_chunk_emit(ctx->chunk, OP_PUSH_STR);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)flags_idx);

            vm_chunk_emit(ctx->chunk, OP_BUILTIN);
            vm_chunk_emit_u16(ctx->chunk, BUILTIN_REGEX_SUBST);
            vm_chunk_emit(ctx->chunk, 4);

            int pair_slot = ctx_add_local(ctx, "__rs_p", '$');
            vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)pair_slot);

            if (is_nondestruct) {
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)pair_slot);
                vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
                vm_chunk_emit_i64(ctx->chunk, 0);
                vm_chunk_emit(ctx->chunk, OP_ARRAY_GET);
            } else {
                int val_slot = ctx_add_local(ctx, "__rs_v", '$');
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)pair_slot);
                vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
                vm_chunk_emit_i64(ctx->chunk, 0);
                vm_chunk_emit(ctx->chunk, OP_ARRAY_GET);
                vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)val_slot);
                compile_lvalue_store_slot(ctx, &lv, val_slot, 0);
                compile_lvalue_finish(ctx, &lv);
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)pair_slot);
                vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
                vm_chunk_emit_i64(ctx->chunk, 1);
                vm_chunk_emit(ctx->chunk, OP_ARRAY_GET);
            }
            break;
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
            if (idx_type == NI_RANGE || vm_array_flatten_kind(ctx, idx_expr) != 0) {
                /* @arr[A..B] / @arr[@idx]: an index array */
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

        int hs_flat = 0;
        for (int i = 0; i < count; i++)
            if (vm_array_flatten_kind(ctx, ast_arr_get(keys, i)) != 0) hs_flat = 1;
        if (hs_flat) {
            /* @h{@k} / @h{LIST}: build the key list, then fetch each */
            int kslot = ctx_add_local(ctx, "__hslice_keys", '@');
            int islot = ctx_add_local(ctx, "__hslice_i", '$');
            vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
            vm_chunk_emit_u32(ctx->chunk, (uint32_t)(count > 0 ? count : 4));
            for (int i = 0; i < count; i++) {
                StradaValue *ke = ast_arr_get(keys, i);
                vm_chunk_emit(ctx->chunk, OP_DUP);
                compile_list_src(ctx, ke);
                vm_chunk_emit(ctx->chunk, vm_array_flatten_kind(ctx, ke) != 0 ? OP_ARRAY_EXTEND : OP_ARRAY_PUSH);
            }
            emit_reset_store_local(ctx, kslot);
            vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
            vm_chunk_emit_i64(ctx->chunk, 0);
            emit_reset_store_local(ctx, islot);
            size_t hs_top = ctx->chunk->code_len;
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL); vm_chunk_emit_u16(ctx->chunk, (uint16_t)islot);
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL); vm_chunk_emit_u16(ctx->chunk, (uint16_t)kslot);
            vm_chunk_emit(ctx->chunk, OP_ARRAY_SIZE);
            vm_chunk_emit(ctx->chunk, OP_LT);
            vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
            size_t hs_exit = emit_patch_u16(ctx);
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL); vm_chunk_emit_u16(ctx->chunk, (uint16_t)hres_slot);
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL); vm_chunk_emit_u16(ctx->chunk, (uint16_t)hsrc_slot);
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL); vm_chunk_emit_u16(ctx->chunk, (uint16_t)kslot);
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL); vm_chunk_emit_u16(ctx->chunk, (uint16_t)islot);
            vm_chunk_emit(ctx->chunk, OP_ARRAY_GET);
            vm_chunk_emit(ctx->chunk, OP_HASH_GET);
            vm_chunk_emit(ctx->chunk, OP_ARRAY_PUSH);
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL); vm_chunk_emit_u16(ctx->chunk, (uint16_t)islot);
            vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
            vm_chunk_emit_i64(ctx->chunk, 1);
            vm_chunk_emit(ctx->chunk, OP_ADD);
            emit_reset_store_local(ctx, islot);
            vm_chunk_emit(ctx->chunk, OP_JMP);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)hs_top);
            patch_u16(ctx, hs_exit, (uint16_t)ctx->chunk->code_len);
        }
        for (int i = 0; !hs_flat && i < count; i++) {
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

    case NI_DO_EXPR: {
        int saved_lc = ctx->local_count;
        int saved_lr = ctx->local_restore_count;
        StradaValue *block = ast_deref(ast_get(node, "block"));
        ctx_push_scope(ctx, (void *)block, saved_lc);
        StradaValue *stmts = block ? ast_get(block, "statements") : NULL;
        int count = block ? (int)ast_int(block, "statement_count") : 0;
        if (count <= 0) {
            vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
        } else {
            for (int i = 0; i < count - 1; i++) {
                compile_stmt(ctx, ast_arr_get(stmts, i));
            }
            StradaValue *last = ast_deref(ast_arr_get(stmts, count - 1));
            int last_type = (last && !STRADA_IS_TAGGED_INT(last)) ? (int)ast_int(last, "type") : 0;
            if (last_type == NI_EXPR_STMT) {
                compile_expr(ctx, ast_get(last, "expr"));
            } else {
                compile_stmt(ctx, last);
                vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
            }
        }
        ctx_pop_scope(ctx);
        while (ctx->local_restore_count > saved_lr) {
            ctx->local_restore_count--;
            char *lr_name = ctx->local_restores[ctx->local_restore_count];
            ctx->local_restores[ctx->local_restore_count] = NULL;
            emit_local_restore(ctx->chunk, lr_name);
            free(lr_name);
        }
        emit_clear_locals_range(ctx->chunk, saved_lc, ctx->local_count);
        ctx->local_count = saved_lc;
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
static void compile_block_expr(CompCtx *ctx, StradaValue *body, int fallback_slot);

/* Shape of a map block's value: 1 = a single reference ([..], {..}, \X),
 * 2 = a hash (%h / %$r: its pairs), 0 = decided at run time. */
static int vm_map_value_kind(StradaValue *body) {
    StradaValue *b = ast_deref(body);
    if (!b || STRADA_IS_TAGGED_INT(b)) return 0;
    int n = (int)ast_int(b, "statement_count");
    if (n <= 0) return 0;
    StradaValue *ls = ast_deref(ast_arr_get(ast_get(b, "statements"), n - 1));
    if (!ls || STRADA_IS_TAGGED_INT(ls) || (int)ast_int(ls, "type") != NI_EXPR_STMT) return 0;
    StradaValue *e = ast_deref(ast_get(ls, "expr"));
    if (!e || STRADA_IS_TAGGED_INT(e)) return 0;
    int t = (int)ast_int(e, "type");
    if (t == NI_ANON_ARRAY && (int)ast_int(e, "is_list") == 0 && (int)ast_int(e, "is_pair") == 0) return 1;
    if (t == NI_ANON_HASH && (int)ast_int(e, "is_empty_list") == 0) return 1;
    if (t == NI_REF) return 1;
    if ((t == NI_VARIABLE || t == NI_DEREF_SCALAR) && ast_str(e, "sigil")[0] == '%') return 2;
    return 0;
}

static void compile_block_expr_inner(CompCtx *ctx, StradaValue *body, int fallback_slot) {
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
            size_t *end_patches = malloc((size_t)(elsif_count + 2) * sizeof(size_t));
            int end_patch_count = 0;

            vm_chunk_emit(ctx->chunk, OP_JMP);
            end_patches[end_patch_count++] = emit_patch_u16(ctx);
            patch_u16(ctx, else_patch, (uint16_t)ctx->chunk->code_len);

            for (int i = 0; i < elsif_count; i++) {
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
            free(end_patches);
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

static void compile_block_expr(CompCtx *ctx, StradaValue *body, int fallback_slot) {
    int saved_lrc = ctx->local_restore_count;
    int saved_lc = ctx->local_count;
    StradaValue *db = body ? ast_deref(body) : NULL;
    int pushed = 0;
    if (db && !STRADA_IS_TAGGED_INT(db)) {
        ctx_push_scope(ctx, (void *)db, saved_lc);
        pushed = 1;
    }
    compile_block_expr_inner(ctx, body, fallback_slot);
    if (pushed) ctx_pop_scope(ctx);
    while (ctx->local_restore_count > saved_lrc) {
        ctx->local_restore_count--;
        emit_local_restore(ctx->chunk, ctx->local_restores[ctx->local_restore_count]);
        free(ctx->local_restores[ctx->local_restore_count]);
        ctx->local_restores[ctx->local_restore_count] = NULL;
    }
    emit_clear_locals_range(ctx->chunk, saved_lc, ctx->local_count);
    ctx->local_count = saved_lc;
}

static void compile_map_expr(CompCtx *ctx, StradaValue *node) {
    /* map { BLOCK } @array */
    StradaValue *body = ast_get(node, "block");
    if (!body) body = ast_get(node, "body");
    StradaValue *array = ast_get(node, "array");

    compile_expr_in_ctx(ctx, array, 1);
    int saved_lc = ctx->local_count;
    int arr_slot = ctx_add_local(ctx, "__mg_arr", '$');
    vm_chunk_emit(ctx->chunk, OP_RESET_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)arr_slot);
    vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)arr_slot);

    vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
    vm_chunk_emit_u32(ctx->chunk, 8);
    int res_slot = ctx_add_local(ctx, "__mg_res", '@');
    vm_chunk_emit(ctx->chunk, OP_RESET_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)res_slot);
    vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)res_slot);

    vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
    vm_chunk_emit_i64(ctx->chunk, 0);
    int idx_slot = ctx_add_local(ctx, "__mg_i", '$');
    vm_chunk_emit(ctx->chunk, OP_RESET_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx_slot);
    vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx_slot);

    int prev_u = ctx_find_local(ctx, "_");
    char *saved_u_name = NULL;
    if (prev_u >= 0) {
        saved_u_name = ctx->local_names[prev_u];
        ctx->local_names[prev_u] = strdup("__shadow_u");
    }
    int underscore_slot = ctx_add_local(ctx, "_", '$');
    vm_chunk_emit(ctx->chunk, OP_RESET_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)underscore_slot);

    int tmp_slot = ctx_add_local(ctx, "__mg_tmp", '$');
    vm_chunk_emit(ctx->chunk, OP_RESET_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)tmp_slot);

    /* A block that can call out publishes the element as the global $_
     * (Perl aliases $_); saved like `local $_` around the loop */
    int mg_topic_lr = -1;
    size_t mg_topic_k = 0;
    if (vm_node_has_call(body, 0)) {
        mg_topic_k = vm_chunk_add_str_const(ctx->chunk, "main::_");
        vm_chunk_emit(ctx->chunk, OP_SAVE_GLOBAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)mg_topic_k);
        mg_topic_lr = ctx->local_restore_count;
        ctx_push_local_restore(ctx, "main::_");
    }

    LoopCtx lctx = {0};
    lctx.parent = ctx->loop_ctx;
    lctx.local_restore_depth = ctx->local_restore_count;
    lctx.break_local_count = ctx->local_count;
    lctx.next_local_count = ctx->local_count;
    lctx.redo_local_count = ctx->local_count;
    ctx->loop_ctx = &lctx;

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
    vm_chunk_emit(ctx->chunk, OP_RESET_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)underscore_slot);
    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)arr_slot);
    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx_slot);
    vm_chunk_emit(ctx->chunk, OP_ARRAY_GET);
    vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)underscore_slot);
    if (mg_topic_lr >= 0) {
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)underscore_slot);
        vm_chunk_emit(ctx->chunk, OP_STORE_GLOBAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)mg_topic_k);
    }

    lctx.redo_target = ctx->chunk->code_len;

    /* Compile body block */
    int iter_lc = ctx->local_count;
    int saved_max_lc = ctx->max_local_count;
    ctx->max_local_count = iter_lc;
    compile_block_expr(ctx, body, underscore_slot);
    int body_max_lc = ctx->max_local_count;
    if (saved_max_lc > ctx->max_local_count) ctx->max_local_count = saved_max_lc;

    /* Store expr value to tmp, then push res_arr, tmp for OP_MAP_PUSH.
     * As in the compiled backend, a block whose value is an [..] / {..} /
     * \X reference yields ONE element (OP_MAP_PUSH would flatten an array
     * value, and the VM cannot tell an array from an array ref at run
     * time); a %h value yields its pairs. */
    int mv_kind = vm_map_value_kind(body);
    if (mv_kind == 2) {
        vm_chunk_emit(ctx->chunk, OP_BUILTIN);
        vm_chunk_emit_u16(ctx->chunk, BUILTIN_HASH_FLAT);
        vm_chunk_emit(ctx->chunk, 1);
    }
    emit_reset_store_local(ctx, tmp_slot);

    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)res_slot);
    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)tmp_slot);
    if (mv_kind == 1) {
        vm_chunk_emit(ctx->chunk, OP_ARRAY_PUSH);
    } else {
        vm_chunk_emit(ctx->chunk, OP_MAP_PUSH);
    }

    for (int i = 0; i < lctx.next_count; i++) {
        patch_u16(ctx, lctx.next_patches[i], (uint16_t)ctx->chunk->code_len);
    }
    if (lctx.next_count > 0 && body_max_lc > iter_lc) {
        emit_clear_locals_range(ctx->chunk, iter_lc, body_max_lc);
    }

    vm_chunk_emit(ctx->chunk, OP_INCR);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx_slot);

    vm_chunk_emit(ctx->chunk, OP_JMP);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)loop_top);

    patch_u16(ctx, exit_patch, (uint16_t)ctx->chunk->code_len);
    for (int i = 0; i < lctx.break_count; i++) {
        patch_u16(ctx, lctx.break_patches[i], (uint16_t)ctx->chunk->code_len);
    }
    loop_free_patches(&lctx);
    ctx->loop_ctx = lctx.parent;
    if (mg_topic_lr >= 0) {
        while (ctx->local_restore_count > mg_topic_lr) {
            ctx->local_restore_count--;
            emit_local_restore(ctx->chunk, ctx->local_restores[ctx->local_restore_count]);
            free(ctx->local_restores[ctx->local_restore_count]);
            ctx->local_restores[ctx->local_restore_count] = NULL;
        }
    }
    if (prev_u >= 0) {
        free(ctx->local_names[prev_u]);
        ctx->local_names[prev_u] = saved_u_name;
    }

    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)res_slot);
    int clear_end = (body_max_lc > ctx->local_count) ? body_max_lc : ctx->local_count;
    emit_clear_locals_range(ctx->chunk, saved_lc, clear_end);
    for (int j = saved_lc; j < ctx->local_count; j++) {
        if (ctx->local_names[j]) { free(ctx->local_names[j]); ctx->local_names[j] = NULL; }
    }
    ctx->local_count = saved_lc;
}

static void compile_grep_expr(CompCtx *ctx, StradaValue *node) {
    StradaValue *body = ast_get(node, "block");
    if (!body) body = ast_get(node, "body");
    StradaValue *array = ast_get(node, "array");

    compile_expr_in_ctx(ctx, array, 1);
    int saved_lc = ctx->local_count;
    int arr_slot = ctx_add_local(ctx, "__gr_arr", '$');
    vm_chunk_emit(ctx->chunk, OP_RESET_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)arr_slot);
    vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)arr_slot);

    vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
    vm_chunk_emit_u32(ctx->chunk, 8);
    int res_slot = ctx_add_local(ctx, "__gr_res", '@');
    vm_chunk_emit(ctx->chunk, OP_RESET_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)res_slot);
    vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)res_slot);

    vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
    vm_chunk_emit_i64(ctx->chunk, 0);
    int idx_slot = ctx_add_local(ctx, "__gr_i", '$');
    vm_chunk_emit(ctx->chunk, OP_RESET_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx_slot);
    vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx_slot);

    int prev_u = ctx_find_local(ctx, "_");
    char *saved_u_name = NULL;
    if (prev_u >= 0) {
        saved_u_name = ctx->local_names[prev_u];
        ctx->local_names[prev_u] = strdup("__shadow_u");
    }
    int underscore_slot = ctx_add_local(ctx, "_", '$');
    vm_chunk_emit(ctx->chunk, OP_RESET_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)underscore_slot);

    /* A block that can call out publishes the element as the global $_
     * (Perl aliases $_); saved like `local $_` around the loop */
    int mg_topic_lr = -1;
    size_t mg_topic_k = 0;
    if (vm_node_has_call(body, 0)) {
        mg_topic_k = vm_chunk_add_str_const(ctx->chunk, "main::_");
        vm_chunk_emit(ctx->chunk, OP_SAVE_GLOBAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)mg_topic_k);
        mg_topic_lr = ctx->local_restore_count;
        ctx_push_local_restore(ctx, "main::_");
    }

    LoopCtx lctx = {0};
    lctx.parent = ctx->loop_ctx;
    lctx.local_restore_depth = ctx->local_restore_count;
    lctx.break_local_count = ctx->local_count;
    lctx.next_local_count = ctx->local_count;
    lctx.redo_local_count = ctx->local_count;
    ctx->loop_ctx = &lctx;

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
    vm_chunk_emit(ctx->chunk, OP_RESET_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)underscore_slot);
    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)arr_slot);
    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx_slot);
    vm_chunk_emit(ctx->chunk, OP_ARRAY_GET);
    vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)underscore_slot);
    if (mg_topic_lr >= 0) {
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)underscore_slot);
        vm_chunk_emit(ctx->chunk, OP_STORE_GLOBAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)mg_topic_k);
    }

    lctx.redo_target = ctx->chunk->code_len;

    /* Compile condition block */
    int iter_lc = ctx->local_count;
    int saved_max_lc = ctx->max_local_count;
    ctx->max_local_count = iter_lc;
    compile_block_expr(ctx, body, underscore_slot);
    int body_max_lc = ctx->max_local_count;
    if (saved_max_lc > ctx->max_local_count) ctx->max_local_count = saved_max_lc;

    /* If condition is true, push $_ to result */
    vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
    size_t skip_patch = emit_patch_u16(ctx);

    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)res_slot);
    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)underscore_slot);
    vm_chunk_emit(ctx->chunk, OP_ARRAY_PUSH);

    patch_u16(ctx, skip_patch, (uint16_t)ctx->chunk->code_len);
    for (int i = 0; i < lctx.next_count; i++) {
        patch_u16(ctx, lctx.next_patches[i], (uint16_t)ctx->chunk->code_len);
    }
    if (lctx.next_count > 0 && body_max_lc > iter_lc) {
        emit_clear_locals_range(ctx->chunk, iter_lc, body_max_lc);
    }

    vm_chunk_emit(ctx->chunk, OP_INCR);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx_slot);
    vm_chunk_emit(ctx->chunk, OP_JMP);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)loop_top);

    patch_u16(ctx, exit_patch, (uint16_t)ctx->chunk->code_len);
    for (int i = 0; i < lctx.break_count; i++) {
        patch_u16(ctx, lctx.break_patches[i], (uint16_t)ctx->chunk->code_len);
    }
    loop_free_patches(&lctx);
    ctx->loop_ctx = lctx.parent;
    if (mg_topic_lr >= 0) {
        while (ctx->local_restore_count > mg_topic_lr) {
            ctx->local_restore_count--;
            emit_local_restore(ctx->chunk, ctx->local_restores[ctx->local_restore_count]);
            free(ctx->local_restores[ctx->local_restore_count]);
            ctx->local_restores[ctx->local_restore_count] = NULL;
        }
    }
    if (prev_u >= 0) {
        free(ctx->local_names[prev_u]);
        ctx->local_names[prev_u] = saved_u_name;
    }

    vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
    vm_chunk_emit_u16(ctx->chunk, (uint16_t)res_slot);
    int clear_end = (body_max_lc > ctx->local_count) ? body_max_lc : ctx->local_count;
    emit_clear_locals_range(ctx->chunk, saved_lc, clear_end);
    for (int j = saved_lc; j < ctx->local_count; j++) {
        if (ctx->local_names[j]) { free(ctx->local_names[j]); ctx->local_names[j] = NULL; }
    }
    ctx->local_count = saved_lc;
}

static void compile_sort_expr(CompCtx *ctx, StradaValue *node) {
    StradaValue *body = ast_get(node, "block");
    if (!body) body = ast_get(node, "body");
    StradaValue *array = ast_get(node, "array");

    if (!body || STRADA_IS_TAGGED_INT(body) || (body->type == STRADA_INT && body->value.iv == 0)) {
        compile_expr_in_ctx(ctx, array, 1);
        vm_chunk_emit(ctx->chunk, OP_BUILTIN);
        vm_chunk_emit_u16(ctx->chunk, BUILTIN_SORT_DEFAULT);
        vm_chunk_emit(ctx->chunk, 1);
        return;
    }

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
    int cmp_func_idx = (int)(ctx->prog->func_count - 1);

    /* Restore outer chunk pointer */
    if (sort_outer_idx >= 0) ctx->chunk = &ctx->prog->funcs[sort_outer_idx];

    cmp_ctx.prog = ctx->prog;
    cmp_ctx.func_name = cmpname;
    cmp_ctx.cur_package = ctx->cur_package;
    cmp_ctx.is_closure = 1;
    cmp_ctx.parent_ctx = ctx;

    /* Add $a and $b as locals */
    ctx_add_local(&cmp_ctx, "a", '$');
    ctx_add_local(&cmp_ctx, "b", '$');
    cmp_ctx.chunk->fixed_param_count = 2;

    /* Compile the comparison expression */
    prescan_vm_labels(&cmp_ctx, body, 0, 0);
    compile_block_expr(&cmp_ctx, body, -1);
    cmp_ctx.chunk = &ctx->prog->funcs[cmp_func_idx];
    if (sort_outer_idx >= 0) ctx->chunk = &ctx->prog->funcs[sort_outer_idx];
    resolve_vm_gotos(&cmp_ctx);
    vm_chunk_emit(cmp_ctx.chunk, OP_RETURN);
    if (cmp_ctx.chunk->code_len > 0xFFFF) {
        fprintf(stderr, "VM compile error: sort comparator '%s' compiles to %zu bytes of bytecode, "
                        "exceeding the VM backend's 65535-byte limit\n",
                cmpname, (size_t)cmp_ctx.chunk->code_len);
        cmp_ctx.errors++;
    }
    ctx->errors += cmp_ctx.errors;
    int cmp_final_lc = cmp_ctx.max_local_count > cmp_ctx.local_count ? cmp_ctx.max_local_count : cmp_ctx.local_count;
    cmp_ctx.chunk->local_count = cmp_final_lc;
    cmp_ctx.chunk->capture_count = cmp_ctx.capture_count;
    if (cmp_ctx.capture_count > 0) {
        cmp_ctx.chunk->capture_names = calloc(cmp_ctx.capture_count, sizeof(char*));
        for (int ci = 0; ci < cmp_ctx.capture_count; ci++) {
            cmp_ctx.chunk->capture_names[ci] = strdup(cmp_ctx.captures[ci].name);
        }
        int outer_slots[64];
        int cap_emit_count = cmp_ctx.capture_count < 64 ? cmp_ctx.capture_count : 64;
        for (int ci = 0; ci < cap_emit_count; ci++) {
            int outer_slot = ctx_find_local_sigil(ctx, cmp_ctx.captures[ci].name, cmp_ctx.captures[ci].sigil);
            if (outer_slot >= 0) {
                /* OP_MAKE_CLOSURE boxes locals[outer_slots[ci]] directly to preserve weakrefs/tied */
                vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
                outer_slots[ci] = outer_slot;
            } else {
                int cap = ctx_find_capture(ctx, cmp_ctx.captures[ci].name, cmp_ctx.captures[ci].sigil);
                if (cap >= 0) {
                    vm_chunk_emit(ctx->chunk, OP_LOAD_CAPTURE_CELL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)cap);
                    outer_slots[ci] = 0xFFFF;
                } else {
                    vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
                    outer_slots[ci] = 0xFFFF;
                }
            }
        }
        vm_chunk_emit(ctx->chunk, OP_MAKE_CLOSURE);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)cmp_func_idx);
        vm_chunk_emit(ctx->chunk, (uint8_t)cap_emit_count);
        for (int ci = 0; ci < cap_emit_count; ci++) {
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)outer_slots[ci]);
        }
    }

    for (int i = 0; i < cmp_final_lc; i++) if (cmp_ctx.local_names[i]) free(cmp_ctx.local_names[i]);
    for (int i = 0; i < cmp_ctx.capture_count; i++) free(cmp_ctx.captures[i].name);
    ctx_free_local_restores(&cmp_ctx);

    /* Now compile: OP_ARRAY_SORT func_idx (or 0xFFFF for closure comparator) */
    compile_expr_in_ctx(ctx, array, 1);
    if (sort_outer_idx >= 0) ctx->chunk = &ctx->prog->funcs[sort_outer_idx];
    if (cmp_ctx.capture_count > 0) {
        vm_chunk_emit(ctx->chunk, OP_ARRAY_SORT);
        vm_chunk_emit_u16(ctx->chunk, 0xFFFF);
    } else {
        int cmp_fidx = vm_program_find_func(ctx->prog, cmpname);
        vm_chunk_emit(ctx->chunk, OP_ARRAY_SORT);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)cmp_fidx);
    }
}

/* ===== Statement compilation ===== */

static int is_pure_append_rhs(StradaValue *node) {
    node = ast_deref(node);
    if (!node || STRADA_IS_TAGGED_INT(node)) return 0;
    int t = (int)ast_int(node, "type");
    return (t == NI_STR_LITERAL || t == NI_INT_LITERAL || t == NI_NUM_LITERAL ||
            t == NI_UNDEF || t == NI_VARIABLE);
}

static int is_self_concat_rhs(StradaValue *value, const char *vname, char vsigil, StradaValue **rhs_out) {
    StradaValue *vn = ast_deref(value);
    if (!vn || STRADA_IS_TAGGED_INT(vn)) return 0;
    if ((int)ast_int(vn, "type") != NI_BINARY_OP || ast_op_code(vn) != OP_AST_CONCAT) return 0;
    StradaValue *ln = ast_deref(ast_get(vn, "left"));
    if (!ln || STRADA_IS_TAGGED_INT(ln) || (int)ast_int(ln, "type") != NI_VARIABLE) return 0;
    const char *ln_name = ast_str(ln, "name");
    const char *ln_sigil = ast_str(ln, "sigil");
    char ls = (ln_sigil && ln_sigil[0]) ? ln_sigil[0] : '$';
    if (ls != vsigil || strcmp(ln_name, vname) != 0) return 0;
    StradaValue *rn = ast_deref(ast_get(vn, "right"));
    if (!is_pure_append_rhs(rn)) return 0;
    *rhs_out = rn;
    return 1;
}

static int is_self_unit_step(StradaValue *value, const char *vname, char vsigil, int *is_inc_out) {
    StradaValue *vn = ast_deref(value);
    if (!vn || STRADA_IS_TAGGED_INT(vn)) return 0;
    if ((int)ast_int(vn, "type") != NI_BINARY_OP) return 0;
    int op = ast_op_code(vn);
    if (op != OP_AST_ADD && op != OP_AST_SUB) return 0;
    StradaValue *ln = ast_deref(ast_get(vn, "left"));
    StradaValue *rn = ast_deref(ast_get(vn, "right"));
    if (!ln || STRADA_IS_TAGGED_INT(ln) || (int)ast_int(ln, "type") != NI_VARIABLE) return 0;
    if (!rn || STRADA_IS_TAGGED_INT(rn) || (int)ast_int(rn, "type") != NI_INT_LITERAL) return 0;
    if (ast_int(rn, "value") != 1) return 0;
    const char *ln_name = ast_str(ln, "name");
    const char *ln_sigil = ast_str(ln, "sigil");
    char ls = (ln_sigil && ln_sigil[0]) ? ln_sigil[0] : '$';
    if (ls != vsigil || strcmp(ln_name, vname) != 0) return 0;
    *is_inc_out = (op == OP_AST_ADD);
    return 1;
}

static void compile_void_expr(CompCtx *ctx, StradaValue *expr) {
    expr = ast_deref(expr);
    if (expr && !STRADA_IS_TAGGED_INT(expr)) {
        int etype = (int)ast_int(expr, "type");
        if (etype == NI_TR || etype == NI_REGEX_SUBST) {
            compile_stmt(ctx, expr);
            return;
        }
        if (etype == NI_INCREMENT) {
            StradaValue *operand = ast_deref(ast_get(expr, "operand"));
            if (operand && !STRADA_IS_TAGGED_INT(operand) && (int)ast_int(operand, "type") == NI_VARIABLE) {
                const char *vname = ast_str(operand, "name");
                const char *vsigil = ast_str(operand, "sigil");
                char sc = (vsigil && vsigil[0]) ? vsigil[0] : '$';
                int slot = ctx_find_local_sigil(ctx, vname, sc);
                if (slot >= 0) {
                    int is_inc = (strcmp(ast_str(expr, "op"), "++") == 0);
                    vm_chunk_emit(ctx->chunk, is_inc ? OP_INCR : OP_DECR);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                    return;
                }
            }
        } else if (etype == NI_ASSIGN) {
            StradaValue *t = ast_deref(ast_get(expr, "target"));
            const char *op = ast_str(expr, "op");
            if (t && !STRADA_IS_TAGGED_INT(t) && (int)ast_int(t, "type") == NI_VARIABLE &&
                !is_shortcircuit_assign(op)) {
                const char *vname = ast_str(t, "name");
                const char *vsigil = ast_str(t, "sigil");
                char sc = (vsigil && vsigil[0]) ? vsigil[0] : '$';
                int slot = ctx_find_local_sigil(ctx, vname, sc);
                if (slot >= 0) {
                    StradaValue *value = ast_get(expr, "value");
                    if (strcmp(op, ".=") == 0) {
                        StradaValue *val_node = ast_deref(value);
                        if (val_node && !STRADA_IS_TAGGED_INT(val_node) &&
                            (int)ast_int(val_node, "type") == NI_STR_LITERAL) {
                            size_t slen = 0;
                            const char *sval = ast_str_len(val_node, "value", &slen);
                            size_t si = vm_chunk_add_str_const_len(ctx->chunk, sval, slen);
                            vm_chunk_emit(ctx->chunk, OP_APPEND_CONST);
                            vm_chunk_emit_u16(ctx->chunk, (uint16_t)si);
                            vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                        } else {
                            compile_expr(ctx, value);
                            vm_chunk_emit(ctx->chunk, OP_APPEND_LOCAL);
                            vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                        }
                        return;
                    }
                    StradaValue *append_rhs = NULL;
                    if (strcmp(op, "=") == 0 && ctx->local_types[slot] == 0 && sc == '$' &&
                        is_self_concat_rhs(value, vname, '$', &append_rhs)) {
                        if ((int)ast_int(append_rhs, "type") == NI_STR_LITERAL) {
                            size_t slen = 0;
                            const char *sval = ast_str_len(append_rhs, "value", &slen);
                            size_t si = vm_chunk_add_str_const_len(ctx->chunk, sval, slen);
                            vm_chunk_emit(ctx->chunk, OP_APPEND_CONST);
                            vm_chunk_emit_u16(ctx->chunk, (uint16_t)si);
                            vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                        } else {
                            compile_expr(ctx, append_rhs);
                            vm_chunk_emit(ctx->chunk, OP_APPEND_LOCAL);
                            vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                        }
                        return;
                    }
                    int is_inc = 0;
                    if (strcmp(op, "=") == 0 && ctx->local_types[slot] == 1 && sc == '$' &&
                        is_self_unit_step(value, vname, '$', &is_inc)) {
                        vm_chunk_emit(ctx->chunk, is_inc ? OP_INCR : OP_DECR);
                        vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                        return;
                    }
                    if (strcmp(op, "=") == 0) {
                        StradaValue *vn = ast_deref(value);
                        if (vn && !STRADA_IS_TAGGED_INT(vn) && (int)ast_int(vn, "type") == NI_BINARY_OP &&
                            ast_op_code(vn) == OP_AST_ADD) {
                            int lslot = find_local_var_slot(ctx, ast_get(vn, "left"));
                            int rslot = find_local_var_slot(ctx, ast_get(vn, "right"));
                            if (lslot == slot && rslot >= 0) {
                                vm_chunk_emit(ctx->chunk, OP_ADD_TO_LOCAL);
                                vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                                vm_chunk_emit_u16(ctx->chunk, (uint16_t)rslot);
                                vm_chunk_emit(ctx->chunk, ctx->local_types[slot]);
                                return;
                            }
                        }
                    }
                    int cop = compound_op_to_opcode(op);
                    if (strcmp(op, "=") == 0 && (sc == '@' || sc == '%')) {
                        /* tied target: CLEAR + STORE, keep the tie */
                        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                        vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                        compile_assign_var_rhs(ctx, sc, value);
                        vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                        vm_chunk_emit_u16(ctx->chunk, BUILTIN_TIE_ASSIGN);
                        vm_chunk_emit(ctx->chunk, 2);
                    } else if (strcmp(op, "=") == 0 && sc == '$' && ctx->local_types[slot] == 1 && vm_try_char_code(ctx, value)) {
                        /* $c = '(' on an int: the character code */
                    } else if (strcmp(op, "=") == 0) {
                        compile_assign_var_rhs(ctx, sc, value);
                    } else if (cop >= 0) {
                        if (cop == OP_ADD && sc == '$') {
                            int rslot = find_local_var_slot(ctx, value);
                            if (rslot >= 0) {
                                vm_chunk_emit(ctx->chunk, OP_ADD_TO_LOCAL);
                                vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                                vm_chunk_emit_u16(ctx->chunk, (uint16_t)rslot);
                                vm_chunk_emit(ctx->chunk, ctx->local_types[slot]);
                                return;
                            }
                        }
                        if ((cop == OP_ADD || cop == OP_SUB) && sc == '$') {
                            int32_t imm = 0;
                            if (ctx->local_types[slot] == 1 && try_const_int32(value, &imm) && imm == 1) {
                                vm_chunk_emit(ctx->chunk, (cop == OP_ADD) ? OP_INCR : OP_DECR);
                                vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                                return;
                            }
                            compile_expr(ctx, value);
                            vm_chunk_emit(ctx->chunk, (cop == OP_ADD) ? OP_ADD_TO_LOCAL_VAL : OP_SUB_TO_LOCAL_VAL);
                            vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                            vm_chunk_emit(ctx->chunk, ctx->local_types[slot]);
                            return;
                        }
                        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                        vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                        compile_expr(ctx, value);
                        vm_chunk_emit(ctx->chunk, (uint8_t)cop);
                    } else {
                        compile_expr(ctx, value);
                    }
                    if (ctx->local_types[slot] != 0 &&
                        (strcmp(op, "=") != 0 || infer_expr_scalar_type(value) != ctx->local_types[slot])) {
                        emit_scalar_type_coerce(ctx, ctx->local_types[slot]);
                    }
                    vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                    return;
                }
            }
            if (strcmp(op, "=") == 0) {
                int is_hash = 0;
                StradaValue *cont_node = NULL, *idx_node = NULL;
                if (extract_cont_idx(t, &is_hash, &cont_node, &idx_node)) {
                    StradaValue *value = ast_get(expr, "value");
                    const char *prefix; StradaValue *suffix;
                    size_t kidx = 0;
                    if (is_hash && (int)ast_int(t, "type") == NI_HASH_ACCESS &&
                        is_concat_key(idx_node, &prefix, &suffix)) {
                        compile_expr(ctx, cont_node);
                        compile_expr(ctx, suffix);
                        compile_expr(ctx, value);
                        size_t pidx = vm_chunk_add_str_const(ctx->chunk, prefix);
                        vm_chunk_emit(ctx->chunk, OP_HASH_SET_CONCAT);
                        vm_chunk_emit_u16(ctx->chunk, (uint16_t)pidx);
                        return;
                    } else if (is_hash && try_const_str_key(ctx, idx_node, &kidx)) {
                        compile_expr(ctx, cont_node);
                        compile_expr(ctx, value);
                        vm_chunk_emit(ctx->chunk, OP_HASH_SET_CONST);
                        vm_chunk_emit_u16(ctx->chunk, (uint16_t)kidx);
                        return;
                    } else {
                        compile_expr(ctx, cont_node);
                        compile_expr(ctx, idx_node);
                        compile_expr(ctx, value);
                        vm_chunk_emit(ctx->chunk, is_hash ? OP_HASH_SET : OP_ARRAY_SET);
                        return;
                    }
                }
            }
        }
    }
    compile_expr(ctx, expr);
    vm_chunk_emit(ctx->chunk, OP_POP);
}

static size_t emit_cond_jmp_false(CompCtx *ctx, StradaValue *cond) {
    StradaValue *cn = ast_deref(cond);
    if (cn && !STRADA_IS_TAGGED_INT(cn) && (int)ast_int(cn, "type") == NI_BINARY_OP) {
        int op = ast_op_code(cn);
        if (op == OP_AST_NUM_LT || op == OP_AST_NUM_LE) {
            StradaValue *ln = ast_get(cn, "left");
            StradaValue *rn = ast_get(cn, "right");
            StradaValue *lsq = ast_get(ln, "from_sq_str");
            StradaValue *rsq = ast_get(rn, "from_sq_str");
            if (lsq && (STRADA_IS_TAGGED_INT(lsq) || lsq->type != STRADA_STR)) lsq = NULL;
            if (rsq && (STRADA_IS_TAGGED_INT(rsq) || rsq->type != STRADA_STR)) rsq = NULL;
            if (!lsq && !rsq) {
                int sa = find_local_var_slot(ctx, ln);
                if (sa >= 0) {
                    int32_t imm = 0;
                    if (try_const_int32(rn, &imm)) {
                        vm_chunk_emit(ctx->chunk, (op == OP_AST_NUM_LT) ? OP_LT_LOCAL_I32_JMP_F : OP_LE_LOCAL_I32_JMP_F);
                        vm_chunk_emit_u16(ctx->chunk, (uint16_t)sa);
                        vm_chunk_emit_u32(ctx->chunk, (uint32_t)imm);
                        return emit_patch_u16(ctx);
                    }
                    if (op == OP_AST_NUM_LT) {
                        int sb = find_local_var_slot(ctx, rn);
                        if (sb >= 0) {
                            vm_chunk_emit(ctx->chunk, OP_LT_LOCAL_LOCAL_JMP_F);
                            vm_chunk_emit_u16(ctx->chunk, (uint16_t)sa);
                            vm_chunk_emit_u16(ctx->chunk, (uint16_t)sb);
                            return emit_patch_u16(ctx);
                        }
                    }
                }
            }
        }
    }
    compile_expr(ctx, cond);
    vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
    return emit_patch_u16(ctx);
}

static int match_local_unit_incr(CompCtx *ctx, StradaValue *step, int *out_slot) {
    if (!step || STRADA_IS_TAGGED_INT(step)) return 0;
    StradaValue *s = ast_deref(step);
    if (!s || STRADA_IS_TAGGED_INT(s)) return 0;
    int etype = (int)ast_int(s, "type");
    if (etype == NI_INCREMENT) {
        if (strcmp(ast_str(s, "op"), "++") != 0) return 0;
        StradaValue *operand = ast_deref(ast_get(s, "operand"));
        if (!operand || STRADA_IS_TAGGED_INT(operand) || (int)ast_int(operand, "type") != NI_VARIABLE) return 0;
        const char *vsigil = ast_str(operand, "sigil");
        char sc = (vsigil && vsigil[0]) ? vsigil[0] : '$';
        if (sc != '$') return 0;
        int slot = ctx_find_local_sigil(ctx, ast_str(operand, "name"), '$');
        if (slot < 0) return 0;
        *out_slot = slot;
        return 1;
    }
    if (etype == NI_ASSIGN) {
        StradaValue *t = ast_deref(ast_get(s, "target"));
        const char *op = ast_str(s, "op");
        if (!t || STRADA_IS_TAGGED_INT(t) || (int)ast_int(t, "type") != NI_VARIABLE) return 0;
        const char *vname = ast_str(t, "name");
        const char *vsigil = ast_str(t, "sigil");
        char sc = (vsigil && vsigil[0]) ? vsigil[0] : '$';
        if (sc != '$') return 0;
        int slot = ctx_find_local_sigil(ctx, vname, '$');
        if (slot < 0) return 0;
        StradaValue *value = ast_get(s, "value");
        if (strcmp(op, "+=") == 0) {
            int32_t imm = 0;
            if (ctx->local_types[slot] == 1 && try_const_int32(value, &imm) && imm == 1) {
                *out_slot = slot;
                return 1;
            }
        } else if (strcmp(op, "=") == 0 && ctx->local_types[slot] == 1) {
            int is_inc = 0;
            if (is_self_unit_step(value, vname, '$', &is_inc) && is_inc) {
                *out_slot = slot;
                return 1;
            }
        }
    }
    return 0;
}

static int match_for_loop_fuse(CompCtx *ctx, StradaValue *cond, StradaValue *step,
                               int *out_slot, int *out_kind, int32_t *out_imm, int *out_bslot) {
    int step_slot = -1;
    if (!match_local_unit_incr(ctx, step, &step_slot)) return 0;
    StradaValue *cn = ast_deref(cond);
    if (!cn || STRADA_IS_TAGGED_INT(cn) || (int)ast_int(cn, "type") != NI_BINARY_OP) return 0;
    int op = ast_op_code(cn);
    if (op != OP_AST_NUM_LT && op != OP_AST_NUM_LE) return 0;
    StradaValue *ln = ast_get(cn, "left");
    StradaValue *rn = ast_get(cn, "right");
    StradaValue *lsq = ast_get(ln, "from_sq_str");
    StradaValue *rsq = ast_get(rn, "from_sq_str");
    if (lsq && (STRADA_IS_TAGGED_INT(lsq) || lsq->type != STRADA_STR)) lsq = NULL;
    if (rsq && (STRADA_IS_TAGGED_INT(rsq) || rsq->type != STRADA_STR)) rsq = NULL;
    if (lsq || rsq) return 0;
    if (find_local_var_slot(ctx, ln) != step_slot) return 0;
    int32_t imm = 0;
    if (try_const_int32(rn, &imm)) {
        *out_slot = step_slot;
        *out_kind = (op == OP_AST_NUM_LT) ? 0 : 1;
        *out_imm = imm;
        *out_bslot = -1;
        return 1;
    }
    if (op == OP_AST_NUM_LT) {
        int sb = find_local_var_slot(ctx, rn);
        if (sb >= 0) {
            *out_slot = step_slot;
            *out_kind = 2;
            *out_imm = 0;
            *out_bslot = sb;
            return 1;
        }
    }
    return 0;
}

static void compile_stmt(CompCtx *ctx, StradaValue *node) {
    if (!node || STRADA_IS_TAGGED_INT(node)) return;
    node = ast_deref(node);
    if (!node || STRADA_IS_TAGGED_INT(node)) return;

    int type = (int)ast_int(node, "type");

    switch (type) {
    case NI_EXPR_STMT: {
        compile_void_expr(ctx, ast_get(node, "expr"));
        break;
    }

    case NI_VAR_DECL: {
        const char *name = ast_str(node, "name");
        const char *sigil = ast_str(node, "sigil");
        int slot = ctx_add_local(ctx, name, sigil[0]);
        int vt = (sigil[0] == '$') ? (int)ast_int(node, "var_type") : 0;
        if (slot >= 0 && slot < 256 && ast_str(node, "struct_name") && ast_str(node, "struct_name")[0])
            ctx->local_struct[slot] = ast_str(node, "struct_name");
        if (slot >= 0) ctx->local_types[slot] = (vt == 1 || vt == 2) ? (uint8_t)vt : 0;
        StradaValue *init = ast_get(node, "init");

        if (sigil[0] == '@') {
            compile_array_init_expr(ctx, node, init, 1);
        } else if (sigil[0] == '%') {
            compile_hash_init_expr(ctx, node, init, 1);
        } else if (init && !STRADA_IS_TAGGED_INT(init) && (int)ast_int(init, "type") == NI_ANON_HASH &&
                   ast_int(init, "is_empty_list") == 1) {
            /* my $x = (): undef */
            emit_default_scalar_init(ctx, vt);
        } else if (init && !STRADA_IS_TAGGED_INT(init) && vt == 1 && vm_try_char_code(ctx, init)) {
            /* my int $c = '(' */
        } else if (init && !STRADA_IS_TAGGED_INT(init) && vm_is_splice_call(init)) {
            /* my $x = splice(...): scalar context is the last removed element */
            compile_expr(ctx, init);
            vm_chunk_emit(ctx->chunk, OP_ARRAY_POP);
            if (slot >= 0 && ctx->local_types[slot] != 0) emit_scalar_type_coerce(ctx, ctx->local_types[slot]);
        } else if (init && !STRADA_IS_TAGGED_INT(init)) {
            compile_expr(ctx, init);
            if (slot >= 0 && ctx->local_types[slot] != 0 &&
                infer_expr_scalar_type(init) != ctx->local_types[slot]) {
                emit_scalar_type_coerce(ctx, ctx->local_types[slot]);
            }
        } else if (ast_str(node, "struct_name") && ast_str(node, "struct_name")[0]) {
            /* my StructType $v; -- the VM models a C struct as a hash of its
             * fields ($v->x = 1 / $v->x) */
            vm_emit_struct_new(ctx, ast_str(node, "struct_name"));
        } else {
            emit_default_scalar_init(ctx, vt);
        }
        if (ctx->may_box_locals) {
            vm_chunk_emit(ctx->chunk, OP_RESET_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
        }
        vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
        break;
    }

    case NI_OUR_DECL: {
        /* our variable — use global registry (package-qualified when non-main) */
        const char *name = ast_str(node, "name");
        const char *sigil = ast_str(node, "sigil");
        const char *pkg = ast_str(node, "package");
        StradaValue *init = ast_get(node, "init");

        char qbuf[256];
        const char *qname = name;
        if (pkg && pkg[0] && strcmp(pkg, "main") != 0 && !strchr(name, ':')) {
            snprintf(qbuf, sizeof(qbuf), "%s::%s", pkg, name);
            qname = qbuf;
        } else if (strncmp(name, "main::", 6) == 0) {
            qname = name + 6;
        }

        register_our_var(qname);

        size_t key_idx = vm_chunk_add_str_const(ctx->chunk, qname);

        if (sigil[0] == '@') {
            compile_array_init_expr(ctx, node, init, 1);
        } else if (sigil[0] == '%') {
            compile_hash_init_expr(ctx, node, init, 1);
        } else if (init && !STRADA_IS_TAGGED_INT(init)) {
            compile_expr(ctx, init);
        } else {
            emit_default_scalar_init(ctx, ast_int(node, "var_type"));
        }
        vm_chunk_emit(ctx->chunk, (init && !STRADA_IS_TAGGED_INT(init)) ? OP_STORE_GLOBAL : OP_INIT_GLOBAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)key_idx);
        break;
    }

    case NI_LOCAL_DECL: {
        const char *name = ast_str(node, "name");
        StradaValue *init = ast_get(node, "init");

        if (ast_int(node, "is_hash_elem") == 1) {
            int ld = ctx->assign_depth++;
            char hbuf[32], kbuf[32];
            snprintf(hbuf, sizeof(hbuf), "__lhh%d", ld);
            snprintf(kbuf, sizeof(kbuf), "__lhk%d", ld);
            int h_slot = ctx_get_or_add_local(ctx, hbuf, '$');
            int k_slot = ctx_get_or_add_local(ctx, kbuf, '$');

            int h_local = ctx_find_local_sigil(ctx, name, '%');
            if (h_local >= 0) {
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)h_local);
            } else {
                int h_cap = ctx_resolve_capture(ctx, name, '%');
                if (h_cap >= 0) {
                    vm_chunk_emit(ctx->chunk, OP_LOAD_CAPTURE);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)h_cap);
                } else {
                    char lgbuf[256];
                    const char *hkey_name = normalize_local_global_key(ctx, name, ast_str(node, "our_key"), lgbuf, sizeof(lgbuf));
                    size_t hk = vm_chunk_add_str_const(ctx->chunk, hkey_name);
                    vm_chunk_emit(ctx->chunk, OP_LOAD_GLOBAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)hk);
                }
            }
            vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)h_slot);

            compile_expr(ctx, ast_get(node, "hash_key"));
            vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)k_slot);

            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)h_slot);
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)k_slot);
            vm_chunk_emit(ctx->chunk, OP_SAVE_HASH_ELEM);

            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)h_slot);
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)k_slot);
            if (init && !STRADA_IS_TAGGED_INT(init)) {
                compile_expr(ctx, init);
            } else {
                vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
            }
            vm_chunk_emit(ctx->chunk, OP_HASH_SET);

            ctx_push_local_restore(ctx, "__HASH_ELEM__");
            ctx->assign_depth--;
            break;
        }

        char lbuf[256];
        const char *lkey = normalize_local_global_key(ctx, name, ast_str(node, "our_key"), lbuf, sizeof(lbuf));
        size_t key_idx = vm_chunk_add_str_const(ctx->chunk, lkey);

        /* Save current value */
        vm_chunk_emit(ctx->chunk, OP_SAVE_GLOBAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)key_idx);

        /* Store new value */
        if (init && !STRADA_IS_TAGGED_INT(init)) {
            compile_expr(ctx, init);
        } else {
            vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
        }
        vm_chunk_emit(ctx->chunk, (init && !STRADA_IS_TAGGED_INT(init)) ? OP_STORE_GLOBAL : OP_INIT_GLOBAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)key_idx);

        /* Track for restore on scope exit / return */
        ctx_push_local_restore(ctx, lkey);
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
        vm_chunk_emit(ctx->chunk, (init && !STRADA_IS_TAGGED_INT(init)) ? OP_STORE_GLOBAL : OP_INIT_GLOBAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)key_idx);
        break;
    }

    case NI_RETURN_STMT: {
        StradaValue *val = ast_get(node, "value");
        int ret_type = ctx->chunk ? ctx->chunk->return_type : 0;
        StradaValue *rvd = ast_deref(val);
        if (rvd && !STRADA_IS_TAGGED_INT(rvd) && rvd->type == STRADA_HASH &&
            (int)ast_int(rvd, "type") == NI_ANON_HASH && ast_int(rvd, "is_empty_list") == 1 &&
            ret_type != AST_TYPE_ARRAY && ret_type != AST_TYPE_HASH_RET) {
            /* return () from a scalar function: undef (compiled Semantic) */
            val = NULL;
        }
        if (val && !STRADA_IS_TAGGED_INT(val) && (val->type == STRADA_HASH || val->type == STRADA_REF)) {
            int can_tc = (!ctx->try_ctx && ctx->local_restore_count == 0);
            StradaValue *vn = can_tc ? ast_deref(val) : NULL;
            int is_direct_call = (vn && !STRADA_IS_TAGGED_INT(vn) && (int)ast_int(vn, "type") == NI_CALL);
            ctx->tail_call_end = (size_t)-1;
            compile_expr(ctx, val);
            if (is_direct_call && ctx->tail_call_end == ctx->chunk->code_len &&
                ctx->chunk->code_len >= 4 && ctx->chunk->code[ctx->chunk->code_len - 4] == OP_CALL) {
                uint16_t cfidx = (uint16_t)ctx->chunk->code[ctx->chunk->code_len - 3] |
                                 ((uint16_t)ctx->chunk->code[ctx->chunk->code_len - 2] << 8);
                VMChunk *cc = (cfidx < ctx->prog->func_count) ? &ctx->prog->funcs[cfidx] : NULL;
                if (cc && (ret_type == 0 || cc == ctx->chunk || cc->return_type == ret_type)) {
                    ctx->chunk->code[ctx->chunk->code_len - 4] = OP_TAIL_CALL;
                    break;
                }
            }
            emit_scalar_type_coerce(ctx, ret_type);
        } else {
            vm_chunk_emit(ctx->chunk, OP_PUSH_UNDEF);
        }
        /* Unwind any enclosing try/finally blocks and local saves in LIFO order */
        emit_try_unwind_for_return(ctx);
        vm_chunk_emit(ctx->chunk, OP_RETURN);
        break;
    }

    case NI_IF_STMT: {
        size_t else_patch = emit_cond_jmp_false(ctx, ast_get(node, "condition"));

        compile_block(ctx, ast_get(node, "then_block"));

        StradaValue *elsif_conds = ast_get(node, "elsif_conditions");
        StradaValue *elsif_blocks = ast_get(node, "elsif_blocks");
        int elsif_count = (int)ast_int(node, "elsif_count");

        size_t *end_patches = malloc((size_t)(elsif_count + 2) * sizeof(size_t));
        int end_patch_count = 0;

        vm_chunk_emit(ctx->chunk, OP_JMP);
        end_patches[end_patch_count++] = emit_patch_u16(ctx);

        patch_u16(ctx, else_patch, (uint16_t)ctx->chunk->code_len);

        for (int i = 0; i < elsif_count; i++) {
            size_t next_patch = emit_cond_jmp_false(ctx, ast_arr_get(elsif_conds, i));
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
        free(end_patches);
        break;
    }

    case NI_WHILE_STMT: {
        LoopCtx lctx = {0};
        lctx.parent = ctx->loop_ctx;
        lctx.local_restore_depth = ctx->local_restore_count;
        lctx.break_local_count = ctx->local_count;
        lctx.next_local_count = ctx->local_count;
        lctx.redo_local_count = ctx->local_count;
        const char *wlabel = ast_str(node, "label");
        if ((!wlabel || !wlabel[0]) && ctx->pending_loop_label && ctx->pending_loop_label[0]) {
            wlabel = ctx->pending_loop_label;
            ctx->pending_loop_label = NULL;
        }
        if (wlabel && wlabel[0]) {
            lctx.label = wlabel;
            VMLabelDef *lbl = vm_ctx_ensure_label(ctx, wlabel);
            lbl->offset = ctx->chunk->code_len;
            lbl->has_offset = 1;
            lbl->actual_local_count = ctx->local_count;
            lbl->has_actual_lc = 1;
        }
        ctx->loop_ctx = &lctx;

        size_t loop_top = ctx->chunk->code_len;
        lctx.continue_target = loop_top; /* next jumps to loop_top to re-check condition */

        size_t exit_patch = emit_cond_jmp_false(ctx, ast_get(node, "condition"));

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

        loop_free_patches(&lctx);
        ctx->loop_ctx = lctx.parent;
        break;
    }

    case NI_DO_WHILE_STMT: {
        LoopCtx lctx = {0};
        lctx.parent = ctx->loop_ctx;
        lctx.local_restore_depth = ctx->local_restore_count;
        lctx.break_local_count = ctx->local_count;
        lctx.next_local_count = ctx->local_count;
        lctx.redo_local_count = ctx->local_count;
        const char *dlabel = ast_str(node, "label");
        if ((!dlabel || !dlabel[0]) && ctx->pending_loop_label && ctx->pending_loop_label[0]) {
            dlabel = ctx->pending_loop_label;
            ctx->pending_loop_label = NULL;
        }
        if (dlabel && dlabel[0]) {
            lctx.label = dlabel;
            VMLabelDef *lbl = vm_ctx_ensure_label(ctx, dlabel);
            lbl->offset = ctx->chunk->code_len;
            lbl->has_offset = 1;
            lbl->actual_local_count = ctx->local_count;
            lbl->has_actual_lc = 1;
        }
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

        loop_free_patches(&lctx);
        ctx->loop_ctx = lctx.parent;
        break;
    }

    case NI_FOR_STMT: {
        int saved_for_lc = ctx->local_count;
        const char *flabel = ast_str(node, "label");
        if ((!flabel || !flabel[0]) && ctx->pending_loop_label && ctx->pending_loop_label[0]) {
            flabel = ctx->pending_loop_label;
            ctx->pending_loop_label = NULL;
        }
        if (flabel && flabel[0]) {
            VMLabelDef *lbl = vm_ctx_ensure_label(ctx, flabel);
            lbl->offset = ctx->chunk->code_len;
            lbl->has_offset = 1;
            lbl->actual_local_count = ctx->local_count;
            lbl->has_actual_lc = 1;
        }
        ctx_push_scope(ctx, (void *)ast_deref(node), saved_for_lc);
        StradaValue *init = ast_get(node, "init");
        if (init && !STRADA_IS_TAGGED_INT(init)) compile_stmt(ctx, init);

        LoopCtx lctx = {0};
        lctx.parent = ctx->loop_ctx;
        lctx.local_restore_depth = ctx->local_restore_count;
        lctx.break_local_count = ctx->local_count;
        lctx.next_local_count = ctx->local_count;
        lctx.redo_local_count = ctx->local_count;
        if (flabel && flabel[0]) lctx.label = flabel;
        ctx->loop_ctx = &lctx;

        size_t loop_top = ctx->chunk->code_len;

        StradaValue *cond = ast_get(node, "condition");
        StradaValue *step = ast_get(node, "update");
        if (!step) step = ast_get(node, "step");
        int fuse_slot = -1, fuse_kind = 0, fuse_bslot = -1;
        int32_t fuse_imm = 0;
        int can_fuse = (cond && !STRADA_IS_TAGGED_INT(cond) && step && !STRADA_IS_TAGGED_INT(step) &&
                        match_for_loop_fuse(ctx, cond, step, &fuse_slot, &fuse_kind, &fuse_imm, &fuse_bslot));
        size_t exit_patch;
        if (cond && !STRADA_IS_TAGGED_INT(cond)) {
            exit_patch = emit_cond_jmp_false(ctx, cond);
        } else {
            emit_push_int(ctx->chunk, 1);
            vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
            exit_patch = emit_patch_u16(ctx);
        }

        /* redo jumps here — body start */
        size_t body_start = ctx->chunk->code_len;
        lctx.redo_target = body_start;

        compile_block(ctx, ast_get(node, "body"));

        /* next jumps to step */
        for (int i = 0; i < lctx.next_count; i++) {
            patch_u16(ctx, lctx.next_patches[i], (uint16_t)ctx->chunk->code_len);
        }

        if (can_fuse) {
            if (fuse_kind == 2) {
                vm_chunk_emit(ctx->chunk, OP_INCR_LT_LOCAL_JMP_T);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)fuse_slot);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)fuse_bslot);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)body_start);
            } else {
                vm_chunk_emit(ctx->chunk, (fuse_kind == 0) ? OP_INCR_LT_I32_JMP_T : OP_INCR_LE_I32_JMP_T);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)fuse_slot);
                vm_chunk_emit_u32(ctx->chunk, (uint32_t)fuse_imm);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)body_start);
            }
        } else {
            if (step && !STRADA_IS_TAGGED_INT(step)) {
                compile_void_expr(ctx, step);
            }

            vm_chunk_emit(ctx->chunk, OP_JMP);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)loop_top);
        }

        patch_u16(ctx, exit_patch, (uint16_t)ctx->chunk->code_len);

        for (int i = 0; i < lctx.break_count; i++) {
            patch_u16(ctx, lctx.break_patches[i], (uint16_t)ctx->chunk->code_len);
        }

        loop_free_patches(&lctx);
        ctx->loop_ctx = lctx.parent;
        ctx_pop_scope(ctx);
        emit_clear_locals_range(ctx->chunk, saved_for_lc, ctx->local_count);
        ctx->local_count = saved_for_lc;
        break;
    }

    case NI_FOREACH_STMT: {
        /* foreach my $var (@array) { body } */
        const char *var_name = ast_str(node, "var_name");
        StradaValue *var_decl = ast_deref(ast_get(node, "var_decl"));
        int is_lex_var = (var_decl && !STRADA_IS_TAGGED_INT(var_decl) && var_decl->type == STRADA_HASH);
        const char *vname = (var_name && var_name[0]) ? var_name : "_";
        StradaValue *array_expr = ast_get(node, "array");

        /* Compile array */
        compile_expr_in_ctx(ctx, array_expr, 1);

        /* Non-lexical loop variables resolve to existing local/capture/our or create enclosing local */
        int var_slot = -1;
        int var_cap = -1;
        const char *var_gkey = NULL;
        int fe_restore = 0;   /* a pre-existing loop variable is restored after the loop (Perl) */
        /* foreach over $_ aliases the global $_ (Perl): saved like `local $_`
         * so return/last/next restore it too */
        int fe_topic_lr = -1;
        if (strcmp(vname, "_") == 0) {
            is_lex_var = 0;
            var_gkey = "main::_";
            size_t tk = vm_chunk_add_str_const(ctx->chunk, "main::_");
            vm_chunk_emit(ctx->chunk, OP_SAVE_GLOBAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)tk);
            fe_topic_lr = ctx->local_restore_count;
            ctx_push_local_restore(ctx, "main::_");
        } else if (!is_lex_var) {
            var_slot = ctx_find_local_sigil(ctx, vname, '$');
            if (var_slot < 0) var_cap = ctx_resolve_capture(ctx, vname, '$');
            if (var_slot < 0 && var_cap < 0) var_gkey = resolve_global_var_key(ctx, vname);
            if (var_slot < 0 && var_cap < 0 && !var_gkey) {
                var_slot = ctx_add_local(ctx, vname, '$');
            } else {
                fe_restore = g_vm_uses_tie ? 0 : 1;
            }
        }
        int fe_save_slot = -1;
        if (fe_restore) {
            /* stack: [array]; save the variable's current value */
            fe_save_slot = ctx_add_local(ctx, "__fe_save", '$');
            if (var_slot >= 0) { vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL); vm_chunk_emit_u16(ctx->chunk, (uint16_t)var_slot); }
            else if (var_cap >= 0) { vm_chunk_emit(ctx->chunk, OP_LOAD_CAPTURE); vm_chunk_emit_u16(ctx->chunk, (uint16_t)var_cap); }
            else { size_t gk0 = vm_chunk_add_str_const(ctx->chunk, var_gkey); vm_chunk_emit(ctx->chunk, OP_LOAD_GLOBAL); vm_chunk_emit_u16(ctx->chunk, (uint16_t)gk0); }
            emit_reset_store_local(ctx, fe_save_slot);
        }
        /* aliasing: an element of @array / @{...} is written back when the
         * body assigns the loop variable */
        StradaValue *fe_src = ast_deref(array_expr);
        int fe_src_t = (fe_src && !STRADA_IS_TAGGED_INT(fe_src)) ? (int)ast_int(fe_src, "type") : 0;
        const char *fe_src_sig = (fe_src_t == NI_VARIABLE || fe_src_t == NI_DEREF_SCALAR) ? ast_str(fe_src, "sigil") : NULL;
        int fe_alias = (fe_src_sig && fe_src_sig[0] == '@' && vm_node_writes_var(ast_get(node, "body"), vname, 0));
        int saved_fe_lc = ctx->local_count;
        ctx_push_scope(ctx, (void *)ast_deref(node), saved_fe_lc);

        int arr_slot = ctx_add_local(ctx, "__fe_arr", '$');
        emit_reset_store_local(ctx, arr_slot);

        /* Index */
        vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
        vm_chunk_emit_i64(ctx->chunk, 0);
        int idx_slot = ctx_add_local(ctx, "__fe_i", '$');
        emit_reset_store_local(ctx, idx_slot);

        /* Lexical loop variable shadows outer declarations and stays loop-scoped */
        if (is_lex_var) {
            var_slot = ctx_add_local(ctx, vname, '$');
            int vt = (int)ast_int(var_decl, "var_type");
            if (vt != 1 && vt != 2) vt = (int)ast_int(node, "var_type");
            if (var_slot >= 0 && var_slot < 256) {
                ctx->local_types[var_slot] = (vt == 1 || vt == 2) ? (uint8_t)vt : 0;
            }
        }

        LoopCtx lctx = {0};
        lctx.parent = ctx->loop_ctx;
        lctx.local_restore_depth = ctx->local_restore_count;
        lctx.break_local_count = ctx->local_count;
        lctx.next_local_count = ctx->local_count;
        lctx.redo_local_count = ctx->local_count;
        const char *felabel = ast_str(node, "label");
        if ((!felabel || !felabel[0]) && ctx->pending_loop_label && ctx->pending_loop_label[0]) {
            felabel = ctx->pending_loop_label;
            ctx->pending_loop_label = NULL;
        }
        if (felabel && felabel[0]) {
            lctx.label = felabel;
            VMLabelDef *lbl = vm_ctx_ensure_label(ctx, felabel);
            lbl->offset = ctx->chunk->code_len;
            lbl->has_offset = 1;
            lbl->actual_local_count = ctx->local_count;
            lbl->has_actual_lc = 1;
        }
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

        /* Reset lexical loop var slot per iteration so closures get distinct cells */
        if (is_lex_var) {
            vm_chunk_emit(ctx->chunk, OP_RESET_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)var_slot);
        }

        /* $var = $arr[$i] */
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)arr_slot);
        vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx_slot);
        vm_chunk_emit(ctx->chunk, OP_ARRAY_GET);
        if (var_slot >= 0) {
            if (var_slot < 256) emit_scalar_type_coerce(ctx, ctx->local_types[var_slot]);
            vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)var_slot);
        } else if (var_cap >= 0) {
            emit_scalar_type_coerce(ctx, ctx->captures[var_cap].var_type);
            vm_chunk_emit(ctx->chunk, OP_STORE_CAPTURE);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)var_cap);
        } else {
            size_t gk = vm_chunk_add_str_const(ctx->chunk, var_gkey);
            vm_chunk_emit(ctx->chunk, OP_STORE_GLOBAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)gk);
        }

        /* redo jumps here — body start */
        lctx.redo_target = ctx->chunk->code_len;

        compile_block(ctx, ast_get(node, "body"));

        /* Patch next jumps to increment */
        for (int i = 0; i < lctx.next_count; i++) {
            patch_u16(ctx, lctx.next_patches[i], (uint16_t)ctx->chunk->code_len);
        }

#define FE_WRITEBACK() do { \
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL); vm_chunk_emit_u16(ctx->chunk, (uint16_t)arr_slot); \
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL); vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx_slot); \
            if (var_slot >= 0) { vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL); vm_chunk_emit_u16(ctx->chunk, (uint16_t)var_slot); } \
            else if (var_cap >= 0) { vm_chunk_emit(ctx->chunk, OP_LOAD_CAPTURE); vm_chunk_emit_u16(ctx->chunk, (uint16_t)var_cap); } \
            else { size_t gkw = vm_chunk_add_str_const(ctx->chunk, var_gkey); vm_chunk_emit(ctx->chunk, OP_LOAD_GLOBAL); vm_chunk_emit_u16(ctx->chunk, (uint16_t)gkw); } \
            vm_chunk_emit(ctx->chunk, OP_BUILTIN); vm_chunk_emit_u16(ctx->chunk, BUILTIN_FE_WRITEBACK); vm_chunk_emit(ctx->chunk, 3); \
            vm_chunk_emit(ctx->chunk, OP_POP); \
        } while (0)
        if (fe_alias) FE_WRITEBACK();

        /* $i++ */
        vm_chunk_emit(ctx->chunk, OP_INCR);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)idx_slot);

        vm_chunk_emit(ctx->chunk, OP_JMP);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)loop_top);

        size_t fe_brk_target = ctx->chunk->code_len;
        if (fe_alias && lctx.break_count > 0) {
            /* `last`: write the current element back, then leave */
            FE_WRITEBACK();
        }
#undef FE_WRITEBACK
        size_t fe_after = ctx->chunk->code_len;
        (void)fe_after;

        patch_u16(ctx, exit_patch, (uint16_t)ctx->chunk->code_len);

        for (int i = 0; i < lctx.break_count; i++) {
            patch_u16(ctx, lctx.break_patches[i], (uint16_t)fe_brk_target);
        }
        if (fe_topic_lr >= 0) {
            while (ctx->local_restore_count > fe_topic_lr) {
                ctx->local_restore_count--;
                emit_local_restore(ctx->chunk, ctx->local_restores[ctx->local_restore_count]);
                free(ctx->local_restores[ctx->local_restore_count]);
                ctx->local_restores[ctx->local_restore_count] = NULL;
            }
        }
        if (fe_restore) {
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)fe_save_slot);
            if (var_slot >= 0) { vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL); vm_chunk_emit_u16(ctx->chunk, (uint16_t)var_slot); }
            else if (var_cap >= 0) { vm_chunk_emit(ctx->chunk, OP_STORE_CAPTURE); vm_chunk_emit_u16(ctx->chunk, (uint16_t)var_cap); }
            else { size_t gkr = vm_chunk_add_str_const(ctx->chunk, var_gkey); vm_chunk_emit(ctx->chunk, OP_STORE_GLOBAL); vm_chunk_emit_u16(ctx->chunk, (uint16_t)gkr); }
        }

        loop_free_patches(&lctx);
        ctx->loop_ctx = lctx.parent;
        ctx_pop_scope(ctx);
        emit_clear_locals_range(ctx->chunk, saved_fe_lc, ctx->local_count);
        ctx->local_count = saved_fe_lc;
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
            emit_clear_locals_range(ctx->chunk, target->break_local_count, ctx->local_count);
            vm_chunk_emit(ctx->chunk, OP_JMP);
            loop_add_break_patch(target, emit_patch_u16(ctx));
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
            emit_clear_locals_range(ctx->chunk, target->next_local_count, ctx->local_count);
            vm_chunk_emit(ctx->chunk, OP_JMP);
            loop_add_next_patch(target, emit_patch_u16(ctx));
        }
        break;
    }

    case NI_REDO: {
        const char *label = ast_str(node, "label");
        LoopCtx *target = ctx->loop_ctx;
        if (label[0]) {
            while (target && (!target->label || strcmp(target->label, label) != 0))
                target = target->parent;
        }
        if (target) {
            emit_try_unwind_for_loop(ctx, target);
            emit_clear_locals_range(ctx->chunk, target->redo_local_count, ctx->local_count);
            vm_chunk_emit(ctx->chunk, OP_JMP);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)target->redo_target);
        }
        break;
    }

    case NI_LABEL: {
        const char *label = ast_str(node, "name");
        if (label && label[0]) {
            VMLabelDef *lbl = vm_ctx_ensure_label(ctx, label);
            lbl->offset = ctx->chunk->code_len;
            lbl->has_offset = 1;
            lbl->actual_local_count = ctx->local_count;
            lbl->has_actual_lc = 1;
        }
        StradaValue *stmt = ast_get(node, "statement");
        if (stmt && !STRADA_IS_TAGGED_INT(stmt)) {
            StradaValue *inner = ast_deref(stmt);
            if (inner && !STRADA_IS_TAGGED_INT(inner) && inner->type == STRADA_HASH) {
                int inner_type = (int)ast_int(inner, "type");
                if (label && label[0] && (inner_type == NI_WHILE_STMT || inner_type == NI_FOR_STMT ||
                    inner_type == NI_FOREACH_STMT || inner_type == NI_DO_WHILE_STMT)) {
                    ctx->pending_loop_label = label;
                }
                compile_stmt(ctx, stmt);
                ctx->pending_loop_label = NULL;
            }
        }
        break;
    }

    case NI_GOTO: {
        const char *target = ast_str(node, "target");
        if (!target || !target[0]) target = ast_str(node, "label");
        VMLabelDef *lbl = (target && target[0]) ? vm_ctx_find_label(ctx, target) : NULL;
        if (!lbl) {
            fprintf(stderr, "VM compile error: unknown goto label '%s'\n", target ? target : "");
            ctx->errors++;
            break;
        }
        int cur_try_h = 0;
        for (TryCtx *tc = ctx->try_ctx; tc != NULL; tc = tc->parent) {
            cur_try_h += tc->handlers_active;
        }
        int same_ancestry = 1;
        if (lbl->try_handler_depth > cur_try_h || lbl->scope_depth > ctx->scope_depth) {
            same_ancestry = 0;
        } else {
            for (int si = 0; si < lbl->scope_depth && si < VM_MAX_GOTO_SCOPE; si++) {
                if (ctx->scope_nodes[si] != lbl->scope_path[si]) {
                    same_ancestry = 0;
                    break;
                }
            }
        }
        if (!same_ancestry) {
            fprintf(stderr, "VM compile error: cannot goto into inner scope or try block '%s'\n", target ? target : "");
            ctx->errors++;
            break;
        }
        if (cur_try_h > lbl->try_handler_depth || ctx->local_restore_count > lbl->local_restore_depth) {
            emit_try_unwind_to_depth(ctx, lbl->try_handler_depth, lbl->local_restore_depth);
        }
        int clear_from = ctx->local_count;
        if (lbl->has_actual_lc) {
            clear_from = lbl->actual_local_count;
        } else if (lbl->scope_depth < ctx->scope_depth) {
            clear_from = ctx->scope_saved_lc[lbl->scope_depth];
        }
        if (ctx->local_count > clear_from) {
            emit_clear_locals_range(ctx->chunk, clear_from, ctx->local_count);
        }
        vm_chunk_emit(ctx->chunk, OP_JMP);
        size_t poff = emit_patch_u16(ctx);
        if (ctx->goto_count >= ctx->goto_cap) {
            ctx->goto_cap = ctx->goto_cap ? ctx->goto_cap * 2 : 16;
            ctx->gotos = realloc(ctx->gotos, (size_t)ctx->goto_cap * sizeof(VMGotoPatch));
        }
        ctx->gotos[ctx->goto_count].name = strdup(target);
        ctx->gotos[ctx->goto_count].patch_off = poff;
        ctx->goto_count++;
        break;
    }

    case NI_SWITCH: {
        /* switch ($expr) { case X { ... } default { ... } }
         * AST: expr, cases (array of exprs), blocks (array of blocks), default_block */
        compile_expr(ctx, ast_get(node, "expr"));
        int val_slot = ctx_add_local(ctx, "__sw_val", '$');
        emit_reset_store_local(ctx, val_slot);

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
            StradaValue *ce_d = ast_deref(case_expr);
            if (ce_d && !STRADA_IS_TAGGED_INT(ce_d) &&
                (int)ast_int(ce_d, "type") == NI_STR_LITERAL &&
                ast_int(ce_d, "has_char_code")) {
                compile_expr(ctx, case_expr);
                vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
                vm_chunk_emit_i64(ctx->chunk, ast_int(ce_d, "char_code"));
                vm_chunk_emit(ctx->chunk, OP_BUILTIN);
                vm_chunk_emit_u16(ctx->chunk, BUILTIN_CASE_EQ_CHAR);
                vm_chunk_emit(ctx->chunk, 3);
            } else {
                compile_expr(ctx, case_expr);
                /* Use runtime type-aware equality so string variables/consts match as strings */
                vm_chunk_emit(ctx->chunk, OP_CASE_EQ);
            }
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
        tctx.local_restore_depth = ctx->local_restore_count;
        tctx.enclosing_scope_depth = ctx->scope_depth;
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

        int try_saved_lc = ctx->local_count;
        int saved_try_max = ctx->max_local_count;
        ctx->max_local_count = try_saved_lc;
        ctx->try_ctx = &tctx;
        compile_block(ctx, try_body);
        int try_max_lc = ctx->max_local_count;

        for (int h = 0; h < tctx.handlers_active; h++) {
            vm_chunk_emit(ctx->chunk, OP_TRY_END);
        }

        /* Jump over catch/rethrow handlers to normal finally / end */
        vm_chunk_emit(ctx->chunk, OP_JMP);
        size_t normal_end_patch = emit_patch_u16(ctx);

        size_t *catch_end_patches = (catch_count > 0) ? malloc((size_t)catch_count * sizeof(size_t)) : NULL;
        int catch_end_count = 0;

        if (catch_count > 0) {
            patch_u16(ctx, catch_patch, (uint16_t)ctx->chunk->code_len);
            emit_clear_locals_range(ctx->chunk, try_saved_lc, try_max_lc);
            /* Inside catch blocks: inner try handler was popped by OP_THROW;
             * outer finally_rethrow handler remains active when has_finally. */
            tctx.handlers_active = has_finally ? 1 : 0;

            int had_catch_all = 0;
            for (int i = 0; i < catch_count; i++) {
                StradaValue *cc = ast_deref(ast_arr_get(catches, i));
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

                    int saved_catch_lc = ctx->local_count;
                    if (cc) ctx_push_scope(ctx, (void *)cc, saved_catch_lc);
                    int slot = ctx_add_local(ctx, var_name[0] ? var_name : "__exc", '$');
                    vm_chunk_emit(ctx->chunk, OP_RESET_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                    vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);

                    compile_block(ctx, ast_get(cc, "catch_block"));
                    if (cc) ctx_pop_scope(ctx);
                    emit_clear_locals_range(ctx->chunk, saved_catch_lc, ctx->local_count);
                    ctx->local_count = saved_catch_lc;

                    if (has_finally) {
                        vm_chunk_emit(ctx->chunk, OP_TRY_END);
                    }
                    vm_chunk_emit(ctx->chunk, OP_JMP);
                    catch_end_patches[catch_end_count++] = emit_patch_u16(ctx);

                    patch_u16(ctx, next_catch, (uint16_t)ctx->chunk->code_len);
                } else {
                    /* Untyped catch-all */
                    int saved_catch_lc = ctx->local_count;
                    if (cc) ctx_push_scope(ctx, (void *)cc, saved_catch_lc);
                    int slot = ctx_add_local(ctx, var_name[0] ? var_name : "__exc", '$');
                    vm_chunk_emit(ctx->chunk, OP_RESET_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                    vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);

                    compile_block(ctx, ast_get(cc, "catch_block"));
                    if (cc) ctx_pop_scope(ctx);
                    emit_clear_locals_range(ctx->chunk, saved_catch_lc, ctx->local_count);
                    ctx->local_count = saved_catch_lc;

                    if (has_finally) {
                        vm_chunk_emit(ctx->chunk, OP_TRY_END);
                    }
                    vm_chunk_emit(ctx->chunk, OP_JMP);
                    catch_end_patches[catch_end_count++] = emit_patch_u16(ctx);

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

        int all_try_max_lc = ctx->max_local_count;
        if (saved_try_max > ctx->max_local_count) ctx->max_local_count = saved_try_max;
        ctx->try_ctx = tctx.parent;

        if (has_finally) {
            /* Exception unwind path: save pending exception, run finally, re-throw */
            patch_u16(ctx, finally_rethrow_patch, (uint16_t)ctx->chunk->code_len);
            emit_clear_locals_range(ctx->chunk, try_saved_lc, all_try_max_lc);
            int saved_fin_lc = ctx->local_count;
            int fin_exc_slot = ctx_add_local(ctx, "__fin_exc", '$');
            vm_chunk_emit(ctx->chunk, OP_RESET_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)fin_exc_slot);
            vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)fin_exc_slot);
            compile_block(ctx, finally_block);
            vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
            vm_chunk_emit_u16(ctx->chunk, (uint16_t)fin_exc_slot);
            emit_clear_locals_range(ctx->chunk, saved_fin_lc, ctx->local_count);
            ctx->local_count = saved_fin_lc;
            vm_chunk_emit(ctx->chunk, OP_THROW);
        }

        /* Normal exit path (try completed or catch handled): run finally if present */
        patch_u16(ctx, normal_end_patch, (uint16_t)ctx->chunk->code_len);
        for (int i = 0; i < catch_end_count; i++) {
            patch_u16(ctx, catch_end_patches[i], (uint16_t)ctx->chunk->code_len);
        }
        free(catch_end_patches);
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
        /* my ($a, $b, $c) = @array or ($a, $b) = @array */
        StradaValue *vars = ast_get(node, "vars");
        int var_count = (int)ast_int(node, "var_count");
        int is_decl = (int)ast_int(node, "is_decl");
        StradaValue *source = ast_get(node, "init");

        /* Compile source expression (a regex match is in list context) */
        if (!compile_regex_list(ctx, source)) compile_expr_in_ctx(ctx, source, 1);
        int src_slot = ctx_add_local(ctx, "__destr_src", '$');
        vm_chunk_emit(ctx->chunk, OP_RESET_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)src_slot);
        vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)src_slot);

        /* Extract each variable */
        for (int i = 0; i < var_count; i++) {
            StradaValue *var = ast_arr_get(vars, i);
            const char *vname = ast_str(var, "name");
            const char *vsigil_s = ast_str(var, "sigil");
            char vsigil = (vsigil_s && vsigil_s[0]) ? vsigil_s[0] : '$';

            if (vsigil == '%' && i == var_count - 1) {
                int tmp_lc = ctx->local_count;
                vm_chunk_emit(ctx->chunk, OP_NEW_HASH);
                vm_chunk_emit_u32(ctx->chunk, 16);
                int slurp_slot = ctx_add_local(ctx, "__destr_slurp", '$');
                vm_chunk_emit(ctx->chunk, OP_RESET_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)slurp_slot);
                vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)slurp_slot);

                vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
                vm_chunk_emit_i64(ctx->chunk, i);
                int j_slot = ctx_add_local(ctx, "__destr_j", '$');
                vm_chunk_emit(ctx->chunk, OP_RESET_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)j_slot);
                vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)j_slot);

                size_t loop_top = ctx->chunk->code_len;
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)j_slot);
                vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
                vm_chunk_emit_i64(ctx->chunk, 1);
                vm_chunk_emit(ctx->chunk, OP_ADD);
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)src_slot);
                vm_chunk_emit(ctx->chunk, OP_ARRAY_SIZE);
                vm_chunk_emit(ctx->chunk, OP_LT);
                vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
                size_t exit_patch = emit_patch_u16(ctx);

                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)slurp_slot);
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)src_slot);
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)j_slot);
                vm_chunk_emit(ctx->chunk, OP_ARRAY_GET);
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)src_slot);
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)j_slot);
                vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
                vm_chunk_emit_i64(ctx->chunk, 1);
                vm_chunk_emit(ctx->chunk, OP_ADD);
                vm_chunk_emit(ctx->chunk, OP_ARRAY_GET);
                vm_chunk_emit(ctx->chunk, OP_HASH_SET);

                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)j_slot);
                vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
                vm_chunk_emit_i64(ctx->chunk, 2);
                vm_chunk_emit(ctx->chunk, OP_ADD);
                vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)j_slot);

                vm_chunk_emit(ctx->chunk, OP_JMP);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)loop_top);

                patch_u16(ctx, exit_patch, (uint16_t)ctx->chunk->code_len);
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)slurp_slot);
                emit_clear_locals_range(ctx->chunk, tmp_lc, ctx->local_count);
                for (int k = tmp_lc; k < ctx->local_count; k++) {
                    if (ctx->local_names[k]) { free(ctx->local_names[k]); ctx->local_names[k] = NULL; }
                }
                ctx->local_count = tmp_lc;
            } else if (vsigil == '@') {
                int tmp_lc = ctx->local_count;
                vm_chunk_emit(ctx->chunk, OP_NEW_ARRAY);
                vm_chunk_emit_u32(ctx->chunk, 8);
                int slurp_slot = ctx_add_local(ctx, "__destr_slurp", '$');
                vm_chunk_emit(ctx->chunk, OP_RESET_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)slurp_slot);
                vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)slurp_slot);

                vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
                vm_chunk_emit_i64(ctx->chunk, i);
                int j_slot = ctx_add_local(ctx, "__destr_j", '$');
                vm_chunk_emit(ctx->chunk, OP_RESET_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)j_slot);
                vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)j_slot);

                size_t loop_top = ctx->chunk->code_len;
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)j_slot);
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)src_slot);
                vm_chunk_emit(ctx->chunk, OP_ARRAY_SIZE);
                vm_chunk_emit(ctx->chunk, OP_LT);
                vm_chunk_emit(ctx->chunk, OP_JMP_IF_FALSE);
                size_t exit_patch = emit_patch_u16(ctx);

                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)slurp_slot);
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)src_slot);
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)j_slot);
                vm_chunk_emit(ctx->chunk, OP_ARRAY_GET);
                vm_chunk_emit(ctx->chunk, OP_ARRAY_PUSH);

                vm_chunk_emit(ctx->chunk, OP_INCR);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)j_slot);

                vm_chunk_emit(ctx->chunk, OP_JMP);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)loop_top);

                patch_u16(ctx, exit_patch, (uint16_t)ctx->chunk->code_len);
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)slurp_slot);
                emit_clear_locals_range(ctx->chunk, tmp_lc, ctx->local_count);
                for (int k = tmp_lc; k < ctx->local_count; k++) {
                    if (ctx->local_names[k]) { free(ctx->local_names[k]); ctx->local_names[k] = NULL; }
                }
                ctx->local_count = tmp_lc;
            } else {
                vm_chunk_emit(ctx->chunk, OP_LOAD_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)src_slot);
                vm_chunk_emit(ctx->chunk, OP_PUSH_INT);
                vm_chunk_emit_i64(ctx->chunk, i);
                vm_chunk_emit(ctx->chunk, OP_ARRAY_GET);
            }

            int vt = (vsigil == '$') ? (int)ast_int(var, "var_type") : 0;
            uint8_t ltype = (vt == 1 || vt == 2) ? (uint8_t)vt : 0;
            if (is_decl != 0) {
                int slot = ctx_add_local(ctx, vname, vsigil);
                if (slot >= 0 && slot < 256) ctx->local_types[slot] = ltype;
                emit_scalar_type_coerce(ctx, ltype);
                vm_chunk_emit(ctx->chunk, OP_RESET_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
                vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
            } else {
                int slot = ctx_find_local_sigil(ctx, vname, vsigil);
                if (slot >= 0) {
                    if (slot < 256) emit_scalar_type_coerce(ctx, ctx->local_types[slot]);
                    vm_chunk_emit(ctx->chunk, OP_STORE_LOCAL);
                    vm_chunk_emit_u16(ctx->chunk, (uint16_t)slot);
                } else {
                    int cap = ctx_resolve_capture(ctx, vname, vsigil);
                    if (cap >= 0) {
                        emit_scalar_type_coerce(ctx, ctx->captures[cap].var_type);
                        vm_chunk_emit(ctx->chunk, OP_STORE_CAPTURE);
                        vm_chunk_emit_u16(ctx->chunk, (uint16_t)cap);
                    } else {
                        const char *gkey = resolve_global_var_key(ctx, vname);
                        if (gkey) {
                            size_t gk = vm_chunk_add_str_const(ctx->chunk, gkey);
                            vm_chunk_emit(ctx->chunk, OP_STORE_GLOBAL);
                            vm_chunk_emit_u16(ctx->chunk, (uint16_t)gk);
                        } else {
                            fprintf(stderr, "VM compile error: undeclared variable '%c%s' in destructuring assignment\n",
                                    vsigil, vname ? vname : "");
                            ctx->errors++;
                            vm_chunk_emit(ctx->chunk, OP_POP);
                        }
                    }
                }
            }
        }
        vm_chunk_emit(ctx->chunk, OP_RESET_LOCAL);
        vm_chunk_emit_u16(ctx->chunk, (uint16_t)src_slot);
        break;
    }

    case NI_REGEX_SUBST: {
        compile_expr(ctx, node);
        vm_chunk_emit(ctx->chunk, OP_POP);
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
        compile_void_expr(ctx, node);
        break;

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

    case NI_DO_EXPR: {
        compile_expr(ctx, node);
        vm_chunk_emit(ctx->chunk, OP_POP);
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
    int saved_lc = ctx->local_count;
    ctx_push_scope(ctx, (void *)block, saved_lc);
    for (int i = 0; i < count; i++) {
        compile_stmt(ctx, ast_arr_get(stmts, i));
    }
    ctx_pop_scope(ctx);
    while (ctx->local_restore_count > saved_lr) {
        ctx->local_restore_count--;
        char *lr_name = ctx->local_restores[ctx->local_restore_count];
        ctx->local_restores[ctx->local_restore_count] = NULL;
        emit_local_restore(ctx->chunk, lr_name);
        free(lr_name);
    }
    emit_clear_locals_range(ctx->chunk, saved_lc, ctx->local_count);
    ctx->local_count = saved_lc;
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
        vm_chunk_free_jit(chunk);
        free(chunk->code);
        free(chunk->dt_code);
        free(chunk->dt_aux);
        free(chunk->int_consts);
        for (size_t ci = 0; ci < chunk->str_const_count; ci++) {
            free(chunk->str_consts[ci]);
            /* the interned constant values too (as vm_program_free does):
             * leaked on every redefinition, e.g. each lib/Eval call that
             * re-declares a function */
            if (chunk->str_const_vals && VM_IS_PTR(chunk->str_const_vals[ci]))
                free((char*)VM_TO_PTR(chunk->str_const_vals[ci]) - offsetof(VMConstString, str));
        }
        free(chunk->str_consts);
        free(chunk->str_const_lens);
        free(chunk->str_const_vals);
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
    chunk->is_async = ((int)ast_int(func_node, "type") == NI_ASYNC_FUNC);
    int is_synth = (int)ast_int(func_node, "is_synthesized");
    StradaValue *params = ast_get(func_node, "params");
    int param_count = (int)ast_int(func_node, "param_count");
    int fixed_params = 0;
    for (int i = 0; i < param_count; i++) {
        StradaValue *param = ast_arr_get(params, i);
        const char *pname = ast_str(param, "name");
        const char *psigil = ast_str(param, "sigil");
        int is_variadic = (int)ast_int(param, "is_variadic");
        int pslot = ctx_add_local(&ctx, pname, psigil[0] ? psigil[0] : '$');
        if (pslot >= 0 && pslot < 256 && ast_str(param, "struct_name") && ast_str(param, "struct_name")[0])
            ctx.local_struct[pslot] = ast_str(param, "struct_name");
        if (pslot >= 0 && (!psigil[0] || psigil[0] == '$')) {
            int pt = (int)ast_int(param, "param_type");
            if (pt == 1 || pt == 2) ctx.local_types[pslot] = (uint8_t)pt;
        }
        if (is_variadic || (is_synth && psigil[0] == '@')) {
            chunk->has_variadic = 1;
        } else {
            fixed_params++;
        }
    }
    chunk->fixed_param_count = fixed_params;
    emit_param_defaults(&ctx, params, param_count, is_synth, (int)(chunk - prog->funcs));
    chunk = ctx.chunk;

    StradaValue *fn_body = ast_get(func_node, "body");
    ctx.may_box_locals = ast_may_box_locals(fn_body, 0) || ast_may_box_locals(params, 0);
    prescan_vm_labels(&ctx, fn_body, 0, 0);
    compile_block(&ctx, fn_body);

    /* Refresh chunk pointer — may have been invalidated by realloc during
     * closure compilation inside compile_block */
    chunk = ctx.chunk;

    /* Emit local restores before implicit return */
    for (int i = ctx.local_restore_count - 1; i >= 0; i--) {
        emit_local_restore(chunk, ctx.local_restores[i]);
    }

    resolve_vm_gotos(&ctx);

    /* Implicit return undef at end */
    vm_chunk_emit(chunk, OP_PUSH_UNDEF);
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
        ctx.errors++;
    }

    int final_lc = ctx.max_local_count > ctx.local_count ? ctx.max_local_count : ctx.local_count;
    chunk->local_count = final_lc;

    /* Detect int-only functions */
    int all_int = ctx.has_non_scalar ? 0 : 1;
    for (int i = 0; i < final_lc; i++) {
        if (ctx.local_sigils[i] != '$' && ctx.local_sigils[i] != '\0') {
            all_int = 0;
        }
        if (ctx.local_names[i]) free(ctx.local_names[i]);
    }
    for (int i = 0; i < ctx.capture_count; i++) free(ctx.captures[i].name);
    ctx_free_local_restores(&ctx);
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


static void collect_vm_mro_rec(VMProgram *prog, const char *cls, const char **out, int max, int *n) {
    if (!cls || !cls[0] || *n >= max) return;
    for (int i = 0; i < *n; i++) {
        if (strcmp(out[i], cls) == 0) return;
    }
    out[(*n)++] = cls;
    for (int i = 0; i < prog->inherit_count; i++) {
        if (strcmp(prog->inherits[i].child, cls) == 0) {
            collect_vm_mro_rec(prog, prog->inherits[i].parent, out, max, n);
        }
    }
}

static int find_func_before(VMProgram *prog, const char *name, int limit) {
    for (int i = 0; i < limit && (size_t)i < prog->func_count; i++) {
        if (prog->func_names[i] && strcmp(prog->func_names[i], name) == 0) return i;
    }
    return -1;
}

static int find_vm_method_impl(VMProgram *prog, const char *cls, const char *method, int max_count) {
    char cand[280];
    snprintf(cand, sizeof(cand), "%s_%s", cls, method);
    int idx = (max_count >= 0) ? find_func_before(prog, cand, max_count) : vm_program_find_func(prog, cand);
    if (idx >= 0) return idx;
    snprintf(cand, sizeof(cand), "%s::%s", cls, method);
    idx = (max_count >= 0) ? find_func_before(prog, cand, max_count) : vm_program_find_func(prog, cand);
    if (idx >= 0) return idx;
    if (strchr(cls, ':')) {
        char sanitized[280];
        int si = 0;
        snprintf(cand, sizeof(cand), "%s_%s", cls, method);
        for (int fi = 0; cand[fi] && si < 278; fi++) {
            if (cand[fi] == ':' && cand[fi + 1] == ':') {
                sanitized[si++] = '_';
                fi++;
            } else {
                sanitized[si++] = cand[fi];
            }
        }
        sanitized[si] = '\0';
        idx = (max_count >= 0) ? find_func_before(prog, sanitized, max_count) : vm_program_find_func(prog, sanitized);
        if (idx >= 0) return idx;
    }
    return -1;
}

typedef struct {
    const char *cls;
    const char *method;
} ModWrapTarget;

/* Synthesize before/after modifier wrappers using full MRO and a two-pass
 * rename-then-wrap scheme so inheritance order never causes nested double-wrapping. */
static void synthesize_modifier_wrappers(VMProgram *prog) {
    int mod_total = prog->modifier_count;
    if (mod_total <= 0) return;

    const char **mod_methods = malloc((size_t)mod_total * sizeof(const char*));
    int mm_count = 0;
    for (int m = 0; m < mod_total; m++) {
        const char *mn = prog->modifiers[m].method;
        int dup = 0;
        for (int j = 0; j < mm_count; j++) {
            if (strcmp(mod_methods[j], mn) == 0) { dup = 1; break; }
        }
        if (!dup) mod_methods[mm_count++] = mn;
    }
    if (mm_count == 0) { free(mod_methods); return; }

    int cls_cap = mod_total + prog->inherit_count * 2 + 8;
    const char **classes = malloc((size_t)cls_cap * sizeof(const char*));
    int cls_count = 0;
    for (int m = 0; m < mod_total; m++) {
        const char *c = prog->modifiers[m].class_name;
        int dup = 0;
        for (int j = 0; j < cls_count; j++) if (strcmp(classes[j], c) == 0) { dup = 1; break; }
        if (!dup) classes[cls_count++] = c;
    }
    for (int i = 0; i < prog->inherit_count; i++) {
        const char *pair[2] = { prog->inherits[i].child, prog->inherits[i].parent };
        for (int p = 0; p < 2; p++) {
            int dup = 0;
            for (int j = 0; j < cls_count; j++) if (strcmp(classes[j], pair[p]) == 0) { dup = 1; break; }
            if (!dup) classes[cls_count++] = pair[p];
        }
    }

    int tgt_cap = cls_count * mm_count + 8;
    ModWrapTarget *targets = malloc((size_t)tgt_cap * sizeof(ModWrapTarget));
    int tgt_count = 0;
    for (int ci = 0; ci < cls_count; ci++) {
        const char *cls = classes[ci];
        const char *mro[64];
        int mro_n = 0;
        collect_vm_mro_rec(prog, cls, mro, 64, &mro_n);
        for (int mi = 0; mi < mm_count; mi++) {
            const char *method = mod_methods[mi];
            int has_mod = 0, has_impl = 0;
            for (int k = 0; k < mro_n; k++) {
                for (int m = 0; m < mod_total; m++) {
                    if (strcmp(prog->modifiers[m].class_name, mro[k]) == 0 &&
                        strcmp(prog->modifiers[m].method, method) == 0) {
                        has_mod = 1;
                        break;
                    }
                }
                if (find_vm_method_impl(prog, mro[k], method, -1) >= 0) has_impl = 1;
            }
            if (has_mod && has_impl) {
                targets[tgt_count].cls = cls;
                targets[tgt_count].method = method;
                tgt_count++;
            }
        }
    }

    int pre_wrap_count = prog->func_count;

    /* Generate MRO-complete __modwrap wrappers so static OP_CALL and SUPER:: still hit unwrapped targets */
    for (int t = 0; t < tgt_count; t++) {
        const char *cls = targets[t].cls;
        const char *method = targets[t].method;
        char wrap_name[280];
        snprintf(wrap_name, sizeof(wrap_name), "%s_%s__modwrap", cls, method);
        if (vm_program_find_func(prog, wrap_name) >= 0) continue;

        const char *mro[64];
        int mro_n = 0;
        collect_vm_mro_rec(prog, cls, mro, 64, &mro_n);

        int raw_orig = -1;
        for (int k = 0; k < mro_n && raw_orig < 0; k++) {
            raw_orig = find_vm_method_impl(prog, mro[k], method, pre_wrap_count);
        }
        if (raw_orig < 0) continue;

        vm_program_add_func(prog, wrap_name);
        int w_idx = (int)(prog->func_count - 1);

        /* around hooks across MRO (base-first, registration order per package);
         * around_fidxs[around_n - 1] is outermost and around_fidxs[0] wraps raw_orig directly. */
        int around_fidxs[64];
        int around_n = 0;
        for (int k = mro_n - 1; k >= 0; k--) {
            for (int m = 0; m < mod_total && around_n < 64; m++) {
                VMModifier *am = &prog->modifiers[m];
                if (am->kind != 2 || strcmp(am->method, method) != 0 ||
                    strcmp(am->class_name, mro[k]) != 0) continue;
                int afidx = vm_program_find_func(prog, am->modifier_func);
                if (afidx >= 0) around_fidxs[around_n++] = afidx;
            }
        }

        int chain_idx[64];
        for (int ai = 0; ai < around_n; ai++) {
            char cname[320];
            snprintf(cname, sizeof(cname), "%s_%s__around_chain_%d", cls, method, ai);
            VMChunk *ac = vm_program_add_func(prog, cname);
            chain_idx[ai] = (int)(prog->func_count - 1);
            ac->fixed_param_count = 0;
            ac->has_variadic = 1;
            ac->local_count = 2;
            int inner_idx = (ai == 0) ? raw_orig : chain_idx[ai - 1];
            vm_chunk_emit(ac, OP_NEW_ARRAY); vm_chunk_emit_u32(ac, 4);
            vm_chunk_emit(ac, OP_STORE_LOCAL); vm_chunk_emit_u16(ac, 1);
            vm_chunk_emit(ac, OP_LOAD_LOCAL); vm_chunk_emit_u16(ac, 1);
            vm_chunk_emit(ac, OP_MAKE_CLOSURE); vm_chunk_emit_u16(ac, (uint16_t)inner_idx); vm_chunk_emit(ac, 0);
            vm_chunk_emit(ac, OP_ARRAY_PUSH);
            vm_chunk_emit(ac, OP_LOAD_LOCAL); vm_chunk_emit_u16(ac, 1);
            vm_chunk_emit(ac, OP_LOAD_LOCAL); vm_chunk_emit_u16(ac, 0);
            vm_chunk_emit(ac, OP_ARRAY_EXTEND);
            vm_chunk_emit(ac, OP_LOAD_LOCAL); vm_chunk_emit_u16(ac, 1);
            vm_chunk_emit(ac, OP_SET_CALL_CTX); vm_chunk_emit(ac, 255);
            vm_chunk_emit(ac, OP_CALL_SPREAD); vm_chunk_emit_u16(ac, (uint16_t)around_fidxs[ai]);
            vm_chunk_emit(ac, OP_RETURN);
        }

        VMChunk *w = &prog->funcs[w_idx];
        w->fixed_param_count = 0;
        w->has_variadic = 1;
        w->local_count = 2; /* slot 0 = packed args [$self, ...], slot 1 = result */

        /* before hooks across MRO (derived-first, registration order per package) */
        for (int k = 0; k < mro_n; k++) {
            for (int m = 0; m < mod_total; m++) {
                VMModifier *bm = &prog->modifiers[m];
                if (bm->kind != 0 || strcmp(bm->method, method) != 0 ||
                    strcmp(bm->class_name, mro[k]) != 0) continue;
                int bidx = vm_program_find_func(prog, bm->modifier_func);
                if (bidx < 0) continue;
                if (!prog->funcs[bidx].has_variadic && prog->funcs[bidx].fixed_param_count == 0) {
                    vm_chunk_emit(w, OP_CALL); vm_chunk_emit_u16(w, (uint16_t)bidx); vm_chunk_emit(w, 0);
                } else {
                    vm_chunk_emit(w, OP_LOAD_LOCAL); vm_chunk_emit_u16(w, 0);
                    vm_chunk_emit(w, OP_CALL_SPREAD); vm_chunk_emit_u16(w, (uint16_t)bidx);
                }
                vm_chunk_emit(w, OP_POP);
            }
        }

        /* outermost around chain (or unwrapped original) */
        if (around_n > 0) {
            vm_chunk_emit(w, OP_LOAD_LOCAL); vm_chunk_emit_u16(w, 0);
            vm_chunk_emit(w, OP_SET_CALL_CTX); vm_chunk_emit(w, 255);
            vm_chunk_emit(w, OP_CALL_SPREAD); vm_chunk_emit_u16(w, (uint16_t)chain_idx[around_n - 1]);
        } else if (!prog->funcs[raw_orig].has_variadic && prog->funcs[raw_orig].fixed_param_count == 0) {
            vm_chunk_emit(w, OP_SET_CALL_CTX); vm_chunk_emit(w, 255);
            vm_chunk_emit(w, OP_CALL); vm_chunk_emit_u16(w, (uint16_t)raw_orig); vm_chunk_emit(w, 0);
        } else {
            vm_chunk_emit(w, OP_LOAD_LOCAL); vm_chunk_emit_u16(w, 0);
            vm_chunk_emit(w, OP_SET_CALL_CTX); vm_chunk_emit(w, 255);
            vm_chunk_emit(w, OP_CALL_SPREAD); vm_chunk_emit_u16(w, (uint16_t)raw_orig);
        }
        vm_chunk_emit(w, OP_STORE_LOCAL); vm_chunk_emit_u16(w, 1);

        /* after hooks across MRO (derived-first, registration order per package) */
        for (int k = 0; k < mro_n; k++) {
            for (int m = 0; m < mod_total; m++) {
                VMModifier *am = &prog->modifiers[m];
                if (am->kind != 1 || strcmp(am->method, method) != 0 ||
                    strcmp(am->class_name, mro[k]) != 0) continue;
                int aidx = vm_program_find_func(prog, am->modifier_func);
                if (aidx < 0) continue;
                if (!prog->funcs[aidx].has_variadic && prog->funcs[aidx].fixed_param_count == 0) {
                    vm_chunk_emit(w, OP_CALL); vm_chunk_emit_u16(w, (uint16_t)aidx); vm_chunk_emit(w, 0);
                } else {
                    vm_chunk_emit(w, OP_LOAD_LOCAL); vm_chunk_emit_u16(w, 0);
                    vm_chunk_emit(w, OP_CALL_SPREAD); vm_chunk_emit_u16(w, (uint16_t)aidx);
                }
                vm_chunk_emit(w, OP_POP);
            }
        }

        vm_chunk_emit(w, OP_LOAD_LOCAL); vm_chunk_emit_u16(w, 1);
        vm_chunk_emit(w, OP_RETURN);
    }

    free(targets);
    free(classes);
    free(mod_methods);
}

/* ===== Scalar-context pass (mirrors the compiler's Semantic rewrites) =====
 * The VM keeps @a and [..] as the same kind of value, so an array/hash value
 * reaching a scalar operation is treated as a reference (address, always
 * true, ARRAY(0x..)). Containers that Strada evaluates for their COUNT are
 * rewritten to scalar(...) here, before compilation, exactly where the
 * compiled backend counts them: operands of arithmetic, comparison and
 * string operators (@a, %h, @{..}, %{..}, map/grep), and `$x = @a` /
 * `my $x = @a` (an array variable assigned to a scalar variable). */
static int vm_sc_is_container(StradaValue *n, int with_hash) {
    n = ast_deref(n);
    if (!n || STRADA_IS_TAGGED_INT(n) || n->type != STRADA_HASH) return 0;
    int t = (int)ast_int(n, "type");
    if (t == NI_VARIABLE || t == NI_DEREF_SCALAR) {
        const char *sg = ast_str(n, "sigil");
        if (sg[0] == '@') return 1;
        if (with_hash && sg[0] == '%') return 1;
        return 0;
    }
    if (t == NI_MAP || t == NI_GREP) return 1;
    return 0;
}
/* keys/values/split assigned to a scalar variable: their count (Perl), as
 * the compiler's sem_is_count_in_scalar_call */
static int vm_sc_is_count_call(StradaValue *n) {
    n = ast_deref(n);
    if (!n || STRADA_IS_TAGGED_INT(n) || n->type != STRADA_HASH || (int)ast_int(n, "type") != NI_CALL) return 0;
    const char *cn = ast_str(n, "name");
    return cn && (strcmp(cn, "keys") == 0 || strcmp(cn, "values") == 0 || strcmp(cn, "split") == 0 ||
                  strcmp(cn, "sys::split") == 0);
}
static StradaValue *vm_sc_wrap(StradaValue *child) {
    StradaValue *node = strada_new_hash();
    strada_hash_set_take(node->value.hv, "type", strada_new_int(NI_CALL));
    strada_hash_set_take(node->value.hv, "name", strada_new_str("scalar"));
    StradaValue *args = strada_new_array();
    strada_incref(child);
    strada_array_push_take(args->value.av, child);
    strada_hash_set_take(node->value.hv, "args", strada_ref_create_take(args));
    strada_hash_set_take(node->value.hv, "arg_count", strada_new_int(1));
    StradaValue *ln = ast_get(child, "line");
    if (ln) { strada_incref(ln); strada_hash_set_take(node->value.hv, "line", ln); }
    return node;
}
static void vm_sc_replace(StradaValue *node, const char *key) {
    StradaValue *child = ast_get(node, key);
    if (!child) return;
    StradaValue *w = vm_sc_wrap(child);
    strada_hash_set_take(ast_deref(node)->value.hv, key, w);
}
static void vm_sc_pass(StradaValue *v, int depth) {
    v = ast_deref(v);
    if (!v || STRADA_IS_TAGGED_INT(v) || depth > 4000) return;
    if (v->type == STRADA_ARRAY) {
        StradaArray *av = v->value.av;
        for (size_t i = 0; av && i < av->size; i++) vm_sc_pass(av->elements[av->head + i], depth + 1);
        return;
    }
    if (v->type != STRADA_HASH) return;
    StradaValue *tv = strada_hash_get(v->value.hv, "type");
    if (tv) {
        int t = (int)ast_int(v, "type");
        if (t == NI_BINARY_OP) {
            const char *op = ast_str(v, "op");
            static const char *ops[] = { "+", "-", "*", "/", "%", "**", "==", "!=", "<", "<=", ">", ">=", "<=>",
                                         ".", "eq", "ne", "lt", "gt", "le", "ge", "cmp", NULL };
            int hit = 0;
            for (int k = 0; ops[k]; k++) if (strcmp(op, ops[k]) == 0) { hit = 1; break; }
            if (hit) {
                if (vm_sc_is_container(ast_get(v, "left"), 1)) vm_sc_replace(v, "left");
                if (vm_sc_is_container(ast_get(v, "right"), 1)) vm_sc_replace(v, "right");
            }
        } else if (t == NI_VAR_DECL || t == NI_OUR_DECL) {
            if (ast_str(v, "sigil")[0] == '$' && (vm_sc_is_container(ast_get(v, "init"), 0) ||
                                                   vm_sc_is_count_call(ast_get(v, "init")))) vm_sc_replace(v, "init");
        } else if (t == NI_ASSIGN) {
            StradaValue *tg = ast_deref(ast_get(v, "target"));
            if (strcmp(ast_str(v, "op"), "=") == 0 && tg && !STRADA_IS_TAGGED_INT(tg)
                && (int)ast_int(tg, "type") == NI_VARIABLE && ast_str(tg, "sigil")[0] == '$'
                && (vm_sc_is_container(ast_get(v, "value"), 0) || vm_sc_is_count_call(ast_get(v, "value")))) {
                vm_sc_replace(v, "value");
            } else if (strcmp(ast_str(v, "op"), "=") == 0 && tg && !STRADA_IS_TAGGED_INT(tg)
                       && vm_sc_is_count_call(ast_get(v, "value"))) {
                /* $h{n} = keys(%h) / $a[0] = split(...): a count (Perl) */
                int tgt = (int)ast_int(tg, "type");
                if (tgt == NI_HASH_ACCESS || tgt == NI_SUBSCRIPT || tgt == NI_DEREF_HASH || tgt == NI_DEREF_ARRAY)
                    vm_sc_replace(v, "value");
            }
        }
    }
    StradaHash *hv = v->value.hv;
    for (uint32_t i = 0; hv && i < hv->next_slot; i++) {
        StradaValue *c = hv->entries[i].value;
        if (!hv->entries[i].key || !c || STRADA_IS_TAGGED_INT(c)) continue;
        if (c->type == STRADA_REF || c->type == STRADA_HASH || c->type == STRADA_ARRAY) vm_sc_pass(c, depth + 1);
    }
}

VMProgram *vm_compile_program(StradaValue *ast) {
    g_vm_structs = ast_get(ast, "structs");
    /* link_lib "z"; ... : libraries the program's __C__ blocks need */
    {
        StradaValue *ll = ast_deref(ast_get(ast, "link_libs"));
        if (ll && !STRADA_IS_TAGGED_INT(ll) && ll->type == STRADA_ARRAY && ll->value.av->size > 0) {
            size_t cap = 16, len = 0;
            char *fl = malloc(cap);
            fl[0] = 0;
            for (size_t i = 0; i < ll->value.av->size; i++) {
                StradaValue *nm = strada_array_get(ll->value.av, (int64_t)i);
                if (!nm || STRADA_IS_TAGGED_INT(nm) || nm->type != STRADA_STR || !nm->value.pv) continue;
                const char *n = nm->value.pv;
                int safe = n[0] != 0;
                for (const char *c = n; *c; c++)
                    if (!((*c >= 'a' && *c <= 'z') || (*c >= 'A' && *c <= 'Z') || (*c >= '0' && *c <= '9') || *c == '_' || *c == '-' || *c == '.' || *c == '+')) safe = 0;
                if (!safe) continue;
                size_t need = len + strlen(n) + 4;
                if (need >= cap) { cap = need * 2; fl = realloc(fl, cap); }
                len += (size_t)sprintf(fl + len, " -l%s", n);
            }
            prog_link_flags_pending = fl;
        }
    }
    g_vm_uses_tie = vm_ast_uses_tie(ast_get(ast, "functions"), 0);
    vm_sc_pass(ast, 0);
    reset_compiler_globals();
    VMProgram *prog = vm_program_new();
    prog->cblock_link_flags = prog_link_flags_pending;
    prog_link_flags_pending = NULL;
    VM *saved_vm = NULL;
    vm_gc_set_compiling_prog(prog, &saved_vm);

    StradaValue *funcs = ast_get(ast, "functions");
    int func_count = (int)ast_int(ast, "function_count");

    StradaValue *inherits = ast_get(ast, "inherits");

    int total_errors = 0;

    /* Process import_lib declarations — must be done before function compilation
     * so that calls to imported functions can be resolved */
    total_errors += process_import_libs(prog, ast);

    {
        const char *sf = ast_str(ast, "source_file");
        if (!sf || !sf[0]) sf = ast_str(ast, "filename");
        g_source_file = strdup((sf && sf[0]) ? sf : "(eval)");
    }

    /* Register enum constants before compiling globals, BEGIN blocks, and functions */
    StradaValue *enums_arr = ast_get(ast, "enums");
    int enum_count = (int)ast_int(ast, "enum_count");
    for (int ei = 0; ei < enum_count; ei++) {
        StradaValue *en = ast_arr_get(enums_arr, ei);
        const char *ename = ast_str(en, "name");
        StradaValue *members = ast_get(en, "members");
        int mcount = (int)ast_int(en, "member_count");
        if (!ename || !ename[0]) continue;
        char norm_en[256];
        size_t nj = 0;
        for (size_t k = 0; ename[k] && nj < sizeof(norm_en) - 1; k++) {
            if (ename[k] == ':' && ename[k + 1] == ':') {
                norm_en[nj++] = '_';
                k++;
            } else {
                norm_en[nj++] = ename[k];
            }
        }
        norm_en[nj] = '\0';
        for (int mi = 0; mi < mcount; mi++) {
            StradaValue *mem = ast_arr_get(members, mi);
            const char *mname = ast_str(mem, "name");
            int64_t mval = ast_int(mem, "value");
            if (!mname || !mname[0]) continue;
            char k1[512], k2[512];
            snprintf(k1, sizeof(k1), "%s::%s", ename, mname);
            snprintf(k2, sizeof(k2), "%s_%s", norm_en, mname);
            register_enum_val(k1, mval);
            register_enum_val(k2, mval);
        }
    }

    /* First pass: register all function names and signatures before globals,
     * BEGIN/END blocks, and functions compile so forward references work everywhere */
    for (int i = 0; i < func_count; i++) {
        StradaValue *func = ast_arr_get(funcs, i);
        const char *fname = ast_str(func, "name");
        int existing = vm_program_find_func(prog, fname);
        VMChunk *fc = (existing < 0) ? vm_program_add_func(prog, fname) : &prog->funcs[existing];
        int fidx_pass1 = (existing < 0) ? (int)(prog->func_count - 1) : existing;
        const char *fpkg = ast_str(func, "package");
        if (fpkg && fpkg[0] && strcmp(fpkg, "main") != 0) {
            const char *fbare = ast_str(func, "bare_name");
            if (fbare && fbare[0] && strcmp(fbare, fname) != 0) {
                register_pkg_func_alias(fbare, fidx_pass1);
            } else {
                size_t plen = strlen(fpkg);
                if (fname && strlen(fname) > plen + 1 && strncmp(fname, fpkg, plen) == 0 &&
                    fname[plen] == '_') {
                    register_pkg_func_alias(fname + plen + 1, fidx_pass1);
                }
            }
        }
        fc->return_type = (int)ast_int(func, "return_type");
        fc->is_async = ((int)ast_int(func, "type") == NI_ASYNC_FUNC);
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

    /* Process global (our) declarations */
    StradaValue *globals = ast_get(ast, "globals");
    int global_count = (int)ast_int(ast, "global_count");
    if (global_count > 0) {
        /* Create an init function for globals */
        char iname[] = "__globals_init";
        VMChunk *ichunk = vm_program_add_func(prog, iname);
        int ifunc_idx = (int)(prog->func_count - 1);
        CompCtx ictx = {0};
        ictx.chunk = ichunk;
        ictx.prog = prog;
        ictx.func_name = iname;
        int icur_ld = 0;
        for (int i = 0; i < global_count; i++) {
            icur_ld = prescan_vm_labels(&ictx, ast_arr_get(globals, i), 0, icur_ld);
        }
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
            ictx.chunk = &prog->funcs[ifunc_idx];
            compile_stmt(&ictx, gdecl);
        }
        ichunk = &prog->funcs[ifunc_idx]; /* refresh after potential realloc */
        ictx.chunk = ichunk;
        resolve_vm_gotos(&ictx);
        vm_chunk_emit(ichunk, OP_PUSH_INT);
        vm_chunk_emit_i64(ichunk, 0);
        vm_chunk_emit(ichunk, OP_RETURN);
        if (ichunk->code_len > 0xFFFF) {
            fprintf(stderr, "VM compile error: __globals_init compiles to %zu bytes of bytecode, "
                            "exceeding the VM backend's 65535-byte limit\n",
                    (size_t)ichunk->code_len);
            ictx.errors++;
        }
        total_errors += ictx.errors;
        int ifinal_lc = ictx.max_local_count > ictx.local_count ? ictx.max_local_count : ictx.local_count;
        ichunk->local_count = ifinal_lc;
        for (int j = 0; j < ifinal_lc; j++) if (ictx.local_names[j]) free(ictx.local_names[j]);
        for (int j = 0; j < ictx.capture_count; j++) free(ictx.captures[j].name);
        ctx_free_local_restores(&ictx);

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
        int bfunc_idx = (int)(prog->func_count - 1);
        CompCtx bctx = {0};
        bctx.chunk = bchunk;
        bctx.prog = prog;
        bctx.func_name = bname;
        bctx.cur_package = ast_str(block, "source_module");
        prescan_vm_labels(&bctx, block, 0, 0);
        compile_block(&bctx, block);
        bchunk = &prog->funcs[bfunc_idx]; /* refresh after potential realloc */
        bctx.chunk = bchunk;
        resolve_vm_gotos(&bctx);
        vm_chunk_emit(bchunk, OP_PUSH_INT);
        vm_chunk_emit_i64(bchunk, 0);
        vm_chunk_emit(bchunk, OP_RETURN);
        if (bchunk->code_len > 0xFFFF) {
            fprintf(stderr, "VM compile error: BEGIN block '%s' compiles to %zu bytes of bytecode, "
                            "exceeding the VM backend's 65535-byte limit\n",
                    bname, (size_t)bchunk->code_len);
            bctx.errors++;
        }
        total_errors += bctx.errors;
        int bfinal_lc = bctx.max_local_count > bctx.local_count ? bctx.max_local_count : bctx.local_count;
        bchunk->local_count = bfinal_lc;
        for (int j = 0; j < bfinal_lc; j++) if (bctx.local_names[j]) free(bctx.local_names[j]);
        for (int j = 0; j < bctx.capture_count; j++) free(bctx.captures[j].name);
        ctx_free_local_restores(&bctx);

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
        int efunc_idx = (int)(prog->func_count - 1);
        CompCtx ectx = {0};
        ectx.chunk = echunk;
        ectx.prog = prog;
        ectx.func_name = ename;
        ectx.cur_package = ast_str(block, "source_module");
        prescan_vm_labels(&ectx, block, 0, 0);
        compile_block(&ectx, block);
        echunk = &prog->funcs[efunc_idx]; /* refresh after potential realloc */
        ectx.chunk = echunk;
        resolve_vm_gotos(&ectx);
        vm_chunk_emit(echunk, OP_PUSH_INT);
        vm_chunk_emit_i64(echunk, 0);
        vm_chunk_emit(echunk, OP_RETURN);
        if (echunk->code_len > 0xFFFF) {
            fprintf(stderr, "VM compile error: END block '%s' compiles to %zu bytes of bytecode, "
                            "exceeding the VM backend's 65535-byte limit\n",
                    ename, (size_t)echunk->code_len);
            ectx.errors++;
        }
        total_errors += ectx.errors;
        int efinal_lc = ectx.max_local_count > ectx.local_count ? ectx.max_local_count : ectx.local_count;
        echunk->local_count = efinal_lc;
        for (int j = 0; j < efinal_lc; j++) if (ectx.local_names[j]) free(ectx.local_names[j]);
        for (int j = 0; j < ectx.capture_count; j++) free(ectx.captures[j].name);
        ctx_free_local_restores(&ectx);

        int fidx = vm_program_find_func(prog, ename);
        prog->end_blocks = realloc(prog->end_blocks, (prog->end_count + 1) * sizeof(int));
        prog->end_blocks[prog->end_count++] = fidx;
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

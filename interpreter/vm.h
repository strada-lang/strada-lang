/* strada_vm.h — Bytecode VM for Strada interpreter
 * Full language support: closures, try/catch, OOP, regex, I/O, etc. */

#ifndef STRADA_VM_H
#define STRADA_VM_H

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

/* Opcodes */
typedef enum {
    OP_NOP = 0,
    OP_PUSH_INT,        /* push immediate int64 */
    OP_PUSH_STR,        /* push string constant (index into string pool) */
    OP_PUSH_UNDEF,      /* push undef */
    OP_POP,             /* discard top of stack */
    OP_DUP,             /* duplicate top of stack */

    OP_LOAD_LOCAL,      /* push local variable by slot index */
    OP_STORE_LOCAL,     /* pop and store into local variable slot */

    OP_ADD,             /* pop 2, push sum */
    OP_SUB,             /* pop 2, push difference */
    OP_MUL,             /* pop 2, push product */
    OP_DIV,             /* pop 2, push quotient */
    OP_MOD,             /* pop 2, push remainder */

    OP_CONCAT,          /* pop 2 strings, push concatenated */

    OP_EQ,              /* pop 2, push 1 if equal (numeric) */
    OP_NE,
    OP_LT,
    OP_GT,
    OP_LE,
    OP_GE,

    OP_STR_EQ,          /* string comparisons */
    OP_STR_NE,

    OP_JMP,             /* unconditional jump (u16 offset) */
    OP_JMP_IF_FALSE,    /* pop, jump if falsy */
    OP_JMP_IF_TRUE,     /* pop, jump if truthy (for ||) */

    OP_CALL,            /* call function (u16 func_index, u8 arg_count) */
    OP_RETURN,          /* return top of stack */

    OP_SAY,             /* pop and print with newline */
    OP_PRINT,           /* pop and print without newline */

    OP_INCR,            /* increment local variable in-place */
    OP_DECR,            /* decrement local variable in-place */

    /* Array operations */
    OP_NEW_ARRAY,       /* push empty array (u32 capacity hint follows) */
    OP_ARRAY_PUSH,      /* pop value, pop array, push to array */
    OP_ARRAY_GET,       /* pop index, pop array, push element */
    OP_ARRAY_SET,       /* pop value, pop index, pop array, set element */
    OP_ARRAY_SIZE,      /* pop array, push int size */

    /* Hash operations */
    OP_NEW_HASH,        /* push empty hash (u32 capacity hint follows) */
    OP_HASH_GET,        /* pop key(str), pop hash, push value */
    OP_HASH_SET,        /* pop value, pop key(str), pop hash, set key=value */
    OP_HASH_DELETE,     /* pop key(str), pop hash, delete key */
    OP_HASH_KEYS,       /* pop hash, push array of keys (as strings) */

    /* String operations */
    OP_STR_LEN,         /* pop string, push int length */
    OP_STR_SPLIT,       /* pop str, pop delimiter, push array */
    OP_STR_SPLIT_LIMIT, /* pop str, pop delimiter, pop limit(int), push array */
    OP_STR_REPLACE,     /* pop replacement, pop pattern, pop str, push new str */

    /* OOP */
    OP_HASH_BLESS,      /* pop class_name(str), pop hash, set __class__, push hash */
    OP_HASH_REF,        /* pop hash, wrap in ref (hash becomes object) */

    /* Defined-or */
    OP_IS_UNDEF,        /* pop, push 1 if undef, 0 otherwise */

    OP_ISA,             /* pop class_name(str), pop object(hash), push 1 if isa, 0 otherwise */

    OP_APPEND_LOCAL,    /* pop value, append to string in local[u16] in-place (StringBuilder) */

    /* Fused hash ops with concat key */
    OP_HASH_GET_CONCAT,
    OP_HASH_SET_CONCAT,
    OP_HASH_DEL_CONCAT,

    OP_HALT,            /* stop execution */

    /* === Phase 2+ opcodes === */

    /* Builtins dispatch */
    OP_BUILTIN,         /* u16 builtin_id, u8 argc — generic builtin call */

    /* Closures */
    OP_MAKE_CLOSURE,    /* u16 func_idx, u8 capture_count — create closure */
    OP_LOAD_CAPTURE,    /* u16 capture_index — load from closure captures */
    OP_STORE_CAPTURE,   /* u16 capture_index — store to closure captures */
    OP_CALL_CLOSURE,    /* u8 argc — call closure on top of stack */

    /* Regex */
    OP_REGEX_MATCH,     /* u16 pattern_str_idx, u8 flags — match, push 1/0 */
    OP_REGEX_NOT_MATCH, /* u16 pattern_str_idx, u8 flags — negated match */
    OP_LOAD_CAPTURE_VAR,/* u8 capture_number ($1-$9) */

    /* Try/catch/throw */
    OP_TRY_BEGIN,       /* u16 catch_offset — push exception handler */
    OP_TRY_END,         /* pop exception handler */
    OP_THROW,           /* pop value, throw exception */

    /* String ops */
    OP_SUBSTR,          /* pop len, pop start, pop str, push substr */
    OP_STR_INDEX,       /* pop needle, pop haystack, push index */
    OP_STR_UPPER,       /* pop str, push uppercased */
    OP_STR_LOWER,       /* pop str, push lowercased */
    OP_CHR,             /* pop int, push char string */
    OP_ORD,             /* pop str, push first char code */
    OP_CHOMP,           /* pop str, push chomp'd str (also in-place variant) */
    OP_TR,              /* u16 from_str_idx, u16 to_str_idx — transliterate */
    OP_STR_REPEAT,      /* pop count, pop str, push repeated */

    /* I/O */
    OP_SAY_FH,          /* pop str, pop fh — say to filehandle */
    OP_PRINT_FH,        /* pop str, pop fh — print to filehandle */

    /* Globals (our/local) */
    OP_LOAD_GLOBAL,     /* u16 name_str_idx — load from global registry */
    OP_STORE_GLOBAL,    /* u16 name_str_idx — store to global registry */
    OP_SAVE_GLOBAL,     /* u16 name_str_idx — save current global for local restore */
    OP_RESTORE_GLOBAL,  /* u16 name_str_idx — restore saved global */

    /* Method dispatch */
    OP_METHOD_CALL,     /* u16 method_str_idx, u8 argc — dynamic method dispatch */
    OP_DYN_METHOD_CALL, /* u8 argc — method name on stack */
    OP_CAN,             /* pop method_name, pop obj, push 1/0 */

    /* Logic/math */
    OP_NEGATE,          /* pop, push negated */
    OP_NOT,             /* pop, push !value */
    OP_DEFINED,         /* pop, push 1 if defined, 0 if undef */
    OP_ABS,             /* pop, push abs value */
    OP_POWER,           /* pop exp, pop base, push base**exp */
    OP_SPRINTF,         /* u8 argc — format string + args on stack */

    /* Array extras */
    OP_ARRAY_POP,       /* pop array, push popped element */
    OP_ARRAY_SHIFT,     /* pop array, push shifted element */
    OP_ARRAY_UNSHIFT,   /* pop value, pop array — unshift */
    OP_ARRAY_REVERSE,   /* pop array, push reversed */
    OP_ARRAY_JOIN,      /* pop array, pop separator, push joined string */
    OP_ARRAY_SORT,      /* sort with comparison — uses callback */

    /* Hash extras */
    OP_HASH_EXISTS,     /* pop key, pop hash, push 1/0 */

    /* Control flow */
    OP_FOREACH,         /* u16 var_slot, u16 body_end — iterate array */

    /* Loop control */
    OP_LOOP_BREAK,      /* u16 target — last */
    OP_LOOP_NEXT,       /* u16 target — next */
    OP_LOOP_REDO,       /* u16 target — redo */

    /* Ref */
    OP_REF_TYPE,        /* pop value, push type string */

    /* Overload-aware arithmetic */
    OP_ADD_OV,          /* like ADD but checks overloads */

    /* Switch */
    OP_SWITCH,          /* complex switch dispatch */

    /* Misc */
    OP_CHAR_AT,         /* pop idx, pop str, push char code at idx */
    OP_BYTES,           /* pop str, push byte length */

    /* Comparison operators */
    OP_STR_LT,          /* string less than */
    OP_STR_GT,          /* string greater than */
    OP_STR_LE,          /* string less than or equal */
    OP_STR_GE,          /* string greater than or equal */
    OP_SPACESHIP,       /* <=> operator */

    OP_CALL_NATIVE,     /* u16 native_idx, u8 argc — call native (import_lib) function */

    OP_C_BLOCK,         /* u16 cblock_idx — JIT-compile and execute __C__ block */

    OP_OPCODE_COUNT     /* must be last */
} StradaOpcode;

/* Builtin IDs for OP_BUILTIN dispatch */
enum {
    BUILTIN_CORE_OPEN = 1,
    BUILTIN_CORE_CLOSE,
    BUILTIN_CORE_READLINE,
    BUILTIN_CORE_EOF,
    BUILTIN_CORE_SEEK,
    BUILTIN_CORE_TELL,
    BUILTIN_CORE_REWIND,
    BUILTIN_CORE_FLUSH,
    BUILTIN_CORE_SLURP,
    BUILTIN_CORE_SPEW,
    BUILTIN_CORE_QX,
    BUILTIN_CORE_SYSTEM,
    BUILTIN_CORE_GETENV,
    BUILTIN_CORE_SETENV,
    BUILTIN_CORE_TIME,
    BUILTIN_CORE_HIRES_TIME,
    BUILTIN_CORE_GLOBAL_SET,
    BUILTIN_CORE_GLOBAL_GET,
    BUILTIN_CORE_GLOBAL_EXISTS,
    BUILTIN_CORE_GLOBAL_DELETE,
    BUILTIN_CORE_GLOBAL_KEYS,
    BUILTIN_CORE_SET_RECURSION_LIMIT,
    BUILTIN_MATH_SQRT,
    BUILTIN_MATH_FLOOR,
    BUILTIN_MATH_CEIL,
    BUILTIN_MATH_POW,
    BUILTIN_MATH_SIN,
    BUILTIN_MATH_COS,
    BUILTIN_SELECT,
    BUILTIN_CORE_POPEN,
    BUILTIN_SIZE,
    BUILTIN_CHOMP_INPLACE,
    BUILTIN_MATH_ABS,
    BUILTIN_UCFIRST,
    BUILTIN_LCFIRST,
    BUILTIN_REPLACE,
    BUILTIN_REPLACE_ALL,
    BUILTIN_MATCH,
    BUILTIN_SORT_DEFAULT,
    BUILTIN_CORE_FILE_EXISTS,
    BUILTIN_CORE_UNLINK,
    BUILTIN_TRIM,
    BUILTIN_HEX,
    BUILTIN_OCT,
    BUILTIN_FILE_TEST_D,
    BUILTIN_FILE_TEST_F,
    BUILTIN_RANGE,
};

/* Heap object type tag */
enum VMObjType {
    VM_OBJ_STR = 0,
    VM_OBJ_ARRAY = 2,
    VM_OBJ_HASH = 4,
    VM_OBJ_STRBUF = 6,
    VM_OBJ_CLOSURE = 8,
    VM_OBJ_FILEHANDLE = 10,
    VM_OBJ_CELL = 12,     /* shared mutable cell for closure capture-by-reference */
};
typedef struct { enum VMObjType obj_type; } VMObjHeader;

/* Forward declarations */
typedef struct VMArray VMArray;
typedef struct VMHash VMHash;
typedef struct VMStrBuf VMStrBuf;
typedef struct VMClosure VMClosure;

/* String buffer for O(1) amortized .= append */
struct VMStrBuf {
    VMObjHeader hdr;
    char *data;
    size_t len;
    size_t cap;
};

/* VMCell defined after VMValue */

/*
 * Tagged pointer VMValue — 8 bytes.
 *
 * Bit 0 = 1: tagged integer (value in bits 1-63)
 * Bit 0 = 0, value = 0: undef
 * Bit 0 = 0, value != 0: heap pointer
 */
typedef uint64_t VMValue;

/* String object — inline flexible array member */
typedef struct {
    VMObjHeader hdr;
    uint32_t len;
    char data[];
} VMString;

/* Tagged value operations */
#define VM_IS_INT(v)    ((v) & 1)
#define VM_IS_UNDEF(v)  ((v) == 0)
#define VM_IS_PTR(v)    (((v) != 0) && !((v) & 1))

static inline int64_t VM_INT_VAL(VMValue v) { return (int64_t)v >> 1; }
static inline VMValue VM_MAKE_INT(int64_t i) { return ((uint64_t)i << 1) | 1; }
#define VM_UNDEF_VAL ((VMValue)0)
static inline VMValue VM_MAKE_PTR(void *p) { return (VMValue)(uintptr_t)p; }
static inline void *VM_TO_PTR(VMValue v) { return (void*)(uintptr_t)v; }

/* Type-safe object accessors */
static inline enum VMObjType VM_PTR_TYPE(VMValue v) {
    return ((VMObjHeader*)VM_TO_PTR(v))->obj_type;
}
static inline VMString *VM_AS_STR(VMValue v) { return (VMString*)VM_TO_PTR(v); }
static inline VMArray  *VM_AS_ARRAY(VMValue v) { return (VMArray*)VM_TO_PTR(v); }
static inline VMHash   *VM_AS_HASH(VMValue v) { return (VMHash*)VM_TO_PTR(v); }
static inline VMStrBuf *VM_AS_STRBUF(VMValue v) { return (VMStrBuf*)VM_TO_PTR(v); }
static inline VMClosure *VM_AS_CLOSURE(VMValue v) { return (VMClosure*)VM_TO_PTR(v); }

/* Shared mutable cell for closure capture-by-reference */
typedef struct {
    VMObjHeader hdr;
    VMValue val;
} VMCell;
static inline VMCell *VM_AS_CELL(VMValue v) { return (VMCell*)VM_TO_PTR(v); }

/* Convenience: create a VM string */
static inline VMValue VM_MAKE_STR(const char *s) {
    size_t len = strlen(s);
    VMString *vs = (VMString*)malloc(sizeof(VMString) + len + 1);
    vs->hdr.obj_type = VM_OBJ_STR;
    vs->len = (uint32_t)len;
    memcpy(vs->data, s, len + 1);
    return VM_MAKE_PTR(vs);
}

static inline VMValue VM_MAKE_STR_N(const char *s, size_t len) {
    VMString *vs = (VMString*)malloc(sizeof(VMString) + len + 1);
    vs->hdr.obj_type = VM_OBJ_STR;
    vs->len = (uint32_t)len;
    memcpy(vs->data, s, len);
    vs->data[len] = '\0';
    return VM_MAKE_PTR(vs);
}

#define vm_int(i)  VM_MAKE_INT(i)
#define vm_undef() VM_UNDEF_VAL
#define vm_str(s)  VM_MAKE_STR(s)

/* Dynamic array */
struct VMArray {
    VMObjHeader hdr;
    VMValue *items;
    int size;
    int cap;
};

/* Hash table (open addressing) */
typedef struct {
    char *key;
    VMValue value;
    uint8_t occupied;
} VMHashEntry;

struct VMHash {
    VMObjHeader hdr;
    VMHashEntry *entries;
    int capacity;
    int size;
    char *class_name;
};

/* Closure */
struct VMClosure {
    VMObjHeader hdr;
    int func_idx;
    int capture_count;
    VMValue *captures;  /* shared mutable capture slots */
};

/* Filehandle wrapper for VM */
typedef struct {
    VMObjHeader hdr;
    void *fp;  /* FILE* */
    int is_pipe;
} VMFileHandle;

/* Bytecode chunk — compiled function */
typedef struct {
    uint8_t *code;
    size_t code_len;
    size_t code_cap;
    int64_t *int_consts;
    size_t int_const_count;
    char **str_consts;
    size_t str_const_count;
    int local_count;
    int fixed_param_count;
    int has_variadic;
    int int_only;
    char *name;
    /* Closure info */
    int capture_count;
    char **capture_names;    /* names of captured variables */
} VMChunk;

/* Inheritance mapping for OOP */
typedef struct {
    char *child;
    char *parent;
} VMInherit;

/* Overload mapping for OOP */
typedef struct {
    char *class_name;
    char *op;
    char *method;
} VMOverload;

/* Method modifier (before/after/around) */
typedef struct {
    char *class_name;
    char *method;
    char *modifier_func;
    int kind; /* 0=before, 1=after, 2=around */
} VMModifier;

/* Has attribute for OOP */
typedef struct {
    char *class_name;
    char *attr_name;
    char *attr_type;
    int is_rw;
    int is_required;
    VMValue default_val;
} VMHasAttr;

/* Exception handler */
typedef struct {
    uint8_t *catch_ip;
    size_t stack_base;
    size_t frame_count;
    uint16_t local_slot; /* where to store caught exception */
} VMExcHandler;

/* Global save for local() */
typedef struct {
    char *name;
    VMValue saved_value;
} VMGlobalSave;

/* Native function pointer (for import_lib) */
typedef VMValue (*VMNativeFunc)(int argc, VMValue *args);

/* Native function entry */
typedef struct {
    char *name;
    VMNativeFunc fn;
    void *dl_handle;  /* dlopen handle */
    void *sym;        /* dlsym pointer to the actual C function */
} VMNativeEntry;

/* JIT-compiled __C__ block */
typedef struct {
    char *code;            /* raw C source from the __C__ block */
    char **var_names;      /* local variable names referenced */
    int *var_slots;        /* corresponding local slot indices */
    int var_count;
    void *dl_handle;       /* cached dlopen handle (NULL = not yet compiled) */
    void (*fn)(void**);    /* cached function pointer (takes StradaValue** at runtime) */
} VMCBlock;

/* VM program — collection of compiled functions */
typedef struct {
    VMChunk *funcs;
    size_t func_count;
    size_t func_cap;
    char **func_names;
    VMInherit *inherits;
    int inherit_count;
    VMOverload *overloads;
    int overload_count;
    int overload_cap;
    VMModifier *modifiers;
    int modifier_count;
    int modifier_cap;
    VMHasAttr *attrs;
    int attr_count;
    int attr_cap;
    int *begin_blocks;  /* func indices */
    int begin_count;
    int *end_blocks;    /* func indices */
    int end_count;
    /* Native functions from import_lib */
    VMNativeEntry *natives;
    int native_count;
    int native_cap;
    /* JIT __C__ blocks */
    VMCBlock *cblocks;
    int cblock_count;
    int cblock_cap;
    char *runtime_include_path; /* path to strada_runtime.h for gcc -I */
} VMProgram;

/* Call frame */
typedef struct {
    VMChunk *chunk;
    uint8_t *ip;
    VMValue *locals;
    size_t stack_base;
    VMClosure *closure;  /* non-NULL if called as closure */
} VMFrame;

/* VM state */
typedef struct {
    VMValue *stack;
    size_t stack_top;
    size_t stack_cap;
    VMFrame *frames;
    size_t frame_count;
    size_t frame_cap;
    VMProgram *program;
    /* Regex captures */
    char *regex_captures[10]; /* $1-$9 (index 0 = $0/full match) */
    /* Exception handling */
    VMExcHandler *exc_stack;
    int exc_top;
    int exc_cap;
    VMValue current_exception;
    int has_exception;
    /* Globals for our/local */
    VMHash *globals;
    VMGlobalSave *global_saves;
    int global_save_count;
    int global_save_cap;
    /* Default output filehandle */
    VMValue default_fh;
} VM;

/* Program API */
VMProgram *vm_program_new(void);
void vm_program_free(VMProgram *prog);
VMChunk *vm_program_add_func(VMProgram *prog, const char *name);
int vm_program_find_func(VMProgram *prog, const char *name);

/* Chunk API */
void vm_chunk_emit(VMChunk *chunk, uint8_t byte);
void vm_chunk_emit_u16(VMChunk *chunk, uint16_t val);
void vm_chunk_emit_i64(VMChunk *chunk, int64_t val);
void vm_chunk_emit_u32(VMChunk *chunk, uint32_t val);
size_t vm_chunk_add_int_const(VMChunk *chunk, int64_t val);
size_t vm_chunk_add_str_const(VMChunk *chunk, const char *s);

/* VM API */
const char *vm_program_find_parent(VMProgram *prog, const char *class_name);
VM *vm_new(VMProgram *prog);
void vm_free(VM *vm);
VMValue vm_execute(VM *vm, const char *func_name);

/* VMArray API */
VMArray *vm_array_new(int cap);
void vm_array_free(VMArray *a);
void vm_array_push(VMArray *a, VMValue v);
VMValue vm_array_get(VMArray *a, int idx);
void vm_array_set(VMArray *a, int idx, VMValue v);

/* VMHash API */
VMHash *vm_hash_new(int cap);
void vm_hash_free(VMHash *h);
void vm_hash_set(VMHash *h, const char *key, VMValue v);
VMValue vm_hash_get(VMHash *h, const char *key);
void vm_hash_delete(VMHash *h, const char *key);
VMArray *vm_hash_keys(VMHash *h);
int vm_hash_exists(VMHash *h, const char *key);

/* Overload/modifier helpers */
void vm_program_add_overload(VMProgram *prog, const char *cls, const char *op, const char *method);
void vm_program_add_modifier(VMProgram *prog, const char *cls, const char *method, const char *mod_func, int kind);
void vm_program_add_attr(VMProgram *prog, const char *cls, const char *name, const char *type, int is_rw, int is_required, VMValue default_val);

#endif

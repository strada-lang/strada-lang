/* strada_vm.c — Bytecode VM implementation
 * Tagged-pointer VMValue (8 bytes) with computed goto dispatch.
 * Full language support: closures, try/catch, OOP, regex, I/O, etc. */

#include "vm.h"
#include "../runtime/strada_runtime.h"
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <setjmp.h>
#include <libgen.h>

#include "vm_generic_builtins.inc"

int vm_generic_builtin_find(const char *name, int argc) {
    for (int i = 0; i < VM_GENERIC_BUILTIN_COUNT; i++) {
        if (vm_generic_builtins[i].argc == argc &&
            strcmp(vm_generic_builtins[i].name, name) == 0)
            return i;
    }
    return -1;
}
#include <signal.h>
#include <sys/wait.h>
#include <errno.h>
#include <fnmatch.h>
#include <glob.h>

#ifdef HAVE_PCRE2
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#endif

#ifndef HAVE_PCRE2
#include <regex.h>
#endif

/* ===== Mark-sweep GC & value helpers ===== */

static VM *g_active_vm = NULL;
static VMProgram *g_compiling_prog = NULL;
static VMObjHeader *g_orphan_objects = NULL;
#define LOCALS_POOL_SIZE (1024 * 1024)
static VMValue g_locals_pool[LOCALS_POOL_SIZE];
static size_t g_locals_top = 0;

void vm_gc_set_compiling_prog(VMProgram *prog, VM **saved_vm) {
    if (prog) {
        if (saved_vm) {
            *saved_vm = g_active_vm;
            g_active_vm = NULL;
        }
        g_compiling_prog = prog;
    } else {
        g_compiling_prog = NULL;
        if (saved_vm) {
            g_active_vm = *saved_vm;
        }
    }
}

void vm_gc_init_hdr(VMObjHeader *hdr, enum VMObjType type) {
    hdr->obj_type = type;
    hdr->gc_marked = 0;
    if (g_active_vm) {
        hdr->gc_immortal = 0;
        hdr->gc_next = g_active_vm->gc_objects;
        g_active_vm->gc_objects = hdr;
        g_active_vm->gc_count++;
    } else if (g_compiling_prog) {
        hdr->gc_immortal = 1;
        hdr->gc_next = g_compiling_prog->compile_objects;
        g_compiling_prog->compile_objects = hdr;
    } else {
        hdr->gc_immortal = 1;
        hdr->gc_next = g_orphan_objects;
        g_orphan_objects = hdr;
    }
}

static void vm_gc_free_one(VMObjHeader *hdr) {
    if (!hdr) return;
    switch (hdr->obj_type) {
    case VM_OBJ_STR:
    case VM_OBJ_FLOAT:
    case VM_OBJ_CELL:
        free(hdr);
        break;
    case VM_OBJ_STRBUF: {
        VMStrBuf *sb = (VMStrBuf*)hdr;
        free(sb->data);
        free(sb);
        break;
    }
    case VM_OBJ_ARRAY: {
        VMArray *a = (VMArray*)hdr;
        free(a->items);
        free(a);
        break;
    }
    case VM_OBJ_HASH: {
        VMHash *h = (VMHash*)hdr;
        if (h->entries) {
            for (int i = 0; i < h->capacity; i++) {
                if (h->entries[i].occupied == 1 && h->entries[i].key) {
                    free(h->entries[i].key);
                }
            }
            free(h->entries);
        }
        free(h->class_name);
        free(h);
        break;
    }
    case VM_OBJ_CLOSURE: {
        VMClosure *c = (VMClosure*)hdr;
        free(c->captures);
        free(c);
        break;
    }
    case VM_OBJ_FILEHANDLE: {
        VMFileHandle *fh = (VMFileHandle*)hdr;
        if (fh->fp) {
            if (fh->is_popen) pclose((FILE*)fh->fp);
            else fclose((FILE*)fh->fp);
            fh->fp = NULL;
        }
        free(fh->mem_buf);
        fh->mem_buf = NULL;
        free(fh);
        break;
    }
    case VM_OBJ_NATIVE_SV: {
        VMNativeSV *nsv = (VMNativeSV*)hdr;
        if (nsv->sv) {
            strada_decref(nsv->sv);
            nsv->sv = NULL;
        }
        free(nsv);
        break;
    }
    case VM_OBJ_SREF: {
        VMScalarRef *sr = (VMScalarRef*)hdr;
        free(sr->global_name);
        free(sr);
        break;
    }
    default:
        free(hdr);
        break;
    }
}

/* Under mark-sweep GC, manual operand/local frees are retired (clearing *v only)
 * to eliminate all operand/exception-unwind UAFs. */
static inline void vm_val_free(VMValue *v) {
    *v = VM_UNDEF_VAL;
}

static inline void vm_temp_free(VMValue *v) {
    *v = VM_UNDEF_VAL;
}

static inline int vm_is_float_operand(VMValue v) {
    if (!VM_IS_PTR(v)) return 0;
    enum VMObjType t = VM_PTR_TYPE(v);
    if (t == VM_OBJ_FLOAT) return 1;
    if (t == VM_OBJ_STR || t == VM_OBJ_STRBUF) {
        const char *s = (t == VM_OBJ_STR) ? VM_AS_STR(v)->data : VM_AS_STRBUF(v)->data;
        if (s) {
            for (const char *p = s; *p; p++) {
                if (*p == '.' || *p == 'e' || *p == 'E') return 1;
            }
        }
    }
    return 0;
}

static inline int64_t vm_to_int(VMValue v) {
    if (VM_IS_INT(v)) return VM_INT_VAL(v);
    if (VM_IS_PTR(v)) {
        switch (VM_PTR_TYPE(v)) {
        case VM_OBJ_FLOAT: {
            double d = VM_AS_FLOAT(v)->val;
            return (isfinite(d) && d >= -9.22e18 && d < 9.22e18) ? (int64_t)d : 0;
        }
        case VM_OBJ_STR: return VM_AS_STR(v)->data ? atoll(VM_AS_STR(v)->data) : 0;
        case VM_OBJ_STRBUF: return VM_AS_STRBUF(v)->data ? atoll(VM_AS_STRBUF(v)->data) : 0;
        case VM_OBJ_ARRAY: return VM_AS_ARRAY(v)->size;
        case VM_OBJ_HASH: return VM_AS_HASH(v)->size;
        default: break;
        }
    }
    return 0;
}

static inline double vm_to_num(VMValue v) {
    if (VM_IS_INT(v)) return (double)VM_INT_VAL(v);
    if (VM_IS_PTR(v)) {
        switch (VM_PTR_TYPE(v)) {
        case VM_OBJ_FLOAT: return VM_AS_FLOAT(v)->val;
        case VM_OBJ_STR: return VM_AS_STR(v)->data ? strtod(VM_AS_STR(v)->data, NULL) : 0.0;
        case VM_OBJ_STRBUF: return VM_AS_STRBUF(v)->data ? strtod(VM_AS_STRBUF(v)->data, NULL) : 0.0;
        case VM_OBJ_ARRAY: return (double)VM_AS_ARRAY(v)->size;
        case VM_OBJ_HASH: return (double)VM_AS_HASH(v)->size;
        default: break;
        }
    }
    return 0.0;
}

/* Push a double result as a boxed VM_OBJ_FLOAT */
#define VM_PUSH_DOUBLE(sp, result) SP_PUSH(VM_MAKE_FLOAT(result))

static inline int vm_to_bool(VMValue v) {
    if (VM_IS_INT(v)) return VM_INT_VAL(v) != 0;
    if (VM_IS_UNDEF(v)) return 0;
    if (VM_IS_PTR(v)) {
        switch (VM_PTR_TYPE(v)) {
        case VM_OBJ_FLOAT: { double d = VM_AS_FLOAT(v)->val; return d != 0.0 && !isnan(d); }
        case VM_OBJ_STR: { char *s = VM_AS_STR(v)->data; return s && s[0] && !(s[0]=='0'&&s[1]=='\0'); }
        case VM_OBJ_STRBUF: { VMStrBuf *sb = VM_AS_STRBUF(v); return sb->len > 0 && !(sb->len == 1 && sb->data[0] == '0'); }
        case VM_OBJ_ARRAY: return VM_AS_ARRAY(v)->size > 0;
        case VM_OBJ_HASH: return VM_AS_HASH(v)->size > 0;
        default: return 1;
        }
    }
    return 0;
}

static const char *vm_to_cstr(VMValue v, char *buf, size_t bufsz) {
    if (VM_IS_INT(v)) { snprintf(buf, bufsz, "%lld", (long long)VM_INT_VAL(v)); return buf; }
    if (VM_IS_UNDEF(v)) return "";
    switch (VM_PTR_TYPE(v)) {
    case VM_OBJ_FLOAT: { snprintf(buf, bufsz, "%.15g", VM_AS_FLOAT(v)->val); return buf; }
    case VM_OBJ_STR: return VM_AS_STR(v)->data ? VM_AS_STR(v)->data : "";
    case VM_OBJ_STRBUF: return VM_AS_STRBUF(v)->data ? VM_AS_STRBUF(v)->data : "";
    case VM_OBJ_ARRAY: snprintf(buf, bufsz, "%d", VM_AS_ARRAY(v)->size); return buf;
    case VM_OBJ_HASH: {
        VMHash *h = VM_AS_HASH(v);
        if (h->class_name) {
            snprintf(buf, bufsz, "%s=HASH(0x%lx)", h->class_name, (unsigned long)(uintptr_t)h);
        } else {
            snprintf(buf, bufsz, "HASH(0x%lx)", (unsigned long)(uintptr_t)h);
        }
        return buf;
    }
    case VM_OBJ_CLOSURE: snprintf(buf, bufsz, "CODE(0x%lx)", (unsigned long)(uintptr_t)VM_TO_PTR(v)); return buf;
    case VM_OBJ_NATIVE_SV: {
        VMNativeSV *nsv = (VMNativeSV*)VM_TO_PTR(v);
        if (nsv->sv && !STRADA_IS_TAGGED_INT(nsv->sv) && nsv->sv->meta && nsv->sv->meta->blessed_package)
            snprintf(buf, bufsz, "%s=HASH(0x%lx)", nsv->sv->meta->blessed_package, (unsigned long)(uintptr_t)nsv->sv);
        else
            snprintf(buf, bufsz, "NativeSV(0x%lx)", (unsigned long)(uintptr_t)nsv->sv);
        return buf;
    }
    case VM_OBJ_SREF: snprintf(buf, bufsz, "SCALAR(0x%lx)", (unsigned long)(uintptr_t)VM_TO_PTR(v)); return buf;
    default: return "";
    }
}

/* Deep copy a VMValue (for strings/strbuf/float, returns a new copy; ints/undef pass through; containers are shared) */
static inline VMValue vm_val_copy(VMValue v) {
    if (VM_IS_PTR(v)) {
        enum VMObjType t = VM_PTR_TYPE(v);
        if (t == VM_OBJ_STR) return VM_MAKE_STR_N(VM_AS_STR(v)->data, VM_AS_STR(v)->len);
        if (t == VM_OBJ_STRBUF) return VM_MAKE_STR_N(VM_AS_STRBUF(v)->data, VM_AS_STRBUF(v)->len);
        if (t == VM_OBJ_FLOAT) return VM_MAKE_FLOAT(VM_AS_FLOAT(v)->val);
    }
    return v;
}

/* ===== VMArray ===== */

VMArray *vm_array_new(int cap) {
    if (cap < 8) cap = 8;
    VMArray *a = calloc(1, sizeof(VMArray));
    vm_gc_init_hdr(&a->hdr, VM_OBJ_ARRAY);
    a->cap = cap;
    a->items = calloc(cap, sizeof(VMValue));
    return a;
}

void vm_array_free(VMArray *a) { if (a) { free(a->items); free(a); } }

void vm_array_push(VMArray *a, VMValue v) {
    if (a->size >= a->cap) {
        int new_cap = a->cap < 1024 ? a->cap * 2 : a->cap + a->cap / 2;
        if (new_cap < 8) new_cap = 8;
        a->cap = new_cap;
        a->items = realloc(a->items, a->cap * sizeof(VMValue));
    }
    a->items[a->size++] = v;
}

VMValue vm_array_get(VMArray *a, int idx) {
    if (idx < 0) idx = a->size + idx;
    if (idx < 0 || idx >= a->size) return VM_UNDEF_VAL;
    return a->items[idx];
}

void vm_array_set(VMArray *a, int idx, VMValue v) {
    /* Negative index counts from the end (mirrors vm_array_get). Without this,
     * $arr[-1] = v wrote at a->items[negative] — an out-of-bounds heap write. */
    if (idx < 0) { idx = a->size + idx; if (idx < 0) return; }
    while (idx >= a->cap) {
        int new_cap = a->cap < 1024 ? a->cap * 2 : a->cap + a->cap / 2;
        if (new_cap < 8) new_cap = 8;
        if (new_cap <= idx) new_cap = idx + 1;
        a->cap = new_cap;
        a->items = realloc(a->items, a->cap * sizeof(VMValue));
    }
    while (idx >= a->size) a->items[a->size++] = VM_UNDEF_VAL;
    a->items[idx] = v;
}

static VMValue vm_array_pop(VMArray *a) {
    if (a->size == 0) return VM_UNDEF_VAL;
    return a->items[--a->size];
}

static VMValue vm_array_shift(VMArray *a) {
    if (a->size == 0) return VM_UNDEF_VAL;
    VMValue v = a->items[0];
    memmove(a->items, a->items + 1, (a->size - 1) * sizeof(VMValue));
    a->size--;
    return v;
}

static void vm_array_unshift(VMArray *a, VMValue v) {
    vm_array_push(a, VM_UNDEF_VAL); /* ensure space */
    memmove(a->items + 1, a->items, (a->size - 1) * sizeof(VMValue));
    a->items[0] = v;
}

/* ===== VMHash ===== */

static uint32_t vm_hash_fn(const char *key) {
    uint32_t h = 5381;
    for (const char *p = key; *p; p++) h = ((h << 5) + h) ^ (uint8_t)*p;
    return h;
}

static inline void key_free(char *k);

VMHash *vm_hash_new(int cap) {
    if (cap < 16) cap = 16;
    int c = 16; while (c < cap) c *= 2;
    VMHash *h = calloc(1, sizeof(VMHash));
    vm_gc_init_hdr(&h->hdr, VM_OBJ_HASH);
    h->capacity = c;
    h->entries = calloc(c, sizeof(VMHashEntry));
    return h;
}

void vm_hash_free(VMHash *h) {
    if (!h) return;
    for (int i = 0; i < h->capacity; i++) {
        if (h->entries[i].occupied == 1) {
            key_free(h->entries[i].key);
            vm_val_free(&h->entries[i].value);
        }
    }
    free(h->entries);
    if (h->class_name) free(h->class_name);
    free(h);
}

static void vm_hash_grow(VMHash *h) {
    int old_cap = h->capacity;
    VMHashEntry *old = h->entries;
    h->capacity *= 2;
    h->entries = calloc(h->capacity, sizeof(VMHashEntry));
    h->size = 0;
    for (int i = 0; i < old_cap; i++) {
        if (old[i].occupied == 1) {
            uint32_t mask = h->capacity - 1;
            uint32_t idx = old[i].hash & mask;
            while (h->entries[idx].occupied == 1) idx = (idx + 1) & mask;
            h->entries[idx] = old[i];
            h->size++;
        }
    }
    free(old);
}

void vm_hash_set(VMHash *h, const char *key, VMValue v) {
    if (h->size * 4 >= h->capacity * 3) vm_hash_grow(h);
    uint32_t hash = vm_hash_fn(key);
    uint16_t klen = (uint16_t)strlen(key);
    uint32_t mask = h->capacity - 1;
    uint32_t idx = hash & mask;
    for (;;) {
        if (h->entries[idx].occupied != 1) {
            h->entries[idx].key = strdup(key);
            h->entries[idx].value = v;
            h->entries[idx].hash = hash;
            h->entries[idx].keylen = klen;
            h->entries[idx].occupied = 1;
            h->size++;
            return;
        }
        if (h->entries[idx].hash == hash && h->entries[idx].keylen == klen &&
            memcmp(h->entries[idx].key, key, klen) == 0) {
            vm_val_free(&h->entries[idx].value);
            h->entries[idx].value = v;
            return;
        }
        idx = (idx + 1) & mask;
    }
}

static void vm_hash_set_n(VMHash *h, const char *key, size_t keylen, VMValue v) {
    if (h->size * 4 >= h->capacity * 3) vm_hash_grow(h);
    uint32_t hash = vm_hash_fn(key);
    uint32_t mask = h->capacity - 1;
    uint32_t idx = hash & mask;
    for (;;) {
        if (h->entries[idx].occupied != 1) {
            char *k = malloc(keylen + 1);
            memcpy(k, key, keylen + 1);
            h->entries[idx].key = k;
            h->entries[idx].value = v;
            h->entries[idx].hash = hash;
            h->entries[idx].keylen = (uint16_t)keylen;
            h->entries[idx].occupied = 1;
            h->size++;
            return;
        }
        if (h->entries[idx].hash == hash && h->entries[idx].keylen == (uint16_t)keylen &&
            memcmp(h->entries[idx].key, key, keylen) == 0) {
            h->entries[idx].value = v;
            return;
        }
        idx = (idx + 1) & mask;
    }
}

VMValue vm_hash_get(VMHash *h, const char *key) {
    uint32_t hash = vm_hash_fn(key);
    uint16_t klen = (uint16_t)strlen(key);
    uint32_t mask = h->capacity - 1;
    uint32_t idx = hash & mask;
    for (;;) {
        if (h->entries[idx].occupied == 0) return VM_UNDEF_VAL;
        if (h->entries[idx].occupied == 1 && h->entries[idx].hash == hash &&
            h->entries[idx].keylen == klen && memcmp(h->entries[idx].key, key, klen) == 0)
            return h->entries[idx].value;
        idx = (idx + 1) & mask;
    }
}

static inline void key_free(char *k) {
    free(k);
}

void vm_hash_delete(VMHash *h, const char *key) {
    uint32_t hash = vm_hash_fn(key);
    uint16_t klen = (uint16_t)strlen(key);
    uint32_t mask = h->capacity - 1;
    uint32_t idx = hash & mask;
    for (;;) {
        if (h->entries[idx].occupied == 0) return;
        if (h->entries[idx].occupied == 1 && h->entries[idx].hash == hash &&
            h->entries[idx].keylen == klen && memcmp(h->entries[idx].key, key, klen) == 0) {
            key_free(h->entries[idx].key); h->entries[idx].key = NULL;
            vm_val_free(&h->entries[idx].value);
            h->entries[idx].occupied = 2;
            h->size--;
            return;
        }
        idx = (idx + 1) & mask;
    }
}

int vm_hash_exists(VMHash *h, const char *key) {
    uint32_t hash = vm_hash_fn(key);
    uint16_t klen = (uint16_t)strlen(key);
    uint32_t mask = h->capacity - 1;
    uint32_t idx = hash & mask;
    for (;;) {
        if (h->entries[idx].occupied == 0) return 0;
        if (h->entries[idx].occupied == 1 && h->entries[idx].hash == hash &&
            h->entries[idx].keylen == klen && memcmp(h->entries[idx].key, key, klen) == 0) return 1;
        idx = (idx + 1) & mask;
    }
}

VMArray *vm_hash_keys(VMHash *h) {
    VMArray *keys = vm_array_new(h->size > 0 ? h->size : 8);
    for (int i = 0; i < h->capacity; i++)
        if (h->entries[i].occupied == 1)
            vm_array_push(keys, vm_str(h->entries[i].key));
    return keys;
}

/* ===== VMChunk ===== */

static VMChunk vm_chunk_new(const char *name) {
    VMChunk c = {0};
    c.code_cap = 256;
    c.code = malloc(c.code_cap);
    c.name = strdup(name);
    return c;
}

void vm_chunk_emit(VMChunk *c, uint8_t byte) {
    if (c->code_len >= c->code_cap) {
        c->code_cap = c->code_cap ? c->code_cap * 2 : 256;
        c->code = realloc(c->code, c->code_cap);
    }
    c->code[c->code_len++] = byte;
}
void vm_chunk_emit_u16(VMChunk *c, uint16_t val) { vm_chunk_emit(c, val & 0xFF); vm_chunk_emit(c, (val >> 8) & 0xFF); }
void vm_chunk_emit_i64(VMChunk *c, int64_t val) { for (int i = 0; i < 8; i++) vm_chunk_emit(c, (val >> (i * 8)) & 0xFF); }
void vm_chunk_emit_f64(VMChunk *c, double val) {
    uint64_t bits = 0;
    memcpy(&bits, &val, sizeof(bits));
    vm_chunk_emit_i64(c, (int64_t)bits);
}
void vm_chunk_emit_u32(VMChunk *c, uint32_t val) { for (int i = 0; i < 4; i++) vm_chunk_emit(c, (val >> (i * 8)) & 0xFF); }

size_t vm_chunk_add_int_const(VMChunk *c, int64_t val) {
    c->int_consts = realloc(c->int_consts, (c->int_const_count + 1) * sizeof(int64_t));
    c->int_consts[c->int_const_count] = val;
    return c->int_const_count++;
}

size_t vm_chunk_add_str_const(VMChunk *c, const char *s) {
    c->str_consts = realloc(c->str_consts, (c->str_const_count + 1) * sizeof(char*));
    c->str_consts[c->str_const_count] = strdup(s);
    return c->str_const_count++;
}

/* ===== VMProgram ===== */

VMProgram *vm_program_new(void) {
    VMProgram *p = calloc(1, sizeof(VMProgram));
    p->func_cap = 16;
    p->funcs = calloc(p->func_cap, sizeof(VMChunk));
    p->func_names = calloc(p->func_cap, sizeof(char*));
    p->overload_cap = 8;
    p->overloads = calloc(p->overload_cap, sizeof(VMOverload));
    p->modifier_cap = 8;
    p->modifiers = calloc(p->modifier_cap, sizeof(VMModifier));
    p->attr_cap = 16;
    p->attrs = calloc(p->attr_cap, sizeof(VMHasAttr));
    return p;
}

void vm_program_free(VMProgram *p) {
    for (size_t i = 0; i < p->func_count; i++) {
        free(p->funcs[i].code); free(p->funcs[i].name); free(p->funcs[i].int_consts);
        for (size_t j = 0; j < p->funcs[i].str_const_count; j++) free(p->funcs[i].str_consts[j]);
        free(p->funcs[i].str_consts); free(p->func_names[i]);
        if (p->funcs[i].capture_names) {
            for (int j = 0; j < p->funcs[i].capture_count; j++) free(p->funcs[i].capture_names[j]);
            free(p->funcs[i].capture_names);
        }
    }
    free(p->funcs); free(p->func_names);
    for (int i = 0; i < p->inherit_count; i++) { free(p->inherits[i].child); free(p->inherits[i].parent); }
    free(p->inherits);
    for (int i = 0; i < p->overload_count; i++) { free(p->overloads[i].class_name); free(p->overloads[i].op); free(p->overloads[i].method); }
    free(p->overloads);
    for (int i = 0; i < p->modifier_count; i++) { free(p->modifiers[i].class_name); free(p->modifiers[i].method); free(p->modifiers[i].modifier_func); }
    free(p->modifiers);
    for (int i = 0; i < p->attr_count; i++) { free(p->attrs[i].class_name); free(p->attrs[i].attr_name); free(p->attrs[i].attr_type); }
    free(p->attrs);
    free(p->begin_blocks);
    free(p->end_blocks);
    if (p->natives) {
        for (int i = 0; i < p->native_count; i++) free(p->natives[i].name);
        free(p->natives);
    }
    if (p->cblocks) {
        for (int i = 0; i < p->cblock_count; i++) {
            free(p->cblocks[i].code);
            for (int j = 0; j < p->cblocks[i].var_count; j++) free(p->cblocks[i].var_names[j]);
            free(p->cblocks[i].var_names);
            free(p->cblocks[i].var_slots);
            if (p->cblocks[i].dl_handle) dlclose(p->cblocks[i].dl_handle);
        }
        free(p->cblocks);
    }
    free(p->runtime_include_path);
    free(p->cblock_prelude);
    VMObjHeader *co = p->compile_objects;
    while (co) {
        VMObjHeader *next = co->gc_next;
        vm_gc_free_one(co);
        co = next;
    }
    if (p->dl_handles) {
        for (int i = 0; i < p->dl_handle_count; i++) {
            if (p->dl_handles[i]) dlclose(p->dl_handles[i]);
        }
        free(p->dl_handles);
    }
    free(p);
}

VMChunk *vm_program_add_func(VMProgram *p, const char *name) {
    if (p->func_count >= p->func_cap) {
        p->func_cap *= 2;
        p->funcs = realloc(p->funcs, p->func_cap * sizeof(VMChunk));
        p->func_names = realloc(p->func_names, p->func_cap * sizeof(char*));
    }
    size_t idx = p->func_count++;
    p->funcs[idx] = vm_chunk_new(name);
    p->func_names[idx] = strdup(name);
    return &p->funcs[idx];
}

int vm_program_find_func(VMProgram *p, const char *name) {
    for (size_t i = 0; i < p->func_count; i++)
        if (strcmp(p->func_names[i], name) == 0) return (int)i;
    return -1;
}

const char *vm_program_find_parent(VMProgram *p, const char *class_name) {
    for (int i = 0; i < p->inherit_count; i++)
        if (strcmp(p->inherits[i].child, class_name) == 0) return p->inherits[i].parent;
    return NULL;
}

/* MI-aware isa: a class can have MULTIPLE inherit entries (multiple
 * inheritance) — vm_program_find_parent only returns the first, so isa
 * checks must recurse over every parent edge. */
int vm_class_isa(VMProgram *p, const char *cls, const char *target, int depth) {
    if (!cls || !target || depth > 32) return 0;
    if (strcmp(cls, target) == 0) return 1;
    for (int i = 0; i < p->inherit_count; i++) {
        if (strcmp(p->inherits[i].child, cls) == 0 &&
            vm_class_isa(p, p->inherits[i].parent, target, depth + 1))
            return 1;
    }
    return 0;
}

void vm_program_add_overload(VMProgram *prog, const char *cls, const char *op, const char *method) {
    if (prog->overload_count >= prog->overload_cap) {
        prog->overload_cap *= 2;
        prog->overloads = realloc(prog->overloads, prog->overload_cap * sizeof(VMOverload));
    }
    VMOverload *ov = &prog->overloads[prog->overload_count++];
    ov->class_name = strdup(cls);
    ov->op = strdup(op);
    ov->method = strdup(method);
}

void vm_program_add_modifier(VMProgram *prog, const char *cls, const char *method, const char *mod_func, int kind) {
    if (prog->modifier_count >= prog->modifier_cap) {
        prog->modifier_cap *= 2;
        prog->modifiers = realloc(prog->modifiers, prog->modifier_cap * sizeof(VMModifier));
    }
    VMModifier *m = &prog->modifiers[prog->modifier_count++];
    m->class_name = strdup(cls);
    m->method = strdup(method);
    m->modifier_func = strdup(mod_func);
    m->kind = kind;
}

void vm_program_add_attr(VMProgram *prog, const char *cls, const char *name, const char *type, int is_rw, int is_required, VMValue default_val) {
    if (prog->attr_count >= prog->attr_cap) {
        prog->attr_cap *= 2;
        prog->attrs = realloc(prog->attrs, prog->attr_cap * sizeof(VMHasAttr));
    }
    VMHasAttr *a = &prog->attrs[prog->attr_count++];
    a->class_name = strdup(cls);
    a->attr_name = strdup(name);
    a->attr_type = strdup(type);
    a->is_rw = is_rw;
    a->is_required = is_required;
    a->default_val = default_val;
}

/* ===== VM ===== */

VM *vm_new(VMProgram *prog) {
    VM *vm = calloc(1, sizeof(VM));
    vm->stack_cap = 1024 * 1024;
    vm->stack = calloc(vm->stack_cap, sizeof(VMValue));
    vm->frame_cap = 256;
    vm->frames = calloc(vm->frame_cap, sizeof(VMFrame));
    vm->recursion_limit = 1000;
    vm->program = prog;
    vm->exc_cap = 32;
    vm->exc_stack = calloc(vm->exc_cap, sizeof(VMExcHandler));
    vm->exc_top = 0;
    vm->global_save_cap = 32;
    vm->global_saves = calloc(vm->global_save_cap, sizeof(VMGlobalSave));
    vm->default_fh = VM_UNDEF_VAL;
    vm->current_exception = VM_UNDEF_VAL;
    vm->has_exception = 0;
    vm->pending_exc = VM_UNDEF_VAL;
    vm->has_pending_exc = 0;
    vm->last_result = VM_UNDEF_VAL;
    vm->gc_threshold = 4096;
    memset(vm->regex_captures, 0, sizeof(vm->regex_captures));
    vm->saved_locals_top = g_locals_top;
    vm->saved_active_vm = g_active_vm;
    vm->trap_uncaught = 0;
    vm->end_ran = 0;
    g_active_vm = vm;
    vm->globals = vm_hash_new(64);
    return vm;
}

static inline void vm_gc_mark_val(VMValue v, VMObjHeader ***work, size_t *wlen, size_t *wcap) {
    if (!VM_IS_PTR(v)) return;
    VMObjHeader *hdr = (VMObjHeader*)VM_TO_PTR(v);
    if (!hdr || hdr->gc_marked) return;
    hdr->gc_marked = 1;
    if (*wlen >= *wcap) {
        *wcap = (*wcap == 0) ? 256 : (*wcap * 2);
        *work = realloc(*work, *wcap * sizeof(VMObjHeader*));
    }
    (*work)[(*wlen)++] = hdr;
}

void vm_gc_collect(VM *vm) {
    if (!vm || vm->gc_pause > 0 || vm->exec_base > 0) return;

    VMObjHeader **work = NULL;
    size_t wlen = 0, wcap = 0;

    for (size_t i = 0; i < vm->stack_top; i++) {
        vm_gc_mark_val(vm->stack[i], &work, &wlen, &wcap);
    }
    for (size_t i = 0; i < vm->frame_count; i++) {
        VMFrame *f = &vm->frames[i];
        if (f->locals && f->chunk) {
            int lc = f->chunk->local_count + 1;
            for (int j = 0; j < lc; j++) {
                vm_gc_mark_val(f->locals[j], &work, &wlen, &wcap);
            }
        }
        if (f->closure) {
            vm_gc_mark_val(VM_MAKE_PTR(f->closure), &work, &wlen, &wcap);
        }
    }
    if (vm->globals) {
        vm_gc_mark_val(VM_MAKE_PTR(vm->globals), &work, &wlen, &wcap);
    }
    for (int i = 0; i < vm->global_save_count; i++) {
        vm_gc_mark_val(vm->global_saves[i].saved_value, &work, &wlen, &wcap);
        if (vm->global_saves[i].is_hash_elem && vm->global_saves[i].hash) {
            vm_gc_mark_val(VM_MAKE_PTR(vm->global_saves[i].hash), &work, &wlen, &wcap);
        }
    }
    vm_gc_mark_val(vm->current_exception, &work, &wlen, &wcap);
    vm_gc_mark_val(vm->pending_exc, &work, &wlen, &wcap);
    vm_gc_mark_val(vm->default_fh, &work, &wlen, &wcap);
    vm_gc_mark_val(vm->last_result, &work, &wlen, &wcap);
    if (vm->argv) {
        vm_gc_mark_val(VM_MAKE_PTR(vm->argv), &work, &wlen, &wcap);
    }
    if (vm->program) {
        for (int i = 0; i < vm->program->attr_count; i++) {
            vm_gc_mark_val(vm->program->attrs[i].default_val, &work, &wlen, &wcap);
        }
    }

    while (wlen > 0) {
        VMObjHeader *obj = work[--wlen];
        switch (obj->obj_type) {
        case VM_OBJ_ARRAY: {
            VMArray *a = (VMArray*)obj;
            for (int i = 0; i < a->size; i++) {
                vm_gc_mark_val(a->items[i], &work, &wlen, &wcap);
            }
            break;
        }
        case VM_OBJ_HASH: {
            VMHash *h = (VMHash*)obj;
            vm_gc_mark_val(h->tied_obj, &work, &wlen, &wcap);
            for (int i = 0; i < h->capacity; i++) {
                if (h->entries[i].occupied == 1) {
                    vm_gc_mark_val(h->entries[i].value, &work, &wlen, &wcap);
                }
            }
            break;
        }
        case VM_OBJ_CLOSURE: {
            VMClosure *c = (VMClosure*)obj;
            for (int i = 0; i < c->capture_count; i++) {
                vm_gc_mark_val(c->captures[i], &work, &wlen, &wcap);
            }
            break;
        }
        case VM_OBJ_CELL: {
            VMCell *cell = (VMCell*)obj;
            vm_gc_mark_val(cell->val, &work, &wlen, &wcap);
            break;
        }
        case VM_OBJ_SREF: {
            VMScalarRef *sr = (VMScalarRef*)obj;
            vm_gc_mark_val(sr->cell, &work, &wlen, &wcap);
            break;
        }
        default:
            break;
        }
    }
    free(work);

    VMObjHeader **pp = &vm->gc_objects;
    while (*pp) {
        VMObjHeader *hdr = *pp;
        if (hdr->gc_marked || hdr->gc_immortal) {
            hdr->gc_marked = 0;
            pp = &hdr->gc_next;
        } else {
            *pp = hdr->gc_next;
            vm->gc_count--;
            vm_gc_free_one(hdr);
        }
    }
    vm->gc_threshold = (vm->gc_count < 2048) ? 4096 : (vm->gc_count * 2);
}

/* ===== Locals pool cleanup needed by vm_free ===== */

void vm_free(VM *vm) {
    if (!vm) return;
    VMObjHeader *obj = vm->gc_objects;
    while (obj) {
        VMObjHeader *next = obj->gc_next;
        vm_gc_free_one(obj);
        obj = next;
    }
    vm->gc_objects = NULL;
    vm->gc_count = 0;

    free(vm->stack);
    for (size_t i = 0; i < vm->frame_count; i++) {
        VMValue *lp = vm->frames[i].locals;
        if (lp && (lp < g_locals_pool || lp >= g_locals_pool + LOCALS_POOL_SIZE)) {
            free(lp);
        }
    }
    if (vm->exec_base == 0) {
        g_locals_top = vm->saved_locals_top;
    }
    free(vm->frames);
    free(vm->exc_stack);
    for (int i = 0; i < vm->global_save_count; i++) {
        free(vm->global_saves[i].name);
        free(vm->global_saves[i].hash_key);
    }
    free(vm->global_saves);
    for (int i = 0; i < 10; i++) {
        free(vm->regex_captures[i]);
        free(vm->regex_capture_names[i]);
    }
    free(vm->script_name);
    if (g_active_vm == vm) g_active_vm = vm->saved_active_vm;
    free(vm);
}

static inline VMValue *locals_alloc(int count, int skip_zero) {
    (void)skip_zero;
    if (count <= 0) count = 1;
    if (__builtin_expect(g_locals_top + count <= LOCALS_POOL_SIZE, 1)) {
        VMValue *p = &g_locals_pool[g_locals_top];
        memset(p, 0, count * sizeof(VMValue));
        g_locals_top += count;
        return p;
    }
    return (VMValue*)calloc(count, sizeof(VMValue));
}

static inline void locals_free(VMValue *p, int count) {
    if (count <= 0) count = 1;
    if (p == &g_locals_pool[g_locals_top - count]) g_locals_top -= count;
    else if (p < g_locals_pool || p >= g_locals_pool + LOCALS_POOL_SIZE) free(p);
}

/* ===== Bytecode reading ===== */
static inline uint16_t read_u16(uint8_t *ip) { return ip[0] | ((uint16_t)ip[1] << 8); }
static inline int64_t read_i64(uint8_t *ip) {
    int64_t val = 0;
    for (int i = 0; i < 8; i++) val |= ((int64_t)ip[i]) << (i * 8);
    return val;
}
static inline double read_f64(uint8_t *ip) {
    uint64_t bits = (uint64_t)read_i64(ip);
    double val = 0.0;
    memcpy(&val, &bits, sizeof(val));
    return val;
}
static inline uint32_t read_u32(uint8_t *ip) {
    return ip[0] | ((uint32_t)ip[1] << 8) | ((uint32_t)ip[2] << 16) | ((uint32_t)ip[3] << 24);
}

/* ===== Regex helpers ===== */

/* === Regex compilation cache === */
#define REGEX_CACHE_SIZE 64
typedef struct {
    char *pattern;         /* heap-copied pattern string (owned) */
    uint32_t hash;
#ifdef HAVE_PCRE2
    pcre2_code *compiled;
    pcre2_match_data *match_data;
#else
    regex_t compiled;
    int valid;
#endif
} RegexCacheEntry;

static RegexCacheEntry g_regex_cache[REGEX_CACHE_SIZE];

static void __attribute__((destructor)) vm_static_cleanup(void) {
    for (int i = 0; i < REGEX_CACHE_SIZE; i++) {
        RegexCacheEntry *e = &g_regex_cache[i];
        if (e->pattern) { free(e->pattern); e->pattern = NULL; }
#ifdef HAVE_PCRE2
        if (e->compiled) { pcre2_code_free(e->compiled); e->compiled = NULL; }
        if (e->match_data) { pcre2_match_data_free(e->match_data); e->match_data = NULL; }
#else
        if (e->valid) { regfree(&e->compiled); e->valid = 0; }
#endif
    }
    VMObjHeader *o = g_orphan_objects;
    g_orphan_objects = NULL;
    while (o) {
        VMObjHeader *next = o->gc_next;
        vm_gc_free_one(o);
        o = next;
    }
}

#ifdef HAVE_PCRE2
static inline pcre2_code *regex_cache_get(const char *pattern, pcre2_match_data **md_out) {
    uint32_t h = vm_hash_fn(pattern);
    uint32_t idx = h & (REGEX_CACHE_SIZE - 1);
    RegexCacheEntry *e = &g_regex_cache[idx];
    if (e->pattern && e->hash == h && strcmp(e->pattern, pattern) == 0) {
        *md_out = e->match_data;
        return e->compiled;
    }
    /* Evict old entry */
    if (e->pattern) { free(e->pattern); e->pattern = NULL; }
    if (e->compiled) { pcre2_code_free(e->compiled); pcre2_match_data_free(e->match_data); e->compiled = NULL; e->match_data = NULL; }
    int errcode; PCRE2_SIZE erroff;
    pcre2_code *re = pcre2_compile((PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED, 0, &errcode, &erroff, NULL);
    if (!re) { *md_out = NULL; return NULL; }
    e->pattern = strdup(pattern);
    e->hash = h;
    e->compiled = re;
    e->match_data = pcre2_match_data_create_from_pattern(re, NULL);
    *md_out = e->match_data;
    return re;
}
#endif

static int vm_regex_match(VM *vm, const char *str, const char *pattern) {
    /* Clear old captures */
    for (int i = 0; i < 10; i++) { if (vm->regex_captures[i]) { free(vm->regex_captures[i]); vm->regex_captures[i] = NULL; } }
    for (int i = 0; i < 10; i++) { if (vm->regex_capture_names[i]) { free(vm->regex_capture_names[i]); vm->regex_capture_names[i] = NULL; } }

#ifdef HAVE_PCRE2
    pcre2_match_data *md;
    pcre2_code *re = regex_cache_get(pattern, &md);
    if (!re) return 0;
    int rc = pcre2_match(re, (PCRE2_SPTR)str, strlen(str), 0, 0, md, NULL);
    if (rc > 0) {
        PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
        int count = rc < 10 ? rc : 10;
        for (int i = 0; i < count; i++) {
            if (ov[2*i] != PCRE2_UNSET) {
                size_t len = ov[2*i+1] - ov[2*i];
                vm->regex_captures[i] = strndup(str + ov[2*i], len);
            }
        }

        /* Extract named-group names (PCRE2 name table) */
        {
            uint32_t __nc = 0, __nes = 0;
            PCRE2_SPTR __nt = NULL;
            pcre2_pattern_info(re, PCRE2_INFO_NAMECOUNT, &__nc);
            if (__nc > 0 &&
                pcre2_pattern_info(re, PCRE2_INFO_NAMETABLE, &__nt) == 0 &&
                pcre2_pattern_info(re, PCRE2_INFO_NAMEENTRYSIZE, &__nes) == 0) {
                for (uint32_t __n = 0; __n < __nc; __n++) {
                    PCRE2_SPTR __e = __nt + __n * __nes;
                    int __g = (__e[0] << 8) | __e[1];
                    if (__g >= 0 && __g < 10 && !vm->regex_capture_names[__g])
                        vm->regex_capture_names[__g] = strdup((const char *)(__e + 2));
                }
            }
        }
    }
    return rc > 0;
#else
    regex_t re;
    if (regcomp(&re, pattern, REG_EXTENDED) != 0) return 0;
    regmatch_t matches[10];
    int rc = regexec(&re, str, 10, matches, 0);
    if (rc == 0) {
        for (int i = 0; i < 10; i++) {
            if (matches[i].rm_so >= 0) {
                size_t len = matches[i].rm_eo - matches[i].rm_so;
                vm->regex_captures[i] = strndup(str + matches[i].rm_so, len);
            }
        }
    }
    regfree(&re);
    return rc == 0;
#endif
}

/* Find the next match of pattern in str at/after byte offset start.
 * On success sets vm->regex_captures and *mstart/*mend (byte offsets). */
static int vm_regex_find(VM *vm, const char *str, const char *pattern,
                         size_t start, size_t *mstart, size_t *mend) {
    size_t slen = strlen(str);
    if (start > slen) return 0;
    for (int i = 0; i < 10; i++) { if (vm->regex_captures[i]) { free(vm->regex_captures[i]); vm->regex_captures[i] = NULL; } }
    for (int i = 0; i < 10; i++) { if (vm->regex_capture_names[i]) { free(vm->regex_capture_names[i]); vm->regex_capture_names[i] = NULL; } }
#ifdef HAVE_PCRE2
    pcre2_match_data *md;
    pcre2_code *re = regex_cache_get(pattern, &md);
    if (!re) return 0;
    int rc = pcre2_match(re, (PCRE2_SPTR)str, slen, start, 0, md, NULL);
    if (rc > 0) {
        PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
        int count = rc < 10 ? rc : 10;
        for (int i = 0; i < count; i++) {
            if (ov[2*i] != PCRE2_UNSET)
                vm->regex_captures[i] = strndup(str + ov[2*i], ov[2*i+1] - ov[2*i]);
        }

        /* Extract named-group names (PCRE2 name table) */
        {
            uint32_t __nc = 0, __nes = 0;
            PCRE2_SPTR __nt = NULL;
            pcre2_pattern_info(re, PCRE2_INFO_NAMECOUNT, &__nc);
            if (__nc > 0 &&
                pcre2_pattern_info(re, PCRE2_INFO_NAMETABLE, &__nt) == 0 &&
                pcre2_pattern_info(re, PCRE2_INFO_NAMEENTRYSIZE, &__nes) == 0) {
                for (uint32_t __n = 0; __n < __nc; __n++) {
                    PCRE2_SPTR __e = __nt + __n * __nes;
                    int __g = (__e[0] << 8) | __e[1];
                    if (__g >= 0 && __g < 10 && !vm->regex_capture_names[__g])
                        vm->regex_capture_names[__g] = strdup((const char *)(__e + 2));
                }
            }
        }
        *mstart = ov[0]; *mend = ov[1];
        return 1;
    }
    return 0;
#else
    regex_t re;
    if (regcomp(&re, pattern, REG_EXTENDED) != 0) return 0;
    regmatch_t matches[10];
    int rc = regexec(&re, str + start, 10, matches, start > 0 ? REG_NOTBOL : 0);
    if (rc == 0) {
        for (int i = 0; i < 10; i++) {
            if (matches[i].rm_so >= 0)
                vm->regex_captures[i] = strndup(str + start + matches[i].rm_so,
                                                matches[i].rm_eo - matches[i].rm_so);
        }
        *mstart = start + matches[0].rm_so; *mend = start + matches[0].rm_eo;
        regfree(&re);
        return 1;
    }
    regfree(&re);
    return 0;
#endif
}

static VMValue vm_regex_replace(const char *str, const char *pattern, const char *replacement) {
#ifdef HAVE_PCRE2
    pcre2_match_data *md;
    pcre2_code *re = regex_cache_get(pattern, &md);
    if (!re) return vm_str(str);
    int rc = pcre2_match(re, (PCRE2_SPTR)str, strlen(str), 0, 0, md, NULL);
    if (rc > 0) {
        PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
        size_t sl = strlen(str), rl = strlen(replacement);
        size_t before = ov[0], mlen = ov[1] - ov[0];
        size_t nl = sl - mlen + rl;
        VMString *vs = malloc(sizeof(VMString) + nl + 1);
        vm_gc_init_hdr(&vs->hdr, VM_OBJ_STR); vs->len = nl;
        memcpy(vs->data, str, before);
        memcpy(vs->data + before, replacement, rl);
        memcpy(vs->data + before + rl, str + ov[1], sl - ov[1]);
        vs->data[nl] = 0;
        return VM_MAKE_PTR(vs);
    }
    return vm_str(str);
#else
    regex_t re;
    if (regcomp(&re, pattern, REG_EXTENDED) != 0) return vm_str(str);
    regmatch_t match;
    if (regexec(&re, str, 1, &match, 0) == 0) {
        size_t sl = strlen(str), rl = strlen(replacement);
        size_t before = match.rm_so, mlen = match.rm_eo - match.rm_so;
        size_t nl = sl - mlen + rl;
        VMString *vs = malloc(sizeof(VMString) + nl + 1);
        vm_gc_init_hdr(&vs->hdr, VM_OBJ_STR); vs->len = nl;
        memcpy(vs->data, str, before);
        memcpy(vs->data + before, replacement, rl);
        memcpy(vs->data + before + rl, str + match.rm_eo, sl - match.rm_eo);
        vs->data[nl] = 0;
        regfree(&re);
        return VM_MAKE_PTR(vs);
    }
    regfree(&re);
    return vm_str(str);
#endif
}

/* ===== Transliteration ===== */

static VMValue vm_transliterate(const char *str, const char *from, const char *to) {
    /* Expand ranges like a-z with unsigned bounds so bytes >= 0x7f never wrap */
    unsigned char from_exp[1024], to_exp[1024];
    int from_len = 0, to_len = 0;

    for (int i = 0; from[i] && from_len < 1024; i++) {
        if (from[i+1] == '-' && from[i+2]) {
            int start = (unsigned char)from[i], end = (unsigned char)from[i+2];
            for (int c = start; c <= end && from_len < 1024; c++) from_exp[from_len++] = (unsigned char)c;
            i += 2;
        } else {
            from_exp[from_len++] = (unsigned char)from[i];
        }
    }

    for (int i = 0; to[i] && to_len < 1024; i++) {
        if (to[i+1] == '-' && to[i+2]) {
            int start = (unsigned char)to[i], end = (unsigned char)to[i+2];
            for (int c = start; c <= end && to_len < 1024; c++) to_exp[to_len++] = (unsigned char)c;
            i += 2;
        } else {
            to_exp[to_len++] = (unsigned char)to[i];
        }
    }

    /* Build translation table */
    unsigned char table[256];
    for (int i = 0; i < 256; i++) table[i] = (unsigned char)i;
    for (int i = 0; i < from_len; i++) {
        unsigned char fc = from_exp[i];
        table[fc] = (i < to_len) ? to_exp[i] : (to_len > 0 ? to_exp[to_len - 1] : '\0');
    }

    size_t slen = strlen(str);
    VMString *vs = malloc(sizeof(VMString) + slen + 1);
    vm_gc_init_hdr(&vs->hdr, VM_OBJ_STR); vs->len = (uint32_t)slen;
    for (size_t i = 0; i < slen; i++) vs->data[i] = (char)table[(unsigned char)str[i]];
    vs->data[slen] = 0;
    return VM_MAKE_PTR(vs);
}

/* ===== sprintf helper ===== */

#define VM_SPRINTF_ENSURE(need_extra) do { \
    size_t _req = rlen + (need_extra) + 1; \
    if (_req > rcap) { \
        while (rcap < _req) rcap *= 2; \
        result = (char*)realloc(result, rcap); \
    } \
} while (0)

static VMValue vm_sprintf(int argc, VMValue *args) {
    if (argc < 1) return vm_str("");
    char fbuf[64];
    const char *fmt = vm_to_cstr(args[0], fbuf, sizeof(fbuf));

    size_t rcap = 256, rlen = 0;
    char *result = (char*)malloc(rcap);
    int ai = 1;

    for (const char *p = fmt; *p; p++) {
        if (*p == '%' && p[1]) {
            p++;
            if (*p == '%') {
                VM_SPRINTF_ENSURE(1);
                result[rlen++] = '%';
                continue;
            }
            char spec[64]; int si = 0;
            spec[si++] = '%';
            while (*p && strchr("-+ #0", *p) && si < 50) spec[si++] = *p++;
            while (*p && isdigit((unsigned char)*p) && si < 50) spec[si++] = *p++;
            if (*p == '.' && si < 50) {
                spec[si++] = *p++;
                while (*p && isdigit((unsigned char)*p) && si < 50) spec[si++] = *p++;
            }
            if (*p == 'l' && si < 50) spec[si++] = *p++;
            if (!*p) break;
            char conv = *p;
            spec[si++] = conv;
            spec[si] = 0;

            if (ai < argc) {
                if (conv == 'd' || conv == 'i') {
                    spec[si-1] = 0;
                    char tmp[72];
                    snprintf(tmp, sizeof(tmp), "%slld", spec);
                    long long val = (long long)vm_to_int(args[ai++]);
                    int n = snprintf(NULL, 0, tmp, val);
                    if (n > 0) { VM_SPRINTF_ENSURE((size_t)n); rlen += (size_t)snprintf(result + rlen, rcap - rlen, tmp, val); }
                } else if (conv == 'u') {
                    spec[si-1] = 0;
                    char tmp[72];
                    snprintf(tmp, sizeof(tmp), "%sllu", spec);
                    unsigned long long val = (unsigned long long)vm_to_int(args[ai++]);
                    int n = snprintf(NULL, 0, tmp, val);
                    if (n > 0) { VM_SPRINTF_ENSURE((size_t)n); rlen += (size_t)snprintf(result + rlen, rcap - rlen, tmp, val); }
                } else if (conv == 'f' || conv == 'e' || conv == 'E' || conv == 'g' || conv == 'G') {
                    double val = vm_to_num(args[ai++]);
                    int n = snprintf(NULL, 0, spec, val);
                    if (n > 0) { VM_SPRINTF_ENSURE((size_t)n); rlen += (size_t)snprintf(result + rlen, rcap - rlen, spec, val); }
                } else if (conv == 's') {
                    char abuf[64];
                    const char *sval = vm_to_cstr(args[ai++], abuf, sizeof(abuf));
                    int n = snprintf(NULL, 0, spec, sval);
                    if (n > 0) { VM_SPRINTF_ENSURE((size_t)n); rlen += (size_t)snprintf(result + rlen, rcap - rlen, spec, sval); }
                } else if (conv == 'c') {
                    int ch = (int)vm_to_int(args[ai++]);
                    VM_SPRINTF_ENSURE(1);
                    result[rlen++] = (char)(unsigned char)ch;
                } else if (conv == 'x' || conv == 'X' || conv == 'o') {
                    spec[si-1] = 0;
                    char tmp2[72];
                    snprintf(tmp2, sizeof(tmp2), "%sll%c", spec, conv);
                    long long val = (long long)vm_to_int(args[ai++]);
                    int n = snprintf(NULL, 0, tmp2, val);
                    if (n > 0) { VM_SPRINTF_ENSURE((size_t)n); rlen += (size_t)snprintf(result + rlen, rcap - rlen, tmp2, val); }
                } else {
                    VM_SPRINTF_ENSURE(1);
                    result[rlen++] = conv;
                    ai++;
                }
            }
        } else {
            VM_SPRINTF_ENSURE(1);
            result[rlen++] = *p;
        }
    }
    result[rlen] = 0;
    VMValue out = VM_MAKE_STR_N(result, rlen);
    free(result);
    return out;
}
#undef VM_SPRINTF_ENSURE

/* ===== Method dispatch ===== */

static int vm_find_direct_method(VMProgram *prog, const char *cls, const char *method) {
    char fullname[256];
    snprintf(fullname, sizeof(fullname), "%s_%s", cls, method);
    int idx = vm_program_find_func(prog, fullname);
    if (idx >= 0) return idx;
    snprintf(fullname, sizeof(fullname), "%s::%s", cls, method);
    idx = vm_program_find_func(prog, fullname);
    if (idx >= 0) return idx;
    snprintf(fullname, sizeof(fullname), "%s_%s", cls, method);
    if (strchr(fullname, ':')) {
        char sanitized[256];
        int si = 0;
        for (int fi = 0; fullname[fi] && si < 254; fi++) {
            if (fullname[fi] == ':' && fullname[fi+1] == ':') {
                sanitized[si++] = '_';
                fi++;
            } else {
                sanitized[si++] = fullname[fi];
            }
        }
        sanitized[si] = '\0';
        idx = vm_program_find_func(prog, sanitized);
        if (idx >= 0) return idx;
    }
    return -1;
}

static int vm_find_super_method(VMProgram *prog, const char *def_pkg, const char *method, int depth) {
    if (!def_pkg || !def_pkg[0] || depth > 32) return -1;
    for (int i = 0; i < prog->inherit_count; i++) {
        if (strcmp(prog->inherits[i].child, def_pkg) == 0) {
            const char *parent = prog->inherits[i].parent;
            int idx = vm_find_direct_method(prog, parent, method);
            if (idx >= 0) return idx;
            idx = vm_find_super_method(prog, parent, method, depth + 1);
            if (idx >= 0) return idx;
        }
    }
    return -1;
}

static int vm_find_method_only(VMProgram *prog, const char *class_name, const char *method, int depth) {
    if (!class_name || depth > 32) return -1;
    int idx = vm_find_direct_method(prog, class_name, method);
    if (idx >= 0) return idx;
    for (int i = 0; i < prog->inherit_count; i++) {
        if (strcmp(prog->inherits[i].child, class_name) == 0) {
            idx = vm_find_method_only(prog, prog->inherits[i].parent, method, depth + 1);
            if (idx >= 0) return idx;
        }
    }
    return -1;
}

static int vm_find_method(VMProgram *prog, const char *class_name, const char *method) {
    int idx = vm_find_method_only(prog, class_name, method, 0);
    if (idx >= 0) return idx;
    if (class_name && strcmp(class_name, "main") == 0) {
        return vm_program_find_func(prog, method);
    }
    return -1;
}

static int vm_find_around_modifier(VMProgram *prog, const char *cls, const char *method, int depth) {
    if (!cls || depth > 32) return -1;
    for (int i = 0; i < prog->modifier_count; i++) {
        VMModifier *m = &prog->modifiers[i];
        if (m->kind == 2 && strcmp(m->method, method) == 0 && strcmp(m->class_name, cls) == 0) {
            return vm_program_find_func(prog, m->modifier_func);
        }
    }
    for (int i = 0; i < prog->inherit_count; i++) {
        if (strcmp(prog->inherits[i].child, cls) == 0) {
            int idx = vm_find_around_modifier(prog, prog->inherits[i].parent, method, depth + 1);
            if (idx >= 0) return idx;
        }
    }
    return -1;
}

/* Cached method lookup — uses hash of class+method strings */
static int vm_find_method_cached(VM *vm, const char *class_name, const char *method) {
    uint32_t h = vm_hash_fn(class_name) ^ (vm_hash_fn(method) * 2246822519u);
    int slot = h & 31;
    if (vm->method_cache[slot].class_name &&
        strcmp(vm->method_cache[slot].class_name, class_name) == 0 &&
        strcmp(vm->method_cache[slot].method, method) == 0) {
        return vm->method_cache[slot].func_idx;
    }
    int fidx = vm_find_method(vm->program, class_name, method);
    vm->method_cache[slot].class_name = class_name;
    vm->method_cache[slot].method = method;
    vm->method_cache[slot].func_idx = fidx;
    return fidx;
}

/* Find overload method for operator on given class (walking MRO/inheritance) */
static const char *vm_find_overload_cls(VMProgram *prog, const char *class_name, const char *op, int depth, const char **owner_cls_out) {
    if (!class_name || depth > 32) return NULL;
    for (int i = 0; i < prog->overload_count; i++) {
        if (strcmp(prog->overloads[i].class_name, class_name) == 0 &&
            strcmp(prog->overloads[i].op, op) == 0) {
            if (owner_cls_out) *owner_cls_out = prog->overloads[i].class_name;
            return prog->overloads[i].method;
        }
    }
    for (int i = 0; i < prog->inherit_count; i++) {
        if (strcmp(prog->inherits[i].child, class_name) == 0) {
            const char *m = vm_find_overload_cls(prog, prog->inherits[i].parent, op, depth + 1, owner_cls_out);
            if (m) return m;
        }
    }
    return NULL;
}

static const char *vm_find_overload(VMProgram *prog, const char *class_name, const char *op) {
    return vm_find_overload_cls(prog, class_name, op, 0, NULL);
}

/* Check if a value has a stringify overload ("") */
static int vm_has_stringify(VMProgram *prog, VMValue v) {
    if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_HASH) {
        VMHash *h = VM_AS_HASH(v);
        if (h->class_name) {
            const char *method = vm_find_overload(prog, h->class_name, "\"\"");
            if (method) return 1;
        }
    }
    return 0;
}

/* ===== __C__ Block JIT Compilation ===== */

#include <dlfcn.h>
#include <sys/stat.h>
#include <errno.h>

/* Simple hash for cache key */
static uint64_t cblock_hash(const char *code, char **var_names, int var_count) {
    uint64_t h = 14695981039346656037ULL; /* FNV-1a offset */
    for (const char *p = code; *p; p++) { h ^= (uint8_t)*p; h *= 1099511628211ULL; }
    for (int i = 0; i < var_count; i++)
        for (const char *p = var_names[i]; *p; p++) { h ^= (uint8_t)*p; h *= 1099511628211ULL; }
    return h;
}

/* Ensure cache directory exists */
static void ensure_cache_dir(char *path, size_t sz) {
    const char *home = getenv("HOME");
    if (!home) home = "/tmp";
    snprintf(path, sz, "%s/.cache/strada/cblocks", home);
    mkdir(path, 0755); /* ignore errors — parent dirs might not exist */
    /* Create parent dirs */
    char parent[512];
    snprintf(parent, sizeof(parent), "%s/.cache", home);
    mkdir(parent, 0755);
    snprintf(parent, sizeof(parent), "%s/.cache/strada", home);
    mkdir(parent, 0755);
    mkdir(path, 0755);
}

/* Evict cached .so files not accessed in 7 days (max 100 checked per sweep) */
#include <dirent.h>
static void cache_evict_stale(const char *cache_dir) {
    DIR *d = opendir(cache_dir);
    if (!d) return;
    time_t cutoff = time(NULL) - 7 * 24 * 3600;
    struct dirent *ent;
    int checked = 0;
    while ((ent = readdir(d)) && checked < 100) {
        size_t nlen = strlen(ent->d_name);
        if (nlen < 4 || strcmp(ent->d_name + nlen - 3, ".so") != 0) continue;
        char full[600];
        snprintf(full, sizeof(full), "%s/%s", cache_dir, ent->d_name);
        struct stat st;
        if (stat(full, &st) == 0 && st.st_atime < cutoff) {
            unlink(full);
        }
        checked++;
    }
    closedir(d);
}

/* Convert VMValue to StradaValue* for passing to __C__ blocks.
 * Depth-capped: cyclic VM structures (e.g. the weak-ref tests' two-hash
 * cycles) previously recursed until the C stack corrupted the heap. */
static StradaValue *vm_to_sv_depth(VMValue v, int depth) {
    if (VM_IS_INT(v)) return STRADA_MAKE_TAGGED_INT(VM_INT_VAL(v));
    if (VM_IS_UNDEF(v)) return strada_new_undef();
    if (depth >= 32) return strada_new_undef();
    if (VM_IS_PTR(v)) {
        switch (VM_PTR_TYPE(v)) {
        case VM_OBJ_FLOAT: return strada_new_num(VM_AS_FLOAT(v)->val);
        case VM_OBJ_STR: return strada_new_str_len(VM_AS_STR(v)->data, VM_AS_STR(v)->len);
        case VM_OBJ_ARRAY: {
            /* Convert VM array to Strada array */
            VMArray *va = VM_AS_ARRAY(v);
            StradaValue *sa = strada_new_array();
            for (int i = 0; i < va->size; i++)
                strada_array_push_take(sa->value.av, vm_to_sv_depth(va->items[i], depth + 1));
            return sa;
        }
        case VM_OBJ_HASH: {
            VMHash *vh = VM_AS_HASH(v);
            StradaValue *sh = strada_new_hash();
            for (int i = 0; i < vh->capacity; i++)
                if (vh->entries[i].occupied == 1)
                    strada_hash_set_take(sh->value.hv, vh->entries[i].key, vm_to_sv_depth(vh->entries[i].value, depth + 1));
            if (vh->class_name) {
                strada_hash_set_take(sh->value.hv, "__class__", strada_new_str(vh->class_name));
                strada_bless(sh, vh->class_name);
            }
            return sh;
        }
        case VM_OBJ_STRBUF: return strada_new_str_len(VM_AS_STRBUF(v)->data, VM_AS_STRBUF(v)->len);
        case VM_OBJ_NATIVE_SV: {
            VMNativeSV *nsv = (VMNativeSV*)VM_TO_PTR(v);
            strada_incref(nsv->sv);
            return nsv->sv;
        }
        case VM_OBJ_CELL:
            return vm_to_sv_depth(VM_AS_CELL(v)->val, depth + 1);
        case VM_OBJ_FILEHANDLE: {
            /* Filehandles pass through as opaque pointers; caller clears value.fh before decref */
            VMFileHandle *fh = (VMFileHandle*)VM_TO_PTR(v);
            StradaValue *sv = strada_new_undef();
            sv->type = STRADA_FILEHANDLE;
            sv->value.fh = fh->fp;
            return sv;
        }
        default: return strada_new_undef();
        }
    }
    return strada_new_undef();
}

static StradaValue *vm_to_sv(VMValue v) {
    return vm_to_sv_depth(v, 0);
}

struct StradaValue *vm_value_to_sv(VMValue v) {
    return vm_to_sv(v);
}

/* Convert StradaValue* back to VMValue.
 * Plain ARRAY/HASH results (and REFs to them) deep-convert into VM
 * containers so element access works — the VM's index/hash-get paths do not
 * understand wrapped native SVs. Opaque types (sockets, regexes, pointers)
 * still wrap as VMNativeSV and round-trip through vm_to_sv by identity.
 * Depth-capped: cyclic structures fall back to wrapping. */
static VMValue sv_to_vm_depth(StradaValue *sv, int depth) {
    if (!sv) return VM_UNDEF_VAL;
    if (STRADA_IS_TAGGED_INT(sv)) return VM_MAKE_INT(STRADA_TAGGED_INT_VAL(sv));
    switch (sv->type) {
    case STRADA_INT: return VM_MAKE_INT(sv->value.iv);
    case STRADA_NUM: return VM_MAKE_FLOAT(sv->value.nv);
    case STRADA_STR: {
        if (!sv->value.pv) return VM_UNDEF_VAL;
        return VM_MAKE_STR_N(sv->value.pv, strada_str_len(sv));
    }
    case STRADA_UNDEF: return VM_UNDEF_VAL;
    case STRADA_ARRAY: {
        if (depth >= 32) break;
        StradaArray *av = sv->value.av;
        VMArray *va = vm_array_new((int)(av ? av->size : 0) + 1);
        if (av) {
            for (size_t i = 0; i < av->size; i++)
                vm_array_push(va, sv_to_vm_depth(strada_array_get(av, (int64_t)i), depth + 1));
        }
        return VM_MAKE_PTR(va);
    }
    case STRADA_HASH: {
        if (depth >= 32) break;
        StradaHash *hv = sv->value.hv;
        VMHash *h = vm_hash_new(16);
        if (hv && hv->entries) {
            /* entries[] is initialized only up to next_slot (the append
             * high-water mark); beyond it is uninitialized memory. Deleted
             * slots inside the region have key == NULL. */
            for (size_t i = 0; i < hv->next_slot; i++) {
                if (hv->entries[i].key == NULL) continue;
                vm_hash_set(h, hv->entries[i].key->data,
                            sv_to_vm_depth(hv->entries[i].value, depth + 1));
            }
        }
        if (sv->meta && sv->meta->blessed_package)
            h->class_name = strdup(sv->meta->blessed_package);
        return VM_MAKE_PTR(h);
    }
    case STRADA_REF: {
        /* Keep blessed refs wrapped as VMNativeSV so native OOP objects
         * retain slot metadata and dispatch via strada_method_call. */
        if (sv->meta && sv->meta->blessed_package) break;
        /* Deref to the container so VM `->[i]` / `->{k}` access works;
         * refs to scalars and opaque targets stay wrapped. */
        if (depth >= 32) break;
        StradaValue *rv = sv->value.rv;
        if (rv && !STRADA_IS_TAGGED_INT(rv) &&
            (rv->type == STRADA_ARRAY || rv->type == STRADA_HASH))
            return sv_to_vm_depth(rv, depth + 1);
        break;
    }
    default: break;
    }
    /* Wrap remaining types (REF-to-scalar, SOCKET, REGEX, etc.) as VMNativeSV */
    strada_incref(sv);
    VMNativeSV *nsv = malloc(sizeof(VMNativeSV));
    vm_gc_init_hdr(&nsv->hdr, VM_OBJ_NATIVE_SV);
    nsv->sv = sv;
    return VM_MAKE_PTR(nsv);
}

static VMValue sv_to_vm(StradaValue *sv) {
    return sv_to_vm_depth(sv, 0);
}

/* JIT-compile a __C__ block and cache the .so */
static int eviction_done = 0;

static void shell_quote_arg(const char *src, char *dst, size_t dstsz) {
    size_t di = 0;
    if (di + 1 < dstsz) dst[di++] = '\'';
    for (size_t i = 0; src && src[i]; i++) {
        if (src[i] == '\'') {
            if (di + 4 >= dstsz) break;
            dst[di++] = '\''; dst[di++] = '\\'; dst[di++] = '\''; dst[di++] = '\'';
        } else {
            if (di + 2 >= dstsz) break;
            dst[di++] = src[i];
        }
    }
    if (di + 1 < dstsz) dst[di++] = '\'';
    dst[di] = '\0';
}

static int cblock_jit_compile(VMCBlock *cb, const char *runtime_inc, const char *prelude) {
    char cache_dir[512];
    ensure_cache_dir(cache_dir, sizeof(cache_dir));

    /* Run eviction once per process */
    if (!eviction_done) { cache_evict_stale(cache_dir); eviction_done = 1; }

    uint64_t hash = cblock_hash(cb->code, cb->var_names, cb->var_count);
    if (prelude) {
        /* prelude participates in the cache key */
        for (const char *pp = prelude; *pp; pp++) hash = hash * 1099511628211ULL ^ (unsigned char)*pp;
    }
    char so_path[600];
    snprintf(so_path, sizeof(so_path), "%s/%016llx.so", cache_dir, (unsigned long long)hash);

    /* Check cache */
    struct stat st;
    if (stat(so_path, &st) == 0 && st.st_size > 0) {
        cb->dl_handle = dlopen(so_path, RTLD_NOW);
        if (cb->dl_handle) {
            cb->fn = (void (*)(void**))dlsym(cb->dl_handle, "__cblock_entry");
            if (cb->fn) return 0; /* cache hit */
            dlclose(cb->dl_handle);
            cb->dl_handle = NULL;
        }
        /* Cache corrupt — recompile */
    }

    /* Private per-compile temporary directory */
    char tmpdir[] = "/tmp/strada_cblock_XXXXXX";
    if (!mkdtemp(tmpdir)) {
        fprintf(stderr, "__C__ JIT: mkdtemp failed\n");
        return -1;
    }

    /* Generate wrapper C source */
    char src_path[640], tmp_so[640];
    snprintf(src_path, sizeof(src_path), "%s/block.c", tmpdir);
    snprintf(tmp_so, sizeof(tmp_so), "%s/block.so", tmpdir);
    FILE *f = fopen(src_path, "w");
    if (!f) { fprintf(stderr, "__C__ JIT: cannot create %s\n", src_path); rmdir(tmpdir); return -1; }

    fprintf(f, "#include \"strada_runtime.h\"\n\n");
    if (prelude && prelude[0]) {
        fprintf(f, "/* program-level __C__ blocks (file-scope helpers) */\n%s\n", prelude);
    }
    fprintf(f, "void __cblock_entry(StradaValue **__vars) {\n");
    /* Declare local variables from the __vars array */
    for (int i = 0; i < cb->var_count; i++) {
        fprintf(f, "    StradaValue *%s = __vars[%d];\n", cb->var_names[i], i);
    }
    fprintf(f, "\n    /* User code */\n");
    fprintf(f, "%s\n", cb->code);
    fprintf(f, "\n    /* Copy back modified variables */\n");
    for (int i = 0; i < cb->var_count; i++) {
        fprintf(f, "    __vars[%d] = %s;\n", i, cb->var_names[i]);
    }
    fprintf(f, "}\n");
    fclose(f);

    /* Compile to shared library with shell-quoted arguments */
    const char *inc = runtime_inc ? runtime_inc : ".";
    char q_inc[1024], q_so[1024], q_src[1024];
    shell_quote_arg(inc, q_inc, sizeof(q_inc));
    shell_quote_arg(tmp_so, q_so, sizeof(q_so));
    shell_quote_arg(src_path, q_src, sizeof(q_src));
    char cmd[3600];
    snprintf(cmd, sizeof(cmd),
        "gcc -shared -fPIC -O2 -I%s -o %s %s -w 2>&1",
        q_inc, q_so, q_src);

    FILE *proc = popen(cmd, "r");
    if (!proc) {
        fprintf(stderr, "__C__ JIT: cannot run gcc\n");
        unlink(src_path); rmdir(tmpdir);
        return -1;
    }
    char errbuf[4096]; size_t errlen = 0;
    errlen = fread(errbuf, 1, sizeof(errbuf)-1, proc);
    errbuf[errlen] = '\0';
    int rc = pclose(proc);
    unlink(src_path); /* clean up source */

    if (rc != 0) {
        fprintf(stderr, "__C__ JIT: compilation failed:\n%s", errbuf);
        unlink(tmp_so); rmdir(tmpdir);
        return -1;
    }

    const char *load_path = so_path;
    if (rename(tmp_so, so_path) != 0) {
        /* Fallback copy if /tmp and cache_dir are across filesystems */
        FILE *in = fopen(tmp_so, "rb");
        FILE *out = in ? fopen(so_path, "wb") : NULL;
        int copy_ok = 0;
        if (in && out) {
            char cbuf[8192];
            size_t nr;
            copy_ok = 1;
            while ((nr = fread(cbuf, 1, sizeof(cbuf), in)) > 0) {
                if (fwrite(cbuf, 1, nr, out) != nr) { copy_ok = 0; break; }
            }
        }
        if (in) fclose(in);
        if (out) fclose(out);
        unlink(tmp_so);
        if (!copy_ok) { rmdir(tmpdir); return -1; }
    }
    rmdir(tmpdir);

    /* Load the compiled .so */
    cb->dl_handle = dlopen(load_path, RTLD_NOW);
    if (!cb->dl_handle) {
        fprintf(stderr, "__C__ JIT: dlopen failed: %s\n", dlerror());
        return -1;
    }
    cb->fn = (void (*)(void**))dlsym(cb->dl_handle, "__cblock_entry");
    if (!cb->fn) {
        fprintf(stderr, "__C__ JIT: dlsym failed: %s\n", dlerror());
        dlclose(cb->dl_handle);
        cb->dl_handle = NULL;
        return -1;
    }
    return 0;
}

/* ===== VM Execution ===== */

/* Grow the VM value stack when a push would exceed stack_cap. The original
 * SP_PUSH had NO bounds check, so input-driven recursion / expression depth
 * overflowed the heap-allocated stack (a wild OOB write). Frame stack_base
 * values are size_t indices (stable across realloc); vm_execute refreshes its
 * cached `stack` pointer right after this returns. A hard cap converts true
 * runaway recursion into a clean fatal error instead of unbounded growth. */
#define VM_STACK_MAX ((size_t)64 * 1024 * 1024)
static void vm_stack_grow(VM *vm, size_t need) {
    size_t newcap = vm->stack_cap ? vm->stack_cap : 1024;
    while (newcap <= need) {
        if (newcap >= VM_STACK_MAX) {
            fprintf(stderr, "VM: value stack overflow (runaway recursion or expression depth)\n");
            exit(1);
        }
        newcap *= 2;
    }
    if (newcap > VM_STACK_MAX) newcap = VM_STACK_MAX;
    if (need >= newcap) {
        fprintf(stderr, "VM: value stack overflow (runaway recursion or expression depth)\n");
        exit(1);
    }
    VMValue *ns = realloc(vm->stack, newcap * sizeof(VMValue));
    if (!ns) { fprintf(stderr, "VM: out of memory growing value stack\n"); exit(1); }
    memset(ns + vm->stack_cap, 0, (newcap - vm->stack_cap) * sizeof(VMValue));
    vm->stack = ns;
    vm->stack_cap = newcap;
}

static void vm_unwind_global_saves(VM *vm, int target_depth) {
    if (!vm || target_depth < 0) return;
    while (vm->global_save_count > target_depth) {
        VMGlobalSave *gs = &vm->global_saves[--vm->global_save_count];
        if (gs->is_hash_elem) {
            if (gs->hash && gs->hash_key) {
                if (gs->existed) {
                    vm_hash_set(gs->hash, gs->hash_key, gs->saved_value);
                } else {
                    vm_hash_delete(gs->hash, gs->hash_key);
                }
            }
            free(gs->hash_key);
            gs->hash_key = NULL;
            gs->hash = NULL;
        } else if (gs->name) {
            vm_hash_set(vm->globals, gs->name, gs->saved_value);
            free(gs->name);
            gs->name = NULL;
        }
    }
}

void vm_run_end_blocks(VM *vm) {
    if (!vm || vm->end_ran) return;
    vm->end_ran = 1;
    if (!vm->program || vm->program->end_count <= 0) return;
    int ec = vm->program->end_count;
    int *blocks = vm->program->end_blocks;
    vm->program->end_count = 0;
    vm->program->end_blocks = NULL;
    for (int i = ec - 1; i >= 0; i--) {
        vm_execute(vm, vm->program->func_names[blocks[i]]);
    }
    free(blocks);
}

static VMValue vm_invoke_native_entry(VM *vm, VMNativeEntry *ne, VMValue *raw_args, int argc, VMHash *sync_self_hash) {
    StradaValue *sv_buf[16];
    StradaValue **sv_args = (argc > 16) ? malloc((size_t)argc * sizeof(StradaValue*)) : sv_buf;
    for (int i = 0; i < argc; i++) {
        sv_args[i] = vm_to_sv(raw_args[i]);
    }

    size_t namelen = strlen(ne->name);
    int is_constructor = (!ne->is_variadic && ne->param_count == 0 &&
                          namelen >= 4 && strcmp(ne->name + namelen - 4, "_new") == 0);

    int max_call = argc + (ne->param_count > argc ? ne->param_count - argc : 0) + 2;
    StradaValue *call_buf[16];
    StradaValue **call_args = (max_call > 16) ? malloc((size_t)max_call * sizeof(StradaValue*)) : call_buf;
    StradaValue *pad_buf[16];
    StradaValue **pads = (max_call > 16) ? malloc((size_t)max_call * sizeof(StradaValue*)) : pad_buf;
    int npad = 0;
    int call_argc = 0;
    StradaValue *va_arr = NULL;

    if (ne->is_variadic == 1) {
        int variadic_idx = ne->param_count > 0 ? ne->param_count - 1 : 0;
        for (int i = 0; i < variadic_idx; i++) {
            call_args[i] = (i < argc) ? sv_args[i] : (pads[npad++] = strada_new_undef());
        }
        va_arr = strada_new_array();
        for (int i = variadic_idx; i < argc; i++) {
            strada_incref(sv_args[i]);
            strada_array_push_take(va_arr->value.av, sv_args[i]);
        }
        call_args[variadic_idx] = va_arr;
        call_argc = variadic_idx + 1;
    } else if (is_constructor) {
        va_arr = strada_new_array();
        for (int i = 0; i < argc; i++) {
            strada_incref(sv_args[i]);
            strada_array_push_take(va_arr->value.av, sv_args[i]);
        }
        call_args[0] = va_arr;
        call_argc = 1;
    } else {
        int expected = ne->param_count > argc ? ne->param_count : argc;
        for (int i = 0; i < argc; i++) call_args[i] = sv_args[i];
        for (int i = argc; i < expected; i++) call_args[i] = (pads[npad++] = strada_new_undef());
        call_argc = expected;
    }

    typedef StradaValue* (*NativeFunc0)(void);
    typedef StradaValue* (*NativeFunc1)(StradaValue*);
    typedef StradaValue* (*NativeFunc2)(StradaValue*, StradaValue*);
    typedef StradaValue* (*NativeFunc3)(StradaValue*, StradaValue*, StradaValue*);
    typedef StradaValue* (*NativeFunc4)(StradaValue*, StradaValue*, StradaValue*, StradaValue*);
    typedef StradaValue* (*NativeFunc5)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);
    typedef StradaValue* (*NativeFunc6)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);
    typedef StradaValue* (*NativeFunc7)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);
    typedef StradaValue* (*NativeFunc8)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);
    typedef void (*VoidNativeFunc0)(void);
    typedef void (*VoidNativeFunc1)(StradaValue*);
    typedef void (*VoidNativeFunc2)(StradaValue*, StradaValue*);
    typedef void (*VoidNativeFunc3)(StradaValue*, StradaValue*, StradaValue*);
    typedef void (*VoidNativeFunc4)(StradaValue*, StradaValue*, StradaValue*, StradaValue*);
    typedef void (*VoidNativeFunc5)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);
    typedef void (*VoidNativeFunc6)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);
    typedef void (*VoidNativeFunc7)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);
    typedef void (*VoidNativeFunc8)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);

    StradaValue *nr = NULL;
    vm->gc_pause++;
    if (ne->return_type == 7) {
        switch (call_argc) {
        case 0: ((VoidNativeFunc0)ne->sym)(); break;
        case 1: ((VoidNativeFunc1)ne->sym)(call_args[0]); break;
        case 2: ((VoidNativeFunc2)ne->sym)(call_args[0], call_args[1]); break;
        case 3: ((VoidNativeFunc3)ne->sym)(call_args[0], call_args[1], call_args[2]); break;
        case 4: ((VoidNativeFunc4)ne->sym)(call_args[0], call_args[1], call_args[2], call_args[3]); break;
        case 5: ((VoidNativeFunc5)ne->sym)(call_args[0], call_args[1], call_args[2], call_args[3], call_args[4]); break;
        case 6: ((VoidNativeFunc6)ne->sym)(call_args[0], call_args[1], call_args[2], call_args[3], call_args[4], call_args[5]); break;
        case 7: ((VoidNativeFunc7)ne->sym)(call_args[0], call_args[1], call_args[2], call_args[3], call_args[4], call_args[5], call_args[6]); break;
        case 8: ((VoidNativeFunc8)ne->sym)(call_args[0], call_args[1], call_args[2], call_args[3], call_args[4], call_args[5], call_args[6], call_args[7]); break;
        default: fprintf(stderr, "VM: native call with %d args not supported (max 8)\n", call_argc); break;
        }
    } else {
        switch (call_argc) {
        case 0: nr = ((NativeFunc0)ne->sym)(); break;
        case 1: nr = ((NativeFunc1)ne->sym)(call_args[0]); break;
        case 2: nr = ((NativeFunc2)ne->sym)(call_args[0], call_args[1]); break;
        case 3: nr = ((NativeFunc3)ne->sym)(call_args[0], call_args[1], call_args[2]); break;
        case 4: nr = ((NativeFunc4)ne->sym)(call_args[0], call_args[1], call_args[2], call_args[3]); break;
        case 5: nr = ((NativeFunc5)ne->sym)(call_args[0], call_args[1], call_args[2], call_args[3], call_args[4]); break;
        case 6: nr = ((NativeFunc6)ne->sym)(call_args[0], call_args[1], call_args[2], call_args[3], call_args[4], call_args[5]); break;
        case 7: nr = ((NativeFunc7)ne->sym)(call_args[0], call_args[1], call_args[2], call_args[3], call_args[4], call_args[5], call_args[6]); break;
        case 8: nr = ((NativeFunc8)ne->sym)(call_args[0], call_args[1], call_args[2], call_args[3], call_args[4], call_args[5], call_args[6], call_args[7]); break;
        default: fprintf(stderr, "VM: native call with %d args not supported (max 8)\n", call_argc); break;
        }
    }
    vm->gc_pause--;

    VMValue ret = nr ? sv_to_vm(nr) : VM_UNDEF_VAL;
    if (nr) strada_decref(nr);

    if (sync_self_hash && argc >= 1 && sv_args[0] && !STRADA_IS_TAGGED_INT(sv_args[0])) {
        StradaValue *ss = sv_args[0];
        if (ss->type == STRADA_REF && ss->value.rv) ss = ss->value.rv;
        if (ss && !STRADA_IS_TAGGED_INT(ss) && ss->type == STRADA_HASH && ss->value.hv) {
            StradaHash *sshv = ss->value.hv;
            for (size_t cb = 0; cb < sshv->next_slot; cb++) {
                StradaHashEntry *ce = &sshv->entries[cb];
                if (ce->key && ce->key->data) {
                    vm_hash_set(sync_self_hash, ce->key->data, sv_to_vm(ce->value));
                }
            }
        }
    }

    if (va_arr) strada_decref(va_arr);
    for (int i = 0; i < npad; i++) strada_decref(pads[i]);
    for (int i = 0; i < argc; i++) {
        if (sv_args[i]) {
            if (VM_IS_PTR(raw_args[i]) && VM_PTR_TYPE(raw_args[i]) == VM_OBJ_FILEHANDLE &&
                !STRADA_IS_TAGGED_INT(sv_args[i])) {
                sv_args[i]->value.fh = NULL;
            }
            strada_decref(sv_args[i]);
        }
    }
    if (sv_args != sv_buf) free(sv_args);
    if (call_args != call_buf) free(call_args);
    if (pads != pad_buf) free(pads);
    return ret;
}

static inline const char *vm_bytes_view(VMValue v, char *buf, size_t bufsz, size_t *out_len) {
    if (VM_IS_PTR(v)) {
        enum VMObjType t = VM_PTR_TYPE(v);
        if (t == VM_OBJ_STR) {
            VMString *vs = VM_AS_STR(v);
            *out_len = vs->len;
            return vs->data;
        }
        if (t == VM_OBJ_STRBUF) {
            VMStrBuf *vsb = VM_AS_STRBUF(v);
            *out_len = vsb->len;
            return vsb->data;
        }
    }
    const char *s = vm_to_cstr(v, buf, bufsz);
    *out_len = strlen(s);
    return s;
}

static VMValue vm_invoke_func(VM *vm, int fidx, VMValue *args, int argc) {
    VMChunk *callee = &vm->program->funcs[fidx];
    if (callee->has_variadic) {
        int fixed = callee->fixed_param_count;
        if (fixed < 0) fixed = 0;
        int vc = argc - fixed;
        VMArray *va = vm_array_new(vc > 0 ? vc : 4);
        for (int i = fixed; i < argc; i++) vm_array_push(va, vm_val_copy(args[i]));
        VMValue pbuf[16];
        int pargc = fixed + 1;
        VMValue *pargs = (pargc > 16) ? malloc((size_t)pargc * sizeof(VMValue)) : pbuf;
        for (int i = 0; i < fixed; i++) {
            pargs[i] = (i < argc) ? args[i] : VM_UNDEF_VAL;
        }
        pargs[fixed] = VM_MAKE_PTR(va);
        VMValue res = vm_execute_call(vm, fidx, pargs, pargc);
        if (pargs != pbuf) free(pargs);
        return res;
    }
    return vm_execute_call(vm, fidx, args, argc);
}

static int vm_resolve_overload_func(VMProgram *prog, const char *class_name, const char *op) {
    const char *owner = NULL;
    const char *method = vm_find_overload_cls(prog, class_name, op, 0, &owner);
    if (!method) return -1;
    int fidx = vm_find_method_only(prog, class_name, method, 0);
    if (fidx < 0 && owner) fidx = vm_find_method_only(prog, owner, method, 0);
    if (fidx < 0) fidx = vm_program_find_func(prog, method);
    return fidx;
}

static int vm_try_bin_overload(VM *vm, const char *op, VMValue a, VMValue b, VMValue *out) {
    int a_blessed = (VM_IS_PTR(a) && VM_PTR_TYPE(a) == VM_OBJ_HASH && VM_AS_HASH(a)->class_name != NULL);
    int b_blessed = (VM_IS_PTR(b) && VM_PTR_TYPE(b) == VM_OBJ_HASH && VM_AS_HASH(b)->class_name != NULL);
    if (!a_blessed && !b_blessed) return 0;

    if (a_blessed) {
        int fidx = vm_resolve_overload_func(vm->program, VM_AS_HASH(a)->class_name, op);
        if (fidx >= 0) {
            VMValue args[3] = { a, b, VM_MAKE_INT(0) };
            *out = vm_invoke_func(vm, fidx, args, 3);
            return 1;
        }
    }
    if (b_blessed) {
        int fidx = vm_resolve_overload_func(vm->program, VM_AS_HASH(b)->class_name, op);
        if (fidx >= 0) {
            VMValue args[3] = { b, a, VM_MAKE_INT(1) };
            *out = vm_invoke_func(vm, fidx, args, 3);
            return 1;
        }
    }

    const char *fb_op = NULL;
    int is_num_cmp = 0;
    if (strcmp(op, "==") == 0 || strcmp(op, "!=") == 0 ||
        strcmp(op, "<") == 0 || strcmp(op, "<=") == 0 ||
        strcmp(op, ">") == 0 || strcmp(op, ">=") == 0) {
        fb_op = "<=>";
        is_num_cmp = 1;
    } else if (strcmp(op, "eq") == 0 || strcmp(op, "ne") == 0 ||
               strcmp(op, "lt") == 0 || strcmp(op, "le") == 0 ||
               strcmp(op, "gt") == 0 || strcmp(op, "ge") == 0) {
        fb_op = "cmp";
    }
    if (!fb_op) return 0;

    int fidx = -1;
    VMValue args[3];
    if (a_blessed) {
        fidx = vm_resolve_overload_func(vm->program, VM_AS_HASH(a)->class_name, fb_op);
        if (fidx >= 0) { args[0] = a; args[1] = b; args[2] = VM_MAKE_INT(0); }
    }
    if (fidx < 0 && b_blessed) {
        fidx = vm_resolve_overload_func(vm->program, VM_AS_HASH(b)->class_name, fb_op);
        if (fidx >= 0) { args[0] = b; args[1] = a; args[2] = VM_MAKE_INT(1); }
    }
    if (fidx < 0) return 0;

    VMValue res = vm_invoke_func(vm, fidx, args, 3);
    if (vm->has_pending_exc) { *out = VM_UNDEF_VAL; return 1; }
    int64_t r = vm_to_int(res);
    int bool_val = 0;
    if (is_num_cmp) {
        if (strcmp(op, "<") == 0) bool_val = (r < 0);
        else if (strcmp(op, "<=") == 0) bool_val = (r <= 0);
        else if (strcmp(op, ">") == 0) bool_val = (r > 0);
        else if (strcmp(op, ">=") == 0) bool_val = (r >= 0);
        else if (strcmp(op, "==") == 0) bool_val = (r == 0);
        else bool_val = (r != 0);
    } else {
        if (strcmp(op, "lt") == 0) bool_val = (r < 0);
        else if (strcmp(op, "le") == 0) bool_val = (r <= 0);
        else if (strcmp(op, "gt") == 0) bool_val = (r > 0);
        else if (strcmp(op, "ge") == 0) bool_val = (r >= 0);
        else if (strcmp(op, "eq") == 0) bool_val = (r == 0);
        else bool_val = (r != 0);
    }
    *out = VM_MAKE_INT(bool_val);
    return 1;
}

static int vm_try_un_overload(VM *vm, const char *op, VMValue a, VMValue *out) {
    if (!(VM_IS_PTR(a) && VM_PTR_TYPE(a) == VM_OBJ_HASH && VM_AS_HASH(a)->class_name != NULL))
        return 0;
    int fidx = vm_resolve_overload_func(vm->program, VM_AS_HASH(a)->class_name, op);
    if (fidx < 0) return 0;
    VMValue args[1] = { a };
    *out = vm_invoke_func(vm, fidx, args, 1);
    return 1;
}

static VMValue vm_stringify_ov(VM *vm, VMValue v) {
    if (vm_has_stringify(vm->program, v)) {
        VMHash *h = VM_AS_HASH(v);
        int fidx = vm_resolve_overload_func(vm->program, h->class_name, "\"\"");
        if (fidx >= 0) {
            vm->gc_pause++;
            VMValue args[1] = { v };
            VMValue res = vm_invoke_func(vm, fidx, args, 1);
            vm->gc_pause--;
            return res;
        }
    }
    return v;
}

VMValue vm_execute(VM *vm, const char *func_name) {
    int func_idx = vm_program_find_func(vm->program, func_name);
    if (func_idx < 0) { fprintf(stderr, "VM error: function '%s' not found\n", func_name); return VM_UNDEF_VAL; }

    /* Run BEGIN blocks first (only once) */
    if (vm->program->begin_count > 0) {
        int bc = vm->program->begin_count;
        int *blocks = vm->program->begin_blocks;
        vm->program->begin_count = 0; /* prevent re-entry */
        vm->program->begin_blocks = NULL;
        for (int i = 0; i < bc; i++) {
            vm_execute(vm, vm->program->func_names[blocks[i]]);
            if (vm->has_exception) {
                free(blocks);
                return VM_MAKE_INT(1);
            }
        }
        free(blocks);
    }

    if (strcmp(func_name, "main") == 0 && vm->program->funcs[func_idx].fixed_param_count >= 2) {
        int extra = vm->argv ? vm->argv->size : 0;
        VMArray *margv = vm_array_new(extra + 2);
        vm_array_push(margv, vm_str(vm->script_name ? vm->script_name : "strada"));
        for (int i = 0; i < extra; i++) {
            vm_array_push(margv, vm_val_copy(vm->argv->items[i]));
        }
        VMValue cargs[2];
        cargs[0] = VM_MAKE_INT(margv->size);
        cargs[1] = VM_MAKE_PTR(margv);
        return vm_execute_call(vm, func_idx, cargs, 2);
    }

    return vm_execute_call(vm, func_idx, NULL, 0);
}

VMValue vm_execute_call(VM *vm, int func_idx, VMValue *cargs, int cargc) {
    VM *prev_vm = g_active_vm;
    g_active_vm = vm;
    size_t saved_exec_base = vm->exec_base;
    vm->exec_base = vm->frame_count;
    #define VM_EXEC_RETURN(val) do { g_active_vm = prev_vm; return (val); } while(0)

    VMChunk *chunk = &vm->program->funcs[func_idx];
    if (vm->frame_count >= vm->frame_cap) {
        vm->frame_cap *= 2;
        vm->frames = realloc(vm->frames, vm->frame_cap * sizeof(VMFrame));
    }
    VMFrame *frame = &vm->frames[vm->frame_count++];
    frame->chunk = chunk;
    frame->ip = chunk->code;
    frame->locals = locals_alloc(chunk->local_count + 1, chunk->int_only);
    frame->stack_base = vm->stack_top;
    frame->closure = NULL;
    frame->global_save_depth = vm->global_save_count;
    for (int ci = 0; ci < cargc && ci < chunk->local_count; ci++)
        frame->locals[ci] = cargs[ci];

    register uint8_t *ip = frame->ip;
    VMValue *locals = frame->locals;
    VMValue *stack = vm->stack;
    size_t sp = vm->stack_top;

    #define SP_PUSH(v) do { \
        if (__builtin_expect(sp >= vm->stack_cap, 0)) { vm_stack_grow(vm, sp); stack = vm->stack; } \
        stack[sp++] = (v); \
    } while (0)
    #define SP_POP()   (stack[--sp])

    #define RELOAD() do { \
        frame = &vm->frames[vm->frame_count - 1]; \
        chunk = frame->chunk; ip = frame->ip; locals = frame->locals; sp = vm->stack_top; \
    } while(0)

    /* Exception handling via longjmp */
    jmp_buf exc_jmp;
    jmp_buf *saved_jmp = NULL;
    uint16_t call_fidx = 0;
    int call_argc = 0;
    uint16_t mcall_idx = 0;
    int mcall_argc = 0;
    const char *mcall_method_cstr = NULL;
    int mcall_is_dyn = 0;
    char mcall_dyn_buf[256];
    VMValue throw_exc = VM_UNDEF_VAL;
    VMValue clcall_val = VM_UNDEF_VAL;
    int clcall_argc = 0;

#ifdef __GNUC__
    /* Build dispatch table for ALL opcodes up to OP_OPCODE_COUNT */
    static const void *dispatch_table[256] = { NULL };
    static int dt_init = 0;
    if (!dt_init) {
        for (int i = 0; i < 256; i++) dispatch_table[i] = &&L_OP_NOP;
        #define DT(op) dispatch_table[op] = &&L_##op
        DT(OP_NOP); DT(OP_PUSH_INT); DT(OP_PUSH_STR); DT(OP_PUSH_UNDEF);
        DT(OP_POP); DT(OP_DUP); DT(OP_LOAD_LOCAL); DT(OP_STORE_LOCAL);
        DT(OP_ADD); DT(OP_SUB); DT(OP_MUL); DT(OP_DIV); DT(OP_MOD);
        DT(OP_CONCAT); DT(OP_EQ); DT(OP_NE); DT(OP_LT); DT(OP_GT);
        DT(OP_LE); DT(OP_GE); DT(OP_STR_EQ); DT(OP_STR_NE);
        DT(OP_JMP); DT(OP_JMP_IF_FALSE); DT(OP_JMP_IF_TRUE);
        DT(OP_CALL); DT(OP_RETURN); DT(OP_SAY); DT(OP_PRINT);
        DT(OP_INCR); DT(OP_DECR);
        DT(OP_NEW_ARRAY); DT(OP_ARRAY_PUSH); DT(OP_ARRAY_GET);
        DT(OP_ARRAY_SET); DT(OP_ARRAY_SIZE);
        DT(OP_NEW_HASH); DT(OP_HASH_GET); DT(OP_HASH_SET);
        DT(OP_HASH_DELETE); DT(OP_HASH_KEYS);
        DT(OP_STR_LEN); DT(OP_STR_SPLIT); DT(OP_STR_SPLIT_LIMIT); DT(OP_STR_REPLACE);
        DT(OP_HASH_BLESS); DT(OP_HASH_REF);
        DT(OP_IS_UNDEF); DT(OP_ISA); DT(OP_APPEND_LOCAL);
        DT(OP_HASH_GET_CONCAT); DT(OP_HASH_SET_CONCAT); DT(OP_HASH_DEL_CONCAT);
        DT(OP_HALT);
        DT(OP_BUILTIN); DT(OP_MAKE_CLOSURE); DT(OP_LOAD_CAPTURE); DT(OP_STORE_CAPTURE);
        DT(OP_CALL_CLOSURE);
        DT(OP_REGEX_MATCH); DT(OP_REGEX_NOT_MATCH); DT(OP_LOAD_CAPTURE_VAR);
        DT(OP_TRY_BEGIN); DT(OP_TRY_END); DT(OP_THROW);
        DT(OP_SUBSTR); DT(OP_STR_INDEX); DT(OP_STR_UPPER); DT(OP_STR_LOWER);
        DT(OP_CHR); DT(OP_ORD); DT(OP_CHOMP); DT(OP_TR); DT(OP_STR_REPEAT);
        DT(OP_SAY_FH); DT(OP_PRINT_FH);
        DT(OP_LOAD_GLOBAL); DT(OP_STORE_GLOBAL); DT(OP_SAVE_GLOBAL); DT(OP_RESTORE_GLOBAL);
        DT(OP_METHOD_CALL); DT(OP_DYN_METHOD_CALL); DT(OP_CAN);
        DT(OP_NEGATE); DT(OP_NOT); DT(OP_DEFINED); DT(OP_ABS); DT(OP_POWER); DT(OP_SPRINTF);
        DT(OP_ARRAY_POP); DT(OP_ARRAY_SHIFT); DT(OP_ARRAY_UNSHIFT);
        DT(OP_ARRAY_REVERSE); DT(OP_ARRAY_JOIN); DT(OP_ARRAY_SORT);
        DT(OP_HASH_EXISTS);
        DT(OP_FOREACH);
        DT(OP_LOOP_BREAK); DT(OP_LOOP_NEXT); DT(OP_LOOP_REDO);
        DT(OP_REF_TYPE);
        DT(OP_ADD_OV);
        DT(OP_CHAR_AT); DT(OP_BYTES);
        DT(OP_STR_LT); DT(OP_STR_GT); DT(OP_STR_LE); DT(OP_STR_GE); DT(OP_SPACESHIP); DT(OP_STR_CMP);
        DT(OP_CALL_NATIVE);
        DT(OP_C_BLOCK);
        DT(OP_BIT_AND); DT(OP_BIT_OR); DT(OP_BIT_XOR);
        DT(OP_BIT_SHL); DT(OP_BIT_SHR); DT(OP_BIT_NOT);
        DT(OP_APPEND_CONST);
        DT(OP_SLICE_IDXS);
        DT(OP_REGEX_FIND);
        DT(OP_SUPER_CALL);
        DT(OP_ARRAY_EXTEND);
        DT(OP_CALL_SPREAD);
        DT(OP_METHOD_CALL_SPREAD);
        DT(OP_CALL_CLOSURE_SPREAD);
        DT(OP_PUSH_FLOAT);
        DT(OP_LOAD_CAPTURE_CELL);
        DT(OP_RESET_LOCAL);
        DT(OP_CASE_EQ);
        DT(OP_SAVE_HASH_ELEM);
        DT(OP_RESTORE_HASH_ELEM);
        DT(OP_MAKE_SREF_LOCAL);
        DT(OP_MAKE_SREF_GLOBAL);
        DT(OP_MAKE_SREF_VAL);
        DT(OP_DEREF_SCALAR);
        DT(OP_STORE_SREF);
        #undef DT
        dt_init = 1;
    }
    #define DISPATCH() goto *dispatch_table[*ip++]
    #define CASE(op) L_##op
    DISPATCH();
#else
    #define DISPATCH() continue
    #define CASE(op) case op
    for (;;) { switch (*ip++) {
#endif

    CASE(OP_NOP): DISPATCH();

    CASE(OP_PUSH_INT): {
        int64_t val = read_i64(ip); ip += 8;
        SP_PUSH(VM_MAKE_INT(val));
        DISPATCH();
    }

    CASE(OP_PUSH_FLOAT): {
        double val = read_f64(ip); ip += 8;
        SP_PUSH(VM_MAKE_FLOAT(val));
        DISPATCH();
    }

    CASE(OP_PUSH_UNDEF): SP_PUSH(VM_UNDEF_VAL); DISPATCH();

    CASE(OP_POP): { VMValue v = SP_POP(); (void)v; DISPATCH(); }

    CASE(OP_DUP): {
        VMValue top = stack[sp - 1];
        if (VM_IS_PTR(top)) {
            enum VMObjType tt = VM_PTR_TYPE(top);
            if (tt == VM_OBJ_STR) { SP_PUSH(VM_MAKE_STR_N(VM_AS_STR(top)->data, VM_AS_STR(top)->len)); DISPATCH(); }
            if (tt == VM_OBJ_STRBUF) { SP_PUSH(VM_MAKE_STR_N(VM_AS_STRBUF(top)->data, VM_AS_STRBUF(top)->len)); DISPATCH(); }
            if (tt == VM_OBJ_FLOAT) { SP_PUSH(VM_MAKE_FLOAT(VM_AS_FLOAT(top)->val)); DISPATCH(); }
        }
        SP_PUSH(top);
        DISPATCH();
    }

    CASE(OP_LOAD_LOCAL): {
        uint16_t slot = read_u16(ip); ip += 2;
        if (__builtin_expect(slot > (uint16_t)chunk->local_count, 0)) {
            fprintf(stderr, "VM error: local slot %u out of bounds (%d)\n", (unsigned)slot, chunk->local_count);
            vm->stack_top = sp; vm->exec_base = saved_exec_base; VM_EXEC_RETURN(VM_UNDEF_VAL);
        }
        VMValue v = locals[slot];
        if (__builtin_expect(VM_IS_PTR(v), 0)) {
            enum VMObjType t = VM_PTR_TYPE(v);
            if (t == VM_OBJ_CELL) { VMValue cv = VM_AS_CELL(v)->val; SP_PUSH(vm_val_copy(cv)); DISPATCH(); }
            if (t == VM_OBJ_STR) { SP_PUSH(VM_MAKE_STR_N(VM_AS_STR(v)->data, VM_AS_STR(v)->len)); DISPATCH(); }
            if (t == VM_OBJ_STRBUF) { VMStrBuf *sb = VM_AS_STRBUF(v); SP_PUSH(VM_MAKE_STR_N(sb->data, sb->len)); DISPATCH(); }
            if (t == VM_OBJ_FLOAT) { SP_PUSH(VM_MAKE_FLOAT(VM_AS_FLOAT(v)->val)); DISPATCH(); }
        }
        SP_PUSH(v);
        DISPATCH();
    }

    CASE(OP_RESET_LOCAL): {
        uint16_t slot = read_u16(ip); ip += 2;
        if (slot <= (uint16_t)chunk->local_count) {
            if (VM_IS_PTR(locals[slot]) && VM_PTR_TYPE(locals[slot]) == VM_OBJ_CELL) {
                locals[slot] = VM_UNDEF_VAL;
            } else {
                vm_temp_free(&locals[slot]);
            }
        }
        DISPATCH();
    }

    CASE(OP_STORE_LOCAL): {
        uint16_t slot = read_u16(ip); ip += 2;
        if (__builtin_expect(slot > (uint16_t)chunk->local_count, 0)) {
            fprintf(stderr, "VM error: local slot %u out of bounds (%d)\n", (unsigned)slot, chunk->local_count);
            vm->stack_top = sp; vm->exec_base = saved_exec_base; VM_EXEC_RETURN(VM_UNDEF_VAL);
        }
        VMValue old = locals[slot];
        VMValue newv = SP_POP();
        /* If the slot holds a cell, store through it */
        if (__builtin_expect(VM_IS_PTR(old) && VM_PTR_TYPE(old) == VM_OBJ_CELL, 0)) {
            VMValue old_cell_val = VM_AS_CELL(old)->val;
            vm_temp_free(&old_cell_val);
            VM_AS_CELL(old)->val = newv;
            DISPATCH();
        }
        /* Only free old scalar temporaries; containers (arrays/hashes) are shared */
        vm_temp_free(&old);
        locals[slot] = newv;
        DISPATCH();
    }

    #define VM_TRY_BIN_OV(opstr) do { \
        if (__builtin_expect((VM_IS_PTR(a) && VM_PTR_TYPE(a) == VM_OBJ_HASH && VM_AS_HASH(a)->class_name != NULL) || \
                             (VM_IS_PTR(b) && VM_PTR_TYPE(b) == VM_OBJ_HASH && VM_AS_HASH(b)->class_name != NULL), 0)) { \
            VMValue _ov_res = VM_UNDEF_VAL; \
            frame->ip = ip; vm->stack_top = sp; \
            if (vm_try_bin_overload(vm, (opstr), a, b, &_ov_res)) { \
                stack = vm->stack; RELOAD(); \
                if (__builtin_expect(vm->has_pending_exc, 0)) { \
                    throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0; \
                    goto L_vm_throw_exc; \
                } \
                vm_temp_free(&a); vm_temp_free(&b); \
                SP_PUSH(_ov_res); \
                DISPATCH(); \
            } \
        } \
    } while (0)

    /* Integer arithmetic — with overload check for + */
    CASE(OP_ADD): {
        VMValue b = SP_POP(), a = SP_POP();
        VM_TRY_BIN_OV("+");
        if (!vm_is_float_operand(a) && !vm_is_float_operand(b)) {
            SP_PUSH(VM_MAKE_INT((int64_t)((uint64_t)vm_to_int(a) + (uint64_t)vm_to_int(b))));
        } else {
            SP_PUSH(VM_MAKE_FLOAT(vm_to_num(a) + vm_to_num(b)));
        }
        vm_temp_free(&a); vm_temp_free(&b);
        DISPATCH();
    }
    CASE(OP_SUB): {
        VMValue b = SP_POP(), a = SP_POP();
        VM_TRY_BIN_OV("-");
        if (!vm_is_float_operand(a) && !vm_is_float_operand(b)) {
            SP_PUSH(VM_MAKE_INT((int64_t)((uint64_t)vm_to_int(a) - (uint64_t)vm_to_int(b))));
        } else {
            SP_PUSH(VM_MAKE_FLOAT(vm_to_num(a) - vm_to_num(b)));
        }
        vm_temp_free(&a); vm_temp_free(&b);
        DISPATCH();
    }
    CASE(OP_MUL): {
        VMValue b = SP_POP(), a = SP_POP();
        VM_TRY_BIN_OV("*");
        if (!vm_is_float_operand(a) && !vm_is_float_operand(b)) {
            SP_PUSH(VM_MAKE_INT((int64_t)((uint64_t)vm_to_int(a) * (uint64_t)vm_to_int(b))));
        } else {
            SP_PUSH(VM_MAKE_FLOAT(vm_to_num(a) * vm_to_num(b)));
        }
        vm_temp_free(&a); vm_temp_free(&b);
        DISPATCH();
    }
    CASE(OP_DIV): {
        VMValue b = SP_POP(), a = SP_POP();
        VM_TRY_BIN_OV("/");
        double db = vm_to_num(b);
        if (db == 0.0) {
            SP_PUSH(VM_MAKE_INT(0));
        } else {
            double dr = vm_to_num(a) / db;
            if (!vm_is_float_operand(a) && !vm_is_float_operand(b) &&
                isfinite(dr) && dr >= -4.6e18 && dr <= 4.6e18 && dr == (double)(int64_t)dr) {
                SP_PUSH(VM_MAKE_INT((int64_t)dr));
            } else {
                SP_PUSH(VM_MAKE_FLOAT(dr));
            }
        }
        vm_temp_free(&a); vm_temp_free(&b);
        DISPATCH();
    }
    CASE(OP_MOD): {
        VMValue b = SP_POP(), a = SP_POP();
        VM_TRY_BIN_OV("%");
        int64_t av = vm_to_int(a), bv = vm_to_int(b);
        int64_t r = 0;
        if (bv != 0 && !(av == INT64_MIN && bv == -1)) {
            r = av % bv;
            if (r != 0 && ((r < 0) != (bv < 0))) r += bv;
        }
        SP_PUSH(VM_MAKE_INT(r));
        vm_temp_free(&a); vm_temp_free(&b);
        DISPATCH();
    }

    /* Bitwise operations */
    CASE(OP_BIT_AND): { VMValue b = SP_POP(), a = SP_POP(); SP_PUSH(VM_MAKE_INT(vm_to_int(a) & vm_to_int(b))); vm_temp_free(&a); vm_temp_free(&b); DISPATCH(); }
    CASE(OP_BIT_OR):  { VMValue b = SP_POP(), a = SP_POP(); SP_PUSH(VM_MAKE_INT(vm_to_int(a) | vm_to_int(b))); vm_temp_free(&a); vm_temp_free(&b); DISPATCH(); }
    CASE(OP_BIT_XOR): { VMValue b = SP_POP(), a = SP_POP(); SP_PUSH(VM_MAKE_INT(vm_to_int(a) ^ vm_to_int(b))); vm_temp_free(&a); vm_temp_free(&b); DISPATCH(); }
    CASE(OP_BIT_SHL): { VMValue b = SP_POP(), a = SP_POP(); int sh = (int)(vm_to_int(b) & 63); SP_PUSH(VM_MAKE_INT((int64_t)((uint64_t)vm_to_int(a) << sh))); vm_temp_free(&a); vm_temp_free(&b); DISPATCH(); }
    CASE(OP_BIT_SHR): { VMValue b = SP_POP(), a = SP_POP(); int sh = (int)(vm_to_int(b) & 63); SP_PUSH(VM_MAKE_INT(vm_to_int(a) >> sh)); vm_temp_free(&a); vm_temp_free(&b); DISPATCH(); }
    CASE(OP_BIT_NOT): { VMValue a = SP_POP(); SP_PUSH(VM_MAKE_INT(~vm_to_int(a))); vm_temp_free(&a); DISPATCH(); }

    /* Integer comparisons */
    CASE(OP_LT): { VMValue b = SP_POP(), a = SP_POP(); VM_TRY_BIN_OV("<"); SP_PUSH(VM_MAKE_INT(vm_to_num(a) < vm_to_num(b))); vm_temp_free(&a); vm_temp_free(&b); DISPATCH(); }
    CASE(OP_LE): { VMValue b = SP_POP(), a = SP_POP(); VM_TRY_BIN_OV("<="); SP_PUSH(VM_MAKE_INT(vm_to_num(a) <= vm_to_num(b))); vm_temp_free(&a); vm_temp_free(&b); DISPATCH(); }
    CASE(OP_GT): { VMValue b = SP_POP(), a = SP_POP(); VM_TRY_BIN_OV(">"); SP_PUSH(VM_MAKE_INT(vm_to_num(a) > vm_to_num(b))); vm_temp_free(&a); vm_temp_free(&b); DISPATCH(); }
    CASE(OP_GE): { VMValue b = SP_POP(), a = SP_POP(); VM_TRY_BIN_OV(">="); SP_PUSH(VM_MAKE_INT(vm_to_num(a) >= vm_to_num(b))); vm_temp_free(&a); vm_temp_free(&b); DISPATCH(); }
    CASE(OP_CASE_EQ): {
        VMValue b = SP_POP(), a = SP_POP();
        int eq = 0;
        int sa = (VM_IS_PTR(a) && (VM_PTR_TYPE(a) == VM_OBJ_STR || VM_PTR_TYPE(a) == VM_OBJ_STRBUF));
        int sb = (VM_IS_PTR(b) && (VM_PTR_TYPE(b) == VM_OBJ_STR || VM_PTR_TYPE(b) == VM_OBJ_STRBUF));
        if (sa || sb) {
            char ba[256], bb[256];
            eq = (strcmp(vm_to_cstr(a, ba, sizeof(ba)), vm_to_cstr(b, bb, sizeof(bb))) == 0);
        } else {
            eq = (vm_to_num(a) == vm_to_num(b));
        }
        SP_PUSH(VM_MAKE_INT(eq));
        vm_temp_free(&a); vm_temp_free(&b);
        DISPATCH();
    }
    CASE(OP_EQ): { VMValue b = SP_POP(), a = SP_POP(); VM_TRY_BIN_OV("=="); SP_PUSH(VM_MAKE_INT(vm_to_num(a) == vm_to_num(b))); vm_temp_free(&a); vm_temp_free(&b); DISPATCH(); }
    CASE(OP_NE): { VMValue b = SP_POP(), a = SP_POP(); VM_TRY_BIN_OV("!="); SP_PUSH(VM_MAKE_INT(vm_to_num(a) != vm_to_num(b))); vm_temp_free(&a); vm_temp_free(&b); DISPATCH(); }

    CASE(OP_STR_EQ): {
        VMValue b = SP_POP(), a = SP_POP();
        VM_TRY_BIN_OV("eq");
        char ba[64], bb[64];
        SP_PUSH(VM_MAKE_INT(strcmp(vm_to_cstr(a, ba, 64), vm_to_cstr(b, bb, 64)) == 0));
        vm_temp_free(&a); vm_temp_free(&b);
        DISPATCH();
    }
    CASE(OP_STR_NE): {
        VMValue b = SP_POP(), a = SP_POP();
        VM_TRY_BIN_OV("ne");
        char ba[64], bb[64];
        SP_PUSH(VM_MAKE_INT(strcmp(vm_to_cstr(a, ba, 64), vm_to_cstr(b, bb, 64)) != 0));
        vm_temp_free(&a); vm_temp_free(&b);
        DISPATCH();
    }

    CASE(OP_STR_LT): {
        VMValue b = SP_POP(), a = SP_POP();
        VM_TRY_BIN_OV("lt");
        char ba[64], bb[64];
        SP_PUSH(VM_MAKE_INT(strcmp(vm_to_cstr(a, ba, 64), vm_to_cstr(b, bb, 64)) < 0));
        vm_temp_free(&a); vm_temp_free(&b);
        DISPATCH();
    }
    CASE(OP_STR_GT): {
        VMValue b = SP_POP(), a = SP_POP();
        VM_TRY_BIN_OV("gt");
        char ba[64], bb[64];
        SP_PUSH(VM_MAKE_INT(strcmp(vm_to_cstr(a, ba, 64), vm_to_cstr(b, bb, 64)) > 0));
        vm_temp_free(&a); vm_temp_free(&b);
        DISPATCH();
    }
    CASE(OP_STR_LE): {
        VMValue b = SP_POP(), a = SP_POP();
        VM_TRY_BIN_OV("le");
        char ba[64], bb[64];
        SP_PUSH(VM_MAKE_INT(strcmp(vm_to_cstr(a, ba, 64), vm_to_cstr(b, bb, 64)) <= 0));
        vm_temp_free(&a); vm_temp_free(&b);
        DISPATCH();
    }
    CASE(OP_STR_GE): {
        VMValue b = SP_POP(), a = SP_POP();
        VM_TRY_BIN_OV("ge");
        char ba[64], bb[64];
        SP_PUSH(VM_MAKE_INT(strcmp(vm_to_cstr(a, ba, 64), vm_to_cstr(b, bb, 64)) >= 0));
        vm_temp_free(&a); vm_temp_free(&b);
        DISPATCH();
    }
    CASE(OP_SPACESHIP): {
        VMValue b = SP_POP(), a = SP_POP();
        VM_TRY_BIN_OV("<=>");
        double av = vm_to_num(a), bv = vm_to_num(b);   /* compare as numbers, not truncated ints */
        SP_PUSH(VM_MAKE_INT(av < bv ? -1 : (av > bv ? 1 : 0)));
        vm_temp_free(&a); vm_temp_free(&b);              /* also fixes a string-operand leak */
        DISPATCH();
    }
    CASE(OP_STR_CMP): {
        VMValue b = SP_POP(), a = SP_POP();
        VM_TRY_BIN_OV("cmp");
        char ba[64], bb[64];
        int c = strcmp(vm_to_cstr(a, ba, 64), vm_to_cstr(b, bb, 64));
        SP_PUSH(VM_MAKE_INT(c < 0 ? -1 : (c > 0 ? 1 : 0)));
        vm_temp_free(&a); vm_temp_free(&b);
        DISPATCH();
    }

    CASE(OP_JMP): {
        uint16_t off = read_u16(ip);
        if (__builtin_expect(off >= chunk->code_len, 0)) {
            fprintf(stderr, "VM error: jump offset %u out of bounds (%zu)\n", (unsigned)off, chunk->code_len);
            vm->stack_top = sp; vm->exec_base = saved_exec_base; VM_EXEC_RETURN(VM_UNDEF_VAL);
        }
        uint8_t *target = chunk->code + off;
        if (target <= ip && __builtin_expect(vm->gc_count >= vm->gc_threshold && vm->exec_base == 0 && vm->gc_pause == 0, 0)) {
            frame->ip = target;
            vm->stack_top = sp;
            vm_gc_collect(vm);
            stack = vm->stack;
        }
        ip = target;
        DISPATCH();
    }

    CASE(OP_JMP_IF_FALSE): {
        uint16_t off = read_u16(ip); ip += 2;
        VMValue c = SP_POP();
        if (VM_IS_INT(c) ? VM_INT_VAL(c) == 0 : !vm_to_bool(c)) {
            if (__builtin_expect(off >= chunk->code_len, 0)) {
                fprintf(stderr, "VM error: jump offset %u out of bounds (%zu)\n", (unsigned)off, chunk->code_len);
                vm_temp_free(&c); vm->stack_top = sp; vm->exec_base = saved_exec_base; VM_EXEC_RETURN(VM_UNDEF_VAL);
            }
            ip = chunk->code + off;
        }
        vm_temp_free(&c);
        DISPATCH();
    }

    CASE(OP_JMP_IF_TRUE): {
        uint16_t off = read_u16(ip); ip += 2;
        VMValue c = SP_POP();
        if (VM_IS_INT(c) ? VM_INT_VAL(c) != 0 : vm_to_bool(c)) {
            if (__builtin_expect(off >= chunk->code_len, 0)) {
                fprintf(stderr, "VM error: jump offset %u out of bounds (%zu)\n", (unsigned)off, chunk->code_len);
                vm_temp_free(&c); vm->stack_top = sp; vm->exec_base = saved_exec_base; VM_EXEC_RETURN(VM_UNDEF_VAL);
            }
            ip = chunk->code + off;
        }
        vm_temp_free(&c);
        DISPATCH();
    }

    CASE(OP_CALL): {
        call_fidx = read_u16(ip); ip += 2;
        call_argc = *ip++;
        goto L_do_op_call;
    }

    CASE(OP_CALL_SPREAD): {
        call_fidx = read_u16(ip); ip += 2;
        VMValue arr_val = SP_POP();
        call_argc = 0;
        if (VM_IS_PTR(arr_val) && VM_PTR_TYPE(arr_val) == VM_OBJ_ARRAY) {
            VMArray *aa = VM_AS_ARRAY(arr_val);
            call_argc = aa->size;
            for (int i = 0; i < call_argc; i++) SP_PUSH(aa->items[i]);
        }
        goto L_do_op_call;
    }

    L_do_op_call: {
        uint16_t fidx = call_fidx;
        int argc = call_argc;
        VMChunk *callee = &vm->program->funcs[fidx];
        frame->ip = ip; vm->stack_top = sp;

        if (vm->recursion_limit > 0 && (int)vm->frame_count >= vm->recursion_limit) {
            fprintf(stderr, "Deep recursion detected (limit %d) — see core::set_recursion_limit\n", vm->recursion_limit);
            exit(1);
        }
        if (vm->frame_count >= vm->frame_cap) {
            vm->frame_cap *= 2;
            vm->frames = realloc(vm->frames, vm->frame_cap * sizeof(VMFrame));
        }
        VMFrame *nf = &vm->frames[vm->frame_count++];
        nf->chunk = callee;
        nf->ip = callee->code;
        nf->locals = locals_alloc(callee->local_count + 1, callee->int_only);
        nf->stack_base = sp - argc;
        nf->closure = NULL;
        nf->global_save_depth = vm->global_save_count;

        if (__builtin_expect(callee->has_variadic, 0)) {
            int fixed = callee->fixed_param_count;
            int vc = argc - fixed;
            VMArray *va = vm_array_new(vc > 0 ? vc : 4);
            if (vc > 0) {
                VMValue *tmp = malloc(vc * sizeof(VMValue));
                for (int i = vc - 1; i >= 0; i--) tmp[i] = stack[--sp];
                for (int i = 0; i < vc; i++) vm_array_push(va, tmp[i]);
                free(tmp);
            }
            int pop_fixed = argc < fixed ? argc : fixed;
            for (int i = pop_fixed - 1; i >= 0; i--) nf->locals[i] = stack[--sp];
            nf->locals[fixed] = VM_MAKE_PTR(va);
        } else {
            int pmax = callee->local_count + 1;
            if (callee->fixed_param_count > 0 && callee->fixed_param_count < pmax) {
                pmax = callee->fixed_param_count;
            }
            for (int i = argc - 1; i >= 0; i--) {
                VMValue av = stack[--sp];
                if (i < pmax) nf->locals[i] = av;
            }
        }

        frame = nf; chunk = callee; ip = callee->code; locals = nf->locals;
        vm->stack_top = sp;
        DISPATCH();
    }

    CASE(OP_RETURN): {
        VMValue result = SP_POP();
        int lc = frame->chunk->local_count;
        vm_unwind_global_saves(vm, frame->global_save_depth);
        locals_free(locals, lc + 1);
        while (vm->exc_top > 0 && vm->exc_stack[vm->exc_top - 1].frame_count >= vm->frame_count) {
            vm->exc_top--;
        }
        vm->last_result = result;
        vm->frame_count--;
        if (vm->frame_count == vm->exec_base) {
            vm->stack_top = frame->stack_base;
            vm->exec_base = saved_exec_base;
            VM_EXEC_RETURN(result);
        }
        sp = frame->stack_base;
        stack[sp++] = result;
        vm->stack_top = sp;
        RELOAD();
        if (__builtin_expect(vm->gc_count >= vm->gc_threshold && vm->exec_base == 0 && vm->gc_pause == 0, 0)) {
            frame->ip = ip;
            vm->stack_top = sp;
            vm_gc_collect(vm);
            stack = vm->stack;
        }
        DISPATCH();
    }

    CASE(OP_SAY): {
        VMValue v = SP_POP();
        if (__builtin_expect(vm_has_stringify(vm->program, v), 0)) {
            frame->ip = ip; vm->stack_top = sp;
            v = vm_stringify_ov(vm, v);
            stack = vm->stack; RELOAD();
            if (__builtin_expect(vm->has_pending_exc, 0)) {
                throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                goto L_vm_throw_exc;
            }
        }
        char buf[64];
        if (VM_IS_PTR(vm->default_fh) && VM_PTR_TYPE(vm->default_fh) == VM_OBJ_FILEHANDLE) {
            VMFileHandle *fh = (VMFileHandle*)VM_TO_PTR(vm->default_fh);
            if (fh->fp) fprintf((FILE*)fh->fp, "%s\n", vm_to_cstr(v, buf, 64));
        } else {
            printf("%s\n", vm_to_cstr(v, buf, 64));
        }
        vm_temp_free(&v); DISPATCH();
    }

    CASE(OP_PRINT): {
        VMValue v = SP_POP();
        if (__builtin_expect(vm_has_stringify(vm->program, v), 0)) {
            frame->ip = ip; vm->stack_top = sp;
            v = vm_stringify_ov(vm, v);
            stack = vm->stack; RELOAD();
            if (__builtin_expect(vm->has_pending_exc, 0)) {
                throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                goto L_vm_throw_exc;
            }
        }
        char buf[64];
        if (VM_IS_PTR(vm->default_fh) && VM_PTR_TYPE(vm->default_fh) == VM_OBJ_FILEHANDLE) {
            VMFileHandle *fh = (VMFileHandle*)VM_TO_PTR(vm->default_fh);
            if (fh->fp) fprintf((FILE*)fh->fp, "%s", vm_to_cstr(v, buf, 64));
        } else {
            printf("%s", vm_to_cstr(v, buf, 64));
        }
        vm_temp_free(&v); DISPATCH();
    }

    CASE(OP_SAY_FH): {
        VMValue str = SP_POP(), fhv = SP_POP();
        if (__builtin_expect(vm_has_stringify(vm->program, str), 0)) {
            frame->ip = ip; vm->stack_top = sp;
            SP_PUSH(fhv); vm->stack_top = sp;
            str = vm_stringify_ov(vm, str);
            stack = vm->stack; RELOAD();
            if (__builtin_expect(vm->has_pending_exc, 0)) {
                throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                goto L_vm_throw_exc;
            }
            fhv = SP_POP();
        }
        char buf[64];
        if (VM_IS_PTR(fhv) && VM_PTR_TYPE(fhv) == VM_OBJ_FILEHANDLE) {
            VMFileHandle *fh = (VMFileHandle*)VM_TO_PTR(fhv);
            if (fh->fp) fprintf((FILE*)fh->fp, "%s\n", vm_to_cstr(str, buf, 64));
        }
        vm_val_free(&str);
        DISPATCH();
    }

    CASE(OP_PRINT_FH): {
        VMValue str = SP_POP(), fhv = SP_POP();
        if (__builtin_expect(vm_has_stringify(vm->program, str), 0)) {
            frame->ip = ip; vm->stack_top = sp;
            SP_PUSH(fhv); vm->stack_top = sp;
            str = vm_stringify_ov(vm, str);
            stack = vm->stack; RELOAD();
            if (__builtin_expect(vm->has_pending_exc, 0)) {
                throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                goto L_vm_throw_exc;
            }
            fhv = SP_POP();
        }
        char buf[64];
        if (VM_IS_PTR(fhv) && VM_PTR_TYPE(fhv) == VM_OBJ_FILEHANDLE) {
            VMFileHandle *fh = (VMFileHandle*)VM_TO_PTR(fhv);
            if (fh->fp) fprintf((FILE*)fh->fp, "%s", vm_to_cstr(str, buf, 64));
        }
        vm_val_free(&str);
        DISPATCH();
    }

    CASE(OP_CONCAT): {
        VMValue b = SP_POP(), a = SP_POP();
        VM_TRY_BIN_OV(".");
        if (__builtin_expect(vm_has_stringify(vm->program, a), 0)) {
            frame->ip = ip; vm->stack_top = sp;
            SP_PUSH(b); vm->stack_top = sp;
            a = vm_stringify_ov(vm, a);
            stack = vm->stack; RELOAD();
            if (__builtin_expect(vm->has_pending_exc, 0)) {
                throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                goto L_vm_throw_exc;
            }
            b = SP_POP();
        }
        if (__builtin_expect(vm_has_stringify(vm->program, b), 0)) {
            frame->ip = ip; vm->stack_top = sp;
            SP_PUSH(a); vm->stack_top = sp;
            b = vm_stringify_ov(vm, b);
            stack = vm->stack; RELOAD();
            if (__builtin_expect(vm->has_pending_exc, 0)) {
                throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                goto L_vm_throw_exc;
            }
            a = SP_POP();
        }
        char ba[256], bb[256];
        const char *sa = vm_to_cstr(a, ba, 256), *sb = vm_to_cstr(b, bb, 256);
        size_t la = (VM_IS_PTR(a) && VM_PTR_TYPE(a) == VM_OBJ_STR) ? VM_AS_STR(a)->len :
                    (VM_IS_PTR(a) && VM_PTR_TYPE(a) == VM_OBJ_STRBUF) ? VM_AS_STRBUF(a)->len : strlen(sa);
        size_t lb = (VM_IS_PTR(b) && VM_PTR_TYPE(b) == VM_OBJ_STR) ? VM_AS_STR(b)->len :
                    (VM_IS_PTR(b) && VM_PTR_TYPE(b) == VM_OBJ_STRBUF) ? VM_AS_STRBUF(b)->len : strlen(sb);
        VMString *r = malloc(sizeof(VMString) + la + lb + 1);
        vm_gc_init_hdr(&r->hdr, VM_OBJ_STR);
        r->len = la + lb;
        memcpy(r->data, sa, la); memcpy(r->data + la, sb, lb); r->data[la+lb] = '\0';
        vm_val_free(&a); vm_val_free(&b);
        SP_PUSH(VM_MAKE_PTR(r));
        DISPATCH();
    }

    CASE(OP_PUSH_STR): {
        uint16_t idx = read_u16(ip); ip += 2;
        if (__builtin_expect(idx >= chunk->str_const_count, 0)) {
            fprintf(stderr, "VM error: str const %u out of bounds (%zu)\n", (unsigned)idx, chunk->str_const_count);
            vm->stack_top = sp; vm->exec_base = saved_exec_base; VM_EXEC_RETURN(VM_UNDEF_VAL);
        }
        SP_PUSH(vm_str(chunk->str_consts[idx]));
        DISPATCH();
    }

    CASE(OP_INCR): {
        uint16_t s = read_u16(ip); ip += 2;
        VMValue *islot = (VM_IS_PTR(locals[s]) && VM_PTR_TYPE(locals[s]) == VM_OBJ_CELL)
                         ? &VM_AS_CELL(locals[s])->val : &locals[s];
        if (VM_IS_INT(*islot)) {
            *islot = VM_MAKE_INT((int64_t)((uint64_t)VM_INT_VAL(*islot) + 1ULL));
        } else {
            /* Coerce undef/float/string instead of corrupting the pointer with += 2. */
            double n = vm_to_num(*islot) + 1.0;
            int was_float = (VM_IS_PTR(*islot) && VM_PTR_TYPE(*islot) == VM_OBJ_FLOAT);
            VMValue old = *islot;
            if (!was_float && isfinite(n) && n == (double)(int64_t)n && n >= -4.6e18 && n <= 4.6e18)
                *islot = VM_MAKE_INT((int64_t)n);
            else
                *islot = VM_MAKE_FLOAT(n);
            vm_temp_free(&old);
        }
        DISPATCH();
    }
    CASE(OP_DECR): {
        uint16_t s = read_u16(ip); ip += 2;
        VMValue *dslot = (VM_IS_PTR(locals[s]) && VM_PTR_TYPE(locals[s]) == VM_OBJ_CELL)
                         ? &VM_AS_CELL(locals[s])->val : &locals[s];
        if (VM_IS_INT(*dslot)) {
            *dslot = VM_MAKE_INT((int64_t)((uint64_t)VM_INT_VAL(*dslot) - 1ULL));
        } else {
            double n = vm_to_num(*dslot) - 1.0;
            int was_float = (VM_IS_PTR(*dslot) && VM_PTR_TYPE(*dslot) == VM_OBJ_FLOAT);
            VMValue old = *dslot;
            if (!was_float && isfinite(n) && n == (double)(int64_t)n && n >= -4.6e18 && n <= 4.6e18)
                *dslot = VM_MAKE_INT((int64_t)n);
            else
                *dslot = VM_MAKE_FLOAT(n);
            vm_temp_free(&old);
        }
        DISPATCH();
    }

    /* ===== Arrays ===== */
    CASE(OP_NEW_ARRAY): { uint32_t c = read_u32(ip); ip += 4; SP_PUSH(VM_MAKE_PTR(vm_array_new(c>0?c:8))); DISPATCH(); }
    CASE(OP_ARRAY_PUSH): { VMValue v = SP_POP(), a = SP_POP(); if (VM_IS_PTR(a) && VM_PTR_TYPE(a) == VM_OBJ_ARRAY) vm_array_push(VM_AS_ARRAY(a), v); DISPATCH(); }
    CASE(OP_ARRAY_EXTEND): {
        VMValue src = SP_POP(), dst = SP_POP();
        if (VM_IS_PTR(dst) && VM_PTR_TYPE(dst) == VM_OBJ_ARRAY) {
            VMArray *da = VM_AS_ARRAY(dst);
            if (VM_IS_PTR(src) && VM_PTR_TYPE(src) == VM_OBJ_ARRAY) {
                VMArray *sa = VM_AS_ARRAY(src);
                for (int i = 0; i < sa->size; i++) {
                    vm_array_push(da, vm_val_copy(sa->items[i]));
                }
            } else if (!VM_IS_UNDEF(src)) {
                vm_array_push(da, src);
            }
        }
        DISPATCH();
    }
    CASE(OP_SLICE_IDXS): {
        /* Range/array-of-indices slice: append src[i] for each index i */
        VMValue idxs = SP_POP(), src = SP_POP(), res = SP_POP();
        if (VM_IS_PTR(idxs) && VM_PTR_TYPE(idxs) == VM_OBJ_ARRAY &&
            VM_IS_PTR(src) && VM_PTR_TYPE(src) == VM_OBJ_ARRAY &&
            VM_IS_PTR(res) && VM_PTR_TYPE(res) == VM_OBJ_ARRAY) {
            VMArray *ia = VM_AS_ARRAY(idxs), *sa = VM_AS_ARRAY(src), *ra = VM_AS_ARRAY(res);
            for (int i = 0; i < ia->size; i++) {
                VMValue e = vm_array_get(sa, (int)vm_to_int(ia->items[i]));
                vm_array_push(ra, vm_val_copy(e));
            }
        }
        DISPATCH();
    }
    CASE(OP_ARRAY_GET): {
        VMValue idx = SP_POP(), arr = SP_POP();
        if (VM_IS_PTR(arr) && VM_PTR_TYPE(arr) == VM_OBJ_ARRAY) {
            VMValue e = vm_array_get(VM_AS_ARRAY(arr), (int)vm_to_int(idx));
            SP_PUSH(vm_val_copy(e));
        } else SP_PUSH(VM_UNDEF_VAL);
        vm_temp_free(&idx);
        DISPATCH();
    }
    CASE(OP_ARRAY_SET): {
        VMValue v = SP_POP(), idx = SP_POP(), arr = SP_POP();
        if (VM_IS_PTR(arr) && VM_PTR_TYPE(arr) == VM_OBJ_ARRAY) vm_array_set(VM_AS_ARRAY(arr), (int)vm_to_int(idx), v);
        DISPATCH();
    }
    CASE(OP_ARRAY_SIZE): {
        VMValue a = SP_POP();
        if (VM_IS_PTR(a) && VM_PTR_TYPE(a) == VM_OBJ_ARRAY)
            SP_PUSH(VM_MAKE_INT(VM_AS_ARRAY(a)->size));
        else
            SP_PUSH(VM_MAKE_INT(0));
        DISPATCH();
    }
    CASE(OP_ARRAY_POP): {
        VMValue a = SP_POP();
        if (VM_IS_PTR(a) && VM_PTR_TYPE(a) == VM_OBJ_ARRAY)
            SP_PUSH(vm_array_pop(VM_AS_ARRAY(a)));
        else SP_PUSH(VM_UNDEF_VAL);
        DISPATCH();
    }
    CASE(OP_ARRAY_SHIFT): {
        VMValue a = SP_POP();
        if (VM_IS_PTR(a) && VM_PTR_TYPE(a) == VM_OBJ_ARRAY)
            SP_PUSH(vm_array_shift(VM_AS_ARRAY(a)));
        else SP_PUSH(VM_UNDEF_VAL);
        DISPATCH();
    }
    CASE(OP_ARRAY_UNSHIFT): {
        VMValue v = SP_POP(), a = SP_POP();
        if (VM_IS_PTR(a) && VM_PTR_TYPE(a) == VM_OBJ_ARRAY) vm_array_unshift(VM_AS_ARRAY(a), v);
        DISPATCH();
    }
    CASE(OP_ARRAY_REVERSE): {
        VMValue a = SP_POP();
        if (VM_IS_PTR(a) && VM_PTR_TYPE(a) == VM_OBJ_ARRAY) {
            VMArray *arr = VM_AS_ARRAY(a);
            VMArray *rev = vm_array_new(arr->size);
            for (int i = arr->size - 1; i >= 0; i--) vm_array_push(rev, vm_val_copy(arr->items[i]));
            SP_PUSH(VM_MAKE_PTR(rev));
        } else if (VM_IS_PTR(a) && (VM_PTR_TYPE(a) == VM_OBJ_STR || VM_PTR_TYPE(a) == VM_OBJ_STRBUF)) {
            /* reverse() on a string: reverse the characters */
            char buf[64];
            const char *s = vm_to_cstr(a, buf, sizeof(buf));
            size_t len = strlen(s);
            char *rev = malloc(len + 1);
            for (size_t i = 0; i < len; i++) rev[i] = s[len - 1 - i];
            rev[len] = '\0';
            SP_PUSH(vm_str(rev));
            free(rev);
        } else SP_PUSH(VM_MAKE_PTR(vm_array_new(0)));
        DISPATCH();
    }
    CASE(OP_ARRAY_JOIN): {
        VMValue arr = SP_POP(), sep = SP_POP();
        char sepbuf[64];
        const char *sep_str = vm_to_cstr(sep, sepbuf, 64);
        size_t sep_len = strlen(sep_str);
        if (VM_IS_PTR(arr) && VM_PTR_TYPE(arr) == VM_OBJ_ARRAY) {
            VMArray *a = VM_AS_ARRAY(arr);
            /* Calculate total length */
            size_t total = 0;
            int n = a->size;   /* no 1024 cap — join ALL elements */
            char **bufs = malloc((size_t)(n > 0 ? n : 1) * sizeof(char*));
            size_t *lens = malloc((size_t)(n > 0 ? n : 1) * sizeof(size_t));
            char tmp[64];
            for (int i = 0; i < n; i++) {
                const char *s = vm_to_cstr(a->items[i], tmp, 64);
                lens[i] = strlen(s);
                bufs[i] = malloc(lens[i] + 1);
                memcpy(bufs[i], s, lens[i] + 1);
                total += lens[i];
                if (i > 0) total += sep_len;
            }
            VMString *vs = malloc(sizeof(VMString) + total + 1);
            vm_gc_init_hdr(&vs->hdr, VM_OBJ_STR); vs->len = total;
            char *p = vs->data;
            for (int i = 0; i < n; i++) {
                if (i > 0) { memcpy(p, sep_str, sep_len); p += sep_len; }
                memcpy(p, bufs[i], lens[i]); p += lens[i];
                free(bufs[i]);
            }
            *p = 0;
            free(bufs); free(lens);
            SP_PUSH(VM_MAKE_PTR(vs));
        } else SP_PUSH(vm_str(""));
        vm_val_free(&sep);
        DISPATCH();
    }
    CASE(OP_ARRAY_SORT): {
        uint16_t func_idx = read_u16(ip); ip += 2;
        VMValue arr = SP_POP();
        if (VM_IS_PTR(arr) && VM_PTR_TYPE(arr) == VM_OBJ_ARRAY) {
            VMArray *a = VM_AS_ARRAY(arr);
            VMArray *sorted = vm_array_new(a->size);
            for (int i = 0; i < a->size; i++) vm_array_push(sorted, vm_val_copy(a->items[i]));

            /* Save current VM state around nested vm_execute_call comparator */
            frame->ip = ip;
            vm->stack_top = sp;

            for (int i = 1; i < sorted->size; i++) {
                VMValue key = sorted->items[i];
                int j = i - 1;
                while (j >= 0) {
                    VMValue cargs[2];
                    cargs[0] = vm_val_copy(sorted->items[j]);
                    cargs[1] = vm_val_copy(key);
                    VMValue cres = vm_execute_call(vm, func_idx, cargs, 2);
                    if (__builtin_expect(vm->has_pending_exc, 0)) {
                        throw_exc = vm->pending_exc;
                        vm->pending_exc = VM_UNDEF_VAL;
                        vm->has_pending_exc = 0;
                        stack = vm->stack;
                        RELOAD();
                        goto L_vm_throw_exc;
                    }
                    int cmp_result = (int)vm_to_int(cres);
                    vm_temp_free(&cres);

                    if (cmp_result > 0) {
                        sorted->items[j + 1] = sorted->items[j];
                        j--;
                    } else {
                        break;
                    }
                }
                sorted->items[j + 1] = key;
            }

            stack = vm->stack;
            RELOAD();
            SP_PUSH(VM_MAKE_PTR(sorted));
        } else SP_PUSH(VM_MAKE_PTR(vm_array_new(0)));
        DISPATCH();
    }

    /* ===== Hashes ===== */
    CASE(OP_NEW_HASH): { uint32_t c = read_u32(ip); ip += 4; SP_PUSH(VM_MAKE_PTR(vm_hash_new(c>0?c:16))); DISPATCH(); }
    CASE(OP_HASH_GET): {
        VMValue key = SP_POP(), hash = SP_POP();
        if (VM_IS_PTR(hash) && VM_PTR_TYPE(hash) == VM_OBJ_HASH) {
            VMHash *h = VM_AS_HASH(hash);
            if (__builtin_expect(!VM_IS_UNDEF(h->tied_obj), 0)) {
                /* tied hash: FETCH(obj, key) via nested call */
                int ffidx = -1;
                if (VM_IS_PTR(h->tied_obj) && VM_PTR_TYPE(h->tied_obj) == VM_OBJ_HASH &&
                    VM_AS_HASH(h->tied_obj)->class_name)
                    ffidx = vm_find_method(vm->program, VM_AS_HASH(h->tied_obj)->class_name, "FETCH");
                if (ffidx >= 0) {
                    frame->ip = ip; vm->stack_top = sp;
                    VMValue cargs[2] = { h->tied_obj, key };
                    VMValue r = vm_execute_call(vm, ffidx, cargs, 2);
                    stack = vm->stack;
                    RELOAD();
                    if (__builtin_expect(vm->has_pending_exc, 0)) {
                        vm_temp_free(&key);
                        throw_exc = vm->pending_exc;
                        vm->pending_exc = VM_UNDEF_VAL;
                        vm->has_pending_exc = 0;
                        goto L_vm_throw_exc;
                    }
                    SP_PUSH(r);
                    vm_temp_free(&key); DISPATCH();
                }
            }
            char buf[64]; const char *ks = vm_to_cstr(key, buf, 64);
            VMValue r = vm_hash_get(h, ks);
            SP_PUSH(vm_val_copy(r));
        } else SP_PUSH(VM_UNDEF_VAL);
        vm_temp_free(&key); DISPATCH();
    }
    CASE(OP_HASH_SET): {
        VMValue v = SP_POP(), key = SP_POP(), hash = SP_POP();
        if (VM_IS_PTR(hash) && VM_PTR_TYPE(hash) == VM_OBJ_HASH) {
            VMHash *h = VM_AS_HASH(hash);
            if (__builtin_expect(!VM_IS_UNDEF(h->tied_obj), 0)) {
                /* tied hash: STORE(obj, key, value) via nested call */
                int sfidx = -1;
                if (VM_IS_PTR(h->tied_obj) && VM_PTR_TYPE(h->tied_obj) == VM_OBJ_HASH &&
                    VM_AS_HASH(h->tied_obj)->class_name)
                    sfidx = vm_find_method(vm->program, VM_AS_HASH(h->tied_obj)->class_name, "STORE");
                if (sfidx >= 0) {
                    frame->ip = ip; vm->stack_top = sp;
                    VMValue cargs[3] = { h->tied_obj, key, v };
                    vm_execute_call(vm, sfidx, cargs, 3);
                    stack = vm->stack;
                    RELOAD();
                    if (__builtin_expect(vm->has_pending_exc, 0)) {
                        vm_temp_free(&key);
                        throw_exc = vm->pending_exc;
                        vm->pending_exc = VM_UNDEF_VAL;
                        vm->has_pending_exc = 0;
                        goto L_vm_throw_exc;
                    }
                    vm_temp_free(&key); DISPATCH();
                }
            }
            char buf[64]; vm_hash_set(h, vm_to_cstr(key, buf, 64), v);
        }
        vm_val_free(&key); DISPATCH();
    }
    CASE(OP_HASH_DELETE): {
        VMValue key = SP_POP(), hash = SP_POP();
        if (VM_IS_PTR(hash) && VM_PTR_TYPE(hash) == VM_OBJ_HASH) {
            VMHash *h = VM_AS_HASH(hash);
            if (__builtin_expect(!VM_IS_UNDEF(h->tied_obj), 0)) {
                int dfidx = -1;
                if (VM_IS_PTR(h->tied_obj) && VM_PTR_TYPE(h->tied_obj) == VM_OBJ_HASH &&
                    VM_AS_HASH(h->tied_obj)->class_name)
                    dfidx = vm_find_method(vm->program, VM_AS_HASH(h->tied_obj)->class_name, "DELETE");
                if (dfidx >= 0) {
                    frame->ip = ip; vm->stack_top = sp;
                    VMValue cargs[2] = { h->tied_obj, key };
                    VMValue r = vm_execute_call(vm, dfidx, cargs, 2);
                    stack = vm->stack;
                    RELOAD();
                    if (__builtin_expect(vm->has_pending_exc, 0)) {
                        vm_temp_free(&key);
                        throw_exc = vm->pending_exc;
                        vm->pending_exc = VM_UNDEF_VAL;
                        vm->has_pending_exc = 0;
                        goto L_vm_throw_exc;
                    }
                    vm_val_free(&r);
                    vm_temp_free(&key);
                    DISPATCH();
                }
            }
            char buf[64];
            vm_hash_delete(h, vm_to_cstr(key, buf, 64));
        }
        vm_val_free(&key); DISPATCH();
    }
    CASE(OP_HASH_KEYS): {
        VMValue hash = SP_POP();
        SP_PUSH(VM_MAKE_PTR(VM_IS_PTR(hash) && VM_PTR_TYPE(hash) == VM_OBJ_HASH ? vm_hash_keys(VM_AS_HASH(hash)) : vm_array_new(0)));
        DISPATCH();
    }
    CASE(OP_HASH_EXISTS): {
        VMValue key = SP_POP(), hash = SP_POP();
        int res = 0;
        if (VM_IS_PTR(hash) && VM_PTR_TYPE(hash) == VM_OBJ_HASH) {
            VMHash *h = VM_AS_HASH(hash);
            if (__builtin_expect(!VM_IS_UNDEF(h->tied_obj), 0)) {
                int efidx = -1;
                if (VM_IS_PTR(h->tied_obj) && VM_PTR_TYPE(h->tied_obj) == VM_OBJ_HASH &&
                    VM_AS_HASH(h->tied_obj)->class_name)
                    efidx = vm_find_method(vm->program, VM_AS_HASH(h->tied_obj)->class_name, "EXISTS");
                if (efidx >= 0) {
                    frame->ip = ip; vm->stack_top = sp;
                    VMValue cargs[2] = { h->tied_obj, key };
                    VMValue r = vm_execute_call(vm, efidx, cargs, 2);
                    stack = vm->stack;
                    RELOAD();
                    if (__builtin_expect(vm->has_pending_exc, 0)) {
                        vm_temp_free(&key);
                        throw_exc = vm->pending_exc;
                        vm->pending_exc = VM_UNDEF_VAL;
                        vm->has_pending_exc = 0;
                        goto L_vm_throw_exc;
                    }
                    res = vm_to_bool(r) ? 1 : 0;
                    vm_val_free(&r);
                    SP_PUSH(VM_MAKE_INT(res));
                    vm_temp_free(&key);
                    DISPATCH();
                }
            }
            char buf[64]; res = vm_hash_exists(h, vm_to_cstr(key, buf, 64));
        }
        SP_PUSH(VM_MAKE_INT(res));
        vm_val_free(&key); DISPATCH();
    }

    /* ===== Strings ===== */
    CASE(OP_STR_LEN): {
        VMValue v = SP_POP();
        if (VM_IS_PTR(v)) {
            if (VM_PTR_TYPE(v) == VM_OBJ_STRBUF) SP_PUSH(VM_MAKE_INT(VM_AS_STRBUF(v)->len));
            else if (VM_PTR_TYPE(v) == VM_OBJ_STR) SP_PUSH(VM_MAKE_INT(strlen(VM_AS_STR(v)->data)));
            else SP_PUSH(VM_MAKE_INT(0));
        } else SP_PUSH(VM_MAKE_INT(0));
        vm_val_free(&v); DISPATCH();
    }

    CASE(OP_STR_SPLIT): {
        VMValue str = SP_POP(), delim = SP_POP();
        char sbuf[256], pbuf[256];
        const char *s = vm_to_cstr(str, sbuf, sizeof(sbuf));
        const char *pat = vm_to_cstr(delim, pbuf, sizeof(pbuf));
        StradaArray *parts = strada_regex_split(s, pat);
        VMArray *res = vm_array_new(parts ? (int)parts->size : 0);
        if (parts) {
            for (size_t i = 0; i < parts->size; i++) {
                vm_array_push(res, sv_to_vm(strada_array_get(parts, (int64_t)i)));
            }
            while (parts->size > 0) {
                StradaValue *el = strada_array_pop(parts);
                strada_decref(el);
            }
            free(parts->elements);
            free(parts);
        }
        vm_val_free(&str); vm_val_free(&delim);
        SP_PUSH(VM_MAKE_PTR(res));
        DISPATCH();
    }

    CASE(OP_STR_SPLIT_LIMIT): {
        VMValue str = SP_POP(), delim = SP_POP(), limit_v = SP_POP();
        int limit = (int)vm_to_int(limit_v);
        char sbuf[256], pbuf[256];
        const char *s = vm_to_cstr(str, sbuf, sizeof(sbuf));
        const char *pat = vm_to_cstr(delim, pbuf, sizeof(pbuf));
        StradaArray *parts = (limit <= 0) ? strada_regex_split(s, pat)
                                          : strada_regex_split_limit(s, pat, limit);
        VMArray *res = vm_array_new(parts ? (int)parts->size : 0);
        if (parts) {
            for (size_t i = 0; i < parts->size; i++) {
                vm_array_push(res, sv_to_vm(strada_array_get(parts, (int64_t)i)));
            }
            while (parts->size > 0) {
                StradaValue *el = strada_array_pop(parts);
                strada_decref(el);
            }
            free(parts->elements);
            free(parts);
        }
        vm_val_free(&str); vm_val_free(&delim);
        SP_PUSH(VM_MAKE_PTR(res));
        DISPATCH();
    }

    CASE(OP_STR_REPLACE): {
        VMValue repl = SP_POP(), pat = SP_POP(), str = SP_POP();
        char sbuf[256], pbuf[256], rbuf[256];
        const char *s = vm_to_cstr(str, sbuf, sizeof(sbuf));
        const char *p = vm_to_cstr(pat, pbuf, sizeof(pbuf));
        const char *r = vm_to_cstr(repl, rbuf, sizeof(rbuf));
        size_t sl = (VM_IS_PTR(str) && VM_PTR_TYPE(str) == VM_OBJ_STR) ? VM_AS_STR(str)->len :
                    (VM_IS_PTR(str) && VM_PTR_TYPE(str) == VM_OBJ_STRBUF) ? VM_AS_STRBUF(str)->len : strlen(s);
        size_t pl = strlen(p), rl = strlen(r);
        const char *f = strstr(s, p);
        if (f) {
            size_t before = (size_t)(f - s), nl = sl - pl + rl;
            VMString *vs = malloc(sizeof(VMString) + nl + 1);
            vm_gc_init_hdr(&vs->hdr, VM_OBJ_STR); vs->len = (uint32_t)nl;
            memcpy(vs->data, s, before); memcpy(vs->data + before, r, rl);
            memcpy(vs->data + before + rl, f + pl, sl - before - pl); vs->data[nl] = 0;
            SP_PUSH(VM_MAKE_PTR(vs));
        } else {
            SP_PUSH(VM_MAKE_STR_N(s, sl));
        }
        vm_val_free(&str); vm_val_free(&pat); vm_val_free(&repl);
        DISPATCH();
    }

    CASE(OP_SUBSTR): {
        VMValue len_v = SP_POP(), start_v = SP_POP(), str_v = SP_POP();
        char buf[64];
        const char *s = vm_to_cstr(str_v, buf, 64);
        int64_t start = vm_to_int(start_v);
        int64_t len = vm_to_int(len_v);
        size_t slen = strlen(s);
        if (start < 0) start = (int64_t)slen + start;
        if (start < 0) start = 0;
        if ((size_t)start > slen) start = slen;
        if (len < 0) {
            /* Negative length trims |len| chars from the end (Perl / strada_substr
             * semantics); the old code clamped it to 0, returning empty. */
            len = (int64_t)slen - start + len;
            if (len < 0) len = 0;
        } else if (start + len > (int64_t)slen) {
            len = (int64_t)slen - start;
        }
        SP_PUSH(VM_MAKE_STR_N(s + start, len));
        vm_val_free(&str_v);
        DISPATCH();
    }

    CASE(OP_STR_INDEX): {
        VMValue needle = SP_POP(), haystack = SP_POP();
        char hbuf[256], nbuf[64];
        const char *h = vm_to_cstr(haystack, hbuf, 256);
        const char *n = vm_to_cstr(needle, nbuf, 64);
        const char *f = strstr(h, n);
        SP_PUSH(VM_MAKE_INT(f ? f - h : -1));
        vm_val_free(&haystack); vm_val_free(&needle);
        DISPATCH();
    }

    CASE(OP_STR_UPPER): {
        VMValue v = SP_POP();
        char buf[64];
        const char *s = vm_to_cstr(v, buf, 64);
        size_t len = strlen(s);
        VMString *vs = malloc(sizeof(VMString) + len + 1);
        vm_gc_init_hdr(&vs->hdr, VM_OBJ_STR); vs->len = len;
        for (size_t i = 0; i < len; i++) vs->data[i] = toupper((unsigned char)s[i]);
        vs->data[len] = 0;
        vm_temp_free(&v);
        SP_PUSH(VM_MAKE_PTR(vs));
        DISPATCH();
    }

    CASE(OP_STR_LOWER): {
        VMValue v = SP_POP();
        char buf[64];
        const char *s = vm_to_cstr(v, buf, 64);
        size_t len = strlen(s);
        VMString *vs = malloc(sizeof(VMString) + len + 1);
        vm_gc_init_hdr(&vs->hdr, VM_OBJ_STR); vs->len = len;
        for (size_t i = 0; i < len; i++) vs->data[i] = tolower((unsigned char)s[i]);
        vs->data[len] = 0;
        vm_temp_free(&v);
        SP_PUSH(VM_MAKE_PTR(vs));
        DISPATCH();
    }

    CASE(OP_CHR): {
        VMValue v = SP_POP();
        int64_t code = vm_to_int(v);
        char c[5];
        size_t clen;
        if (code >= 0 && code < 0x80) {
            c[0] = (char)(unsigned char)code;
            clen = 1;
        } else if (code >= 0x80 && code < 0x800) {
            c[0] = (char)(0xC0 | (code >> 6));
            c[1] = (char)(0x80 | (code & 0x3F));
            clen = 2;
        } else if (code >= 0x800 && code < 0x10000) {
            c[0] = (char)(0xE0 | (code >> 12));
            c[1] = (char)(0x80 | ((code >> 6) & 0x3F));
            c[2] = (char)(0x80 | (code & 0x3F));
            clen = 3;
        } else if (code >= 0x10000 && code < 0x110000) {
            c[0] = (char)(0xF0 | (code >> 18));
            c[1] = (char)(0x80 | ((code >> 12) & 0x3F));
            c[2] = (char)(0x80 | ((code >> 6) & 0x3F));
            c[3] = (char)(0x80 | (code & 0x3F));
            clen = 4;
        } else {
            c[0] = '?';
            clen = 1;
        }
        vm_temp_free(&v);
        SP_PUSH(VM_MAKE_STR_N(c, clen));
        DISPATCH();
    }

    CASE(OP_ORD): {
        VMValue v = SP_POP();
        if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_STR && VM_AS_STR(v)->len > 0 && VM_AS_STR(v)->data[0] == '\0') {
            SP_PUSH(VM_MAKE_INT(0));
        } else {
            char buf[64];
            const char *s = vm_to_cstr(v, buf, 64);
            SP_PUSH(VM_MAKE_INT(strada_ord(s)));
        }
        vm_temp_free(&v);
        DISPATCH();
    }

    CASE(OP_CHOMP): {
        VMValue v = SP_POP();
        char buf[64];
        const char *s = vm_to_cstr(v, buf, 64);
        size_t len = strlen(s);
        while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r')) len--;
        SP_PUSH(VM_MAKE_STR_N(s, len));
        vm_temp_free(&v);
        DISPATCH();
    }

    CASE(OP_TR): {
        /* u16 from, u16 to, u16 flags, u8 store_kind (0=none,1=local,2=cap,3=global), u16 store_idx.
         * Delegates to the runtime's strada_tr (full d/c/s/r flag support,
         * Perl count return). Mutates the subject in place unless 'r'. */
        uint16_t from_idx = read_u16(ip); ip += 2;
        uint16_t to_idx = read_u16(ip); ip += 2;
        uint16_t flags_idx = read_u16(ip); ip += 2;
        uint8_t store_kind = *ip++;
        uint16_t store_idx = read_u16(ip); ip += 2;
        VMValue str = SP_POP();
        const char *tr_flags = chunk->str_consts[flags_idx];
        StradaValue *subj = vm_to_sv(str);
        StradaValue *ret = strada_tr(subj, chunk->str_consts[from_idx],
                                     chunk->str_consts[to_idx], tr_flags);
        if (store_kind != 0 && !strchr(tr_flags, 'r')) {
            VMValue updated = sv_to_vm(subj);
            if (store_kind == 1) {
                VMValue *loc = &locals[store_idx];
                if (VM_IS_PTR(*loc) && VM_PTR_TYPE(*loc) == VM_OBJ_CELL) loc = &VM_AS_CELL(*loc)->val;
                vm_temp_free(loc);
                *loc = updated;
            } else if (store_kind == 2) {
                if (frame->closure && store_idx < (uint16_t)frame->closure->capture_count) {
                    VMValue cv = frame->closure->captures[store_idx];
                    if (VM_IS_PTR(cv) && VM_PTR_TYPE(cv) == VM_OBJ_CELL) {
                        vm_temp_free(&VM_AS_CELL(cv)->val);
                        VM_AS_CELL(cv)->val = updated;
                    } else {
                        frame->closure->captures[store_idx] = updated;
                    }
                } else {
                    vm_temp_free(&updated);
                }
            } else if (store_kind == 3) {
                vm_hash_set(vm->globals, chunk->str_consts[store_idx], updated);
            }
        }
        SP_PUSH(sv_to_vm(ret));
        if (ret) strada_decref(ret);
        strada_decref(subj);
        vm_temp_free(&str);
        DISPATCH();
    }

    CASE(OP_STR_REPEAT): {
        VMValue count = SP_POP(), str = SP_POP();
        char buf[256];
        const char *s;
        size_t slen;
        if (VM_IS_PTR(str) && VM_PTR_TYPE(str) == VM_OBJ_STR) {
            s = VM_AS_STR(str)->data; slen = VM_AS_STR(str)->len;
        } else if (VM_IS_PTR(str) && VM_PTR_TYPE(str) == VM_OBJ_STRBUF) {
            s = VM_AS_STRBUF(str)->data; slen = VM_AS_STRBUF(str)->len;
        } else {
            s = vm_to_cstr(str, buf, sizeof(buf)); slen = strlen(s);
        }
        int64_t n = vm_to_int(count);
        vm_temp_free(&count);
        if (n <= 0 || slen == 0) {
            SP_PUSH(vm_str(""));
        } else if ((uint64_t)n > (256ULL * 1024 * 1024) / slen) {
            fprintf(stderr, "VM error: string repeat overflow\n");
            SP_PUSH(vm_str(""));
        } else {
            size_t total = slen * (size_t)n;
            VMString *vs = malloc(sizeof(VMString) + total + 1);
            vm_gc_init_hdr(&vs->hdr, VM_OBJ_STR); vs->len = (uint32_t)total;
            for (int64_t i = 0; i < n; i++) memcpy(vs->data + (size_t)i * slen, s, slen);
            vs->data[total] = 0;
            SP_PUSH(VM_MAKE_PTR(vs));
        }
        vm_temp_free(&str);
        DISPATCH();
    }

    CASE(OP_CHAR_AT): {
        VMValue idx = SP_POP(), str = SP_POP();
        char buf[64];
        const char *s;
        size_t slen;
        if (VM_IS_PTR(str) && VM_PTR_TYPE(str) == VM_OBJ_STR) {
            s = VM_AS_STR(str)->data; slen = VM_AS_STR(str)->len;
        } else if (VM_IS_PTR(str) && VM_PTR_TYPE(str) == VM_OBJ_STRBUF) {
            s = VM_AS_STRBUF(str)->data; slen = VM_AS_STRBUF(str)->len;
        } else {
            s = vm_to_cstr(str, buf, sizeof(buf)); slen = strlen(s);
        }
        int64_t i = vm_to_int(idx);
        vm_temp_free(&idx);
        if (i >= 0 && (size_t)i < slen) {
            SP_PUSH(VM_MAKE_INT((unsigned char)s[i]));
        } else {
            SP_PUSH(VM_MAKE_INT(0));
        }
        vm_temp_free(&str);
        DISPATCH();
    }

    CASE(OP_BYTES): {
        VMValue v = SP_POP();
        size_t blen;
        if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_STR) blen = VM_AS_STR(v)->len;
        else if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_STRBUF) blen = VM_AS_STRBUF(v)->len;
        else { char buf[64]; blen = strlen(vm_to_cstr(v, buf, 64)); }
        SP_PUSH(VM_MAKE_INT((int64_t)blen));
        vm_temp_free(&v);
        DISPATCH();
    }

    /* ===== OOP ===== */
    CASE(OP_HASH_BLESS): {
        VMValue cn = SP_POP(), hash = SP_POP();
        char cbuf[128];
        const char *cname = vm_to_cstr(cn, cbuf, sizeof(cbuf));
        if (VM_IS_PTR(hash) && VM_PTR_TYPE(hash) == VM_OBJ_HASH && cname[0]) {
            VMHash *h = VM_AS_HASH(hash);
            if (h->class_name) free(h->class_name);
            h->class_name = strdup(cname);
        }
        SP_PUSH(hash); vm_val_free(&cn); DISPATCH();
    }

    CASE(OP_HASH_REF): DISPATCH();

    CASE(OP_MAKE_SREF_LOCAL): {
        uint16_t slot = read_u16(ip); ip += 2;
        if (__builtin_expect(slot > (uint16_t)chunk->local_count, 0)) {
            fprintf(stderr, "VM error: local slot %u out of bounds (%d)\n", (unsigned)slot, chunk->local_count);
            vm->stack_top = sp; vm->exec_base = saved_exec_base; VM_EXEC_RETURN(VM_UNDEF_VAL);
        }
        VMValue cur = locals[slot];
        VMValue cell_val;
        if (VM_IS_PTR(cur) && VM_PTR_TYPE(cur) == VM_OBJ_CELL) {
            cell_val = cur;
        } else {
            VMCell *cell = calloc(1, sizeof(VMCell));
            vm_gc_init_hdr(&cell->hdr, VM_OBJ_CELL);
            cell->val = cur;
            cell_val = VM_MAKE_PTR(cell);
            locals[slot] = cell_val;
        }
        VMScalarRef *sr = calloc(1, sizeof(VMScalarRef));
        vm_gc_init_hdr(&sr->hdr, VM_OBJ_SREF);
        sr->cell = cell_val;
        sr->global_name = NULL;
        SP_PUSH(VM_MAKE_PTR(sr));
        DISPATCH();
    }

    CASE(OP_MAKE_SREF_GLOBAL): {
        uint16_t name_idx = read_u16(ip); ip += 2;
        const char *gname = chunk->str_consts[name_idx];
        VMScalarRef *sr = calloc(1, sizeof(VMScalarRef));
        vm_gc_init_hdr(&sr->hdr, VM_OBJ_SREF);
        sr->cell = VM_UNDEF_VAL;
        sr->global_name = strdup(gname);
        SP_PUSH(VM_MAKE_PTR(sr));
        DISPATCH();
    }

    CASE(OP_MAKE_SREF_VAL): {
        VMValue v = SP_POP();
        VMValue cell_val;
        if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_CELL) {
            cell_val = v;
        } else {
            VMCell *cell = calloc(1, sizeof(VMCell));
            vm_gc_init_hdr(&cell->hdr, VM_OBJ_CELL);
            cell->val = v;
            cell_val = VM_MAKE_PTR(cell);
        }
        VMScalarRef *sr = calloc(1, sizeof(VMScalarRef));
        vm_gc_init_hdr(&sr->hdr, VM_OBJ_SREF);
        sr->cell = cell_val;
        sr->global_name = NULL;
        SP_PUSH(VM_MAKE_PTR(sr));
        DISPATCH();
    }

    CASE(OP_DEREF_SCALAR): {
        VMValue r = SP_POP();
        if (VM_IS_PTR(r) && VM_PTR_TYPE(r) == VM_OBJ_SREF) {
            VMScalarRef *sr = VM_AS_SREF(r);
            if (sr->global_name) {
                SP_PUSH(vm_val_copy(vm_hash_get(vm->globals, sr->global_name)));
            } else if (VM_IS_PTR(sr->cell) && VM_PTR_TYPE(sr->cell) == VM_OBJ_CELL) {
                SP_PUSH(vm_val_copy(VM_AS_CELL(sr->cell)->val));
            } else {
                SP_PUSH(VM_UNDEF_VAL);
            }
        } else if (VM_IS_PTR(r) && VM_PTR_TYPE(r) == VM_OBJ_CELL) {
            SP_PUSH(vm_val_copy(VM_AS_CELL(r)->val));
        } else {
            SP_PUSH(r);
        }
        DISPATCH();
    }

    CASE(OP_STORE_SREF): {
        VMValue newv = SP_POP();
        VMValue r = SP_POP();
        if (VM_IS_PTR(r) && VM_PTR_TYPE(r) == VM_OBJ_SREF) {
            VMScalarRef *sr = VM_AS_SREF(r);
            if (sr->global_name) {
                vm_hash_set(vm->globals, sr->global_name, vm_val_copy(newv));
            } else if (VM_IS_PTR(sr->cell) && VM_PTR_TYPE(sr->cell) == VM_OBJ_CELL) {
                VMValue old_cv = VM_AS_CELL(sr->cell)->val;
                vm_temp_free(&old_cv);
                VM_AS_CELL(sr->cell)->val = vm_val_copy(newv);
            }
        } else if (VM_IS_PTR(r) && VM_PTR_TYPE(r) == VM_OBJ_CELL) {
            VMValue old_cv = VM_AS_CELL(r)->val;
            vm_temp_free(&old_cv);
            VM_AS_CELL(r)->val = vm_val_copy(newv);
        }
        SP_PUSH(newv);
        DISPATCH();
    }

    CASE(OP_IS_UNDEF): { VMValue v = SP_POP(); SP_PUSH(VM_MAKE_INT(VM_IS_UNDEF(v))); vm_temp_free(&v); DISPATCH(); }

    CASE(OP_ISA): {
        VMValue tc = SP_POP(), obj = SP_POP();
        int res = 0;
        char tbuf[128];
        const char *tcname = vm_to_cstr(tc, tbuf, sizeof(tbuf));
        if (tcname[0] && VM_IS_PTR(obj)) {
            if (VM_PTR_TYPE(obj) == VM_OBJ_HASH && VM_AS_HASH(obj)->class_name) {
                res = vm_class_isa(vm->program, VM_AS_HASH(obj)->class_name, tcname, 0);
            } else if (VM_PTR_TYPE(obj) == VM_OBJ_STR && VM_AS_STR(obj)->data && VM_AS_STR(obj)->data[0]) {
                res = vm_class_isa(vm->program, VM_AS_STR(obj)->data, tcname, 0);
            } else if (VM_PTR_TYPE(obj) == VM_OBJ_STRBUF && VM_AS_STRBUF(obj)->data && VM_AS_STRBUF(obj)->data[0]) {
                res = vm_class_isa(vm->program, VM_AS_STRBUF(obj)->data, tcname, 0);
            } else if (VM_PTR_TYPE(obj) == VM_OBJ_NATIVE_SV) {
                VMNativeSV *nsv = (VMNativeSV*)VM_TO_PTR(obj);
                res = strada_isa(nsv->sv, tcname) ? 1 : 0;
            }
        }
        SP_PUSH(VM_MAKE_INT(res)); vm_val_free(&tc); vm_temp_free(&obj); DISPATCH();
    }

    CASE(OP_CAN): {
        VMValue method = SP_POP(), obj = SP_POP();
        int res = 0;
        char mbuf[128];
        const char *mname = vm_to_cstr(method, mbuf, sizeof(mbuf));
        if (mname[0] && VM_IS_PTR(obj)) {
            const char *cls = NULL;
            if (VM_PTR_TYPE(obj) == VM_OBJ_HASH && VM_AS_HASH(obj)->class_name) {
                cls = VM_AS_HASH(obj)->class_name;
            } else if (VM_PTR_TYPE(obj) == VM_OBJ_STR && VM_AS_STR(obj)->data && VM_AS_STR(obj)->data[0]) {
                cls = VM_AS_STR(obj)->data;
            } else if (VM_PTR_TYPE(obj) == VM_OBJ_STRBUF && VM_AS_STRBUF(obj)->data && VM_AS_STRBUF(obj)->data[0]) {
                cls = VM_AS_STRBUF(obj)->data;
            }
            if (cls) {
                if (strcmp(mname, "isa") == 0 || strcmp(mname, "can") == 0 ||
                    vm_find_method(vm->program, cls, mname) >= 0) {
                    res = 1;
                }
            } else if (VM_PTR_TYPE(obj) == VM_OBJ_NATIVE_SV) {
                VMNativeSV *nsv = (VMNativeSV*)VM_TO_PTR(obj);
                res = strada_can(nsv->sv, mname) ? 1 : 0;
            }
        }
        SP_PUSH(VM_MAKE_INT(res));
        vm_val_free(&method);
        vm_temp_free(&obj);
        DISPATCH();
    }

    CASE(OP_METHOD_CALL): {
        mcall_idx = read_u16(ip); ip += 2;
        mcall_argc = *ip++;
        mcall_method_cstr = chunk->str_consts[mcall_idx];
        mcall_is_dyn = 0;
        goto L_do_op_method_call;
    }

    CASE(OP_METHOD_CALL_SPREAD): {
        mcall_idx = read_u16(ip); ip += 2;
        VMValue arr_val = SP_POP();
        mcall_argc = 0;
        if (VM_IS_PTR(arr_val) && VM_PTR_TYPE(arr_val) == VM_OBJ_ARRAY) {
            VMArray *aa = VM_AS_ARRAY(arr_val);
            mcall_argc = aa->size;
            for (int i = 0; i < mcall_argc; i++) SP_PUSH(aa->items[i]);
        }
        mcall_method_cstr = chunk->str_consts[mcall_idx];
        mcall_is_dyn = 0;
        goto L_do_op_method_call;
    }

    L_do_op_method_call: {
        int argc = mcall_argc;
        const char *method = mcall_method_cstr;
        VMValue self = stack[sp - argc - 1];

        /* Handle native OOP objects (VMNativeSV) — use strada_method_call for proper dispatch */
        if (VM_IS_PTR(self) && VM_PTR_TYPE(self) == VM_OBJ_NATIVE_SV) {
            VMNativeSV *nsv = (VMNativeSV*)VM_TO_PTR(self);
            StradaValue *self_sv = nsv->sv;
            VMValue raw_buf[16];
            VMValue *raw_args = (argc > 16) ? malloc((size_t)argc * sizeof(VMValue)) : raw_buf;
            for (int ai = argc - 1; ai >= 0; ai--) raw_args[ai] = stack[--sp];
            stack[--sp]; /* pop self */

            StradaValue *packed_args = strada_new_array();
            StradaValue *fh_buf[16];
            StradaValue **fh_ptrs = (argc > 16) ? malloc((size_t)argc * sizeof(StradaValue*)) : fh_buf;
            for (int ai = 0; ai < argc; ai++) {
                StradaValue *sv = vm_to_sv(raw_args[ai]);
                fh_ptrs[ai] = (VM_IS_PTR(raw_args[ai]) && VM_PTR_TYPE(raw_args[ai]) == VM_OBJ_FILEHANDLE) ? sv : NULL;
                strada_array_push_take(packed_args->value.av, sv);
            }
            vm->gc_pause++;
            StradaValue *nr = strada_method_call(self_sv, method, packed_args);
            vm->gc_pause--;
            for (int ai = 0; ai < argc; ai++) {
                if (fh_ptrs[ai] && !STRADA_IS_TAGGED_INT(fh_ptrs[ai]) && fh_ptrs[ai]->refcount <= 1) {
                    fh_ptrs[ai]->value.fh = NULL;
                }
            }
            strada_decref(packed_args);
            if (fh_ptrs != fh_buf) free(fh_ptrs);
            if (raw_args != raw_buf) free(raw_args);
            SP_PUSH(nr ? sv_to_vm(nr) : VM_UNDEF_VAL);
            if (nr) strada_decref(nr);
            DISPATCH();
        }

        VMHash *h = (VM_IS_PTR(self) && VM_PTR_TYPE(self) == VM_OBJ_HASH) ? VM_AS_HASH(self) : NULL;
        const char *cls_name = NULL;
        int is_hash_blessed = 0;
        if (h && h->class_name) {
            cls_name = h->class_name;
            is_hash_blessed = 1;
        } else if (VM_IS_PTR(self) && VM_PTR_TYPE(self) == VM_OBJ_STR) {
            cls_name = VM_AS_STR(self)->data;
        } else if (VM_IS_PTR(self) && VM_PTR_TYPE(self) == VM_OBJ_STRBUF) {
            cls_name = VM_AS_STRBUF(self)->data;
        }

        int fidx = -1;
        if (cls_name && cls_name[0]) {
            /* Check for around modifier across MRO (most-derived first) */
            int around_idx = vm_find_around_modifier(vm->program, cls_name, method, 0);

            if (is_hash_blessed && !mcall_is_dyn) {
                fidx = vm_find_method_cached(vm, cls_name, method);
            } else {
                fidx = vm_find_method(vm->program, cls_name, method);
            }

            if (around_idx >= 0 && fidx >= 0) {
                if (vm->recursion_limit > 0 && (int)vm->frame_count >= vm->recursion_limit) {
                    fprintf(stderr, "Deep recursion detected (limit %d) — see core::set_recursion_limit\n", vm->recursion_limit);
                    exit(1);
                }
                /* Moose convention: around receives ($orig, $self, @args) */
                VMClosure *orig_cl = calloc(1, sizeof(VMClosure));
                vm_gc_init_hdr(&orig_cl->hdr, VM_OBJ_CLOSURE);
                orig_cl->func_idx = fidx;
                orig_cl->capture_count = 0;
                orig_cl->captures = NULL;

                /* Pop method args and self from stack */
                VMValue saved_buf[256];
                VMValue *saved_args = (argc > 256) ? malloc((size_t)argc * sizeof(VMValue)) : saved_buf;
                for (int j = argc - 1; j >= 0; j--) saved_args[j] = stack[--sp];
                VMValue self_val = stack[--sp];

                VMChunk *callee = &vm->program->funcs[around_idx];
                frame->ip = ip; vm->stack_top = sp;
                if (vm->frame_count >= vm->frame_cap) {
                    vm->frame_cap *= 2;
                    vm->frames = realloc(vm->frames, vm->frame_cap * sizeof(VMFrame));
                }
                VMFrame *nf = &vm->frames[vm->frame_count++];
                nf->chunk = callee; nf->ip = callee->code;
                nf->locals = locals_alloc(callee->local_count + 1, 0);
                nf->stack_base = sp;
                nf->closure = NULL;
                nf->global_save_depth = vm->global_save_count;
                if (callee->has_variadic) {
                    int total_args = argc + 2;
                    int fixed = callee->fixed_param_count;
                    int vc = total_args - fixed;
                    VMArray *va = vm_array_new(vc > 0 ? vc : 4);
                    VMValue all_buf[258];
                    VMValue *all_args = (total_args > 258) ? malloc((size_t)total_args * sizeof(VMValue)) : all_buf;
                    all_args[0] = VM_MAKE_PTR(orig_cl);
                    all_args[1] = self_val;
                    for (int j = 0; j < argc; j++) all_args[2 + j] = saved_args[j];
                    for (int j = 0; j < fixed && j < total_args; j++) {
                        if (j < callee->local_count + 1) nf->locals[j] = all_args[j];
                    }
                    for (int j = fixed; j < total_args; j++) vm_array_push(va, all_args[j]);
                    if (fixed < callee->local_count + 1) nf->locals[fixed] = VM_MAKE_PTR(va);
                    if (all_args != all_buf) free(all_args);
                } else {
                    nf->locals[0] = VM_MAKE_PTR(orig_cl);
                    if (1 < callee->local_count + 1) nf->locals[1] = self_val;
                    for (int j = 0; j < argc && (2 + j) < callee->local_count + 1; j++) {
                        nf->locals[2 + j] = saved_args[j];
                    }
                }
                if (saved_args != saved_buf) free(saved_args);
                frame = nf; chunk = callee; ip = callee->code; locals = nf->locals;
                vm->stack_top = sp;
                DISPATCH();
            }

            if (fidx >= 0) {
                if (vm->recursion_limit > 0 && (int)vm->frame_count >= vm->recursion_limit) {
                    fprintf(stderr, "Deep recursion detected (limit %d) — see core::set_recursion_limit\n", vm->recursion_limit);
                    exit(1);
                }
                /* Normal method call */
                VMChunk *callee = &vm->program->funcs[fidx];
                frame->ip = ip; vm->stack_top = sp;
                if (vm->frame_count >= vm->frame_cap) {
                    vm->frame_cap *= 2;
                    vm->frames = realloc(vm->frames, vm->frame_cap * sizeof(VMFrame));
                }
                VMFrame *nf = &vm->frames[vm->frame_count++];
                nf->chunk = callee; nf->ip = callee->code;
                nf->locals = locals_alloc(callee->local_count + 1, callee->int_only);
                nf->stack_base = sp - argc - 1;
                nf->closure = NULL;
                nf->global_save_depth = vm->global_save_count;
                if (__builtin_expect(callee->has_variadic, 0)) {
                    /* No-param method: pack self + args into @_ array */
                    int total_args = argc + 1; /* self + args */
                    int fixed = callee->fixed_param_count;
                    int vc = total_args - fixed;
                    VMArray *va = vm_array_new(vc > 0 ? vc : 4);
                    VMValue *tmp = malloc(total_args * sizeof(VMValue));
                    for (int i = argc - 1; i >= 0; i--) tmp[i + 1] = stack[--sp];
                    tmp[0] = stack[--sp]; /* self */
                    for (int i = 0; i < fixed && i < total_args; i++) {
                        if (i < callee->local_count + 1) nf->locals[i] = tmp[i];
                    }
                    for (int i = fixed; i < total_args; i++) vm_array_push(va, tmp[i]);
                    if (fixed < callee->local_count + 1) nf->locals[fixed] = VM_MAKE_PTR(va);
                    free(tmp);
                } else {
                    int pmax = callee->local_count + 1;
                    if (callee->fixed_param_count > 0 && callee->fixed_param_count < pmax) {
                        pmax = callee->fixed_param_count;
                    }
                    for (int i = argc - 1; i >= 0; i--) {
                        VMValue av = stack[--sp];
                        if (i + 1 < pmax) nf->locals[i + 1] = av;
                    }
                    nf->locals[0] = stack[--sp]; /* self */
                }
                frame = nf; chunk = callee; ip = callee->code; locals = nf->locals;
                vm->stack_top = sp;
                DISPATCH();
            }

            /* Try native (import_lib) method */
            if (vm->program->native_count > 0) {
                char native_name[600];
                snprintf(native_name, sizeof(native_name), "%s_%s", cls_name, method);
                for (int ni = 0; ni < vm->program->native_count; ni++) {
                    if (strcmp(vm->program->natives[ni].name, native_name) == 0) {
                        VMNativeEntry *ne = &vm->program->natives[ni];
                        int total_argc = argc + 1;
                        VMValue raw_buf[16];
                        VMValue *raw_args = (total_argc > 16) ? malloc((size_t)total_argc * sizeof(VMValue)) : raw_buf;
                        for (int ai = argc - 1; ai >= 0; ai--) raw_args[ai + 1] = stack[--sp];
                        raw_args[0] = stack[--sp]; /* self */
                        VMValue ret = vm_invoke_native_entry(vm, ne, raw_args, total_argc, h);
                        if (raw_args != raw_buf) free(raw_args);
                        SP_PUSH(ret);
                        vm->stack_top = sp;
                        DISPATCH();
                    }
                }
            }

            /* Try AUTOLOAD across class/parent hierarchy before accessor fallback */
            {
                int al_fidx = (is_hash_blessed && !mcall_is_dyn)
                    ? vm_find_method_cached(vm, cls_name, "AUTOLOAD")
                    : vm_find_method(vm->program, cls_name, "AUTOLOAD");
                if (al_fidx >= 0) {
                    if (vm->recursion_limit > 0 && (int)vm->frame_count >= vm->recursion_limit) {
                        fprintf(stderr, "Deep recursion detected (limit %d) — see core::set_recursion_limit\n", vm->recursion_limit);
                        exit(1);
                    }
                    VMChunk *callee = &vm->program->funcs[al_fidx];
                    frame->ip = ip; vm->stack_top = sp;
                    if (vm->frame_count >= vm->frame_cap) {
                        vm->frame_cap *= 2;
                        vm->frames = realloc(vm->frames, vm->frame_cap * sizeof(VMFrame));
                    }
                    VMFrame *nf = &vm->frames[vm->frame_count++];
                    nf->chunk = callee; nf->ip = callee->code;
                    nf->locals = locals_alloc(callee->local_count + 1, callee->int_only);
                    nf->stack_base = sp - argc - 1;
                    nf->closure = NULL;
                    nf->global_save_depth = vm->global_save_count;
                    int total_al = argc + 2;
                    VMValue al_buf_s[18];
                    VMValue *al_args = (total_al > 18) ? malloc((size_t)total_al * sizeof(VMValue)) : al_buf_s;
                    for (int j = argc - 1; j >= 0; j--) al_args[2 + j] = stack[--sp];
                    al_args[0] = stack[--sp]; /* self */
                    al_args[1] = vm_str(method);
                    if (callee->has_variadic) {
                        int fixed = callee->fixed_param_count;
                        if (fixed < 0) fixed = 0;
                        int vc = total_al - fixed;
                        VMArray *va = vm_array_new(vc > 0 ? vc : 4);
                        for (int j = 0; j < fixed && j < total_al; j++) {
                            if (j < callee->local_count + 1) nf->locals[j] = al_args[j];
                        }
                        for (int j = fixed; j < total_al; j++) vm_array_push(va, al_args[j]);
                        if (fixed < callee->local_count + 1) nf->locals[fixed] = VM_MAKE_PTR(va);
                    } else {
                        for (int j = 0; j < total_al && j < callee->local_count + 1; j++) {
                            nf->locals[j] = al_args[j];
                        }
                    }
                    if (al_args != al_buf_s) free(al_args);
                    frame = nf; chunk = callee; ip = callee->code; locals = nf->locals;
                    vm->stack_top = sp;
                    DISPATCH();
                }
            }

            if (is_hash_blessed) {
                /* Accessor: try hash get / set_ */
                sp -= (argc + 1);
                VMValue val = vm_hash_get(h, method);
                if (!VM_IS_UNDEF(val) || (argc == 0 && vm_hash_exists(h, method))) {
                    if (VM_IS_PTR(val) && VM_PTR_TYPE(val) == VM_OBJ_STR) SP_PUSH(vm_str(VM_AS_STR(val)->data));
                    else SP_PUSH(vm_val_copy(val));
                    DISPATCH();
                } else if (strncmp(method, "set_", 4) == 0 && argc == 1) {
                    /* After sp -= (argc+1), stack layout is: [self, arg] at sp, sp+1 */
                    vm_hash_set(h, method + 4, stack[sp + 1]);
                    SP_PUSH(VM_UNDEF_VAL);
                    DISPATCH();
                } else {
                    vm->stack_top = sp;
                    char errbuf[600];
                    snprintf(errbuf, sizeof(errbuf), "Can't locate method \"%s\" via package \"%s\" or its parents", method, h->class_name);
                    throw_exc = vm_str(errbuf);
                    goto L_vm_throw_exc;
                }
            } else {
                sp -= (argc + 1);
                vm->stack_top = sp;
                char errbuf[600];
                snprintf(errbuf, sizeof(errbuf), "Can't locate method \"%s\" via package \"%s\" or its parents", method, cls_name);
                throw_exc = vm_str(errbuf);
                goto L_vm_throw_exc;
            }
        }

        /* Unblessed / non-package target */
        sp -= (argc + 1);
        if (h && !h->class_name && argc == 0 && !mcall_is_dyn && vm_hash_exists(h, method)) {
            VMValue val = vm_hash_get(h, method);
            if (VM_IS_PTR(val) && VM_PTR_TYPE(val) == VM_OBJ_STR) SP_PUSH(vm_str(VM_AS_STR(val)->data));
            else SP_PUSH(vm_val_copy(val));
            DISPATCH();
        }
        vm->stack_top = sp;
        char errbuf[600];
        snprintf(errbuf, sizeof(errbuf), "Can't call method \"%s\" on unblessed reference", method);
        throw_exc = vm_str(errbuf);
        goto L_vm_throw_exc;
    }

    CASE(OP_SUPER_CALL): {
        uint16_t pkg_idx = read_u16(ip); ip += 2;
        uint16_t method_idx = read_u16(ip); ip += 2;
        uint8_t argc = *ip++;
        const char *def_pkg = chunk->str_consts[pkg_idx];
        const char *method = chunk->str_consts[method_idx];
        VMValue self = stack[sp - argc - 1];

        if ((!def_pkg || !def_pkg[0]) && VM_IS_PTR(self) && VM_PTR_TYPE(self) == VM_OBJ_HASH) {
            VMHash *h = VM_AS_HASH(self);
            if (h->class_name) def_pkg = h->class_name;
        }

        int fidx = vm_find_super_method(vm->program, def_pkg, method, 0);
        if (fidx >= 0) {
            if (vm->recursion_limit > 0 && (int)vm->frame_count >= vm->recursion_limit) {
                fprintf(stderr, "Deep recursion detected (limit %d) — see core::set_recursion_limit\n", vm->recursion_limit);
                exit(1);
            }
            VMChunk *callee = &vm->program->funcs[fidx];
            frame->ip = ip; vm->stack_top = sp;
            if (vm->frame_count >= vm->frame_cap) {
                vm->frame_cap *= 2;
                vm->frames = realloc(vm->frames, vm->frame_cap * sizeof(VMFrame));
            }
            VMFrame *nf = &vm->frames[vm->frame_count++];
            nf->chunk = callee; nf->ip = callee->code;
            nf->locals = locals_alloc(callee->local_count + 1, callee->int_only);
            nf->stack_base = sp - argc - 1;
            nf->closure = NULL;
            nf->global_save_depth = vm->global_save_count;
            if (__builtin_expect(callee->has_variadic, 0)) {
                int total_args = argc + 1;
                int fixed = callee->fixed_param_count;
                int vc = total_args - fixed;
                VMArray *va = vm_array_new(vc > 0 ? vc : 4);
                VMValue *tmp = malloc(total_args * sizeof(VMValue));
                for (int i = argc - 1; i >= 0; i--) tmp[i + 1] = stack[--sp];
                tmp[0] = stack[--sp];
                for (int i = 0; i < fixed && i < total_args; i++) {
                    if (i < callee->local_count + 1) nf->locals[i] = tmp[i];
                }
                for (int i = fixed; i < total_args; i++) vm_array_push(va, tmp[i]);
                if (fixed < callee->local_count + 1) nf->locals[fixed] = VM_MAKE_PTR(va);
                free(tmp);
            } else {
                for (int i = argc - 1; i >= 0; i--) {
                    VMValue av = stack[--sp];
                    if ((i + 1) < callee->local_count + 1) nf->locals[i + 1] = av;
                }
                nf->locals[0] = stack[--sp];
            }
            frame = nf; chunk = callee; ip = callee->code; locals = nf->locals;
            vm->stack_top = sp;
            DISPATCH();
        }

        /* No parent method found — throw catchable exception */
        sp -= (argc + 1);
        vm->stack_top = sp;
        char errbuf[256];
        snprintf(errbuf, sizeof(errbuf), "SUPER::%s - no parent method found", method);
        throw_exc = vm_str(errbuf);
        goto L_vm_throw_exc;
    }

    CASE(OP_DYN_METHOD_CALL): {
        mcall_argc = *ip++;
        VMValue method_name = SP_POP();
        const char *mn = vm_to_cstr(method_name, mcall_dyn_buf, sizeof(mcall_dyn_buf));
        if (mn != mcall_dyn_buf) {
            snprintf(mcall_dyn_buf, sizeof(mcall_dyn_buf), "%s", mn);
        }
        vm_val_free(&method_name);
        mcall_method_cstr = mcall_dyn_buf;
        mcall_is_dyn = 1;
        goto L_do_op_method_call;
    }

    CASE(OP_APPEND_LOCAL): {
        uint16_t slot = read_u16(ip); ip += 2;
        VMValue rhs = SP_POP();
        if (__builtin_expect(vm_has_stringify(vm->program, rhs), 0)) {
            frame->ip = ip; vm->stack_top = sp;
            rhs = vm_stringify_ov(vm, rhs);
            stack = vm->stack; RELOAD();
            if (__builtin_expect(vm->has_pending_exc, 0)) {
                throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                goto L_vm_throw_exc;
            }
        }
        char buf[64]; size_t al;
        const char *as = vm_bytes_view(rhs, buf, sizeof(buf), &al);
        VMValue *loc = &locals[slot];
        if (VM_IS_PTR(*loc) && VM_PTR_TYPE(*loc) == VM_OBJ_CELL) loc = &VM_AS_CELL(*loc)->val;
        if (VM_IS_PTR(*loc) && VM_PTR_TYPE(*loc) == VM_OBJ_STRBUF) {
            VMStrBuf *sb = VM_AS_STRBUF(*loc);
            if (sb->len + al >= sb->cap) { sb->cap = (sb->len + al) * 2 + 64; sb->data = realloc(sb->data, sb->cap); }
            memcpy(sb->data + sb->len, as, al); sb->len += al; sb->data[sb->len] = 0;
        } else {
            VMValue lhs = *loc;
            if (__builtin_expect(vm_has_stringify(vm->program, lhs), 0)) {
                VMValue rcopy = VM_MAKE_STR_N(as, al);
                frame->ip = ip; SP_PUSH(rcopy); vm->stack_top = sp;
                lhs = vm_stringify_ov(vm, lhs);
                stack = vm->stack; RELOAD();
                if (__builtin_expect(vm->has_pending_exc, 0)) {
                    throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                    goto L_vm_throw_exc;
                }
                rhs = SP_POP();
                as = vm_bytes_view(rhs, buf, sizeof(buf), &al);
                loc = &locals[slot];
                if (VM_IS_PTR(*loc) && VM_PTR_TYPE(*loc) == VM_OBJ_CELL) loc = &VM_AS_CELL(*loc)->val;
            }
            const char *ex = ""; size_t el = 0; char lbuf[64];
            if (!VM_IS_UNDEF(lhs)) {
                ex = vm_bytes_view(lhs, lbuf, sizeof(lbuf), &el);
            }
            VMStrBuf *sb = malloc(sizeof(VMStrBuf));
            vm_gc_init_hdr(&sb->hdr, VM_OBJ_STRBUF);
            sb->cap = (el + al) * 2 + 64; sb->data = malloc(sb->cap);
            memcpy(sb->data, ex, el); memcpy(sb->data + el, as, al);
            sb->len = el + al; sb->data[sb->len] = 0;
            vm_val_free(loc); *loc = VM_MAKE_PTR(sb);
        }
        vm_val_free(&rhs); DISPATCH();
    }

    /* Append a string constant directly to a local StrBuf — no allocation for the RHS */
    CASE(OP_APPEND_CONST): {
        uint16_t str_idx = read_u16(ip); ip += 2;
        uint16_t slot = read_u16(ip); ip += 2;
        const char *as = chunk->str_consts[str_idx];
        size_t al = strlen(as);
        VMValue *loc = &locals[slot];
        if (VM_IS_PTR(*loc) && VM_PTR_TYPE(*loc) == VM_OBJ_CELL) loc = &VM_AS_CELL(*loc)->val;
        if (VM_IS_PTR(*loc) && VM_PTR_TYPE(*loc) == VM_OBJ_STRBUF) {
            VMStrBuf *sb = VM_AS_STRBUF(*loc);
            if (sb->len + al >= sb->cap) { sb->cap = (sb->len + al) * 2 + 64; sb->data = realloc(sb->data, sb->cap); }
            memcpy(sb->data + sb->len, as, al); sb->len += al; sb->data[sb->len] = 0;
        } else {
            VMValue lhs = *loc;
            if (__builtin_expect(vm_has_stringify(vm->program, lhs), 0)) {
                frame->ip = ip; vm->stack_top = sp;
                lhs = vm_stringify_ov(vm, lhs);
                stack = vm->stack; RELOAD();
                if (__builtin_expect(vm->has_pending_exc, 0)) {
                    throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                    goto L_vm_throw_exc;
                }
                as = chunk->str_consts[str_idx];
                al = strlen(as);
                loc = &locals[slot];
                if (VM_IS_PTR(*loc) && VM_PTR_TYPE(*loc) == VM_OBJ_CELL) loc = &VM_AS_CELL(*loc)->val;
            }
            const char *ex = ""; size_t el = 0; char lbuf[64];
            if (!VM_IS_UNDEF(lhs)) {
                ex = vm_bytes_view(lhs, lbuf, sizeof(lbuf), &el);
            }
            VMStrBuf *sb = malloc(sizeof(VMStrBuf));
            vm_gc_init_hdr(&sb->hdr, VM_OBJ_STRBUF);
            sb->cap = (el + al) * 2 + 64; sb->data = malloc(sb->cap);
            memcpy(sb->data, ex, el); memcpy(sb->data + el, as, al);
            sb->len = el + al; sb->data[sb->len] = 0;
            vm_val_free(loc); *loc = VM_MAKE_PTR(sb);
        }
        DISPATCH();
    }

    /* Int/string concat key building supporting INT64_MIN, >128B keys, and stringify overloads */
    #define CONCAT_KEY_FORMAT_INT(pfx_ptr, plen, suffix_val, key_buf, kptr, heap_key, klen) do { \
        int64_t _n = VM_INT_VAL(suffix_val); \
        uint64_t _u = (_n < 0) ? (0ULL - (uint64_t)_n) : (uint64_t)_n; \
        char _tmp[24]; int _tl = 0; \
        if (_u == 0) { _tmp[_tl++] = '0'; } \
        else { while (_u > 0) { _tmp[_tl++] = (char)('0' + (_u % 10)); _u /= 10; } } \
        size_t _need = (plen) + (_n < 0 ? 1 : 0) + (size_t)_tl; \
        if (_need + 1 > sizeof(key_buf)) { heap_key = malloc(_need + 1); kptr = heap_key; } \
        else { heap_key = NULL; kptr = key_buf; } \
        memcpy(kptr, pfx_ptr, plen); \
        char *_p = kptr + (plen); \
        if (_n < 0) *_p++ = '-'; \
        for (int _i = _tl - 1; _i >= 0; _i--) *_p++ = _tmp[_i]; \
        *_p = '\0'; \
        klen = _need; \
    } while(0)

    #define CONCAT_KEY_FORMAT_BYTES(pfx_ptr, plen, suffix_val, key_buf, kptr, heap_key, klen) do { \
        char _sbuf[64]; size_t _slen = 0; \
        const char *_ss = vm_bytes_view(suffix_val, _sbuf, sizeof(_sbuf), &_slen); \
        size_t _need = (plen) + _slen; \
        if (_need + 1 > sizeof(key_buf)) { heap_key = malloc(_need + 1); kptr = heap_key; } \
        else { heap_key = NULL; kptr = key_buf; } \
        memcpy(kptr, pfx_ptr, plen); \
        if (_slen > 0) memcpy(kptr + (plen), _ss, _slen); \
        kptr[_need] = '\0'; \
        klen = _need; \
    } while(0)

    CASE(OP_HASH_GET_CONCAT): {
        uint16_t pfx_idx = read_u16(ip); ip += 2;
        VMValue suffix = SP_POP(), hash = SP_POP();
        if (!VM_IS_PTR(hash) || VM_PTR_TYPE(hash) != VM_OBJ_HASH) {
            vm_temp_free(&suffix);
            SP_PUSH(VM_UNDEF_VAL);
            DISPATCH();
        }
        if (!VM_IS_INT(suffix) && __builtin_expect(vm_has_stringify(vm->program, suffix), 0)) {
            frame->ip = ip; SP_PUSH(hash); vm->stack_top = sp;
            suffix = vm_stringify_ov(vm, suffix);
            stack = vm->stack; RELOAD();
            hash = SP_POP();
            if (__builtin_expect(vm->has_pending_exc, 0)) {
                vm_temp_free(&suffix);
                throw_exc = vm->pending_exc;
                vm->pending_exc = VM_UNDEF_VAL;
                vm->has_pending_exc = 0;
                goto L_vm_throw_exc;
            }
        }
        const char *pfx = chunk->str_consts[pfx_idx];
        size_t plen = strlen(pfx);
        char key_buf[256]; char *kptr = key_buf; char *heap_key = NULL; size_t klen = 0;
        if (VM_IS_INT(suffix)) {
            CONCAT_KEY_FORMAT_INT(pfx, plen, suffix, key_buf, kptr, heap_key, klen);
        } else {
            CONCAT_KEY_FORMAT_BYTES(pfx, plen, suffix, key_buf, kptr, heap_key, klen);
        }
        VMHash *h = VM_AS_HASH(hash);
        if (__builtin_expect(!VM_IS_UNDEF(h->tied_obj), 0)) {
            int ffidx = -1;
            if (VM_IS_PTR(h->tied_obj) && VM_PTR_TYPE(h->tied_obj) == VM_OBJ_HASH &&
                VM_AS_HASH(h->tied_obj)->class_name)
                ffidx = vm_find_method(vm->program, VM_AS_HASH(h->tied_obj)->class_name, "FETCH");
            if (ffidx >= 0) {
                VMValue ksv = VM_MAKE_STR_N(kptr, klen);
                free(heap_key);
                vm_temp_free(&suffix);
                frame->ip = ip; vm->stack_top = sp;
                VMValue cargs[2] = { h->tied_obj, ksv };
                VMValue r = vm_execute_call(vm, ffidx, cargs, 2);
                stack = vm->stack; RELOAD();
                if (__builtin_expect(vm->has_pending_exc, 0)) {
                    throw_exc = vm->pending_exc;
                    vm->pending_exc = VM_UNDEF_VAL;
                    vm->has_pending_exc = 0;
                    goto L_vm_throw_exc;
                }
                SP_PUSH(r);
                DISPATCH();
            }
        }
        VMValue r = vm_hash_get(h, kptr);
        SP_PUSH(vm_val_copy(r));
        free(heap_key);
        vm_temp_free(&suffix);
        DISPATCH();
    }

    CASE(OP_HASH_SET_CONCAT): {
        uint16_t pfx_idx = read_u16(ip); ip += 2;
        VMValue val = SP_POP(), suffix = SP_POP(), hash = SP_POP();
        if (!VM_IS_PTR(hash) || VM_PTR_TYPE(hash) != VM_OBJ_HASH) {
            vm_temp_free(&val); vm_temp_free(&suffix);
            DISPATCH();
        }
        if (!VM_IS_INT(suffix) && __builtin_expect(vm_has_stringify(vm->program, suffix), 0)) {
            frame->ip = ip; SP_PUSH(hash); SP_PUSH(val); vm->stack_top = sp;
            suffix = vm_stringify_ov(vm, suffix);
            stack = vm->stack; RELOAD();
            val = SP_POP(); hash = SP_POP();
            if (__builtin_expect(vm->has_pending_exc, 0)) {
                vm_temp_free(&val); vm_temp_free(&suffix);
                throw_exc = vm->pending_exc;
                vm->pending_exc = VM_UNDEF_VAL;
                vm->has_pending_exc = 0;
                goto L_vm_throw_exc;
            }
        }
        const char *pfx = chunk->str_consts[pfx_idx];
        size_t plen = strlen(pfx);
        char key_buf[256]; char *kptr = key_buf; char *heap_key = NULL; size_t klen = 0;
        if (VM_IS_INT(suffix)) {
            CONCAT_KEY_FORMAT_INT(pfx, plen, suffix, key_buf, kptr, heap_key, klen);
        } else {
            CONCAT_KEY_FORMAT_BYTES(pfx, plen, suffix, key_buf, kptr, heap_key, klen);
        }
        VMHash *h = VM_AS_HASH(hash);
        if (__builtin_expect(!VM_IS_UNDEF(h->tied_obj), 0)) {
            int sfidx = -1;
            if (VM_IS_PTR(h->tied_obj) && VM_PTR_TYPE(h->tied_obj) == VM_OBJ_HASH &&
                VM_AS_HASH(h->tied_obj)->class_name)
                sfidx = vm_find_method(vm->program, VM_AS_HASH(h->tied_obj)->class_name, "STORE");
            if (sfidx >= 0) {
                VMValue ksv = VM_MAKE_STR_N(kptr, klen);
                free(heap_key);
                vm_temp_free(&suffix);
                frame->ip = ip; vm->stack_top = sp;
                VMValue cargs[3] = { h->tied_obj, ksv, val };
                VMValue r = vm_execute_call(vm, sfidx, cargs, 3);
                vm_val_free(&r);
                stack = vm->stack; RELOAD();
                if (__builtin_expect(vm->has_pending_exc, 0)) {
                    throw_exc = vm->pending_exc;
                    vm->pending_exc = VM_UNDEF_VAL;
                    vm->has_pending_exc = 0;
                    goto L_vm_throw_exc;
                }
                DISPATCH();
            }
        }
        vm_hash_set_n(h, kptr, klen, val);
        free(heap_key);
        vm_temp_free(&suffix);
        DISPATCH();
    }

    CASE(OP_HASH_DEL_CONCAT): {
        uint16_t pfx_idx = read_u16(ip); ip += 2;
        VMValue suffix = SP_POP(), hash = SP_POP();
        if (!VM_IS_PTR(hash) || VM_PTR_TYPE(hash) != VM_OBJ_HASH) {
            vm_temp_free(&suffix);
            DISPATCH();
        }
        if (!VM_IS_INT(suffix) && __builtin_expect(vm_has_stringify(vm->program, suffix), 0)) {
            frame->ip = ip; SP_PUSH(hash); vm->stack_top = sp;
            suffix = vm_stringify_ov(vm, suffix);
            stack = vm->stack; RELOAD();
            hash = SP_POP();
            if (__builtin_expect(vm->has_pending_exc, 0)) {
                vm_temp_free(&suffix);
                throw_exc = vm->pending_exc;
                vm->pending_exc = VM_UNDEF_VAL;
                vm->has_pending_exc = 0;
                goto L_vm_throw_exc;
            }
        }
        const char *pfx = chunk->str_consts[pfx_idx];
        size_t plen = strlen(pfx);
        char key_buf[256]; char *kptr = key_buf; char *heap_key = NULL; size_t klen = 0;
        if (VM_IS_INT(suffix)) {
            CONCAT_KEY_FORMAT_INT(pfx, plen, suffix, key_buf, kptr, heap_key, klen);
        } else {
            CONCAT_KEY_FORMAT_BYTES(pfx, plen, suffix, key_buf, kptr, heap_key, klen);
        }
        VMHash *h = VM_AS_HASH(hash);
        if (__builtin_expect(!VM_IS_UNDEF(h->tied_obj), 0)) {
            int dfidx = -1;
            if (VM_IS_PTR(h->tied_obj) && VM_PTR_TYPE(h->tied_obj) == VM_OBJ_HASH &&
                VM_AS_HASH(h->tied_obj)->class_name)
                dfidx = vm_find_method(vm->program, VM_AS_HASH(h->tied_obj)->class_name, "DELETE");
            if (dfidx >= 0) {
                VMValue ksv = VM_MAKE_STR_N(kptr, klen);
                free(heap_key);
                vm_temp_free(&suffix);
                frame->ip = ip; vm->stack_top = sp;
                VMValue cargs[2] = { h->tied_obj, ksv };
                VMValue r = vm_execute_call(vm, dfidx, cargs, 2);
                vm_val_free(&r);
                stack = vm->stack; RELOAD();
                if (__builtin_expect(vm->has_pending_exc, 0)) {
                    throw_exc = vm->pending_exc;
                    vm->pending_exc = VM_UNDEF_VAL;
                    vm->has_pending_exc = 0;
                    goto L_vm_throw_exc;
                }
                DISPATCH();
            }
        }
        vm_hash_delete(h, kptr);
        free(heap_key);
        vm_temp_free(&suffix);
        DISPATCH();
    }

    #undef CONCAT_KEY_FORMAT_INT
    #undef CONCAT_KEY_FORMAT_BYTES

    /* ===== Closures ===== */
    CASE(OP_MAKE_CLOSURE): {
        uint16_t func_idx = read_u16(ip); ip += 2;
        uint8_t cap_count = *ip++;
        /* Read outer slot indices (u16 each, emitted by compiler after cap_count) */
        uint16_t slots_buf[64];
        uint16_t *outer_slots = (cap_count > 64) ? malloc((size_t)cap_count * sizeof(uint16_t)) : slots_buf;
        for (int i = 0; i < cap_count; i++) {
            outer_slots[i] = read_u16(ip); ip += 2;
        }
        VMClosure *cl = calloc(1, sizeof(VMClosure));
        vm_gc_init_hdr(&cl->hdr, VM_OBJ_CLOSURE);
        cl->func_idx = func_idx;
        cl->capture_count = cap_count;
        if (cap_count > 0) {
            cl->captures = calloc(cap_count, sizeof(VMValue));
            for (int i = cap_count - 1; i >= 0; i--) {
                VMValue raw = SP_POP();
                /* Check if already a cell (e.g., capturing from parent closure) */
                if (VM_IS_PTR(raw) && VM_PTR_TYPE(raw) == VM_OBJ_CELL) {
                    cl->captures[i] = raw;
                } else if (outer_slots[i] != 0xFFFF &&
                           VM_IS_PTR(locals[outer_slots[i]]) &&
                           VM_PTR_TYPE(locals[outer_slots[i]]) == VM_OBJ_CELL) {
                    cl->captures[i] = locals[outer_slots[i]];
                    vm_temp_free(&raw);
                } else {
                    /* Create a new cell for capture-by-reference */
                    VMCell *cell = calloc(1, sizeof(VMCell));
                    vm_gc_init_hdr(&cell->hdr, VM_OBJ_CELL);
                    cell->val = raw;
                    VMValue cell_val = VM_MAKE_PTR(cell);
                    cl->captures[i] = cell_val;
                    /* Store cell back into outer scope local so both share it */
                    if (outer_slots[i] != 0xFFFF) {
                        locals[outer_slots[i]] = cell_val;
                    }
                }
            }
        }
        if (outer_slots != slots_buf) free(outer_slots);
        SP_PUSH(VM_MAKE_PTR(cl));
        DISPATCH();
    }

    CASE(OP_LOAD_CAPTURE_CELL): {
        uint16_t idx = read_u16(ip); ip += 2;
        if (frame->closure && idx < (uint16_t)frame->closure->capture_count) {
            SP_PUSH(frame->closure->captures[idx]);
        } else {
            SP_PUSH(VM_UNDEF_VAL);
        }
        DISPATCH();
    }

    CASE(OP_LOAD_CAPTURE): {
        uint16_t idx = read_u16(ip); ip += 2;
        if (frame->closure && idx < (uint16_t)frame->closure->capture_count) {
            VMValue cv = frame->closure->captures[idx];
            /* Dereference cell for capture-by-reference */
            if (VM_IS_PTR(cv) && VM_PTR_TYPE(cv) == VM_OBJ_CELL) {
                VMValue v = VM_AS_CELL(cv)->val;
                SP_PUSH(vm_val_copy(v));
            } else {
                SP_PUSH(vm_val_copy(cv));
            }
        } else {
            SP_PUSH(VM_UNDEF_VAL);
        }
        DISPATCH();
    }

    CASE(OP_STORE_CAPTURE): {
        uint16_t idx = read_u16(ip); ip += 2;
        VMValue v = SP_POP();
        if (frame->closure && idx < (uint16_t)frame->closure->capture_count) {
            VMValue cv = frame->closure->captures[idx];
            /* Store through cell for capture-by-reference */
            if (VM_IS_PTR(cv) && VM_PTR_TYPE(cv) == VM_OBJ_CELL) {
                VM_AS_CELL(cv)->val = v;
            } else {
                frame->closure->captures[idx] = v;
            }
        }
        DISPATCH();
    }

    CASE(OP_CALL_CLOSURE): {
        clcall_argc = *ip++;
        clcall_val = SP_POP();
        goto L_do_op_call_closure;
    }

    CASE(OP_CALL_CLOSURE_SPREAD): {
        clcall_val = SP_POP();
        VMValue arr_val = SP_POP();
        clcall_argc = 0;
        if (VM_IS_PTR(arr_val) && VM_PTR_TYPE(arr_val) == VM_OBJ_ARRAY) {
            VMArray *aa = VM_AS_ARRAY(arr_val);
            clcall_argc = aa->size;
            for (int i = 0; i < clcall_argc; i++) SP_PUSH(aa->items[i]);
        }
        goto L_do_op_call_closure;
    }

    L_do_op_call_closure: {
        int argc = clcall_argc;
        VMValue closure_val = clcall_val;

        if (VM_IS_PTR(closure_val) && VM_PTR_TYPE(closure_val) == VM_OBJ_CLOSURE) {
            if (vm->recursion_limit > 0 && (int)vm->frame_count >= vm->recursion_limit) {
                fprintf(stderr, "Deep recursion detected (limit %d) — see core::set_recursion_limit\n", vm->recursion_limit);
                exit(1);
            }
            VMClosure *cl = VM_AS_CLOSURE(closure_val);
            VMChunk *callee = &vm->program->funcs[cl->func_idx];
            frame->ip = ip; vm->stack_top = sp;

            if (vm->frame_count >= vm->frame_cap) {
                vm->frame_cap *= 2;
                vm->frames = realloc(vm->frames, vm->frame_cap * sizeof(VMFrame));
            }
            VMFrame *nf = &vm->frames[vm->frame_count++];
            nf->chunk = callee; nf->ip = callee->code;
            nf->locals = locals_alloc(callee->local_count + 1, 0);
            nf->stack_base = sp - argc;
            nf->closure = cl;
            nf->global_save_depth = vm->global_save_count;

            if (__builtin_expect(callee->has_variadic, 0)) {
                int fixed = callee->fixed_param_count;
                int vc = argc - fixed;
                VMArray *va = vm_array_new(vc > 0 ? vc : 4);
                VMValue tmp_buf[256];
                VMValue *tmp_args = (argc > 256) ? malloc((size_t)argc * sizeof(VMValue)) : tmp_buf;
                for (int i = argc - 1; i >= 0; i--) tmp_args[i] = stack[--sp];
                for (int i = 0; i < fixed && i < argc; i++) {
                    if (i < callee->local_count + 1) nf->locals[i] = tmp_args[i];
                }
                for (int i = fixed; i < argc; i++) vm_array_push(va, tmp_args[i]);
                if (tmp_args != tmp_buf) free(tmp_args);
                if (fixed < callee->local_count + 1) nf->locals[fixed] = VM_MAKE_PTR(va);
            } else {
                for (int i = argc - 1; i >= 0; i--) {
                    VMValue av = stack[--sp];
                    if (i < callee->local_count + 1) nf->locals[i] = av;
                }
            }
            frame = nf; chunk = callee; ip = callee->code; locals = nf->locals;
            vm->stack_top = sp;
        } else if (VM_IS_PTR(closure_val) && VM_PTR_TYPE(closure_val) == VM_OBJ_HASH) {
            /* Might be a method call on an object (from around modifier $orig->($self)) */
            /* Not a closure, skip */
            for (int i = 0; i < argc; i++) SP_POP();
            SP_PUSH(VM_UNDEF_VAL);
        } else {
            for (int i = 0; i < argc; i++) SP_POP();
            SP_PUSH(VM_UNDEF_VAL);
        }
        DISPATCH();
    }

    /* ===== Regex ===== */
    CASE(OP_REGEX_MATCH): {
        uint16_t pat_idx = read_u16(ip); ip += 2;
        uint8_t flags = *ip++;
        (void)flags;
        VMValue str_val = SP_POP();
        char buf[256];
        const char *s = vm_to_cstr(str_val, buf, 256);
        const char *pattern = chunk->str_consts[pat_idx];
        int result = vm_regex_match(vm, s, pattern);
        SP_PUSH(VM_MAKE_INT(result));
        vm_val_free(&str_val);
        DISPATCH();
    }

    CASE(OP_REGEX_FIND): {
        uint16_t pat_idx = read_u16(ip); ip += 2;
        VMValue pos_val = SP_POP(), str_val = SP_POP();
        char fbuf[4096];
        const char *s = vm_to_cstr(str_val, fbuf, sizeof(fbuf));
        size_t mstart = 0, mend = 0;
        int64_t start = vm_to_int(pos_val);
        if (start < 0) start = 0;
        if (vm_regex_find(vm, s, chunk->str_consts[pat_idx], (size_t)start, &mstart, &mend)) {
            SP_PUSH(VM_MAKE_INT((int64_t)mstart));
            SP_PUSH(VM_MAKE_INT((int64_t)mend));
        } else {
            SP_PUSH(VM_MAKE_INT(-1));
            SP_PUSH(VM_MAKE_INT(-1));
        }
        vm_temp_free(&str_val);
        DISPATCH();
    }

    CASE(OP_REGEX_NOT_MATCH): {
        uint16_t pat_idx = read_u16(ip); ip += 2;
        uint8_t flags = *ip++;
        (void)flags;
        VMValue str_val = SP_POP();
        char buf[256];
        const char *s = vm_to_cstr(str_val, buf, 256);
        const char *pattern = chunk->str_consts[pat_idx];
        int result = vm_regex_match(vm, s, pattern);
        SP_PUSH(VM_MAKE_INT(!result));
        vm_val_free(&str_val);
        DISPATCH();
    }

    CASE(OP_LOAD_CAPTURE_VAR): {
        uint8_t num = *ip++;
        if (num < 10 && vm->regex_captures[num]) {
            SP_PUSH(vm_str(vm->regex_captures[num]));
        } else {
            SP_PUSH(vm_str(""));
        }
        DISPATCH();
    }

    /* ===== Try/Catch/Throw ===== */
    CASE(OP_TRY_BEGIN): {
        uint16_t catch_off = read_u16(ip); ip += 2;
        if (vm->exc_top >= vm->exc_cap) {
            vm->exc_cap *= 2;
            vm->exc_stack = realloc(vm->exc_stack, vm->exc_cap * sizeof(VMExcHandler));
        }
        VMExcHandler *eh = &vm->exc_stack[vm->exc_top++];
        eh->catch_ip = chunk->code + catch_off;
        eh->stack_base = sp;
        eh->frame_count = vm->frame_count;
        eh->global_save_depth = vm->global_save_count;
        DISPATCH();
    }

    CASE(OP_TRY_END): {
        if (vm->exc_top > 0) vm->exc_top--;
        DISPATCH();
    }

    CASE(OP_THROW): {
        throw_exc = SP_POP();
        goto L_vm_throw_exc;
    }

    L_vm_throw_exc: {
        if (vm->exc_top > 0) {
            VMExcHandler *eh = &vm->exc_stack[vm->exc_top - 1];
            if (vm->exec_base > 0 && eh->frame_count <= vm->exec_base) {
                size_t base_sp = vm->frames[vm->exec_base].stack_base;
                while (vm->frame_count > vm->exec_base) {
                    VMFrame *f = &vm->frames[vm->frame_count - 1];
                    vm_unwind_global_saves(vm, f->global_save_depth);
                    locals_free(f->locals, f->chunk->local_count + 1);
                    vm->frame_count--;
                }
                vm->has_pending_exc = 1;
                vm->pending_exc = throw_exc;
                vm->stack_top = base_sp;
                vm->exec_base = saved_exec_base;
                VM_EXEC_RETURN(VM_UNDEF_VAL);
            }
            vm->exc_top--;
            /* Unwind frames */
            while (vm->frame_count > eh->frame_count) {
                VMFrame *f = &vm->frames[vm->frame_count - 1];
                int lc = f->chunk->local_count;
                locals_free(f->locals, lc + 1);
                vm->frame_count--;
            }
            vm_unwind_global_saves(vm, eh->global_save_depth);
            sp = eh->stack_base;
            SP_PUSH(throw_exc);
            vm->stack_top = sp;
            RELOAD();
            ip = eh->catch_ip;
        } else if (vm->exec_base > 0) {
            size_t base_sp = vm->frames[vm->exec_base].stack_base;
            while (vm->frame_count > vm->exec_base) {
                VMFrame *f = &vm->frames[vm->frame_count - 1];
                vm_unwind_global_saves(vm, f->global_save_depth);
                locals_free(f->locals, f->chunk->local_count + 1);
                vm->frame_count--;
            }
            vm->has_pending_exc = 1;
            vm->pending_exc = throw_exc;
            vm->stack_top = base_sp;
            vm->exec_base = saved_exec_base;
            VM_EXEC_RETURN(VM_UNDEF_VAL);
        } else {
            while (vm->frame_count > saved_exec_base) {
                VMFrame *f = &vm->frames[vm->frame_count - 1];
                vm_unwind_global_saves(vm, f->global_save_depth);
                locals_free(f->locals, f->chunk->local_count + 1);
                vm->frame_count--;
            }
            vm_unwind_global_saves(vm, 0);
            vm->has_exception = 1;
            vm->current_exception = throw_exc;
            if (!vm->trap_uncaught) {
                char buf[256];
                fprintf(stderr, "Uncaught exception: %s\n", vm_to_cstr(throw_exc, buf, 256));
                vm->stack_top = 0;
                vm_run_end_blocks(vm);
            }
            vm->stack_top = 0;
            vm->exec_base = saved_exec_base;
            VM_EXEC_RETURN(VM_MAKE_INT(1));
        }
        DISPATCH();
    }

    /* ===== Globals ===== */
    CASE(OP_LOAD_GLOBAL): {
        uint16_t name_idx = read_u16(ip); ip += 2;
        const char *name = chunk->str_consts[name_idx];
        VMValue v = vm_hash_get(vm->globals, name);
        SP_PUSH(vm_val_copy(v));
        DISPATCH();
    }

    CASE(OP_STORE_GLOBAL): {
        uint16_t name_idx = read_u16(ip); ip += 2;
        const char *name = chunk->str_consts[name_idx];
        VMValue v = SP_POP();
        vm_hash_set(vm->globals, name, v);
        DISPATCH();
    }

    CASE(OP_SAVE_GLOBAL): {
        uint16_t name_idx = read_u16(ip); ip += 2;
        const char *name = chunk->str_consts[name_idx];
        if (vm->global_save_count >= vm->global_save_cap) {
            vm->global_save_cap *= 2;
            vm->global_saves = realloc(vm->global_saves, vm->global_save_cap * sizeof(VMGlobalSave));
        }
        VMGlobalSave *gs = &vm->global_saves[vm->global_save_count++];
        memset(gs, 0, sizeof(*gs));
        gs->name = strdup(name);
        gs->saved_value = vm_val_copy(vm_hash_get(vm->globals, name));
        DISPATCH();
    }

    CASE(OP_RESTORE_GLOBAL): {
        uint16_t name_idx = read_u16(ip); ip += 2;
        const char *name = chunk->str_consts[name_idx];
        /* Find most recent save for this name */
        for (int i = vm->global_save_count - 1; i >= 0; i--) {
            if (!vm->global_saves[i].is_hash_elem && vm->global_saves[i].name &&
                strcmp(vm->global_saves[i].name, name) == 0) {
                vm_hash_set(vm->globals, name, vm->global_saves[i].saved_value);
                free(vm->global_saves[i].name);
                /* Remove this entry */
                for (int j = i; j < vm->global_save_count - 1; j++)
                    vm->global_saves[j] = vm->global_saves[j + 1];
                vm->global_save_count--;
                break;
            }
        }
        DISPATCH();
    }

    CASE(OP_SAVE_HASH_ELEM): {
        VMValue key = SP_POP(), hval = SP_POP();
        if (vm->global_save_count >= vm->global_save_cap) {
            vm->global_save_cap *= 2;
            vm->global_saves = realloc(vm->global_saves, vm->global_save_cap * sizeof(VMGlobalSave));
        }
        VMGlobalSave *gs = &vm->global_saves[vm->global_save_count++];
        memset(gs, 0, sizeof(*gs));
        gs->is_hash_elem = 1;
        if (VM_IS_PTR(hval) && VM_PTR_TYPE(hval) == VM_OBJ_HASH) {
            char kbuf[256];
            const char *ks = vm_to_cstr(key, kbuf, sizeof(kbuf));
            gs->hash = VM_AS_HASH(hval);
            gs->hash_key = strdup(ks);
            gs->existed = vm_hash_exists(gs->hash, ks);
            gs->saved_value = gs->existed ? vm_val_copy(vm_hash_get(gs->hash, ks)) : VM_UNDEF_VAL;
        }
        vm_temp_free(&key);
        DISPATCH();
    }

    CASE(OP_RESTORE_HASH_ELEM): {
        for (int i = vm->global_save_count - 1; i >= 0; i--) {
            if (vm->global_saves[i].is_hash_elem) {
                VMGlobalSave *gs = &vm->global_saves[i];
                if (gs->hash && gs->hash_key) {
                    if (gs->existed) {
                        vm_hash_set(gs->hash, gs->hash_key, gs->saved_value);
                    } else {
                        vm_hash_delete(gs->hash, gs->hash_key);
                    }
                }
                free(gs->hash_key);
                for (int j = i; j < vm->global_save_count - 1; j++)
                    vm->global_saves[j] = vm->global_saves[j + 1];
                vm->global_save_count--;
                break;
            }
        }
        DISPATCH();
    }

    /* ===== Logic/Math ===== */
    CASE(OP_NEGATE): {
        VMValue v = SP_POP();
        if (__builtin_expect(VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_HASH && VM_AS_HASH(v)->class_name != NULL, 0)) {
            VMValue _ov_res = VM_UNDEF_VAL;
            frame->ip = ip; vm->stack_top = sp;
            if (vm_try_un_overload(vm, "neg", v, &_ov_res)) {
                stack = vm->stack; RELOAD();
                if (__builtin_expect(vm->has_pending_exc, 0)) {
                    throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                    goto L_vm_throw_exc;
                }
                vm_temp_free(&v);
                SP_PUSH(_ov_res);
                DISPATCH();
            }
        }
        if (VM_IS_INT(v)) SP_PUSH(VM_MAKE_INT((int64_t)(0ULL - (uint64_t)VM_INT_VAL(v))));
        else SP_PUSH(VM_MAKE_FLOAT(-vm_to_num(v)));
        vm_temp_free(&v);
        DISPATCH();
    }
    CASE(OP_NOT): {
        VMValue v = SP_POP();
        if (__builtin_expect(VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_HASH && VM_AS_HASH(v)->class_name != NULL, 0)) {
            VMValue _ov_res = VM_UNDEF_VAL;
            frame->ip = ip; vm->stack_top = sp;
            if (vm_try_un_overload(vm, "!", v, &_ov_res)) {
                stack = vm->stack; RELOAD();
                if (__builtin_expect(vm->has_pending_exc, 0)) {
                    throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                    goto L_vm_throw_exc;
                }
                vm_temp_free(&v);
                SP_PUSH(_ov_res);
                DISPATCH();
            }
        }
        SP_PUSH(VM_MAKE_INT(!vm_to_bool(v)));
        vm_temp_free(&v);
        DISPATCH();
    }
    CASE(OP_DEFINED): { VMValue v = SP_POP(); SP_PUSH(VM_MAKE_INT(!VM_IS_UNDEF(v))); vm_temp_free(&v); DISPATCH(); }
    CASE(OP_ABS): {
        VMValue v = SP_POP();
        if (VM_IS_INT(v)) {
            int64_t iv = VM_INT_VAL(v);
            SP_PUSH(VM_MAKE_INT(iv < 0 ? (int64_t)(0ULL - (uint64_t)iv) : iv));
        } else {
            SP_PUSH(VM_MAKE_FLOAT(fabs(vm_to_num(v))));
        }
        vm_temp_free(&v);
        DISPATCH();
    }
    CASE(OP_POWER): {
        VMValue b = SP_POP(), a = SP_POP();
        VM_TRY_BIN_OV("**");
        double r = pow(vm_to_num(a), vm_to_num(b));
        if (!vm_is_float_operand(a) && !vm_is_float_operand(b) && vm_to_int(b) >= 0 &&
            isfinite(r) && r >= -4.6e18 && r <= 4.6e18 && r == (double)(int64_t)r) {
            SP_PUSH(VM_MAKE_INT((int64_t)r));
        } else {
            SP_PUSH(VM_MAKE_FLOAT(r));
        }
        vm_temp_free(&a); vm_temp_free(&b);
        DISPATCH();
    }

    CASE(OP_SPRINTF): {
        uint8_t argc = *ip++;
        VMValue args_stk[32];
        VMValue *args = (argc > 32) ? malloc((size_t)argc * sizeof(VMValue)) : args_stk;
        for (int i = argc - 1; i >= 0; i--) {
            args[i] = SP_POP();
        }
        SP_PUSH(vm_sprintf(argc, args));
        for (int i = 0; i < argc; i++) {
            if (VM_IS_PTR(args[i]) && (VM_PTR_TYPE(args[i]) == VM_OBJ_STR || VM_PTR_TYPE(args[i]) == VM_OBJ_STRBUF))
                vm_val_free(&args[i]);
        }
        if (args != args_stk) free(args);
        DISPATCH();
    }

    CASE(OP_REF_TYPE): {
        VMValue v = SP_POP();
        if (VM_IS_PTR(v)) {
            switch (VM_PTR_TYPE(v)) {
            case VM_OBJ_ARRAY: SP_PUSH(vm_str("ARRAY")); break;
            case VM_OBJ_HASH: {
                VMHash *h = VM_AS_HASH(v);
                SP_PUSH(vm_str(h->class_name ? h->class_name : "HASH"));
                break;
            }
            case VM_OBJ_CLOSURE: SP_PUSH(vm_str("CODE")); break;
            case VM_OBJ_SREF: SP_PUSH(vm_str("SCALAR")); break;
            case VM_OBJ_FLOAT:
            case VM_OBJ_STR:
            case VM_OBJ_STRBUF:
                SP_PUSH(vm_str(""));
                break;
            default: SP_PUSH(vm_str("REF")); break;
            }
        } else {
            SP_PUSH(vm_str(""));
        }
        vm_temp_free(&v);
        DISPATCH();
    }

    CASE(OP_ADD_OV): {
        VMValue b = SP_POP(), a = SP_POP();
        VM_TRY_BIN_OV("+");
        SP_PUSH(VM_MAKE_INT(vm_to_int(a) + vm_to_int(b)));
        vm_temp_free(&a); vm_temp_free(&b);
        DISPATCH();
    }

    /* ===== foreach ===== */
    CASE(OP_FOREACH): {
        /* Never actually reached in current compilation — foreach is compiled
         * as a while loop with index variable */
        DISPATCH();
    }

    CASE(OP_LOOP_BREAK): {
        uint16_t target = read_u16(ip);
        ip = chunk->code + target;
        DISPATCH();
    }

    CASE(OP_LOOP_NEXT): {
        uint16_t target = read_u16(ip);
        ip = chunk->code + target;
        DISPATCH();
    }

    CASE(OP_LOOP_REDO): {
        uint16_t target = read_u16(ip);
        ip = chunk->code + target;
        DISPATCH();
    }

    /* ===== Builtins ===== */
    CASE(OP_BUILTIN): {
        uint16_t bid = read_u16(ip); ip += 2;
        uint8_t argc = *ip++;
        VMValue args_stk[16];
        VMValue *args = (argc > 16) ? malloc((size_t)argc * sizeof(VMValue)) : args_stk;
        for (int i = argc - 1; i >= 0; i--) {
            args[i] = SP_POP();
        }

        /* Generic runtime-bridged builtins (vm_generic_builtins.inc): call
         * the Strada runtime's uniform StradaValue* functions through the
         * vm_to_sv/sv_to_vm bridge. The compiler only emits these ids with
         * argc matching the table entry (vm_generic_builtin_find). */
        if (bid >= VM_GENERIC_BID_BASE) {
            int gidx = bid - VM_GENERIC_BID_BASE;
            if (gidx < VM_GENERIC_BUILTIN_COUNT &&
                argc == vm_generic_builtins[gidx].argc) {
                const VMGenericBuiltin *gb = &vm_generic_builtins[gidx];
                StradaValue *sa[4];
                for (int i = 0; i < argc; i++) sa[i] = vm_to_sv(args[i]);
                StradaValue *r = NULL;
                switch (argc) {
                case 0: r = ((VMGenericFn0)gb->fn)(); break;
                case 1: r = ((VMGenericFn1)gb->fn)(sa[0]); break;
                case 2: r = ((VMGenericFn2)gb->fn)(sa[0], sa[1]); break;
                case 3: r = ((VMGenericFn3)gb->fn)(sa[0], sa[1], sa[2]); break;
                case 4: r = ((VMGenericFn4)gb->fn)(sa[0], sa[1], sa[2], sa[3]); break;
                }
                SP_PUSH(sv_to_vm(r));
                if (r) strada_decref(r);
                for (int i = 0; i < argc; i++) strada_decref(sa[i]);
            } else {
                fprintf(stderr, "VM: bad generic builtin %d/%d\n", bid, argc);
                SP_PUSH(VM_UNDEF_VAL);
            }
            /* Free string temporaries (same rule as the switch epilogue) */
            for (int i = 0; i < argc; i++) {
                if (VM_IS_PTR(args[i]) && (VM_PTR_TYPE(args[i]) == VM_OBJ_STR || VM_PTR_TYPE(args[i]) == VM_OBJ_STRBUF))
                    vm_val_free(&args[i]);
            }
            if (args != args_stk) free(args);
            DISPATCH();
        }

        switch (bid) {
        case BUILTIN_CORE_OPEN: {
            if (argc >= 2) {
                char fbuf[256], mbuf[32];
                const char *path = vm_to_cstr(args[0], fbuf, 256);
                const char *mode = vm_to_cstr(args[1], mbuf, 32);
                /* Convert Perl-style modes */
                const char *cmode = mode;
                if (strcmp(mode, "<") == 0) cmode = "r";
                else if (strcmp(mode, ">") == 0) cmode = "w";
                else if (strcmp(mode, ">>") == 0) cmode = "a";
                FILE *fp = fopen(path, cmode);
                if (fp) {
                    VMFileHandle *fh = calloc(1, sizeof(VMFileHandle));
                    vm_gc_init_hdr(&fh->hdr, VM_OBJ_FILEHANDLE);
                    fh->fp = fp;
                    SP_PUSH(VM_MAKE_PTR(fh));
                } else {
                    SP_PUSH(VM_UNDEF_VAL);
                }
            } else SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_CORE_OPEN_STR: {
            if (argc >= 2) {
                char mbuf[32];
                const char *mode = vm_to_cstr(args[1], mbuf, sizeof(mbuf));
                if (strcmp(mode, "<") == 0 || strcmp(mode, "r") == 0 || strcmp(mode, "rb") == 0) {
                    char sbuf[256];
                    const char *s = vm_to_cstr(args[0], sbuf, sizeof(sbuf));
                    size_t slen = (VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_STR)
                                  ? VM_AS_STR(args[0])->len
                                  : (VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_STRBUF)
                                    ? VM_AS_STRBUF(args[0])->len
                                    : strlen(s);
                    VMFileHandle *fh = calloc(1, sizeof(VMFileHandle));
                    vm_gc_init_hdr(&fh->hdr, VM_OBJ_FILEHANDLE);
                    fh->is_mem = 1;
                    fh->mem_len = slen;
                    fh->mem_buf = malloc(slen > 0 ? slen : 1);
                    if (slen > 0) memcpy(fh->mem_buf, s, slen);
                    fh->fp = (slen == 0) ? fopen("/dev/null", "rb")
                                         : fmemopen(fh->mem_buf, slen, "rb");
                    if (fh->fp) {
                        SP_PUSH(VM_MAKE_PTR(fh));
                    } else {
                        SP_PUSH(VM_UNDEF_VAL);
                    }
                } else if (strcmp(mode, ">") == 0 || strcmp(mode, "w") == 0 || strcmp(mode, "wb") == 0) {
                    VMFileHandle *fh = calloc(1, sizeof(VMFileHandle));
                    vm_gc_init_hdr(&fh->hdr, VM_OBJ_FILEHANDLE);
                    fh->is_mem = 2;
                    fh->fp = open_memstream(&fh->mem_buf, &fh->mem_len);
                    if (fh->fp) {
                        SP_PUSH(VM_MAKE_PTR(fh));
                    } else {
                        SP_PUSH(VM_UNDEF_VAL);
                    }
                } else {
                    SP_PUSH(VM_UNDEF_VAL);
                }
            } else {
                SP_PUSH(VM_UNDEF_VAL);
            }
            break;
        }
        case BUILTIN_CORE_STR_FROM_FH: {
            if (argc >= 1 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_FILEHANDLE) {
                VMFileHandle *fh = (VMFileHandle*)VM_TO_PTR(args[0]);
                if (fh->is_mem == 2) {
                    if (fh->fp) fflush((FILE*)fh->fp);
                    SP_PUSH(VM_MAKE_STR_N(fh->mem_buf ? fh->mem_buf : "", fh->mem_len));
                } else {
                    SP_PUSH(VM_UNDEF_VAL);
                }
            } else {
                SP_PUSH(VM_UNDEF_VAL);
            }
            break;
        }
        case BUILTIN_CORE_CLOSE: {
            if (argc >= 1 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_FILEHANDLE) {
                VMFileHandle *fh = (VMFileHandle*)VM_TO_PTR(args[0]);
                if (fh->fp) { fclose((FILE*)fh->fp); fh->fp = NULL; }
            }
            SP_PUSH(VM_MAKE_INT(1));
            break;
        }
        case BUILTIN_CORE_READLINE: {
            FILE *fp = NULL;
            if (argc == 0) {
                fp = stdin;
            } else if (VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_FILEHANDLE) {
                VMFileHandle *fh = (VMFileHandle*)VM_TO_PTR(args[0]);
                fp = (FILE*)fh->fp;
            }
            int64_t max_bytes = (argc >= 2 && !VM_IS_UNDEF(args[1])) ? vm_to_int(args[1]) : 0;
            if (fp) {
                size_t cap = 256, len = 0;
                char *buf = malloc(cap);
                int c = EOF;
                while ((max_bytes <= 0 || (int64_t)len < max_bytes) && (c = fgetc(fp)) != EOF) {
                    if (len + 1 >= cap) {
                        cap *= 2;
                        buf = realloc(buf, cap);
                    }
                    buf[len++] = (char)c;
                    if (c == '\n') break;
                }
                if (len == 0 && c == EOF) {
                    free(buf);
                    SP_PUSH(VM_UNDEF_VAL);
                } else {
                    if (len > 0 && buf[len - 1] == '\n') {
                        len--;
                        if (len > 0 && buf[len - 1] == '\r') len--;
                    }
                    SP_PUSH(VM_MAKE_STR_N(buf, len));
                    free(buf);
                }
            } else {
                SP_PUSH(VM_UNDEF_VAL);
            }
            break;
        }
        case BUILTIN_CORE_EOF: {
            if (argc >= 1 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_FILEHANDLE) {
                VMFileHandle *fh = (VMFileHandle*)VM_TO_PTR(args[0]);
                if (!fh->fp || feof((FILE*)fh->fp)) {
                    SP_PUSH(VM_MAKE_INT(1));
                } else {
                    int c = fgetc((FILE*)fh->fp);
                    if (c == EOF) {
                        SP_PUSH(VM_MAKE_INT(1));
                    } else {
                        ungetc(c, (FILE*)fh->fp);
                        SP_PUSH(vm_str(""));
                    }
                }
            } else SP_PUSH(VM_MAKE_INT(1));
            break;
        }
        case BUILTIN_CORE_SEEK: {
            if (argc >= 3 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_FILEHANDLE) {
                VMFileHandle *fh = (VMFileHandle*)VM_TO_PTR(args[0]);
                if (fh->fp) fseek((FILE*)fh->fp, vm_to_int(args[1]), (int)vm_to_int(args[2]));
            }
            SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        case BUILTIN_CORE_TELL: {
            if (argc >= 1 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_FILEHANDLE) {
                VMFileHandle *fh = (VMFileHandle*)VM_TO_PTR(args[0]);
                SP_PUSH(VM_MAKE_INT(fh->fp ? ftell((FILE*)fh->fp) : 0));
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        case BUILTIN_CORE_REWIND: {
            if (argc >= 1 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_FILEHANDLE) {
                VMFileHandle *fh = (VMFileHandle*)VM_TO_PTR(args[0]);
                if (fh->fp) rewind((FILE*)fh->fp);
            }
            SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_CORE_FLUSH: {
            if (argc >= 1 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_FILEHANDLE) {
                VMFileHandle *fh = (VMFileHandle*)VM_TO_PTR(args[0]);
                if (fh->fp) fflush((FILE*)fh->fp);
            }
            SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_CORE_SLURP: {
            if (argc >= 1) {
                char fbuf[256];
                const char *path = vm_to_cstr(args[0], fbuf, 256);
                FILE *fp = fopen(path, "rb");
                if (fp) {
                    fseek(fp, 0, SEEK_END); long sz = ftell(fp); rewind(fp);
                    char *data = malloc(sz + 1);
                    size_t rd = fread(data, 1, sz, fp);
                    data[rd] = 0;
                    fclose(fp);
                    SP_PUSH(VM_MAKE_STR_N(data, rd));
                    free(data);
                } else SP_PUSH(vm_str(""));
            } else SP_PUSH(vm_str(""));
            break;
        }
        case BUILTIN_CORE_SPEW: {
            if (argc >= 2) {
                char fbuf[256], dbuf[64];
                size_t dlen = 0;
                const char *path = vm_to_cstr(args[0], fbuf, sizeof(fbuf));
                const char *data = vm_bytes_view(args[1], dbuf, sizeof(dbuf), &dlen);
                FILE *fp = fopen(path, "wb");
                if (fp) { fwrite(data, 1, dlen, fp); fclose(fp); }
            }
            SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_CORE_QX: {
            if (argc >= 1) {
                char cbuf[1024];
                const char *cmd = vm_to_cstr(args[0], cbuf, 1024);
                FILE *fp = popen(cmd, "r");
                if (fp) {
                    size_t cap = 4096, total = 0;
                    char *out = malloc(cap);
                    size_t n;
                    for (;;) {
                        if (total + 1024 >= cap) {
                            cap *= 2;
                            out = realloc(out, cap);
                        }
                        n = fread(out + total, 1, cap - total - 1, fp);
                        if (n == 0) break;
                        total += n;
                    }
                    out[total] = 0;
                    pclose(fp);
                    SP_PUSH(VM_MAKE_STR_N(out, total));
                    free(out);
                } else SP_PUSH(vm_str(""));
            } else SP_PUSH(vm_str(""));
            break;
        }
        case BUILTIN_CORE_SYSTEM: {
            if (argc >= 1) {
                char cbuf[1024];
                const char *cmd = vm_to_cstr(args[0], cbuf, 1024);
                int rc = system(cmd);
                SP_PUSH(VM_MAKE_INT(WEXITSTATUS(rc)));
            } else SP_PUSH(VM_MAKE_INT(-1));
            break;
        }
        case BUILTIN_CORE_GETENV: {
            if (argc >= 1) {
                char nbuf[256];
                const char *name = vm_to_cstr(args[0], nbuf, 256);
                char *val = getenv(name);
                SP_PUSH(val ? vm_str(val) : vm_str(""));
            } else SP_PUSH(vm_str(""));
            break;
        }
        case BUILTIN_CORE_SETENV: {
            if (argc >= 2) {
                char nbuf[256], vbuf[256];
                const char *name = vm_to_cstr(args[0], nbuf, 256);
                const char *val = vm_to_cstr(args[1], vbuf, 256);
                setenv(name, val, 1);
            }
            SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_CORE_TIME: {
            SP_PUSH(VM_MAKE_INT(time(NULL)));
            break;
        }
        case BUILTIN_CORE_HIRES_TIME: {
            struct timeval tv;
            gettimeofday(&tv, NULL);
            double t = (double)tv.tv_sec + (double)tv.tv_usec / 1e6;
            VM_PUSH_DOUBLE(sp, t);
            break;
        }
        case BUILTIN_CORE_GLOBAL_SET: {
            if (argc >= 2) {
                char kbuf[256];
                const char *key = vm_to_cstr(args[0], kbuf, 256);
                vm_hash_set(vm->globals, key, vm_val_copy(args[1]));
            }
            SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_CORE_GLOBAL_GET: {
            if (argc >= 1) {
                char kbuf[256];
                const char *key = vm_to_cstr(args[0], kbuf, 256);
                VMValue v = vm_hash_get(vm->globals, key);
                SP_PUSH(vm_val_copy(v));
            } else SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_CORE_GLOBAL_EXISTS: {
            if (argc >= 1) {
                char kbuf[256];
                const char *key = vm_to_cstr(args[0], kbuf, 256);
                SP_PUSH(VM_MAKE_INT(vm_hash_exists(vm->globals, key)));
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        case BUILTIN_CORE_GLOBAL_DELETE: {
            if (argc >= 1) {
                char kbuf[256];
                const char *key = vm_to_cstr(args[0], kbuf, 256);
                vm_hash_delete(vm->globals, key);
            }
            SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_CORE_GLOBAL_KEYS: {
            VMArray *keys = vm_hash_keys(vm->globals);
            SP_PUSH(VM_MAKE_PTR(keys));
            break;
        }
        case BUILTIN_CORE_SET_RECURSION_LIMIT: {
            vm->recursion_limit = argc >= 1 ? (int)vm_to_int(args[0]) : 1000;
            SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_CORE_GET_RECURSION_LIMIT: {
            SP_PUSH(VM_MAKE_INT(vm->recursion_limit));
            break;
        }
        case BUILTIN_CAPTURES: {
            /* captures() — [0]=full match, [1..]=groups from the last match */
            VMArray *ca = vm_array_new(10);
            int last = -1;
            for (int i = 0; i < 10; i++) if (vm->regex_captures[i]) last = i;
            for (int i = 0; i <= last; i++)
                vm_array_push(ca, vm->regex_captures[i] ? vm_str(vm->regex_captures[i]) : VM_UNDEF_VAL);
            SP_PUSH(VM_MAKE_PTR(ca));
            break;
        }
        case BUILTIN_NAMED_CAPTURES: {
            VMHash *nh = vm_hash_new(8);
            for (int i = 0; i < 10; i++) {
                if (vm->regex_capture_names[i] && vm->regex_captures[i])
                    vm_hash_set(nh, vm->regex_capture_names[i], vm_str(vm->regex_captures[i]));
            }
            SP_PUSH(VM_MAKE_PTR(nh));
            break;
        }
        case BUILTIN_INHERIT: {
            /* Runtime inherit(child, parent) — append an inheritance edge */
            if (argc >= 2) {
                char cbuf[128], pbuf[128];
                const char *child = vm_to_cstr(args[0], cbuf, 128);
                const char *parent = vm_to_cstr(args[1], pbuf, 128);
                VMProgram *p = vm->program;
                p->inherits = realloc(p->inherits, (p->inherit_count + 1) * sizeof(VMInherit));
                p->inherits[p->inherit_count].child = strdup(child);
                p->inherits[p->inherit_count].parent = strdup(parent);
                p->inherit_count++;
            }
            SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_MATH_SQRT: {
            VM_PUSH_DOUBLE(sp, sqrt(vm_to_num(args[0])));
            break;
        }
        case BUILTIN_MATH_FLOOR: {
            SP_PUSH(VM_MAKE_INT((int64_t)floor(vm_to_num(args[0]))));
            break;
        }
        case BUILTIN_MATH_CEIL: {
            SP_PUSH(VM_MAKE_INT((int64_t)ceil(vm_to_num(args[0]))));
            break;
        }
        case BUILTIN_MATH_POW: {
            VM_PUSH_DOUBLE(sp, pow(vm_to_num(args[0]), vm_to_num(args[1])));
            break;
        }
        case BUILTIN_SELECT: {
            /* select($fh) — set default output filehandle, return old */
            VMValue old_fh = vm->default_fh;
            if (argc >= 1) vm->default_fh = args[0];
            SP_PUSH(old_fh);
            break;
        }
        case BUILTIN_SIZE: {
            if (argc >= 1 && VM_IS_PTR(args[0])) {
                if (VM_PTR_TYPE(args[0]) == VM_OBJ_ARRAY)
                    SP_PUSH(VM_MAKE_INT(VM_AS_ARRAY(args[0])->size));
                else if (VM_PTR_TYPE(args[0]) == VM_OBJ_HASH)
                    SP_PUSH(VM_MAKE_INT(VM_AS_HASH(args[0])->size));
                else SP_PUSH(VM_MAKE_INT(0));
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        case BUILTIN_CHOMP_INPLACE: {
            /* chomp modifies variable in place — but since we use value semantics,
             * we just return the chomp'd string */
            if (argc >= 1) {
                char buf[256];
                const char *s = vm_to_cstr(args[0], buf, 256);
                size_t len = strlen(s);
                while (len > 0 && (s[len-1] == '\n' || s[len-1] == '\r')) len--;
                SP_PUSH(VM_MAKE_STR_N(s, len));
            } else SP_PUSH(vm_str(""));
            break;
        }
        case BUILTIN_MATH_ABS: {
            if (argc >= 1) {
                double v = vm_to_num(args[0]);
                double result = v < 0 ? -v : v;
                VM_PUSH_DOUBLE(sp, result);
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        case BUILTIN_MATH_SIN:  { VM_PUSH_DOUBLE(sp, sin(vm_to_num(args[0]))); break; }
        case BUILTIN_MATH_COS:  { VM_PUSH_DOUBLE(sp, cos(vm_to_num(args[0]))); break; }
        case BUILTIN_MATH_TAN:  { VM_PUSH_DOUBLE(sp, tan(vm_to_num(args[0]))); break; }
        case BUILTIN_MATH_ASIN: { VM_PUSH_DOUBLE(sp, asin(vm_to_num(args[0]))); break; }
        case BUILTIN_MATH_ACOS: { VM_PUSH_DOUBLE(sp, acos(vm_to_num(args[0]))); break; }
        case BUILTIN_MATH_ATAN: { VM_PUSH_DOUBLE(sp, atan(vm_to_num(args[0]))); break; }
        case BUILTIN_MATH_ATAN2: { VM_PUSH_DOUBLE(sp, atan2(vm_to_num(args[0]), vm_to_num(args[1]))); break; }
        case BUILTIN_MATH_SINH: { VM_PUSH_DOUBLE(sp, sinh(vm_to_num(args[0]))); break; }
        case BUILTIN_MATH_COSH: { VM_PUSH_DOUBLE(sp, cosh(vm_to_num(args[0]))); break; }
        case BUILTIN_MATH_TANH: { VM_PUSH_DOUBLE(sp, tanh(vm_to_num(args[0]))); break; }
        case BUILTIN_MATH_LOG:  { VM_PUSH_DOUBLE(sp, log(vm_to_num(args[0]))); break; }
        case BUILTIN_MATH_LOG10: { VM_PUSH_DOUBLE(sp, log10(vm_to_num(args[0]))); break; }
        case BUILTIN_MATH_EXP:  { VM_PUSH_DOUBLE(sp, exp(vm_to_num(args[0]))); break; }
        case BUILTIN_MATH_ROUND: { VM_PUSH_DOUBLE(sp, round(vm_to_num(args[0]))); break; }
        case BUILTIN_MATH_FMOD: { VM_PUSH_DOUBLE(sp, fmod(vm_to_num(args[0]), vm_to_num(args[1]))); break; }
        case BUILTIN_MATH_FABS: { VM_PUSH_DOUBLE(sp, fabs(vm_to_num(args[0]))); break; }
        case BUILTIN_MATH_RAND: { VM_PUSH_DOUBLE(sp, (double)rand() / (double)RAND_MAX); break; }
        case BUILTIN_MATH_SRAND: { srand((unsigned)vm_to_int(args[0])); SP_PUSH(VM_UNDEF_VAL); break; }
        case BUILTIN_MATH_HYPOT: { VM_PUSH_DOUBLE(sp, hypot(vm_to_num(args[0]), vm_to_num(args[1]))); break; }
        case BUILTIN_MATH_CBRT: { VM_PUSH_DOUBLE(sp, cbrt(vm_to_num(args[0]))); break; }
        case BUILTIN_MATH_ISNAN: { SP_PUSH(VM_MAKE_INT(isnan(vm_to_num(args[0])) ? 1 : 0)); break; }
        case BUILTIN_MATH_ISINF: { SP_PUSH(VM_MAKE_INT(isinf(vm_to_num(args[0])) ? 1 : 0)); break; }
        case BUILTIN_MATH_ISFINITE: { SP_PUSH(VM_MAKE_INT(isfinite(vm_to_num(args[0])) ? 1 : 0)); break; }
        case BUILTIN_UCFIRST: {
            if (argc >= 1) {
                char buf[256];
                const char *s = vm_to_cstr(args[0], buf, 256);
                size_t len = strlen(s);
                char *r = malloc(len + 1);
                memcpy(r, s, len + 1);
                if (len > 0 && r[0] >= 'a' && r[0] <= 'z') r[0] -= 32;
                SP_PUSH(vm_str(r));
                free(r);
            } else SP_PUSH(vm_str(""));
            break;
        }
        case BUILTIN_LCFIRST: {
            if (argc >= 1) {
                char buf[256];
                const char *s = vm_to_cstr(args[0], buf, 256);
                size_t len = strlen(s);
                char *r = malloc(len + 1);
                memcpy(r, s, len + 1);
                if (len > 0 && r[0] >= 'A' && r[0] <= 'Z') r[0] += 32;
                SP_PUSH(vm_str(r));
                free(r);
            } else SP_PUSH(vm_str(""));
            break;
        }
        case BUILTIN_TRIM: {
            if (argc >= 1) {
                char buf[1024];
                const char *s = vm_to_cstr(args[0], buf, 1024);
                while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
                size_t len = strlen(s);
                while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' || s[len-1] == '\n' || s[len-1] == '\r')) len--;
                SP_PUSH(VM_MAKE_STR_N(s, len));
            } else SP_PUSH(vm_str(""));
            break;
        }
        case BUILTIN_REPLACE: {
            if (argc >= 3) {
                char sbuf[4096], pbuf[256], rbuf[256];
                const char *s = vm_to_cstr(args[0], sbuf, 4096);
                const char *pat = vm_to_cstr(args[1], pbuf, 256);
                const char *repl = vm_to_cstr(args[2], rbuf, 256);
                /* Fast path: if pattern has no regex metacharacters, use plain strstr */
                int is_literal = 1;
                for (const char *p = pat; *p; p++) {
                    if (*p == '.' || *p == '*' || *p == '+' || *p == '?' || *p == '[' ||
                        *p == '(' || *p == '{' || *p == '\\' || *p == '|' || *p == '^' || *p == '$') {
                        is_literal = 0; break;
                    }
                }
                if (is_literal) {
                    /* Plain string replace (first occurrence) */
                    const char *f = strstr(s, pat);
                    if (f) {
                        size_t sl = strlen(s), pl = strlen(pat), rl = strlen(repl);
                        size_t before = f - s, nl = sl - pl + rl;
                        VMString *vs = malloc(sizeof(VMString) + nl + 1);
                        vm_gc_init_hdr(&vs->hdr, VM_OBJ_STR); vs->len = (uint32_t)nl;
                        memcpy(vs->data, s, before);
                        memcpy(vs->data + before, repl, rl);
                        memcpy(vs->data + before + rl, f + pl, sl - before - pl);
                        vs->data[nl] = 0;
                        SP_PUSH(VM_MAKE_PTR(vs));
                    } else SP_PUSH(vm_str(s));
                } else {
                    char *result = strada_regex_replace(s, pat, repl, "");
                    SP_PUSH(vm_str(result));
                    free(result);
                }
            } else SP_PUSH(vm_str(""));
            break;
        }
        case BUILTIN_REPLACE_ALL: {
            if (argc >= 3) {
                char sbuf[1024], fbuf[256], rbuf[256];
                const char *s = vm_to_cstr(args[0], sbuf, 1024);
                const char *find = vm_to_cstr(args[1], fbuf, 256);
                const char *repl = vm_to_cstr(args[2], rbuf, 256);
                /* Use regex replace for patterns, plain replace for simple strings */
                char *result = strada_regex_replace_all(s, find, repl, "g");
                SP_PUSH(vm_str(result));
                free(result);
            } else SP_PUSH(vm_str(""));
            break;
        }
        case BUILTIN_MATCH: {
            if (argc >= 2) {
                char sbuf[1024], pbuf[256];
                const char *s = vm_to_cstr(args[0], sbuf, 1024);
                const char *pat = vm_to_cstr(args[1], pbuf, 256);
                int matched = strada_regex_match_with_capture(s, pat, "");
                SP_PUSH(VM_MAKE_INT(matched));
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        case BUILTIN_SORT_DEFAULT: {
            if (argc >= 1 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_ARRAY) {
                VMArray *src = VM_AS_ARRAY(args[0]);
                /* Simple insertion sort by string comparison */
                VMArray *sorted = vm_array_new(src->size > 0 ? src->size : 8);
                for (int i = 0; i < src->size; i++) vm_array_push(sorted, vm_val_copy(src->items[i]));
                for (int i = 1; i < sorted->size; i++) {
                    VMValue key = sorted->items[i];
                    char kbuf[256]; const char *ks = vm_to_cstr(key, kbuf, 256);
                    int j = i - 1;
                    while (j >= 0) {
                        char jbuf[256]; const char *js = vm_to_cstr(sorted->items[j], jbuf, 256);
                        if (strcmp(js, ks) <= 0) break;
                        sorted->items[j+1] = sorted->items[j];
                        j--;
                    }
                    sorted->items[j+1] = key;
                }
                SP_PUSH(VM_MAKE_PTR(sorted));
            } else SP_PUSH(VM_MAKE_PTR(vm_array_new(8)));
            break;
        }
        case BUILTIN_CORE_FILE_EXISTS: {
            if (argc >= 1) {
                char buf[512];
                const char *path = vm_to_cstr(args[0], buf, 512);
                SP_PUSH(VM_MAKE_INT(access(path, F_OK) == 0));
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        case BUILTIN_CORE_UNLINK: {
            if (argc >= 1) {
                char buf[512];
                const char *path = vm_to_cstr(args[0], buf, 512);
                SP_PUSH(VM_MAKE_INT(remove(path) == 0));
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        case BUILTIN_HEX: {
            if (argc >= 1) {
                char buf[64];
                const char *s = vm_to_cstr(args[0], buf, 64);
                SP_PUSH(VM_MAKE_INT((int64_t)strtol(s, NULL, 16)));
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        case BUILTIN_OCT: {
            if (argc >= 1) {
                char buf[64];
                const char *s = vm_to_cstr(args[0], buf, 64);
                SP_PUSH(VM_MAKE_INT((int64_t)strtol(s, NULL, 8)));
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        case BUILTIN_FILE_TEST_D: {
            if (argc >= 1) {
                char buf[512];
                const char *path = vm_to_cstr(args[0], buf, 512);
                struct stat st;
                SP_PUSH(VM_MAKE_INT(stat(path, &st) == 0 && S_ISDIR(st.st_mode)));
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        case BUILTIN_RANGE: {
            if (argc >= 2) {
                int64_t from = vm_to_int(args[0]);
                int64_t to = vm_to_int(args[1]);
                int64_t count = to >= from ? to - from + 1 : 0;
                VMArray *arr = vm_array_new(count > 0 ? (int)count : 8);
                for (int64_t i = from; i <= to; i++) {
                    vm_array_push(arr, VM_MAKE_INT(i));
                }
                SP_PUSH(VM_MAKE_PTR(arr));
            } else SP_PUSH(VM_MAKE_PTR(vm_array_new(8)));
            break;
        }
        case BUILTIN_FILE_TEST_F: {
            if (argc >= 1) {
                char buf[512];
                const char *path = vm_to_cstr(args[0], buf, 512);
                struct stat st;
                SP_PUSH(VM_MAKE_INT(stat(path, &st) == 0 && S_ISREG(st.st_mode)));
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        /* === String builtins === */
        case BUILTIN_STR_RINDEX: {
            if (argc >= 2) {
                char hbuf[128], nbuf[128];
                const char *haystack = vm_to_cstr(args[0], hbuf, sizeof(hbuf));
                const char *needle = vm_to_cstr(args[1], nbuf, sizeof(nbuf));
                int64_t pos = (argc >= 3)
                    ? strada_rindex_from(haystack, needle, vm_to_int(args[2]))
                    : strada_rindex(haystack, needle);
                SP_PUSH(VM_MAKE_INT(pos));
            } else {
                SP_PUSH(VM_MAKE_INT(-1));
            }
            break;
        }
        case BUILTIN_STR_LTRIM: {
            if (argc >= 1) {
                char buf[1024];
                const char *s = vm_to_cstr(args[0], buf, 1024);
                while (*s == ' ' || *s == '\t' || *s == '\n' || *s == '\r') s++;
                SP_PUSH(vm_str(s));
            } else SP_PUSH(vm_str(""));
            break;
        }
        case BUILTIN_STR_RTRIM: {
            if (argc >= 1) {
                char buf[1024];
                const char *s = vm_to_cstr(args[0], buf, 1024);
                size_t len = strlen(s);
                while (len > 0 && (s[len-1] == ' ' || s[len-1] == '\t' || s[len-1] == '\n' || s[len-1] == '\r')) len--;
                SP_PUSH(VM_MAKE_STR_N(s, len));
            } else SP_PUSH(vm_str(""));
            break;
        }
        case BUILTIN_STR_CHOP: {
            if (argc >= 1) {
                char buf[1024];
                const char *s = vm_to_cstr(args[0], buf, 1024);
                size_t len = strlen(s);
                if (len > 0) SP_PUSH(VM_MAKE_STR_N(s, len - 1));
                else SP_PUSH(vm_str(""));
            } else SP_PUSH(vm_str(""));
            break;
        }
        case BUILTIN_STR_REVERSE: {
            if (argc >= 1) {
                if (VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_ARRAY) {
                    /* Array reverse */
                    VMArray *src = VM_AS_ARRAY(args[0]);
                    VMArray *rev = vm_array_new(src->size > 0 ? src->size : 8);
                    for (int i = src->size - 1; i >= 0; i--)
                        vm_array_push(rev, vm_val_copy(src->items[i]));
                    SP_PUSH(VM_MAKE_PTR(rev));
                } else {
                    /* String reverse */
                    char buf[1024];
                    const char *s = vm_to_cstr(args[0], buf, 1024);
                    size_t len = strlen(s);
                    char *r = malloc(len + 1);
                    for (size_t i = 0; i < len; i++) r[i] = s[len - 1 - i];
                    r[len] = '\0';
                    SP_PUSH(vm_str(r));
                    free(r);
                }
            } else SP_PUSH(vm_str(""));
            break;
        }

        /* === Hash/Array builtins === */
        case BUILTIN_HASH_VALUES: {
            if (argc >= 1 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_HASH) {
                VMHash *h = VM_AS_HASH(args[0]);
                VMArray *vals = vm_array_new(h->size > 0 ? h->size : 8);
                for (int i = 0; i < h->capacity; i++) {
                    if (h->entries[i].occupied == 1) {
                        vm_array_push(vals, vm_val_copy(h->entries[i].value));
                    }
                }
                SP_PUSH(VM_MAKE_PTR(vals));
            } else SP_PUSH(VM_MAKE_PTR(vm_array_new(8)));
            break;
        }
        case BUILTIN_ARRAY_SPLICE: {
            if (argc >= 1 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_ARRAY) {
                VMArray *arr = VM_AS_ARRAY(args[0]);
                int64_t offset = (argc >= 2) ? vm_to_int(args[1]) : 0;
                if (offset < 0) offset = arr->size + offset;
                if (offset < 0) offset = 0;
                if (offset > arr->size) offset = arr->size;
                int64_t length = (argc >= 3) ? vm_to_int(args[2]) : arr->size - offset;
                if (length < 0) {
                    length = (arr->size - offset) + length;
                    if (length < 0) length = 0;
                }
                if (offset + length > arr->size) length = arr->size - offset;
                /* Collect removed elements */
                VMArray *removed = vm_array_new(length > 0 ? (int)length : 8);
                for (int64_t i = 0; i < length; i++)
                    vm_array_push(removed, arr->items[offset + i]);
                /* Collect replacement elements */
                int repl_count = argc > 3 ? argc - 3 : 0;
                /* Shift elements */
                int64_t diff = repl_count - length;
                if (diff > 0) {
                    /* Grow */
                    while (arr->size + diff > arr->cap) {
                        arr->cap *= 2;
                        arr->items = realloc(arr->items, arr->cap * sizeof(VMValue));
                    }
                    memmove(&arr->items[offset + repl_count], &arr->items[offset + length],
                            (arr->size - offset - length) * sizeof(VMValue));
                } else if (diff < 0) {
                    memmove(&arr->items[offset + repl_count], &arr->items[offset + length],
                            (arr->size - offset - length) * sizeof(VMValue));
                }
                arr->size += (int)diff;
                /* Insert replacements */
                for (int i = 0; i < repl_count; i++)
                    arr->items[offset + i] = vm_val_copy(args[3 + i]);
                SP_PUSH(VM_MAKE_PTR(removed));
            } else SP_PUSH(VM_MAKE_PTR(vm_array_new(8)));
            break;
        }
        case BUILTIN_HASH_EACH: {
            if (argc >= 1 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_HASH) {
                VMHash *h = VM_AS_HASH(args[0]);
                /* Find next occupied entry starting from iter_pos */
                while (h->iter_pos < h->capacity && h->entries[h->iter_pos].occupied != 1)
                    h->iter_pos++;
                if (h->iter_pos < h->capacity) {
                    VMArray *pair = vm_array_new(2);
                    vm_array_push(pair, vm_str(h->entries[h->iter_pos].key));
                    vm_array_push(pair, vm_val_copy(h->entries[h->iter_pos].value));
                    h->iter_pos++;
                    SP_PUSH(VM_MAKE_PTR(pair));
                } else {
                    h->iter_pos = 0; /* reset for next iteration */
                    SP_PUSH(VM_MAKE_PTR(vm_array_new(0)));
                }
            } else SP_PUSH(VM_MAKE_PTR(vm_array_new(0)));
            break;
        }

        /* === Core builtins === */
        case BUILTIN_CORE_EXIT: {
            int code = argc >= 1 ? (int)vm_to_int(args[0]) : 0;
            vm_run_end_blocks(vm);
            exit(code);
            break;
        }
        case BUILTIN_CORE_SLEEP: {
            if (argc >= 1) sleep((unsigned)vm_to_int(args[0]));
            SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_CORE_USLEEP: {
            if (argc >= 1) usleep((useconds_t)vm_to_int(args[0]));
            SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_CORE_GETPID: {
            SP_PUSH(VM_MAKE_INT(getpid()));
            break;
        }
        case BUILTIN_CORE_GETPPID: {
            SP_PUSH(VM_MAKE_INT(getppid()));
            break;
        }
        case BUILTIN_CORE_CHDIR: {
            if (argc >= 1) {
                char buf[512];
                SP_PUSH(VM_MAKE_INT(chdir(vm_to_cstr(args[0], buf, 512)) == 0 ? 1 : 0));
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        case BUILTIN_CORE_GETCWD: {
            char buf[4096];
            char *cwd = getcwd(buf, sizeof(buf));
            SP_PUSH(cwd ? vm_str(cwd) : VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_CORE_MKDIR: {
            if (argc >= 1) {
                char buf[512];
                int mode = argc >= 2 ? (int)vm_to_int(args[1]) : 0755;
                SP_PUSH(VM_MAKE_INT(mkdir(vm_to_cstr(args[0], buf, 512), mode) == 0 ? 1 : 0));
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        case BUILTIN_CORE_RMDIR: {
            if (argc >= 1) {
                char buf[512];
                SP_PUSH(VM_MAKE_INT(rmdir(vm_to_cstr(args[0], buf, 512)) == 0 ? 1 : 0));
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        case BUILTIN_CORE_RENAME: {
            if (argc >= 2) {
                char obuf[512], nbuf[512];
                SP_PUSH(VM_MAKE_INT(rename(vm_to_cstr(args[0], obuf, 512), vm_to_cstr(args[1], nbuf, 512)) == 0 ? 1 : 0));
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        case BUILTIN_CORE_DIRNAME: {
            if (argc >= 1) {
                char buf[512];
                const char *p = vm_to_cstr(args[0], buf, 512);
                char *tmp = strdup(p);
                char *d = dirname(tmp);
                SP_PUSH(vm_str(d));
                free(tmp);
            } else SP_PUSH(vm_str("."));
            break;
        }
        case BUILTIN_CORE_BASENAME: {
            if (argc >= 1) {
                char buf[512];
                const char *p = vm_to_cstr(args[0], buf, 512);
                char *tmp = strdup(p);
                char *b = basename(tmp);
                SP_PUSH(vm_str(b));
                free(tmp);
            } else SP_PUSH(vm_str(""));
            break;
        }
        case BUILTIN_CORE_STAT:
        case BUILTIN_CORE_LSTAT: {
            if (argc >= 1) {
                char buf[512];
                const char *path = vm_to_cstr(args[0], buf, 512);
                struct stat st;
                int rc = (bid == BUILTIN_CORE_LSTAT) ? lstat(path, &st) : stat(path, &st);
                if (rc == 0) {
                    VMHash *h = vm_hash_new(16);
                    vm_hash_set(h, "dev", VM_MAKE_INT(st.st_dev));
                    vm_hash_set(h, "ino", VM_MAKE_INT(st.st_ino));
                    vm_hash_set(h, "mode", VM_MAKE_INT(st.st_mode));
                    vm_hash_set(h, "nlink", VM_MAKE_INT(st.st_nlink));
                    vm_hash_set(h, "uid", VM_MAKE_INT(st.st_uid));
                    vm_hash_set(h, "gid", VM_MAKE_INT(st.st_gid));
                    vm_hash_set(h, "size", VM_MAKE_INT(st.st_size));
                    vm_hash_set(h, "atime", VM_MAKE_INT(st.st_atime));
                    vm_hash_set(h, "mtime", VM_MAKE_INT(st.st_mtime));
                    vm_hash_set(h, "ctime", VM_MAKE_INT(st.st_ctime));
                    SP_PUSH(VM_MAKE_PTR(h));
                } else SP_PUSH(VM_UNDEF_VAL);
            } else SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_CORE_CHMOD: {
            if (argc >= 2) {
                char buf[512];
                SP_PUSH(VM_MAKE_INT(chmod(vm_to_cstr(args[0], buf, 512), (mode_t)vm_to_int(args[1])) == 0 ? 1 : 0));
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        case BUILTIN_CORE_READDIR: {
            if (argc >= 1) {
                char buf[512];
                const char *path = vm_to_cstr(args[0], buf, 512);
                DIR *d = opendir(path);
                VMArray *entries = vm_array_new(32);
                if (d) {
                    struct dirent *ent;
                    while ((ent = readdir(d)) != NULL) {
                        if (strcmp(ent->d_name, ".") != 0 && strcmp(ent->d_name, "..") != 0)
                            vm_array_push(entries, vm_str(ent->d_name));
                    }
                    closedir(d);
                }
                SP_PUSH(VM_MAKE_PTR(entries));
            } else SP_PUSH(VM_MAKE_PTR(vm_array_new(8)));
            break;
        }
        case BUILTIN_CORE_POPEN: {
            if (argc >= 2) {
                char cbuf[1024], mbuf[16];
                const char *cmd = vm_to_cstr(args[0], cbuf, 1024);
                const char *mode = vm_to_cstr(args[1], mbuf, 16);
                FILE *fp = popen(cmd, mode);
                if (fp) {
                    VMFileHandle *fh = calloc(1, sizeof(VMFileHandle));
                    vm_gc_init_hdr(&fh->hdr, VM_OBJ_FILEHANDLE);
                    fh->fp = fp;
                    fh->is_popen = 1;
                    SP_PUSH(VM_MAKE_PTR(fh));
                } else SP_PUSH(VM_UNDEF_VAL);
            } else SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_CORE_PCLOSE: {
            if (argc >= 1 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_FILEHANDLE) {
                VMFileHandle *fh = (VMFileHandle*)VM_TO_PTR(args[0]);
                int status = pclose(fh->fp);
                fh->fp = NULL;
                SP_PUSH(VM_MAKE_INT(WEXITSTATUS(status)));
            } else SP_PUSH(VM_MAKE_INT(-1));
            break;
        }
        case BUILTIN_CORE_ERRNO: {
            SP_PUSH(VM_MAKE_INT(errno));
            break;
        }
        case BUILTIN_CORE_STRERROR: {
            int errnum = argc >= 1 ? (int)vm_to_int(args[0]) : errno;
            SP_PUSH(vm_str(strerror(errnum)));
            break;
        }
        case BUILTIN_CORE_ISATTY: {
            int fd = argc >= 1 ? (int)vm_to_int(args[0]) : 0;
            SP_PUSH(VM_MAKE_INT(isatty(fd)));
            break;
        }
        case BUILTIN_CORE_ARGV: {
            /* Return the stored argv array */
            if (vm->argv) {
                SP_PUSH(VM_MAKE_PTR(vm->argv));
            } else {
                SP_PUSH(VM_MAKE_PTR(vm_array_new(0)));
            }
            break;
        }
        case BUILTIN_CORE_REALPATH: {
            if (argc >= 1) {
                char buf[512];
                char resolved[PATH_MAX];
                char *rp = realpath(vm_to_cstr(args[0], buf, 512), resolved);
                SP_PUSH(rp ? vm_str(rp) : VM_UNDEF_VAL);
            } else SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_CORE_LINK: {
            if (argc >= 2) {
                char obuf[512], nbuf[512];
                SP_PUSH(VM_MAKE_INT(link(vm_to_cstr(args[0], obuf, 512), vm_to_cstr(args[1], nbuf, 512)) == 0 ? 1 : 0));
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        case BUILTIN_CORE_SYMLINK: {
            if (argc >= 2) {
                char obuf[512], nbuf[512];
                SP_PUSH(VM_MAKE_INT(symlink(vm_to_cstr(args[0], obuf, 512), vm_to_cstr(args[1], nbuf, 512)) == 0 ? 1 : 0));
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        case BUILTIN_CORE_READLINK: {
            if (argc >= 1) {
                char buf[512], rbuf[4096];
                ssize_t len = readlink(vm_to_cstr(args[0], buf, 512), rbuf, sizeof(rbuf) - 1);
                if (len >= 0) { rbuf[len] = '\0'; SP_PUSH(vm_str(rbuf)); }
                else SP_PUSH(VM_UNDEF_VAL);
            } else SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_CORE_FORK: {
            SP_PUSH(VM_MAKE_INT(fork()));
            break;
        }
        case BUILTIN_CORE_WAIT: {
            int status;
            pid_t pid = wait(&status);
            SP_PUSH(VM_MAKE_INT(pid));
            break;
        }
        case BUILTIN_CORE_WAITPID: {
            if (argc >= 2) {
                int status;
                pid_t pid = waitpid((pid_t)vm_to_int(args[0]), &status, (int)vm_to_int(args[1]));
                SP_PUSH(VM_MAKE_INT(pid));
            } else SP_PUSH(VM_MAKE_INT(-1));
            break;
        }
        case BUILTIN_CORE_KILL: {
            if (argc >= 2) {
                SP_PUSH(VM_MAKE_INT(kill((pid_t)vm_to_int(args[0]), (int)vm_to_int(args[1]))));
            } else SP_PUSH(VM_MAKE_INT(-1));
            break;
        }
        case BUILTIN_CORE_SIGNAL: {
            /* Simplified: just set SIG_IGN or SIG_DFL */
            if (argc >= 2) {
                int sig = (int)vm_to_int(args[0]);
                char buf[64];
                const char *action = vm_to_cstr(args[1], buf, 64);
                if (strcmp(action, "IGNORE") == 0 || strcmp(action, "SIG_IGN") == 0)
                    signal(sig, SIG_IGN);
                else if (strcmp(action, "DEFAULT") == 0 || strcmp(action, "SIG_DFL") == 0)
                    signal(sig, SIG_DFL);
                SP_PUSH(VM_UNDEF_VAL);
            } else SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_CORE_ALARM: {
            if (argc >= 1) {
                SP_PUSH(VM_MAKE_INT(alarm((unsigned)vm_to_int(args[0]))));
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        case BUILTIN_CORE_PIPE: {
            int fds[2];
            if (pipe(fds) == 0) {
                VMArray *arr = vm_array_new(2);
                vm_array_push(arr, VM_MAKE_INT(fds[0]));
                vm_array_push(arr, VM_MAKE_INT(fds[1]));
                SP_PUSH(VM_MAKE_PTR(arr));
            } else SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_CORE_DUP2: {
            if (argc >= 2) {
                SP_PUSH(VM_MAKE_INT(dup2((int)vm_to_int(args[0]), (int)vm_to_int(args[1]))));
            } else SP_PUSH(VM_MAKE_INT(-1));
            break;
        }
        case BUILTIN_CORE_ORD_BYTE: {
            if (argc >= 1) {
                if (VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_STR) {
                    VMString *vs = VM_AS_STR(args[0]);
                    SP_PUSH(VM_MAKE_INT(vs->len > 0 ? (unsigned char)vs->data[0] : 0));
                } else if (VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_STRBUF) {
                    VMStrBuf *sb = VM_AS_STRBUF(args[0]);
                    SP_PUSH(VM_MAKE_INT(sb->len > 0 ? (unsigned char)sb->data[0] : 0));
                } else {
                    char buf[256];
                    const char *s = vm_to_cstr(args[0], buf, 256);
                    SP_PUSH(VM_MAKE_INT(s[0] ? (unsigned char)s[0] : 0));
                }
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        case BUILTIN_CORE_BYTE: {
            if (argc >= 1) {
                char b = (char)(unsigned char)(vm_to_int(args[0]) & 0xFF);
                SP_PUSH(VM_MAKE_STR_N(&b, 1));
            } else SP_PUSH(VM_MAKE_STR_N("", 0));
            break;
        }
        case BUILTIN_CORE_GET_BYTE: {
            if (argc >= 2) {
                char buf[1024];
                const char *s;
                size_t len;
                if (VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_STR) {
                    s = VM_AS_STR(args[0])->data;
                    len = VM_AS_STR(args[0])->len;
                } else if (VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_STRBUF) {
                    s = VM_AS_STRBUF(args[0])->data;
                    len = VM_AS_STRBUF(args[0])->len;
                } else {
                    s = vm_to_cstr(args[0], buf, sizeof(buf));
                    len = strlen(s);
                }
                int64_t pos = vm_to_int(args[1]);
                if (pos >= 0 && (size_t)pos < len) SP_PUSH(VM_MAKE_INT((unsigned char)s[pos]));
                else SP_PUSH(VM_MAKE_INT(-1));
            } else SP_PUSH(VM_MAKE_INT(-1));
            break;
        }
        case BUILTIN_CORE_BYTE_LENGTH: {
            if (argc >= 1) {
                size_t len;
                if (VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_STR) {
                    len = VM_AS_STR(args[0])->len;
                } else if (VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_STRBUF) {
                    len = VM_AS_STRBUF(args[0])->len;
                } else {
                    char buf[1024];
                    const char *s = vm_to_cstr(args[0], buf, sizeof(buf));
                    len = strlen(s);
                }
                SP_PUSH(VM_MAKE_INT((int64_t)len));
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        case BUILTIN_CORE_CALLER: {
            /* Return a hash with function, file, line from the calling frame */
            VMHash *info = vm_hash_new(8);
            if (vm->frame_count >= 2) {
                VMFrame *cf = &vm->frames[vm->frame_count - 2];
                if (cf->chunk->name) vm_hash_set(info, "function", vm_str(cf->chunk->name));
                else vm_hash_set(info, "function", vm_str("(unknown)"));
                vm_hash_set(info, "file", vm_str("(unknown)"));
                vm_hash_set(info, "line", VM_MAKE_INT(0));
            }
            SP_PUSH(VM_MAKE_PTR(info));
            break;
        }
        case BUILTIN_CORE_STACK_TRACE: {
            /* Build stack trace string */
            size_t cap = 1024;
            char *trace = malloc(cap);
            size_t pos = 0;
            for (int i = vm->frame_count - 1; i >= 0; i--) {
                VMFrame *f = &vm->frames[i];
                const char *fname = f->chunk->name ? f->chunk->name : "(unknown)";
                int n = snprintf(trace + pos, cap - pos, "  %s\n", fname);
                if (n > 0) pos += n;
                if (pos >= cap - 64) { cap *= 2; trace = realloc(trace, cap); }
            }
            SP_PUSH(vm_str(trace));
            free(trace);
            break;
        }
        case BUILTIN_CORE_PACK: {
            if (argc >= 1) {
                char fbuf[256];
                const char *fmt = vm_to_cstr(args[0], fbuf, sizeof(fbuf));
                StradaValue *pk_arr = strada_new_array();
                for (int i = 1; i < argc; i++) {
                    strada_array_push_take(pk_arr->value.av, vm_to_sv(args[i]));
                }
                StradaValue *packed = strada_pack(fmt, pk_arr);
                VMValue vm_res = sv_to_vm(packed);
                strada_decref(pk_arr);
                strada_decref(packed);
                SP_PUSH(vm_res);
            } else SP_PUSH(vm_str(""));
            break;
        }
        case BUILTIN_CORE_UNPACK: {
            if (argc >= 2) {
                char fbuf[256];
                const char *fmt = vm_to_cstr(args[0], fbuf, sizeof(fbuf));
                StradaValue *data_sv = vm_to_sv(args[1]);
                StradaValue *unpacked = strada_unpack(fmt, data_sv);
                VMValue vm_res = sv_to_vm(unpacked);
                strada_decref(data_sv);
                strada_decref(unpacked);
                SP_PUSH(vm_res);
            } else SP_PUSH(VM_MAKE_PTR(vm_array_new(0)));
            break;
        }
        case BUILTIN_CORE_BASE64_ENCODE: {
            if (argc >= 1) {
                char buf[4096];
                const char *data = vm_to_cstr(args[0], buf, 4096);
                size_t dlen = (VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_STR) ? VM_AS_STR(args[0])->len :
                              (VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_STRBUF) ? VM_AS_STRBUF(args[0])->len : strlen(data);
                static const char b64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
                size_t olen = 4 * ((dlen + 2) / 3);
                char *out = malloc(olen + 1);
                size_t j = 0;
                for (size_t i = 0; i < dlen; i += 3) {
                    uint32_t n = ((unsigned char)data[i]) << 16;
                    if (i+1 < dlen) n |= ((unsigned char)data[i+1]) << 8;
                    if (i+2 < dlen) n |= (unsigned char)data[i+2];
                    out[j++] = b64[(n >> 18) & 0x3F];
                    out[j++] = b64[(n >> 12) & 0x3F];
                    out[j++] = (i+1 < dlen) ? b64[(n >> 6) & 0x3F] : '=';
                    out[j++] = (i+2 < dlen) ? b64[n & 0x3F] : '=';
                }
                out[j] = '\0';
                SP_PUSH(vm_str(out));
                free(out);
            } else SP_PUSH(vm_str(""));
            break;
        }
        case BUILTIN_CORE_BASE64_DECODE: {
            if (argc >= 1) {
                char buf[4096];
                const char *data = vm_to_cstr(args[0], buf, 4096);
                size_t dlen = strlen(data);
                /* Size the output for whole 4-char groups only, with slack, and
                 * decode full quartets exclusively (loop bound `i + 3 < dlen`).
                 * The old code used olen=3*dlen/4 with an `i < dlen` loop that
                 * read data[i+1..i+3] unconditionally — over-reading past `data`
                 * and over-writing `out` on non-multiple-of-4 / unpadded input. */
                size_t olen = (dlen / 4) * 3 + 3;
                unsigned char *out = malloc(olen + 1);
                if (!out) { SP_PUSH(vm_str("")); break; }
                size_t j = 0;
                static const int db64[256] = {
                    ['A']=0,['B']=1,['C']=2,['D']=3,['E']=4,['F']=5,['G']=6,['H']=7,
                    ['I']=8,['J']=9,['K']=10,['L']=11,['M']=12,['N']=13,['O']=14,['P']=15,
                    ['Q']=16,['R']=17,['S']=18,['T']=19,['U']=20,['V']=21,['W']=22,['X']=23,
                    ['Y']=24,['Z']=25,['a']=26,['b']=27,['c']=28,['d']=29,['e']=30,['f']=31,
                    ['g']=32,['h']=33,['i']=34,['j']=35,['k']=36,['l']=37,['m']=38,['n']=39,
                    ['o']=40,['p']=41,['q']=42,['r']=43,['s']=44,['t']=45,['u']=46,['v']=47,
                    ['w']=48,['x']=49,['y']=50,['z']=51,['0']=52,['1']=53,['2']=54,['3']=55,
                    ['4']=56,['5']=57,['6']=58,['7']=59,['8']=60,['9']=61,['+']=62,['/']=63,
                };
                for (size_t i = 0; i + 3 < dlen; i += 4) {
                    uint32_t n = (db64[(unsigned char)data[i]] << 18) | (db64[(unsigned char)data[i+1]] << 12);
                    if (data[i+2] != '=') n |= db64[(unsigned char)data[i+2]] << 6;
                    if (data[i+3] != '=') n |= db64[(unsigned char)data[i+3]];
                    out[j++] = (n >> 16) & 0xFF;
                    if (data[i+2] != '=') out[j++] = (n >> 8) & 0xFF;
                    if (data[i+3] != '=') out[j++] = n & 0xFF;
                }
                out[j] = '\0';
                SP_PUSH(VM_MAKE_STR_N((char*)out, j));
                free(out);
            } else SP_PUSH(vm_str(""));
            break;
        }
        case BUILTIN_CORE_QUOTEMETA: {
            if (argc >= 1) {
                char buf[1024];
                const char *s = vm_to_cstr(args[0], buf, 1024);
                size_t len = strlen(s);
                char *out = malloc(len * 2 + 1);
                size_t j = 0;
                for (size_t i = 0; i < len; i++) {
                    if (!isalnum((unsigned char)s[i]) && s[i] != '_')
                        out[j++] = '\\';
                    out[j++] = s[i];
                }
                out[j] = '\0';
                SP_PUSH(vm_str(out));
                free(out);
            } else SP_PUSH(vm_str(""));
            break;
        }
        case BUILTIN_CORE_FNMATCH: {
            if (argc >= 2) {
                char pbuf[256], sbuf[1024];
                const char *pat = vm_to_cstr(args[0], pbuf, 256);
                const char *str = vm_to_cstr(args[1], sbuf, 1024);
                SP_PUSH(VM_MAKE_INT(fnmatch(pat, str, 0) == 0 ? 1 : 0));
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        case BUILTIN_CORE_FILE_EXT: {
            if (argc >= 1) {
                char buf[512];
                const char *path = vm_to_cstr(args[0], buf, 512);
                const char *dot = strrchr(path, '.');
                const char *slash = strrchr(path, '/');
                if (dot && (!slash || dot > slash)) SP_PUSH(vm_str(dot));
                else SP_PUSH(vm_str(""));
            } else SP_PUSH(vm_str(""));
            break;
        }
        case BUILTIN_CORE_PATH_JOIN: {
            if (argc >= 2) {
                char abuf[512], bbuf[512];
                const char *a = vm_to_cstr(args[0], abuf, 512);
                const char *b = vm_to_cstr(args[1], bbuf, 512);
                size_t alen = strlen(a);
                char out[1024];
                if (alen > 0 && a[alen-1] == '/') snprintf(out, sizeof(out), "%s%s", a, b);
                else snprintf(out, sizeof(out), "%s/%s", a, b);
                SP_PUSH(vm_str(out));
            } else if (argc == 1) {
                char buf[512];
                SP_PUSH(vm_str(vm_to_cstr(args[0], buf, 512)));
            } else SP_PUSH(vm_str(""));
            break;
        }
        case BUILTIN_CORE_GETUID: { SP_PUSH(VM_MAKE_INT(getuid())); break; }
        case BUILTIN_CORE_GETEUID: { SP_PUSH(VM_MAKE_INT(geteuid())); break; }
        case BUILTIN_CORE_GETGID: { SP_PUSH(VM_MAKE_INT(getgid())); break; }
        case BUILTIN_CORE_GETEGID: { SP_PUSH(VM_MAKE_INT(getegid())); break; }
        case BUILTIN_CORE_UMASK: {
            if (argc >= 1) SP_PUSH(VM_MAKE_INT(umask((mode_t)vm_to_int(args[0]))));
            else SP_PUSH(VM_MAKE_INT(umask(0)));
            break;
        }
        case BUILTIN_CORE_EXEC: {
            if (argc >= 1) {
                char buf[1024];
                const char *cmd = vm_to_cstr(args[0], buf, 1024);
                execlp("/bin/sh", "sh", "-c", cmd, NULL);
                /* Only returns on error */
                SP_PUSH(VM_MAKE_INT(-1));
            } else SP_PUSH(VM_MAKE_INT(-1));
            break;
        }
        case BUILTIN_CORE_SRAND: {
            if (argc >= 1) srand((unsigned)vm_to_int(args[0]));
            else srand((unsigned)time(NULL));
            SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_CORE_RAND: {
            VM_PUSH_DOUBLE(sp, (double)rand() / (double)RAND_MAX);
            break;
        }
        case BUILTIN_CORE_TRUNCATE: {
            if (argc >= 2) {
                char buf[512];
                SP_PUSH(VM_MAKE_INT(truncate(vm_to_cstr(args[0], buf, 512), (off_t)vm_to_int(args[1])) == 0 ? 1 : 0));
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        case BUILTIN_CORE_NANOSLEEP: {
            if (argc >= 1) {
                struct timespec ts;
                int64_t ns = vm_to_int(args[0]);
                ts.tv_sec = ns / 1000000000;
                ts.tv_nsec = ns % 1000000000;
                nanosleep(&ts, NULL);
            }
            SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_CORE_GETHOSTNAME: {
            char buf[256];
            if (gethostname(buf, sizeof(buf)) == 0) SP_PUSH(vm_str(buf));
            else SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_CORE_ACCESS: {
            if (argc >= 2) {
                char buf[512];
                SP_PUSH(VM_MAKE_INT(access(vm_to_cstr(args[0], buf, 512), (int)vm_to_int(args[1])) == 0 ? 1 : 0));
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        case BUILTIN_CORE_GLOB: {
            /* Simple glob using the system glob() function */
            if (argc >= 1) {
                char buf[512];
                const char *pattern = vm_to_cstr(args[0], buf, 512);
                glob_t g;
                VMArray *result = vm_array_new(16);
                if (glob(pattern, 0, NULL, &g) == 0) {
                    for (size_t i = 0; i < g.gl_pathc; i++)
                        vm_array_push(result, vm_str(g.gl_pathv[i]));
                    globfree(&g);
                }
                SP_PUSH(VM_MAKE_PTR(result));
            } else SP_PUSH(VM_MAKE_PTR(vm_array_new(0)));
            break;
        }

        /* ===== Runtime-delegated builtins ===== */
        /* These call the Strada runtime functions (StradaValue*-based) and convert results */
        /* Helper: call a runtime function, convert args via vm_to_sv, result via sv_to_vm */
#define RT_CALL_0(fn) { StradaValue *__r = fn(); SP_PUSH(sv_to_vm(__r)); strada_decref(__r); }
#define RT_CALL_1(fn) { StradaValue *__a0 = vm_to_sv(args[0]); StradaValue *__r = fn(__a0); SP_PUSH(sv_to_vm(__r)); strada_decref(__r); strada_decref(__a0); }
#define RT_CALL_2(fn) { StradaValue *__a0 = vm_to_sv(args[0]); StradaValue *__a1 = vm_to_sv(args[1]); StradaValue *__r = fn(__a0, __a1); SP_PUSH(sv_to_vm(__r)); strada_decref(__r); strada_decref(__a0); strada_decref(__a1); }
#define RT_CALL_3(fn) { StradaValue *__a0 = vm_to_sv(args[0]); StradaValue *__a1 = vm_to_sv(args[1]); StradaValue *__a2 = vm_to_sv(args[2]); StradaValue *__r = fn(__a0, __a1, __a2); SP_PUSH(sv_to_vm(__r)); strada_decref(__r); strada_decref(__a0); strada_decref(__a1); strada_decref(__a2); }
#define RT_VOID_1(fn) { StradaValue *__a0 = vm_to_sv(args[0]); fn(__a0); SP_PUSH(VM_UNDEF_VAL); strada_decref(__a0); }
#define RT_VOID_2(fn) { StradaValue *__a0 = vm_to_sv(args[0]); StradaValue *__a1 = vm_to_sv(args[1]); fn(__a0, __a1); SP_PUSH(VM_UNDEF_VAL); strada_decref(__a0); strada_decref(__a1); }

        /* === Socket/networking === */
        case BUILTIN_CORE_SOCKET_CLIENT: {
            char hbuf[256];
            const char *host = vm_to_cstr(args[0], hbuf, 256);
            int port = (int)vm_to_int(args[1]);
            StradaValue *r = strada_socket_client(host, port);
            SP_PUSH(sv_to_vm(r)); strada_decref(r);
            break;
        }
        case BUILTIN_CORE_SOCKET_SERVER: {
            StradaValue *r = strada_socket_server((int)vm_to_int(args[0]));
            SP_PUSH(sv_to_vm(r)); strada_decref(r);
            break;
        }
        case BUILTIN_CORE_SOCKET_SERVER_BACKLOG: {
            StradaValue *r = strada_socket_server_backlog((int)vm_to_int(args[0]), (int)vm_to_int(args[1]));
            SP_PUSH(sv_to_vm(r)); strada_decref(r);
            break;
        }
        case BUILTIN_CORE_SOCKET_ACCEPT: RT_CALL_1(strada_socket_accept); break;
        case BUILTIN_CORE_SOCKET_RECV: {
            StradaValue *sock = vm_to_sv(args[0]);
            StradaValue *r = strada_socket_recv(sock, (int)vm_to_int(args[1]));
            SP_PUSH(sv_to_vm(r)); strada_decref(r); strada_decref(sock);
            break;
        }
        case BUILTIN_CORE_SOCKET_SEND: {
            StradaValue *sock = vm_to_sv(args[0]);
            StradaValue *data = vm_to_sv(args[1]);
            strada_socket_send_sv(sock, data);
            SP_PUSH(VM_UNDEF_VAL);
            strada_decref(sock); strada_decref(data);
            break;
        }
        case BUILTIN_CORE_SOCKET_CLOSE: RT_VOID_1(strada_socket_close); break;
        case BUILTIN_CORE_SOCKET_FLUSH: RT_VOID_1(strada_socket_flush); break;
        case BUILTIN_CORE_SOCKET_SELECT: {
            StradaValue *socks = vm_to_sv(args[0]);
            StradaValue *r = strada_socket_select(socks, (int)vm_to_int(args[1]));
            SP_PUSH(sv_to_vm(r)); strada_decref(r); strada_decref(socks);
            break;
        }
        case BUILTIN_CORE_SOCKET_FD: {
            StradaValue *sock = vm_to_sv(args[0]);
            SP_PUSH(VM_MAKE_INT(strada_socket_fd(sock)));
            strada_decref(sock);
            break;
        }
        case BUILTIN_CORE_SOCKET_SET_NONBLOCKING: {
            StradaValue *sock = vm_to_sv(args[0]);
            strada_socket_set_nonblocking(sock, (int)vm_to_int(args[1]));
            SP_PUSH(VM_UNDEF_VAL);
            strada_decref(sock);
            break;
        }
        case BUILTIN_CORE_UDP_SOCKET: RT_CALL_0(strada_udp_socket); break;
        case BUILTIN_CORE_UDP_BIND: {
            StradaValue *sock = vm_to_sv(args[0]);
            int r = strada_udp_bind(sock, (int)vm_to_int(args[1]));
            SP_PUSH(VM_MAKE_INT(r)); strada_decref(sock);
            break;
        }
        case BUILTIN_CORE_UDP_SERVER: {
            StradaValue *r = strada_udp_server((int)vm_to_int(args[0]));
            SP_PUSH(sv_to_vm(r)); strada_decref(r);
            break;
        }
        case BUILTIN_CORE_UDP_RECVFROM: {
            StradaValue *sock = vm_to_sv(args[0]);
            StradaValue *r = strada_udp_recvfrom(sock, (int)vm_to_int(args[1]));
            SP_PUSH(sv_to_vm(r)); strada_decref(r); strada_decref(sock);
            break;
        }
        case BUILTIN_CORE_UDP_SENDTO: {
            StradaValue *sock = vm_to_sv(args[0]);
            char dbuf[4096], hbuf[256];
            const char *data = vm_to_cstr(args[1], dbuf, 4096);
            const char *host = vm_to_cstr(args[2], hbuf, 256);
            int port = (int)vm_to_int(args[3]);
            int r = strada_udp_sendto(sock, data, (int)strlen(data), host, port);
            SP_PUSH(VM_MAKE_INT(r)); strada_decref(sock);
            break;
        }

        /* === C interop === */
        case BUILTIN_C_ALLOC: {
            size_t sz = (size_t)vm_to_int(args[0]);
            void *p = calloc(1, sz);
            SP_PUSH(VM_MAKE_INT((int64_t)(uintptr_t)p));
            break;
        }
        case BUILTIN_C_FREE: {
            void *p = (void*)(uintptr_t)vm_to_int(args[0]);
            if (p) free(p);
            SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_C_REALLOC: {
            void *p = (void*)(uintptr_t)vm_to_int(args[0]);
            size_t sz = (size_t)vm_to_int(args[1]);
            void *np = realloc(p, sz);
            SP_PUSH(VM_MAKE_INT((int64_t)(uintptr_t)np));
            break;
        }
        case BUILTIN_C_NULL: { SP_PUSH(VM_MAKE_INT(0)); break; }
        case BUILTIN_C_IS_NULL: { SP_PUSH(VM_MAKE_INT(vm_to_int(args[0]) == 0 ? 1 : 0)); break; }
        case BUILTIN_C_PTR_ADD: {
            SP_PUSH(VM_MAKE_INT(vm_to_int(args[0]) + vm_to_int(args[1])));
            break;
        }
        case BUILTIN_C_STR_TO_PTR: {
            char buf[4096];
            const char *s = vm_to_cstr(args[0], buf, 4096);
            char *copy = strdup(s);
            SP_PUSH(VM_MAKE_INT((int64_t)(uintptr_t)copy));
            break;
        }
        case BUILTIN_C_PTR_TO_STR: {
            char *p = (char*)(uintptr_t)vm_to_int(args[0]);
            SP_PUSH(p ? vm_str(p) : VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_C_PTR_TO_STR_N: {
            char *p = (char*)(uintptr_t)vm_to_int(args[0]);
            size_t n = (size_t)vm_to_int(args[1]);
            SP_PUSH(p ? VM_MAKE_STR_N(p, n) : VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_C_READ_INT8:  { int8_t  *p = (int8_t*)(uintptr_t)vm_to_int(args[0]); SP_PUSH(VM_MAKE_INT(p ? *p : 0)); break; }
        case BUILTIN_C_READ_INT16: { int16_t *p = (int16_t*)(uintptr_t)vm_to_int(args[0]); SP_PUSH(VM_MAKE_INT(p ? *p : 0)); break; }
        case BUILTIN_C_READ_INT32: { int32_t *p = (int32_t*)(uintptr_t)vm_to_int(args[0]); SP_PUSH(VM_MAKE_INT(p ? *p : 0)); break; }
        case BUILTIN_C_READ_INT64: { int64_t *p = (int64_t*)(uintptr_t)vm_to_int(args[0]); SP_PUSH(VM_MAKE_INT(p ? *p : 0)); break; }
        case BUILTIN_C_READ_PTR:   { void **p = (void**)(uintptr_t)vm_to_int(args[0]); SP_PUSH(VM_MAKE_INT(p ? (int64_t)(uintptr_t)*p : 0)); break; }
        case BUILTIN_C_READ_FLOAT: { float *p = (float*)(uintptr_t)vm_to_int(args[0]); VM_PUSH_DOUBLE(sp, p ? (double)*p : 0.0); break; }
        case BUILTIN_C_READ_DOUBLE: { double *p = (double*)(uintptr_t)vm_to_int(args[0]); VM_PUSH_DOUBLE(sp, p ? *p : 0.0); break; }
        case BUILTIN_C_WRITE_INT8:  { int8_t  *p = (int8_t*)(uintptr_t)vm_to_int(args[0]); if (p) *p = (int8_t)vm_to_int(args[1]); SP_PUSH(VM_UNDEF_VAL); break; }
        case BUILTIN_C_WRITE_INT16: { int16_t *p = (int16_t*)(uintptr_t)vm_to_int(args[0]); if (p) *p = (int16_t)vm_to_int(args[1]); SP_PUSH(VM_UNDEF_VAL); break; }
        case BUILTIN_C_WRITE_INT32: { int32_t *p = (int32_t*)(uintptr_t)vm_to_int(args[0]); if (p) *p = (int32_t)vm_to_int(args[1]); SP_PUSH(VM_UNDEF_VAL); break; }
        case BUILTIN_C_WRITE_INT64: { int64_t *p = (int64_t*)(uintptr_t)vm_to_int(args[0]); if (p) *p = vm_to_int(args[1]); SP_PUSH(VM_UNDEF_VAL); break; }
        case BUILTIN_C_WRITE_PTR:   { void **p = (void**)(uintptr_t)vm_to_int(args[0]); if (p) *p = (void*)(uintptr_t)vm_to_int(args[1]); SP_PUSH(VM_UNDEF_VAL); break; }
        case BUILTIN_C_WRITE_FLOAT: { float *p = (float*)(uintptr_t)vm_to_int(args[0]); if (p) *p = (float)vm_to_num(args[1]); SP_PUSH(VM_UNDEF_VAL); break; }
        case BUILTIN_C_WRITE_DOUBLE: { double *p = (double*)(uintptr_t)vm_to_int(args[0]); if (p) *p = vm_to_num(args[1]); SP_PUSH(VM_UNDEF_VAL); break; }
        case BUILTIN_C_SIZEOF_INT:    { SP_PUSH(VM_MAKE_INT(sizeof(int))); break; }
        case BUILTIN_C_SIZEOF_LONG:   { SP_PUSH(VM_MAKE_INT(sizeof(long))); break; }
        case BUILTIN_C_SIZEOF_PTR:    { SP_PUSH(VM_MAKE_INT(sizeof(void*))); break; }
        case BUILTIN_C_SIZEOF_SIZE_T: { SP_PUSH(VM_MAKE_INT(sizeof(size_t))); break; }
        case BUILTIN_C_MEMCPY: {
            void *dst = (void*)(uintptr_t)vm_to_int(args[0]);
            void *src = (void*)(uintptr_t)vm_to_int(args[1]);
            size_t n = (size_t)vm_to_int(args[2]);
            if (dst && src) memcpy(dst, src, n);
            SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_C_MEMSET: {
            void *dst = (void*)(uintptr_t)vm_to_int(args[0]);
            int val = (int)vm_to_int(args[1]);
            size_t n = (size_t)vm_to_int(args[2]);
            if (dst) memset(dst, val, n);
            SP_PUSH(VM_UNDEF_VAL);
            break;
        }

        /* === Terminal I/O === */
        case BUILTIN_CORE_TERM_ENABLE_RAW:  RT_CALL_0(strada_term_enable_raw); break;
        case BUILTIN_CORE_TERM_DISABLE_RAW: RT_CALL_0(strada_term_disable_raw); break;
        case BUILTIN_CORE_TERM_ROWS: RT_CALL_0(strada_term_rows); break;
        case BUILTIN_CORE_TERM_COLS: RT_CALL_0(strada_term_cols); break;
        case BUILTIN_CORE_READ_BYTE: {
            StradaValue *fd = vm_to_sv(args[0]);
            StradaValue *r = strada_read_byte(fd);
            SP_PUSH(sv_to_vm(r)); strada_decref(r); strada_decref(fd);
            break;
        }
        case BUILTIN_CORE_TTYNAME: RT_CALL_1(strada_ttyname); break;

        /* === Async/threading === */
        case BUILTIN_THREAD_CREATE: RT_CALL_1(strada_thread_create); break;
        case BUILTIN_THREAD_JOIN:   RT_CALL_1(strada_thread_join); break;
        case BUILTIN_THREAD_DETACH: RT_CALL_1(strada_thread_detach); break;
        case BUILTIN_THREAD_SELF:   RT_CALL_0(strada_thread_self); break;
        case BUILTIN_ASYNC_CHANNEL: {
            int cap = argc >= 1 ? (int)vm_to_int(args[0]) : 0;
            StradaValue *r = strada_channel_new(cap);
            SP_PUSH(sv_to_vm(r)); strada_decref(r);
            break;
        }
        case BUILTIN_ASYNC_SEND: {
            StradaValue *ch = vm_to_sv(args[0]);
            StradaValue *val = vm_to_sv(args[1]);
            strada_channel_send(ch, val);
            SP_PUSH(VM_UNDEF_VAL); strada_decref(ch); strada_decref(val);
            break;
        }
        case BUILTIN_ASYNC_RECV: RT_CALL_1(strada_channel_recv); break;
        case BUILTIN_ASYNC_TRY_SEND: {
            StradaValue *ch = vm_to_sv(args[0]);
            StradaValue *val = vm_to_sv(args[1]);
            int r = strada_channel_try_send(ch, val);
            SP_PUSH(VM_MAKE_INT(r)); strada_decref(ch); strada_decref(val);
            break;
        }
        case BUILTIN_ASYNC_TRY_RECV: RT_CALL_1(strada_channel_try_recv); break;
        case BUILTIN_ASYNC_CLOSE: {
            StradaValue *ch = vm_to_sv(args[0]);
            strada_channel_close(ch);
            SP_PUSH(VM_UNDEF_VAL); strada_decref(ch);
            break;
        }
        case BUILTIN_ASYNC_IS_CLOSED: {
            StradaValue *ch = vm_to_sv(args[0]);
            SP_PUSH(VM_MAKE_INT(strada_channel_is_closed(ch))); strada_decref(ch);
            break;
        }
        case BUILTIN_ASYNC_LEN: {
            StradaValue *ch = vm_to_sv(args[0]);
            SP_PUSH(VM_MAKE_INT(strada_channel_len(ch))); strada_decref(ch);
            break;
        }
        case BUILTIN_ASYNC_MUTEX: {
            StradaValue *r = strada_mutex_new();
            SP_PUSH(sv_to_vm(r)); strada_decref(r);
            break;
        }
        case BUILTIN_ASYNC_LOCK: RT_CALL_1(strada_mutex_lock); break;
        case BUILTIN_ASYNC_UNLOCK: RT_CALL_1(strada_mutex_unlock); break;
        case BUILTIN_ASYNC_TRY_LOCK: RT_CALL_1(strada_mutex_trylock); break;
        case BUILTIN_ASYNC_MUTEX_DESTROY: RT_CALL_1(strada_mutex_destroy); break;
        case BUILTIN_ASYNC_ATOMIC: {
            int64_t init = argc >= 1 ? vm_to_int(args[0]) : 0;
            StradaValue *r = strada_atomic_new(init);
            SP_PUSH(sv_to_vm(r)); strada_decref(r);
            break;
        }
        case BUILTIN_ASYNC_ATOMIC_LOAD: {
            StradaValue *a = vm_to_sv(args[0]);
            SP_PUSH(VM_MAKE_INT(strada_atomic_load(a))); strada_decref(a);
            break;
        }
        case BUILTIN_ASYNC_ATOMIC_STORE: {
            StradaValue *a = vm_to_sv(args[0]);
            strada_atomic_store(a, vm_to_int(args[1]));
            SP_PUSH(VM_UNDEF_VAL); strada_decref(a);
            break;
        }
        case BUILTIN_ASYNC_ATOMIC_ADD: {
            StradaValue *a = vm_to_sv(args[0]);
            SP_PUSH(VM_MAKE_INT(strada_atomic_add(a, vm_to_int(args[1])))); strada_decref(a);
            break;
        }
        case BUILTIN_ASYNC_ATOMIC_SUB: {
            StradaValue *a = vm_to_sv(args[0]);
            SP_PUSH(VM_MAKE_INT(strada_atomic_sub(a, vm_to_int(args[1])))); strada_decref(a);
            break;
        }
        case BUILTIN_ASYNC_ATOMIC_INC: {
            StradaValue *a = vm_to_sv(args[0]);
            SP_PUSH(VM_MAKE_INT(strada_atomic_inc(a))); strada_decref(a);
            break;
        }
        case BUILTIN_ASYNC_ATOMIC_DEC: {
            StradaValue *a = vm_to_sv(args[0]);
            SP_PUSH(VM_MAKE_INT(strada_atomic_dec(a))); strada_decref(a);
            break;
        }
        case BUILTIN_ASYNC_ATOMIC_CAS: {
            StradaValue *a = vm_to_sv(args[0]);
            SP_PUSH(VM_MAKE_INT(strada_atomic_cas(a, vm_to_int(args[1]), vm_to_int(args[2])))); strada_decref(a);
            break;
        }

        /* === Advanced OOP === */
        case BUILTIN_CORE_WEAKEN: {
            StradaValue *sv = vm_to_sv(args[0]);
            strada_weaken(&sv);
            SP_PUSH(VM_UNDEF_VAL); strada_decref(sv);
            break;
        }
        case BUILTIN_CORE_ISWEAK: {
            StradaValue *sv = vm_to_sv(args[0]);
            SP_PUSH(VM_MAKE_INT(strada_isweak(sv)));
            strada_decref(sv);
            break;
        }
        case BUILTIN_TIE: {
            /* tie(variable, classname, ...) — VM classes: call TIEHASH via a
             * nested vm_execute_call and remember the implementation object
             * on the hash; falls back to the runtime for native classes. */
            if (argc < 2) { SP_PUSH(VM_UNDEF_VAL); break; }
            char cbuf[256]; const char *cls = vm_to_cstr(args[1], cbuf, 256);
            if (VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_HASH) {
                int tfidx = vm_find_method(vm->program, cls, "TIEHASH");
                if (tfidx >= 0) {
                    frame->ip = ip; vm->stack_top = sp;
                    VMValue cargs[8];
                    int cn = 0;
                    cargs[cn++] = vm_str(cls);
                    for (int ai = 2; ai < argc && cn < 8; ai++) cargs[cn++] = args[ai];
                    VMValue obj = vm_execute_call(vm, tfidx, cargs, cn);
                    stack = vm->stack;
                    RELOAD();
                    if (__builtin_expect(vm->has_pending_exc, 0)) {
                        for (int _ai = 0; _ai < argc; _ai++) {
                            if (VM_IS_PTR(args[_ai]) && (VM_PTR_TYPE(args[_ai]) == VM_OBJ_STR || VM_PTR_TYPE(args[_ai]) == VM_OBJ_STRBUF))
                                vm_val_free(&args[_ai]);
                        }
                        if (args != args_stk) free(args);
                        throw_exc = vm->pending_exc;
                        vm->pending_exc = VM_UNDEF_VAL;
                        vm->has_pending_exc = 0;
                        goto L_vm_throw_exc;
                    }
                    VM_AS_HASH(args[0])->tied_obj = obj;
                    SP_PUSH(obj);
                    break;
                }
            }
            StradaValue *var = vm_to_sv(args[0]);
            StradaValue *r = strada_tie_hash(var, cls, 0);
            SP_PUSH(sv_to_vm(r)); strada_decref(r); strada_decref(var);
            break;
        }
        case BUILTIN_UNTIE: {
            if (argc < 1) { SP_PUSH(VM_UNDEF_VAL); break; }
            if (VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_HASH &&
                !VM_IS_UNDEF(VM_AS_HASH(args[0])->tied_obj)) {
                VM_AS_HASH(args[0])->tied_obj = VM_UNDEF_VAL;
                SP_PUSH(VM_UNDEF_VAL);
                break;
            }
            StradaValue *var = vm_to_sv(args[0]);
            strada_untie(var);
            SP_PUSH(VM_UNDEF_VAL); strada_decref(var);
            break;
        }
        case BUILTIN_TIED: {
            if (argc < 1) { SP_PUSH(VM_UNDEF_VAL); break; }
            if (VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_HASH &&
                !VM_IS_UNDEF(VM_AS_HASH(args[0])->tied_obj)) {
                SP_PUSH(VM_AS_HASH(args[0])->tied_obj);
                break;
            }
            RT_CALL_1(strada_tied);
            break;
        }

        /* === String operations === */
        case BUILTIN_STR_REPLACE_PLAIN: {
            /* str_replace(str, find, replace) — replace all occurrences (plain string) */
            char sbuf[4096], fbuf[256], rbuf[256];
            const char *s = vm_to_cstr(args[0], sbuf, 4096);
            const char *find = vm_to_cstr(args[1], fbuf, 256);
            const char *repl = vm_to_cstr(args[2], rbuf, 256);
            size_t flen = strlen(find), rlen = strlen(repl), slen = strlen(s);
            if (flen == 0) { SP_PUSH(vm_str(s)); break; }
            size_t cap = slen * 2 + 64;
            char *out = malloc(cap);
            size_t oi = 0;
            for (size_t i = 0; i <= slen; ) {
                if (i + flen <= slen && strncmp(s + i, find, flen) == 0) {
                    if (oi + rlen >= cap) { cap *= 2; out = realloc(out, cap); }
                    memcpy(out + oi, repl, rlen); oi += rlen; i += flen;
                } else {
                    if (oi + 1 >= cap) { cap *= 2; out = realloc(out, cap); }
                    if (i < slen) out[oi++] = s[i++]; else break;
                }
            }
            out[oi] = '\0';
            SP_PUSH(vm_str(out)); free(out);
            break;
        }
        case BUILTIN_STR_REPLACE_FIRST: {
            char sbuf[4096], fbuf[256], rbuf[256];
            const char *s = vm_to_cstr(args[0], sbuf, 4096);
            const char *find = vm_to_cstr(args[1], fbuf, 256);
            const char *repl = vm_to_cstr(args[2], rbuf, 256);
            const char *pos = strstr(s, find);
            if (!pos) { SP_PUSH(vm_str(s)); break; }
            size_t before = pos - s;
            size_t flen = strlen(find), rlen = strlen(repl), slen = strlen(s);
            char *out = malloc(slen - flen + rlen + 1);
            memcpy(out, s, before);
            memcpy(out + before, repl, rlen);
            memcpy(out + before + rlen, pos + flen, slen - before - flen + 1);
            SP_PUSH(vm_str(out)); free(out);
            break;
        }
        case BUILTIN_REGEX_REPLACE: {
            char sbuf[4096], pbuf[256], rbuf[256];
            const char *s = vm_to_cstr(args[0], sbuf, 4096);
            const char *pat = vm_to_cstr(args[1], pbuf, 256);
            const char *repl = vm_to_cstr(args[2], rbuf, 256);
            char *result = strada_regex_replace(s, pat, repl, "");
            SP_PUSH(vm_str(result)); free(result);
            break;
        }
        case BUILTIN_REGEX_REPLACE_ALL: {
            char sbuf[4096], pbuf[256], rbuf[256];
            const char *s = vm_to_cstr(args[0], sbuf, 4096);
            const char *pat = vm_to_cstr(args[1], pbuf, 256);
            const char *repl = vm_to_cstr(args[2], rbuf, 256);
            char *result = strada_regex_replace_all(s, pat, repl, "g");
            SP_PUSH(vm_str(result)); free(result);
            break;
        }

        /* === StringBuilder === */
        case BUILTIN_SB_NEW: RT_CALL_0(strada_sb_new); break;
        case BUILTIN_SB_NEW_CAP: RT_CALL_1(strada_sb_new_cap); break;
        case BUILTIN_SB_APPEND: {
            StradaValue *sb = vm_to_sv(args[0]);
            StradaValue *val = vm_to_sv(args[1]);
            strada_sb_append(sb, val);
            SP_PUSH(VM_UNDEF_VAL);
            strada_decref(sb); strada_decref(val);
            break;
        }
        case BUILTIN_SB_TO_STRING: RT_CALL_1(strada_sb_to_string); break;
        case BUILTIN_SB_LENGTH: RT_CALL_1(strada_sb_length); break;
        case BUILTIN_SB_CLEAR: RT_VOID_1(strada_sb_clear); break;
        case BUILTIN_SB_FREE: RT_VOID_1(strada_sb_free); break;

        /* === Dynamic loading === */
        case BUILTIN_CORE_DL_OPEN: RT_CALL_1(strada_dl_open); break;
        case BUILTIN_CORE_DL_SYM: RT_CALL_2(strada_dl_sym); break;
        case BUILTIN_CORE_DL_CLOSE: RT_CALL_1(strada_dl_close); break;
        case BUILTIN_CORE_DL_ERROR: RT_CALL_0(strada_dl_error); break;
        case BUILTIN_CORE_DL_CALL_INT: RT_CALL_2(strada_dl_call_int); break;
        case BUILTIN_CORE_DL_CALL_STR: RT_CALL_2(strada_dl_call_str); break;
        case BUILTIN_CORE_DL_CALL_VOID: RT_CALL_2(strada_dl_call_void); break;
        case BUILTIN_CORE_DL_CALL_INT_SV: RT_CALL_2(strada_dl_call_int_sv); break;
        case BUILTIN_CORE_DL_CALL_STR_SV: RT_CALL_2(strada_dl_call_str_sv); break;
        case BUILTIN_CORE_DL_CALL_VOID_SV: RT_CALL_2(strada_dl_call_void_sv); break;

        /* === CStruct === */
        case BUILTIN_CORE_CSTRUCT_NEW: {
            char nbuf[256];
            const char *name = vm_to_cstr(args[0], nbuf, 256);
            size_t sz = (size_t)vm_to_int(args[1]);
            StradaValue *r = strada_cstruct_new(name, sz);
            SP_PUSH(sv_to_vm(r)); strada_decref(r);
            break;
        }
        case BUILTIN_CORE_CSTRUCT_PTR: {
            StradaValue *s = vm_to_sv(args[0]);
            void *p = strada_cstruct_ptr(s);
            SP_PUSH(VM_MAKE_INT((int64_t)(uintptr_t)p)); strada_decref(s);
            break;
        }
        case BUILTIN_CORE_CSTRUCT_SET_INT: {
            /* cstruct_set_int(sv, field, offset, value) */
            StradaValue *s = vm_to_sv(args[0]);
            char fbuf[256]; const char *field = vm_to_cstr(args[1], fbuf, 256);
            size_t off = (size_t)vm_to_int(args[2]);
            strada_cstruct_set_int(s, field, off, vm_to_int(args[3]));
            SP_PUSH(VM_UNDEF_VAL); strada_decref(s);
            break;
        }
        case BUILTIN_CORE_CSTRUCT_GET_INT: {
            StradaValue *s = vm_to_sv(args[0]);
            char fbuf[256]; const char *field = vm_to_cstr(args[1], fbuf, 256);
            size_t off = (size_t)vm_to_int(args[2]);
            int64_t r = strada_cstruct_get_int(s, field, off);
            SP_PUSH(VM_MAKE_INT(r)); strada_decref(s);
            break;
        }
        case BUILTIN_CORE_CSTRUCT_SET_STRING: {
            StradaValue *s = vm_to_sv(args[0]);
            char fbuf[256]; const char *field = vm_to_cstr(args[1], fbuf, 256);
            size_t off = (size_t)vm_to_int(args[2]);
            char vbuf[4096]; const char *val = vm_to_cstr(args[3], vbuf, 4096);
            strada_cstruct_set_string(s, field, off, val);
            SP_PUSH(VM_UNDEF_VAL); strada_decref(s);
            break;
        }
        case BUILTIN_CORE_CSTRUCT_GET_STRING: {
            StradaValue *s = vm_to_sv(args[0]);
            char fbuf[256]; const char *field = vm_to_cstr(args[1], fbuf, 256);
            size_t off = (size_t)vm_to_int(args[2]);
            char *r = strada_cstruct_get_string(s, field, off);
            SP_PUSH(r ? vm_str(r) : VM_UNDEF_VAL); strada_decref(s);
            break;
        }
        case BUILTIN_CORE_CSTRUCT_SET_DOUBLE: {
            StradaValue *s = vm_to_sv(args[0]);
            char fbuf[256]; const char *field = vm_to_cstr(args[1], fbuf, 256);
            size_t off = (size_t)vm_to_int(args[2]);
            strada_cstruct_set_double(s, field, off, vm_to_num(args[3]));
            SP_PUSH(VM_UNDEF_VAL); strada_decref(s);
            break;
        }
        case BUILTIN_CORE_CSTRUCT_GET_DOUBLE: {
            StradaValue *s = vm_to_sv(args[0]);
            char fbuf[256]; const char *field = vm_to_cstr(args[1], fbuf, 256);
            size_t off = (size_t)vm_to_int(args[2]);
            double r = strada_cstruct_get_double(s, field, off);
            VM_PUSH_DOUBLE(sp, r); strada_decref(s);
            break;
        }

        /* === UTF-8 === */
        case BUILTIN_UTF8_IS_UTF8: RT_CALL_1(strada_utf8_is_utf8); break;
        case BUILTIN_UTF8_VALID:   RT_CALL_1(strada_utf8_valid); break;
        case BUILTIN_UTF8_ENCODE:  RT_CALL_1(strada_utf8_encode); break;
        case BUILTIN_UTF8_DECODE:  RT_CALL_1(strada_utf8_decode); break;
        case BUILTIN_UTF8_DOWNGRADE: {
            StradaValue *sv = vm_to_sv(args[0]);
            int fail_ok = argc >= 2 ? (int)vm_to_int(args[1]) : 0;
            StradaValue *r = strada_utf8_downgrade(sv, fail_ok);
            SP_PUSH(sv_to_vm(r)); strada_decref(r); strada_decref(sv);
            break;
        }
        case BUILTIN_UTF8_UPGRADE: RT_CALL_1(strada_utf8_upgrade); break;
        case BUILTIN_UTF8_UNICODE_TO_NATIVE: RT_CALL_1(strada_utf8_unicode_to_native); break;

        /* === Misc core === */
        case BUILTIN_CORE_WANTARRAY: { SP_PUSH(VM_MAKE_INT(0)); break; } /* VM always scalar context */
        case BUILTIN_CORE_SELECT_FDS: {
            /* Simplified: just return undef for now */
            SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_CORE_SET_BYTE: {
            /* set_byte(str, pos, byte) — returns modified string */
            char buf[64]; size_t len = 0;
            const char *s = vm_bytes_view(args[0], buf, sizeof(buf), &len);
            int64_t pos = vm_to_int(args[1]);
            int64_t val = vm_to_int(args[2]);
            char *out = malloc(len + 1);
            if (len > 0) memcpy(out, s, len);
            out[len] = '\0';
            if (pos >= 0 && (size_t)pos < len) out[pos] = (char)(val & 0xFF);
            SP_PUSH(VM_MAKE_STR_N(out, len)); free(out);
            break;
        }
        case BUILTIN_CORE_BYTE_SUBSTR: {
            /* byte_substr(str, offset, length) */
            char buf[64]; size_t slen = 0;
            const char *s = vm_bytes_view(args[0], buf, sizeof(buf), &slen);
            int64_t off = vm_to_int(args[1]);
            if (off < 0) off = (int64_t)slen + off;
            if (off < 0) off = 0;
            if ((size_t)off >= slen) {
                SP_PUSH(VM_MAKE_STR_N("", 0));
            } else {
                int64_t rem = (int64_t)slen - off;
                int64_t len = argc >= 3 ? vm_to_int(args[2]) : rem;
                if (len < 0) len = rem + len;
                if (len < 0) len = 0;
                if (len > rem) len = rem;
                SP_PUSH(VM_MAKE_STR_N(s + off, (size_t)len));
            }
            break;
        }

        /* === Common bare functions === */
        case BUILTIN_DIE: {
            if (argc == 0) {
                throw_exc = vm_str("Died");
            } else if (VM_IS_PTR(args[0]) &&
                       (VM_PTR_TYPE(args[0]) == VM_OBJ_HASH ||
                        VM_PTR_TYPE(args[0]) == VM_OBJ_ARRAY ||
                        VM_PTR_TYPE(args[0]) == VM_OBJ_SREF ||
                        VM_PTR_TYPE(args[0]) == VM_OBJ_CLOSURE ||
                        VM_PTR_TYPE(args[0]) == VM_OBJ_NATIVE_SV)) {
                throw_exc = args[0];
            } else {
                char buf[4096];
                throw_exc = vm_str(vm_to_cstr(args[0], buf, sizeof(buf)));
            }
            for (int _ai = 0; _ai < argc; _ai++) {
                if (VM_IS_PTR(args[_ai]) && (VM_PTR_TYPE(args[_ai]) == VM_OBJ_STR || VM_PTR_TYPE(args[_ai]) == VM_OBJ_STRBUF))
                    vm_val_free(&args[_ai]);
            }
            if (args != args_stk) free(args);
            vm->stack_top = sp;
            goto L_vm_throw_exc;
        }
        case BUILTIN_WARN: {
            char buf[4096];
            for (int i = 0; i < argc; i++) {
                const char *s = vm_to_cstr(args[i], buf, sizeof(buf));
                fprintf(stderr, "%s", s);
            }
            if (argc == 0) {
                fprintf(stderr, "\n");
            } else {
                const char *last_s = vm_to_cstr(args[argc - 1], buf, sizeof(buf));
                size_t last_len = strlen(last_s);
                if (last_len == 0 || last_s[last_len - 1] != '\n')
                    fprintf(stderr, "\n");
            }
            SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_BLESSED: {
            if (argc >= 1 && VM_IS_PTR(args[0])) {
                if (VM_PTR_TYPE(args[0]) == VM_OBJ_HASH) {
                    VMHash *h = VM_AS_HASH(args[0]);
                    SP_PUSH(h->class_name ? vm_str(h->class_name) : VM_UNDEF_VAL);
                } else if (VM_PTR_TYPE(args[0]) == VM_OBJ_NATIVE_SV) {
                    VMNativeSV *nsv = (VMNativeSV*)VM_TO_PTR(args[0]);
                    if (nsv->sv && !STRADA_IS_TAGGED_INT(nsv->sv) && nsv->sv->meta && nsv->sv->meta->blessed_package)
                        SP_PUSH(vm_str(nsv->sv->meta->blessed_package));
                    else SP_PUSH(VM_UNDEF_VAL);
                } else SP_PUSH(VM_UNDEF_VAL);
            } else SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_REFCOUNT: {
            if (argc >= 1) {
                if (VM_IS_UNDEF(args[0])) SP_PUSH(VM_MAKE_INT(0));
                else if (VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_NATIVE_SV) {
                    VMNativeSV *nsv = (VMNativeSV*)VM_TO_PTR(args[0]);
                    SP_PUSH(VM_MAKE_INT(strada_refcount(nsv->sv)));
                } else {
                    SP_PUSH(VM_MAKE_INT(1));
                }
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        case BUILTIN_TYPEOF: {
            if (argc >= 1) {
                if (VM_IS_INT(args[0])) SP_PUSH(vm_str("int"));
                else if (VM_IS_UNDEF(args[0])) SP_PUSH(vm_str("undef"));
                else if (VM_IS_PTR(args[0])) {
                    switch (VM_PTR_TYPE(args[0])) {
                    case VM_OBJ_FLOAT: SP_PUSH(vm_str("num")); break;
                    case VM_OBJ_STR: case VM_OBJ_STRBUF: SP_PUSH(vm_str("str")); break;
                    case VM_OBJ_ARRAY: SP_PUSH(vm_str("array")); break;
                    case VM_OBJ_HASH:
                        SP_PUSH(vm_str(VM_AS_HASH(args[0])->class_name ? "ref" : "hash"));
                        break;
                    case VM_OBJ_CLOSURE: SP_PUSH(vm_str("code")); break;
                    case VM_OBJ_FILEHANDLE: SP_PUSH(vm_str("filehandle")); break;
                    case VM_OBJ_NATIVE_SV:
                    case VM_OBJ_SREF:
                        SP_PUSH(vm_str("ref"));
                        break;
                    default: SP_PUSH(vm_str("unknown")); break;
                    }
                } else SP_PUSH(vm_str("unknown"));
            } else SP_PUSH(vm_str("undef"));
            break;
        }
        case BUILTIN_ARRAY_SHALLOW_COPY: {
            if (argc >= 1 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_ARRAY) {
                VMArray *src = VM_AS_ARRAY(args[0]);
                VMArray *cp = vm_array_new(src->size > 0 ? src->size : 8);
                for (int i = 0; i < src->size; i++) {
                    vm_array_push(cp, vm_val_copy(src->items[i]));
                }
                SP_PUSH(VM_MAKE_PTR(cp));
            } else if (argc >= 1 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_HASH &&
                       VM_AS_HASH(args[0])->size == 0) {
                SP_PUSH(VM_MAKE_PTR(vm_array_new(8)));
            } else if (argc >= 1 && !VM_IS_UNDEF(args[0])) {
                VMArray *cp = vm_array_new(8);
                vm_array_push(cp, vm_val_copy(args[0]));
                SP_PUSH(VM_MAKE_PTR(cp));
            } else {
                SP_PUSH(VM_MAKE_PTR(vm_array_new(8)));
            }
            break;
        }
        case BUILTIN_HASH_SHALLOW_COPY: {
            if (argc >= 1 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_HASH) {
                VMHash *src = VM_AS_HASH(args[0]);
                VMHash *cp = vm_hash_new(src->capacity > 16 ? src->capacity : 16);
                for (int i = 0; i < src->capacity; i++) {
                    if (src->entries[i].occupied == 1) {
                        vm_hash_set_n(cp, src->entries[i].key, src->entries[i].keylen,
                                      vm_val_copy(src->entries[i].value));
                    }
                }
                SP_PUSH(VM_MAKE_PTR(cp));
            } else if (argc >= 1 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_ARRAY) {
                VMArray *src = VM_AS_ARRAY(args[0]);
                VMHash *cp = vm_hash_new(src->size > 8 ? src->size : 16);
                if (src->size > 0 && VM_IS_PTR(src->items[0]) && VM_PTR_TYPE(src->items[0]) == VM_OBJ_ARRAY) {
                    for (int i = 0; i < src->size; i++) {
                        if (VM_IS_PTR(src->items[i]) && VM_PTR_TYPE(src->items[i]) == VM_OBJ_ARRAY) {
                            VMArray *pair = VM_AS_ARRAY(src->items[i]);
                            if (pair->size >= 2) {
                                char kbuf[256];
                                const char *k = vm_to_cstr(pair->items[0], kbuf, sizeof(kbuf));
                                vm_hash_set(cp, k, vm_val_copy(pair->items[1]));
                            }
                        }
                    }
                } else {
                    for (int i = 0; i + 1 < src->size; i += 2) {
                        char kbuf[256];
                        const char *k = vm_to_cstr(src->items[i], kbuf, sizeof(kbuf));
                        vm_hash_set(cp, k, vm_val_copy(src->items[i + 1]));
                    }
                }
                SP_PUSH(VM_MAKE_PTR(cp));
            } else {
                SP_PUSH(VM_MAKE_PTR(vm_hash_new(16)));
            }
            break;
        }
        case BUILTIN_REFTYPE: {
            if (argc >= 1 && VM_IS_PTR(args[0])) {
                switch (VM_PTR_TYPE(args[0])) {
                case VM_OBJ_ARRAY: SP_PUSH(vm_str("ARRAY")); break;
                case VM_OBJ_HASH: {
                    VMHash *h = VM_AS_HASH(args[0]);
                    SP_PUSH(vm_str(h->class_name ? h->class_name : "HASH"));
                    break;
                }
                case VM_OBJ_CLOSURE: SP_PUSH(vm_str("CODE")); break;
                case VM_OBJ_SREF: SP_PUSH(vm_str("SCALAR")); break;
                case VM_OBJ_NATIVE_SV: SP_PUSH(vm_str("REF")); break;
                default: SP_PUSH(vm_str("")); break;
                }
            } else {
                SP_PUSH(vm_str(""));
            }
            break;
        }
        case BUILTIN_DEREF: {
            if (argc >= 1) {
                if (VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_SREF) {
                    VMScalarRef *sr = VM_AS_SREF(args[0]);
                    if (sr->global_name) {
                        SP_PUSH(vm_val_copy(vm_hash_get(vm->globals, sr->global_name)));
                    } else if (VM_IS_PTR(sr->cell) && VM_PTR_TYPE(sr->cell) == VM_OBJ_CELL) {
                        SP_PUSH(vm_val_copy(VM_AS_CELL(sr->cell)->val));
                    } else {
                        SP_PUSH(VM_UNDEF_VAL);
                    }
                } else if (VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_CELL) {
                    SP_PUSH(vm_val_copy(VM_AS_CELL(args[0])->val));
                } else {
                    SP_PUSH(vm_val_copy(args[0]));
                }
            } else {
                SP_PUSH(VM_UNDEF_VAL);
            }
            break;
        }
        case BUILTIN_IS_REF:
        case BUILTIN_IS_REFTO: {
            int is_r = 0;
            if (argc >= 1 && VM_IS_PTR(args[0])) {
                enum VMObjType t = VM_PTR_TYPE(args[0]);
                if (t == VM_OBJ_ARRAY || t == VM_OBJ_HASH || t == VM_OBJ_CLOSURE ||
                    t == VM_OBJ_SREF || t == VM_OBJ_NATIVE_SV) {
                    is_r = 1;
                }
            }
            SP_PUSH(VM_MAKE_INT(is_r));
            break;
        }
        case BUILTIN_BYTE_INDEX: {
            if (argc >= 2) {
                StradaValue *h_sv = vm_to_sv(args[0]);
                StradaValue *n_sv = vm_to_sv(args[1]);
                int64_t off = argc >= 3 ? vm_to_int(args[2]) : 0;
                SP_PUSH(VM_MAKE_INT(strada_byte_index(h_sv, n_sv, off)));
                strada_decref(h_sv);
                strada_decref(n_sv);
            } else SP_PUSH(VM_MAKE_INT(-1));
            break;
        }
        case BUILTIN_DUMPER_STR: {
            if (argc >= 1) {
                StradaValue *sv = vm_to_sv(args[0]);
                StradaValue *r = strada_dumper(sv);
                char *s = strada_to_str(r);
                SP_PUSH(vm_str(s));
                free(s); strada_decref(r); strada_decref(sv);
            } else SP_PUSH(vm_str(""));
            break;
        }
        case BUILTIN_NSORT: {
            /* Numeric sort */
            if (argc >= 1 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_ARRAY) {
                VMArray *src = VM_AS_ARRAY(args[0]);
                VMArray *sorted = vm_array_new(src->size > 0 ? src->size : 8);
                for (int i = 0; i < src->size; i++) vm_array_push(sorted, vm_val_copy(src->items[i]));
                /* Insertion sort by numeric value */
                for (int i = 1; i < sorted->size; i++) {
                    VMValue key = sorted->items[i];
                    double kv = vm_to_num(key);
                    int j = i - 1;
                    while (j >= 0 && vm_to_num(sorted->items[j]) > kv) {
                        sorted->items[j+1] = sorted->items[j]; j--;
                    }
                    sorted->items[j+1] = key;
                }
                SP_PUSH(VM_MAKE_PTR(sorted));
            } else SP_PUSH(VM_MAKE_PTR(vm_array_new(8)));
            break;
        }
        case BUILTIN_CAST_INT: {
            SP_PUSH(VM_MAKE_INT(vm_to_int(args[0])));
            break;
        }
        case BUILTIN_CAST_NUM: {
            VM_PUSH_DOUBLE(sp, vm_to_num(args[0]));
            break;
        }
        case BUILTIN_CAST_STR: {
            char buf[256];
            SP_PUSH(vm_str(vm_to_cstr(args[0], buf, 256)));
            break;
        }

#undef RT_CALL_0
#undef RT_CALL_1
#undef RT_CALL_2
#undef RT_CALL_3
#undef RT_VOID_1
#undef RT_VOID_2

        default:
            fprintf(stderr, "VM: unknown builtin %d\n", bid);
            SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        /* Only free string temporaries — containers/fh/closures are shared refs */
        for (int i = 0; i < argc; i++) {
            if (VM_IS_PTR(args[i]) && (VM_PTR_TYPE(args[i]) == VM_OBJ_STR || VM_PTR_TYPE(args[i]) == VM_OBJ_STRBUF))
                vm_val_free(&args[i]);
        }
        if (args != args_stk) free(args);
        DISPATCH();
    }

    CASE(OP_CALL_NATIVE): {
        uint16_t native_idx = read_u16(ip); ip += 2;
        uint8_t argc = *ip++;
        VMNativeEntry *ne = &vm->program->natives[native_idx];
        VMValue raw_buf[16];
        VMValue *raw_args = (argc > 16) ? malloc((size_t)argc * sizeof(VMValue)) : raw_buf;
        for (int i = argc - 1; i >= 0; i--) raw_args[i] = SP_POP();
        VMValue ret = vm_invoke_native_entry(vm, ne, raw_args, argc, NULL);
        if (raw_args != raw_buf) free(raw_args);
        SP_PUSH(ret);
        DISPATCH();
    }

    CASE(OP_C_BLOCK): {
        uint16_t idx = read_u16(ip); ip += 2;
        if (__builtin_expect(idx >= vm->program->cblock_count, 0)) {
            fprintf(stderr, "VM error: cblock index %u out of bounds (%d)\n",
                    (unsigned)idx, vm->program->cblock_count);
            vm->stack_top = sp; vm->exec_base = saved_exec_base; VM_EXEC_RETURN(VM_UNDEF_VAL);
        }
        VMCBlock *cb = &vm->program->cblocks[idx];

        /* JIT-compile on first execution */
        if (!cb->fn) {
            if (cblock_jit_compile(cb, vm->program->runtime_include_path, vm->program->cblock_prelude) != 0) {
                fprintf(stderr, "VM: __C__ block JIT compilation failed\n");
                DISPATCH();
            }
        }

        /* Marshal VM locals → StradaValue* array */
        VMValue orig_buf[32];
        StradaValue *sv_buf[32];
        StradaValue *orig_sv_buf[32];
        VMValue *orig_vars = (cb->var_count > 32) ? malloc((size_t)cb->var_count * sizeof(VMValue)) : orig_buf;
        StradaValue **sv_vars = (cb->var_count > 32) ? malloc((size_t)cb->var_count * sizeof(StradaValue*)) : sv_buf;
        StradaValue **orig_sv = (cb->var_count > 32) ? malloc((size_t)cb->var_count * sizeof(StradaValue*)) : orig_sv_buf;
        for (int i = 0; i < cb->var_count; i++) {
            int slot = cb->var_slots[i];
            VMValue raw_v = locals[slot];
            if (VM_IS_PTR(raw_v) && VM_PTR_TYPE(raw_v) == VM_OBJ_CELL) raw_v = VM_AS_CELL(raw_v)->val;
            orig_vars[i] = raw_v;
            sv_vars[i] = vm_to_sv(orig_vars[i]);
            orig_sv[i] = sv_vars[i];
            if (VM_IS_PTR(orig_vars[i]) && VM_PTR_TYPE(orig_vars[i]) == VM_OBJ_FILEHANDLE &&
                sv_vars[i] && !STRADA_IS_TAGGED_INT(sv_vars[i])) {
                strada_incref(orig_sv[i]);
            }
        }

        /* Call the compiled C block */
        vm->gc_pause++;
        ((void(*)(StradaValue**))cb->fn)(sv_vars);
        vm->gc_pause--;

        /* Marshal back: StradaValue* → VMValue */
        for (int i = 0; i < cb->var_count; i++) {
            int slot = cb->var_slots[i];
            VMValue *dst = &locals[slot];
            if (VM_IS_PTR(*dst) && VM_PTR_TYPE(*dst) == VM_OBJ_CELL) dst = &VM_AS_CELL(*dst)->val;
            int is_fh = (VM_IS_PTR(orig_vars[i]) && VM_PTR_TYPE(orig_vars[i]) == VM_OBJ_FILEHANDLE);
            if (is_fh && sv_vars[i] == orig_sv[i]) {
                *dst = orig_vars[i];
                if (orig_sv[i] && !STRADA_IS_TAGGED_INT(orig_sv[i])) {
                    orig_sv[i]->value.fh = NULL;
                    strada_decref(orig_sv[i]); /* drop extra guard ref */
                    strada_decref(orig_sv[i]); /* drop vm_to_sv ref */
                }
            } else {
                if (is_fh && orig_sv[i] && !STRADA_IS_TAGGED_INT(orig_sv[i])) {
                    orig_sv[i]->value.fh = NULL;
                    strada_decref(orig_sv[i]); /* drop extra guard ref */
                }
                *dst = sv_to_vm(sv_vars[i]);
                if (sv_vars[i]) strada_decref(sv_vars[i]);
            }
        }
        if (orig_vars != orig_buf) { free(orig_vars); free(sv_vars); free(orig_sv); }

        DISPATCH();
    }

    CASE(OP_HALT): vm->stack_top = sp; vm->exec_base = saved_exec_base; VM_EXEC_RETURN(SP_POP());

#ifndef __GNUC__
    default:
        fprintf(stderr, "VM error: unknown opcode %d\n", *(ip-1));
        vm->exec_base = saved_exec_base;
        VM_EXEC_RETURN(VM_UNDEF_VAL);
    } }
#endif

    #undef SP_PUSH
    #undef SP_POP
    #undef RELOAD
    #undef DISPATCH
    #undef CASE
    #undef VM_EXEC_RETURN
    #undef VM_TRY_BIN_OV
}

void vm_run_globals_init(VM *vm) {
    if (!vm || !vm->program) return;
    int gidx = vm_program_find_func(vm->program, "__globals_init");
    if (gidx >= 0 && vm->program->begin_count > 0 && vm->program->begin_blocks[0] == gidx) {
        for (int i = 1; i < vm->program->begin_count; i++) {
            vm->program->begin_blocks[i - 1] = vm->program->begin_blocks[i];
        }
        vm->program->begin_count--;
        VMValue rv = vm_execute_call(vm, gidx, NULL, 0);
        vm_val_free(&rv);
    }
}

void vm_seed_globals(VM *vm, StradaValue *hash_sv, StradaValue *skip_hash_sv) {
    if (!vm || !vm->globals || !hash_sv || STRADA_IS_TAGGED_INT(hash_sv)) return;
    if (hash_sv->type == STRADA_REF && hash_sv->value.rv) hash_sv = hash_sv->value.rv;
    if (!hash_sv || STRADA_IS_TAGGED_INT(hash_sv) || hash_sv->type != STRADA_HASH || !hash_sv->value.hv || !hash_sv->value.hv->entries) return;
    if (skip_hash_sv && !STRADA_IS_TAGGED_INT(skip_hash_sv) && skip_hash_sv->type == STRADA_REF && skip_hash_sv->value.rv) {
        skip_hash_sv = skip_hash_sv->value.rv;
    }
    StradaHash *hv = hash_sv->value.hv;
    StradaHash *skip = (skip_hash_sv && !STRADA_IS_TAGGED_INT(skip_hash_sv) && skip_hash_sv->type == STRADA_HASH) ? skip_hash_sv->value.hv : NULL;
    VM *prev_vm = g_active_vm;
    g_active_vm = vm;
    for (size_t i = 0; i < hv->next_slot; i++) {
        if (!hv->entries[i].key) continue;
        const char *k = hv->entries[i].key->data;
        if (skip && strada_hash_exists(skip, k)) continue;
        vm_hash_set(vm->globals, k, sv_to_vm(hv->entries[i].value));
    }
    g_active_vm = prev_vm;
}

void vm_export_globals(VM *vm, StradaValue *hash_sv) {
    if (!vm || !vm->globals || !hash_sv || STRADA_IS_TAGGED_INT(hash_sv)) return;
    if (hash_sv->type == STRADA_REF && hash_sv->value.rv) hash_sv = hash_sv->value.rv;
    if (!hash_sv || STRADA_IS_TAGGED_INT(hash_sv) || hash_sv->type != STRADA_HASH || !hash_sv->value.hv) return;
    VM *prev_vm = g_active_vm;
    g_active_vm = vm;
    VMHash *g = vm->globals;
    for (int i = 0; i < g->capacity; i++) {
        if (g->entries[i].occupied == 1 && g->entries[i].key) {
            StradaValue *sv = vm_to_sv(g->entries[i].value);
            strada_hash_set_take(hash_sv->value.hv, g->entries[i].key, sv);
        }
    }
    g_active_vm = prev_vm;
}

/* strada_vm.c — Bytecode VM implementation
 * Tagged-pointer VMValue (8 bytes) with computed goto dispatch.
 * Full language support: closures, try/catch, OOP, regex, I/O, etc. */

#include "vm.h"
#include "../runtime/strada_runtime.h"
#include <stddef.h>
#include <stdio.h>
#include <string.h>
#include <ctype.h>
#include <math.h>
#include <time.h>
#include <sys/time.h>
#include <unistd.h>
#include <setjmp.h>
#include <limits.h>
#include <sys/stat.h>
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

#ifndef STRADA_NO_TLS
static __thread VM *g_active_vm = NULL;
static __thread VMProgram *g_compiling_prog = NULL;
#else
typedef struct {
    VM *active_vm;
    VMProgram *compiling_prog;
} VMThreadSlots;
static pthread_key_t g_vm_tls_key;
static pthread_once_t g_vm_tls_once = PTHREAD_ONCE_INIT;
static void vm_tls_destroy(void *p) { free(p); }
static void vm_tls_init(void) { pthread_key_create(&g_vm_tls_key, vm_tls_destroy); }
static VMThreadSlots *vm_tls_slots(void) {
    pthread_once(&g_vm_tls_once, vm_tls_init);
    VMThreadSlots *s = (VMThreadSlots *)pthread_getspecific(g_vm_tls_key);
    if (!s) {
        s = calloc(1, sizeof(VMThreadSlots));
        pthread_setspecific(g_vm_tls_key, s);
    }
    return s;
}
#define g_active_vm (vm_tls_slots()->active_vm)
#define g_compiling_prog (vm_tls_slots()->compiling_prog)
#endif

static VMObjHeader *g_orphan_objects = NULL;
static pthread_mutex_t g_orphan_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_cblock_jit_mutex = PTHREAD_MUTEX_INITIALIZER;
#define VM_LOCALS_POOL_CAP 16384

static void vm_regex_cache_free(VM *vm);

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
    hdr->is_utf8 = 0;
    hdr->gc_finalized = 0;
    if (g_active_vm) {
        VM *avm = g_active_vm;
        hdr->gc_immortal = 0;
        if (__builtin_expect(avm->is_worker_vm, 0)) pthread_mutex_lock(&avm->gc_mutex);
        hdr->gc_next = avm->gc_objects;
        avm->gc_objects = hdr;
        avm->gc_count++;
        if (__builtin_expect(avm->is_worker_vm, 0)) pthread_mutex_unlock(&avm->gc_mutex);
    } else if (g_compiling_prog) {
        hdr->gc_immortal = 1;
        hdr->gc_next = g_compiling_prog->compile_objects;
        g_compiling_prog->compile_objects = hdr;
    } else {
        hdr->gc_immortal = 1;
        pthread_mutex_lock(&g_orphan_mutex);
        hdr->gc_next = g_orphan_objects;
        g_orphan_objects = hdr;
        pthread_mutex_unlock(&g_orphan_mutex);
    }
}

static void vm_gc_free_one(VMObjHeader *hdr) {
    if (!hdr) return;
    switch (hdr->obj_type) {
    case VM_OBJ_STR:
    case VM_OBJ_INT64:
    case VM_OBJ_UINT64:
    case VM_OBJ_CELL:
    case VM_OBJ_TIED_SCALAR:
    case VM_OBJ_WEAKREF:
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
        free(a->holes);
        free(a->class_name);
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
        free(c->class_name);
        free(c);
        break;
    }
    case VM_OBJ_FILEHANDLE: {
        VMFileHandle *fh = (VMFileHandle*)hdr;
        if (fh->owner_sv) {
            if (!fh->fp) fh->owner_sv->value.fh = NULL;
            strada_decref(fh->owner_sv);
            fh->owner_sv = NULL;
            fh->fp = NULL;
        } else if (fh->fp) {
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
        free(sr->class_name);
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

VMValue vm_box_int64(int64_t v) {
    VMInt64 *b = malloc(sizeof(VMInt64));
    vm_gc_init_hdr(&b->hdr, VM_OBJ_INT64);
    b->val = v;
    return VM_MAKE_PTR(b);
}

static VMValue vm_box_uint64(uint64_t v) {
    if (v <= (uint64_t)INT64_MAX) return VM_MAKE_INT((int64_t)v);
    VMUInt64 *b = malloc(sizeof(VMUInt64));
    vm_gc_init_hdr(&b->hdr, VM_OBJ_UINT64);
    b->val = v;
    return VM_MAKE_PTR(b);
}

static inline int vm_str_is_float_num(const char *s) {
    if (!s) return 0;
    const char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\v' || *p == '\f') p++;
    if (*p == '+' || *p == '-') p++;
    if (!*p) return 0;
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X' ||
                        p[1] == 'b' || p[1] == 'B' ||
                        p[1] == 'o' || p[1] == 'O')) {
        return 0;
    }
    if ((p[0] == 'i' || p[0] == 'I') && (p[1] == 'n' || p[1] == 'N') && (p[2] == 'f' || p[2] == 'F')) {
        return 1;
    }
    if ((p[0] == 'n' || p[0] == 'N') && (p[1] == 'a' || p[1] == 'A') && (p[2] == 'n' || p[2] == 'N')) {
        return 1;
    }
    int saw_digit = 0;
    while (*p >= '0' && *p <= '9') { saw_digit = 1; p++; }
    if (*p == '.') {
        p++;
        while (*p >= '0' && *p <= '9') { saw_digit = 1; p++; }
        return saw_digit ? 1 : 0;
    }
    if (!saw_digit) return 0;
    if (*p == 'e' || *p == 'E') {
        p++;
        if (*p == '+' || *p == '-') p++;
        if (*p >= '0' && *p <= '9') return 1;
    }
    return 0;
}

static inline double vm_perl_atof(const char *s) {
    if (!s) return 0.0;
    const char *p = s;
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\v' || *p == '\f') p++;
    const char *after_sign = p;
    if (*after_sign == '+' || *after_sign == '-') after_sign++;
    if (after_sign[0] == '0' && (after_sign[1] == 'x' || after_sign[1] == 'X')) {
        return 0.0;
    }
    return strtod(s, NULL);
}

static inline int vm_is_float_operand(VMValue v) {
    if (VM_IS_FLOAT(v)) return 1;
    if (!VM_IS_PTR(v)) return 0;
    enum VMObjType t = VM_PTR_TYPE(v);
    if (t == VM_OBJ_STR || t == VM_OBJ_STRBUF) {
        const char *s = (t == VM_OBJ_STR) ? VM_AS_STR(v)->data : VM_AS_STRBUF(v)->data;
        return vm_str_is_float_num(s);
    }
    if (t == VM_OBJ_NATIVE_SV) {
        VMNativeSV *nsv = (VMNativeSV*)VM_TO_PTR(v);
        if (nsv->sv && !STRADA_IS_TAGGED_INT(nsv->sv)) {
            if (nsv->sv->type == STRADA_NUM) return 1;
            if (nsv->sv->type == STRADA_STR && nsv->sv->value.pv)
                return vm_str_is_float_num(nsv->sv->value.pv);
        }
    }
    return 0;
}

static inline int64_t vm_to_int(VMValue v) {
    if (VM_IS_INT(v)) return VM_INT_VAL(v);
    if (VM_IS_FLOAT(v)) {
        double d = VM_FLOAT_VAL(v);
        return (isfinite(d) && d >= -9.22e18 && d < 9.22e18) ? (int64_t)d : 0;
    }
    if (VM_IS_PTR(v)) {
        switch (VM_PTR_TYPE(v)) {
        case VM_OBJ_INT64: return VM_AS_INT64(v)->val;
        case VM_OBJ_UINT64: return (int64_t)VM_AS_UINT64(v)->val;
        case VM_OBJ_STR: return atoll(VM_AS_STR(v)->data);
        case VM_OBJ_STRBUF: return VM_AS_STRBUF(v)->data ? atoll(VM_AS_STRBUF(v)->data) : 0;
        case VM_OBJ_ARRAY: return VM_AS_ARRAY(v)->size;
        case VM_OBJ_NATIVE_SV: return strada_to_int(((VMNativeSV*)VM_TO_PTR(v))->sv);
        default: break;
        }
    }
    return 0;
}

static inline double vm_to_num(VMValue v) {
    if (VM_IS_INT(v)) return (double)VM_INT_VAL(v);
    if (VM_IS_FLOAT(v)) return VM_FLOAT_VAL(v);
    if (VM_IS_PTR(v)) {
        switch (VM_PTR_TYPE(v)) {
        case VM_OBJ_INT64: return (double)VM_AS_INT64(v)->val;
        case VM_OBJ_UINT64: return (double)VM_AS_UINT64(v)->val;
        case VM_OBJ_STR: return vm_perl_atof(VM_AS_STR(v)->data);
        case VM_OBJ_STRBUF: return VM_AS_STRBUF(v)->data ? vm_perl_atof(VM_AS_STRBUF(v)->data) : 0.0;
        case VM_OBJ_ARRAY: return (double)VM_AS_ARRAY(v)->size;
        case VM_OBJ_HASH:
        case VM_OBJ_CLOSURE:
        case VM_OBJ_FILEHANDLE:
        case VM_OBJ_WEAKREF:
            return (double)(intptr_t)VM_TO_PTR(v);
        case VM_OBJ_SREF: {
            VMScalarRef *sr = VM_AS_SREF(v);
            return VM_IS_PTR(sr->cell) ? (double)(intptr_t)VM_TO_PTR(sr->cell) : (double)(intptr_t)sr;
        }
        case VM_OBJ_NATIVE_SV: return strada_to_num(((VMNativeSV*)VM_TO_PTR(v))->sv);
        default: break;
        }
    }
    return 0.0;
}

/* Push an unboxed double result */
#define VM_PUSH_DOUBLE(sp, result) SP_PUSH(VM_MAKE_FLOAT(result))

static inline const char *vm_obj_class_name(VMValue v) {
    if (!VM_IS_PTR(v)) return NULL;
    switch (VM_PTR_TYPE(v)) {
    case VM_OBJ_HASH: return VM_AS_HASH(v)->class_name;
    case VM_OBJ_ARRAY: return VM_AS_ARRAY(v)->class_name;
    case VM_OBJ_CLOSURE: return VM_AS_CLOSURE(v)->class_name;
    case VM_OBJ_SREF: return VM_AS_SREF(v)->class_name;
    case VM_OBJ_WEAKREF: return vm_obj_class_name(VM_AS_WEAKREF(v)->target);
    case VM_OBJ_NATIVE_SV: {
        StradaValue *sv = ((VMNativeSV*)VM_TO_PTR(v))->sv;
        return (sv && !STRADA_IS_TAGGED_INT(sv) && sv->meta) ? sv->meta->blessed_package : NULL;
    }
    default: return NULL;
    }
}

static inline int vm_to_bool(VMValue v) {
    if (VM_IS_INT(v)) return v != VM_TAG_INT_BASE;
    if (VM_IS_UNDEF(v)) return 0;
    if (VM_IS_FLOAT(v)) { double d = VM_FLOAT_VAL(v); return d != 0.0 && !isnan(d); }
    if (VM_IS_PTR(v)) {
        switch (VM_PTR_TYPE(v)) {
        case VM_OBJ_INT64: return VM_AS_INT64(v)->val != 0;
        case VM_OBJ_UINT64: return VM_AS_UINT64(v)->val != 0;
        case VM_OBJ_STR: {
            VMString *st = VM_AS_STR(v);
            return st->len > 0 && !(st->len == 1 && st->data[0] == '0');
        }
        case VM_OBJ_STRBUF: { VMStrBuf *sb = VM_AS_STRBUF(v); return sb->len > 0 && !(sb->len == 1 && sb->data[0] == '0'); }
        case VM_OBJ_ARRAY: {
            VMArray *a = VM_AS_ARRAY(v);
            return a->class_name != NULL || a->size > 0;
        }
        case VM_OBJ_HASH: {
            VMHash *h = VM_AS_HASH(v);
            return h->class_name != NULL || h->size > 0;
        }
        case VM_OBJ_WEAKREF: return vm_to_bool(VM_AS_WEAKREF(v)->target);
        default: return 1;
        }
    }
    return 0;
}

static const char *vm_to_cstr(VMValue v, char *buf, size_t bufsz) {
    if (VM_IS_INT(v)) { snprintf(buf, bufsz, "%lld", (long long)VM_INT_VAL(v)); return buf; }
    if (VM_IS_UNDEF(v)) return "";
    if (VM_IS_FLOAT(v)) {
        double nv = VM_FLOAT_VAL(v);
        if (isnan(nv)) { snprintf(buf, bufsz, "NaN"); return buf; }
        if (isinf(nv)) { snprintf(buf, bufsz, "%s", nv < 0 ? "-Inf" : "Inf"); return buf; }
        if (nv == 0.0) { snprintf(buf, bufsz, "0"); return buf; }
        if (nv > 0 && nv < 1.8446744073709552e19 && nv == floor(nv)) {
            snprintf(buf, bufsz, "%llu", (unsigned long long)nv);
            return buf;
        }
        snprintf(buf, bufsz, "%.15g", nv);
        return buf;
    }
    switch (VM_PTR_TYPE(v)) {
    case VM_OBJ_INT64: { snprintf(buf, bufsz, "%lld", (long long)VM_AS_INT64(v)->val); return buf; }
    case VM_OBJ_UINT64: { snprintf(buf, bufsz, "%llu", (unsigned long long)VM_AS_UINT64(v)->val); return buf; }
    case VM_OBJ_STR: return VM_AS_STR(v)->data;
    case VM_OBJ_STRBUF: return VM_AS_STRBUF(v)->data ? VM_AS_STRBUF(v)->data : "";
    case VM_OBJ_ARRAY: {
        VMArray *a = VM_AS_ARRAY(v);
        if (a->class_name) {
            snprintf(buf, bufsz, "%s=ARRAY(0x%lx)", a->class_name, (unsigned long)(uintptr_t)a);
        } else {
            snprintf(buf, bufsz, "%d", a->size);
        }
        return buf;
    }
    case VM_OBJ_HASH: {
        VMHash *h = VM_AS_HASH(v);
        if (h->class_name) {
            snprintf(buf, bufsz, "%s=HASH(0x%lx)", h->class_name, (unsigned long)(uintptr_t)h);
        } else {
            snprintf(buf, bufsz, "HASH(0x%lx)", (unsigned long)(uintptr_t)h);
        }
        return buf;
    }
    case VM_OBJ_CLOSURE: {
        VMClosure *c = VM_AS_CLOSURE(v);
        if (c->class_name) {
            snprintf(buf, bufsz, "%s=CODE(0x%lx)", c->class_name, (unsigned long)(uintptr_t)c);
        } else {
            snprintf(buf, bufsz, "CODE(0x%lx)", (unsigned long)(uintptr_t)c);
        }
        return buf;
    }
    case VM_OBJ_NATIVE_SV: {
        VMNativeSV *nsv = (VMNativeSV*)VM_TO_PTR(v);
        if (nsv->sv && !STRADA_IS_TAGGED_INT(nsv->sv) && nsv->sv->meta && nsv->sv->meta->blessed_package) {
            const char *kind = (nsv->sv->type == STRADA_ARRAY) ? "ARRAY" :
                               (nsv->sv->type == STRADA_REF) ? "SCALAR" : "HASH";
            snprintf(buf, bufsz, "%s=%s(0x%lx)", nsv->sv->meta->blessed_package, kind, (unsigned long)(uintptr_t)nsv->sv);
        } else {
            snprintf(buf, bufsz, "NativeSV(0x%lx)", (unsigned long)(uintptr_t)nsv->sv);
        }
        return buf;
    }
    case VM_OBJ_SREF: {
        VMScalarRef *sr = VM_AS_SREF(v);
        if (sr->class_name) {
            snprintf(buf, bufsz, "%s=SCALAR(0x%lx)", sr->class_name, (unsigned long)(uintptr_t)sr);
        } else {
            snprintf(buf, bufsz, "SCALAR(0x%lx)", (unsigned long)(uintptr_t)sr);
        }
        return buf;
    }
    case VM_OBJ_WEAKREF: return vm_to_cstr(VM_AS_WEAKREF(v)->target, buf, bufsz);
    default: return "";
    }
}

/* Pass-through copy for immutable values; mark StrBuf COW-shared before sharing */
static inline VMValue vm_val_copy(VMValue v) {
    if (!VM_IS_PTR(v)) return v;
    enum VMObjType t = VM_PTR_TYPE(v);
    if (t == VM_OBJ_WEAKREF) {
        v = VM_AS_WEAKREF(v)->target;
        if (!VM_IS_PTR(v)) return v;
        t = VM_PTR_TYPE(v);
    }
    if (t == VM_OBJ_STRBUF) {
        VM_AS_STRBUF(v)->shared = 1;
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

void vm_array_free(VMArray *a) { if (a) { free(a->items); free(a->holes); free(a->class_name); free(a); } }

void vm_array_push(VMArray *a, VMValue v) {
    if (a->size >= a->cap) {
        int old_cap = a->cap;
        int new_cap = a->cap < 1024 ? a->cap * 2 : a->cap + a->cap / 2;
        if (new_cap < 8) new_cap = 8;
        a->cap = new_cap;
        a->items = realloc(a->items, a->cap * sizeof(VMValue));
        if (a->holes) {
            a->holes = realloc(a->holes, (size_t)a->cap);
            if (a->cap > old_cap) memset(a->holes + old_cap, 0, (size_t)(a->cap - old_cap));
        }
    }
    if (a->holes) a->holes[a->size] = 0;
    a->items[a->size++] = v;
}

static VMValue *vm_array_slot_ptr(VMArray *a, int64_t idx) {
    if (!a) return NULL;
    if (idx < 0) idx = (int64_t)a->size + idx;
    if (idx < 0 || idx >= (int64_t)a->size) return NULL;
    return &a->items[idx];
}

VMValue vm_array_get(VMArray *a, int64_t idx) {
    VMValue *slot = vm_array_slot_ptr(a, idx);
    if (!slot) return VM_UNDEF_VAL;
    VMValue v = *slot;
    if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_WEAKREF) return VM_AS_WEAKREF(v)->target;
    return v;
}

void vm_array_set(VMArray *a, int64_t idx, VMValue v) {
    /* Negative index counts from the end (mirrors vm_array_get). Without this,
     * $arr[-1] = v wrote at a->items[negative] — an out-of-bounds heap write. */
    if (idx < 0) { idx = (int64_t)a->size + idx; if (idx < 0) return; }
    if (idx > (int64_t)(INT_MAX / 2)) return;
    int sidx = (int)idx;
    if (sidx >= a->cap) {
        int old_cap = a->cap;
        while (sidx >= a->cap) {
            int new_cap = a->cap < 1024 ? a->cap * 2 : a->cap + a->cap / 2;
            if (new_cap < 8) new_cap = 8;
            if (new_cap <= sidx) new_cap = sidx + 1;
            a->cap = new_cap;
        }
        a->items = realloc(a->items, a->cap * sizeof(VMValue));
        if (a->holes) {
            a->holes = realloc(a->holes, (size_t)a->cap);
            if (a->cap > old_cap) memset(a->holes + old_cap, 0, (size_t)(a->cap - old_cap));
        }
    }
    if (sidx > a->size && !a->holes) {
        a->holes = calloc((size_t)a->cap, sizeof(uint8_t));
    }
    while (a->size < sidx) {
        if (a->holes) a->holes[a->size] = 1;
        a->items[a->size++] = VM_UNDEF_VAL;
    }
    if (sidx >= a->size) a->size = sidx + 1;
    if (a->holes) a->holes[sidx] = 0;
    a->items[sidx] = v;
}

static VMValue vm_array_pop(VMArray *a) {
    if (a->size == 0) return VM_UNDEF_VAL;
    a->size--;
    if (a->holes) a->holes[a->size] = 0;
    VMValue v = a->items[a->size];
    if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_WEAKREF) return VM_AS_WEAKREF(v)->target;
    return v;
}

static VMValue vm_array_shift(VMArray *a) {
    if (a->size == 0) return VM_UNDEF_VAL;
    VMValue v = a->items[0];
    memmove(a->items, a->items + 1, (a->size - 1) * sizeof(VMValue));
    if (a->holes) {
        memmove(a->holes, a->holes + 1, (size_t)(a->size - 1));
        a->holes[a->size - 1] = 0;
    }
    a->size--;
    if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_WEAKREF) return VM_AS_WEAKREF(v)->target;
    return v;
}

static void vm_array_unshift(VMArray *a, VMValue v) {
    vm_array_push(a, VM_UNDEF_VAL); /* ensure space */
    memmove(a->items + 1, a->items, (a->size - 1) * sizeof(VMValue));
    a->items[0] = v;
    if (a->holes) {
        memmove(a->holes + 1, a->holes, (size_t)(a->size - 1));
        a->holes[0] = 0;
    }
}

/* ===== VMHash ===== */

static uint32_t vm_hash_fn_n(const char *key, size_t len) {
    uint32_t h = 5381;
    for (size_t i = 0; i < len; i++) h = ((h << 5) + h) ^ (uint8_t)key[i];
    return h;
}

static uint32_t vm_hash_fn(const char *key) {
    return vm_hash_fn_n(key, key ? strlen(key) : 0);
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

static void vm_hash_set_nh(VMHash *h, const char *key, size_t keylen, uint32_t hash, VMValue v) {
    if (!h || !key) return;
    if (h->size * 4 >= h->capacity * 3) vm_hash_grow(h);
    uint32_t klen = (uint32_t)keylen;
    uint32_t mask = h->capacity - 1;
    uint32_t idx = hash & mask;
    for (;;) {
        if (h->entries[idx].occupied != 1) {
            char *k = malloc((size_t)klen + 1);
            if (klen > 0) memcpy(k, key, klen);
            k[klen] = '\0';
            h->entries[idx].key = k;
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
    if (!h || !key) return;
    vm_hash_set_nh(h, key, keylen, vm_hash_fn_n(key, (uint32_t)keylen), v);
}

void vm_hash_set(VMHash *h, const char *key, VMValue v) {
    vm_hash_set_n(h, key, key ? strlen(key) : 0, v);
}

static VMValue *vm_hash_slot_ptr_nh(VMHash *h, const char *key, size_t keylen, uint32_t hash) {
    if (!h || !key) return NULL;
    uint32_t klen = (uint32_t)keylen;
    uint32_t mask = h->capacity - 1;
    uint32_t idx = hash & mask;
    for (;;) {
        if (h->entries[idx].occupied == 0) return NULL;
        if (h->entries[idx].occupied == 1 && h->entries[idx].hash == hash &&
            h->entries[idx].keylen == klen && memcmp(h->entries[idx].key, key, klen) == 0)
            return &h->entries[idx].value;
        idx = (idx + 1) & mask;
    }
}

static VMValue *vm_hash_slot_ptr_n(VMHash *h, const char *key, size_t keylen) {
    if (!h || !key) return NULL;
    return vm_hash_slot_ptr_nh(h, key, keylen, vm_hash_fn_n(key, (uint32_t)keylen));
}

static VMValue *vm_hash_slot_ptr(VMHash *h, const char *key) {
    return vm_hash_slot_ptr_n(h, key, key ? strlen(key) : 0);
}

static VMValue vm_hash_get_n(VMHash *h, const char *key, size_t keylen) {
    VMValue *slot = vm_hash_slot_ptr_n(h, key, keylen);
    if (!slot) return VM_UNDEF_VAL;
    VMValue v = *slot;
    if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_WEAKREF) return VM_AS_WEAKREF(v)->target;
    return v;
}

VMValue vm_hash_get(VMHash *h, const char *key) {
    return vm_hash_get_n(h, key, key ? strlen(key) : 0);
}

static inline void key_free(char *k) {
    free(k);
}

static VMValue vm_hash_delete_ret_n(VMHash *h, const char *key, size_t keylen) {
    if (!h || !key) return VM_UNDEF_VAL;
    uint32_t klen = (uint32_t)keylen;
    uint32_t hash = vm_hash_fn_n(key, klen);
    uint32_t mask = h->capacity - 1;
    uint32_t idx = hash & mask;
    for (;;) {
        if (h->entries[idx].occupied == 0) return VM_UNDEF_VAL;
        if (h->entries[idx].occupied == 1 && h->entries[idx].hash == hash &&
            h->entries[idx].keylen == klen && memcmp(h->entries[idx].key, key, klen) == 0) {
            VMValue old = h->entries[idx].value;
            if (VM_IS_PTR(old) && VM_PTR_TYPE(old) == VM_OBJ_WEAKREF) old = VM_AS_WEAKREF(old)->target;
            key_free(h->entries[idx].key); h->entries[idx].key = NULL;
            h->entries[idx].value = VM_UNDEF_VAL;
            h->entries[idx].occupied = 2;
            h->size--;
            return old;
        }
        idx = (idx + 1) & mask;
    }
}

VMValue vm_hash_delete_ret(VMHash *h, const char *key) {
    return vm_hash_delete_ret_n(h, key, key ? strlen(key) : 0);
}

static void vm_hash_delete_n(VMHash *h, const char *key, size_t keylen) {
    VMValue old = vm_hash_delete_ret_n(h, key, keylen);
    vm_val_free(&old);
}

void vm_hash_delete(VMHash *h, const char *key) {
    vm_hash_delete_n(h, key, key ? strlen(key) : 0);
}

static int vm_hash_exists_n(VMHash *h, const char *key, size_t keylen) {
    return vm_hash_slot_ptr_n(h, key, keylen) != NULL;
}

int vm_hash_exists(VMHash *h, const char *key) {
    return vm_hash_exists_n(h, key, key ? strlen(key) : 0);
}

VMArray *vm_hash_keys(VMHash *h) {
    VMArray *keys = vm_array_new(h->size > 0 ? h->size : 8);
    for (int i = 0; i < h->capacity; i++)
        if (h->entries[i].occupied == 1)
            vm_array_push(keys, VM_MAKE_STR_N(h->entries[i].key, h->entries[i].keylen));
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

static int vm_utf8_is_valid_str(const char *s, size_t len, int *out_nonascii);

size_t vm_chunk_add_str_const_len(VMChunk *c, const char *s, size_t len) {
    c->str_consts = realloc(c->str_consts, (c->str_const_count + 1) * sizeof(char*));
    c->str_const_lens = realloc(c->str_const_lens, (c->str_const_count + 1) * sizeof(size_t));
    c->str_const_vals = realloc(c->str_const_vals, (c->str_const_count + 1) * sizeof(VMValue));
    char *copy = malloc(len + 1);
    if (len > 0 && s) memcpy(copy, s, len);
    copy[len] = '\0';
    c->str_consts[c->str_const_count] = copy;
    c->str_const_lens[c->str_const_count] = len;

    VMConstString *cs = malloc(offsetof(VMConstString, str) + sizeof(VMString) + len + 1);
    cs->hash = vm_hash_fn_n(copy, len);
    cs->_pad = 0;
    cs->str.hdr.obj_type = VM_OBJ_STR;
    cs->str.hdr.gc_marked = 1;
    cs->str.hdr.gc_immortal = 2;
    int nonascii = 0;
    cs->str.hdr.is_utf8 = (uint8_t)(vm_utf8_is_valid_str(copy, len, &nonascii) && nonascii);
    cs->str.hdr.gc_finalized = 0;
    cs->str.hdr.gc_next = NULL;
    cs->str.len = (uint32_t)len;
    memcpy(cs->str.data, copy, len + 1);
    c->str_const_vals[c->str_const_count] = VM_MAKE_PTR(&cs->str);
    return c->str_const_count++;
}

size_t vm_chunk_add_str_const(VMChunk *c, const char *s) {
    return vm_chunk_add_str_const_len(c, s ? s : "", s ? strlen(s) : 0);
}

/* ===== VMProgram ===== */

VMProgram *vm_program_new(void) {
    VMProgram *p = calloc(1, sizeof(VMProgram));
    p->refcount = 1;
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

void vm_program_retain(VMProgram *p) {
    if (p) __sync_add_and_fetch(&p->refcount, 1);
}

void vm_program_free(VMProgram *p) {
    if (!p || __sync_sub_and_fetch(&p->refcount, 1) > 0) return;
    for (size_t i = 0; i < p->func_count; i++) {
        free(p->funcs[i].code); free(p->funcs[i].name); free(p->funcs[i].int_consts);
        for (size_t j = 0; j < p->funcs[i].str_const_count; j++) {
            free(p->funcs[i].str_consts[j]);
            if (p->funcs[i].str_const_vals && VM_IS_PTR(p->funcs[i].str_const_vals[j])) {
                free((char*)VM_TO_PTR(p->funcs[i].str_const_vals[j]) - offsetof(VMConstString, str));
            }
        }
        free(p->funcs[i].str_consts); free(p->funcs[i].str_const_lens); free(p->funcs[i].str_const_vals); free(p->func_names[i]);
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

static VM *vm_new_base(VMProgram *prog) {
    VM *vm = calloc(1, sizeof(VM));
    vm->stack_cap = 32768;
    vm->stack = calloc(vm->stack_cap, sizeof(VMValue));
    vm->frame_cap = 256;
    vm->frames = calloc(vm->frame_cap, sizeof(VMFrame));
    vm->locals_cap = VM_LOCALS_POOL_CAP;
    vm->locals_pool = calloc(vm->locals_cap, sizeof(VMValue));
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
    vm->gc_threshold = 4096;
    memset(vm->regex_captures, 0, sizeof(vm->regex_captures));
    memset(vm->regex_capture_len, 0, sizeof(vm->regex_capture_len));
    memset(vm->regex_capture_utf8, 0, sizeof(vm->regex_capture_utf8));
    vm->saved_active_vm = g_active_vm;
    vm->trap_uncaught = 0;
    vm->end_ran = 0;
    vm->refcount = 1;
    vm->locals_detached = 0;
    vm->owner_thread = pthread_self();
    pthread_mutex_init(&vm->gc_mutex, NULL);
    pthread_mutex_init(&vm->globals_mutex, NULL);
    g_active_vm = vm;
    return vm;
}

VM *vm_new(VMProgram *prog) {
    VM *vm = vm_new_base(prog);
    vm->globals = vm_hash_new(64);
    return vm;
}

static VM *vm_new_worker(VM *parent) {
    VM *root = (parent && parent->root_vm) ? parent->root_vm : parent;
    VM *vm = vm_new_base(root ? root->program : NULL);
    vm->is_worker_vm = 1;
    vm->root_vm = root;
    if (root) {
        vm_retain(root);
        vm->globals = root->globals;
        vm->default_fh = root->default_fh;
        vm->recursion_limit = root->recursion_limit;
        pthread_mutex_lock(&root->gc_mutex);
        vm->worker_next = root->workers_head;
        root->workers_head = vm;
        pthread_mutex_unlock(&root->gc_mutex);
    } else {
        vm->globals = vm_hash_new(64);
    }
    return vm;
}

static inline pthread_mutex_t *vm_globals_lock_ptr(VM *vm) {
    VM *owner = (vm && vm->root_vm) ? vm->root_vm : vm;
    return owner ? &owner->globals_mutex : NULL;
}

static inline VMValue vm_globals_get(VM *vm, const char *name) {
    pthread_mutex_t *lk = vm_globals_lock_ptr(vm);
    if (lk) pthread_mutex_lock(lk);
    VMValue r = (vm && vm->globals) ? vm_hash_get(vm->globals, name) : VM_UNDEF_VAL;
    if (lk) pthread_mutex_unlock(lk);
    return r;
}

static inline VMValue vm_globals_get_raw(VM *vm, const char *name) {
    pthread_mutex_t *lk = vm_globals_lock_ptr(vm);
    if (lk) pthread_mutex_lock(lk);
    VMValue *slot = (vm && vm->globals) ? vm_hash_slot_ptr(vm->globals, name) : NULL;
    VMValue r = slot ? *slot : VM_UNDEF_VAL;
    if (lk) pthread_mutex_unlock(lk);
    return r;
}

static inline void vm_globals_set(VM *vm, const char *name, VMValue v) {
    pthread_mutex_t *lk = vm_globals_lock_ptr(vm);
    if (lk) pthread_mutex_lock(lk);
    if (vm && vm->globals) vm_hash_set(vm->globals, name, v);
    if (lk) pthread_mutex_unlock(lk);
}

static inline void vm_globals_set_n(VM *vm, const char *name, size_t len, VMValue v) {
    pthread_mutex_t *lk = vm_globals_lock_ptr(vm);
    if (lk) pthread_mutex_lock(lk);
    if (vm && vm->globals) vm_hash_set_n(vm->globals, name, len, v);
    if (lk) pthread_mutex_unlock(lk);
}

static inline int vm_globals_exists(VM *vm, const char *name) {
    pthread_mutex_t *lk = vm_globals_lock_ptr(vm);
    if (lk) pthread_mutex_lock(lk);
    int r = (vm && vm->globals) ? vm_hash_exists(vm->globals, name) : 0;
    if (lk) pthread_mutex_unlock(lk);
    return r;
}

static inline void vm_globals_delete(VM *vm, const char *name) {
    pthread_mutex_t *lk = vm_globals_lock_ptr(vm);
    if (lk) pthread_mutex_lock(lk);
    if (vm && vm->globals) vm_hash_delete(vm->globals, name);
    if (lk) pthread_mutex_unlock(lk);
}

static inline VMArray *vm_globals_keys(VM *vm) {
    pthread_mutex_t *lk = vm_globals_lock_ptr(vm);
    if (lk) pthread_mutex_lock(lk);
    VMArray *r = (vm && vm->globals) ? vm_hash_keys(vm->globals) : vm_array_new(4);
    if (lk) pthread_mutex_unlock(lk);
    return r;
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

static int vm_find_method(VMProgram *prog, const char *class_name, const char *method);

typedef struct {
    VMObjHeader *obj;
    int dfidx;
} VMFinalizerItem;

static void vm_gc_mark_vm_roots(VM *tvm, VMObjHeader ***work, size_t *wlen, size_t *wcap) {
    if (!tvm) return;
    for (size_t i = 0; i < tvm->stack_top; i++) {
        vm_gc_mark_val(tvm->stack[i], work, wlen, wcap);
    }
    for (size_t i = 0; i < tvm->frame_count; i++) {
        VMFrame *f = &tvm->frames[i];
        if (f->locals && f->chunk) {
            int lc = f->chunk->local_count + 1;
            for (int j = 0; j < lc; j++) {
                vm_gc_mark_val(f->locals[j], work, wlen, wcap);
            }
        }
        if (f->closure) {
            vm_gc_mark_val(VM_MAKE_PTR(f->closure), work, wlen, wcap);
        }
    }
    for (int i = 0; i < tvm->global_save_count; i++) {
        vm_gc_mark_val(tvm->global_saves[i].saved_value, work, wlen, wcap);
        if (tvm->global_saves[i].is_hash_elem && tvm->global_saves[i].hash) {
            vm_gc_mark_val(VM_MAKE_PTR(tvm->global_saves[i].hash), work, wlen, wcap);
        }
    }
    vm_gc_mark_val(tvm->current_exception, work, wlen, wcap);
    vm_gc_mark_val(tvm->pending_exc, work, wlen, wcap);
    vm_gc_mark_val(tvm->default_fh, work, wlen, wcap);
    for (VMObjHeader *o = tvm->gc_objects; o; o = o->gc_next) {
        if (o->gc_immortal) {
            vm_gc_mark_val(VM_MAKE_PTR(o), work, wlen, wcap);
        }
    }
}

static void vm_gc_mark_all(VM *vm) {
    VMObjHeader **work = NULL;
    size_t wlen = 0, wcap = 0;

    vm_gc_mark_vm_roots(vm, &work, &wlen, &wcap);
    if (vm->globals) {
        vm_gc_mark_val(VM_MAKE_PTR(vm->globals), &work, &wlen, &wcap);
    }
    if (vm->argv) {
        vm_gc_mark_val(VM_MAKE_PTR(vm->argv), &work, &wlen, &wcap);
    }
    if (vm->program) {
        for (int i = 0; i < vm->program->attr_count; i++) {
            vm_gc_mark_val(vm->program->attrs[i].default_val, &work, &wlen, &wcap);
        }
    }
    for (VM *w = vm->workers_head; w; w = w->worker_next) {
        pthread_mutex_lock(&w->gc_mutex);
        vm_gc_mark_vm_roots(w, &work, &wlen, &wcap);
        pthread_mutex_unlock(&w->gc_mutex);
    }

    while (wlen > 0) {
        VMObjHeader *obj = work[--wlen];
        switch (obj->obj_type) {
        case VM_OBJ_ARRAY: {
            VMArray *a = (VMArray*)obj;
            vm_gc_mark_val(a->tied_obj, &work, &wlen, &wcap);
            for (int i = 0; i < a->size; i++) {
                vm_gc_mark_val(a->items[i], &work, &wlen, &wcap);
            }
            break;
        }
        case VM_OBJ_HASH: {
            VMHash *h = (VMHash*)obj;
            vm_gc_mark_val(h->tied_obj, &work, &wlen, &wcap);
            vm_gc_mark_val(h->tied_last_key, &work, &wlen, &wcap);
            for (int i = 0; i < h->capacity; i++) {
                if (h->entries[i].occupied == 1) {
                    vm_gc_mark_val(h->entries[i].value, &work, &wlen, &wcap);
                }
            }
            break;
        }
        case VM_OBJ_CLOSURE: {
            VMClosure *c = (VMClosure*)obj;
            if (c->has_bound_arg) {
                vm_gc_mark_val(c->bound_arg, &work, &wlen, &wcap);
            }
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
        case VM_OBJ_TIED_SCALAR: {
            VMTiedScalar *ts = (VMTiedScalar*)obj;
            vm_gc_mark_val(ts->tied_obj, &work, &wlen, &wcap);
            break;
        }
        case VM_OBJ_WEAKREF:
            /* Weak references never keep their targets alive */
            break;
        default:
            break;
        }
    }
    free(work);

    /* Scalar references (VMScalarRef) are proxy objects for a backing cell or global slot.
     * Keep any VMScalarRef alive while its backing variable is live so weak scalar refs
     * do not clear prematurely while the referent variable remains in scope. */
    int any_weak = vm->has_weakrefs;
    if (!any_weak) {
        for (VM *w = vm->workers_head; w; w = w->worker_next) {
            if (w->has_weakrefs) { any_weak = 1; break; }
        }
    }
    if (any_weak) {
        int changed = 1;
        while (changed) {
            changed = 0;
            for (VMObjHeader *o = vm->gc_objects; o; o = o->gc_next) {
                if (o->obj_type == VM_OBJ_SREF && !o->gc_marked && !o->gc_immortal) {
                    VMScalarRef *sr = (VMScalarRef*)o;
                    if (sr->global_name || (VM_IS_PTR(sr->cell) &&
                        (((VMObjHeader*)VM_TO_PTR(sr->cell))->gc_marked ||
                         ((VMObjHeader*)VM_TO_PTR(sr->cell))->gc_immortal))) {
                        o->gc_marked = 1;
                        changed = 1;
                    }
                }
            }
            for (VM *w = vm->workers_head; w; w = w->worker_next) {
                if (!w->has_weakrefs) continue;
                pthread_mutex_lock(&w->gc_mutex);
                for (VMObjHeader *o = w->gc_objects; o; o = o->gc_next) {
                    if (o->obj_type == VM_OBJ_SREF && !o->gc_marked && !o->gc_immortal) {
                        VMScalarRef *sr = (VMScalarRef*)o;
                        if (sr->global_name || (VM_IS_PTR(sr->cell) &&
                            (((VMObjHeader*)VM_TO_PTR(sr->cell))->gc_marked ||
                             ((VMObjHeader*)VM_TO_PTR(sr->cell))->gc_immortal))) {
                            o->gc_marked = 1;
                            changed = 1;
                        }
                    }
                }
                pthread_mutex_unlock(&w->gc_mutex);
            }
        }
    }
}

static void vm_gc_clear_weakrefs_list(VMObjHeader *head) {
    for (VMObjHeader *o = head; o; o = o->gc_next) {
        if (o->obj_type == VM_OBJ_WEAKREF) {
            VMWeakRef *wr = (VMWeakRef*)o;
            if (VM_IS_PTR(wr->target)) {
                VMObjHeader *th = (VMObjHeader*)VM_TO_PTR(wr->target);
                if (!th->gc_marked && !th->gc_immortal) {
                    wr->target = VM_UNDEF_VAL;
                }
            }
        }
    }
}

static void vm_gc_clear_weakrefs(VM *vm) {
    if (vm->has_weakrefs) {
        vm_gc_clear_weakrefs_list(vm->gc_objects);
    }
    for (VM *w = vm->workers_head; w; w = w->worker_next) {
        if (!w->has_weakrefs) continue;
        pthread_mutex_lock(&w->gc_mutex);
        vm_gc_clear_weakrefs_list(w->gc_objects);
        pthread_mutex_unlock(&w->gc_mutex);
    }
}

static void vm_gc_reset_worker_marks(VM *vm) {
    for (VM *w = vm->workers_head; w; w = w->worker_next) {
        pthread_mutex_lock(&w->gc_mutex);
        for (VMObjHeader *o = w->gc_objects; o; o = o->gc_next) {
            o->gc_marked = 0;
        }
        pthread_mutex_unlock(&w->gc_mutex);
    }
}

static void vm_run_destroy_batch_ex(VM *vm, VMFinalizerItem *items, size_t count, int propagate_try) {
    if (!vm || !items || count == 0) return;
    vm->gc_pause++;
    int saved_trap = vm->trap_uncaught;
    int saved_has_exc = vm->has_exception;
    VMValue saved_exc = vm->current_exception;
    int saved_has_p = vm->has_pending_exc;
    VMValue saved_p = vm->pending_exc;
    uint8_t saved_ctx = vm->pending_call_context;
    size_t saved_sp = vm->stack_top;
    int saved_exc_top = vm->exc_top;
    int has_first_p = 0;
    VMValue first_p = VM_UNDEF_VAL;

    vm->trap_uncaught = 1;
    for (size_t i = 0; i < count; i++) {
        vm->has_exception = 0;
        vm->current_exception = VM_UNDEF_VAL;
        vm->has_pending_exc = 0;
        vm->pending_exc = VM_UNDEF_VAL;
        vm->pending_call_context = 0;
        vm->stack_top = saved_sp;
        VMValue self_val = VM_MAKE_PTR(items[i].obj);
        vm_execute_call(vm, items[i].dfidx, &self_val, 1);
        if (propagate_try && !has_first_p && vm->has_pending_exc) {
            has_first_p = 1;
            first_p = vm->pending_exc;
        }
        /* Discard any exception thrown by async GC destructor and restore state */
        vm->has_exception = 0;
        vm->current_exception = VM_UNDEF_VAL;
        vm->has_pending_exc = 0;
        vm->pending_exc = VM_UNDEF_VAL;
        vm->pending_call_context = 0;
        vm->stack_top = saved_sp;
        vm->exc_top = saved_exc_top;
    }

    vm->trap_uncaught = saved_trap;
    vm->has_exception = saved_has_exc;
    vm->current_exception = saved_exc;
    if (propagate_try && has_first_p && !saved_has_p) {
        vm->has_pending_exc = 1;
        vm->pending_exc = first_p;
    } else {
        vm->has_pending_exc = saved_has_p;
        vm->pending_exc = saved_p;
    }
    vm->pending_call_context = saved_ctx;
    vm->gc_pause--;
}

static void vm_run_destroy_batch(VM *vm, VMFinalizerItem *items, size_t count) {
    vm_run_destroy_batch_ex(vm, items, count, 0);
}

static size_t vm_collect_finalizer_items(VM *vm, VMFinalizerItem **out_items, int only_unmarked) {
    *out_items = NULL;
    if (!vm || !vm->program) return 0;
    VMFinalizerItem *items = NULL;
    size_t count = 0, cap = 0;
    for (VMObjHeader *o = vm->gc_objects; o; o = o->gc_next) {
        if (only_unmarked && (o->gc_marked || o->gc_immortal)) continue;
        if (o->gc_finalized) continue;
        enum VMObjType ot = o->obj_type;
        if (ot != VM_OBJ_HASH && ot != VM_OBJ_ARRAY && ot != VM_OBJ_CLOSURE && ot != VM_OBJ_SREF) continue;
        const char *cname = vm_obj_class_name(VM_MAKE_PTR(o));
        if (!cname) continue;
        int dfidx = vm_find_method(vm->program, cname, "DESTROY");
        if (dfidx < 0) continue;
        o->gc_finalized = 1;
        if (count >= cap) {
            cap = (cap == 0) ? 16 : (cap * 2);
            items = realloc(items, cap * sizeof(VMFinalizerItem));
        }
        items[count].obj = o;
        items[count].dfidx = dfidx;
        count++;
    }
    *out_items = items;
    return count;
}

static void vm_gc_collect_ex(VM *vm, int propagate_try) {
    if (!vm || vm->is_worker_vm || vm->gc_pause > 0 || vm->exec_base > 0) return;

    pthread_mutex_lock(&vm->gc_mutex);
    pthread_mutex_lock(&vm->globals_mutex);
    vm_gc_mark_all(vm);
    vm_gc_clear_weakrefs(vm);
    VMFinalizerItem *fin = NULL;
    size_t fin_count = vm_collect_finalizer_items(vm, &fin, 1);
    pthread_mutex_unlock(&vm->globals_mutex);

    if (fin_count > 0) {
        pthread_mutex_unlock(&vm->gc_mutex);
        vm_run_destroy_batch_ex(vm, fin, fin_count, propagate_try);
        free(fin);
        pthread_mutex_lock(&vm->gc_mutex);
        pthread_mutex_lock(&vm->globals_mutex);
        for (VMObjHeader *o = vm->gc_objects; o; o = o->gc_next) {
            o->gc_marked = 0;
        }
        vm_gc_reset_worker_marks(vm);
        vm_gc_mark_all(vm);
        vm_gc_clear_weakrefs(vm);
        pthread_mutex_unlock(&vm->globals_mutex);
    }

    VMObjHeader *dead_list = NULL;
    VMObjHeader **pp = &vm->gc_objects;
    while (*pp) {
        VMObjHeader *hdr = *pp;
        if (hdr->gc_marked || hdr->gc_immortal) {
            hdr->gc_marked = 0;
            pp = &hdr->gc_next;
        } else {
            *pp = hdr->gc_next;
            hdr->gc_next = dead_list;
            dead_list = hdr;
            vm->gc_count--;
        }
    }
    vm_gc_reset_worker_marks(vm);
    pthread_mutex_unlock(&vm->gc_mutex);

    while (dead_list) {
        VMObjHeader *next = dead_list->gc_next;
        vm_gc_free_one(dead_list);
        dead_list = next;
    }
    vm->gc_threshold = (vm->gc_count < 2048) ? 4096 : (vm->gc_count * 2);
}

void vm_gc_collect(VM *vm) {
    vm_gc_collect_ex(vm, 0);
}

/* ===== Locals pool cleanup needed by vm_free ===== */

void vm_retain(VM *vm) {
    if (vm) __sync_add_and_fetch(&vm->refcount, 1);
}

void vm_suspend(VM *vm) {
    if (!vm) return;
    if (vm->exec_base == 0 && !vm->locals_detached) {
        vm->locals_detached = 1;
    }
    if (g_active_vm == vm) {
        g_active_vm = vm->saved_active_vm;
    }
}

static void vm_method_cache_clear(VM *vm) {
    if (!vm) return;
    for (int i = 0; i < 32; i++) {
        free(vm->method_cache[i].class_name);
        free(vm->method_cache[i].method);
        vm->method_cache[i].class_name = NULL;
        vm->method_cache[i].method = NULL;
        vm->method_cache[i].func_idx = 0;
    }
    for (int i = 0; i < 256; i++) {
        free(vm->method_ic[i].class_name);
        vm->method_ic[i].ip = NULL;
        vm->method_ic[i].class_name = NULL;
        vm->method_ic[i].func_idx = 0;
    }
}

void vm_free(VM *vm) {
    if (!vm) return;
    if (__sync_sub_and_fetch(&vm->refcount, 1) > 0) return;
    if (!vm->is_worker_vm && vm->program && vm->gc_objects && vm->frame_count == 0 && vm->exec_base == 0 &&
        vm->global_save_count == 0 && vm->gc_pause == 0) {
        for (int pass = 0; pass < 4; pass++) {
            VMFinalizerItem *fin = NULL;
            size_t fin_count = vm_collect_finalizer_items(vm, &fin, 0);
            if (fin_count == 0) break;
            vm_run_destroy_batch(vm, fin, fin_count);
            free(fin);
        }
    }
    vm_suspend(vm);
    if (vm->is_worker_vm && vm->root_vm) {
        VM *root_vm = vm->root_vm;
        pthread_mutex_lock(&root_vm->gc_mutex);
        VM **wp = &root_vm->workers_head;
        while (*wp) {
            if (*wp == vm) {
                *wp = vm->worker_next;
                break;
            }
            wp = &(*wp)->worker_next;
        }
        vm->worker_next = NULL;
        if (vm->has_weakrefs) {
            root_vm->has_weakrefs = 1;
        }
        if (vm->gc_objects) {
            VMObjHeader *tail = vm->gc_objects;
            size_t cnt = 1;
            tail->gc_marked = 0;
            while (tail->gc_next) {
                tail = tail->gc_next;
                tail->gc_marked = 0;
                cnt++;
            }
            tail->gc_next = root_vm->gc_objects;
            root_vm->gc_objects = vm->gc_objects;
            root_vm->gc_count += cnt;
            vm->gc_objects = NULL;
            vm->gc_count = 0;
        }
        pthread_mutex_unlock(&root_vm->gc_mutex);
    } else {
        pthread_mutex_lock(&vm->gc_mutex);
        VMObjHeader *obj = vm->gc_objects;
        vm->gc_objects = NULL;
        vm->gc_count = 0;
        pthread_mutex_unlock(&vm->gc_mutex);
        while (obj) {
            VMObjHeader *next = obj->gc_next;
            vm_gc_free_one(obj);
            obj = next;
        }
    }

    free(vm->stack);
    for (size_t i = 0; i < vm->frame_count; i++) {
        VMValue *lp = vm->frames[i].locals;
        if (lp && (!vm->locals_pool || lp < vm->locals_pool || lp >= vm->locals_pool + vm->locals_cap)) {
            free(lp);
        }
    }
    free(vm->locals_pool);
    vm->locals_pool = NULL;
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
    vm_method_cache_clear(vm);
    vm_regex_cache_free(vm);
    pthread_mutex_destroy(&vm->gc_mutex);
    pthread_mutex_destroy(&vm->globals_mutex);
    VM *root = vm->root_vm;
    free(vm);
    if (root) vm_free(root);
}

static inline VMValue *vm_locals_alloc(VM *avm, int count, int zero_from) {
    if (count <= 0) count = 1;
    if (__builtin_expect(avm != NULL, 1)) {
        if (__builtin_expect(!avm->locals_pool, 0)) {
            avm->locals_pool = calloc(VM_LOCALS_POOL_CAP, sizeof(VMValue));
            if (avm->locals_pool) avm->locals_cap = VM_LOCALS_POOL_CAP;
        }
        if (__builtin_expect(avm->locals_pool && avm->locals_top + (size_t)count <= avm->locals_cap, 1)) {
            VMValue *p = &avm->locals_pool[avm->locals_top];
            if (zero_from < 0) zero_from = 0;
            if (zero_from < count) {
                int rem = count - zero_from;
                if (rem == 1) p[zero_from] = 0;
                else if (rem == 2) { p[zero_from] = 0; p[zero_from + 1] = 0; }
                else memset(p + zero_from, 0, (size_t)rem * sizeof(VMValue));
            }
            avm->locals_top += (size_t)count;
            return p;
        }
    }
    return (VMValue*)calloc((size_t)count, sizeof(VMValue));
}

static inline void vm_locals_free(VM *avm, VMValue *p, int count) {
    if (!p) return;
    if (count <= 0) count = 1;
    if (__builtin_expect(avm && avm->locals_pool && p >= avm->locals_pool && p < avm->locals_pool + avm->locals_cap, 1)) {
        if (avm->locals_top >= (size_t)count && p == &avm->locals_pool[avm->locals_top - (size_t)count]) {
            avm->locals_top -= (size_t)count;
        }
        return;
    }
    if (avm && avm->is_worker_vm) {
        pthread_mutex_lock(&avm->gc_mutex);
        free(p);
        pthread_mutex_unlock(&avm->gc_mutex);
    } else {
        free(p);
    }
}

static void vm_frames_grow(VM *vm) {
    if (vm->is_worker_vm) pthread_mutex_lock(&vm->gc_mutex);
    vm->frame_cap *= 2;
    vm->frames = realloc(vm->frames, vm->frame_cap * sizeof(VMFrame));
    if (vm->is_worker_vm) pthread_mutex_unlock(&vm->gc_mutex);
}

static void vm_global_saves_grow(VM *vm) {
    if (vm->is_worker_vm) pthread_mutex_lock(&vm->gc_mutex);
    vm->global_save_cap *= 2;
    vm->global_saves = realloc(vm->global_saves, vm->global_save_cap * sizeof(VMGlobalSave));
    if (vm->is_worker_vm) pthread_mutex_unlock(&vm->gc_mutex);
}

static inline VMValue *locals_alloc(int count, int skip_zero) {
    (void)skip_zero;
    return vm_locals_alloc(g_active_vm, count, 0);
}

static inline void locals_free(VMValue *p, int count) {
    vm_locals_free(g_active_vm, p, count);
}

/* ===== Bytecode reading ===== */
static inline int8_t read_i8(uint8_t *ip) { return (int8_t)*ip; }
static inline uint16_t read_u16(uint8_t *ip) {
    uint16_t val;
    memcpy(&val, ip, sizeof(val));
    return val;
}
static inline int32_t read_i32(uint8_t *ip) {
    int32_t val;
    memcpy(&val, ip, sizeof(val));
    return val;
}
static inline int64_t read_i64(uint8_t *ip) {
    int64_t val;
    memcpy(&val, ip, sizeof(val));
    return val;
}
static inline double read_f64(uint8_t *ip) {
    double val;
    memcpy(&val, ip, sizeof(val));
    return val;
}
static inline uint32_t read_u32(uint8_t *ip) {
    uint32_t val;
    memcpy(&val, ip, sizeof(val));
    return val;
}

/* ===== Regex helpers ===== */

/* === Regex compilation cache === */
#define REGEX_CACHE_SIZE 64
typedef struct {
    char *pattern;         /* heap-copied pattern bytes (owned) */
    size_t pat_len;
    uint32_t hash;
    uint32_t opts;
#ifdef HAVE_PCRE2
    pcre2_code *compiled;
    pcre2_match_data *match_data;
#else
    regex_t compiled;
    int valid;
#endif
} RegexCacheEntry;

static void vm_regex_cache_free(VM *vm) {
    if (!vm || !vm->regex_cache) return;
    RegexCacheEntry *cache = (RegexCacheEntry *)vm->regex_cache;
    for (int i = 0; i < REGEX_CACHE_SIZE; i++) {
        RegexCacheEntry *e = &cache[i];
        if (e->pattern) { free(e->pattern); e->pattern = NULL; }
#ifdef HAVE_PCRE2
        if (e->compiled) { pcre2_code_free(e->compiled); e->compiled = NULL; }
        if (e->match_data) { pcre2_match_data_free(e->match_data); e->match_data = NULL; }
#else
        if (e->valid) { regfree(&e->compiled); e->valid = 0; }
#endif
    }
    free(cache);
    vm->regex_cache = NULL;
}

static void __attribute__((destructor)) vm_static_cleanup(void) {
    pthread_mutex_lock(&g_orphan_mutex);
    VMObjHeader *o = g_orphan_objects;
    g_orphan_objects = NULL;
    pthread_mutex_unlock(&g_orphan_mutex);
    while (o) {
        VMObjHeader *next = o->gc_next;
        vm_gc_free_one(o);
        o = next;
    }
}

#ifdef HAVE_PCRE2
static uint32_t vm_hash_bytes_opts(const char *s, size_t len, uint32_t opts) {
    uint32_t h = 2166136261u ^ opts;
    for (size_t i = 0; i < len; i++) {
        h ^= (unsigned char)s[i];
        h *= 16777619u;
    }
    return h;
}

static inline pcre2_code *regex_cache_get(VM *vm, const char *pattern, size_t pat_len, uint32_t opts,
                                          pcre2_match_data **md_out, int *need_free_out) {
    *need_free_out = 0;
    if (!pattern) { *md_out = NULL; return NULL; }
    if (!vm) vm = g_active_vm;
    if (!vm) {
        int errcode; PCRE2_SIZE erroff;
        pcre2_code *re = pcre2_compile((PCRE2_SPTR)pattern, (PCRE2_SIZE)pat_len, opts, &errcode, &erroff, NULL);
        if (!re) { *md_out = NULL; return NULL; }
        *md_out = pcre2_match_data_create_from_pattern(re, NULL);
        *need_free_out = 1;
        return re;
    }
    RegexCacheEntry *cache = (RegexCacheEntry *)vm->regex_cache;
    if (!cache) {
        cache = calloc(REGEX_CACHE_SIZE, sizeof(RegexCacheEntry));
        if (!cache) { *md_out = NULL; return NULL; }
        vm->regex_cache = cache;
    }
    uint32_t h = vm_hash_bytes_opts(pattern, pat_len, opts);
    uint32_t idx = h & (REGEX_CACHE_SIZE - 1);
    RegexCacheEntry *e = &cache[idx];
    if (e->pattern && e->hash == h && e->opts == opts && e->pat_len == pat_len &&
        (pat_len == 0 || memcmp(e->pattern, pattern, pat_len) == 0)) {
        *md_out = e->match_data;
        return e->compiled;
    }
    /* Evict old entry */
    if (e->pattern) { free(e->pattern); e->pattern = NULL; }
    if (e->compiled) { pcre2_code_free(e->compiled); pcre2_match_data_free(e->match_data); e->compiled = NULL; e->match_data = NULL; }
    int errcode; PCRE2_SIZE erroff;
    pcre2_code *re = pcre2_compile((PCRE2_SPTR)pattern, (PCRE2_SIZE)pat_len, opts, &errcode, &erroff, NULL);
    if (!re) { *md_out = NULL; return NULL; }
    e->pattern = malloc(pat_len + 1);
    if (e->pattern) {
        if (pat_len > 0) memcpy(e->pattern, pattern, pat_len);
        e->pattern[pat_len] = '\0';
    }
    e->pat_len = pat_len;
    e->hash = h;
    e->opts = opts;
    e->compiled = re;
    e->match_data = pcre2_match_data_create_from_pattern(re, NULL);
    *md_out = e->match_data;
    return re;
}
#endif

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

static inline int vm_is_utf8(VMValue v) {
    if (!VM_IS_PTR(v)) return 0;
    enum VMObjType t = VM_PTR_TYPE(v);
    if (t == VM_OBJ_STR || t == VM_OBJ_STRBUF) {
        return ((VMObjHeader*)VM_TO_PTR(v))->is_utf8 != 0;
    }
    return 0;
}

static inline VMValue vm_make_str_flag(const char *s, size_t len, int is_utf8) {
    VMValue r = VM_MAKE_STR_N(s, len);
    if (is_utf8) VM_AS_STR(r)->hdr.is_utf8 = 1;
    return r;
}

static inline void vm_write_val(FILE *fp, VMValue v, int with_nl) {
    if (!fp) return;
    char buf[64];
    size_t len = 0;
    const char *s = vm_bytes_view(v, buf, sizeof(buf), &len);
    if (len > 0) fwrite(s, 1, len, fp);
    if (with_nl) fputc('\n', fp);
}

static void vm_clear_regex_captures(VM *vm) {
    for (int i = 0; i < 10; i++) {
        if (vm->regex_captures[i]) {
            free(vm->regex_captures[i]);
            vm->regex_captures[i] = NULL;
        }
        vm->regex_capture_len[i] = 0;
        vm->regex_capture_utf8[i] = 0;
        if (vm->regex_capture_names[i]) {
            free(vm->regex_capture_names[i]);
            vm->regex_capture_names[i] = NULL;
        }
    }
}

static void vm_store_regex_capture(VM *vm, int i, const char *s, size_t len, int is_utf8) {
    if (i < 0 || i >= 10) return;
    if (vm->regex_captures[i]) {
        free(vm->regex_captures[i]);
        vm->regex_captures[i] = NULL;
    }
    char *copy = malloc(len + 1);
    if (copy) {
        if (len > 0 && s) memcpy(copy, s, len);
        copy[len] = '\0';
    }
    vm->regex_captures[i] = copy;
    vm->regex_capture_len[i] = copy ? len : 0;
    vm->regex_capture_utf8[i] = (uint8_t)(is_utf8 ? 1 : 0);
}

static int vm_regex_match_len_u8(VM *vm, const char *str, size_t slen, int is_utf8, const char *pattern, size_t pat_len) {
#ifdef HAVE_PCRE2
    pcre2_match_data *md;
    int need_free = 0;
    uint32_t opts = is_utf8 ? (PCRE2_UTF | PCRE2_UCP) : 0;
    pcre2_code *re = regex_cache_get(vm, pattern, pat_len, opts, &md, &need_free);
    if (!re) return 0;
    int rc = pcre2_match(re, (PCRE2_SPTR)str, slen, 0, 0, md, NULL);
    if (rc > 0) {
        vm_clear_regex_captures(vm);
        PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
        int count = rc < 10 ? rc : 10;
        for (int i = 0; i < count; i++) {
            if (ov[2*i] != PCRE2_UNSET) {
                size_t len = ov[2*i+1] - ov[2*i];
                vm_store_regex_capture(vm, i, str + ov[2*i], len, is_utf8);
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
    if (need_free) {
        pcre2_match_data_free(md);
        pcre2_code_free(re);
    }
    return rc > 0;
#else
    (void)slen;
    (void)pat_len;
    regex_t re;
    if (regcomp(&re, pattern, REG_EXTENDED) != 0) return 0;
    regmatch_t matches[10];
    int rc = regexec(&re, str, 10, matches, 0);
    if (rc == 0) {
        vm_clear_regex_captures(vm);
        for (int i = 0; i < 10; i++) {
            if (matches[i].rm_so >= 0) {
                size_t len = matches[i].rm_eo - matches[i].rm_so;
                vm_store_regex_capture(vm, i, str + matches[i].rm_so, len, is_utf8);
            }
        }
    }
    regfree(&re);
    return rc == 0;
#endif
}

static int vm_regex_match_len(VM *vm, const char *str, size_t slen, const char *pattern) {
    return vm_regex_match_len_u8(vm, str, slen, 0, pattern, pattern ? strlen(pattern) : 0);
}

static int vm_regex_match(VM *vm, const char *str, const char *pattern) {
    return vm_regex_match_len_u8(vm, str, str ? strlen(str) : 0, 0, pattern, pattern ? strlen(pattern) : 0);
}

static void vm_sync_runtime_captures(VM *vm) {
    vm_clear_regex_captures(vm);
    StradaValue *caps = strada_captures();
    if (caps && !STRADA_IS_TAGGED_INT(caps) && caps->type == STRADA_ARRAY && caps->value.av) {
        size_t n = caps->value.av->size;
        if (n > 10) n = 10;
        for (size_t i = 0; i < n; i++) {
            StradaValue *el = strada_array_get(caps->value.av, (int64_t)i);
            if (el && (STRADA_IS_TAGGED_INT(el) || el->type != STRADA_UNDEF)) {
                if (!STRADA_IS_TAGGED_INT(el) && el->type == STRADA_STR && el->value.pv) {
                    vm_store_regex_capture(vm, (int)i, el->value.pv, STRADA_STR_BYTELEN(el), STRADA_STR_IS_UTF8(el));
                } else {
                    char *tmp = strada_to_str(el);
                    if (tmp) {
                        vm_store_regex_capture(vm, (int)i, tmp, strlen(tmp), 0);
                        free(tmp);
                    }
                }
            }
        }
    }
    if (caps) strada_decref(caps);
    StradaValue *ncaps = strada_named_captures();
    if (ncaps && !STRADA_IS_TAGGED_INT(ncaps) && ncaps->type == STRADA_HASH && ncaps->value.hv) {
        StradaHash *hv = ncaps->value.hv;
        for (size_t e = 0; e < hv->next_slot; e++) {
            if (!hv->entries[e].key) continue;
            StradaValue *ev = hv->entries[e].value;
            const char *vptr = NULL;
            size_t vlen = 0;
            char *vowned = NULL;
            if (ev && !STRADA_IS_TAGGED_INT(ev) && ev->type == STRADA_STR && ev->value.pv) {
                vptr = ev->value.pv;
                vlen = STRADA_STR_BYTELEN(ev);
            } else if (ev) {
                vowned = strada_to_str(ev);
                if (vowned) { vptr = vowned; vlen = strlen(vowned); }
            }
            if (vptr) {
                for (int i = 1; i < 10; i++) {
                    if (vm->regex_captures[i] && vm->regex_capture_len[i] == vlen &&
                        memcmp(vm->regex_captures[i], vptr, vlen) == 0 && !vm->regex_capture_names[i]) {
                        vm->regex_capture_names[i] = strdup(hv->entries[e].key->data);
                        break;
                    }
                }
            }
            if (vowned) free(vowned);
        }
    }
    if (ncaps) strada_decref(ncaps);
}

/* Find the next match of pattern in str at/after byte offset start.
 * On success sets vm->regex_captures and *mstart / *mend (byte offsets). */
static int vm_regex_find_u8(VM *vm, const char *str, size_t slen, int is_utf8, const char *pattern,
                            size_t pat_len, size_t start, size_t *mstart, size_t *mend) {
    if (start > slen) return 0;
#ifdef HAVE_PCRE2
    pcre2_match_data *md;
    int need_free = 0;
    uint32_t opts = is_utf8 ? (PCRE2_UTF | PCRE2_UCP) : 0;
    pcre2_code *re = regex_cache_get(vm, pattern, pat_len, opts, &md, &need_free);
    if (!re) return 0;
    int rc = pcre2_match(re, (PCRE2_SPTR)str, slen, start, 0, md, NULL);
    if (rc > 0) {
        vm_clear_regex_captures(vm);
        PCRE2_SIZE *ov = pcre2_get_ovector_pointer(md);
        int count = rc < 10 ? rc : 10;
        for (int i = 0; i < count; i++) {
            if (ov[2*i] != PCRE2_UNSET)
                vm_store_regex_capture(vm, i, str + ov[2*i], ov[2*i+1] - ov[2*i], is_utf8);
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
        if (need_free) {
            pcre2_match_data_free(md);
            pcre2_code_free(re);
        }
        return 1;
    }
    if (need_free) {
        pcre2_match_data_free(md);
        pcre2_code_free(re);
    }
    return 0;
#else
    (void)pat_len;
    regex_t re;
    if (regcomp(&re, pattern, REG_EXTENDED) != 0) return 0;
    regmatch_t matches[10];
    int rc = regexec(&re, str + start, 10, matches, start > 0 ? REG_NOTBOL : 0);
    if (rc == 0) {
        vm_clear_regex_captures(vm);
        for (int i = 0; i < 10; i++) {
            if (matches[i].rm_so >= 0)
                vm_store_regex_capture(vm, i, str + start + matches[i].rm_so,
                                       matches[i].rm_eo - matches[i].rm_so, is_utf8);
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
    int need_free = 0;
    pcre2_code *re = regex_cache_get(g_active_vm, pattern, pattern ? strlen(pattern) : 0, 0, &md, &need_free);
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
        if (need_free) { pcre2_match_data_free(md); pcre2_code_free(re); }
        return VM_MAKE_PTR(vs);
    }
    if (need_free) { pcre2_match_data_free(md); pcre2_code_free(re); }
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

static int vm_has_stringify(VMProgram *prog, VMValue v);
static VMValue vm_stringify_ov(VM *vm, VMValue v);
static int64_t vm_tied_array_fetchsize(VM *vm, VMValue tied_obj);
static VMValue sv_to_vm(StradaValue *sv);
static size_t vm_utf8_char_count(const char *s, size_t blen);
static size_t vm_utf8_char_to_byte(const char *s, size_t blen, int64_t char_idx);

static VMArray *vm_split_parts_to_array(StradaArray *parts) {
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
    return res;
}

static void vm_sprintf_append_bin(char **result_p, size_t *rcap_p, size_t *rlen_p,
                                  uint64_t val, const char *spec, char b_char) {
    char bin_buf[65];
    int bi = 64;
    bin_buf[bi] = '\0';
    if (val == 0) {
        bin_buf[--bi] = '0';
    } else {
        while (val > 0) {
            bin_buf[--bi] = (val & 1) ? '1' : '0';
            val >>= 1;
        }
    }
    const char *digits = bin_buf + bi;
    size_t dlen = (size_t)(64 - bi);
    const char *fp = spec;
    if (*fp == '%') fp++;
    int has_hash = 0, zero_pad = 0, left_align = 0;
    while (*fp == '0' || *fp == '-' || *fp == '+' || *fp == ' ' || *fp == '#') {
        if (*fp == '#') has_hash = 1;
        if (*fp == '0') zero_pad = 1;
        if (*fp == '-') left_align = 1;
        fp++;
    }
    int width = 0;
    while (*fp >= '0' && *fp <= '9') {
        if (width > (INT_MAX - (*fp - '0')) / 10) {
            width = INT_MAX;
            while (*fp >= '0' && *fp <= '9') fp++;
            break;
        }
        width = width * 10 + (*fp - '0');
        fp++;
    }
    if (left_align) zero_pad = 0;
    size_t prefix_len = has_hash ? 2 : 0;
    size_t total = prefix_len + dlen;
    size_t pad = (width > 0 && total < (size_t)width) ? ((size_t)width - total) : 0;
    size_t need = total + pad;
    char *result = *result_p;
    size_t rcap = *rcap_p, rlen = *rlen_p;
    VM_SPRINTF_ENSURE(need);
    if (pad > 0 && left_align) {
        if (has_hash) { result[rlen++] = '0'; result[rlen++] = b_char; }
        memcpy(result + rlen, digits, dlen); rlen += dlen;
        memset(result + rlen, ' ', pad); rlen += pad;
    } else if (pad > 0 && zero_pad) {
        if (has_hash) { result[rlen++] = '0'; result[rlen++] = b_char; }
        memset(result + rlen, '0', pad); rlen += pad;
        memcpy(result + rlen, digits, dlen); rlen += dlen;
    } else {
        if (pad > 0) { memset(result + rlen, ' ', pad); rlen += pad; }
        if (has_hash) { result[rlen++] = '0'; result[rlen++] = b_char; }
        memcpy(result + rlen, digits, dlen); rlen += dlen;
    }
    *result_p = result;
    *rcap_p = rcap;
    *rlen_p = rlen;
}

static VMValue vm_sprintf(VM *vm, int argc, VMValue *args) {
    if (argc < 1) return vm_str("");
    VMValue fmt_val = args[0];
    if (vm && vm_has_stringify(vm->program, fmt_val)) {
        fmt_val = vm_stringify_ov(vm, fmt_val);
        if (vm->has_pending_exc) return VM_UNDEF_VAL;
    }
    char fbuf[64];
    size_t flen = 0;
    const char *fmt = vm_bytes_view(fmt_val, fbuf, sizeof(fbuf), &flen);
    int out_utf8 = vm_is_utf8(fmt_val);

    size_t rcap = 256, rlen = 0;
    char *result = (char*)malloc(rcap);
    int ai = 1;

    for (size_t fi = 0; fi < flen; ) {
        if (fmt[fi] == '%' && fi + 1 < flen) {
            fi++;
            if (fmt[fi] == '%') {
                VM_SPRINTF_ENSURE(1);
                result[rlen++] = '%';
                fi++;
                continue;
            }
            /* Optional POSIX/Perl positional index: %N$ */
            int positional = 0;
            int pos_idx = -1;
            if (fi < flen && fmt[fi] >= '0' && fmt[fi] <= '9') {
                size_t pc = fi;
                int pn = 0;
                while (pc < flen && fmt[pc] >= '0' && fmt[pc] <= '9') {
                    if (pn <= 100000000) pn = pn * 10 + (fmt[pc] - '0');
                    pc++;
                }
                if (pc < flen && fmt[pc] == '$') {
                    positional = 1;
                    pos_idx = pn;
                    fi = pc + 1;
                }
            }
            /* Optional Perl vector flag: v or *v */
            int vec_flag = 0;
            int has_vec_sep = 0;
            VMValue vec_sep_val = VM_UNDEF_VAL;
            if (fi < flen && fmt[fi] == 'v') {
                vec_flag = 1;
                fi++;
            } else if (fi + 1 < flen && fmt[fi] == '*' && fmt[fi + 1] == 'v') {
                vec_flag = 1;
                has_vec_sep = 1;
                fi += 2;
                if (ai < argc) vec_sep_val = args[ai++];
            }

            char spec[96]; int si = 0;
            spec[si++] = '%';
            while (fi < flen && strchr("-+ #0", fmt[fi]) && si < 24) spec[si++] = fmt[fi++];
            if (fi < flen && fmt[fi] == '*') {
                fi++;
                int wpos = -1;
                if (fi < flen && fmt[fi] >= '0' && fmt[fi] <= '9') {
                    size_t wc = fi;
                    int wn = 0;
                    while (wc < flen && fmt[wc] >= '0' && fmt[wc] <= '9') {
                        if (wn <= 100000000) wn = wn * 10 + (fmt[wc] - '0');
                        wc++;
                    }
                    if (wc < flen && fmt[wc] == '$') { wpos = wn; fi = wc + 1; }
                }
                VMValue wval = VM_UNDEF_VAL;
                if (wpos >= 0) {
                    if (wpos >= 1 && wpos < argc) wval = args[wpos];
                } else if (ai < argc) {
                    wval = args[ai++];
                }
                if (VM_IS_PTR(wval) && VM_PTR_TYPE(wval) == VM_OBJ_WEAKREF) {
                    wval = VM_AS_WEAKREF(wval)->target;
                }
                int star_w = (int)vm_to_int(wval);
                if (star_w < 0) {
                    int has_minus_flag = 0;
                    for (int k = 1; k < si; k++) {
                        if (spec[k] == '-') { has_minus_flag = 1; break; }
                    }
                    if (!has_minus_flag && si < 25) {
                        memmove(spec + 2, spec + 1, (size_t)(si - 1));
                        spec[1] = '-';
                        si++;
                    }
                    star_w = (star_w == INT_MIN) ? 0 : -star_w;
                }
                int avail = (int)sizeof(spec) - si - 24;
                if (avail > 1) {
                    int nw = snprintf(spec + si, (size_t)avail, "%d", star_w);
                    if (nw > 0) si += (nw < avail ? nw : avail - 1);
                }
            } else {
                while (fi < flen && isdigit((unsigned char)fmt[fi]) && si < 50) spec[si++] = fmt[fi++];
            }
            if (fi < flen && fmt[fi] == '.' && si < 70) {
                fi++;
                if (fi < flen && fmt[fi] == '*') {
                    fi++;
                    int ppos = -1;
                    if (fi < flen && fmt[fi] >= '0' && fmt[fi] <= '9') {
                        size_t pc = fi;
                        int pn = 0;
                        while (pc < flen && fmt[pc] >= '0' && fmt[pc] <= '9') {
                            if (pn <= 100000000) pn = pn * 10 + (fmt[pc] - '0');
                            pc++;
                        }
                        if (pc < flen && fmt[pc] == '$') { ppos = pn; fi = pc + 1; }
                    }
                    VMValue pval = VM_UNDEF_VAL;
                    if (ppos >= 0) {
                        if (ppos >= 1 && ppos < argc) pval = args[ppos];
                    } else if (ai < argc) {
                        pval = args[ai++];
                    }
                    if (VM_IS_PTR(pval) && VM_PTR_TYPE(pval) == VM_OBJ_WEAKREF) {
                        pval = VM_AS_WEAKREF(pval)->target;
                    }
                    int star_p = (int)vm_to_int(pval);
                    if (star_p >= 0) {
                        spec[si++] = '.';
                        int avail = (int)sizeof(spec) - si - 4;
                        if (avail > 1) {
                            int np = snprintf(spec + si, (size_t)avail, "%d", star_p);
                            if (np > 0) si += (np < avail ? np : avail - 1);
                        }
                    }
                } else {
                    spec[si++] = '.';
                    while (fi < flen && isdigit((unsigned char)fmt[fi]) && si < 84) spec[si++] = fmt[fi++];
                }
            }
            /* Strip C length modifiers only when followed by an integer-style
             * conversion (d/i/u/o/x/X/n) so e.g. %j echoes verbatim. */
            if (fi < flen && (fmt[fi] == 'h' || fmt[fi] == 'l' || fmt[fi] == 'z' ||
                              fmt[fi] == 'j' || fmt[fi] == 't' || fmt[fi] == 'L')) {
                size_t ml = 1;
                if (fi + 1 < flen &&
                    ((fmt[fi] == 'h' && fmt[fi + 1] == 'h') ||
                     (fmt[fi] == 'l' && fmt[fi + 1] == 'l'))) {
                    ml = 2;
                }
                if (fi + ml < flen) {
                    char nxt = fmt[fi + ml];
                    if (nxt == 'd' || nxt == 'i' || nxt == 'u' || nxt == 'o' ||
                        nxt == 'x' || nxt == 'X' || nxt == 'n') {
                        fi += ml;
                    }
                }
            }
            if (fi >= flen) break;
            char conv = fmt[fi++];
            if (conv == '\0') {
                VM_SPRINTF_ENSURE(2);
                result[rlen++] = '%';
                result[rlen++] = '\0';
                continue;
            }
            spec[si] = conv;
            spec[si + 1] = 0;

            VMValue arg = VM_UNDEF_VAL;
            if (positional) {
                if (pos_idx >= 1 && pos_idx < argc) arg = args[pos_idx];
            } else {
                if (ai < argc) arg = args[ai++];
            }
            if (VM_IS_PTR(arg) && VM_PTR_TYPE(arg) == VM_OBJ_WEAKREF) {
                arg = VM_AS_WEAKREF(arg)->target;
            }

            if (vec_flag) {
                const char *sep_str = ".";
                size_t sep_len = 1;
                char sbuf[64];
                if (has_vec_sep) {
                    if (VM_IS_PTR(vec_sep_val) && VM_PTR_TYPE(vec_sep_val) == VM_OBJ_WEAKREF) {
                        vec_sep_val = VM_AS_WEAKREF(vec_sep_val)->target;
                    }
                    if (vm && vm_has_stringify(vm->program, vec_sep_val)) {
                        vec_sep_val = vm_stringify_ov(vm, vec_sep_val);
                        if (vm->has_pending_exc) { free(result); return VM_UNDEF_VAL; }
                    }
                    if (vm_is_utf8(vec_sep_val)) out_utf8 = 1;
                    sep_str = vm_bytes_view(vec_sep_val, sbuf, sizeof(sbuf), &sep_len);
                }
                if (vm && vm_has_stringify(vm->program, arg)) {
                    arg = vm_stringify_ov(vm, arg);
                    if (vm->has_pending_exc) { free(result); return VM_UNDEF_VAL; }
                }
                int vu8 = vm_is_utf8(arg);
                if (vu8) out_utf8 = 1;
                char vabuf[64];
                size_t vlen = 0;
                const char *vs = vm_bytes_view(arg, vabuf, sizeof(vabuf), &vlen);
                for (size_t bi = 0; bi < vlen; ) {
                    unsigned int byte_val;
                    size_t adv = 1;
                    if (vu8) {
                        unsigned char c0 = (unsigned char)vs[bi];
                        if (c0 < 0x80) { byte_val = c0; adv = 1; }
                        else if ((c0 & 0xE0) == 0xC0 && bi + 1 < vlen) {
                            byte_val = ((c0 & 0x1F) << 6) | ((unsigned char)vs[bi + 1] & 0x3F); adv = 2;
                        } else if ((c0 & 0xF0) == 0xE0 && bi + 2 < vlen) {
                            byte_val = ((c0 & 0x0F) << 12) | (((unsigned char)vs[bi + 1] & 0x3F) << 6)
                                     | ((unsigned char)vs[bi + 2] & 0x3F); adv = 3;
                        } else if ((c0 & 0xF8) == 0xF0 && bi + 3 < vlen) {
                            byte_val = ((c0 & 0x07) << 18) | (((unsigned char)vs[bi + 1] & 0x3F) << 12)
                                     | (((unsigned char)vs[bi + 2] & 0x3F) << 6) | ((unsigned char)vs[bi + 3] & 0x3F); adv = 4;
                        } else { byte_val = c0; adv = 1; }
                    } else {
                        byte_val = (unsigned char)vs[bi];
                        adv = 1;
                    }
                    if (conv == 'd' || conv == 'i' || conv == 'u' ||
                        conv == 'o' || conv == 'x' || conv == 'X') {
                        spec[si] = 0;
                        char vfmt[104];
                        snprintf(vfmt, sizeof(vfmt), "%sll%c", spec, conv);
                        spec[si] = conv;
                        int vn = snprintf(NULL, 0, vfmt, (long long)byte_val);
                        if (vn > 0) {
                            VM_SPRINTF_ENSURE((size_t)vn);
                            rlen += (size_t)snprintf(result + rlen, rcap - rlen, vfmt, (long long)byte_val);
                        }
                    } else if (conv == 'b' || conv == 'B') {
                        vm_sprintf_append_bin(&result, &rcap, &rlen, (uint64_t)byte_val, spec, conv);
                    } else {
                        int vn = snprintf(NULL, 0, "%u", byte_val);
                        if (vn > 0) {
                            VM_SPRINTF_ENSURE((size_t)vn);
                            rlen += (size_t)snprintf(result + rlen, rcap - rlen, "%u", byte_val);
                        }
                    }
                    bi += adv;
                    if (bi < vlen && sep_len > 0) {
                        VM_SPRINTF_ENSURE(sep_len);
                        memcpy(result + rlen, sep_str, sep_len);
                        rlen += sep_len;
                    }
                }
                continue;
            }

            if (conv == 's') {
                if (vm && vm_has_stringify(vm->program, arg)) {
                    arg = vm_stringify_ov(vm, arg);
                    if (vm->has_pending_exc) { free(result); return VM_UNDEF_VAL; }
                }
                int arg_u8 = vm_is_utf8(arg);
                if (arg_u8) out_utf8 = 1;
                char abuf[64];
                size_t slen = 0;
                const char *sval = vm_bytes_view(arg, abuf, sizeof(abuf), &slen);
                if (si == 1) {
                    if (slen > 0) {
                        VM_SPRINTF_ENSURE(slen);
                        memcpy(result + rlen, sval, slen);
                        rlen += slen;
                    }
                } else {
                    const char *sp = spec + 1;
                    int has_minus = 0, has_zero = 0;
                    while (sp < spec + si && strchr("-+ #0", *sp)) {
                        if (*sp == '-') has_minus = 1;
                        if (*sp == '0') has_zero = 1;
                        sp++;
                    }
                    int width = 0;
                    while (sp < spec + si && isdigit((unsigned char)*sp)) {
                        if (width > (INT_MAX - (*sp - '0')) / 10) {
                            width = INT_MAX;
                            while (sp < spec + si && isdigit((unsigned char)*sp)) sp++;
                            break;
                        }
                        width = width * 10 + (*sp - '0');
                        sp++;
                    }
                    int prec = -1;
                    if (sp < spec + si && *sp == '.') {
                        sp++;
                        prec = 0;
                        while (sp < spec + si && isdigit((unsigned char)*sp)) {
                            if (prec > (INT_MAX - (*sp - '0')) / 10) {
                                prec = INT_MAX;
                                while (sp < spec + si && isdigit((unsigned char)*sp)) sp++;
                                break;
                            }
                            prec = prec * 10 + (*sp - '0');
                            sp++;
                        }
                    }
                    size_t plen = slen;
                    if (prec >= 0) {
                        if (arg_u8) {
                            plen = vm_utf8_char_to_byte(sval, slen, (int64_t)prec);
                        } else if ((size_t)prec < slen) {
                            plen = (size_t)prec;
                        }
                    }
                    size_t disp_len = arg_u8 ? vm_utf8_char_count(sval, plen) : plen;
                    size_t pad = (width > 0 && (size_t)width > disp_len) ? ((size_t)width - disp_len) : 0;
                    char padch = (!has_minus && has_zero) ? '0' : ' ';
                    VM_SPRINTF_ENSURE(pad + plen);
                    if (!has_minus && pad > 0) { memset(result + rlen, padch, pad); rlen += pad; }
                    if (plen > 0) { memcpy(result + rlen, sval, plen); rlen += plen; }
                    if (has_minus && pad > 0) { memset(result + rlen, ' ', pad); rlen += pad; }
                }
                continue;
            }
            if (vm && VM_IS_PTR(arg) && VM_PTR_TYPE(arg) == VM_OBJ_ARRAY &&
                !VM_IS_UNDEF(VM_AS_ARRAY(arg)->tied_obj)) {
                int64_t tsz = vm_tied_array_fetchsize(vm, VM_AS_ARRAY(arg)->tied_obj);
                if (vm->has_pending_exc) { free(result); return VM_UNDEF_VAL; }
                arg = VM_MAKE_INT(tsz);
            }
            if (conv == 'd' || conv == 'i') {
                spec[si] = 0;
                char tmp[104];
                snprintf(tmp, sizeof(tmp), "%slld", spec);
                long long val = (long long)vm_to_int(arg);
                int n = snprintf(NULL, 0, tmp, val);
                if (n > 0) { VM_SPRINTF_ENSURE((size_t)n); rlen += (size_t)snprintf(result + rlen, rcap - rlen, tmp, val); }
            } else if (conv == 'u') {
                spec[si] = 0;
                char tmp[104];
                snprintf(tmp, sizeof(tmp), "%sllu", spec);
                unsigned long long val = (unsigned long long)vm_to_int(arg);
                int n = snprintf(NULL, 0, tmp, val);
                if (n > 0) { VM_SPRINTF_ENSURE((size_t)n); rlen += (size_t)snprintf(result + rlen, rcap - rlen, tmp, val); }
            } else if (conv == 'b' || conv == 'B') {
                uint64_t val = (uint64_t)vm_to_int(arg);
                vm_sprintf_append_bin(&result, &rcap, &rlen, val, spec, conv);
            } else if (conv == 'f' || conv == 'F' || conv == 'e' || conv == 'E' ||
                       conv == 'g' || conv == 'G' || conv == 'a' || conv == 'A') {
                double val = vm_to_num(arg);
                if (isnan(val)) {
                    VM_SPRINTF_ENSURE(3);
                    memcpy(result + rlen, "NaN", 3);
                    rlen += 3;
                } else if (isinf(val)) {
                    const char *is = val < 0 ? "-Inf" : "Inf";
                    size_t il = strlen(is);
                    VM_SPRINTF_ENSURE(il);
                    memcpy(result + rlen, is, il);
                    rlen += il;
                } else {
                    int n = snprintf(NULL, 0, spec, val);
                    if (n > 0) { VM_SPRINTF_ENSURE((size_t)n); rlen += (size_t)snprintf(result + rlen, rcap - rlen, spec, val); }
                }
            } else if (conv == 'c') {
                int ch = (int)vm_to_int(arg);
                char ub[8];
                int ulen = 0;
                if (ch >= 0x80) out_utf8 = 1;
                if (ch < 0) {
                    ub[ulen++] = (char)(unsigned char)ch;
                } else if (ch < 0x80) {
                    ub[ulen++] = (char)ch;
                } else if (ch < 0x800) {
                    ub[ulen++] = (char)(0xC0 | (ch >> 6));
                    ub[ulen++] = (char)(0x80 | (ch & 0x3F));
                } else if (ch < 0x10000) {
                    ub[ulen++] = (char)(0xE0 | (ch >> 12));
                    ub[ulen++] = (char)(0x80 | ((ch >> 6) & 0x3F));
                    ub[ulen++] = (char)(0x80 | (ch & 0x3F));
                } else {
                    ub[ulen++] = (char)(0xF0 | (ch >> 18));
                    ub[ulen++] = (char)(0x80 | ((ch >> 12) & 0x3F));
                    ub[ulen++] = (char)(0x80 | ((ch >> 6) & 0x3F));
                    ub[ulen++] = (char)(0x80 | (ch & 0x3F));
                }
                VM_SPRINTF_ENSURE((size_t)ulen);
                memcpy(result + rlen, ub, (size_t)ulen);
                rlen += (size_t)ulen;
            } else if (conv == 'x' || conv == 'X' || conv == 'o') {
                spec[si] = 0;
                char tmp2[104];
                snprintf(tmp2, sizeof(tmp2), "%sll%c", spec, conv);
                long long val = (long long)vm_to_int(arg);
                int n = snprintf(NULL, 0, tmp2, val);
                if (n > 0) { VM_SPRINTF_ENSURE((size_t)n); rlen += (size_t)snprintf(result + rlen, rcap - rlen, tmp2, val); }
            } else if (conv == 'p') {
                void *ptr = VM_IS_UNDEF(arg) ? NULL : (void*)(uintptr_t)arg;
                char hexbuf[64];
                snprintf(hexbuf, sizeof(hexbuf), "%p", ptr);
                const char *src = hexbuf;
                if (src[0] == '0' && (src[1] == 'x' || src[1] == 'X')) src += 2;
                size_t pl = strlen(src);
                VM_SPRINTF_ENSURE(pl);
                memcpy(result + rlen, src, pl);
                rlen += pl;
            } else {
                size_t slen_raw = (size_t)(si + 1);
                VM_SPRINTF_ENSURE(slen_raw);
                memcpy(result + rlen, spec, slen_raw);
                rlen += slen_raw;
            }
        } else {
            VM_SPRINTF_ENSURE(1);
            result[rlen++] = fmt[fi++];
        }
    }
    result[rlen] = 0;
    VMValue out = vm_make_str_flag(result, rlen, out_utf8);
    free(result);
    return out;
}
#undef VM_SPRINTF_ENSURE

/* ===== Method dispatch ===== */

static int vm_find_direct_named(VMProgram *prog, const char *cls, const char *method,
                                const char *suffix, int allow_colon_sep) {
    char fullname[280];
    snprintf(fullname, sizeof(fullname), "%s_%s%s", cls, method, suffix);
    int idx = vm_program_find_func(prog, fullname);
    if (idx >= 0) return idx;
    if (allow_colon_sep) {
        snprintf(fullname, sizeof(fullname), "%s::%s%s", cls, method, suffix);
        idx = vm_program_find_func(prog, fullname);
        if (idx >= 0) return idx;
        snprintf(fullname, sizeof(fullname), "%s_%s%s", cls, method, suffix);
    }
    if (strchr(fullname, ':')) {
        char sanitized[280];
        int si = 0;
        for (int fi = 0; fullname[fi] && si < 278; fi++) {
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

static int vm_find_direct_method(VMProgram *prog, const char *cls, const char *method) {
    return vm_find_direct_named(prog, cls, method, "", 1);
}

static int vm_find_direct_modwrap(VMProgram *prog, const char *cls, const char *method) {
    return vm_find_direct_named(prog, cls, method, "__modwrap", 0);
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
    int idx = vm_find_direct_modwrap(prog, class_name, method);
    if (idx >= 0) return idx;
    idx = vm_find_direct_method(prog, class_name, method);
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
    free(vm->method_cache[slot].class_name);
    free(vm->method_cache[slot].method);
    vm->method_cache[slot].class_name = strdup(class_name);
    vm->method_cache[slot].method = strdup(method);
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

/* Check if a value has a stringify overload ("") or tied array length stringification */
static int vm_has_stringify(VMProgram *prog, VMValue v) {
    if (VM_IS_PTR(v)) {
        const char *cname = vm_obj_class_name(v);
        if (cname) {
            const char *method = vm_find_overload(prog, cname, "\"\"");
            if (method) return 1;
        }
        if (VM_PTR_TYPE(v) == VM_OBJ_ARRAY) {
            if (!VM_IS_UNDEF(VM_AS_ARRAY(v)->tied_obj)) return 1;
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

static StradaValue *vm_fh_ensure_sv(VMFileHandle *fh) {
    if (fh->owner_sv) {
        if (!fh->fp && fh->owner_sv->value.fh) {
            fh->owner_sv->value.fh = NULL;
        } else if (fh->fp && !fh->owner_sv->value.fh) {
            fh->fp = NULL;
        }
        return fh->owner_sv;
    }
    if (fh->fp) {
        fh->owner_sv = strada_new_filehandle((FILE*)fh->fp);
    } else {
        StradaValue *sv = strada_new_undef();
        sv->type = STRADA_FILEHANDLE;
        sv->value.fh = NULL;
        fh->owner_sv = sv;
    }
    return fh->owner_sv;
}

static inline void vm_fh_sync_from_sv(VMValue v) {
    if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_FILEHANDLE) {
        VMFileHandle *fh = (VMFileHandle*)VM_TO_PTR(v);
        if (fh->owner_sv && !fh->owner_sv->value.fh) {
            fh->fp = NULL;
        }
    }
}

typedef struct {
    VM *vm;
    VMProgram *prog;
    VMClosure *vcl;
} VMExportedClosureCtx;

static StradaValue* vm_exported_closure_call(void *raw, int argc, StradaValue **argv);
static void vm_exported_closure_free(void *raw);
static VMValue vm_tied_scalar_fetch(VM *vm, VMValue ts_val);

/* Convert VMValue to StradaValue* for passing to __C__ blocks.
 * Depth-capped: cyclic VM structures (e.g. the weak-ref tests' two-hash
 * cycles) previously recursed until the C stack corrupted the heap. */
static StradaValue *vm_to_sv_depth(VMValue v, int depth) {
    if (VM_IS_ANY_INT(v)) return strada_new_int(VM_ANY_INT_VAL(v));
    if (VM_IS_FLOAT(v)) return strada_new_num(VM_FLOAT_VAL(v));
    if (VM_IS_UNDEF(v)) return strada_new_undef();
    if (depth >= 32) return strada_new_undef();
    if (VM_IS_PTR(v)) {
        switch (VM_PTR_TYPE(v)) {
        case VM_OBJ_UINT64: return strada_new_uint(VM_AS_UINT64(v)->val);
        case VM_OBJ_STR: {
            StradaValue *r = strada_new_str_len(VM_AS_STR(v)->data, VM_AS_STR(v)->len);
            if (VM_AS_STR(v)->hdr.is_utf8) STRADA_SET_UTF8_FLAG(r);
            return r;
        }
        case VM_OBJ_ARRAY: {
            /* Convert VM array to Strada array */
            VMArray *va = VM_AS_ARRAY(v);
            StradaValue *sa = strada_new_array();
            for (int i = 0; i < va->size; i++)
                strada_array_push_take(sa->value.av, vm_to_sv_depth(va->items[i], depth + 1));
            if (va->class_name) strada_bless(sa, va->class_name);
            return sa;
        }
        case VM_OBJ_HASH: {
            VMHash *vh = VM_AS_HASH(v);
            StradaValue *sh = strada_new_hash();
            for (int i = 0; i < vh->capacity; i++)
                if (vh->entries[i].occupied == 1) {
                    StradaValue *ksv = strada_new_str_len(vh->entries[i].key, vh->entries[i].keylen);
                    StradaValue *vsv = vm_to_sv_depth(vh->entries[i].value, depth + 1);
                    strada_hash_set_sv(sh->value.hv, ksv, vsv);
                    strada_decref(ksv);
                    strada_decref(vsv);
                }
            if (vh->class_name) {
                strada_bless(sh, vh->class_name);
            }
            return sh;
        }
        case VM_OBJ_STRBUF: {
            StradaValue *r = strada_new_str_len(VM_AS_STRBUF(v)->data, VM_AS_STRBUF(v)->len);
            if (VM_AS_STRBUF(v)->hdr.is_utf8) STRADA_SET_UTF8_FLAG(r);
            return r;
        }
        case VM_OBJ_NATIVE_SV: {
            VMNativeSV *nsv = (VMNativeSV*)VM_TO_PTR(v);
            strada_incref(nsv->sv);
            return nsv->sv;
        }
        case VM_OBJ_CELL:
            return vm_to_sv_depth(VM_AS_CELL(v)->val, depth + 1);
        case VM_OBJ_WEAKREF:
            return vm_to_sv_depth(VM_AS_WEAKREF(v)->target, depth + 1);
        case VM_OBJ_TIED_SCALAR: {
            if (!g_active_vm || g_active_vm->has_pending_exc) return strada_new_undef();
            VMValue fv = vm_tied_scalar_fetch(g_active_vm, v);
            if (g_active_vm->has_pending_exc) return strada_new_undef();
            return vm_to_sv_depth(fv, depth + 1);
        }
        case VM_OBJ_FILEHANDLE: {
            VMFileHandle *fh = (VMFileHandle*)VM_TO_PTR(v);
            StradaValue *sv = vm_fh_ensure_sv(fh);
            strada_incref(sv);
            return sv;
        }
        case VM_OBJ_CLOSURE: {
            if (!g_active_vm || !g_active_vm->program) return strada_new_undef();
            VMExportedClosureCtx *ctx = malloc(sizeof(VMExportedClosureCtx));
            if (!ctx) return strada_new_undef();
            ctx->vm = g_active_vm;
            ctx->prog = g_active_vm->program;
            ctx->vcl = VM_AS_CLOSURE(v);
            __sync_add_and_fetch(&ctx->vcl->pin_count, 1);
            ctx->vcl->hdr.gc_immortal = 1;
            vm_retain(ctx->vm);
            vm_program_retain(ctx->prog);
            StradaValue *csv = strada_custom_closure_new(vm_exported_closure_call, vm_exported_closure_free, ctx);
            if (ctx->vcl->class_name && csv) strada_bless(csv, ctx->vcl->class_name);
            return csv;
        }
        case VM_OBJ_SREF: {
            VMScalarRef *sr = VM_AS_SREF(v);
            VMValue tv = VM_UNDEF_VAL;
            if (sr->global_name) {
                if (g_active_vm && g_active_vm->globals) {
                    tv = vm_globals_get(g_active_vm, sr->global_name);
                }
            } else if (VM_IS_PTR(sr->cell) && VM_PTR_TYPE(sr->cell) == VM_OBJ_CELL) {
                tv = VM_AS_CELL(sr->cell)->val;
            }
            StradaValue *rv = strada_ref_create_take(vm_to_sv_depth(tv, depth + 1));
            if (sr->class_name) strada_bless(rv, sr->class_name);
            return rv;
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
    case STRADA_INT:
        if (STRADA_INT_IS_UV(sv)) {
            uint64_t uv = (uint64_t)sv->value.iv;
            if (uv <= (uint64_t)INT64_MAX) return VM_MAKE_INT((int64_t)uv);
            return vm_box_uint64(uv);
        }
        return VM_MAKE_INT(sv->value.iv);
    case STRADA_NUM: return VM_MAKE_FLOAT(sv->value.nv);
    case STRADA_STR: {
        if (!sv->value.pv) return VM_UNDEF_VAL;
        VMValue r = VM_MAKE_STR_N(sv->value.pv, STRADA_STR_BYTELEN(sv));
        if (STRADA_STR_IS_UTF8(sv)) VM_AS_STR(r)->hdr.is_utf8 = 1;
        return r;
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
        if (sv->meta && sv->meta->blessed_package)
            va->class_name = strdup(sv->meta->blessed_package);
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
                vm_hash_set_n(h, hv->entries[i].key->data, hv->entries[i].key->len,
                              sv_to_vm_depth(hv->entries[i].value, depth + 1));
            }
        }
        if (sv->meta && sv->meta->blessed_package)
            h->class_name = strdup(sv->meta->blessed_package);
        return VM_MAKE_PTR(h);
    }
    case STRADA_REF: {
        /* Keep slot refs and blessed refs wrapped as VMNativeSV so native
         * slot references keep variable linkage and OOP objects dispatch. */
        if (strada_is_slot_ref(sv)) break;
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
    case STRADA_FILEHANDLE: {
        VMFileHandle *fh = calloc(1, sizeof(VMFileHandle));
        vm_gc_init_hdr(&fh->hdr, VM_OBJ_FILEHANDLE);
        fh->fp = sv->value.fh;
        strada_incref(sv);
        fh->owner_sv = sv;
        return VM_MAKE_PTR(fh);
    }
    case STRADA_CLOSURE: {
        if (g_active_vm) {
            VMExportedClosureCtx *ectx = (VMExportedClosureCtx *)strada_custom_closure_get_ctx(sv, vm_exported_closure_call);
            if (ectx && ectx->vm == g_active_vm && ectx->vcl) {
                if (sv->meta && sv->meta->blessed_package &&
                    (!ectx->vcl->class_name || strcmp(ectx->vcl->class_name, sv->meta->blessed_package) != 0)) {
                    free(ectx->vcl->class_name);
                    ectx->vcl->class_name = strdup(sv->meta->blessed_package);
                }
                return VM_MAKE_PTR(ectx->vcl);
            }
        }
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

static VMValue vm_abs_val(VMValue v) {
    if (VM_IS_INT(v)) {
        int64_t iv = VM_INT_VAL(v);
        return VM_MAKE_INT(iv < 0 ? -iv : iv);
    }
    if (VM_IS_FLOAT(v)) {
        return VM_MAKE_FLOAT(fabs(VM_FLOAT_VAL(v)));
    }
    if (VM_IS_UNDEF(v)) {
        return VM_MAKE_INT(0);
    }
    if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_INT64) {
        int64_t iv = VM_AS_INT64(v)->val;
        if (iv == INT64_MIN) return vm_box_uint64((uint64_t)INT64_MAX + 1ULL);
        return VM_MAKE_INT(iv < 0 ? -iv : iv);
    }
    if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_UINT64) {
        return v;
    }
    if (VM_IS_PTR(v) && (VM_PTR_TYPE(v) == VM_OBJ_STR || VM_PTR_TYPE(v) == VM_OBJ_STRBUF)) {
        char buf[64];
        size_t slen = 0;
        const char *s = vm_bytes_view(v, buf, sizeof(buf), &slen);
        if (!memchr(s, '\0', slen)) {
            const char *p = s;
            const char *end = s + slen;
            while (p < end && (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r' || *p == '\f' || *p == '\v')) p++;
            if (p < end && (*p == '-' || *p == '+')) p++;
            const char *dstart = p;
            int has_digits = 0;
            while (p < end && *p >= '0' && *p <= '9') { has_digits = 1; p++; }
            if (has_digits) {
                const char *t = p;
                while (t < end && (*t == ' ' || *t == '\t' || *t == '\n' || *t == '\r' || *t == '\f' || *t == '\v')) t++;
                if (t == end) {
                    errno = 0;
                    char *endp = NULL;
                    unsigned long long uv = strtoull(dstart, &endp, 10);
                    if (errno == 0 && endp == p) {
                        if (uv <= (unsigned long long)INT64_MAX) return VM_MAKE_INT((int64_t)uv);
                        return vm_box_uint64((uint64_t)uv);
                    }
                }
            }
        }
    }
    return VM_MAKE_FLOAT(fabs(vm_to_num(v)));
}

static int64_t vm_parse_hex_str(const char *s, size_t len) {
    size_t i = 0;
    while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == '\f' || s[i] == '\v')) i++;
    if (i + 1 < len && s[i] == '0' && (s[i + 1] == 'x' || s[i + 1] == 'X')) i += 2;
    uint64_t acc = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i++];
        if (c == '_') continue;
        unsigned int d;
        if (c >= '0' && c <= '9') d = (unsigned int)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned int)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned int)(c - 'A' + 10);
        else break;
        acc = (acc << 4) | d;
    }
    return (int64_t)acc;
}

static int64_t vm_parse_oct_str(const char *s, size_t len) {
    size_t i = 0;
    int base = 8;
    while (i < len && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r' || s[i] == '\f' || s[i] == '\v')) i++;
    if (i + 1 < len && s[i] == '0') {
        char c = s[i + 1];
        if (c == 'x' || c == 'X') { base = 16; i += 2; }
        else if (c == 'b' || c == 'B') { base = 2; i += 2; }
        else if (c == 'o' || c == 'O') { base = 8; i += 2; }
    }
    uint64_t acc = 0;
    while (i < len) {
        unsigned char c = (unsigned char)s[i++];
        if (c == '_') continue;
        unsigned int d = 99;
        if (c >= '0' && c <= '9') d = (unsigned int)(c - '0');
        else if (c >= 'a' && c <= 'f') d = (unsigned int)(c - 'a' + 10);
        else if (c >= 'A' && c <= 'F') d = (unsigned int)(c - 'A' + 10);
        if (d >= (unsigned int)base) break;
        acc = acc * (uint64_t)base + d;
    }
    return (int64_t)acc;
}

typedef struct {
    VMValue *orig;
    VMValue *clone;
    int count;
    int cap;
} VMCloneMap;

static void vm_clone_map_put(VMCloneMap *m, VMValue orig, VMValue clone) {
    if (m->count >= m->cap) {
        m->cap = m->cap ? m->cap * 2 : 16;
        m->orig = realloc(m->orig, (size_t)m->cap * sizeof(VMValue));
        m->clone = realloc(m->clone, (size_t)m->cap * sizeof(VMValue));
    }
    m->orig[m->count] = orig;
    m->clone[m->count] = clone;
    m->count++;
}

static VMValue vm_clone_val_rec(VM *vm, VMValue v, VMCloneMap *m, int depth) {
    if (!VM_IS_PTR(v)) return v;
    for (int i = 0; i < m->count; i++) {
        if (m->orig[i] == v) return m->clone[i];
    }
    if (depth > 256) return vm_val_copy(v);
    switch (VM_PTR_TYPE(v)) {
    case VM_OBJ_INT64:
    case VM_OBJ_UINT64:
        return vm_val_copy(v);
    case VM_OBJ_STR: {
        VMString *vs = VM_AS_STR(v);
        return vm_make_str_flag(vs->data, vs->len, vs->hdr.is_utf8);
    }
    case VM_OBJ_STRBUF: {
        VMStrBuf *sb = VM_AS_STRBUF(v);
        return vm_make_str_flag(sb->data ? sb->data : "", sb->len, sb->hdr.is_utf8);
    }
    case VM_OBJ_ARRAY: {
        VMArray *oa = VM_AS_ARRAY(v);
        VMArray *na = vm_array_new(oa->cap > 8 ? oa->cap : 8);
        if (oa->class_name) na->class_name = strdup(oa->class_name);
        VMValue out = VM_MAKE_PTR(na);
        vm_clone_map_put(m, v, out);
        if (oa->size > 0) {
            if (oa->holes) {
                na->holes = calloc((size_t)na->cap, 1);
                memcpy(na->holes, oa->holes, (size_t)oa->size);
            }
            na->size = oa->size;
            for (int i = 0; i < oa->size; i++) {
                if (oa->holes && oa->holes[i]) {
                    na->items[i] = VM_UNDEF_VAL;
                } else {
                    VMValue ev = oa->items[i];
                    if (vm && vm->has_weakrefs && VM_IS_PTR(ev) && VM_PTR_TYPE(ev) == VM_OBJ_WEAKREF) {
                        ev = VM_AS_WEAKREF(ev)->target;
                    }
                    na->items[i] = vm_clone_val_rec(vm, ev, m, depth + 1);
                }
            }
        }
        return out;
    }
    case VM_OBJ_HASH: {
        VMHash *oh = VM_AS_HASH(v);
        VMHash *nh = vm_hash_new(oh->capacity > 16 ? oh->capacity : 16);
        if (oh->class_name) nh->class_name = strdup(oh->class_name);
        VMValue out = VM_MAKE_PTR(nh);
        vm_clone_map_put(m, v, out);
        for (int i = 0; i < oh->capacity; i++) {
            if (oh->entries[i].occupied == 1) {
                VMValue ev = oh->entries[i].value;
                if (vm && vm->has_weakrefs && VM_IS_PTR(ev) && VM_PTR_TYPE(ev) == VM_OBJ_WEAKREF) {
                    ev = VM_AS_WEAKREF(ev)->target;
                }
                VMValue cv = vm_clone_val_rec(vm, ev, m, depth + 1);
                vm_hash_set_n(nh, oh->entries[i].key, oh->entries[i].keylen, cv);
            }
        }
        return out;
    }
    case VM_OBJ_SREF: {
        VMScalarRef *sr = VM_AS_SREF(v);
        VMScalarRef *nsr = calloc(1, sizeof(VMScalarRef));
        vm_gc_init_hdr(&nsr->hdr, VM_OBJ_SREF);
        if (sr->class_name) nsr->class_name = strdup(sr->class_name);
        VMCell *nc = calloc(1, sizeof(VMCell));
        vm_gc_init_hdr(&nc->hdr, VM_OBJ_CELL);
        nc->val = VM_UNDEF_VAL;
        nsr->cell = VM_MAKE_PTR(nc);
        VMValue out = VM_MAKE_PTR(nsr);
        vm_clone_map_put(m, v, out);
        VMValue tv = VM_UNDEF_VAL;
        if (sr->global_name) {
            if (vm && vm->globals) tv = vm_globals_get(vm, sr->global_name);
        } else if (VM_IS_PTR(sr->cell) && VM_PTR_TYPE(sr->cell) == VM_OBJ_CELL) {
            tv = VM_AS_CELL(sr->cell)->val;
        }
        nc->val = vm_clone_val_rec(vm, tv, m, depth + 1);
        return out;
    }
    case VM_OBJ_CELL:
        return vm_clone_val_rec(vm, VM_AS_CELL(v)->val, m, depth + 1);
    case VM_OBJ_NATIVE_SV: {
        VMNativeSV *nsv = (VMNativeSV *)VM_TO_PTR(v);
        if (nsv->sv) {
            StradaValue *cs = strada_clone(nsv->sv);
            VMValue r = sv_to_vm(cs);
            strada_decref(cs);
            return r;
        }
        return v;
    }
    default:
        return v;
    }
}

static VMValue vm_clone_val(VM *vm, VMValue v) {
    VMCloneMap m = {0};
    if (vm) vm->gc_pause++;
    VMValue r = vm_clone_val_rec(vm, v, &m, 0);
    if (vm) vm->gc_pause--;
    free(m.orig);
    free(m.clone);
    return r;
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
    if (vm->is_worker_vm) pthread_mutex_lock(&vm->gc_mutex);
    vm->stack = ns;
    vm->stack_cap = newcap;
    if (vm->is_worker_vm) pthread_mutex_unlock(&vm->gc_mutex);
}

/* Unwrap a weak reference slot; if its target is non-undef, run GC while keeping
 * raw + caller roots on the VM stack so dead targets clear deterministically. */
static VMValue vm_unwrap_weak_gc(VM *vm, VMValue raw, VMValue *roots, int nroots) {
    if (!VM_IS_PTR(raw) || VM_PTR_TYPE(raw) != VM_OBJ_WEAKREF) return raw;
    VMWeakRef *wr = VM_AS_WEAKREF(raw);
    if (!VM_IS_UNDEF(wr->target) && vm && vm->exec_base == 0 && vm->gc_pause == 0) {
        size_t saved_sp = vm->stack_top;
        if (vm->stack_top >= vm->stack_cap) vm_stack_grow(vm, vm->stack_top);
        vm->stack[vm->stack_top++] = raw;
        for (int i = 0; i < nroots; i++) {
            if (vm->stack_top >= vm->stack_cap) vm_stack_grow(vm, vm->stack_top);
            vm->stack[vm->stack_top++] = roots[i];
        }
        vm_gc_collect(vm);
        vm->stack_top = saved_sp;
    }
    return wr->target;
}

static void vm_restore_one_hash_elem(VM *vm, VMHash *h, char *hkey, int existed, VMValue saved_value, int propagate_exc) {
    if (h && hkey) {
        const char *tcls = !VM_IS_UNDEF(h->tied_obj) ? vm_obj_class_name(h->tied_obj) : NULL;
        if (tcls) {
            vm->gc_pause++;
            int saved_p = vm->has_pending_exc;
            VMValue saved_pe = vm->pending_exc;
            if (!propagate_exc) {
                vm->has_pending_exc = 0;
                vm->pending_exc = VM_UNDEF_VAL;
            }
            VMValue ksv = vm_str(hkey);
            if (existed) {
                int sfidx = vm_find_method(vm->program, tcls, "STORE");
                if (sfidx >= 0) {
                    VMValue cargs[3] = { h->tied_obj, ksv, saved_value };
                    vm_execute_call(vm, sfidx, cargs, 3);
                }
            } else {
                int dfidx = vm_find_method(vm->program, tcls, "DELETE");
                if (dfidx >= 0) {
                    VMValue cargs[2] = { h->tied_obj, ksv };
                    VMValue dr = vm_execute_call(vm, dfidx, cargs, 2);
                    vm_temp_free(&dr);
                }
            }
            if (!propagate_exc) {
                vm->has_pending_exc = saved_p;
                vm->pending_exc = saved_pe;
            }
            vm->gc_pause--;
        } else {
            if (existed) {
                vm_hash_set(h, hkey, saved_value);
            } else {
                vm_hash_delete(h, hkey);
            }
        }
    }
    free(hkey);
}

static void vm_unwind_global_saves(VM *vm, int target_depth) {
    if (!vm || target_depth < 0) return;
    while (vm->global_save_count > target_depth) {
        VMGlobalSave *gs = &vm->global_saves[--vm->global_save_count];
        if (gs->is_hash_elem) {
            VMHash *h = gs->hash;
            char *hkey = gs->hash_key;
            int existed = gs->existed;
            VMValue saved_value = gs->saved_value;
            gs->hash_key = NULL;
            gs->hash = NULL;
            gs->saved_value = VM_UNDEF_VAL;
            vm_restore_one_hash_elem(vm, h, hkey, existed, saved_value, 0);
        } else if (gs->name) {
            vm_globals_set(vm, gs->name, gs->saved_value);
            free(gs->name);
            gs->name = NULL;
        }
    }
}

static VMValue vm_capture_c_exception(void) {
    StradaValue *exc = strada_get_exception();
    strada_clear_exception();
    VMValue vexc = exc ? sv_to_vm(exc) : vm_str("C exception");
    if (exc) strada_decref(exc);
    return vexc;
}

__attribute__((noinline))
static int vm_c_call_method(VM *vm, StradaValue *self_sv, const char *method, StradaValue *packed_args, StradaValue **out_res, VMValue *out_exc) {
    int cm = strada_cleanup_mark();
    int lm = strada_local_depth_get();
    vm->gc_pause++;
    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        StradaValue *nr = strada_method_call(self_sv, method, packed_args);
        STRADA_TRY_POP();
        vm->gc_pause--;
        *out_res = nr;
        return 0;
    } else {
        STRADA_TRY_POP();
        strada_local_restore_to(lm);
        strada_cleanup_drain_to(cm);
        vm->gc_pause--;
        *out_exc = vm_capture_c_exception();
        return 1;
    }
}

__attribute__((noinline))
static int vm_c_call_closure(VM *vm, StradaValue *cl_sv, StradaValue *packed_args, StradaValue **out_res, VMValue *out_exc) {
    int cm = strada_cleanup_mark();
    int lm = strada_local_depth_get();
    vm->gc_pause++;
    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        StradaValue *nr = strada_closure_call_array(cl_sv, packed_args);
        STRADA_TRY_POP();
        vm->gc_pause--;
        *out_res = nr;
        return 0;
    } else {
        STRADA_TRY_POP();
        strada_local_restore_to(lm);
        strada_cleanup_drain_to(cm);
        vm->gc_pause--;
        *out_exc = vm_capture_c_exception();
        return 1;
    }
}

__attribute__((noinline))
static int vm_c_call_cblock(VM *vm, void (*fn)(StradaValue**), StradaValue **sv_vars, VMValue *out_exc) {
    int cm = strada_cleanup_mark();
    int lm = strada_local_depth_get();
    vm->gc_pause++;
    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        fn(sv_vars);
        STRADA_TRY_POP();
        vm->gc_pause--;
        return 0;
    } else {
        STRADA_TRY_POP();
        strada_local_restore_to(lm);
        strada_cleanup_drain_to(cm);
        vm->gc_pause--;
        *out_exc = vm_capture_c_exception();
        return 1;
    }
}

__attribute__((noinline))
static int vm_c_call_0(VM *vm, StradaValue *(*fn)(void), VMValue *out_val, VMValue *out_exc) {
    int cm = strada_cleanup_mark();
    int lm = strada_local_depth_get();
    vm->gc_pause++;
    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        StradaValue *r = fn();
        STRADA_TRY_POP();
        vm->gc_pause--;
        *out_val = r ? sv_to_vm(r) : VM_UNDEF_VAL;
        if (r) strada_decref(r);
        return 0;
    } else {
        STRADA_TRY_POP();
        strada_local_restore_to(lm);
        strada_cleanup_drain_to(cm);
        vm->gc_pause--;
        *out_exc = vm_capture_c_exception();
        return 1;
    }
}

__attribute__((noinline))
static int vm_c_call_1(VM *vm, StradaValue *(*fn)(StradaValue*), VMValue a0, VMValue *out_val, VMValue *out_exc) {
    int cm = strada_cleanup_mark();
    int lm = strada_local_depth_get();
    StradaValue *sv0 = vm_to_sv(a0);
    strada_cleanup_push(sv0);
    vm->gc_pause++;
    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        StradaValue *r = fn(sv0);
        STRADA_TRY_POP();
        vm->gc_pause--;
        *out_val = r ? sv_to_vm(r) : VM_UNDEF_VAL;
        if (r) strada_decref(r);
        strada_cleanup_drain_to(cm);
        return 0;
    } else {
        STRADA_TRY_POP();
        strada_local_restore_to(lm);
        strada_cleanup_drain_to(cm);
        vm->gc_pause--;
        *out_exc = vm_capture_c_exception();
        return 1;
    }
}

__attribute__((noinline))
static int vm_c_call_2(VM *vm, StradaValue *(*fn)(StradaValue*, StradaValue*), VMValue a0, VMValue a1, VMValue *out_val, VMValue *out_exc) {
    int cm = strada_cleanup_mark();
    int lm = strada_local_depth_get();
    StradaValue *sv0 = vm_to_sv(a0);
    strada_cleanup_push(sv0);
    StradaValue *sv1 = vm_to_sv(a1);
    strada_cleanup_push(sv1);
    vm->gc_pause++;
    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        StradaValue *r = fn(sv0, sv1);
        STRADA_TRY_POP();
        vm->gc_pause--;
        *out_val = r ? sv_to_vm(r) : VM_UNDEF_VAL;
        if (r) strada_decref(r);
        strada_cleanup_drain_to(cm);
        return 0;
    } else {
        STRADA_TRY_POP();
        strada_local_restore_to(lm);
        strada_cleanup_drain_to(cm);
        vm->gc_pause--;
        *out_exc = vm_capture_c_exception();
        return 1;
    }
}

__attribute__((noinline))
static int vm_c_call_3(VM *vm, StradaValue *(*fn)(StradaValue*, StradaValue*, StradaValue*), VMValue a0, VMValue a1, VMValue a2, VMValue *out_val, VMValue *out_exc) {
    int cm = strada_cleanup_mark();
    int lm = strada_local_depth_get();
    StradaValue *sv0 = vm_to_sv(a0);
    strada_cleanup_push(sv0);
    StradaValue *sv1 = vm_to_sv(a1);
    strada_cleanup_push(sv1);
    StradaValue *sv2 = vm_to_sv(a2);
    strada_cleanup_push(sv2);
    vm->gc_pause++;
    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        StradaValue *r = fn(sv0, sv1, sv2);
        STRADA_TRY_POP();
        vm->gc_pause--;
        *out_val = r ? sv_to_vm(r) : VM_UNDEF_VAL;
        if (r) strada_decref(r);
        strada_cleanup_drain_to(cm);
        return 0;
    } else {
        STRADA_TRY_POP();
        strada_local_restore_to(lm);
        strada_cleanup_drain_to(cm);
        vm->gc_pause--;
        *out_exc = vm_capture_c_exception();
        return 1;
    }
}

__attribute__((noinline))
static int vm_c_void_1(VM *vm, void (*fn)(StradaValue*), VMValue a0, VMValue *out_exc) {
    int cm = strada_cleanup_mark();
    int lm = strada_local_depth_get();
    StradaValue *sv0 = vm_to_sv(a0);
    strada_cleanup_push(sv0);
    vm->gc_pause++;
    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        fn(sv0);
        STRADA_TRY_POP();
        vm->gc_pause--;
        strada_cleanup_drain_to(cm);
        return 0;
    } else {
        STRADA_TRY_POP();
        strada_local_restore_to(lm);
        strada_cleanup_drain_to(cm);
        vm->gc_pause--;
        *out_exc = vm_capture_c_exception();
        return 1;
    }
}

__attribute__((noinline))
static int vm_c_call_1_sv_void(VM *vm, StradaValue *(*fn)(StradaValue*), VMValue a0, VMValue *out_exc) {
    VMValue dummy = VM_UNDEF_VAL;
    int rc = vm_c_call_1(vm, fn, a0, &dummy, out_exc);
    vm_temp_free(&dummy);
    return rc;
}

__attribute__((noinline))
static int vm_c_call_downgrade(VM *vm, VMValue a0, int fail_ok, VMValue *out_val, VMValue *out_exc) {
    int cm = strada_cleanup_mark();
    int lm = strada_local_depth_get();
    StradaValue *sv0 = vm_to_sv(a0);
    strada_cleanup_push(sv0);
    vm->gc_pause++;
    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        StradaValue *r = strada_utf8_downgrade(sv0, fail_ok);
        STRADA_TRY_POP();
        vm->gc_pause--;
        *out_val = r ? sv_to_vm(r) : VM_UNDEF_VAL;
        if (r) strada_decref(r);
        strada_cleanup_drain_to(cm);
        return 0;
    } else {
        STRADA_TRY_POP();
        strada_local_restore_to(lm);
        strada_cleanup_drain_to(cm);
        vm->gc_pause--;
        *out_exc = vm_capture_c_exception();
        return 1;
    }
}

__attribute__((noinline))
static int vm_c_dispatch_signals(VM *vm, VMValue *out_exc) {
    int cm = strada_cleanup_mark();
    int lm = strada_local_depth_get();
    vm->gc_pause++;
    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        strada_dispatch_signals();
        STRADA_TRY_POP();
        vm->gc_pause--;
        strada_cleanup_drain_to(cm);
        return 0;
    } else {
        STRADA_TRY_POP();
        strada_local_restore_to(lm);
        strada_cleanup_drain_to(cm);
        vm->gc_pause--;
        *out_exc = vm_capture_c_exception();
        return 1;
    }
}

__attribute__((noinline))
static int vm_c_call_generic(VM *vm, const VMGenericBuiltin *gb, StradaValue **sa, int argc, StradaValue **out_r, VMValue *out_exc) {
    int cm = strada_cleanup_mark();
    int lm = strada_local_depth_get();
    vm->gc_pause++;
    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        StradaValue *r = NULL;
        switch (argc) {
        case 0: r = ((VMGenericFn0)gb->fn)(); break;
        case 1: r = ((VMGenericFn1)gb->fn)(sa[0]); break;
        case 2: r = ((VMGenericFn2)gb->fn)(sa[0], sa[1]); break;
        case 3: r = ((VMGenericFn3)gb->fn)(sa[0], sa[1], sa[2]); break;
        case 4: r = ((VMGenericFn4)gb->fn)(sa[0], sa[1], sa[2], sa[3]); break;
        }
        STRADA_TRY_POP();
        vm->gc_pause--;
        *out_r = r;
        return 0;
    } else {
        STRADA_TRY_POP();
        strada_local_restore_to(lm);
        strada_cleanup_drain_to(cm);
        vm->gc_pause--;
        *out_exc = vm_capture_c_exception();
        return 1;
    }
}

__attribute__((noinline))
static int vm_c_call_native_sym(VM *vm, VMNativeEntry *ne, StradaValue **call_args, int call_argc, StradaValue **out_nr, VMValue *out_exc) {
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

    int cm = strada_cleanup_mark();
    int lm = strada_local_depth_get();
    vm->gc_pause++;
    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        StradaValue *nr = NULL;
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
        STRADA_TRY_POP();
        vm->gc_pause--;
        *out_nr = nr;
        return 0;
    } else {
        STRADA_TRY_POP();
        strada_local_restore_to(lm);
        strada_cleanup_drain_to(cm);
        vm->gc_pause--;
        *out_exc = vm_capture_c_exception();
        return 1;
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

    StradaValue *nr = NULL;
    VMValue c_exc = VM_UNDEF_VAL;
    if (vm_c_call_native_sym(vm, ne, call_args, call_argc, &nr, &c_exc)) {
        if (va_arr) strada_decref(va_arr);
        for (int i = 0; i < npad; i++) strada_decref(pads[i]);
        for (int i = 0; i < argc; i++) {
            if (sv_args[i]) {
                vm_fh_sync_from_sv(raw_args[i]);
                strada_decref(sv_args[i]);
            }
        }
        if (sv_args != sv_buf) free(sv_args);
        if (call_args != call_buf) free(call_args);
        if (pads != pad_buf) free(pads);
        vm->has_pending_exc = 1;
        vm->pending_exc = c_exc;
        return VM_UNDEF_VAL;
    }

    VMValue ret = nr ? sv_to_vm(nr) : VM_UNDEF_VAL;
    if (nr) strada_decref(nr);

    if (sync_self_hash && argc >= 1 && sv_args[0] && !STRADA_IS_TAGGED_INT(sv_args[0])) {
        StradaValue *ss = sv_args[0];
        if (ss->type == STRADA_REF && !strada_is_slot_ref(ss) && ss->value.rv) ss = ss->value.rv;
        if (ss && !STRADA_IS_TAGGED_INT(ss) && ss->type == STRADA_HASH && ss->value.hv) {
            StradaHash *sshv = ss->value.hv;
            for (size_t cb = 0; cb < sshv->next_slot; cb++) {
                StradaHashEntry *ce = &sshv->entries[cb];
                if (ce->key) {
                    vm_hash_set_n(sync_self_hash, ce->key->data, ce->key->len, sv_to_vm(ce->value));
                }
            }
        }
    }

    if (va_arr) strada_decref(va_arr);
    for (int i = 0; i < npad; i++) strada_decref(pads[i]);
    for (int i = 0; i < argc; i++) {
        if (sv_args[i]) {
            vm_fh_sync_from_sv(raw_args[i]);
            strada_decref(sv_args[i]);
        }
    }
    if (sv_args != sv_buf) free(sv_args);
    if (call_args != call_buf) free(call_args);
    if (pads != pad_buf) free(pads);
    return ret;
}

static inline int vm_memcmp_bytes(const char *a, size_t alen, const char *b, size_t blen) {
    size_t minlen = alen < blen ? alen : blen;
    int c = (minlen > 0) ? memcmp(a, b, minlen) : 0;
    if (c != 0) return c;
    return alen < blen ? -1 : (alen > blen ? 1 : 0);
}

static const char *vm_memmem(const char *hay, size_t hlen, const char *ndl, size_t nlen) {
    if (nlen == 0) return hay;
    if (!hay || !ndl || nlen > hlen) return NULL;
    const char *end = hay + hlen - nlen + 1;
    for (const char *p = hay; p < end; ) {
        const char *m = (const char *)memchr(p, (unsigned char)ndl[0], (size_t)(end - p));
        if (!m) return NULL;
        if (nlen == 1 || memcmp(m + 1, ndl + 1, nlen - 1) == 0) return m;
        p = m + 1;
    }
    return NULL;
}

static int vm_utf8_is_valid_str(const char *s, size_t len, int *out_nonascii) {
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    int nonascii = 0;
    while (i < len) {
        unsigned char c = p[i];
        int needed;
        if (c < 0x80) { i++; continue; }
        nonascii = 1;
        if (c >= 0xC2 && c <= 0xDF) needed = 1;
        else if ((c & 0xF0) == 0xE0) needed = 2;
        else if (c >= 0xF0 && c <= 0xF4) needed = 3;
        else return 0;
        if (i + (size_t)needed >= len) return 0;
        for (int j = 1; j <= needed; j++) {
            if ((p[i + j] & 0xC0) != 0x80) return 0;
        }
        if (needed == 2) {
            uint32_t cp = ((uint32_t)(c & 0x0F) << 12) | ((uint32_t)(p[i + 1] & 0x3F) << 6) |
                          (uint32_t)(p[i + 2] & 0x3F);
            if (cp < 0x800 || (cp >= 0xD800 && cp <= 0xDFFF)) return 0;
        } else if (needed == 3) {
            uint32_t cp = ((uint32_t)(c & 0x07) << 18) | ((uint32_t)(p[i + 1] & 0x3F) << 12) |
                          ((uint32_t)(p[i + 2] & 0x3F) << 6) | (uint32_t)(p[i + 3] & 0x3F);
            if (cp < 0x10000 || cp > 0x10FFFF) return 0;
        }
        i += (size_t)needed + 1;
    }
    if (out_nonascii) *out_nonascii = nonascii;
    return 1;
}

static inline int vm_str_is_utf8_chars(VMValue v, const char *s, size_t blen) {
    if (!vm_is_utf8(v)) return 0;
    int nonascii = 0;
    return vm_utf8_is_valid_str(s, blen, &nonascii) && nonascii;
}

static size_t vm_utf8_char_count(const char *s, size_t blen) {
    size_t count = 0;
    for (size_t i = 0; i < blen; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) count++;
    }
    return count;
}

static size_t vm_utf8_char_to_byte(const char *s, size_t blen, int64_t char_idx) {
    if (char_idx <= 0) return 0;
    size_t b = 0;
    int64_t c = 0;
    while (b < blen) {
        if (((unsigned char)s[b] & 0xC0) != 0x80) {
            if (c == char_idx) return b;
            c++;
        }
        b++;
    }
    return blen;
}

static int64_t vm_utf8_byte_to_char(const char *s, size_t byte_off) {
    int64_t c = 0;
    for (size_t i = 0; i < byte_off; i++) {
        if (((unsigned char)s[i] & 0xC0) != 0x80) c++;
    }
    return c;
}

static VMValue vm_reverse_str_val(VMValue v) {
    char buf[1024];
    size_t len = 0;
    const char *s = vm_bytes_view(v, buf, sizeof(buf), &len);
    int u8 = vm_is_utf8(v);
    if (len == 0) return vm_make_str_flag("", 0, u8);
    char *rev = malloc(len + 1);
    if (vm_str_is_utf8_chars(v, s, len)) {
        size_t i = 0, out_pos = len;
        while (i < len) {
            unsigned char c = (unsigned char)s[i];
            size_t step = 1;
            if (c >= 0xC2 && c <= 0xDF && i + 1 < len) step = 2;
            else if ((c & 0xF0) == 0xE0 && i + 2 < len) step = 3;
            else if (c >= 0xF0 && c <= 0xF4 && i + 3 < len) step = 4;
            out_pos -= step;
            memcpy(rev + out_pos, s + i, step);
            i += step;
        }
    } else {
        for (size_t i = 0; i < len; i++) rev[i] = s[len - 1 - i];
    }
    rev[len] = '\0';
    VMValue r = vm_make_str_flag(rev, len, u8);
    free(rev);
    return r;
}

static VMValue vm_str_case_transform(VMValue v, int to_upper) {
    char buf[256];
    size_t len = 0;
    const char *s = vm_bytes_view(v, buf, sizeof(buf), &len);
    int u8 = vm_is_utf8(v);
    int has_nonascii = 0;
    if (u8) {
        for (size_t i = 0; i < len; i++) {
            if ((unsigned char)s[i] >= 0x80) { has_nonascii = 1; break; }
        }
    }
    if (!u8 || !has_nonascii) {
        VMString *vs = malloc(sizeof(VMString) + len + 1);
        vm_gc_init_hdr(&vs->hdr, VM_OBJ_STR);
        if (u8) vs->hdr.is_utf8 = 1;
        vs->len = (uint32_t)len;
        for (size_t i = 0; i < len; i++) {
            vs->data[i] = to_upper ? (char)toupper((unsigned char)s[i])
                                   : (char)tolower((unsigned char)s[i]);
        }
        vs->data[len] = '\0';
        return VM_MAKE_PTR(vs);
    }
    size_t ocap = len + 32, olen = 0;
    char *out = malloc(ocap);
    size_t pos = 0;
    while (pos < len) {
        const char *nul = (const char *)memchr(s + pos, '\0', len - pos);
        size_t seg_len = nul ? (size_t)(nul - (s + pos)) : (len - pos);
        if (seg_len > 0) {
            char *tmp = malloc(seg_len + 1);
            memcpy(tmp, s + pos, seg_len);
            tmp[seg_len] = '\0';
            char *mapped = to_upper ? strada_uc(tmp) : strada_lc(tmp);
            free(tmp);
            size_t mlen = mapped ? strlen(mapped) : 0;
            if (olen + mlen + 2 > ocap) {
                while (olen + mlen + 2 > ocap) ocap *= 2;
                out = realloc(out, ocap);
            }
            if (mlen > 0) memcpy(out + olen, mapped, mlen);
            olen += mlen;
            free(mapped);
        }
        if (nul) {
            if (olen + 2 > ocap) { ocap *= 2; out = realloc(out, ocap); }
            out[olen++] = '\0';
            pos += seg_len + 1;
        } else {
            break;
        }
    }
    VMValue r = vm_make_str_flag(out, olen, 1);
    free(out);
    return r;
}

static VMValue vm_ucfirst_val(VMValue v, int to_upper) {
    char buf[256];
    size_t len = 0;
    const char *s = vm_bytes_view(v, buf, sizeof(buf), &len);
    int u8 = vm_is_utf8(v);
    if (len == 0) return vm_make_str_flag("", 0, u8);
    if (!u8 || (unsigned char)s[0] < 0x80) {
        char *r = malloc(len + 1);
        memcpy(r, s, len);
        r[len] = '\0';
        if (to_upper && r[0] >= 'a' && r[0] <= 'z') r[0] -= 32;
        else if (!to_upper && r[0] >= 'A' && r[0] <= 'Z') r[0] += 32;
        VMValue out = vm_make_str_flag(r, len, u8);
        free(r);
        return out;
    }
    const char *nul = (const char *)memchr(s, '\0', len);
    size_t head_len = nul ? (size_t)(nul - s) : len;
    if (head_len == 0) return vm_make_str_flag(s, len, u8);
    size_t cplen = 1;
    unsigned char c0 = (unsigned char)s[0];
    if ((c0 & 0xE0) == 0xC0) cplen = 2;
    else if ((c0 & 0xF0) == 0xE0) cplen = 3;
    else if ((c0 & 0xF8) == 0xF0) cplen = 4;
    if (cplen > head_len) return vm_make_str_flag(s, len, u8);
    char first_cp[8];
    memcpy(first_cp, s, cplen);
    first_cp[cplen] = '\0';
    char *mapped = to_upper ? strada_ucfirst(first_cp) : strada_lcfirst(first_cp);
    size_t mlen = mapped ? strlen(mapped) : 0;
    size_t out_len = mlen + (len - cplen);
    char *r = malloc(out_len + 1);
    if (mlen > 0) memcpy(r, mapped, mlen);
    if (len > cplen) memcpy(r + mlen, s + cplen, len - cplen);
    r[out_len] = '\0';
    free(mapped);
    VMValue out = vm_make_str_flag(r, out_len, u8);
    free(r);
    return out;
}

static VMValue vm_execute_call_cl(VM *vm, int func_idx, VMClosure *closure, VMValue *cargs, int cargc, int passed_argc);

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
        VMValue res = vm_execute_call_cl(vm, fidx, NULL, pargs, pargc, argc);
        if (pargs != pbuf) free(pargs);
        return res;
    }
    return vm_execute_call_cl(vm, fidx, NULL, args, argc, argc);
}

static VMValue vm_invoke_closure(VM *vm, VMClosure *vcl, VMValue *args, int argc) {
    VMValue *call_args = args;
    int eff_argc = argc;
    VMValue bound_buf[17];
    VMValue *bound_arr = NULL;
    if (vcl->has_bound_arg) {
        eff_argc = argc + 1;
        bound_arr = (eff_argc > 17) ? malloc((size_t)eff_argc * sizeof(VMValue)) : bound_buf;
        bound_arr[0] = vcl->bound_arg;
        for (int i = 0; i < argc; i++) bound_arr[1 + i] = args[i];
        call_args = bound_arr;
    }
    VMChunk *callee = &vm->program->funcs[vcl->func_idx];
    VMValue res;
    if (callee->has_variadic) {
        int fixed = callee->fixed_param_count;
        if (fixed < 0) fixed = 0;
        int vc = eff_argc - fixed;
        VMArray *va = vm_array_new(vc > 0 ? vc : 4);
        for (int i = fixed; i < eff_argc; i++) vm_array_push(va, vm_val_copy(call_args[i]));
        VMValue pbuf[16];
        int pargc = fixed + 1;
        VMValue *pargs = (pargc > 16) ? malloc((size_t)pargc * sizeof(VMValue)) : pbuf;
        for (int i = 0; i < fixed; i++) {
            pargs[i] = (i < eff_argc) ? call_args[i] : VM_UNDEF_VAL;
        }
        pargs[fixed] = VM_MAKE_PTR(va);
        res = vm_execute_call_cl(vm, vcl->func_idx, vcl, pargs, pargc, eff_argc);
        if (pargs != pbuf) free(pargs);
    } else {
        res = vm_execute_call_cl(vm, vcl->func_idx, vcl, call_args, eff_argc, eff_argc);
    }
    if (bound_arr && bound_arr != bound_buf) free(bound_arr);
    return res;
}

static void vm_exported_closure_free(void *raw) {
    VMExportedClosureCtx *ctx = (VMExportedClosureCtx *)raw;
    if (!ctx) return;
    if (ctx->vcl) {
        if (__sync_sub_and_fetch(&ctx->vcl->pin_count, 1) <= 0) {
            ctx->vcl->pin_count = 0;
            ctx->vcl->hdr.gc_immortal = 0;
        }
    }
    VM *vm = ctx->vm;
    VMProgram *prog = ctx->prog;
    free(ctx);
    if (vm) vm_free(vm);
    if (prog) vm_program_free(prog);
}

static StradaValue* vm_exported_closure_call(void *raw, int argc, StradaValue **argv) {
    VMExportedClosureCtx *ctx = (VMExportedClosureCtx *)raw;
    if (!ctx || !ctx->vm || !ctx->vcl) return strada_new_undef();
    int use_worker = !(!ctx->vm->is_worker_vm && pthread_equal(pthread_self(), ctx->vm->owner_thread));
    VM *run_vm = use_worker ? vm_new_worker(ctx->vm) : ctx->vm;
    VM *prev_active = use_worker ? run_vm->saved_active_vm : g_active_vm;
    g_active_vm = run_vm;
    int prev_trap = run_vm->trap_uncaught;
    run_vm->trap_uncaught = 1;
    run_vm->gc_pause++;
    VMValue vargs_buf[16];
    VMValue *vargs = (argc > 16) ? malloc((size_t)argc * sizeof(VMValue)) : vargs_buf;
    for (int i = 0; i < argc; i++) {
        vargs[i] = argv[i] ? sv_to_vm(argv[i]) : VM_UNDEF_VAL;
    }
    run_vm->gc_pause--;
    VMValue vres = vm_invoke_closure(run_vm, ctx->vcl, vargs, argc);
    if (vargs != vargs_buf) free(vargs);
    int had_exc = (run_vm->has_exception || run_vm->has_pending_exc) ? 1 : 0;
    StradaValue *out_sv = NULL;
    if (had_exc) {
        VMValue vexc = run_vm->has_exception ? run_vm->current_exception : run_vm->pending_exc;
        run_vm->has_exception = 0;
        run_vm->current_exception = VM_UNDEF_VAL;
        run_vm->has_pending_exc = 0;
        run_vm->pending_exc = VM_UNDEF_VAL;
        run_vm->gc_pause++;
        out_sv = vm_to_sv(vexc);
        run_vm->gc_pause--;
    } else {
        run_vm->gc_pause++;
        out_sv = vm_to_sv(vres);
        run_vm->gc_pause--;
    }
    if (!use_worker && run_vm->gc_count >= run_vm->gc_threshold &&
        run_vm->exec_base == 0 && run_vm->gc_pause == 0) {
        vm_gc_collect(run_vm);
    }
    run_vm->trap_uncaught = prev_trap;
    if (use_worker) {
        vm_suspend(run_vm);
        vm_free(run_vm);
    }
    g_active_vm = prev_active;
    if (had_exc) {
        strada_throw_value(out_sv);
    }
    return out_sv;
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
    const char *acls = vm_obj_class_name(a);
    const char *bcls = vm_obj_class_name(b);
    if (!acls && !bcls) return 0;

    if (acls) {
        int fidx = vm_resolve_overload_func(vm->program, acls, op);
        if (fidx >= 0) {
            VMValue args[3] = { a, b, VM_MAKE_INT(0) };
            *out = vm_invoke_func(vm, fidx, args, 3);
            return 1;
        }
    }
    if (bcls) {
        int fidx = vm_resolve_overload_func(vm->program, bcls, op);
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
    if (acls) {
        fidx = vm_resolve_overload_func(vm->program, acls, fb_op);
        if (fidx >= 0) { args[0] = a; args[1] = b; args[2] = VM_MAKE_INT(0); }
    }
    if (fidx < 0 && bcls) {
        fidx = vm_resolve_overload_func(vm->program, bcls, fb_op);
        if (fidx >= 0) { args[0] = b; args[1] = a; args[2] = VM_MAKE_INT(1); }
    }
    if (fidx < 0) return 0;

    VMValue res = vm_invoke_func(vm, fidx, args, 3);
    if (vm->has_pending_exc) { *out = VM_UNDEF_VAL; return 1; }
    double r = vm_to_num(res);
    int bool_val = 0;
    if (isnan(r)) {
        bool_val = (strcmp(op, "!=") == 0 || strcmp(op, "ne") == 0) ? 1 : 0;
    } else if (is_num_cmp) {
        if (strcmp(op, "<") == 0) bool_val = (r < 0.0);
        else if (strcmp(op, "<=") == 0) bool_val = (r <= 0.0);
        else if (strcmp(op, ">") == 0) bool_val = (r > 0.0);
        else if (strcmp(op, ">=") == 0) bool_val = (r >= 0.0);
        else if (strcmp(op, "==") == 0) bool_val = (r == 0.0);
        else bool_val = (r != 0.0);
    } else {
        if (strcmp(op, "lt") == 0) bool_val = (r < 0.0);
        else if (strcmp(op, "le") == 0) bool_val = (r <= 0.0);
        else if (strcmp(op, "gt") == 0) bool_val = (r > 0.0);
        else if (strcmp(op, "ge") == 0) bool_val = (r >= 0.0);
        else if (strcmp(op, "eq") == 0) bool_val = (r == 0.0);
        else bool_val = (r != 0.0);
    }
    *out = VM_MAKE_INT(bool_val);
    return 1;
}

static int vm_try_un_overload(VM *vm, const char *op, VMValue a, VMValue *out) {
    const char *acls = vm_obj_class_name(a);
    if (!acls) return 0;
    int fidx = vm_resolve_overload_func(vm->program, acls, op);
    if (fidx < 0) return 0;
    VMValue args[1] = { a };
    *out = vm_invoke_func(vm, fidx, args, 1);
    return 1;
}

static VMValue vm_stringify_ov(VM *vm, VMValue v) {
    if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_ARRAY) {
        VMArray *a = VM_AS_ARRAY(v);
        if (!VM_IS_UNDEF(a->tied_obj)) {
            return VM_MAKE_INT(vm_tied_array_fetchsize(vm, a->tied_obj));
        }
    }
    if (vm_has_stringify(vm->program, v)) {
        const char *cname = vm_obj_class_name(v);
        int fidx = cname ? vm_resolve_overload_func(vm->program, cname, "\"\"") : -1;
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

static int vm_path_join_append(VM *vm, char **buf, size_t *cap, size_t *len, int *out_utf8, VMValue elem, int is_first) {
    if (vm_has_stringify(vm->program, elem)) {
        vm->gc_pause++;
        elem = vm_stringify_ov(vm, elem);
        vm->gc_pause--;
        if (vm->has_pending_exc) return 1;
    }
    char pbuf[1024];
    size_t plen = 0;
    const char *p = vm_bytes_view(elem, pbuf, sizeof(pbuf), &plen);
    if (vm_is_utf8(elem)) *out_utf8 = 1;
    int need_slash = (!is_first && *len > 0 && (*buf)[*len - 1] != '/') ? 1 : 0;
    size_t need = *len + (size_t)need_slash + plen + 1;
    if (need > *cap) {
        size_t ncap = (*cap == 0) ? 256 : *cap;
        while (ncap < need) ncap *= 2;
        char *nb = realloc(*buf, ncap);
        if (!nb) return -1;
        *buf = nb;
        *cap = ncap;
    }
    if (need_slash) (*buf)[(*len)++] = '/';
    if (plen > 0) {
        memcpy(*buf + *len, p, plen);
        *len += plen;
    }
    return 0;
}

static int vm_tied_array_find_method(VM *vm, VMValue tied_obj, const char *method) {
    const char *tcls = vm_obj_class_name(tied_obj);
    if (tcls) {
        return vm_find_method(vm->program, tcls, method);
    }
    return -1;
}

static VMValue vm_tied_array_call(VM *vm, VMValue tied_obj, int fidx, VMValue *extra, int extra_argc) {
    if (fidx < 0) return VM_UNDEF_VAL;
    VMValue buf[16];
    int total = 1 + extra_argc;
    VMValue *args = (total > 16) ? malloc((size_t)total * sizeof(VMValue)) : buf;
    args[0] = tied_obj;
    for (int i = 0; i < extra_argc; i++) {
        args[1 + i] = vm_val_copy(extra[i]);
    }
    vm->gc_pause++;
    VMValue res = vm_invoke_func(vm, fidx, args, total);
    vm->gc_pause--;
    if (args != buf) free(args);
    return res;
}

static VMValue vm_tied_scalar_fetch(VM *vm, VMValue ts_val) {
    if (!(VM_IS_PTR(ts_val) && VM_PTR_TYPE(ts_val) == VM_OBJ_TIED_SCALAR)) return ts_val;
    VMTiedScalar *ts = VM_AS_TIED_SCALAR(ts_val);
    int fidx = vm_tied_array_find_method(vm, ts->tied_obj, "FETCH");
    if (fidx >= 0) return vm_tied_array_call(vm, ts->tied_obj, fidx, NULL, 0);
    return VM_UNDEF_VAL;
}

static void vm_tied_scalar_store(VM *vm, VMValue ts_val, VMValue newv) {
    if (!(VM_IS_PTR(ts_val) && VM_PTR_TYPE(ts_val) == VM_OBJ_TIED_SCALAR)) return;
    VMTiedScalar *ts = VM_AS_TIED_SCALAR(ts_val);
    int fidx = vm_tied_array_find_method(vm, ts->tied_obj, "STORE");
    if (fidx >= 0) vm_tied_array_call(vm, ts->tied_obj, fidx, &newv, 1);
}

static int64_t vm_tied_array_fetchsize(VM *vm, VMValue tied_obj) {
    int fidx = vm_tied_array_find_method(vm, tied_obj, "FETCHSIZE");
    if (fidx < 0) return 0;
    VMValue r = vm_tied_array_call(vm, tied_obj, fidx, NULL, 0);
    if (vm->has_pending_exc) return 0;
    return vm_to_int(r);
}

static int vm_collect_spread_items(VM *vm, VMValue arr_val, VMValue *stk_buf, int stk_cap, VMValue **out_items, int *out_count) {
    *out_items = stk_buf;
    *out_count = 0;
    if (!(VM_IS_PTR(arr_val) && VM_PTR_TYPE(arr_val) == VM_OBJ_ARRAY)) return 0;
    VMArray *sa = VM_AS_ARRAY(arr_val);
    if (VM_IS_UNDEF(sa->tied_obj)) {
        int n = sa->size;
        if (n <= 0) return 0;
        VMValue *dst = (n > stk_cap) ? malloc((size_t)n * sizeof(VMValue)) : stk_buf;
        for (int i = 0; i < n; i++) dst[i] = sa->items[i];
        *out_items = dst;
        *out_count = n;
        return 0;
    }
    vm->gc_pause++;
    int64_t sz = vm_tied_array_fetchsize(vm, sa->tied_obj);
    if (vm->has_pending_exc) { vm->gc_pause--; return 1; }
    int n = (sz > 0) ? (int)sz : 0;
    if (n == 0) { vm->gc_pause--; return 0; }
    VMValue *dst = (n > stk_cap) ? malloc((size_t)n * sizeof(VMValue)) : stk_buf;
    int ffidx = vm_tied_array_find_method(vm, sa->tied_obj, "FETCH");
    for (int i = 0; i < n; i++) {
        VMValue iv = VM_MAKE_INT(i);
        dst[i] = (ffidx >= 0) ? vm_tied_array_call(vm, sa->tied_obj, ffidx, &iv, 1) : VM_UNDEF_VAL;
        if (vm->has_pending_exc) {
            vm->gc_pause--;
            if (dst != stk_buf) free(dst);
            return 1;
        }
    }
    vm->gc_pause--;
    *out_items = dst;
    *out_count = n;
    return 0;
}

static VMArray *vm_materialize_array(VM *vm, VMArray *src) {
    if (!src) return vm_array_new(0);
    if (VM_IS_UNDEF(src->tied_obj)) {
        VMArray *cp = vm_array_new(src->size > 0 ? src->size : 8);
        for (int i = 0; i < src->size; i++) vm_array_push(cp, vm_val_copy(src->items[i]));
        return cp;
    }
    vm->gc_pause++;
    int64_t sz = vm_tied_array_fetchsize(vm, src->tied_obj);
    if (vm->has_pending_exc) { vm->gc_pause--; return NULL; }
    int n = (sz > 0) ? (int)sz : 0;
    VMArray *cp = vm_array_new(n > 0 ? n : 8);
    int ffidx = vm_tied_array_find_method(vm, src->tied_obj, "FETCH");
    for (int i = 0; i < n; i++) {
        VMValue iv = VM_MAKE_INT(i);
        VMValue v = (ffidx >= 0) ? vm_tied_array_call(vm, src->tied_obj, ffidx, &iv, 1) : VM_UNDEF_VAL;
        if (vm->has_pending_exc) { vm->gc_pause--; return NULL; }
        vm_array_push(cp, v);
    }
    vm->gc_pause--;
    return cp;
}

static inline VMValue vm_tied_key_str(VMValue k) {
    if (VM_IS_PTR(k)) {
        enum VMObjType t = VM_PTR_TYPE(k);
        if (t == VM_OBJ_STR) return vm_val_copy(k);
        if (t == VM_OBJ_STRBUF) {
            VMStrBuf *sb = VM_AS_STRBUF(k);
            return vm_make_str_flag(sb->data ? sb->data : "", sb->len, sb->hdr.is_utf8);
        }
    }
    char kbuf[256];
    return vm_str(vm_to_cstr(k, kbuf, sizeof(kbuf)));
}

static VMArray *vm_tied_hash_collect_keys(VM *vm, VMValue tied_obj, int *has_firstkey) {
    int fk_fidx = vm_tied_array_find_method(vm, tied_obj, "FIRSTKEY");
    if (has_firstkey) *has_firstkey = (fk_fidx >= 0);
    if (fk_fidx < 0) return NULL;
    int nk_fidx = vm_tied_array_find_method(vm, tied_obj, "NEXTKEY");
    VMArray *keys = vm_array_new(8);
    vm->gc_pause++;
    VMValue k = vm_tied_array_call(vm, tied_obj, fk_fidx, NULL, 0);
    while (!vm->has_pending_exc && !VM_IS_UNDEF(k)) {
        vm_array_push(keys, vm_tied_key_str(k));
        if (nk_fidx < 0) break;
        k = vm_tied_array_call(vm, tied_obj, nk_fidx, &k, 1);
    }
    vm->gc_pause--;
    if (vm->has_pending_exc) return NULL;
    return keys;
}

static void vm_unwrap_container_weak_gc(VM *vm, VMValue cont, const VMValue *roots, int nroots) {
    if (!vm || !vm->has_weakrefs || vm->exec_base > 0 || vm->gc_pause > 0 || !VM_IS_PTR(cont)) return;
    int has_live_wr = 0;
    enum VMObjType ct = VM_PTR_TYPE(cont);
    if (ct == VM_OBJ_ARRAY) {
        VMArray *a = VM_AS_ARRAY(cont);
        for (int i = 0; i < a->size; i++) {
            VMValue el = a->items[i];
            if (VM_IS_PTR(el) && VM_PTR_TYPE(el) == VM_OBJ_WEAKREF && !VM_IS_UNDEF(VM_AS_WEAKREF(el)->target)) {
                has_live_wr = 1;
                break;
            }
        }
    } else if (ct == VM_OBJ_HASH) {
        VMHash *h = VM_AS_HASH(cont);
        for (int i = 0; i < h->capacity; i++) {
            if (h->entries[i].occupied == 1) {
                VMValue el = h->entries[i].value;
                if (VM_IS_PTR(el) && VM_PTR_TYPE(el) == VM_OBJ_WEAKREF && !VM_IS_UNDEF(VM_AS_WEAKREF(el)->target)) {
                    has_live_wr = 1;
                    break;
                }
            }
        }
    }
    if (!has_live_wr) return;
    size_t saved_sp = vm->stack_top;
    if (vm->stack_top + 1 + (size_t)nroots > vm->stack_cap) {
        vm_stack_grow(vm, vm->stack_top + 1 + (size_t)nroots);
    }
    vm->stack[vm->stack_top++] = cont;
    for (int i = 0; i < nroots; i++) {
        if (roots && !VM_IS_UNDEF(roots[i])) {
            vm->stack[vm->stack_top++] = roots[i];
        }
    }
    vm_gc_collect(vm);
    vm->stack_top = saved_sp;
}

static VMArray *vm_hash_to_flat_kv(VM *vm, VMHash *h) {
    if (!h) return vm_array_new(0);
    if (__builtin_expect(!VM_IS_UNDEF(h->tied_obj), 0)) {
        int has_fk = 0;
        VMArray *keys = vm_tied_hash_collect_keys(vm, h->tied_obj, &has_fk);
        if (vm->has_pending_exc) return NULL;
        if (has_fk) {
            int n = keys ? keys->size : 0;
            VMArray *flat = vm_array_new(n > 0 ? n * 2 : 8);
            int ffidx = vm_tied_array_find_method(vm, h->tied_obj, "FETCH");
            vm->gc_pause++;
            for (int i = 0; i < n; i++) {
                VMValue kv = keys->items[i];
                VMValue vv = (ffidx >= 0) ? vm_tied_array_call(vm, h->tied_obj, ffidx, &kv, 1) : VM_UNDEF_VAL;
                if (vm->has_pending_exc) { vm->gc_pause--; return NULL; }
                vm_array_push(flat, vm_val_copy(kv));
                vm_array_push(flat, vv);
            }
            vm->gc_pause--;
            return flat;
        }
    }
    if (vm->has_weakrefs) {
        vm_unwrap_container_weak_gc(vm, VM_MAKE_PTR(h), NULL, 0);
    }
    VMArray *flat = vm_array_new(h->size > 0 ? h->size * 2 : 8);
    for (int i = 0; i < h->capacity; i++) {
        if (h->entries[i].occupied == 1) {
            int nonascii = 0;
            int u8 = (vm_utf8_is_valid_str(h->entries[i].key, h->entries[i].keylen, &nonascii) && nonascii);
            vm_array_push(flat, vm_make_str_flag(h->entries[i].key, h->entries[i].keylen, u8));
            vm_array_push(flat, vm_val_copy(h->entries[i].value));
        }
    }
    return flat;
}

static VMArray *vm_sort_materialize_input(VM *vm, VMValue input, int allow_scalar) {
    if (vm && vm->has_weakrefs && VM_IS_PTR(input) && VM_PTR_TYPE(input) == VM_OBJ_WEAKREF) {
        input = vm_unwrap_weak_gc(vm, input, NULL, 0);
    }
    if (VM_IS_PTR(input)) {
        enum VMObjType t = VM_PTR_TYPE(input);
        if (t == VM_OBJ_ARRAY) {
            if (vm && vm->has_weakrefs) vm_unwrap_container_weak_gc(vm, input, NULL, 0);
            return vm_materialize_array(vm, VM_AS_ARRAY(input));
        }
        if (t == VM_OBJ_HASH) {
            return vm_hash_to_flat_kv(vm, VM_AS_HASH(input));
        }
        if (t == VM_OBJ_NATIVE_SV) {
            StradaValue *sv = ((VMNativeSV*)VM_TO_PTR(input))->sv;
            StradaValue *msv = sv ? strada_hv_unwrap_meta_sv(sv) : NULL;
            if (msv && (msv->type == STRADA_ARRAY || msv->type == STRADA_HASH)) {
                StradaValue *cp = strada_sort_copy_input(sv);
                VMArray *out = vm_array_new(cp && cp->value.av ? (int)cp->value.av->size : 0);
                if (cp && cp->value.av) {
                    for (size_t i = 0; i < cp->value.av->size; i++) {
                        vm_array_push(out, sv_to_vm(strada_array_get(cp->value.av, (int64_t)i)));
                    }
                }
                if (cp) strada_decref(cp);
                return out;
            }
            if (!allow_scalar) return vm_array_new(0);
            VMArray *one = vm_array_new(1);
            vm_array_push(one, vm_val_copy(input));
            return one;
        }
    }
    if (allow_scalar) {
        VMArray *one = vm_array_new(1);
        vm_array_push(one, vm_val_copy(input));
        return one;
    }
    return vm_array_new(0);
}

typedef struct {
    VMValue val;
    const char *p;
    size_t len;
    char *owned;
    size_t orig_idx;
} VMSortStrDecor;

static int vm_sort_str_decor_cmp(const void *a, const void *b) {
    const VMSortStrDecor *da = (const VMSortStrDecor *)a;
    const VMSortStrDecor *db = (const VMSortStrDecor *)b;
    int c = vm_memcmp_bytes(da->p, da->len, db->p, db->len);
    if (c != 0) return c;
    if (da->orig_idx < db->orig_idx) return -1;
    if (da->orig_idx > db->orig_idx) return 1;
    return 0;
}

typedef struct {
    VMValue val;
    double num;
    size_t orig_idx;
} VMSortNumDecor;

static int vm_sort_num_decor_cmp(const void *a, const void *b) {
    const VMSortNumDecor *da = (const VMSortNumDecor *)a;
    const VMSortNumDecor *db = (const VMSortNumDecor *)b;
    if (da->num < db->num) return -1;
    if (da->num > db->num) return 1;
    int na = isnan(da->num);
    int nb = isnan(db->num);
    if (na != nb) return na ? 1 : -1;
    if (da->orig_idx < db->orig_idx) return -1;
    if (da->orig_idx > db->orig_idx) return 1;
    return 0;
}

static double vm_to_num_ov(VM *vm, VMValue v, int depth) {
    if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_WEAKREF) {
        v = VM_AS_WEAKREF(v)->target;
    }
    if (depth < 8 && vm && vm->program && vm_obj_class_name(v) != NULL) {
        VMValue ov = VM_UNDEF_VAL;
        if (vm_try_un_overload(vm, "0+", v, &ov)) {
            if (vm->has_pending_exc) return 0.0;
            return vm_to_num_ov(vm, ov, depth + 1);
        }
    }
    return vm_to_num(v);
}

static VMArray *vm_sort_default_impl(VM *vm, VMValue input) {
    VMArray *sorted = vm_sort_materialize_input(vm, input, 1);
    if (vm->has_pending_exc) return NULL;
    if (!sorted || sorted->size <= 1) return sorted ? sorted : vm_array_new(0);
    int n = sorted->size;
    VMSortStrDecor *d = calloc((size_t)n, sizeof(VMSortStrDecor));
    if (!d) return sorted;
    vm->gc_pause++;
    for (int i = 0; i < n; i++) {
        VMValue key = sorted->items[i];
        d[i].val = key;
        d[i].orig_idx = (size_t)i;
        if (vm_has_stringify(vm->program, key)) {
            VMValue sk = vm_stringify_ov(vm, key);
            if (vm->has_pending_exc) {
                vm->gc_pause--;
                for (int j = 0; j < i; j++) free(d[j].owned);
                free(d);
                return NULL;
            }
            char sbuf[256];
            size_t slen = 0;
            const char *ss = vm_bytes_view(sk, sbuf, sizeof(sbuf), &slen);
            d[i].owned = malloc(slen > 0 ? slen : 1);
            if (slen > 0 && d[i].owned) memcpy(d[i].owned, ss, slen);
            d[i].p = d[i].owned ? d[i].owned : "";
            d[i].len = slen;
        } else if (VM_IS_PTR(key) && VM_PTR_TYPE(key) == VM_OBJ_STR) {
            d[i].p = VM_AS_STR(key)->data;
            d[i].len = VM_AS_STR(key)->len;
        } else {
            char kbuf[256];
            size_t klen = 0;
            const char *ks = vm_bytes_view(key, kbuf, sizeof(kbuf), &klen);
            d[i].owned = malloc(klen > 0 ? klen : 1);
            if (klen > 0 && d[i].owned) memcpy(d[i].owned, ks, klen);
            d[i].p = d[i].owned ? d[i].owned : "";
            d[i].len = klen;
        }
    }
    qsort(d, (size_t)n, sizeof(VMSortStrDecor), vm_sort_str_decor_cmp);
    for (int i = 0; i < n; i++) {
        sorted->items[i] = d[i].val;
        free(d[i].owned);
    }
    free(d);
    vm->gc_pause--;
    return sorted;
}

static VMArray *vm_nsort_impl(VM *vm, VMValue input) {
    VMArray *sorted = vm_sort_materialize_input(vm, input, 0);
    if (vm->has_pending_exc) return NULL;
    if (!sorted || sorted->size <= 1) return sorted ? sorted : vm_array_new(0);
    int n = sorted->size;
    VMSortNumDecor *d = malloc((size_t)n * sizeof(VMSortNumDecor));
    if (!d) return sorted;
    vm->gc_pause++;
    for (int i = 0; i < n; i++) {
        d[i].val = sorted->items[i];
        d[i].orig_idx = (size_t)i;
        d[i].num = vm_to_num_ov(vm, sorted->items[i], 0);
        if (vm->has_pending_exc) {
            vm->gc_pause--;
            free(d);
            return NULL;
        }
    }
    qsort(d, (size_t)n, sizeof(VMSortNumDecor), vm_sort_num_decor_cmp);
    for (int i = 0; i < n; i++) {
        sorted->items[i] = d[i].val;
    }
    free(d);
    vm->gc_pause--;
    return sorted;
}

static VMArray *vm_tied_array_splice(VM *vm, VMArray *arr, int argc, VMValue *args) {
    vm->gc_pause++;
    int64_t sz = vm_tied_array_fetchsize(vm, arr->tied_obj);
    if (vm->has_pending_exc) { vm->gc_pause--; return vm_array_new(0); }
    if (sz < 0) sz = 0;

    int64_t offset = (argc >= 2 && !VM_IS_UNDEF(args[1])) ? vm_to_int(args[1]) : 0;
    if (offset < 0) offset = sz + offset;
    if (offset < 0) offset = 0;
    if (offset > sz) offset = sz;

    int64_t length;
    if (argc >= 3 && !VM_IS_UNDEF(args[2])) {
        length = vm_to_int(args[2]);
        if (length < 0) {
            length = (sz - offset) + length;
            if (length < 0) length = 0;
        }
        if (length > sz - offset) length = sz - offset;
    } else {
        length = sz - offset;
    }

    VMValue snap_buf[16];
    VMValue *snap = NULL;
    const VMValue *repl_items = NULL;
    int repl_count = 0;
    VMValue single_item = VM_UNDEF_VAL;
    if (argc == 4) {
        VMValue rarg = args[3];
        if (VM_IS_UNDEF(rarg)) {
            repl_count = 0;
        } else if (VM_IS_PTR(rarg) && VM_PTR_TYPE(rarg) == VM_OBJ_ARRAY) {
            VMArray *ra = VM_AS_ARRAY(rarg);
            if (!VM_IS_UNDEF(ra->tied_obj)) {
                int64_t rsz = vm_tied_array_fetchsize(vm, ra->tied_obj);
                if (vm->has_pending_exc) { vm->gc_pause--; return vm_array_new(0); }
                repl_count = (rsz > 0) ? (int)rsz : 0;
                if (repl_count > 0) {
                    snap = (repl_count > 16) ? malloc((size_t)repl_count * sizeof(VMValue)) : snap_buf;
                    int rff = vm_tied_array_find_method(vm, ra->tied_obj, "FETCH");
                    for (int i = 0; i < repl_count; i++) {
                        VMValue iv = VM_MAKE_INT(i);
                        snap[i] = (rff >= 0) ? vm_tied_array_call(vm, ra->tied_obj, rff, &iv, 1) : VM_UNDEF_VAL;
                        if (vm->has_pending_exc) {
                            if (snap != snap_buf) free(snap);
                            vm->gc_pause--;
                            return vm_array_new(0);
                        }
                    }
                    repl_items = snap;
                }
            } else {
                repl_count = ra->size;
                if (ra == arr && repl_count > 0) {
                    snap = (repl_count > 16) ? malloc((size_t)repl_count * sizeof(VMValue)) : snap_buf;
                    for (int i = 0; i < repl_count; i++) snap[i] = vm_val_copy(ra->items[i]);
                    repl_items = snap;
                } else {
                    repl_items = ra->items;
                }
            }
        } else {
            single_item = rarg;
            repl_items = &single_item;
            repl_count = 1;
        }
    } else if (argc > 4) {
        repl_count = argc - 3;
        repl_items = &args[3];
    }

    int sfidx = vm_tied_array_find_method(vm, arr->tied_obj, "SPLICE");
    if (sfidx >= 0) {
        int total_extra = 2 + repl_count;
        VMValue ebuf[16];
        VMValue *extra = (total_extra > 16) ? malloc((size_t)total_extra * sizeof(VMValue)) : ebuf;
        extra[0] = VM_MAKE_INT(offset);
        extra[1] = VM_MAKE_INT(length);
        for (int i = 0; i < repl_count; i++) extra[2 + i] = repl_items[i];
        VMValue res = vm_tied_array_call(vm, arr->tied_obj, sfidx, extra, total_extra);
        if (extra != ebuf) free(extra);
        if (snap && snap != snap_buf) free(snap);
        vm->gc_pause--;
        if (vm->has_pending_exc) return vm_array_new(0);
        if (VM_IS_PTR(res) && VM_PTR_TYPE(res) == VM_OBJ_ARRAY) return VM_AS_ARRAY(res);
        VMArray *wrapped = vm_array_new(1);
        if (!VM_IS_UNDEF(res)) vm_array_push(wrapped, res);
        return wrapped;
    }

    int ffidx = vm_tied_array_find_method(vm, arr->tied_obj, "FETCH");
    int stfidx = vm_tied_array_find_method(vm, arr->tied_obj, "STORE");
    int ssfidx = vm_tied_array_find_method(vm, arr->tied_obj, "STORESIZE");

    VMArray *removed = vm_array_new(length > 0 ? (int)length : 4);
    for (int64_t i = 0; i < length; i++) {
        VMValue iv = VM_MAKE_INT(offset + i);
        VMValue v = (ffidx >= 0) ? vm_tied_array_call(vm, arr->tied_obj, ffidx, &iv, 1) : VM_UNDEF_VAL;
        if (vm->has_pending_exc) {
            if (snap && snap != snap_buf) free(snap);
            vm->gc_pause--;
            return removed;
        }
        vm_array_push(removed, v);
    }

    int64_t new_sz = sz - length + repl_count;
    int64_t tail_len = sz - (offset + length);
    if (repl_count > length) {
        if (ssfidx >= 0) {
            VMValue nsz = VM_MAKE_INT(new_sz);
            vm_tied_array_call(vm, arr->tied_obj, ssfidx, &nsz, 1);
            if (vm->has_pending_exc) {
                if (snap && snap != snap_buf) free(snap);
                vm->gc_pause--;
                return removed;
            }
        }
        for (int64_t i = tail_len - 1; i >= 0; i--) {
            VMValue src_idx = VM_MAKE_INT(offset + length + i);
            VMValue v = (ffidx >= 0) ? vm_tied_array_call(vm, arr->tied_obj, ffidx, &src_idx, 1) : VM_UNDEF_VAL;
            if (vm->has_pending_exc) {
                if (snap && snap != snap_buf) free(snap);
                vm->gc_pause--;
                return removed;
            }
            if (stfidx >= 0) {
                VMValue sargs[2] = { VM_MAKE_INT(offset + repl_count + i), v };
                vm_tied_array_call(vm, arr->tied_obj, stfidx, sargs, 2);
                if (vm->has_pending_exc) {
                    if (snap && snap != snap_buf) free(snap);
                    vm->gc_pause--;
                    return removed;
                }
            }
        }
    } else if (repl_count < length) {
        for (int64_t i = 0; i < tail_len; i++) {
            VMValue src_idx = VM_MAKE_INT(offset + length + i);
            VMValue v = (ffidx >= 0) ? vm_tied_array_call(vm, arr->tied_obj, ffidx, &src_idx, 1) : VM_UNDEF_VAL;
            if (vm->has_pending_exc) {
                if (snap && snap != snap_buf) free(snap);
                vm->gc_pause--;
                return removed;
            }
            if (stfidx >= 0) {
                VMValue sargs[2] = { VM_MAKE_INT(offset + repl_count + i), v };
                vm_tied_array_call(vm, arr->tied_obj, stfidx, sargs, 2);
                if (vm->has_pending_exc) {
                    if (snap && snap != snap_buf) free(snap);
                    vm->gc_pause--;
                    return removed;
                }
            }
        }
        if (ssfidx >= 0) {
            VMValue nsz = VM_MAKE_INT(new_sz);
            vm_tied_array_call(vm, arr->tied_obj, ssfidx, &nsz, 1);
            if (vm->has_pending_exc) {
                if (snap && snap != snap_buf) free(snap);
                vm->gc_pause--;
                return removed;
            }
        }
    }

    for (int i = 0; i < repl_count; i++) {
        if (stfidx >= 0) {
            VMValue sargs[2] = { VM_MAKE_INT(offset + i), repl_items[i] };
            vm_tied_array_call(vm, arr->tied_obj, stfidx, sargs, 2);
            if (vm->has_pending_exc) {
                if (snap && snap != snap_buf) free(snap);
                vm->gc_pause--;
                return removed;
            }
        }
    }

    if (snap && snap != snap_buf) free(snap);
    vm->gc_pause--;
    return removed;
}

static int vm_eval_bool_slow(VM *vm, VMValue v) {
    if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_CELL) {
        v = VM_AS_CELL(v)->val;
    }
    if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_TIED_SCALAR) {
        v = vm_tied_scalar_fetch(vm, v);
        if (vm->has_pending_exc) return 0;
    }
    if (!VM_IS_PTR(v)) return vm_to_bool(v);
    const char *cname = vm_obj_class_name(v);
    if (cname != NULL) {
        int fidx = vm_resolve_overload_func(vm->program, cname, "bool");
        if (fidx >= 0) {
            vm->gc_pause++;
            VMValue args[1] = { v };
            VMValue res = vm_invoke_func(vm, fidx, args, 1);
            vm->gc_pause--;
            if (vm->has_pending_exc) return 0;
            return vm_to_bool(res);
        }
        return 1;
    }
    enum VMObjType t = VM_PTR_TYPE(v);
    if (t == VM_OBJ_HASH) {
        VMHash *h = VM_AS_HASH(v);
        if (!VM_IS_UNDEF(h->tied_obj)) {
            int sfidx = vm_tied_array_find_method(vm, h->tied_obj, "SCALAR");
            if (sfidx >= 0) {
                VMValue sc = vm_tied_array_call(vm, h->tied_obj, sfidx, NULL, 0);
                if (vm->has_pending_exc) return 0;
                return vm_eval_bool_slow(vm, sc);
            }
            int fidx = vm_tied_array_find_method(vm, h->tied_obj, "FIRSTKEY");
            if (fidx >= 0) {
                VMValue fk = vm_tied_array_call(vm, h->tied_obj, fidx, NULL, 0);
                if (vm->has_pending_exc) return 0;
                return !VM_IS_UNDEF(fk);
            }
        }
        return h->size > 0;
    }
    if (t == VM_OBJ_ARRAY) {
        VMArray *a = VM_AS_ARRAY(v);
        if (!VM_IS_UNDEF(a->tied_obj)) {
            return vm_tied_array_fetchsize(vm, a->tied_obj) > 0;
        }
        return a->size > 0;
    }
    return vm_to_bool(v);
}

/* Check whether a pattern contains any regex metacharacters. */
static int vm_pattern_is_literal(const char *pat, size_t plen) {
    for (size_t i = 0; i < plen; i++) {
        char pc = pat[i];
        if (pc == '.' || pc == '*' || pc == '+' || pc == '?' ||
            pc == '[' || pc == ']' || pc == '(' || pc == ')' ||
            pc == '{' || pc == '}' || pc == '\\' || pc == '|' ||
            pc == '^' || pc == '$') {
            return 0;
        }
    }
    return 1;
}

/* Check whether a replacement string has capture/escape metacharacters ($ or \). */
static int vm_repl_is_literal(const char *repl, size_t rlen) {
    for (size_t i = 0; i < rlen; i++) {
        if (repl[i] == '$' || repl[i] == '\\') return 0;
    }
    return 1;
}

/* Binary-safe literal substring replacement for replace/replace_all/str_replace. */
static VMValue vm_replace_literal(const char *s, size_t slen,
                                  const char *pat, size_t plen,
                                  const char *repl, size_t rlen,
                                  int u8_s, int u8_r,
                                  int global, int empty_start) {
    if (plen == 0) {
        if (!empty_start || global) return vm_make_str_flag(s, slen, u8_s);
        size_t nl = slen + rlen;
        VMString *vs = malloc(sizeof(VMString) + nl + 1);
        vm_gc_init_hdr(&vs->hdr, VM_OBJ_STR);
        vs->hdr.is_utf8 = (uint8_t)(u8_s || u8_r);
        vs->len = (uint32_t)nl;
        if (rlen > 0) memcpy(vs->data, repl, rlen);
        if (slen > 0) memcpy(vs->data + rlen, s, slen);
        vs->data[nl] = '\0';
        return VM_MAKE_PTR(vs);
    }
    if (!global) {
        const char *f = vm_memmem(s, slen, pat, plen);
        if (!f) return vm_make_str_flag(s, slen, u8_s);
        size_t before = (size_t)(f - s);
        size_t nl = slen - plen + rlen;
        VMString *vs = malloc(sizeof(VMString) + nl + 1);
        vm_gc_init_hdr(&vs->hdr, VM_OBJ_STR);
        vs->hdr.is_utf8 = (uint8_t)(u8_s || u8_r);
        vs->len = (uint32_t)nl;
        if (before > 0) memcpy(vs->data, s, before);
        if (rlen > 0) memcpy(vs->data + before, repl, rlen);
        if (slen - before - plen > 0)
            memcpy(vs->data + before + rlen, f + plen, slen - before - plen);
        vs->data[nl] = '\0';
        return VM_MAKE_PTR(vs);
    }
    const char *cur = s;
    const char *end = s + slen;
    size_t matches = 0;
    const char *f;
    while (cur <= end && (f = vm_memmem(cur, (size_t)(end - cur), pat, plen)) != NULL) {
        matches++;
        cur = f + plen;
    }
    if (matches == 0) return vm_make_str_flag(s, slen, u8_s);
    size_t nl = slen - matches * plen + matches * rlen;
    VMString *vs = malloc(sizeof(VMString) + nl + 1);
    vm_gc_init_hdr(&vs->hdr, VM_OBJ_STR);
    vs->hdr.is_utf8 = (uint8_t)(u8_s || u8_r);
    vs->len = (uint32_t)nl;
    char *dst = vs->data;
    cur = s;
    while (cur <= end && (f = vm_memmem(cur, (size_t)(end - cur), pat, plen)) != NULL) {
        size_t chunk = (size_t)(f - cur);
        if (chunk > 0) { memcpy(dst, cur, chunk); dst += chunk; }
        if (rlen > 0) { memcpy(dst, repl, rlen); dst += rlen; }
        cur = f + plen;
    }
    size_t tail = (size_t)(end - cur);
    if (tail > 0) { memcpy(dst, cur, tail); dst += tail; }
    *dst = '\0';
    return VM_MAKE_PTR(vs);
}

/* Evaluate abs() with tied-scalar FETCH and unary "abs"/"0+" overload dispatch. */
static int vm_eval_abs_slow(VM *vm, VMValue v, VMValue *out) {
    if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_CELL) {
        v = VM_AS_CELL(v)->val;
    }
    for (int depth = 0; depth < 16 && VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_TIED_SCALAR; depth++) {
        v = vm_tied_scalar_fetch(vm, v);
        if (vm->has_pending_exc) return 1;
        if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_CELL) v = VM_AS_CELL(v)->val;
    }
    if (vm_obj_class_name(v) != NULL) {
        VMValue ov = VM_UNDEF_VAL;
        vm->gc_pause++;
        int has_abs = vm_try_un_overload(vm, "abs", v, &ov);
        vm->gc_pause--;
        if (has_abs) {
            if (vm->has_pending_exc) return 1;
            *out = ov;
            return 0;
        }
        vm->gc_pause++;
        int has_num = vm_try_un_overload(vm, "0+", v, &ov);
        vm->gc_pause--;
        if (has_num) {
            if (vm->has_pending_exc) return 1;
            *out = vm_abs_val(ov);
            return 0;
        }
    }
    *out = vm_abs_val(v);
    return 0;
}

__attribute__((noinline))
static int vm_c_call_utf8_wb(VM *vm, int mode, VMValue a0, int fail_ok, VMValue *out_pair, VMValue *out_exc) {
    int cm = strada_cleanup_mark();
    int lm = strada_local_depth_get();
    StradaValue * volatile slot = vm_to_sv(a0);
    vm->gc_pause++;
    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        StradaValue *s_arg = slot;
        StradaValue *r = NULL;
        if (mode == 0) r = strada_utf8_decode_slot(&s_arg);
        else if (mode == 1) r = strada_utf8_downgrade_slot(&s_arg, fail_ok);
        else r = strada_utf8_upgrade_slot(&s_arg);
        slot = s_arg;
        STRADA_TRY_POP();
        vm->gc_pause--;
        VMArray *pair = vm_array_new(2);
        vm_array_push(pair, slot ? sv_to_vm(slot) : VM_UNDEF_VAL);
        vm_array_push(pair, r ? sv_to_vm(r) : VM_UNDEF_VAL);
        if (r) strada_decref(r);
        if (slot) strada_decref(slot);
        strada_cleanup_drain_to(cm);
        *out_pair = VM_MAKE_PTR(pair);
        return 0;
    } else {
        STRADA_TRY_POP();
        strada_local_restore_to(lm);
        strada_cleanup_drain_to(cm);
        if (slot) strada_decref(slot);
        vm->gc_pause--;
        *out_exc = vm_capture_c_exception();
        return 1;
    }
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
    return vm_execute_call_cl(vm, func_idx, NULL, cargs, cargc, cargc);
}

static VMValue vm_execute_call_cl(VM *vm, int func_idx, VMClosure *closure, VMValue *cargs, int cargc, int passed_argc) {
    VM *prev_vm = g_active_vm;
    g_active_vm = vm;
    size_t saved_exec_base = vm->exec_base;
    vm->exec_base = vm->frame_count;
    #define VM_EXEC_RETURN(val) do { g_active_vm = prev_vm; return (val); } while(0)

    VMChunk *chunk = &vm->program->funcs[func_idx];
    if (vm->frame_count >= vm->frame_cap) {
        vm_frames_grow(vm);
    }
    VMFrame *frame = &vm->frames[vm->frame_count++];
    frame->chunk = chunk;
    frame->ip = chunk->code;
    {
        int cnt = chunk->local_count + 1;
        int pmax = cnt;
        if (!chunk->has_variadic && chunk->fixed_param_count >= 0 && chunk->fixed_param_count < pmax) {
            pmax = chunk->fixed_param_count;
        }
        int zf = cargc < pmax ? cargc : pmax;
        frame->locals = vm_locals_alloc(vm, cnt, zf);
        for (int ci = 0; ci < zf; ci++) frame->locals[ci] = cargs[ci];
    }
    frame->stack_base = vm->stack_top;
    frame->closure = closure;
    frame->global_save_depth = vm->global_save_count;
    frame->passed_argc = passed_argc;
    frame->call_context = vm->pending_call_context;
    vm->pending_call_context = 0;

    register uint8_t *ip = frame->ip;
    VMValue *locals = frame->locals;
    VMValue *stack = vm->stack;
    size_t sp = vm->stack_top;
    uint32_t sig_poll = 0;

    #define SP_PUSH(v) do { \
        if (__builtin_expect(sp >= vm->stack_cap, 0)) { vm_stack_grow(vm, sp); stack = vm->stack; } \
        stack[sp++] = (v); \
    } while (0)
    #define SP_POP()   (stack[--sp])

    #define RELOAD() do { \
        frame = &vm->frames[vm->frame_count - 1]; \
        chunk = frame->chunk; ip = frame->ip; locals = frame->locals; sp = vm->stack_top; \
    } while(0)

    #define VM_CHECK_EXC() do { \
        if (__builtin_expect(vm->has_pending_exc, 0)) { \
            throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0; \
            goto L_vm_throw_exc; \
        } \
    } while (0)

    #define VM_VALUE_IS_TRUTHY(val_expr, res_bool) do { \
        VMValue _tv = (val_expr); \
        if (VM_IS_INT(_tv)) { (res_bool) = (_tv != VM_TAG_INT_BASE); } \
        else if (VM_IS_FLOAT(_tv)) { double _d = VM_FLOAT_VAL(_tv); (res_bool) = (_d != 0.0 && !isnan(_d)); } \
        else if (VM_IS_UNDEF(_tv)) { (res_bool) = 0; } \
        else if (__builtin_expect( \
            VM_PTR_TYPE(_tv) == VM_OBJ_TIED_SCALAR || \
            VM_PTR_TYPE(_tv) == VM_OBJ_CELL || \
            vm_obj_class_name(_tv) != NULL || \
            (VM_PTR_TYPE(_tv) == VM_OBJ_HASH && !VM_IS_UNDEF(VM_AS_HASH(_tv)->tied_obj)) || \
            (VM_PTR_TYPE(_tv) == VM_OBJ_ARRAY && !VM_IS_UNDEF(VM_AS_ARRAY(_tv)->tied_obj)), 0)) { \
            frame->ip = ip; vm->stack_top = sp; \
            (res_bool) = vm_eval_bool_slow(vm, _tv); \
            stack = vm->stack; RELOAD(); \
            VM_CHECK_EXC(); \
        } else { \
            (res_bool) = vm_to_bool(_tv); \
        } \
    } while (0)

    /* Exception handling via longjmp */
    jmp_buf exc_jmp;
    jmp_buf *saved_jmp = NULL;
    uint16_t call_fidx = 0;
    int call_argc = 0;
    uint16_t mcall_idx = 0;
    int mcall_argc = 0;
    const char *mcall_method_cstr = NULL;
    int mcall_is_dyn = 0;
    int mcall_allow_field = 0;
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
        DT(OP_INCR); DT(OP_DECR); DT(OP_POST_INCR); DT(OP_POST_DECR);
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
        DT(OP_ARG_PRESENT);
        DT(OP_SQ_COERCE);
        DT(OP_WEAKEN_SLOT);
        DT(OP_ISWEAK_SLOT);
        DT(OP_CLEAR_LOCALS);
        DT(OP_FIELD_ACCESS);
        DT(OP_MAP_PUSH);
        DT(OP_SET_CALL_CTX);
        DT(OP_INIT_GLOBAL);
        DT(OP_PUSH_I8);
        DT(OP_PUSH_I32);
        DT(OP_CAST_INT);
        DT(OP_CAST_NUM);
        DT(OP_SUB_LOCAL_I32);
        DT(OP_ADD_LOCAL_I32);
        DT(OP_ADD_LOCALS);
        DT(OP_ADD_TO_LOCAL);
        DT(OP_LT_LOCAL_I32_JMP_F);
        DT(OP_LE_LOCAL_I32_JMP_F);
        DT(OP_LT_LOCAL_LOCAL_JMP_F);
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

    CASE(OP_PUSH_I8): {
        int8_t val = read_i8(ip); ip += 1;
        SP_PUSH(VM_MAKE_INT_FAST((int64_t)val));
        DISPATCH();
    }

    CASE(OP_PUSH_I32): {
        int32_t val = read_i32(ip); ip += 4;
        SP_PUSH(VM_MAKE_INT_FAST((int64_t)val));
        DISPATCH();
    }

    CASE(OP_CAST_INT): {
        VMValue v = stack[sp - 1];
        if (__builtin_expect(!VM_IS_INT(v), 0)) {
            if (!VM_IS_PTR(v) || VM_PTR_TYPE(v) != VM_OBJ_INT64) {
                stack[sp - 1] = VM_MAKE_INT(vm_to_int(v));
            }
        }
        DISPATCH();
    }

    CASE(OP_CAST_NUM): {
        VMValue v = stack[sp - 1];
        if (__builtin_expect(!VM_IS_FLOAT(v), 0)) {
            stack[sp - 1] = VM_MAKE_FLOAT(vm_to_num(v));
        }
        DISPATCH();
    }

    CASE(OP_SUB_LOCAL_I32): {
        uint16_t slot = read_u16(ip);
        int32_t imm = read_i32(ip + 2);
        ip += 6;
        VMValue a = locals[slot];
        if (__builtin_expect(VM_IS_INT(a), 1)) {
            SP_PUSH(VM_MAKE_INT(VM_INT_VAL(a) - (int64_t)imm));
            DISPATCH();
        }
        if (VM_IS_FLOAT(a)) {
            SP_PUSH(VM_MAKE_FLOAT(VM_FLOAT_VAL(a) - (double)imm));
            DISPATCH();
        }
        if (VM_IS_UNDEF(a)) {
            SP_PUSH(VM_MAKE_INT(-(int64_t)imm));
            DISPATCH();
        }
        {
            enum VMObjType t = VM_PTR_TYPE(a);
            if (t == VM_OBJ_CELL) {
                a = VM_AS_CELL(a)->val;
                if (VM_IS_PTR(a) && VM_PTR_TYPE(a) == VM_OBJ_TIED_SCALAR) {
                    frame->ip = ip; vm->stack_top = sp;
                    a = vm_tied_scalar_fetch(vm, a);
                    stack = vm->stack; RELOAD(); VM_CHECK_EXC();
                } else if (VM_IS_PTR(a) && VM_PTR_TYPE(a) == VM_OBJ_WEAKREF) {
                    frame->ip = ip; vm->stack_top = sp;
                    a = vm_unwrap_weak_gc(vm, a, NULL, 0);
                    stack = vm->stack; RELOAD();
                }
            } else if (t == VM_OBJ_TIED_SCALAR) {
                frame->ip = ip; vm->stack_top = sp;
                a = vm_tied_scalar_fetch(vm, a);
                stack = vm->stack; RELOAD(); VM_CHECK_EXC();
            } else if (t == VM_OBJ_WEAKREF) {
                frame->ip = ip; vm->stack_top = sp;
                a = vm_unwrap_weak_gc(vm, a, NULL, 0);
                stack = vm->stack; RELOAD();
            }
            VMValue b = VM_MAKE_INT((int64_t)imm);
            if (__builtin_expect(vm_obj_class_name(a) != NULL, 0)) {
                VMValue _ov_res = VM_UNDEF_VAL;
                frame->ip = ip; vm->stack_top = sp;
                if (vm_try_bin_overload(vm, "-", a, b, &_ov_res)) {
                    stack = vm->stack; RELOAD(); VM_CHECK_EXC();
                    SP_PUSH(_ov_res); DISPATCH();
                }
            }
            if (!vm_is_float_operand(a)) SP_PUSH(VM_MAKE_INT((int64_t)((uint64_t)vm_to_int(a) - (uint64_t)(int64_t)imm)));
            else SP_PUSH(VM_MAKE_FLOAT(vm_to_num(a) - (double)imm));
        }
        DISPATCH();
    }

    CASE(OP_ADD_LOCAL_I32): {
        uint16_t slot = read_u16(ip);
        int32_t imm = read_i32(ip + 2);
        ip += 6;
        VMValue a = locals[slot];
        if (__builtin_expect(VM_IS_INT(a), 1)) {
            SP_PUSH(VM_MAKE_INT(VM_INT_VAL(a) + (int64_t)imm));
            DISPATCH();
        }
        if (VM_IS_FLOAT(a)) {
            SP_PUSH(VM_MAKE_FLOAT(VM_FLOAT_VAL(a) + (double)imm));
            DISPATCH();
        }
        if (VM_IS_UNDEF(a)) {
            SP_PUSH(VM_MAKE_INT((int64_t)imm));
            DISPATCH();
        }
        {
            enum VMObjType t = VM_PTR_TYPE(a);
            if (t == VM_OBJ_CELL) {
                a = VM_AS_CELL(a)->val;
                if (VM_IS_PTR(a) && VM_PTR_TYPE(a) == VM_OBJ_TIED_SCALAR) {
                    frame->ip = ip; vm->stack_top = sp;
                    a = vm_tied_scalar_fetch(vm, a);
                    stack = vm->stack; RELOAD(); VM_CHECK_EXC();
                } else if (VM_IS_PTR(a) && VM_PTR_TYPE(a) == VM_OBJ_WEAKREF) {
                    frame->ip = ip; vm->stack_top = sp;
                    a = vm_unwrap_weak_gc(vm, a, NULL, 0);
                    stack = vm->stack; RELOAD();
                }
            } else if (t == VM_OBJ_TIED_SCALAR) {
                frame->ip = ip; vm->stack_top = sp;
                a = vm_tied_scalar_fetch(vm, a);
                stack = vm->stack; RELOAD(); VM_CHECK_EXC();
            } else if (t == VM_OBJ_WEAKREF) {
                frame->ip = ip; vm->stack_top = sp;
                a = vm_unwrap_weak_gc(vm, a, NULL, 0);
                stack = vm->stack; RELOAD();
            }
            VMValue b = VM_MAKE_INT((int64_t)imm);
            if (__builtin_expect(vm_obj_class_name(a) != NULL, 0)) {
                VMValue _ov_res = VM_UNDEF_VAL;
                frame->ip = ip; vm->stack_top = sp;
                if (vm_try_bin_overload(vm, "+", a, b, &_ov_res)) {
                    stack = vm->stack; RELOAD(); VM_CHECK_EXC();
                    SP_PUSH(_ov_res); DISPATCH();
                }
            }
            if (!vm_is_float_operand(a)) SP_PUSH(VM_MAKE_INT((int64_t)((uint64_t)vm_to_int(a) + (uint64_t)(int64_t)imm)));
            else SP_PUSH(VM_MAKE_FLOAT(vm_to_num(a) + (double)imm));
        }
        DISPATCH();
    }

    CASE(OP_ADD_LOCALS): {
        uint16_t sa = read_u16(ip);
        uint16_t sb = read_u16(ip + 2);
        ip += 4;
        VMValue a = locals[sa], b = locals[sb];
        if (__builtin_expect(VM_BOTH_INT(a, b), 1)) {
            SP_PUSH(VM_MAKE_INT(VM_INT_VAL(a) + VM_INT_VAL(b)));
            DISPATCH();
        }
        if (__builtin_expect(VM_IS_FLOAT(a) && VM_IS_FLOAT(b), 1)) {
            SP_PUSH(VM_MAKE_FLOAT(VM_FLOAT_VAL(a) + VM_FLOAT_VAL(b)));
            DISPATCH();
        }
        if (!VM_IS_PTR(a) && !VM_IS_PTR(b)) {
            if (VM_IS_FLOAT(a) || VM_IS_FLOAT(b)) {
                double da = VM_IS_FLOAT(a) ? VM_FLOAT_VAL(a) : (VM_IS_INT(a) ? (double)VM_INT_VAL(a) : 0.0);
                double db = VM_IS_FLOAT(b) ? VM_FLOAT_VAL(b) : (VM_IS_INT(b) ? (double)VM_INT_VAL(b) : 0.0);
                SP_PUSH(VM_MAKE_FLOAT(da + db));
            } else {
                int64_t ia = VM_IS_INT(a) ? VM_INT_VAL(a) : 0;
                int64_t ib = VM_IS_INT(b) ? VM_INT_VAL(b) : 0;
                SP_PUSH(VM_MAKE_INT(ia + ib));
            }
            DISPATCH();
        }
        {
            VMValue va = a, vb = b;
            if (VM_IS_PTR(va) && VM_PTR_TYPE(va) == VM_OBJ_CELL) va = VM_AS_CELL(va)->val;
            if (VM_IS_PTR(va) && VM_PTR_TYPE(va) == VM_OBJ_TIED_SCALAR) {
                frame->ip = ip; vm->stack_top = sp; SP_PUSH(vb); vm->stack_top = sp;
                va = vm_tied_scalar_fetch(vm, va); stack = vm->stack; RELOAD(); VM_CHECK_EXC();
                vb = SP_POP();
            } else if (VM_IS_PTR(va) && VM_PTR_TYPE(va) == VM_OBJ_WEAKREF) {
                frame->ip = ip; vm->stack_top = sp; SP_PUSH(vb); vm->stack_top = sp;
                va = vm_unwrap_weak_gc(vm, va, NULL, 0); stack = vm->stack; RELOAD();
                vb = SP_POP();
            }
            if (VM_IS_PTR(vb) && VM_PTR_TYPE(vb) == VM_OBJ_CELL) vb = VM_AS_CELL(vb)->val;
            if (VM_IS_PTR(vb) && VM_PTR_TYPE(vb) == VM_OBJ_TIED_SCALAR) {
                frame->ip = ip; vm->stack_top = sp; SP_PUSH(va); vm->stack_top = sp;
                vb = vm_tied_scalar_fetch(vm, vb); stack = vm->stack; RELOAD(); VM_CHECK_EXC();
                va = SP_POP();
            } else if (VM_IS_PTR(vb) && VM_PTR_TYPE(vb) == VM_OBJ_WEAKREF) {
                frame->ip = ip; vm->stack_top = sp; SP_PUSH(va); vm->stack_top = sp;
                vb = vm_unwrap_weak_gc(vm, vb, NULL, 0); stack = vm->stack; RELOAD();
                va = SP_POP();
            }
            if (__builtin_expect(vm_obj_class_name(va) != NULL || vm_obj_class_name(vb) != NULL, 0)) {
                VMValue _ov_res = VM_UNDEF_VAL;
                frame->ip = ip; vm->stack_top = sp;
                if (vm_try_bin_overload(vm, "+", va, vb, &_ov_res)) {
                    stack = vm->stack; RELOAD(); VM_CHECK_EXC();
                    SP_PUSH(_ov_res); DISPATCH();
                }
            }
            if (!vm_is_float_operand(va) && !vm_is_float_operand(vb)) {
                SP_PUSH(VM_MAKE_INT((int64_t)((uint64_t)vm_to_int(va) + (uint64_t)vm_to_int(vb))));
            } else {
                SP_PUSH(VM_MAKE_FLOAT(vm_to_num(va) + vm_to_num(vb)));
            }
        }
        DISPATCH();
    }

    CASE(OP_ADD_TO_LOCAL): {
        uint16_t d = read_u16(ip);
        uint16_t s = read_u16(ip + 2);
        uint8_t coerce = ip[4];
        ip += 5;
        VMValue a = locals[d], b = locals[s];
        if (__builtin_expect(VM_BOTH_INT(a, b) && coerce != 2, 1)) {
            locals[d] = VM_MAKE_INT(VM_INT_VAL(a) + VM_INT_VAL(b));
            DISPATCH();
        }
        if (__builtin_expect(VM_IS_FLOAT(a) && VM_IS_FLOAT(b) && coerce != 1, 1)) {
            locals[d] = VM_MAKE_FLOAT(VM_FLOAT_VAL(a) + VM_FLOAT_VAL(b));
            DISPATCH();
        }
        if (!VM_IS_PTR(a) && !VM_IS_PTR(b)) {
            VMValue res;
            if (VM_IS_FLOAT(a) || VM_IS_FLOAT(b) || coerce == 2) {
                double da = VM_IS_FLOAT(a) ? VM_FLOAT_VAL(a) : (VM_IS_INT(a) ? (double)VM_INT_VAL(a) : 0.0);
                double db = VM_IS_FLOAT(b) ? VM_FLOAT_VAL(b) : (VM_IS_INT(b) ? (double)VM_INT_VAL(b) : 0.0);
                res = (coerce == 1) ? VM_MAKE_INT((int64_t)(da + db)) : VM_MAKE_FLOAT(da + db);
            } else {
                int64_t ia = VM_IS_INT(a) ? VM_INT_VAL(a) : 0;
                int64_t ib = VM_IS_INT(b) ? VM_INT_VAL(b) : 0;
                res = VM_MAKE_INT(ia + ib);
            }
            locals[d] = res;
            DISPATCH();
        }
        {
            VMValue rhs = b;
            if (VM_IS_PTR(rhs) && VM_PTR_TYPE(rhs) == VM_OBJ_CELL) rhs = VM_AS_CELL(rhs)->val;
            if (VM_IS_PTR(rhs) && VM_PTR_TYPE(rhs) == VM_OBJ_TIED_SCALAR) {
                frame->ip = ip; vm->stack_top = sp;
                rhs = vm_tied_scalar_fetch(vm, rhs);
                stack = vm->stack; RELOAD(); VM_CHECK_EXC();
            } else if (VM_IS_PTR(rhs) && VM_PTR_TYPE(rhs) == VM_OBJ_WEAKREF) {
                frame->ip = ip; vm->stack_top = sp;
                rhs = vm_unwrap_weak_gc(vm, rhs, NULL, 0);
                stack = vm->stack; RELOAD();
            }
            VMValue *dstp = &locals[d];
            if (VM_IS_PTR(*dstp) && VM_PTR_TYPE(*dstp) == VM_OBJ_CELL) dstp = &VM_AS_CELL(*dstp)->val;
            if (__builtin_expect(VM_IS_PTR(*dstp) && VM_PTR_TYPE(*dstp) == VM_OBJ_TIED_SCALAR, 0)) {
                VMValue ts = *dstp;
                frame->ip = ip; vm->stack_top = sp; SP_PUSH(rhs); vm->stack_top = sp; vm->gc_pause++;
                VMValue lhs = vm_tied_scalar_fetch(vm, ts);
                VMValue cur_rhs = SP_POP();
                VMValue out = VM_UNDEF_VAL;
                if (!vm->has_pending_exc) {
                    if (vm_obj_class_name(lhs) || vm_obj_class_name(cur_rhs)) {
                        if (!vm_try_bin_overload(vm, "+", lhs, cur_rhs, &out)) {
                            out = (!vm_is_float_operand(lhs) && !vm_is_float_operand(cur_rhs))
                                  ? VM_MAKE_INT((int64_t)((uint64_t)vm_to_int(lhs) + (uint64_t)vm_to_int(cur_rhs)))
                                  : VM_MAKE_FLOAT(vm_to_num(lhs) + vm_to_num(cur_rhs));
                        }
                    } else {
                        out = (!vm_is_float_operand(lhs) && !vm_is_float_operand(cur_rhs))
                              ? VM_MAKE_INT((int64_t)((uint64_t)vm_to_int(lhs) + (uint64_t)vm_to_int(cur_rhs)))
                              : VM_MAKE_FLOAT(vm_to_num(lhs) + vm_to_num(cur_rhs));
                    }
                    if (!vm->has_pending_exc) {
                        if (coerce == 1 && !VM_IS_INT(out)) out = VM_MAKE_INT(vm_to_int(out));
                        else if (coerce == 2 && !VM_IS_FLOAT(out)) out = VM_MAKE_FLOAT(vm_to_num(out));
                        vm_tied_scalar_store(vm, ts, out);
                    }
                }
                vm->gc_pause--;
                stack = vm->stack; RELOAD(); VM_CHECK_EXC();
                DISPATCH();
            }
            VMValue lhs = *dstp;
            if (VM_IS_PTR(lhs) && VM_PTR_TYPE(lhs) == VM_OBJ_WEAKREF) {
                frame->ip = ip; vm->stack_top = sp; SP_PUSH(rhs); vm->stack_top = sp;
                lhs = vm_unwrap_weak_gc(vm, lhs, NULL, 0);
                stack = vm->stack; RELOAD();
                rhs = SP_POP();
                dstp = &locals[d];
                if (VM_IS_PTR(*dstp) && VM_PTR_TYPE(*dstp) == VM_OBJ_CELL) dstp = &VM_AS_CELL(*dstp)->val;
            }
            VMValue out = VM_UNDEF_VAL;
            if (__builtin_expect(vm_obj_class_name(lhs) != NULL || vm_obj_class_name(rhs) != NULL, 0)) {
                frame->ip = ip; vm->stack_top = sp;
                int ov = vm_try_bin_overload(vm, "+", lhs, rhs, &out);
                stack = vm->stack; RELOAD(); VM_CHECK_EXC();
                if (ov) {
                    dstp = &locals[d];
                    if (VM_IS_PTR(*dstp) && VM_PTR_TYPE(*dstp) == VM_OBJ_CELL) dstp = &VM_AS_CELL(*dstp)->val;
                    if (coerce == 1 && !VM_IS_INT(out)) out = VM_MAKE_INT(vm_to_int(out));
                    else if (coerce == 2 && !VM_IS_FLOAT(out)) out = VM_MAKE_FLOAT(vm_to_num(out));
                    *dstp = out;
                    DISPATCH();
                }
            }
            if (!vm_is_float_operand(lhs) && !vm_is_float_operand(rhs)) {
                out = VM_MAKE_INT((int64_t)((uint64_t)vm_to_int(lhs) + (uint64_t)vm_to_int(rhs)));
            } else {
                out = VM_MAKE_FLOAT(vm_to_num(lhs) + vm_to_num(rhs));
            }
            if (coerce == 1 && !VM_IS_INT(out)) out = VM_MAKE_INT(vm_to_int(out));
            else if (coerce == 2 && !VM_IS_FLOAT(out)) out = VM_MAKE_FLOAT(vm_to_num(out));
            *dstp = out;
        }
        DISPATCH();
    }

    CASE(OP_LT_LOCAL_I32_JMP_F): {
        uint16_t slot = read_u16(ip);
        int32_t imm = read_i32(ip + 2);
        uint16_t off = read_u16(ip + 6);
        ip += 8;
        VMValue v = locals[slot];
        int cond;
        if (__builtin_expect(VM_IS_INT(v), 1)) {
            cond = (VM_INT_VAL(v) < (int64_t)imm);
        } else if (VM_IS_FLOAT(v)) {
            cond = (VM_FLOAT_VAL(v) < (double)imm);
        } else if (VM_IS_UNDEF(v)) {
            cond = (0 < imm);
        } else {
            enum VMObjType t = VM_PTR_TYPE(v);
            if (t == VM_OBJ_CELL) v = VM_AS_CELL(v)->val;
            if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_TIED_SCALAR) {
                frame->ip = ip; vm->stack_top = sp;
                v = vm_tied_scalar_fetch(vm, v);
                stack = vm->stack; RELOAD(); VM_CHECK_EXC();
            } else if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_WEAKREF) {
                frame->ip = ip; vm->stack_top = sp;
                v = vm_unwrap_weak_gc(vm, v, NULL, 0);
                stack = vm->stack; RELOAD();
            }
            VMValue ovr = VM_UNDEF_VAL;
            if (__builtin_expect(vm_obj_class_name(v) != NULL, 0)) {
                frame->ip = ip; vm->stack_top = sp;
                if (vm_try_bin_overload(vm, "<", v, VM_MAKE_INT((int64_t)imm), &ovr)) {
                    stack = vm->stack; RELOAD(); VM_CHECK_EXC();
                    int _tr = 0; VM_VALUE_IS_TRUTHY(ovr, _tr); cond = _tr; goto L_lt_i32_chk;
                }
            }
            cond = VM_IS_ANY_INT(v) ? (VM_ANY_INT_VAL(v) < (int64_t)imm) : (vm_to_num(v) < (double)imm);
        }
    L_lt_i32_chk:
        if (!cond) {
            if (__builtin_expect(off >= chunk->code_len, 0)) {
                fprintf(stderr, "VM error: jump offset %u out of bounds (%zu)\n", (unsigned)off, chunk->code_len);
                vm->stack_top = sp; vm->exec_base = saved_exec_base; VM_EXEC_RETURN(VM_UNDEF_VAL);
            }
            uint8_t *target = chunk->code + off;
            if (target <= ip && __builtin_expect((++sig_poll & 31) == 0 && strada_has_pending_signals(), 0)) {
                frame->ip = target; vm->stack_top = sp;
                int __th = vm_c_dispatch_signals(vm, &throw_exc);
                stack = vm->stack; RELOAD();
                if (__th) goto L_vm_throw_exc;
            }
            ip = target;
        }
        DISPATCH();
    }

    CASE(OP_LE_LOCAL_I32_JMP_F): {
        uint16_t slot = read_u16(ip);
        int32_t imm = read_i32(ip + 2);
        uint16_t off = read_u16(ip + 6);
        ip += 8;
        VMValue v = locals[slot];
        int cond;
        if (__builtin_expect(VM_IS_INT(v), 1)) {
            cond = (VM_INT_VAL(v) <= (int64_t)imm);
        } else if (VM_IS_FLOAT(v)) {
            cond = (VM_FLOAT_VAL(v) <= (double)imm);
        } else if (VM_IS_UNDEF(v)) {
            cond = (0 <= imm);
        } else {
            enum VMObjType t = VM_PTR_TYPE(v);
            if (t == VM_OBJ_CELL) v = VM_AS_CELL(v)->val;
            if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_TIED_SCALAR) {
                frame->ip = ip; vm->stack_top = sp;
                v = vm_tied_scalar_fetch(vm, v);
                stack = vm->stack; RELOAD(); VM_CHECK_EXC();
            } else if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_WEAKREF) {
                frame->ip = ip; vm->stack_top = sp;
                v = vm_unwrap_weak_gc(vm, v, NULL, 0);
                stack = vm->stack; RELOAD();
            }
            VMValue ovr = VM_UNDEF_VAL;
            if (__builtin_expect(vm_obj_class_name(v) != NULL, 0)) {
                frame->ip = ip; vm->stack_top = sp;
                if (vm_try_bin_overload(vm, "<=", v, VM_MAKE_INT((int64_t)imm), &ovr)) {
                    stack = vm->stack; RELOAD(); VM_CHECK_EXC();
                    int _tr = 0; VM_VALUE_IS_TRUTHY(ovr, _tr); cond = _tr; goto L_le_i32_chk;
                }
            }
            cond = VM_IS_ANY_INT(v) ? (VM_ANY_INT_VAL(v) <= (int64_t)imm) : (vm_to_num(v) <= (double)imm);
        }
    L_le_i32_chk:
        if (!cond) {
            if (__builtin_expect(off >= chunk->code_len, 0)) {
                fprintf(stderr, "VM error: jump offset %u out of bounds (%zu)\n", (unsigned)off, chunk->code_len);
                vm->stack_top = sp; vm->exec_base = saved_exec_base; VM_EXEC_RETURN(VM_UNDEF_VAL);
            }
            uint8_t *target = chunk->code + off;
            if (target <= ip && __builtin_expect((++sig_poll & 31) == 0 && strada_has_pending_signals(), 0)) {
                frame->ip = target; vm->stack_top = sp;
                int __th = vm_c_dispatch_signals(vm, &throw_exc);
                stack = vm->stack; RELOAD();
                if (__th) goto L_vm_throw_exc;
            }
            ip = target;
        }
        DISPATCH();
    }

    CASE(OP_LT_LOCAL_LOCAL_JMP_F): {
        uint16_t sa = read_u16(ip);
        uint16_t sb = read_u16(ip + 2);
        uint16_t off = read_u16(ip + 4);
        ip += 6;
        VMValue a = locals[sa], b = locals[sb];
        int cond;
        if (__builtin_expect(VM_BOTH_INT(a, b), 1)) {
            cond = (VM_INT_VAL(a) < VM_INT_VAL(b));
        } else if (VM_IS_FLOAT(a) && VM_IS_FLOAT(b)) {
            cond = (VM_FLOAT_VAL(a) < VM_FLOAT_VAL(b));
        } else if (!VM_IS_PTR(a) && !VM_IS_PTR(b)) {
            double da = VM_IS_FLOAT(a) ? VM_FLOAT_VAL(a) : (VM_IS_INT(a) ? (double)VM_INT_VAL(a) : 0.0);
            double db = VM_IS_FLOAT(b) ? VM_FLOAT_VAL(b) : (VM_IS_INT(b) ? (double)VM_INT_VAL(b) : 0.0);
            cond = (da < db);
        } else {
            SP_PUSH(locals[sa]); /* load a/b cleanly via fallback */
            sp--;
            if (VM_IS_PTR(a) && VM_PTR_TYPE(a) == VM_OBJ_CELL) a = VM_AS_CELL(a)->val;
            if (VM_IS_PTR(a) && VM_PTR_TYPE(a) == VM_OBJ_TIED_SCALAR) {
                frame->ip = ip; vm->stack_top = sp;
                a = vm_tied_scalar_fetch(vm, a); stack = vm->stack; RELOAD(); VM_CHECK_EXC();
                b = locals[sb];
            } else if (VM_IS_PTR(a) && VM_PTR_TYPE(a) == VM_OBJ_WEAKREF) {
                frame->ip = ip; vm->stack_top = sp;
                a = vm_unwrap_weak_gc(vm, a, NULL, 0); stack = vm->stack; RELOAD();
                b = locals[sb];
            }
            if (VM_IS_PTR(b) && VM_PTR_TYPE(b) == VM_OBJ_CELL) b = VM_AS_CELL(b)->val;
            if (VM_IS_PTR(b) && VM_PTR_TYPE(b) == VM_OBJ_TIED_SCALAR) {
                frame->ip = ip; vm->stack_top = sp; SP_PUSH(a); vm->stack_top = sp;
                b = vm_tied_scalar_fetch(vm, b); stack = vm->stack; RELOAD(); VM_CHECK_EXC();
                a = SP_POP();
            } else if (VM_IS_PTR(b) && VM_PTR_TYPE(b) == VM_OBJ_WEAKREF) {
                frame->ip = ip; vm->stack_top = sp; SP_PUSH(a); vm->stack_top = sp;
                b = vm_unwrap_weak_gc(vm, b, NULL, 0); stack = vm->stack; RELOAD();
                a = SP_POP();
            }
            VMValue ovr = VM_UNDEF_VAL;
            if (__builtin_expect(vm_obj_class_name(a) != NULL || vm_obj_class_name(b) != NULL, 0)) {
                frame->ip = ip; vm->stack_top = sp;
                if (vm_try_bin_overload(vm, "<", a, b, &ovr)) {
                    stack = vm->stack; RELOAD(); VM_CHECK_EXC();
                    int _tr = 0; VM_VALUE_IS_TRUTHY(ovr, _tr); cond = _tr; goto L_lt_ll_chk;
                }
            }
            cond = (VM_IS_ANY_INT(a) && VM_IS_ANY_INT(b)) ? (VM_ANY_INT_VAL(a) < VM_ANY_INT_VAL(b)) : (vm_to_num(a) < vm_to_num(b));
        }
    L_lt_ll_chk:
        if (!cond) {
            if (__builtin_expect(off >= chunk->code_len, 0)) {
                fprintf(stderr, "VM error: jump offset %u out of bounds (%zu)\n", (unsigned)off, chunk->code_len);
                vm->stack_top = sp; vm->exec_base = saved_exec_base; VM_EXEC_RETURN(VM_UNDEF_VAL);
            }
            uint8_t *target = chunk->code + off;
            if (target <= ip && __builtin_expect((++sig_poll & 31) == 0 && strada_has_pending_signals(), 0)) {
                frame->ip = target; vm->stack_top = sp;
                int __th = vm_c_dispatch_signals(vm, &throw_exc);
                stack = vm->stack; RELOAD();
                if (__th) goto L_vm_throw_exc;
            }
            ip = target;
        }
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
        if (__builtin_expect(VM_IS_PTR(top) && VM_PTR_TYPE(top) == VM_OBJ_STRBUF, 0)) {
            VM_AS_STRBUF(top)->shared = 1;
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
        if (__builtin_expect(!VM_IS_PTR(v), 1)) {
            SP_PUSH(v);
            DISPATCH();
        }
        enum VMObjType t = VM_PTR_TYPE(v);
        if (__builtin_expect(t == VM_OBJ_STR, 1)) {
            SP_PUSH(v);
            DISPATCH();
        }
        if (t == VM_OBJ_STRBUF) {
            if (*ip != OP_STR_LEN) VM_AS_STRBUF(v)->shared = 1;
            SP_PUSH(v);
            DISPATCH();
        }
        if (t == VM_OBJ_CELL) {
            VMValue cv = VM_AS_CELL(v)->val;
            if (__builtin_expect(VM_IS_PTR(cv) && VM_PTR_TYPE(cv) == VM_OBJ_TIED_SCALAR, 0)) {
                frame->ip = ip; vm->stack_top = sp;
                VMValue fv = vm_tied_scalar_fetch(vm, cv);
                stack = vm->stack; RELOAD();
                VM_CHECK_EXC();
                SP_PUSH(fv);
                DISPATCH();
            }
            if (__builtin_expect(VM_IS_PTR(cv) && VM_PTR_TYPE(cv) == VM_OBJ_WEAKREF, 0)) {
                frame->ip = ip; vm->stack_top = sp;
                cv = vm_unwrap_weak_gc(vm, cv, NULL, 0);
                stack = vm->stack; RELOAD();
            }
            SP_PUSH(vm_val_copy(cv));
            DISPATCH();
        }
        if (t == VM_OBJ_TIED_SCALAR) {
            frame->ip = ip; vm->stack_top = sp;
            VMValue fv = vm_tied_scalar_fetch(vm, v);
            stack = vm->stack; RELOAD();
            VM_CHECK_EXC();
            SP_PUSH(fv);
            DISPATCH();
        }
        if (t == VM_OBJ_WEAKREF) {
            frame->ip = ip; vm->stack_top = sp;
            VMValue u = vm_unwrap_weak_gc(vm, v, NULL, 0);
            stack = vm->stack; RELOAD();
            SP_PUSH(vm_val_copy(u));
            DISPATCH();
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
        if (__builtin_expect(!VM_IS_PTR(old) && (!vm->has_weakrefs || !VM_IS_PTR(newv)), 1)) {
            locals[slot] = newv;
            DISPATCH();
        }
        if (__builtin_expect(vm->has_weakrefs && VM_IS_PTR(newv) && VM_PTR_TYPE(newv) == VM_OBJ_WEAKREF, 0)) {
            frame->ip = ip; vm->stack_top = sp;
            newv = vm_unwrap_weak_gc(vm, newv, &old, 1);
            stack = vm->stack; RELOAD();
            old = locals[slot];
        }
        /* If the slot holds a cell, store through it (preserving any tied scalar wrapper) */
        if (__builtin_expect(VM_IS_PTR(old) && VM_PTR_TYPE(old) == VM_OBJ_CELL, 0)) {
            VMValue old_cell_val = VM_AS_CELL(old)->val;
            if (__builtin_expect(VM_IS_PTR(old_cell_val) && VM_PTR_TYPE(old_cell_val) == VM_OBJ_TIED_SCALAR, 0)) {
                frame->ip = ip; vm->stack_top = sp;
                vm_tied_scalar_store(vm, old_cell_val, newv);
                stack = vm->stack; RELOAD();
                VM_CHECK_EXC();
                DISPATCH();
            }
            vm_temp_free(&old_cell_val);
            VM_AS_CELL(old)->val = newv;
            DISPATCH();
        }
        if (__builtin_expect(VM_IS_PTR(old) && VM_PTR_TYPE(old) == VM_OBJ_TIED_SCALAR, 0)) {
            frame->ip = ip; vm->stack_top = sp;
            vm_tied_scalar_store(vm, old, newv);
            stack = vm->stack; RELOAD();
            VM_CHECK_EXC();
            DISPATCH();
        }
        /* Only free old scalar temporaries; containers (arrays/hashes) are shared */
        vm_temp_free(&old);
        locals[slot] = newv;
        DISPATCH();
    }

    #define VM_TRY_BIN_OV(opstr) do { \
        if (__builtin_expect(vm_obj_class_name(a) != NULL || vm_obj_class_name(b) != NULL, 0)) { \
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

    /* Integer / unboxed float arithmetic — fast paths bypass overload & box checks */
    CASE(OP_ADD): {
        VMValue b = SP_POP(), a = SP_POP();
        if (__builtin_expect(VM_BOTH_INT(a, b), 1)) {
            SP_PUSH(VM_MAKE_INT(VM_INT_VAL(a) + VM_INT_VAL(b)));
            DISPATCH();
        }
        if (__builtin_expect(VM_IS_FLOAT(a) && VM_IS_FLOAT(b), 1)) {
            SP_PUSH(VM_MAKE_FLOAT(VM_FLOAT_VAL(a) + VM_FLOAT_VAL(b)));
            DISPATCH();
        }
        if (!VM_IS_PTR(a) && !VM_IS_PTR(b)) {
            if (VM_IS_FLOAT(a) || VM_IS_FLOAT(b)) {
                double da = VM_IS_FLOAT(a) ? VM_FLOAT_VAL(a) : (VM_IS_INT(a) ? (double)VM_INT_VAL(a) : 0.0);
                double db = VM_IS_FLOAT(b) ? VM_FLOAT_VAL(b) : (VM_IS_INT(b) ? (double)VM_INT_VAL(b) : 0.0);
                SP_PUSH(VM_MAKE_FLOAT(da + db));
            } else {
                int64_t ia = VM_IS_INT(a) ? VM_INT_VAL(a) : 0;
                int64_t ib = VM_IS_INT(b) ? VM_INT_VAL(b) : 0;
                SP_PUSH(VM_MAKE_INT(ia + ib));
            }
            DISPATCH();
        }
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
        if (__builtin_expect(VM_BOTH_INT(a, b), 1)) {
            SP_PUSH(VM_MAKE_INT(VM_INT_VAL(a) - VM_INT_VAL(b)));
            DISPATCH();
        }
        if (__builtin_expect(VM_IS_FLOAT(a) && VM_IS_FLOAT(b), 1)) {
            SP_PUSH(VM_MAKE_FLOAT(VM_FLOAT_VAL(a) - VM_FLOAT_VAL(b)));
            DISPATCH();
        }
        if (!VM_IS_PTR(a) && !VM_IS_PTR(b)) {
            if (VM_IS_FLOAT(a) || VM_IS_FLOAT(b)) {
                double da = VM_IS_FLOAT(a) ? VM_FLOAT_VAL(a) : (VM_IS_INT(a) ? (double)VM_INT_VAL(a) : 0.0);
                double db = VM_IS_FLOAT(b) ? VM_FLOAT_VAL(b) : (VM_IS_INT(b) ? (double)VM_INT_VAL(b) : 0.0);
                SP_PUSH(VM_MAKE_FLOAT(da - db));
            } else {
                int64_t ia = VM_IS_INT(a) ? VM_INT_VAL(a) : 0;
                int64_t ib = VM_IS_INT(b) ? VM_INT_VAL(b) : 0;
                SP_PUSH(VM_MAKE_INT(ia - ib));
            }
            DISPATCH();
        }
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
        if (__builtin_expect(VM_BOTH_INT(a, b), 1)) {
            SP_PUSH(VM_MAKE_INT((int64_t)((uint64_t)VM_INT_VAL(a) * (uint64_t)VM_INT_VAL(b))));
            DISPATCH();
        }
        if (__builtin_expect(VM_IS_FLOAT(a) && VM_IS_FLOAT(b), 1)) {
            SP_PUSH(VM_MAKE_FLOAT(VM_FLOAT_VAL(a) * VM_FLOAT_VAL(b)));
            DISPATCH();
        }
        if (!VM_IS_PTR(a) && !VM_IS_PTR(b)) {
            if (VM_IS_FLOAT(a) || VM_IS_FLOAT(b)) {
                double da = VM_IS_FLOAT(a) ? VM_FLOAT_VAL(a) : (VM_IS_INT(a) ? (double)VM_INT_VAL(a) : 0.0);
                double db = VM_IS_FLOAT(b) ? VM_FLOAT_VAL(b) : (VM_IS_INT(b) ? (double)VM_INT_VAL(b) : 0.0);
                SP_PUSH(VM_MAKE_FLOAT(da * db));
            } else {
                int64_t ia = VM_IS_INT(a) ? VM_INT_VAL(a) : 0;
                int64_t ib = VM_IS_INT(b) ? VM_INT_VAL(b) : 0;
                SP_PUSH(VM_MAKE_INT((int64_t)((uint64_t)ia * (uint64_t)ib)));
            }
            DISPATCH();
        }
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
        if (__builtin_expect(!VM_IS_PTR(a) && !VM_IS_PTR(b), 1)) {
            double da = VM_IS_FLOAT(a) ? VM_FLOAT_VAL(a) : (VM_IS_INT(a) ? (double)VM_INT_VAL(a) : 0.0);
            double db = VM_IS_FLOAT(b) ? VM_FLOAT_VAL(b) : (VM_IS_INT(b) ? (double)VM_INT_VAL(b) : 0.0);
            if (__builtin_expect(db == 0.0, 0)) {
                throw_exc = vm_str("Illegal division by zero");
                goto L_vm_throw_exc;
            }
            SP_PUSH(VM_MAKE_FLOAT(da / db));
            DISPATCH();
        }
        VM_TRY_BIN_OV("/");
        double db = vm_to_num(b);
        if (db == 0.0) {
            vm_temp_free(&a); vm_temp_free(&b);
            throw_exc = vm_str("Illegal division by zero");
            goto L_vm_throw_exc;
        }
        SP_PUSH(VM_MAKE_FLOAT(vm_to_num(a) / db));
        vm_temp_free(&a); vm_temp_free(&b);
        DISPATCH();
    }
    CASE(OP_MOD): {
        VMValue b = SP_POP(), a = SP_POP();
        if (__builtin_expect(VM_BOTH_INT(a, b), 1)) {
            int64_t av = VM_INT_VAL(a), bv = VM_INT_VAL(b);
            if (__builtin_expect(bv == 0, 0)) {
                throw_exc = vm_str("Illegal modulus zero");
                goto L_vm_throw_exc;
            }
            int64_t r = av % bv;
            if (r != 0 && ((r < 0) != (bv < 0))) r += bv;
            SP_PUSH(VM_MAKE_INT(r));
            DISPATCH();
        }
        VM_TRY_BIN_OV("%");
        int64_t av = vm_to_int(a), bv = vm_to_int(b);
        if (bv == 0) {
            vm_temp_free(&a); vm_temp_free(&b);
            throw_exc = vm_str("Illegal modulus zero");
            goto L_vm_throw_exc;
        }
        int64_t r = 0;
        if (!(av == INT64_MIN && bv == -1)) {
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
    CASE(OP_BIT_SHL): { VMValue b = SP_POP(), a = SP_POP(); int64_t sh = vm_to_int(b); SP_PUSH(VM_MAKE_INT((sh < 0 || sh >= 64) ? 0 : (int64_t)((uint64_t)vm_to_int(a) << (int)sh))); vm_temp_free(&a); vm_temp_free(&b); DISPATCH(); }
    CASE(OP_BIT_SHR): { VMValue b = SP_POP(), a = SP_POP(); int64_t lv = vm_to_int(a), sh = vm_to_int(b); SP_PUSH(VM_MAKE_INT((sh < 0 || sh >= 64) ? (lv < 0 ? -1 : 0) : (lv >> (int)sh))); vm_temp_free(&a); vm_temp_free(&b); DISPATCH(); }
    CASE(OP_BIT_NOT): { VMValue a = SP_POP(); SP_PUSH(VM_MAKE_INT(~vm_to_int(a))); vm_temp_free(&a); DISPATCH(); }

    /* Numeric comparisons — fast paths for inline ints and NaN-boxed doubles */
    #define VM_FAST_NUM_CMP(opstr, cmpop) do { \
        VMValue b = SP_POP(), a = SP_POP(); \
        if (__builtin_expect(VM_BOTH_INT(a, b), 1)) { \
            SP_PUSH(VM_MAKE_INT_FAST(VM_INT_VAL(a) cmpop VM_INT_VAL(b))); \
            DISPATCH(); \
        } \
        if (__builtin_expect(VM_IS_FLOAT(a) && VM_IS_FLOAT(b), 1)) { \
            SP_PUSH(VM_MAKE_INT_FAST(VM_FLOAT_VAL(a) cmpop VM_FLOAT_VAL(b))); \
            DISPATCH(); \
        } \
        if (!VM_IS_PTR(a) && !VM_IS_PTR(b)) { \
            double da = VM_IS_FLOAT(a) ? VM_FLOAT_VAL(a) : (VM_IS_INT(a) ? (double)VM_INT_VAL(a) : 0.0); \
            double db = VM_IS_FLOAT(b) ? VM_FLOAT_VAL(b) : (VM_IS_INT(b) ? (double)VM_INT_VAL(b) : 0.0); \
            SP_PUSH(VM_MAKE_INT_FAST(da cmpop db)); \
            DISPATCH(); \
        } \
        VM_TRY_BIN_OV(opstr); \
        int res = (VM_IS_ANY_INT(a) && VM_IS_ANY_INT(b)) ? (VM_ANY_INT_VAL(a) cmpop VM_ANY_INT_VAL(b)) : (vm_to_num(a) cmpop vm_to_num(b)); \
        SP_PUSH(VM_MAKE_INT(res)); \
        vm_temp_free(&a); vm_temp_free(&b); \
        DISPATCH(); \
    } while (0)

    CASE(OP_LT): { VM_FAST_NUM_CMP("<", <); }
    CASE(OP_LE): { VM_FAST_NUM_CMP("<=", <=); }
    CASE(OP_GT): { VM_FAST_NUM_CMP(">", >); }
    CASE(OP_GE): { VM_FAST_NUM_CMP(">=", >=); }
    CASE(OP_CASE_EQ): {
        VMValue b = SP_POP(), a = SP_POP();
        if (VM_BOTH_INT(a, b)) {
            SP_PUSH(VM_MAKE_INT_FAST(a == b));
            DISPATCH();
        }
        int eq = 0;
        int sa = (VM_IS_PTR(a) && (VM_PTR_TYPE(a) == VM_OBJ_STR || VM_PTR_TYPE(a) == VM_OBJ_STRBUF));
        int sb = (VM_IS_PTR(b) && (VM_PTR_TYPE(b) == VM_OBJ_STR || VM_PTR_TYPE(b) == VM_OBJ_STRBUF));
        if (sa || sb) {
            char ba[256], bb[256];
            size_t alen = 0, blen = 0;
            const char *ap = vm_bytes_view(a, ba, sizeof(ba), &alen);
            const char *bp = vm_bytes_view(b, bb, sizeof(bb), &blen);
            eq = (alen == blen && (alen == 0 || memcmp(ap, bp, alen) == 0));
        } else if (VM_IS_ANY_INT(a) && VM_IS_ANY_INT(b)) {
            eq = (VM_ANY_INT_VAL(a) == VM_ANY_INT_VAL(b));
        } else {
            eq = (vm_to_num(a) == vm_to_num(b));
        }
        SP_PUSH(VM_MAKE_INT(eq));
        vm_temp_free(&a); vm_temp_free(&b);
        DISPATCH();
    }
    CASE(OP_EQ): { VM_FAST_NUM_CMP("==", ==); }
    CASE(OP_NE): { VM_FAST_NUM_CMP("!=", !=); }
    #undef VM_FAST_NUM_CMP

    CASE(OP_STR_EQ): {
        VMValue b = SP_POP(), a = SP_POP();
        VM_TRY_BIN_OV("eq");
        char ba[64], bb[64];
        size_t alen = 0, blen = 0;
        const char *ap = vm_bytes_view(a, ba, sizeof(ba), &alen);
        const char *bp = vm_bytes_view(b, bb, sizeof(bb), &blen);
        SP_PUSH(VM_MAKE_INT(alen == blen && (alen == 0 || memcmp(ap, bp, alen) == 0)));
        vm_temp_free(&a); vm_temp_free(&b);
        DISPATCH();
    }
    CASE(OP_STR_NE): {
        VMValue b = SP_POP(), a = SP_POP();
        VM_TRY_BIN_OV("ne");
        char ba[64], bb[64];
        size_t alen = 0, blen = 0;
        const char *ap = vm_bytes_view(a, ba, sizeof(ba), &alen);
        const char *bp = vm_bytes_view(b, bb, sizeof(bb), &blen);
        SP_PUSH(VM_MAKE_INT(alen != blen || (alen > 0 && memcmp(ap, bp, alen) != 0)));
        vm_temp_free(&a); vm_temp_free(&b);
        DISPATCH();
    }

    CASE(OP_STR_LT): {
        VMValue b = SP_POP(), a = SP_POP();
        VM_TRY_BIN_OV("lt");
        char ba[64], bb[64];
        size_t alen = 0, blen = 0;
        const char *ap = vm_bytes_view(a, ba, sizeof(ba), &alen);
        const char *bp = vm_bytes_view(b, bb, sizeof(bb), &blen);
        SP_PUSH(VM_MAKE_INT(vm_memcmp_bytes(ap, alen, bp, blen) < 0));
        vm_temp_free(&a); vm_temp_free(&b);
        DISPATCH();
    }
    CASE(OP_STR_GT): {
        VMValue b = SP_POP(), a = SP_POP();
        VM_TRY_BIN_OV("gt");
        char ba[64], bb[64];
        size_t alen = 0, blen = 0;
        const char *ap = vm_bytes_view(a, ba, sizeof(ba), &alen);
        const char *bp = vm_bytes_view(b, bb, sizeof(bb), &blen);
        SP_PUSH(VM_MAKE_INT(vm_memcmp_bytes(ap, alen, bp, blen) > 0));
        vm_temp_free(&a); vm_temp_free(&b);
        DISPATCH();
    }
    CASE(OP_STR_LE): {
        VMValue b = SP_POP(), a = SP_POP();
        VM_TRY_BIN_OV("le");
        char ba[64], bb[64];
        size_t alen = 0, blen = 0;
        const char *ap = vm_bytes_view(a, ba, sizeof(ba), &alen);
        const char *bp = vm_bytes_view(b, bb, sizeof(bb), &blen);
        SP_PUSH(VM_MAKE_INT(vm_memcmp_bytes(ap, alen, bp, blen) <= 0));
        vm_temp_free(&a); vm_temp_free(&b);
        DISPATCH();
    }
    CASE(OP_STR_GE): {
        VMValue b = SP_POP(), a = SP_POP();
        VM_TRY_BIN_OV("ge");
        char ba[64], bb[64];
        size_t alen = 0, blen = 0;
        const char *ap = vm_bytes_view(a, ba, sizeof(ba), &alen);
        const char *bp = vm_bytes_view(b, bb, sizeof(bb), &blen);
        SP_PUSH(VM_MAKE_INT(vm_memcmp_bytes(ap, alen, bp, blen) >= 0));
        vm_temp_free(&a); vm_temp_free(&b);
        DISPATCH();
    }
    CASE(OP_SPACESHIP): {
        VMValue b = SP_POP(), a = SP_POP();
        if (__builtin_expect(VM_BOTH_INT(a, b), 1)) {
            int64_t av = VM_INT_VAL(a), bv = VM_INT_VAL(b);
            SP_PUSH(VM_MAKE_INT_FAST(av < bv ? -1 : (av > bv ? 1 : 0)));
            DISPATCH();
        }
        if (VM_IS_FLOAT(a) && VM_IS_FLOAT(b)) {
            double av = VM_FLOAT_VAL(a), bv = VM_FLOAT_VAL(b);
            SP_PUSH(VM_MAKE_INT_FAST(av < bv ? -1 : (av > bv ? 1 : 0)));
            DISPATCH();
        }
        VM_TRY_BIN_OV("<=>");
        int cmp;
        if (VM_IS_ANY_INT(a) && VM_IS_ANY_INT(b)) {
            int64_t av = VM_ANY_INT_VAL(a), bv = VM_ANY_INT_VAL(b);
            cmp = av < bv ? -1 : (av > bv ? 1 : 0);
        } else {
            double av = vm_to_num(a), bv = vm_to_num(b);   /* compare as numbers, not truncated ints */
            cmp = av < bv ? -1 : (av > bv ? 1 : 0);
        }
        SP_PUSH(VM_MAKE_INT(cmp));
        vm_temp_free(&a); vm_temp_free(&b);              /* also fixes a string-operand leak */
        DISPATCH();
    }
    CASE(OP_STR_CMP): {
        VMValue b = SP_POP(), a = SP_POP();
        VM_TRY_BIN_OV("cmp");
        char ba[64], bb[64];
        size_t alen = 0, blen = 0;
        const char *ap = vm_bytes_view(a, ba, sizeof(ba), &alen);
        const char *bp = vm_bytes_view(b, bb, sizeof(bb), &blen);
        int c = vm_memcmp_bytes(ap, alen, bp, blen);
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
        if (target <= ip) {
            if (__builtin_expect(vm->gc_count >= vm->gc_threshold && vm->exec_base == 0 && vm->gc_pause == 0, 0)) {
                frame->ip = target;
                vm->stack_top = sp;
                vm_gc_collect(vm);
                stack = vm->stack;
                RELOAD();
            }
            if (__builtin_expect((++sig_poll & 31) == 0 && strada_has_pending_signals(), 0)) {
                frame->ip = target;
                vm->stack_top = sp;
                int __th = vm_c_dispatch_signals(vm, &throw_exc);
                stack = vm->stack;
                RELOAD();
                if (__th) goto L_vm_throw_exc;
            }
        }
        ip = target;
        DISPATCH();
    }

    CASE(OP_JMP_IF_FALSE): {
        uint16_t off = read_u16(ip); ip += 2;
        VMValue c = SP_POP();
        int _truthy = 0;
        VM_VALUE_IS_TRUTHY(c, _truthy);
        if (!_truthy) {
            if (__builtin_expect(off >= chunk->code_len, 0)) {
                fprintf(stderr, "VM error: jump offset %u out of bounds (%zu)\n", (unsigned)off, chunk->code_len);
                vm->stack_top = sp; vm->exec_base = saved_exec_base; VM_EXEC_RETURN(VM_UNDEF_VAL);
            }
            uint8_t *target = chunk->code + off;
            if (target <= ip && __builtin_expect((++sig_poll & 31) == 0 && strada_has_pending_signals(), 0)) {
                frame->ip = target;
                vm->stack_top = sp;
                int __th = vm_c_dispatch_signals(vm, &throw_exc);
                stack = vm->stack;
                RELOAD();
                if (__th) goto L_vm_throw_exc;
            }
            ip = target;
        }
        DISPATCH();
    }

    CASE(OP_JMP_IF_TRUE): {
        uint16_t off = read_u16(ip); ip += 2;
        VMValue c = SP_POP();
        int _truthy = 0;
        VM_VALUE_IS_TRUTHY(c, _truthy);
        if (_truthy) {
            if (__builtin_expect(off >= chunk->code_len, 0)) {
                fprintf(stderr, "VM error: jump offset %u out of bounds (%zu)\n", (unsigned)off, chunk->code_len);
                vm->stack_top = sp; vm->exec_base = saved_exec_base; VM_EXEC_RETURN(VM_UNDEF_VAL);
            }
            uint8_t *target = chunk->code + off;
            if (target <= ip && __builtin_expect((++sig_poll & 31) == 0 && strada_has_pending_signals(), 0)) {
                frame->ip = target;
                vm->stack_top = sp;
                int __th = vm_c_dispatch_signals(vm, &throw_exc);
                stack = vm->stack;
                RELOAD();
                if (__th) goto L_vm_throw_exc;
            }
            ip = target;
        }
        DISPATCH();
    }

    CASE(OP_SET_CALL_CTX): {
        uint8_t cctx = *ip++;
        vm->pending_call_context = (cctx == 255) ? frame->call_context : cctx;
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
        VMValue sbuf[16];
        VMValue *sitems = NULL;
        int cnt = 0;
        uint8_t call_ctx = vm->pending_call_context;
        vm->pending_call_context = 0;
        frame->ip = ip; vm->stack_top = sp;
        int exc = vm_collect_spread_items(vm, arr_val, sbuf, 16, &sitems, &cnt);
        stack = vm->stack; RELOAD();
        if (exc) { VM_CHECK_EXC(); }
        vm->pending_call_context = call_ctx;
        call_argc = cnt;
        for (int i = 0; i < cnt; i++) SP_PUSH(sitems[i]);
        if (sitems && sitems != sbuf) free(sitems);
        goto L_do_op_call;
    }

    L_do_op_call: {
        uint16_t fidx = call_fidx;
        int argc = call_argc;
        VMChunk *callee = &vm->program->funcs[fidx];
        frame->ip = ip; vm->stack_top = sp;

        if (__builtin_expect(vm->recursion_limit > 0 && (int)vm->frame_count >= vm->recursion_limit, 0)) {
            fprintf(stderr, "Deep recursion detected (limit %d) — see core::set_recursion_limit\n", vm->recursion_limit);
            exit(1);
        }
        if (__builtin_expect(vm->frame_count >= vm->frame_cap, 0)) {
            vm_frames_grow(vm);
        }
        VMFrame *nf = &vm->frames[vm->frame_count++];
        nf->chunk = callee;
        nf->ip = callee->code;
        nf->stack_base = sp - argc;
        nf->closure = NULL;
        nf->global_save_depth = vm->global_save_count;
        nf->passed_argc = argc;
        nf->call_context = vm->pending_call_context;
        vm->pending_call_context = 0;

        int lcnt = callee->local_count + 1;
        if (__builtin_expect(!callee->has_variadic, 1)) {
            int pmax = lcnt;
            if (callee->fixed_param_count >= 0 && callee->fixed_param_count < pmax) {
                pmax = callee->fixed_param_count;
            }
            int zf = argc < pmax ? argc : pmax;
            VMValue *nloc = vm_locals_alloc(vm, lcnt, zf);
            nf->locals = nloc;
            if (__builtin_expect(argc <= pmax, 1)) {
                if (argc == 1) {
                    nloc[0] = stack[--sp];
                } else if (argc == 2) {
                    nloc[1] = stack[--sp];
                    nloc[0] = stack[--sp];
                } else {
                    for (int i = argc - 1; i >= 0; i--) nloc[i] = stack[--sp];
                }
            } else {
                for (int i = argc - 1; i >= 0; i--) {
                    VMValue av = stack[--sp];
                    if (i < pmax) nloc[i] = av;
                }
            }
        } else {
            nf->locals = vm_locals_alloc(vm, lcnt, 0);
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
        }

        frame = nf; chunk = callee; ip = callee->code; locals = nf->locals;
        vm->stack_top = sp;
        DISPATCH();
    }

    CASE(OP_RETURN): {
        VMValue result = SP_POP();
        if (__builtin_expect(vm->global_save_count > frame->global_save_depth, 0)) {
            frame->ip = ip;
            vm->stack_top = sp;
            vm->gc_pause++;
            vm_unwind_global_saves(vm, frame->global_save_depth);
            vm->gc_pause--;
            stack = vm->stack;
            frame = &vm->frames[vm->frame_count - 1];
            locals = frame->locals;
        }
        int lc = chunk->local_count;
        frame->locals = NULL;
        vm_locals_free(vm, locals, lc + 1);
        if (__builtin_expect(vm->exc_top > 0, 0)) {
            while (vm->exc_top > 0 && vm->exc_stack[vm->exc_top - 1].frame_count >= vm->frame_count) {
                vm->exc_top--;
            }
        }
        vm->frame_count--;
        if (__builtin_expect(vm->frame_count == vm->exec_base, 0)) {
            vm->stack_top = frame->stack_base;
            vm->exec_base = saved_exec_base;
            VM_EXEC_RETURN(result);
        }
        sp = frame->stack_base;
        stack[sp++] = result;
        vm->stack_top = sp;
        frame = &vm->frames[vm->frame_count - 1];
        chunk = frame->chunk; ip = frame->ip; locals = frame->locals;
        if (__builtin_expect(vm->gc_count >= vm->gc_threshold && vm->exec_base == 0 && vm->gc_pause == 0, 0)) {
            frame->ip = ip;
            vm->stack_top = sp;
            vm_gc_collect(vm);
            stack = vm->stack;
            RELOAD();
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
        FILE *outfp = stdout;
        if (VM_IS_PTR(vm->default_fh) && VM_PTR_TYPE(vm->default_fh) == VM_OBJ_FILEHANDLE) {
            VMFileHandle *fh = (VMFileHandle*)VM_TO_PTR(vm->default_fh);
            outfp = (FILE*)fh->fp;
        }
        vm_write_val(outfp, v, 1);
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
        FILE *outfp = stdout;
        if (VM_IS_PTR(vm->default_fh) && VM_PTR_TYPE(vm->default_fh) == VM_OBJ_FILEHANDLE) {
            VMFileHandle *fh = (VMFileHandle*)VM_TO_PTR(vm->default_fh);
            outfp = (FILE*)fh->fp;
        }
        vm_write_val(outfp, v, 0);
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
        if (VM_IS_PTR(fhv) && VM_PTR_TYPE(fhv) == VM_OBJ_FILEHANDLE) {
            VMFileHandle *fh = (VMFileHandle*)VM_TO_PTR(fhv);
            vm_write_val((FILE*)fh->fp, str, 1);
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
        if (VM_IS_PTR(fhv) && VM_PTR_TYPE(fhv) == VM_OBJ_FILEHANDLE) {
            VMFileHandle *fh = (VMFileHandle*)VM_TO_PTR(fhv);
            vm_write_val((FILE*)fh->fp, str, 0);
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
        if ((VM_IS_PTR(a) && ((VMObjHeader*)VM_TO_PTR(a))->is_utf8) ||
            (VM_IS_PTR(b) && ((VMObjHeader*)VM_TO_PTR(b))->is_utf8)) {
            r->hdr.is_utf8 = 1;
        }
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
        SP_PUSH(chunk->str_const_vals[idx]);
        DISPATCH();
    }

    CASE(OP_INCR): {
        uint16_t s = read_u16(ip); ip += 2;
        VMValue cur_v = locals[s];
        if (__builtin_expect(VM_IS_INT(cur_v), 1)) {
            locals[s] = VM_MAKE_INT(VM_INT_VAL(cur_v) + 1);
            DISPATCH();
        }
        if (VM_IS_FLOAT(cur_v)) {
            locals[s] = VM_MAKE_FLOAT(VM_FLOAT_VAL(cur_v) + 1.0);
            DISPATCH();
        }
        VMValue *islot = (VM_IS_PTR(cur_v) && VM_PTR_TYPE(cur_v) == VM_OBJ_CELL)
                         ? &VM_AS_CELL(cur_v)->val : &locals[s];
        if (__builtin_expect(VM_IS_PTR(*islot) && VM_PTR_TYPE(*islot) == VM_OBJ_TIED_SCALAR, 0)) {
            VMValue ts = *islot;
            frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
            VMValue cur = vm_tied_scalar_fetch(vm, ts);
            if (!vm->has_pending_exc) {
                VMValue nxt;
                if (VM_IS_ANY_INT(cur)) {
                    nxt = VM_MAKE_INT((int64_t)((uint64_t)VM_ANY_INT_VAL(cur) + 1ULL));
                } else {
                    double n = vm_to_num(cur) + 1.0;
                    int was_float = VM_IS_FLOAT(cur);
                    nxt = (!was_float && isfinite(n) && n == (double)(int64_t)n && n >= -9.2e18 && n <= 9.2e18)
                          ? VM_MAKE_INT((int64_t)n) : VM_MAKE_FLOAT(n);
                }
                vm_tied_scalar_store(vm, ts, nxt);
            }
            vm->gc_pause--;
            stack = vm->stack; RELOAD();
            VM_CHECK_EXC();
            DISPATCH();
        }
        if (VM_IS_ANY_INT(*islot)) {
            *islot = VM_MAKE_INT((int64_t)((uint64_t)VM_ANY_INT_VAL(*islot) + 1ULL));
        } else {
            /* Coerce undef/float/string instead of corrupting the pointer with += 2. */
            double n = vm_to_num(*islot) + 1.0;
            int was_float = VM_IS_FLOAT(*islot);
            VMValue old = *islot;
            if (!was_float && isfinite(n) && n == (double)(int64_t)n && n >= -9.2e18 && n <= 9.2e18)
                *islot = VM_MAKE_INT((int64_t)n);
            else
                *islot = VM_MAKE_FLOAT(n);
            vm_temp_free(&old);
        }
        DISPATCH();
    }
    CASE(OP_DECR): {
        uint16_t s = read_u16(ip); ip += 2;
        VMValue cur_v = locals[s];
        if (__builtin_expect(VM_IS_INT(cur_v), 1)) {
            locals[s] = VM_MAKE_INT(VM_INT_VAL(cur_v) - 1);
            DISPATCH();
        }
        if (VM_IS_FLOAT(cur_v)) {
            locals[s] = VM_MAKE_FLOAT(VM_FLOAT_VAL(cur_v) - 1.0);
            DISPATCH();
        }
        VMValue *dslot = (VM_IS_PTR(cur_v) && VM_PTR_TYPE(cur_v) == VM_OBJ_CELL)
                         ? &VM_AS_CELL(cur_v)->val : &locals[s];
        if (__builtin_expect(VM_IS_PTR(*dslot) && VM_PTR_TYPE(*dslot) == VM_OBJ_TIED_SCALAR, 0)) {
            VMValue ts = *dslot;
            frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
            VMValue cur = vm_tied_scalar_fetch(vm, ts);
            if (!vm->has_pending_exc) {
                VMValue nxt;
                if (VM_IS_ANY_INT(cur)) {
                    nxt = VM_MAKE_INT((int64_t)((uint64_t)VM_ANY_INT_VAL(cur) - 1ULL));
                } else {
                    double n = vm_to_num(cur) - 1.0;
                    int was_float = VM_IS_FLOAT(cur);
                    nxt = (!was_float && isfinite(n) && n == (double)(int64_t)n && n >= -9.2e18 && n <= 9.2e18)
                          ? VM_MAKE_INT((int64_t)n) : VM_MAKE_FLOAT(n);
                }
                vm_tied_scalar_store(vm, ts, nxt);
            }
            vm->gc_pause--;
            stack = vm->stack; RELOAD();
            VM_CHECK_EXC();
            DISPATCH();
        }
        if (VM_IS_ANY_INT(*dslot)) {
            *dslot = VM_MAKE_INT((int64_t)((uint64_t)VM_ANY_INT_VAL(*dslot) - 1ULL));
        } else {
            double n = vm_to_num(*dslot) - 1.0;
            int was_float = VM_IS_FLOAT(*dslot);
            VMValue old = *dslot;
            if (!was_float && isfinite(n) && n == (double)(int64_t)n && n >= -9.2e18 && n <= 9.2e18)
                *dslot = VM_MAKE_INT((int64_t)n);
            else
                *dslot = VM_MAKE_FLOAT(n);
            vm_temp_free(&old);
        }
        DISPATCH();
    }
    CASE(OP_POST_INCR): {
        uint16_t s = read_u16(ip); ip += 2;
        VMValue cur_v = locals[s];
        if (__builtin_expect(VM_IS_INT(cur_v), 1)) {
            locals[s] = VM_MAKE_INT(VM_INT_VAL(cur_v) + 1);
            SP_PUSH(cur_v);
            DISPATCH();
        }
        if (VM_IS_FLOAT(cur_v)) {
            locals[s] = VM_MAKE_FLOAT(VM_FLOAT_VAL(cur_v) + 1.0);
            SP_PUSH(cur_v);
            DISPATCH();
        }
        VMValue *islot = (VM_IS_PTR(cur_v) && VM_PTR_TYPE(cur_v) == VM_OBJ_CELL)
                         ? &VM_AS_CELL(cur_v)->val : &locals[s];
        if (__builtin_expect(VM_IS_PTR(*islot) && VM_PTR_TYPE(*islot) == VM_OBJ_TIED_SCALAR, 0)) {
            VMValue ts = *islot;
            frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
            VMValue cur = vm_tied_scalar_fetch(vm, ts);
            if (!vm->has_pending_exc) {
                VMValue nxt;
                if (VM_IS_ANY_INT(cur)) {
                    nxt = VM_MAKE_INT((int64_t)((uint64_t)VM_ANY_INT_VAL(cur) + 1ULL));
                } else {
                    double n = vm_to_num(cur) + 1.0;
                    int was_float = VM_IS_FLOAT(cur);
                    nxt = (!was_float && isfinite(n) && n == (double)(int64_t)n && n >= -9.2e18 && n <= 9.2e18)
                          ? VM_MAKE_INT((int64_t)n) : VM_MAKE_FLOAT(n);
                }
                vm_tied_scalar_store(vm, ts, nxt);
            }
            vm->gc_pause--;
            stack = vm->stack; RELOAD();
            VM_CHECK_EXC();
            SP_PUSH(cur);
            DISPATCH();
        }
        VMValue old = *islot;
        if (VM_IS_ANY_INT(old)) {
            *islot = VM_MAKE_INT((int64_t)((uint64_t)VM_ANY_INT_VAL(old) + 1ULL));
        } else {
            double n = vm_to_num(old) + 1.0;
            int was_float = VM_IS_FLOAT(old);
            if (!was_float && isfinite(n) && n == (double)(int64_t)n && n >= -9.2e18 && n <= 9.2e18)
                *islot = VM_MAKE_INT((int64_t)n);
            else
                *islot = VM_MAKE_FLOAT(n);
        }
        SP_PUSH(old);
        DISPATCH();
    }
    CASE(OP_POST_DECR): {
        uint16_t s = read_u16(ip); ip += 2;
        VMValue cur_v = locals[s];
        if (__builtin_expect(VM_IS_INT(cur_v), 1)) {
            locals[s] = VM_MAKE_INT(VM_INT_VAL(cur_v) - 1);
            SP_PUSH(cur_v);
            DISPATCH();
        }
        if (VM_IS_FLOAT(cur_v)) {
            locals[s] = VM_MAKE_FLOAT(VM_FLOAT_VAL(cur_v) - 1.0);
            SP_PUSH(cur_v);
            DISPATCH();
        }
        VMValue *dslot = (VM_IS_PTR(cur_v) && VM_PTR_TYPE(cur_v) == VM_OBJ_CELL)
                         ? &VM_AS_CELL(cur_v)->val : &locals[s];
        if (__builtin_expect(VM_IS_PTR(*dslot) && VM_PTR_TYPE(*dslot) == VM_OBJ_TIED_SCALAR, 0)) {
            VMValue ts = *dslot;
            frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
            VMValue cur = vm_tied_scalar_fetch(vm, ts);
            if (!vm->has_pending_exc) {
                VMValue nxt;
                if (VM_IS_ANY_INT(cur)) {
                    nxt = VM_MAKE_INT((int64_t)((uint64_t)VM_ANY_INT_VAL(cur) - 1ULL));
                } else {
                    double n = vm_to_num(cur) - 1.0;
                    int was_float = VM_IS_FLOAT(cur);
                    nxt = (!was_float && isfinite(n) && n == (double)(int64_t)n && n >= -9.2e18 && n <= 9.2e18)
                          ? VM_MAKE_INT((int64_t)n) : VM_MAKE_FLOAT(n);
                }
                vm_tied_scalar_store(vm, ts, nxt);
            }
            vm->gc_pause--;
            stack = vm->stack; RELOAD();
            VM_CHECK_EXC();
            SP_PUSH(cur);
            DISPATCH();
        }
        VMValue old = *dslot;
        if (VM_IS_ANY_INT(old)) {
            *dslot = VM_MAKE_INT((int64_t)((uint64_t)VM_ANY_INT_VAL(old) - 1ULL));
        } else {
            double n = vm_to_num(old) - 1.0;
            int was_float = VM_IS_FLOAT(old);
            if (!was_float && isfinite(n) && n == (double)(int64_t)n && n >= -9.2e18 && n <= 9.2e18)
                *dslot = VM_MAKE_INT((int64_t)n);
            else
                *dslot = VM_MAKE_FLOAT(n);
        }
        SP_PUSH(old);
        DISPATCH();
    }

    /* ===== Arrays ===== */
    CASE(OP_NEW_ARRAY): { uint32_t c = read_u32(ip); ip += 4; SP_PUSH(VM_MAKE_PTR(vm_array_new(c>0?c:8))); DISPATCH(); }
    CASE(OP_ARRAY_PUSH): {
        VMValue v = SP_POP(), a = SP_POP();
        if (VM_IS_PTR(a) && VM_PTR_TYPE(a) == VM_OBJ_ARRAY) {
            VMArray *da = VM_AS_ARRAY(a);
            if (__builtin_expect(!VM_IS_UNDEF(da->tied_obj), 0)) {
                int fidx = vm_tied_array_find_method(vm, da->tied_obj, "PUSH");
                if (fidx >= 0) {
                    frame->ip = ip; vm->stack_top = sp;
                    vm_tied_array_call(vm, da->tied_obj, fidx, &v, 1);
                    stack = vm->stack; RELOAD();
                    VM_CHECK_EXC();
                }
            } else {
                vm_array_push(da, v);
            }
        }
        DISPATCH();
    }
    CASE(OP_ARRAY_EXTEND): {
        VMValue src = SP_POP(), dst = SP_POP();
        if (VM_IS_PTR(dst) && VM_PTR_TYPE(dst) == VM_OBJ_ARRAY) {
            VMArray *da = VM_AS_ARRAY(dst);
            if (VM_IS_PTR(src) && VM_PTR_TYPE(src) == VM_OBJ_ARRAY) {
                VMArray *sa = VM_AS_ARRAY(src);
                if (__builtin_expect(!VM_IS_UNDEF(sa->tied_obj), 0)) {
                    frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
                    int64_t ssz = vm_tied_array_fetchsize(vm, sa->tied_obj);
                    if (vm->has_pending_exc) { vm->gc_pause--; stack = vm->stack; RELOAD(); VM_CHECK_EXC(); }
                    int sn = (ssz > 0) ? (int)ssz : 0;
                    if (sn > 0) {
                        VMValue ebuf[16];
                        VMValue *elems = (sn > 16) ? malloc((size_t)sn * sizeof(VMValue)) : ebuf;
                        int ffidx = vm_tied_array_find_method(vm, sa->tied_obj, "FETCH");
                        for (int i = 0; i < sn; i++) {
                            VMValue iv = VM_MAKE_INT(i);
                            elems[i] = (ffidx >= 0) ? vm_tied_array_call(vm, sa->tied_obj, ffidx, &iv, 1) : VM_UNDEF_VAL;
                            if (vm->has_pending_exc) {
                                if (elems != ebuf) free(elems);
                                vm->gc_pause--; stack = vm->stack; RELOAD(); VM_CHECK_EXC();
                            }
                        }
                        if (!VM_IS_UNDEF(da->tied_obj)) {
                            int pfidx = vm_tied_array_find_method(vm, da->tied_obj, "PUSH");
                            if (pfidx >= 0) vm_tied_array_call(vm, da->tied_obj, pfidx, elems, sn);
                        } else {
                            for (int i = 0; i < sn; i++) vm_array_push(da, vm_val_copy(elems[i]));
                        }
                        if (elems != ebuf) free(elems);
                    }
                    vm->gc_pause--;
                    stack = vm->stack; RELOAD();
                    VM_CHECK_EXC();
                } else if (__builtin_expect(!VM_IS_UNDEF(da->tied_obj), 0)) {
                    if (sa->size > 0) {
                        int pfidx = vm_tied_array_find_method(vm, da->tied_obj, "PUSH");
                        if (pfidx >= 0) {
                            frame->ip = ip; vm->stack_top = sp;
                            vm_tied_array_call(vm, da->tied_obj, pfidx, sa->items, sa->size);
                            stack = vm->stack; RELOAD();
                            VM_CHECK_EXC();
                        }
                    }
                } else {
                    for (int i = 0; i < sa->size; i++) {
                        vm_array_push(da, vm_val_copy(sa->items[i]));
                    }
                }
            } else if (!VM_IS_UNDEF(src)) {
                if (__builtin_expect(!VM_IS_UNDEF(da->tied_obj), 0)) {
                    int pfidx = vm_tied_array_find_method(vm, da->tied_obj, "PUSH");
                    if (pfidx >= 0) {
                        frame->ip = ip; vm->stack_top = sp;
                        vm_tied_array_call(vm, da->tied_obj, pfidx, &src, 1);
                        stack = vm->stack; RELOAD();
                        VM_CHECK_EXC();
                    }
                } else {
                    vm_array_push(da, src);
                }
            }
        }
        DISPATCH();
    }
    CASE(OP_MAP_PUSH): {
        VMValue src = SP_POP(), dst = SP_POP();
        if (VM_IS_PTR(dst) && VM_PTR_TYPE(dst) == VM_OBJ_ARRAY) {
            VMArray *da = VM_AS_ARRAY(dst);
            if (VM_IS_PTR(src) && VM_PTR_TYPE(src) == VM_OBJ_ARRAY) {
                VMArray *sa = VM_AS_ARRAY(src);
                if (__builtin_expect(!VM_IS_UNDEF(sa->tied_obj), 0)) {
                    frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
                    int64_t ssz = vm_tied_array_fetchsize(vm, sa->tied_obj);
                    if (vm->has_pending_exc) { vm->gc_pause--; stack = vm->stack; RELOAD(); VM_CHECK_EXC(); }
                    int sn = (ssz > 0) ? (int)ssz : 0;
                    if (sn > 0) {
                        VMValue ebuf[16];
                        VMValue *elems = (sn > 16) ? malloc((size_t)sn * sizeof(VMValue)) : ebuf;
                        int ffidx = vm_tied_array_find_method(vm, sa->tied_obj, "FETCH");
                        for (int i = 0; i < sn; i++) {
                            VMValue iv = VM_MAKE_INT(i);
                            elems[i] = (ffidx >= 0) ? vm_tied_array_call(vm, sa->tied_obj, ffidx, &iv, 1) : VM_UNDEF_VAL;
                            if (vm->has_pending_exc) {
                                if (elems != ebuf) free(elems);
                                vm->gc_pause--; stack = vm->stack; RELOAD(); VM_CHECK_EXC();
                            }
                        }
                        for (int i = 0; i < sn; i++) vm_array_push(da, vm_val_copy(elems[i]));
                        if (elems != ebuf) free(elems);
                    }
                    vm->gc_pause--;
                    stack = vm->stack; RELOAD();
                    VM_CHECK_EXC();
                } else {
                    for (int i = 0; i < sa->size; i++) {
                        vm_array_push(da, vm_val_copy(sa->items[i]));
                    }
                }
            } else if (VM_IS_PTR(src) && VM_PTR_TYPE(src) == VM_OBJ_NATIVE_SV) {
                VMNativeSV *nsv = (VMNativeSV*)VM_TO_PTR(src);
                if (nsv->sv && strada_sv_is_array_or_aref(nsv->sv)) {
                    int64_t sn = strada_av_fetchsize_sv(nsv->sv);
                    for (int64_t i = 0; i < sn; i++) {
                        StradaValue *el = strada_av_fetch_sv(nsv->sv, i);
                        vm_array_push(da, sv_to_vm_depth(el, 0));
                        strada_decref(el);
                    }
                } else {
                    vm_array_push(da, src);
                }
            } else {
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
            if (__builtin_expect(!VM_IS_UNDEF(sa->tied_obj) || !VM_IS_UNDEF(ia->tied_obj), 0)) {
                frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
                int64_t icnt = !VM_IS_UNDEF(ia->tied_obj) ? vm_tied_array_fetchsize(vm, ia->tied_obj) : (int64_t)ia->size;
                int64_t ssz = (!vm->has_pending_exc && !VM_IS_UNDEF(sa->tied_obj)) ? vm_tied_array_fetchsize(vm, sa->tied_obj) : (int64_t)sa->size;
                int iff = !VM_IS_UNDEF(ia->tied_obj) ? vm_tied_array_find_method(vm, ia->tied_obj, "FETCH") : -1;
                int sff = !VM_IS_UNDEF(sa->tied_obj) ? vm_tied_array_find_method(vm, sa->tied_obj, "FETCH") : -1;
                for (int64_t i = 0; i < icnt && !vm->has_pending_exc; i++) {
                    VMValue iv_val;
                    if (!VM_IS_UNDEF(ia->tied_obj)) {
                        VMValue ii_arg = VM_MAKE_INT(i);
                        iv_val = (iff >= 0) ? vm_tied_array_call(vm, ia->tied_obj, iff, &ii_arg, 1) : VM_UNDEF_VAL;
                        if (vm->has_pending_exc) break;
                    } else {
                        iv_val = ia->items[i];
                    }
                    int64_t ii = vm_to_int(iv_val);
                    if (!VM_IS_UNDEF(sa->tied_obj)) {
                        if (ii < 0) ii += ssz;
                        VMValue arg = VM_MAKE_INT(ii);
                        VMValue e = (sff >= 0) ? vm_tied_array_call(vm, sa->tied_obj, sff, &arg, 1) : VM_UNDEF_VAL;
                        if (vm->has_pending_exc) break;
                        vm_array_push(ra, e);
                    } else {
                        VMValue e = vm_array_get(sa, ii);
                        vm_array_push(ra, vm_val_copy(e));
                    }
                }
                vm->gc_pause--;
                stack = vm->stack; RELOAD();
                VM_CHECK_EXC();
            } else {
                for (int i = 0; i < ia->size; i++) {
                    int64_t ii = vm_to_int(ia->items[i]);
                    VMValue *slot = vm->has_weakrefs ? vm_array_slot_ptr(sa, ii) : NULL;
                    VMValue e;
                    if (slot && VM_IS_PTR(*slot) && VM_PTR_TYPE(*slot) == VM_OBJ_WEAKREF) {
                        frame->ip = ip; vm->stack_top = sp;
                        VMValue roots[3] = { res, src, idxs };
                        e = vm_unwrap_weak_gc(vm, *slot, roots, 3);
                        stack = vm->stack; RELOAD();
                        ia = VM_AS_ARRAY(idxs); sa = VM_AS_ARRAY(src); ra = VM_AS_ARRAY(res);
                    } else {
                        e = vm_array_get(sa, ii);
                    }
                    vm_array_push(ra, vm_val_copy(e));
                }
            }
        }
        DISPATCH();
    }
    CASE(OP_ARRAY_GET): {
        VMValue idx = SP_POP(), arr = SP_POP();
        if (VM_IS_PTR(arr) && VM_PTR_TYPE(arr) == VM_OBJ_ARRAY) {
            VMArray *a = VM_AS_ARRAY(arr);
            if (__builtin_expect(!VM_IS_UNDEF(a->tied_obj), 0)) {
                frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
                int64_t ii = vm_to_int(idx);
                if (ii < 0) ii += vm_tied_array_fetchsize(vm, a->tied_obj);
                VMValue r = VM_UNDEF_VAL;
                if (!vm->has_pending_exc) {
                    int fidx = vm_tied_array_find_method(vm, a->tied_obj, "FETCH");
                    if (fidx >= 0) {
                        VMValue iv = VM_MAKE_INT(ii);
                        r = vm_tied_array_call(vm, a->tied_obj, fidx, &iv, 1);
                    }
                }
                vm->gc_pause--;
                stack = vm->stack; RELOAD();
                VM_CHECK_EXC();
                SP_PUSH(r);
            } else {
                int64_t ii = vm_to_int(idx);
                VMValue *slot = vm->has_weakrefs ? vm_array_slot_ptr(a, ii) : NULL;
                if (slot && VM_IS_PTR(*slot) && VM_PTR_TYPE(*slot) == VM_OBJ_WEAKREF) {
                    frame->ip = ip; vm->stack_top = sp;
                    VMValue roots[2] = { arr, idx };
                    VMValue e = vm_unwrap_weak_gc(vm, *slot, roots, 2);
                    stack = vm->stack; RELOAD();
                    SP_PUSH(vm_val_copy(e));
                } else {
                    VMValue e = vm_array_get(a, ii);
                    SP_PUSH(vm_val_copy(e));
                }
            }
        } else SP_PUSH(VM_UNDEF_VAL);
        vm_temp_free(&idx);
        DISPATCH();
    }
    CASE(OP_ARRAY_SET): {
        VMValue v = SP_POP(), idx = SP_POP(), arr = SP_POP();
        if (VM_IS_PTR(arr) && VM_PTR_TYPE(arr) == VM_OBJ_ARRAY) {
            VMArray *a = VM_AS_ARRAY(arr);
            if (__builtin_expect(!VM_IS_UNDEF(a->tied_obj), 0)) {
                frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
                int64_t ii = vm_to_int(idx);
                if (ii < 0) ii += vm_tied_array_fetchsize(vm, a->tied_obj);
                if (!vm->has_pending_exc) {
                    int fidx = vm_tied_array_find_method(vm, a->tied_obj, "STORE");
                    if (fidx >= 0) {
                        VMValue sargs[2] = { VM_MAKE_INT(ii), v };
                        vm_tied_array_call(vm, a->tied_obj, fidx, sargs, 2);
                    }
                }
                vm->gc_pause--;
                stack = vm->stack; RELOAD();
                VM_CHECK_EXC();
            } else {
                vm_array_set(a, vm_to_int(idx), v);
            }
        }
        vm_temp_free(&idx);
        DISPATCH();
    }
    CASE(OP_ARRAY_SIZE): {
        VMValue a = SP_POP();
        if (VM_IS_PTR(a) && VM_PTR_TYPE(a) == VM_OBJ_ARRAY) {
            VMArray *arr = VM_AS_ARRAY(a);
            if (__builtin_expect(!VM_IS_UNDEF(arr->tied_obj), 0)) {
                frame->ip = ip; vm->stack_top = sp;
                int64_t sz = vm_tied_array_fetchsize(vm, arr->tied_obj);
                stack = vm->stack; RELOAD();
                VM_CHECK_EXC();
                SP_PUSH(VM_MAKE_INT(sz));
            } else {
                SP_PUSH(VM_MAKE_INT(arr->size));
            }
        } else {
            SP_PUSH(VM_MAKE_INT(0));
        }
        DISPATCH();
    }
    CASE(OP_ARRAY_POP): {
        VMValue a = SP_POP();
        if (VM_IS_PTR(a) && VM_PTR_TYPE(a) == VM_OBJ_ARRAY) {
            VMArray *arr = VM_AS_ARRAY(a);
            if (__builtin_expect(!VM_IS_UNDEF(arr->tied_obj), 0)) {
                int fidx = vm_tied_array_find_method(vm, arr->tied_obj, "POP");
                if (fidx >= 0) {
                    frame->ip = ip; vm->stack_top = sp;
                    VMValue r = vm_tied_array_call(vm, arr->tied_obj, fidx, NULL, 0);
                    stack = vm->stack; RELOAD();
                    VM_CHECK_EXC();
                    SP_PUSH(r);
                } else {
                    SP_PUSH(VM_UNDEF_VAL);
                }
            } else {
                if (__builtin_expect(vm->has_weakrefs && arr->size > 0 &&
                    VM_IS_PTR(arr->items[arr->size - 1]) &&
                    VM_PTR_TYPE(arr->items[arr->size - 1]) == VM_OBJ_WEAKREF, 0)) {
                    arr->size--;
                    if (arr->holes) arr->holes[arr->size] = 0;
                    VMValue raw = arr->items[arr->size];
                    frame->ip = ip; vm->stack_top = sp;
                    VMValue uw = vm_unwrap_weak_gc(vm, raw, &a, 1);
                    stack = vm->stack; RELOAD();
                    SP_PUSH(vm_val_copy(uw));
                } else {
                    SP_PUSH(vm_val_copy(vm_array_pop(arr)));
                }
            }
        } else SP_PUSH(VM_UNDEF_VAL);
        DISPATCH();
    }
    CASE(OP_ARRAY_SHIFT): {
        VMValue a = SP_POP();
        if (VM_IS_PTR(a) && VM_PTR_TYPE(a) == VM_OBJ_ARRAY) {
            VMArray *arr = VM_AS_ARRAY(a);
            if (__builtin_expect(!VM_IS_UNDEF(arr->tied_obj), 0)) {
                int fidx = vm_tied_array_find_method(vm, arr->tied_obj, "SHIFT");
                if (fidx >= 0) {
                    frame->ip = ip; vm->stack_top = sp;
                    VMValue r = vm_tied_array_call(vm, arr->tied_obj, fidx, NULL, 0);
                    stack = vm->stack; RELOAD();
                    VM_CHECK_EXC();
                    SP_PUSH(r);
                } else {
                    SP_PUSH(VM_UNDEF_VAL);
                }
            } else {
                if (__builtin_expect(vm->has_weakrefs && arr->size > 0 &&
                    VM_IS_PTR(arr->items[0]) &&
                    VM_PTR_TYPE(arr->items[0]) == VM_OBJ_WEAKREF, 0)) {
                    VMValue raw = arr->items[0];
                    memmove(arr->items, arr->items + 1, (size_t)(arr->size - 1) * sizeof(VMValue));
                    if (arr->holes) {
                        memmove(arr->holes, arr->holes + 1, (size_t)(arr->size - 1));
                        arr->holes[arr->size - 1] = 0;
                    }
                    arr->size--;
                    frame->ip = ip; vm->stack_top = sp;
                    VMValue uw = vm_unwrap_weak_gc(vm, raw, &a, 1);
                    stack = vm->stack; RELOAD();
                    SP_PUSH(vm_val_copy(uw));
                } else {
                    SP_PUSH(vm_val_copy(vm_array_shift(arr)));
                }
            }
        } else SP_PUSH(VM_UNDEF_VAL);
        DISPATCH();
    }
    CASE(OP_ARRAY_UNSHIFT): {
        VMValue v = SP_POP(), a = SP_POP();
        if (VM_IS_PTR(a) && VM_PTR_TYPE(a) == VM_OBJ_ARRAY) {
            VMArray *arr = VM_AS_ARRAY(a);
            if (__builtin_expect(!VM_IS_UNDEF(arr->tied_obj), 0)) {
                int fidx = vm_tied_array_find_method(vm, arr->tied_obj, "UNSHIFT");
                if (fidx >= 0) {
                    frame->ip = ip; vm->stack_top = sp;
                    vm_tied_array_call(vm, arr->tied_obj, fidx, &v, 1);
                    stack = vm->stack; RELOAD();
                    VM_CHECK_EXC();
                }
            } else {
                vm_array_unshift(arr, v);
            }
        }
        DISPATCH();
    }
    CASE(OP_ARRAY_REVERSE): {
        VMValue a = SP_POP();
        if (VM_IS_PTR(a) && VM_PTR_TYPE(a) == VM_OBJ_CELL) a = VM_AS_CELL(a)->val;
        if (__builtin_expect(VM_IS_PTR(a) && VM_PTR_TYPE(a) == VM_OBJ_TIED_SCALAR, 0)) {
            frame->ip = ip; vm->stack_top = sp;
            a = vm_tied_scalar_fetch(vm, a);
            stack = vm->stack; RELOAD();
            VM_CHECK_EXC();
        }
        if (VM_IS_PTR(a) && VM_PTR_TYPE(a) == VM_OBJ_ARRAY) {
            VMArray *arr = VM_AS_ARRAY(a);
            if (__builtin_expect(!VM_IS_UNDEF(arr->tied_obj), 0)) {
                frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
                int64_t sz = vm_tied_array_fetchsize(vm, arr->tied_obj);
                if (vm->has_pending_exc) { vm->gc_pause--; stack = vm->stack; RELOAD(); VM_CHECK_EXC(); }
                int n = (sz > 0) ? (int)sz : 0;
                VMArray *rev = vm_array_new(n);
                int ffidx = vm_tied_array_find_method(vm, arr->tied_obj, "FETCH");
                for (int i = n - 1; i >= 0; i--) {
                    VMValue iv = VM_MAKE_INT(i);
                    VMValue v = (ffidx >= 0) ? vm_tied_array_call(vm, arr->tied_obj, ffidx, &iv, 1) : VM_UNDEF_VAL;
                    if (vm->has_pending_exc) { vm->gc_pause--; stack = vm->stack; RELOAD(); VM_CHECK_EXC(); }
                    vm_array_push(rev, v);
                }
                vm->gc_pause--;
                stack = vm->stack; RELOAD();
                SP_PUSH(VM_MAKE_PTR(rev));
            } else {
                if (__builtin_expect(vm->has_weakrefs, 0)) {
                    frame->ip = ip; vm->stack_top = sp;
                    vm_unwrap_container_weak_gc(vm, a, NULL, 0);
                    stack = vm->stack; RELOAD();
                }
                VMArray *rev = vm_array_new(arr->size);
                for (int i = arr->size - 1; i >= 0; i--) vm_array_push(rev, vm_val_copy(arr->items[i]));
                SP_PUSH(VM_MAKE_PTR(rev));
            }
        } else if (VM_IS_PTR(a) && VM_PTR_TYPE(a) == VM_OBJ_HASH) {
            VMHash *h = VM_AS_HASH(a);
            int has_fk = 0;
            VMArray *keys = NULL;
            if (__builtin_expect(!VM_IS_UNDEF(h->tied_obj), 0)) {
                frame->ip = ip; vm->stack_top = sp;
                keys = vm_tied_hash_collect_keys(vm, h->tied_obj, &has_fk);
                stack = vm->stack; RELOAD();
                VM_CHECK_EXC();
            }
            if (has_fk) {
                int n = keys ? keys->size : 0;
                VMArray *rev = vm_array_new(n * 2);
                int ffidx = vm_tied_array_find_method(vm, h->tied_obj, "FETCH");
                frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
                for (int i = n - 1; i >= 0; i--) {
                    VMValue kv = keys->items[i];
                    VMValue vv = (ffidx >= 0) ? vm_tied_array_call(vm, h->tied_obj, ffidx, &kv, 1) : VM_UNDEF_VAL;
                    if (vm->has_pending_exc) { vm->gc_pause--; stack = vm->stack; RELOAD(); VM_CHECK_EXC(); }
                    vm_array_push(rev, vv);
                    vm_array_push(rev, vm_val_copy(kv));
                }
                vm->gc_pause--;
                stack = vm->stack; RELOAD();
                SP_PUSH(VM_MAKE_PTR(rev));
            } else {
                if (__builtin_expect(vm->has_weakrefs, 0)) {
                    frame->ip = ip; vm->stack_top = sp;
                    vm_unwrap_container_weak_gc(vm, a, NULL, 0);
                    stack = vm->stack; RELOAD();
                }
                VMArray *rev = vm_array_new(h->size > 0 ? h->size * 2 : 8);
                for (int i = h->capacity - 1; i >= 0; i--) {
                    if (h->entries[i].occupied == 1) {
                        int nonascii = 0;
                        int u8 = (vm_utf8_is_valid_str(h->entries[i].key, h->entries[i].keylen, &nonascii) && nonascii);
                        vm_array_push(rev, vm_val_copy(h->entries[i].value));
                        vm_array_push(rev, vm_make_str_flag(h->entries[i].key, h->entries[i].keylen, u8));
                    }
                }
                SP_PUSH(VM_MAKE_PTR(rev));
            }
        } else if (VM_IS_UNDEF(a)) {
            SP_PUSH(vm_str(""));
        } else if (VM_IS_PTR(a) && (VM_PTR_TYPE(a) == VM_OBJ_STR || VM_PTR_TYPE(a) == VM_OBJ_STRBUF)) {
            /* reverse() on a string: reverse characters/bytes */
            SP_PUSH(vm_reverse_str_val(a));
        } else {
            char sbuf[256];
            SP_PUSH(vm_reverse_str_val(vm_str(vm_to_cstr(a, sbuf, sizeof(sbuf)))));
        }
        DISPATCH();
    }
    CASE(OP_ARRAY_JOIN): {
        VMValue arr = SP_POP(), sep = SP_POP();
        char sepbuf[64];
        size_t sep_len = 0;
        const char *sep_str = vm_bytes_view(sep, sepbuf, sizeof(sepbuf), &sep_len);
        int sep_u8 = vm_is_utf8(sep);
        if (VM_IS_PTR(arr) && VM_PTR_TYPE(arr) == VM_OBJ_ARRAY) {
            VMArray *a = VM_AS_ARRAY(arr);
            int n = a->size;   /* no 1024 cap — join ALL elements */
            VMValue *titems = NULL;
            if (__builtin_expect(!VM_IS_UNDEF(a->tied_obj), 0)) {
                frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
                int64_t sz = vm_tied_array_fetchsize(vm, a->tied_obj);
                if (vm->has_pending_exc) { vm->gc_pause--; stack = vm->stack; RELOAD(); VM_CHECK_EXC(); }
                n = (sz > 0) ? (int)sz : 0;
                if (n > 0) {
                    titems = malloc((size_t)n * sizeof(VMValue));
                    int ffidx = vm_tied_array_find_method(vm, a->tied_obj, "FETCH");
                    for (int i = 0; i < n; i++) {
                        VMValue iv = VM_MAKE_INT(i);
                        titems[i] = (ffidx >= 0) ? vm_tied_array_call(vm, a->tied_obj, ffidx, &iv, 1) : VM_UNDEF_VAL;
                        if (vm->has_pending_exc) {
                            free(titems);
                            vm->gc_pause--; stack = vm->stack; RELOAD(); VM_CHECK_EXC();
                        }
                    }
                }
                vm->gc_pause--;
                stack = vm->stack; RELOAD();
                sep_str = vm_bytes_view(sep, sepbuf, sizeof(sepbuf), &sep_len);
            }
            const VMValue *items = titems ? titems : a->items;
            /* Calculate total length */
            size_t total = 0;
            int out_u8 = (n > 1) ? sep_u8 : 0;
            char **bufs = malloc((size_t)(n > 0 ? n : 1) * sizeof(char*));
            size_t *lens = malloc((size_t)(n > 0 ? n : 1) * sizeof(size_t));
            char tmp[64];
            for (int i = 0; i < n; i++) {
                size_t elen = 0;
                const char *s = vm_bytes_view(items[i], tmp, sizeof(tmp), &elen);
                if (vm_is_utf8(items[i])) out_u8 = 1;
                lens[i] = elen;
                bufs[i] = malloc(elen + 1);
                if (elen > 0) memcpy(bufs[i], s, elen);
                bufs[i][elen] = '\0';
                total += elen;
                if (i > 0) total += sep_len;
            }
            free(titems);
            VMString *vs = malloc(sizeof(VMString) + total + 1);
            vm_gc_init_hdr(&vs->hdr, VM_OBJ_STR);
            if (out_u8) vs->hdr.is_utf8 = 1;
            vs->len = (uint32_t)total;
            char *p = vs->data;
            for (int i = 0; i < n; i++) {
                if (i > 0 && sep_len > 0) { memcpy(p, sep_str, sep_len); p += sep_len; }
                if (lens[i] > 0) { memcpy(p, bufs[i], lens[i]); p += lens[i]; }
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
        VMClosure *cl = NULL;
        VMValue cl_val = VM_UNDEF_VAL;
        if (func_idx == 0xFFFF) {
            cl_val = SP_POP();
            if (VM_IS_PTR(cl_val) && VM_PTR_TYPE(cl_val) == VM_OBJ_CLOSURE) {
                cl = VM_AS_CLOSURE(cl_val);
                func_idx = (uint16_t)cl->func_idx;
            }
        }
        {
            /* Save current VM state around tied materialization / nested comparator calls */
            frame->ip = ip;
            vm->stack_top = sp;
            if (!VM_IS_UNDEF(cl_val)) {
                if (vm->stack_top >= vm->stack_cap) vm_stack_grow(vm, vm->stack_top);
                vm->stack[vm->stack_top++] = cl_val;
            }
            VMArray *sorted = vm_sort_materialize_input(vm, arr, 1);
            if (!VM_IS_UNDEF(cl_val)) vm->stack_top = sp;
            stack = vm->stack;
            RELOAD();
            VM_CHECK_EXC();
            if (!sorted) sorted = vm_array_new(0);

            vm->gc_pause++;
            int sn = sorted->size;
            if (sn > 1) {
                const int INS_RUN = 16;
                for (int rstart = 0; rstart < sn; rstart += INS_RUN) {
                    int rend = rstart + INS_RUN;
                    if (rend > sn) rend = sn;
                    for (int i = rstart + 1; i < rend; i++) {
                        VMValue key = sorted->items[i];
                        int j = i - 1;
                        while (j >= rstart) {
                            VMValue cargs[2] = { vm_val_copy(sorted->items[j]), vm_val_copy(key) };
                            VMValue cres = vm_execute_call_cl(vm, func_idx, cl, cargs, 2, 2);
                            if (__builtin_expect(vm->has_pending_exc, 0)) {
                                vm->gc_pause--;
                                throw_exc = vm->pending_exc;
                                vm->pending_exc = VM_UNDEF_VAL;
                                vm->has_pending_exc = 0;
                                stack = vm->stack;
                                RELOAD();
                                goto L_vm_throw_exc;
                            }
                            int64_t cmp_result = vm_to_int(cres);
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
                }
                if (sn > INS_RUN) {
                    VMValue *tmp = malloc((size_t)sn * sizeof(VMValue));
                    VMValue *src = sorted->items;
                    VMValue *dst = tmp;
                    for (int width = INS_RUN; width < sn; width *= 2) {
                        for (int left = 0; left < sn; left += 2 * width) {
                            int mid = left + width;
                            if (mid >= sn) {
                                memcpy(dst + left, src + left, (size_t)(sn - left) * sizeof(VMValue));
                                continue;
                            }
                            int right = left + 2 * width;
                            if (right > sn) right = sn;
                            int li = left, ri = mid, out = left;
                            while (li < mid && ri < right) {
                                VMValue cargs[2] = { vm_val_copy(src[li]), vm_val_copy(src[ri]) };
                                VMValue cres = vm_execute_call_cl(vm, func_idx, cl, cargs, 2, 2);
                                if (__builtin_expect(vm->has_pending_exc, 0)) {
                                    free(tmp);
                                    vm->gc_pause--;
                                    throw_exc = vm->pending_exc;
                                    vm->pending_exc = VM_UNDEF_VAL;
                                    vm->has_pending_exc = 0;
                                    stack = vm->stack;
                                    RELOAD();
                                    goto L_vm_throw_exc;
                                }
                                int64_t cmp_result = vm_to_int(cres);
                                vm_temp_free(&cres);
                                if (cmp_result <= 0) dst[out++] = src[li++];
                                else dst[out++] = src[ri++];
                            }
                            if (li < mid) {
                                memcpy(dst + out, src + li, (size_t)(mid - li) * sizeof(VMValue));
                            } else if (ri < right) {
                                memcpy(dst + out, src + ri, (size_t)(right - ri) * sizeof(VMValue));
                            }
                        }
                        VMValue *swp = src; src = dst; dst = swp;
                    }
                    if (src != sorted->items) {
                        memcpy(sorted->items, src, (size_t)sn * sizeof(VMValue));
                    }
                    free(tmp);
                }
            }
            vm->gc_pause--;

            stack = vm->stack;
            RELOAD();
            SP_PUSH(VM_MAKE_PTR(sorted));
        }
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
                int ffidx = vm_tied_array_find_method(vm, h->tied_obj, "FETCH");
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
            if (__builtin_expect(VM_IS_PTR(key) && VM_PTR_TYPE(key) == VM_OBJ_STR, 1)) {
                VMString *kstr = VM_AS_STR(key);
                uint32_t kh = (kstr->hdr.gc_immortal == 2)
                              ? ((const VMConstString*)((const char*)kstr - offsetof(VMConstString, str)))->hash
                              : vm_hash_fn_n(kstr->data, kstr->len);
                VMValue *slot = vm_hash_slot_ptr_nh(h, kstr->data, kstr->len, kh);
                if (!slot) {
                    SP_PUSH(VM_UNDEF_VAL);
                } else {
                    VMValue r = *slot;
                    if (__builtin_expect(vm->has_weakrefs && VM_IS_PTR(r) && VM_PTR_TYPE(r) == VM_OBJ_WEAKREF, 0)) {
                        frame->ip = ip; vm->stack_top = sp;
                        VMValue roots[2] = { hash, key };
                        r = vm_unwrap_weak_gc(vm, r, roots, 2);
                        stack = vm->stack; RELOAD();
                    }
                    SP_PUSH(vm_val_copy(r));
                }
            } else {
                char buf[64]; size_t klen = 0;
                const char *ks = vm_bytes_view(key, buf, sizeof(buf), &klen);
                VMValue *slot = vm->has_weakrefs ? vm_hash_slot_ptr_n(h, ks, klen) : NULL;
                if (slot && VM_IS_PTR(*slot) && VM_PTR_TYPE(*slot) == VM_OBJ_WEAKREF) {
                    frame->ip = ip; vm->stack_top = sp;
                    VMValue roots[2] = { hash, key };
                    VMValue r = vm_unwrap_weak_gc(vm, *slot, roots, 2);
                    stack = vm->stack; RELOAD();
                    SP_PUSH(vm_val_copy(r));
                } else {
                    VMValue r = vm_hash_get_n(h, ks, klen);
                    SP_PUSH(vm_val_copy(r));
                }
            }
        } else SP_PUSH(VM_UNDEF_VAL);
        vm_temp_free(&key); DISPATCH();
    }
    CASE(OP_HASH_SET): {
        VMValue v = SP_POP(), key = SP_POP(), hash = SP_POP();
        if (VM_IS_PTR(hash) && VM_PTR_TYPE(hash) == VM_OBJ_HASH) {
            VMHash *h = VM_AS_HASH(hash);
            if (__builtin_expect(!VM_IS_UNDEF(h->tied_obj), 0)) {
                /* tied hash: STORE(obj, key, value) via nested call */
                int sfidx = vm_tied_array_find_method(vm, h->tied_obj, "STORE");
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
            if (__builtin_expect(VM_IS_PTR(key) && VM_PTR_TYPE(key) == VM_OBJ_STR, 1)) {
                VMString *kstr = VM_AS_STR(key);
                uint32_t kh = (kstr->hdr.gc_immortal == 2)
                              ? ((const VMConstString*)((const char*)kstr - offsetof(VMConstString, str)))->hash
                              : vm_hash_fn_n(kstr->data, kstr->len);
                vm_hash_set_nh(h, kstr->data, kstr->len, kh, v);
            } else {
                char buf[64]; size_t klen = 0;
                const char *ks = vm_bytes_view(key, buf, sizeof(buf), &klen);
                vm_hash_set_n(h, ks, klen, v);
            }
        }
        vm_val_free(&key); DISPATCH();
    }
    CASE(OP_HASH_DELETE): {
        VMValue key = SP_POP(), hash = SP_POP();
        VMValue deleted = VM_UNDEF_VAL;
        if (VM_IS_PTR(hash) && VM_PTR_TYPE(hash) == VM_OBJ_HASH) {
            VMHash *h = VM_AS_HASH(hash);
            if (__builtin_expect(!VM_IS_UNDEF(h->tied_obj), 0)) {
                int dfidx = vm_tied_array_find_method(vm, h->tied_obj, "DELETE");
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
                    SP_PUSH(r);
                    vm_temp_free(&key);
                    DISPATCH();
                }
            }
            char buf[64]; size_t klen = 0;
            const char *ks = vm_bytes_view(key, buf, sizeof(buf), &klen);
            deleted = vm_hash_delete_ret_n(h, ks, klen);
        }
        SP_PUSH(deleted);
        vm_val_free(&key); DISPATCH();
    }
    CASE(OP_HASH_KEYS): {
        VMValue hash = SP_POP();
        if (VM_IS_PTR(hash) && VM_PTR_TYPE(hash) == VM_OBJ_HASH) {
            VMHash *h = VM_AS_HASH(hash);
            if (__builtin_expect(!VM_IS_UNDEF(h->tied_obj), 0)) {
                int has_fk = 0;
                frame->ip = ip; vm->stack_top = sp;
                VMArray *tkeys = vm_tied_hash_collect_keys(vm, h->tied_obj, &has_fk);
                stack = vm->stack; RELOAD();
                VM_CHECK_EXC();
                if (has_fk) {
                    SP_PUSH(VM_MAKE_PTR(tkeys ? tkeys : vm_array_new(0)));
                    DISPATCH();
                }
            }
            SP_PUSH(VM_MAKE_PTR(vm_hash_keys(h)));
        } else {
            SP_PUSH(VM_MAKE_PTR(vm_array_new(0)));
        }
        DISPATCH();
    }
    CASE(OP_HASH_EXISTS): {
        VMValue key = SP_POP(), hash = SP_POP();
        int res = 0;
        if (VM_IS_PTR(hash) && VM_PTR_TYPE(hash) == VM_OBJ_HASH) {
            VMHash *h = VM_AS_HASH(hash);
            if (__builtin_expect(!VM_IS_UNDEF(h->tied_obj), 0)) {
                int efidx = vm_tied_array_find_method(vm, h->tied_obj, "EXISTS");
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
            char buf[64]; size_t klen = 0;
            const char *ks = vm_bytes_view(key, buf, sizeof(buf), &klen);
            res = vm_hash_exists_n(h, ks, klen);
        }
        SP_PUSH(VM_MAKE_INT(res));
        vm_val_free(&key); DISPATCH();
    }

    /* ===== Strings ===== */
    CASE(OP_STR_LEN): {
        VMValue v = SP_POP();
        int64_t len_val = 0;
        if (VM_IS_PTR(v) && (VM_PTR_TYPE(v) == VM_OBJ_STR || VM_PTR_TYPE(v) == VM_OBJ_STRBUF)) {
            size_t blen = (VM_PTR_TYPE(v) == VM_OBJ_STR) ? VM_AS_STR(v)->len : VM_AS_STRBUF(v)->len;
            const char *s = (VM_PTR_TYPE(v) == VM_OBJ_STR) ? VM_AS_STR(v)->data : VM_AS_STRBUF(v)->data;
            if (vm_str_is_utf8_chars(v, s, blen)) {
                len_val = (int64_t)vm_utf8_char_count(s, blen);
            } else {
                len_val = (int64_t)blen;
            }
        } else if (!VM_IS_UNDEF(v)) {
            char buf[64];
            size_t slen = 0;
            (void)vm_bytes_view(v, buf, sizeof(buf), &slen);
            len_val = (int64_t)slen;
        }
        SP_PUSH(VM_MAKE_INT(len_val));
        vm_val_free(&v); DISPATCH();
    }

    CASE(OP_STR_SPLIT): {
        VMValue str = SP_POP(), delim = SP_POP();
        char sbuf[256], pbuf[256];
        size_t slen = 0;
        const char *s = vm_bytes_view(str, sbuf, sizeof(sbuf), &slen);
        const char *pat = vm_to_cstr(delim, pbuf, sizeof(pbuf));
        strada_regex_set_subject_len(slen);
        strada_regex_set_subject_utf8(vm_is_utf8(str));
        StradaArray *parts = strada_regex_split(s, pat);
        strada_regex_clear_subject_len();
        VMArray *res = vm_split_parts_to_array(parts);
        vm_val_free(&str); vm_val_free(&delim);
        SP_PUSH(VM_MAKE_PTR(res));
        DISPATCH();
    }

    CASE(OP_STR_SPLIT_LIMIT): {
        VMValue str = SP_POP(), delim = SP_POP(), limit_v = SP_POP();
        int limit = (int)vm_to_int(limit_v);
        char sbuf[256], pbuf[256];
        size_t slen = 0;
        const char *s = vm_bytes_view(str, sbuf, sizeof(sbuf), &slen);
        const char *pat = vm_to_cstr(delim, pbuf, sizeof(pbuf));
        strada_regex_set_subject_len(slen);
        strada_regex_set_subject_utf8(vm_is_utf8(str));
        StradaArray *parts = (limit == 0) ? strada_regex_split(s, pat)
                                          : strada_regex_split_limit(s, pat, limit);
        strada_regex_clear_subject_len();
        VMArray *res = vm_split_parts_to_array(parts);
        vm_val_free(&str); vm_val_free(&delim);
        SP_PUSH(VM_MAKE_PTR(res));
        DISPATCH();
    }

    CASE(OP_STR_REPLACE): {
        VMValue repl = SP_POP(), pat = SP_POP(), str = SP_POP();
        char sbuf[256], pbuf[256], rbuf[256];
        size_t sl = 0, pl = 0, rl = 0;
        const char *s = vm_bytes_view(str, sbuf, sizeof(sbuf), &sl);
        const char *p = vm_bytes_view(pat, pbuf, sizeof(pbuf), &pl);
        const char *r = vm_bytes_view(repl, rbuf, sizeof(rbuf), &rl);
        int out_u8 = vm_is_utf8(str) || vm_is_utf8(repl);
        const char *f = vm_memmem(s, sl, p, pl);
        if (f) {
            size_t before = (size_t)(f - s), nl = sl - pl + rl;
            VMString *vs = malloc(sizeof(VMString) + nl + 1);
            vm_gc_init_hdr(&vs->hdr, VM_OBJ_STR);
            if (out_u8) vs->hdr.is_utf8 = 1;
            vs->len = (uint32_t)nl;
            if (before > 0) memcpy(vs->data, s, before);
            if (rl > 0) memcpy(vs->data + before, r, rl);
            if (sl - before - pl > 0) memcpy(vs->data + before + rl, f + pl, sl - before - pl);
            vs->data[nl] = 0;
            SP_PUSH(VM_MAKE_PTR(vs));
        } else {
            SP_PUSH(vm_make_str_flag(s, sl, vm_is_utf8(str)));
        }
        vm_val_free(&str); vm_val_free(&pat); vm_val_free(&repl);
        DISPATCH();
    }

    CASE(OP_SUBSTR): {
        VMValue len_v = SP_POP(), start_v = SP_POP(), str_v = SP_POP();
        char buf[64];
        size_t blen = 0;
        const char *s = vm_bytes_view(str_v, buf, sizeof(buf), &blen);
        int64_t start = vm_to_int(start_v);
        int64_t len = vm_to_int(len_v);
        if (vm_str_is_utf8_chars(str_v, s, blen)) {
            int64_t clen = (int64_t)vm_utf8_char_count(s, blen);
            if (start < 0) start = clen + start;
            if (start < 0) start = 0;
            if (start > clen) start = clen;
            if (len < 0) {
                len = clen - start + len;
                if (len < 0) len = 0;
            } else if (len > clen - start) {
                len = clen - start;
            }
            size_t b0 = vm_utf8_char_to_byte(s, blen, start);
            size_t b1 = vm_utf8_char_to_byte(s, blen, start + len);
            SP_PUSH(vm_make_str_flag(s + b0, b1 - b0, 1));
        } else {
            int64_t slen = (int64_t)blen;
            if (start < 0) start = slen + start;
            if (start < 0) start = 0;
            if (start > slen) start = slen;
            if (len < 0) {
                len = slen - start + len;
                if (len < 0) len = 0;
            } else if (len > slen - start) {
                len = slen - start;
            }
            SP_PUSH(vm_make_str_flag(s + start, (size_t)len, vm_is_utf8(str_v)));
        }
        vm_val_free(&str_v);
        DISPATCH();
    }

    CASE(OP_STR_INDEX): {
        VMValue needle = SP_POP(), haystack = SP_POP();
        char hbuf[256], nbuf[64];
        size_t hlen = 0, nlen = 0;
        const char *h = vm_bytes_view(haystack, hbuf, sizeof(hbuf), &hlen);
        const char *n = vm_bytes_view(needle, nbuf, sizeof(nbuf), &nlen);
        const char *f = vm_memmem(h, hlen, n, nlen);
        int64_t pos = -1;
        if (f) {
            size_t boff = (size_t)(f - h);
            pos = vm_str_is_utf8_chars(haystack, h, hlen) ? vm_utf8_byte_to_char(h, boff) : (int64_t)boff;
        }
        SP_PUSH(VM_MAKE_INT(pos));
        vm_val_free(&haystack); vm_val_free(&needle);
        DISPATCH();
    }

    CASE(OP_STR_UPPER): {
        VMValue v = SP_POP();
        VMValue r = vm_str_case_transform(v, 1);
        vm_temp_free(&v);
        SP_PUSH(r);
        DISPATCH();
    }

    CASE(OP_STR_LOWER): {
        VMValue v = SP_POP();
        VMValue r = vm_str_case_transform(v, 0);
        vm_temp_free(&v);
        SP_PUSH(r);
        DISPATCH();
    }

    CASE(OP_CHR): {
        VMValue v = SP_POP();
        int64_t code = vm_to_int(v);
        char c[5];
        size_t clen;
        int is_u8 = 0;
        if (code >= 0 && code < 0x80) {
            c[0] = (char)(unsigned char)code;
            clen = 1;
        } else if (code >= 0x80 && code < 0x800) {
            c[0] = (char)(0xC0 | (code >> 6));
            c[1] = (char)(0x80 | (code & 0x3F));
            clen = 2;
            is_u8 = 1;
        } else if (code >= 0x800 && code < 0x10000) {
            c[0] = (char)(0xE0 | (code >> 12));
            c[1] = (char)(0x80 | ((code >> 6) & 0x3F));
            c[2] = (char)(0x80 | (code & 0x3F));
            clen = 3;
            is_u8 = 1;
        } else if (code >= 0x10000 && code < 0x110000) {
            c[0] = (char)(0xF0 | (code >> 18));
            c[1] = (char)(0x80 | ((code >> 12) & 0x3F));
            c[2] = (char)(0x80 | ((code >> 6) & 0x3F));
            c[3] = (char)(0x80 | (code & 0x3F));
            clen = 4;
            is_u8 = 1;
        } else {
            c[0] = '?';
            clen = 1;
        }
        vm_temp_free(&v);
        SP_PUSH(vm_make_str_flag(c, clen, is_u8));
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
        size_t len = 0;
        const char *s = vm_bytes_view(v, buf, sizeof(buf), &len);
        if (len > 0) {
            if (s[len - 1] == '\n') {
                len--;
                if (len > 0 && s[len - 1] == '\r') len--;
            } else if (s[len - 1] == '\r') {
                len--;
            }
        }
        SP_PUSH(vm_make_str_flag(s, len, vm_is_utf8(v)));
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
        if (__builtin_expect(vm->has_pending_exc, 0)) {
            if (subj) strada_decref(subj);
            vm_temp_free(&str);
            VM_CHECK_EXC();
        }
        StradaValue *ret = strada_tr(subj, chunk->str_consts[from_idx],
                                     chunk->str_consts[to_idx], tr_flags);
        VMValue tr_ret = sv_to_vm(ret);
        if (ret) strada_decref(ret);
        if (store_kind != 0 && !strchr(tr_flags, 'r')) {
            VMValue updated = sv_to_vm(subj);
            VMValue tied_target = VM_UNDEF_VAL;
            if (store_kind == 1) {
                VMValue *loc = &locals[store_idx];
                if (VM_IS_PTR(*loc) && VM_PTR_TYPE(*loc) == VM_OBJ_CELL) loc = &VM_AS_CELL(*loc)->val;
                if (VM_IS_PTR(*loc) && VM_PTR_TYPE(*loc) == VM_OBJ_TIED_SCALAR) {
                    tied_target = *loc;
                } else {
                    vm_temp_free(loc);
                    *loc = updated;
                }
            } else if (store_kind == 2) {
                if (frame->closure && store_idx < (uint16_t)frame->closure->capture_count) {
                    VMValue cv = frame->closure->captures[store_idx];
                    if (VM_IS_PTR(cv) && VM_PTR_TYPE(cv) == VM_OBJ_CELL) {
                        VMValue *cval = &VM_AS_CELL(cv)->val;
                        if (VM_IS_PTR(*cval) && VM_PTR_TYPE(*cval) == VM_OBJ_TIED_SCALAR) {
                            tied_target = *cval;
                        } else {
                            vm_temp_free(cval);
                            *cval = updated;
                        }
                    } else if (VM_IS_PTR(cv) && VM_PTR_TYPE(cv) == VM_OBJ_TIED_SCALAR) {
                        tied_target = cv;
                    } else {
                        frame->closure->captures[store_idx] = updated;
                    }
                } else {
                    vm_temp_free(&updated);
                }
            } else if (store_kind == 3) {
                const char *gname = chunk->str_consts[store_idx];
                VMValue cur = vm_globals_get(vm, gname);
                if (VM_IS_PTR(cur) && VM_PTR_TYPE(cur) == VM_OBJ_TIED_SCALAR) {
                    tied_target = cur;
                } else {
                    vm_globals_set(vm, gname, updated);
                }
            }
            if (!VM_IS_UNDEF(tied_target)) {
                frame->ip = ip; vm->stack_top = sp;
                vm_tied_scalar_store(vm, tied_target, updated);
                stack = vm->stack; RELOAD();
            }
        }
        strada_decref(subj);
        vm_temp_free(&str);
        VM_CHECK_EXC();
        SP_PUSH(tr_ret);
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
            SP_PUSH(vm_make_str_flag("", 0, vm_is_utf8(str)));
        } else if ((uint64_t)n > (256ULL * 1024 * 1024) / slen) {
            fprintf(stderr, "VM error: string repeat overflow\n");
            SP_PUSH(vm_str(""));
        } else {
            size_t total = slen * (size_t)n;
            VMString *vs = malloc(sizeof(VMString) + total + 1);
            vm_gc_init_hdr(&vs->hdr, VM_OBJ_STR);
            if (vm_is_utf8(str)) vs->hdr.is_utf8 = 1;
            vs->len = (uint32_t)total;
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
        if (VM_IS_PTR(hash) && cname[0]) {
            switch (VM_PTR_TYPE(hash)) {
            case VM_OBJ_HASH: {
                VMHash *h = VM_AS_HASH(hash);
                free(h->class_name);
                h->class_name = strdup(cname);
                break;
            }
            case VM_OBJ_ARRAY: {
                VMArray *a = VM_AS_ARRAY(hash);
                free(a->class_name);
                a->class_name = strdup(cname);
                break;
            }
            case VM_OBJ_SREF: {
                VMScalarRef *sr = VM_AS_SREF(hash);
                free(sr->class_name);
                sr->class_name = strdup(cname);
                break;
            }
            case VM_OBJ_CLOSURE: {
                VMClosure *c = VM_AS_CLOSURE(hash);
                free(c->class_name);
                c->class_name = strdup(cname);
                break;
            }
            case VM_OBJ_NATIVE_SV: {
                VMNativeSV *nsv = (VMNativeSV*)VM_TO_PTR(hash);
                if (nsv->sv && !STRADA_IS_TAGGED_INT(nsv->sv)) strada_bless(nsv->sv, cname);
                break;
            }
            default: break;
            }
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
        if (__builtin_expect(vm->has_weakrefs && VM_IS_PTR(r) && VM_PTR_TYPE(r) == VM_OBJ_WEAKREF, 0)) {
            frame->ip = ip; vm->stack_top = sp;
            r = vm_unwrap_weak_gc(vm, r, NULL, 0);
            stack = vm->stack; RELOAD();
        }
        if (VM_IS_PTR(r) && VM_PTR_TYPE(r) == VM_OBJ_SREF) {
            VMScalarRef *sr = VM_AS_SREF(r);
            VMValue tv = VM_UNDEF_VAL;
            if (sr->global_name) {
                tv = vm_globals_get(vm, sr->global_name);
            } else if (VM_IS_PTR(sr->cell) && VM_PTR_TYPE(sr->cell) == VM_OBJ_CELL) {
                tv = VM_AS_CELL(sr->cell)->val;
            }
            if (__builtin_expect(VM_IS_PTR(tv) && VM_PTR_TYPE(tv) == VM_OBJ_TIED_SCALAR, 0)) {
                frame->ip = ip; vm->stack_top = sp;
                VMValue fv = vm_tied_scalar_fetch(vm, tv);
                stack = vm->stack; RELOAD();
                VM_CHECK_EXC();
                SP_PUSH(fv);
            } else {
                if (__builtin_expect(vm->has_weakrefs && VM_IS_PTR(tv) && VM_PTR_TYPE(tv) == VM_OBJ_WEAKREF, 0)) {
                    frame->ip = ip; vm->stack_top = sp;
                    tv = vm_unwrap_weak_gc(vm, tv, &r, 1);
                    stack = vm->stack; RELOAD();
                }
                SP_PUSH(vm_val_copy(tv));
            }
        } else if (VM_IS_PTR(r) && VM_PTR_TYPE(r) == VM_OBJ_CELL) {
            VMValue tv = VM_AS_CELL(r)->val;
            if (__builtin_expect(VM_IS_PTR(tv) && VM_PTR_TYPE(tv) == VM_OBJ_TIED_SCALAR, 0)) {
                frame->ip = ip; vm->stack_top = sp;
                VMValue fv = vm_tied_scalar_fetch(vm, tv);
                stack = vm->stack; RELOAD();
                VM_CHECK_EXC();
                SP_PUSH(fv);
            } else {
                if (__builtin_expect(vm->has_weakrefs && VM_IS_PTR(tv) && VM_PTR_TYPE(tv) == VM_OBJ_WEAKREF, 0)) {
                    frame->ip = ip; vm->stack_top = sp;
                    tv = vm_unwrap_weak_gc(vm, tv, &r, 1);
                    stack = vm->stack; RELOAD();
                }
                SP_PUSH(vm_val_copy(tv));
            }
        } else if (VM_IS_PTR(r) && VM_PTR_TYPE(r) == VM_OBJ_TIED_SCALAR) {
            frame->ip = ip; vm->stack_top = sp;
            VMValue fv = vm_tied_scalar_fetch(vm, r);
            stack = vm->stack; RELOAD();
            VM_CHECK_EXC();
            SP_PUSH(fv);
        } else if (VM_IS_PTR(r) && VM_PTR_TYPE(r) == VM_OBJ_NATIVE_SV) {
            VMNativeSV *nsv = (VMNativeSV *)VM_TO_PTR(r);
            if (nsv->sv && !STRADA_IS_TAGGED_INT(nsv->sv) && nsv->sv->type == STRADA_REF) {
                StradaValue *d = strada_deref(nsv->sv);
                VMValue res = sv_to_vm(d);
                strada_decref(d);
                SP_PUSH(res);
                vm_temp_free(&r);
            } else {
                SP_PUSH(r);
            }
        } else {
            SP_PUSH(r);
        }
        DISPATCH();
    }

    CASE(OP_STORE_SREF): {
        VMValue newv = SP_POP();
        VMValue r = SP_POP();
        if (__builtin_expect(vm->has_weakrefs, 0)) {
            if (VM_IS_PTR(r) && VM_PTR_TYPE(r) == VM_OBJ_WEAKREF) {
                frame->ip = ip; vm->stack_top = sp;
                r = vm_unwrap_weak_gc(vm, r, &newv, 1);
                stack = vm->stack; RELOAD();
            }
            if (VM_IS_PTR(newv) && VM_PTR_TYPE(newv) == VM_OBJ_WEAKREF) {
                frame->ip = ip; vm->stack_top = sp;
                newv = vm_unwrap_weak_gc(vm, newv, &r, 1);
                stack = vm->stack; RELOAD();
            }
        }
        if (VM_IS_PTR(r) && VM_PTR_TYPE(r) == VM_OBJ_SREF) {
            VMScalarRef *sr = VM_AS_SREF(r);
            if (sr->global_name) {
                VMValue tv = vm_globals_get(vm, sr->global_name);
                if (__builtin_expect(VM_IS_PTR(tv) && VM_PTR_TYPE(tv) == VM_OBJ_TIED_SCALAR, 0)) {
                    frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
                    vm_tied_scalar_store(vm, tv, newv);
                    vm->gc_pause--;
                    stack = vm->stack; RELOAD();
                    VM_CHECK_EXC();
                } else {
                    vm_globals_set(vm, sr->global_name, vm_val_copy(newv));
                }
            } else if (VM_IS_PTR(sr->cell) && VM_PTR_TYPE(sr->cell) == VM_OBJ_CELL) {
                VMValue old_cv = VM_AS_CELL(sr->cell)->val;
                if (__builtin_expect(VM_IS_PTR(old_cv) && VM_PTR_TYPE(old_cv) == VM_OBJ_TIED_SCALAR, 0)) {
                    frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
                    vm_tied_scalar_store(vm, old_cv, newv);
                    vm->gc_pause--;
                    stack = vm->stack; RELOAD();
                    VM_CHECK_EXC();
                } else {
                    vm_temp_free(&old_cv);
                    VM_AS_CELL(sr->cell)->val = vm_val_copy(newv);
                }
            }
        } else if (VM_IS_PTR(r) && VM_PTR_TYPE(r) == VM_OBJ_CELL) {
            VMValue old_cv = VM_AS_CELL(r)->val;
            if (__builtin_expect(VM_IS_PTR(old_cv) && VM_PTR_TYPE(old_cv) == VM_OBJ_TIED_SCALAR, 0)) {
                frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
                vm_tied_scalar_store(vm, old_cv, newv);
                vm->gc_pause--;
                stack = vm->stack; RELOAD();
                VM_CHECK_EXC();
            } else {
                vm_temp_free(&old_cv);
                VM_AS_CELL(r)->val = vm_val_copy(newv);
            }
        } else if (VM_IS_PTR(r) && VM_PTR_TYPE(r) == VM_OBJ_NATIVE_SV) {
            VMNativeSV *nsv = (VMNativeSV *)VM_TO_PTR(r);
            if (nsv->sv) {
                StradaValue *nv = vm_to_sv(newv);
                strada_deref_set(nsv->sv, nv);
                strada_decref(nv);
            }
        }
        vm_temp_free(&r);
        SP_PUSH(newv);
        DISPATCH();
    }

    CASE(OP_ARG_PRESENT): {
        uint16_t pidx = read_u16(ip); ip += 2;
        SP_PUSH(VM_MAKE_INT((int)pidx < frame->passed_argc ? 1 : 0));
        DISPATCH();
    }

    CASE(OP_SQ_COERCE): {
        uint8_t side = *ip++;
        uint16_t idx = read_u16(ip); ip += 2;
        if (sp >= 2 && idx < chunk->str_const_count) {
            VMValue peer = (side == 0) ? stack[sp - 1] : stack[sp - 2];
            int coerce = 0;
            if (VM_IS_FLOAT(peer)) {
                coerce = 1;
            } else if (VM_IS_PTR(peer)) {
                enum VMObjType pt = VM_PTR_TYPE(peer);
                if (pt == VM_OBJ_STR || pt == VM_OBJ_STRBUF) {
                    coerce = 1;
                } else if (pt == VM_OBJ_NATIVE_SV) {
                    VMNativeSV *nsv = (VMNativeSV*)VM_TO_PTR(peer);
                    if (nsv->sv && !STRADA_IS_TAGGED_INT(nsv->sv) &&
                        (nsv->sv->type == STRADA_NUM || nsv->sv->type == STRADA_STR)) {
                        coerce = 1;
                    }
                }
            }
            if (coerce) {
                size_t slot = (side == 0) ? (sp - 2) : (sp - 1);
                vm_temp_free(&stack[slot]);
                size_t slen = chunk->str_const_lens ? chunk->str_const_lens[idx] : strlen(chunk->str_consts[idx]);
                stack[slot] = VM_MAKE_STR_N(chunk->str_consts[idx], slen);
            }
        }
        DISPATCH();
    }

    CASE(OP_IS_UNDEF): { VMValue v = SP_POP(); SP_PUSH(VM_MAKE_INT(VM_IS_UNDEF(v))); vm_temp_free(&v); DISPATCH(); }

    CASE(OP_ISA): {
        VMValue tc = SP_POP(), obj = SP_POP();
        int res = 0;
        char tbuf[128];
        const char *tcname = vm_to_cstr(tc, tbuf, sizeof(tbuf));
        if (tcname[0] && VM_IS_PTR(obj)) {
            const char *cls = vm_obj_class_name(obj);
            if (cls && cls[0]) {
                res = vm_class_isa(vm->program, cls, tcname, 0);
            } else if (VM_PTR_TYPE(obj) == VM_OBJ_STR && VM_AS_STR(obj)->data[0]) {
                res = vm_class_isa(vm->program, VM_AS_STR(obj)->data, tcname, 0);
            } else if (VM_PTR_TYPE(obj) == VM_OBJ_STRBUF && VM_AS_STRBUF(obj)->data && VM_AS_STRBUF(obj)->data[0]) {
                res = vm_class_isa(vm->program, VM_AS_STRBUF(obj)->data, tcname, 0);
            }
            if (!res && VM_PTR_TYPE(obj) == VM_OBJ_NATIVE_SV) {
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
            const char *cls = vm_obj_class_name(obj);
            if (!cls || !cls[0]) {
                if (VM_PTR_TYPE(obj) == VM_OBJ_STR && VM_AS_STR(obj)->data[0]) {
                    cls = VM_AS_STR(obj)->data;
                } else if (VM_PTR_TYPE(obj) == VM_OBJ_STRBUF && VM_AS_STRBUF(obj)->data && VM_AS_STRBUF(obj)->data[0]) {
                    cls = VM_AS_STRBUF(obj)->data;
                }
            }
            if (cls && cls[0]) {
                if (strcmp(mname, "isa") == 0 || strcmp(mname, "can") == 0 ||
                    vm_find_method(vm->program, cls, mname) >= 0) {
                    res = 1;
                }
            }
            if (!res && VM_PTR_TYPE(obj) == VM_OBJ_NATIVE_SV) {
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
        mcall_allow_field = 0;
        goto L_do_op_method_call;
    }

    CASE(OP_FIELD_ACCESS): {
        mcall_idx = read_u16(ip); ip += 2;
        mcall_argc = *ip++;
        mcall_method_cstr = chunk->str_consts[mcall_idx];
        mcall_is_dyn = 0;
        mcall_allow_field = 1;
        goto L_do_op_method_call;
    }

    CASE(OP_METHOD_CALL_SPREAD): {
        mcall_idx = read_u16(ip); ip += 2;
        VMValue arr_val = SP_POP();
        VMValue sbuf[16];
        VMValue *sitems = NULL;
        int cnt = 0;
        uint8_t mcall_ctx = vm->pending_call_context;
        vm->pending_call_context = 0;
        frame->ip = ip; vm->stack_top = sp;
        int exc = vm_collect_spread_items(vm, arr_val, sbuf, 16, &sitems, &cnt);
        stack = vm->stack; RELOAD();
        if (exc) { VM_CHECK_EXC(); }
        vm->pending_call_context = mcall_ctx;
        mcall_argc = cnt;
        for (int i = 0; i < cnt; i++) SP_PUSH(sitems[i]);
        if (sitems && sitems != sbuf) free(sitems);
        mcall_method_cstr = chunk->str_consts[mcall_idx];
        mcall_is_dyn = 0;
        mcall_allow_field = 0;
        goto L_do_op_method_call;
    }

    L_do_op_method_call: {
        int argc = mcall_argc;
        const char *method = mcall_method_cstr;
        VMValue self = stack[sp - argc - 1];
        uint8_t mcall_ctx = vm->pending_call_context;
        vm->pending_call_context = 0;

        /* Handle native OOP objects (VMNativeSV) — use strada_method_call for proper dispatch */
        if (VM_IS_PTR(self) && VM_PTR_TYPE(self) == VM_OBJ_NATIVE_SV) {
            VMNativeSV *nsv = (VMNativeSV*)VM_TO_PTR(self);
            StradaValue *self_sv = nsv->sv;
            VMValue raw_buf[16];
            VMValue *raw_args = (argc > 16) ? malloc((size_t)argc * sizeof(VMValue)) : raw_buf;
            for (int ai = argc - 1; ai >= 0; ai--) raw_args[ai] = stack[--sp];
            --sp; /* pop self */

            StradaValue *packed_args = strada_new_array();
            for (int ai = 0; ai < argc; ai++) {
                StradaValue *sv = vm_to_sv(raw_args[ai]);
                strada_array_push_take(packed_args->value.av, sv);
            }
            StradaValue *nr = NULL;
            frame->ip = ip; vm->stack_top = sp;
            int prev_c_ctx = strada_wantarray() ? 1 : (strada_wanthash() ? 2 : 0);
            strada_set_call_context((int)mcall_ctx);
            int c_threw = vm_c_call_method(vm, self_sv, method, packed_args, &nr, &throw_exc);
            strada_set_call_context(prev_c_ctx);
            if (c_threw) {
                strada_decref(packed_args);
                if (raw_args != raw_buf) free(raw_args);
                goto L_vm_throw_exc;
            }
            stack = vm->stack;
            RELOAD();
            for (int ai = 0; ai < argc; ai++) {
                vm_fh_sync_from_sv(raw_args[ai]);
            }
            strada_decref(packed_args);
            if (raw_args != raw_buf) free(raw_args);
            SP_PUSH(nr ? sv_to_vm(nr) : VM_UNDEF_VAL);
            if (nr) strada_decref(nr);
            DISPATCH();
        }

        VMHash *h = (VM_IS_PTR(self) && VM_PTR_TYPE(self) == VM_OBJ_HASH) ? VM_AS_HASH(self) : NULL;
        const char *obj_cls = vm_obj_class_name(self);
        const char *cls_name = obj_cls;
        int is_hash_blessed = (obj_cls != NULL);
        if (!cls_name) {
            if (VM_IS_PTR(self) && VM_PTR_TYPE(self) == VM_OBJ_STR) {
                cls_name = VM_AS_STR(self)->data;
            } else if (VM_IS_PTR(self) && VM_PTR_TYPE(self) == VM_OBJ_STRBUF) {
                cls_name = VM_AS_STRBUF(self)->data;
            }
        }

        int fidx = -1;
        if (__builtin_expect(!mcall_is_dyn && is_hash_blessed, 1)) {
            uint32_t ic_slot = (uint32_t)(((uintptr_t)ip >> 2) & 255);
            if (__builtin_expect(vm->method_ic[ic_slot].ip == ip && vm->method_ic[ic_slot].class_name &&
                strcmp(vm->method_ic[ic_slot].class_name, cls_name) == 0, 1)) {
                fidx = vm->method_ic[ic_slot].func_idx;
                goto L_mcall_have_fidx;
            }
        }

        /* Built-in UNIVERSAL isa/can for dynamic method calls (static calls lower to OP_ISA/OP_CAN) */
        if (__builtin_expect(strcmp(method, "isa") == 0 || strcmp(method, "can") == 0, 0)) {
            int is_isa = (method[0] == 'i');
            VMValue arg0 = VM_UNDEF_VAL;
            for (int ai = argc - 1; ai >= 0; ai--) {
                VMValue v = SP_POP();
                if (ai == 0) arg0 = v;
                else vm_val_free(&v);
            }
            VMValue self_v = SP_POP();
            int res = 0;
            if (argc >= 1) {
                char buf[128];
                const char *arg_s = vm_to_cstr(arg0, buf, sizeof(buf));
                if (cls_name && cls_name[0] && arg_s[0]) {
                    if (is_isa) {
                        res = vm_class_isa(vm->program, cls_name, arg_s, 0);
                    } else {
                        res = (strcmp(arg_s, "isa") == 0 || strcmp(arg_s, "can") == 0 ||
                               vm_find_method(vm->program, cls_name, arg_s) >= 0) ? 1 : 0;
                    }
                }
                vm_val_free(&arg0);
            }
            vm_temp_free(&self_v);
            SP_PUSH(VM_MAKE_INT(res));
            DISPATCH();
        }

        if (cls_name && cls_name[0]) {
            if (is_hash_blessed && !mcall_is_dyn) {
                fidx = vm_find_method_cached(vm, cls_name, method);
                if (fidx >= 0) {
                    uint32_t ic_slot = (uint32_t)(((uintptr_t)ip >> 2) & 255);
                    free(vm->method_ic[ic_slot].class_name);
                    vm->method_ic[ic_slot].ip = ip;
                    vm->method_ic[ic_slot].class_name = strdup(cls_name);
                    vm->method_ic[ic_slot].func_idx = fidx;
                }
            } else {
                fidx = vm_find_method(vm->program, cls_name, method);
            }

        L_mcall_have_fidx:
            if (fidx >= 0) {
                if (vm->recursion_limit > 0 && (int)vm->frame_count >= vm->recursion_limit) {
                    fprintf(stderr, "Deep recursion detected (limit %d) — see core::set_recursion_limit\n", vm->recursion_limit);
                    exit(1);
                }
                /* Normal method call */
                VMChunk *callee = &vm->program->funcs[fidx];
                frame->ip = ip; vm->stack_top = sp;
                if (vm->frame_count >= vm->frame_cap) {
                    vm_frames_grow(vm);
                }
                VMFrame *nf = &vm->frames[vm->frame_count++];
                nf->chunk = callee; nf->ip = callee->code;
                nf->stack_base = sp - argc - 1;
                nf->closure = NULL;
                nf->global_save_depth = vm->global_save_count;
                nf->passed_argc = argc + 1;
                nf->call_context = mcall_ctx;
                int lcnt = callee->local_count + 1;
                if (__builtin_expect(callee->has_variadic, 0)) {
                    nf->locals = vm_locals_alloc(vm, lcnt, 0);
                    /* No-param method: pack self + args into @_ array */
                    int total_args = argc + 1; /* self + args */
                    int fixed = callee->fixed_param_count;
                    int vc = total_args - fixed;
                    VMArray *va = vm_array_new(vc > 0 ? vc : 4);
                    VMValue *tmp = malloc(total_args * sizeof(VMValue));
                    for (int i = argc - 1; i >= 0; i--) tmp[i + 1] = stack[--sp];
                    tmp[0] = stack[--sp]; /* self */
                    for (int i = 0; i < fixed && i < total_args; i++) {
                        if (i < lcnt) nf->locals[i] = tmp[i];
                    }
                    for (int i = fixed; i < total_args; i++) vm_array_push(va, tmp[i]);
                    if (fixed < lcnt) nf->locals[fixed] = VM_MAKE_PTR(va);
                    free(tmp);
                } else {
                    int pmax = lcnt;
                    if (callee->fixed_param_count >= 0 && callee->fixed_param_count < pmax) {
                        pmax = callee->fixed_param_count;
                    }
                    int zf = (argc + 1 < pmax) ? (argc + 1) : pmax;
                    VMValue *nloc = vm_locals_alloc(vm, lcnt, zf);
                    nf->locals = nloc;
                    for (int i = argc - 1; i >= 0; i--) {
                        VMValue av = stack[--sp];
                        if (i + 1 < pmax) nloc[i + 1] = av;
                    }
                    VMValue sv = stack[--sp]; /* self */
                    if (pmax > 0) nloc[0] = sv;
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
                        frame->ip = ip; vm->stack_top = sp;
                        VMValue ret = vm_invoke_native_entry(vm, ne, raw_args, total_argc, h);
                        stack = vm->stack;
                        RELOAD();
                        if (raw_args != raw_buf) free(raw_args);
                        if (__builtin_expect(vm->has_pending_exc, 0)) {
                            throw_exc = vm->pending_exc;
                            vm->pending_exc = VM_UNDEF_VAL;
                            vm->has_pending_exc = 0;
                            goto L_vm_throw_exc;
                        }
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
                        vm_frames_grow(vm);
                    }
                    VMFrame *nf = &vm->frames[vm->frame_count++];
                    nf->chunk = callee; nf->ip = callee->code;
                    nf->locals = locals_alloc(callee->local_count + 1, callee->int_only);
                    nf->stack_base = sp - argc - 1;
                    nf->closure = NULL;
                    nf->global_save_depth = vm->global_save_count;
                    nf->passed_argc = argc + 2;
                    nf->call_context = mcall_ctx;
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

            (void)is_hash_blessed;
            if (mcall_allow_field && h && vm_hash_exists(h, method)) {
                VMValue fv = vm_hash_get(h, method);
                sp -= (argc + 1);
                vm_temp_free(&self);
                SP_PUSH(fv);
                DISPATCH();
            }
            sp -= (argc + 1);
            vm->stack_top = sp;
            char errbuf[600];
            snprintf(errbuf, sizeof(errbuf), "Can't locate method \"%s\" via package \"%s\" or its parents", method, cls_name);
            throw_exc = vm_str(errbuf);
            goto L_vm_throw_exc;
        }

        /* Unblessed / non-package target */
        if (mcall_allow_field && VM_IS_PTR(self) && VM_PTR_TYPE(self) == VM_OBJ_HASH) {
            VMHash *uh = VM_AS_HASH(self);
            if (vm_hash_exists(uh, method)) {
                VMValue fv = vm_hash_get(uh, method);
                sp -= (argc + 1);
                vm_temp_free(&self);
                SP_PUSH(fv);
                DISPATCH();
            }
        }
        sp -= (argc + 1);
        vm->stack_top = sp;
        char errbuf[600];
        snprintf(errbuf, sizeof(errbuf), "Can't call method \"%s\" on unblessed reference", method);
        throw_exc = vm_str(errbuf);
        goto L_vm_throw_exc;
    }

    CASE(OP_SUPER_CALL): {
        uint16_t pkg_idx = read_u16(ip); ip += 2;
        uint16_t method_idx = read_u16(ip); ip += 2;
        uint8_t raw_argc = *ip++;
        const char *def_pkg = chunk->str_consts[pkg_idx];
        const char *method = chunk->str_consts[method_idx];
        uint8_t super_ctx = vm->pending_call_context;
        vm->pending_call_context = 0;

        VMValue call_buf[18];
        VMValue *call_args = call_buf;
        int total_args = 0; /* includes self at [0] */

        if (raw_argc == 255) {
            VMValue spread_val = SP_POP();
            VMValue self = SP_POP();
            VMArray *sa = (VM_IS_PTR(spread_val) && VM_PTR_TYPE(spread_val) == VM_OBJ_ARRAY)
                          ? VM_AS_ARRAY(spread_val) : NULL;
            if (sa && !VM_IS_UNDEF(sa->tied_obj)) {
                frame->ip = ip; vm->stack_top = sp;
                vm->gc_pause++;
                int64_t tsz = vm_tied_array_fetchsize(vm, sa->tied_obj);
                if (vm->has_pending_exc) { vm->gc_pause--; RELOAD(); VM_CHECK_EXC(); }
                int arr_sz = (tsz > 0) ? (int)tsz : 0;
                total_args = 1 + arr_sz;
                if (total_args > 18) call_args = malloc((size_t)total_args * sizeof(VMValue));
                call_args[0] = self;
                int ffidx = vm_tied_array_find_method(vm, sa->tied_obj, "FETCH");
                for (int i = 0; i < arr_sz; i++) {
                    VMValue iv = VM_MAKE_INT(i);
                    call_args[1 + i] = (ffidx >= 0) ? vm_tied_array_call(vm, sa->tied_obj, ffidx, &iv, 1) : VM_UNDEF_VAL;
                    if (vm->has_pending_exc) {
                        vm->gc_pause--;
                        if (call_args != call_buf) free(call_args);
                        RELOAD();
                        VM_CHECK_EXC();
                    }
                }
                vm->gc_pause--;
                stack = vm->stack; RELOAD();
            } else {
                if (__builtin_expect(vm->has_weakrefs && sa != NULL, 0)) {
                    frame->ip = ip; vm->stack_top = sp;
                    vm_unwrap_container_weak_gc(vm, spread_val, &self, 1);
                    stack = vm->stack; RELOAD();
                }
                int arr_sz = sa ? sa->size : 0;
                total_args = 1 + arr_sz;
                if (total_args > 18) call_args = malloc((size_t)total_args * sizeof(VMValue));
                call_args[0] = self;
                for (int i = 0; i < arr_sz; i++) call_args[1 + i] = vm_val_copy(sa->items[i]);
            }
        } else {
            total_args = 1 + (int)raw_argc;
            if (total_args > 18) call_args = malloc((size_t)total_args * sizeof(VMValue));
            for (int i = (int)raw_argc - 1; i >= 0; i--) call_args[1 + i] = SP_POP();
            call_args[0] = SP_POP();
        }

        VMValue self = call_args[0];
        if (!def_pkg || !def_pkg[0]) {
            const char *sc = vm_obj_class_name(self);
            if (sc) def_pkg = sc;
        }

        int fidx = vm_find_super_method(vm->program, def_pkg, method, 0);
        if (fidx >= 0) {
            if (vm->recursion_limit > 0 && (int)vm->frame_count >= vm->recursion_limit) {
                if (call_args != call_buf) free(call_args);
                fprintf(stderr, "Deep recursion detected (limit %d) — see core::set_recursion_limit\n", vm->recursion_limit);
                exit(1);
            }
            frame->ip = ip; vm->stack_top = sp;
            vm->pending_call_context = super_ctx;
            VMValue ret = vm_invoke_func(vm, fidx, call_args, total_args);
            stack = vm->stack; RELOAD();
            if (call_args != call_buf) free(call_args);
            VM_CHECK_EXC();
            SP_PUSH(ret);
            DISPATCH();
        }

        /* Fallback to ancestor AUTOLOAD(self, method_name, ...args) */
        int al_fidx = vm_find_super_method(vm->program, def_pkg, "AUTOLOAD", 0);
        if (al_fidx >= 0) {
            if (vm->recursion_limit > 0 && (int)vm->frame_count >= vm->recursion_limit) {
                if (call_args != call_buf) free(call_args);
                fprintf(stderr, "Deep recursion detected (limit %d) — see core::set_recursion_limit\n", vm->recursion_limit);
                exit(1);
            }
            int al_total = total_args + 1;
            VMValue al_buf[19];
            VMValue *al_args = (al_total > 19) ? malloc((size_t)al_total * sizeof(VMValue)) : al_buf;
            al_args[0] = call_args[0];
            al_args[1] = vm_str(method);
            for (int i = 1; i < total_args; i++) al_args[i + 1] = call_args[i];
            if (call_args != call_buf) free(call_args);
            frame->ip = ip; vm->stack_top = sp;
            vm->pending_call_context = super_ctx;
            VMValue ret = vm_invoke_func(vm, al_fidx, al_args, al_total);
            stack = vm->stack; RELOAD();
            if (al_args != al_buf) free(al_args);
            VM_CHECK_EXC();
            SP_PUSH(ret);
            DISPATCH();
        }

        /* No parent method found — throw catchable exception */
        if (call_args != call_buf) free(call_args);
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
        mcall_allow_field = 0;
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
        if (__builtin_expect(VM_IS_PTR(*loc) && VM_PTR_TYPE(*loc) == VM_OBJ_TIED_SCALAR, 0)) {
            VMValue ts = *loc;
            frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
            VMValue lhs = vm_tied_scalar_fetch(vm, ts);
            if (!vm->has_pending_exc && __builtin_expect(vm_has_stringify(vm->program, lhs), 0)) {
                lhs = vm_stringify_ov(vm, lhs);
            }
            if (!vm->has_pending_exc) {
                char lbuf[64]; size_t el = 0;
                const char *ex = VM_IS_UNDEF(lhs) ? "" : vm_bytes_view(lhs, lbuf, sizeof(lbuf), &el);
                as = vm_bytes_view(rhs, buf, sizeof(buf), &al);
                VMString *vs = malloc(sizeof(VMString) + el + al + 1);
                vm_gc_init_hdr(&vs->hdr, VM_OBJ_STR);
                if (vm_is_utf8(lhs) || vm_is_utf8(rhs)) vs->hdr.is_utf8 = 1;
                vs->len = (uint32_t)(el + al);
                if (el > 0) memcpy(vs->data, ex, el);
                if (al > 0) memcpy(vs->data + el, as, al);
                vs->data[el + al] = '\0';
                vm_tied_scalar_store(vm, ts, VM_MAKE_PTR(vs));
            }
            vm->gc_pause--;
            stack = vm->stack; RELOAD();
            VM_CHECK_EXC();
            vm_val_free(&rhs);
            DISPATCH();
        }
        if (VM_IS_PTR(*loc) && VM_PTR_TYPE(*loc) == VM_OBJ_STRBUF && !VM_AS_STRBUF(*loc)->shared) {
            VMStrBuf *sb = VM_AS_STRBUF(*loc);
            size_t aoff = (as >= sb->data && as < sb->data + sb->cap) ? (size_t)(as - sb->data) : (size_t)-1;
            if (sb->len + al >= sb->cap) {
                sb->cap = (sb->len + al) * 2 + 64;
                char *nd = realloc(sb->data, sb->cap);
                if (!nd) { fprintf(stderr, "VM: out of memory\n"); exit(1); }
                sb->data = nd;
                if (aoff != (size_t)-1) as = sb->data + aoff;
            }
            if (al > 0) memcpy(sb->data + sb->len, as, al);
            sb->len += al; sb->data[sb->len] = 0;
            if (VM_IS_PTR(rhs) && ((VMObjHeader*)VM_TO_PTR(rhs))->is_utf8) sb->hdr.is_utf8 = 1;
        } else {
            VMValue lhs = *loc;
            if (__builtin_expect(vm_has_stringify(vm->program, lhs), 0)) {
                VMValue rcopy = VM_MAKE_STR_N(as, al);
                if (VM_IS_PTR(rhs)) VM_AS_STR(rcopy)->hdr.is_utf8 = ((VMObjHeader*)VM_TO_PTR(rhs))->is_utf8;
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
            sb->shared = 0;
            if ((VM_IS_PTR(lhs) && ((VMObjHeader*)VM_TO_PTR(lhs))->is_utf8) ||
                (VM_IS_PTR(rhs) && ((VMObjHeader*)VM_TO_PTR(rhs))->is_utf8)) {
                sb->hdr.is_utf8 = 1;
            }
            sb->cap = (el + al) * 2 + 64; sb->data = malloc(sb->cap);
            if (el > 0) memcpy(sb->data, ex, el);
            if (al > 0) memcpy(sb->data + el, as, al);
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
        size_t al = chunk->str_const_lens ? chunk->str_const_lens[str_idx] : strlen(as);
        VMValue *loc = &locals[slot];
        if (VM_IS_PTR(*loc) && VM_PTR_TYPE(*loc) == VM_OBJ_CELL) loc = &VM_AS_CELL(*loc)->val;
        if (__builtin_expect(VM_IS_PTR(*loc) && VM_PTR_TYPE(*loc) == VM_OBJ_TIED_SCALAR, 0)) {
            VMValue ts = *loc;
            frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
            VMValue lhs = vm_tied_scalar_fetch(vm, ts);
            if (!vm->has_pending_exc && __builtin_expect(vm_has_stringify(vm->program, lhs), 0)) {
                lhs = vm_stringify_ov(vm, lhs);
            }
            if (!vm->has_pending_exc) {
                as = chunk->str_consts[str_idx];
                al = chunk->str_const_lens ? chunk->str_const_lens[str_idx] : strlen(as);
                char lbuf[64]; size_t el = 0;
                const char *ex = VM_IS_UNDEF(lhs) ? "" : vm_bytes_view(lhs, lbuf, sizeof(lbuf), &el);
                VMString *vs = malloc(sizeof(VMString) + el + al + 1);
                vm_gc_init_hdr(&vs->hdr, VM_OBJ_STR);
                if (vm_is_utf8(lhs)) vs->hdr.is_utf8 = 1;
                vs->len = (uint32_t)(el + al);
                if (el > 0) memcpy(vs->data, ex, el);
                if (al > 0) memcpy(vs->data + el, as, al);
                vs->data[el + al] = '\0';
                vm_tied_scalar_store(vm, ts, VM_MAKE_PTR(vs));
            }
            vm->gc_pause--;
            stack = vm->stack; RELOAD();
            VM_CHECK_EXC();
            DISPATCH();
        }
        if (VM_IS_PTR(*loc) && VM_PTR_TYPE(*loc) == VM_OBJ_STRBUF && !VM_AS_STRBUF(*loc)->shared) {
            VMStrBuf *sb = VM_AS_STRBUF(*loc);
            if (sb->len + al >= sb->cap) {
                sb->cap = (sb->len + al) * 2 + 64;
                char *nd = realloc(sb->data, sb->cap);
                if (!nd) { fprintf(stderr, "VM: out of memory\n"); exit(1); }
                sb->data = nd;
            }
            if (al > 0) memcpy(sb->data + sb->len, as, al);
            sb->len += al; sb->data[sb->len] = 0;
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
                al = chunk->str_const_lens ? chunk->str_const_lens[str_idx] : strlen(as);
                loc = &locals[slot];
                if (VM_IS_PTR(*loc) && VM_PTR_TYPE(*loc) == VM_OBJ_CELL) loc = &VM_AS_CELL(*loc)->val;
            }
            const char *ex = ""; size_t el = 0; char lbuf[64];
            if (!VM_IS_UNDEF(lhs)) {
                ex = vm_bytes_view(lhs, lbuf, sizeof(lbuf), &el);
            }
            VMStrBuf *sb = malloc(sizeof(VMStrBuf));
            vm_gc_init_hdr(&sb->hdr, VM_OBJ_STRBUF);
            sb->shared = 0;
            if (VM_IS_PTR(lhs) && ((VMObjHeader*)VM_TO_PTR(lhs))->is_utf8) {
                sb->hdr.is_utf8 = 1;
            }
            sb->cap = (el + al) * 2 + 64; sb->data = malloc(sb->cap);
            if (el > 0) memcpy(sb->data, ex, el);
            if (al > 0) memcpy(sb->data + el, as, al);
            sb->len = el + al; sb->data[sb->len] = 0;
            vm_val_free(loc); *loc = VM_MAKE_PTR(sb);
        }
        DISPATCH();
    }

    /* Int/string concat key building supporting INT64_MIN, >128B keys, and stringify overloads */
    #define CONCAT_KEY_FORMAT_INT(pfx_ptr, plen, suffix_val, key_buf, kptr, heap_key, klen) do { \
        int64_t _n = VM_ANY_INT_VAL(suffix_val); \
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
        if (!VM_IS_ANY_INT(suffix) && __builtin_expect(vm_has_stringify(vm->program, suffix), 0)) {
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
        if (VM_IS_ANY_INT(suffix)) {
            CONCAT_KEY_FORMAT_INT(pfx, plen, suffix, key_buf, kptr, heap_key, klen);
        } else {
            CONCAT_KEY_FORMAT_BYTES(pfx, plen, suffix, key_buf, kptr, heap_key, klen);
        }
        VMHash *h = VM_AS_HASH(hash);
        if (__builtin_expect(!VM_IS_UNDEF(h->tied_obj), 0)) {
            int ffidx = vm_tied_array_find_method(vm, h->tied_obj, "FETCH");
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
        VMValue *slot = vm->has_weakrefs ? vm_hash_slot_ptr_n(h, kptr, klen) : NULL;
        if (slot && VM_IS_PTR(*slot) && VM_PTR_TYPE(*slot) == VM_OBJ_WEAKREF) {
            VMValue raw = *slot;
            free(heap_key);
            vm_temp_free(&suffix);
            frame->ip = ip; vm->stack_top = sp;
            VMValue r = vm_unwrap_weak_gc(vm, raw, &hash, 1);
            stack = vm->stack; RELOAD();
            SP_PUSH(vm_val_copy(r));
        } else {
            VMValue r = vm_hash_get_n(h, kptr, klen);
            SP_PUSH(vm_val_copy(r));
            free(heap_key);
            vm_temp_free(&suffix);
        }
        DISPATCH();
    }

    CASE(OP_HASH_SET_CONCAT): {
        uint16_t pfx_idx = read_u16(ip); ip += 2;
        VMValue val = SP_POP(), suffix = SP_POP(), hash = SP_POP();
        if (!VM_IS_PTR(hash) || VM_PTR_TYPE(hash) != VM_OBJ_HASH) {
            vm_temp_free(&val); vm_temp_free(&suffix);
            DISPATCH();
        }
        if (!VM_IS_ANY_INT(suffix) && __builtin_expect(vm_has_stringify(vm->program, suffix), 0)) {
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
        if (VM_IS_ANY_INT(suffix)) {
            CONCAT_KEY_FORMAT_INT(pfx, plen, suffix, key_buf, kptr, heap_key, klen);
        } else {
            CONCAT_KEY_FORMAT_BYTES(pfx, plen, suffix, key_buf, kptr, heap_key, klen);
        }
        VMHash *h = VM_AS_HASH(hash);
        if (__builtin_expect(!VM_IS_UNDEF(h->tied_obj), 0)) {
            int sfidx = vm_tied_array_find_method(vm, h->tied_obj, "STORE");
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
        if (!VM_IS_ANY_INT(suffix) && __builtin_expect(vm_has_stringify(vm->program, suffix), 0)) {
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
        if (VM_IS_ANY_INT(suffix)) {
            CONCAT_KEY_FORMAT_INT(pfx, plen, suffix, key_buf, kptr, heap_key, klen);
        } else {
            CONCAT_KEY_FORMAT_BYTES(pfx, plen, suffix, key_buf, kptr, heap_key, klen);
        }
        VMHash *h = VM_AS_HASH(hash);
        if (__builtin_expect(!VM_IS_UNDEF(h->tied_obj), 0)) {
            int dfidx = vm_tied_array_find_method(vm, h->tied_obj, "DELETE");
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
        vm_hash_delete_n(h, kptr, klen);
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
                } else if (outer_slots[i] != 0xFFFF) {
                    uint16_t os = outer_slots[i];
                    vm_temp_free(&raw);
                    if (os < (uint16_t)chunk->local_count &&
                        VM_IS_PTR(locals[os]) && VM_PTR_TYPE(locals[os]) == VM_OBJ_CELL) {
                        cl->captures[i] = locals[os];
                    } else {
                        /* Box locals[os] directly to preserve VM_OBJ_WEAKREF / VM_OBJ_TIED_SCALAR */
                        VMCell *cell = calloc(1, sizeof(VMCell));
                        vm_gc_init_hdr(&cell->hdr, VM_OBJ_CELL);
                        cell->val = (os < (uint16_t)chunk->local_count) ? locals[os] : VM_UNDEF_VAL;
                        VMValue cell_val = VM_MAKE_PTR(cell);
                        cl->captures[i] = cell_val;
                        if (os < (uint16_t)chunk->local_count) {
                            locals[os] = cell_val;
                        }
                    }
                } else {
                    /* Create a new cell for capture-by-reference */
                    VMCell *cell = calloc(1, sizeof(VMCell));
                    vm_gc_init_hdr(&cell->hdr, VM_OBJ_CELL);
                    cell->val = raw;
                    cl->captures[i] = VM_MAKE_PTR(cell);
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
                if (__builtin_expect(VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_TIED_SCALAR, 0)) {
                    frame->ip = ip; vm->stack_top = sp;
                    VMValue fv = vm_tied_scalar_fetch(vm, v);
                    stack = vm->stack; RELOAD();
                    VM_CHECK_EXC();
                    SP_PUSH(fv);
                    DISPATCH();
                }
                if (__builtin_expect(VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_WEAKREF, 0)) {
                    frame->ip = ip; vm->stack_top = sp;
                    v = vm_unwrap_weak_gc(vm, v, NULL, 0);
                    stack = vm->stack; RELOAD();
                }
                SP_PUSH(vm_val_copy(v));
            } else if (__builtin_expect(VM_IS_PTR(cv) && VM_PTR_TYPE(cv) == VM_OBJ_TIED_SCALAR, 0)) {
                frame->ip = ip; vm->stack_top = sp;
                VMValue fv = vm_tied_scalar_fetch(vm, cv);
                stack = vm->stack; RELOAD();
                VM_CHECK_EXC();
                SP_PUSH(fv);
            } else if (__builtin_expect(VM_IS_PTR(cv) && VM_PTR_TYPE(cv) == VM_OBJ_WEAKREF, 0)) {
                frame->ip = ip; vm->stack_top = sp;
                VMValue u = vm_unwrap_weak_gc(vm, cv, NULL, 0);
                stack = vm->stack; RELOAD();
                SP_PUSH(vm_val_copy(u));
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
                VMValue old_cv = VM_AS_CELL(cv)->val;
                if (__builtin_expect(VM_IS_PTR(old_cv) && VM_PTR_TYPE(old_cv) == VM_OBJ_TIED_SCALAR, 0)) {
                    frame->ip = ip; vm->stack_top = sp;
                    vm_tied_scalar_store(vm, old_cv, v);
                    stack = vm->stack; RELOAD();
                    VM_CHECK_EXC();
                    DISPATCH();
                }
                VM_AS_CELL(cv)->val = v;
            } else if (__builtin_expect(VM_IS_PTR(cv) && VM_PTR_TYPE(cv) == VM_OBJ_TIED_SCALAR, 0)) {
                frame->ip = ip; vm->stack_top = sp;
                vm_tied_scalar_store(vm, cv, v);
                stack = vm->stack; RELOAD();
                VM_CHECK_EXC();
                DISPATCH();
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
        VMValue sbuf[16];
        VMValue *sitems = NULL;
        int cnt = 0;
        uint8_t cl_ctx = vm->pending_call_context;
        vm->pending_call_context = 0;
        frame->ip = ip; vm->stack_top = sp;
        int exc = vm_collect_spread_items(vm, arr_val, sbuf, 16, &sitems, &cnt);
        stack = vm->stack; RELOAD();
        if (exc) { VM_CHECK_EXC(); }
        vm->pending_call_context = cl_ctx;
        clcall_argc = cnt;
        for (int i = 0; i < cnt; i++) SP_PUSH(sitems[i]);
        if (sitems && sitems != sbuf) free(sitems);
        goto L_do_op_call_closure;
    }

    L_do_op_call_closure: {
        int argc = clcall_argc;
        VMValue closure_val = clcall_val;
        uint8_t clcall_ctx = vm->pending_call_context;
        vm->pending_call_context = 0;

        if (VM_IS_PTR(closure_val) && VM_PTR_TYPE(closure_val) == VM_OBJ_CLOSURE) {
            if (vm->recursion_limit > 0 && (int)vm->frame_count >= vm->recursion_limit) {
                fprintf(stderr, "Deep recursion detected (limit %d) — see core::set_recursion_limit\n", vm->recursion_limit);
                exit(1);
            }
            VMClosure *cl = VM_AS_CLOSURE(closure_val);
            VMChunk *callee = &vm->program->funcs[cl->func_idx];
            frame->ip = ip; vm->stack_top = sp;

            if (vm->frame_count >= vm->frame_cap) {
                vm_frames_grow(vm);
            }
            VMFrame *nf = &vm->frames[vm->frame_count++];
            nf->chunk = callee; nf->ip = callee->code;
            nf->stack_base = sp - argc;
            nf->closure = cl;
            nf->global_save_depth = vm->global_save_count;

            int eff_argc = cl->has_bound_arg ? (argc + 1) : argc;
            nf->passed_argc = eff_argc;
            nf->call_context = clcall_ctx;

            int lcnt = callee->local_count + 1;
            if (__builtin_expect(callee->has_variadic || cl->has_bound_arg, 0)) {
                nf->locals = vm_locals_alloc(vm, lcnt, 0);
                VMValue tmp_buf[256];
                VMValue *tmp_args = (eff_argc > 256) ? malloc((size_t)eff_argc * sizeof(VMValue)) : tmp_buf;
                int offset = cl->has_bound_arg ? 1 : 0;
                if (cl->has_bound_arg) tmp_args[0] = cl->bound_arg;
                for (int i = argc - 1; i >= 0; i--) tmp_args[offset + i] = stack[--sp];
                if (callee->has_variadic) {
                    int fixed = callee->fixed_param_count;
                    if (fixed < 0) fixed = 0;
                    int vc = eff_argc - fixed;
                    VMArray *va = vm_array_new(vc > 0 ? vc : 4);
                    for (int i = 0; i < fixed && i < eff_argc; i++) {
                        if (i < lcnt) nf->locals[i] = tmp_args[i];
                    }
                    for (int i = fixed; i < eff_argc; i++) vm_array_push(va, tmp_args[i]);
                    if (fixed < lcnt) nf->locals[fixed] = VM_MAKE_PTR(va);
                } else {
                    int pmax = lcnt;
                    if (callee->fixed_param_count >= 0 && callee->fixed_param_count < pmax) {
                        pmax = callee->fixed_param_count;
                    }
                    for (int i = 0; i < eff_argc && i < pmax; i++) {
                        nf->locals[i] = tmp_args[i];
                    }
                }
                if (tmp_args != tmp_buf) free(tmp_args);
            } else {
                int pmax = lcnt;
                if (callee->fixed_param_count >= 0 && callee->fixed_param_count < pmax) {
                    pmax = callee->fixed_param_count;
                }
                int zf = argc < pmax ? argc : pmax;
                nf->locals = vm_locals_alloc(vm, lcnt, zf);
                for (int i = argc - 1; i >= 0; i--) {
                    VMValue av = stack[--sp];
                    if (i < pmax) nf->locals[i] = av;
                }
            }
            frame = nf; chunk = callee; ip = callee->code; locals = nf->locals;
            vm->stack_top = sp;
        } else if (VM_IS_PTR(closure_val) && VM_PTR_TYPE(closure_val) == VM_OBJ_NATIVE_SV) {
            VMNativeSV *nsv = (VMNativeSV*)VM_TO_PTR(closure_val);
            VMValue raw_buf[16];
            VMValue *raw_args = (argc > 16) ? malloc((size_t)argc * sizeof(VMValue)) : raw_buf;
            for (int i = argc - 1; i >= 0; i--) raw_args[i] = stack[--sp];
            StradaValue *packed = strada_new_array();
            for (int i = 0; i < argc; i++) {
                strada_array_push_take(packed->value.av, vm_to_sv(raw_args[i]));
            }
            frame->ip = ip; vm->stack_top = sp;
            StradaValue *nr = NULL;
            int prev_c_ctx = strada_wantarray() ? 1 : (strada_wanthash() ? 2 : 0);
            strada_set_call_context((int)clcall_ctx);
            int c_threw = vm_c_call_closure(vm, nsv->sv, packed, &nr, &throw_exc);
            strada_set_call_context(prev_c_ctx);
            if (c_threw) {
                stack = vm->stack;
                RELOAD();
                strada_decref(packed);
                if (raw_args != raw_buf) free(raw_args);
                goto L_vm_throw_exc;
            }
            stack = vm->stack;
            RELOAD();
            for (int i = 0; i < argc; i++) vm_fh_sync_from_sv(raw_args[i]);
            strada_decref(packed);
            if (raw_args != raw_buf) free(raw_args);
            VMValue r = nr ? sv_to_vm(nr) : VM_UNDEF_VAL;
            if (nr) strada_decref(nr);
            SP_PUSH(r);
        } else if (VM_IS_PTR(closure_val) && VM_PTR_TYPE(closure_val) == VM_OBJ_HASH) {
            /* Might be a method call on an object (from around modifier $orig->($self)) */
            /* Not a closure, skip */
            for (int i = 0; i < argc; i++) (void)SP_POP();
            SP_PUSH(VM_UNDEF_VAL);
        } else {
            for (int i = 0; i < argc; i++) (void)SP_POP();
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
        size_t slen = 0;
        const char *s = vm_bytes_view(str_val, buf, sizeof(buf), &slen);
        const char *pattern = chunk->str_consts[pat_idx];
        size_t plen = chunk->str_const_lens ? chunk->str_const_lens[pat_idx] : strlen(pattern);
        int result = vm_regex_match_len_u8(vm, s, slen, vm_is_utf8(str_val), pattern, plen);
        SP_PUSH(VM_MAKE_INT(result));
        vm_val_free(&str_val);
        DISPATCH();
    }

    CASE(OP_REGEX_FIND): {
        uint16_t pat_idx = read_u16(ip); ip += 2;
        VMValue pos_val = SP_POP(), str_val = SP_POP();
        char fbuf[4096];
        size_t slen = 0;
        const char *s = vm_bytes_view(str_val, fbuf, sizeof(fbuf), &slen);
        const char *pattern = chunk->str_consts[pat_idx];
        size_t plen = chunk->str_const_lens ? chunk->str_const_lens[pat_idx] : strlen(pattern);
        size_t mstart = 0, mend = 0;
        int64_t start = vm_to_int(pos_val);
        if (start < 0) start = 0;
        if (vm_regex_find_u8(vm, s, slen, vm_is_utf8(str_val), pattern, plen, (size_t)start, &mstart, &mend)) {
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
        size_t slen = 0;
        const char *s = vm_bytes_view(str_val, buf, sizeof(buf), &slen);
        const char *pattern = chunk->str_consts[pat_idx];
        size_t plen = chunk->str_const_lens ? chunk->str_const_lens[pat_idx] : strlen(pattern);
        int result = vm_regex_match_len_u8(vm, s, slen, vm_is_utf8(str_val), pattern, plen);
        SP_PUSH(VM_MAKE_INT(!result));
        vm_val_free(&str_val);
        DISPATCH();
    }

    CASE(OP_LOAD_CAPTURE_VAR): {
        uint8_t num = *ip++;
        if (num < 10 && vm->regex_captures[num]) {
            SP_PUSH(vm_make_str_flag(vm->regex_captures[num], vm->regex_capture_len[num], vm->regex_capture_utf8[num]));
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
        vm->pending_call_context = 0;
        if (vm->exc_top > 0) {
            VMExcHandler eh_copy = vm->exc_stack[vm->exc_top - 1];
            if (vm->exec_base > 0 && eh_copy.frame_count <= vm->exec_base) {
                size_t base_sp = vm->frames[vm->exec_base].stack_base;
                vm->gc_pause++;
                while (vm->frame_count > vm->exec_base) {
                    VMFrame *f = &vm->frames[vm->frame_count - 1];
                    int gsd = f->global_save_depth;
                    VMValue *fl = f->locals;
                    f->locals = NULL;
                    vm_locals_free(vm, fl, f->chunk->local_count + 1);
                    vm->frame_count--;
                    vm->stack_top = base_sp;
                    vm_unwind_global_saves(vm, gsd);
                }
                vm->gc_pause--;
                vm->has_pending_exc = 1;
                vm->pending_exc = throw_exc;
                vm->stack_top = base_sp;
                vm->exec_base = saved_exec_base;
                VM_EXEC_RETURN(VM_UNDEF_VAL);
            }
            vm->exc_top--;
            /* Unwind frames */
            while (vm->frame_count > eh_copy.frame_count) {
                VMFrame *f = &vm->frames[vm->frame_count - 1];
                int lc = f->chunk->local_count;
                VMValue *fl = f->locals;
                f->locals = NULL;
                vm_locals_free(vm, fl, lc + 1);
                vm->frame_count--;
            }
            vm->stack_top = eh_copy.stack_base;
            vm->gc_pause++;
            vm_unwind_global_saves(vm, eh_copy.global_save_depth);
            vm->gc_pause--;
            stack = vm->stack;
            sp = eh_copy.stack_base;
            SP_PUSH(throw_exc);
            vm->stack_top = sp;
            RELOAD();
            ip = eh_copy.catch_ip;
        } else if (vm->exec_base > 0) {
            size_t base_sp = vm->frames[vm->exec_base].stack_base;
            vm->gc_pause++;
            while (vm->frame_count > vm->exec_base) {
                VMFrame *f = &vm->frames[vm->frame_count - 1];
                int gsd = f->global_save_depth;
                VMValue *fl = f->locals;
                f->locals = NULL;
                vm_locals_free(vm, fl, f->chunk->local_count + 1);
                vm->frame_count--;
                vm->stack_top = base_sp;
                vm_unwind_global_saves(vm, gsd);
            }
            vm->gc_pause--;
            vm->has_pending_exc = 1;
            vm->pending_exc = throw_exc;
            vm->stack_top = base_sp;
            vm->exec_base = saved_exec_base;
            VM_EXEC_RETURN(VM_UNDEF_VAL);
        } else {
            vm->gc_pause++;
            while (vm->frame_count > saved_exec_base) {
                VMFrame *f = &vm->frames[vm->frame_count - 1];
                int gsd = f->global_save_depth;
                size_t f_sp = f->stack_base;
                VMValue *fl = f->locals;
                f->locals = NULL;
                vm_locals_free(vm, fl, f->chunk->local_count + 1);
                vm->frame_count--;
                vm->stack_top = f_sp;
                vm_unwind_global_saves(vm, gsd);
            }
            vm->stack_top = 0;
            vm_unwind_global_saves(vm, 0);
            vm->gc_pause--;
            stack = vm->stack;
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
        VMValue v = vm_globals_get_raw(vm, name);
        if (__builtin_expect(VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_TIED_SCALAR, 0)) {
            frame->ip = ip; vm->stack_top = sp;
            VMValue fv = vm_tied_scalar_fetch(vm, v);
            stack = vm->stack; RELOAD();
            VM_CHECK_EXC();
            SP_PUSH(fv);
            DISPATCH();
        }
        if (__builtin_expect(VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_WEAKREF, 0)) {
            frame->ip = ip; vm->stack_top = sp;
            v = vm_unwrap_weak_gc(vm, v, NULL, 0);
            stack = vm->stack; RELOAD();
        }
        SP_PUSH(vm_val_copy(v));
        DISPATCH();
    }

    CASE(OP_STORE_GLOBAL): {
        uint16_t name_idx = read_u16(ip); ip += 2;
        const char *name = chunk->str_consts[name_idx];
        VMValue v = SP_POP();
        VMValue cur = vm_globals_get(vm, name);
        if (__builtin_expect(VM_IS_PTR(cur) && VM_PTR_TYPE(cur) == VM_OBJ_TIED_SCALAR, 0)) {
            frame->ip = ip; vm->stack_top = sp;
            vm_tied_scalar_store(vm, cur, v);
            stack = vm->stack; RELOAD();
            VM_CHECK_EXC();
            DISPATCH();
        }
        vm_globals_set(vm, name, v);
        DISPATCH();
    }

    CASE(OP_INIT_GLOBAL): {
        uint16_t name_idx = read_u16(ip); ip += 2;
        const char *name = chunk->str_consts[name_idx];
        VMValue v = SP_POP();
        pthread_mutex_t *glk = vm_globals_lock_ptr(vm);
        if (glk) pthread_mutex_lock(glk);
        int gex = vm->globals ? vm_hash_exists(vm->globals, name) : 0;
        if (!gex && vm->globals) vm_hash_set(vm->globals, name, v);
        if (glk) pthread_mutex_unlock(glk);
        if (gex) {
            vm_val_free(&v);
        }
        DISPATCH();
    }

    CASE(OP_SAVE_GLOBAL): {
        uint16_t name_idx = read_u16(ip); ip += 2;
        const char *name = chunk->str_consts[name_idx];
        if (vm->global_save_count >= vm->global_save_cap) {
            vm_global_saves_grow(vm);
        }
        VMGlobalSave *gs = &vm->global_saves[vm->global_save_count++];
        memset(gs, 0, sizeof(*gs));
        gs->name = strdup(name);
        pthread_mutex_t *glk = vm_globals_lock_ptr(vm);
        if (glk) pthread_mutex_lock(glk);
        gs->saved_value = vm_val_copy(vm->globals ? vm_hash_get(vm->globals, name) : VM_UNDEF_VAL);
        /* Shadow with undef so localizing a tied our-var does not trigger STORE */
        if (vm->globals) vm_hash_set(vm->globals, name, VM_UNDEF_VAL);
        if (glk) pthread_mutex_unlock(glk);
        DISPATCH();
    }

    CASE(OP_RESTORE_GLOBAL): {
        uint16_t name_idx = read_u16(ip); ip += 2;
        const char *name = chunk->str_consts[name_idx];
        /* Find most recent save for this name */
        for (int i = vm->global_save_count - 1; i >= 0; i--) {
            if (!vm->global_saves[i].is_hash_elem && vm->global_saves[i].name &&
                strcmp(vm->global_saves[i].name, name) == 0) {
                vm_globals_set(vm, name, vm->global_saves[i].saved_value);
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
        VMHash *h = NULL;
        char *hkey = NULL;
        int existed = 0;
        VMValue saved_val = VM_UNDEF_VAL;
        if (VM_IS_PTR(hval) && VM_PTR_TYPE(hval) == VM_OBJ_HASH) {
            h = VM_AS_HASH(hval);
            char kbuf[256];
            const char *ks = vm_to_cstr(key, kbuf, sizeof(kbuf));
            hkey = strdup(ks);
            if (!VM_IS_UNDEF(h->tied_obj) && vm_obj_class_name(h->tied_obj)) {
                frame->ip = ip;
                vm->stack_top = sp;
                vm->gc_pause++;
                int efidx = vm_tied_array_find_method(vm, h->tied_obj, "EXISTS");
                if (efidx >= 0) {
                    VMValue eargs[2] = { h->tied_obj, key };
                    VMValue er = vm_execute_call(vm, efidx, eargs, 2);
                    if (__builtin_expect(vm->has_pending_exc, 0)) {
                        vm->gc_pause--;
                        free(hkey);
                        vm_temp_free(&key);
                        stack = vm->stack;
                        RELOAD();
                        throw_exc = vm->pending_exc;
                        vm->pending_exc = VM_UNDEF_VAL;
                        vm->has_pending_exc = 0;
                        goto L_vm_throw_exc;
                    }
                    existed = vm_to_bool(er) ? 1 : 0;
                    vm_temp_free(&er);
                }
                if (existed) {
                    int ffidx = vm_tied_array_find_method(vm, h->tied_obj, "FETCH");
                    if (ffidx >= 0) {
                        VMValue fargs[2] = { h->tied_obj, key };
                        VMValue fr = vm_execute_call(vm, ffidx, fargs, 2);
                        if (__builtin_expect(vm->has_pending_exc, 0)) {
                            vm->gc_pause--;
                            free(hkey);
                            vm_temp_free(&key);
                            stack = vm->stack;
                            RELOAD();
                            throw_exc = vm->pending_exc;
                            vm->pending_exc = VM_UNDEF_VAL;
                            vm->has_pending_exc = 0;
                            goto L_vm_throw_exc;
                        }
                        saved_val = fr;
                    }
                }
                vm->gc_pause--;
                stack = vm->stack;
                RELOAD();
            } else {
                existed = vm_hash_exists(h, ks);
                saved_val = existed ? vm_val_copy(vm_hash_get(h, ks)) : VM_UNDEF_VAL;
            }
        }
        if (vm->global_save_count >= vm->global_save_cap) {
            vm_global_saves_grow(vm);
        }
        VMGlobalSave *gs = &vm->global_saves[vm->global_save_count++];
        memset(gs, 0, sizeof(*gs));
        gs->is_hash_elem = 1;
        gs->hash = h;
        gs->hash_key = hkey;
        gs->existed = existed;
        gs->saved_value = saved_val;
        vm_temp_free(&key);
        DISPATCH();
    }

    CASE(OP_RESTORE_HASH_ELEM): {
        VMHash *rh = NULL;
        char *rhkey = NULL;
        int rexisted = 0;
        VMValue rsaved = VM_UNDEF_VAL;
        int found = 0;
        for (int i = vm->global_save_count - 1; i >= 0; i--) {
            if (vm->global_saves[i].is_hash_elem) {
                rh = vm->global_saves[i].hash;
                rhkey = vm->global_saves[i].hash_key;
                rexisted = vm->global_saves[i].existed;
                rsaved = vm->global_saves[i].saved_value;
                for (int j = i; j < vm->global_save_count - 1; j++)
                    vm->global_saves[j] = vm->global_saves[j + 1];
                vm->global_save_count--;
                found = 1;
                break;
            }
        }
        if (found) {
            frame->ip = ip;
            vm->stack_top = sp;
            vm_restore_one_hash_elem(vm, rh, rhkey, rexisted, rsaved, 1);
            stack = vm->stack;
            RELOAD();
            if (__builtin_expect(vm->has_pending_exc, 0)) {
                throw_exc = vm->pending_exc;
                vm->pending_exc = VM_UNDEF_VAL;
                vm->has_pending_exc = 0;
                goto L_vm_throw_exc;
            }
        }
        DISPATCH();
    }

    /* ===== Logic/Math ===== */
    CASE(OP_NEGATE): {
        VMValue v = SP_POP();
        if (__builtin_expect(vm_obj_class_name(v) != NULL, 0)) {
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
        if (__builtin_expect(VM_IS_INT(v), 1)) { SP_PUSH(VM_MAKE_INT(-VM_INT_VAL(v))); DISPATCH(); }
        if (__builtin_expect(VM_IS_FLOAT(v), 1)) { SP_PUSH(VM_MAKE_FLOAT(-VM_FLOAT_VAL(v))); DISPATCH(); }
        if (VM_IS_ANY_INT(v)) SP_PUSH(VM_MAKE_INT((int64_t)(0ULL - (uint64_t)VM_ANY_INT_VAL(v))));
        else SP_PUSH(VM_MAKE_FLOAT(-vm_to_num(v)));
        vm_temp_free(&v);
        DISPATCH();
    }
    CASE(OP_NOT): {
        VMValue v = SP_POP();
        if (__builtin_expect(VM_IS_INT(v), 1)) {
            SP_PUSH(VM_MAKE_INT_FAST(v == VM_TAG_INT_BASE));
            DISPATCH();
        }
        if (__builtin_expect(vm_obj_class_name(v) != NULL, 0)) {
            VMValue _ov_res = VM_UNDEF_VAL;
            frame->ip = ip; vm->stack_top = sp;
            if (vm_try_un_overload(vm, "!", v, &_ov_res)) {
                stack = vm->stack; RELOAD();
                VM_CHECK_EXC();
                vm_temp_free(&v);
                SP_PUSH(_ov_res);
                DISPATCH();
            }
        }
        int _tb = 0;
        VM_VALUE_IS_TRUTHY(v, _tb);
        SP_PUSH(VM_MAKE_INT(!_tb));
        vm_temp_free(&v);
        DISPATCH();
    }
    CASE(OP_DEFINED): {
        VMValue v = SP_POP();
        if (__builtin_expect(vm->has_weakrefs && VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_WEAKREF, 0)) {
            frame->ip = ip; vm->stack_top = sp;
            v = vm_unwrap_weak_gc(vm, v, NULL, 0);
            stack = vm->stack; RELOAD();
        }
        SP_PUSH(VM_MAKE_INT_FAST(!VM_IS_UNDEF(v)));
        DISPATCH();
    }
    CASE(OP_ABS): {
        VMValue v = SP_POP();
        if (!VM_IS_PTR(v)) {
            SP_PUSH(vm_abs_val(v));
            DISPATCH();
        }
        VMValue res = VM_UNDEF_VAL;
        frame->ip = ip; vm->stack_top = sp;
        int threw = vm_eval_abs_slow(vm, v, &res);
        stack = vm->stack; RELOAD();
        if (__builtin_expect(threw || vm->has_pending_exc, 0)) {
            throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
            vm_temp_free(&v);
            goto L_vm_throw_exc;
        }
        vm_temp_free(&v);
        SP_PUSH(res);
        DISPATCH();
    }
    CASE(OP_POWER): {
        VMValue b = SP_POP(), a = SP_POP();
        VM_TRY_BIN_OV("**");
        double r = pow(vm_to_num(a), vm_to_num(b));
        SP_PUSH(VM_MAKE_FLOAT(r));
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
        frame->ip = ip; vm->stack_top = sp;
        vm->gc_pause++;
        VMValue res = vm_sprintf(vm, argc, args);
        vm->gc_pause--;
        stack = vm->stack; RELOAD();
        for (int i = 0; i < argc; i++) {
            if (VM_IS_PTR(args[i]) && (VM_PTR_TYPE(args[i]) == VM_OBJ_STR || VM_PTR_TYPE(args[i]) == VM_OBJ_STRBUF))
                vm_val_free(&args[i]);
        }
        if (args != args_stk) free(args);
        VM_CHECK_EXC();
        SP_PUSH(res);
        DISPATCH();
    }

    CASE(OP_REF_TYPE): {
        VMValue v = SP_POP();
        if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_WEAKREF) {
            frame->ip = ip; vm->stack_top = sp;
            v = vm_unwrap_weak_gc(vm, v, NULL, 0);
            stack = vm->stack; RELOAD();
        }
        if (VM_IS_PTR(v)) {
            switch (VM_PTR_TYPE(v)) {
            case VM_OBJ_ARRAY: {
                VMArray *a = VM_AS_ARRAY(v);
                SP_PUSH(vm_str(a->class_name ? a->class_name : "ARRAY"));
                break;
            }
            case VM_OBJ_HASH: {
                VMHash *h = VM_AS_HASH(v);
                SP_PUSH(vm_str(h->class_name ? h->class_name : "HASH"));
                break;
            }
            case VM_OBJ_CLOSURE: {
                VMClosure *c = VM_AS_CLOSURE(v);
                SP_PUSH(vm_str(c->class_name ? c->class_name : "CODE"));
                break;
            }
            case VM_OBJ_SREF: {
                VMScalarRef *sr = VM_AS_SREF(v);
                SP_PUSH(vm_str(sr->class_name ? sr->class_name : "SCALAR"));
                break;
            }
            case VM_OBJ_NATIVE_SV: {
                VMNativeSV *nsv = (VMNativeSV *)VM_TO_PTR(v);
                SP_PUSH(vm_str(nsv->sv ? strada_reftype(nsv->sv) : ""));
                break;
            }
            case VM_OBJ_INT64:
            case VM_OBJ_UINT64:
            case VM_OBJ_STR:
            case VM_OBJ_STRBUF:
            case VM_OBJ_FILEHANDLE:
                SP_PUSH(vm_str(""));
                break;
            default: SP_PUSH(vm_str("")); break;
            }
        } else {
            SP_PUSH(vm_str(""));
        }
        vm_temp_free(&v);
        DISPATCH();
    }

    CASE(OP_WEAKEN_SLOT): {
        uint8_t kind = *ip++;
        uint16_t slot = read_u16(ip); ip += 2;
        VMValue *sp_slot = NULL;
        VMValue cont = VM_UNDEF_VAL;
        VMValue key_tmp = VM_UNDEF_VAL;
        int has_tmp = 0;
        int has_cont = 0;
        pthread_mutex_t *glk = NULL;
        if (kind == 0) {
            if (slot <= (uint16_t)chunk->local_count) {
                sp_slot = &locals[slot];
                if (VM_IS_PTR(*sp_slot) && VM_PTR_TYPE(*sp_slot) == VM_OBJ_CELL)
                    sp_slot = &VM_AS_CELL(*sp_slot)->val;
            }
        } else if (kind == 1) {
            if (frame->closure && slot < (uint16_t)frame->closure->capture_count) {
                sp_slot = &frame->closure->captures[slot];
                if (VM_IS_PTR(*sp_slot) && VM_PTR_TYPE(*sp_slot) == VM_OBJ_CELL)
                    sp_slot = &VM_AS_CELL(*sp_slot)->val;
            }
        } else if (kind == 2) {
            const char *gname = chunk->str_consts[slot];
            glk = vm_globals_lock_ptr(vm);
            if (glk) pthread_mutex_lock(glk);
            sp_slot = vm->globals ? vm_hash_slot_ptr(vm->globals, gname) : NULL;
        } else if (kind == 3) {
            key_tmp = SP_POP();
            cont = SP_POP();
            has_tmp = 1;
            has_cont = 1;
            if (VM_IS_PTR(cont) && VM_PTR_TYPE(cont) == VM_OBJ_HASH && VM_IS_UNDEF(VM_AS_HASH(cont)->tied_obj)) {
                char kbuf[256];
                size_t klen = 0;
                const char *ks = vm_bytes_view(key_tmp, kbuf, sizeof(kbuf), &klen);
                sp_slot = vm_hash_slot_ptr_n(VM_AS_HASH(cont), ks, klen);
            }
        } else if (kind == 4) {
            key_tmp = SP_POP();
            cont = SP_POP();
            has_tmp = 1;
            has_cont = 1;
            if (VM_IS_PTR(cont) && VM_PTR_TYPE(cont) == VM_OBJ_ARRAY && VM_IS_UNDEF(VM_AS_ARRAY(cont)->tied_obj)) {
                sp_slot = vm_array_slot_ptr(VM_AS_ARRAY(cont), vm_to_int(key_tmp));
            }
        } else {
            key_tmp = SP_POP();
            has_tmp = 1;
        }
        int did_weaken = 0;
        if (sp_slot && VM_IS_PTR(*sp_slot)) {
            enum VMObjType wt = VM_PTR_TYPE(*sp_slot);
            if (wt == VM_OBJ_HASH || wt == VM_OBJ_ARRAY || wt == VM_OBJ_CLOSURE ||
                wt == VM_OBJ_SREF || wt == VM_OBJ_NATIVE_SV) {
                VMWeakRef *wr = calloc(1, sizeof(VMWeakRef));
                vm_gc_init_hdr(&wr->hdr, VM_OBJ_WEAKREF);
                wr->target = *sp_slot;
                wr->is_sref = (wt == VM_OBJ_SREF) ? 1 : 0;
                *sp_slot = VM_MAKE_PTR(wr);
                vm->has_weakrefs = 1;
                did_weaken = 1;
            }
        }
        if (glk) pthread_mutex_unlock(glk);
        if (has_tmp) vm_temp_free(&key_tmp);
        if (did_weaken && vm->exec_base == 0 && vm->gc_pause == 0) {
            frame->ip = ip;
            vm->stack_top = sp;
            size_t saved_top = vm->stack_top;
            if (has_cont) {
                if (vm->stack_top >= vm->stack_cap) vm_stack_grow(vm, vm->stack_top);
                vm->stack[vm->stack_top++] = cont;
            }
            vm_gc_collect_ex(vm, (vm->exc_top > 0) ? 1 : 0);
            vm->stack_top = saved_top;
            stack = vm->stack; RELOAD();
            if (__builtin_expect(vm->has_pending_exc, 0)) {
                throw_exc = vm->pending_exc;
                vm->pending_exc = VM_UNDEF_VAL;
                vm->has_pending_exc = 0;
                goto L_vm_throw_exc;
            }
        }
        SP_PUSH(VM_UNDEF_VAL);
        DISPATCH();
    }

    CASE(OP_ISWEAK_SLOT): {
        uint8_t kind = *ip++;
        uint16_t slot = read_u16(ip); ip += 2;
        VMValue *sp_slot = NULL;
        VMValue key_tmp = VM_UNDEF_VAL;
        int has_tmp = 0;
        if (kind == 0) {
            if (slot <= (uint16_t)chunk->local_count) {
                sp_slot = &locals[slot];
                if (VM_IS_PTR(*sp_slot) && VM_PTR_TYPE(*sp_slot) == VM_OBJ_CELL)
                    sp_slot = &VM_AS_CELL(*sp_slot)->val;
            }
        } else if (kind == 1) {
            if (frame->closure && slot < (uint16_t)frame->closure->capture_count) {
                sp_slot = &frame->closure->captures[slot];
                if (VM_IS_PTR(*sp_slot) && VM_PTR_TYPE(*sp_slot) == VM_OBJ_CELL)
                    sp_slot = &VM_AS_CELL(*sp_slot)->val;
            }
        } else if (kind == 2) {
            const char *gname = chunk->str_consts[slot];
            VMValue gv = vm_globals_get_raw(vm, gname);
            int is_w = (VM_IS_PTR(gv) && VM_PTR_TYPE(gv) == VM_OBJ_WEAKREF
                        && (!VM_AS_WEAKREF(gv)->is_sref || !VM_IS_UNDEF(VM_AS_WEAKREF(gv)->target))) ? 1 : 0;
            SP_PUSH(VM_MAKE_INT(is_w));
            DISPATCH();
        } else if (kind == 3) {
            key_tmp = SP_POP();
            VMValue cont = SP_POP();
            has_tmp = 1;
            if (VM_IS_PTR(cont) && VM_PTR_TYPE(cont) == VM_OBJ_HASH && VM_IS_UNDEF(VM_AS_HASH(cont)->tied_obj)) {
                char kbuf[256];
                size_t klen = 0;
                const char *ks = vm_bytes_view(key_tmp, kbuf, sizeof(kbuf), &klen);
                sp_slot = vm_hash_slot_ptr_n(VM_AS_HASH(cont), ks, klen);
            }
        } else if (kind == 4) {
            key_tmp = SP_POP();
            VMValue cont = SP_POP();
            has_tmp = 1;
            if (VM_IS_PTR(cont) && VM_PTR_TYPE(cont) == VM_OBJ_ARRAY && VM_IS_UNDEF(VM_AS_ARRAY(cont)->tied_obj)) {
                sp_slot = vm_array_slot_ptr(VM_AS_ARRAY(cont), vm_to_int(key_tmp));
            }
        } else {
            key_tmp = SP_POP();
            has_tmp = 1;
        }
        if (has_tmp) vm_temp_free(&key_tmp);
        int is_w = (sp_slot && VM_IS_PTR(*sp_slot) && VM_PTR_TYPE(*sp_slot) == VM_OBJ_WEAKREF
                    && (!VM_AS_WEAKREF(*sp_slot)->is_sref || !VM_IS_UNDEF(VM_AS_WEAKREF(*sp_slot)->target))) ? 1 : 0;
        SP_PUSH(VM_MAKE_INT(is_w));
        DISPATCH();
    }

    CASE(OP_CLEAR_LOCALS): {
        uint16_t from = read_u16(ip); ip += 2;
        uint16_t to = read_u16(ip); ip += 2;
        uint16_t lim = (uint16_t)chunk->local_count;
        if (to < lim) lim = to;
        for (uint16_t s = from; s < lim; s++) {
            locals[s] = VM_UNDEF_VAL;
        }
        DISPATCH();
    }

    CASE(OP_ADD_OV): {
        VMValue b = SP_POP(), a = SP_POP();
        VM_TRY_BIN_OV("+");
        SP_PUSH(VM_MAKE_INT((int64_t)((uint64_t)vm_to_int(a) + (uint64_t)vm_to_int(b))));
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
                frame->ip = ip; vm->stack_top = sp;
                int threw = vm_c_call_generic(vm, gb, sa, argc, &r, &throw_exc);
                stack = vm->stack;
                RELOAD();
                for (int i = 0; i < argc; i++) vm_fh_sync_from_sv(args[i]);
                for (int i = 0; i < argc; i++) strada_decref(sa[i]);
                if (threw) {
                    for (int i = 0; i < argc; i++) {
                        if (VM_IS_PTR(args[i]) && (VM_PTR_TYPE(args[i]) == VM_OBJ_STR || VM_PTR_TYPE(args[i]) == VM_OBJ_STRBUF))
                            vm_val_free(&args[i]);
                    }
                    if (args != args_stk) free(args);
                    goto L_vm_throw_exc;
                }
                SP_PUSH(r ? sv_to_vm(r) : VM_UNDEF_VAL);
                if (r) strada_decref(r);
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
                else if (strcmp(mode, "+<") == 0) cmode = "r+";
                else if (strcmp(mode, "+>") == 0) cmode = "w+";
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
                StradaValue *s0 = vm_to_sv(args[0]);
                StradaValue *s1 = vm_to_sv(args[1]);
                StradaValue *fsv = strada_open_str_sv(s0, s1);
                strada_decref(s0);
                strada_decref(s1);
                if (fsv && !STRADA_IS_TAGGED_INT(fsv) && fsv->type == STRADA_FILEHANDLE && fsv->value.fh) {
                    VMFileHandle *fh = calloc(1, sizeof(VMFileHandle));
                    vm_gc_init_hdr(&fh->hdr, VM_OBJ_FILEHANDLE);
                    fh->fp = fsv->value.fh;
                    fh->owner_sv = fsv;
                    fh->is_mem = (mode[0] == '>' || mode[0] == 'w' || mode[0] == 'a') ? 2 : 1;
                    SP_PUSH(VM_MAKE_PTR(fh));
                } else {
                    if (fsv) strada_decref(fsv);
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
                if (fh->owner_sv) {
                    StradaValue *r = strada_str_from_fh(fh->owner_sv);
                    SP_PUSH(sv_to_vm(r));
                    if (r) strada_decref(r);
                } else if (fh->is_mem == 2) {
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
                if (fh->owner_sv) {
                    StradaValue *rc = strada_close(fh->owner_sv);
                    fh->fp = NULL;
                    SP_PUSH(sv_to_vm(rc));
                    if (rc) strada_decref(rc);
                } else if (fh->fp) {
                    fclose((FILE*)fh->fp);
                    fh->fp = NULL;
                    SP_PUSH(VM_MAKE_INT(1));
                } else {
                    SP_PUSH(VM_MAKE_INT(0));
                }
            } else {
                SP_PUSH(VM_MAKE_INT(0));
            }
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
                if (!fh->fp) {
                    SP_PUSH(VM_MAKE_INT(-1));
                } else {
                    int seek_whence = SEEK_SET;
                    if (VM_IS_PTR(args[2]) && VM_PTR_TYPE(args[2]) == VM_OBJ_STR) {
                        const char *ws = VM_AS_STR(args[2])->data;
                        if (strcmp(ws, "SEEK_SET") == 0) seek_whence = SEEK_SET;
                        else if (strcmp(ws, "SEEK_CUR") == 0) seek_whence = SEEK_CUR;
                        else if (strcmp(ws, "SEEK_END") == 0) seek_whence = SEEK_END;
                        else {
                            int64_t w = vm_to_int(args[2]);
                            if (w == 1) seek_whence = SEEK_CUR;
                            else if (w == 2) seek_whence = SEEK_END;
                        }
                    } else {
                        int64_t w = vm_to_int(args[2]);
                        if (w == 1) seek_whence = SEEK_CUR;
                        else if (w == 2) seek_whence = SEEK_END;
                    }
                    int rc = fseek((FILE*)fh->fp, (long)vm_to_int(args[1]), seek_whence);
                    SP_PUSH(VM_MAKE_INT(rc == 0 ? 1 : 0));
                }
            } else {
                SP_PUSH(VM_MAKE_INT(-1));
            }
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
                if (fh->fp) {
                    rewind((FILE*)fh->fp);
                    SP_PUSH(VM_MAKE_INT(1));
                } else {
                    SP_PUSH(VM_MAKE_INT(0));
                }
            } else {
                SP_PUSH(VM_MAKE_INT(0));
            }
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
        case BUILTIN_CORE_FWRITE: {
            if (argc >= 2 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_FILEHANDLE) {
                VMFileHandle *fh = (VMFileHandle*)VM_TO_PTR(args[0]);
                if (fh->fp) {
                    char buf[64];
                    size_t slen = 0;
                    const char *s = vm_bytes_view(args[1], buf, sizeof(buf), &slen);
                    size_t to_write = slen;
                    if (argc >= 3) {
                        int64_t n = vm_to_int(args[2]);
                        if (n <= 0) {
                            SP_PUSH(VM_MAKE_INT(0));
                            break;
                        }
                        if ((uint64_t)n < to_write) to_write = (size_t)n;
                    }
                    if (to_write == 0) {
                        SP_PUSH(VM_MAKE_INT(0));
                    } else {
                        size_t wrote = fwrite(s, 1, to_write, (FILE*)fh->fp);
                        SP_PUSH(VM_MAKE_INT((int64_t)wrote));
                    }
                } else SP_PUSH(VM_MAKE_INT(0));
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        case BUILTIN_CORE_FREAD: {
            if (argc >= 2) {
                if (VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_FILEHANDLE) {
                    VMFileHandle *fh = (VMFileHandle*)VM_TO_PTR(args[0]);
                    if (!fh->fp) {
                        SP_PUSH(VM_UNDEF_VAL);
                    } else {
                        int64_t n = vm_to_int(args[1]);
                        if (n <= 0) {
                            SP_PUSH(vm_str(""));
                        } else {
                            size_t want = (size_t)n;
                            char *buf = malloc(want + 1);
                            if (!buf) {
                                SP_PUSH(VM_UNDEF_VAL);
                            } else {
                                size_t got = fread(buf, 1, want, (FILE*)fh->fp);
                                buf[got] = '\0';
                                SP_PUSH(VM_MAKE_STR_N(buf, got));
                                free(buf);
                            }
                        }
                    }
                } else {
                    SP_PUSH(VM_UNDEF_VAL);
                }
            } else if (argc == 1) {
                VMValue __vr = VM_UNDEF_VAL;
                frame->ip = ip;
                vm->stack_top = sp;
                int __th = vm_c_call_1(vm, strada_read_file, args[0], &__vr, &throw_exc);
                stack = vm->stack;
                RELOAD();
                if (__th) goto L_builtin_throw_exc;
                SP_PUSH(__vr);
            } else {
                SP_PUSH(VM_UNDEF_VAL);
            }
            break;
        }
        case BUILTIN_CORE_SLURP: {
            if (argc >= 1) {
                char fbuf[256];
                const char *path = vm_to_cstr(args[0], fbuf, 256);
                FILE *fp = fopen(path, "rb");
                if (fp) {
                    struct stat st;
                    if (fstat(fileno(fp), &st) != 0 || S_ISDIR(st.st_mode)) {
                        fclose(fp);
                        SP_PUSH(VM_UNDEF_VAL);
                    } else if (S_ISREG(st.st_mode)) {
                        fseek(fp, 0, SEEK_END);
                        long sz = ftell(fp);
                        rewind(fp);
                        if (sz < 0 || sz == LONG_MAX) {
                            fclose(fp);
                            SP_PUSH(VM_UNDEF_VAL);
                        } else {
                            char *data = malloc((size_t)sz + 1);
                            if (!data) {
                                fclose(fp);
                                SP_PUSH(VM_UNDEF_VAL);
                            } else {
                                size_t rd = fread(data, 1, (size_t)sz, fp);
                                if (rd < (size_t)sz && ferror(fp)) {
                                    fclose(fp);
                                    free(data);
                                    SP_PUSH(VM_UNDEF_VAL);
                                } else {
                                    data[rd] = 0;
                                    fclose(fp);
                                    SP_PUSH(VM_MAKE_STR_N(data, rd));
                                    free(data);
                                }
                            }
                        }
                    } else {
                        /* Pipe/FIFO/device: stream incrementally */
                        size_t cap = 4096, len = 0;
                        char *data = malloc(cap);
                        if (!data) {
                            fclose(fp);
                            SP_PUSH(VM_UNDEF_VAL);
                        } else {
                            while (1) {
                                if (len + 4096 > cap) {
                                    cap *= 2;
                                    char *nd = realloc(data, cap);
                                    if (!nd) { free(data); data = NULL; break; }
                                    data = nd;
                                }
                                size_t n = fread(data + len, 1, 4096, fp);
                                len += n;
                                if (n < 4096) break;
                            }
                            int err = ferror(fp);
                            fclose(fp);
                            if (!data || err) {
                                free(data);
                                SP_PUSH(VM_UNDEF_VAL);
                            } else {
                                SP_PUSH(VM_MAKE_STR_N(data, len));
                                free(data);
                            }
                        }
                    }
                } else SP_PUSH(VM_UNDEF_VAL);
            } else SP_PUSH(VM_UNDEF_VAL);
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
                vm_globals_set(vm, key, vm_val_copy(args[1]));
            }
            SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_CORE_GLOBAL_GET: {
            if (argc >= 1) {
                char kbuf[256];
                const char *key = vm_to_cstr(args[0], kbuf, 256);
                VMValue v = vm_globals_get_raw(vm, key);
                if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_WEAKREF) {
                    frame->ip = ip; vm->stack_top = sp;
                    v = vm_unwrap_weak_gc(vm, v, NULL, 0);
                    stack = vm->stack; RELOAD();
                }
                SP_PUSH(vm_val_copy(v));
            } else SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_CORE_GLOBAL_EXISTS: {
            if (argc >= 1) {
                char kbuf[256];
                const char *key = vm_to_cstr(args[0], kbuf, 256);
                SP_PUSH(VM_MAKE_INT(vm_globals_exists(vm, key)));
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        case BUILTIN_CORE_GLOBAL_DELETE: {
            if (argc >= 1) {
                char kbuf[256];
                const char *key = vm_to_cstr(args[0], kbuf, 256);
                vm_globals_delete(vm, key);
            }
            SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_CORE_GLOBAL_KEYS: {
            VMArray *keys = vm_globals_keys(vm);
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
                vm_array_push(ca, vm->regex_captures[i]
                    ? vm_make_str_flag(vm->regex_captures[i], vm->regex_capture_len[i], vm->regex_capture_utf8[i])
                    : VM_UNDEF_VAL);
            SP_PUSH(VM_MAKE_PTR(ca));
            break;
        }
        case BUILTIN_NAMED_CAPTURES: {
            VMHash *nh = vm_hash_new(8);
            for (int i = 0; i < 10; i++) {
                if (vm->regex_capture_names[i] && vm->regex_captures[i])
                    vm_hash_set(nh, vm->regex_capture_names[i],
                                vm_make_str_flag(vm->regex_captures[i], vm->regex_capture_len[i], vm->regex_capture_utf8[i]));
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
                vm_method_cache_clear(vm);
            }
            SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_MATH_SQRT: {
            VM_PUSH_DOUBLE(sp, sqrt(vm_to_num(args[0])));
            break;
        }
        case BUILTIN_MATH_FLOOR: {
            VM_PUSH_DOUBLE(sp, floor(vm_to_num(args[0])));
            break;
        }
        case BUILTIN_MATH_CEIL: {
            VM_PUSH_DOUBLE(sp, ceil(vm_to_num(args[0])));
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
                if (VM_PTR_TYPE(args[0]) == VM_OBJ_ARRAY) {
                    VMArray *arr = VM_AS_ARRAY(args[0]);
                    if (__builtin_expect(!VM_IS_UNDEF(arr->tied_obj), 0)) {
                        frame->ip = ip; vm->stack_top = sp;
                        int64_t sz = vm_tied_array_fetchsize(vm, arr->tied_obj);
                        stack = vm->stack; RELOAD();
                        if (__builtin_expect(vm->has_pending_exc, 0)) {
                            throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                            goto L_builtin_throw_exc;
                        }
                        SP_PUSH(VM_MAKE_INT(sz));
                    } else {
                        SP_PUSH(VM_MAKE_INT(arr->size));
                    }
                } else if (VM_PTR_TYPE(args[0]) == VM_OBJ_HASH) {
                    VMHash *h = VM_AS_HASH(args[0]);
                    if (__builtin_expect(!VM_IS_UNDEF(h->tied_obj), 0)) {
                        int fk_fidx = vm_tied_array_find_method(vm, h->tied_obj, "FIRSTKEY");
                        if (fk_fidx >= 0) {
                            int nk_fidx = vm_tied_array_find_method(vm, h->tied_obj, "NEXTKEY");
                            frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
                            int64_t cnt = 0;
                            VMValue k = vm_tied_array_call(vm, h->tied_obj, fk_fidx, NULL, 0);
                            while (!vm->has_pending_exc && !VM_IS_UNDEF(k)) {
                                cnt++;
                                if (nk_fidx < 0) break;
                                k = vm_tied_array_call(vm, h->tied_obj, nk_fidx, &k, 1);
                            }
                            vm->gc_pause--;
                            stack = vm->stack; RELOAD();
                            if (__builtin_expect(vm->has_pending_exc, 0)) {
                                throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                                goto L_builtin_throw_exc;
                            }
                            SP_PUSH(VM_MAKE_INT(cnt));
                            break;
                        }
                    }
                    SP_PUSH(VM_MAKE_INT(h->size));
                } else SP_PUSH(VM_MAKE_INT(0));
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        case BUILTIN_CHOMP_INPLACE: {
            /* chomp modifies variable in place — but since we use value semantics,
             * we just return the chomp'd string */
            if (argc >= 1) {
                char buf[256];
                size_t len = 0;
                const char *s = vm_bytes_view(args[0], buf, sizeof(buf), &len);
                if (len > 0) {
                    if (s[len - 1] == '\n') {
                        len--;
                        if (len > 0 && s[len - 1] == '\r') len--;
                    } else if (s[len - 1] == '\r') {
                        len--;
                    }
                }
                SP_PUSH(vm_make_str_flag(s, len, vm_is_utf8(args[0])));
            } else SP_PUSH(vm_str(""));
            break;
        }
        case BUILTIN_MATH_ABS: {
            if (argc < 1) { SP_PUSH(VM_MAKE_INT(0)); break; }
            if (!VM_IS_PTR(args[0])) { SP_PUSH(vm_abs_val(args[0])); break; }
            VMValue res = VM_UNDEF_VAL;
            frame->ip = ip; vm->stack_top = sp;
            int threw = vm_eval_abs_slow(vm, args[0], &res);
            stack = vm->stack; RELOAD();
            if (__builtin_expect(threw || vm->has_pending_exc, 0)) {
                throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                goto L_builtin_throw_exc;
            }
            SP_PUSH(res);
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
        case BUILTIN_MATH_SRAND: { unsigned seed = argc >= 1 ? (unsigned)vm_to_int(args[0]) : (unsigned)time(NULL); srand(seed); SP_PUSH(VM_UNDEF_VAL); break; }
        case BUILTIN_MATH_HYPOT: { VM_PUSH_DOUBLE(sp, hypot(vm_to_num(args[0]), vm_to_num(args[1]))); break; }
        case BUILTIN_MATH_CBRT: { VM_PUSH_DOUBLE(sp, cbrt(vm_to_num(args[0]))); break; }
        case BUILTIN_MATH_ISNAN: { SP_PUSH(VM_MAKE_INT(isnan(vm_to_num(args[0])) ? 1 : 0)); break; }
        case BUILTIN_MATH_ISINF: { SP_PUSH(VM_MAKE_INT(isinf(vm_to_num(args[0])) ? 1 : 0)); break; }
        case BUILTIN_MATH_ISFINITE: { SP_PUSH(VM_MAKE_INT(isfinite(vm_to_num(args[0])) ? 1 : 0)); break; }
        case BUILTIN_UCFIRST: {
            SP_PUSH(argc >= 1 ? vm_ucfirst_val(args[0], 1) : vm_str(""));
            break;
        }
        case BUILTIN_LCFIRST: {
            SP_PUSH(argc >= 1 ? vm_ucfirst_val(args[0], 0) : vm_str(""));
            break;
        }
        case BUILTIN_TRIM: {
            if (argc >= 1) {
                char buf[256];
                size_t len = 0;
                const char *s = vm_bytes_view(args[0], buf, sizeof(buf), &len);
                size_t start = 0;
                while (start < len && isspace((unsigned char)s[start])) start++;
                size_t end = len;
                while (end > start && isspace((unsigned char)s[end - 1])) end--;
                SP_PUSH(vm_make_str_flag(s + start, end - start, vm_is_utf8(args[0])));
            } else SP_PUSH(vm_str(""));
            break;
        }
        case BUILTIN_REPLACE: {
            if (argc >= 3) {
                char sbuf[1024], pbuf[256], rbuf[256];
                size_t slen = 0, plen = 0, rlen = 0;
                const char *s = vm_bytes_view(args[0], sbuf, sizeof(sbuf), &slen);
                const char *pat = vm_bytes_view(args[1], pbuf, sizeof(pbuf), &plen);
                const char *repl = vm_bytes_view(args[2], rbuf, sizeof(rbuf), &rlen);
                int u8_s = vm_is_utf8(args[0]);
                int u8_r = vm_is_utf8(args[2]);
                if (plen > 0 && vm_pattern_is_literal(pat, plen) && vm_repl_is_literal(repl, rlen)) {
                    SP_PUSH(vm_replace_literal(s, slen, pat, plen, repl, rlen, u8_s, u8_r, 0, 1));
                } else {
                    char *result = NULL;
                    size_t out_len = 0;
                    int cnt = strada_regex_replace_capture_len2(s, slen, u8_s, pat, plen, repl, rlen, "", 0, &result, &out_len);
                    SP_PUSH(vm_make_str_flag(result ? result : s, result ? out_len : slen,
                                             (cnt > 0) ? (u8_s || u8_r) : u8_s));
                    free(result);
                }
            } else SP_PUSH(vm_str(""));
            break;
        }
        case BUILTIN_REPLACE_ALL: {
            if (argc >= 3) {
                char sbuf[1024], fbuf[256], rbuf[256];
                size_t slen = 0, flen = 0, rlen = 0;
                const char *s = vm_bytes_view(args[0], sbuf, sizeof(sbuf), &slen);
                const char *find = vm_bytes_view(args[1], fbuf, sizeof(fbuf), &flen);
                const char *repl = vm_bytes_view(args[2], rbuf, sizeof(rbuf), &rlen);
                SP_PUSH(vm_replace_literal(s, slen, find, flen, repl, rlen,
                                           vm_is_utf8(args[0]), vm_is_utf8(args[2]), 1, 0));
            } else SP_PUSH(vm_str(""));
            break;
        }
        case BUILTIN_MATCH: {
            if (argc >= 2) {
                char sbuf[1024], pbuf[256];
                size_t slen = 0, plen = 0;
                const char *s = vm_bytes_view(args[0], sbuf, sizeof(sbuf), &slen);
                const char *pat = vm_bytes_view(args[1], pbuf, sizeof(pbuf), &plen);
                int u8_s = vm_is_utf8(args[0]);
                int matched = vm_regex_match_len_u8(vm, s, slen, u8_s, pat, plen);
                if (matched && plen == strlen(pat)) {
                    strada_regex_set_subject_len(slen);
                    strada_regex_set_subject_utf8(u8_s);
                    strada_regex_match_with_capture(s, pat, "");
                    strada_regex_clear_subject_len();
                }
                SP_PUSH(VM_MAKE_INT(matched));
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        case BUILTIN_REGEX_SUBST: {
            if (argc >= 4) {
                char sbuf[1024], pbuf[256], rbuf[256], fbuf[32];
                size_t slen = 0, plen = 0, rlen = 0, flen = 0;
                const char *s = vm_bytes_view(args[0], sbuf, sizeof(sbuf), &slen);
                const char *pat = vm_bytes_view(args[1], pbuf, sizeof(pbuf), &plen);
                const char *repl = vm_bytes_view(args[2], rbuf, sizeof(rbuf), &rlen);
                const char *flags = vm_bytes_view(args[3], fbuf, sizeof(fbuf), &flen);
                char clean_flags[32];
                size_t cf = 0;
                int is_global = 0;
                if (flags) {
                    for (size_t fi = 0; fi < flen && cf + 1 < sizeof(clean_flags); fi++) {
                        if (flags[fi] == 'g') is_global = 1;
                        else clean_flags[cf++] = flags[fi];
                    }
                }
                clean_flags[cf] = '\0';
                char *res_str = NULL;
                size_t out_len = 0;
                int count = strada_regex_replace_capture_len2(s, slen, vm_is_utf8(args[0]), pat, plen, repl, rlen, clean_flags, is_global, &res_str, &out_len);
                if (count > 0) {
                    vm_sync_runtime_captures(vm);
                }
                int out_u8 = (count > 0) ? (vm_is_utf8(args[0]) || vm_is_utf8(args[2])) : vm_is_utf8(args[0]);
                VMArray *pair = vm_array_new(2);
                vm_array_push(pair, res_str ? vm_make_str_flag(res_str, out_len, out_u8)
                                            : vm_make_str_flag(s, slen, vm_is_utf8(args[0])));
                vm_array_push(pair, VM_MAKE_INT(count));
                if (res_str) free(res_str);
                SP_PUSH(VM_MAKE_PTR(pair));
            } else {
                VMArray *pair = vm_array_new(2);
                vm_array_push(pair, vm_str(""));
                vm_array_push(pair, VM_MAKE_INT(0));
                SP_PUSH(VM_MAKE_PTR(pair));
            }
            break;
        }
        case BUILTIN_SORT_DEFAULT: {
            if (argc >= 1) {
                frame->ip = ip; vm->stack_top = sp;
                VMArray *sorted = vm_sort_default_impl(vm, args[0]);
                stack = vm->stack; RELOAD();
                if (__builtin_expect(vm->has_pending_exc, 0)) {
                    throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                    goto L_builtin_throw_exc;
                }
                SP_PUSH(VM_MAKE_PTR(sorted ? sorted : vm_array_new(0)));
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
                SP_PUSH(VM_MAKE_INT(unlink(path)));
            } else SP_PUSH(VM_MAKE_INT(-1));
            break;
        }
        case BUILTIN_HEX: {
            if (argc >= 1) {
                char buf[64];
                size_t len = 0;
                const char *s = vm_bytes_view(args[0], buf, sizeof(buf), &len);
                SP_PUSH(VM_MAKE_INT(vm_parse_hex_str(s, len)));
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        case BUILTIN_OCT: {
            if (argc >= 1) {
                char buf[64];
                size_t len = 0;
                const char *s = vm_bytes_view(args[0], buf, sizeof(buf), &len);
                SP_PUSH(VM_MAKE_INT(vm_parse_oct_str(s, len)));
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
                StradaValue *hsv = vm_to_sv(args[0]);
                StradaValue *nsv = vm_to_sv(args[1]);
                int64_t off = (argc >= 3) ? vm_to_int(args[2]) : 0;
                int64_t pos = strada_rindex_sv(hsv, nsv, off, argc >= 3);
                if (hsv) strada_decref(hsv);
                if (nsv) strada_decref(nsv);
                SP_PUSH(VM_MAKE_INT(pos));
            } else {
                SP_PUSH(VM_MAKE_INT(-1));
            }
            break;
        }
        case BUILTIN_STR_LTRIM: {
            if (argc >= 1) {
                char buf[256];
                size_t len = 0;
                const char *s = vm_bytes_view(args[0], buf, sizeof(buf), &len);
                size_t start = 0;
                while (start < len && isspace((unsigned char)s[start])) start++;
                SP_PUSH(vm_make_str_flag(s + start, len - start, vm_is_utf8(args[0])));
            } else SP_PUSH(vm_str(""));
            break;
        }
        case BUILTIN_STR_RTRIM: {
            if (argc >= 1) {
                char buf[256];
                size_t len = 0;
                const char *s = vm_bytes_view(args[0], buf, sizeof(buf), &len);
                size_t end = len;
                while (end > 0 && isspace((unsigned char)s[end - 1])) end--;
                SP_PUSH(vm_make_str_flag(s, end, vm_is_utf8(args[0])));
            } else SP_PUSH(vm_str(""));
            break;
        }
        case BUILTIN_STR_CHOP: {
            if (argc >= 1) {
                char buf[1024];
                size_t len = 0;
                const char *s = vm_bytes_view(args[0], buf, sizeof(buf), &len);
                int u8 = vm_is_utf8(args[0]);
                if (len > 0) {
                    size_t new_len = len - 1;
                    if (vm_str_is_utf8_chars(args[0], s, len)) {
                        while (new_len > 0 && (((unsigned char)s[new_len] & 0xC0) == 0x80)) {
                            new_len--;
                        }
                    }
                    SP_PUSH(vm_make_str_flag(s, new_len, u8));
                } else {
                    SP_PUSH(vm_make_str_flag("", 0, u8));
                }
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
                    SP_PUSH(vm_reverse_str_val(args[0]));
                }
            } else SP_PUSH(vm_str(""));
            break;
        }

        /* === Hash/Array builtins === */
        case BUILTIN_HASH_VALUES: {
            if (argc >= 1 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_HASH) {
                VMHash *h = VM_AS_HASH(args[0]);
                if (__builtin_expect(!VM_IS_UNDEF(h->tied_obj), 0)) {
                    int has_fk = 0;
                    frame->ip = ip; vm->stack_top = sp;
                    VMArray *tkeys = vm_tied_hash_collect_keys(vm, h->tied_obj, &has_fk);
                    stack = vm->stack; RELOAD();
                    if (__builtin_expect(vm->has_pending_exc, 0)) {
                        throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                        goto L_builtin_throw_exc;
                    }
                    if (has_fk) {
                        int n = tkeys ? tkeys->size : 0;
                        VMArray *vals = vm_array_new(n > 0 ? n : 8);
                        int ffidx = vm_tied_array_find_method(vm, h->tied_obj, "FETCH");
                        vm->gc_pause++;
                        for (int i = 0; i < n; i++) {
                            VMValue kv = tkeys->items[i];
                            VMValue vv = (ffidx >= 0) ? vm_tied_array_call(vm, h->tied_obj, ffidx, &kv, 1) : VM_UNDEF_VAL;
                            if (__builtin_expect(vm->has_pending_exc, 0)) {
                                vm->gc_pause--;
                                stack = vm->stack; RELOAD();
                                throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                                goto L_builtin_throw_exc;
                            }
                            vm_array_push(vals, vv);
                        }
                        vm->gc_pause--;
                        stack = vm->stack; RELOAD();
                        SP_PUSH(VM_MAKE_PTR(vals));
                        break;
                    }
                }
                if (__builtin_expect(vm->has_weakrefs, 0)) {
                    frame->ip = ip; vm->stack_top = sp;
                    vm_unwrap_container_weak_gc(vm, args[0], args, argc);
                    stack = vm->stack; RELOAD();
                }
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
                if (__builtin_expect(!VM_IS_UNDEF(arr->tied_obj), 0)) {
                    frame->ip = ip; vm->stack_top = sp;
                    VMArray *rem = vm_tied_array_splice(vm, arr, argc, args);
                    stack = vm->stack; RELOAD();
                    if (__builtin_expect(vm->has_pending_exc, 0)) {
                        throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                        goto L_builtin_throw_exc;
                    }
                    SP_PUSH(VM_MAKE_PTR(rem));
                    break;
                }
                int64_t offset = (argc >= 2) ? vm_to_int(args[1]) : 0;
                if (offset < 0) offset = arr->size + offset;
                if (offset < 0) offset = 0;
                if (offset > arr->size) offset = arr->size;
                int64_t length = (argc >= 3) ? vm_to_int(args[2]) : arr->size - offset;
                if (length < 0) {
                    length = (arr->size - offset) + length;
                    if (length < 0) length = 0;
                }
                if (length > (int64_t)arr->size - offset) length = (int64_t)arr->size - offset;
                if (__builtin_expect(vm->has_weakrefs, 0)) {
                    frame->ip = ip; vm->stack_top = sp;
                    vm_unwrap_container_weak_gc(vm, args[0], args, argc);
                    for (int ai = 3; ai < argc; ai++) {
                        if (VM_IS_PTR(args[ai]) && VM_PTR_TYPE(args[ai]) == VM_OBJ_ARRAY) {
                            vm_unwrap_container_weak_gc(vm, args[ai], args, argc);
                        } else if (VM_IS_PTR(args[ai]) && VM_PTR_TYPE(args[ai]) == VM_OBJ_WEAKREF) {
                            args[ai] = vm_unwrap_weak_gc(vm, args[ai], args, argc);
                        }
                    }
                    stack = vm->stack; RELOAD();
                }
                /* Collect removed elements */
                VMArray *removed = vm_array_new(length > 0 ? (int)length : 8);
                for (int64_t i = 0; i < length; i++) {
                    if (arr->holes && arr->holes[offset + i]) {
                        vm_array_push(removed, VM_UNDEF_VAL);
                        if (!removed->holes) removed->holes = calloc((size_t)removed->cap, sizeof(uint8_t));
                        removed->holes[removed->size - 1] = 1;
                    } else {
                        vm_array_push(removed, vm_val_copy(arr->items[offset + i]));
                    }
                }
                /* Collect replacement elements: argc==4 carries undef, packed/target VMArray, or scalar */
                VMValue snap_buf[16];
                VMValue *snap = NULL;
                const VMValue *repl_items = NULL;
                int repl_count = 0;
                VMValue single_item = VM_UNDEF_VAL;
                if (argc == 4) {
                    VMValue rarg = args[3];
                    if (VM_IS_UNDEF(rarg)) {
                        repl_count = 0;
                    } else if (VM_IS_PTR(rarg) && VM_PTR_TYPE(rarg) == VM_OBJ_ARRAY) {
                        VMArray *ra = VM_AS_ARRAY(rarg);
                        repl_count = ra->size;
                        if (ra == arr && repl_count > 0) {
                            snap = (repl_count > 16) ? malloc((size_t)repl_count * sizeof(VMValue)) : snap_buf;
                            for (int i = 0; i < repl_count; i++) snap[i] = vm_val_copy(ra->items[i]);
                            repl_items = snap;
                        } else {
                            repl_items = ra->items;
                        }
                    } else {
                        single_item = rarg;
                        repl_items = &single_item;
                        repl_count = 1;
                    }
                } else if (argc > 4) {
                    repl_count = argc - 3;
                    repl_items = &args[3];
                }
                /* Shift elements */
                int64_t diff = (int64_t)repl_count - length;
                int tail_count = arr->size - (int)offset - (int)length;
                if (diff > 0) {
                    int old_cap = arr->cap;
                    while (arr->size + diff > arr->cap) {
                        int new_cap = arr->cap < 1024 ? arr->cap * 2 : arr->cap + arr->cap / 2;
                        if (new_cap < 8) new_cap = 8;
                        arr->cap = new_cap;
                    }
                    arr->items = realloc(arr->items, (size_t)arr->cap * sizeof(VMValue));
                    if (arr->holes) {
                        arr->holes = realloc(arr->holes, (size_t)arr->cap);
                        if (arr->cap > old_cap) memset(arr->holes + old_cap, 0, (size_t)(arr->cap - old_cap));
                    }
                    if (tail_count > 0) {
                        memmove(&arr->items[offset + repl_count], &arr->items[offset + length],
                                (size_t)tail_count * sizeof(VMValue));
                        if (arr->holes) {
                            memmove(&arr->holes[offset + repl_count], &arr->holes[offset + length],
                                    (size_t)tail_count);
                        }
                    }
                } else if (diff < 0 && tail_count > 0) {
                    memmove(&arr->items[offset + repl_count], &arr->items[offset + length],
                            (size_t)tail_count * sizeof(VMValue));
                    if (arr->holes) {
                        memmove(&arr->holes[offset + repl_count], &arr->holes[offset + length],
                                (size_t)tail_count);
                    }
                }
                arr->size += (int)diff;
                /* Insert replacements */
                for (int i = 0; i < repl_count; i++) {
                    arr->items[offset + i] = vm_val_copy(repl_items[i]);
                    if (arr->holes) arr->holes[offset + i] = 0;
                }
                if (snap && snap != snap_buf) free(snap);
                SP_PUSH(VM_MAKE_PTR(removed));
            } else SP_PUSH(VM_MAKE_PTR(vm_array_new(8)));
            break;
        }
        case BUILTIN_HASH_EACH: {
            if (argc >= 1 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_HASH) {
                VMHash *h = VM_AS_HASH(args[0]);
                if (__builtin_expect(!VM_IS_UNDEF(h->tied_obj), 0)) {
                    int fidx = VM_IS_UNDEF(h->tied_last_key)
                        ? vm_tied_array_find_method(vm, h->tied_obj, "FIRSTKEY")
                        : vm_tied_array_find_method(vm, h->tied_obj, "NEXTKEY");
                    if (fidx >= 0 || !VM_IS_UNDEF(h->tied_last_key)) {
                        frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
                        VMValue k = VM_UNDEF_VAL;
                        if (VM_IS_UNDEF(h->tied_last_key)) {
                            k = vm_tied_array_call(vm, h->tied_obj, fidx, NULL, 0);
                        } else if (fidx >= 0) {
                            VMValue last_k = h->tied_last_key;
                            k = vm_tied_array_call(vm, h->tied_obj, fidx, &last_k, 1);
                        }
                        if (__builtin_expect(vm->has_pending_exc, 0)) {
                            vm->gc_pause--;
                            stack = vm->stack; RELOAD();
                            throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                            goto L_builtin_throw_exc;
                        }
                        if (VM_IS_UNDEF(k)) {
                            h->tied_last_key = VM_UNDEF_VAL;
                            vm->gc_pause--;
                            stack = vm->stack; RELOAD();
                            SP_PUSH(VM_MAKE_PTR(vm_array_new(0)));
                        } else {
                            VMValue key_str = vm_tied_key_str(k);
                            h->tied_last_key = key_str;
                            int ffidx = vm_tied_array_find_method(vm, h->tied_obj, "FETCH");
                            VMValue v = (ffidx >= 0) ? vm_tied_array_call(vm, h->tied_obj, ffidx, &k, 1) : VM_UNDEF_VAL;
                            if (__builtin_expect(vm->has_pending_exc, 0)) {
                                vm->gc_pause--;
                                stack = vm->stack; RELOAD();
                                throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                                goto L_builtin_throw_exc;
                            }
                            vm->gc_pause--;
                            stack = vm->stack; RELOAD();
                            VMArray *pair = vm_array_new(2);
                            vm_array_push(pair, vm_val_copy(key_str));
                            vm_array_push(pair, v);
                            SP_PUSH(VM_MAKE_PTR(pair));
                        }
                        break;
                    }
                }
                /* Find next occupied entry starting from iter_pos */
                while (h->iter_pos < h->capacity && h->entries[h->iter_pos].occupied != 1)
                    h->iter_pos++;
                if (h->iter_pos < h->capacity) {
                    VMValue ev = h->entries[h->iter_pos].value;
                    if (__builtin_expect(vm->has_weakrefs && VM_IS_PTR(ev) && VM_PTR_TYPE(ev) == VM_OBJ_WEAKREF, 0)) {
                        frame->ip = ip; vm->stack_top = sp;
                        ev = vm_unwrap_weak_gc(vm, ev, args, argc);
                        stack = vm->stack; RELOAD();
                    }
                    VMArray *pair = vm_array_new(2);
                    vm_array_push(pair, VM_MAKE_STR_N(h->entries[h->iter_pos].key, h->entries[h->iter_pos].keylen));
                    vm_array_push(pair, vm_val_copy(ev));
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
                SP_PUSH(VM_MAKE_INT(chdir(vm_to_cstr(args[0], buf, 512))));
            } else SP_PUSH(VM_MAKE_INT(-1));
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
                mode_t mode = argc >= 2 ? (mode_t)vm_to_int(args[1]) : 0755;
                SP_PUSH(VM_MAKE_INT(mkdir(vm_to_cstr(args[0], buf, 512), mode)));
            } else SP_PUSH(VM_MAKE_INT(-1));
            break;
        }
        case BUILTIN_CORE_RMDIR: {
            if (argc >= 1) {
                char buf[512];
                SP_PUSH(VM_MAKE_INT(rmdir(vm_to_cstr(args[0], buf, 512))));
            } else SP_PUSH(VM_MAKE_INT(-1));
            break;
        }
        case BUILTIN_CORE_RENAME: {
            if (argc >= 2) {
                char obuf[512], nbuf[512];
                SP_PUSH(VM_MAKE_INT(rename(vm_to_cstr(args[0], obuf, 512), vm_to_cstr(args[1], nbuf, 512))));
            } else SP_PUSH(VM_MAKE_INT(-1));
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
                SP_PUSH(VM_MAKE_INT(chmod(vm_to_cstr(args[0], buf, 512), (mode_t)vm_to_int(args[1]))));
            } else SP_PUSH(VM_MAKE_INT(-1));
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
                StradaValue *s0 = vm_to_sv(args[0]);
                StradaValue *s1 = vm_to_sv(args[1]);
                StradaValue *fsv = strada_popen(s0, s1);
                strada_decref(s0);
                strada_decref(s1);
                if (fsv && !STRADA_IS_TAGGED_INT(fsv) && fsv->type == STRADA_FILEHANDLE && fsv->value.fh) {
                    VMFileHandle *fh = calloc(1, sizeof(VMFileHandle));
                    vm_gc_init_hdr(&fh->hdr, VM_OBJ_FILEHANDLE);
                    fh->fp = fsv->value.fh;
                    fh->is_popen = 1;
                    fh->owner_sv = fsv;
                    SP_PUSH(VM_MAKE_PTR(fh));
                } else {
                    if (fsv) strada_decref(fsv);
                    SP_PUSH(VM_UNDEF_VAL);
                }
            } else SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_CORE_PCLOSE: {
            if (argc >= 1 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_FILEHANDLE) {
                VMFileHandle *fh = (VMFileHandle*)VM_TO_PTR(args[0]);
                if (fh->owner_sv) {
                    if (!fh->owner_sv->value.fh) {
                        fh->fp = NULL;
                        SP_PUSH(VM_MAKE_INT(-1));
                    } else {
                        StradaValue *rc = strada_pclose(fh->owner_sv);
                        fh->fp = NULL;
                        SP_PUSH(sv_to_vm(rc));
                        if (rc) strada_decref(rc);
                    }
                } else if (fh->fp) {
                    int status = pclose((FILE*)fh->fp);
                    fh->fp = NULL;
                    SP_PUSH(VM_MAKE_INT(WEXITSTATUS(status)));
                } else {
                    SP_PUSH(VM_MAKE_INT(-1));
                }
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
            fflush(NULL);
            SP_PUSH(VM_MAKE_INT(fork()));
            break;
        }
        case BUILTIN_CORE_WAIT: {
            int status;
            pid_t pid = wait(&status);
            if (__builtin_expect(strada_has_pending_signals(), 0)) {
                frame->ip = ip; vm->stack_top = sp;
                int __th = vm_c_dispatch_signals(vm, &throw_exc);
                stack = vm->stack; RELOAD();
                if (__th) goto L_builtin_throw_exc;
            }
            SP_PUSH(VM_MAKE_INT(pid));
            break;
        }
        case BUILTIN_CORE_WAITPID: {
            if (argc >= 1) {
                int status = 0;
                int options = (argc >= 2) ? (int)vm_to_int(args[1]) : 0;
                pid_t pid = waitpid((pid_t)vm_to_int(args[0]), &status, options);
                if (__builtin_expect(strada_has_pending_signals(), 0)) {
                    frame->ip = ip; vm->stack_top = sp;
                    int __th = vm_c_dispatch_signals(vm, &throw_exc);
                    stack = vm->stack; RELOAD();
                    if (__th) goto L_builtin_throw_exc;
                }
                int64_t res = (pid == -1) ? -1 : ((pid == 0) ? 0 : (int64_t)status);
                SP_PUSH(VM_MAKE_INT(res));
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
            /* Return a hash with function, file, line from the calling frame (optional level) */
            int lvl = argc >= 1 ? (int)vm_to_int(args[0]) : 0;
            int idx = vm->frame_count - 2 - lvl;
            if (lvl < 0 || idx < 0 || idx >= (int)vm->frame_count) {
                SP_PUSH(VM_UNDEF_VAL);
                break;
            }
            VMHash *info = vm_hash_new(8);
            VMFrame *cf = &vm->frames[idx];
            if (cf->chunk && cf->chunk->name) vm_hash_set(info, "function", vm_str(cf->chunk->name));
            else vm_hash_set(info, "function", vm_str("(unknown)"));
            vm_hash_set(info, "file", vm_str("(unknown)"));
            vm_hash_set(info, "line", VM_MAKE_INT(0));
            SP_PUSH(VM_MAKE_PTR(info));
            break;
        }
        case BUILTIN_CORE_STACK_TRACE: {
            /* Build stack trace string */
            size_t cap = 1024;
            char *trace = malloc(cap);
            if (!trace) { SP_PUSH(vm_str("")); break; }
            trace[0] = '\0';
            size_t pos = 0;
            for (int i = vm->frame_count - 1; i >= 0; i--) {
                VMFrame *f = &vm->frames[i];
                const char *fname = (f->chunk && f->chunk->name) ? f->chunk->name : "(unknown)";
                size_t flen = strlen(fname);
                size_t need = pos + flen + 4;
                if (need > cap) {
                    while (cap < need) cap *= 2;
                    char *nt = realloc(trace, cap);
                    if (!nt) { free(trace); trace = NULL; break; }
                    trace = nt;
                }
                memcpy(trace + pos, "  ", 2);
                memcpy(trace + pos + 2, fname, flen);
                trace[pos + 2 + flen] = '\n';
                pos += 3 + flen;
                trace[pos] = '\0';
            }
            if (trace) {
                SP_PUSH(VM_MAKE_STR_N(trace, pos));
                free(trace);
            } else {
                SP_PUSH(vm_str(""));
            }
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
                size_t len = 0;
                const char *s = vm_bytes_view(args[0], buf, sizeof(buf), &len);
                char *out = malloc(len * 2 + 1);
                size_t j = 0;
                for (size_t i = 0; i < len; i++) {
                    unsigned char c = (unsigned char)s[i];
                    if (c < 128 && !isalnum(c) && c != '_')
                        out[j++] = '\\';
                    out[j++] = s[i];
                }
                out[j] = '\0';
                SP_PUSH(vm_make_str_flag(out, j, vm_is_utf8(args[0])));
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
            if (argc <= 0) {
                SP_PUSH(vm_str(""));
                break;
            }
            if (argc == 1 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_NATIVE_SV) {
                VMNativeSV *nsv = (VMNativeSV*)VM_TO_PTR(args[0]);
                StradaValue *sv = nsv->sv;
                if (sv && !STRADA_IS_TAGGED_INT(sv) && sv->type == STRADA_REF) sv = sv->value.rv;
                if (sv && !STRADA_IS_TAGGED_INT(sv) && sv->type == STRADA_ARRAY) {
                    frame->ip = ip; vm->stack_top = sp;
                    StradaValue *pj = strada_path_join(sv);
                    stack = vm->stack; RELOAD();
                    SP_PUSH(pj ? sv_to_vm(pj) : vm_str(""));
                    if (pj) strada_decref(pj);
                    break;
                }
            }
            char *out_buf = NULL;
            size_t out_cap = 0, out_len = 0;
            int out_utf8 = 0, threw = 0;
            frame->ip = ip; vm->stack_top = sp;
            if (argc == 1 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_ARRAY) {
                VMArray *arr = VM_AS_ARRAY(args[0]);
                if (!VM_IS_UNDEF(arr->tied_obj)) {
                    vm->gc_pause++;
                    int64_t sz = vm_tied_array_fetchsize(vm, arr->tied_obj);
                    if (vm->has_pending_exc) threw = 1;
                    else {
                        int ffidx = vm_tied_array_find_method(vm, arr->tied_obj, "FETCH");
                        for (int64_t i = 0; i < sz; i++) {
                            VMValue iv = VM_MAKE_INT(i);
                            VMValue ev = (ffidx >= 0) ? vm_tied_array_call(vm, arr->tied_obj, ffidx, &iv, 1) : VM_UNDEF_VAL;
                            if (vm->has_pending_exc || vm_path_join_append(vm, &out_buf, &out_cap, &out_len, &out_utf8, ev, i == 0)) {
                                threw = 1; break;
                            }
                        }
                    }
                    vm->gc_pause--;
                } else {
                    for (int i = 0; i < arr->size; i++) {
                        VMValue ev = (arr->holes && arr->holes[i]) ? VM_UNDEF_VAL : arr->items[i];
                        if (vm_path_join_append(vm, &out_buf, &out_cap, &out_len, &out_utf8, ev, i == 0)) {
                            threw = 1; break;
                        }
                    }
                }
            } else {
                for (int i = 0; i < argc; i++) {
                    if (vm_path_join_append(vm, &out_buf, &out_cap, &out_len, &out_utf8, args[i], i == 0)) {
                        threw = 1; break;
                    }
                }
            }
            stack = vm->stack; RELOAD();
            if (threw || vm->has_pending_exc) {
                free(out_buf);
                throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                goto L_builtin_throw_exc;
            }
            SP_PUSH(out_buf ? vm_make_str_flag(out_buf, out_len, out_utf8) : vm_str(""));
            free(out_buf);
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
#define RT_CALL_0(fn) { VMValue __vr = VM_UNDEF_VAL; frame->ip = ip; vm->stack_top = sp; int __th = vm_c_call_0(vm, (fn), &__vr, &throw_exc); stack = vm->stack; RELOAD(); if (__th) goto L_builtin_throw_exc; SP_PUSH(__vr); }
#define RT_CALL_1(fn) { VMValue __vr = VM_UNDEF_VAL; frame->ip = ip; vm->stack_top = sp; int __th = vm_c_call_1(vm, (fn), args[0], &__vr, &throw_exc); stack = vm->stack; RELOAD(); if (__th) goto L_builtin_throw_exc; SP_PUSH(__vr); }
#define RT_CALL_2(fn) { VMValue __vr = VM_UNDEF_VAL; frame->ip = ip; vm->stack_top = sp; int __th = vm_c_call_2(vm, (fn), args[0], args[1], &__vr, &throw_exc); stack = vm->stack; RELOAD(); if (__th) goto L_builtin_throw_exc; SP_PUSH(__vr); }
#define RT_CALL_3(fn) { VMValue __vr = VM_UNDEF_VAL; frame->ip = ip; vm->stack_top = sp; int __th = vm_c_call_3(vm, (fn), args[0], args[1], args[2], &__vr, &throw_exc); stack = vm->stack; RELOAD(); if (__th) goto L_builtin_throw_exc; SP_PUSH(__vr); }
#define RT_VOID_1(fn) { frame->ip = ip; vm->stack_top = sp; int __th = vm_c_void_1(vm, (fn), args[0], &throw_exc); stack = vm->stack; RELOAD(); if (__th) goto L_builtin_throw_exc; SP_PUSH(VM_UNDEF_VAL); }
#define RT_VOID_1_SV(fn) { frame->ip = ip; vm->stack_top = sp; int __th = vm_c_call_1_sv_void(vm, (fn), args[0], &throw_exc); stack = vm->stack; RELOAD(); if (__th) goto L_builtin_throw_exc; SP_PUSH(VM_UNDEF_VAL); }

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
        case BUILTIN_CORE_SOCKET_SERVER_HOST: {
            if (argc >= 2) {
                char hbuf[256];
                const char *host = vm_to_cstr(args[0], hbuf, 256);
                int port = (int)vm_to_int(args[1]);
                int backlog = (argc >= 3) ? (int)vm_to_int(args[2]) : 128;
                StradaValue *r = strada_socket_server_host(host, port, backlog);
                SP_PUSH(r ? sv_to_vm(r) : VM_UNDEF_VAL);
                if (r) strada_decref(r);
            } else SP_PUSH(VM_UNDEF_VAL);
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
            int rc = strada_socket_send_sv(sock, data);
            SP_PUSH(VM_MAKE_INT(rc));
            strada_decref(sock); strada_decref(data);
            break;
        }
        case BUILTIN_CORE_SOCKET_CLOSE: RT_VOID_1_SV(strada_socket_close); break;
        case BUILTIN_CORE_SOCKET_FLUSH: RT_VOID_1_SV(strada_socket_flush); break;
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
            int rc = strada_socket_set_nonblocking(sock, (int)vm_to_int(args[1]));
            SP_PUSH(VM_MAKE_INT(rc));
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
            size_t dlen = 0;
            const char *data = vm_bytes_view(args[1], dbuf, sizeof(dbuf), &dlen);
            const char *host = vm_to_cstr(args[2], hbuf, 256);
            int port = (int)vm_to_int(args[3]);
            int r = strada_udp_sendto(sock, data, (int)dlen, host, port);
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
            /* tie(variable, classname, ...) — VM classes: dispatch TIEHASH/TIEARRAY/TIESCALAR;
             * falls back to the runtime for native classes. */
            if (argc < 2) { SP_PUSH(VM_UNDEF_VAL); break; }
            char cbuf[256]; const char *cls = vm_to_cstr(args[1], cbuf, 256);
            const char *tie_m = NULL;
            int tie_kind = 0;
            if (VM_IS_PTR(args[0])) {
                enum VMObjType tt = VM_PTR_TYPE(args[0]);
                if (tt == VM_OBJ_HASH) { tie_m = "TIEHASH"; tie_kind = 1; }
                else if (tt == VM_OBJ_ARRAY) { tie_m = "TIEARRAY"; tie_kind = 2; }
                else if (tt == VM_OBJ_SREF) { tie_m = "TIESCALAR"; tie_kind = 3; }
            }
            if (tie_m) {
                int tfidx = vm_find_method(vm->program, cls, tie_m);
                if (tfidx >= 0) {
                    int cargc = argc - 1;
                    VMValue cbuf_args[16];
                    VMValue *cargs = (cargc > 16) ? malloc((size_t)cargc * sizeof(VMValue)) : cbuf_args;
                    cargs[0] = vm_str(cls);
                    for (int ai = 2; ai < argc; ai++) cargs[ai - 1] = vm_val_copy(args[ai]);
                    frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
                    VMValue obj = vm_invoke_func(vm, tfidx, cargs, cargc);
                    vm->gc_pause--;
                    if (cargs != cbuf_args) free(cargs);
                    stack = vm->stack; RELOAD();
                    if (__builtin_expect(vm->has_pending_exc, 0)) {
                        throw_exc = vm->pending_exc;
                        vm->pending_exc = VM_UNDEF_VAL;
                        vm->has_pending_exc = 0;
                        goto L_builtin_throw_exc;
                    }
                    if (VM_IS_UNDEF(obj)) { SP_PUSH(VM_UNDEF_VAL); break; }
                    if (tie_kind == 1) {
                        VM_AS_HASH(args[0])->tied_obj = obj;
                    } else if (tie_kind == 2) {
                        VM_AS_ARRAY(args[0])->tied_obj = obj;
                    } else {
                        VMScalarRef *sr = VM_AS_SREF(args[0]);
                        VMTiedScalar *ts = calloc(1, sizeof(VMTiedScalar));
                        vm_gc_init_hdr(&ts->hdr, VM_OBJ_TIED_SCALAR);
                        ts->tied_obj = obj;
                        if (sr->global_name) {
                            vm_globals_set(vm, sr->global_name, VM_MAKE_PTR(ts));
                        } else if (VM_IS_PTR(sr->cell) && VM_PTR_TYPE(sr->cell) == VM_OBJ_CELL) {
                            VM_AS_CELL(sr->cell)->val = VM_MAKE_PTR(ts);
                        }
                    }
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
            if (VM_IS_PTR(args[0])) {
                enum VMObjType tt = VM_PTR_TYPE(args[0]);
                VMValue tobj = VM_UNDEF_VAL;
                if (tt == VM_OBJ_HASH && !VM_IS_UNDEF(VM_AS_HASH(args[0])->tied_obj)) {
                    tobj = VM_AS_HASH(args[0])->tied_obj;
                    VM_AS_HASH(args[0])->tied_obj = VM_UNDEF_VAL;
                    VM_AS_HASH(args[0])->tied_last_key = VM_UNDEF_VAL;
                } else if (tt == VM_OBJ_ARRAY && !VM_IS_UNDEF(VM_AS_ARRAY(args[0])->tied_obj)) {
                    tobj = VM_AS_ARRAY(args[0])->tied_obj;
                    VM_AS_ARRAY(args[0])->tied_obj = VM_UNDEF_VAL;
                } else if (tt == VM_OBJ_SREF) {
                    VMScalarRef *sr = VM_AS_SREF(args[0]);
                    if (sr->global_name) {
                        VMValue cur = vm_globals_get(vm, sr->global_name);
                        if (VM_IS_PTR(cur) && VM_PTR_TYPE(cur) == VM_OBJ_TIED_SCALAR) {
                            tobj = VM_AS_TIED_SCALAR(cur)->tied_obj;
                            vm_globals_set(vm, sr->global_name, VM_UNDEF_VAL);
                        }
                    } else if (VM_IS_PTR(sr->cell) && VM_PTR_TYPE(sr->cell) == VM_OBJ_CELL) {
                        VMValue cur = VM_AS_CELL(sr->cell)->val;
                        if (VM_IS_PTR(cur) && VM_PTR_TYPE(cur) == VM_OBJ_TIED_SCALAR) {
                            tobj = VM_AS_TIED_SCALAR(cur)->tied_obj;
                            VM_AS_CELL(sr->cell)->val = VM_UNDEF_VAL;
                        }
                    }
                }
                if (tt == VM_OBJ_HASH || tt == VM_OBJ_ARRAY || tt == VM_OBJ_SREF) {
                    if (!VM_IS_UNDEF(tobj)) {
                        int ufidx = vm_tied_array_find_method(vm, tobj, "UNTIE");
                        if (ufidx >= 0) {
                            frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
                            vm_tied_array_call(vm, tobj, ufidx, NULL, 0);
                            vm->gc_pause--;
                            stack = vm->stack; RELOAD();
                            if (vm->has_pending_exc) {
                                throw_exc = vm->pending_exc;
                                vm->pending_exc = VM_UNDEF_VAL;
                                vm->has_pending_exc = 0;
                                goto L_builtin_throw_exc;
                            }
                        }
                    }
                    SP_PUSH(VM_UNDEF_VAL);
                    break;
                }
            }
            StradaValue *var = vm_to_sv(args[0]);
            strada_untie(var);
            SP_PUSH(VM_UNDEF_VAL); strada_decref(var);
            break;
        }
        case BUILTIN_TIED: {
            if (argc < 1) { SP_PUSH(VM_UNDEF_VAL); break; }
            if (VM_IS_PTR(args[0])) {
                enum VMObjType tt = VM_PTR_TYPE(args[0]);
                if (tt == VM_OBJ_HASH && !VM_IS_UNDEF(VM_AS_HASH(args[0])->tied_obj)) {
                    SP_PUSH(VM_AS_HASH(args[0])->tied_obj);
                    break;
                }
                if (tt == VM_OBJ_ARRAY && !VM_IS_UNDEF(VM_AS_ARRAY(args[0])->tied_obj)) {
                    SP_PUSH(VM_AS_ARRAY(args[0])->tied_obj);
                    break;
                }
                if (tt == VM_OBJ_SREF) {
                    VMScalarRef *sr = VM_AS_SREF(args[0]);
                    VMValue cur = sr->global_name ? vm_globals_get(vm, sr->global_name)
                                : ((VM_IS_PTR(sr->cell) && VM_PTR_TYPE(sr->cell) == VM_OBJ_CELL)
                                   ? VM_AS_CELL(sr->cell)->val : VM_UNDEF_VAL);
                    if (VM_IS_PTR(cur) && VM_PTR_TYPE(cur) == VM_OBJ_TIED_SCALAR) {
                        SP_PUSH(VM_AS_TIED_SCALAR(cur)->tied_obj);
                    } else {
                        SP_PUSH(VM_UNDEF_VAL);
                    }
                    break;
                }
            }
            RT_CALL_1(strada_tied);
            break;
        }

        /* === String operations === */
        case BUILTIN_STR_REPLACE_PLAIN: {
            /* str_replace(str, find, replace) — replace all occurrences (plain string) */
            char sbuf[4096], fbuf[256], rbuf[256];
            size_t slen = 0, flen = 0, rlen = 0;
            const char *s = vm_bytes_view(args[0], sbuf, sizeof(sbuf), &slen);
            const char *find = vm_bytes_view(args[1], fbuf, sizeof(fbuf), &flen);
            const char *repl = vm_bytes_view(args[2], rbuf, sizeof(rbuf), &rlen);
            SP_PUSH(vm_replace_literal(s, slen, find, flen, repl, rlen,
                                       vm_is_utf8(args[0]), vm_is_utf8(args[2]), 1, 0));
            break;
        }
        case BUILTIN_STR_REPLACE_FIRST: {
            char sbuf[4096], fbuf[256], rbuf[256];
            size_t slen = 0, flen = 0, rlen = 0;
            const char *s = vm_bytes_view(args[0], sbuf, sizeof(sbuf), &slen);
            const char *find = vm_bytes_view(args[1], fbuf, sizeof(fbuf), &flen);
            const char *repl = vm_bytes_view(args[2], rbuf, sizeof(rbuf), &rlen);
            SP_PUSH(vm_replace_literal(s, slen, find, flen, repl, rlen,
                                       vm_is_utf8(args[0]), vm_is_utf8(args[2]), 0, 0));
            break;
        }
        case BUILTIN_REGEX_REPLACE: {
            char sbuf[4096], pbuf[256], rbuf[256];
            size_t slen = 0, plen = 0, rlen = 0;
            const char *s = vm_bytes_view(args[0], sbuf, sizeof(sbuf), &slen);
            const char *pat = vm_bytes_view(args[1], pbuf, sizeof(pbuf), &plen);
            const char *repl = vm_bytes_view(args[2], rbuf, sizeof(rbuf), &rlen);
            char *result = NULL;
            size_t out_len = 0;
            (void)strada_regex_replace_capture_len2(s, slen, vm_is_utf8(args[0]), pat, plen, repl, rlen, "", 0, &result, &out_len);
            SP_PUSH(vm_make_str_flag(result ? result : s, result ? out_len : slen,
                                     vm_is_utf8(args[0]) || vm_is_utf8(args[2])));
            free(result);
            break;
        }
        case BUILTIN_REGEX_REPLACE_ALL: {
            char sbuf[4096], pbuf[256], rbuf[256];
            size_t slen = 0, plen = 0, rlen = 0;
            const char *s = vm_bytes_view(args[0], sbuf, sizeof(sbuf), &slen);
            const char *pat = vm_bytes_view(args[1], pbuf, sizeof(pbuf), &plen);
            const char *repl = vm_bytes_view(args[2], rbuf, sizeof(rbuf), &rlen);
            char *result = NULL;
            size_t out_len = 0;
            (void)strada_regex_replace_capture_len2(s, slen, vm_is_utf8(args[0]), pat, plen, repl, rlen, "", 1, &result, &out_len);
            SP_PUSH(vm_make_str_flag(result ? result : s, result ? out_len : slen,
                                     vm_is_utf8(args[0]) || vm_is_utf8(args[2])));
            free(result);
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
            int fail_ok = argc >= 2 ? (int)vm_to_int(args[1]) : 0;
            VMValue vr = VM_UNDEF_VAL;
            frame->ip = ip; vm->stack_top = sp;
            int __th = vm_c_call_downgrade(vm, args[0], fail_ok, &vr, &throw_exc);
            stack = vm->stack; RELOAD();
            if (__th) goto L_builtin_throw_exc;
            SP_PUSH(vr);
            break;
        }
        case BUILTIN_UTF8_UPGRADE: RT_CALL_1(strada_utf8_upgrade); break;
        case BUILTIN_UTF8_DECODE_WB: {
            VMValue pair = VM_UNDEF_VAL;
            frame->ip = ip; vm->stack_top = sp;
            int __th = vm_c_call_utf8_wb(vm, 0, args[0], 0, &pair, &throw_exc);
            stack = vm->stack; RELOAD();
            if (__th) goto L_builtin_throw_exc;
            SP_PUSH(pair);
            break;
        }
        case BUILTIN_UTF8_DOWNGRADE_WB: {
            int fail_ok = (argc >= 2) ? (int)vm_to_int(args[1]) : 0;
            VMValue pair = VM_UNDEF_VAL;
            frame->ip = ip; vm->stack_top = sp;
            int __th = vm_c_call_utf8_wb(vm, 1, args[0], fail_ok, &pair, &throw_exc);
            stack = vm->stack; RELOAD();
            if (__th) goto L_builtin_throw_exc;
            SP_PUSH(pair);
            break;
        }
        case BUILTIN_UTF8_UPGRADE_WB: {
            VMValue pair = VM_UNDEF_VAL;
            frame->ip = ip; vm->stack_top = sp;
            int __th = vm_c_call_utf8_wb(vm, 2, args[0], 0, &pair, &throw_exc);
            stack = vm->stack; RELOAD();
            if (__th) goto L_builtin_throw_exc;
            SP_PUSH(pair);
            break;
        }
        case BUILTIN_UTF8_UNICODE_TO_NATIVE: RT_CALL_1(strada_utf8_unicode_to_native); break;

        /* === Misc core === */
        case BUILTIN_CORE_WANTARRAY: { SP_PUSH(VM_MAKE_INT(frame->call_context == 1 ? 1 : 0)); break; }
        case BUILTIN_CORE_WANTSCALAR: { SP_PUSH(VM_MAKE_INT(frame->call_context == 0 ? 1 : 0)); break; }
        case BUILTIN_CORE_WANTHASH: { SP_PUSH(VM_MAKE_INT(frame->call_context == 2 ? 1 : 0)); break; }
        case BUILTIN_CORE_SELECT_FDS: {
            if (argc >= 2) {
                StradaValue *fds_sv = vm_to_sv(args[0]);
                StradaValue *r = strada_select_fds(fds_sv, (int)vm_to_int(args[1]));
                SP_PUSH(r ? sv_to_vm(r) : VM_UNDEF_VAL);
                if (r) strada_decref(r);
                strada_decref(fds_sv);
            } else SP_PUSH(VM_UNDEF_VAL);
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
            } else if (argc == 1 && VM_IS_PTR(args[0]) &&
                       (VM_PTR_TYPE(args[0]) == VM_OBJ_HASH ||
                        VM_PTR_TYPE(args[0]) == VM_OBJ_ARRAY ||
                        VM_PTR_TYPE(args[0]) == VM_OBJ_SREF ||
                        VM_PTR_TYPE(args[0]) == VM_OBJ_CLOSURE ||
                        VM_PTR_TYPE(args[0]) == VM_OBJ_NATIVE_SV)) {
                throw_exc = args[0];
            } else {
                size_t cap = 128, len = 0;
                char *dbuf = malloc(cap);
                int out_utf8 = 0;
                vm->gc_pause++;
                for (int i = 0; i < argc; i++) {
                    VMValue av = args[i];
                    int sv_tmp = 0;
                    if (vm_has_stringify(vm->program, av)) {
                        frame->ip = ip; vm->stack_top = sp;
                        av = vm_stringify_ov(vm, av);
                        stack = vm->stack; RELOAD();
                        if (__builtin_expect(vm->has_pending_exc, 0)) {
                            vm->gc_pause--;
                            free(dbuf);
                            for (int _ai = 0; _ai < argc; _ai++) {
                                if (VM_IS_PTR(args[_ai]) && (VM_PTR_TYPE(args[_ai]) == VM_OBJ_STR || VM_PTR_TYPE(args[_ai]) == VM_OBJ_STRBUF))
                                    vm_val_free(&args[_ai]);
                            }
                            if (args != args_stk) free(args);
                            throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                            vm->stack_top = sp;
                            goto L_vm_throw_exc;
                        }
                        sv_tmp = 1;
                    }
                    if (vm_is_utf8(av)) out_utf8 = 1;
                    char abuf[64];
                    size_t alen = 0;
                    const char *as = vm_bytes_view(av, abuf, sizeof(abuf), &alen);
                    if (len + alen + 1 > cap) {
                        while (len + alen + 1 > cap) cap *= 2;
                        dbuf = realloc(dbuf, cap);
                    }
                    if (alen > 0) memcpy(dbuf + len, as, alen);
                    len += alen;
                    if (sv_tmp) vm_temp_free(&av);
                }
                vm->gc_pause--;
                dbuf[len] = '\0';
                throw_exc = vm_make_str_flag(dbuf, len, out_utf8);
                free(dbuf);
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
            for (int i = 0; i < argc; i++) {
                vm_write_val(stderr, args[i], 0);
            }
            if (argc == 0) {
                fprintf(stderr, "\n");
            } else {
                char buf[4096];
                size_t last_len = 0;
                const char *last_s = vm_bytes_view(args[argc - 1], buf, sizeof(buf), &last_len);
                if (last_len == 0 || last_s[last_len - 1] != '\n')
                    fputc('\n', stderr);
            }
            SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_BLESSED: {
            const char *cname = (argc >= 1) ? vm_obj_class_name(args[0]) : NULL;
            SP_PUSH(cname ? vm_str(cname) : VM_UNDEF_VAL);
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
                else if (VM_IS_FLOAT(args[0])) SP_PUSH(vm_str("num"));
                else if (VM_IS_UNDEF(args[0])) SP_PUSH(vm_str("undef"));
                else if (VM_IS_PTR(args[0])) {
                    switch (VM_PTR_TYPE(args[0])) {
                    case VM_OBJ_INT64:
                    case VM_OBJ_UINT64: SP_PUSH(vm_str("int")); break;
                    case VM_OBJ_STR: case VM_OBJ_STRBUF: SP_PUSH(vm_str("str")); break;
                    case VM_OBJ_ARRAY:
                        SP_PUSH(vm_str(VM_AS_ARRAY(args[0])->class_name ? "ref" : "array"));
                        break;
                    case VM_OBJ_HASH:
                        SP_PUSH(vm_str(VM_AS_HASH(args[0])->class_name ? "ref" : "hash"));
                        break;
                    case VM_OBJ_CLOSURE:
                        SP_PUSH(vm_str(VM_AS_CLOSURE(args[0])->class_name ? "ref" : "code"));
                        break;
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
                if (__builtin_expect(!VM_IS_UNDEF(src->tied_obj), 0)) {
                    frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
                    int64_t sz = vm_tied_array_fetchsize(vm, src->tied_obj);
                    if (vm->has_pending_exc) {
                        vm->gc_pause--; stack = vm->stack; RELOAD();
                        throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                        goto L_builtin_throw_exc;
                    }
                    int n = (sz > 0) ? (int)sz : 0;
                    VMArray *cp = vm_array_new(n > 0 ? n : 8);
                    int ffidx = vm_tied_array_find_method(vm, src->tied_obj, "FETCH");
                    for (int i = 0; i < n; i++) {
                        VMValue iv = VM_MAKE_INT(i);
                        VMValue v = (ffidx >= 0) ? vm_tied_array_call(vm, src->tied_obj, ffidx, &iv, 1) : VM_UNDEF_VAL;
                        if (vm->has_pending_exc) {
                            vm->gc_pause--; stack = vm->stack; RELOAD();
                            throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                            goto L_builtin_throw_exc;
                        }
                        vm_array_push(cp, v);
                    }
                    vm->gc_pause--;
                    stack = vm->stack; RELOAD();
                    SP_PUSH(VM_MAKE_PTR(cp));
                    break;
                }
                VMArray *cp = vm_array_new(src->size > 0 ? src->size : 8);
                for (int i = 0; i < src->size; i++) {
                    VMValue ev = src->items[i];
                    if (vm->has_weakrefs && VM_IS_PTR(ev) && VM_PTR_TYPE(ev) == VM_OBJ_WEAKREF) {
                        VMValue roots[2] = { args[0], VM_MAKE_PTR(cp) };
                        frame->ip = ip; vm->stack_top = sp;
                        ev = vm_unwrap_weak_gc(vm, ev, roots, 2);
                        stack = vm->stack; RELOAD();
                        src = VM_AS_ARRAY(args[0]);
                    }
                    vm_array_push(cp, vm_val_copy(ev));
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
                if (__builtin_expect(!VM_IS_UNDEF(src->tied_obj), 0)) {
                    frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
                    int has_fk = 0;
                    VMArray *tkeys = vm_tied_hash_collect_keys(vm, src->tied_obj, &has_fk);
                    if (vm->has_pending_exc) {
                        vm->gc_pause--; stack = vm->stack; RELOAD();
                        throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                        goto L_builtin_throw_exc;
                    }
                    if (has_fk && tkeys) {
                        VMHash *cp = vm_hash_new(tkeys->size > 8 ? tkeys->size : 16);
                        int ffidx = vm_tied_array_find_method(vm, src->tied_obj, "FETCH");
                        for (int i = 0; i < tkeys->size; i++) {
                            VMValue kval = tkeys->items[i];
                            VMValue v = (ffidx >= 0) ? vm_tied_array_call(vm, src->tied_obj, ffidx, &kval, 1) : VM_UNDEF_VAL;
                            if (vm->has_pending_exc) {
                                vm->gc_pause--; stack = vm->stack; RELOAD();
                                throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                                goto L_builtin_throw_exc;
                            }
                            if (VM_IS_PTR(kval) && VM_PTR_TYPE(kval) == VM_OBJ_STR) {
                                VMString *ks = VM_AS_STR(kval);
                                vm_hash_set_n(cp, ks->data, ks->len, v);
                            }
                        }
                        vm->gc_pause--;
                        stack = vm->stack; RELOAD();
                        SP_PUSH(VM_MAKE_PTR(cp));
                        break;
                    }
                    vm->gc_pause--;
                    stack = vm->stack; RELOAD();
                }
                VMHash *cp = vm_hash_new(src->capacity > 16 ? src->capacity : 16);
                for (int i = 0; i < src->capacity; i++) {
                    if (src->entries[i].occupied == 1) {
                        VMValue ev = src->entries[i].value;
                        if (vm->has_weakrefs && VM_IS_PTR(ev) && VM_PTR_TYPE(ev) == VM_OBJ_WEAKREF) {
                            VMValue roots[2] = { args[0], VM_MAKE_PTR(cp) };
                            frame->ip = ip; vm->stack_top = sp;
                            ev = vm_unwrap_weak_gc(vm, ev, roots, 2);
                            stack = vm->stack; RELOAD();
                            src = VM_AS_HASH(args[0]);
                        }
                        vm_hash_set_n(cp, src->entries[i].key, src->entries[i].keylen,
                                      vm_val_copy(ev));
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
                                size_t klen = 0;
                                const char *k = vm_bytes_view(pair->items[0], kbuf, sizeof(kbuf), &klen);
                                vm_hash_set_n(cp, k, klen, vm_val_copy(pair->items[1]));
                            }
                        }
                    }
                } else {
                    for (int i = 0; i + 1 < src->size; i += 2) {
                        char kbuf[256];
                        size_t klen = 0;
                        const char *k = vm_bytes_view(src->items[i], kbuf, sizeof(kbuf), &klen);
                        vm_hash_set_n(cp, k, klen, vm_val_copy(src->items[i + 1]));
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
                if (VM_PTR_TYPE(args[0]) == VM_OBJ_WEAKREF) {
                    frame->ip = ip; vm->stack_top = sp;
                    args[0] = vm_unwrap_weak_gc(vm, args[0], args, argc);
                    stack = vm->stack; RELOAD();
                }
                if (VM_IS_PTR(args[0])) {
                    switch (VM_PTR_TYPE(args[0])) {
                    case VM_OBJ_ARRAY: SP_PUSH(vm_str("ARRAY")); break;
                    case VM_OBJ_HASH: SP_PUSH(vm_str("HASH")); break;
                    case VM_OBJ_CLOSURE: SP_PUSH(vm_str("CODE")); break;
                    case VM_OBJ_SREF: SP_PUSH(vm_str("SCALAR")); break;
                    case VM_OBJ_NATIVE_SV: {
                        VMNativeSV *nsv = (VMNativeSV*)VM_TO_PTR(args[0]);
                        SP_PUSH(vm_str(strada_reftype_raw(nsv->sv)));
                        break;
                    }
                    default: SP_PUSH(vm_str("")); break;
                    }
                } else {
                    SP_PUSH(vm_str(""));
                }
            } else {
                SP_PUSH(vm_str(""));
            }
            break;
        }
        case BUILTIN_DEREF: {
            if (argc >= 1) {
                VMValue raw = args[0];
                if (VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_SREF) {
                    VMScalarRef *sr = VM_AS_SREF(args[0]);
                    if (sr->global_name) {
                        raw = vm_globals_get(vm, sr->global_name);
                    } else if (VM_IS_PTR(sr->cell) && VM_PTR_TYPE(sr->cell) == VM_OBJ_CELL) {
                        raw = VM_AS_CELL(sr->cell)->val;
                    } else {
                        raw = VM_UNDEF_VAL;
                    }
                } else if (VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_CELL) {
                    raw = VM_AS_CELL(args[0])->val;
                } else if (VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_NATIVE_SV) {
                    StradaValue *nsv = ((VMNativeSV *)VM_TO_PTR(args[0]))->sv;
                    StradaValue *d = nsv ? strada_deref(nsv) : NULL;
                    VMValue res = d ? sv_to_vm(d) : VM_UNDEF_VAL;
                    if (d) strada_decref(d);
                    SP_PUSH(res);
                    break;
                }
                if (__builtin_expect(VM_IS_PTR(raw) && VM_PTR_TYPE(raw) == VM_OBJ_TIED_SCALAR, 0)) {
                    frame->ip = ip; vm->stack_top = sp;
                    raw = vm_tied_scalar_fetch(vm, raw);
                    stack = vm->stack; RELOAD();
                    if (__builtin_expect(vm->has_pending_exc, 0)) {
                        throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                        goto L_builtin_throw_exc;
                    }
                    SP_PUSH(raw);
                } else {
                    SP_PUSH(vm_val_copy(raw));
                }
            } else {
                SP_PUSH(VM_UNDEF_VAL);
            }
            break;
        }
        case BUILTIN_IS_REF:
        case BUILTIN_IS_REFTO: {
            int is_r = 0;
            const char *rt = "";
            if (argc >= 1 && VM_IS_PTR(args[0])) {
                enum VMObjType t = VM_PTR_TYPE(args[0]);
                if (t == VM_OBJ_ARRAY) { is_r = 1; rt = "ARRAY"; }
                else if (t == VM_OBJ_HASH) { is_r = 1; rt = "HASH"; }
                else if (t == VM_OBJ_CLOSURE) { is_r = 1; rt = "CODE"; }
                else if (t == VM_OBJ_SREF) { is_r = 1; rt = "SCALAR"; }
                else if (t == VM_OBJ_NATIVE_SV) {
                    VMNativeSV *nsv = (VMNativeSV*)VM_TO_PTR(args[0]);
                    if (strada_is_ref(nsv->sv)) {
                        is_r = 1;
                        rt = strada_reftype_raw(nsv->sv);
                    }
                }
            }
            if (bid == BUILTIN_IS_REFTO && argc >= 2) {
                char tbuf[128];
                const char *want = vm_to_cstr(args[1], tbuf, sizeof(tbuf));
                SP_PUSH(VM_MAKE_INT(is_r && rt && strcmp(rt, want) == 0));
            } else {
                SP_PUSH(VM_MAKE_INT(is_r));
            }
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
            if (argc >= 1) {
                frame->ip = ip; vm->stack_top = sp;
                VMArray *sorted = vm_nsort_impl(vm, args[0]);
                stack = vm->stack; RELOAD();
                if (__builtin_expect(vm->has_pending_exc, 0)) {
                    throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                    goto L_builtin_throw_exc;
                }
                SP_PUSH(VM_MAKE_PTR(sorted ? sorted : vm_array_new(0)));
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
#undef RT_VOID_1_SV
#undef RT_VOID_2

        case BUILTIN_CASE_EQ_CHAR: {
            int eq = 0;
            if (argc >= 3) {
                VMValue sw = args[0];
                char lbuf[64], sbuf[256];
                size_t lit_len = 0;
                const char *lit = vm_bytes_view(args[1], lbuf, sizeof(lbuf), &lit_len);
                int64_t char_code = vm_to_int(args[2]);
                if (VM_IS_UNDEF(sw)) {
                    eq = (lit_len == 0);
                } else if (VM_IS_ANY_INT(sw)) {
                    eq = (VM_ANY_INT_VAL(sw) == char_code);
                } else {
                    size_t slen = 0;
                    const char *sp_v = vm_bytes_view(sw, sbuf, sizeof(sbuf), &slen);
                    eq = (slen == lit_len && (lit_len == 0 || memcmp(sp_v, lit, lit_len) == 0));
                }
            }
            SP_PUSH(VM_MAKE_INT(eq));
            break;
        }
        case BUILTIN_STR_INDEX_3: {
            int64_t pos = -1;
            if (argc >= 3) {
                char hbuf[256], nbuf[64];
                size_t hlen = 0, nlen = 0;
                const char *h = vm_bytes_view(args[0], hbuf, sizeof(hbuf), &hlen);
                const char *n = vm_bytes_view(args[1], nbuf, sizeof(nbuf), &nlen);
                int64_t offset = vm_to_int(args[2]);
                if (offset < 0) offset = 0;
                int is_u8 = vm_str_is_utf8_chars(args[0], h, hlen);
                if (is_u8) {
                    int64_t clen = (int64_t)vm_utf8_char_count(h, hlen);
                    if (offset <= clen) {
                        size_t start_b = vm_utf8_char_to_byte(h, hlen, offset);
                        const char *f = vm_memmem(h + start_b, hlen - start_b, n, nlen);
                        if (f) pos = vm_utf8_byte_to_char(h, (size_t)(f - h));
                    }
                } else {
                    if ((size_t)offset <= hlen) {
                        size_t start_b = (size_t)offset;
                        const char *f = vm_memmem(h + start_b, hlen - start_b, n, nlen);
                        if (f) pos = (int64_t)(f - h);
                    }
                }
            }
            SP_PUSH(VM_MAKE_INT(pos));
            break;
        }
        case BUILTIN_ARRAY_EXISTS: {
            int exists = 0;
            if (argc >= 2 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_ARRAY) {
                VMArray *arr = VM_AS_ARRAY(args[0]);
                int64_t idx = vm_to_int(args[1]);
                if (__builtin_expect(!VM_IS_UNDEF(arr->tied_obj), 0)) {
                    frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
                    int64_t sz = vm_tied_array_fetchsize(vm, arr->tied_obj);
                    if (!vm->has_pending_exc) {
                        if (idx < 0) idx += sz;
                        int efidx = vm_tied_array_find_method(vm, arr->tied_obj, "EXISTS");
                        if (efidx >= 0) {
                            VMValue iv = VM_MAKE_INT(idx);
                            VMValue er = vm_tied_array_call(vm, arr->tied_obj, efidx, &iv, 1);
                            if (!vm->has_pending_exc) exists = vm_to_bool(er);
                        } else {
                            exists = (idx >= 0 && idx < sz) ? 1 : 0;
                        }
                    }
                    vm->gc_pause--;
                    stack = vm->stack; RELOAD();
                    if (__builtin_expect(vm->has_pending_exc, 0)) {
                        throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                        goto L_builtin_throw_exc;
                    }
                    SP_PUSH(VM_MAKE_INT(exists));
                    break;
                }
                if (idx < 0) idx += arr->size;
                if (idx >= 0 && idx < arr->size) {
                    exists = (!arr->holes || !arr->holes[idx]) ? 1 : 0;
                }
            }
            SP_PUSH(VM_MAKE_INT(exists));
            break;
        }
        case BUILTIN_ARRAY_DELETE: {
            VMValue deleted = VM_UNDEF_VAL;
            if (argc >= 2 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_ARRAY) {
                VMArray *arr = VM_AS_ARRAY(args[0]);
                int64_t idx = vm_to_int(args[1]);
                if (__builtin_expect(!VM_IS_UNDEF(arr->tied_obj), 0)) {
                    frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
                    if (idx < 0) idx += vm_tied_array_fetchsize(vm, arr->tied_obj);
                    if (!vm->has_pending_exc) {
                        int dfidx = vm_tied_array_find_method(vm, arr->tied_obj, "DELETE");
                        if (dfidx >= 0) {
                            VMValue iv = VM_MAKE_INT(idx);
                            deleted = vm_tied_array_call(vm, arr->tied_obj, dfidx, &iv, 1);
                        }
                    }
                    vm->gc_pause--;
                    stack = vm->stack; RELOAD();
                    if (__builtin_expect(vm->has_pending_exc, 0)) {
                        throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                        goto L_builtin_throw_exc;
                    }
                    SP_PUSH(deleted);
                    break;
                }
                if (idx < 0) idx += arr->size;
                if (idx >= 0 && idx < arr->size) {
                    if (!arr->holes || !arr->holes[idx]) {
                        deleted = arr->items[idx];
                    }
                    if (!arr->holes) arr->holes = calloc((size_t)arr->cap, sizeof(uint8_t));
                    arr->items[idx] = VM_UNDEF_VAL;
                    arr->holes[idx] = 1;
                    if (idx == arr->size - 1) {
                        while (arr->size > 0 && arr->holes[arr->size - 1]) {
                            arr->holes[arr->size - 1] = 0;
                            arr->size--;
                        }
                    }
                }
            }
            SP_PUSH(deleted);
            break;
        }
        case BUILTIN_ARRAY_UNSHIFT_LIST: {
            if (argc >= 2 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_ARRAY &&
                VM_IS_PTR(args[1]) && VM_PTR_TYPE(args[1]) == VM_OBJ_ARRAY) {
                VMArray *dst = VM_AS_ARRAY(args[0]);
                VMArray *src = VM_AS_ARRAY(args[1]);
                VMValue *titems = NULL;
                int n = src->size;
                if (__builtin_expect(!VM_IS_UNDEF(src->tied_obj), 0)) {
                    frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
                    int64_t ssz = vm_tied_array_fetchsize(vm, src->tied_obj);
                    if (vm->has_pending_exc) {
                        vm->gc_pause--; stack = vm->stack; RELOAD();
                        throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                        goto L_builtin_throw_exc;
                    }
                    n = (ssz > 0) ? (int)ssz : 0;
                    if (n > 0) {
                        titems = malloc((size_t)n * sizeof(VMValue));
                        int ffidx = vm_tied_array_find_method(vm, src->tied_obj, "FETCH");
                        for (int i = 0; i < n; i++) {
                            VMValue iv = VM_MAKE_INT(i);
                            titems[i] = (ffidx >= 0) ? vm_tied_array_call(vm, src->tied_obj, ffidx, &iv, 1) : VM_UNDEF_VAL;
                            if (vm->has_pending_exc) {
                                free(titems);
                                vm->gc_pause--; stack = vm->stack; RELOAD();
                                throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                                goto L_builtin_throw_exc;
                            }
                        }
                    }
                    vm->gc_pause--;
                    stack = vm->stack; RELOAD();
                }
                if (n > 0) {
                    const VMValue *src_items = titems ? titems : src->items;
                    if (__builtin_expect(!VM_IS_UNDEF(dst->tied_obj), 0)) {
                        int ufidx = vm_tied_array_find_method(vm, dst->tied_obj, "UNSHIFT");
                        if (ufidx >= 0) {
                            frame->ip = ip; vm->stack_top = sp;
                            vm_tied_array_call(vm, dst->tied_obj, ufidx, (VMValue *)src_items, n);
                            stack = vm->stack; RELOAD();
                            free(titems);
                            if (__builtin_expect(vm->has_pending_exc, 0)) {
                                throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                                goto L_builtin_throw_exc;
                            }
                        } else {
                            free(titems);
                        }
                        break;
                    }
                    VMValue sbuf[16];
                    VMValue *snap = NULL;
                    if (!titems && src == dst) {
                        snap = (n > 16) ? malloc((size_t)n * sizeof(VMValue)) : sbuf;
                        for (int i = 0; i < n; i++) snap[i] = vm_val_copy(src->items[i]);
                        src_items = snap;
                    }
                    int old_sz = dst->size;
                    int need = old_sz + n;
                    if (need > dst->cap) {
                        int old_cap = dst->cap;
                        int new_cap = dst->cap < 1024 ? dst->cap * 2 : dst->cap + dst->cap / 2;
                        while (new_cap < need) new_cap = (new_cap < 1024) ? new_cap * 2 : new_cap + new_cap / 2;
                        dst->cap = new_cap;
                        dst->items = realloc(dst->items, (size_t)dst->cap * sizeof(VMValue));
                        if (dst->holes) {
                            dst->holes = realloc(dst->holes, (size_t)dst->cap);
                            if (dst->cap > old_cap) memset(dst->holes + old_cap, 0, (size_t)(dst->cap - old_cap));
                        }
                    }
                    if (old_sz > 0) {
                        memmove(dst->items + n, dst->items, (size_t)old_sz * sizeof(VMValue));
                        if (dst->holes) memmove(dst->holes + n, dst->holes, (size_t)old_sz);
                    }
                    if (dst->holes) memset(dst->holes, 0, (size_t)n);
                    for (int i = 0; i < n; i++) {
                        dst->items[i] = vm_val_copy(src_items[i]);
                    }
                    dst->size = need;
                    if (snap && snap != sbuf) free(snap);
                    free(titems);
                }
            }
            /* Note: vm_compiler.c immediately emits OP_PUSH_UNDEF */
            break;
        }

        case BUILTIN_REGEX_BYTE_SLICE: {
            if (argc >= 3) {
                char sbuf[256];
                size_t slen = 0;
                const char *s = vm_bytes_view(args[0], sbuf, sizeof(sbuf), &slen);
                int64_t off = vm_to_int(args[1]);
                int64_t rlen = vm_to_int(args[2]);
                if (off < 0) off = 0;
                if ((size_t)off > slen) off = (int64_t)slen;
                if (rlen < 0) rlen = 0;
                if ((size_t)rlen > slen - (size_t)off) rlen = (int64_t)(slen - (size_t)off);
                SP_PUSH(vm_make_str_flag(s + off, (size_t)rlen, vm_is_utf8(args[0])));
            } else SP_PUSH(vm_str(""));
            break;
        }

        case BUILTIN_SUBSTR_BYTES: {
            if (argc >= 2) {
                StradaValue *sv = vm_to_sv(args[0]);
                int64_t off = vm_to_int(args[1]);
                int64_t len = (argc >= 3) ? vm_to_int(args[2]) : -1;
                StradaValue *r = strada_substr_bytes(sv, off, len);
                strada_decref(sv);
                SP_PUSH(r ? sv_to_vm(r) : vm_str(""));
                if (r) strada_decref(r);
            } else SP_PUSH(vm_str(""));
            break;
        }

        case BUILTIN_BYTE_AT: {
            if (argc >= 2) {
                StradaValue *sv0 = vm_to_sv(args[0]);
                StradaValue *sv1 = vm_to_sv(args[1]);
                StradaValue *r = strada_byte_at(sv0, sv1);
                strada_decref(sv0);
                strada_decref(sv1);
                SP_PUSH(r ? sv_to_vm(r) : VM_MAKE_INT(0));
                if (r) strada_decref(r);
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }

        case BUILTIN_CLONE: {
            SP_PUSH(argc >= 1 ? vm_clone_val(vm, args[0]) : VM_UNDEF_VAL);
            break;
        }

        case BUILTIN_VEC_GET: {
            if (argc >= 3) {
                StradaValue *sv = vm_to_sv(args[0]);
                int64_t off = vm_to_int(args[1]);
                int bits = (int)vm_to_int(args[2]);
                int64_t r = strada_vec_get(sv, off, bits);
                strada_decref(sv);
                SP_PUSH(VM_MAKE_INT(r));
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }

        case BUILTIN_VEC_SET: {
            if (argc >= 4) {
                StradaValue *sv = vm_to_sv(args[0]);
                int64_t off = vm_to_int(args[1]);
                int bits = (int)vm_to_int(args[2]);
                int64_t val = vm_to_int(args[3]);
                strada_vec_set_slot(&sv, off, bits, val);
                VMValue updated = sv ? sv_to_vm(sv) : vm_str("");
                if (sv) strada_decref(sv);
                SP_PUSH(updated);
            } else SP_PUSH(VM_UNDEF_VAL);
            break;
        }

        case BUILTIN_DEREF_SET: {
            if (argc >= 2) {
                VMValue refv = args[0];
                VMValue newv = args[1];
                if (VM_IS_PTR(refv) && VM_PTR_TYPE(refv) == VM_OBJ_SREF) {
                    VMScalarRef *sr = VM_AS_SREF(refv);
                    if (sr->global_name) {
                        VMValue tv = vm_globals_get(vm, sr->global_name);
                        if (__builtin_expect(VM_IS_PTR(tv) && VM_PTR_TYPE(tv) == VM_OBJ_TIED_SCALAR, 0)) {
                            frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
                            vm_tied_scalar_store(vm, tv, newv);
                            vm->gc_pause--;
                            stack = vm->stack; RELOAD();
                            if (vm->has_pending_exc) {
                                for (int _ai = 0; _ai < argc; _ai++) {
                                    if (VM_IS_PTR(args[_ai]) && (VM_PTR_TYPE(args[_ai]) == VM_OBJ_STR || VM_PTR_TYPE(args[_ai]) == VM_OBJ_STRBUF))
                                        vm_val_free(&args[_ai]);
                                }
                                if (args != args_stk) free(args);
                                throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                                goto L_vm_throw_exc;
                            }
                        } else {
                            vm_globals_set(vm, sr->global_name, vm_val_copy(newv));
                        }
                    } else if (VM_IS_PTR(sr->cell) && VM_PTR_TYPE(sr->cell) == VM_OBJ_CELL) {
                        VMValue old_cv = VM_AS_CELL(sr->cell)->val;
                        if (__builtin_expect(VM_IS_PTR(old_cv) && VM_PTR_TYPE(old_cv) == VM_OBJ_TIED_SCALAR, 0)) {
                            frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
                            vm_tied_scalar_store(vm, old_cv, newv);
                            vm->gc_pause--;
                            stack = vm->stack; RELOAD();
                            if (vm->has_pending_exc) {
                                for (int _ai = 0; _ai < argc; _ai++) {
                                    if (VM_IS_PTR(args[_ai]) && (VM_PTR_TYPE(args[_ai]) == VM_OBJ_STR || VM_PTR_TYPE(args[_ai]) == VM_OBJ_STRBUF))
                                        vm_val_free(&args[_ai]);
                                }
                                if (args != args_stk) free(args);
                                throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                                goto L_vm_throw_exc;
                            }
                        } else {
                            vm_temp_free(&old_cv);
                            VM_AS_CELL(sr->cell)->val = vm_val_copy(newv);
                        }
                    }
                } else if (VM_IS_PTR(refv) && VM_PTR_TYPE(refv) == VM_OBJ_CELL) {
                    VMValue old_cv = VM_AS_CELL(refv)->val;
                    if (__builtin_expect(VM_IS_PTR(old_cv) && VM_PTR_TYPE(old_cv) == VM_OBJ_TIED_SCALAR, 0)) {
                        frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
                        vm_tied_scalar_store(vm, old_cv, newv);
                        vm->gc_pause--;
                        stack = vm->stack; RELOAD();
                        if (vm->has_pending_exc) {
                            for (int _ai = 0; _ai < argc; _ai++) {
                                if (VM_IS_PTR(args[_ai]) && (VM_PTR_TYPE(args[_ai]) == VM_OBJ_STR || VM_PTR_TYPE(args[_ai]) == VM_OBJ_STRBUF))
                                    vm_val_free(&args[_ai]);
                            }
                            if (args != args_stk) free(args);
                            throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                            goto L_vm_throw_exc;
                        }
                    } else {
                        vm_temp_free(&old_cv);
                        VM_AS_CELL(refv)->val = vm_val_copy(newv);
                    }
                } else if (VM_IS_PTR(refv) && VM_PTR_TYPE(refv) == VM_OBJ_NATIVE_SV) {
                    StradaValue *nsv = ((VMNativeSV *)VM_TO_PTR(refv))->sv;
                    if (nsv) {
                        StradaValue *nv = vm_to_sv(newv);
                        (void)strada_deref_set(nsv, nv);
                        if (nv) strada_decref(nv);
                    }
                }
                SP_PUSH(vm_val_copy(newv));
            } else {
                SP_PUSH(VM_UNDEF_VAL);
            }
            break;
        }

        case BUILTIN_DEREF_ARRAY:
        case BUILTIN_DEREF_HASH: {
            int want_hash = (bid == BUILTIN_DEREF_HASH);
            VMValue cur = (argc >= 1) ? args[0] : VM_UNDEF_VAL;
            int depth = 0;
            while (VM_IS_PTR(cur)) {
                int t = VM_PTR_TYPE(cur);
                if (t == VM_OBJ_SREF || t == VM_OBJ_CELL || t == VM_OBJ_WEAKREF || t == VM_OBJ_TIED_SCALAR) {
                    if (++depth > 1024) {
                        for (int _ai = 0; _ai < argc; _ai++) {
                            if (VM_IS_PTR(args[_ai]) && (VM_PTR_TYPE(args[_ai]) == VM_OBJ_STR || VM_PTR_TYPE(args[_ai]) == VM_OBJ_STRBUF))
                                vm_val_free(&args[_ai]);
                        }
                        if (args != args_stk) free(args);
                        throw_exc = vm_str("Circular reference in deref");
                        goto L_vm_throw_exc;
                    }
                    if (t == VM_OBJ_SREF) {
                        VMScalarRef *sr = VM_AS_SREF(cur);
                        cur = sr->global_name ? vm_globals_get(vm, sr->global_name)
                                              : ((VM_IS_PTR(sr->cell) && VM_PTR_TYPE(sr->cell) == VM_OBJ_CELL) ? VM_AS_CELL(sr->cell)->val : VM_UNDEF_VAL);
                    } else if (t == VM_OBJ_CELL) {
                        cur = VM_AS_CELL(cur)->val;
                    } else if (t == VM_OBJ_WEAKREF) {
                        cur = VM_AS_WEAKREF(cur)->target;
                    } else {
                        frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
                        cur = vm_tied_scalar_fetch(vm, cur);
                        vm->gc_pause--;
                        stack = vm->stack; RELOAD();
                        if (vm->has_pending_exc) {
                            for (int _ai = 0; _ai < argc; _ai++) {
                                if (VM_IS_PTR(args[_ai]) && (VM_PTR_TYPE(args[_ai]) == VM_OBJ_STR || VM_PTR_TYPE(args[_ai]) == VM_OBJ_STRBUF))
                                    vm_val_free(&args[_ai]);
                            }
                            if (args != args_stk) free(args);
                            throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                            goto L_vm_throw_exc;
                        }
                    }
                } else break;
            }
            if (VM_IS_PTR(cur) && VM_PTR_TYPE(cur) == (want_hash ? VM_OBJ_HASH : VM_OBJ_ARRAY)) {
                SP_PUSH(vm_val_copy(cur));
            } else if (VM_IS_PTR(cur) && VM_PTR_TYPE(cur) == VM_OBJ_NATIVE_SV) {
                StradaValue *nsv = ((VMNativeSV *)VM_TO_PTR(cur))->sv;
                StradaValue *d = nsv ? (want_hash ? strada_deref_hash_value_owned(nsv) : strada_deref_array_value_owned(nsv)) : NULL;
                VMValue res = d ? sv_to_vm(d) : VM_MAKE_PTR(want_hash ? (void *)vm_hash_new(8) : (void *)vm_array_new(0));
                if (d) strada_decref(d);
                SP_PUSH(res);
            } else {
                SP_PUSH(VM_MAKE_PTR(want_hash ? (void *)vm_hash_new(8) : (void *)vm_array_new(0)));
            }
            break;
        }

        case BUILTIN_DUMP: {
            if (argc >= 1) {
                StradaValue *sv = vm_to_sv(args[0]);
                int ind = (argc >= 2) ? (int)vm_to_int(args[1]) : 0;
                strada_dump(sv, ind);
                strada_decref(sv);
            }
            SP_PUSH(VM_UNDEF_VAL);
            break;
        }

        case BUILTIN_RESERVE: {
            if (argc >= 2) {
                int64_t cap = vm_to_int(args[1]);
                VMValue cur = args[0];
                for (int d = 0; d < 64 && VM_IS_PTR(cur); d++) {
                    int t = VM_PTR_TYPE(cur);
                    if (t == VM_OBJ_SREF) {
                        VMScalarRef *sr = VM_AS_SREF(cur);
                        cur = sr->global_name ? vm_globals_get(vm, sr->global_name)
                                              : ((VM_IS_PTR(sr->cell) && VM_PTR_TYPE(sr->cell) == VM_OBJ_CELL) ? VM_AS_CELL(sr->cell)->val : VM_UNDEF_VAL);
                    } else if (t == VM_OBJ_CELL) {
                        cur = VM_AS_CELL(cur)->val;
                    } else if (t == VM_OBJ_WEAKREF) {
                        cur = VM_AS_WEAKREF(cur)->target;
                    } else break;
                }
                if (cap > 0 && VM_IS_PTR(cur)) {
                    int ct = VM_PTR_TYPE(cur);
                    if (ct == VM_OBJ_ARRAY) {
                        VMArray *a = VM_AS_ARRAY(cur);
                        if (cap > a->cap && cap <= INT_MAX / 2) {
                            int old_cap = a->cap;
                            int ncap = (int)cap;
                            VMValue *ni = realloc(a->items, (size_t)ncap * sizeof(VMValue));
                            if (ni) {
                                a->items = ni;
                                if (a->holes) {
                                    uint8_t *nh = realloc(a->holes, (size_t)ncap);
                                    if (nh) {
                                        memset(nh + old_cap, 0, (size_t)(ncap - old_cap));
                                        a->holes = nh;
                                    }
                                }
                                a->cap = ncap;
                            }
                        }
                    } else if (ct == VM_OBJ_HASH) {
                        VMHash *h = VM_AS_HASH(cur);
                        while (cap * 4 >= (int64_t)h->capacity * 3 && h->capacity < (1 << 28)) {
                            vm_hash_grow(h);
                        }
                    } else if (ct == VM_OBJ_NATIVE_SV) {
                        StradaValue *nsv = ((VMNativeSV *)VM_TO_PTR(cur))->sv;
                        if (nsv) strada_reserve_sv(nsv, cap);
                    }
                }
            }
            SP_PUSH(VM_UNDEF_VAL);
            break;
        }

        case BUILTIN_CORE_WRITE_FD: {
            if (argc >= 2) {
                StradaValue *sv0 = vm_to_sv(args[0]);
                StradaValue *sv1 = vm_to_sv(args[1]);
                StradaValue *r = strada_write_fd(sv0, sv1);
                strada_decref(sv0);
                strada_decref(sv1);
                SP_PUSH(r ? sv_to_vm(r) : VM_MAKE_INT(-1));
                if (r) strada_decref(r);
            } else SP_PUSH(VM_MAKE_INT(-1));
            break;
        }

        case BUILTIN_CORE_CLOSE_FD: {
            if (argc >= 1) {
                StradaValue *sv0 = vm_to_sv(args[0]);
                StradaValue *r = strada_close_fd(sv0);
                strada_decref(sv0);
                SP_PUSH(r ? sv_to_vm(r) : VM_MAKE_INT(-1));
                if (r) strada_decref(r);
            } else SP_PUSH(VM_MAKE_INT(-1));
            break;
        }

        case BUILTIN_STACKTRACE: {
            fprintf(stderr, "Stack trace:\n");
            for (int i = vm->frame_count - 1; i >= 0; i--) {
                VMFrame *f = &vm->frames[i];
                const char *fname = (f->chunk && f->chunk->name) ? f->chunk->name : "(unknown)";
                fprintf(stderr, "  %s\n", fname);
            }
            SP_PUSH(VM_UNDEF_VAL);
            break;
        }

        case BUILTIN_CORE_GC_COLLECT: {
            frame->ip = ip; vm->stack_top = sp;
            vm_gc_collect_ex(vm, (vm->exc_top > 0) ? 1 : 0);
            stack = vm->stack; RELOAD();
            if (__builtin_expect(vm->has_pending_exc, 0)) {
                throw_exc = vm->pending_exc;
                vm->pending_exc = VM_UNDEF_VAL;
                vm->has_pending_exc = 0;
                goto L_builtin_throw_exc;
            }
            SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_CAPTURE: {
            VMArray *ca = vm_array_new(10);
            if (argc >= 2) {
                char sbuf[256], pbuf[256];
                size_t slen = 0, plen = 0;
                const char *s = vm_bytes_view(args[0], sbuf, sizeof(sbuf), &slen);
                const char *pat = vm_bytes_view(args[1], pbuf, sizeof(pbuf), &plen);
                if (vm_regex_match_len_u8(vm, s, slen, vm_is_utf8(args[0]), pat, plen)) {
                    int last = -1;
                    for (int i = 0; i < 10; i++) {
                        if (vm->regex_captures[i]) last = i;
                    }
                    for (int i = 0; i <= last; i++) {
                        vm_array_push(ca, vm->regex_captures[i]
                            ? vm_make_str_flag(vm->regex_captures[i], vm->regex_capture_len[i], vm->regex_capture_utf8[i])
                            : VM_UNDEF_VAL);
                    }
                }
            }
            SP_PUSH(VM_MAKE_PTR(ca));
            break;
        }

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
    L_builtin_throw_exc:
        for (int _ai = 0; _ai < argc; _ai++) {
            if (VM_IS_PTR(args[_ai]) && (VM_PTR_TYPE(args[_ai]) == VM_OBJ_STR || VM_PTR_TYPE(args[_ai]) == VM_OBJ_STRBUF))
                vm_val_free(&args[_ai]);
        }
        if (args != args_stk) free(args);
        goto L_vm_throw_exc;
    }

    CASE(OP_CALL_NATIVE): {
        uint16_t native_idx = read_u16(ip); ip += 2;
        uint8_t argc = *ip++;
        VMNativeEntry *ne = &vm->program->natives[native_idx];
        uint8_t ncall_ctx = vm->pending_call_context;
        vm->pending_call_context = 0;
        VMValue raw_buf[16];
        VMValue *raw_args = (argc > 16) ? malloc((size_t)argc * sizeof(VMValue)) : raw_buf;
        for (int i = argc - 1; i >= 0; i--) raw_args[i] = SP_POP();
        frame->ip = ip; vm->stack_top = sp;
        int prev_c_ctx = strada_wantarray() ? 1 : (strada_wanthash() ? 2 : 0);
        strada_set_call_context((int)ncall_ctx);
        VMValue ret = vm_invoke_native_entry(vm, ne, raw_args, argc, NULL);
        strada_set_call_context(prev_c_ctx);
        stack = vm->stack;
        RELOAD();
        if (raw_args != raw_buf) free(raw_args);
        if (__builtin_expect(vm->has_pending_exc, 0)) {
            throw_exc = vm->pending_exc;
            vm->pending_exc = VM_UNDEF_VAL;
            vm->has_pending_exc = 0;
            goto L_vm_throw_exc;
        }
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
        if (!__atomic_load_n(&cb->fn, __ATOMIC_ACQUIRE)) {
            pthread_mutex_lock(&g_cblock_jit_mutex);
            int jit_fail = (!cb->fn && cblock_jit_compile(cb, vm->program->runtime_include_path, vm->program->cblock_prelude) != 0);
            pthread_mutex_unlock(&g_cblock_jit_mutex);
            if (jit_fail) {
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
        frame->ip = ip; vm->stack_top = sp;
        for (int i = 0; i < cb->var_count; i++) {
            int slot = cb->var_slots[i];
            VMValue raw_v = locals[slot];
            if (VM_IS_PTR(raw_v) && VM_PTR_TYPE(raw_v) == VM_OBJ_CELL) raw_v = VM_AS_CELL(raw_v)->val;
            orig_vars[i] = raw_v;
            sv_vars[i] = vm_to_sv(orig_vars[i]);
            orig_sv[i] = sv_vars[i];
            if (__builtin_expect(vm->has_pending_exc, 0)) {
                for (int k = 0; k <= i; k++) {
                    if (sv_vars[k]) strada_decref(sv_vars[k]);
                }
                if (orig_vars != orig_buf) { free(orig_vars); free(sv_vars); free(orig_sv); }
                stack = vm->stack;
                RELOAD();
                VM_CHECK_EXC();
            }
        }

        /* Call the compiled C block */
        if (vm_c_call_cblock(vm, (void(*)(StradaValue**))cb->fn, sv_vars, &throw_exc)) {
            stack = vm->stack;
            RELOAD();
            for (int i = 0; i < cb->var_count; i++) {
                if (VM_IS_PTR(orig_vars[i]) && VM_PTR_TYPE(orig_vars[i]) == VM_OBJ_FILEHANDLE)
                    vm_fh_sync_from_sv(orig_vars[i]);
                if (sv_vars[i]) strada_decref(sv_vars[i]);
            }
            if (orig_vars != orig_buf) { free(orig_vars); free(sv_vars); free(orig_sv); }
            goto L_vm_throw_exc;
        }
        stack = vm->stack;
        RELOAD();

        /* Marshal back: StradaValue* → VMValue */
        for (int i = 0; i < cb->var_count; i++) {
            int slot = cb->var_slots[i];
            VMValue *dst = &locals[slot];
            if (VM_IS_PTR(*dst) && VM_PTR_TYPE(*dst) == VM_OBJ_CELL) dst = &VM_AS_CELL(*dst)->val;
            if (VM_IS_PTR(orig_vars[i]) && VM_PTR_TYPE(orig_vars[i]) == VM_OBJ_FILEHANDLE) {
                vm_fh_sync_from_sv(orig_vars[i]);
                if (sv_vars[i] == orig_sv[i]) {
                    *dst = orig_vars[i];
                    if (sv_vars[i]) strada_decref(sv_vars[i]);
                    continue;
                }
            }
            if (VM_IS_PTR(orig_vars[i]) && VM_PTR_TYPE(orig_vars[i]) == VM_OBJ_TIED_SCALAR) {
                *dst = orig_vars[i];
                if (sv_vars[i] != orig_sv[i]) {
                    frame->ip = ip; vm->stack_top = sp;
                    VMValue newv = sv_to_vm(sv_vars[i]);
                    if (sv_vars[i]) { strada_decref(sv_vars[i]); sv_vars[i] = NULL; }
                    vm_tied_scalar_store(vm, orig_vars[i], newv);
                    stack = vm->stack; RELOAD();
                    if (__builtin_expect(vm->has_pending_exc, 0)) {
                        for (int k = i + 1; k < cb->var_count; k++) {
                            if (sv_vars[k]) strada_decref(sv_vars[k]);
                        }
                        if (orig_vars != orig_buf) { free(orig_vars); free(sv_vars); free(orig_sv); }
                        VM_CHECK_EXC();
                    }
                } else {
                    if (sv_vars[i]) strada_decref(sv_vars[i]);
                }
                continue;
            }
            if (sv_vars[i] == orig_sv[i] &&
                VM_IS_PTR(orig_vars[i]) && VM_PTR_TYPE(orig_vars[i]) == VM_OBJ_ARRAY &&
                sv_vars[i] && !STRADA_IS_TAGGED_INT(sv_vars[i]) && sv_vars[i]->type == STRADA_ARRAY) {
                VMArray *va = VM_AS_ARRAY(orig_vars[i]);
                StradaArray *sav = sv_vars[i]->value.av;
                int n = sav ? (int)sav->size : 0;
                if (n > va->cap) {
                    int ncap = va->cap < 8 ? 8 : va->cap;
                    while (ncap < n) ncap *= 2;
                    va->items = realloc(va->items, (size_t)ncap * sizeof(VMValue));
                    if (va->holes) va->holes = realloc(va->holes, (size_t)ncap);
                    va->cap = ncap;
                }
                va->size = n;
                if (va->holes && n > 0) memset(va->holes, 0, (size_t)n);
                for (int k = 0; k < n; k++) {
                    va->items[k] = sv_to_vm(strada_array_get(sav, (int64_t)k));
                }
                *dst = orig_vars[i];
                strada_decref(sv_vars[i]);
                continue;
            }
            if (sv_vars[i] == orig_sv[i] &&
                VM_IS_PTR(orig_vars[i]) && VM_PTR_TYPE(orig_vars[i]) == VM_OBJ_HASH &&
                sv_vars[i] && !STRADA_IS_TAGGED_INT(sv_vars[i]) && sv_vars[i]->type == STRADA_HASH) {
                VMHash *vh = VM_AS_HASH(orig_vars[i]);
                StradaHash *shv = sv_vars[i]->value.hv;
                for (int k = 0; k < vh->capacity; k++) {
                    if (vh->entries[k].occupied == 1) {
                        key_free(vh->entries[k].key);
                        vm_val_free(&vh->entries[k].value);
                    }
                }
                memset(vh->entries, 0, (size_t)vh->capacity * sizeof(VMHashEntry));
                vh->size = 0;
                vh->iter_pos = 0;
                if (shv && shv->entries) {
                    for (size_t k = 0; k < shv->next_slot; k++) {
                        if (!shv->entries[k].key) continue;
                        vm_hash_set_n(vh, shv->entries[k].key->data, shv->entries[k].key->len,
                                      sv_to_vm(shv->entries[k].value));
                    }
                }
                const char *bpkg = strada_blessed_name_cstr(sv_vars[i]);
                if (bpkg && (!vh->class_name || strcmp(vh->class_name, bpkg) != 0)) {
                    free(vh->class_name);
                    vh->class_name = strdup(bpkg);
                } else if (!bpkg && vh->class_name) {
                    free(vh->class_name);
                    vh->class_name = NULL;
                }
                *dst = orig_vars[i];
                strada_decref(sv_vars[i]);
                continue;
            }
            *dst = sv_to_vm(sv_vars[i]);
            if (sv_vars[i]) strada_decref(sv_vars[i]);
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
    #undef VM_CHECK_EXC
    #undef VM_VALUE_IS_TRUTHY
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
    if (hash_sv->type == STRADA_REF && !strada_is_slot_ref(hash_sv) && hash_sv->value.rv) hash_sv = hash_sv->value.rv;
    if (!hash_sv || STRADA_IS_TAGGED_INT(hash_sv) || hash_sv->type != STRADA_HASH || !hash_sv->value.hv || !hash_sv->value.hv->entries) return;
    if (skip_hash_sv && !STRADA_IS_TAGGED_INT(skip_hash_sv) && skip_hash_sv->type == STRADA_REF && !strada_is_slot_ref(skip_hash_sv) && skip_hash_sv->value.rv) {
        skip_hash_sv = skip_hash_sv->value.rv;
    }
    StradaHash *hv = hash_sv->value.hv;
    StradaHash *skip = (skip_hash_sv && !STRADA_IS_TAGGED_INT(skip_hash_sv) && skip_hash_sv->type == STRADA_HASH) ? skip_hash_sv->value.hv : NULL;
    VM *prev_vm = g_active_vm;
    g_active_vm = vm;
    for (size_t i = 0; i < hv->next_slot; i++) {
        if (!hv->entries[i].key) continue;
        const char *k = hv->entries[i].key->data;
        size_t klen = hv->entries[i].key->len;
        if (skip) {
            StradaValue *ksv = strada_new_str_len(k, klen);
            int is_skipped = strada_hash_exists_sv(skip, ksv);
            strada_decref(ksv);
            if (is_skipped) continue;
        }
        vm_globals_set_n(vm, k, klen, sv_to_vm(hv->entries[i].value));
    }
    g_active_vm = prev_vm;
}

void vm_export_globals(VM *vm, StradaValue *hash_sv) {
    if (!vm || !vm->globals || !hash_sv || STRADA_IS_TAGGED_INT(hash_sv)) return;
    if (hash_sv->type == STRADA_REF && !strada_is_slot_ref(hash_sv) && hash_sv->value.rv) hash_sv = hash_sv->value.rv;
    if (!hash_sv || STRADA_IS_TAGGED_INT(hash_sv) || hash_sv->type != STRADA_HASH || !hash_sv->value.hv) return;
    VM *prev_vm = g_active_vm;
    g_active_vm = vm;
    pthread_mutex_t *glk = vm_globals_lock_ptr(vm);
    if (glk) pthread_mutex_lock(glk);
    VMHash *g = vm->globals;
    size_t snap_cap = g ? (size_t)g->capacity : 0;
    char **snap_k = snap_cap ? calloc(snap_cap, sizeof(char *)) : NULL;
    size_t *snap_kl = snap_cap ? calloc(snap_cap, sizeof(size_t)) : NULL;
    VMValue *snap_v = snap_cap ? calloc(snap_cap, sizeof(VMValue)) : NULL;
    size_t snap_n = 0;
    if (g && snap_k && snap_kl && snap_v) {
        for (int i = 0; i < g->capacity; i++) {
            if (g->entries[i].occupied == 1 && g->entries[i].key) {
                size_t kl = g->entries[i].keylen;
                char *kc = malloc(kl + 1);
                if (!kc) continue;
                memcpy(kc, g->entries[i].key, kl);
                kc[kl] = '\0';
                snap_k[snap_n] = kc;
                snap_kl[snap_n] = kl;
                snap_v[snap_n] = g->entries[i].value;
                snap_n++;
            }
        }
    }
    if (glk) pthread_mutex_unlock(glk);
    for (size_t i = 0; i < snap_n; i++) {
        StradaValue *sv = vm_to_sv(snap_v[i]);
        if (vm->has_pending_exc) {
            vm->has_pending_exc = 0;
            vm_temp_free(&vm->pending_exc);
            vm->pending_exc = VM_UNDEF_VAL;
            if (sv) strada_decref(sv);
            free(snap_k[i]);
            continue;
        }
        StradaValue *ksv = strada_new_str_len(snap_k[i], snap_kl[i]);
        strada_hash_set_sv(hash_sv->value.hv, ksv, sv);
        strada_decref(ksv);
        if (sv) strada_decref(sv);
        free(snap_k[i]);
    }
    free(snap_k);
    free(snap_kl);
    free(snap_v);
    g_active_vm = prev_vm;
}

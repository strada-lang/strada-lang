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

#if defined(__x86_64__) && (defined(__linux__) || defined(__APPLE__) || defined(__FreeBSD__)) && !defined(VM_NO_JIT)
#define VM_JIT_X64 1
#include <sys/mman.h>
#endif

#include "vm_generic_builtins.inc"

int vm_generic_builtin_find(const char *name, int argc) {
    for (int i = 0; i < VM_GENERIC_BUILTIN_COUNT; i++) {
        if (vm_generic_builtins[i].argc == argc &&
            strcmp(vm_generic_builtins[i].name, name) == 0)
            return i;
    }
    return -1;
}

/* Like vm_generic_builtin_find, but also accepts an entry taking MORE
 * parameters (the missing trailing ones are passed as NULL). */
int vm_generic_builtin_find_min(const char *name, int argc) {
    int exact = vm_generic_builtin_find(name, argc);
    if (exact >= 0) return exact;
    for (int i = 0; i < VM_GENERIC_BUILTIN_COUNT; i++) {
        if (vm_generic_builtins[i].argc > argc &&
            strcmp(vm_generic_builtins[i].name, name) == 0)
            return i;
    }
    return -1;
}
#include <signal.h>
#include <poll.h>
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
static uint32_t vm_hash_fn_n(const char *key, size_t len);
static void vm_fn_index_insert(VMProgram *p, size_t idx);
static void vm_fn_index_rebuild(VMProgram *p);

/* Matches VMConstString prefix layout so gc_immortal == 2 singletons have valid .hash */
typedef struct {
    uint32_t hash;
    uint32_t _pad;
    VMObjHeader hdr;
    uint32_t len;
    char data[8];
} VMImmortalStr1;

static VMImmortalStr1 g_empty_str_byte;
static VMImmortalStr1 g_empty_str_utf8;
static VMImmortalStr1 g_ascii_strs[2][128];
static pthread_once_t g_static_str_once = PTHREAD_ONCE_INIT;
static int g_static_str_ready = 0;
static uint32_t g_next_prog_id = 1;

static void vm_init_static_strs(void) {
    uint32_t eh = vm_hash_fn_n("", 0);
    g_empty_str_byte.hash = eh;
    g_empty_str_byte.hdr.obj_type = VM_OBJ_STR;
    g_empty_str_byte.hdr.gc_immortal = 2;
    g_empty_str_byte.hdr.is_utf8 = 0;
    g_empty_str_byte.len = 0;
    g_empty_str_utf8.hash = eh;
    g_empty_str_utf8.hdr.obj_type = VM_OBJ_STR;
    g_empty_str_utf8.hdr.gc_immortal = 2;
    g_empty_str_utf8.hdr.is_utf8 = 1;
    g_empty_str_utf8.len = 0;
    for (int u = 0; u < 2; u++) {
        for (int i = 0; i < 128; i++) {
            char ch = (char)i;
            g_ascii_strs[u][i].hash = vm_hash_fn_n(&ch, 1);
            g_ascii_strs[u][i].hdr.obj_type = VM_OBJ_STR;
            g_ascii_strs[u][i].hdr.gc_immortal = 2;
            g_ascii_strs[u][i].hdr.is_utf8 = (uint8_t)u;
            g_ascii_strs[u][i].len = 1;
            g_ascii_strs[u][i].data[0] = ch;
            g_ascii_strs[u][i].data[1] = '\0';
        }
    }
    __atomic_store_n(&g_static_str_ready, 1, __ATOMIC_RELEASE);
}

static inline VMValue vm_get_empty_str(int is_utf8) {
    if (__builtin_expect(!__atomic_load_n(&g_static_str_ready, __ATOMIC_ACQUIRE), 0)) {
        pthread_once(&g_static_str_once, vm_init_static_strs);
    }
    return VM_MAKE_PTR(is_utf8 ? &g_empty_str_utf8.hdr : &g_empty_str_byte.hdr);
}

static inline VMValue vm_get_ascii_str(unsigned char ch, int is_utf8) {
    if (__builtin_expect(!__atomic_load_n(&g_static_str_ready, __ATOMIC_ACQUIRE), 0)) {
        pthread_once(&g_static_str_once, vm_init_static_strs);
    }
    return VM_MAKE_PTR(&g_ascii_strs[is_utf8 ? 1 : 0][ch].hdr);
}

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

static inline void vm_gc_init_hdr_cls(VMObjHeader *hdr, enum VMObjType type, uint8_t pclass) {
    hdr->obj_type = (uint8_t)type;
    hdr->gc_marked = 0;
    hdr->is_utf8 = 0;
    hdr->gc_finalized = 0;
    hdr->pool_class = pclass;
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

void vm_gc_init_hdr(VMObjHeader *hdr, enum VMObjType type) {
    vm_gc_init_hdr_cls(hdr, type, VM_POOL_NONE);
}

static inline VMObjHeader *vm_pool_pop(uint8_t pclass, size_t sz, enum VMObjType type, int zero_rest) {
    VM *avm = g_active_vm;
    VMObjHeader *hdr = NULL;
    uint8_t assigned_cls = VM_POOL_NONE;
    if (__builtin_expect(avm && !avm->is_worker_vm && avm->obj_pools[pclass], 1)) {
        hdr = avm->obj_pools[pclass];
        avm->obj_pools[pclass] = hdr->gc_next;
        avm->obj_pool_len[pclass]--;
        assigned_cls = pclass;
        if (zero_rest) memset((char*)hdr + sizeof(VMObjHeader), 0, sz - sizeof(VMObjHeader));
    } else {
        hdr = (VMObjHeader*)malloc(sz);
        if (!hdr) { fprintf(stderr, "VM: out of memory\n"); exit(1); }
        if (avm) assigned_cls = pclass;
        if (zero_rest) memset((char*)hdr + sizeof(VMObjHeader), 0, sz - sizeof(VMObjHeader));
    }
    vm_gc_init_hdr_cls(hdr, type, assigned_cls);
    return hdr;
}

static inline VMString *vm_str_alloc(size_t len) {
    if (__builtin_expect(len <= VM_STR_SLAB_MAX, 1)) {
        return (VMString*)vm_pool_pop(VM_POOL_STR32,
            sizeof(VMString) + VM_STR_SLAB_MAX + 1, VM_OBJ_STR, 0);
    }
    VMString *vs = (VMString*)malloc(sizeof(VMString) + len + 1);
    if (!vs) { fprintf(stderr, "VM: out of memory\n"); exit(1); }
    vm_gc_init_hdr_cls(&vs->hdr, VM_OBJ_STR, VM_POOL_NONE);
    return vs;
}

static inline VMCell *vm_cell_alloc(void) {
    return (VMCell*)vm_pool_pop(VM_POOL_CELL, sizeof(VMCell), VM_OBJ_CELL, 1);
}

static inline VMScalarRef *vm_sref_alloc(void) {
    return (VMScalarRef*)vm_pool_pop(VM_POOL_SREF, sizeof(VMScalarRef), VM_OBJ_SREF, 1);
}

static inline VMClosure *vm_closure_alloc(void) {
    return (VMClosure*)vm_pool_pop(VM_POOL_CLOSURE, sizeof(VMClosure), VM_OBJ_CLOSURE, 1);
}

static inline VMWeakRef *vm_weakref_alloc(void) {
    return (VMWeakRef*)vm_pool_pop(VM_POOL_WEAKREF, sizeof(VMWeakRef), VM_OBJ_WEAKREF, 1);
}

VMValue vm_make_str_n(const char *s, size_t len) {
    if (len == 0) return vm_get_empty_str(0);
    if (len == 1 && (unsigned char)s[0] < 128) return vm_get_ascii_str((unsigned char)s[0], 0);
    VMString *vs = vm_str_alloc(len);
    vs->len = (uint32_t)len;
    if (len > 0 && s) memcpy(vs->data, s, len);
    vs->data[len] = '\0';
    return VM_MAKE_PTR(vs);
}

static void vm_gc_free_one(VM *pool_vm, VMObjHeader *hdr) {
    if (!hdr || hdr->gc_immortal >= 2) return;
    uint8_t pclass = hdr->pool_class;
    switch (hdr->obj_type) {
    case VM_OBJ_STR:
    case VM_OBJ_INT64:
    case VM_OBJ_UINT64:
    case VM_OBJ_CELL:
    case VM_OBJ_TIED_SCALAR:
    case VM_OBJ_WEAKREF:
        break;
    case VM_OBJ_STRBUF: {
        VMStrBuf *sb = (VMStrBuf*)hdr;
        free(sb->data);
        break;
    }
    case VM_OBJ_ARRAY: {
        VMArray *a = (VMArray*)hdr;
        if (a->items != a->inline_items) free(a->items);
        free(a->holes);
        free(a->class_name);
        break;
    }
    case VM_OBJ_HASH: {
        VMHash *h = (VMHash*)hdr;
        if (h->entries) {
            for (int i = 0; i < h->capacity; i++) {
                if (h->entries[i].occupied == 1 && h->entries[i].key && !h->entries[i].key_interned) {
                    free(h->entries[i].key);
                }
            }
            if (h->entries != h->inline_entries) free(h->entries);
        }
        free(h->index);
        h->index = NULL;
        free(h->class_name);
        break;
    }
    case VM_OBJ_CLOSURE: {
        VMClosure *c = (VMClosure*)hdr;
        free(c->captures);
        free(c->class_name);
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
        break;
    }
    case VM_OBJ_NATIVE_SV: {
        VMNativeSV *nsv = (VMNativeSV*)hdr;
        if (nsv->sv) {
            strada_decref(nsv->sv);
            nsv->sv = NULL;
        }
        break;
    }
    case VM_OBJ_SREF: {
        VMScalarRef *sr = (VMScalarRef*)hdr;
        free(sr->global_name);
        free(sr->class_name);
        break;
    }
    default:
        break;
    }
    if (pool_vm && !pool_vm->is_worker_vm && pclass > 0 && pclass < VM_POOL_COUNT &&
        pool_vm->obj_pool_len[pclass] < VM_POOL_MAX_PER_CLASS) {
        hdr->gc_next = pool_vm->obj_pools[pclass];
        pool_vm->obj_pools[pclass] = hdr;
        pool_vm->obj_pool_len[pclass]++;
        return;
    }
    free(hdr);
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
    for (int depth = 0; depth < 16 && VM_IS_PTR(v); depth++) {
        enum VMObjType t = VM_PTR_TYPE(v);
        if (t == VM_OBJ_CELL) v = VM_AS_CELL(v)->val;
        else if (t == VM_OBJ_WEAKREF) v = VM_AS_WEAKREF(v)->target;
        else break;
    }
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
        /* a reference's numeric value is its address (as compiled code) */
        case VM_OBJ_ARRAY: return (int64_t)(intptr_t)VM_TO_PTR(v);
        case VM_OBJ_HASH: return (int64_t)(intptr_t)VM_TO_PTR(v);
        case VM_OBJ_NATIVE_SV: return strada_to_int(((VMNativeSV*)VM_TO_PTR(v))->sv);
        default: break;
        }
    }
    return 0;
}

static inline double vm_to_num(VMValue v) {
    for (int depth = 0; depth < 16 && VM_IS_PTR(v); depth++) {
        enum VMObjType t = VM_PTR_TYPE(v);
        if (t == VM_OBJ_CELL) v = VM_AS_CELL(v)->val;
        else if (t == VM_OBJ_WEAKREF) v = VM_AS_WEAKREF(v)->target;
        else break;
    }
    if (VM_IS_INT(v)) return (double)VM_INT_VAL(v);
    if (VM_IS_FLOAT(v)) return VM_FLOAT_VAL(v);
    if (VM_IS_PTR(v)) {
        switch (VM_PTR_TYPE(v)) {
        case VM_OBJ_INT64: return (double)VM_AS_INT64(v)->val;
        case VM_OBJ_UINT64: return (double)VM_AS_UINT64(v)->val;
        case VM_OBJ_STR: return vm_perl_atof(VM_AS_STR(v)->data);
        case VM_OBJ_STRBUF: return VM_AS_STRBUF(v)->data ? vm_perl_atof(VM_AS_STRBUF(v)->data) : 0.0;
        case VM_OBJ_ARRAY:
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
        /* An array/hash value in scalar context is a reference (the
         * compiler turns @a / %h in truth context into their counts), and a
         * reference is always true (Perl), also to an empty container. */
        case VM_OBJ_ARRAY:
        case VM_OBJ_HASH:
            return 1;
        case VM_OBJ_WEAKREF: return vm_to_bool(VM_AS_WEAKREF(v)->target);
        default: return 1;
        }
    }
    return 0;
}

/* Decimal text of an int64 into buf (needs 21 bytes; falls back to snprintf
 * for smaller buffers). snprintf was 12% of bench_hotpaths. */
static inline const char *vm_i64_to_cstr(int64_t iv, char *buf, size_t bufsz) {
    if (bufsz < 21) { snprintf(buf, bufsz, "%lld", (long long)iv); return buf; }
    char tmp[21];
    int n = 0;
    uint64_t u = iv < 0 ? (uint64_t)0 - (uint64_t)iv : (uint64_t)iv;
    do { tmp[n++] = (char)('0' + (int)(u % 10)); u /= 10; } while (u);
    size_t o = 0;
    if (iv < 0) buf[o++] = '-';
    while (n) buf[o++] = tmp[--n];
    buf[o] = '\0';
    return buf;
}

static const char *vm_to_cstr(VMValue v, char *buf, size_t bufsz) {
    if (VM_IS_INT(v)) return vm_i64_to_cstr(VM_INT_VAL(v), buf, bufsz);
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
    case VM_OBJ_INT64: return vm_i64_to_cstr(VM_AS_INT64(v)->val, buf, bufsz);
    case VM_OBJ_UINT64: { snprintf(buf, bufsz, "%llu", (unsigned long long)VM_AS_UINT64(v)->val); return buf; }
    case VM_OBJ_STR: return VM_AS_STR(v)->data;
    case VM_OBJ_STRBUF: return VM_AS_STRBUF(v)->data ? VM_AS_STRBUF(v)->data : "";
    case VM_OBJ_ARRAY: {
        VMArray *a = VM_AS_ARRAY(v);
        if (a->class_name) {
            snprintf(buf, bufsz, "%s=ARRAY(0x%lx)", a->class_name, (unsigned long)(uintptr_t)a);
        } else {
            snprintf(buf, bufsz, "ARRAY(0x%lx)", (unsigned long)(uintptr_t)a);
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

static inline void vm_array_ensure_cap(VMArray *a, int new_cap) {
    int old_cap = a->cap;
    VMValue *new_items;
    if (a->items == a->inline_items) {
        new_items = malloc((size_t)new_cap * sizeof(VMValue));
        if (a->size > 0) memcpy(new_items, a->inline_items, (size_t)a->size * sizeof(VMValue));
    } else {
        new_items = realloc(a->items, (size_t)new_cap * sizeof(VMValue));
    }
    if (new_cap > a->size) {
        memset(new_items + a->size, 0, (size_t)(new_cap - a->size) * sizeof(VMValue));
    }
    a->items = new_items;
    a->cap = new_cap;
    if (a->holes) {
        a->holes = realloc(a->holes, (size_t)new_cap);
        if (new_cap > old_cap) memset(a->holes + old_cap, 0, (size_t)(new_cap - old_cap));
    }
}

VMArray *vm_array_new(int cap) {
    VMArray *a = (VMArray*)vm_pool_pop(VM_POOL_ARRAY, sizeof(VMArray), VM_OBJ_ARRAY, 0);
    a->holes = NULL;
    a->size = 0;
    a->class_id = 0;
    a->class_prog_id = 0;
    a->tied_obj = VM_UNDEF_VAL;
    a->class_name = NULL;
    if (cap <= VM_ARRAY_INLINE_CAP) {
        a->cap = VM_ARRAY_INLINE_CAP;
        a->items = a->inline_items;
    } else {
        a->cap = cap;
        a->items = calloc((size_t)cap, sizeof(VMValue));
    }
    return a;
}

void vm_array_free(VMArray *a) {
    if (a) {
        if (a->items != a->inline_items) free(a->items);
        free(a->holes);
        free(a->class_name);
        free(a);
    }
}

void vm_array_push(VMArray *a, VMValue v) {
    if (a->size >= a->cap) {
        int new_cap = a->cap < 1024 ? a->cap * 2 : a->cap + a->cap / 2;
        if (new_cap < 8) new_cap = 8;
        vm_array_ensure_cap(a, new_cap);
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
        int new_cap = a->cap;
        while (sidx >= new_cap) {
            new_cap = new_cap < 1024 ? new_cap * 2 : new_cap + new_cap / 2;
            if (new_cap < 8) new_cap = 8;
            if (new_cap <= sidx) new_cap = sidx + 1;
        }
        vm_array_ensure_cap(a, new_cap);
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

static const char *vm_intern_hash_key(VM *vm, const char *key, size_t klen, uint32_t hash) {
    if (!vm || vm->is_worker_vm || vm->workers_head || klen > VM_KEY_INTERN_MAXLEN) return NULL;
    if (!vm->key_intern) {
        vm->key_intern_cap = VM_KEY_INTERN_SLOTS;
        vm->key_intern = calloc(vm->key_intern_cap, sizeof(*vm->key_intern));
        if (!vm->key_intern) return NULL;
    }
    uint32_t mask = vm->key_intern_cap - 1;
    uint32_t idx = hash & mask;
    for (;;) {
        char *existing = vm->key_intern[idx].key;
        if (!existing) {
            if (vm->key_intern_count >= VM_KEY_INTERN_MAX) return NULL;
            char *copy = malloc(klen + 1);
            if (!copy) return NULL;
            if (klen > 0) memcpy(copy, key, klen);
            copy[klen] = '\0';
            vm->key_intern[idx].key = copy;
            vm->key_intern[idx].hash = hash;
            vm->key_intern[idx].len = (uint32_t)klen;
            vm->key_intern_count++;
            return copy;
        }
        if (vm->key_intern[idx].hash == hash && vm->key_intern[idx].len == (uint32_t)klen &&
            memcmp(existing, key, klen) == 0) {
            return existing;
        }
        idx = (idx + 1) & mask;
    }
}

static inline void key_free_entry(VMHashEntry *e) {
    if (!e->key_interned) free(e->key);
}

static int vm_utf8_is_valid_str(const char *s, size_t len, int *out_nonascii);
static inline VMValue vm_make_str_flag(const char *s, size_t len, int is_utf8);

/* Rebuild index[] from the dense entries (NULL for inline-size hashes). */
static void vm_hash_index_build(VMHash *h) {
    if (h->capacity <= VM_HASH_INLINE_CAP) {
        free(h->index);
        h->index = NULL;
        h->index_mask = 0;
        return;
    }
    uint32_t isz = 8;
    while (isz < (uint32_t)h->capacity * 2) isz <<= 1;
    if (!h->index || h->index_mask + 1 != isz) {
        free(h->index);
        h->index = malloc((size_t)isz * sizeof(int32_t));
        if (!h->index) { fprintf(stderr, "VM: out of memory\n"); exit(1); }
        h->index_mask = isz - 1;
    }
    memset(h->index, 0xff, (size_t)isz * sizeof(int32_t));
    h->index_fill = 0;
    for (int i = 0; i < h->used; i++) {
        if (h->entries[i].occupied != 1) continue;
        uint32_t ix = h->entries[i].hash & h->index_mask;
        while (h->index[ix] >= 0) ix = (ix + 1) & h->index_mask;
        h->index[ix] = i;
        h->index_fill++;
    }
}

VMHash *vm_hash_new(int cap) {
    int c = VM_HASH_INLINE_CAP;
    if (cap > VM_HASH_INLINE_CAP) {
        while (c < cap) c *= 2;
    }
    VMHash *h = (VMHash*)vm_pool_pop(VM_POOL_HASH, sizeof(VMHash), VM_OBJ_HASH, 0);
    h->capacity = c;
    h->size = 0;
    h->tombstones = 0;
    h->used = 0;
    h->index = NULL;
    h->index_mask = 0;
    h->index_fill = 0;
    h->free_head = -1;
    h->class_id = 0;
    h->class_prog_id = 0;
    h->grow_gen = 0;
    h->class_name = NULL;
    h->iter_pos = 0;
    h->tied_obj = VM_UNDEF_VAL;
    h->tied_last_key = VM_UNDEF_VAL;
    if (c <= VM_HASH_INLINE_CAP) {
        h->entries = h->inline_entries;
        h->inline_entries[0].occupied = 0;
        h->inline_entries[1].occupied = 0;
        h->inline_entries[2].occupied = 0;
        h->inline_entries[3].occupied = 0;
    } else {
        h->entries = calloc((size_t)c, sizeof(VMHashEntry));
        vm_hash_index_build(h);
    }
    return h;
}

void vm_hash_free(VMHash *h) {
    if (!h) return;
    for (int i = 0; i < h->capacity; i++) {
        if (h->entries[i].occupied == 1) {
            key_free_entry(&h->entries[i]);
            vm_val_free(&h->entries[i].value);
        }
    }
    if (h->entries != h->inline_entries) free(h->entries);
    free(h->index);
    if (h->class_name) free(h->class_name);
    free(h);
}

/* Empty a hash whose keys/values the caller already released. */
static void vm_hash_reset_storage(VMHash *h) {
    memset(h->entries, 0, (size_t)h->capacity * sizeof(VMHashEntry));
    h->size = 0;
    h->used = 0;
    h->tombstones = 0;
    h->free_head = -1;
    h->index_fill = 0;
    h->iter_pos = 0;
    h->grow_gen++;
    if (h->index) memset(h->index, 0xff, (size_t)(h->index_mask + 1) * sizeof(int32_t));
}

/* Make room for one more entry: compact out deleted slots (order kept) or
 * double the capacity. */
static void vm_hash_grow(VMHash *h) {
    int old_cap = h->capacity;
    VMHashEntry *old = h->entries;
    int new_cap = (h->size * 2 < old_cap) ? old_cap : (old_cap * 2);
    h->grow_gen++;
    if (new_cap == old_cap) {
        int j = 0;
        for (int i = 0; i < h->used; i++) {
            if (old[i].occupied == 1) {
                if (j != i) old[j] = old[i];
                j++;
            }
        }
        if (j < old_cap) memset(&old[j], 0, (size_t)(old_cap - j) * sizeof(VMHashEntry));
        h->used = j;
    } else {
        VMHashEntry *ne = calloc((size_t)new_cap, sizeof(VMHashEntry));
        if (!ne) { fprintf(stderr, "VM: out of memory\n"); exit(1); }
        int j = 0;
        for (int i = 0; i < h->used; i++) {
            if (old[i].occupied == 1) ne[j++] = old[i];
        }
        h->entries = ne;
        h->capacity = new_cap;
        h->used = j;
        if (old != h->inline_entries) free(old);
    }
    h->tombstones = 0;
    h->free_head = -1;
    vm_hash_index_build(h);
}

/* Entry slot of KEY, or -1. */
static int32_t vm_hash_slot_idx_nh(VMHash *h, const char *key, size_t keylen, uint32_t hash) {
    if (!h || !key) return -1;
    uint32_t klen = (uint32_t)keylen;
    if (!h->index) {
        for (int i = 0; i < h->used; i++) {
            VMHashEntry *e = &h->entries[i];
            if (e->occupied == 1 && e->hash == hash && e->keylen == klen && memcmp(e->key, key, klen) == 0)
                return i;
        }
        return -1;
    }
    uint32_t mask = h->index_mask;
    uint32_t ix = hash & mask;
    for (;;) {
        int32_t ei = h->index[ix];
        if (ei < 0) return -1;
        VMHashEntry *e = &h->entries[ei];
        if (e->occupied == 1 && e->hash == hash && e->keylen == klen && memcmp(e->key, key, klen) == 0)
            return ei;
        ix = (ix + 1) & mask;
    }
}

/* Entry at the key's first index position, or NULL when that position is
 * empty -- which proves the key is absent (fast paths). */
static inline VMHashEntry *vm_hash_probe_first(VMHash *h, uint32_t kh) {
    if (h->index) {
        int32_t ix = h->index[kh & h->index_mask];
        return ix >= 0 ? &h->entries[ix] : NULL;
    }
    return h->used > 0 ? &h->entries[0] : NULL;
}

/* Claim the next dense slot for a key known to be absent and index it. The
 * caller fills key/value/hash/keylen/occupied and bumps size. */
static VMHashEntry *vm_hash_append_slot(VMHash *h, uint32_t hash) {
    int32_t dst;
    if (h->free_head >= 0) {
        dst = h->free_head;
        h->free_head = (int32_t)h->entries[dst].hash;
        h->tombstones--;
    } else {
        if (h->used >= h->capacity) vm_hash_grow(h);
        dst = h->used++;
    }
    if (h->index) {
        /* reused slots leave stale index entries behind: rebuild before
         * the index gets crowded (the new entry is indexed by the rebuild
         * only once it is occupied, so index it here after rebuilding) */
        if ((h->index_fill + 1) * 4 > (h->index_mask + 1) * 3) vm_hash_index_build(h);
        uint32_t ix = hash & h->index_mask;
        while (h->index[ix] >= 0) ix = (ix + 1) & h->index_mask;
        h->index[ix] = dst;
        h->index_fill++;
    }
    return &h->entries[dst];
}

/* Key bytes of a non-UTF-8 string being stored (set around the store by
 * OP_HASH_SET): the new entry is marked key_bytes. */
static __thread const char *g_vm_key_bytes_hint = NULL;
static uint32_t vm_hash_set_nh_slot(VMHash *h, const char *key, size_t keylen, uint32_t hash, VMValue v) {
    if (!h || !key) return UINT32_MAX;
    uint32_t klen = (uint32_t)keylen;
    int32_t found = vm_hash_slot_idx_nh(h, key, keylen, hash);
    if (found >= 0) {
        vm_val_free(&h->entries[found].value);
        h->entries[found].value = v;
        return (uint32_t)found;
    }
    VMHashEntry *e = vm_hash_append_slot(h, hash);
    const char *ikey = vm_intern_hash_key(g_active_vm, key, klen, hash);
    if (ikey) {
        e->key = (char *)ikey;
        e->key_interned = 1;
    } else {
        char *k = malloc((size_t)klen + 1);
        if (klen > 0) memcpy(k, key, klen);
        k[klen] = '\0';
        e->key = k;
        e->key_interned = 0;
    }
    e->value = v;
    e->hash = hash;
    e->keylen = klen;
    e->key_bytes = (uint8_t)(key == g_vm_key_bytes_hint);
    e->occupied = 1;
    h->size++;
    return (uint32_t)(e - h->entries);
}

static void vm_hash_set_nh(VMHash *h, const char *key, size_t keylen, uint32_t hash, VMValue v) {
    (void)vm_hash_set_nh_slot(h, key, keylen, hash, v);
}

static void vm_hash_set_n(VMHash *h, const char *key, size_t keylen, VMValue v) {
    if (!h || !key) return;
    vm_hash_set_nh(h, key, keylen, vm_hash_fn_n(key, (uint32_t)keylen), v);
}

void vm_hash_set(VMHash *h, const char *key, VMValue v) {
    vm_hash_set_n(h, key, key ? strlen(key) : 0, v);
}

static VMValue *vm_hash_slot_ptr_nh(VMHash *h, const char *key, size_t keylen, uint32_t hash) {
    int32_t idx = vm_hash_slot_idx_nh(h, key, keylen, hash);
    return idx >= 0 ? &h->entries[idx].value : NULL;
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

static VMValue vm_hash_delete_ret_n(VMHash *h, const char *key, size_t keylen) {
    if (!h || !key) return VM_UNDEF_VAL;
    uint32_t klen = (uint32_t)keylen;
    int32_t idx = vm_hash_slot_idx_nh(h, key, keylen, vm_hash_fn_n(key, klen));
    if (idx < 0) return VM_UNDEF_VAL;
    VMValue old = h->entries[idx].value;
    if (VM_IS_PTR(old) && VM_PTR_TYPE(old) == VM_OBJ_WEAKREF) old = VM_AS_WEAKREF(old)->target;
    key_free_entry(&h->entries[idx]);
    h->entries[idx].key = NULL;
    h->entries[idx].key_interned = 0;
    h->entries[idx].value = VM_UNDEF_VAL;
    h->entries[idx].occupied = 2;   /* the index slot stays as a tombstone */
    h->entries[idx].hash = (uint32_t)h->free_head;
    h->free_head = idx;
    h->size--;
    h->tombstones++;
    h->grow_gen++;
    if (h->size == 0) {
        /* emptied: drop the deleted slots so the next insert starts fresh */
        memset(h->entries, 0, (size_t)h->used * sizeof(VMHashEntry));
        h->used = 0;
        h->tombstones = 0;
        h->free_head = -1;
        h->index_fill = 0;
        if (h->index) memset(h->index, 0xff, (size_t)(h->index_mask + 1) * sizeof(int32_t));
    }
    return old;
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

static inline VMValue vm_hash_make_key_str_b(const char *key, size_t len, int key_bytes) {
    int nonascii = 0;
    int u8 = (!key_bytes && vm_utf8_is_valid_str(key, len, &nonascii) && nonascii);
    return vm_make_str_flag(key, len, u8);
}
static inline VMValue vm_hash_make_key_str(const char *key, size_t len) {
    return vm_hash_make_key_str_b(key, len, 0);
}

VMArray *vm_hash_keys(VMHash *h) {
    VMArray *keys = vm_array_new(h->size > 0 ? h->size : 8);
    for (int i = 0; i < h->capacity; i++)
        if (h->entries[i].occupied == 1)
            vm_array_push(keys, vm_hash_make_key_str_b(h->entries[i].key, h->entries[i].keylen, h->entries[i].key_bytes));
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
    p->prog_id = __sync_add_and_fetch(&g_next_prog_id, 1);
    pthread_mutex_init(&p->class_mutex, NULL);
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

static uint32_t vm_program_intern_class(VMProgram *p, const char *cls) {
    if (!p || !cls || !cls[0]) return 0;
    pthread_mutex_lock(&p->class_mutex);
    for (uint32_t i = 0; i < p->class_count; i++) {
        if (strcmp(p->class_names[i], cls) == 0) {
            pthread_mutex_unlock(&p->class_mutex);
            return i + 1;
        }
    }
    if (p->class_count >= p->class_cap) {
        p->class_cap = p->class_cap ? p->class_cap * 2 : 16;
        p->class_names = realloc(p->class_names, p->class_cap * sizeof(char*));
    }
    uint32_t id = p->class_count + 1;
    p->class_names[p->class_count++] = strdup(cls);
    pthread_mutex_unlock(&p->class_mutex);
    return id;
}

void vm_program_retain(VMProgram *p) {
    if (p) __sync_add_and_fetch(&p->refcount, 1);
}

void vm_chunk_free_jit(VMChunk *c) {
    if (!c) return;
#ifdef VM_JIT_X64
    if (c->jit_code && c->jit_size > 0) {
        munmap(c->jit_code, c->jit_size);
    }
#endif
    c->jit_code = NULL;
    c->jit_size = 0;
    free(c->jit_offs);
    c->jit_offs = NULL;
    c->jit_state = 0;
    c->jit_hot = 0;
}

void vm_program_free(VMProgram *p) {
    if (!p || __sync_sub_and_fetch(&p->refcount, 1) > 0) return;
    for (size_t i = 0; i < p->func_count; i++) {
        vm_chunk_free_jit(&p->funcs[i]);
        free(p->funcs[i].code); free(p->funcs[i].dt_code); free(p->funcs[i].dt_aux);
        free(p->funcs[i].dt_pc); free(p->funcs[i].ln_pc); free(p->funcs[i].ln_line); free(p->funcs[i].src_file);
        free(p->funcs[i].name); free(p->funcs[i].int_consts); free(p->funcs[i].rawint_slots);
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
    free(p->funcs); free(p->func_names); free(p->fn_index);
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
    free(p->cblock_link_flags);
    p->cblock_link_flags = NULL;
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
        vm_gc_free_one(NULL, co);
        co = next;
    }
    if (p->dl_handles) {
        for (int i = 0; i < p->dl_handle_count; i++) {
            if (p->dl_handles[i]) dlclose(p->dl_handles[i]);
        }
        free(p->dl_handles);
    }
    for (uint32_t i = 0; i < p->class_count; i++) free(p->class_names[i]);
    free(p->class_names);
    pthread_mutex_destroy(&p->class_mutex);
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
    if (!p->fn_index || (p->func_count * 2 + 2) > p->fn_index_cap) vm_fn_index_rebuild(p);
    else vm_fn_index_insert(p, idx);
    return &p->funcs[idx];
}

/* A converted scalar that holds an array/hash: the compiled backend's
 * value there is a reference to it. Takes and returns ownership. */
static StradaValue *vm_sv_container_as_ref(StradaValue *sv) {
    if (!sv || STRADA_IS_TAGGED_INT(sv)) return sv;
    if (SV_BLESSED(sv)) return sv;   /* an object already prints as bless(...) */
    if (sv->type == STRADA_ARRAY) return strada_new_ref_take(sv, '@');
    if (sv->type == STRADA_HASH) return strada_new_ref_take(sv, '%');
    return sv;
}

/* Source line a frame is executing (its call site, for a caller frame). */
static int vm_frame_line(VMFrame *f) {
    VMChunk *c = f ? f->chunk : NULL;
    if (!c || !c->dt_pc || !c->dt_code || !f->ip || c->ln_count == 0) return 0;
    long idx = (long)(f->ip - c->dt_code);
    if (idx > 0) idx--;                 /* ip is past the executing instruction */
    if (idx < 0) idx = 0;
    if ((size_t)idx > c->dt_len) idx = (long)c->dt_len;
    uint32_t pc = c->dt_pc[idx];
    int lo = 0, hi = c->ln_count - 1, best = -1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (c->ln_pc[mid] <= pc) { best = mid; lo = mid + 1; } else hi = mid - 1;
    }
    return best >= 0 ? c->ln_line[best] : 0;
}

/* Call stack in the compiled backend's format, innermost first:
 *   "  at NAME (FILE)" for the innermost frame, "  at NAME (FILE:LINE)" for
 * the others (the line of their call). Caller frees. */
static char *vm_build_trace(VM *vm, int top, int plain) {
    size_t cap = 256, pos = 0;
    char *t = malloc(cap);
    if (!t) return NULL;
    t[0] = 0;
    for (int i = top; i >= 0; i--) {
        VMFrame *f = &vm->frames[i];
        const char *nm = (f->chunk && f->chunk->name) ? f->chunk->name : "(unknown)";
        if (nm[0] == '_' && nm[1] == '_') continue;          /* __globals_init, __import_calls ... */
        const char *file = (f->chunk && f->chunk->src_file) ? f->chunk->src_file : "?";
        char cname[256];
        size_t ci = 0;
        for (const char *p = nm; *p && ci + 1 < sizeof(cname); p++) {
            if (p[0] == ':' && p[1] == ':') { cname[ci++] = '_'; p++; } else cname[ci++] = *p;
        }
        cname[ci] = 0;
        char line[1024];
        int ln = (i == top) ? 0 : vm_frame_line(f);
        int n = plain ? snprintf(line, sizeof(line), "  %s\n", cname)
              : ln > 0 ? snprintf(line, sizeof(line), "  at %s (%s:%d)\n", cname, file, ln)
                       : snprintf(line, sizeof(line), "  at %s (%s)\n", cname, file);
        if (n < 0) continue;
        if ((size_t)n >= sizeof(line)) n = (int)sizeof(line) - 1;
        if (pos + (size_t)n + 1 > cap) {
            while (pos + (size_t)n + 1 > cap) cap *= 2;
            char *nt = realloc(t, cap);
            if (!nt) { free(t); return NULL; }
            t = nt;
        }
        memcpy(t + pos, line, (size_t)n);
        pos += (size_t)n;
        t[pos] = 0;
    }
    return t;
}

static uint32_t vm_fn_name_hash(const char *s) {
    uint32_t h = 2166136261u;
    for (; *s; s++) { h ^= (uint8_t)*s; h *= 16777619u; }
    return h;
}

/* Insert func idx under its name unless the name is already indexed
 * (lookups return the first registration, as the old linear scan did). */
static void vm_fn_index_insert(VMProgram *p, size_t idx) {
    size_t mask = p->fn_index_cap - 1;
    const char *name = p->func_names[idx];
    size_t i = vm_fn_name_hash(name) & mask;
    while (p->fn_index[i]) {
        if (strcmp(p->func_names[p->fn_index[i] - 1], name) == 0) return;
        i = (i + 1) & mask;
    }
    p->fn_index[i] = (uint32_t)idx + 1;
}

static void vm_fn_index_rebuild(VMProgram *p) {
    size_t cap = 64;
    while (cap < p->func_count * 2 + 2) cap *= 2;
    free(p->fn_index);
    p->fn_index = calloc(cap, sizeof(uint32_t));
    p->fn_index_cap = cap;
    for (size_t i = 0; i < p->func_count; i++) vm_fn_index_insert(p, i);
}

int vm_program_find_func(VMProgram *p, const char *name) {
    if (!p || !name) return -1;
    if (!p->fn_index) {
        /* not indexed (no function added through vm_program_add_func) */
        for (size_t i = 0; i < p->func_count; i++)
            if (p->func_names[i] && strcmp(p->func_names[i], name) == 0) return (int)i;
        return -1;
    }
    size_t mask = p->fn_index_cap - 1;
    size_t i = vm_fn_name_hash(name) & mask;
    while (p->fn_index[i]) {
        uint32_t fi = p->fn_index[i] - 1;
        if (strcmp(p->func_names[fi], name) == 0) return (int)fi;
        i = (i + 1) & mask;
    }
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
/* Inheritance walks are bounded by VM_INHERIT_MAX_DEPTH (cycle guard only;
 * the compiled runtime walks any depth, so this must not be a real limit). */
#define VM_INHERIT_MAX_DEPTH 4096
int vm_class_isa(VMProgram *p, const char *cls, const char *target, int depth) {
    if (!cls || !target || depth > VM_INHERIT_MAX_DEPTH) return 0;
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
    vm->gc_threshold = 8192;
    memset(vm->regex_captures, 0, sizeof(vm->regex_captures));
    memset(vm->regex_capture_len, 0, sizeof(vm->regex_capture_len));
    memset(vm->regex_capture_utf8, 0, sizeof(vm->regex_capture_utf8));
    vm->saved_active_vm = g_active_vm;
    vm->trap_uncaught = 0;
    vm->end_ran = 0;
    vm->refcount = 1;
    vm->gc_safe = 1;
    vm->gc_requested = 0;
    vm->owner_thread = pthread_self();
    pthread_mutex_init(&vm->gc_mutex, NULL);
    pthread_mutex_init(&vm->gc_wait_mutex, NULL);
    pthread_cond_init(&vm->gc_start_cond, NULL);
    pthread_cond_init(&vm->gc_parked_cond, NULL);
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
    vm->gc_safe = 1;
    vm->gc_threshold = SIZE_MAX;
    vm->root_vm = root;
    if (root) {
        vm_retain(root);
        vm->globals = root->globals;
        vm->default_fh = root->default_fh;
        vm->recursion_limit = root->recursion_limit;
        pthread_mutex_lock(&root->gc_wait_mutex);
        while (__atomic_load_n(&root->gc_requested, __ATOMIC_ACQUIRE)) {
            pthread_cond_wait(&root->gc_start_cond, &root->gc_wait_mutex);
        }
        vm->worker_next = root->workers_head;
        root->workers_head = vm;
        pthread_mutex_unlock(&root->gc_wait_mutex);
    } else {
        vm->globals = vm_hash_new(64);
    }
    return vm;
}

static inline pthread_mutex_t *vm_globals_lock_ptr(VM *vm) {
    VM *owner = (vm && vm->root_vm) ? vm->root_vm : vm;
    if (!owner || (!vm->is_worker_vm && !owner->workers_head)) return NULL;
    return &owner->globals_mutex;
}

/* $_ (main::_) is per VM -- each thread / green task runs in its own VM --
 * while every other global is shared through vm->globals. */
static inline int vm_is_topic_name(const char *name) {
    return name && name[0] == 'm' && memcmp(name, "main::_", 8) == 0;
}
static inline VMHash *vm_gh_for(VM *vm, const char *name) {
    if (!vm) return NULL;
    if (vm_is_topic_name(name)) {
        if (!vm->topic_globals) vm->topic_globals = vm_hash_new(2);
        return vm->topic_globals;
    }
    return vm->globals;
}

static inline VMValue vm_globals_get(VM *vm, const char *name) {
    pthread_mutex_t *lk = vm_globals_lock_ptr(vm);
    if (lk) pthread_mutex_lock(lk);
    VMHash *gh = vm_gh_for(vm, name);
    VMValue r = gh ? vm_hash_get(gh, name) : VM_UNDEF_VAL;
    if (lk) pthread_mutex_unlock(lk);
    return r;
}

static inline VMValue vm_globals_get_raw(VM *vm, const char *name) {
    pthread_mutex_t *lk = vm_globals_lock_ptr(vm);
    if (lk) pthread_mutex_lock(lk);
    VMHash *gh = vm_gh_for(vm, name);
    VMValue *slot = gh ? vm_hash_slot_ptr(gh, name) : NULL;
    VMValue r = slot ? *slot : VM_UNDEF_VAL;
    if (lk) pthread_mutex_unlock(lk);
    return r;
}

static inline void vm_globals_set(VM *vm, const char *name, VMValue v) {
    pthread_mutex_t *lk = vm_globals_lock_ptr(vm);
    if (lk) pthread_mutex_lock(lk);
    { VMHash *gh = vm_gh_for(vm, name); if (gh) vm_hash_set(gh, name, v); }
    if (lk) pthread_mutex_unlock(lk);
}

static inline void vm_globals_set_n(VM *vm, const char *name, size_t len, VMValue v) {
    pthread_mutex_t *lk = vm_globals_lock_ptr(vm);
    if (lk) pthread_mutex_lock(lk);
    { VMHash *gh = (len == 7 && vm_is_topic_name(name)) ? vm_gh_for(vm, "main::_") : (vm ? vm->globals : NULL); if (gh) vm_hash_set_n(gh, name, len, v); }
    if (lk) pthread_mutex_unlock(lk);
}

static inline int vm_globals_exists(VM *vm, const char *name) {
    pthread_mutex_t *lk = vm_globals_lock_ptr(vm);
    if (lk) pthread_mutex_lock(lk);
    VMHash *gh = vm_gh_for(vm, name);
    int r = gh ? vm_hash_exists(gh, name) : 0;
    if (lk) pthread_mutex_unlock(lk);
    return r;
}

/* core::global_*("main::x"): the VM registers package main's `our`
 * variables under the bare name; the compiled registry uses "main::x". */
static const char *vm_global_key_main(VM *vm, const char *key) {
    if (key && strncmp(key, "main::", 6) == 0 && !vm_globals_exists(vm, key) && vm_globals_exists(vm, key + 6))
        return key + 6;
    return key;
}


static inline void vm_globals_delete(VM *vm, const char *name) {
    pthread_mutex_t *lk = vm_globals_lock_ptr(vm);
    if (lk) pthread_mutex_lock(lk);
    { VMHash *gh = vm_gh_for(vm, name); if (gh) vm_hash_delete(gh, name); }
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
    if (!hdr || hdr->gc_immortal >= 2 || hdr->gc_marked) return;
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
    if (tvm->topic_globals) vm_gc_mark_val(VM_MAKE_PTR(tvm->topic_globals), work, wlen, wcap);
    for (size_t i = 0; i < tvm->stack_top; i++) {
        vm_gc_mark_val(tvm->stack[i], work, wlen, wcap);
    }
    for (size_t i = 0; i < tvm->frame_count; i++) {
        VMFrame *f = &tvm->frames[i];
        if (f->chunk) {
            size_t fend = f->local_base + (size_t)(f->chunk->local_count + 1);
            if (fend > tvm->stack_top) {
                size_t limit = fend < tvm->stack_cap ? fend : tvm->stack_cap;
                for (size_t j = tvm->stack_top; j < limit; j++) {
                    vm_gc_mark_val(tvm->stack[j], work, wlen, wcap);
                }
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
        case VM_OBJ_FILEHANDLE: {
            VMFileHandle *fh = (VMFileHandle*)obj;
            vm_gc_mark_val(fh->mem_sref, &work, &wlen, &wcap);
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

static void vm_destroy_cache_clear(VM *vm) {
    for (int i = 0; i < 64; i++) {
        free(vm->destroy_cache[i].cls);
        vm->destroy_cache[i].cls = NULL;
    }
}

/* vm_find_method(cls, "DESTROY"), cached per class name: the collector asks
 * for every freed blessed object, and most classes have no DESTROY */
static int vm_find_destroy_cached(VM *vm, const char *cname) {
    VMProgram *p = vm->program;
    if (vm->destroy_cache_prog != (void *)p || vm->destroy_cache_funcs != p->func_count ||
        vm->destroy_cache_inherits != p->inherit_count) {
        vm_destroy_cache_clear(vm);
        vm->destroy_cache_prog = p;
        vm->destroy_cache_funcs = p->func_count;
        vm->destroy_cache_inherits = p->inherit_count;
    }
    uint32_t h = vm_fn_name_hash(cname);
    int slot = (int)(h & 63);
    if (vm->destroy_cache[slot].cls && vm->destroy_cache[slot].hash == h &&
        strcmp(vm->destroy_cache[slot].cls, cname) == 0)
        return vm->destroy_cache[slot].fidx;
    int fidx = vm_find_method(p, cname, "DESTROY");
    free(vm->destroy_cache[slot].cls);
    vm->destroy_cache[slot].cls = strdup(cname);
    vm->destroy_cache[slot].hash = h;
    vm->destroy_cache[slot].fidx = fidx;
    return fidx;
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
        int dfidx = vm_find_destroy_cached(vm, cname);
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

static __attribute__((noinline)) void vm_worker_park(VM *vm) {
    VM *root = (vm && vm->is_worker_vm) ? vm->root_vm : NULL;
    if (!root) return;
    pthread_mutex_lock(&root->gc_wait_mutex);
    __atomic_store_n(&vm->gc_safe, 1, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&root->gc_parked_cond);
    while (__atomic_load_n(&root->gc_requested, __ATOMIC_ACQUIRE)) {
        pthread_cond_wait(&root->gc_start_cond, &root->gc_wait_mutex);
    }
    __atomic_store_n(&vm->gc_safe, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&root->gc_wait_mutex);
}

static inline void vm_worker_poll_gc(VM *vm) {
    if (__builtin_expect(vm && vm->is_worker_vm && vm->root_vm &&
                         __atomic_load_n(&vm->root_vm->gc_requested, __ATOMIC_ACQUIRE), 0)) {
        vm_worker_park(vm);
    }
}

static void vm_worker_enter_active(VM *vm) {
    if (!vm || !vm->is_worker_vm || !vm->root_vm) return;
    VM *root = vm->root_vm;
    pthread_mutex_lock(&root->gc_wait_mutex);
    while (__atomic_load_n(&root->gc_requested, __ATOMIC_ACQUIRE)) {
        pthread_cond_wait(&root->gc_start_cond, &root->gc_wait_mutex);
    }
    __atomic_store_n(&vm->gc_safe, 0, __ATOMIC_RELEASE);
    pthread_mutex_unlock(&root->gc_wait_mutex);
}

static int vm_worker_enter_safe(VM *vm) {
    if (!vm || !vm->is_worker_vm || !vm->root_vm) return 1;
    if (__atomic_load_n(&vm->gc_safe, __ATOMIC_ACQUIRE)) return 1;
    VM *root = vm->root_vm;
    pthread_mutex_lock(&root->gc_wait_mutex);
    __atomic_store_n(&vm->gc_safe, 1, __ATOMIC_RELEASE);
    if (__atomic_load_n(&root->gc_requested, __ATOMIC_ACQUIRE)) {
        pthread_cond_broadcast(&root->gc_parked_cond);
    }
    pthread_mutex_unlock(&root->gc_wait_mutex);
    return 0;
}

static void vm_worker_leave_safe(VM *vm, int prev_safe) {
    if (prev_safe) return;
    vm_worker_enter_active(vm);
}

static int vm_gc_stw_begin(VM *vm) {
    pthread_mutex_lock(&vm->gc_wait_mutex);
    if (!vm->workers_head) {
        pthread_mutex_unlock(&vm->gc_wait_mutex);
        return 0;
    }
    __atomic_store_n(&vm->gc_requested, 1, __ATOMIC_RELEASE);
    for (;;) {
        int all_safe = 1;
        for (VM *w = vm->workers_head; w; w = w->worker_next) {
            if (!__atomic_load_n(&w->gc_safe, __ATOMIC_ACQUIRE)) {
                __atomic_store_n(&w->gc_threshold, 0, __ATOMIC_RELEASE);
                all_safe = 0;
            }
        }
        if (all_safe) break;
        pthread_cond_wait(&vm->gc_parked_cond, &vm->gc_wait_mutex);
    }
    pthread_mutex_unlock(&vm->gc_wait_mutex);
    return 1;
}

static void vm_gc_stw_end(VM *vm, int was_stw) {
    if (!was_stw) return;
    pthread_mutex_lock(&vm->gc_wait_mutex);
    for (VM *w = vm->workers_head; w; w = w->worker_next) {
        __atomic_store_n(&w->gc_threshold, SIZE_MAX, __ATOMIC_RELEASE);
    }
    __atomic_store_n(&vm->gc_requested, 0, __ATOMIC_RELEASE);
    pthread_cond_broadcast(&vm->gc_start_cond);
    pthread_mutex_unlock(&vm->gc_wait_mutex);
}

static void vm_gc_collect_ex(VM *vm, int propagate_try) {
    if (!vm || vm->is_worker_vm || vm->gc_pause > 0 || vm->exec_base > 0) return;

    int stw = vm_gc_stw_begin(vm);
    pthread_mutex_lock(&vm->gc_mutex);
    pthread_mutex_lock(&vm->globals_mutex);
    vm_gc_mark_all(vm);
    vm_gc_clear_weakrefs(vm);
    VMFinalizerItem *fin = NULL;
    size_t fin_count = vm_collect_finalizer_items(vm, &fin, 1);
    pthread_mutex_unlock(&vm->globals_mutex);

    if (fin_count > 0) {
        pthread_mutex_unlock(&vm->gc_mutex);
        vm_gc_stw_end(vm, stw);
        vm_run_destroy_batch_ex(vm, fin, fin_count, propagate_try);
        free(fin);
        stw = vm_gc_stw_begin(vm);
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
            vm->gc_freed_total++;
        }
    }
    vm_gc_reset_worker_marks(vm);
    pthread_mutex_unlock(&vm->gc_mutex);
    vm_gc_stw_end(vm, stw);

    while (dead_list) {
        VMObjHeader *next = dead_list->gc_next;
        vm_gc_free_one(vm, dead_list);
        dead_list = next;
    }
    vm->gc_threshold = (vm->gc_count < 4096) ? 8192 : (vm->gc_count * 2);
    if (vm->gc_user_disabled) vm->gc_threshold = SIZE_MAX / 2;
    vm->gc_runs++;
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
    memset(vm->method_ic, 0, sizeof(vm->method_ic));
    memset(vm->global_ic, 0, sizeof(vm->global_ic));
    memset(vm->hash_ic, 0, sizeof(vm->hash_ic));
    memset(vm->str_intern_cache, 0, sizeof(vm->str_intern_cache));
}

static void vm_pools_free_all(VM *vm) {
    if (!vm) return;
    for (int c = 1; c < VM_POOL_COUNT; c++) {
        VMObjHeader *p = vm->obj_pools[c];
        while (p) {
            VMObjHeader *next = p->gc_next;
            free(p);
            p = next;
        }
        vm->obj_pools[c] = NULL;
        vm->obj_pool_len[c] = 0;
    }
    if (vm->key_intern) {
        for (uint32_t i = 0; i < vm->key_intern_cap; i++) {
            free(vm->key_intern[i].key);
        }
        free(vm->key_intern);
        vm->key_intern = NULL;
        vm->key_intern_cap = 0;
        vm->key_intern_count = 0;
    }
}

void vm_free(VM *vm) {
    if (!vm) return;
    if (__sync_sub_and_fetch(&vm->refcount, 1) > 0) return;
    free(vm->cur_package);
    vm->cur_package = NULL;
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
        vm_worker_enter_safe(vm);
        pthread_mutex_lock(&root_vm->gc_wait_mutex);
        while (__atomic_load_n(&root_vm->gc_requested, __ATOMIC_ACQUIRE)) {
            pthread_cond_wait(&root_vm->gc_start_cond, &root_vm->gc_wait_mutex);
        }
        VM **wp = &root_vm->workers_head;
        while (*wp) {
            if (*wp == vm) {
                *wp = vm->worker_next;
                break;
            }
            wp = &(*wp)->worker_next;
        }
        vm->worker_next = NULL;
        pthread_mutex_unlock(&root_vm->gc_wait_mutex);
        pthread_mutex_lock(&root_vm->gc_mutex);
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
            vm_gc_free_one(NULL, obj);
            obj = next;
        }
    }
    vm_pools_free_all(vm);

    free(vm->stack);
    free(vm->frames);
    free(vm->exc_stack);
    for (int i = 0; i < vm->global_save_count; i++) {
        free(vm->global_saves[i].name);
        free(vm->global_saves[i].hash_key);
    }
    free(vm->global_saves);
    for (int i = 0; i < VM_RX_MAX; i++) {
        free(vm->regex_captures[i]);
        free(vm->regex_capture_names[i]);
    }
    free(vm->script_name);
    vm_method_cache_clear(vm);
    vm_destroy_cache_clear(vm);
    vm_regex_cache_free(vm);
    pthread_mutex_destroy(&vm->gc_mutex);
    pthread_mutex_destroy(&vm->gc_wait_mutex);
    pthread_cond_destroy(&vm->gc_start_cond);
    pthread_cond_destroy(&vm->gc_parked_cond);
    pthread_mutex_destroy(&vm->globals_mutex);
    /* last: DESTROY methods run above may throw and record a trace */
    free(vm->exc_trace);
    vm->exc_trace = NULL;
    VM *root = vm->root_vm;
    free(vm);
    if (root) vm_free(root);
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
        vm_gc_free_one(NULL, o);
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
        pcre2_code *re = strada_pcre2_compile_perl(pattern, pat_len, opts, &errcode, &erroff);
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
    pcre2_code *re = strada_pcre2_compile_perl(pattern, pat_len, opts, &errcode, &erroff);
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
    if (len == 0) return vm_get_empty_str(is_utf8);
    if (len == 1 && (unsigned char)s[0] < 128) return vm_get_ascii_str((unsigned char)s[0], is_utf8);
    VMString *vs = vm_str_alloc(len);
    if (is_utf8) vs->hdr.is_utf8 = 1;
    vs->len = (uint32_t)len;
    if (len > 0 && s) memcpy(vs->data, s, len);
    vs->data[len] = '\0';
    return VM_MAKE_PTR(vs);
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
    vm->regex_ngroups = 0;
    int hw = vm->regex_hw;
    if (hw > VM_RX_MAX) hw = VM_RX_MAX;
    vm->regex_hw = 0;
    for (int i = 0; i < hw; i++) {
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
    if (i < 0 || i >= VM_RX_MAX) return;
    if (i + 1 > vm->regex_ngroups) vm->regex_ngroups = i + 1;
    if (i + 1 > vm->regex_hw) vm->regex_hw = i + 1;
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
        int count = rc < VM_RX_MAX ? rc : VM_RX_MAX;
        {
            uint32_t ncap = 0;
            pcre2_pattern_info(re, PCRE2_INFO_CAPTURECOUNT, &ncap);
            vm->regex_ngroups = (int)ncap + 1 < VM_RX_MAX ? (int)ncap + 1 : VM_RX_MAX;
        }
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
                    if (__g >= 0 && __g < VM_RX_MAX && !vm->regex_capture_names[__g]) {
                        vm->regex_capture_names[__g] = strdup((const char *)(__e + 2));
                        if (__g + 1 > vm->regex_hw) vm->regex_hw = __g + 1;
                    }
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
        if (n > VM_RX_MAX) n = VM_RX_MAX;
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
                for (int i = 1; i < VM_RX_MAX; i++) {
                    if (vm->regex_captures[i] && vm->regex_capture_len[i] == vlen &&
                        memcmp(vm->regex_captures[i], vptr, vlen) == 0 && !vm->regex_capture_names[i]) {
                        vm->regex_capture_names[i] = strdup(hv->entries[e].key->data);
                        if (i + 1 > vm->regex_hw) vm->regex_hw = i + 1;
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
        int count = rc < VM_RX_MAX ? rc : VM_RX_MAX;
        {
            uint32_t ncap = 0;
            pcre2_pattern_info(re, PCRE2_INFO_CAPTURECOUNT, &ncap);
            vm->regex_ngroups = (int)ncap + 1 < VM_RX_MAX ? (int)ncap + 1 : VM_RX_MAX;
        }
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
                    if (__g >= 0 && __g < VM_RX_MAX && !vm->regex_capture_names[__g]) {
                        vm->regex_capture_names[__g] = strdup((const char *)(__e + 2));
                        if (__g + 1 > vm->regex_hw) vm->regex_hw = __g + 1;
                    }
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
static int vm_resolve_overload_func(VMProgram *prog, const char *class_name, const char *op);
static VMValue vm_invoke_func(VM *vm, int fidx, VMValue *args, int argc);
static int64_t vm_to_int_vm(VM *vm, VMValue v);
static VMValue vm_from_int128(__int128 r);
/* An `int` parameter holding the caller's non-integer value ("7.5"), read by
 * a fused local arithmetic op: int arithmetic uses its integer (compiled
 * converts int parameters at use sites). Objects keep their overloads. */
static inline VMValue vm_rawint_operand(VM *vm, const VMChunk *c, uint16_t slot, VMValue v) {
    (void)vm;
    /* plain values only (no user code runs here: cells, tied scalars and
     * objects keep the ordinary slow paths) */
    if (__builtin_expect(c->rawint_slots != NULL, 0) && slot < 256 && c->rawint_slots[slot] &&
        !VM_IS_INT(v) &&
        (VM_IS_FLOAT(v) || (VM_IS_PTR(v) && (VM_PTR_TYPE(v) == VM_OBJ_STR || VM_PTR_TYPE(v) == VM_OBJ_STRBUF))))
        return VM_MAKE_INT(vm_to_int(v));
    return v;
}
static int vm_exact_int_arith(int op_kind, VMValue a, VMValue b, VMValue *out);
static int vm_tied_array_find_method(VM *vm, VMValue tied_obj, const char *method);
static VMValue vm_tied_array_call(VM *vm, VMValue tied_obj, int fidx, VMValue *extra, int extra_argc);
static VMArray *vm_materialize_array(VM *vm, VMArray *src);
static VMArray *vm_tied_hash_collect_keys(VM *vm, VMValue tied_obj, int *has_firstkey);
static int64_t vm_tied_array_fetchsize(VM *vm, VMValue tied_obj);
static int64_t vm_to_int_vm(VM *vm, VMValue v);
static VMValue vm_stringify_ov(VM *vm, VMValue v);
static VMValue vm_from_int128(__int128 r);
static double vm_to_num_vm(VM *vm, VMValue v);
static int64_t vm_to_int_ov(VM *vm, VMValue v, int depth);
static double vm_to_num_ov(VM *vm, VMValue v, int depth);
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

static StradaValue *vm_to_sv(VMValue v);
/* A filesystem path containing a NUL byte names no file (the compiled
 * runtime's strada_path_buf rule): such calls fail. */
static int vm_path_has_nul(VMValue v) {
    char nb[256];
    size_t nl = 0;
    const char *np = vm_bytes_view(v, nb, sizeof(nb), &nl);
    return np && nl > 0 && memchr(np, 0, nl) != NULL;
}

static VMValue vm_sprintf(VM *vm, int argc, VMValue *args) {
    if (argc < 1) return vm_str("");
    /* Plain scalar arguments (numbers, strings, undef): format with the
     * runtime's sprintf so both backends agree (%c width, %b precision,
     * %5%, h/hh, ...). References and objects keep the VM formatter, which
     * knows VM-side overloads and reference addresses. */
    {
        int plain = 1;
        for (int i = 0; i < argc; i++) {
            if (VM_IS_PTR(args[i]) && VM_PTR_TYPE(args[i]) != VM_OBJ_STR && VM_PTR_TYPE(args[i]) != VM_OBJ_STRBUF) { plain = 0; break; }
        }
        if (plain && argc <= 256) {
            StradaValue *fsv = vm_to_sv(args[0]);
            StradaValue *arr = strada_new_array();
            for (int i = 1; i < argc; i++) strada_array_push_take(arr->value.av, vm_to_sv(args[i]));
            StradaValue * volatile res = NULL;
            int cm = strada_cleanup_mark();
            if (setjmp(*STRADA_TRY_PUSH()) == 0) {
                res = strada_sprintf_sv_arr(fsv, arr);
                STRADA_TRY_POP();
            } else {
                STRADA_TRY_POP();
                strada_cleanup_drain_to(cm);
                strada_clear_exception();
                res = NULL;
            }
            strada_decref(fsv);
            strada_decref(arr);
            if (res) {
                VMValue out = sv_to_vm(res);
                strada_decref(res);
                return out;
            }
        }
    }
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

            if (conv != 's' && vm && vm->program && vm->program->overload_count > 0 &&
                vm_obj_class_name(arg) != NULL) {
                /* numeric conversion of an object: its 0+ value, else its "" string */
                int nf = vm_resolve_overload_func(vm->program, vm_obj_class_name(arg), "0+");
                if (nf >= 0) {
                    VMValue nargs[1] = { arg };
                    vm->gc_pause++;
                    arg = vm_invoke_func(vm, nf, nargs, 1);
                    vm->gc_pause--;
                } else if (vm_has_stringify(vm->program, arg)) {
                    arg = vm_stringify_ov(vm, arg);
                }
                if (vm->has_pending_exc) { free(result); return VM_UNDEF_VAL; }
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
    if (!def_pkg || !def_pkg[0] || depth > VM_INHERIT_MAX_DEPTH) return -1;
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
    if (!class_name || depth > VM_INHERIT_MAX_DEPTH) return -1;
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
    if (!class_name || depth > VM_INHERIT_MAX_DEPTH) return NULL;
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

/* ===== Identity-preserving VM <-> StradaValue conversion (lib/Eval) =====
 * Every conversion runs with a pointer memo (VM object -> SV container and
 * SV container -> VM object), so shared substructure stays shared, cycles
 * terminate, and DAGs convert in linear time instead of once per path
 * (V5-2/V5-3/V5-4). vm_conv_begin()/vm_conv_end() widen the memo over several
 * conversions -- lib/Eval converts all globals of a VM (and the eval result)
 * in one span, so two globals holding the same container still do after the
 * round trip. Nested containers convert to REFs (an element holding an array
 * is an array reference in the compiled runtime; V5-5). */
#ifndef STRADA_NO_TLS
#define VM_CONV_TLS __thread
#else
#define VM_CONV_TLS
#endif
typedef struct { const void *k; uintptr_t v; } VMConvSlot;
typedef struct { VMConvSlot *t; size_t cap, n; } VMConvMap;
static VM_CONV_TLS VMConvMap g_conv_v2s;   /* VM object -> StradaValue* (memo holds a ref) */
static VM_CONV_TLS VMConvMap g_conv_s2v;   /* SV container -> VMValue */
static VM_CONV_TLS int g_conv_depth = 0;
#define VM_CONV_MAX_DEPTH 10000

static inline size_t vm_conv_hash(const void *k, size_t mask) {
    uintptr_t x = (uintptr_t)k;
    x ^= x >> 17; x *= (uintptr_t)0xed5ad4bbU; x ^= x >> 11;
    return (size_t)x & mask;
}
static int vm_conv_get(VMConvMap *m, const void *k, uintptr_t *out) {
    if (g_conv_depth <= 0 || !m->t || !k) return 0;
    size_t mask = m->cap - 1;
    for (size_t i = vm_conv_hash(k, mask);; i = (i + 1) & mask) {
        if (!m->t[i].k) return 0;
        if (m->t[i].k == k) { *out = m->t[i].v; return 1; }
    }
}
static void vm_conv_put(VMConvMap *m, const void *k, uintptr_t v) {
    if (g_conv_depth <= 0 || !k) return;
    if (!m->t || (m->n + 1) * 2 > m->cap) {
        size_t ncap = m->cap ? m->cap * 2 : 64;
        VMConvSlot *nt = calloc(ncap, sizeof(VMConvSlot));
        if (!nt) return;
        for (size_t j = 0; j < m->cap; j++) {
            if (!m->t[j].k) continue;
            size_t i = vm_conv_hash(m->t[j].k, ncap - 1);
            while (nt[i].k) i = (i + 1) & (ncap - 1);
            nt[i] = m->t[j];
        }
        free(m->t);
        m->t = nt;
        m->cap = ncap;
    }
    size_t mask = m->cap - 1;
    size_t i = vm_conv_hash(k, mask);
    while (m->t[i].k && m->t[i].k != k) i = (i + 1) & mask;
    if (!m->t[i].k) m->n++;
    m->t[i].k = k;
    m->t[i].v = v;
}
void vm_conv_begin(void) { g_conv_depth++; }
void vm_conv_end(void) {
    if (g_conv_depth <= 0 || --g_conv_depth > 0) return;
    if (g_conv_v2s.t) {
        for (size_t i = 0; i < g_conv_v2s.cap; i++)
            if (g_conv_v2s.t[i].k) strada_decref((StradaValue *)g_conv_v2s.t[i].v);
        free(g_conv_v2s.t);
    }
    free(g_conv_s2v.t);
    memset(&g_conv_v2s, 0, sizeof(g_conv_v2s));
    memset(&g_conv_s2v, 0, sizeof(g_conv_s2v));
}
static void vm_conv_remember_sv(const void *vobj, StradaValue *sv) {
    if (g_conv_depth <= 0) return;
    strada_incref(sv);
    vm_conv_put(&g_conv_v2s, vobj, (uintptr_t)sv);
}
/* A converted container as the caller wants it: the container itself at the
 * top level (globals export, __C__ arguments), a fresh ref to it (blessed like
 * the container) when it is an element of another container. */
static StradaValue *vm_conv_container_out(StradaValue *c, int nested) {
    if (!nested) { strada_incref(c); return c; }
    StradaValue *r = strada_ref_create(c);
    if (c->meta && c->meta->blessed_package) strada_bless(r, c->meta->blessed_package);
    return r;
}
/* Is CLS a class of the active VM program (methods or inheritance)? A
 * blessed host ref of such a class converts to a VM object, so an object
 * that went out of an eval comes back as one (pointer-identity cache: the
 * runtime's blessed package names are interned sticky). */
static int vm_prog_has_class(VMProgram *p, const char *cls) {
    if (!p || !cls || !cls[0]) return 0;
    static VM_CONV_TLS VMProgram *c_prog;
    static VM_CONV_TLS const char *c_cls;
    static VM_CONV_TLS int c_res;
    if (c_prog == p && c_cls == cls) return c_res;
    int res = 0;
    size_t cl = strlen(cls);
    /* methods are named CLS_method (CLS with :: as _), or CLS::method */
    char san[256];
    size_t sl = 0;
    for (size_t k = 0; cls[k] && sl < sizeof(san) - 1; k++) {
        if (cls[k] == ':' && cls[k + 1] == ':') { san[sl++] = '_'; k++; }
        else san[sl++] = cls[k];
    }
    san[sl] = 0;
    for (size_t i = 0; i < p->func_count && !res; i++) {
        const char *fn = p->func_names ? p->func_names[i] : NULL;
        if (!fn) continue;
        if (strncmp(fn, cls, cl) == 0 && fn[cl] == ':' && fn[cl + 1] == ':') res = 1;
        else if (strncmp(fn, san, sl) == 0 && fn[sl] == '_' && fn[sl + 1]) res = 1;
    }
    for (int i = 0; i < p->inherit_count && !res; i++) {
        if (p->inherits[i].child && strcmp(p->inherits[i].child, cls) == 0) res = 1;
    }
    for (int i = 0; i < p->attr_count && !res; i++) {
        if (p->attrs[i].class_name && strcmp(p->attrs[i].class_name, cls) == 0) res = 1;
    }
    c_prog = p; c_cls = cls; c_res = res;
    return res;
}

/* Convert VMValue to StradaValue* for passing to __C__ blocks / the host. */
static StradaValue *vm_to_sv_depth(VMValue v, int depth, int nested) {
    if (VM_IS_ANY_INT(v)) return strada_new_int(VM_ANY_INT_VAL(v));
    if (VM_IS_FLOAT(v)) return strada_new_num(VM_FLOAT_VAL(v));
    if (VM_IS_UNDEF(v)) return strada_new_undef();
    if (depth >= VM_CONV_MAX_DEPTH) return strada_new_undef();
    if (VM_IS_PTR(v)) {
        switch (VM_PTR_TYPE(v)) {
        case VM_OBJ_UINT64: return strada_new_uint(VM_AS_UINT64(v)->val);
        case VM_OBJ_STR: {
            StradaValue *r = strada_new_str_len(VM_AS_STR(v)->data, VM_AS_STR(v)->len);
            if (VM_AS_STR(v)->hdr.is_utf8) STRADA_SET_UTF8_FLAG(r);
            return r;
        }
        case VM_OBJ_ARRAY:
        case VM_OBJ_HASH: {
            uintptr_t hit;
            if (vm_conv_get(&g_conv_v2s, VM_TO_PTR(v), &hit))
                return vm_conv_container_out((StradaValue *)hit, nested);
            StradaValue *c;
            if (VM_PTR_TYPE(v) == VM_OBJ_ARRAY) {
                VMArray *va = VM_AS_ARRAY(v);
                c = strada_new_array();
                if (va->class_name) strada_bless(c, va->class_name);
                vm_conv_remember_sv(va, c);
                for (int i = 0; i < va->size; i++)
                    strada_array_push_take(c->value.av, vm_to_sv_depth(va->items[i], depth + 1, 1));
            } else {
                VMHash *vh = VM_AS_HASH(v);
                c = strada_new_hash();
                if (vh->class_name) strada_bless(c, vh->class_name);
                vm_conv_remember_sv(vh, c);
                for (int i = 0; i < vh->capacity; i++)
                    if (vh->entries[i].occupied == 1) {
                        StradaValue *ksv = strada_new_str_len(vh->entries[i].key, vh->entries[i].keylen);
                        StradaValue *vsv = vm_to_sv_depth(vh->entries[i].value, depth + 1, 1);
                        strada_hash_set_sv(c->value.hv, ksv, vsv);
                        strada_decref(ksv);
                        strada_decref(vsv);
                    }
            }
            StradaValue *out = vm_conv_container_out(c, nested);
            strada_decref(c);
            return out;
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
            return vm_to_sv_depth(VM_AS_CELL(v)->val, depth + 1, nested);
        case VM_OBJ_WEAKREF:
            return vm_to_sv_depth(VM_AS_WEAKREF(v)->target, depth + 1, nested);
        case VM_OBJ_TIED_SCALAR: {
            if (!g_active_vm || g_active_vm->has_pending_exc) return strada_new_undef();
            VMValue fv = vm_tied_scalar_fetch(g_active_vm, v);
            if (g_active_vm->has_pending_exc) return strada_new_undef();
            return vm_to_sv_depth(fv, depth + 1, nested);
        }
        case VM_OBJ_FILEHANDLE: {
            VMFileHandle *fh = (VMFileHandle*)VM_TO_PTR(v);
            StradaValue *sv = vm_fh_ensure_sv(fh);
            strada_incref(sv);
            return sv;
        }
        case VM_OBJ_CLOSURE: {
            if (!g_active_vm || !g_active_vm->program) return strada_new_undef();
            uintptr_t hit;
            if (vm_conv_get(&g_conv_v2s, VM_TO_PTR(v), &hit)) {
                strada_incref((StradaValue *)hit);
                return (StradaValue *)hit;
            }
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
            if (csv) vm_conv_remember_sv(ctx->vcl, csv);
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
            StradaValue *rv = strada_ref_create_take(vm_to_sv_depth(tv, depth + 1, 1));
            if (sr->class_name) strada_bless(rv, sr->class_name);
            return rv;
        }
        default: return strada_new_undef();
        }
    }
    return strada_new_undef();
}

static StradaValue *vm_to_sv(VMValue v) {
    vm_conv_begin();
    StradaValue *r = vm_to_sv_depth(v, 0, 0);
    vm_conv_end();
    return r;
}

struct StradaValue *vm_value_to_sv(VMValue v) {
    return vm_to_sv(v);
}

/* Like vm_value_to_sv, but a container comes back as a reference to it (a
 * value handed to the host as a scalar: eval results, exceptions). */
struct StradaValue *vm_value_to_sv_scalar(VMValue v) {
    vm_conv_begin();
    StradaValue *r = vm_to_sv_depth(v, 0, 1);
    vm_conv_end();
    return r;
}

/* Convert StradaValue* back to VMValue.
 * Plain ARRAY/HASH results (and REFs to them) deep-convert into VM
 * containers so element access works — the VM's index/hash-get paths do not
 * understand wrapped native SVs. Opaque types (sockets, regexes, pointers)
 * still wrap as VMNativeSV and round-trip through vm_to_sv by identity.
 * Memoized (see vm_conv_begin): shared containers stay shared, cycles close. */
static VMValue sv_to_vm_depth(StradaValue *sv, int depth);
static VMValue sv_to_vm_container(StradaValue *c, const char *cls, int depth) {
    uintptr_t hit;
    if (vm_conv_get(&g_conv_s2v, c, &hit)) return (VMValue)hit;
    if (!cls && c->meta && c->meta->blessed_package) cls = c->meta->blessed_package;
    if (c->type == STRADA_ARRAY) {
        StradaArray *av = c->value.av;
        VMArray *va = vm_array_new((int)(av ? av->size : 0) + 1);
        if (cls) va->class_name = strdup(cls);
        vm_conv_put(&g_conv_s2v, c, (uintptr_t)VM_MAKE_PTR(va));
        if (av) {
            for (size_t i = 0; i < av->size; i++)
                vm_array_push(va, sv_to_vm_depth(strada_array_get(av, (int64_t)i), depth + 1));
        }
        return VM_MAKE_PTR(va);
    }
    StradaHash *hv = c->value.hv;
    VMHash *h = vm_hash_new(16);
    if (cls) h->class_name = strdup(cls);
    vm_conv_put(&g_conv_s2v, c, (uintptr_t)VM_MAKE_PTR(h));
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
    return VM_MAKE_PTR(h);
}

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
        return vm_make_str_flag(sv->value.pv, STRADA_STR_BYTELEN(sv), STRADA_STR_IS_UTF8(sv) != 0);
    }
    case STRADA_UNDEF: return VM_UNDEF_VAL;
    case STRADA_ARRAY:
    case STRADA_HASH:
        if (depth >= VM_CONV_MAX_DEPTH) break;
        return sv_to_vm_container(sv, NULL, depth);
    case STRADA_REF: {
        /* Keep slot refs wrapped as VMNativeSV so native slot references
         * keep variable linkage. */
        if (strada_is_slot_ref(sv)) break;
        if (depth >= VM_CONV_MAX_DEPTH) break;
        StradaValue *rv = sv->value.rv;
        int is_cont = (rv && !STRADA_IS_TAGGED_INT(rv) &&
                       (rv->type == STRADA_ARRAY || rv->type == STRADA_HASH));
        /* A blessed ref stays wrapped (host objects dispatch to host
         * methods) unless its class lives in the VM program: then it is a
         * VM object that went out to the host and comes back. */
        if (sv->meta && sv->meta->blessed_package) {
            if (is_cont && g_active_vm && vm_prog_has_class(g_active_vm->program, sv->meta->blessed_package))
                return sv_to_vm_container(rv, sv->meta->blessed_package, depth + 1);
            break;
        }
        /* Deref to the container so VM `->[i]` / `->{k}` access works;
         * refs to scalars and opaque targets stay wrapped. */
        if (is_cont) return sv_to_vm_container(rv, NULL, depth + 1);
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
    uintptr_t whit;
    if (vm_conv_get(&g_conv_s2v, sv, &whit)) return (VMValue)whit;
    strada_incref(sv);
    VMNativeSV *nsv = malloc(sizeof(VMNativeSV));
    vm_gc_init_hdr(&nsv->hdr, VM_OBJ_NATIVE_SV);
    nsv->sv = sv;
    vm_conv_put(&g_conv_s2v, sv, (uintptr_t)VM_MAKE_PTR(nsv));
    return VM_MAKE_PTR(nsv);
}

static VMValue sv_to_vm(StradaValue *sv) {
    vm_conv_begin();
    VMValue r = sv_to_vm_depth(sv, 0);
    vm_conv_end();
    return r;
}

VMValue vm_value_from_sv(struct StradaValue *sv) {
    return sv_to_vm(sv);
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
    /* Depth cap, as the compiled strada_clone_rec: past its 256 levels the
     * subtree becomes undef, with a one-time warning (it used to share the
     * deeper subtree silently). The compiled clone spends two levels per
     * nested container (the reference and the container); a VM container
     * is both, so the cap here is 128 containers. */
    if (depth >= 128) {
        static int vm_clone_depth_warned = 0;
        if (!vm_clone_depth_warned) {
            vm_clone_depth_warned = 1;
            fprintf(stderr, "Warning: clone() exceeded max depth %d; result truncated\n", 256);
        }
        return VM_UNDEF_VAL;
    }
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
        if (!VM_IS_UNDEF(oa->tied_obj) && vm) {
            VMArray *src = vm_materialize_array(vm, oa);
            if (vm->has_pending_exc) return out;
            for (int i = 0; i < src->size; i++) {
                VMValue cv = vm_clone_val_rec(vm, src->items[i], m, depth + 1);
                if (vm->has_pending_exc) return out;
                vm_array_push(na, cv);
            }
            return out;
        }
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
                    if (vm && vm->has_pending_exc) return out;
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
        if (!VM_IS_UNDEF(oh->tied_obj) && vm) {
            int has_fk = 0;
            VMArray *keys = vm_tied_hash_collect_keys(vm, oh->tied_obj, &has_fk);
            if (vm->has_pending_exc) return out;
            if (keys) {
                int ffidx = vm_tied_array_find_method(vm, oh->tied_obj, "FETCH");
                for (int i = 0; i < keys->size; i++) {
                    VMValue kv = keys->items[i];
                    VMValue vv = VM_UNDEF_VAL;
                    if (ffidx >= 0) {
                        /* vm_tied_array_call prepends the tied object itself. */
                        VMValue fargs[1] = { kv };
                        vv = vm_tied_array_call(vm, oh->tied_obj, ffidx, fargs, 1);
                        if (vm->has_pending_exc) return out;
                    }
                    VMValue cv = vm_clone_val_rec(vm, vv, m, depth + 1);
                    if (vm->has_pending_exc) return out;
                    char kbuf[256];
                    size_t klen = 0;
                    const char *ks = vm_bytes_view(kv, kbuf, sizeof(kbuf), &klen);
                    vm_hash_set_n(nh, ks, klen, cv);
                }
            }
            return out;
        }
        for (int i = 0; i < oh->capacity; i++) {
            if (oh->entries[i].occupied == 1) {
                VMValue ev = oh->entries[i].value;
                if (vm && vm->has_weakrefs && VM_IS_PTR(ev) && VM_PTR_TYPE(ev) == VM_OBJ_WEAKREF) {
                    ev = VM_AS_WEAKREF(ev)->target;
                }
                VMValue cv = vm_clone_val_rec(vm, ev, m, depth + 1);
                if (vm && vm->has_pending_exc) return out;
                vm_hash_set_n(nh, oh->entries[i].key, oh->entries[i].keylen, cv);
            }
        }
        return out;
    }
    case VM_OBJ_SREF: {
        VMScalarRef *sr = VM_AS_SREF(v);
        VMScalarRef *nsr = vm_sref_alloc();
        if (sr->class_name) nsr->class_name = strdup(sr->class_name);
        VMCell *nc = vm_cell_alloc();
        nc->val = VM_UNDEF_VAL;
        nsr->cell = VM_MAKE_PTR(nc);
        VMValue out = VM_MAKE_PTR(nsr);
        vm_clone_map_put(m, v, out);
        VMValue tv = VM_UNDEF_VAL;
        if (sr->global_name) {
            if (vm && vm->globals) tv = vm_globals_get(vm, sr->global_name);
        } else if (VM_IS_PTR(sr->cell) && VM_PTR_TYPE(sr->cell) == VM_OBJ_CELL) {
            tv = VM_AS_CELL(sr->cell)->val;
        } else {
            tv = sr->cell;
        }
        nc->val = vm_clone_val_rec(vm, tv, m, depth + 1);
        return out;
    }
    case VM_OBJ_CELL:
        return vm_clone_val_rec(vm, VM_AS_CELL(v)->val, m, depth + 1);
    case VM_OBJ_WEAKREF:
        return vm_clone_val_rec(vm, VM_AS_WEAKREF(v)->target, m, depth + 1);
    case VM_OBJ_TIED_SCALAR: {
        if (!vm) return v;
        VMValue fv = vm_tied_scalar_fetch(vm, v);
        if (vm->has_pending_exc) return VM_UNDEF_VAL;
        return vm_clone_val_rec(vm, fv, m, depth + 1);
    }
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

static int cblock_bind_entries(VMCBlock *cbs, int n, void *h) {
    void (*fns[n > 0 ? n : 1])(void**);
    for (int bi = 0; bi < n; bi++) {
        char nm[48];
        snprintf(nm, sizeof(nm), "__cblock_entry_%d", bi);
        fns[bi] = (void (*)(void**))dlsym(h, nm);
        if (!fns[bi]) return -1;
    }
    /* one handle owner (cbs[0]); vm_program_free dlcloses non-NULL handles */
    cbs[0].dl_handle = h;
    for (int bi = 0; bi < n; bi++) __atomic_store_n(&cbs[bi].fn, fns[bi], __ATOMIC_RELEASE);
    return 0;
}

/* Compile N __C__ blocks into ONE shared object (entry points
 * __cblock_entry_<i>), so file-scope statics in the program-level prelude
 * are shared by every block -- as in the compiled backend, where they are
 * one translation unit. */
static int cblock_jit_compile_n(VMCBlock *cbs, int n, const char *runtime_inc, const char *prelude, int quiet, const char *link_flags) {
    char cache_dir[512];
    ensure_cache_dir(cache_dir, sizeof(cache_dir));

    /* Run eviction once per process */
    if (!eviction_done) { cache_evict_stale(cache_dir); eviction_done = 1; }

    /* "v2": entry functions return StradaValue* (a `return` in the block) */
    uint64_t hash = (14695981039346656037ULL ^ 0x7632ULL) ^ (uint64_t)n;
    for (int bi = 0; bi < n; bi++)
        hash = hash * 1099511628211ULL ^ cblock_hash(cbs[bi].code, cbs[bi].var_names, cbs[bi].var_count);
    if (prelude) {
        /* prelude participates in the cache key */
        for (const char *pp = prelude; *pp; pp++) hash = hash * 1099511628211ULL ^ (unsigned char)*pp;
    }
    if (link_flags) {
        for (const char *pp = link_flags; *pp; pp++) hash = hash * 1099511628211ULL ^ (unsigned char)*pp;
    }
    /* The blocks compile against the runtime and VM headers (vm.h struct
     * layouts): a cached object built against older headers reads fields at
     * stale offsets, so the headers' identity and this binary's struct sizes
     * are part of the key. */
    {
        const char *hinc = runtime_inc ? runtime_inc : ".";
        const char *hdrs[] = { "strada_runtime.h", "strada_runtime_tcc.h", "../interpreter/vm.h",
                               "../interpreter/vm_compiler.h" };
        for (size_t hi = 0; hi < sizeof(hdrs) / sizeof(hdrs[0]); hi++) {
            char hp[1024];
            struct stat hst;
            snprintf(hp, sizeof(hp), "%s/%s", hinc, hdrs[hi]);
            if (stat(hp, &hst) == 0) {
                hash = hash * 1099511628211ULL ^ (uint64_t)hst.st_size;
                hash = hash * 1099511628211ULL ^ (uint64_t)hst.st_mtim.tv_sec;
                hash = hash * 1099511628211ULL ^ (uint64_t)hst.st_mtim.tv_nsec;
            }
        }
        hash = hash * 1099511628211ULL ^ (uint64_t)sizeof(VM);
        hash = hash * 1099511628211ULL ^ (uint64_t)sizeof(VMProgram);
        hash = hash * 1099511628211ULL ^ (uint64_t)sizeof(VMChunk);
    }
    char so_path[600];
    snprintf(so_path, sizeof(so_path), "%s/%016llx.so", cache_dir, (unsigned long long)hash);

    /* Check cache */
    struct stat st;
    if (stat(so_path, &st) == 0 && st.st_size > 0) {
        void *h = dlopen(so_path, RTLD_NOW);
        if (h) {
            if (cblock_bind_entries(cbs, n, h) == 0) return 0; /* cache hit */
            dlclose(h);
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
    for (int bi = 0; bi < n; bi++) {
        VMCBlock *cb = &cbs[bi];
        fprintf(f, "StradaValue *__cblock_entry_%d(StradaValue **__vars) {\n", bi);
        /* Declare local variables from the __vars array */
        for (int i = 0; i < cb->var_count; i++) {
            fprintf(f, "    StradaValue *%s = __vars[%d];\n", cb->var_names[i], i);
        }
        fprintf(f, "\n    /* User code */\n");
        fprintf(f, "{\n%s\n}\n", cb->code);
        fprintf(f, "\n    /* Copy back modified variables */\n");
        for (int i = 0; i < cb->var_count; i++) {
            fprintf(f, "    __vars[%d] = %s;\n", i, cb->var_names[i]);
        }
        fprintf(f, "    return NULL;   /* no `return` in the block */\n");
        fprintf(f, "}\n");
    }
    fclose(f);

    /* Compile to shared library with shell-quoted arguments */
    const char *inc = runtime_inc ? runtime_inc : ".";
    char q_inc[1024], q_so[1024], q_src[1024];
    shell_quote_arg(inc, q_inc, sizeof(q_inc));
    shell_quote_arg(tmp_so, q_so, sizeof(q_so));
    shell_quote_arg(src_path, q_src, sizeof(q_src));
    /* The VM's own headers (vm.h, vm_compiler.h) sit in interpreter/ next to
     * runtime/ (dev tree and install): blocks that embed a VM, like lib/Eval's,
     * include them. Their symbols resolve against this -rdynamic binary. */
    char interp_inc[1024] = "", q_interp[1100] = "";
    snprintf(interp_inc, sizeof(interp_inc), "%s/../interpreter", inc);
    if (access(interp_inc, R_OK) == 0) {
        char q_i[1024];
        shell_quote_arg(interp_inc, q_i, sizeof(q_i));
        snprintf(q_interp, sizeof(q_interp), " -I%s", q_i);
    }
    char cmd[4800];
    snprintf(cmd, sizeof(cmd),
        "gcc -shared -fPIC -O2 -I%s%s -o %s %s -w%s 2>&1",
        q_inc, q_interp, q_so, q_src, link_flags ? link_flags : "");

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
        if (!quiet) fprintf(stderr, "__C__ JIT: compilation failed:\n%s", errbuf);
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
    void *h = dlopen(load_path, RTLD_NOW);
    if (!h) {
        if (!quiet) fprintf(stderr, "__C__ JIT: dlopen failed: %s\n", dlerror());
        return -1;
    }
    if (cblock_bind_entries(cbs, n, h) != 0) {
        if (!quiet) fprintf(stderr, "__C__ JIT: dlsym failed: %s\n", dlerror());
        dlclose(h);
        return -1;
    }
    return 0;
}

/* JIT the program's __C__ blocks: all of them into one object (shared
 * statics); if that fails, fall back to compiling just the requested block
 * alone (an unrelated broken block must not disable the others). */
static int cblock_jit_compile(VMProgram *prog, int idx) {
    if (!prog->cblock_jit_all_tried) {
        prog->cblock_jit_all_tried = 1;
        int all_unset = 1;
        for (int i = 0; i < prog->cblock_count; i++) if (prog->cblocks[i].fn || prog->cblocks[i].dl_handle) all_unset = 0;
        if (all_unset && prog->cblock_count > 1 &&
            cblock_jit_compile_n(prog->cblocks, prog->cblock_count, prog->runtime_include_path, prog->cblock_prelude, 1, prog->cblock_link_flags) == 0)
            return 0;
    }
    if (prog->cblocks[idx].fn) return 0;
    return cblock_jit_compile_n(&prog->cblocks[idx], 1, prog->runtime_include_path, prog->cblock_prelude, 0, prog->cblock_link_flags);
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

static void vm_restore_one_hash_elem(VM *vm, VMHash *h, char *hkey, size_t hkeylen, int existed, VMValue saved_value, int propagate_exc) {
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
            VMValue ksv = VM_MAKE_STR_N(hkey, hkeylen);
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
                vm_hash_set_n(h, hkey, hkeylen, saved_value);
            } else {
                vm_hash_delete_n(h, hkey, hkeylen);
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
            size_t hkeylen = gs->hash_keylen;
            int existed = gs->existed;
            VMValue saved_value = gs->saved_value;
            gs->hash_key = NULL;
            gs->hash_keylen = 0;
            gs->hash = NULL;
            gs->saved_value = VM_UNDEF_VAL;
            vm_restore_one_hash_elem(vm, h, hkey, hkeylen, existed, saved_value, 0);
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
    volatile int _prev_safe = vm_worker_enter_safe(vm);
    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        StradaValue *nr = strada_method_call(self_sv, method, packed_args);
        STRADA_TRY_POP();
        vm_worker_leave_safe(vm, _prev_safe);
        vm->gc_pause--;
        *out_res = nr;
        return 0;
    } else {
        STRADA_TRY_POP();
        vm_worker_leave_safe(vm, _prev_safe);
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
    volatile int _prev_safe = vm_worker_enter_safe(vm);
    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        StradaValue *nr = strada_closure_call_array(cl_sv, packed_args);
        STRADA_TRY_POP();
        vm_worker_leave_safe(vm, _prev_safe);
        vm->gc_pause--;
        *out_res = nr;
        return 0;
    } else {
        STRADA_TRY_POP();
        vm_worker_leave_safe(vm, _prev_safe);
        strada_local_restore_to(lm);
        strada_cleanup_drain_to(cm);
        vm->gc_pause--;
        *out_exc = vm_capture_c_exception();
        return 1;
    }
}

__attribute__((noinline))
static int vm_c_call_cblock(VM *vm, StradaValue *(*fn)(StradaValue**), StradaValue **sv_vars, VMValue *out_exc, StradaValue **out_ret) {
    int cm = strada_cleanup_mark();
    int lm = strada_local_depth_get();
    vm->gc_pause++;
    volatile int _prev_safe = vm_worker_enter_safe(vm);
    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        *out_ret = fn(sv_vars);
        STRADA_TRY_POP();
        vm_worker_leave_safe(vm, _prev_safe);
        vm->gc_pause--;
        return 0;
    } else {
        STRADA_TRY_POP();
        vm_worker_leave_safe(vm, _prev_safe);
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
    volatile int _prev_safe = vm_worker_enter_safe(vm);
    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        StradaValue *r = fn();
        STRADA_TRY_POP();
        vm_worker_leave_safe(vm, _prev_safe);
        vm->gc_pause--;
        *out_val = r ? sv_to_vm(r) : VM_UNDEF_VAL;
        if (r) strada_decref(r);
        return 0;
    } else {
        STRADA_TRY_POP();
        vm_worker_leave_safe(vm, _prev_safe);
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
    volatile int _prev_safe = vm_worker_enter_safe(vm);
    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        StradaValue *r = fn(sv0);
        STRADA_TRY_POP();
        vm_worker_leave_safe(vm, _prev_safe);
        vm->gc_pause--;
        *out_val = r ? sv_to_vm(r) : VM_UNDEF_VAL;
        if (r) strada_decref(r);
        strada_cleanup_drain_to(cm);
        return 0;
    } else {
        STRADA_TRY_POP();
        vm_worker_leave_safe(vm, _prev_safe);
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
    volatile int _prev_safe = vm_worker_enter_safe(vm);
    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        StradaValue *r = fn(sv0, sv1);
        STRADA_TRY_POP();
        vm_worker_leave_safe(vm, _prev_safe);
        vm->gc_pause--;
        *out_val = r ? sv_to_vm(r) : VM_UNDEF_VAL;
        if (r) strada_decref(r);
        strada_cleanup_drain_to(cm);
        return 0;
    } else {
        STRADA_TRY_POP();
        vm_worker_leave_safe(vm, _prev_safe);
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
    volatile int _prev_safe = vm_worker_enter_safe(vm);
    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        StradaValue *r = fn(sv0, sv1, sv2);
        STRADA_TRY_POP();
        vm_worker_leave_safe(vm, _prev_safe);
        vm->gc_pause--;
        *out_val = r ? sv_to_vm(r) : VM_UNDEF_VAL;
        if (r) strada_decref(r);
        strada_cleanup_drain_to(cm);
        return 0;
    } else {
        STRADA_TRY_POP();
        vm_worker_leave_safe(vm, _prev_safe);
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
    volatile int _prev_safe = vm_worker_enter_safe(vm);
    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        fn(sv0);
        STRADA_TRY_POP();
        vm_worker_leave_safe(vm, _prev_safe);
        vm->gc_pause--;
        strada_cleanup_drain_to(cm);
        return 0;
    } else {
        STRADA_TRY_POP();
        vm_worker_leave_safe(vm, _prev_safe);
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
    volatile int _prev_safe = vm_worker_enter_safe(vm);
    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        StradaValue *r = strada_utf8_downgrade(sv0, fail_ok);
        STRADA_TRY_POP();
        vm_worker_leave_safe(vm, _prev_safe);
        vm->gc_pause--;
        *out_val = r ? sv_to_vm(r) : VM_UNDEF_VAL;
        if (r) strada_decref(r);
        strada_cleanup_drain_to(cm);
        return 0;
    } else {
        STRADA_TRY_POP();
        vm_worker_leave_safe(vm, _prev_safe);
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
    volatile int _prev_safe = vm_worker_enter_safe(vm);
    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        strada_dispatch_signals();
        STRADA_TRY_POP();
        vm_worker_leave_safe(vm, _prev_safe);
        vm->gc_pause--;
        strada_cleanup_drain_to(cm);
        return 0;
    } else {
        STRADA_TRY_POP();
        vm_worker_leave_safe(vm, _prev_safe);
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
    volatile int _prev_safe = vm_worker_enter_safe(vm);
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
        vm_worker_leave_safe(vm, _prev_safe);
        vm->gc_pause--;
        *out_r = r;
        return 0;
    } else {
        STRADA_TRY_POP();
        vm_worker_leave_safe(vm, _prev_safe);
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
    volatile int _prev_safe = vm_worker_enter_safe(vm);
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
        vm_worker_leave_safe(vm, _prev_safe);
        vm->gc_pause--;
        *out_nr = nr;
        return 0;
    } else {
        STRADA_TRY_POP();
        vm_worker_leave_safe(vm, _prev_safe);
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

/* lib/Eval: called before (phase 0) and after (phase 1) host code runs a
 * closure or method of an eval-session VM that is not the active one, so the
 * VM's globals are synced with the session (V5-6). NULL outside lib/Eval. */
void (*vm_foreign_vm_call_hook)(VM *target, int phase) = NULL;

/* Run VM code (closure VCL, or function FUNC_IDX when VCL is NULL) of VM
 * with host arguments; the result (or the thrown value) converts as a
 * scalar: a container comes back as a reference. */
static StradaValue *vm_foreign_call(VM *vm, VMClosure *vcl, int func_idx, int argc, StradaValue **argv) {
    int hook = (vm_foreign_vm_call_hook && vm->eval_epoch != 0 && vm != g_active_vm) ? 1 : 0;
    if (hook) vm_foreign_vm_call_hook(vm, 0);
    int use_worker = !(!vm->is_worker_vm && pthread_equal(pthread_self(), vm->owner_thread));
    /* a green task (coroutine) gets its own worker VM: it may suspend in
     * the middle of its VM code while the main VM keeps running */
    int coro_worker = 0;
    if (!use_worker && strada_coro_in_task()) { use_worker = 1; coro_worker = 1; }
    VM *run_vm = use_worker ? vm_new_worker(vm) : vm;
    /* a coroutine worker shares this OS thread: hand back the VM that was
     * active when the task started */
    VM *prev_active = coro_worker ? g_active_vm : (use_worker ? run_vm->saved_active_vm : g_active_vm);
    g_active_vm = run_vm;
    int prev_trap = run_vm->trap_uncaught;
    run_vm->trap_uncaught = 1;
    if (use_worker) vm_worker_enter_active(run_vm);
    run_vm->gc_pause++;
    VMValue vargs_buf[16];
    VMValue *vargs = (argc > 16) ? malloc((size_t)argc * sizeof(VMValue)) : vargs_buf;
    vm_conv_begin();
    for (int i = 0; i < argc; i++) {
        vargs[i] = argv[i] ? sv_to_vm(argv[i]) : VM_UNDEF_VAL;
    }
    vm_conv_end();
    run_vm->gc_pause--;
    VMValue vres = vcl ? vm_invoke_closure(run_vm, vcl, vargs, argc)
                       : vm_execute_call(run_vm, func_idx, vargs, argc);
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
        out_sv = vm_value_to_sv_scalar(vexc);
        run_vm->gc_pause--;
    } else {
        run_vm->gc_pause++;
        out_sv = vm_value_to_sv_scalar(vres);
        run_vm->gc_pause--;
    }
    if (!use_worker && run_vm->gc_count >= run_vm->gc_threshold &&
        run_vm->exec_base == 0 && run_vm->gc_pause == 0) {
        vm_gc_collect(run_vm);
    }
    run_vm->trap_uncaught = prev_trap;
    if (use_worker) {
        vm_worker_enter_safe(run_vm);
        vm_suspend(run_vm);
        vm_free(run_vm);
    }
    g_active_vm = prev_active;
    if (hook) vm_foreign_vm_call_hook(vm, 1);
    if (had_exc) {
        strada_throw_value(out_sv);
    }
    return out_sv;
}

static StradaValue* vm_exported_closure_call(void *raw, int argc, StradaValue **argv) {
    VMExportedClosureCtx *ctx = (VMExportedClosureCtx *)raw;
    if (!ctx || !ctx->vm || !ctx->vcl) return strada_new_undef();
    return vm_foreign_call(ctx->vm, ctx->vcl, 0, argc, argv);
}

/* lib/Eval: host dispatch of a method of a class defined in eval code (V5-9).
 * Returns NULL when VM's program has no such method. SELF_AND_ARGS[0] is the
 * invocant. Throws what the method throws. */
StradaValue *vm_host_method_call(VM *vm, const char *cls, const char *method, int argc, StradaValue **self_and_args) {
    if (!vm || !vm->program || !cls || !method) return NULL;
    int idx = vm_find_method(vm->program, cls, method);
    if (idx < 0) return NULL;
    return vm_foreign_call(vm, NULL, idx, argc, self_and_args);
}

/* async call / async::spawn: a runtime closure that calls the exported VM
 * closure with the captured argument list, run by strada_future_new on the
 * runtime pool (in a worker VM). */
typedef struct { StradaValue *fn; StradaValue *args; } VMAsyncCallCtx;
static StradaValue *vm_async_call_cb(void *raw, int argc, StradaValue **argv) {
    (void)argc; (void)argv;
    VMAsyncCallCtx *c = (VMAsyncCallCtx *)raw;
    if (!c || !c->fn) return strada_new_undef();
    return strada_closure_call_array(c->fn, c->args);
}
static void vm_async_call_free(void *raw) {
    VMAsyncCallCtx *c = (VMAsyncCallCtx *)raw;
    if (!c) return;
    if (c->fn) strada_decref(c->fn);
    if (c->args) strada_decref(c->args);
    free(c);
}

/* `local $g` is per green task: while a task is suspended its local values
 * are taken out (the outer values put back) and reinstated on resume -- the
 * compiled runtime's coro_local_suspend. dir 0 = suspend (newest first),
 * 1 = resume (oldest first); each pass swaps global <-> saved value. */
static void vm_task_locals_swap(VM *vm, int dir) {
    if (!vm || vm->global_save_count <= 0 || !vm->globals) return;
    pthread_mutex_t *glk = vm_globals_lock_ptr(vm);
    if (glk) pthread_mutex_lock(glk);
    int n = vm->global_save_count;
    for (int k = 0; k < n; k++) {
        int i = dir == 0 ? (n - 1 - k) : k;
        VMGlobalSave *gs = &vm->global_saves[i];
        if (gs->is_hash_elem || !gs->name) continue;
        VMHash *gsh = vm_gh_for(vm, gs->name);
        VMValue cur = vm_val_copy(vm_hash_get(gsh, gs->name));
        vm_hash_set(gsh, gs->name, gs->saved_value);
        gs->saved_value = cur;
    }
    if (glk) pthread_mutex_unlock(glk);
}

static StradaValue *vm_channel_send_rt(StradaValue *ch, StradaValue *v) {
    strada_channel_send(ch, v);
    return strada_new_undef();
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

/* scalar(%tied): the SCALAR method's result, else whether FIRSTKEY gives a
 * key (Perl), else the size. */
static VMValue vm_tied_hash_scalar(VM *vm, VMHash *h) {
    int sfidx = vm_tied_array_find_method(vm, h->tied_obj, "SCALAR");
    if (sfidx >= 0) return vm_tied_array_call(vm, h->tied_obj, sfidx, NULL, 0);
    int fidx = vm_tied_array_find_method(vm, h->tied_obj, "FIRSTKEY");
    if (fidx >= 0) {
        VMValue fk = vm_tied_array_call(vm, h->tied_obj, fidx, NULL, 0);
        return VM_MAKE_INT(VM_IS_UNDEF(fk) ? 0 : 1);
    }
    return VM_MAKE_INT((int64_t)h->size);
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

static int64_t vm_to_int_vm(VM *vm, VMValue v) {
    for (int depth = 0; depth < 16 && VM_IS_PTR(v); depth++) {
        enum VMObjType t = VM_PTR_TYPE(v);
        if (t == VM_OBJ_CELL) v = VM_AS_CELL(v)->val;
        else if (t == VM_OBJ_WEAKREF) v = VM_AS_WEAKREF(v)->target;
        else if (t == VM_OBJ_TIED_SCALAR && vm) {
            v = vm_tied_scalar_fetch(vm, v);
            if (vm->has_pending_exc) return 0;
        } else break;
    }
    /* a "0+" overload gives the number (Perl / strada_to_int) */
    if (vm && vm->program && vm->program->overload_count > 0 && vm_obj_class_name(v) != NULL) {
        int nf = vm_resolve_overload_func(vm->program, vm_obj_class_name(v), "0+");
        if (nf >= 0) {
            VMValue nargs[1] = { v };
            vm->gc_pause++;
            VMValue nr = vm_invoke_func(vm, nf, nargs, 1);
            vm->gc_pause--;
            if (vm->has_pending_exc) return 0;
            return (vm_obj_class_name(nr) == NULL) ? vm_to_int_vm(vm, nr) : vm_to_int(nr);
        }
    }
    /* an object with only a "" overload numifies its string (Perl) */
    if (vm && vm->program && vm_obj_class_name(v) != NULL && vm_has_stringify(vm->program, v) &&
        vm_resolve_overload_func(vm->program, vm_obj_class_name(v), "0+") < 0) {
        VMValue sv_s = vm_stringify_ov(vm, v);
        if (vm->has_pending_exc) return 0;
        int64_t r_s = vm_to_int(sv_s);
        vm_temp_free(&sv_s);
        return r_s;
    }
    if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_ARRAY && vm) {
        VMArray *a = VM_AS_ARRAY(v);
        if (!VM_IS_UNDEF(a->tied_obj)) return vm_tied_array_fetchsize(vm, a->tied_obj);
        return a->size;
    }
    return vm_to_int(v);
}

static double vm_to_num_vm(VM *vm, VMValue v) {
    for (int depth = 0; depth < 16 && VM_IS_PTR(v); depth++) {
        enum VMObjType t = VM_PTR_TYPE(v);
        if (t == VM_OBJ_CELL) v = VM_AS_CELL(v)->val;
        else if (t == VM_OBJ_WEAKREF) v = VM_AS_WEAKREF(v)->target;
        else if (t == VM_OBJ_TIED_SCALAR && vm) {
            v = vm_tied_scalar_fetch(vm, v);
            if (vm->has_pending_exc) return 0.0;
        } else break;
    }
    /* a "0+" overload gives the number (Perl / strada_to_num) */
    if (vm && vm->program && vm->program->overload_count > 0 && vm_obj_class_name(v) != NULL) {
        int nf = vm_resolve_overload_func(vm->program, vm_obj_class_name(v), "0+");
        if (nf >= 0) {
            VMValue nargs[1] = { v };
            vm->gc_pause++;
            VMValue nr = vm_invoke_func(vm, nf, nargs, 1);
            vm->gc_pause--;
            if (vm->has_pending_exc) return 0.0;
            return (vm_obj_class_name(nr) == NULL) ? vm_to_num_vm(vm, nr) : vm_to_num(nr);
        }
    }
    /* an object with only a "" overload numifies its string (Perl) */
    if (vm && vm->program && vm_obj_class_name(v) != NULL && vm_has_stringify(vm->program, v) &&
        vm_resolve_overload_func(vm->program, vm_obj_class_name(v), "0+") < 0) {
        VMValue sv_s = vm_stringify_ov(vm, v);
        if (vm->has_pending_exc) return 0;
        double r_s = vm_to_num(sv_s);
        vm_temp_free(&sv_s);
        return r_s;
    }
    if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_ARRAY && vm) {
        VMArray *a = VM_AS_ARRAY(v);
        if (!VM_IS_UNDEF(a->tied_obj)) return (double)vm_tied_array_fetchsize(vm, a->tied_obj);
        return (double)a->size;
    }
    return vm_to_num(v);
}

static int vm_pair_to_num(VM *vm, VMValue a, VMValue b, double *out_a, double *out_b) {
    if (!VM_IS_PTR(a) && !VM_IS_PTR(b)) {
        *out_a = vm_to_num(a);
        *out_b = vm_to_num(b);
        return 0;
    }
    vm->gc_pause++;
    *out_a = vm_to_num_vm(vm, a);
    *out_b = vm->has_pending_exc ? 0.0 : vm_to_num_vm(vm, b);
    vm->gc_pause--;
    return vm->has_pending_exc;
}

static int vm_pair_to_int(VM *vm, VMValue a, VMValue b, int64_t *out_a, int64_t *out_b) {
    if (!VM_IS_PTR(a) && !VM_IS_PTR(b)) {
        *out_a = vm_to_int(a);
        *out_b = vm_to_int(b);
        return 0;
    }
    vm->gc_pause++;
    *out_a = vm_to_int_vm(vm, a);
    *out_b = vm->has_pending_exc ? 0 : vm_to_int_vm(vm, b);
    vm->gc_pause--;
    return vm->has_pending_exc;
}

static VMValue vm_sref_fetch_val(VM *vm, VMValue ref) {
    if (__builtin_expect(vm->has_weakrefs && VM_IS_PTR(ref) && VM_PTR_TYPE(ref) == VM_OBJ_WEAKREF, 0)) {
        ref = vm_unwrap_weak_gc(vm, ref, NULL, 0);
    }
    VMValue tv = VM_UNDEF_VAL;
    if (VM_IS_PTR(ref) && VM_PTR_TYPE(ref) == VM_OBJ_SREF) {
        VMScalarRef *sr = VM_AS_SREF(ref);
        if (sr->global_name) {
            tv = vm_globals_get(vm, sr->global_name);
        } else if (VM_IS_PTR(sr->cell) && VM_PTR_TYPE(sr->cell) == VM_OBJ_CELL) {
            tv = VM_AS_CELL(sr->cell)->val;
        }
    } else if (VM_IS_PTR(ref) && VM_PTR_TYPE(ref) == VM_OBJ_CELL) {
        tv = VM_AS_CELL(ref)->val;
    } else if (VM_IS_PTR(ref) && VM_PTR_TYPE(ref) == VM_OBJ_TIED_SCALAR) {
        tv = ref;
    } else if (VM_IS_PTR(ref) && VM_PTR_TYPE(ref) == VM_OBJ_NATIVE_SV) {
        VMNativeSV *nsv = (VMNativeSV *)VM_TO_PTR(ref);
        if (nsv->sv && !STRADA_IS_TAGGED_INT(nsv->sv) && nsv->sv->type == STRADA_REF) {
            StradaValue *d = strada_deref(nsv->sv);
            VMValue res = sv_to_vm(d);
            strada_decref(d);
            return res;
        }
        return vm_val_copy(ref);
    } else {
        return vm_val_copy(ref);
    }
    if (__builtin_expect(VM_IS_PTR(tv) && VM_PTR_TYPE(tv) == VM_OBJ_TIED_SCALAR, 0)) {
        vm->gc_pause++;
        VMValue fv = vm_tied_scalar_fetch(vm, tv);
        vm->gc_pause--;
        return fv;
    }
    if (__builtin_expect(vm->has_weakrefs && VM_IS_PTR(tv) && VM_PTR_TYPE(tv) == VM_OBJ_WEAKREF, 0)) {
        tv = vm_unwrap_weak_gc(vm, tv, &ref, 1);
    }
    return vm_val_copy(tv);
}

static void vm_sref_store_val(VM *vm, VMValue ref, VMValue newv) {
    if (__builtin_expect(vm->has_weakrefs && VM_IS_PTR(ref) && VM_PTR_TYPE(ref) == VM_OBJ_WEAKREF, 0)) {
        ref = vm_unwrap_weak_gc(vm, ref, &newv, 1);
    }
    if (VM_IS_PTR(ref) && VM_PTR_TYPE(ref) == VM_OBJ_SREF) {
        VMScalarRef *sr = VM_AS_SREF(ref);
        if (sr->global_name) {
            VMValue tv = vm_globals_get(vm, sr->global_name);
            if (__builtin_expect(VM_IS_PTR(tv) && VM_PTR_TYPE(tv) == VM_OBJ_TIED_SCALAR, 0)) {
                vm->gc_pause++;
                vm_tied_scalar_store(vm, tv, newv);
                vm->gc_pause--;
            } else {
                vm_globals_set(vm, sr->global_name, vm_val_copy(newv));
            }
        } else if (VM_IS_PTR(sr->cell) && VM_PTR_TYPE(sr->cell) == VM_OBJ_CELL) {
            VMValue old_cv = VM_AS_CELL(sr->cell)->val;
            if (__builtin_expect(VM_IS_PTR(old_cv) && VM_PTR_TYPE(old_cv) == VM_OBJ_TIED_SCALAR, 0)) {
                vm->gc_pause++;
                vm_tied_scalar_store(vm, old_cv, newv);
                vm->gc_pause--;
            } else {
                vm_temp_free(&old_cv);
                VM_AS_CELL(sr->cell)->val = vm_val_copy(newv);
            }
        }
    } else if (VM_IS_PTR(ref) && VM_PTR_TYPE(ref) == VM_OBJ_CELL) {
        VMValue old_cv = VM_AS_CELL(ref)->val;
        if (__builtin_expect(VM_IS_PTR(old_cv) && VM_PTR_TYPE(old_cv) == VM_OBJ_TIED_SCALAR, 0)) {
            vm->gc_pause++;
            vm_tied_scalar_store(vm, old_cv, newv);
            vm->gc_pause--;
        } else {
            vm_temp_free(&old_cv);
            VM_AS_CELL(ref)->val = vm_val_copy(newv);
        }
    } else if (VM_IS_PTR(ref) && VM_PTR_TYPE(ref) == VM_OBJ_NATIVE_SV) {
        VMNativeSV *nsv = (VMNativeSV *)VM_TO_PTR(ref);
        if (nsv->sv) {
            StradaValue *nv = vm_to_sv(newv);
            strada_deref_set(nsv->sv, nv);
            strada_decref(nv);
        }
    }
}

/* Flush an open_memstream handle backed by a scalar ref back to its scalar. */
static void vm_fh_sync_mem_sref(VM *vm, VMFileHandle *fh) {
    if (!fh || fh->is_mem != 2 || !VM_IS_PTR(fh->mem_sref)) return;
    if (fh->fp) fflush((FILE*)fh->fp);
    vm->gc_pause++;
    VMValue sv = VM_MAKE_STR_N(fh->mem_buf ? fh->mem_buf : "", fh->mem_len);
    vm_sref_store_val(vm, fh->mem_sref, sv);
    vm->gc_pause--;
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
            vm_array_push(flat, vm_hash_make_key_str_b(h->entries[i].key, h->entries[i].keylen, h->entries[i].key_bytes));
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
    for (int d = 0; d < 16 && VM_IS_PTR(v); d++) {
        enum VMObjType t = VM_PTR_TYPE(v);
        if (t == VM_OBJ_CELL) v = VM_AS_CELL(v)->val;
        else if (t == VM_OBJ_WEAKREF) v = VM_AS_WEAKREF(v)->target;
        else if (t == VM_OBJ_TIED_SCALAR && vm) {
            v = vm_tied_scalar_fetch(vm, v);
            if (vm->has_pending_exc) return 0.0;
        } else break;
    }
    if (depth < 8 && vm && vm->program && vm_obj_class_name(v) != NULL) {
        VMValue ov = VM_UNDEF_VAL;
        if (vm_try_un_overload(vm, "0+", v, &ov)) {
            if (vm->has_pending_exc) return 0.0;
            return vm_to_num_ov(vm, ov, depth + 1);
        }
    }
    return vm_to_num_vm(vm, v);
}

/* Integer coercion that honors a "0+" overload on a blessed value (the
 * int-variable store path: `my int $x op= $obj`), like strada_to_int. */
static int64_t vm_to_int_ov(VM *vm, VMValue v, int depth) {
    for (int d = 0; d < 16 && VM_IS_PTR(v); d++) {
        enum VMObjType t = VM_PTR_TYPE(v);
        if (t == VM_OBJ_CELL) v = VM_AS_CELL(v)->val;
        else if (t == VM_OBJ_WEAKREF) v = VM_AS_WEAKREF(v)->target;
        else if (t == VM_OBJ_TIED_SCALAR && vm) {
            v = vm_tied_scalar_fetch(vm, v);
            if (vm->has_pending_exc) return 0;
        } else break;
    }
    if (depth < 8 && vm && vm->program && vm_obj_class_name(v) != NULL) {
        VMValue ov = VM_UNDEF_VAL;
        if (vm_try_un_overload(vm, "0+", v, &ov)) {
            if (vm->has_pending_exc) return 0;
            return vm_to_int_ov(vm, ov, depth + 1);
        }
    }
    return vm_to_int_vm(vm, v);
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

    int64_t offset = 0;
    if (argc >= 2 && !VM_IS_UNDEF(args[1])) {
        offset = VM_IS_PTR(args[1]) ? vm_to_int_vm(vm, args[1]) : vm_to_int(args[1]);
        if (vm->has_pending_exc) { vm->gc_pause--; return vm_array_new(0); }
    }
    if (offset < 0) offset = sz + offset;
    if (offset < 0) offset = 0;
    if (offset > sz) offset = sz;

    int64_t length;
    if (argc >= 3 && !VM_IS_UNDEF(args[2])) {
        length = VM_IS_PTR(args[2]) ? vm_to_int_vm(vm, args[2]) : vm_to_int(args[2]);
        if (vm->has_pending_exc) { vm->gc_pause--; return vm_array_new(0); }
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
    for (int depth = 0; depth < 16 && VM_IS_PTR(v); depth++) {
        enum VMObjType t = VM_PTR_TYPE(v);
        if (t == VM_OBJ_CELL) v = VM_AS_CELL(v)->val;
        else if (t == VM_OBJ_WEAKREF) v = VM_AS_WEAKREF(v)->target;
        else if (t == VM_OBJ_TIED_SCALAR) {
            v = vm_tied_scalar_fetch(vm, v);
            if (vm->has_pending_exc) return 1;
        } else break;
    }
    if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_ARRAY) {
        VMArray *a = VM_AS_ARRAY(v);
        if (!VM_IS_UNDEF(a->tied_obj)) {
            int64_t sz = vm_tied_array_fetchsize(vm, a->tied_obj);
            if (vm->has_pending_exc) return 1;
            *out = VM_MAKE_INT(sz < 0 ? -sz : sz);
            return 0;
        }
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
        /* autogenerated from "-" (perlovload): $x < 0 ? 0 - $x : $x, the
         * comparison through an overloaded "<" / "<=>" when there is one */
        const char *acls = vm_obj_class_name(v);
        if (vm_resolve_overload_func(vm->program, acls, "-") >= 0) {
            VMValue cr = VM_UNDEF_VAL;
            int neg;
            vm->gc_pause++;
            if (vm_try_bin_overload(vm, "<", v, VM_MAKE_INT(0), &cr)) {
                neg = vm->has_pending_exc ? 0 : vm_to_bool(cr);
            } else {
                neg = vm_to_num_vm(vm, v) < 0;
            }
            if (!vm->has_pending_exc && neg) {
                VMValue nr = VM_UNDEF_VAL;
                vm_try_bin_overload(vm, "-", VM_MAKE_INT(0), v, &nr);
                vm->gc_pause--;
                if (vm->has_pending_exc) return 1;
                *out = nr;
                return 0;
            }
            vm->gc_pause--;
            if (vm->has_pending_exc) return 1;
            *out = v;
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
    volatile int _prev_safe = vm_worker_enter_safe(vm);
    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        StradaValue *s_arg = slot;
        StradaValue *r = NULL;
        if (mode == 0) r = strada_utf8_decode_slot(&s_arg);
        else if (mode == 1) r = strada_utf8_downgrade_slot(&s_arg, fail_ok);
        else r = strada_utf8_upgrade_slot(&s_arg);
        slot = s_arg;
        STRADA_TRY_POP();
        vm_worker_leave_safe(vm, _prev_safe);
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
        vm_worker_leave_safe(vm, _prev_safe);
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

static __attribute__((noinline)) int vm_slow_unwrap_local(VM *vm, VMValue *val_io) {
    VMValue v = *val_io;
    if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_CELL) v = VM_AS_CELL(v)->val;
    if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_TIED_SCALAR) {
        v = vm_tied_scalar_fetch(vm, v);
        if (__builtin_expect(vm->has_pending_exc, 0)) return 1;
    } else if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_WEAKREF) {
        v = vm_unwrap_weak_gc(vm, v, NULL, 0);
    }
    *val_io = v;
    return 0;
}

static __attribute__((noinline)) VMValue vm_slow_load_local_ptr(VM *vm, VMValue v) {
    enum VMObjType t = VM_PTR_TYPE(v);
    if (t == VM_OBJ_CELL) {
        VMValue cv = VM_AS_CELL(v)->val;
        if (__builtin_expect(VM_IS_PTR(cv) && VM_PTR_TYPE(cv) == VM_OBJ_TIED_SCALAR, 0)) {
            return vm_tied_scalar_fetch(vm, cv);
        }
        if (__builtin_expect(VM_IS_PTR(cv) && VM_PTR_TYPE(cv) == VM_OBJ_WEAKREF, 0)) {
            cv = vm_unwrap_weak_gc(vm, cv, NULL, 0);
        }
        return vm_val_copy(cv);
    }
    if (t == VM_OBJ_TIED_SCALAR) {
        return vm_tied_scalar_fetch(vm, v);
    }
    if (t == VM_OBJ_WEAKREF) {
        VMValue u = vm_unwrap_weak_gc(vm, v, NULL, 0);
        return vm_val_copy(u);
    }
    return v;
}

static __attribute__((noinline)) void vm_slow_store_local(VM *vm, uint16_t slot, VMValue old, VMValue newv) {
    if (__builtin_expect(vm->has_weakrefs && VM_IS_PTR(newv) && VM_PTR_TYPE(newv) == VM_OBJ_WEAKREF, 0)) {
        newv = vm_unwrap_weak_gc(vm, newv, &old, 1);
        old = vm->stack[vm->frames[vm->frame_count - 1].local_base + slot];
    }
    VMValue *cur_locals = vm->stack + vm->frames[vm->frame_count - 1].local_base;
    if (__builtin_expect(VM_IS_PTR(old) && VM_PTR_TYPE(old) == VM_OBJ_CELL, 0)) {
        VMValue old_cell_val = VM_AS_CELL(old)->val;
        if (__builtin_expect(VM_IS_PTR(old_cell_val) && VM_PTR_TYPE(old_cell_val) == VM_OBJ_TIED_SCALAR, 0)) {
            vm_tied_scalar_store(vm, old_cell_val, newv);
            return;
        }
        vm_temp_free(&old_cell_val);
        VM_AS_CELL(old)->val = newv;
        return;
    }
    if (__builtin_expect(VM_IS_PTR(old) && VM_PTR_TYPE(old) == VM_OBJ_TIED_SCALAR, 0)) {
        vm_tied_scalar_store(vm, old, newv);
        return;
    }
    vm_temp_free(&old);
    cur_locals[slot] = newv;
}

/* Exact integer arithmetic (the runtime's IV -> UV -> NV promotion):
 * operands that are integers (inline, boxed int64, boxed uint64, undef, or
 * integer-looking strings) are combined in 128 bits; the result is an int64,
 * a uint64, or a double when it fits neither. */
static int vm_exact_int_operand(VMValue v, __int128 *out) {
    if (VM_IS_UNDEF(v)) { *out = 0; return 1; }
    if (VM_IS_ANY_INT(v)) { *out = (__int128)VM_ANY_INT_VAL(v); return 1; }
    if (!VM_IS_PTR(v)) return 0;
    enum VMObjType t = VM_PTR_TYPE(v);
    if (t == VM_OBJ_UINT64) { *out = (__int128)VM_AS_UINT64(v)->val; return 1; }
    if (t == VM_OBJ_STR || t == VM_OBJ_STRBUF) {
        char b[64];
        size_t l = 0;
        const char *p = vm_bytes_view(v, b, sizeof(b), &l);
        if (!p || l == 0) return 0;
        if (l > 21) {
            /* a long all-digit string is still a number (a double) */
            size_t k = 0;
            if (p[k] == '+' || p[k] == '-') k++;
            if (k >= l) return 0;
            for (; k < l; k++) if (p[k] < '0' || p[k] > '9') return 0;
            return -1;
        }
        size_t i = 0;
        int neg = 0;
        while (i < l && (p[i] == ' ' || p[i] == '\t' || p[i] == '\n')) i++;
        if (i < l && (p[i] == '+' || p[i] == '-')) { neg = (p[i] == '-'); i++; }
        if (i >= l) return 0;
        unsigned __int128 acc = 0;
        size_t d0 = i;
        while (i < l && p[i] >= '0' && p[i] <= '9') { acc = acc * 10 + (unsigned)(p[i] - '0'); i++; }
        if (i == d0 || i != l) return 0;
        if (acc > (unsigned __int128)UINT64_MAX) return -1;   /* integer, but only a double holds it */
        *out = neg ? -(__int128)acc : (__int128)acc;
        return 1;
    }
    return 0;
}

static VMValue vm_from_int128(__int128 r) {
    if (r >= (__int128)INT64_MIN && r <= (__int128)INT64_MAX) return VM_MAKE_INT((int64_t)r);
    if (r > 0 && r <= (__int128)UINT64_MAX) return vm_box_uint64((uint64_t)r);
    return VM_MAKE_FLOAT((double)r);
}

/* op_kind: 0 add, 1 sub, 2 mul. Returns 1 and *out when both are exact. */
static int vm_exact_int_arith(int op_kind, VMValue a, VMValue b, VMValue *out) {
    __int128 x = 0, y = 0, r = 0;
    int ka = vm_exact_int_operand(a, &x), kb = vm_exact_int_operand(b, &y);
    if (ka == 0 || kb == 0) return 0;
    if (ka < 0 || kb < 0) {
        double da = vm_to_num(a), db = vm_to_num(b);
        *out = VM_MAKE_FLOAT(op_kind == 0 ? da + db : op_kind == 1 ? da - db : da * db);
        return 1;
    }
    if (op_kind == 0) r = x + y;
    else if (op_kind == 1) r = x - y;
    else if (__builtin_mul_overflow(x, y, &r)) { *out = VM_MAKE_FLOAT((double)x * (double)y); return 1; }
    *out = vm_from_int128(r);
    return 1;
}

static __attribute__((noinline)) VMValue vm_slow_bin_arith(VM *vm, int op_kind, VMValue a, VMValue b) {
    if (VM_IS_PTR(a) && vm_slow_unwrap_local(vm, &a)) return VM_UNDEF_VAL;
    if (VM_IS_PTR(b)) {
        if (vm->stack_top >= vm->stack_cap) vm_stack_grow(vm, vm->stack_top);
        vm->stack[vm->stack_top++] = a;
        int exc = vm_slow_unwrap_local(vm, &b);
        a = vm->stack[--vm->stack_top];
        if (exc) return VM_UNDEF_VAL;
    }
    const char *opstr = (op_kind == 0) ? "+" : (op_kind == 1) ? "-" : "*";
    if (__builtin_expect(vm_obj_class_name(a) != NULL || vm_obj_class_name(b) != NULL, 0)) {
        VMValue ov = VM_UNDEF_VAL;
        if (vm_try_bin_overload(vm, opstr, a, b, &ov)) {
            return ov;
        }
        if (vm->has_pending_exc) return VM_UNDEF_VAL;
    }
    {
        VMValue ex = VM_UNDEF_VAL;
        if (vm_exact_int_arith(op_kind, a, b, &ex)) return ex;
    }
    if (!vm_is_float_operand(a) && !vm_is_float_operand(b)) {
        int64_t ia = 0, ib = 0;
        if (vm_pair_to_int(vm, a, b, &ia, &ib)) return VM_UNDEF_VAL;
        __int128 r = (op_kind == 0) ? (__int128)ia + ib : (op_kind == 1) ? (__int128)ia - ib : (__int128)ia * ib;
        return vm_from_int128(r);
    }
    double da = 0.0, db = 0.0;
    if (vm_pair_to_num(vm, a, b, &da, &db)) return VM_UNDEF_VAL;
    return VM_MAKE_FLOAT((op_kind == 0) ? (da + db) : (op_kind == 1) ? (da - db) : (da * db));
}

static __attribute__((noinline)) VMValue vm_slow_local_imm_arith(VM *vm, VMValue a, int32_t imm, int op_kind) {
    if (VM_IS_PTR(a) && vm_slow_unwrap_local(vm, &a)) return VM_UNDEF_VAL;
    const char *opstr = (op_kind == 0) ? "-" : (op_kind == 1) ? "+" : (op_kind == 2) ? "&" : "%";
    if (__builtin_expect(vm_obj_class_name(a) != NULL, 0)) {
        VMValue ov = VM_UNDEF_VAL;
        if (vm_try_bin_overload(vm, opstr, a, VM_MAKE_INT((int64_t)imm), &ov)) {
            return ov;
        }
        if (vm->has_pending_exc) return VM_UNDEF_VAL;
    }
    if (op_kind == 2) {
        int64_t ia = vm_to_int_vm(vm, a);
        if (vm->has_pending_exc) return VM_UNDEF_VAL;
        return VM_MAKE_INT(ia & (int64_t)imm);
    }
    if (op_kind == 3) {
        int64_t bv = (int64_t)imm;
        if (bv == 0) {
            vm->pending_exc = vm_str("Illegal modulus zero");
            vm->has_pending_exc = 1;
            return VM_UNDEF_VAL;
        }
        int64_t av = vm_to_int_vm(vm, a);
        if (vm->has_pending_exc) return VM_UNDEF_VAL;
        int64_t r = 0;
        if (!(av == INT64_MIN && bv == -1)) {
            r = av % bv;
            if (r != 0 && ((r < 0) != (bv < 0))) r += bv;
        }
        return VM_MAKE_INT(r);
    }
    {
        VMValue ex = VM_UNDEF_VAL;
        if (vm_exact_int_arith(op_kind == 0 ? 1 : 0, a, VM_MAKE_INT((int64_t)imm), &ex)) return ex;
    }
    if (!vm_is_float_operand(a)) {
        int64_t ia = vm_to_int_vm(vm, a);
        if (vm->has_pending_exc) return VM_UNDEF_VAL;
        __int128 r = (op_kind == 0) ? (__int128)ia - imm : (__int128)ia + imm;
        return vm_from_int128(r);
    }
    double da = vm_to_num_vm(vm, a);
    if (vm->has_pending_exc) return VM_UNDEF_VAL;
    return VM_MAKE_FLOAT((op_kind == 0) ? (da - (double)imm) : (da + (double)imm));
}

static __attribute__((noinline)) VMValue vm_slow_add_locals(VM *vm, VMValue va, VMValue vb) {
    if (VM_IS_PTR(va)) {
        if (vm_slow_unwrap_local(vm, &va)) return VM_UNDEF_VAL;
    }
    if (VM_IS_PTR(vb)) {
        if (vm->stack_top >= vm->stack_cap) vm_stack_grow(vm, vm->stack_top);
        vm->stack[vm->stack_top++] = va;
        int exc = vm_slow_unwrap_local(vm, &vb);
        va = vm->stack[--vm->stack_top];
        if (exc) return VM_UNDEF_VAL;
    }
    if (__builtin_expect(vm_obj_class_name(va) != NULL || vm_obj_class_name(vb) != NULL, 0)) {
        VMValue ov = VM_UNDEF_VAL;
        if (vm_try_bin_overload(vm, "+", va, vb, &ov)) return ov;
        if (vm->has_pending_exc) return VM_UNDEF_VAL;
    }
    if (!vm_is_float_operand(va) && !vm_is_float_operand(vb)) {
        int64_t ia = 0, ib = 0;
        if (vm_pair_to_int(vm, va, vb, &ia, &ib)) return VM_UNDEF_VAL;
        return VM_MAKE_INT((int64_t)((uint64_t)ia + (uint64_t)ib));
    }
    double da = 0.0, db = 0.0;
    if (vm_pair_to_num(vm, va, vb, &da, &db)) return VM_UNDEF_VAL;
    return VM_MAKE_FLOAT(da + db);
}

static __attribute__((noinline)) int vm_slow_cmp_local_i32(VM *vm, VMValue v, int32_t imm, int is_le) {
    if (VM_IS_PTR(v) && vm_slow_unwrap_local(vm, &v)) return 0;
    if (__builtin_expect(vm_obj_class_name(v) != NULL, 0)) {
        VMValue ov = VM_UNDEF_VAL;
        if (vm_try_bin_overload(vm, is_le ? "<=" : "<", v, VM_MAKE_INT((int64_t)imm), &ov)) {
            if (vm->has_pending_exc) return 0;
            return vm_to_bool(ov);
        }
        if (vm->has_pending_exc) return 0;
    }
    if (VM_IS_ANY_INT(v)) {
        int64_t iv = VM_ANY_INT_VAL(v);
        return is_le ? (iv <= (int64_t)imm) : (iv < (int64_t)imm);
    }
    double dv = vm_to_num_vm(vm, v);
    if (vm->has_pending_exc) return 0;
    return is_le ? (dv <= (double)imm) : (dv < (double)imm);
}

static __attribute__((noinline)) int vm_slow_cmp_vals_lt(VM *vm, VMValue a, VMValue b) {
    if (VM_IS_PTR(a) && vm_slow_unwrap_local(vm, &a)) return 0;
    if (VM_IS_PTR(b)) {
        if (vm->stack_top >= vm->stack_cap) vm_stack_grow(vm, vm->stack_top);
        vm->stack[vm->stack_top++] = a;
        int exc = vm_slow_unwrap_local(vm, &b);
        a = vm->stack[--vm->stack_top];
        if (exc) return 0;
    }
    if (__builtin_expect(vm_obj_class_name(a) != NULL || vm_obj_class_name(b) != NULL, 0)) {
        VMValue ov = VM_UNDEF_VAL;
        if (vm_try_bin_overload(vm, "<", a, b, &ov)) {
            if (vm->has_pending_exc) return 0;
            return vm_to_bool(ov);
        }
        if (vm->has_pending_exc) return 0;
    }
    if (VM_IS_ANY_INT(a) && VM_IS_ANY_INT(b)) return VM_ANY_INT_VAL(a) < VM_ANY_INT_VAL(b);
    double da = 0.0, db = 0.0;
    if (vm_pair_to_num(vm, a, b, &da, &db)) return 0;
    return da < db;
}

static __attribute__((noinline)) void vm_slow_add_to_local(VM *vm, uint16_t d, VMValue rhs, uint8_t coerce, int is_sub) {
    if (VM_IS_PTR(rhs) && vm_slow_unwrap_local(vm, &rhs)) return;
    VMValue *cur_locals = vm->stack + vm->frames[vm->frame_count - 1].local_base;
    VMValue *dstp = &cur_locals[d];
    if (VM_IS_PTR(*dstp) && VM_PTR_TYPE(*dstp) == VM_OBJ_CELL) dstp = &VM_AS_CELL(*dstp)->val;
    if (__builtin_expect(VM_IS_PTR(*dstp) && VM_PTR_TYPE(*dstp) == VM_OBJ_TIED_SCALAR, 0)) {
        VMValue ts = *dstp;
        if (vm->stack_top >= vm->stack_cap) vm_stack_grow(vm, vm->stack_top);
        vm->stack[vm->stack_top++] = rhs;
        vm->gc_pause++;
        VMValue lhs = vm_tied_scalar_fetch(vm, ts);
        VMValue cur_rhs = vm->stack[--vm->stack_top];
        VMValue out = VM_UNDEF_VAL;
        if (!vm->has_pending_exc) {
            if (vm_obj_class_name(lhs) || vm_obj_class_name(cur_rhs)) {
                if (!vm_try_bin_overload(vm, is_sub ? "-" : "+", lhs, cur_rhs, &out)) {
                    if (!vm->has_pending_exc) {
                        if (!vm_is_float_operand(lhs) && !vm_is_float_operand(cur_rhs)) {
                            int64_t ia = 0, ib = 0;
                            if (!vm_pair_to_int(vm, lhs, cur_rhs, &ia, &ib)) {
                                uint64_t ua = (uint64_t)ia, ub = (uint64_t)ib;
                                out = VM_MAKE_INT(is_sub ? (int64_t)(ua - ub) : (int64_t)(ua + ub));
                            }
                        } else {
                            double da = 0.0, db = 0.0;
                            if (!vm_pair_to_num(vm, lhs, cur_rhs, &da, &db)) {
                                out = VM_MAKE_FLOAT(is_sub ? (da - db) : (da + db));
                            }
                        }
                    }
                }
            } else if (!vm_is_float_operand(lhs) && !vm_is_float_operand(cur_rhs)) {
                int64_t ia = 0, ib = 0;
                if (!vm_pair_to_int(vm, lhs, cur_rhs, &ia, &ib)) {
                    uint64_t ua = (uint64_t)ia, ub = (uint64_t)ib;
                    out = VM_MAKE_INT(is_sub ? (int64_t)(ua - ub) : (int64_t)(ua + ub));
                }
            } else {
                double da = 0.0, db = 0.0;
                if (!vm_pair_to_num(vm, lhs, cur_rhs, &da, &db)) {
                    out = VM_MAKE_FLOAT(is_sub ? (da - db) : (da + db));
                }
            }
            if (!vm->has_pending_exc) {
                if (coerce == 1 && !VM_IS_INT(out)) {
                    int64_t iv = vm_to_int_vm(vm, out);
                    if (vm->has_pending_exc) { vm->gc_pause--; return; }
                    out = VM_MAKE_INT(iv);
                } else if (coerce == 2 && !VM_IS_FLOAT(out)) {
                    double nv = vm_to_num_vm(vm, out);
                    if (vm->has_pending_exc) { vm->gc_pause--; return; }
                    out = VM_MAKE_FLOAT(nv);
                }
                vm_tied_scalar_store(vm, ts, out);
            }
        }
        vm->gc_pause--;
        return;
    }
    VMValue lhs = *dstp;
    if (!VM_IS_PTR(lhs) && !VM_IS_PTR(rhs)) {
        VMValue res;
        if (VM_IS_FLOAT(lhs) || VM_IS_FLOAT(rhs) || coerce == 2) {
            double da = VM_IS_FLOAT(lhs) ? VM_FLOAT_VAL(lhs) : (VM_IS_INT(lhs) ? (double)VM_INT_VAL(lhs) : 0.0);
            double db = VM_IS_FLOAT(rhs) ? VM_FLOAT_VAL(rhs) : (VM_IS_INT(rhs) ? (double)VM_INT_VAL(rhs) : 0.0);
            double r = is_sub ? (da - db) : (da + db);
            res = (coerce == 1) ? VM_MAKE_INT((int64_t)r) : VM_MAKE_FLOAT(r);
        } else {
            int64_t ia = VM_IS_INT(lhs) ? VM_INT_VAL(lhs) : 0;
            int64_t ib = VM_IS_INT(rhs) ? VM_INT_VAL(rhs) : 0;
            res = VM_MAKE_INT(is_sub ? (ia - ib) : (ia + ib));
        }
        *dstp = res;
        return;
    }
    if (VM_IS_PTR(lhs) && VM_PTR_TYPE(lhs) == VM_OBJ_WEAKREF) {
        if (vm->stack_top >= vm->stack_cap) vm_stack_grow(vm, vm->stack_top);
        vm->stack[vm->stack_top++] = rhs;
        lhs = vm_unwrap_weak_gc(vm, lhs, NULL, 0);
        rhs = vm->stack[--vm->stack_top];
        cur_locals = vm->stack + vm->frames[vm->frame_count - 1].local_base;
        dstp = &cur_locals[d];
        if (VM_IS_PTR(*dstp) && VM_PTR_TYPE(*dstp) == VM_OBJ_CELL) dstp = &VM_AS_CELL(*dstp)->val;
    }
    VMValue out = VM_UNDEF_VAL;
    if (__builtin_expect(vm_obj_class_name(lhs) != NULL || vm_obj_class_name(rhs) != NULL, 0)) {
        if (vm_try_bin_overload(vm, is_sub ? "-" : "+", lhs, rhs, &out)) {
            if (vm->has_pending_exc) return;
            cur_locals = vm->stack + vm->frames[vm->frame_count - 1].local_base;
            dstp = &cur_locals[d];
            if (VM_IS_PTR(*dstp) && VM_PTR_TYPE(*dstp) == VM_OBJ_CELL) dstp = &VM_AS_CELL(*dstp)->val;
            if (coerce == 1 && !VM_IS_INT(out)) {
                int64_t iv = vm_to_int_vm(vm, out);
                if (vm->has_pending_exc) return;
                out = VM_MAKE_INT(iv);
            } else if (coerce == 2 && !VM_IS_FLOAT(out)) {
                double nv = vm_to_num_vm(vm, out);
                if (vm->has_pending_exc) return;
                out = VM_MAKE_FLOAT(nv);
            }
            *dstp = out;
            return;
        }
        if (vm->has_pending_exc) return;
    }
    VMValue ex_out = VM_UNDEF_VAL;
    if (coerce != 1 && vm_exact_int_arith(is_sub ? 1 : 0, lhs, rhs, &ex_out)) {
        /* untyped target: IV -> UV -> NV promotion (an int-declared target
         * keeps int64 wrap-around, as compiled) */
        out = ex_out;
    } else if (!vm_is_float_operand(lhs) && !vm_is_float_operand(rhs)) {
        int64_t ia = 0, ib = 0;
        if (vm_pair_to_int(vm, lhs, rhs, &ia, &ib)) return;
        uint64_t ua = (uint64_t)ia, ub = (uint64_t)ib;
        out = VM_MAKE_INT(is_sub ? (int64_t)(ua - ub) : (int64_t)(ua + ub));
    } else {
        double da = 0.0, db = 0.0;
        if (vm_pair_to_num(vm, lhs, rhs, &da, &db)) return;
        out = VM_MAKE_FLOAT(is_sub ? (da - db) : (da + db));
    }
    if (coerce == 1 && !VM_IS_INT(out)) {
        int64_t iv = vm_to_int_vm(vm, out);
        if (vm->has_pending_exc) return;
        out = VM_MAKE_INT(iv);
    } else if (coerce == 2 && !VM_IS_FLOAT(out)) {
        double nv = vm_to_num_vm(vm, out);
        if (vm->has_pending_exc) return;
        out = VM_MAKE_FLOAT(nv);
    }
    cur_locals = vm->stack + vm->frames[vm->frame_count - 1].local_base;
    dstp = &cur_locals[d];
    if (VM_IS_PTR(*dstp) && VM_PTR_TYPE(*dstp) == VM_OBJ_CELL) dstp = &VM_AS_CELL(*dstp)->val;
    *dstp = out;
}

static __attribute__((noinline)) void vm_call_variadic_setup(VM *vm, VMFrame *nf, VMChunk *callee, int argc, size_t *sp_io) {
    size_t arg_base = *sp_io - (size_t)argc;
    int lcnt = callee->local_count + 1;
    int fixed = callee->fixed_param_count;
    if (fixed < 0) fixed = 0;
    int vc = argc - fixed;
    VMArray *va = vm_array_new(vc > 0 ? vc : 4);
    for (int i = fixed; i < argc; i++) {
        vm_array_push(va, vm->stack[arg_base + (size_t)i]);
    }
    if (__builtin_expect(arg_base + (size_t)lcnt >= vm->stack_cap, 0)) {
        vm_stack_grow(vm, arg_base + (size_t)lcnt);
    }
    int zstart = argc < fixed ? argc : fixed;
    if (zstart < lcnt) {
        memset(vm->stack + arg_base + zstart, 0, (size_t)(lcnt - zstart) * sizeof(VMValue));
    }
    if (fixed < lcnt) {
        vm->stack[arg_base + (size_t)fixed] = VM_MAKE_PTR(va);
    }
    nf->local_base = arg_base;
    *sp_io = arg_base + (size_t)lcnt;
}

static __attribute__((noinline)) void vm_mcall_variadic_setup(VM *vm, VMFrame *nf, VMChunk *callee, int argc, size_t *sp_io) {
    int total_args = argc + 1; /* self + args */
    size_t arg_base = *sp_io - (size_t)total_args;
    int lcnt = callee->local_count + 1;
    int fixed = callee->fixed_param_count;
    if (fixed < 0) fixed = 0;
    int vc = total_args - fixed;
    VMArray *va = vm_array_new(vc > 0 ? vc : 4);
    for (int i = fixed; i < total_args; i++) {
        vm_array_push(va, vm->stack[arg_base + (size_t)i]);
    }
    if (__builtin_expect(arg_base + (size_t)lcnt >= vm->stack_cap, 0)) {
        vm_stack_grow(vm, arg_base + (size_t)lcnt);
    }
    int zstart = total_args < fixed ? total_args : fixed;
    if (zstart < lcnt) {
        memset(vm->stack + arg_base + zstart, 0, (size_t)(lcnt - zstart) * sizeof(VMValue));
    }
    if (fixed < lcnt) {
        vm->stack[arg_base + (size_t)fixed] = VM_MAKE_PTR(va);
    }
    nf->local_base = arg_base;
    *sp_io = arg_base + (size_t)lcnt;
}

static __attribute__((noinline)) void vm_mcall_autoload_setup(VM *vm, VMFrame *nf, VMChunk *callee, int argc, const char *method, size_t *sp_io) {
    size_t arg_base = *sp_io - (size_t)argc - 1;
    int lcnt = callee->local_count + 1;
    int total_al = argc + 2; /* self, method, args... */
    size_t need = arg_base + (size_t)(total_al > lcnt ? total_al : lcnt);
    if (__builtin_expect(need >= vm->stack_cap, 0)) {
        vm_stack_grow(vm, need);
    }
    if (argc > 0) {
        memmove(vm->stack + arg_base + 2, vm->stack + arg_base + 1, (size_t)argc * sizeof(VMValue));
    }
    vm->stack[arg_base + 1] = vm_str(method);
    if (callee->has_variadic) {
        int fixed = callee->fixed_param_count;
        if (fixed < 0) fixed = 0;
        int vc = total_al - fixed;
        VMArray *va = vm_array_new(vc > 0 ? vc : 4);
        for (int i = fixed; i < total_al; i++) {
            vm_array_push(va, vm->stack[arg_base + (size_t)i]);
        }
        int zstart = total_al < fixed ? total_al : fixed;
        if (zstart < lcnt) {
            memset(vm->stack + arg_base + zstart, 0, (size_t)(lcnt - zstart) * sizeof(VMValue));
        }
        if (fixed < lcnt) {
            vm->stack[arg_base + (size_t)fixed] = VM_MAKE_PTR(va);
        }
    } else {
        int pmax = lcnt;
        if (callee->fixed_param_count >= 0 && callee->fixed_param_count < pmax) pmax = callee->fixed_param_count;
        int zf = total_al < pmax ? total_al : pmax;
        if (zf < lcnt) {
            memset(vm->stack + arg_base + zf, 0, (size_t)(lcnt - zf) * sizeof(VMValue));
        }
    }
    nf->local_base = arg_base;
    *sp_io = arg_base + (size_t)lcnt;
}

static __attribute__((noinline)) void vm_closure_variadic_setup(VM *vm, VMFrame *nf, VMChunk *callee, VMClosure *cl, int argc, size_t *sp_io) {
    size_t arg_base = *sp_io - (size_t)argc;
    int lcnt = callee->local_count + 1;
    int eff_argc = cl->has_bound_arg ? (argc + 1) : argc;
    if (cl->has_bound_arg) {
        size_t need = arg_base + (size_t)(eff_argc > lcnt ? eff_argc : lcnt);
        if (__builtin_expect(need >= vm->stack_cap, 0)) {
            vm_stack_grow(vm, need);
        }
        if (argc > 0) {
            memmove(vm->stack + arg_base + 1, vm->stack + arg_base, (size_t)argc * sizeof(VMValue));
        }
        vm->stack[arg_base] = cl->bound_arg;
    } else {
        if (__builtin_expect(arg_base + (size_t)lcnt >= vm->stack_cap, 0)) {
            vm_stack_grow(vm, arg_base + (size_t)lcnt);
        }
    }
    if (callee->has_variadic) {
        int fixed = callee->fixed_param_count;
        if (fixed < 0) fixed = 0;
        int vc = eff_argc - fixed;
        VMArray *va = vm_array_new(vc > 0 ? vc : 4);
        for (int i = fixed; i < eff_argc; i++) {
            vm_array_push(va, vm->stack[arg_base + (size_t)i]);
        }
        int zstart = eff_argc < fixed ? eff_argc : fixed;
        if (zstart < lcnt) {
            memset(vm->stack + arg_base + zstart, 0, (size_t)(lcnt - zstart) * sizeof(VMValue));
        }
        if (fixed < lcnt) {
            vm->stack[arg_base + (size_t)fixed] = VM_MAKE_PTR(va);
        }
    } else {
        int pmax = lcnt;
        if (callee->fixed_param_count >= 0 && callee->fixed_param_count < pmax) pmax = callee->fixed_param_count;
        int zf = eff_argc < pmax ? eff_argc : pmax;
        if (zf < lcnt) {
            memset(vm->stack + arg_base + zf, 0, (size_t)(lcnt - zf) * sizeof(VMValue));
        }
    }
    nf->local_base = arg_base;
    *sp_io = arg_base + (size_t)lcnt;
}

static __attribute__((noinline)) void vm_tail_call_variadic_setup(VM *vm, VMChunk *callee, int argc, size_t base, size_t *sp_io) {
    size_t arg_src = *sp_io - (size_t)argc;
    int lcnt = callee->local_count + 1;
    int fixed = callee->fixed_param_count;
    if (fixed < 0) fixed = 0;
    int vc = argc - fixed;
    VMArray *va = vm_array_new(vc > 0 ? vc : 4);
    for (int i = fixed; i < argc; i++) {
        vm_array_push(va, vm->stack[arg_src + (size_t)i]);
    }
    if (__builtin_expect(base + (size_t)lcnt >= vm->stack_cap, 0)) {
        vm_stack_grow(vm, base + (size_t)lcnt);
    }
    int zf = argc < fixed ? argc : fixed;
    if (zf > 0) {
        memmove(vm->stack + base, vm->stack + arg_src, (size_t)zf * sizeof(VMValue));
    }
    if (zf < lcnt) {
        memset(vm->stack + base + zf, 0, (size_t)(lcnt - zf) * sizeof(VMValue));
    }
    if (fixed < lcnt) {
        vm->stack[base + (size_t)fixed] = VM_MAKE_PTR(va);
    }
    *sp_io = base + (size_t)lcnt;
}

static __attribute__((noinline)) int vm_slow_incr_cmp_jmp(VM *vm, uint16_t slot, int kind, int32_t imm, uint16_t b_slot) {
    VMValue *cur_locals = vm->stack + vm->frames[vm->frame_count - 1].local_base;
    VMValue cur_v = cur_locals[slot];
    VMValue *islot = (VM_IS_PTR(cur_v) && VM_PTR_TYPE(cur_v) == VM_OBJ_CELL)
                     ? &VM_AS_CELL(cur_v)->val : &cur_locals[slot];
    if (__builtin_expect(VM_IS_PTR(*islot) && VM_PTR_TYPE(*islot) == VM_OBJ_TIED_SCALAR, 0)) {
        VMValue ts = *islot;
        vm->gc_pause++;
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
        if (vm->has_pending_exc) return 0;
        cur_locals = vm->stack + vm->frames[vm->frame_count - 1].local_base;
        VMValue fv = vm_tied_scalar_fetch(vm, ts);
        if (vm->has_pending_exc) return 0;
        if (kind == 2) return vm_slow_cmp_vals_lt(vm, fv, cur_locals[b_slot]);
        return vm_slow_cmp_local_i32(vm, fv, imm, kind == 1);
    }
    if (VM_IS_ANY_INT(*islot)) {
        *islot = VM_MAKE_INT((int64_t)((uint64_t)VM_ANY_INT_VAL(*islot) + 1ULL));
    } else {
        double n = vm_to_num(*islot) + 1.0;
        int was_float = VM_IS_FLOAT(*islot);
        *islot = (!was_float && isfinite(n) && n == (double)(int64_t)n && n >= -9.2e18 && n <= 9.2e18)
                 ? VM_MAKE_INT((int64_t)n) : VM_MAKE_FLOAT(n);
    }
    VMValue v = *islot;
    if (kind == 2) {
        VMValue b = cur_locals[b_slot];
        if (VM_BOTH_INT(v, b)) return VM_INT_VAL(v) < VM_INT_VAL(b);
        return vm_slow_cmp_vals_lt(vm, v, b);
    }
    if (VM_IS_INT(v)) return (kind == 1) ? (VM_INT_VAL(v) <= (int64_t)imm) : (VM_INT_VAL(v) < (int64_t)imm);
    return vm_slow_cmp_local_i32(vm, v, imm, kind == 1);
}


/* A key/value list becoming a hash: keys that are objects with a ""
 * overload become their strings (in place; the caller owns the list).
 * Returns 1 when an overload threw (pending exception set). */
static int vm_list_keys_has_ov(VM *vm, VMArray *a) {
    if (!a || !vm->program || vm->program->overload_count == 0) return 0;
    for (int i = 0; i < a->size; i += 2)
        if (vm_obj_class_name(a->items[i]) != NULL && vm_has_stringify(vm->program, a->items[i])) return 1;
    return 0;
}
static int vm_list_keys_stringify(VM *vm, VMArray *a) {
    if (!vm_list_keys_has_ov(vm, a)) return 0;
    vm->gc_pause++;
    for (int i = 0; i < a->size && !vm->has_pending_exc; i += 2) {
        if (vm_obj_class_name(a->items[i]) != NULL && vm_has_stringify(vm->program, a->items[i]))
            a->items[i] = vm_stringify_ov(vm, a->items[i]);
    }
    vm->gc_pause--;
    return vm->has_pending_exc;
}

/* How vm_exec_builtin converts a blessed argument in a program with
 * overloads: 0 = passed as is, 1 = numified (0+), 2 = stringified (""),
 * 3 = abs() (the "abs" / "-" overloads). */
static int vm_builtin_ov_kind(uint16_t bid) {
    if (bid >= VM_GENERIC_BID_BASE) {
        int gidx = bid - VM_GENERIC_BID_BASE;
        if (gidx >= VM_GENERIC_BUILTIN_COUNT) return 0;
        const char *n = vm_generic_builtins[gidx].name;
        if (strcmp(n, "math::abs") == 0 || strcmp(n, "strada_abs") == 0) return 3;
        if (strncmp(n, "math::", 6) == 0) return 1;
        static const char *const str_fns[] = {
            "sys::quotemeta", "strada_quotemeta", "sys::base64_encode", "strada_base64_encode",
            "sys::base64_decode", "strada_base64_decode", "sys::basename", "strada_basename",
            "sys::dirname", "strada_dirname", "sys::file_ext", "strada_file_ext",
            "strada_uc_sv", "strada_lc_sv", "strada_ucfirst_sv", "strada_lcfirst_sv",
            "strada_reverse_sv", "sys::hex", "strada_hex", "sys::oct", "strada_oct", NULL };
        for (int i = 0; str_fns[i]; i++) if (strcmp(n, str_fns[i]) == 0) return 2;
        return 0;
    }
    switch (bid) {
    case BUILTIN_MATH_ABS:
        return 3;
    case BUILTIN_MATH_SQRT: case BUILTIN_MATH_FLOOR: case BUILTIN_MATH_CEIL: case BUILTIN_MATH_POW:
    case BUILTIN_MATH_SIN: case BUILTIN_MATH_COS: case BUILTIN_MATH_TAN:
    case BUILTIN_MATH_ASIN: case BUILTIN_MATH_ACOS: case BUILTIN_MATH_ATAN:
    case BUILTIN_MATH_ATAN2: case BUILTIN_MATH_SINH: case BUILTIN_MATH_COSH:
    case BUILTIN_MATH_TANH: case BUILTIN_MATH_LOG: case BUILTIN_MATH_LOG10:
    case BUILTIN_MATH_EXP: case BUILTIN_MATH_ROUND: case BUILTIN_MATH_FMOD:
    case BUILTIN_MATH_FABS: case BUILTIN_MATH_HYPOT: case BUILTIN_MATH_CBRT:
        return 1;
    case BUILTIN_UCFIRST: case BUILTIN_LCFIRST: case BUILTIN_REPLACE:
    case BUILTIN_REPLACE_ALL: case BUILTIN_MATCH: case BUILTIN_TRIM:
    case BUILTIN_HEX: case BUILTIN_OCT:
    case BUILTIN_STR_RINDEX: case BUILTIN_STR_LTRIM: case BUILTIN_STR_RTRIM:
    case BUILTIN_STR_REVERSE: case BUILTIN_CORE_DIRNAME: case BUILTIN_CORE_BASENAME:
    case BUILTIN_CORE_ORD_BYTE: case BUILTIN_CORE_GET_BYTE: case BUILTIN_CORE_BYTE_LENGTH:
    case BUILTIN_CORE_BASE64_ENCODE: case BUILTIN_CORE_BASE64_DECODE:
    case BUILTIN_CORE_QUOTEMETA: case BUILTIN_CORE_FNMATCH: case BUILTIN_CORE_FILE_EXT:
    case BUILTIN_STR_INDEX_3: case BUILTIN_STR_REPLACE_PLAIN: case BUILTIN_STR_REPLACE_FIRST:
    case BUILTIN_REGEX_REPLACE: case BUILTIN_REGEX_REPLACE_ALL: case BUILTIN_REGEX_SUBST:
        return 2;
    default:
        return 0;
    }
}

static __attribute__((noinline)) int vm_exec_builtin(VM *vm, uint16_t bid, uint8_t argc, VMValue *out_throw) {
    VMValue *stack = vm->stack;
    size_t sp = vm->stack_top;
    VMFrame *frame = &vm->frames[vm->frame_count - 1];
    VMValue throw_exc = VM_UNDEF_VAL;
    #define SP_PUSH(v) do { \
        if (__builtin_expect(sp >= vm->stack_cap, 0)) { vm_stack_grow(vm, sp); stack = vm->stack; } \
        stack[sp++] = (v); \
    } while (0)
    #define SP_POP()   (stack[--sp])
    #define RELOAD() do { \
        frame = &vm->frames[vm->frame_count - 1]; \
        sp = vm->stack_top; \
    } while(0)
        VMValue args_stk[16];
        VMValue *args = (argc > 16) ? malloc((size_t)argc * sizeof(VMValue)) : args_stk;
        for (int i = argc - 1; i >= 0; i--) {
            args[i] = SP_POP();
        }

        /* Overloaded arguments (Perl / compiled: the runtime's strada_to_str
         * and strada_to_num honor "" and 0+): string builtins see the
         * object's string, numeric ones its number, abs() its abs. */
        if (__builtin_expect(vm->program && vm->program->overload_count > 0, 0)) {
            int ovk = vm_builtin_ov_kind(bid);
            if (ovk) {
                for (int i = 0; i < argc; i++) {
                    if (vm_obj_class_name(args[i]) == NULL) continue;
                    vm->stack_top = sp; vm->gc_pause++;
                    if (ovk == 3 && argc == 1) {
                        VMValue ar = VM_UNDEF_VAL;
                        int ath = vm_eval_abs_slow(vm, args[0], &ar);
                        vm->gc_pause--;
                        stack = vm->stack; RELOAD();
                        if (ath || vm->has_pending_exc) {
                            throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                            goto L_builtin_throw_exc;
                        }
                        if (args != args_stk) free(args);
                        SP_PUSH(ar);
                        vm->stack_top = sp; return 0;
                    }
                    if (ovk == 1) {
                        double d = vm_to_num_ov(vm, args[i], 0);
                        if (!vm->has_pending_exc) args[i] = VM_MAKE_FLOAT(d);
                    } else if (vm_has_stringify(vm->program, args[i])) {
                        args[i] = vm_stringify_ov(vm, args[i]);
                    }
                    vm->gc_pause--;
                    stack = vm->stack; RELOAD();
                    if (vm->has_pending_exc) {
                        throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                        goto L_builtin_throw_exc;
                    }
                }
            }
        }

        /* Generic runtime-bridged builtins (vm_generic_builtins.inc): call
         * the Strada runtime's uniform StradaValue* functions through the
         * vm_to_sv/sv_to_vm bridge. The compiler only emits these ids with
         * argc matching the table entry (vm_generic_builtin_find). */
        if (bid >= VM_GENERIC_BID_BASE) {
            int gidx = bid - VM_GENERIC_BID_BASE;
            if (gidx < VM_GENERIC_BUILTIN_COUNT &&
                argc <= vm_generic_builtins[gidx].argc) {
                const VMGenericBuiltin *gb = &vm_generic_builtins[gidx];
StradaValue *sa[4] = { NULL, NULL, NULL, NULL };
                /* trailing parameters the call omits are NULL, as the
                 * compiled backend passes them (e.g. localtime() = now) */
                for (int i = 0; i < argc; i++) sa[i] = vm_to_sv(args[i]);
                int call_argc = gb->argc;
                StradaValue *r = NULL;
                vm->stack_top = sp;
                int threw = vm_c_call_generic(vm, gb, sa, call_argc, &r, &throw_exc);
                stack = vm->stack;
                RELOAD();
                for (int i = 0; i < argc; i++) vm_fh_sync_from_sv(args[i]);
                for (int i = 0; i < argc; i++) strada_decref(sa[i]);
                /* a write through an in-memory handle (open(\$buf, ...))
                 * must reach the backing scalar */
                for (int i = 0; i < argc && !threw; i++) {
                    if (VM_IS_PTR(args[i]) && VM_PTR_TYPE(args[i]) == VM_OBJ_FILEHANDLE) {
                        VMFileHandle *gfh = (VMFileHandle*)VM_TO_PTR(args[i]);
                        if (gfh->is_mem == 2 && VM_IS_PTR(gfh->mem_sref)) {
                            if (gfh->fp) fflush((FILE*)gfh->fp);
                            vm->stack_top = sp;
                            vm_fh_sync_mem_sref(vm, gfh);
                            stack = vm->stack; RELOAD();
                        }
                    }
                }
                if (threw) {
                    for (int i = 0; i < argc; i++) {
                        if (VM_IS_PTR(args[i]) && (VM_PTR_TYPE(args[i]) == VM_OBJ_STR || VM_PTR_TYPE(args[i]) == VM_OBJ_STRBUF))
                            vm_val_free(&args[i]);
                    }
                    if (args != args_stk) free(args);
                    { *out_throw = throw_exc; vm->stack_top = sp; return 1; }
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
            vm->stack_top = sp; return 0;
        }

#define RT_CALL_0(fn) { VMValue __vr = VM_UNDEF_VAL; vm->stack_top = sp; int __th = vm_c_call_0(vm, (fn), &__vr, &throw_exc); stack = vm->stack; RELOAD(); if (__th) goto L_builtin_throw_exc; SP_PUSH(__vr); }
#define RT_CALL_1(fn) { VMValue __vr = VM_UNDEF_VAL; vm->stack_top = sp; int __th = vm_c_call_1(vm, (fn), args[0], &__vr, &throw_exc); stack = vm->stack; RELOAD(); if (__th) goto L_builtin_throw_exc; SP_PUSH(__vr); }
#define RT_CALL_2(fn) { VMValue __vr = VM_UNDEF_VAL; vm->stack_top = sp; int __th = vm_c_call_2(vm, (fn), args[0], args[1], &__vr, &throw_exc); stack = vm->stack; RELOAD(); if (__th) goto L_builtin_throw_exc; SP_PUSH(__vr); }
#define RT_CALL_3(fn) { VMValue __vr = VM_UNDEF_VAL; vm->stack_top = sp; int __th = vm_c_call_3(vm, (fn), args[0], args[1], args[2], &__vr, &throw_exc); stack = vm->stack; RELOAD(); if (__th) goto L_builtin_throw_exc; SP_PUSH(__vr); }
        switch (bid) {
        case BUILTIN_CORE_OPEN: {
            if (argc >= 2) {
                VMValue target = args[0];
                char fbuf[256], mbuf[32];
                const char *mode = vm_to_cstr(args[1], mbuf, 32);
                if (VM_IS_PTR(target) && VM_PTR_TYPE(target) == VM_OBJ_WEAKREF) {
                    vm->stack_top = sp;
                    target = vm_unwrap_weak_gc(vm, target, NULL, 0);
                    stack = vm->stack; RELOAD();
                }
                if (VM_IS_PTR(target) && VM_PTR_TYPE(target) == VM_OBJ_SREF) {
                    if (strcmp(mode, "<") == 0 || strcmp(mode, "r") == 0) {
                        vm->stack_top = sp;
                        VMValue cur = vm_sref_fetch_val(vm, target);
                        stack = vm->stack; RELOAD();
                        if (__builtin_expect(vm->has_pending_exc, 0)) {
                            throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                            goto L_builtin_throw_exc;
                        }
                        char dbuf[256];
                        size_t dlen = 0;
                        const char *data = vm_bytes_view(cur, dbuf, sizeof(dbuf), &dlen);
                        char *copy = (dlen > 0) ? malloc(dlen) : NULL;
                        if (copy && dlen > 0) memcpy(copy, data, dlen);
                        FILE *fp = (dlen > 0 && copy) ? fmemopen(copy, dlen, "r") : fopen("/dev/null", "r");
                        if (fp) {
                            VMFileHandle *fh = calloc(1, sizeof(VMFileHandle));
                            vm_gc_init_hdr(&fh->hdr, VM_OBJ_FILEHANDLE);
                            fh->fp = fp;
                            fh->mem_buf = copy;
                            fh->mem_len = dlen;
                            fh->is_mem = 1;
                            SP_PUSH(VM_MAKE_PTR(fh));
                        } else {
                            free(copy);
                            SP_PUSH(VM_UNDEF_VAL);
                        }
                    } else if (strcmp(mode, ">") == 0 || strcmp(mode, "w") == 0 ||
                               strcmp(mode, ">>") == 0 || strcmp(mode, "a") == 0) {
                        int is_append = (mode[0] == '>' && mode[1] == '>') || mode[0] == 'a';
                        VMValue cur = VM_UNDEF_VAL;
                        if (is_append) {
                            vm->stack_top = sp;
                            vm->gc_pause++;
                            cur = vm_sref_fetch_val(vm, target);
                            vm->gc_pause--;
                            stack = vm->stack; RELOAD();
                            if (__builtin_expect(vm->has_pending_exc, 0)) {
                                throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                                goto L_builtin_throw_exc;
                            }
                        }
                        VMFileHandle *fh = calloc(1, sizeof(VMFileHandle));
                        vm_gc_init_hdr(&fh->hdr, VM_OBJ_FILEHANDLE);
                        FILE *fp = open_memstream(&fh->mem_buf, &fh->mem_len);
                        if (fp) {
                            fh->fp = fp;
                            fh->is_mem = 2;
                            fh->mem_sref = target;
                            if (is_append && !VM_IS_UNDEF(cur)) {
                                char dbuf[256];
                                size_t dlen = 0;
                                const char *data = vm_bytes_view(cur, dbuf, sizeof(dbuf), &dlen);
                                if (dlen > 0) fwrite(data, 1, dlen, fp);
                                fflush(fp);
                            }
                            SP_PUSH(VM_MAKE_PTR(fh));
                        } else {
                            free(fh->mem_buf);
                            fh->mem_buf = NULL;
                            fh->fp = NULL;
                            fh->mem_sref = VM_UNDEF_VAL;
                            SP_PUSH(VM_UNDEF_VAL);
                        }
                    } else {
                        SP_PUSH(VM_UNDEF_VAL);
                    }
                    break;
                }
                const char *path = vm_to_cstr(target, fbuf, 256);
                /* Convert Perl-style modes (as strada_normalize_open_mode) */
                const char *cmode = mode;
                if (strcmp(mode, "<") == 0) cmode = "r";
                else if (strcmp(mode, ">") == 0) cmode = "w";
                else if (strcmp(mode, ">>") == 0) cmode = "a";
                else if (strcmp(mode, "+<") == 0) cmode = "r+";
                else if (strcmp(mode, "+>>") == 0) cmode = "a+";
                else if (strcmp(mode, "+>") == 0) cmode = "w+";
                FILE *fp = vm_path_has_nul(target) ? NULL : fopen(path, cmode);
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
                    if (fh->is_mem == 2 && VM_IS_PTR(fh->mem_sref)) {
                        VMValue sref = fh->mem_sref;
                        fh->mem_sref = VM_UNDEF_VAL;
                        fclose((FILE*)fh->fp);
                        fh->fp = NULL;
                        char *mbuf = fh->mem_buf;
                        size_t mlen = fh->mem_len;
                        fh->mem_buf = NULL;
                        fh->mem_len = 0;
                        vm->stack_top = sp;
                        vm->gc_pause++;
                        VMValue sv = VM_MAKE_STR_N(mbuf ? mbuf : "", mlen);
                        free(mbuf);
                        vm_sref_store_val(vm, sref, sv);
                        vm->gc_pause--;
                        stack = vm->stack; RELOAD();
                        if (__builtin_expect(vm->has_pending_exc, 0)) {
                            throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                            goto L_builtin_throw_exc;
                        }
                    } else {
                        fclose((FILE*)fh->fp);
                        fh->fp = NULL;
                        if (fh->is_mem == 1) {
                            free(fh->mem_buf);
                            fh->mem_buf = NULL;
                        }
                    }
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
            if (!fp && argc >= 1 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_NATIVE_SV) {
                /* runtime socket / handle: the runtime's buffered line reader */
                StradaValue *rsv = vm_to_sv(args[0]);
                vm->stack_top = sp;
                int _rl_safe = vm_worker_enter_safe(vm);
                StradaValue *line;
                if (max_bytes > 0) {
                    StradaValue *mx = strada_new_int(max_bytes);
                    line = strada_read_line_max(rsv, mx);
                    strada_decref(mx);
                } else {
                    line = strada_read_line(rsv);
                }
                vm_worker_leave_safe(vm, _rl_safe);
                strada_decref(rsv);
                stack = vm->stack; RELOAD();
                VMValue lv = line ? sv_to_vm(line) : VM_UNDEF_VAL;
                if (line) strada_decref(line);
                SP_PUSH(lv);
                break;
            }
            if (fp) {
                size_t cap = 256, len = 0;
                char *buf = malloc(cap);
                int c = EOF;
                int _prev_safe = vm_worker_enter_safe(vm);
                while ((max_bytes <= 0 || (int64_t)len < max_bytes) && (c = fgetc(fp)) != EOF) {
                    if (len + 1 >= cap) {
                        cap *= 2;
                        buf = realloc(buf, cap);
                    }
                    buf[len++] = (char)c;
                    if (c == '\n') break;
                }
                vm_worker_leave_safe(vm, _prev_safe);
                if (len == 0 && c == EOF) {
                    free(buf);
                    SP_PUSH(VM_UNDEF_VAL);
                } else {
                    /* the newline goes, a CR before it stays (compiled readline) */
                    if (len > 0 && buf[len - 1] == '\n') len--;
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
                SP_PUSH(VM_MAKE_INT(fh->fp ? ftell((FILE*)fh->fp) : -1));   /* closed: -1 */
            } else SP_PUSH(VM_MAKE_INT(-1));
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
                if (fh->is_mem == 2 && VM_IS_PTR(fh->mem_sref)) {
                    vm->stack_top = sp;
                    vm_fh_sync_mem_sref(vm, fh);
                    stack = vm->stack; RELOAD();
                    if (__builtin_expect(vm->has_pending_exc, 0)) {
                        throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                        goto L_builtin_throw_exc;
                    }
                }
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
            /* 1 = written fully, 0 = failed (a path with a NUL byte fails) */
            int spew_ok = 0;
            if (argc >= 2 && !vm_path_has_nul(args[0])) {
                char fbuf[256], dbuf[64];
                size_t dlen = 0;
                const char *path = vm_to_cstr(args[0], fbuf, sizeof(fbuf));
                const char *data = vm_bytes_view(args[1], dbuf, sizeof(dbuf), &dlen);
                FILE *fp = fopen(path, "wb");
                if (fp) {
                    spew_ok = (fwrite(data, 1, dlen, fp) == dlen);
                    if (fclose(fp) != 0) spew_ok = 0;
                }
            }
            SP_PUSH(VM_MAKE_INT(spew_ok));
            break;
        }
        case BUILTIN_CORE_QX: {
            if (argc >= 1) {
                char cbuf[1024];
                const char *cmd = vm_to_cstr(args[0], cbuf, 1024);
                int _prev_safe = vm_worker_enter_safe(vm);
                FILE *fp = popen(cmd, "r");
                char *out = NULL;
                size_t total = 0;
                if (fp) {
                    size_t cap = 4096;
                    out = malloc(cap);
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
                }
                vm_worker_leave_safe(vm, _prev_safe);
                if (out) {
                    SP_PUSH(VM_MAKE_STR_N(out, total));
                    free(out);
                } else {
                    SP_PUSH(vm_str(""));
                }
            } else SP_PUSH(vm_str(""));
            break;
        }
        case BUILTIN_CORE_SYSTEM: {
            if (argc >= 1) {
                char cbuf[1024];
                const char *cmd = vm_to_cstr(args[0], cbuf, 1024);
                int _prev_safe = vm_worker_enter_safe(vm);
                int rc = system(cmd);
                vm_worker_leave_safe(vm, _prev_safe);
                SP_PUSH(VM_MAKE_INT(WEXITSTATUS(rc)));
            } else SP_PUSH(VM_MAKE_INT(-1));
            break;
        }
        case BUILTIN_CORE_GETENV: {
            if (argc >= 1) {
                char nbuf[256];
                const char *name = vm_to_cstr(args[0], nbuf, 256);
                char *val = getenv(name);
                SP_PUSH(val ? vm_str(val) : VM_UNDEF_VAL);   /* unset: undef */
            } else SP_PUSH(VM_UNDEF_VAL);
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
                const char *key = vm_global_key_main(vm, vm_to_cstr(args[0], kbuf, 256));
                vm_globals_set(vm, key, vm_val_copy(args[1]));
            }
            SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_CORE_GLOBAL_GET: {
            if (argc >= 1) {
                char kbuf[256];
                const char *key = vm_global_key_main(vm, vm_to_cstr(args[0], kbuf, 256));
                VMValue v = vm_globals_get_raw(vm, key);
                if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_WEAKREF) {
                    vm->stack_top = sp;
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
                const char *key = vm_global_key_main(vm, vm_to_cstr(args[0], kbuf, 256));
                SP_PUSH(VM_MAKE_INT(vm_globals_exists(vm, key)));
            } else SP_PUSH(VM_MAKE_INT(0));
            break;
        }
        case BUILTIN_CORE_GLOBAL_DELETE: {
            if (argc >= 1) {
                char kbuf[256];
                const char *key = vm_global_key_main(vm, vm_to_cstr(args[0], kbuf, 256));
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
            for (int i = 0; i < vm->regex_hw && i < VM_RX_MAX; i++) if (vm->regex_captures[i]) last = i;
            if (vm->regex_ngroups - 1 > last) last = vm->regex_ngroups - 1;
            for (int i = 0; i <= last; i++)
                vm_array_push(ca, vm->regex_captures[i]
                    ? vm_make_str_flag(vm->regex_captures[i], vm->regex_capture_len[i], vm->regex_capture_utf8[i])
                    : VM_UNDEF_VAL);
            SP_PUSH(VM_MAKE_PTR(ca));
            break;
        }
        case BUILTIN_NAMED_CAPTURES: {
            VMHash *nh = vm_hash_new(8);
            for (int i = 0; i < VM_RX_MAX; i++) {
                if (vm->regex_capture_names[i] && vm->regex_captures[i])
                    vm_hash_set(nh, vm->regex_capture_names[i],
                                vm_make_str_flag(vm->regex_captures[i], vm->regex_capture_len[i], vm->regex_capture_utf8[i]));
            }
            SP_PUSH(VM_MAKE_PTR(nh));
            break;
        }
        case BUILTIN_SPRINTF_LIST:
        case BUILTIN_PACK_LIST: {
            VMArray *la = (argc >= 1 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_ARRAY) ? VM_AS_ARRAY(args[0]) : NULL;
            if (!la || la->size == 0) { SP_PUSH(vm_str("")); break; }
            if (bid == BUILTIN_SPRINTF_LIST) {
                vm->stack_top = sp;
                vm->gc_pause++;
                VMValue sres = vm_sprintf(vm, la->size, la->items);
                vm->gc_pause--;
                stack = vm->stack; RELOAD();
                if (__builtin_expect(vm->has_pending_exc, 0)) {
                    vm_val_free(&sres);
                    throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                    goto L_builtin_throw_exc;
                }
                SP_PUSH(sres);
            } else {
                char fbuf[256];
                const char *fmt = vm_to_cstr(la->items[0], fbuf, sizeof(fbuf));
                StradaValue *pk_arr = strada_new_array();
                for (int i = 1; i < la->size; i++) strada_array_push_take(pk_arr->value.av, vm_to_sv(la->items[i]));
                StradaValue *packed = strada_pack(fmt, pk_arr);
                VMValue vm_res = sv_to_vm(packed);
                strada_decref(pk_arr);
                strada_decref(packed);
                SP_PUSH(vm_res);
            }
            break;
        }
        case BUILTIN_GC_OP: {
            int64_t op = argc >= 1 ? vm_to_int(args[0]) : 0;
            VMValue gres = VM_UNDEF_VAL;
            if (op == 0) {
                vm->stack_top = sp;
                vm_gc_collect(vm);
                strada_gc_collect();
                stack = vm->stack; RELOAD();
            } else if (op == 1) {
                vm->gc_user_disabled = 0;
                vm->gc_threshold = (vm->gc_count < 4096) ? 8192 : (vm->gc_count * 2);
                strada_gc_set_enabled(1);
            } else if (op == 2) {
                vm->gc_user_disabled = 1;
                vm->gc_threshold = SIZE_MAX / 2;
                strada_gc_set_enabled(0);
            } else if (op == 3) {
                int64_t n = argc >= 2 ? vm_to_int(args[1]) : 0;
                if (n > 0) {
                    strada_gc_set_threshold((long)n);
                    if (!vm->gc_user_disabled) vm->gc_threshold = (size_t)(n < 1024 ? 1024 : n);
                }
            } else if (op == 4) {
                gres = VM_MAKE_INT((int64_t)(vm->gc_runs + strada_gc_collections()));
            } else if (op == 5) {
                gres = VM_MAKE_INT((int64_t)(vm->gc_freed_total + strada_gc_objects_freed()));
            }
            SP_PUSH(gres);
            break;
        }
        case BUILTIN_SPEW_FH: {
            if (argc >= 2) {
                StradaValue *fsv = vm_to_sv(args[0]);
                StradaValue *dsv = vm_to_sv(args[1]);
                vm->stack_top = sp;
                if (argc >= 3 && vm_to_int(args[2]) == 1) strada_spew_fd(fsv, dsv);
                else strada_spew_fh(fsv, dsv);
                if (VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_FILEHANDLE) vm_fh_sync_from_sv(args[0]);
                strada_decref(fsv);
                strada_decref(dsv);
            }
            SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_DEFAULT_CAP: {
            int64_t which = argc >= 1 ? vm_to_int(args[0]) : 0;
            if (argc >= 2) {
                int64_t n = vm_to_int(args[1]);
                if (which == 0) strada_set_hash_default_capacity(n);
                else strada_set_array_default_capacity(n);
            }
            SP_PUSH(VM_MAKE_INT(which == 0 ? strada_get_hash_default_capacity() : strada_get_array_default_capacity()));
            break;
        }
        case BUILTIN_REGEX_LIST: {
            if (argc < 4) { SP_PUSH(VM_MAKE_PTR(vm_array_new(0))); break; }
            char sbuf[512], pbuf[256], fbuf[32];
            const char *subj = vm_to_cstr(args[0], sbuf, sizeof(sbuf));
            const char *rpat = vm_to_cstr(args[1], pbuf, sizeof(pbuf));
            const char *rfl = vm_to_cstr(args[2], fbuf, sizeof(fbuf));
            int rglob = (int)vm_to_int(args[3]);
            StradaValue *subj_sv = vm_to_sv(args[0]);
            strada_regex_set_subject_for_sv(subj_sv);
            StradaValue *rl = strada_regex_list(subj ? subj : "", rpat ? rpat : "", (rfl && rfl[0]) ? rfl : NULL, rglob);
            strada_regex_clear_subject_len();
            strada_decref(subj_sv);
            VMValue rv = rl ? sv_to_vm(rl) : VM_MAKE_PTR(vm_array_new(0));
            if (rl) strada_decref(rl);
            SP_PUSH(rv);
            break;
        }
        case BUILTIN_AUTOVIV_ARRAY: {
            VMValue av = argc >= 1 ? args[0] : VM_UNDEF_VAL;
            if (VM_IS_UNDEF(av)) {
                SP_PUSH(VM_MAKE_PTR(vm_array_new(4)));
            } else if (VM_IS_PTR(av) && VM_PTR_TYPE(av) == VM_OBJ_ARRAY) {
                SP_PUSH(av);
            } else if (VM_IS_PTR(av) && VM_PTR_TYPE(av) == VM_OBJ_SREF) {
                SP_PUSH(av);   /* leave scalar refs to the existing error path */
            } else {
                throw_exc = vm_str("Not an ARRAY reference");
                goto L_builtin_throw_exc;
            }
            break;
        }
        case BUILTIN_HASH_DELETE_SLICE: {
            /* delete @h{LIST}: remove each key, return the removed values */
            VMArray *out = vm_array_new(4);
            if (argc >= 2 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_HASH &&
                VM_IS_PTR(args[1]) && VM_PTR_TYPE(args[1]) == VM_OBJ_ARRAY) {
                VMHash *dh = VM_AS_HASH(args[0]);
                VMArray *ka = VM_AS_ARRAY(args[1]);
                for (int i = 0; i < ka->size; i++) {
                    char kb[256];
                    size_t kl = 0;
                    const char *kp = vm_bytes_view(ka->items[i], kb, sizeof(kb), &kl);
                    VMValue old = vm_hash_delete_ret_n(dh, kp ? kp : "", kp ? kl : 0);
                    vm_array_push(out, old);
                }
            }
            SP_PUSH(VM_MAKE_PTR(out));
            break;
        }
        case BUILTIN_CHOMP_ARRAY: {
            /* chomp(@a): chomp every element in place; the count removed */
            int64_t removed = 0;
            if (argc >= 1 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_ARRAY) {
                VMArray *ca = VM_AS_ARRAY(args[0]);
                for (int i = 0; i < ca->size; i++) {
                    VMValue ev = ca->items[i];
                    if (!VM_IS_PTR(ev) || (VM_PTR_TYPE(ev) != VM_OBJ_STR && VM_PTR_TYPE(ev) != VM_OBJ_STRBUF)) continue;
                    char eb[256];
                    size_t el = 0;
                    const char *ep = vm_bytes_view(ev, eb, sizeof(eb), &el);
                    size_t nl = el;
                    if (nl >= 2 && ep[nl - 2] == '\r' && ep[nl - 1] == '\n') nl -= 2;
                    else if (nl >= 1 && ep[nl - 1] == '\n') nl -= 1;
                    if (nl != el) {
                        removed += (int64_t)(el - nl);
                        VMValue nv = vm_make_str_flag(ep, nl, vm_is_utf8(ev));
                        vm_val_free(&ca->items[i]);
                        ca->items[i] = nv;
                    }
                }
            }
            SP_PUSH(VM_MAKE_INT(removed));
            break;
        }
        case BUILTIN_LIST_REPEAT: {
            /* (LIST) x N */
            VMArray *out = vm_array_new(4);
            int64_t n = argc >= 2 ? vm_to_int(args[1]) : 0;
            if (argc >= 1 && n > 0 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_ARRAY) {
                VMArray *la = VM_AS_ARRAY(args[0]);
                for (int64_t r = 0; r < n; r++)
                    for (int i = 0; i < la->size; i++) vm_array_push(out, vm_val_copy(la->items[i]));
            }
            SP_PUSH(VM_MAKE_PTR(out));
            break;
        }
        case BUILTIN_POLL: {
            /* core::poll([{fd, events}, ...], $ms): VM-native so "revents"
             * lands in the caller's hashes (the bridge would copy them) */
            VMArray *pa = (argc >= 1 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_ARRAY) ? VM_AS_ARRAY(args[0]) : NULL;
            if (!pa) { SP_PUSH(VM_MAKE_INT(-1)); break; }
            int pn = pa->size;
            struct pollfd *pfds = calloc(pn > 0 ? (size_t)pn : 1, sizeof(struct pollfd));
            if (!pfds) { SP_PUSH(VM_MAKE_INT(-1)); break; }
            for (int i = 0; i < pn; i++) {
                VMValue ev = pa->items[i];
                pfds[i].fd = -1;
                if (!VM_IS_PTR(ev) || VM_PTR_TYPE(ev) != VM_OBJ_HASH) continue;
                VMValue fv = vm_hash_get(VM_AS_HASH(ev), "fd");
                VMValue evv = vm_hash_get(VM_AS_HASH(ev), "events");
                if (VM_IS_ANY_INT(fv)) {
                    pfds[i].fd = (int)VM_ANY_INT_VAL(fv);
                } else if (!VM_IS_UNDEF(fv)) {
                    StradaValue *fsv = vm_to_sv(fv);
                    int pfd = -1;
                    if (fsv && !STRADA_IS_TAGGED_INT(fsv) && fsv->type == STRADA_SOCKET && fsv->value.sock) pfd = fsv->value.sock->fd;
                    else if (fsv && !STRADA_IS_TAGGED_INT(fsv) && fsv->type == STRADA_FILEHANDLE && fsv->value.fh) pfd = fileno(fsv->value.fh);
                    else pfd = (int)strada_to_int(fsv);
                    strada_decref(fsv);
                    pfds[i].fd = pfd;
                }
                pfds[i].events = VM_IS_UNDEF(evv) ? POLLIN : (short)vm_to_int(evv);
            }
            int tmo = argc >= 2 ? (int)vm_to_int(args[1]) : -1;
            vm->stack_top = sp;
            int prc;
            do { prc = poll(pfds, (nfds_t)pn, tmo); } while (prc < 0 && errno == EINTR);
            for (int i = 0; i < pn; i++) {
                VMValue ev = pa->items[i];
                if (VM_IS_PTR(ev) && VM_PTR_TYPE(ev) == VM_OBJ_HASH)
                    vm_hash_set(VM_AS_HASH(ev), "revents", VM_MAKE_INT(pfds[i].revents));
            }
            free(pfds);
            SP_PUSH(VM_MAKE_INT(prc));
            break;
        }
        case BUILTIN_STRUCT_FIELD_STORE: {
            int64_t sk = argc >= 2 ? vm_to_int(args[1]) : 0;
            VMValue sv0 = argc >= 1 ? args[0] : VM_UNDEF_VAL;
            if (sk == 1) {
                SP_PUSH(VM_MAKE_INT((int64_t)(int32_t)(uint32_t)(uint64_t)vm_to_int(sv0)));
            } else if (sk == 2) {
                SP_PUSH(VM_MAKE_FLOAT(vm_to_num(sv0)));
            } else if (sk == 3) {
                char sb[256];
                size_t sl = 0;
                const char *sp0 = vm_bytes_view(sv0, sb, sizeof(sb), &sl);
                size_t keep = sl > 63 ? 63 : sl;
                const char *nul = sp0 ? memchr(sp0, 0, keep) : NULL;
                if (nul) keep = (size_t)(nul - sp0);
                SP_PUSH(vm_make_str_flag(sp0 ? sp0 : "", keep, vm_is_utf8(sv0)));
            } else {
                SP_PUSH(vm_val_copy(sv0));
            }
            break;
        }
        case BUILTIN_FE_WRITEBACK: {
            if (argc >= 3 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_ARRAY) {
                VMArray *wa = VM_AS_ARRAY(args[0]);
                int64_t wi = vm_to_int(args[1]);
                if (VM_IS_UNDEF(wa->tied_obj) && wi >= 0 && wi < wa->size) {
                    VMValue nv = vm_val_copy(args[2]);
                    vm_val_free(&wa->items[wi]);
                    wa->items[wi] = nv;
                    if (wa->holes) wa->holes[wi] = 0;
                }
            }
            SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_INCR_VALUE: {
            VMValue iv0 = argc >= 1 ? args[0] : VM_UNDEF_VAL;
            int64_t idelta = argc >= 2 ? vm_to_int(args[1]) : 1;
            if (VM_IS_ANY_INT(iv0)) {
                SP_PUSH(vm_from_int128((__int128)VM_ANY_INT_VAL(iv0) + idelta));
            } else if (VM_IS_FLOAT(iv0)) {
                SP_PUSH(VM_MAKE_FLOAT(VM_FLOAT_VAL(iv0) + (double)idelta));
            } else {
                /* strings (magic ++), undef, big ints: the runtime's rules */
                StradaValue *isv = vm_to_sv(iv0);
                StradaValue *inr = strada_inc_value(isv, idelta);
                strada_decref(isv);
                SP_PUSH(inr ? sv_to_vm(inr) : VM_UNDEF_VAL);
                if (inr) strada_decref(inr);
            }
            break;
        }
        case BUILTIN_NUMIFY_EXACT: {
            /* a numeric literal too large for int64: uint64 or double */
            __int128 nx = 0;
            if (argc >= 1 && vm_exact_int_operand(args[0], &nx) > 0) { SP_PUSH(vm_from_int128(nx)); }
            else { SP_PUSH(VM_MAKE_FLOAT(argc >= 1 ? vm_to_num(args[0]) : 0.0)); }
            break;
        }
        case BUILTIN_ASYNC_OP: {
            int64_t aop = argc >= 1 ? vm_to_int(args[0]) : -1;
            VMValue *aa = args + 1;
            int an = argc - 1;
            VMValue a0 = an >= 1 ? aa[0] : VM_UNDEF_VAL;
            VMValue a1 = an >= 2 ? aa[1] : VM_UNDEF_VAL;
            VMValue a2 = an >= 3 ? aa[2] : VM_UNDEF_VAL;
            int a0_future = 0;
            if (VM_IS_PTR(a0) && VM_PTR_TYPE(a0) == VM_OBJ_NATIVE_SV) {
                StradaValue *fsv = ((VMNativeSV *)VM_TO_PTR(a0))->sv;
                a0_future = fsv && !STRADA_IS_TAGGED_INT(fsv) && fsv->type == STRADA_FUTURE;
            }
            VMValue avr = VM_UNDEF_VAL;
            int ath = 0;
            vm->stack_top = sp;
            int _aps = vm_worker_enter_safe(vm);
            switch (aop) {
            case 8: {
                /* (closure, args array) -> future on the runtime pool */
                VMAsyncCallCtx *c = calloc(1, sizeof(VMAsyncCallCtx));
                c->fn = vm_to_sv(a0);
                c->args = (an >= 2) ? vm_to_sv(a1) : strada_new_array();
                StradaValue *cl = strada_custom_closure_new(vm_async_call_cb, vm_async_call_free, c);
                StradaValue *fut = strada_future_new(cl);   /* takes ownership of cl */
                avr = fut ? sv_to_vm(fut) : VM_UNDEF_VAL;
                if (fut) strada_decref(fut);
                break;
            }
            case 0: /* timeout(f, ms) */
                if (a0_future) ath = vm_c_call_2(vm, strada_async_timeout, a0, a1, &avr, &throw_exc);
                else avr = vm_val_copy(a0);
                break;
            case 1: ath = vm_c_call_1(vm, strada_async_sleep, a0, &avr, &throw_exc); break;
            case 2: ath = vm_c_call_0(vm, strada_async_cancelled, &avr, &throw_exc); break;
            case 3: ath = vm_c_call_3(vm, strada_async_map, a0, a1, a2, &avr, &throw_exc); break;
            case 4:
                if (a0_future) strada_future_cancel(((VMNativeSV *)VM_TO_PTR(a0))->sv);
                break;
            case 5: avr = VM_MAKE_INT(a0_future ? strada_future_is_done(((VMNativeSV *)VM_TO_PTR(a0))->sv) : 1); break;
            case 6: avr = VM_MAKE_INT(a0_future ? strada_future_is_cancelled(((VMNativeSV *)VM_TO_PTR(a0))->sv) : 0); break;
            case 7: /* await: a plain (non-future) value awaits to itself */
                if (a0_future) ath = vm_c_call_1(vm, strada_future_await, a0, &avr, &throw_exc);
                else avr = vm_val_copy(a0);
                break;
            case 9: ath = vm_c_call_3(vm, strada_io_wait_async, a0, a1, a2, &avr, &throw_exc); break;
            case 10: ath = vm_c_call_1(vm, strada_resolve_async, a0, &avr, &throw_exc); break;
            case 11: strada_pool_init(an >= 1 ? (int)vm_to_int(a0) : 4); break;
            case 12: strada_pool_shutdown(); break;
            default: break;
            }
            vm_worker_leave_safe(vm, _aps);
            stack = vm->stack; RELOAD();
            if (ath) goto L_builtin_throw_exc;
            SP_PUSH(avr);
            break;
        }
        case BUILTIN_CORO_OP: {
            /* core::coro_*: the runtime's coroutines (green tasks) */
            int64_t cop = argc >= 1 ? vm_to_int(args[0]) : -1;
            VMValue c0 = argc >= 2 ? args[1] : VM_UNDEF_VAL;
            VMValue c1 = argc >= 3 ? args[2] : VM_UNDEF_VAL;
            VMValue c2 = argc >= 4 ? args[3] : VM_UNDEF_VAL;
            VMValue cres = VM_UNDEF_VAL;
            int cth = 0;
            vm->stack_top = sp;
            int _cps = vm_worker_enter_safe(vm);
            switch (cop) {
            case 0: cth = vm_c_call_1(vm, strada_coro_create, c0, &cres, &throw_exc); break;
            case 1:
                cth = vm_c_call_1(vm, strada_coro_resume, c0, &cres, &throw_exc);
                g_active_vm = vm;   /* the task ran in its own VM; we are back */
                break;
            case 2: {
                int in_task = strada_coro_in_task();
                if (in_task) vm_task_locals_swap(vm, 0);
                cth = vm_c_call_3(vm, strada_coro_yield_io, c0, c1, c2, &cres, &throw_exc);
                g_active_vm = vm;   /* resumed: this task's VM is active again */
                if (in_task) vm_task_locals_swap(vm, 1);
                break;
            }
            case 3: cth = vm_c_call_1(vm, strada_coro_state, c0, &cres, &throw_exc); break;
            case 4: cth = vm_c_call_1(vm, strada_coro_wait_fd, c0, &cres, &throw_exc); break;
            case 5: cth = vm_c_call_1(vm, strada_coro_wait_mask, c0, &cres, &throw_exc); break;
            case 6: cth = vm_c_call_1(vm, strada_coro_wait_timeout, c0, &cres, &throw_exc); break;
            case 7: cth = vm_c_call_1(vm, strada_coro_error, c0, &cres, &throw_exc); break;
            case 8: cth = vm_c_call_1(vm, strada_coro_result, c0, &cres, &throw_exc); break;
            case 9: cth = vm_c_call_1(vm, strada_coro_free, c0, &cres, &throw_exc); break;
            default: break;
            }
            vm_worker_leave_safe(vm, _cps);
            stack = vm->stack; RELOAD();
            if (cth) goto L_builtin_throw_exc;
            SP_PUSH(cres);
            break;
        }
        case BUILTIN_EXC_TRACE: {
            SP_PUSH(vm->exc_trace ? vm_str(vm->exc_trace) : vm_str(""));
            break;
        }
        case BUILTIN_CALL_NAMED_GUARDED: {
            /* The compiler's parse recovery (parse_function_guarded) calls a
             * function by name under a try frame: [1, result] / [0, error] /
             * [-1] when there is no such function. Over the runtime bridge it
             * found the interpreter binary's own compiled function and passed
             * it a copy of the argument, so the VM parser never advanced
             * (lib/Eval under strada-interp looped forever). */
            char gnb[256];
            const char *gfn = vm_to_cstr(argc >= 1 ? args[0] : VM_UNDEF_VAL, gnb, sizeof(gnb));
            int gfi = vm_program_find_func(vm->program, gfn);
            if (gfi < 0) {
                size_t gl = strlen(gfn);
                for (size_t gi = 0; gi < vm->program->func_count && gfi < 0; gi++) {
                    const char *fnm = vm->program->func_names[gi];
                    size_t fl = fnm ? strlen(fnm) : 0;
                    if (fl > gl + 2 && strcmp(fnm + fl - gl, gfn) == 0 && fnm[fl - gl - 1] == ':' && fnm[fl - gl - 2] == ':')
                        gfi = (int)gi;
                }
            }
            VMArray *gres = vm_array_new(2);
            if (gfi < 0) {
                vm_array_push(gres, VM_MAKE_INT(-1));
                SP_PUSH(VM_MAKE_PTR(gres));
                break;
            }
            VMValue garg = argc >= 2 ? args[1] : VM_UNDEF_VAL;
            vm->stack_top = sp;
            VMValue gr = vm_invoke_func(vm, gfi, &garg, 1);
            stack = vm->stack; RELOAD();
            if (vm->has_pending_exc) {
                VMValue gex = vm->pending_exc;
                vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                vm_array_push(gres, VM_MAKE_INT(0));
                vm_array_push(gres, gex);
            } else {
                vm_array_push(gres, VM_MAKE_INT(1));
                vm_array_push(gres, gr);
            }
            SP_PUSH(VM_MAKE_PTR(gres));
            break;
        }
        case BUILTIN_TYPEOF_TO_REF: {
            /* typeof($r) with $r holding an array/hash ref is "ref" (compiled);
             * the VM keeps the container itself in the scalar */
            char tb[16];
            const char *tn = vm_to_cstr(argc >= 1 ? args[0] : VM_UNDEF_VAL, tb, sizeof(tb));
            if (strcmp(tn, "array") == 0 || strcmp(tn, "hash") == 0) SP_PUSH(vm_str("ref"));
            else SP_PUSH(vm_val_copy(argc >= 1 ? args[0] : VM_UNDEF_VAL));
            break;
        }
        case BUILTIN_SAY_ARG: {
            /* say(@a) / say(f()) when f may return a list: an (unblessed)
             * array prints its elements, any other value prints as is */
            VMValue sv0 = argc >= 1 ? args[0] : VM_UNDEF_VAL;
            if (!(VM_IS_PTR(sv0) && VM_PTR_TYPE(sv0) == VM_OBJ_ARRAY && vm_obj_class_name(sv0) == NULL)) {
                SP_PUSH(vm_val_copy(sv0));
                break;
            }
            VMArray *sa = VM_AS_ARRAY(sv0);
            StradaValue *acc = strada_new_str("");
            vm->stack_top = sp;
            vm->gc_pause++;
            for (int si = 0; si < sa->size; si++) {
                VMValue piece = vm_stringify_ov(vm, sa->items[si]);
                if (vm->has_pending_exc) break;
                StradaValue *psv = vm_to_sv(piece);
                acc = strada_concat_inplace(acc, psv);
                strada_decref(psv);
            }
            vm->gc_pause--;
            stack = vm->stack; RELOAD();
            if (__builtin_expect(vm->has_pending_exc, 0)) {
                strada_decref(acc);
                throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                goto L_builtin_throw_exc;
            }
            VMValue jout = sv_to_vm(acc);
            strada_decref(acc);
            SP_PUSH(jout);
            break;
        }
        case BUILTIN_REV_LISTIFY: {
            VMValue rv = argc >= 1 ? args[0] : VM_UNDEF_VAL;
            if (VM_IS_PTR(rv) && VM_PTR_TYPE(rv) == VM_OBJ_CELL) rv = VM_AS_CELL(rv)->val;
            if (VM_IS_PTR(rv) && VM_PTR_TYPE(rv) == VM_OBJ_ARRAY && vm_obj_class_name(rv) == NULL) {
                SP_PUSH(vm_val_copy(rv));
            } else {
                VMArray *one = vm_array_new(1);
                vm_array_push(one, vm_val_copy(rv));
                SP_PUSH(VM_MAKE_PTR(one));
            }
            break;
        }
        case BUILTIN_LIST_LAST: {
            VMArray *ll = (argc >= 1 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_ARRAY) ? VM_AS_ARRAY(args[0]) : NULL;
            if (ll) SP_PUSH(ll->size > 0 ? vm_val_copy(vm_array_get(ll, (int64_t)ll->size - 1)) : VM_UNDEF_VAL);
            else SP_PUSH(argc >= 1 ? vm_val_copy(args[0]) : VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_REV_JOINSTR: {
            /* args[0]: the flattened argument list. A single unblessed array
             * argument (the Strada `reverse($aref)` idiom) contributes its
             * elements. Each element is stringified (honouring "" overloads),
             * the pieces are joined, and the string is reversed by the
             * runtime (codepoint-aware, like the compiled backend). */
            VMArray *rl = (argc >= 1 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_ARRAY) ? VM_AS_ARRAY(args[0]) : NULL;
            if (rl && rl->size == 1 && VM_IS_PTR(rl->items[0]) && VM_PTR_TYPE(rl->items[0]) == VM_OBJ_ARRAY &&
                vm_obj_class_name(rl->items[0]) == NULL) {
                rl = VM_AS_ARRAY(rl->items[0]);
            }
            StradaValue *acc = strada_new_str("");
            vm->stack_top = sp;
            vm->gc_pause++;
            for (int ri = 0; rl && ri < rl->size; ri++) {
                VMValue piece = vm_stringify_ov(vm, rl->items[ri]);
                if (vm->has_pending_exc) break;
                StradaValue *psv = vm_to_sv(piece);
                acc = strada_concat_inplace(acc, psv);
                strada_decref(psv);
            }
            vm->gc_pause--;
            stack = vm->stack; RELOAD();
            if (__builtin_expect(vm->has_pending_exc, 0)) {
                strada_decref(acc);
                throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                goto L_builtin_throw_exc;
            }
            StradaValue *rev = strada_reverse_sv(acc);
            strada_decref(acc);
            VMValue out = sv_to_vm(rev);
            strada_decref(rev);
            SP_PUSH(out);
            break;
        }
        case BUILTIN_FE_LISTIFY: {
            /* the list a foreach iterates when its source is a call result:
             * an array as is, undef as the empty list, any other value as a
             * one-element list (strada_foreach_list_take) */
            VMValue fv = argc >= 1 ? args[0] : VM_UNDEF_VAL;
            if (VM_IS_PTR(fv) && VM_PTR_TYPE(fv) == VM_OBJ_ARRAY) {
                SP_PUSH(vm_val_copy(fv));
            } else if (VM_IS_UNDEF(fv)) {
                SP_PUSH(VM_MAKE_PTR(vm_array_new(0)));
            } else {
                VMArray *one = vm_array_new(1);
                vm_array_push(one, vm_val_copy(fv));
                SP_PUSH(VM_MAKE_PTR(one));
            }
            break;
        }
        case BUILTIN_HASH_FLAT: {
            if (argc >= 1 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_HASH) {
                vm->stack_top = sp;
                vm->gc_pause++;
                VMArray *fl = vm_hash_to_flat_kv(vm, VM_AS_HASH(args[0]));
                vm->gc_pause--;
                stack = vm->stack; RELOAD();
                if (__builtin_expect(vm->has_pending_exc, 0)) {
                    throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                    goto L_builtin_throw_exc;
                }
                SP_PUSH(fl ? VM_MAKE_PTR(fl) : VM_MAKE_PTR(vm_array_new(0)));
            } else if (argc >= 1 && !VM_IS_UNDEF(args[0])) {
                SP_PUSH(vm_val_copy(args[0]));
            } else {
                SP_PUSH(VM_MAKE_PTR(vm_array_new(0)));
            }
            break;
        }
        case BUILTIN_PRINTF: {
            /* args[0]: the flattened argument list. A filehandle (VM or
             * runtime socket/handle) first is the destination. */
            VMArray *pa = (argc >= 1 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_ARRAY) ? VM_AS_ARRAY(args[0]) : NULL;
            if (!pa || pa->size == 0) { SP_PUSH(VM_UNDEF_VAL); break; }
            int pstart = 0;
            VMFileHandle *pfh = NULL;
            StradaValue *pnfh = NULL;
            FILE *pfp = stdout;
            if (VM_IS_PTR(vm->default_fh) && VM_PTR_TYPE(vm->default_fh) == VM_OBJ_FILEHANDLE) {
                pfh = (VMFileHandle*)VM_TO_PTR(vm->default_fh);
                pfp = (FILE*)pfh->fp;
            }
            VMValue pfirst = pa->items[0];
            if (pa->size >= 2 && VM_IS_PTR(pfirst) && VM_PTR_TYPE(pfirst) == VM_OBJ_FILEHANDLE) {
                pfh = (VMFileHandle*)VM_TO_PTR(pfirst);
                pfp = (FILE*)pfh->fp;
                pstart = 1;
            } else if (pa->size >= 2 && VM_IS_PTR(pfirst) && VM_PTR_TYPE(pfirst) == VM_OBJ_NATIVE_SV) {
                StradaValue *nsv = ((VMNativeSV*)VM_TO_PTR(pfirst))->sv;
                if (nsv && !STRADA_IS_TAGGED_INT(nsv) && (nsv->type == STRADA_SOCKET || nsv->type == STRADA_FILEHANDLE)) {
                    pnfh = nsv;
                    pstart = 1;
                    pfh = NULL;
                }
            }
            vm->stack_top = sp;
            vm->gc_pause++;
            VMValue pres = vm_sprintf(vm, pa->size - pstart, pa->items + pstart);
            vm->gc_pause--;
            stack = vm->stack; RELOAD();
            if (__builtin_expect(vm->has_pending_exc, 0)) {
                throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                goto L_builtin_throw_exc;
            }
            if (pnfh) {
                StradaValue *psv = vm_to_sv(pres);
                strada_print_fh(psv, pnfh);
                strada_decref(psv);
            } else if (pfp) {
                vm_write_val(pfp, pres, 0);
            }
            vm_val_free(&pres);
            if (pfh && pfh->is_mem == 2 && VM_IS_PTR(pfh->mem_sref)) {
                vm->stack_top = sp;
                vm_fh_sync_mem_sref(vm, pfh);
                stack = vm->stack; RELOAD();
                if (__builtin_expect(vm->has_pending_exc, 0)) {
                    throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                    goto L_builtin_throw_exc;
                }
            }
            SP_PUSH(VM_MAKE_INT(1));
            break;
        }
        case BUILTIN_SET_PACKAGE: {
            char spbuf[256];
            const char *spn = vm_to_cstr(args[0], spbuf, sizeof(spbuf));
            free(vm->cur_package);
            vm->cur_package = strdup(spn ? spn : "main");
            SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_INHERIT: {
            /* Runtime inherit(child, parent) — append an inheritance edge;
             * inherit(parent) uses the package from set_package() */
            if (argc >= 1) {
                char cbuf[128], pbuf[128];
                const char *child = (argc >= 2) ? vm_to_cstr(args[0], cbuf, 128)
                                                : (vm->cur_package ? vm->cur_package : "main");
                const char *parent = vm_to_cstr(args[argc >= 2 ? 1 : 0], pbuf, 128);
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
                        vm->stack_top = sp;
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
                            vm->stack_top = sp; vm->gc_pause++;
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
            vm->stack_top = sp;
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
                vm->stack_top = sp;
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
        case BUILTIN_HEX: { if (argc >= 1) RT_CALL_1(strada_hex) else SP_PUSH(VM_MAKE_INT(0)); break; }   /* shared with compiled code: "x1f", "0x..", "1_0" */
        case BUILTIN_OCT: { if (argc >= 1) RT_CALL_1(strada_oct) else SP_PUSH(VM_MAKE_INT(0)); break; }   /* shared Perl oct() */
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
            if (argc >= 2 && VM_IS_PTR(args[0]) &&
                (VM_PTR_TYPE(args[0]) == VM_OBJ_STR || VM_PTR_TYPE(args[0]) == VM_OBJ_STRBUF)) {
                /* "a".."e" / "x".."ab": the runtime's string range (Perl);
                 * numeric strings still give numbers */
                StradaValue *ra = vm_to_sv(args[0]);
                StradaValue *rb = vm_to_sv(args[1]);
                StradaValue *rr = strada_range(ra, rb);
                strada_decref(ra);
                strada_decref(rb);
                SP_PUSH(rr ? sv_to_vm(rr) : VM_MAKE_PTR(vm_array_new(0)));
                if (rr) strada_decref(rr);
                break;
            }
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
                    vm->stack_top = sp;
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
                    vm->stack_top = sp;
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
                    vm->stack_top = sp;
                    VMArray *rem = vm_tied_array_splice(vm, arr, argc, args);
                    stack = vm->stack; RELOAD();
                    if (__builtin_expect(vm->has_pending_exc, 0)) {
                        throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                        goto L_builtin_throw_exc;
                    }
                    SP_PUSH(VM_MAKE_PTR(rem));
                    break;
                }
                int64_t offset = 0;
                if (argc >= 2) {
                    if (VM_IS_PTR(args[1])) {
                        vm->stack_top = sp; vm->gc_pause++;
                        offset = vm_to_int_vm(vm, args[1]);
                        vm->gc_pause--; stack = vm->stack; RELOAD();
                        if (__builtin_expect(vm->has_pending_exc, 0)) {
                            throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                            goto L_builtin_throw_exc;
                        }
                        arr = VM_AS_ARRAY(args[0]);
                    } else offset = vm_to_int(args[1]);
                }
                int has_len = (argc >= 3);
                int64_t length = 0;
                if (has_len) {
                    if (VM_IS_PTR(args[2])) {
                        vm->stack_top = sp; vm->gc_pause++;
                        length = vm_to_int_vm(vm, args[2]);
                        vm->gc_pause--; stack = vm->stack; RELOAD();
                        if (__builtin_expect(vm->has_pending_exc, 0)) {
                            throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                            goto L_builtin_throw_exc;
                        }
                        arr = VM_AS_ARRAY(args[0]);
                    } else length = vm_to_int(args[2]);
                }
                if (__builtin_expect(vm->has_weakrefs, 0)) {
                    vm->stack_top = sp;
                    vm_unwrap_container_weak_gc(vm, args[0], args, argc);
                    for (int ai = 3; ai < argc; ai++) {
                        if (VM_IS_PTR(args[ai]) && VM_PTR_TYPE(args[ai]) == VM_OBJ_ARRAY) {
                            vm_unwrap_container_weak_gc(vm, args[ai], args, argc);
                        } else if (VM_IS_PTR(args[ai]) && VM_PTR_TYPE(args[ai]) == VM_OBJ_WEAKREF) {
                            args[ai] = vm_unwrap_weak_gc(vm, args[ai], args, argc);
                        }
                    }
                    stack = vm->stack; RELOAD();
                    arr = VM_AS_ARRAY(args[0]);
                }
                /* Collect replacement elements before mutating arr (handles tied & self-spliced arrays) */
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
                        vm->stack_top = sp;
                        int exc = vm_collect_spread_items(vm, rarg, snap_buf, 16, &snap, &repl_count);
                        stack = vm->stack; RELOAD();
                        arr = VM_AS_ARRAY(args[0]);
                        if (__builtin_expect(exc || vm->has_pending_exc, 0)) {
                            if (snap && snap != snap_buf) free(snap);
                            throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                            goto L_builtin_throw_exc;
                        }
                        repl_items = snap;
                    } else {
                        single_item = rarg;
                        repl_items = &single_item;
                        repl_count = 1;
                    }
                } else if (argc > 4) {
                    repl_count = argc - 3;
                    repl_items = &args[3];
                }
                if (offset < 0) offset = arr->size + offset;
                if (offset < 0) offset = 0;
                if (offset > arr->size) offset = arr->size;
                if (!has_len) length = arr->size - offset;
                if (length < 0) {
                    length = (arr->size - offset) + length;
                    if (length < 0) length = 0;
                }
                if (length > (int64_t)arr->size - offset) length = (int64_t)arr->size - offset;
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
                /* Shift elements */
                int64_t diff = (int64_t)repl_count - length;
                int tail_count = arr->size - (int)offset - (int)length;
                if (diff > 0) {
                    int new_cap = arr->cap;
                    while (arr->size + diff > new_cap) {
                        new_cap = new_cap < 1024 ? new_cap * 2 : new_cap + new_cap / 2;
                        if (new_cap < 8) new_cap = 8;
                    }
                    vm_array_ensure_cap(arr, new_cap);
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
                        vm->stack_top = sp; vm->gc_pause++;
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
                        vm->stack_top = sp;
                        ev = vm_unwrap_weak_gc(vm, ev, args, argc);
                        stack = vm->stack; RELOAD();
                    }
                    VMArray *pair = vm_array_new(2);
                    vm_array_push(pair, vm_hash_make_key_str_b(h->entries[h->iter_pos].key, h->entries[h->iter_pos].keylen, h->entries[h->iter_pos].key_bytes));
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
            if (argc >= 1) {
                unsigned sec = (unsigned)vm_to_int(args[0]);
                vm->stack_top = sp;
                int _ps = vm_worker_enter_safe(vm);
                sleep(sec);
                vm_worker_leave_safe(vm, _ps);
                stack = vm->stack; RELOAD();
            }
            SP_PUSH(VM_UNDEF_VAL);
            break;
        }
        case BUILTIN_CORE_USLEEP: {
            if (argc >= 1) {
                useconds_t us = (useconds_t)vm_to_int(args[0]);
                vm->stack_top = sp;
                int _ps = vm_worker_enter_safe(vm);
                usleep(us);
                vm_worker_leave_safe(vm, _ps);
                stack = vm->stack; RELOAD();
            }
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
            /* the runtime's stat: the same keys (blksize, blocks, rdev,
             * mtime_ns, ...) and NUL-path rejection as the compiled backend */
            if (argc >= 1) {
                StradaValue *psv = vm_to_sv(args[0]);
                StradaValue *r = (bid == BUILTIN_CORE_LSTAT) ? strada_lstat(psv) : strada_stat(psv);
                strada_decref(psv);
                SP_PUSH(r ? sv_to_vm(r) : VM_UNDEF_VAL);
                if (r) strada_decref(r);
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
            vm->stack_top = sp;
            int _ps = vm_worker_enter_safe(vm);
            pid_t pid = wait(&status);
            vm_worker_leave_safe(vm, _ps);
            stack = vm->stack; RELOAD();
            if (__builtin_expect(strada_has_pending_signals(), 0)) {
                vm->stack_top = sp;
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
                pid_t target_pid = (pid_t)vm_to_int(args[0]);
                int options = (argc >= 2) ? (int)vm_to_int(args[1]) : 0;
                vm->stack_top = sp;
                int _ps = vm_worker_enter_safe(vm);
                pid_t pid = waitpid(target_pid, &status, options);
                vm_worker_leave_safe(vm, _ps);
                stack = vm->stack; RELOAD();
                if (__builtin_expect(strada_has_pending_signals(), 0)) {
                    vm->stack_top = sp;
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
            vm_hash_set(info, "file", vm_str((cf->chunk && cf->chunk->src_file) ? cf->chunk->src_file : "(unknown)"));
            vm_hash_set(info, "line", VM_MAKE_INT(vm_frame_line(cf)));
            SP_PUSH(VM_MAKE_PTR(info));
            break;
        }
        case BUILTIN_CORE_STACK_TRACE: {
            /* the active frames (the dispatch loop synced frame->ip) */
            char *trace = vm_build_trace(vm, (int)vm->frame_count - 1, 0);
            if (trace) { SP_PUSH(vm_str(trace)); free(trace); }
            else SP_PUSH(vm_str(""));
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
        case BUILTIN_CORE_BASE64_DECODE: { if (argc >= 1) RT_CALL_1(strada_base64_decode) else SP_PUSH(vm_str("")); break; }   /* runtime decoder (padding / whitespace rules) */
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
        case BUILTIN_CORE_FILE_EXT: { if (argc >= 1) RT_CALL_1(strada_file_ext) else SP_PUSH(vm_str("")); break; }
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
                    vm->stack_top = sp;
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
            vm->stack_top = sp;
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
                vm->stack_top = sp;
                int _ps = vm_worker_enter_safe(vm);
                nanosleep(&ts, NULL);
                vm_worker_leave_safe(vm, _ps);
                stack = vm->stack; RELOAD();
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
        case BUILTIN_CORE_GLOB: { if (argc >= 1) RT_CALL_1(strada_glob) else SP_PUSH(VM_MAKE_PTR(vm_array_new(0))); break; }   /* runtime glob: csh brace expansion */

        /* ===== Runtime-delegated builtins ===== */
        /* These call the Strada runtime functions (StradaValue*-based) and convert results */
        /* Helper: call a runtime function, convert args via vm_to_sv, result via sv_to_vm */
#define RT_VOID_1(fn) { vm->stack_top = sp; int __th = vm_c_void_1(vm, (fn), args[0], &throw_exc); stack = vm->stack; RELOAD(); if (__th) goto L_builtin_throw_exc; SP_PUSH(VM_UNDEF_VAL); }
#define RT_VOID_1_SV(fn) { vm->stack_top = sp; int __th = vm_c_call_1_sv_void(vm, (fn), args[0], &throw_exc); stack = vm->stack; RELOAD(); if (__th) goto L_builtin_throw_exc; SP_PUSH(VM_UNDEF_VAL); }

        /* === Socket/networking === */
        case BUILTIN_CORE_SOCKET_CLIENT: {
            char hbuf[256];
            const char *host = vm_to_cstr(args[0], hbuf, 256);
            int port = (int)vm_to_int(args[1]);
            vm->stack_top = sp;
            int _ps = vm_worker_enter_safe(vm);
            StradaValue *r = strada_socket_client(host, port);
            vm_worker_leave_safe(vm, _ps);
            stack = vm->stack; RELOAD();
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
            int nbytes = (int)vm_to_int(args[1]);
            vm->stack_top = sp;
            int _ps = vm_worker_enter_safe(vm);
            StradaValue *r = strada_socket_recv(sock, nbytes);
            vm_worker_leave_safe(vm, _ps);
            stack = vm->stack; RELOAD();
            SP_PUSH(sv_to_vm(r)); strada_decref(r); strada_decref(sock);
            break;
        }
        case BUILTIN_CORE_SOCKET_SEND: {
            StradaValue *sock = vm_to_sv(args[0]);
            StradaValue *data = vm_to_sv(args[1]);
            vm->stack_top = sp;
            int _ps = vm_worker_enter_safe(vm);
            int rc = strada_socket_send_sv(sock, data);
            vm_worker_leave_safe(vm, _ps);
            stack = vm->stack; RELOAD();
            SP_PUSH(VM_MAKE_INT(rc));
            strada_decref(sock); strada_decref(data);
            break;
        }
        case BUILTIN_CORE_SOCKET_CLOSE: RT_VOID_1_SV(strada_socket_close); break;
        case BUILTIN_CORE_SOCKET_FLUSH: RT_VOID_1_SV(strada_socket_flush); break;
        case BUILTIN_CORE_SOCKET_SELECT: {
            StradaValue *socks = vm_to_sv(args[0]);
            int ms = (int)vm_to_int(args[1]);
            int _prev_safe = vm_worker_enter_safe(vm);
            StradaValue *r = strada_socket_select(socks, ms);
            vm_worker_leave_safe(vm, _prev_safe);
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
            int maxb = (int)vm_to_int(args[1]);
            int _prev_safe = vm_worker_enter_safe(vm);
            StradaValue *r = strada_udp_recvfrom(sock, maxb);
            vm_worker_leave_safe(vm, _prev_safe);
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
            vm->stack_top = sp;
            int _ps = vm_worker_enter_safe(vm);
            StradaValue *r = strada_read_byte(fd);
            vm_worker_leave_safe(vm, _ps);
            stack = vm->stack; RELOAD();
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
            /* trapped: sending on a closed channel throws a catchable error */
            VMValue sres = VM_UNDEF_VAL;
            vm->stack_top = sp;
            int _ps = vm_worker_enter_safe(vm);
            int sth = vm_c_call_2(vm, vm_channel_send_rt, args[0], argc >= 2 ? args[1] : VM_UNDEF_VAL, &sres, &throw_exc);
            vm_worker_leave_safe(vm, _ps);
            stack = vm->stack; RELOAD();
            if (sth) goto L_builtin_throw_exc;
            SP_PUSH(VM_UNDEF_VAL);
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
                    vm->stack_top = sp; vm->gc_pause++;
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
                            vm->stack_top = sp; vm->gc_pause++;
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
        case BUILTIN_SUBSTR_REPL: {
            /* 4-arg / lvalue substr through the runtime's strada_substr_replace
             * so both backends share the range rule, UTF-8 handling and the
             * "substr outside of string" error. */
            if (argc < 4) { SP_PUSH(VM_UNDEF_VAL); break; }
            int64_t sr_off = vm_to_int(args[1]);
            int64_t sr_len = vm_to_int(args[2]);   /* 2-arg form passes VM_INT_MAX */
            StradaValue *sr_cur = vm_to_sv(args[0]);
            StradaValue *sr_rep = vm_to_sv(args[3]);
            StradaValue * volatile sr_new = NULL;
            StradaValue *sr_removed = NULL;
            int sr_cm = strada_cleanup_mark();
            volatile int sr_failed = 0;
            if (setjmp(*STRADA_TRY_PUSH()) == 0) {
                sr_new = strada_substr_replace(sr_cur, sr_off, sr_len, sr_rep, &sr_removed);
                STRADA_TRY_POP();
            } else {
                STRADA_TRY_POP();
                strada_cleanup_drain_to(sr_cm);
                strada_clear_exception();
                sr_failed = 1;
            }
            strada_decref(sr_cur);
            strada_decref(sr_rep);
            if (sr_failed) {
                throw_exc = vm_str("substr outside of string");
                { *out_throw = throw_exc; vm->stack_top = sp; return 1; }
            }
            VMArray *sr_pair = vm_array_new(2);
            vm_array_push(sr_pair, sv_to_vm(sr_new));
            vm_array_push(sr_pair, sv_to_vm(sr_removed));
            strada_decref(sr_new);
            strada_decref(sr_removed);
            SP_PUSH(VM_MAKE_PTR(sr_pair));
            break;
        }
        case BUILTIN_CONTAINER_REPLACE: {
            /* @{EXPR} = LIST / %{EXPR} = LIST: replace the referenced
             * container's contents in place (tied containers: CLEAR+STORE,
             * as BUILTIN_TIE_ASSIGN). */
            if (argc < 2) { SP_PUSH(argc ? args[0] : VM_UNDEF_VAL); break; }
            VMValue rt = args[0], rv = args[1];
            int r_arr = VM_IS_PTR(rt) && VM_PTR_TYPE(rt) == VM_OBJ_ARRAY && VM_IS_UNDEF(VM_AS_ARRAY(rt)->tied_obj);
            int r_hash = VM_IS_PTR(rt) && VM_PTR_TYPE(rt) == VM_OBJ_HASH && VM_IS_UNDEF(VM_AS_HASH(rt)->tied_obj);
            if (!r_arr && !r_hash) goto L_builtin_tie_assign;
            vm->stack_top = sp;
            vm->gc_pause++;
            /* snapshot the source first: it may alias the target */
            VMArray *src = NULL;
            if (VM_IS_PTR(rv) && VM_PTR_TYPE(rv) == VM_OBJ_HASH) {
                src = vm_hash_to_flat_kv(vm, VM_AS_HASH(rv));
            } else if (VM_IS_PTR(rv) && VM_PTR_TYPE(rv) == VM_OBJ_ARRAY) {
                VMArray *sa = VM_AS_ARRAY(rv);
                src = vm_array_new(sa->size > 0 ? sa->size : 1);
                for (int i = 0; i < sa->size; i++) vm_array_push(src, vm_val_copy(sa->items[i]));
            } else {
                src = vm_array_new(1);
                if (!VM_IS_UNDEF(rv)) vm_array_push(src, vm_val_copy(rv));
            }
            if (r_hash && vm_list_keys_stringify(vm, src)) {
                vm->gc_pause--;
                stack = vm->stack; RELOAD();
                throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                goto L_builtin_throw_exc;
            }
            if (r_arr) {
                VMArray *ta = VM_AS_ARRAY(rt);
                for (int i = 0; i < ta->size; i++) vm_val_free(&ta->items[i]);
                ta->size = 0;
                if (ta->holes) { free(ta->holes); ta->holes = NULL; }
                for (int i = 0; src && i < src->size; i++) vm_array_push(ta, vm_val_copy(src->items[i]));
            } else {
                VMHash *th = VM_AS_HASH(rt);
                for (int i = 0; i < th->capacity; i++) {
                    if (th->entries[i].occupied == 1) {
                        key_free_entry(&th->entries[i]);
                        vm_val_free(&th->entries[i].value);
                    }
                }
                vm_hash_reset_storage(th);
                for (int i = 0; src && i < src->size; i += 2) {
                    char kb[256];
                    size_t kl = 0;
                    const char *kp = vm_bytes_view(src->items[i], kb, sizeof(kb), &kl);
                    VMValue val = (i + 1 < src->size) ? vm_val_copy(src->items[i + 1]) : VM_UNDEF_VAL;
                    vm_hash_set_n(th, kp ? kp : "", kp ? kl : 0, val);
                }
            }
            vm->gc_pause--;
            stack = vm->stack; RELOAD();
            SP_PUSH(rt);
            break;
        }
        L_builtin_tie_assign:
        case BUILTIN_TIE_ASSIGN: {
            /* @a = LIST / %h = LIST on a tied container (Perl): CLEAR, then
             * STORE each element / pair; the tie stays, and the result (what
             * the caller stores back into the variable) is the target. */
            if (argc < 2) { SP_PUSH(argc ? args[0] : VM_UNDEF_VAL); break; }
            VMValue tgt = args[0], nv = args[1];
            int tkind = 0;
            VMValue tobj = VM_UNDEF_VAL;
            if (VM_IS_PTR(tgt) && VM_PTR_TYPE(tgt) == VM_OBJ_ARRAY && !VM_IS_UNDEF(VM_AS_ARRAY(tgt)->tied_obj)) {
                tkind = 2; tobj = VM_AS_ARRAY(tgt)->tied_obj;
            } else if (VM_IS_PTR(tgt) && VM_PTR_TYPE(tgt) == VM_OBJ_HASH && !VM_IS_UNDEF(VM_AS_HASH(tgt)->tied_obj)) {
                tkind = 1; tobj = VM_AS_HASH(tgt)->tied_obj;
            }
            if (!tkind) { SP_PUSH(nv); break; }
            vm->stack_top = sp;
            int cf = vm_tied_array_find_method(vm, tobj, "CLEAR");
            if (cf >= 0) {
                VMValue r = vm_tied_array_call(vm, tobj, cf, NULL, 0);
                vm_temp_free(&r);
            } else if (tkind == 1) {
                /* no CLEAR (as strada_tied_hash_clear): DELETE every key from
                 * FIRSTKEY/NEXTKEY, else empty the tie object's own hash */
                int df = vm_tied_array_find_method(vm, tobj, "DELETE");
                int has_fk = 0;
                VMArray *tk = (df >= 0) ? vm_tied_hash_collect_keys(vm, tobj, &has_fk) : NULL;
                if (df >= 0 && has_fk && tk && !vm->has_pending_exc) {
                    for (int ki = 0; ki < tk->size && !vm->has_pending_exc; ki++) {
                        VMValue kv = tk->items[ki];
                        VMValue r = vm_tied_array_call(vm, tobj, df, &kv, 1);
                        vm_temp_free(&r);
                    }
                } else if (VM_IS_PTR(tobj) && VM_PTR_TYPE(tobj) == VM_OBJ_HASH) {
                    VMHash *oh = VM_AS_HASH(tobj);
                    for (int hi = 0; hi < oh->capacity; hi++) {
                        if (oh->entries[hi].occupied == 1) {
                            key_free_entry(&oh->entries[hi]);
                            vm_val_free(&oh->entries[hi].value);
                        }
                    }
                    vm_hash_reset_storage(oh);
                }
            } else if (tkind == 2) {
                int ssf = vm_tied_array_find_method(vm, tobj, "STORESIZE");
                VMValue z = VM_MAKE_INT(0);
                if (ssf >= 0) { VMValue r = vm_tied_array_call(vm, tobj, ssf, &z, 1); vm_temp_free(&r); }
            }
            int sf = vm_tied_array_find_method(vm, tobj, "STORE");
            if (sf >= 0 && !vm->has_pending_exc) {
                if (tkind == 2 && VM_IS_PTR(nv) && VM_PTR_TYPE(nv) == VM_OBJ_ARRAY) {
                    VMArray *na = VM_AS_ARRAY(nv);
                    for (int i = 0; i < na->size && !vm->has_pending_exc; i++) {
                        VMValue sa[2] = { VM_MAKE_INT(i), na->items[i] };
                        VMValue r = vm_tied_array_call(vm, tobj, sf, sa, 2);
                        vm_temp_free(&r);
                    }
                } else if (tkind == 1 && VM_IS_PTR(nv) && VM_PTR_TYPE(nv) == VM_OBJ_HASH) {
                    VMHash *nh = VM_AS_HASH(nv);
                    for (int i = 0; i < nh->capacity && !vm->has_pending_exc; i++) {
                        if (nh->entries[i].occupied != 1) continue;
                        VMValue sa[2] = { vm_make_str_n(nh->entries[i].key, nh->entries[i].keylen), nh->entries[i].value };
                        VMValue r = vm_tied_array_call(vm, tobj, sf, sa, 2);
                        vm_temp_free(&r);
                        vm_temp_free(&sa[0]);
                    }
                }
            }
            stack = vm->stack; RELOAD();
            SP_PUSH(tgt);
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
            /* cstruct_new($size) or cstruct_new($name, $size) */
            char nbuf[256];
            const char *name = argc >= 2 ? vm_to_cstr(args[0], nbuf, 256) : "struct";
            int64_t szi = argc >= 2 ? vm_to_int(args[1]) : (argc == 1 ? vm_to_int(args[0]) : 1);
            size_t sz = (size_t)(szi > 0 ? szi : 1);
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
            char fbuf[256]; int cs_short = (argc == 3); const char *field = cs_short ? "" : vm_to_cstr(args[1], fbuf, 256);
            size_t off = (size_t)vm_to_int(args[cs_short ? 1 : 2]);
            strada_cstruct_set_int(s, field, off, vm_to_int(args[cs_short ? 2 : 3]));
            SP_PUSH(VM_UNDEF_VAL); strada_decref(s);
            break;
        }
        case BUILTIN_CORE_CSTRUCT_GET_INT: {
            StradaValue *s = vm_to_sv(args[0]);
            char fbuf[256]; int cs_short = (argc == 2); const char *field = cs_short ? "" : vm_to_cstr(args[1], fbuf, 256);
            size_t off = (size_t)vm_to_int(args[cs_short ? 1 : 2]);
            int64_t r = strada_cstruct_get_int(s, field, off);
            SP_PUSH(VM_MAKE_INT(r)); strada_decref(s);
            break;
        }
        case BUILTIN_CORE_CSTRUCT_SET_STRING: {
            StradaValue *s = vm_to_sv(args[0]);
            char fbuf[256]; int cs_short = (argc == 3); const char *field = cs_short ? "" : vm_to_cstr(args[1], fbuf, 256);
            size_t off = (size_t)vm_to_int(args[cs_short ? 1 : 2]);
            char vbuf[4096]; const char *val = vm_to_cstr(args[cs_short ? 2 : 3], vbuf, 4096);
            strada_cstruct_set_string(s, field, off, val);
            SP_PUSH(VM_UNDEF_VAL); strada_decref(s);
            break;
        }
        case BUILTIN_CORE_CSTRUCT_GET_STRING: {
            StradaValue *s = vm_to_sv(args[0]);
            char fbuf[256]; int cs_short = (argc == 2); const char *field = cs_short ? "" : vm_to_cstr(args[1], fbuf, 256);
            size_t off = (size_t)vm_to_int(args[cs_short ? 1 : 2]);
            char *r = strada_cstruct_get_string(s, field, off);
            SP_PUSH(r ? vm_str(r) : VM_UNDEF_VAL); strada_decref(s);
            break;
        }
        case BUILTIN_CORE_CSTRUCT_SET_DOUBLE: {
            StradaValue *s = vm_to_sv(args[0]);
            char fbuf[256]; int cs_short = (argc == 3); const char *field = cs_short ? "" : vm_to_cstr(args[1], fbuf, 256);
            size_t off = (size_t)vm_to_int(args[cs_short ? 1 : 2]);
            strada_cstruct_set_double(s, field, off, vm_to_num(args[cs_short ? 2 : 3]));
            SP_PUSH(VM_UNDEF_VAL); strada_decref(s);
            break;
        }
        case BUILTIN_CORE_CSTRUCT_GET_DOUBLE: {
            StradaValue *s = vm_to_sv(args[0]);
            char fbuf[256]; int cs_short = (argc == 2); const char *field = cs_short ? "" : vm_to_cstr(args[1], fbuf, 256);
            size_t off = (size_t)vm_to_int(args[cs_short ? 1 : 2]);
            double r = strada_cstruct_get_double(s, field, off);
            VM_PUSH_DOUBLE(sp, r); strada_decref(s);
            break;
        }

        /* === UTF-8 === */
        case BUILTIN_UTF8_IS_UTF8: RT_CALL_1(strada_utf8_is_utf8); break;
        case BUILTIN_UTF8_NFC:  RT_CALL_1(strada_utf8_nfc); break;
        case BUILTIN_UTF8_NFD:  RT_CALL_1(strada_utf8_nfd); break;
        case BUILTIN_UTF8_NFKC: RT_CALL_1(strada_utf8_nfkc); break;
        case BUILTIN_UTF8_NFKD: RT_CALL_1(strada_utf8_nfkd); break;
        case BUILTIN_UTF8_VALID:   RT_CALL_1(strada_utf8_valid); break;
        case BUILTIN_UTF8_ENCODE:  RT_CALL_1(strada_utf8_encode); break;
        case BUILTIN_UTF8_DECODE:  RT_CALL_1(strada_utf8_decode); break;
        case BUILTIN_UTF8_DOWNGRADE: {
            int fail_ok = argc >= 2 ? (int)vm_to_int(args[1]) : 0;
            VMValue vr = VM_UNDEF_VAL;
            vm->stack_top = sp;
            int __th = vm_c_call_downgrade(vm, args[0], fail_ok, &vr, &throw_exc);
            stack = vm->stack; RELOAD();
            if (__th) goto L_builtin_throw_exc;
            SP_PUSH(vr);
            break;
        }
        case BUILTIN_UTF8_UPGRADE: RT_CALL_1(strada_utf8_upgrade); break;
        case BUILTIN_UTF8_DECODE_WB: {
            VMValue pair = VM_UNDEF_VAL;
            vm->stack_top = sp;
            int __th = vm_c_call_utf8_wb(vm, 0, args[0], 0, &pair, &throw_exc);
            stack = vm->stack; RELOAD();
            if (__th) goto L_builtin_throw_exc;
            SP_PUSH(pair);
            break;
        }
        case BUILTIN_UTF8_DOWNGRADE_WB: {
            int fail_ok = (argc >= 2) ? (int)vm_to_int(args[1]) : 0;
            VMValue pair = VM_UNDEF_VAL;
            vm->stack_top = sp;
            int __th = vm_c_call_utf8_wb(vm, 1, args[0], fail_ok, &pair, &throw_exc);
            stack = vm->stack; RELOAD();
            if (__th) goto L_builtin_throw_exc;
            SP_PUSH(pair);
            break;
        }
        case BUILTIN_UTF8_UPGRADE_WB: {
            VMValue pair = VM_UNDEF_VAL;
            vm->stack_top = sp;
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
                int ms = (int)vm_to_int(args[1]);
                int _prev_safe = vm_worker_enter_safe(vm);
                StradaValue *r = strada_select_fds(fds_sv, ms);
                vm_worker_leave_safe(vm, _prev_safe);
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
            vm->exc_from_die = 2;    /* L_vm_throw_exc: the uncaught report is the bare message */
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
                        vm->stack_top = sp;
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
                            { *out_throw = throw_exc; vm->stack_top = sp; return 1; }
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
            { *out_throw = throw_exc; vm->stack_top = sp; return 1; }
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
                    vm->stack_top = sp; vm->gc_pause++;
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
                        vm->stack_top = sp;
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
                    vm->stack_top = sp; vm->gc_pause++;
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
                            vm->stack_top = sp;
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
                if (__builtin_expect(vm_list_keys_has_ov(vm, src), 0)) {
                    VMArray *kc = vm_array_new(src->size);
                    for (int i = 0; i < src->size; i++) vm_array_push(kc, src->items[i]);
                    vm->stack_top = sp;
                    int kth = vm_list_keys_stringify(vm, kc);
                    stack = vm->stack; RELOAD();
                    if (kth) {
                        throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                        goto L_builtin_throw_exc;
                    }
                    src = kc;
                }
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
                    vm->stack_top = sp;
                    args[0] = vm_unwrap_weak_gc(vm, args[0], args, argc);
                    stack = vm->stack; RELOAD();
                }
                if (VM_IS_PTR(args[0])) {
                    switch (VM_PTR_TYPE(args[0])) {
                    case VM_OBJ_ARRAY: SP_PUSH(vm_str("ARRAY")); break;
                    case VM_OBJ_HASH: SP_PUSH(vm_str("HASH")); break;
                    case VM_OBJ_CLOSURE: SP_PUSH(vm_str("CODE")); break;
                    case VM_OBJ_SREF: {
                        /* \$x where $x holds a reference: "REF" (Perl) */
                        VMScalarRef *rsr = VM_AS_SREF(args[0]);
                        VMValue rstv = VM_UNDEF_VAL;
                        if (rsr->global_name) rstv = vm_globals_get(vm, rsr->global_name);
                        else if (VM_IS_PTR(rsr->cell) && VM_PTR_TYPE(rsr->cell) == VM_OBJ_CELL) rstv = VM_AS_CELL(rsr->cell)->val;
                        int r_isref = 0;
                        if (VM_IS_PTR(rstv)) {
                            enum VMObjType rt2 = VM_PTR_TYPE(rstv);
                            r_isref = (rt2 == VM_OBJ_ARRAY || rt2 == VM_OBJ_HASH || rt2 == VM_OBJ_CLOSURE ||
                                       rt2 == VM_OBJ_SREF || rt2 == VM_OBJ_WEAKREF);
                        }
                        SP_PUSH(vm_str(r_isref ? "REF" : "SCALAR"));
                        break;
                    }
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
                    vm->stack_top = sp;
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
        case BUILTIN_DUMPER_STR:
        case BUILTIN_DUMPER_STR_R: {
            if (argc >= 1) {
                StradaValue *sv = vm_to_sv(args[0]);
                if (bid == BUILTIN_DUMPER_STR_R) sv = vm_sv_container_as_ref(sv);
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
                vm->stack_top = sp;
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
            VMValue v = args[0];
            if (VM_IS_PTR(v)) {
                vm->stack_top = sp;
                vm->gc_pause++;
                int64_t iv = vm_to_int_vm(vm, v);
                vm->gc_pause--;
                stack = vm->stack; RELOAD();
                if (__builtin_expect(vm->has_pending_exc, 0)) {
                    throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                    goto L_builtin_throw_exc;
                }
                SP_PUSH(VM_MAKE_INT(iv));
            } else {
                SP_PUSH(VM_MAKE_INT(vm_to_int(v)));
            }
            break;
        }
        case BUILTIN_CAST_NUM: {
            VMValue v = args[0];
            if (VM_IS_PTR(v)) {
                vm->stack_top = sp;
                vm->gc_pause++;
                double nv = vm_to_num_vm(vm, v);
                vm->gc_pause--;
                stack = vm->stack; RELOAD();
                if (__builtin_expect(vm->has_pending_exc, 0)) {
                    throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                    goto L_builtin_throw_exc;
                }
                VM_PUSH_DOUBLE(sp, nv);
            } else {
                VM_PUSH_DOUBLE(sp, vm_to_num(v));
            }
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
                if (nlen == 0) {
                    /* Perl: an empty needle is found at the position
                     * clamped to [0, length] */
                    int64_t total = is_u8 ? (int64_t)vm_utf8_char_count(h, hlen) : (int64_t)hlen;
                    pos = offset > total ? total : offset;
                } else if (is_u8) {
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
                    vm->stack_top = sp; vm->gc_pause++;
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
                    vm->stack_top = sp; vm->gc_pause++;
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
                    vm->stack_top = sp; vm->gc_pause++;
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
                            vm->stack_top = sp;
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
                        int new_cap = dst->cap < 1024 ? dst->cap * 2 : dst->cap + dst->cap / 2;
                        while (new_cap < need) new_cap = (new_cap < 1024) ? new_cap * 2 : new_cap + new_cap / 2;
                        vm_array_ensure_cap(dst, new_cap);
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

        case BUILTIN_READ_ALL_LINES: {
            VMArray *la = vm_array_new(16);
            FILE *lfp = NULL;
            if (argc >= 1 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_FILEHANDLE)
                lfp = (FILE*)((VMFileHandle*)VM_TO_PTR(args[0]))->fp;
            if (lfp) {
                size_t cap = 256;
                char *buf = malloc(cap);
                int _ls = vm_worker_enter_safe(vm);
                for (;;) {
                    size_t len = 0;
                    int c = EOF;
                    while ((c = fgetc(lfp)) != EOF) {
                        if (len + 1 >= cap) { cap *= 2; buf = realloc(buf, cap); }
                        buf[len++] = (char)c;
                        if (c == '\n') break;
                    }
                    if (len == 0 && c == EOF) break;
                    if (len > 0 && buf[len - 1] == '\n') len--;
                    vm_array_push(la, VM_MAKE_STR_N(buf, len));
                    if (c == EOF) break;
                }
                vm_worker_leave_safe(vm, _ls);
                free(buf);
            } else if (argc >= 1 && VM_IS_PTR(args[0]) && VM_PTR_TYPE(args[0]) == VM_OBJ_NATIVE_SV) {
                StradaValue *rsv = vm_to_sv(args[0]);
                StradaValue *all = strada_read_all_lines(rsv);
                strada_decref(rsv);
                VMValue av = sv_to_vm(all);
                strada_decref(all);
                if (VM_IS_PTR(av) && VM_PTR_TYPE(av) == VM_OBJ_ARRAY) { SP_PUSH(av); break; }
            }
            SP_PUSH(VM_MAKE_PTR(la));
            break;
        }

        case BUILTIN_STD_HANDLE: {
            /* STDIN / STDOUT / STDERR: a handle over the process stream,
             * owned by an immortal runtime filehandle (never fclose'd by GC) */
            int64_t which = argc >= 1 ? vm_to_int(args[0]) : 0;
            if (which < 0 || which > 2) which = 0;
            FILE *sfp = which == 1 ? stdout : which == 2 ? stderr : stdin;
            static StradaValue *std_sv[3];   /* one per stream, process-wide */
            static pthread_mutex_t std_sv_lock = PTHREAD_MUTEX_INITIALIZER;
            pthread_mutex_lock(&std_sv_lock);
            if (!std_sv[which]) {
                StradaValue *nsv = strada_new_undef();
                nsv->type = STRADA_FILEHANDLE;
                nsv->value.fh = sfp;
                STRADA_MAKE_IMMORTAL_SV(nsv);
                std_sv[which] = nsv;
            }
            StradaValue *hsv = std_sv[which];
            pthread_mutex_unlock(&std_sv_lock);
            VMFileHandle *sfh = calloc(1, sizeof(VMFileHandle));
            vm_gc_init_hdr(&sfh->hdr, VM_OBJ_FILEHANDLE);
            sfh->fp = sfp;
            sfh->owner_sv = hsv;
            SP_PUSH(VM_MAKE_PTR(sfh));
            break;
        }

        case BUILTIN_REGEX_GPOS_NEXT: {
            /* strada_regex_match_global: past the match; a zero-width match
             * steps one character (the end of the subject + 1 ends the loop) */
            char sbuf[256];
            size_t slen = 0;
            const char *s = vm_bytes_view(args[0], sbuf, sizeof(sbuf), &slen);
            int64_t ms = vm_to_int(args[1]), me = vm_to_int(args[2]);
            int64_t np = me;
            if (me == ms) {
                if ((size_t)me >= slen) np = (int64_t)slen + 1;
                else {
                    np = me + 1;
                    if (vm_is_utf8(args[0])) {
                        while ((size_t)np < slen && (((unsigned char)s[np]) & 0xC0) == 0x80) np++;
                    }
                }
            }
            SP_PUSH(VM_MAKE_INT(np));
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
            if (argc >= 1) {
                vm->stack_top = sp;
                VMValue res = vm_clone_val(vm, args[0]);
                stack = vm->stack; RELOAD();
                if (__builtin_expect(vm->has_pending_exc, 0)) {
                    throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                    goto L_builtin_throw_exc;
                }
                SP_PUSH(res);
            } else {
                SP_PUSH(VM_UNDEF_VAL);
            }
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
                            vm->stack_top = sp; vm->gc_pause++;
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
                                { *out_throw = throw_exc; vm->stack_top = sp; return 1; }
                            }
                        } else {
                            vm_globals_set(vm, sr->global_name, vm_val_copy(newv));
                        }
                    } else if (VM_IS_PTR(sr->cell) && VM_PTR_TYPE(sr->cell) == VM_OBJ_CELL) {
                        VMValue old_cv = VM_AS_CELL(sr->cell)->val;
                        if (__builtin_expect(VM_IS_PTR(old_cv) && VM_PTR_TYPE(old_cv) == VM_OBJ_TIED_SCALAR, 0)) {
                            vm->stack_top = sp; vm->gc_pause++;
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
                                { *out_throw = throw_exc; vm->stack_top = sp; return 1; }
                            }
                        } else {
                            vm_temp_free(&old_cv);
                            VM_AS_CELL(sr->cell)->val = vm_val_copy(newv);
                        }
                    }
                } else if (VM_IS_PTR(refv) && VM_PTR_TYPE(refv) == VM_OBJ_CELL) {
                    VMValue old_cv = VM_AS_CELL(refv)->val;
                    if (__builtin_expect(VM_IS_PTR(old_cv) && VM_PTR_TYPE(old_cv) == VM_OBJ_TIED_SCALAR, 0)) {
                        vm->stack_top = sp; vm->gc_pause++;
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
                            { *out_throw = throw_exc; vm->stack_top = sp; return 1; }
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
                        { *out_throw = throw_exc; vm->stack_top = sp; return 1; }
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
                        vm->stack_top = sp; vm->gc_pause++;
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
                            { *out_throw = throw_exc; vm->stack_top = sp; return 1; }
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

        case BUILTIN_DUMP:
        case BUILTIN_DUMP_R: {
            if (argc >= 1) {
                StradaValue *sv = vm_to_sv(args[0]);
                if (bid == BUILTIN_DUMP_R) sv = vm_sv_container_as_ref(sv);
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
                            vm_array_ensure_cap(a, (int)cap);
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

        case BUILTIN_STACKTRACE_STR: {
            /* stacktrace_str(): "  NAME" per frame, innermost first (compiled) */
            char *trace = vm_build_trace(vm, (int)vm->frame_count - 1, 1);
            if (trace) { SP_PUSH(vm_str(trace)); free(trace); }
            else SP_PUSH(vm_str(""));
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
            vm->stack_top = sp;
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
                    for (int i = 0; i < vm->regex_hw && i < VM_RX_MAX; i++) {
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
        vm->stack_top = sp; return 0;
    L_builtin_throw_exc:
        for (int _ai = 0; _ai < argc; _ai++) {
            if (VM_IS_PTR(args[_ai]) && (VM_PTR_TYPE(args[_ai]) == VM_OBJ_STR || VM_PTR_TYPE(args[_ai]) == VM_OBJ_STRBUF))
                vm_val_free(&args[_ai]);
        }
        if (args != args_stk) free(args);
        { *out_throw = throw_exc; vm->stack_top = sp; return 1; }
    #undef RT_CALL_0
    #undef RT_CALL_1
    #undef RT_CALL_2
    #undef RT_CALL_3
    #undef RT_VOID_1
    #undef RT_VOID_1_SV
    #undef RELOAD
    #undef SP_POP
    #undef SP_PUSH
}

static inline uint16_t vm_dt_map_jump(uint16_t byte_off, size_t code_len, const uint32_t *b2i) {
    if ((size_t)byte_off >= code_len) return 0xFFFF;
    uint32_t idx = b2i[byte_off];
    return (idx == UINT32_MAX || idx > 0xFFFE) ? 0xFFFF : (uint16_t)idx;
}

static void vm_chunk_build_dt_locked(VMChunk *c, const void * const *dt) {
    free(c->dt_code);
    c->dt_code = NULL;
    free(c->dt_pc);
    c->dt_pc = NULL;
    free(c->dt_aux);
    c->dt_aux = NULL;
    c->dt_aux_len = 0;

    size_t clen = c->code_len;
    if (clen == 0 || !c->code) {
        VMInsn *arr = calloc(1, sizeof(VMInsn));
        arr[0].op = OP_HALT;
        arr[0].handler = dt ? dt[OP_HALT] : NULL;
        c->dt_len = 0;
        c->dt_code_len_src = clen;
        __atomic_store_n(&c->dt_code, arr, __ATOMIC_RELEASE);
        return;
    }

    uint32_t *b2i = malloc((clen + 1) * sizeof(uint32_t));
    if (!b2i) { fprintf(stderr, "VM: OOM building threaded code\n"); exit(1); }
    for (size_t i = 0; i <= clen; i++) b2i[i] = UINT32_MAX;

    size_t cap = (clen / 2) + 8;
    VMInsn *arr = calloc(cap + 1, sizeof(VMInsn));
    if (!arr) { free(b2i); fprintf(stderr, "VM: OOM building threaded code\n"); exit(1); }
    size_t cnt = 0;

    uint16_t *aux = NULL;
    size_t aux_len = 0, aux_cap = 0;

    size_t pos = 0;
    uint8_t *code = c->code;
    while (pos < clen) {
        if (cnt >= cap) {
            size_t ncap = cap * 2;
            VMInsn *narr = realloc(arr, (ncap + 1) * sizeof(VMInsn));
            if (!narr) { free(arr); free(aux); free(b2i); fprintf(stderr, "VM: OOM building threaded code\n"); exit(1); }
            memset(narr + cap + 1, 0, (ncap - cap) * sizeof(VMInsn));
            arr = narr;
            cap = ncap;
        }
        b2i[pos] = (uint32_t)cnt;
        uint8_t op = code[pos++];
        VMInsn *ins = &arr[cnt++];
        memset(ins, 0, sizeof(*ins));
        ins->op = op;
        ins->handler = dt ? dt[op] : NULL;
        uint8_t *p = code + pos;
        switch (op) {
        case OP_PUSH_INT:
            if (pos + 8 <= clen) { ins->imm.i64 = read_i64(p); pos += 8; }
            break;
        case OP_PUSH_I8:
            if (pos + 1 <= clen) { ins->imm.i64 = (int64_t)read_i8(p); pos += 1; }
            break;
        case OP_PUSH_I32:
            if (pos + 4 <= clen) { ins->imm.i64 = (int64_t)read_i32(p); pos += 4; }
            break;
        case OP_PUSH_FLOAT:
            if (pos + 8 <= clen) { ins->imm.f64 = read_f64(p); pos += 8; }
            break;
        case OP_NEW_ARRAY:
        case OP_NEW_HASH:
            if (pos + 4 <= clen) { ins->imm.u32 = read_u32(p); pos += 4; }
            break;
        case OP_SET_CALL_CTX:
        case OP_CALL_CLOSURE:
        case OP_LOAD_CAPTURE_VAR:
        case OP_SPRINTF:
        case OP_DYN_METHOD_CALL:
            if (pos + 1 <= clen) { ins->flags = code[pos]; pos += 1; }
            break;
        case OP_PUSH_STR:
        case OP_LOAD_LOCAL:
        case OP_RESET_LOCAL:
        case OP_STORE_LOCAL:
        case OP_INCR:
        case OP_DECR:
        case OP_POST_INCR:
        case OP_POST_DECR:
        case OP_APPEND_LOCAL:
        case OP_HASH_GET_CONCAT:
        case OP_HASH_SET_CONCAT:
        case OP_HASH_DEL_CONCAT:
        case OP_HASH_GET_CONST:
        case OP_HASH_SET_CONST:
        case OP_LOAD_CAPTURE:
        case OP_STORE_CAPTURE:
        case OP_LOAD_CAPTURE_CELL:
        case OP_REGEX_FIND:
        case OP_LOAD_GLOBAL:
        case OP_STORE_GLOBAL:
        case OP_INIT_GLOBAL:
        case OP_SAVE_GLOBAL:
        case OP_RESTORE_GLOBAL:
        case OP_ARRAY_SORT:
        case OP_CALL_SPREAD:
        case OP_METHOD_CALL_SPREAD:
        case OP_C_BLOCK:
        case OP_MAKE_SREF_LOCAL:
        case OP_MAKE_SREF_GLOBAL:
        case OP_ARG_PRESENT:
        case OP_JMP:
        case OP_JMP_IF_FALSE:
        case OP_JMP_IF_TRUE:
        case OP_TRY_BEGIN:
        case OP_LOOP_BREAK:
        case OP_LOOP_NEXT:
        case OP_LOOP_REDO:
            if (pos + 2 <= clen) { ins->a = read_u16(p); pos += 2; }
            break;
        case OP_CALL:
        case OP_TAIL_CALL:
        case OP_BUILTIN:
        case OP_CALL_NATIVE:
        case OP_METHOD_CALL:
        case OP_FIELD_ACCESS:
        case OP_REGEX_MATCH:
        case OP_REGEX_NOT_MATCH:
        case OP_ADD_TO_LOCAL_VAL:
        case OP_SUB_TO_LOCAL_VAL:
            if (pos + 3 <= clen) { ins->a = read_u16(p); ins->flags = code[pos + 2]; pos += 3; }
            break;
        case OP_SQ_COERCE:
        case OP_WEAKEN_SLOT:
        case OP_ISWEAK_SLOT:
            if (pos + 3 <= clen) { ins->flags = code[pos]; ins->a = read_u16(p + 1); pos += 3; }
            break;
        case OP_CLEAR_LOCALS:
        case OP_APPEND_CONST:
        case OP_ADD_LOCALS:
        case OP_SUB_LOCALS:
        case OP_MUL_LOCALS:
            if (pos + 4 <= clen) { ins->a = read_u16(p); ins->b = read_u16(p + 2); pos += 4; }
            break;
        case OP_ADD_TO_LOCAL:
        case OP_SUPER_CALL:
            if (pos + 5 <= clen) { ins->a = read_u16(p); ins->b = read_u16(p + 2); ins->flags = code[pos + 4]; pos += 5; }
            break;
        case OP_SUB_LOCAL_I32:
        case OP_ADD_LOCAL_I32:
        case OP_BIT_AND_LOCAL_I32:
        case OP_MOD_LOCAL_I32:
            if (pos + 6 <= clen) { ins->a = read_u16(p); ins->imm.i32.i0 = read_i32(p + 2); pos += 6; }
            break;
        case OP_BIT_AND_ADD_I32:
            if (pos + 10 <= clen) { ins->a = read_u16(p); ins->imm.i32.i0 = read_i32(p + 2); ins->imm.i32.i1 = read_i32(p + 6); pos += 10; }
            break;
        case OP_LT_LOCAL_I32_JMP_F:
        case OP_LE_LOCAL_I32_JMP_F:
        case OP_INCR_LT_I32_JMP_T:
        case OP_INCR_LE_I32_JMP_T:
            if (pos + 8 <= clen) { ins->a = read_u16(p); ins->imm.i32.i0 = read_i32(p + 2); ins->b = read_u16(p + 6); pos += 8; }
            break;
        case OP_JMP_IF_INT_LOCAL:
            if (pos + 4 <= clen) { ins->a = read_u16(p); ins->b = read_u16(p + 2); pos += 4; }
            break;
        case OP_LT_LOCAL_LOCAL_JMP_F:
        case OP_INCR_LT_LOCAL_JMP_T:
            if (pos + 6 <= clen) { ins->a = read_u16(p); ins->b = read_u16(p + 2); ins->c = read_u16(p + 4); pos += 6; }
            break;
        case OP_TR:
            if (pos + 9 <= clen) {
                ins->a = read_u16(p);
                ins->b = read_u16(p + 2);
                ins->c = read_u16(p + 4);
                ins->flags = code[pos + 6];
                ins->imm.u32 = read_u16(p + 7);
                pos += 9;
            }
            break;
        case OP_MAKE_CLOSURE:
            if (pos + 3 <= clen) {
                ins->a = read_u16(p);
                uint8_t cc = code[pos + 2];
                ins->flags = cc;
                pos += 3;
                ins->imm.u32 = (uint32_t)aux_len;
                if (cc > 0) {
                    if (aux_len + cc > aux_cap) {
                        size_t ncap = aux_cap ? aux_cap * 2 : 32;
                        while (ncap < aux_len + cc) ncap *= 2;
                        aux = realloc(aux, ncap * sizeof(uint16_t));
                        aux_cap = ncap;
                    }
                    for (uint8_t ci = 0; ci < cc; ci++) {
                        uint16_t os = (pos + 2 <= clen) ? read_u16(code + pos) : 0xFFFF;
                        if (pos + 2 <= clen) pos += 2;
                        aux[aux_len++] = os;
                    }
                }
            }
            break;
        default:
            break;
        }
    }

    for (size_t i = 0; i < cnt; i++) {
        VMInsn *ins = &arr[i];
        switch (ins->op) {
        case OP_JMP:
        case OP_JMP_IF_FALSE:
        case OP_JMP_IF_TRUE:
        case OP_TRY_BEGIN:
        case OP_LOOP_BREAK:
        case OP_LOOP_NEXT:
        case OP_LOOP_REDO:
            ins->a = vm_dt_map_jump(ins->a, clen, b2i);
            break;
        case OP_LT_LOCAL_I32_JMP_F:
        case OP_LE_LOCAL_I32_JMP_F:
        case OP_INCR_LT_I32_JMP_T:
        case OP_INCR_LE_I32_JMP_T:
        case OP_JMP_IF_INT_LOCAL:
            ins->b = vm_dt_map_jump(ins->b, clen, b2i);
            break;
        case OP_LT_LOCAL_LOCAL_JMP_F:
        case OP_INCR_LT_LOCAL_JMP_T:
            ins->c = vm_dt_map_jump(ins->c, clen, b2i);
            break;
        default:
            break;
        }
    }

    arr[cnt].op = OP_HALT;
    arr[cnt].handler = dt ? dt[OP_HALT] : NULL;
    {
        /* insn index -> bytecode offset, for source-line lookups */
        uint32_t *i2b = calloc(cnt + 1, sizeof(uint32_t));
        if (i2b) {
            for (size_t bp = 0; bp < clen; bp++)
                if (b2i[bp] != UINT32_MAX && b2i[bp] <= cnt) i2b[b2i[bp]] = (uint32_t)bp;
            i2b[cnt] = (uint32_t)clen;
        }
        c->dt_pc = i2b;
    }
    free(b2i);

    c->dt_aux = aux;
    c->dt_aux_len = aux_len;
    c->dt_len = cnt;
    c->dt_code_len_src = clen;
    __atomic_store_n(&c->dt_code, arr, __ATOMIC_RELEASE);
}

static __attribute__((noinline)) void vm_chunk_ensure_dt(VMProgram *prog, VMChunk *c, const void * const *dt) {
    if (prog) pthread_mutex_lock(&prog->class_mutex);
    if (!c->dt_code || c->dt_code_len_src != c->code_len) {
        vm_chunk_build_dt_locked(c, dt);
    }
    if (prog) pthread_mutex_unlock(&prog->class_mutex);
}

static VMValue vm_execute_call_cl(VM *vm, int func_idx, VMClosure *closure, VMValue *cargs, int cargc, int passed_argc);

#ifdef VM_JIT_X64
/* ===== Baseline x86_64 Native JIT (strict W^X: mmap RW -> mprotect RX) ===== */
enum {
    VM_JIT_OK = 0,
    VM_JIT_INTERP = 1,
    VM_JIT_EXC = 2
};

typedef int (*VMJitTrampolineFn)(VM *vm, void *entry, int is_entry0);

#define JIT_HOT_THRESHOLD 8

typedef struct {
    uint32_t patch_off;
    uint16_t target_idx;
} JitPatch;

typedef struct {
    uint8_t *buf;
    size_t len;
    size_t cap;
    uint32_t *offs;
    JitPatch *patches;
    size_t patch_count;
    size_t patch_cap;
    int oom;
} JitBuf;

enum {
    JR_RAX = 0, JR_RCX = 1, JR_RDX = 2, JR_RBX = 3,
    JR_RSP = 4, JR_RBP = 5, JR_RSI = 6, JR_RDI = 7,
    JR_R8  = 8, JR_R9  = 9, JR_R10 = 10, JR_R11 = 11,
    JR_R12 = 12, JR_R13 = 13, JR_R14 = 14, JR_R15 = 15
};

static inline void jb_reserve(JitBuf *b, size_t n) {
    if (b->oom) return;
    if (b->len + n <= b->cap) return;
    size_t ncap = b->cap ? b->cap * 2 : 1024;
    while (ncap < b->len + n) ncap *= 2;
    uint8_t *nb = realloc(b->buf, ncap);
    if (!nb) { b->oom = 1; return; }
    b->buf = nb;
    b->cap = ncap;
}

static inline void jb_u8(JitBuf *b, uint8_t v) {
    jb_reserve(b, 1);
    if (!b->oom) b->buf[b->len++] = v;
}

static inline void jb_u16(JitBuf *b, uint16_t v) {
    jb_reserve(b, 2);
    if (!b->oom) { memcpy(b->buf + b->len, &v, 2); b->len += 2; }
}

static inline void jb_u32(JitBuf *b, uint32_t v) {
    jb_reserve(b, 4);
    if (!b->oom) { memcpy(b->buf + b->len, &v, 4); b->len += 4; }
}

static inline void jb_u64(JitBuf *b, uint64_t v) {
    jb_reserve(b, 8);
    if (!b->oom) { memcpy(b->buf + b->len, &v, 8); b->len += 8; }
}

static inline void jb_rex(JitBuf *b, int w, int reg, int idx, int rm) {
    uint8_t rex = 0x40 | (w ? 8 : 0) | ((reg >= 8) ? 4 : 0) | ((idx >= 8) ? 2 : 0) | ((rm >= 8) ? 1 : 0);
    if (rex != 0x40) jb_u8(b, rex);
}

static inline void jb_rr(JitBuf *b, int w, uint8_t op, int reg, int rm) {
    jb_rex(b, w, reg, 0, rm);
    jb_u8(b, op);
    jb_u8(b, (uint8_t)(0xC0 | ((reg & 7) << 3) | (rm & 7)));
}

static inline void jb_mem(JitBuf *b, int w, uint8_t op, int reg, int base, int32_t disp) {
    int mod = (disp == 0 && (base & 7) != 5) ? 0 : (disp >= -128 && disp <= 127) ? 1 : 2;
    jb_rex(b, w, reg, 0, base);
    jb_u8(b, op);
    jb_u8(b, (uint8_t)((mod << 6) | ((reg & 7) << 3) | (base & 7)));
    if ((base & 7) == 4) jb_u8(b, 0x24);
    if (mod == 1) jb_u8(b, (uint8_t)(int8_t)disp);
    else if (mod == 2) jb_u32(b, (uint32_t)disp);
}

static inline void jb_sib8(JitBuf *b, int w, uint8_t op, int reg, int base, int idx, int32_t disp) {
    int mod = (disp == 0 && (base & 7) != 5) ? 0 : (disp >= -128 && disp <= 127) ? 1 : 2;
    jb_rex(b, w, reg, idx, base);
    jb_u8(b, op);
    jb_u8(b, (uint8_t)((mod << 6) | ((reg & 7) << 3) | 4));
    jb_u8(b, (uint8_t)((3 << 6) | ((idx & 7) << 3) | (base & 7)));
    if (mod == 1) jb_u8(b, (uint8_t)(int8_t)disp);
    else if (mod == 2) jb_u32(b, (uint32_t)disp);
}

static inline void jb_push(JitBuf *b, int r) {
    if (r >= 8) jb_u8(b, 0x41);
    jb_u8(b, (uint8_t)(0x50 + (r & 7)));
}

static inline void jb_pop(JitBuf *b, int r) {
    if (r >= 8) jb_u8(b, 0x41);
    jb_u8(b, (uint8_t)(0x58 + (r & 7)));
}

static inline void jb_mov_rr(JitBuf *b, int dst, int src) {
    if (dst != src) jb_rr(b, 1, 0x89, src, dst);
}

static inline void jb_mov_imm64(JitBuf *b, int r, uint64_t imm) {
    jb_rex(b, 1, 0, 0, r);
    jb_u8(b, (uint8_t)(0xB8 + (r & 7)));
    jb_u64(b, imm);
}

static inline void jb_mov_imm32(JitBuf *b, int r, uint32_t imm) {
    if (r >= 8) jb_u8(b, 0x41);
    jb_u8(b, (uint8_t)(0xB8 + (r & 7)));
    jb_u32(b, imm);
}

static inline void jb_alu_ri32(JitBuf *b, int subop, int r, int32_t imm) {
    jb_rex(b, 1, 0, 0, r);
    if (imm >= -128 && imm <= 127) {
        jb_u8(b, 0x83);
        jb_u8(b, (uint8_t)(0xC0 | (subop << 3) | (r & 7)));
        jb_u8(b, (uint8_t)(int8_t)imm);
    } else {
        jb_u8(b, 0x81);
        jb_u8(b, (uint8_t)(0xC0 | (subop << 3) | (r & 7)));
        jb_u32(b, (uint32_t)imm);
    }
}

static inline void jb_shift_ri8(JitBuf *b, int w, int subop, int r, uint8_t imm8) {
    jb_rex(b, w, 0, 0, r);
    jb_u8(b, 0xC1);
    jb_u8(b, (uint8_t)(0xC0 | (subop << 3) | (r & 7)));
    jb_u8(b, imm8);
}

static inline uint32_t jb_jmp_placeholder(JitBuf *b) {
    jb_u8(b, 0xE9);
    uint32_t off = (uint32_t)b->len;
    jb_u32(b, 0);
    return off;
}

static inline uint32_t jb_jcc_placeholder(JitBuf *b, uint8_t cc) {
    jb_u8(b, 0x0F);
    jb_u8(b, cc);
    uint32_t off = (uint32_t)b->len;
    jb_u32(b, 0);
    return off;
}

static inline void jb_patch_here(JitBuf *b, uint32_t patch_off) {
    if (b->oom || (size_t)patch_off + 4 > b->len) return;
    int32_t rel = (int32_t)b->len - (int32_t)(patch_off + 4);
    memcpy(b->buf + patch_off, &rel, 4);
}

static inline void jb_add_target_patch(JitBuf *b, uint32_t patch_off, uint16_t target_idx) {
    if (b->oom) return;
    if (b->patch_count >= b->patch_cap) {
        size_t ncap = b->patch_cap ? b->patch_cap * 2 : 32;
        JitPatch *np = realloc(b->patches, ncap * sizeof(JitPatch));
        if (!np) { b->oom = 1; return; }
        b->patches = np;
        b->patch_cap = ncap;
    }
    b->patches[b->patch_count].patch_off = patch_off;
    b->patches[b->patch_count].target_idx = target_idx;
    b->patch_count++;
}

static inline void jb_call_c(JitBuf *b, void *fn) {
    jb_mov_imm64(b, JR_RAX, (uint64_t)(uintptr_t)fn);
    jb_u8(b, 0xFF); jb_u8(b, 0xD0); /* call rax */
}

/* Reload r12 (frame), r14 (stack), r15 (stack_top), r13 (locals = stack + local_base) from rbx (vm) */
static void jb_reload_all(JitBuf *b) {
    jb_mem(b, 1, 0x8B, JR_R14, JR_RBX, (int32_t)offsetof(VM, stack));
    jb_mem(b, 1, 0x8B, JR_R15, JR_RBX, (int32_t)offsetof(VM, stack_top));
    jb_mem(b, 1, 0x8B, JR_RCX, JR_RBX, (int32_t)offsetof(VM, frame_count));
    jb_rr(b, 1, 0x85, JR_RCX, JR_RCX); /* test rcx, rcx */
    uint32_t j_no_frame = jb_jcc_placeholder(b, 0x84);
    jb_mem(b, 1, 0x8B, JR_R12, JR_RBX, (int32_t)offsetof(VM, frames));
    jb_alu_ri32(b, 5, JR_RCX, 1); /* dec rcx */
    jb_rr(b, 1, 0x69, JR_RCX, JR_RCX); jb_u32(b, (uint32_t)sizeof(VMFrame));
    jb_rr(b, 1, 0x01, JR_RCX, JR_R12); /* add r12, rcx */
    jb_mem(b, 1, 0x8B, JR_RAX, JR_R12, (int32_t)offsetof(VMFrame, local_base));
    jb_sib8(b, 1, 0x8D, JR_R13, JR_R14, JR_RAX, 0); /* lea r13, [r14 + rax*8] */
    jb_patch_here(b, j_no_frame);
}

/* Sync frame->ip = next_di and vm->stack_top = r15 */
static void jb_sync_state(JitBuf *b, const VMInsn *next_di) {
    jb_mov_imm64(b, JR_RAX, (uint64_t)(uintptr_t)next_di);
    jb_mem(b, 1, 0x89, JR_RAX, JR_R12, (int32_t)offsetof(VMFrame, ip));
    jb_mem(b, 1, 0x89, JR_R15, JR_RBX, (int32_t)offsetof(VM, stack_top));
}

/* Emit VM_IS_INT(reg): fails (jumps to rel32) if reg < VM_TAG_INT_BASE */
static uint32_t jb_j_not_int(JitBuf *b, int reg) {
    jb_mov_rr(b, JR_RCX, reg);
    jb_shift_ri8(b, 1, 7, JR_RCX, 50); /* sar rcx, 50 */
    jb_alu_ri32(b, 7, JR_RCX, -1);     /* cmp rcx, -1 */
    return jb_jcc_placeholder(b, 0x85); /* jne slow */
}

/* Emit VM_BOTH_INT(rA, rB): jumps if !(both are tagged int) */
static uint32_t jb_j_not_both_int(JitBuf *b, int rA, int rB) {
    jb_mov_rr(b, JR_RCX, rA);
    jb_rr(b, 1, 0x21, rB, JR_RCX);     /* and rcx, rB */
    jb_shift_ri8(b, 1, 7, JR_RCX, 50); /* sar rcx, 50 */
    jb_alu_ri32(b, 7, JR_RCX, -1);     /* cmp rcx, -1 */
    return jb_jcc_placeholder(b, 0x85); /* jne slow */
}

/* Ensure [r15] < vm->stack_cap, else call vm_jit_stack_grow(vm, r15) and reload r14/r13 */
static void vm_jit_stack_grow(VM *vm, size_t need) {
    vm_stack_grow(vm, need);
}

static void jb_ensure_stack_slot(JitBuf *b) {
    jb_mem(b, 1, 0x3B, JR_R15, JR_RBX, (int32_t)offsetof(VM, stack_cap)); /* cmp r15, [rbx+stack_cap] */
    uint32_t j_ok = jb_jcc_placeholder(b, 0x82); /* jb ok */
    jb_mov_rr(b, JR_RDI, JR_RBX);
    jb_mov_rr(b, JR_RSI, JR_R15);
    jb_call_c(b, (void *)vm_jit_stack_grow);
    jb_mem(b, 1, 0x8B, JR_R14, JR_RBX, (int32_t)offsetof(VM, stack));
    jb_mem(b, 1, 0x8B, JR_RAX, JR_R12, (int32_t)offsetof(VMFrame, local_base));
    jb_sib8(b, 1, 0x8D, JR_R13, JR_R14, JR_RAX, 0);
    jb_patch_here(b, j_ok);
}

static void jb_sp_push_reg(JitBuf *b, int reg) {
    jb_push(b, reg);
    jb_ensure_stack_slot(b);
    jb_pop(b, reg);
    jb_sib8(b, 1, 0x89, reg, JR_R14, JR_R15, 0); /* mov [r14 + r15*8], reg */
    jb_alu_ri32(b, 0, JR_R15, 1); /* inc r15 */
}

static void jb_sp_push_rax(JitBuf *b) {
    jb_mem(b, 1, 0x3B, JR_R15, JR_RBX, (int32_t)offsetof(VM, stack_cap));
    uint32_t j_ok = jb_jcc_placeholder(b, 0x82);
    jb_push(b, JR_RAX);
    jb_mov_rr(b, JR_RDI, JR_RBX);
    jb_mov_rr(b, JR_RSI, JR_R15);
    jb_call_c(b, (void *)vm_jit_stack_grow);
    jb_mem(b, 1, 0x8B, JR_R14, JR_RBX, (int32_t)offsetof(VM, stack));
    jb_mem(b, 1, 0x8B, JR_RCX, JR_R12, (int32_t)offsetof(VMFrame, local_base));
    jb_sib8(b, 1, 0x8D, JR_R13, JR_R14, JR_RCX, 0);
    jb_pop(b, JR_RAX);
    jb_patch_here(b, j_ok);
    jb_sib8(b, 1, 0x89, JR_RAX, JR_R14, JR_R15, 0);
    jb_alu_ri32(b, 0, JR_R15, 1);
}

/* Shared JIT trampoline (one 2-page RX mapping, cleaned on process exit) */
static void *g_jit_trampoline = NULL;
static size_t g_jit_trampoline_size = 0;
static pthread_mutex_t g_jit_tramp_mutex = PTHREAD_MUTEX_INITIALIZER;

static void __attribute__((destructor)) vm_jit_trampoline_cleanup(void) {
    if (g_jit_trampoline && g_jit_trampoline_size > 0) {
        munmap(g_jit_trampoline, g_jit_trampoline_size);
        g_jit_trampoline = NULL;
        g_jit_trampoline_size = 0;
    }
}

static VMJitTrampolineFn vm_jit_get_trampoline(void) {
    void *t = __atomic_load_n(&g_jit_trampoline, __ATOMIC_ACQUIRE);
    if (__builtin_expect(t != NULL, 1)) return (VMJitTrampolineFn)t;
    pthread_mutex_lock(&g_jit_tramp_mutex);
    if (!g_jit_trampoline) {
        JitBuf jb = {0};
        JitBuf *b = &jb;
        /* rdi = vm, rsi = entry, edx = is_entry0 */
        jb_push(b, JR_RBX);
        jb_push(b, JR_RBP);
        jb_push(b, JR_R12);
        jb_push(b, JR_R13);
        jb_push(b, JR_R14);
        jb_push(b, JR_R15);
        jb_alu_ri32(b, 5, JR_RSP, 8); /* 16-byte stack alignment */

        jb_mov_rr(b, JR_RBX, JR_RDI);
        jb_reload_all(b);

        /* test edx, edx; jz L_osr */
        jb_rr(b, 0, 0x85, JR_RDX, JR_RDX);
        uint32_t j_osr = jb_jcc_placeholder(b, 0x84);
        /* call rsi */
        jb_u8(b, 0xFF); jb_u8(b, 0xD6);
        uint32_t j_done = jb_jmp_placeholder(b);

        jb_patch_here(b, j_osr);
        /* call L_osr_stub */
        jb_u8(b, 0xE8);
        uint32_t p_stub = (uint32_t)b->len;
        jb_u32(b, 0);
        uint32_t j_done2 = jb_jmp_placeholder(b);

        /* L_osr_stub: push rbp; jmp rsi (matches target's `pop rbp; ret`) */
        jb_patch_here(b, p_stub);
        jb_push(b, JR_RBP);
        jb_u8(b, 0xFF); jb_u8(b, 0xE6);

        jb_patch_here(b, j_done);
        jb_patch_here(b, j_done2);
        jb_alu_ri32(b, 0, JR_RSP, 8);
        jb_pop(b, JR_R15);
        jb_pop(b, JR_R14);
        jb_pop(b, JR_R13);
        jb_pop(b, JR_R12);
        jb_pop(b, JR_RBP);
        jb_pop(b, JR_RBX);
        jb_u8(b, 0xC3);

        size_t alloc_sz = (b->len + 4095) & ~(size_t)4095;
        void *mem = mmap(NULL, alloc_sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem != MAP_FAILED) {
            memcpy(mem, b->buf, b->len);
            if (mprotect(mem, alloc_sz, PROT_READ | PROT_EXEC) == 0) {
                g_jit_trampoline_size = alloc_sz;
                __atomic_store_n(&g_jit_trampoline, mem, __ATOMIC_RELEASE);
            } else {
                munmap(mem, alloc_sz);
            }
        }
        free(b->buf);
    }
    pthread_mutex_unlock(&g_jit_tramp_mutex);
    return (VMJitTrampolineFn)g_jit_trampoline;
}

/* Forward decl for JIT C-helper compiler trigger */
static void vm_jit_try_compile(VMProgram *prog, VMChunk *c);

/* ---- JIT runtime slow/hybrid C helpers (all return 0=ok, 2=pending_exc) ---- */

static __attribute__((noinline)) int vm_jit_poll_signals(VM *vm) {
    if (strada_has_pending_signals()) {
        VMValue exc = VM_UNDEF_VAL;
        if (vm_c_dispatch_signals(vm, &exc)) {
            vm->pending_exc = exc;
            vm->has_pending_exc = 1;
            return VM_JIT_EXC;
        }
    }
    return VM_JIT_OK;
}

static __attribute__((noinline)) int vm_jit_poll_sig_and_gc(VM *vm) {
    if (__builtin_expect(vm->is_worker_vm, 0)) {
        vm_worker_poll_gc(vm);
        return vm_jit_poll_signals(vm);
    }
    if (vm->gc_count >= vm->gc_threshold && vm->exec_base == 0 && vm->gc_pause == 0) {
        vm_gc_collect(vm);
        if (vm->has_pending_exc) return VM_JIT_EXC;
    }
    return vm_jit_poll_signals(vm);
}

static __attribute__((noinline)) int vm_jit_load_local_slow(VM *vm, const VMInsn *di) {
    VMFrame *f = &vm->frames[vm->frame_count - 1];
    VMValue v = vm->stack[f->local_base + di->a];
    if (VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_STRBUF) {
        if ((di + 1)->op != OP_STR_LEN) VM_AS_STRBUF(v)->shared = 1;
        if (vm->stack_top >= vm->stack_cap) vm_stack_grow(vm, vm->stack_top);
        vm->stack[vm->stack_top++] = v;
        return VM_JIT_OK;
    }
    VMValue r = vm_slow_load_local_ptr(vm, v);
    if (vm->has_pending_exc) return VM_JIT_EXC;
    if (vm->stack_top >= vm->stack_cap) vm_stack_grow(vm, vm->stack_top);
    vm->stack[vm->stack_top++] = r;
    return VM_JIT_OK;
}

static __attribute__((noinline)) int vm_jit_store_local_slow(VM *vm, const VMInsn *di) {
    VMFrame *f = &vm->frames[vm->frame_count - 1];
    VMValue newv = vm->stack[--vm->stack_top];
    VMValue old = vm->stack[f->local_base + di->a];
    if ((!VM_IS_PTR(old) || (VM_PTR_TYPE(old) != VM_OBJ_CELL && VM_PTR_TYPE(old) != VM_OBJ_TIED_SCALAR)) &&
        (!vm->has_weakrefs || !VM_IS_PTR(newv))) {
        vm->stack[f->local_base + di->a] = newv;
        return VM_JIT_OK;
    }
    vm_slow_store_local(vm, di->a, old, newv);
    return vm->has_pending_exc ? VM_JIT_EXC : VM_JIT_OK;
}

static __attribute__((noinline)) int vm_jit_cast_slow(VM *vm, const VMInsn *di) {
    VMValue v = vm->stack[vm->stack_top - 1];
    if ((di->op == OP_CAST_ASSIGN_INT || di->op == OP_CAST_ASSIGN_NUM) &&
        vm_obj_class_name(v) != NULL) {
        return VM_JIT_OK;
    }
    if (di->op == OP_CAST_INT || di->op == OP_CAST_ASSIGN_INT) {
        if (!VM_IS_INT(v) && (!VM_IS_PTR(v) || VM_PTR_TYPE(v) != VM_OBJ_INT64)) {
            vm->gc_pause++;
            int64_t iv = vm_to_int_ov(vm, v, 0);
            vm->gc_pause--;
            if (vm->has_pending_exc) return VM_JIT_EXC;
            vm->stack[vm->stack_top - 1] = VM_MAKE_INT(iv);
        }
    } else {
        if (!VM_IS_FLOAT(v)) {
            vm->gc_pause++;
            double nv = vm_to_num_ov(vm, v, 0);
            vm->gc_pause--;
            if (vm->has_pending_exc) return VM_JIT_EXC;
            vm->stack[vm->stack_top - 1] = VM_MAKE_FLOAT(nv);
        }
    }
    return VM_JIT_OK;
}

static __attribute__((noinline)) int vm_jit_incr_decr_slow(VM *vm, const VMInsn *di) {
    VMFrame *f = &vm->frames[vm->frame_count - 1];
    VMValue *locals = vm->stack + f->local_base;
    uint16_t s = di->a;
    int is_dec = (di->op == OP_DECR || di->op == OP_POST_DECR);
    int is_post = (di->op == OP_POST_INCR || di->op == OP_POST_DECR);
    VMValue cur_v = locals[s];
    VMValue *islot = (VM_IS_PTR(cur_v) && VM_PTR_TYPE(cur_v) == VM_OBJ_CELL)
                     ? &VM_AS_CELL(cur_v)->val : &locals[s];
    if (VM_IS_PTR(*islot) && VM_PTR_TYPE(*islot) == VM_OBJ_TIED_SCALAR) {
        VMValue ts = *islot;
        vm->gc_pause++;
        VMValue cur = vm_tied_scalar_fetch(vm, ts);
        if (!vm->has_pending_exc) {
            VMValue nxt;
            if (VM_IS_ANY_INT(cur)) {
                nxt = VM_MAKE_INT((int64_t)((uint64_t)VM_ANY_INT_VAL(cur) + (is_dec ? -1ULL : 1ULL)));
            } else {
                double n = vm_to_num(cur) + (is_dec ? -1.0 : 1.0);
                int was_float = VM_IS_FLOAT(cur);
                nxt = (!was_float && isfinite(n) && n == (double)(int64_t)n && n >= -9.2e18 && n <= 9.2e18)
                      ? VM_MAKE_INT((int64_t)n) : VM_MAKE_FLOAT(n);
            }
            vm_tied_scalar_store(vm, ts, nxt);
        }
        vm->gc_pause--;
        if (vm->has_pending_exc) return VM_JIT_EXC;
        if (is_post) {
            if (vm->stack_top >= vm->stack_cap) vm_stack_grow(vm, vm->stack_top);
            vm->stack[vm->stack_top++] = cur;
        }
        return VM_JIT_OK;
    }
    VMValue old = *islot;
    if (VM_IS_ANY_INT(old)) {
        *islot = VM_MAKE_INT((int64_t)((uint64_t)VM_ANY_INT_VAL(old) + (is_dec ? -1ULL : 1ULL)));
    } else if (!is_dec && VM_IS_PTR(old) && (VM_PTR_TYPE(old) == VM_OBJ_STR || VM_PTR_TYPE(old) == VM_OBJ_STRBUF)) {
        /* magic string ++ (as the interpreter loop) */
        StradaValue *isv = vm_to_sv(old);
        StradaValue *inr = strada_inc_value(isv, 1);
        strada_decref(isv);
        *islot = inr ? sv_to_vm(inr) : VM_UNDEF_VAL;
        if (inr) strada_decref(inr);
        if (is_post) {
            if (vm->stack_top >= vm->stack_cap) vm_stack_grow(vm, vm->stack_top);
            vm->stack[vm->stack_top++] = old;   /* the old string moves to the stack */
        } else {
            vm_temp_free(&old);
        }
        return VM_JIT_OK;
    } else {
        double n = vm_to_num(old) + (is_dec ? -1.0 : 1.0);
        int was_float = VM_IS_FLOAT(old);
        if (!was_float && isfinite(n) && n == (double)(int64_t)n && n >= -9.2e18 && n <= 9.2e18)
            *islot = VM_MAKE_INT((int64_t)n);
        else
            *islot = VM_MAKE_FLOAT(n);
    }
    if (is_post) {
        if (vm->stack_top >= vm->stack_cap) vm_stack_grow(vm, vm->stack_top);
        vm->stack[vm->stack_top++] = old;
    }
    return VM_JIT_OK;
}

static __attribute__((noinline)) int vm_jit_is_truthy(VM *vm) {
    VMValue tv = vm->stack[--vm->stack_top];
    if (VM_IS_INT(tv)) return (tv != VM_TAG_INT_BASE);
    if (VM_IS_FLOAT(tv)) { double d = VM_FLOAT_VAL(tv); return (d != 0.0 && !isnan(d)); }
    if (VM_IS_UNDEF(tv)) return 0;
    enum VMObjType ot = VM_PTR_TYPE(tv);
    if (ot == VM_OBJ_TIED_SCALAR || ot == VM_OBJ_CELL || vm_obj_class_name(tv) != NULL ||
        (ot == VM_OBJ_HASH && !VM_IS_UNDEF(VM_AS_HASH(tv)->tied_obj)) ||
        (ot == VM_OBJ_ARRAY && !VM_IS_UNDEF(VM_AS_ARRAY(tv)->tied_obj))) {
        int r = vm_eval_bool_slow(vm, tv);
        return vm->has_pending_exc ? -1 : r;
    }
    return vm_to_bool(tv);
}

static __attribute__((noinline)) int vm_jit_slow_cmp_jmp(VM *vm, const VMInsn *di) {
    VMValue *locals = vm->stack + vm->frames[vm->frame_count - 1].local_base;
    int cond;
    if (di->op == OP_LT_LOCAL_LOCAL_JMP_F) {
        VMValue a = locals[di->a], b = locals[di->b];
        if (VM_BOTH_INT(a, b)) cond = (VM_INT_VAL(a) < VM_INT_VAL(b));
        else if (VM_IS_FLOAT(a) && VM_IS_FLOAT(b)) cond = (VM_FLOAT_VAL(a) < VM_FLOAT_VAL(b));
        else if (!VM_IS_PTR(a) && !VM_IS_PTR(b)) {
            double da = VM_IS_FLOAT(a) ? VM_FLOAT_VAL(a) : (VM_IS_INT(a) ? (double)VM_INT_VAL(a) : 0.0);
            double db = VM_IS_FLOAT(b) ? VM_FLOAT_VAL(b) : (VM_IS_INT(b) ? (double)VM_INT_VAL(b) : 0.0);
            cond = (da < db);
        } else {
            cond = vm_slow_cmp_vals_lt(vm, a, b);
        }
    } else {
        int is_le = (di->op == OP_LE_LOCAL_I32_JMP_F);
        int32_t imm = di->imm.i32.i0;
        VMValue v = locals[di->a];
        if (VM_IS_INT(v)) cond = is_le ? (VM_INT_VAL(v) <= (int64_t)imm) : (VM_INT_VAL(v) < (int64_t)imm);
        else if (VM_IS_FLOAT(v)) cond = is_le ? (VM_FLOAT_VAL(v) <= (double)imm) : (VM_FLOAT_VAL(v) < (double)imm);
        else if (VM_IS_UNDEF(v)) cond = is_le ? (0 <= imm) : (0 < imm);
        else cond = vm_slow_cmp_local_i32(vm, v, imm, is_le);
    }
    return vm->has_pending_exc ? -1 : cond;
}

static __attribute__((noinline)) int vm_jit_slow_incr_cmp_jmp(VM *vm, const VMInsn *di) {
    int kind = (di->op == OP_INCR_LT_I32_JMP_T) ? 0 : (di->op == OP_INCR_LE_I32_JMP_T) ? 1 : 2;
    int r = vm_slow_incr_cmp_jmp(vm, di->a, kind, di->imm.i32.i0, di->b);
    return vm->has_pending_exc ? -1 : r;
}

static __attribute__((noinline)) int vm_jit_op_step(VM *vm, const VMInsn *di) {
    VMFrame *frame = &vm->frames[vm->frame_count - 1];
    VMChunk *chunk = frame->chunk;
    VMValue *locals = vm->stack + frame->local_base;
    VMValue *stack = vm->stack;
    size_t sp = vm->stack_top;
    uint8_t op = di->op;

    #define J_PUSH(v) do { \
        if (__builtin_expect(sp >= vm->stack_cap, 0)) { vm_stack_grow(vm, sp); stack = vm->stack; } \
        stack[sp++] = (v); \
    } while (0)
    #define J_POP() (stack[--sp])

    /* Overloaded operands: these helpers implement only the plain-value
     * semantics, so a blessed operand in a program with overloads resumes
     * in the interpreter at this instruction (it dispatches ".", "!", "",
     * and ".=" through the overload). */
    if (__builtin_expect(vm->program && vm->program->overload_count > 0, 0)) {
        int bail = 0;
        switch (op) {
        case OP_NOT: case OP_STR_LEN: case OP_STR_OV: case OP_ARRAY_GET:
            bail = (sp >= 1 && vm_obj_class_name(stack[sp - 1]) != NULL);
            break;
        case OP_ARRAY_SET:
            bail = (sp >= 2 && vm_obj_class_name(stack[sp - 2]) != NULL);
            break;
        case OP_CONCAT:
            bail = (sp >= 2 && (vm_obj_class_name(stack[sp - 1]) != NULL ||
                                vm_obj_class_name(stack[sp - 2]) != NULL));
            break;
        case OP_APPEND_LOCAL: case OP_APPEND_CONST: {
            uint16_t slot = (op == OP_APPEND_LOCAL) ? di->a : di->b;
            VMValue lv = locals[slot];
            if (VM_IS_PTR(lv) && VM_PTR_TYPE(lv) == VM_OBJ_CELL) lv = VM_AS_CELL(lv)->val;
            bail = (vm_obj_class_name(lv) != NULL ||
                    (op == OP_APPEND_LOCAL && sp >= 1 && vm_obj_class_name(stack[sp - 1]) != NULL));
            break;
        }
        case OP_BUILTIN: {
            int n = di->flags;
            for (int i = 1; i <= n && (size_t)i <= sp && !bail; i++)
                if (vm_obj_class_name(stack[sp - i]) != NULL) bail = 1;
            break;
        }
        default: break;
        }
        if (bail) {
            frame->ip = (VMInsn *)di; vm->stack_top = sp;
            return VM_JIT_INTERP;
        }
    }

    switch (op) {
    case OP_STR_OV:
        break;   /* not an object (objects resumed in the interpreter above) */
    case OP_ADD:
    case OP_SUB:
    case OP_MUL: {
        int okind = (op == OP_ADD) ? 0 : (op == OP_SUB) ? 1 : 2;
        VMValue b = J_POP(), a = J_POP();
        VMValue r;
        if (VM_BOTH_INT(a, b)) {
            int64_t ia = VM_INT_VAL(a), ib = VM_INT_VAL(b);
            if (okind == 0) r = VM_MAKE_INT(ia + ib);
            else if (okind == 1) r = VM_MAKE_INT(ia - ib);
            else r = VM_MAKE_INT((int64_t)((uint64_t)ia * (uint64_t)ib));
        } else if (!VM_IS_PTR(a) && !VM_IS_PTR(b)) {
            if (VM_IS_FLOAT(a) || VM_IS_FLOAT(b)) {
                double da = VM_IS_FLOAT(a) ? VM_FLOAT_VAL(a) : (VM_IS_INT(a) ? (double)VM_INT_VAL(a) : 0.0);
                double db = VM_IS_FLOAT(b) ? VM_FLOAT_VAL(b) : (VM_IS_INT(b) ? (double)VM_INT_VAL(b) : 0.0);
                r = VM_MAKE_FLOAT(okind == 0 ? (da + db) : okind == 1 ? (da - db) : (da * db));
            } else {
                int64_t ia = VM_IS_INT(a) ? VM_INT_VAL(a) : 0;
                int64_t ib = VM_IS_INT(b) ? VM_INT_VAL(b) : 0;
                r = VM_MAKE_INT(okind == 0 ? (ia + ib) : okind == 1 ? (ia - ib) : (int64_t)((uint64_t)ia * (uint64_t)ib));
            }
        } else {
            vm->stack_top = sp;
            r = vm_slow_bin_arith(vm, okind, a, b);
            stack = vm->stack; sp = vm->stack_top;
            if (vm->has_pending_exc) return VM_JIT_EXC;
        }
        J_PUSH(r);
        break;
    }
    case OP_DIV: {
        VMValue b = J_POP(), a = J_POP();
        double da = VM_IS_FLOAT(a) ? VM_FLOAT_VAL(a) : (VM_IS_INT(a) ? (double)VM_INT_VAL(a) : 0.0);
        double db = VM_IS_FLOAT(b) ? VM_FLOAT_VAL(b) : (VM_IS_INT(b) ? (double)VM_INT_VAL(b) : 0.0);
        if (!VM_IS_PTR(a) && !VM_IS_PTR(b)) {
            if (db == 0.0) { vm->pending_exc = vm_str("Illegal division by zero"); vm->has_pending_exc = 1; vm->stack_top = sp; return VM_JIT_EXC; }
            J_PUSH(VM_MAKE_FLOAT(da / db));
        } else {
            if (vm_obj_class_name(a) || vm_obj_class_name(b)) {
                VMValue ov = VM_UNDEF_VAL; vm->stack_top = sp;
                if (vm_try_bin_overload(vm, "/", a, b, &ov)) {
                    stack = vm->stack; sp = vm->stack_top;
                    if (vm->has_pending_exc) return VM_JIT_EXC;
                    J_PUSH(ov); break;
                }
                stack = vm->stack; sp = vm->stack_top;
            }
            double na = 0.0, nb = 0.0;
            vm->stack_top = sp;
            int pexc = vm_pair_to_num(vm, a, b, &na, &nb);
            stack = vm->stack; sp = vm->stack_top;
            if (pexc) return VM_JIT_EXC;
            if (nb == 0.0) { vm->pending_exc = vm_str("Illegal division by zero"); vm->has_pending_exc = 1; vm->stack_top = sp; return VM_JIT_EXC; }
            J_PUSH(VM_MAKE_FLOAT(na / nb));
        }
        break;
    }
    case OP_MOD: {
        VMValue b = J_POP(), a = J_POP();
        if (VM_BOTH_INT(a, b)) {
            int64_t bv = VM_INT_VAL(b);
            if (bv == 0) { vm->pending_exc = vm_str("Illegal modulus zero"); vm->has_pending_exc = 1; vm->stack_top = sp; return VM_JIT_EXC; }
            int64_t av = VM_INT_VAL(a), r = 0;
            if (!(av == INT64_MIN && bv == -1)) {
                r = av % bv;
                if (r != 0 && ((r < 0) != (bv < 0))) r += bv;
            }
            J_PUSH(VM_MAKE_INT(r));
        } else {
            if (vm_obj_class_name(a) || vm_obj_class_name(b)) {
                VMValue ov = VM_UNDEF_VAL; vm->stack_top = sp;
                if (vm_try_bin_overload(vm, "%", a, b, &ov)) {
                    stack = vm->stack; sp = vm->stack_top;
                    if (vm->has_pending_exc) return VM_JIT_EXC;
                    J_PUSH(ov); break;
                }
                stack = vm->stack; sp = vm->stack_top;
            }
            int64_t av = 0, bv = 0;
            vm->stack_top = sp;
            int pexc = vm_pair_to_int(vm, a, b, &av, &bv);
            stack = vm->stack; sp = vm->stack_top;
            if (pexc) return VM_JIT_EXC;
            if (bv == 0) { vm->pending_exc = vm_str("Illegal modulus zero"); vm->has_pending_exc = 1; vm->stack_top = sp; return VM_JIT_EXC; }
            int64_t r = 0;
            if (!(av == INT64_MIN && bv == -1)) {
                r = av % bv;
                if (r != 0 && ((r < 0) != (bv < 0))) r += bv;
            }
            J_PUSH(VM_MAKE_INT(r));
        }
        break;
    }
    case OP_LT: case OP_LE: case OP_GT: case OP_GE: case OP_EQ: case OP_NE: {
        VMValue b = J_POP(), a = J_POP();
        int res = 0;
        if (VM_BOTH_INT(a, b)) {
            int64_t ia = VM_INT_VAL(a), ib = VM_INT_VAL(b);
            res = (op == OP_LT) ? (ia < ib) : (op == OP_LE) ? (ia <= ib) : (op == OP_GT) ? (ia > ib)
                : (op == OP_GE) ? (ia >= ib) : (op == OP_EQ) ? (ia == ib) : (ia != ib);
        } else if (!VM_IS_PTR(a) && !VM_IS_PTR(b)) {
            double da = VM_IS_FLOAT(a) ? VM_FLOAT_VAL(a) : (VM_IS_INT(a) ? (double)VM_INT_VAL(a) : 0.0);
            double db = VM_IS_FLOAT(b) ? VM_FLOAT_VAL(b) : (VM_IS_INT(b) ? (double)VM_INT_VAL(b) : 0.0);
            res = (op == OP_LT) ? (da < db) : (op == OP_LE) ? (da <= db) : (op == OP_GT) ? (da > db)
                : (op == OP_GE) ? (da >= db) : (op == OP_EQ) ? (da == db) : (da != db);
        } else {
            const char *opstr = (op == OP_LT) ? "<" : (op == OP_LE) ? "<=" : (op == OP_GT) ? ">"
                              : (op == OP_GE) ? ">=" : (op == OP_EQ) ? "==" : "!=";
            if (vm_obj_class_name(a) || vm_obj_class_name(b)) {
                VMValue ov = VM_UNDEF_VAL; vm->stack_top = sp;
                if (vm_try_bin_overload(vm, opstr, a, b, &ov)) {
                    stack = vm->stack; sp = vm->stack_top;
                    if (vm->has_pending_exc) return VM_JIT_EXC;
                    J_PUSH(ov); break;
                }
                stack = vm->stack; sp = vm->stack_top;
            }
            if (VM_IS_ANY_INT(a) && VM_IS_ANY_INT(b)) {
                int64_t ia = VM_ANY_INT_VAL(a), ib = VM_ANY_INT_VAL(b);
                res = (op == OP_LT) ? (ia < ib) : (op == OP_LE) ? (ia <= ib) : (op == OP_GT) ? (ia > ib)
                    : (op == OP_GE) ? (ia >= ib) : (op == OP_EQ) ? (ia == ib) : (ia != ib);
            } else {
                double da = 0.0, db = 0.0;
                vm->stack_top = sp;
                int pexc = vm_pair_to_num(vm, a, b, &da, &db);
                stack = vm->stack; sp = vm->stack_top;
                if (pexc) return VM_JIT_EXC;
                res = (op == OP_LT) ? (da < db) : (op == OP_LE) ? (da <= db) : (op == OP_GT) ? (da > db)
                    : (op == OP_GE) ? (da >= db) : (op == OP_EQ) ? (da == db) : (da != db);
            }
        }
        J_PUSH(VM_MAKE_INT_FAST(res));
        break;
    }
    case OP_BIT_AND: case OP_BIT_OR: case OP_BIT_XOR: {
        VMValue b = J_POP(), a = J_POP();
        const char *opstr = (op == OP_BIT_AND) ? "&" : (op == OP_BIT_OR) ? "|" : "^";
        if (vm_obj_class_name(a) || vm_obj_class_name(b)) {
            VMValue ov = VM_UNDEF_VAL; vm->stack_top = sp;
            if (vm_try_bin_overload(vm, opstr, a, b, &ov)) {
                stack = vm->stack; sp = vm->stack_top;
                if (vm->has_pending_exc) return VM_JIT_EXC;
                J_PUSH(ov); break;
            }
            stack = vm->stack; sp = vm->stack_top;
        }
        int64_t ia = 0, ib = 0;
        if (VM_IS_PTR(a) || VM_IS_PTR(b)) {
            vm->stack_top = sp;
            int pexc = vm_pair_to_int(vm, a, b, &ia, &ib);
            stack = vm->stack; sp = vm->stack_top;
            if (pexc) return VM_JIT_EXC;
        } else {
            ia = vm_to_int(a); ib = vm_to_int(b);
        }
        J_PUSH(VM_MAKE_INT(op == OP_BIT_AND ? (ia & ib) : op == OP_BIT_OR ? (ia | ib) : (ia ^ ib)));
        break;
    }
    case OP_NEGATE: {
        VMValue a = J_POP();
        if (VM_IS_INT(a)) J_PUSH(VM_MAKE_INT(-VM_INT_VAL(a)));
        else if (VM_IS_FLOAT(a)) J_PUSH(VM_MAKE_FLOAT(-VM_FLOAT_VAL(a)));
        else {
            if (vm_obj_class_name(a)) {
                VMValue ov = VM_UNDEF_VAL; vm->stack_top = sp;
                if (vm_try_un_overload(vm, "neg", a, &ov)) {
                    stack = vm->stack; sp = vm->stack_top;
                    if (vm->has_pending_exc) return VM_JIT_EXC;
                    J_PUSH(ov); break;
                }
                stack = vm->stack; sp = vm->stack_top;
            }
            if (VM_IS_PTR(a)) {
                vm->stack_top = sp; vm->gc_pause++;
                if (vm_is_float_operand(a)) {
                    double nv = vm_to_num_vm(vm, a);
                    vm->gc_pause--; stack = vm->stack; sp = vm->stack_top;
                    if (vm->has_pending_exc) return VM_JIT_EXC;
                    J_PUSH(VM_MAKE_FLOAT(-nv));
                } else {
                    int64_t iv = vm_to_int_vm(vm, a);
                    vm->gc_pause--; stack = vm->stack; sp = vm->stack_top;
                    if (vm->has_pending_exc) return VM_JIT_EXC;
                    J_PUSH(VM_MAKE_INT(-iv));
                }
            } else {
                J_PUSH(vm_is_float_operand(a) ? VM_MAKE_FLOAT(-vm_to_num(a)) : VM_MAKE_INT(-vm_to_int(a)));
            }
        }
        break;
    }
    case OP_NOT: {
        vm->stack_top = sp;
        int t = vm_jit_is_truthy(vm);
        stack = vm->stack; sp = vm->stack_top;
        if (t < 0) return VM_JIT_EXC;
        J_PUSH(VM_MAKE_INT_FAST(!t));
        break;
    }
    case OP_SUB_LOCAL_I32:
    case OP_ADD_LOCAL_I32:
    case OP_BIT_AND_LOCAL_I32:
    case OP_MOD_LOCAL_I32: {
        int kind = (op == OP_SUB_LOCAL_I32) ? 0 : (op == OP_ADD_LOCAL_I32) ? 1 : (op == OP_BIT_AND_LOCAL_I32) ? 2 : 3;
        vm->stack_top = sp;
        VMValue lop = (kind == 2) ? locals[di->a] : vm_rawint_operand(vm, chunk, di->a, locals[di->a]);
        VMValue r = vm_slow_local_imm_arith(vm, lop, di->imm.i32.i0, kind);
        stack = vm->stack; sp = vm->stack_top;
        if (vm->has_pending_exc) return VM_JIT_EXC;
        J_PUSH(r);
        break;
    }
    case OP_BIT_AND_ADD_I32: {
        vm->stack_top = sp;
        VMValue r = vm_slow_local_imm_arith(vm, locals[di->a], di->imm.i32.i0, 2);
        stack = vm->stack; sp = vm->stack_top;
        if (vm->has_pending_exc) return VM_JIT_EXC;
        if (VM_IS_INT(r)) r = VM_MAKE_INT(VM_INT_VAL(r) + (int64_t)di->imm.i32.i1);
        else {
            vm->stack_top = sp;
            r = vm_slow_local_imm_arith(vm, r, di->imm.i32.i1, 1);
            stack = vm->stack; sp = vm->stack_top;
            if (vm->has_pending_exc) return VM_JIT_EXC;
        }
        J_PUSH(r);
        break;
    }
    case OP_ADD_LOCALS: {
        vm->stack_top = sp;
        VMValue r = vm_slow_add_locals(vm, vm_rawint_operand(vm, chunk, di->a, locals[di->a]),
                                       vm_rawint_operand(vm, chunk, di->b, locals[di->b]));
        stack = vm->stack; sp = vm->stack_top;
        if (vm->has_pending_exc) return VM_JIT_EXC;
        J_PUSH(r);
        break;
    }
    case OP_SUB_LOCALS:
    case OP_MUL_LOCALS: {
        vm->stack_top = sp;
        VMValue r = vm_slow_bin_arith(vm, (op == OP_SUB_LOCALS) ? 1 : 2,
                                      vm_rawint_operand(vm, chunk, di->a, locals[di->a]),
                                      vm_rawint_operand(vm, chunk, di->b, locals[di->b]));
        stack = vm->stack; sp = vm->stack_top;
        if (vm->has_pending_exc) return VM_JIT_EXC;
        J_PUSH(r);
        break;
    }
    case OP_ADD_TO_LOCAL: {
        vm->stack_top = sp;
        vm_slow_add_to_local(vm, di->a, locals[di->b], di->flags, 0);
        if (vm->has_pending_exc) return VM_JIT_EXC;
        stack = vm->stack; sp = vm->stack_top;
        break;
    }
    case OP_ADD_TO_LOCAL_VAL:
    case OP_SUB_TO_LOCAL_VAL: {
        VMValue rhs = J_POP();
        vm->stack_top = sp;
        vm_slow_add_to_local(vm, di->a, rhs, di->flags, (op == OP_SUB_TO_LOCAL_VAL));
        if (vm->has_pending_exc) return VM_JIT_EXC;
        stack = vm->stack; sp = vm->stack_top;
        break;
    }
    case OP_NEW_ARRAY: { uint32_t c = di->imm.u32; J_PUSH(VM_MAKE_PTR(vm_array_new(c > 0 ? (int)c : 4))); break; }
    case OP_NEW_HASH:  { uint32_t c = di->imm.u32; J_PUSH(VM_MAKE_PTR(vm_hash_new(c > 0 ? (int)c : 4))); break; }
    case OP_ARRAY_SIZE: {
        VMValue a = J_POP();
        if (VM_IS_PTR(a) && VM_PTR_TYPE(a) == VM_OBJ_ARRAY) {
            VMArray *arr = VM_AS_ARRAY(a);
            if (!VM_IS_UNDEF(arr->tied_obj)) {
                int fsz = vm_tied_array_find_method(vm, arr->tied_obj, "FETCHSIZE");
                if (fsz >= 0) {
                    vm->stack_top = sp;
                    VMValue r = vm_execute_call(vm, fsz, &arr->tied_obj, 1);
                    stack = vm->stack; sp = vm->stack_top;
                    if (vm->has_pending_exc) return VM_JIT_EXC;
                    J_PUSH(VM_MAKE_INT(vm_to_int(r))); break;
                }
            }
            J_PUSH(VM_MAKE_INT(arr->size));
        } else if (VM_IS_PTR(a) && VM_PTR_TYPE(a) == VM_OBJ_HASH) {
            VMHash *h = VM_AS_HASH(a);
            if (!VM_IS_UNDEF(h->tied_obj)) {
                vm->stack_top = sp;
                VMValue r = vm_tied_hash_scalar(vm, h);
                stack = vm->stack; sp = vm->stack_top;
                if (vm->has_pending_exc) return VM_JIT_EXC;
                J_PUSH(r);
            } else J_PUSH(VM_MAKE_INT(h->size));
        } else J_PUSH(VM_MAKE_INT(0));
        break;
    }
    case OP_ARRAY_PUSH: {
        VMValue val = J_POP(), arr = J_POP();
        if (VM_IS_PTR(arr) && VM_PTR_TYPE(arr) == VM_OBJ_ARRAY) {
            VMArray *a = VM_AS_ARRAY(arr);
            if (!VM_IS_UNDEF(a->tied_obj)) {
                int pf = vm_tied_array_find_method(vm, a->tied_obj, "PUSH");
                if (pf >= 0) {
                    vm->stack_top = sp;
                    VMValue cargs[2] = { a->tied_obj, val };
                    vm_execute_call(vm, pf, cargs, 2);
                    stack = vm->stack; sp = vm->stack_top;
                    if (vm->has_pending_exc) return VM_JIT_EXC;
                    break;
                }
            }
            vm_array_push(a, val);
        }
        break;
    }
    case OP_ARRAY_GET: {
        VMValue idx = J_POP(), arr = J_POP();
        if (VM_IS_PTR(arr) && VM_PTR_TYPE(arr) == VM_OBJ_ARRAY) {
            VMArray *a = VM_AS_ARRAY(arr);
            if (!VM_IS_UNDEF(a->tied_obj)) {
                int ff = vm_tied_array_find_method(vm, a->tied_obj, "FETCH");
                if (ff >= 0) {
                    vm->stack_top = sp;
                    VMValue cargs[2] = { a->tied_obj, idx };
                    VMValue r = vm_execute_call(vm, ff, cargs, 2);
                    stack = vm->stack; sp = vm->stack_top;
                    if (vm->has_pending_exc) return VM_JIT_EXC;
                    J_PUSH(r); break;
                }
            }
            VMValue r = vm_array_get(a, vm_to_int(idx));
            if (vm->has_weakrefs && VM_IS_PTR(r) && VM_PTR_TYPE(r) == VM_OBJ_WEAKREF) {
                vm->stack_top = sp;
                r = vm_unwrap_weak_gc(vm, r, &arr, 1);
                stack = vm->stack; sp = vm->stack_top;
            }
            if (VM_IS_PTR(r) && VM_PTR_TYPE(r) == VM_OBJ_STRBUF) VM_AS_STRBUF(r)->shared = 1;
            J_PUSH(r);
        } else J_PUSH(VM_UNDEF_VAL);
        break;
    }
    case OP_ARRAY_SET: {
        VMValue val = J_POP(), idx = J_POP(), arr = J_POP();
        if (VM_IS_PTR(arr) && VM_PTR_TYPE(arr) == VM_OBJ_ARRAY) {
            VMArray *a = VM_AS_ARRAY(arr);
            if (!VM_IS_UNDEF(a->tied_obj)) {
                int sf = vm_tied_array_find_method(vm, a->tied_obj, "STORE");
                if (sf >= 0) {
                    vm->stack_top = sp;
                    VMValue cargs[3] = { a->tied_obj, idx, val };
                    vm_execute_call(vm, sf, cargs, 3);
                    stack = vm->stack; sp = vm->stack_top;
                    if (vm->has_pending_exc) return VM_JIT_EXC;
                    break;
                }
            }
            vm_array_set(a, vm_to_int(idx), val);
        }
        break;
    }
    case OP_HASH_GET_CONST: {
        uint16_t kidx = di->a;
        VMValue hash = J_POP();
        if (VM_IS_PTR(hash) && VM_PTR_TYPE(hash) == VM_OBJ_HASH) {
            VMHash *h = VM_AS_HASH(hash);
            if (!VM_IS_UNDEF(h->tied_obj)) {
                int ff = vm_tied_array_find_method(vm, h->tied_obj, "FETCH");
                if (ff >= 0) {
                    vm->stack_top = sp;
                    VMValue cargs[2] = { h->tied_obj, chunk->str_const_vals[kidx] };
                    VMValue r = vm_execute_call(vm, ff, cargs, 2);
                    stack = vm->stack; sp = vm->stack_top;
                    if (vm->has_pending_exc) return VM_JIT_EXC;
                    J_PUSH(r); break;
                }
            }
            VMString *kstr = VM_AS_STR(chunk->str_const_vals[kidx]);
            uint32_t kh = ((const VMConstString*)((const char*)kstr - offsetof(VMConstString, str)))->hash;
            VMHashEntry *e = vm_hash_probe_first(h, kh);
            VMValue r;
            if (__builtin_expect(e && e->occupied == 1 && e->hash == kh && e->keylen == kstr->len &&
                                 memcmp(e->key, kstr->data, kstr->len) == 0, 1)) {
                r = e->value;
            } else {
                uint32_t hic = (uint32_t)(((uintptr_t)di >> 3) & 63);
                uint32_t hs = vm->hash_ic[hic].slot;
                if (vm->hash_ic[hic].ip == di && hs < (uint32_t)h->capacity &&
                    h->entries[hs].occupied == 1 && h->entries[hs].hash == kh &&
                    h->entries[hs].keylen == kstr->len &&
                    memcmp(h->entries[hs].key, kstr->data, kstr->len) == 0) {
                    r = h->entries[hs].value;
                } else {
                    int32_t idx = vm_hash_slot_idx_nh(h, kstr->data, kstr->len, kh);
                    if (idx < 0) { J_PUSH(VM_UNDEF_VAL); break; }
                    vm->hash_ic[hic].ip = di;
                    vm->hash_ic[hic].slot = (uint32_t)idx;
                    r = h->entries[idx].value;
                }
            }
            if (vm->has_weakrefs && VM_IS_PTR(r) && VM_PTR_TYPE(r) == VM_OBJ_WEAKREF) {
                vm->stack_top = sp;
                VMValue roots[2] = { hash, chunk->str_const_vals[kidx] };
                r = vm_unwrap_weak_gc(vm, r, roots, 2);
                stack = vm->stack; sp = vm->stack_top;
            }
            if (VM_IS_PTR(r) && VM_PTR_TYPE(r) == VM_OBJ_STRBUF) VM_AS_STRBUF(r)->shared = 1;
            J_PUSH(r);
        } else J_PUSH(VM_UNDEF_VAL);
        break;
    }
    case OP_HASH_SET_CONST: {
        uint16_t kidx = di->a;
        VMValue v = J_POP(), hash = J_POP();
        if (VM_IS_PTR(hash) && VM_PTR_TYPE(hash) == VM_OBJ_HASH) {
            VMHash *h = VM_AS_HASH(hash);
            if (!VM_IS_UNDEF(h->tied_obj)) {
                int sf = vm_tied_array_find_method(vm, h->tied_obj, "STORE");
                if (sf >= 0) {
                    vm->stack_top = sp;
                    VMValue cargs[3] = { h->tied_obj, chunk->str_const_vals[kidx], v };
                    vm_execute_call(vm, sf, cargs, 3);
                    stack = vm->stack; sp = vm->stack_top;
                    if (vm->has_pending_exc) return VM_JIT_EXC;
                    break;
                }
            }
            VMString *kstr = VM_AS_STR(chunk->str_const_vals[kidx]);
            uint32_t kh = ((const VMConstString*)((const char*)kstr - offsetof(VMConstString, str)))->hash;
            VMHashEntry *e = vm_hash_probe_first(h, kh);
            if (__builtin_expect(e && e->occupied == 1 && e->hash == kh && e->keylen == kstr->len &&
                                 memcmp(e->key, kstr->data, kstr->len) == 0, 1)) {
                e->value = v;
            } else if (__builtin_expect(!e && (h->free_head >= 0 || h->used < h->capacity) &&
                                        !vm->is_worker_vm && !vm->workers_head, 1)) {
                e = vm_hash_append_slot(h, kh);
                uint32_t pid = vm->program->prog_id;
                uint32_t cidx = (uint32_t)(((uintptr_t)kstr >> 4) & 127);
                const char *ikey = NULL;
                if (vm->str_intern_cache[cidx].kstr == kstr && vm->str_intern_cache[cidx].prog_id == pid) {
                    ikey = vm->str_intern_cache[cidx].ikey;
                } else if (kstr->len <= VM_KEY_INTERN_MAXLEN) {
                    ikey = vm_intern_hash_key(vm, kstr->data, kstr->len, kh);
                    if (ikey) {
                        vm->str_intern_cache[cidx].kstr = kstr;
                        vm->str_intern_cache[cidx].prog_id = pid;
                        vm->str_intern_cache[cidx].ikey = ikey;
                    }
                }
                if (ikey) { e->key = (char *)ikey; e->key_interned = 1; }
                else {
                    char *k = malloc(kstr->len + 1);
                    if (kstr->len > 0) memcpy(k, kstr->data, kstr->len);
                    k[kstr->len] = '\0';
                    e->key = k; e->key_interned = 0;
                }
                e->value = v; e->hash = kh; e->keylen = (uint32_t)kstr->len; e->occupied = 1; h->size++;
                e->key_bytes = (uint8_t)!kstr->hdr.is_utf8;
            } else {
                uint32_t hic = (uint32_t)(((uintptr_t)di >> 3) & 63);
                uint32_t hs = vm->hash_ic[hic].slot;
                if (vm->hash_ic[hic].ip == di && hs < (uint32_t)h->capacity &&
                    h->entries[hs].occupied == 1 && h->entries[hs].hash == kh &&
                    h->entries[hs].keylen == kstr->len &&
                    memcmp(h->entries[hs].key, kstr->data, kstr->len) == 0) {
                    h->entries[hs].value = v;
                    break;
                }
                uint32_t ins = vm_hash_set_nh_slot(h, kstr->data, kstr->len, kh, v);
                if (ins != UINT32_MAX) {
                    vm->hash_ic[hic].ip = di;
                    vm->hash_ic[hic].slot = ins;
                }
            }
        }
        break;
    }
    case OP_HASH_GET_CONCAT: {
        uint16_t pfx_idx = di->a;
        VMValue suffix = J_POP(), hash = J_POP();
        if (!VM_IS_PTR(hash) || VM_PTR_TYPE(hash) != VM_OBJ_HASH) { J_PUSH(VM_UNDEF_VAL); break; }
        if (!VM_IS_ANY_INT(suffix) && vm_has_stringify(vm->program, suffix)) {
            J_PUSH(hash); vm->stack_top = sp;
            suffix = vm_stringify_ov(vm, suffix);
            stack = vm->stack; sp = vm->stack_top;
            hash = J_POP();
            if (vm->has_pending_exc) return VM_JIT_EXC;
        }
        const char *pfx = chunk->str_consts[pfx_idx]; size_t plen = strlen(pfx);
        char key_buf[256]; char *kptr = key_buf; char *heap_key = NULL; size_t klen = 0;
        if (VM_IS_ANY_INT(suffix)) {
            int64_t n = VM_ANY_INT_VAL(suffix); uint64_t u = (n < 0) ? (0ULL - (uint64_t)n) : (uint64_t)n;
            char tmp[24]; int tl = 0;
            if (u == 0) tmp[tl++] = '0'; else while (u > 0) { tmp[tl++] = (char)('0' + (u % 10)); u /= 10; }
            klen = plen + (n < 0 ? 1 : 0) + (size_t)tl;
            if (klen + 1 > sizeof(key_buf)) { heap_key = malloc(klen + 1); kptr = heap_key; }
            memcpy(kptr, pfx, plen); char *wp = kptr + plen; if (n < 0) *wp++ = '-';
            for (int ti = tl - 1; ti >= 0; ti--) *wp++ = tmp[ti];
            *wp = '\0';
        } else {
            char sbuf[64]; size_t sl = 0; const char *ss = vm_bytes_view(suffix, sbuf, sizeof(sbuf), &sl);
            klen = plen + sl;
            if (klen + 1 > sizeof(key_buf)) { heap_key = malloc(klen + 1); kptr = heap_key; }
            memcpy(kptr, pfx, plen); if (sl > 0) memcpy(kptr + plen, ss, sl); kptr[klen] = '\0';
        }
        VMHash *h = VM_AS_HASH(hash);
        if (!VM_IS_UNDEF(h->tied_obj)) {
            int ff = vm_tied_array_find_method(vm, h->tied_obj, "FETCH");
            if (ff >= 0) {
                VMValue ksv = VM_MAKE_STR_N(kptr, klen); free(heap_key);
                vm->stack_top = sp; VMValue cargs[2] = { h->tied_obj, ksv };
                VMValue r = vm_execute_call(vm, ff, cargs, 2);
                stack = vm->stack; sp = vm->stack_top;
                if (vm->has_pending_exc) return VM_JIT_EXC;
                J_PUSH(r); break;
            }
        }
        VMValue *slot = vm->has_weakrefs ? vm_hash_slot_ptr_n(h, kptr, klen) : NULL;
        if (slot && VM_IS_PTR(*slot) && VM_PTR_TYPE(*slot) == VM_OBJ_WEAKREF) {
            free(heap_key); vm->stack_top = sp;
            VMValue r = vm_unwrap_weak_gc(vm, *slot, &hash, 1);
            stack = vm->stack; sp = vm->stack_top;
            J_PUSH(r);
        } else {
            VMValue r = vm_hash_get_n(h, kptr, klen);
            J_PUSH(r); free(heap_key);
        }
        break;
    }
    case OP_HASH_SET_CONCAT: {
        uint16_t pfx_idx = di->a;
        VMValue val = J_POP(), suffix = J_POP(), hash = J_POP();
        if (!VM_IS_PTR(hash) || VM_PTR_TYPE(hash) != VM_OBJ_HASH) break;
        if (!VM_IS_ANY_INT(suffix) && vm_has_stringify(vm->program, suffix)) {
            J_PUSH(hash); J_PUSH(val); vm->stack_top = sp;
            suffix = vm_stringify_ov(vm, suffix);
            stack = vm->stack; sp = vm->stack_top;
            val = J_POP(); hash = J_POP();
            if (vm->has_pending_exc) return VM_JIT_EXC;
        }
        const char *pfx = chunk->str_consts[pfx_idx]; size_t plen = strlen(pfx);
        char key_buf[256]; char *kptr = key_buf; char *heap_key = NULL; size_t klen = 0;
        if (VM_IS_ANY_INT(suffix)) {
            int64_t n = VM_ANY_INT_VAL(suffix); uint64_t u = (n < 0) ? (0ULL - (uint64_t)n) : (uint64_t)n;
            char tmp[24]; int tl = 0;
            if (u == 0) tmp[tl++] = '0'; else while (u > 0) { tmp[tl++] = (char)('0' + (u % 10)); u /= 10; }
            klen = plen + (n < 0 ? 1 : 0) + (size_t)tl;
            if (klen + 1 > sizeof(key_buf)) { heap_key = malloc(klen + 1); kptr = heap_key; }
            memcpy(kptr, pfx, plen); char *wp = kptr + plen; if (n < 0) *wp++ = '-';
            for (int ti = tl - 1; ti >= 0; ti--) *wp++ = tmp[ti];
            *wp = '\0';
        } else {
            char sbuf[64]; size_t sl = 0; const char *ss = vm_bytes_view(suffix, sbuf, sizeof(sbuf), &sl);
            klen = plen + sl;
            if (klen + 1 > sizeof(key_buf)) { heap_key = malloc(klen + 1); kptr = heap_key; }
            memcpy(kptr, pfx, plen); if (sl > 0) memcpy(kptr + plen, ss, sl); kptr[klen] = '\0';
        }
        VMHash *h = VM_AS_HASH(hash);
        if (!VM_IS_UNDEF(h->tied_obj)) {
            int sf = vm_tied_array_find_method(vm, h->tied_obj, "STORE");
            if (sf >= 0) {
                VMValue ksv = VM_MAKE_STR_N(kptr, klen); free(heap_key);
                vm->stack_top = sp; VMValue cargs[3] = { h->tied_obj, ksv, val };
                vm_execute_call(vm, sf, cargs, 3);
                stack = vm->stack; sp = vm->stack_top;
                if (vm->has_pending_exc) return VM_JIT_EXC;
                break;
            }
        }
        vm_hash_set_n(h, kptr, klen, val); free(heap_key);
        break;
    }
    case OP_CONCAT: {
        VMValue b = J_POP(), a = J_POP();
        if (vm_has_stringify(vm->program, a)) {
            J_PUSH(b); vm->stack_top = sp;
            a = vm_stringify_ov(vm, a);
            stack = vm->stack; sp = vm->stack_top; b = J_POP();
            if (vm->has_pending_exc) return VM_JIT_EXC;
        }
        if (vm_has_stringify(vm->program, b)) {
            J_PUSH(a); vm->stack_top = sp;
            b = vm_stringify_ov(vm, b);
            stack = vm->stack; sp = vm->stack_top; a = J_POP();
            if (vm->has_pending_exc) return VM_JIT_EXC;
        }
        char ba[64], bb[64]; size_t la = 0, lb = 0;
        const char *sa = vm_bytes_view(a, ba, sizeof(ba), &la);
        const char *sb = vm_bytes_view(b, bb, sizeof(bb), &lb);
        VMString *r = vm_str_alloc(la + lb);
        if (vm_is_utf8(a) || vm_is_utf8(b)) r->hdr.is_utf8 = 1;
        r->len = (uint32_t)(la + lb);
        if (la > 0) memcpy(r->data, sa, la);
        if (lb > 0) memcpy(r->data + la, sb, lb);
        r->data[la + lb] = '\0';
        J_PUSH(VM_MAKE_PTR(r));
        break;
    }
    case OP_STR_LEN: {
        VMValue s = J_POP();
        int64_t len = 0;
        if (VM_IS_PTR(s) && (VM_PTR_TYPE(s) == VM_OBJ_STR || VM_PTR_TYPE(s) == VM_OBJ_STRBUF)) {
            size_t blen = (VM_PTR_TYPE(s) == VM_OBJ_STR) ? VM_AS_STR(s)->len : VM_AS_STRBUF(s)->len;
            const char *p = (VM_PTR_TYPE(s) == VM_OBJ_STR) ? VM_AS_STR(s)->data : VM_AS_STRBUF(s)->data;
            len = vm_str_is_utf8_chars(s, p, blen) ? (int64_t)vm_utf8_char_count(p, blen) : (int64_t)blen;
        } else if (!VM_IS_UNDEF(s)) {
            char b[64]; size_t bl = 0; (void)vm_bytes_view(s, b, sizeof(b), &bl); len = (int64_t)bl;
        }
        J_PUSH(VM_MAKE_INT(len));
        break;
    }
    case OP_SAY:
    case OP_PRINT: {
        VMValue v = J_POP();
        if (vm_has_stringify(vm->program, v)) {
            vm->stack_top = sp;
            v = vm_stringify_ov(vm, v);
            stack = vm->stack; sp = vm->stack_top;
            if (vm->has_pending_exc) return VM_JIT_EXC;
        }
        VMFileHandle *out_fh = (!VM_IS_UNDEF(vm->default_fh) && VM_IS_PTR(vm->default_fh) &&
                                VM_PTR_TYPE(vm->default_fh) == VM_OBJ_FILEHANDLE)
                               ? (VMFileHandle*)VM_TO_PTR(vm->default_fh) : NULL;
        FILE *out = out_fh ? (FILE*)out_fh->fp : stdout;
        if (out) {
            vm_write_val(out, v, 0);
            if (op == OP_SAY) fputc('\n', out);
            fflush(out);
            if (__builtin_expect(out_fh && out_fh->is_mem == 2 && VM_IS_PTR(out_fh->mem_sref), 0)) {
                vm->stack_top = sp;
                vm_fh_sync_mem_sref(vm, out_fh);
                if (vm->has_pending_exc) return VM_JIT_EXC;
            }
        }
        break;
    }
    case OP_APPEND_LOCAL: {
        uint16_t slot = di->a;
        VMValue rhs = J_POP();
        if (__builtin_expect(vm_has_stringify(vm->program, rhs), 0)) {
            vm->stack_top = sp;
            rhs = vm_stringify_ov(vm, rhs);
            if (vm->has_pending_exc) return VM_JIT_EXC;
            stack = vm->stack; sp = vm->stack_top;
            frame = &vm->frames[vm->frame_count - 1];
            locals = stack + frame->local_base;
        }
        char buf[64]; size_t al;
        const char *as = vm_bytes_view(rhs, buf, sizeof(buf), &al);
        VMValue *loc = &locals[slot];
        if (VM_IS_PTR(*loc) && VM_PTR_TYPE(*loc) == VM_OBJ_CELL) loc = &VM_AS_CELL(*loc)->val;
        if (__builtin_expect(VM_IS_PTR(*loc) && VM_PTR_TYPE(*loc) == VM_OBJ_TIED_SCALAR, 0)) {
            frame->ip = (VMInsn *)di; vm->stack_top = sp;
            J_PUSH(rhs);
            vm->stack_top = sp;
            return VM_JIT_INTERP;
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
                VMValue rcopy = vm_make_str_flag(as, al, VM_IS_PTR(rhs) ? ((VMObjHeader*)VM_TO_PTR(rhs))->is_utf8 : 0);
                J_PUSH(rcopy); vm->stack_top = sp;
                lhs = vm_stringify_ov(vm, lhs);
                if (vm->has_pending_exc) return VM_JIT_EXC;
                stack = vm->stack; sp = vm->stack_top;
                rhs = J_POP();
                as = vm_bytes_view(rhs, buf, sizeof(buf), &al);
                frame = &vm->frames[vm->frame_count - 1];
                locals = stack + frame->local_base;
                loc = &locals[slot];
                if (VM_IS_PTR(*loc) && VM_PTR_TYPE(*loc) == VM_OBJ_CELL) loc = &VM_AS_CELL(*loc)->val;
            }
            const char *ex = ""; size_t el = 0; char lbuf[64];
            if (!VM_IS_UNDEF(lhs)) ex = vm_bytes_view(lhs, lbuf, sizeof(lbuf), &el);
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
        vm_val_free(&rhs);
        break;
    }
    case OP_APPEND_CONST: {
        uint16_t str_idx = di->a;
        uint16_t slot = di->b;
        const char *as = chunk->str_consts[str_idx];
        size_t al = chunk->str_const_lens ? chunk->str_const_lens[str_idx] : strlen(as);
        int rhs_u8 = (chunk->str_const_vals && VM_IS_PTR(chunk->str_const_vals[str_idx]) &&
                      ((VMObjHeader*)VM_TO_PTR(chunk->str_const_vals[str_idx]))->is_utf8) ? 1 : 0;
        VMValue *loc = &locals[slot];
        if (VM_IS_PTR(*loc) && VM_PTR_TYPE(*loc) == VM_OBJ_CELL) loc = &VM_AS_CELL(*loc)->val;
        if (__builtin_expect(VM_IS_PTR(*loc) && VM_PTR_TYPE(*loc) == VM_OBJ_TIED_SCALAR, 0)) {
            frame->ip = (VMInsn *)di; vm->stack_top = sp;
            return VM_JIT_INTERP;
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
            if (rhs_u8) sb->hdr.is_utf8 = 1;
        } else {
            VMValue lhs = *loc;
            if (__builtin_expect(vm_has_stringify(vm->program, lhs), 0)) {
                vm->stack_top = sp;
                lhs = vm_stringify_ov(vm, lhs);
                if (vm->has_pending_exc) return VM_JIT_EXC;
                stack = vm->stack; sp = vm->stack_top;
                as = chunk->str_consts[str_idx];
                al = chunk->str_const_lens ? chunk->str_const_lens[str_idx] : strlen(as);
                frame = &vm->frames[vm->frame_count - 1];
                locals = stack + frame->local_base;
                loc = &locals[slot];
                if (VM_IS_PTR(*loc) && VM_PTR_TYPE(*loc) == VM_OBJ_CELL) loc = &VM_AS_CELL(*loc)->val;
            }
            const char *ex = ""; size_t el = 0; char lbuf[64];
            if (!VM_IS_UNDEF(lhs)) ex = vm_bytes_view(lhs, lbuf, sizeof(lbuf), &el);
            VMStrBuf *sb = malloc(sizeof(VMStrBuf));
            vm_gc_init_hdr(&sb->hdr, VM_OBJ_STRBUF);
            sb->shared = 0;
            if (rhs_u8 || (VM_IS_PTR(lhs) && ((VMObjHeader*)VM_TO_PTR(lhs))->is_utf8)) sb->hdr.is_utf8 = 1;
            sb->cap = (el + al) * 2 + 64; sb->data = malloc(sb->cap);
            if (el > 0) memcpy(sb->data, ex, el);
            if (al > 0) memcpy(sb->data + el, as, al);
            sb->len = el + al; sb->data[sb->len] = 0;
            vm_val_free(loc); *loc = VM_MAKE_PTR(sb);
        }
        break;
    }
    case OP_LOAD_GLOBAL: {
        uint16_t name_idx = di->a;
        VMHash *gh = vm_gh_for(vm, chunk->str_consts[name_idx]);
        VMValue v;
        if (__builtin_expect(!vm->is_worker_vm && !vm->workers_head && gh != NULL && chunk->str_const_vals != NULL, 1)) {
            uint32_t ic_slot = (uint32_t)(((uintptr_t)di >> 3) & 63);
            VMString *nstr = VM_AS_STR(chunk->str_const_vals[name_idx]);
            const VMConstString *ncs = (const VMConstString*)((const char*)nstr - offsetof(VMConstString, str));
            if (__builtin_expect(vm->global_ic[ic_slot].ip == di && vm->global_ic[ic_slot].globals == gh &&
                                 vm->global_ic[ic_slot].gen == gh->grow_gen, 1)) {
                uint32_t es = vm->global_ic[ic_slot].slot;
                VMHashEntry *e = &gh->entries[es];
                if (__builtin_expect(es < (uint32_t)gh->capacity && e->occupied == 1 &&
                                     e->hash == ncs->hash && e->keylen == nstr->len &&
                                     memcmp(e->key, nstr->data, nstr->len) == 0, 1)) {
                    v = e->value;
                    goto L_jit_lg_have;
                }
            }
            int32_t idx = vm_hash_slot_idx_nh(gh, nstr->data, nstr->len, ncs->hash);
            if (idx >= 0) {
                vm->global_ic[ic_slot].ip = di; vm->global_ic[ic_slot].globals = gh;
                vm->global_ic[ic_slot].gen = gh->grow_gen; vm->global_ic[ic_slot].slot = (uint32_t)idx;
                v = gh->entries[idx].value;
            } else v = VM_UNDEF_VAL;
        } else {
            v = vm_globals_get_raw(vm, chunk->str_consts[name_idx]);
        }
    L_jit_lg_have:
        if (__builtin_expect(VM_IS_PTR(v) && (VM_PTR_TYPE(v) == VM_OBJ_TIED_SCALAR || VM_PTR_TYPE(v) == VM_OBJ_WEAKREF), 0)) {
            frame->ip = (VMInsn *)di; vm->stack_top = sp;
            return VM_JIT_INTERP;
        }
        J_PUSH(vm_val_copy(v));
        break;
    }
    case OP_STORE_GLOBAL: {
        uint16_t name_idx = di->a;
        VMHash *gh = vm_gh_for(vm, chunk->str_consts[name_idx]);
        VMValue v = stack[sp - 1];
        if (__builtin_expect(!vm->is_worker_vm && !vm->workers_head && gh != NULL && chunk->str_const_vals != NULL, 1)) {
            uint32_t ic_slot = (uint32_t)(((uintptr_t)di >> 3) & 63);
            VMString *nstr = VM_AS_STR(chunk->str_const_vals[name_idx]);
            const VMConstString *ncs = (const VMConstString*)((const char*)nstr - offsetof(VMConstString, str));
            if (__builtin_expect(vm->global_ic[ic_slot].ip == di && vm->global_ic[ic_slot].globals == gh &&
                                 vm->global_ic[ic_slot].gen == gh->grow_gen, 1)) {
                uint32_t es = vm->global_ic[ic_slot].slot;
                VMHashEntry *e = &gh->entries[es];
                if (__builtin_expect(es < (uint32_t)gh->capacity && e->occupied == 1 &&
                                     e->hash == ncs->hash && e->keylen == nstr->len &&
                                     memcmp(e->key, nstr->data, nstr->len) == 0, 1)) {
                    if (__builtin_expect(VM_IS_PTR(e->value) && VM_PTR_TYPE(e->value) == VM_OBJ_TIED_SCALAR, 0)) {
                        frame->ip = (VMInsn *)di; vm->stack_top = sp; return VM_JIT_INTERP;
                    }
                    sp--;
                    e->value = v;
                    break;
                }
            }
            int32_t idx = vm_hash_slot_idx_nh(gh, nstr->data, nstr->len, ncs->hash);
            if (idx >= 0) {
                if (__builtin_expect(VM_IS_PTR(gh->entries[idx].value) && VM_PTR_TYPE(gh->entries[idx].value) == VM_OBJ_TIED_SCALAR, 0)) {
                    frame->ip = (VMInsn *)di; vm->stack_top = sp; return VM_JIT_INTERP;
                }
                sp--;
                vm->global_ic[ic_slot].ip = di; vm->global_ic[ic_slot].globals = gh;
                vm->global_ic[ic_slot].gen = gh->grow_gen; vm->global_ic[ic_slot].slot = (uint32_t)idx;
                gh->entries[idx].value = v;
            } else {
                sp--;
                uint32_t ins = vm_hash_set_nh_slot(gh, nstr->data, nstr->len, ncs->hash, v);
                if (ins != UINT32_MAX) {
                    vm->global_ic[ic_slot].ip = di; vm->global_ic[ic_slot].globals = gh;
                    vm->global_ic[ic_slot].gen = gh->grow_gen; vm->global_ic[ic_slot].slot = ins;
                }
            }
        } else {
            const char *name = chunk->str_consts[name_idx];
            VMValue cur = vm_globals_get(vm, name);
            if (__builtin_expect(VM_IS_PTR(cur) && VM_PTR_TYPE(cur) == VM_OBJ_TIED_SCALAR, 0)) {
                frame->ip = (VMInsn *)di; vm->stack_top = sp; return VM_JIT_INTERP;
            }
            sp--;
            vm_globals_set(vm, name, v);
        }
        break;
    }
    case OP_BUILTIN: {
        VMValue exc = VM_UNDEF_VAL;
        vm->stack_top = sp;
        if (vm_exec_builtin(vm, di->a, di->flags, &exc)) {
            vm->pending_exc = exc;
            vm->has_pending_exc = 1;
            return VM_JIT_EXC;
        }
        return VM_JIT_OK;
    }
    /* The cases below handle only the plain case and leave everything
     * else (tied scalars, weak refs, ...) to the interpreter's handler:
     * `goto jit_step_bail` before the stack is touched. */
    case OP_DEFINED: {
        VMValue v = stack[sp - 1];
        if (__builtin_expect(vm->has_weakrefs && VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_WEAKREF, 0))
            goto jit_step_bail;
        stack[sp - 1] = VM_MAKE_INT_FAST(!VM_IS_UNDEF(v));
        break;
    }
    case OP_LOAD_CAPTURE_CELL: {
        uint16_t idx = di->a;
        J_PUSH((frame->closure && idx < (uint16_t)frame->closure->capture_count)
               ? frame->closure->captures[idx] : VM_UNDEF_VAL);
        break;
    }
    case OP_LOAD_CAPTURE: {
        uint16_t idx = di->a;
        if (!frame->closure || idx >= (uint16_t)frame->closure->capture_count) { J_PUSH(VM_UNDEF_VAL); break; }
        VMValue cv = frame->closure->captures[idx];
        VMValue v = (VM_IS_PTR(cv) && VM_PTR_TYPE(cv) == VM_OBJ_CELL) ? VM_AS_CELL(cv)->val : cv;
        if (VM_IS_PTR(v) && (VM_PTR_TYPE(v) == VM_OBJ_TIED_SCALAR || VM_PTR_TYPE(v) == VM_OBJ_WEAKREF))
            goto jit_step_bail;
        J_PUSH(vm_val_copy(v));
        break;
    }
    case OP_STORE_CAPTURE: {
        uint16_t idx = di->a;
        if (!frame->closure || idx >= (uint16_t)frame->closure->capture_count) { (void)J_POP(); break; }
        VMValue cv = frame->closure->captures[idx];
        if (VM_IS_PTR(cv) && VM_PTR_TYPE(cv) == VM_OBJ_CELL) {
            VMValue old_cv = VM_AS_CELL(cv)->val;
            if (VM_IS_PTR(old_cv) && VM_PTR_TYPE(old_cv) == VM_OBJ_TIED_SCALAR) goto jit_step_bail;
            VM_AS_CELL(cv)->val = J_POP();
        } else {
            if (VM_IS_PTR(cv) && VM_PTR_TYPE(cv) == VM_OBJ_TIED_SCALAR) goto jit_step_bail;
            frame->closure->captures[idx] = J_POP();
        }
        break;
    }
    case OP_SPACESHIP: {
        VMValue b2 = stack[sp - 1], a2 = stack[sp - 2];
        int cmp;
        if (VM_BOTH_INT(a2, b2)) {
            int64_t av = VM_INT_VAL(a2), bv = VM_INT_VAL(b2);
            cmp = av < bv ? -1 : (av > bv ? 1 : 0);
        } else if (VM_IS_FLOAT(a2) && VM_IS_FLOAT(b2)) {
            double av = VM_FLOAT_VAL(a2), bv = VM_FLOAT_VAL(b2);
            cmp = av < bv ? -1 : (av > bv ? 1 : 0);
        } else goto jit_step_bail;
        sp -= 2;
        J_PUSH(VM_MAKE_INT_FAST(cmp));
        break;
    }
    case OP_HASH_GET: {
        /* plain hash, string key (strings are never blessed, so no key
         * overload applies) */
        VMValue key = stack[sp - 1], hash = stack[sp - 2];
        if (!(VM_IS_PTR(hash) && VM_PTR_TYPE(hash) == VM_OBJ_HASH)) goto jit_step_bail;
        VMHash *h = VM_AS_HASH(hash);
        if (!VM_IS_UNDEF(h->tied_obj)) goto jit_step_bail;
        if (!(VM_IS_PTR(key) && VM_PTR_TYPE(key) == VM_OBJ_STR)) goto jit_step_bail;
        VMString *kstr = VM_AS_STR(key);
        uint32_t kh = (kstr->hdr.gc_immortal == 2)
                      ? ((const VMConstString*)((const char*)kstr - offsetof(VMConstString, str)))->hash
                      : vm_hash_fn_n(kstr->data, kstr->len);
        VMValue *slot = vm_hash_slot_ptr_nh(h, kstr->data, kstr->len, kh);
        VMValue r = slot ? *slot : VM_UNDEF_VAL;
        if (VM_IS_PTR(r) && VM_PTR_TYPE(r) == VM_OBJ_WEAKREF) goto jit_step_bail;
        sp -= 2;
        J_PUSH(vm_val_copy(r));
        vm_temp_free(&key);
        break;
    }
    case OP_HASH_SET: {
        VMValue key = stack[sp - 2], hash = stack[sp - 3];
        if (!(VM_IS_PTR(hash) && VM_PTR_TYPE(hash) == VM_OBJ_HASH)) goto jit_step_bail;
        VMHash *h = VM_AS_HASH(hash);
        if (!VM_IS_UNDEF(h->tied_obj)) goto jit_step_bail;
        if (!(VM_IS_PTR(key) && VM_PTR_TYPE(key) == VM_OBJ_STR)) goto jit_step_bail;
        VMValue v = stack[sp - 1];
        sp -= 3;
        VMString *kstr = VM_AS_STR(key);
        uint32_t kh = (kstr->hdr.gc_immortal == 2)
                      ? ((const VMConstString*)((const char*)kstr - offsetof(VMConstString, str)))->hash
                      : vm_hash_fn_n(kstr->data, kstr->len);
        g_vm_key_bytes_hint = kstr->hdr.is_utf8 ? NULL : kstr->data;
        vm_hash_set_nh(h, kstr->data, kstr->len, kh, v);
        g_vm_key_bytes_hint = NULL;
        vm_val_free(&key);
        break;
    }
    case OP_HASH_BLESS: {
        /* bless(HASH, "Class"); other targets and class values in the interpreter */
        VMValue cn = stack[sp - 1], hash = stack[sp - 2];
        if (!(VM_IS_PTR(hash) && VM_PTR_TYPE(hash) == VM_OBJ_HASH)) goto jit_step_bail;
        if (!(VM_IS_PTR(cn) && VM_PTR_TYPE(cn) == VM_OBJ_STR)) goto jit_step_bail;
        const char *cname = VM_AS_STR(cn)->data;
        if (cname[0]) {
            uint32_t cid = vm_program_intern_class(vm->program, cname);
            VMHash *h = VM_AS_HASH(hash);
            free(h->class_name);
            h->class_name = strdup(cname);
            h->class_id = cid;
            h->class_prog_id = vm->program->prog_id;
        }
        sp -= 1;   /* the hash stays on the stack */
        vm_val_free(&cn);
        break;
    }
    default:
    jit_step_bail:
        /* Signal unhandled opcode to bail safely at cur instruction */
        frame->ip = (VMInsn *)di;
        vm->stack_top = sp;
        return VM_JIT_INTERP;
    }

    vm->stack_top = sp;
    return VM_JIT_OK;
    #undef J_PUSH
    #undef J_POP
}

static const void * const *g_vm_dt_table = NULL;

static __attribute__((noinline)) int vm_jit_call_slow(VM *vm, const VMInsn *di) {
    uint16_t fidx = di->a;
    int argc = di->flags;
    VMChunk *callee = &vm->program->funcs[fidx];
    uint8_t cctx = vm->pending_call_context;
    vm->pending_call_context = 0;

    if (callee->jit_state == 0 && ++callee->jit_hot >= JIT_HOT_THRESHOLD) {
        vm_jit_try_compile(vm->program, callee);
    }
    if (vm->recursion_limit > 0 && (int)vm->frame_count >= vm->recursion_limit) {
        fprintf(stderr, "Deep recursion detected (limit %d) — see core::set_recursion_limit\n", vm->recursion_limit);
        exit(1);
    }
    size_t sp = vm->stack_top;
    size_t arg_base = sp - (size_t)argc;
    VMValue *cargs = vm->stack + arg_base;
    VMValue ret;
    if (callee->has_variadic) {
        int fixed = callee->fixed_param_count; if (fixed < 0) fixed = 0;
        int vc = argc - fixed;
        VMArray *va = vm_array_new(vc > 0 ? vc : 4);
        for (int i = fixed; i < argc; i++) vm_array_push(va, cargs[i]);
        VMValue pbuf[16]; int pargc = fixed + 1;
        VMValue *pargs = (pargc > 16) ? malloc((size_t)pargc * sizeof(VMValue)) : pbuf;
        for (int i = 0; i < fixed; i++) pargs[i] = (i < argc) ? cargs[i] : VM_UNDEF_VAL;
        pargs[fixed] = VM_MAKE_PTR(va);
        vm->stack_top = arg_base;
        vm->pending_call_context = cctx;
        ret = vm_execute_call_cl(vm, fidx, NULL, pargs, pargc, argc);
        if (pargs != pbuf) free(pargs);
    } else {
        VMValue abuf[16];
        VMValue *acopy = (argc > 16) ? malloc((size_t)argc * sizeof(VMValue)) : abuf;
        if (argc > 0) memcpy(acopy, cargs, (size_t)argc * sizeof(VMValue));
        vm->stack_top = arg_base;
        vm->pending_call_context = cctx;
        ret = vm_execute_call_cl(vm, fidx, NULL, acopy, argc, argc);
        if (acopy != abuf) free(acopy);
    }
    if (vm->has_pending_exc) return VM_JIT_EXC;
    if (vm->stack_top >= vm->stack_cap) vm_stack_grow(vm, vm->stack_top);
    vm->stack[vm->stack_top++] = ret;
    return VM_JIT_OK;
}

/* OP_CALL_CLOSURE from JIT code (stack: args..., closure). A plain VM
 * closure runs through vm_execute_call_cl (which enters its JIT code when
 * compiled); anything else -- bound or variadic closures, native code,
 * a non-code value -- resumes the interpreter at this instruction. */
static __attribute__((noinline)) int vm_jit_call_closure_slow(VM *vm, const VMInsn *di) {
    int argc = di->flags;
    size_t sp = vm->stack_top;
    VMValue cval = vm->stack[sp - 1];
    if (!(VM_IS_PTR(cval) && VM_PTR_TYPE(cval) == VM_OBJ_CLOSURE)) goto bail;
    VMClosure *cl = VM_AS_CLOSURE(cval);
    VMChunk *callee = &vm->program->funcs[cl->func_idx];
    if (cl->has_bound_arg || callee->has_variadic) goto bail;
    if (vm->recursion_limit > 0 && (int)vm->frame_count >= vm->recursion_limit) goto bail;
    uint8_t cctx = vm->pending_call_context;
    vm->pending_call_context = 0;
    size_t arg_base = sp - 1 - (size_t)argc;
    VMValue abuf[16];
    VMValue *acopy = (argc > 16) ? malloc((size_t)argc * sizeof(VMValue)) : abuf;
    if (argc > 0) memcpy(acopy, vm->stack + arg_base, (size_t)argc * sizeof(VMValue));
    /* the closure stays reachable for the GC through the callee frame */
    vm->stack_top = arg_base;
    vm->pending_call_context = cctx;
    VMValue ret = vm_execute_call_cl(vm, cl->func_idx, cl, acopy, argc, argc);
    if (acopy != abuf) free(acopy);
    if (vm->has_pending_exc) return VM_JIT_EXC;
    if (vm->stack_top >= vm->stack_cap) vm_stack_grow(vm, vm->stack_top);
    vm->stack[vm->stack_top++] = ret;
    return VM_JIT_OK;
bail:
    vm->frames[vm->frame_count - 1].ip = (VMInsn *)di;
    return VM_JIT_INTERP;
}

/* Fast OP_CALL_CLOSURE from JIT code: when the value is a plain VM closure
 * whose function is JIT-compiled, push its frame exactly as the
 * interpreter's L_do_op_call_closure does and return the callee's JIT
 * entry for a direct call (the callee's OP_RETURN restores the caller's
 * registers). NULL: take vm_jit_call_closure_slow. */
static __attribute__((noinline)) void *vm_jit_closure_prep(VM *vm, const VMInsn *di) {
    int argc = di->flags;
    size_t sp = vm->stack_top;
    VMValue cval = vm->stack[sp - 1];
    if (!(VM_IS_PTR(cval) && VM_PTR_TYPE(cval) == VM_OBJ_CLOSURE)) return NULL;
    VMClosure *cl = VM_AS_CLOSURE(cval);
    VMChunk *callee = &vm->program->funcs[cl->func_idx];
    if (cl->has_bound_arg || callee->has_variadic) return NULL;
    if (callee->jit_state == 0 && ++callee->jit_hot >= JIT_HOT_THRESHOLD) {
        vm_jit_try_compile(vm->program, callee);
    }
    if (__atomic_load_n(&callee->jit_state, __ATOMIC_ACQUIRE) != 2) return NULL;
    void *entry = __atomic_load_n(&callee->jit_code, __ATOMIC_ACQUIRE);
    if (!entry || !callee->dt_code || callee->dt_code_len_src != callee->code_len) return NULL;
    if (vm->recursion_limit > 0 && (int)vm->frame_count >= vm->recursion_limit) return NULL;
    if (vm->frame_count >= vm->frame_cap) return NULL;
    size_t arg_base = sp - 1 - (size_t)argc;
    int lcnt = callee->local_count + 1;
    if (arg_base + (size_t)lcnt >= vm->stack_cap) return NULL;
    VMFrame *nf = &vm->frames[vm->frame_count++];
    nf->chunk = callee;
    nf->ip = callee->dt_code;
    nf->stack_base = arg_base;
    nf->local_base = arg_base;
    nf->closure = cl;            /* keeps the closure alive for the GC */
    nf->global_save_depth = vm->global_save_count;
    nf->passed_argc = argc;
    nf->call_context = vm->pending_call_context;
    vm->pending_call_context = 0;
    int pmax = lcnt;
    if (callee->fixed_param_count >= 0 && callee->fixed_param_count < pmax) pmax = callee->fixed_param_count;
    int zf = argc < pmax ? argc : pmax;
    if (zf < lcnt) memset(vm->stack + arg_base + zf, 0, (size_t)(lcnt - zf) * sizeof(VMValue));
    vm->stack_top = arg_base + (size_t)lcnt;
    return entry;
}

/* OP_CALL from JIT code when the inline direct call does not apply (a
 * variadic callee, for one): push the callee's frame exactly as the
 * interpreter's L_do_op_call does and return its JIT entry, or NULL for
 * vm_jit_call_slow (callee not compiled, frame/stack growth, recursion
 * limit). */
static __attribute__((noinline)) void *vm_jit_call_prep(VM *vm, const VMInsn *di) {
    uint16_t fidx = di->a;
    int argc = di->flags;
    VMChunk *callee = &vm->program->funcs[fidx];
    if (callee->jit_state == 0 && ++callee->jit_hot >= JIT_HOT_THRESHOLD) {
        vm_jit_try_compile(vm->program, callee);
    }
    if (__atomic_load_n(&callee->jit_state, __ATOMIC_ACQUIRE) != 2) return NULL;
    void *entry = __atomic_load_n(&callee->jit_code, __ATOMIC_ACQUIRE);
    if (!entry || !callee->dt_code || callee->dt_code_len_src != callee->code_len) return NULL;
    if (vm->recursion_limit > 0 && (int)vm->frame_count >= vm->recursion_limit) return NULL;
    if (vm->frame_count >= vm->frame_cap) return NULL;
    size_t sp = vm->stack_top;
    size_t arg_base = sp - (size_t)argc;
    int lcnt = callee->local_count + 1;
    if (arg_base + (size_t)lcnt >= vm->stack_cap) return NULL;
    VMFrame *nf = &vm->frames[vm->frame_count++];
    nf->chunk = callee;
    nf->ip = callee->dt_code;
    nf->stack_base = arg_base;
    nf->closure = NULL;
    nf->global_save_depth = vm->global_save_count;
    nf->passed_argc = argc;
    nf->call_context = vm->pending_call_context;
    vm->pending_call_context = 0;
    if (!callee->has_variadic) {
        int pmax = lcnt;
        if (callee->fixed_param_count >= 0 && callee->fixed_param_count < pmax) pmax = callee->fixed_param_count;
        int zf = argc < pmax ? argc : pmax;
        if (zf < lcnt) memset(vm->stack + arg_base + zf, 0, (size_t)(lcnt - zf) * sizeof(VMValue));
        nf->local_base = arg_base;
        sp = arg_base + (size_t)lcnt;
    } else {
        vm->stack_top = sp;
        vm_call_variadic_setup(vm, nf, callee, argc, &sp);
    }
    vm->stack_top = sp;
    return entry;
}

static __attribute__((noinline)) int vm_jit_tail_call_slow(VM *vm, const VMInsn *di) {
    /* Sync current frame for interpreter OP_TAIL_CALL fallback cleanly */
    vm->frames[vm->frame_count - 1].ip = (VMInsn *)di;
    return VM_JIT_INTERP;
}

/* Returns 3 if native callee set up cleanly (callee->jit_code ready in *out_callee),
 * 0 if fully executed & result pushed, 1 to bail to interpreter, 2 on exception. */
static __attribute__((noinline)) int vm_jit_mcall_step(VM *vm, const VMInsn *di, int allow_field, VMChunk **out_callee) {
    uint16_t midx = di->a;
    int argc = di->flags;
    VMFrame *caller = &vm->frames[vm->frame_count - 1];
    const char *method = caller->chunk->str_consts[midx];
    size_t sp = vm->stack_top;
    VMValue self = vm->stack[sp - (size_t)argc - 1];
    uint8_t mcall_ctx = vm->pending_call_context;
    vm->pending_call_context = 0;

    VMHash *h = (VM_IS_PTR(self) && VM_PTR_TYPE(self) == VM_OBJ_HASH) ? VM_AS_HASH(self) : NULL;
    uint32_t cur_pid = vm->program->prog_id;
    uint32_t obj_cid = (h && h->class_id && h->class_prog_id == cur_pid) ? h->class_id : 0;
    int fidx = -1;
    if (obj_cid != 0) {
        uint32_t ic_slot = (uint32_t)(((uintptr_t)di >> 3) & 255);
        if (vm->method_ic[ic_slot][0].ip == di && vm->method_ic[ic_slot][0].class_id == obj_cid && vm->method_ic[ic_slot][0].prog_id == cur_pid) {
            fidx = vm->method_ic[ic_slot][0].func_idx;
            goto L_jit_mcall_fidx;
        }
        if (vm->method_ic[ic_slot][1].ip == di && vm->method_ic[ic_slot][1].class_id == obj_cid && vm->method_ic[ic_slot][1].prog_id == cur_pid) {
            fidx = vm->method_ic[ic_slot][1].func_idx;
            goto L_jit_mcall_fidx;
        }
    }
    if (!h && VM_IS_PTR(self) && VM_PTR_TYPE(self) == VM_OBJ_NATIVE_SV) {
        vm->pending_call_context = mcall_ctx;
        caller->ip = (VMInsn *)di;
        return VM_JIT_INTERP;
    }
    const char *cls_name = h ? h->class_name : vm_obj_class_name(self);
    int is_hash_blessed = (cls_name != NULL);
    if (!cls_name) {
        if (VM_IS_PTR(self) && VM_PTR_TYPE(self) == VM_OBJ_STR) cls_name = VM_AS_STR(self)->data;
        else if (VM_IS_PTR(self) && VM_PTR_TYPE(self) == VM_OBJ_STRBUF) cls_name = VM_AS_STRBUF(self)->data;
    }
    if (!cls_name || !cls_name[0] || strcmp(method, "isa") == 0 || strcmp(method, "can") == 0) {
        vm->pending_call_context = mcall_ctx;
        caller->ip = (VMInsn *)di;
        return VM_JIT_INTERP;
    }
    if (is_hash_blessed) {
        fidx = vm_find_method_cached(vm, cls_name, method);
        if (fidx >= 0) {
            if (!obj_cid) {
                obj_cid = vm_program_intern_class(vm->program, cls_name);
                if (h && !vm->is_worker_vm && !vm->workers_head) {
                    h->class_id = obj_cid; h->class_prog_id = cur_pid;
                }
            }
            if (obj_cid) {
                uint32_t ic_slot = (uint32_t)(((uintptr_t)di >> 3) & 255);
                if (!(vm->method_ic[ic_slot][0].ip == di && vm->method_ic[ic_slot][0].prog_id == cur_pid &&
                      vm->method_ic[ic_slot][0].class_id == obj_cid)) {
                    if (vm->method_ic[ic_slot][0].ip == di && vm->method_ic[ic_slot][0].prog_id == cur_pid) {
                        vm->method_ic[ic_slot][1] = vm->method_ic[ic_slot][0];
                    }
                    vm->method_ic[ic_slot][0].ip = di;
                    vm->method_ic[ic_slot][0].prog_id = cur_pid;
                    vm->method_ic[ic_slot][0].class_id = obj_cid;
                    vm->method_ic[ic_slot][0].func_idx = fidx;
                }
            }
        }
    } else {
        fidx = vm_find_method(vm->program, cls_name, method);
    }

L_jit_mcall_fidx:
    if (fidx < 0) {
        (void)allow_field;
        vm->pending_call_context = mcall_ctx;
        caller->ip = (VMInsn *)di;
        return VM_JIT_INTERP;
    }
    if (vm->recursion_limit > 0 && (int)vm->frame_count >= vm->recursion_limit) {
        fprintf(stderr, "Deep recursion detected (limit %d) — see core::set_recursion_limit\n", vm->recursion_limit);
        exit(1);
    }
    VMChunk *callee = &vm->program->funcs[fidx];
    if (__builtin_expect(!__atomic_load_n(&callee->dt_code, __ATOMIC_ACQUIRE) ||
                         callee->dt_code_len_src != callee->code_len, 0)) {
        const void * const *dtp = __atomic_load_n(&g_vm_dt_table, __ATOMIC_ACQUIRE);
        if (!dtp) {
            vm->pending_call_context = mcall_ctx;
            caller->ip = (VMInsn *)di;
            return VM_JIT_INTERP;
        }
        vm_chunk_ensure_dt(vm->program, callee, dtp);
    }
    if (__builtin_expect(callee->jit_state == 0 && ++callee->jit_hot >= JIT_HOT_THRESHOLD, 0)) {
        vm_jit_try_compile(vm->program, callee);
    }
    if (__atomic_load_n(&callee->jit_state, __ATOMIC_ACQUIRE) == 2 && !callee->has_variadic) {
        if (__builtin_expect(vm->frame_count >= vm->frame_cap, 0)) vm_frames_grow(vm);
        VMFrame *nf = &vm->frames[vm->frame_count++];
        nf->chunk = callee;
        nf->ip = callee->dt_code;
        size_t arg_base = sp - (size_t)argc - 1;
        nf->stack_base = arg_base;
        nf->local_base = arg_base;
        nf->closure = NULL;
        nf->global_save_depth = vm->global_save_count;
        nf->passed_argc = argc + 1;
        nf->call_context = mcall_ctx;
        int lcnt = callee->local_count + 1;
        int pmax = lcnt;
        if (callee->fixed_param_count >= 0 && callee->fixed_param_count < pmax) pmax = callee->fixed_param_count;
        int zf = (argc + 1 < pmax) ? (argc + 1) : pmax;
        if (arg_base + (size_t)lcnt >= vm->stack_cap) vm_stack_grow(vm, arg_base + (size_t)lcnt);
        if (zf < lcnt) {
            memset(vm->stack + arg_base + zf, 0, (size_t)(lcnt - zf) * sizeof(VMValue));
        }
        vm->stack_top = arg_base + (size_t)lcnt;
        *out_callee = callee;
        return 3; /* native call ready! */
    }
    /* Execute non-JIT / variadic method via vm_execute_call_cl cleanly */
    size_t arg_base = sp - (size_t)argc - 1;
    int total = argc + 1;
    VMValue *cargs = vm->stack + arg_base;
    VMValue ret;
    if (callee->has_variadic) {
        int fixed = callee->fixed_param_count; if (fixed < 0) fixed = 0;
        int vc = total - fixed;
        VMArray *va = vm_array_new(vc > 0 ? vc : 4);
        for (int i = fixed; i < total; i++) vm_array_push(va, cargs[i]);
        VMValue pbuf[16]; int pargc = fixed + 1;
        VMValue *pargs = (pargc > 16) ? malloc((size_t)pargc * sizeof(VMValue)) : pbuf;
        for (int i = 0; i < fixed; i++) pargs[i] = (i < total) ? cargs[i] : VM_UNDEF_VAL;
        pargs[fixed] = VM_MAKE_PTR(va);
        vm->stack_top = arg_base;
        vm->pending_call_context = mcall_ctx;
        ret = vm_execute_call_cl(vm, fidx, NULL, pargs, pargc, total);
        if (pargs != pbuf) free(pargs);
    } else {
        VMValue abuf[16];
        VMValue *acopy = (total > 16) ? malloc((size_t)total * sizeof(VMValue)) : abuf;
        memcpy(acopy, cargs, (size_t)total * sizeof(VMValue));
        vm->stack_top = arg_base;
        vm->pending_call_context = mcall_ctx;
        ret = vm_execute_call_cl(vm, fidx, NULL, acopy, total, total);
        if (acopy != abuf) free(acopy);
    }
    if (vm->has_pending_exc) return VM_JIT_EXC;
    if (vm->stack_top >= vm->stack_cap) vm_stack_grow(vm, vm->stack_top);
    vm->stack[vm->stack_top++] = ret;
    return VM_JIT_OK;
}

static __attribute__((noinline)) int vm_jit_return_slow(VM *vm, VMValue res) {
    VMFrame *f = &vm->frames[vm->frame_count - 1];
    if (vm->global_save_count > f->global_save_depth) {
        vm->gc_pause++;
        vm_unwind_global_saves(vm, f->global_save_depth);
        vm->gc_pause--;
    }
    if (vm->exc_top > 0) {
        while (vm->exc_top > 0 && vm->exc_stack[vm->exc_top - 1].frame_count >= vm->frame_count) {
            vm->exc_top--;
        }
    }
    size_t sb = vm->frames[vm->frame_count - 1].stack_base;
    vm->frame_count--;
    if (vm->frame_count == vm->jit_stop_base) {
        vm->jit_ret_val = res;
        vm->stack_top = sb;
        return 1; /* stop JIT execution */
    }
    vm->stack[sb] = res;
    vm->stack_top = sb + 1;
    if (vm->gc_count >= vm->gc_threshold && vm->exec_base == 0 && vm->gc_pause == 0) {
        vm_gc_collect(vm);
        if (vm->has_pending_exc) return VM_JIT_EXC;
    }
    return 0; /* continue returning in JIT */
}

static __attribute__((noinline)) int vm_jit_return_gc_only(VM *vm) {
    if (__builtin_expect(vm->is_worker_vm, 0)) {
        vm_worker_poll_gc(vm);
        return VM_JIT_OK;
    }
    if (vm->gc_count >= vm->gc_threshold && vm->exec_base == 0 && vm->gc_pause == 0) {
        vm_gc_collect(vm);
        if (vm->has_pending_exc) return VM_JIT_EXC;
    }
    return VM_JIT_OK;
}

/* Emit a call to vm_jit_op_step(vm, di), syncing state & checking exc/interp */
static void jb_emit_reload_and_ret_check(JitBuf *b) {
    jb_push(b, JR_RAX);
    jb_reload_all(b);
    jb_pop(b, JR_RAX);
    jb_rr(b, 0, 0x85, JR_RAX, JR_RAX); /* test eax, eax */
    uint32_t j_ok = jb_jcc_placeholder(b, 0x84);
    /* Return eax (1 = INTERP, 2 = EXC) cleanly */
    jb_pop(b, JR_RBP);
    jb_u8(b, 0xC3);
    jb_patch_here(b, j_ok);
}

static void jb_emit_step_helper(JitBuf *b, const VMInsn *di, const VMInsn *next_di, int can_bail) {
    jb_sync_state(b, next_di);
    jb_mov_rr(b, JR_RDI, JR_RBX);
    jb_mov_imm64(b, JR_RSI, (uint64_t)(uintptr_t)di);
    jb_call_c(b, (void *)vm_jit_op_step);
    (void)can_bail;
    jb_emit_reload_and_ret_check(b);
}

/* Emit 50-bit int check on RAX; if fits, tag & push to RAX. Else fall back via helper */
static uint32_t jb_tag_rax_int50_or_jump(JitBuf *b) {
    jb_mov_rr(b, JR_RCX, JR_RAX);
    jb_shift_ri8(b, 1, 4, JR_RCX, 14); /* shl rcx, 14 */
    jb_shift_ri8(b, 1, 7, JR_RCX, 14); /* sar rcx, 14 */
    jb_rr(b, 1, 0x39, JR_RAX, JR_RCX); /* cmp rcx, rax */
    uint32_t j_fail = jb_jcc_placeholder(b, 0x85);
    jb_mov_imm64(b, JR_RDX, VM_TAG_INT_BASE);
    jb_rr(b, 1, 0x09, JR_RDX, JR_RAX); /* or rax, rdx */
    return j_fail;
}

/* Shared backedge signal + GC poll helper (polls when gc_count >= gc_threshold or (sig_poll & 255) == 0) */
static void jb_emit_backedge_poll(JitBuf *b, const VMInsn *sync_di) {
    jb_mem(b, 1, 0x8B, JR_RAX, JR_RBX, (int32_t)offsetof(VM, gc_count));
    jb_mem(b, 1, 0x3B, JR_RAX, JR_RBX, (int32_t)offsetof(VM, gc_threshold));
    uint32_t j_gc = jb_jcc_placeholder(b, 0x83); /* jae slow */
    jb_mem(b, 0, 0xFF, 0, JR_RBX, (int32_t)offsetof(VM, jit_sig_poll)); /* inc dword */
    jb_mem(b, 0, 0xF7, 0, JR_RBX, (int32_t)offsetof(VM, jit_sig_poll)); jb_u32(b, 255);
    uint32_t j_no_poll = jb_jcc_placeholder(b, 0x85); /* jnz fast */
    jb_patch_here(b, j_gc);
    jb_sync_state(b, sync_di);
    jb_mov_rr(b, JR_RDI, JR_RBX);
    jb_call_c(b, (void *)vm_jit_poll_sig_and_gc);
    jb_push(b, JR_RAX); jb_reload_all(b); jb_pop(b, JR_RAX);
    jb_rr(b, 0, 0x85, JR_RAX, JR_RAX);
    uint32_t j_ok = jb_jcc_placeholder(b, 0x84);
    jb_pop(b, JR_RBP); jb_u8(b, 0xC3);
    jb_patch_here(b, j_ok);
    jb_patch_here(b, j_no_poll);
}

/* Shared primary-bucket hash probe helper for OP_HASH_GET_CONST / OP_HASH_SET_CONST.
 * Leaves VMHashEntry* in JR_RCX on hit, or jumps to *fails++ (max 12) on miss/slow. */
static int jb_emit_hash_primary_probe(JitBuf *b, const VMInsn *di, VMChunk *c, int hash_stack_off, uint32_t *fails) {
    (void)sizeof(char[(sizeof(VMHashEntry) == 32) ? 1 : -1]);
    if (!c->str_const_vals || di->a >= c->str_const_count) return -1;
    VMString *kstr = VM_AS_STR(c->str_const_vals[di->a]);
    uint32_t klen = kstr->len;
    if (klen > 16) return -1;
    uint32_t khash = ((const VMConstString *)((const char *)kstr - offsetof(VMConstString, str)))->hash;
    const uint8_t *kd = (const uint8_t *)kstr->data;

    int nf = 0;
    jb_sib8(b, 1, 0x8B, JR_RAX, JR_R14, JR_R15, hash_stack_off);
    jb_rr(b, 1, 0x85, JR_RAX, JR_RAX);
    fails[nf++] = jb_jcc_placeholder(b, 0x84);
    jb_mov_rr(b, JR_RCX, JR_RAX);
    jb_shift_ri8(b, 1, 5, JR_RCX, 50);
    fails[nf++] = jb_jcc_placeholder(b, 0x85);
    jb_u8(b, 0x0F); jb_u8(b, 0xB6); jb_u8(b, 0x08); /* movzx ecx, byte [rax] */
    jb_alu_ri32(b, 7, JR_RCX, VM_OBJ_HASH);
    fails[nf++] = jb_jcc_placeholder(b, 0x85);
    jb_mem(b, 1, 0x83, 7, JR_RAX, (int32_t)offsetof(VMHash, tied_obj)); jb_u8(b, 0);
    fails[nf++] = jb_jcc_placeholder(b, 0x85);
    /* slot = index ? index[khash & index_mask] : 0 (inline hashes are
     * scanned linearly; slot 0 is the first-probe guess) */
    jb_mem(b, 1, 0x8B, JR_RDX, JR_RAX, (int32_t)offsetof(VMHash, index)); /* mov rdx, [rax+index] */
    jb_rr(b, 1, 0x85, JR_RDX, JR_RDX);
    uint32_t j_small = jb_jcc_placeholder(b, 0x84); /* jz small */
    jb_mem(b, 0, 0x8B, JR_RCX, JR_RAX, (int32_t)offsetof(VMHash, index_mask)); /* mov ecx, [rax+mask] */
    jb_mov_imm32(b, JR_RSI, khash);
    jb_rr(b, 0, 0x21, JR_RSI, JR_RCX); /* and ecx, esi */
    jb_shift_ri8(b, 1, 4, JR_RCX, 2); /* shl rcx, 2 */
    jb_rr(b, 1, 0x01, JR_RDX, JR_RCX); /* add rcx, rdx */
    jb_mem(b, 1, 0x63, JR_RCX, JR_RCX, 0); /* movsxd rcx, dword [rcx] */
    jb_rr(b, 1, 0x85, JR_RCX, JR_RCX);
    fails[nf++] = jb_jcc_placeholder(b, 0x88); /* js: empty index slot */
    uint32_t j_have = jb_jmp_placeholder(b);
    jb_patch_here(b, j_small);
    jb_rr(b, 0, 0x31, JR_RCX, JR_RCX); /* xor ecx, ecx */
    jb_patch_here(b, j_have);
    jb_shift_ri8(b, 1, 4, JR_RCX, 5); /* shl rcx, 5 */
    jb_mem(b, 1, 0x8B, JR_RAX, JR_RAX, (int32_t)offsetof(VMHash, entries));
    jb_rr(b, 1, 0x01, JR_RAX, JR_RCX); /* add rcx, rax -> VMHashEntry* e */

    jb_mem(b, 0, 0x80, 7, JR_RCX, (int32_t)offsetof(VMHashEntry, occupied)); jb_u8(b, 1);
    fails[nf++] = jb_jcc_placeholder(b, 0x85);
    jb_mem(b, 0, 0x81, 7, JR_RCX, (int32_t)offsetof(VMHashEntry, hash)); jb_u32(b, khash);
    fails[nf++] = jb_jcc_placeholder(b, 0x85);
    jb_mem(b, 0, 0x81, 7, JR_RCX, (int32_t)offsetof(VMHashEntry, keylen)); jb_u32(b, klen);
    fails[nf++] = jb_jcc_placeholder(b, 0x85);

    if (klen > 0) {
        jb_mem(b, 1, 0x8B, JR_RSI, JR_RCX, (int32_t)offsetof(VMHashEntry, key));
        uint32_t off = 0;
        while (off + 4 <= klen) {
            uint32_t v32; memcpy(&v32, kd + off, 4);
            jb_mem(b, 0, 0x81, 7, JR_RSI, (int32_t)off); jb_u32(b, v32);
            fails[nf++] = jb_jcc_placeholder(b, 0x85);
            off += 4;
        }
        if (off + 2 <= klen) {
            uint16_t v16; memcpy(&v16, kd + off, 2);
            jb_u8(b, 0x66); jb_mem(b, 0, 0x81, 7, JR_RSI, (int32_t)off); jb_u16(b, v16);
            fails[nf++] = jb_jcc_placeholder(b, 0x85);
            off += 2;
        }
        if (off < klen) {
            jb_mem(b, 0, 0x80, 7, JR_RSI, (int32_t)off); jb_u8(b, kd[off]);
            fails[nf++] = jb_jcc_placeholder(b, 0x85);
        }
    }
    return nf;
}

/* Shared bailout stub builder at end of chunk: returns eax (1 or 2) with rbp popped */
static void vm_jit_compile_chunk_locked(VMProgram *prog, VMChunk *c) {
    if (__builtin_expect(c->jit_state != 0, 0)) return;
    const char *no_jit = getenv("STRADA_NO_JIT");
    if (no_jit && no_jit[0] == '1') { c->jit_state = 3; return; }
    if (!c->dt_code || c->dt_len == 0 || c->dt_len > 16384 || !vm_jit_get_trampoline()) {
        c->jit_state = 3;
        return;
    }
    c->jit_state = 1;

    JitBuf jb = {0};
    JitBuf *b = &jb;
    b->offs = calloc(c->dt_len + 1, sizeof(uint32_t));
    if (!b->offs) { c->jit_state = 3; return; }

    /* Prologue: rsp % 16 == 8 -> push rbp -> rsp % 16 == 0 */
    jb_push(b, JR_RBP);

    int max_loc = c->local_count;
    int bail_ok = 1;

    for (size_t idx = 0; idx < c->dt_len; idx++) {
        b->offs[idx] = (uint32_t)b->len;
        const VMInsn *di = &c->dt_code[idx];
        const VMInsn *next_di = di + 1;
        uint8_t op = di->op;

        switch (op) {
        case OP_NOP:
            break;

        case OP_PUSH_UNDEF:
            jb_mov_imm32(b, JR_RAX, 0);
            jb_sp_push_rax(b);
            break;

        case OP_PUSH_I8:
        case OP_PUSH_I32: {
            uint64_t tv = VM_MAKE_INT_FAST(di->imm.i64);
            jb_mov_imm64(b, JR_RAX, tv);
            jb_sp_push_rax(b);
            break;
        }

        case OP_PUSH_INT: {
            int64_t iv = di->imm.i64;
            if (((uint64_t)iv + (1ULL << 49)) < (1ULL << 50)) {
                jb_mov_imm64(b, JR_RAX, ((uint64_t)iv) | VM_TAG_INT_BASE);
            } else {
                jb_mov_imm64(b, JR_RDI, (uint64_t)iv);
                jb_call_c(b, (void *)vm_box_int64);
            }
            jb_sp_push_rax(b);
            break;
        }

        case OP_PUSH_FLOAT: {
            uint64_t bits = 0;
            memcpy(&bits, &di->imm.f64, 8);
            VMValue fv = VM_MAKE_FLOAT(di->imm.f64);
            (void)bits;
            jb_mov_imm64(b, JR_RAX, fv);
            jb_sp_push_rax(b);
            break;
        }

        case OP_PUSH_STR: {
            if (di->a >= c->str_const_count) { bail_ok = 0; break; }
            VMValue sv = c->str_const_vals[di->a];
            jb_mov_imm64(b, JR_RAX, sv);
            jb_sp_push_rax(b);
            break;
        }

        case OP_POP:
            jb_alu_ri32(b, 5, JR_R15, 1); /* dec r15 */
            break;

        case OP_DUP: {
            jb_sib8(b, 1, 0x8B, JR_RAX, JR_R14, JR_R15, -8);
            /* Fast check: if (rax >> 50) != 0 || rax == 0, not ptr -> direct push! */
            jb_rr(b, 1, 0x85, JR_RAX, JR_RAX);
            uint32_t j_zero = jb_jcc_placeholder(b, 0x84);
            jb_mov_rr(b, JR_RCX, JR_RAX);
            jb_shift_ri8(b, 1, 5, JR_RCX, 50);
            uint32_t j_notptr = jb_jcc_placeholder(b, 0x85);
            /* Is ptr: check obj_type == VM_OBJ_STRBUF */
            jb_u8(b, 0x0F); jb_u8(b, 0xB6); jb_u8(b, 0x08); /* movzx ecx, byte [rax] */
            jb_alu_ri32(b, 7, JR_RCX, VM_OBJ_STRBUF);
            uint32_t j_not_sb = jb_jcc_placeholder(b, 0x85);
            jb_mem(b, 0, 0xC6, 0, JR_RAX, (int32_t)offsetof(VMStrBuf, shared)); jb_u8(b, 1); /* mov byte [rax+shared], 1 */
            jb_patch_here(b, j_zero);
            jb_patch_here(b, j_notptr);
            jb_patch_here(b, j_not_sb);
            jb_sp_push_rax(b);
            break;
        }

        case OP_LOAD_LOCAL: {
            uint16_t slot = di->a;
            if (slot > (uint16_t)max_loc) { bail_ok = 0; break; }
            jb_mem(b, 1, 0x8B, JR_RAX, JR_R13, (int32_t)slot * 8);
            /* Fast check: rax == 0 || (rax >> 50) != 0 => not ptr */
            jb_rr(b, 1, 0x85, JR_RAX, JR_RAX);
            uint32_t j_fast0 = jb_jcc_placeholder(b, 0x84);
            jb_mov_rr(b, JR_RCX, JR_RAX);
            jb_shift_ri8(b, 1, 5, JR_RCX, 50);
            uint32_t j_fast1 = jb_jcc_placeholder(b, 0x85);
            /* Heap ptr: read obj_type byte [rax] */
            jb_u8(b, 0x0F); jb_u8(b, 0xB6); jb_u8(b, 0x08);
            jb_alu_ri32(b, 7, JR_RCX, VM_OBJ_CELL);        uint32_t j_slow0 = jb_jcc_placeholder(b, 0x84);
            jb_alu_ri32(b, 7, JR_RCX, VM_OBJ_STRBUF);      uint32_t j_slow1 = jb_jcc_placeholder(b, 0x84);
            jb_alu_ri32(b, 7, JR_RCX, VM_OBJ_TIED_SCALAR); uint32_t j_slow2 = jb_jcc_placeholder(b, 0x84);
            jb_alu_ri32(b, 7, JR_RCX, VM_OBJ_WEAKREF);     uint32_t j_slow3 = jb_jcc_placeholder(b, 0x84);
            jb_patch_here(b, j_fast0);
            jb_patch_here(b, j_fast1);
            jb_sp_push_rax(b);
            uint32_t j_done = jb_jmp_placeholder(b);

            jb_patch_here(b, j_slow0); jb_patch_here(b, j_slow1);
            jb_patch_here(b, j_slow2); jb_patch_here(b, j_slow3);
            jb_sync_state(b, next_di);
            jb_mov_rr(b, JR_RDI, JR_RBX);
            jb_mov_imm64(b, JR_RSI, (uint64_t)(uintptr_t)di);
            jb_call_c(b, (void *)vm_jit_load_local_slow);
            jb_push(b, JR_RAX);
            jb_reload_all(b);
            jb_pop(b, JR_RAX);
            jb_rr(b, 0, 0x85, JR_RAX, JR_RAX);
            uint32_t j_ok = jb_jcc_placeholder(b, 0x84);
            jb_pop(b, JR_RBP); jb_u8(b, 0xC3);
            jb_patch_here(b, j_ok);
            jb_patch_here(b, j_done);
            break;
        }

        case OP_STORE_LOCAL: {
            uint16_t slot = di->a;
            if (slot > (uint16_t)max_loc) { bail_ok = 0; break; }
            jb_mem(b, 1, 0x8B, JR_RAX, JR_R13, (int32_t)slot * 8); /* old */
            /* Fast: if !vm->has_weakrefs && (!old || (old>>50)!=0), direct write! */
            jb_mem(b, 0, 0x83, 7, JR_RBX, (int32_t)offsetof(VM, has_weakrefs)); jb_u8(b, 0); /* cmp dword [rbx+has_weakrefs], 0 */
            uint32_t j_weak = jb_jcc_placeholder(b, 0x85);
            jb_rr(b, 1, 0x85, JR_RAX, JR_RAX);
            uint32_t j_ok0 = jb_jcc_placeholder(b, 0x84);
            jb_mov_rr(b, JR_RCX, JR_RAX);
            jb_shift_ri8(b, 1, 5, JR_RCX, 50);
            uint32_t j_ok1 = jb_jcc_placeholder(b, 0x85);
            /* old is heap ptr: still fast unless CELL or TIED_SCALAR */
            jb_u8(b, 0x0F); jb_u8(b, 0xB6); jb_u8(b, 0x08);
            jb_alu_ri32(b, 7, JR_RCX, VM_OBJ_CELL);        uint32_t j_cell = jb_jcc_placeholder(b, 0x84);
            jb_alu_ri32(b, 7, JR_RCX, VM_OBJ_TIED_SCALAR); uint32_t j_tied = jb_jcc_placeholder(b, 0x84);
            jb_patch_here(b, j_ok0); jb_patch_here(b, j_ok1);
            jb_alu_ri32(b, 5, JR_R15, 1);
            jb_sib8(b, 1, 0x8B, JR_RAX, JR_R14, JR_R15, 0);
            jb_mem(b, 1, 0x89, JR_RAX, JR_R13, (int32_t)slot * 8);
            uint32_t j_done = jb_jmp_placeholder(b);

            jb_patch_here(b, j_weak); jb_patch_here(b, j_cell); jb_patch_here(b, j_tied);
            jb_sync_state(b, next_di);
            jb_mov_rr(b, JR_RDI, JR_RBX);
            jb_mov_imm64(b, JR_RSI, (uint64_t)(uintptr_t)di);
            jb_call_c(b, (void *)vm_jit_store_local_slow);
            jb_push(b, JR_RAX); jb_reload_all(b); jb_pop(b, JR_RAX);
            jb_rr(b, 0, 0x85, JR_RAX, JR_RAX);
            uint32_t j_ok = jb_jcc_placeholder(b, 0x84);
            jb_pop(b, JR_RBP); jb_u8(b, 0xC3);
            jb_patch_here(b, j_ok);
            jb_patch_here(b, j_done);
            break;
        }

        case OP_RESET_LOCAL: {
            uint16_t slot = di->a;
            if (slot <= (uint16_t)max_loc) {
                jb_mem(b, 1, 0xC7, 0, JR_R13, (int32_t)slot * 8); jb_u32(b, 0);
            }
            break;
        }

        case OP_CLEAR_LOCALS: {
            uint16_t lim = (uint16_t)max_loc;
            if (di->b < lim) lim = di->b;
            for (uint16_t s = di->a; s < lim; s++) {
                jb_mem(b, 1, 0xC7, 0, JR_R13, (int32_t)s * 8); jb_u32(b, 0);
            }
            break;
        }

        case OP_CAST_INT:
        case OP_CAST_NUM:
        case OP_CAST_ASSIGN_INT:
        case OP_CAST_ASSIGN_NUM: {
            jb_sib8(b, 1, 0x8B, JR_RAX, JR_R14, JR_R15, -8);
            if (op == OP_CAST_INT || op == OP_CAST_ASSIGN_INT) {
                uint32_t j_not = jb_j_not_int(b, JR_RAX);
                uint32_t j_done = jb_jmp_placeholder(b);
                jb_patch_here(b, j_not);
                jb_sync_state(b, next_di);
                jb_mov_rr(b, JR_RDI, JR_RBX);
                jb_mov_imm64(b, JR_RSI, (uint64_t)(uintptr_t)di);
                jb_call_c(b, (void *)vm_jit_cast_slow);
                jb_emit_reload_and_ret_check(b);
                jb_patch_here(b, j_done);
            } else {
                jb_mov_rr(b, JR_RCX, JR_RAX);
                jb_mov_imm64(b, JR_RDX, VM_DOUBLE_BIAS);
                jb_rr(b, 1, 0x29, JR_RDX, JR_RCX); /* sub rcx, VM_DOUBLE_BIAS */
                jb_mov_imm64(b, JR_RDX, VM_TAG_INT_BASE - VM_DOUBLE_BIAS);
                jb_rr(b, 1, 0x39, JR_RDX, JR_RCX); /* cmp rcx, rdx */
                uint32_t j_ok = jb_jcc_placeholder(b, 0x82); /* jb ok */
                jb_sync_state(b, next_di);
                jb_mov_rr(b, JR_RDI, JR_RBX);
                jb_mov_imm64(b, JR_RSI, (uint64_t)(uintptr_t)di);
                jb_call_c(b, (void *)vm_jit_cast_slow);
                jb_emit_reload_and_ret_check(b);
                jb_patch_here(b, j_ok);
            }
            break;
        }

        case OP_INCR:
        case OP_DECR:
        case OP_POST_INCR:
        case OP_POST_DECR: {
            uint16_t slot = di->a;
            if (slot > (uint16_t)max_loc) { bail_ok = 0; break; }
            int is_dec = (op == OP_DECR || op == OP_POST_DECR);
            int is_post = (op == OP_POST_INCR || op == OP_POST_DECR);
            jb_mem(b, 1, 0x8B, JR_RAX, JR_R13, (int32_t)slot * 8);
            uint32_t j_notint = jb_j_not_int(b, JR_RAX);
            jb_mov_rr(b, JR_R8, JR_RAX); /* save old tagged value */
            jb_shift_ri8(b, 1, 4, JR_RAX, 14);
            jb_shift_ri8(b, 1, 7, JR_RAX, 14);
            jb_alu_ri32(b, is_dec ? 5 : 0, JR_RAX, 1);
            uint32_t j_ov = jb_tag_rax_int50_or_jump(b);
            jb_mem(b, 1, 0x89, JR_RAX, JR_R13, (int32_t)slot * 8);
            if (is_post) {
                jb_mov_rr(b, JR_RAX, JR_R8);
                jb_sp_push_rax(b);
            }
            uint32_t j_done = jb_jmp_placeholder(b);

            jb_patch_here(b, j_notint);
            jb_patch_here(b, j_ov);
            jb_sync_state(b, next_di);
            jb_mov_rr(b, JR_RDI, JR_RBX);
            jb_mov_imm64(b, JR_RSI, (uint64_t)(uintptr_t)di);
            jb_call_c(b, (void *)vm_jit_incr_decr_slow);
            jb_push(b, JR_RAX); jb_reload_all(b); jb_pop(b, JR_RAX);
            jb_rr(b, 0, 0x85, JR_RAX, JR_RAX);
            uint32_t j_ok = jb_jcc_placeholder(b, 0x84);
            jb_pop(b, JR_RBP); jb_u8(b, 0xC3);
            jb_patch_here(b, j_ok);
            jb_patch_here(b, j_done);
            break;
        }

        case OP_SUB_LOCAL_I32:
        case OP_ADD_LOCAL_I32:
        case OP_BIT_AND_LOCAL_I32:
        case OP_BIT_AND_ADD_I32: {
            uint16_t slot = di->a;
            if (slot > (uint16_t)max_loc) { bail_ok = 0; break; }
            jb_mem(b, 1, 0x8B, JR_RAX, JR_R13, (int32_t)slot * 8);
            uint32_t j_notint = jb_j_not_int(b, JR_RAX);
            jb_shift_ri8(b, 1, 4, JR_RAX, 14);
            jb_shift_ri8(b, 1, 7, JR_RAX, 14);
            if (op == OP_SUB_LOCAL_I32) jb_alu_ri32(b, 5, JR_RAX, di->imm.i32.i0);
            else if (op == OP_ADD_LOCAL_I32) jb_alu_ri32(b, 0, JR_RAX, di->imm.i32.i0);
            else if (op == OP_BIT_AND_LOCAL_I32) jb_alu_ri32(b, 4, JR_RAX, di->imm.i32.i0);
            else {
                jb_alu_ri32(b, 4, JR_RAX, di->imm.i32.i0);
                jb_alu_ri32(b, 0, JR_RAX, di->imm.i32.i1);
            }
            uint32_t j_ov = jb_tag_rax_int50_or_jump(b);
            jb_sp_push_rax(b);
            uint32_t j_done = jb_jmp_placeholder(b);

            jb_patch_here(b, j_notint);
            jb_patch_here(b, j_ov);
            jb_emit_step_helper(b, di, next_di, 0);
            jb_patch_here(b, j_done);
            break;
        }

        case OP_ADD_LOCALS:
        case OP_SUB_LOCALS:
        case OP_MUL_LOCALS: {
            uint16_t sa = di->a, sb = di->b;
            if (sa > (uint16_t)max_loc || sb > (uint16_t)max_loc) { bail_ok = 0; break; }
            jb_mem(b, 1, 0x8B, JR_RAX, JR_R13, (int32_t)sa * 8);
            jb_mem(b, 1, 0x8B, JR_RDX, JR_R13, (int32_t)sb * 8);
            uint32_t j_notint = jb_j_not_both_int(b, JR_RAX, JR_RDX);
            jb_shift_ri8(b, 1, 4, JR_RAX, 14); jb_shift_ri8(b, 1, 7, JR_RAX, 14);
            jb_shift_ri8(b, 1, 4, JR_RDX, 14); jb_shift_ri8(b, 1, 7, JR_RDX, 14);
            uint32_t j_mulov = 0;
            int has_mulov = 0;
            if (op == OP_ADD_LOCALS) {
                jb_rr(b, 1, 0x01, JR_RDX, JR_RAX); /* add rax, rdx */
            } else if (op == OP_SUB_LOCALS) {
                jb_rr(b, 1, 0x29, JR_RDX, JR_RAX); /* sub rax, rdx */
            } else {
                jb_rex(b, 1, JR_RAX, 0, JR_RDX); jb_u8(b, 0x0F); jb_u8(b, 0xAF); jb_u8(b, 0xC2); /* imul rax, rdx */
                j_mulov = jb_jcc_placeholder(b, 0x80); /* jo slow */
                has_mulov = 1;
            }
            uint32_t j_ov = jb_tag_rax_int50_or_jump(b);
            jb_sp_push_rax(b);
            uint32_t j_done = jb_jmp_placeholder(b);

            jb_patch_here(b, j_notint);
            if (has_mulov) jb_patch_here(b, j_mulov);
            jb_patch_here(b, j_ov);
            jb_emit_step_helper(b, di, next_di, 0);
            jb_patch_here(b, j_done);
            break;
        }

        case OP_ADD_TO_LOCAL: {
            uint16_t d = di->a, s = di->b;
            if (d > (uint16_t)max_loc || s > (uint16_t)max_loc) { bail_ok = 0; break; }
            if (di->flags == 2) { jb_emit_step_helper(b, di, next_di, 0); break; }
            jb_mem(b, 1, 0x8B, JR_RAX, JR_R13, (int32_t)d * 8);
            jb_mem(b, 1, 0x8B, JR_RDX, JR_R13, (int32_t)s * 8);
            uint32_t j_notint = jb_j_not_both_int(b, JR_RAX, JR_RDX);
            jb_shift_ri8(b, 1, 4, JR_RAX, 14); jb_shift_ri8(b, 1, 7, JR_RAX, 14);
            jb_shift_ri8(b, 1, 4, JR_RDX, 14); jb_shift_ri8(b, 1, 7, JR_RDX, 14);
            jb_rr(b, 1, 0x01, JR_RDX, JR_RAX);
            uint32_t j_ov = jb_tag_rax_int50_or_jump(b);
            jb_mem(b, 1, 0x89, JR_RAX, JR_R13, (int32_t)d * 8);
            uint32_t j_done = jb_jmp_placeholder(b);

            jb_patch_here(b, j_notint);
            jb_patch_here(b, j_ov);
            jb_emit_step_helper(b, di, next_di, 0);
            jb_patch_here(b, j_done);
            break;
        }

        case OP_ADD_TO_LOCAL_VAL:
        case OP_SUB_TO_LOCAL_VAL: {
            uint16_t d = di->a;
            if (d > (uint16_t)max_loc) { bail_ok = 0; break; }
            if (di->flags == 2) { jb_emit_step_helper(b, di, next_di, 0); break; }
            jb_mem(b, 1, 0x8B, JR_RAX, JR_R13, (int32_t)d * 8);
            jb_sib8(b, 1, 0x8B, JR_RDX, JR_R14, JR_R15, -8);
            uint32_t j_notint = jb_j_not_both_int(b, JR_RAX, JR_RDX);
            jb_shift_ri8(b, 1, 4, JR_RAX, 14); jb_shift_ri8(b, 1, 7, JR_RAX, 14);
            jb_shift_ri8(b, 1, 4, JR_RDX, 14); jb_shift_ri8(b, 1, 7, JR_RDX, 14);
            jb_rr(b, 1, (op == OP_ADD_TO_LOCAL_VAL) ? 0x01 : 0x29, JR_RDX, JR_RAX);
            uint32_t j_ov = jb_tag_rax_int50_or_jump(b);
            jb_mem(b, 1, 0x89, JR_RAX, JR_R13, (int32_t)d * 8);
            jb_alu_ri32(b, 5, JR_R15, 1); /* pop rhs */
            uint32_t j_done = jb_jmp_placeholder(b);

            jb_patch_here(b, j_notint);
            jb_patch_here(b, j_ov);
            jb_emit_step_helper(b, di, next_di, 0);
            jb_patch_here(b, j_done);
            break;
        }

        case OP_ADD:
        case OP_SUB:
        case OP_MUL: {
            jb_sib8(b, 1, 0x8B, JR_RAX, JR_R14, JR_R15, -16); /* a */
            jb_sib8(b, 1, 0x8B, JR_RDX, JR_R14, JR_R15, -8);  /* b */
            uint32_t j_notint = jb_j_not_both_int(b, JR_RAX, JR_RDX);
            jb_shift_ri8(b, 1, 4, JR_RAX, 14); jb_shift_ri8(b, 1, 7, JR_RAX, 14);
            jb_shift_ri8(b, 1, 4, JR_RDX, 14); jb_shift_ri8(b, 1, 7, JR_RDX, 14);
            uint32_t j_mulov = 0; int has_mulov = 0;
            if (op == OP_ADD) jb_rr(b, 1, 0x01, JR_RDX, JR_RAX);
            else if (op == OP_SUB) jb_rr(b, 1, 0x29, JR_RDX, JR_RAX);
            else {
                jb_rex(b, 1, JR_RAX, 0, JR_RDX); jb_u8(b, 0x0F); jb_u8(b, 0xAF); jb_u8(b, 0xC2);
                j_mulov = jb_jcc_placeholder(b, 0x80);
                has_mulov = 1;
            }
            uint32_t j_ov = jb_tag_rax_int50_or_jump(b);
            jb_alu_ri32(b, 5, JR_R15, 1);
            jb_sib8(b, 1, 0x89, JR_RAX, JR_R14, JR_R15, -8);
            uint32_t j_done = jb_jmp_placeholder(b);

            jb_patch_here(b, j_notint);
            if (has_mulov) jb_patch_here(b, j_mulov);
            jb_patch_here(b, j_ov);
            jb_emit_step_helper(b, di, next_di, 0);
            jb_patch_here(b, j_done);
            break;
        }

        case OP_LT: case OP_LE: case OP_GT: case OP_GE: case OP_EQ: case OP_NE: {
            jb_sib8(b, 1, 0x8B, JR_RAX, JR_R14, JR_R15, -16); /* a */
            jb_sib8(b, 1, 0x8B, JR_RDX, JR_R14, JR_R15, -8);  /* b */
            uint32_t j_notint = jb_j_not_both_int(b, JR_RAX, JR_RDX);
            /* Put 50-bit sign bit (bit 49) into bit 63 so signed cmp works across negative/0! */
            jb_shift_ri8(b, 1, 4, JR_RAX, 14); /* shl rax, 14 */
            jb_shift_ri8(b, 1, 4, JR_RDX, 14); /* shl rdx, 14 */
            jb_rr(b, 1, 0x39, JR_RDX, JR_RAX); /* cmp rax, rdx */
            uint8_t setcc = (op == OP_LT) ? 0x9C : (op == OP_LE) ? 0x9E : (op == OP_GT) ? 0x9F
                          : (op == OP_GE) ? 0x9D : (op == OP_EQ) ? 0x94 : 0x95;
            jb_u8(b, 0x0F); jb_u8(b, setcc); jb_u8(b, 0xC0);
            jb_u8(b, 0x0F); jb_u8(b, 0xB6); jb_u8(b, 0xC0); /* movzx eax, al */
            jb_mov_imm64(b, JR_RDX, VM_TAG_INT_BASE);
            jb_rr(b, 1, 0x09, JR_RDX, JR_RAX);
            jb_alu_ri32(b, 5, JR_R15, 1);
            jb_sib8(b, 1, 0x89, JR_RAX, JR_R14, JR_R15, -8);
            uint32_t j_done = jb_jmp_placeholder(b);

            jb_patch_here(b, j_notint);
            jb_emit_step_helper(b, di, next_di, 0);
            jb_patch_here(b, j_done);
            break;
        }

        case OP_IS_UNDEF: {
            jb_sib8(b, 1, 0x8B, JR_RAX, JR_R14, JR_R15, -8);
            jb_rr(b, 1, 0x85, JR_RAX, JR_RAX);
            jb_u8(b, 0x0F); jb_u8(b, 0x94); jb_u8(b, 0xC0); /* sete al */
            jb_u8(b, 0x0F); jb_u8(b, 0xB6); jb_u8(b, 0xC0);
            jb_mov_imm64(b, JR_RDX, VM_TAG_INT_BASE);
            jb_rr(b, 1, 0x09, JR_RDX, JR_RAX);
            jb_sib8(b, 1, 0x89, JR_RAX, JR_R14, JR_R15, -8);
            break;
        }

        case OP_JMP: {
            uint16_t off = di->a;
            if (off >= c->dt_len) { bail_ok = 0; break; }
            if ((size_t)off <= idx) jb_emit_backedge_poll(b, &c->dt_code[off]);
            uint32_t p = jb_jmp_placeholder(b);
            jb_add_target_patch(b, p, off);
            break;
        }

        case OP_LOOP_BREAK:
        case OP_LOOP_NEXT:
        case OP_LOOP_REDO: {
            uint16_t off = di->a;
            if (off >= c->dt_len) { bail_ok = 0; break; }
            if ((size_t)off <= idx) jb_emit_backedge_poll(b, &c->dt_code[off]);
            uint32_t p = jb_jmp_placeholder(b);
            jb_add_target_patch(b, p, off);
            break;
        }

        case OP_JMP_IF_FALSE:
        case OP_JMP_IF_TRUE: {
            uint16_t off = di->a;
            if (off >= c->dt_len) { bail_ok = 0; break; }
            jb_sib8(b, 1, 0x8B, JR_RAX, JR_R14, JR_R15, -8);
            jb_rr(b, 1, 0x85, JR_RAX, JR_RAX);
            uint32_t j_undef = jb_jcc_placeholder(b, 0x84); /* falsy (eax=0) */
            jb_mov_rr(b, JR_RCX, JR_RAX);
            jb_shift_ri8(b, 1, 7, JR_RCX, 50);
            jb_alu_ri32(b, 7, JR_RCX, -1);
            uint32_t j_notint = jb_jcc_placeholder(b, 0x85);
            jb_mov_imm64(b, JR_RDX, VM_TAG_INT_BASE);
            jb_rr(b, 1, 0x39, JR_RDX, JR_RAX);
            jb_u8(b, 0x0F); jb_u8(b, 0x95); jb_u8(b, 0xC0); /* setne al */
            jb_u8(b, 0x0F); jb_u8(b, 0xB6); jb_u8(b, 0xC0);
            jb_alu_ri32(b, 5, JR_R15, 1); /* pop fast */
            uint32_t j_have_fast = jb_jmp_placeholder(b);

            jb_patch_here(b, j_undef);
            jb_mov_imm32(b, JR_RAX, 0);
            jb_alu_ri32(b, 5, JR_R15, 1); /* pop undef */
            uint32_t j_have_fast2 = jb_jmp_placeholder(b);

            jb_patch_here(b, j_notint);
            jb_sync_state(b, next_di);
            jb_mov_rr(b, JR_RDI, JR_RBX);
            jb_call_c(b, (void *)vm_jit_is_truthy);
            jb_push(b, JR_RAX); jb_reload_all(b); jb_pop(b, JR_RAX);
            jb_rr(b, 0, 0x85, JR_RAX, JR_RAX);
            uint32_t j_noexc = jb_jcc_placeholder(b, 0x89); /* jns noexc */
            jb_mov_imm32(b, JR_RAX, VM_JIT_EXC);
            jb_pop(b, JR_RBP); jb_u8(b, 0xC3);
            jb_patch_here(b, j_noexc);

            jb_patch_here(b, j_have_fast);
            jb_patch_here(b, j_have_fast2);
            jb_rr(b, 0, 0x85, JR_RAX, JR_RAX);
            uint32_t j_skip = jb_jcc_placeholder(b, (op == OP_JMP_IF_FALSE) ? 0x85 : 0x84);
            if ((size_t)off <= idx) jb_emit_backedge_poll(b, &c->dt_code[off]);
            uint32_t p = jb_jmp_placeholder(b);
            jb_add_target_patch(b, p, off);
            jb_patch_here(b, j_skip);
            break;
        }

        case OP_JMP_IF_INT_LOCAL: {
            /* forward jump when locals[a] is an integer or undef (0) */
            uint16_t sa = di->a, off = di->b;
            if (sa > (uint16_t)max_loc || off >= c->dt_len || (size_t)off <= idx) { bail_ok = 0; break; }
            jb_mem(b, 1, 0x8B, JR_RAX, JR_R13, (int32_t)sa * 8);
            jb_rr(b, 1, 0x85, JR_RAX, JR_RAX);
            uint32_t p_undef = jb_jcc_placeholder(b, 0x84); /* je target */
            jb_add_target_patch(b, p_undef, off);
            uint32_t j_notint = jb_j_not_int(b, JR_RAX);
            uint32_t p_int = jb_jmp_placeholder(b);
            jb_add_target_patch(b, p_int, off);
            jb_patch_here(b, j_notint);
            break;
        }

        case OP_LT_LOCAL_I32_JMP_F:
        case OP_LE_LOCAL_I32_JMP_F:
        case OP_LT_LOCAL_LOCAL_JMP_F: {
            uint16_t sa = di->a;
            uint16_t off = (op == OP_LT_LOCAL_LOCAL_JMP_F) ? di->c : di->b;
            if (sa > (uint16_t)max_loc || off >= c->dt_len) { bail_ok = 0; break; }
            if (op == OP_LT_LOCAL_LOCAL_JMP_F && di->b > (uint16_t)max_loc) { bail_ok = 0; break; }

            jb_mem(b, 1, 0x8B, JR_RAX, JR_R13, (int32_t)sa * 8);
            uint32_t j_slow;
            if (op == OP_LT_LOCAL_LOCAL_JMP_F) {
                jb_mem(b, 1, 0x8B, JR_RDX, JR_R13, (int32_t)di->b * 8);
                j_slow = jb_j_not_both_int(b, JR_RAX, JR_RDX);
                jb_shift_ri8(b, 1, 4, JR_RAX, 14); /* put 50-bit sign bit into bit 63 */
                jb_shift_ri8(b, 1, 4, JR_RDX, 14);
                jb_rr(b, 1, 0x39, JR_RDX, JR_RAX); /* cmp rax, rdx */
            } else {
                j_slow = jb_j_not_int(b, JR_RAX);
                jb_shift_ri8(b, 1, 4, JR_RAX, 14);
                jb_shift_ri8(b, 1, 7, JR_RAX, 14);
                jb_alu_ri32(b, 7, JR_RAX, di->imm.i32.i0); /* cmp rax, imm */
            }
            /* Note if condition is true, fall through; if false, jump to off */
            uint32_t j_true = jb_jcc_placeholder(b, (op == OP_LE_LOCAL_I32_JMP_F) ? 0x8E : 0x8C); /* jle/jl */
            uint32_t j_take_jmp = jb_jmp_placeholder(b);

            jb_patch_here(b, j_slow);
            jb_sync_state(b, next_di);
            jb_mov_rr(b, JR_RDI, JR_RBX);
            jb_mov_imm64(b, JR_RSI, (uint64_t)(uintptr_t)di);
            jb_call_c(b, (void *)vm_jit_slow_cmp_jmp);
            jb_push(b, JR_RAX); jb_reload_all(b); jb_pop(b, JR_RAX);
            jb_rr(b, 0, 0x85, JR_RAX, JR_RAX);
            uint32_t j_noexc = jb_jcc_placeholder(b, 0x89);
            jb_mov_imm32(b, JR_RAX, VM_JIT_EXC);
            jb_pop(b, JR_RBP); jb_u8(b, 0xC3);
            jb_patch_here(b, j_noexc);
            jb_rr(b, 0, 0x85, JR_RAX, JR_RAX);
            uint32_t j_true2 = jb_jcc_placeholder(b, 0x85); /* jnz -> true -> fallthrough */

            jb_patch_here(b, j_take_jmp);
            if ((size_t)off <= idx) jb_emit_backedge_poll(b, &c->dt_code[off]);
            uint32_t p = jb_jmp_placeholder(b);
            jb_add_target_patch(b, p, off);

            jb_patch_here(b, j_true);
            jb_patch_here(b, j_true2);
            break;
        }

        case OP_INCR_LT_I32_JMP_T:
        case OP_INCR_LE_I32_JMP_T:
        case OP_INCR_LT_LOCAL_JMP_T: {
            uint16_t sa = di->a;
            uint16_t off = (op == OP_INCR_LT_LOCAL_JMP_T) ? di->c : di->b;
            if (sa > (uint16_t)max_loc || off >= c->dt_len) { bail_ok = 0; break; }
            if (op == OP_INCR_LT_LOCAL_JMP_T && di->b > (uint16_t)max_loc) { bail_ok = 0; break; }

            jb_mem(b, 1, 0x8B, JR_RAX, JR_R13, (int32_t)sa * 8);
            uint32_t j_slow = 0;
            if (op == OP_INCR_LT_LOCAL_JMP_T) {
                jb_mem(b, 1, 0x8B, JR_R8, JR_R13, (int32_t)di->b * 8); /* b in r8 */
                j_slow = jb_j_not_both_int(b, JR_RAX, JR_R8);
            } else {
                j_slow = jb_j_not_int(b, JR_RAX);
            }
            jb_shift_ri8(b, 1, 4, JR_RAX, 14);
            jb_shift_ri8(b, 1, 7, JR_RAX, 14);
            jb_alu_ri32(b, 0, JR_RAX, 1); /* nv = val + 1 in rax */
            jb_mov_rr(b, JR_R9, JR_RAX);
            uint32_t j_ov = jb_tag_rax_int50_or_jump(b);
            jb_mem(b, 1, 0x89, JR_RAX, JR_R13, (int32_t)sa * 8); /* locals[sa] = tagged(nv) */

            if (op == OP_INCR_LT_LOCAL_JMP_T) {
                jb_shift_ri8(b, 1, 4, JR_R8, 14);
                jb_shift_ri8(b, 1, 7, JR_R8, 14);
                jb_rr(b, 1, 0x39, JR_R8, JR_R9); /* cmp nv, b */
            } else {
                jb_alu_ri32(b, 7, JR_R9, di->imm.i32.i0); /* cmp nv, imm */
            }
            uint32_t j_exit = jb_jcc_placeholder(b, (op == OP_INCR_LE_I32_JMP_T) ? 0x8F : 0x8D); /* jg/jge -> fallthrough */
            uint32_t j_jump = jb_jmp_placeholder(b);

            jb_patch_here(b, j_slow);
            jb_patch_here(b, j_ov);
            jb_sync_state(b, next_di);
            jb_mov_rr(b, JR_RDI, JR_RBX);
            jb_mov_imm64(b, JR_RSI, (uint64_t)(uintptr_t)di);
            jb_call_c(b, (void *)vm_jit_slow_incr_cmp_jmp);
            jb_push(b, JR_RAX); jb_reload_all(b); jb_pop(b, JR_RAX);
            jb_rr(b, 0, 0x85, JR_RAX, JR_RAX);
            uint32_t j_noexc = jb_jcc_placeholder(b, 0x89);
            jb_mov_imm32(b, JR_RAX, VM_JIT_EXC);
            jb_pop(b, JR_RBP); jb_u8(b, 0xC3);
            jb_patch_here(b, j_noexc);
            jb_rr(b, 0, 0x85, JR_RAX, JR_RAX);
            uint32_t j_exit2 = jb_jcc_placeholder(b, 0x84); /* jz -> false -> exit */

            jb_patch_here(b, j_jump);
            if ((size_t)off <= idx) jb_emit_backedge_poll(b, &c->dt_code[off]);
            uint32_t p = jb_jmp_placeholder(b);
            jb_add_target_patch(b, p, off);

            jb_patch_here(b, j_exit);
            jb_patch_here(b, j_exit2);
            break;
        }

        case OP_SET_CALL_CTX: {
            if (di->flags == 255) {
                jb_mem(b, 0, 0x8A, JR_RAX, JR_R12, (int32_t)offsetof(VMFrame, call_context));
            } else {
                jb_mov_imm32(b, JR_RAX, di->flags);
            }
            jb_mem(b, 0, 0x88, JR_RAX, JR_RBX, (int32_t)offsetof(VM, pending_call_context));
            break;
        }

        case OP_ARG_PRESENT: {
            jb_mov_imm32(b, JR_RCX, di->a);
            jb_mem(b, 0, 0x39, JR_RCX, JR_R12, (int32_t)offsetof(VMFrame, passed_argc)); /* cmp [r12+passed_argc], ecx -> setg */
            jb_u8(b, 0x0F); jb_u8(b, 0x9F); jb_u8(b, 0xC0); /* setg al */
            jb_u8(b, 0x0F); jb_u8(b, 0xB6); jb_u8(b, 0xC0);
            jb_mov_imm64(b, JR_RDX, VM_TAG_INT_BASE);
            jb_rr(b, 1, 0x09, JR_RDX, JR_RAX);
            jb_sp_push_rax(b);
            break;
        }

        case OP_CALL: {
            uint16_t fidx = di->a;
            int argc = di->flags;
            if (fidx >= prog->func_count) { bail_ok = 0; break; }
            VMChunk *callee = &prog->funcs[fidx];
            jb_mov_imm64(b, JR_RAX, (uint64_t)(uintptr_t)callee);
            /* Fast check: jit_state == 2 && !has_variadic && frame_count < frame_cap && (!rec_limit || frame_count < rec_limit) */
            jb_mem(b, 0, 0x80, 7, JR_RAX, (int32_t)offsetof(VMChunk, jit_state)); jb_u8(b, 2); /* cmp byte [rax+jit_state], 2 */
            uint32_t j_slow0 = jb_jcc_placeholder(b, 0x85);
            jb_mem(b, 0, 0x83, 7, JR_RAX, (int32_t)offsetof(VMChunk, has_variadic)); jb_u8(b, 0);
            uint32_t j_slow1 = jb_jcc_placeholder(b, 0x85);
            jb_mem(b, 1, 0x8B, JR_RCX, JR_RBX, (int32_t)offsetof(VM, frame_count));
            jb_mem(b, 1, 0x3B, JR_RCX, JR_RBX, (int32_t)offsetof(VM, frame_cap));
            uint32_t j_slow2 = jb_jcc_placeholder(b, 0x83); /* jae */
            jb_rex(b, 1, JR_RDX, 0, JR_RBX); jb_u8(b, 0x63); jb_u8(b, 0x93); jb_u32(b, (uint32_t)offsetof(VM, recursion_limit)); /* movsxd rdx, [rbx+rec_limit] */
            jb_rr(b, 1, 0x85, JR_RDX, JR_RDX);
            uint32_t j_nolim = jb_jcc_placeholder(b, 0x8E); /* jle */
            jb_rr(b, 1, 0x39, JR_RDX, JR_RCX); /* cmp rcx, rdx */
            uint32_t j_slow3 = jb_jcc_placeholder(b, 0x8D); /* jge */
            jb_patch_here(b, j_nolim);

            /* Compute lcnt = callee->local_count + 1 in r8, arg_base = r15 - argc in r9 */
            jb_rex(b, 1, JR_R8, 0, JR_RAX); jb_u8(b, 0x63); jb_u8(b, 0x80); jb_u32(b, (uint32_t)offsetof(VMChunk, local_count));
            jb_alu_ri32(b, 0, JR_R8, 1); /* lcnt */
            jb_mov_rr(b, JR_R9, JR_R15);
            if (argc > 0) jb_alu_ri32(b, 5, JR_R9, argc); /* arg_base */
            jb_sib8(b, 1, 0x8D, JR_R10, JR_R9, 0, 0); /* wrong scale, just lea r10, [r9+r8] -> mov+add: */
            b->len -= 4; /* overwrite dummy lea cleanly with mov r10, r9; add r10, r8 */
            jb_mov_rr(b, JR_R10, JR_R9);
            jb_rr(b, 1, 0x01, JR_R8, JR_R10);
            jb_mem(b, 1, 0x3B, JR_R10, JR_RBX, (int32_t)offsetof(VM, stack_cap));
            uint32_t j_slow4 = jb_jcc_placeholder(b, 0x83); /* jae */

            /* Save caller resume IP in frame->ip */
            jb_mov_imm64(b, JR_RDX, (uint64_t)(uintptr_t)next_di);
            jb_mem(b, 1, 0x89, JR_RDX, JR_R12, (int32_t)offsetof(VMFrame, ip));
            jb_mem(b, 1, 0xFF, 0, JR_RBX, (int32_t)offsetof(VM, frame_count)); /* inc frame_count */

            /* nf = r12 + sizeof(VMFrame) */
            jb_mov_rr(b, JR_RDI, JR_R12);
            jb_alu_ri32(b, 0, JR_RDI, (int32_t)sizeof(VMFrame));
            jb_mem(b, 1, 0x89, JR_RAX, JR_RDI, (int32_t)offsetof(VMFrame, chunk));
            jb_mem(b, 1, 0x8B, JR_RDX, JR_RAX, (int32_t)offsetof(VMChunk, dt_code));
            jb_mem(b, 1, 0x89, JR_RDX, JR_RDI, (int32_t)offsetof(VMFrame, ip));
            jb_mem(b, 1, 0x89, JR_R9,  JR_RDI, (int32_t)offsetof(VMFrame, stack_base));
            jb_mem(b, 1, 0x89, JR_R9,  JR_RDI, (int32_t)offsetof(VMFrame, local_base));
            jb_mem(b, 1, 0xC7, 0,      JR_RDI, (int32_t)offsetof(VMFrame, closure)); jb_u32(b, 0);
            jb_mem(b, 0, 0x8B, JR_RCX, JR_RBX, (int32_t)offsetof(VM, global_save_count));
            jb_mem(b, 0, 0x89, JR_RCX, JR_RDI, (int32_t)offsetof(VMFrame, global_save_depth));
            jb_mem(b, 0, 0xC7, 0,      JR_RDI, (int32_t)offsetof(VMFrame, passed_argc)); jb_u32(b, (uint32_t)argc);
            jb_mem(b, 0, 0x8A, JR_RCX, JR_RBX, (int32_t)offsetof(VM, pending_call_context));
            jb_mem(b, 0, 0x88, JR_RCX, JR_RDI, (int32_t)offsetof(VMFrame, call_context));
            jb_mem(b, 0, 0xC6, 0,      JR_RBX, (int32_t)offsetof(VM, pending_call_context)); jb_u8(b, 0);

            /* Compute zf at JIT compile time? Callee fixed_param_count & local_count ARE constant!
             * (Wait: callee->local_count and fixed_param_count never change after compile!) */
            int clcnt = callee->local_count + 1;
            int pmax = clcnt;
            if (callee->fixed_param_count >= 0 && callee->fixed_param_count < pmax) pmax = callee->fixed_param_count;
            int zf = argc < pmax ? argc : pmax;
            for (int zi = zf; zi < clcnt; zi++) {
                /* mov qword [r14 + (r9 + zi)*8], 0 -> r10 holds byte offset or loop; if clcnt-zf <= 6, unroll cleanly! */
                if (zi == zf) { jb_sib8(b, 1, 0x8D, JR_RCX, JR_R14, JR_R9, zf * 8); }
                jb_mem(b, 1, 0xC7, 0, JR_RCX, (zi - zf) * 8); jb_u32(b, 0);
            }
            jb_mov_rr(b, JR_R12, JR_RDI);
            jb_sib8(b, 1, 0x8D, JR_R13, JR_R14, JR_R9, 0);
            jb_mov_rr(b, JR_R15, JR_R10); /* arg_base + lcnt */
            /* call qword [rax + jit_code] */
            jb_mem(b, 1, 0xFF, 2, JR_RAX, (int32_t)offsetof(VMChunk, jit_code));
            jb_rr(b, 0, 0x85, JR_RAX, JR_RAX);
            uint32_t j_call_ok = jb_jcc_placeholder(b, 0x84);
            jb_pop(b, JR_RBP); jb_u8(b, 0xC3); /* propagate INTERP/EXC up C stack */

            jb_patch_here(b, j_slow0); jb_patch_here(b, j_slow1); jb_patch_here(b, j_slow2);
            jb_patch_here(b, j_slow3); jb_patch_here(b, j_slow4);
            jb_sync_state(b, next_di);
            /* direct call through a frame built in C (variadic callees) */
            jb_mov_rr(b, JR_RDI, JR_RBX);
            jb_mov_imm64(b, JR_RSI, (uint64_t)(uintptr_t)di);
            jb_call_c(b, (void *)vm_jit_call_prep);
            jb_rr(b, 1, 0x85, JR_RAX, JR_RAX);
            uint32_t j_prep_none = jb_jcc_placeholder(b, 0x84);
            jb_push(b, JR_RAX); jb_reload_all(b); jb_pop(b, JR_RAX);
            jb_u8(b, 0xFF); jb_u8(b, 0xD0);                 /* call rax */
            jb_rr(b, 0, 0x85, JR_RAX, JR_RAX);
            uint32_t j_prep_ok = jb_jcc_placeholder(b, 0x84);
            jb_pop(b, JR_RBP); jb_u8(b, 0xC3);
            jb_patch_here(b, j_prep_none);
            jb_mov_rr(b, JR_RDI, JR_RBX);
            jb_mov_imm64(b, JR_RSI, (uint64_t)(uintptr_t)di);
            jb_call_c(b, (void *)vm_jit_call_slow);
            jb_push(b, JR_RAX); jb_reload_all(b); jb_pop(b, JR_RAX);
            jb_rr(b, 0, 0x85, JR_RAX, JR_RAX);
            uint32_t j_slow_ok = jb_jcc_placeholder(b, 0x84);
            jb_pop(b, JR_RBP); jb_u8(b, 0xC3);
            jb_patch_here(b, j_slow_ok);
            jb_patch_here(b, j_call_ok);
            jb_patch_here(b, j_prep_ok);
            break;
        }

        case OP_TAIL_CALL: {
            uint16_t fidx = di->a;
            int argc = di->flags;
            if (fidx >= prog->func_count) { bail_ok = 0; break; }
            VMChunk *callee = &prog->funcs[fidx];
            jb_mov_imm64(b, JR_RAX, (uint64_t)(uintptr_t)callee);
            jb_mem(b, 0, 0x80, 7, JR_RAX, (int32_t)offsetof(VMChunk, jit_state)); jb_u8(b, 2);
            uint32_t j_tslow0 = jb_jcc_placeholder(b, 0x85);
            jb_mem(b, 0, 0x83, 7, JR_RAX, (int32_t)offsetof(VMChunk, has_variadic)); jb_u8(b, 0);
            uint32_t j_tslow1 = jb_jcc_placeholder(b, 0x85);
            jb_mem(b, 0, 0x8B, JR_RCX, JR_RBX, (int32_t)offsetof(VM, global_save_count));
            jb_mem(b, 0, 0x3B, JR_RCX, JR_R12, (int32_t)offsetof(VMFrame, global_save_depth));
            uint32_t j_tslow2 = jb_jcc_placeholder(b, 0x8F); /* jg */

            int clcnt = callee->local_count + 1;
            int pmax = clcnt;
            if (callee->fixed_param_count >= 0 && callee->fixed_param_count < pmax) pmax = callee->fixed_param_count;
            int zf = argc < pmax ? argc : pmax;
            jb_mem(b, 1, 0x8B, JR_R9, JR_R12, (int32_t)offsetof(VMFrame, stack_base)); /* base */
            jb_mov_rr(b, JR_R10, JR_R9);
            jb_alu_ri32(b, 0, JR_R10, clcnt); /* base + lcnt */
            jb_mem(b, 1, 0x3B, JR_R10, JR_RBX, (int32_t)offsetof(VM, stack_cap));
            uint32_t j_tslow3 = jb_jcc_placeholder(b, 0x83); /* jae */

            /* Poll signals + GC on tail calls while staying inside JIT */
            jb_emit_backedge_poll(b, di);
            jb_mov_imm64(b, JR_RAX, (uint64_t)(uintptr_t)callee);
            jb_mem(b, 1, 0x8B, JR_R9, JR_R12, (int32_t)offsetof(VMFrame, stack_base));
            jb_mov_rr(b, JR_R10, JR_R9);
            jb_alu_ri32(b, 0, JR_R10, clcnt);

            /* Copy zf args forward: r8 = r15 - argc (arg_src), forward copy stack[r9+i] = stack[r8+i] */
            jb_mov_rr(b, JR_R8, JR_R15);
            if (argc > 0) jb_alu_ri32(b, 5, JR_R8, argc);
            if (zf > 0) {
                jb_sib8(b, 1, 0x8D, JR_RSI, JR_R14, JR_R8, 0);
                jb_sib8(b, 1, 0x8D, JR_RDI, JR_R14, JR_R9, 0);
                for (int ai = 0; ai < zf; ai++) {
                    jb_mem(b, 1, 0x8B, JR_RDX, JR_RSI, ai * 8);
                    jb_mem(b, 1, 0x89, JR_RDX, JR_RDI, ai * 8);
                }
            }
            for (int zi = zf; zi < clcnt; zi++) {
                if (zi == zf) { jb_sib8(b, 1, 0x8D, JR_RCX, JR_R14, JR_R9, zf * 8); }
                jb_mem(b, 1, 0xC7, 0, JR_RCX, (zi - zf) * 8); jb_u32(b, 0);
            }
            jb_mem(b, 1, 0x89, JR_RAX, JR_R12, (int32_t)offsetof(VMFrame, chunk));
            jb_mem(b, 1, 0x8B, JR_RDX, JR_RAX, (int32_t)offsetof(VMChunk, dt_code));
            jb_mem(b, 1, 0x89, JR_RDX, JR_R12, (int32_t)offsetof(VMFrame, ip));
            jb_mem(b, 1, 0x89, JR_R9,  JR_R12, (int32_t)offsetof(VMFrame, local_base));
            jb_mem(b, 1, 0xC7, 0,      JR_R12, (int32_t)offsetof(VMFrame, closure)); jb_u32(b, 0);
            jb_mem(b, 0, 0xC7, 0,      JR_R12, (int32_t)offsetof(VMFrame, passed_argc)); jb_u32(b, (uint32_t)argc);
            jb_mem(b, 0, 0x8A, JR_RCX, JR_RBX, (int32_t)offsetof(VM, pending_call_context));
            jb_mem(b, 0, 0x88, JR_RCX, JR_R12, (int32_t)offsetof(VMFrame, call_context));
            jb_mem(b, 0, 0xC6, 0,      JR_RBX, (int32_t)offsetof(VM, pending_call_context)); jb_u8(b, 0);

            jb_sib8(b, 1, 0x8D, JR_R13, JR_R14, JR_R9, 0);
            jb_mov_rr(b, JR_R15, JR_R10);
            jb_pop(b, JR_RBP);
            jb_mem(b, 1, 0xFF, 4, JR_RAX, (int32_t)offsetof(VMChunk, jit_code)); /* jmp qword [rax+jit_code] */

            jb_patch_here(b, j_tslow0); jb_patch_here(b, j_tslow1);
            jb_patch_here(b, j_tslow2); jb_patch_here(b, j_tslow3);
            jb_sync_state(b, di);
            jb_mov_rr(b, JR_RDI, JR_RBX);
            jb_mov_imm64(b, JR_RSI, (uint64_t)(uintptr_t)di);
            jb_call_c(b, (void *)vm_jit_tail_call_slow);
            jb_pop(b, JR_RBP); jb_u8(b, 0xC3);
            break;
        }

        case OP_RETURN: {
            jb_alu_ri32(b, 5, JR_R15, 1);
            jb_sib8(b, 1, 0x8B, JR_RAX, JR_R14, JR_R15, 0); /* result in rax */
            jb_mem(b, 0, 0x8B, JR_RCX, JR_RBX, (int32_t)offsetof(VM, global_save_count));
            jb_mem(b, 0, 0x3B, JR_RCX, JR_R12, (int32_t)offsetof(VMFrame, global_save_depth));
            uint32_t j_slow0 = jb_jcc_placeholder(b, 0x8F); /* jg */
            jb_mem(b, 0, 0x83, 7, JR_RBX, (int32_t)offsetof(VM, exc_top)); jb_u8(b, 0);
            uint32_t j_slow1 = jb_jcc_placeholder(b, 0x8F); /* jg */

            jb_mem(b, 1, 0xFF, 1, JR_RBX, (int32_t)offsetof(VM, frame_count)); /* dec frame_count */
            jb_mem(b, 1, 0x8B, JR_RCX, JR_RBX, (int32_t)offsetof(VM, frame_count));
            jb_mem(b, 1, 0x3B, JR_RCX, JR_RBX, (int32_t)offsetof(VM, jit_stop_base));
            uint32_t j_stop = jb_jcc_placeholder(b, 0x84); /* je stop */

            /* Return to JIT caller frame */
            jb_mem(b, 1, 0x8B, JR_R15, JR_R12, (int32_t)offsetof(VMFrame, stack_base));
            jb_sib8(b, 1, 0x89, JR_RAX, JR_R14, JR_R15, 0);
            jb_alu_ri32(b, 0, JR_R15, 1);
            jb_alu_ri32(b, 5, JR_R12, (int32_t)sizeof(VMFrame));
            jb_mem(b, 1, 0x8B, JR_RCX, JR_R12, (int32_t)offsetof(VMFrame, local_base));
            jb_sib8(b, 1, 0x8D, JR_R13, JR_R14, JR_RCX, 0);
            /* GC check */
            jb_mem(b, 1, 0x8B, JR_RCX, JR_RBX, (int32_t)offsetof(VM, gc_count));
            jb_mem(b, 1, 0x3B, JR_RCX, JR_RBX, (int32_t)offsetof(VM, gc_threshold));
            uint32_t j_gc = jb_jcc_placeholder(b, 0x83);
            uint32_t j_gc_skip = jb_jmp_placeholder(b);

            jb_patch_here(b, j_gc);
            jb_mem(b, 1, 0x89, JR_R15, JR_RBX, (int32_t)offsetof(VM, stack_top));
            jb_mov_rr(b, JR_RDI, JR_RBX);
            jb_call_c(b, (void *)vm_jit_return_gc_only);
            jb_push(b, JR_RAX);
            jb_reload_all(b);
            jb_pop(b, JR_RAX);
            jb_pop(b, JR_RBP); jb_u8(b, 0xC3); /* returns VM_JIT_OK (0) or VM_JIT_EXC (2) */

            jb_patch_here(b, j_gc_skip);
            jb_u8(b, 0x31); jb_u8(b, 0xC0); /* xor eax, eax */
            jb_pop(b, JR_RBP); jb_u8(b, 0xC3);

            jb_patch_here(b, j_stop);
            jb_mem(b, 1, 0x89, JR_RAX, JR_RBX, (int32_t)offsetof(VM, jit_ret_val));
            jb_mem(b, 1, 0x8B, JR_RCX, JR_R12, (int32_t)offsetof(VMFrame, stack_base));
            jb_mem(b, 1, 0x89, JR_RCX, JR_RBX, (int32_t)offsetof(VM, stack_top));
            jb_u8(b, 0x31); jb_u8(b, 0xC0);
            jb_pop(b, JR_RBP); jb_u8(b, 0xC3);

            jb_patch_here(b, j_slow0);
            jb_patch_here(b, j_slow1);
            jb_mem(b, 1, 0x89, JR_R15, JR_RBX, (int32_t)offsetof(VM, stack_top));
            jb_mov_rr(b, JR_RDI, JR_RBX);
            jb_mov_rr(b, JR_RSI, JR_RAX);
            jb_call_c(b, (void *)vm_jit_return_slow);
            /* vm_jit_return_slow returns 1 (stop->0), 0 (continue->0), or 2 (VM_JIT_EXC->2) */
            jb_rr(b, 0, 0x85, JR_RAX, JR_RAX); /* test eax, eax */
            uint32_t j_slow_cont = jb_jcc_placeholder(b, 0x84);
            jb_alu_ri32(b, 7, JR_RAX, VM_JIT_EXC);
            uint32_t j_ret_exc = jb_jcc_placeholder(b, 0x84);
            jb_u8(b, 0x31); jb_u8(b, 0xC0); /* xor eax, eax */
            jb_patch_here(b, j_ret_exc);
            jb_pop(b, JR_RBP); jb_u8(b, 0xC3);

            jb_patch_here(b, j_slow_cont);
            jb_reload_all(b);
            jb_u8(b, 0x31); jb_u8(b, 0xC0); /* xor eax, eax */
            jb_pop(b, JR_RBP); jb_u8(b, 0xC3);
            break;
        }

        case OP_METHOD_CALL:
        case OP_FIELD_ACCESS: {
            int argc = di->flags;
            int total = argc + 1;
            uint32_t cur_pid = prog->prog_id;
            uint32_t ic_slot = (uint32_t)(((uintptr_t)di >> 3) & 255);
            int32_t ic0_off = (int32_t)(offsetof(VM, method_ic) + (size_t)ic_slot * sizeof(((VM *)0)->method_ic[0]));
            uint32_t j_mslow[16];
            int nm = 0;

            /* Fast mono-IC native dispatch when self is blessed VMHash matching method_ic[ic_slot][0] */
            jb_mov_rr(b, JR_R9, JR_R15);
            jb_alu_ri32(b, 5, JR_R9, total); /* arg_base */
            jb_sib8(b, 1, 0x8B, JR_RAX, JR_R14, JR_R9, 0); /* self */
            jb_rr(b, 1, 0x85, JR_RAX, JR_RAX);
            j_mslow[nm++] = jb_jcc_placeholder(b, 0x84);
            jb_mov_rr(b, JR_RCX, JR_RAX);
            jb_shift_ri8(b, 1, 5, JR_RCX, 50);
            j_mslow[nm++] = jb_jcc_placeholder(b, 0x85);
            jb_u8(b, 0x0F); jb_u8(b, 0xB6); jb_u8(b, 0x08); /* movzx ecx, byte [rax] */
            jb_alu_ri32(b, 7, JR_RCX, VM_OBJ_HASH);
            j_mslow[nm++] = jb_jcc_placeholder(b, 0x85);
            jb_mem(b, 0, 0x8B, JR_RCX, JR_RAX, (int32_t)offsetof(VMHash, class_id));
            jb_rr(b, 0, 0x85, JR_RCX, JR_RCX);
            j_mslow[nm++] = jb_jcc_placeholder(b, 0x84);
            jb_mem(b, 0, 0x81, 7, JR_RAX, (int32_t)offsetof(VMHash, class_prog_id)); jb_u32(b, cur_pid);
            j_mslow[nm++] = jb_jcc_placeholder(b, 0x85);

            jb_mem(b, 1, 0x8D, JR_RDX, JR_RBX, ic0_off); /* &method_ic[ic_slot][0] */
            jb_mov_imm64(b, JR_RSI, (uint64_t)(uintptr_t)di);
            jb_mem(b, 1, 0x39, JR_RSI, JR_RDX, 0); /* cmp [rdx + ip], rsi */
            j_mslow[nm++] = jb_jcc_placeholder(b, 0x85);
            jb_mem(b, 0, 0x81, 7, JR_RDX, (int32_t)(offsetof(VM, method_ic[0][0].prog_id) - offsetof(VM, method_ic[0][0]))); jb_u32(b, cur_pid);
            j_mslow[nm++] = jb_jcc_placeholder(b, 0x85);
            jb_mem(b, 0, 0x39, JR_RCX, JR_RDX, (int32_t)(offsetof(VM, method_ic[0][0].class_id) - offsetof(VM, method_ic[0][0])));
            j_mslow[nm++] = jb_jcc_placeholder(b, 0x85);
            jb_mem(b, 1, 0x63, JR_RAX, JR_RDX, (int32_t)(offsetof(VM, method_ic[0][0].func_idx) - offsetof(VM, method_ic[0][0])));
            jb_mem(b, 1, 0x8B, JR_RCX, JR_RBX, (int32_t)offsetof(VM, program));
            jb_mem(b, 1, 0x3B, JR_RAX, JR_RCX, (int32_t)offsetof(VMProgram, func_count));
            j_mslow[nm++] = jb_jcc_placeholder(b, 0x83); /* jae */
            jb_rr(b, 1, 0x69, JR_RAX, JR_RAX); jb_u32(b, (uint32_t)sizeof(VMChunk));
            jb_mem(b, 1, 0x03, JR_RAX, JR_RCX, (int32_t)offsetof(VMProgram, funcs)); /* rax = callee */

            jb_mem(b, 0, 0x80, 7, JR_RAX, (int32_t)offsetof(VMChunk, jit_state)); jb_u8(b, 2);
            j_mslow[nm++] = jb_jcc_placeholder(b, 0x85);
            jb_mem(b, 0, 0x83, 7, JR_RAX, (int32_t)offsetof(VMChunk, has_variadic)); jb_u8(b, 0);
            j_mslow[nm++] = jb_jcc_placeholder(b, 0x85);
            jb_mem(b, 1, 0x8B, JR_RCX, JR_RBX, (int32_t)offsetof(VM, frame_count));
            jb_mem(b, 1, 0x3B, JR_RCX, JR_RBX, (int32_t)offsetof(VM, frame_cap));
            j_mslow[nm++] = jb_jcc_placeholder(b, 0x83);
            jb_mem(b, 1, 0x63, JR_RDX, JR_RBX, (int32_t)offsetof(VM, recursion_limit));
            jb_rr(b, 1, 0x85, JR_RDX, JR_RDX);
            uint32_t j_mnolim = jb_jcc_placeholder(b, 0x8E);
            jb_rr(b, 1, 0x39, JR_RDX, JR_RCX);
            j_mslow[nm++] = jb_jcc_placeholder(b, 0x8D);
            jb_patch_here(b, j_mnolim);

            jb_mem(b, 1, 0x63, JR_R8, JR_RAX, (int32_t)offsetof(VMChunk, local_count));
            jb_alu_ri32(b, 0, JR_R8, 1); /* lcnt */
            jb_mov_rr(b, JR_R10, JR_R9);
            jb_rr(b, 1, 0x01, JR_R8, JR_R10); /* arg_base + lcnt */
            jb_mem(b, 1, 0x3B, JR_R10, JR_RBX, (int32_t)offsetof(VM, stack_cap));
            j_mslow[nm++] = jb_jcc_placeholder(b, 0x83); /* jae */

            /* Commit frame push and zero [zf .. lcnt) */
            jb_mov_imm64(b, JR_RDX, (uint64_t)(uintptr_t)next_di);
            jb_mem(b, 1, 0x89, JR_RDX, JR_R12, (int32_t)offsetof(VMFrame, ip));
            jb_mem(b, 1, 0xFF, 0, JR_RBX, (int32_t)offsetof(VM, frame_count));
            jb_mov_rr(b, JR_RDI, JR_R12);
            jb_alu_ri32(b, 0, JR_RDI, (int32_t)sizeof(VMFrame));
            jb_mem(b, 1, 0x89, JR_RAX, JR_RDI, (int32_t)offsetof(VMFrame, chunk));
            jb_mem(b, 1, 0x8B, JR_RDX, JR_RAX, (int32_t)offsetof(VMChunk, dt_code));
            jb_mem(b, 1, 0x89, JR_RDX, JR_RDI, (int32_t)offsetof(VMFrame, ip));
            jb_mem(b, 1, 0x89, JR_R9,  JR_RDI, (int32_t)offsetof(VMFrame, stack_base));
            jb_mem(b, 1, 0x89, JR_R9,  JR_RDI, (int32_t)offsetof(VMFrame, local_base));
            jb_mem(b, 1, 0xC7, 0,      JR_RDI, (int32_t)offsetof(VMFrame, closure)); jb_u32(b, 0);
            jb_mem(b, 0, 0x8B, JR_RCX, JR_RBX, (int32_t)offsetof(VM, global_save_count));
            jb_mem(b, 0, 0x89, JR_RCX, JR_RDI, (int32_t)offsetof(VMFrame, global_save_depth));
            jb_mem(b, 0, 0xC7, 0,      JR_RDI, (int32_t)offsetof(VMFrame, passed_argc)); jb_u32(b, (uint32_t)total);
            jb_mem(b, 0, 0x8A, JR_RCX, JR_RBX, (int32_t)offsetof(VM, pending_call_context));
            jb_mem(b, 0, 0x88, JR_RCX, JR_RDI, (int32_t)offsetof(VMFrame, call_context));
            jb_mem(b, 0, 0xC6, 0,      JR_RBX, (int32_t)offsetof(VM, pending_call_context)); jb_u8(b, 0);

            jb_mem(b, 1, 0x63, JR_RCX, JR_RAX, (int32_t)offsetof(VMChunk, fixed_param_count));
            jb_rr(b, 1, 0x85, JR_RCX, JR_RCX);
            uint32_t j_neg_p = jb_jcc_placeholder(b, 0x88); /* js */
            jb_rr(b, 1, 0x39, JR_R8, JR_RCX); /* cmp rcx, r8 */
            uint32_t j_pmax_ok = jb_jcc_placeholder(b, 0x8E); /* jle */
            jb_patch_here(b, j_neg_p);
            jb_mov_rr(b, JR_RCX, JR_R8);
            jb_patch_here(b, j_pmax_ok);
            jb_alu_ri32(b, 7, JR_RCX, total); /* cmp rcx, total */
            uint32_t j_zf_ok = jb_jcc_placeholder(b, 0x8E); /* jle */
            jb_mov_imm32(b, JR_RCX, (uint32_t)total);
            jb_patch_here(b, j_zf_ok);
            jb_sib8(b, 1, 0x8D, JR_RDX, JR_R14, JR_R9, 0); /* &stack[arg_base] */
            uint32_t loop_top = (uint32_t)b->len;
            jb_rr(b, 1, 0x39, JR_R8, JR_RCX); /* cmp rcx, r8 */
            uint32_t j_zdone = jb_jcc_placeholder(b, 0x8D); /* jge */
            jb_sib8(b, 1, 0xC7, 0, JR_RDX, JR_RCX, 0); jb_u32(b, 0);
            jb_alu_ri32(b, 0, JR_RCX, 1);
            jb_u8(b, 0xE9); jb_u32(b, (uint32_t)(int32_t)(loop_top - (b->len + 4)));
            jb_patch_here(b, j_zdone);

            jb_mov_rr(b, JR_R12, JR_RDI);
            jb_sib8(b, 1, 0x8D, JR_R13, JR_R14, JR_R9, 0);
            jb_mov_rr(b, JR_R15, JR_R10);
            jb_mem(b, 1, 0xFF, 2, JR_RAX, (int32_t)offsetof(VMChunk, jit_code));
            jb_rr(b, 0, 0x85, JR_RAX, JR_RAX);
            uint32_t j_fast_done = jb_jcc_placeholder(b, 0x84);
            jb_pop(b, JR_RBP); jb_u8(b, 0xC3);

            for (int fi = 0; fi < nm; fi++) jb_patch_here(b, j_mslow[fi]);
            jb_sync_state(b, next_di);
            jb_mov_rr(b, JR_RDI, JR_RBX);
            jb_mov_imm64(b, JR_RSI, (uint64_t)(uintptr_t)di);
            jb_mov_imm32(b, JR_RDX, (op == OP_FIELD_ACCESS) ? 1 : 0);
            jb_alu_ri32(b, 5, JR_RSP, 16);
            jb_mov_rr(b, JR_RCX, JR_RSP);
            jb_call_c(b, (void *)vm_jit_mcall_step);
            jb_mem(b, 1, 0x8B, JR_R8, JR_RSP, 0); /* out_callee */
            jb_alu_ri32(b, 0, JR_RSP, 16);
            jb_alu_ri32(b, 7, JR_RAX, 3);
            uint32_t j_native = jb_jcc_placeholder(b, 0x84);
            jb_push(b, JR_RAX); jb_reload_all(b); jb_pop(b, JR_RAX);
            jb_rr(b, 0, 0x85, JR_RAX, JR_RAX);
            uint32_t j_done_ok = jb_jcc_placeholder(b, 0x84);
            jb_pop(b, JR_RBP); jb_u8(b, 0xC3);

            jb_patch_here(b, j_native);
            jb_push(b, JR_R8);
            jb_reload_all(b);
            jb_pop(b, JR_RAX);
            jb_mem(b, 1, 0xFF, 2, JR_RAX, (int32_t)offsetof(VMChunk, jit_code));
            jb_rr(b, 0, 0x85, JR_RAX, JR_RAX);
            uint32_t j_native_ok = jb_jcc_placeholder(b, 0x84);
            jb_pop(b, JR_RBP); jb_u8(b, 0xC3);
            jb_patch_here(b, j_native_ok);
            jb_patch_here(b, j_done_ok);
            jb_patch_here(b, j_fast_done);
            break;
        }

        case OP_ARRAY_GET: {
            uint32_t afails[8]; int na = 0;
            jb_mem(b, 0, 0x83, 7, JR_RBX, (int32_t)offsetof(VM, has_weakrefs)); jb_u8(b, 0);
            afails[na++] = jb_jcc_placeholder(b, 0x85);
            jb_sib8(b, 1, 0x8B, JR_RAX, JR_R14, JR_R15, -16); /* arr */
            jb_sib8(b, 1, 0x8B, JR_RDX, JR_R14, JR_R15, -8);  /* idx */
            jb_rr(b, 1, 0x85, JR_RAX, JR_RAX);
            afails[na++] = jb_jcc_placeholder(b, 0x84);
            jb_mov_rr(b, JR_RCX, JR_RAX);
            jb_shift_ri8(b, 1, 5, JR_RCX, 50);
            afails[na++] = jb_jcc_placeholder(b, 0x85);
            jb_u8(b, 0x0F); jb_u8(b, 0xB6); jb_u8(b, 0x08);
            jb_alu_ri32(b, 7, JR_RCX, VM_OBJ_ARRAY);
            afails[na++] = jb_jcc_placeholder(b, 0x85);
            jb_mem(b, 1, 0x83, 7, JR_RAX, (int32_t)offsetof(VMArray, tied_obj)); jb_u8(b, 0);
            afails[na++] = jb_jcc_placeholder(b, 0x85);
            afails[na++] = jb_j_not_int(b, JR_RDX);
            jb_shift_ri8(b, 1, 4, JR_RDX, 14);
            jb_shift_ri8(b, 1, 7, JR_RDX, 14);
            jb_mem(b, 1, 0x63, JR_RCX, JR_RAX, (int32_t)offsetof(VMArray, size));
            jb_rr(b, 1, 0x39, JR_RCX, JR_RDX); /* cmp rdx, rcx */
            afails[na++] = jb_jcc_placeholder(b, 0x83); /* jae (rejects < 0 or >= size) */
            jb_mem(b, 1, 0x8B, JR_RCX, JR_RAX, (int32_t)offsetof(VMArray, items));
            jb_sib8(b, 1, 0x8B, JR_RAX, JR_RCX, JR_RDX, 0); /* items[ii] */
            jb_rr(b, 1, 0x85, JR_RAX, JR_RAX);
            uint32_t j_aok0 = jb_jcc_placeholder(b, 0x84);
            jb_mov_rr(b, JR_RCX, JR_RAX);
            jb_shift_ri8(b, 1, 5, JR_RCX, 50);
            uint32_t j_aok1 = jb_jcc_placeholder(b, 0x85);
            jb_u8(b, 0x0F); jb_u8(b, 0xB6); jb_u8(b, 0x08);
            jb_alu_ri32(b, 7, JR_RCX, VM_OBJ_STRBUF);
            uint32_t j_aok2 = jb_jcc_placeholder(b, 0x85);
            jb_mem(b, 0, 0xC6, 0, JR_RAX, (int32_t)offsetof(VMStrBuf, shared)); jb_u8(b, 1);
            jb_patch_here(b, j_aok0); jb_patch_here(b, j_aok1); jb_patch_here(b, j_aok2);
            jb_alu_ri32(b, 5, JR_R15, 1);
            jb_sib8(b, 1, 0x89, JR_RAX, JR_R14, JR_R15, -8);
            uint32_t j_adone = jb_jmp_placeholder(b);

            for (int fi = 0; fi < na; fi++) jb_patch_here(b, afails[fi]);
            jb_emit_step_helper(b, di, next_di, 1);
            jb_patch_here(b, j_adone);
            break;
        }

        case OP_HASH_GET_CONST: {
            uint32_t hfails[20];
            int nh = jb_emit_hash_primary_probe(b, di, c, -8, hfails);
            if (nh < 0) {
                jb_emit_step_helper(b, di, next_di, 1);
                break;
            }
            jb_mem(b, 1, 0x8B, JR_RAX, JR_RCX, (int32_t)offsetof(VMHashEntry, value));
            jb_rr(b, 1, 0x85, JR_RAX, JR_RAX);
            uint32_t j_hok0 = jb_jcc_placeholder(b, 0x84);
            jb_mov_rr(b, JR_RCX, JR_RAX);
            jb_shift_ri8(b, 1, 5, JR_RCX, 50);
            uint32_t j_hok1 = jb_jcc_placeholder(b, 0x85);
            jb_u8(b, 0x0F); jb_u8(b, 0xB6); jb_u8(b, 0x08);
            jb_alu_ri32(b, 7, JR_RCX, VM_OBJ_WEAKREF);
            hfails[nh++] = jb_jcc_placeholder(b, 0x84);
            jb_alu_ri32(b, 7, JR_RCX, VM_OBJ_STRBUF);
            uint32_t j_hok2 = jb_jcc_placeholder(b, 0x85);
            jb_mem(b, 0, 0xC6, 0, JR_RAX, (int32_t)offsetof(VMStrBuf, shared)); jb_u8(b, 1);
            jb_patch_here(b, j_hok0); jb_patch_here(b, j_hok1); jb_patch_here(b, j_hok2);
            jb_sib8(b, 1, 0x89, JR_RAX, JR_R14, JR_R15, -8);
            uint32_t j_hdone = jb_jmp_placeholder(b);

            for (int fi = 0; fi < nh; fi++) jb_patch_here(b, hfails[fi]);
            jb_emit_step_helper(b, di, next_di, 1);
            jb_patch_here(b, j_hdone);
            break;
        }

        case OP_HASH_SET_CONST: {
            uint32_t hfails[20];
            int nh = jb_emit_hash_primary_probe(b, di, c, -16, hfails);
            if (nh < 0) {
                jb_emit_step_helper(b, di, next_di, 1);
                break;
            }
            jb_sib8(b, 1, 0x8B, JR_RDX, JR_R14, JR_R15, -8); /* v */
            jb_mem(b, 1, 0x89, JR_RDX, JR_RCX, (int32_t)offsetof(VMHashEntry, value));
            jb_alu_ri32(b, 5, JR_R15, 2);
            uint32_t j_hsdone = jb_jmp_placeholder(b);

            for (int fi = 0; fi < nh; fi++) jb_patch_here(b, hfails[fi]);
            jb_emit_step_helper(b, di, next_di, 1);
            jb_patch_here(b, j_hsdone);
            break;
        }

        case OP_THROW: {
            jb_alu_ri32(b, 5, JR_R15, 1);
            jb_sib8(b, 1, 0x8B, JR_RAX, JR_R14, JR_R15, 0);
            jb_mem(b, 1, 0x89, JR_RAX, JR_RBX, (int32_t)offsetof(VM, pending_exc));
            jb_mem(b, 0, 0xC7, 0, JR_RBX, (int32_t)offsetof(VM, has_pending_exc)); jb_u32(b, 1);
            jb_sync_state(b, next_di);
            jb_mov_imm32(b, JR_RAX, VM_JIT_EXC);
            jb_pop(b, JR_RBP); jb_u8(b, 0xC3);
            break;
        }

        case OP_DIV:
        case OP_BIT_AND:
        case OP_BIT_OR:
        case OP_BIT_XOR:
        case OP_NEGATE:
        case OP_NEW_ARRAY:
        case OP_NEW_HASH:
        case OP_ARRAY_PUSH:
        case OP_ARRAY_SET:
        case OP_HASH_GET_CONCAT:
        case OP_HASH_SET_CONCAT:
        case OP_CONCAT:
        case OP_APPEND_LOCAL:
        case OP_APPEND_CONST:
        case OP_LOAD_GLOBAL:
        case OP_STORE_GLOBAL:
        case OP_STR_LEN:
        case OP_SAY:
        case OP_PRINT:
        case OP_BUILTIN:
        case OP_STR_OV:
        case OP_HASH_BLESS:
        case OP_LOAD_CAPTURE_CELL:
        case OP_SPACESHIP:
        case OP_HASH_GET:
        case OP_HASH_SET:
            jb_emit_step_helper(b, di, next_di, 1);
            break;

        case OP_HASH_REF:
            break;   /* a no-op in the interpreter too */

        case OP_ARRAY_SIZE: {
            /* untied array: its size; everything else (hashes, tied) in the helper */
            jb_sib8(b, 1, 0x8B, JR_RAX, JR_R14, JR_R15, -8);
            jb_mov_rr(b, JR_RCX, JR_RAX);
            jb_shift_ri8(b, 1, 5, JR_RCX, 50);
            uint32_t j_s0 = jb_jcc_placeholder(b, 0x85);    /* not a pointer */
            jb_rr(b, 1, 0x85, JR_RAX, JR_RAX);
            uint32_t j_s1 = jb_jcc_placeholder(b, 0x84);    /* undef */
            jb_mem(b, 0, 0x80, 7, JR_RAX, (int32_t)offsetof(VMObjHeader, obj_type)); jb_u8(b, (uint8_t)VM_OBJ_ARRAY);
            uint32_t j_s2 = jb_jcc_placeholder(b, 0x85);
            jb_mem(b, 1, 0x83, 7, JR_RAX, (int32_t)offsetof(VMArray, tied_obj)); jb_u8(b, 0);
            uint32_t j_s3 = jb_jcc_placeholder(b, 0x85);    /* tied */
            jb_rex(b, 1, JR_RAX, 0, JR_RAX); jb_u8(b, 0x63);  /* movsxd rax, dword [rax+size] */
            jb_u8(b, 0x80); jb_u32(b, (uint32_t)offsetof(VMArray, size));
            jb_mov_imm64(b, JR_RDX, VM_TAG_INT_BASE);
            jb_rr(b, 1, 0x09, JR_RDX, JR_RAX);
            jb_sib8(b, 1, 0x89, JR_RAX, JR_R14, JR_R15, -8);
            uint32_t j_done = jb_jmp_placeholder(b);
            jb_patch_here(b, j_s0); jb_patch_here(b, j_s1); jb_patch_here(b, j_s2); jb_patch_here(b, j_s3);
            jb_emit_step_helper(b, di, next_di, 0);
            jb_patch_here(b, j_done);
            break;
        }

        case OP_NOT: {
            jb_sib8(b, 1, 0x8B, JR_RAX, JR_R14, JR_R15, -8);
            uint32_t j_s0 = jb_j_not_int(b, JR_RAX);
            jb_mov_imm64(b, JR_RDX, VM_TAG_INT_BASE);
            jb_rr(b, 1, 0x39, JR_RDX, JR_RAX);              /* cmp rax, TAG|0 */
            jb_u8(b, 0x0F); jb_u8(b, 0x94); jb_u8(b, 0xC0); /* sete al */
            jb_u8(b, 0x0F); jb_u8(b, 0xB6); jb_u8(b, 0xC0); /* movzx eax, al */
            jb_rr(b, 1, 0x09, JR_RDX, JR_RAX);
            jb_sib8(b, 1, 0x89, JR_RAX, JR_R14, JR_R15, -8);
            uint32_t j_done = jb_jmp_placeholder(b);
            jb_patch_here(b, j_s0);
            jb_emit_step_helper(b, di, next_di, 0);
            jb_patch_here(b, j_done);
            break;
        }

        case OP_MOD: {
            /* int % positive int inline (Perl sign rule); else the helper */
            jb_sib8(b, 1, 0x8B, JR_RCX, JR_R14, JR_R15, -8);  /* rcx = divisor */
            jb_sib8(b, 1, 0x8B, JR_RAX, JR_R14, JR_R15, -16); /* rax = dividend */
            jb_mov_rr(b, JR_RDX, JR_RCX);
            uint32_t j_s0 = jb_j_not_both_int(b, JR_RAX, JR_RDX); /* clobbers rcx */
            jb_mov_rr(b, JR_RCX, JR_RDX);
            jb_shift_ri8(b, 1, 4, JR_RAX, 14); jb_shift_ri8(b, 1, 7, JR_RAX, 14);
            jb_shift_ri8(b, 1, 4, JR_RCX, 14); jb_shift_ri8(b, 1, 7, JR_RCX, 14);
            jb_rr(b, 1, 0x85, JR_RCX, JR_RCX);
            uint32_t j_s1 = jb_jcc_placeholder(b, 0x8E);    /* jle: divisor <= 0 */
            jb_u8(b, 0x48); jb_u8(b, 0x99);                 /* cqo */
            jb_rr(b, 1, 0xF7, 7, JR_RCX);                   /* idiv rcx */
            jb_rr(b, 1, 0x85, JR_RDX, JR_RDX);
            uint32_t j_nonneg = jb_jcc_placeholder(b, 0x89);
            jb_rr(b, 1, 0x01, JR_RCX, JR_RDX);              /* add rdx, rcx */
            jb_patch_here(b, j_nonneg);
            jb_mov_rr(b, JR_RAX, JR_RDX);
            jb_mov_imm64(b, JR_RDX, VM_TAG_INT_BASE);
            jb_rr(b, 1, 0x09, JR_RDX, JR_RAX);
            jb_alu_ri32(b, 5, JR_R15, 1);                   /* pop the divisor */
            jb_sib8(b, 1, 0x89, JR_RAX, JR_R14, JR_R15, -8);
            uint32_t j_done = jb_jmp_placeholder(b);
            jb_patch_here(b, j_s0); jb_patch_here(b, j_s1);
            jb_emit_step_helper(b, di, next_di, 0);
            jb_patch_here(b, j_done);
            break;
        }

        case OP_MOD_LOCAL_I32: {
            /* int % positive literal inline (Perl: the result takes the
             * divisor's sign); anything else through the step helper */
            uint16_t slot = di->a;
            int32_t mimm = di->imm.i32.i0;
            if (slot > (uint16_t)max_loc) { bail_ok = 0; break; }
            if (mimm <= 0) { jb_emit_step_helper(b, di, next_di, 0); break; }
            jb_mem(b, 1, 0x8B, JR_RAX, JR_R13, (int32_t)slot * 8);
            uint32_t j_notint = jb_j_not_int(b, JR_RAX);
            jb_shift_ri8(b, 1, 4, JR_RAX, 14);
            jb_shift_ri8(b, 1, 7, JR_RAX, 14);
            jb_mov_imm32(b, JR_RCX, (uint32_t)mimm);
            jb_u8(b, 0x48); jb_u8(b, 0x99);                 /* cqo */
            jb_rr(b, 1, 0xF7, 7, JR_RCX);                   /* idiv rcx */
            jb_rr(b, 1, 0x85, JR_RDX, JR_RDX);              /* test rdx, rdx */
            uint32_t j_nonneg = jb_jcc_placeholder(b, 0x89); /* jns */
            jb_alu_ri32(b, 0, JR_RDX, mimm);                /* add rdx, imm */
            jb_patch_here(b, j_nonneg);
            jb_mov_rr(b, JR_RAX, JR_RDX);
            jb_mov_imm64(b, JR_RDX, VM_TAG_INT_BASE);
            jb_rr(b, 1, 0x09, JR_RDX, JR_RAX);              /* or rax, rdx */
            jb_sp_push_rax(b);
            uint32_t j_done = jb_jmp_placeholder(b);
            jb_patch_here(b, j_notint);
            jb_emit_step_helper(b, di, next_di, 0);
            jb_patch_here(b, j_done);
            break;
        }

        case OP_DEFINED: {
            /* weak refs (vm->has_weakrefs) go through the step helper */
            jb_mem(b, 0, 0x83, 7, JR_RBX, (int32_t)offsetof(VM, has_weakrefs)); jb_u8(b, 0);
            uint32_t j_slow = jb_jcc_placeholder(b, 0x85);
            jb_sib8(b, 1, 0x8B, JR_RAX, JR_R14, JR_R15, -8); /* rax = top */
            jb_rr(b, 1, 0x85, JR_RAX, JR_RAX);
            jb_u8(b, 0x0F); jb_u8(b, 0x95); jb_u8(b, 0xC0); /* setne al */
            jb_u8(b, 0x0F); jb_u8(b, 0xB6); jb_u8(b, 0xC0); /* movzx eax, al */
            jb_mov_imm64(b, JR_RDX, VM_TAG_INT_BASE);
            jb_rr(b, 1, 0x09, JR_RDX, JR_RAX);
            jb_sib8(b, 1, 0x89, JR_RAX, JR_R14, JR_R15, -8); /* top = defined */
            uint32_t j_done = jb_jmp_placeholder(b);
            jb_patch_here(b, j_slow);
            jb_emit_step_helper(b, di, next_di, 0);
            jb_patch_here(b, j_done);
            break;
        }

        case OP_LOAD_CAPTURE:
        case OP_STORE_CAPTURE: {
            /* Inline when the capture (or the cell holding it) is an integer:
             * load pushes it, store replaces it. Everything else -- strings
             * (copied), tied scalars, weak refs, no closure -- through the
             * step helper. */
            uint16_t cidx = di->a;
            jb_mem(b, 1, 0x8B, JR_RAX, JR_R12, (int32_t)offsetof(VMFrame, closure));
            jb_rr(b, 1, 0x85, JR_RAX, JR_RAX);
            uint32_t j_s0 = jb_jcc_placeholder(b, 0x84);
            jb_mem(b, 0, 0x81, 7, JR_RAX, (int32_t)offsetof(VMClosure, capture_count)); jb_u32(b, (uint32_t)cidx);
            uint32_t j_s1 = jb_jcc_placeholder(b, 0x8E);    /* jle: count <= idx */
            jb_mem(b, 1, 0x8B, JR_RAX, JR_RAX, (int32_t)offsetof(VMClosure, captures));
            jb_mem(b, 1, 0x8D, JR_RDX, JR_RAX, (int32_t)cidx * 8); /* rdx = &captures[idx] */
            jb_mem(b, 1, 0x8B, JR_RAX, JR_RDX, 0);          /* rax = capture */
            jb_mov_rr(b, JR_RCX, JR_RAX);
            jb_shift_ri8(b, 1, 5, JR_RCX, 50);              /* shr rcx, 50: 0 for pointers/undef */
            uint32_t j_notptr = jb_jcc_placeholder(b, 0x85);
            jb_rr(b, 1, 0x85, JR_RAX, JR_RAX);
            uint32_t j_s2 = jb_jcc_placeholder(b, 0x84);    /* undef capture */
            jb_mem(b, 0, 0x80, 7, JR_RAX, (int32_t)offsetof(VMObjHeader, obj_type)); jb_u8(b, (uint8_t)VM_OBJ_CELL);
            uint32_t j_s3 = jb_jcc_placeholder(b, 0x85);    /* other pointer */
            jb_mem(b, 1, 0x8D, JR_RDX, JR_RAX, (int32_t)offsetof(VMCell, val)); /* rdx = &cell->val */
            jb_mem(b, 1, 0x8B, JR_RAX, JR_RDX, 0);
            jb_patch_here(b, j_notptr);
            /* rax: the value now in the slot at [rdx]; must be an integer */
            uint32_t j_s4 = jb_j_not_int(b, JR_RAX);
            if (op == OP_LOAD_CAPTURE) {
                jb_sp_push_rax(b);
            } else {
                jb_alu_ri32(b, 5, JR_R15, 1);               /* pop the new value */
                jb_sib8(b, 1, 0x8B, JR_RAX, JR_R14, JR_R15, 0);
                jb_mem(b, 1, 0x89, JR_RAX, JR_RDX, 0);
            }
            uint32_t j_done = jb_jmp_placeholder(b);
            jb_patch_here(b, j_s0); jb_patch_here(b, j_s1); jb_patch_here(b, j_s2);
            jb_patch_here(b, j_s3); jb_patch_here(b, j_s4);
            jb_emit_step_helper(b, di, next_di, 0);
            jb_patch_here(b, j_done);
            break;
        }

        case OP_CALL_CLOSURE: {
            jb_sync_state(b, next_di);
            jb_mov_rr(b, JR_RDI, JR_RBX);
            jb_mov_imm64(b, JR_RSI, (uint64_t)(uintptr_t)di);
            jb_call_c(b, (void *)vm_jit_closure_prep);
            jb_rr(b, 1, 0x85, JR_RAX, JR_RAX);
            uint32_t j_cl_slow = jb_jcc_placeholder(b, 0x84); /* jz slow */
            jb_push(b, JR_RAX); jb_reload_all(b); jb_pop(b, JR_RAX);
            jb_u8(b, 0xFF); jb_u8(b, 0xD0);                 /* call rax */
            jb_rr(b, 0, 0x85, JR_RAX, JR_RAX);
            uint32_t j_cl_ok = jb_jcc_placeholder(b, 0x84);
            jb_pop(b, JR_RBP); jb_u8(b, 0xC3);              /* propagate INTERP/EXC */
            jb_patch_here(b, j_cl_slow);
            jb_mov_rr(b, JR_RDI, JR_RBX);
            jb_mov_imm64(b, JR_RSI, (uint64_t)(uintptr_t)di);
            jb_call_c(b, (void *)vm_jit_call_closure_slow);
            jb_emit_reload_and_ret_check(b);
            jb_patch_here(b, j_cl_ok);
            break;
        }

        default:
            /* Clean bailout to direct-threaded C interpreter at this exact instruction */
            jb_sync_state(b, di);
            jb_mov_imm32(b, JR_RAX, VM_JIT_INTERP);
            jb_pop(b, JR_RBP);
            jb_u8(b, 0xC3);
            break;
        }
        if (!bail_ok || b->oom) break;
    }

    if (bail_ok && !b->oom) {
        b->offs[c->dt_len] = (uint32_t)b->len;
        /* Trailing safety return */
        jb_sync_state(b, &c->dt_code[c->dt_len]);
        jb_mov_imm32(b, JR_RAX, VM_JIT_INTERP);
        jb_pop(b, JR_RBP); jb_u8(b, 0xC3);

        for (size_t pi = 0; pi < b->patch_count; pi++) {
            uint16_t tidx = b->patches[pi].target_idx;
            if (tidx >= c->dt_len) { bail_ok = 0; break; }
            uint32_t poff = b->patches[pi].patch_off;
            int32_t rel = (int32_t)b->offs[tidx] - (int32_t)(poff + 4);
            memcpy(b->buf + poff, &rel, 4);
        }
    }

    if (bail_ok && !b->oom && b->len > 0) {
        size_t alloc_sz = (b->len + 4095) & ~(size_t)4095;
        void *mem = mmap(NULL, alloc_sz, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS, -1, 0);
        if (mem != MAP_FAILED) {
            memcpy(mem, b->buf, b->len);
            if (mprotect(mem, alloc_sz, PROT_READ | PROT_EXEC) == 0) {
                c->jit_size = alloc_sz;
                c->jit_offs = b->offs;
                b->offs = NULL;
                __atomic_store_n(&c->jit_code, mem, __ATOMIC_RELEASE);
                __atomic_store_n(&c->jit_state, 2, __ATOMIC_RELEASE);
            } else {
                munmap(mem, alloc_sz);
                c->jit_state = 3;
            }
        } else {
            c->jit_state = 3;
        }
    } else {
        c->jit_state = 3;
    }

    free(b->buf);
    free(b->offs);
    free(b->patches);
}

static __attribute__((noinline)) void vm_jit_try_compile(VMProgram *prog, VMChunk *c) {
    if (__atomic_load_n(&c->jit_state, __ATOMIC_ACQUIRE) != 0) return;
    if (prog) pthread_mutex_lock(&prog->class_mutex);
    if (c->jit_state == 0) {
        vm_jit_compile_chunk_locked(prog, c);
    }
    if (prog) pthread_mutex_unlock(&prog->class_mutex);
}
#endif /* VM_JIT_X64 */

VMValue vm_execute_call(VM *vm, int func_idx, VMValue *cargs, int cargc) {
    return vm_execute_call_cl(vm, func_idx, NULL, cargs, cargc, cargc);
}

static VMValue vm_execute_call_cl(VM *vm, int func_idx, VMClosure *closure, VMValue *cargs, int cargc, int passed_argc) {
    VM *prev_vm = g_active_vm;
    g_active_vm = vm;
    size_t saved_exec_base = vm->exec_base;
    size_t saved_jit_stop = vm->jit_stop_base;
    vm->exec_base = vm->frame_count;
    #define VM_EXEC_RETURN(val) do { vm->jit_stop_base = saved_jit_stop; g_active_vm = prev_vm; return (val); } while(0)

    VMChunk *chunk = &vm->program->funcs[func_idx];
#ifdef __GNUC__
    /* Build dispatch table for ALL opcodes up to OP_OPCODE_COUNT */
    static const void *dispatch_table[256] = { NULL };
    static int dt_init = 0;
    static pthread_mutex_t g_dt_init_mutex = PTHREAD_MUTEX_INITIALIZER;
    if (__builtin_expect(!__atomic_load_n(&dt_init, __ATOMIC_ACQUIRE), 0)) {
        pthread_mutex_lock(&g_dt_init_mutex);
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
            DT(OP_CAST_INT); DT(OP_JMP_IF_INT_LOCAL);
            DT(OP_CAST_NUM);
            DT(OP_CAST_ASSIGN_INT);
            DT(OP_CAST_ASSIGN_NUM);
            DT(OP_STR_OV);
            DT(OP_SUB_LOCAL_I32);
            DT(OP_ADD_LOCAL_I32);
            DT(OP_ADD_LOCALS);
            DT(OP_ADD_TO_LOCAL);
            DT(OP_LT_LOCAL_I32_JMP_F);
            DT(OP_LE_LOCAL_I32_JMP_F);
            DT(OP_LT_LOCAL_LOCAL_JMP_F);
            DT(OP_ADD_TO_LOCAL_VAL);
            DT(OP_SUB_TO_LOCAL_VAL);
            DT(OP_BIT_AND_LOCAL_I32);
            DT(OP_MOD_LOCAL_I32);
            DT(OP_HASH_GET_CONST);
            DT(OP_HASH_SET_CONST);
            DT(OP_SUB_LOCALS);
            DT(OP_MUL_LOCALS);
            DT(OP_INCR_LT_I32_JMP_T);
            DT(OP_INCR_LE_I32_JMP_T);
            DT(OP_INCR_LT_LOCAL_JMP_T);
            DT(OP_BIT_AND_ADD_I32);
            DT(OP_TAIL_CALL);
            #undef DT
#ifdef VM_JIT_X64
            __atomic_store_n(&g_vm_dt_table, dispatch_table, __ATOMIC_RELEASE);
#endif
            __atomic_store_n(&dt_init, 1, __ATOMIC_RELEASE);
        }
        pthread_mutex_unlock(&g_dt_init_mutex);
    }
    const void * const *dt_ptr = dispatch_table;
#else
    const void * const *dt_ptr = NULL;
#endif
    if (__builtin_expect(!__atomic_load_n(&chunk->dt_code, __ATOMIC_ACQUIRE) ||
                         chunk->dt_code_len_src != chunk->code_len, 0)) {
        vm_chunk_ensure_dt(vm->program, chunk, dt_ptr);
    }
    if (vm->frame_count >= vm->frame_cap) {
        vm_frames_grow(vm);
    }
    VMFrame *frame = &vm->frames[vm->frame_count++];
    frame->chunk = chunk;
    frame->ip = chunk->dt_code;
    size_t base = vm->stack_top;
    {
        int cnt = chunk->local_count + 1;
        int pmax = cnt;
        if (!chunk->has_variadic && chunk->fixed_param_count >= 0 && chunk->fixed_param_count < pmax) {
            pmax = chunk->fixed_param_count;
        }
        int zf = cargc < pmax ? cargc : pmax;
        if (__builtin_expect(base + (size_t)cnt >= vm->stack_cap, 0)) {
            vm_stack_grow(vm, base + (size_t)cnt);
        }
        VMValue *loc = vm->stack + base;
        for (int ci = 0; ci < zf; ci++) loc[ci] = cargs[ci];
        if (zf < cnt) {
            memset(loc + zf, 0, (size_t)(cnt - zf) * sizeof(VMValue));
        }
        vm->stack_top = base + (size_t)cnt;
    }
    frame->local_base = base;
    frame->stack_base = base;
    frame->closure = closure;
    frame->global_save_depth = vm->global_save_count;
    frame->passed_argc = passed_argc;
    frame->call_context = vm->pending_call_context;
    vm->pending_call_context = 0;

    VMInsn *ip = frame->ip;
    const VMInsn *di = NULL;
    VMValue *stack = vm->stack;
    VMValue *locals = stack + frame->local_base;
    size_t sp = vm->stack_top;
    uint32_t sig_poll = 0;

    #define SP_PUSH(v) do { \
        if (__builtin_expect(sp >= vm->stack_cap, 0)) { \
            vm_stack_grow(vm, sp); \
            stack = vm->stack; \
            locals = stack + frame->local_base; \
        } \
        stack[sp++] = (v); \
    } while (0)
    #define SP_POP()   (stack[--sp])

    #define RELOAD() do { \
        frame = &vm->frames[vm->frame_count - 1]; \
        chunk = frame->chunk; ip = frame->ip; locals = vm->stack + frame->local_base; sp = vm->stack_top; \
    } while(0)

    #define VM_CHECK_EXC() do { \
        if (__builtin_expect(vm->has_pending_exc, 0)) { \
            throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0; \
            goto L_vm_throw_exc; \
        } \
    } while (0)

    #define VM_WORKER_SAFEPOINT(sync_ip) do { \
        if (__builtin_expect(vm->is_worker_vm && vm->root_vm && \
                             __atomic_load_n(&vm->root_vm->gc_requested, __ATOMIC_ACQUIRE), 0)) { \
            frame->ip = (sync_ip); vm->stack_top = sp; \
            vm_worker_park(vm); \
            stack = vm->stack; RELOAD(); \
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
    const VMInsn *mcall_ic_ip = NULL;
    int mcall_is_dyn = 0;
    int mcall_allow_field = 0;
    char mcall_dyn_buf[256];
    VMValue throw_exc = VM_UNDEF_VAL;
    VMValue clcall_val = VM_UNDEF_VAL;
    int clcall_argc = 0;

#ifdef VM_JIT_X64
    #define VM_JIT_HANDLE_RC(_rc, _is_entry) do { \
        if ((_rc) == VM_JIT_OK) { \
            if (vm->frame_count == vm->exec_base) { \
                vm->exec_base = saved_exec_base; \
                VM_EXEC_RETURN(vm->jit_ret_val); \
            } \
            stack = vm->stack; \
            if (vm->stack_top >= vm->stack_cap) { vm_stack_grow(vm, vm->stack_top); stack = vm->stack; } \
            stack[vm->stack_top++] = vm->jit_ret_val; \
            RELOAD(); \
            DISPATCH(); \
        } else if ((_rc) == VM_JIT_EXC) { \
            stack = vm->stack; RELOAD(); VM_CHECK_EXC(); \
        } else { \
            stack = vm->stack; RELOAD(); \
            if (!(_is_entry)) { DISPATCH(); } \
        } \
    } while (0)
    #define VM_TRY_JIT_OSR(tgt_insn) do { \
        VMInsn *_tgt = (tgt_insn); \
        if (__builtin_expect(_tgt <= di && \
                             (__atomic_load_n(&chunk->jit_state, __ATOMIC_ACQUIRE) == 2 || \
                              (chunk->jit_state == 0 && ++chunk->jit_hot >= JIT_HOT_THRESHOLD)), 0)) { \
            if (__atomic_load_n(&chunk->jit_state, __ATOMIC_ACQUIRE) == 0) { \
                vm_jit_try_compile(vm->program, chunk); \
            } \
            uint8_t *_jc = __atomic_load_n(&chunk->jit_code, __ATOMIC_ACQUIRE); \
            VMJitTrampolineFn _tramp = _jc ? vm_jit_get_trampoline() : NULL; \
            uint32_t *_offs = chunk->jit_offs; \
            size_t _tidx = (size_t)(_tgt - chunk->dt_code); \
            if (_jc && _tramp && _offs && _tidx < chunk->dt_len && \
                __atomic_load_n(&chunk->jit_state, __ATOMIC_ACQUIRE) == 2) { \
                frame->ip = _tgt; vm->stack_top = sp; \
                vm->jit_stop_base = vm->frame_count - 1; \
                int _jrc = _tramp(vm, _jc + _offs[_tidx], 0); \
                VM_JIT_HANDLE_RC(_jrc, 0); \
            } \
        } \
    } while (0)
    #define VM_MAYBE_ENTER_JIT_CALLEE() do { \
        if (__builtin_expect((__atomic_load_n(&callee->jit_state, __ATOMIC_ACQUIRE) == 2 || \
                              (callee->jit_state == 0 && ++callee->jit_hot >= JIT_HOT_THRESHOLD)), 0)) { \
            if (__atomic_load_n(&callee->jit_state, __ATOMIC_ACQUIRE) == 0) { \
                vm_jit_try_compile(vm->program, callee); \
            } \
            uint8_t *_jc = __atomic_load_n(&callee->jit_code, __ATOMIC_ACQUIRE); \
            VMJitTrampolineFn _tramp = _jc ? vm_jit_get_trampoline() : NULL; \
            if (_jc && _tramp && __atomic_load_n(&callee->jit_state, __ATOMIC_ACQUIRE) == 2) { \
                vm->stack_top = sp; \
                vm->jit_stop_base = vm->frame_count - 1; \
                int _jrc = _tramp(vm, _jc, 1); \
                VM_JIT_HANDLE_RC(_jrc, 0); \
            } \
        } \
    } while (0)
#else
    #define VM_TRY_JIT_OSR(tgt_insn) ((void)0)
    #define VM_MAYBE_ENTER_JIT_CALLEE() ((void)0)
#endif

#ifdef __GNUC__
    #define DISPATCH() do { di = ip++; goto *di->handler; } while (0)
    #define CASE(op) L_##op
#else
    #define DISPATCH() continue
    #define CASE(op) case op
#endif

#ifdef VM_JIT_X64
    /* the frame is complete here (variadic arguments already packed), so
     * any compiled function, closure or variadic, can start in its JIT code */
    if (__builtin_expect((__atomic_load_n(&chunk->jit_state, __ATOMIC_ACQUIRE) == 2 ||
                          (chunk->jit_state == 0 && ++chunk->jit_hot >= JIT_HOT_THRESHOLD)), 0)) {
        if (__atomic_load_n(&chunk->jit_state, __ATOMIC_ACQUIRE) == 0) {
            vm_jit_try_compile(vm->program, chunk);
        }
        uint8_t *jc = __atomic_load_n(&chunk->jit_code, __ATOMIC_ACQUIRE);
        VMJitTrampolineFn tramp = jc ? vm_jit_get_trampoline() : NULL;
        if (jc && tramp && __atomic_load_n(&chunk->jit_state, __ATOMIC_ACQUIRE) == 2) {
            vm->jit_stop_base = vm->exec_base;
            int jrc = tramp(vm, jc, 1);
            VM_JIT_HANDLE_RC(jrc, 1);
        }
    }
#endif

#ifdef __GNUC__
    DISPATCH();
#else
    for (;;) { di = ip++; switch (di->op) {
#endif

    CASE(OP_NOP): DISPATCH();

    CASE(OP_PUSH_INT): {
        SP_PUSH(VM_MAKE_INT(di->imm.i64));
        DISPATCH();
    }

    CASE(OP_PUSH_I8):
    CASE(OP_PUSH_I32): {
        SP_PUSH(VM_MAKE_INT_FAST(di->imm.i64));
        DISPATCH();
    }

    CASE(OP_CAST_ASSIGN_INT):
    CASE(OP_JMP_IF_INT_LOCAL): {
        VMValue v = locals[di->a];
        if (__builtin_expect(VM_IS_INT(v) || VM_IS_UNDEF(v), 1)) {
            ip = chunk->dt_code + di->b;
        }
        DISPATCH();
    }

    CASE(OP_CAST_INT): {
        VMValue v = stack[sp - 1];
        if (__builtin_expect(!VM_IS_INT(v), 0)) {
            if (VM_IS_PTR(v)) {
                if ((di->op != OP_CAST_ASSIGN_INT || vm_obj_class_name(v) == NULL) &&
                    VM_PTR_TYPE(v) != VM_OBJ_INT64) {
                    frame->ip = ip; vm->stack_top = sp;
                    vm->gc_pause++;
                    int64_t iv = vm_to_int_ov(vm, v, 0);
                    vm->gc_pause--;
                    stack = vm->stack; RELOAD(); VM_CHECK_EXC();
                    stack[sp - 1] = VM_MAKE_INT(iv);
                }
            } else {
                stack[sp - 1] = VM_MAKE_INT(vm_to_int(v));
            }
        }
        DISPATCH();
    }

    CASE(OP_CAST_ASSIGN_NUM):
    CASE(OP_CAST_NUM): {
        VMValue v = stack[sp - 1];
        if (__builtin_expect(!VM_IS_FLOAT(v), 0)) {
            if (VM_IS_PTR(v)) {
                if (di->op != OP_CAST_ASSIGN_NUM || vm_obj_class_name(v) == NULL) {
                    frame->ip = ip; vm->stack_top = sp;
                    vm->gc_pause++;
                    double nv = vm_to_num_ov(vm, v, 0);
                    vm->gc_pause--;
                    stack = vm->stack; RELOAD(); VM_CHECK_EXC();
                    stack[sp - 1] = VM_MAKE_FLOAT(nv);
                }
            } else {
                stack[sp - 1] = VM_MAKE_FLOAT(vm_to_num(v));
            }
        }
        DISPATCH();
    }

    CASE(OP_SUB_LOCAL_I32): {
        uint16_t slot = di->a;
        int32_t imm = di->imm.i32.i0;
        VMValue a = locals[slot];
        if (__builtin_expect(VM_IS_INT(a), 1)) {
            SP_PUSH(VM_MAKE_INT(VM_INT_VAL(a) - (int64_t)imm));
            DISPATCH();
        }
        a = vm_rawint_operand(vm, chunk, slot, a);
        if (VM_IS_INT(a)) { SP_PUSH(VM_MAKE_INT(VM_INT_VAL(a) - (int64_t)imm)); DISPATCH(); }
        if (VM_IS_FLOAT(a)) {
            SP_PUSH(VM_MAKE_FLOAT(VM_FLOAT_VAL(a) - (double)imm));
            DISPATCH();
        }
        if (VM_IS_UNDEF(a)) {
            SP_PUSH(VM_MAKE_INT(-(int64_t)imm));
            DISPATCH();
        }
        frame->ip = ip; vm->stack_top = sp;
        VMValue res = vm_slow_local_imm_arith(vm, a, imm, 0);
        stack = vm->stack; RELOAD(); VM_CHECK_EXC();
        SP_PUSH(res); DISPATCH();
    }

    CASE(OP_ADD_LOCAL_I32): {
        uint16_t slot = di->a;
        int32_t imm = di->imm.i32.i0;
        VMValue a = locals[slot];
        if (__builtin_expect(VM_IS_INT(a), 1)) {
            SP_PUSH(VM_MAKE_INT(VM_INT_VAL(a) + (int64_t)imm));
            DISPATCH();
        }
        a = vm_rawint_operand(vm, chunk, slot, a);
        if (VM_IS_INT(a)) { SP_PUSH(VM_MAKE_INT(VM_INT_VAL(a) + (int64_t)imm)); DISPATCH(); }
        if (VM_IS_FLOAT(a)) {
            SP_PUSH(VM_MAKE_FLOAT(VM_FLOAT_VAL(a) + (double)imm));
            DISPATCH();
        }
        if (VM_IS_UNDEF(a)) {
            SP_PUSH(VM_MAKE_INT((int64_t)imm));
            DISPATCH();
        }
        frame->ip = ip; vm->stack_top = sp;
        VMValue res = vm_slow_local_imm_arith(vm, a, imm, 1);
        stack = vm->stack; RELOAD(); VM_CHECK_EXC();
        SP_PUSH(res); DISPATCH();
    }

    CASE(OP_BIT_AND_LOCAL_I32): {
        uint16_t slot = di->a;
        int32_t imm = di->imm.i32.i0;
        VMValue a = locals[slot];
        if (__builtin_expect(VM_IS_INT(a), 1)) {
            SP_PUSH(VM_MAKE_INT(VM_INT_VAL(a) & (int64_t)imm));
            DISPATCH();
        }
        if (VM_IS_UNDEF(a)) {
            SP_PUSH(VM_MAKE_INT_FAST(0));
            DISPATCH();
        }
        frame->ip = ip; vm->stack_top = sp;
        VMValue res = vm_slow_local_imm_arith(vm, a, imm, 2);
        stack = vm->stack; RELOAD(); VM_CHECK_EXC();
        SP_PUSH(res); DISPATCH();
    }

    CASE(OP_BIT_AND_ADD_I32): {
        uint16_t slot = di->a;
        int32_t mask_imm = di->imm.i32.i0;
        int32_t add_imm = di->imm.i32.i1;
        VMValue a = locals[slot];
        if (__builtin_expect(VM_IS_INT(a), 1)) {
            SP_PUSH(VM_MAKE_INT((VM_INT_VAL(a) & (int64_t)mask_imm) + (int64_t)add_imm));
            DISPATCH();
        }
        if (VM_IS_UNDEF(a)) {
            SP_PUSH(VM_MAKE_INT((int64_t)add_imm));
            DISPATCH();
        }
        frame->ip = ip; vm->stack_top = sp;
        VMValue res = vm_slow_local_imm_arith(vm, a, mask_imm, 2);
        stack = vm->stack; RELOAD(); VM_CHECK_EXC();
        if (__builtin_expect(VM_IS_INT(res), 1)) {
            SP_PUSH(VM_MAKE_INT(VM_INT_VAL(res) + (int64_t)add_imm));
        } else {
            frame->ip = ip; vm->stack_top = sp;
            res = vm_slow_local_imm_arith(vm, res, add_imm, 1);
            stack = vm->stack; RELOAD(); VM_CHECK_EXC();
            SP_PUSH(res);
        }
        DISPATCH();
    }

    CASE(OP_INCR_LT_I32_JMP_T): {
        uint16_t slot = di->a;
        int32_t imm = di->imm.i32.i0;
        uint16_t off = di->b;
        VMValue v = locals[slot];
        int cond;
        if (__builtin_expect(VM_IS_INT(v), 1)) {
            int64_t nv = VM_INT_VAL(v) + 1;
            locals[slot] = VM_MAKE_INT(nv);
            cond = (nv < (int64_t)imm);
        } else if (VM_IS_FLOAT(v)) {
            double nv = VM_FLOAT_VAL(v) + 1.0;
            locals[slot] = VM_MAKE_FLOAT(nv);
            cond = (nv < (double)imm);
        } else {
            frame->ip = ip; vm->stack_top = sp;
            cond = vm_slow_incr_cmp_jmp(vm, slot, 0, imm, 0);
            stack = vm->stack; RELOAD(); VM_CHECK_EXC();
        }
        if (__builtin_expect(cond, 1)) {
            if (__builtin_expect(off >= chunk->dt_len, 0)) {
                fprintf(stderr, "VM error: jump offset %u out of bounds (%zu)\n", (unsigned)off, chunk->dt_len);
                vm->stack_top = sp; vm->exec_base = saved_exec_base; VM_EXEC_RETURN(VM_UNDEF_VAL);
            }
            VMInsn *target = chunk->dt_code + off;
            if (target <= ip) {
                VM_WORKER_SAFEPOINT(target);
                if (__builtin_expect((++sig_poll & 31) == 0 && strada_has_pending_signals(), 0)) {
                    frame->ip = target; vm->stack_top = sp;
                    int __th = vm_c_dispatch_signals(vm, &throw_exc);
                    stack = vm->stack; RELOAD();
                    if (__th) goto L_vm_throw_exc;
                }
            }
            VM_TRY_JIT_OSR(target);
            ip = target;
        }
        DISPATCH();
    }

    CASE(OP_INCR_LE_I32_JMP_T): {
        uint16_t slot = di->a;
        int32_t imm = di->imm.i32.i0;
        uint16_t off = di->b;
        VMValue v = locals[slot];
        int cond;
        if (__builtin_expect(VM_IS_INT(v), 1)) {
            int64_t nv = VM_INT_VAL(v) + 1;
            locals[slot] = VM_MAKE_INT(nv);
            cond = (nv <= (int64_t)imm);
        } else if (VM_IS_FLOAT(v)) {
            double nv = VM_FLOAT_VAL(v) + 1.0;
            locals[slot] = VM_MAKE_FLOAT(nv);
            cond = (nv <= (double)imm);
        } else {
            frame->ip = ip; vm->stack_top = sp;
            cond = vm_slow_incr_cmp_jmp(vm, slot, 1, imm, 0);
            stack = vm->stack; RELOAD(); VM_CHECK_EXC();
        }
        if (__builtin_expect(cond, 1)) {
            if (__builtin_expect(off >= chunk->dt_len, 0)) {
                fprintf(stderr, "VM error: jump offset %u out of bounds (%zu)\n", (unsigned)off, chunk->dt_len);
                vm->stack_top = sp; vm->exec_base = saved_exec_base; VM_EXEC_RETURN(VM_UNDEF_VAL);
            }
            VMInsn *target = chunk->dt_code + off;
            if (target <= ip) {
                VM_WORKER_SAFEPOINT(target);
                if (__builtin_expect((++sig_poll & 31) == 0 && strada_has_pending_signals(), 0)) {
                    frame->ip = target; vm->stack_top = sp;
                    int __th = vm_c_dispatch_signals(vm, &throw_exc);
                    stack = vm->stack; RELOAD();
                    if (__th) goto L_vm_throw_exc;
                }
            }
            VM_TRY_JIT_OSR(target);
            ip = target;
        }
        DISPATCH();
    }

    CASE(OP_INCR_LT_LOCAL_JMP_T): {
        uint16_t sa = di->a;
        uint16_t sb = di->b;
        uint16_t off = di->c;
        VMValue a = locals[sa], b = locals[sb];
        int cond;
        if (__builtin_expect(VM_BOTH_INT(a, b), 1)) {
            int64_t nv = VM_INT_VAL(a) + 1;
            locals[sa] = VM_MAKE_INT(nv);
            cond = (nv < VM_INT_VAL(b));
        } else {
            frame->ip = ip; vm->stack_top = sp;
            cond = vm_slow_incr_cmp_jmp(vm, sa, 2, 0, sb);
            stack = vm->stack; RELOAD(); VM_CHECK_EXC();
        }
        if (__builtin_expect(cond, 1)) {
            if (__builtin_expect(off >= chunk->dt_len, 0)) {
                fprintf(stderr, "VM error: jump offset %u out of bounds (%zu)\n", (unsigned)off, chunk->dt_len);
                vm->stack_top = sp; vm->exec_base = saved_exec_base; VM_EXEC_RETURN(VM_UNDEF_VAL);
            }
            VMInsn *target = chunk->dt_code + off;
            if (target <= ip) {
                VM_WORKER_SAFEPOINT(target);
                if (__builtin_expect((++sig_poll & 31) == 0 && strada_has_pending_signals(), 0)) {
                    frame->ip = target; vm->stack_top = sp;
                    int __th = vm_c_dispatch_signals(vm, &throw_exc);
                    stack = vm->stack; RELOAD();
                    if (__th) goto L_vm_throw_exc;
                }
            }
            VM_TRY_JIT_OSR(target);
            ip = target;
        }
        DISPATCH();
    }

    CASE(OP_MOD_LOCAL_I32): {
        uint16_t slot = di->a;
        int32_t imm = di->imm.i32.i0;
        VMValue a = locals[slot];
        if (__builtin_expect(!VM_IS_INT(a), 0)) a = vm_rawint_operand(vm, chunk, slot, a);
        if (__builtin_expect(VM_IS_INT(a), 1)) {
            int64_t av = VM_INT_VAL(a), bv = (int64_t)imm;
            int64_t r = av % bv;
            if (__builtin_expect(r != 0 && ((r < 0) != (bv < 0)), 0)) r += bv;
            SP_PUSH(VM_MAKE_INT(r));
            DISPATCH();
        }
        frame->ip = ip; vm->stack_top = sp;
        VMValue res = vm_slow_local_imm_arith(vm, a, imm, 3);
        stack = vm->stack; RELOAD(); VM_CHECK_EXC();
        SP_PUSH(res); DISPATCH();
    }

    CASE(OP_ADD_LOCALS): {
        uint16_t sa = di->a;
        uint16_t sb = di->b;
        VMValue a = locals[sa], b = locals[sb];
        if (__builtin_expect(chunk->rawint_slots != NULL && !VM_BOTH_INT(a, b), 0)) {
            a = vm_rawint_operand(vm, chunk, sa, a);
            b = vm_rawint_operand(vm, chunk, sb, b);
        }
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
        frame->ip = ip; vm->stack_top = sp;
        VMValue res = vm_slow_add_locals(vm, a, b);
        stack = vm->stack; RELOAD(); VM_CHECK_EXC();
        SP_PUSH(res); DISPATCH();
    }

    CASE(OP_SUB_LOCALS): {
        uint16_t sa = di->a;
        uint16_t sb = di->b;
        VMValue a = locals[sa], b = locals[sb];
        if (__builtin_expect(chunk->rawint_slots != NULL && !VM_BOTH_INT(a, b), 0)) {
            a = vm_rawint_operand(vm, chunk, sa, a);
            b = vm_rawint_operand(vm, chunk, sb, b);
        }
        if (__builtin_expect(VM_BOTH_INT(a, b), 1)) {
            SP_PUSH(VM_MAKE_INT(VM_INT_VAL(a) - VM_INT_VAL(b)));
            DISPATCH();
        }
        if (__builtin_expect(VM_IS_FLOAT(a) && VM_IS_FLOAT(b), 1)) {
            SP_PUSH(VM_MAKE_FLOAT(VM_FLOAT_VAL(a) - VM_FLOAT_VAL(b)));
            DISPATCH();
        }
        frame->ip = ip; vm->stack_top = sp;
        if (VM_IS_PTR(a) && vm_slow_unwrap_local(vm, &a)) { stack = vm->stack; RELOAD(); VM_CHECK_EXC(); }
        if (VM_IS_PTR(b)) {
            SP_PUSH(a); vm->stack_top = sp;
            int exc = vm_slow_unwrap_local(vm, &b);
            stack = vm->stack; RELOAD(); a = SP_POP();
            if (exc) { VM_CHECK_EXC(); }
        }
        VMValue res = vm_slow_bin_arith(vm, 1, a, b);
        stack = vm->stack; RELOAD(); VM_CHECK_EXC();
        SP_PUSH(res); DISPATCH();
    }

    CASE(OP_MUL_LOCALS): {
        uint16_t sa = di->a;
        uint16_t sb = di->b;
        VMValue a = locals[sa], b = locals[sb];
        if (__builtin_expect(chunk->rawint_slots != NULL && !VM_BOTH_INT(a, b), 0)) {
            a = vm_rawint_operand(vm, chunk, sa, a);
            b = vm_rawint_operand(vm, chunk, sb, b);
        }
        if (__builtin_expect(VM_BOTH_INT(a, b), 1)) {
            int64_t mr;
            if (__builtin_expect(!__builtin_mul_overflow(VM_INT_VAL(a), VM_INT_VAL(b), &mr), 1)) {
                SP_PUSH(VM_MAKE_INT(mr));
            } else {
                SP_PUSH(vm_from_int128((__int128)VM_INT_VAL(a) * VM_INT_VAL(b)));
            }
            DISPATCH();
        }
        if (__builtin_expect(VM_IS_FLOAT(a) && VM_IS_FLOAT(b), 1)) {
            SP_PUSH(VM_MAKE_FLOAT(VM_FLOAT_VAL(a) * VM_FLOAT_VAL(b)));
            DISPATCH();
        }
        frame->ip = ip; vm->stack_top = sp;
        if (VM_IS_PTR(a) && vm_slow_unwrap_local(vm, &a)) { stack = vm->stack; RELOAD(); VM_CHECK_EXC(); }
        if (VM_IS_PTR(b)) {
            SP_PUSH(a); vm->stack_top = sp;
            int exc = vm_slow_unwrap_local(vm, &b);
            stack = vm->stack; RELOAD(); a = SP_POP();
            if (exc) { VM_CHECK_EXC(); }
        }
        VMValue res = vm_slow_bin_arith(vm, 2, a, b);
        stack = vm->stack; RELOAD(); VM_CHECK_EXC();
        SP_PUSH(res); DISPATCH();
    }

    CASE(OP_ADD_TO_LOCAL): {
        uint16_t d = di->a;
        uint16_t s = di->b;
        uint8_t coerce = di->flags;
        VMValue a = locals[d], b = locals[s];
        if (__builtin_expect(VM_BOTH_INT(a, b) && coerce != 2, 1)) {
            locals[d] = VM_MAKE_INT(VM_INT_VAL(a) + VM_INT_VAL(b));
            DISPATCH();
        }
        if (__builtin_expect(VM_IS_FLOAT(a) && VM_IS_FLOAT(b) && coerce != 1, 1)) {
            locals[d] = VM_MAKE_FLOAT(VM_FLOAT_VAL(a) + VM_FLOAT_VAL(b));
            DISPATCH();
        }
        frame->ip = ip; vm->stack_top = sp;
        vm_slow_add_to_local(vm, d, b, coerce, 0);
        stack = vm->stack; RELOAD(); VM_CHECK_EXC();
        DISPATCH();
    }

    CASE(OP_ADD_TO_LOCAL_VAL): {
        uint16_t d = di->a;
        uint8_t coerce = di->flags;
        VMValue rhs = SP_POP();
        VMValue a = locals[d];
        if (__builtin_expect(VM_BOTH_INT(a, rhs) && coerce != 2, 1)) {
            locals[d] = VM_MAKE_INT(VM_INT_VAL(a) + VM_INT_VAL(rhs));
            DISPATCH();
        }
        if (__builtin_expect(VM_IS_FLOAT(a) && VM_IS_FLOAT(rhs) && coerce != 1, 1)) {
            locals[d] = VM_MAKE_FLOAT(VM_FLOAT_VAL(a) + VM_FLOAT_VAL(rhs));
            DISPATCH();
        }
        frame->ip = ip; vm->stack_top = sp;
        vm_slow_add_to_local(vm, d, rhs, coerce, 0);
        stack = vm->stack; RELOAD(); VM_CHECK_EXC();
        DISPATCH();
    }

    CASE(OP_SUB_TO_LOCAL_VAL): {
        uint16_t d = di->a;
        uint8_t coerce = di->flags;
        VMValue rhs = SP_POP();
        VMValue a = locals[d];
        if (__builtin_expect(VM_BOTH_INT(a, rhs) && coerce != 2, 1)) {
            locals[d] = VM_MAKE_INT(VM_INT_VAL(a) - VM_INT_VAL(rhs));
            DISPATCH();
        }
        if (__builtin_expect(VM_IS_FLOAT(a) && VM_IS_FLOAT(rhs) && coerce != 1, 1)) {
            locals[d] = VM_MAKE_FLOAT(VM_FLOAT_VAL(a) - VM_FLOAT_VAL(rhs));
            DISPATCH();
        }
        frame->ip = ip; vm->stack_top = sp;
        vm_slow_add_to_local(vm, d, rhs, coerce, 1);
        stack = vm->stack; RELOAD(); VM_CHECK_EXC();
        DISPATCH();
    }

    CASE(OP_LT_LOCAL_I32_JMP_F): {
        uint16_t slot = di->a;
        int32_t imm = di->imm.i32.i0;
        uint16_t off = di->b;
        VMValue v = locals[slot];
        int cond;
        if (__builtin_expect(VM_IS_INT(v), 1)) {
            cond = (VM_INT_VAL(v) < (int64_t)imm);
        } else if (VM_IS_FLOAT(v)) {
            cond = (VM_FLOAT_VAL(v) < (double)imm);
        } else if (VM_IS_UNDEF(v)) {
            cond = (0 < imm);
        } else {
            frame->ip = ip; vm->stack_top = sp;
            cond = vm_slow_cmp_local_i32(vm, v, imm, 0);
            stack = vm->stack; RELOAD(); VM_CHECK_EXC();
        }
        if (!cond) {
            if (__builtin_expect(off >= chunk->dt_len, 0)) {
                fprintf(stderr, "VM error: jump offset %u out of bounds (%zu)\n", (unsigned)off, chunk->dt_len);
                vm->stack_top = sp; vm->exec_base = saved_exec_base; VM_EXEC_RETURN(VM_UNDEF_VAL);
            }
            VMInsn *target = chunk->dt_code + off;
            if (target <= ip) {
                VM_WORKER_SAFEPOINT(target);
                if (__builtin_expect((++sig_poll & 31) == 0 && strada_has_pending_signals(), 0)) {
                    frame->ip = target; vm->stack_top = sp;
                    int __th = vm_c_dispatch_signals(vm, &throw_exc);
                    stack = vm->stack; RELOAD();
                    if (__th) goto L_vm_throw_exc;
                }
            }
            VM_TRY_JIT_OSR(target);
            ip = target;
        }
        DISPATCH();
    }

    CASE(OP_LE_LOCAL_I32_JMP_F): {
        uint16_t slot = di->a;
        int32_t imm = di->imm.i32.i0;
        uint16_t off = di->b;
        VMValue v = locals[slot];
        int cond;
        if (__builtin_expect(VM_IS_INT(v), 1)) {
            cond = (VM_INT_VAL(v) <= (int64_t)imm);
        } else if (VM_IS_FLOAT(v)) {
            cond = (VM_FLOAT_VAL(v) <= (double)imm);
        } else if (VM_IS_UNDEF(v)) {
            cond = (0 <= imm);
        } else {
            frame->ip = ip; vm->stack_top = sp;
            cond = vm_slow_cmp_local_i32(vm, v, imm, 1);
            stack = vm->stack; RELOAD(); VM_CHECK_EXC();
        }
        if (!cond) {
            if (__builtin_expect(off >= chunk->dt_len, 0)) {
                fprintf(stderr, "VM error: jump offset %u out of bounds (%zu)\n", (unsigned)off, chunk->dt_len);
                vm->stack_top = sp; vm->exec_base = saved_exec_base; VM_EXEC_RETURN(VM_UNDEF_VAL);
            }
            VMInsn *target = chunk->dt_code + off;
            if (target <= ip) {
                VM_WORKER_SAFEPOINT(target);
                if (__builtin_expect((++sig_poll & 31) == 0 && strada_has_pending_signals(), 0)) {
                    frame->ip = target; vm->stack_top = sp;
                    int __th = vm_c_dispatch_signals(vm, &throw_exc);
                    stack = vm->stack; RELOAD();
                    if (__th) goto L_vm_throw_exc;
                }
            }
            VM_TRY_JIT_OSR(target);
            ip = target;
        }
        DISPATCH();
    }

    CASE(OP_LT_LOCAL_LOCAL_JMP_F): {
        uint16_t sa = di->a;
        uint16_t sb = di->b;
        uint16_t off = di->c;
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
            frame->ip = ip; vm->stack_top = sp;
            cond = vm_slow_cmp_vals_lt(vm, a, b);
            stack = vm->stack; RELOAD(); VM_CHECK_EXC();
        }
        if (!cond) {
            if (__builtin_expect(off >= chunk->dt_len, 0)) {
                fprintf(stderr, "VM error: jump offset %u out of bounds (%zu)\n", (unsigned)off, chunk->dt_len);
                vm->stack_top = sp; vm->exec_base = saved_exec_base; VM_EXEC_RETURN(VM_UNDEF_VAL);
            }
            VMInsn *target = chunk->dt_code + off;
            if (target <= ip) {
                VM_WORKER_SAFEPOINT(target);
                if (__builtin_expect((++sig_poll & 31) == 0 && strada_has_pending_signals(), 0)) {
                    frame->ip = target; vm->stack_top = sp;
                    int __th = vm_c_dispatch_signals(vm, &throw_exc);
                    stack = vm->stack; RELOAD();
                    if (__th) goto L_vm_throw_exc;
                }
            }
            VM_TRY_JIT_OSR(target);
            ip = target;
        }
        DISPATCH();
    }

    CASE(OP_PUSH_FLOAT): {
        SP_PUSH(VM_MAKE_FLOAT(di->imm.f64));
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
        uint16_t slot = di->a;
        if (__builtin_expect(slot > (uint16_t)chunk->local_count, 0)) {
            fprintf(stderr, "VM error: local slot %u out of bounds (%d)\n", (unsigned)slot, chunk->local_count);
            vm->stack_top = sp; vm->exec_base = saved_exec_base; VM_EXEC_RETURN(VM_UNDEF_VAL);
        }
        VMValue v = locals[slot];
        if (__builtin_expect(!VM_IS_PTR(v), 1)) { SP_PUSH(v); DISPATCH(); }
        uint8_t ot = ((VMObjHeader*)VM_TO_PTR(v))->obj_type;
        if (__builtin_expect(ot != VM_OBJ_STRBUF && ot != VM_OBJ_CELL &&
                             ot != VM_OBJ_TIED_SCALAR && ot != VM_OBJ_WEAKREF, 1)) {
            SP_PUSH(v); DISPATCH();
        }
        if (ot == VM_OBJ_STRBUF) {
            if (ip->op != OP_STR_LEN) VM_AS_STRBUF(v)->shared = 1;
            SP_PUSH(v); DISPATCH();
        }
        frame->ip = ip; vm->stack_top = sp;
        VMValue r = vm_slow_load_local_ptr(vm, v);
        stack = vm->stack; RELOAD();
        VM_CHECK_EXC();
        SP_PUSH(r); DISPATCH();
    }

    CASE(OP_RESET_LOCAL): {
        uint16_t slot = di->a;
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
        uint16_t slot = di->a;
        if (__builtin_expect(slot > (uint16_t)chunk->local_count, 0)) {
            fprintf(stderr, "VM error: local slot %u out of bounds (%d)\n", (unsigned)slot, chunk->local_count);
            vm->stack_top = sp; vm->exec_base = saved_exec_base; VM_EXEC_RETURN(VM_UNDEF_VAL);
        }
        VMValue old = locals[slot];
        VMValue newv = SP_POP();
        if (__builtin_expect((!VM_IS_PTR(old) || (VM_PTR_TYPE(old) != VM_OBJ_CELL && VM_PTR_TYPE(old) != VM_OBJ_TIED_SCALAR)) &&
                             (!vm->has_weakrefs || !VM_IS_PTR(newv)), 1)) {
            locals[slot] = newv;
            DISPATCH();
        }
        frame->ip = ip; vm->stack_top = sp;
        vm_slow_store_local(vm, slot, old, newv);
        stack = vm->stack; RELOAD();
        VM_CHECK_EXC();
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

    /* After VM_TRY_BIN_OV found no operator overload: a "" overload gives
     * each object operand of a string operator its string. */
    #define VM_STR_OV_PAIR() do { \
        if (__builtin_expect(vm_obj_class_name(a) != NULL || vm_obj_class_name(b) != NULL, 0)) { \
            if (vm_has_stringify(vm->program, a)) { \
                frame->ip = ip; SP_PUSH(b); vm->stack_top = sp; \
                a = vm_stringify_ov(vm, a); \
                stack = vm->stack; RELOAD(); \
                VM_CHECK_EXC(); \
                b = SP_POP(); \
            } \
            if (vm_has_stringify(vm->program, b)) { \
                frame->ip = ip; SP_PUSH(a); vm->stack_top = sp; \
                b = vm_stringify_ov(vm, b); \
                stack = vm->stack; RELOAD(); \
                VM_CHECK_EXC(); \
                a = SP_POP(); \
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
        frame->ip = ip; vm->stack_top = sp;
        VMValue res = vm_slow_bin_arith(vm, 0, a, b);
        stack = vm->stack; RELOAD(); VM_CHECK_EXC();
        SP_PUSH(res); DISPATCH();
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
        frame->ip = ip; vm->stack_top = sp;
        VMValue res = vm_slow_bin_arith(vm, 1, a, b);
        stack = vm->stack; RELOAD(); VM_CHECK_EXC();
        SP_PUSH(res); DISPATCH();
    }
    CASE(OP_MUL): {
        VMValue b = SP_POP(), a = SP_POP();
        if (__builtin_expect(VM_BOTH_INT(a, b), 1)) {
            int64_t mr;
            if (__builtin_expect(!__builtin_mul_overflow(VM_INT_VAL(a), VM_INT_VAL(b), &mr), 1)) {
                SP_PUSH(VM_MAKE_INT(mr));
            } else {
                SP_PUSH(vm_from_int128((__int128)VM_INT_VAL(a) * VM_INT_VAL(b)));
            }
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
        frame->ip = ip; vm->stack_top = sp;
        VMValue res = vm_slow_bin_arith(vm, 2, a, b);
        stack = vm->stack; RELOAD(); VM_CHECK_EXC();
        SP_PUSH(res); DISPATCH();
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
        double da = 0.0, db = 0.0;
        frame->ip = ip; vm->stack_top = sp;
        int pexc = vm_pair_to_num(vm, a, b, &da, &db);
        stack = vm->stack; RELOAD();
        if (__builtin_expect(pexc, 0)) { vm_temp_free(&a); vm_temp_free(&b); VM_CHECK_EXC(); }
        if (db == 0.0) {
            vm_temp_free(&a); vm_temp_free(&b);
            throw_exc = vm_str("Illegal division by zero");
            goto L_vm_throw_exc;
        }
        SP_PUSH(VM_MAKE_FLOAT(da / db));
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
            int64_t r = 0;
            if (!(av == INT64_MIN && bv == -1)) {
                r = av % bv;
                if (r != 0 && ((r < 0) != (bv < 0))) r += bv;
            }
            SP_PUSH(VM_MAKE_INT(r));
            DISPATCH();
        }
        VM_TRY_BIN_OV("%");
        int64_t av = 0, bv = 0;
        frame->ip = ip; vm->stack_top = sp;
        int pexc = vm_pair_to_int(vm, a, b, &av, &bv);
        stack = vm->stack; RELOAD();
        if (__builtin_expect(pexc, 0)) { vm_temp_free(&a); vm_temp_free(&b); VM_CHECK_EXC(); }
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

    /* Bitwise operations — fast paths for inline tagged ints */
    #define VM_BIT_BIN_OP(opstr, fast_expr, slow_expr) do { \
        VMValue b = SP_POP(), a = SP_POP(); \
        if (__builtin_expect(VM_BOTH_INT(a, b), 1)) { \
            int64_t lv = VM_INT_VAL(a), rv = VM_INT_VAL(b); \
            SP_PUSH(VM_MAKE_INT(fast_expr)); \
            DISPATCH(); \
        } \
        VM_TRY_BIN_OV(opstr); \
        int64_t av = 0, bv = 0; \
        if (VM_IS_PTR(a) || VM_IS_PTR(b)) { \
            frame->ip = ip; vm->stack_top = sp; \
            int pexc = vm_pair_to_int(vm, a, b, &av, &bv); \
            stack = vm->stack; RELOAD(); \
            if (__builtin_expect(pexc, 0)) { vm_temp_free(&a); vm_temp_free(&b); VM_CHECK_EXC(); } \
        } else { \
            av = vm_to_int(a); bv = vm_to_int(b); \
        } \
        SP_PUSH(VM_MAKE_INT(slow_expr)); \
        vm_temp_free(&a); vm_temp_free(&b); DISPATCH(); \
    } while (0)
    CASE(OP_BIT_AND): { VM_BIT_BIN_OP("&",  lv & rv, av & bv); }
    CASE(OP_BIT_OR):  { VM_BIT_BIN_OP("|",  lv | rv, av | bv); }
    CASE(OP_BIT_XOR): { VM_BIT_BIN_OP("^",  lv ^ rv, av ^ bv); }
    CASE(OP_BIT_SHL): {
        VM_BIT_BIN_OP("<<",
            (rv < 0 || rv >= 64) ? 0 : (int64_t)((uint64_t)lv << (int)rv),
            (bv < 0 || bv >= 64) ? 0 : (int64_t)((uint64_t)av << (int)bv));
    }
    CASE(OP_BIT_SHR): {
        VM_BIT_BIN_OP(">>",
            (rv < 0 || rv >= 64) ? (lv < 0 ? -1 : 0) : (lv >> (int)rv),
            (bv < 0 || bv >= 64) ? (av < 0 ? -1 : 0) : (av >> (int)bv));
    }
    #undef VM_BIT_BIN_OP
    CASE(OP_BIT_NOT): {
        VMValue a = SP_POP();
        if (__builtin_expect(VM_IS_INT(a), 1)) {
            SP_PUSH(VM_MAKE_INT(~VM_INT_VAL(a)));
            DISPATCH();
        }
        if (__builtin_expect(vm_obj_class_name(a) != NULL, 0)) {
            VMValue _ov_res = VM_UNDEF_VAL;
            frame->ip = ip; vm->stack_top = sp;
            if (vm_try_un_overload(vm, "~", a, &_ov_res)) {
                stack = vm->stack; RELOAD();
                if (__builtin_expect(vm->has_pending_exc, 0)) { vm_temp_free(&a); VM_CHECK_EXC(); }
                vm_temp_free(&a);
                SP_PUSH(_ov_res);
                DISPATCH();
            }
        }
        int64_t iv;
        if (VM_IS_PTR(a)) {
            frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
            iv = vm_to_int_vm(vm, a);
            vm->gc_pause--; stack = vm->stack; RELOAD();
            if (__builtin_expect(vm->has_pending_exc, 0)) { vm_temp_free(&a); VM_CHECK_EXC(); }
        } else {
            iv = vm_to_int(a);
        }
        SP_PUSH(VM_MAKE_INT(~iv));
        vm_temp_free(&a); DISPATCH();
    }

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
        int res; \
        if (VM_IS_ANY_INT(a) && VM_IS_ANY_INT(b)) { \
            res = (VM_ANY_INT_VAL(a) cmpop VM_ANY_INT_VAL(b)); \
        } else { \
            double na = 0.0, nb = 0.0; \
            if (VM_IS_PTR(a) || VM_IS_PTR(b)) { \
                frame->ip = ip; vm->stack_top = sp; \
                int pexc = vm_pair_to_num(vm, a, b, &na, &nb); \
                stack = vm->stack; RELOAD(); \
                if (__builtin_expect(pexc, 0)) { vm_temp_free(&a); vm_temp_free(&b); VM_CHECK_EXC(); } \
            } else { \
                na = vm_to_num(a); nb = vm_to_num(b); \
            } \
            res = (na cmpop nb); \
        } \
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
        } else if (VM_IS_PTR(a) || VM_IS_PTR(b)) {
            double na = 0.0, nb = 0.0;
            frame->ip = ip; vm->stack_top = sp;
            int pexc = vm_pair_to_num(vm, a, b, &na, &nb);
            stack = vm->stack; RELOAD();
            if (__builtin_expect(pexc, 0)) { vm_temp_free(&a); vm_temp_free(&b); VM_CHECK_EXC(); }
            eq = (na == nb);
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
        if (__builtin_expect(VM_IS_PTR(a) && VM_IS_PTR(b) &&
                             VM_PTR_TYPE(a) == VM_OBJ_STR && VM_PTR_TYPE(b) == VM_OBJ_STR, 1)) {
            if (a == b) { SP_PUSH(VM_MAKE_INT_FAST(1)); DISPATCH(); }
            VMString *sa = VM_AS_STR(a), *sb = VM_AS_STR(b);
            SP_PUSH(VM_MAKE_INT_FAST(sa->len == sb->len && (sa->len == 0 || memcmp(sa->data, sb->data, sa->len) == 0)));
            DISPATCH();
        }
        VM_TRY_BIN_OV("eq");
        VM_STR_OV_PAIR();
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
        if (__builtin_expect(VM_IS_PTR(a) && VM_IS_PTR(b) &&
                             VM_PTR_TYPE(a) == VM_OBJ_STR && VM_PTR_TYPE(b) == VM_OBJ_STR, 1)) {
            if (a == b) { SP_PUSH(VM_MAKE_INT_FAST(0)); DISPATCH(); }
            VMString *sa = VM_AS_STR(a), *sb = VM_AS_STR(b);
            SP_PUSH(VM_MAKE_INT_FAST(sa->len != sb->len || (sa->len > 0 && memcmp(sa->data, sb->data, sa->len) != 0)));
            DISPATCH();
        }
        VM_TRY_BIN_OV("ne");
        VM_STR_OV_PAIR();
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
        VM_STR_OV_PAIR();
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
        VM_STR_OV_PAIR();
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
        VM_STR_OV_PAIR();
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
        VM_STR_OV_PAIR();
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
            double av = 0.0, bv = 0.0;
            if (VM_IS_PTR(a) || VM_IS_PTR(b)) {
                frame->ip = ip; vm->stack_top = sp;
                int pexc = vm_pair_to_num(vm, a, b, &av, &bv);
                stack = vm->stack; RELOAD();
                if (__builtin_expect(pexc, 0)) { vm_temp_free(&a); vm_temp_free(&b); VM_CHECK_EXC(); }
            } else {
                av = vm_to_num(a); bv = vm_to_num(b);
            }
            cmp = av < bv ? -1 : (av > bv ? 1 : 0);
        }
        SP_PUSH(VM_MAKE_INT(cmp));
        vm_temp_free(&a); vm_temp_free(&b);              /* also fixes a string-operand leak */
        DISPATCH();
    }
    CASE(OP_STR_CMP): {
        VMValue b = SP_POP(), a = SP_POP();
        VM_TRY_BIN_OV("cmp");
        VM_STR_OV_PAIR();
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
        uint16_t off = di->a;
        if (__builtin_expect(off >= chunk->dt_len, 0)) {
            fprintf(stderr, "VM error: jump offset %u out of bounds (%zu)\n", (unsigned)off, chunk->dt_len);
            vm->stack_top = sp; vm->exec_base = saved_exec_base; VM_EXEC_RETURN(VM_UNDEF_VAL);
        }
        VMInsn *target = chunk->dt_code + off;
        if (target <= ip) {
            VM_WORKER_SAFEPOINT(target);
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
        VM_TRY_JIT_OSR(target);
        ip = target;
        DISPATCH();
    }

    CASE(OP_JMP_IF_FALSE): {
        uint16_t off = di->a;
        VMValue c = SP_POP();
        int _truthy = 0;
        VM_VALUE_IS_TRUTHY(c, _truthy);
        if (!_truthy) {
            if (__builtin_expect(off >= chunk->dt_len, 0)) {
                fprintf(stderr, "VM error: jump offset %u out of bounds (%zu)\n", (unsigned)off, chunk->dt_len);
                vm->stack_top = sp; vm->exec_base = saved_exec_base; VM_EXEC_RETURN(VM_UNDEF_VAL);
            }
            VMInsn *target = chunk->dt_code + off;
            if (target <= ip) {
                VM_WORKER_SAFEPOINT(target);
                if (__builtin_expect((++sig_poll & 31) == 0 && strada_has_pending_signals(), 0)) {
                    frame->ip = target;
                    vm->stack_top = sp;
                    int __th = vm_c_dispatch_signals(vm, &throw_exc);
                    stack = vm->stack;
                    RELOAD();
                    if (__th) goto L_vm_throw_exc;
                }
            }
            VM_TRY_JIT_OSR(target);
            ip = target;
        }
        DISPATCH();
    }

    CASE(OP_JMP_IF_TRUE): {
        uint16_t off = di->a;
        VMValue c = SP_POP();
        int _truthy = 0;
        VM_VALUE_IS_TRUTHY(c, _truthy);
        if (_truthy) {
            if (__builtin_expect(off >= chunk->dt_len, 0)) {
                fprintf(stderr, "VM error: jump offset %u out of bounds (%zu)\n", (unsigned)off, chunk->dt_len);
                vm->stack_top = sp; vm->exec_base = saved_exec_base; VM_EXEC_RETURN(VM_UNDEF_VAL);
            }
            VMInsn *target = chunk->dt_code + off;
            if (target <= ip) {
                VM_WORKER_SAFEPOINT(target);
                if (__builtin_expect((++sig_poll & 31) == 0 && strada_has_pending_signals(), 0)) {
                    frame->ip = target;
                    vm->stack_top = sp;
                    int __th = vm_c_dispatch_signals(vm, &throw_exc);
                    stack = vm->stack;
                    RELOAD();
                    if (__th) goto L_vm_throw_exc;
                }
            }
            VM_TRY_JIT_OSR(target);
            ip = target;
        }
        DISPATCH();
    }

    CASE(OP_SET_CALL_CTX): {
        uint8_t cctx = di->flags;
        vm->pending_call_context = (cctx == 255) ? frame->call_context : cctx;
        DISPATCH();
    }

    CASE(OP_CALL): {
        call_fidx = di->a;
        call_argc = di->flags;
        goto L_do_op_call;
    }

    CASE(OP_CALL_SPREAD): {
        call_fidx = di->a;
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
        frame->ip = ip;

        if (__builtin_expect(!__atomic_load_n(&callee->dt_code, __ATOMIC_ACQUIRE) ||
                             callee->dt_code_len_src != callee->code_len, 0)) {
            vm_chunk_ensure_dt(vm->program, callee, dt_ptr);
        }
        if (__builtin_expect(vm->recursion_limit > 0 && (int)vm->frame_count >= vm->recursion_limit, 0)) {
            fprintf(stderr, "Deep recursion detected (limit %d) — see core::set_recursion_limit\n", vm->recursion_limit);
            exit(1);
        }
        if (__builtin_expect(vm->frame_count >= vm->frame_cap, 0)) {
            vm->stack_top = sp;
            vm_frames_grow(vm);
        }
        VMFrame *nf = &vm->frames[vm->frame_count++];
        nf->chunk = callee;
        nf->ip = callee->dt_code;
        size_t arg_base = sp - (size_t)argc;
        nf->stack_base = arg_base;
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
            if (__builtin_expect(arg_base + (size_t)lcnt >= vm->stack_cap, 0)) {
                vm->stack_top = sp;
                vm_stack_grow(vm, arg_base + (size_t)lcnt);
                stack = vm->stack;
            }
            if (zf < lcnt) {
                int rem = lcnt - zf;
                VMValue *zp = stack + arg_base + zf;
                if (rem == 1) zp[0] = 0;
                else if (rem == 2) { zp[0] = 0; zp[1] = 0; }
                else memset(zp, 0, (size_t)rem * sizeof(VMValue));
            }
            nf->local_base = arg_base;
            sp = arg_base + (size_t)lcnt;
        } else {
            vm->stack_top = sp;
            vm_call_variadic_setup(vm, nf, callee, argc, &sp);
            stack = vm->stack;
        }

        frame = nf; chunk = callee; ip = callee->dt_code; locals = stack + arg_base;
        VM_MAYBE_ENTER_JIT_CALLEE();
        DISPATCH();
    }

    CASE(OP_TAIL_CALL): {
        uint16_t fidx = di->a;
        uint8_t argc = di->flags;
        VMChunk *callee = &vm->program->funcs[fidx];
        uint8_t tc_ctx = vm->pending_call_context;
        vm->pending_call_context = 0;
        if (__builtin_expect(!__atomic_load_n(&callee->dt_code, __ATOMIC_ACQUIRE) ||
                             callee->dt_code_len_src != callee->code_len, 0)) {
            vm_chunk_ensure_dt(vm->program, callee, dt_ptr);
        }
        if (__builtin_expect(vm->global_save_count > frame->global_save_depth, 0)) {
            frame->ip = ip;
            vm->stack_top = sp;
            vm->gc_pause++;
            vm_unwind_global_saves(vm, frame->global_save_depth);
            vm->gc_pause--;
            stack = vm->stack;
            frame = &vm->frames[vm->frame_count - 1];
        }
        size_t base = frame->stack_base;
        if (__builtin_expect(!callee->has_variadic, 1)) {
            int lcnt = callee->local_count + 1;
            int pmax = lcnt;
            if (callee->fixed_param_count >= 0 && callee->fixed_param_count < pmax) {
                pmax = callee->fixed_param_count;
            }
            int zf = (int)argc < pmax ? (int)argc : pmax;
            if (__builtin_expect(base + (size_t)lcnt >= vm->stack_cap, 0)) {
                vm->stack_top = sp;
                vm_stack_grow(vm, base + (size_t)lcnt);
                stack = vm->stack;
                frame = &vm->frames[vm->frame_count - 1];
            }
            size_t arg_src = sp - (size_t)argc;
            if (zf == 1) {
                stack[base] = stack[arg_src];
            } else if (zf == 2) {
                VMValue a0 = stack[arg_src], a1 = stack[arg_src + 1];
                stack[base] = a0; stack[base + 1] = a1;
            } else if (zf > 2) {
                memmove(stack + base, stack + arg_src, (size_t)zf * sizeof(VMValue));
            }
            if (zf < lcnt) {
                int rem = lcnt - zf;
                VMValue *zp = stack + base + zf;
                if (rem == 1) zp[0] = 0;
                else if (rem == 2) { zp[0] = 0; zp[1] = 0; }
                else memset(zp, 0, (size_t)rem * sizeof(VMValue));
            }
            sp = base + (size_t)lcnt;
        } else {
            vm->stack_top = sp;
            vm_tail_call_variadic_setup(vm, callee, argc, base, &sp);
            stack = vm->stack;
            frame = &vm->frames[vm->frame_count - 1];
        }
        frame->chunk = callee;
        frame->ip = callee->dt_code;
        frame->local_base = base;
        frame->closure = NULL;
        frame->passed_argc = argc;
        frame->call_context = tc_ctx;
        chunk = callee; ip = callee->dt_code; locals = stack + base;
        VM_WORKER_SAFEPOINT(ip);
        if (__builtin_expect((++sig_poll & 31) == 0 && strada_has_pending_signals(), 0)) {
            frame->ip = ip; vm->stack_top = sp;
            int __th = vm_c_dispatch_signals(vm, &throw_exc);
            stack = vm->stack; RELOAD();
            if (__th) goto L_vm_throw_exc;
        }
        if (__builtin_expect(vm->gc_count >= vm->gc_threshold && vm->exec_base == 0 && vm->gc_pause == 0, 0)) {
            frame->ip = ip; vm->stack_top = sp;
            vm_gc_collect(vm);
            stack = vm->stack; RELOAD();
        }
        VM_MAYBE_ENTER_JIT_CALLEE();
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
        }
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
        frame = &vm->frames[vm->frame_count - 1];
        chunk = frame->chunk; ip = frame->ip; locals = stack + frame->local_base;
        VM_WORKER_SAFEPOINT(ip);
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
        VMFileHandle *out_fh = NULL;
        FILE *outfp = stdout;
        if (VM_IS_PTR(vm->default_fh) && VM_PTR_TYPE(vm->default_fh) == VM_OBJ_FILEHANDLE) {
            out_fh = (VMFileHandle*)VM_TO_PTR(vm->default_fh);
            outfp = (FILE*)out_fh->fp;
        }
        vm_write_val(outfp, v, 1);
        /* say to stdout flushes (compiled strada_say): output stays in
         * program order with stderr, system() and fork() children */
        if (outfp == stdout) fflush(stdout);
        vm_temp_free(&v);
        if (__builtin_expect(out_fh && out_fh->is_mem == 2 && VM_IS_PTR(out_fh->mem_sref), 0)) {
            frame->ip = ip; vm->stack_top = sp;
            vm_fh_sync_mem_sref(vm, out_fh);
            stack = vm->stack; RELOAD(); VM_CHECK_EXC();
        }
        DISPATCH();
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
        VMFileHandle *out_fh = NULL;
        FILE *outfp = stdout;
        if (VM_IS_PTR(vm->default_fh) && VM_PTR_TYPE(vm->default_fh) == VM_OBJ_FILEHANDLE) {
            out_fh = (VMFileHandle*)VM_TO_PTR(vm->default_fh);
            outfp = (FILE*)out_fh->fp;
        }
        vm_write_val(outfp, v, 0);
        vm_temp_free(&v);
        if (__builtin_expect(out_fh && out_fh->is_mem == 2 && VM_IS_PTR(out_fh->mem_sref), 0)) {
            frame->ip = ip; vm->stack_top = sp;
            vm_fh_sync_mem_sref(vm, out_fh);
            stack = vm->stack; RELOAD(); VM_CHECK_EXC();
        }
        DISPATCH();
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
            if ((FILE*)fh->fp == stdout) fflush(stdout);
            vm_val_free(&str);
            if (__builtin_expect(fh->is_mem == 2 && VM_IS_PTR(fh->mem_sref), 0)) {
                frame->ip = ip; vm->stack_top = sp;
                vm_fh_sync_mem_sref(vm, fh);
                stack = vm->stack; RELOAD(); VM_CHECK_EXC();
            }
        } else if (VM_IS_PTR(fhv) && VM_PTR_TYPE(fhv) == VM_OBJ_NATIVE_SV) {
            /* a runtime handle (socket, runtime filehandle): write through
             * the runtime so its buffering and ordering rules apply */
            StradaValue *nfh = ((VMNativeSV*)VM_TO_PTR(fhv))->sv;
            StradaValue *ssv = vm_to_sv(str);
            if (nfh && !STRADA_IS_TAGGED_INT(nfh)
                && (nfh->type == STRADA_SOCKET || nfh->type == STRADA_FILEHANDLE)) {
                strada_say_fh(ssv, nfh);
            }
            strada_decref(ssv);
            vm_val_free(&str);
        } else {
            /* `say(A, B)` where A is not a handle: both are data for stdout
             * (the compiled backend decides at run time the same way) */
            if (__builtin_expect(vm_has_stringify(vm->program, fhv), 0)) {
                frame->ip = ip; vm->stack_top = sp;
                SP_PUSH(str); vm->stack_top = sp;
                fhv = vm_stringify_ov(vm, fhv);
                stack = vm->stack; RELOAD();
                if (__builtin_expect(vm->has_pending_exc, 0)) {
                    throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                    goto L_vm_throw_exc;
                }
                str = SP_POP();
            }
            vm_write_val(stdout, fhv, 0);
            vm_write_val(stdout, str, 1);
            fflush(stdout);
            vm_val_free(&str);
        }
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
            vm_val_free(&str);
            if (__builtin_expect(fh->is_mem == 2 && VM_IS_PTR(fh->mem_sref), 0)) {
                frame->ip = ip; vm->stack_top = sp;
                vm_fh_sync_mem_sref(vm, fh);
                stack = vm->stack; RELOAD(); VM_CHECK_EXC();
            }
        } else if (VM_IS_PTR(fhv) && VM_PTR_TYPE(fhv) == VM_OBJ_NATIVE_SV) {
            /* a runtime handle (socket, runtime filehandle): write through
             * the runtime so its buffering and ordering rules apply */
            StradaValue *nfh = ((VMNativeSV*)VM_TO_PTR(fhv))->sv;
            StradaValue *ssv = vm_to_sv(str);
            if (nfh && !STRADA_IS_TAGGED_INT(nfh)
                && (nfh->type == STRADA_SOCKET || nfh->type == STRADA_FILEHANDLE)) {
                strada_print_fh(ssv, nfh);
            }
            strada_decref(ssv);
            vm_val_free(&str);
        } else {
            /* `print(A, B)` where A is not a handle: both are data for stdout
             * (the compiled backend decides at run time the same way) */
            if (__builtin_expect(vm_has_stringify(vm->program, fhv), 0)) {
                frame->ip = ip; vm->stack_top = sp;
                SP_PUSH(str); vm->stack_top = sp;
                fhv = vm_stringify_ov(vm, fhv);
                stack = vm->stack; RELOAD();
                if (__builtin_expect(vm->has_pending_exc, 0)) {
                    throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                    goto L_vm_throw_exc;
                }
                str = SP_POP();
            }
            vm_write_val(stdout, fhv, 0);
            vm_write_val(stdout, str, 0);
            fflush(stdout);
            vm_val_free(&str);
        }
        DISPATCH();
    }

    CASE(OP_CONCAT): {
        VMValue b = SP_POP(), a = SP_POP();
        if (__builtin_expect(VM_IS_PTR(a) && VM_IS_PTR(b) &&
                             VM_PTR_TYPE(a) == VM_OBJ_STR && VM_PTR_TYPE(b) == VM_OBJ_STR, 1)) {
            VMString *sa_s = VM_AS_STR(a), *sb_s = VM_AS_STR(b);
            size_t la = sa_s->len, lb = sb_s->len;
            int u8 = sa_s->hdr.is_utf8 | sb_s->hdr.is_utf8;
            if (la == 0) {
                SP_PUSH((u8 && !sb_s->hdr.is_utf8) ? vm_make_str_flag(sb_s->data, lb, 1) : b);
                DISPATCH();
            }
            if (lb == 0) {
                SP_PUSH((u8 && !sa_s->hdr.is_utf8) ? vm_make_str_flag(sa_s->data, la, 1) : a);
                DISPATCH();
            }
            VMString *r = vm_str_alloc(la + lb);
            if (u8) r->hdr.is_utf8 = 1;
            r->len = (uint32_t)(la + lb);
            memcpy(r->data, sa_s->data, la);
            memcpy(r->data + la, sb_s->data, lb);
            r->data[la + lb] = '\0';
            SP_PUSH(VM_MAKE_PTR(r));
            DISPATCH();
        }
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
        int out_u8 = (VM_IS_PTR(a) && ((VMObjHeader*)VM_TO_PTR(a))->is_utf8) ||
                     (VM_IS_PTR(b) && ((VMObjHeader*)VM_TO_PTR(b))->is_utf8);
        if (la + lb == 0) {
            vm_val_free(&a); vm_val_free(&b);
            SP_PUSH(vm_get_empty_str(out_u8));
            DISPATCH();
        }
        if (la + lb == 1) {
            unsigned char ch = (unsigned char)(la ? sa[0] : sb[0]);
            if (ch < 128) {
                vm_val_free(&a); vm_val_free(&b);
                SP_PUSH(vm_get_ascii_str(ch, out_u8));
                DISPATCH();
            }
        }
        VMString *r = vm_str_alloc(la + lb);
        if (out_u8) r->hdr.is_utf8 = 1;
        r->len = la + lb;
        memcpy(r->data, sa, la); memcpy(r->data + la, sb, lb); r->data[la+lb] = '\0';
        vm_val_free(&a); vm_val_free(&b);
        SP_PUSH(VM_MAKE_PTR(r));
        DISPATCH();
    }

    CASE(OP_PUSH_STR): {
        uint16_t idx = di->a;
        if (__builtin_expect(idx >= chunk->str_const_count, 0)) {
            fprintf(stderr, "VM error: str const %u out of bounds (%zu)\n", (unsigned)idx, chunk->str_const_count);
            vm->stack_top = sp; vm->exec_base = saved_exec_base; VM_EXEC_RETURN(VM_UNDEF_VAL);
        }
        SP_PUSH(chunk->str_const_vals[idx]);
        DISPATCH();
    }

    CASE(OP_INCR): {
        uint16_t s = di->a;
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
            *islot = vm_from_int128((__int128)VM_ANY_INT_VAL(*islot) + 1);
        } else if (VM_IS_PTR(*islot) && VM_PTR_TYPE(*islot) == VM_OBJ_UINT64) {
            *islot = vm_from_int128((__int128)VM_AS_UINT64(*islot)->val + 1);
        } else if (VM_IS_PTR(*islot) && (VM_PTR_TYPE(*islot) == VM_OBJ_STR || VM_PTR_TYPE(*islot) == VM_OBJ_STRBUF)) {
            /* Perl's magic string increment ("az"++ is "ba") and numeric
             * strings, via the runtime's strada_inc_value */
            StradaValue *isv = vm_to_sv(*islot);
            StradaValue *inr = strada_inc_value(isv, 1);
            strada_decref(isv);
            VMValue old = *islot;
            *islot = inr ? sv_to_vm(inr) : VM_UNDEF_VAL;
            if (inr) strada_decref(inr);
            vm_temp_free(&old);
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
        uint16_t s = di->a;
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
        uint16_t s = di->a;
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
            *islot = vm_from_int128((__int128)VM_ANY_INT_VAL(old) + 1);
        } else if (VM_IS_PTR(old) && VM_PTR_TYPE(old) == VM_OBJ_UINT64) {
            *islot = vm_from_int128((__int128)VM_AS_UINT64(old)->val + 1);
        } else if (VM_IS_PTR(old) && (VM_PTR_TYPE(old) == VM_OBJ_STR || VM_PTR_TYPE(old) == VM_OBJ_STRBUF)) {
            /* magic string ++; the old string moves to the stack */
            StradaValue *isv = vm_to_sv(old);
            StradaValue *inr = strada_inc_value(isv, 1);
            strada_decref(isv);
            *islot = inr ? sv_to_vm(inr) : VM_UNDEF_VAL;
            if (inr) strada_decref(inr);
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
        uint16_t s = di->a;
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
    CASE(OP_NEW_ARRAY): { uint32_t c = di->imm.u32; SP_PUSH(VM_MAKE_PTR(vm_array_new(c > 0 ? (int)c : 4))); DISPATCH(); }
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
                    int ssz = sa->size;
                    for (int i = 0; i < ssz; i++) {
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
                    int ssz = sa->size;
                    for (int i = 0; i < ssz; i++) {
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
    /* An array index that is an object: its 0+ (or "" string) number */
    #define VM_IDX_OV(IDX, PUSH_OTHERS, POP_OTHERS) do { \
        if (__builtin_expect(VM_IS_PTR(IDX) && vm->program->overload_count > 0 && \
                             vm_obj_class_name(IDX) != NULL, 0)) { \
            frame->ip = ip; PUSH_OTHERS; vm->stack_top = sp; \
            int64_t _iv = vm_to_int_vm(vm, IDX); \
            stack = vm->stack; RELOAD(); \
            VM_CHECK_EXC(); \
            POP_OTHERS; \
            IDX = VM_MAKE_INT(_iv); \
        } \
    } while (0)

    CASE(OP_ARRAY_GET): {
        VMValue idx = SP_POP(), arr = SP_POP();
        VM_IDX_OV(idx, SP_PUSH(arr), arr = SP_POP());
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
                if (__builtin_expect(VM_IS_INT(idx) && !vm->has_weakrefs, 1)) {
                    int64_t ii = VM_INT_VAL(idx);
                    if (__builtin_expect((uint64_t)ii < (uint64_t)a->size, 1)) {
                        VMValue e = a->items[ii];
                        if (__builtin_expect(VM_IS_PTR(e) && VM_PTR_TYPE(e) == VM_OBJ_STRBUF, 0)) {
                            VM_AS_STRBUF(e)->shared = 1;
                        }
                        SP_PUSH(e);
                        DISPATCH();
                    }
                }
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
        VM_IDX_OV(idx, { SP_PUSH(arr); SP_PUSH(v); }, { v = SP_POP(); arr = SP_POP(); });
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
        } else if (VM_IS_PTR(a) && VM_PTR_TYPE(a) == VM_OBJ_HASH) {
            /* scalar(%h): the key count; a tied hash asks SCALAR (or
             * FIRSTKEY), as in Perl. It was 0 for every hash. */
            VMHash *h = VM_AS_HASH(a);
            if (__builtin_expect(!VM_IS_UNDEF(h->tied_obj), 0)) {
                frame->ip = ip; vm->stack_top = sp;
                VMValue r = vm_tied_hash_scalar(vm, h);
                stack = vm->stack; RELOAD();
                VM_CHECK_EXC();
                SP_PUSH(r);
            } else {
                SP_PUSH(VM_MAKE_INT(h->size));
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
        if (__builtin_expect(vm_obj_class_name(sep) != NULL && vm_has_stringify(vm->program, sep), 0)) {
            frame->ip = ip; SP_PUSH(arr); vm->stack_top = sp;
            sep = vm_stringify_ov(vm, sep);
            stack = vm->stack; RELOAD();
            VM_CHECK_EXC();
            arr = SP_POP();
        }
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
                VMValue it = items[i];
                if (__builtin_expect(vm->program->overload_count > 0 && vm_obj_class_name(it) != NULL &&
                                     vm_has_stringify(vm->program, it), 0)) {
                    /* a "" overload stringifies the element; one that throws propagates */
                    frame->ip = ip; vm->stack_top = sp; vm->gc_pause++;
                    it = vm_stringify_ov(vm, it);
                    vm->gc_pause--;
                    stack = vm->stack; RELOAD();
                    if (vm->has_pending_exc) {
                        for (int k = 0; k < i; k++) free(bufs[k]);
                        free(bufs); free(lens); free(titems);
                        VM_CHECK_EXC();
                    }
                }
                const char *s = vm_bytes_view(it, tmp, sizeof(tmp), &elen);
                if (vm_is_utf8(it)) out_u8 = 1;
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
        uint16_t func_idx = di->a;
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
    CASE(OP_NEW_HASH): { uint32_t c = di->imm.u32; SP_PUSH(VM_MAKE_PTR(vm_hash_new(c > 0 ? (int)c : 4))); DISPATCH(); }
    CASE(OP_HASH_GET_CONST): {
        uint16_t kidx = di->a;
        VMValue hash = SP_POP();
        if (__builtin_expect(VM_IS_PTR(hash) && VM_PTR_TYPE(hash) == VM_OBJ_HASH, 1)) {
            VMHash *h = VM_AS_HASH(hash);
            if (__builtin_expect(!VM_IS_UNDEF(h->tied_obj), 0)) {
                VMValue key = chunk->str_const_vals[kidx];
                int ffidx = vm_tied_array_find_method(vm, h->tied_obj, "FETCH");
                if (ffidx >= 0) {
                    frame->ip = ip; vm->stack_top = sp;
                    VMValue cargs[2] = { h->tied_obj, key };
                    VMValue r = vm_execute_call(vm, ffidx, cargs, 2);
                    stack = vm->stack; RELOAD();
                    VM_CHECK_EXC();
                    SP_PUSH(r);
                    DISPATCH();
                }
            }
            VMString *kstr = VM_AS_STR(chunk->str_const_vals[kidx]);
            uint32_t kh = ((const VMConstString*)((const char*)kstr - offsetof(VMConstString, str)))->hash;
            VMHashEntry *e = vm_hash_probe_first(h, kh);
            VMValue r;
            if (__builtin_expect(e && e->occupied == 1 && e->hash == kh &&
                                 e->keylen == kstr->len &&
                                 memcmp(e->key, kstr->data, kstr->len) == 0, 1)) {
                r = e->value;
            } else {
                uint32_t hic = (uint32_t)(((uintptr_t)di >> 3) & 63);
                uint32_t hs = vm->hash_ic[hic].slot;
                if (__builtin_expect(vm->hash_ic[hic].ip == di && hs < (uint32_t)h->capacity, 1)) {
                    VMHashEntry *ce = &h->entries[hs];
                    if (__builtin_expect(ce->occupied == 1 && ce->hash == kh &&
                                         ce->keylen == kstr->len &&
                                         memcmp(ce->key, kstr->data, kstr->len) == 0, 1)) {
                        r = ce->value;
                        goto L_hget_const_found;
                    }
                }
                int32_t idx = vm_hash_slot_idx_nh(h, kstr->data, kstr->len, kh);
                if (idx < 0) { SP_PUSH(VM_UNDEF_VAL); DISPATCH(); }
                vm->hash_ic[hic].ip = di;
                vm->hash_ic[hic].slot = (uint32_t)idx;
                r = h->entries[idx].value;
            }
        L_hget_const_found:
            if (__builtin_expect(vm->has_weakrefs && VM_IS_PTR(r) && VM_PTR_TYPE(r) == VM_OBJ_WEAKREF, 0)) {
                frame->ip = ip; vm->stack_top = sp;
                VMValue roots[2] = { hash, chunk->str_const_vals[kidx] };
                r = vm_unwrap_weak_gc(vm, r, roots, 2);
                stack = vm->stack; RELOAD();
            }
            if (__builtin_expect(VM_IS_PTR(r) && VM_PTR_TYPE(r) == VM_OBJ_STRBUF, 0)) {
                VM_AS_STRBUF(r)->shared = 1;
            }
            SP_PUSH(r);
        } else {
            SP_PUSH(VM_UNDEF_VAL);
        }
        DISPATCH();
    }
    CASE(OP_HASH_SET_CONST): {
        uint16_t kidx = di->a;
        VMValue v = SP_POP(), hash = SP_POP();
        if (__builtin_expect(VM_IS_PTR(hash) && VM_PTR_TYPE(hash) == VM_OBJ_HASH, 1)) {
            VMHash *h = VM_AS_HASH(hash);
            if (__builtin_expect(!VM_IS_UNDEF(h->tied_obj), 0)) {
                VMValue key = chunk->str_const_vals[kidx];
                int sfidx = vm_tied_array_find_method(vm, h->tied_obj, "STORE");
                if (sfidx >= 0) {
                    frame->ip = ip; vm->stack_top = sp;
                    VMValue cargs[3] = { h->tied_obj, key, v };
                    vm_execute_call(vm, sfidx, cargs, 3);
                    stack = vm->stack; RELOAD();
                    VM_CHECK_EXC();
                    DISPATCH();
                }
            }
            VMString *kstr = VM_AS_STR(chunk->str_const_vals[kidx]);
            uint32_t kh = ((const VMConstString*)((const char*)kstr - offsetof(VMConstString, str)))->hash;
            VMHashEntry *e = vm_hash_probe_first(h, kh);
            if (__builtin_expect(e && e->occupied == 1 && e->hash == kh &&
                                 e->keylen == kstr->len &&
                                 memcmp(e->key, kstr->data, kstr->len) == 0, 1)) {
                e->value = v;
            } else if (__builtin_expect(!e && (h->free_head >= 0 || h->used < h->capacity) &&
                                        !vm->is_worker_vm && !vm->workers_head, 1)) {
                e = vm_hash_append_slot(h, kh);
                uint32_t pid = vm->program->prog_id;
                uint32_t cidx = (uint32_t)(((uintptr_t)kstr >> 4) & 127);
                const char *ikey = NULL;
                if (__builtin_expect(vm->str_intern_cache[cidx].kstr == kstr &&
                                     vm->str_intern_cache[cidx].prog_id == pid, 1)) {
                    ikey = vm->str_intern_cache[cidx].ikey;
                } else if (kstr->len <= VM_KEY_INTERN_MAXLEN) {
                    ikey = vm_intern_hash_key(vm, kstr->data, kstr->len, kh);
                    if (ikey) {
                        vm->str_intern_cache[cidx].kstr = kstr;
                        vm->str_intern_cache[cidx].prog_id = pid;
                        vm->str_intern_cache[cidx].ikey = ikey;
                    }
                }
                if (ikey) {
                    e->key = (char *)ikey;
                    e->key_interned = 1;
                } else {
                    char *k = malloc(kstr->len + 1);
                    if (kstr->len > 0) memcpy(k, kstr->data, kstr->len);
                    k[kstr->len] = '\0';
                    e->key = k;
                    e->key_interned = 0;
                }
                e->value = v;
                e->hash = kh;
                e->keylen = (uint32_t)kstr->len;
                e->key_bytes = (uint8_t)!kstr->hdr.is_utf8;
                e->occupied = 1;
                h->size++;
            } else {
                uint32_t hic = (uint32_t)(((uintptr_t)di >> 3) & 63);
                uint32_t hs = vm->hash_ic[hic].slot;
                if (__builtin_expect(vm->hash_ic[hic].ip == di && hs < (uint32_t)h->capacity, 1)) {
                    VMHashEntry *ce = &h->entries[hs];
                    if (__builtin_expect(ce->occupied == 1 && ce->hash == kh &&
                                         ce->keylen == kstr->len &&
                                         memcmp(ce->key, kstr->data, kstr->len) == 0, 1)) {
                        ce->value = v;
                        DISPATCH();
                    }
                }
                uint32_t ins = vm_hash_set_nh_slot(h, kstr->data, kstr->len, kh, v);
                if (ins != UINT32_MAX) {
                    vm->hash_ic[hic].ip = di;
                    vm->hash_ic[hic].slot = ins;
                }
            }
        }
        DISPATCH();
    }
    /* A hash key that is an object with a "" overload is its string
     * (every compile path: slices, nested autovivification, ...). The rest
     * of the stack (OTHERS) stays visible to the GC during the call. */
    #define VM_KEY_OV(KEY, PUSH_OTHERS, POP_OTHERS) do { \
        if (__builtin_expect(vm->program->overload_count > 0 && vm_obj_class_name(KEY) != NULL, 0) && \
            vm_has_stringify(vm->program, KEY)) { \
            frame->ip = ip; PUSH_OTHERS; vm->stack_top = sp; \
            KEY = vm_stringify_ov(vm, KEY); \
            stack = vm->stack; RELOAD(); \
            VM_CHECK_EXC(); \
            POP_OTHERS; \
        } \
    } while (0)

    CASE(OP_HASH_GET): {
        VMValue key = SP_POP(), hash = SP_POP();
        VM_KEY_OV(key, SP_PUSH(hash), hash = SP_POP());
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
        VM_KEY_OV(key, { SP_PUSH(hash); SP_PUSH(v); }, { v = SP_POP(); hash = SP_POP(); });
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
                g_vm_key_bytes_hint = kstr->hdr.is_utf8 ? NULL : kstr->data;
                vm_hash_set_nh(h, kstr->data, kstr->len, kh, v);
                g_vm_key_bytes_hint = NULL;
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
        VM_KEY_OV(key, SP_PUSH(hash), hash = SP_POP());
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
            h->iter_pos = 0;   /* keys() resets the each() iterator (Perl) */
            SP_PUSH(VM_MAKE_PTR(vm_hash_keys(h)));
        } else {
            SP_PUSH(VM_MAKE_PTR(vm_array_new(0)));
        }
        DISPATCH();
    }
    CASE(OP_HASH_EXISTS): {
        VMValue key = SP_POP(), hash = SP_POP();
        VM_KEY_OV(key, SP_PUSH(hash), hash = SP_POP());
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
        if (__builtin_expect(VM_IS_PTR(v) && VM_PTR_TYPE(v) == VM_OBJ_STR && !VM_AS_STR(v)->hdr.is_utf8, 1)) {
            SP_PUSH(VM_MAKE_INT_FAST((int64_t)VM_AS_STR(v)->len));
            DISPATCH();
        }
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
        /* Range rule shared with the compiled backend (strada_substr_span):
         * undef when the range lies before the string or starts past it. */
        VMValue len_v = SP_POP(), start_v = SP_POP(), str_v = SP_POP();
        int64_t ss_off, ss_cnt;
        if (__builtin_expect(VM_IS_PTR(str_v) && VM_PTR_TYPE(str_v) == VM_OBJ_STR &&
                             !VM_AS_STR(str_v)->hdr.is_utf8 && VM_BOTH_INT(start_v, len_v), 1)) {
            VMString *vs = VM_AS_STR(str_v);
            if (!strada_substr_span((int64_t)vs->len, VM_INT_VAL(start_v), VM_INT_VAL(len_v), &ss_off, &ss_cnt)) {
                SP_PUSH(VM_UNDEF_VAL);
                DISPATCH();
            }
            SP_PUSH(vm_make_str_flag(vs->data + ss_off, (size_t)ss_cnt, 0));
            DISPATCH();
        }
        char buf[64];
        size_t blen = 0;
        const char *s = vm_bytes_view(str_v, buf, sizeof(buf), &blen);
        int64_t start = vm_to_int(start_v);
        int64_t len = vm_to_int(len_v);
        if (vm_str_is_utf8_chars(str_v, s, blen)) {
            int64_t clen = (int64_t)vm_utf8_char_count(s, blen);
            if (!strada_substr_span(clen, start, len, &ss_off, &ss_cnt)) {
                SP_PUSH(VM_UNDEF_VAL);
            } else {
                size_t b0 = vm_utf8_char_to_byte(s, blen, ss_off);
                size_t b1 = vm_utf8_char_to_byte(s, blen, ss_off + ss_cnt);
                SP_PUSH(vm_make_str_flag(s + b0, b1 - b0, 1));
            }
        } else {
            if (!strada_substr_span((int64_t)blen, start, len, &ss_off, &ss_cnt)) {
                SP_PUSH(VM_UNDEF_VAL);
            } else {
                SP_PUSH(vm_make_str_flag(s + ss_off, (size_t)ss_cnt, vm_is_utf8(str_v)));
            }
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
        if (__builtin_expect(VM_IS_INT(v), 1)) {
            int64_t code = VM_INT_VAL(v);
            if (__builtin_expect((uint64_t)code < 0x80ULL, 1)) {
                SP_PUSH(vm_get_ascii_str((unsigned char)code, 0));
                DISPATCH();
            }
        }
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
            if (s[len - 1] == '\n') {   /* never a lone "\r" (strada_chomp_sv) */
                len--;
                if (len > 0 && s[len - 1] == '\r') len--;
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
        uint16_t from_idx = di->a;
        uint16_t to_idx = di->b;
        uint16_t flags_idx = di->c;
        uint8_t store_kind = di->flags;
        uint16_t store_idx = (uint16_t)di->imm.u32;
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
        VMValue b = SP_POP(), a = SP_POP();
        VM_TRY_BIN_OV("x");
        VM_STR_OV_PAIR();
        VMValue str = a, count = b;
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
            uint32_t cid = vm_program_intern_class(vm->program, cname);
            uint32_t pid = vm->program->prog_id;
            switch (VM_PTR_TYPE(hash)) {
            case VM_OBJ_HASH: {
                VMHash *h = VM_AS_HASH(hash);
                free(h->class_name);
                h->class_name = strdup(cname);
                h->class_id = cid;
                h->class_prog_id = pid;
                break;
            }
            case VM_OBJ_ARRAY: {
                VMArray *a = VM_AS_ARRAY(hash);
                free(a->class_name);
                a->class_name = strdup(cname);
                a->class_id = cid;
                a->class_prog_id = pid;
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
        uint16_t slot = di->a;
        if (__builtin_expect(slot > (uint16_t)chunk->local_count, 0)) {
            fprintf(stderr, "VM error: local slot %u out of bounds (%d)\n", (unsigned)slot, chunk->local_count);
            vm->stack_top = sp; vm->exec_base = saved_exec_base; VM_EXEC_RETURN(VM_UNDEF_VAL);
        }
        VMValue cur = locals[slot];
        VMValue cell_val;
        if (VM_IS_PTR(cur) && VM_PTR_TYPE(cur) == VM_OBJ_CELL) {
            cell_val = cur;
        } else {
            VMCell *cell = vm_cell_alloc();
            cell->val = cur;
            cell_val = VM_MAKE_PTR(cell);
            locals[slot] = cell_val;
        }
        VMScalarRef *sr = vm_sref_alloc();
        sr->cell = cell_val;
        sr->global_name = NULL;
        SP_PUSH(VM_MAKE_PTR(sr));
        DISPATCH();
    }

    CASE(OP_MAKE_SREF_GLOBAL): {
        uint16_t name_idx = di->a;
        const char *gname = chunk->str_consts[name_idx];
        VMScalarRef *sr = vm_sref_alloc();
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
            VMCell *cell = vm_cell_alloc();
            cell->val = v;
            cell_val = VM_MAKE_PTR(cell);
        }
        VMScalarRef *sr = vm_sref_alloc();
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
        uint16_t pidx = di->a;
        SP_PUSH(VM_MAKE_INT((int)pidx < frame->passed_argc ? 1 : 0));
        DISPATCH();
    }

    CASE(OP_SQ_COERCE): {
        uint8_t side = di->flags;
        uint16_t idx = di->a;
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
        mcall_idx = di->a;
        mcall_argc = di->flags;
        mcall_ic_ip = di;
        mcall_method_cstr = chunk->str_consts[mcall_idx];
        mcall_is_dyn = 0;
        mcall_allow_field = 0;
        goto L_do_op_method_call;
    }

    CASE(OP_FIELD_ACCESS): {
        mcall_idx = di->a;
        mcall_argc = di->flags;
        mcall_ic_ip = di;
        mcall_method_cstr = chunk->str_consts[mcall_idx];
        mcall_is_dyn = 0;
        mcall_allow_field = 1;
        goto L_do_op_method_call;
    }

    CASE(OP_METHOD_CALL_SPREAD): {
        mcall_idx = di->a;
        mcall_ic_ip = di;
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

        VMHash *h = (VM_IS_PTR(self) && VM_PTR_TYPE(self) == VM_OBJ_HASH) ? VM_AS_HASH(self) : NULL;
        uint32_t cur_pid = vm->program->prog_id;
        uint32_t obj_cid = (h && h->class_id && h->class_prog_id == cur_pid) ? h->class_id : 0;
        const char *cls_name = NULL;
        int is_hash_blessed = 0;
        int fidx = -1;
        if (__builtin_expect(!mcall_is_dyn && obj_cid != 0, 1)) {
            uint32_t ic_slot = (uint32_t)(((uintptr_t)mcall_ic_ip >> 3) & 255);
            if (__builtin_expect(vm->method_ic[ic_slot][0].ip == mcall_ic_ip &&
                                 vm->method_ic[ic_slot][0].class_id == obj_cid &&
                                 vm->method_ic[ic_slot][0].prog_id == cur_pid, 1)) {
                fidx = vm->method_ic[ic_slot][0].func_idx;
                goto L_mcall_have_fidx;
            }
            if (__builtin_expect(vm->method_ic[ic_slot][1].ip == mcall_ic_ip &&
                                 vm->method_ic[ic_slot][1].class_id == obj_cid &&
                                 vm->method_ic[ic_slot][1].prog_id == cur_pid, 1)) {
                fidx = vm->method_ic[ic_slot][1].func_idx;
                goto L_mcall_have_fidx;
            }
        }

        /* Handle native OOP objects (VMNativeSV) — use strada_method_call for proper dispatch */
        if (__builtin_expect(!h && VM_IS_PTR(self) && VM_PTR_TYPE(self) == VM_OBJ_NATIVE_SV, 0)) {
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
        const char *obj_cls = h ? h->class_name : vm_obj_class_name(self);
        cls_name = obj_cls;
        is_hash_blessed = (obj_cls != NULL);
        if (!cls_name) {
            if (VM_IS_PTR(self) && VM_PTR_TYPE(self) == VM_OBJ_STR) {
                cls_name = VM_AS_STR(self)->data;
            } else if (VM_IS_PTR(self) && VM_PTR_TYPE(self) == VM_OBJ_STRBUF) {
                cls_name = VM_AS_STRBUF(self)->data;
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
                    if (h && !obj_cid) {
                        obj_cid = vm_program_intern_class(vm->program, cls_name);
                        if (!vm->is_worker_vm && !vm->workers_head) {
                            h->class_id = obj_cid;
                            h->class_prog_id = cur_pid;
                        }
                    } else if (!obj_cid) {
                        obj_cid = vm_program_intern_class(vm->program, cls_name);
                    }
                    if (obj_cid) {
                        uint32_t ic_slot = (uint32_t)(((uintptr_t)mcall_ic_ip >> 3) & 255);
                        if (!(vm->method_ic[ic_slot][0].ip == mcall_ic_ip &&
                              vm->method_ic[ic_slot][0].prog_id == cur_pid &&
                              vm->method_ic[ic_slot][0].class_id == obj_cid)) {
                            if (vm->method_ic[ic_slot][0].ip == mcall_ic_ip && vm->method_ic[ic_slot][0].prog_id == cur_pid) {
                                vm->method_ic[ic_slot][1] = vm->method_ic[ic_slot][0];
                            }
                            vm->method_ic[ic_slot][0].ip = mcall_ic_ip;
                            vm->method_ic[ic_slot][0].prog_id = cur_pid;
                            vm->method_ic[ic_slot][0].class_id = obj_cid;
                            vm->method_ic[ic_slot][0].func_idx = fidx;
                        }
                    }
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
                frame->ip = ip;
                if (__builtin_expect(!__atomic_load_n(&callee->dt_code, __ATOMIC_ACQUIRE) ||
                                     callee->dt_code_len_src != callee->code_len, 0)) {
                    vm_chunk_ensure_dt(vm->program, callee, dt_ptr);
                }
                if (__builtin_expect(vm->frame_count >= vm->frame_cap, 0)) {
                    vm->stack_top = sp;
                    vm_frames_grow(vm);
                }
                VMFrame *nf = &vm->frames[vm->frame_count++];
                nf->chunk = callee; nf->ip = callee->dt_code;
                size_t arg_base = sp - (size_t)argc - 1;
                nf->stack_base = arg_base;
                nf->closure = NULL;
                nf->global_save_depth = vm->global_save_count;
                nf->passed_argc = argc + 1;
                nf->call_context = mcall_ctx;
                int lcnt = callee->local_count + 1;
                if (__builtin_expect(!callee->has_variadic, 1)) {
                    int pmax = lcnt;
                    if (callee->fixed_param_count >= 0 && callee->fixed_param_count < pmax) {
                        pmax = callee->fixed_param_count;
                    }
                    int zf = (argc + 1 < pmax) ? (argc + 1) : pmax;
                    if (__builtin_expect(arg_base + (size_t)lcnt >= vm->stack_cap, 0)) {
                        vm->stack_top = sp;
                        vm_stack_grow(vm, arg_base + (size_t)lcnt);
                        stack = vm->stack;
                    }
                    if (zf < lcnt) {
                        int rem = lcnt - zf;
                        VMValue *zp = stack + arg_base + zf;
                        if (rem == 1) zp[0] = 0;
                        else if (rem == 2) { zp[0] = 0; zp[1] = 0; }
                        else memset(zp, 0, (size_t)rem * sizeof(VMValue));
                    }
                    nf->local_base = arg_base;
                    sp = arg_base + (size_t)lcnt;
                } else {
                    vm->stack_top = sp;
                    vm_mcall_variadic_setup(vm, nf, callee, argc, &sp);
                    stack = vm->stack;
                }
                frame = nf; chunk = callee; ip = callee->dt_code; locals = stack + arg_base;
                VM_MAYBE_ENTER_JIT_CALLEE();
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
                    if (__builtin_expect(!__atomic_load_n(&callee->dt_code, __ATOMIC_ACQUIRE) ||
                                         callee->dt_code_len_src != callee->code_len, 0)) {
                        vm_chunk_ensure_dt(vm->program, callee, dt_ptr);
                    }
                    if (vm->frame_count >= vm->frame_cap) {
                        vm_frames_grow(vm);
                    }
                    VMFrame *nf = &vm->frames[vm->frame_count++];
                    nf->chunk = callee; nf->ip = callee->dt_code;
                    nf->stack_base = sp - argc - 1;
                    nf->closure = NULL;
                    nf->global_save_depth = vm->global_save_count;
                    nf->passed_argc = argc + 2;
                    nf->call_context = mcall_ctx;
                    vm->stack_top = sp;
                    vm_mcall_autoload_setup(vm, nf, callee, argc, method, &sp);
                    stack = vm->stack;
                    frame = nf; chunk = callee; ip = callee->dt_code; locals = stack + nf->local_base;
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
        /* Perl's wording (and the compiled runtime's) */
        if (VM_IS_UNDEF(self))
            snprintf(errbuf, sizeof(errbuf), "Can't call method \"%s\" on an undefined value", method);
        else if (!VM_IS_PTR(self) || VM_PTR_TYPE(self) == VM_OBJ_STR || VM_PTR_TYPE(self) == VM_OBJ_STRBUF)
            snprintf(errbuf, sizeof(errbuf), "Can't call method \"%s\" without a package or object reference", method);
        else
            snprintf(errbuf, sizeof(errbuf), "Can't call method \"%s\" on unblessed reference", method);
        throw_exc = vm_str(errbuf);
        goto L_vm_throw_exc;
    }

    CASE(OP_SUPER_CALL): {
        uint16_t pkg_idx = di->a;
        uint16_t method_idx = di->b;
        uint8_t raw_argc = di->flags;
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
        mcall_argc = di->flags;
        mcall_ic_ip = di;
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
        uint16_t slot = di->a;
        VMValue rhs = SP_POP();
        /* $s .= $obj / $obj .= $s: an overloaded "." computes the new value
         * (compiled: strada_concat_inplace dispatches the overload) */
        if (__builtin_expect(vm->program->overload_count > 0, 0)) {
            VMValue *lp = &locals[slot];
            if (VM_IS_PTR(*lp) && VM_PTR_TYPE(*lp) == VM_OBJ_CELL) lp = &VM_AS_CELL(*lp)->val;
            if (!(VM_IS_PTR(*lp) && VM_PTR_TYPE(*lp) == VM_OBJ_TIED_SCALAR) &&
                (vm_obj_class_name(*lp) != NULL || vm_obj_class_name(rhs) != NULL)) {
                VMValue ovr = VM_UNDEF_VAL;
                frame->ip = ip; SP_PUSH(rhs); vm->stack_top = sp; vm->gc_pause++;
                int done = vm_try_bin_overload(vm, ".", *lp, rhs, &ovr);
                vm->gc_pause--;
                stack = vm->stack; RELOAD();
                VM_CHECK_EXC();
                rhs = SP_POP();
                if (done) {
                    lp = &locals[slot];
                    if (VM_IS_PTR(*lp) && VM_PTR_TYPE(*lp) == VM_OBJ_CELL) lp = &VM_AS_CELL(*lp)->val;
                    vm_val_free(lp); *lp = ovr;
                    vm_temp_free(&rhs);
                    DISPATCH();
                }
            }
        }
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
                VMValue rcopy = vm_make_str_flag(as, al, VM_IS_PTR(rhs) ? ((VMObjHeader*)VM_TO_PTR(rhs))->is_utf8 : 0);
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
        uint16_t str_idx = di->a;
        uint16_t slot = di->b;
        const char *as = chunk->str_consts[str_idx];
        size_t al = chunk->str_const_lens ? chunk->str_const_lens[str_idx] : strlen(as);
        int rhs_u8 = (chunk->str_const_vals && VM_IS_PTR(chunk->str_const_vals[str_idx]) &&
                      ((VMObjHeader*)VM_TO_PTR(chunk->str_const_vals[str_idx]))->is_utf8) ? 1 : 0;
        VMValue *loc = &locals[slot];
        if (VM_IS_PTR(*loc) && VM_PTR_TYPE(*loc) == VM_OBJ_CELL) loc = &VM_AS_CELL(*loc)->val;
        if (__builtin_expect(vm->program->overload_count > 0 && vm_obj_class_name(*loc) != NULL, 0)) {
            /* $obj .= "lit" with an overloaded "." */
            VMValue ovr = VM_UNDEF_VAL;
            VMValue rcv = (chunk->str_const_vals && VM_IS_PTR(chunk->str_const_vals[str_idx]))
                          ? chunk->str_const_vals[str_idx] : vm_make_str_flag(as, al, rhs_u8);
            frame->ip = ip; SP_PUSH(rcv); vm->stack_top = sp; vm->gc_pause++;
            int done = vm_try_bin_overload(vm, ".", *loc, rcv, &ovr);
            vm->gc_pause--;
            stack = vm->stack; RELOAD();
            VM_CHECK_EXC();
            (void)SP_POP();
            loc = &locals[slot];
            if (VM_IS_PTR(*loc) && VM_PTR_TYPE(*loc) == VM_OBJ_CELL) loc = &VM_AS_CELL(*loc)->val;
            if (done) { vm_val_free(loc); *loc = ovr; DISPATCH(); }
            as = chunk->str_consts[str_idx];
            al = chunk->str_const_lens ? chunk->str_const_lens[str_idx] : strlen(as);
        }
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
                if (rhs_u8 || vm_is_utf8(lhs)) vs->hdr.is_utf8 = 1;
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
            if (rhs_u8) sb->hdr.is_utf8 = 1;
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
            if (rhs_u8 || (VM_IS_PTR(lhs) && ((VMObjHeader*)VM_TO_PTR(lhs))->is_utf8)) {
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
        uint16_t pfx_idx = di->a;
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
        uint16_t pfx_idx = di->a;
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
        uint16_t pfx_idx = di->a;
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
        uint16_t func_idx = di->a;
        uint8_t cap_count = di->flags;
        const uint16_t *outer_slots = chunk->dt_aux ? (chunk->dt_aux + di->imm.u32) : NULL;
        VMClosure *cl = vm_closure_alloc();
        cl->func_idx = func_idx;
        cl->capture_count = cap_count;
        if (cap_count > 0) {
            cl->captures = calloc(cap_count, sizeof(VMValue));
            for (int i = cap_count - 1; i >= 0; i--) {
                VMValue raw = SP_POP();
                /* Check if already a cell (e.g., capturing from parent closure) */
                if (VM_IS_PTR(raw) && VM_PTR_TYPE(raw) == VM_OBJ_CELL) {
                    cl->captures[i] = raw;
                } else if (outer_slots && outer_slots[i] != 0xFFFF) {
                    uint16_t os = outer_slots[i];
                    vm_temp_free(&raw);
                    if (os < (uint16_t)chunk->local_count &&
                        VM_IS_PTR(locals[os]) && VM_PTR_TYPE(locals[os]) == VM_OBJ_CELL) {
                        cl->captures[i] = locals[os];
                    } else {
                        /* Box locals[os] directly to preserve VM_OBJ_WEAKREF / VM_OBJ_TIED_SCALAR */
                        VMCell *cell = vm_cell_alloc();
                        cell->val = (os < (uint16_t)chunk->local_count) ? locals[os] : VM_UNDEF_VAL;
                        VMValue cell_val = VM_MAKE_PTR(cell);
                        cl->captures[i] = cell_val;
                        if (os < (uint16_t)chunk->local_count) {
                            locals[os] = cell_val;
                        }
                    }
                } else {
                    /* Create a new cell for capture-by-reference */
                    VMCell *cell = vm_cell_alloc();
                    cell->val = raw;
                    cl->captures[i] = VM_MAKE_PTR(cell);
                }
            }
        }
        SP_PUSH(VM_MAKE_PTR(cl));
        DISPATCH();
    }

    CASE(OP_LOAD_CAPTURE_CELL): {
        uint16_t idx = di->a;
        if (frame->closure && idx < (uint16_t)frame->closure->capture_count) {
            SP_PUSH(frame->closure->captures[idx]);
        } else {
            SP_PUSH(VM_UNDEF_VAL);
        }
        DISPATCH();
    }

    CASE(OP_LOAD_CAPTURE): {
        uint16_t idx = di->a;
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
        uint16_t idx = di->a;
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
        clcall_argc = di->flags;
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

            if (__builtin_expect(!__atomic_load_n(&callee->dt_code, __ATOMIC_ACQUIRE) ||
                                 callee->dt_code_len_src != callee->code_len, 0)) {
                vm_chunk_ensure_dt(vm->program, callee, dt_ptr);
            }
            if (vm->frame_count >= vm->frame_cap) {
                vm_frames_grow(vm);
            }
            VMFrame *nf = &vm->frames[vm->frame_count++];
            nf->chunk = callee; nf->ip = callee->dt_code;
            size_t arg_base = sp - (size_t)argc;
            nf->stack_base = arg_base;
            nf->closure = cl;
            nf->global_save_depth = vm->global_save_count;

            int eff_argc = cl->has_bound_arg ? (argc + 1) : argc;
            nf->passed_argc = eff_argc;
            nf->call_context = clcall_ctx;

            int lcnt = callee->local_count + 1;
            if (__builtin_expect(!callee->has_variadic && !cl->has_bound_arg, 1)) {
                int pmax = lcnt;
                if (callee->fixed_param_count >= 0 && callee->fixed_param_count < pmax) {
                    pmax = callee->fixed_param_count;
                }
                int zf = argc < pmax ? argc : pmax;
                if (__builtin_expect(arg_base + (size_t)lcnt >= vm->stack_cap, 0)) {
                    vm->stack_top = sp;
                    vm_stack_grow(vm, arg_base + (size_t)lcnt);
                    stack = vm->stack;
                }
                if (zf < lcnt) {
                    int rem = lcnt - zf;
                    VMValue *zp = stack + arg_base + zf;
                    if (rem == 1) zp[0] = 0;
                    else if (rem == 2) { zp[0] = 0; zp[1] = 0; }
                    else memset(zp, 0, (size_t)rem * sizeof(VMValue));
                }
                nf->local_base = arg_base;
                sp = arg_base + (size_t)lcnt;
            } else {
                vm->stack_top = sp;
                vm_closure_variadic_setup(vm, nf, callee, cl, argc, &sp);
                stack = vm->stack;
            }
            frame = nf; chunk = callee; ip = callee->dt_code; locals = stack + arg_base;
            VM_MAYBE_ENTER_JIT_CALLEE();
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
        } else {
            /* Calling something that is not code dies (Perl), catchably */
            for (int i = 0; i < argc; i++) { VMValue dv = SP_POP(); vm_temp_free(&dv); }
            vm->stack_top = sp;
            throw_exc = vm_str(VM_IS_UNDEF(closure_val)
                ? "Can't use an undefined value as a subroutine reference"
                : "Not a CODE reference");
            goto L_vm_throw_exc;
        }
        DISPATCH();
    }

    /* ===== Regex ===== */
    CASE(OP_REGEX_MATCH): {
        uint16_t pat_idx = di->a;
        uint8_t flags = di->flags;
        (void)flags;
        VMValue pat_val = VM_UNDEF_VAL;
        char pbuf[256];
        const char *pattern;
        size_t plen = 0;
        if (pat_idx == 0xFFFF) {
            /* interpolated pattern built at run time (on top of the subject) */
            pat_val = SP_POP();
            if (__builtin_expect(vm_has_stringify(vm->program, pat_val), 0)) {
                frame->ip = ip; vm->stack_top = sp;
                pat_val = vm_stringify_ov(vm, pat_val);
                stack = vm->stack; RELOAD();
            }
            pattern = vm_bytes_view(pat_val, pbuf, sizeof(pbuf), &plen);
        } else {
            pattern = chunk->str_consts[pat_idx];
            plen = chunk->str_const_lens ? chunk->str_const_lens[pat_idx] : strlen(pattern);
        }
        VMValue str_val = SP_POP();
        char buf[256];
        size_t slen = 0;
        const char *s = vm_bytes_view(str_val, buf, sizeof(buf), &slen);
        int result = vm_regex_match_len_u8(vm, s, slen, vm_is_utf8(str_val), pattern, plen);
        SP_PUSH(VM_MAKE_INT(result));
        vm_val_free(&str_val);
        vm_temp_free(&pat_val);
        DISPATCH();
    }

    CASE(OP_REGEX_FIND): {
        uint16_t pat_idx = di->a;
        VMValue fpat_val = VM_UNDEF_VAL;
        if (pat_idx == 0xFFFF) fpat_val = SP_POP();
        VMValue pos_val = SP_POP(), str_val = SP_POP();
        char fbuf[4096];
        size_t slen = 0;
        const char *s = vm_bytes_view(str_val, fbuf, sizeof(fbuf), &slen);
        char fpbuf[256];
        const char *pattern;
        size_t plen = 0;
        if (pat_idx == 0xFFFF) {
            pattern = vm_bytes_view(fpat_val, fpbuf, sizeof(fpbuf), &plen);
        } else {
            pattern = chunk->str_consts[pat_idx];
            plen = chunk->str_const_lens ? chunk->str_const_lens[pat_idx] : strlen(pattern);
        }
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
        vm_temp_free(&fpat_val);
        vm_temp_free(&str_val);
        DISPATCH();
    }

    CASE(OP_REGEX_NOT_MATCH): {
        uint16_t pat_idx = di->a;
        uint8_t flags = di->flags;
        (void)flags;
        VMValue pat_val = VM_UNDEF_VAL;
        char pbuf[256];
        const char *pattern;
        size_t plen = 0;
        if (pat_idx == 0xFFFF) {
            /* interpolated pattern built at run time (on top of the subject) */
            pat_val = SP_POP();
            if (__builtin_expect(vm_has_stringify(vm->program, pat_val), 0)) {
                frame->ip = ip; vm->stack_top = sp;
                pat_val = vm_stringify_ov(vm, pat_val);
                stack = vm->stack; RELOAD();
            }
            pattern = vm_bytes_view(pat_val, pbuf, sizeof(pbuf), &plen);
        } else {
            pattern = chunk->str_consts[pat_idx];
            plen = chunk->str_const_lens ? chunk->str_const_lens[pat_idx] : strlen(pattern);
        }
        VMValue str_val = SP_POP();
        char buf[256];
        size_t slen = 0;
        const char *s = vm_bytes_view(str_val, buf, sizeof(buf), &slen);
        int result = vm_regex_match_len_u8(vm, s, slen, vm_is_utf8(str_val), pattern, plen);
        SP_PUSH(VM_MAKE_INT(!result));
        vm_val_free(&str_val);
        vm_temp_free(&pat_val);
        DISPATCH();
    }

    CASE(OP_LOAD_CAPTURE_VAR): {
        uint8_t num = di->flags;
        if (num < VM_RX_MAX && vm->regex_captures[num]) {
            SP_PUSH(vm_make_str_flag(vm->regex_captures[num], vm->regex_capture_len[num], vm->regex_capture_utf8[num]));
        } else {
            SP_PUSH(VM_UNDEF_VAL);   /* unset group: undef (Perl, compiled) */
        }
        DISPATCH();
    }

    /* ===== Try/Catch/Throw ===== */
    CASE(OP_TRY_BEGIN): {
        uint16_t catch_off = di->a;
        if (vm->exc_top >= vm->exc_cap) {
            vm->exc_cap *= 2;
            vm->exc_stack = realloc(vm->exc_stack, vm->exc_cap * sizeof(VMExcHandler));
        }
        VMExcHandler *eh = &vm->exc_stack[vm->exc_top++];
        eh->catch_ip = (catch_off < chunk->dt_len) ? (chunk->dt_code + catch_off) : (chunk->dt_code + chunk->dt_len);
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
        vm->exc_from_die = 0;
        goto L_vm_throw_exc;
    }

    L_vm_throw_exc: {
        vm->pending_call_context = 0;
        /* core::exception_trace() / the uncaught trace: the frames active at
         * this throw (every throw -- throw, die, runtime errors -- comes here
         * before any frame is unwound) */
        frame->ip = ip;
        free(vm->exc_trace);
        vm->exc_trace = vm_build_trace(vm, (int)vm->frame_count - 1, 0);
        vm->exc_from_die = (vm->exc_from_die == 2) ? 1 : 0;
        if (vm->exc_top > 0) {
            VMExcHandler eh_copy = vm->exc_stack[vm->exc_top - 1];
            if (vm->exec_base > 0 && eh_copy.frame_count <= vm->exec_base) {
                size_t base_sp = vm->frames[vm->exec_base].stack_base;
                vm->gc_pause++;
                while (vm->frame_count > vm->exec_base) {
                    VMFrame *f = &vm->frames[vm->frame_count - 1];
                    int gsd = f->global_save_depth;
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
            if (vm->frame_count > eh_copy.frame_count) {
                vm->frame_count = eh_copy.frame_count;
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
                /* as the compiled backend: die() prints its message; a throw
                 * prints "Uncaught exception: MSG" and the stack trace */
                StradaValue *usv = vm_to_sv(throw_exc);
                char *umsg = strada_to_str(usv);
                if (vm->exc_from_die == 1) {
                    fprintf(stderr, "%s\n", umsg ? umsg : "");
                } else {
                    fprintf(stderr, "Uncaught exception: %s\n", umsg ? umsg : "");
                    if (vm->exc_trace && vm->exc_trace[0]) fprintf(stderr, "Stack trace:\n%s", vm->exc_trace);
                }
                free(umsg);
                strada_decref(usv);
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
        const VMInsn *ic_ip = di;
        uint16_t name_idx = di->a;
        VMHash *gh = vm_gh_for(vm, chunk->str_consts[name_idx]);
        VMValue v;
        if (__builtin_expect(!vm->is_worker_vm && !vm->workers_head && gh != NULL && chunk->str_const_vals != NULL, 1)) {
            uint32_t ic_slot = (uint32_t)(((uintptr_t)ic_ip >> 3) & 63);
            VMString *nstr = VM_AS_STR(chunk->str_const_vals[name_idx]);
            const VMConstString *ncs = (const VMConstString*)((const char*)nstr - offsetof(VMConstString, str));
            if (__builtin_expect(vm->global_ic[ic_slot].ip == ic_ip &&
                                 vm->global_ic[ic_slot].globals == gh &&
                                 vm->global_ic[ic_slot].gen == gh->grow_gen, 1)) {
                uint32_t es = vm->global_ic[ic_slot].slot;
                VMHashEntry *e = &gh->entries[es];
                if (__builtin_expect(es < (uint32_t)gh->capacity && e->occupied == 1 &&
                                     e->hash == ncs->hash && e->keylen == nstr->len &&
                                     memcmp(e->key, nstr->data, nstr->len) == 0, 1)) {
                    v = e->value;
                    goto L_load_global_have_v;
                }
            }
            int32_t idx = vm_hash_slot_idx_nh(gh, nstr->data, nstr->len, ncs->hash);
            if (idx >= 0) {
                vm->global_ic[ic_slot].ip = ic_ip;
                vm->global_ic[ic_slot].globals = gh;
                vm->global_ic[ic_slot].gen = gh->grow_gen;
                vm->global_ic[ic_slot].slot = (uint32_t)idx;
                v = gh->entries[idx].value;
            } else {
                v = VM_UNDEF_VAL;
            }
        } else {
            v = vm_globals_get_raw(vm, chunk->str_consts[name_idx]);
        }
    L_load_global_have_v:
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
        const VMInsn *ic_ip = di;
        uint16_t name_idx = di->a;
        VMValue v = SP_POP();
        VMHash *gh = vm_gh_for(vm, chunk->str_consts[name_idx]);
        if (__builtin_expect(!vm->is_worker_vm && !vm->workers_head && gh != NULL && chunk->str_const_vals != NULL, 1)) {
            uint32_t ic_slot = (uint32_t)(((uintptr_t)ic_ip >> 3) & 63);
            VMString *nstr = VM_AS_STR(chunk->str_const_vals[name_idx]);
            const VMConstString *ncs = (const VMConstString*)((const char*)nstr - offsetof(VMConstString, str));
            if (__builtin_expect(vm->global_ic[ic_slot].ip == ic_ip &&
                                 vm->global_ic[ic_slot].globals == gh &&
                                 vm->global_ic[ic_slot].gen == gh->grow_gen, 1)) {
                uint32_t es = vm->global_ic[ic_slot].slot;
                VMHashEntry *e = &gh->entries[es];
                if (__builtin_expect(es < (uint32_t)gh->capacity && e->occupied == 1 &&
                                     e->hash == ncs->hash && e->keylen == nstr->len &&
                                     memcmp(e->key, nstr->data, nstr->len) == 0, 1)) {
                    VMValue cur = e->value;
                    if (__builtin_expect(VM_IS_PTR(cur) && VM_PTR_TYPE(cur) == VM_OBJ_TIED_SCALAR, 0)) {
                        frame->ip = ip; vm->stack_top = sp;
                        vm_tied_scalar_store(vm, cur, v);
                        stack = vm->stack; RELOAD();
                        VM_CHECK_EXC();
                        DISPATCH();
                    }
                    e->value = v;
                    DISPATCH();
                }
            }
            int32_t idx = vm_hash_slot_idx_nh(gh, nstr->data, nstr->len, ncs->hash);
            if (idx >= 0) {
                VMValue cur = gh->entries[idx].value;
                if (__builtin_expect(VM_IS_PTR(cur) && VM_PTR_TYPE(cur) == VM_OBJ_TIED_SCALAR, 0)) {
                    frame->ip = ip; vm->stack_top = sp;
                    vm_tied_scalar_store(vm, cur, v);
                    stack = vm->stack; RELOAD();
                    VM_CHECK_EXC();
                    DISPATCH();
                }
                vm->global_ic[ic_slot].ip = ic_ip;
                vm->global_ic[ic_slot].globals = gh;
                vm->global_ic[ic_slot].gen = gh->grow_gen;
                vm->global_ic[ic_slot].slot = (uint32_t)idx;
                gh->entries[idx].value = v;
            } else {
                uint32_t ins = vm_hash_set_nh_slot(gh, nstr->data, nstr->len, ncs->hash, v);
                if (ins != UINT32_MAX) {
                    vm->global_ic[ic_slot].ip = ic_ip;
                    vm->global_ic[ic_slot].globals = gh;
                    vm->global_ic[ic_slot].gen = gh->grow_gen;
                    vm->global_ic[ic_slot].slot = ins;
                }
            }
            DISPATCH();
        }
        const char *name = chunk->str_consts[name_idx];
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
        uint16_t name_idx = di->a;
        const char *name = chunk->str_consts[name_idx];
        VMValue v = SP_POP();
        pthread_mutex_t *glk = vm_globals_lock_ptr(vm);
        if (glk) pthread_mutex_lock(glk);
        VMHash *gdh = vm_gh_for(vm, name);
        int gex = gdh ? vm_hash_exists(gdh, name) : 0;
        if (!gex && gdh) vm_hash_set(gdh, name, v);
        if (glk) pthread_mutex_unlock(glk);
        if (gex) {
            vm_val_free(&v);
        }
        DISPATCH();
    }

    CASE(OP_SAVE_GLOBAL): {
        uint16_t name_idx = di->a;
        const char *name = chunk->str_consts[name_idx];
        if (vm->global_save_count >= vm->global_save_cap) {
            vm_global_saves_grow(vm);
        }
        VMGlobalSave *gs = &vm->global_saves[vm->global_save_count++];
        memset(gs, 0, sizeof(*gs));
        gs->name = strdup(name);
        pthread_mutex_t *glk = vm_globals_lock_ptr(vm);
        if (glk) pthread_mutex_lock(glk);
        VMHash *sgh = vm_gh_for(vm, name);
        gs->saved_value = vm_val_copy(sgh ? vm_hash_get(sgh, name) : VM_UNDEF_VAL);
        /* Shadow with undef so localizing a tied our-var does not trigger STORE */
        if (sgh) vm_hash_set(sgh, name, VM_UNDEF_VAL);
        if (glk) pthread_mutex_unlock(glk);
        DISPATCH();
    }

    CASE(OP_RESTORE_GLOBAL): {
        uint16_t name_idx = di->a;
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
        size_t hkeylen = 0;
        int existed = 0;
        VMValue saved_val = VM_UNDEF_VAL;
        if (VM_IS_PTR(hval) && VM_PTR_TYPE(hval) == VM_OBJ_HASH) {
            h = VM_AS_HASH(hval);
            char kbuf[256];
            const char *ks = vm_bytes_view(key, kbuf, sizeof(kbuf), &hkeylen);
            hkey = malloc(hkeylen + 1);
            if (hkeylen > 0) memcpy(hkey, ks, hkeylen);
            hkey[hkeylen] = '\0';
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
                existed = vm_hash_exists_n(h, hkey, hkeylen);
                saved_val = existed ? vm_val_copy(vm_hash_get_n(h, hkey, hkeylen)) : VM_UNDEF_VAL;
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
        gs->hash_keylen = hkeylen;
        gs->existed = existed;
        gs->saved_value = saved_val;
        vm_temp_free(&key);
        DISPATCH();
    }

    CASE(OP_RESTORE_HASH_ELEM): {
        VMHash *rh = NULL;
        char *rhkey = NULL;
        size_t rhkeylen = 0;
        int rexisted = 0;
        VMValue rsaved = VM_UNDEF_VAL;
        int found = 0;
        for (int i = vm->global_save_count - 1; i >= 0; i--) {
            if (vm->global_saves[i].is_hash_elem) {
                rh = vm->global_saves[i].hash;
                rhkey = vm->global_saves[i].hash_key;
                rhkeylen = vm->global_saves[i].hash_keylen;
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
            vm_restore_one_hash_elem(vm, rh, rhkey, rhkeylen, rexisted, rsaved, 1);
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
        if (__builtin_expect(vm_obj_class_name(v) != NULL, 0)) {
            /* no "neg": -$obj is 0 - $obj through an overloaded "-" */
            VMValue _sub_res = VM_UNDEF_VAL;
            frame->ip = ip; vm->stack_top = sp;
            if (vm_try_bin_overload(vm, "-", VM_MAKE_INT(0), v, &_sub_res)) {
                stack = vm->stack; RELOAD();
                if (__builtin_expect(vm->has_pending_exc, 0)) {
                    throw_exc = vm->pending_exc; vm->pending_exc = VM_UNDEF_VAL; vm->has_pending_exc = 0;
                    goto L_vm_throw_exc;
                }
                vm_temp_free(&v);
                SP_PUSH(_sub_res);
                DISPATCH();
            }
            stack = vm->stack; RELOAD();
        }
        if (__builtin_expect(VM_IS_INT(v), 1)) { SP_PUSH(VM_MAKE_INT(-VM_INT_VAL(v))); DISPATCH(); }
        if (__builtin_expect(VM_IS_FLOAT(v), 1)) { SP_PUSH(VM_MAKE_FLOAT(-VM_FLOAT_VAL(v))); DISPATCH(); }
        if (VM_IS_ANY_INT(v)) {
            SP_PUSH(VM_MAKE_INT((int64_t)(0ULL - (uint64_t)VM_ANY_INT_VAL(v))));
        } else if (VM_IS_PTR(v) && (VM_PTR_TYPE(v) == VM_OBJ_STR || VM_PTR_TYPE(v) == VM_OBJ_STRBUF)) {
            /* Perl string negation: -"foo" is "-foo", -"-foo" is "+foo",
             * numeric strings negate numerically (strada_negate) */
            StradaValue *nsv = vm_to_sv(v);
            StradaValue *nres = strada_negate(nsv);
            strada_decref(nsv);
            VMValue nvv = nres ? sv_to_vm(nres) : VM_UNDEF_VAL;
            if (nres) strada_decref(nres);
            vm_temp_free(&v);
            SP_PUSH(nvv);
        } else if (VM_IS_PTR(v)) {
            frame->ip = ip; vm->stack_top = sp;
            vm->gc_pause++;
            double nv = vm_to_num_vm(vm, v);
            vm->gc_pause--;
            stack = vm->stack; RELOAD();
            if (__builtin_expect(vm->has_pending_exc, 0)) { vm_temp_free(&v); VM_CHECK_EXC(); }
            SP_PUSH(VM_MAKE_FLOAT(-nv));
        } else {
            SP_PUSH(VM_MAKE_FLOAT(-vm_to_num(v)));
        }
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
        double na = 0.0, nb = 0.0;
        if (VM_IS_PTR(a) || VM_IS_PTR(b)) {
            frame->ip = ip; vm->stack_top = sp;
            int pexc = vm_pair_to_num(vm, a, b, &na, &nb);
            stack = vm->stack; RELOAD();
            if (__builtin_expect(pexc, 0)) { vm_temp_free(&a); vm_temp_free(&b); VM_CHECK_EXC(); }
        } else {
            na = vm_to_num(a); nb = vm_to_num(b);
        }
        double r = pow(na, nb);
        SP_PUSH(VM_MAKE_FLOAT(r));
        vm_temp_free(&a); vm_temp_free(&b);
        DISPATCH();
    }

    CASE(OP_SPRINTF): {
        uint8_t argc = di->flags;
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
                if (sr->class_name) { SP_PUSH(vm_str(sr->class_name)); break; }
                /* a reference to a variable holding a reference is "REF" (Perl) */
                VMValue stv = VM_UNDEF_VAL;
                if (sr->global_name) stv = vm_globals_get(vm, sr->global_name);
                else if (VM_IS_PTR(sr->cell) && VM_PTR_TYPE(sr->cell) == VM_OBJ_CELL) stv = VM_AS_CELL(sr->cell)->val;
                int is_refval = 0;
                if (VM_IS_PTR(stv)) {
                    enum VMObjType stt = VM_PTR_TYPE(stv);
                    is_refval = (stt == VM_OBJ_ARRAY || stt == VM_OBJ_HASH || stt == VM_OBJ_CLOSURE ||
                                 stt == VM_OBJ_SREF || stt == VM_OBJ_WEAKREF);
                }
                SP_PUSH(vm_str(is_refval ? "REF" : "SCALAR"));
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
        uint8_t kind = di->flags;
        uint16_t slot = di->a;
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
            { VMHash *sph = vm_gh_for(vm, gname); sp_slot = sph ? vm_hash_slot_ptr(sph, gname) : NULL; }
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
                VMWeakRef *wr = vm_weakref_alloc();
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
        uint8_t kind = di->flags;
        uint16_t slot = di->a;
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

    CASE(OP_STR_OV): {
        VMValue v = stack[sp - 1];
        if (vm_obj_class_name(v) != NULL && vm_has_stringify(vm->program, v)) {
            frame->ip = ip; vm->stack_top = sp;
            VMValue r = vm_stringify_ov(vm, v);
            stack = vm->stack; RELOAD();
            VM_CHECK_EXC();
            stack[sp - 1] = r;
        }
        DISPATCH();
    }

    CASE(OP_CLEAR_LOCALS): {
        uint16_t from = di->a;
        uint16_t to = di->b;
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
        int64_t ia = 0, ib = 0;
        if (VM_IS_PTR(a) || VM_IS_PTR(b)) {
            frame->ip = ip; vm->stack_top = sp;
            int pexc = vm_pair_to_int(vm, a, b, &ia, &ib);
            stack = vm->stack; RELOAD();
            if (__builtin_expect(pexc, 0)) { vm_temp_free(&a); vm_temp_free(&b); VM_CHECK_EXC(); }
        } else {
            ia = vm_to_int(a); ib = vm_to_int(b);
        }
        SP_PUSH(VM_MAKE_INT((int64_t)((uint64_t)ia + (uint64_t)ib)));
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
        uint16_t target = di->a;
        if (__builtin_expect(target < chunk->dt_len, 1)) ip = chunk->dt_code + target;
        DISPATCH();
    }

    CASE(OP_LOOP_NEXT): {
        uint16_t target = di->a;
        if (__builtin_expect(target < chunk->dt_len, 1)) {
            VMInsn *tgt = chunk->dt_code + target;
            if (tgt <= ip) VM_WORKER_SAFEPOINT(tgt);
            ip = tgt;
        }
        DISPATCH();
    }

    CASE(OP_LOOP_REDO): {
        uint16_t target = di->a;
        if (__builtin_expect(target < chunk->dt_len, 1)) {
            VMInsn *tgt = chunk->dt_code + target;
            if (tgt <= ip) VM_WORKER_SAFEPOINT(tgt);
            ip = tgt;
        }
        DISPATCH();
    }

    /* ===== Builtins ===== */
    CASE(OP_BUILTIN): {
        uint16_t bid = di->a;
        uint8_t argc = di->flags;
        frame->ip = ip; vm->stack_top = sp;
        if (__builtin_expect(vm_exec_builtin(vm, bid, argc, &throw_exc), 0)) {
            stack = vm->stack; RELOAD();
            goto L_vm_throw_exc;
        }
        stack = vm->stack; RELOAD();
        DISPATCH();
    }

    CASE(OP_CALL_NATIVE): {
        uint16_t native_idx = di->a;
        uint8_t argc = di->flags;
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
        uint16_t idx = di->a;
        if (__builtin_expect(idx >= vm->program->cblock_count, 0)) {
            fprintf(stderr, "VM error: cblock index %u out of bounds (%d)\n",
                    (unsigned)idx, vm->program->cblock_count);
            vm->stack_top = sp; vm->exec_base = saved_exec_base; VM_EXEC_RETURN(VM_UNDEF_VAL);
        }
        VMCBlock *cb = &vm->program->cblocks[idx];

        /* JIT-compile on first execution */
        if (!__atomic_load_n(&cb->fn, __ATOMIC_ACQUIRE)) {
            int _prev_safe = vm_worker_enter_safe(vm);
            pthread_mutex_lock(&g_cblock_jit_mutex);
            int jit_fail = (!cb->fn && cblock_jit_compile(vm->program, idx) != 0);
            pthread_mutex_unlock(&g_cblock_jit_mutex);
            vm_worker_leave_safe(vm, _prev_safe);
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
        StradaValue *cb_ret = NULL;
        if (vm_c_call_cblock(vm, (StradaValue *(*)(StradaValue**))cb->fn, sv_vars, &throw_exc, &cb_ret)) {
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
                    vm_array_ensure_cap(va, ncap);
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
                        key_free_entry(&vh->entries[k]);
                        vm_val_free(&vh->entries[k].value);
                    }
                }
                vm_hash_reset_storage(vh);
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

        if (cb_ret) {
            /* `return EXPR;` inside the block returns from the Strada
             * function (as in the compiled backend); the value is owned */
            VMValue crv = sv_to_vm(cb_ret);
            strada_decref(cb_ret);
            SP_PUSH(crv);
            goto L_OP_RETURN;
        }
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
    #undef VM_WORKER_SAFEPOINT
    #undef VM_VALUE_IS_TRUTHY
    #undef DISPATCH
    #undef CASE
    #undef VM_EXEC_RETURN
    #undef VM_TRY_BIN_OV
#ifdef VM_JIT_X64
    #undef VM_JIT_HANDLE_RC
#endif
    #undef VM_TRY_JIT_OSR
    #undef VM_MAYBE_ENTER_JIT_CALLEE
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
    vm_conv_begin();   /* one memo: globals sharing a container still do */
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
    vm_conv_end();
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
    vm_conv_begin();   /* one memo: globals sharing a container still do */
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
    vm_conv_end();
    free(snap_k);
    free(snap_kl);
    free(snap_v);
    g_active_vm = prev_vm;
}

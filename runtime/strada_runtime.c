/*
 This file is part of the Strada Language (https://github.com/mjflick/strada-lang).
 Copyright (c) 2026 Michael J. Flickinger
 
 This program is free software: you can redistribute it and/or modify  
 it under the terms of the GNU General Public License as published by  
 the Free Software Foundation, version 2.

 This program is distributed in the hope that it will be useful, but 
 WITHOUT ANY WARRANTY; without even the implied warranty of 
 MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the GNU 
 General Public License for more details.

 You should have received a copy of the GNU General Public License 
 along with this program. If not, see <http://www.gnu.org/licenses/>.
*/

/* strada_runtime.c - Strada Runtime Implementation */
#include "strada_runtime.h"

/* Compatibility: O_NDELAY is O_NONBLOCK on most systems */
#ifndef O_NDELAY
#define O_NDELAY O_NONBLOCK
#endif
#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include <math.h>
#include <unistd.h>
#include <errno.h>
#include <limits.h>
#include <sys/wait.h>
#include <sys/stat.h>
#include <sys/time.h>
#ifdef __linux__
#include <sys/prctl.h>
#endif
#include <signal.h>
#include <dirent.h>
#include <libgen.h>
#include <glob.h>
#include <fnmatch.h>
#include <pwd.h>
#include <grp.h>
#include <sys/resource.h>
#include <sys/times.h>
#include <sys/mman.h>
#include <sys/file.h>
#include <sys/statvfs.h>
#include <termios.h>
#include <poll.h>
#include <utime.h>
#include <sys/utsname.h>
#include <sys/ioctl.h>
#include <locale.h>
#ifdef __linux__
#include <malloc.h>  /* malloc_usable_size() for string buffer growth optimization */
#endif

/* ===== MEMORY CONFIGURATION ===== */

/* Default initial capacity for new arrays (can be changed at runtime) */
static size_t strada_default_array_capacity = 16;

/* Default initial bucket count for new hashes (can be changed at runtime) */
static size_t strada_default_hash_capacity = 32;

/* ===== HASH TABLE OPTIMIZATIONS ===== */

/* Round up to next power of 2 (assumes n > 0) */
static inline size_t strada_next_pow2(size_t n) {
    n--;
    n |= n >> 1;
    n |= n >> 2;
    n |= n >> 4;
    n |= n >> 8;
    n |= n >> 16;
    n |= n >> 32;
    return n + 1;
}

/* Entry free-list to avoid malloc/free per hash entry */
#define STRADA_HASH_ENTRY_FREELIST_MAX 16384
static StradaHashEntry *strada_hash_entry_freelist = NULL;
static size_t strada_hash_entry_freelist_size = 0;

static inline StradaHashEntry *strada_hash_entry_alloc(void) {
    if (strada_hash_entry_freelist) {
        StradaHashEntry *entry = strada_hash_entry_freelist;
        strada_hash_entry_freelist = entry->next;
        strada_hash_entry_freelist_size--;
        return entry;
    }
    return malloc(sizeof(StradaHashEntry));
}

static inline void strada_hash_entry_free(StradaHashEntry *entry) {
    if (strada_hash_entry_freelist_size < STRADA_HASH_ENTRY_FREELIST_MAX) {
        entry->next = strada_hash_entry_freelist;
        strada_hash_entry_freelist = entry;
        strada_hash_entry_freelist_size++;
    } else {
        free(entry);
    }
}

static void strada_hash_entry_freelist_destroy(void) {
    StradaHashEntry *entry = strada_hash_entry_freelist;
    while (entry) {
        StradaHashEntry *next = entry->next;
        free(entry);
        entry = next;
    }
    strada_hash_entry_freelist = NULL;
    strada_hash_entry_freelist_size = 0;
}

/* ===== VALUE CREATION ===== */

/* Metadata helpers — cold fields live behind a pointer, NULL for most values */
#define META_POOL_MAX 4096
static StradaMetadata *meta_pool[META_POOL_MAX];
static int meta_pool_count = 0;

static inline StradaMetadata *strada_ensure_meta(StradaValue *sv) {
    if (!sv->meta) {
        if (meta_pool_count > 0) {
            sv->meta = meta_pool[--meta_pool_count];
            memset(sv->meta, 0, sizeof(StradaMetadata));
        } else {
            sv->meta = calloc(1, sizeof(StradaMetadata));
        }
    }
    return sv->meta;
}

static void strada_meta_pool_cleanup(void) {
    for (int i = 0; i < meta_pool_count; i++)
        free(meta_pool[i]);
    meta_pool_count = 0;
}
#define SV_IS_TIED(sv) ((sv)->meta && (sv)->meta->is_tied)
#define SV_IS_WEAK(sv) ((sv)->meta && (sv)->meta->is_weak)
#define SV_BLESSED(sv) ((sv)->meta ? (sv)->meta->blessed_package : NULL)
#define SV_STRUCT_NAME(sv) ((sv)->meta ? (sv)->meta->struct_name : NULL)
#define SV_TIED_OBJ(sv) ((sv)->meta ? (sv)->meta->tied_obj : NULL)

/* Forward declaration — defined in async section below */
static int strada_threading_active;

/* ===== STRADAVALUE FREE-LIST POOL ===== */
/* Recycles freed StradaValue structs to avoid malloc/free overhead */
#define SV_POOL_MAX 16384
static StradaValue *sv_pool_stack[SV_POOL_MAX];
static int sv_pool_count = 0;

static void strada_sv_pool_cleanup(void) {
    for (int i = 0; i < sv_pool_count; i++)
        free(sv_pool_stack[i]);
    sv_pool_count = 0;
}

/* Fast allocator for StradaValue — pops from free-list or mallocs.
 * Cold fields (struct_name, blessed_package, is_tied, tied_obj, is_weak) are
 * stored behind sv->meta, which is NULL for most values. */
static inline StradaValue *strada_value_alloc(void) {
    StradaValue *sv;
    if (!strada_threading_active && sv_pool_count > 0)
        sv = sv_pool_stack[--sv_pool_count];
    else
        sv = malloc(sizeof(StradaValue));
    sv->struct_size = 0;
    sv->meta = NULL;
    return sv;
}

/* ===== SMALL INTEGER POOL ===== */
/* Integers -1..255 are pre-allocated and immortal (never freed).
 * Covers loop counters, boolean returns, small constants. */
#define STRADA_SMALL_INT_MIN (-1)
#define STRADA_SMALL_INT_MAX 255
#define STRADA_SMALL_INT_COUNT (STRADA_SMALL_INT_MAX - STRADA_SMALL_INT_MIN + 1)
static StradaValue strada_small_ints[STRADA_SMALL_INT_COUNT];
static int strada_small_ints_initialized = 0;

static void strada_init_small_ints(void) {
    for (int i = 0; i < STRADA_SMALL_INT_COUNT; i++) {
        strada_small_ints[i].type = STRADA_INT;
        strada_small_ints[i].refcount = INT32_MAX;  /* immortal */
        strada_small_ints[i].value.iv = STRADA_SMALL_INT_MIN + i;
        strada_small_ints[i].struct_size = 0;
        strada_small_ints[i].meta = NULL;
    }
    strada_small_ints_initialized = 1;
}

/* Forward declaration for select() default output */
#ifdef STRADA_NO_TLS
static StradaValue *strada_default_output;
#else
static __thread StradaValue *strada_default_output;
#endif

/* Forward declaration for memory profiling */
void strada_memprof_alloc(StradaType type, size_t bytes);
void strada_memprof_free(StradaType type, size_t bytes);

/* OOP debug tracing - set STRADA_DEBUG_BLESS=1 to enable */
static int strada_debug_bless_checked = 0;
static int strada_debug_bless = 0;

static void strada_check_debug_bless(void) {
    if (!strada_debug_bless_checked) {
        const char *env = getenv("STRADA_DEBUG_BLESS");
        strada_debug_bless = env && env[0] == '1';
        strada_debug_bless_checked = 1;
    }
}

/* Validate that a string pointer looks valid (basic sanity check) */
static int strada_validate_blessed_package(const char *pkg) {
    if (!pkg) return 0;

    /* Check if pointer value looks like a valid heap address */
    /* Invalid pointers often have suspicious values like small integers, */
    /* or addresses in the first page (0x0 - 0xFFF) which are never valid */
    uintptr_t addr = (uintptr_t)pkg;
    if (addr < 0x10000) {
        /* Pointer is in low memory - definitely invalid */
        fprintf(stderr, "Warning: blessed_package pointer looks invalid (addr=0x%lx)\n", (unsigned long)addr);
        return 0;
    }

    /* On x86-64 Linux, heap typically starts around 0x55... or higher */
    /* Stack is around 0x7f... User space ends at 0x7fffffffffff */
    /* Anything above that is kernel space and invalid for userspace */
    if (addr > 0x7fffffffffff) {
        fprintf(stderr, "Warning: blessed_package pointer in kernel space (addr=0x%lx)\n", (unsigned long)addr);
        return 0;
    }

    /* Now it's safer to try reading - but use a volatile read to prevent optimization issues */
    volatile unsigned char c = *(volatile unsigned char*)pkg;

    /* Check if first char is printable ASCII (valid package names start with letter) */
    if (c < 32 || c > 126) {
        fprintf(stderr, "Warning: corrupted blessed_package detected (first byte: 0x%02x, addr=0x%lx)\n", c, (unsigned long)addr);
        return 0;
    }
    /* Additional sanity: check string length isn't absurd */
    size_t len = 0;
    while (len < 256 && pkg[len] != '\0') {
        c = (unsigned char)pkg[len];
        if (c < 32 || c > 126) {
            fprintf(stderr, "Warning: corrupted blessed_package detected at pos %zu (byte: 0x%02x)\n", len, c);
            return 0;
        }
        len++;
    }
    if (len == 0 || len >= 256) {
        fprintf(stderr, "Warning: corrupted blessed_package detected (len: %zu)\n", len);
        return 0;
    }
    return 1;
}

StradaValue* strada_new_undef(void) {
    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_UNDEF;
    sv->refcount = 1;
    strada_memprof_alloc(STRADA_UNDEF, sizeof(StradaValue));
    return sv;
}

/* Static undef singleton for void-like returns (push, etc.)
 * This avoids allocating a new StradaValue for each void operation.
 * Has very high refcount so it's never freed. */
static StradaValue __strada_undef_static = {
    .type = STRADA_UNDEF,
    .refcount = INT32_MAX,  /* Effectively immortal */
    .struct_size = 0,
    .meta = NULL
};

StradaValue* strada_undef_static(void) {
    return &__strada_undef_static;
}

/* Static empty array/hash singletons for borrowed-reference error paths.
 * Lazily initialized, immortal (high refcount so never freed). */
static StradaValue *__strada_empty_array_static = NULL;
static StradaValue *__strada_empty_hash_static = NULL;

static StradaValue* strada_empty_array_static(void) {
    if (!__strada_empty_array_static) {
        __strada_empty_array_static = strada_new_array();
        __strada_empty_array_static->refcount = INT32_MAX;
    }
    return __strada_empty_array_static;
}

static StradaValue* strada_empty_hash_static(void) {
    if (!__strada_empty_hash_static) {
        __strada_empty_hash_static = strada_new_hash();
        __strada_empty_hash_static->refcount = INT32_MAX;
    }
    return __strada_empty_hash_static;
}

StradaValue* strada_new_int(int64_t i) {
    /* Return pooled value for small integers */
    if (i >= STRADA_SMALL_INT_MIN && i <= STRADA_SMALL_INT_MAX) {
        if (!strada_small_ints_initialized) strada_init_small_ints();
        return &strada_small_ints[i - STRADA_SMALL_INT_MIN];
    }
    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_INT;
    sv->refcount = 1;
    sv->value.iv = i;
    strada_memprof_alloc(STRADA_INT, sizeof(StradaValue));
    return sv;
}

StradaValue* strada_new_num(double n) {
    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_NUM;
    sv->refcount = 1;
    sv->value.nv = n;
    strada_memprof_alloc(STRADA_NUM, sizeof(StradaValue));
    return sv;
}

/* Safe division - returns undef if divisor is zero */
StradaValue* strada_safe_div(double a, double b) {
    if (b == 0.0) {
        return strada_new_undef();
    }
    return strada_new_num(a / b);
}

/* Safe modulo - returns undef if divisor is zero */
StradaValue* strada_safe_mod(int64_t a, int64_t b) {
    if (b == 0) {
        return strada_new_undef();
    }
    return strada_new_int(a % b);
}

StradaValue* strada_new_str(const char *s) {
    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_STR;
    sv->refcount = 1;
    sv->value.pv = s ? strdup(s) : strdup("");
    sv->struct_size = s ? strlen(s) : 0;  /* Store length */
    strada_memprof_alloc(STRADA_STR, sizeof(StradaValue) + (s ? strlen(s) + 1 : 1));
    return sv;
}

/* Take ownership of a string (no strdup - avoids leak from strada_concat) */
StradaValue* strada_new_str_take(char *s) {
    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_STR;
    sv->refcount = 1;
    sv->value.pv = s ? s : strdup("");
    sv->struct_size = s ? strlen(s) : 0;  /* Store length */
    return sv;
}

/* Create string from binary data with explicit length (may contain embedded NULLs) */
StradaValue* strada_new_str_len(const char *s, size_t len) {
    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_STR;
    sv->refcount = 1;
    if (s && len > 0) {
        sv->value.pv = malloc(len + 1);
        memcpy(sv->value.pv, s, len);
        sv->value.pv[len] = '\0';  /* Null-terminate for C compatibility */
        sv->struct_size = len;     /* Store actual length for binary data */
    } else {
        sv->value.pv = strdup("");
        sv->struct_size = 0;
    }
    return sv;
}

/* Get string length (binary-safe - uses stored length if available) */
size_t strada_str_len(StradaValue *sv) {
    if (!sv || sv->type != STRADA_STR) return 0;
    if (sv->struct_size > 0) return sv->struct_size;
    return sv->value.pv ? strlen(sv->value.pv) : 0;
}

StradaValue* strada_new_array(void) {
    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_ARRAY;
    sv->refcount = 1;
    sv->value.av = strada_array_new();
    strada_memprof_alloc(STRADA_ARRAY, sizeof(StradaValue) + sizeof(StradaArray));
    return sv;
}

StradaValue* strada_new_hash(void) {
    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_HASH;
    sv->refcount = 1;
    sv->value.hv = strada_hash_new();
    strada_memprof_alloc(STRADA_HASH, sizeof(StradaValue) + sizeof(StradaHash));
    return sv;
}

StradaValue* strada_new_filehandle(FILE *fh) {
    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_FILEHANDLE;
    sv->refcount = 1;
    sv->value.fh = fh;
    return sv;
}

/* ===== REFERENCE COUNTING ===== */

/* When threading is inactive, skip expensive atomic operations */
static int strada_threading_active = 0;

void strada_incref(StradaValue *sv) {
    if (!sv) return;
    /* Skip immortal/static values (e.g., __strada_undef_static, small int pool)
     * whose refcount starts at INT32_MAX. Incrementing would overflow to negative. */
    if (sv->refcount > 1000000000) return;
    if (strada_threading_active)
        __sync_add_and_fetch(&sv->refcount, 1);
    else
        sv->refcount++;
}

void strada_decref(StradaValue *sv) {
    if (!sv) return;
    /* Skip immortal/static values to prevent freeing non-heap memory */
    if (sv->refcount > 1000000000) return;
    int new_rc;
    if (strada_threading_active)
        new_rc = __sync_sub_and_fetch(&sv->refcount, 1);
    else
        new_rc = --sv->refcount;
    if (new_rc <= 0) {
        strada_free_value(sv);
    }
}

/* ===== WEAK REFERENCE REGISTRY ===== */

/* A simple hash map from target pointer -> linked list of weak references.
 * When a target is freed, all weak references pointing to it are nullified. */

typedef struct StradaWeakEntry {
    StradaValue *weak_ref;          /* The weak reference StradaValue */
    struct StradaWeakEntry *next;   /* Next entry in the chain */
} StradaWeakEntry;

typedef struct StradaWeakBucket {
    StradaValue *target;            /* The target being referenced */
    StradaWeakEntry *entries;       /* List of weak refs pointing to this target */
    struct StradaWeakBucket *next;  /* Next bucket in hash chain */
} StradaWeakBucket;

#define WEAK_REGISTRY_SIZE 256
static StradaWeakBucket *weak_registry[WEAK_REGISTRY_SIZE];
static pthread_mutex_t weak_registry_mutex = PTHREAD_MUTEX_INITIALIZER;
static int weak_registry_has_entries = 0;  /* Phase 3: skip mutex when no weak refs */

static unsigned int weak_hash_ptr(StradaValue *ptr) {
    uintptr_t val = (uintptr_t)ptr;
    return (unsigned int)((val >> 4) % WEAK_REGISTRY_SIZE);
}

void strada_weak_registry_init(void) {
    memset(weak_registry, 0, sizeof(weak_registry));
}

/* Register a weak reference in the registry */
static void weak_registry_register(StradaValue *target, StradaValue *weak_ref) {
    weak_registry_has_entries = 1;
    unsigned int idx = weak_hash_ptr(target);
    pthread_mutex_lock(&weak_registry_mutex);

    /* Find or create bucket for this target */
    StradaWeakBucket *bucket = weak_registry[idx];
    while (bucket && bucket->target != target) {
        bucket = bucket->next;
    }
    if (!bucket) {
        bucket = malloc(sizeof(StradaWeakBucket));
        bucket->target = target;
        bucket->entries = NULL;
        bucket->next = weak_registry[idx];
        weak_registry[idx] = bucket;
    }

    /* Add this weak ref to the bucket's entry list */
    StradaWeakEntry *entry = malloc(sizeof(StradaWeakEntry));
    entry->weak_ref = weak_ref;
    entry->next = bucket->entries;
    bucket->entries = entry;

    pthread_mutex_unlock(&weak_registry_mutex);
}

/* Remove a specific weak ref from the registry (e.g., when weak ref itself is freed) */
void strada_weak_registry_unregister(StradaValue *ref) {
    if (!ref || !SV_IS_WEAK(ref) || ref->type != STRADA_REF || !ref->value.rv) return;

    StradaValue *target = ref->value.rv;
    unsigned int idx = weak_hash_ptr(target);
    pthread_mutex_lock(&weak_registry_mutex);

    StradaWeakBucket *bucket = weak_registry[idx];
    while (bucket && bucket->target != target) {
        bucket = bucket->next;
    }
    if (bucket) {
        StradaWeakEntry **pp = &bucket->entries;
        while (*pp) {
            if ((*pp)->weak_ref == ref) {
                StradaWeakEntry *to_free = *pp;
                *pp = to_free->next;
                free(to_free);
                break;
            }
            pp = &(*pp)->next;
        }
    }

    pthread_mutex_unlock(&weak_registry_mutex);
}

/* Called when a target value is being freed - nullifies all weak references to it */
void strada_weak_registry_remove_target(StradaValue *target) {
    unsigned int idx = weak_hash_ptr(target);
    pthread_mutex_lock(&weak_registry_mutex);

    StradaWeakBucket **bp = &weak_registry[idx];
    while (*bp) {
        if ((*bp)->target == target) {
            StradaWeakBucket *bucket = *bp;
            /* Nullify all weak references pointing to this target */
            StradaWeakEntry *entry = bucket->entries;
            while (entry) {
                StradaWeakEntry *next = entry->next;
                if (entry->weak_ref) {
                    entry->weak_ref->value.rv = NULL;
                    /* Keep is_weak flag set so isweak() still works */
                }
                free(entry);
                entry = next;
            }
            /* Remove bucket from chain */
            *bp = bucket->next;
            free(bucket);
            pthread_mutex_unlock(&weak_registry_mutex);
            return;
        }
        bp = &(*bp)->next;
    }

    pthread_mutex_unlock(&weak_registry_mutex);
}

void strada_weaken(StradaValue **ref_ptr) {
    if (!ref_ptr || !*ref_ptr) return;
    StradaValue *ref = *ref_ptr;

    /* If this is a STRADA_REF, we need to create a new weak wrapper */
    if (ref->type == STRADA_REF) {
        if (SV_IS_WEAK(ref)) return;  /* Already weak */

        StradaValue *target = ref->value.rv;
        if (!target) return;

        /* If refcount > 1, this value is shared between variables.
         * Create a new StradaValue wrapper so only this variable becomes weak. */
        if (ref->refcount > 1) {
            StradaValue *weak_ref = strada_value_alloc();
            weak_ref->type = STRADA_REF;
            weak_ref->refcount = 1;
            weak_ref->value.rv = target;
            const char *bp = SV_BLESSED(ref);
            if (bp) strada_ensure_meta(weak_ref)->blessed_package = strdup(bp);
            strada_ensure_meta(weak_ref)->is_weak = 1;
            /* Don't incref target - the old ref still holds a strong ref,
             * and we're taking over this variable's strong ref by decrementing. */
            /* Decref the old shared value (removing this variable's strong reference) */
            strada_decref(ref);
            /* Install the new weak wrapper */
            *ref_ptr = weak_ref;
            /* Register in weak registry */
            weak_registry_register(target, weak_ref);
        } else {
            /* refcount == 1, only this variable references this StradaValue.
             * We can convert it in-place. */
            strada_ensure_meta(ref)->is_weak = 1;
            weak_registry_register(target, ref);
            /* Decrement the target's refcount (weak refs don't hold a strong ref) */
            if (target->refcount <= 1000000000) {
                if (strada_threading_active)
                    __sync_sub_and_fetch(&target->refcount, 1);
                else
                    target->refcount--;
            }
        }
    } else {
        /* For non-reference types, weaken is a no-op (like Perl) */
        return;
    }
}

int strada_isweak(StradaValue *ref) {
    if (!ref) return 0;
    return SV_IS_WEAK(ref) ? 1 : 0;
}

/* ===== FILE HANDLE METADATA SIDE TABLE ===== */
/* Tracks special file handles (pipes, in-memory I/O) for proper cleanup */

static StradaFhMeta *fh_meta_head = NULL;

static StradaFhMeta* fh_meta_find(FILE *fh) {
    StradaFhMeta *m = fh_meta_head;
    while (m) {
        if (m->fh == fh) return m;
        m = m->next;
    }
    return NULL;
}

static StradaFhMeta* fh_meta_add(FILE *fh, StradaFhType type) {
    StradaFhMeta *m = calloc(1, sizeof(StradaFhMeta));
    m->fh = fh;
    m->fh_type = type;
    m->next = fh_meta_head;
    fh_meta_head = m;
    return m;
}

static void fh_meta_remove(FILE *fh) {
    StradaFhMeta **pp = &fh_meta_head;
    while (*pp) {
        if ((*pp)->fh == fh) {
            StradaFhMeta *old = *pp;
            *pp = old->next;
            free(old);
            return;
        }
        pp = &(*pp)->next;
    }
}

/* Close a file handle with proper cleanup based on its metadata type.
 * Used by both strada_close() and strada_free_value(). */
static void strada_close_fh_meta(FILE *fh) {
    if (!fh) return;
    StradaFhMeta *meta = fh_meta_find(fh);
    if (!meta) {
        /* Normal file — just fclose */
        fclose(fh);
        return;
    }
    switch (meta->fh_type) {
        case FH_PIPE:
            pclose(fh);
            break;
        case FH_MEMREAD:
            fclose(fh);
            free(meta->mem_buf);
            break;
        case FH_MEMWRITE:
            fclose(fh);
            free(meta->mem_buf);
            break;
        case FH_MEMWRITE_REF: {
            fflush(fh);
            /* Copy buffer contents to the target StradaValue */
            StradaValue *target = meta->target_ref->value.rv;
            if (target && target->type == STRADA_STR) {
                free(target->value.pv);
                target->value.pv = strndup(meta->mem_buf, meta->mem_size);
                target->struct_size = meta->mem_size;
            }
            fclose(fh);
            free(meta->mem_buf);
            strada_decref(meta->target_ref);
            break;
        }
        default:
            fclose(fh);
            break;
    }
    fh_meta_remove(fh);
}

/* Forward declaration for hash function used by weak ref helper */
static unsigned int strada_hash_string(const char *str);

void strada_weaken_hv_entry(StradaHash *hv, const char *key) {
    if (!hv || !key) return;
    /* Find the hash entry */
    unsigned int hash = strada_hash_string(key);
    unsigned int idx = hash & (hv->num_buckets - 1);
    StradaHashEntry *entry = hv->buckets[idx];
    while (entry) {
        if (entry->hash == hash && strcmp(entry->key, key) == 0) {
            /* Found the entry - weaken its value in place */
            strada_weaken(&entry->value);
            return;
        }
        entry = entry->next;
    }
}

/* ===== TYPE CONVERSION ===== */

int64_t strada_to_int(StradaValue *sv) {
    if (!sv) return 0;
    switch (sv->type) {
        case STRADA_INT:
            return sv->value.iv;
        case STRADA_NUM:
            return (int64_t)sv->value.nv;
        case STRADA_STR:
            return sv->value.pv ? atoll(sv->value.pv) : 0;
        case STRADA_ARRAY:
            return strada_array_length(sv->value.av);
        case STRADA_UNDEF:
        default:
            return 0;
    }
}

double strada_to_num(StradaValue *sv) {
    if (!sv) return 0.0;
    switch (sv->type) {
        case STRADA_INT:
            return (double)sv->value.iv;
        case STRADA_NUM:
            return sv->value.nv;
        case STRADA_STR:
            return sv->value.pv ? atof(sv->value.pv) : 0.0;
        case STRADA_ARRAY:
            return (double)strada_array_length(sv->value.av);
        case STRADA_HASH:
            return sv->value.hv ? 1.0 : 0.0;  /* Hash is truthy if non-null */
        case STRADA_REF:
            return sv->value.rv ? 1.0 : 0.0;  /* Ref is truthy if non-null */
        case STRADA_UNDEF:
        default:
            return 0.0;
    }
}

char* strada_to_str(StradaValue *sv) {
    static char buf[128];
    if (!sv) return strdup("");
    
    switch (sv->type) {
        case STRADA_INT:
            snprintf(buf, sizeof(buf), "%lld", (long long)sv->value.iv);
            return strdup(buf);
        case STRADA_NUM:
            snprintf(buf, sizeof(buf), "%g", sv->value.nv);
            return strdup(buf);
        case STRADA_STR:
            return sv->value.pv ? strdup(sv->value.pv) : strdup("");
        case STRADA_ARRAY:
            snprintf(buf, sizeof(buf), "ARRAY(0x%p)", (void*)sv->value.av);
            return strdup(buf);
        case STRADA_HASH:
            snprintf(buf, sizeof(buf), "HASH(0x%p)", (void*)sv->value.hv);
            return strdup(buf);
        case STRADA_FILEHANDLE:
            snprintf(buf, sizeof(buf), "FILEHANDLE(0x%p)", (void*)sv->value.fh);
            return strdup(buf);
        case STRADA_REGEX:
#ifdef HAVE_PCRE2
            snprintf(buf, sizeof(buf), "REGEX(0x%p)", (void*)sv->value.pcre2_rx);
#else
            snprintf(buf, sizeof(buf), "REGEX(0x%p)", (void*)sv->value.rx);
#endif
            return strdup(buf);
        case STRADA_SOCKET:
            snprintf(buf, sizeof(buf), "SOCKET(%d)", sv->value.sock ? sv->value.sock->fd : -1);
            return strdup(buf);
        case STRADA_CSTRUCT: {
            const char *sn = SV_STRUCT_NAME(sv);
            if (sn) {
                snprintf(buf, sizeof(buf), "CSTRUCT(%s,0x%p)", sn, sv->value.ptr);
            } else {
                snprintf(buf, sizeof(buf), "CSTRUCT(0x%p)", sv->value.ptr);
            }
            return strdup(buf);
        }
        case STRADA_CPOINTER:
            snprintf(buf, sizeof(buf), "CPOINTER(0x%p)", sv->value.ptr);
            return strdup(buf);
        case STRADA_REF:
            if (sv->value.rv) {
                snprintf(buf, sizeof(buf), "REF(0x%p)", (void*)sv->value.rv);
                return strdup(buf);
            }
            return strdup("");
        case STRADA_UNDEF:
        default:
            return strdup("");
    }
}

/* Fast integer-to-string conversion without snprintf overhead.
 * Writes digits into buf (must be >= 21 bytes for int64_t range).
 * Returns number of characters written (not including NUL terminator). */
static inline int strada_fast_itoa(int64_t val, char *buf) {
    if (val == 0) {
        buf[0] = '0';
        buf[1] = '\0';
        return 1;
    }
    char tmp[21]; /* max digits for int64_t + sign */
    int pos = 0;
    uint64_t uval;
    int neg = 0;
    if (val < 0) {
        neg = 1;
        uval = (uint64_t)(-(val + 1)) + 1; /* handle INT64_MIN safely */
    } else {
        uval = (uint64_t)val;
    }
    while (uval > 0) {
        tmp[pos++] = '0' + (uval % 10);
        uval /= 10;
    }
    int len = 0;
    if (neg) buf[len++] = '-';
    for (int i = pos - 1; i >= 0; i--) {
        buf[len++] = tmp[i];
    }
    buf[len] = '\0';
    return len;
}

/* Non-allocating string conversion for hot paths.
 * Returns pointer to result string:
 *   - If return == buf, the result is in the caller's stack buffer (don't free)
 *   - If return != buf, it points into the StradaValue's own storage (don't free)
 * For INT/NUM: snprintf into caller's buffer, return buf
 * For STR: return sv->value.pv directly (borrowed pointer)
 * For UNDEF: return "" (static literal) */
const char *strada_to_str_buf(StradaValue *sv, char *buf, size_t buflen) {
    if (!sv) return "";
    switch (sv->type) {
        case STRADA_INT:
            strada_fast_itoa(sv->value.iv, buf);
            return buf;
        case STRADA_NUM:
            snprintf(buf, buflen, "%g", sv->value.nv);
            return buf;
        case STRADA_STR:
            return sv->value.pv ? sv->value.pv : "";
        case STRADA_UNDEF:
        default:
            return "";
    }
}

int strada_to_bool(StradaValue *sv) {
    if (!sv) return 0;
    switch (sv->type) {
        case STRADA_UNDEF:
            return 0;
        case STRADA_INT:
            return sv->value.iv != 0;
        case STRADA_NUM:
            return sv->value.nv != 0.0;
        case STRADA_STR:
            return sv->value.pv && sv->value.pv[0] != '\0' &&
                   strcmp(sv->value.pv, "0") != 0;
        case STRADA_ARRAY:
            return sv->value.av && strada_array_length(sv->value.av) > 0;
        case STRADA_HASH:
            return sv->value.hv && sv->value.hv->num_entries > 0;
        case STRADA_REF:
            /* Dereference and check the target value */
            if (sv->value.rv) {
                return strada_to_bool(sv->value.rv);
            }
            return 0;
        default:
            return 1;
    }
}

/* ===== STRING COMPARISON WITH C LITERALS (no temporaries) ===== */

/* Compare StradaValue string to C string literal - returns 1 if equal.
 * Uses length short-circuit: if lengths differ, strings can't be equal. */
int strada_str_eq_lit(StradaValue *sv, const char *lit) {
    if (!sv || !lit) return 0;
    if (sv->type == STRADA_STR) {
        const char *str = sv->value.pv;
        if (!str) return (*lit == '\0');
        if (str == lit) return 1;  /* pointer equality (interned strings) */
        size_t sv_len = sv->struct_size ? sv->struct_size : strlen(str);
        size_t lit_len = strlen(lit);
        if (sv_len != lit_len) return 0;  /* length short-circuit */
        return memcmp(str, lit, sv_len) == 0;
    }
    char buf[128];
    const char *str = strada_to_str_buf(sv, buf, sizeof(buf));
    return strcmp(str, lit) == 0;
}

/* Compare StradaValue string to C string literal - returns 1 if not equal */
int strada_str_ne_lit(StradaValue *sv, const char *lit) {
    return !strada_str_eq_lit(sv, lit);
}

/* Compare StradaValue string to C string literal - returns 1 if less than */
int strada_str_lt_lit(StradaValue *sv, const char *lit) {
    if (!sv || !lit) return 0;
    char buf[128];
    const char *str = strada_to_str_buf(sv, buf, sizeof(buf));
    return strcmp(str, lit) < 0;
}

/* Compare StradaValue string to C string literal - returns 1 if greater than */
int strada_str_gt_lit(StradaValue *sv, const char *lit) {
    if (!sv || !lit) return 0;
    char buf[128];
    const char *str = strada_to_str_buf(sv, buf, sizeof(buf));
    return strcmp(str, lit) > 0;
}

/* Compare StradaValue string to C string literal - returns 1 if less than or equal */
int strada_str_le_lit(StradaValue *sv, const char *lit) {
    if (!sv || !lit) return 0;
    char buf[128];
    const char *str = strada_to_str_buf(sv, buf, sizeof(buf));
    return strcmp(str, lit) <= 0;
}

/* Compare StradaValue string to C string literal - returns 1 if greater than or equal */
int strada_str_ge_lit(StradaValue *sv, const char *lit) {
    if (!sv || !lit) return 0;
    char buf[128];
    const char *str = strada_to_str_buf(sv, buf, sizeof(buf));
    return strcmp(str, lit) >= 0;
}

/* ===== INCREMENT/DECREMENT OPERATIONS ===== */

StradaValue* strada_postincr(StradaValue **pv) {
    if (!pv || !*pv) return strada_new_undef();
    StradaValue *old = *pv;
    strada_incref(old);  /* keep old alive to return it */
    *pv = strada_new_num(strada_to_num(old) + 1);
    strada_decref(old);  /* pv no longer references old */
    return old;          /* refcount 1 for return value, caller owns it */
}

StradaValue* strada_postdecr(StradaValue **pv) {
    if (!pv || !*pv) return strada_new_undef();
    StradaValue *old = *pv;
    strada_incref(old);  /* keep old alive to return it */
    *pv = strada_new_num(strada_to_num(old) - 1);
    strada_decref(old);  /* pv no longer references old */
    return old;          /* refcount 1 for return value, caller owns it */
}

StradaValue* strada_preincr(StradaValue **pv) {
    if (!pv || !*pv) {
        if (pv) *pv = strada_new_num(1);
        return pv ? *pv : strada_new_undef();
    }
    StradaValue *old = *pv;
    *pv = strada_new_num(strada_to_num(old) + 1);
    strada_decref(old);  /* pv no longer references old */
    return *pv;
}

StradaValue* strada_predecr(StradaValue **pv) {
    if (!pv || !*pv) {
        if (pv) *pv = strada_new_num(-1);
        return pv ? *pv : strada_new_undef();
    }
    StradaValue *old = *pv;
    *pv = strada_new_num(strada_to_num(old) - 1);
    strada_decref(old);  /* pv no longer references old */
    return *pv;
}

/* ===== ARRAY OPERATIONS ===== */

StradaArray* strada_array_new(void) {
    StradaArray *av = malloc(sizeof(StradaArray));
    av->capacity = strada_default_array_capacity;
    av->size = 0;
    av->head = 0;
    av->elements = calloc(av->capacity, sizeof(StradaValue*));
    av->refcount = 1;
    return av;
}

void strada_array_push(StradaArray *av, StradaValue *sv) {
    if (!av) return;

    if (av->head + av->size >= av->capacity) {
        if (av->head > 0) {
            /* Compact: move elements to start before growing */
            memmove(av->elements, av->elements + av->head, av->size * sizeof(StradaValue*));
            av->head = 0;
        }
        if (av->size >= av->capacity) {
            av->capacity *= 2;
            av->elements = realloc(av->elements, av->capacity * sizeof(StradaValue*));
        }
    }

    av->elements[av->head + av->size] = sv;
    av->size++;
    strada_incref(sv);
}

/* Deep copy an array - creates a new StradaValue wrapping a new StradaArray
   with incref'd copies of each element. Follows references. Caller owns the returned value. */
StradaValue* strada_array_copy(StradaValue *src) {
    StradaValue *dst = strada_new_array();
    if (!src) return dst;
    /* Follow reference chain to find the array */
    StradaValue *current = src;
    while (current && current->type == STRADA_REF) {
        current = current->value.rv;
    }
    if (!current || current->type != STRADA_ARRAY || !current->value.av) return dst;
    StradaArray *sav = current->value.av;
    StradaArray *dav = dst->value.av;
    if (sav->size > 0) {
        if (sav->size > dav->capacity) {
            dav->capacity = sav->size;
            dav->elements = realloc(dav->elements, dav->capacity * sizeof(StradaValue*));
        }
        for (size_t i = 0; i < sav->size; i++) {
            dav->elements[i] = sav->elements[sav->head + i];
            strada_incref(sav->elements[sav->head + i]);
        }
        dav->size = sav->size;
    }
    return dst;
}

/* Push without incref - caller donates ownership of newly created value */
void strada_array_push_take(StradaArray *av, StradaValue *sv) {
    if (!av) return;

    if (av->head + av->size >= av->capacity) {
        if (av->head > 0) {
            memmove(av->elements, av->elements + av->head, av->size * sizeof(StradaValue*));
            av->head = 0;
        }
        if (av->size >= av->capacity) {
            av->capacity *= 2;
            av->elements = realloc(av->elements, av->capacity * sizeof(StradaValue*));
        }
    }

    av->elements[av->head + av->size] = sv;
    av->size++;
    /* No incref - value starts with refcount 1 and array takes ownership */
}

StradaValue* strada_array_pop(StradaArray *av) {
    if (!av || av->size == 0) return strada_new_undef();
    av->size--;
    return av->elements[av->head + av->size];
}

StradaValue* strada_array_shift(StradaArray *av) {
    if (!av || av->size == 0) return strada_new_undef();

    StradaValue *result = av->elements[av->head];
    av->head++;
    av->size--;
    return result;  /* O(1) — no memmove */
}

void strada_array_unshift(StradaArray *av, StradaValue *sv) {
    if (!av) return;

    if (av->head > 0) {
        /* O(1) path: use head space */
        av->head--;
        av->elements[av->head] = sv;
        av->size++;
        strada_incref(sv);
    } else {
        /* Fallback: need to memmove */
        if (av->head + av->size >= av->capacity) {
            av->capacity *= 2;
            av->elements = realloc(av->elements, av->capacity * sizeof(StradaValue*));
        }
        memmove(av->elements + 1, av->elements, av->size * sizeof(StradaValue*));
        av->elements[0] = sv;
        av->size++;
        strada_incref(sv);
    }
}

StradaValue* strada_array_get(StradaArray *av, int64_t idx) {
    if (!av) return strada_new_undef();

    /* Handle negative indices (-1 = last element, -2 = second to last, etc.) */
    if (idx < 0) {
        idx = (int64_t)av->size + idx;
    }

    /* Check bounds */
    if (idx < 0 || (size_t)idx >= av->size) return strada_new_undef();

    // Return borrowed reference - array still owns it
    return av->elements[av->head + idx];
}

/* Get array element from StradaValue* - safe for destructuring.
 * Always returns an OWNED reference (caller must decref).
 * For in-bounds: increfs the element before returning.
 * For out-of-bounds: returns a new undef (already owned). */
StradaValue* strada_array_get_safe(StradaValue *arr, int64_t idx) {
    if (!arr) return strada_new_undef();

    /* Handle array reference or direct array */
    StradaValue *actual = arr;
    if (arr->type == STRADA_REF && arr->value.rv) {
        actual = arr->value.rv;
    }

    if (actual->type != STRADA_ARRAY || !actual->value.av) {
        return strada_new_undef();
    }

    StradaArray *av = actual->value.av;
    int64_t real_idx = idx;
    if (real_idx < 0) real_idx += (int64_t)av->size;
    if (real_idx < 0 || real_idx >= (int64_t)av->size) {
        return strada_new_undef();
    }

    StradaValue *elem = av->elements[av->head + real_idx];
    strada_incref(elem);
    return elem;
}

void strada_array_set(StradaArray *av, int64_t idx, StradaValue *sv) {
    if (!av) return;

    /* Handle negative indices */
    if (idx < 0) {
        idx = (int64_t)av->size + idx;
    }

    /* Cannot set negative indices that are out of bounds */
    if (idx < 0) return;

    size_t uidx = (size_t)idx;

    /* Extend array if necessary */
    while (av->head + uidx >= av->capacity) {
        if (av->head > 0 && uidx < av->capacity) {
            /* Compact first */
            memmove(av->elements, av->elements + av->head, av->size * sizeof(StradaValue*));
            av->head = 0;
        } else {
            av->capacity *= 2;
            av->elements = realloc(av->elements, av->capacity * sizeof(StradaValue*));
        }
    }

    /* Fill gaps with undef */
    while (av->size <= uidx) {
        av->elements[av->head + av->size] = strada_new_undef();
        av->size++;
    }

    if (av->elements[av->head + uidx]) {
        strada_decref(av->elements[av->head + uidx]);
    }

    av->elements[av->head + uidx] = sv;
    strada_incref(sv);
}

size_t strada_array_length(StradaArray *av) {
    return av ? av->size : 0;
}

/* Reverse array in-place */
void strada_array_reverse(StradaArray *av) {
    if (!av || av->size < 2) return;

    size_t left = av->head;
    size_t right = av->head + av->size - 1;

    while (left < right) {
        StradaValue *tmp = av->elements[left];
        av->elements[left] = av->elements[right];
        av->elements[right] = tmp;
        left++;
        right--;
    }
}

/* Get/set default array capacity for new arrays */
int64_t strada_get_array_default_capacity(void) {
    return (int64_t)strada_default_array_capacity;
}

void strada_set_array_default_capacity(int64_t capacity) {
    if (capacity > 0) {
        strada_default_array_capacity = (size_t)capacity;
    }
}

/* Get/set default hash capacity for new hashes */
int64_t strada_get_hash_default_capacity(void) {
    return (int64_t)strada_default_hash_capacity;
}

void strada_set_hash_default_capacity(int64_t capacity) {
    if (capacity > 0) {
        strada_default_hash_capacity = strada_next_pow2((size_t)capacity);
    }
}

/* Reserve capacity for an existing array (pre-allocate without changing size) */
void strada_array_reserve(StradaArray *av, size_t capacity) {
    if (!av || capacity <= av->capacity) return;

    av->elements = realloc(av->elements, capacity * sizeof(StradaValue*));
    /* Zero out new slots */
    for (size_t i = av->capacity; i < capacity; i++) {
        av->elements[i] = NULL;
    }
    av->capacity = capacity;
}

/* Reserve capacity for array value (handles refs) */
void strada_reserve_sv(StradaValue *sv, int64_t capacity) {
    if (!sv || capacity <= 0) return;

    /* Handle array references */
    if (sv->type == STRADA_REF && sv->value.rv && sv->value.rv->type == STRADA_ARRAY) {
        strada_array_reserve(sv->value.rv->value.av, (size_t)capacity);
        return;
    }

    /* Handle arrays directly */
    if (sv->type == STRADA_ARRAY && sv->value.av) {
        strada_array_reserve(sv->value.av, (size_t)capacity);
    }
}

/* Generic size function - works with arrays, hashes, and references to them */
int64_t strada_size(StradaValue *sv) {
    if (!sv) return 0;

    /* Handle references by dereferencing */
    if (sv->type == STRADA_REF && sv->value.rv) {
        sv = sv->value.rv;
    }

    switch (sv->type) {
        case STRADA_ARRAY:
            return sv->value.av ? (int64_t)sv->value.av->size : 0;
        case STRADA_HASH:
            return sv->value.hv ? (int64_t)sv->value.hv->num_entries : 0;
        case STRADA_STR:
            return sv->value.pv ? (int64_t)strlen(sv->value.pv) : 0;
        default:
            return 0;
    }
}

/* Comparison function for qsort - alphabetical (string) sort */
static int strada_sort_cmp_str(const void *a, const void *b) {
    StradaValue *va = *(StradaValue **)a;
    StradaValue *vb = *(StradaValue **)b;
    char bufa[128], bufb[128];
    const char *sa = strada_to_str_buf(va, bufa, sizeof(bufa));
    const char *sb = strada_to_str_buf(vb, bufb, sizeof(bufb));
    return strcmp(sa, sb);
}

/* Comparison function for qsort - numeric sort */
static int strada_sort_cmp_num(const void *a, const void *b) {
    StradaValue *va = *(StradaValue **)a;
    StradaValue *vb = *(StradaValue **)b;
    double na = strada_to_num(va);
    double nb = strada_to_num(vb);
    if (na < nb) return -1;
    if (na > nb) return 1;
    return 0;
}

/* Sort array alphabetically (by string comparison) */
StradaValue* strada_sort(StradaValue *arr) {
    if (!arr) {
        return strada_new_array();
    }

    /* Handle array references */
    StradaArray *av = NULL;
    if (arr->type == STRADA_REF && arr->value.rv && arr->value.rv->type == STRADA_ARRAY) {
        av = arr->value.rv->value.av;
    } else if (arr->type == STRADA_ARRAY) {
        av = arr->value.av;
    }

    if (!av || av->size == 0) {
        return strada_new_array();
    }

    /* Create a new array with sorted elements */
    StradaValue *result = strada_new_array();
    StradaArray *result_av = result->value.av;

    /* Copy elements to result */
    for (size_t i = 0; i < av->size; i++) {
        strada_array_push(result_av, av->elements[av->head + i]);
    }

    /* Sort in place */
    qsort(result_av->elements + result_av->head, result_av->size, sizeof(StradaValue*), strada_sort_cmp_str);

    return result;
}

/* Sort array numerically */
StradaValue* strada_nsort(StradaValue *arr) {
    if (!arr) {
        return strada_new_array();
    }

    /* Handle array references */
    StradaArray *av = NULL;
    if (arr->type == STRADA_REF && arr->value.rv && arr->value.rv->type == STRADA_ARRAY) {
        av = arr->value.rv->value.av;
    } else if (arr->type == STRADA_ARRAY) {
        av = arr->value.av;
    }

    if (!av || av->size == 0) {
        return strada_new_array();
    }

    /* Create a new array with sorted elements */
    StradaValue *result = strada_new_array();
    StradaArray *result_av = result->value.av;

    /* Copy elements to result */
    for (size_t i = 0; i < av->size; i++) {
        strada_array_push(result_av, av->elements[av->head + i]);
    }

    /* Sort in place */
    qsort(result_av->elements + result_av->head, result_av->size, sizeof(StradaValue*), strada_sort_cmp_num);

    return result;
}

/* Create array from range (start..end) */
StradaValue* strada_range(StradaValue *start, StradaValue *end) {
    StradaValue *result = strada_new_array();
    StradaArray *av = result->value.av;

    int64_t start_val = strada_to_num(start);
    int64_t end_val = strada_to_num(end);

    if (start_val <= end_val) {
        /* Ascending range */
        for (int64_t i = start_val; i <= end_val; i++) {
            strada_array_push_take(av, strada_new_int(i));
        }
    } else {
        /* Descending range */
        for (int64_t i = start_val; i >= end_val; i--) {
            strada_array_push_take(av, strada_new_int(i));
        }
    }

    return result;
}

/* ===== STRING INTERNING FOR HASH KEYS ===== */
/* Short strings (<=64 bytes) are interned to avoid duplicate key allocations
 * and enable pointer-equality fast path in hash lookups. */
#define INTERN_INITIAL_SIZE 1024
#define INTERN_MAX_LEN 64

typedef struct InternEntry {
    char *str;
    int refcount;
    unsigned int hash;  /* Cached full hash for fast resize */
    struct InternEntry *next;
} InternEntry;

static InternEntry **intern_table_buckets = NULL;
static size_t intern_table_size = INTERN_INITIAL_SIZE;
static size_t intern_table_count = 0;
static int intern_table_initialized = 0;

static unsigned int intern_hash_full(const char *s) {
    unsigned int h = 5381;
    for (const char *p = s; *p; p++) {
        h = ((h << 5) + h) + (unsigned char)*p;
    }
    return h;
}

static void strada_intern_resize(void) {
    size_t new_size = intern_table_size * 2;
    InternEntry **new_buckets = calloc(new_size, sizeof(InternEntry*));
    for (size_t i = 0; i < intern_table_size; i++) {
        InternEntry *e = intern_table_buckets[i];
        while (e) {
            InternEntry *next = e->next;
            unsigned int idx = e->hash & (new_size - 1);
            e->next = new_buckets[idx];
            new_buckets[idx] = e;
            e = next;
        }
    }
    free(intern_table_buckets);
    intern_table_buckets = new_buckets;
    intern_table_size = new_size;
}

static char *strada_intern_str(const char *s) {
    if (!s) return strdup("");
    size_t len = strlen(s);
    if (len > INTERN_MAX_LEN) return strdup(s);  /* Don't intern long strings */

    if (!intern_table_initialized) {
        intern_table_buckets = calloc(intern_table_size, sizeof(InternEntry*));
        intern_table_initialized = 1;
    }

    unsigned int full_h = intern_hash_full(s);
    unsigned int h = full_h & (intern_table_size - 1);
    InternEntry *e = intern_table_buckets[h];
    while (e) {
        if (strcmp(e->str, s) == 0) {
            e->refcount++;
            return e->str;
        }
        e = e->next;
    }

    /* New interned string */
    e = malloc(sizeof(InternEntry));
    e->str = strdup(s);
    e->refcount = 1;
    e->hash = full_h;
    e->next = intern_table_buckets[h];
    intern_table_buckets[h] = e;
    intern_table_count++;

    /* Resize when load factor exceeds 2 */
    if (intern_table_count > intern_table_size * 2) {
        strada_intern_resize();
    }

    return e->str;
}

static void strada_intern_release(char *s) {
    if (!s || !intern_table_initialized) { free(s); return; }
    size_t len = strlen(s);
    if (len > INTERN_MAX_LEN) { free(s); return; }

    unsigned int h = intern_hash_full(s) & (intern_table_size - 1);
    InternEntry **pp = &intern_table_buckets[h];
    while (*pp) {
        if ((*pp)->str == s) {  /* Pointer comparison — fast */
            (*pp)->refcount--;
            if ((*pp)->refcount <= 0) {
                InternEntry *old = *pp;
                *pp = old->next;
                free(old->str);
                free(old);
                intern_table_count--;
            }
            return;
        }
        pp = &(*pp)->next;
    }
    /* Not interned (shouldn't happen for keys created via strada_intern_str) */
    free(s);
}

static void strada_intern_cleanup(void) {
    if (!intern_table_initialized) return;
    for (size_t i = 0; i < intern_table_size; i++) {
        InternEntry *e = intern_table_buckets[i];
        while (e) {
            InternEntry *next = e->next;
            free(e->str);
            free(e);
            e = next;
        }
    }
    free(intern_table_buckets);
    intern_table_buckets = NULL;
    intern_table_size = INTERN_INITIAL_SIZE;
    intern_table_count = 0;
    intern_table_initialized = 0;
}

/* ===== HASH OPERATIONS ===== */

static unsigned int strada_hash_string(const char *str) {
    unsigned int hash = 5381;
    int c;
    while ((c = *str++)) {
        hash = ((hash << 5) + hash) + c;
    }
    return hash;
}

/* Resize hash table when load factor exceeds threshold */
static void strada_hash_resize(StradaHash *hv) {
    if (!hv) return;

    size_t old_num_buckets = hv->num_buckets;
    size_t new_num_buckets = old_num_buckets * 2;
    StradaHashEntry **old_buckets = hv->buckets;

    /* Allocate new bucket array */
    hv->buckets = calloc(new_num_buckets, sizeof(StradaHashEntry*));
    hv->num_buckets = new_num_buckets;

    /* Rehash all entries into new buckets */
    for (size_t i = 0; i < old_num_buckets; i++) {
        StradaHashEntry *entry = old_buckets[i];
        while (entry) {
            StradaHashEntry *next = entry->next;

            /* Use cached hash, bitmask for bucket index */
            unsigned int bucket = entry->hash & (new_num_buckets - 1);

            /* Insert at head of new bucket chain */
            entry->next = hv->buckets[bucket];
            hv->buckets[bucket] = entry;

            entry = next;
        }
    }

    free(old_buckets);
}

StradaHash* strada_hash_new(void) {
    static int freelist_cleanup_registered = 0;
    if (!freelist_cleanup_registered) {
        atexit(strada_hash_entry_freelist_destroy);
        atexit(strada_intern_cleanup);
        atexit(strada_sv_pool_cleanup);
        atexit(strada_meta_pool_cleanup);
        freelist_cleanup_registered = 1;
    }
    StradaHash *hv = malloc(sizeof(StradaHash));
    hv->num_buckets = strada_default_hash_capacity;
    hv->num_entries = 0;
    hv->buckets = calloc(hv->num_buckets, sizeof(StradaHashEntry*));
    hv->refcount = 1;
    hv->iter_bucket = 0;
    hv->iter_entry = NULL;
    return hv;
}

void strada_hash_set(StradaHash *hv, const char *key, StradaValue *sv) {
    if (!hv || !key) return;

    unsigned int hash = strada_hash_string(key);
    unsigned int bucket = hash & (hv->num_buckets - 1);

    /* Check if key exists */
    StradaHashEntry *entry = hv->buckets[bucket];
    while (entry) {
        if (entry->hash == hash && strcmp(entry->key, key) == 0) {
            /* IMPORTANT: incref new value BEFORE decref old value
             * This handles the case where sv == entry->value with refcount 1.
             * Without this order, decref would free the object before incref. */
            strada_incref(sv);
            strada_decref(entry->value);
            entry->value = sv;
            return;
        }
        entry = entry->next;
    }

    /* Add new entry - incref the value for shared ownership */
    entry = strada_hash_entry_alloc();
    entry->key = strada_intern_str(key);
    entry->value = sv;
    entry->hash = hash;
    strada_incref(sv);
    entry->next = hv->buckets[bucket];
    hv->buckets[bucket] = entry;
    hv->num_entries++;

    /* Resize if load factor exceeds 0.75 (num_entries * 4 > num_buckets * 3) */
    if (hv->num_entries * 4 > hv->num_buckets * 3) {
        strada_hash_resize(hv);
    }
}

/* strada_hash_set_take - set hash entry, taking ownership (no incref on new value) */
void strada_hash_set_take(StradaHash *hv, const char *key, StradaValue *sv) {
    if (!hv || !key) return;

    unsigned int hash = strada_hash_string(key);
    unsigned int bucket = hash & (hv->num_buckets - 1);

    /* Check if key exists */
    StradaHashEntry *entry = hv->buckets[bucket];
    while (entry) {
        if (entry->hash == hash && strcmp(entry->key, key) == 0) {
            /* Replace: decref old, take ownership of new (no incref) */
            strada_decref(entry->value);
            entry->value = sv;
            return;
        }
        entry = entry->next;
    }

    /* Add new entry - take ownership (no incref) */
    entry = strada_hash_entry_alloc();
    entry->key = strada_intern_str(key);
    entry->value = sv;
    entry->hash = hash;
    entry->next = hv->buckets[bucket];
    hv->buckets[bucket] = entry;
    hv->num_entries++;

    if (hv->num_entries * 4 > hv->num_buckets * 3) {
        strada_hash_resize(hv);
    }
}

StradaValue* strada_hash_get(StradaHash *hv, const char *key) {
    if (!hv || !key) return strada_undef_static();

    unsigned int hash = strada_hash_string(key);
    unsigned int bucket = hash & (hv->num_buckets - 1);

    StradaHashEntry *entry = hv->buckets[bucket];
    while (entry) {
        if (entry->hash == hash && (entry->key == key || strcmp(entry->key, key) == 0)) {
            // Return borrowed reference - hash still owns it
            return entry->value;
        }
        entry = entry->next;
    }

    return strada_undef_static();
}

int strada_hash_exists(StradaHash *hv, const char *key) {
    if (!hv || !key) return 0;

    unsigned int hash = strada_hash_string(key);
    unsigned int bucket = hash & (hv->num_buckets - 1);

    StradaHashEntry *entry = hv->buckets[bucket];
    while (entry) {
        if (entry->hash == hash && (entry->key == key || strcmp(entry->key, key) == 0)) {
            return 1;
        }
        entry = entry->next;
    }

    return 0;
}

void strada_hash_delete(StradaHash *hv, const char *key) {
    if (!hv || !key) return;

    unsigned int hash = strada_hash_string(key);
    unsigned int bucket = hash & (hv->num_buckets - 1);

    StradaHashEntry *entry = hv->buckets[bucket];
    StradaHashEntry *prev = NULL;

    while (entry) {
        if (entry->hash == hash && strcmp(entry->key, key) == 0) {
            if (prev) {
                prev->next = entry->next;
            } else {
                hv->buckets[bucket] = entry->next;
            }

            strada_intern_release(entry->key);
            strada_decref(entry->value);
            strada_hash_entry_free(entry);
            hv->num_entries--;
            return;
        }
        prev = entry;
        entry = entry->next;
    }
}

StradaArray* strada_hash_keys(StradaHash *hv) {
    StradaArray *av = strada_array_new();
    if (!hv) return av;
    
    for (size_t i = 0; i < hv->num_buckets; i++) {
        StradaHashEntry *entry = hv->buckets[i];
        while (entry) {
            strada_array_push_take(av, strada_new_str(entry->key));
            entry = entry->next;
        }
    }
    
    return av;
}

StradaArray* strada_hash_values(StradaHash *hv) {
    StradaArray *av = strada_array_new();
    if (!hv) return av;

    for (size_t i = 0; i < hv->num_buckets; i++) {
        StradaHashEntry *entry = hv->buckets[i];
        while (entry) {
            strada_array_push(av, entry->value);
            entry = entry->next;
        }
    }

    return av;
}

/* Reserve capacity for hash (pre-allocate buckets) */
void strada_hash_reserve(StradaHash *hv, size_t num_buckets) {
    /* Round up to power of 2 for bitmask indexing */
    num_buckets = strada_next_pow2(num_buckets);
    if (!hv || num_buckets <= hv->num_buckets) return;

    size_t old_num_buckets = hv->num_buckets;
    StradaHashEntry **old_buckets = hv->buckets;

    /* Allocate new bucket array */
    hv->buckets = calloc(num_buckets, sizeof(StradaHashEntry*));
    hv->num_buckets = num_buckets;

    /* Rehash all entries into new buckets */
    for (size_t i = 0; i < old_num_buckets; i++) {
        StradaHashEntry *entry = old_buckets[i];
        while (entry) {
            StradaHashEntry *next = entry->next;

            /* Use cached hash, bitmask for bucket index */
            unsigned int bucket = entry->hash & (num_buckets - 1);

            /* Insert at head of new bucket chain */
            entry->next = hv->buckets[bucket];
            hv->buckets[bucket] = entry;

            entry = next;
        }
    }

    free(old_buckets);
}

/* Reserve capacity for hash value (handles refs) */
void strada_hash_reserve_sv(StradaValue *sv, int64_t capacity) {
    if (!sv || capacity <= 0) return;

    /* Handle hash references */
    if (sv->type == STRADA_REF && sv->value.rv && sv->value.rv->type == STRADA_HASH) {
        strada_hash_reserve(sv->value.rv->value.hv, (size_t)capacity);
        return;
    }

    /* Handle hashes directly */
    if (sv->type == STRADA_HASH && sv->value.hv) {
        strada_hash_reserve(sv->value.hv, (size_t)capacity);
    }
}

/* ===== UTF-8 HELPER FUNCTIONS ===== */

/* Get the byte length of a UTF-8 character from its first byte */
static int utf8_char_len(unsigned char c) {
    if ((c & 0x80) == 0) return 1;        /* 0xxxxxxx - ASCII */
    if ((c & 0xE0) == 0xC0) return 2;     /* 110xxxxx */
    if ((c & 0xF0) == 0xE0) return 3;     /* 1110xxxx */
    if ((c & 0xF8) == 0xF0) return 4;     /* 11110xxx */
    return 1;  /* Invalid, treat as single byte */
}

/* Check if byte is a UTF-8 continuation byte */
static int utf8_is_continuation(unsigned char c) {
    return (c & 0xC0) == 0x80;
}

/* Count UTF-8 codepoints in a string */
static size_t utf8_strlen(const char *s) {
    if (!s) return 0;
    size_t count = 0;
    const unsigned char *p = (const unsigned char *)s;
    while (*p) {
        if (!utf8_is_continuation(*p)) {
            count++;
        }
        p++;
    }
    return count;
}

/* Get byte offset for the nth UTF-8 character (0-indexed) */
static size_t utf8_offset(const char *s, size_t char_index) {
    if (!s) return 0;
    const unsigned char *p = (const unsigned char *)s;
    size_t chars = 0;
    size_t bytes = 0;

    while (*p && chars < char_index) {
        int len = utf8_char_len(*p);
        p += len;
        bytes += len;
        chars++;
    }
    return bytes;
}

/* Decode a UTF-8 sequence to a Unicode codepoint */
static uint32_t utf8_decode(const char *s, int *bytes_read) {
    const unsigned char *p = (const unsigned char *)s;
    uint32_t codepoint = 0;
    int len = utf8_char_len(*p);

    if (bytes_read) *bytes_read = len;

    switch (len) {
        case 1:
            return *p;
        case 2:
            codepoint = (*p & 0x1F) << 6;
            codepoint |= (*(p+1) & 0x3F);
            return codepoint;
        case 3:
            codepoint = (*p & 0x0F) << 12;
            codepoint |= (*(p+1) & 0x3F) << 6;
            codepoint |= (*(p+2) & 0x3F);
            return codepoint;
        case 4:
            codepoint = (*p & 0x07) << 18;
            codepoint |= (*(p+1) & 0x3F) << 12;
            codepoint |= (*(p+2) & 0x3F) << 6;
            codepoint |= (*(p+3) & 0x3F);
            return codepoint;
        default:
            return *p;
    }
}

/* Encode a Unicode codepoint to UTF-8, returns number of bytes written */
static int utf8_encode(uint32_t codepoint, char *out) {
    if (codepoint < 0x80) {
        out[0] = (char)codepoint;
        return 1;
    } else if (codepoint < 0x800) {
        out[0] = (char)(0xC0 | (codepoint >> 6));
        out[1] = (char)(0x80 | (codepoint & 0x3F));
        return 2;
    } else if (codepoint < 0x10000) {
        out[0] = (char)(0xE0 | (codepoint >> 12));
        out[1] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[2] = (char)(0x80 | (codepoint & 0x3F));
        return 3;
    } else if (codepoint < 0x110000) {
        out[0] = (char)(0xF0 | (codepoint >> 18));
        out[1] = (char)(0x80 | ((codepoint >> 12) & 0x3F));
        out[2] = (char)(0x80 | ((codepoint >> 6) & 0x3F));
        out[3] = (char)(0x80 | (codepoint & 0x3F));
        return 4;
    }
    /* Invalid codepoint, output replacement character */
    out[0] = '?';
    return 1;
}

/* ===== STRING OPERATIONS ===== */

char* strada_concat(const char *a, const char *b) {
    size_t len_a = a ? strlen(a) : 0;
    size_t len_b = b ? strlen(b) : 0;
    char *result = malloc(len_a + len_b + 1);

    if (a) strcpy(result, a);
    else result[0] = '\0';

    if (b) strcat(result, b);

    return result;
}

/* Concatenate two strings and FREE the inputs (avoids memory leaks in chains) */
char* strada_concat_free(char *a, char *b) {
    size_t len_a = a ? strlen(a) : 0;
    size_t len_b = b ? strlen(b) : 0;
    char *result = malloc(len_a + len_b + 1);

    if (a) strcpy(result, a);
    else result[0] = '\0';

    if (b) strcat(result, b);

    free(a);
    free(b);

    return result;
}

/* Optimized string concatenation working directly on StradaValues
 * KEY OPTIMIZATION: In-place append when a has refcount == 1
 * This makes $s = $s . "x" loops O(n) instead of O(n²) */
StradaValue* strada_concat_sv(StradaValue *a, StradaValue *b) {
    /* Get string pointer and length for b */
    const char *str_b = "";
    size_t len_b = 0;
    char buf_b[32];

    if (b) {
        if (b->type == STRADA_STR && b->value.pv) {
            str_b = b->value.pv;
            len_b = b->struct_size;
        } else if (b->type == STRADA_INT) {
            len_b = strada_fast_itoa(b->value.iv, buf_b);
            str_b = buf_b;
        } else if (b->type == STRADA_NUM) {
            len_b = snprintf(buf_b, sizeof(buf_b), "%g", b->value.nv);
            str_b = buf_b;
        } else if (b->type == STRADA_REF && b->value.rv) {
            len_b = snprintf(buf_b, sizeof(buf_b), "REF(0x%p)", (void*)b->value.rv);
            str_b = buf_b;
        }
    }

    /* Create new string */
    const char *str_a = "";
    size_t len_a = 0;
    char buf_a[32];

    if (a) {
        if (a->type == STRADA_STR && a->value.pv) {
            str_a = a->value.pv;
            len_a = a->struct_size;
        } else if (a->type == STRADA_INT) {
            len_a = strada_fast_itoa(a->value.iv, buf_a);
            str_a = buf_a;
        } else if (a->type == STRADA_NUM) {
            len_a = snprintf(buf_a, sizeof(buf_a), "%g", a->value.nv);
            str_a = buf_a;
        } else if (a->type == STRADA_REF && a->value.rv) {
            len_a = snprintf(buf_a, sizeof(buf_a), "REF(0x%p)", (void*)a->value.rv);
            str_a = buf_a;
        }
    }

    /* Single allocation for result */
    char *result = malloc(len_a + len_b + 1);
    if (len_a > 0) memcpy(result, str_a, len_a);
    if (len_b > 0) memcpy(result + len_a, str_b, len_b);
    result[len_a + len_b] = '\0';

    /* Create result StradaValue directly */
    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_STR;
    sv->refcount = 1;
    sv->value.pv = result;
    sv->struct_size = len_a + len_b;

    return sv;
}

/* In-place string concatenation for $s = $s . expr and $s .= expr patterns.
 * CONSUMES a (reuses if refcount==1, otherwise decrefs after creating new).
 * BORROWS b (caller handles cleanup).
 * Returns owned StradaValue*. */
StradaValue* strada_concat_inplace(StradaValue *a, StradaValue *b) {
    /* Get string pointer and length for b */
    const char *str_b = "";
    size_t len_b = 0;
    char buf_b[32];

    if (b) {
        if (b->type == STRADA_STR && b->value.pv) {
            str_b = b->value.pv;
            len_b = b->struct_size;
        } else if (b->type == STRADA_INT) {
            len_b = strada_fast_itoa(b->value.iv, buf_b);
            str_b = buf_b;
        } else if (b->type == STRADA_NUM) {
            len_b = snprintf(buf_b, sizeof(buf_b), "%g", b->value.nv);
            str_b = buf_b;
        }
    }

    /* Fast path: realloc in place when a is a string with refcount 1 */
    if (a && a->type == STRADA_STR && a->refcount == 1 && a->value.pv) {
        size_t len_a = a->struct_size;
        size_t new_len = len_a + len_b;
#ifdef __linux__
        /* Check if existing buffer has room (malloc often over-allocates) */
        size_t usable = malloc_usable_size(a->value.pv);
        if (usable >= new_len + 1) {
            if (len_b > 0) memcpy(a->value.pv + len_a, str_b, len_b);
            a->value.pv[new_len] = '\0';
            a->struct_size = new_len;
            return a;
        }
#endif
        /* Need to grow — use exponential growth to avoid O(n^2) realloc */
        size_t new_cap = len_a < 64 ? 128 : len_a * 2;
        if (new_cap < new_len + 1) new_cap = new_len + 1;
        a->value.pv = realloc(a->value.pv, new_cap);
        if (len_b > 0) memcpy(a->value.pv + len_a, str_b, len_b);
        a->value.pv[new_len] = '\0';
        a->struct_size = new_len;
        return a;
    }

    /* Slow path: create new string, decref a */
    StradaValue *result = strada_concat_sv(a, b);
    strada_decref(a);
    return result;
}

/* Fast character access by byte index - returns char code, no allocation */
StradaValue* strada_char_at(StradaValue *str, StradaValue *index) {
    if (!str || str->type != STRADA_STR || !str->value.pv) {
        return strada_new_int(0);
    }
    int64_t idx = strada_to_int(index);
    size_t len = str->struct_size;  /* Cached length */
    if (idx < 0 || (size_t)idx >= len) {
        return strada_new_int(0);
    }
    return strada_new_int((unsigned char)str->value.pv[idx]);
}

/* Returns length in UTF-8 codepoints (characters), not bytes */
size_t strada_length(const char *s) {
    return utf8_strlen(s);
}

/* Binary-safe length - uses struct_size to handle embedded NULLs.
 * Returns byte length (not character count) for binary safety. */
size_t strada_length_sv(StradaValue *sv) {
    if (!sv) return 0;
    if (sv->type == STRADA_STR && sv->value.pv) {
        return sv->struct_size;
    }
    /* For non-strings, fall back to converting and measuring */
    char _tb[256];
    const char *s = strada_to_str_buf(sv, _tb, sizeof(_tb));
    return strlen(s);
}

/* Returns length in bytes (for when you need byte-level operations) */
size_t strada_bytes(const char *s) {
    return s ? strlen(s) : 0;
}

/* UTF-8 aware substr - offset and length are in characters, not bytes.
 * Binary-safe: uses struct_size instead of strlen for input strings. */
StradaValue* strada_substr(StradaValue *str, int64_t offset, int64_t length) {
    /* Get source data - use struct_size for binary safety */
    const char *s;
    size_t byte_len;
    char _tb[256];

    if (str && str->type == STRADA_STR && str->value.pv) {
        s = str->value.pv;
        byte_len = str->struct_size;  /* Binary-safe: use struct_size */
    } else {
        s = strada_to_str_buf(str, _tb, sizeof(_tb));
        byte_len = strlen(s);
    }

    /* Calculate character length (may differ from byte_len for UTF-8) */
    size_t char_len = 0;
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;
    while (i < byte_len) {
        if (!utf8_is_continuation(p[i])) {
            char_len++;
        }
        i++;
    }

    /* Handle negative offset (count from end) */
    if (offset < 0) offset = (int64_t)char_len + offset;
    if (offset < 0) offset = 0;
    if (offset >= (int64_t)char_len) {
        return strada_new_str("");
    }

    /* Handle length */
    if (length < 0 || offset + length > (int64_t)char_len) {
        length = char_len - offset;
    }

    /* Find byte positions using binary-safe UTF-8 scanning */
    size_t start_byte = 0;
    size_t end_byte = 0;
    size_t char_count = 0;
    p = (const unsigned char *)s;
    i = 0;
    while (i < byte_len && char_count < (size_t)(offset + length)) {
        if (!utf8_is_continuation(p[i])) {
            if (char_count == (size_t)offset) {
                start_byte = i;
            }
            char_count++;
            if (char_count == (size_t)(offset + length)) {
                /* Find the start of the next character */
                i++;
                while (i < byte_len && utf8_is_continuation(p[i])) i++;
                end_byte = i;
                break;
            }
        }
        i++;
    }
    if (end_byte == 0 && char_count > 0) {
        end_byte = byte_len;
    }

    /* Extract result */
    size_t result_len = end_byte - start_byte;

    /* Use strada_new_str_len for binary safety */
    return strada_new_str_len(s + start_byte, result_len);
}

/* Byte-level substr - offset and length are in bytes, not characters.
 * Binary-safe: uses struct_size instead of strlen. */
StradaValue* strada_substr_bytes(StradaValue *str, int64_t offset, int64_t length) {
    /* Get source data - use struct_size for binary safety */
    const char *s;
    size_t byte_len;
    char _tb[256];

    if (str && str->type == STRADA_STR && str->value.pv) {
        s = str->value.pv;
        byte_len = str->struct_size;  /* Binary-safe: use struct_size */
    } else {
        s = strada_to_str_buf(str, _tb, sizeof(_tb));
        byte_len = strlen(s);
    }

    /* Bounds checking */
    if (offset < 0) offset = 0;
    if (offset >= (int64_t)byte_len) {
        return strada_new_str("");
    }

    if (length < 0 || offset + length > (int64_t)byte_len) {
        length = byte_len - offset;
    }

    /* Extract bytes - use strada_new_str_len for binary safety */
    return strada_new_str_len(s + offset, (size_t)length);
}

/* Convert byte offset to character offset */
static size_t byte_to_char_offset(const char *s, size_t byte_offset) {
    if (!s) return 0;
    const unsigned char *p = (const unsigned char *)s;
    size_t chars = 0;
    size_t bytes = 0;

    while (*p && bytes < byte_offset) {
        if (!utf8_is_continuation(*p)) {
            chars++;
        }
        p++;
        bytes++;
    }
    return chars;
}

/* Returns character position (not byte position) */
int strada_index(const char *haystack, const char *needle) {
    if (!haystack || !needle) return -1;

    char *pos = strstr(haystack, needle);
    if (pos) {
        /* Convert byte offset to character offset */
        return (int)byte_to_char_offset(haystack, pos - haystack);
    }
    return -1;
}

int strada_index_offset(const char *haystack, const char *needle, int offset) {
    if (!haystack || !needle) return -1;
    if (offset < 0) offset = 0;

    /* Convert character offset to byte offset */
    size_t byte_off = utf8_offset(haystack, (size_t)offset);
    size_t haystack_len = strlen(haystack);
    if (byte_off >= haystack_len) return -1;

    char *pos = strstr(haystack + byte_off, needle);
    if (pos) {
        /* Convert byte offset to character offset */
        return (int)byte_to_char_offset(haystack, pos - haystack);
    }
    return -1;
}

/* ===== I/O FUNCTIONS ===== */

void strada_print(StradaValue *sv) {
    if (__builtin_expect(strada_default_output != NULL, 0)) {
        strada_print_fh(sv, strada_default_output);
        return;
    }
    char _tb[256];
    const char *str = strada_to_str_buf(sv, _tb, sizeof(_tb));
    printf("%s", str);
    fflush(stdout);
}

void strada_say(StradaValue *sv) {
    if (__builtin_expect(strada_default_output != NULL, 0)) {
        strada_say_fh(sv, strada_default_output);
        return;
    }
    char _tb[256];
    const char *str = strada_to_str_buf(sv, _tb, sizeof(_tb));
    printf("%s\n", str);
    fflush(stdout);
}

/* Helper: buffered write to socket */
static void socket_buffered_write(StradaSocketBuffer *sb, const char *data, size_t len) {
    while (len > 0) {
        size_t space = STRADA_SOCKET_BUFSIZE - sb->write_len;
        if (space == 0) {
            /* Buffer full, flush it */
            send(sb->fd, sb->write_buf, sb->write_len, 0);
            sb->write_len = 0;
            space = STRADA_SOCKET_BUFSIZE;
        }
        size_t to_copy = (len < space) ? len : space;
        memcpy(sb->write_buf + sb->write_len, data, to_copy);
        sb->write_len += to_copy;
        data += to_copy;
        len -= to_copy;
    }
}

/* Print to filehandle or socket (no newline) */
void strada_print_fh(StradaValue *sv, StradaValue *fh) {
    if (!fh) return;

    char _tb[256];
    const char *str = strada_to_str_buf(sv, _tb, sizeof(_tb));
    size_t len = strlen(str);

    if (fh->type == STRADA_FILEHANDLE && fh->value.fh) {
        fputs(str, fh->value.fh);
    } else if (fh->type == STRADA_SOCKET && fh->value.sock) {
        socket_buffered_write(fh->value.sock, str, len);
        /* Flush if data ends with newline (line-buffered behavior) */
        if (len > 0 && str[len - 1] == '\n') {
            StradaSocketBuffer *sb = fh->value.sock;
            if (sb->write_len > 0) {
                send(sb->fd, sb->write_buf, sb->write_len, 0);
                sb->write_len = 0;
            }
        }
    }
}

/* Say to filehandle or socket (with newline) */
void strada_say_fh(StradaValue *sv, StradaValue *fh) {
    if (!fh) return;

    char _tb[256];
    const char *str = strada_to_str_buf(sv, _tb, sizeof(_tb));
    size_t len = strlen(str);

    if (fh->type == STRADA_FILEHANDLE && fh->value.fh) {
        fputs(str, fh->value.fh);
        fputs("\n", fh->value.fh);
        fflush(fh->value.fh);
    } else if (fh->type == STRADA_SOCKET && fh->value.sock) {
        socket_buffered_write(fh->value.sock, str, len);
        socket_buffered_write(fh->value.sock, "\n", 1);
        /* Flush on newline for line-buffered behavior */
        StradaSocketBuffer *sb = fh->value.sock;
        if (sb->write_len > 0) {
            send(sb->fd, sb->write_buf, sb->write_len, 0);
            sb->write_len = 0;
        }
    }
}

StradaValue* strada_readline(void) {
    char buffer[4096];
    if (fgets(buffer, sizeof(buffer), stdin)) {
        /* Remove trailing newline */
        size_t len = strlen(buffer);
        if (len > 0 && buffer[len-1] == '\n') {
            buffer[len-1] = '\0';
        }
        return strada_new_str(buffer);
    }
    return strada_new_undef();
}

void strada_printf(const char *format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
}

StradaValue* strada_sprintf(const char *format, ...) {
    char buffer[4096];
    va_list args;
    va_start(args, format);
    vsnprintf(buffer, sizeof(buffer), format, args);
    va_end(args);
    return strada_new_str(buffer);
}

/* sprintf_sv - sprintf with StradaValue* arguments
 * Parses format string and converts each arg based on format specifier */
StradaValue* strada_sprintf_sv(StradaValue *format_sv, int arg_count, ...) {
    char _tb[256];
    const char *format = format_sv ? strada_to_str_buf(format_sv, _tb, sizeof(_tb)) : NULL;
    if (!format) return strada_new_str("");

    char buffer[8192];
    char *out = buffer;
    char *end = buffer + sizeof(buffer) - 1;
    const char *p = format;

    va_list args;
    va_start(args, arg_count);

    while (*p && out < end) {
        if (*p != '%') {
            *out++ = *p++;
            continue;
        }

        /* Found %, look for format specifier */
        const char *spec_start = p;
        p++; /* skip % */

        /* Handle %% */
        if (*p == '%') {
            *out++ = '%';
            p++;
            continue;
        }

        /* Skip flags: -, +, space, #, 0 */
        while (*p == '-' || *p == '+' || *p == ' ' || *p == '#' || *p == '0') {
            p++;
        }

        /* Skip width */
        while (*p >= '0' && *p <= '9') {
            p++;
        }

        /* Skip precision */
        if (*p == '.') {
            p++;
            while (*p >= '0' && *p <= '9') {
                p++;
            }
        }

        /* Skip length modifiers: h, hh, l, ll, L, z, j, t */
        if (*p == 'h' || *p == 'l' || *p == 'L' || *p == 'z' || *p == 'j' || *p == 't') {
            if (*p == 'h' && *(p+1) == 'h') p += 2;
            else if (*p == 'l' && *(p+1) == 'l') p += 2;
            else p++;
        }

        /* Get the conversion specifier */
        char spec = *p;
        if (!spec) break;
        p++;

        /* Get next argument if we have one */
        StradaValue *arg = NULL;
        if (arg_count > 0) {
            arg = va_arg(args, StradaValue*);
            arg_count--;
        }

        /* Copy format specifier to temp buffer */
        size_t spec_len = p - spec_start;
        char spec_buf[64];
        if (spec_len < sizeof(spec_buf)) {
            memcpy(spec_buf, spec_start, spec_len);
            spec_buf[spec_len] = '\0';
        } else {
            strcpy(spec_buf, "%s");
            spec = 's';
        }

        /* Format based on specifier */
        char temp[1024];
        switch (spec) {
            case 'd':
            case 'i':
            case 'o':
            case 'u':
            case 'x':
            case 'X': {
                int64_t val = arg ? strada_to_int(arg) : 0;
                /* Build format string preserving flags/width but using lld */
                char int_fmt[64] = "%";
                char *fp = int_fmt + 1;
                const char *sp = spec_start + 1;  /* skip % */
                /* Copy flags */
                while (*sp == '-' || *sp == '+' || *sp == ' ' || *sp == '#' || *sp == '0') {
                    *fp++ = *sp++;
                }
                /* Copy width */
                while (*sp >= '0' && *sp <= '9') {
                    *fp++ = *sp++;
                }
                /* Copy precision */
                if (*sp == '.') {
                    *fp++ = *sp++;
                    while (*sp >= '0' && *sp <= '9') {
                        *fp++ = *sp++;
                    }
                }
                /* Add ll modifier and specifier */
                *fp++ = 'l';
                *fp++ = 'l';
                *fp++ = spec;
                *fp = '\0';
                snprintf(temp, sizeof(temp), int_fmt, (long long)val);
                break;
            }
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G':
            case 'a':
            case 'A': {
                double val = arg ? strada_to_num(arg) : 0.0;
                snprintf(temp, sizeof(temp), spec_buf, val);
                break;
            }
            case 'c': {
                int val = arg ? (int)strada_to_int(arg) : 0;
                snprintf(temp, sizeof(temp), "%c", val);
                break;
            }
            case 's': {
                char _tb2[256];
                const char *s = arg ? strada_to_str_buf(arg, _tb2, sizeof(_tb2)) : NULL;
                snprintf(temp, sizeof(temp), "%s", s ? s : "");
                break;
            }
            case 'p': {
                void *ptr = (void*)arg;
                snprintf(temp, sizeof(temp), "%p", ptr);
                break;
            }
            default:
                /* Unknown specifier, copy as-is */
                snprintf(temp, sizeof(temp), "%s", spec_buf);
                break;
        }

        /* Append formatted value to output */
        size_t len = strlen(temp);
        if (out + len < end) {
            memcpy(out, temp, len);
            out += len;
        }
    }

    *out = '\0';
    va_end(args);

    return strada_new_str(buffer);
}

/* ===== FILE I/O FUNCTIONS ===== */

StradaValue* strada_open(const char *filename, const char *mode) {
    FILE *fh = fopen(filename, mode);
    if (!fh) {
        return strada_new_undef();
    }

    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_FILEHANDLE;
    sv->refcount = 1;
    sv->value.fh = fh;
    return sv;
}

StradaValue* strada_open_str(const char *content, const char *mode) {
    if (!mode) return strada_new_undef();

    if (mode[0] == 'r') {
        /* Read mode: copy content into buffer, fmemopen for reading */
        size_t len = content ? strlen(content) : 0;
        char *buf = malloc(len + 1);
        if (content) memcpy(buf, content, len);
        buf[len] = '\0';
        FILE *fh = fmemopen(buf, len, "r");
        if (!fh) { free(buf); return strada_new_undef(); }
        StradaFhMeta *meta = fh_meta_add(fh, FH_MEMREAD);
        meta->mem_buf = buf;
        return strada_new_filehandle(fh);
    } else if (mode[0] == 'w' || mode[0] == 'a') {
        /* Write/append mode: open_memstream for dynamic buffer */
        StradaFhMeta *meta = fh_meta_add(NULL, FH_MEMWRITE);
        FILE *fh = open_memstream(&meta->mem_buf, &meta->mem_size);
        if (!fh) { fh_meta_remove(NULL); return strada_new_undef(); }
        meta->fh = fh;
        if (mode[0] == 'a' && content && content[0]) {
            fputs(content, fh);
            fflush(fh);
        }
        return strada_new_filehandle(fh);
    }
    return strada_new_undef();
}

StradaValue* strada_open_sv(StradaValue *first_arg, StradaValue *mode_arg) {
    if (!first_arg || !mode_arg) return strada_new_undef();

    if (first_arg->type == STRADA_REF) {
        /* Reference-style open: core::open(\$var, "r"/"w"/"a") */
        StradaValue *referent = first_arg->value.rv;
        char _tb[16];
        const char *mode = strada_to_str_buf(mode_arg, _tb, sizeof(_tb));

        if (mode[0] == 'r') {
            /* Read from referenced string */
            char _tb2[256];
            const char *content = strada_to_str_buf(referent, _tb2, sizeof(_tb2));
            size_t len = strlen(content);
            char *buf = malloc(len + 1);
            memcpy(buf, content, len);
            buf[len] = '\0';
            FILE *fh = fmemopen(buf, len, "r");
            if (!fh) { free(buf); return strada_new_undef(); }
            StradaFhMeta *meta = fh_meta_add(fh, FH_MEMREAD);
            meta->mem_buf = buf;
            return strada_new_filehandle(fh);
        } else if (mode[0] == 'w' || mode[0] == 'a') {
            /* Write to referenced string — writeback on close */
            StradaFhMeta *meta = fh_meta_add(NULL, FH_MEMWRITE_REF);
            FILE *fh = open_memstream(&meta->mem_buf, &meta->mem_size);
            if (!fh) { fh_meta_remove(NULL); return strada_new_undef(); }
            meta->fh = fh;
            meta->target_ref = first_arg;
            strada_incref(first_arg);  /* Keep ref alive until close */
            if (mode[0] == 'a') {
                /* Append: pre-write existing content */
                char _tb3[256];
                const char *existing = strada_to_str_buf(referent, _tb3, sizeof(_tb3));
                fputs(existing, fh);
                fflush(fh);
            }
            return strada_new_filehandle(fh);
        }
        return strada_new_undef();
    }

    /* Not a reference — fall through to regular file open */
    char _tb[PATH_MAX];
    const char *filename = strada_to_str_buf(first_arg, _tb, sizeof(_tb));
    char _tb2[16];
    const char *mode = strada_to_str_buf(mode_arg, _tb2, sizeof(_tb2));
    return strada_open(filename, mode);
}

StradaValue* strada_str_from_fh(StradaValue *fh_sv) {
    if (!fh_sv || fh_sv->type != STRADA_FILEHANDLE || !fh_sv->value.fh) {
        return strada_new_str("");
    }
    StradaFhMeta *meta = fh_meta_find(fh_sv->value.fh);
    if (!meta || (meta->fh_type != FH_MEMWRITE && meta->fh_type != FH_MEMWRITE_REF)) {
        return strada_new_str("");
    }
    fflush(fh_sv->value.fh);
    return strada_new_str_len(meta->mem_buf, meta->mem_size);
}

void strada_close(StradaValue *sv) {
    if (sv && sv->type == STRADA_FILEHANDLE && sv->value.fh) {
        strada_close_fh_meta(sv->value.fh);
        sv->value.fh = NULL;
    }
}

StradaValue* strada_read_file(StradaValue *fh) {
    if (!fh || fh->type != STRADA_FILEHANDLE || !fh->value.fh) {
        return strada_new_undef();
    }
    
    /* Read entire file */
    fseek(fh->value.fh, 0, SEEK_END);
    long size = ftell(fh->value.fh);
    fseek(fh->value.fh, 0, SEEK_SET);
    
    if (size < 0) {
        return strada_new_undef();
    }
    
    char *content = malloc(size + 1);
    size_t read_size = fread(content, 1, size, fh->value.fh);
    content[read_size] = '\0';
    
    StradaValue *result = strada_new_str(content);
    free(content);
    
    return result;
}

StradaValue* strada_read_line(StradaValue *fh) {
    if (!fh) {
        return strada_new_undef();
    }

    /* Handle filehandle (FILE*) */
    if (fh->type == STRADA_FILEHANDLE && fh->value.fh) {
        char buffer[4096];
        if (fgets(buffer, sizeof(buffer), fh->value.fh)) {
            /* Remove trailing newline */
            size_t len = strlen(buffer);
            if (len > 0 && buffer[len-1] == '\n') {
                buffer[len-1] = '\0';
            }
            return strada_new_str(buffer);
        }
        return strada_new_undef();
    }

    /* Handle socket - buffered reading */
    if (fh->type == STRADA_SOCKET && fh->value.sock) {
        StradaSocketBuffer *sb = fh->value.sock;
        char line[4096];
        size_t line_pos = 0;

        while (line_pos < sizeof(line) - 1) {
            /* Refill buffer if empty */
            if (sb->read_pos >= sb->read_len) {
                ssize_t n = recv(sb->fd, sb->read_buf, STRADA_SOCKET_BUFSIZE, 0);
                if (n <= 0) {
                    /* EOF or error */
                    if (line_pos == 0) return strada_new_undef();
                    break;
                }
                sb->read_pos = 0;
                sb->read_len = (size_t)n;
            }

            /* Get next character from buffer */
            char c = sb->read_buf[sb->read_pos++];

            if (c == '\n') {
                break;  /* Don't include newline */
            }
            if (c != '\r') {  /* Skip carriage return */
                line[line_pos++] = c;
            }
        }
        line[line_pos] = '\0';
        return strada_new_str(line);
    }

    return strada_new_undef();
}

/* Read all lines from filehandle or socket into array (list context) */
StradaValue* strada_read_all_lines(StradaValue *fh) {
    StradaValue *arr = strada_new_array();

    if (!fh) {
        return arr;
    }

    /* Handle filehandle (FILE*) */
    if (fh->type == STRADA_FILEHANDLE && fh->value.fh) {
        char buffer[4096];
        while (fgets(buffer, sizeof(buffer), fh->value.fh)) {
            /* Remove trailing newline */
            size_t len = strlen(buffer);
            if (len > 0 && buffer[len-1] == '\n') {
                buffer[len-1] = '\0';
            }
            strada_array_push_take(arr->value.av, strada_new_str(buffer));
        }
        return arr;
    }

    /* Handle socket - buffered reading */
    if (fh->type == STRADA_SOCKET && fh->value.sock) {
        StradaSocketBuffer *sb = fh->value.sock;

        while (1) {
            char line[4096];
            size_t line_pos = 0;
            int got_line = 0;

            while (line_pos < sizeof(line) - 1) {
                /* Refill buffer if empty */
                if (sb->read_pos >= sb->read_len) {
                    ssize_t n = recv(sb->fd, sb->read_buf, STRADA_SOCKET_BUFSIZE, 0);
                    if (n <= 0) {
                        /* EOF or error */
                        if (line_pos > 0) {
                            /* Return what we have */
                            line[line_pos] = '\0';
                            strada_array_push_take(arr->value.av, strada_new_str(line));
                        }
                        return arr;
                    }
                    sb->read_pos = 0;
                    sb->read_len = (size_t)n;
                }

                /* Get next character from buffer */
                char c = sb->read_buf[sb->read_pos++];

                if (c == '\n') {
                    got_line = 1;
                    break;  /* Don't include newline */
                }
                if (c != '\r') {  /* Skip carriage return */
                    line[line_pos++] = c;
                }
            }

            line[line_pos] = '\0';
            strada_array_push_take(arr->value.av, strada_new_str(line));

            if (!got_line) {
                /* Line too long or EOF */
                break;
            }
        }
        return arr;
    }

    return arr;
}

void strada_write_file(StradaValue *fh, const char *content) {
    if (fh && fh->type == STRADA_FILEHANDLE && fh->value.fh && content) {
        fputs(content, fh->value.fh);
    }
}

int strada_file_exists(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (f) {
        fclose(f);
        return 1;
    }
    return 0;
}

StradaValue* strada_slurp(const char *filename) {
    FILE *f = fopen(filename, "r");
    if (!f) {
        return strada_new_undef();
    }

    fseek(f, 0, SEEK_END);
    long size = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (size < 0) {
        fclose(f);
        return strada_new_undef();
    }

    char *content = malloc(size + 1);
    size_t read_size = fread(content, 1, size, f);
    content[read_size] = '\0';

    fclose(f);

    StradaValue *result = strada_new_str(content);
    free(content);

    return result;
}

/* Slurp from an open FILE handle (reads from current position to end) */
StradaValue* strada_slurp_fh(StradaValue *fh_sv) {
    if (!fh_sv || fh_sv->type != STRADA_FILEHANDLE || !fh_sv->value.fh) {
        return strada_new_undef();
    }

    FILE *f = fh_sv->value.fh;
    long start_pos = ftell(f);

    fseek(f, 0, SEEK_END);
    long end_pos = ftell(f);
    fseek(f, start_pos, SEEK_SET);

    long size = end_pos - start_pos;
    if (size < 0) {
        return strada_new_undef();
    }

    char *content = malloc(size + 1);
    size_t read_size = fread(content, 1, size, f);
    content[read_size] = '\0';

    StradaValue *result = strada_new_str(content);
    free(content);

    return result;
}

/* Slurp from a raw file descriptor (reads all available data) */
StradaValue* strada_slurp_fd(StradaValue *fd_sv) {
    if (!fd_sv) {
        return strada_new_undef();
    }

    int fd = (int)strada_to_int(fd_sv);
    if (fd < 0) {
        return strada_new_undef();
    }

    /* Use dynamic buffer since we can't easily get size from fd */
    size_t buf_size = 4096;
    size_t total_read = 0;
    char *content = malloc(buf_size);

    while (1) {
        if (total_read + 1024 > buf_size) {
            buf_size *= 2;
            content = realloc(content, buf_size);
        }

        ssize_t n = read(fd, content + total_read, 1024);
        if (n <= 0) {
            break;
        }
        total_read += n;
    }

    content[total_read] = '\0';

    StradaValue *result = strada_new_str(content);
    free(content);

    return result;
}

void strada_spew(const char *filename, const char *content) {
    FILE *f = fopen(filename, "w");
    if (!f) return;
    if (content) {
        fputs(content, f);
    }
    fclose(f);
}

/* Write content to an open FILE handle */
void strada_spew_fh(StradaValue *fh_sv, StradaValue *content_sv) {
    if (!fh_sv || fh_sv->type != STRADA_FILEHANDLE || !fh_sv->value.fh) {
        return;
    }
    if (!content_sv) {
        return;
    }

    FILE *f = fh_sv->value.fh;
    char _tb[256];
    const char *content = strada_to_str_buf(content_sv, _tb, sizeof(_tb));
    fputs(content, f);
}

/* Write content to a raw file descriptor */
void strada_spew_fd(StradaValue *fd_sv, StradaValue *content_sv) {
    if (!fd_sv || !content_sv) {
        return;
    }

    int fd = (int)strada_to_int(fd_sv);
    if (fd < 0) {
        return;
    }

    char _tb[256];
    const char *content = strada_to_str_buf(content_sv, _tb, sizeof(_tb));
    size_t len = strlen(content);
    size_t written = 0;
    while (written < len) {
        ssize_t n = write(fd, content + written, len - written);
        if (n <= 0) break;  // Error or EOF
        written += n;
    }
}

/* ===== BUILT-IN FUNCTIONS ===== */

void strada_dump(StradaValue *sv, int indent) {
    if (!sv) {
        printf("NULL");
        return;
    }
    
    char *ind = malloc(indent * 2 + 1);
    memset(ind, ' ', indent * 2);
    ind[indent * 2] = '\0';
    
    switch (sv->type) {
        case STRADA_UNDEF:
            printf("undef");
            break;
            
        case STRADA_INT:
            printf("%lld", (long long)sv->value.iv);
            break;
            
        case STRADA_NUM:
            printf("%g", sv->value.nv);
            break;
            
        case STRADA_STR:
            printf("\"%s\"", sv->value.pv ? sv->value.pv : "");
            break;
            
        case STRADA_ARRAY: {
            StradaArray *av = sv->value.av;
            if (av->size == 0) {
                printf("[]");
            } else {
                printf("[\n");
                for (size_t i = 0; i < av->size; i++) {
                    printf("%s  ", ind);
                    strada_dump(av->elements[av->head + i], indent + 1);
                    if (i < av->size - 1) printf(",");
                    printf("\n");
                }
                printf("%s]", ind);
            }
            break;
        }

        case STRADA_HASH: {
            StradaHash *hv = sv->value.hv;
            /* Check if hash is empty */
            int is_empty = 1;
            for (size_t i = 0; i < hv->num_buckets && is_empty; i++) {
                if (hv->buckets[i]) is_empty = 0;
            }
            if (is_empty) {
                printf("{}");
            } else {
                printf("{\n");
                for (size_t i = 0; i < hv->num_buckets; i++) {
                    StradaHashEntry *entry = hv->buckets[i];
                    while (entry) {
                        printf("%s  '%s' => ", ind, entry->key);
                        strada_dump(entry->value, indent + 1);
                        printf(",\n");
                        entry = entry->next;
                    }
                }
                printf("%s}", ind);
            }
            break;
        }

        case STRADA_REF: {
            StradaValue *target = sv->value.rv;
            if (!target) {
                printf("\\undef");
            } else if (SV_BLESSED(sv)) {
                printf("bless(");
                strada_dump(target, indent);
                printf(", '%s')", SV_BLESSED(sv));
            } else {
                printf("\\");
                strada_dump(target, indent);
            }
            break;
        }

        case STRADA_FILEHANDLE:
            if (sv->value.fh) {
                printf("FILEHANDLE(fd=%d)", fileno(sv->value.fh));
            } else {
                printf("FILEHANDLE(closed)");
            }
            break;

        case STRADA_SOCKET:
            printf("SOCKET(fd=%d)", sv->value.sock ? sv->value.sock->fd : -1);
            break;

        case STRADA_REGEX:
            printf("REGEX(%p)", sv->value.ptr);
            break;

        case STRADA_CSTRUCT:
            if (SV_STRUCT_NAME(sv)) {
                printf("CSTRUCT(%s, %p)", SV_STRUCT_NAME(sv), sv->value.ptr);
            } else {
                printf("CSTRUCT(%p)", sv->value.ptr);
            }
            break;

        case STRADA_CPOINTER:
            printf("CPOINTER(%p)", sv->value.ptr);
            break;

        case STRADA_CLOSURE:
            printf("CLOSURE(%p)", sv->value.ptr);
            break;

        default:
            printf("UNKNOWN(type=%d)", sv->type);
            break;
    }
    
    free(ind);
}

void strada_dumper(StradaValue *sv) {
    printf("$VAR = ");
    strada_dump(sv, 0);
    printf(";\n");
}

/* String-building version of dump */
static void strada_dump_to_buf(StradaValue *sv, int indent, char **buf, size_t *len, size_t *cap) {
    // Helper macro to append to buffer
    #define APPEND(...) do { \
        size_t need = snprintf(NULL, 0, __VA_ARGS__); \
        while (*len + need + 1 > *cap) { \
            *cap = *cap * 2; \
            *buf = realloc(*buf, *cap); \
        } \
        *len += snprintf(*buf + *len, *cap - *len, __VA_ARGS__); \
    } while(0)

    if (!sv) {
        APPEND("NULL");
        return;
    }

    char *ind = malloc(indent * 2 + 1);
    memset(ind, ' ', indent * 2);
    ind[indent * 2] = '\0';

    switch (sv->type) {
        case STRADA_UNDEF:
            APPEND("undef");
            break;

        case STRADA_INT:
            APPEND("%lld", (long long)sv->value.iv);
            break;

        case STRADA_NUM:
            APPEND("%g", sv->value.nv);
            break;

        case STRADA_STR:
            APPEND("\"%s\"", sv->value.pv ? sv->value.pv : "");
            break;

        case STRADA_ARRAY: {
            StradaArray *av = sv->value.av;
            if (av->size == 0) {
                APPEND("[]");
            } else {
                APPEND("[\n");
                for (size_t i = 0; i < av->size; i++) {
                    APPEND("%s  ", ind);
                    strada_dump_to_buf(av->elements[av->head + i], indent + 1, buf, len, cap);
                    if (i < av->size - 1) APPEND(",");
                    APPEND("\n");
                }
                APPEND("%s]", ind);
            }
            break;
        }

        case STRADA_HASH: {
            StradaHash *hv = sv->value.hv;
            /* Check if hash is empty */
            int is_empty = 1;
            for (size_t i = 0; i < hv->num_buckets && is_empty; i++) {
                if (hv->buckets[i]) is_empty = 0;
            }
            if (is_empty) {
                APPEND("{}");
            } else {
                APPEND("{\n");
                for (size_t i = 0; i < hv->num_buckets; i++) {
                    StradaHashEntry *entry = hv->buckets[i];
                    while (entry) {
                        APPEND("%s  '%s' => ", ind, entry->key);
                        strada_dump_to_buf(entry->value, indent + 1, buf, len, cap);
                        APPEND(",\n");
                        entry = entry->next;
                    }
                }
                APPEND("%s}", ind);
            }
            break;
        }

        case STRADA_REF: {
            StradaValue *target = sv->value.rv;
            if (!target) {
                APPEND("\\undef");
            } else if (SV_BLESSED(sv)) {
                APPEND("bless(");
                strada_dump_to_buf(target, indent, buf, len, cap);
                APPEND(", '%s')", SV_BLESSED(sv));
            } else {
                APPEND("\\");
                strada_dump_to_buf(target, indent, buf, len, cap);
            }
            break;
        }

        case STRADA_FILEHANDLE:
            if (sv->value.fh) {
                APPEND("FILEHANDLE(fd=%d)", fileno(sv->value.fh));
            } else {
                APPEND("FILEHANDLE(closed)");
            }
            break;

        case STRADA_SOCKET:
            APPEND("SOCKET(fd=%d)", sv->value.sock ? sv->value.sock->fd : -1);
            break;

        case STRADA_REGEX:
            APPEND("REGEX(%p)", sv->value.ptr);
            break;

        case STRADA_CSTRUCT:
            if (SV_STRUCT_NAME(sv)) {
                APPEND("CSTRUCT(%s, %p)", SV_STRUCT_NAME(sv), sv->value.ptr);
            } else {
                APPEND("CSTRUCT(%p)", sv->value.ptr);
            }
            break;

        case STRADA_CPOINTER:
            APPEND("CPOINTER(%p)", sv->value.ptr);
            break;

        case STRADA_CLOSURE:
            APPEND("CLOSURE(%p)", sv->value.ptr);
            break;

        default:
            APPEND("UNKNOWN(type=%d)", sv->type);
            break;
    }

    free(ind);
    #undef APPEND
}

StradaValue* strada_dumper_str(StradaValue *sv) {
    size_t cap = 256;
    size_t len = 0;
    char *buf = malloc(cap);
    buf[0] = '\0';

    strada_dump_to_buf(sv, 0, &buf, &len, &cap);

    StradaValue *result = strada_new_str(buf);
    free(buf);
    return result;
}

StradaValue* strada_defined(StradaValue *sv) {
    return strada_new_int(sv && sv->type != STRADA_UNDEF);
}

/* Non-allocating version - returns int directly, caller handles cleanup */
int strada_defined_bool(StradaValue *sv) {
    return sv && sv->type != STRADA_UNDEF;
}

StradaValue* strada_ref(StradaValue *sv) {
    if (!sv) return strada_new_str("");

    /* If blessed, return the blessed package name (like Perl) */
    const char *bp = SV_BLESSED(sv);
    if (bp) {
        return strada_new_str(bp);
    }

    switch (sv->type) {
        case STRADA_ARRAY:
            return strada_new_str("ARRAY");
        case STRADA_HASH:
            return strada_new_str("HASH");
        case STRADA_REF:
            return strada_new_str("REF");
        default:
            return strada_new_str("");
    }
}

/* ===== UTILITY FUNCTIONS ===== */

/* Exception handling globals */
StradaTryContext strada_try_stack[STRADA_MAX_TRY_DEPTH];
int strada_try_depth = 0;
char *strada_exception_msg = NULL;
StradaValue *strada_exception_value = NULL;  /* Typed exception support */

/* Call stack for stack traces */
StradaStackFrame strada_call_stack[STRADA_MAX_CALL_DEPTH];
int strada_call_depth = 0;
int strada_recursion_limit = 1000;  /* Default limit; 0 = disabled */

void strada_set_recursion_limit(int limit) {
    strada_recursion_limit = limit;
}

int strada_get_recursion_limit(void) {
    return strada_recursion_limit;
}

/* Dynamic return type (wantarray) support */
#ifdef STRADA_NO_TLS
int strada_call_context = 0;
#else
__thread int strada_call_context = 0;
#endif

void strada_set_call_context(int ctx) {
    strada_call_context = ctx;
}

int strada_wantarray(void) {
    return strada_call_context == 1;
}

int strada_wantscalar(void) {
    return strada_call_context == 0;
}

int strada_wanthash(void) {
    return strada_call_context == 2;
}

void strada_stack_push(const char *func_name, const char *file_name) {
    /* Check recursion limit before pushing */
    if (strada_recursion_limit > 0 && strada_call_depth >= strada_recursion_limit) {
        fprintf(stderr, "Error: Maximum recursion depth exceeded (%d)\n", strada_recursion_limit);
        strada_print_stack_trace(stderr);
        fprintf(stderr, "  -> %s (%s)\n", func_name, file_name);
        fprintf(stderr, "\nHint: Use sys::set_recursion_limit(n) to increase the limit, or 0 to disable.\n");
        exit(1);
    }

    if (strada_call_depth < STRADA_MAX_CALL_DEPTH) {
        strada_call_stack[strada_call_depth].func_name = func_name;
        strada_call_stack[strada_call_depth].file_name = file_name;
        strada_call_stack[strada_call_depth].line = 0;
        strada_call_depth++;
    }
}

void strada_stack_pop(void) {
    if (strada_call_depth > 0) {
        strada_call_depth--;
    }
}

void strada_stack_set_line(int line) {
    if (strada_call_depth > 0) {
        strada_call_stack[strada_call_depth - 1].line = line;
    }
}

void strada_print_stack_trace(FILE *out) {
    if (strada_call_depth == 0) {
        return;
    }
    fprintf(out, "Stack trace:\n");
    for (int i = strada_call_depth - 1; i >= 0; i--) {
        StradaStackFrame *frame = &strada_call_stack[i];
        if (frame->line > 0) {
            fprintf(out, "  at %s (%s:%d)\n",
                    frame->func_name ? frame->func_name : "?",
                    frame->file_name ? frame->file_name : "?",
                    frame->line);
        } else {
            fprintf(out, "  at %s (%s)\n",
                    frame->func_name ? frame->func_name : "?",
                    frame->file_name ? frame->file_name : "?");
        }
    }
}

char* strada_capture_stack_trace(void) {
    if (strada_call_depth == 0) {
        return strdup("");
    }
    /* Estimate buffer size */
    size_t bufsize = strada_call_depth * 256;
    char *buf = malloc(bufsize);
    if (!buf) return strdup("");
    buf[0] = '\0';
    size_t pos = 0;

    for (int i = strada_call_depth - 1; i >= 0 && pos < bufsize - 1; i--) {
        StradaStackFrame *frame = &strada_call_stack[i];
        int written;
        if (frame->line > 0) {
            written = snprintf(buf + pos, bufsize - pos, "  at %s (%s:%d)\n",
                    frame->func_name ? frame->func_name : "?",
                    frame->file_name ? frame->file_name : "?",
                    frame->line);
        } else {
            written = snprintf(buf + pos, bufsize - pos, "  at %s (%s)\n",
                    frame->func_name ? frame->func_name : "?",
                    frame->file_name ? frame->file_name : "?");
        }
        if (written > 0) pos += written;
    }
    return buf;
}

/* Pending cleanup for function call args in try blocks */
#define STRADA_MAX_PENDING_CLEANUP 64
static StradaValue *strada_pending_cleanup[STRADA_MAX_PENDING_CLEANUP];
static int strada_pending_cleanup_count = 0;

void strada_cleanup_push(StradaValue *sv) {
    if (strada_pending_cleanup_count < STRADA_MAX_PENDING_CLEANUP) {
        strada_pending_cleanup[strada_pending_cleanup_count++] = sv;
    }
}

void strada_cleanup_pop(void) {
    if (strada_pending_cleanup_count > 0) {
        strada_pending_cleanup_count--;
    }
}

void strada_cleanup_drain(void) {
    while (strada_pending_cleanup_count > 0) {
        StradaValue *sv = strada_pending_cleanup[--strada_pending_cleanup_count];
        if (sv) strada_decref(sv);
    }
}

/* Get current cleanup stack depth (for saving at try entry) */
int strada_cleanup_mark(void) {
    return strada_pending_cleanup_count;
}

/* Restore cleanup stack to a saved depth (pop without decref, for normal try exit) */
void strada_cleanup_restore(int mark) {
    if (mark >= 0 && mark <= strada_pending_cleanup_count) {
        strada_pending_cleanup_count = mark;
    }
}

/* Drain cleanup stack down to a saved depth (decref and pop, for exception) */
void strada_cleanup_drain_to(int mark) {
    while (strada_pending_cleanup_count > mark) {
        StradaValue *sv = strada_pending_cleanup[--strada_pending_cleanup_count];
        if (sv) strada_decref(sv);
    }
}

int strada_in_try_block(void) {
    return strada_try_depth > 0 && strada_try_stack[strada_try_depth - 1].active;
}

void strada_throw(const char *msg) {
    /* Free previous exception message if any */
    if (strada_exception_msg) {
        free(strada_exception_msg);
    }
    strada_exception_msg = msg ? strdup(msg) : strdup("Unknown error");

    /* Clear any previous exception value */
    if (strada_exception_value) {
        strada_decref(strada_exception_value);
        strada_exception_value = NULL;
    }

    if (strada_in_try_block()) {
        /* Jump to the nearest catch block */
        longjmp(strada_try_stack[strada_try_depth - 1].buf, 1);
    } else {
        /* No try block - fatal error with stack trace */
        fprintf(stderr, "Uncaught exception: %s\n", strada_exception_msg);
        strada_print_stack_trace(stderr);
        exit(1);
    }
}

void strada_throw_value(StradaValue *sv) {
    /* Store the actual exception value for typed catches */
    /* Take ownership of sv directly - caller transfers ownership */
    if (strada_exception_value) {
        strada_decref(strada_exception_value);
    }
    strada_exception_value = sv;
    /* Note: don't incref - we take ownership from caller */

    /* Also store the string message for backward compat and error reporting */
    char *msg = strada_to_str(sv);
    if (strada_exception_msg) {
        free(strada_exception_msg);
    }
    strada_exception_msg = msg;  /* Take ownership directly */

    if (strada_in_try_block()) {
        longjmp(strada_try_stack[strada_try_depth - 1].buf, 1);
    } else {
        fprintf(stderr, "Uncaught exception: %s\n", msg);
        strada_print_stack_trace(stderr);
        exit(1);
    }
}

StradaValue* strada_get_exception(void) {
    /* Return the actual exception value if available (for typed catches) */
    if (strada_exception_value) {
        StradaValue *val = strada_exception_value;
        strada_exception_value = NULL;  /* Consume the value */
        return val;
    }
    /* Fallback to string message for backward compatibility */
    if (strada_exception_msg) {
        return strada_new_str(strada_exception_msg);
    }
    return strada_new_str("");
}

void strada_clear_exception(void) {
    if (strada_exception_msg) {
        free(strada_exception_msg);
        strada_exception_msg = NULL;
    }
    if (strada_exception_value) {
        strada_decref(strada_exception_value);
        strada_exception_value = NULL;
    }
}

void strada_die_sv(StradaValue *msg) {
    char buffer[1024];
    char _tb[256];
    const char *str = strada_to_str_buf(msg, _tb, sizeof(_tb));
    snprintf(buffer, sizeof(buffer), "%s", str ? str : "(null)");

    if (strada_in_try_block()) {
        strada_throw(buffer);
    } else {
        fprintf(stderr, "%s\n", buffer);
        exit(1);
    }
}

void strada_die(const char *format, ...) {
    char buffer[1024];

    /* Check if format is "%s" - if so, get the actual message from varargs */
    /* Otherwise, treat format as the literal message (don't interpret % as format specifiers) */
    if (format && strcmp(format, "%s") == 0) {
        va_list args;
        va_start(args, format);
        const char *msg = va_arg(args, const char *);
        va_end(args);
        snprintf(buffer, sizeof(buffer), "%s", msg ? msg : "(null)");
    } else {
        /* Don't interpret format specifiers - just copy the string directly */
        snprintf(buffer, sizeof(buffer), "%s", format ? format : "(null)");
    }

    /* If we're in a try block, throw instead of dying */
    if (strada_in_try_block()) {
        strada_throw(buffer);
    } else {
        fprintf(stderr, "%s\n", buffer);
        exit(1);
    }
}

void strada_warn(const char *format, ...) {
    /* Check if format is "%s" - if so, get the actual message from varargs */
    /* Otherwise, treat format as the literal message (don't interpret % as format specifiers) */
    if (format && strcmp(format, "%s") == 0) {
        va_list args;
        va_start(args, format);
        const char *msg = va_arg(args, const char *);
        va_end(args);
        fprintf(stderr, "%s\n", msg ? msg : "(null)");
    } else {
        /* Don't interpret format specifiers - just print the string directly */
        fprintf(stderr, "%s\n", format ? format : "(null)");
    }
}

void strada_exit(int code) {
    exit(code);
}

/* ===== MEMORY MANAGEMENT ===== */

void strada_free(StradaValue *sv) {
    /* Explicitly decrement reference count and free if zero */
    strada_decref(sv);
}

StradaValue* strada_release(StradaValue *ref) {
    /* Free the referent and set the reference target to undef.
     * This prevents use-after-free by clearing the variable.
     * Usage: release(\$var) - frees $var and sets it to undef */
    if (ref && ref->type == STRADA_REF && ref->value.rv) {
        strada_decref(ref->value.rv);
        ref->value.rv = strada_new_undef();
    }
    return strada_new_undef();
}

StradaValue* strada_undef(StradaValue *sv) {
    /* Free existing value and set to undef */
    if (sv) {
        strada_decref(sv);
    }
    return strada_new_undef();
}

int strada_refcount(StradaValue *sv) {
    /* Return current reference count, 0 for undef/NULL */
    if (!sv || sv->type == STRADA_UNDEF) return 0;
    return sv->refcount;
}

/* ===== SOCKET FUNCTIONS ===== */

StradaValue* strada_socket_create(void) {
    int sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0) {
        return strada_new_undef();
    }

    // Set socket options to reuse address
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    // Allocate buffered socket structure
    StradaSocketBuffer *buf = malloc(sizeof(StradaSocketBuffer));
    buf->fd = sockfd;
    buf->read_pos = 0;
    buf->read_len = 0;
    buf->write_len = 0;

    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_SOCKET;
    sv->refcount = 1;
    sv->value.sock = buf;

    return sv;
}

int strada_socket_connect(StradaValue *sock, const char *host, int port) {
    if (!sock || sock->type != STRADA_SOCKET || !sock->value.sock) {
        return -1;
    }

    struct sockaddr_in server_addr;
    struct hostent *server;

    server = gethostbyname(host);
    if (server == NULL) {
        return -1;
    }

    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    memcpy(&server_addr.sin_addr.s_addr, server->h_addr_list[0], server->h_length);
    server_addr.sin_port = htons(port);

    return connect(sock->value.sock->fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
}

int strada_socket_bind(StradaValue *sock, int port) {
    if (!sock || sock->type != STRADA_SOCKET || !sock->value.sock) {
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    return bind(sock->value.sock->fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
}

int strada_socket_listen(StradaValue *sock, int backlog) {
    if (!sock || sock->type != STRADA_SOCKET || !sock->value.sock) {
        return -1;
    }

    return listen(sock->value.sock->fd, backlog);
}

StradaValue* strada_socket_accept(StradaValue *sock) {
    if (!sock || sock->type != STRADA_SOCKET || !sock->value.sock) {
        return strada_new_undef();
    }

    struct sockaddr_in client_addr;
    socklen_t client_len = sizeof(client_addr);

    int client_fd = accept(sock->value.sock->fd, (struct sockaddr *)&client_addr, &client_len);
    if (client_fd < 0) {
        return strada_new_undef();
    }

    // Allocate buffered socket structure for client
    StradaSocketBuffer *buf = malloc(sizeof(StradaSocketBuffer));
    buf->fd = client_fd;
    buf->read_pos = 0;
    buf->read_len = 0;
    buf->write_len = 0;

    StradaValue *client = strada_value_alloc();
    client->type = STRADA_SOCKET;
    client->refcount = 1;
    client->value.sock = buf;

    return client;
}

int strada_socket_send(StradaValue *sock, const char *data) {
    if (!sock || sock->type != STRADA_SOCKET || !sock->value.sock || !data) {
        return -1;
    }

    size_t len = strlen(data);
    ssize_t sent = send(sock->value.sock->fd, data, len, 0);

    return (int)sent;
}

/* Binary-safe version: uses struct_size to handle embedded NULLs */
int strada_socket_send_sv(StradaValue *sock, StradaValue *data) {
    if (!sock || sock->type != STRADA_SOCKET || !sock->value.sock || !data) {
        return -1;
    }

    const char *buf;
    size_t len;

    if (data->type == STRADA_STR && data->value.pv) {
        buf = data->value.pv;
        len = data->struct_size > 0 ? data->struct_size : strlen(data->value.pv);
    } else {
        return -1;
    }

    ssize_t sent = send(sock->value.sock->fd, buf, len, 0);
    return (int)sent;
}

StradaValue* strada_socket_recv(StradaValue *sock, int max_len) {
    if (!sock || sock->type != STRADA_SOCKET || !sock->value.sock) {
        return strada_new_undef();
    }

    char *buffer = malloc(max_len + 1);
    ssize_t received = recv(sock->value.sock->fd, buffer, max_len, 0);

    if (received < 0) {
        free(buffer);
        return strada_new_undef();
    }

    buffer[received] = '\0';
    /* Use strada_new_str_len to preserve binary data with embedded NULLs */
    StradaValue *result = strada_new_str_len(buffer, (size_t)received);
    free(buffer);

    return result;
}

/* Flush socket write buffer */
void strada_socket_flush(StradaValue *sock) {
    if (!sock || sock->type != STRADA_SOCKET || !sock->value.sock) {
        return;
    }
    StradaSocketBuffer *buf = sock->value.sock;
    if (buf->write_len > 0) {
        send(buf->fd, buf->write_buf, buf->write_len, 0);
        buf->write_len = 0;
    }
}

void strada_socket_close(StradaValue *sock) {
    if (sock && sock->type == STRADA_SOCKET && sock->value.sock) {
        /* Flush any pending write data */
        strada_socket_flush(sock);
        close(sock->value.sock->fd);
        sock->value.sock->fd = -1;
    }
}

/* High-level socket helpers */

StradaValue* strada_socket_server(int port) {
    return strada_socket_server_backlog(port, 128);
}

StradaValue* strada_socket_server_backlog(int port, int backlog) {
    StradaValue *sock = strada_socket_create();
    if (sock->type == STRADA_UNDEF) {
        return sock;
    }

    if (strada_socket_bind(sock, port) < 0) {
        strada_decref(sock);
        return strada_new_undef();
    }

    if (strada_socket_listen(sock, backlog) < 0) {
        strada_decref(sock);
        return strada_new_undef();
    }

    return sock;
}

StradaValue* strada_socket_client(const char *host, int port) {
    StradaValue *sock = strada_socket_create();
    if (sock->type == STRADA_UNDEF) {
        return sock;
    }

    if (strada_socket_connect(sock, host, port) < 0) {
        strada_decref(sock);
        return strada_new_undef();
    }

    return sock;
}

/* socket_select - wait for sockets to become ready for reading
 * Takes an array of sockets and a timeout in milliseconds
 * Returns an array of sockets that are ready for reading
 * Timeout of -1 means wait forever, 0 means poll without blocking
 */
StradaValue* strada_socket_select(StradaValue *sockets, int timeout_ms) {
    if (!sockets) {
        return strada_new_array();
    }

    /* Handle array reference - dereference if needed */
    StradaValue *arr_val = sockets;
    if (sockets->type == STRADA_REF && sockets->value.rv &&
        sockets->value.rv->type == STRADA_ARRAY) {
        arr_val = sockets->value.rv;
    }

    if (arr_val->type != STRADA_ARRAY) {
        return strada_new_array();
    }

    StradaArray *arr = arr_val->value.av;
    int count = (int)arr->size;

    if (count == 0) {
        return strada_new_array();
    }

    fd_set readfds;
    FD_ZERO(&readfds);

    int maxfd = -1;

    /* Add all socket fds to the set */
    for (int i = 0; i < count; i++) {
        StradaValue *sock = arr->elements[arr->head + i];
        if (sock && sock->type == STRADA_SOCKET && sock->value.sock && sock->value.sock->fd >= 0) {
            FD_SET(sock->value.sock->fd, &readfds);
            if (sock->value.sock->fd > maxfd) {
                maxfd = sock->value.sock->fd;
            }
        }
    }

    if (maxfd < 0) {
        return strada_new_array();
    }

    /* Set up timeout */
    struct timeval tv;
    struct timeval *tvp = NULL;

    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        tvp = &tv;
    }

    /* Call select */
    int result = select(maxfd + 1, &readfds, NULL, NULL, tvp);

    /* Build result array of ready sockets */
    StradaValue *ready = strada_new_array();

    if (result > 0) {
        for (int i = 0; i < count; i++) {
            StradaValue *sock = arr->elements[arr->head + i];
            if (sock && sock->type == STRADA_SOCKET && sock->value.sock && sock->value.sock->fd >= 0) {
                if (FD_ISSET(sock->value.sock->fd, &readfds)) {
                    strada_array_push(ready->value.av, sock);
                }
            }
        }
    }

    return ready;
}

/* socket_fd - get the file descriptor from a socket (for debugging) */
int strada_socket_fd(StradaValue *sock) {
    if (sock && sock->type == STRADA_SOCKET && sock->value.sock) {
        return sock->value.sock->fd;
    }
    return -1;
}

/* socket_set_nonblocking - set or clear non-blocking mode on a socket
 * nonblock: 1 to set non-blocking, 0 to clear it
 * Returns 0 on success, -1 on error
 */
int strada_socket_set_nonblocking(StradaValue *sock, int nonblock) {
    if (!sock || sock->type != STRADA_SOCKET || !sock->value.sock) {
        return -1;
    }
    int fd = sock->value.sock->fd;
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags < 0) {
        return -1;
    }
    if (nonblock) {
        flags |= O_NONBLOCK;
    } else {
        flags &= ~O_NONBLOCK;
    }
    return fcntl(fd, F_SETFL, flags);
}

/* select_fds - wait for file descriptors to become ready for reading
 * Takes an array of integers (fds) and a timeout in milliseconds
 * Returns an array of fds that are ready for reading
 * Timeout of -1 means wait forever, 0 means poll without blocking
 */
StradaValue* strada_select_fds(StradaValue *fds, int timeout_ms) {
    if (!fds) {
        return strada_new_array();
    }

    /* Handle array reference - dereference if needed */
    StradaValue *arr_val = fds;

    /* Keep dereferencing until we get to the actual array */
    int deref_count = 0;
    while (arr_val && arr_val->type == STRADA_REF && arr_val->value.rv && deref_count < 10) {
        arr_val = arr_val->value.rv;
        deref_count++;
    }

    if (!arr_val || arr_val->type != STRADA_ARRAY) {
        return strada_new_array();
    }

    StradaArray *arr = arr_val->value.av;
    int count = (int)arr->size;

    if (count == 0) {
        return strada_new_array();
    }

    fd_set readfds;
    FD_ZERO(&readfds);

    int maxfd = -1;

    /* Add all fds to the set */
    for (int i = 0; i < count; i++) {
        StradaValue *fdval = arr->elements[arr->head + i];
        if (fdval) {
            int fd = (int)strada_to_int(fdval);
            if (fd >= 0) {
                FD_SET(fd, &readfds);
                if (fd > maxfd) {
                    maxfd = fd;
                }
            }
        }
    }

    if (maxfd < 0) {
        return strada_new_array();
    }

    /* Set up timeout */
    struct timeval tv;
    struct timeval *tvp = NULL;

    if (timeout_ms >= 0) {
        tv.tv_sec = timeout_ms / 1000;
        tv.tv_usec = (timeout_ms % 1000) * 1000;
        tvp = &tv;
    }

    /* Call select */
    int result = select(maxfd + 1, &readfds, NULL, NULL, tvp);

    /* Build result array of ready fds */
    StradaValue *ready = strada_new_array();

    if (result > 0) {
        for (int i = 0; i < count; i++) {
            StradaValue *fdval = arr->elements[arr->head + i];
            if (fdval) {
                int fd = (int)strada_to_int(fdval);
                if (fd >= 0 && FD_ISSET(fd, &readfds)) {
                    strada_array_push_take(ready->value.av, strada_new_int(fd));
                }
            }
        }
    }

    return ready;
}

/* ===== UDP SOCKET FUNCTIONS ===== */

/* Create a UDP socket */
StradaValue* strada_udp_socket(void) {
    int sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0) {
        return strada_new_undef();
    }

    /* Set socket options to reuse address */
    int opt = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    /* Allocate buffered socket structure */
    StradaSocketBuffer *buf = malloc(sizeof(StradaSocketBuffer));
    buf->fd = sockfd;
    buf->read_pos = 0;
    buf->read_len = 0;
    buf->write_len = 0;

    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_SOCKET;
    sv->refcount = 1;
    sv->value.sock = buf;

    return sv;
}

/* Bind UDP socket to a port */
int strada_udp_bind(StradaValue *sock, int port) {
    if (!sock || sock->type != STRADA_SOCKET || !sock->value.sock) {
        return -1;
    }

    struct sockaddr_in server_addr;
    memset(&server_addr, 0, sizeof(server_addr));
    server_addr.sin_family = AF_INET;
    server_addr.sin_addr.s_addr = INADDR_ANY;
    server_addr.sin_port = htons(port);

    return bind(sock->value.sock->fd, (struct sockaddr *)&server_addr, sizeof(server_addr));
}

/* Create a UDP server socket bound to a port */
StradaValue* strada_udp_server(int port) {
    StradaValue *sock = strada_udp_socket();
    if (sock->type == STRADA_UNDEF) {
        return sock;
    }

    if (strada_udp_bind(sock, port) < 0) {
        strada_decref(sock);
        return strada_new_undef();
    }

    return sock;
}

/* Receive data from UDP socket
 * Returns a hash with:
 *   data - the received data as a string
 *   ip - sender's IP address as a string
 *   port - sender's port as an integer
 * Or undef on error
 */
StradaValue* strada_udp_recvfrom(StradaValue *sock, int max_len) {
    if (!sock || sock->type != STRADA_SOCKET || !sock->value.sock) {
        return strada_new_undef();
    }

    char *buffer = malloc(max_len + 1);
    struct sockaddr_in sender_addr;
    socklen_t sender_len = sizeof(sender_addr);

    ssize_t received = recvfrom(sock->value.sock->fd, buffer, max_len, 0,
                                 (struct sockaddr *)&sender_addr, &sender_len);

    if (received < 0) {
        free(buffer);
        return strada_new_undef();
    }

    buffer[received] = '\0';

    /* Create result hash with data, ip, and port */
    StradaValue *result = strada_new_hash();

    /* Add data - use strada_new_str_len to preserve binary data */
    strada_hash_set_take(result->value.hv, "data", strada_new_str_len(buffer, (size_t)received));

    /* Add sender IP */
    char ip_str[INET_ADDRSTRLEN];
    inet_ntop(AF_INET, &sender_addr.sin_addr, ip_str, INET_ADDRSTRLEN);
    strada_hash_set_take(result->value.hv, "ip", strada_new_str(ip_str));

    /* Add sender port */
    strada_hash_set_take(result->value.hv, "port", strada_new_int(ntohs(sender_addr.sin_port)));

    free(buffer);
    return result;
}

/* Send data via UDP to a specific host and port
 * Returns number of bytes sent, or -1 on error
 */
int strada_udp_sendto(StradaValue *sock, const char *data, int data_len, const char *host, int port) {
    if (!sock || sock->type != STRADA_SOCKET || !data) {
        return -1;
    }

    struct sockaddr_in dest_addr;
    memset(&dest_addr, 0, sizeof(dest_addr));
    dest_addr.sin_family = AF_INET;
    dest_addr.sin_port = htons(port);

    /* Try to parse as IP address first */
    if (inet_pton(AF_INET, host, &dest_addr.sin_addr) != 1) {
        /* If not an IP, resolve hostname */
        struct hostent *he = gethostbyname(host);
        if (!he) {
            return -1;
        }
        memcpy(&dest_addr.sin_addr, he->h_addr_list[0], he->h_length);
    }

    ssize_t sent = sendto(sock->value.sock->fd, data, data_len, 0,
                          (struct sockaddr *)&dest_addr, sizeof(dest_addr));

    return (int)sent;
}

/* Send StradaValue data via UDP (binary-safe) */
int strada_udp_sendto_sv(StradaValue *sock, StradaValue *data, const char *host, int port) {
    if (!sock || sock->type != STRADA_SOCKET || !data) {
        return -1;
    }

    const char *buf;
    int len;

    if (data->type == STRADA_STR && data->value.pv) {
        buf = data->value.pv;
        len = data->struct_size > 0 ? (int)data->struct_size : (int)strlen(data->value.pv);
    } else {
        return -1;
    }

    return strada_udp_sendto(sock, buf, len, host, port);
}

/* ===== REGEX FUNCTIONS ===== */

#ifdef HAVE_PCRE2

/* ===== PCRE2 IMPLEMENTATION ===== */

/* Function pointer for PCRE2 cleanup, used by strada_free_value().
 * This avoids a direct reference to pcre2_code_free in strada_free_value,
 * which would force the linker to pull in PCRE2 even when the program
 * doesn't use regex (defeating -Wl,--gc-sections). Set on first regex use. */
static void (*strada_pcre2_code_free_fn)(void *) = NULL;

static void strada_pcre2_ensure_cleanup_fn(void) {
    if (!strada_pcre2_code_free_fn) {
        strada_pcre2_code_free_fn = (void (*)(void *))pcre2_code_free;
    }
}

/* Helper: Convert flag string to PCRE2 options */
static uint32_t pcre2_get_options(const char *flags) {
    uint32_t options = 0;
    if (flags) {
        if (strchr(flags, 'i')) options |= PCRE2_CASELESS;
        if (strchr(flags, 'm')) options |= PCRE2_MULTILINE;
        if (strchr(flags, 's')) options |= PCRE2_DOTALL;
        if (strchr(flags, 'x')) options |= PCRE2_EXTENDED;
    }
    return options;
}

/* Global storage for last regex captures */
static StradaValue *last_regex_captures = NULL;
/* Global storage for last named captures (PCRE2 only) */
static StradaValue *last_named_captures = NULL;
static int regex_cleanup_registered = 0;

/* ===== REGEX COMPILATION CACHE (PCRE2) ===== */
/* Direct-mapped cache keyed by DJB2 hash of (pattern + options).
 * Avoids recompiling the same regex on every match. */
#define REGEX_CACHE_SIZE 128
typedef struct {
    char *pattern;
    uint32_t options;
    pcre2_code *compiled;
    uint64_t last_used;
} RegexCacheEntry;

static RegexCacheEntry regex_cache[REGEX_CACHE_SIZE];
static int regex_cache_count = 0;
static uint64_t regex_cache_counter = 0;
static int regex_cache_initialized = 0;

static void regex_cache_init(void) {
    memset(regex_cache, 0, sizeof(regex_cache));
    regex_cache_initialized = 1;
}

static unsigned int regex_cache_hash(const char *pattern, uint32_t options) {
    unsigned int h = 5381;
    for (const char *p = pattern; *p; p++) {
        h = ((h << 5) + h) + (unsigned char)*p;
    }
    h = ((h << 5) + h) + options;
    return h;
}

/* Look up a cached compiled regex. Returns NULL on miss. */
static pcre2_code *regex_cache_lookup(const char *pattern, uint32_t options) {
    if (!regex_cache_initialized) regex_cache_init();
    unsigned int h = regex_cache_hash(pattern, options) & (REGEX_CACHE_SIZE - 1);
    RegexCacheEntry *e = &regex_cache[h];
    if (e->pattern && e->options == options && strcmp(e->pattern, pattern) == 0) {
        e->last_used = ++regex_cache_counter;
        return e->compiled;
    }
    return NULL;
}

/* Insert a compiled regex into the cache. Evicts existing entry at same slot. */
static pcre2_code *regex_cache_insert(const char *pattern, uint32_t options, pcre2_code *compiled) {
    if (!regex_cache_initialized) regex_cache_init();
    unsigned int h = regex_cache_hash(pattern, options) & (REGEX_CACHE_SIZE - 1);
    RegexCacheEntry *e = &regex_cache[h];
    /* Evict existing entry */
    if (e->pattern) {
        free(e->pattern);
        pcre2_code_free(e->compiled);
    } else {
        regex_cache_count++;
    }
    e->pattern = strdup(pattern);
    e->options = options;
    e->compiled = compiled;
    e->last_used = ++regex_cache_counter;
    return compiled;
}

/* Compile regex with caching */
static pcre2_code *regex_cache_compile(const char *pattern, uint32_t options) {
    pcre2_code *re = regex_cache_lookup(pattern, options);
    if (re) return re;

    int errcode;
    PCRE2_SIZE erroffset;
    re = pcre2_compile(
        (PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
        options, &errcode, &erroffset, NULL);
    if (!re) return NULL;

    return regex_cache_insert(pattern, options, re);
}

static void regex_cache_cleanup(void) {
    for (int i = 0; i < REGEX_CACHE_SIZE; i++) {
        if (regex_cache[i].pattern) {
            free(regex_cache[i].pattern);
            pcre2_code_free(regex_cache[i].compiled);
            regex_cache[i].pattern = NULL;
            regex_cache[i].compiled = NULL;
        }
    }
    regex_cache_count = 0;
}

static void regex_cleanup_atexit(void) {
    if (last_regex_captures) { strada_decref(last_regex_captures); last_regex_captures = NULL; }
    if (last_named_captures) { strada_decref(last_named_captures); last_named_captures = NULL; }
    regex_cache_cleanup();
}

StradaValue* strada_regex_compile(const char *pattern, const char *flags) {
    strada_pcre2_ensure_cleanup_fn();
    int errcode;
    PCRE2_SIZE erroffset;
    uint32_t options = pcre2_get_options(flags);

    pcre2_code *re = pcre2_compile(
        (PCRE2_SPTR)pattern, PCRE2_ZERO_TERMINATED,
        options, &errcode, &erroffset, NULL);

    if (!re) {
        return strada_new_undef();
    }

    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_REGEX;
    sv->refcount = 1;
    sv->struct_size = 0;
    sv->value.pcre2_rx = re;
    return sv;
}

int strada_regex_match(const char *str, const char *pattern) {
    pcre2_code *re = regex_cache_compile(pattern, 0);
    if (!re) return 0;

    pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(re, NULL);
    int rc = pcre2_match(re, (PCRE2_SPTR)str, strlen(str), 0, 0, match_data, NULL);

    pcre2_match_data_free(match_data);
    /* re is owned by the cache — do NOT free */

    return (rc >= 0) ? 1 : 0;
}

int strada_regex_match_with_capture(const char *str, const char *pattern, const char *flags) {
    uint32_t options = pcre2_get_options(flags);

    pcre2_code *re = regex_cache_compile(pattern, options);

    if (!re) {
        if (last_regex_captures) { strada_decref(last_regex_captures); last_regex_captures = NULL; }
        if (last_named_captures) { strada_decref(last_named_captures); last_named_captures = NULL; }
        return 0;
    }

    pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(re, NULL);
    PCRE2_SIZE subject_length = strlen(str);
    int rc = pcre2_match(re, (PCRE2_SPTR)str, subject_length, 0, 0, match_data, NULL);

    /* Create numbered captures array */
    StradaArray *captures = strada_array_new();

    /* Create named captures hash */
    StradaHash *named_hash = strada_hash_new();

    if (rc >= 0) {
        PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
        uint32_t count = pcre2_get_ovector_count(match_data);

        for (uint32_t i = 0; i < count; i++) {
            if (ovector[2*i] != PCRE2_UNSET) {
                PCRE2_SIZE start = ovector[2*i];
                PCRE2_SIZE end = ovector[2*i + 1];
                size_t len = end - start;
                char *capture = malloc(len + 1);
                memcpy(capture, str + start, len);
                capture[len] = '\0';
                strada_array_push_take(captures, strada_new_str(capture));
                free(capture);
            } else {
                strada_array_push_take(captures, strada_new_undef());
            }
        }

        /* Extract named captures */
        uint32_t namecount;
        PCRE2_SPTR nametable;
        uint32_t nameentrysize;

        pcre2_pattern_info(re, PCRE2_INFO_NAMECOUNT, &namecount);
        if (namecount > 0) {
            pcre2_pattern_info(re, PCRE2_INFO_NAMETABLE, &nametable);
            pcre2_pattern_info(re, PCRE2_INFO_NAMEENTRYSIZE, &nameentrysize);

            PCRE2_SPTR tabptr = nametable;
            for (uint32_t i = 0; i < namecount; i++) {
                int group_num = (tabptr[0] << 8) | tabptr[1];
                const char *group_name = (const char *)(tabptr + 2);

                if (group_num < (int)count && ovector[2*group_num] != PCRE2_UNSET) {
                    PCRE2_SIZE start = ovector[2*group_num];
                    PCRE2_SIZE end = ovector[2*group_num + 1];
                    size_t len = end - start;
                    char *val = malloc(len + 1);
                    memcpy(val, str + start, len);
                    val[len] = '\0';
                    StradaValue *named_val = strada_new_str(val);
                    strada_hash_set(named_hash, group_name, named_val);
                    strada_decref(named_val);  /* hash_set increfs, so release our ref */
                    free(val);
                }
                tabptr += nameentrysize;
            }
        }
    }

    /* Register cleanup handler on first use */
    if (!regex_cleanup_registered) { atexit(regex_cleanup_atexit); regex_cleanup_registered = 1; }

    /* Store numbered captures (free old first) */
    if (last_regex_captures) strada_decref(last_regex_captures);
    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_ARRAY;
    sv->refcount = 1;
    sv->value.av = captures;
    last_regex_captures = sv;

    /* Store named captures (free old first) */
    if (last_named_captures) strada_decref(last_named_captures);
    StradaValue *nh = strada_value_alloc();
    nh->type = STRADA_HASH;
    nh->refcount = 1;
    nh->value.hv = named_hash;
    last_named_captures = nh;

    pcre2_match_data_free(match_data);
    /* re is owned by the cache — do NOT free */

    return (rc >= 0) ? 1 : 0;
}

StradaValue* strada_captures(void) {
    if (last_regex_captures) {
        strada_incref(last_regex_captures);
        return last_regex_captures;
    }
    return strada_new_array();
}

StradaValue* strada_capture_var(int n) {
    if (last_regex_captures && last_regex_captures->type == STRADA_ARRAY) {
        StradaArray *arr = last_regex_captures->value.av;
        if (arr && n >= 0 && (size_t)n < arr->size) {
            strada_incref(arr->elements[n]);
            return arr->elements[n];
        }
    }
    return strada_new_undef();
}

StradaValue* strada_named_captures(void) {
    if (last_named_captures) {
        strada_incref(last_named_captures);
        return last_named_captures;
    }
    return strada_new_hash();
}

StradaValue* strada_regex_match_all(const char *str, const char *pattern) {
    StradaArray *matches = strada_array_new();

    pcre2_code *re = regex_cache_compile(pattern, 0);

    if (!re) {
        StradaValue *sv = strada_value_alloc();
        sv->type = STRADA_ARRAY;
        sv->refcount = 1;
        sv->value.av = matches;
        return sv;
    }

    pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(re, NULL);
    PCRE2_SIZE subject_length = strlen(str);
    PCRE2_SIZE offset = 0;

    while (offset < subject_length) {
        int rc = pcre2_match(re, (PCRE2_SPTR)str, subject_length, offset, 0, match_data, NULL);
        if (rc < 0) break;

        PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
        PCRE2_SIZE start = ovector[0];
        PCRE2_SIZE end = ovector[1];

        size_t len = end - start;
        char *matched = malloc(len + 1);
        memcpy(matched, str + start, len);
        matched[len] = '\0';
        strada_array_push_take(matches, strada_new_str(matched));
        free(matched);

        /* Advance past this match; handle zero-length matches */
        if (end == start) {
            offset = end + 1;
        } else {
            offset = end;
        }
    }

    pcre2_match_data_free(match_data);
    /* re is owned by the cache — do NOT free */

    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_ARRAY;
    sv->refcount = 1;
    sv->value.av = matches;
    return sv;
}

/* Helper: preprocess replacement string for pcre2_substitute.
 * Converts Perl-style \1 backreferences to $1 which pcre2_substitute supports natively.
 * Also handles $+ (last captured group) by expanding to the highest $N.
 */
static char* pcre2_preprocess_replacement(const char *replacement) {
    size_t len = strlen(replacement);
    char *out = malloc(len * 3 + 1);  /* worst case: every char is $ needing escape */
    size_t j = 0;

    for (size_t i = 0; i < len; i++) {
        if (replacement[i] == '\\' && i + 1 < len && replacement[i+1] >= '1' && replacement[i+1] <= '9') {
            /* Convert \1 to $1 */
            out[j++] = '$';
            i++;
            out[j++] = replacement[i];
        } else if (replacement[i] == '$') {
            if (i + 1 < len && replacement[i+1] >= '0' && replacement[i+1] <= '9') {
                /* $0-$9: backreference, pass through */
                out[j++] = '$';
            } else if (i + 1 < len && replacement[i+1] == '{') {
                /* ${...}: named backreference, pass through */
                out[j++] = '$';
            } else if (i + 1 < len && replacement[i+1] == '$') {
                /* $$: already escaped literal $, pass through both */
                out[j++] = '$';
                out[j++] = '$';
                i++;
            } else {
                /* $ followed by non-digit or $ at end: escape as $$ */
                out[j++] = '$';
                out[j++] = '$';
            }
        } else {
            out[j++] = replacement[i];
        }
    }
    out[j] = '\0';
    return out;
}

/* Common helper for regex replace using pcre2_substitute */
static char* pcre2_do_substitute(const char *str, const char *pattern, const char *replacement,
                                  const char *flags, int global) {
    uint32_t options = pcre2_get_options(flags);

    pcre2_code *re = regex_cache_compile(pattern, options);

    if (!re) return strdup(str);

    /* Preprocess replacement: convert \1 to $1 for pcre2_substitute */
    char *processed_repl = pcre2_preprocess_replacement(replacement);

    pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(re, NULL);
    PCRE2_SIZE subject_length = strlen(str);

    uint32_t sub_options = PCRE2_SUBSTITUTE_OVERFLOW_LENGTH;
    if (global) sub_options |= PCRE2_SUBSTITUTE_GLOBAL;

    /* First call: determine output size */
    PCRE2_SIZE outlength = 0;
    int rc = pcre2_substitute(re, (PCRE2_SPTR)str, subject_length, 0,
                              sub_options, match_data, NULL,
                              (PCRE2_SPTR)processed_repl, PCRE2_ZERO_TERMINATED,
                              NULL, &outlength);

    if (rc == PCRE2_ERROR_NOMEMORY) {
        /* Expected: outlength now has the needed size */
    } else if (rc == PCRE2_ERROR_NOMATCH) {
        free(processed_repl);
        pcre2_match_data_free(match_data);
        /* re is owned by the cache — do NOT free */
        return strdup(str);
    } else if (rc < 0) {
        free(processed_repl);
        pcre2_match_data_free(match_data);
        /* re is owned by the cache — do NOT free */
        return strdup(str);
    }

    /* Allocate output buffer and run substitute for real */
    outlength += 1; /* for null terminator */
    PCRE2_UCHAR *output = malloc(outlength);

    /* Remove OVERFLOW_LENGTH for the real call */
    sub_options &= ~PCRE2_SUBSTITUTE_OVERFLOW_LENGTH;

    rc = pcre2_substitute(re, (PCRE2_SPTR)str, subject_length, 0,
                          sub_options, match_data, NULL,
                          (PCRE2_SPTR)processed_repl, PCRE2_ZERO_TERMINATED,
                          output, &outlength);

    free(processed_repl);
    pcre2_match_data_free(match_data);
    /* re is owned by the cache — do NOT free */

    if (rc < 0) {
        free(output);
        return strdup(str);
    }

    /* pcre2_substitute outputs a null-terminated string, outlength = number of code units written (not including null) */
    char *result = malloc(outlength + 1);
    memcpy(result, output, outlength);
    result[outlength] = '\0';
    free(output);

    return result;
}

char* strada_regex_replace(const char *str, const char *pattern, const char *replacement, const char *flags) {
    return pcre2_do_substitute(str, pattern, replacement, flags, 0);
}

char* strada_regex_replace_all(const char *str, const char *pattern, const char *replacement, const char *flags) {
    return pcre2_do_substitute(str, pattern, replacement, flags, 1);
}

/* strada_regex_find_all: Find all matches and return array of match info
 * Each element is an array: [start_offset, end_offset, full_match, capture1, ...]
 * If not global, returns at most 1 match.
 */
StradaValue* strada_regex_find_all(const char *str, const char *pattern, const char *flags, int global) {
    StradaValue *result = strada_new_array();
    if (!str || !pattern) return result;

    uint32_t opts = 0;
    if (flags) {
        for (const char *f = flags; *f; f++) {
            if (*f == 'i') opts |= PCRE2_CASELESS;
            else if (*f == 's') opts |= PCRE2_DOTALL;
            else if (*f == 'm') opts |= PCRE2_MULTILINE;
            else if (*f == 'x') opts |= PCRE2_EXTENDED;
        }
    }

    pcre2_code_8 *code = (pcre2_code_8 *)regex_cache_compile(pattern, opts);
    if (!code) return result;

    pcre2_match_data_8 *md = pcre2_match_data_create_from_pattern_8(code, NULL);
    size_t len = strlen(str);
    size_t offset = 0;

    while (offset <= len) {
        int rc = pcre2_match_8(code, (PCRE2_SPTR)str, len, offset, 0, md, NULL);
        if (rc < 0) break;

        PCRE2_SIZE *ov = pcre2_get_ovector_pointer_8(md);
        StradaValue *match = strada_new_array();

        /* [0] = start offset, [1] = end offset */
        StradaValue *__tmp;
        __tmp = strada_new_int((int64_t)ov[0]);
        strada_array_push(match->value.av, __tmp);
        strada_decref(__tmp);
        __tmp = strada_new_int((int64_t)ov[1]);
        strada_array_push(match->value.av, __tmp);
        strada_decref(__tmp);

        /* [2..] = full match and capture groups */
        for (int i = 0; i < rc; i++) {
            if (ov[2*i] != PCRE2_UNSET) {
                size_t mlen = ov[2*i+1] - ov[2*i];
                char *s = malloc(mlen + 1);
                memcpy(s, str + ov[2*i], mlen);
                s[mlen] = '\0';
                __tmp = strada_new_str(s);
                strada_array_push(match->value.av, __tmp);
                strada_decref(__tmp);
                free(s);
            } else {
                __tmp = strada_new_str("");
                strada_array_push(match->value.av, __tmp);
                strada_decref(__tmp);
            }
        }

        strada_array_push(result->value.av, match);
        strada_decref(match);  /* push increfs; release our ref */

        /* Advance past this match */
        offset = ov[1];
        if (ov[0] == ov[1]) offset++;  /* Zero-length match */
        if (!global) break;
    }

    pcre2_match_data_free_8(md);
    /* code is owned by the cache — do NOT free */
    return result;
}

/* strada_set_captures_sv: Set captures array from a match info array.
 * match is [start, end, full_match, capture1, capture2, ...]
 * Sets the global captures so captures() works in /e replacement expressions.
 */
void strada_set_captures_sv(StradaValue *match) {
    if (!match || match->type != STRADA_ARRAY) return;
    StradaArray *marr = match->value.av;
    if (marr->size < 3) return;

    /* Clear old captures and set new ones */
    if (last_regex_captures) {
        strada_decref(last_regex_captures);
    }
    last_regex_captures = strada_new_array();
    /* captures()[0] = full match, captures()[1] = $1, etc. */
    for (size_t i = 2; i < marr->size; i++) {
        strada_array_push(last_regex_captures->value.av, marr->elements[i]);
    }
}

/* strada_regex_build_result: Build result string by replacing matches.
 * src = original string
 * matches = array of [start, end, ...] from strada_regex_find_all
 * replacements = array of replacement strings (one per match)
 */
StradaValue* strada_regex_build_result(const char *src, StradaValue *matches, StradaValue *replacements) {
    if (!src || !matches || !replacements) return strada_new_str(src ? src : "");
    StradaArray *marr = matches->value.av;
    StradaArray *rarr = replacements->value.av;
    if (marr->size == 0) return strada_new_str(src);

    size_t src_len = strlen(src);
    size_t buf_cap = src_len * 2 + 256;
    char *buf = malloc(buf_cap);
    size_t bpos = 0;
    size_t offset = 0;

    for (size_t i = 0; i < marr->size && i < rarr->size; i++) {
        StradaValue *m = marr->elements[i];
        if (!m || m->type != STRADA_ARRAY || !m->value.av || m->value.av->size < 2) continue;
        int64_t mstart = strada_to_int(m->value.av->elements[0]);
        int64_t mend = strada_to_int(m->value.av->elements[1]);
        if (mstart < (int64_t)offset || mend < mstart || (size_t)mend > src_len) continue;

        /* Append pre-match text */
        size_t pre_len = (size_t)(mstart - (int64_t)offset);
        if (bpos + pre_len + 1 >= buf_cap) {
            buf_cap = (bpos + pre_len) * 2 + 256;
            char *nb = realloc(buf, buf_cap);
            if (!nb) { free(buf); return strada_new_str(src); }
            buf = nb;
        }
        memcpy(buf + bpos, src + offset, pre_len);
        bpos += pre_len;

        /* Append replacement */
        char *repl = strada_to_str(rarr->elements[i]);
        size_t rlen = strlen(repl);
        if (bpos + rlen + 1 >= buf_cap) {
            buf_cap = (bpos + rlen) * 2 + 256;
            char *nb = realloc(buf, buf_cap);
            if (!nb) { free(buf); free(repl); return strada_new_str(src); }
            buf = nb;
        }
        memcpy(buf + bpos, repl, rlen);
        bpos += rlen;
        free(repl);

        offset = mend;
    }

    /* Append remainder */
    size_t rem = (src_len >= offset) ? src_len - offset : 0;
    if (bpos + rem + 1 >= buf_cap) {
        buf_cap = (bpos + rem) * 2 + 256;
        char *nb = realloc(buf, buf_cap);
        if (!nb) { free(buf); return strada_new_str(src); }
        buf = nb;
    }
    memcpy(buf + bpos, src + offset, rem);
    bpos += rem;
    buf[bpos] = '\0';

    StradaValue *result = strada_new_str(buf);
    free(buf);
    return result;
}

StradaArray* strada_regex_split(const char *str, const char *pattern) {
    StradaArray *parts = strada_array_new();

    pcre2_code *re = regex_cache_compile(pattern, 0);

    if (!re) {
        strada_array_push_take(parts, strada_new_str(str));
        return parts;
    }

    pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(re, NULL);
    PCRE2_SIZE subject_length = strlen(str);
    PCRE2_SIZE offset = 0;

    while (offset < subject_length) {
        int rc = pcre2_match(re, (PCRE2_SPTR)str, subject_length, offset, 0, match_data, NULL);
        if (rc < 0) break;

        PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
        PCRE2_SIZE match_start = ovector[0];
        PCRE2_SIZE match_end = ovector[1];

        /* Add part before match */
        size_t part_len = match_start - offset;
        char *part = malloc(part_len + 1);
        memcpy(part, str + offset, part_len);
        part[part_len] = '\0';
        strada_array_push_take(parts, strada_new_str(part));
        free(part);

        /* Handle zero-length match */
        if (match_end == match_start) {
            offset = match_end + 1;
        } else {
            offset = match_end;
        }
    }

    /* Add remaining part */
    if (offset <= subject_length) {
        strada_array_push_take(parts, strada_new_str(str + offset));
    }

    pcre2_match_data_free(match_data);
    /* re is owned by the cache — do NOT free */

    return parts;
}

StradaArray* strada_regex_capture(const char *str, const char *pattern) {
    StradaArray *captures = strada_array_new();

    pcre2_code *re = regex_cache_compile(pattern, 0);

    if (!re) return captures;

    pcre2_match_data *match_data = pcre2_match_data_create_from_pattern(re, NULL);
    int rc = pcre2_match(re, (PCRE2_SPTR)str, strlen(str), 0, 0, match_data, NULL);

    if (rc >= 0) {
        PCRE2_SIZE *ovector = pcre2_get_ovector_pointer(match_data);
        uint32_t count = pcre2_get_ovector_count(match_data);

        for (uint32_t i = 0; i < count; i++) {
            if (ovector[2*i] != PCRE2_UNSET) {
                PCRE2_SIZE start = ovector[2*i];
                PCRE2_SIZE end = ovector[2*i + 1];
                size_t len = end - start;
                char *capture = malloc(len + 1);
                memcpy(capture, str + start, len);
                capture[len] = '\0';
                strada_array_push_take(captures, strada_new_str(capture));
                free(capture);
            } else {
                strada_array_push_take(captures, strada_new_undef());
            }
        }
    }

    pcre2_match_data_free(match_data);
    /* re is owned by the cache — do NOT free */

    return captures;
}

#else /* POSIX regex fallback */

/* Helper: Convert flag string to POSIX regex cflags */
static int regex_get_cflags(const char *flags) {
    int cflags = REG_EXTENDED;
    if (flags) {
        if (strchr(flags, 'i')) cflags |= REG_ICASE;
        if (strchr(flags, 'm')) cflags |= REG_NEWLINE;
    }
    return cflags;
}

/* Helper: Preprocess pattern - converts PCRE shortcuts to POSIX equivalents
 * and handles /s (dotall) and /x (extended) flags.
 *
 * PCRE to POSIX conversions:
 *   \d -> [0-9]           \D -> [^0-9]
 *   \w -> [a-zA-Z0-9_]    \W -> [^a-zA-Z0-9_]
 *   \s -> [ \t\n\r\f\v]   \S -> [^ \t\n\r\f\v]
 */
static char* regex_preprocess_pattern(const char *pattern, const char *flags) {
    int need_dotall = flags && (strchr(flags, 's') != NULL);
    int need_extended = flags && (strchr(flags, 'x') != NULL);

    /* Allocate buffer (worst case: \s becomes 13 chars "[ \t\n\r\f\v]") */
    size_t len = strlen(pattern);
    char *result = malloc(len * 15 + 1);
    char *out = result;

    int escaped = 0;
    int in_bracket = 0;

    for (const char *p = pattern; *p; p++) {
        if (escaped) {
            escaped = 0;
            /* Convert PCRE character classes to POSIX */
            switch (*p) {
                case 'd':
                    /* Remove the backslash we wrote, replace with POSIX class */
                    out--;
                    strcpy(out, "[0-9]");
                    out += 5;
                    continue;
                case 'D':
                    out--;
                    strcpy(out, "[^0-9]");
                    out += 6;
                    continue;
                case 'w':
                    out--;
                    strcpy(out, "[a-zA-Z0-9_]");
                    out += 12;
                    continue;
                case 'W':
                    out--;
                    strcpy(out, "[^a-zA-Z0-9_]");
                    out += 13;
                    continue;
                case 's':
                    out--;
                    strcpy(out, "[ \t\n\r\f\v]");
                    out += 8;  /* 8 bytes: [ space tab newline cr ff vt ] */
                    continue;
                case 'S':
                    out--;
                    strcpy(out, "[^ \t\n\r\f\v]");
                    out += 9;  /* 9 bytes: [ ^ space tab newline cr ff vt ] */
                    continue;
                default:
                    /* Keep other escapes as-is (backslash already written) */
                    *out++ = *p;
                    continue;
            }
        }

        if (*p == '\\') {
            /* Write backslash tentatively - we'll remove it if PCRE shortcut */
            escaped = 1;
            *out++ = '\\';
            continue;
        }

        if (*p == '[') in_bracket = 1;
        if (*p == ']') in_bracket = 0;

        /* /s: Replace . with (.|\n) outside brackets */
        if (need_dotall && *p == '.' && !in_bracket) {
            strcpy(out, "(.|\n)");
            out += 5;
            continue;
        }

        /* /x: Skip whitespace and # comments outside brackets */
        if (need_extended && !in_bracket) {
            if (*p == ' ' || *p == '\t' || *p == '\n') continue;
            if (*p == '#') {
                while (*p && *p != '\n') p++;
                if (!*p) break;
                continue;
            }
        }

        *out++ = *p;
    }
    *out = '\0';
    return result;
}

/* ===== REGEX COMPILATION CACHE (POSIX) ===== */
#define REGEX_CACHE_SIZE 128
typedef struct {
    char *pattern;
    int cflags;
    regex_t compiled;
    uint64_t last_used;
    int valid;
} RegexCacheEntry;

static RegexCacheEntry regex_cache[REGEX_CACHE_SIZE];
static int regex_cache_count = 0;
static uint64_t regex_cache_counter = 0;
static int regex_cache_initialized = 0;

static void regex_cache_init(void) {
    memset(regex_cache, 0, sizeof(regex_cache));
    regex_cache_initialized = 1;
}

static unsigned int regex_cache_hash_posix(const char *pattern, int cflags) {
    unsigned int h = 5381;
    for (const char *p = pattern; *p; p++) {
        h = ((h << 5) + h) + (unsigned char)*p;
    }
    h = ((h << 5) + h) + (unsigned int)cflags;
    return h;
}

/* Look up a cached compiled regex. Returns pointer to regex_t on hit, NULL on miss. */
static regex_t *regex_cache_lookup_posix(const char *pattern, int cflags) {
    if (!regex_cache_initialized) regex_cache_init();
    unsigned int h = regex_cache_hash_posix(pattern, cflags) & (REGEX_CACHE_SIZE - 1);
    RegexCacheEntry *e = &regex_cache[h];
    if (e->valid && e->cflags == cflags && e->pattern && strcmp(e->pattern, pattern) == 0) {
        e->last_used = ++regex_cache_counter;
        return &e->compiled;
    }
    return NULL;
}

/* Insert a compiled regex into the cache. Evicts existing entry at same slot. */
static regex_t *regex_cache_insert_posix(const char *pattern, int cflags, regex_t *compiled) {
    if (!regex_cache_initialized) regex_cache_init();
    unsigned int h = regex_cache_hash_posix(pattern, cflags) & (REGEX_CACHE_SIZE - 1);
    RegexCacheEntry *e = &regex_cache[h];
    /* Evict existing entry */
    if (e->valid) {
        free(e->pattern);
        regfree(&e->compiled);
    } else {
        regex_cache_count++;
    }
    e->pattern = strdup(pattern);
    e->cflags = cflags;
    e->compiled = *compiled;
    e->valid = 1;
    e->last_used = ++regex_cache_counter;
    return &e->compiled;
}

/* Compile regex with caching (POSIX version).
 * Takes the original pattern (before preprocessing) and flags string.
 * Returns pointer to cached regex_t, or NULL on error.
 * The preprocessed pattern is used for compilation but the original is the cache key. */
static regex_t *regex_cache_compile_posix(const char *pattern, const char *flags) {
    int cflags = regex_get_cflags(flags);
    regex_t *cached = regex_cache_lookup_posix(pattern, cflags);
    if (cached) return cached;

    char *processed = regex_preprocess_pattern(pattern, flags);
    regex_t rx;
    int result = regcomp(&rx, processed, cflags);
    free(processed);
    if (result != 0) return NULL;

    return regex_cache_insert_posix(pattern, cflags, &rx);
}

static void regex_cache_cleanup(void) {
    for (int i = 0; i < REGEX_CACHE_SIZE; i++) {
        if (regex_cache[i].valid) {
            free(regex_cache[i].pattern);
            regfree(&regex_cache[i].compiled);
            regex_cache[i].pattern = NULL;
            regex_cache[i].valid = 0;
        }
    }
    regex_cache_count = 0;
}

StradaValue* strada_regex_compile(const char *pattern, const char *flags) {
    regex_t *rx = malloc(sizeof(regex_t));
    int cflags = regex_get_cflags(flags);
    char *processed = regex_preprocess_pattern(pattern, flags);

    int result = regcomp(rx, processed, cflags);
    free(processed);
    if (result != 0) {
        free(rx);
        return strada_new_undef();
    }

    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_REGEX;
    sv->refcount = 1;
    sv->value.rx = rx;

    return sv;
}

int strada_regex_match(const char *str, const char *pattern) {
    regex_t *rx = regex_cache_compile_posix(pattern, NULL);
    if (!rx) return 0;

    int result = regexec(rx, str, 0, NULL, 0);
    /* rx is owned by the cache — do NOT free */

    return (result == 0) ? 1 : 0;
}

/* Global storage for last regex captures */
static StradaValue *last_regex_captures = NULL;
static int regex_cleanup_registered = 0;

static void regex_cleanup_atexit(void) {
    if (last_regex_captures) { strada_decref(last_regex_captures); last_regex_captures = NULL; }
    regex_cache_cleanup();
}

int strada_regex_match_with_capture(const char *str, const char *pattern, const char *flags) {
    int cflags = regex_get_cflags(flags);
    regex_t *rx = regex_cache_compile_posix(pattern, flags);

    if (!rx) {
        /* Clear previous captures on failed compile */
        if (last_regex_captures) {
            strada_decref(last_regex_captures);
            last_regex_captures = NULL;
        }
        return 0;
    }

    size_t nmatch = rx->re_nsub + 1;
    regmatch_t *matches = malloc(sizeof(regmatch_t) * nmatch);

    int matched = (regexec(rx, str, nmatch, matches, 0) == 0);

    /* Create new captures array */
    StradaArray *captures = strada_array_new();

    if (matched) {
        for (size_t i = 0; i < nmatch; i++) {
            if (matches[i].rm_so != -1) {
                int len = matches[i].rm_eo - matches[i].rm_so;
                char *capture = malloc(len + 1);
                strncpy(capture, str + matches[i].rm_so, len);
                capture[len] = '\0';
                strada_array_push_take(captures, strada_new_str(capture));
                free(capture);
            } else {
                strada_array_push_take(captures, strada_new_undef());
            }
        }
    }

    /* Register cleanup handler on first use */
    if (!regex_cleanup_registered) { atexit(regex_cleanup_atexit); regex_cleanup_registered = 1; }

    /* Store captures for later retrieval (free old first) */
    if (last_regex_captures) strada_decref(last_regex_captures);
    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_ARRAY;
    sv->refcount = 1;
    sv->value.av = captures;
    last_regex_captures = sv;

    free(matches);
    /* rx is owned by the cache — do NOT free */

    return matched;
}

StradaValue* strada_captures(void) {
    if (last_regex_captures) {
        strada_incref(last_regex_captures);
        return last_regex_captures;
    }
    /* Return empty array if no captures */
    return strada_new_array();
}

StradaValue* strada_capture_var(int n) {
    if (last_regex_captures && last_regex_captures->type == STRADA_ARRAY) {
        StradaArray *arr = last_regex_captures->value.av;
        if (arr && n >= 0 && (size_t)n < arr->size) {
            strada_incref(arr->elements[n]);
            return arr->elements[n];
        }
    }
    return strada_new_undef();
}

StradaValue* strada_regex_match_all(const char *str, const char *pattern) {
    StradaArray *matches = strada_array_new();

    regex_t *rx = regex_cache_compile_posix(pattern, NULL);
    if (!rx) {
        StradaValue *sv = strada_value_alloc();
        sv->type = STRADA_ARRAY;
        sv->refcount = 1;
        sv->value.av = matches;
        return sv;
    }

    const char *p = str;
    regmatch_t match;

    while (regexec(rx, p, 1, &match, 0) == 0) {
        int len = match.rm_eo - match.rm_so;
        char *matched = malloc(len + 1);
        strncpy(matched, p + match.rm_so, len);
        matched[len] = '\0';

        strada_array_push_take(matches, strada_new_str(matched));
        free(matched);

        p += match.rm_eo;
        if (match.rm_eo == 0) break; // Prevent infinite loop on empty matches
    }

    /* rx is owned by the cache — do NOT free */

    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_ARRAY;
    sv->refcount = 1;
    sv->value.av = matches;
    return sv;
}

char* strada_regex_replace(const char *str, const char *pattern, const char *replacement, const char *flags) {
    regmatch_t match;

    regex_t *rx = regex_cache_compile_posix(pattern, flags);
    if (!rx) return strdup(str);

    if (regexec(rx, str, 1, &match, 0) != 0) {
        /* rx is owned by the cache — do NOT free */
        return strdup(str);
    }

    // Calculate result length
    size_t before_len = match.rm_so;
    size_t after_start = match.rm_eo;
    size_t after_len = strlen(str) - after_start;
    size_t repl_len = strlen(replacement);

    char *result = malloc(before_len + repl_len + after_len + 1);

    // Copy parts
    strncpy(result, str, before_len);
    strcpy(result + before_len, replacement);
    strcpy(result + before_len + repl_len, str + after_start);

    /* rx is owned by the cache — do NOT free */
    return result;
}

char* strada_regex_replace_all(const char *str, const char *pattern, const char *replacement, const char *flags) {
    regex_t *rx = regex_cache_compile_posix(pattern, flags);
    if (!rx) return strdup(str);

    size_t result_size = strlen(str) * 2 + 1;
    char *result = malloc(result_size);
    result[0] = '\0';

    const char *p = str;
    regmatch_t match;
    size_t offset = 0;

    while (regexec(rx, p, 1, &match, 0) == 0) {
        // Ensure buffer is large enough
        size_t needed = offset + match.rm_so + strlen(replacement) + strlen(p + match.rm_eo) + 1;
        if (needed > result_size) {
            result_size = needed * 2;
            char *new_result = realloc(result, result_size);
            if (!new_result) { free(result); return strdup(str); }
            result = new_result;
        }

        // Copy before match
        strncat(result, p, match.rm_so);
        offset += match.rm_so;

        // Copy replacement
        strcat(result, replacement);
        offset += strlen(replacement);

        p += match.rm_eo;
        if (match.rm_eo == 0) break; // Prevent infinite loop
    }

    // Copy remaining
    strcat(result, p);

    /* rx is owned by the cache — do NOT free */
    return result;
}

/* POSIX fallback versions of /e support functions */
StradaValue* strada_regex_find_all(const char *str, const char *pattern, const char *flags, int global) {
    StradaValue *result = strada_new_array();
    if (!str || !pattern) return result;

    regex_t *rx = regex_cache_compile_posix(pattern, flags);
    if (!rx) return result;

    regmatch_t matches[10];
    size_t offset = 0;
    size_t len = strlen(str);

    while (offset <= len) {
        int rc = regexec(rx, str + offset, 10, matches, (offset > 0) ? REG_NOTBOL : 0);
        if (rc != 0) break;

        StradaValue *match = strada_new_array();
        StradaValue *__tmp;
        /* [0] = start offset, [1] = end offset (absolute) */
        __tmp = strada_new_int((int64_t)(offset + matches[0].rm_so));
        strada_array_push(match->value.av, __tmp);
        strada_decref(__tmp);
        __tmp = strada_new_int((int64_t)(offset + matches[0].rm_eo));
        strada_array_push(match->value.av, __tmp);
        strada_decref(__tmp);

        /* [2..] = full match and capture groups */
        for (int i = 0; i < 10 && matches[i].rm_so != -1; i++) {
            size_t mlen = matches[i].rm_eo - matches[i].rm_so;
            char *s = malloc(mlen + 1);
            memcpy(s, str + offset + matches[i].rm_so, mlen);
            s[mlen] = '\0';
            __tmp = strada_new_str(s);
            strada_array_push(match->value.av, __tmp);
            strada_decref(__tmp);
            free(s);
        }

        strada_array_push(result->value.av, match);
        strada_decref(match);  /* push increfs; release our ref */

        offset += matches[0].rm_eo;
        if (matches[0].rm_so == matches[0].rm_eo) offset++;
        if (!global) break;
    }

    /* rx is owned by the cache — do NOT free */
    return result;
}

void strada_set_captures_sv(StradaValue *match) {
    if (!match || match->type != STRADA_ARRAY) return;
    StradaArray *marr = match->value.av;
    if (marr->size < 3) return;
    if (last_regex_captures) {
        strada_decref(last_regex_captures);
    }
    last_regex_captures = strada_new_array();
    for (size_t i = 2; i < marr->size; i++) {
        strada_array_push(last_regex_captures->value.av, marr->elements[i]);
    }
}

StradaValue* strada_regex_build_result(const char *src, StradaValue *matches, StradaValue *replacements) {
    if (!src || !matches || !replacements) return strada_new_str(src ? src : "");
    StradaArray *marr = matches->value.av;
    StradaArray *rarr = replacements->value.av;
    if (marr->size == 0) return strada_new_str(src);

    size_t src_len = strlen(src);
    size_t buf_cap = src_len * 2 + 256;
    char *buf = malloc(buf_cap);
    size_t bpos = 0;
    size_t offset = 0;

    for (size_t i = 0; i < marr->size && i < rarr->size; i++) {
        StradaValue *m = marr->elements[i];
        int64_t mstart = strada_to_int(m->value.av->elements[0]);
        int64_t mend = strada_to_int(m->value.av->elements[1]);

        size_t pre_len = mstart - offset;
        if (bpos + pre_len + 1 >= buf_cap) {
            buf_cap = (bpos + pre_len) * 2 + 256;
            buf = realloc(buf, buf_cap);
        }
        memcpy(buf + bpos, src + offset, pre_len);
        bpos += pre_len;

        char *repl = strada_to_str(rarr->elements[i]);
        size_t rlen = strlen(repl);
        if (bpos + rlen + 1 >= buf_cap) {
            buf_cap = (bpos + rlen) * 2 + 256;
            buf = realloc(buf, buf_cap);
        }
        memcpy(buf + bpos, repl, rlen);
        bpos += rlen;
        free(repl);

        offset = mend;
    }

    size_t rem = src_len - offset;
    if (bpos + rem + 1 >= buf_cap) {
        buf_cap = (bpos + rem) * 2 + 256;
        buf = realloc(buf, buf_cap);
    }
    memcpy(buf + bpos, src + offset, rem);
    bpos += rem;
    buf[bpos] = '\0';

    StradaValue *res = strada_new_str(buf);
    free(buf);
    return res;
}

StradaArray* strada_regex_split(const char *str, const char *pattern) {
    StradaArray *parts = strada_array_new();

    regex_t *rx = regex_cache_compile_posix(pattern, NULL);
    if (!rx) {
        strada_array_push_take(parts, strada_new_str(str));
        return parts;
    }

    const char *p = str;
    regmatch_t match;

    while (regexec(rx, p, 1, &match, 0) == 0) {
        // Add part before match
        char *part = malloc(match.rm_so + 1);
        strncpy(part, p, match.rm_so);
        part[match.rm_so] = '\0';

        strada_array_push_take(parts, strada_new_str(part));
        free(part);

        p += match.rm_eo;
        if (match.rm_eo == 0) break;
    }

    // Add remaining part
    if (*p) {
        strada_array_push_take(parts, strada_new_str(p));
    }

    /* rx is owned by the cache — do NOT free */
    return parts;
}

StradaArray* strada_regex_capture(const char *str, const char *pattern) {
    StradaArray *captures = strada_array_new();

    regex_t *rx = regex_cache_compile_posix(pattern, NULL);
    if (!rx) return captures;

    size_t nmatch = rx->re_nsub + 1;
    regmatch_t *matches = malloc(sizeof(regmatch_t) * nmatch);

    if (regexec(rx, str, nmatch, matches, 0) == 0) {
        for (size_t i = 0; i < nmatch; i++) {
            if (matches[i].rm_so != -1) {
                int len = matches[i].rm_eo - matches[i].rm_so;
                char *capture = malloc(len + 1);
                strncpy(capture, str + matches[i].rm_so, len);
                capture[len] = '\0';

                strada_array_push_take(captures, strada_new_str(capture));
                free(capture);
            } else {
                strada_array_push_take(captures, strada_new_undef());
            }
        }
    }

    free(matches);
    /* rx is owned by the cache — do NOT free */

    return captures;
}

StradaValue* strada_named_captures(void) {
    /* POSIX regex doesn't support named captures - return empty hash */
    return strada_new_hash();
}

#endif /* HAVE_PCRE2 */

/* String split - literal string delimiter (no regex) */
StradaArray* strada_string_split(const char *str, const char *delim) {
    StradaArray *parts = strada_array_new();

    if (!str || !delim) {
        if (str) strada_array_push_take(parts, strada_new_str(str));
        return parts;
    }

    size_t delim_len = strlen(delim);
    if (delim_len == 0) {
        /* Empty delimiter - split into individual characters */
        const char *p = str;
        while (*p) {
            char ch[2] = {*p, '\0'};
            strada_array_push_take(parts, strada_new_str(ch));
            p++;
        }
        return parts;
    }

    const char *p = str;
    const char *found;

    while ((found = strstr(p, delim)) != NULL) {
        /* Add part before delimiter — single allocation via strada_new_str_len */
        strada_array_push_take(parts, strada_new_str_len(p, found - p));
        p = found + delim_len;
    }

    /* Add remaining part */
    strada_array_push_take(parts, strada_new_str(p));

    return parts;
}

/* ===== MEMORY MANAGEMENT ===== */

void strada_free_value(StradaValue *sv) {
    if (!sv) return;

    /* Notify any weak references pointing to this value before freeing
     * (Phase 3: skip mutex when no weak refs have ever been registered) */
    if (weak_registry_has_entries)
        strada_weak_registry_remove_target(sv);

    /* If this is a weak reference being freed, unregister it from the registry */
    if (SV_IS_WEAK(sv) && sv->type == STRADA_REF && sv->value.rv) {
        strada_weak_registry_unregister(sv);
    }

    /* Free tied object if any */
    if (SV_IS_TIED(sv) && SV_TIED_OBJ(sv)) {
        strada_decref(sv->meta->tied_obj);
        sv->meta->tied_obj = NULL;
        sv->meta->is_tied = 0;
    }

    /* Track memory free for profiling */
    strada_memprof_free(sv->type, sizeof(StradaValue));

    /* Call DESTROY method if this is a blessed reference */
    char *bp = SV_BLESSED(sv);
    if (bp) {
        strada_check_debug_bless();

        /* Debug logging if enabled */
        if (strada_debug_bless) {
            fprintf(stderr, "[FREE_VALUE] sv=%p type=%d blessed_package=%p\n",
                    (void*)sv, sv->type, (void*)bp);
        }

        /* Only references (STRADA_REF) should have blessed_package set */
        /* If another type has it, something is very wrong */
        if (sv->type != STRADA_REF) {
            fprintf(stderr, "ERROR: Non-reference type %d has blessed_package set! This indicates memory corruption.\n", sv->type);
            sv->meta->blessed_package = NULL;  /* Clear to prevent crash */
        } else if (!strada_validate_blessed_package(bp)) {
            fprintf(stderr, "Error: strada_free_value detected corrupted blessed_package at sv=%p, skipping DESTROY and free\n", (void*)sv);
            sv->meta->blessed_package = NULL;  /* Clear to prevent crash */
        } else {
            strada_call_destroy(sv);
            strada_intern_release(sv->meta->blessed_package);
            sv->meta->blessed_package = NULL;
        }
    }

    switch (sv->type) {
        case STRADA_STR:
            free(sv->value.pv);
            break;
        case STRADA_ARRAY:
            strada_free_array(sv->value.av);
            break;
        case STRADA_HASH:
            strada_free_hash(sv->value.hv);
            break;
        case STRADA_REF:
            /* Decrement reference count on the referent (skip for weak refs -
             * their refcount contribution was already removed in strada_weaken) */
            if (sv->value.rv && !SV_IS_WEAK(sv)) {
                strada_decref(sv->value.rv);
            }
            break;
        case STRADA_FILEHANDLE:
            if (sv->value.fh) {
                strada_close_fh_meta(sv->value.fh);
            }
            break;
        case STRADA_REGEX:
#ifdef HAVE_PCRE2
            if (sv->value.pcre2_rx && strada_pcre2_code_free_fn) {
                strada_pcre2_code_free_fn(sv->value.pcre2_rx);
            }
#else
            if (sv->value.rx) {
                regfree(sv->value.rx);
                free(sv->value.rx);
            }
#endif
            break;
        case STRADA_SOCKET:
            if (sv->value.sock) {
                /* Flush and close */
                if (sv->value.sock->fd >= 0) {
                    if (sv->value.sock->write_len > 0) {
                        send(sv->value.sock->fd, sv->value.sock->write_buf,
                             sv->value.sock->write_len, 0);
                    }
                    close(sv->value.sock->fd);
                }
                free(sv->value.sock);
            }
            break;
        case STRADA_CSTRUCT:
            if (sv->value.ptr) {
                free(sv->value.ptr);
            }
            if (SV_STRUCT_NAME(sv)) {
                free(sv->meta->struct_name);
            }
            break;
        case STRADA_CPOINTER: {
            const char *sn = SV_STRUCT_NAME(sv);
            if (sv->value.ptr && sn) {
                if (strcmp(sn, "StringBuilder") == 0) {
                    StradaStringBuilder *sb = (StradaStringBuilder*)sv->value.ptr;
                    free(sb->buffer);
                    free(sb);
                } else if (strcmp(sn, "Mutex") == 0) {
                    StradaMutex *m = (StradaMutex*)sv->value.ptr;
                    pthread_mutex_destroy(&m->mutex);
                    free(m);
                } else if (strcmp(sn, "Cond") == 0) {
                    StradaCond *c = (StradaCond*)sv->value.ptr;
                    pthread_cond_destroy(&c->cond);
                    free(c);
                }
            }
            break;
        }
        case STRADA_CLOSURE:
            if (sv->value.ptr) {
                StradaClosure *cl = (StradaClosure*)sv->value.ptr;
                /* Free captured values */
                if (cl->captures) {
                    for (int i = 0; i < cl->capture_count; i++) {
                        if (cl->captures[i]) {
                            if (*(cl->captures[i])) {
                                strada_decref(*(cl->captures[i]));
                            }
                            free(cl->captures[i]);
                        }
                    }
                    free(cl->captures);
                }
                free(cl);
            }
            break;
        case STRADA_FUTURE:
            if (sv->value.ptr) {
                StradaFuture *f = (StradaFuture*)sv->value.ptr;
                /* Wait for completion before cleanup to avoid race conditions */
                pthread_mutex_lock(&f->mutex);
                while (f->state == FUTURE_PENDING || f->state == FUTURE_RUNNING) {
                    f->cancel_requested = 1;  /* Request cancellation */
                    pthread_cond_wait(&f->cond, &f->mutex);
                }
                pthread_mutex_unlock(&f->mutex);

                if (f->closure) strada_decref(f->closure);
                if (f->result) strada_decref(f->result);
                if (f->error) strada_decref(f->error);
                pthread_mutex_destroy(&f->mutex);
                pthread_cond_destroy(&f->cond);
                free(f);
            }
            break;
        case STRADA_CHANNEL:
            if (sv->value.ptr) {
                StradaChannel *ch = (StradaChannel*)sv->value.ptr;
                /* Close channel first to wake any waiting threads */
                pthread_mutex_lock(&ch->mutex);
                ch->closed = 1;
                pthread_cond_broadcast(&ch->not_empty);
                pthread_cond_broadcast(&ch->not_full);
                pthread_mutex_unlock(&ch->mutex);

                /* Free all queued items */
                StradaChannelNode *node = ch->head;
                while (node) {
                    StradaChannelNode *next = node->next;
                    if (node->value) strada_decref(node->value);
                    free(node);
                    node = next;
                }

                pthread_mutex_destroy(&ch->mutex);
                pthread_cond_destroy(&ch->not_empty);
                pthread_cond_destroy(&ch->not_full);
                free(ch);
            }
            break;
        case STRADA_ATOMIC:
            if (sv->value.ptr) {
                free(sv->value.ptr);
            }
            break;
        default:
            break;
    }

    /* Free metadata if allocated, then recycle sv into pool.
     * Set refcount to immortal so stale pointers that call strada_decref()
     * on the pooled value harmlessly skip it (see the >1000000000 guard). */
    if (sv->meta) {
        if (meta_pool_count < META_POOL_MAX) {
            meta_pool[meta_pool_count++] = sv->meta;
        } else {
            free(sv->meta);
        }
        sv->meta = NULL;
    }
    if (!strada_threading_active && sv_pool_count < SV_POOL_MAX) {
        sv->refcount = 2000000000;
        sv_pool_stack[sv_pool_count++] = sv;
    } else
        free(sv);
}

void strada_free_array(StradaArray *av) {
    if (!av) return;

    av->refcount--;
    if (av->refcount > 0) return;

    for (size_t i = 0; i < av->size; i++) {
        strada_decref(av->elements[av->head + i]);
    }

    free(av->elements);
    free(av);
}

void strada_free_hash(StradaHash *hv) {
    if (!hv) return;

    hv->refcount--;
    if (hv->refcount > 0) return;

    for (size_t i = 0; i < hv->num_buckets; i++) {
        StradaHashEntry *entry = hv->buckets[i];
        while (entry) {
            StradaHashEntry *next = entry->next;
            strada_intern_release(entry->key);
            strada_decref(entry->value);
            strada_hash_entry_free(entry);
            entry = next;
        }
    }

    free(hv->buckets);
    free(hv);
}

/* ===== FFI - FOREIGN FUNCTION INTERFACE ===== */

void* strada_dlopen(const char *library) {
    void *handle = dlopen(library, RTLD_LAZY);
    return handle;
}

void* strada_dlsym(void *handle, const char *symbol) {
    if (!handle) return NULL;
    return dlsym(handle, symbol);
}

void strada_dlclose(void *handle) {
    if (handle) {
        dlclose(handle);
    }
}

/* StradaValue wrappers for dynamic loading */

/* dl_open - open a shared library, returns handle as int (pointer) or undef on failure */
StradaValue* strada_dl_open(StradaValue *library) {
    if (!library) return strada_new_undef();
    char _tb[PATH_MAX];
    const char *lib = strada_to_str_buf(library, _tb, sizeof(_tb));
    void *handle = dlopen(lib, RTLD_LAZY);
    if (!handle) return strada_new_undef();
    /* Store handle as int64 (pointer fits in 64-bit int) */
    return strada_new_int((int64_t)(intptr_t)handle);
}

/* dl_sym - get symbol from library, returns function pointer as int or undef on failure */
StradaValue* strada_dl_sym(StradaValue *handle, StradaValue *symbol) {
    if (!handle || !symbol) return strada_new_undef();
    void *h = (void*)(intptr_t)strada_to_int(handle);
    if (!h) return strada_new_undef();
    char _tb[256];
    const char *sym = strada_to_str_buf(symbol, _tb, sizeof(_tb));
    void *ptr = dlsym(h, sym);
    if (!ptr) return strada_new_undef();
    return strada_new_int((int64_t)(intptr_t)ptr);
}

/* dl_close - close a shared library */
StradaValue* strada_dl_close(StradaValue *handle) {
    if (!handle) return strada_new_int(-1);
    void *h = (void*)(intptr_t)strada_to_int(handle);
    if (h) {
        dlclose(h);
        return strada_new_int(0);
    }
    return strada_new_int(-1);
}

/* dl_error - get last dlopen/dlsym error */
StradaValue* strada_dl_error(void) {
    const char *err = dlerror();
    if (err) {
        return strada_new_str(err);
    }
    return strada_new_str("");
}

/* Type definitions for function pointer calls - supports up to 10 arguments */
typedef int64_t (*ffi_func_0)(void);
typedef int64_t (*ffi_func_1)(int64_t);
typedef int64_t (*ffi_func_2)(int64_t, int64_t);
typedef int64_t (*ffi_func_3)(int64_t, int64_t, int64_t);
typedef int64_t (*ffi_func_4)(int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*ffi_func_5)(int64_t, int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*ffi_func_6)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*ffi_func_7)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*ffi_func_8)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*ffi_func_9)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);
typedef int64_t (*ffi_func_10)(int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t, int64_t);

typedef double (*ffi_func_d_0)(void);
typedef double (*ffi_func_d_1)(double);
typedef double (*ffi_func_d_2)(double, double);
typedef double (*ffi_func_d_3)(double, double, double);
typedef double (*ffi_func_d_4)(double, double, double, double);
typedef double (*ffi_func_d_5)(double, double, double, double, double);

typedef char* (*ffi_func_s_1)(const char*);
typedef char* (*ffi_func_s_2)(const char*, const char*);
typedef char* (*ffi_func_s_3)(const char*, const char*, const char*);
typedef char* (*ffi_func_s_4)(const char*, const char*, const char*, const char*);
typedef char* (*ffi_func_s_5)(const char*, const char*, const char*, const char*, const char*);

/* dl_call_int - call function pointer with int args, returns int */
StradaValue* strada_dl_call_int(StradaValue *func_ptr, StradaValue *args) {
    if (!func_ptr) return strada_new_int(0);
    void *fn = (void*)(intptr_t)strada_to_int(func_ptr);
    if (!fn) return strada_new_int(0);

    int arg_count = 0;
    int64_t a[10] = {0};

    /* Dereference if we got a reference to an array */
    StradaValue *arr = args;
    if (arr && arr->type == STRADA_REF) {
        arr = arr->value.rv;
    }

    if (arr && arr->type == STRADA_ARRAY) {
        arg_count = strada_array_length(arr->value.av);
        for (int i = 0; i < arg_count && i < 10; i++) {
            StradaValue *v = strada_array_get(arr->value.av, i);
            a[i] = strada_to_int(v);
        }
    } else if (arr && arr->type != STRADA_UNDEF) {
        /* Single scalar value - treat as 1 argument */
        arg_count = 1;
        a[0] = strada_to_int(arr);
    }

    int64_t result = 0;
    switch (arg_count) {
        case 0: result = ((ffi_func_0)fn)(); break;
        case 1: result = ((ffi_func_1)fn)(a[0]); break;
        case 2: result = ((ffi_func_2)fn)(a[0], a[1]); break;
        case 3: result = ((ffi_func_3)fn)(a[0], a[1], a[2]); break;
        case 4: result = ((ffi_func_4)fn)(a[0], a[1], a[2], a[3]); break;
        case 5: result = ((ffi_func_5)fn)(a[0], a[1], a[2], a[3], a[4]); break;
        case 6: result = ((ffi_func_6)fn)(a[0], a[1], a[2], a[3], a[4], a[5]); break;
        case 7: result = ((ffi_func_7)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6]); break;
        case 8: result = ((ffi_func_8)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]); break;
        case 9: result = ((ffi_func_9)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]); break;
        default: result = ((ffi_func_10)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9]); break;
    }
    return strada_new_int(result);
}

/* dl_call_num - call function pointer with num args, returns num */
StradaValue* strada_dl_call_num(StradaValue *func_ptr, StradaValue *args) {
    if (!func_ptr) return strada_new_num(0.0);
    void *fn = (void*)(intptr_t)strada_to_int(func_ptr);
    if (!fn) return strada_new_num(0.0);

    int arg_count = 0;
    double a[5] = {0};

    /* Dereference if we got a reference to an array */
    StradaValue *arr = args;
    if (arr && arr->type == STRADA_REF) {
        arr = arr->value.rv;
    }

    if (arr && arr->type == STRADA_ARRAY) {
        arg_count = strada_array_length(arr->value.av);
        for (int i = 0; i < arg_count && i < 5; i++) {
            StradaValue *v = strada_array_get(arr->value.av, i);
            a[i] = strada_to_num(v);
        }
    } else if (arr && arr->type != STRADA_UNDEF) {
        /* Single scalar value - treat as 1 argument */
        arg_count = 1;
        a[0] = strada_to_num(arr);
    }

    double result = 0.0;
    switch (arg_count) {
        case 0: result = ((ffi_func_d_0)fn)(); break;
        case 1: result = ((ffi_func_d_1)fn)(a[0]); break;
        case 2: result = ((ffi_func_d_2)fn)(a[0], a[1]); break;
        case 3: result = ((ffi_func_d_3)fn)(a[0], a[1], a[2]); break;
        case 4: result = ((ffi_func_d_4)fn)(a[0], a[1], a[2], a[3]); break;
        default: result = ((ffi_func_d_5)fn)(a[0], a[1], a[2], a[3], a[4]); break;
    }
    return strada_new_num(result);
}

/* dl_call_str - call function with string arg, returns string */
StradaValue* strada_dl_call_str(StradaValue *func_ptr, StradaValue *arg) {
    if (!func_ptr) return strada_new_str("");
    void *fn = (void*)(intptr_t)strada_to_int(func_ptr);
    if (!fn) return strada_new_str("");

    char _tb[256];
    const char *s = arg ? strada_to_str_buf(arg, _tb, sizeof(_tb)) : NULL;
    char *result = ((ffi_func_s_1)fn)(s ? s : "");
    if (result) {
        return strada_new_str(result);
    }
    return strada_new_str("");
}

/* dl_call_void - call function pointer with no return value */
StradaValue* strada_dl_call_void(StradaValue *func_ptr, StradaValue *args) {
    if (!func_ptr) return strada_new_undef();
    void *fn = (void*)(intptr_t)strada_to_int(func_ptr);
    if (!fn) return strada_new_undef();

    int arg_count = 0;
    int64_t a[10] = {0};

    /* Dereference if we got a reference to an array */
    StradaValue *arr = args;
    if (arr && arr->type == STRADA_REF) {
        arr = arr->value.rv;
    }

    if (arr && arr->type == STRADA_ARRAY) {
        arg_count = strada_array_length(arr->value.av);
        for (int i = 0; i < arg_count && i < 10; i++) {
            StradaValue *v = strada_array_get(arr->value.av, i);
            a[i] = strada_to_int(v);
        }
    } else if (arr && arr->type != STRADA_UNDEF) {
        /* Single scalar value - treat as 1 argument */
        arg_count = 1;
        a[0] = strada_to_int(arr);
    }

    switch (arg_count) {
        case 0: ((ffi_func_0)fn)(); break;
        case 1: ((ffi_func_1)fn)(a[0]); break;
        case 2: ((ffi_func_2)fn)(a[0], a[1]); break;
        case 3: ((ffi_func_3)fn)(a[0], a[1], a[2]); break;
        case 4: ((ffi_func_4)fn)(a[0], a[1], a[2], a[3]); break;
        case 5: ((ffi_func_5)fn)(a[0], a[1], a[2], a[3], a[4]); break;
        case 6: ((ffi_func_6)fn)(a[0], a[1], a[2], a[3], a[4], a[5]); break;
        case 7: ((ffi_func_7)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6]); break;
        case 8: ((ffi_func_8)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]); break;
        case 9: ((ffi_func_9)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]); break;
        default: ((ffi_func_10)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9]); break;
    }
    return strada_new_undef();
}

/* ============================================================
 * StradaValue* FFI functions
 * These pass StradaValue* pointers directly to C functions,
 * allowing the C code to extract strings and other types itself.
 * ============================================================ */

/* Function pointer types for StradaValue* arguments - supports up to 10 arguments */
typedef int64_t (*ffi_sv_0)(void);
typedef int64_t (*ffi_sv_1)(StradaValue*);
typedef int64_t (*ffi_sv_2)(StradaValue*, StradaValue*);
typedef int64_t (*ffi_sv_3)(StradaValue*, StradaValue*, StradaValue*);
typedef int64_t (*ffi_sv_4)(StradaValue*, StradaValue*, StradaValue*, StradaValue*);
typedef int64_t (*ffi_sv_5)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);
typedef int64_t (*ffi_sv_6)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);
typedef int64_t (*ffi_sv_7)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);
typedef int64_t (*ffi_sv_8)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);
typedef int64_t (*ffi_sv_9)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);
typedef int64_t (*ffi_sv_10)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);

typedef char* (*ffi_sv_str_0)(void);
typedef char* (*ffi_sv_str_1)(StradaValue*);
typedef char* (*ffi_sv_str_2)(StradaValue*, StradaValue*);
typedef char* (*ffi_sv_str_3)(StradaValue*, StradaValue*, StradaValue*);
typedef char* (*ffi_sv_str_4)(StradaValue*, StradaValue*, StradaValue*, StradaValue*);
typedef char* (*ffi_sv_str_5)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);
typedef char* (*ffi_sv_str_6)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);
typedef char* (*ffi_sv_str_7)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);
typedef char* (*ffi_sv_str_8)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);
typedef char* (*ffi_sv_str_9)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);
typedef char* (*ffi_sv_str_10)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);

typedef void (*ffi_sv_void_0)(void);
typedef void (*ffi_sv_void_1)(StradaValue*);
typedef void (*ffi_sv_void_2)(StradaValue*, StradaValue*);
typedef void (*ffi_sv_void_3)(StradaValue*, StradaValue*, StradaValue*);
typedef void (*ffi_sv_void_4)(StradaValue*, StradaValue*, StradaValue*, StradaValue*);
typedef void (*ffi_sv_void_5)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);
typedef void (*ffi_sv_void_6)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);
typedef void (*ffi_sv_void_7)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);
typedef void (*ffi_sv_void_8)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);
typedef void (*ffi_sv_void_9)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);
typedef void (*ffi_sv_void_10)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);

typedef StradaValue* (*ffi_sv_sv_0)(void);
typedef StradaValue* (*ffi_sv_sv_1)(StradaValue*);
typedef StradaValue* (*ffi_sv_sv_2)(StradaValue*, StradaValue*);
typedef StradaValue* (*ffi_sv_sv_3)(StradaValue*, StradaValue*, StradaValue*);
typedef StradaValue* (*ffi_sv_sv_4)(StradaValue*, StradaValue*, StradaValue*, StradaValue*);
typedef StradaValue* (*ffi_sv_sv_5)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);
typedef StradaValue* (*ffi_sv_sv_6)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);
typedef StradaValue* (*ffi_sv_sv_7)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);
typedef StradaValue* (*ffi_sv_sv_8)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);
typedef StradaValue* (*ffi_sv_sv_9)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);
typedef StradaValue* (*ffi_sv_sv_10)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);

/* dl_call_int_sv - call function passing StradaValue* directly, returns int */
StradaValue* strada_dl_call_int_sv(StradaValue *func_ptr, StradaValue *args) {
    if (!func_ptr) return strada_new_int(0);
    void *fn = (void*)(intptr_t)strada_to_int(func_ptr);
    if (!fn) return strada_new_int(0);

    int arg_count = 0;
    StradaValue *a[10] = {NULL};

    /* Dereference if we got a reference to an array */
    StradaValue *arr = args;
    if (arr && arr->type == STRADA_REF) {
        arr = arr->value.rv;
    }

    if (arr && arr->type == STRADA_ARRAY) {
        arg_count = strada_array_length(arr->value.av);
        for (int i = 0; i < arg_count && i < 10; i++) {
            a[i] = strada_array_get(arr->value.av, i);
        }
    } else if (arr && arr->type != STRADA_UNDEF) {
        /* Single scalar value - treat as 1 argument */
        arg_count = 1;
        a[0] = arr;
    }

    int64_t result = 0;
    switch (arg_count) {
        case 0: result = ((ffi_sv_0)fn)(); break;
        case 1: result = ((ffi_sv_1)fn)(a[0]); break;
        case 2: result = ((ffi_sv_2)fn)(a[0], a[1]); break;
        case 3: result = ((ffi_sv_3)fn)(a[0], a[1], a[2]); break;
        case 4: result = ((ffi_sv_4)fn)(a[0], a[1], a[2], a[3]); break;
        case 5: result = ((ffi_sv_5)fn)(a[0], a[1], a[2], a[3], a[4]); break;
        case 6: result = ((ffi_sv_6)fn)(a[0], a[1], a[2], a[3], a[4], a[5]); break;
        case 7: result = ((ffi_sv_7)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6]); break;
        case 8: result = ((ffi_sv_8)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]); break;
        case 9: result = ((ffi_sv_9)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]); break;
        default: result = ((ffi_sv_10)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9]); break;
    }
    return strada_new_int(result);
}

/* dl_call_str_sv - call function passing StradaValue* directly, returns string */
StradaValue* strada_dl_call_str_sv(StradaValue *func_ptr, StradaValue *args) {
    if (!func_ptr) return strada_new_str("");
    void *fn = (void*)(intptr_t)strada_to_int(func_ptr);
    if (!fn) return strada_new_str("");

    int arg_count = 0;
    StradaValue *a[10] = {NULL};

    /* Dereference if we got a reference to an array */
    StradaValue *arr = args;
    if (arr && arr->type == STRADA_REF) {
        arr = arr->value.rv;
    }

    if (arr && arr->type == STRADA_ARRAY) {
        arg_count = strada_array_length(arr->value.av);
        for (int i = 0; i < arg_count && i < 10; i++) {
            a[i] = strada_array_get(arr->value.av, i);
        }
    } else if (arr && arr->type != STRADA_UNDEF) {
        /* Single scalar value - treat as 1 argument */
        arg_count = 1;
        a[0] = arr;
    }

    char *result = NULL;
    switch (arg_count) {
        case 0: result = ((ffi_sv_str_0)fn)(); break;
        case 1: result = ((ffi_sv_str_1)fn)(a[0]); break;
        case 2: result = ((ffi_sv_str_2)fn)(a[0], a[1]); break;
        case 3: result = ((ffi_sv_str_3)fn)(a[0], a[1], a[2]); break;
        case 4: result = ((ffi_sv_str_4)fn)(a[0], a[1], a[2], a[3]); break;
        case 5: result = ((ffi_sv_str_5)fn)(a[0], a[1], a[2], a[3], a[4]); break;
        case 6: result = ((ffi_sv_str_6)fn)(a[0], a[1], a[2], a[3], a[4], a[5]); break;
        case 7: result = ((ffi_sv_str_7)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6]); break;
        case 8: result = ((ffi_sv_str_8)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]); break;
        case 9: result = ((ffi_sv_str_9)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]); break;
        default: result = ((ffi_sv_str_10)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9]); break;
    }

    if (result) {
        StradaValue *sv = strada_new_str(result);
        /* Don't free result - it may be static or managed by C library */
        return sv;
    }
    return strada_new_str("");
}

/* dl_call_void_sv - call function passing StradaValue* directly, no return */
StradaValue* strada_dl_call_void_sv(StradaValue *func_ptr, StradaValue *args) {
    if (!func_ptr) return strada_new_undef();
    void *fn = (void*)(intptr_t)strada_to_int(func_ptr);
    if (!fn) return strada_new_undef();

    int arg_count = 0;
    StradaValue *a[10] = {NULL};

    /* Dereference if we got a reference to an array */
    StradaValue *arr = args;
    if (arr && arr->type == STRADA_REF) {
        arr = arr->value.rv;
    }

    if (arr && arr->type == STRADA_ARRAY) {
        arg_count = strada_array_length(arr->value.av);
        for (int i = 0; i < arg_count && i < 10; i++) {
            a[i] = strada_array_get(arr->value.av, i);
        }
    } else if (arr && arr->type != STRADA_UNDEF) {
        /* Single scalar value - treat as 1 argument */
        arg_count = 1;
        a[0] = arr;
    }

    switch (arg_count) {
        case 0: ((ffi_sv_void_0)fn)(); break;
        case 1: ((ffi_sv_void_1)fn)(a[0]); break;
        case 2: ((ffi_sv_void_2)fn)(a[0], a[1]); break;
        case 3: ((ffi_sv_void_3)fn)(a[0], a[1], a[2]); break;
        case 4: ((ffi_sv_void_4)fn)(a[0], a[1], a[2], a[3]); break;
        case 5: ((ffi_sv_void_5)fn)(a[0], a[1], a[2], a[3], a[4]); break;
        case 6: ((ffi_sv_void_6)fn)(a[0], a[1], a[2], a[3], a[4], a[5]); break;
        case 7: ((ffi_sv_void_7)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6]); break;
        case 8: ((ffi_sv_void_8)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]); break;
        case 9: ((ffi_sv_void_9)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]); break;
        default: ((ffi_sv_void_10)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9]); break;
    }
    return strada_new_undef();
}

/* dl_call_sv - call function passing StradaValue* directly, returns StradaValue* */
StradaValue* strada_dl_call_sv(StradaValue *func_ptr, StradaValue *args) {
    if (!func_ptr) return strada_new_undef();
    void *fn = (void*)(intptr_t)strada_to_int(func_ptr);
    if (!fn) return strada_new_undef();

    int arg_count = 0;
    StradaValue *a[10] = {NULL};

    /* Dereference if we got a reference to an array */
    StradaValue *arr = args;
    if (arr && arr->type == STRADA_REF) {
        arr = arr->value.rv;
    }

    if (arr && arr->type == STRADA_ARRAY) {
        arg_count = strada_array_length(arr->value.av);
        for (int i = 0; i < arg_count && i < 10; i++) {
            a[i] = strada_array_get(arr->value.av, i);
        }
    } else if (arr && arr->type != STRADA_UNDEF) {
        /* Single scalar value - treat as 1 argument */
        arg_count = 1;
        a[0] = arr;
    }

    StradaValue *result = NULL;
    switch (arg_count) {
        case 0: result = ((ffi_sv_sv_0)fn)(); break;
        case 1: result = ((ffi_sv_sv_1)fn)(a[0]); break;
        case 2: result = ((ffi_sv_sv_2)fn)(a[0], a[1]); break;
        case 3: result = ((ffi_sv_sv_3)fn)(a[0], a[1], a[2]); break;
        case 4: result = ((ffi_sv_sv_4)fn)(a[0], a[1], a[2], a[3]); break;
        case 5: result = ((ffi_sv_sv_5)fn)(a[0], a[1], a[2], a[3], a[4]); break;
        case 6: result = ((ffi_sv_sv_6)fn)(a[0], a[1], a[2], a[3], a[4], a[5]); break;
        case 7: result = ((ffi_sv_sv_7)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6]); break;
        case 8: result = ((ffi_sv_sv_8)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7]); break;
        case 9: result = ((ffi_sv_sv_9)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8]); break;
        default: result = ((ffi_sv_sv_10)fn)(a[0], a[1], a[2], a[3], a[4], a[5], a[6], a[7], a[8], a[9]); break;
    }

    if (result) {
        return result;
    }
    return strada_new_undef();
}

/* ============================================================
 * Pointer access functions for FFI
 * These allow passing Strada variables by reference to C functions
 * ============================================================ */

/* int_ptr - get pointer to an int variable's underlying value */
StradaValue* strada_int_ptr(StradaValue *ref) {
    if (!ref) return strada_new_int(0);

    /* If it's a reference, dereference it first */
    StradaValue *target = ref;
    if (ref->type == STRADA_REF) {
        target = ref->value.rv;
    }

    if (!target || target->type != STRADA_INT) {
        return strada_new_int(0);
    }

    /* Return pointer to the int64_t value */
    return strada_new_int((int64_t)(intptr_t)&target->value.iv);
}

/* num_ptr - get pointer to a num variable's underlying value */
StradaValue* strada_num_ptr(StradaValue *ref) {
    if (!ref) return strada_new_int(0);

    /* If it's a reference, dereference it first */
    StradaValue *target = ref;
    if (ref->type == STRADA_REF) {
        target = ref->value.rv;
    }

    if (!target || target->type != STRADA_NUM) {
        return strada_new_int(0);
    }

    /* Return pointer to the double value */
    return strada_new_int((int64_t)(intptr_t)&target->value.nv);
}

/* str_ptr - get pointer to a string's char data */
StradaValue* strada_str_ptr(StradaValue *ref) {
    if (!ref) return strada_new_int(0);

    /* If it's a reference, dereference it first */
    StradaValue *target = ref;
    if (ref->type == STRADA_REF) {
        target = ref->value.rv;
    }

    if (!target || target->type != STRADA_STR) {
        return strada_new_int(0);
    }

    /* Return the char* pointer itself */
    return strada_new_int((int64_t)(intptr_t)target->value.pv);
}

/* ptr_deref_int - read an int value from a pointer */
StradaValue* strada_ptr_deref_int(StradaValue *ptr) {
    if (!ptr) return strada_new_int(0);
    int64_t *p = (int64_t*)(intptr_t)strada_to_int(ptr);
    if (!p) return strada_new_int(0);
    return strada_new_int(*p);
}

/* ptr_deref_num - read a num value from a pointer */
StradaValue* strada_ptr_deref_num(StradaValue *ptr) {
    if (!ptr) return strada_new_num(0.0);
    double *p = (double*)(intptr_t)strada_to_int(ptr);
    if (!p) return strada_new_num(0.0);
    return strada_new_num(*p);
}

/* ptr_deref_str - read a string from a char* pointer */
StradaValue* strada_ptr_deref_str(StradaValue *ptr) {
    if (!ptr) return strada_new_str("");
    char *p = (char*)(intptr_t)strada_to_int(ptr);
    if (!p) return strada_new_str("");
    return strada_new_str(p);
}

/* ptr_set_int - write an int value to a pointer */
StradaValue* strada_ptr_set_int(StradaValue *ptr, StradaValue *val) {
    if (!ptr || !val) return strada_new_undef();
    int64_t *p = (int64_t*)(intptr_t)strada_to_int(ptr);
    if (!p) return strada_new_undef();
    *p = strada_to_int(val);
    return strada_new_int(*p);
}

/* ptr_set_num - write a num value to a pointer */
StradaValue* strada_ptr_set_num(StradaValue *ptr, StradaValue *val) {
    if (!ptr || !val) return strada_new_undef();
    double *p = (double*)(intptr_t)strada_to_int(ptr);
    if (!p) return strada_new_undef();
    *p = strada_to_num(val);
    return strada_new_num(*p);
}

/* Simple C function call - limited to 5 arguments for now */
StradaValue* strada_c_call(const char *func_name, StradaValue **args, int arg_count) {
    /* This is a simplified FFI for calling standard C library functions */
    /* In a real implementation, you'd use libffi or similar */
    
    /* For now, we'll support a few common C functions */
    if (strcmp(func_name, "strlen") == 0 && arg_count == 1) {
        char _tb[256];
        const char *str = strada_to_str_buf(args[0], _tb, sizeof(_tb));
        size_t len = strlen(str);
        return strada_new_int(len);
    }
    else if (strcmp(func_name, "getpid") == 0 && arg_count == 0) {
        return strada_new_int(getpid());
    }
    else if (strcmp(func_name, "sleep") == 0 && arg_count == 1) {
        unsigned int seconds = (unsigned int)strada_to_int(args[0]);
        unsigned int result = sleep(seconds);
        return strada_new_int(result);
    }
    else if (strcmp(func_name, "system") == 0 && arg_count == 1) {
        char _tb[256];
        const char *cmd = strada_to_str_buf(args[0], _tb, sizeof(_tb));
        int result = system(cmd);
        return strada_new_int(result);
    }
    
    /* Unknown function */
    return strada_new_undef();
}

/* ===== STACK TRACE / DEBUGGING ===== */

void strada_stacktrace(void) {
    void *array[20];
    size_t size;
    char **strings;
    
    size = backtrace(array, 20);
    strings = backtrace_symbols(array, size);
    
    fprintf(stderr, "\nStack trace:\n");
    fprintf(stderr, "============\n");
    
    for (size_t i = 0; i < size; i++) {
        fprintf(stderr, "#%zu  %s\n", i, strings[i]);
    }
    
    free(strings);
}

void strada_backtrace(void) {
    /* Alias for stacktrace */
    strada_stacktrace();
}

/* Return stack trace as a string */
char* strada_stacktrace_str(void) {
    void *array[20];
    size_t size;
    char **strings;

    size = backtrace(array, 20);
    strings = backtrace_symbols(array, size);

    /* Calculate total size needed */
    size_t total_len = 0;
    for (size_t i = 0; i < size; i++) {
        total_len += strlen(strings[i]) + 16; /* room for "#N  " and newline */
    }
    total_len += 32; /* header */

    char *result = malloc(total_len + 1);
    if (!result) {
        free(strings);
        return strdup("Stack trace: (allocation failed)");
    }

    char *p = result;
    p += sprintf(p, "Stack trace:\n");

    for (size_t i = 0; i < size; i++) {
        p += sprintf(p, "#%zu  %s\n", i, strings[i]);
    }

    free(strings);
    return result;
}

const char* strada_caller(int level) {
    void *array[20];
    size_t size;
    char **strings;
    
    size = backtrace(array, 20);
    
    if (level >= 0 && (size_t)level < size) {
        strings = backtrace_symbols(array, size);
        static char result[256];
        snprintf(result, sizeof(result), "%s", strings[level]);
        free(strings);
        return result;
    }
    
    return "unknown";
}

/* ===== C TYPE CONVERSIONS ===== */

int8_t strada_to_int8(StradaValue *sv) {
    return (int8_t)strada_to_int(sv);
}

int16_t strada_to_int16(StradaValue *sv) {
    return (int16_t)strada_to_int(sv);
}

int32_t strada_to_int32(StradaValue *sv) {
    return (int32_t)strada_to_int(sv);
}

uint8_t strada_to_uint8(StradaValue *sv) {
    return (uint8_t)strada_to_int(sv);
}

uint16_t strada_to_uint16(StradaValue *sv) {
    return (uint16_t)strada_to_int(sv);
}

uint32_t strada_to_uint32(StradaValue *sv) {
    return (uint32_t)strada_to_int(sv);
}

uint64_t strada_to_uint64(StradaValue *sv) {
    return (uint64_t)strada_to_int(sv);
}

float strada_to_float(StradaValue *sv) {
    return (float)strada_to_num(sv);
}

void* strada_to_pointer(StradaValue *sv) {
    if (!sv) return NULL;
    if (sv->type == STRADA_CPOINTER || sv->type == STRADA_CSTRUCT) {
        return sv->value.ptr;
    }
    return NULL;
}

StradaValue* strada_from_int8(int8_t val) {
    return strada_new_int(val);
}

StradaValue* strada_from_int16(int16_t val) {
    return strada_new_int(val);
}

StradaValue* strada_from_int32(int32_t val) {
    return strada_new_int(val);
}

StradaValue* strada_from_uint8(uint8_t val) {
    return strada_new_int(val);
}

StradaValue* strada_from_uint16(uint16_t val) {
    return strada_new_int(val);
}

StradaValue* strada_from_uint32(uint32_t val) {
    return strada_new_int(val);
}

StradaValue* strada_from_uint64(uint64_t val) {
    return strada_new_int(val);
}

StradaValue* strada_from_float(float val) {
    return strada_new_num(val);
}

StradaValue* strada_from_pointer(void *ptr) {
    return strada_cpointer_new(ptr);
}

/* ===== C STRUCT SUPPORT ===== */

StradaValue* strada_cstruct_new(const char *struct_name, size_t size) {
    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_CSTRUCT;
    sv->refcount = 1;
    sv->value.ptr = calloc(1, size);  /* Allocate and zero the struct */
    if (struct_name) strada_ensure_meta(sv)->struct_name = strdup(struct_name);
    sv->struct_size = size;
    return sv;
}

void* strada_cstruct_ptr(StradaValue *sv) {
    if (sv && sv->type == STRADA_CSTRUCT) {
        return sv->value.ptr;
    }
    return NULL;
}

void strada_cstruct_set_field(StradaValue *sv, const char *field, size_t offset, void *value, size_t size) {
    (void)field;  /* Reserved for future field name validation */
    if (!sv || sv->type != STRADA_CSTRUCT || !sv->value.ptr) return;
    if (offset + size > sv->struct_size) return;  /* Safety check */
    
    memcpy((char*)sv->value.ptr + offset, value, size);
}

void* strada_cstruct_get_field(StradaValue *sv, const char *field, size_t offset, size_t size) {
    (void)field;  /* Reserved for future field name validation */
    if (!sv || sv->type != STRADA_CSTRUCT || !sv->value.ptr) return NULL;
    if (offset + size > sv->struct_size) return NULL;  /* Safety check */
    
    return (char*)sv->value.ptr + offset;
}

/* ===== C POINTER SUPPORT ===== */

StradaValue* strada_cpointer_new(void *ptr) {
    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_CPOINTER;
    sv->refcount = 1;
    sv->value.ptr = ptr;
    sv->struct_size = 0;
    return sv;
}

void* strada_cpointer_get(StradaValue *sv) {
    if (sv && sv->type == STRADA_CPOINTER) {
        return sv->value.ptr;
    }
    return NULL;
}

/* ===== CLOSURE SUPPORT ===== */

StradaValue* strada_closure_new(void *func, int params, int captures, StradaValue ***cap_array) {
    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_CLOSURE;
    sv->refcount = 1;
    sv->struct_size = 0;

    StradaClosure *cl = malloc(sizeof(StradaClosure));
    cl->func_ptr = func;
    cl->param_count = params;
    cl->capture_count = captures;

    /* Make a deep copy of captures to ensure thread safety.
     * The original cap_array contains pointers to stack variables which may
     * go out of scope before the closure runs (especially in threads). */
    if (captures > 0 && cap_array) {
        cl->captures = malloc(sizeof(StradaValue**) * captures);
        for (int i = 0; i < captures; i++) {
            /* Allocate storage for the captured value pointer */
            cl->captures[i] = malloc(sizeof(StradaValue*));
            /* Copy the current value and increment refcount */
            *(cl->captures[i]) = *(cap_array[i]);
            if (*(cl->captures[i])) {
                strada_incref(*(cl->captures[i]));
            }
        }
    } else {
        cl->captures = NULL;
    }

    sv->value.ptr = cl;
    return sv;
}

StradaValue*** strada_closure_get_captures(StradaValue *closure) {
    if (!closure || closure->type != STRADA_CLOSURE) return NULL;
    StradaClosure *cl = (StradaClosure*)closure->value.ptr;
    return cl->captures;
}

/* Closure call - handles up to 10 arguments
 * Also handles plain function pointers (STRADA_CPOINTER) for compatibility */
StradaValue* strada_closure_call(StradaValue *closure, int argc, ...) {
    if (!closure) return strada_new_undef();

    va_list args;
    va_start(args, argc);

    /* Collect arguments into array */
    StradaValue *argv[10];
    for (int i = 0; i < argc && i < 10; i++) {
        argv[i] = va_arg(args, StradaValue*);
    }
    va_end(args);

    StradaValue *result;

    /* Handle plain function pointers (from \&func syntax) */
    if (closure->type == STRADA_CPOINTER) {
        void *func_ptr = closure->value.ptr;
        /* Plain function pointer types - no captures parameter */
        typedef StradaValue* (*PlainFunc0)(void);
        typedef StradaValue* (*PlainFunc1)(StradaValue*);
        typedef StradaValue* (*PlainFunc2)(StradaValue*, StradaValue*);
        typedef StradaValue* (*PlainFunc3)(StradaValue*, StradaValue*, StradaValue*);
        typedef StradaValue* (*PlainFunc4)(StradaValue*, StradaValue*, StradaValue*, StradaValue*);
        typedef StradaValue* (*PlainFunc5)(StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);

        switch (argc) {
            case 0:
                result = ((PlainFunc0)func_ptr)();
                break;
            case 1:
                result = ((PlainFunc1)func_ptr)(argv[0]);
                break;
            case 2:
                result = ((PlainFunc2)func_ptr)(argv[0], argv[1]);
                break;
            case 3:
                result = ((PlainFunc3)func_ptr)(argv[0], argv[1], argv[2]);
                break;
            case 4:
                result = ((PlainFunc4)func_ptr)(argv[0], argv[1], argv[2], argv[3]);
                break;
            case 5:
                result = ((PlainFunc5)func_ptr)(argv[0], argv[1], argv[2], argv[3], argv[4]);
                break;
            default:
                result = strada_new_undef();
                break;
        }
        return result;
    }

    /* Handle closures */
    if (closure->type != STRADA_CLOSURE) return strada_new_undef();

    StradaClosure *cl = (StradaClosure*)closure->value.ptr;

    /* Function pointer types for different arities (triple pointer for capture-by-reference) */
    typedef StradaValue* (*Func0)(StradaValue***);
    typedef StradaValue* (*Func1)(StradaValue***, StradaValue*);
    typedef StradaValue* (*Func2)(StradaValue***, StradaValue*, StradaValue*);
    typedef StradaValue* (*Func3)(StradaValue***, StradaValue*, StradaValue*, StradaValue*);
    typedef StradaValue* (*Func4)(StradaValue***, StradaValue*, StradaValue*, StradaValue*, StradaValue*);
    typedef StradaValue* (*Func5)(StradaValue***, StradaValue*, StradaValue*, StradaValue*, StradaValue*, StradaValue*);

    switch (argc) {
        case 0:
            result = ((Func0)cl->func_ptr)(cl->captures);
            break;
        case 1:
            result = ((Func1)cl->func_ptr)(cl->captures, argv[0]);
            break;
        case 2:
            result = ((Func2)cl->func_ptr)(cl->captures, argv[0], argv[1]);
            break;
        case 3:
            result = ((Func3)cl->func_ptr)(cl->captures, argv[0], argv[1], argv[2]);
            break;
        case 4:
            result = ((Func4)cl->func_ptr)(cl->captures, argv[0], argv[1], argv[2], argv[3]);
            break;
        case 5:
            result = ((Func5)cl->func_ptr)(cl->captures, argv[0], argv[1], argv[2], argv[3], argv[4]);
            break;
        default:
            result = strada_new_undef();
            break;
    }

    return result;
}

/* ===== THREAD SUPPORT ===== */

/* Thread wrapper that calls Strada closure */
static void* strada_thread_wrapper(void *arg) {
    StradaThread *st = (StradaThread *)arg;
    /* Call the closure with 0 arguments */
    st->result = strada_closure_call(st->closure, 0);
    /* If detached, clean up our own resources since no one will join us */
    if (st->detached) {
        if (st->result) strada_decref(st->result);
        strada_decref(st->closure);
        free(st);
        return NULL;
    }
    return NULL;
}

StradaValue* strada_thread_create(StradaValue *closure) {
    if (!closure || closure->type != STRADA_CLOSURE) {
        return strada_new_int(-1);
    }

    StradaThread *st = malloc(sizeof(StradaThread));
    st->closure = closure;
    st->result = NULL;
    st->detached = 0;
    strada_incref(closure);  /* Keep closure alive */

    int rc = pthread_create(&st->thread, NULL, strada_thread_wrapper, st);
    if (rc != 0) {
        strada_decref(closure);
        free(st);
        return strada_new_int(-1);
    }

    /* Return thread handle as a pointer wrapped in StradaValue */
    return strada_cpointer_new(st);
}

StradaValue* strada_thread_join(StradaValue *thread_val) {
    if (!thread_val || thread_val->type != STRADA_CPOINTER) {
        return strada_new_undef();
    }

    StradaThread *st = (StradaThread *)thread_val->value.ptr;
    if (!st) return strada_new_undef();

    int rc = pthread_join(st->thread, NULL);
    if (rc != 0) {
        return strada_new_undef();
    }

    StradaValue *result = st->result ? st->result : strada_new_undef();
    strada_decref(st->closure);
    free(st);
    thread_val->value.ptr = NULL;  /* Mark as consumed */

    return result;
}

StradaValue* strada_thread_detach(StradaValue *thread_val) {
    if (!thread_val || thread_val->type != STRADA_CPOINTER) {
        return strada_new_int(-1);
    }

    StradaThread *st = (StradaThread *)thread_val->value.ptr;
    if (!st) return strada_new_int(-1);

    st->detached = 1;
    int rc = pthread_detach(st->thread);
    thread_val->value.ptr = NULL;  /* Ownership transferred to thread for self-cleanup */
    return strada_new_int(rc);
}

StradaValue* strada_thread_self(void) {
    pthread_t self = pthread_self();
    return strada_new_int((int64_t)self);
}

/* ===== MUTEX SUPPORT ===== */

StradaValue* strada_mutex_new(void) {
    StradaMutex *m = malloc(sizeof(StradaMutex));
    pthread_mutex_init(&m->mutex, NULL);
    StradaValue *sv = strada_cpointer_new(m);
    strada_ensure_meta(sv)->struct_name = strdup("Mutex");
    return sv;
}

StradaValue* strada_mutex_lock(StradaValue *mutex_val) {
    if (!mutex_val || mutex_val->type != STRADA_CPOINTER) {
        return strada_new_int(-1);
    }
    StradaMutex *m = (StradaMutex *)mutex_val->value.ptr;
    if (!m) return strada_new_int(-1);
    int rc = pthread_mutex_lock(&m->mutex);
    return strada_new_int(rc);
}

StradaValue* strada_mutex_trylock(StradaValue *mutex_val) {
    if (!mutex_val || mutex_val->type != STRADA_CPOINTER) {
        return strada_new_int(-1);
    }
    StradaMutex *m = (StradaMutex *)mutex_val->value.ptr;
    if (!m) return strada_new_int(-1);
    int rc = pthread_mutex_trylock(&m->mutex);
    return strada_new_int(rc);  /* 0 = success, EBUSY = already locked */
}

StradaValue* strada_mutex_unlock(StradaValue *mutex_val) {
    if (!mutex_val || mutex_val->type != STRADA_CPOINTER) {
        return strada_new_int(-1);
    }
    StradaMutex *m = (StradaMutex *)mutex_val->value.ptr;
    if (!m) return strada_new_int(-1);
    int rc = pthread_mutex_unlock(&m->mutex);
    return strada_new_int(rc);
}

StradaValue* strada_mutex_destroy(StradaValue *mutex_val) {
    if (!mutex_val || mutex_val->type != STRADA_CPOINTER) {
        return strada_new_int(-1);
    }
    StradaMutex *m = (StradaMutex *)mutex_val->value.ptr;
    if (!m) return strada_new_int(-1);
    int rc = pthread_mutex_destroy(&m->mutex);
    free(m);
    mutex_val->value.ptr = NULL;  /* Mark as destroyed */
    return strada_new_int(rc);
}

/* ===== CONDITION VARIABLE SUPPORT ===== */

StradaValue* strada_cond_new(void) {
    StradaCond *c = malloc(sizeof(StradaCond));
    pthread_cond_init(&c->cond, NULL);
    StradaValue *sv = strada_cpointer_new(c);
    strada_ensure_meta(sv)->struct_name = strdup("Cond");
    return sv;
}

StradaValue* strada_cond_wait(StradaValue *cond_val, StradaValue *mutex_val) {
    if (!cond_val || cond_val->type != STRADA_CPOINTER ||
        !mutex_val || mutex_val->type != STRADA_CPOINTER) {
        return strada_new_int(-1);
    }
    StradaCond *c = (StradaCond *)cond_val->value.ptr;
    StradaMutex *m = (StradaMutex *)mutex_val->value.ptr;
    if (!c || !m) return strada_new_int(-1);
    int rc = pthread_cond_wait(&c->cond, &m->mutex);
    return strada_new_int(rc);
}

StradaValue* strada_cond_signal(StradaValue *cond_val) {
    if (!cond_val || cond_val->type != STRADA_CPOINTER) {
        return strada_new_int(-1);
    }
    StradaCond *c = (StradaCond *)cond_val->value.ptr;
    if (!c) return strada_new_int(-1);
    int rc = pthread_cond_signal(&c->cond);
    return strada_new_int(rc);
}

StradaValue* strada_cond_broadcast(StradaValue *cond_val) {
    if (!cond_val || cond_val->type != STRADA_CPOINTER) {
        return strada_new_int(-1);
    }
    StradaCond *c = (StradaCond *)cond_val->value.ptr;
    if (!c) return strada_new_int(-1);
    int rc = pthread_cond_broadcast(&c->cond);
    return strada_new_int(rc);
}

StradaValue* strada_cond_destroy(StradaValue *cond_val) {
    if (!cond_val || cond_val->type != STRADA_CPOINTER) {
        return strada_new_int(-1);
    }
    StradaCond *c = (StradaCond *)cond_val->value.ptr;
    if (!c) return strada_new_int(-1);
    int rc = pthread_cond_destroy(&c->cond);
    free(c);
    cond_val->value.ptr = NULL;  /* Mark as destroyed */
    return strada_new_int(rc);
}

/* ===== ASYNC/AWAIT - THREAD POOL SUPPORT ===== */

/* Global thread pool */
StradaThreadPool *strada_thread_pool = NULL;

/* Default pool size */
#define STRADA_DEFAULT_POOL_SIZE 4

/* Worker thread function */
static void* strada_pool_worker(void *arg) {
    StradaThreadPool *pool = (StradaThreadPool *)arg;

    while (1) {
        StradaTask *task = NULL;

        /* Get next task from queue */
        pthread_mutex_lock(&pool->queue_mutex);
        while (pool->running && pool->queue_head == NULL) {
            pthread_cond_wait(&pool->queue_cond, &pool->queue_mutex);
        }

        if (!pool->running && pool->queue_head == NULL) {
            pthread_mutex_unlock(&pool->queue_mutex);
            break;  /* Shutdown */
        }

        /* Dequeue task */
        task = pool->queue_head;
        if (task) {
            pool->queue_head = task->next;
            if (pool->queue_head == NULL) {
                pool->queue_tail = NULL;
            }
            pool->queue_size--;
        }
        pthread_mutex_unlock(&pool->queue_mutex);

        if (task) {
            StradaFuture *f = task->future;

            /* Check if cancelled before starting */
            pthread_mutex_lock(&f->mutex);
            if (f->cancel_requested) {
                f->state = FUTURE_CANCELLED;
                pthread_cond_broadcast(&f->cond);
                pthread_mutex_unlock(&f->mutex);
                free(task);
                continue;
            }
            f->state = FUTURE_RUNNING;
            pthread_mutex_unlock(&f->mutex);

            /* Execute with exception handling */
            /* Use volatile to prevent issues with setjmp/longjmp */
            StradaValue * volatile result = NULL;
            StradaValue * volatile error = NULL;

            if (setjmp(*STRADA_TRY_PUSH()) == 0) {
                result = strada_closure_call(f->closure, 0);
                STRADA_TRY_POP();
            } else {
                STRADA_TRY_POP();
                error = strada_get_exception();
            }

            /* Store result */
            pthread_mutex_lock(&f->mutex);
            if (f->cancel_requested) {
                f->state = FUTURE_CANCELLED;
                if (result) strada_decref((StradaValue*)result);
                if (error) strada_decref((StradaValue*)error);
            } else {
                f->result = (StradaValue*)result;
                f->error = (StradaValue*)error;
                f->state = FUTURE_COMPLETED;
            }
            pthread_cond_broadcast(&f->cond);
            pthread_mutex_unlock(&f->mutex);

            free(task);
        }
    }

    return NULL;
}

/* Initialize thread pool */
void strada_pool_init(int num_workers) {
    if (strada_thread_pool != NULL) return;  /* Already initialized */

    /* Enable atomic refcounting now that we're multi-threaded */
    strada_threading_active = 1;

    if (num_workers <= 0) {
        num_workers = STRADA_DEFAULT_POOL_SIZE;
    }

    StradaThreadPool *pool = malloc(sizeof(StradaThreadPool));
    pool->worker_count = num_workers;
    pool->running = 1;
    pool->queue_head = NULL;
    pool->queue_tail = NULL;
    pool->queue_size = 0;

    pthread_mutex_init(&pool->queue_mutex, NULL);
    pthread_cond_init(&pool->queue_cond, NULL);
    pthread_cond_init(&pool->empty_cond, NULL);

    pool->workers = malloc(sizeof(pthread_t) * num_workers);
    for (int i = 0; i < num_workers; i++) {
        pthread_create(&pool->workers[i], NULL, strada_pool_worker, pool);
    }

    strada_thread_pool = pool;
}

/* Shutdown thread pool */
void strada_pool_shutdown(void) {
    if (strada_thread_pool == NULL) return;

    StradaThreadPool *pool = strada_thread_pool;

    pthread_mutex_lock(&pool->queue_mutex);
    pool->running = 0;
    pthread_cond_broadcast(&pool->queue_cond);  /* Wake all workers */
    pthread_mutex_unlock(&pool->queue_mutex);

    /* Wait for all workers to finish */
    for (int i = 0; i < pool->worker_count; i++) {
        pthread_join(pool->workers[i], NULL);
    }

    /* Cleanup */
    pthread_mutex_destroy(&pool->queue_mutex);
    pthread_cond_destroy(&pool->queue_cond);
    pthread_cond_destroy(&pool->empty_cond);
    free(pool->workers);
    free(pool);
    strada_thread_pool = NULL;
}

/* Submit task to pool */
void strada_pool_submit(StradaFuture *future) {
    /* Auto-initialize pool if needed */
    if (strada_thread_pool == NULL) {
        strada_pool_init(STRADA_DEFAULT_POOL_SIZE);
    }

    StradaThreadPool *pool = strada_thread_pool;

    StradaTask *task = malloc(sizeof(StradaTask));
    task->closure = future->closure;
    task->future = future;
    task->next = NULL;

    pthread_mutex_lock(&pool->queue_mutex);
    if (pool->queue_tail) {
        pool->queue_tail->next = task;
        pool->queue_tail = task;
    } else {
        pool->queue_head = task;
        pool->queue_tail = task;
    }
    pool->queue_size++;
    pthread_cond_signal(&pool->queue_cond);  /* Wake one worker */
    pthread_mutex_unlock(&pool->queue_mutex);
}

/* Create a new future */
StradaValue* strada_future_new(StradaValue *closure) {
    StradaFuture *f = malloc(sizeof(StradaFuture));
    f->result = NULL;
    f->error = NULL;
    f->closure = closure;
    strada_incref(closure);
    f->state = FUTURE_PENDING;
    f->cancel_requested = 0;
    f->has_deadline = 0;
    memset(&f->deadline, 0, sizeof(f->deadline));

    pthread_mutex_init(&f->mutex, NULL);
    pthread_cond_init(&f->cond, NULL);

    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_FUTURE;
    sv->refcount = 1;
    sv->value.ptr = f;

    /* Submit to thread pool */
    strada_pool_submit(f);

    return sv;
}

/* Await: block until future completes */
StradaValue* strada_future_await(StradaValue *future) {
    /* If not a future, pass through the value as-is (makes await idempotent) */
    if (!future) {
        return strada_new_undef();
    }
    if (future->type != STRADA_FUTURE) {
        strada_incref(future);
        return future;
    }

    StradaFuture *f = (StradaFuture*)future->value.ptr;

    pthread_mutex_lock(&f->mutex);
    while (f->state == FUTURE_PENDING || f->state == FUTURE_RUNNING) {
        pthread_cond_wait(&f->cond, &f->mutex);
    }

    StradaFutureState state = f->state;
    StradaValue *result = f->result;
    StradaValue *error = f->error;
    pthread_mutex_unlock(&f->mutex);

    /* Handle different states */
    if (state == FUTURE_CANCELLED) {
        strada_throw("Future was cancelled");
        return strada_new_undef();
    }

    if (error) {
        strada_throw_value(error);
        return strada_new_undef();
    }

    if (result) {
        strada_incref(result);  /* Return a reference that the caller owns */
    }
    return result ? result : strada_new_undef();
}

/* Await with timeout (throws on timeout) */
StradaValue* strada_future_await_timeout(StradaValue *future, int64_t timeout_ms) {
    /* If not a future, pass through the value as-is */
    if (!future) {
        return strada_new_undef();
    }
    if (future->type != STRADA_FUTURE) {
        strada_incref(future);
        return future;
    }

    StradaFuture *f = (StradaFuture*)future->value.ptr;

    /* Calculate deadline */
    struct timespec deadline;
    clock_gettime(CLOCK_REALTIME, &deadline);
    deadline.tv_sec += timeout_ms / 1000;
    deadline.tv_nsec += (timeout_ms % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }

    pthread_mutex_lock(&f->mutex);
    while (f->state == FUTURE_PENDING || f->state == FUTURE_RUNNING) {
        int rc = pthread_cond_timedwait(&f->cond, &f->mutex, &deadline);
        if (rc == ETIMEDOUT) {
            f->state = FUTURE_TIMEOUT;
            pthread_mutex_unlock(&f->mutex);
            strada_throw("Future timed out");
            return strada_new_undef();
        }
    }

    StradaFutureState state = f->state;
    StradaValue *result = f->result;
    StradaValue *error = f->error;
    pthread_mutex_unlock(&f->mutex);

    if (state == FUTURE_CANCELLED) {
        strada_throw("Future was cancelled");
        return strada_new_undef();
    }

    if (error) {
        strada_throw_value(error);
        return strada_new_undef();
    }

    if (result) {
        strada_incref(result);
    }
    return result ? result : strada_new_undef();
}

/* Check if future is done (non-blocking) */
int strada_future_is_done(StradaValue *future) {
    if (!future || future->type != STRADA_FUTURE) return 1;
    StradaFuture *f = (StradaFuture*)future->value.ptr;
    pthread_mutex_lock(&f->mutex);
    int done = (f->state == FUTURE_COMPLETED ||
                f->state == FUTURE_CANCELLED ||
                f->state == FUTURE_TIMEOUT);
    pthread_mutex_unlock(&f->mutex);
    return done;
}

/* Try to get result (non-blocking, returns undef if not done) */
StradaValue* strada_future_try_get(StradaValue *future) {
    if (!future || future->type != STRADA_FUTURE) {
        return strada_new_undef();
    }
    StradaFuture *f = (StradaFuture*)future->value.ptr;
    pthread_mutex_lock(&f->mutex);
    if (f->state != FUTURE_COMPLETED) {
        pthread_mutex_unlock(&f->mutex);
        return strada_new_undef();
    }
    StradaValue *result = f->result;
    pthread_mutex_unlock(&f->mutex);
    if (result) {
        strada_incref(result);
    }
    return result ? result : strada_new_undef();
}

/* Request cancellation */
void strada_future_cancel(StradaValue *future) {
    if (!future || future->type != STRADA_FUTURE) return;
    StradaFuture *f = (StradaFuture*)future->value.ptr;
    pthread_mutex_lock(&f->mutex);
    f->cancel_requested = 1;
    /* If still pending, mark as cancelled immediately */
    if (f->state == FUTURE_PENDING) {
        f->state = FUTURE_CANCELLED;
        pthread_cond_broadcast(&f->cond);
    }
    pthread_mutex_unlock(&f->mutex);
}

/* Check if cancelled */
int strada_future_is_cancelled(StradaValue *future) {
    if (!future || future->type != STRADA_FUTURE) return 0;
    StradaFuture *f = (StradaFuture*)future->value.ptr;
    pthread_mutex_lock(&f->mutex);
    int cancelled = (f->state == FUTURE_CANCELLED);
    pthread_mutex_unlock(&f->mutex);
    return cancelled;
}

/* Wait for all futures to complete, return array of results */
StradaValue* strada_future_all(StradaValue *futures_ref) {
    if (!futures_ref) return strada_new_array();

    /* Dereference to get the array */
    StradaArray *arr = strada_deref_array(futures_ref);
    if (!arr) {
        return strada_new_array();
    }

    size_t count = arr->size;

    /* Create result array */
    StradaValue *results = strada_new_array();

    /* Await each future in order */
    for (size_t i = 0; i < count; i++) {
        StradaValue *future = arr->elements[arr->head + i];
        StradaValue *result = strada_future_await(future);
        strada_array_push_take(results->value.av, result);
    }

    return results;
}

/* Wait for first future to complete, return its result */
StradaValue* strada_future_race(StradaValue *futures_ref) {
    if (!futures_ref) return strada_new_undef();

    /* Dereference to get the array */
    StradaArray *arr = strada_deref_array(futures_ref);
    if (!arr) {
        return strada_new_undef();
    }

    size_t count = arr->size;

    if (count == 0) return strada_new_undef();

    /* Poll until one completes */
    while (1) {
        for (size_t i = 0; i < count; i++) {
            StradaValue *future = arr->elements[arr->head + i];
            if (strada_future_is_done(future)) {
                /* Cancel the others */
                for (size_t j = 0; j < count; j++) {
                    if (j != i) {
                        strada_future_cancel(arr->elements[arr->head + j]);
                    }
                }
                return strada_future_await(future);
            }
        }
        /* Small sleep to avoid busy-waiting */
        usleep(100);  /* 100 microseconds */
    }
}

/* Await with timeout wrapper for Strada */
StradaValue* strada_async_timeout(StradaValue *future, StradaValue *timeout_ms) {
    int64_t ms = strada_to_int(timeout_ms);
    return strada_future_await_timeout(future, ms);
}

/* ============================================================
 * Channel Implementation - Thread-safe Communication
 * ============================================================ */

/* Create a new channel with optional capacity (0 = unbounded) */
StradaValue* strada_channel_new(int capacity) {
    StradaChannel *ch = malloc(sizeof(StradaChannel));
    if (!ch) return strada_new_undef();

    pthread_mutex_init(&ch->mutex, NULL);
    pthread_cond_init(&ch->not_empty, NULL);
    pthread_cond_init(&ch->not_full, NULL);

    ch->head = NULL;
    ch->tail = NULL;
    ch->size = 0;
    ch->capacity = capacity;  /* 0 means unbounded */
    ch->closed = 0;

    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_CHANNEL;
    sv->refcount = 1;
    sv->value.ptr = ch;

    return sv;
}

/* Send a value to the channel (blocks if full, throws if closed) */
void strada_channel_send(StradaValue *channel, StradaValue *value) {
    if (!channel || channel->type != STRADA_CHANNEL) {
        strada_throw("channel::send: invalid channel");
        return;
    }

    StradaChannel *ch = (StradaChannel*)channel->value.ptr;

    pthread_mutex_lock(&ch->mutex);

    /* Check if closed */
    if (ch->closed) {
        pthread_mutex_unlock(&ch->mutex);
        strada_throw("channel::send: channel is closed");
        return;
    }

    /* Wait if channel is full (bounded channel) */
    while (ch->capacity > 0 && ch->size >= ch->capacity && !ch->closed) {
        pthread_cond_wait(&ch->not_full, &ch->mutex);
    }

    /* Check again after waking (might have been closed) */
    if (ch->closed) {
        pthread_mutex_unlock(&ch->mutex);
        strada_throw("channel::send: channel is closed");
        return;
    }

    /* Create new node */
    StradaChannelNode *node = malloc(sizeof(StradaChannelNode));
    node->value = value;
    strada_incref(value);
    node->next = NULL;

    /* Add to tail */
    if (ch->tail) {
        ch->tail->next = node;
        ch->tail = node;
    } else {
        ch->head = node;
        ch->tail = node;
    }
    ch->size++;

    /* Signal waiting receivers */
    pthread_cond_signal(&ch->not_empty);
    pthread_mutex_unlock(&ch->mutex);
}

/* Receive a value from the channel (blocks if empty, returns undef if closed and empty) */
StradaValue* strada_channel_recv(StradaValue *channel) {
    if (!channel || channel->type != STRADA_CHANNEL) {
        strada_throw("channel::recv: invalid channel");
        return strada_new_undef();
    }

    StradaChannel *ch = (StradaChannel*)channel->value.ptr;

    pthread_mutex_lock(&ch->mutex);

    /* Wait while channel is empty and not closed */
    while (ch->head == NULL && !ch->closed) {
        pthread_cond_wait(&ch->not_empty, &ch->mutex);
    }

    /* If closed and empty, return undef */
    if (ch->head == NULL && ch->closed) {
        pthread_mutex_unlock(&ch->mutex);
        return strada_new_undef();
    }

    /* Dequeue from head */
    StradaChannelNode *node = ch->head;
    ch->head = node->next;
    if (ch->head == NULL) {
        ch->tail = NULL;
    }
    ch->size--;

    StradaValue *value = node->value;
    free(node);

    /* Signal waiting senders */
    pthread_cond_signal(&ch->not_full);
    pthread_mutex_unlock(&ch->mutex);

    return value;
}

/* Try to send without blocking (returns 1 on success, 0 if full/closed) */
int strada_channel_try_send(StradaValue *channel, StradaValue *value) {
    if (!channel || channel->type != STRADA_CHANNEL) {
        return 0;
    }

    StradaChannel *ch = (StradaChannel*)channel->value.ptr;

    pthread_mutex_lock(&ch->mutex);

    /* Can't send if closed */
    if (ch->closed) {
        pthread_mutex_unlock(&ch->mutex);
        return 0;
    }

    /* Can't send if full (bounded channel) */
    if (ch->capacity > 0 && ch->size >= ch->capacity) {
        pthread_mutex_unlock(&ch->mutex);
        return 0;
    }

    /* Create new node */
    StradaChannelNode *node = malloc(sizeof(StradaChannelNode));
    node->value = value;
    strada_incref(value);
    node->next = NULL;

    /* Add to tail */
    if (ch->tail) {
        ch->tail->next = node;
        ch->tail = node;
    } else {
        ch->head = node;
        ch->tail = node;
    }
    ch->size++;

    /* Signal waiting receivers */
    pthread_cond_signal(&ch->not_empty);
    pthread_mutex_unlock(&ch->mutex);

    return 1;
}

/* Try to receive without blocking (returns undef if empty) */
StradaValue* strada_channel_try_recv(StradaValue *channel) {
    if (!channel || channel->type != STRADA_CHANNEL) {
        return strada_new_undef();
    }

    StradaChannel *ch = (StradaChannel*)channel->value.ptr;

    pthread_mutex_lock(&ch->mutex);

    /* Empty? Return undef immediately */
    if (ch->head == NULL) {
        pthread_mutex_unlock(&ch->mutex);
        return strada_new_undef();
    }

    /* Dequeue from head */
    StradaChannelNode *node = ch->head;
    ch->head = node->next;
    if (ch->head == NULL) {
        ch->tail = NULL;
    }
    ch->size--;

    StradaValue *value = node->value;
    free(node);

    /* Signal waiting senders */
    pthread_cond_signal(&ch->not_full);
    pthread_mutex_unlock(&ch->mutex);

    return value;
}

/* Close the channel (no more sends allowed, receivers get remaining items then undef) */
void strada_channel_close(StradaValue *channel) {
    if (!channel || channel->type != STRADA_CHANNEL) {
        return;
    }

    StradaChannel *ch = (StradaChannel*)channel->value.ptr;

    pthread_mutex_lock(&ch->mutex);
    ch->closed = 1;
    /* Wake up all waiting threads */
    pthread_cond_broadcast(&ch->not_empty);
    pthread_cond_broadcast(&ch->not_full);
    pthread_mutex_unlock(&ch->mutex);
}

/* Check if channel is closed */
int strada_channel_is_closed(StradaValue *channel) {
    if (!channel || channel->type != STRADA_CHANNEL) {
        return 1;  /* Treat invalid as closed */
    }

    StradaChannel *ch = (StradaChannel*)channel->value.ptr;

    pthread_mutex_lock(&ch->mutex);
    int closed = ch->closed;
    pthread_mutex_unlock(&ch->mutex);

    return closed;
}

/* Get number of items currently in channel */
int strada_channel_len(StradaValue *channel) {
    if (!channel || channel->type != STRADA_CHANNEL) {
        return 0;
    }

    StradaChannel *ch = (StradaChannel*)channel->value.ptr;

    pthread_mutex_lock(&ch->mutex);
    int size = ch->size;
    pthread_mutex_unlock(&ch->mutex);

    return size;
}

/* ============================================================
 * Atomic Implementation - Lock-free Integer Operations
 * ============================================================ */

/* Create a new atomic integer */
StradaValue* strada_atomic_new(int64_t initial) {
    StradaAtomicValue *a = malloc(sizeof(StradaAtomicValue));
    if (!a) return strada_new_undef();

    a->value = initial;

    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_ATOMIC;
    sv->refcount = 1;
    sv->value.ptr = a;

    return sv;
}

/* Load value atomically */
int64_t strada_atomic_load(StradaValue *atomic) {
    if (!atomic || atomic->type != STRADA_ATOMIC) {
        return 0;
    }

    StradaAtomicValue *a = (StradaAtomicValue*)atomic->value.ptr;
    return __atomic_load_n(&a->value, __ATOMIC_SEQ_CST);
}

/* Store value atomically */
void strada_atomic_store(StradaValue *atomic, int64_t value) {
    if (!atomic || atomic->type != STRADA_ATOMIC) {
        return;
    }

    StradaAtomicValue *a = (StradaAtomicValue*)atomic->value.ptr;
    __atomic_store_n(&a->value, value, __ATOMIC_SEQ_CST);
}

/* Add to atomic and return OLD value */
int64_t strada_atomic_add(StradaValue *atomic, int64_t delta) {
    if (!atomic || atomic->type != STRADA_ATOMIC) {
        return 0;
    }

    StradaAtomicValue *a = (StradaAtomicValue*)atomic->value.ptr;
    return __atomic_fetch_add(&a->value, delta, __ATOMIC_SEQ_CST);
}

/* Subtract from atomic and return OLD value */
int64_t strada_atomic_sub(StradaValue *atomic, int64_t delta) {
    if (!atomic || atomic->type != STRADA_ATOMIC) {
        return 0;
    }

    StradaAtomicValue *a = (StradaAtomicValue*)atomic->value.ptr;
    return __atomic_fetch_sub(&a->value, delta, __ATOMIC_SEQ_CST);
}

/* Compare-and-swap: if value == expected, set to desired. Returns 1 on success */
int strada_atomic_cas(StradaValue *atomic, int64_t expected, int64_t desired) {
    if (!atomic || atomic->type != STRADA_ATOMIC) {
        return 0;
    }

    StradaAtomicValue *a = (StradaAtomicValue*)atomic->value.ptr;
    return __atomic_compare_exchange_n(&a->value, &expected, desired,
                                       0, __ATOMIC_SEQ_CST, __ATOMIC_SEQ_CST);
}

/* Increment and return NEW value */
int64_t strada_atomic_inc(StradaValue *atomic) {
    if (!atomic || atomic->type != STRADA_ATOMIC) {
        return 0;
    }

    StradaAtomicValue *a = (StradaAtomicValue*)atomic->value.ptr;
    return __atomic_add_fetch(&a->value, 1, __ATOMIC_SEQ_CST);
}

/* Decrement and return NEW value */
int64_t strada_atomic_dec(StradaValue *atomic) {
    if (!atomic || atomic->type != STRADA_ATOMIC) {
        return 0;
    }

    StradaAtomicValue *a = (StradaAtomicValue*)atomic->value.ptr;
    return __atomic_sub_fetch(&a->value, 1, __ATOMIC_SEQ_CST);
}

/* ===== C STRUCT SUPPORT ===== */
/* String and type functions for strada_runtime.c - append to end of file */

/* ===== COMPREHENSIVE STRING FUNCTIONS ===== */

/* Returns character position (not byte position) */
int strada_rindex(const char *haystack, const char *needle) {
    if (!haystack || !needle) return -1;

    const char *last_pos = NULL;
    const char *pos = haystack;

    while ((pos = strstr(pos, needle)) != NULL) {
        last_pos = pos;
        pos++;
    }

    if (last_pos) {
        return (int)byte_to_char_offset(haystack, last_pos - haystack);
    }
    return -1;
}

char* strada_upper(const char *str) {
    if (!str) return strdup("");
    
    size_t len = strlen(str);
    char *result = malloc(len + 1);
    
    for (size_t i = 0; i < len; i++) {
        result[i] = toupper((unsigned char)str[i]);
    }
    result[len] = '\0';
    
    return result;
}

char* strada_lower(const char *str) {
    if (!str) return strdup("");
    
    size_t len = strlen(str);
    char *result = malloc(len + 1);
    
    for (size_t i = 0; i < len; i++) {
        result[i] = tolower((unsigned char)str[i]);
    }
    result[len] = '\0';
    
    return result;
}

char* strada_uc(const char *str) {
    return strada_upper(str);
}

char* strada_lc(const char *str) {
    return strada_lower(str);
}

char* strada_ucfirst(const char *str) {
    if (!str || !str[0]) return strdup("");
    
    char *result = strdup(str);
    result[0] = toupper((unsigned char)result[0]);
    return result;
}

char* strada_lcfirst(const char *str) {
    if (!str || !str[0]) return strdup("");
    
    char *result = strdup(str);
    result[0] = tolower((unsigned char)result[0]);
    return result;
}

char* strada_trim(const char *str) {
    if (!str) return strdup("");
    
    while (*str && isspace((unsigned char)*str)) {
        str++;
    }
    
    if (!*str) return strdup("");
    
    const char *end = str + strlen(str) - 1;
    while (end > str && isspace((unsigned char)*end)) {
        end--;
    }
    
    size_t len = end - str + 1;
    char *result = malloc(len + 1);
    memcpy(result, str, len);
    result[len] = '\0';
    
    return result;
}

char* strada_ltrim(const char *str) {
    if (!str) return strdup("");
    
    while (*str && isspace((unsigned char)*str)) {
        str++;
    }
    
    return strdup(str);
}

char* strada_rtrim(const char *str) {
    if (!str) return strdup("");
    
    size_t len = strlen(str);
    if (len == 0) return strdup("");
    
    const char *end = str + len - 1;
    while (end >= str && isspace((unsigned char)*end)) {
        end--;
    }
    
    len = end - str + 1;
    char *result = malloc(len + 1);
    memcpy(result, str, len);
    result[len] = '\0';
    
    return result;
}

/* UTF-8 aware reverse - reverses characters, not bytes */
char* strada_reverse(const char *str) {
    if (!str) return strdup("");

    size_t byte_len = strlen(str);
    size_t char_count = utf8_strlen(str);

    if (char_count == 0) return strdup("");

    /* Collect character byte offsets and lengths */
    size_t *offsets = malloc(char_count * sizeof(size_t));
    size_t *lengths = malloc(char_count * sizeof(size_t));

    const unsigned char *p = (const unsigned char *)str;
    size_t char_idx = 0;
    size_t byte_idx = 0;

    while (*p && char_idx < char_count) {
        offsets[char_idx] = byte_idx;
        int clen = utf8_char_len(*p);
        lengths[char_idx] = clen;
        p += clen;
        byte_idx += clen;
        char_idx++;
    }

    /* Build reversed string */
    char *result = malloc(byte_len + 1);
    char *out = result;

    for (size_t i = char_count; i > 0; i--) {
        size_t idx = i - 1;
        memcpy(out, str + offsets[idx], lengths[idx]);
        out += lengths[idx];
    }
    *out = '\0';

    free(offsets);
    free(lengths);

    return result;
}

/* Generic reverse - handles both strings and arrays */
StradaValue* strada_reverse_sv(StradaValue *sv) {
    if (!sv) return strada_new_str("");

    /* Handle array references */
    if (sv->type == STRADA_REF && sv->value.rv && sv->value.rv->type == STRADA_ARRAY) {
        strada_array_reverse(sv->value.rv->value.av);
        strada_incref(sv);
        return sv;
    }

    /* Handle arrays directly */
    if (sv->type == STRADA_ARRAY) {
        strada_array_reverse(sv->value.av);
        strada_incref(sv);
        return sv;
    }

    /* For strings and other types, reverse as string */
    char _tb[256];
    const char *str = strada_to_str_buf(sv, _tb, sizeof(_tb));
    char *reversed = strada_reverse(str);
    StradaValue *result = strada_new_str(reversed);
    free(reversed);
    return result;
}

char* strada_repeat(const char *str, int count) {
    if (!str || count <= 0) return strdup("");

    size_t len = strlen(str);
    if (len == 0) return strdup("");
    if ((size_t)count > SIZE_MAX / len) return strdup("");  /* overflow guard */
    size_t total = len * (size_t)count;
    char *result = malloc(total + 1);
    if (!result) return strdup("");

    for (int i = 0; i < count; i++) {
        memcpy(result + (i * len), str, len);
    }
    result[total] = '\0';

    return result;
}

/* Unicode-aware chr - converts codepoint to UTF-8 string */
char* strada_chr(int code) {
    char *result = malloc(5);  /* Max 4 bytes for UTF-8 + null */
    int len = utf8_encode((uint32_t)code, result);
    result[len] = '\0';
    return result;
}

/* Binary-safe chr - returns StradaValue with correct length (handles NUL bytes).
 * For values 0-255, returns a single raw byte (not UTF-8 encoded).
 * For values >= 256, uses UTF-8 encoding. */
StradaValue* strada_chr_sv(int code) {
    char buf[5];
    int len;
    if (code >= 0 && code <= 255) {
        /* Raw byte output for 0-255 (like Perl's chr) */
        buf[0] = (char)(unsigned char)code;
        len = 1;
    } else {
        /* UTF-8 encode for higher codepoints */
        len = utf8_encode((uint32_t)code, buf);
    }
    buf[len] = '\0';
    return strada_new_str_len(buf, (size_t)len);
}

/* Unicode-aware ord - returns codepoint of first character */
int strada_ord(const char *str) {
    if (!str || !str[0]) return 0;
    return (int)utf8_decode(str, NULL);
}

/* ===== UTF-8 NAMESPACE FUNCTIONS ===== */

/* utf8::is_utf8 / utf8::valid - validate if string is well-formed UTF-8 */
int strada_utf8_is_valid(const char *str, size_t len) {
    if (!str) return 1;  /* NULL/empty is trivially valid */
    size_t i = 0;
    while (i < len) {
        unsigned char c = (unsigned char)str[i];
        int char_len = utf8_char_len(c);
        if (char_len == 0) return 0;  /* Invalid start byte */
        if (i + char_len > len) return 0;  /* Truncated sequence */
        for (int j = 1; j < char_len; j++) {
            if (!utf8_is_continuation((unsigned char)str[i + j])) return 0;
        }
        /* Check for overlong encodings */
        if (char_len == 2 && c < 0xC2) return 0;
        if (char_len == 3) {
            uint32_t cp = ((c & 0x0F) << 12) | (((unsigned char)str[i+1] & 0x3F) << 6) | ((unsigned char)str[i+2] & 0x3F);
            if (cp < 0x800) return 0;  /* Overlong */
            if (cp >= 0xD800 && cp <= 0xDFFF) return 0;  /* Surrogate */
        }
        if (char_len == 4) {
            uint32_t cp = ((c & 0x07) << 18) | (((unsigned char)str[i+1] & 0x3F) << 12) |
                          (((unsigned char)str[i+2] & 0x3F) << 6) | ((unsigned char)str[i+3] & 0x3F);
            if (cp < 0x10000 || cp > 0x10FFFF) return 0;  /* Overlong or out of range */
        }
        i += char_len;
    }
    return 1;
}

StradaValue* strada_utf8_is_utf8(StradaValue *sv) {
    if (!sv || sv->type != STRADA_STR || !sv->value.pv) return strada_new_int(1);
    size_t len = sv->struct_size ? sv->struct_size : strlen(sv->value.pv);
    return strada_new_int(strada_utf8_is_valid(sv->value.pv, len));
}

StradaValue* strada_utf8_valid(StradaValue *sv) {
    return strada_utf8_is_utf8(sv);
}

/* utf8::encode - in Strada, strings are already bytes; this is a no-op that returns the string */
StradaValue* strada_utf8_encode(StradaValue *sv) {
    if (!sv) return strada_new_str("");
    strada_incref(sv);
    return sv;
}

/* utf8::decode - validate UTF-8; returns 1 if valid, 0 if not */
StradaValue* strada_utf8_decode(StradaValue *sv) {
    if (!sv || sv->type != STRADA_STR || !sv->value.pv) return strada_new_int(1);
    size_t len = sv->struct_size ? sv->struct_size : strlen(sv->value.pv);
    return strada_new_int(strada_utf8_is_valid(sv->value.pv, len));
}

/* utf8::downgrade - check if string is ASCII-only; returns string on success, undef on failure with fail_ok */
StradaValue* strada_utf8_downgrade(StradaValue *sv, int fail_ok) {
    if (!sv || sv->type != STRADA_STR || !sv->value.pv) {
        if (sv) { strada_incref(sv); return sv; }
        return strada_new_undef();
    }
    const char *s = sv->value.pv;
    size_t len = sv->struct_size ? sv->struct_size : strlen(s);
    for (size_t i = 0; i < len; i++) {
        if ((unsigned char)s[i] > 127) {
            if (fail_ok) return strada_new_undef();
            fprintf(stderr, "Wide character in subroutine entry\n");
            exit(1);
        }
    }
    strada_incref(sv);
    return sv;
}

/* utf8::upgrade - in Strada, strings are already UTF-8; returns the string */
StradaValue* strada_utf8_upgrade(StradaValue *sv) {
    if (!sv) return strada_new_str("");
    strada_incref(sv);
    return sv;
}

/* utf8::unicode_to_native - identity mapping on modern systems */
StradaValue* strada_utf8_unicode_to_native(StradaValue *sv) {
    if (!sv) return strada_new_int(0);
    int64_t cp = strada_to_int(sv);
    return strada_new_int(cp);
}

/* Binary-safe ord - returns raw byte value (0-255) of first byte */
int strada_ord_byte(StradaValue *sv) {
    if (!sv) return 0;
    char _tb[256];
    const char *str;
    if (sv->type == STRADA_STR && sv->value.pv) {
        str = sv->value.pv;
    } else {
        str = strada_to_str_buf(sv, _tb, sizeof(_tb));
    }
    if (!str || !str[0]) return 0;
    return (unsigned char)str[0];
}

/* Get byte at position (0-indexed) - returns 0-255 or -1 if out of bounds */
int strada_get_byte(StradaValue *sv, int pos) {
    if (!sv) return -1;
    const char *str = NULL;
    char _tb[256];
    size_t len = 0;

    if (sv->type == STRADA_STR && sv->value.pv) {
        str = sv->value.pv;
        len = sv->struct_size > 0 ? sv->struct_size : strlen(str);
    } else {
        str = strada_to_str_buf(sv, _tb, sizeof(_tb));
        len = strlen(str);
    }

    if (pos < 0 || (size_t)pos >= len) return -1;
    return (unsigned char)str[pos];
}

/* Set byte at position - returns new string with byte modified */
StradaValue* strada_set_byte(StradaValue *sv, int pos, int val) {
    if (!sv) return strada_new_str("");
    const char *str = NULL;
    char _tb[256];
    size_t len = 0;

    if (sv->type == STRADA_STR && sv->value.pv) {
        str = sv->value.pv;
        len = sv->struct_size > 0 ? sv->struct_size : strlen(str);
    } else {
        str = strada_to_str_buf(sv, _tb, sizeof(_tb));
        len = strlen(str);
    }

    if (pos < 0 || (size_t)pos >= len) {
        return strada_new_str_len(str, len);
    }

    char *result = malloc(len + 1);
    memcpy(result, str, len);
    result[pos] = (unsigned char)(val & 0xFF);
    result[len] = '\0';

    StradaValue *ret = strada_new_str_len(result, len);
    free(result);
    return ret;
}

/* Get byte length of string (not UTF-8 character count) */
int strada_byte_length(StradaValue *sv) {
    if (!sv) return 0;

    if (sv->type == STRADA_STR) {
        if (sv->struct_size > 0) return (int)sv->struct_size;
        if (sv->value.pv) return (int)strlen(sv->value.pv);
        return 0;
    }

    char _tb[256];
    const char *str = strada_to_str_buf(sv, _tb, sizeof(_tb));
    return str ? (int)strlen(str) : 0;
}

/* Substring by byte positions (not UTF-8 character positions) */
StradaValue* strada_byte_substr(StradaValue *sv, int start, int len) {
    if (!sv) return strada_new_str("");
    const char *str = NULL;
    char _tb[256];
    size_t str_len = 0;

    if (sv->type == STRADA_STR && sv->value.pv) {
        str = sv->value.pv;
        str_len = sv->struct_size > 0 ? sv->struct_size : strlen(str);
    } else {
        str = strada_to_str_buf(sv, _tb, sizeof(_tb));
        str_len = strlen(str);
    }

    /* Handle negative start (from end) */
    if (start < 0) {
        start = (int)str_len + start;
        if (start < 0) start = 0;
    }

    if ((size_t)start >= str_len) {
        return strada_new_str("");
    }

    /* Handle length */
    if (len < 0) {
        /* Negative length means "leave that many off the end" */
        len = (int)str_len - start + len;
        if (len < 0) len = 0;
    }

    size_t actual_len = len;
    if (start + actual_len > str_len) {
        actual_len = str_len - start;
    }

    StradaValue *result = strada_new_str_len(str + start, actual_len);
    return result;
}

/* Pack values into binary string - Perl-like pack() */
StradaValue* strada_pack(const char *fmt, StradaValue *args) {
    if (!fmt || !args) return strada_new_str("");

    StradaArray *av = strada_deref_array(args);
    if (!av) return strada_new_str("");

    size_t buf_size = 1024;
    char *buf = malloc(buf_size);
    size_t buf_pos = 0;
    int arg_idx = 0;
    int arg_count = strada_array_length(av);

    #define ENSURE_SPACE(n) do { \
        if (buf_pos + (n) > buf_size) { \
            buf_size = buf_size * 2 + (n); \
            buf = realloc(buf, buf_size); \
        } \
    } while(0)

    #define GET_ARG_INT() (arg_idx < arg_count ? strada_to_int(strada_array_get(av, arg_idx++)) : 0)
    #define GET_ARG_STR(tbuf) (arg_idx < arg_count ? strada_to_str_buf(strada_array_get(av, arg_idx++), tbuf, sizeof(tbuf)) : "")

    const char *p = fmt;
    while (*p) {
        int count = 1;
        char c = *p++;

        /* Parse repeat count */
        if (*p >= '0' && *p <= '9') {
            count = 0;
            while (*p >= '0' && *p <= '9') {
                count = count * 10 + (*p - '0');
                p++;
            }
        } else if (*p == '*') {
            count = -1;  /* Use remaining string length */
            p++;
        }

        switch (c) {
            case 'c':  /* Signed char */
            case 'C': {  /* Unsigned char */
                for (int i = 0; i < count; i++) {
                    ENSURE_SPACE(1);
                    buf[buf_pos++] = (char)(GET_ARG_INT() & 0xFF);
                }
                break;
            }

            case 's': {  /* Signed short, native endian */
                for (int i = 0; i < count; i++) {
                    ENSURE_SPACE(2);
                    int16_t val = (int16_t)GET_ARG_INT();
                    memcpy(buf + buf_pos, &val, 2);
                    buf_pos += 2;
                }
                break;
            }

            case 'S': {  /* Unsigned short, native endian */
                for (int i = 0; i < count; i++) {
                    ENSURE_SPACE(2);
                    uint16_t val = (uint16_t)GET_ARG_INT();
                    memcpy(buf + buf_pos, &val, 2);
                    buf_pos += 2;
                }
                break;
            }

            case 'n': {  /* Unsigned short, network (big) endian */
                for (int i = 0; i < count; i++) {
                    ENSURE_SPACE(2);
                    uint16_t val = (uint16_t)GET_ARG_INT();
                    buf[buf_pos++] = (val >> 8) & 0xFF;
                    buf[buf_pos++] = val & 0xFF;
                }
                break;
            }

            case 'v': {  /* Unsigned short, little endian */
                for (int i = 0; i < count; i++) {
                    ENSURE_SPACE(2);
                    uint16_t val = (uint16_t)GET_ARG_INT();
                    buf[buf_pos++] = val & 0xFF;
                    buf[buf_pos++] = (val >> 8) & 0xFF;
                }
                break;
            }

            case 'l': {  /* Signed long, native endian */
                for (int i = 0; i < count; i++) {
                    ENSURE_SPACE(4);
                    int32_t val = (int32_t)GET_ARG_INT();
                    memcpy(buf + buf_pos, &val, 4);
                    buf_pos += 4;
                }
                break;
            }

            case 'L': {  /* Unsigned long, native endian */
                for (int i = 0; i < count; i++) {
                    ENSURE_SPACE(4);
                    uint32_t val = (uint32_t)GET_ARG_INT();
                    memcpy(buf + buf_pos, &val, 4);
                    buf_pos += 4;
                }
                break;
            }

            case 'N': {  /* Unsigned long, network (big) endian */
                for (int i = 0; i < count; i++) {
                    ENSURE_SPACE(4);
                    uint32_t val = (uint32_t)GET_ARG_INT();
                    buf[buf_pos++] = (val >> 24) & 0xFF;
                    buf[buf_pos++] = (val >> 16) & 0xFF;
                    buf[buf_pos++] = (val >> 8) & 0xFF;
                    buf[buf_pos++] = val & 0xFF;
                }
                break;
            }

            case 'V': {  /* Unsigned long, little endian */
                for (int i = 0; i < count; i++) {
                    ENSURE_SPACE(4);
                    uint32_t val = (uint32_t)GET_ARG_INT();
                    buf[buf_pos++] = val & 0xFF;
                    buf[buf_pos++] = (val >> 8) & 0xFF;
                    buf[buf_pos++] = (val >> 16) & 0xFF;
                    buf[buf_pos++] = (val >> 24) & 0xFF;
                }
                break;
            }

            case 'q': {  /* Signed quad (8 bytes), native endian */
                for (int i = 0; i < count; i++) {
                    ENSURE_SPACE(8);
                    int64_t val = GET_ARG_INT();
                    memcpy(buf + buf_pos, &val, 8);
                    buf_pos += 8;
                }
                break;
            }

            case 'Q': {  /* Unsigned quad (8 bytes), native endian */
                for (int i = 0; i < count; i++) {
                    ENSURE_SPACE(8);
                    uint64_t val = (uint64_t)GET_ARG_INT();
                    memcpy(buf + buf_pos, &val, 8);
                    buf_pos += 8;
                }
                break;
            }

            case 'a':    /* ASCII string, null padded */
            case 'A': {  /* ASCII string, space padded */
                char _tb_s[256];
                const char *str = GET_ARG_STR(_tb_s);
                size_t str_len = strlen(str);
                size_t pad_len = (count < 0) ? str_len : (size_t)count;
                ENSURE_SPACE(pad_len);

                size_t copy_len = str_len < pad_len ? str_len : pad_len;
                memcpy(buf + buf_pos, str, copy_len);

                char pad_char = (c == 'a') ? '\0' : ' ';
                for (size_t i = copy_len; i < pad_len; i++) {
                    buf[buf_pos + i] = pad_char;
                }
                buf_pos += pad_len;
                break;
            }

            case 'H': {  /* Hex string, high nybble first */
                char _tb_s[256];
                const char *str = GET_ARG_STR(_tb_s);
                size_t str_len = strlen(str);
                size_t hex_count = (count < 0) ? str_len : (size_t)count;
                size_t bytes_needed = (hex_count + 1) / 2;
                ENSURE_SPACE(bytes_needed);

                for (size_t i = 0; i < hex_count && i < str_len; i++) {
                    int nybble = 0;
                    char ch = str[i];
                    if (ch >= '0' && ch <= '9') nybble = ch - '0';
                    else if (ch >= 'a' && ch <= 'f') nybble = ch - 'a' + 10;
                    else if (ch >= 'A' && ch <= 'F') nybble = ch - 'A' + 10;

                    if (i % 2 == 0) {
                        buf[buf_pos] = (nybble << 4);
                    } else {
                        buf[buf_pos++] |= nybble;
                    }
                }
                if (hex_count % 2 == 1) buf_pos++;
                break;
            }

            case 'x': {  /* Null byte */
                ENSURE_SPACE(count);
                for (int i = 0; i < count; i++) {
                    buf[buf_pos++] = '\0';
                }
                break;
            }

            case 'X': {  /* Back up a byte */
                for (int i = 0; i < count && buf_pos > 0; i++) {
                    buf_pos--;
                }
                break;
            }

            default:
                /* Unknown format, skip */
                break;
        }
    }

    #undef ENSURE_SPACE
    #undef GET_ARG_INT
    #undef GET_ARG_STR

    StradaValue *result = strada_new_str_len(buf, buf_pos);
    free(buf);
    return result;
}

/* Unpack binary string into array of values - Perl-like unpack() */
StradaValue* strada_unpack(const char *fmt, StradaValue *data_sv) {
    if (!fmt) return strada_new_array();

    const char *data = NULL;
    char _tb[256];
    size_t data_len = 0;

    if (data_sv && data_sv->type == STRADA_STR && data_sv->value.pv) {
        data = data_sv->value.pv;
        data_len = data_sv->struct_size > 0 ? data_sv->struct_size : strlen(data);
    } else if (data_sv) {
        data = strada_to_str_buf(data_sv, _tb, sizeof(_tb));
        data_len = data ? strlen(data) : 0;
    }

    if (!data) return strada_new_array();

    StradaValue *result = strada_new_array();
    size_t pos = 0;

    const char *p = fmt;
    while (*p && pos <= data_len) {
        int count = 1;
        char c = *p++;

        /* Parse repeat count */
        if (*p >= '0' && *p <= '9') {
            count = 0;
            while (*p >= '0' && *p <= '9') {
                count = count * 10 + (*p - '0');
                p++;
            }
        } else if (*p == '*') {
            count = -1;  /* Remaining data */
            p++;
        }

        switch (c) {
            case 'c': {  /* Signed char */
                int actual = (count < 0) ? (int)(data_len - pos) : count;
                for (int i = 0; i < actual && pos < data_len; i++) {
                    int8_t val = (int8_t)data[pos++];
                    strada_array_push_take(result->value.av, strada_new_int(val));
                }
                break;
            }

            case 'C': {  /* Unsigned char */
                int actual = (count < 0) ? (int)(data_len - pos) : count;
                for (int i = 0; i < actual && pos < data_len; i++) {
                    uint8_t val = (uint8_t)data[pos++];
                    strada_array_push_take(result->value.av, strada_new_int(val));
                }
                break;
            }

            case 's': {  /* Signed short, native endian */
                int actual = (count < 0) ? (int)((data_len - pos) / 2) : count;
                for (int i = 0; i < actual && pos + 2 <= data_len; i++) {
                    int16_t val;
                    memcpy(&val, data + pos, 2);
                    pos += 2;
                    strada_array_push_take(result->value.av, strada_new_int(val));
                }
                break;
            }

            case 'S': {  /* Unsigned short, native endian */
                int actual = (count < 0) ? (int)((data_len - pos) / 2) : count;
                for (int i = 0; i < actual && pos + 2 <= data_len; i++) {
                    uint16_t val;
                    memcpy(&val, data + pos, 2);
                    pos += 2;
                    strada_array_push_take(result->value.av, strada_new_int(val));
                }
                break;
            }

            case 'n': {  /* Unsigned short, network (big) endian */
                int actual = (count < 0) ? (int)((data_len - pos) / 2) : count;
                for (int i = 0; i < actual && pos + 2 <= data_len; i++) {
                    uint16_t val = ((uint8_t)data[pos] << 8) | (uint8_t)data[pos + 1];
                    pos += 2;
                    strada_array_push_take(result->value.av, strada_new_int(val));
                }
                break;
            }

            case 'v': {  /* Unsigned short, little endian */
                int actual = (count < 0) ? (int)((data_len - pos) / 2) : count;
                for (int i = 0; i < actual && pos + 2 <= data_len; i++) {
                    uint16_t val = (uint8_t)data[pos] | ((uint8_t)data[pos + 1] << 8);
                    pos += 2;
                    strada_array_push_take(result->value.av, strada_new_int(val));
                }
                break;
            }

            case 'l': {  /* Signed long, native endian */
                int actual = (count < 0) ? (int)((data_len - pos) / 4) : count;
                for (int i = 0; i < actual && pos + 4 <= data_len; i++) {
                    int32_t val;
                    memcpy(&val, data + pos, 4);
                    pos += 4;
                    strada_array_push_take(result->value.av, strada_new_int(val));
                }
                break;
            }

            case 'L': {  /* Unsigned long, native endian */
                int actual = (count < 0) ? (int)((data_len - pos) / 4) : count;
                for (int i = 0; i < actual && pos + 4 <= data_len; i++) {
                    uint32_t val;
                    memcpy(&val, data + pos, 4);
                    pos += 4;
                    strada_array_push_take(result->value.av, strada_new_int(val));
                }
                break;
            }

            case 'N': {  /* Unsigned long, network (big) endian */
                int actual = (count < 0) ? (int)((data_len - pos) / 4) : count;
                for (int i = 0; i < actual && pos + 4 <= data_len; i++) {
                    uint32_t val = ((uint8_t)data[pos] << 24) |
                                   ((uint8_t)data[pos + 1] << 16) |
                                   ((uint8_t)data[pos + 2] << 8) |
                                   (uint8_t)data[pos + 3];
                    pos += 4;
                    strada_array_push_take(result->value.av, strada_new_int(val));
                }
                break;
            }

            case 'V': {  /* Unsigned long, little endian */
                int actual = (count < 0) ? (int)((data_len - pos) / 4) : count;
                for (int i = 0; i < actual && pos + 4 <= data_len; i++) {
                    uint32_t val = (uint8_t)data[pos] |
                                   ((uint8_t)data[pos + 1] << 8) |
                                   ((uint8_t)data[pos + 2] << 16) |
                                   ((uint8_t)data[pos + 3] << 24);
                    pos += 4;
                    strada_array_push_take(result->value.av, strada_new_int(val));
                }
                break;
            }

            case 'q': {  /* Signed quad (8 bytes), native endian */
                int actual = (count < 0) ? (int)((data_len - pos) / 8) : count;
                for (int i = 0; i < actual && pos + 8 <= data_len; i++) {
                    int64_t val;
                    memcpy(&val, data + pos, 8);
                    pos += 8;
                    strada_array_push_take(result->value.av, strada_new_int(val));
                }
                break;
            }

            case 'Q': {  /* Unsigned quad (8 bytes), native endian */
                int actual = (count < 0) ? (int)((data_len - pos) / 8) : count;
                for (int i = 0; i < actual && pos + 8 <= data_len; i++) {
                    uint64_t val;
                    memcpy(&val, data + pos, 8);
                    pos += 8;
                    strada_array_push_take(result->value.av, strada_new_int((int64_t)val));
                }
                break;
            }

            case 'a':    /* ASCII string, null included */
            case 'A': {  /* ASCII string, trailing whitespace/nulls stripped */
                size_t str_len = (count < 0) ? (data_len - pos) : (size_t)count;
                if (pos + str_len > data_len) str_len = data_len - pos;

                char *str = malloc(str_len + 1);
                memcpy(str, data + pos, str_len);
                str[str_len] = '\0';
                pos += str_len;

                /* 'A' strips trailing whitespace/nulls */
                if (c == 'A') {
                    size_t end = str_len;
                    while (end > 0 && (str[end - 1] == ' ' || str[end - 1] == '\0')) {
                        end--;
                    }
                    str[end] = '\0';
                    str_len = end;
                }

                StradaValue *sv = strada_new_str_len(str, str_len);
                strada_array_push_take(result->value.av, sv);
                free(str);
                break;
            }

            case 'H': {  /* Hex string, high nybble first */
                size_t hex_count = (count < 0) ? ((data_len - pos) * 2) : (size_t)count;
                size_t bytes_needed = (hex_count + 1) / 2;
                if (pos + bytes_needed > data_len) bytes_needed = data_len - pos;

                char *hex = malloc(hex_count + 1);
                size_t hex_pos = 0;

                for (size_t i = 0; i < bytes_needed && hex_pos < hex_count; i++) {
                    uint8_t byte = (uint8_t)data[pos + i];
                    if (hex_pos < hex_count) {
                        hex[hex_pos++] = "0123456789abcdef"[(byte >> 4) & 0xF];
                    }
                    if (hex_pos < hex_count) {
                        hex[hex_pos++] = "0123456789abcdef"[byte & 0xF];
                    }
                }
                hex[hex_pos] = '\0';
                pos += bytes_needed;

                strada_array_push_take(result->value.av, strada_new_str(hex));
                free(hex);
                break;
            }

            case 'x': {  /* Skip forward */
                int skip = (count < 0) ? (int)(data_len - pos) : count;
                pos += skip;
                if (pos > data_len) pos = data_len;
                break;
            }

            case 'X': {  /* Skip backward */
                for (int i = 0; i < count && pos > 0; i++) {
                    pos--;
                }
                break;
            }

            case '@': {  /* Go to absolute position */
                if (count >= 0 && (size_t)count <= data_len) {
                    pos = count;
                }
                break;
            }

            default:
                /* Unknown format, skip */
                break;
        }
    }

    return result;
}

/* Base64 encoding table */
static const char base64_table[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

/* Base64 decoding table (maps ASCII chars to 6-bit values, -1 for invalid) */
static const int8_t base64_decode_table[256] = {
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, 62, -1, -1, -1, 63,
    52, 53, 54, 55, 56, 57, 58, 59, 60, 61, -1, -1, -1, -1, -1, -1,
    -1,  0,  1,  2,  3,  4,  5,  6,  7,  8,  9, 10, 11, 12, 13, 14,
    15, 16, 17, 18, 19, 20, 21, 22, 23, 24, 25, -1, -1, -1, -1, -1,
    -1, 26, 27, 28, 29, 30, 31, 32, 33, 34, 35, 36, 37, 38, 39, 40,
    41, 42, 43, 44, 45, 46, 47, 48, 49, 50, 51, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1,
    -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1
};

/* Base64 encode - encodes binary data to base64 string */
StradaValue* strada_base64_encode(StradaValue *sv) {
    if (!sv) return strada_new_str("");

    const unsigned char *data = NULL;
    char _tb[256];
    size_t len = 0;

    if (sv->type == STRADA_STR && sv->value.pv) {
        data = (const unsigned char *)sv->value.pv;
        len = sv->struct_size > 0 ? sv->struct_size : strlen(sv->value.pv);
    } else {
        const char *_s = strada_to_str_buf(sv, _tb, sizeof(_tb));
        data = (const unsigned char *)_s;
        len = _s ? strlen(_s) : 0;
    }

    if (!data || len == 0) {
        return strada_new_str("");
    }

    /* Output length: 4 chars for every 3 bytes, rounded up */
    size_t out_len = ((len + 2) / 3) * 4;
    char *result = malloc(out_len + 1);

    size_t i, j;
    for (i = 0, j = 0; i < len; ) {
        uint32_t octet_a = i < len ? data[i++] : 0;
        uint32_t octet_b = i < len ? data[i++] : 0;
        uint32_t octet_c = i < len ? data[i++] : 0;

        uint32_t triple = (octet_a << 16) | (octet_b << 8) | octet_c;

        result[j++] = base64_table[(triple >> 18) & 0x3F];
        result[j++] = base64_table[(triple >> 12) & 0x3F];
        result[j++] = base64_table[(triple >>  6) & 0x3F];
        result[j++] = base64_table[triple & 0x3F];
    }

    /* Add padding */
    size_t mod = len % 3;
    if (mod == 1) {
        result[out_len - 1] = '=';
        result[out_len - 2] = '=';
    } else if (mod == 2) {
        result[out_len - 1] = '=';
    }

    result[out_len] = '\0';
    StradaValue *ret = strada_new_str(result);
    free(result);
    return ret;
}

/* Base64 decode - decodes base64 string to binary data */
StradaValue* strada_base64_decode(StradaValue *sv) {
    if (!sv) return strada_new_str("");

    const char *str = NULL;
    char _tb[256];

    if (sv->type == STRADA_STR && sv->value.pv) {
        str = sv->value.pv;
    } else {
        str = strada_to_str_buf(sv, _tb, sizeof(_tb));
    }

    if (!str || !str[0]) {
        return strada_new_str("");
    }

    size_t len = strlen(str);

    /* Remove padding from length calculation */
    size_t pad = 0;
    if (len >= 1 && str[len - 1] == '=') pad++;
    if (len >= 2 && str[len - 2] == '=') pad++;

    /* Output length: 3 bytes for every 4 chars, minus padding */
    size_t out_len = (len / 4) * 3 - pad;
    unsigned char *result = malloc(out_len + 1);

    size_t i, j;
    for (i = 0, j = 0; i + 3 < len; i += 4) {
        /* Get 4 input characters, decode each */
        int8_t sextet_a = base64_decode_table[(unsigned char)str[i]];
        int8_t sextet_b = base64_decode_table[(unsigned char)str[i + 1]];
        int8_t sextet_c = (str[i + 2] != '=') ? base64_decode_table[(unsigned char)str[i + 2]] : 0;
        int8_t sextet_d = (str[i + 3] != '=') ? base64_decode_table[(unsigned char)str[i + 3]] : 0;

        /* Treat invalid characters as 0 */
        if (sextet_a < 0) sextet_a = 0;
        if (sextet_b < 0) sextet_b = 0;
        if (sextet_c < 0) sextet_c = 0;
        if (sextet_d < 0) sextet_d = 0;

        uint32_t triple = ((uint32_t)sextet_a << 18) |
                          ((uint32_t)sextet_b << 12) |
                          ((uint32_t)sextet_c <<  6) |
                          (uint32_t)sextet_d;

        if (j < out_len) result[j++] = (triple >> 16) & 0xFF;
        if (j < out_len) result[j++] = (triple >>  8) & 0xFF;
        if (j < out_len) result[j++] = triple & 0xFF;
    }

    result[out_len] = '\0';
    StradaValue *ret = strada_new_str_len((char *)result, out_len);
    free(result);
    return ret;
}

/* Convert hex string to integer: hex("ff") -> 255, hex("0x1A") -> 26 */
StradaValue* strada_hex(StradaValue *sv) {
    if (!sv) return strada_new_int(0);
    char _tb[256];
    const char *str = strada_to_str_buf(sv, _tb, sizeof(_tb));
    const char *p = str;
    /* Skip optional 0x/0X prefix */
    if (p[0] == '0' && (p[1] == 'x' || p[1] == 'X')) p += 2;
    long long val = strtoll(p, NULL, 16);
    return strada_new_int(val);
}

char* strada_chomp(const char *str) {
    if (!str) return strdup("");

    size_t len = strlen(str);
    if (len == 0) return strdup("");
    
    if (str[len - 1] == '\n') {
        len--;
        if (len > 0 && str[len - 1] == '\r') {
            len--;
        }
    } else if (str[len - 1] == '\r') {
        len--;
    }
    
    char *result = malloc(len + 1);
    memcpy(result, str, len);
    result[len] = '\0';
    
    return result;
}

char* strada_chop(const char *str) {
    if (!str) return strdup("");
    
    size_t len = strlen(str);
    if (len == 0) return strdup("");
    
    len--;
    char *result = malloc(len + 1);
    memcpy(result, str, len);
    result[len] = '\0';
    
    return result;
}

int strada_strcmp(const char *s1, const char *s2) {
    if (!s1 && !s2) return 0;
    if (!s1) return -1;
    if (!s2) return 1;
    return strcmp(s1, s2);
}

int strada_strncmp(const char *s1, const char *s2, int n) {
    if (!s1 && !s2) return 0;
    if (!s1) return -1;
    if (!s2) return 1;
    return strncmp(s1, s2, n);
}

char* strada_join(const char *sep, StradaArray *arr) {
    if (!arr || arr->size == 0) return strdup("");
    if (!sep) sep = "";
    
    size_t total_len = 0;
    size_t sep_len = strlen(sep);
    
    for (size_t i = 0; i < arr->size; i++) {
        char _tb[256];
        const char *str = strada_to_str_buf(arr->elements[arr->head + i], _tb, sizeof(_tb));
        total_len += strlen(str);
        if (i < arr->size - 1) {
            total_len += sep_len;
        }
    }

    char *result = malloc(total_len + 1);
    char *ptr = result;

    for (size_t i = 0; i < arr->size; i++) {
        char _tb[256];
        const char *str = strada_to_str_buf(arr->elements[arr->head + i], _tb, sizeof(_tb));
        size_t len = strlen(str);
        memcpy(ptr, str, len);
        ptr += len;
        
        if (i < arr->size - 1) {
            memcpy(ptr, sep, sep_len);
            ptr += sep_len;
        }
    }
    
    *ptr = '\0';
    return result;
}

/* ===== STRING BUILDER ===== */
/* Efficient string building with O(1) amortized append */

#define SB_INITIAL_CAPACITY 1024

StradaValue* strada_sb_new(void) {
    StradaStringBuilder *sb = malloc(sizeof(StradaStringBuilder));
    sb->capacity = SB_INITIAL_CAPACITY;
    sb->length = 0;
    sb->buffer = malloc(sb->capacity);
    sb->buffer[0] = '\0';

    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_CPOINTER;
    sv->refcount = 1;
    sv->value.ptr = sb;
    strada_ensure_meta(sv)->struct_name = "StringBuilder";
    sv->struct_size = sizeof(StradaStringBuilder);
    return sv;
}

StradaValue* strada_sb_new_cap(StradaValue *capacity) {
    size_t cap = (size_t)strada_to_int(capacity);
    if (cap < 64) cap = 64;

    StradaStringBuilder *sb = malloc(sizeof(StradaStringBuilder));
    sb->capacity = cap;
    sb->length = 0;
    sb->buffer = malloc(sb->capacity);
    sb->buffer[0] = '\0';

    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_CPOINTER;
    sv->refcount = 1;
    sv->value.ptr = sb;
    strada_ensure_meta(sv)->struct_name = "StringBuilder";
    sv->struct_size = sizeof(StradaStringBuilder);
    return sv;
}

void strada_sb_append(StradaValue *sb_val, StradaValue *str_val) {
    if (!sb_val || sb_val->type != STRADA_CPOINTER) return;
    StradaStringBuilder *sb = (StradaStringBuilder*)sb_val->value.ptr;
    if (!sb) return;

    char _tb[256];
    const char *str = strada_to_str_buf(str_val, _tb, sizeof(_tb));
    size_t len = strlen(str);

    /* Grow buffer if needed (double capacity) */
    while (sb->length + len + 1 > sb->capacity) {
        sb->capacity *= 2;
        sb->buffer = realloc(sb->buffer, sb->capacity);
    }

    memcpy(sb->buffer + sb->length, str, len);
    sb->length += len;
    sb->buffer[sb->length] = '\0';
}

void strada_sb_append_str(StradaValue *sb_val, const char *str) {
    if (!sb_val || sb_val->type != STRADA_CPOINTER || !str) return;
    StradaStringBuilder *sb = (StradaStringBuilder*)sb_val->value.ptr;
    if (!sb) return;

    size_t len = strlen(str);

    /* Grow buffer if needed (double capacity) */
    while (sb->length + len + 1 > sb->capacity) {
        sb->capacity *= 2;
        sb->buffer = realloc(sb->buffer, sb->capacity);
    }

    memcpy(sb->buffer + sb->length, str, len);
    sb->length += len;
    sb->buffer[sb->length] = '\0';
}

StradaValue* strada_sb_to_string(StradaValue *sb_val) {
    if (!sb_val || sb_val->type != STRADA_CPOINTER) return strada_new_str("");
    StradaStringBuilder *sb = (StradaStringBuilder*)sb_val->value.ptr;
    if (!sb) return strada_new_str("");

    return strada_new_str(sb->buffer);
}

StradaValue* strada_sb_length(StradaValue *sb_val) {
    if (!sb_val || sb_val->type != STRADA_CPOINTER) return strada_new_int(0);
    StradaStringBuilder *sb = (StradaStringBuilder*)sb_val->value.ptr;
    if (!sb) return strada_new_int(0);

    return strada_new_int(sb->length);
}

void strada_sb_clear(StradaValue *sb_val) {
    if (!sb_val || sb_val->type != STRADA_CPOINTER) return;
    StradaStringBuilder *sb = (StradaStringBuilder*)sb_val->value.ptr;
    if (!sb) return;

    sb->length = 0;
    sb->buffer[0] = '\0';
}

void strada_sb_free(StradaValue *sb_val) {
    if (!sb_val || sb_val->type != STRADA_CPOINTER) return;
    StradaStringBuilder *sb = (StradaStringBuilder*)sb_val->value.ptr;
    if (sb) {
        free(sb->buffer);
        free(sb);
    }
    sb_val->value.ptr = NULL;
}

/* ===== TYPE INTROSPECTION AND CASTING ===== */

const char* strada_typeof(StradaValue *sv) {
    if (!sv) return "undef";
    
    switch (sv->type) {
        case STRADA_UNDEF: return "undef";
        case STRADA_INT: return "int";
        case STRADA_NUM: return "num";
        case STRADA_STR: return "str";
        case STRADA_ARRAY: return "array";
        case STRADA_HASH: return "hash";
        case STRADA_REF: return "ref";
        case STRADA_FILEHANDLE: return "filehandle";
        case STRADA_REGEX: return "regex";
        case STRADA_SOCKET: return "socket";
        case STRADA_CSTRUCT: return "cstruct";
        case STRADA_CPOINTER: return "cpointer";
        default: return "unknown";
    }
}

int strada_is_int(StradaValue *sv) {
    return sv && sv->type == STRADA_INT;
}

int strada_is_num(StradaValue *sv) {
    return sv && sv->type == STRADA_NUM;
}

int strada_is_str(StradaValue *sv) {
    return sv && sv->type == STRADA_STR;
}

int strada_is_array(StradaValue *sv) {
    return sv && sv->type == STRADA_ARRAY;
}

int strada_is_hash(StradaValue *sv) {
    return sv && sv->type == STRADA_HASH;
}

StradaValue* strada_int(StradaValue *sv) {
    return strada_new_int(strada_to_int(sv));
}

StradaValue* strada_num(StradaValue *sv) {
    return strada_new_num(strada_to_num(sv));
}

StradaValue* strada_str(StradaValue *sv) {
    char _tb[256];
    const char *s = strada_to_str_buf(sv, _tb, sizeof(_tb));
    return strada_new_str(s);
}

StradaValue* strada_bool(StradaValue *sv) {
    return strada_new_int(strada_to_bool(sv));
}

int strada_scalar(StradaValue *sv) {
    if (!sv) return 0;
    
    switch (sv->type) {
        case STRADA_ARRAY:
            return (int)strada_array_length(sv->value.av);
        case STRADA_HASH:
            return sv->value.hv ? (int)sv->value.hv->num_entries : 0;
        case STRADA_UNDEF:
            return 0;
        default:
            return 1;
    }
}

/* ===== REFERENCE SYSTEM ===== */

StradaValue* strada_ref_create(StradaValue *sv) {
    /* Create a reference to a value (shared ownership - increfs target) */
    if (!sv) return strada_new_undef();

    StradaValue *ref = strada_value_alloc();
    ref->type = STRADA_REF;
    ref->refcount = 1;
    ref->value.rv = sv;

    /* Increment refcount of referenced value */
    strada_incref(sv);

    return ref;
}

StradaValue* strada_ref_create_take(StradaValue *sv) {
    /* Create a reference taking ownership (no incref - caller donates refcount) */
    /* Use for wrapping newly created values like in strada_anon_array */
    if (!sv) return strada_new_undef();

    StradaValue *ref = strada_value_alloc();
    ref->type = STRADA_REF;
    ref->refcount = 1;
    ref->value.rv = sv;

    /* No incref - caller donates their refcount */
    return ref;
}

StradaValue* strada_ref_deref(StradaValue *ref) {
    /* Dereference - get the value being referenced */
    if (!ref) return strada_new_undef();

    if (ref->type == STRADA_REF) {
        if (!ref->value.rv) return strada_new_undef();  /* Weak ref target was freed */
        return ref->value.rv;  /* borrowed reference */
    }

    /* Not a reference, return as-is */
    return ref;  /* borrowed reference */
}

int strada_is_ref(StradaValue *sv) {
    return sv && sv->type == STRADA_REF;
}

const char* strada_reftype(StradaValue *ref) {
    /* Get type of referenced value - returns uppercase like Perl's ref() */
    if (!ref) return "";

    /* If blessed, return the blessed package name (like Perl) */
    const char *bp = SV_BLESSED(ref);
    if (bp) {
        return bp;
    }

    if (ref->type == STRADA_REF) {
        StradaValue *target = ref->value.rv;
        if (!target) return "";

        /* Follow reference chain until we reach non-REF value */
        /* This handles cases like \@arr where @arr is already a reference */
        int depth = 0;
        while (target->type == STRADA_REF && target->value.rv && depth < 10) {
            /* Check for blessed package at each level */
            const char *tbp = SV_BLESSED(target);
            if (tbp) {
                return tbp;
            }
            target = target->value.rv;
            depth++;
        }

        /* Check if final target is blessed */
        const char *tbp2 = SV_BLESSED(target);
        if (tbp2) {
            return tbp2;
        }
        switch (target->type) {
            case STRADA_ARRAY: return "ARRAY";
            case STRADA_HASH: return "HASH";
            case STRADA_REF: return "REF";
            case STRADA_CLOSURE: return "CODE";
            case STRADA_REGEX: return "Regexp";
            case STRADA_FILEHANDLE: return "GLOB";
            case STRADA_SOCKET: return "GLOB";
            default: return "SCALAR";
        }
    }

    /* For non-ref types, return type name */
    switch (ref->type) {
        case STRADA_ARRAY: return "ARRAY";
        case STRADA_HASH: return "HASH";
        case STRADA_CLOSURE: return "CODE";
        default: return "";
    }
}

StradaValue* strada_ref_scalar(StradaValue **ptr) {
    /* Create reference to a scalar variable */
    if (!ptr || !*ptr) return strada_new_undef();
    return strada_ref_create(*ptr);
}

StradaValue* strada_ref_array(StradaArray **ptr) {
    /* Create reference to an array */
    if (!ptr || !*ptr) return strada_new_undef();

    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_ARRAY;
    sv->refcount = 1;
    sv->value.av = *ptr;
    strada_incref((StradaValue*)sv->value.av);

    return strada_ref_create_take(sv);
}

StradaValue* strada_ref_hash(StradaHash **ptr) {
    /* Create reference to a hash */
    if (!ptr || !*ptr) return strada_new_undef();

    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_HASH;
    sv->refcount = 1;
    sv->value.hv = *ptr;
    strada_incref((StradaValue*)sv->value.hv);

    return strada_ref_create_take(sv);
}

/* New Perl-style reference functions */

StradaValue* strada_new_ref(StradaValue *target, char ref_type) {
    /* Create a reference with type info: \$var, \@arr, \%hash */
    (void)ref_type;  /* Reserved for future type checking */
    if (!target) return strada_new_undef();

    StradaValue *ref = strada_value_alloc();
    ref->type = STRADA_REF;
    ref->refcount = 1;
    ref->value.rv = target;

    /* Increment refcount of referenced value */
    strada_incref(target);

    return ref;
}

StradaValue* strada_deref(StradaValue *ref) {
    /* $$ref - dereference a scalar reference */
    /* Returns an owned reference (increfs the value) */
    if (!ref) return strada_new_undef();
    if (ref->type == STRADA_REF) {
        StradaValue *result = ref->value.rv;
        if (!result) return strada_new_undef();  /* Weak ref target was freed */
        strada_incref(result);
        return result;
    }
    strada_incref(ref);
    return ref;
}

StradaValue* strada_deref_set(StradaValue *ref, StradaValue *new_value) {
    /* Set value through a scalar reference: deref_set($ref, $value) */
    if (!ref || ref->type != STRADA_REF) return new_value;

    StradaValue *target = ref->value.rv;
    if (!target) return new_value;

    /* Clean up old value based on target's current type */
    switch (target->type) {
        case STRADA_STR:
            if (target->value.pv) {
                free(target->value.pv);
                target->value.pv = NULL;
            }
            break;
        case STRADA_REF:
            if (target->value.rv) {
                strada_decref(target->value.rv);
                target->value.rv = NULL;
            }
            break;
        default:
            break;
    }

    /* Copy the new value's type and content into target */
    target->type = new_value->type;
    switch (new_value->type) {
        case STRADA_INT:
            target->value.iv = new_value->value.iv;
            break;
        case STRADA_NUM:
            target->value.nv = new_value->value.nv;
            break;
        case STRADA_STR:
            target->value.pv = new_value->value.pv ? strdup(new_value->value.pv) : NULL;
            break;
        case STRADA_REF:
            target->value.rv = new_value->value.rv;
            if (target->value.rv) strada_incref(new_value->value.rv);
            break;
        default:
            /* For ARRAY, HASH, and other complex types, incref the new value
               and store the whole StradaValue content. The caller's reference
               to new_value keeps the data alive. */
            target->value = new_value->value;
            strada_incref(new_value);
            break;
    }

    return target;
}

StradaHash* strada_deref_hash(StradaValue *ref) {
    /* Get hash from hash reference for $ref->{key} */
    if (!ref) return NULL;

    if (ref->type == STRADA_REF) {
        StradaValue *inner = ref->value.rv;
        if (inner && inner->type == STRADA_HASH) {
            return inner->value.hv;
        }
    } else if (ref->type == STRADA_HASH) {
        return ref->value.hv;
    }

    return NULL;
}

StradaValue* strada_deref_hash_value(StradaValue *ref) {
    /* Get hash StradaValue from hash reference for deref_hash() builtin
     * Returns borrowed reference (caller must NOT decref).
     * Error paths return immortal static singleton to avoid leaks. */
    if (!ref) return strada_empty_hash_static();

    if (ref->type == STRADA_REF) {
        StradaValue *inner = ref->value.rv;
        if (inner && inner->type == STRADA_HASH) {
            return inner;
        }
    } else if (ref->type == STRADA_HASH) {
        return ref;
    }

    return strada_empty_hash_static();
}

StradaArray* strada_deref_array(StradaValue *ref) {
    /* Get array from array reference for $ref->[index] */
    if (!ref) return NULL;

    /* Follow chain of references until we find an array */
    StradaValue *current = ref;
    while (current && current->type == STRADA_REF) {
        current = current->value.rv;
    }

    if (current && current->type == STRADA_ARRAY) {
        return current->value.av;
    }

    return NULL;
}

StradaValue* strada_deref_array_value(StradaValue *ref) {
    /* Get array StradaValue from array reference for deref_array() builtin
     * Returns borrowed reference (caller must NOT decref).
     * Error paths return immortal static singleton to avoid leaks. */
    if (!ref) return strada_empty_array_static();

    /* Follow chain of references until we find an array */
    StradaValue *current = ref;
    while (current && current->type == STRADA_REF) {
        current = current->value.rv;
    }

    if (current && current->type == STRADA_ARRAY) {
        return current;
    }

    return strada_empty_array_static();
}

StradaValue* strada_anon_hash(int count, ...) {
    /* Create anonymous hash: { key => val, ... } */
    StradaValue *sv = strada_new_hash();

    va_list args;
    va_start(args, count);

    for (int i = 0; i < count; i++) {
        const char *key = va_arg(args, const char*);
        StradaValue *val = va_arg(args, StradaValue*);
        strada_hash_set(sv->value.hv, key, val);
    }

    va_end(args);

    /* Return as reference - take ownership since we just created the hash */
    return strada_ref_create_take(sv);
}

StradaValue* strada_anon_array(int count, ...) {
    /* Create anonymous array: [ elem, ... ]
     * Uses strada_array_push to properly incref elements. This is necessary
     * because elements may be existing variables (not just newly created values).
     * When the array is freed, it will decref elements, so we must incref here
     * to avoid use-after-free when the original variable is still in use. */
    StradaValue *sv = strada_new_array();

    va_list args;
    va_start(args, count);

    for (int i = 0; i < count; i++) {
        StradaValue *elem = va_arg(args, StradaValue*);
        strada_array_push(sv->value.av, elem);  /* incref to avoid use-after-free */
    }

    va_end(args);

    /* Return as reference - take ownership since we just created the array */
    return strada_ref_create_take(sv);
}

StradaValue* strada_array_from_ref(StradaValue *ref) {
    /* Convert array reference to a new array (shallow copy) */
    if (!ref) return strada_new_array();
    
    StradaArray *src = NULL;
    
    if (ref->type == STRADA_REF) {
        StradaValue *inner = ref->value.rv;
        if (inner && inner->type == STRADA_ARRAY) {
            src = inner->value.av;
        }
    } else if (ref->type == STRADA_ARRAY) {
        src = ref->value.av;
    }
    
    if (!src) return strada_new_array();
    
    /* Create new array and copy elements */
    StradaValue *result = strada_new_array();
    for (size_t i = 0; i < src->size; i++) {
        strada_array_push(result->value.av, src->elements[src->head + i]);
    }
    
    return result;
}

StradaValue* strada_hash_from_ref(StradaValue *ref) {
    /* Convert hash reference to a new hash (shallow copy) */
    if (!ref) return strada_new_hash();
    
    StradaHash *src = NULL;
    
    if (ref->type == STRADA_REF) {
        StradaValue *inner = ref->value.rv;
        if (inner && inner->type == STRADA_HASH) {
            src = inner->value.hv;
        }
    } else if (ref->type == STRADA_HASH) {
        src = ref->value.hv;
    }
    
    if (!src) return strada_new_hash();
    
    /* Create new hash and copy entries */
    StradaValue *result = strada_new_hash();
    
    for (size_t i = 0; i < src->num_buckets; i++) {
        StradaHashEntry *entry = src->buckets[i];
        while (entry) {
            strada_hash_set(result->value.hv, entry->key, entry->value);
            entry = entry->next;
        }
    }
    
    return result;
}

StradaValue* strada_hash_from_flat_array(StradaValue *arr) {
    /* Convert flat array [key, val, key, val, ...] to hash {key => val, ...} */
    StradaValue *result = strada_new_hash();

    if (!arr) return result;

    /* Handle references to arrays */
    StradaArray *src = NULL;
    if (arr->type == STRADA_REF) {
        StradaValue *inner = arr->value.rv;
        if (inner && inner->type == STRADA_ARRAY) {
            src = inner->value.av;
        }
    } else if (arr->type == STRADA_ARRAY) {
        src = arr->value.av;
    }

    if (!src) return result;

    /* Check if this is an array-of-pairs: [[k,v], [k,v], ...] */
    /* Pairs may be STRADA_ARRAY directly or STRADA_REF to an array */
    if (src->size > 0 && src->elements[src->head]) {
        StradaValue *first = src->elements[src->head];
        StradaArray *first_av = NULL;
        if (first->type == STRADA_ARRAY) {
            first_av = first->value.av;
        } else if (first->type == STRADA_REF && first->value.rv && first->value.rv->type == STRADA_ARRAY) {
            first_av = first->value.rv->value.av;
        }
        if (first_av && first_av->size >= 2) {
            for (size_t i = 0; i < src->size; i++) {
                StradaValue *elem = src->elements[src->head + i];
                StradaArray *pair_av = NULL;
                if (elem && elem->type == STRADA_ARRAY) {
                    pair_av = elem->value.av;
                } else if (elem && elem->type == STRADA_REF && elem->value.rv && elem->value.rv->type == STRADA_ARRAY) {
                    pair_av = elem->value.rv->value.av;
                }
                if (pair_av && pair_av->size >= 2) {
                    char _tb[256];
                    const char *key_str = strada_to_str_buf(pair_av->elements[pair_av->head], _tb, sizeof(_tb));
                    strada_hash_set(result->value.hv, key_str, pair_av->elements[pair_av->head + 1]);
                }
            }
            strada_decref(arr);
            return result;
        }
    }

    /* Iterate in pairs: [0]=key, [1]=val, [2]=key, [3]=val, ... */
    for (size_t i = 0; i + 1 < src->size; i += 2) {
        StradaValue *key = src->elements[src->head + i];
        StradaValue *val = src->elements[src->head + i + 1];

        if (key) {
            char _tb[256];
            const char *key_str = strada_to_str_buf(key, _tb, sizeof(_tb));
            /* strada_hash_set already calls strada_incref on the value */
            strada_hash_set(result->value.hv, key_str, val);
        }
    }

    /* Free the input array since we're consuming it */
    strada_decref(arr);

    return result;
}

StradaValue* strada_hash_to_flat_array(StradaValue *hash) {
    /* Convert hash {k => v, ...} to flat array [k, v, k, v, ...] */
    StradaValue *result = strada_new_array();
    if (!hash || hash->type != STRADA_HASH) return result;
    StradaHash *hv = hash->value.hv;
    for (size_t i = 0; i < hv->num_buckets; i++) {
        StradaHashEntry *entry = hv->buckets[i];
        while (entry) {
            strada_array_push_take(result->value.av, strada_new_str(entry->key));
            strada_array_push(result->value.av, entry->value);
            entry = entry->next;
        }
    }
    return result;
}

/* ===== ARRAY/HASH HELPER FUNCTIONS ===== */

StradaValue* strada_new_array_from_av(StradaArray *av) {
    /* Wrap a StradaArray in a StradaValue - takes ownership of av */
    if (!av) return strada_new_array();

    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_ARRAY;
    sv->refcount = 1;
    sv->value.av = av;

    /* No incref - we take ownership of the newly created array.
     * All callers (strada_hash_keys, strada_hash_values, strada_regex_capture)
     * create new arrays with refcount=1 that we adopt. */

    return sv;
}

/* ===== VARIADIC FUNCTION SUPPORT ===== */

StradaValue* strada_pack_args(int count, ...) {
    /* Pack variable arguments into an array */
    StradaValue *array = strada_new_array();
    
    va_list args;
    va_start(args, count);
    
    for (int i = 0; i < count; i++) {
        StradaValue *arg = va_arg(args, StradaValue*);
        strada_array_push(array->value.av, arg);
    }
    
    va_end(args);
    return array;
}

/* ===== ADDITIONAL UTILITY FUNCTIONS ===== */

StradaValue* strada_clone(StradaValue *sv) {
    if (!sv) return strada_new_undef();
    
    switch (sv->type) {
        case STRADA_UNDEF:
            return strada_new_undef();
        case STRADA_INT:
            return strada_new_int(sv->value.iv);
        case STRADA_NUM:
            return strada_new_num(sv->value.nv);
        case STRADA_STR:
            return strada_new_str(sv->value.pv ? sv->value.pv : "");
        case STRADA_ARRAY: {
            StradaValue *new_arr = strada_new_array();
            if (sv->value.av) {
                for (size_t i = 0; i < sv->value.av->size; i++) {
                    strada_array_push_take(new_arr->value.av, strada_clone(sv->value.av->elements[sv->value.av->head + i]));
                }
            }
            return new_arr;
        }
        case STRADA_HASH: {
            StradaValue *new_hash = strada_new_hash();
            if (sv->value.hv) {
                /* Iterate over hash buckets */
                for (size_t i = 0; i < sv->value.hv->num_buckets; i++) {
                    StradaHashEntry *entry = sv->value.hv->buckets[i];
                    while (entry) {
                        strada_hash_set_take(new_hash->value.hv, entry->key, strada_clone(entry->value));
                        entry = entry->next;
                    }
                }
            }
            return new_hash;
        }
        case STRADA_REF:
            return strada_ref_create_take(strada_clone(sv->value.rv));
        default:
            return strada_new_undef();
    }
}

StradaValue* strada_abs(StradaValue *sv) {
    if (!sv) return strada_new_int(0);
    if (sv->type == STRADA_INT) {
        int64_t v = sv->value.iv;
        return strada_new_int(v < 0 ? -v : v);
    } else if (sv->type == STRADA_NUM) {
        double v = sv->value.nv;
        return strada_new_num(v < 0 ? -v : v);
    }
    return strada_new_int(0);
}

StradaValue* strada_sqrt(StradaValue *sv) {
    if (!sv) return strada_new_num(0.0);
    double v = strada_to_num(sv);
    return strada_new_num(sqrt(v));
}

StradaValue* strada_rand(void) {
    static int seeded = 0;
    if (!seeded) {
        srand((unsigned int)time(NULL));
        seeded = 1;
    }
    return strada_new_num((double)rand() / RAND_MAX);
}

StradaValue* strada_time(void) {
    return strada_new_int((int64_t)time(NULL));
}

/* localtime - convert timestamp to local time hash */
StradaValue* strada_localtime(StradaValue *timestamp) {
    time_t t;
    if (timestamp) {
        t = (time_t)strada_to_int(timestamp);
    } else {
        t = time(NULL);
    }

    struct tm *tm = localtime(&t);
    if (!tm) return strada_new_undef();

    StradaValue *hash = strada_new_hash();
    strada_hash_set_take(hash->value.hv, "sec", strada_new_int(tm->tm_sec));
    strada_hash_set_take(hash->value.hv, "min", strada_new_int(tm->tm_min));
    strada_hash_set_take(hash->value.hv, "hour", strada_new_int(tm->tm_hour));
    strada_hash_set_take(hash->value.hv, "mday", strada_new_int(tm->tm_mday));
    strada_hash_set_take(hash->value.hv, "mon", strada_new_int(tm->tm_mon));      /* 0-11 */
    strada_hash_set_take(hash->value.hv, "year", strada_new_int(tm->tm_year));    /* years since 1900 */
    strada_hash_set_take(hash->value.hv, "wday", strada_new_int(tm->tm_wday));    /* 0=Sunday */
    strada_hash_set_take(hash->value.hv, "yday", strada_new_int(tm->tm_yday));    /* 0-365 */
    strada_hash_set_take(hash->value.hv, "isdst", strada_new_int(tm->tm_isdst));
    return hash;
}

/* gmtime - convert timestamp to UTC time hash */
StradaValue* strada_gmtime(StradaValue *timestamp) {
    time_t t;
    if (timestamp) {
        t = (time_t)strada_to_int(timestamp);
    } else {
        t = time(NULL);
    }

    struct tm *tm = gmtime(&t);
    if (!tm) return strada_new_undef();

    StradaValue *hash = strada_new_hash();
    strada_hash_set_take(hash->value.hv, "sec", strada_new_int(tm->tm_sec));
    strada_hash_set_take(hash->value.hv, "min", strada_new_int(tm->tm_min));
    strada_hash_set_take(hash->value.hv, "hour", strada_new_int(tm->tm_hour));
    strada_hash_set_take(hash->value.hv, "mday", strada_new_int(tm->tm_mday));
    strada_hash_set_take(hash->value.hv, "mon", strada_new_int(tm->tm_mon));      /* 0-11 */
    strada_hash_set_take(hash->value.hv, "year", strada_new_int(tm->tm_year));    /* years since 1900 */
    strada_hash_set_take(hash->value.hv, "wday", strada_new_int(tm->tm_wday));    /* 0=Sunday */
    strada_hash_set_take(hash->value.hv, "yday", strada_new_int(tm->tm_yday));    /* 0-365 */
    strada_hash_set_take(hash->value.hv, "isdst", strada_new_int(tm->tm_isdst));
    return hash;
}

/* mktime - convert time hash back to timestamp */
StradaValue* strada_mktime(StradaValue *time_hash) {
    if (!time_hash || time_hash->type != STRADA_HASH) {
        return strada_new_int(-1);
    }

    struct tm tm = {0};
    StradaValue *v;

    v = strada_hash_get(time_hash->value.hv, "sec");
    if (v) tm.tm_sec = (int)strada_to_int(v);

    v = strada_hash_get(time_hash->value.hv, "min");
    if (v) tm.tm_min = (int)strada_to_int(v);

    v = strada_hash_get(time_hash->value.hv, "hour");
    if (v) tm.tm_hour = (int)strada_to_int(v);

    v = strada_hash_get(time_hash->value.hv, "mday");
    if (v) tm.tm_mday = (int)strada_to_int(v);

    v = strada_hash_get(time_hash->value.hv, "mon");
    if (v) tm.tm_mon = (int)strada_to_int(v);

    v = strada_hash_get(time_hash->value.hv, "year");
    if (v) tm.tm_year = (int)strada_to_int(v);

    v = strada_hash_get(time_hash->value.hv, "isdst");
    if (v) tm.tm_isdst = (int)strada_to_int(v);
    else tm.tm_isdst = -1;  /* Let system determine DST */

    time_t result = mktime(&tm);
    return strada_new_int((int64_t)result);
}

/* strftime - format time according to format string */
StradaValue* strada_strftime(StradaValue *format, StradaValue *time_hash) {
    if (!format) return strada_new_str("");

    char _tb[256];
    const char *fmt = strada_to_str_buf(format, _tb, sizeof(_tb));
    struct tm tm = {0};

    if (time_hash && time_hash->type == STRADA_HASH) {
        StradaValue *v;

        v = strada_hash_get(time_hash->value.hv, "sec");
        if (v) tm.tm_sec = (int)strada_to_int(v);

        v = strada_hash_get(time_hash->value.hv, "min");
        if (v) tm.tm_min = (int)strada_to_int(v);

        v = strada_hash_get(time_hash->value.hv, "hour");
        if (v) tm.tm_hour = (int)strada_to_int(v);

        v = strada_hash_get(time_hash->value.hv, "mday");
        if (v) tm.tm_mday = (int)strada_to_int(v);

        v = strada_hash_get(time_hash->value.hv, "mon");
        if (v) tm.tm_mon = (int)strada_to_int(v);

        v = strada_hash_get(time_hash->value.hv, "year");
        if (v) tm.tm_year = (int)strada_to_int(v);

        v = strada_hash_get(time_hash->value.hv, "wday");
        if (v) tm.tm_wday = (int)strada_to_int(v);

        v = strada_hash_get(time_hash->value.hv, "yday");
        if (v) tm.tm_yday = (int)strada_to_int(v);

        v = strada_hash_get(time_hash->value.hv, "isdst");
        if (v) tm.tm_isdst = (int)strada_to_int(v);
    } else {
        /* Use current time if no hash provided */
        time_t t = time(NULL);
        struct tm *now = localtime(&t);
        if (now) tm = *now;
    }

    char buffer[1024];
    size_t len = strftime(buffer, sizeof(buffer), fmt, &tm);
    if (len == 0) {
        return strada_new_str("");
    }
    return strada_new_str(buffer);
}

/* ctime - convert timestamp to string (like asctime) */
StradaValue* strada_ctime(StradaValue *timestamp) {
    time_t t;
    if (timestamp) {
        t = (time_t)strada_to_int(timestamp);
    } else {
        t = time(NULL);
    }

    char *result = ctime(&t);
    if (!result) return strada_new_str("");

    /* Remove trailing newline */
    size_t len = strlen(result);
    if (len > 0 && result[len-1] == '\n') {
        char *copy = strdup(result);
        copy[len-1] = '\0';
        StradaValue *sv = strada_new_str(copy);
        free(copy);
        return sv;
    }
    return strada_new_str(result);
}

/* sleep - sleep for given seconds */
StradaValue* strada_sleep(StradaValue *seconds) {
    if (!seconds) return strada_new_int(0);
    unsigned int secs = (unsigned int)strada_to_int(seconds);
    unsigned int remaining = sleep(secs);
    return strada_new_int(remaining);
}

/* usleep - sleep for given microseconds */
StradaValue* strada_usleep(StradaValue *usecs) {
    if (!usecs) return strada_new_int(0);
    useconds_t us = (useconds_t)strada_to_int(usecs);
    int result = usleep(us);
    return strada_new_int(result);
}

/* ===== HIGH-RESOLUTION TIME FUNCTIONS ===== */

/* gettimeofday - get current time with microsecond precision */
StradaValue* strada_gettimeofday(void) {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        return strada_new_undef();
    }
    StradaValue *hash = strada_new_hash();
    strada_hash_set_take(hash->value.hv, "sec", strada_new_int(tv.tv_sec));
    strada_hash_set_take(hash->value.hv, "usec", strada_new_int(tv.tv_usec));
    return hash;
}

/* hires_time - get current time as floating point seconds */
StradaValue* strada_hires_time(void) {
    struct timeval tv;
    if (gettimeofday(&tv, NULL) != 0) {
        return strada_new_num(0.0);
    }
    double result = (double)tv.tv_sec + (double)tv.tv_usec / 1000000.0;
    return strada_new_num(result);
}

/* tv_interval - calculate interval between two gettimeofday hashes */
StradaValue* strada_tv_interval(StradaValue *start, StradaValue *end) {
    if (!start || start->type != STRADA_HASH) {
        return strada_new_num(0.0);
    }

    int64_t start_sec = 0, start_usec = 0;
    int64_t end_sec = 0, end_usec = 0;

    StradaValue *v = strada_hash_get(start->value.hv, "sec");
    if (v) start_sec = strada_to_int(v);
    v = strada_hash_get(start->value.hv, "usec");
    if (v) start_usec = strada_to_int(v);

    if (end && end->type == STRADA_HASH) {
        v = strada_hash_get(end->value.hv, "sec");
        if (v) end_sec = strada_to_int(v);
        v = strada_hash_get(end->value.hv, "usec");
        if (v) end_usec = strada_to_int(v);
    } else {
        /* Use current time if end not provided */
        struct timeval tv;
        gettimeofday(&tv, NULL);
        end_sec = tv.tv_sec;
        end_usec = tv.tv_usec;
    }

    double interval = (double)(end_sec - start_sec) +
                      (double)(end_usec - start_usec) / 1000000.0;
    return strada_new_num(interval);
}

/* nanosleep - sleep for nanoseconds */
StradaValue* strada_nanosleep_ns(StradaValue *nanosecs) {
    if (!nanosecs) return strada_new_int(0);
    int64_t ns = strada_to_int(nanosecs);
    struct timespec req, rem;
    req.tv_sec = ns / 1000000000;
    req.tv_nsec = ns % 1000000000;
    int result = nanosleep(&req, &rem);
    if (result != 0) {
        /* Return remaining time if interrupted */
        return strada_new_int(rem.tv_sec * 1000000000 + rem.tv_nsec);
    }
    return strada_new_int(0);
}

/* clock_gettime - get time from specified clock */
StradaValue* strada_clock_gettime(StradaValue *clock_id) {
    clockid_t clk = CLOCK_REALTIME;
    if (clock_id) {
        clk = (clockid_t)strada_to_int(clock_id);
    }
    struct timespec ts;
    if (clock_gettime(clk, &ts) != 0) {
        return strada_new_undef();
    }
    StradaValue *hash = strada_new_hash();
    strada_hash_set_take(hash->value.hv, "sec", strada_new_int(ts.tv_sec));
    strada_hash_set_take(hash->value.hv, "nsec", strada_new_int(ts.tv_nsec));
    return hash;
}

/* clock_getres - get clock resolution */
StradaValue* strada_clock_getres(StradaValue *clock_id) {
    clockid_t clk = CLOCK_REALTIME;
    if (clock_id) {
        clk = (clockid_t)strada_to_int(clock_id);
    }
    struct timespec ts;
    if (clock_getres(clk, &ts) != 0) {
        return strada_new_undef();
    }
    StradaValue *hash = strada_new_hash();
    strada_hash_set_take(hash->value.hv, "sec", strada_new_int(ts.tv_sec));
    strada_hash_set_take(hash->value.hv, "nsec", strada_new_int(ts.tv_nsec));
    return hash;
}

/* ===== ADDITIONAL HELPER FUNCTIONS ===== */

void strada_cstruct_set_int(StradaValue *sv, const char *field, size_t offset, int64_t value) {
    (void)field;  /* unused */
    if (!sv || sv->type != STRADA_CSTRUCT) return;
    char *ptr = (char*)sv->value.ptr;
    *((int*)(ptr + offset)) = (int)value;
}

int64_t strada_cstruct_get_int(StradaValue *sv, const char *field, size_t offset) {
    (void)field;  /* unused */
    if (!sv || sv->type != STRADA_CSTRUCT) return 0;
    char *ptr = (char*)sv->value.ptr;
    return *((int*)(ptr + offset));
}

char* strada_replace_all(const char *str, const char *find, const char *replace) {
    if (!str || !find || !replace) return strdup(str ? str : "");
    
    size_t find_len = strlen(find);
    size_t replace_len = strlen(replace);
    if (find_len == 0) return strdup(str);
    
    /* Count occurrences */
    int count = 0;
    const char *p = str;
    while ((p = strstr(p, find)) != NULL) {
        count++;
        p += find_len;
    }
    
    /* Allocate result */
    size_t result_len = strlen(str) + count * (replace_len - find_len);
    char *result = malloc(result_len + 1);
    char *dest = result;
    
    p = str;
    while (*p) {
        if (strncmp(p, find, find_len) == 0) {
            strcpy(dest, replace);
            dest += replace_len;
            p += find_len;
        } else {
            *dest++ = *p++;
        }
    }
    *dest = '\0';
    return result;
}

void strada_cstruct_set_string(StradaValue *sv, const char *field, size_t offset, const char *value) {
    (void)field;
    if (!sv || sv->type != STRADA_CSTRUCT) return;
    char *ptr = (char*)sv->value.ptr;
    const char *src = value ? value : "";
    size_t src_len = strlen(src);
    /* Bounds check against struct size */
    if (sv->struct_size > 0 && offset + src_len + 1 > sv->struct_size) {
        size_t avail = (offset < sv->struct_size) ? sv->struct_size - offset - 1 : 0;
        memcpy(ptr + offset, src, avail);
        ptr[offset + avail] = '\0';
    } else {
        memcpy(ptr + offset, src, src_len + 1);
    }
}

void strada_cstruct_set_double(StradaValue *sv, const char *field, size_t offset, double value) {
    (void)field;
    if (!sv || sv->type != STRADA_CSTRUCT) return;
    char *ptr = (char*)sv->value.ptr;
    *((double*)(ptr + offset)) = value;
}

char* strada_cstruct_get_string(StradaValue *sv, const char *field, size_t offset) {
    (void)field;
    if (!sv || sv->type != STRADA_CSTRUCT) return "";
    char *ptr = (char*)sv->value.ptr;
    return ptr + offset;
}

double strada_cstruct_get_double(StradaValue *sv, const char *field, size_t offset) {
    (void)field;
    if (!sv || sv->type != STRADA_CSTRUCT) return 0.0;
    char *ptr = (char*)sv->value.ptr;
    return *((double*)(ptr + offset));
}

/* ===== C STRING HELPERS FOR EXTERN FUNCTIONS ===== */

/* Concatenate two C strings, returns newly allocated string (caller must free) */
char* strada_cstr_concat(const char *a, const char *b) {
    if (!a) a = "";
    if (!b) b = "";
    size_t len_a = strlen(a);
    size_t len_b = strlen(b);
    char *result = malloc(len_a + len_b + 1);
    if (!result) return strdup("");
    memcpy(result, a, len_a);
    memcpy(result + len_a, b, len_b + 1);  /* +1 includes null terminator */
    return result;
}

/* Convert int to C string, returns newly allocated string (caller must free) */
char* strada_int_to_cstr(int64_t n) {
    char buf[32];
    snprintf(buf, sizeof(buf), "%ld", (long)n);
    return strdup(buf);
}

/* Convert double to C string, returns newly allocated string (caller must free) */
char* strada_num_to_cstr(double n) {
    char buf[64];
    snprintf(buf, sizeof(buf), "%g", n);
    return strdup(buf);
}

/* ===== PROCESS CONTROL ===== */

StradaValue* strada_fork(void) {
    /* Fork process, returns child PID to parent, 0 to child, -1 on error */
    pid_t pid = fork();
    return strada_new_int(pid);
}

StradaValue* strada_wait(void) {
    /* Wait for any child process, returns child PID or -1 on error */
    int status;
    pid_t pid = wait(&status);
    return strada_new_int(pid);
}

StradaValue* strada_waitpid(StradaValue *pid_val, StradaValue *options_val) {
    /* Wait for specific child process, returns raw status (use strada_exit_status to extract code) */
    pid_t pid = (pid_t)strada_to_int(pid_val);
    int options = options_val ? (int)strada_to_int(options_val) : 0;
    int status;
    pid_t result = waitpid(pid, &status, options);
    if (result == -1) {
        return strada_new_int(-1);
    }
    return strada_new_int(status);
}

StradaValue* strada_getpid(void) {
    /* Get current process ID */
    return strada_new_int(getpid());
}

StradaValue* strada_getppid(void) {
    /* Get parent process ID */
    return strada_new_int(getppid());
}

StradaValue* strada_exit_status(StradaValue *status_val) {
    /* Get exit status from wait status (WEXITSTATUS) */
    int status = (int)strada_to_int(status_val);
    if (WIFEXITED(status)) {
        return strada_new_int(WEXITSTATUS(status));
    }
    return strada_new_int(-1);
}

/* ===== POSIX FUNCTIONS ===== */

StradaValue* strada_getenv(StradaValue *name_val) {
    /* Get environment variable */
    char _tb[256];
    const char *name = strada_to_str_buf(name_val, _tb, sizeof(_tb));
    const char *value = getenv(name);
    if (value == NULL) {
        return strada_new_undef();
    }
    return strada_new_str(value);
}

StradaValue* strada_setenv(StradaValue *name_val, StradaValue *value_val) {
    /* Set environment variable, returns 0 on success */
    char _tb[256];
    const char *name = strada_to_str_buf(name_val, _tb, sizeof(_tb));
    char _tb2[256];
    const char *value = strada_to_str_buf(value_val, _tb2, sizeof(_tb2));
    int result = setenv(name, value, 1);
    return strada_new_int(result);
}

StradaValue* strada_unsetenv(StradaValue *name_val) {
    /* Unset environment variable, returns 0 on success */
    char _tb[256];
    const char *name = strada_to_str_buf(name_val, _tb, sizeof(_tb));
    int result = unsetenv(name);
    return strada_new_int(result);
}

StradaValue* strada_getcwd(void) {
    /* Get current working directory */
    char buf[PATH_MAX];
    if (getcwd(buf, sizeof(buf)) == NULL) {
        return strada_new_undef();
    }
    return strada_new_str(buf);
}

StradaValue* strada_chdir(StradaValue *path_val) {
    /* Change current working directory, returns 0 on success */
    char _tb[PATH_MAX];
    const char *path = strada_to_str_buf(path_val, _tb, sizeof(_tb));
    int result = chdir(path);
    return strada_new_int(result);
}

StradaValue* strada_chroot(StradaValue *path_val) {
    /* Change root directory, returns 0 on success, -1 on error */
    char _tb[PATH_MAX];
    const char *path = strada_to_str_buf(path_val, _tb, sizeof(_tb));
    int result = chroot(path);
    return strada_new_int(result);
}

StradaValue* strada_mkdir(StradaValue *path_val, StradaValue *mode_val) {
    /* Create directory, returns 0 on success */
    char _tb[PATH_MAX];
    const char *path = strada_to_str_buf(path_val, _tb, sizeof(_tb));
    mode_t mode = (mode_t)strada_to_int(mode_val);
    int result = mkdir(path, mode);
    return strada_new_int(result);
}

StradaValue* strada_rmdir(StradaValue *path_val) {
    /* Remove directory, returns 0 on success */
    char _tb[PATH_MAX];
    const char *path = strada_to_str_buf(path_val, _tb, sizeof(_tb));
    int result = rmdir(path);
    return strada_new_int(result);
}

StradaValue* strada_unlink(StradaValue *path_val) {
    /* Remove file, returns 0 on success */
    char _tb[PATH_MAX];
    const char *path = strada_to_str_buf(path_val, _tb, sizeof(_tb));
    int result = unlink(path);
    return strada_new_int(result);
}

StradaValue* strada_link(StradaValue *oldpath_val, StradaValue *newpath_val) {
    /* Create hard link, returns 0 on success */
    char _tb[PATH_MAX];
    const char *oldpath = strada_to_str_buf(oldpath_val, _tb, sizeof(_tb));
    char _tb2[PATH_MAX];
    const char *newpath = strada_to_str_buf(newpath_val, _tb2, sizeof(_tb2));
    int result = link(oldpath, newpath);
    return strada_new_int(result);
}

StradaValue* strada_symlink(StradaValue *target_val, StradaValue *linkpath_val) {
    /* Create symbolic link, returns 0 on success */
    char _tb[PATH_MAX];
    const char *target = strada_to_str_buf(target_val, _tb, sizeof(_tb));
    char _tb2[PATH_MAX];
    const char *linkpath = strada_to_str_buf(linkpath_val, _tb2, sizeof(_tb2));
    int result = symlink(target, linkpath);
    return strada_new_int(result);
}

StradaValue* strada_readlink(StradaValue *path_val) {
    /* Read symbolic link target */
    char _tb[PATH_MAX];
    const char *path = strada_to_str_buf(path_val, _tb, sizeof(_tb));
    char buf[PATH_MAX];
    ssize_t len = readlink(path, buf, sizeof(buf) - 1);
    if (len == -1) {
        return strada_new_undef();
    }
    buf[len] = '\0';
    return strada_new_str(buf);
}

StradaValue* strada_rename(StradaValue *oldpath_val, StradaValue *newpath_val) {
    /* Rename file or directory, returns 0 on success */
    char _tb[PATH_MAX];
    const char *oldpath = strada_to_str_buf(oldpath_val, _tb, sizeof(_tb));
    char _tb2[PATH_MAX];
    const char *newpath = strada_to_str_buf(newpath_val, _tb2, sizeof(_tb2));
    int result = rename(oldpath, newpath);
    return strada_new_int(result);
}

StradaValue* strada_chmod(StradaValue *path_val, StradaValue *mode_val) {
    /* Change file mode, returns 0 on success */
    char _tb[PATH_MAX];
    const char *path = strada_to_str_buf(path_val, _tb, sizeof(_tb));
    mode_t mode = (mode_t)strada_to_int(mode_val);
    int result = chmod(path, mode);
    return strada_new_int(result);
}

StradaValue* strada_access(StradaValue *path_val, StradaValue *mode_val) {
    /* Check file accessibility, returns 0 if accessible */
    char _tb[PATH_MAX];
    const char *path = strada_to_str_buf(path_val, _tb, sizeof(_tb));
    int mode = (int)strada_to_int(mode_val);
    int result = access(path, mode);
    return strada_new_int(result);
}

StradaValue* strada_umask(StradaValue *mask_val) {
    /* Set file creation mask, returns previous mask */
    mode_t mask = (mode_t)strada_to_int(mask_val);
    return strada_new_int(umask(mask));
}

StradaValue* strada_getuid(void) {
    /* Get real user ID */
    return strada_new_int(getuid());
}

StradaValue* strada_geteuid(void) {
    /* Get effective user ID */
    return strada_new_int(geteuid());
}

StradaValue* strada_getgid(void) {
    /* Get real group ID */
    return strada_new_int(getgid());
}

StradaValue* strada_getegid(void) {
    /* Get effective group ID */
    return strada_new_int(getegid());
}

StradaValue* strada_kill(StradaValue *pid_val, StradaValue *sig_val) {
    /* Send signal to process, returns 0 on success */
    pid_t pid = (pid_t)strada_to_int(pid_val);
    int sig = (int)strada_to_int(sig_val);
    return strada_new_int(kill(pid, sig));
}

StradaValue* strada_alarm(StradaValue *seconds_val) {
    /* Set alarm timer, returns seconds remaining from previous alarm */
    unsigned int seconds = (unsigned int)strada_to_int(seconds_val);
    return strada_new_int(alarm(seconds));
}

/* ===== SIGNAL HANDLING ===== */

#define STRADA_MAX_SIGNALS 64

/* Store handler function pointers indexed by signal number */
static void (*strada_signal_handlers[STRADA_MAX_SIGNALS])(StradaValue *);

/* Map signal name to number */
static int strada_signal_name_to_num(const char *name) {
    if (strcmp(name, "INT") == 0 || strcmp(name, "SIGINT") == 0) return SIGINT;
    if (strcmp(name, "TERM") == 0 || strcmp(name, "SIGTERM") == 0) return SIGTERM;
    if (strcmp(name, "HUP") == 0 || strcmp(name, "SIGHUP") == 0) return SIGHUP;
    if (strcmp(name, "QUIT") == 0 || strcmp(name, "SIGQUIT") == 0) return SIGQUIT;
    if (strcmp(name, "USR1") == 0 || strcmp(name, "SIGUSR1") == 0) return SIGUSR1;
    if (strcmp(name, "USR2") == 0 || strcmp(name, "SIGUSR2") == 0) return SIGUSR2;
    if (strcmp(name, "ALRM") == 0 || strcmp(name, "SIGALRM") == 0) return SIGALRM;
    if (strcmp(name, "PIPE") == 0 || strcmp(name, "SIGPIPE") == 0) return SIGPIPE;
    if (strcmp(name, "CHLD") == 0 || strcmp(name, "SIGCHLD") == 0) return SIGCHLD;
    if (strcmp(name, "CONT") == 0 || strcmp(name, "SIGCONT") == 0) return SIGCONT;
    if (strcmp(name, "STOP") == 0 || strcmp(name, "SIGSTOP") == 0) return SIGSTOP;
    if (strcmp(name, "TSTP") == 0 || strcmp(name, "SIGTSTP") == 0) return SIGTSTP;
    if (strcmp(name, "SEGV") == 0 || strcmp(name, "SIGSEGV") == 0) return SIGSEGV;
    if (strcmp(name, "ABRT") == 0 || strcmp(name, "SIGABRT") == 0) return SIGABRT;
    if (strcmp(name, "FPE") == 0 || strcmp(name, "SIGFPE") == 0) return SIGFPE;
    if (strcmp(name, "ILL") == 0 || strcmp(name, "SIGILL") == 0) return SIGILL;
    if (strcmp(name, "BUS") == 0 || strcmp(name, "SIGBUS") == 0) return SIGBUS;
    if (strcmp(name, "WINCH") == 0 || strcmp(name, "SIGWINCH") == 0) return SIGWINCH;
    return -1;
}

/* C signal handler wrapper that calls Strada handler */
static void strada_signal_wrapper(int signum) {
    if (signum >= 0 && signum < STRADA_MAX_SIGNALS && strada_signal_handlers[signum]) {
        /* Call the Strada handler with signal number as argument */
        StradaValue *arg = strada_new_int(signum);
        strada_signal_handlers[signum](arg);
        strada_decref(arg);
    }
}

StradaValue* strada_signal(StradaValue *sig_name, StradaValue *handler) {
    /* Set signal handler. Handler can be:
     * - A function reference: \&my_handler
     * - "IGNORE" string: ignore the signal
     * - "DEFAULT" string: restore default behavior
     * Returns the previous handler or undef
     */
    char _tb[256];
    const char *name = strada_to_str_buf(sig_name, _tb, sizeof(_tb));
    int signum = strada_signal_name_to_num(name);

    if (signum < 0 || signum >= STRADA_MAX_SIGNALS) {
        return strada_new_undef();  /* Unknown signal */
    }

    /* Store previous handler for return value */
    void (*old_handler)(StradaValue *) = strada_signal_handlers[signum];

    /* Handle special string values */
    if (handler->type == STRADA_STR) {
        char _tb2[256];
        const char *action = strada_to_str_buf(handler, _tb2, sizeof(_tb2));
        if (strcmp(action, "IGNORE") == 0 || strcmp(action, "SIG_IGN") == 0) {
            signal(signum, SIG_IGN);
            strada_signal_handlers[signum] = NULL;
        } else if (strcmp(action, "DEFAULT") == 0 || strcmp(action, "SIG_DFL") == 0) {
            signal(signum, SIG_DFL);
            strada_signal_handlers[signum] = NULL;
        }
    } else if (handler->type == STRADA_CPOINTER) {
        /* Function pointer passed via \&func_name */
        strada_signal_handlers[signum] = (void (*)(StradaValue *))handler->value.ptr;
        signal(signum, strada_signal_wrapper);
    } else {
        /* Assume it's a function pointer value */
        strada_signal_handlers[signum] = (void (*)(StradaValue *))strada_to_pointer(handler);
        signal(signum, strada_signal_wrapper);
    }

    /* Return previous handler as cpointer, or undef if none */
    if (old_handler) {
        return strada_cpointer_new((void *)old_handler);
    }
    return strada_new_undef();
}

StradaValue* strada_stat(StradaValue *path_val) {
    /* Get file status, returns hash with file info or undef on error */
    char _tb[PATH_MAX];
    const char *path = strada_to_str_buf(path_val, _tb, sizeof(_tb));
    struct stat st;
    if (stat(path, &st) == -1) {
        return strada_new_undef();
    }
    StradaValue *hash = strada_new_hash();
    strada_hash_set_take(hash->value.hv, "dev", strada_new_int(st.st_dev));
    strada_hash_set_take(hash->value.hv, "ino", strada_new_int(st.st_ino));
    strada_hash_set_take(hash->value.hv, "mode", strada_new_int(st.st_mode));
    strada_hash_set_take(hash->value.hv, "nlink", strada_new_int(st.st_nlink));
    strada_hash_set_take(hash->value.hv, "uid", strada_new_int(st.st_uid));
    strada_hash_set_take(hash->value.hv, "gid", strada_new_int(st.st_gid));
    strada_hash_set_take(hash->value.hv, "rdev", strada_new_int(st.st_rdev));
    strada_hash_set_take(hash->value.hv, "size", strada_new_int(st.st_size));
    strada_hash_set_take(hash->value.hv, "atime", strada_new_int(st.st_atime));
    strada_hash_set_take(hash->value.hv, "mtime", strada_new_int(st.st_mtime));
    strada_hash_set_take(hash->value.hv, "ctime", strada_new_int(st.st_ctime));
    strada_hash_set_take(hash->value.hv, "blksize", strada_new_int(st.st_blksize));
    strada_hash_set_take(hash->value.hv, "blocks", strada_new_int(st.st_blocks));
    return hash;
}

StradaValue* strada_lstat(StradaValue *path_val) {
    /* Get file status (don't follow symlinks) */
    char _tb[PATH_MAX];
    const char *path = strada_to_str_buf(path_val, _tb, sizeof(_tb));
    struct stat st;
    if (lstat(path, &st) == -1) {
        return strada_new_undef();
    }
    StradaValue *hash = strada_new_hash();
    strada_hash_set_take(hash->value.hv, "dev", strada_new_int(st.st_dev));
    strada_hash_set_take(hash->value.hv, "ino", strada_new_int(st.st_ino));
    strada_hash_set_take(hash->value.hv, "mode", strada_new_int(st.st_mode));
    strada_hash_set_take(hash->value.hv, "nlink", strada_new_int(st.st_nlink));
    strada_hash_set_take(hash->value.hv, "uid", strada_new_int(st.st_uid));
    strada_hash_set_take(hash->value.hv, "gid", strada_new_int(st.st_gid));
    strada_hash_set_take(hash->value.hv, "rdev", strada_new_int(st.st_rdev));
    strada_hash_set_take(hash->value.hv, "size", strada_new_int(st.st_size));
    strada_hash_set_take(hash->value.hv, "atime", strada_new_int(st.st_atime));
    strada_hash_set_take(hash->value.hv, "mtime", strada_new_int(st.st_mtime));
    strada_hash_set_take(hash->value.hv, "ctime", strada_new_int(st.st_ctime));
    strada_hash_set_take(hash->value.hv, "blksize", strada_new_int(st.st_blksize));
    strada_hash_set_take(hash->value.hv, "blocks", strada_new_int(st.st_blocks));
    return hash;
}

StradaValue* strada_isatty(StradaValue *fd_val) {
    /* Check if fd is a terminal */
    int fd = (int)strada_to_int(fd_val);
    return strada_new_int(isatty(fd));
}

StradaValue* strada_strerror(StradaValue *errnum_val) {
    /* Get error string for errno value */
    int errnum = (int)strada_to_int(errnum_val);
    return strada_new_str(strerror(errnum));
}

StradaValue* strada_errno(void) {
    /* Get current errno value */
    return strada_new_int(errno);
}

/* ===== PIPE AND IPC SUPPORT ===== */

StradaValue* strada_pipe(void) {
    /* Create a pipe, returns array [read_fd, write_fd] or undef on error */
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        return strada_new_undef();
    }
    StradaValue *arr = strada_new_array();
    strada_array_push_take(arr->value.av, strada_new_int(pipefd[0]));
    strada_array_push_take(arr->value.av, strada_new_int(pipefd[1]));
    return arr;
}

StradaValue* strada_dup2(StradaValue *oldfd_val, StradaValue *newfd_val) {
    /* Duplicate file descriptor */
    int oldfd = (int)strada_to_int(oldfd_val);
    int newfd = (int)strada_to_int(newfd_val);
    int result = dup2(oldfd, newfd);
    return strada_new_int(result);
}

StradaValue* strada_close_fd(StradaValue *fd_val) {
    /* Close a file descriptor */
    int fd = (int)strada_to_int(fd_val);
    int result = close(fd);
    return strada_new_int(result);
}

StradaValue* strada_exec(StradaValue *cmd_val) {
    /* Execute command, replacing current process */
    char *cmd = strada_to_str(cmd_val);
    execlp("/bin/sh", "sh", "-c", cmd, (char*)NULL);
    /* If we get here, exec failed */
    free(cmd);
    return strada_new_int(-1);
}

StradaValue* strada_exec_argv(StradaValue *program, StradaValue *args_arr) {
    /* Execute with explicit program and argv array */
    char *prog = strada_to_str(program);

    /* Count args */
    StradaArray *args = strada_deref_array(args_arr);
    if (!args) {
        execlp(prog, prog, (char*)NULL);
        /* If we get here, exec failed */
        free(prog);
        return strada_new_int(-1);
    }

    /* Build argv array */
    size_t argc = args->size;
    char **argv = malloc((argc + 1) * sizeof(char*));
    for (size_t i = 0; i < argc; i++) {
        argv[i] = strada_to_str(args->elements[args->head + i]);
    }
    argv[argc] = NULL;

    execvp(prog, argv);
    /* If we get here, exec failed - free all allocated strings */
    for (size_t i = 0; i < argc; i++) {
        free(argv[i]);
    }
    free(argv);
    free(prog);
    return strada_new_int(-1);
}

StradaValue* strada_system(StradaValue *cmd_val) {
    /* Run command via shell and wait for completion (like Perl's system()) */
    /* Returns -1 on fork failure, otherwise the exit status */
    char *cmd = strada_to_str(cmd_val);

    pid_t pid = fork();
    if (pid == -1) {
        free(cmd);
        return strada_new_int(-1);
    }

    if (pid == 0) {
        /* Child process - cmd will be cleaned up by OS on exec/exit */
        execlp("/bin/sh", "sh", "-c", cmd, (char*)NULL);
        _exit(127);  /* exec failed */
    }

    /* Parent: wait for child and clean up */
    free(cmd);
    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
        return strada_new_int(WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        return strada_new_int(128 + WTERMSIG(status));
    }
    return strada_new_int(-1);
}

StradaValue* strada_system_argv(StradaValue *program, StradaValue *args_arr) {
    /* Run program with explicit argv and wait for completion */
    char *prog = strada_to_str(program);

    pid_t pid = fork();
    if (pid == -1) {
        free(prog);
        return strada_new_int(-1);
    }

    if (pid == 0) {
        /* Child process - memory will be cleaned up by OS on exec/exit */
        StradaArray *args = strada_deref_array(args_arr);
        if (!args) {
            execlp(prog, prog, (char*)NULL);
            _exit(127);
        }

        size_t argc = args->size;
        char **argv = malloc((argc + 1) * sizeof(char*));
        for (size_t i = 0; i < argc; i++) {
            argv[i] = strada_to_str(args->elements[args->head + i]);
        }
        argv[argc] = NULL;

        execvp(prog, argv);
        _exit(127);  /* exec failed */
    }

    /* Parent: wait for child and clean up */
    free(prog);
    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
        return strada_new_int(WEXITSTATUS(status));
    } else if (WIFSIGNALED(status)) {
        return strada_new_int(128 + WTERMSIG(status));
    }
    return strada_new_int(-1);
}

StradaValue* strada_read_fd(StradaValue *fd_val, StradaValue *size_val) {
    /* Read from file descriptor, returns string */
    int fd = (int)strada_to_int(fd_val);
    size_t size = (size_t)strada_to_int(size_val);

    char *buf = malloc(size + 1);
    if (!buf) return strada_new_str("");

    ssize_t n = read(fd, buf, size);
    if (n <= 0) {
        free(buf);
        return strada_new_str("");
    }
    buf[n] = '\0';
    StradaValue *result = strada_new_str(buf);
    free(buf);
    return result;
}

StradaValue* strada_open_fd(StradaValue *filename_val, StradaValue *mode_val) {
    /* Open file and return raw file descriptor (int) */
    char _tb[PATH_MAX];
    const char *filename = strada_to_str_buf(filename_val, _tb, sizeof(_tb));
    char _tb2[256];
    const char *mode = strada_to_str_buf(mode_val, _tb2, sizeof(_tb2));
    int flags = O_RDONLY;
    int fd = -1;

    if (strcmp(mode, "w") == 0) {
        flags = O_WRONLY | O_CREAT | O_TRUNC;
        fd = open(filename, flags, 0644);
    } else if (strcmp(mode, "r") == 0) {
        flags = O_RDONLY;
        fd = open(filename, flags);
    } else if (strcmp(mode, "a") == 0) {
        flags = O_WRONLY | O_CREAT | O_APPEND;
        fd = open(filename, flags, 0644);
    } else if (strcmp(mode, "rw") == 0) {
        flags = O_RDWR | O_CREAT;
        fd = open(filename, flags, 0644);
    }

    return strada_new_int(fd);
}

StradaValue* strada_write_fd(StradaValue *fd_val, StradaValue *data_val) {
    /* Write to file descriptor, returns bytes written */
    int fd = (int)strada_to_int(fd_val);
    char _tb[256];
    const char *data = strada_to_str_buf(data_val, _tb, sizeof(_tb));
    size_t len = strlen(data);
    ssize_t n = write(fd, data, len);
    return strada_new_int(n);
}

StradaValue* strada_read_all_fd(StradaValue *fd_val) {
    /* Read all available data from file descriptor */
    int fd = (int)strada_to_int(fd_val);

    size_t capacity = 4096;
    size_t total = 0;
    char *buf = malloc(capacity);
    if (!buf) return strada_new_str("");

    while (1) {
        if (total + 1024 > capacity) {
            capacity *= 2;
            char *newbuf = realloc(buf, capacity);
            if (!newbuf) {
                free(buf);
                return strada_new_str("");
            }
            buf = newbuf;
        }
        ssize_t n = read(fd, buf + total, 1024);
        if (n <= 0) break;
        total += n;
    }
    buf[total] = '\0';
    StradaValue *result = strada_new_str(buf);
    free(buf);
    return result;
}

StradaValue* strada_fdopen_read(StradaValue *fd_val) {
    /* Convert fd to a filehandle for reading */
    int fd = (int)strada_to_int(fd_val);
    FILE *fp = fdopen(fd, "r");
    if (!fp) return strada_new_undef();

    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_FILEHANDLE;
    sv->refcount = 1;
    sv->value.fh = fp;
    return sv;
}

StradaValue* strada_fdopen_write(StradaValue *fd_val) {
    /* Convert fd to a filehandle for writing */
    int fd = (int)strada_to_int(fd_val);
    FILE *fp = fdopen(fd, "w");
    if (!fp) return strada_new_undef();

    StradaValue *sv = strada_value_alloc();
    sv->type = STRADA_FILEHANDLE;
    sv->refcount = 1;
    sv->value.fh = fp;
    return sv;
}

/* ===== PROCESS NAME FUNCTIONS ===== */

StradaValue* strada_setprocname(StradaValue *name_val) {
    /* Set the process name (shown in ps, top, /proc/self/comm) */
    /* On Linux, uses prctl(PR_SET_NAME). Max 15 chars + null. */
    char _tb[256];
    const char *name = strada_to_str_buf(name_val, _tb, sizeof(_tb));
#ifdef __linux__
    int result = prctl(PR_SET_NAME, name, 0, 0, 0);
#else
    /* macOS/BSD: setprocname not available via prctl */
    int result = -1;
#endif
    return strada_new_int(result);
}

StradaValue* strada_getprocname(void) {
    /* Get the current process name */
    char name[16];
#ifdef __linux__
    if (prctl(PR_GET_NAME, name, 0, 0, 0) == 0) {
        return strada_new_str(name);
    }
#endif
    return strada_new_str("");
}

/* Global pointer to original argv for setproctitle */
static char **strada_orig_argv = NULL;
static int strada_orig_argc = 0;
static char *strada_argv_end = NULL;

void strada_init_proctitle(int argc, char **argv) {
    /* Call this from main() to enable setproctitle */
    strada_orig_argc = argc;
    strada_orig_argv = argv;
    if (argc > 0) {
        /* Find end of argv/environ area */
        strada_argv_end = argv[0];
        for (int i = 0; i < argc; i++) {
            if (argv[i]) {
                char *end = argv[i] + strlen(argv[i]) + 1;
                if (end > strada_argv_end) strada_argv_end = end;
            }
        }
        /* Also check environ */
        extern char **environ;
        for (char **env = environ; *env; env++) {
            char *end = *env + strlen(*env) + 1;
            if (end > strada_argv_end) strada_argv_end = end;
        }
    }
}

StradaValue* strada_setproctitle(StradaValue *title_val) {
    /* Set the full process title (shown in ps -f) */
    /* This overwrites argv[0] and potentially more */
    if (!strada_orig_argv || strada_orig_argc == 0) {
        return strada_new_int(-1);
    }

    char _tb[256];
    const char *title = strada_to_str_buf(title_val, _tb, sizeof(_tb));
    size_t max_len = strada_argv_end - strada_orig_argv[0] - 1;
    size_t title_len = strlen(title);

    if (title_len > max_len) {
        title_len = max_len;
    }

    memset(strada_orig_argv[0], 0, max_len + 1);
    memcpy(strada_orig_argv[0], title, title_len);

    /* Clear other argv entries */
    for (int i = 1; i < strada_orig_argc; i++) {
        strada_orig_argv[i] = NULL;
    }

    return strada_new_int(0);
}

StradaValue* strada_getproctitle(void) {
    /* Get the current process title from /proc/self/cmdline */
    FILE *f = fopen("/proc/self/cmdline", "r");
    if (!f) {
        /* Fallback: return argv[0] if available */
        if (strada_orig_argv && strada_orig_argv[0]) {
            return strada_new_str(strada_orig_argv[0]);
        }
        return strada_new_str("");
    }

    char buf[4096];
    size_t n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);

    if (n == 0) {
        return strada_new_str("");
    }
    buf[n] = '\0';

    /* cmdline has null separators - just return the first part (the title) */
    return strada_new_str(buf);
}

/* ===== OOP - BLESSED REFERENCES (like Perl's bless) ===== */

/* Method registry: package -> method name -> function pointer */
#define OOP_MAX_PACKAGES 256
#define OOP_MAX_METHODS 256
#define OOP_MAX_NAME_LEN 256
#define OOP_MAX_PARENTS 16  /* Max parents for multiple inheritance */

typedef struct {
    char name[OOP_MAX_NAME_LEN];
    StradaMethod func;
} OopMethod;

typedef struct {
    char op[16];           /* "+", "-", ".", "\"\"", "neg", etc. */
    StradaMethod func;     /* The overload handler */
} OverloadEntry;

#define OOP_MAX_OVERLOADS 32
#define OOP_MAX_MODIFIERS 64

typedef struct {
    char method_name[OOP_MAX_NAME_LEN];
    int type;  /* 1=before, 2=after, 3=around */
    StradaMethod func;
} OopModifier;

typedef struct {
    char name[OOP_MAX_NAME_LEN];
    char parents[OOP_MAX_PARENTS][OOP_MAX_NAME_LEN];  /* Multiple parents */
    int parent_count;
    OopMethod methods[OOP_MAX_METHODS];
    int method_count;
    OverloadEntry overloads[OOP_MAX_OVERLOADS];
    int overload_count;
    OopModifier modifiers[OOP_MAX_MODIFIERS];
    int modifier_count;
} OopPackage;

static OopPackage oop_packages[OOP_MAX_PACKAGES];
static int oop_package_count = 0;
static int oop_initialized = 0;
static int oop_any_modifiers = 0;  /* Set to 1 when any modifier is registered */
static char oop_current_package[OOP_MAX_NAME_LEN] = "";
static char oop_current_method_package[OOP_MAX_NAME_LEN] = "";  /* For SUPER:: */
static int oop_destroying = 0;  /* Prevent recursive DESTROY */

/* ===== PACKAGE LOOKUP HASH TABLE ===== */
/* Direct-mapped hash for O(1) package lookup instead of linear scan */
#define OOP_PKG_HASH_SIZE 128
static int oop_pkg_hash[OOP_PKG_HASH_SIZE];  /* index into oop_packages[], -1 = empty */
static int oop_pkg_hash_initialized = 0;

static void oop_pkg_hash_init(void) {
    memset(oop_pkg_hash, -1, sizeof(oop_pkg_hash));
    oop_pkg_hash_initialized = 1;
}

static unsigned int oop_hash_name(const char *name) {
    unsigned int h = 5381;
    for (const char *p = name; *p; p++) {
        h = ((h << 5) + h) + (unsigned char)*p;
    }
    return h & (OOP_PKG_HASH_SIZE - 1);
}

/* ===== METHOD DISPATCH CACHE ===== */
/* Direct-mapped cache for fast method lookups */
#define METHOD_CACHE_SIZE 1024
typedef struct {
    const char *package;   /* pointer comparison (packages are static) */
    char method[OOP_MAX_NAME_LEN];
    StradaMethod func;
} MethodCacheEntry;
static MethodCacheEntry method_cache[METHOD_CACHE_SIZE];
static int method_cache_initialized = 0;

static void method_cache_init(void) {
    memset(method_cache, 0, sizeof(method_cache));
    method_cache_initialized = 1;
}

static void method_cache_invalidate(void) {
    memset(method_cache, 0, sizeof(method_cache));
}

/* ===== ISA RESULT CACHE ===== */
/* Direct-mapped cache for isa() lookups to avoid repeated inheritance walks.
 * pkg uses pointer comparison (blessed name is interned in OOP metadata).
 * target uses strcmp (caller-provided string may be freed and address reused). */
#define ISA_CACHE_SIZE 256
typedef struct {
    const char *pkg;     /* pointer comparison (interned in OOP metadata) */
    char target[OOP_MAX_NAME_LEN]; /* string comparison (copied) */
    int result;
    int valid;
} IsaCacheEntry;
static IsaCacheEntry isa_cache[ISA_CACHE_SIZE];

static void isa_cache_invalidate(void) {
    memset(isa_cache, 0, sizeof(isa_cache));
}

/* Forward declaration */
static OopPackage* oop_get_or_create_package(const char *name);

void strada_set_package(const char *package) {
    /* Set the current package context (like Perl's package declaration) */
    if (!package) {
        oop_current_package[0] = '\0';
        return;
    }
    strncpy(oop_current_package, package, OOP_MAX_NAME_LEN - 1);
    oop_current_package[OOP_MAX_NAME_LEN - 1] = '\0';

    /* Ensure package exists in registry */
    if (!oop_initialized) strada_oop_init();
    oop_get_or_create_package(package);
}

const char* strada_current_package(void) {
    /* Get the current package name */
    if (oop_current_package[0] == '\0') {
        return NULL;
    }
    return oop_current_package;
}

void strada_oop_init(void) {
    if (oop_initialized) return;
    memset(oop_packages, 0, sizeof(oop_packages));
    oop_package_count = 0;
    oop_pkg_hash_init();
    method_cache_init();
    oop_initialized = 1;
}

static OopPackage* oop_find_package(const char *name) {
    if (!oop_pkg_hash_initialized) oop_pkg_hash_init();
    /* Fast hash lookup first */
    unsigned int h = oop_hash_name(name);
    int idx = oop_pkg_hash[h];
    if (idx >= 0 && idx < oop_package_count && strcmp(oop_packages[idx].name, name) == 0) {
        return &oop_packages[idx];
    }
    /* Hash collision or miss — linear scan fallback */
    for (int i = 0; i < oop_package_count; i++) {
        if (strcmp(oop_packages[i].name, name) == 0) {
            /* Update hash for next time */
            oop_pkg_hash[h] = i;
            return &oop_packages[i];
        }
    }
    return NULL;
}

static OopPackage* oop_get_or_create_package(const char *name) {
    OopPackage *pkg = oop_find_package(name);
    if (pkg) return pkg;

    if (oop_package_count >= OOP_MAX_PACKAGES) {
        fprintf(stderr, "Error: Too many packages (max %d)\n", OOP_MAX_PACKAGES);
        exit(1);
    }

    int idx = oop_package_count;
    pkg = &oop_packages[oop_package_count++];
    strncpy(pkg->name, name, OOP_MAX_NAME_LEN - 1);
    pkg->name[OOP_MAX_NAME_LEN - 1] = '\0';
    pkg->parent_count = 0;
    pkg->method_count = 0;

    /* Register in package hash table */
    if (!oop_pkg_hash_initialized) oop_pkg_hash_init();
    unsigned int h = oop_hash_name(name);
    oop_pkg_hash[h] = idx;

    return pkg;
}

StradaValue* strada_bless(StradaValue *ref, const char *package) {
    /* Bless a reference into a package (like Perl's bless) */
    if (!ref || !package) return ref;

    strada_check_debug_bless();
    if (strada_debug_bless) {
        fprintf(stderr, "[BLESS] ref=%p package='%s' (old_pkg=%s)\n",
                (void*)ref, package, SV_BLESSED(ref) ? SV_BLESSED(ref) : "NULL");
    }

    /* Initialize OOP system if needed */
    if (!oop_initialized) strada_oop_init();

    /* Ensure package exists in registry */
    oop_get_or_create_package(package);

    /* Free old blessed_package if exists */
    if (SV_BLESSED(ref)) {
        strada_intern_release(ref->meta->blessed_package);
    }

    /* Set the blessed package — interned since package names are a small fixed set */
    strada_ensure_meta(ref)->blessed_package = strada_intern_str(package);

    return ref;
}

StradaValue* strada_blessed(StradaValue *ref) {
    /* Return the package name this ref is blessed into, or undef */
    const char *bp = SV_BLESSED(ref);
    if (!ref || !bp) {
        return strada_new_undef();
    }
    return strada_new_str(bp);
}

void strada_inherit(const char *child, const char *parent) {
    /* Add a parent to child's inheritance list (supports multiple inheritance) */
    if (!child || !parent) return;

    if (!oop_initialized) strada_oop_init();

    OopPackage *pkg = oop_get_or_create_package(child);

    /* Check if already inheriting from this parent */
    for (int i = 0; i < pkg->parent_count; i++) {
        if (strcmp(pkg->parents[i], parent) == 0) {
            return;  /* Already inherited */
        }
    }

    if (pkg->parent_count >= OOP_MAX_PARENTS) {
        fprintf(stderr, "Error: Too many parents for package '%s' (max %d)\n",
                child, OOP_MAX_PARENTS);
        exit(1);
    }

    strncpy(pkg->parents[pkg->parent_count], parent, OOP_MAX_NAME_LEN - 1);
    pkg->parents[pkg->parent_count][OOP_MAX_NAME_LEN - 1] = '\0';
    pkg->parent_count++;

    /* Invalidate isa cache since inheritance hierarchy changed */
    isa_cache_invalidate();

    /* Ensure parent exists too */
    oop_get_or_create_package(parent);
}

void strada_inherit_from(const char *parent) {
    /* Inherit from parent using current package as child (like Perl's use base) */
    if (!parent) return;

    const char *child = strada_current_package();
    if (!child) {
        fprintf(stderr, "Error: inherit() with one argument requires package() to be set first\n");
        exit(1);
    }

    strada_inherit(child, parent);
}

void strada_method_register(const char *package, const char *name, StradaMethod func) {
    /* Register a method for a package */
    if (!package || !name || !func) return;

    if (!oop_initialized) strada_oop_init();

    /* Invalidate method cache — method registration is rare, so full flush is fine */
    method_cache_invalidate();

    OopPackage *pkg = oop_get_or_create_package(package);

    if (pkg->method_count >= OOP_MAX_METHODS) {
        fprintf(stderr, "Error: Too many methods in package %s (max %d)\n",
                package, OOP_MAX_METHODS);
        exit(1);
    }

    /* Check if method already exists */
    for (int i = 0; i < pkg->method_count; i++) {
        if (strcmp(pkg->methods[i].name, name) == 0) {
            /* Update existing method */
            pkg->methods[i].func = func;
            return;
        }
    }

    /* Add new method */
    OopMethod *m = &pkg->methods[pkg->method_count++];
    strncpy(m->name, name, OOP_MAX_NAME_LEN - 1);
    m->name[OOP_MAX_NAME_LEN - 1] = '\0';
    m->func = func;
}

/* Method modifier registration */
void strada_modifier_register(const char *package, const char *method,
                              int type, StradaMethod func) {
    if (!package || !method || !func) return;
    if (!oop_initialized) strada_oop_init();

    OopPackage *pkg = oop_get_or_create_package(package);
    if (pkg->modifier_count >= OOP_MAX_MODIFIERS) {
        fprintf(stderr, "Error: Too many modifiers in package %s (max %d)\n",
                package, OOP_MAX_MODIFIERS);
        exit(1);
    }

    OopModifier *mod = &pkg->modifiers[pkg->modifier_count++];
    strncpy(mod->method_name, method, OOP_MAX_NAME_LEN - 1);
    mod->method_name[OOP_MAX_NAME_LEN - 1] = '\0';
    mod->type = type;
    mod->func = func;

    oop_any_modifiers = 1;
}

/* Operator overloading */

void strada_overload_register(const char *package, const char *op, StradaMethod func) {
    if (!package || !op || !func) return;
    if (!oop_initialized) strada_oop_init();

    OopPackage *pkg = oop_get_or_create_package(package);

    /* Check if overload already exists */
    for (int i = 0; i < pkg->overload_count; i++) {
        if (strcmp(pkg->overloads[i].op, op) == 0) {
            pkg->overloads[i].func = func;
            return;
        }
    }

    if (pkg->overload_count >= OOP_MAX_OVERLOADS) {
        fprintf(stderr, "Error: Too many overloads in package %s (max %d)\n",
                package, OOP_MAX_OVERLOADS);
        exit(1);
    }

    OverloadEntry *e = &pkg->overloads[pkg->overload_count++];
    strncpy(e->op, op, sizeof(e->op) - 1);
    e->op[sizeof(e->op) - 1] = '\0';
    e->func = func;
}

static StradaMethod oop_overload_lookup_in(const char *package, const char *op,
                                            const char *visited[], int *visited_count) {
    if (!package || !op || !package[0]) return NULL;

    /* Check if already visited */
    for (int i = 0; i < *visited_count; i++) {
        if (strcmp(visited[i], package) == 0) return NULL;
    }
    if (*visited_count < 64) {
        visited[(*visited_count)++] = package;
    }

    OopPackage *pkg = oop_find_package(package);
    if (!pkg) return NULL;

    /* Look for overload in this package */
    for (int i = 0; i < pkg->overload_count; i++) {
        if (strcmp(pkg->overloads[i].op, op) == 0) {
            return pkg->overloads[i].func;
        }
    }

    /* Search parents */
    for (int i = 0; i < pkg->parent_count; i++) {
        StradaMethod func = oop_overload_lookup_in(pkg->parents[i], op,
                                                    visited, visited_count);
        if (func) return func;
    }

    return NULL;
}

static StradaMethod strada_overload_lookup(const char *package, const char *op) {
    if (!package || !op) return NULL;
    if (!oop_initialized) return NULL;

    const char *visited[64];
    int visited_count = 0;
    return oop_overload_lookup_in(package, op, visited, &visited_count);
}

StradaValue* strada_overload_binary(StradaValue *left, StradaValue *right, const char *op) {
    /* Check left operand first */
    const char *lbp = SV_BLESSED(left);
    if (left && lbp) {
        StradaMethod func = strada_overload_lookup(lbp, op);
        if (func) {
            StradaValue *reversed = strada_new_int(0);
            StradaValue *args = strada_pack_args(2, right, reversed);
            strada_decref(reversed); /* balance: pack_args incref'd it */
            StradaValue *result = func(left, args);
            strada_decref(args);
            return result;
        }
    }
    /* Check right operand (reversed) */
    const char *rbp = SV_BLESSED(right);
    if (right && rbp) {
        StradaMethod func = strada_overload_lookup(rbp, op);
        if (func) {
            StradaValue *reversed = strada_new_int(1);
            StradaValue *args = strada_pack_args(2, left, reversed);
            strada_decref(reversed); /* balance: pack_args incref'd it */
            StradaValue *result = func(right, args);
            strada_decref(args);
            return result;
        }
    }
    return NULL; /* No overload found */
}

StradaValue* strada_overload_unary(StradaValue *operand, const char *op) {
    const char *obp = SV_BLESSED(operand);
    if (operand && obp) {
        StradaMethod func = strada_overload_lookup(obp, op);
        if (func) {
            StradaValue *args = strada_new_array();
            StradaValue *result = func(operand, args);
            strada_decref(args);
            return result;
        }
    }
    return NULL;
}

StradaValue* strada_overload_stringify(StradaValue *val) {
    const char *vbp = SV_BLESSED(val);
    if (val && vbp) {
        StradaMethod func = strada_overload_lookup(vbp, "\"\"");
        if (func) {
            StradaValue *args = strada_new_array();
            StradaValue *result = func(val, args);
            strada_decref(args);
            return result;
        }
    }
    return NULL;
}

static const char* oop_method_lookup_package_in(const char *package, const char *method,
                                                const char *visited[], int *visited_count) {
    /* Recursive depth-first search to find which package has this method */
    if (!package || !method || !package[0]) return NULL;

    /* Check if already visited (prevent infinite loops) */
    for (int i = 0; i < *visited_count; i++) {
        if (strcmp(visited[i], package) == 0) return NULL;
    }
    if (*visited_count < 64) {
        visited[(*visited_count)++] = package;
    }

    OopPackage *pkg = oop_find_package(package);
    if (!pkg) return NULL;

    /* Check this package's methods */
    for (int i = 0; i < pkg->method_count; i++) {
        if (strcmp(pkg->methods[i].name, method) == 0) {
            return package;
        }
    }

    /* Recursively check parent packages */
    for (int i = 0; i < pkg->parent_count; i++) {
        const char *result = oop_method_lookup_package_in(pkg->parents[i], method,
                                                          visited, visited_count);
        if (result) return result;
    }

    return NULL;
}

const char* strada_method_lookup_package(const char *package, const char *method) {
    /* Find which package (including parents) has this method */
    if (!package || !method) return NULL;
    if (!oop_initialized) return NULL;

    const char *visited[64];
    int visited_count = 0;
    return oop_method_lookup_package_in(package, method, visited, &visited_count);
}

static StradaMethod oop_lookup_method_in(const char *package, const char *method,
                                         const char *visited[], int *visited_count) {
    /* Recursive depth-first search through inheritance hierarchy */
    if (!package || !method || !package[0]) return NULL;

    /* Check if already visited (prevent infinite loops) */
    for (int i = 0; i < *visited_count; i++) {
        if (strcmp(visited[i], package) == 0) return NULL;
    }
    if (*visited_count < 64) {
        visited[(*visited_count)++] = package;
    }

    OopPackage *pkg = oop_find_package(package);
    if (!pkg) return NULL;

    /* Look for method in this package */
    for (int i = 0; i < pkg->method_count; i++) {
        if (strcmp(pkg->methods[i].name, method) == 0) {
            return pkg->methods[i].func;
        }
    }

    /* Search all parents (depth-first, left-to-right) */
    for (int i = 0; i < pkg->parent_count; i++) {
        StradaMethod func = oop_lookup_method_in(pkg->parents[i], method,
                                                  visited, visited_count);
        if (func) return func;
    }

    return NULL;
}

static StradaMethod oop_lookup_method(const char *package, const char *method) {
    /* Find method function pointer, following inheritance chain */
    if (!package || !method) return NULL;
    if (!oop_initialized) return NULL;

    /* Check method cache first */
    if (!method_cache_initialized) method_cache_init();
    unsigned int h = (oop_hash_name(package) ^ oop_hash_name(method)) & (METHOD_CACHE_SIZE - 1);
    MethodCacheEntry *ce = &method_cache[h];
    if (ce->func && ce->package == package && strcmp(ce->method, method) == 0) {
        return ce->func;
    }

    const char *visited[64];
    int visited_count = 0;
    StradaMethod func = oop_lookup_method_in(package, method, visited, &visited_count);

    /* Cache the result (even NULL misses are not cached to avoid stale entries) */
    if (func) {
        ce->package = package;
        strncpy(ce->method, method, OOP_MAX_NAME_LEN - 1);
        ce->method[OOP_MAX_NAME_LEN - 1] = '\0';
        ce->func = func;
    }

    return func;
}

StradaValue* strada_method_call(StradaValue *obj, const char *method, StradaValue *args) {
    /* Call a method on a blessed reference */
    if (!obj || !method) {
        fprintf(stderr, "Error: Cannot call method '%s' on undefined value\n",
                method ? method : "(null)");
        exit(1);
    }

    /* Allow string values as class names for static method calls (e.g., TIEHASH) */
    const char *pkg_name = NULL;
    const char *bp = SV_BLESSED(obj);
    if (bp) {
        pkg_name = bp;
    } else if (obj->type == STRADA_STR && obj->value.pv) {
        pkg_name = obj->value.pv;
    } else {
        fprintf(stderr, "Error: Cannot call method '%s' on unblessed reference\n", method);
        exit(1);
    }

    if (!oop_initialized) strada_oop_init();

    /* Handle UNIVERSAL methods: isa and can */
    if (strcmp(method, "isa") == 0) {
        /* $obj->isa("ClassName") - check if object is of a type */
        StradaValue *result;
        if (args && args->type == STRADA_ARRAY && args->value.av && args->value.av->size > 0) {
            StradaValue *classname = args->value.av->elements[0];
            if (classname && classname->type == STRADA_STR && classname->value.pv) {
                result = strada_new_int(strada_isa(obj, classname->value.pv) ? 1 : 0);
            } else {
                result = strada_new_int(0);
            }
        } else {
            result = strada_new_int(0);
        }
        if (args) strada_decref(args);
        return result;
    }

    if (strcmp(method, "can") == 0) {
        /* $obj->can("method_name") - check if object can do a method */
        StradaValue *result;
        if (args && args->type == STRADA_ARRAY && args->value.av && args->value.av->size > 0) {
            StradaValue *methname = args->value.av->elements[0];
            if (methname && methname->type == STRADA_STR && methname->value.pv) {
                result = strada_new_int(strada_can(obj, methname->value.pv) ? 1 : 0);
            } else {
                result = strada_new_int(0);
            }
        } else {
            result = strada_new_int(0);
        }
        if (args) strada_decref(args);
        return result;
    }

    StradaMethod func = oop_lookup_method(pkg_name, method);
    if (!func) {
        /* Try AUTOLOAD fallback */
        StradaMethod autoload = oop_lookup_method(pkg_name, "AUTOLOAD");
        if (autoload) {
            /* Build new args: prepend method name to original args */
            StradaValue *new_args = strada_new_array();
            strada_array_push_take(new_args->value.av, strada_new_str(method));
            if (args && args->type == STRADA_ARRAY && args->value.av) {
                for (size_t i = 0; i < args->value.av->size; i++) {
                    strada_array_push(new_args->value.av, args->value.av->elements[args->value.av->head + i]);
                }
            }
            StradaValue *result = autoload(obj, new_args);
            strada_decref(new_args);
            if (args) strada_decref(args);
            return result;
        }
        fprintf(stderr, "Error: Can't locate method '%s' in package '%s' or its parents\n",
                method, pkg_name);
        exit(1);
    }

    /* Check for method modifiers (before/after/around) — skip entirely when none registered */
    OopPackage *pkg = oop_any_modifiers ? oop_find_package(pkg_name) : NULL;
    if (pkg && pkg->modifier_count > 0) {
        /* Run 'before' modifiers */
        for (int mi = 0; mi < pkg->modifier_count; mi++) {
            if (pkg->modifiers[mi].type == 1 && strcmp(pkg->modifiers[mi].method_name, method) == 0) {
                StradaValue *before_args = strada_new_array();
                StradaValue *before_ret = pkg->modifiers[mi].func(obj, before_args);
                if (before_ret) strada_decref(before_ret);
                strada_decref(before_args);
            }
        }

        /* Check for 'around' modifier — only use the first one found */
        StradaMethod around_func = NULL;
        for (int mi = 0; mi < pkg->modifier_count; mi++) {
            if (pkg->modifiers[mi].type == 3 && strcmp(pkg->modifiers[mi].method_name, method) == 0) {
                around_func = pkg->modifiers[mi].func;
                break;
            }
        }

        StradaValue *result;
        if (around_func) {
            /* Build around args: [original_func_ref, ...original_args] */
            /* For now, around receives the same args; the original method is stored as a closure */
            /* We pass the original method by wrapping it as a function reference in the args */
            StradaValue *around_args = strada_new_array();
            /* First arg: a function pointer to the original (stored as int) */
            strada_array_push_take(around_args->value.av, strada_new_int((int64_t)(intptr_t)func));
            /* Copy remaining args */
            if (args && args->type == STRADA_ARRAY && args->value.av) {
                for (size_t ai = 0; ai < args->value.av->size; ai++) {
                    strada_array_push(around_args->value.av, args->value.av->elements[args->value.av->head + ai]);
                }
            }
            result = around_func(obj, around_args);
            strada_decref(around_args);
        } else {
            result = func(obj, args);
        }

        /* Run 'after' modifiers */
        for (int mi = 0; mi < pkg->modifier_count; mi++) {
            if (pkg->modifiers[mi].type == 2 && strcmp(pkg->modifiers[mi].method_name, method) == 0) {
                StradaValue *after_args = strada_new_array();
                StradaValue *after_ret = pkg->modifiers[mi].func(obj, after_args);
                if (after_ret) strada_decref(after_ret);
                strada_decref(after_args);
            }
        }

        if (args) strada_decref(args);
        return result;
    }

    StradaValue *result = func(obj, args);
    /* Free the args array (created by strada_pack_args) after the call */
    if (args) {
        strada_decref(args);
    }
    return result;
}

/* Helper to get first parent package (for SUPER:: calls) */
const char* strada_get_parent_package(const char *package) {
    if (!package || !oop_initialized) return NULL;

    OopPackage *pkg = oop_find_package(package);
    if (!pkg || pkg->parent_count == 0) return NULL;

    return pkg->parents[0];
}

/* Recursive helper for isa check */
static int oop_isa_check(const char *current_pkg, const char *target,
                         const char *visited[], int *visited_count) {
    if (!current_pkg || !current_pkg[0]) return 0;

    /* Check if this is the target */
    if (strcmp(current_pkg, target) == 0) return 1;

    /* Check if already visited */
    for (int i = 0; i < *visited_count; i++) {
        if (strcmp(visited[i], current_pkg) == 0) return 0;
    }
    if (*visited_count < 64) {
        visited[(*visited_count)++] = current_pkg;
    }

    OopPackage *pkg = oop_find_package(current_pkg);
    if (!pkg) return 0;

    /* Check all parents */
    for (int i = 0; i < pkg->parent_count; i++) {
        if (oop_isa_check(pkg->parents[i], target, visited, visited_count)) {
            return 1;
        }
    }

    return 0;
}

/* Check if a value is blessed into a specific package (or inherits from it) */
int strada_isa(StradaValue *obj, const char *package) {
    const char *bp = SV_BLESSED(obj);
    if (!obj || !package || !bp) return 0;
    if (!oop_initialized) return 0;

    /* Check isa cache — bp is interned (pointer compare), target uses strcmp */
    unsigned int h = (oop_hash_name(bp) ^ oop_hash_name(package)) & (ISA_CACHE_SIZE - 1);
    IsaCacheEntry *ice = &isa_cache[h];
    if (ice->valid && ice->pkg == bp && strcmp(ice->target, package) == 0) {
        return ice->result;
    }

    const char *visited[64];
    int visited_count = 0;
    int result = oop_isa_check(bp, package, visited, &visited_count);

    /* Cache the result */
    ice->pkg = bp;
    strncpy(ice->target, package, OOP_MAX_NAME_LEN - 1);
    ice->target[OOP_MAX_NAME_LEN - 1] = '\0';
    ice->result = result;
    ice->valid = 1;

    return result;
}

/* Check if object can do a method */
int strada_can(StradaValue *obj, const char *method) {
    const char *bp = SV_BLESSED(obj);
    if (!obj || !method || !bp) return 0;
    if (!oop_initialized) return 0;

    return oop_lookup_method(bp, method) != NULL;
}

/* SUPER:: method call - calls method from parent class(es) */
StradaValue* strada_super_call(StradaValue *obj, const char *from_package,
                               const char *method, StradaValue *args) {
    if (!obj || !method || !from_package) {
        fprintf(stderr, "Error: Invalid SUPER:: call\n");
        exit(1);
    }

    if (!SV_BLESSED(obj)) {
        fprintf(stderr, "Error: Cannot call SUPER::%s on unblessed reference\n", method);
        exit(1);
    }

    if (!oop_initialized) strada_oop_init();

    /* Find the package we're calling from */
    OopPackage *pkg = oop_find_package(from_package);
    if (!pkg) {
        fprintf(stderr, "Error: Package '%s' not found for SUPER:: call\n", from_package);
        exit(1);
    }

    /* Search for method in all parents */
    StradaMethod func = NULL;
    for (int i = 0; i < pkg->parent_count; i++) {
        func = oop_lookup_method(pkg->parents[i], method);
        if (func) break;
    }

    if (!func) {
        fprintf(stderr, "Error: Can't locate SUPER::%s via package '%s'\n",
                method, from_package);
        exit(1);
    }

    /* Save and set method package context */
    char saved_pkg[OOP_MAX_NAME_LEN];
    snprintf(saved_pkg, OOP_MAX_NAME_LEN, "%s", oop_current_method_package);

    StradaValue *result = func(obj, args);
    /* Free the args array (created by strada_pack_args) after the call */
    if (args) {
        strada_decref(args);
    }
    return result;
}

/* Set current method package (for SUPER:: context) */
void strada_set_method_package(const char *package) {
    if (package) {
        strncpy(oop_current_method_package, package, OOP_MAX_NAME_LEN - 1);
        oop_current_method_package[OOP_MAX_NAME_LEN - 1] = '\0';
    } else {
        oop_current_method_package[0] = '\0';
    }
}

const char* strada_get_method_package(void) {
    if (oop_current_method_package[0] == '\0') return NULL;
    return oop_current_method_package;
}

/* Call DESTROY on an object if it has one */
void strada_call_destroy(StradaValue *obj) {
    const char *bp = SV_BLESSED(obj);
    if (!obj || !bp || oop_destroying) return;
    if (!oop_initialized) return;

    strada_check_debug_bless();
    if (strada_debug_bless) {
        fprintf(stderr, "[FREE] obj=%p blessed_package ptr=%p\n",
                (void*)obj, (void*)bp);
    }

    /* Validate blessed_package before using it */
    if (!strada_validate_blessed_package(bp)) {
        fprintf(stderr, "Error: strada_call_destroy skipping due to corrupted blessed_package (obj=%p)\n", (void*)obj);
        return;
    }

    if (strada_debug_bless) {
        fprintf(stderr, "[FREE] obj=%p package='%s'\n",
                (void*)obj, bp);
    }

    StradaMethod destroy = oop_lookup_method(bp, "DESTROY");
    if (destroy) {
        oop_destroying = 1;  /* Prevent recursive DESTROY */
        destroy(obj, NULL);
        oop_destroying = 0;
    }
}

/* ===== DIRECTORY FUNCTIONS ===== */

/* Read all entries from a directory, returns array of filenames */
StradaValue* strada_readdir(StradaValue *path_val) {
    char _tb[PATH_MAX];
    const char *path = strada_to_str_buf(path_val, _tb, sizeof(_tb));
    DIR *dir = opendir(path);
    if (!dir) {
        return strada_new_undef();
    }

    StradaValue *result = strada_new_array();
    struct dirent *entry;

    while ((entry = readdir(dir)) != NULL) {
        /* Skip . and .. */
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        strada_array_push_take(result->value.av, strada_new_str(entry->d_name));
    }

    closedir(dir);
    return result;
}

/* Read directory with full paths */
StradaValue* strada_readdir_full(StradaValue *path_val) {
    char _tb[PATH_MAX];
    const char *path = strada_to_str_buf(path_val, _tb, sizeof(_tb));
    DIR *dir = opendir(path);
    if (!dir) {
        return strada_new_undef();
    }

    StradaValue *result = strada_new_array();
    struct dirent *entry;
    char fullpath[PATH_MAX];

    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        snprintf(fullpath, PATH_MAX, "%s/%s", path, entry->d_name);
        strada_array_push_take(result->value.av, strada_new_str(fullpath));
    }

    closedir(dir);
    return result;
}

/* Check if path is a directory */
StradaValue* strada_is_dir(StradaValue *path_val) {
    char _tb[PATH_MAX];
    const char *path = strada_to_str_buf(path_val, _tb, sizeof(_tb));
    struct stat st;
    int is_dir = (stat(path, &st) == 0 && S_ISDIR(st.st_mode));
    return strada_new_int(is_dir ? 1 : 0);
}

/* Check if path is a regular file */
StradaValue* strada_is_file(StradaValue *path_val) {
    char _tb[PATH_MAX];
    const char *path = strada_to_str_buf(path_val, _tb, sizeof(_tb));
    struct stat st;
    int is_file = (stat(path, &st) == 0 && S_ISREG(st.st_mode));
    return strada_new_int(is_file ? 1 : 0);
}

/* Get file size in bytes */
StradaValue* strada_file_size(StradaValue *path_val) {
    char _tb[PATH_MAX];
    const char *path = strada_to_str_buf(path_val, _tb, sizeof(_tb));
    struct stat st;
    long size = 0;
    if (stat(path, &st) == 0) {
        size = (long)st.st_size;
    }
    return strada_new_int(size);
}

/* ===== MATH FUNCTIONS ===== */

StradaValue* strada_sin(StradaValue *x) {
    return strada_new_num(sin(strada_to_num(x)));
}

StradaValue* strada_cos(StradaValue *x) {
    return strada_new_num(cos(strada_to_num(x)));
}

StradaValue* strada_tan(StradaValue *x) {
    return strada_new_num(tan(strada_to_num(x)));
}

StradaValue* strada_asin(StradaValue *x) {
    return strada_new_num(asin(strada_to_num(x)));
}

StradaValue* strada_acos(StradaValue *x) {
    return strada_new_num(acos(strada_to_num(x)));
}

StradaValue* strada_atan(StradaValue *x) {
    return strada_new_num(atan(strada_to_num(x)));
}

StradaValue* strada_atan2(StradaValue *y, StradaValue *x) {
    return strada_new_num(atan2(strada_to_num(y), strada_to_num(x)));
}

StradaValue* strada_log(StradaValue *x) {
    return strada_new_num(log(strada_to_num(x)));
}

StradaValue* strada_log10(StradaValue *x) {
    return strada_new_num(log10(strada_to_num(x)));
}

StradaValue* strada_exp(StradaValue *x) {
    return strada_new_num(exp(strada_to_num(x)));
}

StradaValue* strada_pow(StradaValue *base, StradaValue *exponent) {
    return strada_new_num(pow(strada_to_num(base), strada_to_num(exponent)));
}

StradaValue* strada_floor(StradaValue *x) {
    return strada_new_num(floor(strada_to_num(x)));
}

StradaValue* strada_ceil(StradaValue *x) {
    return strada_new_num(ceil(strada_to_num(x)));
}

StradaValue* strada_round(StradaValue *x) {
    return strada_new_num(round(strada_to_num(x)));
}

StradaValue* strada_fabs(StradaValue *x) {
    return strada_new_num(fabs(strada_to_num(x)));
}

StradaValue* strada_fmod(StradaValue *x, StradaValue *y) {
    return strada_new_num(fmod(strada_to_num(x), strada_to_num(y)));
}

StradaValue* strada_sinh(StradaValue *x) {
    return strada_new_num(sinh(strada_to_num(x)));
}

StradaValue* strada_cosh(StradaValue *x) {
    return strada_new_num(cosh(strada_to_num(x)));
}

StradaValue* strada_tanh(StradaValue *x) {
    return strada_new_num(tanh(strada_to_num(x)));
}

/* ===== FILE SEEK FUNCTIONS ===== */

StradaValue* strada_seek(StradaValue *fh, StradaValue *offset, StradaValue *whence) {
    if (fh->type != STRADA_FILEHANDLE || !fh->value.fh) {
        return strada_new_int(-1);
    }
    int w = strada_to_int(whence);
    int seek_whence = SEEK_SET;
    if (w == 1) seek_whence = SEEK_CUR;
    else if (w == 2) seek_whence = SEEK_END;

    int result = fseek(fh->value.fh, strada_to_int(offset), seek_whence);
    return strada_new_int(result == 0 ? 1 : 0);
}

StradaValue* strada_tell(StradaValue *fh) {
    if (fh->type != STRADA_FILEHANDLE || !fh->value.fh) {
        return strada_new_int(-1);
    }
    return strada_new_int(ftell(fh->value.fh));
}

StradaValue* strada_rewind(StradaValue *fh) {
    if (fh->type != STRADA_FILEHANDLE || !fh->value.fh) {
        return strada_new_int(0);
    }
    rewind(fh->value.fh);
    return strada_new_int(1);
}

StradaValue* strada_eof(StradaValue *fh) {
    if (fh->type != STRADA_FILEHANDLE || !fh->value.fh) {
        return strada_new_int(1);
    }
    return strada_new_int(feof(fh->value.fh) ? 1 : 0);
}

StradaValue* strada_flush(StradaValue *fh) {
    if (fh->type != STRADA_FILEHANDLE || !fh->value.fh) {
        return strada_new_int(0);
    }
    return strada_new_int(fflush(fh->value.fh) == 0 ? 1 : 0);
}

/* ===== DNS/HOSTNAME FUNCTIONS ===== */

StradaValue* strada_gethostbyname(StradaValue *hostname_val) {
    char _tb[256];
    const char *hostname = strada_to_str_buf(hostname_val, _tb, sizeof(_tb));
    struct hostent *he = gethostbyname(hostname);
    if (!he) {
        return strada_new_undef();
    }

    /* Return the first IPv4 address as a string */
    if (he->h_addr_list[0]) {
        char *addr = inet_ntoa(*(struct in_addr*)he->h_addr_list[0]);
        return strada_new_str(addr);
    }
    return strada_new_undef();
}

StradaValue* strada_gethostbyname_all(StradaValue *hostname_val) {
    char _tb[256];
    const char *hostname = strada_to_str_buf(hostname_val, _tb, sizeof(_tb));
    struct hostent *he = gethostbyname(hostname);
    if (!he) {
        return strada_new_undef();
    }

    /* Return all IPv4 addresses as an array */
    StradaValue *result = strada_new_array();
    for (int i = 0; he->h_addr_list[i]; i++) {
        char *addr = inet_ntoa(*(struct in_addr*)he->h_addr_list[i]);
        strada_array_push_take(result->value.av, strada_new_str(addr));
    }
    return result;
}

StradaValue* strada_gethostname(void) {
    char hostname[256];
    if (gethostname(hostname, sizeof(hostname)) == 0) {
        return strada_new_str(hostname);
    }
    return strada_new_undef();
}

StradaValue* strada_getaddrinfo_first(StradaValue *hostname_val, StradaValue *service_val) {
    char _tb[256];
    const char *hostname = strada_to_str_buf(hostname_val, _tb, sizeof(_tb));
    char _tb2[256];
    const char *service = strada_to_str_buf(service_val, _tb2, sizeof(_tb2));

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;

    int status = getaddrinfo(hostname, service[0] ? service : NULL, &hints, &res);
    if (status != 0) {
        return strada_new_undef();
    }

    char ipstr[INET_ADDRSTRLEN];
    struct sockaddr_in *ipv4 = (struct sockaddr_in *)res->ai_addr;
    inet_ntop(AF_INET, &(ipv4->sin_addr), ipstr, sizeof(ipstr));

    freeaddrinfo(res);
    return strada_new_str(ipstr);
}

/* ===== PATH FUNCTIONS ===== */

StradaValue* strada_realpath(StradaValue *path_val) {
    char _tb[PATH_MAX];
    const char *path = strada_to_str_buf(path_val, _tb, sizeof(_tb));
    char resolved[PATH_MAX];

    char *result = realpath(path, resolved);
    if (result) {
        return strada_new_str(resolved);
    }
    return strada_new_undef();
}

StradaValue* strada_dirname(StradaValue *path_val) {
    char *path = strada_to_str(path_val);
    char *dir = dirname(path);
    StradaValue *result = strada_new_str(dir);
    free(path);
    return result;
}

StradaValue* strada_basename(StradaValue *path_val) {
    char *path = strada_to_str(path_val);
    char *base = basename(path);
    StradaValue *result = strada_new_str(base);
    free(path);
    return result;
}

StradaValue* strada_glob(StradaValue *pattern_val) {
    char _tb[PATH_MAX];
    const char *pattern = strada_to_str_buf(pattern_val, _tb, sizeof(_tb));
    glob_t globbuf;

    int flags = GLOB_TILDE | GLOB_BRACE;
    int result = glob(pattern, flags, NULL, &globbuf);

    if (result != 0) {
        globfree(&globbuf);
        return strada_new_array();  /* Return empty array on no match or error */
    }

    StradaValue *arr = strada_new_array();
    for (size_t i = 0; i < globbuf.gl_pathc; i++) {
        strada_array_push_take(arr->value.av, strada_new_str(globbuf.gl_pathv[i]));
    }

    globfree(&globbuf);
    return arr;
}

StradaValue* strada_fnmatch(StradaValue *pattern_val, StradaValue *string_val) {
    char _tb[PATH_MAX];
    const char *pattern = strada_to_str_buf(pattern_val, _tb, sizeof(_tb));
    char _tb2[PATH_MAX];
    const char *string = strada_to_str_buf(string_val, _tb2, sizeof(_tb2));

    int result = fnmatch(pattern, string, FNM_PATHNAME);
    return strada_new_int(result == 0 ? 1 : 0);
}

/* File extension helper */
StradaValue* strada_file_ext(StradaValue *path_val) {
    char _tb[PATH_MAX];
    const char *path = strada_to_str_buf(path_val, _tb, sizeof(_tb));
    const char *dot = strrchr(path, '.');
    if (!dot || dot == path) {
        return strada_new_str("");
    }
    return strada_new_str(dot + 1);
}

/* Join path components */
StradaValue* strada_path_join(StradaValue *parts_val) {
    /* Handle both arrays and references to arrays */
    StradaValue *arr = parts_val;
    if (parts_val->type == STRADA_REF) {
        arr = parts_val->value.rv;
    }
    if (arr->type != STRADA_ARRAY) {
        return strada_new_undef();
    }

    StradaArray *av = arr->value.av;
    size_t len = av->size;
    if (len == 0) {
        return strada_new_str("");
    }

    /* Pre-convert all strings and calculate total length */
    char **strs = malloc(len * sizeof(char*));
    size_t total = 0;
    for (size_t i = 0; i < len; i++) {
        StradaValue *part = strada_array_get(av, i);
        strs[i] = strada_to_str(part);
        total += strlen(strs[i]) + 1;  /* +1 for '/' */
    }

    char *result = malloc(total + 1);
    result[0] = '\0';

    for (size_t i = 0; i < len; i++) {
        if (i > 0 && result[strlen(result)-1] != '/') {
            strcat(result, "/");
        }
        strcat(result, strs[i]);
        free(strs[i]);
    }
    free(strs);

    StradaValue *ret = strada_new_str(result);
    free(result);
    return ret;
}

/* ============================================================
 * NEW LIBC FUNCTION IMPLEMENTATIONS
 * ============================================================ */

/* ===== ADDITIONAL FILE I/O ===== */

StradaValue* strada_fgetc(StradaValue *fh) {
    if (!fh || fh->type != STRADA_FILEHANDLE || !fh->value.fh) {
        return strada_new_int(-1);
    }
    int c = fgetc(fh->value.fh);
    return strada_new_int(c);
}

StradaValue* strada_fputc(StradaValue *ch, StradaValue *fh) {
    if (!fh || fh->type != STRADA_FILEHANDLE || !fh->value.fh) {
        return strada_new_int(-1);
    }
    int c = (int)strada_to_int(ch);
    int result = fputc(c, fh->value.fh);
    return strada_new_int(result);
}

StradaValue* strada_fgets(StradaValue *fh, StradaValue *size) {
    if (!fh || fh->type != STRADA_FILEHANDLE || !fh->value.fh) {
        return strada_new_undef();
    }
    int sz = (int)strada_to_int(size);
    if (sz <= 0) sz = 1024;

    char *buf = malloc(sz);
    if (!buf) return strada_new_undef();

    char *result = fgets(buf, sz, fh->value.fh);
    if (!result) {
        free(buf);
        return strada_new_undef();
    }

    StradaValue *sv = strada_new_str(buf);
    free(buf);
    return sv;
}

StradaValue* strada_fputs(StradaValue *str, StradaValue *fh) {
    if (!fh || fh->type != STRADA_FILEHANDLE || !fh->value.fh) {
        return strada_new_int(-1);
    }
    char _tb[256];
    const char *s = strada_to_str_buf(str, _tb, sizeof(_tb));
    int result = fputs(s, fh->value.fh);
    return strada_new_int(result >= 0 ? 1 : -1);
}

StradaValue* strada_ferror(StradaValue *fh) {
    if (!fh || fh->type != STRADA_FILEHANDLE || !fh->value.fh) {
        return strada_new_int(1);
    }
    return strada_new_int(ferror(fh->value.fh) ? 1 : 0);
}

StradaValue* strada_fileno(StradaValue *fh) {
    if (!fh || fh->type != STRADA_FILEHANDLE || !fh->value.fh) {
        return strada_new_int(-1);
    }
    return strada_new_int(fileno(fh->value.fh));
}

StradaValue* strada_clearerr(StradaValue *fh) {
    if (!fh || fh->type != STRADA_FILEHANDLE || !fh->value.fh) {
        return strada_new_undef();
    }
    clearerr(fh->value.fh);
    return strada_new_int(1);
}

/* ===== TEMPORARY FILES ===== */

StradaValue* strada_tmpfile(void) {
    FILE *fp = tmpfile();
    if (!fp) return strada_new_undef();
    return strada_new_filehandle(fp);
}

StradaValue* strada_mkstemp(StradaValue *template) {
    char *tmp = strada_to_str(template);
    char *tmpl = strdup(tmp);
    free(tmp);
    if (!tmpl) return strada_new_undef();

    int fd = mkstemp(tmpl);
    if (fd < 0) {
        free(tmpl);
        return strada_new_undef();
    }

    /* Return array: [fd, path] */
    StradaValue *result = strada_new_array();
    strada_array_push_take(result->value.av, strada_new_int(fd));
    strada_array_push_take(result->value.av, strada_new_str(tmpl));
    free(tmpl);
    return result;
}

StradaValue* strada_mkdtemp(StradaValue *template) {
    char *tmp = strada_to_str(template);
    char *tmpl = strdup(tmp);
    free(tmp);
    if (!tmpl) return strada_new_undef();

    char *result = mkdtemp(tmpl);
    if (!result) {
        free(tmpl);
        return strada_new_undef();
    }

    StradaValue *sv = strada_new_str(result);
    free(tmpl);
    return sv;
}

/* ===== COMMAND EXECUTION (popen) ===== */

StradaValue* strada_popen(StradaValue *cmd, StradaValue *mode) {
    char _tb[256];
    const char *c = strada_to_str_buf(cmd, _tb, sizeof(_tb));
    char _tb2[256];
    const char *m = strada_to_str_buf(mode, _tb2, sizeof(_tb2));

    FILE *fp = popen(c, m);
    if (!fp) return strada_new_undef();

    fh_meta_add(fp, FH_PIPE);
    return strada_new_filehandle(fp);
}

StradaValue* strada_pclose(StradaValue *fh) {
    if (!fh || fh->type != STRADA_FILEHANDLE || !fh->value.fh) {
        return strada_new_int(-1);
    }
    FILE *fp = fh->value.fh;
    fh_meta_remove(fp);  /* Remove meta before pclose to prevent double-close in free_value */
    int status = pclose(fp);
    fh->value.fh = NULL;  /* Mark as closed */
    return strada_new_int(WEXITSTATUS(status));
}

/* Run command and capture stdout (like Perl's qx// or backticks) */
StradaValue* strada_qx(StradaValue *cmd) {
    char _tb[256];
    const char *c = strada_to_str_buf(cmd, _tb, sizeof(_tb));
    FILE *fp = popen(c, "r");
    if (!fp) {
        return strada_new_str("");
    }

    /* Read all output into a buffer */
    size_t capacity = 4096;
    size_t len = 0;
    char *buf = malloc(capacity);
    if (!buf) {
        pclose(fp);
        return strada_new_str("");
    }

    char temp[1024];
    while (fgets(temp, sizeof(temp), fp)) {
        size_t chunk_len = strlen(temp);
        if (len + chunk_len + 1 > capacity) {
            capacity = (len + chunk_len + 1) * 2;
            char *newbuf = realloc(buf, capacity);
            if (!newbuf) {
                free(buf);
                pclose(fp);
                return strada_new_str("");
            }
            buf = newbuf;
        }
        memcpy(buf + len, temp, chunk_len);
        len += chunk_len;
    }
    buf[len] = '\0';

    pclose(fp);

    StradaValue *result = strada_new_str(buf);
    free(buf);
    return result;
}

/* Aliases for bootstrap compiler compatibility (sys::foo -> sys_foo) */
StradaValue* sys_system(StradaValue *cmd) { return strada_system(cmd); }
StradaValue* sys_qx(StradaValue *cmd) { return strada_qx(cmd); }
StradaValue* sys_unlink(StradaValue *path) { return strada_unlink(path); }

/* ===== ADDITIONAL FILE SYSTEM ===== */

StradaValue* strada_truncate(StradaValue *path, StradaValue *length) {
    char _tb[PATH_MAX];
    const char *p = strada_to_str_buf(path, _tb, sizeof(_tb));
    off_t len = (off_t)strada_to_int(length);
    int result = truncate(p, len);
    return strada_new_int(result);
}

StradaValue* strada_ftruncate(StradaValue *fd, StradaValue *length) {
    int f = (int)strada_to_int(fd);
    off_t len = (off_t)strada_to_int(length);
    return strada_new_int(ftruncate(f, len));
}

StradaValue* strada_chown(StradaValue *path, StradaValue *uid, StradaValue *gid) {
    char _tb[PATH_MAX];
    const char *p = strada_to_str_buf(path, _tb, sizeof(_tb));
    uid_t u = (uid_t)strada_to_int(uid);
    gid_t g = (gid_t)strada_to_int(gid);
    int result = chown(p, u, g);
    return strada_new_int(result);
}

StradaValue* strada_lchown(StradaValue *path, StradaValue *uid, StradaValue *gid) {
    char _tb[PATH_MAX];
    const char *p = strada_to_str_buf(path, _tb, sizeof(_tb));
    uid_t u = (uid_t)strada_to_int(uid);
    gid_t g = (gid_t)strada_to_int(gid);
    int result = lchown(p, u, g);
    return strada_new_int(result);
}

StradaValue* strada_fchmod(StradaValue *fd, StradaValue *mode) {
    int f = (int)strada_to_int(fd);
    mode_t m = (mode_t)strada_to_int(mode);
    return strada_new_int(fchmod(f, m));
}

StradaValue* strada_fchown(StradaValue *fd, StradaValue *uid, StradaValue *gid) {
    int f = (int)strada_to_int(fd);
    uid_t u = (uid_t)strada_to_int(uid);
    gid_t g = (gid_t)strada_to_int(gid);
    return strada_new_int(fchown(f, u, g));
}

StradaValue* strada_utime(StradaValue *path, StradaValue *atime, StradaValue *mtime) {
    char _tb[PATH_MAX];
    const char *p = strada_to_str_buf(path, _tb, sizeof(_tb));
    struct utimbuf times;
    times.actime = (time_t)strada_to_int(atime);
    times.modtime = (time_t)strada_to_int(mtime);
    int result = utime(p, &times);
    return strada_new_int(result);
}

StradaValue* strada_utimes(StradaValue *path, StradaValue *atime, StradaValue *mtime) {
    char _tb[PATH_MAX];
    const char *p = strada_to_str_buf(path, _tb, sizeof(_tb));
    struct timeval times[2];
    times[0].tv_sec = (time_t)strada_to_int(atime);
    times[0].tv_usec = 0;
    times[1].tv_sec = (time_t)strada_to_int(mtime);
    times[1].tv_usec = 0;
    int result = utimes(p, times);
    return strada_new_int(result);
}

/* ===== SESSION/PROCESS GROUP CONTROL ===== */

StradaValue* strada_setsid(void) {
    pid_t result = setsid();
    return strada_new_int((int64_t)result);
}

StradaValue* strada_getsid(StradaValue *pid) {
    pid_t p = (pid_t)strada_to_int(pid);
    pid_t result = getsid(p);
    return strada_new_int((int64_t)result);
}

StradaValue* strada_setpgid(StradaValue *pid, StradaValue *pgid) {
    pid_t p = (pid_t)strada_to_int(pid);
    pid_t pg = (pid_t)strada_to_int(pgid);
    return strada_new_int(setpgid(p, pg));
}

StradaValue* strada_getpgid(StradaValue *pid) {
    pid_t p = (pid_t)strada_to_int(pid);
    pid_t result = getpgid(p);
    return strada_new_int((int64_t)result);
}

StradaValue* strada_getpgrp(void) {
    return strada_new_int((int64_t)getpgrp());
}

StradaValue* strada_setpgrp(void) {
    return strada_new_int(setpgrp());
}

/* ===== USER/GROUP ID CONTROL ===== */

StradaValue* strada_setuid(StradaValue *uid) {
    uid_t u = (uid_t)strada_to_int(uid);
    return strada_new_int(setuid(u));
}

StradaValue* strada_setgid(StradaValue *gid) {
    gid_t g = (gid_t)strada_to_int(gid);
    return strada_new_int(setgid(g));
}

StradaValue* strada_seteuid(StradaValue *uid) {
    uid_t u = (uid_t)strada_to_int(uid);
    return strada_new_int(seteuid(u));
}

StradaValue* strada_setegid(StradaValue *gid) {
    gid_t g = (gid_t)strada_to_int(gid);
    return strada_new_int(setegid(g));
}

StradaValue* strada_setreuid(StradaValue *ruid, StradaValue *euid) {
    uid_t r = (uid_t)strada_to_int(ruid);
    uid_t e = (uid_t)strada_to_int(euid);
    return strada_new_int(setreuid(r, e));
}

StradaValue* strada_setregid(StradaValue *rgid, StradaValue *egid) {
    gid_t r = (gid_t)strada_to_int(rgid);
    gid_t e = (gid_t)strada_to_int(egid);
    return strada_new_int(setregid(r, e));
}

/* ===== ADDITIONAL SOCKET OPERATIONS ===== */

StradaValue* strada_setsockopt(StradaValue *sock, StradaValue *level, StradaValue *optname, StradaValue *optval) {
    int s = strada_to_int(sock);
    int lv = strada_to_int(level);
    int opt = strada_to_int(optname);
    int val = strada_to_int(optval);
    return strada_new_int(setsockopt(s, lv, opt, &val, sizeof(val)));
}

StradaValue* strada_getsockopt(StradaValue *sock, StradaValue *level, StradaValue *optname) {
    int s = strada_to_int(sock);
    int lv = strada_to_int(level);
    int opt = strada_to_int(optname);
    int val = 0;
    socklen_t len = sizeof(val);
    if (getsockopt(s, lv, opt, &val, &len) < 0) {
        return strada_new_undef();
    }
    return strada_new_int(val);
}

StradaValue* strada_shutdown(StradaValue *sock, StradaValue *how) {
    int s = strada_to_int(sock);
    int h = strada_to_int(how);
    return strada_new_int(shutdown(s, h));
}

StradaValue* strada_getpeername(StradaValue *sock) {
    int s = strada_to_int(sock);
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);

    if (getpeername(s, (struct sockaddr*)&addr, &len) < 0) {
        return strada_new_undef();
    }

    StradaValue *result = strada_new_hash();
    strada_hash_set_take(result->value.hv, "addr", strada_new_str(inet_ntoa(addr.sin_addr)));
    strada_hash_set_take(result->value.hv, "port", strada_new_int(ntohs(addr.sin_port)));
    return result;
}

StradaValue* strada_getsockname(StradaValue *sock) {
    int s = strada_to_int(sock);
    struct sockaddr_in addr;
    socklen_t len = sizeof(addr);

    if (getsockname(s, (struct sockaddr*)&addr, &len) < 0) {
        return strada_new_undef();
    }

    StradaValue *result = strada_new_hash();
    strada_hash_set_take(result->value.hv, "addr", strada_new_str(inet_ntoa(addr.sin_addr)));
    strada_hash_set_take(result->value.hv, "port", strada_new_int(ntohs(addr.sin_port)));
    return result;
}

StradaValue* strada_inet_pton(StradaValue *af, StradaValue *src) {
    int family = strada_to_int(af);
    char _tb[256];
    const char *s = strada_to_str_buf(src, _tb, sizeof(_tb));

    if (family == AF_INET) {
        struct in_addr addr;
        if (inet_pton(AF_INET, s, &addr) == 1) {
            return strada_new_int((int64_t)addr.s_addr);
        }
    } else if (family == AF_INET6) {
        struct in6_addr addr;
        if (inet_pton(AF_INET6, s, &addr) == 1) {
            /* Return the first 64 bits as int */
            uint64_t val;
            memcpy(&val, &addr, sizeof(val));
            return strada_new_int((int64_t)val);
        }
    }
    return strada_new_int(-1);
}

StradaValue* strada_inet_ntop(StradaValue *af, StradaValue *src) {
    int family = strada_to_int(af);
    int64_t addr_val = strada_to_int(src);

    char buf[INET6_ADDRSTRLEN];
    if (family == AF_INET) {
        struct in_addr addr;
        addr.s_addr = (in_addr_t)addr_val;
        if (inet_ntop(AF_INET, &addr, buf, sizeof(buf))) {
            return strada_new_str(buf);
        }
    }
    return strada_new_undef();
}

StradaValue* strada_inet_addr(StradaValue *cp) {
    char _tb[256];
    const char *s = strada_to_str_buf(cp, _tb, sizeof(_tb));
    in_addr_t result = inet_addr(s);
    return strada_new_int((int64_t)result);
}

StradaValue* strada_inet_ntoa(StradaValue *in) {
    struct in_addr addr;
    addr.s_addr = (in_addr_t)strada_to_int(in);
    return strada_new_str(inet_ntoa(addr));
}

StradaValue* strada_htons(StradaValue *hostshort) {
    uint16_t hs = (uint16_t)strada_to_int(hostshort);
    return strada_new_int(htons(hs));
}

StradaValue* strada_htonl(StradaValue *hostlong) {
    uint32_t hl = (uint32_t)strada_to_int(hostlong);
    return strada_new_int(htonl(hl));
}

StradaValue* strada_ntohs(StradaValue *netshort) {
    uint16_t ns = (uint16_t)strada_to_int(netshort);
    return strada_new_int(ntohs(ns));
}

StradaValue* strada_ntohl(StradaValue *netlong) {
    uint32_t nl = (uint32_t)strada_to_int(netlong);
    return strada_new_int(ntohl(nl));
}

StradaValue* strada_poll(StradaValue *fds, StradaValue *timeout) {
    /* fds is an array of hashes with {fd, events} */
    StradaValue *arr = fds;
    if (arr->type == STRADA_REF) arr = arr->value.rv;
    if (arr->type != STRADA_ARRAY) return strada_new_int(-1);

    int nfds = strada_array_length(arr->value.av);
    struct pollfd *pfds = calloc(nfds, sizeof(struct pollfd));
    if (!pfds) return strada_new_int(-1);

    for (int i = 0; i < nfds; i++) {
        StradaValue *entry = strada_array_get(arr->value.av, i);
        if (entry->type == STRADA_HASH) {
            StradaValue *fd_val = strada_hash_get(entry->value.hv, "fd");
            StradaValue *ev_val = strada_hash_get(entry->value.hv, "events");
            pfds[i].fd = fd_val ? strada_to_int(fd_val) : -1;
            pfds[i].events = ev_val ? strada_to_int(ev_val) : POLLIN;
        }
    }

    int result = poll(pfds, nfds, strada_to_int(timeout));

    /* Update revents in original array */
    for (int i = 0; i < nfds; i++) {
        StradaValue *entry = strada_array_get(arr->value.av, i);
        if (entry->type == STRADA_HASH) {
            strada_hash_set_take(entry->value.hv, "revents", strada_new_int(pfds[i].revents));
        }
    }

    free(pfds);
    return strada_new_int(result);
}

/* ===== RANDOM SEEDING ===== */

StradaValue* strada_srand(StradaValue *seed) {
    unsigned int s = (unsigned int)strada_to_int(seed);
    srand(s);
    return strada_new_int(1);
}

StradaValue* strada_srandom(StradaValue *seed) {
    unsigned int s = (unsigned int)strada_to_int(seed);
    srandom(s);
    return strada_new_int(1);
}

StradaValue* strada_libc_rand(void) {
    return strada_new_int(rand());
}

StradaValue* strada_libc_random(void) {
    return strada_new_int((long)random());
}

/* Generate cryptographically secure random bytes from /dev/urandom
 * Returns hex string of requested length (2 chars per byte)
 * Returns empty string on failure
 */
StradaValue* strada_random_bytes_hex(StradaValue *num_bytes_sv) {
    int num_bytes = (int)strada_to_int(num_bytes_sv);
    if (num_bytes <= 0 || num_bytes > 1024) {
        return strada_new_str("");
    }

    uint8_t *buffer = malloc(num_bytes);
    char *hex = malloc(num_bytes * 2 + 1);
    if (!buffer || !hex) {
        free(buffer);
        free(hex);
        return strada_new_str("");
    }

    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        free(buffer);
        free(hex);
        return strada_new_str("");
    }

    ssize_t n = read(fd, buffer, num_bytes);
    close(fd);

    if (n != num_bytes) {
        free(buffer);
        free(hex);
        return strada_new_str("");
    }

    for (int i = 0; i < num_bytes; i++) {
        sprintf(&hex[i*2], "%02x", buffer[i]);
    }
    hex[num_bytes * 2] = '\0';

    StradaValue *result = strada_new_str(hex);
    free(buffer);
    free(hex);
    return result;
}

/* Generate cryptographically secure random bytes from /dev/urandom
 * Returns raw binary string of requested length
 * Returns empty string on failure
 */
StradaValue* strada_random_bytes(StradaValue *num_bytes_sv) {
    int num_bytes = (int)strada_to_int(num_bytes_sv);
    if (num_bytes <= 0 || num_bytes > 1024) {
        return strada_new_str_len("", 0);
    }

    uint8_t *buffer = malloc(num_bytes);
    if (!buffer) {
        return strada_new_str_len("", 0);
    }

    int fd = open("/dev/urandom", O_RDONLY);
    if (fd < 0) {
        free(buffer);
        return strada_new_str_len("", 0);
    }

    ssize_t n = read(fd, buffer, num_bytes);
    close(fd);

    if (n != num_bytes) {
        free(buffer);
        return strada_new_str_len("", 0);
    }

    StradaValue *result = strada_new_str_len((char *)buffer, num_bytes);
    free(buffer);
    return result;
}

/* ===== ADVANCED SIGNALS ===== */

StradaValue* strada_sigprocmask(StradaValue *how, StradaValue *set) {
    int h = strada_to_int(how);
    sigset_t sigset, oldset;
    sigemptyset(&sigset);

    /* set should be an array of signal numbers */
    StradaValue *arr = set;
    if (arr->type == STRADA_REF) arr = arr->value.rv;
    if (arr->type == STRADA_ARRAY) {
        for (size_t i = 0; i < strada_array_length(arr->value.av); i++) {
            int sig = strada_to_int(strada_array_get(arr->value.av, i));
            sigaddset(&sigset, sig);
        }
    }

    int result = sigprocmask(h, &sigset, &oldset);
    return strada_new_int(result);
}

StradaValue* strada_raise(StradaValue *sig) {
    int s = strada_to_int(sig);
    return strada_new_int(raise(s));
}

StradaValue* strada_killpg(StradaValue *pgrp, StradaValue *sig) {
    pid_t pg = (pid_t)strada_to_int(pgrp);
    int s = strada_to_int(sig);
    return strada_new_int(killpg(pg, s));
}

StradaValue* strada_pause(void) {
    return strada_new_int(pause());
}

/* ===== USER/GROUP DATABASE ===== */

StradaValue* strada_getpwnam(StradaValue *name) {
    char _tb[256];
    const char *n = strada_to_str_buf(name, _tb, sizeof(_tb));
    struct passwd *pw = getpwnam(n);
    if (!pw) { return strada_new_undef(); }

    StradaValue *result = strada_new_hash();
    strada_hash_set_take(result->value.hv, "name", strada_new_str(pw->pw_name));
    strada_hash_set_take(result->value.hv, "passwd", strada_new_str(pw->pw_passwd));
    strada_hash_set_take(result->value.hv, "uid", strada_new_int(pw->pw_uid));
    strada_hash_set_take(result->value.hv, "gid", strada_new_int(pw->pw_gid));
    strada_hash_set_take(result->value.hv, "gecos", strada_new_str(pw->pw_gecos));
    strada_hash_set_take(result->value.hv, "dir", strada_new_str(pw->pw_dir));
    strada_hash_set_take(result->value.hv, "shell", strada_new_str(pw->pw_shell));
    return result;
}

StradaValue* strada_getpwuid(StradaValue *uid) {
    uid_t u = (uid_t)strada_to_int(uid);
    struct passwd *pw = getpwuid(u);
    if (!pw) return strada_new_undef();

    StradaValue *result = strada_new_hash();
    strada_hash_set_take(result->value.hv, "name", strada_new_str(pw->pw_name));
    strada_hash_set_take(result->value.hv, "passwd", strada_new_str(pw->pw_passwd));
    strada_hash_set_take(result->value.hv, "uid", strada_new_int(pw->pw_uid));
    strada_hash_set_take(result->value.hv, "gid", strada_new_int(pw->pw_gid));
    strada_hash_set_take(result->value.hv, "gecos", strada_new_str(pw->pw_gecos));
    strada_hash_set_take(result->value.hv, "dir", strada_new_str(pw->pw_dir));
    strada_hash_set_take(result->value.hv, "shell", strada_new_str(pw->pw_shell));
    return result;
}

StradaValue* strada_getgrnam(StradaValue *name) {
    char _tb[256];
    const char *n = strada_to_str_buf(name, _tb, sizeof(_tb));
    struct group *gr = getgrnam(n);
    if (!gr) { return strada_new_undef(); }

    StradaValue *result = strada_new_hash();
    strada_hash_set_take(result->value.hv, "name", strada_new_str(gr->gr_name));
    strada_hash_set_take(result->value.hv, "passwd", strada_new_str(gr->gr_passwd));
    strada_hash_set_take(result->value.hv, "gid", strada_new_int(gr->gr_gid));

    /* Convert members array */
    StradaValue *members = strada_new_array();
    for (int i = 0; gr->gr_mem[i]; i++) {
        strada_array_push_take(members->value.av, strada_new_str(gr->gr_mem[i]));
    }
    strada_hash_set_take(result->value.hv, "members", members);
    return result;
}

StradaValue* strada_getgrgid(StradaValue *gid) {
    gid_t g = (gid_t)strada_to_int(gid);
    struct group *gr = getgrgid(g);
    if (!gr) return strada_new_undef();

    StradaValue *result = strada_new_hash();
    strada_hash_set_take(result->value.hv, "name", strada_new_str(gr->gr_name));
    strada_hash_set_take(result->value.hv, "passwd", strada_new_str(gr->gr_passwd));
    strada_hash_set_take(result->value.hv, "gid", strada_new_int(gr->gr_gid));

    /* Convert members array */
    StradaValue *members = strada_new_array();
    for (int i = 0; gr->gr_mem[i]; i++) {
        strada_array_push_take(members->value.av, strada_new_str(gr->gr_mem[i]));
    }
    strada_hash_set_take(result->value.hv, "members", members);
    return result;
}

StradaValue* strada_getlogin(void) {
    char *login = getlogin();
    if (!login) return strada_new_undef();
    return strada_new_str(login);
}

StradaValue* strada_getgroups(void) {
    int ngroups = getgroups(0, NULL);
    if (ngroups < 0) return strada_new_undef();

    gid_t *groups = malloc(ngroups * sizeof(gid_t));
    if (!groups) return strada_new_undef();

    ngroups = getgroups(ngroups, groups);

    StradaValue *result = strada_new_array();
    for (int i = 0; i < ngroups; i++) {
        strada_array_push_take(result->value.av, strada_new_int(groups[i]));
    }
    free(groups);
    return result;
}

/* ===== RESOURCE/PRIORITY ===== */

StradaValue* strada_nice(StradaValue *inc) {
    int i = strada_to_int(inc);
    errno = 0;
    int result = nice(i);
    if (result == -1 && errno != 0) {
        return strada_new_int(-1);
    }
    return strada_new_int(result);
}

StradaValue* strada_getpriority(StradaValue *which, StradaValue *who) {
    int w = strada_to_int(which);
    id_t id = (id_t)strada_to_int(who);
    errno = 0;
    int result = getpriority(w, id);
    if (result == -1 && errno != 0) {
        return strada_new_undef();
    }
    return strada_new_int(result);
}

StradaValue* strada_setpriority(StradaValue *which, StradaValue *who, StradaValue *prio) {
    int w = strada_to_int(which);
    id_t id = (id_t)strada_to_int(who);
    int p = strada_to_int(prio);
    return strada_new_int(setpriority(w, id, p));
}

StradaValue* strada_getrusage(StradaValue *who) {
    int w = strada_to_int(who);
    struct rusage usage;

    if (getrusage(w, &usage) < 0) {
        return strada_new_undef();
    }

    StradaValue *result = strada_new_hash();
    strada_hash_set_take(result->value.hv, "utime_sec", strada_new_int(usage.ru_utime.tv_sec));
    strada_hash_set_take(result->value.hv, "utime_usec", strada_new_int(usage.ru_utime.tv_usec));
    strada_hash_set_take(result->value.hv, "stime_sec", strada_new_int(usage.ru_stime.tv_sec));
    strada_hash_set_take(result->value.hv, "stime_usec", strada_new_int(usage.ru_stime.tv_usec));
    strada_hash_set_take(result->value.hv, "maxrss", strada_new_int(usage.ru_maxrss));
    strada_hash_set_take(result->value.hv, "minflt", strada_new_int(usage.ru_minflt));
    strada_hash_set_take(result->value.hv, "majflt", strada_new_int(usage.ru_majflt));
    strada_hash_set_take(result->value.hv, "nvcsw", strada_new_int(usage.ru_nvcsw));
    strada_hash_set_take(result->value.hv, "nivcsw", strada_new_int(usage.ru_nivcsw));
    return result;
}

StradaValue* strada_getrlimit(StradaValue *resource) {
    int r = strada_to_int(resource);
    struct rlimit rlim;

    if (getrlimit(r, &rlim) < 0) {
        return strada_new_undef();
    }

    StradaValue *result = strada_new_hash();
    strada_hash_set_take(result->value.hv, "cur", strada_new_int((int64_t)rlim.rlim_cur));
    strada_hash_set_take(result->value.hv, "max", strada_new_int((int64_t)rlim.rlim_max));
    return result;
}

StradaValue* strada_setrlimit(StradaValue *resource, StradaValue *rlim) {
    int r = strada_to_int(resource);
    struct rlimit limit;

    if (rlim->type != STRADA_HASH) return strada_new_int(-1);

    StradaValue *cur = strada_hash_get(rlim->value.hv, "cur");
    StradaValue *max = strada_hash_get(rlim->value.hv, "max");

    limit.rlim_cur = cur ? (rlim_t)strada_to_int(cur) : RLIM_INFINITY;
    limit.rlim_max = max ? (rlim_t)strada_to_int(max) : RLIM_INFINITY;

    return strada_new_int(setrlimit(r, &limit));
}

/* ===== ADDITIONAL TIME FUNCTIONS ===== */

StradaValue* strada_difftime(StradaValue *t1, StradaValue *t0) {
    time_t time1 = (time_t)strada_to_int(t1);
    time_t time0 = (time_t)strada_to_int(t0);
    return strada_new_num(difftime(time1, time0));
}

StradaValue* strada_clock(void) {
    return strada_new_int((int64_t)clock());
}

StradaValue* strada_times(void) {
    struct tms buf;
    clock_t result = times(&buf);

    if (result == (clock_t)-1) {
        return strada_new_undef();
    }

    StradaValue *hash = strada_new_hash();
    strada_hash_set_take(hash->value.hv, "ticks", strada_new_int((int64_t)result));
    strada_hash_set_take(hash->value.hv, "utime", strada_new_int((int64_t)buf.tms_utime));
    strada_hash_set_take(hash->value.hv, "stime", strada_new_int((int64_t)buf.tms_stime));
    strada_hash_set_take(hash->value.hv, "cutime", strada_new_int((int64_t)buf.tms_cutime));
    strada_hash_set_take(hash->value.hv, "cstime", strada_new_int((int64_t)buf.tms_cstime));
    return hash;
}

/* ===== ADDITIONAL MEMORY FUNCTIONS ===== */

StradaValue* strada_calloc(StradaValue *nmemb, StradaValue *size) {
    size_t n = (size_t)strada_to_int(nmemb);
    size_t s = (size_t)strada_to_int(size);
    void *ptr = calloc(n, s);
    if (!ptr) return strada_new_int(0);
    return strada_new_int((int64_t)(intptr_t)ptr);
}

StradaValue* strada_realloc(StradaValue *ptr, StradaValue *size) {
    void *p = (void*)(intptr_t)strada_to_int(ptr);
    size_t s = (size_t)strada_to_int(size);
    void *result = realloc(p, s);
    if (!result && s > 0) return strada_new_int(0);
    return strada_new_int((int64_t)(intptr_t)result);
}

StradaValue* strada_mmap(StradaValue *addr, StradaValue *length, StradaValue *prot, StradaValue *flags, StradaValue *fd, StradaValue *offset) {
    void *a = (void*)(intptr_t)strada_to_int(addr);
    size_t len = (size_t)strada_to_int(length);
    int pr = strada_to_int(prot);
    int fl = strada_to_int(flags);
    int f = strada_to_int(fd);
    off_t off = (off_t)strada_to_int(offset);

    void *result = mmap(a, len, pr, fl, f, off);
    if (result == MAP_FAILED) return strada_new_int(-1);
    return strada_new_int((int64_t)(intptr_t)result);
}

StradaValue* strada_munmap(StradaValue *addr, StradaValue *length) {
    void *a = (void*)(intptr_t)strada_to_int(addr);
    size_t len = (size_t)strada_to_int(length);
    return strada_new_int(munmap(a, len));
}

StradaValue* strada_mlock(StradaValue *addr, StradaValue *len) {
    void *a = (void*)(intptr_t)strada_to_int(addr);
    size_t l = (size_t)strada_to_int(len);
    return strada_new_int(mlock(a, l));
}

StradaValue* strada_munlock(StradaValue *addr, StradaValue *len) {
    void *a = (void*)(intptr_t)strada_to_int(addr);
    size_t l = (size_t)strada_to_int(len);
    return strada_new_int(munlock(a, l));
}

/* ===== STRING CONVERSION ===== */

StradaValue* strada_strtol(StradaValue *str, StradaValue *base) {
    char _tb[256];
    const char *s = strada_to_str_buf(str, _tb, sizeof(_tb));
    int b = strada_to_int(base);
    char *endptr;
    errno = 0;
    long result = strtol(s, &endptr, b);

    if (errno != 0) {
        return strada_new_undef();
    }

    int64_t consumed = (int64_t)(endptr - s);
    StradaValue *arr = strada_new_array();
    strada_array_push_take(arr->value.av, strada_new_int(result));
    strada_array_push_take(arr->value.av, strada_new_int(consumed));
    return arr;
}

StradaValue* strada_strtod(StradaValue *str) {
    char _tb[256];
    const char *s = strada_to_str_buf(str, _tb, sizeof(_tb));
    char *endptr;
    errno = 0;
    double result = strtod(s, &endptr);

    if (errno != 0) {
        return strada_new_undef();
    }

    int64_t consumed = (int64_t)(endptr - s);
    StradaValue *arr = strada_new_array();
    strada_array_push_take(arr->value.av, strada_new_num(result));
    strada_array_push_take(arr->value.av, strada_new_int(consumed));
    return arr;
}

StradaValue* strada_atoi(StradaValue *str) {
    char _tb[256];
    const char *s = strada_to_str_buf(str, _tb, sizeof(_tb));
    int64_t result = atoi(s);
    return strada_new_int(result);
}

StradaValue* strada_atof(StradaValue *str) {
    char _tb[256];
    const char *s = strada_to_str_buf(str, _tb, sizeof(_tb));
    double result = atof(s);
    return strada_new_num(result);
}

/* ===== TERMINAL/TTY ===== */

StradaValue* strada_ttyname(StradaValue *fd) {
    int f = strada_to_int(fd);
    char *name = ttyname(f);
    if (!name) return strada_new_undef();
    return strada_new_str(name);
}

StradaValue* strada_tcgetattr(StradaValue *fd) {
    int f = strada_to_int(fd);
    struct termios t;

    if (tcgetattr(f, &t) < 0) {
        return strada_new_undef();
    }

    StradaValue *result = strada_new_hash();
    strada_hash_set_take(result->value.hv, "iflag", strada_new_int(t.c_iflag));
    strada_hash_set_take(result->value.hv, "oflag", strada_new_int(t.c_oflag));
    strada_hash_set_take(result->value.hv, "cflag", strada_new_int(t.c_cflag));
    strada_hash_set_take(result->value.hv, "lflag", strada_new_int(t.c_lflag));
    strada_hash_set_take(result->value.hv, "ispeed", strada_new_int(cfgetispeed(&t)));
    strada_hash_set_take(result->value.hv, "ospeed", strada_new_int(cfgetospeed(&t)));
    return result;
}

StradaValue* strada_tcsetattr(StradaValue *fd, StradaValue *when, StradaValue *attrs) {
    int f = strada_to_int(fd);
    int w = strada_to_int(when);

    if (attrs->type != STRADA_HASH) return strada_new_int(-1);

    struct termios t;
    if (tcgetattr(f, &t) < 0) return strada_new_int(-1);

    StradaValue *v;
    if ((v = strada_hash_get(attrs->value.hv, "iflag"))) t.c_iflag = strada_to_int(v);
    if ((v = strada_hash_get(attrs->value.hv, "oflag"))) t.c_oflag = strada_to_int(v);
    if ((v = strada_hash_get(attrs->value.hv, "cflag"))) t.c_cflag = strada_to_int(v);
    if ((v = strada_hash_get(attrs->value.hv, "lflag"))) t.c_lflag = strada_to_int(v);

    /* Handle baud rate setting */
    if ((v = strada_hash_get(attrs->value.hv, "ispeed"))) {
        cfsetispeed(&t, (speed_t)strada_to_int(v));
    }
    if ((v = strada_hash_get(attrs->value.hv, "ospeed"))) {
        cfsetospeed(&t, (speed_t)strada_to_int(v));
    }
    /* Also support "speed" for setting both at once */
    if ((v = strada_hash_get(attrs->value.hv, "speed"))) {
        speed_t spd = (speed_t)strada_to_int(v);
        cfsetispeed(&t, spd);
        cfsetospeed(&t, spd);
    }

    return strada_new_int(tcsetattr(f, w, &t));
}

StradaValue* strada_cfgetospeed(StradaValue *termios) {
    if (termios->type != STRADA_HASH) return strada_new_int(-1);
    StradaValue *ospeed = strada_hash_get(termios->value.hv, "ospeed");
    if (ospeed) { strada_incref(ospeed); return ospeed; }
    return strada_new_int(0);
}

StradaValue* strada_cfsetospeed(StradaValue *termios, StradaValue *speed) {
    if (termios->type != STRADA_HASH) return strada_new_int(-1);
    strada_hash_set(termios->value.hv, "ospeed", speed);
    return strada_new_int(0);
}

StradaValue* strada_cfgetispeed(StradaValue *termios) {
    if (termios->type != STRADA_HASH) return strada_new_int(-1);
    StradaValue *ispeed = strada_hash_get(termios->value.hv, "ispeed");
    if (ispeed) { strada_incref(ispeed); return ispeed; }
    return strada_new_int(0);
}

StradaValue* strada_cfsetispeed(StradaValue *termios, StradaValue *speed) {
    if (termios->type != STRADA_HASH) return strada_new_int(-1);
    strada_hash_set(termios->value.hv, "ispeed", speed);
    return strada_new_int(0);
}

/* High-level serial port open function
 * Opens a serial port device and configures it for raw I/O
 * Parameters:
 *   device - path to serial device (e.g., "/dev/ttyUSB0")
 *   baud   - baud rate constant (e.g., B9600, B115200)
 *   config - optional config string: "8N1", "7E1", "8N2", etc.
 *            Default is "8N1" (8 data bits, no parity, 1 stop bit)
 * Returns: file descriptor on success, -1 on error
 */
StradaValue* strada_serial_open(const char *device, int baud, const char *config) {
    int fd = open(device, O_RDWR | O_NOCTTY | O_NDELAY);
    if (fd < 0) {
        return strada_new_int(-1);
    }

    /* Clear the non-blocking flag now that we've opened it */
    fcntl(fd, F_SETFL, 0);

    struct termios t;
    if (tcgetattr(fd, &t) < 0) {
        close(fd);
        return strada_new_int(-1);
    }

    /* Set raw mode - disable all processing */
    t.c_iflag &= ~(IGNBRK | BRKINT | PARMRK | ISTRIP | INLCR | IGNCR | ICRNL | IXON);
    t.c_oflag &= ~OPOST;
    t.c_lflag &= ~(ECHO | ECHONL | ICANON | ISIG | IEXTEN);

    /* Parse config string (default "8N1") */
    int databits = 8;
    char parity = 'N';
    int stopbits = 1;

    if (config && strlen(config) >= 3) {
        databits = config[0] - '0';
        parity = config[1];
        stopbits = config[2] - '0';
    }

    /* Set character size */
    t.c_cflag &= ~CSIZE;
    switch (databits) {
        case 5: t.c_cflag |= CS5; break;
        case 6: t.c_cflag |= CS6; break;
        case 7: t.c_cflag |= CS7; break;
        case 8:
        default: t.c_cflag |= CS8; break;
    }

    /* Set parity */
    switch (parity) {
        case 'E': case 'e':
            t.c_cflag |= PARENB;
            t.c_cflag &= ~PARODD;
            break;
        case 'O': case 'o':
            t.c_cflag |= PARENB;
            t.c_cflag |= PARODD;
            break;
        case 'N': case 'n':
        default:
            t.c_cflag &= ~PARENB;
            break;
    }

    /* Set stop bits */
    if (stopbits == 2) {
        t.c_cflag |= CSTOPB;
    } else {
        t.c_cflag &= ~CSTOPB;
    }

    /* Enable receiver and set local mode */
    t.c_cflag |= (CLOCAL | CREAD);

    /* Disable hardware flow control */
#ifdef CRTSCTS
    t.c_cflag &= ~CRTSCTS;
#endif

    /* Set baud rate */
    cfsetispeed(&t, (speed_t)baud);
    cfsetospeed(&t, (speed_t)baud);

    /* Set VMIN and VTIME for blocking read with timeout */
    t.c_cc[VMIN] = 0;   /* Return as soon as any data is available */
    t.c_cc[VTIME] = 10; /* 1 second timeout (in tenths of a second) */

    /* Apply settings */
    if (tcsetattr(fd, TCSANOW, &t) < 0) {
        close(fd);
        return strada_new_int(-1);
    }

    /* Flush any pending data */
    tcflush(fd, TCIOFLUSH);

    return strada_new_int(fd);
}

/* tcflush - flush input/output queues */
StradaValue* strada_tcflush(StradaValue *fd, StradaValue *queue) {
    int f = strada_to_int(fd);
    int q = strada_to_int(queue);
    return strada_new_int(tcflush(f, q));
}

/* tcdrain - wait until all output has been transmitted */
StradaValue* strada_tcdrain(StradaValue *fd) {
    int f = strada_to_int(fd);
    return strada_new_int(tcdrain(f));
}

/* ===== ADVANCED FILE OPERATIONS ===== */

StradaValue* strada_fcntl(StradaValue *fd, StradaValue *cmd, StradaValue *arg) {
    int f = strada_to_int(fd);
    int c = strada_to_int(cmd);
    int a = arg ? strada_to_int(arg) : 0;
    return strada_new_int(fcntl(f, c, a));
}

StradaValue* strada_flock(StradaValue *fd, StradaValue *operation) {
    int f = strada_to_int(fd);
    int op = strada_to_int(operation);
    return strada_new_int(flock(f, op));
}

StradaValue* strada_quotemeta(StradaValue *str) {
    char _tb[256];
    const char *s = strada_to_str_buf(str, _tb, sizeof(_tb));
    size_t len = strlen(s);
    /* Worst case: every char is escaped -> 2x length */
    char *buf = (char *)malloc(len * 2 + 1);
    size_t j = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)s[i];
        /* Escape non-alphanumeric, non-underscore ASCII characters */
        if (c < 128 && !((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                         (c >= '0' && c <= '9') || c == '_')) {
            buf[j++] = '\\';
        }
        buf[j++] = (char)c;
    }
    buf[j] = '\0';
    StradaValue *result = strada_new_str(buf);
    free(buf);
    return result;
}

StradaValue* strada_ioctl(StradaValue *fd, StradaValue *request, StradaValue *arg) {
    int f = strada_to_int(fd);
    unsigned long req = (unsigned long)strada_to_int(request);
    int a = arg ? strada_to_int(arg) : 0;
    return strada_new_int(ioctl(f, req, a));
}

StradaValue* strada_statvfs(StradaValue *path) {
    char _tb[PATH_MAX];
    const char *p = strada_to_str_buf(path, _tb, sizeof(_tb));
    struct statvfs buf;

    if (statvfs(p, &buf) < 0) {
        return strada_new_undef();
    }

    StradaValue *result = strada_new_hash();
    strada_hash_set_take(result->value.hv, "bsize", strada_new_int((int64_t)buf.f_bsize));
    strada_hash_set_take(result->value.hv, "frsize", strada_new_int((int64_t)buf.f_frsize));
    strada_hash_set_take(result->value.hv, "blocks", strada_new_int((int64_t)buf.f_blocks));
    strada_hash_set_take(result->value.hv, "bfree", strada_new_int((int64_t)buf.f_bfree));
    strada_hash_set_take(result->value.hv, "bavail", strada_new_int((int64_t)buf.f_bavail));
    strada_hash_set_take(result->value.hv, "files", strada_new_int((int64_t)buf.f_files));
    strada_hash_set_take(result->value.hv, "ffree", strada_new_int((int64_t)buf.f_ffree));
    strada_hash_set_take(result->value.hv, "favail", strada_new_int((int64_t)buf.f_favail));
    strada_hash_set_take(result->value.hv, "fsid", strada_new_int((int64_t)buf.f_fsid));
    strada_hash_set_take(result->value.hv, "flag", strada_new_int((int64_t)buf.f_flag));
    strada_hash_set_take(result->value.hv, "namemax", strada_new_int((int64_t)buf.f_namemax));
    return result;
}

StradaValue* strada_fstatvfs(StradaValue *fd) {
    int f = strada_to_int(fd);
    struct statvfs buf;

    if (fstatvfs(f, &buf) < 0) {
        return strada_new_undef();
    }

    StradaValue *result = strada_new_hash();
    strada_hash_set_take(result->value.hv, "bsize", strada_new_int((int64_t)buf.f_bsize));
    strada_hash_set_take(result->value.hv, "frsize", strada_new_int((int64_t)buf.f_frsize));
    strada_hash_set_take(result->value.hv, "blocks", strada_new_int((int64_t)buf.f_blocks));
    strada_hash_set_take(result->value.hv, "bfree", strada_new_int((int64_t)buf.f_bfree));
    strada_hash_set_take(result->value.hv, "bavail", strada_new_int((int64_t)buf.f_bavail));
    strada_hash_set_take(result->value.hv, "files", strada_new_int((int64_t)buf.f_files));
    strada_hash_set_take(result->value.hv, "ffree", strada_new_int((int64_t)buf.f_ffree));
    strada_hash_set_take(result->value.hv, "favail", strada_new_int((int64_t)buf.f_favail));
    strada_hash_set_take(result->value.hv, "fsid", strada_new_int((int64_t)buf.f_fsid));
    strada_hash_set_take(result->value.hv, "flag", strada_new_int((int64_t)buf.f_flag));
    strada_hash_set_take(result->value.hv, "namemax", strada_new_int((int64_t)buf.f_namemax));
    return result;
}

StradaValue* strada_dup(StradaValue *oldfd) {
    int f = strada_to_int(oldfd);
    return strada_new_int(dup(f));
}

/* ===== ADDITIONAL MATH FUNCTIONS ===== */

StradaValue* strada_hypot(StradaValue *x, StradaValue *y) {
    return strada_new_num(hypot(strada_to_num(x), strada_to_num(y)));
}

StradaValue* strada_cbrt(StradaValue *x) {
    return strada_new_num(cbrt(strada_to_num(x)));
}

StradaValue* strada_isnan(StradaValue *x) {
    return strada_new_int(isnan(strada_to_num(x)) ? 1 : 0);
}

StradaValue* strada_isinf(StradaValue *x) {
    return strada_new_int(isinf(strada_to_num(x)));
}

StradaValue* strada_isfinite(StradaValue *x) {
    return strada_new_int(isfinite(strada_to_num(x)) ? 1 : 0);
}

StradaValue* strada_fmax(StradaValue *x, StradaValue *y) {
    return strada_new_num(fmax(strada_to_num(x), strada_to_num(y)));
}

StradaValue* strada_fmin(StradaValue *x, StradaValue *y) {
    return strada_new_num(fmin(strada_to_num(x), strada_to_num(y)));
}

StradaValue* strada_copysign(StradaValue *x, StradaValue *y) {
    return strada_new_num(copysign(strada_to_num(x), strada_to_num(y)));
}

StradaValue* strada_remainder(StradaValue *x, StradaValue *y) {
    return strada_new_num(remainder(strada_to_num(x), strada_to_num(y)));
}

StradaValue* strada_trunc(StradaValue *x) {
    return strada_new_num(trunc(strada_to_num(x)));
}

StradaValue* strada_ldexp(StradaValue *x, StradaValue *exp) {
    return strada_new_num(ldexp(strada_to_num(x), strada_to_int(exp)));
}

StradaValue* strada_frexp(StradaValue *x) {
    int exp;
    double result = frexp(strada_to_num(x), &exp);
    StradaValue *arr = strada_new_array();
    strada_array_push_take(arr->value.av, strada_new_num(result));
    strada_array_push_take(arr->value.av, strada_new_int(exp));
    return arr;
}

StradaValue* strada_modf(StradaValue *x) {
    double intpart;
    double result = modf(strada_to_num(x), &intpart);
    StradaValue *arr = strada_new_array();
    strada_array_push_take(arr->value.av, strada_new_num(result));
    strada_array_push_take(arr->value.av, strada_new_num(intpart));
    return arr;
}

StradaValue* strada_scalbn(StradaValue *x, StradaValue *n) {
    return strada_new_num(scalbn(strada_to_num(x), strada_to_int(n)));
}

/* ============================================================
 * Raw dlopen/dlsym functions for import_lib compile-time metadata extraction
 * These return StradaValue* wrapping int to be compatible with Strada calling convention
 * ============================================================ */

/* strada_dl_open_raw - load shared library, return handle wrapped as int */
StradaValue* strada_dl_open_raw(StradaValue *path) {
    if (!path) return strada_new_int(0);
    char _tb[PATH_MAX];
    const char *p = strada_to_str_buf(path, _tb, sizeof(_tb));
    void *handle = dlopen(p, RTLD_LAZY);
    return strada_new_int((int64_t)(intptr_t)handle);
}

/* strada_dl_sym_raw - get symbol from library, return pointer wrapped as int */
StradaValue* strada_dl_sym_raw(StradaValue *handle_sv, StradaValue *symbol) {
    if (!handle_sv || !symbol) return strada_new_int(0);
    void *handle = (void*)(intptr_t)strada_to_int(handle_sv);
    char _tb[256];
    const char *sym = strada_to_str_buf(symbol, _tb, sizeof(_tb));
    void *ptr = dlsym(handle, sym);
    return strada_new_int((int64_t)(intptr_t)ptr);
}

/* strada_dl_close_raw - close library */
StradaValue* strada_dl_close_raw(StradaValue *handle_sv) {
    if (handle_sv) {
        void *handle = (void*)(intptr_t)strada_to_int(handle_sv);
        if (handle) dlclose(handle);
    }
    return strada_new_undef();
}

/* strada_dl_call_export_info - call __strada_export_info function and return string */
StradaValue* strada_dl_call_export_info(StradaValue *fn_ptr_sv) {
    if (!fn_ptr_sv) return strada_new_str("");
    void *fn = (void*)(intptr_t)strada_to_int(fn_ptr_sv);
    if (!fn) return strada_new_str("");

    /* Call the function - it takes no args and returns const char* */
    typedef const char* (*export_info_fn)(void);
    const char *result = ((export_info_fn)fn)();

    if (!result) return strada_new_str("");
    return strada_new_str(result);
}

/* strada_dl_call_version - call __strada_version function and return string */
StradaValue* strada_dl_call_version(StradaValue *fn_ptr_sv) {
    if (!fn_ptr_sv) return strada_new_str("");
    void *fn = (void*)(intptr_t)strada_to_int(fn_ptr_sv);
    if (!fn) return strada_new_str("");

    /* Call the function - it takes no args and returns const char* */
    typedef const char* (*version_fn)(void);
    const char *result = ((version_fn)fn)();

    if (!result) return strada_new_str("");
    return strada_new_str(result);
}

/* ============================================================
 * FUNCTION PROFILER
 * Track function call counts and timing
 * ============================================================ */

#define PROFILE_MAX_FUNCS 4096
#define PROFILE_MAX_STACK 256

typedef struct ProfileEntry {
    const char *name;          /* Function name (string literal, not owned) */
    uint64_t call_count;       /* Number of times called */
    double total_time;         /* Total time in seconds (excluding children) */
    double self_time;          /* Self time (excluding children) */
    double start_time;         /* Start time of current call */
    int active;                /* Currently in this function? */
} ProfileEntry;

typedef struct ProfileStack {
    int func_idx;              /* Index into profile_entries */
    double start_time;         /* When we entered this function */
    double child_time;         /* Time spent in child functions */
} ProfileStack;

static ProfileEntry profile_entries[PROFILE_MAX_FUNCS];
static int profile_entry_count = 0;
static ProfileStack profile_stack[PROFILE_MAX_STACK];
static int profile_stack_depth = 0;
static int profile_initialized = 0;

/* Get high-resolution time */
static double profile_get_time(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return ts.tv_sec + ts.tv_nsec / 1e9;
}

/* Find or create profile entry for function */
static int profile_find_or_create(const char *name) {
    /* Linear search - fine for typical function counts */
    for (int i = 0; i < profile_entry_count; i++) {
        if (profile_entries[i].name == name ||
            (profile_entries[i].name && strcmp(profile_entries[i].name, name) == 0)) {
            return i;
        }
    }

    /* Create new entry */
    if (profile_entry_count >= PROFILE_MAX_FUNCS) {
        fprintf(stderr, "Warning: profile table full, ignoring function %s\n", name);
        return -1;
    }

    int idx = profile_entry_count++;
    profile_entries[idx].name = name;
    profile_entries[idx].call_count = 0;
    profile_entries[idx].total_time = 0.0;
    profile_entries[idx].self_time = 0.0;
    profile_entries[idx].start_time = 0.0;
    profile_entries[idx].active = 0;
    return idx;
}

void strada_profile_init(void) {
    profile_entry_count = 0;
    profile_stack_depth = 0;
    profile_initialized = 1;
    memset(profile_entries, 0, sizeof(profile_entries));
    memset(profile_stack, 0, sizeof(profile_stack));
}

void strada_profile_enter(const char *func_name) {
    if (!profile_initialized) return;

    int idx = profile_find_or_create(func_name);
    if (idx < 0) return;

    double now = profile_get_time();

    /* Push onto call stack */
    if (profile_stack_depth < PROFILE_MAX_STACK) {
        profile_stack[profile_stack_depth].func_idx = idx;
        profile_stack[profile_stack_depth].start_time = now;
        profile_stack[profile_stack_depth].child_time = 0.0;
        profile_stack_depth++;
    }

    profile_entries[idx].call_count++;
    profile_entries[idx].start_time = now;
    profile_entries[idx].active = 1;
}

void strada_profile_exit(const char *func_name) {
    (void)func_name;  /* Currently unused - exit uses stack frame index */
    if (!profile_initialized || profile_stack_depth == 0) return;

    double now = profile_get_time();

    /* Pop from stack */
    profile_stack_depth--;
    ProfileStack *frame = &profile_stack[profile_stack_depth];
    int idx = frame->func_idx;

    double elapsed = now - frame->start_time;
    double self_time = elapsed - frame->child_time;

    profile_entries[idx].total_time += elapsed;
    profile_entries[idx].self_time += self_time;
    profile_entries[idx].active = 0;

    /* Add our time to parent's child_time */
    if (profile_stack_depth > 0) {
        profile_stack[profile_stack_depth - 1].child_time += elapsed;
    }
}

/* Comparison function for sorting by self_time descending */
static int profile_compare(const void *a, const void *b) {
    const ProfileEntry *ea = (const ProfileEntry *)a;
    const ProfileEntry *eb = (const ProfileEntry *)b;
    if (eb->self_time > ea->self_time) return 1;
    if (eb->self_time < ea->self_time) return -1;
    return 0;
}

void strada_profile_report(void) {
    if (!profile_initialized || profile_entry_count == 0) return;

    /* Sort entries by self_time */
    qsort(profile_entries, profile_entry_count, sizeof(ProfileEntry), profile_compare);

    /* Calculate totals */
    double total_time = 0.0;
    uint64_t total_calls = 0;
    for (int i = 0; i < profile_entry_count; i++) {
        total_time += profile_entries[i].self_time;
        total_calls += profile_entries[i].call_count;
    }

    fprintf(stderr, "\n");
    fprintf(stderr, "╔══════════════════════════════════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║                           STRADA FUNCTION PROFILER                           ║\n");
    fprintf(stderr, "╠══════════════════════════════════════════════════════════════════════════════╣\n");
    fprintf(stderr, "║  %%Self    Self(s)   Total(s)     Calls   Function                            ║\n");
    fprintf(stderr, "╠══════════════════════════════════════════════════════════════════════════════╣\n");

    for (int i = 0; i < profile_entry_count && i < 30; i++) {
        ProfileEntry *e = &profile_entries[i];
        if (e->call_count == 0) continue;
        if (e->name == NULL) continue;  /* Skip entries with NULL names */

        double pct = (total_time > 0) ? (e->self_time / total_time * 100.0) : 0.0;

        /* Truncate function name if needed */
        char name_buf[41];
        if (strlen(e->name) > 40) {
            strncpy(name_buf, e->name, 37);
            name_buf[37] = '.';
            name_buf[38] = '.';
            name_buf[39] = '.';
            name_buf[40] = '\0';
        } else {
            strncpy(name_buf, e->name, 40);
            name_buf[40] = '\0';
        }

        fprintf(stderr, "║ %5.1f%%  %9.4f  %9.4f  %8lu   %-40s ║\n",
                pct, e->self_time, e->total_time,
                (unsigned long)e->call_count, name_buf);
    }

    fprintf(stderr, "╠══════════════════════════════════════════════════════════════════════════════╣\n");
    fprintf(stderr, "║ Total: %.4f seconds, %lu function calls, %d unique functions            ║\n",
            total_time, (unsigned long)total_calls, profile_entry_count);
    fprintf(stderr, "╚══════════════════════════════════════════════════════════════════════════════╝\n");
}

/* ============================================================
 * GLOBAL VARIABLE REGISTRY
 * Shared across all modules (including dynamically loaded .so)
 * ============================================================ */

static StradaHash *strada_global_registry = NULL;

static void strada_global_init(void) {
    if (!strada_global_registry) {
        strada_global_registry = strada_hash_new();
    }
}

void strada_global_set(StradaValue *name_sv, StradaValue *val) {
    strada_global_init();
    char _tb[256];
    const char *name = strada_to_str_buf(name_sv, _tb, sizeof(_tb));
    strada_hash_set(strada_global_registry, name, val);
    strada_decref(name_sv);  /* Consume name temp */
    strada_decref(val);      /* hash_set increfs; release caller's ref */
}

StradaValue* strada_global_get(StradaValue *name_sv) {
    strada_global_init();
    char _tb[256];
    const char *name = strada_to_str_buf(name_sv, _tb, sizeof(_tb));
    StradaValue *val = strada_hash_get(strada_global_registry, name);
    strada_decref(name_sv);  /* Consume name temp */
    if (!val || val->type == STRADA_UNDEF) return strada_undef_static();
    strada_incref(val);  /* Return owned reference - caller must decref */
    return val;
}

int strada_global_exists(StradaValue *name_sv) {
    if (!strada_global_registry) {
        strada_decref(name_sv);
        return 0;
    }
    char _tb[256];
    const char *name = strada_to_str_buf(name_sv, _tb, sizeof(_tb));
    int result = strada_hash_exists(strada_global_registry, name);
    strada_decref(name_sv);  /* Consume name temp */
    return result;
}

void strada_global_delete(StradaValue *name_sv) {
    if (!strada_global_registry) {
        strada_decref(name_sv);
        return;
    }
    char _tb[256];
    const char *name = strada_to_str_buf(name_sv, _tb, sizeof(_tb));
    strada_hash_delete(strada_global_registry, name);
    strada_decref(name_sv);  /* Consume name temp */
}

StradaValue* strada_global_keys(void) {
    StradaValue *result = strada_new_array();
    if (!strada_global_registry) return result;
    StradaArray *keys = strada_hash_keys(strada_global_registry);
    for (size_t i = 0; i < keys->size; i++) {
        /* Elements have refcount 1 from hash_keys (push_take).
         * Use push_take to transfer ownership without extra incref. */
        strada_array_push_take(result->value.av, keys->elements[keys->head + i]);
    }
    /* Free the array structure but not the elements (transferred to result) */
    free(keys->elements);
    free(keys);
    return result;
}

/* ============================================================
 * MEMORY PROFILER
 * Track allocations by type and detect leaks
 * ============================================================ */

static int memprof_enabled = 0;

typedef struct MemProfStats {
    uint64_t alloc_count;      /* Number of allocations */
    uint64_t free_count;       /* Number of frees */
    uint64_t current_count;    /* Currently allocated */
    uint64_t peak_count;       /* Peak allocated */
    uint64_t total_bytes;      /* Total bytes allocated */
    uint64_t current_bytes;    /* Currently allocated bytes */
    uint64_t peak_bytes;       /* Peak bytes */
} MemProfStats;

/* Stats by type: undef, int, num, str, array, hash, ref, other */
static MemProfStats memprof_stats[16];
static const char *memprof_type_names[] = {
    "undef", "int", "num", "str", "array", "hash", "ref",
    "filehandle", "regex", "socket", "cstruct", "cpointer", "closure",
    "unknown", "unknown", "unknown"
};

void strada_memprof_enable(void) {
    memprof_enabled = 1;
    memset(memprof_stats, 0, sizeof(memprof_stats));
}

void strada_memprof_disable(void) {
    memprof_enabled = 0;
}

void strada_memprof_reset(void) {
    memset(memprof_stats, 0, sizeof(memprof_stats));
}

/* Internal: record allocation */
void strada_memprof_alloc(StradaType type, size_t bytes) {
    if (!memprof_enabled) return;
    int idx = (type < 16) ? type : 15;
    MemProfStats *s = &memprof_stats[idx];
    s->alloc_count++;
    s->current_count++;
    s->total_bytes += bytes;
    s->current_bytes += bytes;
    if (s->current_count > s->peak_count) s->peak_count = s->current_count;
    if (s->current_bytes > s->peak_bytes) s->peak_bytes = s->current_bytes;
}

/* Internal: record free */
void strada_memprof_free(StradaType type, size_t bytes) {
    if (!memprof_enabled) return;
    int idx = (type < 16) ? type : 15;
    MemProfStats *s = &memprof_stats[idx];
    s->free_count++;
    if (s->current_count > 0) s->current_count--;
    if (s->current_bytes >= bytes) s->current_bytes -= bytes;
}

void strada_memprof_report(void) {
    fprintf(stderr, "\n");
    fprintf(stderr, "╔══════════════════════════════════════════════════════════════════════════════╗\n");
    fprintf(stderr, "║                           STRADA MEMORY PROFILER                             ║\n");
    fprintf(stderr, "╠══════════════════════════════════════════════════════════════════════════════╣\n");
    fprintf(stderr, "║ Type          Allocs     Frees   Current      Peak   Cur Bytes   Peak Bytes ║\n");
    fprintf(stderr, "╠══════════════════════════════════════════════════════════════════════════════╣\n");

    uint64_t total_allocs = 0, total_frees = 0, total_current = 0;
    uint64_t total_cur_bytes = 0, total_peak_bytes = 0;

    for (int i = 0; i < 13; i++) {
        MemProfStats *s = &memprof_stats[i];
        if (s->alloc_count == 0) continue;

        fprintf(stderr, "║ %-10s %9lu %9lu %9lu %9lu %10lu %11lu ║\n",
                memprof_type_names[i],
                (unsigned long)s->alloc_count,
                (unsigned long)s->free_count,
                (unsigned long)s->current_count,
                (unsigned long)s->peak_count,
                (unsigned long)s->current_bytes,
                (unsigned long)s->peak_bytes);

        total_allocs += s->alloc_count;
        total_frees += s->free_count;
        total_current += s->current_count;
        total_cur_bytes += s->current_bytes;
        total_peak_bytes += s->peak_bytes;
    }

    fprintf(stderr, "╠══════════════════════════════════════════════════════════════════════════════╣\n");
    fprintf(stderr, "║ TOTAL      %9lu %9lu %9lu           %10lu              ║\n",
            (unsigned long)total_allocs,
            (unsigned long)total_frees,
            (unsigned long)total_current,
            (unsigned long)total_cur_bytes);

    if (total_current > 0) {
        fprintf(stderr, "╠══════════════════════════════════════════════════════════════════════════════╣\n");
        fprintf(stderr, "║ WARNING: %lu values still allocated (possible memory leak)                   ║\n",
                (unsigned long)total_current);
    }
    fprintf(stderr, "╚══════════════════════════════════════════════════════════════════════════════╝\n");
}

/* ============================================================
 * C INTEROP HELPER FUNCTIONS (c:: namespace)
 * For use with extern "C" FFI
 * ============================================================ */

/* c::str_to_ptr - Convert Strada string to C char* (allocates - user must free) */
StradaValue* strada_c_str_to_ptr(StradaValue *sv) {
    if (!sv) return strada_new_int(0);
    char *s = strada_to_str(sv);
    return strada_new_int((int64_t)s);
}

/* c::ptr_to_str - Convert C char* to Strada string (copies into Strada) */
StradaValue* strada_c_ptr_to_str(StradaValue *ptr_sv) {
    if (!ptr_sv) return strada_new_str("");
    char *p = (char*)strada_to_int(ptr_sv);
    if (!p) return strada_new_str("");
    return strada_new_str(p);
}

/* c::ptr_to_str_n - Convert C char* to Strada string with explicit length */
StradaValue* strada_c_ptr_to_str_n(StradaValue *ptr_sv, StradaValue *len_sv) {
    if (!ptr_sv || !len_sv) return strada_new_str("");
    char *p = (char*)strada_to_int(ptr_sv);
    int64_t len = strada_to_int(len_sv);
    if (!p || len <= 0) return strada_new_str("");
    return strada_new_str_len(p, len);
}

/* c::free - Free memory allocated by C (e.g., from c::str_to_ptr) */
StradaValue* strada_c_free(StradaValue *ptr_sv) {
    if (!ptr_sv) return strada_new_undef();
    void *p = (void*)strada_to_int(ptr_sv);
    if (p) free(p);
    return strada_new_undef();
}

/* c::alloc - Allocate memory (malloc wrapper) */
StradaValue* strada_c_alloc(StradaValue *size_sv) {
    if (!size_sv) return strada_new_int(0);
    size_t size = (size_t)strada_to_int(size_sv);
    void *p = malloc(size);
    return strada_new_int((int64_t)p);
}

/* c::realloc - Reallocate memory */
StradaValue* strada_c_realloc(StradaValue *ptr_sv, StradaValue *size_sv) {
    if (!ptr_sv || !size_sv) return strada_new_int(0);
    void *p = (void*)strada_to_int(ptr_sv);
    size_t size = (size_t)strada_to_int(size_sv);
    void *new_p = realloc(p, size);
    return strada_new_int((int64_t)new_p);
}

/* c::null - Return NULL pointer */
StradaValue* strada_c_null(void) {
    return strada_new_int(0);
}

/* c::is_null - Check if pointer is NULL */
StradaValue* strada_c_is_null(StradaValue *ptr_sv) {
    if (!ptr_sv) return strada_new_int(1);
    void *p = (void*)strada_to_int(ptr_sv);
    return strada_new_int(p == NULL ? 1 : 0);
}

/* c::ptr_add - Pointer arithmetic */
StradaValue* strada_c_ptr_add(StradaValue *ptr_sv, StradaValue *offset_sv) {
    if (!ptr_sv || !offset_sv) return strada_new_int(0);
    char *p = (char*)strada_to_int(ptr_sv);
    int64_t offset = strada_to_int(offset_sv);
    return strada_new_int((int64_t)(p + offset));
}

/* c::read_int8 - Read int8_t at pointer */
StradaValue* strada_c_read_int8(StradaValue *ptr_sv) {
    if (!ptr_sv) return strada_new_int(0);
    int8_t *p = (int8_t*)strada_to_int(ptr_sv);
    if (!p) return strada_new_int(0);
    return strada_new_int(*p);
}

/* c::read_int16 - Read int16_t at pointer */
StradaValue* strada_c_read_int16(StradaValue *ptr_sv) {
    if (!ptr_sv) return strada_new_int(0);
    int16_t *p = (int16_t*)strada_to_int(ptr_sv);
    if (!p) return strada_new_int(0);
    return strada_new_int(*p);
}

/* c::read_int32 - Read int32_t at pointer */
StradaValue* strada_c_read_int32(StradaValue *ptr_sv) {
    if (!ptr_sv) return strada_new_int(0);
    int32_t *p = (int32_t*)strada_to_int(ptr_sv);
    if (!p) return strada_new_int(0);
    return strada_new_int(*p);
}

/* c::read_int64 - Read int64_t at pointer */
StradaValue* strada_c_read_int64(StradaValue *ptr_sv) {
    if (!ptr_sv) return strada_new_int(0);
    int64_t *p = (int64_t*)strada_to_int(ptr_sv);
    if (!p) return strada_new_int(0);
    return strada_new_int(*p);
}

/* c::read_ptr - Read pointer at pointer (void**) */
StradaValue* strada_c_read_ptr(StradaValue *ptr_sv) {
    if (!ptr_sv) return strada_new_int(0);
    void **p = (void**)strada_to_int(ptr_sv);
    if (!p) return strada_new_int(0);
    return strada_new_int((int64_t)*p);
}

/* c::read_float - Read float at pointer */
StradaValue* strada_c_read_float(StradaValue *ptr_sv) {
    if (!ptr_sv) return strada_new_num(0.0);
    float *p = (float*)strada_to_int(ptr_sv);
    if (!p) return strada_new_num(0.0);
    return strada_new_num((double)*p);
}

/* c::read_double - Read double at pointer */
StradaValue* strada_c_read_double(StradaValue *ptr_sv) {
    if (!ptr_sv) return strada_new_num(0.0);
    double *p = (double*)strada_to_int(ptr_sv);
    if (!p) return strada_new_num(0.0);
    return strada_new_num(*p);
}

/* c::write_int8 - Write int8_t at pointer */
StradaValue* strada_c_write_int8(StradaValue *ptr_sv, StradaValue *val_sv) {
    if (!ptr_sv || !val_sv) return strada_new_undef();
    int8_t *p = (int8_t*)strada_to_int(ptr_sv);
    if (!p) return strada_new_undef();
    *p = (int8_t)strada_to_int(val_sv);
    return strada_new_undef();
}

/* c::write_int16 - Write int16_t at pointer */
StradaValue* strada_c_write_int16(StradaValue *ptr_sv, StradaValue *val_sv) {
    if (!ptr_sv || !val_sv) return strada_new_undef();
    int16_t *p = (int16_t*)strada_to_int(ptr_sv);
    if (!p) return strada_new_undef();
    *p = (int16_t)strada_to_int(val_sv);
    return strada_new_undef();
}

/* c::write_int32 - Write int32_t at pointer */
StradaValue* strada_c_write_int32(StradaValue *ptr_sv, StradaValue *val_sv) {
    if (!ptr_sv || !val_sv) return strada_new_undef();
    int32_t *p = (int32_t*)strada_to_int(ptr_sv);
    if (!p) return strada_new_undef();
    *p = (int32_t)strada_to_int(val_sv);
    return strada_new_undef();
}

/* c::write_int64 - Write int64_t at pointer */
StradaValue* strada_c_write_int64(StradaValue *ptr_sv, StradaValue *val_sv) {
    if (!ptr_sv || !val_sv) return strada_new_undef();
    int64_t *p = (int64_t*)strada_to_int(ptr_sv);
    if (!p) return strada_new_undef();
    *p = strada_to_int(val_sv);
    return strada_new_undef();
}

/* c::write_ptr - Write pointer at pointer (void**) */
StradaValue* strada_c_write_ptr(StradaValue *ptr_sv, StradaValue *val_sv) {
    if (!ptr_sv || !val_sv) return strada_new_undef();
    void **p = (void**)strada_to_int(ptr_sv);
    if (!p) return strada_new_undef();
    *p = (void*)strada_to_int(val_sv);
    return strada_new_undef();
}

/* c::write_float - Write float at pointer */
StradaValue* strada_c_write_float(StradaValue *ptr_sv, StradaValue *val_sv) {
    if (!ptr_sv || !val_sv) return strada_new_undef();
    float *p = (float*)strada_to_int(ptr_sv);
    if (!p) return strada_new_undef();
    *p = (float)strada_to_num(val_sv);
    return strada_new_undef();
}

/* c::write_double - Write double at pointer */
StradaValue* strada_c_write_double(StradaValue *ptr_sv, StradaValue *val_sv) {
    if (!ptr_sv || !val_sv) return strada_new_undef();
    double *p = (double*)strada_to_int(ptr_sv);
    if (!p) return strada_new_undef();
    *p = strada_to_num(val_sv);
    return strada_new_undef();
}

/* c::sizeof_int - Return sizeof(int) */
StradaValue* strada_c_sizeof_int(void) {
    return strada_new_int(sizeof(int));
}

/* c::sizeof_long - Return sizeof(long) */
StradaValue* strada_c_sizeof_long(void) {
    return strada_new_int(sizeof(long));
}

/* c::sizeof_ptr - Return sizeof(void*) */
StradaValue* strada_c_sizeof_ptr(void) {
    return strada_new_int(sizeof(void*));
}

/* c::sizeof_size_t - Return sizeof(size_t) */
StradaValue* strada_c_sizeof_size_t(void) {
    return strada_new_int(sizeof(size_t));
}

/* c::memcpy - Copy memory */
StradaValue* strada_c_memcpy(StradaValue *dest_sv, StradaValue *src_sv, StradaValue *n_sv) {
    if (!dest_sv || !src_sv || !n_sv) return strada_new_int(0);
    void *dest = (void*)strada_to_int(dest_sv);
    void *src = (void*)strada_to_int(src_sv);
    size_t n = (size_t)strada_to_int(n_sv);
    if (!dest || !src) return strada_new_int(0);
    memcpy(dest, src, n);
    strada_incref(dest_sv);
    return dest_sv;
}

/* c::memset - Set memory */
StradaValue* strada_c_memset(StradaValue *dest_sv, StradaValue *c_sv, StradaValue *n_sv) {
    if (!dest_sv || !c_sv || !n_sv) return strada_new_int(0);
    void *dest = (void*)strada_to_int(dest_sv);
    int c = (int)strada_to_int(c_sv);
    size_t n = (size_t)strada_to_int(n_sv);
    if (!dest) return strada_new_int(0);
    memset(dest, c, n);
    strada_incref(dest_sv);
    return dest_sv;
}

/* ============================================================
 * String Repetition (x operator): "ab" x 3 → "ababab"
 * ============================================================ */
StradaValue* strada_string_repeat(StradaValue *sv, int64_t count) {
    if (!sv || count <= 0) return strada_new_str("");
    char _tb[256];
    const char *str = strada_to_str_buf(sv, _tb, sizeof(_tb));
    size_t len = strlen(str);
    if (len == 0 || count == 0) { return strada_new_str(""); }
    if ((size_t)count > SIZE_MAX / len) { return strada_new_str(""); }  /* overflow guard */
    size_t total = len * (size_t)count;
    char *buf = malloc(total + 1);
    if (!buf) { return strada_new_str(""); }
    for (int64_t i = 0; i < count; i++) {
        memcpy(buf + i * len, str, len);
    }
    buf[total] = '\0';
    return strada_new_str_take(buf);
}

/* ============================================================
 * Array splice: splice(@arr, offset, length, replacement)
 * ============================================================ */
StradaValue* strada_array_splice_sv(StradaValue *arr_sv, int64_t offset, int64_t length, StradaValue *repl_sv) {
    if (!arr_sv) return strada_new_array();
    StradaArray *av;
    if (arr_sv->type == STRADA_REF && arr_sv->value.rv && arr_sv->value.rv->type == STRADA_ARRAY) {
        av = arr_sv->value.rv->value.av;
    } else if (arr_sv->type == STRADA_ARRAY) {
        av = arr_sv->value.av;
    } else {
        return strada_new_array();
    }
    int64_t size = (int64_t)av->size;

    /* Handle negative offset */
    if (offset < 0) offset = size + offset;
    if (offset < 0) offset = 0;
    if (offset > size) offset = size;

    /* Handle length == -1 (to end) or out of range */
    if (length < 0 || offset + length > size) length = size - offset;

    /* Compact head before splice to simplify offset calculations */
    if (av->head > 0) {
        memmove(av->elements, av->elements + av->head, av->size * sizeof(StradaValue*));
        av->head = 0;
    }

    /* Build result array of removed elements */
    StradaValue *result = strada_new_array();
    StradaArray *result_av = result->value.av;
    for (int64_t i = 0; i < length; i++) {
        StradaValue *elem = av->elements[offset + i];
        strada_array_push(result_av, elem);
    }

    /* Get replacement elements */
    StradaArray *repl_av = NULL;
    int64_t repl_count = 0;
    if (repl_sv && repl_sv->type == STRADA_REF && repl_sv->value.rv &&
        repl_sv->value.rv->type == STRADA_ARRAY) {
        repl_av = repl_sv->value.rv->value.av;
        repl_count = (int64_t)repl_av->size;
    } else if (repl_sv && repl_sv->type == STRADA_ARRAY) {
        repl_av = repl_sv->value.av;
        repl_count = (int64_t)repl_av->size;
    }

    /* Calculate new size */
    int64_t new_size = size - length + repl_count;
    int64_t diff = repl_count - length;

    /* Decref removed elements */
    for (int64_t i = 0; i < length; i++) {
        strada_decref(av->elements[offset + i]);
    }

    /* Shift elements */
    if (diff != 0) {
        /* Ensure capacity */
        if (new_size > (int64_t)av->capacity) {
            size_t new_cap = (size_t)new_size * 2;
            av->elements = realloc(av->elements, new_cap * sizeof(StradaValue*));
            av->capacity = new_cap;
        }
        /* Move tail elements */
        memmove(&av->elements[offset + repl_count],
                &av->elements[offset + length],
                (size - offset - length) * sizeof(StradaValue*));
    }

    /* Insert replacement elements */
    for (int64_t i = 0; i < repl_count; i++) {
        StradaValue *elem = repl_av->elements[repl_av->head + i];
        strada_incref(elem);
        av->elements[offset + i] = elem;
    }
    av->size = (size_t)new_size;

    return result;
}

/* ============================================================
 * Hash each() iterator
 * ============================================================ */
void strada_hash_reset_iter(StradaHash *hv) {
    if (!hv) return;
    hv->iter_bucket = 0;
    hv->iter_entry = NULL;
}

StradaValue* strada_hash_each(StradaHash *hv) {
    if (!hv) return strada_new_array();

    /* If we have a current entry, advance to next in chain */
    if (hv->iter_entry) {
        hv->iter_entry = hv->iter_entry->next;
        if (!hv->iter_entry) {
            /* Moved past end of chain, go to next bucket */
            hv->iter_bucket++;
        }
    }

    /* Find next non-empty bucket */
    while (!hv->iter_entry && hv->iter_bucket < hv->num_buckets) {
        hv->iter_entry = hv->buckets[hv->iter_bucket];
        if (!hv->iter_entry) {
            hv->iter_bucket++;
        }
    }

    if (!hv->iter_entry) {
        /* No more entries - reset iterator and return empty array */
        hv->iter_bucket = 0;
        hv->iter_entry = NULL;
        return strada_new_array();
    }

    /* Build [key, value] pair */
    StradaValue *pair = strada_new_array();
    StradaArray *pair_av = pair->value.av;
    StradaValue *key = strada_new_str(hv->iter_entry->key);
    strada_array_push(pair_av, key);
    strada_decref(key);
    StradaValue *val = hv->iter_entry->value;
    strada_incref(val);
    strada_array_push(pair_av, val);
    strada_decref(val);

    /* Pre-advance: move to next entry for the next call */
    /* The actual advancement happens at the top of next call */

    return pair;
}

/* ============================================================
 * select() - Default output filehandle
 * ============================================================ */
StradaValue* strada_select(StradaValue *fh) {
    StradaValue *prev = strada_default_output;
    if (fh && (fh->type == STRADA_FILEHANDLE || fh->type == STRADA_SOCKET)) {
        strada_incref(fh);
        strada_default_output = fh;
    } else {
        strada_default_output = NULL;
    }
    if (prev) return prev;
    return strada_new_undef();
}

StradaValue* strada_select_get(void) {
    if (strada_default_output) {
        strada_incref(strada_default_output);
        return strada_default_output;
    }
    return strada_new_undef();
}

/* Modified print/say to use default output */
/* Note: the original strada_print/strada_say already exist above.
 * We patch them here by adding the default output check. */

/* ============================================================
 * Transliteration (tr///)
 * ============================================================ */
StradaValue* strada_tr(StradaValue *sv, const char *search, const char *replace, const char *flags) {
    if (!sv || !search) return strada_new_int(0);
    char *str = strada_to_str(sv);
    size_t len = strlen(str);

    /* Parse flags */
    int complement = 0, delete_flag = 0, squeeze = 0;
    if (flags) {
        for (const char *f = flags; *f; f++) {
            if (*f == 'c') complement = 1;
            else if (*f == 'd') delete_flag = 1;
            else if (*f == 's') squeeze = 1;
        }
    }

    /* Build search and replace lists (expand ranges like a-z) */
    unsigned char search_chars[256];
    unsigned char replace_chars[256];
    int search_count = 0, replace_count = 0;

    /* Expand ranges in search */
    for (const char *p = search; *p; p++) {
        if (*(p+1) == '-' && *(p+2)) {
            unsigned char from = (unsigned char)*p;
            unsigned char to = (unsigned char)*(p+2);
            if (from <= to) {
                for (unsigned int c = from; c <= to; c++) {
                    if (search_count < 256) search_chars[search_count++] = (unsigned char)c;
                }
            } else {
                for (unsigned int c = from; c >= to; c--) {
                    if (search_count < 256) search_chars[search_count++] = (unsigned char)c;
                }
            }
            p += 2;
        } else {
            if (search_count < 256) search_chars[search_count++] = (unsigned char)*p;
        }
    }

    /* Expand ranges in replace */
    for (const char *p = replace; *p; p++) {
        if (*(p+1) == '-' && *(p+2)) {
            unsigned char from = (unsigned char)*p;
            unsigned char to = (unsigned char)*(p+2);
            if (from <= to) {
                for (unsigned int c = from; c <= to; c++) {
                    if (replace_count < 256) replace_chars[replace_count++] = (unsigned char)c;
                }
            } else {
                for (unsigned int c = from; c >= to; c--) {
                    if (replace_count < 256) replace_chars[replace_count++] = (unsigned char)c;
                }
            }
            p += 2;
        } else {
            if (replace_count < 256) replace_chars[replace_count++] = (unsigned char)*p;
        }
    }

    /* Build translation table */
    int trans[256];
    for (int i = 0; i < 256; i++) trans[i] = -1;  /* -1 = not in search set */

    if (!complement) {
        for (int i = 0; i < search_count; i++) {
            if (delete_flag && i >= replace_count) {
                trans[search_chars[i]] = -2;  /* -2 = delete */
            } else if (i < replace_count) {
                trans[search_chars[i]] = replace_chars[i];
            } else if (replace_count > 0) {
                /* If replace list is shorter, use last replace char */
                trans[search_chars[i]] = replace_chars[replace_count - 1];
            }
        }
    } else {
        /* Complement: translate chars NOT in search set */
        int in_search[256] = {0};
        for (int i = 0; i < search_count; i++) in_search[search_chars[i]] = 1;
        int rep_idx = 0;
        for (int i = 0; i < 256; i++) {
            if (!in_search[i]) {
                if (delete_flag && rep_idx >= replace_count) {
                    trans[i] = -2;
                } else if (rep_idx < replace_count) {
                    trans[i] = replace_chars[rep_idx++];
                } else if (replace_count > 0) {
                    trans[i] = replace_chars[replace_count - 1];
                }
            }
        }
    }

    /* Apply translation */
    char *result = malloc(len + 1);
    size_t out_idx = 0;
    int count = 0;
    int last_char = -1;

    for (size_t i = 0; i < len; i++) {
        unsigned char c = (unsigned char)str[i];
        int t = trans[c];
        if (t == -2) {
            /* Delete this character */
            count++;
        } else if (t >= 0) {
            /* Translate */
            count++;
            if (squeeze && (unsigned char)t == (unsigned char)last_char) {
                /* Squeeze: skip duplicate */
            } else {
                result[out_idx++] = (char)t;
                last_char = t;
            }
        } else {
            /* Not in translation set - keep as-is */
            result[out_idx++] = (char)c;
            last_char = -1;
        }
    }
    result[out_idx] = '\0';

    /* Modify the original string in-place */
    if (sv->type == STRADA_STR && sv->value.pv) {
        free(sv->value.pv);
        sv->value.pv = result;
        sv->struct_size = out_idx;
    } else {
        free(result);
    }

    free(str);
    return strada_new_int(count);
}

/* ============================================================
 * local() - Dynamic scoping for our variables
 * ============================================================ */
#ifdef STRADA_NO_TLS
static StradaLocalSave strada_local_stack[STRADA_LOCAL_STACK_MAX];
static int strada_local_depth = 0;
#else
static __thread StradaLocalSave strada_local_stack[STRADA_LOCAL_STACK_MAX];
static __thread int strada_local_depth = 0;
#endif

void strada_local_save(const char *name) {
    if (strada_local_depth >= STRADA_LOCAL_STACK_MAX) {
        strada_die("local() stack overflow (max %d)", STRADA_LOCAL_STACK_MAX);
    }
    StradaLocalSave *save = &strada_local_stack[strada_local_depth++];
    save->name = strdup(name);
    /* Get current value from global registry - returns owned reference */
    StradaValue *name_sv = strada_new_str(name);
    save->saved_value = strada_global_get(name_sv);
    /* global_get consumed name_sv and returned an owned ref */
}

void strada_local_restore(void) {
    if (strada_local_depth <= 0) return;
    strada_local_depth--;
    StradaLocalSave *save = &strada_local_stack[strada_local_depth];
    /* Restore the old value - global_set consumes name_sv and decrefs val */
    /* hash_set increfs val for registry, global_set decrefs val releasing our ref */
    StradaValue *name_sv = strada_new_str(save->name);
    strada_global_set(name_sv, save->saved_value);
    /* Don't decref save->saved_value again - global_set already released our ref */
    free(save->name);
    save->name = NULL;
    save->saved_value = NULL;
}

void strada_local_restore_n(int n) {
    for (int i = 0; i < n; i++) {
        strada_local_restore();
    }
}

int strada_local_depth_get(void) {
    return strada_local_depth;
}

void strada_local_restore_to(int depth) {
    while (strada_local_depth > depth) {
        strada_local_restore();
    }
}

/* ============================================================
 * tie/untie/tied - Tied variable support
 * ============================================================ */

/* Helper: call a method on the tied object */
static StradaValue* strada_tied_method_call(StradaValue *tied_obj, const char *method, int argc, ...) {
    StradaValue *args_arr = strada_new_array();
    StradaArray *av = args_arr->value.av;

    va_list ap;
    va_start(ap, argc);
    for (int i = 0; i < argc; i++) {
        StradaValue *arg = va_arg(ap, StradaValue*);
        strada_array_push(av, arg);
    }
    va_end(ap);

    /* strada_method_call consumes (decrefs) args_arr, so don't decref again */
    StradaValue *result = strada_method_call(tied_obj, method, args_arr);
    return result;
}

StradaValue* strada_tied_hash_fetch(StradaValue *sv, const char *key) {
    if (!sv || !SV_TIED_OBJ(sv)) return strada_new_undef();
    StradaValue *key_sv = strada_new_str(key);
    StradaValue *result = strada_tied_method_call(sv->meta->tied_obj, "FETCH", 1, key_sv);
    strada_decref(key_sv);
    return result;
}

void strada_tied_hash_store(StradaValue *sv, const char *key, StradaValue *val) {
    if (!sv || !SV_TIED_OBJ(sv)) return;
    StradaValue *key_sv = strada_new_str(key);
    StradaValue *result = strada_tied_method_call(sv->meta->tied_obj, "STORE", 2, key_sv, val);
    strada_decref(key_sv);
    if (result) strada_decref(result);
}

int strada_tied_hash_exists(StradaValue *sv, const char *key) {
    if (!sv || !SV_TIED_OBJ(sv)) return 0;
    StradaValue *key_sv = strada_new_str(key);
    StradaValue *result = strada_tied_method_call(sv->meta->tied_obj, "EXISTS", 1, key_sv);
    strada_decref(key_sv);
    int ret = strada_to_bool(result);
    if (result) strada_decref(result);
    return ret;
}

void strada_tied_hash_delete(StradaValue *sv, const char *key) {
    if (!sv || !SV_TIED_OBJ(sv)) return;
    StradaValue *key_sv = strada_new_str(key);
    StradaValue *result = strada_tied_method_call(sv->meta->tied_obj, "DELETE", 1, key_sv);
    strada_decref(key_sv);
    if (result) strada_decref(result);
}

StradaValue* strada_tied_hash_firstkey(StradaValue *sv) {
    if (!sv || !SV_TIED_OBJ(sv)) return strada_new_undef();
    return strada_tied_method_call(sv->meta->tied_obj, "FIRSTKEY", 0);
}

StradaValue* strada_tied_hash_nextkey(StradaValue *sv, const char *lastkey) {
    if (!sv || !SV_TIED_OBJ(sv)) return strada_new_undef();
    StradaValue *key_sv = strada_new_str(lastkey);
    StradaValue *result = strada_tied_method_call(sv->meta->tied_obj, "NEXTKEY", 1, key_sv);
    strada_decref(key_sv);
    return result;
}

void strada_tied_hash_clear(StradaValue *sv) {
    if (!sv || !SV_TIED_OBJ(sv)) return;
    StradaValue *result = strada_tied_method_call(sv->meta->tied_obj, "CLEAR", 0);
    if (result) strada_decref(result);
}

/* tie(%hash, "ClassName", @args) */
StradaValue* strada_tie_hash(StradaValue *ref, const char *classname, int argc, ...) {
    if (!ref) return strada_new_undef();
    /* Build args array */
    StradaValue *args_arr = strada_new_array();
    StradaArray *av = args_arr->value.av;
    va_list ap;
    va_start(ap, argc);
    for (int i = 0; i < argc; i++) {
        StradaValue *arg = va_arg(ap, StradaValue*);
        strada_array_push(av, arg);
    }
    va_end(ap);

    /* Call ClassName_TIEHASH(args) directly via dlsym */
    char funcname[256];
    snprintf(funcname, sizeof(funcname), "%s_TIEHASH", classname);
    typedef StradaValue* (*TieHashFunc)(StradaValue*);
    void *handle = dlopen(NULL, RTLD_LAZY);
    TieHashFunc func = (TieHashFunc)dlsym(handle, funcname);
    if (!func) {
        fprintf(stderr, "Error: No TIEHASH method found in package '%s'\n", classname);
        strada_decref(args_arr);
        if (handle) dlclose(handle);
        return strada_new_undef();
    }
    StradaValue *tied_obj = func(args_arr);
    strada_decref(args_arr);
    if (handle) dlclose(handle);

    if (!tied_obj || tied_obj->type == STRADA_UNDEF) {
        if (tied_obj) strada_decref(tied_obj);
        return strada_new_undef();
    }

    /* Set tied on the ref target */
    StradaValue *target = ref;
    if (ref->type == STRADA_REF && ref->value.rv) {
        target = ref->value.rv;
    }
    strada_ensure_meta(target)->is_tied = 1;
    target->meta->tied_obj = tied_obj;  /* takes one reference */
    strada_incref(tied_obj);      /* caller gets another reference */

    return tied_obj;
}

StradaValue* strada_tie_array(StradaValue *ref, const char *classname, int argc, ...) {
    if (!ref) return strada_new_undef();
    StradaValue *args_arr = strada_new_array();
    StradaArray *av = args_arr->value.av;
    va_list ap;
    va_start(ap, argc);
    for (int i = 0; i < argc; i++) {
        StradaValue *arg = va_arg(ap, StradaValue*);
        strada_array_push(av, arg);
    }
    va_end(ap);
    StradaValue *class_sv = strada_new_str(classname);
    StradaValue *tied_obj = strada_method_call(class_sv, "TIEARRAY", args_arr);
    strada_decref(class_sv);
    if (!tied_obj || tied_obj->type == STRADA_UNDEF) {
        if (tied_obj) strada_decref(tied_obj);
        return strada_new_undef();
    }
    StradaValue *target = ref;
    if (ref->type == STRADA_REF && ref->value.rv) target = ref->value.rv;
    strada_ensure_meta(target)->is_tied = 1;
    target->meta->tied_obj = tied_obj;
    strada_incref(tied_obj);      /* caller gets another reference */
    return tied_obj;
}

StradaValue* strada_tie_scalar(StradaValue *ref, const char *classname, int argc, ...) {
    if (!ref) return strada_new_undef();
    StradaValue *args_arr = strada_new_array();
    StradaArray *av = args_arr->value.av;
    va_list ap;
    va_start(ap, argc);
    for (int i = 0; i < argc; i++) {
        StradaValue *arg = va_arg(ap, StradaValue*);
        strada_array_push(av, arg);
    }
    va_end(ap);
    StradaValue *class_sv = strada_new_str(classname);
    StradaValue *tied_obj = strada_method_call(class_sv, "TIESCALAR", args_arr);
    strada_decref(class_sv);
    if (!tied_obj || tied_obj->type == STRADA_UNDEF) {
        if (tied_obj) strada_decref(tied_obj);
        return strada_new_undef();
    }
    StradaValue *target = ref;
    if (ref->type == STRADA_REF && ref->value.rv) target = ref->value.rv;
    strada_ensure_meta(target)->is_tied = 1;
    target->meta->tied_obj = tied_obj;
    strada_incref(tied_obj);      /* caller gets another reference */
    return tied_obj;
}

void strada_untie(StradaValue *ref) {
    if (!ref) return;
    StradaValue *target = ref;
    if (ref->type == STRADA_REF && ref->value.rv) target = ref->value.rv;
    if (!SV_IS_TIED(target)) return;

    /* Call UNTIE if it exists */
    if (target->meta->tied_obj && strada_can(target->meta->tied_obj, "UNTIE")) {
        StradaValue *args = strada_new_array();
        StradaValue *result = strada_method_call(target->meta->tied_obj, "UNTIE", args);
        if (result) strada_decref(result);
    }

    if (target->meta->tied_obj) {
        strada_decref(target->meta->tied_obj);
        target->meta->tied_obj = NULL;
    }
    target->meta->is_tied = 0;
}

StradaValue* strada_tied(StradaValue *ref) {
    if (!ref) return strada_new_undef();
    StradaValue *target = ref;
    if (ref->type == STRADA_REF && ref->value.rv) target = ref->value.rv;
    if (SV_IS_TIED(target) && SV_TIED_OBJ(target)) {
        strada_incref(target->meta->tied_obj);
        return target->meta->tied_obj;
    }
    return strada_new_undef();
}


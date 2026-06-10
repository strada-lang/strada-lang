/* strada_runtime.h - Strada Runtime System */
#ifndef STRADA_RUNTIME_H
#define STRADA_RUNTIME_H

/* Feature test macros - must be defined before any system headers */
#ifndef _POSIX_C_SOURCE
#define _POSIX_C_SOURCE 200809L
#endif
#ifndef _DEFAULT_SOURCE
#define _DEFAULT_SOURCE
#endif
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#ifdef __APPLE__
#ifndef _DARWIN_C_SOURCE
#define _DARWIN_C_SOURCE
#endif
#endif

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <stdarg.h>
#include <setjmp.h>
#ifdef HAVE_PCRE2
#define PCRE2_CODE_UNIT_WIDTH 8
#include <pcre2.h>
#else
#include <regex.h>
#endif
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <unistd.h>
#include <netdb.h>
#include <dlfcn.h>
#include <execinfo.h>
#include <ctype.h>
#include <pthread.h>
#include <termios.h>
#include <fcntl.h>
#include <time.h>

/* Forward declarations */
typedef struct StradaValue StradaValue;
typedef struct StradaArray StradaArray;
typedef struct StradaHash StradaHash;

/* Type enumeration */
typedef enum {
    STRADA_UNDEF,
    STRADA_INT,
    STRADA_NUM,
    STRADA_STR,
    STRADA_ARRAY,
    STRADA_HASH,
    STRADA_REF,
    STRADA_FILEHANDLE,
    STRADA_REGEX,
    STRADA_SOCKET,
    STRADA_CSTRUCT,    /* C struct wrapper */
    STRADA_CPOINTER,   /* Generic C pointer */
    STRADA_CLOSURE,    /* Anonymous function with captured environment */
    STRADA_FUTURE,     /* Async future for await */
    STRADA_CHANNEL,    /* Thread-safe communication channel */
    STRADA_ATOMIC      /* Atomic integer for lock-free operations */
} StradaType;

/* ===== TAGGED INTEGER SUPPORT =====
 * Integers are encoded directly in the StradaValue* pointer:
 *   bit 0 = 1 → tagged integer, upper bits = sign-extended value
 *   bit 0 = 0 → normal heap pointer (always aligned to >= 2 bytes)
 * On 64-bit: bits 1-63 = value, range -(2^62) to (2^62-1)
 * On 32-bit: bits 1-31 = value, range -(2^30) to (2^30-1)
 * Values outside the tagged range fall back to heap allocation.
 * Tagged ints are immortal (no allocation, no refcounting). */

/* Auto-detect pointer size if not provided by build system */
#ifndef STRADA_POINTER_SIZE
#if UINTPTR_MAX == 0xFFFFFFFF
#define STRADA_POINTER_SIZE 4
#else
#define STRADA_POINTER_SIZE 8
#endif
#endif

#define STRADA_IS_TAGGED_INT(sv) ((uintptr_t)(sv) & 1ULL)
#define STRADA_TAGGED_INT_VAL(sv) ((int64_t)((intptr_t)(sv) >> 1))
#define STRADA_MAKE_TAGGED_INT(val) ((StradaValue*)(((uintptr_t)(int64_t)(val) << 1) | 1ULL))

/* Perl-style (floored) integer modulo: the result takes the sign of the RIGHT
 * operand, unlike C's `%` which takes the sign of the left. So -7 %% 3 == 2 and
 * 7 %% -3 == -2. Caller must ensure b != 0. Used by both the runtime
 * (strada_safe_mod) and the codegen's inlined tagged-int modulo fast path. */
static inline int64_t strada_floored_mod(int64_t a, int64_t b) {
    int64_t r = a % b;
    if (r != 0 && ((r < 0) != (b < 0))) r += b;
    return r;
}

/* ASCII string flag: bit 63 of struct_size. When set, the string is pure ASCII
 * and byte offset == char offset, making substr() O(1). */
#define STRADA_ASCII_FLAG ((size_t)1 << 63)
/* UTF-8 char-oriented flag: bit 62. When set, the string is a Perl-style
 * UTF-8 decoded string — length/substr/index operate on characters (counting
 * UTF-8 code points), not bytes. Mirrors Perl's SVf_UTF8.
 *   - Set by: Encode::decode("UTF-8", ...), chr(N) for N > 127, utf8::decode,
 *             utf8::upgrade, string ops that propagate flag from either operand,
 *             string literals from a `use utf8` source file with non-ASCII chars.
 *   - Cleared by: utf8::encode, utf8::downgrade (when downgrade is possible),
 *                 explicit byte-level operations (pack, read from binary FH).
 * ASCII-flagged strings are implicitly char-oriented too (every byte is one
 * codepoint); when both flags are 0 the string is byte-oriented Latin-1ish. */
#define STRADA_UTF8_FLAG ((size_t)1 << 62)
#define STRADA_STR_FLAGS_MASK (STRADA_ASCII_FLAG | STRADA_UTF8_FLAG)
#define STRADA_STR_BYTELEN(sv) ((sv)->struct_size & ~STRADA_STR_FLAGS_MASK)
#define STRADA_STR_IS_ASCII(sv) (((sv)->struct_size & STRADA_ASCII_FLAG) != 0)
#define STRADA_STR_IS_UTF8(sv) (((sv)->struct_size & STRADA_UTF8_FLAG) != 0)
/* True if either flag implies "treat as chars" (ASCII or UTF-8). */
#define STRADA_STR_IS_CHARS(sv) (((sv)->struct_size & STRADA_STR_FLAGS_MASK) != 0)

/* Unsigned-integer flag: bit 61 of struct_size, set ONLY on heap STRADA_INT
 * values whose magnitude exceeds INT64_MAX (i.e. live in (2^63, 2^64-1]).
 * value.iv holds the two's-complement bit pattern of the uint64_t. Mirrors
 * Perl's SVf_IVisUV: the value is still type STRADA_INT (so every existing
 * `type == STRADA_INT` check keeps working), but stringify / numify / compare
 * reinterpret value.iv as uint64_t. Never set on tagged ints (those are always
 * in signed tagged range) — only heap ints reach this. struct_size is otherwise
 * unused for INT, so this never collides with the string-length/flags use. */
#define STRADA_UV_FLAG ((size_t)1 << 61)
#define STRADA_INT_IS_UV(sv) (!STRADA_IS_TAGGED_INT(sv) && (sv) && (sv)->type == STRADA_INT && (((sv)->struct_size & STRADA_UV_FLAG) != 0))

#if STRADA_POINTER_SIZE == 4
#define STRADA_TAGGED_INT_BITS 30
#define STRADA_TAGGED_INT_MIN (-(INT64_C(1) << 30))
#define STRADA_TAGGED_INT_MAX ((INT64_C(1) << 30) - 1)
#else
#define STRADA_TAGGED_INT_BITS 62
#define STRADA_TAGGED_INT_MIN (-(INT64_C(1) << 62))
#define STRADA_TAGGED_INT_MAX ((INT64_C(1) << 62) - 1)
#endif

/* Closure structure */
typedef struct StradaClosure {
    void *func_ptr;           /* Pointer to generated C function */
    int param_count;          /* Number of parameters */
    int capture_count;        /* Number of captured variables */
    StradaValue ***captures;  /* Array of pointers to pointers (capture-by-reference) */
    unsigned char *capture_is_static; /* NULL or array of capture_count bytes; 1 = cell points to a static slot, do not free(cap) */
    char *prototype;          /* Perl prototype string (e.g. "$", "\\@") or NULL.
                                 Set by perla codegen when `sub (PROTO) { ... }`
                                 is used; consumed by Scalar::Util / prototype(\&). */
} StradaClosure;

/* Thread types - store pthread handles as opaque pointers */
typedef struct StradaThread {
    pthread_t thread;
    StradaValue *closure;     /* The closure to run */
    StradaValue *result;      /* Return value from thread */
    int detached;             /* 1 if detached (self-cleanup) */
} StradaThread;

typedef struct StradaMutex {
    pthread_mutex_t mutex;
} StradaMutex;

typedef struct StradaCond {
    pthread_cond_t cond;
} StradaCond;

/* ============================================================
 * Async/Await Support - Thread Pool Model
 * ============================================================ */

/* Forward declarations */
typedef struct StradaFuture StradaFuture;
typedef struct StradaTask StradaTask;
typedef struct StradaThreadPool StradaThreadPool;

/* Task for thread pool queue */
struct StradaTask {
    StradaValue *closure;           /* Closure to execute */
    StradaFuture *future;           /* Associated future */
    struct StradaTask *next;        /* Next task in queue */
};

/* Thread pool for async operations */
struct StradaThreadPool {
    pthread_t *workers;             /* Worker threads */
    int worker_count;               /* Number of workers */
    int running;                    /* 1 if pool is active */

    /* Task queue */
    StradaTask *queue_head;
    StradaTask *queue_tail;
    int queue_size;

    /* Synchronization */
    pthread_mutex_t queue_mutex;
    pthread_cond_t queue_cond;      /* Workers wait on this */
    pthread_cond_t empty_cond;      /* For shutdown wait */
};

/* Future states */
typedef enum {
    FUTURE_PENDING = 0,
    FUTURE_RUNNING = 1,
    FUTURE_COMPLETED = 2,
    FUTURE_CANCELLED = 3,
    FUTURE_TIMEOUT = 4
} StradaFutureState;

/* Future/Promise for async operations */
struct StradaFuture {
    pthread_mutex_t mutex;          /* Protects all fields */
    pthread_cond_t cond;            /* For waiting on completion */

    StradaValue *result;            /* Result value */
    StradaValue *error;             /* Error value (if failed) */
    StradaValue *closure;           /* The closure to run */

    StradaFutureState state;        /* Current state */
    int cancel_requested;           /* 1 if cancellation requested */

    /* For timeout support */
    struct timespec deadline;       /* Absolute deadline (0 = no timeout) */
    int has_deadline;
};

/* Global thread pool */
extern StradaThreadPool *strada_thread_pool;

/* Pool management */
void strada_pool_init(int num_workers);
void strada_pool_shutdown(void);
void strada_pool_submit(StradaFuture *future);

/* Future creation and operations */
StradaValue* strada_future_new(StradaValue *closure);
StradaValue* strada_future_await(StradaValue *future);
StradaValue* strada_future_await_timeout(StradaValue *future, int64_t timeout_ms);
int strada_future_is_done(StradaValue *future);
StradaValue* strada_future_try_get(StradaValue *future);
void strada_future_cancel(StradaValue *future);
int strada_future_is_cancelled(StradaValue *future);

/* Combinators */
StradaValue* strada_future_all(StradaValue *futures_array);
StradaValue* strada_future_race(StradaValue *futures_array);

/* Timeout wrapper for Strada */
StradaValue* strada_async_timeout(StradaValue *future, StradaValue *timeout_ms);

/* ============================================================
 * Channel - Thread-safe Communication
 * ============================================================ */

/* Channel node for linked list queue */
typedef struct StradaChannelNode {
    StradaValue *value;
    struct StradaChannelNode *next;
} StradaChannelNode;

/* Thread-safe channel for inter-thread communication */
typedef struct StradaChannel {
    pthread_mutex_t mutex;          /* Protects all fields */
    pthread_cond_t not_empty;       /* Signaled when items added */
    pthread_cond_t not_full;        /* Signaled when items removed */

    StradaChannelNode *head;        /* Front of queue */
    StradaChannelNode *tail;        /* Back of queue */
    int size;                       /* Current number of items */
    int capacity;                   /* Max items (0 = unbounded) */
    int closed;                     /* 1 if channel is closed */
} StradaChannel;

/* Channel operations */
StradaValue* strada_channel_new(int capacity);
void strada_channel_send(StradaValue *channel, StradaValue *value);
StradaValue* strada_channel_recv(StradaValue *channel);
int strada_channel_try_send(StradaValue *channel, StradaValue *value);
StradaValue* strada_channel_try_recv(StradaValue *channel);
void strada_channel_close(StradaValue *channel);
int strada_channel_is_closed(StradaValue *channel);
int strada_channel_len(StradaValue *channel);
/* async::select / sleep / cancelled / map (concurrency ergonomics) */
StradaValue* strada_channel_select(StradaValue *channels_ref, StradaValue *timeout_ms_sv);
StradaValue* strada_async_sleep(StradaValue *ms_sv);
StradaValue* strada_async_cancelled(void);
StradaValue* strada_async_map(StradaValue *fn, StradaValue *items_ref, StradaValue *workers_sv);
/* thread::tls_* — per-thread named values (freed at thread exit) */
StradaValue* strada_tls_set(StradaValue *name_sv, StradaValue *val);
StradaValue* strada_tls_get(StradaValue *name_sv);
StradaValue* strada_tls_exists(StradaValue *name_sv);
StradaValue* strada_tls_delete(StradaValue *name_sv);


/* ============================================================
 * Atomic - Lock-free Integer Operations
 * ============================================================ */

/* Atomic integer */
typedef struct StradaAtomicValue {
    volatile int64_t value;
} StradaAtomicValue;

/* Atomic operations */
StradaValue* strada_atomic_new(int64_t initial);
int64_t strada_atomic_load(StradaValue *atomic);
void strada_atomic_store(StradaValue *atomic, int64_t value);
int64_t strada_atomic_add(StradaValue *atomic, int64_t delta);
int64_t strada_atomic_sub(StradaValue *atomic, int64_t delta);
int strada_atomic_cas(StradaValue *atomic, int64_t expected, int64_t desired);
int64_t strada_atomic_inc(StradaValue *atomic);
int64_t strada_atomic_dec(StradaValue *atomic);

/* StringBuilder for efficient string building (O(1) amortized append) */
typedef struct StradaStringBuilder {
    char *buffer;       /* Pre-allocated buffer */
    size_t length;      /* Current string length */
    size_t capacity;    /* Total buffer capacity */
} StradaStringBuilder;

/* Buffered socket for efficient I/O */
#define STRADA_SOCKET_BUFSIZE 8192
typedef struct StradaSocketBuffer {
    int fd;                              /* Socket file descriptor */
    /* Read buffer */
    char read_buf[STRADA_SOCKET_BUFSIZE];
    size_t read_pos;                     /* Current position in read buffer */
    size_t read_len;                     /* Amount of valid data in read buffer */
    /* Write buffer */
    char write_buf[STRADA_SOCKET_BUFSIZE];
    size_t write_len;                    /* Amount of data in write buffer */
} StradaSocketBuffer;

/* File handle metadata for special handles (pipes, in-memory I/O) */
typedef enum {
    FH_NORMAL = 0,        /* Regular fopen'd file */
    FH_PIPE = 1,          /* popen'd pipe — needs pclose */
    FH_MEMREAD = 2,       /* fmemopen for reading from string */
    FH_MEMWRITE = 3,      /* open_memstream for writing to string */
    FH_MEMWRITE_REF = 4   /* open_memstream with writeback to StradaValue ref on close */
} StradaFhType;

typedef struct StradaFhMeta {
    FILE *fh;                      /* The FILE* this metadata is for */
    StradaFhType fh_type;          /* Type of handle */
    char *mem_buf;                 /* Buffer for fmemopen/open_memstream */
    size_t mem_size;               /* Size for open_memstream */
    StradaValue *target_ref;       /* For FH_MEMWRITE_REF: the reference to write back to */
    struct StradaFhMeta *next;     /* Linked list */
} StradaFhMeta;

/* Cold metadata — only allocated for OOP, tied, weak, or CSTRUCT values */
typedef struct StradaMetadata {
    char *struct_name;       /* Name of C struct type (CSTRUCT/CPOINTER) */
    char *blessed_package;   /* Package name this ref is blessed into, or NULL */
    StradaValue *tied_obj;   /* Implementation object (NULL when !is_tied) */
    int64_t iv_override;     /* Dual-typed override: when has_iv_override is 1,
                              * strada_to_int/num returns this instead of
                              * parsing the string. Used for $! (errno) and
                              * other Perl dualvars where numification yields
                              * a different value than the string form. */
    uint8_t is_tied;         /* 0 = normal, 1 = tied */
    uint8_t is_weak;         /* 0 = strong reference, 1 = weak reference */
    uint8_t blessed_immortal;/* 1 = blessed_package was set via cached path (skip intern_release) */
    uint8_t has_iv_override; /* 1 = iv_override is valid; 0 = ignore it */
    uint8_t is_hash_locked;  /* Hash::Util::lock_keys flag — 1 = reject
                              * insert of new keys (existing keys still
                              * modifiable). locked_keys (below) holds
                              * the allowed-key set when populated. */
    int32_t regex_pos;       /* Perl pos() value for /g matches; -1 = unset.
                              * Updated by /g matches outside while loops,
                              * read by the pos() builtin and assignable via
                              * `pos($sv) = N`. Cleared on string mutation. */
    void *locked_keys;       /* Pointer to a StradaArray* of allowed keys
                              * when is_hash_locked is set; NULL otherwise.
                              * Void-pointer to keep strada_runtime layer
                              * agnostic to the Hash::Util side-table. */
    StradaValue *glob_scalar_slot; /* The SCALAR slot of the glob this
                              * value represents — i.e. `${*$fh}`. perla
                              * models a filehandle as its own SV (no
                              * separate glob object), so `${*$fh} = X`
                              * stores X here and `${*$fh}` reads it.
                              * File::Temp->new stashes the temp filename
                              * in this slot. NULL until first assigned. */
} StradaMetadata;

/* Main value structure - like Perl's SV (32 bytes) */
struct StradaValue {
    StradaType type;         /* 4B */
    int refcount;            /* 4B */
    union {
        int64_t iv;      /* Integer value */
        double nv;       /* Numeric value */
        char *pv;        /* String value */
        StradaArray *av; /* Array reference */
        StradaHash *hv;  /* Hash reference */
        StradaValue *rv; /* Generic reference */
        FILE *fh;        /* File handle */
#ifdef HAVE_PCRE2
        pcre2_code *pcre2_rx;  /* Compiled PCRE2 regex */
#else
        regex_t *rx;     /* Compiled POSIX regex */
#endif
        StradaSocketBuffer *sock;  /* Buffered socket */
        void *ptr;       /* Generic C pointer */
    } value;                 /* 8B */
    size_t struct_size;      /* 8B - string length cache (hot path) */
    StradaMetadata *meta;    /* 8B - NULL for most values */
};

/* Array structure - like Perl's AV */
struct StradaArray {
    StradaValue **elements;
    size_t size;
    size_t capacity;
    int refcount;
    size_t head;    /* Offset into elements[] where data starts (for O(1) shift) */
};

/* Stack-allocated 1-element array for fast method calls.
 * Usage: STRADA_STACK_ARGS1(args, self_val);
 *        method_func(&args.sv);
 * Avoids heap allocation for the common $self->method() pattern. */
typedef struct {
    StradaValue *elems[1];
    StradaArray av;
    StradaValue sv;
} StradaStackArgs1;

static inline void strada_stack_args1_init(StradaStackArgs1 *sa, StradaValue *elem) {
    sa->elems[0] = elem;
    sa->av.elements = sa->elems;
    sa->av.size = 1;
    sa->av.capacity = 1;
    sa->av.refcount = 2;  /* prevent freeing — StradaArray has no incref/decref API */
    sa->av.head = 0;
    sa->sv.type = STRADA_ARRAY;
    sa->sv.value.av = &sa->av;
    /* Immortal sentinel rather than just "2 to prevent freeing":
     * - strada_incref / strada_decref short-circuit on refcount > 1e9
     *   (see runtime/strada_runtime.c around lines 913 and 1031), so
     *   the stack SV stays at this value no matter how many balanced
     *   incref/decref pairs the callee performs.
     * - More importantly, the survive path in strada_decref calls
     *   cc_possible_root when a non-immortal SV's refcount drops but
     *   stays > 0 — and the cycle collector then buffers a pointer
     *   to this STACK-RESIDENT SV in its cc_roots array. When the
     *   stack frame unwinds the pointer dangles; the cc_atexit final
     *   sweep dereferences it and SIGSEGVs. Setting refcount immortal
     *   from the start makes the immortal check at strada_decref's
     *   top branch out before reaching cc_possible_root, so the stack
     *   SV is never registered with the collector. */
    sa->sv.refcount = 2000000000;
    sa->sv.meta = NULL;
    sa->sv.struct_size = 0;
    /* Match heap-array convention: pushed elements are incref'd so
     * that `my $s = shift @_;` inside the callee doesn't underflow the
     * refcount when the caller's invocant SV is shared. Caller emits
     * a matching strada_decref after the dispatched call returns. */
    if (elem && !STRADA_IS_TAGGED_INT(elem)) elem->refcount++;
}

/* Refcounted string — single allocation for header + data.
 * Used for hash keys; will eventually replace char* in StradaValue. */
/* Refcounted string — single allocation for header + data. */
typedef struct StradaString {
    uint32_t refcount;
    uint32_t hash;              /* cached hash value */
    uint32_t len;
    char data[];                /* flexible array member — string bytes inline */
} StradaString;

/* StradaString operations */
StradaString *ss_new(const char *s, uint32_t len, uint32_t hash);
StradaString *strada_intern_attr_ss(const char *key, unsigned int hash);
char *strada_intern_pkg_name(const char *s);
void ss_decref_slow(StradaString *ss);  /* handles pool return or free */
/* Set once when the first thread is created (defined in the runtime).
 * Gates the atomic branch in refcount ops — single-threaded programs
 * keep the plain increment. */
extern int strada_threading_active;
/* SS refcounts must be atomic under threading, same as StradaValue
 * refcounts: strings cross threads both directly and via zero-copy
 * shares (keys()/each() hand out SVs sharing hash-key StradaStrings,
 * strada_to_str_ss hands out borrowed data pointers). A plain ++/--
 * raced and could double-free or leak the string backbone.
 * Same shape as strada_incref/strada_decref: the runtime exports real
 * functions (for tcc-compiled code and .so ABI); other TUs get the
 * static-inline fast path. */
#ifdef STRADA_RUNTIME_IMPL
void ss_incref(StradaString *ss);
void ss_decref(StradaString *ss);
#else
static inline void ss_incref(StradaString *ss) {
    if (!ss) return;
    if (strada_threading_active)
        __sync_add_and_fetch(&ss->refcount, 1);
    else
        ss->refcount++;
}
static inline void ss_decref(StradaString *ss) {
    if (!ss) return;
    if (strada_threading_active) {
        if (__sync_sub_and_fetch(&ss->refcount, 1) == 0) ss_decref_slow(ss);
    } else {
        if (--ss->refcount == 0) ss_decref_slow(ss);
    }
}
#endif

/* Recover StradaString header from a data pointer (value.pv points to ss->data).
 * sizeof(StradaString) equals the offset of data[] for flexible array members. */
#define SS_FROM_PV(pv) ((StradaString*)((char*)(pv) - sizeof(StradaString)))

/* Allocate a StradaString and return pointer to its data (for use as value.pv).
 * The returned char* is valid for all string ops; to free, use SS_FREE_PV(). */
static inline char *ss_alloc_pv(const char *s, size_t len) {
    StradaString *ss = ss_new(s, (uint32_t)len, 0);
    return ss->data;
}
/* Free a value.pv that was allocated by ss_alloc_pv */
static inline void ss_free_pv(char *pv) {
    if (pv) ss_decref(SS_FROM_PV(pv));
}

/* Open-addressing hash table with linear probing in hash_index,
 * separate ordered entries array for iteration */
#define HASH_EMPTY     UINT32_MAX
#define HASH_TOMBSTONE (UINT32_MAX - 1)

/* Hash entry — key is a refcounted StradaString */
typedef struct StradaHashEntry {
    StradaString *key;          /* NULL = free/deleted slot */
    StradaValue *value;
    uint32_t next;              /* free list link (HASH_EMPTY = end) */
} StradaHashEntry;

/* Hash structure - like Perl's HV */
struct StradaHash {
    StradaHashEntry *entries;   /* contiguous entry array (insertion order) */
    uint32_t *hash_index;       /* open-addressing table: entry index, HASH_EMPTY, or HASH_TOMBSTONE */
    size_t num_buckets;         /* hash_index size (power of 2) */
    size_t num_entries;         /* live entries */
    size_t capacity;            /* allocated entries array size */
    size_t next_slot;           /* next append position */
    size_t num_tombstones;      /* tombstone count in hash_index */
    uint32_t free_head;         /* internal free list head (HASH_EMPTY = none) */
    int refcount;
    size_t iter_index;          /* for each() */
    uint8_t index_dirty;        /* set when cross-boundary dispatch may have desynced
                                 * hash_index from entries[]; when 0, hash_linear_find
                                 * short-circuits (clean hashes never need a full scan). */
};

/* Mark a hash's index as possibly out-of-sync with entries[]. Subsequent
 * probes will fall back to a linear scan + rebuild. Call this at the point
 * where dispatch crosses a dlopen boundary or wherever the index may have
 * been corrupted. Self-clears after the next rebuild. */
void strada_hash_mark_index_dirty(StradaHash *hv);

/* Value creation functions */
StradaValue* strada_new_undef(void);
StradaValue* strada_undef_static(void);  /* Static singleton for void returns */
StradaValue* strada_new_int(int64_t i);
/* Scratch buffer (n StradaValue* slots) wrapped in a freeable STRADA_CSTRUCT —
 * used by the inlined merge-sort so a throwing comparator can't leak it. */
StradaValue* strada_sortbuf_new(int64_t n);
/* Type-preserving ++/-- step: int-valued operand -> int (tagged, no alloc),
 * else float. See strada_inc_value in strada_runtime.c. */
StradaValue* strada_inc_value(StradaValue *old, int64_t delta);
StradaValue* strada_new_uint(uint64_t u);  /* UV: >INT64_MAX stored UV-flagged, else as signed int */
StradaValue* strada_new_num(double n);
StradaValue* strada_safe_div(double a, double b);      /* Returns undef if b==0 */
StradaValue* strada_safe_mod(int64_t a, int64_t b);    /* Returns undef if b==0 */
StradaValue* strada_new_str(const char *s);
StradaValue* strada_new_dualvar(int64_t iv, const char *s);  /* String + numeric override (e.g. $!) */
StradaValue* strada_new_str_take(char *s);  /* Take ownership of string */
StradaValue* strada_new_str_len(const char *s, size_t len);  /* Binary-safe string */
StradaValue* strada_new_str_len_utf8(const char *s, size_t len);  /* Set SVf_UTF8 flag */
StradaValue* strada_new_str_charflag(const char *s, const char *flag_src); /* UTF-8 flag iff flag_src is valid UTF-8 */
void         strada_set_utf8_flag(StradaValue *sv, int on);     /* Toggle UTF-8 flag */
void         strada_array_set_utf8_flag(StradaValue *arr_sv, int on); /* Stamp every STRADA_STR element */
size_t       byte_to_char_offset(const char *s, size_t byte_offset);
StradaValue* strada_new_str_like(const char *s, StradaValue *flag_src);  /* Mirror flag_src's SVf_UTF8 onto a new STR */
char* strada_tr_utf8(const char *subject, size_t subject_byte_len,
                     const char *from_set, const char *to_set,
                     int del, int comp, int squeeze, int *out_count);

/* Build a STRADA_STR from a C string literal, preserving embedded NUL bytes.
 * sizeof on a string literal is computed at translation time and reflects
 * the actual byte width (adjacent literals like "\x00""ABC" concatenate
 * before sizeof, so the length count is accurate). */
#define STRADA_NEW_STR_LIT(LIT) strada_new_str_len((LIT), sizeof(LIT) - 1)

size_t strada_str_len(StradaValue *sv);  /* Get string length (binary-safe) */
StradaValue* strada_new_array(void);
StradaValue* strada_new_hash(void);
StradaValue* strada_new_hash_presized(int capacity);
StradaValue* strada_new_filehandle(FILE *fh);

/* Reference counting and type conversion.
 * When STRADA_RUNTIME_IMPL is defined (in strada_runtime.c), these are regular
 * function declarations (exported for .so ABI). Otherwise, they're static inline
 * fast-path wrappers that call the _impl functions. */
#ifdef STRADA_RUNTIME_IMPL
/* Implementation file — declare as regular exported functions */
void strada_incref(StradaValue *sv);
void strada_decref(StradaValue *sv);
/* break_self_cycle: only the _impl is exported; user code calls the
 * inline wrapper from the non-IMPL branch below. Keeping the wrapper
 * static inline means there's no name collision between user TUs and
 * the runtime symbol — gcc can't get confused into using the impl as
 * a substitute for the wrapper. */
void strada_break_self_cycle_impl(StradaValue *sv);
StradaValue* strada_deref_array_value_owned(StradaValue *ref);
StradaValue* strada_deref_hash_value_owned(StradaValue *ref);
int64_t strada_to_int(StradaValue *sv);
double strada_to_num(StradaValue *sv);
#else
/* Header-only inline fast paths for callers */
void strada_incref_impl(StradaValue *sv);
void strada_decref_impl(StradaValue *sv);
void strada_break_self_cycle_impl(StradaValue *sv);
StradaValue* strada_deref_array_value_owned(StradaValue *ref);
StradaValue* strada_deref_hash_value_owned(StradaValue *ref);
int64_t strada_to_int_impl(StradaValue *sv);
double strada_to_num_impl(StradaValue *sv);
/* always_inline on these three is critical for OOP hot loops: the
 * tagged-int fast path is just a bit-test, and without forced inlining
 * gcc keeps the wrappers as out-of-line calls under -O3+LTO. Inlining
 * folds the tagged-int branch into the surrounding code and lets the
 * impl call disappear when the surrounding code already proves the
 * pointer is non-NULL / not a tagged int. */
static inline __attribute__((always_inline)) void strada_incref(StradaValue *sv) {
    if (!sv || STRADA_IS_TAGGED_INT(sv)) return;
    strada_incref_impl(sv);
}
static inline __attribute__((always_inline)) void strada_decref(StradaValue *sv) {
    if (!sv || STRADA_IS_TAGGED_INT(sv)) return;
    strada_decref_impl(sv);
}
static inline __attribute__((always_inline)) int64_t strada_to_int(StradaValue *sv) {
    if (STRADA_IS_TAGGED_INT(sv)) return STRADA_TAGGED_INT_VAL(sv);
    return strada_to_int_impl(sv);
}
static inline __attribute__((always_inline)) double strada_to_num(StradaValue *sv) {
    if (STRADA_IS_TAGGED_INT(sv)) return (double)STRADA_TAGGED_INT_VAL(sv);
    return strada_to_num_impl(sv);
}
/* Fast-path inline: most calls in hot loops are on unique-held SVs that
 * cannot possibly form a cycle (rc<2). Skip the function call entirely
 * in that case; the impl version handles the rare slow path where rc>=2
 * and we actually need to scan for self-refs.
 *
 * Safety proof: if `sv` is held by only one reference (rc<2), no other
 * live object can point to it, so its contents cannot reach back to it.
 * For REF wrappers, both the outer REF and inner container must be rc<2
 * to guarantee no cycle. */
static inline __attribute__((always_inline)) void strada_break_self_cycle(StradaValue *sv) {
    if (!sv || STRADA_IS_TAGGED_INT(sv)) return;
    if (sv->refcount < 2) {
        int t = sv->type;
        if (t != STRADA_REF) return;
        StradaValue *target = sv->value.rv;
        if (!target || STRADA_IS_TAGGED_INT(target)) return;
        if (target->refcount < 2) return;
    }
    strada_break_self_cycle_impl(sv);
}
#endif
char* strada_to_str(StradaValue *sv);    /* Returns strdup'd char* — free with free() (backward compat) */
char* strada_to_str_ss(StradaValue *sv); /* Returns StradaString-backed char* — free with strada_cstr_free() */
void strada_cstr_free(char *s);          /* Free a strada_to_str_ss() result (handles both SS and malloc'd) */
const char* strada_to_str_buf(StradaValue *sv, char *buf, size_t buflen);  /* Non-allocating variant */
int strada_to_bool(StradaValue *sv);

/* ===== Memory-management features (compile-time: STRADA_CYCLE_GC / STRADA_ARENA) =====
 * These symbols are ALWAYS declared and ALWAYS defined in strada_runtime.c — as
 * the real implementation when the feature macro is set (default via ./configure),
 * or as no-op stubs otherwise. So generated code and the compiler can call them
 * unconditionally regardless of how the runtime was configured. */
void strada_gc_collect(void);            /* Force a cycle collection now */
void strada_gc_set_enabled(int on);      /* Enable/disable automatic collection */
void strada_gc_set_threshold(long n);    /* Candidate-root count that triggers auto-collect */
unsigned long strada_gc_collections(void); /* Stats: collections run */
unsigned long strada_gc_objects_freed(void); /* Stats: cyclic objects reclaimed */
void strada_arena_begin(void);           /* Start a request arena (bump region) */
void strada_arena_end(void);             /* Free the current arena wholesale */
int  strada_arena_active(void);          /* 1 if an arena is currently active */

/* String comparison with C literals (no temporaries) */
int strada_str_eq_lit(StradaValue *sv, const char *lit);
int strada_str_ne_lit(StradaValue *sv, const char *lit);
int strada_str_lt_lit(StradaValue *sv, const char *lit);
int strada_str_gt_lit(StradaValue *sv, const char *lit);
int strada_str_le_lit(StradaValue *sv, const char *lit);
int strada_str_ge_lit(StradaValue *sv, const char *lit);

/* Increment/decrement operations */
StradaValue* strada_postincr(StradaValue **pv);  /* $i++ */
StradaValue* strada_postdecr(StradaValue **pv);  /* $i-- */
StradaValue* strada_preincr(StradaValue **pv);   /* ++$i */
StradaValue* strada_predecr(StradaValue **pv);   /* --$i */

/* Array operations */
StradaArray* strada_array_new(void);
void strada_array_push(StradaArray *av, StradaValue *sv);
void strada_array_push_take(StradaArray *av, StradaValue *sv);
StradaValue* strada_array_pop(StradaArray *av);
StradaValue* strada_array_shift(StradaArray *av);
void strada_array_unshift(StradaArray *av, StradaValue *sv);
StradaValue* strada_array_get(StradaArray *av, int64_t idx);
/* Same as strada_array_get but increfs the result so the caller owns
 * a separate reference. Used by codegen for $h{k}[i] / $h{k}->[i]
 * where the source is an owned-ref temp that will be decref'd
 * immediately afterward — without this, the element could be freed
 * with the temp's array. */
StradaValue* strada_array_get_owned(StradaArray *av, int64_t idx);
StradaValue* strada_array_get_safe(StradaValue *arr, int64_t idx);  /* For destructuring */
void strada_array_set(StradaArray *av, int64_t idx, StradaValue *sv);
int strada_array_exists_idx(StradaArray *av, int64_t idx);
StradaValue* strada_array_delete_idx(StradaArray *av, int64_t idx);
StradaValue* strada_hash_delete_take(StradaHash *hv, const char *key);
StradaValue* strada_hash_delete_take_sv(StradaHash *hv, StradaValue *key_sv);
size_t strada_array_length(StradaArray *av);
void strada_array_reverse(StradaArray *av);
int64_t strada_get_array_default_capacity(void);
void strada_set_array_default_capacity(int64_t capacity);
void strada_array_reserve(StradaArray *av, size_t capacity);
void strada_reserve_sv(StradaValue *sv, int64_t capacity);
int64_t strada_size(StradaValue *sv);
StradaValue* strada_new_array_from_av(StradaArray *av);
StradaValue* strada_sort(StradaValue *arr);   /* Sort array alphabetically */
StradaValue* strada_nsort(StradaValue *arr);  /* Sort array numerically */
StradaValue* strada_range(StradaValue *start, StradaValue *end);  /* Create array from range */

/* Hash operations */
StradaHash* strada_hash_new(void);
int64_t strada_get_hash_default_capacity(void);
void strada_set_hash_default_capacity(int64_t capacity);
void strada_hash_set(StradaHash *hv, const char *key, StradaValue *sv);
void strada_hash_set_take(StradaHash *hv, const char *key, StradaValue *sv);
void strada_hash_set_take_ph(StradaHash *hv, const char *key, unsigned int hash, StradaValue *sv);
/* Single-probe lvalue: pointer to the value slot (autoviv -> undef when absent
 * and autoviv!=0, else NULL). For read-modify-write without a second probe. */
StradaValue **strada_hv_fetch_lvalue(StradaHash *hv, const char *key, int autoviv);
/* StradaValue wrapper: derefs sv to its hash and returns the value-slot ptr.
 * Returns NULL for a tied or non-hash sv (caller must fall back to the
 * fetch/store path so tie FETCH/STORE magic still fires). */
StradaValue **strada_hv_fetch_lvalue_sv(StradaValue *sv, const char *key, int autoviv);
/* Same, but takes the key as a StradaValue: a STR key reuses its pv with no
 * strdup. Returns NULL for tied/non-hash sv (caller falls back). */
StradaValue **strada_hv_fetch_lvalue_sv_key(StradaValue *sv, StradaValue *key_sv, int autoviv);
void strada_hash_set_ss_take(StradaHash *hv, StradaString *key_ss, StradaValue *sv);
StradaValue* strada_hash_get(StradaHash *hv, const char *key);
StradaValue* strada_autoviv_hash(StradaValue *sv, const char *key);
StradaValue* strada_autoviv_array(StradaValue *sv, const char *key);
StradaValue* strada_autoviv_elem_hash(StradaValue *arr_sv, int64_t idx);
StradaValue* strada_autoviv_elem_array(StradaValue *arr_sv, int64_t idx);
int strada_hash_exists(StradaHash *hv, const char *key);
void strada_hash_delete(StradaHash *hv, const char *key);
/* _sv variants: accept StradaValue* key directly (avoids strada_to_str/strdup) */
void strada_hash_set_sv(StradaHash *hv, StradaValue *key_sv, StradaValue *sv);
StradaValue* strada_hash_get_sv(StradaHash *hv, StradaValue *key_sv);
int strada_hash_exists_sv(StradaHash *hv, StradaValue *key_sv);
void strada_hash_delete_sv(StradaHash *hv, StradaValue *key_sv);
StradaArray* strada_hash_keys(StradaHash *hv);
StradaArray* strada_hash_values(StradaHash *hv);
StradaArray* strada_hash_keys_sv(StradaValue *sv);
StradaArray* strada_hash_values_sv(StradaValue *sv);
StradaValue* strada_hash_each_sv(StradaValue *sv);
void strada_tied_each_reset(StradaValue *sv);
void strada_hash_reserve(StradaHash *hv, size_t capacity);
void strada_hash_reserve_sv(StradaValue *sv, int64_t capacity);

/* String operations (UTF-8 aware) */
char* strada_concat(const char *a, const char *b);
char* strada_concat_free(char *a, char *b);  /* Concat and free inputs (avoids leaks) */
StradaValue* strada_concat_sv(StradaValue *a, StradaValue *b);  /* Fast concat on StradaValues */
StradaValue* strada_concat_inplace(StradaValue *a, StradaValue *b);  /* In-place concat: consumes a, borrows b */
StradaValue* strada_concat_inplace_cstr(StradaValue *a, const char *str_b, size_t len_b);  /* In-place concat with C string literal */
size_t strada_length(const char *s);      /* Returns character count (UTF-8 codepoints) */
size_t strada_length_chars_sv(StradaValue *sv);  /* Binary-safe UTF-8 char count */
size_t strada_length_sv(StradaValue *sv); /* Binary-safe length using struct_size */
size_t strada_bytes(const char *s);       /* Returns byte count */
StradaValue* strada_char_at(StradaValue *str, StradaValue *index);  /* Fast char code by index */
StradaValue* strada_byte_at(StradaValue *str, StradaValue *index);  /* Preferred alias for char_at */
StradaValue* strada_idiv(StradaValue *a, StradaValue *b);           /* Integer division (truncated) */
StradaValue* strada_substr(StradaValue *str, int64_t offset, int64_t length);
StradaValue* strada_substr_bytes(StradaValue *str, int64_t offset, int64_t length);
StradaValue* strada_substr_replace(StradaValue *str, int64_t offset, int64_t length,
                                    StradaValue *repl, StradaValue **out_removed);
int strada_index(const char *haystack, const char *needle);
int64_t strada_index_sv(StradaValue *haystack_sv, const char *needle);
int64_t strada_index_sv2(StradaValue *haystack_sv, StradaValue *needle_sv, int64_t offset);
int strada_index_offset(const char *haystack, const char *needle, int offset);
int strada_rindex(const char *haystack, const char *needle);
char* strada_upper(const char *str);
char* strada_lower(const char *str);
char* strada_uc(const char *str);  /* Alias for upper */
char* strada_lc(const char *str);  /* Alias for lower */
char* strada_ucfirst(const char *str);
char* strada_lcfirst(const char *str);
/* ASCII-only case mapping (a-z<->A-Z, bytes >=128 untouched) — used for
 * strings without the UTF8 flag, matching Perl's byte-semantics uc/lc. */
char* strada_uc_ascii(const char *str);
char* strada_lc_ascii(const char *str);
char* strada_ucfirst_ascii(const char *str);
char* strada_lcfirst_ascii(const char *str);
char* strada_trim(const char *str);
char* strada_ltrim(const char *str);
char* strada_rtrim(const char *str);
char* strada_reverse(const char *str);
StradaValue* strada_reverse_sv(StradaValue *sv);  /* Generic reverse for strings and arrays */
char* strada_repeat(const char *str, int count);
char* strada_chr(int code);
StradaValue* strada_chr_sv(int code);  /* Binary-safe version that handles NUL bytes */
StradaValue* strada_byte_chr(int code); /* core::byte(n): single raw byte, never UTF-8 */
int strada_ord(const char *str);
int strada_ord_byte(StradaValue *sv);  /* Binary-safe: returns raw byte value 0-255 */
int strada_get_byte(StradaValue *sv, int pos);  /* Get byte at position, returns 0-255 or -1 */
StradaValue* strada_set_byte(StradaValue *sv, int pos, int val);  /* Set byte, returns new string */
int strada_byte_length(StradaValue *sv);  /* Get byte length (not UTF-8 char count) */
StradaValue* strada_byte_substr(StradaValue *sv, int start, int len);  /* Substring by byte positions */
StradaValue* strada_pack(const char *fmt, StradaValue *args);  /* Pack values into binary string */
/* vec(EXPR, OFFSET, BITS) — perl's bit-vector accessor. BITS in
 * {1,2,4,8,16,32,64}. Sub-byte fields are MSB-first within each byte;
 * multi-byte fields are big-endian. strada_vec_set extends sv in
 * place (zero-fills new bytes). */
int64_t strada_vec_get(StradaValue *sv, int64_t offset, int bits);
void strada_vec_set(StradaValue *sv, int64_t offset, int bits, int64_t value);
StradaValue* strada_unpack(const char *fmt, StradaValue *data);  /* Unpack binary string to array */
StradaValue* strada_base64_encode(StradaValue *sv);  /* Encode string to base64 */
StradaValue* strada_base64_decode(StradaValue *sv);  /* Decode base64 to string */
StradaValue* strada_hex(StradaValue *sv);  /* Convert hex string to integer: hex("ff") -> 255 */
StradaValue* strada_array_copy(StradaValue *src);  /* Deep copy array: new array with incref'd elements */
char* strada_chomp(const char *str);
char* strada_chop(const char *str);
int strada_strcmp(const char *s1, const char *s2);
int strada_strncmp(const char *s1, const char *s2, int n);
char* strada_join(const char *sep, StradaArray *arr);
StradaValue* strada_join_sv(StradaValue *sep_sv, StradaArray *arr);

/* StringBuilder functions for efficient string building */
StradaValue* strada_sb_new(void);                              /* Create new StringBuilder */
StradaValue* strada_sb_new_cap(StradaValue *capacity);         /* Create with initial capacity */
void strada_sb_append(StradaValue *sb, StradaValue *str);      /* Append string */
void strada_sb_append_str(StradaValue *sb, const char *str);   /* Append C string */
StradaValue* strada_sb_to_string(StradaValue *sb);             /* Get final string */
StradaValue* strada_sb_length(StradaValue *sb);                /* Get current length */
void strada_sb_clear(StradaValue *sb);                         /* Clear buffer */
void strada_sb_free(StradaValue *sb);                          /* Free StringBuilder */

/* I/O functions */
void strada_print(StradaValue *sv);
void strada_say(StradaValue *sv);
void strada_print_fh(StradaValue *sv, StradaValue *fh);
/* Hook: route `print $fh ...` to a custom writer (e.g. IO::Socket::SSL's
 * SSL_write). Returns non-zero if it consumed the write. NULL = default. */
extern int (*strada_fh_write_hook)(StradaValue *sv, StradaValue *fh);
void strada_say_fh(StradaValue *sv, StradaValue *fh);
StradaValue* strada_readline(void);
void strada_printf(const char *format, ...);
StradaValue* strada_sprintf(const char *format, ...);
StradaValue* strada_sprintf_sv(StradaValue *format_sv, int arg_count, ...);
StradaValue* strada_sprintf_sv_arr(StradaValue *format_sv, StradaValue *args_sv);
void strada_warn(const char *format, ...);

/* File I/O functions */
StradaValue* strada_open(const char *filename, const char *mode);
StradaValue* strada_open_str(const char *content, const char *mode);  /* In-memory I/O */
StradaValue* strada_open_sv(StradaValue *first_arg, StradaValue *mode_arg);  /* Type-dispatch open */
StradaValue* strada_str_from_fh(StradaValue *fh);  /* Extract string from memstream */
StradaValue* strada_close(StradaValue *fh);  /* returns tagged int: 1 closed, 0 nothing to close */
StradaValue* strada_read_file(StradaValue *fh);
StradaValue* strada_read_line(StradaValue *fh);
StradaValue* strada_read_all_lines(StradaValue *fh);
void strada_write_file(StradaValue *fh, const char *content);
int strada_file_exists(const char *filename);
StradaValue* strada_file_mtime(StradaValue *path);  /* mtime as int sv, -1 on failure */
StradaValue* sys_file_mtime(StradaValue *path);     /* alias used by bootstrap codegen */
StradaValue* strada_slurp(const char *filename);  /* Read entire file */
StradaValue* strada_slurp_fh(StradaValue *fh_sv);  /* Read from FILE handle to end */
StradaValue* strada_slurp_fd(StradaValue *fd_sv);  /* Read from file descriptor to end */
void strada_spew(const char *filename, const char *content);  /* Write entire file */
void strada_spew_sv(const char *filename, StradaValue *content_sv);  /* Binary-safe write from StradaValue */
void strada_spew_len(const char *filename, const char *content, size_t len);  /* Length-aware write */
void strada_spew_fh(StradaValue *fh_sv, StradaValue *content_sv);  /* Write to FILE handle */
void strada_spew_fd(StradaValue *fd_sv, StradaValue *content_sv);  /* Write to file descriptor */

/* Built-in functions */
void strada_dump(StradaValue *sv, int indent);
StradaValue* strada_dumper(StradaValue *sv);       /* Returns dump as string */
StradaValue* strada_dumper_str(StradaValue *sv);  /* Alias for strada_dumper */
StradaValue* strada_defined(StradaValue *sv);
int strada_defined_bool(StradaValue *sv);  /* Non-allocating version */
StradaValue* strada_ref(StradaValue *sv);

/* Utility functions */
void strada_die(const char *format, ...);
void strada_die_sv(StradaValue *msg);
void strada_exit(int code);
void strada_stacktrace(void);
void strada_backtrace(void);
char* strada_stacktrace_str(void);  /* Returns stack trace as string */
const char* strada_caller(int level);

/* Exception handling (try/catch/throw) */
#define STRADA_MAX_TRY_DEPTH 64
typedef struct {
    jmp_buf buf;
    int active;
} StradaTryContext;

/* THREAD-LOCAL (see runtime): per-thread try frames + exception slots. */
#ifdef STRADA_NO_TLS
extern StradaTryContext strada_try_stack[STRADA_MAX_TRY_DEPTH];
extern int strada_try_depth;
#else
extern __thread StradaTryContext strada_try_stack[STRADA_MAX_TRY_DEPTH];
extern __thread int strada_try_depth;
#endif
jmp_buf *strada_try_push_slot(void);   /* tcc-callable macro twins */
int strada_try_pop_slot(void);
StradaValue* strada_exception_trace_get(void);  /* core::exception_trace */
#ifdef STRADA_NO_TLS
extern char *strada_exception_msg;
extern StradaValue *strada_exception_value;
#else
extern __thread char *strada_exception_msg;
extern __thread StradaValue *strada_exception_value;
#endif
/* Hook for `use overload '""' => sub { ... }`. Perla sets this in perla_init.
 * Returns a strdup'd string when an overload sub is installed for `sv`'s
 * blessed package, or NULL to fall through to the default REF(0x…) format. */
extern char *(*strada_overload_stringify_hook)(StradaValue *sv);
/* Hooks for `0+` (numeric) and `bool` overloads.
 * numeric: returns 1 + sets *out if overload fired, else 0.
 * bool:    returns -1 (no overload), 0 (false), 1 (true). */
extern int (*strada_overload_numeric_hook)(StradaValue *sv, double *out);
extern int (*strada_overload_bool_hook)(StradaValue *sv);
/* DESTROY hook — Perl-style packages register DESTROY via perla_code_set,
 * not the strada OOP table that strada_call_destroy walks. If set,
 * strada_call_destroy calls this BEFORE its own lookup. Returns 1 if it
 * handled the call (skip default path), 0 if not. */
extern int (*strada_destroy_hook)(StradaValue *obj);

/* Optional die-path hooks for embedders. Both default to NULL.
 *
 *  strada_die_trace_hook(msg, try_depth) — called on every throw/die
 *  before unwind. Embedder uses this to log a diagnostic with its own
 *  call-stack info (e.g. Perla checks $ENV{PERLA_DIE_TRACE} and dumps
 *  perla_call_stack). The hook does NOT abort or alter control flow.
 *
 *  strada_die_continue_hook() — consulted ONLY when a die is fatal
 *  (no enclosing try block). Returns 1 to suppress exit(1) and warn-
 *  and-continue instead. Embedder uses this for "limp along to see
 *  what dies next" debugging (e.g. PERLA_DIE_WARN). */
extern void (*strada_die_trace_hook)(const char *msg, int try_depth);
extern int (*strada_die_continue_hook)(void);
/* Fills `out` with " at FILE line N." (with leading space) for the
 * current call site, used to suffix die messages that lack a trailing
 * newline (Perl semantics). NULL → no suffix appended. */
extern void (*strada_die_location_hook)(char *out, size_t outlen);

__attribute__((noreturn)) void strada_throw(const char *msg);
__attribute__((noreturn)) void strada_throw_value(StradaValue *sv);
StradaValue* strada_get_exception(void);
void strada_clear_exception(void);
int strada_in_try_block(void);

/* Pending cleanup for function call args and local vars in try blocks.
 * Thread-local + internal to strada_runtime.c (see there): push/pop are real
 * functions, not inline, so the per-thread storage never crosses the ABI
 * boundary (a tcc-built program linking the gcc runtime calls these rather than
 * touching the __thread variable, which tcc cannot express). */
#define STRADA_MAX_PENDING_CLEANUP 4096
void strada_cleanup_push(StradaValue *sv);
void strada_cleanup_pop(void);
void strada_cleanup_drain(void);
int strada_cleanup_mark(void);         /* Get current depth */
void strada_cleanup_restore(int mark); /* Restore to depth (no decref) */
void strada_cleanup_drain_to(int mark); /* Drain to depth (with decref) */

/* Macros for try/catch - used by generated code */
#define STRADA_TRY_PUSH() (strada_try_depth < STRADA_MAX_TRY_DEPTH ? \
    (strada_try_stack[strada_try_depth].active = 1, &strada_try_stack[strada_try_depth++].buf) : NULL)
#define STRADA_TRY_POP() (strada_try_depth > 0 ? (strada_try_stack[--strada_try_depth].active = 0, 1) : 0)

/* Call stack for stack traces */
#define STRADA_MAX_CALL_DEPTH 256
typedef struct {
    const char *func_name;   /* Function name */
    const char *file_name;   /* Source file name */
    int line;                /* Current line number */
} StradaStackFrame;

/* Slot 0 is a permanent sentinel frame; live frames occupy 1..depth. This
 * lets the per-call-site line store (strada_stack_line_il) be a single
 * unconditional write — depth 0 harmlessly hits the sentinel.
 * THREAD-LOCAL: each thread tracks its own frames — a shared stack
 * interleaved pushes/pops across threads (garbled traces, and a racy
 * depth check could write one slot past the array). */
#ifdef STRADA_NO_TLS
extern StradaStackFrame strada_call_stack[STRADA_MAX_CALL_DEPTH + 1];
extern int strada_call_depth;
#else
extern __thread StradaStackFrame strada_call_stack[STRADA_MAX_CALL_DEPTH + 1];
extern __thread int strada_call_depth;
#endif
extern int strada_recursion_limit;  /* Configurable limit (default 1000, 0 = disabled) */
extern int strada_pending_call_line;  /* set by codegen at each call site; strada_stack_push consumes */

void strada_stack_push(const char *func_name, const char *file_name);
void strada_stack_pop(void);
void strada_stack_set_line(int line);

/* Inline fast paths — emitted by the codegen at every function entry/exit
 * and call site, so they must not be out-of-line calls (profiled at ~4.5%
 * of a call-heavy workload). The out-of-line versions above remain the
 * compat ABI for bootstrap-generated C and previously compiled .so
 * modules, and serve as the slow path (recursion limit / overflow). */
static inline void strada_stack_push_il(const char *func_name, const char *file_name) {
    int d = strada_call_depth;
    if (__builtin_expect(d >= STRADA_MAX_CALL_DEPTH
                         || (strada_recursion_limit > 0 && d >= strada_recursion_limit), 0)) {
        strada_stack_push(func_name, file_name);  /* limit/overflow handling */
        return;
    }
    strada_call_depth = d + 1;
    StradaStackFrame *f = &strada_call_stack[d + 1];
    f->func_name = func_name;
    f->file_name = file_name;
    f->line = 0;
}
static inline void strada_stack_pop_il(void) {
    if (strada_call_depth > 0) strada_call_depth--;
}
static inline void strada_stack_line_il(int line) {
    strada_call_stack[strada_call_depth].line = line;
}
StradaValue* strada_caller_info(StradaValue *level);
void strada_print_stack_trace(FILE *out);
char* strada_capture_stack_trace(void);
void strada_set_recursion_limit(int limit);  /* Set max recursion depth (0 = disabled) */
int strada_get_recursion_limit(void);        /* Get current limit */

/* Dynamic return type (wantarray) support */
/* Context values: 0=scalar, 1=array, 2=hash */
#ifdef STRADA_NO_TLS
extern int strada_call_context;
#else
extern __thread int strada_call_context;
#endif
void strada_set_call_context(int ctx);
int strada_wantarray(void);
int strada_wantscalar(void);
int strada_wanthash(void);

/* UTF-8 namespace functions */
int strada_utf8_is_valid(const char *str, size_t len);
StradaValue* strada_utf8_is_utf8(StradaValue *sv);
StradaValue* strada_utf8_valid(StradaValue *sv);
StradaValue* strada_utf8_encode(StradaValue *sv);
StradaValue* strada_utf8_decode(StradaValue *sv);
StradaValue* strada_utf8_downgrade(StradaValue *sv, int fail_ok);
StradaValue* strada_utf8_upgrade(StradaValue *sv);
StradaValue* strada_utf8_unicode_to_native(StradaValue *sv);

/* Unicode normalization (UAX#15 — NFC / NFD / NFKC / NFKD). Backed
 * by the generated UCD tables in strada_norm_tables.h. */
StradaValue *strada_unicode_normalize(StradaValue *sv, int compose, int compat);
StradaValue *strada_utf8_nfc(StradaValue *sv);
StradaValue *strada_utf8_nfd(StradaValue *sv);
StradaValue *strada_utf8_nfkc(StradaValue *sv);
StradaValue *strada_utf8_nfkd(StradaValue *sv);
StradaValue *strada_utf8_normalize(StradaValue *sv, StradaValue *form);

/* Type introspection and casting */
const char* strada_typeof(StradaValue *sv);
int strada_is_int(StradaValue *sv);
int strada_is_num(StradaValue *sv);
int strada_is_str(StradaValue *sv);
int strada_is_array(StradaValue *sv);
int strada_is_hash(StradaValue *sv);
StradaValue* strada_int(StradaValue *sv);    /* Cast to int */
StradaValue* strada_num(StradaValue *sv);    /* Cast to num */
StradaValue* strada_str(StradaValue *sv);    /* Cast to str */
StradaValue* strada_bool(StradaValue *sv);   /* Convert to boolean */
int strada_scalar(StradaValue *sv);          /* Scalar context evaluation */

/* Reference system */
StradaValue* strada_ref_create(StradaValue *sv);      /* Create reference (shared ownership) */
StradaValue* strada_ref_create_take(StradaValue *sv); /* Create reference (take ownership) */
/* Fused blessed-object constructors. Codegen emits a call to one of these
 * for Pkg::new(k1, v1, k2, v2[, k3, v3]) when all has-attrs in the package
 * chain are set explicitly (no defaults to fill). Saves 4-5 function calls
 * per object construction by inlining the allocation + bless + small-hash
 * inserts. Keys must be statically-interned StradaString*; values are taken
 * with no incref. */
/* Directly-blessed HASH ctor — skips the REF wrapper for tight ctor+drop
 * loops where the result doesn't need a REF wrapper (the common case for
 * methods accessed via $obj->method that go through strada_deref_hash,
 * which handles both REF→HASH and direct HASH). Saves ~20-30ns per
 * object alloc on hot paths. \@blessed_hash still produces a REF→HASH
 * when the user explicitly takes a reference, so external semantics
 * are preserved. */
StradaValue* strada_new_blessed_2attr_hv_take(
        char *interned_pkg,
        StradaString *k1, StradaValue *v1,
        StradaString *k2, StradaValue *v2);
StradaValue* strada_new_blessed_3attr_hv_take(
        char *interned_pkg,
        StradaString *k1, StradaValue *v1,
        StradaString *k2, StradaValue *v2,
        StradaString *k3, StradaValue *v3);

StradaValue* strada_new_blessed_2attr_take(
    char *interned_pkg,
    StradaString *k1, StradaValue *v1,
    StradaString *k2, StradaValue *v2);
StradaValue* strada_new_blessed_3attr_take(
    char *interned_pkg,
    StradaString *k1, StradaValue *v1,
    StradaString *k2, StradaValue *v2,
    StradaString *k3, StradaValue *v3);
void strada_overwrite_in_place(StradaValue *dst, StradaValue *src);
StradaValue* strada_ref_deref(StradaValue *ref);      /* Dereference */
int strada_is_ref(StradaValue *sv);                   /* Check if reference */
const char* strada_reftype(StradaValue *ref);         /* Get type of referent */
StradaValue* strada_ref_scalar(StradaValue **ptr);    /* Reference to scalar variable */
StradaValue* strada_ref_array(StradaArray **ptr);     /* Reference to array */
StradaValue* strada_ref_hash(StradaHash **ptr);       /* Reference to hash */

/* New Perl-style reference functions */
StradaValue* strada_new_ref(StradaValue *target, char ref_type);  /* \$var, \@arr, \%hash */
StradaValue* strada_new_ref_take(StradaValue *target, char ref_type);  /* takes ownership — no incref */

/* Slot references — reference to a C local variable's storage (StradaValue**).
 * Allows $$ref and $$ref = val to read/write the original variable.
 * Used for pass-by-reference patterns: func(\$var).
 * WARNING: The slot ref MUST NOT outlive the stack frame containing the variable. */
#define STRADA_SLOT_REF_MARKER ((size_t)0xDEAD5107ULL)
StradaValue* strada_slot_ref_create(StradaValue **slot);
static inline int strada_is_slot_ref(StradaValue *sv) {
    return sv && !STRADA_IS_TAGGED_INT(sv) && sv->type == STRADA_REF &&
           sv->struct_size == STRADA_SLOT_REF_MARKER;
}
StradaValue* strada_deref(StradaValue *ref);          /* $$ref - deref scalar ref */
StradaValue* strada_deref_set(StradaValue *ref, StradaValue *new_value); /* deref_set($ref, $val) */
StradaHash* strada_deref_hash(StradaValue *ref);      /* For $ref->{key} */
StradaArray* strada_deref_array(StradaValue *ref);    /* For $ref->[index] */
StradaValue* strada_deref_hash_value(StradaValue *ref);  /* deref_hash() builtin */
StradaValue* strada_deref_array_value(StradaValue *ref); /* deref_array() builtin */
StradaValue* strada_anon_hash(int count, ...);        /* { key => val, ... } */
StradaValue* strada_anon_array(int count, ...);       /* [ elem, ... ] */
StradaValue* strada_array_from_ref(StradaValue *ref); /* Copy array from ref */
StradaValue* strada_hash_from_ref(StradaValue *ref);  /* Copy hash from ref */
StradaValue* strada_hash_from_flat_array(StradaValue *arr); /* Convert flat array [k,v,k,v,...] to hash */
StradaValue* strada_hash_to_flat_array(StradaValue *hash); /* Convert hash to flat array [k,v,k,v,...] */

/* OOP - Blessed references (like Perl's bless) */
typedef StradaValue* (*StradaMethod)(StradaValue *self, StradaValue *args);
StradaValue* strada_bless(StradaValue *ref, const char *package); /* Bless ref into package, returns ref */
StradaValue* strada_bless_cached(StradaValue *ref, char *interned_pkg_name);
StradaValue* strada_blessed(StradaValue *ref);                  /* Get package name or undef */
void strada_set_package(const char *package);                   /* Set current package context */
const char* strada_current_package(void);                       /* Get current package */
void strada_inherit(const char *child, const char *parent);     /* Set up inheritance (2 args) */
void strada_inherit_from(const char *parent);                   /* Inherit from parent (1 arg, uses current package) */
void strada_method_register(const char *package, const char *name, StradaMethod func);
void strada_modifier_register(const char *package, const char *method, int type, StradaMethod func);
StradaValue* strada_method_call(StradaValue *obj, const char *method, StradaValue *args);
StradaValue* strada_method_call_ph(StradaValue *obj, const char *method, StradaValue *args, unsigned int method_hash);
/* Per-call-site monomorphic inline cache for method dispatch. Codegen emits
 * one `static StradaCallSite` per literal-name call site; the common case
 * (same receiver class as last time, no before/after/around modifiers on
 * the method) dispatches with two compares and an indirect call. Entries
 * are validated by a generation stamp bumped on method/inherit/modifier
 * registration, so they never serve stale resolutions. Zero-initialized
 * statics start stale (gen 0 never matches). */
typedef struct StradaCallSite {
    const char *pkg;   /* blessed-package pointer last dispatched (identity) */
    StradaMethod fn;
    uint32_t gen;      /* generation stamp; 0 = never filled */
    int8_t has_mod;    /* 1 = method has modifiers in its MRO (skip fast path) */
} StradaCallSite;
StradaValue* strada_method_call_cs(StradaValue *obj, const char *method, StradaValue *args, unsigned int method_hash, StradaCallSite *cs);
const char* strada_method_lookup_package(const char *package, const char *method);
const char* strada_get_parent_package(const char *package);     /* Get parent package */
int strada_isa(StradaValue *obj, const char *package);          /* Check inheritance */
int strada_can(StradaValue *obj, const char *method);           /* Check method exists */
void strada_oop_init(void);  /* Initialize OOP system */

/* SUPER:: and DESTROY support */
StradaValue* strada_super_call(StradaValue *obj, const char *from_package,
                               const char *method, StradaValue *args);
void strada_call_destroy(StradaValue *obj);  /* Call DESTROY method */
void strada_set_method_package(const char *pkg);  /* Set current method's package */
const char* strada_get_method_package(void);      /* Get current method's package */

/* Operator overloading */
void strada_overload_register(const char *package, const char *op, StradaMethod func);
StradaValue* strada_overload_binary(StradaValue *left, StradaValue *right, const char *op);
StradaValue* strada_overload_unary(StradaValue *operand, const char *op);
StradaValue* strada_overload_stringify(StradaValue *val);

/* Variadic function support */
StradaValue* strada_pack_args(int count, ...);        /* Pack args into array */

/* Memory management */
void strada_free(StradaValue *sv);  /* Explicitly free (decref) a value */
StradaValue* strada_release(StradaValue *ref);  /* Free via ref and set to undef */
StradaValue* strada_undef(StradaValue *sv);  /* Set to undef and return it */
int strada_refcount(StradaValue *sv);  /* Get reference count */

/* Weak references */
void strada_weaken(StradaValue **ref_ptr);   /* Make a reference weak */
void strada_weaken_hv_entry(StradaHash *hv, const char *key);  /* Weaken a hash entry value */
int strada_isweak(StradaValue *ref);        /* Check if reference is weak */
void strada_weak_registry_init(void);       /* Initialize weak reference registry */
void strada_weak_registry_remove_target(StradaValue *target);  /* Notify weak refs when target dies */
void strada_weak_registry_unregister(StradaValue *ref);        /* Remove a weak ref from registry */

/* FFI - Foreign Function Interface */
typedef void* (*StradaCFunc)(void*, void*, void*, void*, void*);

/* C struct support */
StradaValue* strada_cstruct_new(const char *struct_name, size_t size);
void* strada_cstruct_ptr(StradaValue *sv);
void strada_cstruct_set_field(StradaValue *sv, const char *field, size_t offset, void *value, size_t size);
void* strada_cstruct_get_field(StradaValue *sv, const char *field, size_t offset, size_t size);

/* C pointer support */
StradaValue* strada_cpointer_new(void *ptr);
void* strada_cpointer_get(StradaValue *sv);

/* Closure support (uses triple pointers for capture-by-reference) */
StradaValue* strada_closure_new(void *func, int params, int captures, StradaValue ***cap_array);
StradaValue* strada_closure_call(StradaValue *closure, int argc, ...);
StradaValue* strada_closure_call_array(StradaValue *closure, StradaValue *args_sv);
StradaValue*** strada_closure_get_captures(StradaValue *closure);

/* Closure prototype string accessors (Perl-compat).
 *   - set: copies the C string into the closure (owns the copy).
 *   - get: borrowed pointer to the closure's prototype; NULL if none.
 * Used by perla codegen for `sub (PROTO) { ... }` and by
 * Scalar::Util / prototype(\\&anon) introspection. */
void strada_closure_set_prototype(StradaValue *closure, const char *proto);
const char *strada_closure_get_prototype(StradaValue *closure);

/* Refcounted closure capture cells.
 *
 * Two closures created in the same scope that capture the same lexical share
 * one heap cell — both closures' captures[i] pointers reference the same
 * malloc'd slot so writes are visible across them. Without refcounting, each
 * closure tries to free the cell at teardown → double-free.
 *
 * Layout: the cell is a small header (refcount + slot). Code holds a pointer
 * to &header.slot; arithmetic recovers the header. Initial refcount is 1; the
 * SECOND, THIRD, ... closure sharing the cell calls strada_cell_incref. */
typedef struct PerlaCell {
    int refcount;
    int _pad;
    StradaValue *slot;
} PerlaCell;

StradaValue **strada_cell_alloc(void);
void strada_cell_incref(StradaValue **slot);
int strada_cell_decref(StradaValue **slot);

/* Enhanced FFI */
StradaValue* strada_c_call(const char *func_name, StradaValue **args, int arg_count);
void* strada_dlopen(const char *library);
void* strada_dlsym(void *handle, const char *symbol);
void strada_dlclose(void *handle);

/* Dynamic loading - StradaValue wrappers */
StradaValue* strada_dl_open(StradaValue *library);
StradaValue* strada_dl_sym(StradaValue *handle, StradaValue *symbol);
StradaValue* strada_dl_close(StradaValue *handle);
StradaValue* strada_dl_error(void);
StradaValue* strada_dl_call_int(StradaValue *func_ptr, StradaValue *args);
StradaValue* strada_dl_call_num(StradaValue *func_ptr, StradaValue *args);
StradaValue* strada_dl_call_str(StradaValue *func_ptr, StradaValue *arg);
StradaValue* strada_dl_call_void(StradaValue *func_ptr, StradaValue *args);
StradaValue* strada_dl_call_int_sv(StradaValue *func_ptr, StradaValue *args);
StradaValue* strada_dl_call_str_sv(StradaValue *func_ptr, StradaValue *args);
StradaValue* strada_dl_call_void_sv(StradaValue *func_ptr, StradaValue *args);
StradaValue* strada_dl_call_sv(StradaValue *func_ptr, StradaValue *args);

/* Raw dlopen/dlsym for import_lib compile-time metadata extraction */
StradaValue* strada_dl_open_raw(StradaValue *path);
StradaValue* strada_dl_sym_raw(StradaValue *handle_sv, StradaValue *symbol);
StradaValue* strada_dl_close_raw(StradaValue *handle_sv);
StradaValue* strada_dl_call_export_info(StradaValue *fn_ptr_sv);
StradaValue* strada_dl_call_version(StradaValue *fn_ptr_sv);

/* Pointer access for FFI */
StradaValue* strada_int_ptr(StradaValue *ref);       /* Get pointer to int variable */
StradaValue* strada_num_ptr(StradaValue *ref);       /* Get pointer to num variable */
StradaValue* strada_str_ptr(StradaValue *ref);       /* Get pointer to string data */
StradaValue* strada_ptr_deref_int(StradaValue *ptr); /* Read int from pointer */
StradaValue* strada_ptr_deref_num(StradaValue *ptr); /* Read num from pointer */
StradaValue* strada_ptr_deref_str(StradaValue *ptr); /* Read string from pointer */
StradaValue* strada_ptr_set_int(StradaValue *ptr, StradaValue *val);  /* Write int to pointer */
StradaValue* strada_ptr_set_num(StradaValue *ptr, StradaValue *val);  /* Write num to pointer */

/* C type conversions */
int8_t strada_to_int8(StradaValue *sv);
int16_t strada_to_int16(StradaValue *sv);
int32_t strada_to_int32(StradaValue *sv);
uint8_t strada_to_uint8(StradaValue *sv);
uint16_t strada_to_uint16(StradaValue *sv);
uint32_t strada_to_uint32(StradaValue *sv);
uint64_t strada_to_uint64(StradaValue *sv);
int strada_sv_is_uint(StradaValue *sv);  /* non-negative integer in (INT64_MAX, UINT64_MAX] (UV-flagged int or UV-range string) */
float strada_to_float(StradaValue *sv);
void* strada_to_pointer(StradaValue *sv);

/* Create from C types */
StradaValue* strada_from_int8(int8_t val);
StradaValue* strada_from_int16(int16_t val);
StradaValue* strada_from_int32(int32_t val);
StradaValue* strada_from_uint8(uint8_t val);
StradaValue* strada_from_uint16(uint16_t val);
StradaValue* strada_from_uint32(uint32_t val);
StradaValue* strada_from_uint64(uint64_t val);
StradaValue* strada_from_float(float val);
StradaValue* strada_from_pointer(void *ptr);

/* Regex functions */
StradaValue* strada_regex_compile(const char *pattern, const char *flags);
int strada_regex_match(const char *str, const char *pattern);
int strada_regex_match_with_capture(const char *str, const char *pattern, const char *flags);

/* Tell the regex matchers the subject's byte length up front. Without this
 * they fall back to strlen(str), which silently truncates strings containing
 * NUL bytes. Set BEFORE the regex_match_* call, CLEAR after (or before the
 * next call with a different subject). Thread-local. */
void strada_regex_set_subject_len(size_t n);
void strada_regex_clear_subject_len(void);
void strada_regex_set_subject_for_sv(StradaValue *sv);

StradaValue* strada_captures(void);
StradaValue* strada_capture_var(int n);
StradaValue* strada_last_paren_match(void);
StradaValue* strada_prematch(void);
StradaValue* strada_postmatch(void);
/* @- and @+ — match start/end offset arrays (index 0 = whole match,
 * index N = N-th capture group). Borrowed references; caller increfs. */
StradaValue* strada_match_starts(void);
StradaValue* strada_match_ends(void);
int perl_looks_like_number_c(const char *s);
/* List-context match: returns an array.
 * Match with capture groups → array of capture strings ($1, $2, ...).
 * Match without capture groups → array containing just integer 1.
 * No match → empty array. */
StradaValue* strada_regex_match_list(const char *str, const char *pattern, const char *flags);
StradaValue* strada_regex_match_all(const char *str, const char *pattern);
char* strada_regex_replace(const char *str, const char *pattern, const char *replacement, const char *flags);
char* strada_regex_replace_all(const char *str, const char *pattern, const char *replacement, const char *flags);
int strada_regex_replace_capture(const char *str, const char *pattern, const char *replacement, const char *flags, int global, char **result_out);
StradaArray* strada_string_split(const char *str, const char *delim);
StradaArray* strada_regex_split(const char *str, const char *pattern);
StradaArray* strada_regex_split_limit(const char *str, const char *pattern, int limit);
StradaArray* strada_regex_capture(const char *str, const char *pattern);
StradaValue* strada_named_captures(void);
StradaValue* strada_regex_find_all(const char *str, const char *pattern, const char *flags, int global);
void strada_set_captures_sv(StradaValue *match);
StradaValue* strada_regex_build_result(const char *src, StradaValue *matches, StradaValue *replacements);

/* Socket functions */
StradaValue* strada_socket_create(void);
int strada_socket_connect(StradaValue *sock, const char *host, int port);
int strada_socket_bind(StradaValue *sock, int port);
int strada_socket_listen(StradaValue *sock, int backlog);
StradaValue* strada_socket_accept(StradaValue *sock);
int strada_socket_send(StradaValue *sock, const char *data);
int strada_socket_send_sv(StradaValue *sock, StradaValue *data);  /* Binary-safe version */
StradaValue* strada_socket_recv(StradaValue *sock, int max_len);
StradaValue* strada_socket_close(StradaValue *sock);
StradaValue* strada_socket_flush(StradaValue *sock);  /* Flush write buffer */
StradaValue* strada_socket_server(int port);
StradaValue* strada_socket_server_backlog(int port, int backlog);
StradaValue* strada_socket_server_host(const char *host, int port, int backlog);
StradaValue* strada_socket_client(const char *host, int port);
StradaValue* strada_socket_select(StradaValue *sockets, int timeout_ms);
int strada_socket_fd(StradaValue *sock);
StradaValue* strada_select_fds(StradaValue *fds, int timeout_ms);
int strada_socket_set_nonblocking(StradaValue *sock, int nonblock);

/* UDP socket functions */
StradaValue* strada_udp_socket(void);
int strada_udp_bind(StradaValue *sock, int port);
StradaValue* strada_udp_server(int port);
StradaValue* strada_udp_recvfrom(StradaValue *sock, int max_len);
int strada_udp_sendto(StradaValue *sock, const char *data, int data_len, const char *host, int port);
int strada_udp_sendto_sv(StradaValue *sock, StradaValue *data, const char *host, int port);

/* Memory management */
void strada_free_value(StradaValue *sv);
void strada_free_array(StradaArray *av);
void strada_free_hash(StradaHash *hv);

/* Additional utility functions */
StradaValue* strada_clone(StradaValue *sv);            /* Deep copy */
StradaValue* strada_abs(StradaValue *sv);              /* Absolute value */
StradaValue* strada_sqrt(StradaValue *sv);             /* Square root */
StradaValue* strada_rand(void);                        /* Random 0-1 */
StradaValue* strada_time(void);                        /* Current timestamp */
StradaValue* strada_localtime(StradaValue *timestamp); /* Local time hash */
StradaValue* strada_gmtime(StradaValue *timestamp);    /* UTC time hash */
StradaValue* strada_mktime(StradaValue *time_hash);    /* Hash to timestamp */
StradaValue* strada_strftime(StradaValue *format, StradaValue *time_hash); /* Format time */
StradaValue* strada_ctime(StradaValue *timestamp);     /* Timestamp to string */
StradaValue* strada_sleep(StradaValue *seconds);       /* Sleep seconds */
StradaValue* strada_usleep(StradaValue *usecs);        /* Sleep microseconds */

/* High-resolution time functions */
StradaValue* strada_gettimeofday(void);                /* Get time with usec precision */
StradaValue* strada_hires_time(void);                  /* Get time as float seconds */
StradaValue* strada_tv_interval(StradaValue *start, StradaValue *end); /* Time interval */
StradaValue* strada_nanosleep_ns(StradaValue *nanosecs); /* Sleep nanoseconds */
StradaValue* strada_clock_gettime(StradaValue *clock_id); /* Get clock time */
StradaValue* strada_clock_getres(StradaValue *clock_id);  /* Get clock resolution */

/* CStruct helper functions */
void strada_cstruct_set_int(StradaValue *sv, const char *field, size_t offset, int64_t value);
int64_t strada_cstruct_get_int(StradaValue *sv, const char *field, size_t offset);

/* String helper functions */
char* strada_reverse(const char *str);
char* strada_repeat(const char *str, int count);
char* strada_replace_all(const char *str, const char *find, const char *replace);
char* strada_replace_first(const char *str, const char *find, const char *replace);
void strada_subst_sv(StradaValue **target_ptr, const char *find, const char *replace, int global);

/* Additional CStruct helper functions */
void strada_cstruct_set_int(StradaValue *sv, const char *field, size_t offset, int64_t value);
int64_t strada_cstruct_get_int(StradaValue *sv, const char *field, size_t offset);
char* strada_replace_all(const char *str, const char *find, const char *replace);
char* strada_chomp(const char *str);
char* strada_chop(const char *str);
StradaValue* strada_sv_replace_first(StradaValue *sv, const char *find, const char *replace);
void strada_substr_assign(StradaValue **sv_ptr, int64_t offset, int64_t length, const char *replacement);
int strada_regex_match_global(const char *str, const char *pattern, const char *flags, size_t *pos);
StradaValue* strada_sv_replace_all(StradaValue *sv, const char *find, const char *replace);
StradaValue* strada_concat_cstr_sv(const char *prefix, size_t prefix_len, StradaValue *b);
/* Multi-part concat (codegen-emitted for `.` chains / string interpolation).
 * Varargs groups of (int kind, payload): 0 = ASCII literal (const char*,
 * size_t), 2 = non-ASCII literal (same), 1 = StradaValue*. */
StradaValue* strada_concat_multi(int nparts, ...);
void strada_cstruct_set_string(StradaValue *sv, const char *field, size_t offset, const char *value);
void strada_cstruct_set_double(StradaValue *sv, const char *field, size_t offset, double value);
char* strada_cstruct_get_string(StradaValue *sv, const char *field, size_t offset);
double strada_cstruct_get_double(StradaValue *sv, const char *field, size_t offset);
char* strada_cstruct_get_string(StradaValue *sv, const char *field, size_t offset);
double strada_cstruct_get_double(StradaValue *sv, const char *field, size_t offset);

/* C string helpers for extern functions */
char* strada_cstr_concat(const char *a, const char *b);  /* Concatenate two C strings, returns malloc'd string */
char* strada_int_to_cstr(int64_t n);                     /* Convert int to C string, returns malloc'd string */
char* strada_num_to_cstr(double n);                      /* Convert double to C string, returns malloc'd string */

/* Process control functions */
StradaValue* strada_sleep(StradaValue *seconds);
StradaValue* strada_usleep(StradaValue *microseconds);
StradaValue* strada_fork(void);
StradaValue* strada_wait(void);
StradaValue* strada_waitpid(StradaValue *pid, StradaValue *options);
StradaValue* strada_getpid(void);
StradaValue* strada_getppid(void);
StradaValue* strada_exit_status(StradaValue *status);

/* POSIX functions */
StradaValue* strada_getenv(StradaValue *name);
StradaValue* strada_setenv(StradaValue *name, StradaValue *value);
StradaValue* strada_unsetenv(StradaValue *name);
StradaValue* strada_getcwd(void);
StradaValue* strada_chdir(StradaValue *path);
StradaValue* strada_chroot(StradaValue *path);
StradaValue* strada_mkdir(StradaValue *path, StradaValue *mode);
StradaValue* strada_rmdir(StradaValue *path);
StradaValue* strada_unlink(StradaValue *path);
StradaValue* strada_link(StradaValue *oldpath, StradaValue *newpath);
StradaValue* strada_symlink(StradaValue *target, StradaValue *linkpath);
StradaValue* strada_readlink(StradaValue *path);
StradaValue* strada_rename(StradaValue *oldpath, StradaValue *newpath);
StradaValue* strada_chmod(StradaValue *path, StradaValue *mode);
StradaValue* strada_access(StradaValue *path, StradaValue *mode);
StradaValue* strada_umask(StradaValue *mask);
StradaValue* strada_getuid(void);
StradaValue* strada_geteuid(void);
StradaValue* strada_getgid(void);
StradaValue* strada_getegid(void);
StradaValue* strada_kill(StradaValue *pid, StradaValue *sig);
StradaValue* strada_alarm(StradaValue *seconds);
StradaValue* strada_signal(StradaValue *sig_name, StradaValue *handler);
StradaValue* strada_stat(StradaValue *path);
StradaValue* strada_lstat(StradaValue *path);
StradaValue* strada_isatty(StradaValue *fd);
StradaValue* strada_strerror(StradaValue *errnum);
StradaValue* strada_errno(void);

/* Pipe and IPC functions */
StradaValue* strada_pipe(void);
StradaValue* strada_dup2(StradaValue *oldfd, StradaValue *newfd);
StradaValue* strada_close_fd(StradaValue *fd);
StradaValue* strada_exec(StradaValue *cmd);
StradaValue* strada_exec_argv(StradaValue *program, StradaValue *args);
StradaValue* strada_system(StradaValue *cmd);
StradaValue* strada_system_argv(StradaValue *program, StradaValue *args);
StradaValue* strada_read_fd(StradaValue *fd, StradaValue *size);
StradaValue* strada_open_fd(StradaValue *filename, StradaValue *mode);
StradaValue* strada_write_fd(StradaValue *fd, StradaValue *data);
StradaValue* strada_read_all_fd(StradaValue *fd);
StradaValue* strada_fdopen_read(StradaValue *fd);
StradaValue* strada_fdopen_write(StradaValue *fd);

/* Process name functions */
StradaValue* strada_setprocname(StradaValue *name);
StradaValue* strada_getprocname(void);
void strada_init_proctitle(int argc, char **argv);
StradaValue* strada_setproctitle(StradaValue *title);
StradaValue* strada_getproctitle(void);

/* Directory functions */
StradaValue* strada_readdir(StradaValue *path);
StradaValue* strada_readdir_full(StradaValue *path);
StradaValue* strada_is_dir(StradaValue *path);
StradaValue* strada_is_file(StradaValue *path);
StradaValue* strada_file_is_text(StradaValue *path);
StradaValue* strada_file_is_binary(StradaValue *path);
StradaValue* strada_is_readable(StradaValue *path);
StradaValue* strada_is_writable(StradaValue *path);
StradaValue* strada_is_executable(StradaValue *path);
StradaValue* strada_is_zero_size(StradaValue *path);
StradaValue* strada_is_symlink(StradaValue *path);
StradaValue* strada_file_size(StradaValue *path);

/* Math functions */
StradaValue* strada_sin(StradaValue *x);
StradaValue* strada_cos(StradaValue *x);
StradaValue* strada_tan(StradaValue *x);
StradaValue* strada_asin(StradaValue *x);
StradaValue* strada_acos(StradaValue *x);
StradaValue* strada_atan(StradaValue *x);
StradaValue* strada_atan2(StradaValue *y, StradaValue *x);
StradaValue* strada_log(StradaValue *x);
StradaValue* strada_log10(StradaValue *x);
StradaValue* strada_exp(StradaValue *x);
StradaValue* strada_pow(StradaValue *base, StradaValue *exponent);
StradaValue* strada_floor(StradaValue *x);
StradaValue* strada_ceil(StradaValue *x);
StradaValue* strada_round(StradaValue *x);
StradaValue* strada_fabs(StradaValue *x);
StradaValue* strada_fmod(StradaValue *x, StradaValue *y);
StradaValue* strada_sinh(StradaValue *x);
StradaValue* strada_cosh(StradaValue *x);
StradaValue* strada_tanh(StradaValue *x);

/* File seek functions */
StradaValue* strada_seek(StradaValue *fh, StradaValue *offset, StradaValue *whence);
StradaValue* strada_tell(StradaValue *fh);
StradaValue* strada_rewind(StradaValue *fh);
StradaValue* strada_eof(StradaValue *fh);
StradaValue* strada_flush(StradaValue *fh);
StradaValue* strada_autoflush(StradaValue *fh, StradaValue *flag);

/* DNS/Hostname functions */
StradaValue* strada_gethostbyname(StradaValue *hostname);
StradaValue* strada_gethostbyname_all(StradaValue *hostname);
StradaValue* strada_gethostname(void);
StradaValue* strada_getaddrinfo_first(StradaValue *hostname, StradaValue *service);

/* Path functions */
StradaValue* strada_realpath(StradaValue *path);
StradaValue* strada_dirname(StradaValue *path);
StradaValue* strada_basename(StradaValue *path);
StradaValue* strada_glob(StradaValue *pattern);
StradaValue* strada_fnmatch(StradaValue *pattern, StradaValue *string);
StradaValue* strada_file_ext(StradaValue *path);
StradaValue* strada_path_join(StradaValue *parts);

/* ============================================================ */
/* NEW LIBC FUNCTIONS                                           */
/* ============================================================ */

/* Additional File I/O */
StradaValue* strada_fgetc(StradaValue *fh);
StradaValue* strada_fputc(StradaValue *ch, StradaValue *fh);
StradaValue* strada_fgets(StradaValue *fh, StradaValue *size);
StradaValue* strada_fputs(StradaValue *str, StradaValue *fh);
StradaValue* strada_ferror(StradaValue *fh);
StradaValue* strada_fileno(StradaValue *fh);
StradaValue* strada_clearerr(StradaValue *fh);

/* Temporary files */
StradaValue* strada_tmpfile(void);
StradaValue* strada_mkstemp(StradaValue *template);
StradaValue* strada_mkdtemp(StradaValue *template);

/* Command execution (popen) */
StradaValue* strada_popen(StradaValue *cmd, StradaValue *mode);
StradaValue* strada_pclose(StradaValue *fh);
StradaValue* strada_qx(StradaValue *cmd);  /* Backtick/qx - run command and capture output */
extern int strada_last_qx_status;  /* wait status from most recent strada_qx (pclose return) */

/* Aliases for bootstrap compiler compatibility (sys::foo -> sys_foo) */
StradaValue* sys_system(StradaValue *cmd);
StradaValue* sys_qx(StradaValue *cmd);
StradaValue* sys_unlink(StradaValue *path);

/* Additional file system */
StradaValue* strada_truncate(StradaValue *path, StradaValue *length);
StradaValue* strada_ftruncate(StradaValue *fd, StradaValue *length);
StradaValue* strada_chown(StradaValue *path, StradaValue *uid, StradaValue *gid);
StradaValue* strada_lchown(StradaValue *path, StradaValue *uid, StradaValue *gid);
StradaValue* strada_fchmod(StradaValue *fd, StradaValue *mode);
StradaValue* strada_fchown(StradaValue *fd, StradaValue *uid, StradaValue *gid);
StradaValue* strada_utime(StradaValue *path, StradaValue *atime, StradaValue *mtime);
StradaValue* strada_utimes(StradaValue *path, StradaValue *atime, StradaValue *mtime);

/* Session/process group control */
StradaValue* strada_setsid(void);
StradaValue* strada_getsid(StradaValue *pid);
StradaValue* strada_setpgid(StradaValue *pid, StradaValue *pgid);
StradaValue* strada_getpgid(StradaValue *pid);
StradaValue* strada_getpgrp(void);
StradaValue* strada_setpgrp(void);

/* User/group ID control */
StradaValue* strada_setuid(StradaValue *uid);
StradaValue* strada_setgid(StradaValue *gid);
StradaValue* strada_seteuid(StradaValue *uid);
StradaValue* strada_setegid(StradaValue *gid);
StradaValue* strada_setreuid(StradaValue *ruid, StradaValue *euid);
StradaValue* strada_setregid(StradaValue *rgid, StradaValue *egid);

/* Additional socket operations */
StradaValue* strada_setsockopt(StradaValue *sock, StradaValue *level, StradaValue *optname, StradaValue *optval);
StradaValue* strada_getsockopt(StradaValue *sock, StradaValue *level, StradaValue *optname);
StradaValue* strada_shutdown(StradaValue *sock, StradaValue *how);
StradaValue* strada_getpeername(StradaValue *sock);
StradaValue* strada_getsockname(StradaValue *sock);
StradaValue* strada_inet_pton(StradaValue *af, StradaValue *src);
StradaValue* strada_inet_ntop(StradaValue *af, StradaValue *src);
StradaValue* strada_inet_addr(StradaValue *cp);
StradaValue* strada_inet_ntoa(StradaValue *in);
StradaValue* strada_htons(StradaValue *hostshort);
StradaValue* strada_htonl(StradaValue *hostlong);
StradaValue* strada_ntohs(StradaValue *netshort);
StradaValue* strada_ntohl(StradaValue *netlong);
StradaValue* strada_poll(StradaValue *fds, StradaValue *timeout);

/* Random */
StradaValue* strada_srand(StradaValue *seed);
StradaValue* strada_srandom(StradaValue *seed);
StradaValue* strada_libc_rand(void);
StradaValue* strada_libc_random(void);
StradaValue* strada_random_bytes(StradaValue *num_bytes);
StradaValue* strada_random_bytes_hex(StradaValue *num_bytes);

/* Advanced signals */
StradaValue* strada_sigprocmask(StradaValue *how, StradaValue *set);
StradaValue* strada_raise(StradaValue *sig);
StradaValue* strada_killpg(StradaValue *pgrp, StradaValue *sig);
StradaValue* strada_pause(void);

/* User/Group database */
StradaValue* strada_getpwnam(StradaValue *name);
StradaValue* strada_getpwuid(StradaValue *uid);
StradaValue* strada_getgrnam(StradaValue *name);
StradaValue* strada_getgrgid(StradaValue *gid);
StradaValue* strada_getlogin(void);
StradaValue* strada_getgroups(void);

/* Resource/Priority */
StradaValue* strada_nice(StradaValue *inc);
StradaValue* strada_getpriority(StradaValue *which, StradaValue *who);
StradaValue* strada_setpriority(StradaValue *which, StradaValue *who, StradaValue *prio);
StradaValue* strada_getrusage(StradaValue *who);
StradaValue* strada_getrlimit(StradaValue *resource);
StradaValue* strada_setrlimit(StradaValue *resource, StradaValue *rlim);

/* Additional time functions */
StradaValue* strada_difftime(StradaValue *t1, StradaValue *t0);
StradaValue* strada_clock(void);
StradaValue* strada_times(void);

/* Additional memory functions */
StradaValue* strada_calloc(StradaValue *nmemb, StradaValue *size);
StradaValue* strada_realloc(StradaValue *ptr, StradaValue *size);
StradaValue* strada_mmap(StradaValue *addr, StradaValue *length, StradaValue *prot, StradaValue *flags, StradaValue *fd, StradaValue *offset);
StradaValue* strada_munmap(StradaValue *addr, StradaValue *length);
StradaValue* strada_mlock(StradaValue *addr, StradaValue *len);
StradaValue* strada_munlock(StradaValue *addr, StradaValue *len);

/* String conversion */
StradaValue* strada_strtol(StradaValue *str, StradaValue *base);
StradaValue* strada_strtod(StradaValue *str);
StradaValue* strada_atoi(StradaValue *str);
StradaValue* strada_atof(StradaValue *str);

/* Terminal/TTY */
StradaValue* strada_ttyname(StradaValue *fd);
StradaValue* strada_tcgetattr(StradaValue *fd);
StradaValue* strada_tcsetattr(StradaValue *fd, StradaValue *when, StradaValue *attrs);
StradaValue* strada_cfgetospeed(StradaValue *termios);
StradaValue* strada_cfsetospeed(StradaValue *termios, StradaValue *speed);
StradaValue* strada_cfgetispeed(StradaValue *termios);
StradaValue* strada_cfsetispeed(StradaValue *termios, StradaValue *speed);
StradaValue* strada_serial_open(const char *device, int baud, const char *config);
StradaValue* strada_tcflush(StradaValue *fd, StradaValue *queue);
StradaValue* strada_tcdrain(StradaValue *fd);

/* High-level terminal mode */
StradaValue* strada_term_enable_raw(void);
StradaValue* strada_term_disable_raw(void);
StradaValue* strada_term_rows(void);
StradaValue* strada_term_cols(void);
StradaValue* strada_read_byte(StradaValue *fd);

/* Directory iteration */
StradaValue* strada_opendir(StradaValue *path);
StradaValue* strada_readdir_next(StradaValue *dh);
StradaValue* strada_closedir(StradaValue *dh);

/* Advanced file operations */
StradaValue* strada_fcntl(StradaValue *fd, StradaValue *cmd, StradaValue *arg);
StradaValue* strada_flock(StradaValue *fd, StradaValue *operation);
StradaValue* strada_quotemeta(StradaValue *str);
StradaValue* strada_ioctl(StradaValue *fd, StradaValue *request, StradaValue *arg);
StradaValue* strada_statvfs(StradaValue *path);
StradaValue* strada_fstatvfs(StradaValue *fd);
StradaValue* strada_dup(StradaValue *oldfd);

/* Additional math functions */
StradaValue* strada_hypot(StradaValue *x, StradaValue *y);
StradaValue* strada_cbrt(StradaValue *x);
StradaValue* strada_isnan(StradaValue *x);
StradaValue* strada_isinf(StradaValue *x);
StradaValue* strada_isfinite(StradaValue *x);
StradaValue* strada_fmax(StradaValue *x, StradaValue *y);
StradaValue* strada_fmin(StradaValue *x, StradaValue *y);
StradaValue* strada_copysign(StradaValue *x, StradaValue *y);
StradaValue* strada_remainder(StradaValue *x, StradaValue *y);
StradaValue* strada_trunc(StradaValue *x);
StradaValue* strada_ldexp(StradaValue *x, StradaValue *exp);
StradaValue* strada_frexp(StradaValue *x);
StradaValue* strada_modf(StradaValue *x);
StradaValue* strada_scalbn(StradaValue *x, StradaValue *n);

/* Thread functions */
StradaValue* strada_thread_create(StradaValue *closure);
StradaValue* strada_thread_join(StradaValue *thread_val);
StradaValue* strada_thread_detach(StradaValue *thread_val);
StradaValue* strada_thread_self(void);

/* Mutex functions */
StradaValue* strada_mutex_new(void);
StradaValue* strada_mutex_lock(StradaValue *mutex);
StradaValue* strada_mutex_trylock(StradaValue *mutex);
StradaValue* strada_mutex_unlock(StradaValue *mutex);
StradaValue* strada_mutex_destroy(StradaValue *mutex);

/* Condition variable functions */
StradaValue* strada_cond_new(void);
StradaValue* strada_cond_wait(StradaValue *cond, StradaValue *mutex);
StradaValue* strada_cond_signal(StradaValue *cond);
StradaValue* strada_cond_broadcast(StradaValue *cond);
StradaValue* strada_cond_destroy(StradaValue *cond);

/* ============================================================
 * C Interop Helper Functions (c:: namespace)
 * For explicit memory management with extern "C" functions
 * ============================================================ */
StradaValue* strada_c_str_to_ptr(StradaValue *sv);           /* c::str_to_ptr - Strada string to C char* (allocs) */
StradaValue* strada_c_ptr_to_str(StradaValue *ptr_sv);       /* c::ptr_to_str - C char* to Strada string (copies) */
StradaValue* strada_c_ptr_to_str_n(StradaValue *ptr_sv, StradaValue *len_sv); /* c::ptr_to_str_n - with length */
StradaValue* strada_c_free(StradaValue *ptr_sv);             /* c::free - Free C-allocated memory */
StradaValue* strada_c_alloc(StradaValue *size_sv);           /* c::alloc - Allocate memory (malloc) */
/* c::callback — libffi trampoline minting a C function pointer (returned as
 * an int address) that invokes a Strada closure. Signature strings:
 * ret = void/int/int32/num/ptr; args = comma list of int/int32/num/ptr/str.
 * Dies at runtime when built without libffi. */
StradaValue* strada_ffi_callback_new(StradaValue *closure, StradaValue *ret_sv, StradaValue *args_sv);
StradaValue* strada_ffi_callback_free(StradaValue *cb_sv);
StradaValue* strada_c_realloc(StradaValue *ptr_sv, StradaValue *size_sv); /* c::realloc */
StradaValue* strada_c_null(void);                            /* c::null - Return NULL pointer */
StradaValue* strada_c_is_null(StradaValue *ptr_sv);          /* c::is_null - Check if NULL */
StradaValue* strada_c_ptr_add(StradaValue *ptr_sv, StradaValue *offset_sv); /* c::ptr_add - Pointer arithmetic */

/* c:: memory read functions */
StradaValue* strada_c_read_int8(StradaValue *ptr_sv);
StradaValue* strada_c_read_int16(StradaValue *ptr_sv);
StradaValue* strada_c_read_int32(StradaValue *ptr_sv);
StradaValue* strada_c_read_int64(StradaValue *ptr_sv);
StradaValue* strada_c_read_ptr(StradaValue *ptr_sv);
StradaValue* strada_c_read_float(StradaValue *ptr_sv);
StradaValue* strada_c_read_double(StradaValue *ptr_sv);

/* c:: memory write functions */
StradaValue* strada_c_write_int8(StradaValue *ptr_sv, StradaValue *val_sv);
StradaValue* strada_c_write_int16(StradaValue *ptr_sv, StradaValue *val_sv);
StradaValue* strada_c_write_int32(StradaValue *ptr_sv, StradaValue *val_sv);
StradaValue* strada_c_write_int64(StradaValue *ptr_sv, StradaValue *val_sv);
StradaValue* strada_c_write_ptr(StradaValue *ptr_sv, StradaValue *val_sv);
StradaValue* strada_c_write_float(StradaValue *ptr_sv, StradaValue *val_sv);
StradaValue* strada_c_write_double(StradaValue *ptr_sv, StradaValue *val_sv);

/* c:: size introspection */
StradaValue* strada_c_sizeof_int(void);
StradaValue* strada_c_sizeof_long(void);
StradaValue* strada_c_sizeof_ptr(void);
StradaValue* strada_c_sizeof_size_t(void);

/* c:: memory operations */
StradaValue* strada_c_memcpy(StradaValue *dest_sv, StradaValue *src_sv, StradaValue *n_sv);
StradaValue* strada_c_memset(StradaValue *dest_sv, StradaValue *c_sv, StradaValue *n_sv);

/* ============================================================
 * Profiling - Function timing and call counts
 * ============================================================ */
void strada_profile_init(void);
void strada_profile_enter(const char *func_name);
void strada_profile_exit(const char *func_name);
void strada_profile_report(void);

/* ============================================================
 * Full Profiling - Line-level timing (NYTProf-style)
 * ============================================================ */
void strada_full_profile_init(const char *output_file);
int strada_full_profile_register_file(const char *filename);
void strada_full_profile_line(int file_id, int line_no);
void strada_full_profile_enter(const char *func_name);
void strada_full_profile_exit(const char *func_name);
void strada_full_profile_start(const char *output_file);
void strada_full_profile_stop(void);
void strada_full_profile_write(void);

/* ============================================================
 * Global Variable Registry - Shared across all modules
 * ============================================================ */
void strada_global_set(StradaValue *name, StradaValue *val);
void strada_global_set_cstr(const char *name, StradaValue *val);
StradaValue* strada_global_get(StradaValue *name);
int strada_global_exists(StradaValue *name);
void strada_global_delete(StradaValue *name);
StradaValue* strada_global_keys(void);

/* ============================================================
 * Memory Profiler - Track allocations by type
 * ============================================================ */
void strada_memprof_enable(void);
void strada_memprof_disable(void);
void strada_memprof_report(void);
void strada_memprof_reset(void);

/* ============================================================
 * String Repetition (x operator)
 * ============================================================ */
StradaValue* strada_string_repeat(StradaValue *sv, int64_t count);

/* ============================================================
 * Array splice
 * ============================================================ */
StradaValue* strada_array_splice_sv(StradaValue *arr_sv, int64_t offset, int64_t length, StradaValue *repl_sv);

/* ============================================================
 * Hash each() iterator
 * ============================================================ */
StradaValue* strada_hash_each(StradaHash *hv);
void strada_hash_reset_iter(StradaHash *hv);

/* ============================================================
 * select() - Default output filehandle
 * ============================================================ */
StradaValue* strada_select(StradaValue *fh);
StradaValue* strada_select_get(void);

/* ============================================================
 * Transliteration (tr///)
 * ============================================================ */
StradaValue* strada_tr(StradaValue *sv, const char *search, const char *replace, const char *flags);

/* ============================================================
 * local() - Dynamic scoping for our variables
 * ============================================================ */
#define STRADA_LOCAL_STACK_MAX 256
typedef struct {
    char *name;           /* For our-variable saves (global registry key) */
    StradaValue *saved_value;
    StradaValue *hash_ref; /* For hash element saves (NULL = our-variable save) */
    char *hash_key;        /* Hash key for element saves */
} StradaLocalSave;

void strada_local_save(const char *name);
void strada_local_save_hash_elem(StradaValue *hash, const char *key);
void strada_local_restore(void);
void strada_local_restore_n(int n);
int strada_local_depth_get(void);
void strada_local_restore_to(int depth);

/* ============================================================
 * tie/untie/tied - Tied variable support
 * ============================================================ */
/* Wrapper functions with __builtin_expect for zero overhead on untied vars */
static inline StradaValue* strada_hv_fetch(StradaValue *sv, const char *key);
static inline StradaValue* strada_hv_fetch_owned(StradaValue *sv, const char *key);
static inline void strada_hv_store(StradaValue *sv, const char *key, StradaValue *val);
static inline int strada_hv_exists(StradaValue *sv, const char *key);
static inline void strada_hv_delete(StradaValue *sv, const char *key);

/* Tied dispatch functions */
StradaValue* strada_tied_hash_fetch(StradaValue *sv, const char *key);
void strada_tied_hash_store(StradaValue *sv, const char *key, StradaValue *val);
int strada_tied_hash_exists(StradaValue *sv, const char *key);
void strada_tied_hash_delete(StradaValue *sv, const char *key);
StradaValue* strada_tied_hash_firstkey(StradaValue *sv);
StradaValue* strada_tied_hash_nextkey(StradaValue *sv, const char *lastkey);
void strada_tied_hash_clear(StradaValue *sv);
StradaValue* strada_tied_hash_scalar(StradaValue *sv);
StradaValue* strada_tied_scalar_fetch(StradaValue *sv);
int strada_tied_scalar_store(StradaValue *sv, StradaValue *val);

/* Tied-array dispatch — invokes TIEARRAY methods on the tied object.
 * Each helper no-ops or returns undef/0 on non-tied SVs. */
StradaValue* strada_tied_array_fetch(StradaValue *sv, int64_t idx);
void         strada_tied_array_store(StradaValue *sv, int64_t idx, StradaValue *val);
int64_t      strada_tied_array_fetchsize(StradaValue *sv);
void         strada_tied_array_storesize(StradaValue *sv, int64_t n);
StradaValue* strada_tied_array_pop(StradaValue *sv);
StradaValue* strada_tied_array_shift(StradaValue *sv);
int64_t      strada_tied_array_push(StradaValue *sv, StradaValue *vals_array);
int64_t      strada_tied_array_unshift(StradaValue *sv, StradaValue *vals_array);
int          strada_tied_array_exists(StradaValue *sv, int64_t idx);
void         strada_tied_array_delete(StradaValue *sv, int64_t idx);
void         strada_tied_array_clear(StradaValue *sv);

/* Tied-aware array accessors. Use these instead of raw strada_array_get /
 * strada_array_push when operating on an SV that might be tied — they
 * dispatch FETCH/STORE/PUSH/etc. when sv->meta->is_tied. */
static inline StradaValue* strada_av_fetch_sv(StradaValue *sv, int64_t idx) {
    if (sv && !STRADA_IS_TAGGED_INT(sv) && sv->meta && sv->meta->is_tied) {
        return strada_tied_array_fetch(sv, idx);
    }
    StradaArray *av = strada_deref_array(sv);
    StradaValue *el = strada_array_get(av, (int)idx);
    if (el) strada_incref(el);
    return el ? el : strada_new_undef();
}
static inline void strada_av_store_sv(StradaValue *sv, int64_t idx, StradaValue *val) {
    if (sv && !STRADA_IS_TAGGED_INT(sv) && sv->meta && sv->meta->is_tied) {
        strada_tied_array_store(sv, idx, val);
        return;
    }
    StradaArray *av = strada_deref_array(sv);
    if (val) strada_incref(val);
    strada_array_set(av, (int)idx, val);
}
static inline int64_t strada_av_fetchsize_sv(StradaValue *sv) {
    if (sv && !STRADA_IS_TAGGED_INT(sv) && sv->meta && sv->meta->is_tied) {
        return strada_tied_array_fetchsize(sv);
    }
    StradaArray *av = strada_deref_array(sv);
    return av ? (int64_t)strada_array_length(av) : 0;
}
static inline int strada_av_exists_sv(StradaValue *sv, int64_t idx) {
    if (sv && !STRADA_IS_TAGGED_INT(sv) && sv->meta && sv->meta->is_tied) {
        return strada_tied_array_exists(sv, idx);
    }
    StradaArray *av = strada_deref_array(sv);
    if (!av) return 0;
    int64_t len = (int64_t)strada_array_length(av);
    if (idx < 0) idx += len;
    if (idx < 0 || idx >= len) return 0;
    StradaValue *el = av->elements[av->head + idx];
    return el != NULL;
}
static inline void strada_av_delete_sv(StradaValue *sv, int64_t idx) {
    if (sv && !STRADA_IS_TAGGED_INT(sv) && sv->meta && sv->meta->is_tied) {
        strada_tied_array_delete(sv, idx);
        return;
    }
    StradaArray *av = strada_deref_array(sv);
    strada_array_delete_idx(av, (int)idx);
}

/* Attach a metadata block to an SV if absent. Public so perla can set
 * is_tied/tied_obj on scalars without re-implementing the meta pool. */
struct StradaMetadata *strada_ensure_meta(StradaValue *sv);

/* Pluggable method-dispatch hook for tie callbacks. Perla sets this to
 * perla_method_dispatch so user-defined TIESCALAR/FETCH/STORE — which
 * live in perla's stash, not strada's OOP — are actually found by
 * strada's tied_method_call dispatch. NULL = use strada_method_call. */
extern StradaValue* (*strada_method_dispatch_hook)(StradaValue *obj, const char *method, StradaValue *args);
extern int (*strada_method_can_hook)(StradaValue *obj, const char *method);

/* tie/untie/tied built-ins */
StradaValue* strada_tie_hash(StradaValue *ref, const char *classname, int argc, ...);
StradaValue* strada_tie_array(StradaValue *ref, const char *classname, int argc, ...);
StradaValue* strada_tie_scalar(StradaValue *ref, const char *classname, int argc, ...);
void strada_untie(StradaValue *ref);
StradaValue* strada_tied(StradaValue *ref);

/* Inline wrapper implementations */
static inline StradaValue* strada_hv_fetch(StradaValue *sv, const char *key) {
    if (STRADA_IS_TAGGED_INT(sv)) return strada_undef_static();
    if (__builtin_expect(sv->meta && sv->meta->is_tied, 0)) return strada_tied_hash_fetch(sv, key);
    return strada_hash_get(strada_deref_hash(sv), key);
}
StradaValue* strada_hash_get_with_hash(StradaHash *hv, const char *key, unsigned int hash);
static inline StradaValue* strada_hv_fetch_owned(StradaValue *sv, const char *key) {
    if (!sv || STRADA_IS_TAGGED_INT(sv)) return strada_undef_static();
    if (__builtin_expect(sv->meta && sv->meta->is_tied, 0)) return strada_tied_hash_fetch(sv, key);
    StradaValue *result = strada_hash_get(strada_deref_hash(sv), key);
    strada_incref(result);
    return result;
}
/* Pre-hashed fetch: caller provides DJB2 hash of key — skips re-hashing */
static inline StradaValue* strada_hv_fetch_owned_ph(StradaValue *sv, const char *key, unsigned int hash) {
    if (STRADA_IS_TAGGED_INT(sv)) return strada_undef_static();
    if (__builtin_expect(sv->meta && sv->meta->is_tied, 0)) return strada_tied_hash_fetch(sv, key);
    StradaValue *result = strada_hash_get_with_hash(strada_deref_hash(sv), key, hash);
    strada_incref(result);
    return result;
}
/* Pre-hashed int fetch: returns int64_t directly, skipping StradaValue temp +
 * incref/decref. Used by inline int accessors (e.g. $self->x()) where the
 * accessor's has-attr is declared int. Returns 0 for missing/undef keys.
 *
 * Hot-path streamlined: probe the first bucket inline. The vast majority
 * of fetches on a clean (no-tombstones, no-index-dirty) hash hit on the
 * first probe — we can check that without leaving the inline body, which
 * lets gcc keep the whole `$self->x()` chain in registers under -O3+LTO.
 * Falls back to strada_hash_get_with_hash for collisions / tombstones /
 * dirty index, which is far slower but rare. */
static inline __attribute__((always_inline)) int64_t strada_hv_fetch_int_ph(StradaValue *sv, const char *key, unsigned int hash) {
    if (STRADA_IS_TAGGED_INT(sv)) return 0;
    if (__builtin_expect(sv->meta && sv->meta->is_tied, 0)) {
        StradaValue *r = strada_tied_hash_fetch(sv, key);
        int64_t v = strada_to_int(r);
        strada_decref(r);
        return v;
    }
    StradaHash *hv = strada_deref_hash(sv);
    if (__builtin_expect(hv != NULL, 1)) {
        uint32_t pos = hash & (hv->num_buckets - 1);
        uint32_t idx = hv->hash_index[pos];
        if (__builtin_expect(idx < HASH_TOMBSTONE, 1)) {
            StradaHashEntry *e = &hv->entries[idx];
            /* Hash alone is not sufficient — DJB2 is 32-bit and two
             * distinct attribute names can collide; we also need to
             * confirm the key content matches. strcmp on short attribute
             * names lowers to a handful of cmp/branch insns when `key`
             * is a string literal, so the fast path stays cheap. */
            if (__builtin_expect(e->key && e->key->hash == hash && strcmp(e->key->data, key) == 0, 1)) {
                return strada_to_int(e->value);
            }
        }
    }
    /* Slow path: collision, tombstone, or dirty index. */
    StradaValue *v = strada_hash_get_with_hash(hv, key, hash);
    return strada_to_int(v);
}
/* Hash::Util::lock_keys check: if locked, reject inserts of keys not
 * in the allowed set. Existing keys may still be modified. Returns 1
 * if the operation is allowed, 0 if locked and rejected. On rejection
 * issues strada_die with Perl's canonical error format. */
static inline int strada_hv_check_lock(StradaValue *sv, const char *key) {
    if (!sv || STRADA_IS_TAGGED_INT(sv) || !sv->meta || !sv->meta->is_hash_locked) return 1;
    /* Existing key — always allowed. */
    StradaHash *hv = strada_deref_hash(sv);
    if (hv && strada_hash_exists(hv, key)) return 1;
    /* No allowed-keys set means lock-with-existing-keys-only (the
     * `lock_keys(%h)` form). Existing keys checked above; everything
     * else is rejected. */
    if (!sv->meta->locked_keys) {
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "Attempt to access disallowed key '%s' in a restricted hash",
                 key);
        strada_die(msg);
        return 0;
    }
    /* Check against the allowed-keys array (the `lock_keys(%h, @allowed)`
     * form). Linear walk; the allowed set is typically small. */
    StradaArray *ak = (StradaArray *)sv->meta->locked_keys;
    if (ak) {
        for (size_t i = 0; i < ak->size; i++) {
            StradaValue *e = ak->elements[ak->head + i];
            if (e && !STRADA_IS_TAGGED_INT(e) && e->type == STRADA_STR
                && e->value.pv && strcmp(e->value.pv, key) == 0) return 1;
        }
    }
    char msg[256];
    snprintf(msg, sizeof(msg),
             "Attempt to access disallowed key '%s' in a restricted hash",
             key);
    strada_die(msg);
    return 0;
}

/* In-place num store for accumulator assignments ($numvar = <numeric> /
 * $numvar += n). When a is the sole owner of a plain heap NUM, write the
 * double into it — no pool traffic, no refcount dispatch; otherwise
 * release a and box fresh. Returns the value to store back into the
 * variable. Shared/tied/weak values (refcount > 1 or meta set) take the
 * fresh-box path, so observable semantics are unchanged. */
static inline StradaValue* strada_num_set_inplace(StradaValue *a, double v) {
    if (a && !STRADA_IS_TAGGED_INT(a) && a->type == STRADA_NUM
        && a->refcount == 1 && !a->meta) {
        a->value.nv = v;
        return a;
    }
    strada_decref(a);
    return strada_new_num(v);
}

static inline void strada_hv_store(StradaValue *sv, const char *key, StradaValue *val) {
    if (!sv || STRADA_IS_TAGGED_INT(sv)) return;
    if (__builtin_expect(sv->meta && sv->meta->is_tied, 0)) { strada_tied_hash_store(sv, key, val); return; }
    if (__builtin_expect(sv->meta && sv->meta->is_hash_locked, 0)) {
        if (!strada_hv_check_lock(sv, key)) return;
    }
    strada_hash_set(strada_deref_hash(sv), key, val);
}
/* _take variant: caller donates ownership of val (no incref) */
static inline void strada_hv_store_take(StradaValue *sv, const char *key, StradaValue *val) {
    if (!sv || STRADA_IS_TAGGED_INT(sv)) return;
    if (__builtin_expect(sv->meta && sv->meta->is_tied, 0)) { strada_tied_hash_store(sv, key, val); return; }
    if (__builtin_expect(sv->meta && sv->meta->is_hash_locked, 0)) {
        if (!strada_hv_check_lock(sv, key)) { strada_decref(val); return; }
    }
    strada_hash_set_take(strada_deref_hash(sv), key, val);
}
static inline int strada_hv_exists(StradaValue *sv, const char *key) {
    if (!sv || STRADA_IS_TAGGED_INT(sv)) return 0;
    if (__builtin_expect(sv->meta && sv->meta->is_tied, 0)) return strada_tied_hash_exists(sv, key);
    return strada_hash_exists(strada_deref_hash(sv), key);
}
static inline void strada_hv_delete(StradaValue *sv, const char *key) {
    if (!sv || STRADA_IS_TAGGED_INT(sv)) return;
    if (__builtin_expect(sv->meta && sv->meta->is_tied, 0)) { strada_tied_hash_delete(sv, key); return; }
    strada_hash_delete(strada_deref_hash(sv), key);
}
/* _sv inline wrappers: accept StradaValue* key directly */
static inline void strada_hv_store_sv(StradaValue *sv, StradaValue *key, StradaValue *val) {
    if (STRADA_IS_TAGGED_INT(sv)) return;
    if (__builtin_expect(sv->meta && sv->meta->is_tied, 0)) { char *ks = strada_to_str(key); strada_tied_hash_store(sv, ks, val); free(ks); return; }
    strada_hash_set_sv(strada_deref_hash(sv), key, val);
}
static inline StradaValue* strada_hv_fetch_owned_sv(StradaValue *sv, StradaValue *key) {
    if (STRADA_IS_TAGGED_INT(sv)) return strada_undef_static();
    if (__builtin_expect(sv->meta && sv->meta->is_tied, 0)) { char *ks = strada_to_str(key); StradaValue *r = strada_tied_hash_fetch(sv, ks); free(ks); return r; }
    StradaValue *result = strada_hash_get_sv(strada_deref_hash(sv), key);
    strada_incref(result);
    return result;
}
static inline int strada_hv_exists_sv(StradaValue *sv, StradaValue *key) {
    if (STRADA_IS_TAGGED_INT(sv)) return 0;
    if (__builtin_expect(sv->meta && sv->meta->is_tied, 0)) { char *ks = strada_to_str(key); int r = strada_tied_hash_exists(sv, ks); free(ks); return r; }
    return strada_hash_exists_sv(strada_deref_hash(sv), key);
}
static inline void strada_hv_delete_sv(StradaValue *sv, StradaValue *key) {
    if (STRADA_IS_TAGGED_INT(sv)) return;
    if (__builtin_expect(sv->meta && sv->meta->is_tied, 0)) { char *ks = strada_to_str(key); strada_tied_hash_delete(sv, ks); free(ks); return; }
    strada_hash_delete_sv(strada_deref_hash(sv), key);
}
/* Tied-aware variant of strada_hash_delete_take_sv: returns the deleted
 * value (caller owns). For tied hashes, FETCH-then-DELETE through the
 * tied vtable so user-defined DELETE handlers still fire. */
static inline StradaValue* strada_hv_delete_take_sv(StradaValue *sv, StradaValue *key) {
    if (STRADA_IS_TAGGED_INT(sv) || !sv) return strada_undef_static();
    if (__builtin_expect(sv->meta && sv->meta->is_tied, 0)) {
        char *ks = strada_to_str(key);
        StradaValue *old = strada_tied_hash_fetch(sv, ks);
        strada_tied_hash_delete(sv, ks);
        free(ks);
        return old ? old : strada_undef_static();
    }
    return strada_hash_delete_take_sv(strada_deref_hash(sv), key);
}

/* Concat key operations: build key from prefix + suffix in stack buffer (zero allocations) */
/* Autovivifying fetch: if key doesn't exist, create empty hash and store it */
static inline StradaValue* strada_hv_autoviv(StradaValue *sv, const char *key) {
    if (STRADA_IS_TAGGED_INT(sv) || !sv) return strada_new_hash();
    StradaValue *result = strada_hash_get(strada_deref_hash(sv), key);
    if (!result || (result->type == STRADA_UNDEF)) {
        result = strada_new_hash();
        strada_hash_set(strada_deref_hash(sv), key, result);
    }
    return result;
}

StradaValue* strada_hv_fetch_owned_concat(StradaValue *sv, const char *prefix, size_t prefix_len, StradaValue *suffix);
void strada_hv_store_concat(StradaValue *sv, const char *prefix, size_t prefix_len, StradaValue *suffix, StradaValue *value);
void strada_hv_delete_concat(StradaValue *sv, const char *prefix, size_t prefix_len, StradaValue *suffix);

/* Single-lookup hash-element compound assignment: $h{k} op= rhs.
 * op is '+', '-', '*', '/' or '.'; rhs is borrowed. One probe instead of
 * the fetch_owned + store pair; tied/locked hashes fall back to
 * FETCH/compute/STORE. _ph takes a precomputed strada_hash_string hash
 * (compile-time djb2 for ASCII literal keys). */
void strada_hv_compound_ph(StradaValue *sv, const char *key, unsigned int hash, StradaValue *rhs, int op);
void strada_hv_compound(StradaValue *sv, const char *key, StradaValue *rhs, int op);
void strada_hv_compound_sv(StradaValue *sv, StradaValue *key_sv, StradaValue *rhs, int op);
/* Single-pass array-element compound assignment ($a[i] op= rhs); rhs borrowed. */
void strada_array_compound(StradaValue *arr_sv, int64_t idx, StradaValue *rhs, int op);

#endif /* STRADA_RUNTIME_H */

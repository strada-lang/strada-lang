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

/* Closure structure */
typedef struct StradaClosure {
    void *func_ptr;           /* Pointer to generated C function */
    int param_count;          /* Number of parameters */
    int capture_count;        /* Number of captured variables */
    StradaValue ***captures;  /* Array of pointers to pointers (capture-by-reference) */
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

/* Main value structure - like Perl's SV */
struct StradaValue {
    StradaType type;
    int refcount;
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
    } value;

    /* C struct metadata (when type == STRADA_CSTRUCT) */
    char *struct_name;   /* Name of C struct type */
    size_t struct_size;  /* Size of struct in bytes */

    /* Blessed package (for OOP - like Perl's bless) */
    char *blessed_package;  /* Package name this ref is blessed into, or NULL */

    /* Tied variable support */
    uint8_t is_tied;           /* 0 = normal, 1 = tied */
    StradaValue *tied_obj;     /* Implementation object (NULL when !is_tied) */

    /* Weak reference support */
    uint8_t is_weak;           /* 0 = strong reference, 1 = weak reference */
};

/* Array structure - like Perl's AV */
struct StradaArray {
    StradaValue **elements;
    size_t size;
    size_t capacity;
    int refcount;
};

/* Hash entry */
typedef struct StradaHashEntry {
    char *key;
    StradaValue *value;
    struct StradaHashEntry *next;
} StradaHashEntry;

/* Hash structure - like Perl's HV */
struct StradaHash {
    StradaHashEntry **buckets;
    size_t num_buckets;
    size_t num_entries;
    int refcount;
    /* Iterator state for each() */
    size_t iter_bucket;
    StradaHashEntry *iter_entry;
};

/* Value creation functions */
StradaValue* strada_new_undef(void);
StradaValue* strada_undef_static(void);  /* Static singleton for void returns */
StradaValue* strada_new_int(int64_t i);
StradaValue* strada_new_num(double n);
StradaValue* strada_safe_div(double a, double b);      /* Returns undef if b==0 */
StradaValue* strada_safe_mod(int64_t a, int64_t b);    /* Returns undef if b==0 */
StradaValue* strada_new_str(const char *s);
StradaValue* strada_new_str_take(char *s);  /* Take ownership of string */
StradaValue* strada_new_str_len(const char *s, size_t len);  /* Binary-safe string */
size_t strada_str_len(StradaValue *sv);  /* Get string length (binary-safe) */
StradaValue* strada_new_array(void);
StradaValue* strada_new_hash(void);
StradaValue* strada_new_filehandle(FILE *fh);

/* Reference counting */
void strada_incref(StradaValue *sv);
void strada_decref(StradaValue *sv);

/* Type conversion */
int64_t strada_to_int(StradaValue *sv);
double strada_to_num(StradaValue *sv);
char* strada_to_str(StradaValue *sv);
int strada_to_bool(StradaValue *sv);

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
StradaValue* strada_array_get_safe(StradaValue *arr, int64_t idx);  /* For destructuring */
void strada_array_set(StradaArray *av, int64_t idx, StradaValue *sv);
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
StradaValue* strada_hash_get(StradaHash *hv, const char *key);
int strada_hash_exists(StradaHash *hv, const char *key);
void strada_hash_delete(StradaHash *hv, const char *key);
StradaArray* strada_hash_keys(StradaHash *hv);
StradaArray* strada_hash_values(StradaHash *hv);
void strada_hash_reserve(StradaHash *hv, size_t num_buckets);
void strada_hash_reserve_sv(StradaValue *sv, int64_t capacity);

/* String operations (UTF-8 aware) */
char* strada_concat(const char *a, const char *b);
char* strada_concat_free(char *a, char *b);  /* Concat and free inputs (avoids leaks) */
StradaValue* strada_concat_sv(StradaValue *a, StradaValue *b);  /* Fast concat on StradaValues */
StradaValue* strada_concat_inplace(StradaValue *a, StradaValue *b);  /* In-place concat: consumes a, borrows b */
size_t strada_length(const char *s);      /* Returns character count (UTF-8 codepoints) */
size_t strada_length_sv(StradaValue *sv); /* Binary-safe length using struct_size */
size_t strada_bytes(const char *s);       /* Returns byte count */
StradaValue* strada_char_at(StradaValue *str, StradaValue *index);  /* Fast char code by index */
StradaValue* strada_substr(StradaValue *str, int64_t offset, int64_t length);
StradaValue* strada_substr_bytes(StradaValue *str, int64_t offset, int64_t length);
int strada_index(const char *haystack, const char *needle);
int strada_index_offset(const char *haystack, const char *needle, int offset);
int strada_rindex(const char *haystack, const char *needle);
char* strada_upper(const char *str);
char* strada_lower(const char *str);
char* strada_uc(const char *str);  /* Alias for upper */
char* strada_lc(const char *str);  /* Alias for lower */
char* strada_ucfirst(const char *str);
char* strada_lcfirst(const char *str);
char* strada_trim(const char *str);
char* strada_ltrim(const char *str);
char* strada_rtrim(const char *str);
char* strada_reverse(const char *str);
StradaValue* strada_reverse_sv(StradaValue *sv);  /* Generic reverse for strings and arrays */
char* strada_repeat(const char *str, int count);
char* strada_chr(int code);
StradaValue* strada_chr_sv(int code);  /* Binary-safe version that handles NUL bytes */
int strada_ord(const char *str);
int strada_ord_byte(StradaValue *sv);  /* Binary-safe: returns raw byte value 0-255 */
int strada_get_byte(StradaValue *sv, int pos);  /* Get byte at position, returns 0-255 or -1 */
StradaValue* strada_set_byte(StradaValue *sv, int pos, int val);  /* Set byte, returns new string */
int strada_byte_length(StradaValue *sv);  /* Get byte length (not UTF-8 char count) */
StradaValue* strada_byte_substr(StradaValue *sv, int start, int len);  /* Substring by byte positions */
StradaValue* strada_pack(const char *fmt, StradaValue *args);  /* Pack values into binary string */
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
void strada_say_fh(StradaValue *sv, StradaValue *fh);
StradaValue* strada_readline(void);
void strada_printf(const char *format, ...);
StradaValue* strada_sprintf(const char *format, ...);
StradaValue* strada_sprintf_sv(StradaValue *format_sv, int arg_count, ...);
void strada_warn(const char *format, ...);

/* File I/O functions */
StradaValue* strada_open(const char *filename, const char *mode);
StradaValue* strada_open_str(const char *content, const char *mode);  /* In-memory I/O */
StradaValue* strada_open_sv(StradaValue *first_arg, StradaValue *mode_arg);  /* Type-dispatch open */
StradaValue* strada_str_from_fh(StradaValue *fh);  /* Extract string from memstream */
void strada_close(StradaValue *fh);
StradaValue* strada_read_file(StradaValue *fh);
StradaValue* strada_read_line(StradaValue *fh);
StradaValue* strada_read_all_lines(StradaValue *fh);
void strada_write_file(StradaValue *fh, const char *content);
int strada_file_exists(const char *filename);
StradaValue* strada_slurp(const char *filename);  /* Read entire file */
StradaValue* strada_slurp_fh(StradaValue *fh_sv);  /* Read from FILE handle to end */
StradaValue* strada_slurp_fd(StradaValue *fd_sv);  /* Read from file descriptor to end */
void strada_spew(const char *filename, const char *content);  /* Write entire file */
void strada_spew_fh(StradaValue *fh_sv, StradaValue *content_sv);  /* Write to FILE handle */
void strada_spew_fd(StradaValue *fd_sv, StradaValue *content_sv);  /* Write to file descriptor */

/* Built-in functions */
void strada_dump(StradaValue *sv, int indent);
void strada_dumper(StradaValue *sv);
StradaValue* strada_dumper_str(StradaValue *sv);  /* Returns dump as string */
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

extern StradaTryContext strada_try_stack[STRADA_MAX_TRY_DEPTH];
extern int strada_try_depth;
extern char *strada_exception_msg;

__attribute__((noreturn)) void strada_throw(const char *msg);
__attribute__((noreturn)) void strada_throw_value(StradaValue *sv);
StradaValue* strada_get_exception(void);
void strada_clear_exception(void);
int strada_in_try_block(void);

/* Pending cleanup for function call args and local vars in try blocks */
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

extern StradaStackFrame strada_call_stack[STRADA_MAX_CALL_DEPTH];
extern int strada_call_depth;
extern int strada_recursion_limit;  /* Configurable limit (default 1000, 0 = disabled) */

void strada_stack_push(const char *func_name, const char *file_name);
void strada_stack_pop(void);
void strada_stack_set_line(int line);
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
StradaValue* strada_ref_deref(StradaValue *ref);      /* Dereference */
int strada_is_ref(StradaValue *sv);                   /* Check if reference */
const char* strada_reftype(StradaValue *ref);         /* Get type of referent */
StradaValue* strada_ref_scalar(StradaValue **ptr);    /* Reference to scalar variable */
StradaValue* strada_ref_array(StradaArray **ptr);     /* Reference to array */
StradaValue* strada_ref_hash(StradaHash **ptr);       /* Reference to hash */

/* New Perl-style reference functions */
StradaValue* strada_new_ref(StradaValue *target, char ref_type);  /* \$var, \@arr, \%hash */
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
StradaValue* strada_blessed(StradaValue *ref);                  /* Get package name or undef */
void strada_set_package(const char *package);                   /* Set current package context */
const char* strada_current_package(void);                       /* Get current package */
void strada_inherit(const char *child, const char *parent);     /* Set up inheritance (2 args) */
void strada_inherit_from(const char *parent);                   /* Inherit from parent (1 arg, uses current package) */
void strada_method_register(const char *package, const char *name, StradaMethod func);
void strada_modifier_register(const char *package, const char *method, int type, StradaMethod func);
StradaValue* strada_method_call(StradaValue *obj, const char *method, StradaValue *args);
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
StradaValue*** strada_closure_get_captures(StradaValue *closure);

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
StradaValue* strada_captures(void);
StradaValue* strada_capture_var(int n);
StradaValue* strada_regex_match_all(const char *str, const char *pattern);
char* strada_regex_replace(const char *str, const char *pattern, const char *replacement, const char *flags);
char* strada_regex_replace_all(const char *str, const char *pattern, const char *replacement, const char *flags);
StradaArray* strada_string_split(const char *str, const char *delim);
StradaArray* strada_regex_split(const char *str, const char *pattern);
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
void strada_socket_close(StradaValue *sock);
void strada_socket_flush(StradaValue *sock);  /* Flush write buffer */
StradaValue* strada_socket_server(int port);
StradaValue* strada_socket_server_backlog(int port, int backlog);
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

/* Additional CStruct helper functions */
void strada_cstruct_set_int(StradaValue *sv, const char *field, size_t offset, int64_t value);
int64_t strada_cstruct_get_int(StradaValue *sv, const char *field, size_t offset);
char* strada_replace_all(const char *str, const char *find, const char *replace);
char* strada_chomp(const char *str);
char* strada_chop(const char *str);
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
 * Global Variable Registry - Shared across all modules
 * ============================================================ */
void strada_global_set(StradaValue *name, StradaValue *val);
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
    char *name;
    StradaValue *saved_value;
} StradaLocalSave;

void strada_local_save(const char *name);
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

/* tie/untie/tied built-ins */
StradaValue* strada_tie_hash(StradaValue *ref, const char *classname, int argc, ...);
StradaValue* strada_tie_array(StradaValue *ref, const char *classname, int argc, ...);
StradaValue* strada_tie_scalar(StradaValue *ref, const char *classname, int argc, ...);
void strada_untie(StradaValue *ref);
StradaValue* strada_tied(StradaValue *ref);

/* Inline wrapper implementations */
static inline StradaValue* strada_hv_fetch(StradaValue *sv, const char *key) {
    if (__builtin_expect(sv->is_tied, 0)) return strada_tied_hash_fetch(sv, key);
    return strada_hash_get(strada_deref_hash(sv), key);
}
static inline StradaValue* strada_hv_fetch_owned(StradaValue *sv, const char *key) {
    if (__builtin_expect(sv->is_tied, 0)) return strada_tied_hash_fetch(sv, key);
    StradaValue *result = strada_hash_get(strada_deref_hash(sv), key);
    strada_incref(result);
    return result;
}
static inline void strada_hv_store(StradaValue *sv, const char *key, StradaValue *val) {
    if (__builtin_expect(sv->is_tied, 0)) { strada_tied_hash_store(sv, key, val); return; }
    strada_hash_set(strada_deref_hash(sv), key, val);
}
static inline int strada_hv_exists(StradaValue *sv, const char *key) {
    if (__builtin_expect(sv->is_tied, 0)) return strada_tied_hash_exists(sv, key);
    return strada_hash_exists(strada_deref_hash(sv), key);
}
static inline void strada_hv_delete(StradaValue *sv, const char *key) {
    if (__builtin_expect(sv->is_tied, 0)) { strada_tied_hash_delete(sv, key); return; }
    strada_hash_delete(strada_deref_hash(sv), key);
}

#endif /* STRADA_RUNTIME_H */

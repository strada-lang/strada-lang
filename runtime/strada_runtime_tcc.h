/* strada_runtime_tcc.h - Header for TCC compilation */
/* This header mirrors strada_runtime.h but avoids problematic system includes */
#ifndef STRADA_RUNTIME_TCC_H
#define STRADA_RUNTIME_TCC_H

#include <stdint.h>
#include <stddef.h>

/* Forward declarations */
typedef struct StradaValue StradaValue;
typedef struct StradaArray StradaArray;
typedef struct StradaHash StradaHash;
typedef struct StradaHashEntry StradaHashEntry;

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
    STRADA_CSTRUCT,
    STRADA_CPOINTER,
    STRADA_CLOSURE,
    STRADA_FUTURE,
    STRADA_CHANNEL,
    STRADA_ATOMIC
} StradaType;

/* Opaque types we don't need full definitions for */
typedef void regex_t;
typedef void StradaSocketBuffer;
typedef void StradaFhMeta;
typedef void FILE;

/* File handle metadata types */
typedef enum {
    FH_NORMAL = 0,
    FH_PIPE = 1,
    FH_MEMREAD = 2,
    FH_MEMWRITE = 3,
    FH_MEMWRITE_REF = 4
} StradaFhType;

/* Cold metadata — only allocated for OOP, tied, weak, or CSTRUCT values */
typedef struct StradaMetadata {
    char *struct_name;
    char *blessed_package;
    StradaValue *tied_obj;
    uint8_t is_tied;
    uint8_t is_weak;
} StradaMetadata;

/* Value structure - core of Strada runtime (32 bytes) */
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
        regex_t *rx;     /* Compiled regex */
        StradaSocketBuffer *sock;  /* Buffered socket */
        void *ptr;       /* Generic C pointer */
    } value;
    size_t struct_size;      /* String length cache (hot path) */
    StradaMetadata *meta;    /* NULL for most values */
};

/* Array structure - like Perl's AV */
struct StradaArray {
    StradaValue **elements;
    size_t size;
    size_t capacity;
    int refcount;
    size_t head;    /* Offset into elements[] where data starts (for O(1) shift) */
};

/* Hash entry */
struct StradaHashEntry {
    char *key;
    StradaValue *value;
    struct StradaHashEntry *next;
    unsigned int hash;
};

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

/* Exception handling macros */
#ifndef STRADA_MAX_TRY_DEPTH
#define STRADA_MAX_TRY_DEPTH 64
#endif

/* ============================================================
 * Value Creation
 * ============================================================ */
StradaValue* strada_new_undef(void);
StradaValue* strada_undef_static(void);
StradaValue* strada_new_int(int64_t value);
StradaValue* strada_new_num(double value);
StradaValue* strada_safe_div(double a, double b);
StradaValue* strada_safe_mod(int64_t a, int64_t b);
StradaValue* strada_new_str(const char *value);
StradaValue* strada_new_str_len(const char *value, size_t len);
StradaValue* strada_new_str_take(char *s);
StradaValue* strada_new_array(void);
StradaValue* strada_new_array_with_capacity(size_t capacity);
StradaValue* strada_new_hash(void);
StradaValue* strada_new_hash_with_capacity(size_t capacity);
StradaValue* strada_new_filehandle(FILE *fh);
StradaValue* strada_new_ref(StradaValue *target, char ref_type);

/* ============================================================
 * Reference Counting
 * ============================================================ */
void strada_incref(StradaValue *sv);
void strada_decref(StradaValue *sv);

/* ============================================================
 * Type Conversion
 * ============================================================ */
int64_t strada_to_int(StradaValue *sv);
double strada_to_num(StradaValue *sv);
char* strada_to_str(StradaValue *sv);  /* Returns malloc'd string - caller must free */
const char* strada_to_str_buf(StradaValue *sv, char *buf, size_t buflen);  /* Non-allocating variant */
int strada_to_bool(StradaValue *sv);

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

/* String comparison with C literals (no temporaries) */
int strada_str_eq_lit(StradaValue *sv, const char *lit);
int strada_str_ne_lit(StradaValue *sv, const char *lit);
int strada_str_lt_lit(StradaValue *sv, const char *lit);
int strada_str_gt_lit(StradaValue *sv, const char *lit);
int strada_str_le_lit(StradaValue *sv, const char *lit);
int strada_str_ge_lit(StradaValue *sv, const char *lit);

/* ============================================================
 * Type Checking and Introspection
 * ============================================================ */
int strada_is_true(StradaValue *sv);
int strada_is_defined(StradaValue *sv);
int strada_defined_bool(StradaValue *sv);
const char* strada_typeof(StradaValue *sv);
int strada_is_int(StradaValue *sv);
int strada_is_num(StradaValue *sv);
int strada_is_str(StradaValue *sv);
int strada_is_array(StradaValue *sv);
int strada_is_hash(StradaValue *sv);
StradaValue* strada_int(StradaValue *sv);
StradaValue* strada_num(StradaValue *sv);
StradaValue* strada_str(StradaValue *sv);
StradaValue* strada_bool(StradaValue *sv);
int strada_scalar(StradaValue *sv);

/* Increment/decrement operations */
StradaValue* strada_postincr(StradaValue **pv);
StradaValue* strada_postdecr(StradaValue **pv);
StradaValue* strada_preincr(StradaValue **pv);
StradaValue* strada_predecr(StradaValue **pv);

/* ============================================================
 * String Operations
 * ============================================================ */
char* strada_concat(const char *a, const char *b);
char* strada_concat_free(char *a, char *b);
StradaValue* strada_concat_sv(StradaValue *a, StradaValue *b);
StradaValue* strada_concat_inplace(StradaValue *a, StradaValue *b);
size_t strada_length(const char *s);
size_t strada_length_sv(StradaValue *sv);
size_t strada_bytes(const char *s);
size_t strada_str_len(StradaValue *sv);
StradaValue* strada_char_at(StradaValue *str, StradaValue *index);
StradaValue* strada_substr(StradaValue *str, int64_t offset, int64_t length);
StradaValue* strada_substr_bytes(StradaValue *str, int64_t offset, int64_t length);
int strada_index(const char *haystack, const char *needle);
int strada_index_offset(const char *haystack, const char *needle, int offset);
int strada_rindex(const char *haystack, const char *needle);
char* strada_upper(const char *str);
char* strada_lower(const char *str);
char* strada_uc(const char *str);
char* strada_lc(const char *str);
char* strada_ucfirst(const char *str);
char* strada_lcfirst(const char *str);
char* strada_trim(const char *str);
char* strada_ltrim(const char *str);
char* strada_rtrim(const char *str);
char* strada_reverse(const char *str);
StradaValue* strada_reverse_sv(StradaValue *sv);
char* strada_repeat(const char *str, int count);
char* strada_chr(int code);
StradaValue* strada_chr_sv(int code);
int strada_ord(const char *str);
int strada_ord_byte(StradaValue *sv);
int strada_get_byte(StradaValue *sv, int pos);
StradaValue* strada_set_byte(StradaValue *sv, int pos, int val);
int strada_byte_length(StradaValue *sv);
StradaValue* strada_byte_substr(StradaValue *sv, int start, int len);
StradaValue* strada_pack(const char *fmt, StradaValue *args);
StradaValue* strada_unpack(const char *fmt, StradaValue *data);
StradaValue* strada_base64_encode(StradaValue *sv);
StradaValue* strada_base64_decode(StradaValue *sv);
StradaValue* strada_hex(StradaValue *sv);
char* strada_chomp(const char *str);
char* strada_chop(const char *str);
int strada_strcmp(const char *s1, const char *s2);
int strada_strncmp(const char *s1, const char *s2, int n);
char* strada_join(const char *sep, StradaArray *arr);
char* strada_replace_all(const char *str, const char *find, const char *replace);
StradaValue* strada_quotemeta(StradaValue *str);

/* StringBuilder */
StradaValue* strada_sb_new(void);
StradaValue* strada_sb_new_cap(StradaValue *capacity);
void strada_sb_append(StradaValue *sb, StradaValue *str);
void strada_sb_append_str(StradaValue *sb, const char *str);
StradaValue* strada_sb_to_string(StradaValue *sb);
StradaValue* strada_sb_length(StradaValue *sb);
void strada_sb_clear(StradaValue *sb);
void strada_sb_free(StradaValue *sb);

/* String repetition (x operator) */
StradaValue* strada_string_repeat(StradaValue *sv, int64_t count);

/* ============================================================
 * Array Operations
 * ============================================================ */
StradaArray* strada_array_new(void);
void strada_array_push(StradaArray *arr, StradaValue *val);
void strada_array_push_take(StradaArray *arr, StradaValue *val);
StradaValue* strada_array_pop(StradaArray *arr);
StradaValue* strada_array_shift(StradaArray *arr);
void strada_array_unshift(StradaArray *arr, StradaValue *val);
StradaValue* strada_array_get(StradaArray *arr, int64_t index);
StradaValue* strada_array_get_safe(StradaValue *arr, int64_t idx);
void strada_array_set(StradaArray *arr, int64_t index, StradaValue *val);
size_t strada_array_length(StradaArray *arr);
void strada_array_reverse(StradaArray *arr);
void strada_array_reserve(StradaArray *av, size_t capacity);
void strada_reserve_sv(StradaValue *sv, int64_t capacity);
int64_t strada_size(StradaValue *sv);
int64_t strada_get_array_default_capacity(void);
void strada_set_array_default_capacity(int64_t capacity);
StradaValue* strada_new_array_from_av(StradaArray *av);
StradaValue* strada_array_copy(StradaValue *src);
StradaValue* strada_sort(StradaValue *arr);
StradaValue* strada_nsort(StradaValue *arr);
StradaValue* strada_range(StradaValue *start, StradaValue *end);
StradaValue* strada_array_slice(StradaValue *arr, StradaValue *start, StradaValue *end);
StradaValue* strada_array_splice(StradaValue *arr, StradaValue *offset, StradaValue *length, StradaValue *replacement);
StradaValue* strada_array_splice_sv(StradaValue *arr_sv, int64_t offset, int64_t length, StradaValue *repl_sv);

/* ============================================================
 * Hash Operations
 * ============================================================ */
StradaHash* strada_hash_new(void);
StradaValue* strada_hash_get(StradaHash *hash, const char *key);
void strada_hash_set(StradaHash *hash, const char *key, StradaValue *val);
void strada_hash_set_take(StradaHash *hash, const char *key, StradaValue *val);
int strada_hash_exists(StradaHash *hash, const char *key);
void strada_hash_delete(StradaHash *hash, const char *key);
StradaArray* strada_hash_keys(StradaHash *hash);
StradaArray* strada_hash_values(StradaHash *hash);
int64_t strada_hash_size(StradaHash *hash);
void strada_hash_reserve(StradaHash *hv, size_t num_buckets);
void strada_hash_reserve_sv(StradaValue *sv, int64_t capacity);
int64_t strada_get_hash_default_capacity(void);
void strada_set_hash_default_capacity(int64_t capacity);
StradaValue* strada_hash_each(StradaHash *hv);
void strada_hash_reset_iter(StradaHash *hv);
int strada_exists(StradaValue *hash, StradaValue *key);
void strada_delete(StradaValue *hash, StradaValue *key);

/* ============================================================
 * Reference Operations
 * ============================================================ */
StradaValue* strada_ref_create(StradaValue *sv);
StradaValue* strada_ref_create_take(StradaValue *sv);
StradaValue* strada_ref_deref(StradaValue *ref);
int strada_is_ref(StradaValue *sv);
const char* strada_reftype(StradaValue *ref);
StradaValue* strada_ref_scalar(StradaValue **ptr);
StradaValue* strada_ref_array(StradaArray **ptr);
StradaValue* strada_ref_hash(StradaHash **ptr);
StradaValue* strada_deref(StradaValue *ref);
StradaValue* strada_deref_set(StradaValue *ref, StradaValue *new_value);
StradaHash* strada_deref_hash(StradaValue *ref);
StradaArray* strada_deref_array(StradaValue *ref);
StradaValue* strada_deref_hash_value(StradaValue *ref);
StradaValue* strada_deref_array_value(StradaValue *ref);
StradaValue* strada_anon_hash(int count, ...);
StradaValue* strada_anon_array(int count, ...);
StradaValue* strada_array_from_ref(StradaValue *ref);
StradaValue* strada_hash_from_ref(StradaValue *ref);
StradaValue* strada_hash_from_flat_array(StradaValue *arr);
StradaValue* strada_hash_to_flat_array(StradaValue *hash);

/* ============================================================
 * Comparison
 * ============================================================ */
int strada_num_cmp(StradaValue *a, StradaValue *b);
int strada_str_cmp(StradaValue *a, StradaValue *b);
int strada_str_eq(StradaValue *a, StradaValue *b);
int strada_str_ne(StradaValue *a, StradaValue *b);
int strada_str_lt(StradaValue *a, StradaValue *b);
int strada_str_gt(StradaValue *a, StradaValue *b);
int strada_str_le(StradaValue *a, StradaValue *b);
int strada_str_ge(StradaValue *a, StradaValue *b);

/* ============================================================
 * I/O - Console
 * ============================================================ */
void strada_say(StradaValue *sv);
void strada_print(StradaValue *sv);
void strada_print_fh(StradaValue *sv, StradaValue *fh);
void strada_say_fh(StradaValue *sv, StradaValue *fh);
StradaValue* strada_readline(void);
void strada_printf(const char *format, ...);
StradaValue* strada_sprintf(const char *format, ...);
StradaValue* strada_sprintf_sv(StradaValue *format_sv, int arg_count, ...);
void strada_warn(const char *format, ...);
void strada_die(const char *format, ...);
void strada_die_sv(StradaValue *msg);

/* ============================================================
 * I/O - File Operations
 * ============================================================ */
StradaValue* strada_open(const char *filename, const char *mode);
StradaValue* strada_open_str(const char *content, const char *mode);
StradaValue* strada_open_sv(StradaValue *first_arg, StradaValue *mode_arg);
StradaValue* strada_str_from_fh(StradaValue *fh);
void strada_close(StradaValue *fh);
StradaValue* strada_read_file(StradaValue *fh);
StradaValue* strada_read_line(StradaValue *fh);
StradaValue* strada_read_all_lines(StradaValue *fh);
void strada_write_file(StradaValue *fh, const char *content);
int strada_file_exists(const char *filename);
StradaValue* strada_slurp(const char *filename);
StradaValue* strada_slurp_fh(StradaValue *fh_sv);
StradaValue* strada_slurp_fd(StradaValue *fd_sv);
void strada_spew(const char *filename, const char *content);
void strada_spew_fh(StradaValue *fh_sv, StradaValue *content_sv);
void strada_spew_fd(StradaValue *fd_sv, StradaValue *content_sv);
StradaValue* strada_fgetc(StradaValue *fh);
StradaValue* strada_fputc(StradaValue *ch, StradaValue *fh);
StradaValue* strada_fgets(StradaValue *fh, StradaValue *size);
StradaValue* strada_fputs(StradaValue *str, StradaValue *fh);
StradaValue* strada_ferror(StradaValue *fh);
StradaValue* strada_fileno(StradaValue *fh);
StradaValue* strada_clearerr(StradaValue *fh);
StradaValue* strada_seek(StradaValue *fh, StradaValue *offset, StradaValue *whence);
StradaValue* strada_tell(StradaValue *fh);
StradaValue* strada_rewind(StradaValue *fh);
StradaValue* strada_eof(StradaValue *fh);
StradaValue* strada_flush(StradaValue *fh);
StradaValue* strada_select(StradaValue *fh);
StradaValue* strada_select_get(void);

/* Temporary files */
StradaValue* strada_tmpfile(void);
StradaValue* strada_mkstemp(StradaValue *tmpl);
StradaValue* strada_mkdtemp(StradaValue *tmpl);

/* Command execution (popen) */
StradaValue* strada_popen(StradaValue *cmd, StradaValue *mode);
StradaValue* strada_pclose(StradaValue *fh);
StradaValue* strada_qx(StradaValue *cmd);

/* Aliases for bootstrap compiler compatibility */
StradaValue* sys_system(StradaValue *cmd);
StradaValue* sys_qx(StradaValue *cmd);
StradaValue* sys_unlink(StradaValue *path);

/* ============================================================
 * Regex
 * ============================================================ */
StradaValue* strada_regex_compile(const char *pattern, const char *flags);
int strada_regex_match(const char *str, const char *pattern);
int strada_regex_match_with_capture(const char *str, const char *pattern, const char *flags);
StradaValue* strada_captures(void);
StradaValue* strada_capture_var(int n);
StradaValue* strada_named_captures(void);
StradaValue* strada_regex_match_all(const char *str, const char *pattern);
char* strada_regex_replace(const char *str, const char *pattern, const char *replacement, const char *flags);
char* strada_regex_replace_all(const char *str, const char *pattern, const char *replacement, const char *flags);
StradaArray* strada_string_split(const char *str, const char *delim);
StradaArray* strada_regex_split(const char *str, const char *pattern);
StradaArray* strada_regex_capture(const char *str, const char *pattern);
StradaValue* strada_regex_find_all(const char *str, const char *pattern, const char *flags, int global);
void strada_set_captures_sv(StradaValue *match);
StradaValue* strada_regex_build_result(const char *src, StradaValue *matches, StradaValue *replacements);
StradaValue* strada_tr(StradaValue *sv, const char *search, const char *replace, const char *flags);

/* ============================================================
 * OOP - Blessed References
 * ============================================================ */
typedef StradaValue* (*StradaMethod)(StradaValue *self, StradaValue *args);
StradaValue* strada_bless(StradaValue *ref, const char *package);
StradaValue* strada_blessed(StradaValue *ref);
void strada_set_package(const char *package);
const char* strada_current_package(void);
void strada_inherit(const char *child, const char *parent);
void strada_inherit_from(const char *parent);
void strada_method_register(const char *package, const char *name, StradaMethod func);
void strada_register_method(const char *package, const char *name, void *func);
void strada_modifier_register(const char *package, const char *method, int type, StradaMethod func);
StradaValue* strada_method_call(StradaValue *obj, const char *method, StradaValue *args);
const char* strada_method_lookup_package(const char *package, const char *method);
const char* strada_get_parent_package(const char *package);
int strada_isa(StradaValue *obj, const char *package);
int strada_can(StradaValue *obj, const char *method);
void strada_oop_init(void);

/* SUPER:: and DESTROY support */
StradaValue* strada_super_call(StradaValue *obj, const char *from_package,
                               const char *method, StradaValue *args);
void strada_call_destroy(StradaValue *obj);
void strada_set_method_package(const char *pkg);
const char* strada_get_method_package(void);

/* Operator overloading */
void strada_overload_register(const char *package, const char *op, StradaMethod func);
StradaValue* strada_overload_binary(StradaValue *left, StradaValue *right, const char *op);
StradaValue* strada_overload_unary(StradaValue *operand, const char *op);
StradaValue* strada_overload_stringify(StradaValue *val);

/* ============================================================
 * Closures
 * ============================================================ */
StradaValue* strada_closure_new(void *func, int arg_count, int capture_count, StradaValue ***captures);
StradaValue*** strada_closure_get_captures(StradaValue *closure);
StradaValue* strada_closure_call(StradaValue *closure, int argc, ...);
StradaValue* strada_closure_call_method(StradaValue *closure, StradaValue *self, StradaValue *args);

/* Variadic function support */
StradaValue* strada_pack_args(int count, ...);

/* ============================================================
 * Memory Management
 * ============================================================ */
void strada_free(StradaValue *sv);
StradaValue* strada_release(StradaValue *ref);
StradaValue* strada_undef(StradaValue *sv);
int strada_refcount(StradaValue *sv);
void strada_free_value(StradaValue *sv);
void strada_free_array(StradaArray *av);
void strada_free_hash(StradaHash *hv);

/* Weak references */
void strada_weaken(StradaValue **ref_ptr);
void strada_weaken_hv_entry(StradaHash *hv, const char *key);
int strada_isweak(StradaValue *ref);
void strada_weak_registry_init(void);
void strada_weak_registry_remove_target(StradaValue *target);
void strada_weak_registry_unregister(StradaValue *ref);

/* ============================================================
 * Exceptions
 * ============================================================ */
void strada_throw(const char *msg);
void strada_throw_value(StradaValue *val);
StradaValue* strada_get_exception(void);
void strada_clear_exception(void);
int strada_in_try_block(void);

/* Cleanup stack (temp value tracking) */
void strada_cleanup_push(StradaValue *sv);
void strada_cleanup_pop(void);
void strada_cleanup_drain(void);
int strada_cleanup_mark(void);
void strada_cleanup_restore(int mark);
void strada_cleanup_drain_to(int mark);

/* ============================================================
 * Stack Trace Management
 * ============================================================ */
void strada_stack_push(const char *func_name, const char *file_name);
void strada_stack_pop(void);
void strada_stack_set_line(int line);
void strada_print_stack_trace(void);
StradaValue* strada_capture_stack_trace(void);
void strada_set_recursion_limit(int limit);
int strada_get_recursion_limit(void);
void strada_stacktrace(void);
void strada_backtrace(void);
char* strada_stacktrace_str(void);
const char* strada_caller(int level);

/* ============================================================
 * Built-in Functions
 * ============================================================ */
StradaValue* strada_defined(StradaValue *sv);
StradaValue* strada_ref(StradaValue *sv);
void strada_dump(StradaValue *sv, int indent);
void strada_dumper(StradaValue *sv);
StradaValue* strada_dumper_str(StradaValue *sv);
StradaValue* strada_clone(StradaValue *sv);

/* ============================================================
 * Dynamic Return Type (wantarray)
 * ============================================================ */
extern int strada_call_context;
void strada_set_call_context(int ctx);
int strada_wantarray(void);
int strada_wantscalar(void);
int strada_wanthash(void);

/* ============================================================
 * UTF-8 Namespace Functions
 * ============================================================ */
int strada_utf8_is_valid(const char *str, size_t len);
StradaValue* strada_utf8_is_utf8(StradaValue *sv);
StradaValue* strada_utf8_valid(StradaValue *sv);
StradaValue* strada_utf8_encode(StradaValue *sv);
StradaValue* strada_utf8_decode(StradaValue *sv);
StradaValue* strada_utf8_downgrade(StradaValue *sv, int fail_ok);
StradaValue* strada_utf8_upgrade(StradaValue *sv);
StradaValue* strada_utf8_unicode_to_native(StradaValue *sv);

/* ============================================================
 * Math
 * ============================================================ */
double strada_math_sin(double x);
double strada_math_cos(double x);
double strada_math_tan(double x);
double strada_math_sqrt(double x);
double strada_math_pow(double x, double y);
double strada_math_log(double x);
double strada_math_exp(double x);
double strada_math_floor(double x);
double strada_math_ceil(double x);
double strada_math_abs(double x);
int64_t strada_math_rand(void);
void strada_math_srand(int64_t seed);

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
StradaValue* strada_abs(StradaValue *sv);
StradaValue* strada_sqrt(StradaValue *sv);
StradaValue* strada_rand(void);

/* ============================================================
 * Time Functions
 * ============================================================ */
StradaValue* strada_time(void);
StradaValue* strada_localtime(StradaValue *timestamp);
StradaValue* strada_gmtime(StradaValue *timestamp);
StradaValue* strada_mktime(StradaValue *time_hash);
StradaValue* strada_strftime(StradaValue *format, StradaValue *time_hash);
StradaValue* strada_ctime(StradaValue *timestamp);
StradaValue* strada_sleep(StradaValue *seconds);
StradaValue* strada_usleep(StradaValue *usecs);
StradaValue* strada_gettimeofday(void);
StradaValue* strada_hires_time(void);
StradaValue* strada_tv_interval(StradaValue *start, StradaValue *end);
StradaValue* strada_nanosleep_ns(StradaValue *nanosecs);
StradaValue* strada_clock_gettime(StradaValue *clock_id);
StradaValue* strada_clock_getres(StradaValue *clock_id);
StradaValue* strada_difftime(StradaValue *t1, StradaValue *t0);
StradaValue* strada_clock(void);
StradaValue* strada_times(void);

/* ============================================================
 * Process Control
 * ============================================================ */
StradaValue* strada_fork(void);
StradaValue* strada_wait(void);
StradaValue* strada_waitpid(StradaValue *pid, StradaValue *options);
StradaValue* strada_getpid(void);
StradaValue* strada_getppid(void);
StradaValue* strada_exit_status(StradaValue *status);
void strada_exit(int code);

/* ============================================================
 * POSIX Functions
 * ============================================================ */
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

/* Pipe and IPC */
StradaValue* strada_pipe(void);
StradaValue* strada_dup2(StradaValue *oldfd, StradaValue *newfd);
StradaValue* strada_dup(StradaValue *oldfd);
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

/* User/Group database */
StradaValue* strada_getpwnam(StradaValue *name);
StradaValue* strada_getpwuid(StradaValue *uid);
StradaValue* strada_getgrnam(StradaValue *name);
StradaValue* strada_getgrgid(StradaValue *gid);
StradaValue* strada_getlogin(void);
StradaValue* strada_getgroups(void);

/* Process name */
StradaValue* strada_setprocname(StradaValue *name);
StradaValue* strada_getprocname(void);
StradaValue* strada_setproctitle(StradaValue *title);
StradaValue* strada_getproctitle(void);

/* Signals */
StradaValue* strada_sigprocmask(StradaValue *how, StradaValue *set);
StradaValue* strada_raise(StradaValue *sig);
StradaValue* strada_killpg(StradaValue *pgrp, StradaValue *sig);
StradaValue* strada_pause(void);

/* Resource/Priority */
StradaValue* strada_nice(StradaValue *inc);
StradaValue* strada_getpriority(StradaValue *which, StradaValue *who);
StradaValue* strada_setpriority(StradaValue *which, StradaValue *who, StradaValue *prio);
StradaValue* strada_getrusage(StradaValue *who);
StradaValue* strada_getrlimit(StradaValue *resource);
StradaValue* strada_setrlimit(StradaValue *resource, StradaValue *rlim);

/* ============================================================
 * Directory/Path Functions
 * ============================================================ */
StradaValue* strada_readdir(StradaValue *path);
StradaValue* strada_readdir_full(StradaValue *path);
StradaValue* strada_is_dir(StradaValue *path);
StradaValue* strada_is_file(StradaValue *path);
StradaValue* strada_file_size(StradaValue *path);
StradaValue* strada_realpath(StradaValue *path);
StradaValue* strada_dirname(StradaValue *path);
StradaValue* strada_basename(StradaValue *path);
StradaValue* strada_glob(StradaValue *pattern);
StradaValue* strada_fnmatch(StradaValue *pattern, StradaValue *string);
StradaValue* strada_file_ext(StradaValue *path);
StradaValue* strada_path_join(StradaValue *parts);

/* Additional file system */
StradaValue* strada_truncate(StradaValue *path, StradaValue *length);
StradaValue* strada_ftruncate(StradaValue *fd, StradaValue *length);
StradaValue* strada_chown(StradaValue *path, StradaValue *uid, StradaValue *gid);
StradaValue* strada_lchown(StradaValue *path, StradaValue *uid, StradaValue *gid);
StradaValue* strada_fchmod(StradaValue *fd, StradaValue *mode);
StradaValue* strada_fchown(StradaValue *fd, StradaValue *uid, StradaValue *gid);
StradaValue* strada_utime(StradaValue *path, StradaValue *atime, StradaValue *mtime);
StradaValue* strada_utimes(StradaValue *path, StradaValue *atime, StradaValue *mtime);

/* ============================================================
 * Socket Functions
 * ============================================================ */
StradaValue* strada_socket_create(void);
int strada_socket_connect(StradaValue *sock, const char *host, int port);
int strada_socket_bind(StradaValue *sock, int port);
int strada_socket_listen(StradaValue *sock, int backlog);
StradaValue* strada_socket_accept(StradaValue *sock);
int strada_socket_send(StradaValue *sock, const char *data);
int strada_socket_send_sv(StradaValue *sock, StradaValue *data);
StradaValue* strada_socket_recv(StradaValue *sock, int max_len);
void strada_socket_close(StradaValue *sock);
void strada_socket_flush(StradaValue *sock);
StradaValue* strada_socket_server(int port);
StradaValue* strada_socket_server_backlog(int port, int backlog);
StradaValue* strada_socket_client(const char *host, int port);
StradaValue* strada_socket_select(StradaValue *sockets, int timeout_ms);
int strada_socket_fd(StradaValue *sock);
StradaValue* strada_select_fds(StradaValue *fds, int timeout_ms);
int strada_socket_set_nonblocking(StradaValue *sock, int nonblock);

/* UDP */
StradaValue* strada_udp_socket(void);
int strada_udp_bind(StradaValue *sock, int port);
StradaValue* strada_udp_server(int port);
StradaValue* strada_udp_recvfrom(StradaValue *sock, int max_len);
int strada_udp_sendto(StradaValue *sock, const char *data, int data_len, const char *host, int port);
int strada_udp_sendto_sv(StradaValue *sock, StradaValue *data, const char *host, int port);

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

/* DNS/Hostname */
StradaValue* strada_gethostbyname(StradaValue *hostname);
StradaValue* strada_gethostbyname_all(StradaValue *hostname);
StradaValue* strada_gethostname(void);
StradaValue* strada_getaddrinfo_first(StradaValue *hostname, StradaValue *service);

/* ============================================================
 * Random
 * ============================================================ */
StradaValue* strada_srand(StradaValue *seed);
StradaValue* strada_srandom(StradaValue *seed);
StradaValue* strada_libc_rand(void);
StradaValue* strada_libc_random(void);
StradaValue* strada_random_bytes(StradaValue *num_bytes);
StradaValue* strada_random_bytes_hex(StradaValue *num_bytes);

/* ============================================================
 * C Struct Support
 * ============================================================ */
StradaValue* strada_cstruct_new(const char *struct_name, size_t size);
void* strada_cstruct_ptr(StradaValue *sv);
void strada_cstruct_set_field(StradaValue *sv, const char *field, size_t offset, void *value, size_t size);
void* strada_cstruct_get_field(StradaValue *sv, const char *field, size_t offset, size_t size);
void strada_cstruct_set_int(StradaValue *sv, const char *field, size_t offset, int64_t value);
int64_t strada_cstruct_get_int(StradaValue *sv, const char *field, size_t offset);
void strada_cstruct_set_string(StradaValue *sv, const char *field, size_t offset, const char *value);
void strada_cstruct_set_double(StradaValue *sv, const char *field, size_t offset, double value);
char* strada_cstruct_get_string(StradaValue *sv, const char *field, size_t offset);
double strada_cstruct_get_double(StradaValue *sv, const char *field, size_t offset);

/* C Pointer Support */
StradaValue* strada_cpointer_new(void *ptr);
void* strada_cpointer_get(StradaValue *sv);

/* ============================================================
 * C Interop (c:: namespace)
 * ============================================================ */
StradaValue* strada_c_str_to_ptr(StradaValue *sv);
StradaValue* strada_c_ptr_to_str(StradaValue *ptr_sv);
StradaValue* strada_c_ptr_to_str_n(StradaValue *ptr_sv, StradaValue *len_sv);
StradaValue* strada_c_free(StradaValue *ptr_sv);
StradaValue* strada_c_alloc(StradaValue *size_sv);
StradaValue* strada_c_realloc(StradaValue *ptr_sv, StradaValue *size_sv);
StradaValue* strada_c_null(void);
StradaValue* strada_c_is_null(StradaValue *ptr_sv);
StradaValue* strada_c_ptr_add(StradaValue *ptr_sv, StradaValue *offset_sv);
StradaValue* strada_c_read_int8(StradaValue *ptr_sv);
StradaValue* strada_c_read_int16(StradaValue *ptr_sv);
StradaValue* strada_c_read_int32(StradaValue *ptr_sv);
StradaValue* strada_c_read_int64(StradaValue *ptr_sv);
StradaValue* strada_c_read_ptr(StradaValue *ptr_sv);
StradaValue* strada_c_read_float(StradaValue *ptr_sv);
StradaValue* strada_c_read_double(StradaValue *ptr_sv);
StradaValue* strada_c_write_int8(StradaValue *ptr_sv, StradaValue *val_sv);
StradaValue* strada_c_write_int16(StradaValue *ptr_sv, StradaValue *val_sv);
StradaValue* strada_c_write_int32(StradaValue *ptr_sv, StradaValue *val_sv);
StradaValue* strada_c_write_int64(StradaValue *ptr_sv, StradaValue *val_sv);
StradaValue* strada_c_write_ptr(StradaValue *ptr_sv, StradaValue *val_sv);
StradaValue* strada_c_write_float(StradaValue *ptr_sv, StradaValue *val_sv);
StradaValue* strada_c_write_double(StradaValue *ptr_sv, StradaValue *val_sv);
StradaValue* strada_c_sizeof_int(void);
StradaValue* strada_c_sizeof_long(void);
StradaValue* strada_c_sizeof_ptr(void);
StradaValue* strada_c_sizeof_size_t(void);
StradaValue* strada_c_memcpy(StradaValue *dest_sv, StradaValue *src_sv, StradaValue *n_sv);
StradaValue* strada_c_memset(StradaValue *dest_sv, StradaValue *c_sv, StradaValue *n_sv);

/* C string helpers */
char* strada_cstr_concat(const char *a, const char *b);
char* strada_int_to_cstr(int64_t n);
char* strada_num_to_cstr(double n);

/* ============================================================
 * Dynamic Loading
 * ============================================================ */
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
StradaValue* strada_dl_open_raw(StradaValue *path);
StradaValue* strada_dl_sym_raw(StradaValue *handle_sv, StradaValue *symbol);
StradaValue* strada_dl_close_raw(StradaValue *handle_sv);
StradaValue* strada_dl_call_export_info(StradaValue *fn_ptr_sv);
StradaValue* strada_dl_call_version(StradaValue *fn_ptr_sv);

/* Pointer access for FFI */
StradaValue* strada_int_ptr(StradaValue *ref);
StradaValue* strada_num_ptr(StradaValue *ref);
StradaValue* strada_str_ptr(StradaValue *ref);
StradaValue* strada_ptr_deref_int(StradaValue *ptr);
StradaValue* strada_ptr_deref_num(StradaValue *ptr);
StradaValue* strada_ptr_deref_str(StradaValue *ptr);
StradaValue* strada_ptr_set_int(StradaValue *ptr, StradaValue *val);
StradaValue* strada_ptr_set_num(StradaValue *ptr, StradaValue *val);

/* ============================================================
 * Terminal/TTY
 * ============================================================ */
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
StradaValue* strada_ioctl(StradaValue *fd, StradaValue *request, StradaValue *arg);
StradaValue* strada_statvfs(StradaValue *path);
StradaValue* strada_fstatvfs(StradaValue *fd);

/* String conversion */
StradaValue* strada_strtol(StradaValue *str, StradaValue *base);
StradaValue* strada_strtod(StradaValue *str);
StradaValue* strada_atoi(StradaValue *str);
StradaValue* strada_atof(StradaValue *str);

/* Memory functions */
StradaValue* strada_calloc(StradaValue *nmemb, StradaValue *size);
StradaValue* strada_realloc(StradaValue *ptr, StradaValue *size);
StradaValue* strada_mmap(StradaValue *addr, StradaValue *length, StradaValue *prot, StradaValue *flags, StradaValue *fd, StradaValue *offset);
StradaValue* strada_munmap(StradaValue *addr, StradaValue *length);
StradaValue* strada_mlock(StradaValue *addr, StradaValue *len);
StradaValue* strada_munlock(StradaValue *addr, StradaValue *len);

/* ============================================================
 * Thread/Mutex/Cond (declarations only - opaque handles)
 * ============================================================ */
StradaValue* strada_thread_create(StradaValue *closure);
StradaValue* strada_thread_join(StradaValue *thread_val);
StradaValue* strada_thread_detach(StradaValue *thread_val);
StradaValue* strada_thread_self(void);
StradaValue* strada_mutex_new(void);
StradaValue* strada_mutex_lock(StradaValue *mutex);
StradaValue* strada_mutex_trylock(StradaValue *mutex);
StradaValue* strada_mutex_unlock(StradaValue *mutex);
StradaValue* strada_mutex_destroy(StradaValue *mutex);
StradaValue* strada_cond_new(void);
StradaValue* strada_cond_wait(StradaValue *cond, StradaValue *mutex);
StradaValue* strada_cond_signal(StradaValue *cond);
StradaValue* strada_cond_broadcast(StradaValue *cond);
StradaValue* strada_cond_destroy(StradaValue *cond);

/* ============================================================
 * Async/Await (declarations only - opaque handles)
 * ============================================================ */
StradaValue* strada_future_new(StradaValue *closure);
StradaValue* strada_future_await(StradaValue *future);
StradaValue* strada_future_await_timeout(StradaValue *future, int64_t timeout_ms);
int strada_future_is_done(StradaValue *future);
StradaValue* strada_future_try_get(StradaValue *future);
void strada_future_cancel(StradaValue *future);
int strada_future_is_cancelled(StradaValue *future);
StradaValue* strada_future_all(StradaValue *futures_array);
StradaValue* strada_future_race(StradaValue *futures_array);
StradaValue* strada_async_timeout(StradaValue *future, StradaValue *timeout_ms);

/* Channels */
StradaValue* strada_channel_new(int capacity);
void strada_channel_send(StradaValue *channel, StradaValue *value);
StradaValue* strada_channel_recv(StradaValue *channel);
int strada_channel_try_send(StradaValue *channel, StradaValue *value);
StradaValue* strada_channel_try_recv(StradaValue *channel);
void strada_channel_close(StradaValue *channel);
int strada_channel_is_closed(StradaValue *channel);
int strada_channel_len(StradaValue *channel);

/* Atomics */
StradaValue* strada_atomic_new(int64_t initial);
int64_t strada_atomic_load(StradaValue *atomic);
void strada_atomic_store(StradaValue *atomic, int64_t value);
int64_t strada_atomic_add(StradaValue *atomic, int64_t delta);
int64_t strada_atomic_sub(StradaValue *atomic, int64_t delta);
int strada_atomic_cas(StradaValue *atomic, int64_t expected, int64_t desired);
int64_t strada_atomic_inc(StradaValue *atomic);
int64_t strada_atomic_dec(StradaValue *atomic);

/* ============================================================
 * Profiling
 * ============================================================ */
void strada_profile_init(void);
void strada_profile_enter(const char *func_name);
void strada_profile_exit(const char *func_name);
void strada_profile_report(void);
void strada_memprof_enable(void);
void strada_memprof_disable(void);
void strada_memprof_reset(void);
void strada_memprof_report(void);

/* ============================================================
 * Global Variable Registry
 * ============================================================ */
void strada_global_set(StradaValue *name, StradaValue *val);
StradaValue* strada_global_get(StradaValue *name);
int strada_global_exists(StradaValue *name);
void strada_global_delete(StradaValue *name);
StradaValue* strada_global_keys(void);

/* ============================================================
 * local() - Dynamic Scoping
 * ============================================================ */
void strada_local_save(const char *name);
void strada_local_restore(void);
void strada_local_restore_n(int n);
int strada_local_depth_get(void);
void strada_local_restore_to(int depth);

/* ============================================================
 * tie/untie/tied
 * ============================================================ */
StradaValue* strada_tie_hash(StradaValue *ref, const char *classname, int argc, ...);
StradaValue* strada_tie_array(StradaValue *ref, const char *classname, int argc, ...);
StradaValue* strada_tie_scalar(StradaValue *ref, const char *classname, int argc, ...);
void strada_untie(StradaValue *ref);
StradaValue* strada_tied(StradaValue *ref);
StradaValue* strada_tied_hash_fetch(StradaValue *sv, const char *key);
void strada_tied_hash_store(StradaValue *sv, const char *key, StradaValue *val);
int strada_tied_hash_exists(StradaValue *sv, const char *key);
void strada_tied_hash_delete(StradaValue *sv, const char *key);
StradaValue* strada_tied_hash_firstkey(StradaValue *sv);
StradaValue* strada_tied_hash_nextkey(StradaValue *sv, const char *lastkey);
void strada_tied_hash_clear(StradaValue *sv);

/* hv_fetch/store wrappers for tied variable support */
static inline StradaValue* strada_hv_fetch(StradaValue *sv, const char *key) {
    if (__builtin_expect(sv->meta && sv->meta->is_tied, 0)) return strada_tied_hash_fetch(sv, key);
    return strada_hash_get(strada_deref_hash(sv), key);
}
static inline StradaValue* strada_hv_fetch_owned(StradaValue *sv, const char *key) {
    if (__builtin_expect(sv->meta && sv->meta->is_tied, 0)) return strada_tied_hash_fetch(sv, key);
    StradaValue *result = strada_hash_get(strada_deref_hash(sv), key);
    strada_incref(result);
    return result;
}
static inline void strada_hv_store(StradaValue *sv, const char *key, StradaValue *val) {
    if (__builtin_expect(sv->meta && sv->meta->is_tied, 0)) { strada_tied_hash_store(sv, key, val); return; }
    strada_hash_set(strada_deref_hash(sv), key, val);
}
static inline int strada_hv_exists(StradaValue *sv, const char *key) {
    if (__builtin_expect(sv->meta && sv->meta->is_tied, 0)) return strada_tied_hash_exists(sv, key);
    return strada_hash_exists(strada_deref_hash(sv), key);
}
static inline void strada_hv_delete(StradaValue *sv, const char *key) {
    if (__builtin_expect(sv->meta && sv->meta->is_tied, 0)) { strada_tied_hash_delete(sv, key); return; }
    strada_hash_delete(strada_deref_hash(sv), key);
}

#endif /* STRADA_RUNTIME_TCC_H */

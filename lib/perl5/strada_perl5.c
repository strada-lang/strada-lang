/*
 This file is part of the Strada Language (https://github.com/strada-lang/strada-lang).
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

/*
 * strada_perl5.c - Perl 5 embedding for Strada
 *
 * This module allows Strada programs to embed a Perl 5 interpreter
 * and call Perl code/subroutines from Strada.
 */

#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif
#include "strada_runtime.h"
#include <string.h>
#include <stdio.h>

#include <EXTERN.h>
#include <perl.h>

/*
 * Note: XS modules (like POSIX, JSON::XS, etc.) require special handling.
 * Run with: LD_PRELOAD=/lib/x86_64-linux-gnu/libperl.so.5.38 ./your_program
 * Pure Perl modules work without this.
 */

/* Global Perl interpreter (singleton) */
static PerlInterpreter *my_perl = NULL;

static int perl5_valid_module_name(const char *m) {
    if (!m || !*m || strlen(m) > 1024) return 0;
    for (const char *p = m; *p; p++) {
        char c = *p;
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
              (c >= '0' && c <= '9') || c == '_' || c == ':')) {
            return 0;
        }
    }
    return 1;
}

static void perl5_push_inc(const char *path) {
    AV *inc = GvAVn(PL_incgv);
    if (inc && path) {
        av_push(inc, newSVpv(path, 0));
    }
}

static void perl5_set_sv_from_strada(SV *sv, StradaValue *v) {
    if (!sv) return;
    StradaValue *fetched = NULL;
    StradaValue *val = v;
    if (v && !STRADA_IS_TAGGED_INT(v) && v->meta && v->meta->is_tied && v->meta->tied_obj) {
        fetched = strada_tied_scalar_fetch(v);
        val = fetched;
    }
    if (fetched) strada_cleanup_push(fetched);
    if (!val || (!STRADA_IS_TAGGED_INT(val) && val->type == STRADA_UNDEF)) {
        sv_setsv(sv, &PL_sv_undef);
    } else if (!STRADA_IS_TAGGED_INT(val) && val->type == STRADA_STR
               && !(val->meta && (val->meta->blessed_package || val->meta->is_tied))) {
        size_t len = STRADA_STR_BYTELEN(val);
        sv_setpvn(sv, val->value.pv ? val->value.pv : "", (STRLEN)len);
        if (STRADA_STR_IS_UTF8(val)) {
            SvUTF8_on(sv);
        } else {
            SvUTF8_off(sv);
        }
    } else {
        char *s = strada_to_str(val);
        size_t len = s ? strlen(s) : 0;
        sv_setpvn(sv, s ? s : "", (STRLEN)len);
        free(s);
        SvUTF8_off(sv);
    }
    if (fetched) strada_cleanup_pop_decref(fetched);
}

typedef enum {
    P5_ARG_UNDEF = 0,
    P5_ARG_INT,
    P5_ARG_NUM,
    P5_ARG_STR
} Perl5ArgKind;

typedef struct {
    Perl5ArgKind kind;
    int64_t iv;
    double nv;
    char *pv;
    size_t len;
    int is_utf8;
} Perl5ConvArg;

static void perl5_conv_args_free(Perl5ConvArg *cargs, size_t count) {
    if (!cargs) return;
    for (size_t i = 0; i < count; i++) {
        free(cargs[i].pv);
    }
    free(cargs);
}

static void perl5_conv_one_arg(StradaValue *arg, Perl5ConvArg *dst) {
    dst->kind = P5_ARG_UNDEF;
    dst->iv = 0;
    dst->nv = 0.0;
    dst->pv = NULL;
    dst->len = 0;
    dst->is_utf8 = 0;
    if (!arg) return;

    StradaValue *fetched = NULL;
    if (!STRADA_IS_TAGGED_INT(arg) && arg->meta && arg->meta->is_tied && arg->meta->tied_obj) {
        fetched = strada_tied_scalar_fetch(arg);
        arg = fetched;
        if (!arg) return;
    }

    if (!STRADA_IS_TAGGED_INT(arg) && arg->type == STRADA_UNDEF) {
        dst->kind = P5_ARG_UNDEF;
    } else if (STRADA_IS_TAGGED_INT(arg)) {
        dst->kind = P5_ARG_INT;
        dst->iv = STRADA_TAGGED_INT_VAL(arg);
    } else if (arg->type == STRADA_INT) {
        dst->kind = P5_ARG_INT;
        dst->iv = arg->value.iv;
    } else if (arg->type == STRADA_NUM) {
        dst->kind = P5_ARG_NUM;
        dst->nv = arg->value.nv;
    } else if (arg->type == STRADA_STR && !(arg->meta && arg->meta->blessed_package)) {
        size_t len = STRADA_STR_BYTELEN(arg);
        char *buf = malloc(len + 1);
        if (buf) {
            if (len > 0 && arg->value.pv) memcpy(buf, arg->value.pv, len);
            buf[len] = '\0';
        }
        dst->kind = P5_ARG_STR;
        dst->pv = buf;
        dst->len = buf ? len : 0;
        dst->is_utf8 = STRADA_STR_IS_UTF8(arg) ? 1 : 0;
    } else {
        StradaValue *tmp = fetched;
        fetched = NULL; /* Avoid double-decref if strada_to_str throws */
        if (tmp) strada_inflight_push(tmp);
        char *s = strada_to_str(arg);
        if (tmp) {
            strada_inflight_pop();
            strada_decref(tmp);
        }
        dst->kind = P5_ARG_STR;
        dst->pv = s;
        dst->len = s ? strlen(s) : 0;
        dst->is_utf8 = 0;
        return;
    }
    if (fetched) strada_decref(fetched);
}

static SV* perl5_sv_from_conv(const Perl5ConvArg *ca) {
    if (ca->kind == P5_ARG_INT) {
        return sv_2mortal(newSViv((IV)ca->iv));
    }
    if (ca->kind == P5_ARG_NUM) {
        return sv_2mortal(newSVnv((NV)ca->nv));
    }
    if (ca->kind == P5_ARG_STR) {
        SV *sv = sv_2mortal(newSVpvn(ca->pv ? ca->pv : "", (STRLEN)ca->len));
        if (ca->is_utf8) SvUTF8_on(sv);
        return sv;
    }
    return &PL_sv_undef;
}

static StradaValue* perl5_strada_from_sv(SV *sv) {
    if (!sv || !SvOK(sv)) {
        return strada_new_str("");
    }
    STRLEN len = 0;
    const char *p = SvPV(sv, len);
    if (SvUTF8(sv)) {
        return strada_new_str_len_utf8(p ? p : "", (size_t)len);
    }
    return strada_new_str_len(p ? p : "", (size_t)len);
}

/* Initialize the Perl interpreter */
int strada_perl5_init(void) {
    if (my_perl) {
        return 1;  /* Already initialized */
    }

    int argc = 3;
    char *argv[] = { "", "-e", "0", NULL };
    char **argv_ptr = argv;
    char *env[] = { NULL };
    char **env_ptr = env;

    PERL_SYS_INIT3(&argc, &argv_ptr, &env_ptr);
    my_perl = perl_alloc();
    if (!my_perl) {
        return 0;
    }

    perl_construct(my_perl);
    PL_exit_flags |= PERL_EXIT_DESTRUCT_END;

    /* Use NULL xs_init - XS modules require LD_PRELOAD=/path/to/libperl.so */
    if (perl_parse(my_perl, NULL, argc, argv, NULL) != 0) {
        perl_destruct(my_perl);
        perl_free(my_perl);
        my_perl = NULL;
        return 0;
    }

    perl_run(my_perl);
    return 1;
}

/* Shutdown the Perl interpreter */
void strada_perl5_shutdown(void) {
    if (my_perl) {
        perl_destruct(my_perl);
        perl_free(my_perl);
        PERL_SYS_TERM();
        my_perl = NULL;
    }
}

/* Check if Perl is initialized */
int strada_perl5_is_init(void) {
    return my_perl != NULL;
}

/* Evaluate Perl code, return StradaValue preserving UTF-8 and NUL bytes */
StradaValue* strada_perl5_eval_sv(StradaValue *code_sv) {
    if (!my_perl) {
        return strada_new_str("Error: Perl not initialized");
    }

    Perl5ConvArg ca;
    memset(&ca, 0, sizeof(ca));
    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        perl5_conv_one_arg(code_sv, &ca);
        STRADA_TRY_POP();
    } else {
        STRADA_TRY_POP();
        free(ca.pv);
        strada_throw_value(strada_get_exception());
        return NULL;
    }

    dSP;
    ENTER;
    SAVETMPS;
    SV *sv = perl5_sv_from_conv(&ca);
    free(ca.pv);
    ca.pv = NULL;
    PUTBACK;
    int count = eval_sv(sv, G_SCALAR);
    SPAGAIN;

    StradaValue *out = NULL;
    if (SvTRUE(ERRSV)) {
        if (count > 0) (void)POPs;
        out = perl5_strada_from_sv(ERRSV);
    } else if (count > 0) {
        SV *ret = POPs;
        out = perl5_strada_from_sv(ret);
    } else {
        out = strada_new_str("");
    }

    PUTBACK;
    FREETMPS;
    LEAVE;
    return out;
}

/* Evaluate Perl code, return result as string */
char* strada_perl5_eval(StradaValue *code_sv) {
    StradaValue *sv = strada_perl5_eval_sv(code_sv);
    char *out = strada_to_str(sv);
    strada_decref(sv);
    return out ? out : strdup("");
}

/* Execute Perl code (no return value) */
void strada_perl5_run(StradaValue *code_sv) {
    if (!my_perl) {
        return;
    }

    Perl5ConvArg ca;
    memset(&ca, 0, sizeof(ca));
    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        perl5_conv_one_arg(code_sv, &ca);
        STRADA_TRY_POP();
    } else {
        STRADA_TRY_POP();
        free(ca.pv);
        strada_throw_value(strada_get_exception());
        return;
    }

    dSP;
    ENTER;
    SAVETMPS;
    SV *sv = perl5_sv_from_conv(&ca);
    free(ca.pv);
    ca.pv = NULL;
    PUTBACK;
    eval_sv(sv, G_DISCARD | G_VOID);
    SPAGAIN;
    PUTBACK;
    FREETMPS;
    LEAVE;
}

/* Call a Perl subroutine with arguments, return StradaValue preserving UTF-8 and NUL bytes */
StradaValue* strada_perl5_call_sv(StradaValue *sub_sv, StradaValue *args_sv) {
    if (!my_perl) {
        return strada_new_str("Error: Perl not initialized");
    }

    StradaValue *resolved_args = (args_sv && !STRADA_IS_TAGGED_INT(args_sv) && args_sv->type == STRADA_REF)
        ? (strada_is_slot_ref(args_sv)
            ? (args_sv->value.ptr ? *(StradaValue **)args_sv->value.ptr : NULL)
            : args_sv->value.rv)
        : args_sv;

    size_t argc = 0;
    int is_array = 0;
    if (resolved_args) {
        if (STRADA_IS_TAGGED_INT(resolved_args)) {
            argc = 1;
        } else if (resolved_args->type == STRADA_ARRAY) {
            is_array = 1;
            argc = strada_array_length(resolved_args->value.av);
        } else if (resolved_args->type != STRADA_UNDEF) {
            argc = 1;
        }
    }

    Perl5ConvArg *cargs = argc > 0 ? calloc(argc, sizeof(Perl5ConvArg)) : NULL;
    if (argc > 0 && !cargs) {
        return strada_new_str("Error: Out of memory");
    }

    StradaValue * volatile conv_args = resolved_args;
    char * volatile sub_name = NULL;
    volatile size_t converted = 0;
    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        StradaValue *fetched_sub = NULL;
        if (sub_sv && !STRADA_IS_TAGGED_INT(sub_sv) && sub_sv->meta && sub_sv->meta->is_tied && sub_sv->meta->tied_obj) {
            fetched_sub = strada_tied_scalar_fetch(sub_sv);
            if (fetched_sub) strada_inflight_push(fetched_sub);
            sub_name = strada_to_str(fetched_sub);
            if (fetched_sub) {
                strada_inflight_pop();
                strada_decref(fetched_sub);
            }
        } else {
            sub_name = strada_to_str(sub_sv);
        }

        if (is_array) {
            StradaArray *arr = conv_args->value.av;
            for (size_t i = 0; i < argc; i++) {
                StradaValue *arg = strada_array_get(arr, i);
                perl5_conv_one_arg(arg, &cargs[i]);
                converted = i + 1;
            }
        } else if (argc == 1) {
            perl5_conv_one_arg(conv_args, &cargs[0]);
            converted = 1;
        }
        STRADA_TRY_POP();
    } else {
        STRADA_TRY_POP();
        free(sub_name);
        perl5_conv_args_free(cargs, converted);
        strada_throw_value(strada_get_exception());
        return NULL;
    }

    dSP;
    ENTER;
    SAVETMPS;
    PUSHMARK(SP);

    for (size_t i = 0; i < argc; i++) {
        XPUSHs(perl5_sv_from_conv(&cargs[i]));
    }
    perl5_conv_args_free(cargs, argc);

    PUTBACK;
    int count = call_pv(sub_name ? sub_name : "", G_SCALAR | G_EVAL);
    SPAGAIN;
    free(sub_name);

    StradaValue *result = NULL;
    if (SvTRUE(ERRSV)) {
        if (count > 0) {
            (void)POPs;
        }
        result = perl5_strada_from_sv(ERRSV);
    } else if (count > 0) {
        SV *ret = POPs;
        result = perl5_strada_from_sv(ret);
    } else {
        result = strada_new_str("");
    }

    PUTBACK;
    FREETMPS;
    LEAVE;

    return result ? result : strada_new_str("");
}

/* Call a Perl subroutine with arguments, return scalar result */
char* strada_perl5_call(StradaValue *sub_sv, StradaValue *args_sv) {
    StradaValue *sv = strada_perl5_call_sv(sub_sv, args_sv);
    char *out = strada_to_str(sv);
    strada_decref(sv);
    return out ? out : strdup("");
}

/* Call a Perl subroutine, return array result */
StradaValue* strada_perl5_call_array(StradaValue *sub_sv, StradaValue *args_sv) {
    if (!my_perl) {
        return strada_new_array();
    }

    /* Push arguments from Strada array (guard slot refs & tagged-int odd pointers) */
    StradaValue *resolved_args = (args_sv && !STRADA_IS_TAGGED_INT(args_sv) && args_sv->type == STRADA_REF)
        ? (strada_is_slot_ref(args_sv)
            ? (args_sv->value.ptr ? *(StradaValue **)args_sv->value.ptr : NULL)
            : args_sv->value.rv)
        : args_sv;

    size_t argc = 0;
    if (resolved_args && !STRADA_IS_TAGGED_INT(resolved_args) && resolved_args->type == STRADA_ARRAY) {
        argc = strada_array_length(resolved_args->value.av);
    }

    Perl5ConvArg *cargs = argc > 0 ? calloc(argc, sizeof(Perl5ConvArg)) : NULL;
    if (argc > 0 && !cargs) {
        return strada_new_array();
    }

    char * volatile sub_name = NULL;
    volatile size_t converted = 0;
    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        StradaValue *fetched_sub = NULL;
        if (sub_sv && !STRADA_IS_TAGGED_INT(sub_sv) && sub_sv->meta && sub_sv->meta->is_tied && sub_sv->meta->tied_obj) {
            fetched_sub = strada_tied_scalar_fetch(sub_sv);
            if (fetched_sub) strada_inflight_push(fetched_sub);
            sub_name = strada_to_str(fetched_sub);
            if (fetched_sub) {
                strada_inflight_pop();
                strada_decref(fetched_sub);
            }
        } else {
            sub_name = strada_to_str(sub_sv);
        }

        if (argc > 0) {
            StradaArray *arr = resolved_args->value.av;
            for (size_t i = 0; i < argc; i++) {
                StradaValue *arg = strada_array_get(arr, i);
                perl5_conv_one_arg(arg, &cargs[i]);
                converted = i + 1;
            }
        }
        STRADA_TRY_POP();
    } else {
        STRADA_TRY_POP();
        free(sub_name);
        perl5_conv_args_free(cargs, converted);
        strada_throw_value(strada_get_exception());
        return NULL;
    }

    dSP;
    ENTER;
    SAVETMPS;
    PUSHMARK(SP);

    for (size_t i = 0; i < argc; i++) {
        XPUSHs(perl5_sv_from_conv(&cargs[i]));
    }
    perl5_conv_args_free(cargs, argc);

    PUTBACK;
    int count = call_pv(sub_name ? sub_name : "", G_ARRAY | G_EVAL);
    SPAGAIN;
    free(sub_name);

    StradaValue *result = strada_new_array();

    if (SvTRUE(ERRSV)) {
        for (int i = 0; i < count; i++) {
            (void)POPs;
        }
    } else if (count > 0) {
        /* Pop results in reverse order, store in temp array */
        StradaValue **temp = malloc((size_t)count * sizeof(StradaValue*));
        if (temp) {
            for (int i = 0; i < count; i++) {
                SV *sv = POPs;
                if (SvOK(sv)) {
                    temp[i] = perl5_strada_from_sv(sv);
                } else {
                    temp[i] = strada_new_undef();
                }
            }
            /* Add in correct order (reverse of pop order) */
            for (int i = count - 1; i >= 0; i--) {
                strada_array_push_take(result->value.av, temp[i]);
            }
            free(temp);
        } else {
            for (int i = 0; i < count; i++) {
                (void)POPs;
            }
        }
    }

    PUTBACK;
    FREETMPS;
    LEAVE;

    return result;
}

/* Use a Perl module */
int strada_perl5_use(StradaValue *module_sv) {
    if (!my_perl) {
        return 0;
    }

    char *module = strada_to_str(module_sv);
    if (!perl5_valid_module_name(module)) {
        free(module);
        return 0;
    }
    char code[2048];
    snprintf(code, sizeof(code), "use %s; 1;", module);
    free(module);

    eval_pv(code, FALSE);
    return !SvTRUE(ERRSV);
}

/* Require a Perl module */
int strada_perl5_require(StradaValue *module_sv) {
    if (!my_perl) {
        return 0;
    }

    char *module = strada_to_str(module_sv);
    if (!perl5_valid_module_name(module)) {
        free(module);
        return 0;
    }
    char code[2048];
    snprintf(code, sizeof(code), "require %s; 1;", module);
    free(module);

    eval_pv(code, FALSE);
    return !SvTRUE(ERRSV);
}

/* Set a Perl scalar variable */
void strada_perl5_set_scalar(StradaValue *name_sv, StradaValue *value_sv) {
    if (!my_perl) {
        return;
    }

    char *orig_name = strada_to_str(name_sv);
    const char *name = orig_name;

    /* Skip the sigil if present */
    if (name[0] == '$') {
        name++;
    }

    SV *sv = get_sv(name, GV_ADD);
    if (sv) {
        perl5_set_sv_from_strada(sv, value_sv);
    }
    free(orig_name);
}

/* Get a Perl scalar variable as StradaValue preserving UTF-8 and NUL bytes */
StradaValue* strada_perl5_get_scalar_sv(StradaValue *name_sv) {
    if (!my_perl) {
        return strada_new_str("");
    }

    char *orig_name = strada_to_str(name_sv);
    const char *name = orig_name;

    /* Skip the sigil if present */
    if (name[0] == '$') {
        name++;
    }

    SV *sv = get_sv(name, 0);
    free(orig_name);
    return perl5_strada_from_sv(sv);
}

/* Get a Perl scalar variable */
char* strada_perl5_get_scalar(StradaValue *name_sv) {
    StradaValue *sv = strada_perl5_get_scalar_sv(name_sv);
    char *out = strada_to_str(sv);
    strada_decref(sv);
    return out ? out : strdup("");
}

/* Get the last Perl error ($@) as StradaValue preserving UTF-8 and NUL bytes */
StradaValue* strada_perl5_get_error_sv(void) {
    if (!my_perl) {
        return strada_new_str("Perl not initialized");
    }

    if (SvTRUE(ERRSV)) {
        return perl5_strada_from_sv(ERRSV);
    }

    return strada_new_str("");
}

/* Get the last Perl error ($@) */
char* strada_perl5_get_error(void) {
    StradaValue *sv = strada_perl5_get_error_sv();
    char *out = strada_to_str(sv);
    strada_decref(sv);
    return out ? out : strdup("");
}

/* Add a path to @INC */
void strada_perl5_add_inc(StradaValue *path_sv) {
    if (!my_perl) {
        return;
    }

    char *path = strada_to_str(path_sv);
    perl5_push_inc(path);
    free(path);
}

/* ============== Raw C Functions for extern "C" ============== */

/* Evaluate Perl code, return result as string (raw C types) */
char* strada_perl5_eval_raw(const char *code) {
    if (!my_perl) {
        return strdup("Error: Perl not initialized");
    }

    SV *result = eval_pv(code, FALSE);

    if (SvTRUE(ERRSV)) {
        STRLEN len;
        const char *err = SvPV(ERRSV, len);
        return strdup(err);
    }

    if (result && SvOK(result)) {
        STRLEN len;
        const char *str = SvPV(result, len);
        return strdup(str);
    }

    return strdup("");
}

/* Execute Perl code (no return value) - raw C types */
void strada_perl5_run_raw(const char *code) {
    if (!my_perl) {
        return;
    }
    eval_pv(code, FALSE);
}

/* Use a Perl module - raw C types */
int strada_perl5_use_raw(const char *module) {
    if (!my_perl || !perl5_valid_module_name(module)) {
        return 0;
    }

    char code[2048];
    snprintf(code, sizeof(code), "use %s; 1;", module);

    eval_pv(code, FALSE);
    return !SvTRUE(ERRSV);
}

/* Require a Perl module - raw C types */
int strada_perl5_require_raw(const char *module) {
    if (!my_perl || !perl5_valid_module_name(module)) {
        return 0;
    }

    char code[2048];
    snprintf(code, sizeof(code), "require %s; 1;", module);

    eval_pv(code, FALSE);
    return !SvTRUE(ERRSV);
}

/* Set a Perl scalar variable - raw C types */
void strada_perl5_set_scalar_raw(const char *name, const char *value) {
    if (!my_perl || !name) {
        return;
    }

    /* Skip the sigil if present */
    if (name[0] == '$') {
        name++;
    }

    SV *sv = get_sv(name, GV_ADD);
    if (sv) {
        if (value) {
            sv_setpv(sv, value);
        } else {
            sv_setsv(sv, &PL_sv_undef);
        }
    }
}

/* Get a Perl scalar variable - raw C types */
char* strada_perl5_get_scalar_raw(const char *name) {
    if (!my_perl || !name) {
        return strdup("");
    }

    /* Skip the sigil if present */
    if (name[0] == '$') {
        name++;
    }

    SV *sv = get_sv(name, 0);
    if (sv && SvOK(sv)) {
        STRLEN len;
        const char *str = SvPV(sv, len);
        return strdup(str);
    }

    return strdup("");
}

/* Get the last Perl error ($@) - raw C types */
char* strada_perl5_get_error_raw(void) {
    if (!my_perl) {
        return strdup("Perl not initialized");
    }

    if (SvTRUE(ERRSV)) {
        STRLEN len;
        const char *err = SvPV(ERRSV, len);
        return strdup(err);
    }

    return strdup("");
}

/* Add a path to @INC - raw C types */
void strada_perl5_add_inc_raw(const char *path) {
    if (!my_perl) {
        return;
    }

    perl5_push_inc(path);
}

/* Call a Perl subroutine with a single string argument - raw C types */
char* strada_perl5_call_str_raw(const char *sub_name, const char *arg) {
    if (!my_perl) {
        return strdup("Error: Perl not initialized");
    }

    dSP;
    ENTER;
    SAVETMPS;
    PUSHMARK(SP);

    if (arg != NULL) {
        XPUSHs(sv_2mortal(newSVpv(arg, 0)));
    }

    PUTBACK;
    int count = call_pv(sub_name, G_SCALAR | G_EVAL);
    SPAGAIN;

    char *result = NULL;
    if (SvTRUE(ERRSV)) {
        if (count > 0) {
            (void)POPs;
        }
        STRLEN len;
        const char *err = SvPV(ERRSV, len);
        result = strdup(err);
    } else if (count > 0) {
        SV *ret = POPs;
        if (SvOK(ret)) {
            STRLEN len;
            const char *str = SvPV(ret, len);
            result = strdup(str);
        }
    }

    PUTBACK;
    FREETMPS;
    LEAVE;

    return result ? result : strdup("");
}

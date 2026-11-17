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

/* XS support: register DynaLoader so `use List::Util` / POSIX / JSON::XS can
 * load their shared objects (perl_parse used to get a NULL xs_init, so any
 * XS module failed with "dynamic loading not available"). */
EXTERN_C void boot_DynaLoader(pTHX_ CV *cv);
static void strada_perl5_xs_init(pTHX) {
    static const char file[] = __FILE__;
    dXSUB_SYS;
    newXS("DynaLoader::boot_DynaLoader", boot_DynaLoader, (char *)file);
}

/* PERL_SYS_INIT3 / PERL_SYS_TERM may run only once per process, so a
 * shutdown() followed by init() used to kill the process; do the system
 * init once and the term at exit. */
static int strada_perl5_sys_inited = 0;
static void strada_perl5_sys_term(void) { PERL_SYS_TERM(); }

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
    if (v && !STRADA_IS_TAGGED_INT(v) && v->meta && v->meta->is_tied && v->meta->tied_obj
        && v->type != STRADA_ARRAY && v->type != STRADA_HASH) {
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
        StradaValue *ev = strada_to_str_sv(val);
        size_t len = (ev && !STRADA_IS_TAGGED_INT(ev)) ? STRADA_STR_BYTELEN(ev) : 0;
        sv_setpvn(sv, (ev && !STRADA_IS_TAGGED_INT(ev) && ev->value.pv) ? ev->value.pv : "", (STRLEN)len);
        if (ev && !STRADA_IS_TAGGED_INT(ev) && STRADA_STR_IS_UTF8(ev)) {
            SvUTF8_on(sv);
        } else {
            SvUTF8_off(sv);
        }
        if (ev) strada_decref(ev);
    }
    SvSETMAGIC(sv);
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
    int is_uv;
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
    dst->is_uv = 0;
    if (!arg) return;

    StradaValue *fetched = NULL;
    if (!STRADA_IS_TAGGED_INT(arg) && arg->meta && arg->meta->is_tied && arg->meta->tied_obj
        && arg->type != STRADA_ARRAY && arg->type != STRADA_HASH) {
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
        dst->is_uv = STRADA_INT_IS_UV(arg) ? 1 : 0;
    } else if (arg->type == STRADA_NUM) {
        dst->kind = P5_ARG_NUM;
        dst->nv = arg->value.nv;
    } else if (arg->type == STRADA_STR && !(arg->meta && (arg->meta->blessed_package || arg->meta->is_tied))) {
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
        fetched = NULL; /* Avoid double-decref if strada_to_str_sv throws */
        if (tmp) strada_inflight_push(tmp);
        StradaValue *ev = strada_to_str_sv(arg);
        if (tmp) {
            strada_inflight_pop();
            strada_decref(tmp);
        }
        size_t len = (ev && !STRADA_IS_TAGGED_INT(ev)) ? STRADA_STR_BYTELEN(ev) : 0;
        char *buf = malloc(len + 1);
        if (buf) {
            if (len > 0 && ev && ev->value.pv) memcpy(buf, ev->value.pv, len);
            buf[len] = '\0';
        }
        dst->kind = P5_ARG_STR;
        dst->pv = buf;
        dst->len = buf ? len : 0;
        dst->is_utf8 = (ev && !STRADA_IS_TAGGED_INT(ev) && STRADA_STR_IS_UTF8(ev)) ? 1 : 0;
        if (ev) strada_decref(ev);
        return;
    }
    if (fetched) strada_decref(fetched);
}

static SV* perl5_sv_from_conv(const Perl5ConvArg *ca) {
    if (ca->kind == P5_ARG_INT) {
        return sv_2mortal(ca->is_uv ? newSVuv((UV)(uint64_t)ca->iv) : newSViv((IV)ca->iv));
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

static StradaValue* perl5_strada_from_sv_nomg(SV *sv) {
    if (!sv || !SvOK(sv)) {
        return strada_new_str("");
    }
    STRLEN len = 0;
    const char *p = SvPV_nomg(sv, len);
    if (SvUTF8(sv)) {
        return strada_new_str_len_utf8(p ? p : "", (size_t)len);
    }
    return strada_new_str_len(p ? p : "", (size_t)len);
}

static StradaValue* perl5_strada_from_sv(SV *sv) {
    if (!sv) {
        return strada_new_str("");
    }
    SvGETMAGIC(sv);
    return perl5_strada_from_sv_nomg(sv);
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

    if (!strada_perl5_sys_inited) {
        PERL_SYS_INIT3(&argc, &argv_ptr, &env_ptr);
        strada_perl5_sys_inited = 1;
        atexit(strada_perl5_sys_term);
    }
    my_perl = perl_alloc();
    if (!my_perl) {
        return 0;
    }

    perl_construct(my_perl);
    PL_exit_flags |= PERL_EXIT_DESTRUCT_END;

    if (perl_parse(my_perl, strada_perl5_xs_init, argc, argv, NULL) != 0) {
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
    (void)sp;
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
    (void)sp;
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

static int perl5_prepare_call_args(StradaValue *sub_sv, StradaValue *args_sv, int allow_single,
                                   char **out_sub_name, Perl5ConvArg **out_cargs, size_t *out_argc) {
    *out_sub_name = NULL;
    *out_cargs = NULL;
    *out_argc = 0;

    int cm = strada_cleanup_mark();
    int lm = strada_local_depth_get();
    char * volatile sub_name = NULL;
    Perl5ConvArg * volatile cargs = NULL;
    volatile size_t argc = 0;
    volatile size_t converted = 0;
    volatile int oom = 0;

    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        if (sub_sv && !STRADA_IS_TAGGED_INT(sub_sv) && sub_sv->meta && sub_sv->meta->is_tied
            && sub_sv->meta->tied_obj && sub_sv->type != STRADA_ARRAY && sub_sv->type != STRADA_HASH) {
            StradaValue *fetched_sub = strada_tied_scalar_fetch(sub_sv);
            if (fetched_sub) strada_cleanup_push(fetched_sub);
            sub_name = strada_to_str(fetched_sub);
            if (fetched_sub) strada_cleanup_pop_decref(fetched_sub);
        } else {
            sub_name = strada_to_str(sub_sv);
        }

        StradaValue *resolved_args = args_sv;
        for (int depth = 0; depth < 10 && resolved_args && !STRADA_IS_TAGGED_INT(resolved_args)
                            && resolved_args->type == STRADA_REF; depth++) {
            resolved_args = strada_is_slot_ref(resolved_args)
                ? (resolved_args->value.ptr ? *(StradaValue **)resolved_args->value.ptr : NULL)
                : resolved_args->value.rv;
        }
        if (resolved_args && !STRADA_IS_TAGGED_INT(resolved_args) && resolved_args->meta
            && resolved_args->meta->is_tied && resolved_args->meta->tied_obj
            && resolved_args->type != STRADA_ARRAY && resolved_args->type != STRADA_HASH) {
            StradaValue *fetched_args = strada_tied_scalar_fetch(resolved_args);
            if (fetched_args) strada_cleanup_push(fetched_args);
            resolved_args = fetched_args;
            for (int depth = 0; depth < 10 && resolved_args && !STRADA_IS_TAGGED_INT(resolved_args)
                                && resolved_args->type == STRADA_REF; depth++) {
                resolved_args = strada_is_slot_ref(resolved_args)
                    ? (resolved_args->value.ptr ? *(StradaValue **)resolved_args->value.ptr : NULL)
                    : resolved_args->value.rv;
            }
        }

        int is_array = 0;
        int is_tied_arr = 0;
        if (resolved_args && !STRADA_IS_TAGGED_INT(resolved_args) && resolved_args->type == STRADA_ARRAY) {
            is_array = 1;
            if (resolved_args->meta && resolved_args->meta->is_tied) {
                is_tied_arr = 1;
                int64_t tlen = strada_tied_array_fetchsize(resolved_args);
                argc = tlen > 0 ? (size_t)tlen : 0;
            } else {
                argc = strada_array_length(resolved_args->value.av);
            }
        } else if (allow_single && resolved_args) {
            if (STRADA_IS_TAGGED_INT(resolved_args) || resolved_args->type != STRADA_UNDEF) {
                argc = 1;
            }
        }

        if (argc > 0) {
            cargs = calloc(argc, sizeof(Perl5ConvArg));
            if (!cargs) {
                oom = 1;
            } else if (is_array) {
                if (is_tied_arr) {
                    for (size_t i = 0; i < argc; i++) {
                        StradaValue *el = strada_tied_array_fetch(resolved_args, (int64_t)i);
                        if (el) strada_cleanup_push(el);
                        perl5_conv_one_arg(el, &cargs[i]);
                        if (el) strada_cleanup_pop_decref(el);
                        converted = i + 1;
                    }
                } else {
                    StradaArray *arr = resolved_args->value.av;
                    for (size_t i = 0; i < argc; i++) {
                        perl5_conv_one_arg(strada_array_get(arr, i), &cargs[i]);
                        converted = i + 1;
                    }
                }
            } else {
                perl5_conv_one_arg(resolved_args, &cargs[0]);
                converted = 1;
            }
        }
        STRADA_TRY_POP();
        strada_cleanup_drain_to(cm);
    } else {
        STRADA_TRY_POP();
        strada_local_restore_to(lm);
        strada_cleanup_drain_to(cm);
        free(sub_name);
        perl5_conv_args_free(cargs, converted);
        strada_throw_value(strada_get_exception());
        return -1;
    }

    if (oom) {
        free(sub_name);
        return 0;
    }
    *out_sub_name = sub_name;
    *out_cargs = cargs;
    *out_argc = argc;
    return 1;
}

/* Call a Perl subroutine with arguments, return StradaValue preserving UTF-8 and NUL bytes */
StradaValue* strada_perl5_call_sv(StradaValue *sub_sv, StradaValue *args_sv) {
    if (!my_perl) {
        return strada_new_str("Error: Perl not initialized");
    }

    char *sub_name = NULL;
    Perl5ConvArg *cargs = NULL;
    size_t argc = 0;
    int prep = perl5_prepare_call_args(sub_sv, args_sv, 1, &sub_name, &cargs, &argc);
    if (prep == 0) {
        return strada_new_str("Error: Out of memory");
    }
    if (prep < 0) {
        return NULL;
    }

    dSP;
    (void)sp;
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

    char *sub_name = NULL;
    Perl5ConvArg *cargs = NULL;
    size_t argc = 0;
    int prep = perl5_prepare_call_args(sub_sv, args_sv, 0, &sub_name, &cargs, &argc);
    if (prep == 0) {
        return strada_new_array();
    }
    if (prep < 0) {
        return NULL;
    }

    dSP;
    (void)sp;
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
                if (sv) SvGETMAGIC(sv);
                if (sv && SvOK(sv)) {
                    temp[i] = perl5_strada_from_sv_nomg(sv);
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

static int perl5_eval_mod_cmd(const char *verb, const char *mod) {
    if (!my_perl || !perl5_valid_module_name(mod)) {
        return 0;
    }
    char code[2048];
    snprintf(code, sizeof(code), "%s %s; 1;", verb, mod);
    dSP;
    (void)sp;
    ENTER;
    SAVETMPS;
    eval_pv(code, FALSE);
    int ok = !SvTRUE(ERRSV);
    FREETMPS;
    LEAVE;
    return ok;
}

/* Use a Perl module */
int strada_perl5_use(StradaValue *module_sv) {
    if (!my_perl) {
        return 0;
    }
    char *module = strada_to_str(module_sv);
    int ok = perl5_eval_mod_cmd("use", module);
    free(module);
    return ok;
}

/* Require a Perl module */
int strada_perl5_require(StradaValue *module_sv) {
    if (!my_perl) {
        return 0;
    }
    char *module = strada_to_str(module_sv);
    int ok = perl5_eval_mod_cmd("require", module);
    free(module);
    return ok;
}

/* Set a Perl scalar variable */
void strada_perl5_set_scalar(StradaValue *name_sv, StradaValue *value_sv) {
    if (!my_perl) {
        return;
    }

    char *orig_name = strada_to_str(name_sv);
    const char *name = orig_name ? orig_name : "";

    /* Skip the sigil if present */
    if (name[0] == '$') {
        name++;
    }

    SV *sv = get_sv(name, GV_ADD);
    free(orig_name);
    if (sv) {
        perl5_set_sv_from_strada(sv, value_sv);
    }
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

    char *ret = NULL;
    dSP;
    (void)sp;
    ENTER;
    SAVETMPS;
    SV *result = eval_pv(code, FALSE);

    if (SvTRUE(ERRSV)) {
        STRLEN len;
        const char *err = SvPV_nomg(ERRSV, len);
        (void)len;
        ret = strdup(err);
    } else if (result) {
        SvGETMAGIC(result);
        if (SvOK(result)) {
            STRLEN len;
            const char *str = SvPV_nomg(result, len);
            (void)len;
            ret = strdup(str);
        }
    }
    FREETMPS;
    LEAVE;

    return ret ? ret : strdup("");
}

/* Execute Perl code (no return value) - raw C types */
void strada_perl5_run_raw(const char *code) {
    if (!my_perl) {
        return;
    }
    dSP;
    (void)sp;
    ENTER;
    SAVETMPS;
    eval_pv(code, FALSE);
    FREETMPS;
    LEAVE;
}

/* Use a Perl module - raw C types */
int strada_perl5_use_raw(const char *module) {
    return perl5_eval_mod_cmd("use", module);
}

/* Require a Perl module - raw C types */
int strada_perl5_require_raw(const char *module) {
    return perl5_eval_mod_cmd("require", module);
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
        SvSETMAGIC(sv);
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
    if (sv) {
        SvGETMAGIC(sv);
        if (SvOK(sv)) {
            STRLEN len;
            const char *str = SvPV_nomg(sv, len);
            return strdup(str);
        }
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
        const char *err = SvPV_nomg(ERRSV, len);
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
    (void)sp;
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
        const char *err = SvPV_nomg(ERRSV, len);
        result = strdup(err);
    } else if (count > 0) {
        SV *ret = POPs;
        if (ret) {
            SvGETMAGIC(ret);
            if (SvOK(ret)) {
                STRLEN len;
                const char *str = SvPV_nomg(ret, len);
                result = strdup(str);
            }
        }
    }

    PUTBACK;
    FREETMPS;
    LEAVE;

    return result ? result : strdup("");
}

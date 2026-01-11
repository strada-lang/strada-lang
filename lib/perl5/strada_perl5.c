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

/*
 * strada_perl5.c - Perl 5 embedding for Strada
 *
 * This module allows Strada programs to embed a Perl 5 interpreter
 * and call Perl code/subroutines from Strada.
 */

#define _GNU_SOURCE
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

/* Evaluate Perl code, return result as string */
char* strada_perl5_eval(StradaValue *code_sv) {
    if (!my_perl) {
        return strdup("Error: Perl not initialized");
    }

    const char *code = strada_to_str(code_sv);

    SV *result = eval_pv(code, FALSE);

    if (SvTRUE(ERRSV)) {
        /* Return error message */
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

/* Execute Perl code (no return value) */
void strada_perl5_run(StradaValue *code_sv) {
    if (!my_perl) {
        return;
    }

    const char *code = strada_to_str(code_sv);
    eval_pv(code, FALSE);
}

/* Call a Perl subroutine with arguments, return scalar result */
char* strada_perl5_call(StradaValue *sub_sv, StradaValue *args_sv) {
    if (!my_perl) {
        return strdup("Error: Perl not initialized");
    }

    const char *sub_name = strada_to_str(sub_sv);

    dSP;
    ENTER;
    SAVETMPS;
    PUSHMARK(SP);

    /* Push arguments from Strada array */
    int pushed = 0;
    if (args_sv) {
        /* Dereference if it's a reference */
        if (args_sv->type == STRADA_REF) {
            args_sv = args_sv->value.rv;
        }
        if (args_sv->type == STRADA_ARRAY) {
            StradaArray *arr = args_sv->value.av;
            size_t len = strada_array_length(arr);
            for (size_t i = 0; i < len; i++) {
                StradaValue *arg = strada_array_get(arr, i);
                if (arg->type == STRADA_INT) {
                    XPUSHs(sv_2mortal(newSViv(arg->value.iv)));
                } else if (arg->type == STRADA_NUM) {
                    XPUSHs(sv_2mortal(newSVnv(arg->value.nv)));
                } else {
                    const char *str = strada_to_str(arg);
                    XPUSHs(sv_2mortal(newSVpv(str, 0)));
                }
                pushed++;
            }
        } else if (args_sv->type == STRADA_STR || args_sv->type == STRADA_INT || args_sv->type == STRADA_NUM) {
            /* Single scalar argument */
            if (args_sv->type == STRADA_INT) {
                XPUSHs(sv_2mortal(newSViv(args_sv->value.iv)));
            } else if (args_sv->type == STRADA_NUM) {
                XPUSHs(sv_2mortal(newSVnv(args_sv->value.nv)));
            } else {
                const char *str = strada_to_str(args_sv);
                XPUSHs(sv_2mortal(newSVpv(str, 0)));
            }
            pushed++;
        }
    }

    PUTBACK;
    int count = call_pv(sub_name, G_SCALAR | G_EVAL);
    SPAGAIN;

    char *result = NULL;
    if (SvTRUE(ERRSV)) {
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

/* Call a Perl subroutine, return array result */
StradaValue* strada_perl5_call_array(StradaValue *sub_sv, StradaValue *args_sv) {
    if (!my_perl) {
        return strada_new_array();
    }

    const char *sub_name = strada_to_str(sub_sv);

    dSP;
    ENTER;
    SAVETMPS;
    PUSHMARK(SP);

    /* Push arguments from Strada array */
    if (args_sv && args_sv->type == STRADA_REF) {
        args_sv = args_sv->value.rv;
    }
    if (args_sv && args_sv->type == STRADA_ARRAY) {
        StradaArray *arr = args_sv->value.av;
        size_t len = strada_array_length(arr);
        for (size_t i = 0; i < len; i++) {
            StradaValue *arg = strada_array_get(arr, i);
            if (arg->type == STRADA_INT) {
                XPUSHs(sv_2mortal(newSViv(arg->value.iv)));
            } else if (arg->type == STRADA_NUM) {
                XPUSHs(sv_2mortal(newSVnv(arg->value.nv)));
            } else {
                XPUSHs(sv_2mortal(newSVpv(strada_to_str(arg), 0)));
            }
        }
    }

    PUTBACK;
    int count = call_pv(sub_name, G_ARRAY | G_EVAL);
    SPAGAIN;

    StradaValue *result = strada_new_array();

    if (!SvTRUE(ERRSV)) {
        /* Pop results in reverse order, store in temp array */
        char **temp = malloc(count * sizeof(char*));
        for (int i = 0; i < count; i++) {
            SV *sv = POPs;
            if (SvOK(sv)) {
                STRLEN len;
                const char *str = SvPV(sv, len);
                temp[i] = strdup(str);
            } else {
                temp[i] = strdup("");
            }
        }
        /* Add in correct order (reverse of pop order) */
        for (int i = count - 1; i >= 0; i--) {
            strada_array_push(result->value.av, strada_new_str(temp[i]));
            free(temp[i]);
        }
        free(temp);
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

    const char *module = strada_to_str(module_sv);
    char code[2048];
    snprintf(code, sizeof(code), "use %s; 1;", module);

    eval_pv(code, FALSE);
    return !SvTRUE(ERRSV);
}

/* Require a Perl module */
int strada_perl5_require(StradaValue *module_sv) {
    if (!my_perl) {
        return 0;
    }

    const char *module = strada_to_str(module_sv);
    char code[2048];
    snprintf(code, sizeof(code), "require %s; 1;", module);

    eval_pv(code, FALSE);
    return !SvTRUE(ERRSV);
}

/* Set a Perl scalar variable */
void strada_perl5_set_scalar(StradaValue *name_sv, StradaValue *value_sv) {
    if (!my_perl) {
        return;
    }

    const char *name = strada_to_str(name_sv);
    const char *value = strada_to_str(value_sv);

    /* Skip the sigil if present */
    if (name[0] == '$') {
        name++;
    }

    SV *sv = get_sv(name, GV_ADD);
    sv_setpv(sv, value);
}

/* Get a Perl scalar variable */
char* strada_perl5_get_scalar(StradaValue *name_sv) {
    if (!my_perl) {
        return strdup("");
    }

    const char *name = strada_to_str(name_sv);

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

/* Get the last Perl error ($@) */
char* strada_perl5_get_error(void) {
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

/* Add a path to @INC */
void strada_perl5_add_inc(StradaValue *path_sv) {
    if (!my_perl) {
        return;
    }

    const char *path = strada_to_str(path_sv);
    char code[2048];
    snprintf(code, sizeof(code), "push @INC, '%s';", path);
    eval_pv(code, FALSE);
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
    if (!my_perl) {
        return 0;
    }

    char code[2048];
    snprintf(code, sizeof(code), "use %s; 1;", module);

    eval_pv(code, FALSE);
    return !SvTRUE(ERRSV);
}

/* Require a Perl module - raw C types */
int strada_perl5_require_raw(const char *module) {
    if (!my_perl) {
        return 0;
    }

    char code[2048];
    snprintf(code, sizeof(code), "require %s; 1;", module);

    eval_pv(code, FALSE);
    return !SvTRUE(ERRSV);
}

/* Set a Perl scalar variable - raw C types */
void strada_perl5_set_scalar_raw(const char *name, const char *value) {
    if (!my_perl) {
        return;
    }

    /* Skip the sigil if present */
    if (name[0] == '$') {
        name++;
    }

    SV *sv = get_sv(name, GV_ADD);
    sv_setpv(sv, value);
}

/* Get a Perl scalar variable - raw C types */
char* strada_perl5_get_scalar_raw(const char *name) {
    if (!my_perl) {
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

    char code[2048];
    snprintf(code, sizeof(code), "push @INC, '%s';", path);
    eval_pv(code, FALSE);
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

    if (arg && strlen(arg) > 0) {
        XPUSHs(sv_2mortal(newSVpv(arg, 0)));
    }

    PUTBACK;
    int count = call_pv(sub_name, G_SCALAR | G_EVAL);
    SPAGAIN;

    char *result = NULL;
    if (SvTRUE(ERRSV)) {
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

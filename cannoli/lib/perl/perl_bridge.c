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
 * perl_bridge.c - Minimal C bridge for Perl embedding
 *
 * This provides low-level Perl API access for use from Strada.
 * Build as a shared library: libperl_bridge.so
 *
 * Functions accept StradaValue* to avoid FFI string conversion issues.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <EXTERN.h>
#include <perl.h>

/* Forward declaration - Strada runtime provides this */
extern const char* strada_to_str(void *sv);

/* Helper to safely get C string from StradaValue */
static const char* sv_to_cstr(void *sv) {
    if (!sv) return NULL;
    return strada_to_str(sv);
}

/* Global Perl interpreter */
static PerlInterpreter *my_perl = NULL;

/* Handler subroutine name (set by Strada during init) */
static char *handler_sub = NULL;

/* Path to Cannoli.pm module */
static char *cannoli_pm_path = NULL;

/* Set handler name (called from Strada init) */
void perl_bridge_set_handler(void *handler_sv) {
    const char *h = sv_to_cstr(handler_sv);
    if (handler_sub) free(handler_sub);
    handler_sub = h ? strdup(h) : NULL;
}

/* Set Cannoli.pm path (called from Strada init) */
void perl_bridge_set_cannoli_path(void *path_sv) {
    const char *p = sv_to_cstr(path_sv);
    if (cannoli_pm_path) free(cannoli_pm_path);
    cannoli_pm_path = p ? strdup(p) : NULL;
}

/* Get handler name */
const char* perl_bridge_get_handler(void) {
    return handler_sub;
}

/* Initialize the Perl interpreter (unused param for Strada FFI compatibility) */
int perl_bridge_init(const char *unused) {
    (void)unused;  /* Silence warning */
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
        fprintf(stderr, "perl_bridge: Failed to allocate interpreter\n");
        return 0;
    }

    perl_construct(my_perl);
    PL_exit_flags |= PERL_EXIT_DESTRUCT_END;

    if (perl_parse(my_perl, NULL, argc, argv, NULL) != 0) {
        perl_destruct(my_perl);
        perl_free(my_perl);
        my_perl = NULL;
        fprintf(stderr, "perl_bridge: Failed to parse\n");
        return 0;
    }

    perl_run(my_perl);
    return 1;
}

/* Check if Perl is initialized */
int perl_bridge_is_init(void) {
    return my_perl != NULL;
}

/* Shutdown the Perl interpreter */
void perl_bridge_shutdown(void) {
    if (my_perl) {
        perl_destruct(my_perl);
        perl_free(my_perl);
        PERL_SYS_TERM();
        my_perl = NULL;
    }
    if (handler_sub) {
        free(handler_sub);
        handler_sub = NULL;
    }
    if (cannoli_pm_path) {
        free(cannoli_pm_path);
        cannoli_pm_path = NULL;
    }
}

/* Add a path to @INC (accepts StradaValue*) */
int perl_bridge_add_inc(void *path_sv) {
    const char *path = sv_to_cstr(path_sv);
    if (!my_perl || !path) return 0;

    char code[2048];
    snprintf(code, sizeof(code), "push @INC, '%s'; 1;", path);
    eval_pv(code, FALSE);

    if (SvTRUE(ERRSV)) {
        fprintf(stderr, "perl_bridge: add_inc error: %s\n", SvPV_nolen(ERRSV));
        return 0;
    }
    return 1;
}

/* Use a Perl module (accepts StradaValue*) */
int perl_bridge_use(void *module_sv) {
    const char *module = sv_to_cstr(module_sv);
    if (!my_perl || !module) return 0;

    char code[2048];
    snprintf(code, sizeof(code), "use %s; 1;", module);
    eval_pv(code, FALSE);

    if (SvTRUE(ERRSV)) {
        fprintf(stderr, "perl_bridge: use error: %s\n", SvPV_nolen(ERRSV));
        return 0;
    }
    return 1;
}

/* Load a Perl script with 'do' (accepts StradaValue*) */
int perl_bridge_do(void *script_sv) {
    const char *script = sv_to_cstr(script_sv);
    if (!my_perl) {
        fprintf(stderr, "perl_bridge_do: Perl not initialized!\n");
        return 0;
    }
    if (!script) {
        fprintf(stderr, "perl_bridge_do: script is NULL!\n");
        return 0;
    }

    char code[4096];
    snprintf(code, sizeof(code), "do '%s'; die $@ if $@; 1;", script);
    eval_pv(code, FALSE);

    if (SvTRUE(ERRSV)) {
        fprintf(stderr, "perl_bridge: do error: %s\n", SvPV_nolen(ERRSV));
        return 0;
    }
    return 1;
}

/* Load Cannoli.pm module */
int perl_bridge_load_cannoli(void *path_sv) {
    const char *path = sv_to_cstr(path_sv);
    if (!my_perl) return 0;

    /* Store the path */
    if (cannoli_pm_path) free(cannoli_pm_path);
    cannoli_pm_path = path ? strdup(path) : NULL;

    /* Load Cannoli.pm */
    char code[4096];
    if (path && strlen(path) > 0) {
        snprintf(code, sizeof(code), "require '%s'; 1;", path);
    } else {
        snprintf(code, sizeof(code), "use Cannoli; 1;");
    }
    eval_pv(code, FALSE);

    if (SvTRUE(ERRSV)) {
        fprintf(stderr, "perl_bridge: load Cannoli error: %s\n", SvPV_nolen(ERRSV));
        return 0;
    }
    return 1;
}

/* Parse headers string into Perl hash
 * Format: "header-name:value\nheader-name:value\n..."
 */
static HV* parse_headers_to_hv(const char *headers_str) {
    HV *hv = newHV();
    if (!headers_str || !*headers_str) return hv;

    const char *p = headers_str;
    while (*p) {
        /* Find colon */
        const char *colon = strchr(p, ':');
        if (!colon) break;

        /* Find end of line */
        const char *eol = strchr(colon, '\n');
        if (!eol) eol = p + strlen(p);

        /* Extract name (lowercase) and value */
        size_t name_len = colon - p;
        char *name = malloc(name_len + 1);
        for (size_t i = 0; i < name_len; i++) {
            name[i] = (p[i] >= 'A' && p[i] <= 'Z') ? p[i] + 32 : p[i];
        }
        name[name_len] = '\0';

        /* Skip colon and leading space */
        const char *val_start = colon + 1;
        while (*val_start == ' ') val_start++;

        size_t val_len = eol - val_start;
        /* Trim trailing \r */
        while (val_len > 0 && (val_start[val_len-1] == '\r' || val_start[val_len-1] == '\n')) {
            val_len--;
        }

        /* Store in hash */
        hv_store(hv, name, name_len, newSVpvn(val_start, val_len), 0);
        free(name);

        /* Move to next line */
        p = (*eol) ? eol + 1 : eol;
    }
    return hv;
}

/* Call handler with Cannoli object (new interface)
 * Returns response string (caller must free)
 */
char* perl_bridge_call_handler(
    void *sub_name_sv,
    void *method_sv,
    void *path_sv,
    void *path_info_sv,
    void *query_string_sv,
    void *body_sv,
    void *headers_sv,
    void *remote_addr_sv,
    void *content_type_sv
) {
    const char *sub_name = sv_to_cstr(sub_name_sv);
    const char *method = sv_to_cstr(method_sv);
    const char *path = sv_to_cstr(path_sv);
    const char *path_info = sv_to_cstr(path_info_sv);
    const char *query_string = sv_to_cstr(query_string_sv);
    const char *body = sv_to_cstr(body_sv);
    const char *headers_str = sv_to_cstr(headers_sv);
    const char *remote_addr = sv_to_cstr(remote_addr_sv);
    const char *content_type = sv_to_cstr(content_type_sv);

    if (!my_perl || !sub_name) return strdup("");

    dSP;
    ENTER;
    SAVETMPS;

    /* Parse headers into hash */
    HV *headers_hv = parse_headers_to_hv(headers_str);

    /* Create Cannoli object: Cannoli->new(...) */
    PUSHMARK(SP);
    XPUSHs(sv_2mortal(newSVpv("Cannoli", 0)));
    XPUSHs(sv_2mortal(newSVpv("method", 0)));
    XPUSHs(sv_2mortal(newSVpv(method ? method : "GET", 0)));
    XPUSHs(sv_2mortal(newSVpv("path", 0)));
    XPUSHs(sv_2mortal(newSVpv(path ? path : "/", 0)));
    XPUSHs(sv_2mortal(newSVpv("path_info", 0)));
    XPUSHs(sv_2mortal(newSVpv(path_info ? path_info : "", 0)));
    XPUSHs(sv_2mortal(newSVpv("query_string", 0)));
    XPUSHs(sv_2mortal(newSVpv(query_string ? query_string : "", 0)));
    XPUSHs(sv_2mortal(newSVpv("body", 0)));
    XPUSHs(sv_2mortal(newSVpv(body ? body : "", 0)));
    XPUSHs(sv_2mortal(newSVpv("headers", 0)));
    XPUSHs(sv_2mortal(newRV_noinc((SV*)headers_hv)));
    XPUSHs(sv_2mortal(newSVpv("remote_addr", 0)));
    XPUSHs(sv_2mortal(newSVpv(remote_addr ? remote_addr : "", 0)));
    XPUSHs(sv_2mortal(newSVpv("content_type", 0)));
    XPUSHs(sv_2mortal(newSVpv(content_type ? content_type : "", 0)));
    PUTBACK;

    int count = call_method("new", G_SCALAR | G_EVAL);
    SPAGAIN;

    if (SvTRUE(ERRSV) || count < 1) {
        if (SvTRUE(ERRSV)) {
            fprintf(stderr, "perl_bridge: Cannoli->new error: %s\n", SvPV_nolen(ERRSV));
        }
        PUTBACK;
        FREETMPS;
        LEAVE;
        return strdup("STATUS:500:Failed to create Cannoli object");
    }

    SV *cannoli_obj = POPs;
    SvREFCNT_inc(cannoli_obj);  /* Keep it alive */
    PUTBACK;

    /* Call the handler: handler($cannoli_obj) */
    PUSHMARK(SP);
    XPUSHs(cannoli_obj);
    PUTBACK;

    count = call_pv(sub_name, G_SCALAR | G_EVAL);
    SPAGAIN;

    char *result = NULL;

    if (SvTRUE(ERRSV)) {
        /* Handler threw an error */
        STRLEN len;
        const char *err = SvPV(ERRSV, len);
        result = malloc(len + 32);
        snprintf(result, len + 32, "STATUS:500:%s", err);
        fprintf(stderr, "perl_bridge: handler error: %s\n", err);
    } else {
        /* Discard handler return value */
        if (count > 0) POPs;
        PUTBACK;

        /* Extract response: $cannoli_obj->_build_response() */
        PUSHMARK(SP);
        XPUSHs(cannoli_obj);
        PUTBACK;

        count = call_method("_build_response", G_SCALAR | G_EVAL);
        SPAGAIN;

        if (SvTRUE(ERRSV)) {
            STRLEN len;
            const char *err = SvPV(ERRSV, len);
            result = malloc(len + 32);
            snprintf(result, len + 32, "STATUS:500:%s", err);
        } else if (count > 0) {
            SV *ret = POPs;
            if (SvOK(ret)) {
                STRLEN len;
                const char *str = SvPV(ret, len);
                result = malloc(len + 1);
                memcpy(result, str, len);
                result[len] = '\0';
            }
        }
    }

    SvREFCNT_dec(cannoli_obj);
    PUTBACK;
    FREETMPS;
    LEAVE;

    return result ? result : strdup("");
}

/* Legacy call4 function - kept for backward compatibility */
char* perl_bridge_call4(void *sub_name_sv,
                        void *arg1_sv, void *arg2_sv,
                        void *arg3_sv, void *arg4_sv) {
    const char *sub_name = sv_to_cstr(sub_name_sv);
    const char *arg1 = sv_to_cstr(arg1_sv);
    const char *arg2 = sv_to_cstr(arg2_sv);
    const char *arg3 = sv_to_cstr(arg3_sv);
    const char *arg4 = sv_to_cstr(arg4_sv);

    if (!my_perl || !sub_name) return strdup("");

    dSP;
    ENTER;
    SAVETMPS;
    PUSHMARK(SP);

    /* Push 4 string arguments */
    XPUSHs(sv_2mortal(newSVpv(arg1 ? arg1 : "", 0)));
    XPUSHs(sv_2mortal(newSVpv(arg2 ? arg2 : "", 0)));
    XPUSHs(sv_2mortal(newSVpv(arg3 ? arg3 : "", 0)));
    XPUSHs(sv_2mortal(newSVpv(arg4 ? arg4 : "", 0)));

    PUTBACK;
    int count = call_pv(sub_name, G_SCALAR | G_EVAL);
    SPAGAIN;

    char *result = NULL;

    if (SvTRUE(ERRSV)) {
        STRLEN len;
        const char *err = SvPV(ERRSV, len);
        result = malloc(len + 16);
        snprintf(result, len + 16, "STATUS:500:%s", err);
    } else if (count > 0) {
        SV *ret = POPs;
        if (SvOK(ret)) {
            STRLEN len;
            const char *str = SvPV(ret, len);
            result = malloc(len + 1);
            memcpy(result, str, len);
            result[len] = '\0';
        }
    }

    PUTBACK;
    FREETMPS;
    LEAVE;

    return result ? result : strdup("");
}

/* Evaluate Perl code and return result as string (caller must free) */
char* perl_bridge_eval(const char *code) {
    if (!my_perl || !code) return strdup("");

    SV *result = eval_pv(code, FALSE);

    if (SvTRUE(ERRSV)) {
        STRLEN len;
        const char *err = SvPV(ERRSV, len);
        char *error_str = malloc(len + 16);
        snprintf(error_str, len + 16, "ERROR:%s", err);
        return error_str;
    }

    if (result && SvOK(result)) {
        STRLEN len;
        const char *str = SvPV(result, len);
        char *ret = malloc(len + 1);
        memcpy(ret, str, len);
        ret[len] = '\0';
        return ret;
    }

    return strdup("");
}

/* Get the last Perl error */
char* perl_bridge_get_error(void) {
    if (!my_perl) return strdup("Perl not initialized");

    if (SvTRUE(ERRSV)) {
        STRLEN len;
        const char *err = SvPV(ERRSV, len);
        return strdup(err);
    }
    return strdup("");
}

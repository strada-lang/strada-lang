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

/* test_runtime.c - Test the Strada runtime */
#include "strada_runtime.h"
#include <assert.h>
#include <unistd.h>
#include <signal.h>
#include <arpa/inet.h>
#include <sys/wait.h>

static int g_rt1_boom_armed = 0;
static StradaValue *g_rt1_resurrect_arr = NULL;
static int g_rt1_res_armed = 0;

static StradaValue* r10_boom_destroy(StradaValue *self, StradaValue *args) {
    (void)args;
    if (g_rt1_boom_armed) {
        g_rt1_boom_armed = 0;
        strada_die_sv(self);
    }
    return strada_new_undef();
}

static StradaValue* r10_res_destroy(StradaValue *self, StradaValue *args) {
    (void)args;
    if (g_rt1_res_armed && g_rt1_resurrect_arr) {
        g_rt1_res_armed = 0;
        strada_array_push(g_rt1_resurrect_arr->value.av, self);
    }
    return strada_new_undef();
}

static StradaValue *g_rt4_sv = NULL;
static int g_rt4_reenter = 0;
static int g_rt4_compound_phase = 0;

static StradaValue* r10_guard_destroy(StradaValue *self, StradaValue *args) {
    (void)self;
    (void)args;
    if (g_rt4_sv) {
        strada_untie(g_rt4_sv);
    }
    return strada_new_undef();
}

static StradaValue* r10_tie4_new(StradaValue *cls, StradaValue *args) {
    (void)args;
    const char *cname = cls ? strada_to_str(cls) : "R10Tie4";
    StradaValue *h = strada_new_hash();
    StradaValue *res = strada_bless(h, cname);
    if (cls) strada_decref((StradaValue *)cname); /* strada_to_str returns owned char* */
    return res;
}

static StradaValue* r10_tie4_tiescalar(StradaValue *cls, StradaValue *args) {
    (void)args;
    char *cname = strada_to_str(cls);
    StradaValue *h = strada_new_hash();
    StradaValue *res = strada_bless(h, cname ? cname : "R10Tie4");
    free(cname);
    return res;
}

static StradaValue* r10_tie4_store(StradaValue *self, StradaValue *args) {
    (void)self;
    (void)args;
    return strada_new_undef();
}

static StradaValue* r10_tie4_fetch(StradaValue *self, StradaValue *args) {
    (void)self;
    (void)args;
    if (g_rt4_reenter == 0) {
        g_rt4_reenter = 1;
        StradaValue *inner = strada_tied_scalar_fetch(g_rt4_sv);
        strada_decref(inner);
        return strada_new_str("rt4_fresh_val");
    }
    return strada_bless(strada_new_hash(), "R10Guard");
}

static StradaValue* r10_tie4c_fetch(StradaValue *self, StradaValue *args) {
    (void)self;
    (void)args;
    g_rt4_compound_phase++;
    if (g_rt4_compound_phase == 1) {
        return STRADA_MAKE_TAGGED_INT(41);
    }
    if (g_rt4_compound_phase == 2) {
        StradaValue *inner = strada_tied_scalar_fetch(g_rt4_sv);
        strada_decref(inner);
        return strada_new_str("rt4_compound_val");
    }
    return strada_bless(strada_new_hash(), "R10Guard");
}

static int g_rt5_destroy_count = 0;

static StradaValue* r10_rt5_destroy(StradaValue *self, StradaValue *args) {
    (void)self;
    (void)args;
    g_rt5_destroy_count++;
    StradaValue *c = strada_new_hash();
    StradaValue *d = strada_new_hash();
    strada_hv_store(c, "other", d);
    strada_hv_store(d, "other", c);
    strada_decref(c);
    strada_decref(d);
    strada_throw("rt5_cyc_exc");
    return strada_new_undef();
}

static StradaValue* r10_rt6_worker1(void *ctx, int argc, StradaValue **argv) {
    (void)ctx; (void)argc; (void)argv;
    for (int i = 0; i < 300; i++) {
        StradaValue *lt = strada_localtime(STRADA_MAKE_TAGGED_INT(0));
        assert(lt && lt->type == STRADA_HASH);
        StradaValue *yr = strada_hv_fetch(lt, "year");
        assert(yr && strada_to_int(yr) == 70);
        strada_decref(yr);
        strada_decref(lt);

        StradaValue *ip = strada_inet_ntoa(STRADA_MAKE_TAGGED_INT((int64_t)htonl(0x0A0B0C0Du)));
        char *ips = strada_to_str(ip);
        assert(ips && strcmp(ips, "10.11.12.13") == 0);
        free(ips);
        strada_decref(ip);
    }
    return STRADA_MAKE_TAGGED_INT(1);
}

static StradaValue* r10_rt6_worker2(void *ctx, int argc, StradaValue **argv) {
    (void)ctx; (void)argc; (void)argv;
    for (int i = 0; i < 300; i++) {
        StradaValue *gt = strada_gmtime(STRADA_MAKE_TAGGED_INT(1000000000LL));
        assert(gt && gt->type == STRADA_HASH);
        StradaValue *yr = strada_hv_fetch(gt, "year");
        assert(yr && strada_to_int(yr) == 101);
        strada_decref(yr);
        strada_decref(gt);

        StradaValue *ip = strada_inet_ntoa(STRADA_MAKE_TAGGED_INT((int64_t)htonl(0xC0A80102u)));
        char *ips = strada_to_str(ip);
        assert(ips && strcmp(ips, "192.168.1.2") == 0);
        free(ips);
        strada_decref(ip);
    }
    return STRADA_MAKE_TAGGED_INT(2);
}

static volatile int g_rt7_term_seen = 0;

static StradaValue* r10_rt7_sigint(void *ctx, int argc, StradaValue **argv) {
    (void)ctx; (void)argc; (void)argv;
    strada_throw("rt7_sigint");
    return strada_new_undef();
}

static StradaValue* r10_rt7_sigterm(void *ctx, int argc, StradaValue **argv) {
    (void)ctx; (void)argc; (void)argv;
    g_rt7_term_seen = 1;
    return strada_new_undef();
}

static void test_r10_runtime(void) {
    (void)r10_tie4_new;
    printf("Testing R10 runtime fixes...\n");

    /* R10-RT1: DESTROY die($self) and resurrection */
    strada_oop_init();
    strada_method_register("R10Boom", "DESTROY", r10_boom_destroy);
    strada_method_register("R10Resurrect", "DESTROY", r10_res_destroy);

    g_rt1_boom_armed = 1;
    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        StradaValue *obj = strada_bless(strada_new_hash(), "R10Boom");
        StradaValue *tag = strada_new_str("boom_self");
        strada_hv_store(obj, "tag", tag);
        strada_decref(tag);
        strada_decref(obj);
        STRADA_TRY_POP();
        assert(0 && "R10-RT1 expected DESTROY exception");
    } else {
        STRADA_TRY_POP();
        StradaValue *exc = strada_get_exception();
        strada_clear_exception();
        assert(exc && !STRADA_IS_TAGGED_INT(exc));
        const char *bpkg = SV_BLESSED(exc);
        assert(bpkg != NULL && strcmp(bpkg, "R10Boom") == 0);
        StradaValue *tv = strada_hv_fetch(exc, "tag");
        char *ts = strada_to_str(tv);
        assert(ts && strcmp(ts, "boom_self") == 0);
        free(ts);
        strada_decref(tv);
        strada_decref(exc);
    }

    g_rt1_resurrect_arr = strada_new_array();
    g_rt1_res_armed = 1;
    StradaValue *robj = strada_bless(strada_new_hash(), "R10Resurrect");
    StradaValue *rtag = strada_new_str("survived");
    strada_hv_store(robj, "tag", rtag);
    strada_decref(rtag);
    strada_decref(robj);
    assert(g_rt1_resurrect_arr->value.av->size == 1);
    StradaValue *kept = strada_array_get(g_rt1_resurrect_arr->value.av, 0);
    StradaValue *ktv = strada_hv_fetch(kept, "tag");
    char *kts = strada_to_str(ktv);
    assert(kts && strcmp(kts, "survived") == 0);
    free(kts);
    strada_decref(ktv);
    strada_decref(g_rt1_resurrect_arr);
    g_rt1_resurrect_arr = NULL;

    /* R10-RT2: io_poller_main handles EPOLLHUP/POLLHUP on "w" and empty mask without spinning */
    int p[2];
    assert(pipe(p) == 0);
    close(p[1]);
    StradaValue *msv = strada_new_str("w");
    StradaValue *f = strada_io_wait_async(STRADA_MAKE_TAGGED_INT(p[0]), msv, STRADA_MAKE_TAGGED_INT(2000));
    strada_decref(msv);
    StradaValue *ev = strada_future_await(f);
    char *evs = strada_to_str(ev);
    assert(evs && evs[0] != '\0' && strcmp(evs, "timeout") != 0);
    free(evs);
    strada_decref(ev);
    strada_decref(f);
    close(p[0]);

    int p2[2];
    assert(pipe(p2) == 0);
    StradaValue *emsv = strada_new_str("");
    StradaValue *f2 = strada_io_wait_async(STRADA_MAKE_TAGGED_INT(p2[0]), emsv, STRADA_MAKE_TAGGED_INT(0));
    strada_decref(emsv);
    StradaValue *ev2 = strada_future_await(f2);
    strada_decref(ev2);
    strada_decref(f2);
    close(p2[0]);
    close(p2[1]);

    /* R10-RT3: strada_deref_set & strada_overwrite_in_place with child SV owned solely by target */
    StradaValue *inner = strada_new_str("hello world");
    StradaValue *mid = strada_new_ref_take(inner, '$');
    StradaValue *outer = strada_new_ref_take(mid, '$');
    StradaValue *dres = strada_deref_set(outer, strada_ref_deref(strada_ref_deref(outer)));
    assert(dres != NULL);
    StradaValue *chk3 = strada_ref_deref(outer);
    char *s3 = strada_to_str(chk3);
    assert(s3 && strcmp(s3, "hello world") == 0);
    free(s3);
    strada_decref(outer);

    StradaValue *c1 = strada_new_str("in_place_payload");
    StradaValue *dst = strada_new_ref_take(c1, '$');
    strada_overwrite_in_place(dst, c1);
    assert(dst->type == STRADA_STR && strcmp(dst->value.pv, "in_place_payload") == 0);
    strada_decref(dst);

    /* R10-RT4: tied_scalar_assign & tied_scalar_compound when old_cache2 DESTROY unties */
    strada_method_register("R10Guard", "DESTROY", r10_guard_destroy);
    strada_method_register("R10Tie4", "TIESCALAR", r10_tie4_tiescalar);
    strada_method_register("R10Tie4", "FETCH", r10_tie4_fetch);
    strada_method_register("R10Tie4", "STORE", r10_tie4_store);
    strada_method_register("R10Tie4C", "TIESCALAR", r10_tie4_tiescalar);
    strada_method_register("R10Tie4C", "FETCH", r10_tie4c_fetch);
    strada_method_register("R10Tie4C", "STORE", r10_tie4_store);

    StradaValue *ts = strada_new_undef();
    StradaValue *ts_ref = strada_new_ref(ts, '$');
    g_rt4_sv = ts;
    g_rt4_reenter = 0;
    strada_decref(strada_tie_scalar(ts_ref, "R10Tie4", 0));
    StradaValue *rhs4 = strada_new_str("x");
    StradaValue *ares = strada_tied_scalar_assign(ts, rhs4);
    assert(ares && ares->type == STRADA_STR && strcmp(ares->value.pv, "rt4_fresh_val") == 0);
    strada_decref(rhs4);
    strada_decref(ts_ref);
    strada_decref(ts);
    g_rt4_sv = NULL;

    StradaValue *tsc = strada_new_undef();
    StradaValue *tsc_ref = strada_new_ref(tsc, '$');
    g_rt4_sv = tsc;
    g_rt4_compound_phase = 0;
    strada_decref(strada_tie_scalar(tsc_ref, "R10Tie4C", 0));
    StradaValue *cres = strada_tied_scalar_compound(tsc, STRADA_MAKE_TAGGED_INT(1), '+', 1);
    assert(cres && cres->type == STRADA_STR && strcmp(cres->value.pv, "rt4_compound_val") == 0);
    strada_decref(tsc_ref);
    strada_decref(tsc);
    g_rt4_sv = NULL;

    /* R10-RT5: cc_destroy_depth does not leak on throwing recursive cc_collect cycles */
    strada_method_register("R10CycA", "DESTROY", r10_rt5_destroy);
    g_rt5_destroy_count = 0;
    for (int iter = 0; iter < 6; iter++) {
        if (setjmp(*STRADA_TRY_PUSH()) == 0) {
            StradaValue *a = strada_bless(strada_new_hash(), "R10CycA");
            StradaValue *b = strada_new_hash();
            strada_hv_store(a, "b", b);
            strada_hv_store(b, "a", a);
            strada_decref(a);
            strada_decref(b);
            strada_gc_collect();
            STRADA_TRY_POP();
        } else {
            STRADA_TRY_POP();
            strada_clear_exception();
        }
    }
#ifdef STRADA_CYCLE_GC
    assert(g_rt5_destroy_count == 6);
#endif

    /* R10-RT6: concurrent thread-safe localtime/gmtime/inet_ntoa */
    StradaValue *cl1 = strada_custom_closure_new(r10_rt6_worker1, NULL, NULL);
    StradaValue *cl2 = strada_custom_closure_new(r10_rt6_worker2, NULL, NULL);
    StradaValue *t1 = strada_thread_create(cl1);
    StradaValue *t2 = strada_thread_create(cl2);
    StradaValue *j1 = strada_thread_join(t1);
    StradaValue *j2 = strada_thread_join(t2);
    assert(strada_to_int(j1) == 1 && strada_to_int(j2) == 2);
    strada_decref(j1);
    strada_decref(j2);
    strada_decref(t1);
    strada_decref(t2);
    strada_decref(cl1);
    strada_decref(cl2);

    /* R10-RT7: pending signal not dropped if earlier handler throws */
    g_rt7_term_seen = 0;
    {
        StradaValue *sig_int_name = strada_new_str("INT");
        StradaValue *sig_term_name = strada_new_str("TERM");
        StradaValue *int_cl = strada_custom_closure_new(r10_rt7_sigint, NULL, NULL);
        StradaValue *term_cl = strada_custom_closure_new(r10_rt7_sigterm, NULL, NULL);
        strada_decref(strada_signal(sig_int_name, int_cl));
        strada_decref(strada_signal(sig_term_name, term_cl));
        strada_decref(sig_int_name);
        strada_decref(sig_term_name);
        strada_decref(int_cl);
        strada_decref(term_cl);
    }
    raise(SIGINT);
    raise(SIGTERM);
    if (setjmp(*STRADA_TRY_PUSH()) == 0) {
        strada_dispatch_signals();
        STRADA_TRY_POP();
        assert(0 && "R10-RT7 expected SIGINT throw");
    } else {
        STRADA_TRY_POP();
        strada_clear_exception();
    }
    strada_dispatch_signals();
    assert(g_rt7_term_seen == 1);
    {
        StradaValue *sig_int_name = strada_new_str("INT");
        StradaValue *sig_term_name = strada_new_str("TERM");
        StradaValue *dfl_str = strada_new_str("DEFAULT");
        strada_decref(strada_signal(sig_int_name, dfl_str));
        strada_decref(strada_signal(sig_term_name, dfl_str));
        strada_decref(dfl_str);
        strada_decref(sig_int_name);
        strada_decref(sig_term_name);
    }

    printf("R10 runtime tests OK\n");
}

static StradaValue *g_r11_rt1_slot = NULL;
static StradaValue* r11_drop_slot_destroy(StradaValue *self, StradaValue *args) {
    (void)self; (void)args;
    if (g_r11_rt1_slot) {
        StradaValue *old = g_r11_rt1_slot;
        g_r11_rt1_slot = strada_new_str("replaced");
        strada_decref(old);
    }
    return strada_new_undef();
}

#ifdef STRADA_CYCLE_GC
static pthread_mutex_t g_r11_rt2_mtx = PTHREAD_MUTEX_INITIALIZER;
static StradaValue *g_r11_rt2_queue[64];
static int g_r11_rt2_qlen = 0;
static volatile int g_r11_rt2_done = 0;

static StradaValue* r11_rt2_destroy(StradaValue *self, StradaValue *args) {
    (void)args;
    StradaValue *peer = strada_hv_fetch_owned(self, "peer");
    if (peer && !STRADA_IS_TAGGED_INT(peer)) {
        pthread_mutex_lock(&g_r11_rt2_mtx);
        if (g_r11_rt2_qlen < 64) {
            g_r11_rt2_queue[g_r11_rt2_qlen++] = peer;
            peer = NULL;
        }
        pthread_mutex_unlock(&g_r11_rt2_mtx);
        if (peer) strada_decref(peer);
    } else if (peer) {
        strada_decref(peer);
    }
    return strada_new_undef();
}

static StradaValue* r11_rt2_worker(void *ctx, int argc, StradaValue **argv) {
    (void)ctx; (void)argc; (void)argv;
    while (!g_r11_rt2_done) {
        StradaValue *batch[64];
        int n = 0;
        pthread_mutex_lock(&g_r11_rt2_mtx);
        while (g_r11_rt2_qlen > 0) {
            batch[n++] = g_r11_rt2_queue[--g_r11_rt2_qlen];
        }
        pthread_mutex_unlock(&g_r11_rt2_mtx);
        for (int i = 0; i < n; i++) {
            strada_decref(batch[i]);
        }
        if (n > 0) strada_gc_collect();
    }
    return STRADA_MAKE_TAGGED_INT(1);
}
#endif

static StradaValue *g_r11_rt3_rhs = NULL;
static StradaValue *g_r11_rt3_sv = NULL;

static StradaValue* r11_tie3_tiescalar(StradaValue *cls, StradaValue *args) {
    (void)args;
    const char *pkg = (cls && !STRADA_IS_TAGGED_INT(cls) && cls->type == STRADA_STR && cls->value.pv) ? cls->value.pv : "R11Tie3";
    StradaValue *h = strada_new_hash();
    StradaValue *v = strada_new_str("hi_");
    strada_hv_store(h, "val", v);
    strada_decref(v);
    return strada_bless(h, pkg);
}

static StradaValue* r11_tie3_fetch(StradaValue *self, StradaValue *args) {
    (void)args;
    if (g_r11_rt3_rhs) {
        StradaValue *old = g_r11_rt3_rhs;
        g_r11_rt3_rhs = NULL;
        strada_decref(old);
    }
    return strada_hv_fetch_owned(self, "val");
}

static StradaValue* r11_tie3_store(StradaValue *self, StradaValue *args) {
    if (args && args->type == STRADA_ARRAY && args->value.av->size > 0) {
        strada_hv_store(self, "val", strada_array_get(args->value.av, 0));
    } else {
        StradaValue *u = strada_new_undef();
        strada_hv_store(self, "val", u);
        strada_decref(u);
    }
    if (g_r11_rt3_sv) {
        StradaValue *old = g_r11_rt3_sv;
        g_r11_rt3_sv = NULL;
        strada_decref(old);
    }
    return strada_new_undef();
}

static StradaValue *g_r11_rt7_dst = NULL;
static StradaValue* r11_dst_guard_destroy(StradaValue *self, StradaValue *args) {
    (void)self; (void)args;
    if (g_r11_rt7_dst) {
        StradaValue *old = g_r11_rt7_dst;
        g_r11_rt7_dst = NULL;
        strada_decref(old);
    }
    return strada_new_undef();
}

static void test_r11_runtime(void) {
    printf("Testing R11 runtime fixes...\n");

    /* R11-RT1: deref_assign_container flat-array refcount balance & slot target pinning */
    strada_oop_init();
    strada_method_register("R11DropSlot", "DESTROY", r11_drop_slot_destroy);

    StradaValue *flat = strada_new_array();
    StradaValue *fk1 = strada_new_str("k1");
    StradaValue *fk2 = strada_new_str("k2");
    strada_array_push(flat->value.av, fk1);
    strada_array_push(flat->value.av, STRADA_MAKE_TAGGED_INT(11));
    strada_array_push(flat->value.av, fk2);
    strada_array_push(flat->value.av, STRADA_MAKE_TAGGED_INT(22));
    strada_decref(fk1);
    strada_decref(fk2);
    StradaValue *htarg = strada_new_hash();
    StradaValue *href = strada_new_ref(htarg, '%');
    StradaValue *dres1 = strada_deref_assign_container(href, flat, '%');
    assert(dres1 == htarg);
    assert(flat->refcount == 1 && flat->value.av->size == 4);
    StradaValue *hv1 = strada_hv_fetch(htarg, "k1");
    StradaValue *hv2 = strada_hv_fetch(htarg, "k2");
    assert(strada_to_int(hv1) == 11 && strada_to_int(hv2) == 22);
    strada_decref(hv1);
    strada_decref(hv2);
    strada_decref(href);
    strada_decref(htarg);
    strada_decref(flat);

    g_r11_rt1_slot = strada_new_hash();
    StradaValue *drop_obj = strada_bless(strada_new_hash(), "R11DropSlot");
    strada_hv_store(g_r11_rt1_slot, "drop", drop_obj);
    strada_decref(drop_obj);
    StradaValue *sref = strada_slot_ref_create(&g_r11_rt1_slot);
    StradaValue *rhs_h = strada_new_hash();
    strada_hv_store(rhs_h, "ok", STRADA_MAKE_TAGGED_INT(99));
    StradaValue *dres2 = strada_deref_assign_container(sref, rhs_h, '%');
    assert(dres2 && !STRADA_IS_TAGGED_INT(dres2) && dres2->type == STRADA_HASH);
    StradaValue *dok = strada_hv_fetch(dres2, "ok");
    assert(strada_to_int(dok) == 99);
    strada_decref(dok);
    strada_decref(rhs_h);
    strada_decref(sref);
    strada_decref(g_r11_rt1_slot);
    g_r11_rt1_slot = NULL;

#ifdef STRADA_CYCLE_GC
    /* R11-RT2: cross-thread cc_reclaim_watches synchronization */
    strada_method_register("R11CycA", "DESTROY", r11_rt2_destroy);
    g_r11_rt2_done = 0;
    g_r11_rt2_qlen = 0;
    StradaValue *wcl = strada_custom_closure_new(r11_rt2_worker, NULL, NULL);
    StradaValue *wt = strada_thread_create(wcl);
    for (int iter = 0; iter < 80; iter++) {
        StradaValue *a = strada_bless(strada_new_hash(), "R11CycA");
        StradaValue *b = strada_new_hash();
        strada_hv_store(a, "peer", b);
        strada_hv_store(b, "a", a);
        strada_decref(a);
        strada_decref(b);
        strada_gc_collect();
    }
    g_r11_rt2_done = 1;
    StradaValue *wj = strada_thread_join(wt);
    assert(strada_to_int(wj) == 1);
    strada_decref(wj);
    strada_decref(wt);
    strada_decref(wcl);
    while (g_r11_rt2_qlen > 0) {
        strada_decref(g_r11_rt2_queue[--g_r11_rt2_qlen]);
    }
    strada_gc_collect();
#endif

    /* R11-RT3: tied_scalar_compound & tied_scalar_assign pin sv and rhs */
    strada_method_register("R11Tie3", "TIESCALAR", r11_tie3_tiescalar);
    strada_method_register("R11Tie3", "FETCH", r11_tie3_fetch);
    strada_method_register("R11Tie3", "STORE", r11_tie3_store);

    g_r11_rt3_sv = strada_new_undef();
    StradaValue *t3ref = strada_new_ref(g_r11_rt3_sv, '$');
    strada_decref(strada_tie_scalar(t3ref, "R11Tie3", 0));
    strada_decref(t3ref);
    g_r11_rt3_rhs = strada_new_str("world_payload_0123456789");
    StradaValue *t3res = strada_tied_scalar_compound(g_r11_rt3_sv, g_r11_rt3_rhs, '.', 1);
    assert(g_r11_rt3_rhs == NULL && g_r11_rt3_sv == NULL);
    assert(t3res && t3res->type == STRADA_STR && strcmp(t3res->value.pv, "hi_world_payload_0123456789") == 0);

    g_r11_rt3_sv = strada_new_undef();
    StradaValue *t3ref2 = strada_new_ref(g_r11_rt3_sv, '$');
    strada_decref(strada_tie_scalar(t3ref2, "R11Tie3", 0));
    strada_decref(t3ref2);
    StradaValue *newv3 = strada_new_str("assigned_via_rt3");
    StradaValue *t3ares = strada_tied_scalar_assign(g_r11_rt3_sv, newv3);
    assert(g_r11_rt3_sv == NULL);
    assert(t3ares && t3ares->type == STRADA_STR && strcmp(t3ares->value.pv, "assigned_via_rt3") == 0);
    strada_decref(newv3);

    /* R11-RT4: strada_regex_replace_capture_len sets strada_regex_subject_utf8 before compile */
    char *rx_out = NULL;
    size_t rx_olen = 0;
    int rx_rc = strada_regex_replace_capture_len("ab", 2, 0, "a", "b", "", 0, &rx_out, &rx_olen);
    assert(rx_rc == 1);
    free(rx_out);

#ifdef HAVE_PCRE2
    rx_out = NULL; rx_olen = 0;
    rx_rc = strada_regex_replace_capture_len("\xC3\xA9", 2, 1, "\\p{Ll}", "X", "", 0, &rx_out, &rx_olen);
    assert(rx_rc == 1 && rx_olen == 1 && rx_out[0] == 'X');
    free(rx_out);

    rx_out = NULL; rx_olen = 0;
    rx_rc = strada_regex_replace_capture_len("\xff\xfe", 2, 0, "\\xff", "Z", "", 0, &rx_out, &rx_olen);
    assert(rx_rc == 1 && rx_olen == 2 && rx_out[0] == 'Z' && (unsigned char)rx_out[1] == 0xfe);
    free(rx_out);
#endif

    /* R11-RT5: %vld format and snprintf < 0 deterministic temp[0] = '\0' */
    StradaValue *fmt_v = strada_new_str("%vld");
    StradaValue *arg_ab = strada_new_str("AB");
    StradaValue *sp_v = strada_sprintf_sv(fmt_v, 1, arg_ab);
    assert(sp_v && sp_v->type == STRADA_STR && strcmp(sp_v->value.pv, "65.66") == 0);
    strada_decref(sp_v);
    strada_decref(arg_ab);
    strada_decref(fmt_v);

    StradaValue *fmt_eof1 = strada_new_str("%#.*f");
    StradaValue *val_one = strada_new_num(1.0);
    StradaValue *sp_eof1 = strada_sprintf_sv(fmt_eof1, 2, STRADA_MAKE_TAGGED_INT(2147483647LL), val_one);
    assert(sp_eof1 && sp_eof1->type == STRADA_STR && strcmp(sp_eof1->value.pv, "") == 0);
    strada_decref(sp_eof1);
    strada_decref(fmt_eof1);

    StradaValue *fmt_eof2 = strada_new_str("%#.*e");
    StradaValue *sp_eof2 = strada_sprintf_sv(fmt_eof2, 2, STRADA_MAKE_TAGGED_INT(2147483647LL), val_one);
    assert(sp_eof2 && sp_eof2->type == STRADA_STR && strcmp(sp_eof2->value.pv, "") == 0);
    strada_decref(sp_eof2);
    strada_decref(fmt_eof2);
    strada_decref(val_one);

    /* R11-RT6: strada_unpack '/' sign-extension for s>/s</l>/l< and size_t for N/Q */
    StradaValue *up_sbe = strada_new_str_len("\xff\xfe" "ABCDE", 7);
    StradaValue *ua1 = strada_unpack("s>/a*", up_sbe);
    assert(ua1 && ua1->type == STRADA_ARRAY && ua1->value.av->size == 1);
    StradaValue *u1e = strada_array_get(ua1->value.av, 0);
    assert(u1e && u1e->type == STRADA_STR && u1e->value.pv[0] == '\0');
    strada_decref(ua1); strada_decref(up_sbe);

    StradaValue *up_sle = strada_new_str_len("\xfe\xff" "ABCDE", 7);
    StradaValue *ua2 = strada_unpack("s</a*", up_sle);
    assert(ua2 && ua2->type == STRADA_ARRAY && ua2->value.av->size == 1);
    StradaValue *u2e = strada_array_get(ua2->value.av, 0);
    assert(u2e && u2e->type == STRADA_STR && u2e->value.pv[0] == '\0');
    strada_decref(ua2); strada_decref(up_sle);

    StradaValue *up_lbe = strada_new_str_len("\xff\xff\xff\xfe" "ABCDE", 9);
    StradaValue *ua3 = strada_unpack("l>/a*", up_lbe);
    assert(ua3 && ua3->type == STRADA_ARRAY && ua3->value.av->size == 1);
    StradaValue *u3e = strada_array_get_owned(ua3->value.av, 0);
    assert(u3e && u3e->type == STRADA_STR && u3e->value.pv[0] == '\0');
    strada_decref(u3e); strada_decref(ua3); strada_decref(up_lbe);

    StradaValue *up_n = strada_new_str_len("\x80\x00\x00\x01" "abc", 7);
    StradaValue *ua4 = strada_unpack("N/a*", up_n);
    assert(ua4 && ua4->type == STRADA_ARRAY && ua4->value.av->size == 1);
    StradaValue *u4e = strada_array_get_owned(ua4->value.av, 0);
    assert(u4e && u4e->type == STRADA_STR && strcmp(u4e->value.pv, "abc") == 0);
    strada_decref(u4e); strada_decref(ua4); strada_decref(up_n);

    char qbuf[13];
    uint64_t qlen = 0x0000000100000001ULL;
    memcpy(qbuf, &qlen, 8);
    memcpy(qbuf + 8, "hello", 5);
    StradaValue *up_q = strada_new_str_len(qbuf, 13);
    StradaValue *ua5 = strada_unpack("Q/a*", up_q);
    assert(ua5 && ua5->type == STRADA_ARRAY && ua5->value.av->size == 1);
    StradaValue *u5e = strada_array_get_owned(ua5->value.av, 0);
    assert(u5e && u5e->type == STRADA_STR && strcmp(u5e->value.pv, "hello") == 0);
    strada_decref(u5e); strada_decref(ua5); strada_decref(up_q);

    /* R11-RT7: strada_overwrite_in_place pins dst across strada_release_payload */
    strada_method_register("R11DstGuard", "DESTROY", r11_dst_guard_destroy);
    g_r11_rt7_dst = strada_new_hash();
    StradaValue *dguard = strada_bless(strada_new_hash(), "R11DstGuard");
    strada_hv_store(g_r11_rt7_dst, "guard", dguard);
    strada_decref(dguard);
    StradaValue *ov_src = strada_new_str("overwritten_ok");
    strada_overwrite_in_place(g_r11_rt7_dst, ov_src);
    strada_decref(ov_src);
    assert(g_r11_rt7_dst == NULL);

    printf("R11 runtime tests OK\n");
}

/* --- R12 runtime test helpers --- */
static StradaValue *g_r12_coro_handle = NULL;
static int g_r12_rt2_resume_threw = 0;
static int g_r12_rt2_free_threw = 0;
static char g_r12_coro_caller_mid[64] = "";

static StradaValue* r12_coro_body(void *ctx, int argc, StradaValue **argv) {
    (void)ctx; (void)argc; (void)argv;
    strada_stack_push_il("r12_coro_fn", "r12.c");
    strada_stack_line_il(77);

    /* R12-RT2: self-resume and self-free while state == 1 must throw */
    {
        jmp_buf *jb = STRADA_TRY_PUSH();
        if (jb && setjmp(*jb) == 0) {
            strada_decref(strada_coro_resume(g_r12_coro_handle));
            STRADA_TRY_POP();
        } else {
            g_r12_rt2_resume_threw = 1;
            strada_clear_exception();
        }
    }
    {
        jmp_buf *jb = STRADA_TRY_PUSH();
        if (jb && setjmp(*jb) == 0) {
            strada_decref(strada_coro_free(g_r12_coro_handle));
            STRADA_TRY_POP();
        } else {
            g_r12_rt2_free_threw = 1;
            strada_clear_exception();
        }
    }

    StradaValue *y = strada_coro_yield_io(STRADA_MAKE_TAGGED_INT(-1),
                                          STRADA_MAKE_TAGGED_INT(0),
                                          STRADA_MAKE_TAGGED_INT(0));
    if (y) strada_decref(y);

    StradaValue *ci = strada_caller_info(STRADA_MAKE_TAGGED_INT(-1));
    if (ci && ci->type == STRADA_HASH) {
        StradaValue *fn = strada_hv_fetch_owned(ci, "function");
        if (fn && fn->type == STRADA_STR && fn->value.pv) {
            snprintf(g_r12_coro_caller_mid, sizeof(g_r12_coro_caller_mid), "%s", fn->value.pv);
        }
        if (fn) strada_decref(fn);
    }
    if (ci) strada_decref(ci);

    strada_stack_pop_il();
    return STRADA_MAKE_TAGGED_INT(123);
}

#ifdef STRADA_CYCLE_GC
static int g_r12_tied_destroyed = 0;
static StradaValue* r12_tiescalar(StradaValue *self, StradaValue *args) {
    (void)self; (void)args;
    return strada_bless(strada_new_hash(), "R12TiedObj");
}
static StradaValue* r12_tied_destroy(StradaValue *self, StradaValue *args) {
    (void)self; (void)args;
    g_r12_tied_destroyed++;
    return strada_new_undef();
}

static StradaValue* r12_reenter_destroy(StradaValue *self, StradaValue *args) {
    (void)args;
    StradaValue *extra = strada_hv_fetch_owned(self, "extra");
    if (extra) strada_decref(extra);
    /* Nested cycle GC while unwinding free queue must be a safe no-op */
    strada_gc_collect();
    return strada_new_undef();
}
#endif

static StradaValue* r12_long_pkg_ping(StradaValue *self, StradaValue *args) {
    (void)self; (void)args;
    return STRADA_MAKE_TAGGED_INT(777);
}

static void test_r12_runtime(void) {
    printf("Testing R12 runtime fixes...\n");

    /* R12-RT1 & R12-RT2: coroutine call_stack off-by-one + re-entrancy/free guards */
    strada_stack_push_il("r12_host_fn", "r12.c");
    strada_stack_line_il(42);
    StradaValue *ccl = strada_custom_closure_new(r12_coro_body, NULL, NULL);
    g_r12_coro_handle = strada_coro_create(ccl);
    g_r12_rt2_resume_threw = 0;
    g_r12_rt2_free_threw = 0;
    g_r12_coro_caller_mid[0] = '\0';

    StradaValue *st1 = strada_coro_resume(g_r12_coro_handle);
    assert(strada_to_int(st1) == 2); /* suspended */
    strada_decref(st1);
    assert(g_r12_rt2_resume_threw == 1 && g_r12_rt2_free_threw == 1);

    /* Host top call frame must be intact across yield */
    StradaValue *hci1 = strada_caller_info(STRADA_MAKE_TAGGED_INT(-1));
    StradaValue *hfn1 = strada_hv_fetch_owned(hci1, "function");
    assert(hfn1 && hfn1->type == STRADA_STR && strcmp(hfn1->value.pv, "r12_host_fn") == 0);
    strada_decref(hfn1);
    strada_decref(hci1);

    StradaValue *st2 = strada_coro_resume(g_r12_coro_handle);
    assert(strada_to_int(st2) == 3); /* done */
    strada_decref(st2);
    assert(strcmp(g_r12_coro_caller_mid, "r12_coro_fn") == 0);

    StradaValue *hci2 = strada_caller_info(STRADA_MAKE_TAGGED_INT(-1));
    StradaValue *hfn2 = strada_hv_fetch_owned(hci2, "function");
    assert(hfn2 && hfn2->type == STRADA_STR && strcmp(hfn2->value.pv, "r12_host_fn") == 0);
    strada_decref(hfn2);
    strada_decref(hci2);

    StradaValue *cres = strada_coro_result(g_r12_coro_handle);
    assert(strada_to_int(cres) == 123);
    strada_decref(cres);
    strada_decref(strada_coro_free(g_r12_coro_handle));
    strada_decref(g_r12_coro_handle);
    g_r12_coro_handle = NULL;
    strada_decref(ccl);
    strada_stack_pop_il();

#ifdef STRADA_CYCLE_GC
    /* R12-RT3: dead untied cycle owning a tied scalar */
    g_r12_tied_destroyed = 0;
    strada_method_register("R12TiedObj", "TIESCALAR", r12_tiescalar);
    strada_method_register("R12TiedObj", "DESTROY", r12_tied_destroy);
    {
        StradaValue *ca = strada_new_hash();
        StradaValue *cb = strada_new_hash();
        StradaValue *tv = strada_new_undef();
        StradaValue *tref = strada_new_ref(tv, '$');
        strada_decref(strada_tie_scalar(tref, "R12TiedObj", 0));
        strada_decref(tref);
        strada_hv_store(ca, "b", cb);
        strada_hv_store(ca, "t", tv);
        strada_hv_store(cb, "a", ca);
        strada_decref(tv);
        strada_decref(cb);
        strada_decref(ca);
        strada_gc_collect();
        assert(g_r12_tied_destroyed == 1);
    }

    /* R12-RT4: free queue reentry & nested strada_gc_collect */
    strada_method_register("R12Reenter", "DESTROY", r12_reenter_destroy);
    {
        StradaValue *ro = strada_bless(strada_new_hash(), "R12Reenter");
        StradaValue *ex = strada_new_array();
        strada_array_push(ex->value.av, strada_new_str("payload"));
        strada_hv_store(ro, "extra", ex);
        strada_decref(ex);
        strada_decref(ro);
        strada_gc_collect();
    }
#endif

    /* R12-RT5: sprintf %v >1023 bytes (wide element & long string) and %*v non-STRADA_STR sep */
    {
        char big[1500];
        memset(big, 'A', sizeof(big));
        StradaValue *big_sv = strada_new_str_len(big, sizeof(big));
        StradaValue *fmt_vd = strada_new_str("%vd");
        StradaValue *res_vd = strada_sprintf_sv(fmt_vd, 1, big_sv);
        assert(res_vd && res_vd->type == STRADA_STR);
        assert(STRADA_STR_BYTELEN(res_vd) == 1500 * 2 + 1499);
        assert(memcmp(res_vd->value.pv, "65.65.65.", 9) == 0);
        assert(memcmp(res_vd->value.pv + STRADA_STR_BYTELEN(res_vd) - 3, ".65", 3) == 0);
        strada_decref(res_vd);
        strada_decref(fmt_vd);
        strada_decref(big_sv);

        StradaValue *fmt_w = strada_new_str("%v1500d");
        StradaValue *a1_sv = strada_new_str("A");
        StradaValue *res_w = strada_sprintf_sv(fmt_w, 1, a1_sv);
        assert(res_w && res_w->type == STRADA_STR && STRADA_STR_BYTELEN(res_w) == 1500);
        assert(res_w->value.pv[0] == ' ' && memcmp(res_w->value.pv + 1498, "65", 2) == 0);
        strada_decref(res_w);
        strada_decref(a1_sv);
        strada_decref(fmt_w);

        StradaValue *fmt_sv = strada_new_str("%*vd");
        StradaValue *ab_sv = strada_new_str("AB");
        StradaValue *res_sv = strada_sprintf_sv(fmt_sv, 2, STRADA_MAKE_TAGGED_INT(99), ab_sv);
        assert(res_sv && res_sv->type == STRADA_STR && strcmp(res_sv->value.pv, "659966") == 0);
        strada_decref(res_sv);
        strada_decref(ab_sv);
        strada_decref(fmt_sv);
    }

    /* R12-RT6: embedded NUL & SVf_UTF8 preservation in sprintf %s / join / strada_to_str_ss */
    {
        StradaValue *bin = strada_new_str_len("a\0b", 3);
        StradaValue *fmt6 = strada_new_str("%6s");
        StradaValue *sp_bin = strada_sprintf_sv(fmt6, 1, bin);
        assert(STRADA_STR_BYTELEN(sp_bin) == 6);
        assert(memcmp(sp_bin->value.pv, "   a\0b", 6) == 0);
        assert(!STRADA_STR_IS_UTF8(sp_bin));
        strada_decref(sp_bin);
        strada_decref(fmt6);

        StradaValue *u8 = strada_new_str_len_utf8("\xc3\xa9\0z", 4);
        StradaValue *fmt_l5 = strada_new_str("%-5s");
        StradaValue *sp_u8 = strada_sprintf_sv(fmt_l5, 1, u8);
        assert(STRADA_STR_IS_UTF8(sp_u8));
        assert(STRADA_STR_BYTELEN(sp_u8) == 6);
        assert(memcmp(sp_u8->value.pv, "\xc3\xa9\0z  ", 6) == 0);
        strada_decref(sp_u8);
        strada_decref(fmt_l5);

        StradaValue *jarr = strada_new_array();
        strada_array_push(jarr->value.av, bin);
        strada_array_push(jarr->value.av, u8);
        StradaValue *sep = strada_new_str_len("\0", 1);
        StradaValue *jres = strada_join_sv(sep, jarr->value.av);
        assert(STRADA_STR_BYTELEN(jres) == 8);
        assert(memcmp(jres->value.pv, "a\0b\0\xc3\xa9\0z", 8) == 0);
        assert(STRADA_STR_IS_UTF8(jres));
        strada_decref(jres);
        strada_decref(sep);
        strada_decref(jarr);

        char *ss = strada_to_str_ss(bin);
        assert(ss && SS_FROM_PV(ss)->len == 3 && memcmp(ss, "a\0b", 3) == 0);
        strada_cstr_free(ss);
        strada_decref(u8);
        strada_decref(bin);
    }

    /* R12-RT7: pack/unpack (pat)0, (pat)*, nested groups, '/' with q/i */
    {
        StradaValue *d_ab = strada_new_str("AB");
        StradaValue *u0 = strada_unpack("(C)0", d_ab);
        assert(u0 && u0->type == STRADA_ARRAY && u0->value.av->size == 0);
        strada_decref(u0);

        StradaValue *d_empty = strada_new_str("");
        StradaValue *ustar = strada_unpack("(C)*", d_empty);
        assert(ustar && ustar->type == STRADA_ARRAY && ustar->value.av->size == 0);
        strada_decref(ustar);
        strada_decref(d_empty);

        StradaValue *nested = strada_new_str_len("A\x01\x02" "B\x03\x04", 6);
        StradaValue *un = strada_unpack("(a(C)2)2", nested);
        assert(un && un->type == STRADA_ARRAY && un->value.av->size == 6);
        StradaValue *un0 = strada_array_get_owned(un->value.av, 0);
        StradaValue *un1 = strada_array_get_owned(un->value.av, 1);
        StradaValue *un2 = strada_array_get_owned(un->value.av, 2);
        StradaValue *un3 = strada_array_get_owned(un->value.av, 3);
        StradaValue *un4 = strada_array_get_owned(un->value.av, 4);
        StradaValue *un5 = strada_array_get_owned(un->value.av, 5);
        assert(strcmp(un0->value.pv, "A") == 0 && strada_to_int(un1) == 1 && strada_to_int(un2) == 2);
        assert(strcmp(un3->value.pv, "B") == 0 && strada_to_int(un4) == 3 && strada_to_int(un5) == 4);
        strada_decref(un0); strada_decref(un1); strada_decref(un2);
        strada_decref(un3); strada_decref(un4); strada_decref(un5);
        strada_decref(un);
        strada_decref(nested);
        strada_decref(d_ab);

        StradaValue *pargs = strada_new_array();
        StradaValue *hi = strada_new_str("hello");
        strada_array_push(pargs->value.av, hi);
        strada_decref(hi);
        StradaValue *pq = strada_pack("q/a*", pargs);
        assert(pq && STRADA_STR_BYTELEN(pq) == 8 + 5);
        StradaValue *uq = strada_unpack("q/a*", pq);
        assert(uq && uq->value.av->size == 1);
        StradaValue *uq0 = strada_array_get_owned(uq->value.av, 0);
        assert(strcmp(uq0->value.pv, "hello") == 0);
        strada_decref(uq0); strada_decref(uq); strada_decref(pq);

        StradaValue *pi = strada_pack("i/a*", pargs);
        assert(pi && STRADA_STR_BYTELEN(pi) == sizeof(int) + 5);
        StradaValue *ui = strada_unpack("i/a*", pi);
        assert(ui && ui->value.av->size == 1);
        StradaValue *ui0 = strada_array_get_owned(ui->value.av, 0);
        assert(strcmp(ui0->value.pv, "hello") == 0);
        strada_decref(ui0); strada_decref(ui); strada_decref(pi);
        strada_decref(pargs);
    }

    /* R12-RT8: sticky package interning >64 bytes */
    {
        const char *long_pkg = "R12LongPkg::"
            "AlphaBetaGammaDeltaEpsilonZetaEtaThetaIotaKappaLambdaMuNuXiOmicronPiRhoSigmaTauUpsilonPhiChiPsiOmega";
        char *ip1 = strada_intern_pkg_name(long_pkg);
        char *ip2 = strada_intern_pkg_name(long_pkg);
        assert(ip1 == ip2 && strlen(ip1) > 64);
        strada_method_register(long_pkg, "ping", r12_long_pkg_ping);
        StradaValue *obj = strada_bless(strada_new_hash(), long_pkg);
        StradaValue *margs = strada_new_array();
        StradaValue *mres = strada_method_call(obj, "ping", margs);
        assert(strada_to_int(mres) == 777);
        strada_decref(mres);
        strada_decref(obj);
    }

    printf("R12 runtime tests OK\n");
}

/* --- R13 runtime test helpers --- */
static StradaValue* r13_long_pkg1_id(StradaValue *self, StradaValue *args) {
    (void)self; (void)args;
    return STRADA_MAKE_TAGGED_INT(101);
}
static StradaValue* r13_long_pkg2_id(StradaValue *self, StradaValue *args) {
    (void)self; (void)args;
    return STRADA_MAKE_TAGGED_INT(202);
}
static StradaValue* r13_long_base_only(StradaValue *self, StradaValue *args) {
    (void)self; (void)args;
    return STRADA_MAKE_TAGGED_INT(303);
}
static StradaValue* r13_bin_stringify(StradaValue *self, StradaValue *args) {
    (void)args;
    StradaValue *k = strada_hv_fetch_owned(self, "val");
    if (!k) return strada_new_str("");
    return k;
}
static StradaValue* r13_tied_tiescalar(StradaValue *cls, StradaValue *args) {
    StradaValue *h = strada_new_hash();
    if (args && args->type == STRADA_ARRAY && args->value.av->size > 0) {
        StradaValue *v = strada_array_get(args->value.av, 0);
        strada_hv_store(h, "v", v);
    }
    char *cname = strada_to_str(cls);
    StradaValue *obj = strada_bless(h, cname);
    free(cname);
    return obj;
}
static StradaValue* r13_tied_fetch(StradaValue *self, StradaValue *args) {
    (void)args;
    return strada_hv_fetch_owned(self, "v");
}

static void test_r13_runtime(void) {
    printf("Testing R13 runtime fixes...\n");

    /* R13-RT1: >=256-byte package names in OopPackage, inheritance, strada_set_package/method_package */
    {
        char long_base[300];
        char long_sub1[310];
        char long_sub2[310];
        memset(long_base, 'B', 270);
        long_base[270] = '\0';
        memcpy(long_sub1, long_base, 270);
        strcpy(long_sub1 + 270, "::SubOne");
        memcpy(long_sub2, long_base, 270);
        strcpy(long_sub2 + 270, "::SubTwo");

        strada_set_package(long_base);
        assert(strada_current_package() && strcmp(strada_current_package(), long_base) == 0);
        strada_method_register(long_base, "base_only", r13_long_base_only);
        strada_inherit(long_sub1, long_base);
        strada_inherit(long_sub2, long_base);
        strada_method_register(long_sub1, "ident", r13_long_pkg1_id);
        strada_method_register(long_sub2, "ident", r13_long_pkg2_id);

        StradaValue *o1 = strada_bless(strada_new_hash(), long_sub1);
        StradaValue *o2 = strada_bless(strada_new_hash(), long_sub2);
        assert(strada_isa(o1, long_base) == 1 && strada_isa(o2, long_base) == 1);
        assert(strada_isa(o1, long_sub2) == 0 && strada_isa(o2, long_sub1) == 0);
        assert(strada_can(o1, "base_only") == 1 && strada_can(o2, "base_only") == 1);

        StradaValue *r1 = strada_method_call(o1, "ident", strada_new_array());
        StradaValue *r2 = strada_method_call(o2, "ident", strada_new_array());
        assert(strada_to_int(r1) == 101 && strada_to_int(r2) == 202);
        strada_decref(r1); strada_decref(r2);
        {
            const char bin_s[] = "a\0b\0c";
            const char bin_d[] = "\0";
            strada_regex_set_subject_len(5);
            StradaArray *sp = strada_string_split_n(bin_s, bin_d, 1);
            strada_regex_clear_subject_len();
            StradaValue *spv = strada_new_array_from_av(sp);
            assert(spv->value.av->size == 3);
            assert(STRADA_STR_BYTELEN(strada_array_get(spv->value.av, 0)) == 1);
            assert(STRADA_STR_BYTELEN(strada_array_get(spv->value.av, 1)) == 1);
            assert(STRADA_STR_BYTELEN(strada_array_get(spv->value.av, 2)) == 1);
            assert(strada_array_get(spv->value.av, 0)->value.pv[0] == 'a');
            assert(strada_array_get(spv->value.av, 1)->value.pv[0] == 'b');
            assert(strada_array_get(spv->value.av, 2)->value.pv[0] == 'c');
            strada_decref(spv);
        }

        strada_set_method_package(long_sub1);
        assert(strada_get_method_package() && strcmp(strada_get_method_package(), long_sub1) == 0);
        strada_set_method_package(NULL);
        assert(strada_get_method_package() == NULL);
        strada_set_package("main");
        strada_decref(o1); strada_decref(o2);
    }

    /* R13-RT2: directories / non-fd streams / meta section guards */
    {
        StradaValue *dfh = strada_open(".", "r");
        if (dfh && dfh->type == STRADA_FILEHANDLE && dfh->value.fh) {
            StradaValue *rf = strada_read_file(dfh);
            assert(rf && rf->type == STRADA_UNDEF);
            strada_decref(rf);
            StradaValue *sf = strada_slurp_fh(dfh);
            assert(sf && sf->type == STRADA_UNDEF);
            strada_decref(sf);
            strada_decref(strada_close(dfh));
        }
        if (dfh) strada_decref(dfh);

        StradaValue *dot = strada_new_str(".");
        StradaValue *ms = strada_read_meta_section(dot);
        assert(ms && ms->type == STRADA_STR && STRADA_STR_BYTELEN(ms) == 0);
        strada_decref(ms); strada_decref(dot);

        /* fmemopen (fileno == -1) must still work */
        StradaValue *mfh = strada_open_str("abc\0def", "r");
        /* Note strada_open_str uses C strlen, verify normal non-dir memstream read */
        StradaValue *mrf = strada_read_file(mfh);
        assert(mrf && mrf->type == STRADA_STR && strcmp(mrf->value.pv, "abc") == 0);
        strada_decref(mrf);
        strada_decref(strada_close(mfh));
        strada_decref(mfh);
    }

    /* R13-RT3: sprintf %*v combined with * width and .* precision */
    {
        StradaValue *fmt1 = strada_new_str("%*v*d");
        StradaValue *sep = strada_new_str("-");
        StradaValue *ab = strada_new_str("AB");
        StradaValue *r1 = strada_sprintf_sv(fmt1, 3, sep, STRADA_MAKE_TAGGED_INT(4), ab);
        assert(r1 && r1->type == STRADA_STR && strcmp(r1->value.pv, "  65-  66") == 0);
        strada_decref(r1); strada_decref(fmt1);

        StradaValue *fmt2 = strada_new_str("%*v.*d");
        StradaValue *r2 = strada_sprintf_sv(fmt2, 3, sep, STRADA_MAKE_TAGGED_INT(3), ab);
        assert(r2 && r2->type == STRADA_STR && strcmp(r2->value.pv, "065-066") == 0);
        strada_decref(r2); strada_decref(fmt2); strada_decref(sep); strada_decref(ab);
    }

    /* R13-RT4, RT5, RT6 setup: blessed '""' object + tied scalar with embedded NUL & UTF-8 */
    strada_overload_register("R13BinOver", "\"\"", r13_bin_stringify);
    strada_method_register("R13Tied", "TIESCALAR", r13_tied_tiescalar);
    strada_method_register("R13Tied", "FETCH", r13_tied_fetch);
    {
        StradaValue *h1 = strada_new_hash();
        StradaValue *v1 = strada_new_str_len_utf8("\xc3\xa9\0a", 4);
        strada_hv_store(h1, "val", v1);
        strada_decref(v1);
        StradaValue *o1 = strada_bless(h1, "R13BinOver");

        StradaValue *h2 = strada_new_hash();
        StradaValue *v2 = strada_new_str_len_utf8("\xc3\xa9\0b", 4);
        strada_hv_store(h2, "val", v2);
        strada_decref(v2);
        StradaValue *o2 = strada_bless(h2, "R13BinOver");

        /* R13-RT4: NUL-safe compare for blessed '""' and tied scalars */
        assert(strada_sv_strcmp(o1, o2) < 0);
        assert(strada_sv_strcmp(o2, o1) > 0);
        StradaValue *tv1 = strada_new_undef();
        StradaValue *tref1 = strada_new_ref(tv1, '$');
        StradaValue *tv2 = strada_new_undef();
        StradaValue *tref2 = strada_new_ref(tv2, '$');
        StradaValue *bv1 = strada_new_str_len("p\0x", 3);
        StradaValue *bv2 = strada_new_str_len("p\0y", 3);
        strada_decref(strada_tie_scalar(tref1, "R13Tied", 1, bv1));
        strada_decref(strada_tie_scalar(tref2, "R13Tied", 1, bv2));
        assert(strada_sv_strcmp(tv1, tv2) < 0);
        strada_decref(bv1); strada_decref(bv2);
        strada_decref(tref1); strada_decref(tref2);
        strada_decref(tv1); strada_decref(tv2);

        /* R13-RT5: strada_regex_build_result preserves UTF-8 & embedded NUL from overload */
        StradaValue *matches = strada_new_array();
        StradaValue *m0 = strada_new_array();
        strada_array_push(m0->value.av, STRADA_MAKE_TAGGED_INT(1));
        strada_array_push(m0->value.av, STRADA_MAKE_TAGGED_INT(2));
        strada_array_push(matches->value.av, m0);
        strada_decref(m0);
        StradaValue *repls = strada_new_array();
        strada_array_push(repls->value.av, o1);
        StradaValue *rebuilt = strada_regex_build_result("XYZ", matches, repls);
        assert(rebuilt && rebuilt->type == STRADA_STR);
        assert(STRADA_STR_BYTELEN(rebuilt) == 6);
        assert(memcmp(rebuilt->value.pv, "X\xc3\xa9\0aZ", 6) == 0);
        assert(STRADA_STR_IS_UTF8(rebuilt));
        strada_decref(rebuilt); strada_decref(matches); strada_decref(repls);

        /* R13-RT6: fwrite / write_file_n honor overload + binary NULs */
        StradaValue *wfh = strada_open_str("", "w");
        assert(strada_fwrite_sv(wfh, o1) == 4);
        assert(strada_fwrite_sv_n(wfh, o2, 3) == 3);
        assert(strada_write_file_n(wfh, o2, 4) == 4);
        StradaValue *written = strada_str_from_fh(wfh);
        assert(written && STRADA_STR_BYTELEN(written) == 11);
        assert(memcmp(written->value.pv, "\xc3\xa9\0a\xc3\xa9\0\xc3\xa9\0b", 11) == 0);
        strada_decref(written);
        strada_decref(strada_close(wfh));
        strada_decref(wfh);

        strada_decref(o1); strada_decref(o2);
    }

    /* R13-RT7: pack 'U' skips negative codepoints */
    {
        StradaValue *pa = strada_new_array();
        strada_array_push(pa->value.av, STRADA_MAKE_TAGGED_INT(-1));
        strada_array_push(pa->value.av, STRADA_MAKE_TAGGED_INT(0x110000));
        strada_array_push(pa->value.av, STRADA_MAKE_TAGGED_INT(65));
        StradaValue *pk = strada_pack("U*", pa);
        assert(pk && pk->type == STRADA_STR && STRADA_STR_BYTELEN(pk) == 1 && pk->value.pv[0] == 'A');
        strada_decref(pk); strada_decref(pa);
    }

    printf("R13 runtime tests OK\n");
}

/* --- R14 runtime test helpers --- */
extern StradaValue *perla_irs;

static StradaValue* r14_th_tiehash(StradaValue *self, StradaValue *args) {
    (void)self; (void)args;
    return strada_bless(strada_new_hash(), "R14TiedHash");
}
static StradaValue* r14_th_fetch(StradaValue *self, StradaValue *args) {
    StradaValue *k = strada_array_get_owned(args->value.av, 0);
    char *ks = strada_to_str(k);
    StradaValue *v = strada_hash_get(self->value.hv, ks);
    free(ks);
    strada_decref(k);
    if (!v) return strada_new_undef();
    strada_incref(v);
    return v;
}
static StradaValue* r14_th_store(StradaValue *self, StradaValue *args) {
    StradaValue *k = strada_array_get_owned(args->value.av, 0);
    StradaValue *v = strada_array_get_owned(args->value.av, 1);
    char *ks = strada_to_str(k);
    strada_hash_set(self->value.hv, ks, v);
    free(ks);
    strada_decref(k);
    strada_decref(v);
    return strada_new_undef();
}
static StradaValue* r14_ta_tiearray(StradaValue *self, StradaValue *args) {
    (void)self; (void)args;
    return strada_bless(strada_new_hash(), "R14TiedArr");
}
static StradaValue* r14_ta_fetch(StradaValue *self, StradaValue *args) {
    StradaValue *idx_sv = strada_array_get_owned(args->value.av, 0);
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)strada_to_int(idx_sv));
    strada_decref(idx_sv);
    StradaValue *v = strada_hash_get(self->value.hv, buf);
    if (!v) return strada_new_undef();
    strada_incref(v);
    return v;
}
static StradaValue* r14_ta_store(StradaValue *self, StradaValue *args) {
    StradaValue *idx_sv = strada_array_get_owned(args->value.av, 0);
    StradaValue *v = strada_array_get_owned(args->value.av, 1);
    char buf[32];
    snprintf(buf, sizeof(buf), "%lld", (long long)strada_to_int(idx_sv));
    strada_decref(idx_sv);
    strada_hash_set(self->value.hv, buf, v);
    strada_decref(v);
    return strada_new_undef();
}

void test_r14_runtime() {
    printf("Testing R14 runtime fixes...\n");

    /* R14-RT1: strada_regex_replace_sv literal fast path guards empty pattern & $/\ metachars */
    {
        StradaValue *s = strada_new_str("cat");
        StradaValue *p_a = strada_new_str("a");
        StradaValue *r_meta = strada_new_str("[$&]");
        StradaValue *res1 = strada_regex_replace_sv(s, p_a, r_meta, "", 0);
#ifdef HAVE_PCRE2
        assert(res1 && res1->type == STRADA_STR && strcmp(res1->value.pv, "c[a]t") == 0);
#else
        assert(res1 && res1->type == STRADA_STR && strcmp(res1->value.pv, "c[$&]t") == 0);
#endif
        strada_decref(res1);

        StradaValue *r_ctx = strada_new_str("[$0$$]");
        StradaValue *res2 = strada_regex_replace_sv(s, p_a, r_ctx, "", 0);
#ifdef HAVE_PCRE2
        assert(res2 && res2->type == STRADA_STR && strcmp(res2->value.pv, "c[a$]t") == 0);
#else
        assert(res2 && res2->type == STRADA_STR && strcmp(res2->value.pv, "c[$0$$]t") == 0);
#endif
        strada_decref(res2);

        StradaValue *p_empty = strada_new_str("");
        StradaValue *r_x = strada_new_str("X");
        StradaValue *res3 = strada_regex_replace_sv(s, p_empty, r_x, "", 0);
        assert(res3 && res3->type == STRADA_STR && strcmp(res3->value.pv, "Xcat") == 0);
        strada_decref(res3);

        StradaValue *sbin = strada_new_str_len("a\0b", 3);
        StradaValue *r_e = strada_new_str("");
        StradaValue *res4 = strada_regex_replace_sv(sbin, p_empty, r_e, "", 1);
#ifdef HAVE_PCRE2
        assert(res4 && res4->type == STRADA_STR && STRADA_STR_BYTELEN(res4) == 3);
        assert(memcmp(res4->value.pv, "a\0b", 3) == 0);
#else
        assert(res4 && res4->type == STRADA_STR && strcmp(res4->value.pv, "a") == 0);
#endif
        strada_decref(res4);

        strada_decref(sbin);
        strada_decref(r_e);
        strada_decref(r_x);
        strada_decref(p_empty);
        strada_decref(r_ctx);
        strada_decref(r_meta);
        strada_decref(p_a);
        strada_decref(s);
    }

    /* R14-RT2: strada_autoviv_* & strada_hv_autoviv on tied hashes and tied arrays */
    {
        strada_method_register("R14TiedHash", "TIEHASH", r14_th_tiehash);
        strada_method_register("R14TiedHash", "FETCH", r14_th_fetch);
        strada_method_register("R14TiedHash", "STORE", r14_th_store);
        strada_method_register("R14TiedArr", "TIEARRAY", r14_ta_tiearray);
        strada_method_register("R14TiedArr", "FETCH", r14_ta_fetch);
        strada_method_register("R14TiedArr", "STORE", r14_ta_store);

        StradaValue *th = strada_new_hash();
        StradaValue *th_ref = strada_new_ref(th, '%');
        strada_decref(strada_tie_hash(th_ref, "R14TiedHash", 0));

        StradaValue *vh1 = strada_autoviv_hash(th, "h1");
        StradaValue *v_ok1 = strada_new_str("ok1");
        strada_hv_store(vh1, "k1", v_ok1);
        strada_decref(v_ok1);
        StradaValue *vh1_again = strada_hv_autoviv(th, "h1");
        StradaValue *k1_got = strada_hv_fetch_owned(vh1_again, "k1");
        assert(k1_got && k1_got->type == STRADA_STR && strcmp(k1_got->value.pv, "ok1") == 0);
        strada_decref(k1_got);

        StradaValue *va1 = strada_autoviv_array(th, "a1");
        StradaValue *v_ok2 = strada_new_str("ok2");
        strada_av_push_sv(va1, v_ok2);
        strada_decref(v_ok2);
        StradaValue *va1_got = strada_tied_hash_fetch(th, "a1");
        StradaValue *elem0 = strada_array_get_owned(strada_deref_array(va1_got), 0);
        assert(elem0 && elem0->type == STRADA_STR && strcmp(elem0->value.pv, "ok2") == 0);
        strada_decref(elem0);
        strada_decref(va1_got);

        StradaValue *ta = strada_new_array();
        StradaValue *ta_ref = strada_new_ref(ta, '@');
        strada_decref(strada_tie_array(ta_ref, "R14TiedArr", 0));

        StradaValue *eh = strada_autoviv_elem_hash(ta, 2);
        StradaValue *v_ev = strada_new_str("ev");
        strada_hv_store(eh, "ek", v_ev);
        strada_decref(v_ev);
        StradaValue *eh_got = strada_tied_array_fetch(ta, 2);
        StradaValue *ek_got = strada_hv_fetch_owned(eh_got, "ek");
        assert(ek_got && ek_got->type == STRADA_STR && strcmp(ek_got->value.pv, "ev") == 0);
        strada_decref(ek_got);
        strada_decref(eh_got);

        StradaValue *ea = strada_autoviv_elem_array(ta, 3);
        StradaValue *v_av = strada_new_str("av3");
        strada_av_push_sv(ea, v_av);
        strada_decref(v_av);
        StradaValue *ea_got = strada_tied_array_fetch(ta, 3);
        StradaValue *av0 = strada_array_get_owned(strada_deref_array(ea_got), 0);
        assert(av0 && av0->type == STRADA_STR && strcmp(av0->value.pv, "av3") == 0);
        strada_decref(av0);
        strada_decref(ea_got);

        strada_decref(ta_ref);
        strada_decref(ta);
        strada_decref(th_ref);
        strada_decref(th);
    }

    /* R14-RT3: strada_read_line_max & strada_read_all_lines respect multi-char & undef $/ */
    {
        StradaValue *saved_irs = perla_irs;
        StradaValue *irs_mc = strada_new_str("::");
        perla_irs = irs_mc;

        StradaValue *fh1 = strada_open_str("alpha::beta::gamma", "r");
        StradaValue *l1 = strada_read_line_max(fh1, STRADA_MAKE_TAGGED_INT(0));
        StradaValue *l2 = strada_read_line_max(fh1, STRADA_MAKE_TAGGED_INT(0));
        StradaValue *l3 = strada_read_line_max(fh1, STRADA_MAKE_TAGGED_INT(0));
        StradaValue *l4 = strada_read_line_max(fh1, STRADA_MAKE_TAGGED_INT(0));
        assert(l1 && l1->type == STRADA_STR && strcmp(l1->value.pv, "alpha::") == 0);
        assert(l2 && l2->type == STRADA_STR && strcmp(l2->value.pv, "beta::") == 0);
        assert(l3 && l3->type == STRADA_STR && strcmp(l3->value.pv, "gamma") == 0);
        assert(l4 && l4->type == STRADA_UNDEF);
        strada_decref(l1); strada_decref(l2); strada_decref(l3); strada_decref(l4);
        strada_decref(strada_close(fh1));
        strada_decref(fh1);

        StradaValue *fh2 = strada_open_str("alpha::beta::gamma", "r");
        StradaValue *all1 = strada_read_all_lines(fh2);
        assert(all1 && all1->type == STRADA_ARRAY && all1->value.av->size == 3);
        StradaValue *a0 = strada_array_get_owned(all1->value.av, 0);
        StradaValue *a2 = strada_array_get_owned(all1->value.av, 2);
        assert(strcmp(a0->value.pv, "alpha::") == 0);
        assert(strcmp(a2->value.pv, "gamma") == 0);
        strada_decref(a0); strada_decref(a2); strada_decref(all1);
        strada_decref(strada_close(fh2));
        strada_decref(fh2);

        StradaValue *irs_undef = strada_new_undef();
        perla_irs = irs_undef;
        StradaValue *in_sv = strada_new_str_len("x\0y\nz", 5);
        StradaValue *mode_r = strada_new_str("r");
        StradaValue *fh3 = strada_open_str_sv(in_sv, mode_r);
        StradaValue *sl1 = strada_read_line_max(fh3, STRADA_MAKE_TAGGED_INT(0));
        assert(sl1 && sl1->type == STRADA_STR && STRADA_STR_BYTELEN(sl1) == 5 && memcmp(sl1->value.pv, "x\0y\nz", 5) == 0);
        strada_decref(sl1);
        strada_decref(strada_close(fh3));
        strada_decref(fh3);

        StradaValue *fh4 = strada_open_str_sv(in_sv, mode_r);
        StradaValue *all2 = strada_read_all_lines(fh4);
        assert(all2 && all2->type == STRADA_ARRAY && all2->value.av->size == 1);
        StradaValue *sl2 = strada_array_get_owned(all2->value.av, 0);
        assert(sl2 && sl2->type == STRADA_STR && STRADA_STR_BYTELEN(sl2) == 5 && memcmp(sl2->value.pv, "x\0y\nz", 5) == 0);
        strada_decref(sl2); strada_decref(all2);
        strada_decref(strada_close(fh4));
        strada_decref(fh4);

        perla_irs = saved_irs;
        strada_decref(in_sv);
        strada_decref(mode_r);
        strada_decref(irs_undef);
        strada_decref(irs_mc);
    }

    /* R14-RT4: strada_slurp_fd across >4096 bytes & embedded NULs */
    {
        unsigned char expected[10000];
        memset(expected, 'Z', sizeof(expected));
        expected[0] = '\0';
        expected[17] = '\0';
        expected[4095] = '\0';
        expected[4096] = '\0';
        expected[8192] = '\0';
        expected[9999] = '\0';

        int fds[2];
        assert(pipe(fds) == 0);
        pid_t pid = fork();
        assert(pid >= 0);
        if (pid == 0) {
            close(fds[0]);
            size_t off = 0;
            while (off < sizeof(expected)) {
                ssize_t w = write(fds[1], expected + off, sizeof(expected) - off);
                if (w <= 0) _exit(1);
                off += (size_t)w;
            }
            close(fds[1]);
            _exit(0);
        }
        close(fds[1]);
        StradaValue *slurped = strada_slurp_fd(STRADA_MAKE_TAGGED_INT(fds[0]));
        close(fds[0]);
        int st = 0;
        assert(waitpid(pid, &st, 0) == pid && WIFEXITED(st) && WEXITSTATUS(st) == 0);
        assert(slurped && slurped->type == STRADA_STR);
        assert(STRADA_STR_BYTELEN(slurped) == sizeof(expected));
        assert(memcmp(slurped->value.pv, expected, sizeof(expected)) == 0);
        strada_decref(slurped);
    }

    printf("R14 runtime tests OK\n");
}

static void test_r17_runtime(void) {
    printf("Testing R17 runtime fixes...\n");

    /* R17: strada_release on weak STRADA_REF (in-place weakened, target alive) */
    {
        StradaValue *t = strada_new_hash();
        StradaValue *sv_a = strada_new_str("alive");
        strada_hv_store(t, "k", sv_a);
        strada_decref(sv_a);
        StradaValue *strong1 = strada_new_ref(t, '%');
        StradaValue *strong2 = strada_new_ref(t, '%');
        strada_decref(t); /* t owned by strong1 and strong2 (refcount == 2) */

        /* strong2->refcount == 1 and t->refcount == 2 -> in-place weaken */
        strada_weaken(&strong2);
        assert(strada_isweak(strong2) == 1);

        StradaValue *rel = strada_release(strong2);
        assert(rel && rel->type == STRADA_UNDEF);
        strada_decref(rel);
        assert(strada_isweak(strong2) == 0);
        strada_decref(strong2);

        /* strong1 & t must remain intact and live */
        StradaValue *v = strada_hv_fetch_owned(strong1->value.rv, "k");
        assert(v && v->type == STRADA_STR && strcmp(v->value.pv, "alive") == 0);
        strada_decref(v);
        strada_decref(strong1);
    }

    /* R17: strada_release on weak STRADA_REF after target already died */
    {
        StradaValue *t = strada_new_array();
        StradaValue *el = strada_new_int(42);
        strada_array_push(t->value.av, el);
        strada_decref(el);
        StradaValue *strong = strada_new_ref(t, '@');
        strada_decref(t);
        StradaValue *w = strong;
        strada_incref(w);
        strada_weaken(&w);
        assert(strada_isweak(w) == 1);
        strada_decref(strong); /* frees t and nullifies w->value.rv */
        StradaValue *rel = strada_release(w);
        strada_decref(rel);
        strada_decref(w);
    }

    printf("R17 runtime tests OK\n");
}


int main() {
    printf("=== Strada Runtime Test ===\n\n");
    
    /* Test scalar values */
    printf("Testing scalars:\n");
    StradaValue *sv_int = strada_new_int(42);
    StradaValue *sv_num = strada_new_num(3.14159);
    StradaValue *sv_str = strada_new_str("Hello, Strada!");
    
    strada_print(sv_int); printf("\n");
    strada_print(sv_num); printf("\n");
    strada_print(sv_str); printf("\n");
    printf("\n");
    
    /* Test arrays */
    printf("Testing arrays:\n");
    StradaValue *sv_arr = strada_new_array();
    strada_array_push(sv_arr->value.av, strada_new_int(1));
    strada_array_push(sv_arr->value.av, strada_new_int(2));
    strada_array_push(sv_arr->value.av, strada_new_int(3));
    strada_array_push(sv_arr->value.av, strada_new_int(4));
    strada_array_push(sv_arr->value.av, strada_new_int(5));
    
    printf("Array contents:\n");
    strada_dumper(sv_arr);
    printf("\n");
    
    /* Test hashes */
    printf("Testing hashes:\n");
    StradaValue *sv_hash = strada_new_hash();
    strada_hash_set(sv_hash->value.hv, "name", strada_new_str("Alice"));
    strada_hash_set(sv_hash->value.hv, "age", strada_new_int(30));
    strada_hash_set(sv_hash->value.hv, "city", strada_new_str("NYC"));
    
    printf("Hash contents:\n");
    strada_dumper(sv_hash);
    printf("\n");
    
    /* Test string operations */
    printf("Testing string operations:\n");
    char *concat = strada_concat("Hello, ", "World!");
    printf("Concatenation: %s\n", concat);
    printf("Length: %zu\n", strada_length(concat));
    free(concat);
    printf("\n");
    
    /* Test type conversions */
    printf("Testing type conversions:\n");
    printf("Int to string: %s\n", strada_to_str(sv_int));
    printf("Num to int: %lld\n", (long long)strada_to_int(sv_num));
    printf("String to num: %g\n", strada_to_num(strada_new_str("123.45")));
    printf("\n");
    
    /* Test nested structures */
    printf("Testing nested structures:\n");
    StradaValue *nested = strada_new_hash();
    
    StradaValue *arr1 = strada_new_array();
    strada_array_push(arr1->value.av, strada_new_int(1));
    strada_array_push(arr1->value.av, strada_new_int(2));
    strada_array_push(arr1->value.av, strada_new_int(3));
    
    StradaValue *inner_hash = strada_new_hash();
    strada_hash_set(inner_hash->value.hv, "x", strada_new_int(10));
    strada_hash_set(inner_hash->value.hv, "y", strada_new_int(20));
    
    strada_hash_set(nested->value.hv, "numbers", arr1);
    strada_hash_set(nested->value.hv, "point", inner_hash);
    strada_hash_set(nested->value.hv, "name", strada_new_str("Nested"));
    
    printf("Nested structure:\n");
    strada_dumper(nested);
    printf("\n");
    
    /* Cleanup */
    strada_decref(sv_int);
    strada_decref(sv_num);
    strada_decref(sv_str);
    strada_decref(sv_arr);
    strada_decref(sv_hash);
    strada_decref(nested);

    test_r10_runtime();
    test_r11_runtime();
    test_r12_runtime();
    test_r13_runtime();
    test_r14_runtime();
    test_r17_runtime();
    
    printf("=== All tests completed ===\n");
    
    return 0;
}

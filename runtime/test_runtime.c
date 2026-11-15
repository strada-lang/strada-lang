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
    strada_decref(u1e); strada_decref(ua1); strada_decref(up_sbe);

    StradaValue *up_sle = strada_new_str_len("\xfe\xff" "ABCDE", 7);
    StradaValue *ua2 = strada_unpack("s</a*", up_sle);
    assert(ua2 && ua2->type == STRADA_ARRAY && ua2->value.av->size == 1);
    StradaValue *u2e = strada_array_get(ua2->value.av, 0);
    assert(u2e && u2e->type == STRADA_STR && u2e->value.pv[0] == '\0');
    strada_decref(u2e); strada_decref(ua2); strada_decref(up_sle);

    StradaValue *up_lbe = strada_new_str_len("\xff\xff\xff\xfe" "ABCDE", 9);
    StradaValue *ua3 = strada_unpack("l>/a*", up_lbe);
    assert(ua3 && ua3->type == STRADA_ARRAY && ua3->value.av->size == 1);
    StradaValue *u3e = strada_array_get(ua3->value.av, 0);
    assert(u3e && u3e->type == STRADA_STR && u3e->value.pv[0] == '\0');
    strada_decref(u3e); strada_decref(ua3); strada_decref(up_lbe);

    StradaValue *up_n = strada_new_str_len("\x80\x00\x00\x01" "abc", 7);
    StradaValue *ua4 = strada_unpack("N/a*", up_n);
    assert(ua4 && ua4->type == STRADA_ARRAY && ua4->value.av->size == 1);
    StradaValue *u4e = strada_array_get(ua4->value.av, 0);
    assert(u4e && u4e->type == STRADA_STR && strcmp(u4e->value.pv, "abc") == 0);
    strada_decref(u4e); strada_decref(ua4); strada_decref(up_n);

    char qbuf[13];
    uint64_t qlen = 0x0000000100000001ULL;
    memcpy(qbuf, &qlen, 8);
    memcpy(qbuf + 8, "hello", 5);
    StradaValue *up_q = strada_new_str_len(qbuf, 13);
    StradaValue *ua5 = strada_unpack("Q/a*", up_q);
    assert(ua5 && ua5->type == STRADA_ARRAY && ua5->value.av->size == 1);
    StradaValue *u5e = strada_array_get(ua5->value.av, 0);
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
    
    printf("=== All tests completed ===\n");
    
    return 0;
}

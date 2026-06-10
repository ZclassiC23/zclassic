/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "framework/condition.h"
#include "util/log_macros.h"

#include "services/chain_restore_integrity.h"
#include "services/chain_restore_repair.h"
#include "util/util.h"
#include "validation/main_state.h"

#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static _Atomic int g_remedy_calls;
static _Atomic int g_last_zero_nbits;
static _Atomic int g_last_tip_window_holes;
static _Atomic int g_last_total_holes;
static _Atomic int g_last_mismatches;
static _Atomic int g_last_tip_height;

static void record_integrity(const struct chain_integrity_result *r)
{
    atomic_store(&g_last_zero_nbits, r ? r->zero_nbits_count : 0);
    atomic_store(&g_last_tip_window_holes, r ? r->tip_window_holes : 0);
    atomic_store(&g_last_total_holes, r ? r->active_chain_holes : 0);
    atomic_store(&g_last_mismatches, r ? r->active_chain_mismatches : 0);
    atomic_store(&g_last_tip_height, r ? r->tip_height : -1);
}

static bool check_integrity(struct chain_integrity_result *out)
{
    struct main_state *ms = condition_engine_main_state();
    if (!ms) {
        if (out) memset(out, 0, sizeof(*out));
        return false;
    }

    struct chain_integrity_result r;
    chain_integrity_check_post_restore(&r, ms);
    record_integrity(&r);
    if (out) *out = r;
    return true;
}

static bool detect_chain_integrity_failed(void)
{
    struct chain_integrity_result r;
    if (!check_integrity(&r))
        return false;
    return !r.ok;
}

static enum condition_remedy_result remedy_chain_integrity_failed(void)
{
    struct main_state *ms = condition_engine_main_state();
    if (!ms)
        return COND_REMEDY_SKIP;

    atomic_fetch_add(&g_remedy_calls, 1);

    LOG_WARN("condition", "[condition:chain_integrity_failed] zero_nbits=%d " "tip_window_holes=%d total_holes=%d mismatches=%d tip_h=%d " "action=chain_restore_finalize", atomic_load(&g_last_zero_nbits), atomic_load(&g_last_tip_window_holes), atomic_load(&g_last_total_holes), atomic_load(&g_last_mismatches), atomic_load(&g_last_tip_height));

    char datadir[1024];
    GetDataDir(true, datadir, sizeof(datadir));

    struct zcl_result fr = chain_restore_finalize(ms, datadir);
    if (!fr.ok)
        LOG_WARN("condition",
                 "[condition:chain_integrity_failed] finalize failed "
                 "code=%d msg=%s", fr.code, fr.message);
    return fr.ok ? COND_REMEDY_OK : COND_REMEDY_FAILED;
}

static bool witness_chain_integrity_failed(int64_t target_at_detect)
{
    (void)target_at_detect;
    // honest-witness-ok: this is NOT poison-absence. check_integrity()
    // re-walks the real active_chain over the block index
    // (chain_integrity_check_post_restore) and recomputes holes / mismatches /
    // zero-nbits from scratch. The remedy (chain_restore_finalize) repairs the
    // chain but sets NO flag this witness reads, so it cannot self-certify —
    // r.ok is an independent structural re-verification that the symptom
    // (broken chain) is gone.
    struct chain_integrity_result r;
    return check_integrity(&r) && r.ok;
}

static struct condition c_chain_integrity_failed = {
    .name = "chain_integrity_failed",
    .severity = COND_CRITICAL,
    .poll_secs = 30,
    .backoff_secs = 300,
    .max_attempts = 2,
    .detect = detect_chain_integrity_failed,
    .remedy = remedy_chain_integrity_failed,
    .witness = witness_chain_integrity_failed,
    .witness_window_secs = 60,
};

void register_chain_integrity_failed(void)
{
    (void)condition_register(&c_chain_integrity_failed);
}

#ifdef ZCL_TESTING
void chain_integrity_failed_test_reset(void)
{
    struct condition_state *s = &c_chain_integrity_failed.state;
    atomic_store(&s->first_detect_unix, 0);
    atomic_store(&s->last_poll_unix, 0);
    atomic_store(&s->last_remedy_unix, 0);
    atomic_store(&s->last_operator_needed_unix, 0);
    atomic_store(&s->target_at_detect, 0);
    atomic_store(&s->cleared_count, 0);
    condition_reset_state(&c_chain_integrity_failed);
    atomic_store(&g_remedy_calls, 0);
    atomic_store(&g_last_zero_nbits, 0);
    atomic_store(&g_last_tip_window_holes, 0);
    atomic_store(&g_last_total_holes, 0);
    atomic_store(&g_last_mismatches, 0);
    atomic_store(&g_last_tip_height, -1);
}

int chain_integrity_failed_test_remedy_calls(void)
{
    return atomic_load(&g_remedy_calls);
}
#endif

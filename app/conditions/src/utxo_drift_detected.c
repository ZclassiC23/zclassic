/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "conditions/utxo_drift_detected.h"
#include "util/log_macros.h"
#include "framework/condition.h"

#include "config/runtime.h"
#include "event/event.h"
#include "models/database.h"

#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <string.h>

static _Atomic int64_t g_height_at_detect = -1;

#ifdef ZCL_TESTING
static struct node_db *g_test_ndb;
static _Atomic int g_test_remedy_calls;
#endif

static struct node_db *runtime_ndb(void)
{
#ifdef ZCL_TESTING
    if (g_test_ndb)
        return g_test_ndb;
#endif
    return app_runtime_node_db();
}

static bool read_drift_flag(struct node_db *ndb, int64_t *out)
{
    int64_t drift = 0;
    if (out)
        *out = 0;
    if (!ndb || !ndb->open)
        return false;
    if (!node_db_state_get_int(ndb, "utxo_drift_detected", &drift))
        return false;
    if (out)
        *out = drift;
    return drift != 0;
}

static bool detect_utxo_drift_detected(void)
{
    struct node_db *ndb = runtime_ndb();
    int64_t drift = 0;
    if (!read_drift_flag(ndb, &drift))
        return false;

    int64_t height = -1;
    (void)node_db_state_get_int(ndb, "utxo_audit_last_height", &height);
    atomic_store(&g_height_at_detect, height);
    return true;
}

static void read_state_text(struct node_db *ndb, const char *key,
                            char *out, size_t out_len)
{
    size_t len = 0;
    if (!out || out_len == 0)
        return;
    out[0] = '\0';
    if (!ndb || !key ||
        !node_db_state_get(ndb, key, out, out_len - 1, &len))
        return;
    if (len >= out_len)
        len = out_len - 1;
    out[len] = '\0';
}

static enum condition_remedy_result remedy_utxo_drift_detected(void)
{
    struct node_db *ndb = runtime_ndb();
    char local[65];
    char remote[65];
    read_state_text(ndb, "utxo_audit_last_local_sha3",
                    local, sizeof(local));
    read_state_text(ndb, "utxo_audit_last_remote_sha3",
                    remote, sizeof(remote));

#ifdef ZCL_TESTING
    atomic_fetch_add(&g_test_remedy_calls, 1);
#endif

    LOG_WARN("condition", "[condition:utxo_drift_detected] height=%" PRId64 " " "local_sha3=%s remote_sha3=%s action=operator_escalation", atomic_load(&g_height_at_detect), local[0] ? local : "-", remote[0] ? remote : "-");
    event_emitf(EV_UTXO_DRIFT_DETECTED, 0,
                "condition=utxo_drift_detected height=%lld local_sha3=%s "
                "remote_sha3=%s",
                (long long)atomic_load(&g_height_at_detect),
                local[0] ? local : "-",
                remote[0] ? remote : "-");

    /* UTXO drift is consensus-critical. Until the repair job lands, keep the
     * condition unresolved so the engine pages instead of pretending a
     * destructive wipe/reimport was safe to run automatically. */
    return COND_REMEDY_FAILED;
}

static bool witness_utxo_drift_detected(int64_t target_at_detect)
{
    (void)target_at_detect;
    // honest-witness-ok: remedy_utxo_drift_detected returns COND_REMEDY_FAILED
    // and NEVER clears the drift flag — only an external repair (or operator)
    // can. So this poison-absence read cannot be self-certified by the remedy
    // (the Law-7 trap). It exists solely for the engine's !detected
    // deactivation path: once drift genuinely resolves externally the
    // condition must clear, which needs a truthful flag read, never a constant.
    return !read_drift_flag(runtime_ndb(), NULL);
}

static struct condition c_utxo_drift_detected = {
    .name = "utxo_drift_detected",
    .severity = COND_CRITICAL,
    .poll_secs = 60,
    .backoff_secs = 300,
    .max_attempts = 1,
    .detect = detect_utxo_drift_detected,
    .remedy = remedy_utxo_drift_detected,
    .witness = witness_utxo_drift_detected,
    .witness_window_secs = 60,
};

void register_utxo_drift_detected(void)
{
    (void)condition_register(&c_utxo_drift_detected);
}

#ifdef ZCL_TESTING
void utxo_drift_detected_test_reset(void)
{
    g_test_ndb = NULL;
    atomic_store(&g_height_at_detect, -1);
    atomic_store(&g_test_remedy_calls, 0);
}

void utxo_drift_detected_test_set_node_db(struct node_db *ndb)
{
    g_test_ndb = ndb;
}

int utxo_drift_detected_test_remedy_calls(void)
{
    return atomic_load(&g_test_remedy_calls);
}
#endif

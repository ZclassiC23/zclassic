/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * utxo_parity_service — STANDING UTXO-set parity vs a reference commitment.
 *
 * At the finalized frontier the service recomputes the LOCAL SHA3 UTXO
 * commitment and diffs it against a pluggable REFERENCE commitment. On a
 * same-height byte mismatch it persists the existing 'utxo_drift_detected'
 * flag; the already-wired utxo_drift_detected Condition escalates it and the
 * condition framework pages the operator for free (no new EV_OPERATOR_NEEDED
 * emit lives here — single paging source of truth).
 *
 * Threading: this module creates no background work. The supervised
 * chain.utxo_parity_poll Job owns cadence and calls utxo_parity_tick_once().
 * The tick is event-gated by a finalized-frontier marker maintained from an
 * EV_CHAIN_TIP_COMMIT observer cross-checked against the durable finalized
 * height, so the service is genuinely dormant until the tip advances and an
 * exact reference source is wired.
 *
 * Dormancy/guarding: prod ships cfg.enabled=false with no reference and no
 * ZCL_PARITY_ENABLE, so nothing runs against the live node by default.
 */

#ifndef ZCL_SERVICES_UTXO_PARITY_SERVICE_H
#define ZCL_SERVICES_UTXO_PARITY_SERVICE_H

#include "services/utxo_audit_service.h"
#include "services/utxo_reference_source.h"
#include "util/result.h"

#include <stdbool.h>
#include <stdint.h>

struct node_db;
struct json_value;

struct utxo_parity_config {
    bool enabled;             /* master gate; default false (dormant) */
    int  finality_depth;      /* stability margin below the frontier; def 100 */
    int  max_checks_per_tick; /* bound full-set SHA3 cost per tick; def 1 */
    struct utxo_parity_rpc_config rpc; /* prod zclassicd reference transport */
};

/* Lifecycle. `ndb` is the runtime node_db (may be NULL in early boot — the
 * tick re-reads app_runtime_node_db() defensively when its handle is NULL). */
struct zcl_result utxo_parity_init(const struct utxo_parity_config *cfg,
                                   struct node_db *ndb);

/* Inject the reference source (test seam + prod wire). `src` is caller-owned
 * and must outlive the service. Passing NULL detaches the source (dormant). */
void utxo_parity_set_reference_source(const struct utxo_reference_source *src);

/* Install the EV_CHAIN_TIP_COMMIT observer that maintains the finalized
 * frontier marker. Cheap and always safe; idempotent. */
void utxo_parity_observe_finalization(void);

/* Scheduler-independent Job body. Dormant unless enabled + a reference + the
 * env gate (or cfg.enabled) and a fresh stable frontier target exist. */
void utxo_parity_tick_once(void);

/* Synchronous one-shot check at the live applied `height` against the wired
 * reference (unit test + MCP escape hatch). Returns the comparator result. */
struct zcl_result utxo_parity_check_height(int32_t height,
                                           struct utxo_audit_result *out);

/* Test seam: force the finalized-frontier marker (mirrors the observer). */
void utxo_parity_set_frontier_for_test(int32_t height);

bool utxo_parity_dump_state_json(struct json_value *out, const char *key);
void utxo_parity_reset_for_test(void);

#endif /* ZCL_SERVICES_UTXO_PARITY_SERVICE_H */

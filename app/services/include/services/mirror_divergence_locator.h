/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * mirror_divergence_locator — fail-loud validation pack, check 6.
 *
 * Evidence (2026-06-11): the mirror hash-disagreement blocker fired 279
 * times as identical quiet warnings while the node SERVED a poisoned
 * chain. Disagreement alone is not actionable; the operator needs WHERE
 * the chains diverge. On a hash-disagreement this locator binary-searches
 * the first diverging height against the co-located zclassicd (bounded
 * ~22 getblockhash probes over 3.2M heights), then emits ONE loud
 * EV_OPERATOR_NEEDED with (first_diverging_height, ours, theirs),
 * registers the PERMANENT `mirror.divergence_located` blocker (distinct
 * from the existing rate-limited transient `mirror.hash-disagreement`,
 * which stays as-is), latches the HOLD (a node serving a poisoned chain
 * must not extend it), and feeds oracle_policy_record_disagreement.
 *
 * Rate limits: re-locates at most once per 10 minutes; re-pages only when
 * first_div changes. Aborts silently on RPC errors (the existing
 * mirror.rpc-unreachable blocker covers that) and on a transient
 * disagreement that re-verifies as agreement. Crash-only: never FATAL. */

#ifndef ZCL_SERVICES_MIRROR_DIVERGENCE_LOCATOR_H
#define ZCL_SERVICES_MIRROR_DIVERGENCE_LOCATOR_H

#include <stdbool.h>

/* Entry point, called from the legacy-mirror hash-disagreement sites.
 * Runs inline on the mirror tick thread (seconds, bounded by the RPC
 * timeouts). Returns the located first diverging height, or -1 when the
 * run was skipped/aborted (rate-limited, RPC error, transient). */
int mirror_divergence_locate(int disagree_height);

#ifdef ZCL_TESTING
/* Probe injection: local/remote hash-at-height (65-byte hex out).
 * NULL restores the production probes. */
typedef bool (*mdl_probe_fn)(int height, char out_hex[65]);
void mirror_divergence_set_probes_for_testing(mdl_probe_fn local,
                                              mdl_probe_fn remote);
void mirror_divergence_reset_for_testing(void);
int  mirror_divergence_probes_last_run_for_testing(void);
#endif

#endif /* ZCL_SERVICES_MIRROR_DIVERGENCE_LOCATOR_H */

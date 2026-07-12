/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP Metrics — Prometheus-style in-process counters and histograms.
 *
 * Observes EV_MCP_REQUEST and maintains:
 *   - per-(tool, code) request counters
 *   - per-tool latency histogram (5 buckets: 1ms, 5ms, 25ms, 100ms, 500ms)
 *   - aggregated summary counters (total, ok, error, rate-limited, etc.)
 *
 * The metrics view is exposed by the `zcl_metrics` tool, which emits a
 * Prometheus text-format dump that any operator or scraper can consume.
 *
 * `zcl_metrics_reset` clears every counter (destructive-gated).
 */

#ifndef ZCL_METRICS_PROMETHEUS_H
#define ZCL_METRICS_PROMETHEUS_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Max distinct tool names tracked.  Beyond this limit new tools are
 * folded into a "__other__" bucket rather than growing the table. */
#define METRICS_PROMETHEUS_MAX_TOOLS 80

/* Max distinct (tool, code) pairs.  Bounded to keep memory predictable
 * and to protect against unbounded-cardinality attacks via tool spoofing. */
#define METRICS_PROMETHEUS_MAX_COUNTERS 512

/* Histogram bucket count. */
#define METRICS_PROMETHEUS_HIST_BUCKETS 6

/* Max distinct (kind, reason) pairs tracked for consensus rejects.
 * Beyond this limit, rejects still count toward the per-kind totals but
 * their reason is folded into a "__other__" bucket.  The REJECT_IF/UNLESS
 * reason strings are a closed set defined by the consensus code, so the
 * practical upper bound is known, but we cap cardinality defensively. */
#define METRICS_PROMETHEUS_MAX_REJECT_REASONS 48

/* Register the EV_MCP_REQUEST observer.  Idempotent — calling twice does
 * nothing on the second call. */
void metrics_prometheus_init(void);

/* Manual counter increment — used by tests and call sites that don't
 * route through the event system.  Code "OK" for success; any other
 * string for an error code (AUTH_REQUIRED, RATE_LIMITED, …). */
void metrics_prometheus_record(const char *tool, const char *code, int64_t dur_us);

/* Clear all counters.  Tests and `zcl_metrics_reset`. */
void metrics_prometheus_reset(void);

/* Write the Prometheus text format dump into buf.  Returns bytes
 * written (excluding NUL).  Truncates silently if the buffer is small. */
size_t metrics_prometheus_render_prometheus(char *buf, size_t cap);

/* Introspection (tests). */
size_t metrics_prometheus_counter_count(void);
uint64_t metrics_prometheus_get(const char *tool, const char *code);
uint64_t metrics_prometheus_total_requests(void);
uint64_t metrics_prometheus_total_errors(void);

/* ── Peer scoring counters ────────────────────────────────────
 *
 * Subscribed to EV_PEER_MISBEHAVE and EV_PEER_BANNED via the same
 * observer install path as the MCP request counters.  The handler
 * extracts the offence kind from the event payload (the first
 * whitespace-separated word after the score header) and buckets it
 * into a small allowlisted set; anything unrecognised goes into
 * "other".  Counts are exposed in the Prometheus dump as:
 *
 *   zcl_peer_offences_total{kind="..."} N
 *   zcl_peer_offences_total{kind="all"} N         # convenience aggregate
 *   zcl_peer_bans_total N
 *
 * The `zcl_peer_report` MCP tool wraps these in a small JSON object
 * with the live peer-scoring config so an operator can see the
 * threshold/decay/bans-since-boot in one call.
 */

/* Manual record helpers — used by tests and by the in-process event
 * observer.  `kind` should be one of the names returned by
 * peer_offence_name() (timeout, invalid_message, unrequested,
 * offer_rejected, flood, invalid_payload, invalid_header,
 * invalid_chunk, invalid_block, invalid_proof, protocol_violation) —
 * anything else is folded into "other" rather than expanding the
 * cardinality. */
void metrics_prometheus_record_peer_offence(const char *kind);
void metrics_prometheus_record_peer_ban(void);

/* Aggregate query helpers (tests + zcl_peer_report). */
uint64_t metrics_prometheus_peer_offences_total(void);
uint64_t metrics_prometheus_peer_offences_for_kind(const char *kind);
uint64_t metrics_prometheus_peer_bans_total(void);

/* Render the peer-scoring summary as a small JSON object suitable
 * for embedding in an MCP response body.  Includes the live config
 * (threshold / ban_hours / decay_per_min) and the per-kind counters.
 * Returns bytes written (excluding NUL); silently truncates on a
 * too-small buffer the same way the Prometheus dump does. */
size_t metrics_prometheus_peer_report_json(char *buf, size_t cap);

/* ── HTTP RPC middleware report ───────────────────────────────
 *
 * Snapshots the live RPC middleware stats (from the global handle
 * registered by httpserver.c via rpc_http_middleware_set_global) and
 * renders a small JSON object suitable for embedding in an MCP
 * response body.  Shape:
 *
 *   {
 *     "config": { global_rps, global_burst, per_ip_rps, per_ip_burst,
 *                 auth_fail_threshold, ban_seconds },
 *     "stats":  { allowed, rate_limited_global, rate_limited_per_ip,
 *                 banned_rejected, bans_issued, auth_failures },
 *     "tracked_ips": N,
 *     "active_bans": N
 *   }
 *
 * When the RPC server hasn't started (or is in the middle of shutdown),
 * the global pointer is NULL and the report returns an empty-stats
 * envelope with `"rpc_server":"inactive"` set so operators can tell
 * the difference between "no traffic yet" and "no server at all".
 *
 * The Prometheus dump (metrics_prometheus_render_prometheus) also emits a
 * `zcl_rpc_*` block derived from the same snapshot. */
size_t metrics_prometheus_rpc_report_json(char *buf, size_t cap);

/* ── Consensus reject counters ────────────────────────────────
 *
 * Subscribed to EV_CONSENSUS_REJECT_TX and EV_CONSENSUS_REJECT_BLOCK
 * via `metrics_prometheus_init()`.  The handler extracts the `reason=...`
 * field from the event payload and records it against a bounded
 * (kind, reason) table — where kind is "tx" or "block".  Reasons
 * beyond `METRICS_PROMETHEUS_MAX_REJECT_REASONS` fold into "__other__" so
 * cardinality stays bounded under unexpected payloads.
 *
 * The Prometheus dump exposes:
 *
 *   zcl_consensus_rejects_total{kind="tx",reason="..."}     N
 *   zcl_consensus_rejects_total{kind="block",reason="..."}  N
 *   zcl_consensus_rejects_total{kind="tx",reason="__other__"} N
 *   zcl_consensus_rejects_total{kind="block",reason="__other__"} N
 *   zcl_consensus_rejects_total{kind="all",reason="all"}    N
 *
 * The `zcl_consensus_report` MCP tool wraps the counters in a small
 * JSON envelope for operators who want a single-call snapshot.  This
 * is the observability counterpart to AGENT2's upcoming
 * `zcl_explain_reject` lookup tool. */

/* Manual record helper — used by tests and the in-process observer. */
void metrics_prometheus_record_consensus_reject(const char *kind, const char *reason);

/* Query helpers (tests + zcl_consensus_report). */
uint64_t metrics_prometheus_consensus_rejects_total(void);
uint64_t metrics_prometheus_consensus_rejects_for_kind(const char *kind);
uint64_t metrics_prometheus_consensus_rejects_tracked_reasons(void);

/* JSON snapshot for `zcl_consensus_report`.
 *
 * Shape:
 *   {
 *     "totals": { "tx": N, "block": N, "all": N },
 *     "overflow": { "tx": N, "block": N },
 *     "tracked_reasons": N,
 *     "capacity": N,
 *     "by_reason": [
 *       { "kind": "tx|block", "reason": "...", "count": N }, ...
 *     ]
 *   }
 *
 * Returns bytes written (excluding NUL); silently truncates on
 * too-small buffers just like the other JSON snapshots. */
size_t metrics_prometheus_consensus_report_json(char *buf, size_t cap);

/* ── Node-level gauges ────────────────────────────────────────────
 *
 * These are updated periodically by the caller (e.g. the metrics
 * thread) and rendered in the Prometheus dump as:
 *
 *   zcl_block_height      <height>
 *   zcl_peer_count        <count>
 *   zcl_rss_mb            <mb>
 *   zcl_utxo_count        <count>
 *   zcl_sync_state        <state>
 *   zcl_uptime_seconds    <seconds>
 */

void metrics_prometheus_set_node_gauges(int64_t block_height, int64_t peer_count,
                                 double rss_mb, int64_t utxo_count,
                                 int64_t uptime_seconds);

/* Set the sync state gauge separately (name is a static string). */
void metrics_prometheus_set_sync_state(int state, const char *name);

/* Seconds since the most recent EV_BLOCK_CONNECTED (or -1 if the node
 * has not seen one yet). Pair with zcl_sync_state in PromQL to alert
 * on `tip_advance_age > 600 AND sync_state != at_tip`. Fed from
 * lib/metrics/src/metrics.c periodic tick via
 * sync_monitor_tip_advance_age(). */
void metrics_prometheus_set_tip_advance_age(int64_t seconds);

/* Mirror lag SLO gauges. Updated on each metrics tick from
 * legacy_mirror_sync_stats: lag_blocks is the live zclassicd-vs-local
 * height delta; breach_seconds and critical_seconds are the durations
 * of the current SLO breach episodes (0 when not breached). */
void metrics_prometheus_set_mirror_lag(int64_t lag_blocks,
                                int64_t breach_seconds,
                                int64_t critical_seconds);

/* Peer-kind gauges. Tracks "Magic Bean reporting" over time: how many
 * connected peers identify as zclassicd-era /MagicBean:.../ clients
 * and how many identify as native ZClassic23 (via NODE_ZCL23 service
 * bit or subver tag). Fed from node_health_snapshot peer iteration. */
void metrics_prometheus_set_peer_kinds(int64_t magicbean_count,
                                int64_t zcl23_count);

/* Header-height vs served-height (H*) gap, in blocks. Pass
 * `header_height - served_height`, or -1 when the header tip is not yet
 * known. Rendered as `zcl_header_gap_blocks`; internally accrues a
 * companion `zcl_header_gap_breach_seconds` hysteresis gauge (using the
 * node's own uptime counter as the clock basis, not wall-clock) once the
 * gap exceeds `ZCL_ALERT_HEADER_GAP_BLOCKS` (default 144) OUTSIDE
 * SYNC_HEADERS_DOWNLOAD — a large gap during initial header download is
 * the normal shape of IBD, not a stall. Must be called after
 * metrics_prometheus_set_node_gauges() (uptime) and metrics_prometheus_set_sync_state()
 * (sync state) in the same tick — see lib/metrics/src/metrics.c's
 * ordering. Feeds the `header_gap_growing` alert rule. */
void metrics_prometheus_set_header_gap(int64_t gap_blocks);

/* ── Metric-threshold alert rules (C3) ────────────────────────────
 *
 * A small declarative table of {gauge, comparator, threshold} rules
 * evaluated against the SAME in-process gauges the Prometheus dump
 * reads (no re-parsing of rendered text, no new metric enumeration).
 * On a rising edge (value crosses the threshold after being clear)
 * the rule fires immediately; while the value stays crossed it can
 * re-fire only after its cooldown elapses (mirrors the blocker
 * escape-dispatch rate-limit idea in util/blocker.h). Firing raises
 * EV_CONDITION_DETECTED (payload "name=metric_alert.<rule> severity=..
 * gauge=.. value=.. threshold=.."), the same generic event type the
 * condition-engine healers use, so it flows through the existing
 * mcp_notify push channel once "condition.detected" is allow-listed
 * there — no new event type, no new thread.
 *
 * Evaluated once per metrics tick from metrics_prometheus_set_peer_kinds()
 * (the last of the four gauge setters lib/metrics's 1-second thread
 * calls each tick), so it runs automatically whenever the node's
 * metrics thread is active (default: on, see `-showmetrics`). Also
 * callable directly — tests call it after seeding gauges via the
 * setters above to drive one evaluation pass deterministically. */
void metrics_prometheus_evaluate_alert_rules(void);

/* Introspection (tests). */
size_t metrics_prometheus_alert_rule_count(void);
uint64_t metrics_prometheus_alert_fire_count(const char *rule_event_name);

/* Reset per-rule edge/cooldown state and fire counters. Tests call
 * this to isolate; folded into metrics_prometheus_reset() for `zcl_metrics_reset`. */
void metrics_prometheus_alerts_reset(void);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_METRICS_PROMETHEUS_H */

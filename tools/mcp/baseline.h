/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP metrics baseline — labeled point-in-time snapshots of the
 * Prometheus metrics render (mcp/metrics.h), diffable against the
 * live snapshot so an operator can answer "what changed in the last
 * hour" in one call.
 *
 * Design: a baseline captures metrics_prometheus_render_prometheus()'s TEXT
 * output verbatim — that render is metrics.c's one existing full
 * enumeration of every counter/gauge/histogram it exports. Diffing
 * walks that text generically ("name{labels} value" lines, see
 * baseline.c) with zero per-metric knowledge, so every metric
 * render_prometheus emits today OR adds in the future is
 * automatically captured and diffable without touching this module.
 * The alternative (re-deriving a second, parallel JSON enumeration of
 * metrics.c's internal counter/histogram/reject-reason tables) would
 * duplicate metrics.c's own enumeration and silently drift the day a
 * new gauge is added there but not here — see the top comment in
 * baseline.c for the fuller rationale.
 *
 * Ring of MCP_BASELINE_RING_SIZE labeled snapshots, mirroring the
 * shape of replay.c (MCP-owned auxiliary state, fixed-size storage,
 * no allocation for the ring itself — capture and diff still malloc
 * scratch buffers, same as replay.c's dump path). Unlike replay.c
 * this ring is guarded by a mutex, matching metrics.c's g_lock
 * pattern: baselines can be set/diffed from both the stdio MCP path
 * and the HTTP RPC path (rpc_http_middleware also reaches
 * mcp_router_dispatch), so an unguarded ring would race the same way
 * metrics.c's counters would without g_lock.
 */

#ifndef ZCL_MCP_BASELINE_H
#define ZCL_MCP_BASELINE_H

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

#define MCP_BASELINE_RING_SIZE    16
#define MCP_BASELINE_LABEL_MAX    64

/* Per-slot captured text capacity. Matches the cap
 * tools/mcp/controllers/meta_controller.c's h_zcl_metrics uses for its
 * own Prometheus render, so a baseline never truncates anything the
 * live zcl_metrics tool wouldn't also truncate. 16 slots * 128 KiB =
 * 2 MiB static. */
#define MCP_BASELINE_SNAPSHOT_CAP 131072

/* Reset the ring to empty (idempotent). Tests call this between cases. */
void mcp_baseline_init(void);

/* Capture a new baseline labeled `label` (copied, truncated to
 * MCP_BASELINE_LABEL_MAX-1 bytes). If label is NULL or empty, an
 * auto-generated "b<seq>" label is assigned. The assigned label is
 * copied into out_label (best-effort truncated to out_label_cap);
 * pass NULL/0 to skip. out_timestamp_us, if non-NULL, receives the
 * capture wall-clock time in microseconds since epoch.
 *
 * Overwrites the oldest ring slot once MCP_BASELINE_RING_SIZE
 * baselines are live — that is normal ring operation, not a failure.
 * Returns false only if the metrics render came back empty (should
 * not happen in practice; the Prometheus render always emits its
 * fixed HELP/TYPE preamble). */
bool mcp_baseline_set(const char *label, char *out_label,
                      size_t out_label_cap, uint64_t *out_timestamp_us);

/* Number of live baselines, 0..MCP_BASELINE_RING_SIZE. */
size_t mcp_baseline_count(void);

/* Find the ring slot for the most recent baseline with this exact
 * label (a re-used label resolves to the newest match — ring entries
 * are never deduplicated by label, matching replay.c's append-only
 * shape). Returns a slot index usable by mcp_baseline_diff_json(), or
 * -1 if no live baseline has that label. */
int mcp_baseline_find(const char *label);

/* Find the most recent baseline captured at or before since_us
 * (microseconds since epoch), or -1 if the ring is empty or every
 * live baseline postdates since_us. */
int mcp_baseline_find_nearest_before(uint64_t since_us);

/* Slot index of the most recently captured baseline, or -1 if none. */
int mcp_baseline_latest(void);

/* JSON object describing every live baseline, oldest to newest:
 *   {"count":N,"ring_size":16,"baselines":[
 *     {"label":..,"timestamp_us":N,"age_seconds":N,"bytes":N}, ...
 *   ]}
 * Malloc'd, caller frees. NULL only on OOM (logged). */
char *mcp_baseline_list_json(void);

/* Diff the baseline at ring slot `idx` (from mcp_baseline_find /
 * _find_nearest_before / _latest — the caller is responsible for
 * range/validity checks on the lookup result before calling this)
 * against a freshly rendered live metrics snapshot. Only numeric
 * leaves whose value changed are included. Shape:
 *   {"label":..,"baseline_timestamp_us":N,"age_seconds":N,
 *    "changed_count":N,"changed":[
 *      {"metric":"zcl_block_height","from":100,"to":142,"delta":42}, ...
 *   ]}
 * Malloc'd, caller frees. NULL if idx is out of range, the slot is
 * empty, or an allocation failed (logged either way). */
char *mcp_baseline_diff_json(int idx);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_MCP_BASELINE_H */

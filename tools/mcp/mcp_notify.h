/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * MCP event push out-channel.
 *
 * The node emits structured EV_* events (lib/event) into a lock-free
 * ring. Until now an operator had to POLL them (zcl_events -> eventlog
 * RPC). This out-channel inverts that: it converts new operator-class
 * events into MCP `notifications/message` JSON-RPC frames PUSHED to the
 * connected agent on the stdio channel, so a chain halt / SLO breach /
 * operator-needed condition surfaces without anyone asking.
 *
 * Source seam (read this before extending):
 *   The -mcp process is a separate proxy (Claude <-stdio-> proxy
 *   <-HTTP/RPC-> node). It does NOT share the node's in-process event
 *   ring, so it cannot register an event_observe() callback. Its only
 *   window onto the live event stream is the `eventlog` RPC, which
 *   returns the last-N ring entries each tagged with a monotonic `seq`.
 *   So the source today is a tight background poll keyed on `seq`: each
 *   poll forwards only entries newer than the high-water mark, so no
 *   event is duplicated or fabricated — every notification corresponds
 *   to one real ring entry the node published.
 *
 *   When the MCP loop is hosted in-process on a node thread (exec-plan
 *   4.1), the poll source is replaced by a direct event_observe()/async
 *   subscription that feeds mcp_notify_consider_event() with zero
 *   latency. The emitter (mcp_notify_emit / the wire sink) and the
 *   operator-class filter stay unchanged — only fetch_fn is swapped.
 *   That is the marked 4.1 dependency: it removes the poll, not the
 *   channel.
 */

#ifndef ZCL_MCP_NOTIFY_H
#define ZCL_MCP_NOTIFY_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Sink for a fully-formed JSON-RPC notification line (no trailing
 * newline). In the live server this writes to stdout+flush; tests
 * install a capture sink. Must be safe to call from the notifier
 * thread. */
typedef void (*mcp_notify_sink_fn)(const char *json_line, void *ctx);

/* Source for the latest eventlog snapshot. Returns a malloc'd JSON
 * body of the shape `{"events":[{"seq":N,"type":"...","data":"..."},
 * ...]}` (the eventlog RPC result), or NULL on failure; caller frees.
 * In the live server this is the eventlog RPC over HTTP. Tests inject
 * a deterministic source. This is the 4.1 swap point. */
typedef char *(*mcp_notify_fetch_fn)(void *ctx);

/* True if `event_type` (an event_type_name string, e.g.
 * "oracle.chain_halted") is in the operator-class push allow-list.
 * Exposed for the test and for the registration self-check. */
bool mcp_notify_is_operator_event(const char *event_type);

/* Build the MCP notifications/message frame for one event into `out`.
 * Returns the number of bytes written (excludes the NUL), 0 on overflow
 * or bad input. Pure / no I/O — directly unit-testable. The frame is a
 * JSON-RPC 2.0 notification (no id): method "notifications/message",
 * params carry MCP logging fields (level, logger, data{type,seq,...}). */
size_t mcp_notify_build_frame(char *out, size_t out_sz,
                              const char *event_type, int64_t seq,
                              const char *data, int64_t ts_us, uint32_t peer);

/* Consider one eventlog snapshot body: parse events[], and for each
 * entry with seq > the internal high-water mark AND an operator-class
 * type, build a frame and hand it to `sink`. Advances the high-water
 * mark past every entry seen (operator-class or not) so non-pushed
 * events still move the watermark and are never re-examined. Returns
 * the number of notifications emitted. Reentrant on its own state via
 * the module mutex. */
int mcp_notify_consider_snapshot(const char *eventlog_body,
                                 mcp_notify_sink_fn sink, void *sink_ctx);

/* Reset the module's high-water mark and counters. Tests call this to
 * isolate; the live server calls it once at start. If `prime_to` >= 0,
 * the high-water mark is set so that only seq > prime_to is pushed
 * (used on connect to avoid replaying historical events). */
void mcp_notify_reset(int64_t prime_to);

/* Start the background notifier thread. `fetch` pulls eventlog
 * snapshots; `sink` writes notification frames. poll_ms is the poll
 * interval. Returns false if already running or thread spawn failed.
 * On the first successful fetch the watermark is primed to the newest
 * seq so the agent is not flooded with pre-connection history. */
bool mcp_notify_start(mcp_notify_fetch_fn fetch, void *fetch_ctx,
                      mcp_notify_sink_fn sink, void *sink_ctx,
                      unsigned poll_ms);

/* Signal the notifier thread to stop and join it. Idempotent. */
void mcp_notify_stop(void);

/* Total notifications emitted since the last reset (diagnostics/tests). */
uint64_t mcp_notify_total_emitted(void);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_MCP_NOTIFY_H */

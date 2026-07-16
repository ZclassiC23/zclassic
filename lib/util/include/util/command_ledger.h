/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Command interaction ledger — the "agent flight recorder" (Phase D).
 *
 * Every dispatch through the kernel command registry
 * (zcl_command_registry_execute_json) can be appended, CONTENT-FREE, to a
 * durable NDJSON ledger at <datadir>/telemetry/command_ledger.ndjson. Records
 * are the zcl.cmd_ledger.v1 schema: byte COUNTS and typed metadata only, never
 * the input or output content. This lets an agent (or an operator) self-review
 * its own command usage — call rates, error rates, latency p50/p99, output
 * sizes, budget overruns — over its own history.
 *
 * DESIGN INVARIANTS
 *  - Each record renders to well under PIPE_BUF (4096 B), so concurrent
 *    fresh-process CLI appends (O_APPEND) are kernel-atomic; the hot append
 *    path takes NO lock.
 *  - The file is capped (default 8 MiB) and rotated to `.ndjson.1` under an
 *    advisory flock — one retained generation (a ~16 MiB ceiling). When a
 *    second rotation drops the previous `.1`, a durable retention-gap marker is
 *    set and every query surfaces retention_gap=true.
 *  - THREADING: the sink appends SYNCHRONOUSLY from the calling (dispatching)
 *    thread. There is NO background thread and NO supervisor contract — nothing
 *    to register with the liveness tree, nothing to tick. Install once, and the
 *    kernel calls the sink inline per dispatch.
 */

#ifndef ZCL_UTIL_COMMAND_LEDGER_H
#define ZCL_UTIL_COMMAND_LEDGER_H

#include "kernel/command_registry.h" /* struct zcl_command_ledger_record */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

struct json_value;

/* Open/create <datadir>/telemetry/ and the ledger fd, then register the sink
 * (zcl_command_registry_set_ledger_sink) and the durable p99 latency source
 * (zcl_command_registry_set_latency_source). Idempotent: a second call closes
 * the prior fds and re-points at `datadir`. Returns false (and registers
 * nothing) on a null/empty datadir or an unopenable telemetry dir/fd. */
bool command_ledger_install(const char *datadir);

/* Unregister the sink + latency source and close the fds. Symmetric with
 * install; safe to call when not installed. */
void command_ledger_uninstall(void);

/* The registered sink: durably append one content-free NDJSON record and emit
 * one `command_dispatch` structured log line. No-op if not installed. */
void command_ledger_sink(const struct zcl_command_ledger_record *record);

/* p99 dispatch latency (us) for `leaf` over the trailing `window_s` seconds
 * (<=0 == all retained), from a bounded reverse scan of the current + `.1`
 * files. Returns true with the p99_us + samples out-params set when >=1 sample
 * matched, else false with samples==0. Matches zcl_command_latency_source_fn. */
bool command_ledger_p99(const char *leaf, int64_t window_s,
                        int64_t *p99_us, uint32_t *samples);

/* Per-leaf usage summary over the trailing `window_s` seconds (<=0 == all),
 * optionally filtered to `leaf_filter` (NULL/"" == every leaf), emitting the
 * top `top` (clamped to [1,50]) leaves by call count into the caller-init'd
 * object `out`: per leaf {calls,error_rate,p50_us,p99_us,avg_output_bytes,
 * budget_exceeded_count}, plus retention_gap and the window actually covered. */
bool command_ledger_summary(int64_t window_s, const char *leaf_filter,
                            int top, struct json_value *out);

/* The most recent `n` (clamped to [1,200]) records, optionally filtered to
 * `leaf_filter`, newest first, into the caller-init'd object `out`. */
bool command_ledger_tail(int n, const char *leaf_filter,
                         struct json_value *out);

/* dumpstate introspection (see CLAUDE.md "Adding state introspection").
 * Reentrant-safe; caller runs json_set_object(out) first. */
bool command_ledger_dump_state_json(struct json_value *out, const char *key);

/* TEST-ONLY: override the rotation cap (bytes) so a rotation test does not have
 * to write the full 8 MiB. Pass <=0 to restore the default. Not for production
 * callers. */
void command_ledger_test_set_cap(int64_t max_bytes);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_UTIL_COMMAND_LEDGER_H */

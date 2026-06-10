/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Header Probe — pull headers in bulk from a co-located
 * zclassicd (the legacy C++ ZClassic node, RPC 8232) via JSON-RPC
 * when our local header tip lags behind. Validates each fetched
 * header locally via accept_block_header() (Equihash + nBits
 * lineage + checkpoints) and inserts into our block_index.
 *
 * Why this exists:
 *   When P2P header sync stalls or is the bottleneck, the co-located
 *   zclassicd has all the headers and can serve them in single-digit
 *   milliseconds per call. This service closes the gap in seconds
 *   instead of hours.
 *
 * Architecture:
 *   - Pull loop calls getblockhash(h) → getblockheader(hash, false)
 *     for height range [start, start+max] on the local zclassicd.
 *   - Each returned hex-serialized header is parsed via
 *     block_header_deserialize and handed to accept_block_header().
 *   - A supervised header_probe_poll Job calls header_probe_tick_once()
 *     whenever our header tip trails the remote tip by more than
 *     `lag_threshold`.
 *   - No new pthreads. The supervised header_probe_poll Job owns cadence.
 *
 * See CLAUDE.md "Adding state introspection" — this module follows
 * the *_dump_state_json convention and is wired into the generic
 * zcl_state dispatcher.
 */

#ifndef ZCL_SERVICES_HEADER_PROBE_H
#define ZCL_SERVICES_HEADER_PROBE_H

#include <stdbool.h>
#include <stdint.h>

#include "util/result.h"

struct main_state;
struct chain_params;
struct json_value;

struct header_probe_config {
    const char *rpc_host;       /* default "127.0.0.1" */
    int         rpc_port;       /* default 8232 */
    const char *rpc_user;       /* read zclassic.conf if NULL */
    const char *rpc_password;
    int         batch_size;     /* default 2000; max 5000 */
    int         lag_threshold;  /* only probe when our_tip < their_tip - this; default 100 */
};

/* Apply config + load credentials. Safe to call before start to
 * override defaults. Idempotent. Returns a non-ok zcl_result only on a
 * missing zclassic.conf when no user/password were supplied. */
struct zcl_result header_probe_init(const struct header_probe_config *cfg,
                                    struct main_state *ms,
                                    const struct chain_params *params);

/* One-shot poll tick. Cheap getblockcount discovers the remote tip,
 * compares it against the local header tip, and pulls a batch if the
 * lag threshold is exceeded. Safe to call when not initialized (no-op).
 *
 * Used by the `header_probe_poll` Job (app/jobs/) as the body of
 * its supervisor tick callback. Pure scheduling separation — same
 * RPC, same validation, same accept_block_header path. */
void header_probe_tick_once(void);

/* Synchronous one-shot for the MCP tool + tests:
 *   start_height: where to begin (inclusive). Use our_tip+1 normally.
 *   max_headers:  cap on number of headers to pull in this call
 *                 (clamped to [1, 5000]).
 *   out_added:    receives the number successfully accepted (NULL OK).
 * Returns ZCL_OK if at least one header was added, we're already at
 * tip, or the legacy node is unreachable (RPC errors are surfaced via
 * state counters, not the return). Returns a non-ok zcl_result only if
 * init() hasn't been called or arguments are bogus. */
struct zcl_result header_probe_pull_range(int start_height, int max_headers,
                                          int *out_added);

/* zcl_state subsystem=header_probe dispatcher entry. See CLAUDE.md
 * "Adding state introspection". Reentrant-safe. */
bool header_probe_dump_state_json(struct json_value *out, const char *key);

/* Test hooks — reset state between unit tests. */
void header_probe_reset_for_test(void);

#endif /* ZCL_SERVICES_HEADER_PROBE_H */

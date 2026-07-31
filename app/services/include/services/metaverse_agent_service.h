/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * metaverse_agent_service — the read side of the confined-agent broker, for
 * the `metaverse agent status` / `metaverse agent audit` leaves.
 *
 * The broker runs as its own process (`zclassic23 --metaverse-broker`), so its
 * state is a DIRECTORY, not in-process memory. These two calls are therefore
 * pure readers over that directory: they open nothing outside it, create
 * nothing, and answer "not present" for a directory that has never hosted a
 * broker. That is deliberate — a read leaf that mkdir'd its own subject would
 * make "no broker has run here" indistinguishable from "a broker ran and left
 * nothing", and would write into whatever datadir a caller named.
 *
 * Both calls return struct zcl_result. The refusal REASON travels with the
 * refusal: `code` is one of the MVS_ERR_* values below, so the controller can
 * map it onto a named command error, and `message` carries the detail an
 * operator needs to fix the call.
 */

#ifndef ZCL_SERVICES_METAVERSE_AGENT_SERVICE_H
#define ZCL_SERVICES_METAVERSE_AGENT_SERVICE_H

#include "base/result.h"

#include <stddef.h>

/* Refusal codes carried in struct zcl_result::code. */
enum {
    MVS_ERR_BAD_ARGS = -1,      /* dir NULL/empty, too long, or not absolute */
    MVS_ERR_NOT_A_DIR = -2,     /* dir does not exist, or is not a directory */
    MVS_ERR_RENDER_FAILED = -3, /* the document did not fit `out_cap`        */
};

/* Render the broker's recorded state for `dir` into `out`, writing the byte
 * length to `*out_len` on success. */
struct zcl_result metaverse_agent_service_status(const char *dir, char *out,
                                                 size_t out_cap,
                                                 size_t *out_len);

/* Render the verified audit trail for `dir` into `out`, tail-bounded by
 * `limit` (0 selects the default). The tamper verdict always covers the whole
 * log even when the rendered tail is shorter. */
struct zcl_result metaverse_agent_service_audit(const char *dir, size_t limit,
                                                char *out, size_t out_cap,
                                                size_t *out_len);

#endif /* ZCL_SERVICES_METAVERSE_AGENT_SERVICE_H */

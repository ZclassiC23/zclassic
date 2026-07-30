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
 * `dir` is validated by the caller (the controller) before it reaches here.
 */

#ifndef ZCL_SERVICES_METAVERSE_AGENT_SERVICE_H
#define ZCL_SERVICES_METAVERSE_AGENT_SERVICE_H

#include <stddef.h>

/* Refusal tokens written to `why` (the controller maps them onto named
 * command errors; the test asserts them exactly):
 *   BAD_ARGS      — dir is NULL/empty, too long, or not an absolute path
 *   NOT_A_DIR     — dir does not exist or is not a directory
 *   RENDER_FAILED — the document did not fit the caller's buffer          */

/* Render the broker's recorded state for `dir` into `out`. Returns the byte
 * length written, or 0 with a token in `why`. */
size_t metaverse_agent_service_status(const char *dir, char *out,
                                      size_t out_cap, char *why,
                                      size_t why_cap);

/* Render the verified audit trail for `dir` into `out`, tail-bounded by
 * `limit` (0 selects the default). Returns the byte length written, or 0 with
 * a token in `why`. The tamper verdict always covers the whole log even when
 * the rendered tail is shorter. */
size_t metaverse_agent_service_audit(const char *dir, size_t limit, char *out,
                                     size_t out_cap, char *why,
                                     size_t why_cap);

#endif /* ZCL_SERVICES_METAVERSE_AGENT_SERVICE_H */

/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ROM-fetch peer scoring — see net/rom_peer_scoring.h.
 *
 * STEP-0 STATUS: contracts-commit stub. Validates its arguments and logs the
 * offence at WARN, but does not yet wire the p2p_node offence route or the
 * bounded local deprioritize list — lane 2C (wf/artifact-protocol) lands that.
 * No caller invokes this yet. */

#include "net/rom_peer_scoring.h"

#include "util/log_macros.h"

bool rom_peer_note_bad_chunk(const char *peer_addr, uint16_t port,
                             uint32_t idx, const char *reason)
{
    if (!peer_addr || !peer_addr[0])
        LOG_FAIL("rom_peer_scoring", "NULL/empty peer_addr");
    LOG_WARN("rom_peer_scoring",
             "bad ROM chunk from %s:%u idx=%u reason=%s (scoring not yet wired)",
             peer_addr, (unsigned)port, idx, reason ? reason : "?");
    /* Not yet routed to a real scoreboard (lane 2C). Report "not recorded"
     * so a caller can fall back to plain round-robin retry. */
    return false;
}

/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ROM-fetch peer scoring — records a bad-chunk offence against the seeder that
 * served a chunk whose per-chunk SHA3 digest or transport MAC failed.
 *
 * Routing (lane 2C, wf/artifact-protocol): when the offending endpoint maps to
 * a connected `p2p_node`, the offence is charged through the shared
 * peer_scoring_record() ban logic; otherwise it lands in a small bounded
 * local deprioritize list (same shape as the snapsync blacklist[16]) so a
 * misbehaving ROM-only seeder is skipped on the next round without touching
 * the P2P ban table.
 *
 * A bad chunk is never poisonous (the content proof rejects it before use);
 * this scoring only stops us from wasting bandwidth on a peer that keeps
 * serving garbage. STEP-0 STATUS: contract + stub. */

#ifndef ZCL_NET_ROM_PEER_SCORING_H
#define ZCL_NET_ROM_PEER_SCORING_H

#include <stdbool.h>
#include <stdint.h>

/* Record that `peer_addr:port` served a bad chunk (`idx`) for `reason`
 * (a short static string, e.g. "digest" / "mac" / "short"). Returns true if
 * the offence was recorded (either as a p2p_node offence or into the local
 * deprioritize list). NULL/empty `peer_addr` returns false. */
bool rom_peer_note_bad_chunk(const char *peer_addr, uint16_t port,
                             uint32_t idx, const char *reason);

#endif /* ZCL_NET_ROM_PEER_SCORING_H */

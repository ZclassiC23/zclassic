/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ZENDP — the SIGNED ENDPOINT RECORD body of the sovereign identity
 * layer. A node's address stops being a bare string anyone can assert
 * and becomes a document the node SIGNED, with a key the chain vouches
 * for (docs/work/NAT_AND_ONION_TRANSPORT.md, "Directory records should
 * eventually be SIGNED by the peer key").
 *
 * The record is a zid_doc BODY with its own 4-byte tag "ZIDE", exactly
 * as the descriptor uses "ZIDD" and the release record uses "ZIDR". The
 * doc around it (version, master_pubkey, seq, expiry, signature) is
 * unchanged — the identity layer already answers "who signed this, is
 * it fresh, is it newer than what I hold". This file answers only "what
 * does the body say", and it answers it purely: no store, no network,
 * no chain, no allocation, no clock of its own.
 *
 * Frozen body wire (a WIRE CONTRACT from the first publish):
 *
 *   "ZIDE"          4
 *   flags           1    ZENDP_HAS_* bitmask; unknown bits REJECT
 *   services        8    LE service bitmask (the P2P nServices vocabulary)
 *   height          4    LE best height the publisher claims
 *   not_before      8    LE unix seconds; validity OPENS here
 *   onion           62   present iff ZENDP_HAS_ONION
 *   onion_port      2    LE, present iff ZENDP_HAS_ONION
 *   ipv4            4    present iff ZENDP_HAS_IPV4
 *   ipv4_port       2    LE, present iff ZENDP_HAS_IPV4
 *   ipv6            16   present iff ZENDP_HAS_IPV6
 *   ipv6_port       2    LE, present iff ZENDP_HAS_IPV6
 *
 * WHY NOT_AFTER IS ABSENT. The record's expiry is zid_doc.expiry, which
 * is already signed. Carrying it twice would be two writable copies of
 * one fact — the same reason zdesc carries not_before only.
 *
 * WHY EVERY TRANSPORT IS OPTIONAL, AND WHY "NONE" IS NOT. Clearnet-only
 * nodes, onion-only nodes, and dual nodes are all first-class, so each
 * block is flag-gated. A record with flags == 0 names no way to reach
 * anything, so it is refused by name at BOTH ends rather than stored as
 * a fact that cannot be acted on.
 *
 * CANONICAL BY CONSTRUCTION. A field whose flag is clear MUST be zero in
 * the struct, so one endpoint set has exactly one encoding and one
 * digest. Encode refuses a struct that violates this rather than
 * silently dropping the bytes; decode memsets first, so a decoded record
 * always satisfies it.
 *
 * SIZE. ZENDP_BODY_MAX is 113, so the signed doc is at most
 * 51 + 113 + 64 = 228 bytes — past the 223-byte OP_RETURN relay cap that
 * lib/zid already pins (ZID_ANCHOR_RELAY_MAX), and far under one swarm
 * chunk. Endpoint records are therefore CONTENT (vcs/zendp_swarm.h
 * carries them as blobs over the frozen 'zpkgswm' codec); the chain
 * carries only the identity anchor. Rotation is a new seq and a new
 * signature: no transaction, no fee.
 *
 * WHAT THIS FILE CANNOT ANSWER. Whether the signing key is anchored
 * on-chain. zendp_verify checks a signature against the key the doc
 * CARRIES; binding that key to the chain needs the zid_identities
 * projection, which lives far above lib/zid (rank 10). That check is
 * vcs/zendp_swarm.h's job and it is mandatory there. */

#ifndef ZCL_ZID_ZENDP_H
#define ZCL_ZID_ZENDP_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zid/zdesc.h"

struct zid_doc;

/* The onion hostname rule and the PERIOD CONTRACT are defined once, in
 * zid/zdesc.h, and shared here rather than copied: a second calendar or
 * a third copy of the v3 hostname rule would be two writable copies of
 * one fact. Endpoint records use zdesc_onion_valid(),
 * zdesc_period_at() and zdesc_period_prev() unchanged. */
#define ZENDP_ONION_LEN ZDESC_ONION_LEN

#define ZENDP_HAS_ONION 0x01u
#define ZENDP_HAS_IPV4  0x02u
#define ZENDP_HAS_IPV6  0x04u
#define ZENDP_FLAGS_KNOWN (ZENDP_HAS_ONION | ZENDP_HAS_IPV4 | ZENDP_HAS_IPV6)

#define ZENDP_BODY_MIN   (4 + 1 + 8 + 4 + 8)          /* 25 */
#define ZENDP_ONION_WIRE (ZENDP_ONION_LEN + 2)        /* 64 */
#define ZENDP_IPV4_WIRE  (4 + 2)                      /* 6  */
#define ZENDP_IPV6_WIRE  (16 + 2)                     /* 18 */
#define ZENDP_BODY_MAX \
    (ZENDP_BODY_MIN + ZENDP_ONION_WIRE + ZENDP_IPV4_WIRE + ZENDP_IPV6_WIRE)

struct zendp {
    uint8_t  flags;
    uint64_t services;
    uint32_t height;
    uint64_t not_before;    /* unix seconds; record invalid before */
    char     onion[ZENDP_ONION_LEN + 1];
    uint16_t onion_port;
    uint8_t  ipv4[4];
    uint16_t ipv4_port;
    uint8_t  ipv6[16];
    uint16_t ipv6_port;
};

/* The shape rule, in ONE place, used by encode and re-run after decode:
 *   - no unknown flag bits;
 *   - at least one endpoint (flags != 0);
 *   - a claimed onion is a real v3 hostname with a non-zero port;
 *   - a claimed IP is non-zero (0.0.0.0 / :: reach nobody) with a
 *     non-zero port;
 *   - every field whose flag is clear is zero (canonical encoding).
 * A record that fails is REJECTED, never sanitized. */
bool zendp_valid(const struct zendp *ep);

/* Exact body length for a flags byte, or 0 when flags carry an unknown
 * bit or no endpoint at all. */
size_t zendp_body_len(uint8_t flags);

/* Encode the body. Returns the encoded size, or 0 on error (NULL args,
 * a record that fails zendp_valid, an undersized buffer). */
size_t zendp_encode_body(uint8_t *out, size_t out_len, const struct zendp *ep);

/* Bounds-strict decode: exact "ZIDE" tag, a length that matches the
 * declared flags EXACTLY (no trailing bytes), and the full shape rule
 * re-run on the decoded struct. */
bool zendp_decode_body(struct zendp *ep, const uint8_t *body,
                       uint16_t body_len);

/* Encode ep as the body and zid_doc_sign it. Refuses expiry <=
 * ep->not_before (a window that never opens is a publisher bug, not a
 * verifier's problem). */
bool zendp_sign(struct zid_doc *doc, const struct zendp *ep, uint64_t seq,
                uint64_t expiry, const uint8_t seed[32]);

/* zid_doc_verify (signature + version + expiry) against now_unix, then
 * decode the ZIDE body, then enforce now_unix >= not_before. ep_out may
 * be NULL to verify without decoding.
 *
 * The key checked is doc->master_pubkey — whatever the doc CARRIES.
 * Whether that key is anchored on-chain is NOT asked here; see
 * vcs/zendp_swarm.h, where it is mandatory. */
bool zendp_verify(const struct zid_doc *doc, struct zendp *ep_out,
                  uint64_t now_unix);

/* The record key an endpoint record is addressed by:
 *   SHA3-256("ZIDE" ‖ zid_blinded_key(master_pubkey, period))
 *
 * The blinded key is the anti-enumeration address (only someone who
 * already knows the master key can derive it); the extra "ZIDE" step
 * gives endpoint records their OWN address space, so a descriptor and
 * an endpoint record for one identity can never collide at one address
 * if a future carrier ever distributes record_key -> root mappings
 * globally. Frozen by golden vector in lib/test/src/test_zendp.c. */
void zendp_record_key(uint8_t out[32], const uint8_t master_pubkey[32],
                      uint64_t period);

#endif /* ZCL_ZID_ZENDP_H */

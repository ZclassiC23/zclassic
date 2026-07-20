/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Sync trust → capability policy: the SINGLE source of truth mapping the
 * node's self-derivation posture to the set of operations it may perform
 * (serve a validated tip, receive/spend in the wallet, mine, export a state
 * bundle, seed a bundle). Today the same decision is scattered across
 * sovereignty_guard_allow, bx_qualified, boot_snapshot_offer and export_proof,
 * keyed on two ORTHOGONAL provenance bits that genuinely diverge:
 *
 *   S = self_derived                       (mint / spend / offer authority)
 *   X = proven_authority && refold_marker  (export authority)
 *
 * This table derives one `sync_trust_state` from (proven, refold, self_derived)
 * and maps it to a `sync_capability` bitmask, so the provenance-bit portion of
 * every gate resolves in exactly one place. It replaces ONLY that portion —
 * every other local AND a call site applies (cursors, build_commit,
 * coins_ram_active, payload authority, and the SNAPSYNC_ACTIVATION_CONTAINED
 * door) stays exactly where it is and is never weakened.
 *
 * Pure/deterministic: no clock, RNG, or IO. Mirrors app/services/authz_policy.h. */

#ifndef ZCL_SERVICES_SYNC_TRUST_POLICY_H
#define ZCL_SERVICES_SYNC_TRUST_POLICY_H

#include <stdbool.h>
#include <stdint.h>

/* Ordered least → most trusted. PEER_ASSISTED_READY is RESERVED — the derive
 * function does not yet return it this slice (peer-provided state is not a
 * distinct readiness tier until the artifact-protocol lands). */
enum sync_trust_state {
    SYNC_TRUST_EMPTY = 0,             /* no usable state */
    SYNC_TRUST_HEADERS_VERIFIED,      /* ¬S ∧ X: export authority only */
    SYNC_TRUST_ARTIFACT_VERIFIED,     /* S ∧ ¬X: self-derived, no export */
    SYNC_TRUST_RELEASE_ASSISTED_READY,/* ¬S ∧ ¬X ∧ proven: assisted operational */
    SYNC_TRUST_PEER_ASSISTED_READY,   /* reserved (not derived this slice) */
    SYNC_TRUST_SOVEREIGN,             /* S ∧ X: full authority */
    SYNC_TRUST_STATE_COUNT
};

/* Capability bitmask. Each bit is one operator-visible action. */
enum sync_capability {
    SYNC_CAP_NONE               = 0,
    SYNC_CAP_SERVE_VALIDATED_TIP = 1u << 0,
    SYNC_CAP_WALLET_RECEIVE      = 1u << 1,
    SYNC_CAP_WALLET_SPEND        = 1u << 2,
    SYNC_CAP_MINE                = 1u << 3,
    SYNC_CAP_EXPORT_BUNDLE       = 1u << 4,
    SYNC_CAP_SEED_BUNDLE         = 1u << 5,
};

/* All six capabilities — the SOVEREIGN mask. */
#define SYNC_CAP_ALL (SYNC_CAP_SERVE_VALIDATED_TIP | SYNC_CAP_WALLET_RECEIVE | \
                      SYNC_CAP_WALLET_SPEND | SYNC_CAP_MINE |                   \
                      SYNC_CAP_EXPORT_BUNDLE | SYNC_CAP_SEED_BUNDLE)

/* Derive the trust state from the two orthogonal provenance bits. `proven` is
 * proven_authority; `refold` is the refold_marker; `self_derived` is S.
 * X = proven && refold. Total: every combination maps to a defined state.
 * Fails closed to SYNC_TRUST_EMPTY. */
enum sync_trust_state sync_trust_derive(bool proven, bool refold,
                                        bool self_derived);

/* The capability mask granted by `st`. Unknown states fail closed to
 * SYNC_CAP_NONE. */
uint32_t sync_trust_caps(enum sync_trust_state st);

/* True iff `st` grants `cap` (a single SYNC_CAP_* bit). */
bool sync_trust_cap_allowed(enum sync_trust_state st, enum sync_capability cap);

/* Stable lowercase state name; out-of-range → "?". */
const char *sync_trust_state_name(enum sync_trust_state st);

#endif /* ZCL_SERVICES_SYNC_TRUST_POLICY_H */

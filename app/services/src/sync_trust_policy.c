/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Sync trust → capability policy table (single source of truth). See
 * services/sync_trust_policy.h.
 *
 * one-result-type-ok:pure-total-policy-lookup — these are total, infallible
 * table lookups (provenance bits -> trust state -> capability mask) that cannot
 * fail; a zcl_result would be noise. Unknown inputs fail closed to the
 * least-privilege value (EMPTY / CAP_NONE).
 *
 * STEP-0 STATUS: the derivation matrix + the capability table are REAL and
 * final; lanes 3B/3C/3D route the existing gate sites through them. */

// one-result-type-ok:pure-total-policy-lookup — total infallible provenance->
// state->capability lookups; a zcl_result would be noise (fail closed).
#include "services/sync_trust_policy.h"

enum sync_trust_state sync_trust_derive(bool proven, bool refold,
                                        bool self_derived)
{
    const bool S = self_derived;
    const bool X = proven && refold;   /* export authority */

    if (S && X)
        return SYNC_TRUST_SOVEREIGN;            /* full authority */
    if (S && !X)
        return SYNC_TRUST_ARTIFACT_VERIFIED;    /* self-derived, no export */
    if (!S && X)
        return SYNC_TRUST_HEADERS_VERIFIED;     /* export only */
    if (!S && !X && proven)
        return SYNC_TRUST_RELEASE_ASSISTED_READY;
    return SYNC_TRUST_EMPTY;                     /* fail closed */
}

uint32_t sync_trust_caps(enum sync_trust_state st)
{
    switch (st) {
    case SYNC_TRUST_EMPTY:
        return SYNC_CAP_NONE;
    case SYNC_TRUST_HEADERS_VERIFIED:
        /* ¬S ∧ X: export authority only. */
        return SYNC_CAP_EXPORT_BUNDLE;
    case SYNC_TRUST_ARTIFACT_VERIFIED:
        /* S ∧ ¬X: self-derived operational — everything but EXPORT. */
        return SYNC_CAP_SERVE_VALIDATED_TIP | SYNC_CAP_WALLET_RECEIVE |
               SYNC_CAP_WALLET_SPEND | SYNC_CAP_MINE | SYNC_CAP_SEED_BUNDLE;
    case SYNC_TRUST_RELEASE_ASSISTED_READY:
        /* Proven but not self-derived: assisted readiness only — serve a
         * validated tip and receive; never mine/spend/export/seed. */
        return SYNC_CAP_SERVE_VALIDATED_TIP | SYNC_CAP_WALLET_RECEIVE;
    case SYNC_TRUST_PEER_ASSISTED_READY:
        /* Reserved: same assisted shape as release-assisted; NEVER mine/seed. */
        return SYNC_CAP_SERVE_VALIDATED_TIP | SYNC_CAP_WALLET_RECEIVE;
    case SYNC_TRUST_SOVEREIGN:
        return SYNC_CAP_ALL;
    case SYNC_TRUST_STATE_COUNT:
        break;
    }
    return SYNC_CAP_NONE; /* fail closed on an unknown state */
}

bool sync_trust_cap_allowed(enum sync_trust_state st, enum sync_capability cap)
{
    return (sync_trust_caps(st) & (uint32_t)cap) != 0u;
}

const char *sync_trust_state_name(enum sync_trust_state st)
{
    switch (st) {
    case SYNC_TRUST_EMPTY:                return "empty";
    case SYNC_TRUST_HEADERS_VERIFIED:     return "headers_verified";
    case SYNC_TRUST_ARTIFACT_VERIFIED:    return "artifact_verified";
    case SYNC_TRUST_RELEASE_ASSISTED_READY: return "release_assisted_ready";
    case SYNC_TRUST_PEER_ASSISTED_READY:  return "peer_assisted_ready";
    case SYNC_TRUST_SOVEREIGN:            return "sovereign";
    case SYNC_TRUST_STATE_COUNT:          break;
    }
    return "?";
}

/* ── Compile-time bans on impossible capability combinations ─────────── */

/* The per-state masks as compile-time constants (kept byte-identical to the
 * sync_trust_caps() switch arms above) so the invariants below are proven at
 * compile time, not just tested. */
#define ZCL_TRUST_MASK_ARTIFACT                                          \
    (SYNC_CAP_SERVE_VALIDATED_TIP | SYNC_CAP_WALLET_RECEIVE |            \
     SYNC_CAP_WALLET_SPEND | SYNC_CAP_MINE | SYNC_CAP_SEED_BUNDLE)
#define ZCL_TRUST_MASK_ASSISTED                                         \
    (SYNC_CAP_SERVE_VALIDATED_TIP | SYNC_CAP_WALLET_RECEIVE)

/* EMPTY grants nothing; SOVEREIGN uniquely holds the full mask. */
_Static_assert(SYNC_CAP_NONE == 0u, "SYNC_CAP_NONE must be 0");
/* ARTIFACT_VERIFIED must be a strict subset of SOVEREIGN. */
_Static_assert((ZCL_TRUST_MASK_ARTIFACT & ~(unsigned)SYNC_CAP_ALL) == 0u,
               "artifact_verified caps must be a subset of sovereign");
/* MINE ⇒ WALLET_SPEND wherever MINE appears. */
_Static_assert((ZCL_TRUST_MASK_ARTIFACT & SYNC_CAP_MINE) == 0u ||
                   (ZCL_TRUST_MASK_ARTIFACT & SYNC_CAP_WALLET_SPEND) != 0u,
               "MINE must imply WALLET_SPEND (artifact_verified)");
_Static_assert((SYNC_CAP_ALL & SYNC_CAP_MINE) == 0u ||
                   (SYNC_CAP_ALL & SYNC_CAP_WALLET_SPEND) != 0u,
               "MINE must imply WALLET_SPEND (sovereign)");
/* Assisted-ready states never grant MINE or SEED. */
_Static_assert((ZCL_TRUST_MASK_ASSISTED &
                (SYNC_CAP_MINE | SYNC_CAP_SEED_BUNDLE)) == 0u,
               "assisted-ready states must never grant MINE or SEED");
/* SOVEREIGN is the unique holder of EXPORT together with MINE. */
_Static_assert((SYNC_CAP_ALL & SYNC_CAP_EXPORT_BUNDLE) != 0u &&
                   (ZCL_TRUST_MASK_ARTIFACT & SYNC_CAP_EXPORT_BUNDLE) == 0u,
               "only SOVEREIGN combines EXPORT with the self-derived caps");

#undef ZCL_TRUST_MASK_ARTIFACT
#undef ZCL_TRUST_MASK_ASSISTED

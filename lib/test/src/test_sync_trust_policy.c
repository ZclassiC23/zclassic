/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Sync trust → capability policy (services/sync_trust_policy.h). Step-0
 * contract test: the full 8-combo derivation matrix + per-state capability
 * masks + the impossible-combo bans. WF3 lanes 3B/3C/3D add the per-site
 * equivalence tests (old expr == table answer) as they route each gate. */

#include "test/test_helpers.h"
#include "services/sync_trust_policy.h"
#include <string.h>

/* Expected trust state for each (proven, refold, self_derived) combo, per the
 * plan's derivation: S=self_derived, X=proven&&refold. */
static enum sync_trust_state expected(bool proven, bool refold, bool S)
{
    bool X = proven && refold;
    if (S && X) return SYNC_TRUST_SOVEREIGN;
    if (S && !X) return SYNC_TRUST_ARTIFACT_VERIFIED;
    if (!S && X) return SYNC_TRUST_HEADERS_VERIFIED;
    if (!S && !X && proven) return SYNC_TRUST_RELEASE_ASSISTED_READY;
    return SYNC_TRUST_EMPTY;
}

static int test_trust_derive_matrix(void)
{
    int failures = 0;
    TEST("sync_trust: all 8 (proven,refold,self) combos derive as specified") {
        for (int c = 0; c < 8; c++) {
            bool proven = (c >> 2) & 1;
            bool refold = (c >> 1) & 1;
            bool S      = (c >> 0) & 1;
            enum sync_trust_state got = sync_trust_derive(proven, refold, S);
            ASSERT(got == expected(proven, refold, S));
        }
        PASS();
    } _test_next:;
    return failures;
}

static int test_trust_caps(void)
{
    int failures = 0;
    TEST("sync_trust: per-state capability masks match the doctrine") {
        /* SOVEREIGN uniquely holds every capability. */
        ASSERT(sync_trust_caps(SYNC_TRUST_SOVEREIGN) == SYNC_CAP_ALL);
        /* EMPTY holds none. */
        ASSERT(sync_trust_caps(SYNC_TRUST_EMPTY) == SYNC_CAP_NONE);
        /* HEADERS_VERIFIED == export only (X path). */
        ASSERT(sync_trust_caps(SYNC_TRUST_HEADERS_VERIFIED) ==
               SYNC_CAP_EXPORT_BUNDLE);
        /* ARTIFACT_VERIFIED grants MINE + SEED but NOT EXPORT (S, ¬X). */
        uint32_t art = sync_trust_caps(SYNC_TRUST_ARTIFACT_VERIFIED);
        ASSERT(art & SYNC_CAP_MINE);
        ASSERT(art & SYNC_CAP_SEED_BUNDLE);
        ASSERT(!(art & SYNC_CAP_EXPORT_BUNDLE));
        /* MINE ⇒ WALLET_SPEND. */
        ASSERT(art & SYNC_CAP_WALLET_SPEND);
        /* Assisted-ready never mines or seeds. */
        uint32_t rel = sync_trust_caps(SYNC_TRUST_RELEASE_ASSISTED_READY);
        ASSERT(!(rel & (SYNC_CAP_MINE | SYNC_CAP_SEED_BUNDLE)));
        ASSERT(!(sync_trust_caps(SYNC_TRUST_PEER_ASSISTED_READY) &
                 (SYNC_CAP_MINE | SYNC_CAP_SEED_BUNDLE)));
        PASS();
    } _test_next:;
    return failures;
}

static int test_trust_cap_allowed_and_name(void)
{
    int failures = 0;
    TEST("sync_trust: cap_allowed reflects the mask; names round-trip") {
        ASSERT(sync_trust_cap_allowed(SYNC_TRUST_SOVEREIGN, SYNC_CAP_EXPORT_BUNDLE));
        ASSERT(!sync_trust_cap_allowed(SYNC_TRUST_ARTIFACT_VERIFIED,
                                       SYNC_CAP_EXPORT_BUNDLE));
        ASSERT(!sync_trust_cap_allowed(SYNC_TRUST_EMPTY, SYNC_CAP_MINE));
        for (int s = 0; s < SYNC_TRUST_STATE_COUNT; s++)
            ASSERT(strcmp(sync_trust_state_name((enum sync_trust_state)s), "?") != 0);
        ASSERT(strcmp(sync_trust_state_name((enum sync_trust_state)SYNC_TRUST_STATE_COUNT),
                      "?") == 0);
        PASS();
    } _test_next:;
    return failures;
}

/* Per-site equivalence: for ALL 8 combos of (proven, refold, self_derived) the
 * central table answer equals the LITERAL old boolean expression each routed
 * gate site used before centralization. This is the behavior-preservation
 * proof WF3 lanes 3B/3C/3D rely on — the derivation is the single source of
 * truth, and every site's old provenance predicate is exactly reproduced.
 *
 *   EXPORT (sites a+b: bundle_exporter.bx_qualified,
 *           consensus_state_snapshot_export_proof) — old expr: proven && refold
 *           == X, independent of self_derived.
 *   MINE / SPEND (site c: sovereignty_guard_allow) — old expr: self_derived
 *           == S, independent of proven/refold.
 *   SEED (site d: boot_snapshot_offer, lane B2) — old expr: self_derived == S.
 */
static int test_trust_per_site_equivalence(void)
{
    int failures = 0;
    TEST("sync_trust: per-site table answer == old boolean expr (all 8 combos)") {
        for (int c = 0; c < 8; c++) {
            bool proven = (c >> 2) & 1;
            bool refold = (c >> 1) & 1;
            bool S      = (c >> 0) & 1;
            enum sync_trust_state st = sync_trust_derive(proven, refold, S);

            /* Site a/b — EXPORT: old expr was (proven && refold), i.e. X. */
            bool old_export = proven && refold;
            ASSERT(sync_trust_cap_allowed(st, SYNC_CAP_EXPORT_BUNDLE) ==
                   old_export);

            /* Site c — MINE and WALLET_SPEND: old expr was self_derived. */
            ASSERT(sync_trust_cap_allowed(st, SYNC_CAP_MINE) == S);
            ASSERT(sync_trust_cap_allowed(st, SYNC_CAP_WALLET_SPEND) == S);

            /* Site d — SEED_BUNDLE: old expr was self_derived. */
            ASSERT(sync_trust_cap_allowed(st, SYNC_CAP_SEED_BUNDLE) == S);
        }
        PASS();
    } _test_next:;
    return failures;
}

int test_sync_trust_policy(void)
{
    int failures = 0;
    failures += test_trust_derive_matrix();
    failures += test_trust_caps();
    failures += test_trust_cap_allowed_and_name();
    failures += test_trust_per_site_equivalence();
    return failures;
}

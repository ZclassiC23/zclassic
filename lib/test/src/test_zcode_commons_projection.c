/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: prove ZC23 epoch creation-set accounting and byte-identical
 * rebuild-from-CAS of the Living Commons projection. */
#include "test/test_core.h"

#include "vcs/vcs_object.h"
#include "vcs/zcode_commons_projection.h"
#include "vcs/zcode_creation_attribution.h"
#include "vcs/zcode_epoch_creation.h"

#include <string.h>

static void commons_fill(uint8_t out[32], uint8_t value)
{
    memset(out, value, 32);
}

/* A valid standalone attribution: exact challenge timing, policy-epoch 1. */
static void commons_attribution_fixture(
    struct vcs_zcode_creation_attribution_v1 *a, uint8_t fill_base,
    uint64_t award_atoms)
{
    memset(a, 0, sizeof(*a));
    a->schema_version = VCS_ZCODE_CREATION_ATTRIBUTION_VERSION;
    a->category = VCS_ZCODE_CREATION_PUBLIC_SOURCE;
    a->lineage_kind = VCS_ZCODE_CREATION_LINEAGE_NONE;
    a->epoch = 1;
    a->award_atoms = award_atoms;
    a->challenge_opening_height = 100;
    commons_fill(a->challenge_opening_hash, fill_base);
    a->challenge_opening_mtp = 1000;
    a->challenge_maturity_height = 8164;
    a->challenge_maturity_mtp = 605800;
    a->created_unix = 605801;
    commons_fill(a->network_genesis_root, (uint8_t)(fill_base + 1));
    commons_fill(a->zc23_policy_root, (uint8_t)(fill_base + 2));
    commons_fill(a->contributor_binding_root, (uint8_t)(fill_base + 3));
    commons_fill(a->task_root, (uint8_t)(fill_base + 4));
    commons_fill(a->candidate_root, (uint8_t)(fill_base + 5));
    commons_fill(a->proof_policy_root, (uint8_t)(fill_base + 6));
    commons_fill(a->proof_set_root, (uint8_t)(fill_base + 7));
    commons_fill(a->proven_lane_root, (uint8_t)(fill_base + 8));
    commons_fill(a->score_receipt_root, (uint8_t)(fill_base + 9));
    commons_fill(a->package_root, (uint8_t)(fill_base + 10));
    commons_fill(a->release_root, (uint8_t)(fill_base + 11));
    commons_fill(a->license_evidence_root, (uint8_t)(fill_base + 12));
}

static void commons_epoch_fixture(
    struct vcs_zcode_epoch_creation_set_v1 *set, uint64_t epoch,
    uint8_t previous_fill, uint8_t roots[][32], size_t count, uint64_t mint)
{
    vcs_zcode_epoch_creation_init(set);
    set->schema_version = VCS_ZCODE_EPOCH_CREATION_VERSION;
    set->epoch = epoch;
    set->emission_cap_atoms = UINT64_C(5000000000000); /* era-0 cap */
    set->actual_mint_atoms = mint;
    set->unissued_atoms = set->emission_cap_atoms - mint;
    commons_fill(set->network_genesis_root, 21);
    commons_fill(set->zc23_policy_root, 22);
    commons_fill(set->previous_epoch_creation_root, previous_fill);
    commons_fill(set->committee_evidence_snapshot_root, 24);
    set->opening_height = 100;
    commons_fill(set->opening_hash, 25);
    set->opening_mtp = 1000;
    set->maturity_height = 8164;
    commons_fill(set->maturity_hash, 26);
    set->maturity_mtp = 605800;
    set->attribution_roots = roots;
    set->attribution_count = count;
}

static bool commons_store_attribution(
    const char *workspace,
    const struct vcs_zcode_creation_attribution_v1 *attribution,
    uint8_t root_out[32])
{
    uint8_t wire[VCS_ZCODE_CREATION_ATTRIBUTION_WIRE_BYTES];
    if (vcs_zcode_creation_attribution_serialize(attribution, wire) !=
            VCS_ZCODE_CREATION_OK ||
        vcs_zcode_creation_attribution_root(attribution, root_out) !=
            VCS_ZCODE_CREATION_OK)
        return false;
    return vcs_object_put_addressed(workspace, root_out, wire, sizeof(wire));
}

static bool commons_store_epoch(
    const char *workspace, struct vcs_zcode_epoch_creation_set_v1 *set,
    uint8_t root_out[32])
{
    uint8_t *wire = NULL;
    size_t wire_len = 0;
    if (vcs_zcode_epoch_creation_serialize(set, &wire, &wire_len) !=
            VCS_ZCODE_EPOCH_CREATION_OK ||
        vcs_zcode_epoch_creation_root(set, root_out) !=
            VCS_ZCODE_EPOCH_CREATION_OK) {
        free(wire);
        return false;
    }
    bool stored = vcs_object_put_addressed(workspace, root_out, wire,
                                           wire_len);
    free(wire);
    return stored;
}

static int epoch_creation_accounting_test(void)
{
    int failures = 0;
    TEST("ZC23 epoch creation: accounting invariants and wire round-trip") {
        uint8_t roots[2][32];
        commons_fill(roots[0], 27);
        commons_fill(roots[1], 28);
        struct vcs_zcode_epoch_creation_set_v1 set;
        commons_epoch_fixture(&set, 1, 23, roots, 2, UINT64_C(375000000));
        ASSERT_EQ(vcs_zcode_epoch_creation_validate(&set),
                  VCS_ZCODE_EPOCH_CREATION_OK);

        struct vcs_zcode_epoch_creation_set_v1 mutated = set;
        memset(mutated.previous_epoch_creation_root, 0, 32);
        ASSERT_EQ(vcs_zcode_epoch_creation_validate(&mutated),
                  VCS_ZCODE_EPOCH_CREATION_PREDECESSOR);
        mutated = set;
        mutated.epoch = 0; /* epoch 0 must not carry a predecessor */
        mutated.emission_cap_atoms = VCS_ZC23_INITIAL_SUPPLY_ATOMS;
        ASSERT_EQ(vcs_zcode_epoch_creation_validate(&mutated),
                  VCS_ZCODE_EPOCH_CREATION_PREDECESSOR);
        mutated = set;
        mutated.emission_cap_atoms++;
        ASSERT_EQ(vcs_zcode_epoch_creation_validate(&mutated),
                  VCS_ZCODE_EPOCH_CREATION_CAP);
        mutated = set;
        mutated.unissued_atoms++;
        ASSERT_EQ(vcs_zcode_epoch_creation_validate(&mutated),
                  VCS_ZCODE_EPOCH_CREATION_SUM);
        mutated = set;
        mutated.actual_mint_atoms = 0; /* mint of zero means no attributions */
        mutated.unissued_atoms = mutated.emission_cap_atoms;
        ASSERT_EQ(vcs_zcode_epoch_creation_validate(&mutated),
                  VCS_ZCODE_EPOCH_CREATION_SUM);
        mutated = set;
        mutated.attribution_count = VCS_ZCODE_EPOCH_CREATION_MAX_ATTRIBUTIONS + 1;
        ASSERT_EQ(vcs_zcode_epoch_creation_validate(&mutated),
                  VCS_ZCODE_EPOCH_CREATION_ORDER);
        uint8_t swapped[2][32];
        memcpy(swapped[0], roots[1], 32);
        memcpy(swapped[1], roots[0], 32);
        mutated = set;
        mutated.attribution_roots = swapped;
        ASSERT_EQ(vcs_zcode_epoch_creation_validate(&mutated),
                  VCS_ZCODE_EPOCH_CREATION_ORDER);
        uint8_t with_zero[2][32];
        memset(with_zero[0], 0, 32);
        memcpy(with_zero[1], roots[1], 32);
        mutated = set;
        mutated.attribution_roots = with_zero;
        ASSERT_EQ(vcs_zcode_epoch_creation_validate(&mutated),
                  VCS_ZCODE_EPOCH_CREATION_ORDER);
        mutated = set;
        mutated.attribution_count = 1;
        mutated.attribution_roots = NULL;
        ASSERT_EQ(vcs_zcode_epoch_creation_validate(&mutated),
                  VCS_ZCODE_EPOCH_CREATION_ORDER);
        mutated = set;
        mutated.maturity_height = 8163; /* below opening + challenge blocks */
        ASSERT_EQ(vcs_zcode_epoch_creation_validate(&mutated),
                  VCS_ZCODE_EPOCH_CREATION_TIME);
        mutated = set;
        mutated.maturity_mtp = 605799; /* below opening mtp + challenge secs */
        ASSERT_EQ(vcs_zcode_epoch_creation_validate(&mutated),
                  VCS_ZCODE_EPOCH_CREATION_TIME);
        mutated = set;
        mutated.opening_height = UINT64_MAX; /* checked-add must fire */
        ASSERT_EQ(vcs_zcode_epoch_creation_validate(&mutated),
                  VCS_ZCODE_EPOCH_CREATION_OVERFLOW);

        /* The frozen whole-token era curve. */
        uint64_t atoms = UINT64_MAX;
        ASSERT_EQ(vcs_zc23_policy_epoch_cap_atoms(0, &atoms),
                  VCS_ZCODE_EPOCH_CREATION_OK);
        ASSERT(atoms == VCS_ZC23_INITIAL_SUPPLY_ATOMS);
        ASSERT_EQ(vcs_zc23_policy_epoch_cap_atoms(208, &atoms),
                  VCS_ZCODE_EPOCH_CREATION_OK);
        ASSERT(atoms == UINT64_C(5000000000000));
        ASSERT_EQ(vcs_zc23_policy_epoch_cap_atoms(209, &atoms),
                  VCS_ZCODE_EPOCH_CREATION_OK);
        ASSERT(atoms == UINT64_C(2500000000000));
        ASSERT_EQ(vcs_zc23_policy_epoch_cap_atoms(3328, &atoms),
                  VCS_ZCODE_EPOCH_CREATION_OK);
        ASSERT(atoms == VCS_ZC23_ATOMS_PER_TOKEN);
        ASSERT_EQ(vcs_zc23_policy_epoch_cap_atoms(3329, &atoms),
                  VCS_ZCODE_EPOCH_CREATION_OK && atoms == 0);
        ASSERT_EQ(vcs_zc23_policy_epoch_cap_atoms(1, NULL),
                  VCS_ZCODE_EPOCH_CREATION_NULL);

        /* Two-attribution wire: round-trip is byte-exact, truncation fails. */
        uint8_t *wire = NULL, *second = NULL;
        size_t wire_len = 0, second_len = 0;
        ASSERT_EQ(vcs_zcode_epoch_creation_serialize(&set, &wire, &wire_len),
                  VCS_ZCODE_EPOCH_CREATION_OK);
        ASSERT(wire_len == VCS_ZCODE_EPOCH_CREATION_HEADER_BYTES + 64u);
        struct vcs_zcode_epoch_creation_set_v1 parsed, zero;
        vcs_zcode_epoch_creation_init(&zero);
        ASSERT_EQ(vcs_zcode_epoch_creation_parse(wire, wire_len, &parsed),
                  VCS_ZCODE_EPOCH_CREATION_OK);
        vcs_zcode_epoch_creation_free(&parsed);
        for (size_t cut = 0; cut < wire_len; cut++) {
            ASSERT(vcs_zcode_epoch_creation_parse(wire, cut, &parsed) !=
                   VCS_ZCODE_EPOCH_CREATION_OK);
            ASSERT(memcmp(&parsed, &zero, sizeof(parsed)) == 0);
        }
        ASSERT_EQ(vcs_zcode_epoch_creation_parse(wire, wire_len, &parsed),
                  VCS_ZCODE_EPOCH_CREATION_OK);
        ASSERT_EQ(vcs_zcode_epoch_creation_serialize(&parsed, &second,
                                                     &second_len),
                  VCS_ZCODE_EPOCH_CREATION_OK);
        ASSERT(second_len == wire_len && memcmp(wire, second, wire_len) == 0);
        uint8_t root_a[32], root_b[32];
        ASSERT_EQ(vcs_zcode_epoch_creation_root(&set, root_a),
                  VCS_ZCODE_EPOCH_CREATION_OK);
        ASSERT_EQ(vcs_zcode_epoch_creation_root(&parsed, root_b),
                  VCS_ZCODE_EPOCH_CREATION_OK);
        ASSERT(memcmp(root_a, root_b, 32) == 0);
        wire[0] ^= 1;
        ASSERT_EQ(vcs_zcode_epoch_creation_parse(wire, wire_len, &parsed),
                  VCS_ZCODE_EPOCH_CREATION_MAGIC);
        ASSERT(memcmp(&parsed, &zero, sizeof(parsed)) == 0);
        wire[0] ^= 1;
        wire[10] = 1; /* reserved u16 must stay zero */
        ASSERT_EQ(vcs_zcode_epoch_creation_parse(wire, wire_len, &parsed),
                  VCS_ZCODE_EPOCH_CREATION_RESERVED);
        ASSERT(memcmp(&parsed, &zero, sizeof(parsed)) == 0);
        free(second);
        free(wire);
        vcs_zcode_epoch_creation_free(&parsed);
        PASS();
    } _test_next:;
    return failures;
}

struct epoch_callback_fixture {
    bool anchor_active;
    uint64_t opening_height;
    uint8_t opening_hash[32];
    uint64_t maturity_height;
    uint8_t maturity_hash[32];
    uint64_t award_atoms;
};

static bool epoch_test_anchor(void *opaque, uint64_t height,
                              const uint8_t hash[32])
{
    const struct epoch_callback_fixture *fixture = opaque;
    return fixture && fixture->anchor_active &&
           ((height == fixture->opening_height &&
             memcmp(hash, fixture->opening_hash, 32) == 0) ||
            (height == fixture->maturity_height &&
             memcmp(hash, fixture->maturity_hash, 32) == 0));
}

static bool epoch_test_duplicate(void *opaque,
                                 const uint8_t candidate_root[32],
                                 const uint8_t attribution_root[32])
{
    (void)opaque;
    (void)candidate_root;
    (void)attribution_root;
    return false;
}

static bool epoch_test_award(
    void *opaque, const struct vcs_zcode_creation_attribution_v1 *attribution,
    uint64_t *expected_atoms)
{
    const struct epoch_callback_fixture *fixture = opaque;
    (void)attribution;
    if (!fixture || !expected_atoms)
        return false;
    *expected_atoms = fixture->award_atoms;
    return true;
}

static int epoch_creation_verify_failclosed_test(void)
{
    int failures = 0;
    TEST("ZC23 epoch creation: CAS verification fails closed rung by rung") {
        char workspace[256];
        test_make_tmpdir(workspace, sizeof(workspace),
                         "zcode_epoch_verify", "empty");
        ASSERT(vcs_object_store_init(workspace));

        uint8_t roots[1][32];
        commons_fill(roots[0], 27);
        struct vcs_zcode_epoch_creation_set_v1 set;
        commons_epoch_fixture(&set, 1, 23, roots, 1, UINT64_C(125000000));
        ASSERT_EQ(vcs_zcode_epoch_creation_validate(&set),
                  VCS_ZCODE_EPOCH_CREATION_OK);

        struct epoch_callback_fixture callbacks;
        memset(&callbacks, 0, sizeof(callbacks));
        callbacks.anchor_active = true;
        callbacks.opening_height = set.opening_height;
        memcpy(callbacks.opening_hash, set.opening_hash, 32);
        callbacks.maturity_height = set.maturity_height;
        memcpy(callbacks.maturity_hash, set.maturity_hash, 32);
        callbacks.award_atoms = set.actual_mint_atoms;

        struct vcs_zcode_epoch_creation_validation_context context = {
            .workspace = workspace,
            .expected_network_genesis_root = set.network_genesis_root,
            .expected_zc23_policy_root = set.zc23_policy_root,
            .expected_previous_epoch_creation_root =
                set.previous_epoch_creation_root,
            .observed_actual_mint_atoms = set.actual_mint_atoms,
            .active_height = set.maturity_height,
            .active_mtp = set.maturity_mtp,
            .now_unix = 605801,
            .anchor_is_active = epoch_test_anchor,
            .contribution_is_duplicate = epoch_test_duplicate,
            .award_atoms_for_creation = epoch_test_award,
            .callback_opaque = &callbacks,
        };
        ASSERT_EQ(vcs_zcode_epoch_creation_verify_cas(&set, NULL),
                  VCS_ZCODE_EPOCH_CREATION_CONTEXT);
        context.now_unix = 0;
        ASSERT_EQ(vcs_zcode_epoch_creation_verify_cas(&set, &context),
                  VCS_ZCODE_EPOCH_CREATION_CONTEXT);
        context.now_unix = 605801;
        context.anchor_is_active = NULL;
        ASSERT_EQ(vcs_zcode_epoch_creation_verify_cas(&set, &context),
                  VCS_ZCODE_EPOCH_CREATION_CONTEXT);
        context.anchor_is_active = epoch_test_anchor;
        context.contribution_is_duplicate = NULL;
        ASSERT_EQ(vcs_zcode_epoch_creation_verify_cas(&set, &context),
                  VCS_ZCODE_EPOCH_CREATION_CONTEXT);
        context.contribution_is_duplicate = epoch_test_duplicate;
        context.award_atoms_for_creation = NULL;
        ASSERT_EQ(vcs_zcode_epoch_creation_verify_cas(&set, &context),
                  VCS_ZCODE_EPOCH_CREATION_CONTEXT);
        context.award_atoms_for_creation = epoch_test_award;

        uint8_t zero_root[32] = {0};
        context.expected_previous_epoch_creation_root = zero_root;
        ASSERT_EQ(vcs_zcode_epoch_creation_verify_cas(&set, &context),
                  VCS_ZCODE_EPOCH_CREATION_PREDECESSOR);
        context.expected_previous_epoch_creation_root =
            set.previous_epoch_creation_root;
        context.observed_actual_mint_atoms = 1;
        ASSERT_EQ(vcs_zcode_epoch_creation_verify_cas(&set, &context),
                  VCS_ZCODE_EPOCH_CREATION_MINT);
        context.observed_actual_mint_atoms = set.actual_mint_atoms;
        context.active_height = set.maturity_height - 1;
        ASSERT_EQ(vcs_zcode_epoch_creation_verify_cas(&set, &context),
                  VCS_ZCODE_EPOCH_CREATION_IMMATURE);
        context.active_height = set.maturity_height;
        callbacks.anchor_active = false; /* anchor reorged off the chain */
        ASSERT_EQ(vcs_zcode_epoch_creation_verify_cas(&set, &context),
                  VCS_ZCODE_EPOCH_CREATION_REORG);
        callbacks.anchor_active = true;

        /* Context and chain facts pass; the attribution object is absent. */
        ASSERT_EQ(vcs_zcode_epoch_creation_verify_cas(&set, &context),
                  VCS_ZCODE_EPOCH_CREATION_CAS);
        test_rm_rf(workspace);
        PASS();
    } _test_next:;
    return failures;
}

static int commons_rebuild_identity_test(void)
{
    int failures = 0;
    TEST("ZC23 commons projection: rebuild from CAS is byte-identical") {
        char workspace[256];
        test_make_tmpdir(workspace, sizeof(workspace),
                         "zcode_commons_rebuild", "populated");
        ASSERT(vcs_object_store_init(workspace));

        struct vcs_zcode_creation_attribution_v1 first_attribution,
            second_attribution;
        commons_attribution_fixture(&first_attribution, 27,
                                    UINT64_C(125000000));
        commons_attribution_fixture(&second_attribution, 41,
                                    UINT64_C(250000000));
        uint8_t first_root[32], second_root[32];
        ASSERT(commons_store_attribution(workspace, &first_attribution,
                                         first_root));
        ASSERT(commons_store_attribution(workspace, &second_attribution,
                                         second_root));
        ASSERT(memcmp(first_root, second_root, 32) != 0);

        uint8_t ordered[2][32];
        if (memcmp(first_root, second_root, 32) < 0) {
            memcpy(ordered[0], first_root, 32);
            memcpy(ordered[1], second_root, 32);
        } else {
            memcpy(ordered[0], second_root, 32);
            memcpy(ordered[1], first_root, 32);
        }
        struct vcs_zcode_epoch_creation_set_v1 set;
        commons_epoch_fixture(&set, 1, 23, ordered, 2, UINT64_C(375000000));
        uint8_t epoch_root[32];
        ASSERT(commons_store_epoch(workspace, &set, epoch_root));

        struct vcs_zcode_commons_projection *first =
            vcs_zcode_commons_projection_build(workspace);
        struct vcs_zcode_commons_projection *second =
            vcs_zcode_commons_projection_build(workspace);
        ASSERT(first && second);
        ASSERT(vcs_zcode_commons_projection_status(first) ==
               VCS_ZCODE_COMMONS_PARTIAL);
        ASSERT(vcs_zcode_commons_projection_creation_count(first) == 2);
        ASSERT(vcs_zcode_commons_projection_epoch_count(first) == 1);
        ASSERT(vcs_zcode_commons_projection_attributed_atoms(first) ==
               UINT64_C(375000000));
        ASSERT(vcs_zcode_commons_projection_minted_atoms(first) ==
               UINT64_C(375000000));
        ASSERT(vcs_zcode_commons_projection_unissued_atoms(first) ==
               UINT64_C(5000000000000) - UINT64_C(375000000));
        const struct vcs_zcode_commons_creation_entry *creation_first =
            vcs_zcode_commons_projection_creation_at(first, 0);
        const struct vcs_zcode_commons_creation_entry *creation_second =
            vcs_zcode_commons_projection_creation_at(first, 1);
        const struct vcs_zcode_commons_epoch_entry *epoch =
            vcs_zcode_commons_projection_epoch_at(first, 0);
        ASSERT(creation_first && creation_second && epoch);
        ASSERT(vcs_zcode_commons_projection_creation_at(first, 2) == NULL);
        ASSERT(vcs_zcode_commons_projection_epoch_at(first, 1) == NULL);
        ASSERT(memcmp(creation_first->root, ordered[0], 32) == 0);
        ASSERT(memcmp(creation_second->root, ordered[1], 32) == 0);
        ASSERT(creation_first->epoch == 1 &&
               creation_second->epoch == 1);
        ASSERT(creation_first->category == VCS_ZCODE_CREATION_PUBLIC_SOURCE);
        ASSERT(creation_first->award_atoms +
                   creation_second->award_atoms ==
               UINT64_C(375000000));
        ASSERT(memcmp(epoch->root, epoch_root, 32) == 0);
        ASSERT(epoch->epoch == 1);
        ASSERT(epoch->cap_atoms == UINT64_C(5000000000000));
        ASSERT(epoch->minted_atoms == UINT64_C(375000000));
        ASSERT(epoch->unissued_atoms ==
               UINT64_C(5000000000000) - UINT64_C(375000000));
        ASSERT(epoch->attribution_count == 2);
        ASSERT(memcmp(epoch->previous_root,
                      set.previous_epoch_creation_root, 32) == 0);
        uint8_t failure_root[32];
        const char *failure_reason = NULL;
        ASSERT(!vcs_zcode_commons_projection_first_failure(
                   first, failure_root, &failure_reason));
        uint8_t first_projection_root[32], second_projection_root[32];
        ASSERT(vcs_zcode_commons_projection_root(
                   first, first_projection_root));
        ASSERT(vcs_zcode_commons_projection_root(
                   second, second_projection_root));
        ASSERT(memcmp(first_projection_root, second_projection_root,
                      32) == 0);
        vcs_zcode_commons_projection_free(second);
        vcs_zcode_commons_projection_free(first);
        test_rm_rf(workspace);
        PASS();
    } _test_next:;
    return failures;
}

static int commons_accounting_failures_test(void)
{
    int failures = 0;
    TEST("ZC23 commons projection: broken accounting is a named failure") {
        uint8_t failure_root[32];
        const char *failure_reason = NULL;

        /* Two epoch sets referencing the same attribution: the duplicate is
         * flagged, never double-counted silently. */
        char duplicate_workspace[256];
        test_make_tmpdir(duplicate_workspace, sizeof(duplicate_workspace),
                         "zcode_commons_failure", "duplicate");
        ASSERT(vcs_object_store_init(duplicate_workspace));
        struct vcs_zcode_creation_attribution_v1 attribution;
        commons_attribution_fixture(&attribution, 27, UINT64_C(125000000));
        uint8_t attribution_root[32];
        ASSERT(commons_store_attribution(duplicate_workspace, &attribution,
                                         attribution_root));
        uint8_t one_root[1][32];
        memcpy(one_root[0], attribution_root, 32);
        struct vcs_zcode_epoch_creation_set_v1 first_set, second_set;
        commons_epoch_fixture(&first_set, 1, 23, one_root, 1,
                              UINT64_C(125000000));
        commons_epoch_fixture(&second_set, 1, 29, one_root, 1,
                              UINT64_C(125000000));
        uint8_t first_epoch_root[32], second_epoch_root[32];
        ASSERT(commons_store_epoch(duplicate_workspace, &first_set,
                                   first_epoch_root));
        ASSERT(commons_store_epoch(duplicate_workspace, &second_set,
                                   second_epoch_root));
        ASSERT(memcmp(first_epoch_root, second_epoch_root, 32) != 0);
        struct vcs_zcode_commons_projection *projection =
            vcs_zcode_commons_projection_build(duplicate_workspace);
        ASSERT(projection);
        ASSERT(vcs_zcode_commons_projection_status(projection) ==
               VCS_ZCODE_COMMONS_PARTIAL);
        ASSERT(vcs_zcode_commons_projection_creation_count(projection) == 1);
        ASSERT(vcs_zcode_commons_projection_epoch_count(projection) == 2);
        ASSERT(vcs_zcode_commons_projection_first_failure(
                   projection, failure_root, &failure_reason));
        ASSERT(memcmp(failure_root, attribution_root, 32) == 0);
        ASSERT_STR_EQ(failure_reason, "duplicate-attribution");
        vcs_zcode_commons_projection_free(projection);
        test_rm_rf(duplicate_workspace);

        /* An epoch set referencing an attribution the CAS does not hold. */
        char missing_workspace[256];
        test_make_tmpdir(missing_workspace, sizeof(missing_workspace),
                         "zcode_commons_failure", "missing");
        ASSERT(vcs_object_store_init(missing_workspace));
        uint8_t absent[1][32];
        commons_fill(absent[0], 99);
        struct vcs_zcode_epoch_creation_set_v1 missing_set;
        commons_epoch_fixture(&missing_set, 1, 23, absent, 1,
                              UINT64_C(125000000));
        uint8_t missing_epoch_root[32];
        ASSERT(commons_store_epoch(missing_workspace, &missing_set,
                                   missing_epoch_root));
        projection = vcs_zcode_commons_projection_build(missing_workspace);
        ASSERT(projection);
        ASSERT(vcs_zcode_commons_projection_creation_count(projection) == 0);
        ASSERT(vcs_zcode_commons_projection_epoch_count(projection) == 1);
        ASSERT(vcs_zcode_commons_projection_attributed_atoms(projection) == 0);
        ASSERT(vcs_zcode_commons_projection_minted_atoms(projection) ==
               UINT64_C(125000000));
        ASSERT(vcs_zcode_commons_projection_first_failure(
                   projection, failure_root, &failure_reason));
        bool absent_is_lower =
            memcmp(absent[0], missing_epoch_root, 32) < 0;
        ASSERT(memcmp(failure_root,
                      absent_is_lower ? absent[0] : missing_epoch_root,
                      32) == 0);
        ASSERT_STR_EQ(failure_reason,
                      absent_is_lower ? "missing-attribution"
                                      : "epoch-attribution-mismatch");
        vcs_zcode_commons_projection_free(projection);
        test_rm_rf(missing_workspace);

        /* Minted atoms that do not equal the referenced awards. */
        char sum_workspace[256];
        test_make_tmpdir(sum_workspace, sizeof(sum_workspace),
                         "zcode_commons_failure", "sum");
        ASSERT(vcs_object_store_init(sum_workspace));
        ASSERT(commons_store_attribution(sum_workspace, &attribution,
                                         attribution_root));
        memcpy(one_root[0], attribution_root, 32);
        struct vcs_zcode_epoch_creation_set_v1 sum_set;
        commons_epoch_fixture(&sum_set, 1, 23, one_root, 1,
                              UINT64_C(125000001));
        uint8_t sum_epoch_root[32];
        ASSERT(commons_store_epoch(sum_workspace, &sum_set, sum_epoch_root));
        projection = vcs_zcode_commons_projection_build(sum_workspace);
        ASSERT(projection);
        ASSERT(vcs_zcode_commons_projection_first_failure(
                   projection, failure_root, &failure_reason));
        ASSERT(memcmp(failure_root, sum_epoch_root, 32) == 0);
        ASSERT_STR_EQ(failure_reason, "epoch-sum-mismatch");
        vcs_zcode_commons_projection_free(projection);
        test_rm_rf(sum_workspace);
        PASS();
    } _test_next:;
    return failures;
}

int test_zcode_commons_projection(void)
{
    return epoch_creation_accounting_test() +
           epoch_creation_verify_failclosed_test() +
           commons_rebuild_identity_test() +
           commons_accounting_failures_test();
}

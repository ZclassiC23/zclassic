/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: simulation-only ZC23 policy and approved-reproducer-set proofs. */
#include "test/test_core.h"

#include "vcs/zcode_score_receipt.h"
#include "vcs/zcode_shadow_policy.h"

#include <string.h>

static void shadow_fill(uint8_t out[32], uint8_t value)
{
    memset(out, value, 32);
}

static void shadow_entry(struct vcs_zcode_approved_reproducer_entry_v1 *entry,
                         uint8_t signer)
{
    memset(entry, 0, sizeof(*entry));
    shadow_fill(entry->signer_pubkey, signer);
    shadow_fill(entry->contributor_binding_root, (uint8_t)(signer + 1u));
    shadow_fill(entry->operator_group_root, (uint8_t)(signer + 2u));
    vcs_zcode_score_action_root(VCS_ZCODE_SCORE_INDEPENDENT_REPRODUCTION,
                                entry->action_root);
    entry->valid_from_epoch = 2;
    entry->valid_through_epoch = 8;
    entry->valid_from_unix = 1000;
    entry->valid_through_unix = 9000;
}

static bool shadow_set(struct vcs_zcode_approved_reproducer_set_v1 *set)
{
    struct vcs_zcode_approved_reproducer_entry_v1 high, low;
    vcs_zcode_approved_reproducer_set_init(set);
    shadow_fill(set->network_genesis_root, 11);
    set->sequence = 1;
    shadow_entry(&high, 40);
    shadow_entry(&low, 20);
    return vcs_zcode_approved_reproducer_set_add(set, &high) ==
               VCS_ZCODE_SHADOW_OK &&
           vcs_zcode_approved_reproducer_set_add(set, &low) ==
               VCS_ZCODE_SHADOW_OK;
}

static bool shadow_policy(struct vcs_zcode_policy_candidate_v1 *policy,
                          const struct vcs_zcode_approved_reproducer_set_v1 *set)
{
    uint8_t set_root[32], network[32], covenant[32];
    shadow_fill(network, 11);
    shadow_fill(covenant, 12);
    if (vcs_zcode_approved_reproducer_set_root(set, set_root) !=
        VCS_ZCODE_SHADOW_OK)
        return false;
    vcs_zcode_policy_candidate_init(policy, network, set_root, covenant);
    return true;
}

static int shadow_set_codec_test(void)
{
    int failures = 0;
    TEST("ZC23 approved reproducer set: canonical order, exact wire and root") {
        struct vcs_zcode_approved_reproducer_set_v1 set, parsed, zero;
        uint8_t wire[VCS_ZCODE_APPROVED_REPRODUCER_SET_MAX_WIRE_BYTES];
        uint8_t second[VCS_ZCODE_APPROVED_REPRODUCER_SET_MAX_WIRE_BYTES];
        uint8_t root_a[32], root_b[32];
        size_t wire_len = 0, second_len = 0;
        ASSERT(shadow_set(&set));
        ASSERT(set.entry_count == 2);
        ASSERT(set.entries[0].signer_pubkey[0] == 20);
        ASSERT(vcs_zcode_approved_reproducer_set_serialize(
                   &set, wire, sizeof(wire), &wire_len) ==
               VCS_ZCODE_SHADOW_OK);
        ASSERT(wire_len == VCS_ZCODE_APPROVED_REPRODUCER_SET_HEADER_BYTES +
                           2u * VCS_ZCODE_APPROVED_REPRODUCER_ENTRY_BYTES);
        ASSERT(vcs_zcode_approved_reproducer_set_parse(
                   wire, wire_len, &parsed) == VCS_ZCODE_SHADOW_OK);
        ASSERT(vcs_zcode_approved_reproducer_set_serialize(
                   &parsed, second, sizeof(second), &second_len) ==
               VCS_ZCODE_SHADOW_OK);
        ASSERT(second_len == wire_len && memcmp(wire, second, wire_len) == 0);
        ASSERT(vcs_zcode_approved_reproducer_set_root(&set, root_a) ==
               VCS_ZCODE_SHADOW_OK);
        ASSERT(vcs_zcode_approved_reproducer_set_root(&parsed, root_b) ==
               VCS_ZCODE_SHADOW_OK);
        ASSERT(memcmp(root_a, root_b, 32) == 0);
        static const uint8_t root_kat[32] = {
            0x37, 0xf5, 0x3a, 0x1b, 0xa3, 0x1d, 0x9f, 0xd5,
            0x97, 0xfb, 0x57, 0x6d, 0xa9, 0x41, 0x57, 0x3f,
            0x11, 0x9f, 0x5c, 0x2c, 0x53, 0xb7, 0x12, 0xa5,
            0xae, 0xa3, 0x65, 0xc6, 0xba, 0x33, 0x3c, 0xf1,
        };
        ASSERT(memcmp(root_a, root_kat, sizeof(root_kat)) == 0);

        memset(&zero, 0, sizeof(zero));
        for (size_t cut = 0; cut < wire_len; cut++) {
            ASSERT(vcs_zcode_approved_reproducer_set_parse(
                       wire, cut, &parsed) != VCS_ZCODE_SHADOW_OK);
            ASSERT(memcmp(&parsed, &zero, sizeof(parsed)) == 0);
        }
        wire[wire_len] = 0;
        ASSERT(vcs_zcode_approved_reproducer_set_parse(
                   wire, wire_len + 1u, &parsed) != VCS_ZCODE_SHADOW_OK);
        ASSERT(memcmp(&parsed, &zero, sizeof(parsed)) == 0);
        ASSERT(vcs_zcode_approved_reproducer_set_serialize(
                   &set, wire, sizeof(wire), &wire_len) ==
               VCS_ZCODE_SHADOW_OK);
        wire[0] ^= 1u;
        ASSERT(vcs_zcode_approved_reproducer_set_parse(
                   wire, wire_len, &parsed) == VCS_ZCODE_SHADOW_MAGIC);
        ASSERT(memcmp(&parsed, &zero, sizeof(parsed)) == 0);
        ASSERT(vcs_zcode_approved_reproducer_set_serialize(
                   &set, wire, sizeof(wire), &wire_len) ==
               VCS_ZCODE_SHADOW_OK);
        wire[8] = 2;
        ASSERT(vcs_zcode_approved_reproducer_set_parse(
                   wire, wire_len, &parsed) == VCS_ZCODE_SHADOW_VERSION);
        ASSERT(memcmp(&parsed, &zero, sizeof(parsed)) == 0);
        ASSERT(vcs_zcode_approved_reproducer_set_serialize(
                   &set, wire, sizeof(wire), &wire_len) ==
               VCS_ZCODE_SHADOW_OK);
        wire[12] = 1;
        ASSERT(vcs_zcode_approved_reproducer_set_parse(
                   wire, wire_len, &parsed) == VCS_ZCODE_SHADOW_RESERVED);
        ASSERT(memcmp(&parsed, &zero, sizeof(parsed)) == 0);
        PASS();
    } _test_next:;
    return failures;
}

static int shadow_set_rejection_test(void)
{
    int failures = 0;
    TEST("ZC23 approved reproducer set: duplicates, stale grants and actions fail closed") {
        struct vcs_zcode_approved_reproducer_set_v1 set, other;
        struct vcs_zcode_approved_reproducer_entry_v1 entry, found;
        ASSERT(shadow_set(&set));
        entry = set.entries[0];
        shadow_fill(entry.operator_group_root, 99);
        ASSERT(vcs_zcode_approved_reproducer_set_add(&set, &entry) ==
               VCS_ZCODE_SHADOW_DUPLICATE);
        shadow_entry(&entry, 60);
        memset(entry.action_root, 0, 32);
        ASSERT(vcs_zcode_approved_reproducer_set_add(&set, &entry) ==
               VCS_ZCODE_SHADOW_ACTION);
        shadow_entry(&entry, 60);
        entry.valid_from_epoch = entry.valid_through_epoch + 1u;
        ASSERT(vcs_zcode_approved_reproducer_set_add(&set, &entry) ==
               VCS_ZCODE_SHADOW_TIME);

        ASSERT(vcs_zcode_approved_reproducer_set_find(
                   &set, set.entries[0].signer_pubkey,
                   set.entries[0].action_root, 4, 4000, &found) ==
               VCS_ZCODE_SHADOW_OK);
        ASSERT(vcs_zcode_approved_reproducer_set_find(
                   &set, set.entries[0].signer_pubkey,
                   set.entries[0].action_root, 9, 4000, &found) ==
               VCS_ZCODE_SHADOW_EPOCH);
        ASSERT(vcs_zcode_approved_reproducer_set_find(
                   &set, set.entries[0].signer_pubkey,
                   set.entries[0].action_root, 4, 999, &found) ==
               VCS_ZCODE_SHADOW_TIME);
        uint8_t wrong_action[32]; shadow_fill(wrong_action, 77);
        ASSERT(vcs_zcode_approved_reproducer_set_find(
                   &set, set.entries[0].signer_pubkey, wrong_action,
                   4, 4000, &found) == VCS_ZCODE_SHADOW_ACTION);

        other = set;
        other.flags &= (uint16_t)~VCS_ZCODE_SHADOW_NOT_OWNER_APPROVED;
        ASSERT(vcs_zcode_approved_reproducer_set_validate(&other) ==
               VCS_ZCODE_SHADOW_FLAGS);
        ASSERT(!vcs_zcode_score_offhost_reproducer_approved(
            set.entries[0].signer_pubkey));
        PASS();
    } _test_next:;
    return failures;
}

static int shadow_policy_codec_test(void)
{
    int failures = 0;
    TEST("ZC23 policy candidate: fixed simulation covenant and award table") {
        struct vcs_zcode_approved_reproducer_set_v1 set, wrong_set;
        struct vcs_zcode_policy_candidate_v1 policy, parsed, zero, changed;
        uint8_t wire[VCS_ZCODE_POLICY_CANDIDATE_WIRE_BYTES];
        uint8_t wire2[VCS_ZCODE_POLICY_CANDIDATE_WIRE_BYTES];
        uint8_t root_a[32], root_b[32];
        uint64_t award = UINT64_MAX;
        ASSERT(shadow_set(&set));
        memset(&policy, 0, sizeof(policy));
        ASSERT(shadow_policy(&policy, &set));
        ASSERT(vcs_zcode_policy_candidate_validate(&policy) ==
               VCS_ZCODE_SHADOW_OK);
        ASSERT(vcs_zcode_policy_candidate_validate_set(&policy, &set) ==
               VCS_ZCODE_SHADOW_OK);
        ASSERT(vcs_zcode_policy_candidate_serialize(&policy, wire) ==
               VCS_ZCODE_SHADOW_OK);
        ASSERT(vcs_zcode_policy_candidate_parse(wire, sizeof(wire), &parsed) ==
               VCS_ZCODE_SHADOW_OK);
        ASSERT(vcs_zcode_policy_candidate_serialize(&parsed, wire2) ==
               VCS_ZCODE_SHADOW_OK);
        ASSERT(memcmp(wire, wire2, sizeof(wire)) == 0);
        ASSERT(vcs_zcode_policy_candidate_root(&policy, root_a) ==
               VCS_ZCODE_SHADOW_OK);
        ASSERT(vcs_zcode_policy_candidate_root(&parsed, root_b) ==
               VCS_ZCODE_SHADOW_OK);
        ASSERT(memcmp(root_a, root_b, 32) == 0);
        static const uint8_t root_kat[32] = {
            0xe6, 0x0c, 0xca, 0xb1, 0x2c, 0x6e, 0xbe, 0x6f,
            0xc5, 0x90, 0x8e, 0x39, 0x7b, 0x09, 0xb1, 0x84,
            0x7f, 0x44, 0x08, 0x84, 0x77, 0x78, 0xf3, 0xf4,
            0x22, 0xaf, 0x5e, 0x77, 0x76, 0xce, 0xdf, 0x77,
        };
        ASSERT(memcmp(root_a, root_kat, sizeof(root_kat)) == 0);
        ASSERT(vcs_zcode_policy_candidate_award_atoms(
                   &policy, VCS_ZCODE_CREATION_PUBLIC_SOURCE, &award) ==
               VCS_ZCODE_SHADOW_OK &&
               award == VCS_ZC23_SHADOW_PUBLIC_SOURCE_ATOMS);
        ASSERT(vcs_zcode_policy_candidate_award_atoms(
                   &policy, VCS_ZCODE_CREATION_SECURITY_FIX, &award) ==
               VCS_ZCODE_SHADOW_OK &&
               award == VCS_ZC23_SHADOW_BORN_RED_ATOMS);

        changed = policy;
        changed.flags &= (uint16_t)~VCS_ZCODE_SHADOW_SIMULATION_ONLY;
        ASSERT(vcs_zcode_policy_candidate_validate(&changed) ==
               VCS_ZCODE_SHADOW_FLAGS);
        changed = policy;
        changed.award_atoms[VCS_ZCODE_CREATION_PUBLIC_SOURCE - 1u]++;
        ASSERT(vcs_zcode_policy_candidate_validate(&changed) ==
               VCS_ZCODE_SHADOW_AMOUNT);
        wrong_set = set;
        shadow_fill(wrong_set.network_genesis_root, 91);
        ASSERT(vcs_zcode_policy_candidate_validate_set(&policy, &wrong_set) ==
               VCS_ZCODE_SHADOW_NETWORK);
        wrong_set = set;
        wrong_set.entries[0].operator_group_root[0] ^= 1u;
        ASSERT(vcs_zcode_policy_candidate_validate_set(&policy, &wrong_set) ==
               VCS_ZCODE_SHADOW_POLICY);

        memset(&zero, 0, sizeof(zero));
        for (size_t cut = 0; cut < sizeof(wire); cut++) {
            ASSERT(vcs_zcode_policy_candidate_parse(wire, cut, &parsed) !=
                   VCS_ZCODE_SHADOW_OK);
            ASSERT(memcmp(&parsed, &zero, sizeof(parsed)) == 0);
        }
        ASSERT(vcs_zcode_policy_candidate_parse(wire, sizeof(wire) - 1u,
                                                &parsed) !=
               VCS_ZCODE_SHADOW_OK);
        ASSERT(vcs_zcode_policy_candidate_serialize(&policy, wire) ==
               VCS_ZCODE_SHADOW_OK);
        wire[0] ^= 1u;
        ASSERT(vcs_zcode_policy_candidate_parse(wire, sizeof(wire), &parsed) ==
               VCS_ZCODE_SHADOW_MAGIC);
        ASSERT(memcmp(&parsed, &zero, sizeof(parsed)) == 0);
        ASSERT(vcs_zcode_policy_candidate_serialize(&policy, wire) ==
               VCS_ZCODE_SHADOW_OK);
        wire[8] = 2;
        ASSERT(vcs_zcode_policy_candidate_parse(wire, sizeof(wire), &parsed) ==
               VCS_ZCODE_SHADOW_VERSION);
        ASSERT(memcmp(&parsed, &zero, sizeof(parsed)) == 0);
        ASSERT(vcs_zcode_policy_candidate_serialize(&policy, wire) ==
               VCS_ZCODE_SHADOW_OK);
        wire[224] = 1;
        ASSERT(vcs_zcode_policy_candidate_parse(wire, sizeof(wire), &parsed) ==
               VCS_ZCODE_SHADOW_RESERVED);
        ASSERT(memcmp(&parsed, &zero, sizeof(parsed)) == 0);
        PASS();
    } _test_next:;
    return failures;
}

int test_zcode_shadow_policy(void)
{
    int failures = 0;
    failures += shadow_set_codec_test();
    failures += shadow_set_rejection_test();
    failures += shadow_policy_codec_test();
    return failures;
}

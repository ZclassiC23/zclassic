/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: prove ZC23 patronage offer/funding/settlement wire codecs and that
 * the simulated settlement lifecycle fails closed on every live-money path. */
#include "test/test_core.h"

#include "crypto/ed25519.h"
#include "keys/key.h"
#include "keys/pubkey.h"
#include "vcs/vcs_object.h"
#include "vcs/zcode_contributor_binding.h"
#include "vcs/zcode_patronage.h"
#include "vcs/zcode_patronage_funding.h"
#include "vcs/zcode_patronage_projection.h"
#include "vcs/zcode_patronage_settlement.h"

#include <string.h>

#define PATRONAGE_GIFT_AMOUNT UINT64_C(500000000)

static void patronage_fill(uint8_t out[32], uint8_t value)
{
    memset(out, value, 32);
}

static void patronage_gift_intent(
    struct vcs_zcode_patronage_intent_v1 *intent, const uint8_t network[32],
    const uint8_t patron_binding_root[32], const uint8_t patron_pubkey[32],
    const uint8_t recipient_binding_root[32], uint8_t trust_mode)
{
    memset(intent, 0, sizeof(*intent));
    intent->schema_version = VCS_ZCODE_PATRONAGE_INTENT_VERSION;
    intent->mode = VCS_ZCODE_PATRONAGE_DIRECT_GIFT;
    intent->target_kind = VCS_ZCODE_PATRONAGE_TARGET_CONTRIBUTOR;
    intent->settlement_trust_mode = trust_mode;
    intent->flags = VCS_ZCODE_PATRONAGE_NO_AUTHORITY |
                    VCS_ZCODE_PATRONAGE_SIMULATION_ONLY;
    memcpy(intent->network_genesis_root, network, 32);
    patronage_fill(intent->zc23_token_or_simulation_root, 32);
    memcpy(intent->patron_contributor_binding_root, patron_binding_root, 32);
    memcpy(intent->patron_zid_pubkey, patron_pubkey, 32);
    memcpy(intent->target_root, recipient_binding_root, 32);
    memcpy(intent->intended_recipient_binding_root,
           recipient_binding_root, 32);
    intent->amount_atoms = PATRONAGE_GIFT_AMOUNT;
    intent->created_unix = 1000;
    intent->expires_unix = 2000;
    intent->sequence = 1;
    intent->maximum_zcl_fee_zat = 10000;
}

static void patronage_funding_fixture(
    struct vcs_zcode_patronage_funding_v1 *funding, const uint8_t network[32],
    const uint8_t intent_root[32], const uint8_t patron_binding_root[32],
    const uint8_t patron_pubkey[32])
{
    memset(funding, 0, sizeof(*funding));
    funding->schema_version = VCS_ZCODE_PATRONAGE_FUNDING_VERSION;
    funding->funding_kind = VCS_ZCODE_PATRONAGE_FUNDING_FULLY_SIMULATED;
    funding->flags = VCS_ZCODE_PATRONAGE_FUNDING_NO_LIVE_FUNDS |
                     VCS_ZCODE_PATRONAGE_FUNDING_NO_TRANSACTION_BYTES;
    memcpy(funding->network_genesis_root, network, 32);
    memcpy(funding->patronage_intent_root, intent_root, 32);
    memcpy(funding->funder_contributor_binding_root,
           patron_binding_root, 32);
    memcpy(funding->funder_zid_pubkey, patron_pubkey, 32);
    funding->amount_atoms = PATRONAGE_GIFT_AMOUNT;
    funding->created_unix = 1500;
    funding->sequence = 1;
    (void)vcs_zcode_patronage_simulation_plan_root(
        intent_root, PATRONAGE_GIFT_AMOUNT, funding->simulation_plan_root);
}

static int patronage_intent_gift_test(void)
{
    int failures = 0;
    TEST("ZC23 patronage intent: direct-gift wire is exact and non-authoritative") {
        uint8_t seed[32], secret[32], pubkey[32];
        memset(seed, 42, sizeof(seed));
        zcl_ed25519_keypair(pubkey, secret, seed);
        uint8_t network[32], patron_binding[32], recipient_binding[32];
        patronage_fill(network, 0xc1);
        patronage_fill(patron_binding, 33);
        patronage_fill(recipient_binding, 36);

        struct vcs_zcode_patronage_intent_v1 intent, parsed, zero;
        memset(&zero, 0, sizeof(zero));
        patronage_gift_intent(&intent, network, patron_binding, pubkey,
                              recipient_binding,
                              VCS_ZCODE_PATRONAGE_UNFUNDED_OFFER);
        ASSERT_EQ(vcs_zcode_patronage_intent_seal(&intent, secret, pubkey),
                  VCS_ZCODE_PATRONAGE_OK);
        ASSERT_EQ(vcs_zcode_patronage_intent_verify(&intent, 1000),
                  VCS_ZCODE_PATRONAGE_OK);
        ASSERT_EQ(vcs_zcode_patronage_intent_verify(&intent, 1999),
                  VCS_ZCODE_PATRONAGE_OK);
        ASSERT_EQ(vcs_zcode_patronage_intent_verify(&intent, 999),
                  VCS_ZCODE_PATRONAGE_TIME);
        ASSERT_EQ(vcs_zcode_patronage_intent_verify(&intent, 2000),
                  VCS_ZCODE_PATRONAGE_TIME);

        uint8_t wire[VCS_ZCODE_PATRONAGE_INTENT_WIRE_BYTES];
        uint8_t second[VCS_ZCODE_PATRONAGE_INTENT_WIRE_BYTES];
        ASSERT_EQ(vcs_zcode_patronage_intent_serialize(&intent, wire),
                  VCS_ZCODE_PATRONAGE_OK);
        static const uint8_t prefix_kat[16] = {
            'Z','C','P','A','T','R','\r','\n',
            0x01, 0x00, 0x03, 0x04, 0x01, 0x05, 0x00, 0x00,
        };
        ASSERT(memcmp(wire, prefix_kat, sizeof(prefix_kat)) == 0);
        ASSERT_EQ(vcs_zcode_patronage_intent_parse(wire, sizeof(wire),
                                                   &parsed),
                  VCS_ZCODE_PATRONAGE_OK);
        ASSERT_EQ(vcs_zcode_patronage_intent_serialize(&parsed, second),
                  VCS_ZCODE_PATRONAGE_OK);
        ASSERT(memcmp(wire, second, sizeof(wire)) == 0);
        uint8_t root_a[32], root_b[32];
        ASSERT_EQ(vcs_zcode_patronage_intent_root(&intent, root_a),
                  VCS_ZCODE_PATRONAGE_OK);
        ASSERT_EQ(vcs_zcode_patronage_intent_root(&parsed, root_b),
                  VCS_ZCODE_PATRONAGE_OK);
        ASSERT(memcmp(root_a, root_b, 32) == 0);

        for (size_t cut = 0; cut < sizeof(wire); cut++) {
            ASSERT_EQ(vcs_zcode_patronage_intent_parse(wire, cut, &parsed),
                      VCS_ZCODE_PATRONAGE_WIRE_SIZE);
            ASSERT(memcmp(&parsed, &zero, sizeof(parsed)) == 0);
        }
        uint8_t malformed[VCS_ZCODE_PATRONAGE_INTENT_WIRE_BYTES + 1];
        memcpy(malformed, wire, sizeof(wire));
        malformed[sizeof(wire)] = 0;
        ASSERT_EQ(vcs_zcode_patronage_intent_parse(
                      malformed, sizeof(malformed), &parsed),
                  VCS_ZCODE_PATRONAGE_WIRE_SIZE);
        ASSERT(memcmp(&parsed, &zero, sizeof(parsed)) == 0);
        malformed[0] ^= 1;
        ASSERT_EQ(vcs_zcode_patronage_intent_parse(
                      malformed, sizeof(wire), &parsed),
                  VCS_ZCODE_PATRONAGE_MAGIC);
        ASSERT(memcmp(&parsed, &zero, sizeof(parsed)) == 0);
        memcpy(malformed, wire, sizeof(wire));
        malformed[14] = 1; /* reserved u16 must stay zero */
        ASSERT_EQ(vcs_zcode_patronage_intent_parse(
                      malformed, sizeof(wire), &parsed),
                  VCS_ZCODE_PATRONAGE_SHAPE);
        ASSERT(memcmp(&parsed, &zero, sizeof(parsed)) == 0);
        memcpy(malformed, wire, sizeof(wire));
        malformed[sizeof(wire) - 1] ^= 1;
        ASSERT_EQ(vcs_zcode_patronage_intent_parse(
                      malformed, sizeof(wire), &parsed),
                  VCS_ZCODE_PATRONAGE_OK);
        ASSERT_EQ(vcs_zcode_patronage_intent_verify(&parsed, 1500),
                  VCS_ZCODE_PATRONAGE_SIGNATURE);

        /* Direct-gift shape: no refund schedule, no proof roots, no task
         * target — any of them present is rejected. */
        patronage_gift_intent(&intent, network, patron_binding, pubkey,
                              recipient_binding,
                              VCS_ZCODE_PATRONAGE_UNFUNDED_OFFER);
        intent.refund_height = 3000;
        ASSERT_EQ(vcs_zcode_patronage_intent_validate(&intent),
                  VCS_ZCODE_PATRONAGE_SHAPE);
        patronage_gift_intent(&intent, network, patron_binding, pubkey,
                              recipient_binding,
                              VCS_ZCODE_PATRONAGE_UNFUNDED_OFFER);
        patronage_fill(intent.task_root, 34);
        ASSERT_EQ(vcs_zcode_patronage_intent_validate(&intent),
                  VCS_ZCODE_PATRONAGE_SHAPE);
        patronage_gift_intent(&intent, network, patron_binding, pubkey,
                              recipient_binding,
                              VCS_ZCODE_PATRONAGE_UNFUNDED_OFFER);
        intent.target_kind = VCS_ZCODE_PATRONAGE_TARGET_TASK;
        ASSERT_EQ(vcs_zcode_patronage_intent_validate(&intent),
                  VCS_ZCODE_PATRONAGE_SHAPE);
        patronage_gift_intent(&intent, network, patron_binding, pubkey,
                              recipient_binding,
                              VCS_ZCODE_PATRONAGE_UNFUNDED_OFFER);
        intent.mode = 0;
        ASSERT_EQ(vcs_zcode_patronage_intent_validate(&intent),
                  VCS_ZCODE_PATRONAGE_ENUM);
        patronage_gift_intent(&intent, network, patron_binding, pubkey,
                              recipient_binding,
                              VCS_ZCODE_PATRONAGE_UNFUNDED_OFFER);
        intent.settlement_trust_mode = 0;
        ASSERT_EQ(vcs_zcode_patronage_intent_validate(&intent),
                  VCS_ZCODE_PATRONAGE_ENUM);
        patronage_gift_intent(&intent, network, patron_binding, pubkey,
                              recipient_binding,
                              VCS_ZCODE_PATRONAGE_UNFUNDED_OFFER);
        intent.amount_atoms = 0;
        ASSERT_EQ(vcs_zcode_patronage_intent_validate(&intent),
                  VCS_ZCODE_PATRONAGE_AMOUNT);
        patronage_gift_intent(&intent, network, patron_binding, pubkey,
                              recipient_binding,
                              VCS_ZCODE_PATRONAGE_UNFUNDED_OFFER);
        intent.flags &= (uint8_t)~VCS_ZCODE_PATRONAGE_SIMULATION_ONLY;
        ASSERT_EQ(vcs_zcode_patronage_intent_validate(&intent),
                  VCS_ZCODE_PATRONAGE_FLAGS);

        /* CAS verification fails closed before any authority is granted. */
        patronage_gift_intent(&intent, network, patron_binding, pubkey,
                              recipient_binding,
                              VCS_ZCODE_PATRONAGE_UNFUNDED_OFFER);
        ASSERT_EQ(vcs_zcode_patronage_intent_seal(&intent, secret, pubkey),
                  VCS_ZCODE_PATRONAGE_OK);
        ASSERT_EQ(vcs_zcode_patronage_intent_verify_cas(&intent, NULL),
                  VCS_ZCODE_PATRONAGE_TIME);
        struct vcs_zcode_patronage_validation_context context = {
            .workspace = NULL,
            .expected_network_genesis_root = network,
            .now_unix = 1500,
        };
        ASSERT_EQ(vcs_zcode_patronage_intent_verify_cas(&intent, &context),
                  VCS_ZCODE_PATRONAGE_CONTEXT);
        uint8_t other_network[32];
        patronage_fill(other_network, 0xc2);
        context.workspace = "./test-tmp";
        context.expected_network_genesis_root = other_network;
        ASSERT_EQ(vcs_zcode_patronage_intent_verify_cas(&intent, &context),
                  VCS_ZCODE_PATRONAGE_NETWORK);
        PASS();
    } _test_next:;
    return failures;
}

static int patronage_funding_codec_test(void)
{
    int failures = 0;
    TEST("ZC23 patronage funding: simulated receipt wire rejects live funds") {
        uint8_t seed[32], secret[32], pubkey[32];
        memset(seed, 51, sizeof(seed));
        zcl_ed25519_keypair(pubkey, secret, seed);
        uint8_t network[32], patron_binding[32], intent_root[32];
        patronage_fill(network, 0xc1);
        patronage_fill(patron_binding, 33);
        patronage_fill(intent_root, 41);

        /* Simulation plan root is pure deterministic arithmetic. */
        uint8_t plan_a[32], plan_b[32];
        ASSERT_EQ(vcs_zcode_patronage_simulation_plan_root(
                      intent_root, PATRONAGE_GIFT_AMOUNT, NULL),
                  VCS_ZCODE_PATRONAGE_FUNDING_NULL);
        ASSERT_EQ(vcs_zcode_patronage_simulation_plan_root(
                      NULL, PATRONAGE_GIFT_AMOUNT, plan_a),
                  VCS_ZCODE_PATRONAGE_FUNDING_NULL);
        uint8_t zero_root[32] = {0};
        ASSERT_EQ(vcs_zcode_patronage_simulation_plan_root(
                      zero_root, PATRONAGE_GIFT_AMOUNT, plan_a),
                  VCS_ZCODE_PATRONAGE_FUNDING_ROOT);
        ASSERT(memcmp(plan_a, zero_root, 32) == 0);
        ASSERT_EQ(vcs_zcode_patronage_simulation_plan_root(
                      intent_root, 0, plan_a),
                  VCS_ZCODE_PATRONAGE_FUNDING_AMOUNT);
        ASSERT(memcmp(plan_a, zero_root, 32) == 0);
        ASSERT_EQ(vcs_zcode_patronage_simulation_plan_root(
                      intent_root, PATRONAGE_GIFT_AMOUNT, plan_a),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        ASSERT_EQ(vcs_zcode_patronage_simulation_plan_root(
                      intent_root, PATRONAGE_GIFT_AMOUNT, plan_b),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        ASSERT(memcmp(plan_a, plan_b, 32) == 0);
        ASSERT_EQ(vcs_zcode_patronage_simulation_plan_root(
                      intent_root, PATRONAGE_GIFT_AMOUNT + 1, plan_b),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        ASSERT(memcmp(plan_a, plan_b, 32) != 0);

        struct vcs_zcode_patronage_funding_v1 funding, parsed, zero;
        memset(&zero, 0, sizeof(zero));
        patronage_funding_fixture(&funding, network, intent_root,
                                  patron_binding, pubkey);
        ASSERT_EQ(vcs_zcode_patronage_funding_seal(&funding, secret, pubkey),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        ASSERT_EQ(vcs_zcode_patronage_funding_verify(&funding),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);

        uint8_t wire[VCS_ZCODE_PATRONAGE_FUNDING_WIRE_BYTES];
        uint8_t second[VCS_ZCODE_PATRONAGE_FUNDING_WIRE_BYTES];
        ASSERT_EQ(vcs_zcode_patronage_funding_serialize(&funding, wire),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        static const uint8_t prefix_kat[16] = {
            'Z','C','P','F','U','N','\r','\n',
            0x01, 0x00, 0x01, 0x03, 0x00, 0x00, 0x00, 0x00,
        };
        ASSERT(memcmp(wire, prefix_kat, sizeof(prefix_kat)) == 0);
        ASSERT_EQ(vcs_zcode_patronage_funding_parse(wire, sizeof(wire),
                                                    &parsed),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        ASSERT_EQ(vcs_zcode_patronage_funding_serialize(&parsed, second),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        ASSERT(memcmp(wire, second, sizeof(wire)) == 0);
        uint8_t root_a[32], root_b[32];
        ASSERT_EQ(vcs_zcode_patronage_funding_root(&funding, root_a),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        ASSERT_EQ(vcs_zcode_patronage_funding_root(&parsed, root_b),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        ASSERT(memcmp(root_a, root_b, 32) == 0);

        for (size_t cut = 0; cut < sizeof(wire); cut++) {
            ASSERT_EQ(vcs_zcode_patronage_funding_parse(wire, cut, &parsed),
                      VCS_ZCODE_PATRONAGE_FUNDING_WIRE_SIZE);
            ASSERT(memcmp(&parsed, &zero, sizeof(parsed)) == 0);
        }
        uint8_t malformed[VCS_ZCODE_PATRONAGE_FUNDING_WIRE_BYTES + 1];
        memcpy(malformed, wire, sizeof(wire));
        malformed[sizeof(wire)] = 0;
        ASSERT_EQ(vcs_zcode_patronage_funding_parse(
                      malformed, sizeof(malformed), &parsed),
                  VCS_ZCODE_PATRONAGE_FUNDING_WIRE_SIZE);
        ASSERT(memcmp(&parsed, &zero, sizeof(parsed)) == 0);
        malformed[0] ^= 1;
        ASSERT_EQ(vcs_zcode_patronage_funding_parse(
                      malformed, sizeof(wire), &parsed),
                  VCS_ZCODE_PATRONAGE_FUNDING_MAGIC);
        ASSERT(memcmp(&parsed, &zero, sizeof(parsed)) == 0);
        memcpy(malformed, wire, sizeof(wire));
        malformed[12] = 1; /* reserved u32 must stay zero */
        ASSERT_EQ(vcs_zcode_patronage_funding_parse(
                      malformed, sizeof(wire), &parsed),
                  VCS_ZCODE_PATRONAGE_FUNDING_SHAPE);
        ASSERT(memcmp(&parsed, &zero, sizeof(parsed)) == 0);
        memcpy(malformed, wire, sizeof(wire));
        malformed[sizeof(wire) - 1] ^= 1;
        ASSERT_EQ(vcs_zcode_patronage_funding_parse(
                      malformed, sizeof(wire), &parsed),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        ASSERT_EQ(vcs_zcode_patronage_funding_verify(&parsed),
                  VCS_ZCODE_PATRONAGE_FUNDING_SIGNATURE);

        /* The simulation-only shape is mandatory: clearing the no-live-funds
         * flag, faking the plan root, or wrong kind all fail closed. */
        patronage_funding_fixture(&funding, network, intent_root,
                                  patron_binding, pubkey);
        funding.flags &= (uint8_t)~VCS_ZCODE_PATRONAGE_FUNDING_NO_LIVE_FUNDS;
        ASSERT_EQ(vcs_zcode_patronage_funding_validate(&funding),
                  VCS_ZCODE_PATRONAGE_FUNDING_SHAPE);
        patronage_funding_fixture(&funding, network, intent_root,
                                  patron_binding, pubkey);
        funding.flags &=
            (uint8_t)~VCS_ZCODE_PATRONAGE_FUNDING_NO_TRANSACTION_BYTES;
        ASSERT_EQ(vcs_zcode_patronage_funding_validate(&funding),
                  VCS_ZCODE_PATRONAGE_FUNDING_SHAPE);
        patronage_funding_fixture(&funding, network, intent_root,
                                  patron_binding, pubkey);
        funding.funding_kind = 2;
        ASSERT_EQ(vcs_zcode_patronage_funding_validate(&funding),
                  VCS_ZCODE_PATRONAGE_FUNDING_SHAPE);
        patronage_funding_fixture(&funding, network, intent_root,
                                  patron_binding, pubkey);
        patronage_fill(funding.simulation_plan_root, 99);
        ASSERT_EQ(vcs_zcode_patronage_funding_validate(&funding),
                  VCS_ZCODE_PATRONAGE_FUNDING_ROOT);
        patronage_funding_fixture(&funding, network, intent_root,
                                  patron_binding, pubkey);
        funding.amount_atoms = 0;
        ASSERT_EQ(vcs_zcode_patronage_funding_validate(&funding),
                  VCS_ZCODE_PATRONAGE_FUNDING_AMOUNT);
        patronage_funding_fixture(&funding, network, intent_root,
                                  patron_binding, pubkey);
        funding.sequence = 0;
        ASSERT_EQ(vcs_zcode_patronage_funding_validate(&funding),
                  VCS_ZCODE_PATRONAGE_FUNDING_TIME);

        /* CAS verification requires the intent object and a live window. */
        char workspace[256];
        test_make_tmpdir(workspace, sizeof(workspace),
                         "zcode_patronage_funding", "empty");
        ASSERT(vcs_object_store_init(workspace));
        patronage_funding_fixture(&funding, network, intent_root,
                                  patron_binding, pubkey);
        ASSERT_EQ(vcs_zcode_patronage_funding_seal(&funding, secret, pubkey),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        struct vcs_zcode_patronage_validation_context context = {
            .workspace = workspace,
            .expected_network_genesis_root = network,
            .now_unix = 1600,
        };
        ASSERT_EQ(vcs_zcode_patronage_funding_verify_cas(&funding, &context),
                  VCS_ZCODE_PATRONAGE_FUNDING_INTENT);
        context.now_unix = 1400; /* funding.created_unix (1500) is future */
        ASSERT_EQ(vcs_zcode_patronage_funding_verify_cas(&funding, &context),
                  VCS_ZCODE_PATRONAGE_FUNDING_TIME);
        context.workspace = NULL;
        context.now_unix = 1600;
        ASSERT_EQ(vcs_zcode_patronage_funding_verify_cas(&funding, &context),
                  VCS_ZCODE_PATRONAGE_FUNDING_CONTEXT);
        ASSERT_EQ(vcs_zcode_patronage_funding_verify_cas(&funding, NULL),
                  VCS_ZCODE_PATRONAGE_FUNDING_CONTEXT);
        test_rm_rf(workspace);
        PASS();
    } _test_next:;
    return failures;
}

struct patronage_lifecycle {
    char workspace[256];
    uint8_t network[32];
    uint8_t patron_pubkey[32];
    uint8_t patron_secret[32];
    uint8_t patron_binding_root[32];
    uint8_t recipient_binding_root[32];
    struct vcs_zcode_patronage_intent_v1 intent;
    uint8_t intent_root[32];
    struct vcs_zcode_patronage_funding_v1 funding;
    uint8_t funding_root[32];
};

static bool patronage_store_binding(const char *workspace,
                                    const uint8_t network[32],
                                    uint8_t zid_seed_value,
                                    uint8_t zcl_value,
                                    uint8_t zid_pubkey[32],
                                    uint8_t zid_secret[32],
                                    uint8_t root_out[32])
{
    uint8_t seed[32], zcl_secret[32];
    memset(seed, zid_seed_value, sizeof(seed));
    zcl_ed25519_keypair(zid_pubkey, zid_secret, seed);
    memset(zcl_secret, zcl_value, sizeof(zcl_secret));
    struct privkey secret;
    struct pubkey pubkey;
    memset(secret.vch, zcl_value, 32);
    secret.fValid = true;
    secret.fCompressed = true;
    if (!privkey_get_pubkey(&secret, &pubkey) ||
        pubkey.size != COMPRESSED_PUBLIC_KEY_SIZE)
        return false;
    struct vcs_zcode_contributor_binding_v1 binding;
    memset(&binding, 0, sizeof(binding));
    binding.schema_version = VCS_ZCODE_CONTRIBUTOR_BINDING_VERSION;
    memcpy(binding.network_genesis_root, network, 32);
    memcpy(binding.zid_pubkey, zid_pubkey, 32);
    memcpy(binding.zcl_pubkey, pubkey.vch, 33);
    struct key_id key_id = pubkey_get_id(&pubkey);
    memcpy(binding.zcl_key_id, key_id.id.data, 20);
    binding.sequence = 1;
    binding.issued_unix = 100;
    binding.expires_unix = 1000000;
    binding.operation = VCS_ZCODE_BINDING_ACTIVE;
    if (vcs_zcode_contributor_binding_seal(
            &binding, zid_secret, zid_pubkey, zcl_secret) !=
        VCS_ZCODE_BINDING_OK)
        return false;
    uint8_t wire[VCS_ZCODE_CONTRIBUTOR_BINDING_WIRE_BYTES];
    if (vcs_zcode_contributor_binding_root(&binding, root_out) !=
            VCS_ZCODE_BINDING_OK ||
        vcs_zcode_contributor_binding_serialize(&binding, wire) !=
            VCS_ZCODE_BINDING_OK)
        return false;
    return vcs_object_put_addressed(workspace, root_out, wire, sizeof(wire));
}

static bool patronage_lifecycle_build(struct patronage_lifecycle *life)
{
    memset(life, 0, sizeof(*life));
    test_make_tmpdir(life->workspace, sizeof(life->workspace),
                     "zcode_patronage_life", "cas");
    if (!vcs_object_store_init(life->workspace))
        return false;
    patronage_fill(life->network, 0xc1);
    uint8_t recipient_pubkey[32], recipient_secret[32];
    if (!patronage_store_binding(life->workspace, life->network, 0x41, 0x42,
                                 life->patron_pubkey, life->patron_secret,
                                 life->patron_binding_root) ||
        !patronage_store_binding(life->workspace, life->network, 0x43, 0x44,
                                 recipient_pubkey, recipient_secret,
                                 life->recipient_binding_root))
        return false;
    patronage_gift_intent(&life->intent, life->network,
                          life->patron_binding_root, life->patron_pubkey,
                          life->recipient_binding_root,
                          VCS_ZCODE_PATRONAGE_SIMULATED_FUNDING);
    if (vcs_zcode_patronage_intent_seal(
            &life->intent, life->patron_secret, life->patron_pubkey) !=
        VCS_ZCODE_PATRONAGE_OK)
        return false;
    uint8_t intent_wire[VCS_ZCODE_PATRONAGE_INTENT_WIRE_BYTES];
    if (vcs_zcode_patronage_intent_root(&life->intent, life->intent_root) !=
            VCS_ZCODE_PATRONAGE_OK ||
        vcs_zcode_patronage_intent_serialize(&life->intent, intent_wire) !=
            VCS_ZCODE_PATRONAGE_OK ||
        !vcs_object_put_addressed(life->workspace, life->intent_root,
                                  intent_wire, sizeof(intent_wire)))
        return false;
    patronage_funding_fixture(&life->funding, life->network,
                              life->intent_root, life->patron_binding_root,
                              life->patron_pubkey);
    if (vcs_zcode_patronage_funding_seal(
            &life->funding, life->patron_secret, life->patron_pubkey) !=
        VCS_ZCODE_PATRONAGE_FUNDING_OK)
        return false;
    uint8_t funding_wire[VCS_ZCODE_PATRONAGE_FUNDING_WIRE_BYTES];
    if (vcs_zcode_patronage_funding_root(&life->funding,
                                         life->funding_root) !=
            VCS_ZCODE_PATRONAGE_FUNDING_OK ||
        vcs_zcode_patronage_funding_serialize(&life->funding, funding_wire) !=
            VCS_ZCODE_PATRONAGE_FUNDING_OK ||
        !vcs_object_put_addressed(life->workspace, life->funding_root,
                                  funding_wire, sizeof(funding_wire)))
        return false;
    return true;
}

static void patronage_gift_settlement(
    struct vcs_zcode_patronage_settlement_v1 *settlement,
    const struct patronage_lifecycle *life)
{
    memset(settlement, 0, sizeof(*settlement));
    settlement->schema_version = VCS_ZCODE_PATRONAGE_SETTLEMENT_VERSION;
    settlement->action = VCS_ZCODE_PATRONAGE_SIMULATED_SETTLED;
    settlement->flags = VCS_ZCODE_PATRONAGE_SETTLEMENT_SIMULATION_ONLY |
        VCS_ZCODE_PATRONAGE_SETTLEMENT_NO_LIVE_FUNDS |
        VCS_ZCODE_PATRONAGE_SETTLEMENT_NO_TRANSACTION_BYTES;
    memcpy(settlement->network_genesis_root, life->network, 32);
    memcpy(settlement->patronage_intent_root, life->intent_root, 32);
    memcpy(settlement->patronage_funding_root, life->funding_root, 32);
    memcpy(settlement->recipient_contributor_binding_root,
           life->recipient_binding_root, 32);
    memcpy(settlement->settler_zid_pubkey, life->patron_pubkey, 32);
    settlement->amount_atoms = PATRONAGE_GIFT_AMOUNT;
    settlement->created_unix = 1700;
    settlement->observed_height = 100;
    settlement->observed_mtp = 1600;
    settlement->sequence = 1;
}

static int patronage_settlement_lifecycle_test(void)
{
    int failures = 0;
    TEST("ZC23 patronage settlement: simulated lifecycle, live paths closed") {
        struct patronage_lifecycle life;
        ASSERT(patronage_lifecycle_build(&life));
        struct vcs_zcode_patronage_validation_context context = {
            .workspace = life.workspace,
            .expected_network_genesis_root = life.network,
            .now_unix = 1500,
        };
        ASSERT_EQ(vcs_zcode_patronage_intent_verify_cas(
                      &life.intent, &context),
                  VCS_ZCODE_PATRONAGE_OK);
        uint8_t other_network[32];
        patronage_fill(other_network, 0xc2);
        context.expected_network_genesis_root = other_network;
        ASSERT_EQ(vcs_zcode_patronage_intent_verify_cas(
                      &life.intent, &context),
                  VCS_ZCODE_PATRONAGE_NETWORK);
        context.expected_network_genesis_root = life.network;
        ASSERT_EQ(vcs_zcode_patronage_funding_verify_cas(
                      &life.funding, &context),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);

        /* Funding against the intent fails closed on amount and timing. */
        struct vcs_zcode_patronage_funding_v1 funding = life.funding;
        funding.amount_atoms = PATRONAGE_GIFT_AMOUNT + 1;
        ASSERT_EQ(vcs_zcode_patronage_simulation_plan_root(
                      life.intent_root, funding.amount_atoms,
                      funding.simulation_plan_root),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        ASSERT_EQ(vcs_zcode_patronage_funding_seal(
                      &funding, life.patron_secret, life.patron_pubkey),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        ASSERT_EQ(vcs_zcode_patronage_funding_verify_cas(&funding, &context),
                  VCS_ZCODE_PATRONAGE_FUNDING_AMOUNT);
        funding = life.funding;
        funding.created_unix = 500; /* before the intent existed */
        ASSERT_EQ(vcs_zcode_patronage_funding_seal(
                      &funding, life.patron_secret, life.patron_pubkey),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        ASSERT_EQ(vcs_zcode_patronage_funding_verify_cas(&funding, &context),
                  VCS_ZCODE_PATRONAGE_FUNDING_TIME);

        struct vcs_zcode_patronage_settlement_v1 settlement;
        patronage_gift_settlement(&settlement, &life);
        ASSERT_EQ(vcs_zcode_patronage_settlement_seal(
                      &settlement, life.patron_secret, life.patron_pubkey),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_OK);
        struct vcs_zcode_patronage_settlement_validation_context
            settlement_context = {
                .patronage = &context,
                .creation = NULL,
                .active_height = 200,
                .active_mtp = 1650,
                .now_unix = 1700,
            };
        ASSERT_EQ(vcs_zcode_patronage_settlement_verify_cas(
                      &settlement, &settlement_context),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_OK);

        /* Every deviation from the simulated gift fails closed with its own
         * typed error — none can move or imply live funds. */
        ASSERT_EQ(vcs_zcode_patronage_settlement_verify_cas(&settlement,
                                                            NULL),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_CONTEXT);
        settlement_context.active_height = 0;
        ASSERT_EQ(vcs_zcode_patronage_settlement_verify_cas(
                      &settlement, &settlement_context),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_CONTEXT);
        settlement_context.active_height = 200;

        struct vcs_zcode_patronage_settlement_v1 mutated = settlement;
        mutated.created_unix = 1701; /* after the chain's now */
        ASSERT_EQ(vcs_zcode_patronage_settlement_seal(
                      &mutated, life.patron_secret, life.patron_pubkey),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_OK);
        ASSERT_EQ(vcs_zcode_patronage_settlement_verify_cas(
                      &mutated, &settlement_context),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_TIME);

        mutated = settlement;
        mutated.observed_height = 201; /* above the active chain */
        ASSERT_EQ(vcs_zcode_patronage_settlement_seal(
                      &mutated, life.patron_secret, life.patron_pubkey),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_OK);
        ASSERT_EQ(vcs_zcode_patronage_settlement_verify_cas(
                      &mutated, &settlement_context),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_TIME);

        mutated = settlement;
        mutated.created_unix = 1500; /* not after the funding receipt */
        ASSERT_EQ(vcs_zcode_patronage_settlement_seal(
                      &mutated, life.patron_secret, life.patron_pubkey),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_OK);
        ASSERT_EQ(vcs_zcode_patronage_settlement_verify_cas(
                      &mutated, &settlement_context),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_TIME);

        mutated = settlement;
        mutated.amount_atoms = PATRONAGE_GIFT_AMOUNT + 1;
        ASSERT_EQ(vcs_zcode_patronage_settlement_seal(
                      &mutated, life.patron_secret, life.patron_pubkey),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_OK);
        ASSERT_EQ(vcs_zcode_patronage_settlement_verify_cas(
                      &mutated, &settlement_context),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_AMOUNT);

        mutated = settlement;
        memcpy(mutated.patronage_funding_root, life.intent_root, 32);
        ASSERT_EQ(vcs_zcode_patronage_settlement_seal(
                      &mutated, life.patron_secret, life.patron_pubkey),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_OK);
        ASSERT_EQ(vcs_zcode_patronage_settlement_verify_cas(
                      &mutated, &settlement_context),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_FUNDING);

        mutated = settlement;
        mutated.created_unix = 2000; /* offer already expired */
        settlement_context.now_unix = 2000;
        ASSERT_EQ(vcs_zcode_patronage_settlement_seal(
                      &mutated, life.patron_secret, life.patron_pubkey),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_OK);
        ASSERT_EQ(vcs_zcode_patronage_settlement_verify_cas(
                      &mutated, &settlement_context),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_INTENT);
        settlement_context.now_unix = 1700;

        mutated = settlement;
        memcpy(mutated.recipient_contributor_binding_root,
               life.patron_binding_root, 32);
        ASSERT_EQ(vcs_zcode_patronage_settlement_seal(
                      &mutated, life.patron_secret, life.patron_pubkey),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_OK);
        ASSERT_EQ(vcs_zcode_patronage_settlement_verify_cas(
                      &mutated, &settlement_context),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_INTENT);

        mutated = settlement;
        mutated.action = VCS_ZCODE_PATRONAGE_SIMULATED_REFUNDED;
        ASSERT_EQ(vcs_zcode_patronage_settlement_seal(
                      &mutated, life.patron_secret, life.patron_pubkey),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_OK);
        ASSERT_EQ(vcs_zcode_patronage_settlement_verify_cas(
                      &mutated, &settlement_context),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_INTENT);

        /* A gift carrying a proof chain is not even wire-canonical. */
        patronage_gift_settlement(&settlement, &life);
        patronage_fill(settlement.task_root, 64);
        ASSERT_EQ(vcs_zcode_patronage_settlement_validate(&settlement),
                  VCS_ZCODE_PATRONAGE_SETTLEMENT_SHAPE);
        test_rm_rf(life.workspace);
        PASS();
    } _test_next:;
    return failures;
}

static int patronage_projection_test(void)
{
    int failures = 0;
    TEST("ZC23 patronage projection: CAS rebuild arithmetic is deterministic") {
        ASSERT(vcs_zcode_patronage_projection_build(NULL) == NULL);
        uint8_t network[32];
        patronage_fill(network, 0xc1);
        struct vcs_zcode_patronage_validation_context context = {
            .workspace = "./test-tmp",
            .expected_network_genesis_root = network,
            .now_unix = 0,
        };
        ASSERT(vcs_zcode_patronage_projection_build(&context) == NULL);

        char absent[256];
        test_fmt_tmpdir(absent, sizeof(absent),
                        "zcode_patronage_projection", "absent");
        test_rm_rf(absent);
        ASSERT(access(absent, F_OK) != 0);
        context.workspace = absent;
        context.now_unix = 1700;
        struct vcs_zcode_patronage_projection *empty_first =
            vcs_zcode_patronage_projection_build(&context);
        struct vcs_zcode_patronage_projection *empty_second =
            vcs_zcode_patronage_projection_build(&context);
        ASSERT(empty_first && empty_second);
        ASSERT(access(absent, F_OK) != 0);
        ASSERT(vcs_zcode_patronage_projection_count(empty_first) == 0);
        ASSERT(vcs_zcode_patronage_projection_at(empty_first, 0) == NULL);
        uint8_t failure_root[32];
        const char *failure_reason = NULL;
        ASSERT(!vcs_zcode_patronage_projection_first_failure(
                   empty_first, failure_root, &failure_reason));
        uint8_t empty_root_a[32], empty_root_b[32];
        ASSERT(vcs_zcode_patronage_projection_root(
                   empty_first, empty_root_a));
        ASSERT(vcs_zcode_patronage_projection_root(
                   empty_second, empty_root_b));
        ASSERT(memcmp(empty_root_a, empty_root_b, 32) == 0);
        vcs_zcode_patronage_projection_free(empty_second);
        vcs_zcode_patronage_projection_free(empty_first);

        struct patronage_lifecycle life;
        ASSERT(patronage_lifecycle_build(&life));
        context.workspace = life.workspace;
        context.expected_network_genesis_root = life.network;
        struct vcs_zcode_patronage_projection *first =
            vcs_zcode_patronage_projection_build(&context);
        struct vcs_zcode_patronage_projection *second =
            vcs_zcode_patronage_projection_build(&context);
        ASSERT(first && second);
        ASSERT(vcs_zcode_patronage_projection_count(first) == 2);
        const struct vcs_zcode_patronage_projection_entry *offer = NULL;
        const struct vcs_zcode_patronage_projection_entry *funding = NULL;
        const struct vcs_zcode_patronage_projection_entry *previous = NULL;
        for (size_t i = 0; i < 2; i++) {
            const struct vcs_zcode_patronage_projection_entry *entry =
                vcs_zcode_patronage_projection_at(first, i);
            ASSERT(entry != NULL);
            if (previous)
                ASSERT(memcmp(previous->root, entry->root, 32) < 0);
            previous = entry;
            if (entry->kind == VCS_ZCODE_PATRONAGE_PROJECTION_OFFER)
                offer = entry;
            if (entry->kind == VCS_ZCODE_PATRONAGE_PROJECTION_SIMULATED_FUNDING)
                funding = entry;
        }
        ASSERT(offer && funding);
        ASSERT(memcmp(offer->root, life.intent_root, 32) == 0);
        ASSERT(memcmp(offer->target_root, life.recipient_binding_root,
                      32) == 0);
        ASSERT(offer->amount_atoms == PATRONAGE_GIFT_AMOUNT);
        ASSERT(offer->created_unix == 1000);
        ASSERT(offer->expires_unix == 2000);
        ASSERT(memcmp(funding->root, life.funding_root, 32) == 0);
        ASSERT(memcmp(funding->target_root, life.intent_root, 32) == 0);
        ASSERT(funding->amount_atoms == PATRONAGE_GIFT_AMOUNT);
        ASSERT(funding->created_unix == 1500);
        ASSERT(funding->expires_unix == 0);
        ASSERT(!vcs_zcode_patronage_projection_first_failure(
                   first, failure_root, &failure_reason));
        uint8_t first_root[32], second_root[32];
        ASSERT(vcs_zcode_patronage_projection_root(first, first_root));
        ASSERT(vcs_zcode_patronage_projection_root(second, second_root));
        ASSERT(memcmp(first_root, second_root, 32) == 0);
        vcs_zcode_patronage_projection_free(second);
        vcs_zcode_patronage_projection_free(first);

        /* A misaddressed object is an integrity failure, never an entry. */
        uint8_t funding_wire[VCS_ZCODE_PATRONAGE_FUNDING_WIRE_BYTES];
        ASSERT_EQ(vcs_zcode_patronage_funding_serialize(
                      &life.funding, funding_wire),
                  VCS_ZCODE_PATRONAGE_FUNDING_OK);
        uint8_t bogus_root[32];
        patronage_fill(bogus_root, 0x77);
        ASSERT(memcmp(bogus_root, life.funding_root, 32) != 0);
        ASSERT(vcs_object_put_addressed(life.workspace, bogus_root,
                                        funding_wire, sizeof(funding_wire)));
        struct vcs_zcode_patronage_projection *flagged =
            vcs_zcode_patronage_projection_build(&context);
        ASSERT(flagged);
        ASSERT(vcs_zcode_patronage_projection_count(flagged) == 2);
        ASSERT(vcs_zcode_patronage_projection_first_failure(
                   flagged, failure_root, &failure_reason));
        ASSERT(memcmp(failure_root, bogus_root, 32) == 0);
        ASSERT_STR_EQ(failure_reason, "funding-authority");
        vcs_zcode_patronage_projection_free(flagged);
        test_rm_rf(life.workspace);
        PASS();
    } _test_next:;
    return failures;
}

int test_zcode_patronage(void)
{
    return patronage_intent_gift_test() + patronage_funding_codec_test() +
           patronage_settlement_lifecycle_test() + patronage_projection_test();
}

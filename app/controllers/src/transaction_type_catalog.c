/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: render the canonical transaction-type catalog as typed JSON.
 */

#include "controllers/transaction_type_catalog.h"

#include "json/json.h"
#include "primitives/transaction.h"
#include "script/standard.h"

#include <stdio.h>
#include <string.h>

#define TX_TYPE(id_, family_, availability_, role_, encoding_, privacy_,       \
                lifecycle_, builder_, commit_, inspect_, components_, policy_, \
                lab_case_, proof_, test_, summary_)                            \
    { .id = id_, .family = family_, .availability = availability_,             \
      .transaction_role = role_, .chain_encoding = encoding_,                  \
      .privacy = privacy_, .lifecycle = lifecycle_,                            \
      .builder_command = builder_, .commit_command = commit_,                  \
      .inspect_command = inspect_, .component_commands_csv = components_,      \
      .network_policy = policy_, .lab_case_id = lab_case_,                     \
      .proof_level = proof_, .test_group = test_, .summary = summary_ },
static const struct zcl_transaction_type_contract k_transaction_types[] = {
#include "controllers/transaction_types.def"
};
#undef TX_TYPE

struct transaction_type_supplemental_tests {
    const char *id;
    const char *test_groups_csv;
};

#define TX_TYPE_SUPPLEMENTAL(id_, tests_) \
    { .id = id_, .test_groups_csv = tests_ },
static const struct transaction_type_supplemental_tests
k_supplemental_tests[] = {
#include "controllers/transaction_type_supplemental_tests.def"
};
#undef TX_TYPE_SUPPLEMENTAL

static void csv_json(const char *csv, struct json_value *out)
{
    json_set_array(out);
    if (!csv || !csv[0])
        return;
    const char *cursor = csv;
    while (*cursor) {
        const char *start = cursor;
        while (*cursor && *cursor != ',')
            cursor++;
        size_t len = (size_t)(cursor - start);
        if (len > 0 && len < 128) {
            char item_buf[128];
            memcpy(item_buf, start, len);
            item_buf[len] = '\0';
            struct json_value item;
            json_init(&item);
            json_set_str(&item, item_buf);
            (void)json_push_back(out, &item);
            json_free(&item);
        }
        if (*cursor == ',')
            cursor++;
    }
}

static const char *supplemental_tests_for(const char *id)
{
    if (!id)
        return "";
    for (size_t i = 0;
         i < sizeof(k_supplemental_tests) / sizeof(k_supplemental_tests[0]);
         i++) {
        if (strcmp(k_supplemental_tests[i].id, id) == 0)
            return k_supplemental_tests[i].test_groups_csv;
    }
    return "";
}

#define PROOF_GROUP_MAX 64
#define PROOF_GROUP_LEN 64

static void proof_group_add_csv(const char *csv,
                                char groups[PROOF_GROUP_MAX][PROOF_GROUP_LEN],
                                size_t *count)
{
    if (!csv || !count)
        return;
    const char *cursor = csv;
    while (*cursor && *count < PROOF_GROUP_MAX) {
        const char *start = cursor;
        while (*cursor && *cursor != ',')
            cursor++;
        size_t len = (size_t)(cursor - start);
        if (len > 0 && len < PROOF_GROUP_LEN) {
            bool duplicate = false;
            for (size_t i = 0; i < *count; i++) {
                if (strlen(groups[i]) == len &&
                    memcmp(groups[i], start, len) == 0) {
                    duplicate = true;
                    break;
                }
            }
            if (!duplicate) {
                memcpy(groups[*count], start, len);
                groups[*count][len] = '\0';
                (*count)++;
            }
        }
        if (*cursor == ',')
            cursor++;
    }
}

const struct zcl_transaction_type_contract *
zcl_transaction_type_catalog(size_t *count)
{
    if (count)
        *count = sizeof(k_transaction_types) / sizeof(k_transaction_types[0]);
    return k_transaction_types;
}

const struct zcl_transaction_type_contract *
zcl_transaction_type_find(const char *id)
{
    size_t count = 0;
    const struct zcl_transaction_type_contract *catalog =
        zcl_transaction_type_catalog(&count);
    if (!id || !id[0])
        return NULL;
    for (size_t i = 0; i < count; i++)
        if (strcmp(catalog[i].id, id) == 0)
            return &catalog[i];
    return NULL;
}

static void transaction_type_json(
    const struct zcl_transaction_type_contract *type, struct json_value *out)
{
    const bool demonstrated = strcmp(type->proof_level,
                                     "not_demonstrated") != 0;
    const bool mainnet_live_proven = strcmp(type->proof_level,
                                            "live_confirmed") == 0;

    json_set_object(out);
    (void)json_push_kv_str(out, "schema", ZCL_TRANSACTION_TYPE_SCHEMA);
    (void)json_push_kv_str(out, "id", type->id);
    (void)json_push_kv_str(out, "family", type->family);
    (void)json_push_kv_str(out, "availability", type->availability);
    (void)json_push_kv_str(out, "transaction_role", type->transaction_role);
    (void)json_push_kv_str(out, "chain_encoding", type->chain_encoding);
    (void)json_push_kv_str(out, "privacy", type->privacy);
    (void)json_push_kv_str(out, "lifecycle", type->lifecycle);
    (void)json_push_kv_str(out, "builder_command", type->builder_command);
    (void)json_push_kv_str(out, "commit_command", type->commit_command);
    (void)json_push_kv_str(out, "inspect_command", type->inspect_command);
    struct json_value components;
    json_init(&components);
    csv_json(type->component_commands_csv, &components);
    (void)json_push_kv(out, "component_commands", &components);
    json_free(&components);
    (void)json_push_kv_str(out, "network_policy", type->network_policy);
    (void)json_push_kv_str(out, "lab_case_id", type->lab_case_id);
    (void)json_push_kv_str(out, "proof_level", type->proof_level);
    (void)json_push_kv_str(out, "test_group", type->test_group);
    struct json_value supplemental_tests;
    json_init(&supplemental_tests);
    csv_json(supplemental_tests_for(type->id), &supplemental_tests);
    (void)json_push_kv(out, "supplemental_test_groups", &supplemental_tests);
    json_free(&supplemental_tests);
    (void)json_push_kv_str(out, "evidence_status",
                          demonstrated ? "demonstrated" : "blocked");
    (void)json_push_kv_bool(out, "mainnet_live_proven",
                           mainnet_live_proven);
    (void)json_push_kv_str(out, "summary", type->summary);
}

/* Collection rows stay below the native 8 KiB list budget.  Full workflow,
 * encoding, component, privacy, and safety detail belongs to the member
 * resource; this is progressive disclosure, not a second contract. */
static void transaction_type_summary_json(
    const struct zcl_transaction_type_contract *type, struct json_value *out)
{
    json_set_object(out);
    (void)json_push_kv_str(out, "id", type->id);
    (void)json_push_kv_str(out, "family", type->family);
    (void)json_push_kv_str(out, "availability", type->availability);
    (void)json_push_kv_str(out, "network_policy", type->network_policy);
    (void)json_push_kv_str(out, "builder_command", type->builder_command);
    (void)json_push_kv_str(out, "proof_level", type->proof_level);
}

bool zcl_transaction_types_index_json(struct json_value *out)
{
    if (!out)
        return false;
    json_set_object(out);
    (void)json_push_kv_str(out, "schema", ZCL_TRANSACTION_TYPES_INDEX_SCHEMA);
    (void)json_push_kv_str(out, "consensus_boundary",
        "catalogs_transactions_valid_under_legacy_zclassic_consensus_only");
    (void)json_push_kv_str(out, "authority",
        "discovery_only_catalog_grants_no_wallet_or_broadcast_authority");
    (void)json_push_kv_str(out, "agent_workflow",
        "select_type_then_discover_schema_then_plan_then_owner_authorized_commit_then_inspect_txid");
    (void)json_push_kv_str(out, "live_proof_source",
        "docs/work/transaction-lab-events.jsonl");
    (void)json_push_kv_str(out, "wire_catalog_command",
        "app.transaction-types.wire");

    struct json_value types;
    json_init(&types);
    json_set_array(&types);
    size_t count = 0;
    size_t ready = 0, process_only = 0, contained = 0, planned = 0;
    size_t demonstrated = 0, blocked = 0, chain_confirmed = 0;
    size_t mainnet_live_proven = 0, proof_test_groups = 0;
    char proof_groups[PROOF_GROUP_MAX][PROOF_GROUP_LEN] = {{0}};
    const struct zcl_transaction_type_contract *catalog =
        zcl_transaction_type_catalog(&count);
    for (size_t i = 0; i < count; i++) {
        struct json_value item;
        json_init(&item);
        transaction_type_summary_json(&catalog[i], &item);
        (void)json_push_back(&types, &item);
        json_free(&item);
        if (strcmp(catalog[i].availability, "ready") == 0) ready++;
        else if (strcmp(catalog[i].availability, "process_only") == 0)
            process_only++;
        else if (strcmp(catalog[i].availability, "contained") == 0)
            contained++;
        else if (strcmp(catalog[i].availability, "planned") == 0)
            planned++;
        if (strcmp(catalog[i].proof_level, "not_demonstrated") == 0)
            blocked++;
        else
            demonstrated++;
        if (strcmp(catalog[i].proof_level, "simnet_confirmed") == 0 ||
            strcmp(catalog[i].proof_level, "live_confirmed") == 0)
            chain_confirmed++;
        if (strcmp(catalog[i].proof_level, "live_confirmed") == 0)
            mainnet_live_proven++;
        proof_group_add_csv(catalog[i].test_group, proof_groups,
                            &proof_test_groups);
        proof_group_add_csv(supplemental_tests_for(catalog[i].id),
                            proof_groups, &proof_test_groups);
    }
    (void)json_push_kv_int(out, "transaction_type_count", (int64_t)count);
    (void)json_push_kv_int(out, "ready_count", (int64_t)ready);
    (void)json_push_kv_int(out, "process_only_count", (int64_t)process_only);
    (void)json_push_kv_int(out, "contained_count", (int64_t)contained);
    (void)json_push_kv_int(out, "planned_count", (int64_t)planned);
    (void)json_push_kv_int(out, "demonstrated_count",
                           (int64_t)demonstrated);
    (void)json_push_kv_int(out, "blocked_count", (int64_t)blocked);
    (void)json_push_kv_int(out, "chain_confirmed_count",
                           (int64_t)chain_confirmed);
    (void)json_push_kv_int(out, "mainnet_live_proven_count",
                           (int64_t)mainnet_live_proven);
    (void)json_push_kv_int(out, "proof_test_group_count",
                           (int64_t)proof_test_groups);
    (void)json_push_kv_bool(out, "fully_demonstrated", blocked == 0);
    (void)json_push_kv(out, "transaction_types", &types);
    json_free(&types);
    return true;
}

bool zcl_transaction_type_show_json(const char *id, struct json_value *out)
{
    const struct zcl_transaction_type_contract *type =
        zcl_transaction_type_find(id);
    if (!out || !type)
        return false;
    transaction_type_json(type, out);
    return true;
}

struct transaction_wire_family {
    const char *id;
    int version;
    bool overwintered;
    uint32_t version_group_id;
    const char *active_epoch;
    bool has_expiry_height;
    const char *fields_csv;
    const char *sprout_proof;
};

static const struct transaction_wire_family k_wire_families[] = {
    { "legacy_v1", 1, false, 0, "pre_overwinter", false,
      "transparent_inputs,transparent_outputs,lock_time", "none" },
    { "legacy_v2", 2, false, 0, "pre_overwinter", false,
      "transparent_inputs,transparent_outputs,lock_time,sprout_joinsplits,joinsplit_signature",
      "phgr13" },
    { "overwinter_v3", OVERWINTER_TX_VERSION, true,
      OVERWINTER_VERSION_GROUP_ID, "overwinter_before_sapling", true,
      "transparent_inputs,transparent_outputs,lock_time,expiry_height,sprout_joinsplits,joinsplit_signature",
      "phgr13" },
    { "sapling_v4", SAPLING_TX_VERSION, true, SAPLING_VERSION_GROUP_ID,
      "sapling_and_later", true,
      "transparent_inputs,transparent_outputs,lock_time,expiry_height,value_balance,sapling_spends,sapling_outputs,sprout_joinsplits,joinsplit_signature,binding_signature",
      "groth16" },
};

struct transaction_script_class {
    enum txnouttype type;
    bool standard_relay_class;
    const char *spendability;
    const char *destination_shape;
};

static const struct transaction_script_class k_script_classes[] = {
    { TX_NONSTANDARD, false, "script_dependent", "script_dependent" },
    { TX_PUBKEY,      true,  "spendable",        "single" },
    { TX_PUBKEYHASH,  true,  "spendable",        "single" },
    { TX_SCRIPTHASH,  true,  "spendable",        "single" },
    { TX_MULTISIG,    true,  "spendable",        "multiple" },
    { TX_NULL_DATA,   true,  "provably_unspendable", "none" },
};

struct transaction_application_codec {
    const char *id;
    const char *carrier;
    const char *tag;
    const char *coverage;
};

static const struct transaction_application_codec k_application_codecs[] = {
    { "zslp", "op_return", "SLP\\0", "semantic_catalog" },
    { "znam", "op_return", "ZNAM", "semantic_catalog" },
    { "zid_anchor", "op_return", "ZID\\0", "semantic_catalog" },
    { "zdir", "op_return", "ZDIR", "semantic_catalog" },
    { "zanc_and_zcode", "op_return", "ZANC", "semantic_catalog" },
    { "zblg", "op_return", "ZBLG", "semantic_catalog" },
    { "zmsg", "sapling_memo", "ZMSG", "semantic_catalog" },
    { "zpay", "sapling_memo", "ZPAY", "codec_only_no_typed_chain_workflow" },
    { "commerce", "transparent_or_sapling", "application_bound",
      "semantic_catalog" },
};

static void wire_family_json(const struct transaction_wire_family *family,
                             struct json_value *out)
{
    json_set_object(out);
    (void)json_push_kv_str(out, "id", family->id);
    (void)json_push_kv_int(out, "version", family->version);
    (void)json_push_kv_bool(out, "overwintered", family->overwintered);
    char group_id[11] = "none";
    if (family->overwintered)
        snprintf(group_id, sizeof(group_id), "0x%08x",
                 family->version_group_id);
    (void)json_push_kv_str(out, "version_group_id", group_id);
    (void)json_push_kv_str(out, "active_epoch", family->active_epoch);
    (void)json_push_kv_bool(out, "has_expiry_height",
                            family->has_expiry_height);
    struct json_value fields;
    json_init(&fields);
    csv_json(family->fields_csv, &fields);
    (void)json_push_kv(out, "serialized_fields", &fields);
    json_free(&fields);
    (void)json_push_kv_str(out, "sprout_proof", family->sprout_proof);
}

bool zcl_transaction_wire_catalog_json(struct json_value *out)
{
    if (!out)
        return false;
    json_set_object(out);
    (void)json_push_kv_str(out, "schema",
                           ZCL_TRANSACTION_WIRE_CATALOG_SCHEMA);
    (void)json_push_kv_str(out, "authority",
        "source_derived_discovery_only_no_wallet_or_broadcast_authority");
    (void)json_push_kv_str(out, "semantic_catalog_command",
                           "app.transaction-types.list");
    (void)json_push_kv_str(out, "raw_inspection_command",
                           "core.chain.transaction.get");
    (void)json_push_kv_bool(out, "consensus_wire_families_are_finite", true);
    (void)json_push_kv_bool(out, "application_semantics_are_open_ended", true);
    (void)json_push_kv_str(out, "open_ended_reason",
        "consensus_accepts_arbitrary_scripts_opaque_sapling_memos_and_unknown_future_op_return_tags");

    struct json_value families;
    json_init(&families);
    json_set_array(&families);
    for (size_t i = 0; i < sizeof(k_wire_families) /
                                sizeof(k_wire_families[0]); i++) {
        struct json_value item;
        json_init(&item);
        wire_family_json(&k_wire_families[i], &item);
        (void)json_push_back(&families, &item);
        json_free(&item);
    }
    (void)json_push_kv_int(out, "wire_family_count",
        (int64_t)(sizeof(k_wire_families) / sizeof(k_wire_families[0])));
    (void)json_push_kv(out, "wire_families", &families);
    json_free(&families);

    struct json_value scripts;
    json_init(&scripts);
    json_set_array(&scripts);
    for (size_t i = 0; i < sizeof(k_script_classes) /
                                sizeof(k_script_classes[0]); i++) {
        struct json_value item;
        json_init(&item);
        json_set_object(&item);
        (void)json_push_kv_str(&item, "id",
                               get_txn_output_type(k_script_classes[i].type));
        (void)json_push_kv_int(&item, "enum_value",
                               (int64_t)k_script_classes[i].type);
        (void)json_push_kv_bool(&item, "standard_relay_class",
                                k_script_classes[i].standard_relay_class);
        (void)json_push_kv_str(&item, "spendability",
                               k_script_classes[i].spendability);
        (void)json_push_kv_str(&item, "destination_shape",
                               k_script_classes[i].destination_shape);
        (void)json_push_back(&scripts, &item);
        json_free(&item);
    }
    (void)json_push_kv_int(out, "script_class_count",
        (int64_t)(sizeof(k_script_classes) / sizeof(k_script_classes[0])));
    (void)json_push_kv(out, "script_classes", &scripts);
    json_free(&scripts);
    (void)json_push_kv_str(out, "nonstandard_script_policy",
        "process_if_consensus_valid_but_do_not_infer_relay_or_builder_support");
    (void)json_push_kv_str(out, "unknown_op_return_policy",
        "index_tag_and_payload_digest_without_inventing_semantics");
    (void)json_push_kv_str(out, "sapling_memo_policy",
        "consensus_opaque_512_bytes_decode_only_with_an_explicit_application_codec");

    struct json_value codecs;
    json_init(&codecs);
    json_set_array(&codecs);
    for (size_t i = 0; i < sizeof(k_application_codecs) /
                                sizeof(k_application_codecs[0]); i++) {
        struct json_value item;
        json_init(&item);
        json_set_object(&item);
        (void)json_push_kv_str(&item, "id", k_application_codecs[i].id);
        (void)json_push_kv_str(&item, "carrier",
                               k_application_codecs[i].carrier);
        (void)json_push_kv_str(&item, "tag", k_application_codecs[i].tag);
        (void)json_push_kv_str(&item, "coverage",
                               k_application_codecs[i].coverage);
        (void)json_push_back(&codecs, &item);
        json_free(&item);
    }
    (void)json_push_kv_int(out, "recognized_application_codec_count",
        (int64_t)(sizeof(k_application_codecs) /
                  sizeof(k_application_codecs[0])));
    (void)json_push_kv(out, "recognized_application_codecs", &codecs);
    json_free(&codecs);
    return true;
}

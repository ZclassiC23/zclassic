/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: render the canonical transaction-type catalog as typed JSON.
 */

#include "controllers/transaction_type_catalog.h"

#include "json/json.h"

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

    struct json_value types;
    json_init(&types);
    json_set_array(&types);
    size_t count = 0;
    size_t ready = 0, process_only = 0, contained = 0, planned = 0;
    size_t demonstrated = 0, blocked = 0, chain_confirmed = 0;
    size_t mainnet_live_proven = 0, proof_test_groups = 0;
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
        bool first_test_group = true;
        for (size_t j = 0; j < i; j++) {
            if (strcmp(catalog[i].test_group, catalog[j].test_group) == 0) {
                first_test_group = false;
                break;
            }
        }
        if (first_test_group)
            proof_test_groups++;
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

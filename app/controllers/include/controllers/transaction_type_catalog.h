/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: public contract for the semantic ZCL transaction-type catalog.
 */

#ifndef ZCL_CONTROLLERS_TRANSACTION_TYPE_CATALOG_H
#define ZCL_CONTROLLERS_TRANSACTION_TYPE_CATALOG_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZCL_TRANSACTION_TYPE_SCHEMA "zcl.transaction_type.v1"
#define ZCL_TRANSACTION_TYPES_INDEX_SCHEMA "zcl.transaction_types.index.v1"

struct json_value;

/* One semantic transaction shape, not one CLI alias.  A composite row names
 * every command needed to reach its chain transaction.  This is discovery
 * data only: it grants no wallet, node, or broadcast authority. */
struct zcl_transaction_type_contract {
    const char *id;
    const char *family;
    const char *availability;
    const char *transaction_role;
    const char *chain_encoding;
    const char *privacy;
    const char *lifecycle;
    const char *builder_command;
    const char *commit_command;
    const char *inspect_command;
    const char *component_commands_csv;
    const char *network_policy;
    const char *lab_case_id;
    const char *proof_level;
    const char *test_group;
    const char *summary;
};

const struct zcl_transaction_type_contract *
zcl_transaction_type_catalog(size_t *count);
const struct zcl_transaction_type_contract *
zcl_transaction_type_find(const char *id);
bool zcl_transaction_types_index_json(struct json_value *out);
bool zcl_transaction_type_show_json(const char *id, struct json_value *out);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_CONTROLLERS_TRANSACTION_TYPE_CATALOG_H */

/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Public API for the canonical native test group catalog. */

#ifndef ZCL_TEST_GROUP_CATALOG_H
#define ZCL_TEST_GROUP_CATALOG_H

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define ZCL_TEST_GROUP_FULL_MAX 96

/* Canonical registry order, shared with test_parallel's dispatch table. */
size_t zcl_test_group_catalog_count(void);
const char *zcl_test_group_catalog_at(size_t index);
bool zcl_test_group_catalog_contains(const char *full_id);

/* True only for exact catalog groups whose wall-clock assertions must run
 * before the worker pool starts. Repository-exclusive lint groups remain a
 * separate runner policy. */
bool zcl_test_group_requires_exclusive_run(const char *full_id);

/* True only for a mechanically audited test translation-unit proof leaf. */
bool zcl_test_group_source_is_semantic_leaf(const char *path);

/* Resolve a full or legacy prefixless ID to exactly one canonical full ID.
 * Substrings are never accepted. */
bool zcl_test_group_resolve_exact(
    const char *id, char out[ZCL_TEST_GROUP_FULL_MAX]);

/* True when `full_id` belongs to the validated plan ID's preserved legacy
 * family (substring union plus declared semantic families). The exact primary
 * must exist or this returns false. */
bool zcl_test_group_plan_selects(const char *plan_id, const char *full_id);

/* Expand plan IDs to a deterministic, deduplicated exact execution set in
 * canonical registry order. Returns the total selected count. When total is
 * greater than cap, the first cap rows are retained and *truncated is true. A
 * missing/ambiguous primary fails closed and returns SIZE_MAX. */
size_t zcl_test_group_expand_plan(
    const char *const *plan_ids, size_t plan_count,
    char (*out)[ZCL_TEST_GROUP_FULL_MAX], size_t cap, bool *truncated);

/* Integration-only groups remain in the complete expansion above but are
 * excluded from the bounded save-cycle expansion below. The predicate accepts
 * exact canonical IDs only. */
bool zcl_test_group_is_integration_only(const char *full_id);
bool zcl_test_group_integration_policy_valid(void);
size_t zcl_test_group_expand_plan_immediate(
    const char *const *plan_ids, size_t plan_count,
    char (*out)[ZCL_TEST_GROUP_FULL_MAX], size_t cap, bool *truncated);

#ifdef __cplusplus
}
#endif

#endif /* ZCL_TEST_GROUP_CATALOG_H */

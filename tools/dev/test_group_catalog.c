/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Resolve canonical test IDs and exact proof execution sets. */

#include "test_group_catalog.h"

#include <fnmatch.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static const char *const g_test_groups[] = {
#define ZCL_TEST_GROUP(name) "test_" #name,
#define ZCL_SPEC_GROUP(name) "spec_" #name,
#include "test_group_catalog.def"
#undef ZCL_SPEC_GROUP
#undef ZCL_TEST_GROUP
};

static const char *const g_semantic_leaf_sources[] = {
#define ZCL_TEST_SEMANTIC_LEAF(name) "lib/test/src/test_" #name ".c",
#include "test_semantic_leaves.def"
#undef ZCL_TEST_SEMANTIC_LEAF
    NULL, /* Zero declared leaves is the safe, valid state. */
};

struct proof_family {
    const char *plan_id;
    const char *full_id_glob;
};

static const struct proof_family g_proof_families[] = {
#define ZCL_TEST_PROOF_FAMILY(plan_id_, glob_) {plan_id_, glob_},
#include "test_proof_families.def"
#undef ZCL_TEST_PROOF_FAMILY
};

size_t zcl_test_group_catalog_count(void)
{
    return sizeof(g_test_groups) / sizeof(g_test_groups[0]);
}

const char *zcl_test_group_catalog_at(size_t index)
{
    return index < zcl_test_group_catalog_count() ? g_test_groups[index] : NULL;
}

bool zcl_test_group_catalog_contains(const char *full_id)
{
    if (!full_id || !full_id[0])
        return false;
    for (size_t i = 0; i < zcl_test_group_catalog_count(); i++)
        if (strcmp(g_test_groups[i], full_id) == 0)
            return true;
    return false;
}

bool zcl_test_group_source_is_semantic_leaf(const char *path)
{
    if (!path || !path[0])
        return false;
    for (size_t i = 0; g_semantic_leaf_sources[i] != NULL; i++)
        if (strcmp(g_semantic_leaf_sources[i], path) == 0)
            return true;
    return false;
}

bool zcl_test_group_resolve_exact(
    const char *id, char out[ZCL_TEST_GROUP_FULL_MAX])
{
    if (!id || !id[0] || !out)
        return false;
    const char *hit = NULL;
    size_t hits = 0;
    char test_id[ZCL_TEST_GROUP_FULL_MAX];
    char spec_id[ZCL_TEST_GROUP_FULL_MAX];
    int tn = snprintf(test_id, sizeof(test_id), "test_%s", id);
    int sn = snprintf(spec_id, sizeof(spec_id), "spec_%s", id);
    for (size_t i = 0; i < zcl_test_group_catalog_count(); i++) {
        const char *full = g_test_groups[i];
        if (strcmp(full, id) == 0 ||
            (tn > 0 && (size_t)tn < sizeof(test_id) &&
             strcmp(full, test_id) == 0) ||
            (sn > 0 && (size_t)sn < sizeof(spec_id) &&
             strcmp(full, spec_id) == 0)) {
            hit = full;
            hits++;
        }
    }
    if (hits != 1 || strlen(hit) >= ZCL_TEST_GROUP_FULL_MAX)
        return false;
    snprintf(out, ZCL_TEST_GROUP_FULL_MAX, "%s", hit);
    return true;
}

static bool declared_family_selects(const char *plan_id, const char *full_id)
{
    for (size_t i = 0; i < sizeof(g_proof_families) /
                            sizeof(g_proof_families[0]); i++) {
        if (strcmp(g_proof_families[i].plan_id, plan_id) == 0 &&
            fnmatch(g_proof_families[i].full_id_glob, full_id, 0) == 0)
            return true;
    }
    return false;
}

bool zcl_test_group_plan_selects(const char *plan_id, const char *full_id)
{
    char primary[ZCL_TEST_GROUP_FULL_MAX];
    if (!plan_id || !full_id ||
        !zcl_test_group_resolve_exact(plan_id, primary) ||
        !zcl_test_group_catalog_contains(full_id))
        return false;
    return strstr(full_id, plan_id) != NULL ||
           declared_family_selects(plan_id, full_id);
}

size_t zcl_test_group_expand_plan(
    const char *const *plan_ids, size_t plan_count,
    char (*out)[ZCL_TEST_GROUP_FULL_MAX], size_t cap, bool *truncated)
{
    if (truncated)
        *truncated = false;
    if ((!plan_ids && plan_count > 0) || (!out && cap > 0) || !truncated)
        return SIZE_MAX;
    for (size_t p = 0; p < plan_count; p++) {
        char primary[ZCL_TEST_GROUP_FULL_MAX];
        if (!zcl_test_group_resolve_exact(plan_ids[p], primary))
            return SIZE_MAX;
    }
    size_t total = 0;
    for (size_t i = 0; i < zcl_test_group_catalog_count(); i++) {
        bool selected = false;
        for (size_t p = 0; p < plan_count; p++) {
            if (zcl_test_group_plan_selects(plan_ids[p], g_test_groups[i])) {
                selected = true;
                break;
            }
        }
        if (!selected)
            continue;
        if (total < cap)
            snprintf(out[total], ZCL_TEST_GROUP_FULL_MAX, "%s", g_test_groups[i]);
        total++;
    }
    *truncated = total > cap;
    return total;
}

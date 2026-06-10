/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "jobs/stage_anchor.h"

#include "jobs/stage_helpers.h"
#include "storage/coins_kv.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"
#include "util/stage.h"

#include <stddef.h>
#include <string.h>

static bool anchor_target_for_stage(sqlite3 *db, const char *stage,
                                    uint64_t requested, const char *tag,
                                    const char *reason, uint64_t *out)
{
    if (!out)
        return false;
    *out = requested;
    if (!stage || strcmp(stage, "utxo_apply") != 0)
        return true;

    if (!progress_meta_table_ensure(db)) {
        LOG_WARN(tag,
                 "[%s] anchor upstream cursor failed "
                 "stage=utxo_apply reason=coins_frontier_schema reason=%s",
                 tag, reason ? reason : "");
        return false;
    }

    int32_t applied = 0;
    bool found = false;
    if (!coins_kv_get_applied_height(db, &applied, &found)) {
        LOG_WARN(tag,
                 "[%s] anchor upstream cursor failed "
                 "stage=utxo_apply reason=coins_frontier_read reason=%s",
                 tag, reason ? reason : "");
        return false;
    }
    if (!found)
        return true; /* Snapshot/import bootstrap may predate the frontier key. */
    if (applied < 0) {
        LOG_WARN(tag,
                 "[%s] anchor upstream cursor failed "
                 "stage=utxo_apply malformed_coins_frontier=%d reason=%s",
                 tag, applied, reason ? reason : "");
        return false;
    }
    if (requested > (uint64_t)applied) {
        *out = (uint64_t)applied;
        LOG_WARN(tag,
                 "[%s] anchor upstream cursor capped "
                 "stage=utxo_apply requested=%llu coins_applied=%d reason=%s",
                 tag, (unsigned long long)requested, applied,
                 reason ? reason : "");
    }
    return true;
}

bool stage_anchor_upstream_cursors_to(sqlite3 *db, uint64_t target,
                                      const char *owner,
                                      const char *reason)
{
    if (!db)
        return false;
    static const char *const upstream[] = {
        "header_admit",
        "validate_headers",
        "body_fetch",
        "body_persist",
        "script_validate",
        "proof_validate",
        "utxo_apply",
    };
    const char *tag = owner && owner[0] ? owner : "stage_anchor";

    for (size_t i = 0; i < sizeof(upstream) / sizeof(upstream[0]); i++) {
        uint64_t stage_target = target;
        if (!anchor_target_for_stage(db, upstream[i], target, tag, reason,
                                     &stage_target))
            return false;
        uint64_t before = stage_cursor_persisted(db, upstream[i], tag);
        if (!stage_set_named_cursor_if_behind(db, upstream[i], stage_target)) {
            LOG_WARN(tag,
                     "[%s] anchor upstream cursor failed "
                     "stage=%s from=%llu to=%llu reason=%s",
                     tag, upstream[i], (unsigned long long)before,
                     (unsigned long long)stage_target, reason ? reason : "");
            return false;
        }
        if (before < stage_target) {
            LOG_INFO(tag,
                     "[%s] anchor upstream cursor stage=%s "
                     "from=%llu to=%llu reason=%s",
                     tag, upstream[i], (unsigned long long)before,
                     (unsigned long long)stage_target, reason ? reason : "");
        }
    }
    return true;
}

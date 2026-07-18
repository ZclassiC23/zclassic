/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * shielded_history_import_bind_guard — the fail-closed bind-height guard for
 * the complete shielded-history import. The contract lives in
 * services/shielded_history_import_service.h; in short:
 *
 * The import keys its single frontier row at the SOURCE chainstate's
 * persisted best block, while the reducer's fold resumes at the TARGET
 * datadir's coins authority (fold_resume = coins_applied_height, so the
 * frontier must BE the tree state at fold_resume - 1 == the coins island
 * root). A zclassicd whose on-disk chainstate lags its live tip (real case:
 * it had stopped flushing its block DB for ~2 weeks) would otherwise
 * manufacture a datadir whose shielded frontier is height-mismatched — the
 * fold then hard-wedges at the first Sapling-commitment block above the
 * island (fold_sapling appends to the stale tree and mismatches the
 * header-committed hashFinalSaplingRoot; utxo_apply.apply_failed, H*
 * pinned), deterministically.
 *
 * The guard therefore REFUSES any bind where
 * `tip_height != fold_resume_height - 1` (the coins island root as derived
 * by reducer_frontier_derive_coins_best), BEFORE the import transaction
 * opens, so a refusal commits nothing. A target with NO coins authority yet
 * (fresh datadir — shielded-first ordering) passes silently.
 *
 * Both callers share this one predicate: the -import-complete-shielded verb
 * (src/main.c, terminal-visible refusal with both heights + the remedy) and
 * shielded_history_import_from_chainstate itself (defense in depth — a
 * caller bypassing the verb cannot manufacture the mismatch either). Split
 * out of shielded_history_import_service.c to keep that file under its
 * file-size ceiling.
 *
 * one-result-type-ok:owner-gated-boot-import — a pure bool predicate on the
 * same owner-gated boot/import surface as shielded_history_import_service.c
 * (which carries the same marker): the refusal reason travels via node.log
 * [shielded_import] (every refusal path LOG_RETURNs the exact heights) and
 * via *coins_best_out to the verb's terminal message; there is no
 * zcl_result-returning runtime surface to thread. */
// one-result-type-ok:owner-gated-boot-import

#include "services/shielded_history_import_service.h"

#include "jobs/reducer_frontier.h"
#include "util/log_macros.h"

#include <sqlite3.h>
#include <stdint.h>

#define SHI_BIND_GUARD_SUBSYS "shielded_import"

bool shielded_history_import_bind_guard_probe(sqlite3 *progress_db,
                                              int64_t tip_height,
                                              int32_t *coins_best_out)
{
    if (coins_best_out)
        *coins_best_out = -1;
    if (!progress_db || tip_height < 0)
        LOG_RETURN(false, SHI_BIND_GUARD_SUBSYS, "bind guard: invalid args");

    int32_t coins_best = -1;
    uint8_t cb_hash[32];
    bool cb_hash_found = false, cb_found = false;
    if (!reducer_frontier_derive_coins_best(progress_db, &coins_best, cb_hash,
                                            &cb_hash_found, &cb_found))
        LOG_RETURN(false, SHI_BIND_GUARD_SUBSYS,
                   "bind guard: coins-best derive failed (progress.kv read "
                   "error) — refusing rather than guessing the fold-resume "
                   "anchor");
    if (!cb_found)
        return true;   /* no coins authority yet — fresh datadir, legal */
    if (coins_best_out)
        *coins_best_out = coins_best;
    if (tip_height == (int64_t)coins_best)
        return true;
    LOG_RETURN(false, SHI_BIND_GUARD_SUBSYS,
               "bind guard REFUSAL: tip bind h=%lld != fold-resume anchor "
               "h=%lld (coins island root) — importing would key the shielded "
               "frontier %lld block(s) off the resume point and the fold "
               "would hard-wedge at the first Sapling-commitment block above "
               "the island (hashFinalSaplingRoot mismatch). Remedy: re-run "
               "against a consistent source whose chainstate best block == "
               "the island root (h=%lld). Refusing; nothing committed.",
               (long long)tip_height, (long long)coins_best,
               (long long)(tip_height - (int64_t)coins_best),
               (long long)coins_best);
}

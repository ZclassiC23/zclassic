// one-result-type-ok:bounded-backfill-progress-counter
/* E2 override: this module's public surface is a bounded best-effort
 * background walk (op_return_backfill_run_once returns the count of
 * blocks folded this batch; 0 is a normal "nothing to do yet" state, not
 * a failure) plus registration/JSON-dump helpers, same shape as the
 * sibling authority_projection_audit / invariant_sentinel sweeps. A
 * per-height read/save failure is already fail-loud via LOG_WARN and
 * simply stops the batch for a retry next tick (see op_return_backfill_
 * run_once) — a zcl_result on the outer entry points would duplicate
 * that channel with a code/message callers must not branch on. */
// repair-rung-ok:test_op_return_index
// TENACITY I3: this is NOT a consensus-state repair rung — op_return_index
// is a rebuildable, non-consensus PROJECTION (never consulted by
// utxo_apply/consensus), and this service only ever POPULATES it forward
// from already-validated, already-persisted block bodies (never patches a
// torn write). test_op_return_index's backfill-e2e case proves a
// truncate + fresh re-derive reproduces the exact same row set and
// running digest, i.e. there is no "bad state" here to repair — the same
// populate-only shape as nullifier_backfill_service.c.

/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * op_return_backfill_service — see services/op_return_backfill_service.h. */

#include "services/op_return_backfill_service.h"

#include "chain/chain.h"
#include "config/runtime.h"
#include "encoding/utilstrencodings.h"
#include "jobs/reducer_frontier.h"
#include "json/json.h"
#include "models/database.h"
#include "models/op_return_index.h"
#include "primitives/block.h"
#include "storage/disk_block_io.h"
#include "supervisors/domains.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"
#include "util/supervisor.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"

#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>

static _Atomic uint64_t g_backfill_ticks          = 0;
static _Atomic uint64_t g_backfill_blocks_folded  = 0;
static _Atomic uint64_t g_backfill_rows_seen      = 0;
static _Atomic uint64_t g_backfill_holes          = 0;
static _Atomic uint64_t g_backfill_oversize_blocks = 0;
static _Atomic int64_t  g_backfill_last_height    = -1;

static const char *g_backfill_datadir = NULL; /* process-lifetime string */

#ifdef ZCL_TESTING
struct node_db *g_op_return_backfill_test_ndb;
struct main_state *g_op_return_backfill_test_ms;
const char *g_op_return_backfill_test_datadir;
#endif

void op_return_backfill_set_datadir(const char *datadir)
{
    g_backfill_datadir = datadir;
}

static struct node_db *backfill_ndb(void)
{
#ifdef ZCL_TESTING
    if (g_op_return_backfill_test_ndb) return g_op_return_backfill_test_ndb;
#endif
    return app_runtime_node_db();
}

static struct main_state *backfill_main_state(void)
{
#ifdef ZCL_TESTING
    if (g_op_return_backfill_test_ms) return g_op_return_backfill_test_ms;
#endif
    return app_runtime_main_state();
}

static const char *backfill_datadir(void)
{
#ifdef ZCL_TESTING
    if (g_op_return_backfill_test_datadir) return g_op_return_backfill_test_datadir;
#endif
    return g_backfill_datadir;
}

/* ── One bounded batch ────────────────────────────────────────────── */

int op_return_backfill_run_once(void)
{
    struct node_db *ndb = backfill_ndb();
    struct main_state *ms = backfill_main_state();
    const char *datadir = backfill_datadir();
    if (!ndb || !ndb->open || !ms || !datadir || !datadir[0])
        return 0; /* not wired yet (early boot / unit tests) — benign */

    int32_t cursor = -1;
    uint8_t digest[32];
    if (!op_return_index_get_cursor(ndb, &cursor, digest))
        return 0;

    int32_t hstar = reducer_frontier_provable_tip_cached();
    if (hstar < 0) hstar = 0;
    if (cursor >= hstar)
        return 0; /* fully caught up to the provable frontier */

    int32_t target = cursor + OP_RETURN_BACKFILL_BATCH_BLOCKS;
    if (target > hstar) target = hstar;

    struct op_return_index_row *rows = zcl_malloc(
        (size_t)OP_RETURN_BACKFILL_MAX_ROWS_PER_BLOCK * sizeof(*rows),
        "op_return_backfill/rows");
    if (!rows) {
        LOG_WARN("op_return_index", "backfill: rows buffer alloc failed");
        return 0;
    }

    int folded = 0;
    for (int32_t h = cursor + 1; h <= target; h++) {
        struct block_index *bi = active_chain_at(&ms->chain_active, h);
        if (!bi || !bi->phashBlock ||
            !(block_index_status_load(bi) & BLOCK_HAVE_DATA)) {
            atomic_fetch_add(&g_backfill_holes, 1);
            LOG_WARN("op_return_index",
                     "backfill: h=%d not readable yet (index/data missing) "
                     "— stopping this batch, retrying next tick", h);
            break;
        }

        struct block blk;
        block_init(&blk);
        if (!read_block_from_disk_index_pread(&blk, bi, datadir)) {
            block_free(&blk);
            atomic_fetch_add(&g_backfill_holes, 1);
            LOG_WARN("op_return_index",
                     "backfill: h=%d body unreadable — stopping this batch",
                     h);
            break;
        }

        size_t n = 0;
        (void)op_return_index_apply_block_rows(
            ndb, &blk, h, rows, OP_RETURN_BACKFILL_MAX_ROWS_PER_BLOCK, &n);
        atomic_fetch_add(&g_backfill_rows_seen, (uint64_t)n);

        if (n > OP_RETURN_BACKFILL_MAX_ROWS_PER_BLOCK) {
            /* Every row is already saved (apply_block_rows never truncates
             * the inserts) but the digest buffer above only holds the cap
             * — folding a partial set would silently diverge the chain
             * from a peer that saw the full set. Stop the batch loudly; a
             * real-chain block should never hit this. */
            atomic_fetch_add(&g_backfill_oversize_blocks, 1);
            LOG_WARN("op_return_index",
                     "backfill: h=%d has %zu OP_RETURN outputs > cap=%d — "
                     "digest fold SKIPPED, cursor will not advance past %d",
                     h, n, OP_RETURN_BACKFILL_MAX_ROWS_PER_BLOCK, h - 1);
            block_free(&blk);
            break;
        }

        uint8_t new_digest[32];
        op_return_index_fold_block_digest(digest, h, bi->phashBlock->data,
                                          rows, n, new_digest);
        if (!op_return_index_set_cursor(ndb, h, new_digest)) {
            block_free(&blk);
            LOG_WARN("op_return_index",
                     "backfill: cursor persist failed at h=%d", h);
            break;
        }
        memcpy(digest, new_digest, 32);
        block_free(&blk);
        folded++;
        atomic_store(&g_backfill_last_height, h);
    }

    free(rows);
    if (folded > 0)
        atomic_fetch_add(&g_backfill_blocks_folded, (uint64_t)folded);
    return folded;
}

/* ── Supervision (no dedicated thread — root supervisor drives on_tick) ── */

static struct liveness_contract g_backfill_contract;
static _Atomic supervisor_child_id g_backfill_id = SUPERVISOR_INVALID_ID;

static void backfill_tick(struct liveness_contract *c)
{
    (void)c;
    (void)op_return_backfill_run_once();
    atomic_fetch_add(&g_backfill_ticks, 1);
    supervisor_progress(atomic_load(&g_backfill_id),
                        (int64_t)atomic_load(&g_backfill_blocks_folded));
    supervisor_tick(atomic_load(&g_backfill_id));
}

void op_return_backfill_register(void)
{
    supervisor_domains_init();
    if (atomic_load(&g_backfill_id) != SUPERVISOR_INVALID_ID)
        return;
    liveness_contract_init(&g_backfill_contract, "chain.op_return_backfill");
    atomic_store(&g_backfill_contract.period_secs,
                (int64_t)OP_RETURN_BACKFILL_PERIOD_SECS);
    atomic_store(&g_backfill_contract.deadline_secs, (int64_t)0);
    atomic_store(&g_backfill_contract.progress_max_quiet_us, (int64_t)0);
    g_backfill_contract.on_tick = backfill_tick;
    g_backfill_contract.on_stall = NULL;
    supervisor_child_id id =
        supervisor_register_in_domain(g_chain_sup, &g_backfill_contract);
    atomic_store(&g_backfill_id, id);
    if (id == SUPERVISOR_INVALID_ID)
        LOG_WARN("op_return_index", "backfill: supervisor register failed");
}

void op_return_backfill_reset_for_test(void)
{
    atomic_store(&g_backfill_ticks, 0);
    atomic_store(&g_backfill_blocks_folded, 0);
    atomic_store(&g_backfill_rows_seen, 0);
    atomic_store(&g_backfill_holes, 0);
    atomic_store(&g_backfill_oversize_blocks, 0);
    atomic_store(&g_backfill_last_height, -1);
}

/* ── `zclassic23 dumpstate op_return_index` ─────────────────────────── */

bool op_return_index_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out) return false;
    json_set_object(out);

    json_push_kv_int(out, "ticks", (int64_t)atomic_load(&g_backfill_ticks));
    json_push_kv_int(out, "blocks_folded",
                     (int64_t)atomic_load(&g_backfill_blocks_folded));
    json_push_kv_int(out, "rows_seen",
                     (int64_t)atomic_load(&g_backfill_rows_seen));
    json_push_kv_int(out, "holes", (int64_t)atomic_load(&g_backfill_holes));
    json_push_kv_int(out, "oversize_blocks",
                     (int64_t)atomic_load(&g_backfill_oversize_blocks));
    json_push_kv_int(out, "last_folded_height",
                     atomic_load(&g_backfill_last_height));

    struct node_db *ndb = backfill_ndb();
    bool db_open = ndb && ndb->open;
    json_push_kv_bool(out, "wired", db_open);

    int32_t cursor = -1;
    uint8_t digest[32] = {0};
    bool have_cursor = db_open &&
                       op_return_index_get_cursor(ndb, &cursor, digest);
    json_push_kv_int(out, "cursor_height", have_cursor ? cursor : -1);
    char digest_hex[65] = {0};
    if (have_cursor) HexStr(digest, 32, false, digest_hex, sizeof(digest_hex));
    json_push_kv_str(out, "cursor_digest", digest_hex);

    int32_t hstar = reducer_frontier_provable_tip_cached();
    json_push_kv_int(out, "provable_tip", hstar);
    json_push_kv_int(out, "blocks_remaining",
        (have_cursor && hstar > cursor) ? (int64_t)(hstar - cursor) : 0);

    if (db_open) {
        int64_t total = op_return_index_count(ndb);
        int64_t znam = op_return_index_count_by_tag_text(ndb, "ZNAM");
        int64_t zslp = op_return_index_count_by_tag_text(ndb, "SLP");
        int64_t zanc = op_return_index_count_by_tag_text(ndb, "ZANC");
        int64_t known = znam + zslp + zanc;
        int64_t other = total - known;
        if (other < 0) other = 0;
        json_push_kv_int(out, "total_rows", total);
        json_push_kv_int(out, "znam_rows", znam);
        json_push_kv_int(out, "zslp_rows", zslp);
        json_push_kv_int(out, "zanc_rows", zanc);
        json_push_kv_int(out, "other_rows", other);
    }
    return true;
}

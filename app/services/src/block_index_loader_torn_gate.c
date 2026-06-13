/* Copyright 2026 Rhett Creighton - Apache License 2.0 */
// one-result-type-ok:torn-import-verdict-predicate — the single function here
// is a boolean VERDICT predicate (tear fires / does not). On a fire the
// actionable reason does NOT travel via a return code: it is published as a
// typed PERMANENT blocker ('seed.torn_import', owner='validation_pack') plus
// EV_OPERATOR_NEEDED naming the hole — exactly where an operator reads it. A
// struct zcl_result would bury that diagnostic in a discarded enum.
/*
 * Block Index Loader — (2a.5) BOOT-TIME TORN-IMPORT GATE.
 *
 * This file owns ONLY the durable forward-evidence torn-import verdict, kept
 * apart from the seed-loader plumbing in block_index_loader_rebuild.c so each
 * stays a readable, single-purpose TU. The seed loader calls
 * block_index_loader_torn_import_gate_fires() right before its forward-only
 * early-return.
 *
 * THE TEAR (live, 2026-06-13): a cold-import coin set matches the recorded
 * count yet is MISSING a canonical coinbase (60fc6f43a630b5b7:0 ~3,145,486).
 * The active chain spends it at the apply frontier, so forward validation
 * recorded a DURABLE `ok=0 prevout_unresolved` row at the SPENDING block
 * (3,145,595 == coins_applied_height), 138 blocks ABOVE seed H — persistent
 * proof the trusted base is torn.
 *
 * THE VERDICT FIRES ONLY WHEN ALL THREE HOLD (the durability guard):
 *   (1) hole_found && hole_h in (checkpoint, ceiling], ceiling = the
 *       FORWARD-APPLY frontier (coins_applied_height raised by the active
 *       chain height), NOT seed H — the live hole sits ABOVE H and a
 *       (checkpoint, H] window misses it.
 *   (2) hole_status is 'prevout_unresolved' — EXCLUDE 'internal_error' (and
 *       every other class). script_validate persists status='internal_error'
 *       DURABLY (ok=0) for TRANSIENT, RESURRECTABLE infra failures (sapling-
 *       verification-ctx-init-failed / error-computing-signature-hash;
 *       script_validate_contextual.c:55-58,112-116), which the stale-script-
 *       hole repair re-attempts. Firing a PERMANENT wipe verdict on those
 *       would false-reject a HEALTHY node that hit a transient blip and
 *       rebooted before re-validation. 'block_decode_failed' is a FUTURE
 *       class, deliberately NOT accepted here: coin_backfill returns
 *       NOT_APPLICABLE for any lowest hole whose status != 'prevout_unresolved'
 *       (stage_repair_coin_backfill.c backfill_run G2 short-circuit), so it
 *       never reaches a refuse()
 *       and never persists the (3) marker — accepting it would be a dead
 *       branch that LOOKS covered but can never fire. Before this gate may
 *       accept block_decode_failed, coin_backfill must (a) handle decode-failed
 *       holes and (b) durably persist a terminal marker for that class.
 *   (3) coin_backfill has DURABLY REFUSED this hole as unprovable: the
 *       progress.kv meta key 'coin_backfill.refused.<h>.<holehash-hex>' is
 *       present. coin_backfill (the RUNTIME reducer self-heal stage) writes
 *       this row from persist_terminal_refusal() at every TERMINAL
 *       REFUSED_UNPROVABLE/REFUSED_SPENT verdict that carries a bound hole
 *       hash — the live tear's txindex_miss path (resolve_creator ->
 *       persist_terminal_refusal, guarded on node.db tx_index_complete>=3 so
 *       an in-progress IBD miss is never marked terminal), plus the corrupt
 *       creator_* classes, enumerate metadata-tear, round-cap, scan-gap, and
 *       the proven-spend write at stage_repair_coin_backfill.c. This is the
 *       DURABLE, COPY-PORTABLE proof the boot gate reads via
 *       coin_backfill_meta_present — NOT the in-memory, non-persisted blocker
 *       coin_backfill.unprovable.<h> (that is raised per-tick by the page latch
 *       and is gone on the next boot; the gate runs BEFORE coin_backfill ticks
 *       this boot, so it can ONLY rely on the durable row a PRIOR boot wrote).
 *       It is also the REFUSAL-LATENCY guard: an ok=0 prevout row coin_backfill
 *       has NOT yet durably refused is still being worked, so the gate WAITS
 *       rather than racing a not-yet-confirmed hole. internal_error is NEVER
 *       refused by coin_backfill (transient), so it never gets a marker.
 *
 * We deliberately do NOT require coin_backfill_owner_ack()
 * (env ZCL_REDUCER_COIN_BACKFILL_ACK): that ack gates DESTRUCTIVE coin_backfill
 * writes; THIS verdict is purely DIAGNOSTIC (refuse-to-bless + raise blocker +
 * EV_OPERATOR_NEEDED), so a per-process env requirement would (a) be wrong — a
 * fresh boot / copy-prove has no ack set — and (b) silently disable the
 * diagnosis. The refusal marker (3) is the durable, copy-portable proof; the
 * env is not.
 *
 * script_validate writes NO row for a not-yet-fetched body (JOB_IDLE;
 * script_validate_stage.c:401/409/425/435); an ok=0 prevout_unresolved /
 * block_decode_failed row exists ONLY after the body decoded and a prevout
 * genuinely failed (:267-285) or the body was undecodable (:230-234). A node
 * that has not reached the height has no row → strict no-op.
 *
 * A REFUSAL + typed PERMANENT blocker, NOT a heal/reconcile/backfill (TENACITY
 * I3: no new repair rung). Stamps NOTHING — H* stays pinned at the checkpoint;
 * the VALUE is a CLEAR actionable verdict in place of the opaque I4.3
 * window.consistency HOLD. */

#include "services/block_index_loader.h"
#include "validation/main_state.h"
#include "validation/chainstate.h"
#include "chain/chain.h"
#include "storage/progress_store.h"
#include "storage/coins_kv.h"
#include "core/uint256.h"
#include "event/event.h"

#include <sqlite3.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "util/log_macros.h"
#include "util/blocker.h"

/* The read-only progress.kv hole scanner + the two coin_backfill durable-state
 * accessors live in the jobs backfill-util TU
 * (app/jobs/src/stage_repair_coin_backfill_util.h, private to the backfill
 * TUs) and are exported non-static. Forward-declare their EXACT signatures
 * here rather than dragging the private jobs header onto this services TU's
 * include path (which would trip the E3 shape-includes lint). The caller of
 * the scanner MUST hold progress_store_tx_lock (the _unlocked variant). See
 * stage_repair_coin_backfill_util.c:77/106/133. */
bool find_lowest_prevout_unresolved_hole_unlocked(
    struct sqlite3 *db, int cursor, int *out_height, char status_out[32],
    struct uint256 *hash_out, bool *hash_found);
bool coin_backfill_key_h_hash(char out[192], const char *prefix, int height,
                              const struct uint256 *hash);
bool coin_backfill_meta_present(struct sqlite3 *db, const char *key,
                                bool *present);

bool block_index_loader_torn_import_gate_fires(struct main_state *ms,
                                               struct sqlite3 *progress_db,
                                               int32_t H, int32_t checkpoint)
{
    if (!ms || !progress_db)
        return false;

    int32_t applied = 0;
    bool applied_found = false;
    int active_h = active_chain_height(&ms->chain_active);
    int hole_h = -1;
    char hole_status[32];
    struct uint256 hole_hash;
    bool hole_found = false;
    bool hole_hash_found = false;
    bool backfill_refused = false;

    progress_store_tx_lock();
    (void)coins_kv_get_applied_height(progress_db, &applied, &applied_found);
    /* ceiling = the forward-apply frontier. coins_applied_height is the
     * NEXT-height cursor (applied-through B means value B+1), so it equals the
     * blocked height directly; raise by the active chain height so a hole
     * recorded by script_validate slightly ahead of the coins cursor is still
     * inside the window. Both are read-only forward frontiers. */
    int32_t ceiling = applied_found ? applied : 0;
    if (active_h > ceiling)
        ceiling = active_h;
    if (ceiling > checkpoint) {
        /* find_lowest_..._unlocked selects ok=0 rows with height < cursor;
         * cursor = ceiling+1 admits a hole AT the ceiling (the live spending
         * block == coins_applied_height). One lock span (the lock is
         * recursive): the hole scan AND the refusal-marker read see a
         * consistent progress.kv snapshot. */
        (void)find_lowest_prevout_unresolved_hole_unlocked(
            progress_db, (int)ceiling + 1, &hole_h, hole_status, &hole_hash,
            &hole_hash_found);
        hole_found = (hole_h > 0);
        /* (3) require coin_backfill's durable refusal of THIS hole. The key is
         * built from the hole's own block_hash (captured by the helper) — the
         * exact key the producer writes. A hole the helper could not hash-bind
         * (hole_hash_found false) cannot be matched to a refusal marker, so it
         * never fires. */
        if (hole_found && hole_hash_found) {
            char refused_key[192];
            if (coin_backfill_key_h_hash(refused_key, "coin_backfill.refused",
                                         hole_h, &hole_hash))
                (void)coin_backfill_meta_present(progress_db, refused_key,
                                                 &backfill_refused);
        }
    }
    progress_store_tx_unlock();

    /* (2) status filter: ONLY a durable prevout_unresolved tear fires.
     * 'internal_error' (transient) and 'block_decode_failed' (a FUTURE class
     * coin_backfill never refuses — see the rationale comment) are excluded. */
    bool hole_is_tear =
        hole_found && strcmp(hole_status, "prevout_unresolved") == 0;

    if (!(hole_is_tear && backfill_refused &&
          hole_h > checkpoint && hole_h <= (int)ceiling))
        return false;

    /* Self-contained reason < BLOCKER_REASON_MAX (256): the load-bearing
     * coordinates (h, frontier, seed H) plus a short ACTION token. The full
     * recovery recipe rides the EV_OPERATOR_NEEDED payload below (no cap). */
    char reason[BLOCKER_REASON_MAX];
    snprintf(reason, sizeof(reason),
             "torn cold-import coin set: durable unresolved prevout at h=%d "
             "(coin_backfill refused unprovable; frontier=%d seed H=%d). "
             "ACTION: wipe+re-import (two-step cold-sync)",
             hole_h, (int)ceiling, H);
    LOG_WARN("block_index", "cold-import seed REFUSED at H=%d: %s", H, reason);
    struct blocker_record rec;
    if (blocker_init(&rec, "seed.torn_import", "validation_pack",
                     BLOCKER_PERMANENT, reason) &&
        blocker_set(&rec) == 0)
        event_emitf(EV_OPERATOR_NEEDED, 0,
                    "check=seed.torn_import seed_h=%d first_hole_h=%d "
                    "ceiling=%d torn cold-import coin set: forward validation "
                    "found an unresolved prevout at h=%d (coin_backfill refused "
                    "it as unprovable; forward-apply frontier=%d, seed H=%d); "
                    "the imported trusted base is coin-incomplete. ACTION: wipe "
                    "~/.zclassic-c23 and re-import with the two-step cold-sync "
                    "recipe (--importblockindex then a normal boot)",
                    H, hole_h, (int)ceiling, hole_h, (int)ceiling, H);
    return true;
}

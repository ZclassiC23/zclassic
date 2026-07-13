/* boot_mint_anchor.c — the ANCHOR-SET MINT driver. Contract in config/boot.h
 * (boot_mint_anchor_run). Lives here, separate from boot.c, so each file keeps
 * one focused responsibility.
 *
 * After app_init has (via boot_mint_anchor_reset) reset the staged reducer to
 * genesis and capped the fold at the compiled SHA3 UTXO checkpoint anchor, this
 * driver:
 *   (1) drives the staged reducer synchronously (reducer_kick under the
 *       activation mutex — the same drain the supervisor uses) until the
 *       utxo_apply frontier reaches the anchor (or progress stalls);
 *   (2) writes the resulting coins_kv set to a SHA3-committed snapshot artifact
 *       in the loader's USS format (coins_kv_snapshot_write);
 *   (3) HARD-ASSERTS the written commitment + count == the compiled checkpoint
 *       (FATAL + _exit on mismatch — a mismatch means our fold disagrees with
 *       zclassicd's checkpoint, the h=478544 class: page, never proceed).
 *
 * The validation policy is selected before this driver starts: normal
 * -mint-anchor keeps crypto validation on; -mint-anchor-fast passes the
 * script/proof stages through while preserving the state fold and final
 * SHA3/count hard-assert.
 *
 * OBSERVABILITY — the drive loop appends a throttled progress line
 * (height / anchor / rate / ETA / elapsed) to <datadir>/mint-progress.log (or
 * $ZCL_MINT_PROGRESS_LOG) every ~5s so a long fold is readable FROM DISK, not
 * only from stderr. Best-effort: a log-write failure never affects the fold.
 *
 * RESUMABILITY — a fresh mint does NOT re-fold from genesis after a crash. Three
 * durable mechanisms already cover resume; this driver only observes them:
 *   (1) progress.kv stage cursors are committed per drain batch (durable);
 *   (2) mint_anchor_progress (config/src/mint_anchor_progress.c) binds a
 *       checkpoint-scoped resume marker so boot_mint_anchor_reset SKIPS the
 *       genesis reset and resumes at coins_applied_height (see the "resuming
 *       existing checkpoint-bound fold" branch);
 *   (3) with -fold-inram, coins_ram flushes the in-RAM overlay to durable
 *       coins_kv every ZCL_FOLD_INRAM_FLUSH_EVERY blocks (default 50,000) and
 *       coins_ram_reconcile_boot rewinds the cursor to that flush watermark on
 *       the next boot, so a crash loses at most one flush window, not the fold.
 * The worst-case resume rewind is thus one flush window; lower
 * ZCL_FOLD_INRAM_FLUSH_EVERY to tighten it. A finer (per-batch) synchronous
 * checkpoint of the full in-RAM working set is intentionally NOT added here — it
 * would trade the fold's dominant throughput lever (a low fsync cadence) for a
 * marginal resume-granularity gain, and the mechanisms above already make the
 * mint resumable without it. */

#include "config/boot.h"
#include "config/mint_anchor_progress.h"
#include "config/consensus_state_producer_receipt.h"

#include <errno.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>          /* EXIT_FAILURE, getenv */
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>          /* _exit */
#include <sqlite3.h>

#include "chain/checkpoints.h"                  /* get_sha3_utxo_checkpoint */
#include "storage/coins_kv.h"                   /* coins_kv_snapshot_write,
                                                 * coins_kv_get_applied_height,
                                                 * coins_kv_count,
                                                 * coins_kv_commitment */
#include "storage/coins_ram.h"                  /* coins_ram_active,
                                                 * coins_ram_commitment */
#include "storage/snapshot_shielded.h"          /* snapshot_shielded_collect_from_db */
#include "storage/consensus_state_bundle_codec.h" /* CONSENSUS_STATE_VALIDATION_* */
#include "storage/progress_store.h"             /* progress_store_db */
#include "jobs/mint_skip_crypto.h"              /* mint_skip_crypto_get */
#include "services/chain_activation_service.h"  /* reducer_kick,
                                                 * boot_activation_controller */
#include "event/event.h"                        /* event_emitf */
#include "util/log_macros.h"
#include "core/utiltime.h"                       /* GetTimeMicros */

bool boot_mint_anchor_normal_boot_gate(sqlite3 *progress_db)
{
    char reason[512];
    if (mint_anchor_normal_boot_allowed(progress_db, reason, sizeof(reason)))
        return true;
    fprintf(stderr, "FATAL: normal node boot refused by producer evidence "
            "containment: %s\n",
            reason[0] ? reason : "validation-profile scan failed");
    event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
                "producer_evidence_contained reason=%s",
                reason[0] ? reason : "profile_scan_failed");
    return false;
}

void boot_mint_anchor_require_producer_lane(sqlite3 *progress_db,
                                            bool checkpoint_fold)
{
    if (mint_anchor_producer_lane_bind(progress_db, checkpoint_fold))
        return;
    fprintf(stderr, "FATAL: -mint-anchor producer datadir is bound to a "
            "different validation profile; refusing mixed generations\n");
    event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
                "mint_anchor producer_lane_profile_mismatch");
    _exit(EXIT_FAILURE);
}

/* The utxo_apply frontier is a NEXT-height cursor: applied-through `h` means
 * coins_applied_height == h+1. Read it; return -1 when unknown (absent). */
static int32_t mint_applied_through(sqlite3 *pdb)
{
    int32_t frontier = 0;
    bool found = false;
    if (!coins_kv_get_applied_height(pdb, &frontier, &found) || !found)
        return -1;
    return frontier - 1;
}

bool boot_mint_anchor_should_log_progress(int32_t applied_through,
                                          int32_t anchor)
{
    const int kProgressEvery = 10000;
    return (applied_through % kProgressEvery) == 0 ||
           applied_through >= anchor - 16;
}

/* Resolve the on-disk progress-log path: $ZCL_MINT_PROGRESS_LOG, else
 * <datadir>/mint-progress.log. */
static void mint_progress_log_path(const char *datadir, char *out, size_t n)
{
    const char *env = getenv("ZCL_MINT_PROGRESS_LOG");
    if (env && env[0])
        snprintf(out, n, "%s", env);
    else
        snprintf(out, n, "%s/mint-progress.log", datadir ? datadir : ".");
}

/* Append one throttled progress line to the on-disk mint-progress.log so a
 * long fold is observable FROM DISK. Throttled to ~every 5s of wall time; the
 * final-anchor line is always written. Rate is computed over the interval since
 * the last write (blocks/s), ETA from the remaining span at that rate. All
 * best-effort — a failure to open/write NEVER affects the fold. */
static void mint_progress_log_tick(const char *path, int32_t through,
                                   int32_t anchor, int64_t start_us,
                                   bool force)
{
    static int64_t last_write_us   = 0;
    static int32_t last_write_h     = -1;
    const int64_t  kEveryUs        = 5 * 1000 * 1000;  /* 5s */

    int64_t now_us = GetTimeMicros();
    if (last_write_us == 0) {            /* first call: seed the interval base */
        last_write_us = start_us > 0 ? start_us : now_us;
        last_write_h  = through;
    }
    int64_t since_us = now_us - last_write_us;
    if (!force && since_us < kEveryUs)
        return;

    double interval_s = since_us > 0 ? (double)since_us / 1e6 : 0.0;
    int32_t d_h       = through - last_write_h;
    double  rate      = interval_s > 0.0 ? (double)d_h / interval_s : 0.0;
    int32_t remaining = anchor > through ? anchor - through : 0;
    long    eta_s     = rate > 0.0 ? (long)((double)remaining / rate) : -1;
    double  elapsed_s = start_us > 0 ? (double)(now_us - start_us) / 1e6 : 0.0;

    FILE *f = fopen(path, "a");
    if (!f)
        return;                          /* best-effort: never block the fold */
    if (eta_s >= 0)
        fprintf(f,
                "mint height=%d / %d rate=%.1f blk/s eta=%ld:%02ld:%02ld "
                "elapsed=%.0fs\n",
                through, anchor, rate,
                eta_s / 3600, (eta_s % 3600) / 60, eta_s % 60, elapsed_s);
    else
        fprintf(f,
                "mint height=%d / %d rate=%.1f blk/s eta=unknown "
                "elapsed=%.0fs\n",
                through, anchor, rate, elapsed_s);
    fclose(f);

    last_write_us = now_us;
    last_write_h  = through;
}

bool boot_mint_anchor_run(const char *datadir)
{
    const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
    if (!cp) {
        fprintf(stderr, "FATAL: -mint-anchor: no compiled SHA3 UTXO checkpoint\n");
        event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
                    "mint_anchor no_compiled_checkpoint");
        _exit(EXIT_FAILURE);
    }
    const int32_t anchor = cp->height;

    sqlite3 *pdb = progress_store_db();
    if (!pdb) {
        fprintf(stderr, "FATAL: -mint-anchor: progress store not open\n");
        _exit(EXIT_FAILURE);
    }
    struct chain_activation_controller *ctl = boot_activation_controller();
    if (!ctl) {
        fprintf(stderr, "FATAL: -mint-anchor: no activation controller\n");
        _exit(EXIT_FAILURE);
    }

    /* Producer-START ownership of the durable source receipt (see
     * config/consensus_state_producer_receipt.h): record the running executable
     * + source-identity claim and publish the source-epoch digest BEFORE the
     * fold, so every stamped stage row carries it — the binding the contained
     * full-history exporter's stage-row proof requires. Best-effort: a build
     * with no exact 40-hex commit cannot earn a receipt, but the mint's primary
     * artifact (the verified anchor snapshot) is still produced. */
    {
        uint8_t profile = mint_skip_crypto_get()
                              ? CONSENSUS_STATE_VALIDATION_CHECKPOINT_FOLD
                              : CONSENSUS_STATE_VALIDATION_FULL;
        char rc_err[256] = {0};
        if (!consensus_state_producer_receipt_begin(pdb, profile, rc_err,
                                                    sizeof(rc_err)))
            LOG_WARN("mint_anchor",
                     "[mint-anchor] source receipt begin skipped (mint still "
                     "runs; artifact not exporter-admissible): %s", rc_err);
        else
            fprintf(stderr,
                    "[mint-anchor] durable source receipt session opened "
                    "(profile=%s)\n",
                    profile == CONSENSUS_STATE_VALIDATION_FULL
                        ? "full" : "checkpoint_fold");
    }

    /* (1) Drive the fold to the anchor. reducer_kick_unbudgeted drains the same
     * eight-stage pipeline the supervisor uses, under the activation mutex (no
     * race with the background ticks) AND with the reducer-drive guard held so
     * the supervisor yields its 2s stage ticks for the whole drain. Unlike the
     * budgeted reducer_kick, each call folds back-to-back until convergence
     * instead of stopping every 2s and returning here to re-read the frontier —
     * so the genesis..anchor fold is not chopped into 2s slices. The
     * header_admit ceiling (boot_mint_anchor_reset) caps the fold AT the anchor,
     * so the pipeline converges there and the kick returns 0 advances. We loop
     * until the utxo_apply frontier reaches the anchor; we also break on a
     * no-progress plateau so a bodies-missing datadir cannot spin forever (the
     * caller then reports the mint as incomplete, not a false anchor). */
    int32_t last_through = mint_applied_through(pdb);
    int stall_kicks = 0;
    const int kStallLimit = 64;   /* consecutive no-progress kicks → bodies gap */
    char progress_log[1200];
    mint_progress_log_path(datadir, progress_log, sizeof(progress_log));
    const int64_t drive_start_us = GetTimeMicros();
    fprintf(stderr,
            "[mint-anchor] driving the genesis..%d fold; "
            "starting at applied-through=%d; progress log -> %s\n",
            anchor, last_through, progress_log);
    mint_progress_log_tick(progress_log, last_through, anchor,
                           drive_start_us, /*force=*/true);

    for (;;) {
        int32_t through = mint_applied_through(pdb);
        if (through >= anchor)
            break;

        (void)reducer_kick_unbudgeted(ctl);   /* tight back-to-back drain to convergence */

        int32_t now = mint_applied_through(pdb);
        /* Throttled on-disk progress (every ~5s) — readable while the fold
         * runs, regardless of the sparse stderr cadence below. */
        mint_progress_log_tick(progress_log, now, anchor, drive_start_us,
                               /*force=*/false);
        if (now > last_through) {
            last_through = now;
            stall_kicks = 0;
            if (boot_mint_anchor_should_log_progress(now, anchor))
                fprintf(stderr, "[mint-anchor] applied-through=%d / %d\n",
                        now, anchor);
        } else if (++stall_kicks >= kStallLimit) {
            fprintf(stderr,
                    "[mint-anchor] fold stalled at applied-through=%d (target "
                    "anchor=%d) after %d no-progress kicks — the on-disk bodies "
                    "below the anchor are incomplete; cannot mint. Import the "
                    "full header+body history first.\n",
                    now, anchor, kStallLimit);
            return false;
        }
    }

    int32_t through = mint_applied_through(pdb);
    int64_t count = coins_kv_count(pdb);
    mint_progress_log_tick(progress_log, through, anchor, drive_start_us,
                           /*force=*/true);
    fprintf(stderr,
            "[mint-anchor] fold reached the anchor: applied-through=%d, "
            "coins_kv count=%lld — writing the snapshot\n",
            through, (long long)count);

    /* (2) Write the snapshot artifact. Output path: $ZCL_MINT_ANCHOR_OUT, else
     * <datadir>/utxo-anchor.snapshot. */
    char out_path[1100];
    const char *env_out = getenv("ZCL_MINT_ANCHOR_OUT");
    if (env_out && env_out[0]) {
        snprintf(out_path, sizeof(out_path), "%s", env_out);
    } else {
        snprintf(out_path, sizeof(out_path), "%s/utxo-anchor.snapshot",
                 datadir ? datadir : ".");
    }

    /* Collect the live SHIELDED frontier at the anchor (Sapling + Sprout
     * commitment-tree frontiers + the nullifier set) so the legacy artifact is
     * a locally self-minted current-state candidate, not a coins-only borrow.
     * It still omits historical anchor rows and therefore is not a complete
     * sovereign bundle. The transparent set is checked against the compiled
     * checkpoint; Sprout and nullifier completeness still require the canonical
     * bundle proof manifest and copy proof. FAIL LOUD if the
     * frontier is unavailable — never emit a coins-only snapshot mislabeled
     * shielded (the birth-defect this cure exists to close). */
    struct snapshot_shielded shielded;
    if (!snapshot_shielded_collect_from_db(pdb, anchor, &shielded)) {
        fprintf(stderr,
                "FATAL: -mint-anchor: shielded-frontier collection at h=%d "
                "failed — refusing to emit a coins-only snapshot mislabeled "
                "shielded\n", anchor);
        event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
                    "mint_anchor shielded_collect_failed h=%d", anchor);
        _exit(EXIT_FAILURE);
    }

    uint8_t got_sha3[32] = {0};
    uint64_t got_count = 0;
    int64_t  got_supply = 0;
    if (!coins_kv_snapshot_write(pdb, out_path, anchor, cp->block_hash,
                                 &shielded, got_sha3, &got_count, &got_supply)) {
        snapshot_shielded_free_collected(&shielded);
        fprintf(stderr, "FATAL: -mint-anchor: snapshot write to %s failed\n",
                out_path);
        event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
                    "mint_anchor snapshot_write_failed path=%s", out_path);
        _exit(EXIT_FAILURE);
    }
    snapshot_shielded_free_collected(&shielded);

    /* Coins-only commitment over the SAME effective set the writer streamed.
     * got_sha3 is the v3 BODY SHA3 (coins + shielded section), so it is NOT the
     * coins commitment the compiled checkpoint pins; compute that separately,
     * mirroring the writer's overlay predicate (coins_ram_active), so the value
     * covers any un-flushed in-RAM fold tail exactly as the writer did. */
    uint8_t coins_sha3[32] = {0};
    int crc = coins_ram_active() ? coins_ram_commitment(coins_sha3)
                                 : coins_kv_commitment(pdb, coins_sha3);
    if (crc != 0) {
        fprintf(stderr, "FATAL: -mint-anchor: coins commitment computation at "
                "h=%d failed — cannot verify the minted set\n", anchor);
        event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
                    "mint_anchor coins_commitment_failed h=%d", anchor);
        unlink(out_path);
        _exit(EXIT_FAILURE);
    }

    /* (3) HARD-ASSERT the written set == the compiled checkpoint. The writer's
     * body SHA3 equals coins_kv_commitment (same record encoder),
     * so a match here proves our independently-folded anchor set reproduces
     * zclassicd's checkpoint exactly. A MISMATCH means our fold disagrees with
     * the checkpoint (the h=478544 class): page EV_BOOT_VALIDATION_FAILED and
     * _exit — NEVER retain an unproven artifact. */
    bool sha3_match = memcmp(coins_sha3, cp->sha3_hash, 32) == 0;
    bool count_match = got_count == cp->utxo_count;
    if (!sha3_match || !count_match) {
        char want_hex[65], got_hex[65];
        for (int i = 0; i < 32; i++) {
            snprintf(want_hex + 2 * i, 3, "%02x", cp->sha3_hash[i]);
            snprintf(got_hex + 2 * i, 3, "%02x", coins_sha3[i]);
        }
        fprintf(stderr,
                "FATAL: -mint-anchor: minted anchor set FAILED the SHA3/count "
                "check (count=%llu want=%llu, sha3=%s want=%s) — our genesis.."
                "%d fold disagrees with the compiled checkpoint. Refusing to "
                "retain; the artifact at %s is NOT trustworthy.\n",
                (unsigned long long)got_count,
                (unsigned long long)cp->utxo_count, got_hex, want_hex,
                anchor, out_path);
        event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
                    "check=mint_anchor anchor_h=%d minted set mismatch "
                    "(count=%llu want=%llu sha3_match=%d) — fold disagrees with "
                    "the compiled checkpoint; do NOT trust the artifact",
                    anchor, (unsigned long long)got_count,
                    (unsigned long long)cp->utxo_count, sha3_match ? 1 : 0);
        /* Remove the bad artifact so a later -refold-from-anchor cannot load it. */
        unlink(out_path);
        _exit(EXIT_FAILURE);
    }

    char sha3_hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(sha3_hex + 2 * i, 3, "%02x", coins_sha3[i]);
    fprintf(stderr,
            "[mint-anchor] SUCCESS: minted a checkpoint-matching anchor UTXO "
            "set at h=%d "
            "(count=%llu, supply=%lld zatoshi, coins_sha3=%s) — matches the "
            "compiled checkpoint. Locally self-minted legacy v3 current-state "
            "candidate (history incomplete; non-serving and not canonically "
            "publishable): "
            "%s\n",
            anchor, (unsigned long long)got_count, (long long)got_supply,
            sha3_hex, out_path);
    if (!mint_anchor_progress_clear(pdb))
        LOG_WARN("mint_anchor",
                 "[mint-anchor] verified snapshot was written, but clearing "
                 "the resume marker failed; future -mint-anchor runs may "
                 "resume/rewrite the same verified artifact");

    /* Producer-END ownership: finalize the durable source receipt, binding it
     * to the completed (anchor, cp->block_hash) generation and the H*+1 fold
     * cursor, and verifying the SAME running executable that opened the start
     * session. This is what lets the contained full-history exporter admit a
     * producer THIS binary ran itself. Best-effort: a missing start session
     * (unstamped build) or an incomplete header corpus leaves no receipt, and
     * the exporter then correctly refuses (fail closed). */
    {
        char rc_err[256] = {0};
        if (!consensus_state_producer_receipt_finalize(pdb, anchor,
                                                       cp->block_hash, rc_err,
                                                       sizeof(rc_err)))
            LOG_WARN("mint_anchor",
                     "[mint-anchor] source receipt finalize skipped (artifact "
                     "verified; not exporter-admissible): %s", rc_err);
        else
            fprintf(stderr,
                    "[mint-anchor] durable source receipt finalized at h=%d "
                    "(fold_cursor=%d) — exporter can now admit this producer\n",
                    anchor, anchor + 1);
    }
    return true;
}

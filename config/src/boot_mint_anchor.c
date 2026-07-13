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
#include "storage/event_log_singleton.h"        /* event_log_set_singleton (S1.2) */
#include "storage/consensus_state_bundle_codec.h" /* CONSENSUS_STATE_VALIDATION_* */
#include "storage/progress_store.h"             /* progress_store_db */
#include "jobs/mint_skip_crypto.h"              /* mint_skip_crypto_get */
#include "jobs/header_admit_stage.h"            /* header_admit_stage_step_us_ewma */
#include "jobs/validate_headers_stage.h"        /* validate_headers_stage_step_us_ewma */
#include "jobs/body_fetch_stage.h"              /* body_fetch_stage_step_us_ewma */
#include "jobs/body_persist_stage.h"            /* body_persist_stage_step_us_ewma */
#include "jobs/script_validate_stage.h"         /* script_validate_stage_step_us_ewma */
#include "jobs/proof_validate_stage.h"          /* proof_validate_stage_step_us_ewma */
#include "jobs/utxo_apply_stage.h"              /* utxo_apply_stage_cursor,
                                                 * utxo_apply_stage_step_us_ewma */
#include "jobs/tip_finalize_stage.h"            /* tip_finalize_stage_step_us_ewma */
#include "jobs/stage_helpers.h"                 /* stage_cursor_persisted */
#include "services/chain_activation_service.h"  /* reducer_kick,
                                                 * boot_activation_controller */
#include "event/event.h"                        /* event_emitf */
#include "util/blocker.h"                       /* blocker_init, blocker_set */
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

/* The IMMEDIATE fold frontier: the utxo_apply STAGE cursor (next height to
 * apply; batch-committed by the drain). This is the drive loop's progress
 * metric. mint_applied_through above reads the durable coins_applied_height
 * key, which under -fold-inram is written only at coins_ram FLUSH boundaries
 * (every ZCL_FOLD_INRAM_FLUSH_EVERY blocks, coins_ram.c) — it reads -1 for the
 * whole first flush window and then lags by up to a window, so a drive loop
 * gated on it can neither see progress (a false "stall") nor see the anchor
 * being reached (a false "incomplete" after a COMPLETE fold whose tail is
 * still overlay-resident). Returns -1 when nothing has been applied. */
static int32_t mint_frontier_through(void)
{
    uint64_t cursor = utxo_apply_stage_cursor();
    if (cursor == 0)
        return -1;
    if (cursor > (uint64_t)INT32_MAX)
        return INT32_MAX;
    return (int32_t)cursor - 1;
}

/* Fail-closed stall diagnosis: the fold frontier stopped below the anchor.
 * Read the eight durable stage cursors, name the WALLED stage (the earliest
 * pipeline stage sitting at the minimum cursor — upstream of the wall runs
 * ahead, downstream can never pass it), register a typed PERMANENT blocker
 * carrying all eight cursors, and page the operator via EV_OPERATOR_NEEDED.
 * The bodies-gap wording is used ONLY when the wall is body_fetch (headers
 * validated but no on-disk body to fetch at the frontier) — every other wall
 * names its stage instead of blaming the body import. Public (config/boot.h)
 * so the mint-fold livelock regression test can assert the blocker payload. */
void boot_mint_anchor_report_frontier_walled(sqlite3 *pdb, int32_t frontier,
                                             int32_t anchor, int stall_kicks)
{
    /* Pipeline order; tip_finalize is read for the report but excluded from
     * wall selection (it intentionally trails the fold during a mint). */
    static const char *const stages[8] = {
        "header_admit", "validate_headers", "body_fetch", "body_persist",
        "script_validate", "proof_validate", "utxo_apply", "tip_finalize" };
    uint64_t cur[8] = {0};
    for (int i = 0; i < 8; i++)
        cur[i] = stage_cursor_persisted(pdb, stages[i], "mint_anchor");

    int wall = 0;
    for (int i = 1; i < 7; i++)          /* exclude tip_finalize (index 7) */
        if (cur[i] < cur[wall])
            wall = i;
    bool bodies_gap = (wall == 2);       /* body_fetch */

    char reason[BLOCKER_REASON_MAX];
    snprintf(reason, sizeof(reason),
             "mint fold frontier walled at h=%d (anchor=%d) after %d "
             "no-progress kicks; wall=%s cursors ha=%llu vh=%llu bf=%llu "
             "bp=%llu sv=%llu pv=%llu ua=%llu tf=%llu",
             frontier, anchor, stall_kicks, stages[wall],
             (unsigned long long)cur[0], (unsigned long long)cur[1],
             (unsigned long long)cur[2], (unsigned long long)cur[3],
             (unsigned long long)cur[4], (unsigned long long)cur[5],
             (unsigned long long)cur[6], (unsigned long long)cur[7]);

    struct blocker_record rec;
    if (blocker_init(&rec, "mint_fold.frontier_walled", "mint_anchor",
                     BLOCKER_PERMANENT, reason))
        (void)blocker_set(&rec);
    event_emitf(EV_OPERATOR_NEEDED, 0,
                "condition=mint_fold_frontier_walled %s", reason);

    if (bodies_gap)
        fprintf(stderr,
                "[mint-anchor] fold stalled at applied-through=%d (target "
                "anchor=%d): body_fetch is the walled stage — the on-disk "
                "bodies below the anchor are incomplete; cannot mint. Import "
                "the full header+body history first. (%s)\n",
                frontier, anchor, reason);
    else
        fprintf(stderr,
                "[mint-anchor] fold stalled at applied-through=%d (target "
                "anchor=%d): stage %s is walled — NOT a bodies gap; inspect "
                "its stage log at the frontier height. (%s)\n",
                frontier, anchor, stages[wall], reason);
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

/* Per-stage step-timing EWMA snapshot for the mint-progress.log line below.
 * -mint-anchor producers run WITHOUT RPC (no dumpstate reachable), so this is
 * the only offline surface for "which of the eight stages is the fold's
 * bottleneck right now" — diagnosing that today required /proc wchan
 * sampling. Same pipeline order + abbreviations as
 * boot_mint_anchor_report_frontier_walled's cursor report above (ha, vh, bf,
 * bp, sv, pv, ua, tf), so the two reports read consistently side by side. */
static void mint_stage_ewma_collect(const char *abbrev_out[8], int64_t ewma_out[8])
{
    static const char *const abbrev[8] = {
        "ha", "vh", "bf", "bp", "sv", "pv", "ua", "tf" };
    for (int i = 0; i < 8; i++)
        abbrev_out[i] = abbrev[i];
    ewma_out[0] = header_admit_stage_step_us_ewma();
    ewma_out[1] = validate_headers_stage_step_us_ewma();
    ewma_out[2] = body_fetch_stage_step_us_ewma();
    ewma_out[3] = body_persist_stage_step_us_ewma();
    ewma_out[4] = script_validate_stage_step_us_ewma();
    ewma_out[5] = proof_validate_stage_step_us_ewma();
    ewma_out[6] = utxo_apply_stage_step_us_ewma();
    ewma_out[7] = tip_finalize_stage_step_us_ewma();
}

/* Append one throttled progress line to the on-disk mint-progress.log so a
 * long fold is observable FROM DISK. Throttled to ~every 5s of wall time; the
 * final-anchor line is always written. Rate is computed over the interval since
 * the last write (blocks/s), ETA from the remaining span at that rate. All
 * best-effort — a failure to open/write NEVER affects the fold.
 *
 * The line also carries the eight stages' live step_us_ewma (in-process only —
 * a different process, e.g. `anchorstatus`, cannot read them; this log line is
 * the only durable trace of the snapshot) so one `tail -1 mint-progress.log`
 * names the slowest stage (`slow=<abbrev>:<ewma_us>us`) without attaching a
 * debugger or sampling /proc/<pid>/wchan. */
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

    const char *stage_abbrev[8];
    int64_t     stage_ewma[8];
    mint_stage_ewma_collect(stage_abbrev, stage_ewma);
    int slow = 0;
    for (int i = 1; i < 8; i++)
        if (stage_ewma[i] > stage_ewma[slow])
            slow = i;

    char stages_buf[240];
    int  off = snprintf(stages_buf, sizeof(stages_buf), "stages=[");
    for (int i = 0; i < 8 && off > 0 && (size_t)off < sizeof(stages_buf); i++)
        off += snprintf(stages_buf + off, sizeof(stages_buf) - (size_t)off,
                        "%s%s:%lldus", i == 0 ? "" : " ", stage_abbrev[i],
                        (long long)stage_ewma[i]);
    if (off > 0 && (size_t)off < sizeof(stages_buf))
        snprintf(stages_buf + off, sizeof(stages_buf) - (size_t)off, "]");

    FILE *f = fopen(path, "a");
    if (!f)
        return;                          /* best-effort: never block the fold */
    if (eta_s >= 0)
        fprintf(f,
                "mint height=%d / %d rate=%.1f blk/s eta=%ld:%02ld:%02ld "
                "elapsed=%.0fs slow=%s:%lldus %s\n",
                through, anchor, rate,
                eta_s / 3600, (eta_s % 3600) / 60, eta_s % 60, elapsed_s,
                stage_abbrev[slow], (long long)stage_ewma[slow],
                stages_buf);
    else
        fprintf(f,
                "mint height=%d / %d rate=%.1f blk/s eta=unknown "
                "elapsed=%.0fs slow=%s:%lldus %s\n",
                through, anchor, rate, elapsed_s,
                stage_abbrev[slow], (long long)stage_ewma[slow],
                stages_buf);
    fclose(f);

    last_write_us = now_us;
    last_write_h  = through;
}

/* Test-only forwarder (declared in config/boot.h) so the
 * reducer_step_drain_harness test group can drive one tick and assert the
 * on-disk line without duplicating this TU's static throttle/format logic. */
void boot_mint_anchor_progress_log_tick_for_test(const char *path,
                                                 int32_t through,
                                                 int32_t anchor,
                                                 int64_t start_us,
                                                 bool force)
{
    mint_progress_log_tick(path, through, anchor, start_us, force);
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
    /* Progress metric: the IMMEDIATE utxo_apply stage frontier
     * (mint_frontier_through), NOT the durable coins_applied_height. Under
     * -fold-inram the durable key only moves at coins_ram flush boundaries
     * (every ZCL_FOLD_INRAM_FLUSH_EVERY blocks), so gating on it makes the
     * stall detector blind for a whole flush window AND leaves the anchor
     * break unreachable when the fold's tail is overlay-resident. On resume
     * the stage cursor already reflects the durable resume point. */
    /* S1.2 — skip event_log emission during the offline mint. The mint's only
     * output (utxo-anchor.snapshot) is built from coins_kv (progress.kv,
     * written directly by utxo_apply) + node.db shielded state; it never reads
     * the event_log or its projections. So the fold-thread EV_BLOCK_BODY /
     * EV_BLOCK_HEADER emissions — serialize + pwrite + fsync per block — are
     * pure overhead here. Every fold-path emitter routes through
     * event_log_singleton() and is NULL-tolerant (skips on NULL), so unwiring
     * the singleton suppresses all of them for the mint's duration.
     *
     * Two env escapes keep the emission on for A/B measurement:
     *   ZCL_EVENTLOG_SYNC_PER_APPEND=1  (the S1.1 kill switch — restore the OLD
     *                                    per-append-fsync baseline: emission ON)
     *   ZCL_MINT_KEEP_EVENTLOG=1        (keep emission ON but let S1.1 batch it
     *                                    — isolates the S1.2 delta). */
    bool keep_eventlog = (getenv("ZCL_EVENTLOG_SYNC_PER_APPEND") != NULL) ||
                         (getenv("ZCL_MINT_KEEP_EVENTLOG") != NULL);
    if (!keep_eventlog) {
        event_log_set_singleton(NULL);
        fprintf(stderr,
                "[mint-anchor] S1.2: event_log emission suppressed for the fold "
                "(artifact reads coins_kv + shielded only)\n");
    }

    int32_t last_through = mint_frontier_through();
    if (last_through < 0)
        last_through = mint_applied_through(pdb);  /* pre-init fallback */
    int stall_kicks = 0;
    const int kStallLimit = 64;   /* consecutive no-progress kicks → walled */
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
        int32_t through = mint_frontier_through();
        if (through >= anchor)
            break;

        /* Bounded drain chunk: returns within ZCL_MINT_KICK_BUDGET_MS (or at
         * the first frontier-stalled round) so THIS loop reliably regains
         * control to log progress and run the stall detector below — the
         * budgetless kick was a silent multi-hour spin (the 2026-07-13 mint
         * livelock: no progress line, stall guard never ran). */
        (void)reducer_kick_unbudgeted(ctl);

        int32_t now = mint_frontier_through();
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
            /* Fail CLOSED with a named blocker: register
             * mint_fold.frontier_walled (all eight stage cursors in the
             * reason), page EV_OPERATOR_NEEDED, and print a diagnosis that
             * names the walled stage — "bodies incomplete" only when
             * body_fetch really is the wall. */
            boot_mint_anchor_report_frontier_walled(pdb, now, anchor,
                                                    kStallLimit);
            return false;
        }
    }

    int32_t through = mint_frontier_through();
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

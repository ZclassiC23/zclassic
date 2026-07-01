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
 * SHA3/count hard-assert. */

#include "config/boot.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>          /* EXIT_FAILURE, getenv */
#include <string.h>
#include <unistd.h>          /* _exit */
#include <sqlite3.h>

#include "chain/checkpoints.h"                  /* get_sha3_utxo_checkpoint */
#include "storage/coins_kv.h"                   /* coins_kv_snapshot_write,
                                                 * coins_kv_get_applied_height,
                                                 * coins_kv_count */
#include "storage/progress_store.h"             /* progress_store_db */
#include "services/chain_activation_service.h"  /* reducer_kick,
                                                 * boot_activation_controller */
#include "event/event.h"                        /* event_emitf */
#include "util/log_macros.h"

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
    fprintf(stderr,
            "[mint-anchor] driving the genesis..%d fold; "
            "starting at applied-through=%d\n", anchor, last_through);

    for (;;) {
        int32_t through = mint_applied_through(pdb);
        if (through >= anchor)
            break;

        (void)reducer_kick_unbudgeted(ctl);   /* tight back-to-back drain to convergence */

        int32_t now = mint_applied_through(pdb);
        if (now > last_through) {
            last_through = now;
            stall_kicks = 0;
            if (now % 50000 == 0 || now >= anchor - 16)
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

    uint8_t got_sha3[32] = {0};
    uint64_t got_count = 0;
    int64_t  got_supply = 0;
    if (!coins_kv_snapshot_write(pdb, out_path, anchor, cp->block_hash,
                                 got_sha3, &got_count, &got_supply)) {
        fprintf(stderr, "FATAL: -mint-anchor: snapshot write to %s failed\n",
                out_path);
        event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
                    "mint_anchor snapshot_write_failed path=%s", out_path);
        _exit(EXIT_FAILURE);
    }

    /* (3) HARD-ASSERT the written set == the compiled checkpoint. The writer's
     * body SHA3 equals coins_kv_commitment (same record encoder),
     * so a match here proves our independently-folded anchor set reproduces
     * zclassicd's checkpoint exactly. A MISMATCH means our fold disagrees with
     * the checkpoint (the h=478544 class): page EV_BOOT_VALIDATION_FAILED and
     * _exit — NEVER publish an unproven artifact. */
    bool sha3_match = memcmp(got_sha3, cp->sha3_hash, 32) == 0;
    bool count_match = got_count == cp->utxo_count;
    if (!sha3_match || !count_match) {
        char want_hex[65], got_hex[65];
        for (int i = 0; i < 32; i++) {
            snprintf(want_hex + 2 * i, 3, "%02x", cp->sha3_hash[i]);
            snprintf(got_hex + 2 * i, 3, "%02x", got_sha3[i]);
        }
        fprintf(stderr,
                "FATAL: -mint-anchor: minted anchor set FAILED the SHA3/count "
                "check (count=%llu want=%llu, sha3=%s want=%s) — our genesis.."
                "%d fold disagrees with the compiled checkpoint. Refusing to "
                "publish; the artifact at %s is NOT trustworthy.\n",
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
        snprintf(sha3_hex + 2 * i, 3, "%02x", got_sha3[i]);
    fprintf(stderr,
            "[mint-anchor] SUCCESS: minted the verified anchor UTXO set at h=%d "
            "(count=%llu, supply=%lld zatoshi, sha3=%s) — matches the compiled "
            "checkpoint. Snapshot artifact: %s\n",
            anchor, (unsigned long long)got_count, (long long)got_supply,
            sha3_hex, out_path);
    return true;
}

// one-result-type-ok:selfmint-best-effort-semantics
/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * anchor_selfmint — implementation. See services/anchor_selfmint.h.
 *
 * Return type is bare void (NOT struct zcl_result), same as seal_service: this
 * is OBSERVE-ONLY and BEST-EFFORT — a write/verify failure here MUST NOT fail
 * the block, so there is nothing for a caller to branch on. A failure is logged
 * (LOG_WARN) and dropped.
 *
 * The snapshot writer (coins_kv_snapshot_write) carries the raw-sqlite hatch for
 * the cross-thread progress.kv handle; this TU only resolves a path, probes an
 * existing file via uss_open (read-only mmap), and calls that writer. No raw
 * sqlite here.
 */

#include "services/anchor_selfmint.h"

#include "chain/checkpoints.h"
#include "chain/utxo_snapshot_loader.h"
#include "storage/coins_kv.h"
#include "util/log_macros.h"

#include <stdio.h>
#include <string.h>
#include <unistd.h>

bool anchor_selfmint_resolve_path(const char *datadir, char *buf, size_t cap)
{
    if (!buf || cap == 0)
        return false;
    const char *env_out = getenv("ZCL_MINT_ANCHOR_OUT");
    if (env_out && env_out[0]) {
        int n = snprintf(buf, cap, "%s", env_out);
        return n > 0 && (size_t)n < cap;
    }
    const char *dir = (datadir && datadir[0]) ? datadir : ".";
    int n = snprintf(buf, cap, "%s/utxo-anchor.snapshot", dir);
    return n > 0 && (size_t)n < cap;
}

/* True iff a SHA3-verified snapshot bound to `cp` already exists at `path`.
 * Reuses the existing loader's verification (full-body SHA3 + expected_sha3 ==
 * cp->sha3_hash + count match) — never reimplemented. Read-only mmap, closed
 * immediately. A NULL handle means absent / wrong-magic / wrong-version /
 * SHA3-mismatch — every "no usable verified snapshot" case. */
static bool verified_snapshot_present(const char *path,
                                      const struct sha3_utxo_checkpoint *cp)
{
    char err[256] = {0};
    struct uss_header hdr;
    struct uss_handle *h = uss_open(path, /*verify_full_sha3=*/true,
                                    cp->sha3_hash, &hdr, err, sizeof(err));
    if (!h)
        return false;
    bool ok = (hdr.count == cp->utxo_count);
    uss_close(h);
    return ok;
}

void anchor_selfmint_hook_in_tx(struct sqlite3 *db, const char *datadir,
                                int32_t next_cursor)
{
    if (!db)
        return;

    const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
    if (!cp)
        return;  /* no compiled checkpoint to mint toward */

    /* One-shot at the EXACT anchor crossing. next_cursor is the just-applied
     * frontier (cursor_in + 1); == cp->height is the apply that lands the anchor
     * block, when coins_kv provably holds the applied-through-anchor set inside
     * the caller's stage txn. Below or above the anchor coins_kv is NOT the
     * anchor set — never mint then. Gated on the EXACT height, NOT on
     * is_initial_block_download (the anchor crossing is during IBD on a cold
     * sync). */
    if (next_cursor != cp->height)
        return;

    char path[1100];
    if (!anchor_selfmint_resolve_path(datadir, path, sizeof(path))) {
        LOG_WARN("selfmint", "[selfmint] could not resolve snapshot path "
                 "(datadir=%s) — skipping anchor self-mint",
                 datadir ? datadir : "(null)");
        return;
    }

    /* Idempotency: a valid SHA3-verified snapshot already at the path is left
     * untouched — never rewrite a good artifact (and never pay the scan twice).
     * A stale/corrupt/wrong-height file is treated as absent: the write below
     * atomically replaces it (tmp + rename), then HARD-VERIFY gates trust. */
    if (verified_snapshot_present(path, cp)) {
        LOG_INFO("selfmint", "[selfmint] verified anchor snapshot already "
                 "present at %s — not re-minting", path);
        return;
    }

    /* WRITE: stream the live coins_kv set (the committed applied-through-anchor
     * set) to the snapshot. coins_kv_snapshot_write writes atomically
     * (out_path.tmp then rename) and fills got_sha3 / got_count. This is a
     * read-only SELECT over coins; it does NOT mutate the fold or the txn's
     * consensus state — it only PERSISTS already-validated state. */
    uint8_t got_sha3[32] = {0};
    uint64_t got_count = 0;
    int64_t got_supply = 0;
    if (!coins_kv_snapshot_write(db, path, cp->height, cp->block_hash,
                                 got_sha3, &got_count, &got_supply)) {
        LOG_WARN("selfmint", "[selfmint] snapshot write to %s failed — leaving "
                 "no artifact (the offline -mint-anchor ceremony remains the "
                 "fallback)", path);
        return;  /* writer already removed its temp file on error */
    }

    /* HARD-VERIFY the written set == the compiled checkpoint. The writer's body
     * SHA3 equals coins_kv_commitment (same record encoder), so a match here
     * proves our independently-folded anchor set reproduces the compiled
     * checkpoint exactly. A MISMATCH means our fold disagrees with the
     * checkpoint (the h=478544 class) — UNLINK the artifact so a later
     * -refold-from-anchor can NEVER load an unverified set, and LOG_WARN (NOT
     * fatal — this is observe-only and must not fail the block). */
    bool sha3_match = memcmp(got_sha3, cp->sha3_hash, 32) == 0;
    bool count_match = got_count == cp->utxo_count;
    if (!sha3_match || !count_match) {
        char want_hex[65], got_hex[65];
        for (int i = 0; i < 32; i++) {
            snprintf(want_hex + 2 * i, 3, "%02x", cp->sha3_hash[i]);
            snprintf(got_hex + 2 * i, 3, "%02x", got_sha3[i]);
        }
        LOG_WARN("selfmint", "[selfmint] minted anchor set FAILED the SHA3/count "
                 "check (count=%llu want=%llu sha3=%s want=%s) — our fold "
                 "disagrees with the compiled checkpoint; UNLINKING %s (never "
                 "publish an unverified artifact)",
                 (unsigned long long)got_count,
                 (unsigned long long)cp->utxo_count, got_hex, want_hex, path);
        unlink(path);
        return;
    }

    char sha3_hex[65];
    for (int i = 0; i < 32; i++)
        snprintf(sha3_hex + 2 * i, 3, "%02x", got_sha3[i]);
    LOG_INFO("selfmint", "[selfmint] SELF-MINTED the verified anchor UTXO set at "
             "h=%d (count=%llu, supply=%lld zatoshi, sha3=%s) from the live fold "
             "— matches the compiled checkpoint. Snapshot artifact: %s (the "
             "torn-import self-heal can now re-seed from it)",
             cp->height, (unsigned long long)got_count, (long long)got_supply,
             sha3_hex, path);
}

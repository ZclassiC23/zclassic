/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Block Index Loader: read/write block_index.bin flat file, SQLite cache, and
 * LevelDB block tree compatibility. */

#include "platform/time_compat.h"
#include "services/block_index_loader.h"
#include "services/block_index_integrity.h"
#include "services/invariant_sentinel.h"
#include "services/chain_state_service.h"
#include "services/chain_tip.h"
#include "chain/chain.h"
#include "chain/chainparams.h"
#include "chain/pow.h"
#include "validation/chainstate.h"
#include "validation/main_state.h"
#include "storage/block_index_db.h"
#include "storage/sha3_sidecar_io.h"
#include "crypto/sha3.h"
#include "models/database.h"
#include "models/block.h"
#include "primitives/block.h"
#include "core/uint256.h"
#include "core/arith_uint256.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <limits.h>
#include <sys/stat.h>
#include <sys/mman.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <errno.h>
#include <sqlite3.h>

#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include "util/safe_alloc.h"

/* ── Flat file format ────────────────────────────────────── */

/* Compact on-disk format: height-sorted, 172 bytes per entry (packed). The
 * size-on-disk math below uses sizeof(struct block_index_flat), so it tracks
 * this layout automatically. */
struct __attribute__((packed)) block_index_flat {
    uint8_t  hash[32];
    uint8_t  prev_hash[32];
    int32_t  height;
    uint32_t n_bits;
    uint32_t n_time;
    int32_t  n_version;
    uint32_t n_status;
    int32_t  n_file;
    uint32_t n_data_pos;
    uint32_t n_undo_pos;
    uint32_t n_tx;
    uint32_t n_chain_tx;
    uint8_t  chain_work[32];
    uint32_t n_cached_branch_id;
    uint8_t  sapling_root[32];
};

static int cmp_height(const void *a, const void *b)
{
    const struct block_index *pa = *(const struct block_index *const *)a;
    const struct block_index *pb = *(const struct block_index *const *)b;
    if (pa->nHeight < pb->nHeight) return -1; // raw-return-ok:qsort-comparator
    if (pa->nHeight > pb->nHeight) return 1;
    return 0;
}

/* Forward pass over a height-sorted block_index array: recompute nChainWork,
 * nChainTx, skip links, cached branch id, and failed-child propagation from
 * each entry's already-linked pprev. Shared by the legacy LevelDB loader and
 * the event-log projection rebuild so both compute pointer-graph-derived
 * fields through one helper. Declared in services/block_index_loader.h. */
void block_index_forward_pass(struct block_index **sorted,
                              size_t count)
{
    for (size_t i = 0; i < count; i++) {
        struct block_index *pindex = sorted[i];

        struct arith_uint256 proof = GetBlockProof(pindex);
        if (pindex->pprev)
            arith_uint256_add(&pindex->nChainWork,
                              &pindex->pprev->nChainWork, &proof);
        else
            pindex->nChainWork = proof;

        if (pindex->nTx > 0) {
            if (pindex->pprev) {
                if (pindex->pprev->nChainTx)
                    pindex->nChainTx = pindex->pprev->nChainTx + pindex->nTx;
                else
                    pindex->nChainTx = 0;
            } else {
                pindex->nChainTx = pindex->nTx;
            }
        }

        block_index_build_skip(pindex);

        if (pindex->pprev) {
            if (block_index_is_valid(pindex, BLOCK_VALID_CONSENSUS) &&
                !pindex->nCachedBranchId)
                pindex->nCachedBranchId = pindex->pprev->nCachedBranchId;
        }

        if (!(pindex->nStatus & BLOCK_FAILED_MASK) && pindex->pprev &&
            (pindex->pprev->nStatus & BLOCK_FAILED_MASK))
            pindex->nStatus |= BLOCK_FAILED_CHILD;
    }
}

/* After the map is loaded and the forward pass has recomputed nChainWork,
 * make the header frontier REAL: publish the max-chainwork header into
 * pindex_best_header and seat it in the active_chain[] window.
 *
 * THE LINCHPIN. On a full-index boot (`--importblockindex` then a normal
 * boot) load_block_index fills the header MAP but the empty-datadir fallback
 * is the ONLY path that ever set pindex_best_header or the active_chain
 * window. So active_chain_tip() stays NULL over a map of millions of headers,
 * push_getheaders falls to a genesis-only locator, and header sync pins near
 * genesis with nothing to escalate it. Promoting the best header here anchors
 * the getheaders locator at the true frontier so block bodies can be fetched
 * and folded forward.
 *
 * This changes ONLY which header the chain/locator anchors at — never
 * consensus validity, coins, or H*. active_chain_height()/tip() defer to the
 * reducer authority once tip_finalize registers it, and getblockcount/health
 * read H* (the committed reducer prefix), so a served tip seated above coins
 * is the designed recovered-datadir state (see tip_finalize_stage_init), not a
 * false "synced" claim. Idempotent (work-ranked promotion never downgrades)
 * and safe: a later coins-reconcile (bii_anchor) only restores the tip UP to
 * the coins height, never below this frontier. */
static void promote_best_header_after_load(struct main_state *ms,
                                           struct block_index **sorted,
                                           size_t count)
{
    if (!ms || !sorted || count == 0)
        return;

    struct block_index *best = NULL;
    for (size_t i = 0; i < count; i++) {
        struct block_index *pi = sorted[i];
        if (!pi || !pi->phashBlock)
            continue;
        if (pi->nStatus & BLOCK_FAILED_MASK)
            continue;
        if (!best) {
            best = pi;
            continue;
        }
        bool have_work = !arith_uint256_is_zero(&pi->nChainWork) &&
                         !arith_uint256_is_zero(&best->nChainWork);
        bool better = have_work
            ? arith_uint256_compare(&pi->nChainWork, &best->nChainWork) > 0
            : pi->nHeight > best->nHeight;
        if (better)
            best = pi;
    }
    if (!best)
        return;

    /* (1) Publish the header frontier through the work-ranked CSR seam — the
     * same primitive header_admit_stage uses on the live path. Idempotent:
     * a strictly-better candidate wins, otherwise this is a no-op. */
    bool promoted = false;
    enum csr_result prc = csr_promote_header_tip(
        csr_instance(), &ms->chain_active, &ms->pindex_best_header, best,
        "loader_best_header", &promoted);
    if (prc != CSR_OK) {
#ifdef ZCL_TESTING
        /* Isolated fixtures do not boot the process-wide CSR — mirror
         * header_admit_stage's test-only fallback so the frontier is still
         * published. */
        if (prc == CSR_REJECTED_NOT_INITIALIZED) {
            struct block_index *current = ms->pindex_best_header;
            bool advance = current == NULL;
            if (current && current != best) {
                bool have_work =
                    !arith_uint256_is_zero(&best->nChainWork) &&
                    !arith_uint256_is_zero(&current->nChainWork);
                advance = have_work
                    ? arith_uint256_compare(&best->nChainWork,
                                            &current->nChainWork) > 0
                    : best->nHeight > current->nHeight;
            }
            if (advance)
                ms->pindex_best_header = best;
        } else
#endif
        LOG_WARN("block_index",
                 "loader: best-header promotion rejected code=%s h=%d",
                 csr_result_name(prc), best->nHeight);
    }

    /* (2) Seat the frontier in the active_chain[] window so active_chain_tip()
     * is non-NULL over a non-empty map. Only advance — never downgrade a
     * higher window a prior step may have installed. chain_set_active_tip
     * publishes the served-tip authority + events; a NULL/OOM refusal is
     * non-fatal (pindex_best_header still carries the frontier and the
     * NULL-tip locator anchors there). */
    struct block_index *cur = active_chain_cached_tip(&ms->chain_active);
    if (!cur || best->nHeight > cur->nHeight) {
        struct zcl_result r = chain_set_active_tip(ms, best,
                                                   TIP_FROM_P2P_REPAIR,
                                                   "loader_best_header");
        if (!r.ok)
            LOG_WARN("block_index",
                     "loader: best-header window seat failed h=%d: %s",
                     best->nHeight, r.message);
    }
}

/* ── save_block_index_flat ───────────────────────────────── */

/* Payload writer for the embedded single-file format. Streams the
 * "ZCLI"+count+entries payload at the current file offset (48, right
 * after the integrity header the shared helper owns) while hashing
 * exactly those bytes.
 * The shared helper back-patches the {magic, version, size, sha3}
 * header and publishes the whole file with ONE atomic rename, so the
 * integrity commitment and the bytes it certifies can never diverge
 * across a crash. */
struct bif_emit_ctx {
    struct block_index **sorted;
    size_t               count;
};

static bool bif_emit_payload(FILE *f, void *vctx,
                             uint64_t *out_payload_size,
                             uint8_t out_payload_sha3[32])
{
    struct bif_emit_ctx *c = (struct bif_emit_ctx *)vctx;

    struct sha3_256_ctx hctx;
    sha3_256_init(&hctx);
    uint64_t bytes = 0;

    uint32_t magic = 0x5A434C49; /* "ZCLI" payload magic */
    uint32_t count32 = (uint32_t)c->count;
    if (fwrite(&magic, 4, 1, f) != 1 || // disk-io-lock: private-fd (block index flat file)
        fwrite(&count32, 4, 1, f) != 1) {
        LOG_FAIL("block_index_flat",
                 "save_block_index_flat: payload header write failed");
    }
    sha3_256_write(&hctx, (const uint8_t *)&magic, 4);
    sha3_256_write(&hctx, (const uint8_t *)&count32, 4);
    bytes += 8;

    for (size_t i = 0; i < c->count; i++) {
        struct block_index_flat entry;
        memset(&entry, 0, sizeof(entry));
        if (c->sorted[i]->phashBlock)
            memcpy(entry.hash, c->sorted[i]->phashBlock->data, 32);
        if (c->sorted[i]->pprev && c->sorted[i]->pprev->phashBlock)
            memcpy(entry.prev_hash, c->sorted[i]->pprev->phashBlock->data, 32);
        entry.height = c->sorted[i]->nHeight;
        entry.n_bits = c->sorted[i]->nBits;
        entry.n_time = c->sorted[i]->nTime;
        entry.n_version = c->sorted[i]->nVersion;
        entry.n_status = c->sorted[i]->nStatus;
        entry.n_file = c->sorted[i]->nFile;
        entry.n_data_pos = c->sorted[i]->nDataPos;
        entry.n_undo_pos = c->sorted[i]->nUndoPos;
        entry.n_tx = c->sorted[i]->nTx;
        entry.n_chain_tx = c->sorted[i]->nChainTx;
        memcpy(entry.chain_work, c->sorted[i]->nChainWork.pn, 32);
        entry.n_cached_branch_id = (uint32_t)c->sorted[i]->nCachedBranchId;
        memcpy(entry.sapling_root, c->sorted[i]->hashFinalSaplingRoot.data, 32);
        if (fwrite(&entry, sizeof(entry), 1, f) != 1) { // disk-io-lock: private-fd
            LOG_FAIL("block_index_flat",
                     "save_block_index_flat: write failed at entry "
                     "%zu/%zu: %s", i, c->count, strerror(errno));
        }
        sha3_256_write(&hctx, (const uint8_t *)&entry, sizeof(entry));
        bytes += sizeof(entry);
    }

    sha3_256_finalize(&hctx, out_payload_sha3);
    *out_payload_size = bytes;
    return true;
}

void save_block_index_flat(const char *datadir, struct main_state *ms)
{
    size_t count = ms->map_block_index.size;
    struct block_index **sorted = zcl_malloc(count * sizeof(void *), "block_index sorted save");
    if (!sorted) {
        LOG_WARN("block_index_flat",
                 "save_block_index_flat: malloc failed for %zu entries",
                 count);
        return;
    }

    size_t idx = 0, iter = 0;
    struct block_index *p;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &p)) {
        if (p && idx < count) sorted[idx++] = p;
    }
    count = idx;

    qsort(sorted, count, sizeof(struct block_index *), cmp_height);

    int64_t t0 = (int64_t)platform_time_wall_time_t();

    /* ONE file, ONE rename. The 48-byte integrity header (magic
     * "BIIE", version 2, payload size + SHA3-256) prefixes the body
     * inside block_index.bin itself, so a kill anywhere before the
     * single rename leaves only the old good file — there is no second
     * sidecar file and therefore no inter-rename window that can strand
     * a fresh body under a stale commitment. No lock is taken on this
     * path (it only iterates the single-threaded block_map), so the
     * shutdown/drive lock order is unaffected. */
    struct bif_emit_ctx ectx = { .sorted = sorted, .count = count };
    struct zcl_result wr = bii_write_embedded(datadir, bif_emit_payload, &ectx);
    free(sorted);
    if (!wr.ok) {
        LOG_WARN("save_block_index_flat",
                 "save_block_index_flat: embedded write failed: %s",
                 wr.message);
        return;
    }

    int64_t elapsed = (int64_t)platform_time_wall_time_t() - t0;
    LOG_INFO("block_index_flat",
             "Block index flat file: %zu entries, %zuMB (%llds)",
             count, count * sizeof(struct block_index_flat) / (1024*1024),
             (long long)elapsed);
}

/* ── load_block_index_flat ───────────────────────────────── */

/* Tier-2 P2 fast-restart arm (see header). Single-shot; consumed on the next
 * load_block_index_flat. Boot-time only; not a hot-swappable TU. */
static struct {
    bool     armed;
    int64_t  expected_count;
    uint8_t  tip_hash[32];
    int64_t  tip_height;
} g_trust_flat_arm;

void block_index_loader_arm_trust_flat_fields(int64_t expected_count,
                                              const uint8_t tip_hash[32],
                                              int64_t tip_height)
{
    memset(&g_trust_flat_arm, 0, sizeof(g_trust_flat_arm));
    if (expected_count > 0 && tip_hash) {
        g_trust_flat_arm.armed = true;
        g_trust_flat_arm.expected_count = expected_count;
        memcpy(g_trust_flat_arm.tip_hash, tip_hash, 32);
        g_trust_flat_arm.tip_height = tip_height;
    }
}

struct zcl_result load_block_index_flat(const char *datadir, struct main_state *ms)
{
    char path[1024];
    snprintf(path, sizeof(path), "%s/block_index.bin", datadir);

    int fd = open(path, O_RDONLY);
    if (fd < 0)
        return ZCL_ERR(-1, "block_index_flat: cannot open %s: %s",
                       path, strerror(errno));

    struct stat st;
    if (fstat(fd, &st) != 0) {
        int saved_errno = errno;
        close(fd);
        return ZCL_ERR(-2, "block_index_flat: fstat failed: %s",
                       strerror(saved_errno));
    }
    size_t file_size = (size_t)st.st_size;
    if (file_size < 8) {
        close(fd);
        return ZCL_ERR(-3, "block_index_flat: file too small (%zu bytes)",
                       file_size);
    }

    uint8_t *data = mmap(NULL, file_size, PROT_READ, MAP_PRIVATE, fd, 0);
    close(fd);
    if (data == MAP_FAILED)
        return ZCL_ERR(-4, "block_index_flat: mmap failed (%zu bytes): %s",
                       file_size, strerror(errno));

    /* Format detection: the embedded single-file format prefixes the
     * body with a 48-byte integrity header (magic "BIIE"). The legacy
     * two-file format starts directly with the "ZCLI" payload magic at
     * offset 0 and carries its integrity in a separate .sha3 sidecar.
     * We peek 4 bytes to choose the payload offset, then — for the
     * embedded format — VERIFY the embedded SHA3 over the payload
     * BEFORE trusting a single byte. */
    uint64_t payload_off = 0;
    {
        uint32_t lead;
        memcpy(&lead, data, 4);
        uint32_t embedded_magic;
        memcpy(&embedded_magic, BII_EMBEDDED_MAGIC, 4);
        if (lead == embedded_magic) {
            /* New format — re-hash the payload against the embedded
             * header; refuse loudly on any mismatch so boot falls
             * through to the loader's quarantine path rather than
             * trusting unverified bytes. */
            struct ssio_sidecar_header ehdr;
            int ev = bii_verify_embedded(datadir, &ehdr, &payload_off);
            if (ev != 0) {
                munmap(data, file_size);
                return ZCL_ERR(-5, "block_index_flat: embedded integrity check "
                               "FAILED (verdict=%d) — refusing the body", ev);
            }
        }
        /* else: legacy "ZCLI"-magic body — payload_off stays 0; the
         * sidecar bii_verify gate downstream owns its integrity. */
    }

    uint32_t magic, count;
    memcpy(&magic, data + payload_off, 4);
    memcpy(&count, data + payload_off + 4, 4);
    if (magic != 0x5A434C49) {
        munmap(data, file_size);
        return ZCL_ERR(-6, "block_index_flat: bad payload magic 0x%08x "
                       "(expected 0x5A434C49)", magic);
    }
    if (count > 10000000) {
        munmap(data, file_size);
        return ZCL_ERR(-7, "block_index_flat: count %u too large (max 10M)",
                       count);
    }
    if (count == 0) {
        /* An empty index is useless and would make entries[count-1] below an
         * out-of-bounds read (entries[-1]); reject so the caller re-derives. */
        munmap(data, file_size);
        return ZCL_ERR(-8, "block_index_flat: empty index (count 0)");
    }

    size_t expected = payload_off + 8 + (size_t)count * sizeof(struct block_index_flat);
    if (file_size < expected) {
        munmap(data, file_size);
        return ZCL_ERR(-9, "block_index_flat: truncated — %zu bytes < %zu "
                       "expected (%u entries)", file_size, expected, count);
    }

    int64_t t0 = (int64_t)platform_time_wall_time_t();
    int64_t t0_ms = platform_time_monotonic_ms();  /* ms-resolution split timer */
    const struct block_index_flat *entries =
        (const struct block_index_flat *)(data + payload_off + 8);

    /* Pre-size hash map + arena. Pre-fault memory. */
    block_map_reserve(&ms->map_block_index, count);
    struct block_index *arena = zcl_calloc(count, sizeof(struct block_index), "block_index arena");
    if (!arena) {
        munmap(data, file_size);
        return ZCL_ERR(-10, "block_index_flat: calloc failed for %u entries "
                       "(%zu bytes)", count,
                       (size_t)count * sizeof(struct block_index));
    }
    memset(arena, 0, count * sizeof(struct block_index)); /* pre-fault */

    /* Height→index for O(1) pprev linking */
    int32_t max_h = entries[count - 1].height;
    struct block_index **by_height = NULL;
    if (max_h > 0 && max_h < 10000000) {
        by_height = zcl_calloc((size_t)(max_h + 1), sizeof(struct block_index *), "block_index by_height");
        if (!by_height)
            LOG_WARN("block_index_flat", "block_index_flat: by_height calloc failed " "(%d entries) — pprev linking will be slow", max_h + 1);
    }

    /* Bulk insert directly into the hash table; loader is single-threaded. */
    struct block_map *bm = &ms->map_block_index;
    for (uint32_t i = 0; i < count; i++) {
        if (uint256_is_null((const struct uint256 *)entries[i].hash))
            continue;

        struct block_index *pindex = &arena[i];
        block_index_init(pindex);

        uint64_t h;
        memcpy(&h, entries[i].hash, 8);
        size_t slot = h & (bm->capacity - 1);
        bool duplicate = false;
        while (bm->buckets[slot].occupied) {
            if (uint256_eq(&bm->buckets[slot].hash,
                           (const struct uint256 *)entries[i].hash)) {
                duplicate = true;
                break;
            }
            slot = (slot + 1) & (bm->capacity - 1);
        }
        if (duplicate) continue;
        memcpy(bm->buckets[slot].hash.data, entries[i].hash, 32);
        bm->buckets[slot].index = pindex;
        bm->buckets[slot].occupied = true;
        bm->size++;

        /* Option A: point phashBlock at per-node storage, not the bucket.
         * The bucket keeps its own .hash key (memcpy above) for lookups. */
        memcpy(pindex->hashBlock.data, entries[i].hash, 32);
        pindex->phashBlock = &pindex->hashBlock;
        pindex->nHeight = entries[i].height;
        pindex->nBits = entries[i].n_bits;
        pindex->nTime = entries[i].n_time;
        pindex->nVersion = entries[i].n_version;
        pindex->nStatus = entries[i].n_status;
        pindex->nFile = entries[i].n_file;
        pindex->nDataPos = entries[i].n_data_pos;
        pindex->nUndoPos = entries[i].n_undo_pos;
        pindex->nTx = entries[i].n_tx;
        pindex->nChainTx = entries[i].n_chain_tx;
        memcpy(pindex->nChainWork.pn, entries[i].chain_work, 32);
        pindex->nCachedBranchId = entries[i].n_cached_branch_id;
        memcpy(pindex->hashFinalSaplingRoot.data, entries[i].sapling_root, 32);

        if (by_height && pindex->nHeight >= 0 && pindex->nHeight <= max_h)
            by_height[pindex->nHeight] = pindex;
    }

    /* Link pprev via prev_hash lookup (handles orphans correctly).
     * Height-based linking breaks when orphan blocks at the same height
     * overwrite the main chain entry in by_height[], causing the pprev
     * chain to follow orphan forks instead of the main chain. */
    for (uint32_t i = 0; i < count; i++) {
        struct block_index *pindex = &arena[i];
        /* Link by the entry's own prev_hash, NOT by its stored height:
         * an entry persisted with a corrupt height 0 (a detached root a
         * relabel stamped) is skipped by an `nHeight > 0` test and stays
         * detached on every boot. An all-zero prev_hash is genesis. */
        bool has_prev = false;
        for (int pb = 0; pb < 32; pb++)
            if (entries[i].prev_hash[pb]) { has_prev = true; break; }
        if (has_prev) {
            /* Look up prev_hash in block_map */
            struct uint256 prev;
            memcpy(prev.data, entries[i].prev_hash, 32);
            struct block_index *pp = block_map_find(bm, &prev);
            if (pp)
                pindex->pprev = pp;
            else if (by_height && pindex->nHeight - 1 >= 0 &&
                     pindex->nHeight - 1 <= max_h) {
                pindex->pprev = by_height[pindex->nHeight - 1]; /* fallback */
                LOG_WARN("block_index_flat",
                         "WARNING: pprev for height %d resolved via height fallback "
                         "(prev_hash not found in block_map)",
                         pindex->nHeight);
                /* Validation pack: a by-height fallback is exactly the
                 * ambiguity a label splice exploits — counted so it
                 * surfaces in `zclassic23 dumpstate validation_pack`. */
                invariant_sentinel_note_loader_height_fallback();
            }
        }
    }
    free(by_height);

    /* Timing only (no behavior change): the qsort + forward pass below is a
     * distinct cost class from the parse/insert loop above (it sorts and walks
     * all ~3M entries a second time). Split them so the warm-start profile can
     * tell parse/insert time from forward-pass time. Cheap monotonic reads. */
    int64_t t_parse_ms = platform_time_monotonic_ms() - t0_ms;
    int64_t t_fwd_ms = platform_time_monotonic_ms();

    /* Tier-2 P2 fast restart: consume the trust-flat arm. Under a verified
     * clean-shutdown binding the flat file's stored nChainWork/nChainTx/skip/
     * branch-id already equal what the forward pass would recompute (payload
     * SHA3 verified above), so skip the O(n log n) re-derivation — but ONLY
     * when the file's count AND the marker's tip_hash@height both bind here;
     * otherwise run the full pass (bit-identical to today). */
    bool trust_flat = false;
    if (g_trust_flat_arm.armed) {
        bool count_ok = ((int64_t)count == g_trust_flat_arm.expected_count);
        struct uint256 th;
        memcpy(th.data, g_trust_flat_arm.tip_hash, 32);
        struct block_index *fr_tip = count_ok ? block_map_find(bm, &th) : NULL;
        trust_flat = count_ok && fr_tip &&
                     (int64_t)fr_tip->nHeight == g_trust_flat_arm.tip_height;
        if (!trust_flat)
            LOG_WARN("block_index_flat",
                     "fast-restart trust-flat arm did NOT bind (count %u vs "
                     "%lld) — running full forward-pass re-derivation",
                     count, (long long)g_trust_flat_arm.expected_count);
        memset(&g_trust_flat_arm, 0, sizeof(g_trust_flat_arm)); /* single-shot */
    }

    /* Recompute every pointer-graph-derived field through the canonical
     * forward pass (nChainWork, nChainTx, skip links, cached branch id,
     * failed-child propagation) — the same helper the LevelDB loader and
     * the projection rebuild use. The flat file may carry stale values
     * for blocks saved mid-sync. The forward pass zeroes nChainTx
     * whenever an ancestor is header-only (pprev->nChainTx == 0), so
     * nChainTx > 0 means "every ancestor's tx count is known" — without
     * this, wrongly-eligible entries reach find_most_work_chain.
     * Inserted entries are marked by phashBlock != NULL (dropped and
     * duplicate arena slots never get it set). */
    if (trust_flat) {
        printf("[boot]   %-28s %s\n", "blkidx.flat_forward_pass",
               "skipped (fast-restart verified-clean binding)");
    } else {
        struct block_index **sorted =
            zcl_malloc((size_t)count * sizeof(*sorted), "flat forward pass");
        if (sorted) {
            size_t n = 0;
            for (uint32_t i = 0; i < count; i++) {
                if (arena[i].phashBlock)
                    sorted[n++] = &arena[i];
            }
            qsort(sorted, n, sizeof(*sorted), cmp_height);
            block_index_forward_pass(sorted, n);
            free(sorted);
        } else {
            /* boot's later multi-pass nChainTx propagation still runs;
             * work/skip recompute is what we lose — log it. */
            LOG_WARN("block_index_flat", "block_index_flat: forward-pass alloc failed "
                     "(%u entries) — chain stats may be stale", count);
        }
    }

    munmap(data, file_size);

    t_fwd_ms = platform_time_monotonic_ms() - t_fwd_ms;
    printf("[boot]   %-28s %lldms\n", "blkidx.flat_parse_insert",
           (long long)t_parse_ms);
    if (!trust_flat)
        printf("[boot]   %-28s %lldms\n", "blkidx.flat_forward_pass",
               (long long)t_fwd_ms);

    int64_t elapsed = (int64_t)platform_time_wall_time_t() - t0;
    LOG_INFO("block_index_flat",
             "Block index flat: loaded %u entries in %llds",
             count, (long long)elapsed);

    return ZCL_OK;
}

/* ── save_block_index_recent ─────────────────────────────── */

void save_block_index_recent(struct node_db *ndb, struct main_state *ms)
{
    if (!ndb || !ndb->open) return;

    size_t total = ms->map_block_index.size;
    if (total == 0) return;

    int64_t t0 = (int64_t)platform_time_wall_time_t();
    bool tx_open = false;
    int exec_rc = ar_exec_write_sql(ndb->db, "DELETE FROM block_index_cache");
    if (exec_rc != SQLITE_OK) {
        LOG_WARN("boot_index",
                 "boot-index: failed to clear block_index_cache: %s",
                 sqlite3_errmsg(ndb->db));
        return;
    }
    exec_rc = sqlite3_exec(ndb->db, "BEGIN", NULL, NULL, NULL);
    if (exec_rc != SQLITE_OK) {
        LOG_WARN("boot_index",
                 "boot-index: failed to begin block_index_cache save: %s",
                 sqlite3_errmsg(ndb->db));
        return;
    }
    tx_open = true;

    sqlite3_stmt *ins = NULL;
    if (sqlite3_prepare_v2(ndb->db,
        "INSERT OR REPLACE INTO block_index_cache "
        "(hash,prev_hash,height,n_bits,n_time,n_version,n_status,"
        "n_file,n_data_pos,n_undo_pos,n_tx,chain_work,"
        "n_cached_branch_id,n_chain_tx) "
        "VALUES (?,?,?,?,?,?,?,?,?,?,?,?,?,?)",
        -1, &ins, NULL) != SQLITE_OK || !ins) {
        LOG_WARN("boot_index",
                 "boot-index: failed to prepare block_index_cache insert: %s",
                 sqlite3_errmsg(ndb->db));
        sqlite3_exec(ndb->db, "ROLLBACK", NULL, NULL, NULL);
        return;
    }

    static const unsigned char zero32[32] = {0};
    size_t iter = 0, count = 0;
    struct block_index *p;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &p)) {
        if (!p || !p->phashBlock) continue;

        sqlite3_reset(ins);
        if (sqlite3_bind_blob(ins, 1, p->phashBlock->data, 32, SQLITE_STATIC) != SQLITE_OK)
            goto fail;
        const unsigned char *prev = (p->pprev && p->pprev->phashBlock)
            ? p->pprev->phashBlock->data : zero32;
        if (sqlite3_bind_blob(ins, 2, prev, 32, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_bind_int(ins, 3, p->nHeight) != SQLITE_OK ||
            sqlite3_bind_int(ins, 4, (int)p->nBits) != SQLITE_OK ||
            sqlite3_bind_int(ins, 5, (int)p->nTime) != SQLITE_OK ||
            sqlite3_bind_int(ins, 6, p->nVersion) != SQLITE_OK ||
            sqlite3_bind_int(ins, 7, (int)p->nStatus) != SQLITE_OK ||
            sqlite3_bind_int(ins, 8, p->nFile) != SQLITE_OK ||
            sqlite3_bind_int(ins, 9, (int)p->nDataPos) != SQLITE_OK ||
            sqlite3_bind_int(ins, 10, (int)p->nUndoPos) != SQLITE_OK ||
            sqlite3_bind_int(ins, 11, (int)p->nTx) != SQLITE_OK ||
            sqlite3_bind_blob(ins, 12, p->nChainWork.pn, 32, SQLITE_STATIC) != SQLITE_OK ||
            sqlite3_bind_int(ins, 13, (int)p->nCachedBranchId) != SQLITE_OK ||
            sqlite3_bind_int(ins, 14, (int)p->nChainTx) != SQLITE_OK ||
            AR_STEP_WRITE(ins) != SQLITE_DONE)
            goto fail;
        count++;

        if (count % 50000 == 0) {
            if (sqlite3_exec(ndb->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK)
                goto fail;
            tx_open = false;
            if (sqlite3_exec(ndb->db, "BEGIN", NULL, NULL, NULL) != SQLITE_OK)
                goto fail;
            tx_open = true;
        }
    }
    sqlite3_finalize(ins);
    ins = NULL;
    if (sqlite3_exec(ndb->db, "COMMIT", NULL, NULL, NULL) != SQLITE_OK) {
        LOG_WARN("boot_index",
                 "boot-index: failed to commit block_index_cache save: %s",
                 sqlite3_errmsg(ndb->db));
        sqlite3_exec(ndb->db, "ROLLBACK", NULL, NULL, NULL);
        return;
    }

    int64_t elapsed = (int64_t)platform_time_wall_time_t() - t0;
    LOG_INFO("boot_index",
             "Block index: cached %zu/%zu entries in SQLite (%llds)",
             count, total, (long long)elapsed);
    return;

fail:
    LOG_WARN("chain", "boot-index: block_index_cache save aborted: %s", sqlite3_errmsg(ndb->db));
    if (ins)
        sqlite3_finalize(ins);
    if (tx_open)
        sqlite3_exec(ndb->db, "ROLLBACK", NULL, NULL, NULL);
}

/* ── load_block_index_sqlite ─────────────────────────────── */

struct zcl_result load_block_index_sqlite(struct node_db *ndb, struct main_state *ms)
{
    if (!ndb || !ndb->open)
        return ZCL_ERR(-1, "load_block_index_sqlite: called with null or closed db");

    int64_t cached_count = 0;
    sqlite3_stmt *cnt = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "SELECT COUNT(*) FROM block_index_cache", -1, &cnt, NULL) == SQLITE_OK && cnt) {
        if (AR_STEP_ROW_READONLY(cnt) == SQLITE_ROW)
            cached_count = sqlite3_column_int64(cnt, 0);
        sqlite3_finalize(cnt);
    }
    if (cached_count < 1000)
        return ZCL_ERR(-2, "load_block_index_sqlite: SQLite "
                       "block_index_cache too small: %lld entries",
                       (long long)cached_count);

    int64_t t0 = (int64_t)platform_time_wall_time_t();
    LOG_INFO("block_index",
             "Loading block index from SQLite (%lld entries)...",
             (long long)cached_count);

    sqlite3_stmt *sel = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "SELECT hash,prev_hash,height,n_bits,n_time,n_version,n_status,"
            "n_file,n_data_pos,n_undo_pos,n_tx,chain_work,"
            "n_cached_branch_id,n_chain_tx "
            "FROM block_index_cache ORDER BY height",
            -1, &sel, NULL) != SQLITE_OK || !sel)
        return ZCL_ERR(-3, "load_block_index_sqlite: failed to prepare SQLite SELECT for block_index_cache");

    size_t loaded = 0;
    while (AR_STEP_ROW_READONLY(sel) == SQLITE_ROW) {
        const void *hash_blob = sqlite3_column_blob(sel, 0);
        if (!hash_blob || sqlite3_column_bytes(sel, 0) < 32) continue;

        struct uint256 hash;
        memcpy(hash.data, hash_blob, 32);
        struct block_index *pindex = chainstate_insert_block_index(
            (struct chainstate *)ms, &hash);
        if (!pindex) continue;

        pindex->nHeight         = sqlite3_column_int(sel, 2);
        pindex->nBits           = (uint32_t)sqlite3_column_int(sel, 3);
        pindex->nTime           = (uint32_t)sqlite3_column_int(sel, 4);
        pindex->nVersion        = sqlite3_column_int(sel, 5);
        pindex->nStatus         = (uint32_t)sqlite3_column_int(sel, 6);
        pindex->nFile           = sqlite3_column_int(sel, 7);
        pindex->nDataPos        = (uint32_t)sqlite3_column_int(sel, 8);
        pindex->nUndoPos        = (uint32_t)sqlite3_column_int(sel, 9);
        pindex->nTx             = (uint32_t)sqlite3_column_int(sel, 10);

        const void *cw = sqlite3_column_blob(sel, 11);
        if (cw && sqlite3_column_bytes(sel, 11) >= 32)
            memcpy(pindex->nChainWork.pn, cw, 32);

        pindex->nCachedBranchId = (uint32_t)sqlite3_column_int(sel, 12);
        pindex->nChainTx        = (uint32_t)sqlite3_column_int(sel, 13);
        loaded++;
    }
    sqlite3_finalize(sel);

    /* Link pprev pointers after every cached index entry is loaded. */
    sel = NULL;
    if (sqlite3_prepare_v2(ndb->db,
            "SELECT hash,prev_hash FROM block_index_cache "
            "WHERE prev_hash != X'0000000000000000000000000000000000000000000000000000000000000000'",
            -1, &sel, NULL) == SQLITE_OK && sel) {
        while (AR_STEP_ROW_READONLY(sel) == SQLITE_ROW) {
            const void *h = sqlite3_column_blob(sel, 0);
            const void *ph = sqlite3_column_blob(sel, 1);
            if (!h || !ph) continue;
            if (sqlite3_column_bytes(sel, 0) < 32 ||
                sqlite3_column_bytes(sel, 1) < 32) continue;

            struct uint256 hash, prev;
            memcpy(hash.data, h, 32);
            memcpy(prev.data, ph, 32);

            struct block_index *pindex = block_map_find(&ms->map_block_index, &hash);
            struct block_index *pprev = block_map_find(&ms->map_block_index, &prev);
            if (pindex && pprev)
                pindex->pprev = pprev;
        }
        sqlite3_finalize(sel);
    }

    int64_t elapsed = (int64_t)platform_time_wall_time_t() - t0;
    LOG_INFO("block_index",
             "Block index SQLite: loaded %zu entries in %llds",
             loaded, (long long)elapsed);

    if (loaded == 0)
        return ZCL_ERR(-4, "load_block_index_sqlite: 0 rows loaded from "
                       "%lld cached entries", (long long)cached_count);
    return ZCL_OK;
}

/* ── load_block_index (LevelDB + post-process) ──────────── */

static struct block_index *insert_block_index_cb(void *ctx_ptr,
                                                  const struct uint256 *hash)
{
    struct main_state *ms = (struct main_state *)ctx_ptr;
    return chainstate_insert_block_index(
        (struct chainstate *)ms, hash);
}

struct zcl_result load_block_index(struct main_state *ms,
                       const struct chain_params *params,
                       struct block_tree_db *btdb, bool btdb_open)
{
    if (btdb_open) {
        if (!block_tree_db_load_block_index_guts(btdb,
                                                  insert_block_index_cb, ms))
            return ZCL_ERR(-1, "load_block_index: LevelDB block-tree "
                           "deserialization failed (corrupt block tree db)");
    }

    /* Option A: ensure every node owns its hash in per-node storage and
     * phashBlock references it (not the reallocatable bucket array).
     * The inner loader loop and chainstate_insert_block_index already do
     * this at insert; this pass is a belt-and-suspenders re-seed that is
     * also safe under concurrent grow (it writes per-node storage, never
     * re-points into buckets). */
    {
        size_t iter = 0;
        struct block_index *pi;
        const struct uint256 *hash;
        while (block_map_next(&ms->map_block_index, &iter, &hash, &pi)) {
            if (pi && hash) {
                pi->hashBlock = *hash;
                pi->phashBlock = &pi->hashBlock;
            }
        }
    }

    if (ms->map_block_index.size == 0) {
        struct block_index *genesis = chainstate_insert_block_index(
            (struct chainstate *)ms,
            &params->consensus.hashGenesisBlock);
        if (genesis) {
            genesis->nHeight = 0;
            genesis->nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
            genesis->nTx = 1;
            genesis->nChainTx = 1;
            genesis->nBits = 0x1f07ffff;
            genesis->nChainWork = GetBlockProof(genesis);
            struct chain_state_rollback_authorization rollback_auth = {
                .source = CSR_ROLLBACK_SOURCE_RESTORE,
                .decision = POLICY_ALLOW,
                .from_height = active_chain_height(&ms->chain_active),
                .to_height = genesis->nHeight,
                .max_depth = INT64_MAX,
                .evidence_class = "block_index_loader_genesis_verified",
                .reason = "loader_init_genesis",
            };
            struct chain_state_commit commit = {
                .new_tip = genesis,
                .new_coins_best = *genesis->phashBlock,
                .expected_utxo_count = 0,
                .update_header_tip = true,
                .rollback_auth = &rollback_auth,
                .wallet_scan_height = -1,
                .reason = "loader_init_genesis",
            };
            enum csr_result rc = csr_commit_tip(csr_instance(), &commit);
            if (rc == CSR_OK) {
                return ZCL_OK;
            }
#ifdef ZCL_TESTING
            if (rc == CSR_REJECTED_NOT_INITIALIZED) {
                (void)chain_set_active_tip(ms, genesis, TIP_FROM_RESTORE,
                                      "loader_init_genesis_csr_uninit");
                ms->pindex_best_header = genesis;
                return ZCL_OK;
            }
#endif
            return ZCL_ERR(-3, "load_block_index: csr rejected genesis tip "
                           "commit (%s)", csr_result_name(rc));
        }
        return ZCL_OK;
    }

    /* Post-load: compute nChainWork, nChainTx, skip links */
    size_t count = ms->map_block_index.size;
    struct block_index **sorted = zcl_malloc(count * sizeof(struct block_index *), "block_index sorted load");
    if (!sorted)
        return ZCL_ERR(-2, "load_block_index: out of memory allocating %zu "
                       "sorted block_index pointers", count);

    size_t idx = 0;
    size_t iter = 0;
    struct block_index *pindex;
    while (block_map_next(&ms->map_block_index, &iter, NULL, &pindex)) {
        if (pindex && idx < count)
            sorted[idx++] = pindex;
    }
    count = idx;

    qsort(sorted, count, sizeof(struct block_index *), cmp_height);

    block_index_forward_pass(sorted, count);

    /* THE LINCHPIN: make the header frontier real so active_chain_tip() is not
     * NULL over this just-loaded map and header sync anchors above genesis. */
    promote_best_header_after_load(ms, sorted, count);

    free(sorted);
    return ZCL_OK;
}

/* ── load_block_index_from_blocks_table ──────────────────── */

/* Column order shared by the validate + insert passes of the blocks-table
 * hydrator. Keep these in lock-step with the SELECT string BLOCKS_HYDRATE_SEL. */
enum {
    BHC_HASH = 0, BHC_PREV, BHC_VERSION, BHC_MERKLE, BHC_SAPLING,
    BHC_TIME, BHC_BITS, BHC_NONCE, BHC_SOLUTION, BHC_STATUS,
    BHC_NUMTX, BHC_HEIGHT
};

#define BLOCKS_HYDRATE_SEL \
    "SELECT hash,prev_hash,version,merkle_root,sapling_root,time,bits," \
    "nonce,solution,status,num_tx,height FROM blocks ORDER BY height"

/* Reconstruct the canonical block_header from a BLOCKS_HYDRATE_SEL row so its
 * PoW hash can be recomputed and compared to the stored `hash` column (the
 * same hash-bind test app/models/src/block.c performs on a stored row). Returns
 * false when a fixed header field is missing/short or the Equihash solution is
 * empty/oversize — every such row makes the whole hydration refuse. */
static bool blocks_row_to_header(sqlite3_stmt *s, struct block_header *out)
{
    block_header_init(out);
    out->nVersion = (int32_t)sqlite3_column_int(s, BHC_VERSION);

    const void *prev = sqlite3_column_blob(s, BHC_PREV);
    if (!prev || sqlite3_column_bytes(s, BHC_PREV) < 32)
        return false;
    memcpy(out->hashPrevBlock.data, prev, 32);

    const void *mr = sqlite3_column_blob(s, BHC_MERKLE);
    if (!mr || sqlite3_column_bytes(s, BHC_MERKLE) < 32)
        return false;
    memcpy(out->hashMerkleRoot.data, mr, 32);

    /* sapling_root is a nullable projection column; a NULL/short value leaves
     * the header field at zero, which only hash-binds for a block whose real
     * hashFinalSaplingRoot is genuinely zero (pre-Sapling). A post-Sapling row
     * with a dropped root simply fails the hash-bind check below — refused,
     * never silently accepted. */
    const void *sr = sqlite3_column_blob(s, BHC_SAPLING);
    if (sr && sqlite3_column_bytes(s, BHC_SAPLING) >= 32)
        memcpy(out->hashFinalSaplingRoot.data, sr, 32);

    out->nTime = (uint32_t)sqlite3_column_int64(s, BHC_TIME);
    out->nBits = (uint32_t)sqlite3_column_int64(s, BHC_BITS);

    const void *nn = sqlite3_column_blob(s, BHC_NONCE);
    if (!nn || sqlite3_column_bytes(s, BHC_NONCE) < 32)
        return false;
    memcpy(out->nNonce.data, nn, 32);

    int sol_len = sqlite3_column_bytes(s, BHC_SOLUTION);
    const void *sol = sqlite3_column_blob(s, BHC_SOLUTION);
    if (!sol || sol_len <= 0 || sol_len > MAX_SOLUTION_SIZE)
        return false;
    memcpy(out->nSolution, sol, (size_t)sol_len);
    out->nSolutionSize = (size_t)sol_len;
    return true;
}

/* Hydrate the in-memory block_index map from the node.db `blocks` table.
 *
 * The `--importblockindex <src>` CLI bulk-loads ~3.1M header rows into `blocks`
 * (app/controllers/src/snapshot_controller_import.c) but writes NO flat file,
 * NO block_index_cache, and NO datadir LevelDB. So every other loader rung
 * leaves a genesis-only map on a freshly-imported datadir and the node serves
 * H*=0 until the legacy pull rescues it. This rung closes that hole.
 *
 * Validation (matches what the sibling rungs trust vs re-check): every row's
 * header is re-serialized and its PoW hash recomputed; a row whose stored
 * `hash` does not equal that recomputed value REFUSES the WHOLE hydration (a
 * loud named blocker), so a poisoned `blocks` table can never seed a partial
 * map — the map is left exactly as the caller passed it and boot falls through
 * to the LevelDB / legacy rungs. Parent linkage is asserted structurally via
 * the pprev link pass. NOTE the imported rows carry chain_work=0 (the importer
 * never populates it), so nChainWork is recomputed from the linked pprev chain
 * by block_index_forward_pass, exactly like the flat/LevelDB rungs.
 *
 * Validity is installed HONESTLY as header-only: the entry's BLOCK_VALID level
 * is clamped to at most BLOCK_VALID_TREE and NO HAVE_DATA/HAVE_UNDO bit is set
 * (we hold no block body — bodies are fetched lazily via P2P). Higher validity
 * is never fabricated from the stored status.
 *
 * Returns .ok=true on a non-empty hydrate; .ok=false (map untouched by the
 * refusal path) on an empty table, a prepare error, or the first non-hash-bound
 * row. */
struct zcl_result load_block_index_from_blocks_table(struct node_db *ndb,
                                                     struct main_state *ms)
{
    if (!ndb || !ndb->open)
        return ZCL_ERR(-1, "load_block_index_from_blocks_table: null/closed db");
    if (!ms)
        return ZCL_ERR(-1, "load_block_index_from_blocks_table: null main_state");

    int64_t row_count = 0;
    sqlite3_stmt *cnt = NULL;
    if (sqlite3_prepare_v2(ndb->db, "SELECT COUNT(*) FROM blocks", -1, &cnt,
                           NULL) == SQLITE_OK && cnt) {
        if (AR_STEP_ROW_READONLY(cnt) == SQLITE_ROW)
            row_count = sqlite3_column_int64(cnt, 0);
        sqlite3_finalize(cnt);
    }
    if (row_count <= 0)
        return ZCL_ERR(-2, "load_block_index_from_blocks_table: `blocks` "
                       "table empty — nothing to hydrate");

    int64_t t0 = (int64_t)platform_time_wall_time_t();
    LOG_INFO("block_index",
             "Hydrating block index from node.db `blocks` (%lld rows)...",
             (long long)row_count);

    /* ── Pass A: VALIDATE every row hash-binds BEFORE touching the map. A
     *    single poisoned row refuses the WHOLE hydration so a corrupt `blocks`
     *    table never seeds a partial map. */
    sqlite3_stmt *sel = NULL;
    if (sqlite3_prepare_v2(ndb->db, BLOCKS_HYDRATE_SEL, -1, &sel, NULL)
            != SQLITE_OK || !sel)
        return ZCL_ERR(-3, "load_block_index_from_blocks_table: failed to "
                       "prepare validate SELECT over `blocks`");

    int64_t validated = 0;
    while (AR_STEP_ROW_READONLY(sel) == SQLITE_ROW) {
        const void *hb = sqlite3_column_blob(sel, BHC_HASH);
        int h = sqlite3_column_int(sel, BHC_HEIGHT);
        if (!hb || sqlite3_column_bytes(sel, BHC_HASH) < 32) {
            sqlite3_finalize(sel);
            LOG_WARN("block_index",
                     "blocks-hydrate: row at height=%d has no/short hash — "
                     "refusing hydration (poisoned `blocks` table)", h);
            return ZCL_ERR(-4, "load_block_index_from_blocks_table: short hash "
                           "at height=%d", h);
        }
        struct block_header hdr;
        if (!blocks_row_to_header(sel, &hdr)) {
            sqlite3_finalize(sel);
            LOG_WARN("block_index",
                     "blocks-hydrate: row at height=%d has an unusable header "
                     "(missing field or empty/oversize solution) — refusing "
                     "hydration", h);
            return ZCL_ERR(-4, "load_block_index_from_blocks_table: unusable "
                           "header at height=%d", h);
        }
        struct uint256 computed;
        block_header_get_hash(&hdr, &computed);
        if (memcmp(computed.data, hb, 32) != 0) {
            sqlite3_finalize(sel);
            LOG_WARN("block_index",
                     "blocks-hydrate: stored row does not hash-bind at "
                     "height=%d — refusing hydration (poisoned `blocks` "
                     "table)", h);
            return ZCL_ERR(-5, "load_block_index_from_blocks_table: row does "
                           "not hash-bind at height=%d", h);
        }
        validated++;
    }
    sqlite3_finalize(sel);
    sel = NULL;
    if (validated == 0)
        return ZCL_ERR(-6, "load_block_index_from_blocks_table: 0 usable rows");

    /* ── Pass B: INSERT (rows are hash-bound now). ORDER BY height means the
     *    collected array is height-ASC, so pprev is always resolvable in the
     *    link pass and the forward pass sees each parent before its child. */
    struct block_index **sorted = zcl_malloc(
        (size_t)validated * sizeof(*sorted), "blocks_table hydrate sorted");
    if (!sorted)
        return ZCL_ERR(-7, "load_block_index_from_blocks_table: OOM for %lld "
                       "sorted pointers", (long long)validated);

    if (sqlite3_prepare_v2(ndb->db, BLOCKS_HYDRATE_SEL, -1, &sel, NULL)
            != SQLITE_OK || !sel) {
        free(sorted);
        return ZCL_ERR(-3, "load_block_index_from_blocks_table: failed to "
                       "prepare insert SELECT over `blocks`");
    }

    size_t n = 0;
    while (AR_STEP_ROW_READONLY(sel) == SQLITE_ROW && n < (size_t)validated) {
        const void *hb = sqlite3_column_blob(sel, BHC_HASH);
        if (!hb || sqlite3_column_bytes(sel, BHC_HASH) < 32)
            continue;   /* validated in pass A; defensive */
        struct uint256 hh;
        memcpy(hh.data, hb, 32);
        struct block_index *bi = chainstate_insert_block_index(
            (struct chainstate *)ms, &hh);
        if (!bi)
            continue;

        bi->nHeight  = sqlite3_column_int(sel, BHC_HEIGHT);
        bi->nVersion = (int32_t)sqlite3_column_int(sel, BHC_VERSION);
        bi->nTime    = (uint32_t)sqlite3_column_int64(sel, BHC_TIME);
        bi->nBits    = (uint32_t)sqlite3_column_int64(sel, BHC_BITS);
        {
            int nt = sqlite3_column_int(sel, BHC_NUMTX);
            if (nt > 0)
                bi->nTx = (unsigned int)nt;
        }

        const void *mr = sqlite3_column_blob(sel, BHC_MERKLE);
        if (mr && sqlite3_column_bytes(sel, BHC_MERKLE) >= 32)
            memcpy(bi->hashMerkleRoot.data, mr, 32);
        const void *sr = sqlite3_column_blob(sel, BHC_SAPLING);
        if (sr && sqlite3_column_bytes(sel, BHC_SAPLING) >= 32)
            memcpy(bi->hashFinalSaplingRoot.data, sr, 32);
        const void *nn = sqlite3_column_blob(sel, BHC_NONCE);
        if (nn && sqlite3_column_bytes(sel, BHC_NONCE) >= 32)
            memcpy(bi->nNonce.data, nn, 32);

        /* HONEST validity: we verified the header hash-binds (and the link
         * pass verifies parent linkage), but we hold NO block body and have
         * NOT checked tx/script/context validity. Clamp the BLOCK_VALID level
         * to at most BLOCK_VALID_TREE and assert NO HAVE_DATA/HAVE_UNDO — the
         * node fetches bodies lazily via P2P. Never fabricate a higher
         * validity than the stored row, and never above TREE. */
        unsigned int stored = (unsigned int)sqlite3_column_int(sel, BHC_STATUS);
        unsigned int level = stored & (unsigned int)BLOCK_VALID_MASK;
        if (level > (unsigned int)BLOCK_VALID_TREE)
            level = (unsigned int)BLOCK_VALID_TREE;
        bi->nStatus = level;   /* header-only: no HAVE bits, no FAILED bits */

        sorted[n++] = bi;
    }
    sqlite3_finalize(sel);
    sel = NULL;

    /* ── Pass C: link pprev by prev_hash (both endpoints now in the map). */
    if (sqlite3_prepare_v2(ndb->db, "SELECT hash,prev_hash FROM blocks",
                           -1, &sel, NULL) == SQLITE_OK && sel) {
        while (AR_STEP_ROW_READONLY(sel) == SQLITE_ROW) {
            const void *h = sqlite3_column_blob(sel, 0);
            const void *ph = sqlite3_column_blob(sel, 1);
            if (!h || !ph)
                continue;
            if (sqlite3_column_bytes(sel, 0) < 32 ||
                sqlite3_column_bytes(sel, 1) < 32)
                continue;
            struct uint256 hash, prev;
            memcpy(hash.data, h, 32);
            memcpy(prev.data, ph, 32);
            struct block_index *bi = block_map_find(&ms->map_block_index, &hash);
            struct block_index *pprev =
                block_map_find(&ms->map_block_index, &prev);
            if (bi && pprev)
                bi->pprev = pprev;
        }
        sqlite3_finalize(sel);
    }

    /* Post-load, identical to the LevelDB rung: recompute nChainWork/nChainTx/
     * skip from the linked pprev chain (the imported chain_work column is
     * zero), then promote the best header so active_chain_tip() is non-NULL
     * over the just-hydrated map and header sync anchors above genesis. */
    block_index_forward_pass(sorted, n);
    promote_best_header_after_load(ms, sorted, n);
    free(sorted);

    int64_t elapsed = (int64_t)platform_time_wall_time_t() - t0;
    LOG_INFO("block_index",
             "Block index `blocks` hydrate: %zu header-only entries in %llds",
             n, (long long)elapsed);

    if (n == 0)
        return ZCL_ERR(-6, "load_block_index_from_blocks_table: 0 entries "
                       "hydrated");
    return ZCL_OK;
}

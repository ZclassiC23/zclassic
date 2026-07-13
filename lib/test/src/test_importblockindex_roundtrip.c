/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_importblockindex_roundtrip — execution coverage for
 * snapshot_import_block_index() (app/controllers/src/
 * snapshot_controller_import.c), the function src/main.c's
 * `--importblockindex <src-datadir> [<target-node.db>]` dispatches to
 * directly. That CLI flag is the mandatory first step of the two-step
 * cold-sync recipe (~3.17M headers in ~52s on the real zclassicd datadir,
 * see CLAUDE.md "Tenacity & recovery") and, before this test, had ZERO
 * execution coverage — a regression here silently breaks every fresh mint.
 *
 * The function is called from two real production sites that pass
 * DIFFERENT header_only values:
 *   - src/main.c's --importblockindex CLI: always header_only=true
 *     (positions force-zeroed; header-only, bodies fetched lazily via P2P).
 *   - the parallel snapshot-bundle loader (same file,
 *     snapshot_import_job_start -> import_block_index_thread via
 *     thread_registry_spawn): header_only=false (positions copied
 *     VERBATIM from the source LevelDB record) — this is the code path
 *     where nDataPos preservation is load-bearing, including the
 *     historical bug shape where a low file-0 offset (e.g. 1711) got
 *     silently translated/doubled instead of passed through untouched.
 *
 * Both shapes are exercised here against the SAME synthetic legacy
 * blocks/index LevelDB (built with the real disk_block_index_serialize()
 * wire format, not hand-rolled bytes) so a regression in either call shape
 * of the shared import function is caught.
 *
 * make t ONLY=importblockindex_roundtrip
 */

#include "test/test_helpers.h"

#include "chain/chain.h"           /* BLOCK_HAVE_DATA / BLOCK_HAVE_UNDO / BLOCK_VALID_* */
#include "controllers/snapshot_controller.h"
#include "core/serialize.h"
#include "models/block.h"
#include "models/database.h"
#include "storage/block_index_db.h"
#include "storage/dbwrapper.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define IBR_N_BLOCKS 32

#define IBR_CHECK(name, expr) do {                              \
    printf("importblockindex_roundtrip: %s... ", (name));       \
    if ((expr)) printf("OK\n");                                 \
    else { printf("FAIL\n"); failures++; }                      \
} while (0)

static int ibr_mkdir_p(const char *p)
{
    if (mkdir(p, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

struct ibr_block_fixture {
    uint8_t  hash[32];
    uint8_t  prev_hash[32];
    uint8_t  merkle_root[32];
    uint32_t time;
    uint32_t bits;
    unsigned int status;   /* nStatus as written to the source record */
    int32_t  nfile;
    uint32_t ndatapos;
    uint32_t nundopos;
    int64_t  sapling_value;
};

/* Synthesize a tiny legacy `<src_dir>/blocks/index` LevelDB with N
 * CDiskBlockIndex-shaped 'b'-prefixed records, using the REAL
 * disk_block_index_serialize() wire format (not hand-rolled bytes).
 * Chains hashPrev height-to-height like a real header chain. Block h=1
 * deliberately carries the historical file-0 low-offset bug shape
 * (nFile=0, nDataPos=1711) — the exact value class ("h=1's payload
 * position 1711") an old candidate once silently translated to 3414
 * instead of passing through untouched. Records the expected values into
 * fx[] for the caller's later assertions. Returns false on any failure. */
static bool ibr_build_fixture(const char *src_dir,
                              struct ibr_block_fixture *fx, int n)
{
    char idx_dir[512];
    snprintf(idx_dir, sizeof(idx_dir), "%s/blocks/index", src_dir);

    struct db_wrapper dbw;
    if (!db_wrapper_open(&dbw, idx_dir, 4 << 20, false, true)) {
        fprintf(stderr, "ibr_build_fixture: db_wrapper_open failed: %s\n",
                idx_dir);
        return false;
    }

    uint8_t prev[32];
    memset(prev, 0, sizeof(prev));
    bool ok = true;

    for (int h = 0; h < n && ok; h++) {
        struct ibr_block_fixture *b = &fx[h];
        memset(b, 0, sizeof(*b));

        /* Deterministic, never-all-zero hash (validates_presence_of
         * requires it regardless of height). */
        b->hash[0] = 0xAA;
        b->hash[1] = (uint8_t)(h & 0xff);
        b->hash[2] = (uint8_t)((h >> 8) & 0xff);
        b->hash[31] = 0x01;

        memcpy(b->prev_hash, prev, 32);

        b->merkle_root[0] = 0xBB;
        b->merkle_root[1] = (uint8_t)(h & 0xff);
        b->merkle_root[31] = 0x02;

        b->time = 1231006505u + (uint32_t)h;
        b->bits = 0x1d00ffffu;
        /* VALID_TRANSACTIONS | HAVE_DATA | HAVE_UNDO — a normal
         * fully-connected zclassicd block-index record. */
        b->status = (unsigned int)(BLOCK_VALID_TRANSACTIONS |
                                   BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO);
        /* Split across two files for realism; heights 0-15 -> file 0
         * (where the historical bug shape lived), 16-31 -> file 1. */
        b->nfile = (h < 16) ? 0 : 1;
        /* The historical bug shape: h=1, file 0, a SMALL offset (1711) —
         * exactly the case an old candidate mistranslated. */
        b->ndatapos = (h == 1) ? 1711u : (2000u + (uint32_t)h * 100u);
        b->nundopos = b->ndatapos + 500u;
        b->sapling_value = (int64_t)h * 1000;

        struct disk_block_index dbi;
        disk_block_index_init(&dbi);
        dbi.nHeight = h;
        memcpy(dbi.hashPrev.data, b->prev_hash, 32);
        dbi.nStatus = b->status;
        dbi.nTx = 1;
        dbi.nFile = b->nfile;
        dbi.nDataPos = b->ndatapos;
        dbi.nUndoPos = b->nundopos;
        dbi.nVersion = 4;
        memcpy(dbi.hashMerkleRoot.data, b->merkle_root, 32);
        dbi.nTime = b->time;
        dbi.nBits = b->bits;
        dbi.nSolutionSize = 0;
        dbi.has_sprout_value = false;
        dbi.nSaplingValue = b->sapling_value;

        struct byte_stream s;
        stream_init(&s, 256);
        if (!disk_block_index_serialize(&dbi, &s) || s.error) {
            fprintf(stderr, "ibr_build_fixture: serialize failed h=%d\n", h);
            ok = false;
        } else {
            char key[33];
            key[0] = 'b';
            memcpy(key + 1, b->hash, 32);
            if (!db_write(&dbw, key, sizeof(key), (const char *)s.data,
                          s.size, false)) {
                fprintf(stderr, "ibr_build_fixture: db_write failed h=%d\n", h);
                ok = false;
            }
        }
        stream_free(&s);

        memcpy(prev, b->hash, 32);
    }

    db_wrapper_close(&dbw);
    return ok;
}

/* Assert every fixture row (hash/prev_hash/merkle_root/time/bits) landed in
 * the target blocks table unchanged, regardless of header_only mode — those
 * fields are never touched by the header_only branch. */
static void ibr_check_header_fields(int *failures, struct node_db *ndb,
                                    const struct ibr_block_fixture *fx, int n)
{
    bool all_match = true;
    for (int h = 0; h < n; h++) {
        struct db_block row;
        if (!db_block_find_by_height(ndb, h, &row)) {
            fprintf(stderr, "  >> missing row at height %d\n", h);
            all_match = false;
            continue;
        }
        if (memcmp(row.hash, fx[h].hash, 32) != 0 ||
            memcmp(row.prev_hash, fx[h].prev_hash, 32) != 0 ||
            memcmp(row.merkle_root, fx[h].merkle_root, 32) != 0 ||
            row.time != fx[h].time || row.bits != fx[h].bits) {
            fprintf(stderr, "  >> header field mismatch at height %d\n", h);
            all_match = false;
        }
    }
    IBR_CHECK("header fields (hash/prev_hash/merkle_root/time/bits) match "
              "the source for every height", all_match);
}

int test_importblockindex_roundtrip(void)
{
    int failures = 0;

    ibr_mkdir_p("./test-tmp");
    char base[300];
    test_fmt_tmpdir(base, sizeof(base), "importblockindex_roundtrip", "main");
    ibr_mkdir_p(base);

    char src_dir[340];
    snprintf(src_dir, sizeof(src_dir), "%s/legacy-src", base);
    ibr_mkdir_p(src_dir);

    struct ibr_block_fixture fx[IBR_N_BLOCKS];
    bool fixture_ok = ibr_build_fixture(src_dir, fx, IBR_N_BLOCKS);
    IBR_CHECK("fixture: synthesize N=32 legacy blocks/index LevelDB records "
              "(incl. file-0 low-offset shape nDataPos=1711 at h=1)",
              fixture_ok);
    if (!fixture_ok)
        return failures + 1;

    /* ==================================================================
     * Scenario A — header_only=true: the LITERAL --importblockindex CLI
     * dispatch shape (src/main.c:2552). Positions must be force-zeroed
     * (never leaked/garbled from the source), HAVE_DATA/HAVE_UNDO cleared.
     * ================================================================== */
    {
        char db_path[380];
        snprintf(db_path, sizeof(db_path), "%s/node_a.db", base);

        int count = -1;
        bool ok = snapshot_import_block_index(src_dir, db_path,
                                              /*header_only=*/true, &count);
        IBR_CHECK("scenario A (header-only, CLI dispatch shape): import "
                  "returns ok", ok);
        IBR_CHECK("scenario A: imported row count == N", count == IBR_N_BLOCKS);

        struct node_db ndb;
        IBR_CHECK("scenario A: target node.db opens", node_db_open(&ndb, db_path));

        ibr_check_header_fields(&failures, &ndb, fx, IBR_N_BLOCKS);

        bool positions_zeroed = true, status_masked = true;
        for (int h = 0; h < IBR_N_BLOCKS; h++) {
            struct db_block row;
            if (!db_block_find_by_height(&ndb, h, &row)) { positions_zeroed = false; continue; }
            if (row.file_num != 0 || row.data_pos != 0 || row.undo_pos != 0)
                positions_zeroed = false;
            unsigned int expect_status =
                fx[h].status & ~(unsigned int)(BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO);
            if ((unsigned int)row.status != expect_status)
                status_masked = false;
        }
        IBR_CHECK("scenario A: file_num/data_pos/undo_pos force-zeroed for "
                  "EVERY height (not leaked/garbled from the source, even "
                  "though the fixture carries nFile=1 and large nDataPos "
                  "values for h>=16)", positions_zeroed);
        IBR_CHECK("scenario A: status has HAVE_DATA/HAVE_UNDO cleared, other "
                  "bits (BLOCK_VALID_TRANSACTIONS) preserved", status_masked);

        int64_t pprev_h = -2, shielded_h = -2;
        bool cursors_ok = node_db_state_get_int(&ndb, "pprev_repaired_height", &pprev_h) &&
                          node_db_state_get_int(&ndb, "shielded_backfill_height", &shielded_h);
        IBR_CHECK("scenario A: fast-boot cursors (pprev_repaired_height, "
                  "shielded_backfill_height) stamped to max height (N-1)",
                  cursors_ok && pprev_h == IBR_N_BLOCKS - 1 &&
                  shielded_h == IBR_N_BLOCKS - 1);

        int count_before = db_block_count(&ndb);
        node_db_close(&ndb);

        /* Idempotency: re-run the SAME import against the SAME target. The
         * function's own `DELETE FROM blocks` at the top of
         * import_block_index_thread makes this a real assertion — a
         * regression that dropped/moved that reset would double the row
         * count here (2N), not just leave it at N. */
        int count2 = -1;
        bool ok2 = snapshot_import_block_index(src_dir, db_path,
                                               /*header_only=*/true, &count2);
        IBR_CHECK("scenario A: re-running import is idempotent (ok)", ok2);
        IBR_CHECK("scenario A: re-running import reports the same count", count2 == IBR_N_BLOCKS);

        struct node_db ndb2;
        IBR_CHECK("scenario A: target node.db reopens after re-import",
                  node_db_open(&ndb2, db_path));
        int count_after = db_block_count(&ndb2);
        IBR_CHECK("scenario A: row count unchanged after re-import (== N, "
                  "not 2N)", count_before == IBR_N_BLOCKS && count_after == IBR_N_BLOCKS);
        node_db_close(&ndb2);
    }

    /* ==================================================================
     * Scenario B — header_only=false: the OTHER real caller shape of the
     * same shared function (the parallel snapshot-bundle loader's
     * import_block_index_thread invocation, which leaves header_only at
     * its struct-memset default of false). Positions must be copied
     * VERBATIM — this is the path where the historical "nDataPos 1711
     * translated to 3414" bug shape actually bites.
     * ================================================================== */
    {
        char db_path[380];
        snprintf(db_path, sizeof(db_path), "%s/node_b.db", base);

        int count = -1;
        bool ok = snapshot_import_block_index(src_dir, db_path,
                                              /*header_only=*/false, &count);
        IBR_CHECK("scenario B (full positions, snapshot-bundle-loader shape): "
                  "import returns ok", ok);
        IBR_CHECK("scenario B: imported row count == N", count == IBR_N_BLOCKS);

        struct node_db ndb;
        IBR_CHECK("scenario B: target node.db opens", node_db_open(&ndb, db_path));

        ibr_check_header_fields(&failures, &ndb, fx, IBR_N_BLOCKS);

        bool positions_exact = true, status_exact = true;
        for (int h = 0; h < IBR_N_BLOCKS; h++) {
            struct db_block row;
            if (!db_block_find_by_height(&ndb, h, &row)) { positions_exact = false; continue; }
            if (row.file_num != fx[h].nfile ||
                (uint32_t)row.data_pos != fx[h].ndatapos ||
                (uint32_t)row.undo_pos != fx[h].nundopos)
                positions_exact = false;
            if ((unsigned int)row.status != fx[h].status)
                status_exact = false;
        }
        IBR_CHECK("scenario B: file_num/data_pos/undo_pos copied VERBATIM "
                  "for every height (no translation)", positions_exact);
        IBR_CHECK("scenario B: status copied verbatim (unmasked)", status_exact);

        /* The direct regression check named in the plan: the file-0
         * low-offset entry (h=1, nFile=0, nDataPos=1711) must land as
         * EXACTLY 1711 — not 3414, not any other translated value. */
        struct db_block row1;
        bool h1_ok = db_block_find_by_height(&ndb, 1, &row1);
        IBR_CHECK("scenario B: file-0 low-offset entry (h=1) nDataPos is "
                  "PRESERVED EXACTLY at 1711 (the historical bug translated "
                  "this to 3414 instead of passing it through)",
                  h1_ok && row1.file_num == 0 && row1.data_pos == 1711);

        int64_t pprev_h = -2, shielded_h = -2;
        bool cursors_ok = node_db_state_get_int(&ndb, "pprev_repaired_height", &pprev_h) &&
                          node_db_state_get_int(&ndb, "shielded_backfill_height", &shielded_h);
        IBR_CHECK("scenario B: fast-boot cursors stamped to max height (N-1)",
                  cursors_ok && pprev_h == IBR_N_BLOCKS - 1 &&
                  shielded_h == IBR_N_BLOCKS - 1);

        int count_before = db_block_count(&ndb);
        node_db_close(&ndb);

        int count2 = -1;
        bool ok2 = snapshot_import_block_index(src_dir, db_path,
                                               /*header_only=*/false, &count2);
        IBR_CHECK("scenario B: re-running import is idempotent (ok)", ok2);
        IBR_CHECK("scenario B: re-running import reports the same count", count2 == IBR_N_BLOCKS);

        struct node_db ndb2;
        IBR_CHECK("scenario B: target node.db reopens after re-import",
                  node_db_open(&ndb2, db_path));
        int count_after = db_block_count(&ndb2);
        IBR_CHECK("scenario B: row count unchanged after re-import (== N, "
                  "not 2N)", count_before == IBR_N_BLOCKS && count_after == IBR_N_BLOCKS);
        node_db_close(&ndb2);
    }

    return failures;
}

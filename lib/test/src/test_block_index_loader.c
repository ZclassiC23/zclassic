/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for block_index_loader service — flat file save/load,
 * SQLite cache save/load, round-trip integrity, and edge cases.
 */

#include "test/test_helpers.h"
#include "services/block_index_loader.h"
#include "services/block_index_integrity.h"
#include "services/chain_tip.h"
#include "storage/sha3_sidecar_io.h"
#include "storage/progress_store.h"
#include "storage/coins_kv.h"
#include "jobs/tip_finalize_stage.h"
#include "validation/main_state.h"
#include "chain/chain.h"
#include "chain/chainparams.h"
#include "chain/chainparamsbase.h"
#include "chain/pow.h"
#include "core/arith_uint256.h"
#include <sqlite3.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define BIL_CHECK(name, expr) do {          \
    printf("%s... ", (name));              \
    if ((expr)) printf("OK\n");            \
    else { printf("FAIL\n"); failures++; } \
} while (0)

/* Build a minimal block_index chain of `n` blocks in main_state. */
static void build_synthetic_chain(struct main_state *ms, int n)
{
    struct uint256 hashes[2048];
    int limit = n < 2048 ? n : 2048;

    for (int h = 0; h < limit; h++) {
        memset(&hashes[h], 0, sizeof(hashes[h]));
        hashes[h].data[0] = (uint8_t)(h & 0xFF);
        hashes[h].data[1] = (uint8_t)((h >> 8) & 0xFF);
        hashes[h].data[2] = (uint8_t)((h >> 16) & 0xFF);
        hashes[h].data[3] = 0xAA;

        struct block_index *pi = chainstate_insert_block_index(
            (struct chainstate *)ms, &hashes[h]);
        if (!pi) continue;

        pi->nHeight = h;
        pi->nBits = 0x1f07ffff;
        pi->nTime = 1000000 + (uint32_t)h * 150;
        pi->nVersion = 4;
        pi->nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
        pi->nTx = 1;
        pi->nFile = h / 1000;
        pi->nDataPos = (uint32_t)(h * 2048);

        if (h > 0) {
            struct block_index *prev = block_map_find(
                &ms->map_block_index, &hashes[h - 1]);
            if (prev) {
                pi->pprev = prev;
                struct arith_uint256 proof = GetBlockProof(pi);
                arith_uint256_add(&pi->nChainWork,
                                  &prev->nChainWork, &proof);
                pi->nChainTx = prev->nChainTx + pi->nTx;
            }
        } else {
            pi->nChainWork = GetBlockProof(pi);
            pi->nChainTx = 1;
        }
    }
}

static struct uint256 make_test_hash(int h)
{
    struct uint256 hash;
    memset(&hash, 0, sizeof(hash));
    hash.data[0] = (uint8_t)(h & 0xFF);
    hash.data[1] = (uint8_t)((h >> 8) & 0xFF);
    hash.data[2] = (uint8_t)((h >> 16) & 0xFF);
    hash.data[3] = 0xAA;
    return hash;
}

int test_block_index_loader(void)
{
    printf("\n=== block index loader tests ===\n");
    int failures = 0;

    /* ── 1. Flat file round-trip ─────────────────────────── */

    {
        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        block_map_init(&ms.map_block_index);
        active_chain_init(&ms.chain_active);
        build_synthetic_chain(&ms, 100);

        char tmpdir[256];
        snprintf(tmpdir, sizeof(tmpdir), "./test-tmp/%d_bil_flat", getpid());
        mkdir("./test-tmp", 0755);
        mkdir(tmpdir, 0755);

        save_block_index_flat(tmpdir, &ms);

        char path[512];
        snprintf(path, sizeof(path), "%s/block_index.bin", tmpdir);
        struct stat st;
        bool file_ok = (stat(path, &st) == 0 && st.st_size > 8);

        /* Embedded single-file format (task #32): the body itself starts
         * with the 48-byte "BIIE" integrity header, and NO separate
         * sidecar file is written. */
        bool embedded_header_ok = false;
        {
            FILE *bf = fopen(path, "rb");
            if (bf) {
                struct ssio_sidecar_header hdr;
                if (fread(&hdr, 1, sizeof(hdr), bf) == sizeof(hdr))
                    embedded_header_ok =
                        (memcmp(hdr.magic, BII_EMBEDDED_MAGIC, 4) == 0 &&
                         hdr.version == BII_EMBEDDED_VERSION);
                fclose(bf);
            }
        }
        char sidecar_path[512];
        snprintf(sidecar_path, sizeof(sidecar_path), "%s/block_index.bin.sha3",
                 tmpdir);
        bool no_sidecar = (stat(sidecar_path, &st) != 0);

        struct main_state ms2;
        memset(&ms2, 0, sizeof(ms2));
        block_map_init(&ms2.map_block_index);
        active_chain_init(&ms2.chain_active);

        bool loaded = file_ok && load_block_index_flat(tmpdir, &ms2);
        bool count_ok = loaded && (ms2.map_block_index.size == ms.map_block_index.size);

        bool heights_ok = count_ok;
        for (int h = 0; h < 100 && heights_ok; h++) {
            struct uint256 hash = make_test_hash(h);
            struct block_index *pi = block_map_find(&ms2.map_block_index, &hash);
            if (!pi || pi->nHeight != h) heights_ok = false;
        }

        BIL_CHECK("bil: flat save embeds integrity header, writes no sidecar",
                  file_ok && embedded_header_ok && no_sidecar);
        BIL_CHECK("bil: flat file round-trip preserves 100 entries", heights_ok);

        unlink(sidecar_path);
        unlink(path);
        rmdir(tmpdir);
        block_map_free(&ms.map_block_index);
        block_map_free(&ms2.map_block_index);
    }

    /* ── 2. Flat file pprev linking ──────────────────────── */

    {
        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        block_map_init(&ms.map_block_index);
        active_chain_init(&ms.chain_active);
        build_synthetic_chain(&ms, 50);

        char tmpdir[256];
        snprintf(tmpdir, sizeof(tmpdir), "./test-tmp/%d_bil_pprev", getpid());
        mkdir("./test-tmp", 0755);
        mkdir(tmpdir, 0755);

        save_block_index_flat(tmpdir, &ms);

        struct main_state ms2;
        memset(&ms2, 0, sizeof(ms2));
        block_map_init(&ms2.map_block_index);
        active_chain_init(&ms2.chain_active);
        load_block_index_flat(tmpdir, &ms2);

        struct uint256 tip_hash = make_test_hash(49);
        struct block_index *tip = block_map_find(&ms2.map_block_index, &tip_hash);
        bool ok = (tip != NULL && tip->nHeight == 49);

        int walk = 0;
        struct block_index *cur = tip;
        while (cur && walk < 100) { walk++; cur = cur->pprev; }
        ok = ok && (walk == 50);

        BIL_CHECK("bil: flat file pprev walk from h=49 to genesis (50 hops)", ok);

        char path[512];
        snprintf(path, sizeof(path), "%s/block_index.bin", tmpdir);
        unlink(path);
        rmdir(tmpdir);
        block_map_free(&ms.map_block_index);
        block_map_free(&ms2.map_block_index);
    }

    /* ── 3. Flat file bad magic rejected ─────────────────── */

    {
        char tmpdir[256];
        snprintf(tmpdir, sizeof(tmpdir), "./test-tmp/%d_bil_badm", getpid());
        mkdir("./test-tmp", 0755);
        mkdir(tmpdir, 0755);

        char path[512];
        snprintf(path, sizeof(path), "%s/block_index.bin", tmpdir);
        FILE *f = fopen(path, "wb");
        bool ok = (f != NULL);
        if (f) {
            uint32_t bad_magic = 0xDEADBEEF, count = 0;
            fwrite(&bad_magic, 4, 1, f);
            fwrite(&count, 4, 1, f);
            fclose(f);

            struct main_state ms;
            memset(&ms, 0, sizeof(ms));
            block_map_init(&ms.map_block_index);
            active_chain_init(&ms.chain_active);
            ok = ok && !load_block_index_flat(tmpdir, &ms);
            block_map_free(&ms.map_block_index);
        }

        BIL_CHECK("bil: flat file rejects bad magic", ok);

        unlink(path);
        rmdir(tmpdir);
    }

    /* ── 4. Flat file truncated rejected ─────────────────── */

    {
        char tmpdir[256];
        snprintf(tmpdir, sizeof(tmpdir), "./test-tmp/%d_bil_trunc", getpid());
        mkdir("./test-tmp", 0755);
        mkdir(tmpdir, 0755);

        char path[512];
        snprintf(path, sizeof(path), "%s/block_index.bin", tmpdir);
        FILE *f = fopen(path, "wb");
        bool ok = (f != NULL);
        if (f) {
            uint32_t magic = 0x5A434C49, count = 100;
            fwrite(&magic, 4, 1, f);
            fwrite(&count, 4, 1, f);
            fclose(f);

            struct main_state ms;
            memset(&ms, 0, sizeof(ms));
            block_map_init(&ms.map_block_index);
            active_chain_init(&ms.chain_active);
            ok = ok && !load_block_index_flat(tmpdir, &ms);
            block_map_free(&ms.map_block_index);
        }

        BIL_CHECK("bil: flat file rejects truncated file", ok);

        unlink(path);
        rmdir(tmpdir);
    }

    /* ── 5. SQLite round-trip ────────────────────────────── */

    {
        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        block_map_init(&ms.map_block_index);
        active_chain_init(&ms.chain_active);
        build_synthetic_chain(&ms, 1500);

        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));
        bool ok = (sqlite3_open(":memory:", &ndb.db) == SQLITE_OK);
        ndb.open = ok;

        if (ok) {
            sqlite3_exec(ndb.db,
                "CREATE TABLE block_index_cache ("
                "hash BLOB PRIMARY KEY,"
                "prev_hash BLOB,"
                "height INTEGER,"
                "n_bits INTEGER,"
                "n_time INTEGER,"
                "n_version INTEGER,"
                "n_status INTEGER,"
                "n_file INTEGER,"
                "n_data_pos INTEGER,"
                "n_undo_pos INTEGER,"
                "n_tx INTEGER,"
                "chain_work BLOB,"
                "n_cached_branch_id INTEGER,"
                "n_chain_tx INTEGER"
                ")", NULL, NULL, NULL);

            save_block_index_recent(&ndb, &ms);

            sqlite3_stmt *cnt = NULL;
            sqlite3_prepare_v2(ndb.db,
                "SELECT COUNT(*) FROM block_index_cache", -1, &cnt, NULL);
            int64_t saved = 0;
            if (sqlite3_step(cnt) == SQLITE_ROW)
                saved = sqlite3_column_int64(cnt, 0);
            sqlite3_finalize(cnt);
            ok = ok && (saved >= 1500);

            struct main_state ms2;
            memset(&ms2, 0, sizeof(ms2));
            block_map_init(&ms2.map_block_index);
            active_chain_init(&ms2.chain_active);

            ok = ok && load_block_index_sqlite(&ndb, &ms2);
            ok = ok && (ms2.map_block_index.size >= 1500);

            block_map_free(&ms2.map_block_index);
            sqlite3_close(ndb.db);
        }

        BIL_CHECK("bil: SQLite cache round-trip (1500 entries)", ok);

        block_map_free(&ms.map_block_index);
    }

    /* ── 6. SQLite rejects small cache ───────────────────── */

    {
        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        block_map_init(&ms.map_block_index);
        active_chain_init(&ms.chain_active);
        build_synthetic_chain(&ms, 500);

        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));
        bool ok = (sqlite3_open(":memory:", &ndb.db) == SQLITE_OK);
        ndb.open = ok;

        if (ok) {
            sqlite3_exec(ndb.db,
                "CREATE TABLE block_index_cache ("
                "hash BLOB PRIMARY KEY,"
                "prev_hash BLOB,"
                "height INTEGER,"
                "n_bits INTEGER,"
                "n_time INTEGER,"
                "n_version INTEGER,"
                "n_status INTEGER,"
                "n_file INTEGER,"
                "n_data_pos INTEGER,"
                "n_undo_pos INTEGER,"
                "n_tx INTEGER,"
                "chain_work BLOB,"
                "n_cached_branch_id INTEGER,"
                "n_chain_tx INTEGER"
                ")", NULL, NULL, NULL);

            save_block_index_recent(&ndb, &ms);

            struct main_state ms2;
            memset(&ms2, 0, sizeof(ms2));
            block_map_init(&ms2.map_block_index);
            active_chain_init(&ms2.chain_active);

            ok = ok && !load_block_index_sqlite(&ndb, &ms2);

            block_map_free(&ms2.map_block_index);
            sqlite3_close(ndb.db);
        }

        BIL_CHECK("bil: SQLite load rejects cache with < 1000 entries", ok);

        block_map_free(&ms.map_block_index);
    }

    /* ── 7. Flat file chain_work preserved ───────────────── */

    {
        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        block_map_init(&ms.map_block_index);
        active_chain_init(&ms.chain_active);
        build_synthetic_chain(&ms, 20);

        struct uint256 tip_hash = make_test_hash(19);
        struct block_index *orig_tip = block_map_find(&ms.map_block_index, &tip_hash);
        struct arith_uint256 orig_work = {0};
        bool ok = (orig_tip != NULL);
        if (ok) orig_work = orig_tip->nChainWork;

        char tmpdir[256];
        snprintf(tmpdir, sizeof(tmpdir), "./test-tmp/%d_bil_cw", getpid());
        mkdir("./test-tmp", 0755);
        mkdir(tmpdir, 0755);

        if (ok) save_block_index_flat(tmpdir, &ms);

        struct main_state ms2;
        memset(&ms2, 0, sizeof(ms2));
        block_map_init(&ms2.map_block_index);
        active_chain_init(&ms2.chain_active);

        if (ok) ok = load_block_index_flat(tmpdir, &ms2);
        if (ok) {
            struct block_index *loaded = block_map_find(&ms2.map_block_index, &tip_hash);
            ok = ok && loaded && (arith_uint256_compare(&loaded->nChainWork, &orig_work) == 0);
        }

        BIL_CHECK("bil: flat file preserves chain_work at height 19", ok);

        char path[512];
        snprintf(path, sizeof(path), "%s/block_index.bin", tmpdir);
        unlink(path);
        rmdir(tmpdir);
        block_map_free(&ms.map_block_index);
        block_map_free(&ms2.map_block_index);
    }

    /* ── 8. Missing flat file returns false ───────────────── */

    {
        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        block_map_init(&ms.map_block_index);
        active_chain_init(&ms.chain_active);

        bool ok = !load_block_index_flat("/nonexistent/path", &ms);

        BIL_CHECK("bil: load_block_index_flat returns false for missing file", ok);

        block_map_free(&ms.map_block_index);
    }

    /* ── 9. Embedded format: corrupted payload byte rejected ── */

    {
        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        block_map_init(&ms.map_block_index);
        active_chain_init(&ms.chain_active);
        build_synthetic_chain(&ms, 30);

        char tmpdir[256];
        snprintf(tmpdir, sizeof(tmpdir), "./test-tmp/%d_bil_corrupt", getpid());
        mkdir("./test-tmp", 0755);
        mkdir(tmpdir, 0755);
        save_block_index_flat(tmpdir, &ms);

        char path[512];
        snprintf(path, sizeof(path), "%s/block_index.bin", tmpdir);

        /* Flip a byte deep in the payload (well past the 48-byte header). */
        FILE *bf = fopen(path, "r+b");
        bool wrote = false;
        if (bf) {
            if (fseek(bf, BII_EMBEDDED_HEADER_BYTES + 64, SEEK_SET) == 0) {
                int c = fgetc(bf);
                if (c != EOF &&
                    fseek(bf, BII_EMBEDDED_HEADER_BYTES + 64, SEEK_SET) == 0) {
                    fputc(c ^ 0xFF, bf);
                    wrote = true;
                }
            }
            fclose(bf);
        }

        struct main_state ms2;
        memset(&ms2, 0, sizeof(ms2));
        block_map_init(&ms2.map_block_index);
        active_chain_init(&ms2.chain_active);
        bool rejected = wrote && !load_block_index_flat(tmpdir, &ms2);

        BIL_CHECK("bil: embedded format rejects corrupted payload", rejected);

        unlink(path);
        rmdir(tmpdir);
        block_map_free(&ms.map_block_index);
        block_map_free(&ms2.map_block_index);
    }

    /* ── 10. Embedded format: truncated file rejected ───────── */

    {
        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        block_map_init(&ms.map_block_index);
        active_chain_init(&ms.chain_active);
        build_synthetic_chain(&ms, 30);

        char tmpdir[256];
        snprintf(tmpdir, sizeof(tmpdir), "./test-tmp/%d_bil_etrunc", getpid());
        mkdir("./test-tmp", 0755);
        mkdir(tmpdir, 0755);
        save_block_index_flat(tmpdir, &ms);

        char path[512];
        snprintf(path, sizeof(path), "%s/block_index.bin", tmpdir);

        /* Truncate the file to half its size — payload no longer matches
         * the embedded body_size, so the hash/size check must reject it. */
        struct stat st;
        bool tok = (stat(path, &st) == 0 && st.st_size > 200);
        if (tok) tok = (truncate(path, st.st_size / 2) == 0);

        struct main_state ms2;
        memset(&ms2, 0, sizeof(ms2));
        block_map_init(&ms2.map_block_index);
        active_chain_init(&ms2.chain_active);
        bool rejected = tok && !load_block_index_flat(tmpdir, &ms2);

        BIL_CHECK("bil: embedded format rejects truncated file", rejected);

        unlink(path);
        rmdir(tmpdir);
        block_map_free(&ms.map_block_index);
        block_map_free(&ms2.map_block_index);
    }

    /* ── 11. Embedded format: header-only file rejected ─────── */

    {
        char tmpdir[256];
        snprintf(tmpdir, sizeof(tmpdir), "./test-tmp/%d_bil_hdronly", getpid());
        mkdir("./test-tmp", 0755);
        mkdir(tmpdir, 0755);

        char path[512];
        snprintf(path, sizeof(path), "%s/block_index.bin", tmpdir);

        /* A valid-magic embedded header that claims a non-zero payload,
         * with NO payload bytes following — the size check must reject. */
        struct ssio_sidecar_header hdr;
        memset(&hdr, 0, sizeof(hdr));
        memcpy(hdr.magic, BII_EMBEDDED_MAGIC, 4);
        hdr.version = BII_EMBEDDED_VERSION;
        hdr.body_size = 200;  /* claim payload we will not write */
        FILE *f = fopen(path, "wb");
        bool wrote = (f != NULL);
        if (f) { wrote = (fwrite(&hdr, sizeof(hdr), 1, f) == 1); fclose(f); }

        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        block_map_init(&ms.map_block_index);
        active_chain_init(&ms.chain_active);
        bool rejected = wrote && !load_block_index_flat(tmpdir, &ms);

        BIL_CHECK("bil: embedded format rejects header-only file", rejected);

        unlink(path);
        rmdir(tmpdir);
        block_map_free(&ms.map_block_index);
    }

    /* ── 12. Legacy two-file format still loads ─────────────── */

    {
        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        block_map_init(&ms.map_block_index);
        active_chain_init(&ms.chain_active);
        build_synthetic_chain(&ms, 40);

        char tmpdir[256];
        snprintf(tmpdir, sizeof(tmpdir), "./test-tmp/%d_bil_legacy", getpid());
        mkdir("./test-tmp", 0755);
        mkdir(tmpdir, 0755);

        /* Hand-write a LEGACY body: "ZCLI" magic + count + entries at
         * offset 0, no embedded header. Then stamp a matching sidecar
         * (the pre-task-#32 on-disk shape) so the first boot after deploy
         * loads cleanly. */
        char path[512];
        snprintf(path, sizeof(path), "%s/block_index.bin", tmpdir);

        /* Build the legacy body by saving the embedded format then
         * stripping the 48-byte header back off — gives a byte-identical
         * legacy payload + lets us stamp the legacy sidecar over it. */
        save_block_index_flat(tmpdir, &ms);
        bool legacy_ok = false;
        {
            FILE *src = fopen(path, "rb");
            if (src) {
                struct stat st;
                if (stat(path, &st) == 0 &&
                    (size_t)st.st_size > BII_EMBEDDED_HEADER_BYTES) {
                    size_t plen = (size_t)st.st_size - BII_EMBEDDED_HEADER_BYTES;
                    uint8_t *payload = malloc(plen);  // raw-alloc-ok:test-fixture
                    if (payload &&
                        fseek(src, BII_EMBEDDED_HEADER_BYTES, SEEK_SET) == 0 &&
                        fread(payload, 1, plen, src) == plen) {
                        fclose(src); src = NULL;
                        FILE *dst = fopen(path, "wb");
                        if (dst) {
                            legacy_ok = (fwrite(payload, 1, plen, dst) == plen);
                            fclose(dst);
                        }
                    }
                    free(payload);
                }
                if (src) fclose(src);
            }
        }
        /* Stamp the legacy sidecar over the legacy body. */
        if (legacy_ok)
            legacy_ok = bii_write_sidecar(tmpdir).ok;

        struct main_state ms2;
        memset(&ms2, 0, sizeof(ms2));
        block_map_init(&ms2.map_block_index);
        active_chain_init(&ms2.chain_active);
        bool loaded = legacy_ok && load_block_index_flat(tmpdir, &ms2);
        bool count_ok = loaded &&
            (ms2.map_block_index.size == ms.map_block_index.size);

        /* The legacy body verifies through the sidecar path. */
        bool verify_ok = count_ok &&
            (bii_verify(tmpdir, NULL, NULL, NULL, 0) == BII_OK);

        BIL_CHECK("bil: legacy two-file format still loads + verifies",
                  count_ok && verify_ok);

        char sidecar_path[512];
        snprintf(sidecar_path, sizeof(sidecar_path),
                 "%s/block_index.bin.sha3", tmpdir);
        unlink(sidecar_path);
        unlink(path);
        rmdir(tmpdir);
        block_map_free(&ms.map_block_index);
        block_map_free(&ms2.map_block_index);
    }

    /* ── 13. Legacy body, missing sidecar = SIDECAR_MISSING ── */

    {
        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        block_map_init(&ms.map_block_index);
        active_chain_init(&ms.chain_active);
        build_synthetic_chain(&ms, 40);

        char tmpdir[256];
        snprintf(tmpdir, sizeof(tmpdir), "./test-tmp/%d_bil_nosc", getpid());
        mkdir("./test-tmp", 0755);
        mkdir(tmpdir, 0755);

        char path[512];
        snprintf(path, sizeof(path), "%s/block_index.bin", tmpdir);

        /* Legacy body (strip the embedded header) with NO sidecar. */
        save_block_index_flat(tmpdir, &ms);
        bool legacy_ok = false;
        {
            FILE *src = fopen(path, "rb");
            if (src) {
                struct stat st;
                if (stat(path, &st) == 0 &&
                    (size_t)st.st_size > BII_EMBEDDED_HEADER_BYTES) {
                    size_t plen = (size_t)st.st_size - BII_EMBEDDED_HEADER_BYTES;
                    uint8_t *payload = malloc(plen);  // raw-alloc-ok:test-fixture
                    if (payload &&
                        fseek(src, BII_EMBEDDED_HEADER_BYTES, SEEK_SET) == 0 &&
                        fread(payload, 1, plen, src) == plen) {
                        fclose(src); src = NULL;
                        FILE *dst = fopen(path, "wb");
                        if (dst) {
                            legacy_ok = (fwrite(payload, 1, plen, dst) == plen);
                            fclose(dst);
                        }
                    }
                    free(payload);
                }
                if (src) fclose(src);
            }
        }

        /* The body still loads (legacy magic), and bii_verify reports
         * SIDECAR_MISSING — exactly today's first-run-after-upgrade
         * behavior, which boot accepts. */
        struct main_state ms2;
        memset(&ms2, 0, sizeof(ms2));
        block_map_init(&ms2.map_block_index);
        active_chain_init(&ms2.chain_active);
        bool loaded = legacy_ok && load_block_index_flat(tmpdir, &ms2);
        bool missing_ok = loaded &&
            (bii_verify(tmpdir, NULL, NULL, NULL, 0) == BII_SIDECAR_MISSING);

        BIL_CHECK("bil: legacy body with no sidecar = SIDECAR_MISSING (accepted)",
                  missing_ok);

        unlink(path);
        rmdir(tmpdir);
        block_map_free(&ms.map_block_index);
        block_map_free(&ms2.map_block_index);
    }

    /* ── 14. seed_tip_from_finalized: genesis-root install + REFUSE cases ──
     *
     * Regression guard for the kill-9-at-genesis recovery (2026-06-17). A
     * fresh regtest node mined N blocks, was kill-9'd, and rebooted to a NULL
     * active tip while the durable tip_finalize cursor + coins were at N. The
     * genesis-root branch of block_index_loader_seed_tip_from_finalized must
     * INSTALL the tip at N (rooted at the canonical genesis) yet REFUSE every
     * unsafe variant: an oversized walk (mainnet floor cap), a link missing
     * HAVE_DATA/VALID_SCRIPTS, a non-canonical genesis terminus, and
     * coins_applied < tip_height. It also must still cleanly extend an
     * existing live tip (cur_tip != NULL, the unchanged branch). */
    {
        chain_params_select(CHAIN_REGTEST);
        const struct chain_params *cp = chain_params_get();
        const struct uint256 gen = cp->consensus.hashGenesisBlock;

        char dir[256];
        mkdir("./test-tmp", 0755);
        test_fmt_tmpdir(dir, sizeof(dir), "bil_seedfin", "main");
        mkdir(dir, 0755);

        progress_store_close();
        bool store_ok = progress_store_open(dir);
        sqlite3 *db = store_ok ? progress_store_db() : NULL;
        BIL_CHECK("bil/seedfin: progress_store opens", store_ok && db);

        /* Build a synthetic regtest chain 0..N whose GENESIS hash is the real
         * params genesis (so the canonical-terminus check can pass), with the
         * rest of the chain HAVE_DATA + VALID_SCRIPTS + contiguous pprev. */
        const int N = 5;
        struct main_state ms;
        memset(&ms, 0, sizeof(ms));
        block_map_init(&ms.map_block_index);
        active_chain_init(&ms.chain_active);

        struct uint256 hashes[16];
        for (int h = 0; h <= N; h++) {
            if (h == 0) {
                hashes[0] = gen;
            } else {
                memset(&hashes[h], 0, sizeof(hashes[h]));
                hashes[h].data[0] = (uint8_t)(h & 0xFF);
                hashes[h].data[31] = 0xC2; /* distinct from genesis */
            }
            struct block_index *pi = chainstate_insert_block_index(
                (struct chainstate *)&ms, &hashes[h]);
            if (pi) {
                pi->nHeight = h;
                pi->nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
                pi->nTx = 1;
                pi->nBits = 0x200f0f0f;
                if (h > 0) {
                    struct block_index *prev = block_map_find(
                        &ms.map_block_index, &hashes[h - 1]);
                    pi->pprev = prev;
                    if (prev) {
                        struct arith_uint256 proof = GetBlockProof(pi);
                        arith_uint256_add(&pi->nChainWork, &prev->nChainWork,
                                          &proof);
                        pi->nChainTx = prev->nChainTx + pi->nTx;
                    }
                } else {
                    pi->nChainWork = GetBlockProof(pi);
                    pi->nChainTx = 1;
                }
            }
        }
        /* phashBlock must point at per-node storage (the canonical-terminus
         * memcmp reads node->phashBlock->data). */
        {
            size_t it = 0; struct block_index *pi; const struct uint256 *hp;
            while (block_map_next(&ms.map_block_index, &it, &hp, &pi)) {
                if (pi && hp) { pi->hashBlock = *hp; pi->phashBlock = &pi->hashBlock; }
            }
        }
        struct block_index *tipN = block_map_find(&ms.map_block_index, &hashes[N]);

        /* Register the tip_finalize stage so seed_anchor can stamp the
         * served-tip cursor (the value resolve_durable_tip reads). Without
         * tip_finalize_stage_handle() the cursor stamp is skipped and the
         * durable tip never resolves. */
        bool tf_init = tip_finalize_stage_init(&ms);
        BIL_CHECK("bil/seedfin: tip_finalize stage init", tf_init);

        /* Seed the durable finalized anchor at N (cursor -> N, own-hash). */
        bool anchor_ok = db && tf_init &&
            tip_finalize_stage_seed_anchor(N, hashes[N].data, true);
        BIL_CHECK("bil/seedfin: durable anchor seeded at N", anchor_ok);

        /* Helper: set coins_kv applied_height directly (utxo_apply convention
         * stores applied-through height; the gate compares applied >= N). */
        #define BIL_SET_APPLIED(H) do {                                       \
            if (db) {                                                         \
                sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);        \
                coins_kv_set_applied_height_in_tx(db, (H));                   \
                sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);                 \
            }                                                                 \
        } while (0)

        /* (iv-pre) coins_applied ABSENT → refuse (cur_tip NULL, no install). */
        int r_no_coins = db ? block_index_loader_seed_tip_from_finalized(
                                  &ms, cp, db) : 1;
        BIL_CHECK("bil/seedfin: REFUSE when coins_applied absent (<N)",
                  r_no_coins == 0 && active_chain_tip(&ms.chain_active) == NULL);

        /* (iv) coins_applied = N-1 (< N) → refuse (finalized>coins). */
        BIL_SET_APPLIED(N - 1);
        int r_low_coins = db ? block_index_loader_seed_tip_from_finalized(
                                   &ms, cp, db) : 1;
        BIL_CHECK("bil/seedfin: REFUSE when coins_applied < tip_height",
                  r_low_coins == 0 && active_chain_tip(&ms.chain_active) == NULL);

        /* Raise coins to N for the install + remaining structural cases. */
        BIL_SET_APPLIED(N);

        /* (ii) a mid-chain link missing HAVE_DATA → refuse. Clear it, test,
         * restore it. */
        struct block_index *mid = block_map_find(&ms.map_block_index, &hashes[3]);
        uint32_t saved_status = mid ? mid->nStatus : 0;
        if (mid) mid->nStatus &= ~(uint32_t)BLOCK_HAVE_DATA;
        int r_nodata = db ? block_index_loader_seed_tip_from_finalized(
                                &ms, cp, db) : 1;
        BIL_CHECK("bil/seedfin: REFUSE when a mid link lacks HAVE_DATA",
                  r_nodata == 0 && active_chain_tip(&ms.chain_active) == NULL);
        if (mid) mid->nStatus = saved_status;

        /* (iii) non-canonical genesis terminus → refuse. Re-point genesis's
         * stored hash to a NON-params value (height stays 0, HAVE_DATA set),
         * so the terminus memcmp fails. Restore after. */
        struct block_index *g0 = block_map_find(&ms.map_block_index, &hashes[0]);
        struct uint256 saved_g0 = g0 ? g0->hashBlock : gen;
        if (g0) {
            struct uint256 fake = gen; fake.data[0] ^= 0xFF;
            g0->hashBlock = fake;
            g0->phashBlock = &g0->hashBlock;
        }
        int r_nongen = db ? block_index_loader_seed_tip_from_finalized(
                                &ms, cp, db) : 1;
        BIL_CHECK("bil/seedfin: REFUSE when terminus is non-canonical genesis",
                  r_nongen == 0 && active_chain_tip(&ms.chain_active) == NULL);
        if (g0) { g0->hashBlock = saved_g0; g0->phashBlock = &g0->hashBlock; }

        /* (v) ALL preconditions hold → INSTALL h=N, cur_tip was NULL,
         * active_chain_tip() == tipN afterward. */
        int r_install = db ? block_index_loader_seed_tip_from_finalized(
                                 &ms, cp, db) : 0;
        BIL_CHECK("bil/seedfin: INSTALL genesis-root h=N (cur_tip NULL)",
                  r_install == 1 &&
                  active_chain_tip(&ms.chain_active) == tipN &&
                  active_chain_height(&ms.chain_active) == N);

        /* (vi) extend-live-chain branch (cur_tip != NULL) still returns 1 and
         * lands pointer-equal. Reset the active tip DOWN to a live tip at N-2
         * (a forward-only rewind is allowed for repair installs), then the
         * seed's walk N..N-1 must land on the N-2 tip and re-install N. The
         * durable cursor is still at N from the install above. */
        {
            struct block_index *live = block_map_find(&ms.map_block_index,
                                                      &hashes[N - 2]);
            (void)chain_set_active_tip(&ms, live, TIP_FROM_RESTORE,
                                       "bil_seedfin_extend_setup");
            BIL_SET_APPLIED(N);
            int r_ext = (db && live)
                ? block_index_loader_seed_tip_from_finalized(&ms, cp, db) : 0;
            BIL_CHECK("bil/seedfin: extend-live-chain branch installs h=N",
                      r_ext == 1 &&
                      active_chain_tip(&ms.chain_active) == tipN &&
                      active_chain_height(&ms.chain_active) == N);
        }

        #undef BIL_SET_APPLIED

        /* Done with the shared cursor/stage; tear it down before the isolated
         * oversized-walk case opens its OWN progress store + stage so the
         * big_h anchor cannot clobber the cases above. */
        tip_finalize_stage_shutdown();
        progress_store_close();
        block_map_free(&ms.map_block_index);
        test_cleanup_tmpdir(dir);

        /* (i) oversized walk (tip_height - 0 > MAX_GAP) → refuse. Isolated
         * store + stage, fresh ms, cur_tip NULL, a single tip node at a height
         * beyond the cap with a durable anchor there. The MAX_GAP bound check
         * fires BEFORE the walk — the load-bearing mainnet refusal. */
        {
            char dir2[256];
            test_fmt_tmpdir(dir2, sizeof(dir2), "bil_seedfin_big", "main");
            mkdir(dir2, 0755);
            progress_store_close();
            bool s2 = progress_store_open(dir2);
            sqlite3 *db2 = s2 ? progress_store_db() : NULL;

            struct main_state ms2;
            memset(&ms2, 0, sizeof(ms2));
            block_map_init(&ms2.map_block_index);
            active_chain_init(&ms2.chain_active);
            bool tf2 = tip_finalize_stage_init(&ms2);

            int big_h = BLOCK_INDEX_LOADER_SEED_MAX_GAP + 10000; /* 60000 */
            struct uint256 bh;
            memset(&bh, 0, sizeof(bh));
            bh.data[0] = 0x77; bh.data[1] = 0xEE; bh.data[31] = 0xC2;
            struct block_index *bn = chainstate_insert_block_index(
                (struct chainstate *)&ms2, &bh);
            if (bn) {
                bn->nHeight = big_h;
                bn->nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
                bn->hashBlock = bh; bn->phashBlock = &bn->hashBlock;
            }
            /* coins_applied = big_h so ONLY the MAX_GAP cap can be the refuser
             * (the precondition would otherwise also refuse). */
            if (db2) {
                sqlite3_exec(db2, "BEGIN IMMEDIATE", NULL, NULL, NULL);
                coins_kv_set_applied_height_in_tx(db2, big_h);
                sqlite3_exec(db2, "COMMIT", NULL, NULL, NULL);
            }
            bool anchor2 = db2 && tf2 &&
                tip_finalize_stage_seed_anchor(big_h, bh.data, true);
            int r_big = (db2 && anchor2)
                ? block_index_loader_seed_tip_from_finalized(&ms2, cp, db2) : 1;
            BIL_CHECK("bil/seedfin: REFUSE oversized walk (tip-0 > MAX_GAP)",
                      r_big == 0 && active_chain_tip(&ms2.chain_active) == NULL);

            tip_finalize_stage_shutdown();
            progress_store_close();
            block_map_free(&ms2.map_block_index);
            test_cleanup_tmpdir(dir2);
        }

        chain_params_select(CHAIN_MAIN);
    }

    printf("=== block index loader: %d failures ===\n", failures);
    return failures;
}

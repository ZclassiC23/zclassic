/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for block_index_loader service — flat file save/load,
 * SQLite cache save/load, round-trip integrity, and edge cases.
 */

#include "test/test_helpers.h"
#include "services/block_index_loader.h"
#include "services/block_index_integrity.h"
#include "storage/sha3_sidecar_io.h"
#include "validation/main_state.h"
#include "chain/chain.h"
#include "chain/chainparams.h"
#include "chain/pow.h"
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

    printf("=== block index loader: %d failures ===\n", failures);
    return failures;
}

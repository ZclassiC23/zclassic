/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_e2e_cold_start — hermetic END-TO-END slice for the two-step cold-sync
 * recipe (CLAUDE.md "Tenacity & recovery"): a SMALL synthetic legacy-shaped
 * source datadir feeds the REAL import path AND the REAL hydrate-at-boot
 * service entry point, in-process, at commit time.
 *
 * WHY THIS EXISTS
 * ----------------
 * "unit tests green, cold start broken" is a real regression class here: the
 * fresh-datadir header-hydration hole (--importblockindex fills node.db
 * `blocks` but every other loader rung left a genesis-only map, so a freshly
 * imported node served H*=0) was found by a LIVE run, not by CI, even though
 * both halves of the pipe already had unit coverage individually:
 *   - test_importblockindex_roundtrip.c / test_importblockindex_cli_dispatch.c
 *     prove the import half in isolation.
 *   - test_block_index_loader.c cases 15/16 prove the hydrate half in
 *     isolation, but build the `blocks` table by hand
 *     (bih_insert_header_row), never by running the real importer.
 * Neither test drives the SEAM: a poisoned or well-formed row as the real
 * importer would actually leave it. This slice closes that gap by chaining
 * the two REAL functions the two-step recipe and the `-cold-start` driver
 * both call:
 *   snapshot_import_block_index()             (app/controllers/src/
 *                                               snapshot_controller_import.c)
 *   load_block_index_from_blocks_table()       (app/services/src/
 *                                               block_index_loader.c)
 * against a synthetic legacy `blocks/index` LevelDB + blk*.dat body files
 * built with the REAL on-disk wire formats (disk_block_index_serialize(),
 * write_block_to_disk()) — not hand-rolled bytes.
 *
 * COVERAGE
 * --------
 *   (1) Build a source datadir with N=300 hash-linked headers AND real block
 *       bodies (write_block_to_disk — the same function connect_block uses).
 *       One block is read back via read_block_from_disk_pread to prove the
 *       bodies are genuine, not just header metadata.
 *   (2) Cold-start a FRESH datadir: snapshot_import_block_index() with
 *       header_only=true — the literal argv[1] `--importblockindex <src>`
 *       CLI shape (routing already proven live by
 *       test_importblockindex_cli_dispatch.c) — then
 *       load_block_index_from_blocks_table() against that fresh node.db.
 *       Asserts the map hydrates to N entries and tip linkage pprev-walks
 *       exactly N hops to the fixture root — the exact property the live
 *       incident lacked (a freshly-imported node stuck at H*=0).
 *   (3) A poisoned SOURCE row (LevelDB value's hashMerkleRoot mutated so its
 *       content no longer hashes to its own claimed key) survives import
 *       UNCHANGED (the importer copies fields verbatim, no hash-bind check)
 *       and is only caught at hydrate time, which REFUSES the WHOLE
 *       hydration (a loud named blocker, .ok=false) rather than seeding a
 *       partial map.
 *
 * FIDELITY GAP (documented, not closed here)
 * -------------------------------------------
 * This slice drives the two real service entry points directly, in-process
 * — it does NOT fork a full `zclassic23` serving boot (app_init, the eight
 * reducer stages, RPC/P2P listeners) against the cold-started datadir. A
 * full child-process boot would additionally prove: the boot loader-rung
 * ordering actually calls load_block_index_from_blocks_table when the
 * earlier rungs see a genesis-only map (config/src/boot_services.c wiring),
 * `zclassic23 status`/RPC surfaces the hydrated tip height, and the process
 * stays up. That is deliberately left to the existing heavy, self-skipping
 * child-process slices (test_importblockindex_cli_dispatch.c's pattern:
 * skip unless build/bin/zclassic23 exists and is newer than its witness
 * sources) — spawning a full serving node (port/RPC binding, background
 * validation threads, shutdown sequencing) inside this fast in-process test
 * risks exactly the flakiness/runtime cost the sibling heavy slices already
 * gate behind opt-in env vars. If the boot-ordering wiring regresses,
 * docs/AGENT_TRAPS.md + the live `zclassic23 status` check remain the
 * second line of defense; this slice's job is pinning the import->hydrate
 * DATA seam, which is exactly the seam the live incident broke on.
 *
 * make t ONLY=e2e_cold_start
 */

#include "test/test_helpers.h"

#include "chain/chain.h"                 /* BLOCK_HAVE_DATA / BLOCK_VALID_* */
#include "controllers/snapshot_controller.h"
#include "services/block_index_loader.h"
#include "storage/block_index_db.h"
#include "storage/dbwrapper.h"
#include "storage/disk_block_io.h"
#include "primitives/block.h"
#include "validation/main_state.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define E2E_N_BLOCKS   300
#define E2E_NO_POISON  (-1)

#define E2E_CHECK(name, expr) do {                              \
    printf("e2e_cold_start: %s... ", (name));                   \
    if ((expr)) printf("OK\n");                                 \
    else { printf("FAIL\n"); failures++; }                      \
} while (0)

struct e2e_fixture {
    struct uint256         hash[E2E_N_BLOCKS];
    struct disk_block_pos  pos[E2E_N_BLOCKS];
};

static int e2e_mkdir_p(const char *p)
{
    if (mkdir(p, 0700) == 0) return 0;
    if (errno == EEXIST) return 0;
    return -1;
}

/* Build a real legacy-shaped source datadir: `src_dir/blocks/index` (a
 * LevelDB block-tree, real disk_block_index_serialize() wire format) plus
 * real `src_dir/blocks/blkNNNNN.dat` bodies (write_block_to_disk() — the
 * same writer connect_block uses), for N hash-linked blocks. Each block's
 * LevelDB key is its REAL block-header PoW-style hash (computed via
 * disk_block_index_get_hash, which reconstructs and hashes the identical
 * fields load_block_index_from_blocks_table later re-derives), so a
 * downstream hash-bind check genuinely passes for every unpoisoned row.
 *
 * If poison_height >= 0, that ONE row's STORED VALUE gets a corrupted
 * hashMerkleRoot before the LevelDB write — its key (the hash children link
 * against via hashPrev, exactly as real corruption would leave it) is left
 * as the correct, originally-computed hash, so chain linkage stays
 * structurally intact but that one row no longer hash-binds to its own
 * content.
 *
 * fx->hash[h] records every block's real computed hash for the caller's
 * later assertions; fx->pos[h] records its real body position. */
static bool e2e_build_fixture_chain(const char *src_dir, int n,
                                    int poison_height,
                                    struct e2e_fixture *fx)
{
    char idx_dir[512];
    snprintf(idx_dir, sizeof(idx_dir), "%s/blocks/index", src_dir);

    struct db_wrapper dbw;
    if (!db_wrapper_open(&dbw, idx_dir, 4 << 20, false, true)) {
        fprintf(stderr, "e2e_build_fixture_chain: db_wrapper_open failed: %s\n",
                idx_dir);
        return false;
    }

    static const unsigned char msg_start[4] = { 0x24, 0xe9, 0x27, 0x64 };
    struct uint256 prev;
    memset(&prev, 0, sizeof(prev));
    bool ok = true;

    for (int h = 0; h < n && ok; h++) {
        struct block b;
        block_init(&b);
        b.header.nVersion = 4;
        b.header.hashPrevBlock = prev;
        memset(b.header.hashMerkleRoot.data, 0, 32);
        b.header.hashMerkleRoot.data[0] = 0xBB;
        b.header.hashMerkleRoot.data[1] = (uint8_t)(h & 0xff);
        b.header.hashMerkleRoot.data[2] = (uint8_t)((h >> 8) & 0xff);
        b.header.hashMerkleRoot.data[31] = 0x02;
        memset(b.header.hashFinalSaplingRoot.data, 0, 32);
        b.header.nTime = 1231006505u + (uint32_t)h;
        b.header.nBits = 0x1d00ffffu;
        memset(b.header.nNonce.data, 0, 32);
        b.header.nNonce.data[0] = 0xCC;
        b.header.nNonce.data[1] = (uint8_t)(h & 0xff);
        memset(b.header.nSolution, 0x40 + (h & 0x3f), MAX_SOLUTION_SIZE);
        b.header.nSolutionSize = MAX_SOLUTION_SIZE;

        b.num_vtx = 1;
        b.vtx = zcl_calloc(1, sizeof(struct transaction), "e2e_fixture_vtx");
        if (!b.vtx) {
            fprintf(stderr, "e2e_build_fixture_chain: vtx alloc failed h=%d\n", h);
            block_free(&b);
            ok = false;
            break;
        }
        transaction_init(&b.vtx[0]);
        if (!transaction_alloc(&b.vtx[0], 1, 1)) {
            fprintf(stderr, "e2e_build_fixture_chain: transaction_alloc "
                    "failed h=%d\n", h);
            block_free(&b);
            ok = false;
            break;
        }
        b.vtx[0].vin[0].sequence = 0xffffffff;
        b.vtx[0].vout[0].value = 10 * COIN;

        struct disk_block_pos pos = { .nFile = -1, .nPos = 0 };
        if (!write_block_to_disk(&b, &pos, src_dir, msg_start)) {
            fprintf(stderr, "e2e_build_fixture_chain: write_block_to_disk "
                    "failed h=%d\n", h);
            block_free(&b);
            ok = false;
            break;
        }

        struct disk_block_index dbi;
        disk_block_index_init(&dbi);
        dbi.nHeight = h;
        dbi.hashPrev = prev;
        dbi.nStatus = (unsigned int)(BLOCK_VALID_TRANSACTIONS |
                                     BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO);
        dbi.nTx = 1;
        dbi.nFile = pos.nFile;
        dbi.nDataPos = pos.nPos;
        dbi.nUndoPos = pos.nPos + 500u;
        dbi.nVersion = b.header.nVersion;
        dbi.hashMerkleRoot = b.header.hashMerkleRoot;
        dbi.hashFinalSaplingRoot = b.header.hashFinalSaplingRoot;
        dbi.nTime = b.header.nTime;
        dbi.nBits = b.header.nBits;
        dbi.nNonce = b.header.nNonce;
        memcpy(dbi.nSolution, b.header.nSolution, b.header.nSolutionSize);
        dbi.nSolutionSize = b.header.nSolutionSize;
        dbi.nSaplingValue = (int64_t)h * 1000;

        struct uint256 real_hash;
        disk_block_index_get_hash(&dbi, &real_hash);

        struct disk_block_index dbi_stored = dbi;
        if (h == poison_height) {
            /* Corrupt the STORED VALUE only — the key (real_hash, used for
             * chain linkage below) is untouched, matching how a bit-flip /
             * partial write would corrupt a real legacy datadir. */
            memset(dbi_stored.hashMerkleRoot.data, 0xEE, 32);
        }

        struct byte_stream s;
        stream_init(&s, 2048);
        bool ser_ok = disk_block_index_serialize(&dbi_stored, &s) && !s.error;
        if (ser_ok) {
            char key[33];
            key[0] = 'b';
            memcpy(key + 1, real_hash.data, 32);
            ser_ok = db_write(&dbw, key, sizeof(key), (const char *)s.data,
                              s.size, false);
        }
        stream_free(&s);
        if (!ser_ok) {
            fprintf(stderr, "e2e_build_fixture_chain: db_write failed h=%d\n", h);
            block_free(&b);
            ok = false;
            break;
        }

        fx->hash[h] = real_hash;
        fx->pos[h] = pos;
        prev = real_hash;
        block_free(&b);
    }

    db_wrapper_close(&dbw);
    return ok;
}

/* Round-trip one body: proves the source datadir's bodies are REAL
 * write_block_to_disk() output, not just header metadata — the fixture
 * genuinely has "headers+bodies", matching what a real legacy zclassicd
 * datadir looks like. */
static bool e2e_body_reads_back(const char *src_dir,
                                const struct e2e_fixture *fx, int height)
{
    struct block b;
    if (!read_block_from_disk_pread(&b, &fx->pos[height], src_dir))
        return false;
    bool ok = b.header.nTime == 1231006505u + (uint32_t)height &&
              b.num_vtx == 1;
    block_free(&b);
    return ok;
}

int test_e2e_cold_start(void)
{
    int failures = 0;
    printf("\n=== e2e cold start (real --importblockindex path + real "
           "hydrate-at-boot entry point) ===\n");

    char base[300];
    test_make_tmpdir(base, sizeof(base), "e2e_cold_start", "main");

    /* ── Scenario A: clean fixture — import + hydrate reach the tip ────── */
    {
        char src_dir[340];
        snprintf(src_dir, sizeof(src_dir), "%s/legacy-src-clean", base);
        e2e_mkdir_p(src_dir);

        struct e2e_fixture *fx = zcl_malloc(sizeof(*fx), "e2e_fixture_clean");
        bool built = fx && e2e_build_fixture_chain(src_dir, E2E_N_BLOCKS,
                                                    E2E_NO_POISON, fx);
        E2E_CHECK("(1) build source datadir: N=300 hash-linked headers+real "
                  "bodies (disk_block_index_serialize + write_block_to_disk)",
                  built);

        bool body_ok = built &&
            e2e_body_reads_back(src_dir, fx, E2E_N_BLOCKS - 1);
        E2E_CHECK("(1b) a source body round-trips via read_block_from_disk_"
                  "pread (bodies are real, not just header metadata)", body_ok);

        char target_db[380];
        snprintf(target_db, sizeof(target_db), "%s/node_clean.db", base);
        int count = -1;
        bool import_ok = built && snapshot_import_block_index(
            src_dir, target_db, /*header_only=*/true, &count);
        E2E_CHECK("(2a) real import path: snapshot_import_block_index("
                  "header_only=true — the literal argv[1] --importblockindex "
                  "CLI shape) imports all N headers",
                  import_ok && count == E2E_N_BLOCKS);

        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));
        bool opened = import_ok && node_db_open(&ndb, target_db);
        E2E_CHECK("(2b) fresh target node.db opens after import", opened);

        struct main_state ms;
        main_state_init(&ms);
        bool hydrate_ok = false;
        if (opened)
            hydrate_ok = load_block_index_from_blocks_table(&ndb, &ms).ok;
        E2E_CHECK("(2c) real hydrate-at-boot entry point "
                  "(load_block_index_from_blocks_table) hydrates the map "
                  "from the freshly-imported `blocks` table", hydrate_ok);

        bool size_ok = hydrate_ok &&
                       ms.map_block_index.size == (size_t)E2E_N_BLOCKS;
        E2E_CHECK("(2d) hydrated map size == N (the fresh-datadir "
                  "header-hydration hole this slice pins closed)", size_ok);

        bool tip_ok = false;
        bool honest_header_only = false;
        if (size_ok) {
            struct block_index *tip = block_map_find(&ms.map_block_index,
                                                      &fx->hash[E2E_N_BLOCKS - 1]);
            int walk = 0;
            struct block_index *cur = tip;
            while (cur && walk < E2E_N_BLOCKS + 5) { walk++; cur = cur->pprev; }
            tip_ok = tip && tip->nHeight == E2E_N_BLOCKS - 1 &&
                     walk == E2E_N_BLOCKS;

            struct block_index *mid = block_map_find(&ms.map_block_index,
                                                      &fx->hash[E2E_N_BLOCKS / 2]);
            honest_header_only = mid &&
                ((mid->nStatus & BLOCK_VALID_MASK) == BLOCK_VALID_TREE) &&
                !(mid->nStatus & BLOCK_HAVE_DATA);
        }
        E2E_CHECK("(2e) tip linkage reaches the fixture tip (pprev walks "
                  "exactly N hops to the fixture root)", tip_ok);
        E2E_CHECK("(2f) fidelity boundary: header-only import installs "
                  "entries HONESTLY (BLOCK_VALID_TREE, no HAVE_DATA) even "
                  "though the SOURCE datadir carries real bodies",
                  honest_header_only);

        if (opened) node_db_close(&ndb);
        main_state_free(&ms);
        free(fx);
    }

    /* ── Scenario B: a poisoned SOURCE row survives import, refuses at
     *    hydrate ──────────────────────────────────────────────────────── */
    {
        char src_dir[340];
        snprintf(src_dir, sizeof(src_dir), "%s/legacy-src-poison", base);
        e2e_mkdir_p(src_dir);

        struct e2e_fixture *fx = zcl_malloc(sizeof(*fx), "e2e_fixture_poison");
        int poison_h = E2E_N_BLOCKS / 2;
        bool built = fx && e2e_build_fixture_chain(src_dir, E2E_N_BLOCKS,
                                                    poison_h, fx);
        E2E_CHECK("(3a) build source datadir with ONE poisoned row "
                  "(h=N/2's stored merkle_root corrupted; its key stays the "
                  "originally-computed hash, matching real bit-flip "
                  "corruption)", built);

        char target_db[380];
        snprintf(target_db, sizeof(target_db), "%s/node_poison.db", base);
        int count = -1;
        bool import_ok = built && snapshot_import_block_index(
            src_dir, target_db, /*header_only=*/true, &count);
        E2E_CHECK("(3b) the poisoned row SURVIVES the real import unchanged "
                  "(the importer copies fields verbatim — no hash-bind check "
                  "at import time)", import_ok && count == E2E_N_BLOCKS);

        struct node_db ndb;
        memset(&ndb, 0, sizeof(ndb));
        bool opened = import_ok && node_db_open(&ndb, target_db);

        struct main_state ms;
        main_state_init(&ms);
        bool refused = false, untouched = false;
        if (opened) {
            struct zcl_result hydrate =
                load_block_index_from_blocks_table(&ndb, &ms);
            refused = !hydrate.ok;
            untouched = ms.map_block_index.size == 0;
        }
        E2E_CHECK("(3c) the real hydrate-at-boot entry point REFUSES the "
                  "WHOLE hydration loudly (.ok=false) — a poisoned source "
                  "row never seeds a partial map, even after surviving a "
                  "real import", opened && refused && untouched);

        if (opened) node_db_close(&ndb);
        main_state_free(&ms);
        free(fx);
    }

    test_rm_rf(base);

    if (failures == 0)
        printf("e2e_cold_start OK (real import -> real hydrate seam: clean "
               "fixture reaches the tip, poisoned fixture refuses loudly)\n");
    return failures;
}

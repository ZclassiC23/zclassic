/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_boot_header_seed_import — the headers-as-artifact fast path
 * (config/src/boot_header_seed_import.c).
 *
 * Proves, against the REAL file-service serve path on a loopback seeder, that a
 * fresh node can pull the header-chain seed (block_index.bin,
 * ROM_ARTIFACT_HEADER_SEED) as a swarm artifact and import it into its header
 * index — replacing the serial P2P header crawl that otherwise gates the
 * checkpoint-bundle install:
 *
 *   (a) round-trip: a seeder registers a synthetic block_index.bin; the client
 *       discovers it via the directory listing, downloads it content-verified
 *       into <datadir>/bundles/, and boot_header_seed_import_maybe imports it —
 *       pindex_best_header climbs to the artifact tip, every non-genesis row is
 *       clamped header-only (HAVE_DATA stripped so bodies re-fetch + fully
 *       re-validate), and the artifact is re-homed at the datadir root so the
 *       node re-serves it.
 *   (b) bad-row: an artifact carrying a row whose stored hash fails its PoW
 *       target is QUARANTINED per-row (dropped + counter advances + typed
 *       blocker), the import CONTINUES with the remaining rows, and the boot is
 *       never aborted.
 *
 * Fixtures live under ./test-tmp and mkdtemp() dirs — never a real datadir. The
 * synthetic index uses regtest params (easy PoW target) so honest rows pass the
 * loader's frozen CheckProofOfWork gate and a crafted max-hash row fails it. */

#include "test/test_helpers.h"

#include "config/boot_header_seed_import.h"
#include "config/boot_bundle_fetch.h"
#include "net/rom_fetch.h"
#include "net/rom_seed.h"
#include "net/file_service.h"
#include "services/block_index_loader.h"
#include "validation/main_state.h"
#include "validation/chainstate.h"
#include "chain/chain.h"
#include "chain/chainparams.h"
#include "chain/pow.h"
#include "core/arith_uint256.h"
#include "util/blocker.h"
#include "platform/time_compat.h"

#include <fcntl.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

static struct uint256 hsi_make_hash(int h)
{
    struct uint256 hash;
    memset(&hash, 0, sizeof(hash));
    hash.data[0] = (uint8_t)(h & 0xFF);
    hash.data[1] = (uint8_t)((h >> 8) & 0xFF);
    hash.data[2] = (uint8_t)((h >> 16) & 0xFF);
    hash.data[3] = 0xAA;
    return hash;
}

/* Build a minimal height-linked block_index chain of `n` blocks. When
 * bad_row_at >= 0, that height's stored hash is forced to all-0xFF (numerically
 * above any regtest PoW target) so the loader's per-row gate quarantines it. */
static void hsi_build_chain(struct main_state *ms, int n, int bad_row_at)
{
    for (int h = 0; h < n; h++) {
        struct uint256 hash = hsi_make_hash(h);
        if (h == bad_row_at)
            memset(hash.data, 0xFF, sizeof(hash.data));

        struct block_index *pi =
            chainstate_insert_block_index((struct chainstate *)ms, &hash);
        if (!pi)
            continue;
        pi->nHeight = h;
        pi->nBits = 0x1f07ffff;
        pi->nTime = 1000000 + (uint32_t)h * 150;
        pi->nVersion = 4;
        pi->nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA | BLOCK_HAVE_UNDO;
        pi->nTx = 1;
        pi->nFile = h / 1000;
        pi->nDataPos = (uint32_t)(h * 2048);
        if (h > 0) {
            struct uint256 prev_hash = hsi_make_hash(h - 1);
            if (h - 1 == bad_row_at)
                memset(prev_hash.data, 0xFF, sizeof(prev_hash.data));
            struct block_index *prev =
                block_map_find(&ms->map_block_index, &prev_hash);
            if (prev) {
                pi->pprev = prev;
                struct arith_uint256 proof = GetBlockProof(pi);
                arith_uint256_add(&pi->nChainWork, &prev->nChainWork, &proof);
                pi->nChainTx = prev->nChainTx + pi->nTx;
            }
        } else {
            pi->nChainWork = GetBlockProof(pi);
            pi->nChainTx = 1;
        }
    }
}

/* Wrap the serve-side artifacts array into the {"artifacts":[...]} object the
 * picker consumes (mirrors test_boot_bundle_fetch). */
static void hsi_directory_json(char *out, size_t cap)
{
    char arts[3072];
    size_t an = rom_seed_directory_json(arts, sizeof(arts));
    snprintf(out, cap, "{\"artifacts\":%s}", (an > 0) ? arts : "[]");
}

/* Start the loopback seeder on an OS-assigned port (no cross-checkout
 * port collisions when suites run in parallel worktrees). */
static uint16_t hsi_start_seeder(const char *sdir)
{
    fs_server_start(sdir, 0);
    for (int w = 0; w < 40 && !fs_server_is_running(); w++)
        platform_sleep_ms(50);
    return fs_server_is_running() ? fs_server_get_port() : 0;
}

/* Save a synthetic header index to `sdir`/block_index.bin and register+serve it. */
static bool hsi_seed_artifact(const char *sdir, int n, int bad_row_at,
                              struct rom_artifact *out_art)
{
    struct main_state ms;
    memset(&ms, 0, sizeof(ms));
    block_map_init(&ms.map_block_index);
    active_chain_init(&ms.chain_active);
    hsi_build_chain(&ms, n, bad_row_at);
    save_block_index_flat(sdir, &ms);
    block_map_free(&ms.map_block_index);
    active_chain_free(&ms.chain_active);

    return rom_seed_register(sdir, "block_index.bin", NULL, out_art) == ROM_REG_OK;
}

static int case_import_roundtrip(void)
{
    int failures = 0;
    TEST("boot_header_seed: swarm-download + import climbs the header frontier") {
        chain_params_select(CHAIN_REGTEST);
        rom_seed_reset();
        rom_seed_set_peer_bps_cap(1ull << 30);
        rom_seed_set_global_bps_cap(1ull << 30);

        char sroot[] = "/tmp/zcl_hsi_srv_XXXXXX";
        char *sdir = mkdtemp(sroot);
        ASSERT(sdir != NULL);

        struct rom_artifact art;
        ASSERT(hsi_seed_artifact(sdir, 100, -1, &art));
        ASSERT(art.kind == ROM_ARTIFACT_HEADER_SEED);

        uint16_t port = hsi_start_seeder(sdir);
        ASSERT(port != 0);

        /* The client picks the header-seed manifest from the seeder's real
         * directory listing and downloads it content-verified into bundles/. */
        char dirjson[3200];
        hsi_directory_json(dirjson, sizeof(dirjson));
        struct rom_fetch_manifest hm;
        memset(&hm, 0, sizeof(hm));
        ASSERT(boot_bundle_pick_header_seed_manifest(dirjson, &hm));
        ASSERT(hm.kind == ROM_ARTIFACT_HEADER_SEED);
        ASSERT(strcmp(hm.filename, "block_index.bin") == 0);
        ASSERT(memcmp(hm.chunk_root, art.chunk_root, 32) == 0);

        char croot[] = "/tmp/zcl_hsi_cli_XXXXXX";
        char *cdir = mkdtemp(croot);
        ASSERT(cdir != NULL);
        struct rom_fetch_peer peers[1];
        memset(peers, 0, sizeof(peers));
        snprintf(peers[0].addr, sizeof(peers[0].addr), "%s", "127.0.0.1");
        peers[0].port = port;
        ASSERT(boot_bundle_fetch_download(cdir, peers, 1, &hm));
        char landed[1200];
        snprintf(landed, sizeof(landed), "%s/bundles/block_index.bin", cdir);
        struct stat st;
        ASSERT(stat(landed, &st) == 0);

        /* Import into a fresh client header index. */
        struct main_state cms;
        memset(&cms, 0, sizeof(cms));
        block_map_init(&cms.map_block_index);
        active_chain_init(&cms.chain_active);

        ASSERT(boot_header_seed_import_maybe(cdir, &cms));

        /* Frontier climbed to the artifact tip; every row landed. */
        ASSERT(cms.map_block_index.size == 100);
        ASSERT(cms.pindex_best_header != NULL);
        ASSERT(cms.pindex_best_header->nHeight == 99);

        /* Header-only clamp: a non-genesis row has HAVE_DATA/UNDO stripped so
         * the body is re-fetched + fully re-validated (never trust the seeder's
         * persisted body validity). */
        struct uint256 h50 = hsi_make_hash(50);
        struct block_index *pi50 = block_map_find(&cms.map_block_index, &h50);
        ASSERT(pi50 != NULL);
        unsigned int s50 = block_index_status_load(pi50);
        ASSERT((s50 & (unsigned int)BLOCK_HAVE_DATA) == 0);
        ASSERT((s50 & (unsigned int)BLOCK_HAVE_UNDO) == 0);
        ASSERT((s50 & (unsigned int)BLOCK_VALID_MASK) <= (unsigned int)BLOCK_VALID_TREE);

        /* The artifact was re-homed at the datadir root (re-serve on this node). */
        char root_bin[1200];
        snprintf(root_bin, sizeof(root_bin), "%s/block_index.bin", cdir);
        ASSERT(stat(root_bin, &st) == 0);

        /* Idempotent: a second call no-ops (root file present, ladder-owned). */
        ASSERT(!boot_header_seed_import_maybe(cdir, &cms));

        block_map_free(&cms.map_block_index);
        active_chain_free(&cms.chain_active);
        fs_server_stop();
        test_rm_rf_recursive(cdir);
        char p[1200];
        snprintf(p, sizeof(p), "%s/block_index.bin", sdir);
        unlink(p);
        rmdir(sdir);
        rom_seed_reset();
    } _test_next:;
    return failures;
}

static int case_bad_row(void)
{
    int failures = 0;
    TEST("boot_header_seed: a bad-PoW row is quarantined, import continues") {
        chain_params_select(CHAIN_REGTEST);
        rom_seed_reset();
        rom_seed_set_peer_bps_cap(1ull << 30);
        rom_seed_set_global_bps_cap(1ull << 30);
        blocker_clear("block_index.flat_row_quarantine");

        char sroot[] = "/tmp/zcl_hsi_bad_srv_XXXXXX";
        char *sdir = mkdtemp(sroot);
        ASSERT(sdir != NULL);

        /* 100 rows h=0..99; the TIP (h=99) carries an all-0xFF stored hash that
         * exceeds its PoW target — nothing depends on it, so the rest survives. */
        struct rom_artifact art;
        ASSERT(hsi_seed_artifact(sdir, 100, 99, &art));

        uint16_t port = hsi_start_seeder(sdir);
        ASSERT(port != 0);

        char dirjson[3200];
        hsi_directory_json(dirjson, sizeof(dirjson));
        struct rom_fetch_manifest hm;
        memset(&hm, 0, sizeof(hm));
        ASSERT(boot_bundle_pick_header_seed_manifest(dirjson, &hm));

        char croot[] = "/tmp/zcl_hsi_bad_cli_XXXXXX";
        char *cdir = mkdtemp(croot);
        ASSERT(cdir != NULL);
        struct rom_fetch_peer peers[1];
        memset(peers, 0, sizeof(peers));
        snprintf(peers[0].addr, sizeof(peers[0].addr), "%s", "127.0.0.1");
        peers[0].port = port;
        ASSERT(boot_bundle_fetch_download(cdir, peers, 1, &hm));

        int64_t q_before = block_index_flat_row_quarantined();

        struct main_state cms;
        memset(&cms, 0, sizeof(cms));
        block_map_init(&cms.map_block_index);
        active_chain_init(&cms.chain_active);

        /* Import CONTINUES past the bad row (never a whole-boot abort). */
        ASSERT(boot_header_seed_import_maybe(cdir, &cms));

        /* The poisoned tip was dropped: 99 good rows land, frontier = h=98. */
        ASSERT(cms.map_block_index.size == 99);
        ASSERT(cms.pindex_best_header != NULL);
        ASSERT(cms.pindex_best_header->nHeight == 98);

        /* Quarantine counter advanced + typed blocker raised — not a silent
         * accept. */
        ASSERT(block_index_flat_row_quarantined() > q_before);
        struct blocker_snapshot found;
        ASSERT(blocker_find_by_id_prefix("block_index.flat_row_quarantine",
                                         &found));

        block_map_free(&cms.map_block_index);
        active_chain_free(&cms.chain_active);
        fs_server_stop();
        test_rm_rf_recursive(cdir);
        char p[1200];
        snprintf(p, sizeof(p), "%s/block_index.bin", sdir);
        unlink(p);
        rmdir(sdir);
        rom_seed_reset();
    } _test_next:;
    return failures;
}

int test_boot_header_seed_import(void)
{
    printf("\n=== boot_header_seed_import ===\n");
    int failures = 0;
    failures += case_import_roundtrip();
    failures += case_bad_row();
    printf("=== boot_header_seed_import: %d failure(s) ===\n", failures);
    return failures;
}

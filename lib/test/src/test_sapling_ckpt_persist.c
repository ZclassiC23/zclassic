/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Sapling note-commitment-tree flat-file checkpoint persistence (P1-7).
 *
 * Proves the fail-closed, verify-then-trust cache that lets a clean restart
 * resume the Sapling replay from a persisted frontier instead of re-folding
 * the whole history:
 *   (a) v2 save/load round-trip restores an identical frontier + root + the
 *       {height, block_hash} key.
 *   (b) a corrupted / root-mismatched cache is rejected on load (fail-closed).
 *   (c) a stale cache above the current tip is discarded, not partially used.
 *   (d) a reorg (block hash at H changed) is discarded.
 *   (e) an absent cache behaves like the current full-replay path.
 *
 * All hermetic: tree ops are Pedersen hashing over synthetic commitments —
 * no ~/.zcash-params and no live chain are needed. The header-chain binding
 * is exercised through the pure sapling_ckpt_verify_binding() decision. */

#include "test/test_helpers.h"

static void ckpt_fill_hash(struct uint256 *h, uint8_t seed, size_t idx)
{
    for (size_t i = 0; i < 32; i++)
        h->data[i] = (uint8_t)(seed ^ (idx + i));
}

static void ckpt_build_tree(size_t n, struct incremental_merkle_tree *t_out)
{
    sapling_tree_init(t_out);
    for (size_t i = 0; i < n; i++) {
        struct uint256 cm;
        ckpt_fill_hash(&cm, 0x3C, i);
        incremental_tree_append(t_out, &cm);
    }
}

int test_sapling_ckpt_persist(void)
{
    int failures = 0;
    char path[128];

    /* (a) round-trip: save then load restores frontier + root + block hash. */
    printf("sapling_ckpt_persist round-trip (frontier + block-hash key) ... ");
    {
        strcpy(path, "/tmp/zcl_sapling_ckpt_rt_XXXXXX");
        int fd = mkstemp(path);
        if (fd < 0) { printf("FAIL (mkstemp)\n"); failures++; goto done_rt; }
        close(fd);
        unlink(path);

        struct incremental_merkle_tree src;
        ckpt_build_tree(300, &src);

        uint8_t bhash[32];
        for (int i = 0; i < 32; i++) bhash[i] = (uint8_t)(0x11 + i);

        if (!sapling_tree_flush_checkpoint(&src, 700000, bhash, path)) {
            printf("FAIL (flush)\n"); failures++; unlink(path); goto done_rt;
        }

        struct incremental_merkle_tree dst;
        sapling_tree_init(&dst);
        int64_t got_h = -1;
        uint8_t got_hash[32] = {0};
        if (!sapling_tree_load_checkpoint(&dst, &got_h, got_hash, path)) {
            printf("FAIL (load)\n"); failures++; unlink(path); goto done_rt;
        }
        struct uint256 rs, rd;
        incremental_tree_root(&src, &rs);
        incremental_tree_root(&dst, &rd);
        if (got_h != 700000) {
            printf("FAIL (height %lld)\n", (long long)got_h); failures++;
        } else if (memcmp(got_hash, bhash, 32) != 0) {
            printf("FAIL (block hash not restored)\n"); failures++;
        } else if (memcmp(rs.data, rd.data, 32) != 0) {
            printf("FAIL (root mismatch)\n"); failures++;
        } else if (incremental_tree_size(&dst) != incremental_tree_size(&src)) {
            printf("FAIL (size mismatch)\n"); failures++;
        } else {
            printf("OK\n");
        }
        unlink(path);
done_rt:;
    }

    /* (b) corrupted cache is rejected on load (SHA3 tamper detection). */
    printf("sapling_ckpt_persist corrupted cache rejected ... ");
    {
        strcpy(path, "/tmp/zcl_sapling_ckpt_corrupt_XXXXXX");
        int fd = mkstemp(path);
        if (fd < 0) { printf("FAIL (mkstemp)\n"); failures++; goto done_corrupt; }
        close(fd);
        unlink(path);

        struct incremental_merkle_tree src;
        ckpt_build_tree(90, &src);
        uint8_t bhash[32];
        for (int i = 0; i < 32; i++) bhash[i] = (uint8_t)(0x55 ^ i);
        if (!sapling_tree_flush_checkpoint(&src, 600000, bhash, path)) {
            printf("FAIL (flush)\n"); failures++; unlink(path); goto done_corrupt;
        }

        /* Flip a byte in the serialized tree blob region (past the header). */
        FILE *f = fopen(path, "r+b");
        if (!f) { printf("FAIL (reopen)\n"); failures++; unlink(path); goto done_corrupt; }
        fseek(f, 90, SEEK_SET);
        uint8_t b;
        if (fread(&b, 1, 1, f) != 1) {
            printf("FAIL (fread)\n"); failures++; fclose(f); unlink(path); goto done_corrupt;
        }
        b ^= 0x40;
        fseek(f, 90, SEEK_SET);
        fwrite(&b, 1, 1, f);
        fclose(f);

        struct incremental_merkle_tree dst;
        sapling_tree_init(&dst);
        int64_t got_h = -1;
        uint8_t got_hash[32] = {0};
        bool ok = sapling_tree_load_checkpoint(&dst, &got_h, got_hash, path);
        if (ok) { printf("FAIL (tampered file accepted)\n"); failures++; }
        else    { printf("OK\n"); }
        unlink(path);
done_corrupt:;
    }

    /* (c)+(d)+(e) fail-closed binding decision (pure, no chain needed). */
    printf("sapling_ckpt_persist verify_binding fail-closed verdicts ... ");
    {
        struct uint256 root, other, bhash_u, exp_hash_u;
        ckpt_fill_hash(&root, 0xAA, 1);
        ckpt_fill_hash(&other, 0xBB, 2);
        ckpt_fill_hash(&bhash_u, 0xC1, 3);
        exp_hash_u = bhash_u; /* same block at H by default */

        int local = 0;

        /* OK: height <= tip, block hash matches, root matches. */
        if (sapling_ckpt_verify_binding(1000, &root, bhash_u.data,
                2000, exp_hash_u.data, true, &root, true) != SAPLING_CKPT_OK)
            local++;

        /* (c) stale above tip: H=3000 > tip=2000. */
        if (sapling_ckpt_verify_binding(3000, &root, bhash_u.data,
                2000, exp_hash_u.data, true, &root, true)
            != SAPLING_CKPT_STALE_ABOVE_TIP)
            local++;

        /* (d) reorg: block hash at H differs from expected. */
        if (sapling_ckpt_verify_binding(1000, &root, bhash_u.data,
                2000, other.data, true, &root, true) != SAPLING_CKPT_REORG)
            local++;

        /* reorg: expected hash unknown (absent body) but ckpt carries one. */
        if (sapling_ckpt_verify_binding(1000, &root, bhash_u.data,
                2000, NULL, false, &root, true) != SAPLING_CKPT_REORG)
            local++;

        /* root mismatch: header binding present but differs. */
        if (sapling_ckpt_verify_binding(1000, &root, bhash_u.data,
                2000, exp_hash_u.data, true, &other, true)
            != SAPLING_CKPT_ROOT_MISMATCH)
            local++;

        /* root unknown: header hashFinalSaplingRoot absent -> cannot verify. */
        if (sapling_ckpt_verify_binding(1000, &root, bhash_u.data,
                2000, exp_hash_u.data, true, NULL, false)
            != SAPLING_CKPT_ROOT_UNKNOWN)
            local++;

        /* zero block hash in ckpt skips the reorg gate, relies on root. */
        uint8_t zeros[32] = {0};
        if (sapling_ckpt_verify_binding(1000, &root, zeros,
                2000, other.data, true, &root, true) != SAPLING_CKPT_OK)
            local++;

        if (local) { printf("FAIL (%d verdicts wrong)\n", local); failures += local; }
        else       { printf("OK\n"); }
    }

    /* (e) absent cache: load of a missing file returns false (full replay). */
    printf("sapling_ckpt_persist absent cache = full-replay path ... ");
    {
        struct incremental_merkle_tree dst;
        sapling_tree_init(&dst);
        int64_t got_h = -1;
        uint8_t got_hash[32] = {0};
        bool ok = sapling_tree_load_checkpoint(&dst, &got_h, got_hash,
            "/tmp/zcl_sapling_ckpt_absent_does_not_exist_998877");
        if (ok) { printf("FAIL (loaded from missing file)\n"); failures++; }
        else    { printf("OK\n"); }
    }

    /* delta-replay equivalence: a resumed frontier + forward replay yields the
     * same root as a full replay (the fast-resume correctness guarantee). */
    printf("sapling_ckpt_persist delta-replay == full-replay root ... ");
    {
        strcpy(path, "/tmp/zcl_sapling_ckpt_delta_XXXXXX");
        int fd = mkstemp(path);
        if (fd < 0) { printf("FAIL (mkstemp)\n"); failures++; goto done_delta; }
        close(fd);
        unlink(path);

        struct incremental_merkle_tree at150;
        sapling_tree_init(&at150);
        for (size_t i = 0; i < 150; i++) {
            struct uint256 cm; ckpt_fill_hash(&cm, 0x7E, i);
            incremental_tree_append(&at150, &cm);
        }
        uint8_t bhash[32]; for (int i = 0; i < 32; i++) bhash[i] = (uint8_t)i;
        if (!sapling_tree_flush_checkpoint(&at150, 150, bhash, path)) {
            printf("FAIL (flush)\n"); failures++; unlink(path); goto done_delta;
        }

        struct incremental_merkle_tree full;
        sapling_tree_init(&full);
        for (size_t i = 0; i < 400; i++) {
            struct uint256 cm; ckpt_fill_hash(&cm, 0x7E, i);
            incremental_tree_append(&full, &cm);
        }

        struct incremental_merkle_tree delta;
        sapling_tree_init(&delta);
        int64_t ch = 0;
        uint8_t chash[32] = {0};
        if (!sapling_tree_load_checkpoint(&delta, &ch, chash, path)) {
            printf("FAIL (load)\n"); failures++; unlink(path); goto done_delta;
        }
        for (size_t i = 150; i < 400; i++) {
            struct uint256 cm; ckpt_fill_hash(&cm, 0x7E, i);
            incremental_tree_append(&delta, &cm);
        }
        struct uint256 rf, rd;
        incremental_tree_root(&full, &rf);
        incremental_tree_root(&delta, &rd);
        if (memcmp(rf.data, rd.data, 32) != 0) {
            printf("FAIL (delta root diverges)\n"); failures++;
        } else if (incremental_tree_size(&delta) != 400) {
            printf("FAIL (delta size)\n"); failures++;
        } else {
            printf("OK\n");
        }
        unlink(path);
done_delta:;
    }

    return failures;
}

/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Merkle Mountain Range implementation.
 * See mmr.h for design notes and domain separation scheme. */

#include "chain/mmr.h"
#include "crypto/sha3.h"
#include "util/log_macros.h"
#include <string.h>
#include <stdio.h>

/* ── Hashing primitives ────────────────────────────────── */

void mmr_hash_leaf(const uint8_t block_hash[32], uint8_t out[32])
{
    uint8_t buf[33];
    buf[0] = MMR_TAG_LEAF;
    memcpy(buf + 1, block_hash, 32);
    sha3_256(buf, 33, out);
}

void mmr_hash_internal(const uint8_t left[32], const uint8_t right[32],
                       uint8_t out[32])
{
    uint8_t buf[65];
    buf[0] = MMR_TAG_INTERNAL;
    memcpy(buf + 1, left, 32);
    memcpy(buf + 33, right, 32);
    sha3_256(buf, 65, out);
}

/* ── Core operations ───────────────────────────────────── */

void mmr_init(struct mmr *m)
{
    memset(m, 0, sizeof(*m));
}

/* Shared append: takes an already-hashed leaf and runs the peak-merge loop.
 * Copies the incoming hash into a local working buffer because the merge
 * loop mutates it in place. */
static int mmr_append_prehashed(struct mmr *m, const uint8_t h_in[32])
{
    uint8_t h[32];
    memcpy(h, h_in, 32);

    int merges = 0;

    /* Merge with existing peaks while the new leaf completes a pair.
     * Binary trick: count trailing 1-bits in (num_leaves + 1). */
    uint64_t n = m->num_leaves + 1;
    while (n % 2 == 0 && m->num_peaks > 0) {
        uint8_t parent[32];
        mmr_hash_internal(m->peaks[m->num_peaks - 1], h, parent);
        memcpy(h, parent, 32);
        m->num_peaks--;
        n /= 2;
        merges++;
    }

    /* Push the (possibly merged) peak */
    memcpy(m->peaks[m->num_peaks], h, 32);
    m->num_peaks++;
    m->num_leaves++;

    return merges;
}

int mmr_append(struct mmr *m, const uint8_t block_hash[32])
{
    uint8_t h[32];
    mmr_hash_leaf(block_hash, h);
    return mmr_append_prehashed(m, h);
}

/* Bag all peaks: SHA3-256(0x02 || peak_0 || ... || peak_k) */
static void mmr_bag_peaks(const uint8_t (*peaks)[32], uint32_t n, uint8_t *out)
{
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    uint8_t tag = MMR_TAG_ROOT;
    sha3_256_write(&ctx, &tag, 1);
    for (uint32_t i = 0; i < n; i++)
        sha3_256_write(&ctx, peaks[i], 32);
    sha3_256_finalize(&ctx, out);
}

void mmr_root(const struct mmr *m, uint8_t out[32])
{
    if (m->num_peaks == 0) {
        memset(out, 0, 32);
        return;
    }
    if (m->num_peaks == 1) {
        memcpy(out, m->peaks[0], 32);
        return;
    }

    mmr_bag_peaks(m->peaks, m->num_peaks, out);
}

/* ── Serialization ─────────────────────────────────────── */

size_t mmr_serialize(const struct mmr *m, uint8_t *buf, size_t buflen)
{
    size_t need = 8 + 4 + (size_t)m->num_peaks * 32;
    if (buflen < need) return 0;

    /* Little-endian num_leaves (8 bytes) */
    for (int i = 0; i < 8; i++)
        buf[i] = (uint8_t)(m->num_leaves >> (i * 8));

    /* Little-endian num_peaks (4 bytes) */
    for (int i = 0; i < 4; i++)
        buf[8 + i] = (uint8_t)(m->num_peaks >> (i * 8));

    /* Peak hashes */
    memcpy(buf + 12, m->peaks, (size_t)m->num_peaks * 32);

    return need;
}

bool mmr_deserialize(struct mmr *m, const uint8_t *buf, size_t len)
{
    if (len < 12)
        LOG_FAIL("mmr", "deserialize: buffer too short (%zu < 12)", len);

    mmr_init(m);

    m->num_leaves = 0;
    for (int i = 7; i >= 0; i--)
        m->num_leaves = (m->num_leaves << 8) | buf[i];

    uint32_t np = 0;
    for (int i = 3; i >= 0; i--)
        np = (np << 8) | buf[8 + i];

    if (np > MMR_MAX_PEAKS)
        LOG_FAIL("mmr", "deserialize: peak count %u exceeds max %d", np, MMR_MAX_PEAKS);
    if (len < 12 + (size_t)np * 32)
        LOG_FAIL("mmr", "deserialize: buffer too short for %u peaks", np);

    m->num_peaks = np;
    memcpy(m->peaks, buf + 12, (size_t)np * 32);

    return true;
}

/* ── Proof generation from leaf array ──────────────────── */

/* Build a full MMR from leaves and extract the authentication path
 * for a specific leaf. This is O(n) but only needed for proof generation. */
bool mmr_prove_from_leaves(const uint8_t (*all_leaves)[32],
                           uint64_t num_leaves,
                           uint64_t leaf_index,
                           struct mmr_proof *proof)
{
    if (!all_leaves || !proof || leaf_index >= num_leaves)
        LOG_FAIL("mmr", "prove_from_leaves: invalid args (leaves=%p, proof=%p, index=%lu/%lu)",
                 (const void *)all_leaves, (const void *)proof,
                 (unsigned long)leaf_index, (unsigned long)num_leaves);

    memset(proof, 0, sizeof(*proof));
    proof->leaf_index = leaf_index;
    proof->mmr_size = num_leaves;

    /* Hash the target leaf */
    mmr_hash_leaf(all_leaves[leaf_index], proof->leaf_hash);

    /* Rebuild the MMR and track the path for our target leaf.
     *
     * We use a stack-based approach: maintain a stack of (hash, height).
     * When we push a node and the top two have the same height, merge.
     * Track which merges involve our target leaf's lineage. */

    /* Stack: max depth is log2(num_leaves) + 1 */
    uint8_t stack[MMR_MAX_PEAKS][32];
    int stack_height[MMR_MAX_PEAKS];
    int stack_top = 0;
    /* Track which stack entry is "ours" (contains the target) */
    int target_idx = -1;

    uint32_t sib_count = 0;

    for (uint64_t i = 0; i < num_leaves; i++) {
        uint8_t h[32];
        mmr_hash_leaf(all_leaves[i], h);

        memcpy(stack[stack_top], h, 32);
        stack_height[stack_top] = 0;
        bool is_target = (i == leaf_index);
        if (is_target) target_idx = stack_top;
        stack_top++;

        /* Merge while top two have same height */
        while (stack_top >= 2 &&
               stack_height[stack_top - 1] == stack_height[stack_top - 2]) {
            int left = stack_top - 2;
            int right = stack_top - 1;

            /* If our target is one of these, the other is a sibling */
            if (target_idx == left) {
                memcpy(proof->siblings[sib_count++], stack[right], 32);
                target_idx = left; /* merged node takes left's position */
            } else if (target_idx == right) {
                memcpy(proof->siblings[sib_count++], stack[left], 32);
                target_idx = left;
            }

            uint8_t parent[32];
            mmr_hash_internal(stack[left], stack[right], parent);
            memcpy(stack[left], parent, 32);
            stack_height[left]++;
            stack_top--;
        }
    }

    proof->num_siblings = sib_count;

    /* The remaining stack entries are the peaks */
    proof->num_peaks = (uint32_t)stack_top;
    for (int i = 0; i < stack_top; i++)
        memcpy(proof->peak_hashes[i], stack[i], 32);

    return true;
}

/* ── Unified commitment leaf ──────────────────────────── */

void mmr_hash_commitment(const struct mmr_commitment *c, uint8_t out[32])
{
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    uint8_t tag = MMR_TAG_LEAF;
    sha3_256_write(&ctx, &tag, 1);
    sha3_256_write(&ctx, (const uint8_t *)&c->height, 4);
    sha3_256_write(&ctx, c->block_hash, 32);
    sha3_256_write(&ctx, c->utxo_root, 32);
    sha3_256_write(&ctx, c->data_root, 32);
    sha3_256_finalize(&ctx, out);
}

int mmr_append_commitment(struct mmr *m, const struct mmr_commitment *c)
{
    uint8_t leaf[32];
    mmr_hash_commitment(c, leaf);
    return mmr_append_prehashed(m, leaf);
}

/* ── Proof verification ────────────────────────────────── */

bool mmr_verify(const struct mmr_proof *proof,
                const uint8_t expected_root[32])
{
    if (!proof)
        LOG_FAIL("mmr", "verify: proof is NULL");

    /* Reconstruct the peak hash from the leaf + siblings */
    uint8_t current[32];
    memcpy(current, proof->leaf_hash, 32);

    /* Walk up the tree using siblings.
     * Determine left/right position from the leaf_index bits. */
    uint64_t idx = proof->leaf_index;
    for (uint32_t i = 0; i < proof->num_siblings; i++) {
        uint8_t parent[32];
        if (idx % 2 == 0) {
            /* We're the left child */
            mmr_hash_internal(current, proof->siblings[i], parent);
        } else {
            /* We're the right child */
            mmr_hash_internal(proof->siblings[i], current, parent);
        }
        memcpy(current, parent, 32);
        idx /= 2;
    }

    /* current should now equal one of the peaks */
    bool found_peak = false;
    for (uint32_t i = 0; i < proof->num_peaks; i++) {
        if (memcmp(current, proof->peak_hashes[i], 32) == 0) {
            found_peak = true;
            break;
        }
    }
    if (!found_peak)
        LOG_FAIL("mmr", "verify: reconstructed hash does not match any peak");

    /* Bag the peaks and verify root */
    uint8_t root[32];
    if (proof->num_peaks == 1) {
        memcpy(root, proof->peak_hashes[0], 32);
    } else {
        mmr_bag_peaks(proof->peak_hashes, proof->num_peaks, root);
    }

    return memcmp(root, expected_root, 32) == 0;
}

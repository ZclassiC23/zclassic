/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_checkpoint_rom_authority — focused unit test for the CHECKPOINT_ROM
 * ACTIVATE authority (config/src/consensus_state_snapshot_install_checkpoint_
 * authority.c).
 *
 * CHECKPOINT_ROM lifts ACTIVATE containment SOVEREIGNLY when an install
 * candidate's manifest reproduces EVERY component of the compiled shielded ROM
 * state checkpoint (g_rom_state_checkpoint) byte-for-byte — header-independent,
 * fail-closed. It closes the sovereignty hole where the historical Sprout
 * anchors and the full ~1.49M nullifier set were unbound under the
 * header-dependent CHECKPOINT_CONTENT authority (which binds only the
 * transparent coins + the Sapling tip frontier root).
 *
 * Pure synthetic fixture: it drives the content-authority resolver
 * (consensus_state_activate_resolve_content_authority_name_for_test — the
 * CHECKPOINT_ROM -> CHECKPOINT_CONTENT half of the resolver, bypassing the
 * evidence/receipt gate) with a hand-built manifest against a
 * checkpoints_set_rom_state_override_for_test() keystone. No bundle file,
 * datadir, replay receipt, or 3M-block boot. Asserts:
 *   (a) full component match at the override height   -> "checkpoint_rom"
 *   (b) any single flipped component                  -> not activate ("none")
 *   (c) height mismatch                               -> falls through to the
 *       CHECKPOINT_CONTENT path UNCHANGED (no SHA3 match -> "none"; a matching
 *       SHA3 checkpoint + validated-header Sapling root -> "checkpoint_content")
 *   (d) unbaked (placeholder) keystone                -> not activate ("none")
 */

#include "test/test_helpers.h"

#include "chain/checkpoints.h"
#include "config/consensus_state_snapshot_install.h"
#include "storage/consensus_state_bundle_codec.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#define CROM_H 4242 /* synthetic ROM checkpoint height (never the real 3056758) */

/* A fully-baked synthetic keystone: every component nonzero so it is not a
 * placeholder. Values mirror the real keystone's magnitudes for readability
 * (they are not asserted against production — this is a self-contained fixture). */
static void synth_rom(struct rom_state_checkpoint *rom, int32_t height)
{
    memset(rom, 0, sizeof(*rom));
    rom->height = height;
    memset(rom->block_hash, 0xb1, 32);
    memset(rom->utxo_root, 0x11, 32);
    rom->utxo_count = 1354769;
    rom->total_supply = 1036413794674881LL;
    memset(rom->anchor_digest, 0x22, 32);
    rom->anchor_count = 631645;
    memset(rom->sprout_frontier_root, 0x33, 32);
    rom->sprout_frontier_height = 2124937;
    memset(rom->sapling_frontier_root, 0x44, 32);
    rom->sapling_frontier_height = height - 16;
    memset(rom->nullifier_digest, 0x55, 32);
    rom->nullifier_count = 1495126;
    memset(rom->rom_state_root, 0x66, 32); /* nonzero -> not a placeholder */
}

/* A manifest whose every ROM-bound component equals the keystone. */
static void manifest_from_rom(struct consensus_state_bundle_manifest *m,
                              const struct rom_state_checkpoint *rom)
{
    memset(m, 0, sizeof(*m));
    m->height = rom->height;
    memcpy(m->block_hash, rom->block_hash, 32);
    m->history_complete = true;
    memcpy(m->utxo_root, rom->utxo_root, 32);
    m->utxo_count = rom->utxo_count;
    m->total_supply = rom->total_supply;
    memcpy(m->anchor_digest, rom->anchor_digest, 32);
    m->anchor_count = rom->anchor_count;
    memcpy(m->sprout_frontier_root, rom->sprout_frontier_root, 32);
    m->sprout_frontier_height = rom->sprout_frontier_height;
    memcpy(m->sapling_frontier_root, rom->sapling_frontier_root, 32);
    m->sapling_frontier_height = rom->sapling_frontier_height;
    memcpy(m->nullifier_digest, rom->nullifier_digest, 32);
    m->nullifier_count = rom->nullifier_count;
}

int test_checkpoint_rom_authority(void)
{
    printf("\n=== checkpoint_rom_authority ===\n");
    int failures = 0;

#define CHECK(desc, cond)                                                    \
    do {                                                                     \
        printf("checkpoint_rom_authority: %s... ", (desc));                  \
        if (cond) printf("OK\n");                                            \
        else { printf("FAIL\n"); failures++; }                              \
    } while (0)

    /* Hermetic: clear any override a sibling group may have left installed. */
    checkpoints_reset_rom_state_override_for_test();
    checkpoints_reset_sha3_override_for_test();

    struct rom_state_checkpoint rom;
    synth_rom(&rom, CROM_H);

    /* Zeroed request: no validated-header Sapling root. CHECKPOINT_ROM must not
     * need one; CHECKPOINT_CONTENT (the fallback) would CONTAIN without it. */
    struct consensus_state_activate_request req;
    memset(&req, 0, sizeof(req));

    /* (a) POSITIVE — full component match at the override height activates via
     *     CHECKPOINT_ROM with NO validated header in the request. */
    checkpoints_set_rom_state_override_for_test(&rom);
    {
        struct consensus_state_bundle_manifest m;
        manifest_from_rom(&m, &rom);
        const char *name =
            consensus_state_activate_resolve_content_authority_name_for_test(
                &m, &req);
        CHECK("full component match -> checkpoint_rom (no header needed)",
              name && strcmp(name, "checkpoint_rom") == 0);
    }

    /* (b) NEGATIVE — every single flipped component drops the ROM authority
     *     (fail-closed). With no SHA3 checkpoint at this synthetic height the
     *     content fallback also declines, so the verdict is "none". */
#define FLIP_CHECK(desc, mutate)                                              \
    do {                                                                      \
        struct consensus_state_bundle_manifest m;                             \
        manifest_from_rom(&m, &rom);                                          \
        mutate;                                                               \
        const char *nm =                                                     \
            consensus_state_activate_resolve_content_authority_name_for_test( \
                &m, &req);                                                    \
        CHECK("flipped " desc " -> not activate (none)",                      \
              nm && strcmp(nm, "none") == 0);                                 \
    } while (0)

    FLIP_CHECK("utxo_root", m.utxo_root[0] ^= 0xff);
    FLIP_CHECK("utxo_count", m.utxo_count += 1);
    FLIP_CHECK("total_supply", m.total_supply += 1);
    FLIP_CHECK("anchor_digest", m.anchor_digest[0] ^= 0xff);
    FLIP_CHECK("anchor_count", m.anchor_count += 1);
    FLIP_CHECK("sprout_frontier_root", m.sprout_frontier_root[0] ^= 0xff);
    FLIP_CHECK("sprout_frontier_height", m.sprout_frontier_height += 1);
    FLIP_CHECK("sapling_frontier_root", m.sapling_frontier_root[0] ^= 0xff);
    FLIP_CHECK("sapling_frontier_height", m.sapling_frontier_height += 1);
    FLIP_CHECK("nullifier_digest", m.nullifier_digest[0] ^= 0xff);
    FLIP_CHECK("nullifier_count", m.nullifier_count += 1);

#undef FLIP_CHECK

    /* (c1) HEIGHT MISMATCH — the ROM binding is inert off the checkpoint
     *      height, and with no SHA3 checkpoint here the content fallback
     *      declines too: "none". Proves CHECKPOINT_ROM never false-fires. */
    {
        struct consensus_state_bundle_manifest m;
        manifest_from_rom(&m, &rom);
        m.height = CROM_H + 1000; /* off the ROM checkpoint height */
        const char *nm =
            consensus_state_activate_resolve_content_authority_name_for_test(
                &m, &req);
        CHECK("height mismatch -> ROM inert, falls through to content (none)",
              nm && strcmp(nm, "none") == 0);
    }

    /* (c2) HEIGHT MISMATCH + a matching SHA3 checkpoint + a validated-header
     *      Sapling root -> the CHECKPOINT_CONTENT fallback still activates,
     *      UNCHANGED by the new ROM authority (which is inert off its height). */
    {
        struct consensus_state_bundle_manifest m;
        manifest_from_rom(&m, &rom);
        int32_t cc_h = CROM_H + 1000;
        m.height = cc_h;

        struct sha3_utxo_checkpoint sha3;
        memset(&sha3, 0, sizeof(sha3));
        sha3.height = cc_h;
        memcpy(sha3.block_hash, m.block_hash, 32);
        sha3.utxo_count = m.utxo_count;
        sha3.total_supply = m.total_supply;
        memcpy(sha3.sha3_hash, m.utxo_root, 32);
        checkpoints_set_sha3_override_for_test(&sha3);

        struct consensus_state_activate_request creq = req;
        creq.checkpoint_sapling_root_from_validated_header = true;
        memcpy(creq.checkpoint_sapling_root, m.sapling_frontier_root, 32);

        const char *nm =
            consensus_state_activate_resolve_content_authority_name_for_test(
                &m, &creq);
        CHECK("height mismatch + SHA3 match + header root -> checkpoint_content "
              "(fallback intact)",
              nm && strcmp(nm, "checkpoint_content") == 0);
        checkpoints_reset_sha3_override_for_test();
    }

    /* (d) FAIL-CLOSED — an UNBAKED (placeholder) keystone at the checkpoint
     *     height, matched exactly by the manifest, must NOT activate: an
     *     all-zero shielded fold is not a trust root. Without the placeholder
     *     guard the all-zero == all-zero components would falsely "match" and
     *     grant CHECKPOINT_ROM. Falls through to content -> "none". */
    {
        struct rom_state_checkpoint ph = rom;
        memset(ph.nullifier_digest, 0, 32); /* unbaked shielded fold */
        checkpoints_set_rom_state_override_for_test(&ph);
        struct consensus_state_bundle_manifest m;
        manifest_from_rom(&m, &ph); /* manifest matches the placeholder exactly */
        const char *nm =
            consensus_state_activate_resolve_content_authority_name_for_test(
                &m, &req);
        CHECK("placeholder (unbaked) keystone -> not activate (none)",
              nm && strcmp(nm, "none") == 0);
    }

    checkpoints_reset_rom_state_override_for_test();
    checkpoints_reset_sha3_override_for_test();

#undef CHECK

    printf("=== checkpoint_rom_authority: %d failure(s) ===\n", failures);
    return failures;
}

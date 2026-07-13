/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * See config/boot_snapshot_install.h. */

#include "config/boot.h"
#include "config/boot_snapshot_install.h"

#include "chain/checkpoints.h"
#include "chain/utxo_snapshot_loader.h"
#include "event/event.h"
#include "storage/coins_kv.h"
#include "storage/progress_store.h"
#include "util/log_macros.h"

#include <stddef.h>
#include <limits.h>
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define SNAPSHOT_INSTALL_MARKER_VERSION 1u

static void put_u32_le(uint8_t *out, uint32_t value)
{
    out[0] = (uint8_t)value; out[1] = (uint8_t)(value >> 8);
    out[2] = (uint8_t)(value >> 16); out[3] = (uint8_t)(value >> 24);
}

static uint32_t get_u32_le(const uint8_t *in)
{
    return (uint32_t)in[0] | ((uint32_t)in[1] << 8) |
           ((uint32_t)in[2] << 16) | ((uint32_t)in[3] << 24);
}

static void put_u64_le(uint8_t *out, uint64_t value)
{
    for (size_t i = 0; i < 8; i++) out[i] = (uint8_t)(value >> (8u * i));
}

static uint64_t get_u64_le(const uint8_t *in)
{
    uint64_t value = 0;
    for (size_t i = 0; i < 8; i++) value |= (uint64_t)in[i] << (8u * i);
    return value;
}

static void marker_encode(uint8_t out[BOOT_SNAPSHOT_INSTALL_MARKER_BYTES],
                          int32_t height, uint64_t count,
                          const uint8_t payload_sha3[32])
{
    memset(out, 0, BOOT_SNAPSHOT_INSTALL_MARKER_BYTES);
    memcpy(out, "ZCLSIM1", 7);
    put_u32_le(out + 8, SNAPSHOT_INSTALL_MARKER_VERSION);
    put_u32_le(out + 12, BOOT_SNAPSHOT_INSTALL_MARKER_BYTES);
    put_u32_le(out + 16, (uint32_t)height);
    put_u64_le(out + 24, count);
    memcpy(out + 32, payload_sha3, 32);
}

static bool marker_matches(
    const uint8_t marker[BOOT_SNAPSHOT_INSTALL_MARKER_BYTES], size_t len,
    int32_t height, uint64_t count, const uint8_t payload_sha3[32])
{
    if (!marker || len != BOOT_SNAPSHOT_INSTALL_MARKER_BYTES || height < 0 ||
        !payload_sha3 || memcmp(marker, "ZCLSIM1", 7) != 0 || marker[7] != 0 ||
        get_u32_le(marker + 8) != SNAPSHOT_INSTALL_MARKER_VERSION ||
        get_u32_le(marker + 12) != BOOT_SNAPSHOT_INSTALL_MARKER_BYTES ||
        (int32_t)get_u32_le(marker + 16) != height ||
        get_u64_le(marker + 24) != count ||
        memcmp(marker + 32, payload_sha3, 32) != 0)
        return false;
    return marker[20] == 0 && marker[21] == 0 &&
           marker[22] == 0 && marker[23] == 0;
}

bool boot_snapshot_install_marker_begin(struct sqlite3 *db,
                                        int32_t height, uint64_t count,
                                        const uint8_t payload_sha3[32])
{
    if (!db || height < 0 || !payload_sha3)
        LOG_FAIL("boot", "snapshot install marker: invalid begin arguments");
    uint8_t marker[BOOT_SNAPSHOT_INSTALL_MARKER_BYTES];
    marker_encode(marker, height, count, payload_sha3);
    if (!progress_meta_set(db, BOOT_SNAPSHOT_INSTALL_MARKER_KEY,
                           marker, sizeof(marker)))
        LOG_FAIL("boot", "snapshot install marker: durable begin failed");
    return true;
}

bool boot_snapshot_install_marker_blocks_resume(
    struct sqlite3 *db, int32_t height, uint64_t count,
    const uint8_t payload_sha3[32], bool *matches)
{
    if (matches) *matches = false;
    if (!db || height < 0 || !payload_sha3) return true;
    uint8_t marker[BOOT_SNAPSHOT_INSTALL_MARKER_BYTES] = {0};
    size_t len = 0;
    bool found = false;
    if (!progress_meta_get(db, BOOT_SNAPSHOT_INSTALL_MARKER_KEY,
                           marker, sizeof(marker), &len, &found))
        return true;
    if (!found) return false;
    if (matches) *matches = marker_matches(marker, len, height, count,
                                            payload_sha3);
    return true;
}

bool boot_snapshot_install_marker_clear_in_tx(struct sqlite3 *db)
{
    return db && progress_meta_delete_in_tx(db,
                                             BOOT_SNAPSHOT_INSTALL_MARKER_KEY);
}

bool boot_snapshot_install_marker_clear(struct sqlite3 *db)
{
    if (!db)
        LOG_FAIL("boot", "snapshot install marker: NULL clear store");

    char *err = NULL;
    bool ok = true;
    progress_store_tx_lock();
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, &err) != SQLITE_OK)
        ok = false;
    if (ok && !boot_snapshot_install_marker_clear_in_tx(db))
        ok = false;
    if (ok && sqlite3_exec(db, "COMMIT", NULL, NULL, &err) != SQLITE_OK)
        ok = false;
    if (!ok)
        (void)sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    if (err)
        sqlite3_free(err);
    progress_store_tx_unlock();
    if (!ok)
        LOG_WARN("boot", "snapshot install marker: durable clear failed");
    return ok;
}

bool boot_snapshot_install_marker_pending(struct sqlite3 *db, bool *pending)
{
    if (pending) *pending = true;
    if (!db || !pending)
        LOG_FAIL("boot", "snapshot install marker: invalid pending arguments");
    uint8_t marker[BOOT_SNAPSHOT_INSTALL_MARKER_BYTES] = {0};
    size_t len = 0;
    bool found = false;
    if (!progress_meta_get(db, BOOT_SNAPSHOT_INSTALL_MARKER_KEY,
                           marker, sizeof(marker), &len, &found))
        LOG_FAIL("boot", "snapshot install marker: pending read failed");
    *pending = found;
    return true;
}

bool boot_snapshot_install_pending_artifact_matches(
    struct sqlite3 *db, const char *path, bool *pending_out)
{
    if (pending_out)
        *pending_out = true;
    bool pending = true;
    if (!db || !pending_out ||
        !boot_snapshot_install_marker_pending(db, &pending))
        return false;
    *pending_out = pending;
    if (!pending)
        return true;
    if (!path || !path[0])
        return false;

    char err[256] = {0};
    struct uss_header hdr;
    struct uss_handle *snapshot = uss_open(
        path, /*verify_full_sha3=*/true, /*expected_sha3=*/NULL,
        &hdr, err, sizeof(err));
    if (!snapshot)
        return false;
    bool matches = false;
    bool blocked = hdr.height <= (uint64_t)INT32_MAX &&
        boot_snapshot_install_marker_blocks_resume(
            db, (int32_t)hdr.height, hdr.count, hdr.sha3_hash, &matches);
    uss_close(snapshot);
    return blocked && matches;
}

bool boot_snapshot_install_resume_allowed(struct sqlite3 *db,
    const struct uss_header *snapshot, int32_t *applied,
    bool *install_pending, bool *marker_matches)
{
    if (applied) *applied = -1;
    if (install_pending) *install_pending = true;
    if (marker_matches) *marker_matches = false;
    if (!db || !snapshot || !applied) return false;
    bool matches = false;
    bool pending = boot_snapshot_install_marker_blocks_resume(
        db, (int32_t)snapshot->height, snapshot->count, snapshot->sha3_hash,
        &matches);
    if (install_pending) *install_pending = pending;
    if (marker_matches) *marker_matches = matches;
    return !pending && coins_kv_is_proven_authority(db, applied) &&
           *applied >= (int32_t)snapshot->height + 1;
}

bool boot_snapshot_anchor_hash_matches(const unsigned char *index_hash,
                                       const unsigned char *snapshot_hash)
{
    return index_hash && snapshot_hash &&
           memcmp(index_hash, snapshot_hash, 32) == 0;
}

bool boot_snapshot_install_headers_equal(const struct uss_header *a,
                                         const struct uss_header *b)
{
    return a && b && a->version == b->version && a->height == b->height &&
           a->count == b->count && a->total_supply == b->total_supply &&
           memcmp(a->anchor_block_hash, b->anchor_block_hash, 32) == 0 &&
           memcmp(a->sha3_hash, b->sha3_hash, 32) == 0;
}

bool boot_legacy_uss_matches_checkpoint(
    struct uss_handle *snapshot, const struct uss_header *header,
    const struct sha3_utxo_checkpoint *checkpoint,
    char *err, size_t err_size)
{
    if (!snapshot || !header || !checkpoint)
        return false;
    struct uss_utxo_component component;
    if (!uss_utxo_component_compute(snapshot, &component, err, err_size))
        return false;
    return header->height == (uint32_t)checkpoint->height &&
           memcmp(header->anchor_block_hash, checkpoint->block_hash, 32) == 0 &&
           component.count == checkpoint->utxo_count &&
           component.total_supply == checkpoint->total_supply &&
           memcmp(component.sha3_hash, checkpoint->sha3_hash, 32) == 0;
}

void boot_snapshot_install_require_chain_context(
    const struct main_state *main_state)
{
#ifdef ZCL_TESTING
    (void)main_state;
#else
    if (main_state) return;
    fprintf(stderr,
            "FATAL: snapshot install requires main_state to bind its anchor "
            "to the validated local header chain; REFUSING unbound state\n");
    event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
                "load_snapshot_at_own_height missing_main_state");
    _exit(EXIT_FAILURE);
#endif
}

void boot_snapshot_install_gate_boot(bool progress_open,
                                     const char *loader_path)
{
    if (!progress_open) {
        fprintf(stderr,
                "FATAL: progress_store unavailable; reducer cursors and "
                "snapshot journal cannot be verified; REFUSING to boot\n");
        event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
                    "progress_store_unavailable");
        _exit(EXIT_FAILURE);
    }
    bool pending = true;
    if (!boot_snapshot_install_marker_pending(progress_store_db(), &pending)) {
        fprintf(stderr,
                "FATAL: snapshot install journal read failed; REFUSING "
                "possibly partial state\n");
        event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
                    "snapshot_install_journal_read_failed");
        _exit(EXIT_FAILURE);
    }
    if (!pending)
        return;

    bool bound_pending = true;
    if (!boot_snapshot_install_pending_artifact_matches(
            progress_store_db(), loader_path, &bound_pending) ||
        !bound_pending) {
        fprintf(stderr,
                "FATAL: snapshot install incomplete; the exact prior "
                "height/count/payload-SHA3 artifact is required and must "
                "fully verify before recovery can resume\n");
        event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
                    "snapshot_install_pending_loader_mismatch_or_invalid");
        _exit(EXIT_FAILURE);
    }
}

void boot_snapshot_install_require_complete(void)
{
    bool pending = true;
    if (!boot_snapshot_install_marker_pending(progress_store_db(), &pending) ||
        pending) {
        fprintf(stderr,
                "FATAL: snapshot loader returned with an incomplete install "
                "journal; REFUSING normal boot\n");
        event_emitf(EV_BOOT_VALIDATION_FAILED, 0,
                    "snapshot_install_loader_returned_pending");
        _exit(EXIT_FAILURE);
    }
}

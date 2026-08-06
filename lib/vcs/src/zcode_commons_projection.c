/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Purpose: read-only rebuildable projection of the ZC23 Living Commons. */
#include "vcs/zcode_commons_projection.h"

#include "base/checked.h"
#include "base/hex.h"
#include "base/log_macros.h"
#include "base/safe_alloc.h"
#include "base/serialize_le.h"
#include "crypto/sha3.h"
#include "vcs/vcs_object.h"
#include "vcs/zcode_creation_attribution.h"
#include "vcs/zcode_epoch_creation.h"

#include <dirent.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define COMMONS_LOG "vcs.commons_projection"

static const uint8_t creation_magic[8] =
    {'Z','C','C','R','E','A','\r','\n'};
static const uint8_t epoch_magic[8] =
    {'Z','C','E','P','O','C','\r','\n'};

struct vcs_zcode_commons_projection {
    struct vcs_zcode_commons_creation_entry *creations;
    size_t creation_count;
    struct vcs_zcode_commons_epoch_entry *epochs;
    size_t epoch_count;
    uint64_t attributed_atoms;
    uint64_t minted_atoms;
    uint64_t unissued_atoms;
    enum vcs_zcode_commons_verification_status status;
    bool has_failure;
    uint8_t failure_root[32];
    char failure_reason[48];
};

static bool commons_hex(const char *text, size_t length)
{
    for (size_t i = 0; i < length; i++) {
        char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
    }
    return text[length] == '\0';
}

static void commons_failure(struct vcs_zcode_commons_projection *projection,
                            const uint8_t root[32], const char *reason)
{
    if (projection->has_failure &&
        memcmp(root, projection->failure_root, 32) >= 0)
        return;
    projection->has_failure = true;
    memcpy(projection->failure_root, root, 32);
    (void)snprintf(projection->failure_reason,
                   sizeof(projection->failure_reason), "%s", reason);
}

static int commons_creation_cmp(const void *left, const void *right)
{
    return memcmp(((const struct vcs_zcode_commons_creation_entry *)left)->root,
                  ((const struct vcs_zcode_commons_creation_entry *)right)->root,
                  32);
}

static int commons_epoch_cmp(const void *left, const void *right)
{
    return memcmp(((const struct vcs_zcode_commons_epoch_entry *)left)->root,
                  ((const struct vcs_zcode_commons_epoch_entry *)right)->root,
                  32);
}

static void commons_consider_creation(
    struct vcs_zcode_commons_projection *projection, const uint8_t address[32],
    const uint8_t *wire, size_t wire_len)
{
    struct vcs_zcode_creation_attribution_v1 attribution;
    uint8_t root[32];
    if (vcs_zcode_creation_attribution_parse(wire, wire_len, &attribution) !=
            VCS_ZCODE_CREATION_OK ||
        vcs_zcode_creation_attribution_root(&attribution, root) !=
            VCS_ZCODE_CREATION_OK || memcmp(root, address, 32) != 0) {
        commons_failure(projection, address, "creation-root-or-wire");
        return;
    }
    if (projection->creation_count >=
        VCS_ZCODE_COMMONS_PROJECTION_MAX_OBJECTS) {
        commons_failure(projection, address, "creation-cap");
        return;
    }
    struct vcs_zcode_commons_creation_entry *entry =
        &projection->creations[projection->creation_count++];
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->root, address, 32);
    memcpy(entry->package_root, attribution.package_root, 32);
    memcpy(entry->release_root, attribution.release_root, 32);
    memcpy(entry->candidate_root, attribution.candidate_root, 32);
    memcpy(entry->contributor_binding_root,
           attribution.contributor_binding_root, 32);
    entry->epoch = attribution.epoch;
    entry->award_atoms = attribution.award_atoms;
    entry->category = attribution.category;
    uint64_t next = 0;
    if (!zcl_u64_add(projection->attributed_atoms, attribution.award_atoms,
                     &next))
        commons_failure(projection, address, "attributed-overflow");
    else
        projection->attributed_atoms = next;
}

static void commons_consider_epoch(
    struct vcs_zcode_commons_projection *projection, const uint8_t address[32],
    const uint8_t *wire, size_t wire_len)
{
    struct vcs_zcode_epoch_creation_set_v1 epoch;
    uint8_t root[32];
    if (vcs_zcode_epoch_creation_parse(wire, wire_len, &epoch) !=
            VCS_ZCODE_EPOCH_CREATION_OK ||
        vcs_zcode_epoch_creation_root(&epoch, root) !=
            VCS_ZCODE_EPOCH_CREATION_OK || memcmp(root, address, 32) != 0) {
        commons_failure(projection, address, "epoch-root-or-wire");
        return;
    }
    if (projection->epoch_count >= VCS_ZCODE_COMMONS_PROJECTION_MAX_OBJECTS) {
        commons_failure(projection, address, "epoch-cap");
        vcs_zcode_epoch_creation_free(&epoch);
        return;
    }
    struct vcs_zcode_commons_epoch_entry *entry =
        &projection->epochs[projection->epoch_count++];
    memset(entry, 0, sizeof(*entry));
    memcpy(entry->root, address, 32);
    memcpy(entry->previous_root, epoch.previous_epoch_creation_root, 32);
    entry->epoch = epoch.epoch;
    entry->cap_atoms = epoch.emission_cap_atoms;
    entry->minted_atoms = epoch.actual_mint_atoms;
    entry->unissued_atoms = epoch.unissued_atoms;
    entry->attribution_count = (uint32_t)epoch.attribution_count;
    uint64_t minted = 0, unissued = 0;
    if (!zcl_u64_add(projection->minted_atoms, epoch.actual_mint_atoms,
                     &minted) ||
        !zcl_u64_add(projection->unissued_atoms, epoch.unissued_atoms,
                     &unissued))
        commons_failure(projection, address, "epoch-total-overflow");
    else {
        projection->minted_atoms = minted;
        projection->unissued_atoms = unissued;
    }
    vcs_zcode_epoch_creation_free(&epoch);
}

static void commons_consider(struct vcs_zcode_commons_projection *projection,
                             const char *workspace, const char *hex64)
{
    uint8_t address[32] = {0}, *wire = NULL;
    size_t wire_len = 0;
    if (!zcl_hex_decode_lower(hex64, address, 32)) {
        commons_failure(projection, address, "cas-read");
        return;
    }
    int loaded = vcs_object_load_raw_bounded(
        workspace, address, VCS_ZCODE_EPOCH_CREATION_MAX_WIRE_BYTES,
        &wire, &wire_len);
    /* The shared CAS contains package archives and other objects larger than
     * either commons wire. They are unrelated, not integrity failures. */
    if (loaded == -2)
        return;
    if (loaded != 0) {
        commons_failure(projection, address, "cas-read");
        return;
    }
    if (wire_len == VCS_ZCODE_CREATION_ATTRIBUTION_WIRE_BYTES &&
        memcmp(wire, creation_magic, 8) == 0)
        commons_consider_creation(projection, address, wire, wire_len);
    else if (wire_len >= VCS_ZCODE_EPOCH_CREATION_HEADER_BYTES &&
             memcmp(wire, epoch_magic, 8) == 0)
        commons_consider_epoch(projection, address, wire, wire_len);
    free(wire);
}

static void commons_scan_shard(
    struct vcs_zcode_commons_projection *projection, const char *workspace,
    const char *objects, const char *shard)
{
    char path[4400];
    int n = snprintf(path, sizeof(path), "%s/%s", objects, shard);
    if (n <= 0 || (size_t)n >= sizeof(path))
        return;
    DIR *directory = opendir(path);
    if (!directory)
        return;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL) {
        if (!commons_hex(entry->d_name, 62))
            continue;
        char hex64[65];
        n = snprintf(hex64, sizeof(hex64), "%s%s", shard, entry->d_name);
        if (n == 64)
            commons_consider(projection, workspace, hex64);
    }
    closedir(directory);
}

struct vcs_zcode_commons_projection *vcs_zcode_commons_projection_build(
    const char *workspace)
{
    if (!workspace)
        LOG_RETURN(NULL, COMMONS_LOG, "null workspace");
    struct vcs_zcode_commons_projection *projection =
        zcl_calloc(1, sizeof(*projection), "zcode_commons_projection");
    if (!projection)
        return NULL;
    projection->creations = zcl_calloc(
        VCS_ZCODE_COMMONS_PROJECTION_MAX_OBJECTS,
        sizeof(*projection->creations), "zcode_commons_creations");
    projection->epochs = zcl_calloc(
        VCS_ZCODE_COMMONS_PROJECTION_MAX_OBJECTS,
        sizeof(*projection->epochs), "zcode_commons_epochs");
    if (!projection->creations || !projection->epochs) {
        vcs_zcode_commons_projection_free(projection);
        return NULL;
    }
    char objects[4400];
    int n = snprintf(objects, sizeof(objects), "%s/.zvcs/objects", workspace);
    if (n <= 0 || (size_t)n >= sizeof(objects)) {
        vcs_zcode_commons_projection_free(projection);
        return NULL;
    }
    DIR *directory = opendir(objects);
    if (!directory)
        return projection;
    struct dirent *entry;
    while ((entry = readdir(directory)) != NULL)
        if (commons_hex(entry->d_name, 2))
            commons_scan_shard(projection, workspace, objects, entry->d_name);
    closedir(directory);
    qsort(projection->creations, projection->creation_count,
          sizeof(*projection->creations), commons_creation_cmp);
    qsort(projection->epochs, projection->epoch_count,
          sizeof(*projection->epochs), commons_epoch_cmp);
    projection->status = projection->has_failure
        ? VCS_ZCODE_COMMONS_PARTIAL
        : (projection->creation_count || projection->epoch_count)
            ? VCS_ZCODE_COMMONS_PARTIAL : VCS_ZCODE_COMMONS_UNKNOWN;
    return projection;
}

void vcs_zcode_commons_projection_free(
    struct vcs_zcode_commons_projection *projection)
{
    if (!projection) return;
    free(projection->epochs);
    free(projection->creations);
    free(projection);
}

enum vcs_zcode_commons_verification_status
vcs_zcode_commons_projection_status(
    const struct vcs_zcode_commons_projection *projection)
{ return projection ? projection->status : VCS_ZCODE_COMMONS_UNKNOWN; }

size_t vcs_zcode_commons_projection_creation_count(
    const struct vcs_zcode_commons_projection *projection)
{ return projection ? projection->creation_count : 0; }

size_t vcs_zcode_commons_projection_epoch_count(
    const struct vcs_zcode_commons_projection *projection)
{ return projection ? projection->epoch_count : 0; }

const struct vcs_zcode_commons_creation_entry *
vcs_zcode_commons_projection_creation_at(
    const struct vcs_zcode_commons_projection *projection, size_t index)
{ return projection && index < projection->creation_count
    ? &projection->creations[index] : NULL; }

const struct vcs_zcode_commons_epoch_entry *
vcs_zcode_commons_projection_epoch_at(
    const struct vcs_zcode_commons_projection *projection, size_t index)
{ return projection && index < projection->epoch_count
    ? &projection->epochs[index] : NULL; }

uint64_t vcs_zcode_commons_projection_attributed_atoms(
    const struct vcs_zcode_commons_projection *projection)
{ return projection ? projection->attributed_atoms : 0; }
uint64_t vcs_zcode_commons_projection_minted_atoms(
    const struct vcs_zcode_commons_projection *projection)
{ return projection ? projection->minted_atoms : 0; }
uint64_t vcs_zcode_commons_projection_unissued_atoms(
    const struct vcs_zcode_commons_projection *projection)
{ return projection ? projection->unissued_atoms : 0; }

bool vcs_zcode_commons_projection_root(
    const struct vcs_zcode_commons_projection *projection, uint8_t out[32])
{
    if (!projection || !out) return false;
    struct sha3_256_ctx sha; uint8_t le[8];
    sha3_256_init(&sha);
    sha3_256_write(&sha,
        (const uint8_t *)VCS_ZCODE_COMMONS_PROJECTION_DOMAIN,
        sizeof(VCS_ZCODE_COMMONS_PROJECTION_DOMAIN));
    zcl_write_u64_le(le, projection->creation_count);
    sha3_256_write(&sha, le, 8);
    for (size_t i = 0; i < projection->creation_count; i++)
        sha3_256_write(&sha, projection->creations[i].root, 32);
    zcl_write_u64_le(le, projection->epoch_count);
    sha3_256_write(&sha, le, 8);
    for (size_t i = 0; i < projection->epoch_count; i++)
        sha3_256_write(&sha, projection->epochs[i].root, 32);
    uint8_t status = (uint8_t)projection->status;
    sha3_256_write(&sha, &status, sizeof(status));
    sha3_256_write(&sha, projection->failure_root, 32);
    sha3_256_finalize(&sha, out);
    return true;
}

bool vcs_zcode_commons_projection_first_failure(
    const struct vcs_zcode_commons_projection *projection,
    uint8_t root_out[32], const char **reason_out)
{
    if (root_out) memset(root_out, 0, 32);
    if (reason_out) *reason_out = NULL;
    if (!projection || !root_out || !reason_out || !projection->has_failure)
        return false;
    memcpy(root_out, projection->failure_root, 32);
    *reason_out = projection->failure_reason;
    return true;
}

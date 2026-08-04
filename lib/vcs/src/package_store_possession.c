/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Full-byte package possession proof for signed storage ACKs. */

#include "vcs/package_store.h"

#include "vcs/package_manifest.h"

#include <stdlib.h>
#include <string.h>

bool vcs_package_store_verify_possession(
    struct vcs_package_store *store, const uint8_t package_root[32],
    bool require_pinned)
{
  if (!store || !package_root)
    return false;
  struct vcs_package_store_status before;
  if (!vcs_package_store_package_status(store, package_root, &before) ||
      !before.complete || (require_pinned && !before.pinned))
    return false;
  uint8_t *manifest_wire = NULL;
  size_t manifest_wire_len = 0;
  if (vcs_package_store_get_manifest_wire(
          store, package_root, &manifest_wire, &manifest_wire_len) !=
      VCS_PACKAGE_STORE_OK)
    return false;
  struct vcs_package_manifest manifest;
  memset(&manifest, 0, sizeof(manifest));
  uint8_t derived_root[32];
  bool valid = vcs_package_manifest_parse(
                   manifest_wire, manifest_wire_len, &manifest) &&
               vcs_package_manifest_root(&manifest, derived_root) &&
               memcmp(derived_root, package_root, 32) == 0;
  free(manifest_wire);
  for (size_t file_index = 0; valid && file_index < manifest.count;
       file_index++) {
    const struct vcs_package_file *file = &manifest.files[file_index];
    for (uint32_t chunk_index = 0;
         valid && chunk_index < file->chunk_count; chunk_index++) {
      uint8_t *chunk = NULL;
      size_t chunk_len = 0;
      valid = vcs_package_store_get_chunk_at(
                  store, package_root, (uint32_t)file_index, chunk_index,
                  &chunk, &chunk_len) == VCS_PACKAGE_STORE_OK &&
              vcs_package_verify_chunk(file, chunk_index, chunk, chunk_len);
      free(chunk);
    }
  }
  vcs_package_manifest_free(&manifest);
  struct vcs_package_store_status after;
  return valid &&
         vcs_package_store_package_status(store, package_root, &after) &&
         after.complete && (!require_pinned || after.pinned);
}

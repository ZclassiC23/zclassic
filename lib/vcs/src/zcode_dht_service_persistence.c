/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Canonical crash-safe persistence for authenticated DHT contacts. */

#include "zcode_dht_service_internal.h"

#include "base/safe_alloc.h"
#include "util/write_all.h"
#include "vcs/zcode_dht_identity.h"

#include <errno.h>
#include <fcntl.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <sys/stat.h>
#include <unistd.h>

static bool contacts_path(const struct vcs_zcode_dht_service *s,
                          char out[1400]) {
  int n = snprintf(out, 1400, "%s/%s/contacts.v2", s->datadir,
                   VCS_ZCODE_DHT_IDENTITY_DIR);
  return n > 0 && n < 1400;
}

static uint32_t flatten(const struct vcs_zcode_dht_table *t,
                        struct vcs_zcode_dht_contact *out) {
  uint32_t n = 0;
  for (size_t b = 0; b < VCS_ZCODE_DHT_BUCKET_COUNT; b++)
    for (size_t i = 0; i < t->bucket_sizes[b]; i++)
      out[n++] = t->buckets[b][i];
  return n;
}

struct vcs_zcode_dht_persistence_snapshot {
  uint8_t *wire;
  size_t wire_len;
  uint64_t generation, serial;
  char path[1400], directory[1400], error[96];
};

/* More than one detached snapshot may be written while a composition root is
 * retiring a service.  Keep their temporary names distinct even when both
 * snapshots describe the same persistence generation. */
static _Atomic uint64_t g_snapshot_serial;

struct vcs_zcode_dht_persistence_snapshot *
vcs_zcode_dht_service_persistence_snapshot(
    struct vcs_zcode_dht_service *s, uint64_t monotonic_s, bool force) {
  if (!s || !s->persistence_dirty ||
      (!force && monotonic_s < s->dirty_since_mono +
                                     VCS_ZCODE_DHT_SERVICE_SAVE_DEBOUNCE_S))
    return NULL;
  struct vcs_zcode_dht_persistence_snapshot *snapshot =
      zcl_calloc(1, sizeof(*snapshot), "dht.save.snapshot");
  struct vcs_zcode_dht_contact *contacts = zcl_malloc(
      VCS_ZCODE_DHT_MAX_CONTACTS * sizeof(*contacts), "dht.save.contacts");
  uint8_t *wire =
      zcl_malloc(VCS_ZCODE_DHT_CONTACTS_MAX_WIRE_BYTES, "dht.save.wire");
  if (!snapshot || !contacts || !wire) {
    free(snapshot);
    free(contacts);
    free(wire);
    vcs_zcode_dht_service_set_error(s, "persistence allocation failed");
    return NULL;
  }
  uint32_t count = flatten(s->table, contacts);
  size_t len = 0;
  enum vcs_zcode_dht_error e = vcs_zcode_dht_contacts_serialize(
      contacts, count, s->genesis, s->self_id, wire,
      VCS_ZCODE_DHT_CONTACTS_MAX_WIRE_BYTES, &len);
  free(contacts);
  if (e != VCS_ZCODE_DHT_OK) {
    free(wire);
    free(snapshot);
    vcs_zcode_dht_service_set_error(s, "contacts serialize failed");
    return NULL;
  }
  if (!contacts_path(s, snapshot->path)) {
    free(wire);
    free(snapshot);
    vcs_zcode_dht_service_set_error(s, "contacts path too long");
    return NULL;
  }
  (void)snprintf(snapshot->directory, sizeof(snapshot->directory), "%s/%s",
                 s->datadir,
                 VCS_ZCODE_DHT_IDENTITY_DIR);
  snapshot->wire = wire;
  snapshot->wire_len = len;
  snapshot->generation = s->persistence_generation;
  snapshot->serial = atomic_fetch_add_explicit(
                         &g_snapshot_serial, 1, memory_order_relaxed) +
                     1;
  return snapshot;
}

bool vcs_zcode_dht_persistence_snapshot_write(
    struct vcs_zcode_dht_persistence_snapshot *snapshot) {
  if (!snapshot || !snapshot->wire)
    return false;
  char tmp[1460];
  (void)snprintf(tmp, sizeof(tmp), "%s.tmp.%ld.%llu.%llu", snapshot->path,
                 (long)getpid(),
                 (unsigned long long)snapshot->generation,
                 (unsigned long long)snapshot->serial);
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  bool ok = fd >= 0 &&
            zcl_write_all(fd, snapshot->wire, snapshot->wire_len) &&
            fsync(fd) == 0;
  if (fd >= 0 && close(fd) != 0)
    ok = false;
  if (!ok) {
    (void)unlink(tmp);
    snprintf(snapshot->error, sizeof(snapshot->error),
             "contacts temp write failed");
    return false;
  }
  if (rename(tmp, snapshot->path) != 0) {
    (void)unlink(tmp);
    snprintf(snapshot->error, sizeof(snapshot->error),
             "contacts rename failed");
    return false;
  }
  int dfd = open(snapshot->directory,
                 O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (dfd < 0 || fsync(dfd) != 0) {
    if (dfd >= 0)
      (void)close(dfd);
    snprintf(snapshot->error, sizeof(snapshot->error),
             "contacts directory fsync failed");
    return false;
  }
  (void)close(dfd);
  return true;
}

void vcs_zcode_dht_service_persistence_commit(
    struct vcs_zcode_dht_service *s,
    const struct vcs_zcode_dht_persistence_snapshot *snapshot, bool written) {
  if (!s || !snapshot)
    return;
  if (!written) {
    vcs_zcode_dht_service_set_error(
        s, snapshot->error[0] ? snapshot->error : "contacts write failed");
    return;
  }
  s->persistence_save_count++;
  if (s->persistence_generation == snapshot->generation)
    s->persistence_dirty = false;
}

void vcs_zcode_dht_persistence_snapshot_free(
    struct vcs_zcode_dht_persistence_snapshot *snapshot) {
  if (!snapshot)
    return;
  free(snapshot->wire);
  free(snapshot);
}

bool vcs_zcode_dht_service_persistence_save(struct vcs_zcode_dht_service *s) {
  if (!s || !s->persistence_dirty)
    return true;
  struct vcs_zcode_dht_persistence_snapshot *snapshot =
      vcs_zcode_dht_service_persistence_snapshot(s, 0, true);
  if (!snapshot)
    return false;
  bool written = vcs_zcode_dht_persistence_snapshot_write(snapshot);
  vcs_zcode_dht_service_persistence_commit(s, snapshot, written);
  vcs_zcode_dht_persistence_snapshot_free(snapshot);
  return written;
}

bool vcs_zcode_dht_service_persistence_load(struct vcs_zcode_dht_service *s,
                                            uint64_t now) {
  char path[1400];
  if (!contacts_path(s, path))
    return false;
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0)
    return errno == ENOENT;
  struct stat st;
  if (fstat(fd, &st) != 0 ||
      st.st_size < (off_t)VCS_ZCODE_DHT_CONTACTS_HEADER_BYTES ||
      st.st_size > (off_t)VCS_ZCODE_DHT_CONTACTS_MAX_WIRE_BYTES) {
    (void)close(fd);
    vcs_zcode_dht_service_set_error(s, "contacts file size invalid");
    return false;
  }
  size_t len = (size_t)st.st_size;
  uint8_t *wire = zcl_malloc(len, "dht.load.wire");
  struct vcs_zcode_dht_contact *contacts = zcl_malloc(
      VCS_ZCODE_DHT_MAX_CONTACTS * sizeof(*contacts), "dht.load.contacts");
  struct vcs_zcode_dht_table *tmp = zcl_malloc(sizeof(*tmp), "dht.load.table");
  if (!wire || !contacts || !tmp) {
    (void)close(fd);
    free(wire);
    free(contacts);
    free(tmp);
    vcs_zcode_dht_service_set_error(s, "contacts load allocation failed");
    return false;
  }
  size_t off = 0;
  while (off < len) {
    ssize_t n = read(fd, wire + off, len - off);
    if (n < 0 && errno == EINTR)
      continue;
    if (n <= 0)
      break;
    off += (size_t)n;
  }
  (void)close(fd);
  uint32_t count = 0;
  enum vcs_zcode_dht_error e =
      off == len
          ? vcs_zcode_dht_contacts_parse(
                wire, len, s->genesis, s->self_id, now, s->chain_verify,
                s->chain_ctx, contacts, VCS_ZCODE_DHT_MAX_CONTACTS, &count)
          : VCS_ZCODE_DHT_ERR_WIRE_SIZE;
  free(wire);
  bool ok = e == VCS_ZCODE_DHT_OK && vcs_zcode_dht_table_init(tmp, s->self_id);
  for (uint32_t i = 0; ok && i < count; i++)
    ok = vcs_zcode_dht_table_add_contact(tmp, &contacts[i], (int64_t)now) ==
         VCS_ZCODE_DHT_ADD_ADDED;
  free(contacts);
  if (!ok) {
    free(tmp);
    vcs_zcode_dht_service_set_error(s, vcs_zcode_dht_error_string(e));
    return false;
  }
  free(s->table);
  s->table = tmp;
  s->persistence_loaded = true;
  s->persistence_load_count = count;
  return true;
}

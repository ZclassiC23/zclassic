/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Canonical crash-safe persistence for authenticated DHT contacts. */

#include "zcode_dht_service_internal.h"

#include "base/safe_alloc.h"
#include "util/write_all.h"
#include "vcs/zcode_dht_identity.h"

#include <errno.h>
#include <fcntl.h>
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

bool vcs_zcode_dht_service_persistence_save(struct vcs_zcode_dht_service *s) {
  struct vcs_zcode_dht_contact *contacts = zcl_malloc(
      VCS_ZCODE_DHT_MAX_CONTACTS * sizeof(*contacts), "dht.save.contacts");
  uint8_t *wire =
      zcl_malloc(VCS_ZCODE_DHT_CONTACTS_MAX_WIRE_BYTES, "dht.save.wire");
  if (!contacts || !wire) {
    free(contacts);
    free(wire);
    vcs_zcode_dht_service_set_error(s, "persistence allocation failed");
    return false;
  }
  uint32_t count = flatten(s->table, contacts);
  size_t len = 0;
  enum vcs_zcode_dht_error e = vcs_zcode_dht_contacts_serialize(
      contacts, count, s->genesis, s->self_id, wire,
      VCS_ZCODE_DHT_CONTACTS_MAX_WIRE_BYTES, &len);
  free(contacts);
  if (e != VCS_ZCODE_DHT_OK) {
    free(wire);
    vcs_zcode_dht_service_set_error(s, "contacts serialize failed");
    return false;
  }
  char path[1400], tmp[1460], dir[1400];
  if (!contacts_path(s, path)) {
    free(wire);
    vcs_zcode_dht_service_set_error(s, "contacts path too long");
    return false;
  }
  (void)snprintf(dir, sizeof(dir), "%s/%s", s->datadir,
                 VCS_ZCODE_DHT_IDENTITY_DIR);
  (void)snprintf(tmp, sizeof(tmp), "%s.tmp.%ld", path, (long)getpid());
  int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
  bool ok = fd >= 0 && zcl_write_all(fd, wire, len) && fsync(fd) == 0;
  free(wire);
  if (fd >= 0 && close(fd) != 0)
    ok = false;
  if (!ok) {
    (void)unlink(tmp);
    vcs_zcode_dht_service_set_error(s, "contacts temp write failed");
    return false;
  }
  if (rename(tmp, path) != 0) {
    (void)unlink(tmp);
    vcs_zcode_dht_service_set_error(s, "contacts rename failed");
    return false;
  }
  int dfd = open(dir, O_RDONLY | O_DIRECTORY | O_CLOEXEC | O_NOFOLLOW);
  if (dfd < 0 || fsync(dfd) != 0) {
    if (dfd >= 0)
      (void)close(dfd);
    vcs_zcode_dht_service_set_error(s, "contacts directory fsync failed");
    return false;
  }
  (void)close(dfd);
  s->persistence_dirty = false;
  s->persistence_save_count++;
  return true;
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

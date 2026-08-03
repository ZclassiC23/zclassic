/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * purpose: Native provisioning and live adapters for the ZCODE DHT. */

#include "command/native_command.h"

#include "base/hex.h"
#include "chain/chainparams.h"
#include "controllers/rpc_client.h"
#include "controllers/rpc_params.h"
#include "crypto/ed25519.h"
#include "models/zid_identity.h"
#include "net/v2_identity.h"
#include "platform/time_compat.h"
#include "support/cleanse.h"
#include "validation/main_constants.h"
#include "vcs/zcode_dht_identity.h"
#include "json/json.h"

#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define ZDN_COMMAND "zcode.network.delegate"

static void zdn_fail(struct zcl_command_reply *reply, const char *code,
                     const char *phase, const char *message,
                     const char *evidence) {
  zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                         ZCL_COMMAND_EXIT_INVALID, code, phase, false, false,
                         message, evidence ? evidence : ZDN_COMMAND);
}

static const char *zdn_str(const struct json_value *in, const char *key) {
  const struct json_value *v = in ? json_get(in, key) : NULL;
  return v && v->type == JSON_STR ? json_get_str(v) : NULL;
}

static bool zdn_u64(const struct json_value *in, const char *key, uint64_t *out,
                    bool *present) {
  const struct json_value *v = in ? json_get(in, key) : NULL;
  *present = v != NULL;
  if (!v)
    return true;
  if (v->type == JSON_INT && json_get_int(v) >= 0) {
    *out = (uint64_t)json_get_int(v);
    return true;
  }
  const char *s = v->type == JSON_STR ? json_get_str(v) : NULL;
  if (!s || !s[0] || s[0] == '-')
    return false;
  errno = 0;
  char *end = NULL;
  unsigned long long parsed = strtoull(s, &end, 10);
  if (errno != 0 || !end || *end != '\0')
    return false;
  *out = (uint64_t)parsed;
  return true;
}

static bool zdn_read_master_seed(const char *path, uint8_t out[32], char *err,
                                 size_t err_cap) {
  int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
  if (fd < 0) {
    snprintf(err, err_cap, "cannot open master seed: %s", strerror(errno));
    return false;
  }
  struct stat st;
  if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode) ||
      ((st.st_mode & 0777) != 0600 && (st.st_mode & 0777) != 0400)) {
    close(fd);
    snprintf(err, err_cap, "master seed must be a regular mode-0400/0600 file");
    return false;
  }
  char hex[66];
  ssize_t n = read(fd, hex, sizeof(hex));
  close(fd);
  if (n != 64 && !(n == 65 && hex[64] == '\n')) {
    memory_cleanse(hex, sizeof(hex));
    snprintf(err, err_cap, "master seed must contain exactly 64 hex chars");
    return false;
  }
  hex[64] = '\0';
  bool ok = zcl_hex_decode_lower(hex, out, 32);
  memory_cleanse(hex, sizeof(hex));
  if (!ok)
    snprintf(err, err_cap, "master seed is not canonical lowercase hex");
  return ok;
}

static bool zdn_rpc_int(const char *method, int64_t arg, bool has_arg,
                        int64_t *out) {
  struct rpc_arg_builder params;
  rpc_arg_builder_init(&params);
  if (has_arg)
    rpc_arg_builder_push_int(&params, arg);
  char *json_params = rpc_arg_builder_to_json(&params);
  char *raw = node_rpc_call(method, json_params);
  free(json_params);
  if (!raw)
    return false;
  struct json_value doc;
  bool ok = json_read(&doc, raw, strlen(raw)) && doc.type == JSON_INT;
  if (ok)
    *out = json_get_int(&doc);
  json_free(&doc);
  free(raw);
  return ok;
}

static bool zdn_rpc_block_hash(int64_t height, struct uint256 *out) {
  struct rpc_arg_builder params;
  rpc_arg_builder_init(&params);
  rpc_arg_builder_push_int(&params, height);
  char *json_params = rpc_arg_builder_to_json(&params);
  char *raw = node_rpc_call("getblockhash", json_params);
  free(json_params);
  if (!raw)
    return false;
  struct json_value doc;
  bool ok = json_read(&doc, raw, strlen(raw)) && doc.type == JSON_STR;
  const char *hex = ok ? json_get_str(&doc) : NULL;
  if (ok && hex && strlen(hex) == 64)
    uint256_set_hex(out, hex);
  else
    ok = false;
  json_free(&doc);
  free(raw);
  return ok;
}

enum zdn_existing_result {
  ZDN_EXISTING_ABSENT = 0,
  ZDN_EXISTING_VALID,
  ZDN_EXISTING_CORRUPT,
};

static enum zdn_existing_result zdn_existing_sequence(const char *datadir,
                                                      uint64_t *sequence,
                                                      char *err,
                                                      size_t err_cap) {
  char path[1400];
  int n = snprintf(path, sizeof(path), "%s/%s/%s", datadir,
                   VCS_ZCODE_DHT_IDENTITY_DIR, VCS_ZCODE_DHT_DELEGATION_FILE);
  if (n <= 0 || (size_t)n >= sizeof(path)) {
    snprintf(err, err_cap, "delegation path too long");
    return ZDN_EXISTING_CORRUPT;
  }
  if (access(path, F_OK) != 0) {
    if (errno == ENOENT)
      return ZDN_EXISTING_ABSENT;
    snprintf(err, err_cap, "cannot inspect existing delegation: %s",
             strerror(errno));
    return ZDN_EXISTING_CORRUPT;
  }
  struct vcs_zcode_dht_delegation old;
  if (!vcs_zcode_dht_delegation_load(datadir, &old, err, err_cap))
    return ZDN_EXISTING_CORRUPT;
  *sequence = old.doc.seq;
  return ZDN_EXISTING_VALID;
}

void zcl_native_handle_zcode_network_delegate(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply) {
  if (!request || !reply)
    return;
  const char *seed_file = zdn_str(request->input, "seed_file");
  const char *datadir = zdn_str(request->input, "datadir");
  if (!datadir || !datadir[0])
    datadir = zcl_native_command_datadir();
  if (!seed_file || !seed_file[0] || !datadir || !datadir[0]) {
    zdn_fail(reply, "MISSING_INPUT", "normalize",
             "seed_file and datadir are required", ZDN_COMMAND);
    return;
  }

  uint64_t now = (uint64_t)platform_time_wall_unix();
  bool supplied = false;
  if (!zdn_u64(request->input, "now", &now, &supplied)) {
    zdn_fail(reply, "BAD_NOW", "normalize", "now must be uint64", ZDN_COMMAND);
    return;
  }
  if (now > (uint64_t)INT64_MAX ||
      now > UINT64_MAX - VCS_ZCODE_DHT_DELEGATION_DEFAULT_SECONDS) {
    zdn_fail(reply, "BAD_NOW", "normalize",
             "now is outside the supported signed time range", ZDN_COMMAND);
    return;
  }
  uint64_t expiry = now + VCS_ZCODE_DHT_DELEGATION_DEFAULT_SECONDS;
  if (!zdn_u64(request->input, "expiry", &expiry, &supplied) || expiry <= now) {
    zdn_fail(reply, "BAD_WINDOW", "normalize",
             "expiry must be a uint64 strictly after now", ZDN_COMMAND);
    return;
  }
  if (expiry > (uint64_t)INT64_MAX) {
    zdn_fail(reply, "BAD_WINDOW", "normalize",
             "expiry is outside the supported signed time range", ZDN_COMMAND);
    return;
  }

  uint8_t master_seed[32], master_pub[32], master_secret_copy[32];
  char err[192] = {0};
  if (!zdn_read_master_seed(seed_file, master_seed, err, sizeof(err))) {
    zdn_fail(reply, "MASTER_SEED_UNREADABLE", "normalize", err, seed_file);
    return;
  }
  zcl_ed25519_keypair(master_pub, master_secret_copy, master_seed);
  memory_cleanse(master_secret_copy, sizeof(master_secret_copy));

  sqlite3 *db = NULL;
  struct node_db ndb;
  if (!zcl_native_node_db_require_readonly(
          datadir, reply, "the ZID anchor projection", &db, &ndb)) {
    memory_cleanse(master_seed, sizeof(master_seed));
    return;
  }
  struct zid_identity identity;
  bool found = db_zid_identity_find(&ndb, master_pub, &identity);
  zcl_native_node_db_close_readonly(&db, &ndb);
  if (!found || strcmp(identity.status, ZID_IDENTITY_STATUS_ACTIVE) != 0) {
    memory_cleanse(master_seed, sizeof(master_seed));
    zdn_fail(reply, found ? "MASTER_NOT_ACTIVE" : "MASTER_NOT_ANCHORED",
             "authorize", "master identity must be ACTIVE on-chain",
             ZDN_COMMAND);
    return;
  }
  if (identity.anchor_height < 0 ||
      identity.anchor_height > INT_MAX - 2 * ZCL_FINALITY_DEPTH) {
    memory_cleanse(master_seed, sizeof(master_seed));
    zdn_fail(reply, "ANCHOR_HEIGHT_INVALID", "authorize",
             "anchor height cannot produce a final beacon", ZDN_COMMAND);
    return;
  }
  int64_t tip = -1;
  zcl_native_bridge_ensure_rpc();
  if (!zdn_rpc_int("getblockcount", 0, false, &tip)) {
    memory_cleanse(master_seed, sizeof(master_seed));
    zdn_fail(reply, "CHAIN_UNAVAILABLE", "authorize",
             "running node did not return its provable tip", ZDN_COMMAND);
    return;
  }
  uint32_t beacon_height =
      (uint32_t)(identity.anchor_height + ZCL_FINALITY_DEPTH);
  if (tip < (int64_t)beacon_height + ZCL_FINALITY_DEPTH) {
    memory_cleanse(master_seed, sizeof(master_seed));
    zdn_fail(reply, "BEACON_PROVISIONAL", "authorize",
             "the identity beacon is not itself ten blocks deep", ZDN_COMMAND);
    return;
  }
  struct uint256 beacon;
  if (!zdn_rpc_block_hash(beacon_height, &beacon)) {
    memory_cleanse(master_seed, sizeof(master_seed));
    zdn_fail(reply, "BEACON_UNAVAILABLE", "authorize",
             "running node could not resolve the beacon block", ZDN_COMMAND);
    return;
  }

  uint8_t noise_seed[32], noise_pub[32], online_seed[32], online_pub[32];
  if (!v2_identity_load_or_create(datadir, noise_seed, noise_pub, err,
                                  sizeof(err)) ||
      !vcs_zcode_dht_online_key_load_or_create(datadir, online_seed, online_pub,
                                               err, sizeof(err))) {
    memory_cleanse(master_seed, 32);
    memory_cleanse(noise_seed, 32);
    memory_cleanse(online_seed, 32);
    zdn_fail(reply, "IDENTITY_IO", "persist", err, ZDN_COMMAND);
    return;
  }
  memory_cleanse(noise_seed, sizeof(noise_seed));

  uint64_t sequence = 1, previous = 0;
  bool seq_present = false;
  if (!zdn_u64(request->input, "sequence", &sequence, &seq_present)) {
    memory_cleanse(master_seed, 32);
    memory_cleanse(online_seed, 32);
    zdn_fail(reply, "BAD_SEQUENCE", "normalize", "sequence must be uint64",
             ZDN_COMMAND);
    return;
  }
  enum zdn_existing_result existing =
      zdn_existing_sequence(datadir, &previous, err, sizeof(err));
  if (existing == ZDN_EXISTING_CORRUPT) {
    memory_cleanse(master_seed, 32);
    memory_cleanse(online_seed, 32);
    zdn_fail(reply, "EXISTING_DELEGATION_CORRUPT", "normalize", err,
             ZDN_COMMAND);
    return;
  }
  if (existing == ZDN_EXISTING_VALID) {
    if (!seq_present) {
      if (previous == UINT64_MAX) {
        memory_cleanse(master_seed, 32);
        memory_cleanse(online_seed, 32);
        zdn_fail(reply, "SEQUENCE_OVERFLOW", "normalize",
                 "delegation sequence cannot advance", ZDN_COMMAND);
        return;
      }
      sequence = previous + 1;
    } else if (sequence <= previous) {
      memory_cleanse(master_seed, 32);
      memory_cleanse(online_seed, 32);
      zdn_fail(reply, "STALE_SEQUENCE", "normalize",
               "sequence must exceed the filed delegation", ZDN_COMMAND);
      return;
    }
  }
  if (sequence > (uint64_t)INT64_MAX) {
    memory_cleanse(master_seed, 32);
    memory_cleanse(online_seed, 32);
    zdn_fail(reply, "BAD_SEQUENCE", "normalize",
             "sequence is outside the supported signed range", ZDN_COMMAND);
    return;
  }

  const struct chain_params *params = chain_params_get();
  struct vcs_zcode_dht_delegation delegation;
  enum vcs_zcode_dht_delegation_error signed_result =
      vcs_zcode_dht_delegation_sign(
          &delegation, params->consensus.hashGenesisBlock.data, online_pub,
          noise_pub, beacon_height, beacon.data, now, expiry, sequence,
          master_seed);
  memory_cleanse(master_seed, sizeof(master_seed));
  memory_cleanse(online_seed, sizeof(online_seed));
  if (signed_result != VCS_ZCODE_DHT_DELEGATION_OK ||
      !vcs_zcode_dht_delegation_save(datadir, &delegation, err, sizeof(err))) {
    zdn_fail(reply, "DELEGATION_WRITE_FAILED", "persist",
             signed_result == VCS_ZCODE_DHT_DELEGATION_OK
                 ? err
                 : vcs_zcode_dht_delegation_error_string(signed_result),
             ZDN_COMMAND);
    return;
  }

  uint8_t node_id[32];
  char node_hex[65], beacon_hex[65];
  if (!vcs_zcode_dht_delegation_node_id(node_id, &delegation)) {
    zdn_fail(reply, "NODE_ID_FAILED", "serialize",
             "signed delegation did not derive a node ID", ZDN_COMMAND);
    return;
  }
  zcl_hex_encode(node_id, 32, node_hex);
  zcl_hex_encode(beacon.data, 32, beacon_hex);
  json_push_kv_str(&reply->data, "node_id", node_hex);
  json_push_kv_int(&reply->data, "sequence", (int64_t)sequence);
  json_push_kv_int(&reply->data, "not_before", (int64_t)now);
  json_push_kv_int(&reply->data, "expiry", (int64_t)expiry);
  json_push_kv_int(&reply->data, "beacon_height", beacon_height);
  json_push_kv_str(&reply->data, "beacon_hash", beacon_hex);
  json_push_kv_bool(&reply->data, "key_material_returned", false);
  reply->status = ZCL_COMMAND_STATUS_PASSED;
  reply->exit_code = ZCL_COMMAND_EXIT_OK;
}

static void zdn_forward(const struct zcl_command_request *request,
                        struct zcl_command_reply *reply,
                        const char *rpc_method) {
  if (!request || !reply || !rpc_method)
    return;
  struct json_value empty;
  json_init(&empty);
  json_set_object(&empty);
  const struct json_value *input =
      request->input && request->input->type == JSON_OBJ ? request->input
                                                         : &empty;
  struct rpc_arg_builder args;
  rpc_arg_builder_init(&args);
  rpc_arg_builder_push_value(&args, input);
  char *params = rpc_arg_builder_to_json(&args);
  json_free(&empty);
  if (!params) {
    zdn_fail(reply, "ARG_BUILD_FAILED", "normalize",
             "could not encode the bounded DHT request", rpc_method);
    return;
  }
  zcl_native_bridge_ensure_rpc();
  char *raw = node_rpc_call(rpc_method, params);
  free(params);
  if (!raw) {
    zcl_command_reply_fail(
        reply, ZCL_COMMAND_STATUS_BLOCKED, ZCL_COMMAND_EXIT_TRANSIENT,
        "NODE_UNAVAILABLE", "dispatch", true, false,
        "the running node returned no DHT response", rpc_method);
    return;
  }
  struct json_value body;
  json_init(&body);
  if (!json_read(&body, raw, strlen(raw)) || body.type != JSON_OBJ) {
    free(raw);
    json_free(&body);
    zcl_command_reply_fail(
        reply, ZCL_COMMAND_STATUS_FAILED, ZCL_COMMAND_EXIT_INTERNAL,
        "BAD_RPC_BODY", "serialize", false, false,
        "the DHT RPC returned an unreadable body", rpc_method);
    return;
  }
  free(raw);
  if (!json_get_bool_or(&body, "ok", false)) {
    const char *code = json_get_str(json_get(&body, "code"));
    const char *message = json_get_str(json_get(&body, "message"));
    bool timeout = code && strcmp(code, "LOOKUP_TIMEOUT") == 0;
    zcl_command_reply_fail(
        reply, timeout ? ZCL_COMMAND_STATUS_BLOCKED : ZCL_COMMAND_STATUS_FAILED,
        timeout ? ZCL_COMMAND_EXIT_TRANSIENT : ZCL_COMMAND_EXIT_FAILED,
        code && code[0] ? code : "DHT_REFUSED", "execute", timeout, false,
        message && message[0] ? message : "the DHT service refused the request",
        rpc_method);
    /* LOOKUP_TIMEOUT deliberately retains bounded partial node-id evidence. */
    json_copy(&reply->data, &body);
    json_free(&body);
    return;
  }
  json_copy(&reply->data, &body);
  json_free(&body);
  reply->status = ZCL_COMMAND_STATUS_PASSED;
  reply->exit_code = ZCL_COMMAND_EXIT_OK;
}

void zcl_native_handle_zcode_network_status(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply) {
  zdn_forward(request, reply, "zcode_dht_status");
}

void zcl_native_handle_zcode_network_peers(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply) {
  zdn_forward(request, reply, "zcode_dht_peers");
}

void zcl_native_handle_zcode_network_find(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply) {
  zdn_forward(request, reply, "zcode_dht_find");
}

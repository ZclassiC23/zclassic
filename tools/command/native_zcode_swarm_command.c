/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Native handlers for the slice-12 `zcode package` swarm leaves — the
 * authenticated package swarm's operator surface:
 *
 *   zcode package fetch    start (or resume) a swarm download of one
 *                          package root: against the live node-global
 *                          engine when the hosting node is running,
 *                          otherwise a one-shot engine over the datadir
 *                          store persists the resumable download record
 *                          (<datadir>/zcode/downloads/<root-hex>) for the
 *                          next hosting boot to resume
 *   zcode package peers    the live swarm's view of the peers advertising
 *                          one package root (tier, in-flight, verified
 *                          bytes both ways, allowance + offence state)
 *   zcode package pin      operator-pin a tracked package (PINS pool,
 *                          never evicted, never tier-gated)
 *   zcode package unpin    release an operator pin
 *
 * LIVE VS ONE-SHOT (the same discipline as the publish branch): the CAS
 * bytes under <datadir>/zcode are the only package truth. fetch reads
 * the node-global engine (set by the boot glue when -packagehost=1) so a
 * running node downloads immediately; a one-shot CLI never fakes liveness
 * — it persists the download record and reports live:false, and the
 * engine's resume replay picks the record up at the next hosting boot.
 * Every rejection names the exact rule. */

#include "base/hex.h"
#include "command/native_command.h"

#include "json/json.h"
#include "platform/time_compat.h"
#include "crypto/sha3.h"
#include "vcs/package_reward.h"
#include "vcs/package_service.h"
#include "vcs/package_store.h"
#include "vcs/package_swarm_node.h"

#include <stdio.h>
#include <string.h>

/* Render cap for peer rows (the LIST budget). */
#define ZW_MAX_PEERS 32u

/* ── small input helpers (the native_zcode_* pattern) ───────────────── */

static const char *zw_input_str(const struct json_value *input,
                                const char *key)
{
    const struct json_value *v = json_get(input, key);
    return v ? json_get_str(v) : NULL;
}

static const char *zw_datadir(const struct zcl_command_request *request)
{
    const char *dd = zw_input_str(request->input, "datadir");
    if (dd && dd[0])
        return dd;
    dd = zcl_native_command_datadir();
    return (dd && dd[0]) ? dd : NULL;
}

static bool zw_zcode_dir(const struct zcl_command_request *request,
                         struct zcl_command_reply *reply,
                         const char *command, char out[4400])
{
    const char *datadir = zw_datadir(request);
    if (!datadir) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_DATADIR",
                               "normalize", false, false,
                               "no datadir given (input datadir or --datadir)",
                               command);
        return false;
    }
    int n = snprintf(out, 4400, "%s/zcode", datadir);
    if (n < 0 || (size_t)n >= 4400) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "DATADIR_TOO_LONG",
                               "normalize", false, false,
                               "datadir path too long", datadir);
        return false;
    }
    return true;
}

/* Parse the required `root` input (64 lowercase hex). False with the
 * error body set on bad input. */
static bool zw_root(const struct zcl_command_request *request,
                    struct zcl_command_reply *reply, const char *command,
                    uint8_t out[32])
{
    const char *hex = zw_input_str(request->input, "root");
    if (!hex || !zcl_hex_decode(hex, out, 32)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "BAD_ROOT",
                               "normalize", false, false,
                               "root must be 64 lowercase hex chars (the "
                               "package root)", hex ? hex : "(missing)");
        return false;
    }
    (void)command;
    return true;
}

static void zw_push_status(struct json_value *obj,
                           const struct vcs_swarm_download_status *st)
{
    (void)json_push_kv_str(obj, "state",
                           vcs_swarm_download_state_string(st->state));
    if (st->rule)
        (void)json_push_kv_str(obj, "rule", st->rule);
    (void)json_push_kv_int(obj, "advertisers", (int64_t)st->advertisers);
    (void)json_push_kv_int(obj, "inflight", (int64_t)st->inflight);
    (void)json_push_kv_int(obj, "present_chunks",
                           (int64_t)st->present_chunks);
    (void)json_push_kv_int(obj, "total_chunks", (int64_t)st->total_chunks);
    (void)json_push_kv_int(obj, "present_bytes", (int64_t)st->present_bytes);
    (void)json_push_kv_int(obj, "total_bytes", (int64_t)st->total_bytes);
    (void)json_push_kv_int(obj, "fetched_bytes", (int64_t)st->fetched_bytes);
}

/* ── zcode package fetch ────────────────────────────────────────────── */

void zcl_native_handle_zcode_package_fetch(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    char zcode_dir[4400];
    if (!zw_zcode_dir(request, reply, "zcode.package.fetch", zcode_dir))
        return;
    uint8_t root[32];
    if (!zw_root(request, reply, "zcode.package.fetch", root))
        return;
    /* Live engine first: a running hosting node downloads immediately. */
    struct vcs_swarm_engine *engine = vcs_swarm_engine_global();
    bool live = engine != NULL;

    /* One-shot fallback: open the datadir store + service book and build
     * a temporary engine whose ONLY lasting effect is the persisted,
     * resumable download record (the engine replays <zcode_dir>/
     * downloads at creation, so the next hosting boot resumes it). */
    struct vcs_package_store *store = NULL;
    struct vcs_service_book *book = NULL;
    if (!live) {
        store = vcs_package_store_open(zw_datadir(request),
                                       vcs_package_store_quota_bytes());
        if (!store) {
            zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                                   ZCL_COMMAND_EXIT_INTERNAL, "NO_STORE",
                                   "execute", false, false,
                                   "the package store failed to open",
                                   zcode_dir);
            return;
        }
        book = vcs_service_book_load(zcode_dir);
        engine = vcs_swarm_engine_create(store, book, zcode_dir, NULL, NULL);
        if (!engine) {
            vcs_service_book_free(book);
            vcs_package_store_close(store);
            zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                                   ZCL_COMMAND_EXIT_INTERNAL, "ENGINE",
                                   "execute", false, false,
                                   "the swarm engine failed to initialize",
                                   zcode_dir);
            return;
        }
    }

    int64_t day = (int64_t)platform_time_wall_unix() / 86400;
    const struct json_value *dv = json_get(request->input, "day");
    if (dv)
        day = json_get_int(dv);
    uint64_t now = (uint64_t)platform_time_wall_unix();
    enum vcs_swarm_fetch_result r =
        vcs_swarm_engine_fetch(engine, root, day, now);

    struct vcs_swarm_download_status st;
    memset(&st, 0, sizeof(st));
    bool have_status = vcs_swarm_engine_download_status(engine, root, &st);

    if (!live) {
        vcs_swarm_engine_free(engine);
        vcs_service_book_free(book);
        vcs_package_store_close(store);
    }

    if (r != VCS_SWARM_FETCH_OK && r != VCS_SWARM_FETCH_ALREADY_COMPLETE) {
        const char *code = "FETCH_REFUSED";
        if (r == VCS_SWARM_FETCH_NO_STORE)
            code = "NO_STORE";
        else if (r == VCS_SWARM_FETCH_FULL)
            code = "DOWNLOAD_SLOTS_FULL";
        else if (r == VCS_SWARM_FETCH_RECORD_IO)
            code = "RECORD_IO";
        else if (r == VCS_SWARM_FETCH_BAD_INPUT)
            code = "BAD_ROOT";
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, code, "execute",
                               false, true,
                               vcs_swarm_fetch_result_string(r),
                               "zcode.package.fetch");
        return;
    }

    char hex[65];
    zcl_hex_encode(root, 32, hex);
    (void)json_push_kv_str(&reply->data, "package_root", hex);
    (void)json_push_kv_bool(&reply->data, "live", live);
    (void)json_push_kv_bool(&reply->data, "already_complete",
                            r == VCS_SWARM_FETCH_ALREADY_COMPLETE);
    (void)json_push_kv_str(
        &reply->data, "result", vcs_swarm_fetch_result_string(r));
    if (have_status) {
        struct json_value sj;
        json_init(&sj);
        json_set_object(&sj);
        zw_push_status(&sj, &st);
        (void)json_push_kv(&reply->data, "download", &sj);
        json_free(&sj);
    }
    if (!live)
        (void)json_push_kv_str(
            &reply->data, "note",
            "no live hosting engine: the resumable download record is "
            "persisted under <datadir>/zcode/downloads and the next "
            "-packagehost=1 boot resumes it (manifest-first, then chunks "
            "rarest-first from the peers advertising the root)");
}

/* ── zcode package peers ────────────────────────────────────────────── */

void zcl_native_handle_zcode_package_peers(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    uint8_t root[32];
    if (!zw_root(request, reply, "zcode.package.peers", root))
        return;

    char hex[65];
    zcl_hex_encode(root, 32, hex);
    (void)json_push_kv_str(&reply->data, "package_root", hex);

    struct vcs_swarm_engine *engine = vcs_swarm_engine_global();
    (void)json_push_kv_bool(&reply->data, "live", engine != NULL);
    if (!engine) {
        (void)json_push_kv_int(&reply->data, "peer_count", 0);
        (void)json_push_kv_str(
            &reply->data, "note",
            "no live hosting engine on this process (-packagehost=1 on a "
            "running node wires it); peer facts are session-scoped and "
            "never persisted, so a one-shot CLI has none to report");
        return;
    }

    struct vcs_swarm_download_status st;
    memset(&st, 0, sizeof(st));
    if (vcs_swarm_engine_download_status(engine, root, &st)) {
        struct json_value sj;
        json_init(&sj);
        json_set_object(&sj);
        zw_push_status(&sj, &st);
        (void)json_push_kv(&reply->data, "download", &sj);
        json_free(&sj);
    }

    struct vcs_swarm_peer_info infos[ZW_MAX_PEERS];
    size_t count = vcs_swarm_engine_peers_for(engine, root, infos,
                                              ZW_MAX_PEERS);
    struct json_value rows;
    json_init(&rows);
    json_set_array(&rows);
    for (size_t i = 0; i < count; i++) {
        const struct vcs_swarm_peer_info *p = &infos[i];
        char key_hex[67];
        zcl_hex_encode(p->key, 33, key_hex);
        struct json_value row;
        json_init(&row);
        json_set_object(&row);
        (void)json_push_kv_int(&row, "peer", (int64_t)p->peer);
        (void)json_push_kv_str(&row, "session_key", key_hex);
        (void)json_push_kv_str(&row, "tier", vcs_policy_tier_string(p->tier));
        (void)json_push_kv_int(&row, "inflight", (int64_t)p->inflight);
        (void)json_push_kv_int(&row, "verified_bytes_served",
                               (int64_t)p->verified_served);
        (void)json_push_kv_int(&row, "verified_bytes_received",
                               (int64_t)p->verified_from);
        (void)json_push_kv_bool(&row, "allowance_exhausted",
                                p->allowance_exhausted);
        (void)json_push_kv_int(&row, "offence_total",
                               (int64_t)p->offence_total);
        (void)json_push_back(&rows, &row);
        json_free(&row);
    }
    (void)json_push_kv(&reply->data, "peers", &rows);
    json_free(&rows);
    (void)json_push_kv_int(&reply->data, "peer_count", (int64_t)count);
    (void)json_push_kv_bool(&reply->data, "peers_truncated",
                            count == ZW_MAX_PEERS);
    (void)json_push_kv_str(
        &reply->data, "note",
        "session_key is the LOCAL transport pseudo-key (0x02 || "
        "SHA3-256(domain || host identity)) — it scopes the service book "
        "to a transport session and is NOT a contributor identity");
}

/* ── zcode package pin / unpin ──────────────────────────────────────── */

static void zw_handle_pin(const struct zcl_command_request *request,
                          struct zcl_command_reply *reply, bool pinned,
                          const char *command)
{
    if (!request || !reply)
        return;
    char zcode_dir[4400];
    if (!zw_zcode_dir(request, reply, command, zcode_dir))
        return;
    uint8_t root[32];
    if (!zw_root(request, reply, command, root))
        return;
    const char *mode = zw_input_str(request->input, "mode");
    if (!mode || (strcmp(mode, "plan") != 0 && strcmp(mode, "commit") != 0)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "INVALID_MODE",
                               "normalize", false, false,
                               "mode must be exactly plan or commit", command);
        return;
    }

    struct vcs_package_store *store = vcs_package_store_open(
        zw_datadir(request), vcs_package_store_quota_bytes());
    if (!store) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "NO_STORE",
                               "execute", false, false,
                               "the package store failed to open", zcode_dir);
        return;
    }
    struct vcs_package_store_status st;
    memset(&st, 0, sizeof(st));
    bool have_status = vcs_package_store_package_status(store, root, &st);
    if (!have_status) {
        vcs_package_store_close(store);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "UNKNOWN_PACKAGE",
                               "plan", false, true,
                               "package root is not tracked", command);
        return;
    }
    struct sha3_256_ctx sha;
    uint8_t token[32], mutation = pinned ? 1 : 0;
    sha3_256_init(&sha);
    sha3_256_write(&sha, (const uint8_t *)"zcl.package.pin.plan.v1", 24);
    sha3_256_write(&sha, root, 32);
    sha3_256_write(&sha, &mutation, 1);
    sha3_256_write(&sha, (const uint8_t *)&st, sizeof(st));
    sha3_256_finalize(&sha, token);
    enum vcs_package_store_result r = VCS_PACKAGE_STORE_OK;
    if (strcmp(mode, "commit") == 0) {
        const char *supplied_hex = zw_input_str(request->input, "plan_token");
        uint8_t supplied[32], difference = 0;
        if (!supplied_hex || strlen(supplied_hex) != 64 ||
            !zcl_hex_decode_lower(supplied_hex, supplied, 32)) {
            vcs_package_store_close(store);
            zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                                   ZCL_COMMAND_EXIT_INVALID,
                                   "INVALID_PLAN_TOKEN", "commit", false,
                                   false, "commit requires canonical plan_token",
                                   command);
            return;
        }
        for (size_t i = 0; i < 32; i++)
            difference |= supplied[i] ^ token[i];
        if (difference) {
            vcs_package_store_close(store);
            zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                                   ZCL_COMMAND_EXIT_INVALID, "STALE_PLAN",
                                   "commit", false, true,
                                   "package state changed after plan", command);
            return;
        }
        r = vcs_package_store_pin(store, root, pinned);
        memset(&st, 0, sizeof(st));
        have_status = vcs_package_store_package_status(store, root, &st);
    }
    vcs_package_store_close(store);

    if (r != VCS_PACKAGE_STORE_OK) {
        const char *code = "STORE_IO";
        if (r == VCS_PACKAGE_STORE_ERR_UNKNOWN_PACKAGE)
            code = "UNKNOWN_PACKAGE";
        else if (r == VCS_PACKAGE_STORE_ERR_QUOTA)
            code = "PINS_POOL_FULL";
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, code, "execute",
                               false, true,
                               vcs_package_store_result_string(r), command);
        return;
    }

    char hex[65];
    char token_hex[65];
    zcl_hex_encode(root, 32, hex);
    zcl_hex_encode(token, 32, token_hex);
    (void)json_push_kv_str(&reply->data, "package_root", hex);
    (void)json_push_kv_str(&reply->data, "mode", mode);
    (void)json_push_kv_str(&reply->data, "plan_token", token_hex);
    (void)json_push_kv_bool(&reply->data, "committed",
                            strcmp(mode, "commit") == 0);
    (void)json_push_kv_bool(&reply->data, "pinned",
                            strcmp(mode, "commit") == 0 ? pinned : st.pinned);
    (void)json_push_kv_str(&reply->data, "result",
                           vcs_package_store_result_string(r));
    if (have_status) {
        struct json_value sj;
        json_init(&sj);
        json_set_object(&sj);
        (void)json_push_kv_bool(&sj, "tracked", st.tracked);
        (void)json_push_kv_bool(&sj, "pinned", st.pinned);
        (void)json_push_kv_bool(&sj, "complete", st.complete);
        (void)json_push_kv_str(&sj, "pool",
                               vcs_package_store_pool_string(st.pool));
        (void)json_push_kv_int(&sj, "present_bytes",
                               (int64_t)st.present_bytes);
        (void)json_push_kv_int(&sj, "total_bytes", (int64_t)st.total_bytes);
        (void)json_push_kv(&reply->data, "package", &sj);
        json_free(&sj);
    }
    (void)json_push_kv_str(
        &reply->data, "note",
        "operator pins are never tier-gated and the PINS pool is never "
        "evicted; contributor-requested pins (pin-allowance-exceeded, "
        "vcs_policy_check_pin) are a separate, earned allowance");
}

void zcl_native_handle_zcode_package_pin(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    zw_handle_pin(request, reply, true, "zcode.package.pin");
}

void zcl_native_handle_zcode_package_unpin(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    zw_handle_pin(request, reply, false, "zcode.package.unpin");
}

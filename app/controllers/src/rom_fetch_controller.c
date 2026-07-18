/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ROM-fetch controller: the native command handlers for
 * `ops.debug.rom_fetch.*` — the operator surface of the fetch engine
 * (docs/ROM_DELIVERY.md, docs/work/wt-rom-fetch-engine.md). Each handler
 * parses its input, delegates to net/rom_fetch.h, and renders one bounded
 * JSON document into reply->data (parse -> authorize -> call one service,
 * per the controller shape). Every failure path sets a structured error
 * body via zcl_command_reply_fail.
 *
 * Trust model: the `bundle` leaf takes the expected digests as EXPLICIT
 * operator input (root/whole_sha3/size) — the operator commits to the
 * content before a single byte is requested. The engine downloads +
 * content-verifies the file and leaves it at <datadir>/<filename>; it
 * NEVER installs or activates it. Activation funnels only through the
 * unified installer's RECEIPT / CHECKPOINT_CONTENT authority
 * (-install-consensus-bundle), which the operator runs separately.
 *
 * Nested under `ops.debug` for the same reason ops.debug.rom_seed is (the
 * top-level ops menu is at its ZCL_COMMAND_BRANCH_BUDGET ceiling). */

#include "kernel/command_registry.h"
#include "command/native_command.h"
#include "controllers/diagnostics_internal.h"
#include "net/rom_fetch.h"
#include "net/file_service.h"     /* FS_PORT default */
#include "net/rom_seed.h"         /* ROM_SEED_* bounds */
#include "encoding/utilstrencodings.h"
#include "json/json.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define RFC_SUBSYS "ops.debug.rom_fetch"

/* ── ops.debug.rom_fetch.status ─────────────────────────────────────── */
void zcl_native_handle_rom_fetch_status(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    (void)request;
    struct json_value body;
    json_init(&body);
    (void)rom_fetch_dump_state_json(&body, NULL);
    json_copy(&reply->data, &body);
    json_free(&body);
}

/* ── ops.debug.rom_fetch.bundle ─────────────────────────────────────── */

static const char *rfc_input(const struct zcl_command_request *request,
                             const char *key)
{
    const char *v = json_get_str(json_get(request->input, key));
    return v ? v : "";
}

static bool rfc_parse_hex32(const char *hex, uint8_t out[32])
{
    return hex && strlen(hex) == 64 && ParseHex(hex, out, 32) == 32;
}

void zcl_native_handle_rom_fetch_bundle(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    const char *peer = rfc_input(request, "peer");
    const char *port_s = rfc_input(request, "port");
    const char *root_s = rfc_input(request, "root");
    const char *whole_s = rfc_input(request, "whole_sha3");
    const char *size_s = rfc_input(request, "size");
    const char *filename = rfc_input(request, "filename");
    const char *out_dir = rfc_input(request, "out_dir");

    if (!peer[0]) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_PEER",
                               "parse", false, false,
                               "input.peer is required (the seeder's "
                               "file-service host)", RFC_SUBSYS);
        return;
    }
    if (!out_dir[0])
        out_dir = diag_datadir();
    if (!out_dir[0]) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "NO_OUT_DIR",
                               "parse", false, false,
                               "no input.out_dir given and the node datadir "
                               "is not available to this process", RFC_SUBSYS);
        return;
    }

    uint16_t port = FS_PORT;
    if (port_s[0]) {
        char *end = NULL;
        long p = strtol(port_s, &end, 10);
        if (!end || *end != '\0' || p < 1 || p > 65535) {
            zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                                   ZCL_COMMAND_EXIT_INVALID, "BAD_PORT",
                                   "parse", false, false,
                                   "input.port must be 1..65535", RFC_SUBSYS);
            return;
        }
        port = (uint16_t)p;
    }

    struct rom_fetch_manifest m;
    memset(&m, 0, sizeof(m));
    m.used = true;
    if (!rfc_parse_hex32(root_s, m.chunk_root)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "BAD_ROOT",
                               "parse", false, false,
                               "input.root must be the 64-hex chunk_root the "
                               "operator committed to (from a trusted "
                               "directory listing or the bundle publisher)",
                               RFC_SUBSYS);
        return;
    }
    if (!rfc_parse_hex32(whole_s, m.whole_sha3)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "BAD_WHOLE_SHA3",
                               "parse", false, false,
                               "input.whole_sha3 must be the 64-hex "
                               "whole-file digest the operator committed to",
                               RFC_SUBSYS);
        return;
    }
    if (!size_s[0]) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_SIZE",
                               "parse", false, false,
                               "input.size (bytes, decimal) is required",
                               RFC_SUBSYS);
        return;
    }
    {
        char *end = NULL;
        unsigned long long sz = strtoull(size_s, &end, 10);
        if (!end || *end != '\0' || sz == 0) {
            zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                                   ZCL_COMMAND_EXIT_INVALID, "BAD_SIZE",
                                   "parse", false, false,
                                   "input.size must be a positive decimal "
                                   "byte count", RFC_SUBSYS);
            return;
        }
        m.size_bytes = (uint64_t)sz;
    }
    m.chunk_size = ROM_SEED_CHUNK_SIZE;
    m.num_chunks =
        (uint32_t)((m.size_bytes + m.chunk_size - 1) / m.chunk_size);
    if (filename[0]) {
        if (strlen(filename) >= sizeof(m.filename)) {
            zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                                   ZCL_COMMAND_EXIT_INVALID, "BAD_FILENAME",
                                   "parse", false, false,
                                   "input.filename is too long", RFC_SUBSYS);
            return;
        }
        snprintf(m.filename, sizeof(m.filename), "%s", filename);
    }
    if (!rom_fetch_manifest_sane(&m)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID,
                               "MANIFEST_NOT_SANE", "parse", false, false,
                               "the committed (size, chunking, filename) "
                               "tuple fails the ROM artifact bounds — check "
                               "size/filename against the publisher's "
                               "manifest", RFC_SUBSYS);
        return;
    }

    /* Synchronous download + whole-file content proof. Blocks this command
     * worker for the duration (minutes for a full bundle over a slow peer);
     * a background/scheduling engine is the follow-up lane.
     * `peer` may be a comma-separated host list (all sharing `port`): a
     * single peer uses the sequential driver, several peers use the
     * parallel multi-seeder scheduler (chunks round-robin across peers,
     * per-peer retry before a chunk is declared failed). */
    bool fetched = false;
    if (strchr(peer, ',')) {
        struct rom_fetch_peer peers[4];
        size_t npeers = 0;
        char list[512];
        snprintf(list, sizeof(list), "%s", peer);
        for (char *tok = strtok(list, ","); tok && npeers < 4;
             tok = strtok(NULL, ",")) {
            while (*tok == ' ')
                tok++;
            size_t tl = strlen(tok);
            while (tl > 0 && tok[tl - 1] == ' ')
                tok[--tl] = '\0';
            if (tl == 0 || tl >= sizeof(peers[0].addr))
                continue;
            snprintf(peers[npeers].addr, sizeof(peers[npeers].addr),
                     "%s", tok);
            peers[npeers].port = port;
            npeers++;
        }
        if (npeers == 0) {
            zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                                   ZCL_COMMAND_EXIT_INVALID, "BAD_PEER_LIST",
                                   "parse", false, false,
                                   "input.peer held a comma but no usable "
                                   "host", RFC_SUBSYS);
            return;
        }
        uint32_t workers = (uint32_t)(2 * npeers);
        if (workers > ROM_FETCH_MAX_WORKERS)
            workers = ROM_FETCH_MAX_WORKERS;
        fetched = rom_fetch_download_parallel(peers, npeers, &m, out_dir,
                                              workers, NULL, NULL);
    } else {
        fetched = rom_fetch_download(peer, port, &m, out_dir, NULL, NULL);
    }
    if (!fetched) {
        struct rom_fetch_status st;
        rom_fetch_status_snapshot(&st);
        char msg[256];
        snprintf(msg, sizeof(msg),
                 "fetch from %s:%u failed: %s", peer, (unsigned)port,
                 st.detail[0] ? st.detail : "see node log");
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_FAILED, "ROM_FETCH_FAILED",
                               "execute", true, false, msg, RFC_SUBSYS);
        (void)zcl_command_reply_add_next(reply, "ops.debug.rom_fetch.status",
                                         "{}", "inspect the fetch status");
        return;
    }

    /* The download derives a name from the digest when none was given; the
     * status snapshot holds the post-derivation name. */
    struct rom_fetch_status st;
    rom_fetch_status_snapshot(&st);
    (void)json_push_kv_str(&reply->data, "installed", st.filename);
    char path[1200];
    snprintf(path, sizeof(path), "%s/%s", out_dir, st.filename);
    (void)json_push_kv_str(&reply->data, "path", path);
    (void)json_push_kv_int(&reply->data, "size_bytes",
                           (int64_t)m.size_bytes);
    (void)json_push_kv_int(&reply->data, "num_chunks",
                           (int64_t)m.num_chunks);
    (void)json_push_kv_bool(&reply->data, "verified", true);
    char root_hex[65];
    HexStr(m.chunk_root, 32, false, root_hex, sizeof(root_hex));
    (void)json_push_kv_str(&reply->data, "chunk_root", root_hex);
    (void)json_push_kv_str(&reply->data, "next",
                           "reboot with -install-consensus-bundle=<path> to "
                           "activate through the unified installer");
}

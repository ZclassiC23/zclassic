/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Native handlers for the `zcode release` tree — Sovereign Registry v1:
 * sign and verify zid release records (docs/spec/sovereign-identity-
 * layer.md). A release record is the canonical zid_doc body ("ZIDR" ‖
 * name ‖ version ‖ manifest_root) binding a package release to an
 * identity's master ed25519 key (lib/zid). v1 is file-based only: no DB,
 * no swarm distribution — sign writes <datadir>/zcode/releases/
 * <name>-<version>.zid, verify reads a doc from hex or file.
 *
 * Secret hygiene (sign): the seed file must be exactly 64 hex chars and
 * 0600/0400 perms (refused otherwise, with the chmod hint); the seed
 * buffer is memory_cleanse'd after use and is NEVER logged or echoed. */

#include "command/native_command.h"

#include "base/log_macros.h"
#include "encoding/utilstrencodings.h"
#include "json/json.h"
#include "platform/time_compat.h"
#include "support/cleanse.h"
#include "zid/zid.h"

#include <errno.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

/* ── small input helpers (native_zcode_command.c shape) ────────────── */

static const char *zr_input_str(const struct json_value *input,
                                const char *key)
{
    const struct json_value *v = json_get(input, key);
    return v ? json_get_str(v) : NULL;
}

static const char *zr_datadir(const struct zcl_command_request *request)
{
    const char *dd = zr_input_str(request->input, "datadir");
    if (dd && dd[0])
        return dd;
    dd = zcl_native_command_datadir();
    return (dd && dd[0]) ? dd : NULL;
}

/* ── seed loading ──────────────────────────────────────────────────── */

/* Read exactly 32 seed bytes from a 64-hex-char file. Accepts one
 * trailing newline (echo > file). Refuses any other size and any perm
 * bits beyond 0600/0400. The 64-byte hex buffer is cleansed before
 * return (caller cleanses `seed_out`). Never logs content. */
static bool zr_read_seed(const char *path, uint8_t seed_out[32],
                         char *err, size_t err_size)
{
    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0) {
        snprintf(err, err_size, "cannot open seed file: %s", strerror(errno));
        return false;
    }
    struct stat st;
    if (fstat(fd, &st) != 0) {
        snprintf(err, err_size, "cannot stat seed file: %s", strerror(errno));
        close(fd);
        return false;
    }
    unsigned perms = (unsigned)(st.st_mode & 0777u);
    if (perms != 0600u && perms != 0400u) {
        snprintf(err, err_size,
                 "seed file perms are %03o — a master seed must be 0600 "
                 "(chmod 600 %s)", perms, path);
        close(fd);
        return false;
    }
    uint8_t raw[65]; /* 64 hex + optional one trailing newline */
    ssize_t n = read(fd, raw, sizeof(raw));
    close(fd);
    if (n < 64) {
        snprintf(err, err_size,
                 "seed file must be exactly 64 hex chars (read %zd bytes)", n);
        return false;
    }
    if (n != 64 && !(n == 65 && raw[64] == '\n')) {
        snprintf(err, err_size,
                 "seed file must be exactly 64 hex chars (read %zd bytes)", n);
        memory_cleanse(raw, sizeof(raw));
        return false;
    }
    char hex[65];
    memcpy(hex, raw, 64);
    hex[64] = '\0';
    memory_cleanse(raw, sizeof(raw));
    bool ok = IsHex(hex) && ParseHex(hex, seed_out, 32) == 32;
    memory_cleanse(hex, sizeof(hex));
    if (!ok)
        snprintf(err, err_size, "seed file is not 64 hex chars");
    return ok;
}

/* ── doc file I/O ──────────────────────────────────────────────────── */

static bool zr_write_doc_file(const char *datadir, const char *name,
                              const char *version, const char *doc_hex,
                              char *path_out, size_t path_size,
                              char *err, size_t err_size)
{
    char dir[1024];
    int n = snprintf(dir, sizeof(dir), "%s/zcode", datadir);
    if (n <= 0 || (size_t)n >= sizeof(dir))
        goto too_long;
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        snprintf(err, err_size, "mkdir %s: %s", dir, strerror(errno));
        return false;
    }
    n = snprintf(dir, sizeof(dir), "%s/zcode/releases", datadir);
    if (n <= 0 || (size_t)n >= sizeof(dir))
        goto too_long;
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) {
        snprintf(err, err_size, "mkdir %s: %s", dir, strerror(errno));
        return false;
    }
    n = snprintf(path_out, path_size, "%s/%s-%s.zid", dir, name, version);
    if (n <= 0 || (size_t)n >= path_size)
        goto too_long;
    int fd = open(path_out, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) {
        snprintf(err, err_size, "open %s: %s", path_out, strerror(errno));
        return false;
    }
    size_t len = strlen(doc_hex);
    ssize_t w = write(fd, doc_hex, len);
    ssize_t w2 = (w == (ssize_t)len) ? write(fd, "\n", 1) : -1;
    close(fd);
    if (w != (ssize_t)len || w2 != 1) {
        snprintf(err, err_size, "write %s: %s", path_out, strerror(errno));
        return false;
    }
    return true;
too_long:
    snprintf(err, err_size, "path too long under datadir");
    return false;
}

/* ── zcode.release.sign ────────────────────────────────────────────── */

void zcl_native_handle_zcode_release_sign(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    const struct json_value *in = request->input;

    const char *name = zr_input_str(in, "name");
    if (!name || !name[0]) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_NAME",
                               "normalize", false, false,
                               "missing name — the package name this release "
                               "binds (1..64 printable ASCII)",
                               "zcode.release.sign");
        return;
    }
    const char *version = zr_input_str(in, "version");
    if (!version || !version[0]) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_VERSION",
                               "normalize", false, false,
                               "missing version — e.g. \"1.0.0\" (1..32 "
                               "printable ASCII)",
                               "zcode.release.sign");
        return;
    }
    const char *root_hex = zr_input_str(in, "root");
    if (!root_hex || strlen(root_hex) != 64 || !IsHex(root_hex)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "BAD_ROOT",
                               "normalize", false, false,
                               "root must be the 64-hex manifest root — get "
                               "it from `zcode package publish plan`",
                               "zcode.release.sign");
        return;
    }
    const char *seed_file = zr_input_str(in, "seed_file");
    if (!seed_file || !seed_file[0]) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_SEED_FILE",
                               "normalize", false, false,
                               "missing seed_file — path to a 0600 file "
                               "holding the 64-hex master seed",
                               "zcode.release.sign");
        return;
    }

    int64_t seq = json_get_int_or(in, "seq", 1);
    if (seq < 0) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "BAD_SEQ",
                               "normalize", false, false,
                               "seq must be >= 0 (monotonic per identity — a "
                               "newer release must use a strictly higher seq)",
                               "zcode.release.sign");
        return;
    }
    int64_t expiry = json_get_int_or(in, "expiry", 0);
    if (expiry == 0)
        expiry = platform_time_wall_unix() + 365 * 86400;

    struct zid_release rel;
    memset(&rel, 0, sizeof(rel));
    snprintf(rel.name, sizeof(rel.name), "%s", name);
    snprintf(rel.version, sizeof(rel.version), "%s", version);
    if (ParseHex(root_hex, rel.manifest_root, 32) != 32) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "BAD_ROOT",
                               "normalize", false, false,
                               "root is not decodable 64-hex",
                               "zcode.release.sign");
        return;
    }
    if (strlen(name) > ZID_RELEASE_NAME_MAX ||
        strlen(version) > ZID_RELEASE_VERSION_MAX) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "FIELD_TOO_LONG",
                               "normalize", false, false,
                               "name exceeds 64 or version exceeds 32 chars",
                               "zcode.release.sign");
        return;
    }

    uint8_t seed[32];
    char err[512];
    if (!zr_read_seed(seed_file, seed, err, sizeof(err))) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "BAD_SEED_FILE",
                               "normalize", false, false, err, seed_file);
        return;
    }

    struct zid_doc doc;
    bool signed_ok = zid_release_sign(&doc, &rel, (uint64_t)seq,
                                      (uint64_t)expiry, seed);
    memory_cleanse(seed, sizeof(seed));
    if (!signed_ok) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "SIGN_FAILED",
                               "execute", false, false,
                               "zid_release_sign failed (name/version not "
                               "printable ASCII?)", name);
        return;
    }

    uint8_t wire[ZID_DOC_MAX];
    size_t wire_len = zid_doc_encode(wire, sizeof(wire), &doc);
    if (wire_len == 0) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INTERNAL, "ENCODE_FAILED",
                               "serialize", false, false,
                               "zid_doc_encode failed", name);
        return;
    }
    char doc_hex[ZID_DOC_MAX * 2 + 1];
    HexStr(wire, wire_len, false, doc_hex, sizeof(doc_hex));

    char pk_hex[65];
    HexStr(doc.master_pubkey, 32, false, pk_hex, sizeof(pk_hex));

    /* Persist under <datadir>/zcode/releases/ (best-effort but reported:
     * a sign that cannot persist still returns the doc hex, with the
     * save error named). */
    char saved_path[1200];
    bool saved = false;
    char save_err[512] = {0};
    const char *datadir = zr_datadir(request);
    if (datadir)
        saved = zr_write_doc_file(datadir, rel.name, rel.version, doc_hex,
                                  saved_path, sizeof(saved_path),
                                  save_err, sizeof(save_err));

    json_push_kv_str(&reply->data, "doc_hex", doc_hex);
    json_push_kv_str(&reply->data, "master_pubkey", pk_hex);
    json_push_kv_str(&reply->data, "name", rel.name);
    json_push_kv_str(&reply->data, "version", rel.version);
    char root_out[65];
    HexStr(rel.manifest_root, 32, false, root_out, sizeof(root_out));
    json_push_kv_str(&reply->data, "manifest_root", root_out);
    json_push_kv_int(&reply->data, "seq", seq);
    json_push_kv_int(&reply->data, "expiry", expiry);
    json_push_kv_int(&reply->data, "doc_bytes", (int64_t)wire_len);
    json_push_kv_bool(&reply->data, "saved", saved);
    if (saved)
        json_push_kv_str(&reply->data, "saved_path", saved_path);
    else if (datadir)
        json_push_kv_str(&reply->data, "save_error", save_err);
    else
        json_push_kv_str(&reply->data, "save_error",
                         "no datadir resolved — pass --datadir to persist "
                         "the doc under <datadir>/zcode/releases/");
    json_push_kv_str(&reply->data, "next",
                     "distribute doc_hex to verifiers; anyone can check it "
                     "with `zclassic23 zcode release verify --doc=<hex>`");
    reply->status = ZCL_COMMAND_STATUS_PASSED;
    reply->exit_code = ZCL_COMMAND_EXIT_OK;
}

/* ── zcode.release.verify ──────────────────────────────────────────── */

void zcl_native_handle_zcode_release_verify(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;
    const struct json_value *in = request->input;

    const char *doc_hex = zr_input_str(in, "doc");
    const char *file = zr_input_str(in, "file");
    if ((!doc_hex || !doc_hex[0]) && (!file || !file[0])) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "MISSING_DOC",
                               "normalize", false, false,
                               "give --doc=<hex> (from zcode release sign) "
                               "or --file=<path to a saved .zid>",
                               "zcode.release.verify");
        return;
    }

    char file_hex[ZID_DOC_MAX * 2 + 2];
    if (!doc_hex || !doc_hex[0]) {
        int fd = open(file, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                                   ZCL_COMMAND_EXIT_INVALID, "DOC_UNREADABLE",
                                   "normalize", false, false,
                                   "cannot open doc file", strerror(errno));
            return;
        }
        ssize_t n = read(fd, file_hex, sizeof(file_hex) - 1);
        close(fd);
        if (n <= 0) {
            zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                                   ZCL_COMMAND_EXIT_INVALID, "DOC_UNREADABLE",
                                   "normalize", false, false,
                                   "doc file empty or unreadable", file);
            return;
        }
        while (n > 0 && (file_hex[n - 1] == '\n' || file_hex[n - 1] == '\r' ||
                         file_hex[n - 1] == ' '))
            n--;
        file_hex[n] = '\0';
        doc_hex = file_hex;
    }

    size_t hex_len = strlen(doc_hex);
    if (hex_len == 0 || (hex_len & 1u) != 0 || hex_len > ZID_DOC_MAX * 2 ||
        !IsHex(doc_hex)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "BAD_DOC_HEX",
                               "normalize", false, false,
                               "doc must be even-length hex, at most "
                               "2*ZID_DOC_MAX chars — pass the exact "
                               "doc_hex from zcode release sign",
                               "zcode.release.verify");
        return;
    }
    uint8_t wire[ZID_DOC_MAX];
    size_t wire_len = (size_t)ParseHex(doc_hex, wire, sizeof(wire));
    if (wire_len == 0) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "BAD_DOC_HEX",
                               "normalize", false, false,
                               "doc hex did not decode", "zcode.release.verify");
        return;
    }

    struct zid_doc doc;
    if (!zid_doc_decode(&doc, wire, wire_len)) {
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "DOC_DECODE_FAILED",
                               "execute", false, false,
                               "not a well-formed zid doc (version/wire "
                               "layout) — check the hex was not truncated",
                               "zcode.release.verify");
        return;
    }

    int64_t now = platform_time_wall_unix();
    struct zid_release rel;
    memset(&rel, 0, sizeof(rel));
    bool valid = zid_release_verify(&doc, &rel, (uint64_t)now);
    bool release_shape = true;
    if (!valid)
        /* Re-decode for display (verify left rel unfilled on failure). */
        release_shape = zid_release_decode_body(&rel, doc.body,
                                                doc.body_len);

    /* Always show the decoded fields so the caller can see WHAT failed;
     * an invalid doc is a hard failure with the reason named, never a
     * silent valid:false. */
    json_push_kv_str(&reply->data, "name", rel.name);
    json_push_kv_str(&reply->data, "version", rel.version);
    char hex[65];
    HexStr(rel.manifest_root, 32, false, hex, sizeof(hex));
    json_push_kv_str(&reply->data, "manifest_root", hex);
    HexStr(doc.master_pubkey, 32, false, hex, sizeof(hex));
    json_push_kv_str(&reply->data, "master_pubkey", hex);
    json_push_kv_int(&reply->data, "seq", (int64_t)doc.seq);
    json_push_kv_int(&reply->data, "expiry", (int64_t)doc.expiry);
    json_push_kv_int(&reply->data, "verified_at", now);
    json_push_kv_bool(&reply->data, "valid", valid);

    if (!valid) {
        if (!release_shape)
            zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                                   ZCL_COMMAND_EXIT_FAILED,
                                   "NOT_A_RELEASE_BODY", "execute", false,
                                   false,
                                   "signature/body is not a ZIDR release "
                                   "record — this doc was signed for "
                                   "something else", "zcode.release.verify");
        else if ((uint64_t)now >= doc.expiry)
            zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                                   ZCL_COMMAND_EXIT_FAILED,
                                   "DOC_EXPIRED", "execute", false, false,
                                   "release doc is expired — ask the "
                                   "publisher for a re-signed doc with a "
                                   "higher seq and later expiry",
                                   "zcode.release.verify");
        else
            zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                                   ZCL_COMMAND_EXIT_FAILED,
                                   "BAD_SIGNATURE", "execute", false, false,
                                   "ed25519 signature does not verify "
                                   "against the doc's master_pubkey — the "
                                   "doc was tampered with or corrupted",
                                   "zcode.release.verify");
        return;
    }
    reply->status = ZCL_COMMAND_STATUS_PASSED;
    reply->exit_code = ZCL_COMMAND_EXIT_OK;
}

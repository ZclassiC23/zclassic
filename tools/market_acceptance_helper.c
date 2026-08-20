/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * C23-only data helper for tools/dev/market_acceptance.sh.  The shell owns
 * process isolation and cleanup; this program owns JSON interpretation,
 * deterministic fixture bytes, SHA3 manifest roots, and typed assertions.
 * It never opens a wallet, node datadir, network socket, or private key.
 */

#include "json/json.h"
#include "sha3/sha3.h"
#include "base/safe_alloc.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define MAH_INPUT_MAX (4u * 1024u * 1024u)
#define MAH_CHUNK_BYTES (50u * 1024u * 1024u)
#define MAH_BLOCK_BYTES 65536u
#define MAH_MANIFEST_CHUNKS_MAX 4096u

static int mah_fail(const char *message)
{
    fprintf(stderr, "market-acceptance-helper: %s\n", message);
    return 1;
}

static bool mah_parse_u64(const char *text, uint64_t *out)
{
    if (!text || !text[0] || text[0] == '-') return false;
    errno = 0;
    char *end = NULL;
    uintmax_t value = strtoumax(text, &end, 10);
    if (errno || !end || end == text || end[0] || value > UINT64_MAX)
        return false;
    *out = (uint64_t)value;
    return true;
}

static bool mah_read_stdin(char **raw_out, size_t *bytes_out)
{
    char *raw = zcl_malloc(MAH_INPUT_MAX + 1u, "market_helper.stdin");
    if (!raw) return false;
    size_t used = 0;
    while (used < MAH_INPUT_MAX) {
        ssize_t got = read(STDIN_FILENO, raw + used, MAH_INPUT_MAX - used);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) { free(raw); return false; }
        if (got == 0) break;
        used += (size_t)got;
    }
    if (used == MAH_INPUT_MAX) {
        char extra;
        ssize_t got;
        do { got = read(STDIN_FILENO, &extra, 1); } while (got < 0 && errno == EINTR);
        if (got != 0) { free(raw); return false; }
    }
    raw[used] = '\0';
    *raw_out = raw;
    *bytes_out = used;
    return true;
}

static bool mah_read_json(struct json_value *doc)
{
    char *raw = NULL;
    size_t bytes = 0;
    if (!mah_read_stdin(&raw, &bytes)) return false;
    bool ok = json_read(doc, raw, bytes);
    free(raw);
    return ok;
}

static const struct json_value *mah_member(const struct json_value *object,
                                           const char *key)
{
    return object && object->type == JSON_OBJ ? json_get(object, key) : NULL;
}

static const char *mah_string(const struct json_value *object, const char *key)
{
    const struct json_value *value = mah_member(object, key);
    return value && value->type == JSON_STR ? json_get_str(value) : NULL;
}

static bool mah_streq(const struct json_value *object, const char *key,
                      const char *expected)
{
    const char *value = mah_string(object, key);
    return value && strcmp(value, expected) == 0;
}

static bool mah_bool(const struct json_value *object, const char *key,
                     bool expected)
{
    const struct json_value *value = mah_member(object, key);
    return value && value->type == JSON_BOOL &&
        json_get_bool(value) == expected;
}

static bool mah_int(const struct json_value *object, const char *key,
                    int64_t *out)
{
    const struct json_value *value = mah_member(object, key);
    if (!value || value->type != JSON_INT) return false;
    *out = json_get_int(value);
    return true;
}

static bool mah_hex64(const char *text)
{
    if (!text || strlen(text) != 64u) return false;
    bool nonzero = false;
    for (size_t i = 0; i < 64u; i++) {
        char c = text[i];
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f')))
            return false;
        nonzero = nonzero || c != '0';
    }
    return nonzero;
}

static const struct json_value *mah_data(const struct json_value *doc)
{
    return doc && doc->type == JSON_OBJ && mah_bool(doc, "ok", true)
        ? mah_member(doc, "data") : NULL;
}

static void mah_digest_hex(const uint8_t digest[32], char out[65])
{
    static const char hex[] = "0123456789abcdef";
    for (size_t i = 0; i < 32u; i++) {
        out[i * 2u] = hex[digest[i] >> 4u];
        out[i * 2u + 1u] = hex[digest[i] & 15u];
    }
    out[64] = '\0';
}

static bool mah_write_all(int fd, const uint8_t *bytes, size_t size)
{
    size_t written = 0;
    while (written < size) {
        ssize_t rc = write(fd, bytes + written, size - written);
        if (rc < 0 && errno == EINTR) continue;
        if (rc <= 0) return false;
        written += (size_t)rc;
    }
    return true;
}

static bool mah_fixture_create(const char *path, uint64_t price,
                               uint64_t tail, uint64_t *size_out,
                               char root_out[65], uint64_t *total_out)
{
    if (tail == 0 || tail > MAH_CHUNK_BYTES || price == 0) return false;
    uint64_t size = 2u * (uint64_t)MAH_CHUNK_BYTES + tail;
    uint64_t mb = 1024u * 1024u;
    uint64_t whole = size / mb, rem = size % mb;
    if (price > UINT64_MAX / (whole + 2u)) return false;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC | O_NOFOLLOW,
                  0600);
    if (fd < 0) return false;
    uint8_t *block = zcl_malloc(MAH_BLOCK_BYTES, "market_helper.fixture");
    uint8_t digests[3][32];
    bool ok = block != NULL;
    for (uint32_t index = 0; ok && index < 2u; index++) {
        for (size_t i = 0; i < MAH_BLOCK_BYTES; i++)
            block[i] = (uint8_t)((index * 131u + i * 7u) & 0xffu);
        struct sha3_256_ctx sha;
        sha3_256_init(&sha);
        for (size_t offset = 0; ok && offset < MAH_CHUNK_BYTES;
             offset += MAH_BLOCK_BYTES) {
            ok = mah_write_all(fd, block, MAH_BLOCK_BYTES);
            if (ok) sha3_256_write(&sha, block, MAH_BLOCK_BYTES);
        }
        if (ok) sha3_256_finalize(&sha, digests[index]);
    }
    if (ok) {
        struct sha3_256_ctx sha;
        sha3_256_init(&sha);
        uint64_t offset = 0;
        while (ok && offset < tail) {
            size_t take = tail - offset > MAH_BLOCK_BYTES
                ? MAH_BLOCK_BYTES : (size_t)(tail - offset);
            for (size_t i = 0; i < take; i++)
                block[i] = (uint8_t)((5u + (offset + i) * 11u) & 0xffu);
            ok = mah_write_all(fd, block, take);
            if (ok) sha3_256_write(&sha, block, take);
            offset += take;
        }
        if (ok) sha3_256_finalize(&sha, digests[2]);
    }
    if (ok && fsync(fd) != 0) ok = false;
    if (close(fd) != 0) ok = false;
    free(block);
    if (!ok) { (void)unlink(path); return false; }

    uint8_t manifest[32];
    sha3_256((const uint8_t *)digests, sizeof(digests), manifest);
    mah_digest_hex(manifest, root_out);
    uint64_t price_whole = price / mb, price_rem = price % mb;
    *size_out = size;
    *total_out = whole * price + rem * price_whole +
        (rem * price_rem + mb - 1u) / mb;
    return true;
}

static bool mah_file_manifest_root(const char *path, char root_out[65])
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0) return false;
    uint8_t *buffer = zcl_malloc(MAH_BLOCK_BYTES, "market_helper.file_buffer");
    uint8_t *digests = zcl_calloc(MAH_MANIFEST_CHUNKS_MAX, 32u,
                                  "market_helper.manifest");
    bool ok = buffer && digests;
    size_t chunks = 0, in_chunk = 0;
    struct sha3_256_ctx sha;
    sha3_256_init(&sha);
    while (ok) {
        size_t remaining = MAH_CHUNK_BYTES - in_chunk;
        size_t want = remaining < MAH_BLOCK_BYTES ? remaining : MAH_BLOCK_BYTES;
        ssize_t got = read(fd, buffer, want);
        if (got < 0 && errno == EINTR) continue;
        if (got < 0) { ok = false; break; }
        if (got == 0) break;
        sha3_256_write(&sha, buffer, (size_t)got);
        in_chunk += (size_t)got;
        if (in_chunk == MAH_CHUNK_BYTES) {
            if (chunks == MAH_MANIFEST_CHUNKS_MAX) { ok = false; break; }
            sha3_256_finalize(&sha, digests + chunks * 32u);
            chunks++;
            in_chunk = 0;
            sha3_256_init(&sha);
        }
    }
    if (ok && in_chunk > 0) {
        if (chunks == MAH_MANIFEST_CHUNKS_MAX) ok = false;
        else {
            sha3_256_finalize(&sha, digests + chunks * 32u);
            chunks++;
        }
    }
    if (close(fd) != 0) ok = false;
    if (ok && chunks == 0) ok = false;
    if (ok) {
        uint8_t root[32];
        sha3_256(digests, chunks * 32u, root);
        mah_digest_hex(root, root_out);
    }
    free(digests);
    free(buffer);
    return ok;
}

static const struct json_value *mah_path(const struct json_value *root,
                                         const char *path)
{
    char copy[256];
    if (!path || strlen(path) >= sizeof(copy)) return NULL;
    (void)snprintf(copy, sizeof(copy), "%s", path);
    const struct json_value *current = root;
    char *save = NULL;
    for (char *part = strtok_r(copy, ".", &save); part;
         part = strtok_r(NULL, ".", &save)) {
        if (current->type == JSON_OBJ) current = json_get(current, part);
        else if (current->type == JSON_ARR) {
            uint64_t index = 0;
            if (!mah_parse_u64(part, &index) || index > SIZE_MAX) return NULL;
            current = json_at(current, (size_t)index);
        } else return NULL;
        if (!current) return NULL;
    }
    return current;
}

static bool mah_print_value(const struct json_value *value)
{
    if (!value) return false;
    switch (value->type) {
    case JSON_STR: printf("%s\n", json_get_str(value)); return true;
    case JSON_BOOL: printf("%s\n", json_get_bool(value) ? "True" : "False"); return true;
    case JSON_INT: printf("%" PRId64 "\n", json_get_int(value)); return true;
    case JSON_REAL: printf("%.17g\n", json_get_real(value)); return true;
    case JSON_NULL: printf("null\n"); return true;
    case JSON_ARR:
    case JSON_OBJ: {
        size_t bytes = json_write(value, NULL, 0);
        char *wire = zcl_malloc(bytes + 1u, "market_helper.json_wire");
        if (!wire) return false;
        bool ok = json_write(value, wire, bytes + 1u) == bytes;
        if (ok) printf("%s\n", wire);
        free(wire);
        return ok;
    }
    }
    return false;
}

static bool mah_expect_offer_plan(const struct json_value *doc,
                                  const char *root, int64_t size,
                                  int64_t chunks, int64_t total)
{
    const struct json_value *data = mah_data(doc);
    int64_t got_size = 0, got_chunks = 0, got_total = 0, price = 0;
    return data && mah_streq(data, "stage", "plan") &&
        mah_bool(data, "committed", false) &&
        mah_bool(data, "spends_funds", false) &&
        mah_streq(data, "root_hash", root) &&
        mah_int(data, "size_bytes", &got_size) && got_size == size &&
        mah_int(data, "num_chunks", &got_chunks) && got_chunks == chunks &&
        mah_int(data, "total_zat", &got_total) && got_total == total &&
        mah_int(data, "price_per_mb_zat", &price) && price > 0 &&
        mah_member(data, "commit_input") && !mah_member(data, "offer_id") &&
        !mah_member(data, "seller_pubkey");
}

static const char *mah_expect_offer_commit(const struct json_value *doc,
                                           const char *root)
{
    const struct json_value *data = mah_data(doc);
    const char *offer = mah_string(data, "offer_id");
    const char *seller = mah_string(data, "seller_pubkey");
    return data && mah_streq(data, "stage", "committed") &&
        mah_bool(data, "committed", true) &&
        mah_bool(data, "idempotent_replay", false) &&
        mah_bool(data, "announced", true) &&
        mah_streq(data, "root_hash", root) &&
        mah_hex64(offer) && seller && strlen(seller) == 64u ? offer : NULL;
}

static bool mah_expect_buyer_entry(const struct json_value *doc,
                                   const char *offer, const char *root,
                                   int64_t price, int64_t chunks, int64_t total)
{
    const struct json_value *view = mah_data(doc);
    int64_t offer_count = -1, hidden_count = -1;
    if (!view || !mah_streq(view, "profile", "open-view") ||
        !mah_bool(view, "profile_override", true) ||
        !mah_int(view, "offer_count", &offer_count) || offer_count < 1 ||
        !mah_int(view, "hidden_count", &hidden_count) || hidden_count != 0)
        return false;
    const struct json_value *rows = mah_member(view, "offers");
    if (!rows || rows->type != JSON_ARR ||
        (uint64_t)offer_count != rows->num_children)
        return false;
    const struct json_value *match = NULL;
    size_t count = 0;
    for (size_t i = 0; i < rows->num_children; i++) {
        const struct json_value *row = json_at(rows, i);
        const char *id = mah_string(row, "offer_id");
        if (id && strcmp(id, offer) == 0) { match = row; count++; }
    }
    int64_t got_price = 0, got_chunks = 0, got_total = 0, peer_port = 0;
    return count == 1 && match && mah_streq(match, "root_hash", root) &&
        mah_int(match, "price_per_mb_zat", &got_price) && got_price == price &&
        mah_int(match, "num_chunks", &got_chunks) && got_chunks == chunks &&
        mah_int(match, "total_cost_zat", &got_total) && got_total == total &&
        mah_bool(match, "authenticated", true) &&
        mah_int(match, "peer_port", &peer_port) && peer_port > 0;
}

static bool mah_expect_market_hidden(const struct json_value *doc,
                                     const char *offer)
{
    const struct json_value *view = mah_data(doc);
    int64_t offer_count = -1, hidden_count = -1;
    if (!view || !mah_streq(view, "profile", "general-audience.v1") ||
        !mah_bool(view, "profile_override", false) ||
        !mah_int(view, "offer_count", &offer_count) || offer_count < 0 ||
        !mah_int(view, "hidden_count", &hidden_count) || hidden_count < 1)
        return false;
    const struct json_value *rows = mah_member(view, "offers");
    if (!rows || rows->type != JSON_ARR ||
        (uint64_t)offer_count != rows->num_children)
        return false;
    for (size_t i = 0; i < rows->num_children; i++) {
        const struct json_value *row = json_at(rows, i);
        const char *id = mah_string(row, "offer_id");
        if (id && strcmp(id, offer) == 0) return false;
    }
    return true;
}

static bool mah_expect_market_empty(const struct json_value *doc)
{
    int64_t offers = -1, hidden = -1;
    const struct json_value *rows = mah_member(doc, "offers");
    return doc && doc->type == JSON_OBJ &&
        mah_string(doc, "profile") &&
        rows && rows->type == JSON_ARR && rows->num_children == 0u &&
        mah_int(doc, "offer_count", &offers) && offers == 0 &&
        mah_int(doc, "hidden_count", &hidden) && hidden == 0;
}

static const char *mah_expect_purchase_plan(const struct json_value *doc,
                                            const char *offer, int64_t total)
{
    const struct json_value *data = mah_data(doc);
    int64_t amount = 0, fee = 0, reserved = 0, start = -1, paid = 0;
    const char *plan = mah_string(data, "plan_id");
    return data && mah_streq(data, "stage", "plan") &&
        mah_bool(data, "committed", false) &&
        mah_bool(data, "spends_funds", false) &&
        mah_streq(data, "offer_id", offer) &&
        mah_int(data, "amount_zat", &amount) && amount == total &&
        mah_int(data, "maximum_fee_zat", &fee) && fee > 0 &&
        mah_int(data, "reserved_zat", &reserved) && reserved == amount + fee &&
        mah_int(data, "chunk_start", &start) && start == 0 &&
        mah_int(data, "chunks_paid", &paid) && paid > 0 &&
        mah_streq(data, "state", "planned") &&
        mah_bool(data, "idempotent_replay", false) &&
        mah_member(data, "commit_input") && mah_hex64(plan) ? plan : NULL;
}

static const char *mah_expect_purchase_commit(const struct json_value *doc)
{
    const struct json_value *data = mah_data(doc);
    const char *txid = mah_string(data, "txid");
    return data && mah_streq(data, "stage", "committed") &&
        mah_bool(data, "committed", true) &&
        mah_bool(data, "spends_funds", true) &&
        mah_bool(data, "idempotent_replay", false) &&
        mah_bool(data, "payment_notification_queued", true) &&
        mah_streq(data, "state", "mempool_accepted") &&
        mah_hex64(txid) && mah_hex64(mah_string(data, "claim_id")) ? txid : NULL;
}

static bool mah_expect_early_refusal(const struct json_value *doc)
{
    const struct json_value *error = mah_member(doc, "error");
    const char *code = mah_string(error, "code");
    const char *message = mah_string(error, "message");
    return doc && doc->type == JSON_OBJ && mah_bool(doc, "ok", false) &&
        code && strcmp(code, "DELIVERY_NOT_READY") == 0 && message &&
        (strstr(message, "PENDING") || strstr(message, "UNKNOWN"));
}

static bool mah_expect_purchase_status(const struct json_value *doc,
                                       const char *txid)
{
    const struct json_value *data = mah_data(doc);
    return data && mah_streq(data, "state", "confirmed") &&
        mah_streq(data, "txid", txid) &&
        mah_hex64(mah_string(data, "claim_id"));
}

static bool mah_expect_retrieve(const struct json_value *doc,
                                int64_t size, int64_t chunks)
{
    const struct json_value *data = mah_data(doc);
    int64_t got = 0, total = 0, bytes = 0, size_bytes = 0;
    return data && mah_streq(data, "stage", "retrieved") &&
        mah_streq(data, "download_state", "complete") &&
        mah_bool(data, "destination_published", true) &&
        mah_int(data, "chunks_received", &got) && got == chunks &&
        mah_int(data, "num_chunks", &total) && total == chunks &&
        mah_int(data, "bytes_received", &bytes) && bytes == size &&
        mah_int(data, "size_bytes", &size_bytes) && size_bytes == size;
}

static bool mah_expect_claim(const struct json_value *doc)
{
    const struct json_value *data = mah_data(doc);
    const struct json_value *columns = mah_member(data, "columns");
    const struct json_value *rows = mah_member(data, "rows");
    if (!columns || columns->type != JSON_ARR || !rows || rows->type != JSON_ARR ||
        rows->num_children != 1u) return false;
    const struct json_value *row = json_at(rows, 0);
    if (!row || row->type != JSON_ARR || row->num_children != columns->num_children)
        return false;
    const struct json_value *status = NULL, *confirmations = NULL, *height = NULL;
    for (size_t i = 0; i < columns->num_children; i++) {
        const char *name = json_get_str(json_at(columns, i));
        if (!name) return false;
        if (strcmp(name, "status") == 0) status = json_at(row, i);
        else if (strcmp(name, "confirmations") == 0) confirmations = json_at(row, i);
        else if (strcmp(name, "block_height") == 0) height = json_at(row, i);
    }
    return status && status->type == JSON_STR &&
        strcmp(json_get_str(status), "CONFIRMED") == 0 &&
        confirmations && confirmations->type == JSON_INT &&
        json_get_int(confirmations) >= 1 && height && height->type == JSON_INT &&
        json_get_int(height) == 102;
}

static bool mah_expect_replay(const struct json_value *doc,
                              const char *field, const char *expected,
                              bool require_committed)
{
    const struct json_value *data = mah_data(doc);
    return data && mah_bool(data, "idempotent_replay", true) &&
        mah_streq(data, field, expected) &&
        (!require_committed ||
         (mah_streq(data, "stage", "committed") &&
          mah_bool(data, "committed", true)));
}

static bool mah_arg_i64(const char *text, int64_t *out)
{
    uint64_t value = 0;
    if (!mah_parse_u64(text, &value) || value > INT64_MAX) return false;
    *out = (int64_t)value;
    return true;
}

static int mah_json_command(int argc, char **argv)
{
    struct json_value doc;
    json_init(&doc);
    if (!mah_read_json(&doc)) { json_free(&doc); return mah_fail("invalid JSON input"); }
    bool ok = false;
    const char *output = NULL;
    if (strcmp(argv[1], "get") == 0 && argc == 3) {
        ok = mah_print_value(mah_path(&doc, argv[2]));
    } else if (strcmp(argv[1], "rpc-result") == 0 && argc == 2) {
        const struct json_value *error = mah_member(&doc, "error");
        ok = (!error || error->type == JSON_NULL) &&
            mah_print_value(mah_member(&doc, "result"));
    } else if (strcmp(argv[1], "offer-plan") == 0 && argc == 6) {
        int64_t size, chunks, total;
        ok = mah_arg_i64(argv[3], &size) && mah_arg_i64(argv[4], &chunks) &&
            mah_arg_i64(argv[5], &total) &&
            mah_expect_offer_plan(&doc, argv[2], size, chunks, total);
    } else if (strcmp(argv[1], "offer-commit") == 0 && argc == 3) {
        output = mah_expect_offer_commit(&doc, argv[2]); ok = output != NULL;
    } else if (strcmp(argv[1], "buyer-entry") == 0 && argc == 7) {
        int64_t price, chunks, total;
        ok = mah_arg_i64(argv[4], &price) && mah_arg_i64(argv[5], &chunks) &&
            mah_arg_i64(argv[6], &total) &&
            mah_expect_buyer_entry(&doc, argv[2], argv[3], price, chunks, total);
    } else if (strcmp(argv[1], "market-empty") == 0 && argc == 2) {
        ok = mah_expect_market_empty(&doc);
    } else if (strcmp(argv[1], "market-hidden") == 0 && argc == 3) {
        ok = mah_expect_market_hidden(&doc, argv[2]);
    } else if (strcmp(argv[1], "purchase-plan") == 0 && argc == 4) {
        int64_t total;
        if (mah_arg_i64(argv[3], &total))
            output = mah_expect_purchase_plan(&doc, argv[2], total);
        ok = output != NULL;
    } else if (strcmp(argv[1], "purchase-commit") == 0 && argc == 2) {
        output = mah_expect_purchase_commit(&doc); ok = output != NULL;
    } else if (strcmp(argv[1], "early-refusal") == 0 && argc == 2) {
        ok = mah_expect_early_refusal(&doc);
    } else if (strcmp(argv[1], "purchase-status") == 0 && argc == 3) {
        ok = mah_expect_purchase_status(&doc, argv[2]);
    } else if (strcmp(argv[1], "retrieve") == 0 && argc == 4) {
        int64_t size, chunks;
        ok = mah_arg_i64(argv[2], &size) && mah_arg_i64(argv[3], &chunks) &&
            mah_expect_retrieve(&doc, size, chunks);
    } else if (strcmp(argv[1], "claim") == 0 && argc == 2) {
        ok = mah_expect_claim(&doc);
    } else if (strcmp(argv[1], "recommit") == 0 && argc == 3) {
        ok = mah_expect_replay(&doc, "txid", argv[2], false);
    } else if (strcmp(argv[1], "replan") == 0 && argc == 3) {
        ok = mah_expect_replay(&doc, "plan_id", argv[2], false);
    } else if (strcmp(argv[1], "reoffer") == 0 && argc == 3) {
        ok = mah_expect_replay(&doc, "offer_id", argv[2], true);
    }
    if (output) printf("%s\n", output);
    json_free(&doc);
    return ok ? 0 : mah_fail("JSON contract mismatch");
}

static int mah_selftest(void)
{
    const char sample[] = "{\"result\":{\"ready\":true,\"n\":7}}";
    struct json_value doc;
    json_init(&doc);
    bool ok = json_read(&doc, sample, sizeof(sample) - 1u) &&
        mah_path(&doc, "result.ready") &&
        json_get_bool(mah_path(&doc, "result.ready")) &&
        json_get_int(mah_path(&doc, "result.n")) == 7 &&
        mah_hex64("0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef");
    json_free(&doc);
    if (!ok) return mah_fail("selftest failed");
    puts("market-acceptance-helper: PASS");
    return 0;
}

int main(int argc, char **argv)
{
    if (argc == 2 && strcmp(argv[1], "--selftest") == 0)
        return mah_selftest();
    if (argc == 5 && strcmp(argv[1], "fixture-create") == 0) {
        uint64_t price = 0, tail = 0, size = 0, total = 0;
        char root[65];
        if (!mah_parse_u64(argv[3], &price) || !mah_parse_u64(argv[4], &tail) ||
            !mah_fixture_create(argv[2], price, tail, &size, root, &total))
            return mah_fail("fixture creation failed");
        printf("%" PRIu64 " %s %" PRIu64 "\n", size, root, total);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "file-root") == 0) {
        char root[65];
        if (!mah_file_manifest_root(argv[2], root))
            return mah_fail("file manifest verification failed");
        printf("%s\n", root);
        return 0;
    }
    if (argc == 3 && strcmp(argv[1], "reverse-hex") == 0) {
        if (!mah_hex64(argv[2])) return mah_fail("invalid 64-byte-order hex");
        for (int i = 62; i >= 0; i -= 2) {
            putchar(argv[2][i]);
            putchar(argv[2][i + 1]);
        }
        putchar('\n');
        return 0;
    }
    if (argc >= 2) return mah_json_command(argc, argv);
    return mah_fail("command required");
}

/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_zcode_fetch — the slice-12 typed-command contract gate for the
 * swarm operator surface (tools/command/native_zcode_swarm_command.c):
 *
 *   zcode package fetch    one-shot (no live engine): the resumable
 *                          download record is persisted under
 *                          <datadir>/zcode/downloads/<root-hex> and the
 *                          reply honestly reports live:false; a complete
 *                          package reports already_complete; BAD_ROOT
 *                          names the bad input
 *   zcode package peers    one-shot: live:false, empty list, never a
 *                          replayed-from-disk fake
 *   zcode package pin      UNKNOWN_PACKAGE names the untracked root; a
 *                          tracked package pins (operator path, never
 *                          tier-gated) and reports its pool
 *   zcode package unpin    releases the pin; idempotent
 *
 * Handlers are called directly with a typed JSON input (the zp_cmd
 * idiom from test_zcode_publish.c) over ./test-tmp datadirs. */

#include "test/test_core.h"

#include "command/native_command.h"

#include "chain/chainparams.h"
#include "json/json.h"
#include "vcs/package_manifest.h"
#include "vcs/package_reward.h"
#include "vcs/package_store.h"
#include "vcs/package_swarm_node.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define ZF_MAX_FILE 256u

/* ── zp_cmd idiom ───────────────────────────────────────────────────── */

struct zf_cmd {
    struct json_value input;
    struct zcl_command_request request;
    struct zcl_command_reply reply;
};

static void zf_cmd_init(struct zf_cmd *c, const char *datadir)
{
    json_init(&c->input);
    json_set_object(&c->input);
    memset(&c->request, 0, sizeof(c->request));
    c->request.input = &c->input;
    zcl_command_reply_init(&c->reply, "zcl.zcode_test.v1");
    (void)json_push_kv_str(&c->input, "datadir", datadir);
}

static void zf_cmd_free(struct zf_cmd *c)
{
    zcl_command_reply_free(&c->reply);
    json_free(&c->input);
}

/* ── fixture package (two single-chunk files) ───────────────────────── */

struct zf_pkg {
    struct vcs_package_manifest manifest;
    uint8_t *wire;
    size_t wire_len;
    uint8_t root[32];
    char root_hex[65];
    uint8_t contents[2][ZF_MAX_FILE];
    size_t lens[2];
};

static bool zf_make_package(struct zf_pkg *p, uint8_t seed)
{
    static const char *const k_paths[2] = { "LICENSE", "src/a.c" };
    memset(p, 0, sizeof(*p));
    vcs_package_manifest_init(&p->manifest);
    for (size_t i = 0; i < 2; i++) {
        size_t len = 48u + i * 37u + seed;
        for (size_t j = 0; j < len; j++)
            p->contents[i][j] = (uint8_t)(seed + i * 7u + j * 3u);
        p->lens[i] = len;
        uint8_t hash[32];
        if (!vcs_package_chunk_hash(p->contents[i], len, hash))
            return false;
        if (!vcs_package_manifest_add(&p->manifest, k_paths[i],
                                      VCS_PACKAGE_MODE_FILE, len, hash, 1))
            return false;
    }
    if (!vcs_package_manifest_serialize(&p->manifest, &p->wire,
                                        &p->wire_len))
        return false;
    if (!vcs_package_manifest_root(&p->manifest, p->root))
        return false;
    vcs_reward_hex_encode(p->root, 32, p->root_hex);
    return true;
}

static void zf_free_package(struct zf_pkg *p)
{
    vcs_package_manifest_free(&p->manifest);
    free(p->wire);
    p->wire = NULL;
}

static bool zf_store_package(struct vcs_package_store *store,
                             const struct zf_pkg *p)
{
    uint8_t root[32];
    if (vcs_package_store_put_manifest(store, p->wire, p->wire_len,
                                       root) != VCS_PACKAGE_STORE_OK)
        return false;
    for (size_t i = 0; i < 2; i++) {
        const char *path = p->manifest.files[i].path;
        if (vcs_package_store_put_chunk(store, root, path, 0,
                                        p->contents[i],
                                        p->lens[i]) != VCS_PACKAGE_STORE_OK)
            return false;
    }
    return true;
}

/* ── the cases (one TEST per function — the label is function-scoped) ── */

static int zf_t_fetch_one_shot(void)
{
    int failures = 0;
    TEST("zcode package fetch (one-shot): record persisted, live:false, "
         "state want-manifest; idempotent redelivery") {
        char dd[1024];
        test_make_tmpdir(dd, sizeof(dd), "zcode_fetch", "fetch");
        struct zf_pkg pkg;
        ASSERT(zf_make_package(&pkg, 0x44));

        struct zf_cmd c;
        zf_cmd_init(&c, dd);
        (void)json_push_kv_str(&c.input, "root", pkg.root_hex);
        (void)json_push_kv_int(&c.input, "day", 20500);
        zcl_native_handle_zcode_package_fetch(&c.request, &c.reply);
        ASSERT(c.reply.status == ZCL_COMMAND_STATUS_PASSED);
        ASSERT(json_get(&c.reply.data, "live") &&
               !json_get_bool(json_get(&c.reply.data, "live")));
        ASSERT(json_get(&c.reply.data, "already_complete") &&
               !json_get_bool(json_get(&c.reply.data, "already_complete")));
        {
            const struct json_value *dl = json_get(&c.reply.data, "download");
            ASSERT(dl != NULL);
            const char *state = json_get_str(json_get(dl, "state"));
            ASSERT(state && strcmp(state, "want-manifest") == 0);
        }
        zf_cmd_free(&c);

        /* The resumable record is the one-shot command's only lasting
         * effect — persisted under <datadir>/zcode/downloads. */
        {
            char path[1200];
            snprintf(path, sizeof(path), "%s/zcode/downloads/%s", dd,
                     pkg.root_hex);
            struct stat st;
            ASSERT(stat(path, &st) == 0 && st.st_size > 0);
        }

        /* Idempotent: the same fetch again is still OK (active download
         * for the same root), and the record survives. */
        zf_cmd_init(&c, dd);
        (void)json_push_kv_str(&c.input, "root", pkg.root_hex);
        (void)json_push_kv_int(&c.input, "day", 20500);
        zcl_native_handle_zcode_package_fetch(&c.request, &c.reply);
        ASSERT(c.reply.status == ZCL_COMMAND_STATUS_PASSED);
        zf_cmd_free(&c);
        zf_free_package(&pkg);
        PASS();
    } _test_next:;
    return failures;
}

static int zf_t_fetch_bad_root(void)
{
    int failures = 0;
    TEST("zcode package fetch: BAD_ROOT names the bad input") {
        char dd[1024];
        test_make_tmpdir(dd, sizeof(dd), "zcode_fetch", "badroot");
        struct zf_cmd c;
        zf_cmd_init(&c, dd);
        (void)json_push_kv_str(&c.input, "root", "not-hex");
        zcl_native_handle_zcode_package_fetch(&c.request, &c.reply);
        ASSERT(c.reply.status == ZCL_COMMAND_STATUS_FAILED);
        ASSERT(strcmp(c.reply.error.code, "BAD_ROOT") == 0);
        zf_cmd_free(&c);
        PASS();
    } _test_next:;
    return failures;
}

static int zf_t_fetch_complete(void)
{
    int failures = 0;
    TEST("zcode package fetch on a complete package: already_complete") {
        char dd[1024];
        test_make_tmpdir(dd, sizeof(dd), "zcode_fetch", "complete");
        struct zf_pkg pkg;
        ASSERT(zf_make_package(&pkg, 0x55));
        {
            struct vcs_package_store *store = vcs_package_store_open(
                dd, VCS_PACKAGE_STORE_DEFAULT_QUOTA_BYTES);
            ASSERT(store != NULL);
            ASSERT(zf_store_package(store, &pkg));
            vcs_package_store_close(store);
        }
        struct zf_cmd c;
        zf_cmd_init(&c, dd);
        (void)json_push_kv_str(&c.input, "root", pkg.root_hex);
        zcl_native_handle_zcode_package_fetch(&c.request, &c.reply);
        ASSERT(c.reply.status == ZCL_COMMAND_STATUS_PASSED);
        ASSERT(json_get(&c.reply.data, "already_complete") &&
               json_get_bool(json_get(&c.reply.data, "already_complete")));
        {
            const struct json_value *dl = json_get(&c.reply.data, "download");
            ASSERT(dl != NULL);
            const char *state = json_get_str(json_get(dl, "state"));
            ASSERT(state && strcmp(state, "complete") == 0);
        }
        zf_cmd_free(&c);
        zf_free_package(&pkg);
        PASS();
    } _test_next:;
    return failures;
}

static int zf_t_peers_one_shot(void)
{
    int failures = 0;
    TEST("zcode package peers (one-shot): live:false, empty, honest note") {
        char dd[1024];
        test_make_tmpdir(dd, sizeof(dd), "zcode_fetch", "peers");
        struct zf_pkg pkg;
        ASSERT(zf_make_package(&pkg, 0x66));
        struct zf_cmd c;
        zf_cmd_init(&c, dd);
        (void)json_push_kv_str(&c.input, "root", pkg.root_hex);
        zcl_native_handle_zcode_package_peers(&c.request, &c.reply);
        ASSERT(c.reply.status == ZCL_COMMAND_STATUS_PASSED);
        ASSERT(json_get(&c.reply.data, "live") &&
               !json_get_bool(json_get(&c.reply.data, "live")));
        ASSERT(json_get_int(json_get(&c.reply.data, "peer_count")) == 0);
        ASSERT(json_get_str(json_get(&c.reply.data, "note")) != NULL);
        zf_cmd_free(&c);
        zf_free_package(&pkg);
        PASS();
    } _test_next:;
    return failures;
}

static int zf_t_pin_roundtrip(void)
{
    int failures = 0;
    TEST("zcode package pin: UNKNOWN_PACKAGE, then pin + unpin round-trip") {
        char dd[1024];
        test_make_tmpdir(dd, sizeof(dd), "zcode_fetch", "pin");
        struct zf_pkg pkg;
        ASSERT(zf_make_package(&pkg, 0x77));

        /* Unknown root: named rejection. */
        struct zf_cmd c;
        zf_cmd_init(&c, dd);
        (void)json_push_kv_str(&c.input, "root", pkg.root_hex);
        zcl_native_handle_zcode_package_pin(&c.request, &c.reply);
        ASSERT(c.reply.status == ZCL_COMMAND_STATUS_FAILED);
        ASSERT(strcmp(c.reply.error.code, "UNKNOWN_PACKAGE") == 0);
        zf_cmd_free(&c);

        {
            struct vcs_package_store *store = vcs_package_store_open(
                dd, VCS_PACKAGE_STORE_DEFAULT_QUOTA_BYTES);
            ASSERT(store != NULL);
            ASSERT(zf_store_package(store, &pkg));
            vcs_package_store_close(store);
        }

        /* Pin (operator path): pinned:true, pool pins. */
        zf_cmd_init(&c, dd);
        (void)json_push_kv_str(&c.input, "root", pkg.root_hex);
        zcl_native_handle_zcode_package_pin(&c.request, &c.reply);
        ASSERT(c.reply.status == ZCL_COMMAND_STATUS_PASSED);
        ASSERT(json_get(&c.reply.data, "pinned") &&
               json_get_bool(json_get(&c.reply.data, "pinned")));
        {
            const struct json_value *pj = json_get(&c.reply.data, "package");
            ASSERT(pj != NULL);
            ASSERT(json_get_bool(json_get(pj, "pinned")));
            const char *pool = json_get_str(json_get(pj, "pool"));
            ASSERT(pool && strcmp(pool, "pins") == 0);
        }
        zf_cmd_free(&c);

        /* Unpin: pinned:false, idempotent. */
        zf_cmd_init(&c, dd);
        (void)json_push_kv_str(&c.input, "root", pkg.root_hex);
        zcl_native_handle_zcode_package_unpin(&c.request, &c.reply);
        ASSERT(c.reply.status == ZCL_COMMAND_STATUS_PASSED);
        ASSERT(json_get(&c.reply.data, "pinned") &&
               !json_get_bool(json_get(&c.reply.data, "pinned")));
        zf_cmd_free(&c);

        zf_cmd_init(&c, dd);
        (void)json_push_kv_str(&c.input, "root", pkg.root_hex);
        zcl_native_handle_zcode_package_unpin(&c.request, &c.reply);
        ASSERT(c.reply.status == ZCL_COMMAND_STATUS_PASSED);
        zf_cmd_free(&c);
        zf_free_package(&pkg);
        PASS();
    } _test_next:;
    return failures;
}

int test_zcode_fetch(void)
{
    int failures = 0;
    chain_params_select(CHAIN_MAIN);
    failures += zf_t_fetch_one_shot();
    failures += zf_t_fetch_bad_root();
    failures += zf_t_fetch_complete();
    failures += zf_t_peers_one_shot();
    failures += zf_t_pin_roundtrip();
    return failures;
}

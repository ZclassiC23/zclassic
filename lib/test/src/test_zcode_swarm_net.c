/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_zcode_swarm_net — the slice-12 swarm over the REAL wire seam.
 * test_zcode_swarm.c drives one engine with hand-built frames; this file
 * puts two (and three) independent swarm engines behind two independent
 * msg_processors and lets real "zpkgswm" P2P messages (magic + command +
 * length + checksum, queued by p2p_node_begin/write/end_message, parsed
 * by the real p2p_node_receive_bytes, dispatched by the real
 * msg_process_messages through the dispatch-table row) carry every
 * ANNOUNCE/WANT/DATA/CANCEL between them. Only the socket syscalls are
 * elided (the sentinel technique from test_snapshot_serve_loopback.c).
 *
 * The test glue mirrors config/src/boot_zcode_swarm.c 1:1 — same
 * session pseudo-key derivation (0x02 || SHA3-256(domain || host)),
 * same penalty→peer_offence mapping, same reply/drain sends — with two
 * deliberate, documented substitutions: the score callback returns a
 * fixed contributor score (the production glue reads the reward ledger;
 * a NEW_USER peer has a 0/hour announce rate in the frozen slice-11
 * table, so honest announces would otherwise be flood-penalized), and
 * the tick clock is a deterministic counter instead of wall time.
 *
 * Covered:
 *   1. Golden path: two engines, end-to-end verified fetch into the CAS,
 *      byte-identical chunks, both books credited, zero misbehavior.
 *   2. Malicious server (wrong-hash chunks): PEER_OFFENCE_INVALID_CHUNK
 *      misbehavior on the real peer object, no credit, nothing stored,
 *      download ends in a NAMED failure.
 *   3. Unrequested DATA: PEER_OFFENCE_UNREQUESTED, no credit.
 *   4. Restart mid-download: engine freed and recreated over the same
 *      datadir; the persisted record resumes and the download completes.
 *   5. Disconnect requeue: one of two servers drops mid-download; the
 *      in-flight work moves to the survivor and the download completes.
 *   6. Sovereign source: one signed workspace-head lookup fetches a bundled
 *      content.v2 evidence closure, then accepted source rebuilds Git-free. */

#include "test/test_core.h"
#include "test/accepted_work_fixture.h"

#include "base/hex.h"
#include "chain/chainparams.h"
#include "coins/coins_view.h"
#include "core/serialize.h"
#include "core/uint256.h"
#include "crypto/ed25519.h"
#include "crypto/sha256.h"
#include "crypto/sha3.h"
#include "event/event.h"
#include "keys/key.h"
#include "keys/pubkey.h"
#include "net/fast_sync.h"
#include "net/msgprocessor.h"
#include "net/net.h"
#include "net/peer_identity.h"
#include "net/peer_scoring.h"
#include "services/package_lifecycle.h"
#include "util/safe_alloc.h"
#include "validation/main_state.h"
#include "validation/txmempool.h"
#include "vcs/package_build.h"
#include "vcs/package_checkout.h"
#include "vcs/package_content.h"
#include "vcs/package_service.h"
#include "vcs/package_mapping.h"
#include "vcs/package_prepare.h"
#include "vcs/package_release.h"
#include "vcs/package_store.h"
#include "vcs/package_swarm.h"
#include "vcs/package_swarm_node.h"
#include "vcs/package_transport.h"
#include "vcs/source_package_checkout.h"
#include "vcs/source_package_transport.h"
#include "vcs/vcs.h"
#include "vcs/vcs_devloop.h"
#include "vcs/vcs_object.h"
#include "vcs/zcode_c23_corpus.h"
#include "vcs/zcode_commons_v2.h"
#include "vcs/zcode_dht_identity.h"
#include "vcs/zcode_dht_service.h"
#include "vcs/zcode_lane.h"

#include <errno.h>
#include <fcntl.h>
#include <inttypes.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#define ZWN_CHECK(name, expr) do {                                       \
    if (expr) { printf("  zcode_swarm_net: %s... OK\n", (name)); }       \
    else { printf("  zcode_swarm_net: %s... FAIL\n", (name)); \
        failures++; }                                                    \
} while (0)

#define ZWN_DAY 20500
#define ZWN_SCORE UINT64_C(100) /* EARNED_CONTRIBUTOR: announces allowed */
#define ZWN_MAX_FILES 13u
#define ZWN_MAX_FILE 512u
#define ZWN_KEY_DOMAIN "zcl.zcode_swarm_peer.v1"
#define ZWN_EVIDENCE_ASSIGNMENT "evidence/source-assignment.v1"
#define ZWN_EVIDENCE_COMMIT "evidence/zvcs-commit.v1"
#define ZWN_EVIDENCE_PASSPORT "evidence/module-passport.v1"
#define ZWN_EVIDENCE_RELEASE "evidence/package-release.v1"
#define ZWN_EVIDENCE_WORKSPACE "evidence/workspace-manifest.v1"

/* ── fixture package (single-chunk files, same shape as the engine gate) */

struct zwn_pkg {
    struct vcs_package_manifest manifest;
    uint8_t *wire;
    size_t wire_len;
    uint8_t root[32];
    size_t count;
    uint8_t contents[ZWN_MAX_FILES][ZWN_MAX_FILE];
    size_t lens[ZWN_MAX_FILES];
    uint64_t total_bytes;
};

struct zwn_sovereign_receipt {
    bool ready;
    uint8_t source_root[32];
    uint8_t accepted_work_root[32];
    uint8_t proof_set_root[32];
    uint8_t commit_root[32];
    uint8_t release_root[32];
    uint8_t passport_root[32];
    uint8_t workspace_root[32];
    uint8_t workspace_carrier_root[32];
    uint8_t source_package_root[32];
    uint8_t publisher_binary_sha3[32];
    uint8_t successor_release_root[32];
    uint8_t successor_package_root[32];
    uint8_t successor_binary_sha3[32];
    uint8_t storage_ack_roots[2][32];
    uint16_t storage_ack_count;
    uint8_t provider_record_root[32];
    uint8_t publication_job_root[32];
    uint8_t publication_progress_root[32];
    int64_t publication_enqueue_us;
    struct vcs_package_mapping_metrics publication_metrics;
    struct vcs_source_bundle_metrics source_metrics;
};

static struct zwn_sovereign_receipt g_zwn_sovereign_receipt;

static bool zwn_hashes_match(const char *label,
                             const uint8_t expected[32],
                             const uint8_t actual[32])
{
    if (memcmp(expected, actual, 32) == 0)
        return true;
    char expected_hex[65], actual_hex[65];
    zcl_hex_encode(expected, 32, expected_hex);
    zcl_hex_encode(actual, 32, actual_hex);
    printf("sovereign-source-roundtrip: DIVERGENCE first=%s expected=%s actual=%s\n",
           label, expected_hex, actual_hex);
    return false;
}

static void zwn_print_root_json(const char *key, const uint8_t root[32])
{
    char hex[65];
    zcl_hex_encode(root, 32, hex);
    printf(",\"%s\":\"%s\"", key, hex);
}

static void zwn_print_sovereign_receipt(void)
{
    const struct zwn_sovereign_receipt *r = &g_zwn_sovereign_receipt;
    printf("{\"schema\":\"zcl.sovereign_source_roundtrip.v1\","
           "\"status\":\"passed\"");
    zwn_print_root_json("source_tree_root", r->source_root);
    zwn_print_root_json("accepted_work_root", r->accepted_work_root);
    zwn_print_root_json("proof_set_root", r->proof_set_root);
    zwn_print_root_json("zvcs_commit_root", r->commit_root);
    zwn_print_root_json("release_root", r->release_root);
    zwn_print_root_json("passport_root", r->passport_root);
    zwn_print_root_json("workspace_root", r->workspace_root);
    zwn_print_root_json("workspace_carrier_root",
                        r->workspace_carrier_root);
    zwn_print_root_json("source_package_root", r->source_package_root);
    zwn_print_root_json("publisher_binary_sha3",
                        r->publisher_binary_sha3);
    zwn_print_root_json("successor_release_root",
                        r->successor_release_root);
    zwn_print_root_json("successor_package_root",
                        r->successor_package_root);
    zwn_print_root_json("successor_binary_sha3",
                        r->successor_binary_sha3);
    zwn_print_root_json("storage_ack_a_root", r->storage_ack_roots[0]);
    zwn_print_root_json("storage_ack_b_root", r->storage_ack_roots[1]);
    zwn_print_root_json("provider_record_root", r->provider_record_root);
    zwn_print_root_json("publication_job_root", r->publication_job_root);
    zwn_print_root_json("publication_progress_root",
                        r->publication_progress_root);
    printf(",\"source_bytes\":%" PRIu64
           ",\"new_source_blobs\":%u,\"reused_source_blobs\":%u"
           ",\"publication_enqueue_us\":%" PRId64
           ",\"publication_bytes_scanned\":%" PRIu64
           ",\"publication_new_chunks\":%u"
           ",\"publication_reused_chunks\":%u"
           ",\"serving_providers\":2,\"pinned_providers\":2"
           ",\"storage_acknowledgements\":%u"
           ",\"storage_ack_status\":\"2/2\""
           ",\"reproduced\":true,\"provider_failover\":true"
           ",\"corrupt_chunk_recovery\":true"
           ",\"previous_release_fetchable\":true"
           ",\"publication_job\":\"storage_acknowledged\""
           ",\"github_mirror\":\"not_applicable_fixture\"}\n",
           r->source_metrics.source_bytes,
           r->source_metrics.new_blobs,
           r->source_metrics.reused_blobs,
           r->publication_enqueue_us,
           r->publication_metrics.bytes_scanned,
           r->publication_metrics.new_chunks,
           r->publication_metrics.reused_chunks,
           r->storage_ack_count);
}

static bool zwn_make_package(struct zwn_pkg *p, size_t count, uint8_t seed)
{
    static const char *const k_paths[ZWN_MAX_FILES] = {
        "LICENSE", "include/a.h", "include/b.h", "include/c.h",
        "include/d.h", "src/a.c", "src/b.c", "src/c.c", "src/d.c",
        "src/e.c", "tests/t1.c", "tests/t2.c", "tests/t3.c",
    };
    memset(p, 0, sizeof(*p));
    if (count == 0 || count > ZWN_MAX_FILES)
        return false;
    vcs_package_manifest_init(&p->manifest);
    for (size_t i = 0; i < count; i++) {
        size_t len = 40u + i * 31u + seed;
        for (size_t j = 0; j < len; j++)
            p->contents[i][j] = (uint8_t)(seed + i * 7u + j * 3u);
        p->lens[i] = len;
        p->total_bytes += len;
        uint8_t hash[32];
        if (!vcs_package_chunk_hash(p->contents[i], len, hash))
            return false;
        if (!vcs_package_manifest_add(&p->manifest, k_paths[i],
                                      VCS_PACKAGE_MODE_FILE, len, hash, 1))
            return false;
    }
    p->count = count;
    if (!vcs_package_manifest_serialize(&p->manifest, &p->wire,
                                        &p->wire_len))
        return false;
    return vcs_package_manifest_root(&p->manifest, p->root);
}

static bool zwn_make_evidence_package(
    struct zwn_pkg *p, const uint8_t *const wires[5],
    const size_t lengths[5])
{
    static const char *const paths[5] = {
        ZWN_EVIDENCE_PASSPORT,
        ZWN_EVIDENCE_RELEASE,
        ZWN_EVIDENCE_ASSIGNMENT,
        ZWN_EVIDENCE_WORKSPACE,
        ZWN_EVIDENCE_COMMIT,
    };
    memset(p, 0, sizeof(*p));
    vcs_package_manifest_init(&p->manifest);
    for (size_t i = 0; i < 5; i++) {
        if (!wires[i] || lengths[i] == 0 || lengths[i] > ZWN_MAX_FILE)
            return false;
        memcpy(p->contents[i], wires[i], lengths[i]);
        p->lens[i] = lengths[i];
        p->total_bytes += lengths[i];
        uint8_t hash[32];
        if (!vcs_package_chunk_hash(wires[i], lengths[i], hash) ||
            !vcs_package_manifest_add(
                &p->manifest, paths[i], VCS_PACKAGE_MODE_FILE,
                lengths[i], hash, 1))
            return false;
    }
    p->count = 5;
    if (!vcs_package_manifest_serialize(
            &p->manifest, &p->wire, &p->wire_len))
        return false;
    return vcs_package_manifest_root(&p->manifest, p->root);
}

static void zwn_free_package(struct zwn_pkg *p)
{
    vcs_package_manifest_free(&p->manifest);
    free(p->wire);
    p->wire = NULL;
}

static bool zwn_store_package(struct vcs_package_store *store,
                              const struct zwn_pkg *p)
{
    uint8_t root[32];
    if (vcs_package_store_put_manifest(store, p->wire, p->wire_len,
                                       root) != VCS_PACKAGE_STORE_OK)
        return false;
    for (size_t i = 0; i < p->count; i++) {
        const char *path = p->manifest.files[i].path;
        if (vcs_package_store_put_chunk(store, root, path, 0,
                                        p->contents[i],
                                        p->lens[i]) != VCS_PACKAGE_STORE_OK)
            return false;
    }
    return true;
}

static bool zwn_store_source_transport(
    struct vcs_package_store *store,
    const struct vcs_source_package_transport *transport)
{
    uint8_t root[32];
    if (!store || !transport ||
        vcs_package_store_put_manifest(
            store, transport->manifest_wire, transport->manifest_wire_len,
            root) != VCS_PACKAGE_STORE_OK ||
        memcmp(root, transport->package_root, 32) != 0)
        return false;
    size_t count = vcs_source_package_transport_file_count(transport);
    for (size_t i = 0; i < count; i++) {
        const char *path = NULL;
        const uint8_t *bytes = NULL;
        size_t len = 0;
        if (!vcs_source_package_transport_file_at(
                transport, i, &path, &bytes, &len))
            return false;
        if (vcs_package_content_put_file(
                store, root, path, bytes, len) != VCS_PACKAGE_STORE_OK)
            return false;
    }
    struct vcs_package_store_status status;
    return vcs_package_store_package_status(store, root, &status) &&
           status.complete;
}

static bool zwn_write_file(const char *root, const char *relative,
                           const void *bytes, size_t len, mode_t mode)
{
    char path[1400];
    int n = snprintf(path, sizeof(path), "%s/%s", root, relative);
    if (!bytes || n <= 0 || (size_t)n >= sizeof(path))
        return false;
    int fd = open(path, O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC,
                  mode);
    if (fd < 0)
        return false;
    size_t off = 0;
    while (off < len) {
        ssize_t wrote = write(fd, (const uint8_t *)bytes + off, len - off);
        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0)
            break;
        off += (size_t)wrote;
    }
    bool ok = off == len && fsync(fd) == 0;
    if (close(fd) != 0)
        ok = false;
    return ok;
}

static bool zwn_compile_c23(const char *source, const char *binary)
{
    pid_t child = fork();
    if (child < 0)
        return false;
    if (child == 0) {
        char *const argv[] = {
            (char *)"cc", (char *)"-std=c2x", (char *)"-O2",
            (char *)"-fno-ident", (char *)"-Wl,--build-id=none",
            (char *)source, (char *)"-o", (char *)binary, NULL,
        };
        execvp(argv[0], argv);
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool zwn_run_fixture_binary(const char *binary)
{
    pid_t child = fork();
    if (child < 0)
        return false;
    if (child == 0) {
        char *const argv[] = {(char *)binary, NULL};
        execv(binary, argv);
        _exit(127);
    }
    int status = 0;
    while (waitpid(child, &status, 0) < 0) {
        if (errno != EINTR)
            return false;
    }
    return WIFEXITED(status) && WEXITSTATUS(status) == 0;
}

static bool zwn_sha3_file(const char *path, uint8_t out[32])
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return false;
    struct sha3_256_ctx hash;
    sha3_256_init(&hash);
    uint8_t buffer[16384];
    bool ok = true;
    for (;;) {
        ssize_t got = read(fd, buffer, sizeof(buffer));
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0) {
            ok = false;
            break;
        }
        if (got == 0)
            break;
        sha3_256_write(&hash, buffer, (size_t)got);
    }
    if (close(fd) != 0)
        ok = false;
    if (ok)
        sha3_256_finalize(&hash, out);
    return ok;
}

static bool zwn_sha256_file(const char *path, uint8_t out[32])
{
    int fd = open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return false;
    struct sha256_ctx hash;
    sha256_init(&hash);
    uint8_t buffer[16384];
    bool ok = true;
    for (;;) {
        ssize_t got = read(fd, buffer, sizeof(buffer));
        if (got < 0 && errno == EINTR)
            continue;
        if (got < 0) {
            ok = false;
            break;
        }
        if (got == 0)
            break;
        sha256_write(&hash, buffer, (size_t)got);
    }
    if (close(fd) != 0)
        ok = false;
    if (ok)
        sha256_finalize(&hash, out);
    return ok;
}

static void zwn_literal_root(const char *literal, uint8_t out[32])
{
    sha3_256((const uint8_t *)literal, strlen(literal), out);
}

static bool zwn_release_make(
    const uint8_t package_root[32], const uint8_t recipe_root[32],
    const char *semver, uint64_t sequence, const uint8_t *parent_root,
    struct vcs_package_release *release, uint8_t release_root[32])
{
    struct privkey key;
    memset(&key, 0, sizeof(key));
    memset(key.vch, 0x47, sizeof(key.vch));
    key.fValid = true;
    key.fCompressed = true;
    struct pubkey pubkey;
    if (!privkey_get_pubkey(&key, &pubkey) ||
        pubkey.size != COMPRESSED_PUBLIC_KEY_SIZE)
        return false;
    memset(release, 0, sizeof(*release));
    release->schema_version = VCS_PACKAGE_RELEASE_VERSION;
    (void)snprintf(release->name, sizeof(release->name),
                   "fixture/sovereign-source");
    (void)snprintf(release->semver, sizeof(release->semver), "%s", semver);
    memcpy(release->package_root, package_root, 32);
    release->has_parent = parent_root != NULL;
    if (parent_root)
        memcpy(release->parent_root, parent_root, 32);
    memcpy(release->publisher_pubkey, pubkey.vch,
           COMPRESSED_PUBLIC_KEY_SIZE);
    release->publisher_sequence = sequence;
    (void)snprintf(release->license, sizeof(release->license),
                   "Apache-2.0");
    memcpy(release->recipe_root, recipe_root, 32);
    (void)snprintf(release->chain_id, sizeof(release->chain_id),
                   "zclassic-regtest");
    if (vcs_package_release_id(release, release_root) !=
        VCS_PACKAGE_RELEASE_OK)
        return false;
    struct uint256 hash;
    memcpy(hash.data, release_root, 32);
    unsigned char compact[COMPACT_SIGNATURE_SIZE];
    if (!privkey_sign_compact(&key, &hash, compact))
        return false;
    memcpy(release->signature, compact + 1,
           VCS_PACKAGE_RELEASE_SIGNATURE_BYTES);
    return vcs_package_release_verify(release) == VCS_PACKAGE_RELEASE_OK;
}

struct zwn_package_scenario {
    const char *name;
    const char *dht_namespace;
    const char *source_dir;
    uint64_t publisher_sequence;
    const char *expected_package_root_hex;
};

#define ZCODE_PACKAGE(name, dir, sequence, content, release, recipe, lock, capsule, publisher, signature) \
    {name, "package.c23-commons", dir, sequence, content},
static const struct zwn_package_scenario zwn_package_scenarios[] = {
#include "../../../config/zcode_package_registry.def"
#include "../../../config/zcode_c23_commons_app.def"
};
#undef ZCODE_PACKAGE

#define ZWN_PACKAGE_SCENARIO_COUNT \
    (sizeof(zwn_package_scenarios) / sizeof(zwn_package_scenarios[0]))

struct zwn_package_scenario_result {
    uint8_t package_root[32];
    uint8_t recipe_root[32];
    uint8_t release_id[32];
    uint8_t transport_root[32];
    uint64_t cold_bytes;
    uint64_t repeat_bytes;
    uint64_t source_bytes;
    uint32_t source_chunks;
    uint32_t reused_objects;
    bool survived_publisher;
};

static bool zwn_prepare_package_transport(
    const struct zwn_package_scenario *scenario,
    const char *source_dir, uint64_t publisher_sequence,
    const char *expected_package_root_hex,
    struct vcs_package_prepared *prepared,
    struct vcs_package_transport *transport)
{
    struct privkey key;
    memset(&key, 0, sizeof(key));
    memset(key.vch, 0x47, sizeof(key.vch));
    key.fValid = true;
    key.fCompressed = true;
    struct pubkey pubkey;
    if (!scenario || !prepared || !transport ||
        !privkey_get_pubkey(&key, &pubkey) ||
        pubkey.size != COMPRESSED_PUBLIC_KEY_SIZE)
        return false;
    struct vcs_package_prepare_options options = {
        .dir = source_dir,
        .publisher_sequence = publisher_sequence,
        .reward_address = "",
        .chain_id = "zclassic-main",
    };
    memcpy(options.publisher_pubkey, pubkey.vch,
           COMPRESSED_PUBLIC_KEY_SIZE);
    char detail[160] = {0};
    vcs_package_prepared_init(prepared);
    if (vcs_package_prepare(&options, prepared, detail, sizeof(detail)) !=
        VCS_PACKAGE_PREPARE_OK) {
        fprintf(stderr, "zwn package prepare: %s\n", detail);
        return false;
    }
    uint8_t expected[32];
    if (expected_package_root_hex &&
        (!zcl_hex_decode(expected_package_root_hex,
                         expected, sizeof(expected)) ||
         memcmp(expected, prepared->package_root, 32) != 0))
        return false;
    struct uint256 digest;
    memcpy(digest.data, prepared->signing_digest, 32);
    uint8_t compact[COMPACT_SIGNATURE_SIZE];
    if (!privkey_sign_compact(&key, &digest, compact))
        return false;
    memcpy(prepared->release.signature, compact + 1,
           VCS_PACKAGE_RELEASE_SIGNATURE_BYTES);
    uint8_t *release_wire = NULL;
    size_t release_wire_len = 0;
    bool ok = vcs_package_release_verify(&prepared->release) ==
                  VCS_PACKAGE_RELEASE_OK &&
        vcs_package_release_serialize(&prepared->release, &release_wire,
                                      &release_wire_len) ==
            VCS_PACKAGE_RELEASE_OK &&
        vcs_package_transport_build(
            release_wire, release_wire_len,
            prepared->recipe_wire, prepared->recipe_wire_len,
            prepared->manifest_wire, prepared->manifest_wire_len,
            transport) == VCS_PACKAGE_TRANSPORT_OK;
    free(release_wire);
    return ok;
}

/* ── the loopback node: msg_processor + engine + store + book ───────── */

struct zwn_node {
    struct main_state ms;
    struct tx_mempool mempool;
    struct coins_view null_view;
    struct coins_view_cache coins;
    struct net_manager nm;
    struct msg_processor mp;
    char datadir[1024];
    char zcode_dir[1100];
    struct vcs_package_store *store;
    struct vcs_service_book *book;
    struct vcs_swarm_engine *engine;
    uint64_t now;
    bool tamper_chunks; /* corrupt DATA replies carrying chunk objects */
};

struct zwn_link {
    struct zwn_node *owner;
    struct p2p_node *node; /* owner's connection object for the remote */
    struct send_segment *sentinel;
};

static struct vcs_zcode_dht_time zwn_dht_time(uint64_t now)
{
    return (struct vcs_zcode_dht_time){
        .wall_unix = now,
        .monotonic_s = now,
    };
}

static bool zwn_dht_chain_ok(
    void *ctx, const struct vcs_zcode_dht_delegation *delegation)
{
    (void)ctx;
    return delegation && delegation->beacon_height == 120;
}

static bool zwn_dht_policy_allow(
    void *ctx, enum vcs_zcode_sovereignty_action action,
    const struct vcs_zcode_sovereignty_subject *subject)
{
    (void)ctx;
    return action < VCS_ZCODE_SOVEREIGNTY_ACTION_COUNT && subject;
}

static bool zwn_dht_identity(
    const char *datadir, uint8_t master_byte, const uint8_t genesis[32],
    const uint8_t noise[32])
{
    uint8_t online_seed[32], online_pubkey[32], master_seed[32];
    uint8_t beacon_hash[32];
    char error[160];
    memset(master_seed, master_byte, sizeof(master_seed));
    memset(beacon_hash, 0x44, sizeof(beacon_hash));
    if (!vcs_zcode_dht_online_key_load_or_create(
            datadir, online_seed, online_pubkey, error, sizeof(error)))
        return false;
    struct vcs_zcode_dht_delegation delegation;
    bool ok = vcs_zcode_dht_delegation_sign(
                  &delegation, genesis, online_pubkey, noise, 120,
                  beacon_hash, 1000, 90000, 1, master_seed) ==
              VCS_ZCODE_DHT_DELEGATION_OK;
    if (ok)
        ok = vcs_zcode_dht_delegation_save(
            datadir, &delegation, error, sizeof(error));
    memset(online_seed, 0, sizeof(online_seed));
    memset(master_seed, 0, sizeof(master_seed));
    return ok;
}

static struct vcs_zcode_dht_service *zwn_dht_service(
    const char *datadir, const uint8_t genesis[32], const uint8_t noise[32])
{
    struct vcs_zcode_dht_service_params params = {
        .datadir = datadir,
        .transport_enabled = true,
        .now = {.wall_unix = 1000, .monotonic_s = 1000},
        .chain_verify = zwn_dht_chain_ok,
        .policy_decide = zwn_dht_policy_allow,
    };
    memcpy(params.network_genesis, genesis, 32);
    memcpy(params.local_noise_static, noise, 32);
    return vcs_zcode_dht_service_create(&params);
}

static bool zwn_dht_pump_half(
    struct vcs_zcode_dht_service *from,
    struct vcs_zcode_dht_service *to, uint64_t expected_outbound_peer,
    uint64_t inbound_peer, uint64_t now, bool *moved)
{
    uint8_t wire[VCS_ZCODE_DHT_MAX_FRAME_BYTES];
    uint64_t peer = 0;
    size_t wire_len = 0;
    while (vcs_zcode_dht_service_next_outbound(
               from, 0, &peer, wire, sizeof(wire), &wire_len)) {
        enum vcs_zcode_dht_reject_reason rejected =
            VCS_ZCODE_DHT_REJECT_MALFORMED;
        if (peer != expected_outbound_peer)
            return false;
        if (!vcs_zcode_dht_service_handle_frame(
                to, inbound_peer, wire, wire_len, zwn_dht_time(now),
                &rejected)) {
            fprintf(stderr, "zwn DHT frame rejected: %s\n",
                    vcs_zcode_dht_reject_reason_string(rejected));
            return false;
        }
        *moved = true;
    }
    return true;
}

static bool zwn_dht_drive_pair(
    struct vcs_zcode_dht_service *publisher,
    struct vcs_zcode_dht_service *consumer, uint64_t now)
{
    for (size_t round = 0; round < 64; round++) {
        bool moved = false;
        if (!zwn_dht_pump_half(
                publisher, consumer, 2, 1, now, &moved) ||
            !zwn_dht_pump_half(
                consumer, publisher, 1, 2, now, &moved))
            return false;
        if (!moved)
            return true;
    }
    return false;
}

static bool zwn_dht_drive_record_query(
    struct vcs_zcode_dht_service *publisher,
    struct vcs_zcode_dht_service *consumer, uint64_t operation,
    uint64_t now, struct vcs_zcode_dht_record_operation_result *result)
{
    for (size_t round = 0; round < 256; round++) {
        bool moved = false;
        if (!zwn_dht_pump_half(
                publisher, consumer, 2, 1, now, &moved)) {
            fprintf(stderr, "zwn DHT query: publisher frame rejected\n");
            return false;
        }
        if (!zwn_dht_pump_half(
                consumer, publisher, 1, 2, now, &moved)) {
            fprintf(stderr, "zwn DHT query: consumer frame rejected\n");
            return false;
        }
        if (!vcs_zcode_dht_service_record_operation_poll(
                consumer, operation, zwn_dht_time(now), result)) {
            fprintf(stderr, "zwn DHT query: operation missing\n");
            return false;
        }
        if (result->state == VCS_ZCODE_DHT_RECORD_OPERATION_COMPLETE)
            return true;
        if (result->state != VCS_ZCODE_DHT_RECORD_OPERATION_PENDING) {
            fprintf(stderr, "zwn DHT query: terminal state %d\n",
                    (int)result->state);
            return false;
        }
        if (!moved) {
            fprintf(stderr, "zwn DHT query: pending without frames\n");
            return false;
        }
    }
    return false;
}

static bool zwn_discover_transport(
    const struct zwn_node *publisher, const struct zwn_node *mirror,
    const struct zwn_node *consumer, const char *namespace_name,
    const uint8_t semantic_root[32],
    const uint8_t expected_transport_root[32], uint8_t transport_root[32],
    struct vcs_zcode_dht_record *provider_record_out, uint64_t now)
{
    if (!namespace_name || now < 2)
        return false;
    struct vcs_package_store_status publisher_status, mirror_status;
    if (!vcs_package_store_package_status(
            publisher->store, expected_transport_root, &publisher_status) ||
        !vcs_package_store_package_status(
            mirror->store, expected_transport_root, &mirror_status) ||
        !publisher_status.complete || !publisher_status.pinned ||
        !mirror_status.complete || !mirror_status.pinned)
        return false;
    char publisher_dht_dir[1200], consumer_dht_dir[1200];
    int publisher_dir_len = snprintf(
        publisher_dht_dir, sizeof(publisher_dht_dir), "%s/dht-%" PRIu64,
        publisher->datadir, now);
    int consumer_dir_len = snprintf(
        consumer_dht_dir, sizeof(consumer_dht_dir), "%s/dht-%" PRIu64,
        consumer->datadir, now);
    if (publisher_dir_len <= 0 ||
        (size_t)publisher_dir_len >= sizeof(publisher_dht_dir) ||
        consumer_dir_len <= 0 ||
        (size_t)consumer_dir_len >= sizeof(consumer_dht_dir) ||
        (mkdir(publisher_dht_dir, 0700) != 0 && errno != EEXIST) ||
        (mkdir(consumer_dht_dir, 0700) != 0 && errno != EEXIST))
        return false;
    uint8_t genesis[32], publisher_noise[32], consumer_noise[32];
    uint8_t transcript[32];
    memset(genesis, 0x71, sizeof(genesis));
    memset(publisher_noise, 0x72, sizeof(publisher_noise));
    memset(consumer_noise, 0x73, sizeof(consumer_noise));
    memset(transcript, 0x74, sizeof(transcript));
    if (!zwn_dht_identity(
            publisher_dht_dir, 0x75, genesis, publisher_noise) ||
        !zwn_dht_identity(
            consumer_dht_dir, 0x76, genesis, consumer_noise))
        return false;

    struct vcs_zcode_dht_service *publisher_dht =
        zwn_dht_service(publisher_dht_dir, genesis, publisher_noise);
    struct vcs_zcode_dht_service *consumer_dht =
        zwn_dht_service(consumer_dht_dir, genesis, consumer_noise);
    const char *stage = "service-create";
    bool ok = publisher_dht && consumer_dht;
    if (!ok)
        goto out;
    struct vcs_zcode_dht_session publisher_session = {
        .established = true,
        .generation = 1,
        .connection_serial = 1,
    };
    struct vcs_zcode_dht_session consumer_session = publisher_session;
    consumer_session.connection_serial = 2;
    memcpy(publisher_session.remote_static, consumer_noise, 32);
    memcpy(consumer_session.remote_static, publisher_noise, 32);
    memcpy(publisher_session.transcript_hash, transcript, 32);
    memcpy(consumer_session.transcript_hash, transcript, 32);
    stage = "session-open";
    ok = vcs_zcode_dht_service_session_open(
             publisher_dht, 2, &publisher_session, zwn_dht_time(now - 1)) &&
         vcs_zcode_dht_service_session_open(
             consumer_dht, 1, &consumer_session, zwn_dht_time(now - 1)) &&
         zwn_dht_drive_pair(publisher_dht, consumer_dht, now);
    if (!ok)
        goto out;

    struct vcs_zcode_dht_publish_spec pointer_spec;
    memset(&pointer_spec, 0, sizeof(pointer_spec));
    pointer_spec.kind = VCS_ZCODE_DHT_RECORD_POINTER;
    (void)snprintf(pointer_spec.namespace_name,
                   sizeof(pointer_spec.namespace_name),
                   "%s", namespace_name);
    memcpy(pointer_spec.semantic_root, semantic_root, 32);
    memcpy(pointer_spec.transport_root, expected_transport_root, 32);
    pointer_spec.sequence = 1;
    pointer_spec.not_before = 1000;
    pointer_spec.expiry = 2000;
    uint8_t plan_token[32];
    struct vcs_zcode_dht_record record;
    stage = "publish";
    ok = vcs_zcode_dht_service_record_publish_plan(
             publisher_dht, &pointer_spec, plan_token, &record) &&
         vcs_zcode_dht_service_record_publish_commit(
             publisher_dht, &pointer_spec, plan_token, zwn_dht_time(now),
             &record) == VCS_ZCODE_DHT_RECORD_STORE_ADDED;
    if (!ok)
        goto out;

    struct vcs_zcode_dht_record_selector selector = {
        .kind = VCS_ZCODE_DHT_RECORD_POINTER,
    };
    (void)snprintf(selector.namespace_name, sizeof(selector.namespace_name),
                   "%s", namespace_name);
    memcpy(selector.root, semantic_root, 32);
    struct vcs_zcode_dht_record empty_cache[1];
    stage = "empty-local-query";
    if (vcs_zcode_dht_service_record_local_query(
            consumer_dht, now, &selector, empty_cache, 1) != 0) {
        ok = false;
        goto out;
    }
    uint64_t operation = 0;
    stage = "query-begin";
    ok = vcs_zcode_dht_service_record_query_begin(
        consumer_dht, 1, &selector, zwn_dht_time(now), &operation);
    struct vcs_zcode_dht_record_operation_result result;
    memset(&result, 0, sizeof(result));
    if (ok) {
        stage = "query-drive";
        ok = zwn_dht_drive_record_query(
            publisher_dht, consumer_dht, operation, now, &result);
    }
    if (!ok)
        goto out;
    stage = "query-result";
    ok = result.state == VCS_ZCODE_DHT_RECORD_OPERATION_COMPLETE &&
         result.record_count == 1 &&
         result.records[0].kind == VCS_ZCODE_DHT_RECORD_POINTER &&
         memcmp(result.records[0].semantic_root, semantic_root, 32) == 0 &&
         memcmp(result.records[0].transport_root,
                expected_transport_root, 32) == 0;
    if (ok)
        memcpy(transport_root, result.records[0].transport_root, 32);
    if (!ok || !provider_record_out)
        goto out;

    struct vcs_zcode_dht_publish_spec provider_spec;
    memset(&provider_spec, 0, sizeof(provider_spec));
    provider_spec.kind = VCS_ZCODE_DHT_RECORD_PROVIDER;
    (void)snprintf(provider_spec.namespace_name,
                   sizeof(provider_spec.namespace_name),
                   "%s", namespace_name);
    memcpy(provider_spec.transport_root, expected_transport_root, 32);
    provider_spec.sequence = 1;
    provider_spec.not_before = 1000;
    provider_spec.expiry = 2000;
    struct vcs_zcode_dht_record provider_record;
    stage = "provider-publish";
    ok = vcs_zcode_dht_service_record_publish_plan(
             publisher_dht, &provider_spec, plan_token, &provider_record) &&
         vcs_zcode_dht_service_record_publish_commit(
             publisher_dht, &provider_spec, plan_token,
             zwn_dht_time(now + 1),
             &provider_record) == VCS_ZCODE_DHT_RECORD_STORE_ADDED;
    if (!ok)
        goto out;

    struct vcs_zcode_dht_record_selector provider_selector = {
        .kind = VCS_ZCODE_DHT_RECORD_PROVIDER,
    };
    (void)snprintf(provider_selector.namespace_name,
                   sizeof(provider_selector.namespace_name),
                   "%s", namespace_name);
    memcpy(provider_selector.root, expected_transport_root, 32);
    operation = 0;
    stage = "provider-query-begin";
    ok = vcs_zcode_dht_service_record_query_begin(
        consumer_dht, 1, &provider_selector, zwn_dht_time(now + 1),
        &operation);
    memset(&result, 0, sizeof(result));
    if (ok) {
        stage = "provider-query-drive";
        ok = zwn_dht_drive_record_query(
            publisher_dht, consumer_dht, operation, now + 1, &result);
    }
    if (!ok)
        goto out;
    stage = "provider-query-result";
    ok = result.state == VCS_ZCODE_DHT_RECORD_OPERATION_COMPLETE &&
         result.record_count == 1 &&
         result.records[0].kind == VCS_ZCODE_DHT_RECORD_PROVIDER &&
         memcmp(result.records[0].transport_root,
                expected_transport_root, 32) == 0 &&
         memcmp(result.records[0].provider_node_id,
                provider_record.provider_node_id, 32) == 0;
    if (ok && provider_record_out)
        *provider_record_out = result.records[0];

out:
    if (!ok)
        fprintf(stderr, "zwn DHT: %s failed\n", stage);
    if (consumer_dht)
        vcs_zcode_dht_service_free(consumer_dht, zwn_dht_time(now + 2));
    if (publisher_dht)
        vcs_zcode_dht_service_free(publisher_dht, zwn_dht_time(now + 2));
    return ok;
}

static bool zwn_author_storage_ack(
    const struct zwn_node *provider, const char *namespace_name,
    const uint8_t transport_root[32], uint8_t identity_byte,
    uint8_t owner_group_byte, struct vcs_zcode_dht_record *record_out,
    uint8_t record_root_out[32])
{
    if (!provider || !namespace_name || !transport_root || !record_out ||
        !record_root_out)
        return false;
    struct vcs_package_store_status status;
    if (!vcs_package_store_package_status(
            provider->store, transport_root, &status) ||
        !status.complete || !status.pinned)
        return false;

    uint8_t genesis[32], noise[32];
    memset(genesis, 0x71, sizeof(genesis));
    memset(noise, identity_byte, sizeof(noise));
    if (!zwn_dht_identity(
            provider->datadir, identity_byte, genesis, noise))
        return false;
    struct vcs_zcode_dht_service *service =
        zwn_dht_service(provider->datadir, genesis, noise);
    if (!service)
        return false;

    struct vcs_zcode_dht_publish_spec spec;
    memset(&spec, 0, sizeof(spec));
    spec.kind = VCS_ZCODE_DHT_RECORD_STORAGE_ACK;
    (void)snprintf(spec.namespace_name, sizeof(spec.namespace_name),
                   "%s", namespace_name);
    memcpy(spec.transport_root, transport_root, 32);
    memset(spec.owner_group, owner_group_byte, sizeof(spec.owner_group));
    spec.sequence = 1;
    spec.not_before = 1000;
    spec.expiry = 2000;
    uint8_t plan_token[32];
    struct vcs_zcode_dht_record generic_record;
    bool ok = !vcs_zcode_dht_service_record_publish_plan(
                  service, &spec, plan_token, &generic_record) &&
        vcs_zcode_dht_service_storage_ack_plan(
            service, provider->store, &spec, plan_token, record_out) &&
        vcs_zcode_dht_service_storage_ack_commit(
            service, provider->store, &spec, plan_token,
            zwn_dht_time(1002), record_out) ==
            VCS_ZCODE_DHT_RECORD_STORE_ADDED &&
        record_out->kind == VCS_ZCODE_DHT_RECORD_STORAGE_ACK &&
        memcmp(record_out->transport_root, transport_root, 32) == 0 &&
        vcs_zcode_dht_record_id(record_out, record_root_out) ==
            VCS_ZCODE_DHT_RECORD_OK;
    struct vcs_zcode_dht_record_selector selector = {
        .kind = VCS_ZCODE_DHT_RECORD_STORAGE_ACK,
    };
    (void)snprintf(selector.namespace_name, sizeof(selector.namespace_name),
                   "%s", namespace_name);
    memcpy(selector.root, transport_root, 32);
    struct vcs_zcode_dht_record queried;
    ok = ok && vcs_zcode_dht_service_record_local_query(
                   service, 1002, &selector, &queried, 1) == 1 &&
        memcmp(queried.provider_node_id, record_out->provider_node_id, 32) ==
            0;
    vcs_zcode_dht_service_free(service, zwn_dht_time(1003));
    return ok;
}

static uint64_t zwn_score(const uint8_t contributor[33], void *ctx)
{
    (void)contributor;
    (void)ctx;
    return ZWN_SCORE;
}

/* Production-identical session pseudo-key derivation. */
static bool zwn_peer_key(const struct p2p_node *node, uint8_t out[33])
{
    char host[ZCL_PEER_HOST_KEY_MAX];
    if (!zcl_peer_host_key(node, host, sizeof(host)))
        return false;
    out[0] = 0x02;
    struct sha3_256_ctx h;
    sha3_256_init(&h);
    sha3_256_write(&h, (const unsigned char *)ZWN_KEY_DOMAIN,
                   sizeof(ZWN_KEY_DOMAIN) - 1);
    sha3_256_write(&h, (const unsigned char *)host, strlen(host));
    sha3_256_finalize(&h, out + 1);
    return true;
}

static enum peer_offence zwn_offence(enum vcs_swarm_penalty penalty)
{
    switch (penalty) {
    case VCS_SWARM_PENALTY_MALFORMED:
        return PEER_OFFENCE_INVALID_MESSAGE;
    case VCS_SWARM_PENALTY_ANNOUNCE_FLOOD:
    case VCS_SWARM_PENALTY_REQUEST_FLOOD:
        return PEER_OFFENCE_FLOOD;
    case VCS_SWARM_PENALTY_REPLAYED_REQUEST:
    case VCS_SWARM_PENALTY_REPLAYED_DATA:
        return PEER_OFFENCE_INVALID_PAYLOAD;
    case VCS_SWARM_PENALTY_UNREQUESTED_DATA:
        return PEER_OFFENCE_UNREQUESTED;
    case VCS_SWARM_PENALTY_INVALID_DATA:
        return PEER_OFFENCE_INVALID_CHUNK;
    case VCS_SWARM_PENALTY_NONE:
    default:
        return PEER_OFFENCE_NONE;
    }
}

static void zwn_send(struct zwn_node *z, struct p2p_node *node,
                     const uint8_t *frame, size_t frame_len)
{
    if (atomic_load(&node->disconnect))
        return;
    if (!p2p_node_begin_message(node, "zpkgswm",
                                z->mp.params->pchMessageStart))
        return;
    p2p_node_write_message_data(node, frame, frame_len);
    (void)p2p_node_end_message(node);
}

/* The frame hook: config/src/boot_zcode_swarm.c's boot_zcode_swarm_frame
 * with the deterministic-clock + fixed-score substitutions (file header). */
static bool zwn_frame(struct msg_processor *mp, struct p2p_node *node,
                      const uint8_t *payload, size_t payload_len, void *ctx)
{
    struct zwn_node *z = ctx;
    uint8_t key[33];
    if (!zwn_peer_key(node, key))
        return true;
    (void)vcs_swarm_engine_peer_add(z->engine, (uint64_t)node->id, key);
    struct vcs_swarm_frame_result ev = vcs_swarm_engine_handle_frame(
        z->engine, (uint64_t)node->id, payload, payload_len, ZWN_DAY,
        ++z->now);
    if (ev.penalty != VCS_SWARM_PENALTY_NONE)
        peer_scoring_record(mp->net_mgr, node, zwn_offence(ev.penalty),
                            "zcode swarm test");
    if (ev.reply) {
        if (z->tamper_chunks && ev.reply_len > 0) {
            /* Corrupt only CHUNK data replies: the manifest still
             * verifies, so the download reaches the chunk stage and the
             * bad-hash path is what gets exercised. */
            struct vcs_package_swarm_message msg;
            if (vcs_package_swarm_parse(ev.reply, ev.reply_len, &msg) &&
                msg.type == VCS_PACKAGE_SWARM_DATA &&
                msg.body.data.object.object_kind ==
                    VCS_PACKAGE_SWARM_OBJECT_CHUNK)
                ev.reply[ev.reply_len - 1] ^= 0xff;
        }
        zwn_send(z, node, ev.reply, ev.reply_len);
        free(ev.reply);
    }
    if (ev.disconnect_peer)
        atomic_store(&node->disconnect, true);
    return true;
}

/* The tick hook: scheduler tick + per-peer outbound drain. Called from
 * the real msg_process_messages (proving the wiring) and directly by the
 * drive loop (so the scheduler also advances in quiet cycles). */
static void zwn_tick(struct msg_processor *mp, struct p2p_node *node,
                     void *ctx)
{
    struct zwn_node *z = ctx;
    (void)mp;
    vcs_swarm_engine_tick(z->engine, ZWN_DAY, ++z->now);
    uint8_t frame[VCS_SWARM_OUTBOUND_FRAME_MAX];
    for (;;) {
        uint64_t peer = 0;
        size_t frame_len = 0;
        if (!vcs_swarm_engine_next_outbound(z->engine, (uint64_t)node->id,
                                            &peer, frame, &frame_len))
            break;
        if (peer != (uint64_t)node->id || frame_len == 0)
            break;
        zwn_send(z, node, frame, frame_len);
    }
}

static bool zwn_node_init(struct zwn_node *z, const char *tag,
                          const struct chain_params *params)
{
    memset(&z->ms, 0, sizeof(z->ms));
    memset(&z->mempool, 0, sizeof(z->mempool));
    memset(&z->null_view, 0, sizeof(z->null_view));
    memset(&z->nm, 0, sizeof(z->nm));
    memset(&z->mp, 0, sizeof(z->mp));
    main_state_init(&z->ms);
    tx_mempool_init(&z->mempool, 0);
    coins_view_cache_init(&z->coins, &z->null_view);
    net_manager_init(&z->nm);
    z->mp.main_state = &z->ms;
    z->mp.mempool = &z->mempool;
    z->mp.coins_tip = &z->coins;
    z->mp.params = params;
    z->mp.net_mgr = &z->nm;
    z->now = 0;
    z->tamper_chunks = false;
    test_make_tmpdir(z->datadir, sizeof(z->datadir), "zcode_swarm_net",
                     tag);
    z->mp.datadir = z->datadir;
    snprintf(z->zcode_dir, sizeof(z->zcode_dir), "%s/zcode", z->datadir);
    z->store = vcs_package_store_open(
        z->datadir, VCS_PACKAGE_STORE_DEFAULT_QUOTA_BYTES);
    z->book = vcs_service_book_load(z->zcode_dir);
    if (!z->store || !z->book)
        return false;
    z->engine = vcs_swarm_engine_create(z->store, z->book, z->zcode_dir,
                                        zwn_score, NULL);
    if (!z->engine)
        return false;
    msg_processor_set_zcode_swarm(&z->mp, zwn_frame, zwn_tick, z);
    return true;
}

static void zwn_node_free(struct zwn_node *z)
{
    vcs_swarm_engine_free(z->engine);
    vcs_service_book_free(z->book);
    vcs_package_store_close(z->store);
    z->engine = NULL;
    z->book = NULL;
    z->store = NULL;
    net_manager_free(&z->nm);
    coins_view_cache_free(&z->coins);
    tx_mempool_free(&z->mempool);
    main_state_free(&z->ms);
}

/* A fresh engine over the same datadir (the restart-resume substitution:
 * store + book stay open; only the engine is recreated, exactly like a
 * process restart that keeps the datadir). */
static bool zwn_node_restart_engine(struct zwn_node *z)
{
    vcs_swarm_engine_free(z->engine);
    z->engine = vcs_swarm_engine_create(z->store, z->book, z->zcode_dir,
                                        zwn_score, NULL);
    return z->engine != NULL;
}

/* ── links + pump (the sentinel technique) ──────────────────────────── */

static bool zwn_link(struct zwn_node *owner, struct zwn_link *l,
                     uint8_t ip_a, uint8_t ip_b, uint8_t ip_c, uint8_t ip_d,
                     const char *name)
{
    /* p2p_node_create leaves id 0 on a fresh net_manager; the engine
     * reserves peer id 0 (the next_outbound "no filter" convention), so
     * loopback links take explicit nonzero ids. Distinct per owner. */
    static node_id_t next_id = 100;
    struct net_address addr;
    memset(&addr, 0, sizeof(addr));
    memcpy(addr.svc.addr.ip, pchIPv4Prefix, 12);
    addr.svc.addr.ip[12] = ip_a;
    addr.svc.addr.ip[13] = ip_b;
    addr.svc.addr.ip[14] = ip_c;
    addr.svc.addr.ip[15] = ip_d;
    addr.svc.port = 18033;
    l->owner = owner;
    l->node = p2p_node_create(&owner->nm, ZCL_INVALID_SOCKET, &addr,
                              name, false);
    if (!l->node)
        return false;
    l->node->id = next_id++;
    l->node->version = 1;
    l->node->services = NODE_ZCL23;
    l->node->state = PEER_HANDSHAKE_COMPLETE;
    l->sentinel = zcl_calloc(1, sizeof(*l->sentinel),
                             "zcode_swarm_net_sentinel");
    if (!l->sentinel)
        return false;
    l->node->send_head = l->sentinel;
    l->node->send_tail = l->sentinel;
    l->node->send_offset = 0;
    return true;
}

static void zwn_link_free(struct zwn_link *l)
{
    send_segment_free(l->sentinel);
    l->node->send_head = NULL;
    l->node->send_tail = NULL;
    l->node->recv_msg_count = 0;
    p2p_node_free(l->node);
    l->node = NULL;
    l->sentinel = NULL;
}

/* Drain from_link's queued wire bytes into to_link's owner through the
 * real receive parser + real dispatcher. */
static bool zwn_pump(struct zwn_link *from, struct zwn_link *to,
                     const unsigned char msgstart[MESSAGE_START_SIZE])
{
    bool any = false;
    struct send_segment *sentinel = from->sentinel;
    while (sentinel->next) {
        struct send_segment *seg = sentinel->next;
        sentinel->next = seg->next;
        if (from->node->send_tail == seg)
            from->node->send_tail = sentinel;
        if (from->node->send_size >= seg->size)
            from->node->send_size -= seg->size;
        else
            from->node->send_size = 0;
        if (!p2p_node_receive_bytes(to->node, (const char *)seg->data,
                                    (unsigned int)seg->size, msgstart)) {
            send_segment_free(seg);
            return false;
        }
        any = true;
        send_segment_free(seg);
    }
    from->node->send_head = sentinel;
    from->node->send_offset = 0;
    if (any)
        return msg_process_messages(&to->owner->mp, to->node);
    return true;
}

/* Free every queued segment without delivering it — the loopback
 * emulation of a socket teardown (a real restart/disconnect kills the
 * queued bytes with the fd). */
static void zwn_drain_quiet(struct zwn_link *l)
{
    while (l->sentinel->next) {
        struct send_segment *seg = l->sentinel->next;
        l->sentinel->next = seg->next;
        if (l->node->send_tail == seg)
            l->node->send_tail = l->sentinel;
        if (l->node->send_size >= seg->size)
            l->node->send_size -= seg->size;
        else
            l->node->send_size = 0;
        send_segment_free(seg);
    }
    l->node->send_head = l->sentinel;
    l->node->send_offset = 0;
}

/* Membership, mirroring the boot glue's sync. Semantic discovery may bind a
 * fetch directly to a provider, in which case bulk gossip is unnecessary. */
static bool zwn_meet_side_mode(struct zwn_node *me,
                               struct zwn_link *my_link, bool announce)
{
    uint8_t key[33];
    if (!zwn_peer_key(my_link->node, key))
        return false;
    bool known = vcs_swarm_engine_peer_known(me->engine,
                                             (uint64_t)my_link->node->id);
    if (!vcs_swarm_engine_peer_add(me->engine, (uint64_t)my_link->node->id,
                                   key))
        return false;
    if (!known && announce)
        (void)vcs_swarm_engine_announce_to(me->engine,
                                           (uint64_t)my_link->node->id);
    return true;
}

static bool zwn_meet_side(struct zwn_node *me, struct zwn_link *my_link)
{
    return zwn_meet_side_mode(me, my_link, true);
}

static bool zwn_meet_side_quiet(struct zwn_node *me,
                                struct zwn_link *my_link)
{
    return zwn_meet_side_mode(me, my_link, false);
}

/* One full round: both schedulers advance, then both directions pump. */
static bool zwn_round(struct zwn_link *a_b, struct zwn_link *b_a,
                      const unsigned char msgstart[MESSAGE_START_SIZE])
{
    zwn_tick(&a_b->owner->mp, a_b->node, a_b->owner);
    zwn_tick(&b_a->owner->mp, b_a->node, b_a->owner);
    if (!zwn_pump(a_b, b_a, msgstart))
        return false;
    return zwn_pump(b_a, a_b, msgstart);
}

static bool zwn_download_done(struct zwn_node *b, const uint8_t root[32],
                              enum vcs_swarm_download_state *state_out)
{
    struct vcs_swarm_download_status st;
    memset(&st, 0, sizeof(st));
    if (!vcs_swarm_engine_download_status(b->engine, root, &st))
        return false;
    *state_out = st.state;
    return st.state == VCS_SWARM_DL_COMPLETE ||
           st.state == VCS_SWARM_DL_FAILED;
}

static bool zwn_fetch_package_from_peers(
    struct zwn_node *consumer, const uint8_t root[32],
    struct zwn_link *provider_to_consumer,
    struct zwn_link *consumer_to_provider,
    struct zwn_link *mirror_to_consumer,
    struct zwn_link *consumer_to_mirror,
    const unsigned char msgstart[MESSAGE_START_SIZE])
{
    if (!consumer || !root || !provider_to_consumer ||
        !consumer_to_provider || !msgstart ||
        ((mirror_to_consumer == NULL) != (consumer_to_mirror == NULL)) ||
        vcs_swarm_engine_fetch(
            consumer->engine, root, ZWN_DAY, ++consumer->now) !=
            VCS_SWARM_FETCH_OK)
        return false;
    enum vcs_swarm_download_state state = VCS_SWARM_DL_INACTIVE;
    bool terminal = false;
    for (size_t round = 0; round < 600u && !terminal; round++) {
        if (!zwn_round(provider_to_consumer, consumer_to_provider,
                       msgstart) ||
            (mirror_to_consumer &&
             !zwn_round(mirror_to_consumer, consumer_to_mirror, msgstart)))
            return false;
        terminal = zwn_download_done(consumer, root, &state);
    }
    return terminal && state == VCS_SWARM_DL_COMPLETE;
}

static bool zwn_fetch_package_from_provider(
    struct zwn_node *consumer, const uint8_t root[32],
    struct zwn_link *provider_to_consumer,
    struct zwn_link *consumer_to_provider,
    const unsigned char msgstart[MESSAGE_START_SIZE])
{
    if (!consumer || !root || !provider_to_consumer ||
        !consumer_to_provider || !msgstart)
        return false;
    uint64_t provider = (uint64_t)consumer_to_provider->node->id;
    if (vcs_swarm_engine_fetch_from(
            consumer->engine, root, ZWN_DAY, ++consumer->now,
            &provider, 1) != VCS_SWARM_FETCH_OK)
        return false;
    enum vcs_swarm_download_state state = VCS_SWARM_DL_INACTIVE;
    bool terminal = false;
    for (size_t round = 0; round < 600u && !terminal; round++) {
        if (!zwn_round(provider_to_consumer, consumer_to_provider,
                       msgstart))
            return false;
        terminal = zwn_download_done(consumer, root, &state);
    }
    return terminal && state == VCS_SWARM_DL_COMPLETE;
}

static bool zwn_store_matches(struct vcs_package_store *store,
                              const struct zwn_pkg *p)
{
    for (size_t i = 0; i < p->count; i++) {
        if (!vcs_package_store_chunk_present(store, p->root, (uint32_t)i,
                                             0))
            return false;
        uint8_t *bytes = NULL;
        size_t len = 0;
        if (vcs_package_store_get_chunk_at(store, p->root, (uint32_t)i, 0,
                                           &bytes,
                                           &len) != VCS_PACKAGE_STORE_OK)
            return false;
        bool match = len == p->lens[i] &&
                     memcmp(bytes, p->contents[i], len) == 0;
        free(bytes);
        if (!match)
            return false;
    }
    return true;
}

static bool zwn_read_package_file(
    struct vcs_package_store *store, const uint8_t package_root[32],
    const char *path, uint8_t *out, size_t out_capacity, size_t *out_len)
{
    if (out_len)
        *out_len = 0;
    if (!store || !package_root || !path || !out || !out_len)
        return false;
    uint8_t *manifest_wire = NULL;
    size_t manifest_wire_len = 0;
    if (vcs_package_store_get_manifest_wire(
            store, package_root, &manifest_wire, &manifest_wire_len) !=
        VCS_PACKAGE_STORE_OK)
        return false;
    struct vcs_package_manifest manifest;
    vcs_package_manifest_init(&manifest);
    uint8_t checked[32];
    bool ok = vcs_package_manifest_parse(
            manifest_wire, manifest_wire_len, &manifest) &&
        vcs_package_manifest_root(&manifest, checked) &&
        memcmp(checked, package_root, 32) == 0;
    free(manifest_wire);
    size_t index = 0;
    while (ok && index < manifest.count &&
           strcmp(manifest.files[index].path, path) != 0)
        index++;
    if (!ok || index == manifest.count ||
        manifest.files[index].chunk_count != 1 ||
        manifest.files[index].size > out_capacity) {
        vcs_package_manifest_free(&manifest);
        return false;
    }
    uint8_t *chunk = NULL;
    size_t chunk_len = 0;
    ok = vcs_package_store_get_chunk_at(
             store, package_root, (uint32_t)index, 0,
             &chunk, &chunk_len) == VCS_PACKAGE_STORE_OK &&
        chunk_len == manifest.files[index].size &&
        vcs_package_verify_chunk(
            &manifest.files[index], 0, chunk, chunk_len);
    if (ok) {
        memcpy(out, chunk, chunk_len);
        *out_len = chunk_len;
    }
    free(chunk);
    vcs_package_manifest_free(&manifest);
    return ok;
}

static bool zwn_append_edit(const char *root, const char *relative,
                            const char *marker, off_t *original_size)
{
    char path[1400];
    int n = snprintf(path, sizeof(path), "%s/%s", root, relative);
    if (n <= 0 || (size_t)n >= sizeof(path) || !marker || !original_size)
        return false;
    int fd = open(path, O_WRONLY | O_APPEND | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return false;
    struct stat st;
    bool ok = fstat(fd, &st) == 0 && S_ISREG(st.st_mode);
    if (ok)
        *original_size = st.st_size;
    size_t len = strlen(marker), off = 0;
    while (ok && off < len) {
        ssize_t wrote = write(fd, marker + off, len - off);
        if (wrote < 0 && errno == EINTR)
            continue;
        if (wrote <= 0)
            ok = false;
        else
            off += (size_t)wrote;
    }
    ok = ok && fsync(fd) == 0;
    if (close(fd) != 0)
        ok = false;
    return ok;
}

static bool zwn_revert_edit(const char *root, const char *relative,
                            off_t original_size)
{
    char path[1400];
    int n = snprintf(path, sizeof(path), "%s/%s", root, relative);
    if (n <= 0 || (size_t)n >= sizeof(path) || original_size < 0)
        return false;
    int fd = open(path, O_WRONLY | O_CLOEXEC | O_NOFOLLOW);
    if (fd < 0)
        return false;
    bool ok = ftruncate(fd, original_size) == 0 && fsync(fd) == 0;
    if (close(fd) != 0)
        ok = false;
    return ok;
}

static size_t zwn_package_scenario_index(const char *name)
{
    for (size_t i = 0; i < ZWN_PACKAGE_SCENARIO_COUNT; i++)
        if (strcmp(zwn_package_scenarios[i].name, name) == 0)
            return i;
    return ZWN_PACKAGE_SCENARIO_COUNT;
}

static bool zwn_build_package(const char *datadir,
                              const uint8_t package_root[32],
                              int64_t *clock, size_t *rebuilt_out)
{
    char root_hex[65];
    zcl_hex_encode(package_root, 32, root_hex);
    int64_t planned_at = *clock;
    *clock += 4;
    struct package_lifecycle_plan_report plan;
    struct zcl_result planned = package_lifecycle_plan(
        datadir, root_hex, planned_at, &plan);
    if (!planned.ok || !plan.ready) {
        printf("  zcode_swarm_net: plan %s failed rule=%s detail=%s "
               "message=%s\n", root_hex, plan.rule, plan.detail,
               planned.message);
        return false;
    }
    struct package_lifecycle_commit_report commit;
    struct zcl_result committed = package_lifecycle_commit(
        datadir, plan.plan_id, planned_at + 1, &commit);
    if (!committed.ok || !commit.installed) {
        printf("  zcode_swarm_net: commit %s failed rule=%s detail=%s "
               "message=%s\n", root_hex, commit.rule, commit.detail,
               committed.message);
        return false;
    }
    for (size_t i = 0; i < commit.step_count; i++)
        if (!commit.steps[i].already_installed)
            (*rebuilt_out)++;
    return true;
}

static bool zwn_build_original_graph(const char *datadir, int64_t *clock,
                                     size_t *rebuilt_out)
{
    for (size_t i = 0; i < ZWN_PACKAGE_SCENARIO_COUNT; i++) {
        uint8_t root[32];
        if (!zcl_hex_decode_lower(
                zwn_package_scenarios[i].expected_package_root_hex,
                root, sizeof(root)) ||
            !zwn_build_package(datadir, root, clock, rebuilt_out))
            return false;
    }
    return true;
}

static bool zwn_original_receipts_present(const char *datadir,
                                          size_t *present_out)
{
    *present_out = 0;
    for (size_t i = 0; i < ZWN_PACKAGE_SCENARIO_COUNT; i++) {
        const char *root_hex =
            zwn_package_scenarios[i].expected_package_root_hex;
        char path[1400];
        int n = snprintf(path, sizeof(path),
                         "%s/zcode/installed/%s/build-report", datadir,
                         root_hex);
        if (n <= 0 || (size_t)n >= sizeof(path))
            return false;
        FILE *file = fopen(path, "rb");
        if (!file)
            return false;
        uint8_t wire[VCS_PACKAGE_BUILD_MAX_WIRE_BYTES];
        size_t wire_len = fread(wire, 1, sizeof(wire), file);
        bool read_ok = wire_len > 0 && feof(file) && !ferror(file);
        if (fclose(file) != 0 || !read_ok)
            return false;
        struct vcs_package_build_receipt receipt;
        uint8_t expected[32];
        if (vcs_package_build_parse(wire, wire_len, &receipt) !=
                VCS_PACKAGE_BUILD_OK ||
            !zcl_hex_decode_lower(root_hex, expected, sizeof(expected)) ||
            memcmp(receipt.package_root, expected, sizeof(expected)) != 0)
            return false;
        (*present_out)++;
    }
    return true;
}

/* ── 1 + 4 + 5: the golden fetch (with restart / disconnect variants) ── */

enum zwn_golden_mode {
    ZWN_GOLDEN_PLAIN = 0,
    ZWN_GOLDEN_RESTART,
    ZWN_GOLDEN_DISCONNECT,
};

static int zwn_test_golden(enum zwn_golden_mode mode,
                           const struct chain_params *params)
{
    int failures = 0;
    const char *name =
        mode == ZWN_GOLDEN_PLAIN
            ? "golden: end-to-end verified fetch over real zpkgswm frames"
        : mode == ZWN_GOLDEN_RESTART
            ? "restart mid-download: engine recreated over the same "
              "datadir resumes and completes"
            : "disconnect requeue: in-flight work moves to the surviving "
              "server and the download completes";

    TEST(name) {
        /* 12 single-chunk files (restart/disconnect) so the chunk stage
         * spans more than one round (per-peer in-flight is 8) and a
         * mid-download window deterministically exists; 6 for the plain
         * run. */
        size_t file_count = mode == ZWN_GOLDEN_PLAIN ? 6 : 12;
        struct zwn_pkg pkg;
        ASSERT(zwn_make_package(&pkg, file_count, 0x11));

        struct zwn_node a, b, a2;
        ASSERT(zwn_node_init(&a, mode == ZWN_GOLDEN_PLAIN ? "ga" :
                             mode == ZWN_GOLDEN_RESTART ? "ra" : "da",
                             params));
        ASSERT(zwn_node_init(&b, mode == ZWN_GOLDEN_PLAIN ? "gb" :
                             mode == ZWN_GOLDEN_RESTART ? "rb" : "db",
                             params));
        ASSERT(zwn_store_package(a.store, &pkg));

        struct zwn_link a_b, b_a;
        ASSERT(zwn_link(&a, &a_b, 5, 6, 7, 8, "peer-b"));
        ASSERT(zwn_link(&b, &b_a, 1, 2, 3, 4, "peer-a"));
        ASSERT(zwn_meet_side(&a, &a_b));
        ASSERT(zwn_meet_side(&b, &b_a));

        struct zwn_link a2_b, b_a2;
        bool two_servers = mode == ZWN_GOLDEN_DISCONNECT;
        if (two_servers) {
            ASSERT(zwn_node_init(&a2, "da2", params));
            ASSERT(zwn_store_package(a2.store, &pkg));
            ASSERT(zwn_link(&a2, &a2_b, 9, 9, 9, 9, "peer-b"));
            ASSERT(zwn_link(&b, &b_a2, 8, 8, 8, 8, "peer-a2"));
            ASSERT(zwn_meet_side(&a2, &a2_b));
            ASSERT(zwn_meet_side(&b, &b_a2));
        }

        ASSERT(vcs_swarm_engine_fetch(b.engine, pkg.root, ZWN_DAY,
                                      ++b.now) == VCS_SWARM_FETCH_OK);

        bool restarted = false;
        bool dropped = false;
        bool saw_partial = mode != ZWN_GOLDEN_PLAIN ? false : true;
        enum vcs_swarm_download_state state = VCS_SWARM_DL_INACTIVE;
        bool terminal = false;
        for (int i = 0; i < 400 && !terminal; i++) {
            ASSERT(zwn_round(&a_b, &b_a, params->pchMessageStart));
            if (two_servers)
                ASSERT(zwn_round(&a2_b, &b_a2, params->pchMessageStart));

            if (mode == ZWN_GOLDEN_RESTART && !restarted) {
                struct vcs_swarm_download_status st;
                memset(&st, 0, sizeof(st));
                ASSERT(vcs_swarm_engine_download_status(b.engine, pkg.root,
                                                        &st));
                if (st.state == VCS_SWARM_DL_CHUNKS &&
                    st.present_chunks > 0 &&
                    st.present_chunks < st.total_chunks) {
                    saw_partial = true;
                    restarted = true;
                    /* A real restart kills the sockets with their queued
                     * bytes: drop both queues first, or the new engine
                     * would read the pre-restart DATAs as unrequested
                     * (they would be — its outstanding table is gone). */
                    zwn_drain_quiet(&a_b);
                    zwn_drain_quiet(&b_a);
                    ASSERT(zwn_node_restart_engine(&b));
                    /* The restart forgets session peers AND their
                     * advertisements (transport state, never persisted).
                     * A real restart also drops the connections: emulate
                     * the reconnect on both sides — B's new engine
                     * re-registers A, A sees B as a new peer and
                     * re-announces its packages (exactly what the boot
                     * glue's membership sync does for a new node). */
                    ASSERT(zwn_meet_side(&b, &b_a));
                    vcs_swarm_engine_peer_drop(a.engine,
                                               (uint64_t)a_b.node->id);
                    ASSERT(zwn_meet_side(&a, &a_b));
                }
            }
            if (mode == ZWN_GOLDEN_DISCONNECT && !dropped) {
                struct vcs_swarm_download_status st;
                memset(&st, 0, sizeof(st));
                ASSERT(vcs_swarm_engine_download_status(b.engine, pkg.root,
                                                        &st));
                if (st.state == VCS_SWARM_DL_CHUNKS &&
                    st.present_chunks > 0 &&
                    st.present_chunks < st.total_chunks) {
                    saw_partial = true;
                    dropped = true;
                    /* The membership-sync drop: the connection is gone,
                     * so the engine forgets the peer and requeues its
                     * in-flight work onto the surviving advertiser. The
                     * dead connection's queued bytes die with it. */
                    atomic_store(&b_a.node->disconnect, true);
                    zwn_drain_quiet(&a_b);
                    zwn_drain_quiet(&b_a);
                    vcs_swarm_engine_peer_drop(b.engine,
                                               (uint64_t)b_a.node->id);
                }
            }
            terminal = zwn_download_done(&b, pkg.root, &state);
        }

        ASSERT(saw_partial);
        if (mode == ZWN_GOLDEN_RESTART)
            ASSERT(restarted);
        if (mode == ZWN_GOLDEN_DISCONNECT)
            ASSERT(dropped);
        ASSERT(terminal);
        ASSERT(state == VCS_SWARM_DL_COMPLETE);
        ASSERT(zwn_store_matches(b.store, &pkg));
        ASSERT(b_a.node->misbehavior == 0);
        if (!dropped)
            ASSERT(a_b.node->misbehavior == 0);

        /* Accounting: B credited the verified bytes it received under the
         * serving peers' session keys (two keys in the disconnect run —
         * the work moved from A to A2 mid-download); A credited the
         * verified bytes it served to B's session key. The manifest wire
         * is verified+credited too, so the credit sits in [chunks,
         * chunks + manifest wire]. */
        {
            uint64_t credited = 0;
            uint8_t key[33];
            ASSERT(zwn_peer_key(b_a.node, key));
            struct vcs_service_key_totals kt;
            ASSERT(vcs_service_key_totals(b.book, key, ZWN_DAY, &kt));
            credited += kt.verified_bytes_downloaded;
            if (two_servers) {
                ASSERT(zwn_peer_key(b_a2.node, key));
                ASSERT(vcs_service_key_totals(b.book, key, ZWN_DAY, &kt));
                credited += kt.verified_bytes_downloaded;
            }
            ASSERT(credited >= pkg.total_bytes);
            ASSERT(credited <= pkg.total_bytes + pkg.wire_len);
        }
        {
            uint64_t served = 0;
            uint8_t key[33];
            ASSERT(zwn_peer_key(a_b.node, key));
            struct vcs_service_key_totals kt;
            ASSERT(vcs_service_key_totals(a.book, key, ZWN_DAY, &kt));
            served += kt.verified_bytes_uploaded;
            if (two_servers) {
                ASSERT(zwn_peer_key(a2_b.node, key));
                ASSERT(vcs_service_key_totals(a2.book, key, ZWN_DAY, &kt));
                served += kt.verified_bytes_uploaded;
            }
            ASSERT(served >= pkg.total_bytes);
            /* Non-plain runs discard answered-but-undelivered DATAs with
             * the drained socket queues, so the surviving server
             * genuinely re-serves those chunks (upload credit is
             * recorded at serve time, against the request id — that is
             * honest); the slack bound is the worst-case in-flight set
             * at the drain. The plain run keeps the exact bound. */
            uint64_t slack = mode == ZWN_GOLDEN_PLAIN
                                 ? 0
                                 : VCS_SWARM_PEER_INFLIGHT_MAX *
                                       ZWN_MAX_FILE;
            ASSERT(served <= pkg.total_bytes + pkg.wire_len + slack);
        }

        zwn_link_free(&a_b);
        zwn_link_free(&b_a);
        if (two_servers) {
            zwn_link_free(&a2_b);
            zwn_link_free(&b_a2);
            zwn_node_free(&a2);
        }
        zwn_node_free(&a);
        zwn_node_free(&b);
        zwn_free_package(&pkg);
        PASS();
    } _test_next:;

    return failures;
}

/* ── proven source carrier: peer-only checkout + reproducible build ── */

static int zwn_t_sovereign_source_build(const struct chain_params *params)
{
    int failures = 0;
    TEST("proven C23 source: two providers feed a fresh Git-free checkout "
         "whose rebuilt binary matches the publisher SHA3") {
        char publisher[512], consumer_cas[512], checkout[512], checkout2[512];
        char publisher_build[512], consumer_build[512], consumer_build2[512];
        test_make_tmpdir(publisher, sizeof(publisher),
                         "zcode_swarm_net", "source-publisher");
        test_make_tmpdir(consumer_cas, sizeof(consumer_cas),
                         "zcode_swarm_net", "source-consumer-cas");
        test_make_tmpdir(checkout, sizeof(checkout),
                         "zcode_swarm_net", "source-checkout");
        test_make_tmpdir(checkout2, sizeof(checkout2),
                         "zcode_swarm_net", "source-checkout2");
        test_make_tmpdir(publisher_build, sizeof(publisher_build),
                         "zcode_swarm_net", "source-publisher-build");
        test_make_tmpdir(consumer_build, sizeof(consumer_build),
                         "zcode_swarm_net", "source-consumer-build");
        test_make_tmpdir(consumer_build2, sizeof(consumer_build2),
                         "zcode_swarm_net", "source-consumer-build2");
        char path[1400];
        ASSERT(snprintf(path, sizeof(path), "%s/src", publisher) > 0);
        ASSERT(mkdir(path, 0700) == 0);
        ASSERT(snprintf(path, sizeof(path), "%s/vendor", publisher) > 0);
        ASSERT(mkdir(path, 0700) == 0);
        ASSERT(snprintf(path, sizeof(path), "%s/vendor/.cache", publisher) >
               0);
        ASSERT(mkdir(path, 0700) == 0);
        static const char license[] = "Apache-2.0\n";
        static const char program[] =
            "#include <stdint.h>\n"
            "static uint32_t mix(uint32_t x) { return (x << 5) ^ (x >> 3); }\n"
            "int main(void) { return mix(UINT32_C(23)) == 738 ? 0 : 1; }\n";
        ASSERT(zwn_write_file(publisher, "LICENSE", license,
                              sizeof(license) - 1u, 0644));
        ASSERT(zwn_write_file(publisher, "src/main.c", program,
                              sizeof(program) - 1u, 0644));
        static const char offline[] = "hermetic-offline-input\n";
        for (size_t i = 0;
             i < vcs_source_package_offline_input_count(); i++)
            ASSERT(zwn_write_file(
                publisher, vcs_source_package_offline_input_path(i),
                offline, sizeof(offline) - 1u, 0600));

        char publisher_source[1400], publisher_binary[1400];
        ASSERT(snprintf(publisher_source, sizeof(publisher_source),
                        "%s/src/main.c", publisher) > 0);
        ASSERT(snprintf(publisher_binary, sizeof(publisher_binary),
                        "%s/program", publisher_build) > 0);
        ASSERT(zwn_compile_c23(publisher_source, publisher_binary));
        ASSERT(zwn_run_fixture_binary(publisher_binary));
        uint8_t publisher_sha3[32], publisher_sha256[32];
        uint8_t initial_publisher_sha3[32];
        ASSERT(zwn_sha3_file(publisher_binary, publisher_sha3));
        memcpy(initial_publisher_sha3, publisher_sha3, 32);
        ASSERT(zwn_sha256_file(publisher_binary, publisher_sha256));

        uint8_t source_root[32];
        ASSERT(vcs_tree_capture_path(publisher, source_root) == VCS_OK);
        const int64_t accepted_now = 1700000000;
        struct test_accepted_work_fixture accepted;
        ASSERT(test_accepted_work_fixture_create(
            publisher, source_root, accepted_now, 0x66, &accepted));
        struct vcs_zcode_accepted_work_v1 resolved_accepted;
        ASSERT(vcs_zcode_accepted_work_resolve(
            publisher, accepted.accepted.accepted_work_root, accepted_now,
            &resolved_accepted));
        struct vcs_source_package_transport transport;
        vcs_source_package_transport_init(&transport);
        ASSERT(vcs_source_package_transport_build_accepted(
            publisher, source_root, accepted.accepted.accepted_work_root,
            accepted_now, &transport));

        char source_hex[65], package_hex[65], generation_hex[65];
        zcl_hex_encode(source_root, 32, source_hex);
        zcl_hex_encode(transport.package_root, 32, package_hex);
        zcl_hex_encode(publisher_sha256, 32, generation_hex);
        struct vcs_devloop_verdict dev_verdict = {
            .verdict_status = 0,
            .phase = "verify",
            .elapsed_ms = 23,
            .generation_hex = generation_hex,
            .agent_id = "fixture-agent",
            .session_id = "sovereign-roundtrip",
            .task_ref = "c23/source",
            .proof_complete = true,
            .proof_scope = "source_wide_compile_tests_lint_fast",
            .source_identity_hex = source_hex,
            .source_cas_hex = package_hex,
        };
        struct vcs_devloop_anchor_result publication_anchor = {0};
        vcs_devloop_anchor_cycle(
            publisher, &dev_verdict, &publication_anchor);
        ASSERT(publication_anchor.status == VCS_DEVLOOP_ANCHOR_OK);
        ASSERT(publication_anchor.publication_status ==
               VCS_DEVLOOP_PUBLICATION_QUEUED);
        ASSERT(publication_anchor.publication_enqueue_us >= 0);
        uint8_t commit_root[32];
        memcpy(commit_root, publication_anchor.commit_id, 32);
        struct vcs_devloop_publication_job publication_job;
        ASSERT(vcs_devloop_publication_job_load(
            publisher, publication_anchor.publication_job_root,
            &publication_job));
        ASSERT(memcmp(publication_job.vcs_commit_root, commit_root, 32) == 0);
        ASSERT(memcmp(publication_job.source_tree_root, source_root, 32) == 0);
        uint8_t publication_progress_root[32];
        bool publication_reused = true;
        ASSERT(vcs_devloop_publication_advance_waiting_acceptance(
            publisher, publication_anchor.publication_job_root,
            publication_progress_root, &publication_reused));
        ASSERT(!publication_reused);
        ASSERT(vcs_devloop_publication_advance_proven_work(
            publisher, publication_anchor.publication_job_root,
            accepted.accepted.accepted_work_root, accepted_now,
            publication_progress_root, &publication_reused));
        ASSERT(!publication_reused);
        struct vcs_package_mapping_metrics publication_metrics;
        uint8_t publication_mapping_root[32];
        ASSERT(vcs_package_mapping_set_build(
            publisher, source_root, accepted.accepted.accepted_work_root,
            &publication_metrics, publication_mapping_root));
        ASSERT(vcs_devloop_publication_advance_package_mapping(
            publisher, publication_anchor.publication_job_root,
            publication_mapping_root, publication_metrics.bytes_scanned,
            publication_metrics.new_chunks, publication_metrics.reused_chunks,
            publication_progress_root, &publication_reused));
        ASSERT(!publication_reused);
        uint8_t *commit_wire = NULL;
        size_t commit_wire_len = 0;
        ASSERT(vcs_object_get(
                   publisher, commit_root, VCS_TAG_COMMIT,
                   &commit_wire, &commit_wire_len) == 0);
        ASSERT(commit_wire_len == VCS_COMMIT_PREIMAGE_BYTES);
        struct vcs_commit publisher_commit;
        ASSERT(vcs_commit_parse_preimage(
            commit_wire, commit_wire_len, &publisher_commit));
        ASSERT(memcmp(publisher_commit.tree_hash, source_root, 32) == 0);
        ASSERT(memcmp(publisher_commit.generation_sha256,
                      publisher_sha256, 32) == 0);

        uint8_t license_root[32], author_binding_root[32];
        sha3_256((const uint8_t *)license, sizeof(license) - 1u,
                 license_root);
        zwn_literal_root("fixture/author", author_binding_root);
        struct vcs_zcode_source_assignment_v1 assignment = {
            .schema_version = 1,
            .flags = VCS_ZCODE_C23_CORPUS_REQUIRED_FLAGS,
            .source_kind = VCS_ZCODE_SOURCE_AI_AUTHORED,
            .sequence = 1,
            .assigned_height = 101,
            .assigned_mtp = accepted_now,
        };
        memcpy(assignment.source_root, source_root, 32);
        memcpy(assignment.author_binding_root, author_binding_root, 32);
        memcpy(assignment.license_root, license_root, 32);
        memcpy(assignment.assignment_evidence_root,
               accepted.accepted.accepted_work_root, 32);
        uint8_t assignment_seed[32];
        memset(assignment_seed, 0x91, sizeof(assignment_seed));
        ASSERT(vcs_zcode_source_assignment_v1_sign(
                   &assignment, assignment_seed) == VCS_ZCODE_C23_OK);
        uint8_t assignment_wire[VCS_ZCODE_SOURCE_ASSIGNMENT_WIRE_BYTES];
        size_t assignment_wire_len = 0;
        uint8_t assignment_root[32];
        ASSERT(vcs_zcode_source_assignment_v1_encode(
                   &assignment, assignment_wire, sizeof(assignment_wire),
                   &assignment_wire_len) == VCS_ZCODE_C23_OK);
        ASSERT(vcs_zcode_source_assignment_v1_root(
                   &assignment, assignment_root) == VCS_ZCODE_C23_OK);

        struct vcs_package_release first_release;
        uint8_t first_release_root[32];
        ASSERT(zwn_release_make(
            transport.package_root, transport.recipe_root, "1.0.0", 1,
            NULL, &first_release, first_release_root));
        uint8_t *first_release_wire = NULL;
        size_t first_release_wire_len = 0;
        ASSERT(vcs_package_release_serialize(
                   &first_release, &first_release_wire,
                   &first_release_wire_len) == VCS_PACKAGE_RELEASE_OK);
        ASSERT(vcs_object_put_addressed(
            publisher, first_release_root, first_release_wire,
            first_release_wire_len));
        ASSERT(vcs_devloop_publication_advance_release(
            publisher, publication_anchor.publication_job_root,
            publication_mapping_root, first_release_root,
            publication_progress_root, &publication_reused));
        ASSERT(!publication_reused);

        struct vcs_zcode_module_passport_v1 passport = {
            .schema_version = 1,
            .flags = VCS_ZCODE_COMMONS_V2_REQUIRED_FLAGS,
        };
        zwn_literal_root("fixture/sovereign-source-api-v1",
                         passport.stable_api_root);
        memcpy(passport.recipe_root, transport.recipe_root, 32);
        memcpy(passport.toolchain_root,
               accepted.accepted.task.toolchain_capsule_root, 32);
        memcpy(passport.tests_root, accepted.accepted.proof_set_root, 32);
        memcpy(passport.license_root, license_root, 32);
        memcpy(passport.semantic_fingerprint_root, source_root, 32);
        memcpy(passport.workspace_lineage_root, commit_root, 32);
        memcpy(passport.source_assignment_root, assignment_root, 32);
        zwn_literal_root("fixture/universal-quality-v1",
                         passport.quality_profiles_root);
        uint8_t passport_seed[32];
        memset(passport_seed, 0x92, sizeof(passport_seed));
        ASSERT(vcs_zcode_module_passport_v1_sign(
                   &passport, passport_seed) == VCS_ZCODE_COMMONS_V2_OK);
        uint8_t passport_wire[VCS_ZCODE_MODULE_PASSPORT_V1_WIRE_BYTES];
        size_t passport_wire_len = 0;
        uint8_t passport_root[32];
        ASSERT(vcs_zcode_module_passport_v1_encode(
                   &passport, passport_wire, sizeof(passport_wire),
                   &passport_wire_len) == VCS_ZCODE_COMMONS_V2_OK);
        ASSERT(vcs_zcode_module_passport_v1_root(
                   &passport, passport_root) == VCS_ZCODE_COMMONS_V2_OK);
        ASSERT(vcs_object_put_addressed(
            publisher, passport_root, passport_wire, passport_wire_len));
        ASSERT(vcs_devloop_publication_advance_passport(
            publisher, publication_anchor.publication_job_root,
            publication_mapping_root, first_release_root, passport_root,
            publication_progress_root, &publication_reused));
        ASSERT(!publication_reused);

        struct vcs_zcode_workspace_entry_v1 workspace_entry = {
            .sequence = 1,
        };
        memcpy(workspace_entry.module_release_root,
               first_release_root, 32);
        memcpy(workspace_entry.module_passport_root, passport_root, 32);
        memcpy(workspace_entry.semantic_fingerprint_root,
               passport.semantic_fingerprint_root, 32);
        memcpy(workspace_entry.source_assignment_root,
               assignment_root, 32);
        struct vcs_zcode_workspace_manifest_v1 workspace_manifest = {
            .schema_version = 1,
            .flags = VCS_ZCODE_COMMONS_V2_REQUIRED_FLAGS,
            .sequence = 1,
            .entries = &workspace_entry,
            .entry_count = 1,
        };
        uint8_t workspace_seed[32], workspace_secret[32];
        memset(workspace_seed, 0x93, sizeof(workspace_seed));
        ed25519_keypair(workspace_manifest.signer_root,
                        workspace_secret, workspace_seed);
        uint8_t workspace_payload[
            VCS_ZCODE_WORKSPACE_MANIFEST_V1_SIGNING_PAYLOAD_BYTES];
        size_t workspace_payload_len = 0;
        ASSERT(vcs_zcode_workspace_manifest_v1_signing_payload(
                   &workspace_manifest, workspace_payload,
                   sizeof(workspace_payload), &workspace_payload_len) ==
               VCS_ZCODE_COMMONS_V2_OK);
        ed25519_sign(workspace_manifest.signature, workspace_payload,
                     workspace_payload_len, workspace_secret,
                     workspace_manifest.signer_root);
        memset(workspace_secret, 0, sizeof(workspace_secret));
        ASSERT(vcs_zcode_workspace_manifest_v1_verify(
                   &workspace_manifest) == VCS_ZCODE_COMMONS_V2_OK);
        size_t workspace_wire_size = 0, workspace_wire_len = 0;
        ASSERT(vcs_zcode_workspace_manifest_v1_wire_size(
                   &workspace_manifest, &workspace_wire_size) ==
               VCS_ZCODE_COMMONS_V2_OK);
        uint8_t *workspace_wire = zcl_malloc(
            workspace_wire_size, "zwn.workspace_wire");
        ASSERT(workspace_wire != NULL);
        ASSERT(vcs_zcode_workspace_manifest_v1_encode(
                   &workspace_manifest, workspace_wire,
                   workspace_wire_size, &workspace_wire_len) ==
               VCS_ZCODE_COMMONS_V2_OK);
        uint8_t workspace_root[32];
        ASSERT(vcs_zcode_workspace_manifest_v1_root(
                   &workspace_manifest, workspace_root) ==
               VCS_ZCODE_COMMONS_V2_OK);
        ASSERT(vcs_object_put_addressed(
            publisher, workspace_root, workspace_wire, workspace_wire_len));
        ASSERT(vcs_devloop_publication_advance_workspace(
            publisher, publication_anchor.publication_job_root,
            publication_mapping_root, first_release_root, passport_root,
            workspace_root, publication_progress_root,
            &publication_reused));
        ASSERT(!publication_reused);
        struct vcs_devloop_publication_receipt publication_progress;
        uint8_t loaded_publication_progress_root[32];
        ASSERT(vcs_devloop_publication_progress_load(
            publisher, publication_anchor.publication_job_root,
            &publication_progress, loaded_publication_progress_root));
        ASSERT(publication_progress.phase ==
               VCS_DEVLOOP_PUBLICATION_PHASE_WORKSPACE_PUBLISHED);
        ASSERT(memcmp(publication_progress.artifact_root,
                      workspace_root, 32) == 0);
        ASSERT(memcmp(publication_progress_root,
                      loaded_publication_progress_root, 32) == 0);
        publication_reused = false;
        ASSERT(vcs_devloop_publication_advance_workspace(
            publisher, publication_anchor.publication_job_root,
            publication_mapping_root, first_release_root, passport_root,
            workspace_root, publication_progress_root,
            &publication_reused));
        ASSERT(publication_reused);

        const uint8_t *evidence_wires[5] = {
            passport_wire, first_release_wire, assignment_wire,
            workspace_wire, commit_wire,
        };
        const size_t evidence_lengths[5] = {
            passport_wire_len, first_release_wire_len, assignment_wire_len,
            workspace_wire_len, commit_wire_len,
        };
        struct zwn_pkg workspace_carrier;
        ASSERT(zwn_make_evidence_package(
            &workspace_carrier, evidence_wires, evidence_lengths));

        struct zwn_node a, b, a2;
        ASSERT(zwn_node_init(&a, "sa", params));
        ASSERT(zwn_node_init(&a2, "sa2", params));
        ASSERT(zwn_store_source_transport(a.store, &transport));
        ASSERT(vcs_package_store_pin(
                   a.store, transport.package_root, true) ==
               VCS_PACKAGE_STORE_OK);
        ASSERT(zwn_store_package(a.store, &workspace_carrier));
        ASSERT(vcs_package_store_pin(
                   a.store, workspace_carrier.root, true) ==
               VCS_PACKAGE_STORE_OK);
        struct zwn_link a_a2, a2_a;
        ASSERT(zwn_link(&a, &a_a2, 10, 1, 1, 2, "source-server-b"));
        ASSERT(zwn_link(&a2, &a2_a, 10, 1, 1, 1, "source-server-a"));
        ASSERT(zwn_meet_side(&a, &a_a2));
        ASSERT(zwn_meet_side(&a2, &a2_a));
        enum vcs_swarm_download_state state = VCS_SWARM_DL_INACTIVE;
        bool terminal = false;
        ASSERT(zwn_fetch_package_from_peers(
            &a2, transport.package_root, &a_a2, &a2_a, NULL, NULL,
            params->pchMessageStart));
        ASSERT(vcs_package_store_pin(
                   a2.store, transport.package_root, true) ==
               VCS_PACKAGE_STORE_OK);
        ASSERT(zwn_fetch_package_from_peers(
            &a2, workspace_carrier.root, &a_a2, &a2_a,
            NULL, NULL, params->pchMessageStart));
        ASSERT(vcs_package_store_pin(
                   a2.store, workspace_carrier.root, true) ==
               VCS_PACKAGE_STORE_OK);
        struct vcs_package_store_status server_a_status, server_b_status;
        ASSERT(vcs_package_store_package_status(
                   a.store, transport.package_root, &server_a_status));
        ASSERT(vcs_package_store_package_status(
                   a2.store, transport.package_root, &server_b_status));
        ASSERT(server_a_status.complete && server_a_status.pinned);
        ASSERT(server_b_status.complete && server_b_status.pinned);
        ASSERT(vcs_package_store_package_status(
            a.store, workspace_carrier.root, &server_a_status));
        ASSERT(vcs_package_store_package_status(
            a2.store, workspace_carrier.root, &server_b_status));
        ASSERT(server_a_status.complete && server_a_status.pinned);
        ASSERT(server_b_status.complete && server_b_status.pinned);
        vcs_swarm_engine_peer_drop(a.engine, (uint64_t)a_a2.node->id);
        vcs_swarm_engine_peer_drop(a2.engine, (uint64_t)a2_a.node->id);
        zwn_link_free(&a_a2);
        zwn_link_free(&a2_a);

        ASSERT(zwn_node_init(&b, "sb", params));
        struct zwn_link a_b, b_a, a2_b, b_a2;
        ASSERT(zwn_link(&a, &a_b, 5, 6, 7, 8, "source-b"));
        ASSERT(zwn_link(&b, &b_a, 1, 2, 3, 4, "source-a"));
        ASSERT(zwn_link(&a2, &a2_b, 9, 9, 9, 9, "source-b"));
        ASSERT(zwn_link(&b, &b_a2, 8, 8, 8, 8, "source-a2"));
        ASSERT(zwn_meet_side(&a, &a_b));
        ASSERT(zwn_meet_side(&b, &b_a));
        ASSERT(zwn_meet_side(&a2, &a2_b));
        ASSERT(zwn_meet_side(&b, &b_a2));

        uint8_t received_wire[2048];
        size_t received_wire_len = 0;
        uint8_t discovered_workspace_carrier[32];
        ASSERT(zwn_discover_transport(
            &a, &a2, &b, "zcode.workspace", workspace_root,
            workspace_carrier.root, discovered_workspace_carrier, NULL,
            1002));
        ASSERT(memcmp(discovered_workspace_carrier,
                      workspace_carrier.root, 32) == 0);
        ASSERT(zwn_fetch_package_from_peers(
            &b, discovered_workspace_carrier, &a_b, &b_a,
            &a2_b, &b_a2, params->pchMessageStart));
        ASSERT(zwn_read_package_file(
            b.store, discovered_workspace_carrier,
            ZWN_EVIDENCE_WORKSPACE, received_wire,
            sizeof(received_wire), &received_wire_len));
        struct vcs_zcode_workspace_manifest_v1_decoded consumer_workspace =
            {0};
        ASSERT(vcs_zcode_workspace_manifest_v1_decode(
                   &consumer_workspace, received_wire,
                   received_wire_len) == VCS_ZCODE_COMMONS_V2_OK);
        uint8_t checked_root[32];
        ASSERT(vcs_zcode_workspace_manifest_v1_root(
                   &consumer_workspace.manifest, checked_root) ==
               VCS_ZCODE_COMMONS_V2_OK);
        ASSERT(memcmp(checked_root, workspace_root, 32) == 0);
        ASSERT(consumer_workspace.manifest.entry_count == 1);
        struct vcs_zcode_workspace_entry_v1 consumer_entry =
            consumer_workspace.manifest.entries[0];
        vcs_zcode_workspace_manifest_v1_decoded_free(&consumer_workspace);

        ASSERT(zwn_read_package_file(
            b.store, discovered_workspace_carrier,
            ZWN_EVIDENCE_RELEASE, received_wire,
            sizeof(received_wire), &received_wire_len));
        struct vcs_package_release consumer_release;
        ASSERT(vcs_package_release_parse(
                   received_wire, received_wire_len, &consumer_release) ==
               VCS_PACKAGE_RELEASE_OK);
        ASSERT(vcs_package_release_verify(&consumer_release) ==
               VCS_PACKAGE_RELEASE_OK);
        ASSERT(vcs_package_release_id(
                   &consumer_release, checked_root) ==
               VCS_PACKAGE_RELEASE_OK);
        ASSERT(memcmp(checked_root,
                      consumer_entry.module_release_root, 32) == 0);

        ASSERT(zwn_read_package_file(
            b.store, discovered_workspace_carrier,
            ZWN_EVIDENCE_PASSPORT, received_wire,
            sizeof(received_wire), &received_wire_len));
        struct vcs_zcode_module_passport_v1 consumer_passport;
        ASSERT(vcs_zcode_module_passport_v1_decode(
                   &consumer_passport, received_wire,
                   received_wire_len) == VCS_ZCODE_COMMONS_V2_OK);
        ASSERT(vcs_zcode_module_passport_v1_root(
                   &consumer_passport, checked_root) ==
               VCS_ZCODE_COMMONS_V2_OK);
        ASSERT(memcmp(checked_root,
                      consumer_entry.module_passport_root, 32) == 0);
        ASSERT(memcmp(consumer_passport.recipe_root,
                      consumer_release.recipe_root, 32) == 0);
        ASSERT(memcmp(consumer_passport.semantic_fingerprint_root,
                      consumer_entry.semantic_fingerprint_root, 32) == 0);
        ASSERT(memcmp(consumer_passport.source_assignment_root,
                      consumer_entry.source_assignment_root, 32) == 0);

        ASSERT(zwn_read_package_file(
            b.store, discovered_workspace_carrier,
            ZWN_EVIDENCE_ASSIGNMENT, received_wire,
            sizeof(received_wire), &received_wire_len));
        struct vcs_zcode_source_assignment_v1 consumer_assignment;
        ASSERT(vcs_zcode_source_assignment_v1_decode(
                   &consumer_assignment, received_wire,
                   received_wire_len) == VCS_ZCODE_C23_OK);
        ASSERT(vcs_zcode_source_assignment_v1_root(
                   &consumer_assignment, checked_root) ==
               VCS_ZCODE_C23_OK);
        ASSERT(memcmp(checked_root,
                      consumer_passport.source_assignment_root, 32) == 0);
        ASSERT(memcmp(consumer_assignment.license_root,
                      consumer_passport.license_root, 32) == 0);

        ASSERT(zwn_read_package_file(
            b.store, discovered_workspace_carrier,
            ZWN_EVIDENCE_COMMIT, received_wire,
            sizeof(received_wire), &received_wire_len));
        struct vcs_commit consumer_commit;
        ASSERT(vcs_commit_parse_preimage(
            received_wire, received_wire_len, &consumer_commit));
        ASSERT(vcs_commit_id(&consumer_commit, checked_root));
        ASSERT(memcmp(checked_root,
                      consumer_passport.workspace_lineage_root, 32) == 0);
        ASSERT(memcmp(consumer_commit.tree_hash,
                      consumer_assignment.source_root, 32) == 0);

        uint8_t discovered_package_root[32];
        struct vcs_zcode_dht_record provider_record;
        ASSERT(zwn_discover_transport(
            &a, &a2, &b, "zcode.source",
            consumer_assignment.source_root, consumer_release.package_root,
            discovered_package_root, &provider_record, 1010));
        ASSERT(memcmp(discovered_package_root,
                      consumer_release.package_root, 32) == 0);
        uint8_t provider_wire[VCS_ZCODE_DHT_RECORD_WIRE_BYTES];
        ASSERT(vcs_zcode_dht_record_encode(
                   &provider_record, provider_wire) ==
               VCS_ZCODE_DHT_RECORD_OK);
        uint8_t provider_record_root[32];
        ASSERT(vcs_zcode_dht_record_id(
                   &provider_record, provider_record_root) ==
               VCS_ZCODE_DHT_RECORD_OK);
        struct vcs_zcode_dht_record_verify_context provider_verify;
        memset(&provider_verify, 0, sizeof(provider_verify));
        memset(provider_verify.network_genesis, 0x71,
               sizeof(provider_verify.network_genesis));
        provider_verify.now_unix = 1011;
        provider_verify.chain_verify = zwn_dht_chain_ok;
        uint8_t provider_progress_root[32];
        bool provider_progress_reused = true;
        ASSERT(vcs_devloop_publication_advance_provider(
            publisher, publication_anchor.publication_job_root,
            provider_wire, sizeof(provider_wire), &provider_verify,
            provider_progress_root, &provider_progress_reused));
        ASSERT(!provider_progress_reused);
        ASSERT(vcs_devloop_publication_progress_load(
            publisher, publication_anchor.publication_job_root,
            &publication_progress, loaded_publication_progress_root));
        ASSERT(publication_progress.phase ==
               VCS_DEVLOOP_PUBLICATION_PHASE_PROVIDER_ANNOUNCED);
        ASSERT(publication_progress.providers == 1);
        ASSERT(publication_progress.storage_acks == 0);
        ASSERT(memcmp(publication_progress_root,
                      publication_progress.predecessor_receipt_root,
                      32) == 0);
        ASSERT(memcmp(provider_progress_root,
                      loaded_publication_progress_root, 32) == 0);
        provider_progress_reused = false;
        ASSERT(vcs_devloop_publication_advance_provider(
            publisher, publication_anchor.publication_job_root,
            provider_wire, sizeof(provider_wire), &provider_verify,
            loaded_publication_progress_root,
            &provider_progress_reused));
        ASSERT(provider_progress_reused);
        ASSERT(memcmp(provider_progress_root,
                      loaded_publication_progress_root, 32) == 0);
        ASSERT(zwn_fetch_package_from_peers(
            &b, discovered_package_root, &a_b, &b_a, &a2_b, &b_a2,
            params->pchMessageStart));

        ASSERT(vcs_object_store_init(consumer_cas));
        ASSERT(access(checkout, F_OK) == 0);
        ASSERT(snprintf(path, sizeof(path), "%s/.git", checkout) > 0);
        ASSERT(access(path, F_OK) != 0 && errno == ENOENT);
        uint8_t wrong_source_root[32], refused_accepted_root[32];
        memcpy(wrong_source_root, consumer_assignment.source_root, 32);
        wrong_source_root[0] ^= 1u;
        memset(refused_accepted_root, 0xa5,
               sizeof(refused_accepted_root));
        ASSERT(vcs_source_package_accepted_work_discover(
                   b.store, discovered_package_root, wrong_source_root,
                   refused_accepted_root) ==
               VCS_SOURCE_PACKAGE_CHECKOUT_SOURCE);
        uint8_t zero_root[32] = {0};
        ASSERT(memcmp(refused_accepted_root, zero_root, 32) == 0);
        uint8_t discovered_accepted_root[32];
        ASSERT(vcs_source_package_accepted_work_discover(
                   b.store, discovered_package_root,
                   consumer_assignment.source_root,
                   discovered_accepted_root) ==
               VCS_SOURCE_PACKAGE_CHECKOUT_OK);
        ASSERT(memcmp(discovered_accepted_root,
                      accepted.accepted.accepted_work_root, 32) == 0);
        struct vcs_source_package_checkout_metrics metrics;
        ASSERT(vcs_source_package_checkout_accepted(
                   b.store, discovered_package_root,
                   consumer_assignment.source_root,
                   discovered_accepted_root, consumer_cas,
                   checkout, &metrics) ==
               VCS_SOURCE_PACKAGE_CHECKOUT_OK);
        ASSERT(metrics.authority_objects >= 9 && metrics.work_receipts == 2);
        ASSERT(metrics.source.file_count == 2);
        ASSERT(metrics.offline_input_files ==
               vcs_source_package_offline_input_count());
        ASSERT(metrics.carrier_files ==
               vcs_source_package_transport_file_count(&transport));
        struct vcs_zcode_accepted_work_v1 consumer_accepted;
        ASSERT(vcs_zcode_accepted_work_resolve(
            consumer_cas, discovered_accepted_root, accepted_now,
            &consumer_accepted));
        ASSERT(memcmp(consumer_passport.tests_root,
                      consumer_accepted.proof_set_root, 32) == 0);
        ASSERT(memcmp(consumer_passport.toolchain_root,
                      consumer_accepted.task.toolchain_capsule_root, 32) ==
               0);
        ASSERT(memcmp(consumer_assignment.assignment_evidence_root,
                      discovered_accepted_root, 32) == 0);

        char consumer_source[1400], consumer_binary[1400];
        ASSERT(snprintf(consumer_source, sizeof(consumer_source),
                        "%s/src/main.c", checkout) > 0);
        ASSERT(snprintf(consumer_binary, sizeof(consumer_binary),
                        "%s/program", consumer_build) > 0);
        struct stat publisher_stat, consumer_stat;
        ASSERT(stat(publisher_source, &publisher_stat) == 0);
        ASSERT(stat(consumer_source, &consumer_stat) == 0);
        ASSERT((publisher_stat.st_mode & 0777) ==
               (consumer_stat.st_mode & 0777));
        ASSERT(publisher_stat.st_size == consumer_stat.st_size);
        ASSERT(zwn_compile_c23(consumer_source, consumer_binary));
        ASSERT(zwn_run_fixture_binary(consumer_binary));
        uint8_t consumer_sha3[32], consumer_sha256[32];
        ASSERT(zwn_sha3_file(consumer_binary, consumer_sha3));
        ASSERT(zwn_sha256_file(consumer_binary, consumer_sha256));
        ASSERT(zwn_hashes_match("publisher_binary_sha3",
                                publisher_sha3, consumer_sha3));
        ASSERT(memcmp(consumer_commit.generation_sha256,
                      consumer_sha256, 32) == 0);

        static const char successor_program[] =
            "#include <stdint.h>\n"
            "static uint32_t mix(uint32_t x) { return (x << 5) ^ (x >> 3); }\n"
            "int main(void) { return mix(UINT32_C(24)) == 771 ? 0 : 1; }\n";
        ASSERT(unlink(publisher_source) == 0);
        ASSERT(zwn_write_file(publisher, "src/main.c", successor_program,
                              sizeof(successor_program) - 1u, 0644));
        uint8_t successor_source_root[32];
        ASSERT(vcs_tree_capture_path(publisher, successor_source_root) ==
               VCS_OK);
        struct test_accepted_work_fixture successor_accepted;
        ASSERT(test_accepted_work_fixture_create(
            publisher, successor_source_root, accepted_now + 1, 0x67,
            &successor_accepted));
        struct vcs_source_package_transport successor_transport;
        vcs_source_package_transport_init(&successor_transport);
        ASSERT(vcs_source_package_transport_build_accepted(
            publisher, successor_source_root,
            successor_accepted.accepted.accepted_work_root,
            accepted_now + 1, &successor_transport));
        ASSERT(memcmp(successor_transport.package_root,
                      transport.package_root, 32) != 0);
        ASSERT(zwn_store_source_transport(a.store, &successor_transport));
        ASSERT(vcs_package_store_pin(
                   a.store, successor_transport.package_root, true) ==
               VCS_PACKAGE_STORE_OK);

        ASSERT(zwn_store_source_transport(a2.store, &successor_transport));
        ASSERT(vcs_package_store_pin(
                   a2.store, successor_transport.package_root, true) ==
               VCS_PACKAGE_STORE_OK);
        ASSERT(zwn_node_restart_engine(&a));
        ASSERT(zwn_node_restart_engine(&a2));
        ASSERT(zwn_meet_side(&a, &a_b));
        ASSERT(zwn_meet_side(&a2, &a2_b));

        struct vcs_package_release successor_release;
        uint8_t successor_release_root[32], observed_parent[32];
        ASSERT(zwn_release_make(
            successor_transport.package_root,
            successor_transport.recipe_root, "1.0.1", 2,
            first_release_root, &successor_release,
            successor_release_root));
        ASSERT(vcs_package_release_parent(
                   &successor_release, observed_parent) &&
               memcmp(observed_parent, first_release_root, 32) == 0);
        uint8_t *successor_release_wire = NULL;
        size_t successor_release_wire_len = 0;
        ASSERT(vcs_package_release_serialize(
                   &successor_release, &successor_release_wire,
                   &successor_release_wire_len) == VCS_PACKAGE_RELEASE_OK);
        ASSERT(vcs_object_put_addressed(
            publisher, successor_release_root, successor_release_wire,
            successor_release_wire_len));
        free(successor_release_wire);

        uint8_t discovered_successor_root[32];
        ASSERT(zwn_discover_transport(
            &a, &a2, &b, "zcode.source", successor_source_root,
            successor_transport.package_root, discovered_successor_root,
            NULL, 1020));
        ASSERT(vcs_swarm_engine_fetch(
                   b.engine, discovered_successor_root, ZWN_DAY, ++b.now) ==
               VCS_SWARM_FETCH_OK);
        state = VCS_SWARM_DL_INACTIVE;
        terminal = false;
        for (int i = 0; i < 600 && !terminal; i++) {
            ASSERT(zwn_round(&a_b, &b_a, params->pchMessageStart));
            ASSERT(zwn_round(&a2_b, &b_a2, params->pchMessageStart));
            terminal = zwn_download_done(
                &b, discovered_successor_root, &state);
        }
        ASSERT(terminal && state == VCS_SWARM_DL_COMPLETE);
        uint8_t discovered_successor_accepted_root[32];
        ASSERT(vcs_source_package_accepted_work_discover(
                   b.store, discovered_successor_root,
                   successor_source_root,
                   discovered_successor_accepted_root) ==
               VCS_SOURCE_PACKAGE_CHECKOUT_OK);
        ASSERT(memcmp(discovered_successor_accepted_root,
                      successor_accepted.accepted.accepted_work_root,
                      32) == 0);
        struct vcs_source_package_checkout_metrics successor_metrics;
        ASSERT(vcs_source_package_checkout_accepted(
                   b.store, discovered_successor_root,
                   successor_source_root,
                   discovered_successor_accepted_root,
                   consumer_cas, checkout2, &successor_metrics) ==
               VCS_SOURCE_PACKAGE_CHECKOUT_OK);
        ASSERT(successor_metrics.authority_objects >= 9 &&
               successor_metrics.work_receipts == 2);
        char successor_consumer_source[1400], successor_publisher_binary[1400];
        char successor_consumer_binary[1400];
        ASSERT(snprintf(successor_consumer_source,
                        sizeof(successor_consumer_source), "%s/src/main.c",
                        checkout2) > 0);
        ASSERT(snprintf(successor_publisher_binary,
                        sizeof(successor_publisher_binary), "%s/program2",
                        publisher_build) > 0);
        ASSERT(snprintf(successor_consumer_binary,
                        sizeof(successor_consumer_binary), "%s/program",
                        consumer_build2) > 0);
        ASSERT(zwn_compile_c23(
            publisher_source, successor_publisher_binary));
        ASSERT(zwn_compile_c23(
            successor_consumer_source, successor_consumer_binary));
        ASSERT(zwn_run_fixture_binary(successor_publisher_binary));
        ASSERT(zwn_run_fixture_binary(successor_consumer_binary));
        ASSERT(zwn_sha3_file(successor_publisher_binary, publisher_sha3));
        ASSERT(zwn_sha3_file(successor_consumer_binary, consumer_sha3));
        ASSERT(zwn_hashes_match("successor_binary_sha3",
                                publisher_sha3, consumer_sha3));
        ASSERT(vcs_package_store_package_status(
                   a2.store, transport.package_root, &server_b_status));
        ASSERT(server_b_status.complete && server_b_status.pinned);
        struct vcs_zcode_dht_record storage_ack_records[2];
        uint8_t storage_ack_roots[2][32];
        ASSERT(zwn_author_storage_ack(
            &a, "zcode.source", transport.package_root, 0x81, 0x91,
            &storage_ack_records[0], storage_ack_roots[0]));
        ASSERT(zwn_author_storage_ack(
            &a2, "zcode.source", transport.package_root, 0x82, 0x92,
            &storage_ack_records[1], storage_ack_roots[1]));
        ASSERT(memcmp(storage_ack_records[0].provider_node_id,
                      storage_ack_records[1].provider_node_id, 32) != 0);
        ASSERT(memcmp(storage_ack_records[0].owner_group,
                      storage_ack_records[1].owner_group, 32) != 0);
        ASSERT(memcmp(storage_ack_roots[0], storage_ack_roots[1], 32) != 0);
        uint8_t storage_ack_wires[2][VCS_ZCODE_DHT_RECORD_WIRE_BYTES];
        const uint8_t *storage_ack_wire_ptrs[2] = {
            storage_ack_wires[0], storage_ack_wires[1],
        };
        size_t storage_ack_wire_lengths[2] = {
            VCS_ZCODE_DHT_RECORD_WIRE_BYTES,
            VCS_ZCODE_DHT_RECORD_WIRE_BYTES,
        };
        ASSERT(vcs_zcode_dht_record_encode(
                   &storage_ack_records[0], storage_ack_wires[0]) ==
               VCS_ZCODE_DHT_RECORD_OK);
        ASSERT(vcs_zcode_dht_record_encode(
                   &storage_ack_records[1], storage_ack_wires[1]) ==
               VCS_ZCODE_DHT_RECORD_OK);
        const uint8_t *duplicate_ack_wires[2] = {
            storage_ack_wires[0], storage_ack_wires[0],
        };
        uint8_t storage_ack_progress_root[32];
        bool storage_ack_progress_reused = true;
        ASSERT(!vcs_devloop_publication_advance_storage_acks(
            publisher, publication_anchor.publication_job_root,
            duplicate_ack_wires, storage_ack_wire_lengths, 2,
            &provider_verify, storage_ack_progress_root,
            &storage_ack_progress_reused));
        ASSERT(!storage_ack_progress_reused);
        ASSERT(vcs_devloop_publication_advance_storage_acks(
            publisher, publication_anchor.publication_job_root,
            storage_ack_wire_ptrs, storage_ack_wire_lengths, 2,
            &provider_verify, storage_ack_progress_root,
            &storage_ack_progress_reused));
        ASSERT(!storage_ack_progress_reused);
        ASSERT(vcs_devloop_publication_progress_load(
            publisher, publication_anchor.publication_job_root,
            &publication_progress, loaded_publication_progress_root));
        ASSERT(publication_progress.phase ==
               VCS_DEVLOOP_PUBLICATION_PHASE_STORAGE_ACKNOWLEDGED);
        ASSERT(publication_progress.providers == 2);
        ASSERT(publication_progress.storage_acks == 2);
        ASSERT(memcmp(publication_progress.predecessor_receipt_root,
                      provider_progress_root, 32) == 0);
        ASSERT(memcmp(loaded_publication_progress_root,
                      storage_ack_progress_root, 32) == 0);
        const uint8_t *reversed_ack_wires[2] = {
            storage_ack_wires[1], storage_ack_wires[0],
        };
        storage_ack_progress_reused = false;
        ASSERT(vcs_devloop_publication_advance_storage_acks(
            publisher, publication_anchor.publication_job_root,
            reversed_ack_wires, storage_ack_wire_lengths, 2,
            &provider_verify, loaded_publication_progress_root,
            &storage_ack_progress_reused));
        ASSERT(storage_ack_progress_reused);
        ASSERT(memcmp(loaded_publication_progress_root,
                      storage_ack_progress_root, 32) == 0);
        provider_progress_reused = false;
        ASSERT(vcs_devloop_publication_advance_provider(
            publisher, publication_anchor.publication_job_root,
            provider_wire, sizeof(provider_wire), &provider_verify,
            loaded_publication_progress_root, &provider_progress_reused));
        ASSERT(provider_progress_reused);
        ASSERT(memcmp(loaded_publication_progress_root,
                      storage_ack_progress_root, 32) == 0);

        memset(&g_zwn_sovereign_receipt, 0,
               sizeof(g_zwn_sovereign_receipt));
        g_zwn_sovereign_receipt.ready = true;
        memcpy(g_zwn_sovereign_receipt.source_root, source_root, 32);
        memcpy(g_zwn_sovereign_receipt.accepted_work_root,
               accepted.accepted.accepted_work_root, 32);
        memcpy(g_zwn_sovereign_receipt.proof_set_root,
               accepted.accepted.proof_set_root, 32);
        memcpy(g_zwn_sovereign_receipt.commit_root, commit_root, 32);
        memcpy(g_zwn_sovereign_receipt.release_root,
               first_release_root, 32);
        memcpy(g_zwn_sovereign_receipt.passport_root, passport_root, 32);
        memcpy(g_zwn_sovereign_receipt.workspace_root, workspace_root, 32);
        memcpy(g_zwn_sovereign_receipt.workspace_carrier_root,
               workspace_carrier.root, 32);
        memcpy(g_zwn_sovereign_receipt.source_package_root,
               transport.package_root, 32);
        memcpy(g_zwn_sovereign_receipt.publisher_binary_sha3,
               initial_publisher_sha3, 32);
        memcpy(g_zwn_sovereign_receipt.successor_release_root,
               successor_release_root, 32);
        memcpy(g_zwn_sovereign_receipt.successor_package_root,
               successor_transport.package_root, 32);
        memcpy(g_zwn_sovereign_receipt.successor_binary_sha3,
               publisher_sha3, 32);
        memcpy(g_zwn_sovereign_receipt.storage_ack_roots,
               storage_ack_roots, sizeof(storage_ack_roots));
        g_zwn_sovereign_receipt.storage_ack_count = 2;
        memcpy(g_zwn_sovereign_receipt.provider_record_root,
               provider_record_root, 32);
        memcpy(g_zwn_sovereign_receipt.publication_job_root,
               publication_anchor.publication_job_root, 32);
        memcpy(g_zwn_sovereign_receipt.publication_progress_root,
               storage_ack_progress_root, 32);
        g_zwn_sovereign_receipt.publication_enqueue_us =
            publication_anchor.publication_enqueue_us;
        g_zwn_sovereign_receipt.publication_metrics = publication_metrics;
        g_zwn_sovereign_receipt.source_metrics = transport.bundle_metrics;
        ASSERT(vcs_package_store_package_status(
                   a2.store, workspace_carrier.root,
                   &server_b_status));
        ASSERT(server_b_status.complete && server_b_status.pinned);

        zwn_link_free(&a_b);
        zwn_link_free(&b_a);
        zwn_link_free(&a2_b);
        zwn_link_free(&b_a2);
        zwn_node_free(&a);
        zwn_node_free(&a2);
        zwn_node_free(&b);
        zwn_free_package(&workspace_carrier);
        free(workspace_wire);
        free(first_release_wire);
        free(commit_wire);
        vcs_source_package_transport_free(&successor_transport);
        vcs_source_package_transport_free(&transport);
        test_rm_rf_recursive(publisher);
        test_rm_rf_recursive(consumer_cas);
        test_rm_rf_recursive(checkout);
        test_rm_rf_recursive(checkout2);
        test_rm_rf_recursive(publisher_build);
        test_rm_rf_recursive(consumer_build);
        test_rm_rf_recursive(consumer_build2);
        PASS();
    } _test_next:;
    return failures;
}

/* One data-driven product acceptance. The runner knows only the generic
 * package lifecycle; lib/base is scenario data and a later library can use
 * the same path without another publish/fetch/reproduce harness. */
static int zwn_t_package_lifecycle(const struct chain_params *params)
{
    int failures = 0;
    const struct zwn_package_scenario scenario = zwn_package_scenarios[0];
    TEST("parameterized signed C23 package graph: A publishes, B discovers, "
         "C and D fetch onward after A disappears") {
        struct vcs_package_prepared prepared;
        struct vcs_package_transport transport;
        struct vcs_package_prepared
            graph_prepared[ZWN_PACKAGE_SCENARIO_COUNT - 1u];
        struct vcs_package_transport
            graph_transport[ZWN_PACKAGE_SCENARIO_COUNT - 1u];
        vcs_package_transport_init(&transport);
        ASSERT(zwn_prepare_package_transport(
            &scenario, scenario.source_dir, scenario.publisher_sequence,
            scenario.expected_package_root_hex, &prepared, &transport));
        for (size_t i = 1; i < ZWN_PACKAGE_SCENARIO_COUNT; i++) {
            const struct zwn_package_scenario *item =
                &zwn_package_scenarios[i];
            vcs_package_transport_init(&graph_transport[i - 1u]);
            ASSERT(zwn_prepare_package_transport(
                item, item->source_dir, item->publisher_sequence,
                item->expected_package_root_hex, &graph_prepared[i - 1u],
                &graph_transport[i - 1u]));
        }

        struct zwn_node a, b, c, d;
        ASSERT(zwn_node_init(&a, "pkg-a", params));
        ASSERT(zwn_node_init(&b, "pkg-b", params));
        ASSERT(zwn_node_init(&c, "pkg-c", params));
        ASSERT(zwn_node_init(&d, "pkg-d", params));
        ASSERT(vcs_package_transport_store(
                   a.store, &transport, scenario.source_dir) ==
               VCS_PACKAGE_TRANSPORT_OK);
        ASSERT(vcs_package_store_pin(a.store, transport.transport_root,
                                     true) == VCS_PACKAGE_STORE_OK);
        for (size_t i = 1; i < ZWN_PACKAGE_SCENARIO_COUNT; i++) {
            ASSERT(vcs_package_transport_store(
                       a.store, &graph_transport[i - 1u],
                       zwn_package_scenarios[i].source_dir) ==
                   VCS_PACKAGE_TRANSPORT_OK);
            ASSERT(vcs_package_store_pin(
                       a.store, graph_transport[i - 1u].transport_root,
                       true) == VCS_PACKAGE_STORE_OK);
        }

        struct vcs_package_store_status status;
        ASSERT(!vcs_package_store_package_status(
            b.store, transport.transport_root, &status));
        ASSERT(!vcs_package_store_package_status(
            b.store, transport.package_root, &status));

        struct zwn_link a_b, b_a;
        ASSERT(zwn_link(&a, &a_b, 10, 0, 0, 1, "package-b"));
        ASSERT(zwn_link(&b, &b_a, 10, 0, 0, 2, "package-a"));
        ASSERT(zwn_meet_side_quiet(&a, &a_b));
        ASSERT(zwn_meet_side_quiet(&b, &b_a));

        uint8_t discovered[32];
        struct vcs_zcode_dht_record provider;
        ASSERT(zwn_discover_transport(
            &a, &a, &b, scenario.dht_namespace, transport.release_id,
            transport.transport_root, discovered, &provider, 1100));
        ASSERT(memcmp(discovered, transport.transport_root, 32) == 0);
        ASSERT(zwn_fetch_package_from_provider(
            &b, discovered, &a_b, &b_a, params->pchMessageStart));
        struct vcs_swarm_download_status cold;
        memset(&cold, 0, sizeof(cold));
        ASSERT(vcs_swarm_engine_download_status(
            b.engine, transport.transport_root, &cold));
        ASSERT(cold.state == VCS_SWARM_DL_COMPLETE);
        ASSERT(cold.fetched_bytes > 0);
        ASSERT(cold.requested_bytes == cold.transferred_bytes);
        ASSERT(cold.transferred_bytes == cold.fetched_bytes);
        ASSERT(cold.requested_objects == cold.transferred_objects);
        ASSERT(cold.reused_objects == 0);
        uint64_t cold_transferred_bytes = cold.transferred_bytes;
        uint64_t cold_reused_bytes = cold.reused_bytes;
        uint32_t cold_missing_objects = cold.transferred_objects;
        uint32_t cold_reused_objects = cold.reused_objects;

        /* Fetch/verify is inert. The semantic package does not exist in B's
         * store until the separately explicit import reconstructs it. */
        ASSERT(!vcs_package_store_package_status(
            b.store, transport.package_root, &status));
        struct vcs_package_transport_import imported_b;
        ASSERT(vcs_package_transport_import(
                   b.store, transport.transport_root, &imported_b) ==
               VCS_PACKAGE_TRANSPORT_OK);
        ASSERT(memcmp(imported_b.package_root,
                      transport.package_root, 32) == 0);
        ASSERT(memcmp(imported_b.recipe_root,
                      transport.recipe_root, 32) == 0);
        ASSERT(memcmp(imported_b.release_id,
                      transport.release_id, 32) == 0);
        ASSERT(imported_b.source_chunks > 0);
        ASSERT(imported_b.cas_objects_reused == imported_b.source_chunks);
        ASSERT(vcs_package_store_package_status(
            b.store, transport.package_root, &status));
        ASSERT(status.complete);
        ASSERT(vcs_package_store_pin(b.store, transport.transport_root,
                                     true) == VCS_PACKAGE_STORE_OK);
        for (size_t i = 1; i < ZWN_PACKAGE_SCENARIO_COUNT; i++) {
            const struct zwn_package_scenario *item =
                &zwn_package_scenarios[i];
            struct vcs_package_transport *item_transport =
                &graph_transport[i - 1u];
            memset(discovered, 0, sizeof(discovered));
            ASSERT(zwn_discover_transport(
                &a, &a, &b, item->dht_namespace,
                item_transport->release_id,
                item_transport->transport_root, discovered, &provider,
                1100u + (uint64_t)i * 10u));
            ASSERT(zwn_fetch_package_from_provider(
                &b, discovered, &a_b, &b_a,
                params->pchMessageStart));
            struct vcs_swarm_download_status item_cold;
            memset(&item_cold, 0, sizeof(item_cold));
            ASSERT(vcs_swarm_engine_download_status(
                b.engine, item_transport->transport_root, &item_cold));
            ASSERT(item_cold.state == VCS_SWARM_DL_COMPLETE);
            ASSERT(item_cold.requested_bytes ==
                   item_cold.transferred_bytes);
            ASSERT(item_cold.requested_objects ==
                   item_cold.transferred_objects);
            cold_transferred_bytes += item_cold.transferred_bytes;
            cold_reused_bytes += item_cold.reused_bytes;
            cold_missing_objects += item_cold.transferred_objects;
            cold_reused_objects += item_cold.reused_objects;
            ASSERT(!vcs_package_store_package_status(
                b.store, item_transport->package_root, &status));
            struct vcs_package_transport_import item_import;
            ASSERT(vcs_package_transport_import(
                       b.store, item_transport->transport_root,
                       &item_import) == VCS_PACKAGE_TRANSPORT_OK);
            ASSERT(memcmp(item_import.package_root,
                          item_transport->package_root, 32) == 0);
            ASSERT(vcs_package_store_pin(
                       b.store, item_transport->transport_root, true) ==
                   VCS_PACKAGE_STORE_OK);
        }

        /* Publisher A is genuinely removed before C begins. B is now an
         * ordinary onward provider of the entire exact package graph. */
        zwn_link_free(&a_b);
        zwn_link_free(&b_a);
        zwn_node_free(&a);

        struct zwn_link b_c, c_b;
        ASSERT(zwn_link(&b, &b_c, 10, 0, 0, 3, "package-c"));
        ASSERT(zwn_link(&c, &c_b, 10, 0, 0, 4, "package-b"));
        ASSERT(zwn_meet_side_quiet(&b, &b_c));
        ASSERT(zwn_meet_side_quiet(&c, &c_b));
        memset(discovered, 0, sizeof(discovered));
        ASSERT(zwn_discover_transport(
            &b, &b, &c, scenario.dht_namespace, transport.release_id,
            transport.transport_root, discovered, &provider, 1200));
        ASSERT(zwn_fetch_package_from_provider(
            &c, discovered, &b_c, &c_b, params->pchMessageStart));
        struct vcs_package_transport_import imported_c;
        ASSERT(vcs_package_transport_import(
                   c.store, transport.transport_root, &imported_c) ==
               VCS_PACKAGE_TRANSPORT_OK);
        ASSERT(memcmp(imported_c.package_root,
                      transport.package_root, 32) == 0);
        ASSERT(vcs_package_store_verify_possession(
            c.store, transport.transport_root, false));
        ASSERT(vcs_package_store_verify_possession(
            c.store, transport.package_root, false));
        ASSERT(vcs_package_store_pin(c.store, transport.transport_root,
                                     true) == VCS_PACKAGE_STORE_OK);

        /* Exact repeat is a pure local-complete result: no WANT and no
         * payload bytes. */
        struct vcs_swarm_download_status repeat_status;
        memset(&repeat_status, 0, sizeof(repeat_status));
        ASSERT(vcs_swarm_engine_download_status(
            c.engine, transport.transport_root, &repeat_status));
        uint64_t repeat_bytes_present = repeat_status.total_bytes;
        uint32_t repeat_objects_present = repeat_status.total_chunks;
        ASSERT(vcs_swarm_engine_fetch(
                   c.engine, transport.transport_root, ZWN_DAY, ++c.now) ==
               VCS_SWARM_FETCH_ALREADY_COMPLETE);

        for (size_t i = 1; i < ZWN_PACKAGE_SCENARIO_COUNT; i++) {
            const struct zwn_package_scenario *item =
                &zwn_package_scenarios[i];
            struct vcs_package_transport *item_transport =
                &graph_transport[i - 1u];
            memset(discovered, 0, sizeof(discovered));
            ASSERT(zwn_discover_transport(
                &b, &b, &c, item->dht_namespace,
                item_transport->release_id,
                item_transport->transport_root, discovered, &provider,
                1200u + (uint64_t)i * 10u));
            ASSERT(zwn_fetch_package_from_provider(
                &c, discovered, &b_c, &c_b,
                params->pchMessageStart));
            struct vcs_package_transport_import item_import;
            ASSERT(vcs_package_transport_import(
                       c.store, item_transport->transport_root,
                       &item_import) == VCS_PACKAGE_TRANSPORT_OK);
            ASSERT(memcmp(item_import.package_root,
                          item_transport->package_root, 32) == 0);
            ASSERT(vcs_package_store_pin(
                       c.store, item_transport->transport_root, true) ==
                   VCS_PACKAGE_STORE_OK);
            memset(&repeat_status, 0, sizeof(repeat_status));
            ASSERT(vcs_swarm_engine_download_status(
                c.engine, item_transport->transport_root, &repeat_status));
            repeat_bytes_present += repeat_status.total_bytes;
            repeat_objects_present += repeat_status.total_chunks;
            ASSERT(vcs_swarm_engine_fetch(
                       c.engine, item_transport->transport_root, ZWN_DAY,
                       ++c.now) == VCS_SWARM_FETCH_ALREADY_COMPLETE);
        }

        /* Build/test is a separate local-policy step. The first pass builds
         * all ten exact roots; the repeat consumes the installed receipts
         * without compiling any package again. */
        int64_t build_clock = INT64_C(1700010000);
        size_t cold_packages_rebuilt = 0;
        ASSERT(zwn_build_original_graph(
            c.datadir, &build_clock, &cold_packages_rebuilt));
        ASSERT_EQ(cold_packages_rebuilt, ZWN_PACKAGE_SCENARIO_COUNT);
        size_t original_receipts = 0;
        ASSERT(zwn_original_receipts_present(
            c.datadir, &original_receipts));
        ASSERT_EQ(original_receipts, ZWN_PACKAGE_SCENARIO_COUNT);
        size_t repeat_packages_rebuilt = 0;
        ASSERT(zwn_build_original_graph(
            c.datadir, &build_clock, &repeat_packages_rebuilt));
        ASSERT_EQ(repeat_packages_rebuilt, 0);

        /* C is another ordinary provider. D fetches and imports the same
         * graph without A or any repository/registry contact. */
        struct zwn_link c_d, d_c;
        ASSERT(zwn_link(&c, &c_d, 10, 0, 0, 5, "package-d"));
        ASSERT(zwn_link(&d, &d_c, 10, 0, 0, 6, "package-c"));
        ASSERT(zwn_meet_side_quiet(&c, &c_d));
        ASSERT(zwn_meet_side_quiet(&d, &d_c));
        for (size_t i = 0; i < ZWN_PACKAGE_SCENARIO_COUNT; i++) {
            const struct zwn_package_scenario *item =
                &zwn_package_scenarios[i];
            struct vcs_package_transport *item_transport = i == 0
                ? &transport : &graph_transport[i - 1u];
            memset(discovered, 0, sizeof(discovered));
            ASSERT(zwn_discover_transport(
                &c, &c, &d, item->dht_namespace,
                item_transport->release_id,
                item_transport->transport_root, discovered, &provider,
                1300u + (uint64_t)i * 10u));
            ASSERT(zwn_fetch_package_from_provider(
                &d, discovered, &c_d, &d_c,
                params->pchMessageStart));
            struct vcs_package_transport_import item_import;
            ASSERT(vcs_package_transport_import(
                       d.store, item_transport->transport_root,
                       &item_import) == VCS_PACKAGE_TRANSPORT_OK);
            ASSERT(memcmp(item_import.package_root,
                          item_transport->package_root, 32) == 0);
            ASSERT(vcs_package_store_pin(
                       d.store, item_transport->transport_root, true) ==
                   VCS_PACKAGE_STORE_OK);
            ASSERT(vcs_package_store_package_status(
                b.store, item_transport->transport_root, &status));
            ASSERT(status.complete && status.pinned);
            ASSERT(vcs_package_store_package_status(
                c.store, item_transport->transport_root, &status));
            ASSERT(status.complete && status.pinned);
            ASSERT(vcs_package_store_package_status(
                d.store, item_transport->transport_root, &status));
            ASSERT(status.complete && status.pinned);
        }

        /* D now becomes an author from a leaf package reconstructed out of
         * the decentralized graph. Editing a leaf makes the rebuild and
         * evidence-reuse accounting exact: one changed package, nine
         * unchanged packages. Checkout is the production inert primitive,
         * not a test-local materializer. */
        size_t edit_index = zwn_package_scenario_index(
            "zclassic23/presentation");
        ASSERT(edit_index > 0 && edit_index < ZWN_PACKAGE_SCENARIO_COUNT);
        const struct zwn_package_scenario *edit_scenario =
            &zwn_package_scenarios[edit_index];
        struct vcs_package_transport *edit_transport =
            &graph_transport[edit_index - 1u];
        char checkout_parent[512], checkout[640];
        test_make_tmpdir(checkout_parent, sizeof(checkout_parent),
                         "zcode_swarm_net", "package-checkout");
        int checkout_n = snprintf(checkout, sizeof(checkout), "%s/source",
                                  checkout_parent);
        ASSERT(checkout_n > 0 && (size_t)checkout_n < sizeof(checkout));
        struct vcs_package_checkout_metrics checkout_metrics;
        ASSERT(vcs_package_checkout(
                   d.store, edit_transport->package_root, checkout,
                   &checkout_metrics) == VCS_PACKAGE_CHECKOUT_OK);
        ASSERT(checkout_metrics.files > 0 && checkout_metrics.chunks > 0 &&
               checkout_metrics.bytes > 0);
        off_t source_size = 0;
        ASSERT(zwn_append_edit(checkout, "src/zclassic_brand.c",
                               "\n/* lifecycle source edit */\n",
                               &source_size));
        struct vcs_package_prepared source_prepared;
        struct vcs_package_transport source_transport;
        vcs_package_transport_init(&source_transport);
        ASSERT(zwn_prepare_package_transport(
            edit_scenario, checkout, ZWN_PACKAGE_SCENARIO_COUNT + 1u, NULL,
            &source_prepared, &source_transport));
        ASSERT(memcmp(source_transport.package_root,
                      edit_transport->package_root, 32) != 0);
        ASSERT_EQ(source_prepared.lock.count, 1);
        ASSERT(vcs_package_transport_store(
                   d.store, &source_transport, checkout) ==
               VCS_PACKAGE_TRANSPORT_OK);
        ASSERT(vcs_package_store_pin(d.store,
                                     source_transport.transport_root,
                                     true) == VCS_PACKAGE_STORE_OK);
        ASSERT(zwn_discover_transport(
            &d, &d, &c, edit_scenario->dht_namespace,
            source_transport.release_id, source_transport.transport_root,
            discovered, &provider, 1400));
        ASSERT(zwn_fetch_package_from_provider(
            &c, source_transport.transport_root, &d_c, &c_d,
            params->pchMessageStart));
        struct vcs_swarm_download_status source_edit;
        memset(&source_edit, 0, sizeof(source_edit));
        ASSERT(vcs_swarm_engine_download_status(
            c.engine, source_transport.transport_root, &source_edit));
        ASSERT(source_edit.state == VCS_SWARM_DL_COMPLETE);
        ASSERT(source_edit.requested_bytes == source_edit.transferred_bytes);
        ASSERT(source_edit.transferred_bytes == source_edit.fetched_bytes);
        ASSERT(source_edit.requested_objects ==
               source_edit.transferred_objects);
        ASSERT(source_edit.reused_objects > 0);
        ASSERT(source_edit.reused_bytes > 0);
        ASSERT(source_edit.transferred_bytes < cold_transferred_bytes);
        struct vcs_package_transport_import source_import;
        ASSERT(vcs_package_transport_import(
                   c.store, source_transport.transport_root,
                   &source_import) == VCS_PACKAGE_TRANSPORT_OK);
        size_t source_packages_rebuilt = 0;
        ASSERT(zwn_build_package(
            c.datadir, source_import.package_root, &build_clock,
            &source_packages_rebuilt));
        ASSERT_EQ(source_packages_rebuilt, 1);
        size_t source_retained_receipts = 0;
        ASSERT(zwn_original_receipts_present(
            c.datadir, &source_retained_receipts));
        ASSERT_EQ(source_retained_receipts, ZWN_PACKAGE_SCENARIO_COUNT);

        ASSERT(zwn_revert_edit(
            checkout, "src/zclassic_brand.c", source_size));
        off_t header_size = 0;
        ASSERT(zwn_append_edit(
                               checkout,
                               "include/presentation/zclassic_brand.h",
                               "\n/* lifecycle header edit */\n",
                               &header_size));
        struct vcs_package_prepared header_prepared;
        struct vcs_package_transport header_transport;
        vcs_package_transport_init(&header_transport);
        ASSERT(zwn_prepare_package_transport(
            edit_scenario, checkout, ZWN_PACKAGE_SCENARIO_COUNT + 2u, NULL,
            &header_prepared, &header_transport));
        ASSERT(memcmp(header_transport.package_root,
                      edit_transport->package_root, 32) != 0);
        ASSERT(memcmp(header_transport.package_root,
                      source_transport.package_root, 32) != 0);
        ASSERT_EQ(header_prepared.lock.count, 1);
        ASSERT(vcs_package_transport_store(
                   d.store, &header_transport, checkout) ==
               VCS_PACKAGE_TRANSPORT_OK);
        ASSERT(vcs_package_store_pin(d.store,
                                     header_transport.transport_root,
                                     true) == VCS_PACKAGE_STORE_OK);
        ASSERT(zwn_discover_transport(
            &d, &d, &c, edit_scenario->dht_namespace,
            header_transport.release_id, header_transport.transport_root,
            discovered, &provider, 1410));
        ASSERT(zwn_fetch_package_from_provider(
            &c, header_transport.transport_root, &d_c, &c_d,
            params->pchMessageStart));
        struct vcs_swarm_download_status header_edit;
        memset(&header_edit, 0, sizeof(header_edit));
        ASSERT(vcs_swarm_engine_download_status(
            c.engine, header_transport.transport_root, &header_edit));
        ASSERT(header_edit.state == VCS_SWARM_DL_COMPLETE);
        ASSERT(header_edit.requested_bytes == header_edit.transferred_bytes);
        ASSERT(header_edit.transferred_bytes == header_edit.fetched_bytes);
        ASSERT(header_edit.requested_objects ==
               header_edit.transferred_objects);
        ASSERT(header_edit.reused_objects > 0);
        ASSERT(header_edit.reused_bytes > 0);
        ASSERT(header_edit.transferred_bytes < cold_transferred_bytes);
        struct vcs_package_transport_import header_import;
        ASSERT(vcs_package_transport_import(
                   c.store, header_transport.transport_root,
                   &header_import) == VCS_PACKAGE_TRANSPORT_OK);
        size_t header_packages_rebuilt = 0;
        ASSERT(zwn_build_package(
            c.datadir, header_import.package_root, &build_clock,
            &header_packages_rebuilt));
        ASSERT_EQ(header_packages_rebuilt, 1);
        size_t header_retained_receipts = 0;
        ASSERT(zwn_original_receipts_present(
            c.datadir, &header_retained_receipts));
        ASSERT_EQ(header_retained_receipts, ZWN_PACKAGE_SCENARIO_COUNT);

        /* Exact byte revert reproduces the original semantic and transport
         * roots. C already owns it, so the swarm performs no transfer. */
        ASSERT(zwn_revert_edit(
            checkout, "include/presentation/zclassic_brand.h", header_size));
        struct vcs_package_prepared revert_prepared;
        struct vcs_package_transport revert_transport;
        vcs_package_transport_init(&revert_transport);
        ASSERT(zwn_prepare_package_transport(
            edit_scenario, checkout, edit_scenario->publisher_sequence,
            edit_scenario->expected_package_root_hex,
            &revert_prepared, &revert_transport));
        ASSERT(memcmp(revert_transport.package_root,
                      edit_transport->package_root, 32) == 0);
        ASSERT(memcmp(revert_transport.release_id,
                      edit_transport->release_id, 32) == 0);
        ASSERT(memcmp(revert_transport.transport_root,
                      edit_transport->transport_root, 32) == 0);
        struct vcs_swarm_download_status revert_status;
        memset(&revert_status, 0, sizeof(revert_status));
        ASSERT(vcs_swarm_engine_download_status(
            c.engine, revert_transport.transport_root, &revert_status));
        ASSERT(vcs_swarm_engine_fetch(
                   c.engine, revert_transport.transport_root,
                   ZWN_DAY, ++c.now) == VCS_SWARM_FETCH_ALREADY_COMPLETE);
        size_t revert_packages_rebuilt = 0;
        ASSERT(zwn_build_package(
            c.datadir, revert_transport.package_root, &build_clock,
            &revert_packages_rebuilt));
        ASSERT_EQ(revert_packages_rebuilt, 0);
        size_t revert_reusable_receipts = 0;
        ASSERT(zwn_original_receipts_present(
            c.datadir, &revert_reusable_receipts));
        ASSERT_EQ(revert_reusable_receipts, ZWN_PACKAGE_SCENARIO_COUNT);

        char package_hex[65], transport_hex[65], recipe_hex[65];
        char release_hex[65];
        zcl_hex_encode(transport.package_root, 32, package_hex);
        zcl_hex_encode(transport.transport_root, 32, transport_hex);
        zcl_hex_encode(transport.recipe_root, 32, recipe_hex);
        zcl_hex_encode(transport.release_id, 32, release_hex);
        printf("{\"schema\":\"zcl.package_graph_lifecycle.v2\","
               "\"scenario\":\"c23-commons-alpha\","
               "\"seed_package\":\"%s\",\"package_root\":\"%s\","
               "\"transport_root\":\"%s\",\"recipe_root\":\"%s\","
               "\"release_id\":\"%s\","
               "\"package_count\":%zu,\"full_node_count\":4,"
               "\"cold_objects_already_present\":%u,"
               "\"cold_missing_objects\":%u,"
               "\"cold_bytes_transferred\":%" PRIu64 ","
               "\"cold_bytes_reused\":%" PRIu64 ","
               "\"cold_manifests_changed\":%zu,"
               "\"cold_packages_rebuilt\":%zu,"
               "\"cold_prior_evidence_reusable\":0,"
               "\"repeat_objects_already_present\":%u,"
               "\"repeat_missing_objects\":0,"
               "\"repeat_bytes_transferred\":0,"
               "\"repeat_bytes_reused\":%" PRIu64 ","
               "\"repeat_manifests_changed\":0,"
               "\"repeat_packages_rebuilt\":%zu,"
               "\"repeat_prior_evidence_reusable\":%zu,"
               "\"edited_package\":\"%s\","
               "\"source_edit_objects_already_present\":%u,"
               "\"source_edit_missing_objects\":%u,"
               "\"source_edit_transferred_bytes\":%" PRIu64 ","
               "\"source_edit_reused_bytes\":%" PRIu64 ","
               "\"source_edit_manifests_changed\":1,"
               "\"source_edit_packages_rebuilt\":%zu,"
               "\"source_edit_prior_evidence_reusable\":%zu,"
               "\"header_edit_objects_already_present\":%u,"
               "\"header_edit_missing_objects\":%u,"
               "\"header_edit_transferred_bytes\":%" PRIu64 ","
               "\"header_edit_reused_bytes\":%" PRIu64 ","
               "\"header_edit_manifests_changed\":1,"
               "\"header_edit_packages_rebuilt\":%zu,"
               "\"header_edit_prior_evidence_reusable\":%zu,"
               "\"revert_objects_already_present\":%u,"
               "\"revert_missing_objects\":0,"
               "\"revert_bytes_transferred\":0,"
               "\"revert_bytes_reused\":%" PRIu64 ","
               "\"revert_manifests_changed\":0,"
               "\"revert_packages_rebuilt\":%zu,"
               "\"revert_prior_evidence_reusable\":%zu,"
               "\"publisher_disappeared\":true,"
               "\"onward_path\":\"A-B-C-D-C\","
               "\"new_author\":\"D\",\"github_contacted\":false,"
               "\"central_registry\":false,"
               "\"special_coordinator\":false,"
               "\"downloaded_c_auto_executed\":false,"
               "\"build_test_explicit_local_policy\":true}\n",
               scenario.name, package_hex, transport_hex, recipe_hex,
               release_hex,
               ZWN_PACKAGE_SCENARIO_COUNT,
               cold_reused_objects, cold_missing_objects,
               cold_transferred_bytes, cold_reused_bytes,
               ZWN_PACKAGE_SCENARIO_COUNT, cold_packages_rebuilt,
               repeat_objects_present +
                   (uint32_t)ZWN_PACKAGE_SCENARIO_COUNT,
               repeat_bytes_present,
               repeat_packages_rebuilt, original_receipts,
               edit_scenario->name,
               source_edit.reused_objects,
               source_edit.transferred_objects,
               source_edit.transferred_bytes, source_edit.reused_bytes,
               source_packages_rebuilt,
               source_retained_receipts - 1u,
               header_edit.reused_objects,
               header_edit.transferred_objects,
               header_edit.transferred_bytes, header_edit.reused_bytes,
               header_packages_rebuilt,
               header_retained_receipts - 1u,
               revert_status.total_chunks + 1u, revert_status.total_bytes,
               revert_packages_rebuilt, revert_reusable_receipts);

        test_rm_rf_recursive(checkout_parent);
        vcs_package_transport_free(&revert_transport);
        vcs_package_prepared_free(&revert_prepared);
        vcs_package_transport_free(&header_transport);
        vcs_package_prepared_free(&header_prepared);
        vcs_package_transport_free(&source_transport);
        vcs_package_prepared_free(&source_prepared);
        zwn_link_free(&c_d);
        zwn_link_free(&d_c);
        zwn_link_free(&b_c);
        zwn_link_free(&c_b);
        zwn_node_free(&b);
        zwn_node_free(&c);
        zwn_node_free(&d);
        for (size_t i = 1; i < ZWN_PACKAGE_SCENARIO_COUNT; i++) {
            vcs_package_transport_free(&graph_transport[i - 1u]);
            vcs_package_prepared_free(&graph_prepared[i - 1u]);
        }
        vcs_package_transport_free(&transport);
        vcs_package_prepared_free(&prepared);
        PASS();
    } _test_next:;
    return failures;
}

/* ── 2: the malicious server ────────────────────────────────────────── */

static int zwn_t_malicious(const struct chain_params *params)
{
    int failures = 0;
    TEST("malicious server: wrong-hash chunks -> INVALID_CHUNK offence on "
         "the real peer object, no credit, nothing stored, named failure") {
        struct zwn_pkg pkg;
        ASSERT(zwn_make_package(&pkg, 4, 0x22));

        struct zwn_node a, b;
        ASSERT(zwn_node_init(&a, "ma", params));
        ASSERT(zwn_node_init(&b, "mb", params));
        ASSERT(zwn_store_package(a.store, &pkg));
        a.tamper_chunks = true;

        struct zwn_link a_b, b_a;
        ASSERT(zwn_link(&a, &a_b, 5, 6, 7, 8, "peer-b"));
        ASSERT(zwn_link(&b, &b_a, 1, 2, 3, 4, "peer-a"));
        ASSERT(zwn_meet_side(&a, &a_b));
        ASSERT(zwn_meet_side(&b, &b_a));

        ASSERT(vcs_swarm_engine_fetch(b.engine, pkg.root, ZWN_DAY,
                                      ++b.now) == VCS_SWARM_FETCH_OK);

        enum vcs_swarm_download_state state = VCS_SWARM_DL_INACTIVE;
        bool terminal = false;
        for (int i = 0; i < 800 && !terminal; i++) {
            ASSERT(zwn_round(&a_b, &b_a, params->pchMessageStart));
            terminal = zwn_download_done(&b, pkg.root, &state);
        }

        ASSERT(terminal);
        /* The bounded-attempts rule names the failure; the peer's real
         * misbehavior score carries the typed offence (and at the
         * default 100-point threshold the auto-ban disconnected it:
         * INVALID_CHUNK weighs 50, so the second bad chunk bans). */
        ASSERT(state == VCS_SWARM_DL_FAILED);
        {
            struct vcs_swarm_download_status st;
            memset(&st, 0, sizeof(st));
            ASSERT(vcs_swarm_engine_download_status(b.engine, pkg.root,
                                                    &st));
            ASSERT(st.rule != NULL);
        }
        ASSERT(b_a.node->misbehavior >=
               peer_offence_weight(PEER_OFFENCE_INVALID_CHUNK));
        /* Verify-before-store: not one bad chunk byte reached the CAS. */
        for (size_t i = 0; i < pkg.count; i++)
            ASSERT(!vcs_package_store_chunk_present(b.store, pkg.root,
                                                    (uint32_t)i, 0));
        /* No credit for unverified bytes; the book names them. */
        {
            uint8_t key[33];
            ASSERT(zwn_peer_key(b_a.node, key));
            struct vcs_service_key_totals kt;
            ASSERT(vcs_service_key_totals(b.book, key, ZWN_DAY, &kt));
            ASSERT(kt.verified_bytes_downloaded <= pkg.wire_len);
            ASSERT(kt.no_credit_bytes > 0);
            ASSERT(kt.offence_total > 0);
        }

        zwn_link_free(&a_b);
        zwn_link_free(&b_a);
        zwn_node_free(&a);
        zwn_node_free(&b);
        zwn_free_package(&pkg);
        PASS();
    } _test_next:;
    return failures;
}

/* A completed download is only a cache of verified possession. If a later
 * read quarantines one address-mismatched CAS object, a retry must re-open
 * the completed engine slot and obtain the exact chunk from a surviving
 * provider. This is the sovereign checkout recovery path: no source-tree or
 * Git fallback is available to the buyer. */
static int zwn_t_corrupt_local_repair(const struct chain_params *params)
{
    int failures = 0;
    TEST("corrupt local chunk: quarantine then repair from the surviving "
         "provider over real zpkgswm frames") {
        struct zwn_pkg pkg;
        ASSERT(zwn_make_package(&pkg, 6, 0x2a));

        struct zwn_node a, b, a2;
        ASSERT(zwn_node_init(&a, "ca", params));
        ASSERT(zwn_node_init(&a2, "ca2", params));
        ASSERT(zwn_node_init(&b, "cb", params));
        ASSERT(zwn_store_package(a.store, &pkg));
        ASSERT(zwn_store_package(a2.store, &pkg));

        struct zwn_link a_b, b_a, a2_b, b_a2;
        ASSERT(zwn_link(&a, &a_b, 5, 6, 7, 8, "peer-b"));
        ASSERT(zwn_link(&b, &b_a, 1, 2, 3, 4, "peer-a"));
        ASSERT(zwn_link(&a2, &a2_b, 9, 9, 9, 9, "peer-b"));
        ASSERT(zwn_link(&b, &b_a2, 8, 8, 8, 8, "peer-a2"));
        ASSERT(zwn_meet_side(&a, &a_b));
        ASSERT(zwn_meet_side(&b, &b_a));
        ASSERT(zwn_meet_side(&a2, &a2_b));
        ASSERT(zwn_meet_side(&b, &b_a2));

        ASSERT(vcs_swarm_engine_fetch(b.engine, pkg.root, ZWN_DAY,
                                      ++b.now) == VCS_SWARM_FETCH_OK);
        enum vcs_swarm_download_state state = VCS_SWARM_DL_INACTIVE;
        bool terminal = false;
        for (int i = 0; i < 400 && !terminal; i++) {
            ASSERT(zwn_round(&a_b, &b_a, params->pchMessageStart));
            ASSERT(zwn_round(&a2_b, &b_a2, params->pchMessageStart));
            terminal = zwn_download_done(&b, pkg.root, &state);
        }
        ASSERT(terminal && state == VCS_SWARM_DL_COMPLETE);
        ASSERT(zwn_store_matches(b.store, &pkg));

        /* Provider A disappears after the first complete checkout. */
        atomic_store(&b_a.node->disconnect, true);
        zwn_drain_quiet(&a_b);
        zwn_drain_quiet(&b_a);
        vcs_swarm_engine_peer_drop(b.engine, (uint64_t)b_a.node->id);

        char hash_hex[65], cas_path[1400];
        zcl_hex_encode(pkg.manifest.files[0].chunk_hashes, 32, hash_hex);
        int cas_path_len = snprintf(cas_path, sizeof(cas_path),
                                    "%s/cas/sha3/%.2s/%s", b.zcode_dir,
                                    hash_hex, hash_hex);
        ASSERT(cas_path_len > 0 && (size_t)cas_path_len < sizeof(cas_path));
        FILE *corrupt = fopen(cas_path, "r+b");
        ASSERT(corrupt != NULL);
        int first = fgetc(corrupt);
        ASSERT(first != EOF);
        rewind(corrupt);
        ASSERT(fputc(first ^ 0x80, corrupt) != EOF);
        ASSERT(fclose(corrupt) == 0);
        uint8_t *rejected = NULL;
        size_t rejected_len = 0;
        ASSERT(vcs_package_store_get_chunk_at(
                   b.store, pkg.root, 0, 0, &rejected, &rejected_len) ==
               VCS_PACKAGE_STORE_ERR_CHUNK_HASH);
        ASSERT(rejected == NULL && rejected_len == 0);
        ASSERT(!vcs_package_store_chunk_present(b.store, pkg.root, 0, 0));

        ASSERT(vcs_swarm_engine_fetch(b.engine, pkg.root, ZWN_DAY,
                                      ++b.now) == VCS_SWARM_FETCH_OK);
        terminal = false;
        state = VCS_SWARM_DL_INACTIVE;
        for (int i = 0; i < 400 && !terminal; i++) {
            ASSERT(zwn_round(&a2_b, &b_a2, params->pchMessageStart));
            terminal = zwn_download_done(&b, pkg.root, &state);
        }
        ASSERT(terminal && state == VCS_SWARM_DL_COMPLETE);
        ASSERT(zwn_store_matches(b.store, &pkg));
        ASSERT(b_a2.node->misbehavior == 0);

        zwn_link_free(&a_b);
        zwn_link_free(&b_a);
        zwn_link_free(&a2_b);
        zwn_link_free(&b_a2);
        zwn_node_free(&a);
        zwn_node_free(&a2);
        zwn_node_free(&b);
        zwn_free_package(&pkg);
        PASS();
    } _test_next:;
    return failures;
}

/* ── 3: unrequested bytes ───────────────────────────────────────────── */

static int zwn_t_unrequested(const struct chain_params *params)
{
    int failures = 0;
    TEST("unrequested DATA: UNREQUESTED offence on the wire, no credit") {
        struct zwn_pkg pkg;
        ASSERT(zwn_make_package(&pkg, 2, 0x33));

        struct zwn_node a, b;
        ASSERT(zwn_node_init(&a, "ua", params));
        ASSERT(zwn_node_init(&b, "ub", params));

        struct zwn_link a_b, b_a;
        ASSERT(zwn_link(&a, &a_b, 5, 6, 7, 8, "peer-b"));
        ASSERT(zwn_link(&b, &b_a, 1, 2, 3, 4, "peer-a"));
        ASSERT(zwn_meet_side(&a, &a_b));
        ASSERT(zwn_meet_side(&b, &b_a));

        /* A hand-built DATA frame with a request id B never issued —
         * queued straight onto the wire, no engine involvement. */
        struct vcs_package_swarm_message data;
        memset(&data, 0, sizeof(data));
        data.type = VCS_PACKAGE_SWARM_DATA;
        data.body.data.object.request_id = 424242;
        data.body.data.object.object_kind = VCS_PACKAGE_SWARM_OBJECT_CHUNK;
        memcpy(data.body.data.object.package_root, pkg.root, 32);
        data.body.data.object.file_index = 0;
        data.body.data.object.chunk_index = 0;
        ASSERT(vcs_package_chunk_hash(pkg.contents[0], pkg.lens[0],
                                      data.body.data.object.expected_hash));
        data.body.data.bytes = pkg.contents[0];
        data.body.data.bytes_len = (uint32_t)pkg.lens[0];
        uint8_t frame[1024];
        size_t frame_len = 0;
        ASSERT(vcs_package_swarm_serialize(&data, frame, sizeof(frame),
                                           &frame_len));
        zwn_send(&a, a_b.node, frame, frame_len);
        ASSERT(zwn_pump(&a_b, &b_a, params->pchMessageStart));

        ASSERT(b_a.node->misbehavior ==
               peer_offence_weight(PEER_OFFENCE_UNREQUESTED));
        {
            uint8_t key[33];
            ASSERT(zwn_peer_key(b_a.node, key));
            struct vcs_service_key_totals kt;
            ASSERT(vcs_service_key_totals(b.book, key, ZWN_DAY, &kt));
            ASSERT(kt.verified_bytes_downloaded == 0);
            ASSERT(kt.offence_total > 0);
        }

        zwn_link_free(&a_b);
        zwn_link_free(&b_a);
        zwn_node_free(&a);
        zwn_node_free(&b);
        zwn_free_package(&pkg);
        PASS();
    } _test_next:;

    return failures;
}

int test_zcode_swarm_net(void)
{
    int failures = 0;
    memset(&g_zwn_sovereign_receipt, 0,
           sizeof(g_zwn_sovereign_receipt));
    chain_params_select(CHAIN_MAIN);
    const struct chain_params *params = chain_params_get();

    failures += zwn_test_golden(ZWN_GOLDEN_PLAIN, params);
    failures += zwn_test_golden(ZWN_GOLDEN_RESTART, params);
    failures += zwn_test_golden(ZWN_GOLDEN_DISCONNECT, params);
    failures += zwn_t_package_lifecycle(params);
    failures += zwn_t_sovereign_source_build(params);
    failures += zwn_t_malicious(params);
    failures += zwn_t_corrupt_local_repair(params);
    failures += zwn_t_unrequested(params);
    if (failures == 0 && g_zwn_sovereign_receipt.ready)
        zwn_print_sovereign_receipt();
    return failures;
}

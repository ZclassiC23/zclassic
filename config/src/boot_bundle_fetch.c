/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * boot_bundle_fetch.c — THE WELD. See config/boot_bundle_fetch.h for the
 * contract. Downloads (never installs) a content-verified consensus-state
 * checkpoint bundle into <datadir>/bundles/ so the already-wired autodetect
 * installs it under the compiled CHECKPOINT_ROM authority. Fail-closed on the
 * bytes (per-chunk / whole-file SHA3), fail-open on the wiring (any miss leaves
 * boot on its unchanged path). Consumes net/rom_fetch.h; edits none of it. */

#include "config/boot_bundle_fetch.h"

#include "config/boot.h"                       /* struct app_context */
#include "config/boot_consensus_bundle_marker.h"
#include "config/consensus_state_install_runtime.h" /* boot_autodetect_consensus_bundle */
#include "chain/checkpoints.h"                 /* get_sha3_utxo_checkpoint */
#include "net/rom_fetch.h"
#include "net/rom_seed.h"                       /* ROM_SEED_* bounds */
#include "net/file_service.h"                   /* FS_PORT default */
#include "util/log_macros.h"
#include "util/safe_alloc.h"                    /* zcl_malloc */

#include <errno.h>
#include <limits.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>

#define BBF_SUBSYS "boot_bundle_fetch"

/* The manifest commitment (a /directory.json body) is small + auditable; cap
 * the read well above a realistic ROM directory (a few artifacts). */
#define BBF_DIRECTORY_JSON_MAX (64u * 1024u)

/* ── Gate ───────────────────────────────────────────────────────────────── */

bool boot_bundle_fetch_should_run(const char *datadir,
                                  const struct app_context *ctx)
{
    if (!datadir || !datadir[0])
        return false;
    /* -nofilesync already means "do not reach out to file-service downloads". */
    if (ctx && ctx->no_file_sync)
        return false;
    if (getenv("ZCL_NO_BUNDLE_FETCH"))
        return false;
    /* Never re-fetch over already-sovereign state. */
    if (boot_consensus_bundle_marker_exists(datadir))
        return false;
    /* A bundle already staged under <datadir>/bundles/ → the autodetect installs
     * it; no download needed. (Autodetect returns NULL when the marker is set or
     * every candidate is <name>.failed; the marker case is already handled.) */
    char *staged = boot_autodetect_consensus_bundle(datadir);
    if (staged) {
        free(staged);
        return false;
    }
    return true;
}

/* ── Manifest pick from a /directory.json body ──────────────────────────── */

bool boot_bundle_pick_manifest(const char *directory_json,
                               struct rom_fetch_manifest *out)
{
    if (!directory_json || !out)
        return false;

    struct rom_fetch_manifest arts[ROM_FETCH_MAX_ARTIFACTS];
    memset(arts, 0, sizeof(arts));
    int n = rom_fetch_parse_directory(directory_json, arts,
                                      ROM_FETCH_MAX_ARTIFACTS);
    if (n <= 0)
        return false;

    /* The complete-state bundle is the largest artifact a seeder advertises. */
    int best = -1;
    for (int i = 0; i < n; i++) {
        if (!arts[i].used)
            continue;
        if (best < 0 || arts[i].size_bytes > arts[best].size_bytes)
            best = i;
    }
    if (best < 0)
        return false;

    *out = arts[best];

    /* directory.json entries carry digests + layout but NO filename. Assign a
     * canonical, classifiable name so both boot_autodetect_consensus_bundle
     * (requires *.sqlite) and the installer's classify step (requires the
     * consensus-state-bundle- prefix) accept the downloaded file. The height is
     * cosmetic to the classifier; the CHECKPOINT_ROM authority is what actually
     * binds the installed state. */
    const struct sha3_utxo_checkpoint *cp = get_sha3_utxo_checkpoint();
    long h = cp ? (long)cp->height : 0;
    snprintf(out->filename, sizeof(out->filename),
             "consensus-state-bundle-%ld.sqlite", h);
    out->used = true;

    /* Re-check with the assigned filename (rom_fetch_manifest_sane also enforces
     * the filename is a bare basename — it is). */
    if (!rom_fetch_manifest_sane(out))
        return false;
    return true;
}

/* ── Verified download into <datadir>/bundles/ ──────────────────────────── */

bool boot_bundle_fetch_download(const char *datadir,
                                const struct rom_fetch_peer *peers,
                                size_t npeers,
                                const struct rom_fetch_manifest *m)
{
    if (!datadir || !datadir[0] || !peers || npeers == 0 || !m)
        return false;
    if (!m->filename[0] || !rom_fetch_manifest_sane(m))
        LOG_FAIL(BBF_SUBSYS,
                 "refusing bundle fetch: committed manifest not sane / no name");

    char bundles[PATH_MAX];
    int bn = snprintf(bundles, sizeof(bundles), "%s/bundles", datadir);
    if (bn < 0 || (size_t)bn >= sizeof(bundles))
        LOG_FAIL(BBF_SUBSYS, "bundles path too long under %s", datadir);
    if (mkdir(bundles, 0700) != 0 && errno != EEXIST)
        LOG_FAIL(BBF_SUBSYS, "mkdir(%s) failed: %s", bundles, strerror(errno));

    /* Prefer the per-chunk-verified swarm path (rom_fetch_download_verified_
     * parallel): probe reachable seeders for the artifact's "RMF" manifest and,
     * on the first that serves one matching the committed num_chunks, run the
     * content-verify-and-failover download. Only if NO reachable peer serves a
     * manifest (an all-legacy seeder set) do we take the whole-file-only
     * multi-seeder scheduler. Both verify the bytes against `m` before the
     * atomic rename — a byte-mismatch never lands a *.sqlite. */
    uint8_t (*chunk_sha3)[32] =
        zcl_malloc((size_t)ROM_SEED_MAX_CHUNKS * 32, "bbf_chunk_sha3");
    if (!chunk_sha3)
        LOG_FAIL(BBF_SUBSYS, "OOM allocating chunk-manifest buffer");

    uint32_t manifest_chunks = 0;
    bool have_manifest = false;
    for (size_t i = 0; i < npeers && !have_manifest; i++) {
        if (rom_fetch_get_manifest(peers[i].addr, peers[i].port, m->chunk_root,
                                   chunk_sha3, ROM_SEED_MAX_CHUNKS,
                                   &manifest_chunks) &&
            manifest_chunks == m->num_chunks)
            have_manifest = true;
    }

    uint32_t workers = (uint32_t)(2 * npeers);
    if (workers > ROM_FETCH_MAX_WORKERS)
        workers = ROM_FETCH_MAX_WORKERS;

    bool ok;
    if (have_manifest)
        ok = rom_fetch_download_verified_parallel(peers, npeers, m, chunk_sha3,
                                                  manifest_chunks, bundles,
                                                  NULL, NULL);
    else
        ok = rom_fetch_download_parallel(peers, npeers, m, bundles, workers,
                                         NULL, NULL);
    free(chunk_sha3);

    if (!ok) {
        LOG_WARN(BBF_SUBSYS,
                 "instant-on bundle fetch did not complete for %s "
                 "(content-verify/transport failure — left partial for resume, "
                 "boot falls back to P2P / operator bundle)", m->filename);
        return false;
    }

    LOG_INFO(BBF_SUBSYS,
             "instant-on bundle fetch landed %s/%s (%u chunks, content-verified) "
             "— the autodetect installs it under the CHECKPOINT_ROM authority",
             bundles, m->filename, m->num_chunks);
    return true;
}

/* ── Production entry: assemble seeds, read the manifest hint, download ──── */

/* Append host[:port] to peers[] (default port FS_PORT). No-op when full or the
 * host does not fit rom_fetch_peer.addr. */
static void bbf_add_peer(struct rom_fetch_peer *peers, size_t *np, size_t cap,
                         const char *host_port)
{
    if (*np >= cap || !host_port || !host_port[0])
        return;

    char host[128];
    snprintf(host, sizeof(host), "%s", host_port);
    uint16_t port = FS_PORT;

    /* A trailing ":<port>" overrides FS_PORT (operator/test convenience). Split
     * on the LAST ':' only when the suffix is a pure decimal port. */
    char *colon = strrchr(host, ':');
    if (colon && colon[1]) {
        char *end = NULL;
        long p = strtol(colon + 1, &end, 10);
        if (end && *end == '\0' && p >= 1 && p <= 65535) {
            port = (uint16_t)p;
            *colon = '\0';
        }
    }
    if (!host[0] || strlen(host) >= sizeof(peers[0].addr))
        return;

    /* De-dup on (addr, port). */
    for (size_t i = 0; i < *np; i++)
        if (peers[i].port == port && strcmp(peers[i].addr, host) == 0)
            return;

    snprintf(peers[*np].addr, sizeof(peers[*np].addr), "%s", host);
    peers[*np].port = port;
    (*np)++;
}

/* Read up to `cap` bytes of a text file into a NUL-terminated malloc'd buffer.
 * Returns NULL when absent/empty/too-large (all non-fatal). */
static char *bbf_read_text_file(const char *path, size_t cap)
{
    FILE *f = fopen(path, "rb");
    if (!f)
        return NULL;
    char *buf = zcl_malloc(cap + 1, "bbf_directory_json");
    if (!buf) {
        fclose(f);
        return NULL;
    }
    size_t rd = fread(buf, 1, cap, f);
    int too_big = (rd == cap && fgetc(f) != EOF);
    fclose(f);
    if (rd == 0 || too_big) {
        free(buf);
        return NULL;
    }
    buf[rd] = '\0';
    return buf;
}

bool boot_bundle_fetch_maybe(const char *datadir, const struct app_context *ctx)
{
    if (!boot_bundle_fetch_should_run(datadir, ctx))
        return false;

    /* Manifest commitment: the publisher's small /directory.json, staged at
     * <datadir>/bundles/directory.json. The multi-GB bytes are swarmed and
     * content-verified against it; the install gate then binds the result to the
     * compiled checkpoint. Absent hint → safe no-op (this is the common fresh
     * boot with nothing staged; P2P IBD / the operator bundle remain the path). */
    char hint_path[PATH_MAX];
    int hn = snprintf(hint_path, sizeof(hint_path),
                      "%s/bundles/directory.json", datadir);
    if (hn < 0 || (size_t)hn >= sizeof(hint_path))
        return false;

    char *body = bbf_read_text_file(hint_path, BBF_DIRECTORY_JSON_MAX);
    if (!body) {
        LOG_INFO(BBF_SUBSYS,
                 "no bundle manifest at %s — skipping instant-on fetch",
                 hint_path);
        return false;
    }

    struct rom_fetch_manifest m;
    memset(&m, 0, sizeof(m));
    bool picked = boot_bundle_pick_manifest(body, &m);
    free(body);
    if (!picked) {
        LOG_WARN(BBF_SUBSYS,
                 "manifest hint present at %s but no usable consensus-state "
                 "artifact — skipping", hint_path);
        return false;
    }

    /* Assemble the file-service seed set from the SAME sources the node's other
     * cold-start file-sync path uses: the operator's -fileservice= peer first,
     * then the hardcoded clearnet file-service seeds (skipped in connect-only
     * mode, where all bootstrap data must come from the explicit peer set). The
     * seeds are unauthenticated transport — that is fine here: the download is
     * content-verified against the committed manifest and the install path binds
     * the result to the compiled checkpoint, so a MITM/forged seed can at worst
     * fail the fetch or get refused at install, never seed a forged UTXO set. */
    struct rom_fetch_peer peers[ROM_FETCH_MAX_WORKERS];
    memset(peers, 0, sizeof(peers));
    size_t np = 0;
    const size_t cap = sizeof(peers) / sizeof(peers[0]);

    if (ctx && ctx->file_service_peer && ctx->file_service_peer[0])
        bbf_add_peer(peers, &np, cap, ctx->file_service_peer);

    if (!(ctx && ctx->connect_only)) {
        static const char *const clearnet_fs_seeds[] = {
            "205.209.104.118",
            "140.174.189.3",
            NULL,
        };
        for (int i = 0; clearnet_fs_seeds[i]; i++)
            bbf_add_peer(peers, &np, cap, clearnet_fs_seeds[i]);
    }

    if (np == 0) {
        LOG_INFO(BBF_SUBSYS,
                 "no file-service seeds available (connect-only with no "
                 "-fileservice peer) — skipping instant-on fetch");
        return false;
    }

    return boot_bundle_fetch_download(datadir, peers, np, &m);
}

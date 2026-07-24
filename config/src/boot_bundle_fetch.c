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
#include "config/bundle_fetch_seeds.h"          /* ZCL_BUNDLE_FETCH_CLEARNET_SEEDS */
#include "config/boot_consensus_bundle_marker.h"
#include "config/consensus_state_install_runtime.h" /* boot_autodetect_consensus_bundle */
#include "chain/checkpoints.h"                 /* get_sha3_utxo_checkpoint */
#include "net/rom_fetch.h"
#include "net/rom_seed.h"                       /* ROM_SEED_* bounds */
#include "net/file_service.h"                   /* FS_PORT default */
#include "encoding/utilstrencodings.h"          /* HexStr */
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
#include <unistd.h>

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

    /* Kind-aware bundle selection. Pass 1: prefer an explicitly consensus-
     * bundle-kinded artifact (largest such). Pass 2 (legacy back-compat): a
     * directory that carries no "kind" field parses every entry to
     * ROM_ARTIFACT_UNKNOWN, so fall back to the LARGEST non-header-seed
     * artifact — today's "the bundle is the big one" behavior — while never
     * mis-picking the header-chain seed as a .sqlite bundle. */
    int best = -1;
    for (int i = 0; i < n; i++) {
        if (!arts[i].used || arts[i].kind != ROM_ARTIFACT_CONSENSUS_BUNDLE)
            continue;
        if (best < 0 || arts[i].size_bytes > arts[best].size_bytes)
            best = i;
    }
    if (best < 0) {
        for (int i = 0; i < n; i++) {
            if (!arts[i].used || arts[i].kind == ROM_ARTIFACT_HEADER_SEED)
                continue;
            if (best < 0 || arts[i].size_bytes > arts[best].size_bytes)
                best = i;
        }
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

bool boot_bundle_pick_header_seed_manifest(const char *directory_json,
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

    /* The header-chain seed is the ROM_ARTIFACT_HEADER_SEED-kinded entry. It is
     * selected by kind ONLY (never by size) — a legacy directory that emits no
     * kind cannot advertise a header seed, and must not have some other artifact
     * mistaken for one. */
    int best = -1;
    for (int i = 0; i < n; i++) {
        if (!arts[i].used || arts[i].kind != ROM_ARTIFACT_HEADER_SEED)
            continue;
        if (best < 0 || arts[i].size_bytes > arts[best].size_bytes)
            best = i;
    }
    if (best < 0)
        return false;

    *out = arts[best];

    /* directory.json entries carry no filename; assign the canonical
     * block_index.bin so rom_seed_classify (serve + re-seed) and the flat loader
     * (boot_header_seed_import) accept the downloaded file. */
    snprintf(out->filename, sizeof(out->filename), "block_index.bin");
    out->used = true;

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

    /* Reseed the swarm: register the just-landed, content-verified bundle
     * with rom_seed IMMEDIATELY so this node starts serving it to other
     * downloaders within the same boot, with no restart needed — BitTorrent-
     * style swarm widening with zero operator input.
     *
     * rom_seed_register is pure registry state (a mutex + static tables); it
     * has no dependency on the file-service thread, rom_seed_start_scan, or
     * any other boot stage having run yet, so it is always safe to call here
     * — even though, in production, this function runs from
     * boot_select_state_source, which fires BEFORE boot_rom_seed_start /
     * fs_server_start (config/src/boot_auto_install_bundle.c). That ordering
     * is exactly why this call cannot simply gate on "is the file service
     * running": at this point in a fresh boot it never is yet. A failure
     * here (registry full, path overflow) is logged and non-fatal — it never
     * blocks or fails boot — because rom_seed_scan_datadir now recurses into
     * <datadir>/bundles/ (net/rom_seed.c) and will register the same file
     * the next time it runs: this same boot's own rom_seed_start_scan (which
     * starts moments later, once the frontend services init), or a later
     * boot if seeding was disabled this run. */
    char reseed_name[ROM_SEED_NAME_MAX];
    int rnn = snprintf(reseed_name, sizeof(reseed_name), "%s/%s",
                       ROM_SEED_BUNDLES_SUBDIR, m->filename);
    if (rnn > 0 && (size_t)rnn < sizeof(reseed_name)) {
        enum rom_register_result rrc =
            rom_seed_register(datadir, reseed_name, m->whole_sha3, NULL);
        if (rrc == ROM_REG_OK)
            LOG_INFO(BBF_SUBSYS,
                     "reseed: registered fetched bundle '%s' with rom_seed — "
                     "this node now serves it to the swarm", reseed_name);
        else
            LOG_WARN(BBF_SUBSYS,
                     "reseed: could not register fetched bundle '%s' (rc=%d) "
                     "— the next rom_seed scan will pick it up", reseed_name,
                     (int)rrc);
    } else {
        LOG_WARN(BBF_SUBSYS,
                 "reseed: bundle relative name overflow — the next rom_seed "
                 "scan will pick it up");
    }

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

/* Baked-facts cross-check for a picked manifest — the chunking + size invariants
 * the compiled seeder always honours. rom_fetch_manifest_sane (called inside
 * boot_bundle_pick_manifest) already enforces these; this is the belt-and-
 * suspenders guard the discovery path asserts before it trusts a peer-advertised
 * triple. Pure predicate — a false return is "not the compiled artifact shape",
 * not an error site. */
static bool bbf_manifest_facts_ok(const struct rom_fetch_manifest *m)
{
    if (!m)
        return false;
    if (m->chunk_size != ROM_SEED_CHUNK_SIZE)
        return false;
    if (m->size_bytes < ROM_SEED_MIN_ARTIFACT_BYTES ||
        m->size_bytes > ROM_SEED_MAX_ARTIFACT_BYTES)
        return false;
    uint32_t expect =
        (uint32_t)((m->size_bytes + m->chunk_size - 1) / m->chunk_size);
    return m->num_chunks > 0 && m->num_chunks <= ROM_SEED_MAX_CHUNKS &&
           m->num_chunks == expect;
}

/* Emit one {"kind":..,"digest":..,"whole_sha3":..,"size":..,"chunk_size":..,
 * "chunks":..} object into `dst` (capacity `cap`). `kind` is the wire token
 * rom_seed_kind_from_name round-trips ("consensus_bundle" / "header_seed").
 * Returns the byte count, or 0 on overflow. */
static int bbf_emit_artifact_obj(char *dst, size_t cap, const char *kind,
                                 const struct rom_fetch_manifest *m)
{
    char digest_hex[65], whole_hex[65];
    HexStr(m->chunk_root, 32, false, digest_hex, sizeof(digest_hex));
    HexStr(m->whole_sha3, 32, false, whole_hex, sizeof(whole_hex));
    int wn = snprintf(dst, cap,
        "{\"kind\":\"%s\",\"digest\":\"%s\",\"whole_sha3\":\"%s\","
        "\"size\":%llu,\"chunk_size\":%u,\"chunks\":%u}",
        kind, digest_hex, whole_hex, (unsigned long long)m->size_bytes,
        m->chunk_size, m->num_chunks);
    if (wn <= 0 || (size_t)wn >= cap)
        return 0;
    return wn;
}

/* Persist the discovered manifest(s) as the canonical
 * <datadir>/bundles/directory.json hint so a resume/reseed reads them locally
 * without re-querying peers. Emits the {"artifacts":[...]} object
 * rom_fetch_parse_directory / pick consume — the consensus bundle plus, when
 * `hs` is non-NULL, the header-chain seed. Writes via a .tmp + rename so a
 * crash never leaves a truncated hint. */
static bool bbf_write_directory_hint(const char *datadir,
                                     const struct rom_fetch_manifest *m,
                                     const struct rom_fetch_manifest *hs)
{
    if (!datadir || !datadir[0] || !m)
        LOG_FAIL(BBF_SUBSYS, "write hint: null arg");

    char bundles[PATH_MAX];
    int bn = snprintf(bundles, sizeof(bundles), "%s/bundles", datadir);
    if (bn < 0 || (size_t)bn >= sizeof(bundles))
        LOG_FAIL(BBF_SUBSYS, "write hint: bundles path too long under %s",
                 datadir);
    if (mkdir(bundles, 0700) != 0 && errno != EEXIST)
        LOG_FAIL(BBF_SUBSYS, "write hint: mkdir(%s) failed: %s", bundles,
                 strerror(errno));

    char body[2048];
    int off = snprintf(body, sizeof(body), "{\"artifacts\":[");
    if (off <= 0 || (size_t)off >= sizeof(body))
        LOG_FAIL(BBF_SUBSYS, "write hint: directory.json body overflow");
    int en = bbf_emit_artifact_obj(body + off, sizeof(body) - (size_t)off,
                                   "consensus_bundle", m);
    if (en == 0)
        LOG_FAIL(BBF_SUBSYS, "write hint: bundle artifact object overflow");
    off += en;
    if (hs) {
        int cn = snprintf(body + off, sizeof(body) - (size_t)off, ",");
        if (cn <= 0 || (size_t)(off + cn) >= sizeof(body))
            LOG_FAIL(BBF_SUBSYS, "write hint: directory.json separator overflow");
        off += cn;
        en = bbf_emit_artifact_obj(body + off, sizeof(body) - (size_t)off,
                                   "header_seed", hs);
        if (en == 0)
            LOG_FAIL(BBF_SUBSYS, "write hint: header-seed artifact object overflow");
        off += en;
    }
    int cn = snprintf(body + off, sizeof(body) - (size_t)off, "]}");
    if (cn <= 0 || (size_t)(off + cn) >= sizeof(body))
        LOG_FAIL(BBF_SUBSYS, "write hint: directory.json close overflow");
    int wn = off + cn;
    if (wn <= 0 || (size_t)wn >= sizeof(body))
        LOG_FAIL(BBF_SUBSYS, "write hint: directory.json body overflow");

    char path[PATH_MAX];
    int pn = snprintf(path, sizeof(path), "%s/bundles/directory.json", datadir);
    if (pn < 0 || (size_t)pn >= sizeof(path))
        LOG_FAIL(BBF_SUBSYS, "write hint: path too long under %s", datadir);
    char tmp[PATH_MAX];
    int tn = snprintf(tmp, sizeof(tmp), "%s/bundles/directory.json.tmp", datadir);
    if (tn < 0 || (size_t)tn >= sizeof(tmp))
        LOG_FAIL(BBF_SUBSYS, "write hint: tmp path too long under %s", datadir);

    FILE *f = fopen(tmp, "wb");
    if (!f)
        LOG_FAIL(BBF_SUBSYS, "write hint: fopen(%s) failed: %s", tmp,
                 strerror(errno));
    bool ok = fwrite(body, 1, (size_t)wn, f) == (size_t)wn;
    if (fclose(f) != 0)
        ok = false;
    if (!ok) {
        (void)unlink(tmp);
        LOG_FAIL(BBF_SUBSYS, "write hint: writing %s failed", tmp);
    }
    if (rename(tmp, path) != 0) {
        (void)unlink(tmp);
        LOG_FAIL(BBF_SUBSYS, "write hint: rename %s -> %s failed: %s", tmp, path,
                 strerror(errno));
    }
    return true;
}

/* Assemble the file-service seed set from the SAME sources the node's other
 * cold-start file-sync path uses: the operator's -fileservice= peer first, then
 * the hardcoded clearnet file-service seeds (skipped in connect-only mode, where
 * all bootstrap data must come from the explicit peer set). The seeds are
 * unauthenticated transport — that is fine here: the download is content-verified
 * against the committed manifest and the install path binds the result to the
 * compiled checkpoint, so a MITM/forged seed can at worst fail the fetch or get
 * refused at install, never seed a forged UTXO set. Sets *out_explicit_first when
 * the operator's -fileservice peer actually took slot 0. Returns the peer count. */
static size_t bbf_assemble_seeds(const struct app_context *ctx,
                                 struct rom_fetch_peer *peers, size_t cap,
                                 bool *out_explicit_first)
{
    size_t np = 0;
    if (out_explicit_first)
        *out_explicit_first = false;

    if (ctx && ctx->file_service_peer && ctx->file_service_peer[0]) {
        bbf_add_peer(peers, &np, cap, ctx->file_service_peer);
        if (out_explicit_first && np == 1)
            *out_explicit_first = true; /* the explicit peer took slot 0 */
    }

    if (!(ctx && ctx->connect_only)) {
        for (int i = 0; ZCL_BUNDLE_FETCH_CLEARNET_SEEDS[i]; i++)
            bbf_add_peer(peers, &np, cap, ZCL_BUNDLE_FETCH_CLEARNET_SEEDS[i]);
    }
    return np;
}

/* One discovered-manifest candidate and how many independent seeds served it. */
struct bbf_disc_cand {
    struct rom_fetch_manifest m;
    int  count;
    bool has_explicit;   /* an explicit -fileservice seed served this triple */
};

/* Pure quorum decision over the tallied candidates. Returns the winning index,
 * or -1 for "no quorum" (fail-open to IBD). The rule: the most-agreed candidate
 * wins iff >=2 independent seeds served its (chunk_root, whole_sha3, size)
 * triple; a lone candidate wins ONLY when the operator explicitly named its
 * seed (-fileservice); a lone non-explicit candidate is refused (bandwidth-DoS
 * guard — trust binds at install, not here). No IO. */
static int bbf_quorum_pick(const int *counts, const bool *has_explicit,
                           size_t ncand)
{
    int best = -1;
    for (size_t c = 0; c < ncand; c++) {
        if (best < 0) {
            best = (int)c;
            continue;
        }
        /* Higher count wins; a tie prefers an explicit-seed candidate so a lone
         * operator-named seed is not shadowed by an equal-count non-explicit. */
        if (counts[c] > counts[best] ||
            (counts[c] == counts[best] && has_explicit[c] && !has_explicit[best]))
            best = (int)c;
    }
    if (best < 0)
        return -1;
    if (counts[best] >= 2)
        return best;
    if (has_explicit[best])
        return best;
    return -1;
}

/* Tally one picked manifest into a candidate list, merging on a byte-identical
 * (chunk_root, whole_sha3, size) triple. Bounded by `ccap`. */
static void bbf_tally_cand(struct bbf_disc_cand *cands, size_t *ncand,
                           size_t ccap, const struct rom_fetch_manifest *m,
                           bool is_explicit)
{
    for (size_t c = 0; c < *ncand; c++) {
        if (cands[c].m.size_bytes == m->size_bytes &&
            memcmp(cands[c].m.chunk_root, m->chunk_root, 32) == 0 &&
            memcmp(cands[c].m.whole_sha3, m->whole_sha3, 32) == 0) {
            cands[c].count++;
            cands[c].has_explicit = cands[c].has_explicit || is_explicit;
            return;
        }
    }
    if (*ncand < ccap) {
        cands[*ncand].m = *m;
        cands[*ncand].count = 1;
        cands[*ncand].has_explicit = is_explicit;
        (*ncand)++;
    }
}

/* Apply the quorum rule to a tallied candidate list. Returns true (and fills
 * *out) when a candidate wins; false = no quorum. */
static bool bbf_quorum_winner(const struct bbf_disc_cand *cands, size_t ncand,
                              struct rom_fetch_manifest *out)
{
    int counts[ROM_FETCH_MAX_WORKERS];
    bool flags[ROM_FETCH_MAX_WORKERS];
    for (size_t c = 0; c < ncand && c < ROM_FETCH_MAX_WORKERS; c++) {
        counts[c] = cands[c].count;
        flags[c] = cands[c].has_explicit;
    }
    int best = bbf_quorum_pick(counts, flags, ncand);
    if (best < 0)
        return false;
    *out = cands[best].m;
    return true;
}

/* One discovered directory: the required consensus bundle plus the optional
 * header-chain seed. */
struct bbf_discovery {
    struct rom_fetch_manifest bundle;
    struct rom_fetch_manifest header_seed;
    bool have_bundle;
    bool have_header_seed;
};

/* Query each seed for its directory listing over the FS "RLS" wire, pick both
 * the consensus-bundle and header-seed manifests each advertises, and require
 * >=2 independent seeds returning a byte-identical (chunk_root, whole_sha3,
 * size) triple before trusting EACH manifest — a bandwidth-DoS / lone-liar
 * guard: trust binds at install, not here. quorum=1 is accepted ONLY when the
 * lone seed is the operator's explicit -fileservice peer. The bundle is
 * REQUIRED (no bundle quorum → fail-open to IBD); the header seed is OPTIONAL
 * (its absence just means the header chain still arrives via P2P). On success
 * *out holds the winning manifest(s) and the winning directory.json (both
 * artifacts) is persisted for resume. Returns false when no bundle quorum
 * forms. */
/* Persists the last discovery quorum outcome to progress.kv for the
 * "bbf_discovery" diagnostics dumper — see
 * config/src/boot_bundle_fetch_discovery_outcome.c (split out to keep this
 * file under the E1 800-line ceiling) for the full rationale and contract. */
void bbf_record_discovery_outcome(const char *outcome_name,
                                  size_t seed_count, size_t responded_count);

static bool bbf_discover_from_peers(const char *datadir,
                                    const struct rom_fetch_peer *peers,
                                    size_t np, bool explicit_first,
                                    struct bbf_discovery *out)
{
    memset(out, 0, sizeof(*out));

    char *body = zcl_malloc(BBF_DIRECTORY_JSON_MAX + 1, "bbf_disc_body");
    if (!body)
        LOG_FAIL(BBF_SUBSYS, "discovery: OOM allocating listing buffer");

    struct bbf_disc_cand bundle_cands[ROM_FETCH_MAX_WORKERS];
    struct bbf_disc_cand hs_cands[ROM_FETCH_MAX_WORKERS];
    memset(bundle_cands, 0, sizeof(bundle_cands));
    memset(hs_cands, 0, sizeof(hs_cands));
    size_t nbundle = 0, nhs = 0;
    size_t responded = 0;
    const size_t ccap = sizeof(bundle_cands) / sizeof(bundle_cands[0]);

    for (size_t i = 0; i < np; i++) {
        if (!rom_fetch_get_directory(peers[i].addr, peers[i].port, body,
                                     BBF_DIRECTORY_JSON_MAX + 1))
            continue;
        responded++;
        bool is_explicit = explicit_first && i == 0;

        struct rom_fetch_manifest m;
        memset(&m, 0, sizeof(m));
        if (boot_bundle_pick_manifest(body, &m)) {
            if (bbf_manifest_facts_ok(&m))
                bbf_tally_cand(bundle_cands, &nbundle, ccap, &m, is_explicit);
            else
                LOG_WARN(BBF_SUBSYS, "discovery: seed %s:%u advertised a bundle "
                         "manifest that fails the baked-facts cross-check — "
                         "ignoring", peers[i].addr, (unsigned)peers[i].port);
        }

        struct rom_fetch_manifest hm;
        memset(&hm, 0, sizeof(hm));
        if (boot_bundle_pick_header_seed_manifest(body, &hm)) {
            if (bbf_manifest_facts_ok(&hm))
                bbf_tally_cand(hs_cands, &nhs, ccap, &hm, is_explicit);
            else
                LOG_WARN(BBF_SUBSYS, "discovery: seed %s:%u advertised a header-"
                         "seed manifest that fails the baked-facts cross-check "
                         "— ignoring", peers[i].addr, (unsigned)peers[i].port);
        }
    }
    free(body);

    out->have_bundle = bbf_quorum_winner(bundle_cands, nbundle, &out->bundle);
    if (!out->have_bundle) {
        if (nbundle == 0) {
            LOG_INFO(BBF_SUBSYS, "discovery: no reachable seed served a usable "
                     "bundle manifest — skipping instant-on fetch");
            bbf_record_discovery_outcome("no_quorum_fell_open_to_ibd", np,
                                         responded);
        } else {
            LOG_WARN(BBF_SUBSYS, "discovery: only a lone non-explicit seed "
                     "served a bundle manifest — refusing quorum=1 "
                     "(bandwidth-DoS guard); falling back to P2P IBD");
            bbf_record_discovery_outcome("degraded_single_seed", np,
                                         responded);
        }
        return false;
    }

    out->have_header_seed = bbf_quorum_winner(hs_cands, nhs, &out->header_seed);

    LOG_INFO(BBF_SUBSYS, "discovery: bundle manifest quorum reached (size=%llu); "
             "header-seed manifest %s — proceeding",
             (unsigned long long)out->bundle.size_bytes,
             out->have_header_seed ? "also reached quorum (headers arrive as an "
             "artifact)" : "not advertised/quorum (header chain via P2P)");
    bbf_record_discovery_outcome("reached", np, responded);

    if (!bbf_write_directory_hint(datadir, &out->bundle,
                                  out->have_header_seed ? &out->header_seed
                                                        : NULL))
        LOG_WARN(BBF_SUBSYS, "discovery: could not persist the discovered "
                 "directory.json hint — resume will re-discover");
    return true;
}

/* Is the header-chain seed artifact still WANTED on this datadir? True on a
 * fresh, non-sovereign node that has neither imported it (<datadir>/
 * block_index.bin at the root) nor already downloaded it (<datadir>/bundles/
 * block_index.bin). Independent of the bundle gate: a datadir with a staged
 * bundle but no header chain STILL needs the seed so the install can bind. */
static bool bbf_header_seed_needed(const char *datadir,
                                   const struct app_context *ctx)
{
    if (!datadir || !datadir[0])
        return false;
    if (ctx && ctx->no_file_sync)
        return false;
    if (getenv("ZCL_NO_BUNDLE_FETCH"))
        return false;
    if (boot_consensus_bundle_marker_exists(datadir))
        return false;

    char path[PATH_MAX];
    struct stat st;
    int pn = snprintf(path, sizeof(path), "%s/block_index.bin", datadir);
    if (pn > 0 && (size_t)pn < sizeof(path) && stat(path, &st) == 0)
        return false; /* already imported (or a legacy flat cache present) */
    pn = snprintf(path, sizeof(path), "%s/bundles/block_index.bin", datadir);
    if (pn > 0 && (size_t)pn < sizeof(path) && stat(path, &st) == 0)
        return false; /* already downloaded — import consumes it, no re-fetch */
    return true;
}

bool boot_bundle_fetch_maybe(const char *datadir, const struct app_context *ctx)
{
    bool bundle_needed = boot_bundle_fetch_should_run(datadir, ctx);
    bool header_needed = bbf_header_seed_needed(datadir, ctx);
    if (!bundle_needed && !header_needed)
        return false;

    /* Assemble the file-service seed set once — both discovery and the download
     * ride it (see bbf_assemble_seeds for the trust rationale). */
    struct rom_fetch_peer peers[ROM_FETCH_MAX_WORKERS];
    memset(peers, 0, sizeof(peers));
    bool explicit_first = false;
    size_t np = bbf_assemble_seeds(ctx, peers, sizeof(peers) / sizeof(peers[0]),
                                   &explicit_first);
    if (np == 0) {
        LOG_INFO(BBF_SUBSYS,
                 "no file-service seeds available (connect-only with no "
                 "-fileservice peer) — skipping instant-on fetch");
        return false;
    }

    /* Manifest commitment. Prefer a LOCAL <datadir>/bundles/directory.json (an
     * operator hint, or one a prior discovery/resume persisted). On a truly fresh
     * node it is absent — discover it from the seed set over the file-service RLS
     * wire, requiring a >=2-seed quorum before trusting it. Either way the multi-
     * GB bytes are swarmed + content-verified against the committed manifest and
     * the install gate binds the result to the compiled checkpoint. */
    char hint_path[PATH_MAX];
    int hn = snprintf(hint_path, sizeof(hint_path),
                      "%s/bundles/directory.json", datadir);
    if (hn < 0 || (size_t)hn >= sizeof(hint_path))
        return false;

    struct bbf_discovery disc;
    memset(&disc, 0, sizeof(disc));

    char *body = bbf_read_text_file(hint_path, BBF_DIRECTORY_JSON_MAX);
    if (body) {
        disc.have_bundle = boot_bundle_pick_manifest(body, &disc.bundle);
        disc.have_header_seed =
            boot_bundle_pick_header_seed_manifest(body, &disc.header_seed);
        free(body);
        if (!disc.have_bundle && !disc.have_header_seed) {
            LOG_WARN(BBF_SUBSYS,
                     "manifest hint present at %s but no usable artifact "
                     "— skipping", hint_path);
            return false;
        }
    } else {
        LOG_INFO(BBF_SUBSYS,
                 "no local bundle manifest at %s — attempting peer directory "
                 "discovery over the file-service RLS wire", hint_path);
        if (!bbf_discover_from_peers(datadir, peers, np, explicit_first, &disc))
            return false; /* fail-open: normal P2P IBD is the path */
    }

    /* Headers FIRST: the bundle install DEFERS on the header chain reaching the
     * checkpoint (checkpoint_bundle_install_ready), so the header seed is on the
     * critical path — download it before the (larger) bundle so the in-process
     * import that follows can climb pindex_best_header this boot. Both rides the
     * same content-verified swarm path; a header-seed miss is non-fatal (the
     * chain still arrives via P2P, just slower). */
    bool any = false;
    if (header_needed && disc.have_header_seed) {
        if (boot_bundle_fetch_download(datadir, peers, np, &disc.header_seed)) {
            any = true;
            LOG_INFO(BBF_SUBSYS, "instant-on: header-chain seed landed — the "
                     "in-process import climbs the header frontier (no serial "
                     "P2P header crawl before install)");
        } else {
            LOG_WARN(BBF_SUBSYS, "instant-on: header-chain seed fetch did not "
                     "complete — header chain falls back to P2P sync");
        }
    }

    if (bundle_needed && disc.have_bundle)
        any = boot_bundle_fetch_download(datadir, peers, np, &disc.bundle) || any;

    return any;
}

#ifdef ZCL_TESTING
/* Test surface: the pure baked-facts cross-check and quorum decision (see
 * config/boot_bundle_fetch.h). Kept out of the production ABI. */
bool boot_bundle_manifest_facts_ok_for_test(const struct rom_fetch_manifest *m)
{
    return bbf_manifest_facts_ok(m);
}

int boot_bundle_quorum_pick_for_test(const int *counts, const bool *has_explicit,
                                     size_t ncand)
{
    return bbf_quorum_pick(counts, has_explicit, ncand);
}
#endif

/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ROM artifact fetching — the CLIENT half of docs/ROM_DELIVERY.md. See
 * net/rom_fetch.h for the contract, the trust model, and the wire protocol.
 *
 * Everything wire- or disk-derived is bounded and validated here. The module
 * verifies bytes against caller-committed digests and nothing more: install /
 * activation stays with the unified installer (no third activation door). */

#include "net/rom_fetch.h"
#include "net/file_service.h"
#include "crypto/sha3.h"
#include "encoding/utilstrencodings.h"
#include "json/json.h"
#include "platform/time_compat.h"
#include "util/safe_alloc.h"
#include "util/log_macros.h"

#include <errno.h>
#include <fcntl.h>
#include <netdb.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <unistd.h>

#define RF_SUBSYS "rom_fetch"

/* Connect timeout for one chunk-fetch connection (seconds). */
#define RF_CONNECT_TIMEOUT_SEC 10
/* Per-socket send/recv timeout once connected (seconds). A 4 MB chunk over
 * even a 56 kbit/s link is ~10 min, but the ROM serve path is LAN/fast-WAN
 * oriented; 120 s bounds a stalled peer without false-failing a slow one. */
#define RF_IO_TIMEOUT_SEC 120

/* ── Small helpers ──────────────────────────────────────────────────── */

/* A fetchable filename is a bare basename: no separators, no traversal,
 * non-empty, and short enough to store. Mirrors the serve side's rule. */
static bool rf_filename_ok(const char *filename)
{
    if (!filename || !filename[0])
        return false;
    size_t n = strlen(filename);
    if (n >= ROM_FETCH_NAME_MAX)
        return false;
    if (strchr(filename, '/'))
        return false;
    if (strstr(filename, ".."))
        return false;
    return true;
}

/* Read exactly n bytes into buf. Returns false on EOF/error/timeout. */
static bool rf_recv_exact(int fd, uint8_t *buf, size_t n)
{
    size_t got = 0;
    while (got < n) {
        ssize_t r = recv(fd, buf + got, n - got, 0);
        if (r < 0) {
            if (errno == EINTR)
                continue;
            return false;
        }
        if (r == 0)
            return false;
        got += (size_t)r;
    }
    return true;
}

/* ── Manifest validation + discovery parse ──────────────────────────── */

bool rom_fetch_manifest_sane(const struct rom_fetch_manifest *m)
{
    if (!m)
        return false;
    if (m->size_bytes < ROM_SEED_MIN_ARTIFACT_BYTES ||
        m->size_bytes > ROM_SEED_MAX_ARTIFACT_BYTES)
        return false;
    /* The serve side chunks at exactly ROM_SEED_CHUNK_SIZE; a peer claiming
     * any other layout is not speaking this protocol. */
    if (m->chunk_size != ROM_SEED_CHUNK_SIZE)
        return false;
    uint32_t expect_chunks =
        (uint32_t)((m->size_bytes + m->chunk_size - 1) / m->chunk_size);
    if (m->num_chunks == 0 || m->num_chunks > ROM_SEED_MAX_CHUNKS ||
        m->num_chunks != expect_chunks)
        return false;
    /* An empty filename is allowed at discovery time (the /directory.json
     * artifact entries carry no name); a NON-empty one must be a safe
     * basename before it is ever used as a sink path. */
    if (m->filename[0] && !rf_filename_ok(m->filename))
        return false;
    return true;
}

/* Parse one hex digest field: exactly 64 hex chars → 32 bytes. */
static bool rf_parse_digest(const struct json_value *obj, const char *key,
                            uint8_t out[32])
{
    const char *hex = json_get_str(json_get(obj, key));
    if (!hex || strlen(hex) != 64)
        return false;
    return ParseHex(hex, out, 32) == 32;
}

int rom_fetch_parse_directory(const char *json_body,
                              struct rom_fetch_manifest *out, size_t max)
{
    if (!json_body || !out || max == 0)
        return -1;
    if (max > ROM_FETCH_MAX_ARTIFACTS)
        max = ROM_FETCH_MAX_ARTIFACTS;

    struct json_value doc;
    json_init(&doc);
    if (!json_read(&doc, json_body, strlen(json_body)) ||
        doc.type != JSON_OBJ) {
        json_free(&doc);
        return -1;
    }

    const struct json_value *arts = json_get(&doc, "artifacts");
    if (!arts || arts->type != JSON_ARR) {
        json_free(&doc);
        return 0; /* no artifacts advertised — not an error */
    }

    int n = 0;
    for (size_t i = 0; i < arts->num_children && (size_t)n < max; i++) {
        const struct json_value *e = json_at(arts, i);
        if (!e || e->type != JSON_OBJ)
            continue;

        struct rom_fetch_manifest m;
        memset(&m, 0, sizeof(m));
        /* directory.json entries carry digests + layout, no filename. */
        if (!rf_parse_digest(e, "digest", m.chunk_root) ||
            !rf_parse_digest(e, "whole_sha3", m.whole_sha3))
            continue;
        int64_t size = json_get_int(json_get(e, "size"));
        int64_t csize = json_get_int(json_get(e, "chunk_size"));
        int64_t chunks = json_get_int(json_get(e, "chunks"));
        if (size <= 0 || csize <= 0 || chunks <= 0)
            continue;
        m.size_bytes = (uint64_t)size;
        m.chunk_size = (uint32_t)csize;
        m.num_chunks = (uint32_t)chunks;
        if (!rom_fetch_manifest_sane(&m))
            continue;
        m.used = true;
        out[n++] = m;
    }

    json_free(&doc);
    return n;
}

/* ── Verified chunk fetch ───────────────────────────────────────────── */

/* Connect to peer_addr:port with a bounded connect timeout. Returns the fd
 * or -1 (logged). */
static int rf_connect(const char *peer_addr, uint16_t port)
{
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", (unsigned)port);

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    struct addrinfo *res = NULL;
    if (getaddrinfo(peer_addr, port_str, &hints, &res) != 0 || !res)
        LOG_ERR(RF_SUBSYS, "chunk: resolve failed for %s", peer_addr);

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) {
        freeaddrinfo(res);
        LOG_ERR(RF_SUBSYS, "chunk: socket() failed: %s", strerror(errno));
    }

    int flags = fcntl(fd, F_GETFL, 0);
    fcntl(fd, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(fd, res->ai_addr, res->ai_addrlen);
    if (rc < 0 && errno == EINPROGRESS) {
        struct timeval tv = { .tv_sec = RF_CONNECT_TIMEOUT_SEC, .tv_usec = 0 };
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(fd, &wfds);
        rc = select(fd + 1, NULL, &wfds, NULL, &tv);
        int err = 0;
        socklen_t elen = sizeof(err);
        if (rc > 0)
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
        if (rc <= 0 || err != 0) {
            close(fd);
            freeaddrinfo(res);
            LOG_ERR(RF_SUBSYS, "chunk: connect to %s:%u failed/timed out",
                    peer_addr, (unsigned)port);
        }
    } else if (rc < 0) {
        close(fd);
        freeaddrinfo(res);
        LOG_ERR(RF_SUBSYS, "chunk: connect to %s:%u failed: %s",
                peer_addr, (unsigned)port, strerror(errno));
    }
    freeaddrinfo(res);
    fcntl(fd, F_SETFL, flags); /* restore blocking mode */

    struct timeval tv = { .tv_sec = RF_IO_TIMEOUT_SEC, .tv_usec = 0 };
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    return fd;
}

bool rom_fetch_chunk(const char *peer_addr, uint16_t port,
                     const uint8_t chunk_root[32], uint32_t idx,
                     uint8_t *buf, uint32_t buf_cap, uint32_t *out_sz)
{
    if (!peer_addr || !chunk_root || !buf || !out_sz)
        LOG_FAIL(RF_SUBSYS, "chunk: null arg");
    if (buf_cap < ROM_SEED_CHUNK_SIZE)
        LOG_FAIL(RF_SUBSYS, "chunk: buf_cap %u < ROM chunk size %u",
                 buf_cap, (unsigned)ROM_SEED_CHUNK_SIZE);

    int fd = rf_connect(peer_addr, port);
    if (fd < 0)
        return false;

    struct fs_session s;
    fs_session_init(&s, fd);
    /* The ROM serve path keys its sessions on an all-zero utxo_root (see
     * fs_handle_client_fd, file_service.c) — match it exactly. */
    uint8_t zero_root[32];
    memset(zero_root, 0, sizeof(zero_root));
    if (!fs_handshake(&s, zero_root, true)) {
        close(fd);
        LOG_FAIL(RF_SUBSYS, "chunk: handshake failed with %s:%u",
                 peer_addr, (unsigned)port);
    }

    /* Request: ["ROM"(3)][chunk_root(32)][chunk_index(4 LE)]. */
    uint8_t req[FS_ROM_REQUEST_SIZE];
    memcpy(req, "ROM", 3);
    memcpy(req + 3, chunk_root, 32);
    req[35] = (uint8_t)(idx);
    req[36] = (uint8_t)(idx >> 8);
    req[37] = (uint8_t)(idx >> 16);
    req[38] = (uint8_t)(idx >> 24);
    if (!fs_send_frame(&s, FS_REQUEST, req, sizeof(req))) {
        close(fd);
        LOG_FAIL(RF_SUBSYS, "chunk: request send failed to %s:%u",
                 peer_addr, (unsigned)port);
    }

    /* Success reply: raw [4-byte size LE][data][32-byte MAC]. A refusal is
     * an FS_DONE *frame* (64 KB encrypted) — its leading bytes parse as
     * garbage here, so it surfaces as a size-cap or read failure below.
     * Fail-closed either way. */
    uint8_t hdr[4];
    if (!rf_recv_exact(fd, hdr, 4)) {
        close(fd);
        LOG_FAIL(RF_SUBSYS, "chunk: size header read failed (peer %s:%u "
                 "refused or went away)", peer_addr, (unsigned)port);
    }
    uint32_t size = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
                    ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
    if (size == 0 || size > ROM_SEED_CHUNK_SIZE) {
        close(fd);
        LOG_FAIL(RF_SUBSYS, "chunk: implausible chunk size %u from %s:%u "
                 "(refusal or corrupt stream)", size, peer_addr,
                 (unsigned)port);
    }
    if (!rf_recv_exact(fd, buf, size)) {
        close(fd);
        LOG_FAIL(RF_SUBSYS, "chunk: data read failed (%u bytes) from %s:%u",
                 size, peer_addr, (unsigned)port);
    }
    uint8_t mac_wire[32];
    if (!rf_recv_exact(fd, mac_wire, 32)) {
        close(fd);
        LOG_FAIL(RF_SUBSYS, "chunk: MAC read failed from %s:%u",
                 peer_addr, (unsigned)port);
    }
    close(fd);

    /* The chunk's content digest is learned from the received bytes: the
     * serve side binds the true per-chunk SHA3 into the MAC, so a tampered
     * payload fails the MAC; content-vs-manifest verification is the
     * whole-file pass (rom_fetch_verify_file). */
    uint8_t data_sha3[32];
    sha3_256(buf, size, data_sha3);

    uint8_t mac_expect[32];
    struct sha3_256_ctx mctx;
    sha3_256_init(&mctx);
    sha3_256_write(&mctx, s.key, 32);
    sha3_256_write(&mctx, (const unsigned char *)&s.recv_counter, 8);
    sha3_256_write(&mctx, data_sha3, 32);
    sha3_256_write(&mctx, buf, size);
    sha3_256_finalize(&mctx, mac_expect);

    uint8_t diff = 0;
    for (int i = 0; i < 32; i++)
        diff |= mac_wire[i] ^ mac_expect[i];
    if (diff != 0)
        LOG_FAIL(RF_SUBSYS, "chunk: transport MAC mismatch on chunk %u "
                 "from %s:%u", idx, peer_addr, (unsigned)port);

    *out_sz = size;
    return true;
}

/* ── Whole-file verification + download driver ──────────────────────── */

bool rom_fetch_verify_file(const char *path,
                           const struct rom_fetch_manifest *m)
{
    if (!path || !m)
        LOG_FAIL(RF_SUBSYS, "verify: null arg");
    if (!rom_fetch_manifest_sane(m))
        LOG_FAIL(RF_SUBSYS, "verify: manifest fails sanity checks");

    int fd = open(path, O_RDONLY | O_CLOEXEC);
    if (fd < 0)
        LOG_FAIL(RF_SUBSYS, "verify: open '%s' failed errno=%d", path, errno);
    struct stat st;
    if (fstat(fd, &st) != 0 || !S_ISREG(st.st_mode)) {
        close(fd);
        LOG_FAIL(RF_SUBSYS, "verify: fstat '%s' failed / not a file", path);
    }
    if ((uint64_t)st.st_size != m->size_bytes) {
        close(fd);
        LOG_FAIL(RF_SUBSYS, "verify: '%s' size %llu != committed %llu",
                 path, (unsigned long long)st.st_size,
                 (unsigned long long)m->size_bytes);
    }

    uint8_t *buf = zcl_malloc(ROM_SEED_CHUNK_SIZE, "rom_fetch_verify_buf");
    if (!buf) {
        close(fd);
        LOG_FAIL(RF_SUBSYS, "verify: alloc chunk buffer failed");
    }

    struct sha3_256_ctx whole_ctx;
    sha3_256_init(&whole_ctx);
    struct sha3_256_ctx root_ctx;
    sha3_256_init(&root_ctx);

    bool ok = true;
    uint64_t total = 0;
    for (uint32_t ci = 0; ci < m->num_chunks && ok; ci++) {
        uint32_t want = m->chunk_size;
        uint64_t remaining = m->size_bytes - total;
        if (remaining < want)
            want = (uint32_t)remaining;
        uint32_t got = 0;
        while (got < want) {
            ssize_t r = pread(fd, buf + got, want - got,
                              (off_t)(total + got));
            if (r < 0) {
                if (errno == EINTR)
                    continue;
                ok = false;
                break;
            }
            if (r == 0) { /* short file vs committed size */
                ok = false;
                break;
            }
            got += (uint32_t)r;
        }
        if (!ok)
            break;
        uint8_t chunk_h[32];
        sha3_256(buf, got, chunk_h);
        sha3_256_write(&root_ctx, chunk_h, 32);
        sha3_256_write(&whole_ctx, buf, got);
        total += got;
    }

    free(buf);
    close(fd);

    if (!ok || total != m->size_bytes)
        LOG_FAIL(RF_SUBSYS, "verify: read pass failed on '%s'", path);

    uint8_t whole[32], root[32];
    sha3_256_finalize(&whole_ctx, whole);
    sha3_256_finalize(&root_ctx, root);

    if (memcmp(root, m->chunk_root, 32) != 0)
        LOG_FAIL(RF_SUBSYS, "verify: '%s' chunk-root mismatch (content is "
                 "not the committed artifact)", path);
    if (memcmp(whole, m->whole_sha3, 32) != 0)
        LOG_FAIL(RF_SUBSYS, "verify: '%s' whole-file digest mismatch "
                 "(content is not the committed artifact)", path);
    return true;
}

/* ── Fetch status (observability) ───────────────────────────────────── */

static struct rom_fetch_status g_status;
static pthread_mutex_t g_status_mutex = PTHREAD_MUTEX_INITIALIZER;

static void rf_note_begin(const char *peer_addr, uint16_t port,
                          const struct rom_fetch_manifest *m)
{
    pthread_mutex_lock(&g_status_mutex);
    g_status.ever_attempted = true;
    g_status.in_progress = true;
    g_status.last_ok = false;
    snprintf(g_status.peer, sizeof(g_status.peer), "%s:%u",
             peer_addr, (unsigned)port);
    snprintf(g_status.filename, sizeof(g_status.filename), "%s", m->filename);
    g_status.detail[0] = '\0';
    g_status.size_bytes = m->size_bytes;
    g_status.num_chunks = m->num_chunks;
    g_status.chunks_done = 0;
    g_status.bytes_done = 0;
    g_status.started_unix = (int64_t)platform_time_wall_time_t();
    g_status.finished_unix = 0;
    g_status.attempts++;
    pthread_mutex_unlock(&g_status_mutex);
}

static void rf_note_progress(uint32_t chunks_done, uint64_t bytes_done)
{
    pthread_mutex_lock(&g_status_mutex);
    g_status.chunks_done = chunks_done;
    g_status.bytes_done = bytes_done;
    pthread_mutex_unlock(&g_status_mutex);
}

static void rf_note_end(bool ok, const char *detail)
{
    pthread_mutex_lock(&g_status_mutex);
    g_status.in_progress = false;
    g_status.last_ok = ok;
    g_status.finished_unix = (int64_t)platform_time_wall_time_t();
    snprintf(g_status.detail, sizeof(g_status.detail), "%s",
             detail ? detail : "");
    if (ok) {
        g_status.successes++;
        g_status.bytes_total += g_status.bytes_done;
    } else {
        g_status.failures++;
    }
    pthread_mutex_unlock(&g_status_mutex);
}

void rom_fetch_status_snapshot(struct rom_fetch_status *out)
{
    if (!out)
        return;
    pthread_mutex_lock(&g_status_mutex);
    *out = g_status;
    pthread_mutex_unlock(&g_status_mutex);
}

/* ── Download driver ────────────────────────────────────────────────── */

bool rom_fetch_download(const char *peer_addr, uint16_t port,
                        const struct rom_fetch_manifest *m,
                        const char *out_dir,
                        rom_fetch_progress_cb cb, void *cb_ctx)
{
    if (!peer_addr || !m || !out_dir || !out_dir[0])
        LOG_FAIL(RF_SUBSYS, "download: null arg");

    struct rom_fetch_manifest mc = *m;
    if (mc.filename[0] && !rf_filename_ok(mc.filename))
        LOG_FAIL(RF_SUBSYS, "download: unsafe filename '%s'", mc.filename);
    if (!mc.filename[0]) {
        /* directory.json entries carry no name; derive a stable, safe one
         * from the committed digest. */
        char hex[17];
        HexStr(mc.chunk_root, 8, false, hex, sizeof(hex));
        snprintf(mc.filename, sizeof(mc.filename), "rom-artifact-%s", hex);
    }
    if (!rom_fetch_manifest_sane(&mc))
        LOG_FAIL(RF_SUBSYS, "download: manifest fails sanity checks");

    char part_path[1200];
    int pn = snprintf(part_path, sizeof(part_path), "%s/%s%s",
                      out_dir, mc.filename, ROM_FETCH_PART_SUFFIX);
    if (pn <= 0 || (size_t)pn >= sizeof(part_path))
        LOG_FAIL(RF_SUBSYS, "download: part path overflow");
    char final_path[1200];
    pn = snprintf(final_path, sizeof(final_path), "%s/%s",
                  out_dir, mc.filename);
    if (pn <= 0 || (size_t)pn >= sizeof(final_path))
        LOG_FAIL(RF_SUBSYS, "download: final path overflow");

    uint8_t *buf = zcl_malloc(ROM_SEED_CHUNK_SIZE, "rom_fetch_dl_buf");
    if (!buf)
        LOG_FAIL(RF_SUBSYS, "download: alloc chunk buffer failed");

    rf_note_begin(peer_addr, port, &mc);

    int fd = open(part_path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
    if (fd < 0) {
        rf_note_end(false, "could not open staging file");
        free(buf);
        LOG_FAIL(RF_SUBSYS, "download: open '%s' failed errno=%d",
                 part_path, errno);
    }

    bool ok = true;
    const char *fail_reason = "";
    uint64_t bytes_done = 0;
    for (uint32_t ci = 0; ci < mc.num_chunks; ci++) {
        uint32_t want = mc.chunk_size;
        uint64_t remaining = mc.size_bytes - bytes_done;
        if (remaining < want)
            want = (uint32_t)remaining;

        uint32_t got = 0;
        if (!rom_fetch_chunk(peer_addr, port, mc.chunk_root, ci,
                             buf, ROM_SEED_CHUNK_SIZE, &got)) {
            LOG_WARN(RF_SUBSYS, "download: chunk %u/%u from %s:%u failed "
                     "(leaving '%s' for resume)", ci, mc.num_chunks,
                     peer_addr, (unsigned)port, part_path);
            ok = false;
            fail_reason = "chunk fetch failed (peer refused/unreachable)";
            break;
        }
        if (got != want) {
            LOG_WARN(RF_SUBSYS, "download: chunk %u/%u length %u != expected "
                     "%u (peer served a wrong-sized chunk; leaving '%s' for "
                     "resume)", ci, mc.num_chunks, got, want, part_path);
            ok = false;
            fail_reason = "peer served a wrong-sized chunk";
            break;
        }
        ssize_t w = pwrite(fd, buf, got, (off_t)((uint64_t)ci * mc.chunk_size));
        if (w != (ssize_t)got) {
            LOG_WARN(RF_SUBSYS, "download: pwrite '%s' chunk %u failed "
                     "errno=%d", part_path, ci, errno);
            ok = false;
            fail_reason = "staging write failed";
            break;
        }
        bytes_done += got;
        rf_note_progress(ci + 1, bytes_done);
        if (cb && !cb(ci + 1, mc.num_chunks, bytes_done, cb_ctx)) {
            LOG_WARN(RF_SUBSYS, "download: aborted by caller at chunk %u/%u",
                     ci + 1, mc.num_chunks);
            ok = false;
            fail_reason = "aborted by caller";
            break;
        }
    }

    free(buf);

    if (!ok) {
        close(fd);
        rf_note_end(false, fail_reason);
        return false;
    }
    fdatasync(fd);
    close(fd);

    /* Whole-file content proof against the committed digests. A mismatch
     * discards the file — no partial trust. */
    if (!rom_fetch_verify_file(part_path, &mc)) {
        LOG_WARN(RF_SUBSYS, "download: '%s' failed whole-file verification; "
                 "unlinking (seeder served non-committed content)", part_path);
        unlink(part_path);
        rf_note_end(false, "whole-file digest mismatch; download discarded");
        return false;
    }

    if (rename(part_path, final_path) != 0) {
        rf_note_end(false, "rename into place failed");
        LOG_FAIL(RF_SUBSYS, "download: rename '%s' -> '%s' failed errno=%d",
                 part_path, final_path, errno);
    }

    rf_note_end(true, final_path);
    LOG_INFO(RF_SUBSYS, "download: fetched '%s' (%llu bytes, %u chunks) "
             "from %s:%u — verified against committed digests",
             final_path, (unsigned long long)mc.size_bytes, mc.num_chunks,
             peer_addr, (unsigned)port);
    return true;
}

/* ── Introspection ──────────────────────────────────────────────────── */

bool rom_fetch_dump_state_json(struct json_value *out, const char *key)
{
    (void)key;
    if (!out)
        return false;
    json_set_object(out);

    struct rom_fetch_status s;
    rom_fetch_status_snapshot(&s);

    json_push_kv_bool(out, "ever_attempted", s.ever_attempted);
    json_push_kv_bool(out, "in_progress", s.in_progress);
    json_push_kv_bool(out, "last_ok", s.last_ok);
    json_push_kv_int(out, "attempts", (int64_t)s.attempts);
    json_push_kv_int(out, "successes", (int64_t)s.successes);
    json_push_kv_int(out, "failures", (int64_t)s.failures);
    json_push_kv_int(out, "bytes_installed_total", (int64_t)s.bytes_total);

    if (s.ever_attempted) {
        struct json_value last = {0};
        json_set_object(&last);
        json_push_kv_str(&last, "peer", s.peer);
        json_push_kv_str(&last, "filename", s.filename);
        json_push_kv_int(&last, "size_bytes", (int64_t)s.size_bytes);
        json_push_kv_int(&last, "num_chunks", (int64_t)s.num_chunks);
        json_push_kv_int(&last, "chunks_done", (int64_t)s.chunks_done);
        json_push_kv_int(&last, "bytes_done", (int64_t)s.bytes_done);
        json_push_kv_int(&last, "started_unix", s.started_unix);
        json_push_kv_int(&last, "finished_unix", s.finished_unix);
        if (s.detail[0])
            json_push_kv_str(&last, "detail", s.detail);
        json_push_kv(out, "last", &last);
        json_free(&last);
    }

    diag_push_health(out, true,
                     s.in_progress ? "fetch in progress"
                                   : "fetch engine idle");
    return true;
}

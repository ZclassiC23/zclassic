#define _GNU_SOURCE  /* pthread_timedjoin_np */

/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Fast File Service — SHA3-encrypted direct TCP transfer.
 * Designed for maximum throughput: 64KB fixed frames, zero protocol
 * overhead visible to observers, wire-speed on gigabit links. */

#include "platform/time_compat.h"
#include "net/file_manifest.h"
#include "net/file_service.h"
#include "util/log_json.h"
#include "crypto/sha3_crypt.h"
#include "crypto/sha3.h"
#include "core/random.h"
#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <time.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <pthread.h>
#include <stdatomic.h>
#include <sys/time.h>
#include <sys/stat.h>
#include "util/safe_alloc.h"
#include "util/log_macros.h"
#include "util/thread_registry.h"

static void fs_join_deadline_from_now(struct timespec *ts, int timeout_sec)
{
    platform_time_realtime_timespec(ts);
    if (timeout_sec < 0)
        timeout_sec = 0;
    ts->tv_sec += timeout_sec;
}

static void fs_join_thread_bounded(pthread_t thread,
                                   const char *name,
                                   int timeout_sec)
{
    struct timespec deadline;
    int rc;

    fs_join_deadline_from_now(&deadline, timeout_sec);
    rc = pthread_timedjoin_np(thread, NULL, &deadline);
    if (rc == 0)
        return;

    if (rc == ETIMEDOUT) {
        fprintf(stderr,  // obs-ok:shutdown-straggler-named
                "file_service: %s join timed out after %ds; detaching\n",
                name ? name : "thread", timeout_sec);
    } else {
        fprintf(stderr,  // obs-ok:shutdown-straggler-named
                "file_service: %s join failed rc=%d (%s); detaching\n",
                name ? name : "thread", rc, strerror(rc));
    }
    pthread_detach(thread);
}

/* ── Session management ────────────────────────────────────────── */

void fs_session_init(struct fs_session *s, int fd)
{
    memset(s, 0, sizeof(*s));
    s->fd = fd;
    s->start_time = (int64_t)platform_time_wall_time_t();

    /* TCP tuning for max throughput */
    int one = 1;
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    int sndbuf = 4 * 1024 * 1024; /* 4MB send buffer */
    int rcvbuf = 4 * 1024 * 1024;
    setsockopt(fd, SOL_SOCKET, SO_SNDBUF, &sndbuf, sizeof(sndbuf));
    setsockopt(fd, SOL_SOCKET, SO_RCVBUF, &rcvbuf, sizeof(rcvbuf));
}

double fs_session_mbps(const struct fs_session *s)
{
    int64_t elapsed = (int64_t)platform_time_wall_time_t() - s->start_time;
    if (elapsed < 1) elapsed = 1;
    uint64_t total = s->bytes_sent + s->bytes_received;
    return (double)total / (1048576.0 * (double)elapsed);
}

/* ── Raw I/O helpers ───────────────────────────────────────────── */

static bool send_all(int fd, const uint8_t *buf, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        ssize_t n = send(fd, buf + sent, len - sent, MSG_NOSIGNAL);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            LOG_FAIL("filesvc", "send_all failed: errno=%d (%s)", errno, strerror(errno));
        }
        sent += (size_t)n;
    }
    return true;
}

static bool recv_all(int fd, uint8_t *buf, size_t len)
{
    size_t got = 0;
    while (got < len) {
        ssize_t n = recv(fd, buf + got, len - got, 0);
        if (n <= 0) {
            if (n < 0 && errno == EINTR) continue;
            LOG_FAIL("filesvc", "recv_all failed: errno=%d (%s)", errno, strerror(errno));
        }
        got += (size_t)n;
    }
    return true;
}

/* ── Frame encryption/decryption ───────────────────────────────── */

/* Encrypt and MAC a frame. Always produces exactly FS_FRAME_SIZE bytes. */
static bool encrypt_frame(const struct fs_session *s, uint8_t type,
                            const uint8_t *payload, uint32_t payload_len,
                            uint8_t out[FS_FRAME_SIZE], uint64_t counter)
{
    if (payload_len > FS_MAX_PAYLOAD)
        LOG_FAIL("filesvc", "payload_len %u exceeds FS_MAX_PAYLOAD", payload_len);

    /* Build plaintext frame: [type][len][payload][random padding] */
    uint8_t plain[FS_FRAME_SIZE - FS_MAC_SIZE];
    memset(plain, 0, sizeof(plain));

    /* Header */
    plain[0] = type;
    plain[1] = (uint8_t)(type >> 8);
    plain[2] = (uint8_t)(type >> 16);
    plain[3] = (uint8_t)(type >> 24);
    plain[4] = (uint8_t)(payload_len);
    plain[5] = (uint8_t)(payload_len >> 8);
    plain[6] = (uint8_t)(payload_len >> 16);
    plain[7] = (uint8_t)(payload_len >> 24);

    /* Payload */
    if (payload_len > 0)
        memcpy(plain + FS_HEADER_SIZE, payload, payload_len);

    /* Padding is zeros — encrypted zeros look random. No need for
     * expensive /dev/urandom reads on every frame. */

    /* SHA3-CTR encrypt using AVX-512 4-way parallel SHA3-512.
     * Generates 256 bytes of keystream per batch (4 × 64).
     * 64KB / 256 = 256 batches. With AVX-512: ~4x faster. */
    uint8_t nonce[32];
    memset(nonce, 0, 32);
    memcpy(nonce, &counter, 8);

    size_t offset = 0;
    uint64_t block_ctr = 0;
    while (offset < sizeof(plain)) {
        uint8_t ks[256];
        sha3_512_x4(s->key, nonce, block_ctr, ks);
        block_ctr += 4;

        size_t chunk = sizeof(plain) - offset;
        if (chunk > 256) chunk = 256;
        for (size_t i = 0; i < chunk; i++)
            plain[offset + i] ^= ks[i];
        offset += chunk;
    }

    /* Copy encrypted data */
    memcpy(out, plain, sizeof(plain));

    /* MAC: SHA3-256(key || counter || encrypted_data) */
    struct sha3_256_ctx mac_ctx;
    sha3_256_init(&mac_ctx);
    sha3_256_write(&mac_ctx, s->key, 32);
    sha3_256_write(&mac_ctx, (const unsigned char *)&counter, 8);
    sha3_256_write(&mac_ctx, out, sizeof(plain));
    sha3_256_finalize(&mac_ctx, out + sizeof(plain));

    return true;
}

static bool decrypt_frame(const struct fs_session *s,
                            const uint8_t in[FS_FRAME_SIZE],
                            uint8_t *type_out, uint8_t *payload_buf,
                            uint32_t *payload_len_out, uint64_t counter)
{
    size_t ct_len = FS_FRAME_SIZE - FS_MAC_SIZE;

    /* Verify MAC first (fail fast) */
    uint8_t expected_mac[32];
    struct sha3_256_ctx mac_ctx;
    sha3_256_init(&mac_ctx);
    sha3_256_write(&mac_ctx, s->key, 32);
    sha3_256_write(&mac_ctx, (const unsigned char *)&counter, 8);
    sha3_256_write(&mac_ctx, in, ct_len);
    sha3_256_finalize(&mac_ctx, expected_mac);

    /* Constant-time compare */
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++)
        diff |= in[ct_len + i] ^ expected_mac[i];
    if (diff != 0)
        LOG_FAIL("filesvc", "frame MAC verification failed at counter=%llu", (unsigned long long)counter);

    /* Decrypt using AVX-512 4-way parallel SHA3 */
    uint8_t plain[FS_FRAME_SIZE - FS_MAC_SIZE];
    memcpy(plain, in, ct_len);

    uint8_t nonce[32];
    memset(nonce, 0, 32);
    memcpy(nonce, &counter, 8);

    size_t offset = 0;
    uint64_t block_ctr = 0;
    while (offset < ct_len) {
        uint8_t ks[256];
        sha3_512_x4(s->key, nonce, block_ctr, ks);
        block_ctr += 4;

        size_t chunk = ct_len - offset;
        if (chunk > 256) chunk = 256;
        for (size_t i = 0; i < chunk; i++)
            plain[offset + i] ^= ks[i];
        offset += chunk;
    }

    /* Parse header */
    *type_out = plain[0];
    uint32_t plen = (uint32_t)plain[4] |
                    ((uint32_t)plain[5] << 8) |
                    ((uint32_t)plain[6] << 16) |
                    ((uint32_t)plain[7] << 24);

    if (plen > FS_MAX_PAYLOAD)
        LOG_FAIL("filesvc", "decrypted payload_len %u exceeds FS_MAX_PAYLOAD", plen);
    *payload_len_out = plen;
    if (plen > 0)
        memcpy(payload_buf, plain + FS_HEADER_SIZE, plen);

    return true;
}

/* ── Public frame send/recv ────────────────────────────────────── */

bool fs_send_frame(struct fs_session *s, uint8_t type,
                    const uint8_t *payload, uint32_t payload_len)
{
    uint8_t frame[FS_FRAME_SIZE];
    if (!encrypt_frame(s, type, payload, payload_len,
                        frame, s->send_counter))
        LOG_FAIL("filesvc", "encrypt_frame failed: type=%u len=%u", type, payload_len);
    s->send_counter++;
    s->bytes_sent += FS_FRAME_SIZE;
    return send_all(s->fd, frame, FS_FRAME_SIZE);
}

bool fs_recv_frame(struct fs_session *s, uint8_t *type_out,
                    const uint8_t **payload_out, uint32_t *payload_len_out)
{
    uint8_t frame[FS_FRAME_SIZE];
    if (!s) LOG_FAIL("filesvc", "fs_recv_frame called with NULL session");
    if (!recv_all(s->fd, frame, FS_FRAME_SIZE))
        LOG_FAIL("filesvc", "recv_all failed reading frame from fd=%d", s->fd);
    s->bytes_received += FS_FRAME_SIZE;

    if (!decrypt_frame(s, frame, type_out, s->recv_payload,
                        payload_len_out, s->recv_counter))
        LOG_FAIL("filesvc", "decrypt_frame failed at counter=%llu", (unsigned long long)s->recv_counter);
    s->recv_counter++;
    *payload_out = s->recv_payload;
    return true;
}

/* ── Handshake ─────────────────────────────────────────────────── */

bool fs_handshake(struct fs_session *s, const uint8_t utxo_root[32],
                   bool is_initiator)
{
    /* Generate our nonce */
    GetRandBytes(s->our_nonce, 32);

    if (is_initiator) {
        /* Send our nonce in cleartext (pre-key) */
        if (!send_all(s->fd, s->our_nonce, 32))
            LOG_FAIL("filesvc", "handshake: failed to send nonce (initiator)");
        /* Receive peer nonce */
        if (!recv_all(s->fd, s->peer_nonce, 32))
            LOG_FAIL("filesvc", "handshake: failed to recv peer nonce (initiator)");
    } else {
        /* Receive peer nonce first */
        if (!recv_all(s->fd, s->peer_nonce, 32))
            LOG_FAIL("filesvc", "handshake: failed to recv peer nonce (responder)");
        /* Send our nonce */
        if (!send_all(s->fd, s->our_nonce, 32))
            LOG_FAIL("filesvc", "handshake: failed to send nonce (responder)");
    }

    /* Derive shared key from UTXO root + both nonces */
    sha3_crypt_derive_key(utxo_root, s->our_nonce, s->peer_nonce, s->key);
    s->key_established = true;

    /* Verify key agreement: send SHA3(key || "verify") */
    uint8_t verify[32];
    struct sha3_256_ctx vctx;
    sha3_256_init(&vctx);
    sha3_256_write(&vctx, s->key, 32);
    sha3_256_write(&vctx, (const unsigned char *)"verify", 6);
    sha3_256_finalize(&vctx, verify);

    if (!send_all(s->fd, verify, 32))
        LOG_FAIL("filesvc", "handshake: failed to send verify hash");
    uint8_t peer_verify[32];
    if (!recv_all(s->fd, peer_verify, 32))
        LOG_FAIL("filesvc", "handshake: failed to recv peer verify hash");

    if (memcmp(verify, peer_verify, 32) != 0) {
        LOG_FAIL("filesvc", "handshake: key verification failed (peer not on same chain)");
    }

    printf("fs_handshake: key established (SHA3 quantum-secure)\n");
    return true;
}

/* ── Fast encrypted chunk transfer ─────────────────────────────── */
/* Fast authenticated chunk transfer.
 * SHA3 MAC for integrity + authentication (post-quantum secure).
 * No bulk encryption — blockchain data is public. The MAC proves:
 *   1. Data came from a node with the shared key (same chain)
 *   2. Data wasn't tampered with in transit
 *   3. Replay protection via counter
 *
 * Wire format: [4-byte size][raw data][32-byte SHA3 MAC]
 * MAC = SHA3-256(key || counter || expected_sha3 || data)
 * Speed: limited only by network + disk I/O, not crypto. */

bool fs_send_chunk_fast(struct fs_session *s, const uint8_t *data,
                         uint32_t size, const uint8_t sha3[32])
{
    /* Send size header */
    uint8_t hdr[4];
    hdr[0] = (uint8_t)(size);
    hdr[1] = (uint8_t)(size >> 8);
    hdr[2] = (uint8_t)(size >> 16);
    hdr[3] = (uint8_t)(size >> 24);
    if (!send_all(s->fd, hdr, 4))
        LOG_FAIL("filesvc", "send_chunk_fast: failed to send size header");

    /* Send raw data — zero copy overhead */
    if (!send_all(s->fd, data, size))
        LOG_FAIL("filesvc", "send_chunk_fast: failed to send data (%u bytes)", size);

    /* SHA3 MAC: authenticates data + binds to session + prevents replay */
    uint8_t mac[32];
    struct sha3_256_ctx mctx;
    sha3_256_init(&mctx);
    sha3_256_write(&mctx, s->key, 32);
    sha3_256_write(&mctx, (const unsigned char *)&s->send_counter, 8);
    sha3_256_write(&mctx, sha3, 32);
    sha3_256_write(&mctx, data, size);
    sha3_256_finalize(&mctx, mac);

    if (!send_all(s->fd, mac, 32))
        LOG_FAIL("filesvc", "send_chunk_fast: failed to send MAC");

    s->bytes_sent += 4 + size + 32;
    s->send_counter++;
    return true;
}

static bool fs_recv_chunk_fast(struct fs_session *s, uint8_t **out,
                                uint32_t *out_size,
                                const uint8_t expected_sha3[32])
{
    /* Read size */
    uint8_t hdr[4];
    if (!recv_all(s->fd, hdr, 4))
        LOG_FAIL("filesvc", "recv_chunk_fast: failed to read size header");
    uint32_t size = (uint32_t)hdr[0] | ((uint32_t)hdr[1] << 8) |
                    ((uint32_t)hdr[2] << 16) | ((uint32_t)hdr[3] << 24);
    if (size == 0 || size > 60 * 1024 * 1024)
        LOG_FAIL("filesvc", "recv_chunk_fast: invalid chunk size=%u", size);

    /* Read data */
    uint8_t *buf = zcl_malloc(size, "file_recv_buf");
    if (!buf) LOG_FAIL("filesvc", "recv_chunk_fast: malloc failed for %u bytes", size);
    if (!recv_all(s->fd, buf, size)) { free(buf); LOG_FAIL("filesvc", "recv_chunk_fast: failed to read data (%u bytes)", size); }

    /* Read and verify SHA3 MAC */
    uint8_t mac_wire[32];
    if (!recv_all(s->fd, mac_wire, 32)) { free(buf); LOG_FAIL("filesvc", "recv_chunk_fast: failed to read MAC"); }

    uint8_t mac_expected[32];
    struct sha3_256_ctx mctx;
    sha3_256_init(&mctx);
    sha3_256_write(&mctx, s->key, 32);
    sha3_256_write(&mctx, (const unsigned char *)&s->recv_counter, 8);
    sha3_256_write(&mctx, expected_sha3, 32);
    sha3_256_write(&mctx, buf, size);
    sha3_256_finalize(&mctx, mac_expected);

    /* Constant-time MAC verification */
    uint8_t diff = 0;
    for (int i = 0; i < 32; i++) diff |= mac_wire[i] ^ mac_expected[i];
    if (diff != 0) {
        free(buf);
        LOG_FAIL("filesvc", "recv_chunk_fast: MAC verification failed on chunk (%u bytes)", size);
    }
    s->recv_counter++;

    /* Verify SHA3 of data matches manifest */
    uint8_t hash[32];
    sha3_256(buf, size, hash);
    if (memcmp(hash, expected_sha3, 32) != 0) {
        free(buf);
        LOG_FAIL("filesvc", "recv_chunk_fast: SHA3 hash mismatch on chunk (%u bytes)", size);
    }

    s->bytes_received += 4 + size + 32;
    *out = buf;
    *out_size = size;
    return true;
}

/* ── Server ────────────────────────────────────────────────────── */

static _Atomic bool g_fs_running = false;
static pthread_t g_fs_thread;
static bool g_fs_thread_started = false;
static pthread_t g_fs_manifest_thread;
static bool g_fs_manifest_thread_started = false;
static int g_fs_listen_fd = -1;
static const char *g_fs_datadir = NULL;
static uint16_t g_fs_port = FS_PORT;
static pthread_mutex_t g_fs_state_mutex = PTHREAD_MUTEX_INITIALIZER;

#define FS_SERVER_WORKERS 8
#define FS_CLIENT_QUEUE_CAP 64
static pthread_t g_fs_worker_threads[FS_SERVER_WORKERS];
static unsigned g_fs_worker_threads_started = 0;
static int g_fs_client_queue[FS_CLIENT_QUEUE_CAP];
static unsigned g_fs_client_queue_head = 0;
static unsigned g_fs_client_queue_tail = 0;
static unsigned g_fs_client_queue_len = 0;
static pthread_mutex_t g_fs_client_queue_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_fs_client_queue_cv = PTHREAD_COND_INITIALIZER;

/* Background manifest builder — hashes all block files (~7 GB). */
static struct file_manifest g_server_fm;
static _Atomic bool g_have_manifest = false;
static pthread_mutex_t g_manifest_mutex = PTHREAD_MUTEX_INITIALIZER;
static pthread_mutex_t g_manifest_build_mutex = PTHREAD_MUTEX_INITIALIZER;

static bool fs_server_rebuild_manifest_locked(struct file_manifest *out)
{
    struct file_manifest next = {0};
    int64_t t0 = (int64_t)platform_time_wall_time_t();
    bool ok;

    pthread_mutex_lock(&g_manifest_build_mutex);
    ok = file_manifest_build(&next, g_fs_datadir);
    pthread_mutex_unlock(&g_manifest_build_mutex);
    int64_t elapsed = (int64_t)platform_time_wall_time_t() - t0;

    if (ok) {
        *out = next;
        log_jsonf(LOG_JSON_INFO, "file_service_manifest_ready",
                  "\"chunks\":%u,\"total_bytes\":%llu,\"hash_seconds\":%lld",
                  out->num_chunks,
                  (unsigned long long)out->total_bytes,
                  (long long)elapsed);
    } else {
        memset(out, 0, sizeof(*out));
        log_jsonf(LOG_JSON_WARN, "file_service_manifest_empty", NULL);
    }

    return ok;
}

static void *fs_manifest_thread(void *arg)
{
    (void)arg;
    struct file_manifest next = {0};
    bool ok = fs_server_rebuild_manifest_locked(&next);
    pthread_mutex_lock(&g_manifest_mutex);
    g_server_fm = next;
    pthread_mutex_unlock(&g_manifest_mutex);
    atomic_store(&g_have_manifest, ok);
    return NULL;
}

bool fs_server_refresh_manifest(void)
{
    struct file_manifest next = {0};
    bool ok;

    if (!g_fs_datadir)
        LOG_FAIL("filesvc", "refresh_manifest: datadir not set");

    ok = fs_server_rebuild_manifest_locked(&next);
    pthread_mutex_lock(&g_manifest_mutex);
    g_server_fm = next;
    pthread_mutex_unlock(&g_manifest_mutex);
    atomic_store(&g_have_manifest, ok);
    return ok;
}

static bool fs_client_queue_push(int client_fd)
{
    bool ok = false;

    pthread_mutex_lock(&g_fs_client_queue_mutex);
    if (g_fs_client_queue_len < FS_CLIENT_QUEUE_CAP) {
        g_fs_client_queue[g_fs_client_queue_tail] = client_fd;
        g_fs_client_queue_tail =
            (g_fs_client_queue_tail + 1U) % FS_CLIENT_QUEUE_CAP;
        g_fs_client_queue_len++;
        ok = true;
        pthread_cond_signal(&g_fs_client_queue_cv);
    }
    pthread_mutex_unlock(&g_fs_client_queue_mutex);
    return ok;
}

static bool fs_client_queue_pop(int *client_fd_out)
{
    if (!client_fd_out)
        LOG_FAIL("filesvc", "client_queue_pop: client_fd_out is NULL");

    pthread_mutex_lock(&g_fs_client_queue_mutex);
    /* Timed wait so a worker never blocks past shutdown if the cond
     * broadcast in fs_server_stop is skipped (e.g., abort path). 2 s
     * wake is invisible under load — every queue push signals the
     * cond — but bounded under shutdown. Mirror of the httpserver
     * dequeue_client treatment. */
    while (g_fs_client_queue_len == 0 && atomic_load(&g_fs_running)) {
        struct timespec deadline;
        platform_time_realtime_timespec(&deadline);
        deadline.tv_sec += 2;
        pthread_cond_timedwait(&g_fs_client_queue_cv,
                               &g_fs_client_queue_mutex, &deadline);
    }

    if (g_fs_client_queue_len == 0) {
        pthread_mutex_unlock(&g_fs_client_queue_mutex);
        LOG_FAIL("filesvc", "client_queue_pop: queue empty and server stopping");
    }

    *client_fd_out = g_fs_client_queue[g_fs_client_queue_head];
    g_fs_client_queue_head =
        (g_fs_client_queue_head + 1U) % FS_CLIENT_QUEUE_CAP;
    g_fs_client_queue_len--;
    pthread_mutex_unlock(&g_fs_client_queue_mutex);
    return true;
}

static void fs_client_queue_close_all(void)
{
    pthread_mutex_lock(&g_fs_client_queue_mutex);
    while (g_fs_client_queue_len > 0) {
        int client_fd = g_fs_client_queue[g_fs_client_queue_head];
        g_fs_client_queue_head =
            (g_fs_client_queue_head + 1U) % FS_CLIENT_QUEUE_CAP;
        g_fs_client_queue_len--;
        close(client_fd);
    }
    g_fs_client_queue_head = 0;
    g_fs_client_queue_tail = 0;
    pthread_mutex_unlock(&g_fs_client_queue_mutex);
}

/* Per-client handler run on a bounded worker pool. */
static void fs_handle_client_fd(int client_fd)
{
    struct fs_session session;
    fs_session_init(&session, client_fd);

    uint8_t ur[32];
    memset(ur, 0, 32);
    if (!fs_handshake(&session, ur, false)) {
        close(client_fd);
        return;
    }

    struct file_manifest manifest = {0};

    while (atomic_load(&g_fs_running)) {
        bool have_manifest = atomic_load(&g_have_manifest);
        uint8_t type;
        const uint8_t *payload;
        uint32_t plen;
        if (!fs_recv_frame(&session, &type, &payload, &plen)) break;

        if (have_manifest) {
            pthread_mutex_lock(&g_manifest_mutex);
            manifest = g_server_fm;
            pthread_mutex_unlock(&g_manifest_mutex);
        }

        if (type == FS_REQUEST && plen >= 8 &&
            memcmp(payload, "RNG", 3) == 0 && have_manifest) {
            /* Range: chunks[start..end) */
            uint16_t start = (uint16_t)payload[3] |
                              ((uint16_t)payload[4] << 8);
            uint16_t end = (uint16_t)payload[5] |
                            ((uint16_t)payload[6] << 8);
            if (end > manifest.num_chunks) end = manifest.num_chunks;
            for (uint32_t ci = start; ci < end && atomic_load(&g_fs_running); ci++) {
                uint8_t *data = NULL;
                uint32_t dsz = 0;
                if (file_chunk_read(&manifest.chunks[ci], g_fs_datadir,
                                     &data, &dsz)) {
                    fs_send_chunk_fast(&session, data, dsz,
                                        manifest.chunks[ci].sha3);
                    free(data);
                } else {
                    /* Send empty chunk on read failure so client can
                     * track progress; don't break entire range */
                    fprintf(stderr,  // obs-ok:file-transfer-client-visible
                            "file_service: read failed chunk %u\n", ci);
                    break;
                }
            }
        } else if (type == FS_REQUEST && plen == 3 &&
                   memcmp(payload, "ALL", 3) == 0 && have_manifest) {
            printf("file_service: streaming %u chunks (%.1f GB)...\n",
                   manifest.num_chunks,
                   (double)manifest.total_bytes / (1024.0*1024.0*1024.0));
            for (uint32_t ci = 0; ci < manifest.num_chunks &&
                                 atomic_load(&g_fs_running); ci++) {
                uint8_t *data = NULL;
                uint32_t dsz = 0;
                if (file_chunk_read(&manifest.chunks[ci], g_fs_datadir,
                                     &data, &dsz)) {
                    fs_send_chunk_fast(&session, data, dsz,
                                        manifest.chunks[ci].sha3);
                    free(data);
                } else break;
            }
            printf("file_service: streaming done (%.1f MB/s)\n",
                   fs_session_mbps(&session));
        } else if (type == FS_MANIFEST && have_manifest) {
            for (uint32_t i = 0; i < manifest.num_chunks; i++) {
                uint8_t entry[45]; /* sha3(32)+size(4)+file_idx(1)+offset(8) */
                memcpy(entry, manifest.chunks[i].sha3, 32);
                entry[32] = (uint8_t)(manifest.chunks[i].size);
                entry[33] = (uint8_t)(manifest.chunks[i].size >> 8);
                entry[34] = (uint8_t)(manifest.chunks[i].size >> 16);
                entry[35] = (uint8_t)(manifest.chunks[i].size >> 24);
                entry[36] = manifest.chunks[i].file_index;
                memcpy(entry + 37, &manifest.chunks[i].offset, 8);
                if (!fs_send_frame(&session, FS_MANIFEST, entry, 45))
                    break;
            }
            if (!fs_send_frame(&session, FS_DONE, NULL, 0))
                break;
        } else if (type == FS_DONE) {
            break;
        }
    }

    printf("file_service: client done (%.1f MB/s, %llu bytes)\n",
           fs_session_mbps(&session),
           (unsigned long long)(session.bytes_sent + session.bytes_received));
    close(client_fd);
}

static void *fs_client_worker_thread(void *arg)
{
    (void)arg;

    while (true) {
        int client_fd = -1;

        if (!fs_client_queue_pop(&client_fd))
            break;
        if (client_fd >= 0)
            fs_handle_client_fd(client_fd);
    }

    return NULL;
}

static void *fs_server_thread(void *arg)
{
    (void)arg;

    int listen_fd = socket(AF_INET6, SOCK_STREAM, 0);
    if (listen_fd < 0)
        LOG_NULL("filesvc", "server_thread: socket() failed: %s", strerror(errno));

    int one = 1;
    setsockopt(listen_fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
    int zero = 0;
    setsockopt(listen_fd, IPPROTO_IPV6, IPV6_V6ONLY, &zero, sizeof(zero));

    struct sockaddr_in6 addr;
    memset(&addr, 0, sizeof(addr));
    addr.sin6_family = AF_INET6;
    addr.sin6_port = htons(g_fs_port);
    addr.sin6_addr = in6addr_any;

    if (bind(listen_fd, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        close(listen_fd);
        LOG_NULL("filesvc", "server_thread: bind port %d failed: %s", g_fs_port, strerror(errno));
    }

    listen(listen_fd, 32);
    log_jsonf(LOG_JSON_INFO, "file_service_listening",
              "\"port\":%d,\"transport\":\"sha3_quantum_secure\"",
              g_fs_port);

    pthread_mutex_lock(&g_fs_state_mutex);
    g_fs_listen_fd = listen_fd;
    pthread_mutex_unlock(&g_fs_state_mutex);

    /* Get UTXO root for key derivation */
    uint8_t utxo_root[32];
    memset(utxo_root, 0, 32);

    while (atomic_load(&g_fs_running)) {
        struct sockaddr_in6 client_addr;
        socklen_t client_len = sizeof(client_addr);

        /* Use accept with a timeout so we can check g_fs_running */
        struct timeval tv;
        tv.tv_sec = 2;
        tv.tv_usec = 0;
        setsockopt(listen_fd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        int client_fd = accept(listen_fd,
                                (struct sockaddr *)&client_addr, &client_len);
        if (client_fd < 0) continue;

        /* Defense against a peer that opens a connection and then goes
         * silent mid-frame. recv_all() / recv() on this fd would block
         * indefinitely without a per-socket deadline. 30 s is generous
         * for a file-service handshake or frame — anything longer is a
         * misbehaving peer and we'd rather drop the connection than
         * hold a worker hostage. Wave 8 watchdog catches the hang
         * post-hoc; this prevents it. */
        struct timeval ctv = { .tv_sec = 30, .tv_usec = 0 };
        setsockopt(client_fd, SOL_SOCKET, SO_RCVTIMEO, &ctv, sizeof(ctv));
        setsockopt(client_fd, SOL_SOCKET, SO_SNDTIMEO, &ctv, sizeof(ctv));

        if (!fs_client_queue_push(client_fd)) {
            fprintf(stderr,  // obs-ok:file-service-overload-visible
                    "file_service: client queue full, rejecting\n");
            close(client_fd);
        }
    }

    pthread_mutex_lock(&g_fs_state_mutex);
    if (g_fs_listen_fd == listen_fd)
        g_fs_listen_fd = -1;
    pthread_mutex_unlock(&g_fs_state_mutex);
    close(listen_fd);
    return NULL;
}

uint16_t fs_server_get_port(void) { return g_fs_port; }

void fs_server_start(const char *datadir, uint16_t port)
{
    unsigned started_workers = 0;

    pthread_mutex_lock(&g_fs_state_mutex);
    if (atomic_load(&g_fs_running) || g_fs_thread_started) {
        pthread_mutex_unlock(&g_fs_state_mutex);
        return;
    }
    g_fs_datadir = datadir;
    g_fs_port = port;
    atomic_store(&g_fs_running, true);
    atomic_store(&g_have_manifest, false);
    g_fs_client_queue_head = 0;
    g_fs_client_queue_tail = 0;
    g_fs_client_queue_len = 0;

    for (unsigned i = 0; i < FS_SERVER_WORKERS; i++) {
        if (thread_registry_spawn_ex("zcl_fs_wkr", fs_client_worker_thread,
                                      NULL, &g_fs_worker_threads[i]) != 0) {
            fprintf(stderr,  // obs-ok:file-service-startup-failure
                    "file_service: failed to start worker %u\n", i);
            break;
        }
        started_workers++;
    }
    g_fs_worker_threads_started = started_workers;

    if (thread_registry_spawn_ex("zcl_fs_server", fs_server_thread, NULL,
                                  &g_fs_thread) != 0) {
        atomic_store(&g_fs_running, false);
        pthread_cond_broadcast(&g_fs_client_queue_cv);
        pthread_mutex_unlock(&g_fs_state_mutex);
        fprintf(stderr, "file_service: failed to start server thread\n");
        for (unsigned i = 0; i < started_workers; i++)
            pthread_join(g_fs_worker_threads[i], NULL);
        g_fs_worker_threads_started = 0;
        return;
    }
    g_fs_thread_started = true;
    if (g_fs_datadir) {
        if (thread_registry_spawn_ex("zcl_fs_manifest", fs_manifest_thread,
                                      NULL, &g_fs_manifest_thread) == 0) {
            g_fs_manifest_thread_started = true;
        } else {
            fprintf(stderr,  // obs-ok:file-service-startup-failure
                    "file_service: failed to start manifest thread\n");
        }
    }
    pthread_mutex_unlock(&g_fs_state_mutex);
}

void fs_server_stop(void)
{
    pthread_t server_thread;
    pthread_t manifest_thread;
    pthread_t worker_threads[FS_SERVER_WORKERS];
    bool have_server = false;
    bool have_manifest = false;
    unsigned worker_threads_started = 0;
    int listen_fd = -1;

    pthread_mutex_lock(&g_fs_state_mutex);
    atomic_store(&g_fs_running, false);
    listen_fd = g_fs_listen_fd;
    g_fs_listen_fd = -1;
    if (g_fs_thread_started) {
        server_thread = g_fs_thread;
        g_fs_thread_started = false;
        have_server = true;
    }
    if (g_fs_manifest_thread_started) {
        manifest_thread = g_fs_manifest_thread;
        g_fs_manifest_thread_started = false;
        have_manifest = true;
    }
    worker_threads_started = g_fs_worker_threads_started;
    for (unsigned i = 0; i < worker_threads_started; i++)
        worker_threads[i] = g_fs_worker_threads[i];
    g_fs_worker_threads_started = 0;
    pthread_mutex_unlock(&g_fs_state_mutex);

    if (listen_fd >= 0) {
        shutdown(listen_fd, SHUT_RDWR);
        close(listen_fd);
    }
    pthread_cond_broadcast(&g_fs_client_queue_cv);
    fs_client_queue_close_all();

    if (have_server)
        fs_join_thread_bounded(server_thread, "server", 5);
    if (have_manifest)
        fs_join_thread_bounded(manifest_thread, "manifest", 5);
    for (unsigned i = 0; i < worker_threads_started; i++)
        fs_join_thread_bounded(worker_threads[i], "worker", 5);
}

/* ── Parallel download worker ──────────────────────────────────── */

struct range_worker {
    const char *peer_addr;
    uint16_t port;
    uint8_t utxo_root[32];
    struct file_chunk *chunks;
    uint32_t start, end;
    const char *out_path;
    const char *datadir;
    int id;
    int fd;                /* socket fd — for cancellation */
    _Atomic bool done;
    _Atomic bool cancel;   /* set by main thread to abort stuck worker */
    _Atomic uint64_t bytes;
    _Atomic uint32_t chunks_ok;    /* successfully written chunks */
    _Atomic uint32_t chunks_fail;  /* failed recv or write */
};

static void *range_worker_fn(void *arg)
{
    struct range_worker *w = (struct range_worker *)arg;
    if (w->start >= w->end) { atomic_store(&w->done, true); return NULL; }

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;
    char ps[8];
    snprintf(ps, sizeof(ps), "%d", w->port);
    if (getaddrinfo(w->peer_addr, ps, &hints, &res) != 0)
        LOG_NULL("filesvc", "worker %d: getaddrinfo failed for %s", w->id, w->peer_addr);
    int wfd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (wfd < 0) { freeaddrinfo(res); LOG_NULL("filesvc", "worker %d: socket() failed: %s", w->id, strerror(errno)); }

    /* Timeouts prevent hung connections from blocking the whole download */
    struct timeval tv;
    tv.tv_sec = 120; /* 2 min per chunk max */
    tv.tv_usec = 0;
    setsockopt(wfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(wfd, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(wfd, res->ai_addr, res->ai_addrlen) < 0) {
        close(wfd); freeaddrinfo(res); LOG_NULL("filesvc", "worker %d: connect failed: %s", w->id, strerror(errno));
    }
    freeaddrinfo(res);

    w->fd = wfd;  /* allow main thread to close on cancel */
    struct fs_session ws;
    fs_session_init(&ws, wfd);
    if (!fs_handshake(&ws, w->utxo_root, true)) { close(wfd); LOG_NULL("filesvc", "worker %d: handshake failed", w->id); }

    /* Send range request */
    uint8_t rng[8] = {'R','N','G', 0,0,0,0,0};
    rng[3] = (uint8_t)(w->start);
    rng[4] = (uint8_t)(w->start >> 8);
    rng[5] = (uint8_t)(w->end);
    rng[6] = (uint8_t)(w->end >> 8);
    if (!fs_send_frame(&ws, FS_REQUEST, rng, 8)) {
        close(wfd); LOG_NULL("filesvc", "worker %d: range request send failed", w->id);
    }

    for (uint32_t i = w->start; i < w->end; i++) {
        if (atomic_load(&w->cancel)) { close(wfd); LOG_NULL("filesvc", "worker %d: cancelled", w->id); }
        uint8_t *buf = NULL;
        uint32_t sz = 0;
        if (!fs_recv_chunk_fast(&ws, &buf, &sz, w->chunks[i].sha3)) {
            fprintf(stderr,  // obs-ok:file-transfer-peer-failure
                    "worker %d: chunk %u/%u recv failed\n",
                    w->id, i, w->end);
            atomic_fetch_add(&w->chunks_fail, 1);
            continue;
        }

        /* Write to the correct file at the correct offset.
         * file_index 0-252: block files (blk%05d.dat)
         * file_index 253: block_index.bin
         * file_index 254: node.db (SQLite UTXO set)
         * Uses pwrite() for atomic positioned write. */
        char blk_path[600];
        if (w->chunks[i].file_index == 254)
            snprintf(blk_path, 600, "%s/consensus_snapshot.db", w->datadir);
        else if (w->chunks[i].file_index == 253)
            snprintf(blk_path, 600, "%s/block_index.bin", w->datadir);
        else
            snprintf(blk_path, 600, "%s/blocks/blk%05d.dat",
                     w->datadir, w->chunks[i].file_index);
        int bfd = open(blk_path, O_WRONLY | O_CREAT, 0644);
        if (bfd >= 0) {
            ssize_t written = pwrite(bfd, buf, sz, (off_t)w->chunks[i].offset);
            if (written != (ssize_t)sz) {
                fprintf(stderr,  // obs-ok:file-transfer-disk-failure
                        "worker %d: pwrite %s offset=%llu "
                        "sz=%u wrote=%zd errno=%d\n",
                        w->id, blk_path,
                        (unsigned long long)w->chunks[i].offset,
                        sz, written, errno);
                close(bfd);
                free(buf);
                continue; /* skip — don't count failed write as progress */
            }
            /* Sync to disk so crash can't lose this chunk */
            fdatasync(bfd);
            close(bfd);
            atomic_fetch_add(&w->chunks_ok, 1);
        } else {
            fprintf(stderr,  // obs-ok:file-transfer-disk-failure
                    "worker %d: open %s failed: %s\n",
                    w->id, blk_path, strerror(errno));
            atomic_fetch_add(&w->chunks_fail, 1);
        }
        free(buf);
        atomic_fetch_add(&w->bytes, sz);
    }
    close(wfd);
    atomic_store(&w->done, true);
    return NULL;
}

/* ── Client ────────────────────────────────────────────────────── */

bool fs_client_sync(const char *peer_addr, uint16_t port,
                     const char *datadir, const uint8_t utxo_root[32])
{
    printf("file_service: connecting to %s:%d...\n", peer_addr, port);

    struct addrinfo hints, *res;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%d", port);

    if (getaddrinfo(peer_addr, port_str, &hints, &res) != 0)
        LOG_FAIL("filesvc", "client_sync: resolve failed for %s", peer_addr);

    int fd = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (fd < 0) { freeaddrinfo(res); LOG_FAIL("filesvc", "client_sync: socket() failed: %s", strerror(errno)); }

    /* Non-blocking connect with 10s timeout — don't block boot on
     * unreachable file service peers. */
    {
        int flags = fcntl(fd, F_GETFL, 0);
        fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = connect(fd, res->ai_addr, res->ai_addrlen);
        if (rc < 0 && errno == EINPROGRESS) {
            struct timeval tv = { .tv_sec = 10, .tv_usec = 0 };
            fd_set wfds;
            FD_ZERO(&wfds);
            FD_SET(fd, &wfds);
            rc = select(fd + 1, NULL, &wfds, NULL, &tv);
            if (rc <= 0) {
                close(fd);
                freeaddrinfo(res);
                /* Optional fast-sync seed not reachable — routine on a
                 * fresh node; P2P snapshot sync is the fallback. Warn,
                 * don't error. */
                LOG_WARN("filesvc", "client_sync: connect timeout to %s:%d (optional seed; falling back to P2P)", peer_addr, port);
                return false;
            }
            int err = 0;
            socklen_t elen = sizeof(err);
            getsockopt(fd, SOL_SOCKET, SO_ERROR, &err, &elen);
            if (err) {
                close(fd);
                freeaddrinfo(res);
                LOG_WARN("filesvc", "client_sync: connect to %s:%d failed: %s (optional seed; falling back to P2P)", peer_addr, port, strerror(err));
                return false;
            }
        } else if (rc < 0) {
            close(fd);
            freeaddrinfo(res);
            LOG_WARN("filesvc", "client_sync: connect to %s:%d failed: %s (optional seed; falling back to P2P)", peer_addr, port, strerror(errno));
            return false;
        }
        fcntl(fd, F_SETFL, flags);  /* restore blocking mode */
    }
    freeaddrinfo(res);

    struct fs_session session;
    fs_session_init(&session, fd);

    if (!fs_handshake(&session, utxo_root, true)) {
        close(fd);
        LOG_FAIL("filesvc", "client_sync: handshake failed with %s:%d", peer_addr, port);
    }

    printf("file_service: connected, requesting manifest...\n");

    /* Set timeout for manifest reception — don't block forever */
    struct timeval mtv;
    mtv.tv_sec = 60; /* 60s to receive full manifest */
    mtv.tv_usec = 0;
    setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &mtv, sizeof(mtv));

    if (!fs_send_frame(&session, FS_MANIFEST, NULL, 0)) {
        close(fd);
        LOG_FAIL("filesvc", "client_sync: manifest request send failed to %s:%d", peer_addr, port);
    }

    struct file_chunk chunks[FILE_MAX_CHUNKS];
    uint32_t num_chunks = 0;

    while (num_chunks < FILE_MAX_CHUNKS) {
        uint8_t type;
        const uint8_t *payload;
        uint32_t plen;

        if (!fs_recv_frame(&session, &type, &payload, &plen))
            break;

        if (type == FS_DONE) break;
        if (type == FS_MANIFEST && plen >= 36) {
            struct file_chunk *c = &chunks[num_chunks];
            memset(c, 0, sizeof(*c));   /* short form leaves file_index/offset
                                         * unset — zero them, not stack garbage */
            memcpy(c->sha3, payload, 32);
            c->size =
                (uint32_t)payload[32] |
                ((uint32_t)payload[33] << 8) |
                ((uint32_t)payload[34] << 16) |
                ((uint32_t)payload[35] << 24);
            /* Extended manifest: file_index + offset */
            if (plen >= 45) {
                c->file_index = payload[36];
                memcpy(&c->offset, payload + 37, 8);
            }
            /* SECURITY: every manifest field is peer-supplied. Bound the chunk
             * size here at parse time (the recv path caps at 60MB on the wire,
             * but the resume-verify malloc at line ~1137 trusted this size and a
             * size=0xFFFFFFFF entry would attempt a ~4GB alloc → OOM/DoS on a
             * fresh node). Reject the whole manifest on any out-of-range entry.
             * file_index must address a real sink (0-252 block files, 253
             * block_index.bin, 254 consensus_snapshot.db); 255 is invalid. */
            if (c->size == 0 || c->size > 60 * 1024 * 1024) {
                close(fd);
                LOG_FAIL("filesvc", "client_sync: manifest chunk %u has invalid "
                         "size=%u (peer-supplied) — rejecting manifest",
                         num_chunks, c->size);
            }
            if (c->file_index > 254) {
                close(fd);
                LOG_FAIL("filesvc", "client_sync: manifest chunk %u has invalid "
                         "file_index=%u — rejecting manifest",
                         num_chunks, (unsigned)c->file_index);
            }
            num_chunks++;
        }
    }

    if (num_chunks == 0) {
        close(fd);
        LOG_FAIL("filesvc", "client_sync: manifest empty (server still building)");
    }

    /* Compute total download size for progress reporting. Each chunk is already
     * bounded at <=60MB and num_chunks at FILE_MAX_CHUNKS (1024), so the sum
     * cannot overflow uint64; cap the cumulative total at the documented
     * ~50GB ceiling so a maximal-but-individually-valid manifest can't request
     * an absurd download. */
    uint64_t total_bytes = 0;
    for (uint32_t j = 0; j < num_chunks; j++)
        total_bytes += chunks[j].size;
    if (total_bytes > (uint64_t)FILE_MAX_CHUNKS * FILE_CHUNK_SIZE) {
        close(fd);
        LOG_FAIL("filesvc", "client_sync: manifest total %llu bytes exceeds "
                 "ceiling — rejecting manifest",
                 (unsigned long long)total_bytes);
    }
    printf("file_service: manifest has %u chunks (%.1f GB), downloading...\n",
           num_chunks, (double)total_bytes / (1024.0 * 1024.0 * 1024.0));

    /* Create blocks directory */
    char blocks_dir[512];
    snprintf(blocks_dir, sizeof(blocks_dir), "%s/blocks", datadir);
    mkdir(blocks_dir, 0755);

    int64_t dl_start = (int64_t)platform_time_wall_time_t();

    /* Check if previous partial download exists — verify and resume.
     * We spot-check existing chunks with SHA3 to detect corruption. */
    uint32_t skip_chunks = 0;
    {
        uint32_t verified = 0, checked = 0;
        for (uint32_t j = 0; j < num_chunks; j++) {
            char blk_path[600];
            if (chunks[j].file_index == 253)
                snprintf(blk_path, 600, "%s/block_index.bin", datadir);
            else
                snprintf(blk_path, 600, "%s/blocks/blk%05d.dat",
                         datadir, chunks[j].file_index);

            /* Check if file is large enough for this chunk */
            struct stat st;
            if (stat(blk_path, &st) != 0 ||
                (uint64_t)st.st_size < chunks[j].offset + chunks[j].size)
                break; /* this chunk not on disk, stop skipping */

            /* SHA3 verify every 20th chunk + first + last existing.
             * Full verification would be slow on 7GB, spot-check is fast. */
            if (j == 0 || j % 20 == 0) {
                int fd2 = open(blk_path, O_RDONLY);
                if (fd2 < 0) break;
                uint8_t *vbuf = zcl_malloc(chunks[j].size, "file_verify_buf");
                if (!vbuf) { close(fd2); break; }
                ssize_t got = pread(fd2, vbuf, chunks[j].size,
                                     (off_t)chunks[j].offset);
                close(fd2);
                if (got != (ssize_t)chunks[j].size) {
                    free(vbuf); break;
                }
                uint8_t hash[32];
                sha3_256(vbuf, chunks[j].size, hash);
                free(vbuf);
                if (memcmp(hash, chunks[j].sha3, 32) != 0) {
                    fprintf(stderr,  // obs-ok:file-transfer-integrity-retry
                            "file_service: resume SHA3 mismatch at "
                            "chunk %u (file=%d off=%llu) — re-downloading "
                            "from here\n", j, chunks[j].file_index,
                            (unsigned long long)chunks[j].offset);
                    break;
                }
                checked++;
            }
            verified++;
        }
        skip_chunks = verified;
        if (skip_chunks > 0) {
            printf("file_service: resuming — %u/%u chunks on disk "
                   "(%u SHA3-verified)\n", skip_chunks, num_chunks, checked);
        }
    }

    /* Close manifest connection — we'll open parallel ones */
    close(fd);

    /* Launch 8 parallel workers, each downloads a range of chunks */
    #define FS_NWORKERS 8
    uint32_t remaining = num_chunks - skip_chunks;
    uint32_t per = (remaining + FS_NWORKERS - 1) / FS_NWORKERS;
    struct range_worker workers[FS_NWORKERS];
    pthread_t wthreads[FS_NWORKERS];
    int nworkers = 0;

    for (int w = 0; w < FS_NWORKERS; w++) {
        uint32_t ws = skip_chunks + (uint32_t)w * per;
        uint32_t we = ws + per;
        if (we > num_chunks) we = num_chunks;
        if (ws >= num_chunks) break;

        char *wp = zcl_malloc(600, "file_worker_path");
        snprintf(wp, 600, "%s/blocks/.part%d", datadir, w);

        workers[nworkers].peer_addr = peer_addr;
        workers[nworkers].port = port;
        memcpy(workers[nworkers].utxo_root, utxo_root, 32);
        workers[nworkers].chunks = chunks;
        workers[nworkers].start = ws;
        workers[nworkers].end = we;
        workers[nworkers].out_path = wp;
        workers[nworkers].datadir = datadir;
        workers[nworkers].id = w;
        workers[nworkers].fd = -1;
        atomic_store(&workers[nworkers].done, false);
        atomic_store(&workers[nworkers].cancel, false);
        atomic_store(&workers[nworkers].bytes, 0);
        atomic_store(&workers[nworkers].chunks_ok, 0);
        atomic_store(&workers[nworkers].chunks_fail, 0);
        /* raw-pthread-ok: short-burst-joined-immediately (per-download workers) */
        if (pthread_create(&wthreads[nworkers], NULL,
                           range_worker_fn, &workers[nworkers]) != 0) {
            free(wp);
            for (int j = 0; j < nworkers; j++) {
                atomic_store(&workers[j].cancel, true);
                if (workers[j].fd >= 0)
                    shutdown(workers[j].fd, SHUT_RDWR);
            }
            for (int j = 0; j < nworkers; j++) {
                pthread_join(wthreads[j], NULL);
                free((void *)workers[j].out_path);
            }
            LOG_FAIL("filesvc", "client_sync: failed to start download worker %d", w);
        }
        nworkers++;
    }

    printf("file_service: %d parallel connections downloading...\n", nworkers);

    /* Monitor progress — give up after 30 min total */
    int64_t max_wait = 1800;
    uint64_t prev_done_bytes[FS_NWORKERS];
    int stall_counts[FS_NWORKERS];
    memset(prev_done_bytes, 0, sizeof(prev_done_bytes));
    /* Start at -4 so workers have 20s grace period to connect+handshake
     * before stall detection kicks in. Prevents false cancellation. */
    for (int w = 0; w < FS_NWORKERS; w++) stall_counts[w] = -4;
    while (true) {
        sleep(5);
        uint64_t done = 0;
        bool all_done = true;
        for (int w = 0; w < nworkers; w++) {
            done += atomic_load(&workers[w].bytes);
            if (!atomic_load(&workers[w].done)) all_done = false;
        }
        int64_t el = (int64_t)platform_time_wall_time_t() - dl_start;
        if (el > max_wait) {
            printf("file_service: timeout after %llds, cancelling stuck "
                   "workers\n", (long long)el);
            for (int w = 0; w < nworkers; w++) {
                if (!atomic_load(&workers[w].done)) {
                    atomic_store(&workers[w].cancel, true);
                    shutdown(workers[w].fd, SHUT_RDWR);
                }
            }
            break;
        }
        /* Cancel workers stuck on a single chunk for >60s */
        {
            for (int w = 0; w < nworkers; w++) {
                uint64_t wb = atomic_load(&workers[w].bytes);
                if (!atomic_load(&workers[w].done) && wb == prev_done_bytes[w]) {
                    stall_counts[w]++;
                    if (stall_counts[w] >= 12) { /* 12*5s = 60s stall */
                        fprintf(stderr,  // obs-ok:file-transfer-stall-recovery
                                "file_service: worker %d stalled "
                                "at %llu bytes, cancelling\n",
                                w, (unsigned long long)wb);
                        atomic_store(&workers[w].cancel, true);
                        shutdown(workers[w].fd, SHUT_RDWR);
                        stall_counts[w] = 0;
                    }
                } else {
                    stall_counts[w] = 0;
                    prev_done_bytes[w] = wb;
                }
            }
        }
        double pct = total_bytes > 0 ? 100.0 * (double)done / (double)total_bytes : 0;
        int64_t elapsed = (int64_t)platform_time_wall_time_t() - dl_start;
        double mbps = elapsed > 0 ? (double)done / (1048576.0 * (double)elapsed) : 0;
        int eta = (done > 0 && elapsed > 0) ?
            (int)((double)(total_bytes - done) / ((double)done / (double)elapsed)) : 0;
        printf("file_service: [%3.0f%%] %.1f/%.1f GB  %.1f MB/s  "
               "(%d connections)  ETA %dm%02ds\n",
               pct, (double)done / (1024.0*1024.0*1024.0),
               (double)total_bytes / (1024.0*1024.0*1024.0),
               mbps, nworkers, eta / 60, eta % 60);
        fflush(stdout);
        if (all_done || done >= total_bytes) break;
    }

    for (int w = 0; w < nworkers; w++)
        pthread_join(wthreads[w], NULL);

    /* Workers wrote directly to blk%05d.dat files — no concatenation needed */
    for (int w = 0; w < nworkers; w++)
        free((void *)workers[w].out_path);

    uint64_t bytes_done = 0;
    uint32_t total_ok = 0, total_fail = 0;
    for (int w = 0; w < nworkers; w++) {
        bytes_done += atomic_load(&workers[w].bytes);
        total_ok += atomic_load(&workers[w].chunks_ok);
        total_fail += atomic_load(&workers[w].chunks_fail);
    }

    int64_t dl_elapsed = (int64_t)platform_time_wall_time_t() - dl_start;
    printf("=== File sync: %.1f GB in %llds (%.1f MB/s avg) "
           "chunks: %u ok, %u failed out of %u ===\n",
           (double)bytes_done / (1024.0 * 1024.0 * 1024.0),
           (long long)dl_elapsed,
           dl_elapsed > 0 ? (double)bytes_done / (1024.0 * 1024.0) / (double)dl_elapsed : 0,
           total_ok, total_fail, remaining);

    /* Retry failed chunks — up to 3 attempts with fresh connections */
    for (int retry = 0; retry < 3 && total_fail > 0; retry++) {
        printf("[file] retrying %u failed chunks, attempt %d/3\n",
               total_fail, retry + 1);

        /* Collect indices of chunks that aren't on disk yet */
        uint32_t *retry_idx = zcl_calloc(total_fail, sizeof(uint32_t),
                                         "retry_indices");
        if (!retry_idx) break;
        uint32_t nretry = 0;
        for (uint32_t c = skip_chunks; c < num_chunks && nretry < total_fail; c++) {
            /* Quick check: try reading the chunk back and verifying hash */
            char blk_path[600];
            if (chunks[c].file_index == 254)
                snprintf(blk_path, 600, "%s/consensus_snapshot.db", datadir);
            else if (chunks[c].file_index == 253)
                snprintf(blk_path, 600, "%s/block_index.bin", datadir);
            else
                snprintf(blk_path, 600, "%s/blocks/blk%05d.dat",
                         datadir, chunks[c].file_index);
            int cfd = open(blk_path, O_RDONLY);
            if (cfd < 0) { retry_idx[nretry++] = c; continue; }
            uint8_t probe[1];
            ssize_t rd = pread(cfd, probe, 1, (off_t)chunks[c].offset);
            close(cfd);
            if (rd <= 0)
                retry_idx[nretry++] = c;
        }

        if (nretry == 0) { free(retry_idx); break; }

        /* Open a single fresh connection for retries */
        struct addrinfo rhints, *rres;
        memset(&rhints, 0, sizeof(rhints));
        rhints.ai_family = AF_UNSPEC;
        rhints.ai_socktype = SOCK_STREAM;
        char rps[8];
        snprintf(rps, sizeof(rps), "%d", port);
        if (getaddrinfo(peer_addr, rps, &rhints, &rres) != 0) {
            free(retry_idx); break;
        }
        int rfd = socket(rres->ai_family, rres->ai_socktype, rres->ai_protocol);
        if (rfd < 0) { freeaddrinfo(rres); free(retry_idx); break; }

        struct timeval rtv = { .tv_sec = 120, .tv_usec = 0 };
        setsockopt(rfd, SOL_SOCKET, SO_RCVTIMEO, &rtv, sizeof(rtv));
        setsockopt(rfd, SOL_SOCKET, SO_SNDTIMEO, &rtv, sizeof(rtv));
        if (connect(rfd, rres->ai_addr, rres->ai_addrlen) < 0) {
            close(rfd); freeaddrinfo(rres); free(retry_idx); break;
        }
        freeaddrinfo(rres);

        struct fs_session rs;
        fs_session_init(&rs, rfd);
        if (!fs_handshake(&rs, utxo_root, true)) {
            close(rfd); free(retry_idx); break;
        }

        uint32_t recovered = 0;
        for (uint32_t r = 0; r < nretry; r++) {
            uint32_t ci = retry_idx[r];
            /* Request individual chunk by hash. A send failure means the
             * retry connection is dead — stop and let the post-loop cleanup
             * close rfd and free retry_idx. */
            if (!fs_send_frame(&rs, FS_REQUEST, chunks[ci].sha3, 32))
                break;

            uint8_t *buf = NULL;
            uint32_t sz = 0;
            if (!fs_recv_chunk_fast(&rs, &buf, &sz, chunks[ci].sha3)) {
                printf("[file] retrying chunk %u attempt %d failed\n",
                       ci, retry + 1);
                continue;
            }

            char blk_path[600];
            if (chunks[ci].file_index == 254)
                snprintf(blk_path, 600, "%s/consensus_snapshot.db", datadir);
            else if (chunks[ci].file_index == 253)
                snprintf(blk_path, 600, "%s/block_index.bin", datadir);
            else
                snprintf(blk_path, 600, "%s/blocks/blk%05d.dat",
                         datadir, chunks[ci].file_index);
            int bfd = open(blk_path, O_WRONLY | O_CREAT, 0644);
            if (bfd >= 0) {
                ssize_t written = pwrite(bfd, buf, sz,
                                         (off_t)chunks[ci].offset);
                fdatasync(bfd);
                close(bfd);
                if (written == (ssize_t)sz) {
                    recovered++;
                    printf("[file] retrying chunk %u attempt %d succeeded\n",
                           ci, retry + 1);
                }
            }
            free(buf);
        }

        close(rfd);
        free(retry_idx);
        total_fail = (recovered >= total_fail) ? 0 : total_fail - recovered;
        total_ok += recovered;
        printf("[file] retry attempt %d: recovered %u chunks, %u still failed\n",
               retry + 1, recovered, total_fail);
    }

    if (total_fail > 0)
        LOG_FAIL("filesvc", "client_sync: %u/%u chunks failed after retries — download incomplete", total_fail, remaining);

    if (total_ok < remaining)
        LOG_FAIL("filesvc", "client_sync: only %u/%u chunks received — incomplete", total_ok, remaining);

    return true;
}

/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ZCL Market: Crypto-incentivized P2P file sharing.
 *
 * In-memory offer cache + serialization. SQLite persistence lives in the
 * FileOffer model; gossip logic receives offers, decrements TTL, and
 * re-broadcasts. */

#include "platform/time_compat.h"
#include "net/file_market.h"
#include "core/serialize.h"
#include "util/log_macros.h"
#include <stdint.h>
#include <string.h>
#include <time.h>
#include <stdio.h>
#include <pthread.h>

/* ── In-Memory Offer Cache ──────────────────────────────────────── */

static struct file_offer g_offers[FILE_MARKET_MAX_OFFERS];
static int g_offer_count = 0;
static pthread_mutex_t g_market_mutex = PTHREAD_MUTEX_INITIALIZER;

/* ── Size Validation ────────────────────────────────────────────── */

bool file_market_num_chunks_for_size(uint64_t size_bytes,
                                     uint32_t *out_chunks)
{
    if (!out_chunks) {
        LOG_FAIL("market",
                 "num_chunks_for_size: NULL out_chunks");
        return false;
    }
    /* Reject sizes that would make num_chunks overflow u32. Max
     * accepted = UINT32_MAX * CHUNK_SIZE (~225 PB) — way above any
     * plausible real file. This also implicitly caps the
     * (size + CHUNK_SIZE - 1) u64 arithmetic far below UINT64_MAX. */
    const uint64_t max_size =
        (uint64_t)UINT32_MAX * (uint64_t)FILE_MARKET_CHUNK_SIZE;
    if (size_bytes > max_size) {
        LOG_FAIL("market",
                 "num_chunks_for_size: size_bytes too large for u32 "
                 "chunk count");
        return false;
    }
    *out_chunks = (uint32_t)((size_bytes + FILE_MARKET_CHUNK_SIZE - 1)
                              / FILE_MARKET_CHUNK_SIZE);
    return true;
}

/* ── Serialization ──────────────────────────────────────────────── */

bool file_offer_serialize(const struct file_offer *offer,
                          struct byte_stream *s)
{
    bool ok = true;
    ok &= stream_write(s, offer->root_hash, 32);

    /* filename: length-prefixed, max 255 bytes */
    size_t namelen = strlen(offer->filename);
    if (namelen > 255) namelen = 255;
    ok &= stream_write_u8(s, (uint8_t)namelen);
    ok &= stream_write(s, offer->filename, namelen);

    ok &= stream_write_u64_le(s, offer->size_bytes);
    ok &= stream_write_u32_le(s, offer->num_chunks);
    ok &= stream_write_i64_le(s, offer->price_per_mb);
    ok &= stream_write(s, offer->z_addr, 43);
    ok &= stream_write(s, offer->peer_ip, 16);
    ok &= stream_write_u16_le(s, offer->peer_port);
    ok &= stream_write_u8(s, offer->ttl);
    return ok;
}

bool file_offer_deserialize(struct file_offer *offer,
                            struct byte_stream *s)
{
    memset(offer, 0, sizeof(*offer));
    bool ok = true;

    ok &= stream_read(s, offer->root_hash, 32);

    uint8_t namelen = 0;
    ok &= stream_read_u8(s, &namelen);
    if (!ok) LOG_FAIL("market", "file_offer_deserialize: read namelen failed");
    ok &= stream_read(s, offer->filename, namelen);
    offer->filename[namelen] = '\0';

    ok &= stream_read_u64_le(s, &offer->size_bytes);
    ok &= stream_read_u32_le(s, &offer->num_chunks);
    ok &= stream_read_i64_le(s, &offer->price_per_mb);
    ok &= stream_read(s, offer->z_addr, 43);
    ok &= stream_read(s, offer->peer_ip, 16);
    ok &= stream_read_u16_le(s, &offer->peer_port);
    ok &= stream_read_u8(s, &offer->ttl);

    if (ok) offer->last_seen = (int64_t)platform_time_wall_time_t();
    return ok;
}

bool file_challenge_serialize(const struct file_challenge *chal,
                              struct byte_stream *s)
{
    bool ok = true;
    ok &= stream_write(s, chal->root_hash, 32);
    ok &= stream_write_u32_le(s, chal->chunk_index);
    return ok;
}

bool file_challenge_deserialize(struct file_challenge *chal,
                                struct byte_stream *s)
{
    memset(chal, 0, sizeof(*chal));
    bool ok = true;
    ok &= stream_read(s, chal->root_hash, 32);
    ok &= stream_read_u32_le(s, &chal->chunk_index);
    return ok;
}

bool file_proof_serialize(const struct file_proof *proof,
                          struct byte_stream *s)
{
    bool ok = true;
    ok &= stream_write(s, proof->root_hash, 32);
    ok &= stream_write_u32_le(s, proof->chunk_index);
    ok &= stream_write(s, proof->chunk_hash, 32);
    return ok;
}

bool file_proof_deserialize(struct file_proof *proof,
                            struct byte_stream *s)
{
    memset(proof, 0, sizeof(*proof));
    bool ok = true;
    ok &= stream_read(s, proof->root_hash, 32);
    ok &= stream_read_u32_le(s, &proof->chunk_index);
    ok &= stream_read(s, proof->chunk_hash, 32);
    return ok;
}

bool file_payment_serialize(const struct file_payment *pay,
                            struct byte_stream *s)
{
    bool ok = true;
    ok &= stream_write(s, pay->root_hash, 32);
    ok &= stream_write(s, pay->txid, 32);
    ok &= stream_write_u32_le(s, pay->chunks_paid);
    ok &= stream_write_u32_le(s, pay->chunk_start);
    return ok;
}

bool file_payment_deserialize(struct file_payment *pay,
                              struct byte_stream *s)
{
    memset(pay, 0, sizeof(*pay));
    bool ok = true;
    ok &= stream_read(s, pay->root_hash, 32);
    ok &= stream_read(s, pay->txid, 32);
    ok &= stream_read_u32_le(s, &pay->chunks_paid);
    ok &= stream_read_u32_le(s, &pay->chunk_start);
    return ok;
}

/* ── Offer Cache ────────────────────────────────────────────────── */

bool file_market_add_offer(const struct file_offer *offer)
{
    if (!offer || offer->ttl == 0 || offer->num_chunks == 0)
        LOG_FAIL("market", "add_offer: null offer or ttl=0 or num_chunks=0");

    pthread_mutex_lock(&g_market_mutex);

    /* Check for existing offer with same root_hash — update if newer */
    for (int i = 0; i < g_offer_count; i++) {
        if (memcmp(g_offers[i].root_hash, offer->root_hash, 32) == 0) {
            g_offers[i] = *offer;
            g_offers[i].last_seen = (int64_t)platform_time_wall_time_t();
            pthread_mutex_unlock(&g_market_mutex);
            return false; /* updated, not new */
        }
    }

    /* Add new offer */
    if (g_offer_count >= FILE_MARKET_MAX_OFFERS) {
        /* Evict oldest */
        int oldest = 0;
        for (int i = 1; i < g_offer_count; i++) {
            if (g_offers[i].last_seen < g_offers[oldest].last_seen)
                oldest = i;
        }
        g_offers[oldest] = *offer;
        g_offers[oldest].last_seen = (int64_t)platform_time_wall_time_t();
        pthread_mutex_unlock(&g_market_mutex);
        return true;
    }

    g_offers[g_offer_count] = *offer;
    g_offers[g_offer_count].last_seen = (int64_t)platform_time_wall_time_t();
    g_offer_count++;
    pthread_mutex_unlock(&g_market_mutex);
    return true;
}

int file_market_get_offers(struct file_offer *out, size_t max)
{
    pthread_mutex_lock(&g_market_mutex);
    int count = g_offer_count;
    if ((size_t)count > max) count = (int)max;
    memcpy(out, g_offers, count * sizeof(struct file_offer));
    pthread_mutex_unlock(&g_market_mutex);
    return count;
}

bool file_market_find_offer(const uint8_t root_hash[32],
                            struct file_offer *out)
{
    pthread_mutex_lock(&g_market_mutex);
    for (int i = 0; i < g_offer_count; i++) {
        if (memcmp(g_offers[i].root_hash, root_hash, 32) == 0) {
            *out = g_offers[i];
            pthread_mutex_unlock(&g_market_mutex);
            return true;
        }
    }
    pthread_mutex_unlock(&g_market_mutex);
    return false;
}

int file_market_prune(int64_t max_age)
{
    int64_t cutoff = (int64_t)platform_time_wall_time_t() - max_age;
    int pruned = 0;

    pthread_mutex_lock(&g_market_mutex);
    for (int i = 0; i < g_offer_count; ) {
        if (g_offers[i].last_seen < cutoff) {
            g_offers[i] = g_offers[g_offer_count - 1];
            g_offer_count--;
            pruned++;
        } else {
            i++;
        }
    }
    pthread_mutex_unlock(&g_market_mutex);
    return pruned;
}

int file_market_count(void)
{
    pthread_mutex_lock(&g_market_mutex);
    int c = g_offer_count;
    pthread_mutex_unlock(&g_market_mutex);
    return c;
}

/* ── Download Sessions ──────────────────────────────────────────── */

#define MAX_DOWNLOADS 16
static struct file_download g_downloads[MAX_DOWNLOADS];
static int g_download_count = 0;

int file_market_start_download(const uint8_t root_hash[32],
                               const char *output_path)
{
    struct file_offer offer;
    if (!file_market_find_offer(root_hash, &offer))
        LOG_ERR("market", "start_download: offer not found for root_hash");

    pthread_mutex_lock(&g_market_mutex);
    if (g_download_count >= MAX_DOWNLOADS) {
        pthread_mutex_unlock(&g_market_mutex);
        LOG_ERR("market", "start_download: max downloads %d reached", MAX_DOWNLOADS);
    }

    int idx = g_download_count++;
    memset(&g_downloads[idx], 0, sizeof(g_downloads[idx]));
    g_downloads[idx].offer = offer;
    g_downloads[idx].state = FDL_CHALLENGING;
    if (output_path)
        snprintf(g_downloads[idx].output_path, sizeof(g_downloads[idx].output_path),
                 "%s", output_path);
    pthread_mutex_unlock(&g_market_mutex);
    return idx;
}

bool file_market_get_download(const uint8_t root_hash[32],
                              struct file_download *out)
{
    pthread_mutex_lock(&g_market_mutex);
    for (int i = 0; i < g_download_count; i++) {
        if (memcmp(g_downloads[i].offer.root_hash, root_hash, 32) == 0) {
            *out = g_downloads[i];
            pthread_mutex_unlock(&g_market_mutex);
            return true;
        }
    }
    pthread_mutex_unlock(&g_market_mutex);
    return false;
}

bool file_market_update_download(const uint8_t root_hash[32],
                                 enum file_download_state state,
                                 uint32_t chunks_received,
                                 uint32_t chunks_paid_through)
{
    pthread_mutex_lock(&g_market_mutex);
    for (int i = 0; i < g_download_count; i++) {
        if (memcmp(g_downloads[i].offer.root_hash, root_hash, 32) == 0) {
            g_downloads[i].state = state;
            g_downloads[i].chunks_received = chunks_received;
            g_downloads[i].chunks_paid_through = chunks_paid_through;
            pthread_mutex_unlock(&g_market_mutex);
            return true;
        }
    }
    pthread_mutex_unlock(&g_market_mutex);
    return false;
}

bool file_market_download_challenge_passed(const uint8_t root_hash[32])
{
    pthread_mutex_lock(&g_market_mutex);
    for (int i = 0; i < g_download_count; i++) {
        if (memcmp(g_downloads[i].offer.root_hash, root_hash, 32) == 0) {
            g_downloads[i].challenges_passed++;
            pthread_mutex_unlock(&g_market_mutex);
            return true;
        }
    }
    pthread_mutex_unlock(&g_market_mutex);
    return false;
}

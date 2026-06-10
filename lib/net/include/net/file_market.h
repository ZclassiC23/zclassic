/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ZCL Market: Crypto-incentivized P2P file sharing.
 *
 * BitTorrent-style file distribution with shielded ZCL payments
 * instead of ratio tracking. Seeders announce files with price/MB,
 * downloaders pay per batch of chunks via Sapling shielded tx.
 *
 * Privacy: shielded payments + SHA3-CTR transport + Tor onion.
 * Sybil resistance: random chunk challenges before payment. */

#ifndef ZCL_NET_FILE_MARKET_H
#define ZCL_NET_FILE_MARKET_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* P2P message commands (max 12 chars) */
#define MSG_FILE_LIST   "zfilelist"    /* gossip: file announcements */
#define MSG_FILE_CHAL   "zfilechal"   /* challenge: prove you have data */
#define MSG_FILE_PROOF  "zfileproof"  /* response: SHA3 of challenged chunk */
#define MSG_FILE_PAY    "zfilepay"    /* payment notification */

/* Gossip limits */
#define FILE_MARKET_MAX_TTL        4
#define FILE_MARKET_MAX_OFFERS     256   /* per-node offer cache */
#define FILE_MARKET_CHALLENGES     3     /* random chunks to challenge */
#define FILE_MARKET_BATCH_SIZE     10    /* chunks per payment batch */

/* Chunk size matches file_service.h: 50MB */
#define FILE_MARKET_CHUNK_SIZE     (50 * 1024 * 1024)

/* ── File Offer ─────────────────────────────────────────────────── */

struct file_offer {
    uint8_t  root_hash[32];       /* SHA3-256 of file manifest */
    char     filename[256];       /* human-readable name */
    uint64_t size_bytes;          /* total file size */
    uint32_t num_chunks;          /* ceil(size / CHUNK_SIZE) */
    int64_t  price_per_mb;        /* price in zatoshis per MB */
    uint8_t  z_addr[43];         /* seeder's Sapling payment address (raw) */
    uint8_t  peer_ip[16];        /* seeder's IP */
    uint16_t peer_port;          /* seeder's file service port */
    int64_t  last_seen;          /* unix timestamp */
    uint8_t  ttl;                /* remaining gossip hops */
};

/* ── Chunk Challenge ────────────────────────────────────────────── */

struct file_challenge {
    uint8_t  root_hash[32];       /* which file */
    uint32_t chunk_index;         /* which chunk to prove */
};

struct file_proof {
    uint8_t  root_hash[32];       /* which file */
    uint32_t chunk_index;         /* which chunk */
    uint8_t  chunk_hash[32];      /* SHA3-256 of that chunk's data */
};

/* ── Payment Notification ───────────────────────────────────────── */

struct file_payment {
    uint8_t  root_hash[32];       /* which file */
    uint8_t  txid[32];           /* shielded payment txid */
    uint32_t chunks_paid;        /* number of chunks this covers */
    uint32_t chunk_start;        /* first chunk index covered */
};

/* ── Download Session ───────────────────────────────────────────── */

enum file_download_state {
    FDL_IDLE        = 0,
    FDL_CHALLENGING = 1,   /* sending chunk challenges */
    FDL_PAYING      = 2,   /* creating/sending payment */
    FDL_DOWNLOADING = 3,   /* receiving chunks */
    FDL_COMPLETE    = 4,
    FDL_FAILED      = 5
};

struct file_download {
    struct file_offer offer;
    enum file_download_state state;
    uint32_t chunks_received;
    uint32_t chunks_paid_through;   /* last chunk index paid for */
    uint32_t challenges_sent;
    uint32_t challenges_passed;
    int peer_id;                    /* P2P peer serving this file */
    char output_path[512];          /* where to save */
};

/* ── Size Validation ─────────────────────────────────────────────── */

/* Compute num_chunks = ceil(size_bytes / CHUNK_SIZE) with overflow
 * guards. Returns false (and leaves *out_chunks untouched) if
 * size_bytes would make num_chunks overflow uint32_t — i.e., if
 * size_bytes > (uint64_t)UINT32_MAX * FILE_MARKET_CHUNK_SIZE. Also
 * rejects NULL out_chunks.
 *
 * Fixes zmarket_offer previously silently truncated 225 PB+
 * files to a wrong u32 chunk count via (uint32_t)(u64_expr). */
bool file_market_num_chunks_for_size(uint64_t size_bytes,
                                     uint32_t *out_chunks);

/* ── Serialization ──────────────────────────────────────────────── */

struct byte_stream;

bool file_offer_serialize(const struct file_offer *offer,
                          struct byte_stream *s);
bool file_offer_deserialize(struct file_offer *offer,
                            struct byte_stream *s);

bool file_challenge_serialize(const struct file_challenge *chal,
                              struct byte_stream *s);
bool file_challenge_deserialize(struct file_challenge *chal,
                                struct byte_stream *s);

bool file_proof_serialize(const struct file_proof *proof,
                          struct byte_stream *s);
bool file_proof_deserialize(struct file_proof *proof,
                            struct byte_stream *s);

bool file_payment_serialize(const struct file_payment *pay,
                            struct byte_stream *s);
bool file_payment_deserialize(struct file_payment *pay,
                              struct byte_stream *s);

/* ── Offer Management ───────────────────────────────────────────── */

/* Add or update an offer in the local cache. Returns true if new. */
bool file_market_add_offer(const struct file_offer *offer);

/* Get all known offers. Returns count written to out (up to max). */
int file_market_get_offers(struct file_offer *out, size_t max);

/* Find offers by root hash. Returns true if found. */
bool file_market_find_offer(const uint8_t root_hash[32],
                            struct file_offer *out);

/* Remove expired offers (older than max_age seconds). */
int file_market_prune(int64_t max_age);

/* Get offer count. */
int file_market_count(void);

/* ── Download Management ────────────────────────────────────────── */

/* Start a download. Returns session index or -1 on error. */
int file_market_start_download(const uint8_t root_hash[32],
                               const char *output_path);

/* Get active download status. Returns false if not found. */
bool file_market_get_download(const uint8_t root_hash[32],
                              struct file_download *out);

/* Update download state. Returns false if not found. */
bool file_market_update_download(const uint8_t root_hash[32],
                                 enum file_download_state state,
                                 uint32_t chunks_received,
                                 uint32_t chunks_paid_through);

/* Increment challenges_passed for a download. */
bool file_market_download_challenge_passed(const uint8_t root_hash[32]);

/* Release chunks assigned to a disconnected peer. */
bool file_market_release_peer_chunks(int peer_id);

/* ── SQLite Persistence ─────────────────────────────────────────── */

struct node_db;

bool db_file_offer_save(struct node_db *ndb,
                        const struct file_offer *offer);
int db_file_offer_list(struct node_db *ndb,
                       struct file_offer *out, size_t max);
bool db_file_offer_find(struct node_db *ndb,
                        const uint8_t root_hash[32],
                        struct file_offer *out);
int db_file_offer_prune(struct node_db *ndb, int64_t max_age);

#endif

/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ZID — sovereign identity document codec: blinded keys, encode/decode,
 * sign/verify, and the monotonic-seq supersede rule (no allocation). */

#include "zid/zid.h"
#include "crypto/ed25519.h"
#include "crypto/sha3.h"
#include "base/log_macros.h"
#include <string.h>

/* Fixed wire prefix length: version:1 ‖ pubkey:32 ‖ seq:8 ‖ expiry:8 ‖
 * body_len:2. The signature (64 bytes) follows the body. */
#define ZID_PREFIX_LEN 51

static void put_le64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t get_le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

void zid_blinded_key(uint8_t out[32], const uint8_t master_pubkey[32],
                     uint64_t period)
{
    /* "zid-blind" ‖ master_pubkey ‖ period_le64 — 9 + 32 + 8 bytes. */
    uint8_t buf[9 + 32 + 8];
    memcpy(buf, "zid-blind", 9);
    memcpy(buf + 9, master_pubkey, 32);
    put_le64(buf + 9 + 32, period);
    sha3_256(buf, sizeof(buf), out);
}

/* Encode the signature-covered prefix (everything before the signature)
 * into out[0..ZID_PREFIX_LEN + doc->body_len). Caller guarantees
 * body_len <= ZID_BODY_MAX and a large-enough buffer. */
static size_t zid_encode_prefix(uint8_t *out, const struct zid_doc *doc)
{
    size_t n = 0;
    out[n++] = ZID_DOC_VERSION;
    memcpy(out + n, doc->master_pubkey, 32);
    n += 32;
    put_le64(out + n, doc->seq);
    n += 8;
    put_le64(out + n, doc->expiry);
    n += 8;
    out[n++] = (uint8_t)(doc->body_len & 0xff);
    out[n++] = (uint8_t)(doc->body_len >> 8);
    memcpy(out + n, doc->body, doc->body_len);
    n += doc->body_len;
    return n;
}

size_t zid_doc_encode(uint8_t *out, size_t out_len, const struct zid_doc *doc)
{
    if (!out || !doc)
        LOG_RETURN(0, "zid", "encode: NULL argument (out=%p doc=%p)",
                   (void *)out, (const void *)doc);
    if (doc->body_len > ZID_BODY_MAX)
        LOG_RETURN(0, "zid", "encode: body_len %u exceeds ZID_BODY_MAX %d",
                   doc->body_len, ZID_BODY_MAX);
    size_t total = ZID_PREFIX_LEN + (size_t)doc->body_len + 64;
    if (out_len < total)
        LOG_RETURN(0, "zid",
                   "encode: out_len %zu too small (need %zu for body_len %u)",
                   out_len, total, doc->body_len);
    size_t n = zid_encode_prefix(out, doc);
    memcpy(out + n, doc->signature, 64);
    return n + 64;
}

bool zid_doc_decode(struct zid_doc *doc, const uint8_t *buf, size_t len)
{
    if (!doc || !buf)
        LOG_FAIL("zid", "decode: NULL argument (doc=%p buf=%p)",
                 (void *)doc, (const void *)buf);
    if (len < ZID_PREFIX_LEN + 64)
        LOG_FAIL("zid", "decode: len %zu below minimum %d",
                 len, ZID_PREFIX_LEN + 64);
    if (len > ZID_DOC_MAX)
        LOG_FAIL("zid", "decode: len %zu exceeds ZID_DOC_MAX %d",
                 len, ZID_DOC_MAX);
    if (buf[0] != ZID_DOC_VERSION)
        LOG_FAIL("zid", "decode: unsupported version %u (want %d)",
                 buf[0], ZID_DOC_VERSION);

    uint16_t body_len = (uint16_t)buf[49] | ((uint16_t)buf[50] << 8);
    if (len != ZID_PREFIX_LEN + (size_t)body_len + 64)
        LOG_FAIL("zid",
                 "decode: len %zu does not match body_len %u (want exactly %zu)",
                 len, body_len, ZID_PREFIX_LEN + (size_t)body_len + 64);

    memcpy(doc->master_pubkey, buf + 1, 32);
    doc->seq = get_le64(buf + 33);
    doc->expiry = get_le64(buf + 41);
    doc->body_len = body_len;
    memcpy(doc->body, buf + ZID_PREFIX_LEN, body_len);
    memcpy(doc->signature, buf + ZID_PREFIX_LEN + body_len, 64);
    return true;
}

bool zid_doc_verify(const struct zid_doc *doc, uint64_t now_unix)
{
    if (!doc)
        LOG_FAIL("zid", "verify: NULL doc");
    if (doc->body_len > ZID_BODY_MAX)
        LOG_FAIL("zid", "verify: body_len %u exceeds ZID_BODY_MAX %d",
                 doc->body_len, ZID_BODY_MAX);
    if (now_unix >= doc->expiry)
        LOG_FAIL("zid", "verify: doc expired (expiry=%llu now=%llu)",
                 (unsigned long long)doc->expiry,
                 (unsigned long long)now_unix);

    uint8_t prefix[ZID_PREFIX_LEN + ZID_BODY_MAX];
    size_t prefix_len = zid_encode_prefix(prefix, doc);
    if (!ed25519_verify(doc->signature, prefix, prefix_len,
                        doc->master_pubkey))
        LOG_FAIL("zid", "verify: ed25519 signature check failed");
    return true;
}

bool zid_doc_sign(struct zid_doc *doc, const uint8_t *body, uint16_t body_len,
                  uint64_t seq, uint64_t expiry, const uint8_t seed[32])
{
    if (!doc || !seed)
        LOG_FAIL("zid", "sign: NULL argument (doc=%p seed=%p)",
                 (void *)doc, (const void *)seed);
    if (body_len > ZID_BODY_MAX)
        LOG_FAIL("zid", "sign: body_len %u exceeds ZID_BODY_MAX %d",
                 body_len, ZID_BODY_MAX);
    if (body_len > 0 && !body)
        LOG_FAIL("zid", "sign: NULL body with body_len %u", body_len);

    memset(doc, 0, sizeof(*doc));
    uint8_t sk[32];
    ed25519_keypair(doc->master_pubkey, sk, seed);
    doc->seq = seq;
    doc->expiry = expiry;
    doc->body_len = body_len;
    if (body_len > 0)
        memcpy(doc->body, body, body_len);

    uint8_t prefix[ZID_PREFIX_LEN + ZID_BODY_MAX];
    size_t prefix_len = zid_encode_prefix(prefix, doc);
    ed25519_sign(doc->signature, prefix, prefix_len, sk, doc->master_pubkey);
    return true;
}

bool zid_doc_supersedes(const struct zid_doc *candidate,
                        const struct zid_doc *current)
{
    if (!candidate || !current)
        LOG_FAIL("zid", "supersedes: NULL argument (candidate=%p current=%p)",
                 (const void *)candidate, (const void *)current);
    if (memcmp(candidate->master_pubkey, current->master_pubkey, 32) != 0)
        return false; /* raw-return-ok: different identity, a predicate answer not an error */
    return candidate->seq > current->seq;
}

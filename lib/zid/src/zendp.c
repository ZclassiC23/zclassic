/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ZENDP — signed endpoint record body codec. See zid/zendp.h for the
 * frozen wire, the canonical-encoding rule, and the size argument (a
 * 228-byte doc, past the 223-byte OP_RETURN relay cap, so records are
 * content and never chain data). Pure: no allocation, no store, no
 * network, no chain. */

#include "zid/zendp.h"

#include "zid/zid.h"
#include "base/log_macros.h"
#include "crypto/sha3.h"

#include <string.h>

#define ZENDP_LOG "zid.endpoint"

static void zendp_put_le16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)(v & 0xffu);
    p[1] = (uint8_t)((v >> 8) & 0xffu);
}

static uint16_t zendp_get_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void zendp_put_le32(uint8_t *p, uint32_t v)
{
    for (int i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint32_t zendp_get_le32(const uint8_t *p)
{
    uint32_t v = 0;
    for (int i = 3; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

static void zendp_put_le64(uint8_t *p, uint64_t v)
{
    for (int i = 0; i < 8; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t zendp_get_le64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--)
        v = (v << 8) | p[i];
    return v;
}

static bool zendp_all_zero(const uint8_t *p, size_t n)
{
    uint8_t acc = 0;
    for (size_t i = 0; i < n; i++)
        acc |= p[i];
    return acc == 0;
}

size_t zendp_body_len(uint8_t flags)
{
    if ((flags & ~(uint8_t)ZENDP_FLAGS_KNOWN) != 0)
        return 0;
    if (flags == 0)
        return 0;
    size_t n = (size_t)ZENDP_BODY_MIN;
    if (flags & ZENDP_HAS_ONION)
        n += (size_t)ZENDP_ONION_WIRE;
    if (flags & ZENDP_HAS_IPV4)
        n += (size_t)ZENDP_IPV4_WIRE;
    if (flags & ZENDP_HAS_IPV6)
        n += (size_t)ZENDP_IPV6_WIRE;
    return n;
}

bool zendp_valid(const struct zendp *ep)
{
    if (!ep)
        LOG_FAIL(ZENDP_LOG, "valid: NULL record");
    if ((ep->flags & ~(uint8_t)ZENDP_FLAGS_KNOWN) != 0)
        LOG_FAIL(ZENDP_LOG, "valid: unknown flag bits 0x%02x (known 0x%02x)",
                 (unsigned)(ep->flags & ~(uint8_t)ZENDP_FLAGS_KNOWN),
                 (unsigned)ZENDP_FLAGS_KNOWN);
    if (ep->flags == 0)
        LOG_FAIL(ZENDP_LOG,
                 "valid: no endpoint claimed (flags == 0) — the record names "
                 "no way to reach anything");

    if (ep->flags & ZENDP_HAS_ONION) {
        if (!zdesc_onion_valid(ep->onion))
            LOG_FAIL(ZENDP_LOG,
                     "valid: onion hostname is not a v3 address (56 base32 "
                     "a-z2-7 chars + \".onion\")");
        if (ep->onion_port == 0)
            LOG_FAIL(ZENDP_LOG, "valid: onion port 0 reaches no service");
    } else {
        if (ep->onion[0] != '\0' || ep->onion_port != 0)
            LOG_FAIL(ZENDP_LOG,
                     "valid: onion fields set without ZENDP_HAS_ONION — a "
                     "field a flag does not claim must be zero");
    }

    if (ep->flags & ZENDP_HAS_IPV4) {
        if (zendp_all_zero(ep->ipv4, sizeof(ep->ipv4)))
            LOG_FAIL(ZENDP_LOG, "valid: 0.0.0.0 reaches nobody");
        if (ep->ipv4_port == 0)
            LOG_FAIL(ZENDP_LOG, "valid: ipv4 port 0 reaches no service");
    } else {
        if (!zendp_all_zero(ep->ipv4, sizeof(ep->ipv4)) || ep->ipv4_port != 0)
            LOG_FAIL(ZENDP_LOG,
                     "valid: ipv4 fields set without ZENDP_HAS_IPV4 — a field "
                     "a flag does not claim must be zero");
    }

    if (ep->flags & ZENDP_HAS_IPV6) {
        if (zendp_all_zero(ep->ipv6, sizeof(ep->ipv6)))
            LOG_FAIL(ZENDP_LOG, "valid: :: reaches nobody");
        if (ep->ipv6_port == 0)
            LOG_FAIL(ZENDP_LOG, "valid: ipv6 port 0 reaches no service");
    } else {
        if (!zendp_all_zero(ep->ipv6, sizeof(ep->ipv6)) || ep->ipv6_port != 0)
            LOG_FAIL(ZENDP_LOG,
                     "valid: ipv6 fields set without ZENDP_HAS_IPV6 — a field "
                     "a flag does not claim must be zero");
    }
    return true;
}

size_t zendp_encode_body(uint8_t *out, size_t out_len, const struct zendp *ep)
{
    if (!out || !ep)
        LOG_RETURN(0, ZENDP_LOG, "encode: NULL argument (out=%p ep=%p)",
                   (void *)out, (const void *)ep);
    if (!zendp_valid(ep))
        LOG_RETURN(0, ZENDP_LOG, "encode: record failed the shape rule");

    size_t total = zendp_body_len(ep->flags);
    if (total == 0)
        LOG_RETURN(0, ZENDP_LOG, "encode: flags 0x%02x have no body length",
                   (unsigned)ep->flags);
    if (out_len < total)
        LOG_RETURN(0, ZENDP_LOG, "encode: out_len %zu too small (need %zu)",
                   out_len, total);

    size_t n = 0;
    memcpy(out + n, "ZIDE", 4);
    n += 4;
    out[n++] = ep->flags;
    zendp_put_le64(out + n, ep->services);
    n += 8;
    zendp_put_le32(out + n, ep->height);
    n += 4;
    zendp_put_le64(out + n, ep->not_before);
    n += 8;
    if (ep->flags & ZENDP_HAS_ONION) {
        memcpy(out + n, ep->onion, ZENDP_ONION_LEN);
        n += ZENDP_ONION_LEN;
        zendp_put_le16(out + n, ep->onion_port);
        n += 2;
    }
    if (ep->flags & ZENDP_HAS_IPV4) {
        memcpy(out + n, ep->ipv4, sizeof(ep->ipv4));
        n += sizeof(ep->ipv4);
        zendp_put_le16(out + n, ep->ipv4_port);
        n += 2;
    }
    if (ep->flags & ZENDP_HAS_IPV6) {
        memcpy(out + n, ep->ipv6, sizeof(ep->ipv6));
        n += sizeof(ep->ipv6);
        zendp_put_le16(out + n, ep->ipv6_port);
        n += 2;
    }
    return n;
}

bool zendp_decode_body(struct zendp *ep, const uint8_t *body,
                       uint16_t body_len)
{
    if (!ep || !body)
        LOG_FAIL(ZENDP_LOG, "decode: NULL argument (ep=%p body=%p)",
                 (void *)ep, (const void *)body);
    if (body_len < ZENDP_BODY_MIN)
        LOG_FAIL(ZENDP_LOG, "decode: body_len %u below minimum %d", body_len,
                 ZENDP_BODY_MIN);
    if (body_len > ZENDP_BODY_MAX)
        LOG_FAIL(ZENDP_LOG, "decode: body_len %u exceeds max %d", body_len,
                 ZENDP_BODY_MAX);
    if (memcmp(body, "ZIDE", 4) != 0)
        LOG_FAIL(ZENDP_LOG, "decode: bad tag (want ZIDE)");

    uint8_t flags = body[4];
    size_t want = zendp_body_len(flags);
    if (want == 0)
        LOG_FAIL(ZENDP_LOG,
                 "decode: flags 0x%02x are unknown or claim no endpoint",
                 (unsigned)flags);
    if ((size_t)body_len != want)
        LOG_FAIL(ZENDP_LOG,
                 "decode: body_len %u does not match flags 0x%02x (want "
                 "exactly %zu — no trailing bytes)",
                 body_len, (unsigned)flags, want);

    memset(ep, 0, sizeof(*ep));
    ep->flags = flags;
    ep->services = zendp_get_le64(body + 5);
    ep->height = zendp_get_le32(body + 13);
    ep->not_before = zendp_get_le64(body + 17);

    size_t at = (size_t)ZENDP_BODY_MIN;
    if (flags & ZENDP_HAS_ONION) {
        memcpy(ep->onion, body + at, ZENDP_ONION_LEN);
        at += ZENDP_ONION_LEN;
        ep->onion_port = zendp_get_le16(body + at);
        at += 2;
    }
    if (flags & ZENDP_HAS_IPV4) {
        memcpy(ep->ipv4, body + at, sizeof(ep->ipv4));
        at += sizeof(ep->ipv4);
        ep->ipv4_port = zendp_get_le16(body + at);
        at += 2;
    }
    if (flags & ZENDP_HAS_IPV6) {
        memcpy(ep->ipv6, body + at, sizeof(ep->ipv6));
        at += sizeof(ep->ipv6);
        ep->ipv6_port = zendp_get_le16(body + at);
        at += 2;
    }

    /* The trailing NUL on the hostname comes from the memset; now prove
     * the bytes are a real v3 address and the rest of the shape holds.
     * A control byte or a wrong alphabet is a REJECT, not a silent
     * sanitize (same discipline as zdesc_decode_body). */
    if (!zendp_valid(ep)) {
        memset(ep, 0, sizeof(*ep));
        LOG_FAIL(ZENDP_LOG, "decode: decoded record failed the shape rule");
    }
    return true;
}

bool zendp_sign(struct zid_doc *doc, const struct zendp *ep, uint64_t seq,
                uint64_t expiry, const uint8_t seed[32])
{
    if (!doc || !ep || !seed)
        LOG_FAIL(ZENDP_LOG, "sign: NULL argument (doc=%p ep=%p seed=%p)",
                 (void *)doc, (const void *)ep, (const void *)seed);
    if (expiry <= ep->not_before)
        LOG_FAIL(ZENDP_LOG,
                 "sign: expiry %llu is not after not_before %llu — the "
                 "validity window never opens",
                 (unsigned long long)expiry,
                 (unsigned long long)ep->not_before);
    uint8_t body[ZENDP_BODY_MAX];
    size_t body_len = zendp_encode_body(body, sizeof(body), ep);
    if (body_len == 0)
        LOG_FAIL(ZENDP_LOG, "sign: body encode failed");
    if (!zid_doc_sign(doc, body, (uint16_t)body_len, seq, expiry, seed))
        LOG_FAIL(ZENDP_LOG, "sign: doc sign failed");
    return true;
}

bool zendp_verify(const struct zid_doc *doc, struct zendp *ep_out,
                  uint64_t now_unix)
{
    if (!doc)
        LOG_FAIL(ZENDP_LOG, "verify: NULL doc");
    if (!zid_doc_verify(doc, now_unix))
        LOG_FAIL(ZENDP_LOG,
                 "verify: doc verify failed (signature or expiry)");
    struct zendp ep;
    if (!zendp_decode_body(&ep, doc->body, doc->body_len))
        LOG_FAIL(ZENDP_LOG, "verify: body is not a valid ZIDE record");
    if (now_unix < ep.not_before)
        LOG_FAIL(ZENDP_LOG,
                 "verify: record not yet valid (now %llu < not_before %llu)",
                 (unsigned long long)now_unix,
                 (unsigned long long)ep.not_before);
    if (ep_out)
        *ep_out = ep;
    return true;
}

void zendp_record_key(uint8_t out[32], const uint8_t master_pubkey[32],
                      uint64_t period)
{
    if (!out || !master_pubkey) {
        LOG_ERROR(ZENDP_LOG, "record_key: NULL argument (out=%p pk=%p)",
                  (void *)out, (const void *)master_pubkey);
        return;
    }
    uint8_t blinded[32];
    zid_blinded_key(blinded, master_pubkey, period);

    uint8_t buf[4 + 32];
    memcpy(buf, "ZIDE", 4);
    memcpy(buf + 4, blinded, 32);
    sha3_256(buf, sizeof(buf), out);
}

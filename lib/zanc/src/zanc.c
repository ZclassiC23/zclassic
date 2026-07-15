/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ZCL Anchors (ZANC) — OP_RETURN parser and builder. Same PUSH encoding as
 * ZNAM/ZSLP (script/op_return_push.h). */

#include "zanc/zanc.h"
#include "script/op_return_push.h"
#include <string.h>

bool zanc_hash_type_valid(uint8_t t)
{
    return t == ZANC_HASH_SHA2_256 || t == ZANC_HASH_SHA3_256;
}

const char *zanc_hash_type_name(uint8_t t)
{
    switch (t) {
    case ZANC_HASH_SHA2_256: return "sha2-256";
    case ZANC_HASH_SHA3_256: return "sha3-256";
    default: return "unknown";
    }
}

/* Minimal RFC 3629 UTF-8 validator: rejects overlong forms, surrogates,
 * code points above U+10FFFF, embedded NUL, and C0 control bytes (<0x20). A
 * label is operator-facing metadata, so control bytes are disallowed too. */
bool zanc_label_valid(const char *label, size_t len)
{
    if (len > ZANC_LABEL_MAX) return false;
    if (len == 0) return true;
    if (!label) return false;

    const uint8_t *p = (const uint8_t *)label;
    size_t i = 0;
    while (i < len) {
        uint8_t c = p[i];
        if (c == 0x00) return false;
        if (c < 0x20) return false;              /* C0 control */
        if (c < 0x80) { i++; continue; }         /* ASCII */

        uint32_t cp;
        size_t extra;
        if ((c & 0xe0) == 0xc0) { cp = c & 0x1f; extra = 1; }
        else if ((c & 0xf0) == 0xe0) { cp = c & 0x0f; extra = 2; }
        else if ((c & 0xf8) == 0xf0) { cp = c & 0x07; extra = 3; }
        else return false;                       /* invalid lead byte */

        if (i + extra >= len) return false;
        for (size_t k = 1; k <= extra; k++) {
            uint8_t cc = p[i + k];
            if ((cc & 0xc0) != 0x80) return false;
            cp = (cp << 6) | (cc & 0x3f);
        }
        /* Reject overlong encodings and out-of-range/surrogate code points. */
        if (extra == 1 && cp < 0x80) return false;
        if (extra == 2 && cp < 0x800) return false;
        if (extra == 3 && cp < 0x10000) return false;
        if (cp > 0x10ffff) return false;
        if (cp >= 0xd800 && cp <= 0xdfff) return false;
        i += extra + 1;
    }
    return true;
}

bool zanc_parse(const uint8_t *script, size_t script_len,
                struct zanc_message *msg)
{
    if (!msg) return false;
    memset(msg, 0, sizeof(*msg));

    if (!script || script_len == 0) return false;
    const uint8_t *p = script;
    const uint8_t *end = script + script_len;

    if (p >= end || *p != 0x6a) return false;    /* OP_RETURN */
    p++;

    const uint8_t *data;
    size_t len;

    /* Field 0: lokad "ZANC" */
    p = read_push(p, end, &data, &len);
    if (!p || len != 4 || memcmp(data, ZANC_LOKAD_BYTES, 4) != 0) return false;

    /* Field 1: version */
    p = read_push(p, end, &data, &len);
    if (!p || len != 1 || data[0] != ZANC_VERSION) return false;
    msg->version = data[0];

    /* Field 2: hash_type */
    p = read_push(p, end, &data, &len);
    if (!p || len != 1 || !zanc_hash_type_valid(data[0])) return false;
    msg->hash_type = data[0];

    /* Field 3: digest (exactly 32 bytes) */
    p = read_push(p, end, &data, &len);
    if (!p || len != ZANC_DIGEST_LEN) return false;
    memcpy(msg->digest, data, ZANC_DIGEST_LEN);

    /* Field 4: label (0..32 bytes, UTF-8) */
    p = read_push(p, end, &data, &len);
    if (!p || len > ZANC_LABEL_MAX) return false;
    if (!zanc_label_valid((const char *)data, len)) return false;
    msg->label_len = (uint8_t)len;
    if (len) memcpy(msg->label, data, len);
    msg->label[len] = '\0';

    /* No trailing bytes after the label push. */
    if (p != end) return false;
    return true;
}

size_t zanc_build_anchor(uint8_t *out, size_t out_len, uint8_t hash_type,
                         const uint8_t digest[ZANC_DIGEST_LEN],
                         const char *label)
{
    if (!out || !digest) return 0;
    if (!zanc_hash_type_valid(hash_type)) return 0;

    size_t label_len = label ? strlen(label) : 0;
    if (!zanc_label_valid(label, label_len)) return 0;

    if (out_len < 1) return 0;
    size_t off = 0;
    out[off++] = 0x6a;                           /* OP_RETURN */

    uint8_t version = ZANC_VERSION;
    bool ok = push_data_checked(out, &off, out_len,
                                (const uint8_t *)ZANC_LOKAD_BYTES, 4);
    ok = ok && push_data_checked(out, &off, out_len, &version, 1);
    ok = ok && push_data_checked(out, &off, out_len, &hash_type, 1);
    ok = ok && push_data_checked(out, &off, out_len, digest, ZANC_DIGEST_LEN);
    ok = ok && push_data_checked(out, &off, out_len,
                                 (const uint8_t *)(label ? label : ""),
                                 label_len);
    return ok ? off : 0;
}

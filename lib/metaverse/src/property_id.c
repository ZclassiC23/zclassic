/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Property identity — token table, equality, and the text codec.
 * Pure: no clock, no filesystem, no allocation. */

#include "metaverse/property_id.h"

#include "base/log_macros.h"

#include <string.h>

#define PROPID_LOG "metaverse.property_id"

/* Index IS the enum value, so a reordered enum breaks the build rather than
 * silently renaming a kind. */
static const char *const k_kind_tokens[METAVERSE_KIND_COUNT] = {
    "unknown",
    "content",
    "zcode-package",
    "znam-name",
    "zslp-asset",
    "hosted-service",
    "endpoint-onion",
    "storefront-product",
    "contract-swap",
};

const char *metaverse_kind_token(enum metaverse_property_kind kind)
{
    if (kind < 0 || kind >= METAVERSE_KIND_COUNT)
        return "unknown";
    return k_kind_tokens[kind];
}

enum metaverse_property_kind metaverse_kind_parse(const char *token)
{
    if (!token || token[0] == '\0')
        return METAVERSE_KIND_UNKNOWN;
    for (int i = 1; i < METAVERSE_KIND_COUNT; i++) {
        if (strcmp(token, k_kind_tokens[i]) == 0)
            return (enum metaverse_property_kind)i;
    }
    return METAVERSE_KIND_UNKNOWN;
}

bool metaverse_property_id_equal(const struct metaverse_property_id *a,
                                const struct metaverse_property_id *b)
{
    if (!a || !b) return false;
    if (a->kind != b->kind) return false;
    return memcmp(a->root, b->root, METAVERSE_PROPERTY_ROOT_LEN) == 0;
}

bool metaverse_property_id_valid(const struct metaverse_property_id *id)
{
    if (!id) return false;
    return id->kind > METAVERSE_KIND_UNKNOWN && id->kind < METAVERSE_KIND_COUNT;
}

static const char k_hex[] = "0123456789abcdef";

bool metaverse_property_id_render(const struct metaverse_property_id *id,
                                 char *out, size_t out_cap)
{
    if (!out || out_cap == 0)
        LOG_FAIL(PROPID_LOG, "render: no output buffer");
    out[0] = '\0';
    if (!metaverse_property_id_valid(id))
        LOG_FAIL(PROPID_LOG, "render: id is not well-formed");

    const char *token = metaverse_kind_token(id->kind);
    size_t tlen = strlen(token);
    size_t need = tlen + 1 + (METAVERSE_PROPERTY_ROOT_LEN * 2) + 1;
    if (out_cap < need)
        LOG_FAIL(PROPID_LOG, "render: buffer %zu < %zu needed", out_cap, need);

    memcpy(out, token, tlen);
    out[tlen] = ':';
    char *p = out + tlen + 1;
    for (size_t i = 0; i < METAVERSE_PROPERTY_ROOT_LEN; i++) {
        *p++ = k_hex[(id->root[i] >> 4) & 0x0f];
        *p++ = k_hex[id->root[i] & 0x0f];
    }
    *p = '\0';
    return true;
}

static bool hex_nibble(char c, uint8_t *out)
{
    if (c >= '0' && c <= '9') { *out = (uint8_t)(c - '0'); return true; }
    if (c >= 'a' && c <= 'f') { *out = (uint8_t)(c - 'a' + 10); return true; }
    if (c >= 'A' && c <= 'F') { *out = (uint8_t)(c - 'A' + 10); return true; }
    return false;
}

bool metaverse_property_id_parse(const char *text,
                                struct metaverse_property_id *out)
{
    if (!text || !out)
        LOG_FAIL(PROPID_LOG, "parse: NULL text or output");

    const char *colon = strchr(text, ':');
    if (!colon)
        LOG_FAIL(PROPID_LOG, "parse: no kind separator in '%.32s'", text);

    char token[32];
    size_t tlen = (size_t)(colon - text);
    if (tlen == 0 || tlen >= sizeof(token))
        LOG_FAIL(PROPID_LOG, "parse: kind token length %zu out of range", tlen);
    memcpy(token, text, tlen);
    token[tlen] = '\0';

    enum metaverse_property_kind kind = metaverse_kind_parse(token);
    if (kind == METAVERSE_KIND_UNKNOWN)
        LOG_FAIL(PROPID_LOG, "parse: unknown kind token '%s'", token);

    const char *hex = colon + 1;
    if (strlen(hex) != METAVERSE_PROPERTY_ROOT_LEN * 2)
        LOG_FAIL(PROPID_LOG, "parse: root hex length %zu != %d",
                 strlen(hex), METAVERSE_PROPERTY_ROOT_LEN * 2);

    uint8_t root[METAVERSE_PROPERTY_ROOT_LEN];
    for (size_t i = 0; i < METAVERSE_PROPERTY_ROOT_LEN; i++) {
        uint8_t hi = 0, lo = 0;
        if (!hex_nibble(hex[i * 2], &hi) || !hex_nibble(hex[i * 2 + 1], &lo))
            LOG_FAIL(PROPID_LOG, "parse: non-hex digit at root byte %zu", i);
        root[i] = (uint8_t)((hi << 4) | lo);
    }

    out->kind = kind;
    memcpy(out->root, root, sizeof(root));
    return true;
}

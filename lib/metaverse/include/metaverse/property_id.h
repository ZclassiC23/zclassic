/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * property_id — the canonical name of one piece of sovereign digital
 * property. Pure rules: no I/O, no store, no chain, no allocation. Every
 * other metaverse layer (catalog, grants, receipts, broker) speaks this
 * type and nothing coarser.
 *
 * A property_id is (kind, root) where `root` is the underlying object's
 * OWN immutable root as the authoritative model already computes it — a
 * ZCODE package's content.v2 manifest root, a blob's manifest root, a
 * ZNAM registration hash, a ZSLP genesis txid, a market offer's manifest
 * root. This layer mints no identifier of its own, because a second
 * identifier would be a second ownership truth: two names for one object
 * eventually disagree about who owns it.
 *
 * `kind` is not decoration. Two different kinds may legitimately carry
 * the same 32 bytes (a blob published as a one-file ZCODE package shares
 * its manifest root), and they are DIFFERENT properties with different
 * authority sources and different available actions. Equality therefore
 * compares both fields, and the text form always carries both.
 *
 * Text form (the wire/CLI form, stable): "<kind_name>:<64 lowercase hex>",
 * e.g. "zcode_package:9f2c...". Lowercase hex only on output; parsing
 * accepts either case. Round-trips exactly.
 *
 * AUTHORITY SOURCE: each kind names the ONE existing model that owns its
 * ownership truth (third column of the kind table). This layer never
 * becomes that authority; it records which one to ask.
 */

#ifndef ZCL_METAVERSE_PROPERTY_ID_H
#define ZCL_METAVERSE_PROPERTY_ID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The property kinds, and for each one the wire name plus the existing
 * subsystem that owns its ownership truth. Adding a kind is one row here
 * plus one adapter row (metaverse/property_adapter.h) — the adapter table
 * is static_assert'd against this count, so a kind can never appear in
 * the vocabulary with no reader behind it.
 *
 * Room to extend is deliberate: world/object kinds land as further rows,
 * never as a parallel enum. */
#define METAVERSE_KIND_TABLE(X)                                              \
    X(CONTENT,            "content",            "vcs.blob_store")            \
    X(ZCODE_PACKAGE,      "zcode_package",      "vcs.package_store")         \
    X(ZNAM_NAME,          "znam_name",          "znam.registry")             \
    X(ZSLP_ASSET,         "zslp_asset",         "zslp.ledger")               \
    X(HOSTED_SERVICE,     "hosted_service",     "service.registry")          \
    X(ENDPOINT_ONION,     "endpoint_onion",     "net.onion_service")         \
    X(STOREFRONT_PRODUCT, "storefront_product", "store.product")             \
    X(CONTRACT_SWAP,      "contract_swap",      "swap.contract")

enum metaverse_kind {
    /* Zero is not a kind. A zeroed struct is an explicitly invalid id, so
     * a forgotten initialization can never read as CONTENT. */
    METAVERSE_KIND_UNKNOWN = 0,
#define METAVERSE_KIND_ENUM(id_, name_, authority_) METAVERSE_KIND_##id_,
    METAVERSE_KIND_TABLE(METAVERSE_KIND_ENUM)
#undef METAVERSE_KIND_ENUM
    METAVERSE_KIND_COUNT
};

#define METAVERSE_ROOT_BYTES 32u

/* "storefront_product" (18) + ':' + 64 hex + NUL = 84; 96 leaves room for
 * one more long kind name without a wire change. */
#define METAVERSE_ID_TEXT_MAX 96u

struct metaverse_property_id {
    enum metaverse_kind kind;
    uint8_t root[METAVERSE_ROOT_BYTES];
};

/* Wire name / authority source for a kind. Never NULL; an out-of-range or
 * UNKNOWN kind renders as "unknown". */
const char *metaverse_kind_name(enum metaverse_kind kind);
const char *metaverse_kind_authority(enum metaverse_kind kind);

/* Exact wire-name lookup. METAVERSE_KIND_UNKNOWN when nothing matches
 * (including NULL, "", and "unknown" itself). */
enum metaverse_kind metaverse_kind_from_name(const char *name);

/* True for a real kind (not UNKNOWN, not >= COUNT). */
bool metaverse_kind_valid(enum metaverse_kind kind);

/* Build an id. Rejects a NULL out, a NULL root, an invalid kind, and an
 * all-zero root (no authoritative model mints one, so accepting it would
 * make "uninitialized" indistinguishable from "a real object"). *out is
 * zeroed on every rejection. */
bool metaverse_property_id_make(enum metaverse_kind kind,
                                const uint8_t root[METAVERSE_ROOT_BYTES],
                                struct metaverse_property_id *out);

/* Render "<kind_name>:<64 lowercase hex>" into out (cap >=
 * METAVERSE_ID_TEXT_MAX). False (and out[0] = 0 when cap > 0) on a NULL
 * out, a short buffer, or an invalid id. */
bool metaverse_property_id_format(const struct metaverse_property_id *id,
                                  char *out, size_t cap);

/* Parse the text form. Accepts upper or lower hex; requires exactly one
 * ':', a known kind name, exactly 64 hex digits, and no trailing bytes.
 * *out is zeroed on every rejection. */
bool metaverse_property_id_parse(const char *text,
                                 struct metaverse_property_id *out);

/* Both kind and root must match. NULL on either side is never equal. */
bool metaverse_property_id_equal(const struct metaverse_property_id *a,
                                 const struct metaverse_property_id *b);

/* True when the id is well-formed (valid kind, non-zero root). */
bool metaverse_property_id_valid(const struct metaverse_property_id *id);

/* A kind-set is one bit per kind, so a grant can be scoped to kinds without
 * enumerating property ids. Bit 0 (UNKNOWN) is never set by
 * metaverse_kind_bit and a set containing it matches nothing. */
typedef uint32_t metaverse_kind_set;

static inline metaverse_kind_set metaverse_kind_bit(enum metaverse_kind kind)
{
    if (kind <= METAVERSE_KIND_UNKNOWN || kind >= METAVERSE_KIND_COUNT)
        return 0u;
    return (metaverse_kind_set)1u << (unsigned)kind;
}

#endif /* ZCL_METAVERSE_PROPERTY_ID_H */

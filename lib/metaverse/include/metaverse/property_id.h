/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Property identity — the (kind, immutable_root) pair every metaverse
 * property is named by.
 *
 * ── SEAM NOTICE ───────────────────────────────────────────────────────────
 * This is the MINIMAL stand-in for the canonical property_id, which the
 * property-catalog lane owns. It exists because the grant/receipt engine
 * cannot be written without a property name, and the two lanes were built in
 * parallel. Reconcile FIELD-FOR-FIELD at merge time; the grant engine depends
 * on exactly three properties of this type and nothing else:
 *   1. `kind` is a small closed enum with a stable token per value,
 *   2. `root` is 32 immutable bytes that never change for the life of the
 *      property (a revision bump changes the property's REVISION, never its
 *      root — that is what makes optimistic-concurrency control possible),
 *   3. equality is bytewise over (kind, root).
 * Anything the catalog adds — authority_source, provenance, descriptor root,
 * evidence grade — is catalog-side and does not belong here.
 *
 * There is no ownership truth in this file. A property_id NAMES a property;
 * who controls it is answered by the catalog, freshly, at commit time. */

#ifndef ZCL_METAVERSE_PROPERTY_ID_H
#define ZCL_METAVERSE_PROPERTY_ID_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* The immutable root is a 32-byte commitment (SHA3-256 sized). */
#define METAVERSE_PROPERTY_ROOT_LEN 32

/* Rendered form is "<kind-token>:<64 lowercase hex>". */
#define METAVERSE_PROPERTY_ID_TEXT_MAX 96

enum metaverse_property_kind {
    METAVERSE_KIND_UNKNOWN = 0,
    METAVERSE_KIND_CONTENT,
    METAVERSE_KIND_ZCODE_PACKAGE,
    METAVERSE_KIND_ZNAM_NAME,
    METAVERSE_KIND_ZSLP_ASSET,
    METAVERSE_KIND_HOSTED_SERVICE,
    METAVERSE_KIND_ENDPOINT_ONION,
    METAVERSE_KIND_STOREFRONT_PRODUCT,
    METAVERSE_KIND_CONTRACT_SWAP,
    METAVERSE_KIND_COUNT
};

struct metaverse_property_id {
    enum metaverse_property_kind kind;
    uint8_t root[METAVERSE_PROPERTY_ROOT_LEN];
};

/* Stable lowercase token per kind ("content", "zcode-package", …). Tests and
 * the CLI both assert these exactly, so they are contract. UNKNOWN and any
 * out-of-range value render "unknown". */
const char *metaverse_kind_token(enum metaverse_property_kind kind);

/* Parse a kind token. Returns METAVERSE_KIND_UNKNOWN for NULL, "", "unknown",
 * and anything unrecognized — the caller decides whether that is fatal. */
enum metaverse_property_kind metaverse_kind_parse(const char *token);

/* A kind-set is one bit per kind, so a grant can be scoped to kinds without
 * enumerating property ids. Bit 0 (UNKNOWN) is never set by
 * metaverse_kind_bit and a set containing it matches nothing. */
typedef uint32_t metaverse_kind_set;

static inline metaverse_kind_set metaverse_kind_bit(
    enum metaverse_property_kind kind)
{
    if (kind <= METAVERSE_KIND_UNKNOWN || kind >= METAVERSE_KIND_COUNT)
        return 0u;
    return (metaverse_kind_set)1u << (unsigned)kind;
}

/* True when both ids name the same property: same kind AND same 32-byte root.
 * NULL on either side is false (never "equal because both absent"). */
bool metaverse_property_id_equal(const struct metaverse_property_id *a,
                                const struct metaverse_property_id *b);

/* An id is well-formed when its kind is in range and not UNKNOWN. An all-zero
 * root IS well-formed — it is a legitimate commitment value, and rejecting it
 * here would put a consensus-shaped rule in a naming header. */
bool metaverse_property_id_valid(const struct metaverse_property_id *id);

/* Render "<kind-token>:<hex64>" into `out` (needs
 * METAVERSE_PROPERTY_ID_TEXT_MAX + 1 bytes). Always NUL-terminates when
 * out_cap > 0; returns false on NULL/short buffer having written "". */
bool metaverse_property_id_render(const struct metaverse_property_id *id,
                                 char *out, size_t out_cap);

/* Inverse of _render. Returns false (and leaves *out untouched) on any
 * malformed input: missing ':', unknown kind, wrong hex length, non-hex
 * digit. Uppercase hex is accepted on parse; render only ever emits lower. */
bool metaverse_property_id_parse(const char *text,
                                struct metaverse_property_id *out);

#endif /* ZCL_METAVERSE_PROPERTY_ID_H */

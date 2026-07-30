/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * agent_broker_proto — the TYPED wire protocol a confined agent speaks to the
 * metaverse agent broker over a Unix-domain socket.
 *
 * THE INVARIANT OF THIS FILE: the wire cannot express a filesystem path, a
 * shell word, an SQL string, or an RPC method name. A request is a fixed-layout
 * record — a verb from a closed enum, a 32-byte property id, a kind, a value in
 * zatoshis, and ONE bounded `param` that mvap_param_is_safe() restricts to
 * [A-Za-z0-9_.-] with no '/' , no '\\' and no "..". There is therefore no
 * request an agent can compose that names a file, so path escape is not
 * mitigated here, it is unrepresentable.
 *
 * Encoding is explicit little-endian byte-at-a-time (never a struct memcpy),
 * length-prefixed, and bounded by MVAP_MAX_FRAME. Decode validates magic,
 * version, verb range, and every declared length against the remaining bytes
 * before it copies anything.
 *
 * Reconciliation note (parallel lanes): the property-kind enum and the
 * 32-byte property id are the shapes Lane 1 (property catalog) owns; the
 * receipt id echoed in a response is the shape Lane 2 (grant/receipt engine)
 * owns. Both are represented here by value, not by including their headers, so
 * this file compiles and is provable on its own.
 */

#ifndef ZCL_SESSION_AGENT_BROKER_PROTO_H
#define ZCL_SESSION_AGENT_BROKER_PROTO_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* "MVA1" — magic + version guard the frame boundary, so a stray write from a
 * non-agent process on the socket is rejected before any field is trusted. */
#define MVAP_MAGIC       0x3141564Du
#define MVAP_VERSION     1u

#define MVAP_PROPERTY_ID_LEN 32
#define MVAP_RECEIPT_ID_LEN  32
#define MVAP_PARAM_MAX       96
#define MVAP_BODY_MAX        1024

/* Wire-record sizes: fixed header + at most one bounded tail. */
#define MVAP_REQ_FIXED   56
#define MVAP_RESP_FIXED  50
#define MVAP_MAX_FRAME   (MVAP_RESP_FIXED + MVAP_BODY_MAX)

/* The closed action vocabulary. Values are wire constants — append only. */
enum mvap_verb {
    MVAP_VERB_NONE              = 0,
    MVAP_VERB_INSPECT           = 1,
    MVAP_VERB_LIST              = 2,
    MVAP_VERB_HOST              = 3,
    MVAP_VERB_PUBLISH_REVISION  = 4,
    MVAP_VERB_UPDATE_POINTER    = 5,
    MVAP_VERB_SELL              = 6,
    MVAP_VERB_BUY               = 7,
    MVAP_VERB_DELIVER           = 8,
    MVAP_VERB_LEASE             = 9,
    MVAP_VERB_TRANSFER          = 10,
    MVAP_VERB_ACCEPT_PAYMENT    = 11,
    MVAP_VERB_DELEGATE          = 12,
    MVAP_VERB_REVOKE            = 13,
    MVAP_VERB__COUNT            = 14,
};

/* Property kinds. Lane 1 owns the authoritative list; these are the two
 * adapters the first vertical slice requires plus the declared future rows. */
enum mvap_kind {
    MVAP_KIND_ANY       = 0,
    MVAP_KIND_CONTENT   = 1,   /* ordinary content / blob                */
    MVAP_KIND_ZCODE     = 2,   /* ZCODE package                          */
    MVAP_KIND_NAME      = 3,   /* ZNAM name                              */
    MVAP_KIND_ASSET     = 4,   /* ZSLP asset or badge                    */
    MVAP_KIND_SERVICE   = 5,   /* hosted service                         */
    MVAP_KIND_ENDPOINT  = 6,   /* endpoint / onion site                  */
    MVAP_KIND_PRODUCT   = 7,   /* storefront product                     */
    MVAP_KIND_CONTRACT  = 8,   /* contract / swap                        */
    MVAP_KIND__COUNT    = 9,
};

/* Response status. Negative values are refusals; every refusal names WHICH
 * check failed, because "denied" alone cannot be operated on. */
enum mvap_status {
    MVAP_OK                        = 0,
    MVAP_ERR_BAD_REQUEST           = -1,
    MVAP_ERR_UNKNOWN_VERB          = -2,
    MVAP_ERR_NOT_FOUND             = -3,
    MVAP_ERR_INTERNAL              = -4,
    MVAP_ERR_DENIED_NO_GRANT       = -5,
    MVAP_ERR_DENIED_REVOKED        = -6,
    MVAP_ERR_DENIED_EXPIRED        = -7,
    MVAP_ERR_DENIED_ACTION         = -8,   /* verb not in the grant's mask   */
    MVAP_ERR_DENIED_PROPERTY       = -9,   /* property id not in grant scope */
    MVAP_ERR_DENIED_KIND           = -10,  /* kind not in grant scope        */
    MVAP_ERR_DENIED_VALUE          = -11,  /* over the per-action ceiling    */
    MVAP_ERR_DENIED_BUDGET         = -12,  /* over the cumulative budget     */
    MVAP_ERR_DENIED_RATE           = -13,  /* over the rate/window limit     */
    MVAP_ERR_DENIED_COUNTERPARTY   = -14,  /* not on the allowlist           */
    MVAP_ERR_DENIED_DELEGATION     = -15,  /* delegation not permitted/depth */
    MVAP_ERR_DENIED_PEER_IDENTITY  = -16,  /* SO_PEERCRED check failed       */
    MVAP_ERR_PLAN_FAILED           = -17,
    MVAP_ERR_COMMIT_FAILED         = -18,
    MVAP_ERR_REVISION_MOVED        = -19,  /* commit-time recheck disagreed  */
};

/* One request. `param` is NUL-terminated after decode and always passes
 * mvap_param_is_safe(); `property_id` all-zero means "no specific property"
 * (only LIST and REVOKE accept that). */
struct mvap_request {
    uint32_t verb;                                  /* enum mvap_verb   */
    uint32_t request_id;                            /* idempotency key  */
    uint64_t value_zats;
    uint8_t  property_id[MVAP_PROPERTY_ID_LEN];
    uint16_t kind;                                  /* enum mvap_kind   */
    char     param[MVAP_PARAM_MAX + 1];
};

/* One response. `receipt_id` is all-zero for a pure read (no mutation, no
 * receipt); `body` is bounded JSON text. */
struct mvap_response {
    uint32_t verb;
    uint32_t request_id;
    int32_t  status;                                /* enum mvap_status */
    uint8_t  receipt_id[MVAP_RECEIPT_ID_LEN];
    char     body[MVAP_BODY_MAX + 1];
};

/* True iff `s` is a safe wire token: NUL-terminated, at most MVAP_PARAM_MAX
 * bytes, every byte in [A-Za-z0-9_.-], and no ".." anywhere. The '/' and '\\'
 * exclusions plus the ".." rejection are what make a path unrepresentable. An
 * empty string is safe (it means "no parameter"). */
bool mvap_param_is_safe(const char *s);

/* True iff every byte of `id` is zero — the "no specific property" sentinel. */
bool mvap_property_id_is_zero(const uint8_t id[MVAP_PROPERTY_ID_LEN]);

/* Stable names for the wire enums (used in receipts, audit rows, and replies).
 * Never NULL: an out-of-range value renders as "unknown". */
const char *mvap_verb_name(uint32_t verb);
const char *mvap_kind_name(uint16_t kind);
const char *mvap_status_name(int32_t status);

/* Parse a verb/kind name back to its wire value; returns MVAP_VERB_NONE /
 * MVAP_KIND_ANY on no match. Names are compared case-insensitively. */
uint32_t mvap_verb_from_name(const char *name);
uint16_t mvap_kind_from_name(const char *name);

/* True iff the verb MUTATES state (so it must run PLAN -> COMMIT and mint a
 * receipt). INSPECT and LIST are the only reads. */
bool mvap_verb_is_mutation(uint32_t verb);

/* Encode `req` into `out` (capacity `out_cap`). Writes the 4-byte little-endian
 * length prefix followed by the record. Returns the total bytes written, or 0
 * on a NULL argument, an unsafe param, or insufficient capacity. */
size_t mvap_request_encode(const struct mvap_request *req, uint8_t *out,
                           size_t out_cap);

/* Decode a request record from `in` (`in_len` bytes, WITHOUT the length
 * prefix — the caller has already framed it). Returns true and fills `out`
 * only when magic, version, verb range, kind range, and the declared param
 * length all validate and the param passes mvap_param_is_safe(). */
bool mvap_request_decode(const uint8_t *in, size_t in_len,
                         struct mvap_request *out);

/* Encode/decode a response, same framing contract as the request pair. */
size_t mvap_response_encode(const struct mvap_response *resp, uint8_t *out,
                            size_t out_cap);
bool mvap_response_decode(const uint8_t *in, size_t in_len,
                          struct mvap_response *out);

/* Read the 4-byte little-endian frame length at the head of `in`. Returns 0
 * when `in_len` < 4 or the declared length exceeds MVAP_MAX_FRAME — both are
 * "do not trust this stream" answers, never a silent clamp. */
uint32_t mvap_frame_length(const uint8_t *in, size_t in_len);

/* Frame prefix width, so callers need no magic 4. */
#define MVAP_FRAME_PREFIX 4

#endif /* ZCL_SESSION_AGENT_BROKER_PROTO_H */

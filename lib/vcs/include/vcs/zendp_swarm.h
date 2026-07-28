/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * zendp_swarm — publish, accept and fetch SIGNED ENDPOINT RECORDS over
 * the content-addressed swarm, and bind them to the chain.
 *
 * This is the slice docs/work/NAT_AND_ONION_TRANSPORT.md names first:
 * "Directory records should eventually be SIGNED by the peer key (onion
 * + clearnet endpoints + height + expiry), so a poisoned directory
 * cannot reroute peers to attacker endpoints."
 *
 * WHAT IS NEW ON THE WIRE: nothing. An endpoint record is a zid_doc
 * (body tag "ZIDE", zid/zendp.h) carried as a blob (vcs/blob_store.h),
 * which is a one-file content.v2 package on the already-frozen
 * 'zpkgswm' codec. No new message, no new store, no new bound.
 *
 * WHAT IS NEW HERE, AND IS THE POINT: the chain check. zdesc_swarm
 * verifies against a CALLER-SUPPLIED master key and says so. This file
 * does not accept a key from the caller at all — the doc carries its
 * own master_pubkey, and that key is resolved through the on-chain
 * zid_identities projection (db_zid_identity_find). A record whose key
 * was never anchored, or was rotated away, or was revoked, is refused
 * with a DISTINCT NAMED ERROR for each case. With no lookup registered
 * the module fails CLOSED (ZENDP_ERR_NO_ANCHOR_LOOKUP): "I could not
 * ask the chain" is never quietly the same as "the chain said yes".
 *
 * THE PORT, and why. db_zid_identity_find lives in app/models, far
 * above lib/vcs, so the lookup is inverted: lib/ names the port,
 * config/ registers the implementation at process start — the same
 * shape node_db_runtime.h and net/onion_discovery.h use.
 *
 * THE RECORD IS STILL A HINT — read this before consuming one.
 * A verified record proves: this identity, anchored at this height,
 * signed "you can reach me at these endpoints, and this document is
 * valid until this expiry". It does NOT prove that the party answering
 * at that address is that identity. Binding the SESSION to the key
 * needs the Noise v2 transport (-v2transport, net/connman.h), which is
 * default OFF because every peer on the live network speaks v1 today.
 * Until that flips, an endpoint record must only ever ADD a place to
 * try; it must never narrow, rank down, or exclude any peer. A poisoned
 * record can then cost at most one wasted connection attempt.
 *
 * FRESHNESS IS PER-RECORD. There is no refresh heartbeat in this
 * design and no global "last updated" clock: a record is fresh because
 * ITS OWN signed window is open and its seq is the highest we have
 * seen. An entry whose window has closed simply stops being projected.
 *
 * PERIOD ADDRESSING. A record is addressable at the period it was
 * published in and the one after (zdesc's period contract, shared
 * unchanged), so a publisher republishes each period. */

#ifndef ZCL_VCS_ZENDP_SWARM_H
#define ZCL_VCS_ZENDP_SWARM_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "zid/zendp.h"
#include "zid/zid.h"

struct vcs_package_store;
struct vcs_swarm_engine;

/* Identities tracked locally. One entry per master_pubkey. */
#define ZENDP_DIR_MAX 32

/* ── the chain-binding port (registered from config/) ──────────────── */

enum zendp_anchor_state {
    ZENDP_ANCHOR_UNKNOWN = 0, /* not asked (no lookup registered) */
    ZENDP_ANCHOR_ABSENT,      /* asked; the key has no anchor row */
    ZENDP_ANCHOR_ACTIVE,      /* anchored and live */
    ZENDP_ANCHOR_ROTATED,     /* anchored, then superseded by a successor */
    ZENDP_ANCHOR_REVOKED,     /* anchored, then retired */
};

const char *zendp_anchor_state_string(enum zendp_anchor_state s);

struct zendp_anchor {
    enum zendp_anchor_state state;
    int32_t anchor_height;    /* height that anchored the key (0 if none) */
    int32_t updated_height;   /* height of the last status change */
};

/* Resolve a master key against the on-chain identity projection.
 * Returns false when the lookup could not run at all (no node.db yet,
 * read failure) — which is NOT the same as "no such identity", and is
 * reported separately. On true, *out carries the answer, ABSENT
 * included. */
typedef bool (*zendp_anchor_lookup_fn)(void *ctx, const uint8_t pubkey[32],
                                       struct zendp_anchor *out);

/* Install (or, with fn == NULL, remove) the port. Idempotent, and safe
 * to call before or after any directory exists. */
void zendp_set_anchor_lookup(zendp_anchor_lookup_fn fn, void *ctx);
bool zendp_anchor_lookup_registered(void);

enum zendp_result {
    ZENDP_OK = 0,
    ZENDP_ERR_NULL,               /* null argument */
    ZENDP_ERR_SHAPE,              /* record fails zendp_valid */
    ZENDP_ERR_ENCODE,             /* doc/body would not encode */
    ZENDP_ERR_SIGN,               /* zendp_sign refused (bad window/seed) */
    ZENDP_ERR_BLOB,               /* blob put/get refused (named in the log) */
    ZENDP_ERR_ABSENT,             /* no local witness for the record key */
    ZENDP_ERR_DECODE,             /* bytes are not a well-formed zid doc */
    ZENDP_ERR_VERIFY,             /* signature or validity window failed */
    ZENDP_ERR_BODY,               /* doc verified but body is not a ZIDE */
    ZENDP_ERR_KEY_MISMATCH,       /* doc is signed by a different identity */
    ZENDP_ERR_STALE,              /* seq does not supersede what we hold */
    ZENDP_ERR_FULL,               /* directory has no free slot */
    ZENDP_ERR_FETCH,              /* swarm refused the download */
    /* The four chain outcomes, each named separately on purpose. */
    ZENDP_ERR_NO_ANCHOR_LOOKUP,   /* nothing can ask the chain — fail closed */
    ZENDP_ERR_ANCHOR_UNAVAILABLE, /* asked; the lookup itself failed */
    ZENDP_ERR_NOT_ANCHORED,       /* the signing key was never anchored */
    ZENDP_ERR_ROTATED,            /* the signing key was rotated away */
    ZENDP_ERR_REVOKED,            /* the signing key was revoked */
};

const char *zendp_result_string(enum zendp_result r);

/* Ask the chain about one key. Public because it is the same question
 * every consumer of a record wants answered, and there must be exactly
 * one implementation of the mapping from a projection row to a verdict.
 * `out` may be NULL. */
enum zendp_result zendp_anchor_check(const uint8_t master_pubkey[32],
                                     struct zendp_anchor *out);

/* ── the directory (a caller-owned projection, never a truth source) ── */

struct zendp_entry {
    bool used;
    uint8_t record_key[32];  /* zendp_record_key(master_pubkey, period) */
    uint64_t period;
    uint8_t master_pubkey[32];
    uint8_t root[32];        /* blob root of the canonical doc wire */
    struct zid_doc doc;      /* the accepted doc — the seq authority */
    struct zendp ep;         /* its decoded ZIDE body */
    struct zendp_anchor anchor; /* what the chain said when we accepted */
};

struct zendp_directory {
    struct zendp_entry e[ZENDP_DIR_MAX];
    size_t count;
};

/* What a consumer gets: the endpoints plus the provenance that makes
 * them worth trying, and nothing it would have to re-derive. */
struct zendp_record_view {
    uint8_t master_pubkey[32];
    uint64_t seq;
    uint64_t expiry;
    int32_t anchor_height;
    struct zendp ep;
};

void zendp_directory_init(struct zendp_directory *dir);

/* Find by the blinded record key — the anti-enumeration address. */
bool zendp_directory_lookup(const struct zendp_directory *dir,
                            const uint8_t record_key[32],
                            const struct zendp_entry **out);

/* Find by identity. */
bool zendp_directory_find(const struct zendp_directory *dir,
                          const uint8_t master_pubkey[32],
                          const struct zendp_entry **out);

/* Copy out every record that is (a) inside its own signed validity
 * window at now_unix and (b) backed by an ACTIVE on-chain anchor.
 * Returns how many were written. Never allocates, never blocks on I/O,
 * never re-asks the chain — the anchor verdict was recorded when the
 * record was accepted. This is the discovery projection. */
size_t zendp_directory_records(const struct zendp_directory *dir,
                               uint64_t now_unix,
                               struct zendp_record_view *out, size_t max);

/* The one process-wide directory, and a locked snapshot of it. */
struct zendp_directory *zendp_directory_global(void);
size_t zendp_global_records(uint64_t now_unix, struct zendp_record_view *out,
                            size_t max);

/* ── publish ───────────────────────────────────────────────────────── */

/* Sign `ep` with `seed`, store the canonical doc wire as a blob in
 * `store`, and install the directory entry under
 * zendp_record_key(pubkey, zdesc_period_at(now_unix)).
 *
 * Monotonic by construction: a held record for this identity must be
 * strictly superseded or the call is refused ZENDP_ERR_STALE with
 * NOTHING written to the store.
 *
 * Publishing does NOT require the key to be anchored yet — an operator
 * may sign before the anchor transaction confirms — but the anchor
 * state observed at publish IS recorded, and only ACTIVE entries are
 * ever projected to discovery. out_root / out_pubkey may be NULL. */
enum zendp_result zendp_publish_to(struct vcs_package_store *store,
                                   struct zendp_directory *dir,
                                   const struct zendp *ep, uint64_t seq,
                                   uint64_t expiry, const uint8_t seed[32],
                                   uint64_t now_unix, uint8_t out_root[32],
                                   uint8_t out_pubkey[32]);

/* ── accept / fetch ────────────────────────────────────────────────── */

/* Ingest record wire bytes from anywhere (the shape a post-swarm
 * download uses). NO KEY IS SUPPLIED: the doc's own master_pubkey is
 * resolved against the chain, and acceptance requires an ACTIVE anchor.
 * The stored root is derived from the CONTENT, never from a claim.
 * ep_out / pubkey_out may be NULL. */
enum zendp_result zendp_accept(struct zendp_directory *dir,
                               const uint8_t *wire, size_t wire_len,
                               uint64_t now_unix, struct zendp *ep_out,
                               uint8_t pubkey_out[32]);

/* Resolve an identity's current record: derive the record key for the
 * current period, fall back to the previous one, read the blob back out
 * of `store`, and re-run the WHOLE pipeline (decode, key identity,
 * signature, window, chain anchor) on the bytes that came off disk.
 * Read-only: the directory is not mutated. */
enum zendp_result zendp_fetch_from(struct vcs_package_store *store,
                                   const struct zendp_directory *dir,
                                   const uint8_t master_pubkey[32],
                                   uint64_t now_unix, struct zendp *ep_out);

/* Schedule a swarm download of the identity's record blob. Requires a
 * local witness for the record key -> root mapping (the same open edge
 * zdesc_swarm has); without one this is ZENDP_ERR_ABSENT, by name. */
enum zendp_result zendp_swarm_fetch(struct vcs_swarm_engine *engine,
                                    const struct zendp_directory *dir,
                                    const uint8_t master_pubkey[32],
                                    int64_t day, uint64_t now_unix);

#endif /* ZCL_VCS_ZENDP_SWARM_H */

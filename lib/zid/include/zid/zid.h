/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ZID — sovereign identity layer Phase 1: signed identity documents and
 * blinded record keys (pure codec; see docs/spec/sovereign-identity-layer.md).
 *
 * A chain-anchored master ed25519 key signs off-chain documents
 * {version, master_pubkey, seq, expiry, body, signature}. Consumers verify
 * the signature against the anchored key and enforce a monotonic seq.
 * Blinded keys (SHA3-256 of pubkey + period, Tor v3 pattern) keep service
 * records non-enumerable. No allocation anywhere: caller buffers only.
 *
 * Anchor domains: an append-only Merkle Mountain Range over record digests
 * lets ONE on-chain ZANC anchor commit an unbounded batch of records, with
 * O(log n) inclusion proofs per record (see
 * docs/spec/sovereign-identity-layer.md). The MMR is self-contained here
 * (same shape as lib/chain's mmr.c) with a zid-specific "ZIDL" leaf domain
 * tag so a zid proof can never be replayed against the chain MMR. */

#ifndef ZCL_ZID_H
#define ZCL_ZID_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define ZID_DOC_VERSION 1
#define ZID_BODY_MAX 1024
#define ZID_DOC_MAX (1 + 32 + 8 + 8 + 2 + ZID_BODY_MAX + 64)

struct zid_doc {
    uint8_t master_pubkey[32];
    uint64_t seq;        /* monotonic per identity */
    uint64_t expiry;     /* unix seconds; doc invalid at/after */
    uint16_t body_len;
    uint8_t body[ZID_BODY_MAX];
    uint8_t signature[64];
};

/* blinded = SHA3-256("zid-blind" ‖ master_pubkey ‖ period_le64) —
 * anti-enumeration record key (Tor v3 blinded-pubkey pattern). Only
 * someone who already knows the master key can derive the record key. */
void zid_blinded_key(uint8_t out[32], const uint8_t master_pubkey[32],
                     uint64_t period);

/* Wire: version:1 ‖ pubkey:32 ‖ seq:8 LE ‖ expiry:8 LE ‖ body_len:2 LE ‖
 * body ‖ sig:64. The signature covers everything before it.
 * Returns the encoded size (51 + body_len + 64), or 0 on error. */
size_t zid_doc_encode(uint8_t *out, size_t out_len, const struct zid_doc *doc);

/* Bounds-strict decode: `len` must be exactly 51 + body_len + 64 and the
 * version byte must be ZID_DOC_VERSION. Does NOT verify the signature. */
bool zid_doc_decode(struct zid_doc *doc, const uint8_t *buf, size_t len);

/* Verify signature + version + expiry against a caller-supplied clock:
 * false when body_len is out of range, when now_unix >= expiry, or when
 * the ed25519 signature over the wire prefix does not verify. */
bool zid_doc_verify(const struct zid_doc *doc, uint64_t now_unix);

/* Fill + sign a doc: derives the keypair from `seed`, sets master_pubkey,
 * copies body/seq/expiry, and signs the wire prefix (uses
 * ed25519_keypair/ed25519_sign). */
bool zid_doc_sign(struct zid_doc *doc, const uint8_t *body, uint16_t body_len,
                  uint64_t seq, uint64_t expiry, const uint8_t seed[32]);

/* Monotonic-seq rule: true iff candidate supersedes current (strictly
 * greater seq, same master_pubkey). */
bool zid_doc_supersedes(const struct zid_doc *candidate,
                        const struct zid_doc *current);

/* ── Anchor-domain tree (MMR over record digests) ──────────────────
 *
 * Domain separation (SHA3-256):
 *   Leaf:     SHA3-256(0x00 ‖ "ZIDL" ‖ record_digest32)
 *   Internal: SHA3-256(0x01 ‖ left ‖ right)
 *   Root:     SHA3-256(0x02 ‖ peak_0 ‖ … ‖ peak_k)   (bagged peaks)
 * Root edge cases mirror chain mmr.c exactly: empty tree → all-zero root;
 * single peak → the peak itself (no bag); ≥2 peaks → bagged. */

#define ZID_TREE_MAX_PEAKS 64

struct zid_tree {
    uint64_t num_leaves;
    uint8_t  peaks[ZID_TREE_MAX_PEAKS][32];
    uint32_t num_peaks;
};

void   zid_tree_init(struct zid_tree *t);
/* Append a 32-byte record digest as a leaf (leaf hash includes the "ZIDL"
 * tag). Updates peaks; false only on internal capacity overflow. */
bool   zid_tree_append(struct zid_tree *t, const uint8_t record_digest[32]);
void   zid_tree_root(const struct zid_tree *t, uint8_t out[32]);

/* Inclusion proof for leaf `index`, rebuilt from the full leaf list.
 * The tree struct stores peaks only, so proofs require the operator-side
 * leaf digests (batch cadence — O(n) rebuild is fine). Emits the sibling
 * path to the leaf's peak (bottom-up) followed by the OTHER peaks in
 * bagging order, plus the bagged root in root_out. proof_siblings has
 * room for up to ZID_TREE_MAX_PEAKS hashes; false if index >= num_leaves
 * or the proof would exceed that capacity. */
bool   zid_tree_prove_from_leaves(const uint8_t leaves[][32],
                                  uint64_t num_leaves, uint64_t index,
                                  uint8_t proof_siblings[][32],
                                  uint32_t *proof_len, uint8_t root_out[32]);

/* THE critical verifier-side function — everyone must agree on it forever:
 * true iff record_digest is leaf `index` of a tree with `num_leaves` leaves
 * whose bagged root equals `root`. Recomputes the leaf (with the "ZIDL"
 * tag), folds the sibling path to the peak, bags with the remaining proof
 * hashes, and compares the root. */
bool   zid_tree_verify(const uint8_t root[32], const uint8_t record_digest[32],
                       uint64_t index, uint64_t num_leaves,
                       const uint8_t proof_siblings[][32], uint32_t proof_len);

#endif /* ZCL_ZID_H */

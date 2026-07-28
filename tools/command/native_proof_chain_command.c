/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * `zcode proof walk` — the light-client proof-chain walker
 * (docs/spec/sovereign-identity-layer.md, "Light-client proof chain").
 *
 * The spec's chain — FlyClient header proof -> anchor tx's block
 * inclusion -> domain MMR root -> record inclusion proof -> zid
 * signature -> master-key anchor — existed only as prose, with each rung
 * verified by a different command in a different place, if at all. This
 * is that chain as ONE read-only command over evidence the caller hands
 * in: no node contact, no chain database, no 10 GB of blocks.
 *
 * THE OUTPUT IS THE PRODUCT. Each rung reports independently as
 * passed / failed / not_checked, and `not_checked` ALWAYS carries a
 * `reason`. Three properties make the report un-cheatable:
 *
 *   - `verified_prefix` counts only CONSECUTIVE passed rungs from 1. A
 *     later rung still states its own honest verdict, but it can never
 *     lift the prefix past an earlier break — the mechanical encoding of
 *     "never report a later rung as passing on the strength of a broken
 *     earlier one".
 *   - `chain_complete` is true only when all seven rungs passed. Rung 7
 *     is structurally not_checked in this build (see the SEAM below), so
 *     it is false, and `chain_complete_reason` says why.
 *   - `report[]` renders `PASS` / `FAIL` / `-- SKIP` so a skipped rung is
 *     never mistaken for a passing one at a glance.
 *
 * WHY A BROKEN CHAIN STILL RETURNS status=PASSED. serialize_reply() in
 * lib/kernel/src/command_registry.c emits `data` only for PASSED /
 * ACCEPTED and drops it entirely on any other status. Returning FAILED on
 * a broken rung would therefore delete the seven-rung report — the whole
 * product — and collapse it to one error code, which is exactly the lie
 * this command exists to prevent. So: a walk that RAN always returns
 * PASSED with every verdict in `data`; zcl_command_reply_fail() is
 * reserved for input that makes a walk impossible at all (no document),
 * where there is no report to lose. Read `verdict` / `first_break` /
 * `chain_complete`, never the exit code, as the answer.
 *
 * Layering: this consumes lib/zid (rung 5-6), lib/zanc (rung 3),
 * lib/bloom + lib/primitives (rung 2) and core/chainparams (rung 1).
 * lib/zid ranks BELOW bloom/chain/script in config/lib_module_order.def,
 * so a walker inside lib/zid would be an upward reference; tools/ sits at
 * the top of the graph and this is an operator diagnostic, so it lives
 * here. Frozen wire formats (zid_proof, zid_doc, ZANC OP_RETURN) are
 * consumed, never re-implemented: zid_tree_verify() is THE verifier-side
 * function and is called, not copied. */

#include "command/native_command.h"

#include "base/log_macros.h"
#include "base/safe_alloc.h"
#include "bloom/merkle.h"
#include "chain/chainparams.h"
#include "chain/equihash.h"
#include "chain/pow.h"
#include "core/serialize.h"
#include "core/uint256.h"
#include "encoding/utilstrencodings.h"
#include "json/json.h"
#include "platform/time_compat.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "script/script.h"
#include "zanc/zanc.h"
#include "zid/zid.h"

#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#define PCW_LOG "zcode.proof.walk"

#define PCW_RUNGS 7
#define PCW_DETAIL_MAX 240
/* Ancestor headers and merkle siblings are caller-supplied; bound both. */
#define PCW_MAX_ANCESTORS 64
#define PCW_MAX_BRANCH 64
/* A raw anchoring tx: generous for an OP_RETURN carrier, still bounded. */
#define PCW_TX_MAX 200000

enum pcw_result {
    PCW_NOT_CHECKED = 0,
    PCW_PASSED,
    PCW_FAILED
};

struct pcw_rung {
    int n;
    const char *id;
    const char *title;
    const char *proves;
    enum pcw_result result;
    char detail[PCW_DETAIL_MAX];
    struct json_value fields; /* object; flattened into the rung row */
};

struct pcw_walk {
    struct pcw_rung rung[PCW_RUNGS];

    /* Carried between rungs. Every `have_*` flag is set ONLY where the
     * corresponding artefact actually verified out; a later rung reads
     * the flag, never the buffer, so nothing downstream can be built on
     * an artefact that failed upstream. */
    bool have_header;
    struct block_header header;

    bool have_tx;
    struct transaction tx;

    bool have_anchor;
    struct zanc_message anchor;

    bool have_doc;
    uint8_t doc_wire[ZID_DOC_MAX];
    size_t doc_wire_len;
    struct zid_doc doc;

    bool have_root;         /* an effective domain root is available */
    uint8_t root[32];
    bool root_from_chain;   /* the root byte-equals the on-chain anchor */
};

/* ── rung plumbing ─────────────────────────────────────────────────── */

static const char *pcw_result_name(enum pcw_result r)
{
    switch (r) {
    case PCW_PASSED:      return "passed";
    case PCW_FAILED:      return "failed";
    case PCW_NOT_CHECKED: break;
    }
    return "not_checked";
}

static const char *pcw_result_tag(enum pcw_result r)
{
    switch (r) {
    case PCW_PASSED:      return "PASS   ";
    case PCW_FAILED:      return "FAIL   ";
    case PCW_NOT_CHECKED: break;
    }
    return "-- SKIP";
}

__attribute__((format(printf, 3, 4)))
static void pcw_set(struct pcw_rung *r, enum pcw_result result,
                    const char *fmt, ...)
{
    r->result = result;
    va_list ap;
    va_start(ap, fmt);
    (void)vsnprintf(r->detail, sizeof(r->detail), fmt, ap);
    va_end(ap);
}

static void pcw_field_str(struct pcw_rung *r, const char *key, const char *v)
{
    (void)json_push_kv_str(&r->fields, key, v);
}

static void pcw_field_int(struct pcw_rung *r, const char *key, int64_t v)
{
    (void)json_push_kv_int(&r->fields, key, v);
}

static void pcw_field_hash(struct pcw_rung *r, const char *key,
                           const uint8_t *bytes, size_t len)
{
    char hex[2 * 64 + 1];
    if (len > 64)
        len = 64;
    HexStr(bytes, len, false, hex, sizeof(hex));
    (void)json_push_kv_str(&r->fields, key, hex);
}

/* uint256 is stored little-endian internally; uint256_get_hex renders the
 * usual big-endian display form (what block explorers and the RPCs show). */
static void pcw_field_uint256(struct pcw_rung *r, const char *key,
                              const struct uint256 *h)
{
    char hex[65];
    uint256_get_hex(h, hex);
    (void)json_push_kv_str(&r->fields, key, hex);
}

static void pcw_walk_init(struct pcw_walk *w)
{
    memset(w, 0, sizeof(*w));
    transaction_init(&w->tx);
    static const struct {
        const char *id;
        const char *title;
        const char *proves;
    } spec[PCW_RUNGS] = {
        { "header_pow", "header + proof-of-work",
          "work was done on this exact header (and any supplied ancestors "
          "chain into it) — NOT that it is the most-work chain" },
        { "tx_inclusion", "anchor tx is in that block",
          "the anchoring transaction is committed by the header's merkle "
          "root, so it is bound to the work proven at rung 1" },
        { "anchor_decode", "OP_RETURN parses as ZANC",
          "the transaction really carries a well-formed anchor and yields "
          "the digest that was committed on-chain" },
        { "domain_root", "domain root matches the anchored digest",
          "the root rung 5 verifies against came off the chain, not from "
          "whoever handed you the record" },
        { "record_proof", "record inclusion proof",
          "this exact record is a leaf of the tree that produced the "
          "anchored root (zid_tree_verify — the frozen verifier)" },
        { "signature", "zid signature over the document",
          "the document was authored by the holder of master_pubkey and is "
          "not expired at the given clock" },
        { "identity_anchor", "master key is anchored and active",
          "the signing key itself is registered on-chain and has not been "
          "revoked or superseded" },
    };
    for (int i = 0; i < PCW_RUNGS; i++) {
        w->rung[i].n = i + 1;
        w->rung[i].id = spec[i].id;
        w->rung[i].title = spec[i].title;
        w->rung[i].proves = spec[i].proves;
        w->rung[i].result = PCW_NOT_CHECKED;
        (void)snprintf(w->rung[i].detail, sizeof(w->rung[i].detail),
                       "not checked");
        json_init(&w->rung[i].fields);
        json_set_object(&w->rung[i].fields);
    }
}

static void pcw_walk_free(struct pcw_walk *w)
{
    for (int i = 0; i < PCW_RUNGS; i++)
        json_free(&w->rung[i].fields);
    transaction_free(&w->tx);
}

/* ── input helpers ─────────────────────────────────────────────────── */

static const char *pcw_input_str(const struct json_value *input,
                                 const char *key)
{
    const struct json_value *v = json_get(input, key);
    if (!v || v->type != JSON_STR)
        return NULL;
    const char *s = json_get_str(v);
    return (s && s[0]) ? s : NULL;
}

/* Decode an even-length hex string into `out`. Returns the byte count, or
 * 0 on any malformation (odd length, non-hex, oversize, empty). */
static size_t pcw_unhex(const char *hex, uint8_t *out, size_t out_len)
{
    if (!hex || !hex[0])
        return 0;
    size_t n = strlen(hex);
    if ((n & 1u) != 0 || n / 2 > out_len || !IsHex(hex))
        return 0;
    return (size_t)ParseHex(hex, out, out_len);
}

/* ── rung 1: header + proof-of-work ────────────────────────────────── */

static bool pcw_decode_header(const char *hex, struct block_header *out)
{
    uint8_t buf[BLOCK_HEADER_SIZE + 3 + MAX_SOLUTION_SIZE];
    size_t len = pcw_unhex(hex, buf, sizeof(buf));
    if (len == 0)
        return false;
    struct byte_stream s;
    stream_init_from_data(&s, buf, len);
    block_header_init(out);
    bool ok = block_header_deserialize(out, &s);
    /* Trailing bytes mean the caller pasted something that is not exactly
     * one header; refuse rather than verify a prefix. */
    if (ok && stream_remaining(&s) != 0)
        ok = false;
    stream_free(&s);
    return ok;
}

/* Both PoW halves for one header: a real Equihash witness AND a hash at or
 * under the nBits target. They are separate checks in consensus and stay
 * separate here so the detail can name which half failed. */
static bool pcw_header_pow_ok(const struct block_header *h,
                              const struct chain_params *cp,
                              struct uint256 *hash_out, const char **why)
{
    block_header_get_hash(h, hash_out);
    if (!check_equihash_solution(h, cp)) {
        *why = "the Equihash solution is not a valid witness for this header";
        return false;
    }
    if (!CheckProofOfWork(*hash_out, h->nBits, &cp->consensus)) {
        *why = "the block hash is above the target encoded by nBits (or "
               "nBits is out of range)";
        return false;
    }
    *why = NULL;
    return true;
}

static void pcw_rung1_header_pow(struct pcw_walk *w,
                                 const struct json_value *input)
{
    struct pcw_rung *r = &w->rung[0];
    const char *hdr_hex = pcw_input_str(input, "header");
    if (!hdr_hex) {
        pcw_set(r, PCW_NOT_CHECKED,
                "no `header` supplied — pass the anchoring block's raw "
                "header hex to check the work behind the anchor");
        return;
    }

    const struct chain_params *cp = chain_params_get();
    if (!cp) {
        pcw_set(r, PCW_NOT_CHECKED,
                "chain parameters are not selected in this process, so "
                "there is no target or Equihash (N,K) to check against");
        return;
    }

    if (!pcw_decode_header(hdr_hex, &w->header)) {
        pcw_set(r, PCW_FAILED,
                "`header` is not exactly one well-formed block header "
                "(wire layout, solution length, or trailing bytes)");
        return;
    }

    struct uint256 hash;
    const char *why = NULL;
    bool ok = pcw_header_pow_ok(&w->header, cp, &hash, &why);
    pcw_field_uint256(r, "block_hash", &hash);
    pcw_field_int(r, "nbits", (int64_t)w->header.nBits);
    pcw_field_int(r, "ntime", (int64_t)w->header.nTime);
    pcw_field_int(r, "solution_size", (int64_t)w->header.nSolutionSize);
    /* The header decoded, so rung 2 has a merkle root to compare against
     * even when the work itself is bad — the rungs stay independent. */
    w->have_header = true;
    if (!ok) {
        pcw_field_int(r, "headers_linked", 0);
        pcw_set(r, PCW_FAILED, "%s", why);
        return;
    }

    /* Optional ancestors, oldest-first, each linking into the next and the
     * last into `header`. Every ancestor must itself carry valid work. */
    const struct json_value *anc = json_get(input, "headers");
    int linked = 0;
    if (anc && anc->type == JSON_ARR) {
        size_t n = json_size(anc);
        if (n > PCW_MAX_ANCESTORS) {
            pcw_field_int(r, "headers_linked", 0);
            pcw_set(r, PCW_FAILED,
                    "`headers` carries %zu ancestors, over the %d cap",
                    n, PCW_MAX_ANCESTORS);
            return;
        }
        struct uint256 prev_hash;
        uint256_set_null(&prev_hash);
        for (size_t i = 0; i < n; i++) {
            const struct json_value *e = json_at(anc, i);
            const char *ehex = (e && e->type == JSON_STR) ? json_get_str(e)
                                                          : NULL;
            struct block_header ah;
            if (!ehex || !pcw_decode_header(ehex, &ah)) {
                pcw_field_int(r, "headers_linked", linked);
                pcw_set(r, PCW_FAILED,
                        "ancestor header %zu (oldest-first) is not a "
                        "well-formed header", i);
                return;
            }
            struct uint256 ahash;
            const char *awhy = NULL;
            if (!pcw_header_pow_ok(&ah, cp, &ahash, &awhy)) {
                pcw_field_int(r, "headers_linked", linked);
                pcw_set(r, PCW_FAILED, "ancestor header %zu: %s", i, awhy);
                return;
            }
            if (i > 0 && memcmp(ah.hashPrevBlock.data, prev_hash.data,
                                32) != 0) {
                pcw_field_int(r, "headers_linked", linked);
                pcw_set(r, PCW_FAILED,
                        "ancestor header %zu does not build on ancestor "
                        "%zu — the supplied chain is broken", i, i - 1);
                return;
            }
            prev_hash = ahash;
            linked++;
        }
        if (n > 0 && memcmp(w->header.hashPrevBlock.data, prev_hash.data,
                            32) != 0) {
            pcw_field_int(r, "headers_linked", linked);
            pcw_set(r, PCW_FAILED,
                    "the anchoring header does not build on the newest "
                    "supplied ancestor — the supplied chain is broken");
            return;
        }
    }

    pcw_field_int(r, "headers_linked", linked);
    if (linked > 0)
        pcw_set(r, PCW_PASSED,
                "valid Equihash witness, hash <= target, and %d supplied "
                "ancestor header(s) chain into it", linked);
    else
        pcw_set(r, PCW_PASSED,
                "valid Equihash witness and hash <= target; no ancestor "
                "headers were supplied, so only this header is proven");
}

/* ── rung 2: tx inclusion (merkle path) ────────────────────────────── */

static void pcw_rung2_tx_inclusion(struct pcw_walk *w,
                                   const struct json_value *input)
{
    struct pcw_rung *r = &w->rung[1];
    const char *tx_hex = pcw_input_str(input, "tx");
    if (!tx_hex) {
        pcw_set(r, PCW_NOT_CHECKED,
                "no `tx` supplied — pass the raw anchoring transaction hex "
                "to check it is committed by the block");
        return;
    }

    uint8_t *buf = zcl_malloc(PCW_TX_MAX, "proof_chain.tx");
    if (!buf) {
        pcw_set(r, PCW_NOT_CHECKED,
                "out of memory decoding the anchoring transaction");
        LOG_ERROR(PCW_LOG, "tx buffer allocation failed (%d bytes)",
                  PCW_TX_MAX);
        return;
    }
    size_t len = pcw_unhex(tx_hex, buf, PCW_TX_MAX);
    if (len == 0) {
        free(buf);
        pcw_set(r, PCW_FAILED,
                "`tx` is not even-length hex within the %d-byte cap",
                PCW_TX_MAX);
        return;
    }
    struct byte_stream s;
    stream_init_from_data(&s, buf, len);
    bool ok = transaction_deserialize(&w->tx, &s);
    if (ok && stream_remaining(&s) != 0)
        ok = false;
    stream_free(&s);
    free(buf);
    if (!ok) {
        pcw_set(r, PCW_FAILED,
                "`tx` is not exactly one well-formed transaction (wire "
                "layout or trailing bytes)");
        return;
    }
    w->have_tx = true;
    pcw_field_uint256(r, "txid", &w->tx.hash);

    if (!w->have_header) {
        pcw_set(r, PCW_NOT_CHECKED,
                "the block header did not decode at rung 1, so there is no "
                "merkle root to check inclusion against");
        return;
    }
    pcw_field_uint256(r, "merkle_root", &w->header.hashMerkleRoot);

    int64_t index = 0;
    const struct json_value *iv = json_get(input, "merkle_index");
    if (iv && iv->type == JSON_INT)
        index = json_get_int(iv);
    if (index < 0) {
        pcw_set(r, PCW_FAILED, "`merkle_index` is negative (%lld)",
                (long long)index);
        return;
    }
    pcw_field_int(r, "merkle_index", index);

    uint8_t sib[PCW_MAX_BRANCH][32];
    size_t branch_len = 0;
    const char *branch_hex = json_get_str(json_get(input, "merkle_branch"));
    if (branch_hex && branch_hex[0]) {
        uint8_t raw[PCW_MAX_BRANCH * 32];
        size_t n = pcw_unhex(branch_hex, raw, sizeof(raw));
        if (n == 0 || (n % 32) != 0) {
            pcw_set(r, PCW_FAILED,
                    "`merkle_branch` must be a multiple of 32 bytes of hex "
                    "(at most %d siblings)", PCW_MAX_BRANCH);
            return;
        }
        branch_len = n / 32;
        memcpy(sib, raw, n);
    }
    pcw_field_int(r, "branch_len", (int64_t)branch_len);

    /* A branch of length k can only address 2^k leaf positions. */
    if (branch_len < 63 && index >= ((int64_t)1 << branch_len)) {
        pcw_set(r, PCW_FAILED,
                "merkle_index %lld is outside the %lld positions a "
                "%zu-sibling branch can address",
                (long long)index, (long long)((int64_t)1 << branch_len),
                branch_len);
        return;
    }

    struct uint256 acc = w->tx.hash;
    for (size_t i = 0; i < branch_len; i++) {
        struct uint256 other;
        memcpy(other.data, sib[i], 32);
        struct uint256 next;
        if (((index >> (int)i) & 1) == 0)
            merkle_hash_pair(&acc, &other, &next);
        else
            merkle_hash_pair(&other, &acc, &next);
        acc = next;
    }
    pcw_field_uint256(r, "computed_root", &acc);

    if (memcmp(acc.data, w->header.hashMerkleRoot.data, 32) != 0) {
        pcw_set(r, PCW_FAILED,
                "the merkle path folds to a different root than the "
                "header commits — wrong branch, wrong index, or wrong "
                "block for this transaction");
        return;
    }
    pcw_set(r, PCW_PASSED,
            "the transaction folds to the header's merkle root through "
            "%zu sibling(s) at position %lld",
            branch_len, (long long)index);
}

/* ── rung 3: anchor decode (first OP_RETURN parses as ZANC) ────────── */

static void pcw_rung3_anchor_decode(struct pcw_walk *w)
{
    struct pcw_rung *r = &w->rung[2];
    if (!w->have_tx) {
        pcw_set(r, PCW_NOT_CHECKED,
                "the anchoring transaction is absent or did not decode at "
                "rung 2, so there is no output to parse");
        return;
    }

    /* ZANC is defined as the tx's FIRST OP_RETURN output — scanning past
     * it would let a later output override the anchor. */
    size_t vout = 0;
    bool found = false;
    for (size_t i = 0; i < w->tx.num_vout; i++) {
        const struct script *sp = &w->tx.vout[i].script_pub_key;
        if (sp->size > 0 && sp->data[0] == OP_RETURN) {
            vout = i;
            found = true;
            break;
        }
    }
    if (!found) {
        pcw_set(r, PCW_FAILED,
                "the transaction has no OP_RETURN output, so it anchors "
                "nothing (%zu output(s) scanned)", w->tx.num_vout);
        return;
    }
    pcw_field_int(r, "vout", (int64_t)vout);

    const struct script *sp = &w->tx.vout[vout].script_pub_key;
    if (!zanc_parse(sp->data, sp->size, &w->anchor)) {
        pcw_set(r, PCW_FAILED,
                "the first OP_RETURN (vout %zu) is not a well-formed ZANC "
                "anchor — lokad id, version, hash type, label, or trailing "
                "bytes", vout);
        return;
    }
    w->have_anchor = true;
    pcw_field_hash(r, "anchored_digest", w->anchor.digest, ZANC_DIGEST_LEN);
    pcw_field_int(r, "zanc_version", (int64_t)w->anchor.version);
    pcw_field_str(r, "hash_type", zanc_hash_type_name(w->anchor.hash_type));
    pcw_field_str(r, "label", w->anchor.label);
    pcw_set(r, PCW_PASSED,
            "vout %zu is a ZANC v%u anchor over a %s digest, label \"%s\"",
            vout, (unsigned)w->anchor.version,
            zanc_hash_type_name(w->anchor.hash_type), w->anchor.label);
}

/* ── rung 4: the supplied root IS the anchored digest ──────────────── */

static void pcw_rung4_domain_root(struct pcw_walk *w,
                                  const struct json_value *input)
{
    struct pcw_rung *r = &w->rung[3];
    const char *root_hex = pcw_input_str(input, "root");
    uint8_t supplied[32];
    bool have_supplied = false;
    if (root_hex) {
        if (strlen(root_hex) != 64 || pcw_unhex(root_hex, supplied, 32) != 32) {
            pcw_set(r, PCW_FAILED,
                    "`root` must be exactly 64 hex characters (a 32-byte "
                    "domain root)");
            return;
        }
        have_supplied = true;
        pcw_field_hash(r, "supplied_root", supplied, 32);
        /* Available to rung 5 even if it does not match the chain — rung 5
         * then says so in root_source rather than silently skipping. */
        memcpy(w->root, supplied, 32);
        w->have_root = true;
    }

    if (!w->have_anchor) {
        pcw_set(r, PCW_NOT_CHECKED,
                "no on-chain anchor was decoded at rung 3, so the supplied "
                "root cannot be tied to anything the chain committed");
        return;
    }
    pcw_field_hash(r, "anchored_digest", w->anchor.digest, ZANC_DIGEST_LEN);

    /* A zid domain root is SHA3-256. A SHA2 anchor over the same bytes is
     * a different commitment and must not be accepted as one. */
    if (w->anchor.hash_type != ZANC_HASH_SHA3_256) {
        pcw_set(r, PCW_FAILED,
                "the anchor commits a %s digest, but a zid domain root is "
                "SHA3-256 — this anchor is not a domain root",
                zanc_hash_type_name(w->anchor.hash_type));
        return;
    }

    if (!have_supplied) {
        /* Nothing to contradict the chain: adopt the anchored digest as
         * the root rung 5 verifies against. */
        memcpy(w->root, w->anchor.digest, 32);
        w->have_root = true;
        w->root_from_chain = true;
        pcw_set(r, PCW_PASSED,
                "no `root` was supplied, so the on-chain anchored SHA3-256 "
                "digest is used as the domain root");
        return;
    }

    if (memcmp(supplied, w->anchor.digest, 32) != 0) {
        pcw_set(r, PCW_FAILED,
                "the supplied root is NOT the digest this transaction "
                "anchored — stale root, wrong batch, or a root that was "
                "never put on-chain");
        return;
    }
    w->root_from_chain = true;
    pcw_set(r, PCW_PASSED,
            "the supplied domain root byte-equals the SHA3-256 digest "
            "anchored on-chain by this transaction");
}

/* ── rung 5: record inclusion proof (zid_tree_verify) ──────────────── */

static void pcw_rung5_record_proof(struct pcw_walk *w,
                                   const struct json_value *input)
{
    struct pcw_rung *r = &w->rung[4];

    /* Stated on every outcome: whether the root this rung verified against
     * came off the chain or from whoever handed you the record. */
    pcw_field_str(r, "root_source",
                  w->root_from_chain
                      ? "on-chain anchored digest (confirmed at rung 4)"
                      : "supplied --root, NOT confirmed against the "
                        "on-chain anchor (see rung 4)");

    if (!w->have_doc) {
        pcw_set(r, PCW_NOT_CHECKED,
                "no document, so there is no record digest to prove "
                "inclusion of");
        return;
    }
    uint8_t rec[32];
    zid_record_digest(rec, w->doc_wire, w->doc_wire_len);
    pcw_field_hash(r, "record_digest", rec, 32);

    const char *proof_hex = pcw_input_str(input, "proof");
    if (!proof_hex) {
        pcw_set(r, PCW_NOT_CHECKED,
                "no `proof` supplied — pass the zid_proof wire hex from "
                "`zcode release prove` to check batch inclusion");
        return;
    }
    if (!w->have_root) {
        pcw_set(r, PCW_NOT_CHECKED,
                "no domain root is available (none supplied and none "
                "decoded from the chain at rungs 3-4), so a proof cannot "
                "be checked against anything");
        return;
    }
    pcw_field_hash(r, "root", w->root, 32);

    uint8_t wire[ZID_PROOF_WIRE_MAX];
    size_t wire_len = pcw_unhex(proof_hex, wire, sizeof(wire));
    if (wire_len == 0) {
        pcw_set(r, PCW_FAILED,
                "`proof` is not even-length hex within the %d-byte "
                "zid_proof cap", (int)ZID_PROOF_WIRE_MAX);
        return;
    }
    /* Pre-reads for a precise detail ONLY — zid_proof_decode below is the
     * authority and re-checks all of this. Evidence that was supplied but
     * is unusable is a FAILURE, never a skip. */
    if (wire[0] != ZID_PROOF_VERSION) {
        pcw_field_int(r, "proof_version", (int64_t)wire[0]);
        pcw_set(r, PCW_FAILED,
                "proof wire version %u is unknown (this verifier pins "
                "version %d) — an unknown version is rejected, never "
                "skipped", (unsigned)wire[0], ZID_PROOF_VERSION);
        return;
    }
    if (wire_len < 19) {
        pcw_set(r, PCW_FAILED,
                "proof wire is %zu bytes — truncated below the 19-byte "
                "fixed header (version, index, num_leaves, proof_len)",
                wire_len);
        return;
    }

    uint64_t index = 0, num_leaves = 0;
    uint8_t sibs[ZID_TREE_MAX_PEAKS][32];
    uint32_t proof_len = 0;
    if (!zid_proof_decode(&index, &num_leaves, sibs, &proof_len, wire,
                          wire_len)) {
        uint16_t claimed = (uint16_t)(wire[17] | ((uint16_t)wire[18] << 8));
        pcw_set(r, PCW_FAILED,
                "proof wire is %zu bytes but declares %u siblings "
                "(expected %zu bytes) — truncated or malformed",
                wire_len, (unsigned)claimed,
                (size_t)19 + (size_t)32 * claimed);
        return;
    }
    pcw_field_int(r, "index", (int64_t)index);
    pcw_field_int(r, "num_leaves", (int64_t)num_leaves);
    pcw_field_int(r, "proof_len", (int64_t)proof_len);
    if (index >= num_leaves) {
        pcw_set(r, PCW_FAILED,
                "the proof claims leaf %llu of a tree with only %llu "
                "leaves — no such leaf exists",
                (unsigned long long)index,
                (unsigned long long)num_leaves);
        return;
    }

    /* THE frozen verifier. Never reimplemented, never second-guessed. */
    if (!zid_tree_verify(w->root, rec, index, num_leaves,
                         (const uint8_t (*)[32])sibs, proof_len)) {
        pcw_set(r, PCW_FAILED,
                "zid_tree_verify rejects this proof: the record digest is "
                "not leaf %llu of the tree that produced this root",
                (unsigned long long)index);
        return;
    }
    pcw_set(r, PCW_PASSED,
            "zid_tree_verify accepts: the record is leaf %llu of %llu in "
            "the tree that produced this root",
            (unsigned long long)index, (unsigned long long)num_leaves);
}

/* ── rung 6: zid signature over the document ───────────────────────── */

static void pcw_rung6_signature(struct pcw_walk *w, int64_t now)
{
    struct pcw_rung *r = &w->rung[5];
    if (!w->have_doc) {
        pcw_set(r, PCW_NOT_CHECKED,
                "no document decoded, so there is no signature to verify");
        return;
    }
    pcw_field_hash(r, "master_pubkey", w->doc.master_pubkey, 32);
    pcw_field_int(r, "seq", (int64_t)w->doc.seq);
    pcw_field_int(r, "expiry", (int64_t)w->doc.expiry);
    pcw_field_int(r, "now", now);

    if (zid_doc_verify(&w->doc, (uint64_t)now)) {
        pcw_set(r, PCW_PASSED,
                "the ed25519 signature verifies against the document's "
                "master_pubkey and the document is unexpired at now=%lld",
                (long long)now);
        return;
    }
    /* Disambiguate the way zcode.release.verify already does: expiry is a
     * clock verdict, a bad signature is a forgery verdict. */
    if ((uint64_t)now >= w->doc.expiry)
        pcw_set(r, PCW_FAILED,
                "the document is expired: expiry=%llu, now=%lld — ask the "
                "publisher for a re-signed document with a higher seq",
                (unsigned long long)w->doc.expiry, (long long)now);
    else
        pcw_set(r, PCW_FAILED,
                "the ed25519 signature does not verify against the "
                "document's own master_pubkey — the document was tampered "
                "with or corrupted");
}

/* ── rung 7: identity anchor ───────────────────────────────────────── */

static void pcw_rung7_identity_anchor(struct pcw_walk *w)
{
    struct pcw_rung *r = &w->rung[6];

    /* ── SEAM: rung 7 (identity anchor) — DO NOT CLOSE HERE ───────────
     * The one and only call site for the chain-anchored identity lookup.
     * It does NOT exist in this tree: db_zid_identity_find() — master key
     * -> anchor txid/height/status over the on-chain ZID anchor index — is
     * being built in a separate lane and lands at merge. Until it does,
     * this rung is not_checked with that reason.
     * NEVER return `passed` here, and NEVER infer it from rungs 1-6: a
     * valid signature proves the document came from that key, not that the
     * key is anchored on-chain or still active. */
    pcw_field_str(r, "needs", "db_zid_identity_find");
    if (w->have_doc)
        pcw_field_hash(r, "master_pubkey", w->doc.master_pubkey, 32);
    pcw_set(r, PCW_NOT_CHECKED,
            "needs the chain-anchored identity lookup "
            "(db_zid_identity_find), which is not present in this build; a "
            "valid signature at rung 6 proves authorship by that key, "
            "never that the key is anchored on-chain or still active");
}

/* ── document intake ───────────────────────────────────────────────── */

/* Load the document wire bytes from `doc` hex or `doc_file`. Fills
 * why/why_size on any failure and returns false. */
static bool pcw_load_doc(const struct json_value *input, struct pcw_walk *w,
                         char *why, size_t why_size)
{
    const char *doc_hex = pcw_input_str(input, "doc");
    const char *doc_file = pcw_input_str(input, "doc_file");
    char file_hex[ZID_DOC_MAX * 2 + 2];

    if (!doc_hex) {
        if (!doc_file) {
            (void)snprintf(why, why_size,
                           "give --doc=<hex> (a zid document, e.g. from "
                           "`zcode release sign`) or --doc_file=<path to a "
                           "saved .zid>");
            return false;
        }
        int fd = open(doc_file, O_RDONLY | O_CLOEXEC);
        if (fd < 0) {
            (void)snprintf(why, why_size, "cannot open doc_file: %s",
                           strerror(errno));
            return false;
        }
        ssize_t n = read(fd, file_hex, sizeof(file_hex) - 1);
        close(fd);
        if (n <= 0) {
            (void)snprintf(why, why_size, "doc_file is empty or unreadable");
            return false;
        }
        while (n > 0 && (file_hex[n - 1] == '\n' || file_hex[n - 1] == '\r' ||
                         file_hex[n - 1] == ' ' || file_hex[n - 1] == '\t'))
            n--;
        file_hex[n] = '\0';
        doc_hex = file_hex;
    }

    size_t len = pcw_unhex(doc_hex, w->doc_wire, sizeof(w->doc_wire));
    if (len == 0) {
        (void)snprintf(why, why_size,
                       "the document must be even-length hex, at most %d "
                       "bytes", (int)ZID_DOC_MAX);
        return false;
    }
    if (!zid_doc_decode(&w->doc, w->doc_wire, len)) {
        (void)snprintf(why, why_size,
                       "not a well-formed zid document (version byte or "
                       "wire layout) — check the hex was not truncated");
        return false;
    }
    w->doc_wire_len = len;
    w->have_doc = true;
    return true;
}

/* ── report assembly ───────────────────────────────────────────────── */

static void pcw_emit(const struct pcw_walk *w, struct json_value *data)
{
    int passed = 0, failed = 0, skipped = 0;
    int prefix = 0;
    bool prefix_open = true;
    /* `first_break` names the first rung that actually FAILED — a rung
     * that was merely not checked is a gap, not a break, and calling it
     * one would blame the wrong link. `first_gap` is the first rung that
     * did not pass for any reason, which is exactly where the verified
     * prefix stops. */
    int first_break = 0;
    int first_gap = 0;
    for (int i = 0; i < PCW_RUNGS; i++) {
        switch (w->rung[i].result) {
        case PCW_PASSED:      passed++;  break;
        case PCW_FAILED:      failed++;  break;
        case PCW_NOT_CHECKED: skipped++; break;
        }
        if (w->rung[i].result == PCW_FAILED && first_break == 0)
            first_break = w->rung[i].n;
        if (prefix_open) {
            if (w->rung[i].result == PCW_PASSED) {
                prefix++;
            } else {
                prefix_open = false;
                first_gap = w->rung[i].n;
            }
        }
    }

    (void)json_push_kv_bool(data, "node_free", true);
    (void)json_push_kv_bool(data, "chain_complete", passed == PCW_RUNGS);
    (void)json_push_kv_str(data, "chain_complete_reason",
                           passed == PCW_RUNGS
                               ? "every rung passed"
                               : "not every rung passed — `first_break` names "
                                 "the first rung that FAILED (null if none) "
                                 "and `first_gap` the first that was not "
                                 "checked; rung 7 (identity_anchor) is never "
                                 "checked in this build, so this can never be "
                                 "true here");

    char verdict[192];
    if (failed > 0)
        (void)snprintf(verdict, sizeof(verdict),
                       "BROKEN — rung %d (%s) failed; %d passed, %d failed, "
                       "%d not checked",
                       first_break, w->rung[first_break - 1].id, passed,
                       failed, skipped);
    else
        (void)snprintf(verdict, sizeof(verdict),
                       "INCOMPLETE — %d/%d rungs passed, %d not checked "
                       "(nothing failed)",
                       passed, PCW_RUNGS, skipped);
    (void)json_push_kv_str(data, "verdict", verdict);

    (void)json_push_kv_int(data, "verified_prefix", prefix);
    char plabel[128];
    if (prefix == 0)
        (void)snprintf(plabel, sizeof(plabel),
                       "none — rung 1 (%s) did not pass", w->rung[0].id);
    else
        (void)snprintf(plabel, sizeof(plabel), "rungs 1-%d (%s .. %s)",
                       prefix, w->rung[0].id, w->rung[prefix - 1].id);
    (void)json_push_kv_str(data, "verified_prefix_label", plabel);
    (void)json_push_kv_str(data, "verified_prefix_meaning",
                           "consecutive passing rungs from 1; a later rung "
                           "reports its own honest verdict but can never "
                           "lift this past an earlier break");

    if (first_break > 0) {
        (void)json_push_kv_int(data, "first_break", first_break);
        (void)json_push_kv_str(data, "first_break_id",
                               w->rung[first_break - 1].id);
    } else {
        struct json_value nul;
        json_init(&nul);
        json_set_null(&nul);
        (void)json_push_kv(data, "first_break", &nul);
        json_free(&nul);
    }
    if (first_gap > 0) {
        (void)json_push_kv_int(data, "first_gap", first_gap);
        (void)json_push_kv_str(data, "first_gap_id",
                               w->rung[first_gap - 1].id);
        (void)json_push_kv_str(data, "first_gap_detail",
                               w->rung[first_gap - 1].detail);
    }

    struct json_value summary;
    json_init(&summary);
    json_set_object(&summary);
    (void)json_push_kv_int(&summary, "passed", passed);
    (void)json_push_kv_int(&summary, "failed", failed);
    (void)json_push_kv_int(&summary, "not_checked", skipped);
    (void)json_push_kv(data, "summary", &summary);
    json_free(&summary);

    struct json_value report;
    json_init(&report);
    json_set_array(&report);
    struct json_value rungs;
    json_init(&rungs);
    json_set_array(&rungs);

    for (int i = 0; i < PCW_RUNGS; i++) {
        const struct pcw_rung *r = &w->rung[i];
        char line[PCW_DETAIL_MAX + 80];
        (void)snprintf(line, sizeof(line), "%s %d/%d %-15s %s",
                       pcw_result_tag(r->result), r->n, PCW_RUNGS, r->id,
                       r->detail);
        struct json_value lv;
        json_init(&lv);
        json_set_str(&lv, line);
        (void)json_push_back(&report, &lv);
        json_free(&lv);

        struct json_value row;
        json_init(&row);
        json_set_object(&row);
        (void)json_push_kv_int(&row, "n", r->n);
        (void)json_push_kv_str(&row, "id", r->id);
        (void)json_push_kv_str(&row, "title", r->title);
        (void)json_push_kv_str(&row, "result", pcw_result_name(r->result));
        (void)json_push_kv_str(&row, "detail", r->detail);
        /* `reason` exists ONLY on not_checked rows: a passing rung never
         * carries one, so the two can never be confused. */
        if (r->result == PCW_NOT_CHECKED)
            (void)json_push_kv_str(&row, "reason", r->detail);
        (void)json_push_kv_str(&row, "proves", r->proves);
        for (size_t k = 0; k < r->fields.num_children; k++)
            (void)json_push_kv(&row, r->fields.keys[k],
                               &r->fields.children[k]);
        (void)json_push_back(&rungs, &row);
        json_free(&row);
    }
    (void)json_push_kv(data, "report", &report);
    json_free(&report);
    (void)json_push_kv(data, "rungs", &rungs);
    json_free(&rungs);
}

/* ── zcode.proof.walk ──────────────────────────────────────────────── */

void zcl_native_handle_proof_chain_walk(
    const struct zcl_command_request *request,
    struct zcl_command_reply *reply)
{
    if (!request || !reply)
        return;

    struct pcw_walk w;
    pcw_walk_init(&w);

    char why[256] = {0};
    if (!pcw_load_doc(request->input, &w, why, sizeof(why))) {
        /* The ONLY failure path: with no document there is no chain to
         * walk and therefore no report to lose to the envelope's
         * data-drop-on-error rule. */
        pcw_walk_free(&w);
        zcl_command_reply_fail(reply, ZCL_COMMAND_STATUS_FAILED,
                               ZCL_COMMAND_EXIT_INVALID, "NO_DOCUMENT",
                               "normalize", false, false, why,
                               "zcode.proof.walk");
        return;
    }

    int64_t now = platform_time_wall_unix();
    const struct json_value *nv = json_get(request->input, "now");
    if (nv && nv->type == JSON_INT)
        now = json_get_int(nv);

    pcw_rung1_header_pow(&w, request->input);
    pcw_rung2_tx_inclusion(&w, request->input);
    pcw_rung3_anchor_decode(&w);
    pcw_rung4_domain_root(&w, request->input);
    pcw_rung5_record_proof(&w, request->input);
    pcw_rung6_signature(&w, now);
    pcw_rung7_identity_anchor(&w);

    pcw_emit(&w, &reply->data);
    pcw_walk_free(&w);

    reply->status = ZCL_COMMAND_STATUS_PASSED;
    reply->exit_code = ZCL_COMMAND_EXIT_OK;
}

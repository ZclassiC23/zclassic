/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_proof_chain — the light-client proof-chain walker
 * (tools/command/native_proof_chain_command.c, `zcode proof walk`).
 *
 * WHAT THIS GUARDS. The walker's product is the seven-rung report, and
 * the only way that report can lie is by reporting a rung as passing when
 * it was skipped, or by letting a later rung's pass paper over an earlier
 * break. So every case below breaks exactly ONE rung and asserts three
 * things at once:
 *
 *   1. that rung reports `failed` (or `not_checked`, where evidence was
 *      withheld rather than supplied-and-broken),
 *   2. every OTHER rung still reports its own honest, independent verdict
 *      — a broken rung 1 does not silently poison rung 2, and
 *   3. `verified_prefix` — the count of CONSECUTIVE passing rungs from 1
 *      — stops at the break, so no later pass can lift it.
 *
 * Rung 7 (identity_anchor) is asserted `not_checked` in EVERY case and
 * never `passed`: the chain-anchored identity lookup does not exist in
 * this tree, so `chain_complete` must be false everywhere too.
 *
 * The fixture is hermetic and real: three signed zid release documents
 * folded into a real zid anchor-domain tree, a real ZANC OP_RETURN over
 * the bagged root, a real transaction carrying it, a real merkle path,
 * and two REAL Equihash-mined regtest headers (48,5 solves in << 1 ms).
 * Everything crosses the hex/wire boundary, so the wire path is what is
 * proven, not internal structs. No network, no node, no /tmp files. */

#include "test/test_core.h"

#include "command/native_command.h"

#include "bloom/merkle.h"
#include "chain/chainparams.h"
#include "core/arith_uint256.h"
#include "core/serialize.h"
#include "core/uint256.h"
#include "encoding/utilstrencodings.h"
#include "json/json.h"
#include "mining/miner.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "script/script.h"
#include "zanc/zanc.h"
#include "zid/zid.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define PC_CHECK(name, expr) do {                                        \
    if (expr) { printf("  proof_chain: %s... OK\n", (name)); }            \
    else { printf("  proof_chain: %s... FAIL\n", (name)); failures++; }   \
} while (0)

#define PC_LEAVES 3
#define PC_TARGET 1 /* which of the three docs the walk is about */

/* ── fixture ───────────────────────────────────────────────────────── */

enum pc_variant {
    PC_GOOD = 0,
    PC_ANCHOR_OTHER_DIGEST, /* anchor a digest that is NOT the tree root */
    PC_ANCHOR_NOT_ZANC,     /* an OP_RETURN that does not parse as ZANC   */
    PC_ANCHOR_SHA2,         /* a well-formed ZANC anchor over a SHA2 root */
    PC_DOC_BAD_SIGNATURE    /* target doc's signature flipped BEFORE batching */
};

struct pc_fixture {
    char doc_hex[ZID_DOC_MAX * 2 + 2];
    char proof_hex[ZID_PROOF_WIRE_MAX * 2 + 2];
    char root_hex[65];
    char tx_hex[4096];
    char header_hex[4096];
    char ancestor_hex[4096];
    char branch_hex[65];
    uint64_t expiry;
    uint64_t index;
    uint64_t num_leaves;
    /* Dispatch knobs. A case copies the fixture and edits the copy — the
     * input object is built ONCE from it, because json_push_kv appends
     * and json_get returns the FIRST match, so "push again to override"
     * would silently keep the original value. An empty hex string means
     * the key is omitted entirely (evidence withheld). */
    int64_t merkle_index;
    int64_t now;
};

static void pc_hex(const unsigned char *in, size_t len, char *out)
{
    static const char d[] = "0123456789abcdef";
    for (size_t i = 0; i < len; i++) {
        out[2 * i] = d[(in[i] >> 4) & 0xf];
        out[2 * i + 1] = d[in[i] & 0xf];
    }
    out[2 * len] = '\0';
}

/* Serialize a header (or a tx) through the canonical wire codec and hex
 * it into `out`. Returns false if the wire does not fit. */
static bool pc_header_hex(const struct block_header *h, char *out,
                          size_t out_size)
{
    struct byte_stream s;
    stream_init(&s, 512);
    bool ok = block_header_serialize(h, &s);
    if (ok && s.size * 2 + 1 <= out_size)
        pc_hex(s.data, s.size, out);
    else
        ok = false;
    stream_free(&s);
    return ok;
}

static bool pc_tx_hex(const struct transaction *tx, char *out,
                      size_t out_size)
{
    struct byte_stream s;
    stream_init(&s, 512);
    bool ok = transaction_serialize(tx, &s);
    if (ok && s.size * 2 + 1 <= out_size)
        pc_hex(s.data, s.size, out);
    else
        ok = false;
    stream_free(&s);
    return ok;
}

/* Mine a regtest header at `height` on top of prev_hash committing
 * merkle_root. Real Equihash (48,5) + a real nBits target. */
static bool pc_mine(struct block_header *out, int height,
                    const struct uint256 *prev_hash,
                    const struct uint256 *merkle_root,
                    const struct chain_params *cp)
{
    struct block b;
    block_init(&b);
    b.header.hashPrevBlock = *prev_hash;
    b.header.hashMerkleRoot = *merkle_root;
    uint256_set_null(&b.header.hashFinalSaplingRoot);
    b.header.nTime = 1600000000u + (uint32_t)height;

    struct arith_uint256 pow_limit;
    uint256_to_arith(&pow_limit, &cp->consensus.powLimit);
    b.header.nBits = arith_uint256_get_compact(&pow_limit, false);

    if (!mine_block_pow(&b, height, cp, 1u << 20))
        return false;
    *out = b.header;
    return true;
}

/* Build the anchoring transaction: vout[0] carries `script`, vout[1] is a
 * throwaway spendable output so the OP_RETURN is not the only output. */
static bool pc_build_anchor_tx(struct transaction *tx,
                               const unsigned char *script, size_t len)
{
    transaction_init(tx);
    if (!transaction_alloc(tx, 1, 2))
        return false;
    /* One input with a null-ish prevout and a token scriptSig. */
    memset(tx->vin[0].prevout.hash.data, 0x11, 32);
    tx->vin[0].prevout.n = 0;
    tx->vin[0].script_sig.size = 1;
    tx->vin[0].script_sig.data[0] = 0x51; /* OP_1 */
    tx->vin[0].sequence = 0xffffffffu;

    tx->vout[0].value = 0;
    if (len > MAX_SCRIPT_SIZE)
        return false;
    memcpy(tx->vout[0].script_pub_key.data, script, len);
    tx->vout[0].script_pub_key.size = len;

    tx->vout[1].value = 1000;
    tx->vout[1].script_pub_key.data[0] = 0x51; /* OP_1 */
    tx->vout[1].script_pub_key.size = 1;

    transaction_compute_hash(tx);
    return true;
}

static bool pc_build_fixture(struct pc_fixture *f, enum pc_variant variant)
{
    memset(f, 0, sizeof(*f));
    const struct chain_params *cp = chain_params_get();
    if (!cp)
        return false;

    /* 1. Three signed release documents from fixed seeds. */
    uint8_t wires[PC_LEAVES][ZID_DOC_MAX];
    size_t wire_lens[PC_LEAVES];
    uint8_t digests[PC_LEAVES][32];
    const uint64_t expiry = 4000000000ull;
    for (int i = 0; i < PC_LEAVES; i++) {
        uint8_t seed[32];
        memset(seed, 0x20 + i, 32);
        struct zid_release rel;
        memset(&rel, 0, sizeof(rel));
        snprintf(rel.name, sizeof(rel.name), "pkg%d", i);
        snprintf(rel.version, sizeof(rel.version), "1.%d", i);
        memset(rel.manifest_root, 0x40 + i, 32);
        struct zid_doc doc;
        if (!zid_release_sign(&doc, &rel, (uint64_t)(i + 1), expiry, seed))
            return false;
        if (variant == PC_DOC_BAD_SIGNATURE && i == PC_TARGET)
            doc.signature[7] ^= 0x01; /* forged BEFORE the batch is folded */
        wire_lens[i] = zid_doc_encode(wires[i], sizeof(wires[i]), &doc);
        if (wire_lens[i] == 0)
            return false;
        zid_record_digest(digests[i], wires[i], wire_lens[i]);
    }
    f->expiry = expiry;
    pc_hex(wires[PC_TARGET], wire_lens[PC_TARGET], f->doc_hex);

    /* 2. Canonical batch: digests sorted by bytes, then folded. */
    uint8_t leaves[PC_LEAVES][32];
    memcpy(leaves, digests, sizeof(leaves));
    for (int a = 0; a < PC_LEAVES; a++)
        for (int b = a + 1; b < PC_LEAVES; b++)
            if (memcmp(leaves[a], leaves[b], 32) > 0) {
                uint8_t t[32];
                memcpy(t, leaves[a], 32);
                memcpy(leaves[a], leaves[b], 32);
                memcpy(leaves[b], t, 32);
            }
    uint64_t index = PC_LEAVES;
    for (int i = 0; i < PC_LEAVES; i++)
        if (memcmp(leaves[i], digests[PC_TARGET], 32) == 0)
            index = (uint64_t)i;
    if (index >= PC_LEAVES)
        return false;

    uint8_t sibs[ZID_TREE_MAX_PEAKS][32];
    uint32_t proof_len = 0;
    uint8_t root[32];
    if (!zid_tree_prove_from_leaves((const uint8_t (*)[32])leaves, PC_LEAVES,
                                    index, sibs, &proof_len, root))
        return false;
    f->index = index;
    f->num_leaves = PC_LEAVES;
    pc_hex(root, 32, f->root_hex);

    uint8_t pwire[ZID_PROOF_WIRE_MAX];
    size_t pwire_len = zid_proof_encode(pwire, sizeof(pwire), index,
                                        PC_LEAVES,
                                        (const uint8_t (*)[32])sibs,
                                        proof_len);
    if (pwire_len == 0)
        return false;
    pc_hex(pwire, pwire_len, f->proof_hex);

    /* 3. The on-chain anchor script. */
    unsigned char script[128];
    size_t script_len = 0;
    if (variant == PC_ANCHOR_NOT_ZANC) {
        /* OP_RETURN PUSH4 "ZZZZ" — an OP_RETURN that is not a ZANC anchor. */
        script[0] = 0x6a;
        script[1] = 0x04;
        memcpy(script + 2, "ZZZZ", 4);
        script_len = 6;
    } else if (variant == PC_ANCHOR_OTHER_DIGEST) {
        uint8_t other[32];
        memcpy(other, root, 32);
        other[0] ^= 0xff;
        script_len = zanc_build_anchor(script, sizeof(script),
                                       ZANC_HASH_SHA3_256, other, "zcode@1");
    } else if (variant == PC_ANCHOR_SHA2) {
        script_len = zanc_build_anchor(script, sizeof(script),
                                       ZANC_HASH_SHA2_256, root, "zcode@1");
    } else {
        script_len = zanc_build_anchor(script, sizeof(script),
                                       ZANC_HASH_SHA3_256, root, "zcode@1");
    }
    if (script_len == 0)
        return false;

    /* 4. The transaction carrying it, and the block committing that tx at
     *    merkle position 1 (sibling = a fixed stand-in coinbase txid). */
    struct transaction tx;
    if (!pc_build_anchor_tx(&tx, script, script_len)) {
        transaction_free(&tx);
        return false;
    }
    bool ok = pc_tx_hex(&tx, f->tx_hex, sizeof(f->tx_hex));
    struct uint256 txid = tx.hash;
    transaction_free(&tx);
    if (!ok)
        return false;

    struct uint256 leaves256[2];
    memset(leaves256[0].data, 0xa5, 32); /* stand-in coinbase txid */
    leaves256[1] = txid;
    pc_hex(leaves256[0].data, 32, f->branch_hex);
    struct uint256 merkle_root = compute_merkle_root(leaves256, 2);

    /* 5. Two real mined headers: an ancestor, then the anchoring header
     *    that builds on it. */
    struct uint256 zero;
    uint256_set_null(&zero);
    struct uint256 anc_merkle;
    memset(anc_merkle.data, 0x33, 32);
    struct block_header ancestor, header;
    if (!pc_mine(&ancestor, 1, &zero, &anc_merkle, cp))
        return false;
    struct uint256 anc_hash;
    block_header_get_hash(&ancestor, &anc_hash);
    if (!pc_mine(&header, 2, &anc_hash, &merkle_root, cp))
        return false;
    if (!pc_header_hex(&ancestor, f->ancestor_hex, sizeof(f->ancestor_hex)))
        return false;
    if (!pc_header_hex(&header, f->header_hex, sizeof(f->header_hex)))
        return false;

    f->merkle_index = 1; /* the anchor tx is the second leaf */
    f->now = 1700000000; /* well before the fixture expiry */
    return true;
}

/* ── driving the command ───────────────────────────────────────────── */

struct pc_run {
    struct json_value input;
    struct zcl_command_request request;
    struct zcl_command_reply reply;
};

static void pc_run_init(struct pc_run *r)
{
    json_init(&r->input);
    json_set_object(&r->input);
    memset(&r->request, 0, sizeof(r->request));
    r->request.input = &r->input;
    zcl_command_reply_init(&r->reply, "zcl.zcode_proof_walk.v1");
}

static void pc_run_free(struct pc_run *r)
{
    zcl_command_reply_free(&r->reply);
    json_free(&r->input);
}

/* Build the whole input object from the fixture exactly once. An empty
 * hex field is omitted rather than passed empty. */
static void pc_run_load(struct pc_run *r, const struct pc_fixture *f)
{
    if (f->doc_hex[0])
        (void)json_push_kv_str(&r->input, "doc", f->doc_hex);
    if (f->proof_hex[0])
        (void)json_push_kv_str(&r->input, "proof", f->proof_hex);
    if (f->root_hex[0])
        (void)json_push_kv_str(&r->input, "root", f->root_hex);
    if (f->tx_hex[0])
        (void)json_push_kv_str(&r->input, "tx", f->tx_hex);
    if (f->header_hex[0])
        (void)json_push_kv_str(&r->input, "header", f->header_hex);
    if (f->ancestor_hex[0]) {
        struct json_value anc;
        json_init(&anc);
        json_set_array(&anc);
        struct json_value one;
        json_init(&one);
        json_set_str(&one, f->ancestor_hex);
        (void)json_push_back(&anc, &one);
        json_free(&one);
        (void)json_push_kv(&r->input, "headers", &anc);
        json_free(&anc);
    }
    if (f->branch_hex[0])
        (void)json_push_kv_str(&r->input, "merkle_branch", f->branch_hex);
    (void)json_push_kv_int(&r->input, "merkle_index", f->merkle_index);
    (void)json_push_kv_int(&r->input, "now", f->now);
}

/* The rung row for 1-based rung `n`, or NULL. */
static const struct json_value *pc_rung(const struct zcl_command_reply *rep,
                                        int n)
{
    const struct json_value *rungs = json_get(&rep->data, "rungs");
    if (!rungs || rungs->type != JSON_ARR)
        return NULL;
    return json_at(rungs, (size_t)(n - 1));
}

static bool pc_is(const struct zcl_command_reply *rep, int n,
                  const char *want)
{
    const struct json_value *row = pc_rung(rep, n);
    if (!row)
        return false;
    const char *got = json_get_str(json_get(row, "result"));
    return got && strcmp(got, want) == 0;
}

/* A not_checked rung must carry a non-empty `reason`; a passed rung must
 * NOT — that asymmetry is what keeps the two from being confused. */
static bool pc_reason_discipline(const struct zcl_command_reply *rep)
{
    for (int n = 1; n <= 7; n++) {
        const struct json_value *row = pc_rung(rep, n);
        if (!row)
            return false;
        const char *result = json_get_str(json_get(row, "result"));
        const struct json_value *reason = json_get(row, "reason");
        if (!result)
            return false;
        if (strcmp(result, "not_checked") == 0) {
            if (!reason || reason->type != JSON_STR ||
                json_get_str(reason)[0] == '\0')
                return false;
        } else if (reason) {
            return false;
        }
        const char *detail = json_get_str(json_get(row, "detail"));
        if (!detail || detail[0] == '\0')
            return false;
    }
    return true;
}

/* Every case shares this floor: rung 7 is never passed, the chain is
 * never complete, the report line for each rung is tagged distinctly,
 * the envelope keeps `data` (status PASSED), and the report fits the
 * declared 16384-byte leaf budget. */
static bool pc_invariants(const struct zcl_command_reply *rep)
{
    if (rep->status != ZCL_COMMAND_STATUS_PASSED)
        return false;
    if (!pc_is(rep, 7, "not_checked"))
        return false;
    const struct json_value *cc = json_get(&rep->data, "chain_complete");
    if (!cc || cc->type != JSON_BOOL || json_get_bool(cc))
        return false;
    if (!pc_reason_discipline(rep))
        return false;
    const struct json_value *report = json_get(&rep->data, "report");
    if (!report || report->type != JSON_ARR || json_size(report) != 7)
        return false;
    for (int n = 1; n <= 7; n++) {
        const char *line = json_get_str(json_at(report, (size_t)(n - 1)));
        const char *result = json_get_str(json_get(pc_rung(rep, n), "result"));
        if (!line || !result)
            return false;
        const char *want = strcmp(result, "passed") == 0      ? "PASS"
                           : strcmp(result, "failed") == 0    ? "FAIL"
                                                              : "-- SKIP";
        if (strncmp(line, want, strlen(want)) != 0)
            return false;
    }
    /* The leaf's 16384-byte budget bounds the WHOLE envelope, not just
     * `data`, and write_bounded_json truncates over budget — a truncated
     * report is a lie. Assert real headroom for the envelope. */
    return json_write(&rep->data, NULL, 0) < 14000;
}

static int64_t pc_prefix(const struct zcl_command_reply *rep)
{
    return json_get_int(json_get(&rep->data, "verified_prefix"));
}

static bool pc_detail_has(const struct zcl_command_reply *rep, int n,
                          const char *needle)
{
    const struct json_value *row = pc_rung(rep, n);
    if (!row)
        return false;
    const char *detail = json_get_str(json_get(row, "detail"));
    return detail && strstr(detail, needle) != NULL;
}

/* ── the group ─────────────────────────────────────────────────────── */

int test_proof_chain(void);
int test_proof_chain(void)
{
    int failures = 0;
    printf("\n=== proof chain: light-client walker, rung by rung ===\n");

    /* Regtest for a cheap real Equihash; restore MAIN on the way out so
     * the sequential runner is unaffected. */
    chain_params_select(CHAIN_REGTEST);

    struct pc_fixture good;
    bool built = pc_build_fixture(&good, PC_GOOD);
    PC_CHECK("fixture: real mined headers + anchored batch", built);
    if (!built) {
        chain_params_select(CHAIN_MAIN);
        return failures + 1;
    }

    /* 1. The full chain: rungs 1-6 pass, rung 7 is not_checked. */
    {
        struct pc_run r;
        pc_run_init(&r);
        pc_run_load(&r, &good);
        zcl_native_handle_proof_chain_walk(&r.request, &r.reply);
        PC_CHECK("valid chain: rungs 1-6 all pass",
                 pc_is(&r.reply, 1, "passed") && pc_is(&r.reply, 2, "passed") &&
                     pc_is(&r.reply, 3, "passed") &&
                     pc_is(&r.reply, 4, "passed") &&
                     pc_is(&r.reply, 5, "passed") &&
                     pc_is(&r.reply, 6, "passed"));
        PC_CHECK("valid chain: rung 7 not_checked, never passed",
                 pc_is(&r.reply, 7, "not_checked") &&
                     pc_detail_has(&r.reply, 7, "db_zid_identity_find"));
        PC_CHECK("valid chain: verified_prefix is 6",
                 pc_prefix(&r.reply) == 6);
        PC_CHECK("valid chain: no first_break",
                 json_get(&r.reply.data, "first_break") != NULL &&
                     json_get(&r.reply.data, "first_break")->type ==
                         JSON_NULL);
        PC_CHECK("valid chain: chain_complete stays false + invariants",
                 pc_invariants(&r.reply));
        PC_CHECK("valid chain: ancestor header linked",
                 json_get_int(json_get(pc_rung(&r.reply, 1),
                                       "headers_linked")) == 1);
        PC_CHECK("valid chain: rung 5 names the on-chain root source",
                 strstr(json_get_str(json_get(pc_rung(&r.reply, 5),
                                              "root_source")),
                        "on-chain") != NULL);
        pc_run_free(&r);
    }

    /* 2. Rung 1 broken: flip the last Equihash solution byte. The merkle
     *    root is untouched, so rung 2 must STILL pass on its own merits. */
    {
        struct pc_fixture f = good;
        size_t n = strlen(f.header_hex);
        f.header_hex[n - 1] = (f.header_hex[n - 1] == 'a') ? 'b' : 'a';
        struct pc_run r;
        pc_run_init(&r);
        pc_run_load(&r, &f);
        zcl_native_handle_proof_chain_walk(&r.request, &r.reply);
        PC_CHECK("break 1: rung 1 failed", pc_is(&r.reply, 1, "failed"));
        PC_CHECK("break 1: rung 2 still reports its own pass",
                 pc_is(&r.reply, 2, "passed"));
        PC_CHECK("break 1: rungs 3-6 still pass honestly",
                 pc_is(&r.reply, 3, "passed") && pc_is(&r.reply, 4, "passed") &&
                     pc_is(&r.reply, 5, "passed") &&
                     pc_is(&r.reply, 6, "passed"));
        PC_CHECK("break 1: verified_prefix collapses to 0",
                 pc_prefix(&r.reply) == 0);
        PC_CHECK("break 1: first_break is 1",
                 json_get_int(json_get(&r.reply.data, "first_break")) == 1);
        PC_CHECK("break 1: invariants hold", pc_invariants(&r.reply));
        pc_run_free(&r);
    }

    /* 3. Rung 2 broken: the wrong merkle position. */
    {
        struct pc_fixture f = good;
        f.merkle_index = 0;
        struct pc_run r;
        pc_run_init(&r);
        pc_run_load(&r, &f);
        zcl_native_handle_proof_chain_walk(&r.request, &r.reply);
        PC_CHECK("break 2: rung 1 unaffected", pc_is(&r.reply, 1, "passed"));
        PC_CHECK("break 2: rung 2 failed naming the fold",
                 pc_is(&r.reply, 2, "failed") &&
                     pc_detail_has(&r.reply, 2, "different root"));
        PC_CHECK("break 2: rungs 3-6 still pass honestly",
                 pc_is(&r.reply, 3, "passed") && pc_is(&r.reply, 4, "passed") &&
                     pc_is(&r.reply, 5, "passed") &&
                     pc_is(&r.reply, 6, "passed"));
        PC_CHECK("break 2: verified_prefix stops at 1",
                 pc_prefix(&r.reply) == 1);
        PC_CHECK("break 2: invariants hold", pc_invariants(&r.reply));
        pc_run_free(&r);
    }

    /* 4. Rung 3 broken: an OP_RETURN that is not a ZANC anchor. Rung 4
     *    then has nothing from the chain to compare against, so it is
     *    not_checked — NOT passed. */
    {
        struct pc_fixture f;
        PC_CHECK("break 3: fixture with a non-ZANC OP_RETURN",
                 pc_build_fixture(&f, PC_ANCHOR_NOT_ZANC));
        struct pc_run r;
        pc_run_init(&r);
        pc_run_load(&r, &f);
        zcl_native_handle_proof_chain_walk(&r.request, &r.reply);
        PC_CHECK("break 3: rungs 1-2 pass",
                 pc_is(&r.reply, 1, "passed") && pc_is(&r.reply, 2, "passed"));
        PC_CHECK("break 3: rung 3 failed", pc_is(&r.reply, 3, "failed"));
        PC_CHECK("break 3: rung 4 is not_checked, never passed",
                 pc_is(&r.reply, 4, "not_checked"));
        PC_CHECK("break 3: rung 5 still verifies, but says the root is "
                 "unconfirmed",
                 pc_is(&r.reply, 5, "passed") &&
                     strstr(json_get_str(json_get(pc_rung(&r.reply, 5),
                                                  "root_source")),
                            "NOT confirmed") != NULL);
        PC_CHECK("break 3: verified_prefix stops at 2",
                 pc_prefix(&r.reply) == 2);
        PC_CHECK("break 3: invariants hold", pc_invariants(&r.reply));
        pc_run_free(&r);
    }

    /* 5. Rung 4 broken: a real ZANC anchor over a DIFFERENT digest. The
     *    proof still verifies against the supplied root — and rung 5 must
     *    say plainly that the root was never confirmed on-chain. */
    {
        struct pc_fixture f;
        PC_CHECK("break 4: fixture anchoring a different digest",
                 pc_build_fixture(&f, PC_ANCHOR_OTHER_DIGEST));
        struct pc_run r;
        pc_run_init(&r);
        pc_run_load(&r, &f);
        zcl_native_handle_proof_chain_walk(&r.request, &r.reply);
        PC_CHECK("break 4: rungs 1-3 pass",
                 pc_is(&r.reply, 1, "passed") && pc_is(&r.reply, 2, "passed") &&
                     pc_is(&r.reply, 3, "passed"));
        PC_CHECK("break 4: rung 4 failed naming the mismatch",
                 pc_is(&r.reply, 4, "failed") &&
                     pc_detail_has(&r.reply, 4, "NOT the digest"));
        PC_CHECK("break 4: rung 5 passes but flags an unconfirmed root",
                 pc_is(&r.reply, 5, "passed") &&
                     strstr(json_get_str(json_get(pc_rung(&r.reply, 5),
                                                  "root_source")),
                            "NOT confirmed") != NULL);
        PC_CHECK("break 4: verified_prefix stops at 3",
                 pc_prefix(&r.reply) == 3);
        PC_CHECK("break 4: invariants hold", pc_invariants(&r.reply));
        pc_run_free(&r);
    }

    /* 5b. A SHA2 anchor is not a zid domain root even when the bytes
     *     match — the hash type is part of the commitment. */
    {
        struct pc_fixture f;
        PC_CHECK("break 4b: fixture anchoring the root as SHA2",
                 pc_build_fixture(&f, PC_ANCHOR_SHA2));
        struct pc_run r;
        pc_run_init(&r);
        pc_run_load(&r, &f);
        zcl_native_handle_proof_chain_walk(&r.request, &r.reply);
        PC_CHECK("break 4b: rung 4 failed on the hash type",
                 pc_is(&r.reply, 4, "failed") &&
                     pc_detail_has(&r.reply, 4, "SHA3-256"));
        PC_CHECK("break 4b: invariants hold", pc_invariants(&r.reply));
        pc_run_free(&r);
    }

    /* 6. Rung 5 broken: a corrupted proof sibling. */
    {
        struct pc_fixture f = good;
        size_t n = strlen(f.proof_hex);
        f.proof_hex[n - 1] = (f.proof_hex[n - 1] == 'a') ? 'b' : 'a';
        struct pc_run r;
        pc_run_init(&r);
        pc_run_load(&r, &f);
        zcl_native_handle_proof_chain_walk(&r.request, &r.reply);
        PC_CHECK("break 5: rungs 1-4 pass",
                 pc_is(&r.reply, 1, "passed") && pc_is(&r.reply, 2, "passed") &&
                     pc_is(&r.reply, 3, "passed") &&
                     pc_is(&r.reply, 4, "passed"));
        PC_CHECK("break 5: rung 5 failed naming zid_tree_verify",
                 pc_is(&r.reply, 5, "failed") &&
                     pc_detail_has(&r.reply, 5, "zid_tree_verify"));
        PC_CHECK("break 5: rung 6 still reports its own pass",
                 pc_is(&r.reply, 6, "passed"));
        PC_CHECK("break 5: verified_prefix stops at 4",
                 pc_prefix(&r.reply) == 4);
        PC_CHECK("break 5: invariants hold", pc_invariants(&r.reply));
        pc_run_free(&r);
    }

    /* 7. Rung 6 broken by the clock: now == expiry. */
    {
        struct pc_fixture f = good;
        f.now = (int64_t)good.expiry;
        struct pc_run r;
        pc_run_init(&r);
        pc_run_load(&r, &f);
        zcl_native_handle_proof_chain_walk(&r.request, &r.reply);
        PC_CHECK("break 6a: rungs 1-5 pass",
                 pc_is(&r.reply, 1, "passed") && pc_is(&r.reply, 2, "passed") &&
                     pc_is(&r.reply, 3, "passed") &&
                     pc_is(&r.reply, 4, "passed") &&
                     pc_is(&r.reply, 5, "passed"));
        PC_CHECK("break 6a: rung 6 failed naming expiry, not the signature",
                 pc_is(&r.reply, 6, "failed") &&
                     pc_detail_has(&r.reply, 6, "expired"));
        PC_CHECK("break 6a: verified_prefix stops at 5",
                 pc_prefix(&r.reply) == 5);
        PC_CHECK("break 6a: invariants hold", pc_invariants(&r.reply));
        pc_run_free(&r);
    }

    /* 8. Rung 6 broken by forgery: the signature is flipped BEFORE the
     *    batch is folded, so the forged doc really is the anchored leaf —
     *    rungs 1-5 pass and only the signature rung fails. This is the
     *    case that would be missed if a valid inclusion proof were ever
     *    allowed to imply authenticity. */
    {
        struct pc_fixture f;
        PC_CHECK("break 6b: fixture batching a forged document",
                 pc_build_fixture(&f, PC_DOC_BAD_SIGNATURE));
        struct pc_run r;
        pc_run_init(&r);
        pc_run_load(&r, &f);
        zcl_native_handle_proof_chain_walk(&r.request, &r.reply);
        PC_CHECK("break 6b: rungs 1-5 pass (the forgery IS anchored)",
                 pc_is(&r.reply, 1, "passed") && pc_is(&r.reply, 2, "passed") &&
                     pc_is(&r.reply, 3, "passed") &&
                     pc_is(&r.reply, 4, "passed") &&
                     pc_is(&r.reply, 5, "passed"));
        PC_CHECK("break 6b: rung 6 failed naming the signature",
                 pc_is(&r.reply, 6, "failed") &&
                     pc_detail_has(&r.reply, 6, "signature does not verify"));
        PC_CHECK("break 6b: verified_prefix stops at 5",
                 pc_prefix(&r.reply) == 5);
        PC_CHECK("break 6b: invariants hold", pc_invariants(&r.reply));
        pc_run_free(&r);
    }

    /* 9. A truncated proof wire is a FAILURE, not a skip. */
    {
        struct pc_fixture f = good;
        f.proof_hex[strlen(f.proof_hex) - 4] = '\0';
        struct pc_run r;
        pc_run_init(&r);
        pc_run_load(&r, &f);
        zcl_native_handle_proof_chain_walk(&r.request, &r.reply);
        PC_CHECK("truncated proof: rung 5 failed, named as truncated",
                 pc_is(&r.reply, 5, "failed") &&
                     pc_detail_has(&r.reply, 5, "truncated"));
        PC_CHECK("truncated proof: rungs 1-4 unaffected",
                 pc_is(&r.reply, 1, "passed") && pc_is(&r.reply, 2, "passed") &&
                     pc_is(&r.reply, 3, "passed") &&
                     pc_is(&r.reply, 4, "passed"));
        PC_CHECK("truncated proof: invariants hold", pc_invariants(&r.reply));
        pc_run_free(&r);
    }

    /* 10. An unknown proof version is rejected LOUDLY, never skipped. */
    {
        struct pc_fixture f = good;
        f.proof_hex[0] = '0';
        f.proof_hex[1] = '2';
        struct pc_run r;
        pc_run_init(&r);
        pc_run_load(&r, &f);
        zcl_native_handle_proof_chain_walk(&r.request, &r.reply);
        PC_CHECK("unknown proof version: rung 5 failed naming the version",
                 pc_is(&r.reply, 5, "failed") &&
                     pc_detail_has(&r.reply, 5, "version 2 is unknown") &&
                     pc_detail_has(&r.reply, 5, "never skipped"));
        PC_CHECK("unknown proof version: invariants hold",
                 pc_invariants(&r.reply));
        pc_run_free(&r);
    }

    /* 11. index >= num_leaves: a proof that claims a leaf that cannot
     *     exist. zid_proof_encode does not police this, so the walker
     *     must. */
    {
        uint8_t sibs[ZID_TREE_MAX_PEAKS][32];
        memset(sibs, 0x77, sizeof(sibs));
        uint8_t pwire[ZID_PROOF_WIRE_MAX];
        size_t n = zid_proof_encode(pwire, sizeof(pwire), 99, PC_LEAVES,
                                    (const uint8_t (*)[32])sibs, 2);
        PC_CHECK("bogus-index proof: encodes", n > 0);
        struct pc_fixture f = good;
        pc_hex(pwire, n, f.proof_hex);
        struct pc_run r;
        pc_run_init(&r);
        pc_run_load(&r, &f);
        zcl_native_handle_proof_chain_walk(&r.request, &r.reply);
        PC_CHECK("bogus-index proof: rung 5 failed naming the leaf count",
                 pc_is(&r.reply, 5, "failed") &&
                     pc_detail_has(&r.reply, 5, "leaf 99 of a tree with only"));
        PC_CHECK("bogus-index proof: invariants hold", pc_invariants(&r.reply));
        pc_run_free(&r);
    }

    /* 12. Evidence WITHHELD is not_checked with a reason — visibly
     *     different from case 9's supplied-but-broken failure. */
    {
        struct pc_fixture f = good;
        f.proof_hex[0] = '\0';
        f.root_hex[0] = '\0';
        f.tx_hex[0] = '\0';
        f.header_hex[0] = '\0';
        f.ancestor_hex[0] = '\0';
        f.branch_hex[0] = '\0';
        struct pc_run r;
        pc_run_init(&r);
        pc_run_load(&r, &f);
        zcl_native_handle_proof_chain_walk(&r.request, &r.reply);
        PC_CHECK("doc only: rungs 1-5 are not_checked with reasons",
                 pc_is(&r.reply, 1, "not_checked") &&
                     pc_is(&r.reply, 2, "not_checked") &&
                     pc_is(&r.reply, 3, "not_checked") &&
                     pc_is(&r.reply, 4, "not_checked") &&
                     pc_is(&r.reply, 5, "not_checked"));
        PC_CHECK("doc only: rung 6 still verifies the signature",
                 pc_is(&r.reply, 6, "passed"));
        PC_CHECK("doc only: verified_prefix is 0 (nothing consecutive passed)",
                 pc_prefix(&r.reply) == 0);
        PC_CHECK("doc only: no rung is reported failed",
                 json_get_int(json_get(json_get(&r.reply.data, "summary"),
                                       "failed")) == 0 &&
                     json_get_int(json_get(json_get(&r.reply.data, "summary"),
                                           "not_checked")) == 6);
        PC_CHECK("doc only: invariants hold", pc_invariants(&r.reply));
        pc_run_free(&r);
    }

    /* 13. No document at all: the ONE input that makes a walk impossible,
     *     so it is the ONE case that fails the envelope outright. */
    {
        struct pc_run r;
        pc_run_init(&r);
        zcl_native_handle_proof_chain_walk(&r.request, &r.reply);
        PC_CHECK("no document: NO_DOCUMENT, not an empty report",
                 r.reply.status == ZCL_COMMAND_STATUS_FAILED &&
                     strcmp(r.reply.error.code, "NO_DOCUMENT") == 0 &&
                     r.reply.error.message[0] != '\0');
        pc_run_free(&r);
    }

    /* 14. A malformed document is refused, never walked. */
    {
        struct pc_run r;
        pc_run_init(&r);
        (void)json_push_kv_str(&r.input, "doc", "00ff00ff");
        zcl_native_handle_proof_chain_walk(&r.request, &r.reply);
        PC_CHECK("bad document: refused with a reason",
                 r.reply.status == ZCL_COMMAND_STATUS_FAILED &&
                     strcmp(r.reply.error.code, "NO_DOCUMENT") == 0);
        pc_run_free(&r);
    }

    chain_params_select(CHAIN_MAIN);
    return failures;
}

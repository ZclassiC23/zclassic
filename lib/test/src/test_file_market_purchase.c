/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Durable buyer plan/commit, idempotency, and fail-closed restart tests. */

#include "test/test_core.h"

#include "chain/chainparams.h"
#include "crypto/ed25519.h"
#include "models/database.h"
#include "models/file_offer.h"
#include "models/vault_intent.h"
#include "models/wallet_identity.h"
#include "net/file_market.h"
#include "platform/time_compat.h"
#include "sapling/sapling.h"
#include "services/file_market_purchase_service.h"
#include "wallet/wallet_lock.h"

#include <stdio.h>
#include <string.h>

#define PURCHASE_CHECK(label, condition) do {                       \
    printf("file_market purchase: %s... ", (label));                \
    if (condition) printf("OK\n");                                  \
    else { printf("FAIL\n"); failures++; }                          \
} while (0)

struct purchase_fixture {
    struct node_db *ndb;
    struct wallet_identity_row identity;
    uint8_t initial_root[32];
    uint8_t reserved_root[32];
    int32_t tip_height;
    uint8_t tip_hash[32];
    int money_reads;
    int sends;
    int notifications;
    bool money_current;
    bool source_owned;
    int64_t expected_amount;
    uint8_t expected_memo[FILE_MARKET_PAYMENT_MEMO_BYTES];
};

static struct zcl_result purchase_money(
    void *opaque, const char *scope, struct wallet_money_snapshot *out)
{
    struct purchase_fixture *f = opaque;
    if (!f || !out || strcmp(scope, "dev") != 0)
        return ZCL_ERR(-1, "fixture scope mismatch");
    memset(out, 0, sizeof(*out));
    out->identity = f->identity;
    snprintf(out->wallet_scope, sizeof(out->wallet_scope), "dev");
    snprintf(out->status, sizeof(out->status), "%s",
             f->money_current ? "CURRENT" : "UNKNOWN");
    snprintf(out->reason, sizeof(out->reason), "%s",
             f->money_current ? "fixture current" : "fixture reader unavailable");
    out->complete = f->money_current;
    out->confirmed_zat = 30000000;
    out->agent_available_zat = 5000000;
    out->intent_reserved_zat = vault_intent_reserved_total(
        f->ndb, "dev", f->identity.wallet_instance_id);
    out->tip_height = f->tip_height;
    memcpy(out->tip_hash, f->tip_hash, 32);
    memcpy(out->snapshot_root,
           f->money_reads++ == 0 ? f->initial_root : f->reserved_root, 32);
    return ZCL_OK;
}

static struct zcl_result purchase_source(void *opaque, const char *source)
{
    struct purchase_fixture *f = opaque;
    return f && f->source_owned && source &&
               strcmp(source, "fixture-owned-source") == 0
        ? ZCL_OK : ZCL_ERR(-1, "source is not owned");
}

static struct zcl_result purchase_send(
    void *opaque, const char *source, const char *seller, int64_t amount,
    const uint8_t memo[FILE_MARKET_PAYMENT_MEMO_BYTES], uint8_t txid[32])
{
    struct purchase_fixture *f = opaque;
    if (!f || strcmp(source, "fixture-owned-source") != 0 ||
        !seller || seller[0] == '\0' || amount != f->expected_amount ||
        memcmp(memo, f->expected_memo, FILE_MARKET_PAYMENT_MEMO_BYTES) != 0)
        return ZCL_ERR(-2, "exact send contract changed");
    f->sends++;
    memset(txid, 0x91, 32);
    return ZCL_OK;
}

static bool purchase_notify(void *opaque, const struct file_payment *payment)
{
    struct purchase_fixture *f = opaque;
    if (!f || !payment || payment->amount_zat != f->expected_amount ||
        file_payment_auth_verify(payment, payment->network_genesis) !=
            FILE_PAYMENT_AUTH_OK)
        return false;
    f->notifications++;
    return true;
}

static bool purchase_offer(struct file_offer *offer, int64_t now)
{
    const struct chain_params *params = chain_params_get();
    struct jub_point payment_key;
    uint8_t seed[32], secret[32];
    if (!params) return false;
    memset(offer, 0, sizeof(*offer));
    memset(seed, 0x57, sizeof(seed));
    memset(offer->root_hash, 0x67, 32);
    memcpy(offer->network_genesis,
           params->consensus.hashGenesisBlock.data, 32);
    ed25519_keypair(offer->seller_pubkey, secret, seed);
    snprintf(offer->filename, sizeof(offer->filename), "purchase.bin");
    offer->size_bytes = FILE_MARKET_CHUNK_SIZE + 1u;
    offer->num_chunks = 2;
    offer->price_per_mb = 1200;
    for (uint8_t d = 1; ; d++) {
        memset(offer->z_addr, 0, sizeof(offer->z_addr));
        offer->z_addr[0] = d;
        if (sapling_diversifier_to_gd(&payment_key, offer->z_addr)) break;
        if (d == UINT8_MAX) return false;
    }
    jub_to_bytes(offer->z_addr + 11, &payment_key);
    offer->peer_ip[15] = 1;
    offer->peer_port = 18034;
    offer->ttl = FILE_MARKET_MAX_TTL;
    offer->last_seen = now;
    offer->auth_version = FILE_MARKET_OFFER_VERSION;
    offer->nonce = 9901;
    offer->issued_unix = now - 60;
    offer->expires_unix = now + 600;
    return file_offer_auth_seal(offer, seed) == FILE_OFFER_AUTH_OK;
}

int file_market_purchase_tests(void)
{
    int failures = 0;
    char dir[256], path[320];
    test_make_tmpdir(dir, sizeof(dir), "file_market_purchase", "intent");
    snprintf(path, sizeof(path), "%s/node.db", dir);
    struct node_db ndb; memset(&ndb, 0, sizeof(ndb));
    int64_t now = (int64_t)platform_time_wall_time_t();
    struct file_offer offer;
    bool ready = node_db_open(&ndb, path) && purchase_offer(&offer, now) &&
                 db_file_offer_save(&ndb, &offer);
    const struct chain_params *params = chain_params_get();
    struct purchase_fixture fixture; memset(&fixture, 0, sizeof(fixture));
    fixture.ndb = &ndb;
    memset(fixture.initial_root, 0x31, 32);
    memset(fixture.reserved_root, 0x32, 32);
    fixture.tip_height = 420;
    memset(fixture.tip_hash, 0x44, 32);
    fixture.money_current = true;
    fixture.source_owned = true;
    ready = ready && params && wallet_identity_ensure(
        &ndb, params->consensus.hashGenesisBlock.data, "dev",
        &fixture.identity);
    wallet_lock_reset_for_test();
    wallet_lock_note_encrypted_at_rest();
    ready = ready && wallet_lock_unlock(NULL, NULL, "market-purchase-test").ok;
    PURCHASE_CHECK("authenticated offer, identity, and encrypted fixture", ready);
    if (!ready) goto cleanup;

    struct market_purchase_runtime runtime; memset(&runtime, 0, sizeof(runtime));
    runtime.node_db = &ndb;
    runtime.read_money = purchase_money;
    runtime.money_ctx = &fixture;
    runtime.check_source = purchase_source;
    runtime.source_ctx = &fixture;
    runtime.send = purchase_send;
    runtime.send_ctx = &fixture;
    runtime.notify = purchase_notify;
    runtime.notify_ctx = &fixture;
    runtime.tip_height = fixture.tip_height;
    memcpy(runtime.tip_hash, fixture.tip_hash, 32);
    runtime.maximum_fee_zat = 10000;
    runtime.now_unix = now;
    struct market_purchase_request request; memset(&request, 0, sizeof(request));
    snprintf(request.wallet_scope, sizeof(request.wallet_scope), "dev");
    memcpy(request.offer_id, offer.offer_id, 32);
    snprintf(request.source_address, sizeof(request.source_address),
             "fixture-owned-source");
    request.chunk_start = 0;
    request.chunks_paid = 1;
    snprintf(request.idempotency_key, sizeof(request.idempotency_key),
             "purchase-1");
    file_market_offer_range_zat(&offer, 0, 1, &fixture.expected_amount);

    struct market_purchase_view plan, replay, committed;
    struct zcl_result planned = market_purchase_plan(&runtime, &request, &plan);
    struct file_payment memo_contract; memset(&memo_contract, 0,
                                               sizeof(memo_contract));
    memo_contract.version = FILE_MARKET_PAYMENT_VERSION;
    memcpy(memo_contract.network_genesis, offer.network_genesis, 32);
    memcpy(memo_contract.offer_id, offer.offer_id, 32);
    memcpy(memo_contract.txid, plan.plan_id, 32);
    memo_contract.chunk_start = request.chunk_start;
    memo_contract.chunks_paid = request.chunks_paid;
    memo_contract.amount_zat = fixture.expected_amount;
    memcpy(memo_contract.buyer_pubkey, plan.buyer_pubkey, 32);
    bool memo_ready = file_payment_memo_encode(
        &memo_contract, fixture.expected_memo) == FILE_PAYMENT_AUTH_OK;
    PURCHASE_CHECK("plan reserves exact range value plus maximum fee",
        planned.ok && memo_ready && plan.amount_zat == fixture.expected_amount &&
        plan.reserved_zat == fixture.expected_amount + 10000 &&
        vault_intent_reserved_total(
            &ndb, "dev", fixture.identity.wallet_instance_id) ==
                plan.reserved_zat);

    struct zcl_result replayed = market_purchase_plan(
        &runtime, &request, &replay);
    PURCHASE_CHECK("same idempotency key returns the same durable plan",
        replayed.ok && replay.idempotent_replay &&
        memcmp(replay.plan_id, plan.plan_id, 32) == 0);
    struct market_purchase_request changed = request;
    changed.chunk_start = 1;
    PURCHASE_CHECK("changed request under same idempotency key conflicts",
        !market_purchase_plan(&runtime, &changed, &replay).ok);

    struct zcl_result committed_result = market_purchase_commit(
        &runtime, "dev", plan.plan_id, &committed);
    PURCHASE_CHECK("commit sends exact memo and seals a buyer payment claim",
        committed_result.ok && fixture.sends == 1 &&
        fixture.notifications == 1 && committed.has_txid &&
        committed.has_claim && committed.payment_notification_queued);
    struct zcl_result committed_replay = market_purchase_commit(
        &runtime, "dev", plan.plan_id, &replay);
    PURCHASE_CHECK("commit replay never broadcasts a second transaction",
        committed_replay.ok && replay.idempotent_replay && fixture.sends == 1 &&
        fixture.notifications == 2 &&
        memcmp(replay.txid, committed.txid, 32) == 0);

    node_db_close(&ndb);
    PURCHASE_CHECK("restart reopens durable purchase intent",
                   node_db_open(&ndb, path));
    fixture.ndb = &ndb;
    struct market_purchase_view restarted;
    PURCHASE_CHECK("restart reconstructs claim without buyer key disclosure",
        market_purchase_status(&runtime, plan.plan_id, &restarted).ok &&
        restarted.has_claim &&
        memcmp(restarted.claim_id, committed.claim_id, 32) == 0);

    request.idempotency_key[9] = '2';
    fixture.money_reads = 1;
    struct market_purchase_view uncertain;
    struct zcl_result planned_uncertain = market_purchase_plan(
        &runtime, &request, &uncertain);
    bool claimed = planned_uncertain.ok && vault_intent_claim_commit(
        &ndb, uncertain.plan_id, now);
    int before = fixture.sends;
    struct zcl_result refused = market_purchase_commit(
        &runtime, "dev", uncertain.plan_id, &replay);
    PURCHASE_CHECK("restart-uncertain proving state fails closed without resend",
        claimed && !refused.ok && fixture.sends == before);

    request.idempotency_key[9] = '3';
    fixture.money_reads = 1;
    struct market_purchase_view stale;
    struct zcl_result planned_stale = market_purchase_plan(
        &runtime, &request, &stale);
    runtime.tip_hash[0] ^= 1;
    before = fixture.sends;
    struct zcl_result stale_commit = market_purchase_commit(
        &runtime, "dev", stale.plan_id, &replay);
    PURCHASE_CHECK("changed tip-bound state conflicts before wallet mutation",
        planned_stale.ok && !stale_commit.ok && fixture.sends == before);

    runtime.tip_hash[0] ^= 1;
    request.idempotency_key[9] = '4';
    fixture.money_reads = 1;
    struct market_purchase_view fee_bound;
    struct zcl_result planned_fee = market_purchase_plan(
        &runtime, &request, &fee_bound);
    runtime.maximum_fee_zat++;
    before = fixture.sends;
    struct zcl_result changed_fee = market_purchase_commit(
        &runtime, "dev", fee_bound.plan_id, &replay);
    PURCHASE_CHECK("changed maximum fee conflicts before wallet mutation",
        planned_fee.ok && !changed_fee.ok && fixture.sends == before);
    runtime.maximum_fee_zat--;

    request.idempotency_key[9] = '5';
    fixture.money_reads = 1;
    struct market_purchase_view source_bound;
    struct zcl_result planned_source = market_purchase_plan(
        &runtime, &request, &source_bound);
    fixture.source_owned = false;
    before = fixture.sends;
    struct zcl_result unowned = market_purchase_commit(
        &runtime, "dev", source_bound.plan_id, &replay);
    PURCHASE_CHECK("unowned source refuses before wallet mutation",
        planned_source.ok && !unowned.ok && fixture.sends == before);
    fixture.source_owned = true;

    request.idempotency_key[9] = '6';
    fixture.money_reads = 1;
    fixture.identity.network_genesis[0] ^= 1;
    PURCHASE_CHECK("wrong-network signed offer refuses without reservation",
        !market_purchase_plan(&runtime, &request, &replay).ok);
    fixture.identity.network_genesis[0] ^= 1;

    request.idempotency_key[9] = '7';
    fixture.money_reads = 1;
    fixture.money_current = false;
    PURCHASE_CHECK("unknown money reader never collapses to a zero-balance plan",
        !market_purchase_plan(&runtime, &request, &replay).ok);
    fixture.money_current = true;

cleanup:
    if (ndb.open) node_db_close(&ndb);
    wallet_lock_reset_for_test();
    test_rm_rf(dir);
    return failures;
}

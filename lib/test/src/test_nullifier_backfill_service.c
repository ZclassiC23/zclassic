/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Unit tests for the owner-gated nullifier_backfill_service. The service
 * must be populate-only: it reuses utxo_apply_check_and_insert_nullifiers()
 * and leaves the consensus check path unchanged. */

#include "test/test_helpers.h"

#include "controllers/agent_security_posture.h"
#include "core/uint256.h"
#include "jobs/utxo_apply_delta.h"
#include "jobs/utxo_apply_nullifiers.h"
#include "json/json.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "services/nullifier_backfill_service.h"
#include "storage/nullifier_kv.h"
#include "storage/progress_store.h"
#include "util/blocker.h"
#include "util/safe_alloc.h"

#include <sqlite3.h>
#include <stdio.h>
#include <string.h>

#define NB_CHECK(name, expr) do {                                      \
    if (expr) { printf("  nullifier_backfill: %s... OK\n", (name)); } \
    else { printf("  nullifier_backfill: %s... FAIL\n", (name));      \
           failures++; }                                               \
} while (0)

struct nbf_chain {
    struct block bodies[4];
    int n;
};

static void nbf_nf(uint8_t out[32], uint8_t tag, uint8_t mark)
{
    memset(out, 0, 32);
    out[0] = tag;
    out[1] = mark;
    out[31] = 0xBF;
}

static void nbf_txid(struct uint256 *out, int height, int salt)
{
    uint256_set_null(out);
    out->data[0] = (uint8_t)(0x40 + height);
    out->data[1] = (uint8_t)salt;
}

static bool nbf_make_block(struct block *b, int height, int salt)
{
    block_init(b);
    b->header.nVersion = 4;
    b->header.nTime = (uint32_t)(1700005000u + (uint32_t)height);
    b->header.nBits = 0x1f07ffff;
    b->num_vtx = 1;
    b->vtx = zcl_calloc(1, sizeof(struct transaction), "nbf_vtx");
    if (!b->vtx)
        return false;
    transaction_init(&b->vtx[0]);
    nbf_txid(&b->vtx[0].hash, height, salt);
    return true;
}

static bool nbf_add_sapling_spend(struct transaction *tx,
                                  uint8_t tag, uint8_t mark)
{
    tx->v_shielded_spend =
        zcl_calloc(1, sizeof(struct spend_description), "nbf_sapling");
    if (!tx->v_shielded_spend)
        return false;
    tx->num_shielded_spend = 1;
    nbf_nf(tx->v_shielded_spend[0].nullifier.data, tag, mark);
    return true;
}

static bool nbf_add_joinsplit(struct transaction *tx,
                              uint8_t tag0, uint8_t tag1, uint8_t mark)
{
    tx->v_joinsplit =
        zcl_calloc(1, sizeof(struct js_description), "nbf_sprout");
    if (!tx->v_joinsplit)
        return false;
    tx->num_joinsplit = 1;
    nbf_nf(tx->v_joinsplit[0].nullifiers[0].data, tag0, mark);
    nbf_nf(tx->v_joinsplit[0].nullifiers[1].data, tag1, mark);
    return true;
}

static bool nbf_chain_init(struct nbf_chain *ch)
{
    memset(ch, 0, sizeof(*ch));
    ch->n = 4;
    for (int i = 0; i < ch->n; i++) {
        if (!nbf_make_block(&ch->bodies[i], i, 0x10 + i))
            return false;
    }
    return nbf_add_sapling_spend(&ch->bodies[1].vtx[0], 0xA1, 0x51) &&
           nbf_add_joinsplit(&ch->bodies[2].vtx[0], 0xB1, 0xB2, 0x52) &&
           nbf_add_sapling_spend(&ch->bodies[3].vtx[0], 0xA1, 0x51);
}

static void nbf_chain_free(struct nbf_chain *ch)
{
    for (int i = 0; i < ch->n; i++)
        block_free(&ch->bodies[i]);
    memset(ch, 0, sizeof(*ch));
}

static bool nbf_reader(struct block *out, int64_t height,
                       const char *datadir, void *user, bool *found_out)
{
    (void)datadir;
    struct nbf_chain *ch = user;
    if (found_out)
        *found_out = false;
    if (!out || !ch || !found_out || height < 0 || height >= ch->n)
        return false;
    if (!test_block_copy(out, &ch->bodies[height], "nbf_reader_copy"))
        return false;
    *found_out = true;
    return true;
}

static int64_t nbf_row_count(sqlite3 *db)
{
    sqlite3_stmt *st = NULL;
    int64_t n = -1;

    if (sqlite3_prepare_v2(db, "SELECT COUNT(*) FROM nullifiers",
                           -1, &st, NULL) != SQLITE_OK)
        return -1;
    if (sqlite3_step(st) == SQLITE_ROW)
        n = sqlite3_column_int64(st, 0);
    sqlite3_finalize(st);
    return n;
}

static bool nbf_marker_absent(sqlite3 *db, const char *key)
{
    char buf[32];
    size_t len = 0;
    bool found = true;

    return progress_meta_get(db, key, buf, sizeof(buf), &len, &found) &&
           !found;
}

static bool nbf_posture_complete(void)
{
    struct json_value root;
    const struct json_value *posture;
    bool ok;

    json_init(&root);
    json_set_object(&root);
    agent_push_security_posture_json(&root, "security_posture", NULL);
    posture = json_get(&root, "security_posture");
    ok = posture &&
         json_get_bool(json_get(posture, "nullifier_history_complete")) &&
         !json_get_bool(json_get(posture, "nullifier_backfill_gap")) &&
         json_get_int(json_get(posture, "nullifier_activation_cursor")) == 0;
    json_free(&root);
    return ok;
}

static bool nbf_candidate_accepted(sqlite3 *db, const struct block *blk,
                                   bool *accepted_out)
{
    struct delta_summary summary;
    bool ok = true;

    if (!db || !blk || !accepted_out)
        return false;
    memset(&summary, 0, sizeof(summary));
    summary.ok = true;
    summary.status = "ok";

    progress_store_tx_lock();
    if (sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL) != SQLITE_OK) {
        progress_store_tx_unlock();
        return false;
    }
    ok = utxo_apply_check_and_insert_nullifiers(db, blk, 3, &summary);
    if (sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL) != SQLITE_OK)
        ok = false;
    progress_store_tx_unlock();
    if (!ok)
        return false;
    *accepted_out = summary.ok;
    return true;
}

static bool nbf_expected_rows_present(sqlite3 *db)
{
    uint8_t a[32], b[32], c[32];
    bool fa = false, fb = false, fc = false;
    int64_t ha = -1, hb = -1, hc = -1;

    nbf_nf(a, 0xA1, 0x51);
    nbf_nf(b, 0xB1, 0x52);
    nbf_nf(c, 0xB2, 0x52);
    return nullifier_kv_get(db, a, NULLIFIER_POOL_SAPLING, &fa, &ha) &&
           nullifier_kv_get(db, b, NULLIFIER_POOL_SPROUT, &fb, &hb) &&
           nullifier_kv_get(db, c, NULLIFIER_POOL_SPROUT, &fc, &hc) &&
           fa && fb && fc && ha == 1 && hb == 2 && hc == 2 &&
           nbf_row_count(db) == 3;
}

static bool nbf_seed_marker(sqlite3 *db, const char *key, const char *value)
{
    return progress_meta_set(db, key, value, strlen(value));
}

int test_nullifier_backfill_service(void);
int test_nullifier_backfill_service(void)
{
    int failures = 0;
    char dir[256];
    struct nbf_chain ch;

    test_make_tmpdir(dir, sizeof(dir), "nullifier_backfill", "main");
    NB_CHECK("chain init", nbf_chain_init(&ch));
    NB_CHECK("progress_store opens", progress_store_open(dir));
    sqlite3 *db = progress_store_db();
    NB_CHECK("db handle", db != NULL);
    NB_CHECK("schema ensure", nullifier_kv_ensure_schema(db));

    blocker_clear(UTXO_APPLY_NF_GAP_BLOCKER_ID);
    NB_CHECK("activation marker set",
             nbf_seed_marker(db, NULLIFIER_BACKFILL_ACTIVATION_KEY, "3"));
    utxo_apply_nullifier_gap_blocker_refresh(db);
    NB_CHECK("gap blocker visible before remediation",
             blocker_exists(UTXO_APPLY_NF_GAP_BLOCKER_ID));

    bool accepted = false;
    NB_CHECK("pre-backfill duplicate candidate runs",
             nbf_candidate_accepted(db, &ch.bodies[3], &accepted));
    NB_CHECK("pre-backfill duplicate candidate accepted because set is empty",
             accepted);
    NB_CHECK("pre-backfill rollback left set empty", nbf_row_count(db) == 0);

    struct nullifier_backfill_config cfg = {
        .progress_db = db,
        .datadir = dir,
        .read_block = nbf_reader,
        .read_block_user = &ch,
    };
    struct nullifier_backfill_report rep;
    struct zcl_result r = nullifier_backfill_service_run(&cfg, &rep);
    NB_CHECK("service run ok", r.ok);
    NB_CHECK("service walked expected range",
             rep.completed && !rep.already_complete &&
             rep.start_height == 0 && rep.target_exclusive == 3 &&
             rep.blocks_scanned == 3 && rep.next_height == 3);
    NB_CHECK("expected nullifier rows inserted", nbf_expected_rows_present(db));
    NB_CHECK("activation marker cleared",
             nbf_marker_absent(db, NULLIFIER_BACKFILL_ACTIVATION_KEY));
    NB_CHECK("resume marker cleared",
             nbf_marker_absent(db, NULLIFIER_BACKFILL_RESUME_KEY));
    NB_CHECK("gap blocker cleared",
             !blocker_exists(UTXO_APPLY_NF_GAP_BLOCKER_ID));
    NB_CHECK("security posture reports nullifier history complete",
             nbf_posture_complete());

    accepted = true;
    NB_CHECK("post-backfill duplicate candidate runs",
             nbf_candidate_accepted(db, &ch.bodies[3], &accepted));
    NB_CHECK("post-backfill duplicate candidate rejected", !accepted);
    NB_CHECK("post-backfill rollback kept exact set", nbf_row_count(db) == 3);

    struct nullifier_backfill_report rep2;
    r = nullifier_backfill_service_run(&cfg, &rep2);
    NB_CHECK("idempotent second run ok", r.ok);
    NB_CHECK("idempotent second run no-op",
             rep2.completed && rep2.already_complete &&
             rep2.blocks_scanned == 0 && nbf_row_count(db) == 3);

    NB_CHECK("resume setup clears rows",
             nullifier_kv_delete_range(db, 0, 10) && nbf_row_count(db) == 0);
    {
        uint8_t a[32];
        nbf_nf(a, 0xA1, 0x51);
        NB_CHECK("resume setup seeds completed h=1 row",
                 nullifier_kv_add(db, a, NULLIFIER_POOL_SAPLING, 1));
    }
    NB_CHECK("resume setup activation marker",
             nbf_seed_marker(db, NULLIFIER_BACKFILL_ACTIVATION_KEY, "3"));
    NB_CHECK("resume setup cursor marker",
             nbf_seed_marker(db, NULLIFIER_BACKFILL_RESUME_KEY, "2"));

    /* dump_state_json (zcl_state subsystem=nullifier_backfill) while the
     * two durable markers are seeded mid-run: status must read
     * "in_progress" (resume_cursor 2 < activation_cursor 3). */
    {
        struct json_value v = {0};
        json_set_object(&v);
        bool ok = nullifier_backfill_dump_state_json(&v, NULL);
        const struct json_value *status = json_get(&v, "status");
        const struct json_value *activation =
            json_get(&v, "activation_cursor");
        const struct json_value *resume = json_get(&v, "resume_cursor");
        bool shape_ok = ok && status &&
                        strcmp(json_get_str(status), "in_progress") == 0 &&
                        activation && json_get_int(activation) == 3 &&
                        resume && json_get_int(resume) == 2;
        json_free(&v);
        NB_CHECK("dump_state_json reports in_progress with cursors",
                 shape_ok);
    }

    struct nullifier_backfill_report rep3;
    r = nullifier_backfill_service_run(&cfg, &rep3);
    NB_CHECK("resume run ok", r.ok);
    NB_CHECK("resume run starts at persisted next height",
             rep3.completed && rep3.start_height == 2 &&
             rep3.target_exclusive == 3 && rep3.blocks_scanned == 1 &&
             rep3.next_height == 3);
    NB_CHECK("resume run restored expected set", nbf_expected_rows_present(db));
    NB_CHECK("resume run cleared markers",
             nbf_marker_absent(db, NULLIFIER_BACKFILL_ACTIVATION_KEY) &&
             nbf_marker_absent(db, NULLIFIER_BACKFILL_RESUME_KEY));

    {
        struct json_value v = {0};
        json_set_object(&v);
        bool ok = nullifier_backfill_dump_state_json(&v, NULL);
        const struct json_value *status = json_get(&v, "status");
        bool shape_ok = ok && status &&
                        strcmp(json_get_str(status),
                              "not_needed_or_complete") == 0;
        json_free(&v);
        NB_CHECK("dump_state_json reports not_needed_or_complete post-run",
                 shape_ok);
    }

    progress_store_close();
    blocker_clear(UTXO_APPLY_NF_GAP_BLOCKER_ID);
    nbf_chain_free(&ch);
    test_cleanup_tmpdir(dir);
    return failures;
}

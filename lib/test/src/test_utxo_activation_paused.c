/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#include "test/test_helpers.h"

#include "conditions/utxo_activation_paused.h"
#include "chain/mmb.h"
#include "chain/mmr.h"
#include "coins/utxo_commitment.h"
#include "event/event.h"
#include "sync/sync_state.h"
#include "framework/condition.h"
#include "models/database.h"
#include "platform/clock.h"
#include "net/snapshot_sync_contract.h"
#include "services/sync_monitor.h"
#include "validation/process_block.h"

#include <stdatomic.h>
#include <sqlite3.h>
#include <string.h>

#define UAP_CHECK(name, expr) do { \
    printf("utxo_activation_paused: %s... ", (name)); \
    if (expr) printf("OK\n"); \
    else { printf("FAIL\n"); failures++; } \
} while (0)

void register_block_failed_mask_at_tip(void);
void register_tip_wedged_resnapshot(void);
void block_failed_mask_at_tip_test_reset(void);
int block_failed_mask_at_tip_test_stall_type(void);
void block_failed_mask_at_tip_test_mark_exhausted(int target_height);
void tip_wedged_resnapshot_test_reset(void);
void tip_wedged_resnapshot_test_set_runtime(
    struct node_db *ndb,
    struct snapshot_sync_service *svc);
int tip_wedged_resnapshot_test_remedy_calls(void);
int tip_wedged_resnapshot_test_recovery_accepted(void);
int tip_wedged_resnapshot_test_last_manifest_height(void);

struct fake_clock {
    _Atomic int64_t wall_ms;
};

static int64_t fake_now_mono(void *self)
{
    (void)self;
    return 1;
}

static int64_t fake_now_wall(void *self)
{
    struct fake_clock *c = (struct fake_clock *)self;
    return atomic_load(&c->wall_ms);
}

static void fake_clock_install(struct fake_clock *c, int64_t unix_s)
{
    atomic_store(&c->wall_ms, unix_s * 1000);
    static clock_iface_t iface;
    iface.now_monotonic_ns = fake_now_mono;
    iface.now_wall_ms = fake_now_wall;
    iface.self = c;
    clock_set_default(&iface);
}

static void fake_clock_set(struct fake_clock *c, int64_t unix_s)
{
    atomic_store(&c->wall_ms, unix_s * 1000);
}

static void operator_observer(enum event_type type, uint32_t peer_id,
                              const void *payload, uint32_t payload_len,
                              void *ctx)
{
    (void)type;
    (void)peer_id;
    (void)payload;
    (void)payload_len;
    _Atomic int *count = (_Atomic int *)ctx;
    atomic_fetch_add(count, 1);
}

static void reset_conditions(void)
{
    event_log_init();
    condition_engine_reset_for_testing();
    event_clear_all_observers();
    process_block_test_set_utxo_activation_paused_height(-1);
    utxo_activation_paused_test_reset();
    block_failed_mask_at_tip_test_reset();
    tip_wedged_resnapshot_test_reset();
}

static struct block_index *insert_test_block(struct main_state *ms,
                                             struct uint256 *hashes,
                                             int height,
                                             unsigned status)
{
    memset(&hashes[height], 0, sizeof(hashes[height]));
    hashes[height].data[0] = (uint8_t)height;
    hashes[height].data[1] = 0xA5;
    struct block_index *bi = chainstate_insert_block_index(
        (struct chainstate *)ms, &hashes[height]);
    if (!bi) return NULL;
    bi->nHeight = height;
    bi->nStatus = status;
    bi->nTx = 1;
    bi->nChainTx = (uint32_t)(height + 1);
    if (height > 0)
        bi->pprev = block_map_find(&ms->map_block_index, &hashes[height - 1]);
    return bi;
}

static void persist_snapshot_roots(struct node_db *ndb,
                                   const uint8_t block_hash[32],
                                   const uint8_t chain_work[32])
{
    struct mmr mmr;
    struct mmb mmb;
    struct mmb_leaf leaf;
    uint8_t buf[MMB_SERIALIZED_MAX];
    size_t len;

    mmr_init(&mmr);
    mmr_append(&mmr, block_hash);
    len = mmr_serialize(&mmr, buf, sizeof(buf));
    node_db_state_set(ndb, "mmr_state", buf, len);

    mmb_init(&mmb);
    mmb_leaf_from_block(&leaf, block_hash, 101, 1234567890, 0x1d00ffff,
                        block_hash, chain_work);
    mmb_append(&mmb, &leaf);
    len = mmb_serialize(&mmb, buf, sizeof(buf));
    node_db_state_set(ndb, "mmb_state", buf, len);
}

static bool seed_snapshot_manifest_db(struct node_db *ndb,
                                      const uint8_t block_hash[32],
                                      const uint8_t chain_work[32])
{
    uint8_t prev_hash[32];
    uint8_t merkle_root[32];
    uint8_t nonce[32];
    uint8_t solution[1] = {0};
    sqlite3_stmt *st = NULL;

    memset(prev_hash, 0x40, sizeof(prev_hash));
    memset(merkle_root, 0x42, sizeof(merkle_root));
    memset(nonce, 0x43, sizeof(nonce));

    if (!node_db_state_set_int(ndb, "tip_height", 101) ||
        !node_db_state_set(ndb, "tip_hash", block_hash, 32))
        return false;
    if (sqlite3_prepare_v2(ndb->db,
            "INSERT INTO blocks "
            "(hash,height,prev_hash,version,merkle_root,time,bits,nonce,"
            "solution,chain_work,status) "
            "VALUES (?,?,?,?,?,?,?,?,?,?,3)",
            -1, &st, NULL) != SQLITE_OK)
        return false;
    sqlite3_bind_blob(st, 1, block_hash, 32, SQLITE_STATIC);
    sqlite3_bind_int(st, 2, 101);
    sqlite3_bind_blob(st, 3, prev_hash, 32, SQLITE_STATIC);
    sqlite3_bind_int(st, 4, 4);
    sqlite3_bind_blob(st, 5, merkle_root, 32, SQLITE_STATIC);
    sqlite3_bind_int(st, 6, 1234567890);
    sqlite3_bind_int(st, 7, 0x1d00ffff);
    sqlite3_bind_blob(st, 8, nonce, 32, SQLITE_STATIC);
    sqlite3_bind_blob(st, 9, solution, sizeof(solution), SQLITE_STATIC);
    sqlite3_bind_blob(st, 10, chain_work, 32, SQLITE_STATIC);
    bool ok = sqlite3_step(st) == SQLITE_DONE;
    sqlite3_finalize(st);
    if (!ok)
        return false;
    if (!node_db_exec(ndb,
            "INSERT INTO utxos "
            "(txid,vout,value,script,script_type,height,is_coinbase) "
            "VALUES (X'0102030405060708090A0B0C0D0E0F101112131415161718"
            "191A1B1C1D1E1F20',0,5000,X'51',0,101,0)"))
        return false;
    persist_snapshot_roots(ndb, block_hash, chain_work);
    return true;
}

int test_utxo_activation_paused(void)
{
    printf("\n=== utxo activation paused condition tests ===\n");
    int failures = 0;

    {
        reset_conditions();
        struct fake_clock clock;
        fake_clock_install(&clock, 1000);
        bool ok = true;

        struct main_state ms;
        main_state_init(&ms);
        struct uint256 hashes[2];
        struct block_index *tip = insert_test_block(
            &ms, hashes, 0, BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA);
        struct block_index *next = insert_test_block(
            &ms, hashes, 1, BLOCK_HAVE_DATA);
        ok = ok && tip && next && active_chain_move_window_tip(&ms.chain_active, tip);
        condition_engine_set_main_state(&ms);
        register_block_failed_mask_at_tip();

        condition_engine_tick();
        fake_clock_set(&clock, 1301);
        condition_engine_tick();
        ok = ok && condition_engine_get_active_count() == 1;
        ok = ok && block_failed_mask_at_tip_test_stall_type() == 2;
        ok = ok && active_chain_move_window_tip(&ms.chain_active, next);
        fake_clock_set(&clock, 1302);
        condition_engine_tick();
        ok = ok && condition_engine_get_active_count() == 0;

        UAP_CHECK("block condition fires on stale tip with HAVE_DATA", ok);
        main_state_free(&ms);
        clock_reset_default();
    }

    {
        reset_conditions();
        struct fake_clock clock;
        fake_clock_install(&clock, 2000);
        bool ok = true;
        register_utxo_activation_paused();
        process_block_test_set_utxo_activation_paused_height(1500);

        condition_engine_tick();
        fake_clock_set(&clock, 2299);
        condition_engine_tick();
        ok = ok && condition_engine_get_active_count() == 0;
        ok = ok && process_block_test_get_utxo_activation_paused_height() == 1500;
        UAP_CHECK("detect does not fire before 300s", ok);
        clock_reset_default();
    }

    {
        reset_conditions();
        struct fake_clock clock;
        fake_clock_install(&clock, 3000);
        bool ok = true;
        /* Honest witness (Law 7) requires the chain tip to have ACTUALLY
         * advanced past the paused height — not just the pause flag cleared.
         * Stand up a real chain whose tip sits at the paused height so the
         * resumed activation observably reached it. */
        struct main_state ms;
        main_state_init(&ms);
        struct uint256 hashes[2];
        struct block_index *b0 = insert_test_block(
            &ms, hashes, 0, BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA);
        struct block_index *b1 = insert_test_block(
            &ms, hashes, 1, BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA);
        b1->nHeight = 1600;
        ok = ok && b0 && b1 &&
             active_chain_move_window_tip(&ms.chain_active, b1);
        condition_engine_set_main_state(&ms);
        register_utxo_activation_paused();
        process_block_test_set_utxo_activation_paused_height(1600);

        condition_engine_tick();
        fake_clock_set(&clock, 3301);
        condition_engine_tick();
        ok = ok && utxo_activation_paused_test_resume_calls() == 1;
        ok = ok && utxo_activation_paused_test_repair_calls() == 0;
        ok = ok && process_block_test_get_utxo_activation_paused_height() == -1;
        ok = ok && condition_engine_get_active_count() == 0;
        UAP_CHECK("resume remedy clears pause", ok);
        main_state_free(&ms);
        clock_reset_default();
    }

    {
        reset_conditions();
        struct fake_clock clock;
        fake_clock_install(&clock, 4000);
        bool ok = true;
        struct main_state ms;
        main_state_init(&ms);
        struct uint256 hashes[2];
        struct block_index *b0 = insert_test_block(
            &ms, hashes, 0, BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA);
        struct block_index *b1 = insert_test_block(
            &ms, hashes, 1, BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA);
        b1->nHeight = 1700;
        ok = ok && b0 && b1 &&
             active_chain_move_window_tip(&ms.chain_active, b1);
        condition_engine_set_main_state(&ms);
        register_utxo_activation_paused();
        utxo_activation_paused_test_set_reason("utxo_audit_drift");
        process_block_test_set_utxo_activation_paused_height(1700);

        condition_engine_tick();
        fake_clock_set(&clock, 4301);
        condition_engine_tick();
        ok = ok && utxo_activation_paused_test_resume_calls() == 0;
        ok = ok && utxo_activation_paused_test_repair_calls() == 1;
        ok = ok && process_block_test_get_utxo_activation_paused_height() == -1;
        UAP_CHECK("drift reason takes repair path", ok);
        main_state_free(&ms);
        clock_reset_default();
    }

    {
        reset_conditions();
        struct fake_clock clock;
        fake_clock_install(&clock, 5000);
        _Atomic int operator_events;
        atomic_store(&operator_events, 0);
        bool ok = true;
        event_observe(EV_OPERATOR_NEEDED, operator_observer,
                      &operator_events);
        register_utxo_activation_paused();
        utxo_activation_paused_test_set_remedy_clear_enabled(false);
        process_block_test_set_utxo_activation_paused_height(1800);

        condition_engine_tick();
        fake_clock_set(&clock, 5301);
        condition_engine_tick();
        fake_clock_set(&clock, 5332);
        condition_engine_tick();
        ok = ok && condition_engine_get_unresolved_count() == 1;
        ok = ok && atomic_load(&operator_events) >= 1;
        UAP_CHECK("max attempts emits operator event", ok);
        clock_reset_default();
    }

    {
        reset_conditions();
        struct fake_clock clock;
        fake_clock_install(&clock, 6000);
        bool ok = true;
        struct main_state ms;
        struct block_index tip;
        struct block_index recovered_tip;
        struct block_index best_header;
        struct node_db ndb;
        struct snapshot_sync_service svc;
        uint8_t block_hash[32];
        uint8_t chain_work[32];
        struct json_value root;
        const struct json_value *conditions;
        const struct json_value *condition;
        const struct json_value *attempts;
        const struct json_value *last_outcome;
        struct watchdog_stats wd;

        memset(&tip, 0, sizeof(tip));
        memset(&recovered_tip, 0, sizeof(recovered_tip));
        memset(&best_header, 0, sizeof(best_header));
        memset(block_hash, 0x51, sizeof(block_hash));
        memset(chain_work, 0x55, sizeof(chain_work));
        main_state_init(&ms);
        tip.nHeight = 100;
        tip.nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
        recovered_tip.nHeight = 101;
        recovered_tip.nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
        recovered_tip.pprev = &tip;
        best_header.nHeight = 110;
        ms.pindex_best_header = &best_header;
        ok = ok && active_chain_move_window_tip(&ms.chain_active, &tip);

        ok = ok && node_db_open(&ndb, ":memory:");
        ok = ok && seed_snapshot_manifest_db(&ndb, block_hash, chain_work);
        snapsync_init(&svc, &ndb);

        condition_engine_set_main_state(&ms);
        sync_monitor_init();
        sync_monitor_set_context(NULL, NULL, &ms);
        tip_wedged_resnapshot_test_set_runtime(&ndb, &svc);
        block_failed_mask_at_tip_test_mark_exhausted(101);
        register_tip_wedged_resnapshot();

        condition_engine_tick();
        bool observed = true;
        observed = observed && tip_wedged_resnapshot_test_remedy_calls() == 1;
        observed = observed && tip_wedged_resnapshot_test_recovery_accepted() == 1;
        observed = observed &&
                   tip_wedged_resnapshot_test_last_manifest_height() == 101;
        observed = observed && svc.state == SNAPSYNC_NEGOTIATING;
        observed = observed && condition_engine_get_active_count() == 1;
        sync_monitor_get_stats(&wd);
        observed = observed &&
                   wd.last_recovery == WATCHDOG_SNAPSHOT_RESNAPSHOT;
        observed = observed && wd.last_recovery_local_height == 100;
        observed = observed && wd.last_recovery_peer_height == 110;
        observed = observed && wd.last_recovery_target_height == 101;
        observed = observed && wd.last_recovery_manifest_height == 101;
        observed = observed &&
                   strcmp(wd.last_recovery_trigger,
                          "block_failed_mask_exhausted") == 0;

        json_init(&root);
        json_set_object(&root);
        bool dump_ok = condition_engine_dump_state_json(&root, NULL);
        observed = observed && dump_ok;
        conditions = json_get(&root, "conditions");
        condition = conditions ? json_at(conditions, 0) : NULL;
        attempts = condition ? json_get(condition, "attempts") : NULL;
        last_outcome = condition ? json_get(condition, "last_outcome") : NULL;
        const struct json_value *name =
            condition ? json_get(condition, "name") : NULL;
        observed = observed && condition != NULL;
        observed = observed && name &&
                   strcmp(json_get_str(name),
                          "tip_wedged_resnapshot") == 0;
        observed = observed && attempts && json_get_int(attempts) == 1;
        observed = observed && last_outcome &&
                   strcmp(json_get_str(last_outcome), "unwitnessed") == 0;

        ok = ok && active_chain_move_window_tip(&ms.chain_active, &recovered_tip);
        fake_clock_set(&clock, 6001);
        condition_engine_tick();
        observed = observed && condition_engine_get_active_count() == 0;
        ok = ok && observed;
        json_free(&root);

        UAP_CHECK("tip_wedged_resnapshot requests snapshot recovery", ok);
        snapsync_reset(&svc);
        node_db_close(&ndb);
        main_state_free(&ms);
        clock_reset_default();
    }

    {
        reset_conditions();
        struct fake_clock clock;
        fake_clock_install(&clock, 7000);
        bool ok = true;
        struct main_state ms;
        struct block_index tip;
        struct block_index best_header;
        struct node_db ndb;
        struct snapshot_sync_service svc;
        uint8_t block_hash[32];
        uint8_t chain_work[32];
        struct watchdog_stats wd;

        memset(&tip, 0, sizeof(tip));
        memset(&best_header, 0, sizeof(best_header));
        memset(block_hash, 0x52, sizeof(block_hash));
        memset(chain_work, 0x56, sizeof(chain_work));
        main_state_init(&ms);
        tip.nHeight = 100;
        tip.nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
        best_header.nHeight = 110;
        ms.pindex_best_header = &best_header;
        ok = ok && active_chain_move_window_tip(&ms.chain_active, &tip);

        ok = ok && node_db_open(&ndb, ":memory:");
        ok = ok && seed_snapshot_manifest_db(&ndb, block_hash, chain_work);
        snapsync_init(&svc, &ndb);

        condition_engine_set_main_state(&ms);
        sync_monitor_init();
        sync_monitor_set_context(NULL, NULL, &ms);
        sync_monitor_test_set_local_recovery(true, true, 101, 3,
                                             "next-child-missing");
        tip_wedged_resnapshot_test_set_runtime(&ndb, &svc);
        register_tip_wedged_resnapshot();

        condition_engine_tick();
        ok = ok && tip_wedged_resnapshot_test_remedy_calls() == 1;
        ok = ok && tip_wedged_resnapshot_test_recovery_accepted() == 1;
        ok = ok && tip_wedged_resnapshot_test_last_manifest_height() == 101;
        ok = ok && svc.state == SNAPSYNC_NEGOTIATING;
        ok = ok && condition_engine_get_active_count() == 1;
        sync_monitor_get_stats(&wd);
        ok = ok && wd.last_recovery == WATCHDOG_SNAPSHOT_RESNAPSHOT;
        ok = ok && wd.last_recovery_target_height == 101;
        ok = ok && wd.last_recovery_manifest_height == 101;
        ok = ok && strcmp(wd.last_recovery_trigger,
                          "local_import_exhausted") == 0;

        UAP_CHECK("tip_wedged_resnapshot uses exhausted local import", ok);
        snapsync_reset(&svc);
        node_db_close(&ndb);
        main_state_free(&ms);
        clock_reset_default();
    }

    {
        reset_conditions();
        struct fake_clock clock;
        fake_clock_install(&clock, 8000);
        bool ok = true;
        struct main_state ms;
        struct block_index tip;
        struct block_index best_header;
        struct node_db ndb;
        struct snapshot_sync_service svc;
        uint8_t block_hash[32];
        uint8_t chain_work[32];

        memset(&tip, 0, sizeof(tip));
        memset(&best_header, 0, sizeof(best_header));
        memset(block_hash, 0x53, sizeof(block_hash));
        memset(chain_work, 0x57, sizeof(chain_work));
        main_state_init(&ms);
        tip.nHeight = 100;
        tip.nStatus = BLOCK_VALID_SCRIPTS | BLOCK_HAVE_DATA;
        best_header.nHeight = 110;
        ms.pindex_best_header = &best_header;
        ok = ok && active_chain_move_window_tip(&ms.chain_active, &tip);

        ok = ok && node_db_open(&ndb, ":memory:");
        ok = ok && seed_snapshot_manifest_db(&ndb, block_hash, chain_work);
        snapsync_init(&svc, &ndb);
        svc.state = SNAPSYNC_NEGOTIATING;

        condition_engine_set_main_state(&ms);
        sync_monitor_init();
        sync_monitor_set_context(NULL, NULL, &ms);
        tip_wedged_resnapshot_test_set_runtime(&ndb, &svc);
        block_failed_mask_at_tip_test_mark_exhausted(101);
        register_tip_wedged_resnapshot();

        condition_engine_tick();
        ok = ok && tip_wedged_resnapshot_test_remedy_calls() == 0;
        ok = ok && tip_wedged_resnapshot_test_recovery_accepted() == 0;
        ok = ok && svc.state == SNAPSYNC_NEGOTIATING;
        ok = ok && condition_engine_get_active_count() == 0;

        UAP_CHECK("tip_wedged_resnapshot waits for idle snapshot sync", ok);
        snapsync_reset(&svc);
        node_db_close(&ndb);
        main_state_free(&ms);
        clock_reset_default();
    }

    reset_conditions();
    clock_reset_default();
    return failures;
}

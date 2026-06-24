/* Unit tests for coins_ram — the flag-gated in-RAM UTXO hot store for the bulk
 * fold (storage/coins_ram.h).
 *
 * THE load-bearing assertion: the SHA3 commitment over the EFFECTIVE set
 * (coins_ram_commitment, RAM overlay merged over the durable coins_kv) is
 * BYTE-IDENTICAL to coins_kv_commitment over the same logical set folded
 * straight into SQLite. That identity is what keeps the from-genesis fold
 * SELF-VERIFYING against the compiled checkpoint when the in-RAM store is on.
 *
 * Also covers: read-through (a cold miss reads the durable set), tombstone
 * shadowing (a spend of a durable coin reads ABSENT before flush), flush
 * durability (after flush the overlay is empty and coins_kv holds the truth),
 * and the crash-replay watermark (coins_ram_reconcile_boot rewinds an ahead
 * cursor to the last durable flush).
 *
 * The flag is read once from ZCL_FOLD_INRAM and cached, so this whole group
 * runs in the flag-ON process (test_parallel forks per group).
 *
 * THE identity proof is self-contained: the commitment over the FULL overlay
 * (nothing durable yet) must equal the commitment AFTER a flush (everything
 * durable, overlay empty — i.e. the pure SQLite coins_kv_commitment path). If
 * the overlay-merge encoder and the SQLite encoder ever diverged a byte, these
 * two roots would differ. */

#include "test/test_helpers.h"

#include "coins/coins.h"
#include "core/uint256.h"
#include "script/script.h"
#include "storage/coins_kv.h"
#include "storage/coins_ram.h"
#include "storage/progress_store.h"

#include <sqlite3.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CR_CHECK(name, expr) do {                                       \
    if (expr) { printf("  coins_ram: %s... OK\n", (name)); }            \
    else { printf("  coins_ram: %s... FAIL\n", (name)); failures++; }   \
} while (0)

static struct uint256 cr_txid(uint8_t tag)
{
    struct uint256 t; uint256_set_null(&t);
    t.data[0] = tag; t.data[1] = 0x5A; t.data[31] = 0xA5;
    return t;
}

/* One logical mutation in a deterministic fold script. */
struct fold_op { bool spend; uint8_t tag; uint32_t vout; int64_t value;
                 int32_t height; bool cb; uint8_t script[8]; size_t slen; };

/* A from-"genesis" fold script that creates several coins, then spends some,
 * including a create-then-spend within the same window (must NOT survive). */
static const struct fold_op SCRIPT[] = {
    { false, 0x01, 0, 5000, 10, true,  {0xA0,0xA1},      2 },
    { false, 0x01, 1, 6000, 10, true,  {0xB0,0xB1,0xB2}, 3 },
    { false, 0x02, 0, 7000, 11, false, {0xC0},           1 },
    { false, 0x03, 0, 8000, 12, false, {0},              0 },  /* empty script */
    { false, 0x04, 0, 9000, 13, false, {0xD0,0xD1},      2 },
    { true,  0x02, 0, 0,    0,  false, {0},              0 },  /* spend a live coin */
    { false, 0x05, 0, 1234, 14, false, {0xE0},           1 },
    { true,  0x05, 0, 0,    0,  false, {0},              0 },  /* create-then-spend */
    { false, 0x04, 1, 4321, 15, false, {0xF0,0xF1,0xF2}, 3 },
    { true,  0x01, 0, 0,    0,  false, {0},              0 },  /* spend coinbase out */
};
#define SCRIPT_N (sizeof(SCRIPT) / sizeof(SCRIPT[0]))

/* Apply the script through the PUBLIC coins_kv API (overlay-routed when the
 * flag is on, raw SQLite when off). */
static void apply_script_public(sqlite3 *db)
{
    for (size_t i = 0; i < SCRIPT_N; i++) {
        const struct fold_op *o = &SCRIPT[i];
        struct uint256 t = cr_txid(o->tag);
        if (o->spend)
            coins_kv_spend(db, t.data, o->vout);
        else
            coins_kv_add(db, t.data, o->vout, o->value, o->height, o->cb,
                         o->slen ? o->script : NULL, o->slen);
    }
}

/* ── A/B + multi-flush extension scaffolding ───────────────────────────────
 *
 * The original test above proves the overlay-vs-flushed IDENTITY within ONE
 * overlay leg. The two sections that follow close the gaps the task names:
 *  (a) a TRUE flag-OFF (pure-SQLite, overlay never initialised) leg vs a
 *      flag-ON overlay leg, asserting coins_kv_commitment() is byte-identical.
 *  (b) MULTI-FLUSH: overlay merged over an already-populated durable set, with
 *      cross-flush tombstones (spend of a coin that was flushed in a PRIOR
 *      flush) and a re-add over a durably-flushed key.
 *
 * Both reuse a wider op type than `struct fold_op`: a full 32-byte txid is
 * derived from a u32 tag so we can mint many distinct coins, and `flush==true`
 * marks a flush boundary (a no-op on the pure-SQLite leg, a real
 * coins_ram_flush on the overlay leg). */
struct ab_op {
    enum { AB_ADD, AB_SPEND, AB_FLUSH } kind;
    uint32_t tag;        /* seeds the txid */
    uint32_t vout;
    int64_t  value;
    int32_t  height;     /* also used as the flush watermark for OP_FLUSH */
    bool     cb;
    uint8_t  script[12];
    size_t   slen;
};

/* Deterministic 32-byte txid from a u32 tag — spreads bytes so the open-
 * addressed probe and the SQLite WITHOUT ROWID B-tree both see well-mixed
 * keys (and so distinct tags never collide). */
static struct uint256 ab_txid(uint32_t tag)
{
    struct uint256 t; uint256_set_null(&t);
    t.data[0]  = (uint8_t)(tag);
    t.data[1]  = (uint8_t)(tag >> 8);
    t.data[2]  = (uint8_t)(tag >> 16);
    t.data[3]  = (uint8_t)(tag >> 24);
    t.data[7]  = 0x5A;
    t.data[15] = (uint8_t)(tag * 2654435761u);          /* knuth mix */
    t.data[23] = (uint8_t)((tag * 2654435761u) >> 8);
    t.data[31] = 0xA5;
    return t;
}

/* Apply ONE op through the public coins_kv API. On the overlay leg the caller
 * passes overlay=true so OP_FLUSH drains the overlay; on the pure-SQLite leg
 * overlay=false and OP_FLUSH is a no-op (rows are already durable). Returns
 * false on any failed mutation/flush. */
static bool ab_apply_one(sqlite3 *db, const struct ab_op *o, bool overlay)
{
    struct uint256 t = ab_txid(o->tag);
    switch (o->kind) {
    case AB_ADD:
        return coins_kv_add(db, t.data, o->vout, o->value, o->height, o->cb,
                            o->slen ? o->script : NULL, o->slen);
    case AB_SPEND:
        return coins_kv_spend(db, t.data, o->vout);
    case AB_FLUSH:
        if (overlay) return coins_ram_flush(o->height);
        return true;  /* pure-SQLite: nothing buffered, already durable */
    }
    return false;
}

/* Run the whole op list. `overlay` chooses the leg. Returns the number of
 * failed ops (0 == clean). */
static int ab_apply_all(sqlite3 *db, const struct ab_op *ops, size_t n,
                        bool overlay)
{
    int fails = 0;
    for (size_t i = 0; i < n; i++)
        if (!ab_apply_one(db, &ops[i], overlay)) fails++;
    return fails;
}

int test_coins_ram(void);
int test_coins_ram(void)
{
    int failures = 0;

    /* Force the flag ON before the first coins_ram_enabled() (cached once). */
    setenv("ZCL_FOLD_INRAM", "1", 1);
    CR_CHECK("flag enabled", coins_ram_enabled());

    /* ── overlay fold: bind the overlay to a coins_kv handle and apply via the
     *    PUBLIC API (routed into the overlay) ── */
    char dir[256];
    test_make_tmpdir(dir, sizeof(dir), "coins_ram_main", "main");
    CR_CHECK("ov: progress_store opens", progress_store_open(dir));
    sqlite3 *db = progress_store_db();
    CR_CHECK("ov: db handle", db != NULL);
    CR_CHECK("ov: schema", coins_kv_ensure_schema(db));
    CR_CHECK("ov: init", coins_ram_init(db, 1000000 /* never auto-flush */));
    CR_CHECK("ov: active", coins_ram_active());

    apply_script_public(db);

    /* The overlay is the effective set. Compute its commitment. */
    uint8_t ov_root[32] = {0};
    CR_CHECK("ov: commitment ok", coins_kv_commitment(db, ov_root) == 0);

    /* Determinism / stability: a second commitment over the unchanged overlay
     * must be identical. */
    uint8_t ov_root2[32] = {0};
    CR_CHECK("ov: commitment stable", coins_kv_commitment(db, ov_root2) == 0 &&
             memcmp(ov_root, ov_root2, 32) == 0);

    /* Read semantics over the effective set (before any flush): */
    int64_t v = 0; size_t sl = 0; uint8_t sbuf[16] = {0};
    struct uint256 t1 = cr_txid(0x01), t2 = cr_txid(0x02), t4 = cr_txid(0x04),
                   t5 = cr_txid(0x05);
    CR_CHECK("read: t1.0 spent -> absent",
             !coins_kv_get(db, t1.data, 0, &v, sbuf, sizeof(sbuf), &sl));
    CR_CHECK("read: t1.1 live",
             coins_kv_get(db, t1.data, 1, &v, sbuf, sizeof(sbuf), &sl) &&
             v == 6000 && sl == 3);
    CR_CHECK("read: t2.0 spent -> absent",
             !coins_kv_exists(db, t2.data, 0));
    CR_CHECK("read: t4.0 live", coins_kv_exists(db, t4.data, 0));
    CR_CHECK("read: t4.1 live", coins_kv_exists(db, t4.data, 1));
    CR_CHECK("read: t5.0 create-then-spend -> absent",
             !coins_kv_exists(db, t5.data, 0));

    /* Effective live count: created 7 outputs (t1.0,t1.1,t2.0,t3.0,t4.0,t5.0,
     * t4.1), spent t2.0,t5.0,t1.0 -> 4 live. */
    CR_CHECK("count: 4 live before flush", coins_kv_count(db) == 4);

    /* get_coins reconstruction for t4 (two live vouts 0 and 1). */
    struct coins c4; coins_init(&c4);
    bool g4 = coins_kv_get_coins(db, t4.data, &c4);
    CR_CHECK("get_coins: t4 has >=2 vouts", g4 && c4.num_vout >= 2 &&
             coins_is_available(&c4, 0) && coins_is_available(&c4, 1));
    coins_free(&c4);

    /* ── coins_ram_get_prevout: O(1) point resolver must return the SAME four
     *    fields (value/script/height/is_coinbase) as the get_coins
     *    reconstruction for a LIVE overlay coin, ABSENT for a spent/tombstoned
     *    coin, and ABSENT for a never-created vout. This is the fast path
     *    projection_live_lookup now routes through; it must be byte-identical
     *    to the reconstruction path it replaces. ── */
    {
        int64_t  pv = 0; size_t pl = 0; int32_t ph = -1; bool pcb = true;
        uint8_t  pbuf[16] = {0};
        /* t4.1 is LIVE in the overlay: value=4321, height=15, cb=false,
         * script {0xF0,0xF1,0xF2} (3 bytes) — from SCRIPT[8]. */
        bool h41 = coins_ram_get_prevout(t4.data, 1, &pv, pbuf, sizeof(pbuf),
                                         &pl, &ph, &pcb);
        CR_CHECK("prevout: t4.1 live hit", h41);
        CR_CHECK("prevout: t4.1 value==4321", h41 && pv == 4321);
        CR_CHECK("prevout: t4.1 script_len==3", h41 && pl == 3);
        CR_CHECK("prevout: t4.1 script bytes",
                 h41 && pbuf[0] == 0xF0 && pbuf[1] == 0xF1 && pbuf[2] == 0xF2);
        CR_CHECK("prevout: t4.1 height==15", h41 && ph == 15);
        CR_CHECK("prevout: t4.1 is_coinbase==false", h41 && !pcb);

        /* t4.0 is LIVE: value=9000, height=13, cb=false, script {0xD0,0xD1}. */
        int64_t  pv0 = 0; size_t pl0 = 0; int32_t ph0 = -1; bool pcb0 = true;
        uint8_t  pbuf0[16] = {0};
        bool h40 = coins_ram_get_prevout(t4.data, 0, &pv0, pbuf0, sizeof(pbuf0),
                                         &pl0, &ph0, &pcb0);
        CR_CHECK("prevout: t4.0 live hit", h40 && pv0 == 9000 && ph0 == 13 &&
                 !pcb0 && pl0 == 2 && pbuf0[0] == 0xD0 && pbuf0[1] == 0xD1);

        /* t1.0 was SPENT (tombstoned) -> ABSENT. */
        int32_t junk_h = 7; bool junk_cb = true;
        bool h10 = coins_ram_get_prevout(t1.data, 0, NULL, NULL, 0, NULL,
                                         &junk_h, &junk_cb);
        CR_CHECK("prevout: t1.0 spent -> absent", !h10);

        /* t5.0 create-then-spend -> ABSENT. */
        bool h50 = coins_ram_get_prevout(t5.data, 0, NULL, NULL, 0, NULL,
                                         NULL, NULL);
        CR_CHECK("prevout: t5.0 create-then-spend -> absent", !h50);

        /* never-created vout on a live txid -> ABSENT. */
        bool h4x = coins_ram_get_prevout(t4.data, 99, NULL, NULL, 0, NULL,
                                         NULL, NULL);
        CR_CHECK("prevout: t4.99 never-created -> absent", !h4x);
    }

    /* ── FLUSH: drain the overlay into coins_kv, then the overlay is empty and
     *    reads fall through to the (now-complete) durable set. ── */
    uint8_t pre_flush_root[32];
    memcpy(pre_flush_root, ov_root, 32);
    CR_CHECK("flush: ok", coins_ram_flush(20));

    /* After the flush the overlay holds nothing; coins_kv_count now reads the
     * durable set directly (overlay still active but empty). */
    CR_CHECK("count: 4 live after flush", coins_kv_count(db) == 4);

    /* Commitment IDENTITY: the post-flush effective commitment (overlay empty,
     * all rows durable) must equal the pre-flush effective commitment (overlay
     * full, nothing durable). This is the from-genesis self-verify guarantee:
     * the SHA3 is independent of WHEN the overlay was flushed. */
    uint8_t post_flush_root[32] = {0};
    CR_CHECK("commitment ok post-flush",
             coins_kv_commitment(db, post_flush_root) == 0);
    CR_CHECK("IDENTITY: pre-flush root == post-flush root",
             memcmp(pre_flush_root, post_flush_root, 32) == 0);

    /* The durable cursor + watermark moved to the flushed height. */
    int32_t applied = -1; bool found = false;
    CR_CHECK("applied frontier read",
             coins_kv_get_applied_height(db, &applied, &found));
    CR_CHECK("applied frontier == flushed+1", found && applied == 21);

    /* ── crash-replay reconcile: simulate the cursor advancing PAST the last
     *    flush (a crash between flushes), then reconcile must rewind it. ── */
    {
        progress_store_tx_lock();
        sqlite3_exec(db, "BEGIN IMMEDIATE", NULL, NULL, NULL);
        sqlite3_stmt *s = NULL;
        sqlite3_prepare_v2(db,
            "INSERT INTO stage_cursor(name,cursor,updated_at) "
            "VALUES('utxo_apply', 99, 0) "
            "ON CONFLICT(name) DO UPDATE SET cursor=99", -1, &s, NULL);
        sqlite3_step(s); sqlite3_finalize(s);
        sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
        progress_store_tx_unlock();
    }
    CR_CHECK("reconcile: ok", coins_ram_reconcile_boot(db));
    {
        progress_store_tx_lock();
        sqlite3_stmt *s = NULL;
        int64_t cur = -1;
        if (sqlite3_prepare_v2(db,
                "SELECT cursor FROM stage_cursor WHERE name='utxo_apply'",
                -1, &s, NULL) == SQLITE_OK) {
            if (sqlite3_step(s) == SQLITE_ROW) cur = sqlite3_column_int64(s, 0);
            sqlite3_finalize(s);
        }
        progress_store_tx_unlock();
        /* watermark was 20 -> cursor rewound to 21 (watermark+1). */
        CR_CHECK("reconcile: cursor rewound to watermark+1 (21)", cur == 21);
    }

    coins_ram_shutdown();
    progress_store_close();
    test_rm_rf_recursive(dir);

    /* ──────────────────────────────────────────────────────────────────────
     * (a) TRUE A/B: identical op script, pure-SQLite leg vs overlay leg.
     *
     * The cached flag (ZCL_FOLD_INRAM, read once by coins_ram_enabled) does NOT
     * gate the routing — coins_kv.c routes on coins_ram_active(), which is
     * (G.active && G.slots). So a leg that NEVER calls coins_ram_init runs the
     * byte-for-byte original SQLite path even with the env flag set. That gives
     * a genuine cross-encoder A/B inside this one (flag-ON) process:
     *   leg A: no coins_ram_init  -> coins_kv_* all hit pure SQLite, and
     *          coins_kv_commitment iterates the durable B-tree directly.
     *   leg B: coins_ram_init     -> coins_kv_* route through the overlay, and
     *          coins_kv_commitment iterates the EFFECTIVE (overlay+durable) set.
     * The op script includes spends, a create-then-spend, an empty script, and
     * an INSERT-OR-REPLACE over a live key — all the encoder corner cases.
     * ────────────────────────────────────────────────────────────────────── */
    {
        static const struct ab_op AB[] = {
            { AB_ADD,100, 0, 5000, 10, true,  {0xA0,0xA1},      2, },
            { AB_ADD,100, 1, 6000, 10, true,  {0xB0,0xB1,0xB2}, 3, },
            { AB_ADD,101, 0, 7000, 11, false, {0xC0},           1, },
            { AB_ADD,102, 0, 8000, 12, false, {0},              0, }, /* empty */
            { AB_ADD,103, 0, 9000, 13, false, {0xD0,0xD1},      2, },
            { AB_SPEND,101, 0, 0,    0,  false, {0},              0, },
            { AB_ADD,104, 0, 1234, 14, false, {0xE0},           1, },
            { AB_SPEND,104, 0, 0,    0,  false, {0},              0, }, /* create+spend */
            { AB_ADD,103, 1, 4321, 15, false, {0xF0,0xF1,0xF2}, 3, },
            { AB_ADD,100, 0, 5555, 16, false, {0x11},           1, }, /* REPLACE live */
            { AB_SPEND,100, 1, 0,    0,  false, {0},              0, },
            { AB_ADD,105, 7, 222,  17, false, {0x22,0x33},      2, },
        };
        const size_t AB_N = sizeof(AB) / sizeof(AB[0]);

        /* leg A — pure SQLite (overlay never initialised). */
        char dirA[256];
        test_make_tmpdir(dirA, sizeof(dirA), "coins_ram_ab_a", "a");
        CR_CHECK("ab.A: open", progress_store_open(dirA));
        sqlite3 *dbA = progress_store_db();
        CR_CHECK("ab.A: schema", coins_kv_ensure_schema(dbA));
        CR_CHECK("ab.A: overlay NOT active (pure sqlite)", !coins_ram_active());
        CR_CHECK("ab.A: apply clean", ab_apply_all(dbA, AB, AB_N, false) == 0);
        uint8_t rootA[32] = {0};
        CR_CHECK("ab.A: commitment ok", coins_kv_commitment(dbA, rootA) == 0);
        int64_t cntA = coins_kv_count(dbA);
        progress_store_close();
        test_rm_rf_recursive(dirA);

        /* leg B — overlay. Same ops, with a flush in the middle and at the end
         * so the commitment is taken once with rows split overlay/durable and
         * once fully drained. Both must equal leg A. */
        char dirB[256];
        test_make_tmpdir(dirB, sizeof(dirB), "coins_ram_ab_b", "b");
        CR_CHECK("ab.B: open", progress_store_open(dirB));
        sqlite3 *dbB = progress_store_db();
        CR_CHECK("ab.B: schema", coins_kv_ensure_schema(dbB));
        CR_CHECK("ab.B: init", coins_ram_init(dbB, 1000000 /* manual flush */));
        CR_CHECK("ab.B: overlay active", coins_ram_active());
        CR_CHECK("ab.B: apply clean", ab_apply_all(dbB, AB, AB_N, true) == 0);

        /* Commitment with the overlay STILL holding everything (nothing
         * durable yet) — the cross-encoder path. */
        uint8_t rootB_overlay[32] = {0};
        CR_CHECK("ab.B: commitment (overlay-full) ok",
                 coins_kv_commitment(dbB, rootB_overlay) == 0);
        int64_t cntB_overlay = coins_kv_count(dbB);

        /* Now flush and re-take: rows fully durable, overlay empty. */
        CR_CHECK("ab.B: flush ok", coins_ram_flush(20));
        uint8_t rootB_flushed[32] = {0};
        CR_CHECK("ab.B: commitment (flushed) ok",
                 coins_kv_commitment(dbB, rootB_flushed) == 0);
        int64_t cntB_flushed = coins_kv_count(dbB);

        coins_ram_shutdown();
        progress_store_close();
        test_rm_rf_recursive(dirB);

        /* THE cross-encoder proofs. */
        CR_CHECK("AB: count A == B(overlay) == B(flushed)",
                 cntA == cntB_overlay && cntA == cntB_flushed);
        CR_CHECK("AB: root A == B(overlay)  [pure-sqlite == overlay-effective]",
                 memcmp(rootA, rootB_overlay, 32) == 0);
        CR_CHECK("AB: root A == B(flushed)  [pure-sqlite == drained-overlay]",
                 memcmp(rootA, rootB_flushed, 32) == 0);
    }

    /* ──────────────────────────────────────────────────────────────────────
     * (b) MULTI-FLUSH: the dominant real case the original test never hit —
     *     the overlay merged over an ALREADY-POPULATED durable set across many
     *     flush rounds, with cross-flush tombstones (spend of a coin durable
     *     since a PRIOR flush) and a re-add over a durably-flushed key.
     *
     * We fold the SAME op stream two ways and compare the FINAL commitment +
     * per-coin reads:
     *   golden  : pure SQLite (overlay off) — flushes are no-ops.
     *   overlay : flushes are real coins_ram_flush calls interleaved through
     *             the stream, so later ops mutate over a populated durable set.
     * ────────────────────────────────────────────────────────────────────── */
    {
        /* Round 1: mint a base set.            (tags 200..205)
         * flush.
         * Round 2: spend two NOW-DURABLE coins (cross-flush tombstone),
         *          spend a coin minted+spent in THIS round (overlay-only),
         *          re-add (REPLACE) a durably-flushed key,
         *          mint fresh coins.
         * flush.
         * Round 3: spend a coin minted in round 2 (durable now),
         *          spend a coin that was re-added in round 2 (durable now),
         *          mint more.
         * flush.
         * Round 4: spend across all prior rounds + mint, NO trailing flush so
         *          the final commitment is taken with a non-empty overlay over
         *          a populated durable set (the live mint's actual posture). */
        static const struct ab_op MF[] = {
            /* round 1 */
            { AB_ADD,200, 0, 1000, 1, true,  {0x01},           1, },
            { AB_ADD,200, 1, 1100, 1, true,  {0x02,0x03},      2, },
            { AB_ADD,201, 0, 1200, 2, false, {0x04},           1, },
            { AB_ADD,202, 0, 1300, 3, false, {0},              0, },
            { AB_ADD,203, 0, 1400, 4, false, {0x05,0x06,0x07}, 3, },
            { AB_ADD,204, 0, 1500, 5, false, {0x08},           1, },
            { AB_ADD,205, 0, 1600, 6, false, {0x09},           1, },
            { AB_FLUSH,0,   0, 0,    6, false, {0},              0, },
            /* round 2 */
            { AB_SPEND,201, 0, 0,    0, false, {0},              0, }, /* cross-flush tomb */
            { AB_SPEND,200, 1, 0,    0, false, {0},              0, }, /* cross-flush tomb */
            { AB_ADD,206, 0, 1700, 7, false, {0x0A},           1, },
            { AB_SPEND,206, 0, 0,    0, false, {0},              0, }, /* overlay-only c+s */
            { AB_ADD,202, 0, 9999, 8, false, {0x0B,0x0C},      2, }, /* REPLACE durable key */
            { AB_ADD,207, 0, 1800, 8, false, {0x0D},           1, },
            { AB_ADD,207, 1, 1850, 8, false, {0x0E},           1, },
            { AB_FLUSH,0,   0, 0,    8, false, {0},              0, },
            /* round 3 */
            { AB_SPEND,207, 0, 0,    0, false, {0},              0, }, /* round-2 coin now durable */
            { AB_SPEND,202, 0, 0,    0, false, {0},              0, }, /* the REPLACEd coin, now durable */
            { AB_ADD,208, 0, 1900, 9, false, {0x0F},           1, },
            { AB_ADD,208, 1, 1950, 9, false, {0x10,0x11},      2, },
            { AB_FLUSH,0,   0, 0,    9, false, {0},              0, },
            /* round 4 — NO trailing flush */
            { AB_SPEND,200, 0, 0,    0, false, {0},              0, }, /* round-1 coinbase, durable */
            { AB_SPEND,208, 1, 0,    0, false, {0},              0, }, /* round-3 coin, durable */
            { AB_ADD,209, 0, 2000, 10,false, {0x12},          1, },
            { AB_ADD,205, 0, 7777, 11,false, {0x13,0x14},     2, }, /* REPLACE round-1 durable */
        };
        const size_t MF_N = sizeof(MF) / sizeof(MF[0]);

        /* golden: pure SQLite. */
        char dirG[256];
        test_make_tmpdir(dirG, sizeof(dirG), "coins_ram_mf_g", "g");
        CR_CHECK("mf.G: open", progress_store_open(dirG));
        sqlite3 *dbG = progress_store_db();
        CR_CHECK("mf.G: schema", coins_kv_ensure_schema(dbG));
        CR_CHECK("mf.G: pure sqlite", !coins_ram_active());
        CR_CHECK("mf.G: apply clean", ab_apply_all(dbG, MF, MF_N, false) == 0);
        uint8_t rootG[32] = {0};
        CR_CHECK("mf.G: commitment ok", coins_kv_commitment(dbG, rootG) == 0);
        int64_t cntG = coins_kv_count(dbG);
        /* Capture a few golden point reads to compare against the overlay leg. */
        struct uint256 q_live  = ab_txid(209);  /* minted round 4, vout 0 */
        struct uint256 q_repl  = ab_txid(205);  /* re-added round 4, vout 0 */
        struct uint256 q_spent = ab_txid(202);  /* spent round 3, vout 0 */
        struct uint256 q_xtomb = ab_txid(201);  /* cross-flush tomb, vout 0 */
        int64_t gv_live = 0, gv_repl = 0;
        bool gex_live  = coins_kv_get(dbG, q_live.data,  0, &gv_live, NULL, 0, NULL);
        bool gex_repl  = coins_kv_get(dbG, q_repl.data,  0, &gv_repl, NULL, 0, NULL);
        bool gex_spent = coins_kv_exists(dbG, q_spent.data, 0);
        bool gex_xtomb = coins_kv_exists(dbG, q_xtomb.data, 0);
        progress_store_close();
        test_rm_rf_recursive(dirG);

        /* overlay: real flushes interleaved. */
        char dirO[256];
        test_make_tmpdir(dirO, sizeof(dirO), "coins_ram_mf_o", "o");
        CR_CHECK("mf.O: open", progress_store_open(dirO));
        sqlite3 *dbO = progress_store_db();
        CR_CHECK("mf.O: schema", coins_kv_ensure_schema(dbO));
        CR_CHECK("mf.O: init", coins_ram_init(dbO, 1000000));
        CR_CHECK("mf.O: overlay active", coins_ram_active());
        CR_CHECK("mf.O: apply clean", ab_apply_all(dbO, MF, MF_N, true) == 0);

        /* Final commitment taken with a NON-EMPTY overlay over a populated
         * durable set (round 4 has no trailing flush). */
        uint8_t rootO[32] = {0};
        CR_CHECK("mf.O: commitment ok", coins_kv_commitment(dbO, rootO) == 0);
        int64_t cntO = coins_kv_count(dbO);
        int64_t ov_live = 0, ov_repl = 0;
        bool oex_live  = coins_kv_get(dbO, q_live.data,  0, &ov_live, NULL, 0, NULL);
        bool oex_repl  = coins_kv_get(dbO, q_repl.data,  0, &ov_repl, NULL, 0, NULL);
        bool oex_spent = coins_kv_exists(dbO, q_spent.data, 0);
        bool oex_xtomb = coins_kv_exists(dbO, q_xtomb.data, 0);

        coins_ram_shutdown();
        progress_store_close();
        test_rm_rf_recursive(dirO);

        CR_CHECK("MULTIFLUSH: count golden == overlay", cntG == cntO);
        CR_CHECK("MULTIFLUSH: final root golden == overlay",
                 memcmp(rootG, rootO, 32) == 0);
        CR_CHECK("MULTIFLUSH: live read matches (val + presence)",
                 gex_live == oex_live && gex_live && gv_live == ov_live);
        CR_CHECK("MULTIFLUSH: replaced-key read matches",
                 gex_repl == oex_repl && gex_repl && gv_repl == ov_repl);
        CR_CHECK("MULTIFLUSH: spent-after-flush reads ABSENT both legs",
                 gex_spent == oex_spent && !gex_spent);
        CR_CHECK("MULTIFLUSH: cross-flush tombstone reads ABSENT both legs",
                 gex_xtomb == oex_xtomb && !gex_xtomb);
    }

    /* ──────────────────────────────────────────────────────────────────────
     * (c) MICRO-BENCHMARK: per-op cost of an in-RAM overlay probe vs a SQLite
     *     coins_kv B-tree get at a large populated N. Diagnostic only (printed,
     *     not asserted) — gives the real speedup the accelerator must earn
     *     before the ~35h mint is committed. N is sized down automatically if
     *     the build can't afford the larger fill cheaply. */
    {
        const int N = 1000000;  /* target populated coins */
        char dirP[256];
        test_make_tmpdir(dirP, sizeof(dirP), "coins_ram_bench", "p");
        if (progress_store_open(dirP)) {
            sqlite3 *dbP = progress_store_db();
            coins_kv_ensure_schema(dbP);

            /* ── populate the durable SQLite B-tree (overlay OFF) ── */
            struct timespec t0, t1;
            clock_gettime(CLOCK_MONOTONIC, &t0);  // platform-ok:coins-ram-benchmark-realtime
            progress_store_tx_lock();
            sqlite3_exec(dbP, "BEGIN IMMEDIATE", NULL, NULL, NULL);
            int filled = 0;
            for (int i = 0; i < N; i++) {
                struct uint256 t = ab_txid((uint32_t)i + 1000000u);
                uint8_t scr[24];
                for (int k = 0; k < 24; k++) scr[k] = (uint8_t)(i + k);
                if (!coins_kv_add(dbP, t.data, 0, 1000 + i, i % 1000, false,
                                  scr, sizeof(scr)))
                    break;
                filled++;
            }
            sqlite3_exec(dbP, "COMMIT", NULL, NULL, NULL);
            progress_store_tx_unlock();
            clock_gettime(CLOCK_MONOTONIC, &t1);  // platform-ok:coins-ram-benchmark-realtime
            double fill_s = (t1.tv_sec - t0.tv_sec)
                          + (t1.tv_nsec - t0.tv_nsec) / 1e9;
            printf("  coins_ram: BENCH filled %d durable coins in %.2fs\n",
                   filled, fill_s);

            /* ── SQLite B-tree get cost (overlay OFF) ── */
            const int PROBES = 200000;
            clock_gettime(CLOCK_MONOTONIC, &t0);  // platform-ok:coins-ram-benchmark-realtime
            volatile int64_t sink = 0; int hit = 0;
            for (int p = 0; p < PROBES; p++) {
                uint32_t idx = (uint32_t)(((uint64_t)p * 2654435761u) % (uint32_t)filled);
                struct uint256 t = ab_txid(idx + 1000000u);
                int64_t v = 0;
                if (coins_kv_get_sqlite(dbP, t.data, 0, &v, NULL, 0, NULL)) {
                    sink += v; hit++;
                }
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);  // platform-ok:coins-ram-benchmark-realtime
            double sqlite_ns = ((t1.tv_sec - t0.tv_sec) * 1e9
                              + (t1.tv_nsec - t0.tv_nsec)) / PROBES;

            /* ── overlay probe cost: bind the overlay to the SAME db and read
             *    the SAME keys. The keys live ONLY in durable coins_kv, so each
             *    overlay get is a probe (miss) + read-through; to isolate the
             *    in-RAM probe cost we ALSO pre-load the keys into the overlay
             *    (LIVE slots) and read those (overlay hit, no SQLite). ── */
            coins_ram_init(dbP, 1000000);
            /* warm the overlay with the probed keys as LIVE slots */
            for (int p = 0; p < PROBES; p++) {
                uint32_t idx = (uint32_t)(((uint64_t)p * 2654435761u) % (uint32_t)filled);
                struct uint256 t = ab_txid(idx + 1000000u);
                uint8_t scr[24];
                for (int k = 0; k < 24; k++) scr[k] = (uint8_t)(idx + k);
                coins_ram_add(t.data, 0, 1000 + idx, (int32_t)(idx % 1000), false,
                              scr, sizeof(scr));
            }
            clock_gettime(CLOCK_MONOTONIC, &t0);  // platform-ok:coins-ram-benchmark-realtime
            int hit2 = 0;
            for (int p = 0; p < PROBES; p++) {
                uint32_t idx = (uint32_t)(((uint64_t)p * 2654435761u) % (uint32_t)filled);
                struct uint256 t = ab_txid(idx + 1000000u);
                int64_t v = 0;
                if (coins_ram_get(t.data, 0, &v, NULL, 0, NULL)) { sink += v; hit2++; }
            }
            clock_gettime(CLOCK_MONOTONIC, &t1);  // platform-ok:coins-ram-benchmark-realtime
            double ram_ns = ((t1.tv_sec - t0.tv_sec) * 1e9
                           + (t1.tv_nsec - t0.tv_nsec)) / PROBES;

            printf("  coins_ram: BENCH N=%d  sqlite_get=%.0f ns/op  "
                   "ram_probe=%.0f ns/op  speedup=%.1fx  (sqlite_hits=%d "
                   "ram_hits=%d sink=%lld)\n",
                   filled, sqlite_ns, ram_ns,
                   ram_ns > 0 ? sqlite_ns / ram_ns : 0.0, hit, hit2,
                   (long long)sink);
            /* Sanity: both legs must have actually hit, else the numbers lie. */
            CR_CHECK("bench: both legs hit", hit > 0 && hit2 > 0);

            coins_ram_shutdown();
            progress_store_close();
        }
        test_rm_rf_recursive(dirP);
    }

    printf("test_coins_ram: %d failures\n", failures);
    return failures;
}

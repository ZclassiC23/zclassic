/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_coins_view_projection — B4: the projection-backed coins_view
 * reconstructs `struct coins` correctly from EV_UTXO_* events.
 *
 * Proves the read primitive that closes the validation feedback loop:
 *   1. reconstruct — a multi-output txid (one output spent) yields a
 *      struct coins with the right num_vout, the spent vout nulled, the
 *      live vouts' value/script intact, version==1 (matching
 *      coins_view_sqlite), and height/is_coinbase preserved.
 *   2. have/absent — have_coins is true for a live txid, false once all
 *      its outputs are spent and false for an unknown txid. */

#include "test/test_helpers.h"

#include "coins/coins.h"
#include "coins/coins_view.h"
#include "core/uint256.h"
#include "storage/coins_view_projection.h"
#include "storage/event_log.h"
#include "storage/utxo_projection.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#define CVP_CHECK(name, expr) do { \
    if (!(expr)) { printf("  FAIL: %s\n", name); failures++; } \
} while (0)

static void make_txid(uint8_t txid[32], uint8_t seed)
{
    for (int i = 0; i < 32; i++) txid[i] = (uint8_t)(seed + i);
}

static bool emit_add(uint8_t seed, uint32_t vout, int64_t value,
                     uint32_t height, bool coinbase, uint32_t script_len)
{
    uint8_t txid[32]; make_txid(txid, seed);
    uint8_t script[64];
    for (uint32_t k = 0; k < script_len && k < sizeof(script); k++)
        script[k] = (uint8_t)((seed * 11 + k) & 0xFF);
    return utxo_projection_emit_add(txid, vout, value, height, coinbase,
                                    script_len ? script : NULL, script_len);
}

static bool emit_spend(uint8_t seed, uint32_t vout)
{
    uint8_t txid[32]; make_txid(txid, seed);
    return utxo_projection_emit_spend(txid, vout);
}

int test_coins_view_projection(void);
int test_coins_view_projection(void)
{
    int failures = 0;
    printf("test_coins_view_projection: B4 projection-backed coins_view\n");

    char dir[256];
    snprintf(dir, sizeof(dir), "/tmp/zcl_cvp_%d", (int)getpid());
    mkdir(dir, 0700);
    char log_path[512], proj_path[512];
    snprintf(log_path,  sizeof(log_path),  "%s/events.log",         dir);
    snprintf(proj_path, sizeof(proj_path), "%s/utxo_projection.db", dir);

    event_log_t *log = event_log_open(log_path);
    utxo_projection_t *p = utxo_projection_open(proj_path, log);
    CVP_CHECK("open log+projection", log && p);
    if (!log || !p) goto done;

    utxo_projection_set_event_log(log);

    /* txid T (seed 0x70): coinbase tx, 3 outputs; vout 1 later spent. */
    CVP_CHECK("emit vout0", emit_add(0x70, 0, 5000000000LL, 250, true, 25));
    CVP_CHECK("emit vout1", emit_add(0x70, 1, 1500, 250, true, 10));
    CVP_CHECK("emit vout2", emit_add(0x70, 2, 2500, 250, true, 33));
    /* An unrelated txid U (seed 0x90): single output, will be fully spent. */
    CVP_CHECK("emit U", emit_add(0x90, 0, 777, 251, false, 5));
    CVP_CHECK("spend T:1", emit_spend(0x70, 1));
    CVP_CHECK("spend U:0", emit_spend(0x90, 0));
    CVP_CHECK("catch_up", utxo_projection_catch_up(p) != UINT64_MAX);

    struct coins_view_projection cvp;
    CVP_CHECK("init adapter", coins_view_projection_init(&cvp, p));

    /* 1. reconstruct T */
    uint8_t tT[32]; make_txid(tT, 0x70);
    struct uint256 T; memcpy(T.data, tT, 32);
    struct coins c;
    bool got = coins_view_get_coins(&cvp.view, &T, &c);
    CVP_CHECK("T get_coins true", got);
    if (got) {
        CVP_CHECK("T num_vout==3", c.num_vout == 3);
        CVP_CHECK("T version==1", c.version == 1);
        CVP_CHECK("T height==250", c.height == 250);
        CVP_CHECK("T is_coinbase", c.is_coinbase == true);
        if (c.num_vout == 3) {
            CVP_CHECK("T vout0 value", c.vout[0].value == 5000000000LL);
            CVP_CHECK("T vout0 script_len", c.vout[0].script_pub_key.size == 25);
            CVP_CHECK("T vout1 spent (null)", tx_out_is_null(&c.vout[1]));
            CVP_CHECK("T vout2 value", c.vout[2].value == 2500);
            CVP_CHECK("T vout2 script_len", c.vout[2].script_pub_key.size == 33);
            bool s0ok = true;
            for (uint32_t k = 0; k < 25; k++)
                if (c.vout[0].script_pub_key.data[k] !=
                    (uint8_t)((0x70 * 11 + k) & 0xFF)) { s0ok = false; break; }
            CVP_CHECK("T vout0 script bytes", s0ok);
        }
        coins_free(&c);
    }

    /* 2. have / absent */
    CVP_CHECK("have T (live)", coins_view_have_coins(&cvp.view, &T));

    uint8_t tU[32]; make_txid(tU, 0x90);
    struct uint256 U; memcpy(U.data, tU, 32);
    CVP_CHECK("U absent (all spent)", !coins_view_have_coins(&cvp.view, &U));
    struct coins cu;
    CVP_CHECK("U get_coins false", !coins_view_get_coins(&cvp.view, &U, &cu));
    CVP_CHECK("U coins_init'd (num_vout==0)", cu.num_vout == 0);

    uint8_t tX[32]; make_txid(tX, 0x12);
    struct uint256 X; memcpy(X.data, tX, 32);
    CVP_CHECK("unknown txid absent", !coins_view_have_coins(&cvp.view, &X));

    utxo_projection_set_event_log(NULL);
    utxo_projection_close(p);
    event_log_close(log);
done:
    test_cleanup_tmpdir(dir);
    if (failures == 0)
        printf("  all coins_view_projection checks passed\n");
    return failures;
}

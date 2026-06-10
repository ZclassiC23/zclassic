/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * WalletTx read-model helpers.
 *
 * Read-only aggregate and activity queries over wallet_transactions and
 * wallet_utxos. Row lifecycle and validation live in wallet_tx.c; these helpers
 * do not persist records.
 *
 * ar-validate-skip:read-only-wallet-aggregates
 */

#include "models/wallet_tx.h"

int64_t db_wallet_tx_total_fees(struct node_db *ndb, int *fee_paying_count)
{
    if (fee_paying_count) *fee_paying_count = 0;
    if (!ndb || !ndb->open)
        return 0;
    sqlite3_stmt *s = NULL;
    int64_t total = 0;
    int count = 0;
    AR_PREPARE_RET(ndb, s,
        "SELECT fee FROM wallet_transactions WHERE from_me = 1 AND fee > 0",
        0);
    while (AR_STEP_ROW(s)) {
        total += AR_COL_INT(s, 0);
        count++;
    }
    AR_FINALIZE(s);
    if (fee_paying_count) *fee_paying_count = count;
    return total;
}

int db_wallet_utxo_recent_activity(struct node_db *ndb,
                                   struct db_wallet_activity *out, size_t max)
{
    if (!ndb->open) return 0;
    sqlite3_stmt *s = NULL;
    AR_QUERY_LIST(ndb, s,
        "SELECT wu.value, wu.height, COALESCE(b.time,0)"
        " FROM wallet_utxos wu"
        " LEFT JOIN blocks b ON b.height=wu.height"
        " WHERE wu.spent_txid IS NULL"
        " ORDER BY wu.height DESC LIMIT ?",
        out, max,
        AR_BIND_INT(s, 1, (int)max),
        out[count].value = AR_COL_INT(s, 0);
        out[count].height = (int)AR_COL_INT(s, 1);
        out[count].time = AR_COL_INT(s, 2));
}

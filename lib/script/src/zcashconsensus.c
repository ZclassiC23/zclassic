/* Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "script/zcashconsensus.h"
#include "consensus/upgrades.h"
#include "primitives/transaction.h"
#include "script/interpreter.h"
#include "script/script.h"
#include "core/serialize.h"
#include "validation/sighash.h"
#include "validation/tx_verifier.h"
#include <string.h>

static int set_error(zcl_consensus_error *ret, zcl_consensus_error e)
{
    if (ret) *ret = e;
    return 0;
}

int zcl_consensus_verify_script(const unsigned char *script_pub_key,
                                unsigned int script_pub_key_len,
                                const unsigned char *tx_to,
                                unsigned int tx_to_len,
                                unsigned int n_in,
                                unsigned int flags,
                                zcl_consensus_error *err)
{
    struct transaction tx;
    transaction_init(&tx);

    struct byte_stream s;
    stream_init_from_data(&s, tx_to, tx_to_len);

    if (!transaction_deserialize(&tx, &s)) {
        transaction_free(&tx);
        stream_free(&s);
        return set_error(err, zcl_consensus_ERR_TX_DESERIALIZE);
    }

    if (n_in >= tx.num_vin) {
        transaction_free(&tx);
        stream_free(&s);
        return set_error(err, zcl_consensus_ERR_TX_INDEX);
    }

    size_t ser_size = transaction_serialize_size(&tx);
    if (ser_size != tx_to_len) {
        transaction_free(&tx);
        stream_free(&s);
        return set_error(err, zcl_consensus_ERR_TX_SIZE_MISMATCH);
    }

    set_error(err, zcl_consensus_ERR_OK);

    struct precomputed_tx_data txdata;
    precompute_tx_data(&tx, &txdata);

    struct tx_sig_checker tsc;
    tx_sig_checker_init(&tsc, &tx, n_in, 0, SPROUT_BRANCH_ID, &txdata);
    struct sig_checker checker = tx_make_sig_checker(&tsc);

    struct script spk;
    script_init(&spk);
    if (script_pub_key_len <= MAX_SCRIPT_SIZE) {
        memcpy(spk.data, script_pub_key, script_pub_key_len);
        spk.size = script_pub_key_len;
    }

    ScriptError serror;
    int result = verify_script(&tx.vin[n_in].script_sig, &spk,
                               flags, &checker,
                               SPROUT_BRANCH_ID, &serror) ? 1 : 0;

    transaction_free(&tx);
    stream_free(&s);
    return result;
}

unsigned int zcl_consensus_version(void)
{
    return ZCASHCONSENSUS_API_VER;
}

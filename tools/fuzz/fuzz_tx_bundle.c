/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * fuzz_tx_bundle — libFuzzer harness for the full transaction
 * deserializer INCLUDING its Sapling/shielded bundle (shielded
 * spends, shielded outputs, JoinSplits, binding sig) plus the
 * stateless structural validator check_transaction.
 *
 * This is the exact parse path a malicious peer's raw tx bytes hit
 * BEFORE any signature / proof / UTXO validation: the p2p tx handler
 * calls transaction_deserialize on untrusted bytes, then runs the
 * context-free structural checks. fuzz_block reaches tx decoding via
 * the block path; this target puts depth on the SHIELDED-bundle
 * deserialization (spend/output/joinsplit count-vs-remaining-bytes
 * bounds, value_balance range, GROTH_PROOF_SIZE / ciphertext reads)
 * and the structural bounds around it.
 *
 * transaction_deserialize must never read beyond the stream, and
 * check_transaction must never fault, under ANY input. Runs with
 * -fsanitize=fuzzer,address,undefined under clang.
 */

#include "primitives/transaction.h"
#include "core/serialize.h"
#include "consensus/validation.h"
#include "chain/chainparams.h"
#include "validation/check_transaction.h"

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Required by transaction.c's dependency graph (process/sync globals).
 * Provided by main.c in the real binary and by test.c in the suite; the
 * fuzzer is neither, so the global lives here — same as fuzz_block.c. */
volatile sig_atomic_t g_shutdown_requested = 0;

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;
    chain_params_select(CHAIN_MAIN);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0 || size > (1u << 20))
        return 0;  /* libFuzzer convention: return 0 means "keep going" */

    struct byte_stream s;
    stream_init_from_data(&s, data, size);

    struct transaction tx;
    /* transaction_deserialize re-inits tx at entry and leaves a
     * transaction_free-safe (possibly partial) object on false. */
    if (transaction_deserialize(&tx, &s)) {
        /* Only reached on a fully-decoded tx (including any Sapling
         * bundle). Run the stateless structural validator — the same
         * context-free bounds a relayed tx hits: vin/vout/joinsplit/
         * shielded counts, value_balance range, serialized-size cap,
         * duplicate inputs/nullifiers, coinbase restrictions. */
        struct validation_state st;
        validation_state_init(&st);
        (void)check_transaction(&tx, &st);

        /* The in-block variant consults the oversize grandfather set;
         * exercise it too so both structural entry points are covered. */
        struct validation_state st2;
        validation_state_init(&st2);
        (void)check_transaction_in_block(&tx, &st2);
    }

    /* Idempotent-free-safe on both the success and partial-decode paths. */
    transaction_free(&tx);
    stream_free(&s);
    return 0;
}

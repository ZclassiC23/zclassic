/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * fuzz_compactblock — libFuzzer harness for the BIP152 compact-block
 * relay wire deserializers.
 *
 * A prior DoS audit READ these paths and confirmed each length prefix
 * is cap-bounded (MAX_COMPACT_BLOCK_TXNS / MAX_GETBLOCKTXN_INDICES)
 * before the array is allocated. This harness proves those bounds
 * actually hold on adversarial bytes by feeding raw untrusted input
 * straight into the three real deserialize entry points:
 *
 *   compact_block_msg_deserialize   (cmpctblock payload)
 *   block_txn_request_deserialize   (getblocktxn payload)
 *   block_txn_response_deserialize  (blocktxn payload)
 *
 * Each is a pure wire parser over a byte_stream — no sockets, no disk,
 * no globals — so every iteration is deterministic and self-contained.
 * The out-structs own heap allocations, so each is freed before the
 * next parse. Runs with -fsanitize=fuzzer,address,undefined under clang.
 */

#include "net/compact_blocks.h"
#include "core/serialize.h"
#include "chain/chainparams.h"

#include <signal.h>
#include <stddef.h>
#include <stdint.h>

volatile sig_atomic_t g_shutdown_requested = 0;

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;
    /* compact-block short-txid derivation reads header fields; select
     * mainnet params once so the parse hits a stable configuration. */
    chain_params_select(CHAIN_MAIN);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0 || size > (1u << 20))
        return 0;  /* libFuzzer convention: 0 means "keep going" */

    /* cmpctblock: header + nonce + short-txid array + prefilled-tx array.
     * The short-txid and prefilled counts are compact-size-encoded and
     * capped at MAX_COMPACT_BLOCK_TXNS before allocation. */
    {
        struct byte_stream s;
        stream_init_from_data(&s, data, size);
        struct compact_block_msg cb;
        (void)compact_block_msg_deserialize(&cb, &s);
        compact_block_msg_free(&cb);
        stream_free(&s);
    }

    /* getblocktxn: 32-byte block hash + differential index array.
     * The index count is capped at MAX_GETBLOCKTXN_INDICES. */
    {
        struct byte_stream s;
        stream_init_from_data(&s, data, size);
        struct block_txn_request req;
        (void)block_txn_request_deserialize(&req, &s);
        block_txn_request_free(&req);
        stream_free(&s);
    }

    /* blocktxn: 32-byte block hash + full-tx array.
     * The tx count is capped at MAX_COMPACT_BLOCK_TXNS. */
    {
        struct byte_stream s;
        stream_init_from_data(&s, data, size);
        struct block_txn_response resp;
        (void)block_txn_response_deserialize(&resp, &s);
        block_txn_response_free(&resp);
        stream_free(&s);
    }

    return 0;
}

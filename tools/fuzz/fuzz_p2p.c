/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * fuzz_p2p — libFuzzer harness for the P2P message deserializers.
 *
 * We don't invoke msg_process() here — that reaches into peer
 * state, connman, and the rest of the networking stack, which is
 * poorly suited to a stateless fuzzer. Instead we target the pure
 * parsers: version_message_deserialize, plus a generic header
 * decode via byte_stream. Crashes here translate directly to
 * "malicious peer crashes node at handshake time", which is the
 * class of bug we care most about catching.
 */

#include "net/p2p_message.h"
#include "net/protocol.h"
#include "net/netaddr.h"
#include "core/serialize.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "chain/chainparams.h"

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

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
        return 0;

    /* version_message_deserialize is the handshake entry point. A
     * malicious peer can send any sub_version string, nonce, etc.
     * The parser must bound-check the sub_version length and the
     * net_address reads without reading past `size`. */
    {
        struct byte_stream s;
        stream_init_from_data(&s, data, size);
        struct version_message v;
        version_message_init(&v);
        (void)version_message_deserialize(&v, &s);
        stream_free(&s);
    }

    /* Raw transaction deserialization — every inbound tx pass
     * through this path, and the ZCL overwintered/sapling format
     * has a lot of optional fields that are easy to get wrong. */
    {
        struct byte_stream s;
        stream_init_from_data(&s, data, size);
        struct transaction tx;
        transaction_init(&tx);
        if (transaction_deserialize(&tx, &s)) {
            /* Round-trip check: a successfully-parsed tx should
             * serialize back without blowing up. Mismatches aren't
             * necessarily bugs (we may canonicalise on the way
             * out), but segfaults absolutely are. */
            struct byte_stream out;
            stream_init(&out, 256);
            (void)transaction_serialize(&tx, &out);
            stream_free(&out);
        }
        transaction_free(&tx);
        stream_free(&s);
    }

    /* Raw block deserialize — an inbound `block` message carries
     * one of these. We share the shape with fuzz_block.c but only
     * the parse half, not the validator, so that this harness
     * stays cheap and focused on message boundary bugs. */
    {
        struct byte_stream s;
        stream_init_from_data(&s, data, size);
        struct block b;
        block_init(&b);
        (void)block_deserialize(&b, &s);
        block_free(&b);
        stream_free(&s);
    }

    return 0;
}

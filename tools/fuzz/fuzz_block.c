/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * fuzz_block — libFuzzer harness for block deserialization +
 * check_block / check_block_header.
 *
 * The goal is to catch out-of-bounds reads, UB, and integer
 * overflow in the parse+validate path for untrusted block bytes.
 * Runs with -fsanitize=fuzzer,address,undefined under clang.
 *
 * The harness initialises chain params once (via a libFuzzer
 * LLVMFuzzerInitialize callback) and then reuses the same struct
 * chain_params pointer for each iteration so the fuzzer hits the
 * hot path repeatedly rather than re-initialising state.
 */

#include "primitives/block.h"
#include "core/serialize.h"
#include "consensus/validation.h"
#include "chain/chainparams.h"
#include "validation/check_block.h"

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

volatile sig_atomic_t g_shutdown_requested = 0;

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static const struct chain_params *g_params;

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;
    chain_params_select(CHAIN_MAIN);
    g_params = chain_params_get();
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0 || size > (1u << 20))
        return 0;  /* libFuzzer convention: return 0 means "keep going" */

    /* Header-only path — fast and exercises the fixed 80-byte prefix
     * plus solution size decoding. */
    {
        struct byte_stream s;
        stream_init_from_data(&s, data, size);
        struct block_header header;
        block_header_init(&header);
        if (block_header_deserialize(&header, &s)) {
            struct validation_state st;
            validation_state_init(&st);
            /* check_pow=false so we don't waste fuzzer cycles on PoW. */
            (void)check_block_header(&header, &st, g_params, false);
        }
        stream_free(&s);
    }

    /* Full-block path — exercises tx decoding + merkle root.
     * Same input bytes, fresh stream cursor. */
    {
        struct byte_stream s;
        stream_init_from_data(&s, data, size);
        struct block block;
        block_init(&block);
        if (block_deserialize(&block, &s)) {
            struct validation_state st;
            validation_state_init(&st);
            (void)check_block(&block, &st, g_params,
                              /*check_pow=*/false,
                              /*check_merkle_root=*/true,
                              /*check_size_limits=*/true);
        }
        block_free(&block);
        stream_free(&s);
    }
    return 0;
}

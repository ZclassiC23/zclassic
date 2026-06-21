/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "mining/gen.h"
#include "metrics/metrics.h"
#include "chain/equihash.h"
#include "chain/pow.h"
#include "core/random.h"
#include "core/serialize.h"
#include "crypto/equihash.h"
#include "crypto/equihash_solver.h"
#include "validation/chainstate.h"
#include <pthread.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include "util/log_macros.h"
#include "util/util.h"
#include "util/safe_alloc.h"
#include "util/thread_registry.h"

static pthread_t *g_miner_threads = NULL;
static int g_num_miner_threads = 0;

static bool try_solve_equihash(struct block *blk,
                                const struct chain_params *params,
                                int height)
{
    unsigned int n = chain_params_equihash_n(params, height);
    unsigned int k = chain_params_equihash_k(params, height);

    struct equihash_params ep;
    equihash_params_init(&ep, n, k);

    struct blake2b_ctx base_state;
    equihash_initialise_state(&ep, &base_state);

    /* Hash block header fields before nonce */
    struct byte_stream s;
    stream_init(&s, 256);
    stream_write_i32_le(&s, blk->header.nVersion);
    stream_write_bytes(&s, blk->header.hashPrevBlock.data, 32);
    stream_write_bytes(&s, blk->header.hashMerkleRoot.data, 32);
    stream_write_bytes(&s, blk->header.hashFinalSaplingRoot.data, 32);
    stream_write_u32_le(&s, blk->header.nTime);
    stream_write_u32_le(&s, blk->header.nBits);
    blake2b_update(&base_state, s.data, s.size);
    stream_free(&s);

    /* Tromp solver path is for (192,7); ZClassic is (200,9) on mainnet/testnet
     * and (48,5) on regtest, so the brute-force else-branch is the live path. */
    if (n == 192 && k == 7) {
        struct eh_solver *solver = eh_solver_new();
        if (!solver)
            return false;

        /* Try nonces until a solution is found */
        for (int nonce_try = 0; nonce_try < 256; nonce_try++) {
            for (int b = 0; b < 32; b++)
                blk->header.nNonce.data[b] = (unsigned char)(GetRand(256));

            struct blake2b_ctx curr = base_state;
            blake2b_update(&curr, blk->header.nNonce.data, 32);

            eh_solver_set_state(solver, &curr);
            uint32_t nsols = eh_solver_run(solver);
            metrics_increment_eh_solver_runs();

            for (uint32_t i = 0; i < nsols; i++) {
                /* Convert indices to minimal/compressed solution */
                unsigned char sol_bytes[EH_SOL_BYTES];
                size_t sol_len = eh_get_minimal_from_indices(
                    solver->sols[i], EH_PROOFSIZE,
                    ep.collision_bit_length,
                    sol_bytes, sizeof(sol_bytes));

                if (sol_len != EH_SOL_BYTES)
                    continue;

                /* Set solution on block */
                memcpy(blk->header.nSolution, sol_bytes, sol_len);
                blk->header.nSolutionSize = sol_len;

                /* Verify: hash must meet target */
                struct uint256 hash;
                block_header_get_hash(&blk->header, &hash);
                if (CheckProofOfWork(hash, blk->header.nBits,
                                      &params->consensus)) {
                    /* Double-check equihash validity */
                    if (check_equihash_solution(&blk->header, params)) {
                        eh_solver_free(solver);
                        return true;
                    }
                }
            }
        }
        eh_solver_free(solver);
    } else {
        /* Regtest / small params: brute-force random nonces.
         * The equihash solution is trivial at low difficulty. */
        for (unsigned int attempt = 0; attempt < 1000000; attempt++) {
            for (int b = 0; b < 32; b++)
                blk->header.nNonce.data[b] = (unsigned char)(GetRand(256));

            struct uint256 hash;
            block_header_get_hash(&blk->header, &hash);

            if (CheckProofOfWork(hash, blk->header.nBits,
                                  &params->consensus)) {
                return true;
            }
        }
    }

    return false;
}

static void *miner_thread(void *arg)
{
    struct gen_context *ctx = (struct gen_context *)arg;
    LogPrintf("Miner thread started.\n");

    while (ctx->running) {
        struct block_index *tip = active_chain_tip(&ctx->ms->chain_active);
        if (!tip) {
            sleep(1);
            continue;
        }

        struct block_template *tmpl = create_new_block(
            &ctx->coinbase_script, ctx->ms, ctx->coins_tip,
            ctx->mempool, ctx->params);
        if (!tmpl) {
            sleep(1);
            continue;
        }

        unsigned int extra_nonce = 0;
        increment_extra_nonce(&tmpl->block, tip, &extra_nonce);

        if (try_solve_equihash(&tmpl->block, ctx->params,
                               tip->nHeight + 1)) {
            LogPrintf("Found block!\n");
            if (ctx->block_found &&
                ctx->block_found(&tmpl->block, ctx->block_found_ctx)) {
                struct block_index *new_tip =
                    active_chain_tip(&ctx->ms->chain_active);
                if (new_tip && new_tip->phashBlock) {
                    char hex[65];
                    uint256_get_hex(new_tip->phashBlock, hex);
                    LogPrintf("New block: height=%d hash=%s\n",
                              new_tip->nHeight, hex);
                }
            }
        }

        block_template_free(tmpl);
        free(tmpl);
    }

    LogPrintf("Miner thread stopped.\n");
    return NULL;
}

void gen_start(struct gen_context *ctx)
{
    int started = 0;

    if (!ctx)
        return;
    if (ctx->num_threads <= 0)
        ctx->num_threads = 1;

    if (atomic_load(&ctx->running))
        return;
    g_num_miner_threads = ctx->num_threads;
    g_miner_threads = zcl_calloc((size_t)g_num_miner_threads, sizeof(pthread_t), "miner_threads");
    if (!g_miner_threads)
        return;

    atomic_store(&ctx->running, true);

    for (int i = 0; i < g_num_miner_threads; i++) {
        if (thread_registry_spawn_ex("zcl_miner", miner_thread, ctx,
                                      &g_miner_threads[i]) != 0) {
            LOG_WARN("mining", "gen_start: failed to start miner thread %d", i);
            atomic_store(&ctx->running, false);
            for (int j = 0; j < started; j++)
                pthread_join(g_miner_threads[j], NULL);
            free(g_miner_threads);
            g_miner_threads = NULL;
            g_num_miner_threads = 0;
            return;
        }
        started++;
    }

    LogPrintf("Mining started with %d thread(s).\n", g_num_miner_threads);
}

void gen_stop(struct gen_context *ctx)
{
    if (!ctx || !g_miner_threads)
        return;
    atomic_store(&ctx->running, false);
    for (int i = 0; i < g_num_miner_threads; i++)
        pthread_join(g_miner_threads[i], NULL);
    free(g_miner_threads);
    g_miner_threads = NULL;
    g_num_miner_threads = 0;
    LogPrintf("Mining stopped.\n");
}

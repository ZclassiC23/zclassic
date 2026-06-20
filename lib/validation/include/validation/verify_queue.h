/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/*
 * verify_queue — a tagged batch of self-contained verification jobs that
 * AND-reduces to a single batch verdict, fanned across the persistent
 * thread_pool (validation/thread_pool.h) when -par>1, run inline when -par=1.
 *
 * ADDITIVE FOUNDATION — NOT wired into the staged reducer or any consensus
 * path. The three job kinds (SCRIPT_CHECK / GROTH16_PROOF / EQUIHASH_POW)
 * name the consensus verifiers this engine is intended to eventually wrap:
 *   - SCRIPT_CHECK  -> verify_script()           (script/interpreter.h)
 *   - GROTH16_PROOF -> zclassic_sapling_check_*  (sapling/sapling_prover.h)
 *   - EQUIHASH_POW  -> check_equihash_solution() (chain/equihash.h)
 * For now each job carries an OPAQUE self-contained payload (`arg`) plus a
 * function pointer `fn(arg) -> bool`. The kind tag is metadata; dispatch is
 * via the function pointer, so the engine has no dependency on the heavy
 * consensus libraries and can be exercised in isolation. When wired, each
 * call site fills `arg` with a heap/owned payload and points `fn` at a thin
 * wrapper over the real verifier.
 *
 * Determinism contract: every `fn` MUST be a pure function of its `arg`
 * (no shared mutable state, no rand/clock), so the batch verdict and every
 * per-job verdict are identical whether run serially or across N workers.
 *
 * Results land in a PER-JOB slot (verify_job.result) — never a shared
 * counter. verify_queue_submit_batch() runs the batch, then a single
 * AND-reduce over the per-job slots yields the batch verdict.
 */

#ifndef ZCL_VALIDATION_VERIFY_QUEUE_H
#define ZCL_VALIDATION_VERIFY_QUEUE_H

#include "validation/thread_pool.h"

#include <stdbool.h>

enum verify_job_kind {
    VERIFY_JOB_SCRIPT_CHECK = 0,  /* ECDSA scriptSig vs scriptPubKey       */
    VERIFY_JOB_GROTH16_PROOF,     /* Sapling/Sprout Groth16 spend/output   */
    VERIFY_JOB_EQUIHASH_POW       /* Equihash(200,9) header solution + PoW */
};

/* One verification job. Self-contained: `arg` owns everything `fn` needs.
 * `result` is written by the engine (the per-job verdict slot). */
struct verify_job {
    enum verify_job_kind kind;       /* metadata / future dispatch hint     */
    bool (*fn)(void *arg);           /* pure verifier: true=pass false=fail */
    void *arg;                       /* self-contained payload (caller owns)*/
    bool result;                     /* out: per-job verdict                */
};

/* Run a batch of jobs and AND-reduce to a single verdict.
 *
 *   - pool == NULL  OR  pool has zero worker threads  -> SERIAL: jobs run
 *     inline on the calling thread (this is the -par=1 path).
 *   - otherwise -> jobs fanned across the pool's workers.
 *
 * Either way each job's per-job verdict is written into jobs[i].result, and
 * the return value is the AND over all of them (true iff every job passed).
 * An empty batch (n==0) returns true (vacuous). On a NULL-jobs/negative-n
 * guard it logs and returns false.
 *
 * `pool` may be NULL to force the serial path without owning a pool. */
bool verify_queue_submit_batch(struct thread_pool *pool,
                               struct verify_job *jobs, int n);

#endif /* ZCL_VALIDATION_VERIFY_QUEUE_H */

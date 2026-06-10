/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton
 *
 * domain/consensus/script_interp.h — pure ZClassic script-VM.
 *
 * This module hosts the OP_* evaluator and the verify_script orchestrator
 * (scriptSig + scriptPubKey + P2SH redeem path). It is consensus-critical
 * — the script VM is the gate by which UTXO outputs become spendable.
 *
 * The interpreter is pure: it takes a script byte-array, an execution
 * stack, the SCRIPT_VERIFY_* flags, an optional signature-checker
 * callback (struct sig_checker), and a consensus-branch-id, and returns
 * a bool plus a precise rejection reason via ScriptError out-param.
 *
 *   - NO I/O (no socket, file, db touched)
 *   - NO clock (no wall-time decisions; nLockTime is supplied by the
 *               caller's checker->check_lock_time)
 *   - NO RNG  (deterministic from inputs alone)
 *   - NO state reads (no globals, no caches; signature verification
 *                     is performed through the caller-provided
 *                     checker callback — the impure ECDSA verifier
 *                     and any sigcache lookups live in the lib/
 *                     wrapper that fills `struct sig_checker`)
 *
 * Replays byte-exactly from (script_sig, script_pub_key, flags,
 * checker outputs, consensus_branch_id). This is what lets us pin
 * regression behaviour and put the VM under fuzz.
 *
 * Layering: domain/ may #include from util/, core/, chain/, consensus/,
 * crypto/, sapling/, script/, primitives/. The stack/checker types and
 * SCRIPT_VERIFY_* bit defines are defined in script/, not duplicated
 * here, so adapter and lib layers can pass-through unchanged.
 *
 * Background: zclassicd src/script/interpreter.cpp::EvalScript +
 * VerifyScript are the historic source-of-truth. The extraction
 * preserves byte-exact behaviour for every input that legacy accepts
 * and every error code legacy rejects with.
 */

#ifndef ZCL_DOMAIN_CONSENSUS_SCRIPT_INTERP_H
#define ZCL_DOMAIN_CONSENSUS_SCRIPT_INTERP_H

#include <stdbool.h>
#include <stdint.h>

#include "script/interpreter.h"   /* struct script_stack, struct sig_checker, etc. */
#include "script/script.h"
#include "script/script_error.h"
#include "script/script_flags.h"

/* Pure OP_* evaluator. Evaluates `script` against the supplied `stack`
 * (caller must have stack_init'd it). On success returns true and
 * leaves `stack` in the post-execution state. On failure returns false
 * and writes a precise rejection reason via `serror`.
 *
 * `checker` may be NULL — in that case OP_CHECKSIG / OP_CHECKMULTISIG /
 * OP_CHECKDATASIG always treat the signature as invalid (yielding a
 * pushed empty/false), which is the historic behaviour used by
 * fuzz harnesses and structural tests.
 *
 * No clock / RNG / IO / state-reads. Pure function of inputs. */
bool domain_consensus_eval_script(struct script_stack *stack,
                                  const struct script *script,
                                  unsigned int flags,
                                  const struct sig_checker *checker,
                                  uint32_t consensus_branch_id,
                                  ScriptError *serror);

/* Pure scriptSig+scriptPubKey verifier. Allocates its own evaluation
 * stacks via stack_init / stack_free (caller does not pass them).
 * Performs:
 *   1. SIGPUSHONLY gate on scriptSig
 *   2. eval scriptSig
 *   3. eval scriptPubKey on the resulting stack
 *   4. if P2SH wrapper detected, eval the redeem script
 *   5. CLEANSTACK gate
 *
 * The signature-checker is supplied by the caller and is the ONLY
 * impure surface — every ECDSA verify, every sigcache lookup, every
 * locktime/sequence comparison is funneled through it. The interpreter
 * itself reads no global state.
 *
 * No clock / RNG / IO / state-reads. */
bool domain_consensus_verify_script(const struct script *script_sig,
                                    const struct script *script_pub_key,
                                    unsigned int flags,
                                    const struct sig_checker *checker,
                                    uint32_t consensus_branch_id,
                                    ScriptError *serror);

#endif /* ZCL_DOMAIN_CONSENSUS_SCRIPT_INTERP_H */

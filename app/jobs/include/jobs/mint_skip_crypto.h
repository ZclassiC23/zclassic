/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * mint_skip_crypto — the OFFLINE FAST-MINT crypto pass-through toggle.
 *
 * The `-mint-anchor-fast` boot mode (honored ONLY together with -mint-anchor)
 * folds the on-disk block BODIES genesis..anchor through the staged pipeline,
 * but PASSES THROUGH the two crypto stages (script_validate's per-input ECDSA
 * verify_script loop and proof_validate's Groth16/Ed25519/PHGR13/binding-sig
 * verification) WITHOUT running them. The state-transition stage (utxo_apply)
 * is UNCHANGED, so the folded coins_kv set is identical to the full-validated
 * fold — the set of coins is a pure function of WHICH outputs exist and WHICH
 * are spent (the bodies + the state rules), not of the signature/proof witness.
 *
 * SOUNDNESS — this mode is sound ONLY for PRODUCING the one anchor snapshot,
 * whose correctness is certified independently by the terminal hard-assert
 * (coins_kv_commitment(anchor) == g_sha3_checkpoint.sha3_hash AND count ==
 * the compiled utxo_count, boot_mint_anchor.c). By SHA3-256 collision
 * resistance a state-only fold whose result hashes to the trusted fingerprint
 * IS the fully-validated set. Any divergence the skipped crypto would have
 * caught (a block that should have been rejected, changing the coin set) yields
 * a DIFFERENT SHA3 → the assert FATALs and unlinks the artifact. So the skip
 * can only ever PRODUCE-AND-PUBLISH the correct set or REFUSE — never publish a
 * wrong one. It is NOT sound for validating the chain and NEVER gates a running
 * node's accept/reject decision.
 *
 * MECHANISM — a process-global atomic bool. Both crypto step bodies read it
 * once per height; when ON they write the SAME "verified"/ok=true log row the
 * verified path writes (and script_validate still raises BLOCK_VALID_SCRIPTS),
 * so utxo_apply/tip_finalize are byte-identical downstream.
 *
 * NORMAL-BOOT INVARIANT: the toggle defaults to OFF. A normal boot NEVER calls
 * mint_skip_crypto_set, so `if (mint_skip_crypto_get())` is provably never true
 * and every height runs REAL crypto — byte-identical to today. The setter is
 * called ONLY from the mint driver TUs (config/src/boot_mint_anchor.c via
 * config/src/boot_refold_staged.c::boot_mint_anchor_reset), gated under
 * ctx->mint_anchor, and the -mint-anchor process is a one-shot that never
 * starts P2P/RPC and _exit()s after the mint. Enforced by the lint gate
 * tools/lint/check_mint_skip_crypto_offline_only.sh + test_mint_skip_crypto.
 *
 * This module changes NO validation rule for a running node — it only controls
 * whether the offline mint re-derives the (already-once-validated) crypto
 * verdict while producing a fingerprint-certified set.
 */

#ifndef ZCL_JOBS_MINT_SKIP_CRYPTO_H
#define ZCL_JOBS_MINT_SKIP_CRYPTO_H

#include <stdbool.h>

/* Set the OFFLINE FAST-MINT crypto pass-through. true => script_validate and
 * proof_validate skip their per-block crypto and write the verified row
 * directly. ONLY the -mint-anchor driver (gated under ctx->mint_anchor) calls
 * this; a normal boot never does. */
void mint_skip_crypto_set(bool skip);

/* Read the toggle. Cheap atomic read — safe from any stage thread. Returns
 * false (run REAL crypto) when unset (the default). */
bool mint_skip_crypto_get(void);

#endif /* ZCL_JOBS_MINT_SKIP_CRYPTO_H */

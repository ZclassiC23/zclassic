# Sapling shielded ops in the deterministic simulator — research spike

Scope: what it takes to exercise z-address fund / shielded send / memo /
nullifier double-spend through REAL `connect_block` validation inside
`lib/sim/*` (driven via `test_simnet_txkit` / the `simnet_wire` harness),
BEFORE any live-ZCL shielded op.

---

## 1. Proving / verifying machinery that exists in-binary TODAY

**A real Groth16 PROVER exists in-binary — pure C23, NOT Rust/librustzcash.**
(The only Rust in the tree is `vendor/tor`; no `Cargo.toml`/`.rs` in app/lib.)

- **Prover** — `lib/sapling/src/sapling_prover_c23.c` (404 lines), API in
  `lib/sapling/include/sapling/sapling_prover.h`:
  - `zclassic_sapling_proving_ctx_init/_free`
  - `zclassic_sapling_output_proof(...)` — output description + value commit
  - `zclassic_sapling_spend_proof(...)` — takes ak/nsk/diversifier/rcm/ar +
    a 1057-byte merkle witness (`depth || 32×(sibling||bit)`) + anchor
  - `zclassic_sapling_binding_sig(...)`
  - Proving keys are **mmap'd from disk**: `load_pk_file()` opens
    `<params_dir>/sapling-spend.params` and `sapling-output.params`
    (`ensure_spend_pk`/`ensure_output_pk`, params_dir set by
    `sapling_init_params`). Files live at `~/.zcash-params/`:
    `sapling-spend.params` = **48 MB**, `sapling-output.params` = **3.5 MB**
    (sprout-groth16 725 MB + sprout-proving.key 910 MB also present but not
    needed for Sapling).
- **Verifier** (the REAL consensus check) —
  `zclassic_sapling_verification_ctx_init / check_spend / check_output /
  final_check`, invoked from **`lib/validation/src/contextual_check_tx.c:106-146`**
  (`contextual_check_transaction`). This is the real path: spend proof +
  output proof + binding sig, DoS-100 reject on failure. No mock anywhere.
- **Wallet build path (real prover already wired):**
  - t→z: `app/controllers/src/wallet_shielded_send.c:260-407` — inits a
    proving ctx, calls `sapling_build_output_with_ctx(...)` per shielded out,
    then `zclassic_sapling_binding_sig`.
  - z→z / z→t: `app/controllers/src/wallet_shielded_send_shielded.c` (the
    `z_sendmany_shielded` branch; also `wallet_mark_sapling_nullifiers_spent`).
- **Nullifier double-spend enforcement:**
  `app/jobs/src/utxo_apply_nullifiers.c` — checks each nullifier against the
  durable set AND an earlier-tx-in-block accumulator; durable insert stmt in
  `app/models/src/database.c` (`stmt_nullifier_insert/_exists`), applied via
  `sync_controller_blocks.c:317`. Deferred/bg re-verify also exists:
  `app/jobs/src/proof_validate_stage.c:246`.
- **Proof-skip gating** is a global IBD/deferral height
  (`g_deferred_proof_validation_below_height`, `contextual_check_tx.c:76-77`)
  plus the block-level `if (!is_ibd)` gate in
  `check_block.c` `contextual_check_block` (line ~449). It is **NOT** tied to
  the simnet synthetic checkpoint / `expensive_checks` flag. So a simnet block
  can run the FULL Sapling verification as long as it drives the non-IBD path
  with the deferral height off.
- **In-binary round-trip already proven** where params are present:
  `lib/test/src/test_snark_kat.c` (SKIPS when `~/.zcash-params` absent),
  `test_sapling_crypto.c`, and the params-free algebraic slices
  `test_shielded_spend_slice.c` / `test_shielded_receive_slice.c`.

## 2. Minimal viable path to a deterministic in-sim shielded send

The simnet harness (`lib/sim/src/simnet.c`) already drives **real
`connect_block`** and holds `params` as a **value-copy** of
`chain_params_get()` (`simnet.c:223`, and `simnet_chain.c:516`) plus a
synthetic checkpoint at height 1,000,000 that lets PoW/parallel-script checks
be skipped. It deliberately mints at `SIM_BASE_HEIGHT = 100`, which is BELOW
Sapling activation (mainnet `UPGRADE_SAPLING.nActivationHeight = 476969`,
`lib/chain/src/chainparams.c:132`) precisely so the all-zero
`hashFinalSaplingRoot` check is skipped (`simnet.c:35-36`,
`has_chain_sapling_value = false`).

Minimal viable sequence:
1. **Sim-local activation profile.** In the sim's `params` value-copy, lower
   `vUpgrades[UPGRADE_OVERWINTER/UPGRADE_SAPLING].nActivationHeight` to a low
   sim height (e.g. 100) — OR mint the sim tip to height ≥ 476969 under the
   existing checkpoint. The value-copy approach keeps it sim-local (see §5).
2. **Build the Sapling note-commitment tree in-sim.** Maintain an
   incremental merkle tree (`lib/sapling/incremental_merkle_tree`), compute the
   real anchor + per-note witness, and set `header.hashFinalSaplingRoot` to the
   computed tree root so `connect_block`'s root check
   (`connect_block.c:697-728`) passes.
3. **Fund:** mint a transparent coin (existing `simnet_mint_coinbase_to` /
   `simnet_wallet`), then build a **t→z** tx with the REAL prover
   (`sapling_build_output_with_ctx`) → a shielded output + memo (memo path is
   the `0xF6`-padded 512-byte field, already in `wallet_shielded_send.c:113`).
4. **Shielded send:** build a **z→z** tx via the spend prover, supplying the
   witness/anchor from step 2.
5. **Drive real verification:** connect the block with `is_ibd=false` and the
   deferral height off, so `contextual_check_transaction` runs
   check_spend/check_output/final_check and `utxo_apply_nullifiers` records the
   nullifier.
6. **Nullifier double-spend negative test:** replay a tx re-using the spent
   nullifier → assert reject via the durable-set check.

Requires proving-key files present; gate SKIPS cleanly when absent
(test_snark_kat pattern).

## 3. Hard blockers

- **No seedable prover RNG (determinism blocker).** `rcv`/`esk`/`rcm` come
  from `sapling_generate_r()` (`lib/sapling/src/sapling.c:511`) →
  `zcl_random_secret_bytes` → `GetRandBytes` (`lib/core/src/random.c`, real
  urandom; only a fault-injection env var, no deterministic seed hook). The
  `seed_tape` (`lib/sim/src/seed_tape.c`) does NOT reach it. Proofs still
  VERIFY, but tx bytes/txids are non-reproducible run-to-run — breaks the
  deterministic-simulator contract. Needs a sim/test-only seeded-RNG injection
  into the prover.
- **Param files not bundled / large.** 48 MB spend + 3.5 MB output proving
  keys must exist at `~/.zcash-params`; not in-repo, not deterministic to
  produce. The shielded-sim gate must be opt-in / skip-gated (like
  test_snark_kat) or CI must stage the files.
- **Proving wall-clock.** Real Groth16 spend proof is seconds (pure-C, no
  GPU). Too slow for the fast `test_parallel` default pool — belongs in a
  params-heavy opt-in gate.
- **Sapling anchor/witness + tree not built in sim today.** Sim sets
  `has_chain_sapling_value=false` and an all-zero root; shielded needs a live
  note-commitment tree to produce anchors/witnesses AND the header root. This
  is the largest net-new build.

## 4. Recommended lane decomposition (effort tier)

- **Lane A — Sonnet:** sim-local Sapling/Overwinter activation profile on the
  `params` value-copy; verify `s->params.consensus` is threaded through
  connect_block/contextual_check (not a global read). Prove no mainnet leak.
- **Lane B — Opus:** seedable prover RNG — inject a seed_tape-driven source
  into `sapling_generate_r` behind a sim/test-only hook; production stays on
  `GetRandBytes`. Crypto-hygiene sensitive.
- **Lane C — Opus:** in-sim Sapling note-commitment tree + anchor/witness +
  `hashFinalSaplingRoot`; wire t→z and z→z builds through the real prover.
- **Lane D — Sonnet:** nullifier double-spend negative gate (replay spent
  nullifier, assert `utxo_apply_nullifiers` reject).
- **Lane E — Haiku:** params-present detection + opt-in gate registration
  (SKIP when `~/.zcash-params` absent), keep out of the fast parallel pool.

## 5. Consensus-touch flags (owner-gated per docs/CONSENSUS_PARITY_DOCTRINE.md)

- **Activation-height override (Lane A) is the ONLY consensus-adjacent move.**
  It is safe IFF it mutates ONLY the sim's local `params` value-copy and never
  `chain_params_get()` / the mainnet definition in
  `lib/chain/src/chainparams.c`. Must audit that connect_block and
  `contextual_check_transaction` read the PASSED `params->consensus`, not a
  global. If any change instead edits mainnet activation heights or the
  verifier, it is a consensus change → **owner-gated, do not ship to
  zclassic23 first.**
- **The verifier is inviolable.** Do NOT add a `skip_proofs` shortcut, weaken
  `check_spend/check_output/final_check`, or bypass `utxo_apply_nullifiers` —
  that would violate the parity doctrine. The sim must exercise the real
  verifier unchanged.
- Prover, simnet harness, note-commitment tree, and seeded-RNG injection are
  all NON-consensus and require no owner gate.

# The Sync Keystone — bind the UTXO set to the PoW-proven chain

> Built 2026-06-20 from a grounded, adversarially-verified research pass
> (`sync-truth-research`). Every claim here is file:line-checked or a live
> measurement. Supersedes the optimistic "the cure is built" framing.

## The one root cause

The UTXO set is **never bound to the PoW-proven chain.** The only object
FlyClient verifies with PoW + an inclusion proof is the **MMB leaf**
(`lib/chain/include/chain/mmb.h:41-50`): `{block_hash, height, timestamp,
nBits, sapling_root, chain_work}` — preimage 108 bytes, **no `utxo_root`**.

So every UTXO trust check degenerates to one of:
- **circular** — verify the offered set against the *peer's own* claimed root
  (`app/services/src/snapshot_verify.c:132`: `memcmp(local_root, offered_utxo_root)`),
- or **one borrowed height** — the single zclassicd-derived checkpoint
  (`lib/chain/src/checkpoints.c:86-104`, provenance "verified bit-for-bit
  against zclassicd at 3,056,758").

Above that one height there is **no independent per-height UTXO authority**, so a
height-correct / state-wrong coin is undetectable by construction. That single
absence is simultaneously why (1) the network fast path can't be trusted, (2) a
borrowed wrong-fork coin wedges forward sync until a hard spend-miss, and (3) the
node must borrow zclassicd's chainstate at all. **Bind a non-forgeable per-height
SHA3 UTXO root into the PoW-proven leaf and all three collapse into one solved
problem.**

A per-height commitment structure **already exists**: `struct mmr_commitment`
(`lib/chain/include/chain/mmr.h:71-80`) carries `utxo_root[32]` and folds it into
its SHA3 leaf hash. But it is (a) fed a **forgeable XOR accumulator** on the live
connect path (`app/controllers/src/blockchain_controller.c:266`,
`memcpy(c.utxo_root, xor_accumulator, 32)`) instead of the real SHA3 set root, and
(b) **not** the FlyClient-proven object. The real SHA3 root
(`utxo_commitment_sha3_compute`) is computed only on the snapshot-OFFER path.

## Live truth (grounded 2026-06-20, not a stale story)

- H\* served `getblockcount` = **3,056,758** (the MIN-prefix provable frontier,
  `reducer_frontier.c`) — pinned at the anchor by the hole at h=3,056,759.
- active chain = **3,151,411**, stuck there (not closing the gap to header tip
  3,154,459). verificationprogress 0.9975.
- The node honestly NAMES its halt. **Honest paralysis is still paralysis.**

## The impressive-sync architecture (what "fast + trustless" actually is)

0. **(one-time, offline / per-release)** Self-mint the anchor: `-mint-anchor`
   drives the validated genesis→anchor fold on the *builder's* machine, writes a
   SHA3-committed snapshot, asserts it == the compiled root. The checkpoint
   becomes self-minted, not borrowed.
1. **(fresh node, ~1s)** Load the baked anchor snapshot, SHA3-verify the body
   against the compiled root, seed coins_kv at the anchor. No P2P, no zclassicd.
2. **(~seconds)** FlyClient header proof: ~20 PoW-checked, MMB-included samples +
   an `nMinimumChainWork` floor. Proves the peer's header chain is real high-work PoW.
3. **(THE BINDING)** Every PoW-committed leaf carries `utxo_root`. FlyClient forces
   the anchor height to be a mandatory sample and asserts `leaf.utxo_root == the
   root we loaded` — so the UTXO set is bound to PoW-proven chain state, not the
   peer's claim.
4. **(minutes on first sync)** Delta-fold anchor+1..tip; each 100-block window's
   `coins_kv_commitment` is checked against that height's PoW-proven leaf
   `utxo_root` → a state-wrong coin is caught + bisected + bound-window-repaired,
   never wedges.
5. **(steady state)** Serve H\* only; serve the same minted snapshot + bound roots
   to the next fresh node. Network bootstraps itself, zero zclassicd dependency.

Honest floor: ~60s is the snapshot-load + header-proof phase **after** a
self-minted artifact exists AND the delta is small. A ~94k-block serial delta is
minutes. "60s trustless cold sync" is a design target, not today's reality.

## Build order (each step independently verifiable; parity-inviolable; bootable)

1. **KEYSTONE — bind a real per-height SHA3 utxo_root into the PoW-proven leaf.**
   Either add `utxo_root[32]` to `struct mmb_leaf` (bump preimage 108→140, fold
   into `mmb_hash_leaf`) OR FlyClient-prove the `mmr_commitment` leaves that already
   carry it — whichever is cleaner. CRITICAL: populate it from `coins_kv_commitment(H)`
   (the real SHA3 set root) on the live connect path (`tip_finalize_post_step.c:188-197`),
   NOT the XOR accumulator. Test: a `zcl_mmb` proof for H round-trips a leaf whose
   `utxo_root == coins_kv_commitment(H)`. Parity-inviolable (internal authenticated
   structure; zclassicd validates nothing here). Kills the XOR forgeability.
2. **Verify against the bound root.** `snapsync_verify_flyclient` forces the anchor
   as a mandatory sample and asserts `leaf.utxo_root == offered root` before the SHA3
   memcmp. Removes the circular trust. Adversarial test: a fabricated set + genuine
   headers must now be REJECTED.
3. **Self-mint + replace the borrowed checkpoint.** Wire LB-1 (parallel verify) into
   the genesis→anchor fold so the mint saturates all cores (the riskiest sub-project:
   the reducer holds coins_kv + the LOCK-ORDER LAW); run `-mint-anchor` to actually
   produce the artifact (NEVER yet produced on a real datadir); assert SHA3 == the
   compiled root ONCE, then declare the minted artifact authoritative.
4. **Per-window divergence detection on the live fold** (uses step 1). Retire the dead
   Commitment-MMR + XOR + the self-certifying seal.
5. **Delete the borrowed-seed copy path; bodies-only-from-anchor default cold-start.**
   Gated on 1-4 green; keep the ~25-min crutch as the recovery floor.

## What we had wrong (frame errors — don't repeat)

- Optimizing a ONE-TIME mint's per-block rate as if it were the hot path.
- Treating the borrowed-seed wedge as a repair problem; it is a symptom of the
  missing binding.
- Reading the existence of built-but-DEAD code (LB-1 unwired, ibd_throttle
  zero-callers, Commitment-MMR 1 leaf, XOR root) as progress. Built ≠ live.
- Calling the stack "self-sovereign" while the checkpoint stays borrowed.

## Open dependencies the truth-check flagged (don't gloss)

- Step 3's mint is GATED on reproducing the borrowed zclassicd root (its own
  hard-assert, `boot_mint_anchor.c:150`) — the borrowed checkpoint is the mint's
  acceptance oracle even in the plan that deletes it (reproduce-then-declare).
- LB-1 wiring into the staged reducer is a real concurrency hazard (LOCK-ORDER LAW).
- A `-mint-anchor` run has never completed + emitted an artifact — until it does,
  steps 3-5 rest on an unproduced artifact.
- Spot-check before quoting as fact: Commitment-MMR dead-leaf, ibd_throttle
  zero-callers, connect_block money-rule dead, the 61GB figure, the seal
  self-certificate (confirmed: verify_queue dead + the XOR binding).

# The Sync Keystone — historical proposal (superseded)

> **CORRECTION (2026-07-12):** this document's proposed MMR/MMB `utxo_root`
> cannot bind peer-provided state to ZClassic PoW. ZClassic headers do not
> commit that root; a peer can construct an internally consistent auxiliary
> leaf and payload together. Changing ZClassic consensus first is forbidden.
> Treat everything below as historical problem analysis only. The valid path is
> local full-history derivation or explicitly release/peer-assisted state that
> remains restricted until background self-verification promotes it. Canonical
> is currently wedged at H*=3,176,325 on incomplete shielded history; commit
> `ab512d577` fixed only the earlier transparent-loader failure. Current plan:
> [`SOVEREIGN-NETWORK-ROADMAP.md`](../SOVEREIGN-NETWORK-ROADMAP.md).
>
> Built 2026-06-20 from a grounded, adversarially-verified research pass. Every
> claim here is file:line-checked or a live measurement.

## The one root cause

The UTXO set is **never bound to the PoW-proven chain.** At the time of this
proposal the sampled **MMB leaf** had no `utxo_root`. The current leaf carries
an auxiliary root, but ZClassic headers still commit neither that field nor
the MMB root, so the trust conclusion is unchanged.

So every UTXO trust check degenerates to one of:
- **circular** — verify the offered set against the *peer's own* claimed root
  (`app/services/src/snapshot_verify.c:132`: `memcmp(local_root, offered_utxo_root)`),
- or **one borrowed height** — the single zclassicd-derived checkpoint
  (`lib/chain/src/checkpoints.c:86-104`, provenance "verified bit-for-bit
  against zclassicd at 3,056,758").

Above that one height there is **no independent per-height UTXO authority**, so a
height-correct / state-wrong coin is undetectable by construction. That single
absence is simultaneously why the network fast path must remain assisted and a
borrowed wrong state can wedge forward sync. The old proposal below attempted
to add a root to an auxiliary leaf, but that cannot make the root a ZClassic
consensus commitment; only local derivation can promote sovereignty without a
forbidden consensus change.

A per-height commitment structure **already exists**: `struct mmr_commitment`
(`lib/chain/include/chain/mmr.h:71-80`) carries `utxo_root[32]` and folds it into
its SHA3 leaf hash. But it is (a) fed a **forgeable XOR accumulator** on the live
connect path (`app/controllers/src/blockchain_controller.c:266`,
`memcpy(c.utxo_root, xor_accumulator, 32)`) instead of the real SHA3 set root, and
(b) **not** the sampled MMB object. The real SHA3 root
(`utxo_commitment_sha3_compute`) is computed only on the snapshot-OFFER path.

## Historical snapshot (2026-06-20 — superseded; canonical now wedged at H*=3,176,325)

Prior (2026-06-20) state, kept as a dated record:
- H\* served `getblockcount` = **3,056,758** (the MIN-prefix provable frontier,
  `reducer_frontier.c`) — was pinned at the anchor by the hole at h=3,056,759.
- active chain = **3,151,411**, stuck there (not closing the gap to header tip
  3,154,459). verificationprogress 0.9975.
- The node honestly NAMED its halt. **Honest paralysis was still paralysis.**

The 2026-06-23 at-tip observation was temporary historical evidence. As of
2026-07-12 canonical is wedged at H*=3,176,325; the borrowed artifact did not
provide complete historical anchors/nullifiers. Its digest and header match do
not prove state derivation.

## Historical fast-sync proposal (corrected trust boundaries)

0. **(one-time, offline / per-release)** Self-mint the anchor: `-mint-anchor`
   drives the validated genesis→anchor fold on the *builder's* machine, writes a
   SHA3-committed snapshot, asserts it == the compiled root. The checkpoint
   becomes self-minted, not borrowed.
1. **(fresh node, ~1s)** Load the baked anchor snapshot, SHA3-verify the body
   against the compiled root, seed coins_kv at the anchor. No P2P, no zclassicd.
2. **(~seconds)** FlyClient header proof: ~20 PoW-checked, MMB-included samples +
   an `nMinimumChainWork` floor. Proves the peer's header chain is real high-work PoW.
3. **(SUPERSEDED IDEA)** Carrying `utxo_root` in a peer-built auxiliary leaf and
   sampling the height does not bind it to ZClassic PoW; the peer controls both
   the leaf root and snapshot. Keep this only as an integrity/cache signal.
4. **(sovereign replacement)** Delta-fold locally validated bodies into a
   separate namespace and compare complete state at the serving tip before
   atomic trust promotion.
5. **(steady state)** Serve H\* only; a served snapshot remains assisted at the
   receiver until that receiver completes local sovereignty promotion. Network
   distribution removes zclassicd runtime dependency, not the need to verify.

Honest floor: ~60s is the assisted snapshot-load + header-evidence phase **after** a
self-minted artifact exists AND the delta is small. A ~94k-block serial delta is
minutes. A "60s trustless cold sync" claim is unsupported under current
ZClassic consensus; the valid target is fast assisted readiness plus background
local promotion.

## Build order (each step independently verifiable; parity-inviolable; bootable)

1. **SUPERSEDED — an auxiliary SHA3 root does not become PoW-committed state.**
   Either add `utxo_root[32]` to `struct mmb_leaf` (bump preimage 108→140, fold
   into `mmb_hash_leaf`) OR FlyClient-prove the `mmr_commitment` leaves that already
   carry it — whichever is cleaner. CRITICAL: populate it from `coins_kv_commitment(H)`
   (the real SHA3 set root) on the live connect path (`tip_finalize_post_step.c:188-197`),
   NOT the XOR accumulator. Test: a `zcl_mmb` proof for H round-trips a leaf whose
   `utxo_root == coins_kv_commitment(H)`. Parity-inviolable (internal authenticated
   structure; zclassicd validates nothing here). Kills the XOR forgeability.
2. **SUPERSEDED — comparing two peer-controlled roots does not remove circular
   trust.** A mandatory sample and `leaf.utxo_root == offered root` check remains
   useful for corruption detection only. A fabricated set plus self-consistent
   auxiliary evidence must remain assisted, never be promoted by this check.
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

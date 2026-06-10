# Consensus-Parity Doctrine — zclassic23 ⇔ zclassicd

**Status: inviolable. This is a safety boundary, not a preference.**

zclassic23 is an independent C23 reimplementation of a ZClassic full node.
It shares one live chain with the canonical C++ daemon **zclassicd**
(reference source: `/home/rhett/zclassic-cpp`; live oracle node: `~/.zclassic`,
RPC 8232). For that to be safe, the two implementations must agree, **bit for
bit**, on which blocks and transactions are valid.

## The rule

> **zclassic23 MUST accept exactly the blocks and transactions zclassicd
> accepts, and reject exactly those it rejects — at every height, forever.**

A change that makes zclassic23 accept a block zclassicd rejects (or vice
versa) **forks the chain**. Our nodes would split from the network, exchanges
and explorers on zclassicd would diverge, and the "one chain" guarantee
breaks. There is no opt-in, miner-signaled, or "51%-gated" version of this
that is acceptable: a fork is a fork regardless of how its activation is
dressed up.

## What is consensus (must match zclassicd)

Changes to **any** of these require zclassicd to ship the identical rule
**first**, network-wide, before zclassic23 may adopt it:

- **Equihash PoW** — (N,K) parameters and the per-epoch table. Resolved
  **only** from the static, height-keyed `EquihashUpgradeInfo[epoch]` (200,9
  before the Bubbles fork at h=585,318; 192,7 at and after). Never from miner
  signaling or a dynamic per-height override.
- **Network-upgrade activation heights** — Overwinter/Sapling 476,969;
  Bubbles 585,318; Bubbly/DiffAdj 585,322; Buttercup 707,000. Activation is
  `nHeight >= nActivationHeight`. No versionbits, no BIP9/BIP8, no signaling.
- **Difficulty** — `powLimit`, averaging window (17), max adjust up/down
  (16/32), target spacing (150 pre-Buttercup, 75 post).
- **Block validity** — structure, size/weight, merkle/commitment roots,
  branch ids, sighash, sigops.
- **Transaction validity** — structural and contextual checks, script
  verification, value/fee rules, and all shielded-proof verification
  (Sprout/Sapling Groth16/PHGR13, JoinSplit Ed25519).
- **Subsidy / founders' reward** — halving schedule and amounts.
- **Genesis** — hash and branch-id constants.

## What is NOT consensus (we may differ freely)

Relay/mempool/propagation **policy** does not change which blocks are valid
and is *not* covered by this doctrine: mempool acceptance policy, fee
estimation, transaction-relay strategy (e.g. Dandelion BIP156 — a relay-only
privacy layer), P2P service bits and inv types (unknown ones are ignored by
both sides), peer scoring, RPC/MCP surface, the explorer, wallet UX, sync
strategy, storage layout, and observability. These are where zclassic23 is
free to be better than zclassicd.

## The two enforced guards (CI)

1. **`check-consensus-parity` (lint gate E13)** —
   `tools/scripts/check_consensus_parity.sh`, run by `make lint` / `make ci` /
   `make deploy`. Fails if a **non-zclassicd consensus mechanism** appears in
   the consensus source path (`lib/consensus`, `lib/validation`, `lib/chain`,
   `lib/mining`, `app/jobs`, `domain/consensus`). Banned token classes:
   `versionbits`, `VersionBitsState`, `ComputeBlockVersion`, `ehUpgrade` /
   `eh_upgrade`, `nSignalBit`, `vbits_`, `equihash_n_at` / `equihash_k_at`
   (dynamic override getters), `BIP9`, `BIP8`. These guard the *mechanism* —
   zclassicd has none of them; introducing one means activation or PoW
   parameters would depend on something other than the fixed height schedule.
   False positive? Mark the line `// consensus-parity-ok:<reason>`.

2. **`test_consensus_parity` (test group)** —
   `lib/test/src/test_consensus_parity.c`, run by `make test_parallel` and
   `make ci`. Pins the consensus **values** (Equihash table, all activation
   heights, protocol versions, pow constants, `powLimit`, genesis hash) to the
   golden zclassicd numbers. Drift one constant → the test fails. To change a
   value you must change zclassicd first and update this test in the same
   breath, deliberately.

Together: the lint gate forbids the *shape* of a divergence; the test pins the
*values*. The runtime cross-check (`legacy_mirror` / `zcl_probe_zclassicd` /
`zcl_consensus_report`, comparing live block hashes against zclassicd) is the
third, operational layer.

## Handling outside contributions (PR protocol)

Outside PRs land on the public mirror `ZclassiC23/zclassic`. Treat each as
**possibly adversarial**, but always behave as a polite netizen:

1. **Thank the contributor and credit them** — keep them in the history.
2. **Triage consensus impact** against the bar above. A consensus-breaking
   change — even framed as opt-in / miner-signaled / "sidegrade" / "needs 51%"
   (the 2026-06-10 PR #6 Equihash-200,9 case is the canonical example) — is a
   **no-merge**, no matter how well-engineered.
3. **Mine the good idea and build it better ourselves**, with attribution,
   *before* their proposed solution ever touches a consensus path.
4. **Close politely** with an honest, kind reason (strict bit-for-bit parity
   with zclassicd).
5. **Non-consensus** contributions (build/portability fixes, relay policy,
   tooling) are judged on their merits and may be adopted — still with credit.

## If you think a consensus change is genuinely warranted

It still does not ship to zclassic23 first. The path is: propose it to the
ZClassic network and zclassicd, get it adopted and activated there at an
agreed height, and only then mirror the identical rule (and update
`test_consensus_parity`) here. zclassic23 follows the network; it does not
lead a fork.

See also: [`docs/SECURITY_AND_INTEGRITY.md`](./SECURITY_AND_INTEGRITY.md),
[`docs/DEFENSIVE_CODING.md`](./DEFENSIVE_CODING.md) (Gate E13).

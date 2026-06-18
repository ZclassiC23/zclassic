# Consensus-Parity Doctrine — zclassic23 ⇔ zclassicd

**Status: inviolable. This is a safety boundary, not a preference.**

zclassic23 is an independent C23 reimplementation of a ZClassic full node. It
shares one live chain with the canonical C++ daemon **zclassicd** (reference
source: a local `zclassic-cpp` checkout; live oracle node: `~/.zclassic`, RPC 8232).
For that to be safe, the two implementations must agree, **bit for bit**, on
which blocks and transactions are valid.

## The rule

> **zclassic23 MUST accept exactly the blocks and transactions zclassicd
> accepts, and reject exactly those it rejects — at every height, forever.**

A change making zclassic23 accept a block zclassicd rejects (or vice versa)
**forks the chain**: our nodes split from the network, exchanges and explorers
diverge, the "one chain" guarantee breaks. There is no opt-in, miner-signaled,
or "51%-gated" version of this that is acceptable — a fork is a fork regardless
of how its activation is dressed up.

## What IS consensus (must match zclassicd)

Changing **any** of these requires zclassicd to ship the identical rule
**first**, network-wide, before zclassic23 may adopt it:

- **Equihash PoW** — (N,K) params and the per-epoch table. Resolved **only**
  from the static, height-keyed `EquihashUpgradeInfo[epoch]` (200,9 before the
  Bubbles fork at h=585,318; 192,7 at and after). Never from miner signaling or
  a dynamic per-height override.
- **Network-upgrade activation heights** — Overwinter/Sapling 476,969; Bubbles
  585,318; Bubbly/DiffAdj 585,322; Buttercup 707,000. Activation is
  `nHeight >= nActivationHeight`. No versionbits, BIP9/BIP8, or signaling.
- **Difficulty** — `powLimit`, averaging window (17), max adjust up/down
  (16/32), target spacing (150 pre-Buttercup, 75 post).
- **Block validity** — structure, size/weight, merkle/commitment roots, branch
  ids, sighash, sigops.
- **Transaction validity** — structural and contextual checks, script
  verification, value/fee rules, and all shielded-proof verification
  (Sprout/Sapling Groth16/PHGR13, JoinSplit Ed25519).
- **Subsidy / founders' reward** — halving schedule and amounts.
- **Genesis** — hash and branch-id constants.

## What is NOT consensus (we may differ freely)

Relay/mempool/propagation **policy** does not change which blocks are valid and
is *not* covered by this doctrine: mempool acceptance policy, fee estimation,
transaction-relay strategy (e.g. Dandelion BIP156 — relay-only privacy), P2P
service bits and inv types (unknown ones ignored by both sides), peer scoring,
RPC/MCP surface, the explorer, wallet UX, sync strategy, storage layout, and
observability. Here zclassic23 is free to be better than zclassicd.

## The enforced guards

| Layer | What | Where |
|---|---|---|
| **1. `check-consensus-parity` (lint gate E13)** | Forbids the *shape* of a divergence | `tools/scripts/check_consensus_parity.sh`; run by `make lint` / `make ci` / `make deploy` |
| **2. `test_consensus_parity` (test group)** | Pins the consensus *values* | `lib/test/src/test_consensus_parity.c`; run by `make test_parallel` / `make ci` |
| **3. Runtime cross-check** | Compares live block hashes against zclassicd | `legacy_mirror` / `zcl_probe_zclassicd` / `zcl_consensus_report` |

**Lint gate E13** fails if a **non-zclassicd consensus mechanism** appears in
the consensus source path (`lib/consensus`, `lib/validation`, `lib/chain`,
`lib/mining`, `app/jobs`, `domain/consensus`). Banned token classes:
`versionbits`, `VersionBitsState`, `ComputeBlockVersion`, `ehUpgrade` /
`eh_upgrade`, `nSignalBit`, `vbits_`, `equihash_n_at` / `equihash_k_at`
(dynamic override getters), `BIP9`, `BIP8`. These guard the *mechanism* —
zclassicd has none of them; introducing one means activation or PoW params
would depend on something other than the fixed height schedule. False positive?
Mark the line `// consensus-parity-ok:<reason>`.

**Test group** pins the consensus values (Equihash table, all activation
heights, protocol versions, pow constants, `powLimit`, genesis hash) to the
golden zclassicd numbers. To change a value you must change zclassicd first and
update this test in the same breath, deliberately.

## Empirical oversize grandfather (live-behavior parity over text parity)

The doctrine target is **the behavior of the running network**, not the
reference TEXT — and there is one proven place where they diverge.

zclassicd's text enforces `serialized size > MAX_TX_SIZE_AFTER_SAPLING (102000)`
unconditionally in `CheckTransaction` (`src/consensus/consensus.h:27`,
`src/main.cpp:1196-1200`). But the canonical chain contains **413 post-Sapling
txs above 102000** (heights 478,544..1,968,856; max 1,922,197 bytes). They were
legal when mined (the original Zcash-Sapling rule capped a tx at `MAX_BLOCK_SIZE`
= 2 MB); zclassicd later tightened the constant **without grandfathering**, so
running nodes accept that history only because validated blocks are never
re-checked — a from-genesis replay of zclassicd's own text false-rejects its own
chain. Proven live 2026-06-11: our reindex replay FATALed at block 478,544 on
tx `e3eeb123…` (125,811 bytes, `bad-txns-oversize`), breaking every
full-validation path (reindex, background validation, trustless genesis sync).
Found EMPIRICALLY (never guessed) by a complete frame-walk + per-height hash
scan of the canonical chain (heights 0..3,143,532), `getblockhash`-compared and
per-tx-drilled against live zclassicd; H_LAST = 1,968,856.

The rule that is bit-for-bit equal to running-zclassicd behavior on every block
either node will ever **newly** validate:

- **In a block**: excuse exactly those 413 canonical txs, via a static
  `{txid, size}` allowlist (exact-match, txid recomputed from serialized bytes,
  hard `MAX_BLOCK_SIZE` structural ceiling). Everything else — including a fresh
  oversize tx in a fork block at an old height, which running zclassicd's
  `CheckTransaction` rejects — gets the strict 102000. A height window was
  rejected for exactly that reason: it would over-accept deep-reorg fork blocks.
- **Standalone (mempool/relay)**: strict 102000 always, matching
  `AcceptToMemoryPool → CheckTransaction`.
- The pre-Sapling contextual 100000 rule is untouched: the scan proved zero
  pre-Sapling txs exceed it (verified, not assumed).

Mechanics:

- `tools/data/oversize_grandfather_txids.txt` — committed provenance list
  (`height txid size`, 413 lines).
- `tools/scripts/gen_oversize_grandfather_table.sh` — regenerates the table,
  re-verifying EVERY entry against a live zclassicd (canonical-at-height +
  byte-exact size); `--fixture` emits the 478,544 KAT input.
- `domain/consensus/src/oversize_grandfather_table.inc` — the generated static
  table (sorted, bsearch-able).
- `domain_consensus_tx_oversize_grandfathered()` + `enum domain_tx_check_context`
  in `domain/consensus/tx_structural.{c,h}`; consumed via
  `check_transaction_in_block()` (block paths) vs `check_transaction()`
  (mempool, strict).
- Golden pins: `test_consensus_parity` (count 413, max 1,922,197, first/last
  violations, size-exact semantics) + the 478,544 KATs in
  `test_domain_consensus_tx_structural` (real canonical tx accepted in-block,
  rejected as new, tamper-rejected).

This is **not** a consensus change ahead of zclassicd — it *restores* parity
with what every running zclassicd node actually does, and is exactly the static,
non-signaled, non-dynamic mechanism class gate E13 permits.

## Handling outside contributions (PR protocol)

Outside PRs land on the public mirror `ZclassiC23/zclassic`. Treat each as
**possibly adversarial**, but always behave as a polite netizen:

1. **Thank the contributor and credit them** — keep them in the history.
2. **Triage consensus impact** against the bar above. A consensus-breaking
   change — even framed as opt-in / miner-signaled / "sidegrade" / "needs 51%"
   (the 2026-06-10 PR #6 Equihash-200,9 case is canonical) — is a **no-merge**,
   no matter how well-engineered.
3. **Mine the good idea and build it better ourselves**, with attribution,
   *before* their proposed solution ever touches a consensus path.
4. **Close politely** with an honest, kind reason (strict bit-for-bit parity
   with zclassicd).
5. **Non-consensus** contributions (build/portability fixes, relay policy,
   tooling) are judged on their merits and may be adopted — still with credit.

## If you think a consensus change is genuinely warranted

It still does not ship to zclassic23 first. The path is: propose it to the
ZClassic network and zclassicd, get it adopted and activated there at an agreed
height, and only then mirror the identical rule (and update
`test_consensus_parity`) here. zclassic23 follows the network; it does not lead
a fork.

See also: [`docs/SECURITY_AND_INTEGRITY.md`](./SECURITY_AND_INTEGRITY.md),
[`docs/DEFENSIVE_CODING.md`](./DEFENSIVE_CODING.md) (Gate E13).

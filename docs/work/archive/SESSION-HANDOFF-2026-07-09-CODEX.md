# Codex stabilization handoff — 2026-07-09

This handoff covers the stabilization work based on `d8d9b32e9` and pushed
directly to `main` at the owner's request. It is the first document to read
after `docs/HANDOFF.md`.

## Deployment status: HOLD

**Do not deploy this commit yet.** The tree builds, lints, passes the complete
strict test suite, and has a green dependency audit, but one deliberate
fail-closed data migration and two wallet publication/rollback races remain
owner-gated work:

1. Existing imported/snapshot datadirs do not have a complete historical
   Sprout/Sapling anchor set. They now name the permanent blocker
   `utxo_apply.anchor_backfill_gap` and fail closed on unknown anchors. Build
   the owner-gated genesis-to-current-cursor backfill before deploying this
   tree to an existing datadir.
2. Wallet commit still publishes a transaction to the mempool before the
   corresponding wallet rows and spend reservations are durable. Split this
   into validate-without-publication, durable wallet commit, then one atomic
   mempool publication step.
3. Wallet compensation is not ownership-aware: a failing concurrent attempt
   can clear an outpoint or Sapling note reserved by a different transaction.
   Persist the spender txid/attempt identity and compare-and-clear on undo.

No service was deployed or restarted and no live datadir was edited during
this session.

## What changed

### Durable shielded anchor membership

- Added `anchor_kv`, a progress-log-backed store for Sprout and Sapling roots
  and serialized frontiers.
- The `utxo_apply` reducer now checks every shielded transaction against the
  pre-block active anchor set, then folds and stores new frontiers in the same
  transaction as coins, nullifiers, the reducer log, and its cursor.
- Preserved zclassicd ordering: roots created by an earlier transaction in a
  block cannot satisfy a later transaction; same-transaction Sprout
  intermediate roots remain valid.
- Sapling folding cross-checks `hashFinalSaplingRoot`; reorg/reset paths unwind
  anchor state with the reducer.
- Incomplete imported history is explicit and fail-closed through
  `utxo_apply.anchor_backfill_gap`; no partial migration was shipped.

### Sapling proving integrity

- Pinned proving code to librustzcash commit
  `06da3b9ac8f278e5d4ae13088cf0a4c03d2c13f5` and source SHA-256
  `9909ec59fa7a411c2071d6237b3363a0bc6e5e42358505cf64b7da0f58a7ff5a`.
- Kept consensus verification in the C23 implementation; Rust is proving-only.
- Initialization now performs a real Spend proof, Output proof, binding
  signature, and independent verification self-test and fails closed.
- Corrected Sapling witness-path ordering and added deterministic/KAT coverage.

### Mempool and wallet fund safety

- Threaded the active chain state and coins view into a common mempool
  admission path; persistent mempool reload revalidates entries.
- Transaction copies are all-or-nothing under allocation failure.
- Wallet SQLite flushes now go through the node database's single-writer path
  with a documented lock order.
- Shielded sends reserve notes atomically and persist wallet rows with explicit
  compensation instead of using the old unchecked mempool insertion.
- Legacy keypool entries track exact key IDs and a generation watermark;
  unpersisted keys cannot be handed out.
- `getnewaddress` and `z_getnewaddress` refuse a missing wallet database and
  undo the exact generated key/child on persistence failure. Sapling undo is
  exact-child and LIFO.
- MCP address-generation tools are destructive and the MCP self-test proves it
  does not invoke them.

### Operator truth and sync state

- Chain evidence binds the persisted tip hash to the canonical projection
  instead of reporting height agreement alone.
- Sync decisions use local/header/peer heights plus block/header intake queues
  and quiescence, with a deterministic catch-up to `at_tip` transition.
- One-shot `mcpcall` initializes mainnet chain parameters before dispatch.

### Vendor provenance gates

- Added deterministic per-archive stamps binding the source pin, build recipe,
  flags, toolchain, dependency provenance, and output archive digest.
- Missing, stale, tampered, or dependency-mismatched stamps force a rebuild.
- Added the exact committed `libsecp256k1.a` hash/size manifest; its source
  remains honestly unresolved rather than inferred.
- CI, install, deploy, release, and remote-release paths now require proven
  vendor artifacts. Releases include `VENDOR-PROVENANCE`.
- The audit's JSON mode now exits nonzero when findings exist.
- The full migration rebuilt all archives and now passes `make audit` with
  OpenSSL 3.0.16. Generic tool fingerprinting no longer invokes compiler-only
  flags, LevelDB resolves an absolute CMake archiver, and build failures retain
  actionable logs.
- Rust builds remap both source and Cargo-home paths and reject any archive
  containing a build-host path. A pinned, digest-bound libevent compatibility
  patch preserves the RNG symbol required by embedded Tor, with a post-build
  symbol assertion.

## Verification on the final worktree

- `make -j8 build-only` — PASS (all node objects compiled).
- `make lint` — PASS (508 groups registered; advisory file-size warnings only).
- `tools/scripts/test_vendor_provenance.sh` — PASS.
- `make vendor` and `make audit` — PASS; every archive provenance-verified.
- `make zclassic23` — PASS under strict `-O3 -flto -Werror`; final binary has
  no build-user home paths.
- `make test` — PASS, 508/508 groups; 15 documented stress/live self-skips,
  zero failures.
- `make t-fast ONLY=wallet` — PASS, 33/33 matching groups.
- Focused anchor/UTXO/storage sweep — PASS, 22 unique groups (23 executions),
  zero failures/skips. This includes all 41 anchor-membership assertions.
- Focused MCP/sync/chain-evidence sweep — PASS, 5/5 groups, zero skips.
- `groth16_selfverify`, `accept_to_mempool`, and `wallet_funds_safety` —
  PASS, 3/3 groups, zero skips.
- `git diff --check` — PASS before final staging.

`make ci` is intentionally not claimed: its fuzz, coverage, crash, and
live/stress lanes are broader than this wrap-up. Its vendor-ready prerequisite,
strict production build, lint, and complete 508-group test suite all pass.

## Remaining safety work, in order

1. Implement the historical anchor backfill on a copied datadir. It must read
   canonical block bodies, verify chain binding, checkpoint crash-resumably,
   prove exact completion to the reducer cursor, and only then clear
   `utxo_apply.anchor_backfill_gap`.
2. Redesign wallet publication ordering and ownership-aware rollback as
   described in the HOLD list. Add deterministic two-thread and crash-window
   tests before enabling relay.
3. Make durable compensation atomic. A failed SQLite delete must leave a named
   blocker/retry record rather than only reverting RAM.
4. Bind Sapling child undo to the active seed epoch (or serialize seed reset
   with address derivation).
5. Run a real genesis-to-tip replay on a copied chain before changing any
   default-off parity-tightening flag: `-enforce-coinbase-maturity`,
   `-enforce-sapling-root`, or `-enforce-checkdatasig-sigops`.
6. Return to the v1 sovereign-cure and soak gates after the safety migrations.

## Lower-priority product truth

Name/ZSLP admission now uses the shared context, but these surfaces remain
off-v1 and are not end-to-end settlement systems. Name/ZSLP calls can still
overstate “broadcast” without an explicit durable relay result, the ZSLP
balance model is credit-only, market payment settlement is incomplete, and
atomic-swap redeem/refund/settlement remains unwired. Do not advertise these as
finished until their durable network lifecycle exists.

## Live MCP self-test incident

During read-only verification, the old `zcl_self_test` routing table invoked
`getnewaddress` and `z_getnewaddress` at approximately 19:20:14 UTC because
they were not marked destructive. The live wallet database was locked, so no
wallet row, HD child counter, transaction, or funds changed persistently. The
running process did acquire 100 transparent keypool entries and derive one
Sapling child in memory; they disappear on an ordinary future restart. No
restart or cleanup was performed in this session. The routes are now marked
destructive and a regression proves self-test performs zero calls to them.

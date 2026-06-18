# ZClassic23 Sync Guide

How a fresh zclassic23 node reaches chain tip, plus the one legacy-bootstrap
path that exists only while the native peer network is still small. Primary =
zclassic23-native; legacy = pulling data from the old C++ `zclassicd`.

## Canonical Authority Model

Canonical chain state is always **locally validated and evidence-published**.
The only publishable active tip is one that passes the local validation and
evidence gate:

- `chain_advance_coordinator` chooses which input source may provide candidate
  headers or bodies.
- `chain_activation_controller` is the block-connection entrypoint.
- the chain-evidence logic (now in `chain_evidence_persistence_service`/`_authority_service`/`_snapshot`)
  decides whether a tip transition has enough local evidence to publish.
- `chain_state_repository` performs the atomic in-memory/persistent state
  update, and `chain_tip` is the public active-tip publication wrapper.
- `legacy_mirror_sync_service` may fetch candidate data from `zclassicd` and
  request work, but it cannot make `zclassicd` a consensus authority.

`zclassicd` is removable advisory infrastructure that may accelerate bootstrap.
Matching it is not proof of canonical state; diverging from it is not by itself
a reason to rewind or publish a tip. Health/status surfaces must keep
`consensus_authority=local_consensus_validation`; `candidate_*` fields describe
source and trust class only. Any `unsafe_overrides_total > 0` is fail-loud.

---

## Method 1 (native): P2P Fast Sync (~60 s)

A fresh node downloads a verified UTXO snapshot from another zclassic23 peer,
then catches up the tail via standard P2P. Activation is automatic — any peer
advertising service bit `NODE_ZCL23` (`lib/net/include/net/fast_sync.h`)
becomes a snapshot candidate.

```bash
build/bin/zclassic23 -addnode=<zclassic23_peer>
```

What happens:
1. Find a peer advertising `NODE_ZCL23`.
2. Receive a strict v2 UTXO snapshot manifest (protocol/schema version,
   anchor height/hash, serving peer tip, chainwork, UTXO SHA3, byte length,
   UTXO count, chunk size/count, and per-chunk hashes).
3. Download chunks in parallel, verify each chunk hash before import.
4. Verify FlyClient MMR/MMB proof binding the UTXOs to the PoW chain and
   reject missing or non-competitive chainwork.
5. Verify the imported UTXO set SHA3 exactly matches the manifest.
6. Activate only an anchor at least 10 blocks behind the serving peer's tip,
   then delta-sync the finality window to tip by normal block validation.

**Use this for any fresh deployment where at least one other zclassic23 peer
exists.** Once the network is established it is the default, only user-facing path.

### zclassic-only serving profile

`-profile=zclassic-only` is for power nodes whose job is to sync other nodes
quickly. It keeps consensus state, P2P, RPC, snapshot offer construction,
FlyClient/MMB proof serving, and normal block relay; it does not start explorer
cache prewarming, store/market services, onion hosting (unless `-tor` is set), or
file-service snapshot export and chunk/block-piece manifests.

Full, onion-node, and legacy-compat profiles keep the broader app surfaces. The
explorer profile keeps explorer APIs and cache prewarming but still avoids store
and file-service serving.

---

## Method 2 (native): Full P2P Sync (~7 h)

Trustless sync from genesis over the standard P2P protocol. No snapshot.

```bash
build/bin/zclassic23 -addnode=<any_peer>
```

Headers → blocks → connect. Scripts/signatures below deferred proof validation
height (h=3,054,000) are accepted; full validation runs above that. Background
services then re-verify every hash, signature, and proof end to end.

Use when no snapshot source is available, or to validate from scratch.

---

## Method 3 (legacy bootstrap, development only): Import from zclassicd

**This path exists because the native peer network is still small on mainnet.**
It reads data from a synced legacy `zclassicd` (C++) on the same machine to get
developer workstations to tip fast. Its block files, UTXO snapshots, and
height/hash answers seed candidates only — tip publication still requires the
local activation/evidence path (see Canonical Authority Model above). It goes
away once the native peer network is healthy.

Requirements: a local synced legacy `zclassicd` with `~/.zclassic/` (leave it
running — P2P 8033 / RPC 8232).

This is the canonical home for the recipe — **two steps, in this order**:

```bash
# 1. Headers FIRST — imports ~3.1M headers in ~60-74 s from the legacy datadir.
build/bin/zclassic23 --importblockindex $HOME/.zclassic

# 2. Then a NORMAL boot — legacy import is on by default; it auto-reads/links
#    ~/.zclassic and reaches tip. Opt out with -nolegacyimport.
build/bin/zclassic23
```

Skipping step 1 is a footgun: importing UTXOs without the header import leaves a
~3.1M-header hole (headers=960) and the node pins. The old single-flag forms
(`-cold-import=`/`-fastimport=`) no longer exist — the argv loop ignores unknown
flags, so passing them silently no-ops.

**Caveat:** cold import is still slow + fragile (a ~12k-block header band
backfills over P2P, and the first boot can latch a transient freeze that needs a
restart). The robust path for a known-good datadir is to copy one onto the target
lane; the durable fix is tracked as C1/C2/O2 in
`work/stability-improvements-2026-06-16.md`.

Rules:
- The import flags **only run on an empty datadir** (or one below the legacy
  tip). They refuse if our active tip already meets/exceeds legacy.
- Legacy data is acceleration only. It must match compiled SHA3 windows,
  runtime windows, local consensus checks, or zclassic23 quorum before it
  elevates trust.
- Force reimport after a first run:
  `build/bin/zclassic23 -reimport-utxos -datadir=~/.zclassic-c23`

The legacy peer ships as the `zclassicd-peer` systemd user service (see CLAUDE.md "Services").

### Legacy chain oracle boundary

All direct reads from local legacy `zclassicd` RPC go through
`rpc/legacy_rpc_client.h` for transport and `rpc/legacy_chain_oracle.h` for
typed chain data such as block hashes, MMB leaves, and chainwork. Boot-time MMB
catchup, fast-sync offer construction, FlyClient proof fallback, and the
zclassicd drift oracle share that transport instead of parsing JSON-RPC in
their own service code.

This boundary keeps legacy compatibility behind a small adapter. Snapshot
acceptance still requires the native v2 manifest contract, FlyClient MMB/MMR
proofs, PoW/chainwork checks, finality policy, and UTXO SHA3 verification.

---

## Verification Layers

Every sync method reaches the same verified state. Background services run
after sync completes:

| Service | What it verifies | Speed |
|---------|------------------|-------|
| `bg_hash_verify` | SHA256d of every block header | 83K blk/s |
| `bg_validation`  | Equihash, ECDSA, Groth16, merkle roots | 400–500 blk/s |
| Boot checks      | UTXO count + XOR commitment vs checkpoint | ~2 s |
| Post-import      | SHA3-256 full UTXO set vs hardcoded commitment | ~5 s |

See [`validation/VALIDATION_MATRIX.md`](validation/VALIDATION_MATRIX.md) for the full matrix.

Self-healing recovery mechanisms (missing UTXO, reorg unwind, wrong block on
disk, stale `coins_best_block`, download stall) are documented in
[`validation/VALIDATION_MATRIX.md`](validation/VALIDATION_MATRIX.md) → "Self-Healing
Mechanisms".

---

## Data Directory

```
~/.zclassic-c23/
├── node.db                  SQLite (UTXOs, block index, node state, wallet)
├── node.db-{wal,shm}        WAL companion files
├── consensus_snapshot.db    SQLite (snapshot under construction or applied)
├── blocks/
│   ├── blk*.dat             Block data (4B magic + 4B size + block)
│   ├── rev*.dat             Undo data for reorgs
│   └── index/               LevelDB block index (legacy / cold-import only)
├── block_index.bin          Optional flat file cache (instant restart load)
├── mmb_leaves.bin           Merkle Mountain Belt leaf cache
├── file_manifest.bin        File-service chunk manifest
├── explorer/                Block-explorer cache (factoids, CSS)
├── tor_data/                Embedded Tor state (when -tor is set)
├── .cookie                  RPC auth cookie
└── node.log                 Structured event log
```

`node_state` SQLite keys are documented in
[`validation/VALIDATION_MATRIX.md`](validation/VALIDATION_MATRIX.md) → "SQLite
`node_state` keys".

Finality policy: `ZCL_FINALITY_DEPTH=10`. Heights `<= tip - 10` are treated as
immutable for reorg refusal, snapshot eligibility, rolling SHA3 anchors,
block-window reuse, and diagnostics. Steady-state reorgs of 10 blocks or less
are allowed; 11-block reorgs are refused. IBD can still resolve deeper
competition before a verified immutable anchor is installed. These decisions are
centralized in `validation/sync_evidence_policy.h`. `COINBASE_MATURITY=100`
remains a consensus spend-maturity rule and does not control reorg depth or
immutable-prefix policy.

Snapshot protocol: zclassic23 peers must speak
`FAST_SYNC_PROTOCOL_VERSION=2` and `FAST_SYNC_SNAPSHOT_SCHEMA_VERSION=1`.
Missing v2 fields, zero chainwork, non-final anchors, missing MMR/MMB roots,
or stale schemas are rejected. Legacy/non-v2 data may still be used locally
for bootstrap acceleration, but it is not trusted P2P snapshot sync.

Quorum model: votes are grouped by source class — local zclassic23, local
zclassicd, remote zclassic23 peers. Remote votes are keyed by unique peer and
expire by TTL; rolling-anchor commits require a matching source-class quorum when
multiple classes are available. Splits halt anchor extension and are visible
through the quorum/oracle dumpstate surface.

Rolling anchors: runtime SHA3 windows are persisted only for fully immutable
windows with local block bytes present, normal oracle policy, and quorum
approval. On load, runtime anchor files are checksum-, schema-, alignment-, and
continuity-checked against compiled anchors; failures discard the runtime file.

---

## Operator Runbook

### Check sync status
MCP: `zcl_status`, `zcl_kpi`, `zcl_syncstate`, `zcl_validationstatus`. Or the
`zcl-rpc` escape hatch: `build/bin/zcl-rpc getblockchaininfo`. Status/dumpstate
surfaces include sync phase, local/header/peer heights, immutable height,
snapshot anchor, UTXO root, chainwork/quorum verdict, watchdog state, last
recovery, and active acceleration source where available.

### Recovery from OOM kill
Just restart — the node detects stale state and resets to a consistent point.

### Nuclear reset
```bash
systemctl --user stop zclassic23
rm -rf ~/.zclassic-c23/node.db ~/.zclassic-c23/blocks/ ~/.zclassic-c23/chainstate/
rm -f  ~/.zclassic-c23/block_index.bin
systemctl --user start zclassic23
```
The node re-syncs via Method 1 or 2 from its configured peers.

---

## Architecture

```
        NATIVE                                   LEGACY BOOTSTRAP
┌──────────────────────┬──────────────────┐  ┌────────────────────┐
│  Method 1: fastsync  │ Method 2: full   │  │ Method 3: from     │
│  zclassic23 peer     │ P2P from genesis │  │ zclassicd          │
│  ~60 s               │ ~7 h             │  │ chainstate → SQLite│
│  NODE_ZCL23 + chunks │ headers + blocks │  │ ~20 s, dev-only    │
└──────────┬───────────┴────────┬─────────┘  └─────────┬──────────┘
           │                    │                      │
           ▼                    ▼                      ▼
         ┌───────────────────────────────────────────────┐
         │           UTXO SET (SQLite)                   │
         │  1.35M entries · coins_best_block @ tip       │
         └────────────────────┬──────────────────────────┘
                              ▼
         ┌───────────────────────────────────────────────┐
         │        BACKGROUND VERIFICATION                │
         │  bg_hash_verify · bg_validation · checkpoints │
         └───────────────────────────────────────────────┘
```

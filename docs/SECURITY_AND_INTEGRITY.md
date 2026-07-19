# Security and Integrity Model

ZClassic23's security model is built around operator ownership: one local
full-node binary, explicit network listeners, private wallet state, an
onion-hosted explorer, and a typed local native command operator surface. Tor support
publishes the operator's own service, wallet/key code stays inside the
operator's datadir, and fuzz/chaos harnesses exercise isolated recovery paths.
This document states the boundary, safeguards, and evidence that make those
properties auditable.

## Current status

- ZClassic23 is pre-v1 and in active stabilization. It is not production-ready
  and has no supported release line yet.
- The v1 bar is [`MVP.md`](./MVP.md). The project does not claim v1 until all
  eight operator acceptance criteria pass at the documented bar.
- Known gaps are documented instead of hidden. Examples include the current
  live forward-progress blocker, off-chain ZMSG plaintext transport, and
  incomplete marketplace/swap settlement paths.
- The June 2026 third-party security-audit disposition (fixed findings,
  refuted findings with citations, and deferred items) is folded into
  "Concrete safeguards" below.

## Operator-owned scope

This repository supports authorized operation and development of a ZClassic
node and its local operator surfaces:

- validating and serving the ZClassic P2P protocol;
- running an operator-owned wallet, block explorer, onion service, and RPC
  interface;
- testing this implementation with local unit tests, fuzzers, simulators,
  isolated regtest nodes, and consenting peers.

All network, wallet, fuzz, and chaos workflows are scoped to resources the
operator controls or has explicit permission to use. If a test needs a live
process, peer, datadir, network, or wallet, use operator-owned resources,
isolated fixtures, or consenting peers.

## Security properties by component

| Component | Purpose | Safety boundary |
|-----------|---------|-----------------|
| Embedded Tor | Publish the operator's own explorer/API as a hidden service | `-tor` is explicit; opt-in build (default links a stub, onion off); test harnesses disable Tor |
| P2P networking and peer scoring | Implement the public ZClassic node protocol | Peer policy protects consensus and network health |
| Wallet and key code | Local transparent/Sapling wallet operation | Diagnostics must not return private key material |
| Native commands | Local typed operator API for AI-assisted node operation | Destructive commands are explicit and privilege-gated |
| `zclassic23 dbquery` | Incident-response inspection of local `node.db` | SELECT-only, semicolon-rejected, limited, and rate-gated |
| Fuzzers, chaos, kill-9 harnesses | Find crashes and recovery bugs in this codebase | Isolated datadirs and ports; no live-node mutation |
| Atomic swap and market code | Application protocol scaffolding | Settlement gaps are documented; scaffolding is not claimed complete |

## Shielded transaction validation posture

This is stated explicitly because an auditor reading the source will find
`return true` in a shielded-validation helper and should know what it does and
does not mean.

- **Nullifier double-spend is enforced** on the live block-application (reducer
  fold) path — `utxo_apply_check_and_insert_nullifiers()`
  (`app/jobs/src/utxo_apply_nullifiers.c`): a two-pass check rejects a nullifier
  reused against the durable set or within the same block, then inserts the
  block's nullifiers only after it validates. The
  `coins_view_cache_have_joinsplit_requirements()` stub
  (`lib/coins/src/coins_view.c`, `return true`) is an **interface placeholder**;
  it is not the enforcement point.
- **Groth16 spend/output proofs, the binding signature, and the JoinSplit
  Ed25519 signature are checkpoint-gated ONLY in the legacy `connect_block()`
  path** (`lib/validation/src/connect_block.c`), equivalent to Bitcoin Core's
  `-assumevalid` (removed as a direct flag; controlled via
  `-deferproofvalidationbelow=<blockhash|0>`, default the highest in-binary
  PoW checkpoint, height 3,100,000; `lib/chain/src/chainparams.c`). That path
  is driven by `-reindex-chainstate` (`reindex_chainstate()` in
  `config/src/boot_index.c`), by the offline harness/simnet code, and by the
  background revalidation walker (`app/services/src/bg_validation_service.c`,
  `-nobgvalidation` to disable) — which itself re-verifies every proof it
  walks and clears the deferred-height gate once the walk passes it.
  **The reducer's state-advancing path — `proof_validate_stage()`
  (`app/jobs/src/proof_validate_stage.c`), which owns the durable `ok=1`
  cursor and H\* on a normal boot — verifies every Groth16 spend/output
  proof, binding signature, and JoinSplit Ed25519 signature
  UNCONDITIONALLY on every height; it has no checkpoint-gated skip.** The
  only crypto pass-through in that reducer pipeline is the `mint_skip_crypto`
  toggle (`app/jobs/include/jobs/mint_skip_crypto.h`), which is exclusively
  set by the offline one-shot `-mint-anchor` driver
  (`config/src/boot_mint_anchor.c`, gated under `ctx->mint_anchor`), defaults
  OFF on every normal boot (a normal boot never calls the setter), and its
  output is durably marked `checkpoint_fold` (never `verified`) —
  excluded from serving validity, H\*, and tip finalization. Enforced by the
  lint gate `check-mint-skip-crypto-offline-only` + `test_mint_skip_crypto`.
- **Anchor (note-commitment-tree root) membership is not checked independently.**
  It is certified *implicitly* by the Groth16 proof, whose circuit constrains the
  Merkle path of the commitment to equal the claimed anchor
  (`lib/sapling/src/sapling_circuit.c`). On the reducer path this proof is
  verified unconditionally (above); on the legacy `connect_block()` path it
  carries the same deferred-height gating as proof verification there.

Net: on the reducer path that actually advances the node's durable tip, a
reorg cannot exceed the bounded reorg depth AND every Groth16/Ed25519
proof/signature below tip is verified — there is no `assumevalid`-style trust
window on the live, state-advancing path. The `assumevalid`-equivalent
deferred-height gate is real but confined to the legacy `connect_block()`
reindex/import/simnet path, where it carries the same practical exposure as
any `assumevalid`-style node until the background walker (or a later
`-reindex-chainstate`) passes that height. Independent (non-`assumevalid`)
anchor-membership verification on that legacy path is tracked as a hardening
item, not a claimed property.

## Concrete safeguards

- **Defensive-coding gates:** `make lint` checks raw SQLite writes, raw
  allocation use, silent error paths, native command error bodies, supervisor liveness,
  app-shape boundaries, one-write-path rules, and no-silent-ready rules. The
  detailed contract is [`DEFENSIVE_CODING.md`](./DEFENSIVE_CODING.md).
- **Local integration gate:** `make ci` runs lint before tests, then the test
  harness, benchmark regression, hermetic MVP slice gates, crash tests, and
  fuzz smoke tests where the toolchain is available. This checkout contains two
  GitHub Actions workflows under `.github/workflows/` (`pr-security-review.yml`,
  `pr-security-comment.yml`) that run an automated, fork-safe security
  review/comment on pull requests; there is no hosted build/test CI workflow
  (CI runs locally via `make ci`).
- **Operator-private HTTP routes:** `/api/wallet`, `/api/messages`, and
  `/api/swaps` are classified operator-private (`api_route_is_operator_private`,
  `lib/net/src/https_server.c`) and 403'd before dispatch on the 0.0.0.0 TLS
  clearnet listener (no CORS header); public chain-data routes are unaffected,
  the onion listener exposes no `/api`, and the in-process `wallet_gui`
  consumer bypasses the listener entirely.
- **Landed fixes (2026-06-09 audit response, re-verified against HEAD):**
  destination-capacity guard in `script_get_op` before the opcode-data memcpy
  (was an up-to-~9994-byte copy into 520-byte caller buffers — remote
  DoS/corruption on the validation path), paired with a `script_get_sig_op_count`
  fix so sigops after an oversized push are never undercounted; a coinbase
  subsidy ceiling on the live reducer path (`app/jobs/src/utxo_apply_delta.c`,
  status `bad_cb_amount` — deliberately distinct from `value_overflow` so
  repair machinery never treats inflation as repairable); a consensus
  nullifier set (`nullifier_kv`, Sprout/Sapling separate namespaces, checked
  + inserted atomically with the coins commit inside the `utxo_apply` stage
  txn); wallet-backup encryption (`WALLET_BACKUP_PASSWORD` → ChaCha20-Poly1305
  via `wallet_backup_encrypt_file`; no password means plaintext continues with
  a loud boot warning, since refusing would silently kill the fleet-wide
  key-loss safety net); the operator-private HTTP routes above.
  **Known limit:** nullifier enforcement is activation-forward on
  snapshot-seeded datadirs (a from-genesis replay/reindex gets the complete
  set automatically); the pre-activation backfill gap is a permanent typed
  blocker (`utxo_apply.nullifier_backfill_gap`), remediated by the owner-gated
  `app/services/src/nullifier_backfill_service.c` populate-only walker.
- **Refuted findings (pinned so they are not "fixed" again):** the retarget
  half of "difficulty retarget not enforced on live ingest" is false — every
  P2P header runs `accept_block_header → contextual_check_block_header →
  GetNextWorkRequired` (`bad-diffbits`); and "SIGHASH_SINGLE should return the
  Bitcoin `uint256(1)` sentinel" is a false positive — zclassicd itself throws
  and catches a `logic_error` there and returns false (bug-for-bug parity,
  pinned by `lib/test/src/test_sighash_malleability.c`); implementing the
  sentinel would be the actual fork.
- **Data-integrity discipline:** application writes go through the ActiveRecord
  lifecycle or explicit storage-layer APIs; chain progress is represented as
  durable stage cursors; recovery and self-heal paths must be observable.
- **Live-data discipline:** consensus-adjacent fixes are proven on a datadir
  copy before deployment. The isolated node harness refuses live datadirs and
  live ports and runs on throwaway `/tmp/zcl23-*` state.
- **Release integrity:** `tools/release.sh` builds with deterministic release
  flags, writes `BUILDINFO`, emits a SHA3-256 attestation, and supports GPG.
  Its `--unsigned` output is explicitly local-development-only. Stable
  publication is contained until exact-candidate quality evidence,
  independently reproduced bytes, complete SBOM/provenance/manifests, and the
  required offline signature quorum are all enforced.
- **Dependency provenance:** the shipped binary is not libc-only — it
  statically links vendored, source-built, SHA256-pinned third-party static
  libraries (OpenSSL, libevent, LevelDB, secp256k1, librustzcash; see
  `tools/scripts/build_vendor.sh` and [`BUILD.md`](./BUILD.md)'s dependency
  table). The precise claim is **no unvendored, unpinned, or dynamically
  fetched-at-runtime dependencies** — every third-party source tarball is
  fetched from a pinned URL and verified against a pinned SHA256 before it is
  built and linked in; `libsecp256k1.a` (a custom fork build) ships
  committed. Vendored and ported third-party code is tracked in
  [`ATTRIBUTIONS.md`](./ATTRIBUTIONS.md), [`../NOTICE`](../NOTICE), and the
  repository tree. Packaging of several static libraries is still a known
  pre-v1 build gap and is documented in the README.

## Reviewer checklist

High-signal local checks:

```bash
git status --short --branch
make lint
make ci
```

Evidence files worth reading first:

- [`../README.md`](../README.md) - public status and feature scope.
- [`../.github/SECURITY.md`](../.github/SECURITY.md) - vulnerability reporting.
- [`MVP.md`](./MVP.md) - v1 acceptance criteria and readiness score.
- [`RUNBOOK.md`](./RUNBOOK.md) - operational safety rails.
- [`../tools/scripts/isolated_node_env.sh`](../tools/scripts/isolated_node_env.sh) - isolated process/datadir guardrails.
- [`../tools/release.sh`](../tools/release.sh) - reproducible release and signing logic.

## Reporting

Report vulnerabilities privately through GitHub security advisories as described
in [`../.github/SECURITY.md`](../.github/SECURITY.md). Do not bury a valid
finding in reassuring language. If a claim is not currently proven, document the
gap and the proof needed to close it.

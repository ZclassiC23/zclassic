# ZCODE — decentralized C23 source-package hosting (owner spec, 2026-07-27)

Working plan for the ZCODE program. The authority on existing foundations is
[`../P2P_SOURCE_HOSTING.md`](../P2P_SOURCE_HOSTING.md); this file carries the
owner's 15-slice build order, naming, and boundaries on top of it. Consensus
parity is untouched: ZCODE is an application protocol over ZClassic23.

Core description: publish, test, maintain and share permissive C23 code
through ZClassic23. Earn ZCODE, climb the rankings and collect permanent
contributor badges.

## Naming (use consistently)

- Service: ZCODE; package network: ZCODE Library; contributor token: ZCODE
- Contribution score: ZCODE Score; leaderboards: ZCODE Rankings
- Achievement NFTs: ZCODE Badges; P2P service credit: ZCODE Credit

## Identity chain

```text
ZNAM name → signed release record → immutable package root
          → verified manifest → verified SHA3 chunks
```

The package hash is authoritative. ZNAM is only a human-readable pointer;
changing a ZNAM record must never change the identity of an existing release.
secp256k1 publisher keys are the authoritative contributor identity. Release
ID = domain-separated SHA3-256 over a canonical binary encoding — never sign
JSON (JSON is display-only).

## Existing foundations (reuse — do not duplicate)

- `lib/vcs/package_manifest.*` — frozen content.v2 manifest + chunk verify (KAT in P2P_SOURCE_HOSTING.md)
- `lib/vcs/package_swarm.*` — pure ANNOUNCE/WANT/DATA/CANCEL wire codec
- `lib/vcs/vcs_object.*` — CAS put discipline (tmp/fsync/atomic rename, rehash on read)
- `lib/zslp/` codec + `app/models/zslp_ledger.*` — contributor token + badge assets
- `lib/znam/` + `app/models/znam.*` — ZNAM names (`ZNAM_TYPE_CONTENT` fits package roots)
- `app/models/principal.*` + `auth_challenge.*` — external pubkey identity
- `lib/net/peer_scoring.*` — offence taxonomy (INVALID_CHUNK=50 already exists)
- `app/controllers/src/name_site_controller.c` — web/native/model integration template
- Onion routes: prefix dispatch in `https_server.c` + `onion_service.c`; classify
  new routes in `lib/net/src/onion_ratelimit.c`

## The 15 slices (build in this order; each lands green as its own commit)

1. [ ] Signed ZCODE package release envelope (in flight: branch `work/zcode-slices`)
2. [ ] 10 GiB content-addressed package store (`-packagehost=0|1`, `-packagequota=10737418240`,
       disabled by default; 2 GiB pins / 4 GiB hot verified / 3 GiB rare / 1 GiB staging+quarantine;
       `<datadir>/zcode/{manifests,releases,attestations,badges,cas/sha3/aa/,staging/,pins/}`;
       verify-before-store, dedup, atomic rename, crash recovery, never evict pins,
       no credit for unverified bytes, quota enforced before accepting; 64 MiB per-package v1 cap)
3. [ ] Package publication and local search
4. [ ] Contributor identity + ZNAM pointers
5. [ ] Declarative C23 build recipe (bounded: public_headers/sources/test_sources/include_dirs/
       defines/allowed system libs = libc,libm,pthread/expected exit/max seconds/max bytes;
       node never compiles or executes downloaded code)
6. [ ] External verifier attestations (`zclassic23-package-verify <release-root>`: no network,
       no wallet, no datadir, read-only source, tmp build dir, CPU/RAM/proc/time limits,
       GCC+Clang, ASan+UBSan, delete binaries after attestation; ≥2 approved independent
       verifier keys sign matching attestations before any reward)
7. [ ] Contribution scoring (bounded deterministic; semantic-line component ≤500/release,
       tests > source; caps per release/contributor-week/releases-day)
8. [ ] Simulated ZCODE rewards (placeholder token ID only — never the real token in dev;
       settlement accrues into a daily queue — see ZCL fuel economics below)
9. [ ] Daily/weekly/monthly/all-time rankings (rank earned score, never token balance;
       store earned_score / token_rewards_received / current_token_balance separately)
10. [ ] Simulated ZCODE badges (ZSLP-based, permanent, no double-issue per period,
        owner-reviewed plan/commit issuance in v1)
11. [ ] Local P2P ratio + anti-spam policy (free allowance for new users; verified-bytes
        ratio is local credit; no global ZCODE mint for bandwidth; every rejection names
        the exact failed rule)
12. [ ] Authenticated package swarm (wire package_swarm codec to authenticated transport
        only after signatures + storage are complete; rarest-first, bounded windows,
        timeouts/retries/cancel/disconnect-requeue/resume, per-peer offence accounting)
13. [ ] Onion website (`/zcode*` routes, same models/projections as typed commands —
        no second package truth)
14. [ ] Owner-reviewed real ZCODE transfers (plan/commit; no automatic payout in v1;
        daily batched settlement — one ZSLP SEND per settlement window, not one per reward)
15. [ ] Owner-reviewed badge issuance

## ZCL fuel economics (owner directive, 2026-07-27)

ZCL is the fuel: every ZSLP mint/send (rewards, badges) and every ZNAM
record pays a ZCL transaction fee. Two deliverables:

1. **Daily batched settlement.** Rewards and badge transfers accrue as
   off-chain score facts and settle in ONE batched ZSLP SEND per settlement
   window (default daily), so N payouts cost ≈ one transaction instead of N.
   The settle plan must preview the exact ZCL fee before commit
   (`zcode reward plan` shows `estimated_fee_zcl`, recipient count, byte
   size); the owner confirms an irreversible spend, matching the burn-preview
   discipline in `docs/P2P_SOURCE_HOSTING.md`. Fee model: transparent ZCL tx
   with 1 OP_RETURN (ZSLP lokad payload) + N token outputs + change; size and
   fee grow ~linearly in N, so the per-recipient fee falls as the batch grows.
   Settlement stays reorg-aware and idempotent: a settled window records its
   txid; re-settling the same window is rejected by naming the rule.
2. **Cost-estimation surface.** A read-only estimator that reports the ZCL
   fuel cost of running these P2P open-source systems at declared scale —
   parameters: packages hosted, package bytes, swarm bandwidth, releases/day,
   reward recipients/day, badges/day, ZNAM records, verifier attestations,
   and LLM-token volume where an LLM-assisted workflow is part of the system.
   Output: per-action fee breakdown (bytes × fee rate), daily/monthly ZCL
   totals, and the batching savings vs naive per-action sends. Exposed as a
   typed command (`zcode cost estimate`, bounded typed JSON) and later an
   onion route, both reading the same projection. No chain writes — pure
   arithmetic over the current fee rate and payload sizes.

Tests for the economics slice(s): batch-size accounting matches the real
built transaction's vsize; fee preview within the wallet's own estimate;
idempotent re-settle rejection; reorg of the settlement tx returns the window
to the queue; estimator output is deterministic for fixed inputs and matches
the per-action formulas.

## Governance: none (owner correction 2026-07-27)

ZClassic is pure PoW consensus — **there is no voting, and ZCODE does not
add one.** An earlier draft of this section sketched token-weighted votes
(pin lists, quota changes), a `ZGOV` OP_RETURN vote service, a ZNAM
governance token, and GG20 threshold-signature ratification. All of that is
cut: no voting machinery, no governance tokens, no threshold-MPC slice.
ZCODE policy decisions (verifier-key set, scoring constants, quota defaults,
pin lists) are made by the owner through the same reviewed plan/commit flow
as rewards and badges. GG20-in-C23 remains a note here only as a possible
future building block if a concrete multisig need ever appears; nothing is
scheduled.

## Typed commands (one branch)

`zcode package publish plan|commit`, `search|show|fetch|pin|unpin|peers|verify`;
`zcode contributor show|packages|rewards|badges`;
`zcode leaderboard daily|weekly|monthly|all`;
`zcode reward score|eligible|queue|plan|commit|receipt`;
`zcode badge eligible|plan|issue`; `zcode seed status|ratio`; `zcode storage status`;
`zcode cost estimate` (read-only ZCL fuel-cost projection).
All replies bounded typed JSON; publishing/rewards/badges use plan/commit.

## Boundaries (absolute)

- No consensus change; no real ZCODE token during development; no automatic
  mint/badges/execution of downloaded code; no downloaded build scripts
  (Make/CMake/shell/Python/configure never run); no ranking by transferable
  balance; ZNAM never trusted over hashes; anonymous peer count is never a
  verifier quorum; no global ZCODE for bandwidth; never test against the live
  wallet or canonical datadir; extend canonical models instead of adding a
  second package database.
- License allowlist v1: 0BSD, MIT, Apache-2.0, BSD-2-Clause, BSD-3-Clause,
  ISC, Zlib. Unknown/missing/compound rejected. Package must include license text.
- Package structure: `include/ src/ tests/ examples/ LICENSE zcode-package.json`;
  reject absolute paths, traversal, symlinks, device files, sockets, hidden
  executable payloads, oversized manifests, unknown modes, duplicate paths.

## Process per slice

Smallest complete feature → focused adversarial tests → `make build-only` +
`make t-fast ONLY=<group>` + `make lint` green in an isolated worktree →
commit → merge to main → push → update this checklist.

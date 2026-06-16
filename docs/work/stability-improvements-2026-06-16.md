# Stability improvements — 2026-06-16

Source: post-recovery review (5-dimension workflow, every finding grep-verified at
HEAD `fdc1f3a30`). Prioritized, safety-classified hardening work.

**Safety classes:** `safe-now` = non-consensus, hermetically testable, no live
mutation. `copy-prove` = touches boot/recovery/coins; needs a datadir-copy proof
before deploy. `owner-gated` = consensus parity or a structural/topology decision.

## Applied this pass (safe-now)

- **O1 — deploy never ships a stale binary** (`Makefile` `deploy`): the
  `$(ZCLASSIC23_BIN)` rule is a single whole-program `cc` over `$(ALL_SRCS)` with
  **no depfile tracking**, so a header-only edit left every `.c` mtime unchanged
  and `make` skipped the relink. `deploy` now `rm`s the binary and forces a
  rebuild; `deploy_verify.sh` confirms the running `build_commit`.
- **S4 — DNS-seed resolution is loud** (`lib/net/src/connman.c`): a failed
  `getaddrinfo` used to `continue` silently; now `LOG_WARN`s with `gai_strerror`.
  (`lib/net/` is outside the `check-silent-errors` lint gate — a structural blind
  spot; the warn is the guard.)
- **C3 (log-only) — cold-import seed failure is unconditional** (`block_index_
  loader_rebuild.c`): the `applied_height` set-failure log was gated on `err`
  (silent when `coins_kv` returned false with no sqlite errmsg). Now always logs;
  logic unchanged, the SHA3/refuse behavior is C-queue work.

## SAFE-NOW queue (remaining — non-consensus, hermetic)

- **S1 — `header_band` in `zcl_state`**: expose island-root / contiguous-frontier
  / remaining / ETA so an operator can tell "frozen forever" from "backfilling N
  of M". `header_band_service.c` + `diagnostics_registry.c` + the two enum lists.
- **S2 — `connman` outbound-health in `zcl_state`** + surface `healthy_outbound`
  (not `num_nodes`) in health: "N/3 healthy, dialing, K addnodes in backoff".
- **S3 — split the overloaded `block-not-finalized-by-reducer` reason**: emit a
  benign `reducer-finalize-pending` for an in-flight block (stages advancing),
  reserve the hard reason for a genuine tear; add a derived `{advancing,
  idle-at-tip, stalled, torn}` health verdict.
- **S5/S6/S7 — untested recovery discriminators**: `peer_floor` witness branches;
  the zero-outbound forgive-all-backoff remedy; the `contradiction_frozen`
  condition loop through the engine.
- **S8 — IBD-class getheaders interval while a band hole exists** (10s, request
  side only; reverts when the band closes).
- **S9 — band-aware stall condition** keyed on the *contiguous frontier* (not
  `best_header`, which sits at the high island root and hides a pinned frontier).
- **S10 — split nTx disk-recovery counters** (hash-mismatch vs unreadable).
- **S11 — postmortem capsule on operator-needed escalation** (not just on a
  fatal signal).
- **S12 — seed-reachability CI canary** (the "28 tcp failures" root: the hardcoded
  seed/addnode list rotted to mostly-dead). Probe `vSeeds` + `vFixedSeeds` and fail
  under a floor; lint the deploy env IPs against the same list.

## COPY-PROVE queue (boot/recovery/coins — datadir-copy proof first)

- **C1 — local band-fill from the already-hardlinked zclassicd bodies** (HIGH):
  the ~12k-block band currently fills over P2P at ~160 headers/round-trip even
  though the bodies are on local disk (hardlinked at import). Scan local
  `blk*.dat`, `accept_block_header` to link pprev, close the band in-process.
  Turns a ~10-min peer-gated crawl into a seconds-long local scan.
- **C2 — first-boot freeze distinguishes band-hole from genuine contradiction**:
  enter a non-terminal `band_backfilling` state (self-heals on band close)
  instead of `contradiction_frozen`, so cold import never needs a manual restart.
  Sequence after C1.
- **C4 — SHA3 snapshot-refresh rc checked** (`utxo_recovery_restore.c`): the
  `BEGIN;END;` refresh that guards the false-wipe decision discards its rc.
- **C5 — replace `system("cp …")` block-file copy with a checked in-process copy**
  (`config/src/boot.c`): a failed copy currently proceeds with a missing/truncated
  `blk*.dat`; should refuse-to-bless + page.
- **C6 — band-backfill latency sub-budget in the replay canary**.
- **C7 — end-to-end cold-import recovery test/sim** (detect→freeze→backfill→close,
  asserts un-freeze with no operator restart); wire into `make ci`.

## OWNER-GATED / strategic

- **O2 — make self-sufficient trustless sync (FlyClient + SHA3 snapshot + delta
  P2P over the same reducer stages) the flagship; demote cold-import to opt-in.**
  Both Layer-0 failure classes (coin tear, restart fragility) are symptoms of
  cold import being a privileged write path that bypasses the validated reducer.
  Tactical bridge (copy-prove-able): a write-time import-correctness gate that
  refuses imported state until SHA3-verified AND completeness-checked at every
  height.
- **O3 — chain-aware peer-floor "healthy" definition**: count a peer toward the
  anti-eclipse floor only if it recently served a header/block at/above our tip.
  Do NOT lower the floor.
- **O4 — INJECT event vocabulary for the seed-tape simulator** (PEER_DROP /
  HEADER_ADMIT / IMPORT_TIP) so cold-import/peer-floor failures become replayable
  64-bit seeds + chaos commands.

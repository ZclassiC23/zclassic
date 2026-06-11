# Tenacity Roadmap — sync super fast, fail almost never

**Date:** 2026-06-11 · **Status:** active · **Builds on:** [`canonical-frontier-derived-state-plan.md`](./canonical-frontier-derived-state-plan.md) (referenced below as "the canonical plan" — not duplicated here)

## The lens (anchor every item to it)

1. **State duplication = guaranteed divergence.** Redundant encodings of the same fact
   without one authoritative source + derivation discipline WILL diverge under partial
   writes; repair code that guesses between copies converts crashes into wedges. Cure is
   architectural: one source (the log), derived views, divergence unrepresentable.
2. **Verification must sample the real distribution.** The chain is the only ground truth
   for consensus (reference source text is a lossy proxy); live failure modes (boot
   restore, recovery, crash, import) need gates that exercise them, or green tests measure
   the wrong distribution. `test_parallel 0/409` stayed green through a 6-hour outage.
3. **Sync speed has an information floor:** headers + UTXO snapshot (trusted) or FlyClient
   sampling (trustless). We already measure at the floor (`--importblockindex` 3.14M
   headers in 60–74 s, then `-cold-import` → hash-identical tip in ~25 min). The remaining
   work is making the floor the *only* path and making "fast" never mean "silently
   incomplete".

**Standing rule (write into doctrine, enforce on every PR):** a new wedge gets a new
*write-time invariant at a chokepoint*, never a new repair module.

---

## Ordered items

### 1. Land Invariant A — frontier-bounded tip *(IN FLIGHT — wt2, branch `fix/invariant-a-restore-clamp`)*

- **Goal:** make Wedge A (restore installs a tip above the validated header frontier →
  post-restore integrity FATAL → crash-loop) *unwritable*. This is the boot crash-loop
  class that took the node down 6+ hours on 2026-06-11.
- **Mechanism:** canonical plan steps 3–4 — frontier gate in `csr_validate_locked`
  (tip committable only ≤ contiguous ok=1 prefix of `validate_headers_log`, modulo typed
  `rollback_auth`), `pindex_best_header` becomes a projection (never slammed), restore
  tip-selection collapses to derive-from-frontier, evidence-based floor rewind replaces the
  anti-rewind refusal that turned a consistent rollback target (3137373) into a wedge.
- **Acceptance proof:** (a) COPY of the frozen fixture
  `~/.zclassic-c23-postrestore-wedge-20260611` (tip 3143175 > frontier 3141533, 1267
  holes) boots degraded-but-live at the frontier with **no FATAL**, and the reducer
  advances forward; (b) clean-datadir warm boot shows tip == frontier (no over-reject);
  (c) build + lint + test_parallel green. Deploy stays owner-gated on copy parity.
- **Size:** in flight; ~+300/−200 LOC at the chokepoint per the canonical plan.

### 2. Land the oversize grandfather fix + re-prove full genesis replay *(IN FLIGHT — branch `fix/consensus-oversize-grandfather`)*

- **Goal:** the canonical chain must replay from genesis without a single consensus
  reject. Today FR-3's text-copied `MAX_TX_SIZE_AFTER_SAPLING=102000` false-rejects the
  real 125,811-byte tx at h=478544 (mined when the rule was 2 MB; Sapling active 476969;
  zclassicd tightened later without grandfathering — the reference cannot resync its own
  chain). This FATALs auto-reindex and stalls the forward reducer.
- **Mechanism:** scan the REAL chain, pin every scanned violation as a grandfathered
  exception, enforce 102000 strictly above the scan horizon. This is chain-conformance,
  not parity divergence — the accept/reject set vs the *chain* is what parity means.
- **Acceptance proof:** full genesis→anchor replay (`-reindex-chainstate` on a datadir
  copy) passes h=478544 with zero rejects and zero internal_errors, and the final UTXO
  commitment == zclassicd `gettxoutsetinfo` at the same height. That replay run IS the
  first execution of canary #5.
- **Size:** in flight; small (~150 LOC + scan tool), unblocks items 5 and 9.

### 3. Reindex epilogue derivation — recovery must not manufacture the next wedge

- **Goal:** `-reindex-chainstate` (now the crash-only auto-recovery path, `706a7c00a`)
  currently ends in a torn state: with the never-give-up unit (`0b45e93a5`) this can
  degrade into an infinite reindex loop. Close it.
- **Mechanism:** code-read confirmed gaps in `config/src/boot_index.c:160–297` and
  `config/src/boot.c:3321–3344`:
  - `boot_index_clear_coins_state` **deletes** `coins_best_block` / `utxo_commitment` /
    `utxo_sha3` but the epilogue never **recomputes** them;
  - `coins_kv` (progress.kv) is never reseeded from the replayed set;
  - the `utxo_apply` / `coins_applied` cursors keep stale pre-reindex values — the
    recovery path itself manufactures the `coins_applied > hstar` wedge shape.
  Fix = the epilogue derives, in one ordered commit discipline: reseed coins_kv from the
  replayed set, recompute (not delete) the SHA3 commitment, clamp the reducer cursors to
  the replayed tip. Same lens as everything else: derived, never installed-stale.
- **Acceptance proof:** (a) regtest reindex smoke (seed ~50 blocks, `-reindex-chainstate`,
  assert tip + row-count parity + recomputed==served commitment + SERVING) — gates every
  push, ~3 min; (b) mainnet copy reindex ends with cursors == replayed tip and no
  `coins_applied > hstar`; (c) kill -9 mid-reindex then reboot converges (feeds item 7).
- **Size:** ~250 LOC + smoke harness; 1 worker session.

### 4. Repair-ladder deletion (post-Invariant-A) — the net-subtractive payoff

- **Goal:** delete the guess-between-copies repair the invariants obsolete — ~4,500
  gross production LOC, ≈ −2,900 net across the canonical plan once its gates land
  (canonical plan step 7: `chain_restore_integrity`, the chain_restore rebuild
  ladder, `stage_repair_reducer_frontier_{tipfin,refill,purge}` + tear branch,
  `reducer_frontier_reconcile_light`, `utxo_recovery_torn_anchor`, coins_view_sqlite
  rewind/reconcile) plus their pinned tests. Every deletion retires its reconciliation
  path with it, or it isn't done.
- **Mechanism:** ordering constraint is hard (audit-verified): zero-callers is the state
  *after* canonical-plan steps 1–6 — `utxo_recovery_heal_torn_legacy_coins_anchor` is
  still live-called from boot, `chain_restore_integrity` has 3 non-test callers today.
  Items 1–3 above land first; then grep-prove zero callers; then delete.
  `boot_auto_reindex` (`706a7c00a`) stays as the crash-only backstop — the one rung that
  re-derives instead of guessing.
- **Acceptance proof:** grep shows zero callers per module before each delete; build +
  lint + test_parallel green after; canaries #5/#7 green for 7 consecutive days with the
  ladder gone (the honest proof that nothing was load-bearing).
- **Size:** ≈ −4,500 gross production LOC (≈ −2,900 net across the plan), ≈ −3,600
  test LOC; 1–2 worker sessions, mechanical.

### 5. Standing full-history replay canary (the single highest-leverage gate)

- **Goal:** a recurring institution that samples the REAL distribution: would have caught
  3 of this session's 4 live failures. The gates ARE the product — this lands before/with
  the deletions in item 4 because it is the only honest verification of them.
- **Mechanism:** nightly cron (systemd user timer; the binary's own tools, no python):
  HEAD binary, scratch datadir, bootstrap to anchor 3,056,758, replay all bodies through
  the reducer; assert zero consensus rejects, zero internal_errors, final commitment ==
  zclassicd `gettxoutsetinfo`. Weekly variant replays from genesis (~6 h). Verdict
  sentinel file + `EV_OPERATOR_NEEDED` on failure — never exit-0-as-proof. Also merge the
  already-verified mvp-spawn workflow (`c086c5136`) so *something* gates push.
- **Acceptance proof:** the canary itself failing red on a seeded known-bad binary (prove
  the gate fires), then 7 consecutive green nights; wired into `docs/MVP.md` CI status.
- **Size:** ~200 LOC shell/C harness + 2 timer units; 1 session. ~45 min/night + ~6 h/wk
  on the 32-core box.

### 6. Chain-derived golden extremals for every bounded consensus predicate

- **Goal:** permanently close the text-vs-chain class. FR-3 was one of 12 recorded
  text-derived rule findings resting on the same history-safe-by-text argument
  (FR-1–FR-5 + the miner cap + the 6 confirmed in
  [`consensus-parity-supplemental-audit-2026-06-08.md`](./consensus-parity-supplemental-audit-2026-06-08.md),
  whose summary explicitly invokes "exactly the FR-3 argument" — the argument the
  h=478544 tx refuted). "History-safe" must be a machine invariant, not a header comment
  (`consensus.h:23–29` was prose, never checked, and was wrong).
- **Mechanism:** one-time ~20-min C scan of the real chain producing a pinned per-era
  extremals table: max tx size, max block size, max sigops, version/locktime/expiry
  ranges, per activation era. New test group: every bounded consensus predicate must
  ACCEPT every pinned real extremal. Amend `docs/CONSENSUS_PARITY_DOCTRINE.md` with the
  precedence rule — **chain > deployed zclassicd behavior > zclassicd source text** — and
  require any bounded-predicate change to cite the table.
- **Acceptance proof:** the test group red-fails when `MAX_TX_SIZE_AFTER_SAPLING` is set
  to 102000 *without* the grandfather (reproducing FR-3), green with it; doctrine amended.
- **Size:** ~300 LOC scan tool + table + test group; 1 session. Scan cost is one-time.

### 7. Crash-boot soak gate — sample the off-diagonal torn-state space

- **Goal:** hand-enumerated unit tears cannot cover real multi-store crash states; the
  restore/recovery path must be exercised the way it fails. 0 of this session's 4
  failures were catchable by any gate as configured — this and #5 fix the measurement
  channel.
- **Mechanism:** nightly (~40 min, ~10 cycles): reflink-copy the frozen wedge fixture +
  a fresh live snapshot; boot HEAD; `kill -9` at randomized phases (boot, mid-advance,
  mid-reindex); reboot; assert SERVING, tip ≥ floor, no FATAL, no restart loop, within
  time budget. This is also the standing watch on the never-give-up unit: an infinite
  reindex loop is a red soak, not a mystery.
- **Acceptance proof:** soak red on the pre-`706a7c00a` binary against the wedge fixture
  (gate fires on the known-bad past), then 7 consecutive green nights at HEAD.
- **Size:** ~250 LOC harness + timer unit; 1 session.

### 8. One-command tenacious bootstrap — make the proven recipe the only path

- **Goal:** kill the cold-sync footgun. `-cold-import` alone leaves a 3.1M-header hole
  (headers=960) and pins forever; the proven recipe is `--importblockindex` FIRST
  (3.14M headers, 60–74 s) then `-cold-import` boot → hash-identical tip in ~25 min
  (multi-height verified vs zclassicd, warm-reboot-proven). A recipe that lives only in
  docs will rot (the docs already had it wrong once, plus the wrong zclassicd P2P port —
  it is 8033).
- **Mechanism:** wrap the two steps in the binary. Preferred shape: `-cold-import` boot
  **auto-detects** the header hole (header count ≪ source index extent) and runs the
  block-index import itself before applying the snapshot — auto-compose, don't document
  around. Optionally alias the whole thing as `-tenacious-sync=<sourcedir>`. Post-import
  completeness invariant always-on: contiguity genesis..tip, zero nBits==0, per-keyspace
  counts vs source LevelDB (the 1,561-record silent-tail-drop precedent is the reason
  row-count plausibility is banned) — seconds of cost, refuses to mark the sync complete
  otherwise.
- **Acceptance proof:** single command from an EMPTY datadir → SERVING at a
  hash-identical tip vs zclassicd in ~25 min; weekly cold-sync canary (~30 min) executes
  exactly this command forever, keeping the recipe honest by running it; the old
  partial-recipe path either composes or refuses loudly (no silent pin).
- **Size:** ~200 LOC boot wiring + invariant + canary unit; 1 session.

### 9. Remove the temporary `-nobgvalidation` once item 2 lands

- **Goal:** restore full background re-verification on the live node. The flag is a
  TEMPORARY mitigation injected 2026-06-11 via `~/.config/zclassic23/env`
  (word-split into `ZCL_ADDNODE_FLAGS`) because `bg_validation_service.c:291`
  `check_block` would false-flag canonical block 478544 → a `BLOCK_FAILED_VALID`
  tip-wedge vector (one stale failed-bit wedges the tip).
- **Mechanism:** after item 2 deploys, delete the flag from the env file, restart the
  service, let bg validation walk past h=478544.
- **Acceptance proof:** live `zcl_validationstatus` shows progress monotonically past
  478544 with zero false flags; no `BLOCK_FAILED_VALID` entries appear; RSS stays within
  the known bg-validation envelope (watch the stair-step gotcha).
- **Size:** config-only + one live observation window. Owner-visible (live deploy).

---

## Sequencing at a glance

```
NOW (parallel, in flight):  1 (Invariant A, wt2)   2 (grandfather + replay proof)
THEN:                       3 (reindex epilogue)   5 (replay canary — lands WITH 3, gates 4)
THEN:                       4 (delete the ladder)  6 (extremals)  7 (crash soak)
THEN:                       8 (one-command bootstrap + weekly canary)
LAST (live, owner-gated):   9 (drop -nobgvalidation)
```

Everything here is either an invariant at a chokepoint, a deletion, or a gate that
samples reality. Nothing adds a repair module. Net production LOC across the roadmap is
**negative** (~−4,500 of gross ladder deletion against ~+1,300–1,600 of
invariants/harnesses ≈ −2,900 net, matching the canonical plan's −2,800..−3,100), and
the standing compute cost is ~1–1.5 h/night + ~30 min/week on hardware we already own.

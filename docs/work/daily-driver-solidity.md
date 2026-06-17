# Daily-driver solidity — scorecard & path

Owner bar (2026-06-17): zclassic23 must be solid enough to be the **canonical
daily-driver node** BEFORE services are built on top. "Solid" = survives the
three things you do to a daily driver — **reboot it, kill it, walk away for a
week** — while **continuously proving it still equals zclassicd**. Audited by
the `daily-driver-solidity` workflow (6-dimension audit + de-risked plans,
verified against live state: node healthy at tip 3151016 == zclassicd).

## Scorecard

| Dimension | Solid? | Blocks daily-driver? | Pri |
|---|:---:|:---:|:---:|
| Consensus parity (bit-for-bit vs zclassicd) | partial | yes | **P0** |
| Restart-durability (reboot without dropping the chain) | **no** | yes | **P0** |
| Cold-import correctness (no silent coin tears) | **no** | yes | **P0** |
| Liveness / no-silent-wedge | partial | no | P1 |
| Kill-9 recovery + crash-safety | **yes** | no | P1 |
| Soak / unattended-for-weeks (168h) | **no** | yes | P1 |

**Steady-state at tip is proven (live, now). Every remaining gap is a
cold-import / restart correctness gap — the node holds tip fine; the reboot and
the cold-import are what bite.** None require new consensus logic; the hard part
is *proving*, not coding. Distance ≈ 3 copy-proven fixes + one 168h soak clock.

## P0 fix sequence (each gated by its own copy-prove; live node untouched until proven)

Rule for all: clone to `/tmp`, isolated 39xxx ports, dead-sink `-connect`; never
start/stop/mutate the live main node, dev lane, soak node, or zclassicd.

### P0-A — Restart-durability copy-prove (the reboot blocker) — #1 ACTION
- Build a **genuine pre-fix baseline** from `d643a836b^` (= `4976d22a3`) — every
  existing binary already contains OPTION-1, so `strings | grep -c
  zcd_import_tip_suppressed` must be **0** on the baseline or the test is toothless.
- Clone `~/.zclassic-c23-brokenrestart-20260617`; boot pre-fix → must reproduce
  the genesis-drop (teeth). Boot HEAD → must (a) emit `zcd_import_tip_suppressed`,
  (b) do **no** backward CSR commit, (c) hold the derived frontier (never genesis),
  (d) re-save the enriched flat.
- **CRITICAL FINDING (verify first):** the captured fixture's real failure looks
  like *stale-flat-rejection → detached-root → `contradiction_frozen`*, which
  **OPTION-1 does NOT cover.** If HEAD still wedges on the clone, the true reboot
  fix is **forward-extent connectivity preservation** (raise-only projection
  topup so coins-best is never a detached root) — i.e. the restart fix is NOT done.
- Negative (two-sided): a detached-island/torn clone must **still refuse**
  (`restore_tip_refused` / `seed.torn_import`); a below-zcd clone must **still
  promote** normally.
- Replace the *replica* regression test (`test_coldimport_restart_fragility.c`)
  with one that calls the real `boot.c` predicate.
- Live reboot stays owner-gated + snapshot-first even after green.

### P0-B — Cold-import write-time-honest gate (the recurring wedge class)
- Honest framing: a true per-outpoint write-time completeness check is infeasible
  (bodies stripped at import). Deliverable = **forward-evidence bless-time refusal
  everywhere + checkpoint-reject** at the compiled height (3,056,758).
- Genuine new work: route the **P2P snapshot/FlyClient seed path**
  (`snapshot_apply.c`) through the *same* torn-import predicate the LDB path
  already uses — today it can silently bless a torn snapshot.
- Copy-prove **fires** on the frozen torn fixtures (`cointear-20260612`,
  `livetear-20260613`) → `seed.torn_import` raised, H* pinned at checkpoint, no
  anchor stamp (first confirm each fixture carries the durable `ok=0` precondition).
- Copy-prove **no-op** on a faithful import (`make replay-canary-anchor`) → zero
  false-reject (the safety-critical leg; real-chain validated — h=478544 lesson).
- Residual: an *unspent-forever* torn coin is caught only by an external/checkpoint
  reference (owner-gated higher-checkpoint ceremony; deferred).

### P0-C — Make consensus parity EXACT, not coarse (closes the C8 set-divergence hole)
- Live confirms `reference_exact:false`: the standing UTXO check asserts *height
  only* → structurally **cannot** page a UTXO-set tear (the class that wedged us).
- Promote the parity reference to **byte-exact** (zclassic23-format SHA3 over the
  canonical UTXO set, generated cross-node on the C++ reference daemon), wire
  `utxo_audit_compare_remote` to byte-compare. Copy-prove it **latches** on an
  injected-drift fixture and matches at a reorg-safe height with `exact:true`.
- Stand up the **replay canary** as an enforced recurring proof (systemd user
  timer) → fresh PASS sentinel on HEAD. Today the only sentinel is a stale Jun-13
  FAIL — there is **no current positive proof** HEAD replays genesis→tip cleanly.

## P1 (after the P0s land and the binary is deployable)
- **C6 soak:** recover the soak node with the current binary + clean re-import,
  OR repoint the collector at the healthy main node (`ZCL_SOAK_RPC_CMD` /
  `ZCL_SOAK_UNIT` seams proven) — then bank a clean judge-graded **168h
  VERDICT=MET** window. Add a **judge timer** so a live-but-wedged gap-growing
  node *pages* (today a wedged node returns `ok:true` and never trips OnFailure).
- **Liveness:** close the total-process-hang SPOF — supervisor and
  `zcl_health_sweep` mutually watch each other, and/or an off-node replay-canary
  deadman (the daily-driver unit is `Type=simple`, no `WatchdogSec`, so a full
  deadlock currently pages nothing).
- **C7:** promote the full-binary crash harnesses into a `make ci-crash` stage +
  a mainnet-scale kill-9-restart copy-prove on a `~/.zclassic-c23` copy.

## The single highest-leverage next action
**Build the pre-fix baseline binary from `4976d22a3` (= `d643a836b^`), then run the
restart-durability copy-prove on a clone of `~/.zclassic-c23-brokenrestart-20260617`.**
A daily driver must reboot safely and we have NEVER proven this node does — this one
run either proves OPTION-1 closes it or reveals the real fix is forward-extent
connectivity preservation. Either way it unblocks "safe to reboot," the gate to
everything else. Parity floor untouched; no consensus change anywhere in this plan.

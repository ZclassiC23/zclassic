# zclassic23 — MVP target

**MVP = "someone we don't know can run zclassic23 + use it for a week
without intervention."** Eight binary acceptance criteria, each with
a verification target. The MVP Readiness Score (MRS) is the count of passing
criteria; MVP is achieved at 8/8.

## Acceptance criteria

| # | Criterion | How we verify | Status |
|---|---|---|---|
| 1 | **Single-binary install on clean Ubuntu/Debian** | Target: clean container, `make install && systemctl --user start zclassic23`, exit 0 — **hermetic proxy: `make ci-install`** (not in the `make ci` target): builds + installs the node + `zcl-rpc` to a throwaway `/tmp` prefix (the file-copy a real `make install` performs), then starts ONE node FROM that prefix as a fully isolated regtest node (unique `/tmp` datadir + 39xxx non-live ports + `-connect=39999` dead sink, via the audited `tools/scripts/isolated_node_env.sh`), polls RPC readiness, asserts the installed binary answers + bound only non-live ports, and cleans up. The literal `make install` target now EXISTS (2026-06-06) — `install -m 755` the two binaries to `$(DESTDIR)$(PREFIX)/bin` + install the systemd `--user` unit (path-rewritten to the installed binary; skipped under `DESTDIR` for hermetic staging), and `make ci-install` now drives that REAL target (`make install DESTDIR=<throwaway> PREFIX=`) instead of a hand-rolled copy. Still ◐: no real `systemctl --user start` on a clean container is the remaining gap to ✅. | ◐ |
| 2 | **Tor onion bootstrap in <60s** | `zcl_onion_status` returns `bootstrap_state=ready` within 60s of start — test: `lib/test/src/test_onion_bootstrap.c` (`make ci-stress`; needs Tor egress). **Verified PASSING 2026-06-06**: on a host with Tor egress the embedded Tor reached a ready `.onion` in **53s** (`OK (53s, …onion)`, 0 failures, under the 60s budget). Stays ☐ in default CI: it needs real Tor network egress (an isolated runner without it would burn the 90s poll then fail), so it lives in `make ci-stress`, not `make ci`. | ☐ |
| 3 | **Cold-start sync to tip in <10 min** | Fresh datadir → `zcl_syncstate.phase=ready` within 10 min on 100 Mbps link — test: `lib/test/src/test_cold_start_sync.c` (**CI slice: `make ci-mvp-gates`** — drives the sync FSM to `at_tip` in ~7s; does **not** download/validate a real 3M-block chain) | ◐ |
| 4 | **Receive shielded payment end-to-end** | Test wallet receives 1 ZCL to a z-addr, balance reflects within 2 blocks — test: `lib/test/src/test_shielded_payment_gate.c` (`make ci-stress`; needs `~/.zcash-params`). **Runs GREEN e2e now (2026-06-06, `d1c0395e0`)**: with the params present the gate builds a real Sapling proof, the t→z tx enters mempool (value_balance −1.25 ZCL), and the wallet decrypts the note back to 1.25000000 ZCL. Was a gate double-registration FATAL before, never executed. Still ◐/☐ in CI: it lives in `make ci-stress` (the ~770 MB `~/.zcash-params` fixture isn't in the default `make ci` runner), so it's not yet hermetic-gated. | ◐ |
| 5 | **List + sell file via store** | Operator lists product → buyer pays shielded → buyer receives file — test: `lib/test/src/test_store_e2e_gate.c` (**CI slice: `make ci-mvp-gates`** — in-process store + seeded note + balance check; **not** a real shielded purchase + file transfer) | ◐ |
| 6 | **7-day soak with zero operator intervention** | Live node + synthetic load for 168h: no manual restarts, RSS plateau — **hermetic proxy: `make soak-ci`** (not in the `make ci` target) — ~180s bounded soak on an isolated `/tmp` regtest node under synthetic `generate`-load, scored by the unchanged `soak_harness.c` verdict math (RSS-plateau + tip-HWM + zero-crash). The real 168h live soak (`make soak-7day`) remains operational and gated on the live wedge. | ◐ |
| 7 | **Recover from `kill -9` in <2 min** | kill -9 mid-block, restart, caught up to peer-tip within 2 min — in-process unit `lib/test/src/test_kill9_recovery.c` (**CI slice: `make ci-mvp-gates`** — proves `node.db` atomic UTXO recovery after SIGKILL) **plus** the full-binary harness `make test-crash-bootstrap` (not in the `make ci` target) which spawns a real isolated regtest node and proves clean **boot recovery** under a real process-group `SIGKILL`. Still ◐: regtest `generate` now WORKS end-to-end (SOLVED 2026-06-06 `bcd44e68e` — datadir mismatch), so `make test-crash-bootstrap` now SEEDS 30 blocks. But the full-binary overshoot teeth still report `over=-1`: the reducer-mined blocks are NOT durable across a restart (even a CLEAN shutdown) — `getblockcount` goes 0→5 live but restart returns 0. Root cause: the reducer/`generate` path authors the `utxo_projection`, while boot recovery + `shutdown_flush_coins_to_sqlite` (boot_services.c:1549) read/flush the legacy `coins_tip`/`coins.db` anchor, which the reducer path never advances for mined blocks. Durability keystone Option A LANDED 2026-06-06 (`801832692`, 3-lens consensus panel GO_WITH_FIXES): reducer-mined blocks now survive a CLEAN restart (`generate 5`→clean shutdown→restart→`getblockcount=5`) by persisting the small block_index on shutdown (`>1` not `>1000`); the forward-only finalized-tip seed recovers the rest. Mainnet byte-identical. KILL-9 RECOVERY LANDED 2026-06-06 (`4e7fc176f`, 3-lens consensus panel GO_WITH_FIXES): under a real SIGKILL with NO clean shutdown the node now FULLY recovers — `make test-crash-bootstrap` PASSES (seed 30 → kill-9 → recover 30 → mine 31 → kill-9 → recover → 32, `height_regress: 0`). Boot rebuilds the block_index map from the durable per-block `block_index_projection` (a shared helper that also collapsed the `-rebuildfromlog` path, netting boot.c smaller); the pure-map-rebuild lets the existing GUARDED forward seed adopt the finalized tip (no unguarded publish). Mainnet unaffected (the fallback only fires when the legacy loaders return an empty map). RESTART-TO-PEER-TIP LANDED 2026-06-06 (`f135abb5f`): the full-binary two-node harness `make test-two-node-peer-tip` spawns miner A + follower B on disjoint ports/datadirs and proves the LITERAL claim — A mines 10 → B syncs over native P2P → kill-9 B, A mines +5, restart B → **B recovers and catches up to peer-tip 15**. Required fixing the P2P body-SERVE path (`msgprocessor.c`) to open bodies from the net-specific datadir (symmetric twin of the write-path fix; mainnet byte-identical). REMAINING for ✅: it spawns real nodes so it stays outside the hermetic `make ci` target; promotion to ✅ needs the FULL operator claim (real systemd start for #1, real 168h soak for #6), not merely proxy wiring. | ◐ |
| 8 | **Consensus parity with zclassicd** | Continuous diff service: 0 mismatches over the 7-day soak window — service **exists** (`app/services/src/utxo_parity_service.c`, wired at boot via `boot_utxo_parity.c`), ships **dormant**. **CI slice: `make ci-mvp-gates`** (`make mvp-parity-slice`, `lib/test/src/test_parity_slice.c`) drives the parity service against the in-process FIXTURE reference and asserts its `mismatches` stat with a paired control: a CONSISTENT reducer-vs-fixture set reports **0 mismatches** (status MATCH), and a REAL injected extra outpoint is **DETECTED** (status DRIFT, `mismatches>0`, drift flag persisted) — the negative control that proves the diff machinery has teeth. Still ◐: the slice protects the mismatch-detection MACHINERY via a hermetic fixture; the FULL claim still needs the live `zclassicd` oracle (RPC 8232) over the 7-day soak window. | ◐ |

**Legend:** ☐ unmet / not gated · ◐ a hermetic CI gate runs green for a
**slice or proxy** of the criterion (real, automated regression protection —
but not the full operator-acceptance claim) · ✅ the full criterion is verified
end-to-end in CI. The MRS counts only ✅.

**Full criteria met: ~2 / 8 (manual, NOT CI-enforced). CI-verified full
criteria (✅): 0 / 8.** (The "~2/8 manual" is a by-hand demonstration of #1/#7
only — it is *not* readiness and is not gated; the load-bearing number is
✅ = 0/8.)
What the `ci-mvp-gates` wiring added: **five hermetic gates** (◐ / supporting)
now run-and-pass (not SKIP) under `make ci` and block the build — **#3**
cold-start sync FSM (~7s), **#5** store end-to-end proxy (sub-second), **#7**
kill-9 SQLite-atomicity recovery (~4-8s), a supporting `chain_advance_atomicity`
fork test, and the **"it works" gate** `reducer_ingest`
(`test_reducer_block_ingest_gate.c`, `make mvp-it-works`) — mines one real
regtest Equihash (48,5) block and drives it through the **reducer front door**
(`reducer_ingest_block`, the same entry live intake uses) on production stage
defaults, asserting the authoritative tip advances by exactly 1 with a
consistent UTXO commitment. It is teeth-verified (fails if the reducer cannot
finalize forward — the live-wedge failure mode) but is supporting
infrastructure, not a numbered criterion, so the MRS is unchanged. Each gate
runs FOCUSED via `ZCL_TEST_ONLY` under `ZCL_STRESS_TESTS=1`
and is guarded against false-green fall-through (a vanished selector fails the
gate loudly instead of silently running the full suite). That is real
regression protection for a *slice* of each criterion — but none proves the
full operator claim (a real 3M-block sync, a real shielded purchase, a
full-binary restart to peer-tip), so they are ◐, not ✅, and the MRS is
unchanged.

Additional full-scope gaps remain:
- **#2 onion** and **#4 shielded** full gates are NOT hermetic — #2 needs real Tor
  network egress (would burn its 90s ceiling then fail on an isolated runner)
  and #4 needs the ~770 MB `~/.zcash-params` fixture (silently SKIPs without
  it, a false-green risk). They live in `make ci-stress`, to run on a worker
  that has those resources. NOT in `make ci`.
- **#1** (single-binary install) now has the hermetic `make ci-install` proxy
  gate (not in the `make ci` target) — install-to-throwaway-`/tmp`-prefix +
  isolated regtest start + RPC-ready proof; it does not run a real `systemctl
  --user start`, so it is ◐ not ✅.
- **#6** (7-day soak) has a bounded proxy (`make soak-ci`), but no test in the
  default `make ci` target and no real
  168h soak. **#8** (consensus parity) has the hermetic mismatch-detection slice
  in `make ci` (`parity_slice`) but no live-`zclassicd` oracle gate. Both also
  need live forward progress, blocked on the live wedge.

Manual status: #1 and #7 are also demonstrated by hand; #2's onion is live but
its <60s timing isn't measured; #6 is **regressing** (no soak — node holds at
tip without finalizing).

**Update rule:** ◐ → ✅ ONLY when a criterion's acceptance test exercises the
**full** operator claim end-to-end and run-and-passes in the **hermetic**
`make ci` job (not a slice, not opt-in, not SKIP). A slice test earns ◐; a test
that SKIPs in CI earns neither. To promote #2/#4, provision their fixtures on
the `mvp-stress` runner; #1 and #8 need net-new CI jobs; #3/#5/#7 need
full-scope tests to replace their slice-gates.

**THE plan to drive MRS to 8/8 is [`docs/work/FORWARD_PLAN.md`](./work/FORWARD_PLAN.md).**

**MVP achieved when:** MRS = 8/8.

## Why these and not others

- **Operator UX over feature breadth.** ZNAM, ZMSG, Market, Swaps,
  P2P games are great features, but they don't define MVP. MVP is
  "the chain works, payments work, the operator can leave it
  alone." Differentiated features come after MVP.
- **Decentralized commerce as the headline value.** The store flow
  (criterion 5) is the wedge that makes zclassic23 different from
  any other Zcash node — pay via shielded, receive via .onion.
- **Soak time as the hardest gate.** Criterion 6 (7-day uninterrupted
  operation) is what proves we're past firefighting. Today: ~3h
  between operator restarts. MVP requires a 56× improvement.

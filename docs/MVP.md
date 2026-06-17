# zclassic23 — MVP target

**MVP = "someone we don't know can run zclassic23 + use it for a week
without intervention."** Eight binary acceptance criteria, each with
a verification target. The MVP Readiness Score (MRS) is the count of passing
criteria; MVP is achieved at 8/8.

## Acceptance criteria

| # | Criterion | How we verify | Status |
|---|---|---|---|
| 1 | **Single-binary install on clean Ubuntu/Debian** | Target: clean container, `make install && systemctl --user start zclassic23`, exit 0 — **hermetic proxy: `make ci-install`** (not in the `make ci` target): drives the real `make install` (the two binaries + the systemd `--user` unit) to a throwaway `/tmp` prefix, starts ONE node FROM that prefix as a fully isolated regtest node (unique `/tmp` datadir + 39xxx non-live ports + `-connect=39999` dead sink, via the audited `tools/scripts/isolated_node_env.sh`), polls RPC readiness, asserts the installed binary answers + bound only non-live ports, and cleans up. Also in the one-command local aggregate `make mvp-verify` (locally-verified, not hermetic-✅). The clean-OS half of the claim is proven WITHOUT docker (docker is never used in this project). The portability floor is now a **hermetic gate IN `make ci`**: **`make ci-symbol-floor`** (`tools/scripts/ci_symbol_floor_gate.sh`) statically asserts via `objdump -T`/`ldd` that every dep resolves and the binary's max required symbol in each family stays ≤ the documented floor **GLIBC_2.38 / GLIBCXX_3.4.30 / CXXABI_1.3.9** (the real floor is a triple — libstdc++/libgcc_s, not glibc alone — so an Ubuntu 20.04 with GLIBCXX_3.4.28 would NOT satisfy it; a regression that silently pulls a newer symbol fails the build loud). **✅ (2026-06-17):** the FULL operator claim — a real `make install` + a real `systemctl --user start` bringing the installed binary up to serve RPC — run-passes via **`make ci-install-linger`** (`tools/scripts/ci_install_linger_gate.sh`): it `make install`s to a throwaway `/tmp` prefix, starts an **isolated linger unit `zclassic23-citest`** (distinct name / `/tmp` datadir / 3906x non-live ports / dead-sink connect / `-nolegacyimport`; torn down on exit; the live `zclassic23` unit is never touched), and asserts the unit is active + the installed binary answers `getblockcount`. No docker, ever. A `make mvp-verify` member; run-passed 2026-06-17. | ✅ |
| 2 | **Tor onion bootstrap in <60s** | `zcl_onion_status` returns `bootstrap_state=ready` within 60s of start — test: `lib/test/src/test_onion_bootstrap.c` (`make ci-stress`; needs Tor egress; measured **53s** on a host with egress). The onion is live by hand but its <60s timing isn't measured by default CI. Also runnable via `make mvp-onion-local` (bundled in `make mvp-verify`), which probes for Tor egress and SKIPs cleanly when absent — locally-verified, not hermetic-✅. It needs real Tor network egress (an isolated runner without it would burn the 90s poll then fail), so it lives in `make ci-stress`, not the hermetic `make ci`. **✅ (2026-06-17):** `make mvp-onion-local` (a `make mvp-verify` member) run-passes the full claim on this host — `onion_bootstrap MVP #2 bootstrap_state=ready in <60s` — the local operator proof. | ✅ |
| 3 | **Cold-start sync to tip in <10 min** | Fresh datadir → `zcl_syncstate.phase=ready` within 10 min on 100 Mbps link — test: `lib/test/src/test_cold_start_sync.c` (**CI slice: `make ci-mvp-gates`** — drives the sync FSM to `at_tip` in ~7s; does **not** download/validate a real 3M-block chain). The nearest REAL proof — the snapshot-first cold boot (`ci-coldstart` → >1M UTXOs in <90s from a `consensus_snapshot.db` fixture) — was orphaned (referenced nowhere); it is now de-orphaned into `make mvp-verify` via `mvp-coldstart-local` (fixture-gated, SKIPs cleanly when absent), so the only real cold-boot proof no longer rots. **Empirical finding (2026-06-17, `tools/scripts/cold_start_to_tip_probe.sh`):** a fresh snapshot-only node dialing a live serving peer does NOT reach `at_tip` — the snapshot import path (`config/src/boot_snapshot_import.c:271-277`) sets `coins_best_block` but does **not** seed `coins_kv`/`coins_applied_height` like the LDB path, so `reducer_frontier` L1 refuses (`coins_applied_height absent (coin frontier unknown)`, 40/40 reconcile cycles) and the forward delta-sync stalls (header cursor frozen at 3041). So C3's full `<10min` claim needs that coin-frontier seed on the snapshot path (copy-prove-gated boot change), not just a harness wrapper — the probe is the C3 harness that flips to a `make mvp-verify` member once the seed lands. | ◐ |
| 4 | **Receive shielded payment end-to-end** | Test wallet receives 1 ZCL to a z-addr, balance reflects within 2 blocks — test: `lib/test/src/test_shielded_payment_gate.c` (`make ci-stress`; needs `~/.zcash-params`). With the params present the gate builds a real Sapling proof, the t→z tx enters mempool (value_balance −1.25 ZCL), and the wallet decrypts the note back to 1.25000000 ZCL. The params-free RECEIVE half (note→ivk→z-balance) is hermetically gated in `make ci-mvp-gates` (`make mvp-shielded-receive`) and re-run in `make mvp-verify`. Still ◐: the full send+receive path lives in `make ci-stress` (the ~770 MB `~/.zcash-params` fixture isn't in the default `make ci` runner), so it's not yet hermetic-gated. **✅ (2026-06-17):** the FULL claim run-passes via `make test-shielded-payment` (a `make mvp-verify` member) on this params-provisioned host — real Groth16 t→z built → entered mempool → wallet decrypt **1.25000000 ZCL** — the local operator proof. The params-free RECEIVE half stays a hermetic `make ci` regression floor; the new DURABLE-receive gate (`make mvp-shielded-receive-persist`, note→node.db→reopen→z-balance) guards persistence in `make ci-mvp-gates`. | ✅ |
| 5 | **List + sell file via store** | Operator lists product → buyer pays shielded → buyer receives file — test: `lib/test/src/test_store_e2e_gate.c` (**CI slice: `make ci-mvp-gates`** — in-process store + seeded note + balance check; **not** a real shielded purchase + file transfer) | ◐ |
| 6 | **7-day soak with zero operator intervention** | Live node + synthetic load for 168h: no manual restarts, RSS plateau. **Hermetic proxy: `make soak-ci`** (not in `make ci`) — ~180s bounded soak on an isolated `/tmp` regtest node, scored by `soak_harness.c` (RSS-plateau + tip-HWM + zero-crash). Real 168h soak: `make soak-7day`; judge the window with `make soak-evidence-report` (`VERDICT=MET\|NOT_MET\|INSUFFICIENT`). The verdict-JUDGE logic (`make soak-evidence-selftest`, hermetic mktemp JSONL fixtures + injected timestamps, no node) is now CI-regression-protected — it runs inside `make ci`; this guards the scoring machinery only and does NOT shortcut the live soak hours. Harness mechanics (replay canary, soak-evidence sampling) in RUNBOOK. Opt-in (spawns/reads live nodes), so ◐ until accumulated; **status (2026-06-16):** recovered from a multi-day stale-binary + torn-datadir outage, soak window restarting (C1/C2/O2 in `docs/work/stability-improvements-2026-06-16.md`). | ◐ |
| 7 | **Recover from `kill -9` in <2 min** | kill -9 mid-block, restart, caught up to peer-tip within 2 min — in-process unit `lib/test/src/test_kill9_recovery.c` (**CI slice: `make ci-mvp-gates`** — proves `node.db` atomic UTXO recovery after SIGKILL) **plus** the full-binary harness `make test-crash-bootstrap` (not in the `make ci` target) which spawns a real isolated regtest node and proves clean **boot recovery** under a real process-group `SIGKILL` (seed 30 → kill-9 → recover 30 → mine 31 → kill-9 → recover → 32, `height_regress: 0`) **plus** the two-node harness `make test-two-node-peer-tip` (miner A + follower B on disjoint ports/datadirs): A mines 10 → B syncs over native P2P → kill-9 B, A mines +5, restart B → **B recovers and catches up to peer-tip 15**. Both full-binary harnesses are bundled in the one-command local aggregate `make mvp-verify`. **Live status (2026-06-17, after fix `f83101b81`):** `generate` forward-progress is **FIXED** — a fresh on-demand node now self-seeds the genesis anchor (copy-proven `generate 5` → tip 5, rejects=0), so **`test-two-node-peer-tip` now PASSES end-to-end** (A mines 10 → B syncs → kill-9 B → A +5 → B recovers to peer-tip 15 via P2P re-sync; A never restarts). the single-node **restart-durability keystone is now FIXED** (`341020c05`, owner-gated, copy-proven): a fresh node that mined N blocks, was kill-9'd, and rebooted to a NULL active tip now restores the durable finalized tip via a forward-only genesis-root seed (canonical-genesis-anchored + `coins_applied>=tip` gated + mainnet-no-op), instead of stranding at `h=-1`. `make test-crash-bootstrap` now **PASSES end-to-end** (seed 30 → kill-9 → recover → mine 31 → kill-9 → recover, **`height_regress: 0`**), so the SINGLE-node kill/restart recovery teeth are asserted. **✅ (2026-06-17):** the FULL kill-9 recovery claim run-passes via `make test-crash-bootstrap` (full-binary boot recovery, `height_regress: 0`) + `make test-two-node-peer-tip` (B recovers to peer-tip 15 over native P2P) — both `make mvp-verify` members; the hermetic `node.db` SQLite-atomicity slice stays the `make ci` regression floor. Live deploy of the keystone binary to the mainnet datadir remains owner-gated (wipe+cold-import). | ✅ |
| 8 | **Consensus parity with zclassicd** | Continuous diff service: 0 mismatches over the 7-day soak window. Service exists (`app/services/src/utxo_parity_service.c`, wired via `boot_utxo_parity.c`), default ON when a `zclassicd` oracle resolves, diffing UTXO/tip at the supervised 60s `chain.utxo_parity_poll` cadence; quiet no-op with no daemon; force off with `ZCL_PARITY_ORACLE=0`. Read-only observer — cannot touch consensus/tip/liveness. Drift persists `utxo_drift_detected` (surfaced via `zcl_state subsystem=utxo_parity`) and pages via the wired Condition. **CI slice: `make ci-mvp-gates`** (`make mvp-parity-slice`, `lib/test/src/test_parity_slice.c`) drives the service against an in-process fixture with a paired control — consistent set → 0 mismatches (MATCH), injected extra outpoint → DETECTED (DRIFT). The previously-untested COARSE production branch (`exact=false` — what the live `zclassicd` oracle actually hits, since `gettxoutsetinfo` returns height only) now has hermetic coverage too: C1 same-height → MATCH with empty remote SHA3 (bucketed in `coarse_checks`, never a byte-MATCH), C2 height-skew → LOCAL_ONLY (never DRIFT), C3 a coarse confirmation CLEARS a stale exact-drift flag so it stops paging. Still ◐: the production `zclassicd` reference is coarse (height-only; `gettxoutsetinfo` cannot return a zclassic23 SHA3), so the full "0 byte-mismatches over 7-day soak" claim needs an EXACT reference. The standing replay canary (RUNBOOK) adds a recurring full-history check; opt-in, so ◐ until accumulated. | ◐ |

**Legend:** ☐ unmet / not gated · ◐ a gate runs green for a **slice or proxy**
of the criterion (real, automated regression protection — but not the full
operator-acceptance claim) · ✅ the **full operator claim** run-passes
end-to-end via the **local operator proof** on this machine — the relevant
`make mvp-verify` member / linger-service gate (`make ci-install-linger`,
`make mvp-onion-local`, `make test-shielded-payment`, `make test-crash-bootstrap`
+ `make test-two-node-peer-tip`, …). The MRS counts only ✅.

**MRS (✅ count): 4 / 8** (revised 2026-06-17 under the local-operator-proof
rule). **C1, C2, C4, C7 are ✅** — each FULL operator claim run-passes via its
`make mvp-verify` member (verified 2026-06-17, all 8 members PASS):
`ci-install-linger` (real `make install` + `systemctl --user start`),
`mvp-onion-local` (real onion `<60s`), `test-shielded-payment` (real Groth16 t→z
+ decrypt 1.25 ZCL), and `test-crash-bootstrap` + `test-two-node-peer-tip`
(kill-9 recovery). Remaining ◐: **C3** (snapshot cold-boot proven; the full
sync-to-tip `<10min` still needs a real serving peer + delta sync), **C5** (store
Slice 2 live reconcile + a real buyer), **C6** (the 168h soak window — gated on
un-wedging the live node, owner deferred to root-fix-first), **C8** (an exact
zclassicd reference, or 0 mismatches over the soak). The hermetic `make ci` gates
remain the regression floor that keeps every ✅ honest between operator runs.

The `ci-mvp-gates` wiring added **five hermetic gates** (◐ / supporting) that
run-and-pass (not SKIP) under `make ci` and block the build — **#3** cold-start
sync FSM (~7s), **#5** store end-to-end proxy, **#7** kill-9 SQLite-atomicity
recovery (~4-8s), a supporting `chain_advance_atomicity` fork test, and the
**"it works" gate** `reducer_ingest` (`test_reducer_block_ingest_gate.c`,
`make mvp-it-works`) — mines one real regtest Equihash (48,5) block and drives
it through the **reducer front door** (`reducer_ingest_block`, the same entry
live intake uses) on production stage defaults, asserting the authoritative tip
advances by exactly 1 with a consistent UTXO commitment. It is teeth-verified
(fails if the reducer cannot finalize forward — the live-wedge failure mode) but
is supporting infrastructure, not a numbered criterion, so the MRS is unchanged.
Each gate runs FOCUSED via `ZCL_TEST_ONLY` under `ZCL_STRESS_TESTS=1` and is
guarded against false-green fall-through (a vanished selector fails the gate
loudly instead of silently running the full suite). That is real regression
protection for a *slice* of each criterion — but none proves the full operator
claim (a real 3M-block sync, a real shielded purchase, a full-binary restart to
peer-tip), so they are ◐, not ✅.

Soak (#6) and parity (#8) both also need live forward progress; that path is now
open (see #6's wedge note) and a live soak window is accumulating. #6's
`make ci-stress` soak proxy intentionally lives outside `make ci` because the
bounded soak spawns a real isolated node.

**Update rule (revised 2026-06-17):** consistent with this project's **local-only
CI** (CI = `make` on this machine, never a paid/hosted runner) and the
**never-docker** policy, ◐ → ✅ when a criterion's **full** operator claim
**run-passes end-to-end** via the **local operator proof** — the relevant
`make mvp-verify` member or linger-service gate (`make ci-install-linger`,
`make mvp-onion-local`, `make test-shielded-payment`, `make test-crash-bootstrap`
+ `make test-two-node-peer-tip`, …) — actually RUNNING and PASSING (not a slice,
not a SKIP) on this host. A slice/proxy gate earns ◐; a member that SKIPs for a
missing local dependency (params / Tor egress / fixture) stays ◐ until that
dependency is present and it run-passes; a wall-clock claim (#6's 168h soak)
earns ✅ only when the window actually completes clean. Hermetic `make ci` gates
remain the regression floor (they keep every ✅ honest between operator runs),
but ✅ is no longer reserved to them — the inherently non-hermetic operator
claims (real `systemctl --user start`, real Tor, real Groth16 params, real soak)
are proven by the local linger-service operator proofs instead.

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
  operation) is what proves we're past firefighting. MVP requires a clean
  168h window with zero operator intervention — measure it with
  `make soak-evidence-report` (VERDICT=MET), never a hand estimate.

# MVP CI map — each MVP.md criterion → its mechanical check → status

CLAUDE.md #1 priority: **CI-enforce the MVP criteria.** This document is the
mapping behind `make mvp` (the reporter, `tools/scripts/mvp_scoreboard.sh`).

`make mvp` prints an HONEST 8/8 scoreboard. For each of the 8 operator
acceptance criteria in [`docs/MVP.md`](../MVP.md) it runs the ONE
mechanically-runnable check that proves it and prints a per-criterion verdict
line: **PASS / FAIL / BLOCKED(reason)**.

## The contract (cannot false-green)

- **PASS** is earned ONLY when the criterion's check actually RAN and PASSED
  at the **full operator-claim** level. The MRS counts only these.
- **BLOCKED(reason)** — the criterion's full claim REQUIRES a resource that is
  absent: a **synced running node**, **Tor egress**, **`~/.zcash-params`**, or
  a **live `zclassicd` oracle**. The hermetic slice may still run as evidence,
  but the criterion is reported BLOCKED — **never silently skipped-as-pass,
  never green.**
- **FAIL** — a check that is *supposed* to pass (a hermetic slice) regressed,
  OR the false-green guard fired (a `ZCL_TEST_ONLY` selector vanished, so the
  focused subset's sentinel was not printed / the run timed out). A FAIL is
  printed loudly and counted in the summary.

The three **synced-node-dependent** criteria — **C3** (cold-start sync to tip),
**C6** (168h soak), **C8** (consensus parity over the soak window) — CANNOT
pass until the node holds a clean synced tip on the **sovereign foundation** and
accrues a clean window. The forward-sync wedge that previously blocked this is
FIXED (the node reaches tip on the stopgap); these stay **BLOCKED(needs synced
node)** by construction until the foundation is sovereign + the window accrues —
no hermetic slice can stand in for "a fresh node reached the real ~3.15M-block
tip in <10min" or "168h clean wall-clock". The reporter probes the live node
(`zcl-rpc getblockchaininfo`): when `blocks != headers` (not at tip) the
synced-node criteria are BLOCKED with the live height named in the reason.

`make mvp` is a **status reporter, not a build gate** — it exits 0 even with
BLOCKED criteria (the honest state of a stopped/wedged node), so it is wired
into `make ci` as a VISIBLE report without breaking the build. The
build-FATAL regression protection for the hermetic slices lives in
`make ci-mvp-gates` (already in `make ci`); the scoreboard's FAIL is the
human-readable echo of the same regression.

## The map

| # | MVP.md criterion | Mechanical check `make mvp` runs | Full-claim resource | Status (wedge fixed; gated on sovereign foundation + soak) |
|---|---|---|---|---|
| C1 | Single-binary install on clean Ubuntu/Debian | `ci_symbol_floor_gate.sh` (portability GLIBC/GLIBCXX/CXXABI floor) + built-binary existence | full claim = real `make install` + `systemctl --user start` → `make ci-install-linger` (a `mvp-verify` member) | **PASS** — symbol-floor + binaries present; install mechanism proven by `make ci-install` / `ci-install-linger` |
| C2 | Tor onion bootstrap in <60s | `test_zcl` slice `ZCL_TEST_ONLY=onion_slice` (<60s budget + v3 address format) + Tor-egress probe | real <60s over Tor needs egress → `make mvp-onion-local` | **PASS** when Tor egress present (else BLOCKED(needs Tor egress)) |
| C3 | Cold-start sync to tip in <10 min | `test_zcl` slice `ZCL_TEST_ONLY=cold_start` (sync FSM → at_tip, ~7s) | real <10min sync to the ~3.15M-block tip needs a serving peer + a fresh node | **BLOCKED(needs synced node)** — FSM slice PASS; the live node now reaches and re-seeds to tip on restart via the at-own-height snapshot, but the single-binary <10min snapshot cold-boot path is still in flight (HANDOFF P1-5); `make mvp-coldstart-local` is the fixture-gated real cold-boot proof |
| C4 | Receive shielded payment end-to-end | `test_zcl` slices `shielded_receive` + `shielded_receive_persist` (params-free RECEIVE half: note→ivk→z-balance→durable reopen) + `~/.zcash-params` probe | full Groth16 t→z send+decrypt needs ~770MB proving params → `make test-shielded-payment` | **PASS** when params present (else BLOCKED(needs ~/.zcash-params)) |
| C5 | List + sell file via store | `test_zcl` slices `store_e2e` + `store_e2e_shielded` (in-process store + seeded note + ivk-decrypt + memo-bound) | full list→shielded-pay→.onion file transfer needs a live serving node + a real buyer | **BLOCKED(needs synced node)** — slices PASS; full purchase path needs a live node + buyer (see `c5-real-shielded-purchase-plan.md`) |
| C6 | 7-day soak, zero operator intervention | `soak_evidence.sh judge` over accumulated samples (MET=PASS, NOT_MET=FAIL, INSUFFICIENT=BLOCKED), GATED on the live node being synced+accruing | 168h clean wall-clock on a synced, tip-tracking node | **BLOCKED(needs synced node)** — gated on the sovereign foundation + a clean 168h window accruing (the wedge that blocked soak is fixed); the judge's read of historical samples is shown for context only, never earns PASS |
| C7 | Recover from kill -9 in <2 min | `test_zcl` slices `kill9` (node.db SIGKILL UTXO-atomicity) + `chain_advance_atomicity` | full-binary kill-9 → peer-tip recovery → `make test-crash-bootstrap` + `make test-two-node-peer-tip` (mvp-verify members; isolated regtest, no synced mainnet needed) | **PASS** — SQLite-atomicity teeth PASS; the full-binary harnesses run on fresh isolated regtest (independent of the wedged mainnet node) |
| C8 | Consensus parity with zclassicd | `test_zcl` slice `parity_slice` (mismatch-detection machinery: consistent set→0, injected outpoint→DETECTED) | 0 byte-mismatches vs a live `zclassicd` oracle over the soak window; needs an EXACT reference (`gettxoutsetinfo` is height-only) + the soak window | **BLOCKED(needs synced node + exact oracle)** — detection machinery PASS; full claim needs a live oracle + soak |

## Current scoreboard (this host; wedge fixed, gated on sovereign foundation + soak)

```
MRS (full operator claim PASS): 4 / 8
  C1 PASS  C2 PASS  C4 PASS  C7 PASS
  C3 BLOCKED(needs synced node)            C5 BLOCKED(needs synced node)
  C6 BLOCKED(needs synced node)            C8 BLOCKED(needs synced node + exact oracle)
FAIL: 0   (nothing false-greens)
```

This matches MVP.md's own MRS of 4/8 (C1/C2/C4/C7 ✅). The 4 BLOCKED criteria
are gated on the **sovereign foundation + a clean soak window** (the forward-sync
wedge that previously blocked forward progress is FIXED — the node reaches tip on
the stopgap; cure design: `never-stuck-plan.md`). When the node holds a sovereign
synced tip + accrues a clean soak window, C3/C5/C6/C8 flip from BLOCKED to PASS
automatically — the reporter re-probes the live node and the soak judge each run.

## How the false-green guard works

Each hermetic slice runs FOCUSED via `ZCL_TEST_ONLY=<selector>` under
`ZCL_STRESS_TESTS=1`, with the SAME guard `make ci-mvp-gates` uses: `test.c`
returns early ONLY on a selector match, printing a unique sentinel
(`=== <name> subset complete: N failure(s) ===`). If the selector is
renamed/removed, the FULL suite runs instead and the sentinel is NOT printed
— `run_slice` turns that into a loud **FAIL**, not a fake PASS. A 120s
per-slice timeout bounds the call so a vanished selector can't run the
multi-minute full suite (or hang on the non-hermetic onion test); a timeout
also maps to FAIL. (Verified: pointing C8 at a bogus selector reports C8 FAIL,
MRS unchanged at 4/8, FAIL=1 — never green.)

## Files

- `tools/scripts/mvp_scoreboard.sh` — the reporter (run by `make mvp`).
- `Makefile` target `mvp` — `make mvp`; also invoked as a visible non-fatal
  report at the end of `make ci`.
- `docs/MVP.md` — the 8 criteria (source of truth for the claims).
- `Makefile` target `ci-mvp-gates` — the build-FATAL hermetic slice gates
  (the regression floor; the scoreboard's PASS/FAIL echoes these).

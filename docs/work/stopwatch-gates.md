# Stopwatch gates — C3 and PROOF B wall-clock evidence

Two opt-in, periodic wall-clock proofs, each split the same way as
`tools/scripts/soak_evidence.sh`: a **collect** half that runs a real
stopwatch and durably records the outcome, and a **judge** half that reads
the ledger and prints a gate-able verdict. Neither runs inside `make ci` —
both need a live binary/peer/fixture, not hermetic fixtures.

## The two gates

| gate | proves | harness script | collect wrapper | ledger |
|------|--------|-----------------|------------------|--------|
| C3 | a genuinely-wiped fresh node reaches network tip within budget (`docs/MVP.md` criterion 3) | `tools/scripts/cold_start_to_tip_stopwatch.sh` | `tools/scripts/c3_stopwatch_run_and_record.sh` | `~/.local/state/zclassic23-c3-stopwatch/history.jsonl` |
| PROOF B | an already-at-tip node recovers from an upstream network outage within budget | `tools/scripts/network_disruption_recovery_stopwatch.sh` | `tools/scripts/netdisrupt_stopwatch_run_and_record.sh` | `~/.local/state/zclassic23-netdisrupt-stopwatch/history.jsonl` |

Both harnesses gate on the same real claim `cold_start_to_tip_stopwatch.sh`
already uses for C3: `dumpstate reducer_frontier`'s `hstar` (the reducer's
authoritative, provable tip) reaching `network_tip` (the best height any
handshake-complete peer advertised) — never "the sync FSM says at_tip".
Both share the same five-way exit-code contract: `0` PASS, `1` FAIL, `2`
SKIP (a fixture was absent — not a verdict either way), `3` SEAM (real
forward/recovery progress but the budget expired), `4` STALLED-NAMED (no
progress, but an active named blocker off `dumpstate blocker` explains
why).

PROOF B additionally needs an already-running, already-at-tip client
(`--client-rpc=` / `--client-datadir=`) and a controllable upstream peer
process (`--upstream-pid-file=` or a bare `ZCL_ND_UPSTREAM_PID`). It never
spawns either node — it SIGSTOPs the upstream pid to simulate a clean
network partition (not a crash), sleeps `--cut-secs` (default 600s), then
SIGCONTs it and times how long the client's `hstar` takes to re-catch
`network_tip`. The upstream is **always** SIGCONT'd on exit, including on
a hard failure or Ctrl-C (an `EXIT`/`INT`/`TERM` trap) — the harness must
never leave a peer parked STOPped.

## Failure legibility

On any non-PASS verdict, both harnesses additionally capture into the same
`RUN_ID` artifact dir: `frontier.json` (raw `dumpstate reducer_frontier`),
`blocker.json` (raw `dumpstate blocker`), and `ops.log.tail.txt` (the typed
`ops logs` command against the live node if still RPC-reachable, else a
plain tail of `node.log`). A `bundle_capture_failed` field in `proof.json`
records when any piece could not be captured — never a silent drop.

## Collect half — never gates

`c3_stopwatch_run_and_record.sh` / `netdisrupt_stopwatch_run_and_record.sh`
run their harness once and append **one** JSON line
(`ts, verdict, exit_code, wall_clock_seconds, budget_seconds, peer,
node_bin, build_commit, artifact_dir` — PROOF B adds `cut_seconds`) to
their ledger, `flock`-serialized the same way `soak_evidence.sh`
serializes its append. Same discipline as the soak collector: the wrapper
exits `0` once the append succeeds, **regardless** of the run's own
PASS/FAIL/SKIP/SEAM/STALLED-NAMED verdict — that verdict is recorded, not
paged. The only thing that makes a collect run itself fail is being unable
to lock or append the ledger line.

## Judge half — `stopwatch_evidence_judge.sh`

```bash
tools/scripts/stopwatch_evidence_judge.sh <history.jsonl> [--max-age-secs N]
```

Reads only the **last** line (a stopwatch run is a point-in-time proof,
not an accrual claim like the 168h soak window, so there is no window to
cover — only freshness). Prints one line:

```
stopwatch-judge: VERDICT=PASS|FAIL|STALE reason=... artifact=<dir>
```

and exits `0`/`1`/`2` respectively. `PASS` requires the last run's verdict
to be exactly `pass` **and** fresh (age <= `--max-age-secs`, default
86400s = 24h). Any other recorded verdict (including `skip`) reads as
`FAIL`, never a silent pass. `STALE` — the timer-died case — fires when
the ledger is missing/empty/malformed, or the last sample is older than
`--max-age-secs`: a green run from last week must not keep reporting PASS
forever once the collector stops running.

## Running the reports

```bash
make c3-stopwatch-report           # judges ~/.local/state/zclassic23-c3-stopwatch/history.jsonl
make netdisrupt-stopwatch-report   # judges ~/.local/state/zclassic23-netdisrupt-stopwatch/history.jsonl
```

Both are false-green-guarded the same way `soak-evidence-report` is: if
the judge does not print a `VERDICT=` line at all (a crashed/no-op judge),
the recipe fails loud regardless of the judge's own exit code.

## Running the harnesses directly (manual / one-off)

```bash
make mvp-coldstart-to-tip-stopwatch          # C3, ZCL_BIN=/ZCL_PEER= override the target
make mvp-netdisrupt-recovery-stopwatch       # PROOF B, ZCL_ND_* override the client/upstream/timing
```

Both propagate exit codes `1`/`3`/`4` as a failing `make` recipe; a `2`
(SKIP) maps to a clean `exit 0` — a missing fixture is not itself a
verdict on the underlying claim.

## Periodic timers

`deploy/examples/zcl-stopwatch-peer.service` is a dedicated minimal
serving peer (ports 39070-39073) the C3 harness dials, kept separate from
the canonical `zclassic23.service` (port 8033) so these gates can never
contend with or churn the live node. `zcl-c3-stopwatch-run@.service` /
`.timer` and `zcl-netdisrupt-run@.service` / `.timer` run the two
collectors every 6h (offset 30 minutes apart); both `OnFailure=` into
`zcl-stopwatch-onfailure.service`, which fires only on a lock/append
failure, never on a legitimate non-PASS run verdict. See
`deploy/examples/README.md` for the full unit descriptions.

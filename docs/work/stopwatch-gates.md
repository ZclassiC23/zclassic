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
Both share the same seven-way exit-code contract: `0` PASS, `1` FAIL, `2`
SKIP (a fixture was absent — not a verdict either way), `3` SEAM (real
forward/recovery progress but the budget expired), `4` STALLED-NAMED (no
progress, but an active named blocker off `dumpstate blocker` explains
why), `5` FRONTIER-BUSY-TIMEOUT (`dumpstate reducer_frontier` never yielded
a usable sample inside the busy-timeout window — an instrument failure, not
a claim about the node), and `6` READBACK-FAILED (no observed progress and
no named blocker, but the final frontier readback could not be taken;
"we could not observe" is not "we observed nothing happening"). The
collectors map all seven, plus `error` for anything else.

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
node_bin, build_commit, artifact_dir, skip_reason, skip_class, skip_streak,
no_pass_streak` — PROOF B adds `cut_seconds`; C3 adds the fixture-shape and
`peer_precheck` fields) to
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

plus one `SKIP_STREAK` report line (see the skip taxonomy above),
and exits `0`/`1`/`2` respectively. `PASS` requires the last run's verdict
to be exactly `pass` **and** fresh (age <= `--max-age-secs`, default
86400s = 24h). Any other recorded verdict (including `skip`) reads as
`FAIL`, never a silent pass. `STALE` — the timer-died case — fires when
the ledger is missing/empty/malformed, or the last sample is older than
`--max-age-secs`: a green run from last week must not keep reporting PASS
forever once the collector stops running.

## Skip taxonomy and the skip-streak alarm

A harness exits `2` (SKIP) for two completely different reasons: *"nothing
was configured, so there was nothing to prove"* and *"the fixture I need has
been dead for days"*. Both used to land in the ledger as the identical string
`"verdict":"skip"`, and nothing looked at them. That is how the C3 gate
recorded a skip on **every** scheduled run from 2026-07-28 06:02 onward with
no operator-visible signal: the judge grades a skip as `FAIL`, but nothing
ran the judge, and `tools/scripts/arch_score.sh` scores KPI1 off `tail -n 5`
of the ledger, so one surviving old pass held the architecture score at 85
for four consecutive skips (~30h at the 6h cadence).

### The class table

`app/services/include/services/stopwatch_skip_classes.def` is the one table.
Three consumers read it and none keeps a private copy:
`app/services/src/stopwatch_skip_watch.c` (the typed surface),
`tools/scripts/stopwatch_skip_class.sh` (sourced by the collector and the
judge). Add a `skip()` site to a harness, add its row there.

| class | threshold | why that threshold | example reasons |
|-------|-----------|--------------------|-----------------|
| `not_configured` | **0 — never alarms** | genuinely benign; the optional input was simply not supplied. Only PROOF B can reach it: the C3 collector always defaults `ZCL_PEER`, so *every* C3 skip is a defect in something. | `no valid --client-rpc …`, `no valid upstream PID …` |
| `config_error` | **1** | never self-heals — the next run produces the identical skip forever, so waiting a cycle buys no information | `node binary absent/not executable: …`, `invalid peer address: …`, `bundle fixture absent: …` |
| `fixture_absent` | **2** | can self-heal (a peer restarts), so one sample is a bouncing fixture, not evidence; still absent one full 6h interval later, it is gone | `serving peer not reachable: …`, `client RPC not reachable …`, `… is not a live process` |
| `harness_misuse` | **1** | structural, not reason-matched: the argv loops `exit 2` on an unknown flag *without* calling `skip()`, so no artifact and no reason are written. `rc=2` with no `artifact_dir` **is** the signature. Never self-heals. | an operator typo in a flag |
| `unclassified` | **2** | a reason this table does not know, or a ledger row written before `skip_reason` existed. Loud-ish on purpose — an unknown skip is a gap in the table, and the honest answer is to say so, not to assume it was benign. | anything new |

Why `2` and not some other number for the self-healing class: it is the
smallest streak that survives a full timer interval, and it fires **three
intervals (~18h) before the architecture score can move** — the alarm's whole
job is to precede the number changing. It also sits strictly inside the
judge's own 24h staleness window, so the two rungs never race to describe the
same hole: the streak means *"the collector ran and could not prove
anything"*, `STALE` means *"the collector stopped running"*. Do **not**
inherit a threshold from another prober — `node_slo_probe.sh` uses 10 because
it polls on a minutes cadence; 10 here would be 60 hours of silence.

There is deliberately no env knob to raise a threshold. An override would be
a supported way to silence this, which is the defect.

### Where the alarm shows up

- **Collect time** — `c3_stopwatch_run_and_record.sh` prints one `ALARM` line
  to **stderr** and to `logger -t stopwatch-gate` (the same syslog tag
  `zcl-stopwatch-onfailure.service` uses, so one grep finds both classes).
  The unit has `StandardError=journal`, so it lands in
  `journalctl --user -u zcl-c3-stopwatch-run@default.service` every 6h.
  Below threshold it prints a `WARN` instead; a benign class prints a `note`
  and never alarms.
- **Report time** — `make c3-stopwatch-report` /
  `make netdisrupt-stopwatch-report` print one extra `SKIP_STREAK` line on
  stdout and, on a crossing, one `ALARM` line on **stderr**.
- **Typed interface** — `zclassic23 dumpstate stopwatch_evidence` (key `c3`
  or `netdisrupt`, or empty for both) reports `skip_streak`,
  `no_pass_streak`, `skip_class`, `alarm_threshold`, `alarm`, and a plain
  English `summary`, re-read from the ledger file on every call.

### It reports; it never grades

Six mechanisms keep the alarm off the score, listed strongest first:

1. `tools/scripts/arch_score.sh` gets a **zero-byte diff**.
2. The alarm is on **stderr**, and every scoring path discards stderr at the
   source (`arch_score.sh` runs
   `make -s c3-stopwatch-report 2>/dev/null | grep -q VERDICT=PASS`; the
   Makefile recipe captures stdout only). A fact that cannot physically reach
   the grader cannot flatter it.
3. The `ALARM` line contains **no `VERDICT=` token**, so even a consumer that
   merged the streams cannot read it as a verdict, and the recipes'
   FALSE-GREEN guard is unaffected.
4. The judge's `VERDICT` token and exit code come from the unchanged code
   path. The 12 pre-existing `--selftest` cases are the regression proof, and
   the new cases assert `(token, rc)` is byte-identical with and without an
   `ALARM`.
5. It is one-directional by construction: it can only *add* a line. There is
   no input under which it removes a `FAIL`, upgrades a verdict, or extends a
   window.
6. Streaks are **recomputed** from the ledger's `verdict` values by every
   consumer that acts on them, never read from the `skip_streak` field the
   collector records — a forged field cannot silence the detector either.

The collector records `skip_reason`, `peer_precheck`, `skip_class`,
`skip_streak` and `no_pass_streak` as additive ledger fields. `skip_reason`
in particular is the one that had been missing: the harness always wrote it
into its `proof.json`, but the ledger line did not carry it, so no
ledger-only consumer could tell a week-dead fixture from a typo.

### Regression guard

`make lint` runs `check-stopwatch-skip-detector`
(`tools/lint/check_stopwatch_skip_detector.sh`), which runs both shell
selftests with a false-green guard and cross-checks that the shell parser
sees exactly the class rows in the `.def`. The C half is the
`stopwatch_skip_watch` test group.

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

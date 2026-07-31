# EXAMPLE_LANE_PROMPT — the same lane, prompted two ways

    VERSION: v1

This is the artifact that shows whether
[`LANE_CONTRACT.md`](LANE_CONTRACT.md) pays for itself. Both prompts below launch
the **same lane** (the one that produced these files). The first is the doctrine
preamble the orchestrator hand-wrote into the prompt. The second references the
contract instead.

## Measured

Measured with `wc -l -w -c` over the exact text, not estimated.

| | lines | words | characters |
|---|---:|---:|---:|
| **Before** — hand-written doctrine + build etiquette + house facts | 55 | 488 | 3,302 |
| **After** — reference to the contract | 6 | 56 | 426 |
| **Saved per lane prompt** | 49 (−89%) | 432 (−89%) | 2,876 (−87%) |

For scale: the same workflow also pasted a 60-line / 3,359-character
friction-and-objective block into all three of its lanes verbatim, so the fully
duplicated frame was **115 lines / 6,661 characters per lane**, hand-maintained
in the workflow script.

**The honest counter-number.** [`LANE_CONTRACT.md`](LANE_CONTRACT.md) is 198
lines / ~11,100 characters — about 3.4× the preamble it replaces. An executor
that reads the whole contract reads *more* text than the paste, not less. So the
saving is **not** in tokens read. It is in:

1. **One place to change.** The measured cost of the old shape was three
   workflow restarts in one day, because doctrine lived inside the workflow
   script and a doctrine change meant stopping and relaunching every lane. A
   tracked file is edited between lanes, or during one, with no restart.
2. **Auditability.** Every clause in the contract carries a citation to a file
   or a recorded incident. A pasted preamble carries none, so a wrong clause is
   indistinguishable from a right one and propagates to every future lane.
3. **Version pinning.** "Follow LANE_CONTRACT v1" makes staleness detectable. A
   pasted preamble has no version, so no lane can be told it followed the old
   rules.
4. **Prompt authoring cost**, which is what the −87% actually measures: the
   hand-written, hand-maintained part of each lane launch.

## Before — the hand-written preamble (55 lines, 3,302 characters)

Reproduced as written, with **one redaction**: the line that pinned the
registered-test-group and lint-gate counts is shown with placeholders.
Reproducing either number verbatim in a tracked document would trip this repo's
own `check-doc-counts` and `check-doc-accuracy` gates the moment that count moves
— and it did, the moment this file merged alongside a lane that added a gate.
That is precisely the argument for deriving them
([`LANE_CONTRACT.md`](LANE_CONTRACT.md) §F). The redaction adds 6 characters to
the 3,308-character block shown here; the 3,302 figure above is the unredacted
original.

```text
You are an executor in the zclassic23 repository, working on the AGENT
DEVELOPMENT HARNESS itself — the tooling this project's own AI orchestrator uses
to run lanes. Your user is that orchestrator.

PROJECT DOCTRINE — violations are rejected outright:
* NEVER 'git stash'. The stash is SHARED across worktrees and has already cost
  this project two lanes' work.
* NEVER touch or unseal core/. It is byte-sealed by core/MANIFEST.sha3.
* NEVER run a native command that takes a 'datadir' input without an explicit
  --datadir=/tmp/... . It defaults to the OWNER'S LIVE NODE and ~43 nominally
  read-only leaves write to it.
* C23 only for compiled code. No Rust, NO PYTHON (a standing owner rule), no
  CMake, no new external dependency. Shell is acceptable for dev tooling and is
  what tools/dev/ already uses.
* Avoid 'printf ... | grep -q' under 'set -o pipefail': a match becomes exit 141
  and INVERTS the decision. This has already shipped real inverted gates in this
  repo. Use tools/scripts/sh_str.sh.
* Scope every text search with 'git grep', never 'grep -r' from the repo root
  (untracked full checkouts live under .claude/worktrees/, scratch under
  test-tmp/).
* LOG_FAIL/LOG_ERR/LOG_NULL from util/log_macros.h RETURN a value. Every
  allocation goes through zcl_malloc(size, "label").
* Green means the literal token 'ALL TESTS PASSED' present AND
  'SOME TESTS FAILED' absent. A zero exit alone is NOT green.
* Commit messages speak to users, not as a dev diary.

BUILD ETIQUETTE — three other workflows are running RIGHT NOW on this host:
* A critical-path lane owns /home/rhett/github/zclassic23-w1d. Do not touch it.
* 'make lint' (~29 s) is fine. 'make build-only' is acceptable but heavy (664
  parallel cc). Do NOT run 'make test-parallel' (the full suite) — it would
  contend with the critical path's verifier. Use 'make t-fast ONLY=<group>'.
* Never run two heavy builds concurrently with yourself.

Your final message IS your return value. Be specific and honest. Never claim a
gate you did not run. An honest "I could not do X" is worth far more than a
claim that does not hold.

HOUSE FACTS — verified by the orchestrator; re-read anything you rely on.
* Makefile has 389 targets. Relevant ones: 't-fast' (l.1462), 'test-parallel'
  (l.1432), 'lint' (l.6855, 128 gates), 'ci' (l.6965), 'worktree-prime' (l.929),
  'worktree-init' (l.5219).
* Existing dev tooling to match in style: tools/dev/agent-doctor.sh,
  tools/dev/agent-dev-status.sh, tools/dev/agent-clear-stale-reindex.sh,
  tools/dev/generate-compdb.sh, tools/agent_fast_ci.sh, tools/agent_test_runner.sh.
* Lint gates live in tools/lint/check_*.sh and are wired into the 'lint' target.
  Read two of them before writing anything that must pass as a gate.
* The string-safety helper that exists because of the pipefail inversion:
  tools/scripts/sh_str.sh.
* The only skill in the repo: .claude/skills/zclassic23-dev/SKILL.md — a stub
  that imports docs/DEVELOPING.md.
* The doctrine an agent must know is scattered across CLAUDE.md,
  docs/DEVELOPING.md, docs/DEFENSIVE_CODING.md, docs/AGENT_TRAPS.md.
* Baseline to pin: main = b82b40e77, <derived group count>, <derived gate count>.
* SHA3 utilities already exist in-tree — find them with 'git grep -l sha3 -- lib'.
  Do not add a hashing dependency.
```

## After — the same frame, referencing the contract (6 lines, 426 characters)

```text
You are an executor in the zclassic23 repository. Work in
/home/rhett/github/zclassic23-t3 on branch lane/agent-skills.
Follow LANE_CONTRACT v1 (docs/agent/LANE_CONTRACT.md) — read it first; it is the
doctrine, the build etiquette and the gate definitions for this lane.
Report in the shape of docs/agent/LANE_REPORT.md as your final message.
You own: .claude/skills/**, docs/agent/**. Do not touch: Makefile, tools/, lib/.
```

The task description follows unchanged. Nothing else about the stash, the
datadir default, pipefail, `core/`, C23, the pass token, or the house facts
belongs in the prompt.

## What the "after" form deliberately drops

* **The house-facts block.** Every fact in it is derivable at lane start —
  [`LANE_LAUNCH.md`](LANE_LAUNCH.md) §2 gives the commands. A hand-typed
  Makefile line number is wrong after the next merge; `grep -n` never is.
* **The build-etiquette block.** It is now
  [`LANE_CONTRACT.md`](LANE_CONTRACT.md) §D, minus the host-specific detail
  (which lane owns which directory) that genuinely belongs in the task, because
  it changes per workflow.
* **The "be honest, quote your gates" paragraph.** It is now
  [`LANE_CONTRACT.md`](LANE_CONTRACT.md) §E plus
  [`LANE_REPORT.md`](LANE_REPORT.md) §5, with the exact terminal line to quote
  for each gate — which the paragraph never specified, so every lane invented
  its own format.

## Reproducing these numbers

```sh
# write the two prompt frames to files, then:
wc -l -w -c before.txt after.txt
```

Nothing here is estimated; if the contract grows, re-run the counts and update
the table above rather than leaving a stale one.

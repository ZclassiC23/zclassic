# LANE_CONTRACT — the doctrine an executor lane follows

    VERSION: v1

> **Who this is for.** Everything in `docs/agent/` describes how *this
> repository's maintainer* runs several AI executor agents at once, each in its
> own git worktree on one checkout. **It is not a requirement for contributing
> and it is not the project's development process.** If you are here to build
> the node or send a patch, you need none of it — read
> [`../GETTING_STARTED.md`](../GETTING_STARTED.md), then
> [`../../.github/CONTRIBUTING.md`](../../.github/CONTRIBUTING.md) and
> [`../DEVELOPING.md`](../DEVELOPING.md). The rules below exist only because
> parallel agents sharing one checkout collide over things a single developer
> never has to think about: one stash stack, one build directory, one branch
> namespace.

A lane prompt says *"follow LANE_CONTRACT v1 (`docs/agent/LANE_CONTRACT.md`)"*
and nothing else about doctrine. **Do not paste these rules into a prompt** — a
pasted copy is a second ledger that drifts, the failure mode
[`ARCHITECTURE_NORTH_STAR.md`](../ARCHITECTURE_NORTH_STAR.md) calls cloned
ledgers. Bump `VERSION` when a clause changes meaning, so a prompt pinned to v1
can be told it is stale.

**Scope.** Doctrine for one executor, one lane, one worktree. The lane *ritual*
(startup, per-task discipline, completion, forbidden moves) is
[`docs/work/agent-protocol.md`](../work/agent-protocol.md) and is not repeated
here. The report you owe is [`LANE_REPORT.md`](LANE_REPORT.md); the developer
manual is [`docs/DEVELOPING.md`](../DEVELOPING.md). **Every clause carries a
citation** — no citation, no clause. If a citation and the tree disagree, the
tree wins, and you say so (§G).

## A. The six that have already cost this project work

**A1 — Never `git stash`.** The stash stack lives in the *common* git dir: in a
worktree `git rev-parse --git-dir` and `--git-common-dir` differ, and
`refs/stash` resolves in the common one. Every worktree on this checkout shares
one stash, so your `git stash` can bury or restore another lane's work. Park WIP
as a commit on your lane branch.
*Cite:* the two `git rev-parse` forms + `git worktree list` (verify it yourself);
shared-object-store premise in `docs/work/agent-protocol.md` §Integration
discipline. Owner-recorded incident (2026-07-29): the shared stash cost two lanes
their work — that record is in the owner's memory index, not in-tree; this clause
is its first in-tree statement.

**A2 — Never invoke a `datadir`-taking command without an explicit
`--datadir=/tmp/…`.** The `datadir` input falls back to the operator's canonical
datadir. Leaves declared READ have opened it read-write, migrated its schema and
deleted rows; `node.db` is WAL, so a running node blocks none of it.
*Cite:* `lib/test/src/test_read_leaf_no_datadir_write.c` header — the reproduced
2026-07-29 bug, and the authority for which leaves are covered vs. still open
(never a hand count). Also `docs/AGENT_TRAPS.md` §0, "A flagless CLI invocation
targets the CANONICAL datadir."

**A3 — Never `printf … | grep -q` (or `echo | grep -q`) under `set -o
pipefail`.** `grep -q` exits at the first match, `printf` takes SIGPIPE,
`pipefail` surfaces `141` — a *successful match* becomes indistinguishable from a
miss and the decision inverts. For a gate greping for a violation, "found it"
reads as "clean". Use `tools/scripts/sh_str.sh`.
*Cite:* `tools/scripts/sh_str.sh` header (measured 30/30 false verdicts at 349 KB
of input); `tools/lint/check_pipefail_status_pipe.sh` (a release-symbol gate
reported a planted violation CLEAN in 20 of 20 runs).

**A4 — Never edit, unseal or re-seal `core/`.** It is byte-sealed by
`core/MANIFEST.sha3`; `check-core-seal` fails `make lint` *after* your edit is
already written. Unsealing is an owner ritual, not a lane move. If the task seems
to require a `core/` edit, stop and report the conflict (§G).
*Cite:* `CLAUDE.md` §"Every consensus predicate lives under core/";
`docs/adr/0002-sealed-consensus-core.md`; `core/UNSEAL.md`.

**A5 — Green is a token, not an exit code.** The suite prints `SUITE VERDICT
mode=… groups_total=N groups_ran=N groups_failed=N` and then one of `ALL TESTS
PASSED` / `ALL TESTS PASSED (CACHED)` / `SOME TESTS FAILED`. Green = `ALL TESTS
PASSED` present **and** `SOME TESTS FAILED` absent. A bare `grep -q "ALL TESTS
PASSED"` also matches the `(CACHED)` form, which can mean **zero groups ran** —
that false green already shipped once.
*Cite:* `lib/test/src/test_parallel.c` (comment above the `SUITE VERDICT`
printf); `docs/DEVELOPING.md` §Build/test/deploy; `docs/work/agent-protocol.md`
§Completion ritual.

**A6 — No new language, no new dependency.** Compiled code is C23. No Rust, no
Python, no CMake, no third-party library. Shell is the accepted medium for dev
tooling — write it dependency-free (no `jq`, no helper binary).
*Cite:* stated at its use sites — `tools/scripts/intervention_ledger.sh` ("No
python (banned), no jq"), `tools/scripts/simnet_nightly.sh` ("no python (project
ban)"), `tools/mvp_gate.sh` ("no python/sqlite3 per project rule");
`docs/GETTING_STARTED.md` (stock `cc`/`ld`/`make`). Pre-existing Python under
`tools/scripts/` is legacy, not a licence to add more.

## B. Standing engineering rules

**B1 — Scope every text search.** `git grep`, never `grep -r` / `find .` from the
repo root: `.claude/worktrees/` holds a full checkout per lane and `test-tmp/`
per-run scratch, both untracked. *Cite:* `CLAUDE.md` §"First five commands";
`docs/DEVELOPING.md` §Understand fast.

**B2 — Ask the navigator before you grep or read.** `z23 code
sym|refs|capsule` answers "where is X / who calls X / what breaks if I change it"
for a fraction of a file read. *Cite:* `docs/DEVELOPING.md` §Understand fast.

**B3 — Defensive coding is lint-enforced.** Allocations go through
`zcl_malloc(size, "label")`; every error return logs context with
`LOG_FAIL`/`LOG_ERR`/`LOG_NULL` (**they return a value** — read the macro before
using it); every write goes through the ActiveRecord lifecycle, never raw
`sqlite3_step()`; every native command handler sets an error body.
*Cite:* `docs/DEFENSIVE_CODING.md`; `util/safe_alloc.h`; `util/log_macros.h`;
`models/activerecord.h`; gates `check-malloc`, `check-raw-malloc`,
`check-log-macro-return-type`, `check-model-ar-lifecycle`.

**B4 — Consensus parity is inviolable.** Never ship a consensus change to
z23 first, not even opt-in; validate against the real chain, not the
`zclassicd` source text. *Cite:* `docs/CONSENSUS_PARITY_DOCTRINE.md`; gate
`check-consensus-parity`.

**B5 — Copy-prove before live; never live surgery.** Copy the datadir, reproduce
there, prove the fix *fires* on the copy, then deploy. Gate on H\* climbing, not
on "booted without FATAL". *Cite:* `docs/AI_SAFETY_GATES.md` §Copy-prove;
`tools/repro_on_copy.sh`.

**B6 — Less is more.** Prefer deleting or unifying over adding; a second copy of
an existing fact is a defect. *Cite:* `docs/DEVELOPING.md` §The inviolable rules;
`docs/ARCHITECTURE_NORTH_STAR.md`.

## C. Lane mechanics

**C1 — Stay inside your declared file set.** Lanes run concurrently on disjoint
ownership; an edit outside your set is a merge conflict charged to the whole
workflow. Something outside your set that now needs changing goes in the report,
not in your diff.
*Cite:* `docs/work/agent-protocol.md` §Forbidden moves; `docs/DEVELOPING.md`
§Parallel-worktree workflow.

**C2 — Commit on your lane branch; never on `main`, never push, never force.**
All worktrees share one object store, so handing off is a branch name plus a head
SHA. No `--no-verify`; no `--amend` after the orchestrator merged.
*Cite:* `docs/work/agent-protocol.md` §Integration discipline, §Forbidden moves.

**C3 — Commit messages describe the change to a user of the software.**
Imperative subject under ~70 characters; the body says *why*; the *what* is the
diff. No lane numbers, no session narrative.
*Cite:* `docs/work/agent-protocol.md` §Commit discipline. The "speaks to users,
not a dev diary" framing is an owner directive in the owner's memory index, not
in-tree; the in-tree analogue is the gate `check-no-dev-history-in-contracts`.

## D. Shared-host build etiquette

Several lanes run on this host at once. One heavy build at a time, per lane and
per host. Never touch another lane's worktree.

* `make lint` — cheap enough to run freely (per-gate cached).
* `make build-only` — heavy (hundreds of parallel `cc -c`) and **does not
  link**, so green there is not green.
* `make t-fast ONLY=<substring>` — the focused run. `ONLY=` is **mandatory,
  unvalidated, and a substring**: a literal placeholder gives a full test-binary
  compile then `sh: 1: Syntax error: end of file unexpected`, and an `ONLY=`
  matching nothing runs nothing and still exits green. Always confirm
  `groups_ran`.
* `make test-parallel` — the full suite; do not start one against another lane's
  verifier unless asked.

*Cite:* `CLAUDE.md` §"First five commands" (the `ONLY=` footgun, verbatim);
`docs/DEVELOPING.md` §Build/test/deploy.

## E. Gate receipts — what you must be able to quote

A gate claim is worth nothing unless it can be checked without re-running the
gate. Quote the literal terminal line:

| Gate | Quote | Machine receipt |
|---|---|---|
| `make lint` | `── lint timing: N gates, wall … ──`, and absence of `══ LINT: … gates FAILED ══` | `.cache/lint-timing/last-run.json` → `"failed_count":0` |
| `make t-fast` / `make test-parallel` | the `SUITE VERDICT …` line **and** the verdict token line | `.cache/test-timing/last-run.json` |
| `make build-only` | its last line, plus the reminder that it does not link | — |

Never paraphrase a verdict. Never report a gate you did not run — a gate you
could not run is reported as *not run*, with the reason. Report `groups_ran`, not
just the token: a cached or empty run is the failure mode (A5).
*Cite:* `tools/lint/run_lint.sh` (summary + JSON artifact);
`lib/test/src/test_parallel.c` (`SUITE VERDICT`);
`tools/scripts/gate-and-report.sh` (lint → link build → suite).

## F. Derive facts; never pin them

Anything the tree can compute is computed at lane start, not typed into a prompt
or a doc. This repo lints against pinned counts in Markdown
(`check-doc-counts`, `check-doc-accuracy`, `check-no-stale-pinned-facts`)
precisely because prose copies rot.

| Fact | Derivation |
|---|---|
| baseline commit | `git rev-parse HEAD` |
| content id of a file that must not change | `git rev-parse HEAD:<path>` — git's own blob id, no hashing dependency |
| registered test groups | `git grep -hoE 'X\([a-z_0-9]+\)' lib/test/src/test_parallel.c \| tr -d 'X()'` — keep `-h`, or the filename is glued to every name |
| umbrella lint gate set | the `LINT_GATES` list in `Makefile` (authoritative for what runs) |
| which worktree am I | `pwd`; `git rev-parse --git-common-dir` for the shared store |

*Cite:* `CLAUDE.md` §"First five commands" (the `-h` trap);
`tools/scripts/check_doc_counts.sh`; `tools/scripts/check_doc_accuracy.sh` ("the
intended fix is not to re-pin a fresh number; it is to delete the number and
point at the derived source"); `tools/lint/check_no_stale_pinned_facts.sh`.

## G. When this contract is wrong

A lane that silently follows a wrong contract recreates the defect the contract
exists to delete. If a clause contradicts the tree, is unfollowable, or names a
path that has moved: follow the **tree**, not this file; do **not** edit this
file mid-lane (it is shared — an edit here is a cross-lane conflict); and record
it under **"Where the contract was wrong"** in
[`LANE_REPORT.md`](LANE_REPORT.md) with the clause id, what you did instead, and
the evidence. That section is this contract's only maintenance path — an
unreported wrong clause costs every future lane.

# LANE_LAUNCH — the orchestrator's checklist for starting a lane

    VERSION: v1

> **Who this is for.** The maintainer's own multi-agent setup — several AI
> executors running at once, each in its own worktree on one checkout. Not a
> requirement for contributing; see [`LANE_CONTRACT.md`](LANE_CONTRACT.md) for
> the full note. A contributor wants
> [`../GETTING_STARTED.md`](../GETTING_STARTED.md) and
> [`../DEVELOPING.md`](../DEVELOPING.md) instead.

This is the body of the `zclassic23-lane` skill. It is the **launch** side; the
executor's side is [`LANE_CONTRACT.md`](LANE_CONTRACT.md) (doctrine) and
[`LANE_REPORT.md`](LANE_REPORT.md) (the report shape). The per-worker ritual
already lives in [`docs/work/agent-protocol.md`](../work/agent-protocol.md) and
is not restated here.

**The point of this page:** a lane prompt should carry the *task* and nothing
else. Doctrine is referenced, not pasted. Ground truth is derived, not typed.
Worked before/after with real numbers:
[`EXAMPLE_LANE_PROMPT.md`](EXAMPLE_LANE_PROMPT.md).

## 1. Pick a worktree

Every lane gets its own worktree; two lanes in one checkout is a lost lane.

```sh
git worktree list                       # what exists, and what is locked
tools/scripts/worktree_gc.sh            # DRY RUN: classify stale lanes
git worktree add ~/github/zclassic23-<slug> -b lane/<slug> main
cd ~/github/zclassic23-<slug> && make worktree-prime   # copy vendor/lib/*.a
```

* `make worktree-prime` first, before any other `make` — a fresh worktree cannot
  link without the gitignored `vendor/lib/*.a`, and priming copies them in
  ~1 s instead of rebuilding from pinned source
  ([`docs/DEVELOPING.md`](../DEVELOPING.md) §Parallel-worktree workflow).
* Reclaim with `tools/scripts/worktree_gc.sh --apply`, never `rm -rf` — the GC
  has a hard protect list (locked lanes, unmerged branches, dirty trees) and a
  hand `rm -rf` leaves stale git admin state. Directories present on disk but
  absent from `git worktree list` are **reported only, never auto-removed**; a
  worktree whose build artifacts are not removable by the current user has to be
  cleared by the owner before `git worktree remove` will succeed.
* Confirm which checkout the live node is actually running before diagnosing
  anything live: `make agent-doctor` reports `running_this_tree`.

## 2. Pin the baseline — by deriving it, never by typing it

Run this in the new worktree and paste its output into the lane's scratch notes
(not into the prompt):

```sh
echo "baseline:      $(git rev-parse HEAD)"
echo "branch:        $(git rev-parse --abbrev-ref HEAD)"
echo "worktree:      $(pwd)"
echo "common gitdir: $(git rev-parse --git-common-dir)"
echo "test groups:   $(git grep -hoE 'X\([a-z_0-9]+\)' lib/test/src/test_parallel.c | tr -d 'X()' | wc -l)"
echo "lint gates:    $(awk '/^LINT_GATES[[:space:]]*:=/{f=1} f{print; if ($0 !~ /\\[[:space:]]*$/) exit}' Makefile | grep -coE 'check-[a-z0-9-]+')"
```

For "these files must not change", use git's own content ids — no hashing
dependency, and they are stable across checkouts:

```sh
git rev-parse HEAD:core/MANIFEST.sha3 HEAD:Makefile
```

Do **not** write derived counts into a tracked doc: `check-doc-counts`,
`check-doc-accuracy` and `check-no-stale-pinned-facts` fail `make lint` on pinned
counts in Markdown, and they are right to.

## 3. Structure disjoint file ownership

* Give each lane an explicit **owns** list and an explicit **do not touch** list,
  by path prefix. Overlap of one file costs the whole workflow a conflict.
* High-contention files that should belong to at most one lane per workflow:
  `Makefile`, `lib/test/src/test_parallel.c`, `docs/CODEBASE_MAP.md`, anything
  under `core/` (sealed — normally nobody).
* Tell the lane what to do with work it finds outside its set: report it under
  [`LANE_REPORT.md`](LANE_REPORT.md) §6, do not edit it.
* Merge in dependency order; a lane that adds a caller merges after the lane that
  adds the callee.

## 4. Name the gates you will demand

Ask for the smallest set that actually proves the change, and demand the
**literal verdict line** for each ([`LANE_CONTRACT.md`](LANE_CONTRACT.md) §E) —
otherwise checking the claim costs a full re-run.

| change class | gates to demand |
|---|---|
| docs / skills only | `make lint` |
| shell tooling under `tools/` | `make lint`, plus the tool's own `--selftest` if it has one |
| a `.c` change | `make lint`, `make build-only`, `make t-fast ONLY=<group>` |
| anything on the chain-advance path | the above plus live forward-progress evidence (`docs/work/agent-protocol.md` §Completion ritual) |
| merge candidate | `tools/scripts/gate-and-report.sh <lintlog> <testlog>` — lint → link build → full suite, keyed on the pass token, rejects a cached run |

`make build-only` does **not** link, so it cannot catch a missing symbol; green
there is not green.

## 5. Write the prompt

Five lines of frame, then the task:

```
You are an executor in the zclassic23 repository. Work in <abs worktree path>
on branch lane/<slug>. Follow LANE_CONTRACT v1 (docs/agent/LANE_CONTRACT.md) —
read it first; it is the doctrine, the build etiquette and the gate definitions.
Report in the shape of docs/agent/LANE_REPORT.md as your final message.
You own: <paths>. Do not touch: <paths>.

TASK: <the actual work>
```

Nothing about the stash, the datadir default, pipefail, `core/`, C23, or the
pass token belongs in the prompt. It is all in the contract, under version
control, in one place. If doctrine changes, edit the contract and bump its
`VERSION` — do not relaunch the workflow.

## 6. On the way back

Check the report against [`LANE_REPORT.md`](LANE_REPORT.md) before merging.
Two sections decide whether to trust the rest: **§5 gates** (is a literal verdict
line quoted, with `groups_ran`?) and **§7 where the contract was wrong** (empty
in a lane that obviously hit friction is a red flag, not a clean bill).

Then review the diff (`git diff main...lane/<slug>`), merge in dependency order,
and fold any §7 finding back into [`LANE_CONTRACT.md`](LANE_CONTRACT.md) with a
`VERSION` bump. That fold-back is the only thing that keeps the contract from
becoming another stale doc.

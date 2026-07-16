# Worktree cleanup — 2026-07-16

Snapshot: `git worktree list` shows **69** registered worktrees against `main` @
`d27d6e603` (the task that requested this list was opened when the count was
~65; a handful more were created by in-flight workflow lanes between then and
this audit — see `LOCKED / ACTIVE` below).

**This document is a produced list only. Nothing in it has been executed.**
No `git worktree remove` / `git branch -D` was run. Every command below is
written out for the owner (or a follow-up agent) to run by hand, in the
listed order, after a final glance.

## Method

For each entry in `git worktree list --porcelain` under
`.claude/worktrees/<name>`:

1. **`locked`** — from git's own worktree-lock bit (`git worktree lock`).
   Locked worktrees are excluded unconditionally regardless of merge status —
   a lock is an explicit "still in use" signal (in this repo, an active
   workflow lane). At the time of this audit, `wf_2d9e8fbd-6e5-2` was
   *provably* live: its `tools/scripts/import-copy-prove.sh` was mid-run as a
   child process (`ps aux`), confirming the lock is not stale.
2. **`merged_into_main`** — `git merge-base --is-ancestor <worktree HEAD> main`.
   `yes` means every commit reachable from that worktree's branch tip is
   already on `main`; the worktree's *committed* history is fully captured
   upstream and the branch is redundant.
3. **dirty / clean** — `git status --porcelain=v1 -uall` run *inside* the
   worktree. This catches uncommitted modifications AND untracked files that
   `merged_into_main` cannot see (a branch can be fully merged while its
   working tree still holds abandoned edits that were never committed at
   all). `git worktree remove` refuses on dirty content unless forced, and
   forcing silently discards it — so dirty worktrees get a separate,
   more cautious bucket below even when merged.

Four buckets result:

| Bucket | Meaning | Action |
|---|---|---|
| **LOCKED / ACTIVE** | git-locked, in use by a live lane | **skip — do not touch** |
| **SAFE** | merged AND clean | `git worktree remove` + `git branch -D` |
| **CAUTION** | merged but working tree has uncommitted content | eyeball the diff first (commands below print it), then remove |
| **KEEP** | branch tip has commits NOT on `main` | **do not remove** — unmerged work |

## LOCKED / ACTIVE — excluded, do not touch (6)

| worktree | branch | note |
|---|---|---|
| `wf_2d9e8fbd-6e5-1` | `worktree-wf_2d9e8fbd-6e5-1` | sibling lane of the workflow that produced this doc |
| `wf_2d9e8fbd-6e5-2` | `worktree-wf_2d9e8fbd-6e5-2` | **confirmed live**: `import-copy-prove.sh` running as a child process at audit time |
| `wf_2d9e8fbd-6e5-4` | `worktree-wf_2d9e8fbd-6e5-4` | this worktree (produced this doc + the `reducer_frontier_dump.c` change) |
| `wf_849bc50e-d3e-1` | `worktree-wf_849bc50e-d3e-1` | another workflow's lane |
| `wf_849bc50e-d3e-3` | `worktree-wf_849bc50e-d3e-3` | another workflow's lane |
| `wf_849bc50e-d3e-4` | `worktree-wf_849bc50e-d3e-4` | another workflow's lane |

Re-run the audit script (below) once these workflows land/merge — most will
drop into SAFE at that point.

## KEEP — unmerged commits, do NOT remove (8)

| worktree | branch | unmerged commits | tip subject |
|---|---|---|---|
| `agent-aaaa4333f1f0e86b5` | `worktree-agent-aaaa4333f1f0e86b5` | 2 | `Merge branch 'main' into ops.rom — reconcile with pv_lookahead/bn254_accel` |
| `agent-aba30104b9d13a048` | `pv-lookahead` | 1 | `mint: pre-verify shielded proofs ahead of the fold drive` |
| `wf_2d9e8fbd-6e5-3` | `worktree-wf_2d9e8fbd-6e5-3` | 1 | `docs(cutover): owner-gated live cutover runbook for the import-path cure` |
| `wf_491b069f-188-3` | `worktree-wf_491b069f-188-3` | 1 | `feat(net): OS-A6 adaptive client-puzzle admission primitive` |
| `wf_ed06ab0f-b37-1` | `worktree-wf_ed06ab0f-b37-1` | 1 | `docs(fold): design the Parallel State Compiler (order-independent range-fold)` |
| `wf_f035bec1-ad9-1` | `worktree-wf_f035bec1-ad9-1` | 1 | `perf(shielded-import): never-silent progress + drop the redundant per-anchor root re-hash` |
| `wf_f809738c-c9d-3` | `worktree-wf_f809738c-c9d-3` | 1 | `feat(storage): harden sealed-segment substrate — prune-after-seal + writer-frontier gate + read-source counters` |
| `wf_f809738c-c9d-4` | `worktree-wf_f809738c-c9d-4` | 1 | `perf(boot): OS-S2 finish — cursor-gate the deferred #3 single-pass scan` |

These look like real, un-landed work (some may be superseded-in-spirit by a
squash-merge elsewhere under a different commit hash, which `--is-ancestor`
cannot detect) — verify with `git log main.. -p` / `git cherry main <branch>`
per-branch before deciding, don't bulk-drop.

## CAUTION — merged, but the working tree has uncommitted content (4)

`git worktree remove` will refuse these without `--force`; forcing discards
the uncommitted content permanently. Diff each before deciding.

| worktree | branch | uncommitted content |
|---|---|---|
| `agent-a291a0465e2a94cb7` | `worktree-agent-a291a0465e2a94cb7` | modified `boot.h`/`chainstate_legacy_reader.*`/`nullifier_kv.c` + untracked `shielded_history_import_service.{c,h}`, `boot_import_shielded_history.c` — reads as an **earlier draft of the now-landed shielded-history importer** (same filenames as the real thing on `main` today); almost certainly superseded scratch, but confirm with a diff before discarding |
| `wf_1c1df811-ee0-3` | `perf/fold-vh-skip-hoist` | 1 modified file (`validate_headers_validator.c`) |
| `wf_491b069f-188-2` | `worktree-wf_491b069f-188-2` | modified `agent_impact_rules.def`/`test_helpers.h`/`test.c` + untracked `test_sovereign_cure_rehearsal.c` |
| `wf_b897cac6-41c-1` | `worktree-wf_b897cac6-41c-1` | modified `agent_impact_rules.def`/`test_parallel.c` + untracked `test_import_fold_continuity.c` |

Suggested review command per row (prints the diff without touching anything):
```bash
git -C .claude/worktrees/<name> status -uall
git -C .claude/worktrees/<name> diff
```

## SAFE — merged AND clean (51)

Every commit is already on `main`; the working tree matches `HEAD` exactly
(no modified or untracked files). Safe to run, in this order, once you've
confirmed nothing above is still building against one of these paths:

```bash
# from the main checkout (/home/rhett/github/zclassic23)
for wt in \
  agent-a0e79fd7cbd592b5a agent-a1c7a494bcd23527a agent-a34822400a8434d45 \
  agent-a3730bdce81137f36 agent-a680e751a84b1807b agent-a711c7dfe04350608 \
  agent-a72a1a7ffcab3bbf9 agent-aad9ecf4cc5ddaccf agent-aba54b0f6f26b21bc \
  wf_15c48e10-bed-1 wf_15c48e10-bed-2 wf_15c48e10-bed-3 wf_15c48e10-bed-4 \
  wf_2b93d95e-816-1 wf_2b93d95e-816-2 \
  wf_38a8a7cc-2b5-1 wf_38a8a7cc-2b5-2 wf_38a8a7cc-2b5-3 \
  wf_491b069f-188-1 wf_491b069f-188-4 \
  wf_5aeface4-aba-1 wf_5aeface4-aba-2 wf_5aeface4-aba-3 wf_5aeface4-aba-4 \
  wf_7d44ab99-a21-1 wf_7d44ab99-a21-2 wf_7d44ab99-a21-3 wf_7d44ab99-a21-4 \
  wf_92ee534b-6ce-1 \
  wf_b897cac6-41c-2 wf_b897cac6-41c-4 \
  wf_c2a91bd5-4f3-1 wf_c2a91bd5-4f3-2 wf_c2a91bd5-4f3-3 wf_c2a91bd5-4f3-4 \
  wf_ca8805ba-6a6-1 wf_ca8805ba-6a6-2 wf_ca8805ba-6a6-3 wf_ca8805ba-6a6-4 \
  wf_ed06ab0f-b37-2 \
  wf_f035bec1-ad9-2 wf_f035bec1-ad9-3 wf_f035bec1-ad9-4 \
  wf_f5abb511-2d2-1 wf_f5abb511-2d2-2 wf_f5abb511-2d2-3 wf_f5abb511-2d2-4 \
  wf_f5abb511-2d2-5 wf_f5abb511-2d2-6 \
  wf_f809738c-c9d-1 wf_f809738c-c9d-2 \
; do
  branch=$(git -C ".claude/worktrees/$wt" rev-parse --abbrev-ref HEAD)
  echo "== $wt (branch=$branch) =="
  git worktree remove ".claude/worktrees/$wt"      # refuses if it's not actually clean/merged — that's the safety net
  git branch -D "$branch"
done
```

Two branches in this bucket have non-`worktree-*` names —
`wf_7d44ab99-a21-1` → `perf/utxo-apply-read-stmt-cache` (merged as
`f8e680c70`) — the loop above derives the branch name from the worktree
itself rather than assuming the `worktree-<dirname>` pattern, so this is
handled correctly without a special case.

`git worktree remove` is the safety net here, not a formality: it independently
re-checks for uncommitted/untracked content and **refuses** if the worktree
drifted since this audit was taken (e.g. a lane resumed writing to one of
these paths between the snapshot above and execution). Don't add `--force` to
this loop.

## Re-running this audit

The classification above was produced by a short read-only script (worktree
list → per-entry ancestry + status check, no writes). To re-derive it fresh
(e.g. after landing the KEEP branches, or once the LOCKED lanes finish):

```bash
cd /home/rhett/github/zclassic23
git worktree list --porcelain | awk '
/^worktree /{ if (path!="") print path"\t"head"\t"branch"\t"locked; path=$2; head=""; branch=""; locked="no"; next }
/^HEAD /{ head=$2 }
/^branch /{ branch=$2 }
/^locked/{ locked="yes" }
/^detached/{ branch="(detached)" }
END{ if (path!="") print path"\t"head"\t"branch"\t"locked }
' | while IFS=$'\t' read -r path head branch locked; do
    case "$path" in */.claude/worktrees/*) ;; *) continue ;; esac
    branch_short="${branch#refs/heads/}"
    if git merge-base --is-ancestor "$head" main 2>/dev/null; then merged=yes; else merged=no; fi
    status=$(git -C "$path" status --porcelain=v1 -uall 2>/dev/null)
    [ -z "$status" ] && dirty=clean || dirty=DIRTY
    echo "$path|$branch_short|$merged|$dirty"
done
```

## Out of scope (noted, not acted on)

`git for-each-ref refs/heads/` currently lists **153** local branches that
have *no* worktree at all (their worktree was already removed but the branch
never got `git branch -D`'d, or they were created directly). That is a
separate, larger cleanup from "which of the 65-69 registered worktrees are
stale" — this doc does not enumerate or judge those 153; a follow-up pass
should treat them the same way (ancestor-of-main check) before any bulk
delete.

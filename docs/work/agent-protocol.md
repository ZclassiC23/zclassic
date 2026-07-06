# Agent Protocol — Worker Startup & Completion

**Every worker agent in a worktree (`~/github/zclassic23-N`) MUST follow
this protocol exactly.** It is the contract between the orchestrator and
the workers. Deviating breaks coordination and risks data loss.

**Workflow:** workers push DIRECTLY to `main`. No feature branches. No
orchestrator merge step. The remote has exactly one branch (`main`),
and every worker rebases on top of it before pushing.

---

## Startup ritual (run on EVERY fresh session)

```
1. Identify yourself
   $ pwd                                  # e.g., ~/github/zclassic23-2
   → worktree ID = "wt2"  (extract suffix after "zclassic23-")
   → if no suffix → you are the orchestrator; read the orchestrator section
                    of REFACTOR_STATUS.md, NOT a worker assignment

2. Sync from origin (always main, no feature branches)
   $ git fetch origin
   $ git checkout main
   $ git pull --ff-only origin main

3. Load the architecture
   $ cat docs/FRAMEWORK.md                # canonical architecture
   $ cat docs/REFACTOR_STATUS.md          # current phase + your row in "In flight"

4. Load your assignment
   $ ls docs/work/wt<N>-*.md docs/work/wt-*.md   # specific + cross-worker
   $ for f in docs/work/wt<N>-*.md docs/work/wt-*.md; do
       [ -e "$f" ] || continue
       echo "=== $f ==="; grep -A2 '^## Status$' "$f" | head -4
     done
   # Pick the assignment whose Status is **READY** (or IN PROGRESS (wt<N>) resumable).
   # `wt<N>-*.md` are worker-specific. `wt-*.md` are cross-worker — claim by
   # marking IN PROGRESS (wt<N>). First worker to mark wins; second worker
   # skips it and picks the next READY assignment.
   # SKIP any whose Status starts with "✅ DONE — merged" — work is closed.
   # SKIP any whose Status starts with "QUEUED" — gated on prerequisite.
   $ cat docs/work/<chosen-file>           # full spec — branch name, scope, tasks

5. Verify the assignment doc's "Status" section: DONE → report to user;
   BLOCKED/FAILED → re-read, retry or escalate; READY/IN PROGRESS (wt<N>) → proceed.

6. Stay on main (see Workflow note above — commit directly to main).
   - **IGNORE any `**Branch:**` field at the top of older assignment docs.**
     That was a legacy of the feature-branch workflow; the field is dead.
     You stay on `main` regardless of what the doc says.

7. Mark in-progress + push
   - Edit the assignment doc's Status section to "IN PROGRESS (wt<N>)"
   - $ git add docs/work/<file>
   - $ git commit -m "wt<N>: mark <slug> in progress"
   - $ git push origin main                   # see "Push discipline" below
```

Then **execute the assignment's Tasks section in order**.

---

## Per-task discipline

Each Task has a **scope** (exact files) and an **acceptance test** (concrete
check that proves done). For each task:

1. `git pull --rebase origin main` to stay current.
2. Implement.
3. Run the acceptance test (`make test_parallel`, build, lint).
4. **Green:** `git add` the specific files, commit, `git push origin main`.
5. **Red:** debug; never commit a broken state. Stuck > 30 min → append a
   `BLOCKED` note to the assignment.

One commit per task — no megacommits.

---

## Commit discipline

- **Subject < 70 chars, imperative:** "add CONDITION macro", not "added".
- **Body** explains the *why*; the *what* is in the diff.
- **Co-author trailer** per the convention in `CLAUDE.md` (current model).
- **No `--no-verify`** (hooks are load-bearing). **No `--amend` after push** —
  always new commits.

---

## Push discipline — DIRECTLY TO MAIN

Push to `origin/main` after every committed task. On non-fast-forward
(another worker pushed first):

```bash
git pull --rebase origin main      # rebase on top of theirs
git push origin main               # retry
```

Conflicts are unlikely (workers own different scopes). If a rebase conflict
takes > 10 min, `git rebase --abort`, stash, `git pull --ff-only`, re-read the
file the other worker changed, stash pop, resolve, commit, push.

**Never `git push --force` or `--force-with-lease` to any branch.** Fix
mistakes with a NEW commit; let the orchestrator clean up history.

---

## Completion ritual

When all tasks pass:

1. **Run the full suite once more:** `make test_parallel` + `make lint`. All
   green = ready.

   **⚠️ A GREEN SUITE IS NOT A HEALTHY NODE (RESILIENCE DOCTRINE #1).** If your
   change touches sync, validation, header/block admit, a cutover, or anything
   on the chain-advance path, live forward progress is a required gate:
   ```bash
   build/bin/zclassic23 agent
   build/bin/zclassic23 dumpstate chain_advance_coordinator
   build/bin/zclassic23 -bench-kill9       # benchmark rows
   ```
   If you can't show the live tip advancing past your change, it is NOT done —
   report it, don't ship it.

2. **Update the assignment doc:** set Status to
   `✅ DONE — pushed YYYY-MM-DD` (with commit sha), and append a `## Completion
   (wt<N>, YYYY-MM-DD)` section covering: summary, which TOP 10 BENCHMARK
   (REFACTOR_STATUS.md) it moved and by how much (or what it unblocks),
   commits, files added/modified, acceptance verification (incl. the live
   check), and any surprises/follow-ups.

3. **Push:** `git commit -m "wt<N>: complete <slug>"`, `git push origin main`.

4. **Report to the user:** completed `<slug>`, pushed to main.

The orchestrator does NOT merge — your commits are already on main. It only
writes assignments, updates `REFACTOR_STATUS.md`, and curates the pipeline.

---

## Memory discipline

After completing the assignment:

- **Save a project memory** if the work shipped a non-obvious primitive or surprising finding. Use `feedback_*` for guidance, `project_*` for things that shipped.
- **Do NOT save memory for routine completions** — the assignment doc + git log are the record.

---

## What to do if you're confused

In this priority order:

1. Re-read `docs/FRAMEWORK.md` for the architectural shape.
2. Re-read your assignment doc — it has a Tasks section in order.
3. Re-read `docs/REFACTOR_STATUS.md` to see where you fit in the bigger picture.
4. Check the existing codebase for examples (e.g., for Job shape, look at `app/jobs/src/header_admit_stage.c` — the canonical stage adopter).
5. If still stuck > 30 min: append `BLOCKED: <reason>` to your assignment, push, report to user. Do NOT guess and ship questionable code.

---

## Forbidden moves

- ❌ Creating new branches on `origin` (workers push to main only).
- ❌ **Pushing ANY non-main branch to `origin`**, even by accident. If your local checkout has stale feature branches from the old workflow, run `git branch -D <name>` to delete them locally before doing any `git push`. **NEVER `git push --all` or `git push --mirror`** — those push every local branch.
- ❌ Editing `docs/REFACTOR_STATUS.md` directly (orchestrator only — exception: workers may update the mega-module roster when they DELETE a module).
- ❌ Touching files outside your assignment's scope.
- ❌ `git push --force` / `--force-with-lease` to ANY branch.
- ❌ Deleting another worker's commits via rebase-and-overwrite.
- ❌ `--no-verify`, `--amend` after push, skipping tests "just this once".
- ❌ Editing CLAUDE.md without orchestrator sign-off (it's auto-loaded into every session).
- ❌ Writing code that doesn't match one of the 8 framework shapes.

---

## End-of-session

Whether you finished or not:

1. `git status` — verify clean (or known WIP).
2. `git push origin main` — back up.
3. Update assignment Status: `✅ DONE — pushed YYYY-MM-DD` if finished, else
   `IN PROGRESS (wt<N>) — paused at task <X>` (and `git push origin main`).
4. Report briefly to user.

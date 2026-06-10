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
   $ pwd                                  # e.g., /home/rhett/github/zclassic23-2
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

5. Verify assignment is unclaimed / in-progress for you
   - Check the "Status" section at the bottom of your assignment doc
   - If status is "DONE" → nothing to do; report to user
   - If status is "BLOCKED" or "FAILED" → re-read the issue, decide whether
     to retry or escalate
   - If status is "READY" or "IN PROGRESS (wt<N>)" → proceed

6. Stay on main
   - DO NOT branch off — workers commit directly to main.
   - If you need to experiment, use `git stash` or a LOCAL-ONLY scratch
     branch, but the work that ships goes onto main directly.
   - **IGNORE any `**Branch:**` field at the top of older assignment docs.**
     That was a legacy of the feature-branch workflow; the field is dead.
     You stay on `main` regardless of what the doc says.

7. Mark in-progress + push
   - Edit the assignment doc's Status section to "IN PROGRESS (wt<N>)"
   - $ git add docs/work/<file>
   - $ git commit -m "wt<N>: mark <slug> in progress"
   - $ git push origin main                   # see "Push discipline" below
```

After this, **execute the assignment's Tasks section in order**.

---

## Per-task discipline

Each Task in an assignment has:

- A **scope** — exact files to create/edit
- An **acceptance test** — concrete check that proves done

For each task:

1. `git pull --rebase origin main` to stay current.
2. Implement.
3. Run the acceptance test (`make test_parallel`, build, lint, etc.).
4. **If green:** `git add` the specific files, `git commit -m "<task description>"`.
5. **If red:** debug. Do NOT commit a broken state. If stuck > 30 min, append a `BLOCKED` note to the assignment.
6. `git push origin main` after EACH committed task (see "Push discipline").

---

## Commit discipline

- **Small commits.** Each task is its own commit. No 5-task megacommits.
- **Subject line < 70 chars.** Imperative mood: "add CONDITION macro", not "added".
- **Body** explains the *why*. The *what* is in the diff.
- **Co-author trailer:** every commit ends with
  ```
  Co-Authored-By: Claude Opus 4.7 (1M context) <noreply@anthropic.com>
  ```
- **No `--no-verify`.** Pre-commit hooks exist for a reason.
- **No `--amend`** after push. Always create new commits.

---

## Push discipline — DIRECTLY TO MAIN

Push directly to `origin/main` after **every committed task**.

```bash
git push origin main
```

If push fails due to non-fast-forward (another worker pushed first):

```bash
git pull --rebase origin main      # rebase your commits on top of theirs
# resolve any conflicts (probably none — workers own different scopes)
git push origin main               # retry
```

If the rebase has conflicts you can't resolve in <10 minutes:

```bash
git rebase --abort
git stash                          # save your work
git pull --ff-only origin main     # get the new state
# re-read the file you were touching; the other worker may have changed it
git stash pop                      # apply your work, resolve conflicts manually
# commit + push
```

**Never** `git push --force` to main. **Never** `git push --force-with-lease`
either. If you've made a mistake, write a NEW commit that fixes it; let the
orchestrator clean up history if needed.

---

## Completion ritual

When all tasks pass and acceptance criteria are met:

1. **Run the full test suite** one more time:
   ```bash
   make test_parallel    # fork-based runner, ~1-2 min
   make lint             # all 20+ gates
   ```
   All green = ready. Any red = back to debugging.

   **⚠️ A GREEN TEST SUITE IS NOT A HEALTHY NODE (RESILIENCE DOCTRINE #1).**
   On 2026-05-24 the C-3 header cutover (`ad34efb65`) shipped with
   `test_parallel` 0/196 green and **froze the live chain for the whole
   session** — the tip never advanced one block. Tests passed; the node was
   dead. So: **if your change touches sync, validation, header/block admit, a
   cutover, or anything on the chain-advance path, forward progress on the
   LIVE node is a required gate, not optional:**
   ```bash
   # the C health gate reads live chain_advance state:
   build/bin/zcl-rpc healthcheck | jq '.checks.chain_advance'
   # for benchmark rows:
   build/bin/zclassic23 -bench-kill9
   ```
   If you cannot show the live tip advancing past your change
   (e.g. past the cutover height), it is NOT done — report it, don't ship it.

2. **Update the assignment doc** — append a Completion section AND change Status:

   ```markdown
   ## Status

   **✅ DONE — pushed 2026-MM-DD** to main as commit `<sha>`.

   ## Completion (wt<N>, YYYY-MM-DD)

   ### Summary
   <1-3 sentence summary of what shipped>

   ### Benchmark moved
   <which of the TOP 10 BENCHMARKS (REFACTOR_STATUS.md) this advances, and by
    how much if measurable — e.g. "#2 warm restart 33s→29s" or "#3 RSS: removed
    a 1,410-LOC module, expect ~80MB". If purely enabling, say which it unblocks.>

   ### Commits
   - <sha> <subject>
   - <sha> <subject>
   - ...

   ### Files added/modified
   - app/conditions/block_failed_mask_at_tip.c   (NEW, 48 LOC)
   - lib/framework/condition.h                    (NEW, 102 LOC)
   - ...

   ### Acceptance verification
   - [x] Test 1: <description> — PASS
   - [x] Test 2: <description> — PASS
   - [x] Live check: <description> — PASS

   ### Surprises / follow-ups
   <anything orchestrator should know — found bug elsewhere, scope change, etc.>
   ```

3. **Push the completion update**:
   ```bash
   git add docs/work/<file>
   git commit -m "wt<N>: complete <slug>"
   git push origin main
   ```

4. **Report to the user**:
   > Completed assignment `<slug>`. Pushed directly to main.

5. **The orchestrator does NOT need to merge anything** — your commits are
   already on main. The orchestrator only writes new assignments, updates
   `REFACTOR_STATUS.md`, and curates the work pipeline.

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
4. Check the existing codebase for examples (e.g., for Job shape, look at `app/services/src/header_admit_stage.c` — the canonical stage adopter).
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
3. If finished: update assignment Status to `✅ DONE — pushed YYYY-MM-DD`.
4. If not finished: update assignment Status to `IN PROGRESS (wt<N>) — paused at task <X>` and `git push origin main`.
5. Report briefly to user.

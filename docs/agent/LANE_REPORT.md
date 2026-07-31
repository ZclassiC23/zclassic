# LANE_REPORT — the required shape of an executor's report back

    VERSION: v1

The report **is** the lane's return value: the orchestrator merges, rejects or
re-dispatches on it alone. Write it as your final message, not to a file — a file
the orchestrator has to go find is not a report.

Rules that make a report usable:

* **Absolute paths, always.** A relative path is ambiguous across 20+ worktrees.
* **Quote verdicts literally.** Paraphrase is indistinguishable from invention.
  See [`LANE_CONTRACT.md`](LANE_CONTRACT.md) §E for the exact line to quote per
  gate.
* **A gate you did not run is reported as not run**, with the reason. "I could
  not do X" is worth more than a claim that does not hold.
* **No prose padding.** Every section is a list or a table.

---

## The nine sections, in order

### 1. Capability landed
One or two sentences: what an operator or the orchestrator can now do that they
could not before. Not "added three files" — that is section 2.

### 2. What changed
Table: absolute path | added / modified / **deleted** | one-line why.
Include the branch name and head SHA (`git rev-parse HEAD`), and the baseline
commit the lane forked from.

### 3. What was deleted
Called out separately from section 2, because deletion is the preferred outcome
([`LANE_CONTRACT.md`](LANE_CONTRACT.md) §B6) and it is the one change class that
silently breaks callers. For each deletion: what it was, what now covers it, and
how you know nothing else referenced it (`zclassic23 code refs`, `git grep`).
"Nothing deleted" is a valid answer — say it.

### 4. What could not be done, and why
Every task item you did not complete, each with the blocker and what you tried.
An item silently dropped is the most expensive line in a report, because the
orchestrator merges believing it landed.

### 5. Gates — every one, with its literal verdict
Table: gate command | ran? | **the literal terminal line** | machine receipt.

| gate | ran | literal verdict line | receipt |
|---|---|---|---|
| `make lint` | yes | `── lint timing: N gates, wall … ──` | `.cache/lint-timing/last-run.json` `"failed_count":0` |
| `make t-fast ONLY=<group>` | yes | `SUITE VERDICT … groups_ran=N groups_failed=0` + `ALL TESTS PASSED` | `.cache/test-timing/last-run.json` |
| `make build-only` | no | — | not run: another lane held the build; nothing compiled in this lane |

Report `groups_ran`, not just the pass token: a cached or zero-group run is the
known false green ([`LANE_CONTRACT.md`](LANE_CONTRACT.md) §A5).

### 6. Call sites outside the lane that now need updating
Everything you found that must change but was **not yours to touch** — another
lane's file, a doc index, a Makefile target, a caller in `lib/`. For each: the
absolute path, what needs to change, and why you did not change it. This is how
disjoint file ownership stays cheap instead of becoming a merge conflict.

### 7. Where the contract was wrong — **the section that matters most**
A lane that silently follows a wrong contract recreates the defect the contract
exists to delete, and the next lane pays again. For each clause of
[`LANE_CONTRACT.md`](LANE_CONTRACT.md) (or of the task prompt) that turned out to
be wrong, unfollowable, or stale:

* clause id (e.g. `A2`, or "prompt: house facts");
* what it says;
* what the tree actually does, with the evidence (command + output, or
  `path:line`);
* what you did instead.

"Nothing was wrong" is an acceptable answer **only if you looked**. If you never
verified a clause you relied on, say that instead.

### 8. Velocity numbers
Cheap to produce, and the owner asks for them per lane. Derive, do not estimate:

| number | derivation |
|---|---|
| unique authored lines | `git diff --shortstat <baseline>..HEAD` |
| files touched | `git diff --name-only <baseline>..HEAD \| wc -l` |
| commits | `git rev-list --count <baseline>..HEAD` |
| assignment-to-green iterations | how many times you ran the gates before they passed |
| focused test wall time | the runner's own timing line |

State plainly which numbers you could not derive.

### 9. Handoff line
One line the orchestrator can act on:
`branch <lane/slug> at <sha>, gates <green|red|partial>, ready for merge | needs <X>`.

---

## Anti-patterns that get a report rejected

* A gate verdict with no literal line quoted.
* "All tests pass" with no `groups_ran`.
* A relative path.
* An empty section 7 in a lane that clearly hit friction.
* A completed-work claim for something in section 4.
* Deleting a file and not saying so.

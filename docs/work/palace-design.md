# The Palace — a legibility + invariant layer for fast LLM development

> **One binary. One chain. One way to do each thing — and a future LLM can
> learn WHERE a thing lives, WHAT it does, and WHAT it breaks in near-zero
> tokens, because the layout is indexed and the beauty is lint-enforced.**

Status: **design proposal (Phase 4).** Read-only. This proposes a legibility
layer *on top of* what already exists — it rebuilds nothing and moves (almost)
nothing. Cites are `file:line`, verified by reading on 2026-07-12.

## 0. The problem, in information-theoretic terms

An LLM working in this tree should spend near-zero tokens to know:

- **(a) WHERE** any concept lives — *location predicts content*.
- **(b) WHAT** a file/module does without opening it — *content self-describes*.
- **(c) WHAT breaks** if it changes — *impact is a lookup, not a guess*.

Three of the four properties are **already built and enforced**; one is stubbed:

| Property | Mechanism that already exists | Gap |
|---|---|---|
| Location predicts content | 8 shape folders, lint-enforced by `tools/lint/framework_shape_check.sh` (#18) + `tools/lint/check_framework_filename_suffix.sh` (#22); shapes canonical in `lib/codeindex/src/codeindex_group.c:31` (`k_app_shapes[]`) | — solid |
| Navigation is O(1) indexed | `lib/codeindex/` SQLite index + the `code` branch (`config/commands/code.def`, `tools/command/native_code_command.c`) answer where-is/what-calls/what's-in-file without grep | — solid |
| Impact is a lookup | `app/controllers/src/agent_impact_rules.c:86` (`agent_impact_apply_shared_rules`) maps a changed path → focused test groups, shared by native `agentimpact` and `make fast-ci` | — solid |
| **Content self-describes** | the store already has a `purpose` column (`codeindex_store.c:99`, `files` table) and `code file`/`code group` already render `finfo.purpose` | **STUBBED — written empty** at `lib/codeindex/src/codeindex_build.c:288` (`f.purpose[0] = '\0'; /* per-file purpose is a later-lane enrichment */`) |

**The palace is finishing the fourth property and locking all four with
gates so they cannot decay.** It is not a rebuild and not (materially) a
file-move. Sections 1–5 are the proposal.

Grounded in and building ON (never duplicating):
- `docs/FRAMEWORK.md` — the Prime Directive, the Ten Laws of Beauty, the eight
  shapes and their WARN→RATCHET→HARD gate ladder (`FRAMEWORK.md` §5).
- `docs/CODEBASE_MAP.md` — where-things-live + how-to-do-each-thing.
- `docs/REFACTOR_STATUS.md` — the architecture debt board (~90% done, off the
  v1 path). The palace adds *legibility*, not architecture debt.
- Phase-1 in flight (assumed landed): `code map` (grouped floor plan with file
  counts), `code tests <path>` (file → focused test groups via the impact
  resolver), and counts on `code group`.

The four category namespaces are today **disconnected**: directory-groups
(codeindex), the 8 shapes, ~580 test groups (`g_groups[]` in
`lib/test/src/test_parallel.c:333`, built from `TEST_LIST`/`SPEC_LIST` X-macros),
and command branches/layers (`config/commands/*.def` — 6 files: apps, code,
core, dev, ops, root). The only cross-links are the impact rules
(`agent_impact_rules.def`) + a few parity tests. The palace **joins** them with
one composed CLI call — it does **not** add a fifth namespace.

---

## 1. Self-description layer — populate `ci_file.purpose`

### 1.1 Source of truth: the existing top-of-file block comment

Do **not** invent a new mandatory `/* purpose: ... */` header for ~1,600 files
(that is a big-bang cleanup, forbidden by §4). The tree **already** carries the
convention. Verified samples:

- `lib/codeindex/src/codeindex_group.c:4` — `codeindex_group — the group hierarchy: how a repo-relative path maps to a navigator group...`
- `app/services/src/replay_verify_service.c:3` — `replay_verify_service — implementation. See services/replay_verify_service.h.`
- `lib/net/src/download.c:3` — `Block download manager — coordinates parallel block downloads.`
- `app/models/src/block.c:3` — `ActiveRecord model: Block`

The pattern is: line 1 = Copyright/license, line 2 = blank `*` fill, line 3 =
the **purpose line**, usually `<stem> — <one-line purpose>` (em-dash), sometimes
`<stem>: <purpose>` or a bare sentence.

**Canonical rule (the one obvious place):** *a file's purpose is the first
substantive line of its top-of-file block comment.* An optional leading
`<stem> — ` / `<stem>: ` / `<stem> - ` prefix is stripped so the stored purpose
is the bare description. This is derive-from-existing, not a new required tag —
which means it lands with a **shrink-only baseline** (§3), not a rewrite.

Escape hatch for a file whose leading comment is genuinely not a purpose (rare):
allow an explicit `/* purpose: ... */` first line to override the derivation.
This mirrors the `// suffix-ok:<tag>` override already used by
`check_framework_filename_suffix.sh`.

### 1.2 The exact scan/build change surface (two files, ~40 LOC)

Everything downstream already works — the store column exists
(`codeindex_store.c:99`), `code file` already prints `finfo.purpose`
(`native_code_command.c:261`), and `code group` already prints `files[i].purpose`
(`native_code_command.c:195`). Only the *producer* is stubbed. No schema
migration; no new query; no new command.

**`lib/codeindex/src/codeindex_scan.c`** — the scanner already blanks comments
and captures each doc block's first line (`capture_doc`, lines 125–168;
`c->comments[]`). Add one bounded extractor and one out-param:

- New static helper `ci_file_purpose(const struct scan_ctx *c, char out[160])`:
  find the first block comment whose `start_off` precedes the first code token,
  walk its body lines, skip the Copyright/license line and blank `*` fill, take
  the first substantive line, strip a leading `<stem> [—:-] ` prefix, and copy
  ≤159 bytes. Reuse the existing comment offsets in `c->comments[]` — no second
  parse pass.
- Thread it out: `ci_scan_text(...)` gains a `char purpose_out[160]` param (or
  a `ci_file_meta *` if a cleaner signature is preferred); `ci_scan_file`
  (declared `codeindex_priv.h:97`) forwards it. Purely additive.

**`lib/codeindex/src/codeindex_build.c`** — at the stub (line 288), replace
`f.purpose[0] = '\0';` with the value returned from `ci_scan_file`:

```c
uint8_t sha[32];
char purpose[160] = "";
if (!ci_scan_file(env->root, relpath, on_sym_cb, on_ref_cb, b, sha, purpose)) { ... }
...
snprintf(f.purpose, sizeof(f.purpose), "%s", purpose);   /* was: f.purpose[0] = '\0' */
```

`ci_store_put_file` already binds `f->purpose` (`codeindex_store.c:268`). Done.
`code file X` and `code map` now tell you what everything **is** without opening
it. `test_codeindex` gets one fixture case: a file with a known header yields the
expected stored purpose; a file with `/* purpose: ... */` override yields the
override; a header-less file yields `""`.

Cost model unchanged: purpose is derived during the existing single scan pass,
so `codeindex_rebuild` stays O(source bytes) and the staleness stamp
(`codeindex_source_root_sha3`, `codeindex_build.c:180`) is untouched.

---

## 2. Unified namespace view — one call for "everything about this room"

Reuse, don't rebuild. The four namespaces are already individually queryable;
the palace adds one **composing** leaf that joins them for a single path. No
fifth namespace, no new index.

**`code room <path>`** (new leaf in `config/commands/code.def` +
`zcl_native_handle_code_room` in `native_code_command.c`) answers, in one
bounded JSON document (same 4096-byte budget as the other `code` leaves):

| Field | Namespace | Reused source (already exists) |
|---|---|---|
| `shape` | the 8 shapes | derive from `codeindex_file()` → `finfo.group`; if it begins `app/` the second component is the shape (`k_app_shapes[]`, `codeindex_group.c:31`) |
| `purpose` | self-description | `finfo.purpose` (§1) |
| `group` + `neighbors` | directory-groups | `codeindex_file()` for the group; `codeindex_files_in_group()` for siblings (both already in `codeindex.h`) |
| `tests[]` | ~580 test groups | `agent_impact_apply_shared_rules(path, acc)` (`agent_impact_rules.c:86`) — the same resolver Phase-1 `code tests` uses |
| `commands[]` | command branches | see 2.1 — a one-token macro add resolves owning command(s) |

So `code room app/jobs/src/utxo_apply_stage.c` returns: *shape = jobs; purpose =
"…"; group = app/jobs with N sibling stages; tests = the reducer groups the
impact rules route here; commands = (any command whose handler is defined in this
file).* One call, all four namespaces, no grep, no file read.

`code map` (Phase-1) remains the **floor plan** (grouped/categorized counts);
`code room` is the **single-room detail** that stitches the four namespaces for
one path. They compose: `code map` → pick a group → `code group <g>` → pick a
file → `code room <file>`.

### 2.1 The command → file join (minimal, honest)

The command registry holds handler *function pointers*, not symbol names, so
today you cannot cheaply ask "which command does this file back?". The
reuse-first fix is **one token** in the command X-macro: stringize the handler
(`#handler`) so each registry row also carries the handler's *symbol name* at
zero runtime cost (compile-time only). Then `code room` resolves owning
command(s) by: for each command row, `codeindex_symbol(handler_name)` →
`def_path`; if `def_path == path`, that command belongs to this file. This
reuses `codeindex_symbol` (already indexed) — no new table.

If the `#handler` stringize is deemed out of scope, `code room` still returns
the other three namespaces (shape, neighbors, tests) fully; `commands[]` degrades
to `null` with a stated reason, never a wrong guess (Law 7 honesty). The command
join is the one genuinely weak cross-link today and is explicitly optional.

---

## 3. Decay-proof invariants — the lint gates that keep the palace beautiful

Modeled exactly on the existing shape gates: `ZCL_LINT_MODE` selects
**WARN → RATCHET → FAIL(HARD)**, RATCHET compares against a **shrink-only
baseline/allowlist** so a gate lands *without* a big-bang cleanup
(`framework_shape_check.sh:4`, `FRAMEWORK.md` §5). `make ci` runs lint before a
single test, so a violation never reaches a human (`FRAMEWORK.md:296`). Each gate
is itself planted-and-asserted in `test_make_lint_gates` (per `FRAMEWORK.md:311`).

### Gate P1 — `check-file-purpose` (the keystone; property (b))

- **Checks:** every indexed `.c`/`.h` under the codeindex roots (the
  `lib/<mod>`, `app/<shape>`, `core`, `config`, `domain`, `adapters`, `tools`
  set enumerated in `ci_enumerate_sources`, `codeindex_build.c:111`) yields a
  non-empty derivable purpose (§1.1) — i.e. a substantive top-of-file comment
  line, or an explicit `/* purpose: ... */` override.
- **Why it can't decay:** a new file with no purpose line fails the gate; the
  index can never regress to blank `purpose` for a fresh file.
- **Rollout:** ship **WARN** (measure the tree). Write current purpose-less
  files to `file_purpose_baseline.txt`. Flip to **RATCHET** (shrink-only:
  baseline files tolerated, any *new* purpose-less file fails). As agents touch
  files they add the one line; baseline monotonically shrinks. Graduate to
  **HARD/FAIL** when the baseline is empty.
- **Baseline approach:** shrink-only, identical to
  `framework_shape_allowlist.txt`. No cleanup blocks the landing.

### Gate P2 — `check-group-purpose` (property (b) for modules/dirs)

- **Checks:** every group node emitted by `ci_group_emit_all`
  (`codeindex_group.c:128`) has a non-empty purpose. Today `ci_group_purpose`
  (`codeindex_group.c:91`) covers only the well-known top groups and the 8
  `app/<shape>` folders; every `lib/<mod>` (35 modules, `k_lib_modules[]`,
  `codeindex_group.c:23`) and every `domain/<ctx>` falls through to `""`.
- **Why it can't decay:** adding a new `lib/` module (which means editing
  `k_lib_modules[]` — a parity-tested list, `codeindex_group.c:8`) without a
  one-line purpose fails the gate. "Every module has a one-line description"
  becomes a build law.
- **Rollout:** the group list is **finite and small** (~35 + fixed roots), so
  this can be filled in one bounded PR and land **HARD quickly** — no long
  ratchet. WARN for one cycle to confirm zero-broadening, then FAIL.
- **Baseline approach:** ideally none (fill all at once); if any lag, a
  shrink-only `group_purpose_baseline.txt`.

### Gate P3 — `check-no-orphan-placement` (property (a): exactly one obvious place)

- **Checks:** no *new* source file lands in the catch-all `root` group
  (`ci_group_for_path` returns `"root"` for a top-level file it can't place,
  `codeindex_group.c:86`). Every file must resolve to a known
  `lib/<mod>`/`app/<shape>`/`core`/`config`/`domain`/`adapters`/`ports`/`tools`
  group. This is the complement of the shape gate (#18 covers `app/`; P3 covers
  the whole tree) and operationalizes "exactly one obvious place for each
  concept."
- **Why it can't decay:** a file dropped in an un-grouped location (or a new
  top-level dir) is caught before merge.
- **Rollout:** WARN → RATCHET with a shrink-only `orphan_placement_baseline.txt`
  seeded from today's `root`-group files → HARD when empty.

**Top 3, ranked:** **P1 (file-purpose)** is the keystone — it is the mechanism
that makes content self-describe and is what unblocks `code map`/`code file`
from being informative. **P2 (group-purpose)** is the cheapest HARD win (finite
list) and guarantees every module is legible. **P3 (orphan-placement)** locks
location-predicts-content for the whole tree, not just `app/`. All three copy the
proven shape-gate ladder verbatim, so none blocks on a cleanup.

*(Deliberately NOT proposed: a filename↔purpose consistency gate, or a
purpose-uniqueness gate. Both are gilding — they add gate surface without closing
a real legibility hole, and Law 8 says DRY the knowledge, not add indirection.)*

---

## 4. Physical-reorg verdict — **against a big-bang move; the palace is a layer**

**Recommendation: do not move files.** The palace is a *legibility + invariant
layer*, not a file-move. Reasons, honestly:

- The location structure is **already ~90% done and enforced** — 8 shapes,
  filename-suffix gate, the whole `app/` tree conformant
  (`REFACTOR_STATUS.md`, "the `app/` layer is already conformant"). The
  remaining structural debt is tracked there and is **off the v1 path**.
- A tree-wide move is **high-risk**: it churns includes/depfiles, risks the
  consensus boot path, and demands copy-prove for anything it touches — against
  the copy-prove and consensus-parity doctrine for near-zero legibility gain that
  §§1–3 deliver without moving a byte.
- The self-description layer makes moves **unnecessary**: once `ci_file.purpose`
  and `code room` exist, an LLM navigates by index, so the *physical* directory
  no longer has to carry the whole meaning.

**Surgical candidates considered and declined:** the large boot monoliths
(`config/src/boot.c` at 3949 lines, `boot_refold_staged.c` at 2107 —
`REFACTOR_STATUS.md` addendum) are legibility warts, but splitting them touches
the consensus boot/refold path and is exactly the copy-prove, owner-gated,
off-queue work the palace is designed to *avoid*. They are already visible via
the file-size ceiling gate and its shrink-only baseline; the palace does not
accelerate them.

**No file or directory move is warranted for the palace.** If a future split of a
boot monolith happens, it happens on the architecture axis under copy-prove — not
as part of this legibility layer.

---

## 5. Sequencing — Phase 4, each step independently landable

Built on the Phase 1–3 fast loop (`code map`/`code tests`/counts). Every phase
ships behind the WARN→RATCHET ladder so **nothing blocks on a giant cleanup**.

- **P4.0 — self-description data (§1).** Scanner extractor + build populate +
  `test_codeindex` fixture case. Lands alone: `code file`/`code map` immediately
  show purpose. No gate yet. *(2 files, ~40 LOC.)*
- **P4.1 — `check-file-purpose` gate P1, WARN.** Measure the tree; write
  `file_purpose_baseline.txt`. Plant the fixture in `test_make_lint_gates`.
  Independent of P4.0's data landing cleanly.
- **P4.2 — group purpose + gate P2.** Fill `ci_group_purpose` for all
  `lib/<mod>` + `domain/<ctx>`; land P2 **HARD** (finite list). Fully
  independent.
- **P4.3 — unified view `code room` (§2).** Compose shape + neighbors + tests;
  add the optional `#handler` stringize for the command join. Independent —
  purely additive command leaf.
- **P4.4 — ratchet the invariants.** Flip P1 WARN→RATCHET; add P3
  (`check-no-orphan-placement`) WARN→RATCHET with seeded shrink-only baselines.
- **P4.5 — harden.** As each baseline reaches empty (agents add the one-line
  purpose during ordinary edits), graduate P1/P3 to HARD/FAIL. No dedicated
  cleanup sprint — the ratchet does the work incrementally.

Ordering constraint: only P4.1 depends on P4.0 (the gate needs the extractor's
definition of "a valid purpose"). P4.2, P4.3, P4.4-P3 are mutually independent
and can land in any order or in parallel worktrees on disjoint files
(`codeindex_group.c` vs `native_code_command.c`+`code.def` vs
`tools/lint/`).

---

## Appendix — the exact change surface (for the implementer)

| Phase | File | Change |
|---|---|---|
| P4.0 | `lib/codeindex/src/codeindex_scan.c` | add `ci_file_purpose()` extractor; add `purpose_out[160]` to `ci_scan_text`/`ci_scan_file` |
| P4.0 | `lib/codeindex/src/codeindex_build.c:288` | replace `f.purpose[0]='\0'` with scanned purpose |
| P4.0 | `lib/codeindex/src/codeindex_priv.h:97` | extend `ci_scan_file` prototype |
| P4.0 | `lib/test/src/test_codeindex.c` | fixture: header→purpose, override, header-less→"" |
| P4.1/4.4 | `tools/lint/check_file_purpose.sh` (new) + `Makefile` `lint` dep + `file_purpose_baseline.txt` | shrink-only gate, WARN→RATCHET→HARD |
| P4.2 | `lib/codeindex/src/codeindex_group.c:91` | fill `ci_group_purpose` for all `lib/<mod>` + `domain/<ctx>` |
| P4.2 | `tools/lint/check_group_purpose.sh` (new) + `Makefile` | HARD gate over the finite group list |
| P4.3 | `config/commands/code.def` + `tools/command/native_code_command.c` | `code room` leaf composing the four namespaces |
| P4.3 | command X-macro (in `kernel/command_registry.h` / the `ZCL_COMMAND_READY_READ` expansion) | optional `#handler` stringize for the command→file join |
| P4.4 | `tools/lint/check_no_orphan_placement.sh` (new) + `orphan_placement_baseline.txt` | shrink-only gate over `ci_group_for_path` == "root" |

Nothing here changes consensus, the reducer, the fact log, or any runtime path.
Everything is derived, read-only, and recomputed-never-repaired — the same
posture as `lib/codeindex/` and `lib/vcs/` already have.

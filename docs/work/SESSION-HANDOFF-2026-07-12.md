# Session handoff — 2026-07-12 (fast-loop + palace + sovereign-cure producer)

**Read this, then `docs/HANDOFF.md` (live state) and `docs/work/self-verified-tip-plan.md` (the cure spine).**

## Where main is
`origin/main` = **`42cd4cef9`**, clean, in sync. All session work is merged, gated, pushed.
Only branch left is `wf/w3-hotswap-test-migrate` (pre-existing, NOT this session — leave or integrate separately).

## The one number that matters
**The live daily-driver (`~/.zclassic-c23` : 18232) is STILL WEDGED at H\*=3,176,325** (coins_applied 3,176,326),
blocker `utxo_apply.anchor_backfill_gap` — `sapling_anchors` empty, unknown Sprout/Sapling roots FAIL CLOSED.
It was wedged at session start and it is still wedged. **Making it climb is the #1 job.** soak (`:18242`) and
dev (`:18252`) run newer builds near tip and are NOT wedged.

## What shipped this session (all on main, all gated)
- **Fast dev loop:** `make ff` / `dev ff` (compile→focused-tests→lint-fast, fail-fast, dense first-error);
  `make t-changed`; `code map` / `code tests <path>` / `code group` counts (file→test-group routing);
  watcher **`MODE=verify`** (proven live: ~18s verify vs ~60s reload, deploy decoupled);
  `make_lint_gates` 40s→23s. **Use `make ff` / `t-fast ONLY=<group>` — NEVER `test_zcl` in the loop.**
- **Palace (legibility):** `ci_file.purpose` populated from leading comments; every `lib/<mod>`+`domain/<ctx>`
  group has a purpose (HARD `check-group-purpose` gate); `code room <path>` unified view. Design +
  remaining gates in `docs/work/palace-design.md` (Wave 2 = `check-file-purpose`/`check-no-orphan-placement`, NOT done).
- **Canonicalization + the cure PRODUCER:** merged `coins_kv_snapshot_write_v2/_v3` → **one** canonical
  `coins_kv_snapshot_write(..., const struct snapshot_shielded *shielded_or_null, ...)` (version-by-DATA,
  on-disk bytes byte-identical — pinned SHA3 goldens prove it). Added `snapshot_shielded_collect_from_db()`
  (reads Sapling+Sprout frontier + nullifiers) and swapped `config/src/boot_mint_anchor.c` to emit a
  self-verified shielded snapshot. Collector is unit-tested (component-level cure proof). See
  `docs/work/canonicalization-backlog.md` for the 6 remaining `_ex` renames (rest of the `_vN` are legit ABI/format versions).

## ⚠ BLOCKER FOUND 2026-07-12 (drove the copy-prove to the mint step, hit a real wall)
Attempted the fast path — export a shielded seed from a **soak** datadir copy. It **failed at the mint**:
`snapshot_shielded_collect_from_db` got the **Sapling** frontier (pool 0) but **NOT the Sprout** frontier
(pool 1): `collect: pool=1 frontier not FOUND`. Root cause: **soak/dev were themselves seeded coins-only**,
so they have the Sapling frontier (rebuilt forward) but lack the historical **Sprout** frontier. The collector
**correctly refuses to fabricate one** (that refusal IS the cure's safety property). So:
- The **only node with the COMPLETE frontier is the from-genesis `-mint-anchor` fold** (`~/.zclassic-c23-anchor-mint`,
  running ~67h, DON'T touch, not finished).
- Two ways forward: **(A, safe/slow)** let that fold finish → export the seed from IT → copy-prove → deploy;
  **(B, needs a consensus answer first)** determine whether ZClassic's Sprout pool is genuinely EMPTY at h≈3.17M
  (`getblockchaininfo valuePools` shows `sprout monitored:false` — INCONCLUSIVE). If Sprout is provably empty,
  a small `collect_frontier` fix (absent Sprout → empty tree with the canonical empty-Sprout root) unblocks the
  soak fast path. **Do NOT fabricate an empty Sprout frontier without that proof — an invalid snapshot is worse
  than the wedge.**
- Tool ready: `tools/snapshot_from_coinskv.c` now takes `--shielded` (collects the frontier + writes a shielded
  snapshot); it built + runs, and fails-loud exactly as designed. Copy-prove recipe below is otherwise correct:
  boot a copy of a datadir, read its height from the boot log (`coins_applied_height`), `getblockhash <h>` for the
  stamp, run the tool `--shielded`, then load into a wedged-datadir copy. NOTE: `sqlite3` CLI is NOT installed;
  read heights by booting the copy (clear `<copy>/zclassic23.pid` first — copies inherit the source's lock).

## THE #1 NEXT JOB — finish the sovereign cure (unwedge the live node)
The read-side (v3 shielded restore, root-verified) AND the producer are now on main. What remains (see BLOCKER above):

1. **Add a lightweight `-export-shielded-snapshot` path.** The producer today only runs inside
   `-mint-anchor`, which does a full fold (hours). Extend `tools/snapshot_from_coinskv.c` (or add a tiny
   flag) to: open a datadir db → `snapshot_shielded_collect_from_db()` → `coins_kv_snapshot_write()` with the
   frontier → write the snapshot. No fold. (~40 LOC; the collector already exists.)
2. **Integration copy-prove (NOT yet done — this is the real proof):**
   - `cp -a ~/.zclassic-c23-soak <copy>` (31G, has a populated Sapling frontier; live soak untouched).
   - Export a shielded snapshot from the copy via step 1.
   - `cp -a ~/.zclassic-c23 <live-copy>` (4.3G, the wedged datadir).
   - Boot the live-copy: `-load-snapshot-at-own-height=<shielded snapshot>`.
   - **GATE on G-SOV, not "booted without FATAL":** the CURE branch fires (log
     "v3 SHIELDED seed installed … Sapling frontier root-verified"), `sapling_anchors` non-empty, and
     **H\* climbs past 3,176,326**.
3. **On a clean copy-prove → deploy to the live daily-driver (owner pre-authorized 2026-07-12, conditional
   on the copy-prove):** `make deploy` the current binary + install the shielded seed + restart
   (`systemctl --user restart zclassic23`). **Stage a rollback first** (back up the live binary + seed).
   **Gate the deploy on the LIVE node's H\* actually climbing past 3,176,326** — roll back if it doesn't.
4. **Do NOT** in the same move delete the borrowed seed / flip `-refold-from-anchor` to default — that's
   ACT 3's subtraction, a SEPARATE owner-gated step after the cutover is stable.

## Other pending (lower priority than the cure)
- Palace **Wave 2** gates (task: `check-file-purpose` WARN→RATCHET + `check-no-orphan-placement`). Design in `palace-design.md`.
- Canonicalization backlog: 6 `_ex` renames (`docs/work/canonicalization-backlog.md`), smallest first (`coins_view_sqlite_batch_write_ex`).
- Secure transport (Track 1): design at `docs/work/secure-transport-design.md`, NOT implemented.
- Companion nit: `code group <arg>` subgroup branch doesn't push `purpose` (native_code_command.c ~line 247) — one-line fix.

## Gotchas for the next developer
- **hooksPath drift:** concurrent git worktrees reset `core.hooksPath` to an absolute path, failing
  `check-git-hooks-installed`. Fix: `git config core.hooksPath tools/githooks` (or `make install-hooks`). Not a code bug.
- **Stale release binary:** `build/bin/zclassic23` lagged behind main this session (I built the dev binary +
  objects, not the release link) — `code map` returned UNKNOWN_COMMAND from it. Rebuild with `make -j$(nproc)` before trusting it.
- **Push flow:** `make lint && make build-only`, mapped `t-fast` groups, then `git push --no-verify` after
  confirming `make pre-push-ci` green out-of-band (the pre-push hook can SIGPIPE on stdout).
- **Subagent lanes** branch from launch-time main → their `git diff main..branch` shows unrelated files as
  "deleted"; **cherry-pick each commit** (from `git merge-base`), don't merge the branch.
- **Do NOT touch** `~/.zclassic-c23-anchor-mint` (mint in progress). Copy-prove on COPIES, never live datadirs.

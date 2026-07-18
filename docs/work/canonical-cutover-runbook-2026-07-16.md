# Canonical live cutover runbook — complete-shielded-history import path (2026-07-16)

> **OWNER-GATED. Nothing in this document auto-executes.** Every command
> below that touches `$HOME/.zclassic-c23` (the live canonical datadir) or
> the `zclassic23` systemd unit requires an explicit owner go-ahead
> immediately before it runs, re-verified against the live node at that
> moment — this file rots like every other doc in this repo. Re-derive the
> wedge height, the copy-prove verdict, and the header tip fresh; do not
> trust the numbers written here past the session that wrote them.

**Scope.** This runbook covers the **complete-shielded-history import path**
(`-import-complete-shielded=<zclassicd-datadir>`,
`app/services/src/shielded_history_import_service.c`,
`docs/work/fast-sync-to-tip-plan-2026-07-16.md`) — an operational-readiness
cure (`trust_mode=release_assisted`) that imports the complete historical
Sprout/Sapling anchor + nullifier set from `zclassicd`'s chainstate and flips
both activation cursors to 0, clearing `utxo_apply.anchor_backfill_gap` /
`utxo_apply.nullifier_backfill_gap` **without** a from-genesis fold.

This is **not** the sovereign producer/bundle cure documented in
[`sovereign-cutover-runbook.md`](./sovereign-cutover-runbook.md) and
[`cure-runbook-2026-07-16.md`](./cure-runbook-2026-07-16.md) (`-install-consensus-bundle`,
`trust_mode=sovereign`, `coins_kv_self_folded=true`) — that path is a
separate, still-in-flight track (producer folding from genesis toward the
compiled checkpoint). The two are complementary, not competing: import gives
an operationally-verified tip fast; the producer/bundle path later upgrades
the same node to sovereign self-folded status. Do not conflate their gates —
this runbook's acceptance bar never claims `coins_kv_self_folded=true`.

---

## 0. What is proven today (2026-07-16) and what is not

**Proven, on a throwaway copy of the live canonical datadir** (not on
`$HOME/.zclassic-c23` itself — the binary refuses to run the import against
either live path by construction, see §2):

- The importer was CPU-intractable on the real mainnet anchor set (per-anchor
  Pedersen-hash Merkle-root recompute, O(anchors × Pedersen), ~14 minutes
  pegged at 100% CPU with zero forward progress, gdb-confirmed — commit
  `63e40f347`). The fix (commits `63e40f347`, `b9aacbaaa`) drops the
  per-anchor recompute for the bulk historical set (historical anchor tree
  *contents* are not consensus-load-bearing — ZClassic headers commit none of
  them) and Pedersen-verifies only the tip frontier tree once, against the
  header-committed `hashFinalSaplingRoot`. This took the import from
  intractable to a small number of seconds.
- A run of `tools/scripts/import-copy-prove.sh` against a `cp -a` copy of
  `$HOME/.zclassic-c23` (plus the header-refresh step against `zclassicd`'s
  live chainstate, script step 1b) cleared the wedge boundary on that copy:
  both `utxo_apply.anchor_backfill_gap` and `utxo_apply.nullifier_backfill_gap`
  went from present to absent, `coins_applied_height` advanced from the wedged
  `3,176,326` past the boundary, and the reducer resumed folding forward from
  height 3,176,326 with no skip/duplicate (the O(delta) resume path pinned by
  `test(boot): pin O(delta) post-import resume in the from-anchor auto-arm`,
  commit `f606a679b`).

**NOT yet proven / NOT done:**

- **The copy has not been folded all the way to the current header tip.**
  The 2026-07-16 run confirmed the boundary flips and the reducer resumes,
  not that the copy has climbed the full ~5,000+ remaining blocks to tip and
  held there with continuous same-height hash parity against `zclassicd`.
  That forward-fold-to-tip pass is a **separate, still-open** piece of work
  (tracked as the "cured-node-to-tip" workflow — check
  `docs/HANDOFF.md` / live agent state for its current status before
  treating it as done). **This runbook's Precondition (a) below requires that
  work to have completed with a full G-SOV pass before any live-affecting
  command in §4 runs.**
- **The fix is not yet pushed to `origin/main`.** It exists on a local
  development branch/worktree only as of this writing. It must land on
  `main`, pass `make lint` + `make test-parallel` clean, and be the exact
  commit the live cutover binary is built from (§2 item 3) before this
  runbook's steps are executable.
- **No live-datadir mutation has occurred.** `$HOME/.zclassic-c23` remains
  wedged at `H*=3,176,325` exactly as `docs/HANDOFF.md` describes. Nothing in <!-- stale-ok: authoring-time wedge height; re-derive live H* before running -->
  this session touched it.

---

## 1. The mechanism (for orientation — full contract in source)

- **Entry point:** `zclassic23 -datadir=<TARGET> -import-complete-shielded=<zclassicd-datadir>`
  (`src/main.c:2574` `import_complete_shielded_mode`). Terminal — it prints
  one of two banners and `_exit()`s; it never starts P2P/RPC/frontend
  services:
  - Success: `IMPORT COMPLETE (committed=1): sapling_anchors=... sprout_anchors=... sapling_nf=... sprout_nf=...` — both activation cursors are now 0, the reducer resumes folding from `tip_height + 1` on the next **normal** boot.
  - Failure: `IMPORT REFUSED — nothing committed, wedge intact (both activation cursors stay POSITIVE, gap blockers remain).` — the whole import runs inside one `BEGIN IMMEDIATE` transaction in `progress.kv` (`shielded_history_import_service.c`); any anomaly rolls the entire transaction back, so a partial/incomplete import can never flip a cursor to 0 (the load-bearing safety property — see `docs/work/fast-sync-to-tip-plan-2026-07-16.md` §1 for why an incomplete flip is worse than the wedge itself).
- **Source read:** the target's own `progress.kv`/`node.db`; the *source*
  chainstate (`<zclassicd-datadir>/chainstate`) is read through a
  point-in-time LevelDB snapshot (`ldb_snapshot_make`, manifest-changed
  retry) — `zclassicd` is never stopped, never written to, and can keep
  advancing during the import.
- **Tip bind:** the importer requires the target's own header chain
  (`blocks.sapling_root`, populated by `--importblockindex` from the block
  *header*'s `hashFinalSaplingRoot`) to already cover the source chainstate's
  best block, and verifies the imported Sapling frontier against that
  header-committed root before accepting it. **Run `--importblockindex`
  against the live `zclassicd` datadir immediately before the import call**
  if the target's headers might be stale (a stale header chain fails closed
  with `tip bind SOURCE is all-zero`, never binds against a zero root).
- **Hard containment (baked into the binary, not a doc convention):**
  `import_shielded_is_live_datadir()` (`src/main.c:2559`) REFUSES to run
  when `-datadir=` resolves to `$HOME/.zclassic-c23` or
  `$HOME/.zclassic-c23-mint`, printing
  `REFUSING: <path> is a live datadir. Copy-prove on a COPY first ...` and
  exiting 1 — before opening `progress.kv` at all. **This means the live
  cutover cannot run `-import-complete-shielded` in place against the live
  datadir under any flag or environment override; there is no bypass.** The
  only way to bring the live datadir to the cured state is to **install the
  already-proven-and-completed copy** in its place — see §4.

---

## 2. Preconditions (do not run ANY command in §4 until every one holds)

1. **Copy-prove is a clean PASS, and the copy has reached the header tip.**
   Run (or confirm a prior run of) the full pipeline against a **fresh**
   `cp -a` of the then-current live datadir:
   ```bash
   tools/scripts/import-copy-prove.sh \
     --src=$HOME/.zclassic-c23 \
     --chainstate-src=$HOME/.zclassic/chainstate \
     --expect-climb-past=<CURRENT-LIVE-H*>  \
     --deadline=<seconds sized for a full forward-fold to tip, not just the wedge>
   ```
   Re-derive `<CURRENT-LIVE-H*>` from `zclassic23 status` /
   `dumpstate reducer_frontier` on the live node immediately before running —
   do not reuse `3,176,325` from this doc without checking it is still
   current. **PASS requires all of:**
   - Gate (a): `H*` climbs strictly past the wedge height **and both**
     `utxo_apply.anchor_backfill_gap` / `utxo_apply.nullifier_backfill_gap`
     are absent from `dumpstate blocker`.
   - Gate (b): `coins_applied_height == H* + 1` at every observed sample (no
     rowless span) throughout the run.
   - Gate (c): exact same-height block-hash parity against `zclassicd` at the
     wedge height, wedge+1, **and the copy's final tip** — not skipped
     (`--skip-zclassicd-check` unset).
   - **Additionally for THIS runbook (stricter than the script's own PASS
     bar):** the copy's final tip is within a small, explicitly stated block
     count of `zclassicd`'s current tip (i.e. the copy actually reached
     operational tip, not merely past the wedge), and it has been observed
     holding there — advancing with live P2P/mirror sync, not stalled —
     for a stated minimum window (recommend at least the time it takes to
     receive and validate several new blocks past where the fold caught up,
     so the copy is proven to track the live chain going forward, not just
     replay history). Do not shortcut this to "PASS printed" — read the
     script's full evidence block per its own false-green warning.
   - No `download_queue_starved` escalation to `EV_OPERATOR_NEEDED` and no
     finalized row whose upstream verdict was not `ok=1`
     (`zclassic23 ops debug dash selfheal` / `zclassic23 dumpstate
     condition_engine`, or `zclassic23 dbquery` over
     `tip_finalize_log` vs `utxo_apply_log`, on the copy).
2. **The fix is on `main`, gates green.** `git log --oneline -- app/services/src/shielded_history_import_service.c src/main.c` on the checked-out `origin/main` shows the tip-bind fix (`2477bc499`), the Pedersen fast-fix (`63e40f347`), and the bulk-reader integration (`b9aacbaaa`); `make build-only`, `make lint`, and `make test-parallel` are clean on that exact commit (read the `N passed, M failed` line, not the pipe exit).
3. **Binary identity is pinned end-to-end.** The binary that ran the
   successful copy-prove in item 1 is **the same content-identical binary**
   that will run the live boot in §4 — confirm via SHA-256 of
   `build/bin/zclassic23` (or the deployed `~/zclassic23/build/bin/zclassic23`
   the systemd unit's `ExecStart` points at), not by trusting that "nothing
   changed" since the copy-prove run. If the binary is rebuilt between
   copy-prove and cutover, **re-run copy-prove** — this importer has no
   binary-bound receipt mechanism (unlike the bundle path's
   `consensus_state_replay_receipt`), so an unmatched binary is a silent risk
   the tooling does not catch for you.
4. **Revert path is ready before you start — see §3.** Confirm free disk
   space for two full copies of the live datadir sitting side-by-side
   (`df -h $HOME` — the live datadir is single-digit GB at this height, so
   this is not a binding constraint on this host, but check anyway; do not
   assume the free-space figures from earlier `docs/work/cure-runbook-2026-07-16.md`
   preconditions still hold — they rot).
5. **`zclassicd` stays running throughout.** Nothing in this runbook stops
   or writes to it; it is read only via RPC (`getblockcount`,
   `getblockhash`) and via the point-in-time chainstate snapshot the
   importer itself takes.

---

## 3. Why the revert path is a datadir swap, not a transaction rollback

The bundle-install path (`sovereign-cutover-runbook.md`) captures a
`VACUUM INTO` prior-generation snapshot of `progress.kv` **inside** the
install transaction itself, so its revert is "restore the named preinstall
file." **This importer has no equivalent auto-capture** — it is a one-shot
boot mode with a single all-or-nothing SQLite transaction, and (per §1) it
structurally refuses to even run against the live datadir. The revert
mechanism for this runbook is therefore operator-driven and coarser, but
simple and robust:

- **Before any live-affecting step, the pre-cutover live datadir is renamed
  aside, never deleted, never overwritten in place.** A directory rename on
  the same filesystem is fast (not a byte copy) and the live service stays
  fully stoppable/restartable against the old path for as long as that
  renamed copy exists.
- **The proven COPY becomes the new live datadir by being moved into the
  live path**, not by running any node binary against the live path in
  import mode (which is refused, per §1). No node process ever runs the
  import against `$HOME/.zclassic-c23`; the import only ever ran against the
  copy, before the swap.
- Because this is a whole-directory swap, "restore the prior generation" is
  exactly "rename the preserved pre-cutover directory back to the live
  path" — the same operation in both directions, no partial-file surgery.

---

## 4. Owner-gated live cutover procedure

**Do not begin unless every item in §2 is independently re-confirmed at
execution time.** The live node stays readable and wedged (unchanged) through
step 3; the only unavailable window is steps 4–7.

### (a) Final copy-prove re-confirmation, immediately before cutover

State can have moved since the copy-prove in §2 item 1 (the live datadir
keeps churning at the wedge — `coins_applied_height` retries at
`3,176,326`, `zclassicd` keeps advancing). Immediately before starting step
1 below, re-run `tools/scripts/import-copy-prove.sh` one more time against a
**fresh** copy of the **current** live state, and require the same clean
PASS as §2 item 1 (including the "reached tip and held" bar). Do not reuse an
older copy-prove result as authorization for a cutover run hours or days
later — re-prove close to the cutover window.

Keep this final proven copy — it is `$COPY_DIR` below, the exact directory
that becomes the new live datadir in step 4.

### (b) Preserve the pre-cutover state (the revert path)

```bash
systemctl --user stop zclassic23
# Confirm it actually stopped and released its datadir lock before touching
# anything:
systemctl --user is-active zclassic23   # expect: inactive
```

```bash
PRECUTOVER="$HOME/.zclassic-c23.PRECUTOVER-$(date +%Y%m%d%H%M%S)"
mv "$HOME/.zclassic-c23" "$PRECUTOVER"
echo "Pre-cutover live datadir preserved at: $PRECUTOVER"
```

This `mv` **is** the revert path — see §3. Do not `rm` it, do not overwrite
it, and do not start any node against it accidentally (it is not on the live
path anymore, so the systemd unit cannot pick it up by name — but a manual
`-datadir=$PRECUTOVER` invocation would still work against it; be deliberate).

### (c) Install the proven copy as the new live datadir

```bash
# $COPY_DIR is the final-proven copy from step (a) above — the one that
# passed the FULL G-SOV bar including "reached tip and held."
cp -a "$COPY_DIR" "$HOME/.zclassic-c23"
# Verify the copy landed intact before trusting it (cheap sanity check —
# not a substitute for the copy-prove gates already run against $COPY_DIR):
du -sh "$HOME/.zclassic-c23" "$COPY_DIR"
```

`cp -a` (not `mv`) is deliberate: it leaves `$COPY_DIR` intact as a second,
independent piece of evidence until the live restart in step (c.1) below is
confirmed good, at the cost of one extra full-datadir copy's worth of disk
and time (§2 item 4 sized for exactly this). If disk is genuinely tight and
the operator explicitly accepts losing that redundancy, `mv "$COPY_DIR"
"$HOME/.zclassic-c23"` is equivalent and faster — but then the only
remaining pre-restart evidence is `$PRECUTOVER` (the old wedged state) plus
whatever logs `import-copy-prove.sh` already wrote under the old `$COPY_DIR`
path before the move.

Confirm the binary that is about to run (§2 item 3's pinned identity) is the
one the systemd unit's `ExecStart` actually points at:

```bash
SERVICE_BIN=$(systemctl --user show zclassic23 -p ExecStart --value | sed -n 's/.*path=\([^ ;]*\).*/\1/p')
"$SERVICE_BIN" -version   # or equivalent build-identity check
sha256sum "$SERVICE_BIN"  # compare against the binary that ran copy-prove
```

If the deployed binary does not match, `make deploy` first (this also
restarts the service — stop it again before the restart in step (c.1), same
caution `sovereign-cutover-runbook.md` documents for the bundle path).

### (c.1) Restart and verify

```bash
systemctl --user start zclassic23
```

The node boots **normally** (no special flag — the copy already has both
activation cursors at 0 and is already at/near tip; this is an ordinary
warm boot from a healthy datadir, not a forward-fold-from-anchor run).

**Verification — do not declare success on "started without FATAL":**

```bash
zclassic23 status
# or: RPC getblockcount / getinfo on rpcport 18232
```

Check, in order:

1. **H\* climbs and keeps climbing.** `dumpstate reducer_frontier` shows
   `hstar` advancing past the old wedge height and tracking new blocks as
   they arrive (poll a few times over a few minutes — a single high reading
   is not proof of a live, advancing fold).
2. **Both gap blockers stay absent.** `dumpstate blocker` — neither
   `utxo_apply.anchor_backfill_gap` nor `utxo_apply.nullifier_backfill_gap`
   is present.
3. **`coins_applied_height == H* + 1`** continuously, not just at one sample.
4. **Same-height hash parity vs `zclassicd`** at the old wedge height and at
   the current live tip (`getblockhash` on both RPC 18232 and `zclassicd`'s
   RPC 8232).
5. **Explorer / onion surface returns.** With `-tor` on the live unit,
   `zclassic23 status` → `health.checks.onion_address` reports the same (or
   a freshly regenerated) onion address, and the explorer answers over the
   configured HTTPS port (default derived from `-rpcport` unless
   `-httpsport=` is set explicitly in the unit's env file — check
   `~/.config/zclassic23/env` if unsure) — confirm with a local `curl -k
   https://127.0.0.1:<httpsport>/` (or the onion address via a Tor-enabled
   client) returning a normal explorer page, not a connection refusal.
6. **P2P is healthy.** Peer count above the coordinator floor (3), no
   `download_queue_starved` escalation in the minutes after restart
   (`zclassic23 ops debug dash selfheal` / `zclassic23 dumpstate condition_engine`).
7. **No new consensus-parity divergence.** Continue the hash-parity check at
   the live tip periodically for the first hour post-cutover — a divergence
   surfacing only after real-time P2P blocks (not just replayed history) is
   exactly the scenario `sovereign-cutover-runbook.md`'s rollback section
   was written for on the bundle path, and it applies here too.

### (d) Rollback if H\* does not climb within N minutes

**Trigger:** any of the following within a bounded observation window after
step (c.1) — **recommend N = 30 minutes**, sized generously above the time a
healthy warm boot takes to reconnect P2P and resume folding (re-derive this
from a normal live restart's observed reconnect time if available, rather
than trusting 30 as a magic number):

- `H*` has not advanced at all since the restart, or has *regressed*.
- Either gap blocker (`anchor_backfill_gap` / `nullifier_backfill_gap`)
  reappears.
- `coins_applied_height != H* + 1` at any sampled point.
- A hash mismatch against `zclassicd` at any checked height.
- The process crash-loops (`systemctl --user status zclassic23` shows
  repeated restarts via `Restart=always`) without ever reaching a stable
  `hstar` reading.

**Rollback procedure:**

```bash
systemctl --user stop zclassic23
# Preserve the failed post-cutover state for diagnosis before overwriting
# anything — do not skip this even under time pressure:
mv "$HOME/.zclassic-c23" "$HOME/.zclassic-c23.FAILED-CUTOVER-$(date +%Y%m%d%H%M%S)"
# Restore the preserved pre-cutover datadir from step (b):
mv "$PRECUTOVER" "$HOME/.zclassic-c23"
systemctl --user start zclassic23
```

Re-confirm the pre-cutover wedge state is exactly as it was
(`H*` back at the old wedge height, `utxo_apply.anchor_backfill_gap`
present again) before deciding on a next attempt — this is a genuine
whole-datadir restore, not a partial one, so there is no ambiguity about
what state the node is in afterward. Do not delete
`.FAILED-CUTOVER-*` or the exhausted `$COPY_DIR` until the failure is
root-caused; they are the only evidence of what went wrong.

### After a successful cutover

1. Update `docs/HANDOFF.md`: new live `H*`, confirm
   `utxo_apply.anchor_backfill_gap` / `utxo_apply.nullifier_backfill_gap`
   are clear in `zclassic23 dumpstate condition_engine` / `zclassic23 ops debug dash selfheal`, note
   `trust_mode=release_assisted` (not yet `sovereign` — see scope note at
   the top of this doc) via the `sovereignty` dumpstate subsystem if
   registered on the deployed build.
2. Clean up `$PRECUTOVER` and the exhausted `$COPY_DIR` only once the live
   node has held a stable, advancing `H*` with continued hash parity for a
   deliberately chosen soak window (do not delete same-day) — they are the
   only rollback evidence until then.
3. The producer/bundle sovereign-cure track (§0 of `docs/HANDOFF.md`,
   `sovereign-cutover-runbook.md`) continues independently and can later
   upgrade this same live node from `release_assisted` to `sovereign`
   (`coins_kv_self_folded=true`) via its own copy-prove + cutover sequence —
   that is a separate owner-gated event, not implied by completing this one.

---

## 5. Standing rules

- **Never run `-import-complete-shielded` against `$HOME/.zclassic-c23` or
  `$HOME/.zclassic-c23-mint` directly** — the binary refuses this by
  construction (§1); do not attempt to defeat the containment check (e.g. by
  symlink tricks or a modified binary) as a shortcut. The only sanctioned
  path to a cured live datadir is the copy-prove-then-swap sequence in §4.
- **Copy-prove on a fresh copy, gate on H\* CLIMB *to tip* and hash parity —
  never "the import printed IMPORT COMPLETE."** That banner proves the
  import transaction committed, not that the subsequent forward fold reaches
  and holds at tip. This is the same false-green trap the sovereign-cure
  runbook warns about for its own install banner.
- **Preserve, never delete, the pre-cutover datadir until the new state is
  independently confirmed durable and correct** — same standing rule as
  `docs/HANDOFF.md` §5 "Standing method" and the sovereign-cutover-runbook's
  rollback section.
- **This runbook is contained by construction wherever the mechanism itself
  is contained** — nothing here should be read as authorizing an automated
  agent to execute §4 without a fresh, explicit owner go-ahead at the moment
  of execution.

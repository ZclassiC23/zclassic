## CURRENT STATE (2026-07-01, live verified)

**2026-07-01 14:36 UTC update.** Source is clean up through pushed
`8f31dd95d` (`improve anchor mint progress observability`), with one local
follow-up now proof-gated and ready to commit: the mint progress cadence
predicate is exposed as `boot_mint_anchor_should_log_progress()` and
`mint_skip_crypto` proves 10k heartbeats at 10000/20000 plus the final-anchor
tail. Gates run for this follow-up: `git diff --check`, `make -j$(nproc)
build-only`, `make t ONLY=mint_skip_crypto`, `make lint`, `make
check-doc-accuracy`, full `make test` (`0/485` failed, 14 self-skipped), and
`make -j$(nproc) build/bin/zclassic23`. A full copied-chain sidecar attempt at
`$HOME/.zclassic-c23-anchor-mint-cp2-proof` did not reach the mint loop before
being stopped and was removed, so CP-2 still needs runtime evidence from a
fixture/next mint with two increasing 10k heartbeat samples before it can close.
The active real producer was not touched: `zclassic23-anchor-mint.service` is
still active as PID `3160516`, using the earlier fixed `16c841ca8` binary,
memory about 6.5 GB, no snapshot artifact yet, with reducer cursors around
17,000 (`tip_finalize` around 16,000) of 3,056,758. Main remains healthy at tip:
native `zclassic23 agent` reported `status=healthy`, `serving=true`,
`height=3166533`, `target_height=3166534`, `gap=1`, 4 peers, and Tor/onion
ready. Soak remains red: `status=blocked`, `serving=false`, `height=3056758`,
`target_height=3166534`, `gap=109776`, 4 peers, `sync_state=blocks_download`.
Mirror remains legacy-oracle blocked: `mirror_running=true`, `reachable=false`,
`zclassic23_height=3166534`, `zclassicd_height=0`, `in_flight=false`,
`consensus_authority=local_consensus_validation`, `overrides_total=0`,
`activation_blocker=rpc-unreachable`, and `last_error="rpc error -28:
Activating best chain... height 0 (1%)"`.

**2026-07-01 13:27 UTC update.** Source is clean and pushed at
`81a6cc816` (`fix starter pack low frontier reseed`). That patch is proof-gated
but not deployed to the live main/soak binaries yet; the deployed live runtime
remains the earlier `a02a1bac6` lineage. Main is healthy at tip: native
`zclassic23 agent` reported `status=healthy`, `serving=true`,
`operator_needed=false`, `height=3166481`, `target_height=3166482`, `gap=1`,
Tor/onion ready, and 4 peers. Soak is still not green:
`zclassic23-soak -datadir=$HOME/.zclassic-c23-soak -rpcport=18242 agent`
reported `status=blocked`, `serving=false`, `height=3056758`,
`indexed_height=3145595`, `header_height=3166481`, `gap=109723`, 4 peers, and
`sync_state=blocks_download`. Mirror status is degraded only on the legacy
oracle side: `./tools/z mirror --json` reported `zclassic23_height=3166483`,
`zclassicd_height=0`, `reachable=false`, `mirror_running=true`,
`in_flight=false`, `consensus_authority=local_consensus_validation`,
`activation_blocker=rpc-unreachable`, and `last_error="rpc error -28:
Activating best chain... height 0 (1%)"`.

**2026-07-01 13:46 UTC correction.** The first transient anchor-mint producer
was stopped before publishing an artifact because the review found a real
offline-boundary bug: `-mint-anchor-fast` had armed the crypto pass-through but
normal frontend/P2P/RPC/runtime services still started before the one-shot fold
driver. The fix now makes `ctx->mint_anchor` return from `app_init` before
`app_init_services`, initialize only the eight reducer stages offline, and exit
through `app_shutdown_offline`. A foreground smoke with the fixed binary reached
`[boot] -mint-anchor: offline reducer stages initialized; skipping
frontend/P2P/runtime services` and `[mint-anchor] driving the genesis..3056758
fold; starting at applied-through=-1`, with no P2P/RPC/frontend markers.

**2026-07-01 14:05 UTC producer update.** The fix is committed as
`16c841ca8` (`fix offline anchor mint boundary`) and `build/bin/zclassic23`
was rebuilt clean with `ZCL_BUILD_COMMIT="16c841ca8"`. The isolated mint
datadir was recreated fresh from
`$HOME/.zclassic-c23-COPY-20260701-113424-stall-3166384` and the producer is
running as `zclassic23-anchor-mint.service`, PID `3160516`, with output target
`$HOME/.zclassic-c23-anchor-mint/utxo-anchor.snapshot`. At 14:03:45 UTC the
journal reached the expected fixed path:
`[boot] -mint-anchor: offline reducer stages initialized; skipping
frontend/P2P/runtime services` and `[mint-anchor] driving the genesis..3056758
fold; starting at applied-through=-1`. A negative journal scan since
14:00:18 UTC found no `p2p_services_start`, peer-connection, RPC/frontend,
API-cache, condition-engine, or `ZClassic C23 node initialized` lines. At
14:05 UTC the process was active, memory was about 6.4 GB, the fold had emitted
`applied_height=0`, and no snapshot artifact existed yet.

The earlier smoke used the already-interrupted
`$HOME/.zclassic-c23-anchor-mint` workspace and then safely reported a missing
body span at `applied-through=2999`; do not reuse that mutated datadir for the
real producer. If this active producer must be restarted before publishing the
snapshot, recreate it from the stopped source copy first:

```bash
rm -rf $HOME/.zclassic-c23-anchor-mint
cp -a $HOME/.zclassic-c23-COPY-20260701-113424-stall-3166384/. \
  $HOME/.zclassic-c23-anchor-mint/
rm -f $HOME/.zclassic-c23-anchor-mint/zclassic23.pid \
  $HOME/.zclassic-c23-anchor-mint/.lock \
  $HOME/.zclassic-c23-anchor-mint/.cookie
```

Then start the producer from the fixed binary:

```bash
systemd-run --user --unit=zclassic23-anchor-mint \
  --property=WorkingDirectory=/home/rhett/github/zclassic23 \
  --setenv=ZCL_MINT_ANCHOR_OUT=/home/rhett/.zclassic-c23-anchor-mint/utxo-anchor.snapshot \
  /home/rhett/github/zclassic23/build/bin/zclassic23 \
  -datadir=/home/rhett/.zclassic-c23-anchor-mint \
  -nolegacyimport -mint-anchor -mint-anchor-fast -nobgvalidation
```

Expected journal markers: `-refold-staged: staged reducer reset to genesis OK`,
`-mint-anchor-fast: OFFLINE FAST-MINT`, `[boot] -mint-anchor: offline reducer
stages initialized; skipping frontend/P2P/runtime services`, and
`[mint-anchor] driving the genesis..3056758 fold; starting at
applied-through=-1`. There must be no `p2p_services_start`, peer-connection,
RPC/frontend, or `ZClassic C23 node initialized` lines on this path. Inspect
with:

```bash
systemctl --user status zclassic23-anchor-mint --no-pager -l
journalctl --user -u zclassic23-anchor-mint -n 120 --no-pager
ls -lh $HOME/.zclassic-c23-anchor-mint/utxo-anchor.snapshot*
```

Do not stage anything until the final snapshot exists and the mint has exited
successfully; the mint path unlinks/fails the artifact on SHA3/count mismatch.
After the artifact exists, run the opt-in artifact check and the full H* climb
copy proof before any live cutover:

```bash
ZCL_SELF_FOLD_ANCHOR_FIXTURE=$HOME/.zclassic-c23-anchor-mint/utxo-anchor.snapshot build/bin/test_zcl
ZCL_ANCHOR_SNAPSHOT_SRC=$HOME/.zclassic-c23-anchor-mint/utxo-anchor.snapshot make seed-anchor-snapshot
make repro-on-copy SLUG=soak-refold REPRO_SRC=$HOME/.zclassic-c23-soak REPRO_FULL=1 CLIMB_PAST=3056758 ARGS='-refold-from-anchor -nobgvalidation -paramsdir=$$HOME/.zcash-params'
```

**2026-07-01 12:55 UTC update.** The code-review lane reopened briefly for one
safe-fail hardening found by the soak copy proof; it is not a soak cure. The
earlier live `block_failed_mask_at_tip` / missing-3166384 blocker is no longer
the active main-service state. Commit `a02a1bac6` remains deployed to the main
linger service and to the pinned soak binary; the latest source follow-up
hardens `tip_finalize` but is not deployed because the copy proof still does
not climb. Current main check: `zclassic23 agent` reports `status=healthy`,
`serving=true`,
`operator_needed=false`, `height=3166414`, `sync_state=at_tip`, `gap=1`, and 3
peers. The pinned soak lane was deliberately refreshed from stale build
`b6ab458f0` to build `a02a1bac6` at
`2026-07-01 12:15:43 UTC`; that restart is a conscious C6 rebaseline, not soak
credit. Post-restart soak RPC is now up and observable, but not green:
`agent` reports `status=blocked`, `serving=false`, `height=3056758`,
`indexed_height=3145595`, `header_height=3166415`, `gap=109657`, 4 peers, and
`sync_state=blocks_download`.

The latest review patch fixes two liveness/API clarity edges in the condition
layer:

- `block_failed_mask_at_tip` no longer treats any `HAVE_DATA` block at
  `active_tip + 1` as a no-advance successor; it must be the active tip's
  direct child.
- `local_header_refill_needed` now queues the same-height best-header ancestor
  body when the best header is ahead on a fork, preserves
  `SYNC_BLOCKS_DOWNLOAD` after a successful body queue, and uses the same
  `block_has_any_failure()` gate as the queue helper.

Proof for that patch: `git diff --check`, `make -j$(nproc) build-only`,
`make lint`, `make t ONLY=sync_watchdog_conditions`, `make t
ONLY=utxo_activation_paused`, `make t ONLY=tip_fork_stale`, full `make test`
(`485` groups, `0` failed, `14` self-skipped), full `make -j$(nproc)`, and
`make sim-fast` all passed. Commit `a02a1bac6` was pushed to `main` and
deployed to the main user linger service with `ZCL_BUILD_COMMIT="a02a1bac6"`.
The proof-validate Act-1 symmetry TODO in `self-verified-tip-plan.md` is also
already implemented and proof-gated: `make t ONLY=reducer_frontier_reconcile_light`
passes the readable stale-proof production route where a proof-only
`internal_error` rewinds proof state without clobbering script state.

The 12:55 UTC follow-up adds a `tip_finalize` clamp guard: the one-height
`coins_applied` next-frame lag is still allowed, but a deeper coins cap now
refuses with `tip_finalize clamp refused` instead of rewinding the stage cursor
below the trusted H\* floor. Focused proof: `make t
ONLY=reducer_frontier_reconcile_light` passes the new `deep-coin-lag` fixture;
`make lint` passes (remaining file-size advisory is pre-existing
`config/src/boot_refold_staged.c`); full `make test` passes (`0/485` groups
failed, `14` self-skipped). Full soak copy proof still fails to climb, which is
the correct remaining blocker, not a green deploy signal.

**Restart command:** type **`continue zclassic23 development`**. First checks:
`git status --short --branch`, then the native API discovery command:
`zclassic23 api`. It returns the same `zcl.rest_index.v1` body as
`GET /api` and `GET /api/v1`, including the v1 resource/CRUD shape. For compact
live state use `zclassic23 agent` (same contract as MCP `zcl_agent` and REST
`GET /api/v1/agent`). For version-milestone progress use
`zclassic23 milestone` (same contract as REST `/api/v1/milestone` and MCP
`zcl_milestone`): it emits node-computed ASCII `systems`, `goals`, and
`subgoals` bars while keeping strict MRS separate from partial proof progress.
Use `zclassic23 refold` (same contract as REST `/api/v1/refold` and MCP
`zcl_refold_status`) for the sovereign anchor readiness check before any
`-refold-from-anchor` copy proof. Use `zclassic23 healthcheck`,
`/api/v1/node/status`, `zcl_status`, `zcl_state subsystem=reducer_frontier`,
and `zcl_state subsystem=refold` only for drill-down. Current runtime code build
deployed to main + soak is `a02a1bac6` (`fix fork body recovery conditions`);
use `git log --oneline -3` for the exact docs/handoff commit on top.

**Live node.** The user linger service is running the locally deployed binary
(`make deploy`, installed at `$HOME/.local/bin/zclassic23-live`). The latest
API simplification deploy is native-only: `zclassic23 api` is the discovery
entry point, `zclassic23 agent` is the compact status entry point,
`zclassic23 milestone` is the v1 progress/status entry point, and
`zclassic23 refold` is the anchor-readiness entry point, and
`zclassic23 healthcheck` is the drill-down entry point. No helper binary or
shell wrapper is part of the operator path. Bare `zclassic23 agent`,
`zclassic23 milestone`, and `zclassic23 refold` discover the running user
service datadir/rpcport from systemd when the default cookie is absent, so no
`-datadir` flag is needed for the normal owner command. `make deploy` now
refreshes the owner-command symlink at `$HOME/.local/bin/zclassic23` and any
existing `$HOME/bin/zclassic23` PATH shadow so those commands cannot point at a
stale pre-API binary. At the latest handoff check the native node reported
`status=healthy`, `serving=true`, `operator_needed=false`, Tor/onion ready,
served height `3166414`, header/indexed height `3166415`, `gap=1`, and 3 peers.
The public `/api/status` surface is aligned with the health contract for the
normal H* race: a one-block served-frontier gap reports `status=healthy`,
`operator_needed=false`, and `primary_blocker=none`; gaps greater than one
remain named as catching-up or degraded.

```
systems
native node service        [##########] build a02a1bac6 healthy/serving on main; normal one-block H* race observed
native API discovery       [##########] `zclassic23 api`, REST `/api`/`/api/v1`, shared `zcl.rest_index.v1`
simple agent API           [##########] native `zclassic23 agent` live; reports healthy/serving on main
milestone status API       [##########] native `zclassic23 milestone`, REST v1, MCP `zcl_milestone` live
refold readiness API       [##########] native `zclassic23 refold`, REST v1, MCP `zcl_refold_status` live on main
REST resource routing      [#########-] `/api/v1` route table wired; dynamic/member routes still controller-owned
API version contract       [##########] `/api/v1` canonical; unsupported `/api/vN` returns supported versions
lint-gate hardening        [##########] coin lookup hollow scans + raw node.db DML exec are proof-gated
startup health evidence    [##########] health falls back to durable tip_finalize cursor during boot
HODL website freshness     [########--] view caps to served tip; verify after each deploy
factoids website freshness [########--] capped to served tip; verify after each deploy
legacy mirror advisory     [####------] monitor running; zclassicd RPC -28 at height 0
formal soak evidence       [#---------] rebaselined on a02a1bac6 at 2026-07-01 12:15 UTC; wait for fresh 168h window
```

**Soak evidence / rebaseline.** C6 is **not green**. `make
soak-evidence-report` on 2026-07-01 after the rebaseline reported
`VERDICT=NOT_MET reason=operator_intervention_detected_x5`; the historical
window includes deliberate restarts and therefore cannot count for C6. The soak
service was deliberately rebaselined again at `2026-07-01 12:15:43 UTC` because
it was still pinned to stale build `b6ab458f0`; the pinned binary now reports
runtime build `a02a1bac6`. The live local systemd drop-in
`~/.config/systemd/user/zclassic23-soak.service.d/zz-oom-budget.conf` remains
at `MemoryHigh=24G` / `MemoryMax=32G` (matching the unit's intended budget).
Do not mark C6 complete until a fresh uninterrupted window is judged `MET`.
Post-restart RPC/health evidence: soak `agent` reports `status=blocked`,
`serving=false`, `height=3056758`, `indexed_height=3145595`,
`header_height=3166415`, `gap=109657`, 4 peers, and
`sync_state=blocks_download`. `healthcheck` shows peer health is usable but
`reducer_frontier_reconcile_light` is active with
`last_reconcile_coin_backfill_status_label="owner_refused"` at hole height
`3145595`; `reducer_frontier` shows H\*=3056758 and first blocker
`validate_headers_log` `missing-success-row` at 3056759. A fresh
`tools/scripts/soak_evidence.sh collect` sample was appended and is correctly
red (`ok:false`) because zclassicd oracle was returning RPC `-28`, so the
sample cannot prove gap=0. Next work is not another soak restart; it is a
copy-proven sovereign/refold or coin-backfill cure, then a fresh uninterrupted
168h judge window.

**Soak copy proof result.** Do **not** enable
`ZCL_REDUCER_COIN_BACKFILL_ACK=1` on the live soak datadir yet. The older full
copy proof at
`~/.zclassic-c23-COPY-20260701-090829-soak-coin-backfill-ack` showed an unsafe
L1 path: `reducer_frontier_reconcile_light` clamped major cursors below the
anchor (`validate_headers` near 2100, `utxo_apply` near 1, `tip_finalize` near
0) while H\* stayed at 3056758. The 12:55 UTC follow-up fixes that unsafe
subpath. Re-run proof:
`make repro-on-copy SLUG=soak-window-consistency-tipguard
REPRO_SRC=$HOME/.zclassic-c23-soak REPRO_FULL=1 CLIMB_PAST=3056758
REPRO_DEADLINE=240 ARGS='-nobgvalidation -paramsdir=/home/rhett/.zcash-params
-showmetrics=0'` created
`~/.zclassic-c23-COPY-20260701-124112-soak-window-consistency-tipguard` and
failed safely: `first_tip=0`, `max_tip=3056758`, `climbed=0`. The copy log now
shows `tip_finalize clamp refused: coins_applied=1 applied_through=0 is below
hstar=3056758 min_allowed=3056757 cursor=3160268`, then
`state_window_inconsistent` fires. Treat this as a no-go for live owner ack and
as evidence that the remaining work is the state-window/refold cure, not another
L1 cursor clamp.

**2026-07-01 13:11 UTC code-review follow-up.** The starter-pack autodetect
gate now treats `coins_kv_is_proven_authority()` as a resume skip only when the
durable `coins_applied_height` is at or above the detected `utxo-seed-<H>`
snapshot seed (`applied >= H+1`). A low-frontier but stamped authority
(`coins_applied_height=1`, migration stamp, non-empty coins) no longer blocks
autodetect from selecting the verified bundle, so the existing
`-load-snapshot-at-own-height` reset can reseed instead of leaving H\* pinned
at the compiled anchor. Regression coverage:
`make t ONLY=boot_snapshot_failure_memory` now includes the low-frontier case,
and `make t ONLY=load_verify_boot`, `make -j$(nproc) build-only`,
`git diff --check`, `make lint`, `make check-doc-accuracy`, and full
`make test` pass (`0/485` groups failed, `14` self-skipped). This still does
**not** make the current soak node green by itself: both main and soak
`zclassic23 refold` continue to report
`primary_blocker=missing_verified_anchor_snapshot`, so the sovereign cutover
still needs a staged/minted verified anchor and an H\* climb copy proof before
deploy.

**Copy-proof climb/refold preflight landed.** `tools/repro_on_copy.sh` accepts
`--expect-climb-past=H`, and `make repro-on-copy` exposes it as
`CLIMB_PAST=H`. This upgrades the existing copy harness from only "no public tip
regression" to the runbook bar: if RPC answers but H\* never climbs strictly
past the named blocker/anchor before the deadline, the run exits FAIL instead
of PASS. `-refold-from-anchor` proofs now also fail before copying unless they
use a full datadir copy (`REPRO_FULL=1` / `--full`) and an anchor snapshot
candidate exists at `$ZCL_MINT_ANCHOR_OUT` or `<src>/utxo-anchor.snapshot`.
Once the copy boots, the harness still refuses a refold PASS unless the node log
proves the SHA3-verified MINTED snapshot was loaded, and unless H\* is observed
at/below `CLIMB_PAST` before crossing it. A first observed tip already above the
gate is no longer accepted as a climb proof.
The repro manifest records `climb_past`, `refold`, and
`anchor_snapshot_candidate`, so failed copies are auditable after the process
exits. Use this for soak refold/cure work only after staging the anchor
snapshot, for example:
`make repro-on-copy SLUG=soak-refold REPRO_SRC=$HOME/.zclassic-c23-soak REPRO_FULL=1 CLIMB_PAST=3056758 ARGS='-refold-from-anchor -nobgvalidation -paramsdir=$$HOME/.zcash-params'`.

**Native refold readiness surface landed.** The node now owns the preflight
answer directly: `zclassic23 refold`, REST `GET /api/v1/refold`, and MCP
`zcl_refold_status` all return `zcl.refold_status.v1`. The response includes
the compiled checkpoint, candidate path/source, stat result, snapshot header
when readable, and the same full-body SHA3 + expected checkpoint SHA3 + count
predicate that boot uses before trusting the anchor snapshot. It is read-only:
no `coins_kv` mutation, no boot reset, no shell helper. While the current anchor
artifact is absent the expected blocker is
`primary_blocker=missing_verified_anchor_snapshot`; after a verified snapshot is
staged, the next action is the full `-refold-from-anchor` copy proof above.
Commit `2e8a1639b` was pushed to `main`; focused tests, `make lint`,
`make test`, and `make sim-fast` passed before deploy. `make deploy` then
installed the build but failed the post-restart health gate because live chain
progress did not return to green. Next developer: do not debug this by live
surgery. First reproduce the `block_failed_mask_at_tip` / missing 3166384 class
on a copied fullhist datadir, prove the fix fires on the copy, then deploy.

**Reducer-frontier refusal hardening landed.** A follow-up on 2026-07-01 pins
the live soak failure class where `coin_backfill_status_label=owner_refused`
could fall through the L1 reconcile pass and let unrelated cursor clamps report
`last_reconcile_repaired=true` while the missing-coin blocker remained
unresolved. Coin-backfill refusal statuses now claim the repair tick as a named
blocker without setting `repaired`; the condition detector treats typed
repair/backfill evidence as actionable even when no cursor was clamped, so the
refusal still escalates instead of going quiet. Regression coverage lives in
`test_reducer_frontier_reconcile_light`: the new terminal-refusal fixture
asserts no body/tip/script/utxo cursor mutation and the condition fixture
asserts `last_reconcile_repaired=false` while returning `COND_REMEDY_FAILED`.
This is a safety/diagnostic hardening only; it does **not** prove live owner ack
and does **not** make the current soak node green. It was deployed to both the
main service and the pinned soak service on 2026-07-01; the soak service restart
time was `2026-07-01 09:52:38 UTC`.

**Simulator coverage fast gate landed.** The deterministic chaos harness now has
a native seed override (`zclassic23-chaos --seed=0x...`) and `make sim-fast`.
`sim-fast` runs the `chaos_harness` unit slice, every checked-in chaos scenario,
and a bounded seed sweep of the seed-sensitive peer-churn scenario
(`CHAOS_SEEDS=64` by default). Use it as the first high-performance simulation
gate for reducer/boot/peer/fault-injection changes before escalating to
full-binary crash or soak gates.

**Lint-gate hardening landed.** The controller chainstate coin-lookup guard now
fails loud instead of silently passing if its scan surface disappears:
`tools/scripts/check_coins_lookup_nullcheck.sh` requires controller `.c` files,
requires at least one `coins_view_cache_get_coins()` call site, and uses
`gate_grep` for grep error handling. `test_make_lint_gates` now inject-verifies
both an empty controller scan and a non-empty/no-lookup scan exit 2 before the
real tree passes. The audit note is in `docs/work/lint-gate-hollowness-audit.md`.
The raw-SQL gate also now rejects direct node.db DML through
`sqlite3_exec(ndb->db|ndb.db, "INSERT/DELETE/UPDATE/REPLACE ...")`; import and
boot bulk writes that fit this class use `ar_exec_write_sql()` /
`AR_STEP_WRITE`, and `test_make_lint_gates` plants
`raw_sqlite_exec_node_db_fixture.c` to prove the class fails loud.

**Versioned agent API landed.** The owner/agent first call is now the same shape
across the binary, REST, and MCP: `zclassic23 agent`, `GET /api/v1/agent`, and
`zcl_agent` all return compact `zcl.public_status.v1` JSON with
`api_version="v1"`. REST discovery is `GET /api` or `GET /api/v1`
(`zcl.rest_index.v1`) and names `/api/v1` as canonical while keeping `/api`
compat aliases. The REST version constants live in
`app/controllers/src/api_controller_internal.h`; requests for unsupported
version prefixes such as `GET /api/v2/agent` now return structured
`zcl.rest_error.v1` JSON with `error="unsupported_api_version"`,
`requested_version`, `supported_versions=["v1"]`, and `base_path="/api/v1"`.
Resource routing now has an explicit v1 route table for exact noun resources
(`node`, `blocks`, `transactions`, `peers`, `hodl`, `factoids`, `files`,
private `wallet`); the older dynamic/member routes remain in their controllers
until they are worth folding behind resource actions.

**Native API discovery landed.** `zclassic23 api` and its RPC alias `apiindex`
now return the same `zcl.rest_index.v1` JSON body as REST `GET /api` and
`GET /api/v1`, without HTTP headers and without routing the operator through an
extra binary or shell wrapper. The shared body lives in
`api_rest_index_body_json()`, so native CLI, REST, and tests cannot drift. The
discovery document now points at native commands only:
`zclassic23 api`, `zclassic23 agent`, and `zclassic23 healthcheck`.

**Node-owned milestone bars landed.** `zclassic23 milestone` and its RPC alias
`mvpstatus` return `zcl.milestone_status.v1`: machine-readable v1 MVP progress
plus pre-rendered ASCII bars under `ascii.systems`, `ascii.goals`, and
`ascii.subgoals`. REST serves the same contract at `/api/v1/milestone`, and MCP
proxies it as `zcl_milestone`. The node computes `systems` from live runtime
health, keeps strict MVP readiness at MRS `4/8`, and reports partial/proxy proof
work separately as subgoal units, so slices never masquerade as completed MVP
criteria.

**Public served-tip fallback landed.** Public status, HODL, and factoids now use
the same internal `api_served_tip_height()` contract. It prefers the published
in-memory H* frontier and falls back to the durable
`stage_cursor('tip_finalize')` + `tip_finalize_log` anchor while the process is
still warming up. That removes the startup-only window where health already knew
the durable served tip but `/api/status`, `/api/v1/hodl`, or
`/api/v1/factoids` could briefly report height 0 or unavailable. Regression
coverage lives in `test_api`: public status uses the durable tip before H*
publication, and HODL/factoids cap to the durable served tip before H* is
published.

**Startup health fallback landed.** A deploy of the versioned REST API exposed a
startup-only false red health state: the node was at tip, but the in-memory
`tip_finalize_stage_cursor()` was still absent, so health reported
`log_head_unknown` while the durable `progress.kv`
`stage_cursor('tip_finalize')` already existed. `node_health_collect()` now
prefers the live cursor and falls back to that durable cursor during startup.
Regression coverage is
`node_health_service: durable tip_finalize cursor backs startup health`.

**API/data hardening landed.** The REST/factoid/HODL work now serves honest JSON
instead of transient 503s for normal projection races: `/api/hodl` refreshes
synchronously from the transparent UTXO set when cache lags the served tip, and
`/api/factoids` caps to `served_height` while reporting
`chain_height`/`served_height`/`indexed_height`/`index_capped`. The
`hodl_history` table is still a background time-series (`MAX(height)=3162240` at
handoff), but the website appends the latest on-demand HODL point, so the visible
current stats stay tied to the latest served block. Live check after deploy:
`GET /api/v1/hodl` returned `height=3166202`,
`served_tip_height=3166202`, `indexed_tip_height=3166202`,
`block_tip_height=3166202`, `utxo_tip_height=3166202`, `skipped_rows=0`;
`GET /api/v1/factoids` returned `chain_height=3166202`,
`served_height=3166202`, `indexed_height=3166202`, `index_capped=false`.
If v1 wants the historical table caught up to tip, make that an explicit
follow-up gate.

**Wallet-view cleanup landed.** `wallet_sapling_notes` schema v21 adds validated
`source` (`local` by default, `view` for zclassicd mirror placeholders), a
partial `idx_snote_view_address`, and a fail-closed migration. Empty successful
wallet-view RPC result arrays clear stale mirror rows; Sapling refreshes replace
only `source='view'`; real local notes are preserved; and spend attempts against
view-only placeholder balance return `view-only balance synced from zclassicd`.
Important root cause from the deploy: the partial index must live in the v21
migration, not the baseline v20 create path, because live v20 databases do not
yet have `source`.

**Verification completed before this handoff.** Focused gates covered the new
wallet/data/API paths, `git diff --check`, `make check-test-registration`,
`make check-doc-accuracy`, `tools/scripts/check_raw_sqlite.sh`, `make lint`,
full `make test` (`0/485 groups failed, 15 skipped` after the v1 API work),
production binary builds, and `make deploy`. The native-entrypoint fix re-ran
`make -j32 build-only`, `make t ONLY=make_lint_gates`, `make lint`,
`make deploy`, and live checks for `zclassic23 api`, `zclassic23 agent`,
`GET /api/v1/agent`, `GET /api/v1`, and `zclassic23 healthcheck`. The lint-gate
hardening pass re-ran `bash -n`, `git diff --check`,
`make check-coins-lookup-nullcheck`, `make t ONLY=make_lint_gates`,
`make check-doc-accuracy`, `make lint`, `make -j32 build-only`, full
`make test` (`0/485 groups failed, 14 skipped`), `make deploy`, and live checks
for `zclassic23 agent`, `GET /api/v1/agent`, `zclassic23 healthcheck`,
`zcl_status`, and `make soak-evidence-report`. The follow-up API
version and startup-health hardening re-ran `git diff --check`,
`make t ONLY=api`, `make t ONLY=node_health_service`,
`make -j32 build-only`, `make lint`, full `make test`
(`0/485 groups failed, 14 skipped`), `make deploy`, and live checks for
`zclassic23 agent`, `zclassic23 healthcheck`, `GET /api/v1`,
`GET /api/v2/agent`, `GET /api/v1/hodl`, `GET /api/v1/factoids`, and
`make soak-evidence-report`. Live DB inspection from the wallet cleanup
confirmed `wallet_sapling_notes.source`, schema migration `021`, and
`idx_snote_view_address`.

The native API simplification pass re-ran `git diff --check`,
`make t ONLY=syncdiag_rpc`, `make t ONLY=api`,
`make t ONLY=make_lint_gates`, `make -j32 build-only`, `make lint`, and full
`make test` (`0/485 groups failed, 14 skipped`). The durable public served-tip
follow-up re-ran `git diff --check`, `make t ONLY=api`,
`make -j32 build-only`, `make lint`, and full `make test`
(`0/485 groups failed, 14 skipped`). The node-owned milestone follow-up re-ran
`git diff --check`, `make -j32 build-only`, `make t ONLY=api`,
`make t ONLY=syncdiag_rpc`, `make t ONLY=mcp_controllers`, and
`make t ONLY=make_lint_gates`, `make check-doc-accuracy`, `make lint`, and full
`make test` (`0/485 groups failed, 15 skipped`). It intentionally did not add
or depend on a new helper binary or shell script.

The reducer-frontier refusal hardening pass re-ran
`make t ONLY=reducer_frontier_reconcile_light`,
`make t ONLY=stage_repair_coin_backfill`,
`make t ONLY=stage_repair_script_refill`, `make lint`,
`make -j32 build-only`, full `make test` (`0/485 groups failed, 14 skipped`),
and `make sim-fast` (`64` seeded replays). It was committed and pushed as
`3347f42ab`, then deployed with `make deploy` (`Deployed + RPC live at block
3166321`, build_commit `3347f42ab`). Post-deploy live checks: `zclassic23
agent` healthy/serving at served height 3166321, `zclassic23 milestone` systems
`[##########]`, goals `[#####-----]`, subgoals `[########--]`; soak RPC showed
build `3347f42ab`, `healthy=false`, `serving=false`, `sync=blocks_download`,
`operator_needed=true`, and `last_reconcile_repaired=false` for the
owner-refused coin-backfill blocker.

The copy-proof climb-gate pass re-ran `sh -n tools/repro_on_copy.sh`,
`make -n repro-on-copy SLUG=soak-refold REPRO_SRC=$HOME/.zclassic-c23-soak
REPRO_FULL=1 CLIMB_PAST=3056758 ARGS='-refold-from-anchor -nobgvalidation
-paramsdir=$$HOME/.zcash-params'`, missing-anchor + light-copy guard checks,
a fake-node harness proving (a) missing verified snapshot-load log fails,
(b) first-observed-above-gate fails, and (c) an observed 42→43 climb with the
verified-load log passes, `git diff --check`, `make -j32 build-only`,
`make check-doc-accuracy`, full `make lint`, full `make test` (`0/485 groups
failed, 14 skipped`), and `make sim-fast` (`64` seeded replays). It was
committed and pushed as `b6ab458f0`, deployed to the main service with
`make deploy`, and installed into the pinned soak binary followed by
`systemctl --user restart zclassic23-soak`.

**Final deploy note.** A post-commit restart exposed a startup-only evidence
lag: `tip_finalize_stage_init()` could publish an existing durable served tip
without stamping the chain-evidence pending tip, leaving health red
(`active_tip_hash_mismatch`) until the next normal block finalized. The startup
path now calls `chain_evidence_note_finalized_tip(existing_tip)`, so the first
health collection drains evidence to the recovered served tip immediately.

**Public status false-alarm cleanup.** Commit `e0885b027` fixed the compact REST
status contract after the startup-evidence deploy, and the current running
build `e72677f96` still includes it: `/api/status` no longer pages operator
action for the normal one-block gap between `served_height` and
`indexed_height`/`header_height`. Regression coverage lives in `test_api`
(`public status treats one-block served gap as healthy` and
`public status still degrades material served gap`).

**Mirror status.** The zclassicd-authoritative mirror is advisory-only right now:
`mirror_running=true`, transport reachable, but `zclassicd` RPC returns `-28`
(`Activating best chain... height 0 (1%)`). Native authority remains
`local_consensus_validation`; no unsafe overrides are active.

## PREVIOUS STATE (2026-06-29, live verified)

**Live node is healthy and at tip after two 2026-06-29 wedge cures.** The
commitment-audit false positive first unwedged H\*=3164075 → 3164482. The deeper
3164483 coin-hole was then fixed, copy-proven, deployed to `zclassic23.service`,
and live-verified: `coin_backfill` inserted the missing 3164371 outpoint,
`reducer_frontier_reconcile_light` cleared, service state became healthy,
`healthcheck` reported `sync_state=at_tip`, and public RPC reported
`getblockcount=3164694` / H\*=3164694 during the verification sweep. Verify fresh:
`zcl_status`, then `zcl_state subsystem=reducer_frontier` and
`zcl_state subsystem=coin_backfill`.

**Commitment-audit false-positive cure.** A UTXO-commitment audit (a heuristic
over the *rebuildable `utxos` projection*) was raising a permanent
`chain_linkage` HOLD that rolled back a PoW-proven `tip_finalize` (`JOB_FATAL` →
txn rollback → "missing-success-row"), with its owner condition stuck on a
`COND_REMEDY_FAILED` stub. The cure decouples the audit from consensus finalize,
self-heals the checkpoint on the growth path + clears the latch, gates the
diagnostic behind 2 consecutive verdicts (kills the mirror-rebuild race), and
gives it an auto-terminating owner. Full write-up:
[`docs/work/commitment-audit-wedge-cure-2026-06-29.md`](work/commitment-audit-wedge-cure-2026-06-29.md)
+ review [`docs/work/commit-audit-cure-review-2026-06-29.md`](work/commit-audit-cure-review-2026-06-29.md).

**3164483 coin-hole cure.** Block 3164483 spent a coin missing from `coins_kv`
and the old `coin_backfill` scan could persist a terminal refusal before proving
terminal linkage to the hole hash. The fix makes the no-spend scan bind terminal
lineage before `SPENT_FOUND`, treats `node.db` txindex as a hint with a bounded
active-chain fallback, re-proves legacy unversioned refusal markers, and removes
the delta horizon as a refusal boundary. Full write-up:
[`docs/work/coin-hole-3164483-next-2026-06-29.md`](work/coin-hole-3164483-next-2026-06-29.md).

**Reductions in the commitment-audit cure (for the next reviewer):** the audit is
now a non-fatal diagnostic, not a consensus gate — real coin-set integrity rests on
the per-block fold ok-verdict + the SHA3 full-set commitment (untouched). The review
doc flags follow-ups (stale comments to fix, the growth-path checkpoint write shares
the unserialized node.db connection — benign/self-healing, and the audit is now a
transient-stability check rather than a durable-baseline detector).

**Bootstrap is still a borrowed-but-consensus-bound stopgap.** The node seeds
`coins_kv` from a near-tip SHA3 snapshot (`utxo-seed-3155842.snapshot`,
starterpack-3155842) whose anchor hash is bound to the in-binary PoW header, then
folds forward. The snapshot's UTXO *content* is not yet independently re-derived
from genesis — the sovereign self-mint anchor below remains the cure that deletes
the borrowed seed path.

**Remaining in-flight work — the sovereign self-mint anchor.** Goal: self-mint a
from-genesis SHA3 anchor at a compiled checkpoint, cut over via
`-refold-from-anchor`, and DELETE the borrowed near-tip loader. Code lives in
`tools/mint_v2_snapshot.c` (auto-pick seed height + Sapling-frontier reuse so it
can mint from a blocks-pruned source) and `lib/test/src/test_self_folded_anchor.c`
(re-derive == baked checkpoint; a mutated coin is detectable). `mint_v2_snapshot`
is a standalone `BUILD_NODE_TOOL` target (not in default `all`); the anchor
fixture runs under `build/bin/test_zcl`, not `test-parallel`. Plan of record:
[`docs/work/sticky-node-plan.md`](work/sticky-node-plan.md) (sovereign-cure spine:
[`docs/work/self-verified-tip-plan.md`](work/self-verified-tip-plan.md)).

**Do NOT re-chase the cured wedges.** Verify the live node first — a doc can be stale, the
node cannot.

# HANDOFF — read this first

**Restart command:** type **`continue zclassic23 development`**.
Verify HEAD with `git status --short --branch`; detect your worktree with `pwd`
(`main` = orchestrator; `wt2`/`wt3` = workers — `docs/work/README.md`).

## Recent history

`git log --oneline -20` is the authoritative recent record. The most recent
landed work is the **commitment-audit wedge cure** (see **CURRENT STATE** above);
before it, the forward-sync never-wedge cure (`75f35dcaf` + the deadlock/de-latch
commits) and the explorer refresh (`90539edd4`). The dated sections below this line
are historical context — trust `git log` and the live node over any of them.

---

## 0. Meta-discipline (read before you trust ANY claim, including this file)

Trust ONLY what you re-derive from the code THIS minute + the live node. Every
"FIXED/cured/cleared" in older docs is UNPROVEN until you re-read the cited
file:line. Be terse with the owner (3-6 lines). less-is-more. Verify live state
with `zcl_status` — never assume "synced"/"wedged" from a doc.

---

## 1. Where things stand (2026-06-23) — wedge FIXED, node reaches tip

The forward-sync **wedge class is FIXED**. The node runs a **consolidated
daily-driver binary** and **reaches and HOLDS the real network tip** — verified
live (`getblockcount` ~3,156,986, climbing; `verificationprogress=1`; the former
wedge block 3,156,171 is now active). It does so via a
**borrowed-but-consensus-bound STOPGAP**: it trusts the reference node's UTXO set
at a near-tip height (provenance proven, anchor hash bound to the in-binary
PoW header), instead of folding that set from its own checkpoint. It is a
**labeled, time-boxed bridge — not the cure.**

Six proven, consensus-audited fixes make up the consolidated binary:

1. **loader-owns-seed** — `-load-snapshot-at-own-height` re-seeds `coins_kv` from
   a checkpoint-OR-anchor-bound snapshot and raises the reducer trusted base to
   the seed height; a `loader_owns_seed` gate stops the torn-import auto-arm from
   re-seeding off the compiled checkpoint (which had re-pinned H\*).
2. **header-sync forward-only `pindex_best_header`** (`chain_state_service.c`) —
   rejects an equal-work lower-height tip so the best-header pointer can't
   regress; matches zclassicd `pindexBestHeader`, reorg-safe.
3. **stopgap consensus cross-check + Sapling rebuild** — the snapshot's
   `anchor_block_hash` must equal the in-binary PoW header at the seed height
   (FATAL on mismatch; validated live as a match), and the Sapling
   note-commitment tree is rebuilt at the seed.
4. **wallet-persistence ordering** — `node.db` is opened before the wallet block
   so keys/shielded notes persist across restart (was a RAM-only-keys boot bug).
5. **fold-speedup** — a block-parse cache (dedup the repeated per-block parse) +
   `coins_kv` batch apply. Proven **bit-identical**
   (`test_reducer_forward_progress_gate`).
6. **snapshot-above-coins-best window extension** (`ab512d577`) — when the
   snapshot seed height is ABOVE coins-best, the loader widens the active-chain
   window forward to `pindex_best_header` (`active_chain_extend_window`,
   `boot_refold_staged.c:702`) so the consensus anchor-hash cross-check reads the
   real PoW-proven `block_index` at the seed height, instead of FATAL-ing
   "Run --importblockindex". A pprev gap still leaves the slot NULL and the
   downstream FATAL still fires (a forged/missing anchor fails closed). This is
   the fix that actually unwedged the live node: a COMPLETE SHA3-verified seed at
   h=3,156,809 (`utxo-seed-3156809.snapshot`, count 1,344,918, sha3
   `040bde49…692bd125`, block_hash `00000933f7…b039ea074`) sits above the height
   that wedged the older torn 3,151,901 seed, and folds forward without
   re-touching it.

**Resilience posture (red-team verdict 2026-06-22, `docs/work/recovery-selfheal-redteam-2026-06-21.md`):**
the deployed config self-heals UNATTENDED from **3 of 4** corruption classes —
chainstate-corruption (→ `-reindex-chainstate`, confirmed live ~390 blk/s),
kill-9 mid-fold (durable per-block SQLite txn, recovers to tip bit-identical —
but SLOW ~16 min because every boot re-replays the Sapling tree), and
supervisor restart (`Restart=always`, never burns out). A missing/forged anchor
snapshot DECLINES honestly (`operator_needed`, never serves wrong data). cgroup-OOM
is PROVEN survivable (a runaway hits its own cgroup cap, the box survives, the node
restarts). **Two P1 gaps remain:** (a) a corrupted `coins_kv`/`progress.kv`
authority store CRASH-LOOPS under the loader flag (FATALs each boot instead of
dropping+reseeding); (b) recovery is slow (Sapling replay every boot — should
persist + load a cached tree). See backlog P1-6/P1-7.

**Validation posture:** the staged forward-fold `proof_validate` (shielded-proof
verification on the fold) is **NOT** gated by `-nobgvalidation`. `-nobgvalidation`
only skips the from-genesis re-verify of borrowed history (the RAM-heavy
`bg_validation_service`). Enabling full forward-fold proof verification RAM-safely
without the genesis re-verify is open work (see the backlog, P0-1).

---

## 2. The wedge class — the exact mechanism (keep this; it grounds the cure)

`reducer_frontier_compute_hstar` (`app/jobs/src/reducer_frontier.c`) defines the
**provable tip H\*** = MIN over the success-logs' **contiguous `ok=1` prefix from
a trusted anchor** (validate_headers / body_fetch / body_persist /
script_validate / proof_validate / utxo_apply / tip_finalize). A missing or
`ok=0` row **terminates the prefix**, so H\* stops one short of the first hole and
**refuses to advance** — it cannot serve a height it cannot fold-prove. Keep this
core; it is the SELECT-only honesty engine.

The trusted anchor floor starts at the compiled SHA3 checkpoint and only RAISES
to a declared base height if every upstream stage cursor is at/above it.

**Why seeding a high UTXO set wedged:** copying a coin set in at height H (cold
import or snapshot) gives the coins but writes **no per-height `ok=1` log rows**
for `[anchor+1 .. H]`. The contiguous prefix from the compiled anchor hits a hole
at anchor+1 and H\* pins there, even though `coins_applied_height` is far higher
("the borrowed-seed hole"). zclassicd cannot represent this: it has ONE UTXO
writer = ONE cursor = the same object, advanced one block at a time, and refuses
any block whose parent ≠ the coins best-block — a hole below the cursor is
structurally impossible.

Two ways to make H\* climb: **(a)** fold forward from a trusted anchor, writing
real `ok=1` rows for every height (the sovereign cure); **(b)** raise the trusted
base to the seed height so the prefix starts there (the stopgap) — sound **only
if the seed is verified**, which the consensus cross-check (fix #3) now enforces.

---

## 3. The sovereign cure (the North Star) — in flight

The stopgap trades sovereignty for "working today." The real fix makes the node
derive every UTXO by folding from its OWN cryptographic checkpoint, so it never
serves an unproven value:

```
mint genesis→compiled-checkpoint (verified anchor)
  → -refold-from-anchor (proven to climb H* anchor→tip, zero decoupled-authority walls)
  → live cutover
  → retroactively validate the borrowed near-tip snapshot
  → make -refold-from-anchor the DEFAULT + delete the borrowed-seed machinery (~715 LOC)
```

Proven-to-climb evidence: the refold reset hard-asserts commitment+count vs the
checkpoint (FATAL on mismatch), the fold fills all bounding logs contiguously
over on-disk bodies, and H\* = MIN climbs (code-read high-confidence + a regtest
fixture where H\* climbed after a reseed + forward fold).

The bottleneck is the **verified anchor**: the only place a UTXO set can be
cryptographically trusted is the compiled checkpoint, and rolling the near-tip
snapshot back to it is impossible (the reference node's durable block+undo index
ends below the snapshot height). So the **genesis→checkpoint mint** is the only
sound source; the fold-speedup work targets making it tractable.

---

## 4. The backlog (P0/P1/P2) — what to do next

| Pri | Item | State |
|---|---|---|
| **P0-1** | Full validation (drop `-nobgvalidation`) RAM-safely — enable forward-fold shielded-proof verify WITHOUT the genesis re-verify | in flight |
| **P0-2** | The sovereign anchor — genesis→checkpoint mint + fold-speedup | in flight |
| **P0-3** | Sovereign cutover (gated on P0-2): refold-from-anchor on a copy → gate on H\* climb → live cutover → retroactively validate the near-tip snapshot. Re-wedge guard: clamp the resume target to the on-disk body ceiling, not active_chain_height | designed |
| **P1-1** | Sapling caveats: `sapling_tree_rebuild` logs a root MISMATCH but does NOT fail; the resume path accepts an unverified checkpoint when `hashFinalSaplingRoot==0` at a 100k boundary | **LANDED 2026-06-24** (`7ffb3be68` fail-closed rebuilds + `a3fe506a3` checkpoint-binding tests) |
| **P1-2** | Boot-reindex always-terminates gap: a clean/reconcilable mid-replay reindex failure can loop without advancing the budget | **LANDED** (`186eb74e4` persists terminal exhaustion; `test_boot_reindex_terminates`) |
| **P1-3** | OOM durable guard: add a lint/runtime check that the systemd `MemoryMax` cap-sum < physical RAM (a global OOM once killed every node). **Cap re-budget (108G→60G) is APPLIED without root** via user-cgroup drop-ins + linger; only the optional swappiness + negative `oom_score_adj` would need sudo and are not load-bearing. | **LANDED 2026-06-24** (`23a75e868` `check-systemd-memory-budget` in `make lint`) |
| **P1-4** | Persistent .onion + onion-seed `/directory.json` discovery (the .onion regenerates every boot) | in flight |
| **P1-5** | Cold-start <10min (MVP C3): the single-binary snapshot import sets `coins_best_block` but does NOT seed `coins_kv`/`coins_applied_height`, so a snapshot cold-boot stalls (reducer L1 refuses). Banked partial C3: coins_kv seed works but needs header/block-index seeding too | in flight |
| **P1-6** | Corrupted-authority crash-loop: a torn `coins_kv`/`progress.kv` authority store FATALs each boot under the loader flag instead of dropping+reseeding (red-team gap) | open |
| **P1-7** | Slow recovery: every boot re-replays the Sapling note-commitment tree (~16 min). Persist + load a cached Sapling tree so a clean restart doesn't pay the full replay | open |
| **P2-1** | MVP gate (CLAUDE.md #1 priority) — live scorer `tools/mvp_gate.sh`; hermetic `make ci-mvp-gates` / `make mvp-verify` | open |
| **P2-2** | File market end-to-end (MVP C5) — `zmarket_buy` parks and `root_hash` is a `path:size` placeholder. The store-side payment hardening is no longer the blocker: the synthetic `zs1_pay_<time>` fallback is removed, live store reconcile is memo-bound, and `store_e2e_shielded` proves real ivk-decrypt + memo-bound credit hermetically. Remaining C5 work is a full live buyer/file-transfer proof. | open |
| **P2-3** | The permanent subtraction (~715 LOC): once a verified anchor is guaranteed reachable on every cold-start, make `-refold-from-anchor` the DEFAULT and delete the borrowed-seed machinery. Re-derive the delete-set from the CURRENT tree. KEEP the 5 legit `tip_finalize_stage_seed_anchor` callers. Parity-safe | gated on P0-2/P0-3 |

The wedge-class root-fix design (the fold-forward cure, the per-100-block UTXO
ladder/keystone, the never-stuck hardening map) is
[`docs/work/never-stuck-plan.md`](./work/never-stuck-plan.md); the ordered
subtraction worksheet is
[`docs/work/sync-fix-plan-2026-06-21.md`](./work/sync-fix-plan-2026-06-21.md);
the trustless-fast-sync substrate is
[`docs/work/sync-keystone.md`](./work/sync-keystone.md).

---

## 5. Features: real vs stub (verify before presenting)

**REAL & chain-derived (keep):** wallet, explorer (+projections), ZNAM,
embedded Tor, MCP/RPC, P2P ping/latency.

**STUBS (don't present as working):**
- **Atomic swaps** — builds HTLC script + P2SH + DB row; no funding broadcast,
  no chain monitoring, no settlement.
- **File market** — offer cache + P2P serialize exist; `root_hash` is a SHA3 of
  `path:size` PLACEHOLDER; `zmarket_buy` parks forever; no transfer/payment.
- **On-chain ZMSG** — persists a record labeled ONCHAIN; no Sapling memo
  build/broadcast/scan. (Off-chain P2P ZMSG is real, plaintext on wire.)
- **TicTacToe** — correct logic + tests, but no P2P wiring; not playable.

---

## 6. Live ops state & lanes

| Lane | Datadir | Deploy |
|---|---|---|
| **live** | `$HOME/.zclassic-c23` | `make deploy` (owner-gated) |
| **dev** | `$HOME/.zclassic-c23-dev` | `make deploy-dev` |
| **soak** | `$HOME/.zclassic-c23-soak` | deliberate re-baseline |

The live datadir runs `-load-snapshot-at-own-height` re-seeding 3,156,809 and
folding forward, so each restart is a ~13 min self-healing boot (cold `node.db`
read + Sapling tree rebuild + fold); a plain boot serving the persisted
`coins_kv` is likely faster but is NOT yet proven live.

`zclassicd` (the C++ reference) runs as a co-located service — **never stop it.**
The default ports and the operator runbook specifics live in `docs/SYNC.md` and
the dev notes; re-pull live state yourself (`zcl_status`, `zcl_state`) before
acting.

**Standing method:** `make deploy` rm's the binary first (the binary rule isn't
depfile-tracked; a stale binary was a real multi-day outage; `deploy_verify.sh`
confirms `build_commit`). Copy-prove every recovery path on a COPY before live,
never live surgery; gate on **H\* CLIMB**, not "booted without FATAL." Never
weaken a safety/operator gate. Gate every change: `make` + `make lint` +
`make test_parallel` (read the `N passed, M failed` line, not the pipe exit).
Replay any consensus-predicate tightening against REAL history first (the
h=478544 lesson — `docs/CONSENSUS_PARITY_DOCTRINE.md`).

---

## 7. MVP status

**MRS 4/8** (do NOT bump without proof): **C1** single-binary install, **C2** Tor
onion <60s, **C4** receive shielded, **C7** kill-9 recovery all pass their local
operator proof. **C3** cold-start (<10min), **C5** file market, **C6** 7-day soak,
**C8** exact parity are gated on the **sovereign foundation + accumulated soak
time**. Soak hours can now ACCRUE because the node reaches and holds tip, but
the current formal C6 judge is `NOT_MET` (`operator_intervention_detected_x2`);
the remaining gate is a fresh clean soak window plus the sovereign cure, not
un-wedging.
The v1 contract is [`docs/MVP.md`](./MVP.md); THE plan is
[`docs/work/FORWARD_PLAN.md`](./work/FORWARD_PLAN.md). Live scorer:
`tools/mvp_gate.sh`.

---

## 8. Verify before you trust this file

A map, not the territory. Before building on any claim, re-read the cited
file:line — code moves and old narratives recur. If a claim doesn't match the
code you read THIS minute, trust the code. Architecture reference:
[`docs/FRAMEWORK.md`](./FRAMEWORK.md) + [`docs/REFACTOR_STATUS.md`](./REFACTOR_STATUS.md).

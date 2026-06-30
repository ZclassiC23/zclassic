# Code quality audit — 2026-06-30

## Scope reality

The repository is about 526k lines across 1,725 C/header files. A literal
line-by-line review belongs in repeated bounded passes, not one churn-heavy
mega-refactor. This page is the running backlog for those passes.

## Running checklist

- [x] Live node health checked before code review work.
- [x] False unhealthy-at-tip health semantics reviewed, fixed, and tested.
- [x] Coin-backfill retry/refusal marker surface reviewed, DRYed, and tested.
- [x] Block-serving peer definition reviewed, tightened, and tested.
- [x] `/api/health` runtime JSON escaping reviewed, fixed, and tested.
- [x] Operator mirror/status helper reviewed and fixed to target the live
  service datadir/port when no explicit RPC env is supplied.
- [x] Split health output into explicit serving status vs warning observations.
- [x] Review the reducer-frontier coin-backfill/refusal-marker seam against the
  post-refold deletion plan.
- [x] Wire and test checkpoint-root/self-folded provenance marking for the
  verified `-refold-from-anchor` minted-snapshot path.
- [x] Gate explicit `-refold-from-anchor` reset on contiguous anchor-to-tip
  block bodies before any `coins_kv` reset.
- [x] Extend retro-validation coverage so G-SOV parts 2+3 are asserted at the
  climbed H* tip, not just immediately after the anchor reseed.
- [x] Run sidecar reviews on the refold provenance surface and zclassicd oracle
  failure surface; triage findings into fixed items and the next backlog seam.
- [x] Review zclassicd oracle failure reporting and split process/RPC/oracle
  usability in operator status surfaces.
- [x] Centralize reducer-frontier reconcile-result evidence predicates so memo,
  gate-loudness, and post-refold shrink boundaries share one contract.
- [ ] Continue reducer-frontier repair helper review for post-refold
  deletion/shrinkage.
- [ ] Re-home the retained reducer cursor-refill helpers before deleting
  tear-only refill/backfill repair files after the sovereign cure.
- [x] Add a guard or doc refresh for the remaining `stage_repair_coin_backfill`
  production caller/torn-gate coupling before the post-cure delete pass.
- [x] Separate tear-only reducer-frontier pre-refusal helpers from the retained
  cursor-refill implementation before the post-cure delete pass.
- [x] Refresh the post-cure deletion manifest for the remaining coin-backfill
  torn-gate and diagnostics coupling.
- [x] Harden retained reducer-frontier refill/purge paths so cursor-refill
  failures are logged and tip-finalize residue replacement is status-whitelisted.
- [x] Review health/source-selection unknown-evidence handling so stale
  `SYNC_AT_TIP`, unknown reducer evidence, and invalid lag cannot look healthy.
- [x] Expose reducer-frontier script/proof/hash-split refill evidence in the
  reconcile-light condition detail snapshot and tests.
- [x] Snapshot active-chain canonical hashes before noncanonical
  reducer-frontier purge deletes progress-log rows.
- [x] DRY reducer-frontier one-shot repair markers and harden stale tipfin
  witness/progress-meta handling.
- [x] Split retained reducer-frontier stale script/proof/hash-split verdict
  replay out of `_coin.c` and add both-stale hash-split classifier coverage.
- [x] Pin reducer-frontier replay dispatcher ordering so lower stale-script
  holes keep ownership over higher script-side hash splits.
- [x] Add readable-block production fixtures for stale script and proof replay.
- [x] Fix deploy verification so it installs and verifies the live service
  binary/datadir/port.
- [x] Harden deploy verification against restart-time cookie races, nested JSON
  false-greens, and single-RPC hangs.
- [x] Fix deploy verification's `zcl-rpc` fallback so it uses the client's env
  contract, unwraps JSON-RPC envelopes, and reports finalized `log_head`.
- [x] Teach deploy verification to name pre-RPC crash-only chainstate reindex
  progress instead of timing out with only a socket error.
- [x] Fail closed on unknown-height chain sources before source selection.
- [x] Align outbound peer-floor counting with block-serving `NODE_NETWORK`
  peers.
- [x] Remove stale zclassicd-authority wording from operator recovery tool
  descriptions and source comments.
- [x] Decide whether to repair/restart the C++ `zclassicd` oracle or leave it as
  an advisory-only unavailable dependency while native P2P remains healthy.
- [x] Start oversized factoids-view shrinkage with a behavior-preserving
  checkpoint-row extraction and regression test.
- [x] Continue oversized factoids-view shrinkage with a behavior-preserving
  data-integrity section extraction and regression test.
- [x] Fix stale `download_queue_starved` warn condition so reaching
  `SYNC_AT_TIP` clears the download-only symptom instead of blocking deploy
  health.
- [x] Continue oversized factoids-view shrinkage with a behavior-preserving
  difficulty section extraction and regression test.
- [x] Continue oversized factoids-view shrinkage with a behavior-preserving
  empty-block section extraction and regression test.
- [x] Clear the factoids chain-data file-size warning with behavior-preserving
  blocktime/transaction section extractions and regression tests.
- [x] Clear the factoids history file-size warning with behavior-preserving
  records/address section extractions and regression tests.
- [x] Continue oversized-file review with a behavior-preserving diagnostics
  block-index/header-band extraction and diagnostics controller regression
  test.
- [x] Continue oversized-file review with a behavior-preserving
  proof-validation dumpstate extraction and stage regression test.
- [x] Continue oversized-file review with a behavior-preserving boot-worker
  supervisor helper extraction and supervisor regression test.
- [x] Continue oversized-file review with a behavior-preserving chain-restore
  backing extraction and seed-anchor provenance regression test.
- [x] Continue oversized-file review with a behavior-preserving
  validate-headers worker-pool extraction and stage regression test.
- [x] Continue oversized-file review with a behavior-preserving
  script-validation dumpstate/prevout extraction and stage regression test.
- [x] Continue oversized-file review with a behavior-preserving
  reducer-frontier reader extraction and frontier/coins-best regressions.
- [x] Continue oversized-file review with a behavior-preserving chain-state
  result-name extraction and repository regression test.
- [x] Continue oversized-file review with a behavior-preserving wallet read
  helper extraction and wallet model regressions.
- [x] Continue oversized-file review with a behavior-preserving validation-pack
  state/dump extraction and sentinel regressions.
- [x] Continue oversized-file review with a behavior-preserving coin-backfill
  creator-proof extraction and repair regressions.
- [x] Continue oversized-file review with a behavior-preserving utxo-apply
  live-lookup extraction and stage regression test.
- [x] Fix header-admit forward-fork replay so a stale `active_tip+1` parent row
  rewinds header/validation/body cursors before the reducer idles indefinitely.
- [x] Let header-admit replay canonical best-header ancestors above the active
  body window after a forward-fork rewind, so stale forward rows are overwritten
  by audited headers instead of waiting on body-window extension; guard the
  fallback so fork-mismatched best-header children hold at the replay cursor
  instead of re-entering the same rewind loop.
- [x] Guard tip-finalize best-header lookahead so a fork-mismatched header child
  holds cleanly instead of writing stale `reorg_detected` residue that caps H*.
- [x] Teach `dumpstate block_index <height>` to resolve best-header ancestors
  above the finalized active-chain window and report the lookup source.
- [x] Teach `tip_fork_stale` to name same-height stale active-tip siblings
  using best-header ancestry and hash comparisons, with a direct condition
  regression.
- [x] Harden `header_admit` best-header replay so above-tip fallback requires a
  matching active parent or prior admitted-header hash, preventing stale rows
  from re-advancing the forward-fork cursor loop.
- [x] Add a native P2P fallback for `tip_fork_stale` when the zclassicd
  rebuild oracle is unavailable: after the stale fork target is invalidated,
  queue the best-header ancestor body at the proven stale height so the node
  fetches the missing canonical parent instead of repeatedly naming tip+1.
- [x] Continue oversized-file review with a behavior-preserving
  `utxo_apply_stage` observability extraction: reject dedup, upstream-hole
  warning state, label-splice refusal logging, and fold-progress heartbeat now
  live in a sibling helper, bringing `utxo_apply_stage.c` below the advisory
  ceiling without changing the coin-authoring path.
- [x] Continue oversized-file review with a behavior-preserving
  `tip_finalize_stage` observability extraction: counters, blocked-reason
  names, throttled warn state, dumpstate serialization, and the published
  served-tip snapshot now live in a sibling helper; durable-tip restore/read
  helpers live in a second sibling helper, leaving the stage file focused on
  finalization decisions and durable reducer writes.
- [x] Continue oversized-file review with a behavior-preserving
  `reducer_frontier_replay` transaction-mechanics extraction: replay SQL probes,
  created-output backfill, inverse-delta checks, log deletion, dry-run setup,
  and script/proof replay transactions now live in a sibling helper, leaving
  the original file focused on repair detection, routing, and blocker naming.
- [x] Continue oversized-file review with a behavior-preserving
  `reducer_frontier_reconcile_light` observability extraction: at-detect
  baselines, progress-meta readers, reconcile-result detail JSON,
  tear-bypass/remedy counters, and testing reset support now live in
  `reducer_frontier_light_observe.*`, leaving the condition file focused on
  detect/remedy/witness/progress decisions.
- [x] Continue oversized-file review with a behavior-preserving boot snapshot
  failure-memory extraction: autodetect/explicit seed `.failed` marker policy
  now lives in `boot_snapshot_failure_memory.*`, with direct regressions for
  marker write, prior-marker skip, proven-authority skip, and fail-closed
  no-marker behavior.
- [x] Continue oversized-file review with a behavior-preserving boot
  postmortem extraction: boot-owned crash-capsule directory, seed-tape event,
  restart compression, and pruning policy now live in
  `boot_postmortem.*`, keeping `boot.c` focused on phase ordering.
- [x] Continue oversized-file review with a behavior-preserving boot datadir
  lock extraction: PID lock acquire/release now live in
  `boot_datadir_lock.*`, with regressions for current-PID refusal, stale PID
  overwrite, release cleanup, and invalid datadir guards.
- [x] Continue oversized-file review with a behavior-preserving boot shutdown
  marker extraction: clean/unclean shutdown marker detect/write policy now
  lives in `boot_shutdown_marker.*`, with regressions for WAL crash detection,
  clean marker consumption, first boot, timestamp write, and invalid datadir
  guards.
- [x] Continue oversized-file review with a behavior-preserving boot stale-lock
  preflight extraction: LevelDB/chainstate LOCK hygiene and SQLite WAL notice
  now live in `boot_stale_locks.*`, with regressions for stale PID removal,
  live PID retention, WAL detection, and invalid datadir guards.
- [ ] Continue oversized-file review with only behavior-preserving extractions.
- [ ] Continue sovereign `-refold-from-anchor` cure work so borrowed-seed repair
  ladders can be removed.

## Live baseline

- `zclassic23.service` is running.
- Public RPC height during this audit: 3164716 before the patch; 3164726
  after deploy/restart verification.
- `sync_state=at_tip`.
- `condition_engine.active_count=0`.
- `healthcheck.healthy=false` was explained only by `degraded_reason=tip_stale`;
  no reducer, peer, validation, queue, or operator-needed blocker was active.
- After the health-semantics patch, `healthcheck.healthy=true`, peer count is
  4, tip lag is 0, and no condition-engine item is active.
- `./tools/z mirror --json` reports `reachable=false`, `mirror_running=false`,
  `activation_blocker=rpc-unreachable`, and
  `consensus_authority=local_consensus_validation`. The C++ `zclassicd`
  service process is running, but its RPC currently returns
  `Activating best chain... height 0 (1%)`; treat it as an unavailable oracle
  until repaired or restarted by operator choice.
- Continuation sample after the operator-helper fix:
  `./tools/z mirror --json` reports `zclassic23_height=3164787`,
  `local_height=3164787`, `best_header_height=3164787`,
  `mirror_running=true`, `reachable=false`, and
  `activation_blocker=rpc-unreachable`. `./tools/z status` reports
  `healthy=true`, `sync_state=at_tip`, `condition_engine.active_count=0`,
  and `tip_lag=0`.
- Continuation sample before the health-split pass:
  `./tools/z mirror --json` reports `zclassic23_height=3164791`,
  `local_height=3164791`, `best_header_height=3164791`,
  `mirror_running=true`, `reachable=false`, and
  `activation_blocker=rpc-unreachable`. `./tools/z status` reports
  `healthy=true`, `sync_state=at_tip`, `condition_engine.active_count=0`,
  and `tip_lag=0`.
- Continuation sample after the repair-marker contract pass:
  `./tools/z mirror --json` reports `zclassic23_height=3164796`,
  `mirror_running=true`, `reachable=false`,
  `consensus_authority=local_consensus_validation`, and
  `activation_blocker=rpc-unreachable`. `./tools/z status` reports
  `healthy=true` and `sync_state=at_tip`.
- Continuation sample after the refold provenance/span-gate pass:
  `./tools/z mirror --json` reports `zclassic23_height=3164799`,
  `local_height=3164799`, `best_header_height=3164799`,
  `mirror_running=true`, `reachable=false`,
  `consensus_authority=local_consensus_validation`, and
  `activation_blocker=rpc-unreachable`. `./tools/z status` reports
  `healthy=true`, `sync_state=at_tip`, `checks.peer_count=4`,
  `checks.tip_lag=0`, `condition_engine.active_count=0`, and
  `chain_advance.selected_source=p2p`.
- Continuation sample after the oracle-reporting code review (source tree built
  but not deployed): `./tools/z mirror --json` reports
  `zclassic23_height=3164834`, `mirror_running=true`, `reachable=false`,
  `active_error_code=rpc-unreachable`, and
  `active_error_detail="rpc error -28: Activating best chain... height 0 (1%)"`.
  `./tools/z status` reports `healthy=true`, `sync_state=at_tip`,
  `checks.peer_count=4`, `checks.tip_lag=0`, `condition_engine.active_count=0`,
  and `chain_advance.selected_source=p2p`.
- Continuation sample before the reducer-frontier predicate pass (source tree
  built but not deployed): `./tools/z status` reports `healthy=true`,
  `sync_state=at_tip`, `checks.peer_count=5`, `checks.tip_lag=0`,
  `condition_engine.active_count=0`, `local_height=3164835`,
  `best_header_height=3164835`, `log_head=3164835`, and
  `chain_advance.selected_source_trust=native_peer_validated`.
  `./tools/z mirror --json` reports `zclassic23_height=3164835`,
  `mirror_running=true`, `reachable=false`,
  `consensus_authority=local_consensus_validation`,
  `activation_blocker=rpc-unreachable`, and
  `active_error_detail="rpc error -28: Activating best chain... height 0 (1%)"`.
- Post-verification live sample after the reducer-frontier predicate pass
  (source tree built but not deployed): `./tools/z status` reports
  `healthy=true`, `sync_state=at_tip`, `checks.peer_count=4`,
  `checks.tip_lag=0`, `condition_engine.active_count=0`,
  `local_height=3164842`, `best_header_height=3164842`,
  `log_head=3164842`, and `chain_advance.selected_source=p2p`.
  `./tools/z mirror --json` reports `zclassic23_height=3164842`,
  `mirror_running=true`, `reachable=false`,
  `consensus_authority=local_consensus_validation`,
  `activation_blocker=rpc-unreachable`, and
  `active_error_detail="rpc error -28: Activating best chain... height 0 (1%)"`.
- Post-verification live sample after the coin-backfill caller-gate pass
  (source tree built but not deployed): `./tools/z status` reports
  `healthy=true`, `sync_state=at_tip`, `checks.peer_count=5`,
  `checks.tip_lag=0`, `condition_engine.active_count=0`,
  `local_height=3164848`, `best_header_height=3164848`,
  `log_head=3164848`, and `chain_advance.selected_source=p2p`.
  `./tools/z mirror --json` reports `zclassic23_height=3164848`,
  `mirror_running=true`, `reachable=false`,
  `consensus_authority=local_consensus_validation`,
  `activation_blocker=rpc-unreachable`, and
  `active_error_detail="rpc error -28: Activating best chain... height 0 (1%)"`.
- Post-verification live sample after the reducer-frontier pre-refusal/refill
  split (source tree built but not deployed): `./tools/z status` reports
  `healthy=true`, `sync_state=at_tip`, `checks.peer_count=5`,
  `checks.tip_lag=0`, `condition_engine.active_count=0`,
  `local_height=3164853`, `best_header_height=3164853`,
  `log_head=3164853`, and `chain_advance.selected_source=p2p`.
  `./tools/z mirror --json` reports `zclassic23_height=3164853`,
  `mirror_running=true`, `reachable=false`,
  `consensus_authority=local_consensus_validation`,
  `activation_blocker=rpc-unreachable`, and
  `active_error_detail="rpc error -28: Activating best chain... height 0 (1%)"`.
- Post-verification live sample after the post-cure deletion-manifest refresh
  (docs-only; no deploy/restart): `./tools/z status` reports
  `healthy=true`, `sync_state=at_tip`, `checks.peer_count=6`,
  `checks.tip_lag=0`, `condition_engine.active_count=0`,
  `checks.log_head=3164857`, `checks.chain_advance.local_height=3164857`,
  `checks.chain_advance.best_header_height=3164857`, and
  `checks.chain_advance.selected_source=p2p`.
  `./tools/z mirror --json` reports `zclassic23_height=3164857`,
  `lag=0`, `mirror_running=true`, `reachable=false`,
  `consensus_authority=local_consensus_validation`,
  `activation_blocker=rpc-unreachable`, and
  `active_error_detail="rpc error -28: Activating best chain... height 0 (1%)"`.
- Post-verification live sample after the retained reducer repair hardening
  (source tree built/tested but not deployed): `./tools/z status` reports
  `healthy=true`, `sync_state=at_tip`, `checks.peer_count=5`,
  `checks.tip_lag=0`, `condition_engine.active_count=0`,
  `checks.log_head=3164881`, `checks.chain_advance.local_height=3164881`,
  `checks.chain_advance.best_header_height=3164881`, and
  `checks.chain_advance.selected_source=p2p` with
  `selected_source_trust=native_peer_validated`.
  `./tools/z mirror --json` reports `zclassic23_height=3164881`,
  `lag=0`, `mirror_running=true`, `reachable=false`,
  `consensus_authority=local_consensus_validation`,
  `activation_blocker=rpc-unreachable`, and
  `active_error_detail="rpc error -28: Activating best chain... height 0 (1%)"`.
- Post-verification live sample after the health/source-selection evidence pass
  (source tree built/tested but not deployed): `./tools/z status` reports
  `healthy=true`, `sync_state=at_tip`, `checks.peer_count=5`,
  `checks.tip_lag=0`, `condition_engine.active_count=0`,
  `checks.log_head=3164899`, `checks.log_head_gap=-65`, and
  `checks.chain_advance.selected_source=p2p` with
  `selected_source_trust=native_peer_validated`. The negative log-head gap is
  expected when local reducer evidence is ahead of the best peer height; the
  new source health gate requires known peer/log evidence but does not treat a
  negative gap as invalid. `./tools/z mirror --json` reports
  `zclassic23_height=3164899`, `lag_known=false`, `lag_valid=false`,
  `candidate_lag_known=false`, `candidate_lag_valid=false`,
  `mirror_running=true`, `reachable=false`,
  `consensus_authority=local_consensus_validation`,
  `activation_blocker=rpc-unreachable`, `activation_blocker_class=transient`,
  and `active_error_detail="rpc error -28: Activating best chain... height 0 (1%)"`.
- Post-verification live sample after the reducer-frontier detail snapshot pass
  (source tree built/tested but not deployed): `./tools/z status` reports
  `healthy=true`, `sync_state=at_tip`, `checks.peer_count=5`,
  `checks.tip_lag=0`, `condition_engine.active_count=0`,
  `checks.log_head=3164901`, `checks.log_head_gap=-67`, and
  `checks.chain_advance.selected_source=p2p` with
  `selected_source_trust=native_peer_validated`. `./tools/z mirror --json`
  reports `zclassic23_height=3164901`, `lag_known=false`,
  `lag_valid=false`, `mirror_running=true`, `reachable=false`,
  `consensus_authority=local_consensus_validation`,
  `activation_blocker=rpc-unreachable`, `activation_blocker_class=transient`,
  and `active_error_detail="rpc error -28: Activating best chain... height 0 (1%)"`.
- Post-verification live sample after the noncanonical-purge snapshot pass
  (source tree built/tested but not deployed): `./tools/z status` reports
  `healthy=true`, `sync_state=at_tip`, `checks.peer_count=5`,
  `checks.tip_lag=0`, `condition_engine.active_count=0`,
  `checks.log_head=3164911`, `checks.log_head_gap=-77`, and
  `checks.chain_advance.selected_source=p2p` with
  `selected_source_trust=native_peer_validated`. `./tools/z mirror --json`
  reports `zclassic23_height=3164911`, `lag_known=false`,
  `lag_valid=false`, `mirror_running=true`, `reachable=false`,
  `consensus_authority=local_consensus_validation`,
  `activation_blocker=rpc-unreachable`, `activation_blocker_class=transient`,
  and `active_error_detail="rpc error -28: Activating best chain... height 0 (1%)"`.
- Post-verification live sample after the reducer-frontier marker/witness pass
  (source tree built/tested but not deployed): `./tools/z status` reports
  `healthy=true`, `sync_state=at_tip`, `checks.peer_count=5`,
  `checks.tip_lag=0`, `condition_engine.active_count=0`,
  `checks.log_head=3164933`, `checks.log_head_gap=-99`,
  `checks.chain_advance.local_height=3164933`,
  `checks.chain_advance.best_header_height=3164933`,
  `checks.chain_advance.projection_height=3164933`,
  `checks.chain_advance.selected_source=p2p`, and
  `selected_source_trust=native_peer_validated`. `./tools/z mirror --json`
  reports `zclassic23_height=3164933`, `local_height=3164933`,
  `best_header_height=3164933`, `lag_known=false`, `lag_valid=false`,
  `mirror_running=true`, `reachable=false`,
  `consensus_authority=local_consensus_validation`,
  `activation_blocker=rpc-unreachable`,
  `activation_blocker_class=transient`, and
  `active_error_detail="rpc error -28: Activating best chain... height 0 (1%)"`.
- Post-verification live sample after the retained verdict-replay split
  (source tree built/tested but not deployed): `./tools/z status` reports
  `healthy=true`, `sync_state=at_tip`, `checks.peer_count=4`,
  `checks.tip_lag=0`, `condition_engine.active_count=0`,
  `checks.log_head=3164945`, `checks.log_head_gap=-111`,
  `checks.chain_advance.local_height=3164945`,
  `checks.chain_advance.best_header_height=3164945`,
  `checks.chain_advance.projection_height=3164945`,
  `checks.chain_advance.selected_source=p2p`, and
  `selected_source_trust=native_peer_validated`. `./tools/z mirror --json`
  reports `zclassic23_height=3164945`, `local_height=3164945`,
  `best_header_height=3164945`, `lag_known=false`, `lag_valid=false`,
  `mirror_running=true`, `reachable=false`,
  `consensus_authority=local_consensus_validation`,
  `activation_blocker=rpc-unreachable`,
  `activation_blocker_class=transient`, and
  `active_error_detail="rpc error -28: Activating best chain... height 0 (1%)"`.
- Post-verification live sample after the reducer-frontier dispatcher-order pin
  (source tree built/tested but not deployed): `./tools/z status` reports
  `healthy=true`, `sync_state=at_tip`, `checks.peer_count=4`,
  `checks.tip_lag=0`, `condition_engine.active_count=0`,
  `checks.log_head=3164951`, `checks.log_head_gap=-117`,
  `checks.chain_advance.local_height=3164951`,
  `checks.chain_advance.best_header_height=3164951`,
  `checks.chain_advance.projection_height=3164951`,
  `checks.chain_advance.selected_source=p2p`, and
  `selected_source_trust=native_peer_validated`. `./tools/z mirror --json`
  reports `zclassic23_height=3164951`, `local_height=3164951`,
  `best_header_height=3164951`, `lag_known=false`, `lag_valid=false`,
  `mirror_running=true`, `reachable=false`,
  `consensus_authority=local_consensus_validation`,
  `activation_blocker=rpc-unreachable`,
  `activation_blocker_class=transient`, and
  `active_error_detail="rpc error -28: Activating best chain... height 0 (1%)"`.
- Post-verification live sample after the readable replay fixture pass (source
  tree built/tested but not deployed): `./tools/z status` reports
  `healthy=true`, `sync_state=at_tip`, `checks.peer_count=4`,
  `checks.tip_lag=0`, `condition_engine.active_count=0`,
  `checks.log_head=3164960`, `checks.log_head_gap=-126`,
  `checks.chain_advance.local_height=3164960`,
  `checks.chain_advance.best_header_height=3164960`,
  `checks.chain_advance.projection_height=3164960`,
  `checks.chain_advance.selected_source=p2p`, and
  `selected_source_trust=native_peer_validated`. `./tools/z mirror --json`
  reports `zclassic23_height=3164960`, `local_height=3164960`,
  `best_header_height=3164960`, `lag_known=false`, `lag_valid=false`,
  `mirror_running=true`, `reachable=false`,
  `consensus_authority=local_consensus_validation`,
  `activation_blocker=rpc-unreachable`,
  `activation_blocker_class=transient`, and
  `active_error_detail="rpc error -28: Activating best chain... height 0 (1%)"`.
- Post-deploy live sample after commit `29329bffe`: the manually installed
  live binary reports `build_commit=29329bffe`; `./tools/deploy_verify.sh`
  reports `Deployed + RPC live at block 3164990`. `./tools/z status` reports
  `healthy=true`, `serving=true`, `sync_state=at_tip`, `checks.peer_count=5`,
  `checks.tip_lag=0`, `condition_engine.active_count=0`,
  `checks.log_head=3164991`, `checks.chain_advance.local_height=3164991`,
  `checks.chain_advance.best_header_height=3164991`,
  `checks.chain_advance.projection_height=3164991`, and no chain-evidence
  health reason.
- Post-deploy live sample after deploy-verifier hardening commit
  `5b4888096`: `make deploy` rebuilt the binary with
  `ZCL_BUILD_COMMIT="5b4888096"`, installed it to the effective service
  executable `/home/rhett/.local/bin/zclassic23-live`, restarted the service,
  and `tools/deploy_verify.sh` reported
  `Deployed + RPC live at block 3165011 (build_commit 5b4888096)`. A final
  `./tools/z status` sample reports `build_commit=5b4888096`,
  `healthy=true`, `serving=true`, `sync_state=at_tip`, `checks.peer_count=5`,
  `checks.tip_lag=0`, `condition_engine.active_count=0`, `local_height=3165012`,
  `best_header_height=3165012`, `projection_height=3165012`,
  `projection_lag=0`, `log_head=3165012`, `active_tip=3165012`,
  `persisted_active_tip=3165012`, `coins_best_block_height=3165012`, and
  `csr_sqlite_max_height=3165012`. The legacy `zclassicd` mirror remains
  advisory-unavailable (`reachable=false`,
  `consensus_authority=local_consensus_validation`,
  `activation_blocker=rpc-unreachable`,
  RPC error `-28 Activating best chain... height 0 (1%)`), while zclassic23 is
  healthy from native P2P/local consensus evidence.
- Pre-patch live sample for the source-selection/peer-floor wording pass:
  `./tools/z status` reports `build_commit=6e5e60f6a`, `healthy=true`,
  `serving=true`, `sync_state=at_tip`, `checks.peer_count=5`,
  `checks.tip_lag=0`, `condition_engine.active_count=0`,
  `checks.log_head=3165042`, and `chain_advance.selected_source=p2p`.
  `./tools/z mirror --json` reports `zclassic23_height=3165042`,
  `mirror_monitor_running=true`,
  `consensus_authority=local_consensus_validation`, and legacy `zclassicd`
  remains advisory-unusable while its RPC is warming up at height 0.
- Post-deploy live sample after commit `e2db5ca87`: the service binary reports
  `build_commit=e2db5ca87`, RPC responds at height 3165236, and the reducer is
  named-stalled rather than silent: `tip_finalize.cursor=3165236`,
  `utxo_apply.cursor=3165237`, `last_blocked_reason=lookahead_tip_missing`,
  `chain_advance.best_header_height=3165344`, and active conditions include
  `block_failed_mask_at_tip`, `local_header_refill_needed`, and
  `tip_wedged_resnapshot`. The next review seam is the missing-child/body-window
  path at height 3165237.
- Post-deploy live sample after commit `57ce22e78`: `make deploy` installed
  and restarted the service with `build_commit=57ce22e78`, but verifier failed
  health after 600s because the same named stall remains. The new
  `dumpstate block_index` lookup shows height 3165236 resolves from
  `lookup_source=active_chain` with hash
  `00000ba2a2017615a140c648b13540e93115f0edced6fd606e2d34d76b9b1ac7`,
  while height 3165237 resolves only from `lookup_source=best_header_ancestor`
  with `hash_prev=000000c753e149fd16ecb9556ca7ca9ae5d4d36881e4b3db49d8856e6b1fd74d`.
  That above-tip best-header block is therefore not the finalized tip's child;
  `tip_finalize` correctly holds at `lookahead_tip_missing` instead of
  finalizing across a fork.

## Fixed in this pass

1. **False unhealthy at tip**
   - File: `app/services/src/node_health_service.c`
   - Problem: a quiet chain with an old block timestamp set `tip_stale=true`
     and made the whole node unhealthy even when it was synced, peer-connected,
     zero-lag, and condition-clean.
   - Fix: keep reporting `tip_stale` as an observation, but do not fail
     `healthy` solely for block-time staleness. Real non-progress remains
     covered by sync state, header/tip/log gaps, `tip_advance_age` in non-tip
     states, queue pressure, DB stalls, chain evidence, mirror fatality, and
     operator-needed.
   - Test: added a regression case for at-tip + peers + stale block timestamp.

2. **Coin-backfill retryability and marker parsing**
   - Files: `app/jobs/src/stage_repair_coin_backfill.c`,
     `app/jobs/src/stage_repair_coin_backfill_util.{c,h}`,
     `app/services/src/block_index_loader_torn_gate.c`
   - Problem: an unreadable creator block, stale txindex row, or bounded
     active-chain scan could be treated like a terminal refusal. That can
     persist a "never retry" marker for a recoverable body/index gap.
   - Fix: creator body gaps and bounded scan-horizon exhaustion are retryable;
     stale txindex rows are only hints; terminal `txindex_miss:v2` is written
     only after an active-chain proof. Runtime repair and boot torn-gate now
     share one refusal-marker parser with an explicit whitelist, so legacy or
     unknown marker payloads do not silently gate startup.
   - Tests: added stale-txindex fallback and unreadable-creator retry cases.

3. **Block-serving peer definition**
   - Files: `lib/net/src/connman.c`,
     `app/services/src/node_health_service.c`
   - Problem: peer height and health paths could treat handshaked but unusable
     peers as sync-capable.
   - Fix: max peer height, health peer-height sampling, and outbound health
     now require a non-disconnecting peer at `PEER_HANDSHAKE_COMPLETE` or later
     with `NODE_NETWORK`.
   - Tests: added coverage for unhandshaked, disconnecting, and non-network
     peers being ignored.

4. **Health API JSON escaping**
   - File: `app/controllers/src/api_controller_node.c`
   - Problem: `/api/health` formatted runtime strings directly into JSON.
   - Fix: all runtime string fields are JSON-quoted before formatting, with a
     hard overflow check.
   - Test: injected quotes/newlines into the error ring and parsed the health
     response as JSON.

5. **Operator mirror/status target selection**
   - File: `tools/z`
   - Problem: `./tools/z mirror --json` could query the default
     `~/.zclassic-c23` cookie while the linger service was actually running
     `~/.zclassic-c23-fullhist`, producing `zclassic23_height=0` even when the
     live node was healthy at tip.
   - Fix: the wrapper now forwards `-datadir` and `-rpcport` to
     `zclassic-cli`, honors `ZCL_DATADIR`/`ZCL_RPCPORT`, and falls back to the
     running `zclassic23.service` `ExecStart` datadir/port when the default
     cookie is absent.
   - Test: extended the operator-diagnostics contract test and verified the
     live helper reports the actual service height.

6. **Health serving/warning split**
   - Files: `app/services/include/services/node_health_service.h`,
     `app/services/src/node_health_service.c`,
     `app/controllers/src/api_controller_node.c`,
     `app/controllers/src/event_controller.c`,
     `docs/RUNBOOK.md`
   - Problem: `healthy` and `degraded_reason` were doing two jobs: red
     restart/blocker state and yellow observations such as stale block time,
     recent recovered errors, or validation-pack holds.
   - Fix: the snapshot now publishes `serving`, `blocking_reason`,
     `warning_count`, and `warning_reasons` while preserving existing
     `healthy` / `degraded_reason` compatibility. REST `/api/health` and RPC
     `healthcheck` expose the split.
   - Tests: added service assertions for red blockers vs yellow warnings and
     REST JSON assertions for the new fields.

7. **Reducer repair marker contract**
   - Files: `app/jobs/src/stage_repair_coin_backfill_util.h`,
     `lib/test/src/test_stage_repair_coin_backfill.c`
   - Problem: the shared runtime/boot refusal-marker parser was indirectly
     covered by higher-level repair tests, but the exact active-v2 vs
     legacy-reproof vs unknown-ignore contract was not directly pinned. The
     utility header also still described delta horizon as a creator-refusal
     boundary even though the current code treats it as observability only.
   - Fix: added direct decoder-contract assertions for `spent:v2`,
     `txindex_miss:v2`, `unprovable`, `round_cap`, `relost`, legacy `spent`,
     legacy `txindex_miss`, unknown markers, and empty markers; corrected the
     delta-horizon comment to match the current repair semantics.
   - Tests: rebuilt `build/bin/test_parallel`, ran
     `build/bin/test_parallel --only=stage_repair_coin_backfill`, ran
     `git diff --check`, and ran `make lint`.

8. **Checkpoint-root refold provenance and span gate**
   - Files: `config/src/boot_refold_staged.c`,
     `lib/storage/include/storage/coins_kv.h`,
     `config/src/boot.c`, `lib/test/src/test_refold_from_anchor_fatal.c`,
     `lib/test/src/test_refold_auto_arm.c`,
     `lib/test/src/test_refold_retro_validate.c`,
     `lib/test/src/test_make_lint_gates.c`,
     `docs/work/sovereign-cutover-runbook.md`
   - Problem: `coins_kv` had the self-folded provenance predicate and reset
     clearing behavior, but no production `-refold-from-anchor` path set the
     marker after proving a minted snapshot. A successful explicit refold could
     therefore still look borrowed-and-stamped to
     `coins_kv_tip_is_self_derived`. The explicit flag path also skipped the
     body-span preflight used by auto-arm, so it could reset the anchor before
     proving the anchor-to-tip bodies were present.
   - Fix: when `boot_refold_from_anchor_reset` reseeds from the verified minted
     snapshot, it now writes `COINS_KV_SELF_FOLDED_KEY` inside the same Phase 2
     progress transaction that sets `coins_applied_height` and arms the anchor
     cursors. The explicit `-refold-from-anchor` boot path now calls
     `boot_refold_body_span_contiguous` before reset/stamp and emits a named
     body-gap blocker if the span is incomplete. The node.db fallback remains
     unmarked, so borrowed reseeds cannot claim this checkpoint-root-verified
     provenance. Documentation now states the marker proves the loaded set
     equals the compiled checkpoint root; it does not prove which machine
     produced the snapshot file.
   - Tests: rebuilt `build/bin/test_parallel`; ran
     `build/bin/test_parallel --only=make_lint_gates`,
     `build/bin/test_parallel --only=refold_from_anchor_fatal`,
     `--only=refold_auto_arm`, `--only=coins_kv_sovereign_gate`,
     `--only=refold_retro_validate`, and `--only=coins_kv_reset_for_reseed`;
     ran `git diff --check`, `make lint`, and the full
     `build/bin/test_parallel` suite (`0/466` groups failed, 14 self-skipped).

9. **zclassicd oracle failure surface**
   - Files: `lib/rpc/src/legacy_rpc_client.c`,
     `app/services/src/zclassicd_oracle_service.c`,
     `app/services/src/legacy_mirror_sync_state.c`,
     `app/services/src/block_source_policy_runtime.c`,
     `app/controllers/src/event_controller.c`,
     `app/controllers/src/health_controller.c`,
     `lib/test/src/test_zclassicd_oracle.c`,
     `lib/test/src/test_syncdiag_rpc.c`,
     `lib/test/src/test_chain_advance_coordinator.c`,
     `lib/test/src/test_lag_slo.c`,
     `docs/RUNBOOK.md`
   - Problem: the live C++ `zclassicd` service can be process-alive while its
     RPC returns JSON-RPC warmup `-28`. The old status wording collapsed process
     running, RPC transport reachability, and oracle usability into
     `reachable=false` plus a top-level `candidate_blocker=rpc-unreachable`,
     which made a healthy native-P2P node look blocked by an advisory oracle.
   - Fix: JSON-RPC numeric error codes are preserved; the oracle records
     attempts, last attempt/error height and time, last error code/text,
     `rpc_transport_reachable`, `oracle_usable`, and a compatibility
     `reachable` alias. Legacy mirror status now reports
     `mirror_monitor_running`, `zclassicd_rpc_transport_reachable`,
     `legacy_oracle_usable`, `zclassicd_rpc_error_code`, and
     `zclassicd_rpc_error_message`. Chain-source policy no longer authorizes an
     unavailable mirror. `healthcheck`, `getsyncdetail`, and `getservicehealth`
     expose `active_source` separately from `legacy_advisory_blocker`, and scope
     transient mirror RPC failures as `advisory_only` when native P2P is the
     selected source. Permanent safety blockers and unsafe overrides still
     surface.
   - Tests: rebuilt `build/bin/test_parallel`; ran
     `build/bin/test_parallel --only=zclassicd_oracle`,
     `--only=syncdiag_rpc`, `--only=chain_advance_coordinator`, and
     `--only=lag_slo`; ran `git diff --check`, `make lint`, and the full
     `build/bin/test_parallel` suite (`0/466` groups failed, 14 self-skipped).

10. **Reducer-frontier repair-result evidence predicates**
   - Files: `app/jobs/include/jobs/stage_repair.h`,
     `app/jobs/src/stage_repair_reducer_frontier.c`,
     `app/conditions/src/reducer_frontier_reconcile_light.c`,
     `lib/test/src/test_reducer_frontier_reconcile_light.c`
   - Problem: the reducer-frontier job memo cache and condition gate-loudness
     path duplicated overlapping checks for the same reconcile-result fields.
     That made the post-refold delete boundary harder to audit and created risk
     that future repair evidence would be wired into one path but not the other.
   - Fix: added named classifiers for coin-repair evidence, row-residue
     evidence, gate-loudness evidence, and memo-clean results. The job cache and
     condition now use those helpers, preserving the existing separation between
     the coin-tear peer-gate bypass and warning-only gate suppression. Tests pin
     clean, coin-repair, row-residue, coin-tear-bypass, and repaired-result
     behavior.
   - Tests: rebuilt `build/bin/test_parallel`; ran
     `build/bin/test_parallel --only=reducer_frontier_reconcile_light`,
     `--only=stage_repair_tipfin_backfill`,
     `--only=stage_repair_coin_backfill`,
     `--only=reorg_residue_tipfin_replace`,
     `--only=stage_repair_script_refill`,
     `--only=seed_torn_import_gate`, and
     `--only=utxo_activation_paused`; ran targeted `git diff --check` and
     `make lint`.

11. **Coin-backfill repair caller ratchet**
   - Files: `tools/lint/check_no_new_coin_backfill_caller.sh`, `Makefile`,
     `docs/DEFENSIVE_CODING.md`, `lib/test/src/test_make_lint_gates.c`
   - Problem: the public `stage_repair_coin_backfill_try` entry point had only
     convention and review notes keeping it owned by reducer-frontier. A new
     production caller, or a second call in the allowed dispatcher, would widen
     the borrowed-seed repair fabric before the sovereign cure deletes it.
   - Fix: added `check-no-new-coin-backfill-caller` to `make lint`. It requires
     exactly one non-test production call, in
     `app/jobs/src/stage_repair_reducer_frontier_coin.c`, and fails on every
     other production caller. The lint-gate regression plants both a new
     production caller and a duplicate dispatcher call, verifies the gate trips,
     restores the files, and verifies recovery. This pass also added a planted
     regression for the existing borrowed-seed caller gate.
   - Tests: rebuilt `build/bin/test_parallel`; ran
     `make check-no-new-coin-backfill-caller`, `make check-doc-accuracy`,
     `build/bin/test_parallel --only=make_lint_gates`, `git diff --check`, and
     `make lint`.

12. **Reducer-frontier pre-refusal/refill split**
   - Files: `app/jobs/src/stage_repair_reducer_frontier_pre_refusal.c`,
     `app/jobs/src/stage_repair_reducer_frontier_refill.c`,
     `app/jobs/src/stage_repair_reducer_frontier_internal.h`,
     `app/jobs/src/stage_repair_reducer_frontier.c`
   - Problem: `stage_repair_reducer_frontier_refill.c` mixed the retained
     crash-recovery cursor-refill core with tear-only pre-refusal adapters.
     That made the post-refold delete boundary blurry: the code that must
     survive after the borrowed-seed repair ladder is deleted lived beside code
     that exists only to clear a coin-tear refusal before escalation.
   - Fix: moved `stage_reducer_frontier_try_unapplied_hole_clamp` and
     `stage_reducer_frontier_reconcile_validate_hash_split_cursor` into the new
     pre-refusal translation unit. The shared script/proof refill core and
     validate-header hash-split scanner stay in the retained refill file behind
     explicitly named internal helpers. Stale comments that claimed no-tear
     calls WARN were corrected to match the current silent no-op behavior.
     The new file carries the repair-rung ratchet marker and cites the focused
     script-refill regression instead of expanding the grandfathered baseline.
   - Tests: rebuilt `build/bin/test_parallel`; ran
     `build/bin/test_parallel --only=stage_repair_script_refill`,
     `--only=stage_repair_tipfin_backfill`,
     `--only=reducer_frontier_reconcile_light`,
     `--only=validate_script_hash_split_repair`,
     `--only=reorg_residue_tipfin_replace`,
     `make check-no-new-repair-rung`, `git diff --check`, and `make lint`.

13. **Post-cure coin-backfill deletion manifest refresh**
   - Files: `docs/work/architecture-deletion-plan.md`,
     `docs/work/coin-backfill-repair.md`
   - Problem: the old deletion plan still classified
     `stage_repair_reducer_frontier_refill.c` as cleanly deletable and did not
     fully list the current boot/diagnostics couplings that would dangle after
     removing `stage_repair_coin_backfill_util.c`.
   - Fix: re-derived the current delete/keep boundary from the tree. The plan now
     classifies coin-backfill TUs and headers, the boot torn gate, the
     pre-refusal adapter, and coin-backfill diagnostics as post-cure delete
     targets; keeps/re-homes the reducer-frontier refill core and purge
     machinery; calls out `_coin.c` as mixed and needing a split/rewrite before
     deletion; marks tip-finalize row backfill as prune/re-home rather than a
     blanket file delete; and names the current auto-arm, rebuild, registry,
     condition, script-validation pending-prevout producer, and header
     declarations that must be removed in the same cut. The
     coin-backfill repair note now points to that manifest and the
     `check-no-new-coin-backfill-caller` ratchet.
   - Tests: ran targeted current-tree greps, `make check-doc-accuracy`,
     `git diff --check`, and `make lint`.

14. **Reducer-frontier retained repair hardening**
   - Files: `app/jobs/src/stage_repair_reducer_frontier_refill.c`,
     `app/jobs/src/stage_repair_reducer_frontier_purge.c`,
     `lib/test/src/test_reorg_residue_tipfin_replace.c`,
     `tools/lint/silent_bool_errors_baseline.txt`
   - Problem: the retained cursor-refill core still propagated several
     fallible subcalls as bare `false`, leaving the next operator with only the
     caller-level symptom. The stale tip-finalize residue replacement also
     matched every `ok=0` row below `coins_applied`, despite the contract
     limiting that repair to stale `reorg_detected` /
     `utxo_count_diverged` residue; that could rewrite real
     `upstream_failed` or `precondition_failed` blockers into
     `ok=1 finalize_backfill`.
   - Fix: added contextual `LOG_WARN` breadcrumbs around retained refill scans,
     cursor snapshots, presence probes, and cursor-clamp writes; shrank the
     silent-bool ratchet baseline from 105 to 91 tracked call guards; made the
     noncanonical purge probes fail closed on non-`SQLITE_ROW` /
     non-`SQLITE_DONE` step results; and whitelisted tip-finalize replacement
     to only `reorg_detected` / `utxo_count_diverged`. The regression now
     proves `upstream_failed` and `precondition_failed` rows stay present,
     `ok=0`, and unchanged even when the header-admit binder exists.
   - Tests: rebuilt `build/bin/test_parallel`; ran
     `build/bin/test_parallel --only=reorg_residue_tipfin_replace`,
     `build/bin/test_parallel --only=stage_repair_script_refill`,
     `make check-no-new-repair-rung`,
     `make check-no-new-coin-backfill-caller`, `git diff --check`, and
     `./tools/lint/check_silent_bool_errors.sh`; final gates:
     `make check-doc-accuracy` and `make lint`.

15. **Health/source-selection evidence must be known before green**
   - Files: `app/services/src/node_health_service.c`,
     `app/services/include/services/node_health_service.h`,
     `tools/z`, `app/services/src/block_source_policy_runtime.c`,
     `app/services/include/services/block_source_policy.h`,
     `lib/test/src/test_node_health_service.c`,
     `lib/test/src/test_chain_advance_coordinator.c`,
     `lib/test/src/test_zclassicd_oracle.c`,
     `lib/test/src/test_make_lint_gates.c`
   - Problem: a stale `SYNC_AT_TIP` FSM state could remain green despite a
     source-policy blocker/target gap; missing active-tip or reducer-log
     evidence could satisfy the old final health predicate through negative
     sentinel comparisons; `tools/z mirror` collapsed `lag_known` and
     `lag_valid`; and source policy classified mirror hash-disagreement as
     permanent even though mirror consensus keeps it transient until the
     divergence locator proves a confirmed split.
   - Fix: health now demotes stale at-tip state when chain-advance policy names
     a blocker or target gap, requires known active tip, peer height, and
     reducer log-head before reporting healthy, and still permits a negative
     log-head gap when local reducer evidence is ahead of peers. `tools/z`
     tracks lag-known and lag-valid independently in both normal and fallback
     JSON paths. Source policy delegates consensus blocker classification to
     mirror consensus while keeping its local resource/dependency classes.
   - Tests: rebuilt `build/bin/test_parallel`; ran
     `build/bin/test_parallel --only=node_health_service`,
     `build/bin/test_parallel --only=chain_advance_coordinator`,
     `build/bin/test_parallel --only=zclassicd_oracle`,
     `build/bin/test_parallel --only=make_lint_gates`, `git diff --check`,
     and `make lint`.

16. **Reducer-frontier reconcile detail exposes script/proof refill evidence**
   - Files: `app/conditions/src/reducer_frontier_reconcile_light.c`,
     `app/jobs/src/stage_repair_reducer_frontier_pre_refusal.c`,
     `lib/test/src/test_reducer_frontier_reconcile_light.c`,
     `lib/test/src/test_stage_repair_script_refill.c`
   - Problem: the retained cursor-refill path logged script/proof refill holes
     and clamps, but the condition detail snapshot did not expose those fields.
     Operators looking at `zcl_state` / condition detail could see a generic
     reconcile run without knowing whether script/proof refill, proof refill,
     or script-side hash-split evidence was present. The tear-only pre-refusal
     validate-hash-split adapter also still had a couple of silent `false`
     paths.
   - Fix: added `last_reconcile_lowest_script_validate_hash_split`,
     `last_reconcile_lowest_script_validate_refill_hole`,
     `last_reconcile_lowest_proof_validate_refill_hole`,
     script/proof cursor before/after fields, script/proof clamp booleans, and
     `last_reconcile_pre_refusal_unapplied_clamp` to the condition detail
     snapshot. The remedy WARN now includes the same script/proof refill fields.
     The pre-refusal adapter now logs invalid arguments, scan failure context,
     and cursor-clamp failure context. The script-refill test comment was
     corrected so it no longer claims the pre-refusal export lives in the
     retained refill translation unit.
   - Tests: ran `make build-only`,
     `make t ONLY=stage_repair_script_refill`,
     `make t ONLY=reducer_frontier_reconcile_light`,
     `make check-no-new-repair-rung`, `make check-silent-errors-jobs`,
     `git diff --check`, and `make lint`.

17. **Reducer-frontier noncanonical purge uses a stable active-chain snapshot**
   - Files: `app/jobs/src/stage_repair_reducer_frontier_purge.c`,
     `lib/test/src/test_reducer_frontier_reconcile_light.c`
   - Problem: the noncanonical stage-log purge read `tip_h` once, then called
     `active_chain_at()` during the SQL scan/delete loop. Active-chain readers
     are memory-safe, but not a coherent logical snapshot across a multi-height
     purge. A concurrent active-chain move could make the purge compare rows
     against a mixed view, then delete by height from `progress.kv`.
   - Fix: the purge now owns `progress_store_tx_lock()` for the compare/delete
     pass, briefly takes `active_chain.write_lock` in the documented
     `progress_store_tx_lock -> active_chain.write_lock` order, copies up to
     `RF_NONCANON_MAX_PER_PASS` canonical hashes into a local checked-allocation
     snapshot, and makes every mismatch decision from that immutable snapshot.
     The SQL step handling remains fail-closed, and no direct authoritative RAM
     state reads were added.
   - Tests: ran `make build-only`,
     `make t ONLY=reducer_frontier_reconcile_light`,
     `make t ONLY=validate_script_hash_split_repair`,
     `make t ONLY=reorg_residue_tipfin_replace`,
     `make t ONLY=active_chain_extend`, `git diff --check`,
     `make check-silent-errors-jobs`, `make check-no-new-repair-rung`, and
     `make lint`.

18. **Reducer-frontier marker/progress-meta DRY and stale tipfin witness gate**
   - Files: `app/jobs/src/reducer_frontier_marker.c`,
     `app/jobs/src/stage_repair_reducer_frontier_internal.h`,
     `app/jobs/src/stage_repair_reducer_frontier_coin.c`,
     `app/jobs/src/stage_repair_reducer_frontier_tipfin.c`,
     `app/conditions/src/reducer_frontier_reconcile_light.c`,
     `lib/test/src/test_stage_repair_tipfin_backfill.c`,
     `lib/test/src/test_reducer_reconcile_witness.c`
   - Problem: stale proof replay and tip-finalize backfill had duplicate
     one-shot marker key/read/write helpers. The tipfin resume path also only
     warned when `tipfin_backfill.progress` pointed at a different span, then
     continued as a resume; that skipped the G7 one-shot marker check and
     would not record a new marker for the new span. The condition-layer
     diagnostic reader accepted any tipfin progress blob of at least four
     bytes, while the repair writer/reader treat the witness as an exact
     8-byte record.
   - Fix: added one shared reducer-frontier marker utility for key construction,
     progress-meta read, and in-transaction marker writes; both stale proof
     replay and tipfin backfill use it. A stale tipfin resume witness now logs
     the skew, resets resume state, and restarts through the normal marker gate.
     The condition progress snapshot now requires the same exact 8-byte
     `[last_backfilled_height i32 LE][total u32 LE]` format and treats malformed
     records as absent with a warning. The coin-backfill scan cursor reader
     remains separate because its progress record has different semantics.
   - Tests: ran `make build-only`,
     `make t ONLY=stage_repair_tipfin_backfill`,
     `make t ONLY=reducer_reconcile_witness`,
     `make t ONLY=stage_repair` (covering stage repair, coin backfill,
     script refill, and tipfin backfill),
     `make t ONLY=reducer_frontier_reconcile_light`, `git diff --check`,
     `make check-no-new-repair-rung`, `make check-silent-errors-jobs`, and
     `make lint`.

19. **Reducer-frontier retained verdict replay split**
   - Files: `app/jobs/src/reducer_frontier_replay.c`,
     `app/jobs/src/stage_repair_reducer_frontier_coin.c`,
     `app/jobs/src/stage_repair_reducer_frontier_internal.h`,
     `lib/test/src/test_validate_script_hash_split_repair.c`
   - Problem: `_coin.c` mixed borrowed-seed coin-backfill/value-overflow repair
     hooks with retained crash/reorg verdict-replay machinery: active-chain
     block reads, stale script/proof replay, validate/script hash-split
     classification, and test seams. That made the post-cure delete boundary
     easy to misread: deleting `_coin.c` after the sovereign cure would also
     delete code that must survive.
   - Fix: moved the retained verdict replay code into neutral
     `reducer_frontier_replay.c`, with explicit internal entry points for stale
     script replay, validate/script hash-split replay, stale proof replay, and
     the shared log-hole scanner. `_coin.c` is now the small dispatcher for
     value-overflow, the single allowed coin-backfill caller, and the retained
     replay call order. The split also adds contextual `LOG_RETURN` call-site
     logs required by the silent-bool ratchet. The hash-split classifier now has
     direct coverage for the both-stale case (`validate_headers != active` and
     `script_validate != active`), which routes to the script-side dual replay.
   - Tests: ran `make build-only`,
     `make t ONLY=validate_script_hash_split_repair`,
     `make t ONLY=stage_repair` (covering stage repair, coin backfill,
     script refill, and tipfin backfill),
     `make t ONLY=reducer_frontier_reconcile_light`,
     `make t ONLY=reducer_reconcile_witness`,
     `make check-no-new-repair-rung`,
     `make check-no-new-coin-backfill-caller`,
     `make check-silent-errors-jobs`, `make check-doc-accuracy`,
     `git diff --check`, and `make lint`.

20. **Reducer-frontier replay dispatcher-order pin**
   - Files: `app/jobs/src/reducer_frontier_replay.c`,
     `lib/test/src/test_reducer_frontier_reconcile_light.c`
   - Problem: the reconcile-light dispatcher can see more than one replay
     candidate in one pass. A lower stale-script `internal_error` and a higher
     script-side validate/script hash split both write into the shared
     `stale_script_*` result fields. Without an ordering guard, the later
     hash-split probe could overwrite the lower stale-script owner and make
     the reported next repair height point past the first transient hole.
   - Fix: the script-side hash-split replay probe now returns without
     overwriting the shared stale-script result fields when an equal/lower
     stale-script owner is already present. The new reconcile-light regression
     seeds a lower stale-script hole and a higher hash split, then asserts that
     dry-run ownership remains at the lower height and no cursors move.
   - Tests: ran `make build-only`,
     `make t ONLY=reducer_frontier_reconcile_light`,
     `make t ONLY=validate_script_hash_split_repair`,
     `make t ONLY=stage_repair`,
     `make check-no-new-repair-rung`,
     `make check-no-new-coin-backfill-caller`,
     `make check-silent-errors-jobs`, `make check-doc-accuracy`,
     `git diff --check`, and `make lint`.

21. **Readable stale script/proof replay fixtures**
   - Files: `lib/test/src/test_reducer_frontier_reconcile_light.c`,
     `tools/lint/check_no_new_coin_backfill_caller.sh`,
     `lib/test/src/test_make_lint_gates.c`,
     `app/jobs/include/jobs/stage_repair_coin_backfill.h`
   - Problem: stale script/proof replay was covered by dry-run and unreadable
     active-chain refusal paths, but not by a production-readable block fixture
     proving the apply path actually rewinds the owned cursors without creating
     a synthetic coin tear. The coin-backfill caller ratchet also scanned app,
     config, lib, and tools roots but missed newer compiled production roots.
   - Fix: added regtest-mined readable active-chain fixtures for stale script
     and transient proof `internal_error` replay. The fixtures force the
     reducer compiled-anchor test override to the fixture anchor, reset chain
     params/datadir/anchor state in teardown, and recursively remove nested
     block files. The stale-script regression proves dry-run is non-mutating
     and apply rewinds script/proof/tip from the readable hole while preserving
     body and UTXO cursors; the stale-proof regression proves apply rewinds
     proof/tip only and leaves script validation rows intact. The coin-backfill
     caller gate now scans `domain`, `application`, and
     `adapters/outbound/persistence`, with a planted domain fixture proving the
     expanded scope trips. The coin-backfill public-header comment now states
     that the inverse-delta horizon is observability, not eligibility.
   - Tests: ran `make build-only`,
     `make t ONLY=reducer_frontier_reconcile_light`,
     `make t ONLY=make_lint_gates`,
     `make t ONLY=stage_repair`,
     `make t ONLY=validate_script_hash_split_repair`,
     `make check-no-new-repair-rung`,
     `make check-no-new-coin-backfill-caller`,
     `make check-silent-errors-jobs`, `git diff --check`,
     `make check-doc-accuracy`, `make lint`, and the full
     `build/bin/test_parallel` suite (`0/466` groups failed, 14 self-skipped).

22. **Deploy target installs and verifies the live service target**
   - Files: `Makefile`, `tools/deploy_verify.sh`,
     `docs/work/code-quality-audit-2026-06-30.md`
   - Problem: the deploy target rebuilt and restarted the live user service,
     whose drop-in `ExecStart` uses `~/.local/bin/zclassic23-live` and
     `~/.zclassic-c23-fullhist`, but it only rebuilt `build/bin/zclassic23` and
     `tools/deploy_verify.sh` polled plain `build/bin/zclassic-cli` against the
     default `~/.zclassic-c23` cookie. A restart could therefore keep the old
     live binary and/or false-fail with "Cannot read cookie" while the
     full-history service was healthy. Follow-up deployment review found three
     verifier bugs: it only switched to the service datadir if the service
     cookie already existed at script startup, it could match nested
     `"healthy":true` fields while top-level `healthcheck.healthy=false`, and a
     single slow diagnostic RPC could hang the verifier inside the outer
     deadline.
   - Fix: `make deploy` now reads the effective `zclassic23.service`
     `ExecStart` binary path and installs the rebuilt `build/bin/zclassic23`
     there before restart when it differs from the in-tree binary. The verifier
     mirrors `tools/z` target selection: it honors explicit `ZCL_DATADIR`,
     `ZCL_RPCPORT`, and `ZCL_RPCCONNECT`; otherwise it reads the service
     `-datadir` / `-rpcport` immediately and passes those to `zclassic-cli` /
     `zcl-rpc`. It now validates top-level `healthcheck` contract fields with
     JSON parsing instead of loose grep, refreshes the success height from the
     verified health evidence so an early `getblockcount=0` cannot produce a
     green zero-height success line, and wraps each RPC probe with a bounded
     timeout (`ZCL_DEPLOY_RPC_TIMEOUT`, default 20s). Custom wrapper tools are
     still called without added datadir/port options, but they are also bounded
     by the same timeout wrapper.
   - Tests: ran `sh -n tools/deploy_verify.sh`, then
     `ZCL_DEPLOY_EXPECT_COMMIT=29329bffe ZCL_DEPLOY_VERIFY_TIMEOUT=180
     ./tools/deploy_verify.sh`, which passed against the live full-history
     service. Follow-up tests added a fake RPC fixture proving nested
     `"healthy":true` no longer hides top-level `healthy=false`, a fake slow RPC
     fixture proving per-call timeout reporting, a fake zero-`getblockcount`
     fixture proving the success line reports the verified health height,
     direct live verifier runs for `b615dee88`, `f0cd0be9a`, and `5b4888096`,
     `git diff --check`, `make check-doc-accuracy`, and final `make deploy` at
     `5b4888096` (`Deployed + RPC live at block 3165011`).

23. **Deploy verifier fallback RPC compatibility**
   - Files: `tools/deploy_verify.sh`, `lib/test/src/test_make_lint_gates.c`
   - Problem: the verifier's fallback to `build/bin/zcl-rpc` passed
     `-datadir=` and `-rpcport=` as positional arguments, but `zcl-rpc` reads
     `ZCL_DATADIR` / `ZCL_RPCPORT` from the environment and treats argv[1] as
     the RPC method. The same fallback also returns JSON-RPC envelopes, so
     top-level health checks could fail on valid fallback output. The success
     line preferred chain-advance local height over `checks.log_head`, which
     could overstate the finalized stage cursor by one block when health still
     allows a bounded log-head gap.
   - Fix: the verifier now branches `zclassic-cli` and `zcl-rpc` invocation
     contracts explicitly, unwraps successful JSON-RPC `result` envelopes before
     checking diagnostic objects, keeps error envelopes intact for useful
     failures, and reports `checks.log_head` first when a healthy response
     supplies it. The make-lint gate test now asserts the fallback env contract,
     JSON-RPC unwrapping helper, and log-head height preference remain in the
     verifier.
   - Tests: ran `sh -n tools/deploy_verify.sh` and a fake executable named
     `zcl-rpc` that proved the verifier exports `ZCL_DATADIR` / `ZCL_RPCPORT`,
     falls back to quoted `dumpstate` params, unwraps JSON-RPC envelopes, and
     reports `block 100` from `checks.log_head` instead of
     `local_height=101`. Final local verification for this pass: `git diff
     --check`, `make t ONLY=node_health_service`,
     `make t ONLY=make_lint_gates`, `make check-doc-accuracy`, `make lint`,
     and the full `build/bin/test_parallel` suite (`0/466` groups failed,
     14 self-skipped).

24. **Source-selection and operator-authority cleanup**
   - Files: `app/services/src/block_source_policy.c`,
     `lib/net/src/connman.c`, `lib/net/include/net/connman.h`,
     `tools/mcp/controllers/ops_controller.c`,
     `app/controllers/include/controllers/repair_controller.h`,
     `app/controllers/src/repair_controller_rebuild.c`,
     `app/views/src/explorer_factoids_chaindata.c`,
     `lib/test/src/test_chain_advance_coordinator.c`,
     `lib/test/src/test_connman_addnode_fallback.c`,
     `lib/test/src/test_mcp_controllers.c`,
     `lib/test/src/test_rebuild_recent.c`
   - Problem: pure source-selection policy could mark a source with
     `height=-1` selectable when local/target heights were already equal;
     the outbound peer-floor helper counted any handshaked outbound peer even
     if it did not advertise `NODE_NETWORK`; and recovery operator wording
     still gave local `zclassicd` authority semantics despite the current
     consensus model making it an advisory source only.
   - Fix: `source_selection_blocker` now treats unknown source height as a
     named blocker, `connman_outbound_healthy_count` now matches
     `connman_get_outbound_health` by requiring block-serving `NODE_NETWORK`
     peers, and the rebuild-recent/MCP/help/comment wording now says
     "legacy advisory source" plus local consensus validation.
   - Tests: ran `make t ONLY=chain_advance_coordinator`,
     `make t ONLY=connman_addnode_fallback`, and
     `make t ONLY=mcp_controllers`; final gate `make lint`.

25. **Advisory zclassicd oracle decision**
   - Files: `docs/work/code-quality-audit-2026-06-30.md`
   - Decision: do not restart or repair the C++ `zclassicd` oracle as part of
     this code-review pass. The live zclassic23 node is healthy from native
     P2P/local consensus validation, while the legacy process is only an
     advisory dependency and is currently unusable at the RPC layer
     (`-28 Activating best chain... height 0 (1%)`). Leave it running and
     unavailable rather than mutating the operator's reference process during a
     code-quality pass. Revisit only for the C8 exact-parity/soak gate, where
     the oracle repair is an explicit operational task.
   - Tests: no code change. Current batch verification for the surrounding
     operator surface includes `make t ONLY=chain_advance_coordinator`,
     `make t ONLY=connman_addnode_fallback`, `make t ONLY=mcp_controllers`, and
     `make t ONLY=rebuild_recent`; final gate `make lint`.

26. **Oversized factoids checkpoint-row extraction**
   - Files: `app/views/src/explorer_factoids_chaindata.c`,
     `app/views/src/explorer_factoids_checkpoints.c`,
     `app/views/include/views/explorer_factoids_internal.h`,
     `lib/test/src/test_explorer.c`
   - Problem: `explorer_factoids_chaindata.c` is still an oversized view file,
     and section 12 mixed immutable checkpoint data, row rendering, and section
     orchestration inside the larger chain-data section file.
   - Fix: moved the immutable checkpoint table and row renderer into the new
     private factoids view translation unit
     `explorer_factoids_checkpoints.c`. Section 12 now only emits the heading
     and table wrapper, then delegates row rendering. This is display-only and
     consensus-neutral; checkpoint enforcement remains in chain/validation
     code. The chain-data file shrank from 1475 to 1439 lines.
   - Tests: added a direct explorer regression for the checkpoint section that
     asserts the rendered section, latest checkpoint link/hash preview, and
     SHA3 receipt remain present. Ran `make t ONLY=explorer` and `make lint`.

27. **Oversized factoids data-integrity extraction**
   - Files: `app/views/src/explorer_factoids_chaindata.c`,
     `app/views/src/explorer_factoids_integrity.c`,
     `app/views/include/views/explorer_factoids_internal.h`,
     `app/views/src/explorer_factoids_view.c`,
     `lib/test/src/test_explorer.c`
   - Problem: section 17's last-100-block integrity hash rendering still lived
     inside the oversized chain-data section file even though it has its own
     verification contract and no coupling to the other archaeology sections.
   - Fix: moved the Data Integrity section renderer into
     `explorer_factoids_integrity.c` and refreshed the private factoids view
     ownership comments. The chain-data file shrank from 1439 to 1375 lines.
   - Tests: added a direct explorer regression for the integrity section that
     asserts chain height, indexed block/transaction counts, the
     `chain_height-99 .. chain_height` coverage text, and the deterministic
     SHA3 integrity hash remain present. Ran `make t ONLY=explorer` and
     `make lint`.

28. **Download-queue starvation clears at tip**
   - Files: `app/conditions/src/download_queue_starved.c`,
     `lib/test/src/test_sync_watchdog_conditions.c`
   - Problem: deploy verification exposed a live restart case where
     `download_queue_starved` stayed active after the node reached
     `SYNC_AT_TIP`: the detector became false because the node was no longer
     in `SYNC_BLOCKS_DOWNLOAD`, but the witness only accepted a request-counter
     increase, so the warn condition could not clear. That left an otherwise
     at-tip node with one active condition and made deploy health wait/fail.
   - Fix: the witness now treats leaving `SYNC_BLOCKS_DOWNLOAD` as an honest
     symptom clear. The request-counter witness still protects the in-download
     remedy path from self-reporting success before the queue actually refills.
   - Tests: added a sync-watchdog regression that detects the starvation,
     records one unwitnessed remedy, transitions to `SYNC_AT_TIP`, and asserts
     the active condition clears. Ran `make t ONLY=sync_watchdog_conditions`
     and `make lint`.

29. **Oversized factoids difficulty extraction**
   - Files: `app/views/src/explorer_factoids_chaindata.c`,
     `app/views/src/explorer_factoids_difficulty.c`,
     `app/views/include/views/explorer_factoids_internal.h`,
     `app/views/src/explorer_factoids_view.c`,
     `lib/test/src/test_explorer.c`
   - Problem: section 16's difficulty records, yearly peak table, and
     display-only compact-target/chainwork math still lived inside the
     oversized chain-data section file.
   - Fix: moved the Difficulty History section renderer and its display-only
     decoders into `explorer_factoids_difficulty.c`, leaving
     `explorer_factoids_chaindata.c` responsible for sections 8-15 only. The
     chain-data file shrank from 1375 to 1182 lines.
   - Tests: added a direct explorer regression for the difficulty section that
     asserts the records card, hardest compact target, block link, bounded
     recent-retarget count, chainwork line, yearly peak table, and SHA3 receipt
     remain present. Ran `make t ONLY=explorer` and `make lint`.

30. **Oversized factoids empty-block extraction**
   - Files: `app/views/src/explorer_factoids_chaindata.c`,
     `app/views/src/explorer_factoids_empty_blocks.c`,
     `app/views/include/views/explorer_factoids_internal.h`,
     `app/views/src/explorer_factoids_view.c`,
     `lib/test/src/test_explorer.c`
   - Problem: section 15's empty-block totals, yearly table, busiest-block
     record, and longest-empty-run query still lived inside the oversized
     chain-data section file.
   - Fix: moved the Empty Blocks renderer into
     `explorer_factoids_empty_blocks.c`, leaving
     `explorer_factoids_chaindata.c` responsible for sections 8-14 only. The
     chain-data file shrank from 1182 to 1076 lines.
   - Tests: added a direct explorer regression for the empty-block section that
     asserts the 5-of-8 summary, yearly table row, busiest block link, longest
     empty run, and both SHA3 receipts remain present. Ran
     `make t ONLY=explorer` and `make lint`.

31. **Oversized factoids blocktime/transaction extraction**
   - Files: `app/views/src/explorer_factoids_chaindata.c`,
     `app/views/src/explorer_factoids_blocktimes.c`,
     `app/views/src/explorer_factoids_transactions.c`,
     `app/views/include/views/explorer_factoids_internal.h`,
     `app/views/src/explorer_factoids_view.c`,
     `lib/test/src/test_explorer.c`
   - Problem: after the first four factoids splits, sections 13 and 14 still
     kept block cadence and transaction archaeology inside the chain-data view
     file, leaving it above the 800-line file-size advisory ceiling.
   - Fix: moved Block Time Analysis into `explorer_factoids_blocktimes.c` and
     Transaction Archaeology into `explorer_factoids_transactions.c`, leaving
     `explorer_factoids_chaindata.c` responsible for sections 8-12 only. The
     chain-data file shrank from 1076 to 746 lines, clearing its advisory
     file-size warning.
   - Tests: added direct explorer regressions for the blocktime section
     (pre/post Buttercup cadence, fast/slow interval records, and receipts) and
     transaction section (canonical totals, yearly row, records, output
     script-type split, and receipts). Ran `make t ONLY=explorer` and
     `make lint`.

32. **Oversized factoids records/address extraction**
   - Files: `app/views/src/explorer_factoids_history.c`,
     `app/views/src/explorer_factoids_records.c`,
     `app/views/src/explorer_factoids_addresses.c`,
     `app/views/include/views/explorer_factoids_internal.h`,
     `app/views/src/explorer_factoids_view.c`,
     `lib/test/src/test_explorer.c`
   - Problem: after clearing the chain-data file warning, the sibling
     `explorer_factoids_history.c` still owned records, supply, and address
     statistics in one oversized view file.
   - Fix: moved All-Time Records into `explorer_factoids_records.c` and
     Address Statistics into `explorer_factoids_addresses.c`, leaving
     `explorer_factoids_history.c` responsible for genesis, upgrades, mining
     eras, milestones, and supply only. The history file shrank from 1089 to
     638 lines, so all factoids section files are now under the 800-line
     advisory ceiling.
   - Tests: added direct explorer regressions for the records section
   (largest unspent/ever outputs, tx/shielding records, HODL and
   Buttercup-age receipts) and address section (holder counts,
   concentration table, richest-address row, and receipts). Ran
   `make t ONLY=explorer`; final gate `make lint`.

33. **Oversized diagnostics block-index/header-band extraction**
   - Files: `app/controllers/src/diagnostics_registry.c`,
     `app/controllers/src/diagnostics_block_index.c`,
     `app/controllers/include/controllers/diagnostics_internal.h`
   - Problem: `diagnostics_registry.c` mixed the routing table and
     controller-owned state with the heavier block-index/header-band dump JSON
     implementation, leaving the registry above the 800-line advisory target.
   - Fix: moved the read-only block-index and header-band dumpers into a
     focused controller translation unit, added a `diag_main_state()` accessor
     for the shared controller state, and kept `g_dumpers[]` as the only
     routing authority. The registry is now 798 lines.
   - Tests: ran `make t ONLY=mcp_controllers` and `git diff --check`; final
     gate `make lint`.

34. **Oversized proof-validation dumpstate extraction**
   - Files: `app/jobs/src/proof_validate_stage.c`,
     `app/jobs/src/proof_validate_stage_dump.c`,
     `app/jobs/src/proof_validate_stage_internal.h`
   - Problem: `proof_validate_stage.c` was just above the 800-line advisory
     ceiling even though the zcl_state dump/query tail was a read-only concern
     separate from proof verification and cursor advancement.
   - Fix: moved the proof-validation dump JSON and first-failure query into a
     focused sibling job translation unit, added a small sibling-private stage
     state accessor seam, and left proof validation/cursor behavior unchanged.
     `proof_validate_stage.c` is now 749 lines.
   - Tests: ran `make t ONLY=proof_validate_stage`; final gates
     `git diff --check` and `make lint`.

35. **Oversized boot-worker supervisor helper extraction**
   - Files: `config/src/boot_background_workers.c`,
     `config/src/boot_worker_supervisor.c`,
     `config/src/boot_snapshot_offer.c`,
     `config/include/config/boot_background_workers.h`
   - Problem: `boot_background_workers.c` still owned both worker loops and the
     shared observe-only supervisor stall/register helper bodies used by the
     snapshot-offer worker, leaving the unit above the 800-line advisory
     ceiling even though the helper concern was already shared.
   - Fix: moved the shared `worker_on_stall` and
     `boot_register_worker_supervisor` implementations into a focused config
     translation unit, leaving each worker's contract/id and thread loop in the
     existing owner. `boot_background_workers.c` is now 731 lines.
   - Tests: ran `make t ONLY=supervisor_production_tree`; final gates
     `git diff --check` and `make lint`.

36. **Oversized chain-restore backing extraction**
   - Files: `app/services/src/chain_restore_repair.c`,
     `app/services/src/chain_restore_backing.c`,
     `lib/test/src/test_chain_restore_service.c`
   - Problem: `chain_restore_repair.c` mixed the post-restore repair/finalize
     flow with public consensus-backed and on-disk-backed block predicates,
     leaving the repair unit above the 800-line advisory ceiling. The
     cold-import seed-anchor carve-out also lacked a direct regression pinning
     exact hash/height provenance.
   - Fix: moved the backing predicates and nearest-backed-ancestor walkers into
     a focused service translation unit while leaving repair orchestration in
     `chain_restore_repair.c`. Added a disk-backed seed-anchor test proving the
     no-`pprev` carve-out only applies to the exact supplied seed hash and
     height. `chain_restore_repair.c` is now 650 lines.
   - Tests: ran `make t ONLY=chain_restore_service`; final gates
     `git diff --check` and `make lint`.

37. **Oversized validate-headers worker-pool extraction**
   - Files: `app/jobs/src/validate_headers_stage.c`,
     `app/jobs/src/validate_headers_pool.c`,
     `app/jobs/src/validate_headers_pool.h`
   - Problem: `validate_headers_stage.c` still owned both reducer-stage
     validation/cursor behavior and the pthread worker-pool machinery, leaving
     the stage above the 800-line advisory ceiling even though the pool only
     distributes opaque job slots and waits for completion.
   - Fix: moved the fixed worker pool into a sibling-private helper with a
     small opaque-job callback. The stage still owns validator selection,
     validation verdicts, failed-row rechecks, counter updates, log writes, and
     cursor movement. `validate_headers_stage.c` is now 722 lines.
   - Tests: ran `make t ONLY=validate_headers_stage`; final gates
     `git diff --check` and `make lint`.

38. **Oversized script-validation dumpstate/prevout extraction**
   - Files: `app/jobs/src/script_validate_stage.c`,
     `app/jobs/src/script_validate_stage_dump.c`,
     `app/jobs/src/script_validate_stage_prevout.c`,
     `app/jobs/src/script_validate_stage_internal.h`
   - Problem: `script_validate_stage.c` owned validation/cursor behavior, the
     read-only zcl_state dump/query tail, and the production created-output /
     `coins_kv` prevout resolver, leaving the stage above the 800-line advisory
     ceiling.
   - Fix: moved dumpstate and first-failure reporting into a sibling dump TU,
     moved the production prevout resolver into a sibling prevout TU, and kept
     validation verdicts, log writes, counters, and cursor movement in the
     stage owner. `script_validate_stage.c` is now 734 lines.
   - Tests: ran `make t ONLY=script_validate_stage`; final gates
     `git diff --check` and `make lint`.

39. **Oversized reducer-frontier reader extraction**
   - Files: `app/jobs/src/reducer_frontier.c`,
     `app/jobs/src/reducer_frontier_readers.c`
   - Problem: `reducer_frontier.c` mixed the L0 H* authority with public
     derived-state helper readers for coins-best and per-log coverage, keeping
     the authority file above the 800-line advisory ceiling.
   - Fix: moved the reusable SELECT-only coins-best derivation and coverage
     floor readers into a sibling job translation unit. The original file keeps
     trusted-anchor selection, contiguous-prefix scans, hash-agreement clamping,
     and public H* computation. `reducer_frontier.c` is now 783 lines.
   - Tests: ran `make t ONLY=reducer_frontier` and
     `make t ONLY=coins_best_derivation`; final gates `git diff --check` and
     `make lint`.

40. **Oversized chain-state result-name extraction**
   - Files: `app/services/src/chain_state_service.c`,
     `app/services/src/chain_state_result.c`
   - Problem: `chain_state_service.c` mixed repository validation/persistence
     with the standalone public result-name table, keeping the service just
     above the 800-line advisory ceiling.
   - Fix: moved `csr_result_name()` into a sibling service translation unit.
     The repository file retains validation, persistence, event emission, and
     chain-tip state mutation. `chain_state_service.c` is now 794 lines.
   - Tests: ran `make t ONLY=chain_state_repo`; final gates
     `git diff --check` and `make lint`.

41. **Oversized wallet read-helper extraction**
   - Files: `app/models/src/wallet_tx.c`,
     `app/models/src/wallet_tx_reads.c`,
     `app/models/include/models/wallet_tx_internal.h`
   - Problem: `wallet_tx.c` still mixed AR validation/hooks, persistence, and
     relationship reads with read-only aggregate/tip-summary helpers, keeping
     the model above the 800-line advisory ceiling.
   - Fix: moved the shared sum/count aggregate helper, max-height reader,
     chain/effective tip readers, and wallet projection summary into the
     existing read-model helper TU. The public wallet API is unchanged; the
     internal header now documents that read-only wallet aggregates belong in
     `wallet_tx_reads.c` while hooks, validation, persistence, and
     relationships stay in `wallet_tx.c`. `wallet_tx.c` is now 772 lines.
   - Tests: ran `make t ONLY=models` and
     `make t ONLY=wallet_funds_safety`; final gates `git diff --check` and
     `make lint`.

42. **Oversized validation-pack state/dump extraction**
   - Files: `app/services/src/invariant_sentinel.c`,
     `app/services/src/invariant_sentinel_state.c`,
     `app/services/src/invariant_sentinel_internal.h`
   - Problem: the core invariant sentinel mixed fail-loud detection/audit
     behavior with health rollup, `zcl_state validation_pack` JSON export,
     cross-module seed/locator/loader counters, and testing reset hooks,
     making the ownership boundary harder for future AI passes to navigate.
   - Fix: moved health, dumpstate, cross-module counter feeds, and testing
     reset support into a sibling state TU behind a sentinel-owned internal
     state header. The core file now keeps pair checks, window sweeps,
     commitment audits, and supervisor registration. `invariant_sentinel.c`
     is now 702 lines.
   - Tests: ran `make t ONLY=invariant_sentinel`,
     `make t ONLY=node_health`, and `make t ONLY=mcp_controllers`; final
     gates `git diff --check` and `make lint`.

43. **Oversized coin-backfill creator-proof extraction**
   - Files: `app/jobs/src/stage_repair_coin_backfill.c`,
     `app/jobs/src/stage_repair_coin_backfill_creator.c`,
     `app/jobs/src/stage_repair_coin_backfill_internal.h`
   - Problem: the guarded coin-backfill job mixed orchestration/refusal policy,
     marker checks, no-spend scan handoff, insert transaction, unresolved
     prevout enumeration, and active-chain creator resolution in one oversized
     repair file. That made the borrowed-seed tear repair harder for future AI
     passes to review before post-cure deletion.
   - Fix: moved G5 unresolved-prevout enumeration and G7/G8 creator-proof
     resolution into a private creator TU behind the existing src-private
     backfill contract. The orchestration file now keeps refusal policy,
     marker reads, scan orchestration, and the single `coins_kv` insert
     transaction. `stage_repair_coin_backfill.c` is now 630 lines.
   - Tests: ran `make t ONLY=stage_repair_coin_backfill` and
     `make t ONLY=script_validate_stage`; final gates `git diff --check` and
     `make lint`.

44. **Oversized utxo-apply live-lookup extraction**
   - Files: `app/jobs/src/utxo_apply_stage.c`,
     `app/jobs/src/utxo_apply_stage_lookup.c`,
     `app/jobs/src/utxo_apply_stage_internal.h`
   - Problem: `utxo_apply_stage.c` mixed the stage cursor/write transaction
     flow with the production live-prevout resolver. That resolver is
     read-only but consensus-sensitive: it must resolve only currently
     unspent `coins_kv` / `coins_ram` outputs so an already-spent prevout
     cannot look live.
   - Fix: moved the production resolver to a sibling-private lookup TU behind
     `utxo_apply_stage_internal.h`. The stage file now owns orchestration,
     blockers, cursor movement, `coins_kv` writes, and the `coins_ram` writer
     bracket; the lookup file owns the live UTXO read contract and RAM-overlay
     fast path. `utxo_apply_stage.c` is now 983 lines.
   - Tests: ran `make t ONLY=utxo_apply`; final gates `git diff --check` and
     `make lint`.

45. **Above-tip block-index diagnostics**
   - Files: `app/controllers/src/diagnostics_block_index.c`,
     `lib/test/src/test_rpc.c`
   - Problem: `dumpstate block_index <height>` resolved numeric heights only
     through `active_chain_at`, so the diagnostic returned `found=false` for
     the exact above-finalized best-header entries future agents need while
     investigating reducer stalls such as `lookahead_tip_missing`.
   - Fix: numeric lookup now checks the finalized active-chain slot first, then
     falls back to `pindex_best_header` ancestry for canonical above-tip
     headers. The JSON reports `lookup_source` (`active_chain`,
     `best_header_ancestor`, `block_map_hash`, or `missing`) so operators can
     tell which authority answered the query.
   - Tests: ran `make t ONLY=rpc`, including a regression that finalizes height
     0 while `pindex_best_header` reaches height 1 and asserts
     `dumpstate block_index 1` resolves via `best_header_ancestor`.

46. **Reducer-frontier reconcile-light observability split**
   - Files: `app/conditions/src/reducer_frontier_reconcile_light.c`,
     `app/conditions/src/reducer_frontier_light_observe.c`,
     `app/conditions/src/reducer_frontier_light_observe.h`
   - Problem: the reconcile-light condition mixed active condition decisions
     with a large observe-only surface: at-detect snapshots, progress-meta
     readers, reconcile-result detail fields, bypass/remedy counters, and test
     reset state. That made future review harder because a reader had to
     separate repair semantics from diagnostic bookkeeping in one 1,240-line
     file.
   - Fix: moved the observe-only state and readers behind a private helper with
     a small API. The condition file now owns the active decisions
     (`detect`, `remedy`, `witness`, `progressing`) and calls the helper for
     snapshots/detail/test counters. The split keeps the H*-only witness and
     refresh-only progress semantics unchanged, adds null-output guards to the
     private progress-meta readers, and brings the condition file down to
     583 lines (`reducer_frontier_light_observe.c` is 708 lines).
   - Tests: ran `make -j$(nproc) build-only`,
     `make t ONLY=reducer_frontier_reconcile_light`,
     `make t ONLY=reducer_reconcile_witness`,
     `make t ONLY=reducer_forward_progress_gate`,
     `make t ONLY=refold_premature_clear`, `git diff --check`, and
     `make lint`.

47. **Boot snapshot failure-memory split**
   - Files: `config/src/boot.c`,
     `config/src/boot_snapshot_failure_memory.c`,
     `config/include/config/boot_snapshot_failure_memory.h`,
     `lib/test/src/test_boot_snapshot_failure_memory.c`
   - Problem: the `app_init` boot path still carried the full starter-pack
     failure-memory policy inline: autodetect selection, `.failed` marker
     creation, explicit prior-marker skip, marker-unwritable fallback, and
     success cleanup. This made the fresh-judge crash-loop guard harder for
     future agents to find and review.
   - Fix: moved the policy into `boot_snapshot_failure_memory_prepare()` and
     `boot_snapshot_failure_memory_clear()`. `boot.c` now reads as "prepare
     failure memory, run the loader if selected, clear the marker after a clean
     return"; the helper owns the concrete autodetect/explicit branches and
     fail-closed no-marker behavior.
   - Tests: added `test_boot_snapshot_failure_memory`, covering explicit marker
     write+clear, prior explicit marker skip, autodetect marker creation,
     proven-authority autodetect suppression, and too-small marker buffer
     fallback to P2P. Ran `make t ONLY=boot_snapshot_failure_memory`,
     `build/bin/test_parallel --only=loader_owns_seed_gate`,
     `build/bin/test_parallel --only=boot_refold_window_extend`, and
     `build/bin/test_parallel --only=load_verify_boot`.

48. **Boot postmortem lifecycle split**
   - Files: `config/src/boot.c`, `config/src/boot_postmortem.c`,
     `config/include/config/boot_postmortem.h`,
     `config/include/config/boot.h`, `lib/test/src/test_postmortem.c`
   - Problem: the boot composition root still carried the crash-capsule
     lifecycle inline: postmortem directory policy, seed-tape boot event,
     restart-time capsule compression, retention pruning, and testing hooks.
     That made it harder for future agents to distinguish boot phase order from
     crash-capsule policy.
   - Fix: moved the boot-owned postmortem lifecycle into
     `boot_postmortem_start()` / `boot_postmortem_stop()`. `boot.c` now calls a
     named lifecycle boundary during prologue and shutdown, while the new
     module owns the policy constants and seed-tape state. `boot.c` is now
     3965 lines and the new helper is 114 lines.
   - Tests: ran `make -j$(nproc) build-only` and `make t ONLY=postmortem`;
     the existing boot postmortem install/restart tests still cover signal
     capture, boot-event replay, and restart compression through the moved
     hooks.

49. **Boot datadir lock split**
   - Files: `config/src/boot.c`, `config/src/boot_datadir_lock.c`,
     `config/include/config/boot_datadir_lock.h`,
     `lib/test/src/test_boot_datadir_lock.c`,
     `lib/test/include/test/test_helpers.h`, `lib/test/src/test.c`,
     `lib/test/src/test_parallel.c`
   - Problem: the boot composition root still carried PID-file datadir lock
     policy inline. That obscured the boot phase order with single-process
     guard details: path construction, running-PID refusal, stale-PID
     overwrite, and non-fatal PID-file creation failure.
   - Fix: moved the lock lifecycle into `boot_datadir_lock_acquire()` /
     `boot_datadir_lock_release()`. `boot.c` now calls a named phase boundary,
     while the new helper owns PID-file policy and fails closed for null,
     empty, or truncated datadir paths. `boot.c` is now 3916 lines and the new
     helper is 67 lines.
   - Tests: ran `make -j$(nproc) build-only` and
     `make t ONLY=boot_datadir_lock`.

50. **Boot shutdown marker split**
   - Files: `config/src/boot.c`, `config/src/boot_shutdown_marker.c`,
     `config/include/config/boot_shutdown_marker.h`,
     `lib/test/src/test_boot_shutdown_marker.c`,
     `lib/test/include/test/test_helpers.h`, `lib/test/src/test.c`,
     `lib/test/src/test_parallel.c`
   - Problem: the boot composition root still carried clean/unclean shutdown
     marker policy inline at both ends of the lifecycle: startup WAL+marker
     crash detection and final shutdown marker write. That forced future boot
     reviews to jump between prologue and teardown code to validate one policy.
   - Fix: moved the policy into `boot_shutdown_marker_detect_unclean()` and
     `boot_shutdown_marker_write_clean()`. `boot.c` now calls named lifecycle
     boundaries; the helper owns path construction, EV_CRASH_RECOVERY_START
     emission, boot-time marker consumption, and best-effort clean marker
     writes. `boot.c` is now 3873 lines and the new helper is 70 lines.
   - Tests: ran `make -j$(nproc) build-only` and
     `make t ONLY=boot_shutdown_marker`.

51. **Boot stale-lock preflight split**
   - Files: `config/src/boot.c`, `config/src/boot_stale_locks.c`,
     `config/include/config/boot_stale_locks.h`,
     `lib/test/src/test_boot_stale_locks.c`,
     `lib/test/include/test/test_helpers.h`, `lib/test/src/test.c`,
     `lib/test/src/test_parallel.c`
   - Problem: the boot composition root still carried startup lock hygiene
     inline between maintenance-service startup and SQLite open. That mixed
     phase order with PID-file parsing, dead-process detection, and WAL-crash
     notices.
   - Fix: moved the policy into `boot_stale_locks_preflight()`. `boot.c` now
     calls a named pre-database boundary; the helper owns path construction,
     LevelDB/chainstate LOCK inspection, SQLite WAL notice output, and a small
     result struct for tests. It also treats `EPERM`/unknown `kill(pid, 0)`
     failures as "not proven dead" so a lock is only removed when the owner PID
     is definitely gone. `boot.c` is now 3820 lines and the new helper is 139
     lines.
   - Tests: ran `make -j$(nproc) build-only`,
     `make t ONLY=boot_stale_locks`, and
     `ZCL_TEST_ONLY=boot_stale_locks build/bin/test_zcl`.

## High-priority review backlog

1. **Repair fabric shrink plan**
   - Review `stage_repair_reducer_frontier_*` and `stage_repair_coin_backfill*`
     against `docs/work/architecture-deletion-plan.md`.
   - Goal: identify which borrowed-seed tear paths can be deleted after the
     sovereign refold cure, and which crash-recovery cursor clamps must remain.
   - Current finding: the retained cursor-refill core is now separated from the
     tear-only pre-refusal adapters. The next delete-manifest pass should treat
     `stage_repair_reducer_frontier_pre_refusal.c` as borrowed-seed-era repair
     fabric and `stage_repair_reducer_frontier_refill.c` as retained
     crash-recovery cursor refill unless the sovereign cure makes a narrower
     split possible.
   - Current finding: the coin-backfill delete boundary now has a current
     manifest. The public production caller is guarded by
     `check-no-new-coin-backfill-caller`; `_coin.c` has been reduced to
     value-overflow, the single coin-backfill hook, and dispatch order. The
     retained script/proof/hash-split verdict replay now lives in
     `reducer_frontier_replay.c`. After the sovereign cure, delete or re-home
     the remaining borrowed-seed value-overflow/coin-backfill dispatcher path
     and rewrite the condition to keep only retained reorg/flag/cursor remedies.
  - Current finding: sidecar review found the tip-finalize replacement
     whitelist/fail-closed SQLite handling, the retained refill logging gap,
     missing script/proof/hash-split condition detail fields, and the
     noncanonical purge active-chain snapshot risk, duplicated marker helpers,
     stale tipfin witness/progress handling, retained verdict-replay split, and
     lower stale-script ownership over higher hash splits, and missing readable
     stale script/proof replay outcomes; those are fixed above. Remaining
     reducer-frontier review item: continue deleting or re-homing borrowed-seed
     repair fabric after the sovereign cure.

2. **Health/source-selection diagnostics**
   - Fixed this pass: stale `SYNC_AT_TIP` plus source gap/blocker, missing
     active-tip evidence, unknown reducer log-head, shell lag-known/lag-valid
     collapse, and mirror blocker-class drift now have regressions.
   - Fixed this pass: unknown-height source candidates now fail closed, and
     the outbound peer-floor counter now requires block-serving peers.
   - Follow-up after deployment: verify the live binary preserves
     `healthy=true` when local reducer evidence is ahead of peers
     (`log_head_gap < 0`) and still turns red for unknown `log_head=-1`.

3. **zclassicd oracle availability**
   - Status output now distinguishes the process, RPC transport, and usable
     height/hash oracle.
   - Decision for this code-review pass: keep it unavailable while zclassic23
     advances from native P2P/local consensus validation. Repair/restart is a
     separate operational task for the exact-parity/soak gate, not a prerequisite
     for this audit batch.

4. **Oversized files**
   - `make lint` reports advisory file-size warnings, including
     `boot_refold_staged.c`, `boot.c`, and `boot_services.c`.
   - Split only when it reduces real coupling; avoid mechanical churn.
   - First behavior-preserving extraction landed for the factoids chain-data
     view: immutable checkpoint row data now lives in its own private view TU.
   - Second behavior-preserving extraction landed for the same view: the
     Data Integrity section now lives in its own private view TU, reducing
     `explorer_factoids_chaindata.c` to 1375 lines while preserving the
     last-100-block receipt contract.
   - Third behavior-preserving extraction landed for the same view: Difficulty
     History now lives in its own private view TU, reducing
     `explorer_factoids_chaindata.c` to 1182 lines while preserving the
     compact-target/chainwork display contract.
   - Fourth behavior-preserving extraction landed for the same view: Empty
     Blocks now lives in its own private view TU, reducing
     `explorer_factoids_chaindata.c` to 1076 lines while preserving the
     empty-block summary/record receipt contract.
   - Fifth behavior-preserving extraction landed for the same view:
     Block Time Analysis and Transaction Archaeology now live in private view
     TUs, reducing `explorer_factoids_chaindata.c` to 746 lines and clearing
     that file's advisory ceiling warning while preserving the cadence and
     transaction receipt contracts.
   - Sixth behavior-preserving extraction landed for the factoids history
     view: All-Time Records and Address Statistics now live in private view
     TUs, reducing `explorer_factoids_history.c` to 638 lines. All factoids
     section files are now below the 800-line advisory ceiling.
   - Seventh behavior-preserving extraction landed for diagnostics:
     block-index and header-band dump JSON now live in
     `diagnostics_block_index.c`, reducing `diagnostics_registry.c` to 798
     lines while preserving the dumpstate registry contract.
   - Eighth behavior-preserving extraction landed for the reducer jobs:
     proof-validation dumpstate now lives in `proof_validate_stage_dump.c`,
     reducing `proof_validate_stage.c` to 749 lines while preserving the
     proof-validation stage and dumpstate contracts.
   - Ninth behavior-preserving extraction landed for boot workers:
     shared observe-only worker supervisor helpers now live in
     `boot_worker_supervisor.c`, reducing `boot_background_workers.c` to 731
     lines while preserving worker registration and stall handling contracts.
   - Tenth behavior-preserving extraction landed for chain restore:
     consensus-backed/on-disk-backed block predicates and nearest-backed
     ancestor walks now live in `chain_restore_backing.c`, reducing
     `chain_restore_repair.c` to 650 lines while preserving repair/finalize
     behavior and directly testing seed-anchor provenance.
   - Eleventh behavior-preserving extraction landed for validate headers:
     the fixed worker pool now lives in `validate_headers_pool.c`, reducing
     `validate_headers_stage.c` to 722 lines while preserving validator,
     recheck, log-write, and cursor semantics.
   - Twelfth behavior-preserving extraction landed for script validation:
     dumpstate now lives in `script_validate_stage_dump.c` and the production
     prevout resolver now lives in `script_validate_stage_prevout.c`, reducing
     `script_validate_stage.c` to 734 lines while preserving dry-run, log-write,
     and cursor semantics.
   - Thirteenth behavior-preserving extraction landed for reducer-frontier:
     coins-best derivation and coverage-floor readers now live in
     `reducer_frontier_readers.c`, reducing `reducer_frontier.c` to 783 lines
     while preserving H* computation and derived-state reader contracts.
   - Fourteenth behavior-preserving extraction landed for chain state:
     result-code names now live in `chain_state_result.c`, reducing
     `chain_state_service.c` to 794 lines while preserving the repository
     contract and all existing callers.
   - Fifteenth behavior-preserving extraction landed for wallet models:
     read-only aggregate/tip-summary helpers now live in `wallet_tx_reads.c`,
     reducing `wallet_tx.c` to 772 lines while preserving the public wallet
     model API and AR hook/write-path ownership.
   - Sixteenth behavior-preserving extraction landed for validation pack:
     health, dumpstate, cross-module counter feeds, and testing reset support
     now live in `invariant_sentinel_state.c`, reducing
     `invariant_sentinel.c` to 702 lines while preserving fail-loud detector
     behavior and the validation-pack state API.
   - Seventeenth behavior-preserving extraction landed for coin backfill:
     unresolved-prevout enumeration and active-chain creator proof resolution
     now live in `stage_repair_coin_backfill_creator.c`, reducing
     `stage_repair_coin_backfill.c` to 630 lines while preserving the guarded
     repair API, refusal policy, no-spend scan handoff, and single-write
     transaction.
   - Eighteenth behavior-preserving extraction landed for utxo apply:
     production live-prevout lookup now lives in
     `utxo_apply_stage_lookup.c`, reducing `utxo_apply_stage.c` to 983 lines
     while preserving live-coin freshness, RAM-overlay lookup, and
     double-spend-safe "currently unspent" semantics.
   - Nineteenth behavior-preserving extraction landed for utxo apply:
     reject/event dedup, durable upstream-hole warning memo state,
     label-splice refusal logging/blocker decoration, and fold-progress
     heartbeat state now live in `utxo_apply_stage_observe.c`, reducing
     `utxo_apply_stage.c` to 759 lines while preserving the stage's
     consensus write path and public API.
   - Twentieth behavior-preserving extraction landed for tip finalize:
     counters, blocked-class state, throttled precondition/cursor-gap WARN
     state, dumpstate JSON serialization, and the published served-tip snapshot
     now live in `tip_finalize_stage_observe.c`; convention-aware durable-tip
     reads plus init cursor hydration now live in `tip_finalize_stage_durable.c`.
     This reduces `tip_finalize_stage.c` to 799 lines while preserving
     finalization decisions, durable `tip_finalize_log` writes, and the public
     stage API.
   - Twenty-first behavior-preserving extraction landed for reducer-frontier
     replay: low-level SQL probes, created-output backfill, inverse-delta
     checks, log-range deletion, dry-run transaction setup, and script/proof
     replay transaction bodies now live in `reducer_frontier_replay_tx.c`.
     This reduces `reducer_frontier_replay.c` to 800 lines while preserving
     repair detection, hash-split classification, blocker naming, and the
     existing private repair API.
   - Twenty-second behavior-preserving extraction landed for reducer-frontier
     reconcile-light: observability state, progress-meta readers, detail JSON,
     bypass/remedy counters, and test reset support now live in
     `reducer_frontier_light_observe.c`. This reduces
     `reducer_frontier_reconcile_light.c` to 583 lines while preserving
     detect/remedy/witness/progress semantics and the condition's public test
     hooks.
   - Twenty-third behavior-preserving extraction landed for boot snapshot
     failure memory: autodetect starter-pack selection, explicit
     `-load-snapshot-at-own-height` prior-marker handling, marker-unwritable
     fail-closed fallback, and success cleanup now live in
     `boot_snapshot_failure_memory.c`. This keeps `boot.c` focused on boot
     phase order while preserving the one-crash-then-P2P seed recovery policy
     and adding direct regression coverage.
   - Twenty-fourth behavior-preserving extraction landed for boot postmortem
     lifecycle: crash-capsule directory selection, seed-tape boot event,
     restart-time compression, and retention pruning now live in
     `boot_postmortem.c`. This keeps future boot reviews focused on phase
     ordering while preserving the existing boot postmortem signal-capture and
     restart-compression regressions.
   - Twenty-fifth behavior-preserving extraction landed for boot datadir lock
     lifecycle: PID-file path construction, running-PID refusal, stale-PID
     overwrite, and shutdown release now live in `boot_datadir_lock.c`. This
     keeps future boot reviews focused on phase ordering while preserving the
     single-process guard and non-fatal PID-file creation posture.
   - Twenty-sixth behavior-preserving extraction landed for boot shutdown
     marker lifecycle: WAL+clean-marker crash detection, crash-recovery event
     emission, boot-time marker consumption, and final clean marker write now
     live in `boot_shutdown_marker.c`. This keeps future boot reviews focused
     on phase ordering while preserving the existing clean-shutdown marker
     contract.
   - Twenty-seventh behavior-preserving extraction landed for boot stale-lock
     preflight: LevelDB/chainstate LOCK inspection, dead-PID cleanup, live-PID
     retention, and SQLite WAL notices now live in `boot_stale_locks.c`. This
     keeps future boot reviews focused on phase ordering while preserving
     startup recovery hygiene before SQLite opens.
   - Header-admit forward-fork liveness repair landed after deploy exposed a
     stale `header_admit_log` row at active_tip+1 whose parent was not the
     active tip. `header_admit` now clamps downstream reducer cursors to the
     replay point and rewinds its own cursor so the correct child is re-fetched
     and revalidated. Regression coverage lives in `test_header_admit_stage`.
   - Tip-finalize fork-lookahead guard landed after deploy exposed stale
     `tip_finalize_log` residue from accepting a best-header child whose parent
     did not match the finalized tip. `tip_finalize` now uses best-header
     ancestry to fill a missing lookahead only when it extends the resolved tip;
     fork candidates hold for the reorg/window owner instead of writing ok=0
     residue. Regression coverage lives in `test_tip_finalize_stage`.
   - Deploy verification now inspects the node log on RPC timeout and reports
     pre-RPC crash-only `reindex-chainstate` progress when an auto-reindex is
     the reason the socket never opened inside the deploy deadline. This keeps
     deploy fail-closed while naming the active recovery phase for operators and
     future agents.
   - Block-index dumpstate now resolves numeric heights through finalized
     active-chain slots first and best-header ancestry second, with an explicit
     `lookup_source`. This keeps above-tip reducer stalls inspectable without a
     new bespoke RPC.
   - Tip-fork-stale now covers the live same-height stale active-tip sibling
     shape as well as the older tip+1 stale-child shape. The condition compares
     block hashes instead of block-index pointer identity, captures a generic
     stale target, and keeps the same conservative guards: sustained no-advance,
     strictly higher-work best-header chain, and proof that the data-bearing
     target is off that best-header chain. Regression coverage lives in
     `test_tip_fork_stale`.
   - Header-admit best-header replay now requires an explicit contiguous
     parent proof before admitting above the active body window: active parent
     hash at active_tip+1, then prior `header_admit_log` hash for h+2 and
     beyond. This closes the live hot-loop where a stale h+1 row with the right
     parent shape let h+2 re-advance the cursor after every rewind. Regression
     coverage lives in `test_header_admit_stage`.
   - Tip-fork-stale now degrades cleanly when zclassicd cannot serve
     `rebuild_recent`: it invalidates the proven-stale fork target, then queues
     the best-header ancestor body via native P2P at that same height. This
     names the live same-height gap directly (canonical h=3165236 body missing,
     child h=3165237 already present) without adding a new condition or repair
     ladder. Regression coverage lives in `test_tip_fork_stale`.

5. **Long-lived dirty deployment discipline**
   - Fixed this pass: the live binary now reports `build_commit=29329bffe`, and
     the verifier was taught to check the same datadir/port that the service
     actually runs.

6. **Sovereign cure**
   - Continue `-refold-from-anchor` / self-verified tip work so the borrowed
     near-tip seed and its repair ladders can be deleted rather than hardened
     forever.
   - Next concrete seam: prove the remaining default boot routing/cutover path
     uses the self-derived anchor by default, then delete the borrowed-seed
     repair ladder per `docs/work/architecture-deletion-plan.md`.

## Review rule

For every future pass: pick one bounded subsystem, state the risk, patch only
proven defects, add a regression test, and run the narrow test plus `make lint`
before broad tests.

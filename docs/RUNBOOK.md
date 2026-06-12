# ZClassic23 Operator Runbook

Symptom-driven troubleshooting. Each section: what you see, how to diagnose, how to fix.

---

## Running the soak / chaos harnesses safely

Two opt-in harnesses spawn a **real** node. Both are **fully isolated**
and never touch the live node — but read this before running.

### Hard rails (enforced by `tools/scripts/isolated_node_env.sh`)

- They run on a throwaway `/tmp/zcl23-*` datadir and 39xxx ports only.
  They **refuse** to start if any chosen port is already `LISTEN`ing or
  is in the live set, or if the datadir would resolve under
  `~/.zclassic-c23*`.
- They never use `-tor`, always pass `-nobgvalidation -nolegacyimport`,
  and `-connect` a dead sink so they form 0 peers and cannot dial
  `zclassicd`.
- The spawned node is killed by **process group** and the datadir is
  `rm -rf`'d on exit (success, failure, or Ctrl-C).
- They are **NOT** in `make ci` (CI stays hermetic). Run them explicitly.

### Commands

```bash
# C7 full-binary kill-9 self-test: spawn an isolated regtest node,
# mine a seed, run kill/restart cycles, assert recovery invariants.
make test-crash-bootstrap

# C6 bounded compressed-soak PROXY: spawn an isolated regtest node,
# drive ~180s of synthetic generate-load, assert SOAK_OK.
make soak-ci
```

### Reading the output

- **Crash harness summary** prints `passes / height_regress /
  utxo_decrease / commitment_drift / utxo_above_tip / harness_errors`.
  Any non-zero failure column → exit 1. `over=-1` means
  "not-applicable" (no UTXO set, e.g. genesis-only).
- **Soak runner** writes a TSV log (`# ts<TAB>alive<TAB>height<TAB>rss_bytes`)
  and a trailer (`# ended=… verdict=… tip_hwm=… rss_max=… rss_baseline=…`).
  Exit status = verdict ordinal (`0 = SOAK_OK`). `make soak-ci` greps for
  `verdict=OK` as a false-green guard.

### Known build caveat

On the current build the regtest `generate` RPC does not solve Equihash,
so a self-spawned chain stays at genesis. The crash harness reports this
as `DEGRADED genesis-only recovery mode` (still validates boot recovery);
the soak proxy will report `FAIL_TIP_STALL` with `tip_hwm=0` and an
explicit NOTE that the **node miner**, not the harness, is the cause.
`make soak-ci` therefore goes red until a working regtest miner lands —
that is honest, not a harness defect.

### Escalating to the long / operational versions (OPERATOR actions)

These touch a **live or real-peer** environment — they are not hermetic:

- **`make soak-7day`** — the real 168 h MVP #6 soak against the
  *installed live node* under real tx load. Needs 7 days of wall time
  AND the live wedge cleared (node must hold tip AND finalize forward).
  The CI proxy is a signal, not this acceptance.
- **C7 `--with-peer`** two-node resync-to-peer-tip — the operational
  form of the literal MVP #7 "caught up to peer-tip within 2 min" claim;
  timing-sensitive, opt-in, not in default CI.

---

## Benign log patterns at tip

These patterns look alarming during a healthy soak but are **expected** on a
node that is holding tip and finalizing forward. Each entry cites the emitting
source file so a future reader can re-verify. A pattern stops being benign only
when it crosses the named **real-alarm** threshold — until then, do **not**
restart or intervene.

The shared mental model: the served tip and every derived projection (headers,
bodies, scripts, proofs, coins) converge a few seconds *after* each block lands.
Most "noise" below is one stage briefly observing a frontier that another stage
has not caught up to yet. At tip this self-resolves on the next tick; sustained
firing across many consecutive blocks is the actual signal.

### header-resync WARN storms when at tip

- **Pattern:**
  `[supervisor] staged.header_admit stalled (cursor=… admitted=…) — stage log behind live chain`,
  `[supervisor] staged.validate_headers stalled (cursor=… passed=… failed=…) — validator behind admit`,
  `[condition:header_stall_at_height] header=… peer_max=… age=…s action=kick_headers`,
  and `WARNING: Peer …: all N headers rejected — sync stalled!`
- **Emitted by:** `app/supervisors/src/staged_sync_supervisor.c:76,83`,
  `app/conditions/src/header_stall_at_height.c:74`, `lib/net/src/msg_headers.c:410`.
- **Why it fires:** at tip the node already holds every header a peer can offer,
  so a fresh `headers` message is 100% already-known and "all N rejected" is the
  duplicate-rejection path — not an invalid-header path. The staged supervisor
  WARNs whenever the header stage cursor momentarily trails the live chain (which
  it does for a few seconds after each new block until the admit/validate stages
  re-converge), and `header_stall_at_height` kicks a re-request as a precaution.
- **Why it is benign:** these are duplicate/known-header rejections and a
  precautionary re-request, not header-validation failures. The header chain is
  complete; the WARN is the stage describing a transient lag it then closes.
- **Real alarm if:** the same `staged.validate_headers stalled` line repeats with
  `failed>0` climbing (genuine header-validation failures, not duplicates), or
  `header_stall_at_height` keeps firing with `age` growing for many minutes while
  `peer_max` stays well above your header height — meaning headers are genuinely
  not advancing, not merely re-offered. Cross-check height against zclassicd.

### have_data_missing races at tip (body announced, not yet persisted)

- **Pattern:** `EV_BLOCK_REJECTED … tip_finalize … reason=have_data_missing`
  (also surfaces as the `tip_finalize` precondition reason `have_data_missing` /
  `block_missing` in `zcl_state subsystem=…` precondition fields).
- **Emitted by:** `app/jobs/src/tip_finalize_stage.c:173` (the
  `precondition_block_reason` → `record_precondition_block` path; see the TRANSIENT
  case comment at `tip_finalize_stage.c:390`).
- **Why it fires:** a block (or the tip's one-block lookahead successor H+1) has a
  header but its body has not finished the
  `body_persist → script_validate → utxo_apply` pipeline yet. `BLOCK_HAVE_DATA` is
  still clear for that index entry when the finalize stage looks.
- **Why it is benign:** it is a TRANSIENT precondition, handled explicitly: the
  finalize stage returns `JOB_IDLE` (cursor unchanged, the framework rolls back
  the txn so no junk row is written) and retries on the next tick once the body
  lands. H itself stays genuinely finalizable; only its lookahead witness is
  momentarily missing.
- **Real alarm if:** the SAME height stays `have_data_missing` for many
  consecutive ticks (minutes), i.e. a body that never arrives — that is a stuck
  body fetch, not a race. Confirm via `zcl_syncstate` (body frontier not
  advancing) and the `tip_advance_age_seconds` health check climbing.

### "database is locked" transients (SQLite WAL contention)

- **Pattern:** SQLite `SQLITE_BUSY` / `SQLITE_LOCKED` retries; the `database is
  locked` boot-table row notes a stale *second instance* (see "Boot Failure"
  below — that case is a real conflict, not this transient one).
- **Emitted by:** the bounded busy/locked retry loop in
  `app/services/src/chain_state_service.c:159-219`
  (`csr_sqlite_busy_or_locked` + `csr_set_last_persist_locked` only after the
  retry budget is exhausted), plus `sqlite3_busy_timeout(...)` set on the
  hot writers (e.g. `app/controllers/src/snapshot_controller_import.c:91`,
  `app/views/src/explorer_stats_view.c:387`).
- **Why it fires:** several stage writers (chain-state cursor, body persist, tx
  index, explorer projections) share one WAL-mode node.db. Under a burst of
  writes two of them briefly contend for the write lock; SQLite returns BUSY and
  the writer retries within its busy_timeout.
- **Why it is benign:** WAL contention is expected and the retry loop is bounded
  and succeeds — the persist completes on a later attempt within the timeout.
  Only after the retry budget is **exhausted** does the code call
  `csr_set_last_persist_locked(... "bounded retry exhausted")` and surface it.
- **Real alarm if:** you see the exhausted-retry surface (`last_persist_locked`
  set / "bounded retry exhausted"), or a `database is locked` that coincides with
  two live `zclassic23` PIDs on the same datadir (a real second-instance
  conflict — see the Boot Failure table). A bloated WAL (>100 MB) can also cause
  sustained contention; force a checkpoint (see "Disk > 99% Full").

### bg-validation undo-data-missing warnings

- **Pattern:** `[bg-valid] h=…: N non-coinbase tx(s) NOT script-verified (undo
  missing) — block advances, not fully verified`.
- **Emitted by:** `app/services/src/bg_validation_service.c:389-392`; the
  health surface notes the same in `app/controllers/src/health_controller.c:196`
  ("undo missing/mismatched — expected post-snapshot").
- **Why it fires:** background validation re-verifies historical blocks and wants
  each block's undo data to fully reconstruct inputs. Blocks brought in via a UTXO
  snapshot / fast-sync (rather than connected from genesis with full undo) do not
  carry undo data for the pre-snapshot range.
- **Why it is benign:** this is **expected post-snapshot**. The block still
  advances — its scripts were verified at connect time where data existed; only
  the optional historical re-verification is skipped for the undo-less range. The
  skip count is tallied (persisted across restarts) for honesty, not as an error.
- **Real alarm if:** `[bg-valid] script verification FAILED h=…`
  (`bg_validation_service.c:384`) appears — that is a genuine verification
  failure, not a missing-undo skip — or the skip count keeps growing for blocks
  that were connected normally (with undo data) rather than only the
  pre-snapshot range.

### crash-only auto-reindex ("auto-recovery" instead of a FATAL crash-loop)

- **Pattern:**
  `[boot] crash-only recovery: post-restore tip-above-extent at tip_h=… (zero_nbits=0, attempt M/3) — requesting -reindex-chainstate; restarting to rebuild from blocks/`
  followed on the next boot by
  `[boot] crash-only recovery: consuming auto-reindex request — rebuilding the UTXO set from block data (-reindex-chainstate)`.
- **Emitted by:** `config/src/boot_crashonly.c:22,70` (the recovery decision),
  written/consumed via `lib/storage/src/boot_auto_reindex.c`. Landed in commit
  `706a7c00a`, which turned the old FATAL crash-loop into this auto-recovery.
- **Why it fires:** a kill-9 mid-connect can leave the derived chain tip installed
  ABOVE the validated on-disk index extent. The post-restore integrity gate used
  to FATAL on this (crash-loop to systemd-FAILED, operator required). Now, since
  blocks/ + wallet are the only durable truth and the UTXO set is derived, the
  node RE-DERIVES it: it records a bounded reindex request and restarts to rebuild
  the UTXO set from blocks/ (`-reindex-chainstate`).
- **Why it is benign:** this is strictly safer than the old crash-loop in every
  case — it never deletes blocks/ or wallet, only rebuilds the derived UTXO set,
  and the request is bounded (max 3 attempts per anchor episode). A single
  request → restart → consume cycle that ends with the node serving is a
  successful self-heal, not an incident.
- **Real alarm if:** you see
  `[boot] crash-only recovery EXHAUSTED after N reindex attempts …`
  (`boot_crashonly.c:81`) — the bounded budget ran out, which means blocks/ is
  genuinely unable to back the tip and the node is paging the operator (a real
  corrupt-block-data signal), OR the same anchor keeps requesting a reindex across
  many restarts without ever converging.

### rpc-unreachable alerts during a deploy restart window (~60s)

- **Pattern:** external monitors / `mirror_status` showing `rpc-unreachable`,
  connection-refused, or "RPC did not become ready" briefly around a
  `make deploy` / `systemctl --user restart zclassic23`.
- **Emitted by:** the mirror blocker `lms_record_blocker("rpc-unreachable", …)`
  in `app/services/src/legacy_mirror_sync_service.c:270,299,516`
  (and the `mirror.rpc-unreachable` note in
  `app/services/src/mirror_divergence_locator.c:7`); the deploy/control path
  tolerates a readiness poll via `rpc_ready(c23, 90)` in `tools/zcl-nodectl.c:562`.
- **Why it fires:** during a restart the node process is down and then re-opening
  its datadir, rebuilding the block-index map, and binding the RPC socket. For
  that window the RPC port is not yet answering, so any poller sees connection
  refused / unreachable. The control tooling explicitly budgets up to ~90s for the
  RPC to come ready before treating it as a failure.
- **Why it is benign:** it is the expected restart gap. Once the node finishes
  boot and binds RPC, `rpc-unreachable` clears on its own and the mirror resumes.
- **Real alarm if:** RPC stays unreachable well past the readiness budget (node
  did not finish boot — check `journalctl --user -u zclassic23` for a boot
  failure or the crash-only auto-reindex flow above), or `rpc-unreachable`
  appears while the node process is **up and stable** (a bound/auth problem, not a
  restart window).

### val.block_rejected "block-not-finalized-by-reducer" single events at tip arrival

- **Pattern:** a single `EV_BLOCK_REJECTED` carrying
  `tip_finalize precondition_failed … reason=…` or the validation reason
  `block-not-finalized-by-reducer`, fired right as a new tip arrives.
- **Emitted by:** `app/jobs/src/tip_finalize_stage.c:294,380,439` (the finalize
  stage's transient/precondition emits) and the reducer-ingest read-back path
  `app/services/src/reducer_ingest_service.c:152` (returns
  `block-not-finalized-by-reducer` when the just-arrived block has not yet been
  finalized by the reducer at read-back time). The repair controller knows this
  string is benign: `app/controllers/src/repair_controller_rebuild.c:255,272`
  documents `not-finalized-by-reducer` as tip_finalize's one-block lookahead.
- **Why it fires:** when a block first arrives, the reducer ingests it but the
  finalize stage's one-block lookahead has not yet observed the successor, so a
  read-back of "is this block finalized?" momentarily answers no. A reorg
  cursor-rewind at the lookahead also emits `EV_BLOCK_REJECTED` describing the
  rewind (not a rejection of a valid block).
- **Why it is benign:** the event is the reducer/finalize machinery describing a
  transient lookahead/rewind state for the freshly-arrived tip, not a consensus
  rejection of a valid block. On the next tick the successor is observed and the
  tip finalizes; the convention-aware read-back
  (`reducer_ingest_service.c:138-149`) then answers yes.
- **Real alarm if:** the SAME height keeps emitting `block-not-finalized-by-reducer`
  across many ticks (the tip never finalizes — the live-wedge failure mode), or
  an `EV_BLOCK_REJECTED` carries a hard consensus reason (e.g. a script/proof
  failure from `script_validate_stage.c` / `proof_validate_stage.c`, or a
  `bad-txns-*` reason) rather than a transient finalize/rewind reason.

---

## BIP30 Stale Coinbase Wedge — fixed structurally (2026-05-26)

**Symptoms:** `zcl_status` shows the tip frozen (`tip_advance_age_seconds`
climbing, gap > 0) while legacy peers advance, and `node.log` repeats lines like
`bad-txns-BIP30` or `csr-tip-commit-rejected` at `tip+1`.

This was the stale coinbase / coins-overshoot shape: after a kill-9 mid-connect
the UTXO set lands at `tip+1` (or higher) while the cursor rewinds, so a retry
sees the block's own outputs already present and rejects it as BIP30 — a false
positive (post-BIP34 coinbase txids are height-unique).

**It now self-heals on restart — no manual unwedge.** The cure is structural,
not operational:
- `connect_block.c` tolerates a same-height self-write for *every* vtx (overwrites
  the stale coins instead of rejecting; full script/proof validation still runs).
- `chain_evidence_reconstruct.c` treats a coins-cursor lag/overshoot as a
  recoverable projection state, not a freeze — the tip publishes as `LOCAL_IMPORT`.
- `chain_evidence_controller.c` re-derives any stale freeze on boot and lifts it
  when the tip is provably consistent.

**Diagnose, read-only:**
```bash
build/bin/zcl-rpc healthcheck | jq '.checks.chain_advance'   # tip_advance_age + named blocker
build/bin/zcl-rpc getblockcount                              # is it climbing?
```

If the tip is genuinely stuck, the live `chain_evidence` state names the precise
reason (`zcl_state subsystem=chain_evidence`) — a blocker, never a silent halt.
Recovery is a plain `systemctl --user restart zclassic23` (or `make deploy` for a
new binary); boot routes through the tolerant `chain_restore` + evidence
re-derivation path. Full forensic history: the `project_bip30_stale_coins_wedge`
memory note.

**Do not:** manually delete UTXO ranges or bypass BIP30. The fix is bounded to a
same-height self-write; a different-height duplicate is still a hard rejection.

---

## Disk > 99% Full

**Symptoms:** `EV_DISK_CRITICAL` events, node may refuse new blocks, SQLite writes fail.

**Diagnose:**
```bash
df -h $(build/bin/zclassic23 -datadir 2>/dev/null || echo ~/.zclassic-c23)
du -sh ~/.zclassic-c23/*
build/bin/zcl-rpc healthcheck | jq '.disk'
```

**Fix:**
1. Prune old debug logs: `rm -f ~/.zclassic-c23/debug.log.old*`
2. Remove stale peer data: `rm -f ~/.zclassic-c23/peers.dat.bak`
3. If WAL is bloated (>100MB), force checkpoint:
   ```bash
   sqlite3 ~/.zclassic-c23/node.db 'PRAGMA wal_checkpoint(TRUNCATE);'
   ```
4. If block files are consuming >5GB and you don't need full history, enable block pruning (`app/services/src/block_pruning_service.c`).
5. Move datadir to larger volume: stop node, `mv ~/.zclassic-c23 /mnt/bigger/`, symlink or use `-datadir=`.

**Prevention:** Set `ZCL_WAL_MAX_BYTES=104857600` (100MB cap, auto-checkpoint). Monitor `zcl_disk_free_bytes` in Grafana with alert at 1GB.

---

## Peer Misbehaving / Banned

**Symptoms:** `EV_PEER_MISBEHAVE` or `EV_PEER_BANNED` events. Peer count dropping.

**Diagnose:**
```bash
build/bin/zcl-rpc getpeerinfo | jq '.[] | {id, addr, banscore, subver}'
# Via MCP:
# zcl_peers → check banscore field
# zcl_peer_report → offence breakdown by kind
# zcl_events → filter for peer.misbehave, peer.banned
```

**Fix:**
1. If a specific peer is spamming bad blocks/txs, disconnect:
   ```bash
   build/bin/zcl-rpc addnode "IP:PORT" "remove"
   ```
2. If legitimate peers are getting banned (false positive), check whether the node's chain is correct:
   ```bash
   build/bin/zcl-rpc getblockchaininfo | jq '{blocks, bestblockhash}'
   # Compare height with a known-good explorer
   ```
3. If your node is on a stale fork, see **Tip Regressed / Stuck on Wrong Fork** below.

**Prevention:** Monitor `zcl_peer_offences_total` rate in Grafana. A sudden spike in `invalid_block` offences usually means your node or the peers are on a bad chain.

---

## Public Node Strength

**Symptoms:** The node is synced but public P2P looks weak: peers stay in `connecting`, no completed MagicBean or ZClassic-C23 handshakes, or watchdog logs repeat `PEER_FLOOR`, `HEADER_STALL`, or `STATE_STUCK`.

**Diagnose:**
```bash
build/bin/zcl-rpc getnetworkinfo | jq '{
  connections, inbound_connections, outbound_connections,
  handshaked_connections, inbound_handshaked_connections,
  outbound_handshaked_connections, inbound_handshake_seen,
  remote_handshake_seen, magicbean_peers, zclassic_c23_peers,
  localservices, localaddresses, listening,
  peer_lifecycle
}'

build/bin/zcl-rpc getpeerinfo | jq '.[] | {
  id, addr, state, inbound, subver, magicbean, zclassic_c23,
  startingheight, lifecycle
}'

build/bin/zcl-rpc dumpstate peer_lifecycle | jq '{
  summary: .state.summary,
  sources: [.state.sources[] | {
    source, attempted, connected, handshake_complete,
    active, disconnected, timeout, rejected,
    magicbean_handshakes, zclassic_c23_handshakes
  }]
}'
build/bin/zcl-rpc healthcheck | jq '.checks.chain_advance'
build/bin/zcl-rpc dumpstate chain_advance_coordinator | jq '{
  initialized, has_connman, has_main_state, has_node_db,
  authority, decision, selected_source, activation_allowed,
  mirror_fallback_allowed, local_height, target_height, reason,
  blocker, sources: [.sources[] | {
    source, trust, available, healthy, blocked, selectable,
    selection_blocker, score, state,
    score_base, score_health, score_height, score_authorized,
    score_target_lag_penalty, score_failure_penalty,
    score_mirror_gate_penalty,
    addnode_tcp_failures, addnode_protocol_failures,
    reason, blocker
  }]
}'
build/bin/zcl-rpc dumpstate legacy_mirror | jq '{
  state, lag, activation_blocker, last_blocker_code,
  stuck_reason, stuck_height, stalls_total,
  consensus_authority, candidate_source, candidate_trust, candidate_lag,
  candidate_blocker, overrides_total, unsafe_overrides_total,
  blockers_total, last_override_safe, last_override_scope, last_error
}'
```

**Fix:**
1. **No completed handshakes:** check outbound reachability and peer quality before restarting.
   ```bash
   ss -tlnp | grep 8033
   build/bin/zcl-rpc addnode "IP:8033" "onetry"
   build/bin/zcl-rpc dumpstate peer_lifecycle | jq '.state.sources[] | select(.timeout>0 or .rejected>0 or .handshake_complete==0)'
   build/bin/zcl-rpc dumpstate peer_lifecycle | jq '.state.peers[] | select(.timeout>0 or .rejected>0)'
   ```
2. **External IP missing or wrong:** set `-externalip=<public-ip>` in the service environment and verify it appears in `getnetworkinfo.localaddresses`. For public reachability, `inbound_handshake_seen=true` or `inbound_handshaked_connections > 0` is stronger evidence than outbound-only handshakes.
3. **Only `connecting` peers:** prefer fresh addnodes from known ZClassic peers. `peer_lifecycle.sources[]` shows whether failures are concentrated in `addnode`, `addrman`, `manual`, `zcl23_db`, or `inbound`; the coordinator dump distinguishes TCP failures (`addnode_tcp_failures`) from post-connect protocol/handshake failures (`addnode_protocol_failures`).
4. **Coordinator blocked or waiting:** use `dumpstate chain_advance_coordinator` first. `initialized=true` plus `has_connman=true`, `has_main_state=true`, and `has_node_db=true` confirm the running daemon has the coordinator wired into live P2P, chainstate, and persistence. `authority` must remain `local_consensus_validation`; `selected_source` shows the best current input, `selected_source_trust`/`sources[].trust` explain its trust class, and `sources[].selectable=false` with `selection_blocker` explains why a source was excluded before score ranking. `activation_allowed=false` or a non-empty `blocker` explains why the node is refusing to advance.
5. **Legacy advisory active:** legacy data may be used only as `candidate_source=legacy_advisory`. `legacy_mirror.state` should be `observing`, `catching_up`, or `healthy`. Treat `blocked`, `gated_by_local_retries`, or `legacy_advisory_gated_by_native_retries=true` as actionable states; inspect `candidate_blocker`, `last_blocker_code`, `stuck_reason`, `stalls_total`, `blockers_total`, `unsafe_overrides_total`, `last_override_safe`, `last_override_scope`, and `last_error`. `consensus_authority` must stay `local_consensus_validation`; `candidate_trust` describes candidate data, not a co-authority. `unsafe_overrides_total` must stay `0` on a healthy node; any increase fails health and means an unsafe override path was reached. Stop trusting legacy-assisted recovery until the cause is understood and local validation has re-established the tip.
6. **When not to restart:** if `chain_advance.decision` is `use_source` or `wait` with a clear reason, `lag <= 1`, and peer lifecycle shows active handshakes, leave the node running. Restarting resets peer reputation and can make public reachability look worse for a few minutes.

**Prevention:** Alert when `handshaked_connections == 0` for 5 minutes, `peer_lifecycle.timeout` rises quickly, `chain_advance.decision == "blocked"`, or `legacy_mirror.state == "blocked"`.

---

## Wallet Backup Failed

**Symptoms:** `EV_WALLET_BACKUP_FAILED` event. `zcl_status` health check shows wallet backup warning.

**Diagnose:**
```bash
build/bin/zcl-rpc healthcheck | jq '.wallet'
ls -la ~/.zclassic-c23/node.db
# Check if backup destination is writable
ls -la ~/.zclassic-c23/backups/
```

**Fix:**
1. If backup directory doesn't exist: `mkdir -p ~/.zclassic-c23/backups`
2. If permissions: `chmod 700 ~/.zclassic-c23/backups`
3. If disk full: see **Disk > 99% Full** above.
4. Manual backup while node is running (SQLite online backup is safe):
   ```bash
   sqlite3 ~/.zclassic-c23/node.db ".backup '~/.zclassic-c23/backups/node-$(date +%Y%m%d).db'"
   ```

**Prevention:** The node's built-in backup service runs automatically. Verify it works after first boot by checking for `EV_WALLET_BACKUP` events.

---

## Tip Regressed / Stuck on Wrong Fork

**Symptoms:** Chain height decreases or lags significantly behind network. `EV_REORG_START` with large depth. Peers disconnecting because of chain mismatch.

**Diagnose:**
```bash
build/bin/zcl-rpc getblockchaininfo | jq '{blocks, headers, bestblockhash, difficulty}'
build/bin/zcl-rpc getpeerinfo | jq '.[0:3] | .[] | {addr, startingheight, synced_headers}'
# Compare your tip hash against a trusted peer or explorer
# Via MCP: zcl_syncstate, zcl_dataintegrity
```

**Fix:**
1. If tip is just behind (node is syncing): wait. Check `zcl_syncstate` — if state is `BLOCKS_DOWNLOAD` or `CONNECTING_BLOCKS`, sync is in progress.
2. If tip regressed after a reorg:
   - Small reorg (<10 blocks): normal, node should recover automatically. Watch `EV_REORG_RECOVERY_COMPLETE`.
   - Large reorg (>10 blocks): investigate. Check if peers agree on the fork:
     ```bash
     build/bin/zcl-rpc getpeerinfo | jq '.[] | {addr, startingheight}'
     ```
3. If node is stuck on a dead fork (no peers agree), see the
   nuclear option below — invalidateblock / reconsiderblock RPCs are
   not currently exposed.
4. Nuclear option (last resort): stop node, delete state, resync:
   ```bash
   systemctl --user stop zclassic23
   rm -f ~/.zclassic-c23/node.db ~/.zclassic-c23/node.db-{wal,shm}
   rm -f ~/.zclassic-c23/block_index.bin{,.sha3}
   systemctl --user start zclassic23
   # Node will rebuild from block files, snapshot sync, or -cold-import.
   ```

**Prevention:** Run background validation (`-nobgvalidation` NOT set). Monitor `zcl_chain_height` derivative — alert if zero for >10 minutes while peers show higher heights.

---

## Node Stuck (Not Syncing)

**Symptoms:** Chain height frozen. `zcl_syncstate` returns `IDLE` or `FAILED`. No blocks connecting.

**Diagnose:**
```bash
build/bin/zcl-rpc getpeerinfo | jq 'length'        # Any peers?
build/bin/zcl-rpc getpeerinfo | jq '.[] | .addr'    # Who's connected?
build/bin/zcl-rpc getnetworkinfo | jq '{connections, localservices}'
# Via MCP: zcl_syncstate, zcl_peers
```

**Fix:**
1. **Zero peers:** Network issue or all seeds down.
   ```bash
   # Add a known peer manually
   build/bin/zcl-rpc addnode "IP:PORT" "onetry"
   # Check firewall — P2P port 8033 must be reachable
   ss -tlnp | grep 8033
   ```
2. **Has peers but no blocks:** Peers may be stale or node is rejecting valid blocks.
   ```bash
   # Check recent events for reject reasons
   build/bin/zcl-rpc eventlog | grep -i reject
   # Via MCP: zcl_events, zcl_consensus_report
   ```
3. **Sync state is FAILED:**
   ```bash
   # Restart the node — transient failures often clear on restart
   systemctl --user restart zclassic23
   ```
4. **Stuck in SNAPSHOT_RECEIVE:** Snapshot peer may have disconnected.
   ```bash
   # Restart to retry snapshot from another peer
   systemctl --user restart zclassic23
   ```

**Prevention:** Monitor sync state. Alert on `FAILED` state or height stalled >10 min.

---

## RPC 429 (Rate Limited)

**Symptoms:** RPC clients receive HTTP 429 responses. `EV_RPC_TIMEOUT` events. `zcl_rpc_rate_limited_*` counters climbing.

**Diagnose:**
```bash
# Via MCP: zcl_metrics → look at rpc_rate_limited_global, rpc_rate_limited_per_ip
# Check current limits
echo "Global: ${ZCL_RPC_RPS:-50} rps, burst ${ZCL_RPC_BURST:-100}"
echo "Per-IP: ${ZCL_RPC_PER_IP_RPS:-5} rps, burst ${ZCL_RPC_PER_IP_BURST:-10}"
```

**Fix:**
1. If your own tooling is hitting per-IP limits, increase per-IP budget:
   ```bash
   export ZCL_RPC_PER_IP_RPS=20
   export ZCL_RPC_PER_IP_BURST=40
   systemctl --user restart zclassic23
   ```
2. If global limit is hit by many clients, raise global:
   ```bash
   export ZCL_RPC_RPS=200
   export ZCL_RPC_BURST=400
   systemctl --user restart zclassic23
   ```
3. If a specific IP is flooding, it will auto-ban after `ZCL_RPC_AUTH_FAIL_THRESHOLD` (default 5) auth failures. For non-auth flooding, the per-IP rate limit handles it.
4. Loopback (127.0.0.1) bypasses per-IP limits but still counts against global. If your local tools are fighting each other for global budget, raise `ZCL_RPC_RPS`.

**Prevention:** Monitor `zcl_rpc_rate_limited_*` in Grafana. Right-size limits for your deployment.

---

## RPC Auth Failures / Unexpected Bans

**Symptoms:** Legitimate clients get 403. `EV_PEER_BANNED` on RPC layer. `zcl_rpc_auth_failures` climbing.

**Diagnose:**
```bash
# Check if cookie file exists and is readable
ls -la ~/.zclassic-c23/.cookie
cat ~/.zclassic-c23/.cookie
# Check if client is using the current cookie (rotates every 24h by default)
echo "Rotation interval: ${ZCL_RPC_COOKIE_ROTATE_SEC:-86400}s"
```

**Fix:**
1. If cookie file is stale or missing: restart node to regenerate.
2. If client caches the cookie: client must re-read `.cookie` file on 401 response. During rotation, both current and previous cookies are valid.
3. If IP is banned from too many auth failures:
   - Ban auto-expires after `ZCL_RPC_BAN_SECONDS` (default 3600s = 1hr).
   - To clear immediately: restart the node (ban table is in-memory).
4. If using `rpcuser`/`rpcpassword` instead of cookie: rotation doesn't apply, check credentials.

**Prevention:** Ensure clients re-read the `.cookie` file periodically (at least once per rotation interval).

---

## High Memory Usage

**Symptoms:** Process RSS growing unbounded. OOM killer risk.

**Diagnose:**
```bash
ps aux | grep zclassic23 | grep -v grep
cat /proc/$(pgrep zclassic23)/status | grep -E 'VmRSS|VmPeak'
# Via MCP: zcl_dbstats → check cache sizes
```

**Fix:**
1. If background validation is consuming too much: disable with `-nobgvalidation` or restart (it auto-detects <8GB machines and reduces batch size).
2. If UTXO cache is large: the node batches flushes. A restart forces a flush and reclaims memory.
3. If mempool is bloated:
   ```bash
   build/bin/zcl-rpc getmempoolinfo | jq '{size, bytes}'
   # Mempool has configurable limits — check environment
   ```

**Prevention:** Monitor RSS via external tool (e.g., `node_exporter` for Prometheus). Background validation is the biggest consumer — disable on RAM-constrained machines.

---

## Boot Failure (Node Won't Start)

**Symptoms:** Service fails to start. `journalctl --user -u zclassic23` shows errors.

**Diagnose:**
```bash
journalctl --user -u zclassic23 --since "5 min ago" --no-pager
# Look for EV_BOOT_VALIDATION_FAILED or specific error messages
```

**Fix by error type:**

| Error | Fix |
|-------|-----|
| `database is locked` | Another instance is running. `pgrep zclassic23`. Kill stale process. |
| `block index corrupt` | `EV_BLOCK_INDEX_CORRUPT`. Delete and rebuild: `rm ~/.zclassic-c23/block_index.bin{,.sha3}; restart` |
| `node.db corrupt` | Delete `node.db*`, restart — will rebuild from block files or snapshot. |
| `schema version mismatch` | Node was downgraded. Use the matching binary version or delete+resync. |
| `permission denied` | `chmod 700 ~/.zclassic-c23; chown -R $USER ~/.zclassic-c23` |
| `address already in use` | Port conflict. `ss -tlnp | grep 8033`. Change with `-port=` or stop conflicting service. |

**Prevention:** Don't run multiple instances against the same datadir. Use `-datadir=` for test instances.

---

## Quick Reference: Environment Variables

| Variable | Default | Description |
|----------|---------|-------------|
| `ZCL_RPC_RPS` | 50 | Global RPC rate limit (requests/sec) |
| `ZCL_RPC_BURST` | 100 | Global RPC burst bucket |
| `ZCL_RPC_PER_IP_RPS` | 5 | Per-IP RPC rate limit |
| `ZCL_RPC_PER_IP_BURST` | 10 | Per-IP burst bucket |
| `ZCL_RPC_AUTH_FAIL_THRESHOLD` | 5 | Auth failures before auto-ban |
| `ZCL_RPC_BAN_SECONDS` | 3600 | Ban duration (seconds) |
| `ZCL_RPC_COOKIE_ROTATE_SEC` | 86400 | Cookie rotation interval (0=disable) |
| `ZCL_MCP_GLOBAL_RPS` | 100 | MCP global rate limit |
| `ZCL_MCP_DESTRUCTIVE_RPS` | 1 | MCP write-op rate limit |
| `ZCL_MCP_TIMEOUT_MS` | 5000 | MCP per-tool timeout |
| `ZCL_MCP_BEARER_TOKEN` | (none) | MCP auth token (optional) |
| `ZCL_WAL_MAX_BYTES` | 104857600 | SQLite WAL size cap (bytes) |
| `ZCL_METRICS_HTTP_ENABLE` | 0 | Expose /metrics Prometheus endpoint |

## Quick Reference: Key Events

| Event | Severity | Meaning |
|-------|----------|---------|
| `EV_DISK_CRITICAL` | Critical | Disk usage above critical threshold |
| `EV_BOOT_VALIDATION_FAILED` | Critical | Boot aborted — data corruption or config error |
| `EV_REORG_DISCONNECT_FAILED` | Critical | Reorg stuck — manual intervention likely needed |
| `EV_DB_TXN_LEAKED` | High | Database transaction not committed or rolled back |
| `EV_COINS_FLUSH_FAILED` | High | UTXO persistence failed — data loss risk |
| `EV_WALLET_BACKUP_FAILED` | High | Wallet backup did not complete |
| `EV_PEER_BANNED` | Medium | Peer auto-banned for misbehavior |
| `EV_CONSENSUS_REJECT_BLOCK` | Medium | Received invalid block from peer |
| `EV_REORG_START` | Medium | Chain reorganization in progress |
| `EV_DISK_LOW` | Warning | Disk usage approaching limit |
| `EV_IBD_THROTTLED` | Info | Initial block download rate-limited (saves bandwidth) |
| `EV_PEER_THROTTLED` | Info | Peer bandwidth throttled by token bucket |

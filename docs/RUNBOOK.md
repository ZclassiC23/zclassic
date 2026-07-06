# ZClassic23 Operator Runbook

Symptom-driven troubleshooting. Each section: what you see, how to diagnose, how to fix.

## Soak / chaos harnesses

Run `make test-crash-bootstrap` (C7 kill-9) and `make soak-ci` (C6 soak proxy)
to exercise a real but fully isolated node. For the isolation contract, build
caveat, output/verdict reading, and the operational forms (`make soak-7day` =
MVP #6, C7 `--with-peer` = MVP #7), see [`CHAOS_HARNESS.md`](./CHAOS_HARNESS.md).

---

## High-Availability First Check

For live operators, start with one read-only topology probe before deciding to
restart anything:

```bash
build/bin/zclassic23 agentops
build/bin/zclassic23 agent
build/bin/zclassic23 agentliveness
```

`agentops` is the no-jq command center: it names the preferred transport,
diagnostic drill-down commands, lane safety, runtime availability, and the next
architecture work. `agent` is the compact live health packet. `agentliveness`
checks whether the lane, supervisor tree, and background quality lanes are
actually alive.

Do not restart on a single stale-looking field if these probes show active
services, handshaked peers, and bounded mirror lag. Drill down with
`build/bin/zclassic23 getmirrorstatus`,
`build/bin/zclassic23 dumpstate reducer_frontier`, and
`build/bin/zclassic23 dumpstate peer_lifecycle` only when the first-call JSON
names a concrete problem.

---

## Benign log patterns at tip

On a node holding tip and finalizing forward, these patterns look alarming but
are **expected**. Mental model: the served tip and every derived projection
(headers, bodies, scripts, proofs, coins) converge a few seconds *after* each
block lands; most "noise" is one stage briefly observing a frontier another
hasn't caught up to. This self-resolves next tick — sustained firing across many
consecutive blocks is the real signal. Do **not** restart until a pattern
crosses its **real-alarm** threshold.

| Pattern | Meaning (emitted by) | Benign? | Real alarm if |
|---------|----------------------|---------|---------------|
| **header-resync WARN storm** — `staged.header_admit/validate_headers stalled …`, `condition:header_stall_at_height … action=kick_headers`, `Peer …: all N headers rejected` | At tip the node already holds every header a peer offers; "all N rejected" is the duplicate-rejection path, plus a precautionary re-request. `staged_sync_supervisor.c:76,83`, `header_stall_at_height.c:74`, `msg_headers.c:410` | Yes | `validate_headers stalled` repeats with `failed>0` climbing (genuine validation failures, not dups), or `header_stall_at_height` keeps firing with `age` growing for minutes while `peer_max` stays well above your height. Cross-check height vs zclassicd. |
| **have_data_missing race** — `EV_BLOCK_REJECTED … tip_finalize … reason=have_data_missing` / `block_missing` | A block (or tip's H+1 lookahead) has a header but its body hasn't finished `body_persist → script_validate → utxo_apply`. Finalize returns `JOB_IDLE` (cursor unchanged, txn rolled back) and retries next tick. `tip_finalize_stage.c:173` (TRANSIENT case at `:390`) | Yes | The SAME height stays `have_data_missing` for many consecutive ticks (minutes) — a body that never arrives. Confirm via `zcl_syncstate` (body frontier not advancing) + `tip_advance_age_seconds` climbing. |
| **"database is locked" transient** — SQLite `SQLITE_BUSY`/`SQLITE_LOCKED` retries | Stage writers (chain-state cursor, body persist, tx index, explorer projections) share one WAL `node.db`; under a write burst two briefly contend and retry within `busy_timeout`. Bounded retry loop `chain_state_service.c:159-219`; `sqlite3_busy_timeout` on hot writers e.g. `snapshot_controller_import.c:91`, `explorer_stats_view.c:387` | Yes (retry succeeds) | The exhausted-retry surface appears (`last_persist_locked` set / "bounded retry exhausted"), or `database is locked` coincides with two live `zclassic23` PIDs on one datadir (real second-instance — see Boot Failure). A bloated WAL (>100 MB) can sustain contention — force a checkpoint (see "Disk > 99% Full"). |
| **bg-validation undo-data-missing** — `[bg-valid] h=…: N non-coinbase tx(s) NOT script-verified (undo missing) — block advances, not fully verified` | Snapshot/fast-sync blocks carry no undo data for the pre-snapshot range; scripts were verified at connect time, only optional historical re-verify is skipped. Skip count tallied for honesty. `bg_validation_service.c:389-392`; `health_controller.c:196` | Yes (expected post-snapshot) | `[bg-valid] script verification FAILED h=…` (`bg_validation_service.c:384`) appears, or the skip count grows for blocks connected normally (with undo data), not just the pre-snapshot range. |
| **crash-only auto-reindex** — `[boot] crash-only recovery: post-restore tip-above-extent … requesting -reindex-chainstate; restarting …` then `… consuming auto-reindex request — rebuilding the UTXO set from block data` | A kill-9 mid-connect left the derived tip above the validated on-disk extent. blocks/ + wallet are the only durable truth and the UTXO set is derived, so the node bounded-requests a rebuild and restarts. Never deletes blocks/ or wallet; max 3 attempts/anchor. `boot_crashonly.c:22,70`; `boot_auto_reindex.c` (commit `706a7c00a`) | Yes (strictly safer self-heal) | `[boot] crash-only recovery EXHAUSTED after N reindex attempts …` (`boot_crashonly.c:81`) — blocks/ genuinely can't back the tip (real corrupt-block-data), or the same anchor keeps requesting a reindex without converging. |
| **rpc-unreachable during deploy** — monitors / `mirror_status` show `rpc-unreachable` / connection-refused briefly around `make deploy` / restart | The process is down then re-opening the datadir, rebuilding the index map, binding RPC; the port isn't answering yet. If crash-only auto-reindex is active, `deploy_verify.sh` reports `boot diagnostic: pre-RPC recovery: reindex-chainstate ...` from `node.log` instead of only the socket error. Control tooling budgets ~90s (`rpc_ready(c23, 90)` `zcl-nodectl.c:562`). `tools/deploy_verify.sh`; `legacy_mirror_sync_service.c:270,299,516`; `mirror_divergence_locator.c:7` | Yes (expected restart gap) | RPC stays unreachable well past the readiness budget with no named boot diagnostic (boot didn't finish — check `journalctl --user -u zclassic23` or the crash-only flow above), reindex progress stops for a long interval, or `rpc-unreachable` appears while the process is **up and stable** after READY (a bind/auth problem). |
| **block-not-finalized-by-reducer single event** — one `EV_BLOCK_REJECTED … tip_finalize precondition_failed …` / reason `block-not-finalized-by-reducer` right as a new tip arrives | The reducer ingested the block but finalize's one-block lookahead hasn't seen the successor yet, so a read-back momentarily answers no; a reorg cursor-rewind also emits this. `tip_finalize_stage.c:294,380,439`; `reducer_ingest_service.c:152` (read-back), known-benign at `repair_controller_rebuild.c:255,272` | Yes | The SAME height keeps emitting `block-not-finalized-by-reducer` across many ticks (tip never finalizes — the live-wedge mode), or an `EV_BLOCK_REJECTED` carries a hard consensus reason (script/proof failure from `script_validate_stage.c`/`proof_validate_stage.c`, or `bad-txns-*`). |
| **`tip_stale` during a slow block** — `/api/health` / `healthcheck` stays `healthy=true`, `serving=true`, and reports `status.warning_reasons="tip_stale"` | `tip_stale = (now - tip->nTime) > 600` (`node_health_service.c`). Target interval is 2.5 min; Poisson variance puts honest gaps past 600s, so a synced node can report this several times a day. | Yes (chain is slow, not the node — peer height matches, `tip_lag=0`) | `tip_stale` persists while peer heights are ahead of the node, `tip_lag` grows, or `status.blocking_reason` becomes non-null — a genuine stall, see "Tip Regressed / Stuck on Wrong Fork". |

---

## BIP30 Stale Coinbase Wedge — fixed structurally (2026-05-26)

**Symptoms:** tip frozen (`tip_advance_age_seconds` climbing, gap > 0) while
legacy peers advance; `node.log` repeats `bad-txns-BIP30` / `csr-tip-commit-rejected`
at `tip+1`.

**Shape:** after a kill-9 mid-connect the UTXO set lands at `tip+1`+ while the
cursor rewinds, so a retry sees the block's own outputs already present and
false-rejects it as BIP30 (post-BIP34 coinbase txids are height-unique).

**It self-heals on restart — no manual unwedge.** The cure is structural:
`connect_block.c` tolerates a same-height self-write for every vtx (full
script/proof validation still runs); `chain_evidence_reconstruct.c` treats a
coins-cursor lag/overshoot as recoverable (tip publishes as `LOCAL_IMPORT`);
`chain_evidence_controller.c` re-derives any stale freeze on boot.

**Diagnose, read-only:**
```bash
build/bin/zclassic23 agent
build/bin/zclassic23 dumpstate chain_advance_coordinator
build/bin/zclassic23 getblockcount
```

If genuinely stuck, `zcl_state subsystem=chain_evidence` names the precise reason
(a blocker, never a silent halt). Recovery is `systemctl --user restart zclassic23`
(or `make deploy` for a new binary). Forensics: `project_bip30_stale_coins_wedge`.

**Do not** manually delete UTXO ranges or bypass BIP30 — the fix is bounded to a
same-height self-write; a different-height duplicate is still a hard rejection.

---

## Disk > 99% Full

**Symptoms:** `EV_DISK_CRITICAL`, node may refuse new blocks, SQLite writes fail.

**Diagnose:**
```bash
df -h $(build/bin/zclassic23 -datadir 2>/dev/null || echo ~/.zclassic-c23)
du -sh ~/.zclassic-c23/*
build/bin/zclassic23 dumpstate disk_monitor
```

**Fix:**
1. Prune old debug logs: `rm -f ~/.zclassic-c23/debug.log.old*`
2. Remove stale peer data: `rm -f ~/.zclassic-c23/peers.dat.bak`
3. If WAL is bloated (>100MB), force checkpoint:
   ```bash
   sqlite3 ~/.zclassic-c23/node.db 'PRAGMA wal_checkpoint(TRUNCATE);'
   ```
4. If block files consume >5GB and you don't need full history, enable block pruning (`app/services/src/block_pruning_service.c`).
5. Move datadir to larger volume: stop node, `mv ~/.zclassic-c23 /mnt/bigger/`, symlink or use `-datadir=`.

**Prevention:** Set `ZCL_WAL_MAX_BYTES=104857600` (100MB cap, auto-checkpoint). Monitor `zcl_disk_free_bytes` in Grafana with alert at 1GB.

---

## Peer Misbehaving / Banned

**Symptoms:** `EV_PEER_MISBEHAVE` / `EV_PEER_BANNED`. Peer count dropping.

**Diagnose:**
```bash
build/bin/zclassic23 getpeerinfo
build/bin/zclassic23 dumpstate peer_lifecycle
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
   build/bin/zclassic23 getblockchaininfo
   # Compare height with a known-good explorer
   ```
3. If your node is on a stale fork, see **Tip Regressed / Stuck on Wrong Fork**.

**Prevention:** Monitor `zcl_peer_offences_total` rate in Grafana. A sudden spike in `invalid_block` offences usually means your node or the peers are on a bad chain.

---

## Public Node Strength

**Symptoms:** synced but public P2P looks weak: peers stay `connecting`, no
completed legacy-compatible or ZClassic23 handshakes, or watchdog repeats
`PEER_FLOOR`, `HEADER_STALL`, or `STATE_STUCK`.

**Diagnose:** these read-only native JSON calls cover the P2P/advance picture:
```bash
build/bin/zclassic23 agent
build/bin/zclassic23 getnetworkinfo
build/bin/zclassic23 getpeerinfo
build/bin/zclassic23 healthcheck
build/bin/zclassic23 dumpstate peer_lifecycle incidents
build/bin/zclassic23 dumpstate peer_lifecycle
build/bin/zclassic23 dumpstate chain_advance_coordinator
build/bin/zclassic23 dumpstate legacy_mirror
```

**Fix:**
1. **No completed handshakes:** check outbound reachability and peer quality before restarting.
   ```bash
   ss -tlnp | grep 8033
   build/bin/zcl-rpc addnode "IP:8033" "onetry"
   # peer_lifecycle incidents: inspect top_incidents and duplicate_host_groups
   ```
2. **External IP missing or wrong:** set `-externalip=<public-ip>[:p2p-port]`
   in the service environment and verify it appears in
   `getnetworkinfo.localaddresses`. Include `:p2p-port` when the public port
   differs from the local listen port. For public reachability,
   `inbound_handshake_seen=true` or `inbound_handshaked_connections > 0` is
   stronger evidence than outbound-only handshakes.
3. **Only `connecting` peers:** prefer fresh addnodes from known ZClassic peers. The compact peer incident view shows reconnect pressure, duplicate host groups, direction, handshake age, advertised height, services, and whether a peer is currently useful for bootstrap. The full `peer_lifecycle.sources[]` view shows whether failures concentrate in `addnode`, `addrman`, `manual`, `zcl23_db`, or `inbound`; the coordinator dump distinguishes TCP failures (`addnode_tcp_failures`) from post-connect protocol/handshake failures (`addnode_protocol_failures`).
4. **Coordinator blocked or waiting:** use `dumpstate chain_advance_coordinator` first. `initialized=true` plus `has_connman=true`, `has_main_state=true`, `has_node_db=true` confirm the coordinator is wired into live P2P, chainstate, and persistence. `authority` must stay `local_consensus_validation`; `selected_source` shows the best input, `selected_source_trust`/`sources[].trust` explain its trust class, and `sources[].selectable=false` with `selection_blocker` explains why a source was excluded before score ranking. `activation_allowed=false` or a non-empty `blocker` explains why the node refuses to advance.
5. **Legacy advisory active:** legacy data may be used only as `candidate_source=legacy_advisory`. Read the mirror fields as three separate facts: `mirror_monitor_running` means the zclassic23 monitor loop is alive, `zclassicd_rpc_transport_reachable` means the C++ RPC answered at the HTTP/JSON-RPC layer, and `legacy_oracle_usable` means it supplied a usable height/hash oracle. `rpc error -28: Activating best chain...` should be `zclassicd_rpc_transport_reachable=true` and `legacy_oracle_usable=false`. When `active_source=p2p` or another native source and `candidate_blocker_scope=advisory_only`, the node is not blocked by the legacy oracle. Treat `candidate_blocker_scope=active_or_safety`, `unsafe_overrides_total > 0`, `last_override_safe=false`, or a non-empty `active_blocker` as actionable. Inspect `legacy_advisory_blocker`, `candidate_blocker`, `last_blocker_code`, `stuck_reason`, `stalls_total`, `blockers_total`, `unsafe_overrides_total`, `last_override_scope`, `zclassicd_rpc_error_code`, `zclassicd_rpc_error_message`, and `last_error`. `consensus_authority` must stay `local_consensus_validation`; `candidate_trust` describes candidate data, not a co-authority.
6. **When not to restart:** if `chain_advance.decision` is `use_source` or `wait` with a clear reason, `lag <= 1`, and peer lifecycle shows active handshakes, leave it running. Restarting resets peer reputation and can make reachability look worse for a few minutes.

**Prevention:** Alert when `handshaked_connections == 0` for 5 minutes, `peer_lifecycle.timeout` rises quickly, `chain_advance.decision == "blocked"`, `candidate_blocker_scope == "active_or_safety"`, or `unsafe_overrides_total > 0`.

---

## Wallet Backup Failed

**Symptoms:** `EV_WALLET_BACKUP_FAILED`. `zcl_status` health shows wallet backup warning.

**Diagnose:**
```bash
build/bin/zclassic23 healthcheck full
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

**Prevention:** The built-in backup service runs automatically. Verify after first boot by checking for `EV_WALLET_BACKUP` events.

---

## Tip Regressed / Stuck on Wrong Fork

**Symptoms:** Height decreases or lags far behind network. `EV_REORG_START` with large depth. Peers disconnecting over chain mismatch.

**Diagnose:**
```bash
build/bin/zclassic23 getblockchaininfo
build/bin/zclassic23 getpeerinfo
# Compare your tip hash against a trusted peer or explorer
# Via MCP: zcl_syncstate, zcl_dataintegrity
```

**Fix:**
1. If tip is just behind (syncing): wait. Check `zcl_syncstate` — `BLOCKS_DOWNLOAD` or `CONNECTING_BLOCKS` means sync in progress.
2. If tip regressed after a reorg:
   - Small (<10 blocks): normal, recovers automatically. Watch `EV_REORG_RECOVERY_COMPLETE`.
   - Large (>10 blocks): investigate whether peers agree on the fork:
     ```bash
     build/bin/zclassic23 getpeerinfo
     ```
3. If stuck on a dead fork (no peers agree), see the nuclear option below — invalidateblock / reconsiderblock RPCs are exposed (also as MCP tools zcl_invalidateblock / zcl_reconsiderblock) — use them to drop a stale fork.
4. Nuclear option (last resort): stop node, delete state, resync:
   ```bash
   systemctl --user stop zclassic23
   rm -f ~/.zclassic-c23/node.db ~/.zclassic-c23/node.db-{wal,shm}
   rm -f ~/.zclassic-c23/block_index.bin{,.sha3}
   systemctl --user start zclassic23
   # Node will rebuild from block files or snapshot sync (cold bootstrap is now --importblockindex then a normal boot).
   ```

**Prevention:** Run background validation (`-nobgvalidation` NOT set). Monitor `zcl_chain_height` derivative — alert if zero for >10 minutes while peers show higher heights.

---

## Node Stuck (Not Syncing)

**Symptoms:** Height frozen. `zcl_syncstate` returns `IDLE` or `FAILED`. No blocks connecting.

**Diagnose:**
```bash
build/bin/zclassic23 agent
build/bin/zclassic23 getpeerinfo
build/bin/zclassic23 getnetworkinfo
build/bin/zclassic23 dumpstate peer_lifecycle
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
3. **Sync state is FAILED:** restart — transient failures often clear:
   ```bash
   systemctl --user restart zclassic23
   ```
4. **Stuck in SNAPSHOT_RECEIVE:** snapshot peer may have disconnected. Restart to retry from another peer:
   ```bash
   systemctl --user restart zclassic23
   ```

**Prevention:** Monitor sync state. Alert on `FAILED` or height stalled >10 min.

---

## RPC 429 (Rate Limited)

**Symptoms:** RPC clients get HTTP 429. `EV_RPC_TIMEOUT`. `zcl_rpc_rate_limited_*` counters climbing.

**Diagnose:**
```bash
# Via MCP: zcl_metrics → look at rpc_rate_limited_global, rpc_rate_limited_per_ip
# Check current limits
echo "Global: ${ZCL_RPC_RPS:-50} rps, burst ${ZCL_RPC_BURST:-100}"
echo "Per-IP: ${ZCL_RPC_PER_IP_RPS:-5} rps, burst ${ZCL_RPC_PER_IP_BURST:-10}"
```

**Fix:**
1. If your own tooling hits per-IP limits, raise per-IP budget:
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
3. If a specific IP is flooding, it auto-bans after `ZCL_RPC_AUTH_FAIL_THRESHOLD` (default 5) auth failures. For non-auth flooding, the per-IP rate limit handles it.
4. Loopback (127.0.0.1) bypasses per-IP limits but still counts against global. If local tools fight for global budget, raise `ZCL_RPC_RPS`.

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
2. If client caches the cookie: it must re-read `.cookie` on a 401. During rotation, both current and previous cookies are valid.
3. If IP is banned from too many auth failures:
   - Ban auto-expires after `ZCL_RPC_BAN_SECONDS` (default 3600s = 1hr).
   - To clear immediately: restart the node (ban table is in-memory).
4. If using `rpcuser`/`rpcpassword` instead of cookie: rotation doesn't apply, check credentials.

**Prevention:** Ensure clients re-read the `.cookie` file at least once per rotation interval.

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
1. If background validation consumes too much: disable with `-nobgvalidation` or restart (it auto-detects <8GB machines and reduces batch size).
2. If UTXO cache is large: the node batches flushes; a restart forces a flush and reclaims memory.
3. If mempool is bloated:
   ```bash
   build/bin/zclassic23 getmempoolinfo
   # Mempool has configurable limits — check environment
   ```

**Prevention:** Monitor RSS externally (e.g. `node_exporter`). Background validation is the biggest consumer — disable on RAM-constrained machines.

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

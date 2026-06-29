# Never-stuck hardening roadmap (2026-06-28) — competition

Adversarial sweep (23 agents). FIXED since: step 3 (a35ca0c8f) + full de-latch (75967c574).
Remaining gaps (verbatim, JSON-escaped from the sweep result):

{
  "summary": "Enumerate every way the node can fail to reach/hold tip (fresh judge + running judge) and whether each auto-recovers or can get permanently stuck — competition hardening roadmap",
  "agentCount": 23,
  "logs": [],
  "result": {
    "roadmap": "Verified against HEAD `525d6579c`. Key finding: the de-latch package (525d6579c) was **incomplete** — it added `cooldown_secs`/`cooldown_max_rearms` to only 3 conditions and left 4 still legacy-latching. That, not the already-fixed set, is the top gap. Roadmap below.

---

# NEVER-STUCK HARDENING ROADMAP (verified @ 525d6579c)

## ★ SINGLE HIGHEST-LEVERAGE FIX — finish the de-latch package (4 conditions still latch forever)

`lib/framework/src/condition.c:220-221` confirms: `if (cond->cooldown_secs <= 0) return false; /* legacy: latch permanently at max_attempts */`. The 525d6579c de-latch only patched `peer_floor_violated`, `download_queue_starved`, `stale_validate_headers_repair`. **These four were missed and have NO cooldown fields, so they latch at operator_needed and never re-arm:**

| Condition | file:line (struct) | max_attempts |
|---|---|---|
| `header_stall_at_height` | `app/conditions/src/header_stall_at_height.c:120` | 3 |
| `body_fetch_missing_have_data` | `app/conditions/src/body_fetch_missing_have_data.c:142` | 5 |
| `sync_state_stuck` | `app/conditions/src/sync_state_stuck.c:99` | 3 |
| `snapshot_receive_stalled` | `app/conditions/src/snapshot_receive_stalled.c:75` | 2 |

All four are external-resource (peers/bandwidth) faults — exactly the class the cooldown re-arm was built for. **Fix (per struct, ~2 lines each):** add `.cooldown_secs = 600, .cooldown_max_rearms = 0,` — identical to the proven `peer_floor_violated.c:216-217` pattern. **Risk: near-zero** (mirrors shipped, tested pattern; turns a permanent latch into page-and-keep-trying). This single change closes attack verdicts on `header_stall`, `download_queue_starved` (precondition), `snapshot_receive_stalled`, `body_fetch_missing`, plus fresh-judge map rows for the same. **Caveat:** the underlying P2P sync advances independently of the kick, so the latch only bites when the remedy kick is load-bearing — but the fix is so cheap/safe it should land regardless.

---

## FRESH-JUDGE-PATH GAPS (clean install + starter bundle = the judged path)

**F1. `.failed`-marker crash-loop when the marker can't be written** — `config/src/boot.c:3505`. `fopen(...,"we")` failure is silently ignored; the loader then `_exit()`s on any unseedable bundle (`boot_refold_staged.c:866`, anchor mismatch `:956-968`, pprev gap `:944-954`), and next boot's autodetect re-selects the same bundle because the skip check `boot_refold_staged.c:802` keys on `access(failp,F_OK)==0` (file never created). Read-only mount / ENOSPC / EACCES / inode-exhaustion → infinite crash-loop. **Hit by 3 attack verdicts** (SHA3 self-verify, anchor mismatch, explicit-loader fopen). **Fix (auto-terminating):** at `boot.c:3505`, if `mf==NULL`, do NOT proceed — set `ctx->load_snapshot_at_own_height = NULL; snap_from_autodetect=false;` and fall through to P2P IBD. "Can't record failure memory" ⇒ never gamble on a seed that might need it. **Risk: low.**

**F2. Record-count-mismatch / truncated-snapshot has no failure memory at all** — `boot_refold_staged.c:858-866`. The first `uss_open` SHA3-verify runs *before* the retry label (`:985`) and before the marker is even relevant; on a truncated body it `_exit()`s immediately. The autodetect marker at `boot.c:3505` *does* cover this (written before the loader runs) — **so this is auto-recovering for the autodetect path** and the attack verdict is **over-stated** *unless* F1 also fires (marker unwritable). Net: subsumed by F1. The genuinely uncovered variant is the **explicit `-load-snapshot-at-own-height` flag** (no marker by design) — but a judge uses autodetect, so low judge impact. Document, don't prioritize.

**F3. Stale fixed-seed set, no live DNS** — `lib/chain/src/chainparams.c:162` (`nSeeds=0`) + 10 hardcoded IPv4 seeds "known-good as of 2026-04" (`:166-177`) + 1 onion. If those 10 IPs have churned and Tor is off, a fresh judge with working internet can fail to bootstrap. Auto-recovery exists (`peer_floor_violated` unbounded re-arm) **but has nothing to connect to**. **Fix:** refresh the 10 seed IPs against the live network the day before judging; add 2-3 more onion seeds. **Risk: trivial (data), but verify reachability live.** This is the maps/attacks disagreement on "dns-seed-infra-failure": it self-heals *only* when a seed is actually reachable — not a code defect, an operational freshness item, but judge-critical.

**F4. utxo_apply upstream-hole refill refused when `coins_applied_found==false`** — attack on `stage_repair_reducer_frontier_refill.c:524` + `reducer_frontier_reconcile_light.c:818` (`refused_coin_unknown` gate). Fresh install + watchdog restart before `coins_applied` is ever written → hole detected, refill refused, utxo_apply `JOB_IDLE` forever. Narrower trigger (torn write during very-early boot) but on the fresh path. **Fix:** when `coins_applied_found==false` AND below anchor/seed, treat as "re-fold from floor" rather than refuse (the floor is known). **Risk: medium — touches the fold authority; copy-prove on a fixture first.**

---

## RUNNING-JUDGE-PATH GAPS (node up for days)

**R1. Disk: no write-path gate + no reclaim implementation** — `disk_monitor_is_critical()` (`disk_monitor.h:19`) is never checked before `db_txn_begin`/commit, and `remedy_disk_full_pause()` (`app/conditions/src/disk_full_pause.c:65-70`) only logs intent + polls — "HEAD has no public reclaim entry point yet." Over days, WAL grows unbounded (no aggressive checkpoint policy), fills disk, writes fail `SQLITE_FULL`, fold stalls. The condition re-arms forever (`disk_full_pause.c:120-121`) but **can't free anything**. **Fix (two parts):** (a) implement `storage_reclaim_derived()` = `PRAGMA wal_checkpoint(TRUNCATE)` across progress.kv/node.db/projections + sweep `*.tmp`, call it from the remedy; (b) gate `db_txn_begin` and `ibd_throttle` refill on `disk_monitor_is_critical()` to apply backpressure. **Risk: medium.** Highest *running*-judge structural risk after the cooldown fix.

**R2. WAL unbounded growth even without disk-full** — no size-triggered checkpoint; `db_txn.c:128-152` doesn't checkpoint on commit. Same fix family as R1(a): periodic + post-N-blocks `wal_checkpoint(TRUNCATE)`. **Risk: low-medium.**

**R3. `tip_finalize` chainwork_not_greater / equal-work sibling not adopted** — `tip_finalize_stage.c:666-676` writes ok=0 and advances cursor, but a corrupt zero-work incumbent is never marked `BLOCK_FAILED_VALID`, so `active_chain` stays pinned and H* freezes. The needed equal-work-sibling-adopt (commits `e8e4eb092` / `10d04f03a`) is **not on HEAD**. Distinct from the step-3 lookahead deadlock. **Fix:** cherry-pick/port the sibling-adopt + mark-failed-incumbent logic. **Risk: medium (consensus-adjacent) — replay-gate.**

**R4. `tip_finalize` TF_BLOCKED_UV_ROW_MISSING has no refill** — attack: `reducer_frontier_reconcile_light` has no code to refill a missing `utxo_apply_log` row; `tip_finalize_stage.c:500-502` returns JOB_IDLE forever. Maps claim refill exists; **attack says it doesn't for this specific log** — disagreement. **Action: verify** whether `stage_repair_reducer_frontier_refill.c` covers `utxo_apply_log` rows; if not, add a re-fold-the-row path. **Risk: medium.**

**R5. JOB_FATAL stages with no escalator** — `validate_headers_stage.c` and `script_validate_stage.c` return JOB_FATAL on a sqlite read error (disk full / transient lock / corruption) with no condition/escalator wired (maps: both "blocks-tip"). A transient `SQLITE_BUSY` shouldn't be fatal. **Fix:** retry-with-backoff on transient sqlite errors; route persistent failure to `boot_auto_reindex_request` instead of dead JOB_FATAL. **Risk: medium.**

---

## SECONDARY / LOWER JUDGE-IMPACT

- **Boot FATAL-before-self-heal** (`boot.c:1728/1808/2515/3427`). **Maps disagree:** boot-restart map says auto-recovers via `boot_crashonly_storage_gate`; crash-kill9 map (S5) says these FATAL before `self_heal_register` (`boot_services.c:1463`). Reality: under systemd `Restart=always` they converge via the bounded reindex budget; **direct-launch (no systemd) stays down.** Judge almost certainly uses the service unit ⇒ low impact. Harden later by moving the auto-reindex consume ahead of these checks.
- **Sticky escalator network rungs are stubs** — `sticky_escalator.c:128-184`: RESNAPSHOT / SELF_MINT_REFOLD / WIDEN_PEERS / REBOOTSTRAP all return NOT_IMPLEMENTED (only REINDEX `:134-166` is real). So a network-class stall has no escalator backstop — which is *why* the ★ cooldown fix matters (it's the only real auto-recovery for that class). Cheap partial win: make `rung_widen_peers_default` actually call `connman_kick_onion_seeds()` + `connman_kick_seed_discovery()` instead of just emitting an event.
- **Explicit `-load-snapshot-at-own-height` crash-loop on corrupt file** — operator-flag-only, judge uses autodetect; low impact.

---

## STALE MAP/ATTACK ENTRIES (do not spend effort)
- `download_queue_starved` attack verdict "GAP-can-stick" is **stale** — `download_queue_starved.c:115-116` now has `cooldown_secs=300, cooldown_max_rearms=0` (unbounded re-arm). Residual is only the remedy precondition (`sync_monitor_kick_local_sync` early-return when state≠READY/AT_TIP) — minor.
- `peer_floor` / `dns-seed` "permanent stuck" attacks describe genuine **network unavailability**, not a code defect; they auto-resume the moment any seed/peer is reachable (unbounded re-arm). The actionable part is F3 (seed freshness), not the retry logic.

**Bottom line:** ship the ★ 4-condition cooldown one-liner + F1 marker-fopen guard first (both ~hours, near-zero risk, close the most attack verdicts across fresh AND running paths), then F3 seed refresh (operational), then R1 disk reclaim.",
    "gap_count": 27,
    "attack_gaps": [
      "utxo_apply_stage: upstream hole (proof_validate_log row missing at cursor)",
      "tip_finalize_stage: TF_BLOCKED_UV_ROW_MISSING",
      "tip_finalize_stage: TF_BLOCKED_LOOKAHEAD_MISSING",
      "tip_finalize_stage: chainwork_not_greater",
      "Explicit snapshot loader",
      "Progress.kv unavailable: coins_kv database cannot open",
      "explicit -load-snapshot-at-own-height: BEGIN transaction fails",
      "Explicit -load-snapshot-at-own-height: snapshot changed between verification and load",
      "Explicit -load-snapshot-at-own-height: record count mismatch after load",
      "No healthy outbound peers (below PEER_FLOOR_MIN_HEALTHY=3)",
      "download_queue_starved",
      "stale_header_validation",
      "dns-seed-infra-failure",
      "snapshot SHA3 self-verify fails",
      "snapshot anchor hash mismatch with PoW header"
    ]
  },
  "workflowProgress": [
    {
      "type": "workflow_phase",
      "index": 1,
      "title": "Map"
    },
    {
      "type": "workflow_phase",
      "index": 2,
      "title": "Attack"
    },
    {
      "type": "workflow_phase",
      "index": 3,
      "title": "Roadmap"
    },
    {
      "type": "workflow_agent",
      "index": 1,
      "label": "map:fold-stages",
      "phaseIndex": 1,
      "phaseTitle": "Map",
      "agentId": "ac70783713e2387a5",
      "agentType": "Explore",
      "model": "claude-haiku-4-5-20251001",
      "state": "done",
      "startedAt": 1782613757573,
      "queuedAt": 1782613757561,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "fold-stages",
      "promptPreview": "COMPETITION: MrBeast $1M, judged in DAYS, BOTH fresh-install-sync-speed AND long-run-robustness judged; some teams sync fast. A node that gets stuck (or needs a human) LOSES. The prime directive: "stuck-needs-a-human" must be STRUCTURALLY IMPOSSIBLE — every failure mode must auto-recover in bounded time, OR the fold always advances/names a blocker, never silently sits.
ALREADY FIXED (do not re-rep…",
      "lastProgressAt": 1782613891292,
      "tokens": 95857,
      "toolCalls": 32,
      "durationMs": 133719,
      "resultPreview": "{"category":"fold-stages","failure_modes":[{"mode":"header_admit_stage: missing block_index entry","trigger":"concurrent reorg invalidates block during fold","auto_recovers":true,"remedy_or_gap":"header_admit_stage.c:99-105 (pending_header_take returns NULL, supervisor retries)","severity":"cosmetic"},{"mode":"validate_headers_stage: database read error on log_ok_at","trigger":"sqlite3 prepare/ste…"
    },
    {
      "type": "workflow_agent",
      "index": 2,
      "label": "map:boot-restart",
      "phaseIndex": 1,
      "phaseTitle": "Map",
      "agentId": "ab44059e1e2fc1a99",
      "agentType": "Explore",
      "model": "claude-haiku-4-5-20251001",
      "state": "done",
      "startedAt": 1782613757576,
      "queuedAt": 1782613757562,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "boot-restart",
      "promptPreview": "COMPETITION: MrBeast $1M, judged in DAYS, BOTH fresh-install-sync-speed AND long-run-robustness judged; some teams sync fast. A node that gets stuck (or needs a human) LOSES. The prime directive: "stuck-needs-a-human" must be STRUCTURALLY IMPOSSIBLE — every failure mode must auto-recover in bounded time, OR the fold always advances/names a blocker, never silently sits.
ALREADY FIXED (do not re-rep…",
      "lastProgressAt": 1782613951165,
      "tokens": 90173,
      "toolCalls": 57,
      "durationMs": 193589,
      "resultPreview": "{"category":"boot-restart","failure_modes":[{"mode":"Explicit snapshot loader: corrupted file / anchor mismatch / verification failure","trigger":"Operator passes -load-snapshot-at-own-height with bad file; autodetect detects bundle","auto_recovers":true,"remedy_or_gap":"boot.c:3492-3509,3525-3526 failure memory (.failed marker); autodetect skips marked bundles and falls back to P2P IBD. Explicit …"
    },
    {
      "type": "workflow_agent",
      "index": 3,
      "label": "map:network-peers",
      "phaseIndex": 1,
      "phaseTitle": "Map",
      "agentId": "acee64ba4d384eca8",
      "agentType": "Explore",
      "model": "claude-haiku-4-5-20251001",
      "state": "done",
      "startedAt": 1782613757576,
      "queuedAt": 1782613757562,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "network-peers",
      "promptPreview": "COMPETITION: MrBeast $1M, judged in DAYS, BOTH fresh-install-sync-speed AND long-run-robustness judged; some teams sync fast. A node that gets stuck (or needs a human) LOSES. The prime directive: "stuck-needs-a-human" must be STRUCTURALLY IMPOSSIBLE — every failure mode must auto-recover in bounded time, OR the fold always advances/names a blocker, never silently sits.
ALREADY FIXED (do not re-rep…",
      "lastProgressAt": 1782613929366,
      "tokens": 91380,
      "toolCalls": 42,
      "durationMs": 171790,
      "resultPreview": "{"category":"network-peers","failure_modes":[{"mode":"No healthy outbound peers (below PEER_FLOOR_MIN_HEALTHY=3)","trigger":"connman_outbound_health.healthy < 3 for >60 seconds","auto_recovers":true,"remedy_or_gap":"<repo>/app/conditions/src/peer_floor_violated.c:141-142 (connman_kick_onion_seeds) with unbounded cooldown retry (cooldown_secs=600, cooldown_max_rearms=0)","sev…"
    },
    {
      "type": "workflow_agent",
      "index": 4,
      "label": "map:fresh-judge",
      "phaseIndex": 1,
      "phaseTitle": "Map",
      "agentId": "aec2df745fc5fc4f7",
      "agentType": "Explore",
      "model": "claude-haiku-4-5-20251001",
      "state": "done",
      "startedAt": 1782613757577,
      "queuedAt": 1782613757562,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "fresh-judge",
      "promptPreview": "COMPETITION: MrBeast $1M, judged in DAYS, BOTH fresh-install-sync-speed AND long-run-robustness judged; some teams sync fast. A node that gets stuck (or needs a human) LOSES. The prime directive: "stuck-needs-a-human" must be STRUCTURALLY IMPOSSIBLE — every failure mode must auto-recover in bounded time, OR the fold always advances/names a blocker, never silently sits.
ALREADY FIXED (do not re-rep…",
      "lastProgressAt": 1782613931012,
      "tokens": 84300,
      "toolCalls": 44,
      "durationMs": 173435,
      "resultPreview": "{"category":"fresh-judge","failure_modes":[{"mode":"bundle missing","trigger":"Fresh install with no block_index.bin or utxo-seed-*.snapshot in datadir","auto_recovers":true,"remedy_or_gap":"boot_autodetect_bundle_snapshot returns NULL (config/src/boot_refold_staged.c:751-838), falls through to normal P2P IBD","severity":"degrades"},{"mode":"snapshot marked failed from prior seed attempt","trigger…"
    },
    {
      "type": "workflow_agent",
      "index": 5,
      "label": "map:crash-kill9",
      "phaseIndex": 1,
      "phaseTitle": "Map",
      "agentId": "a39b4ba81bd56b70b",
      "agentType": "Explore",
      "model": "claude-haiku-4-5-20251001",
      "state": "done",
      "startedAt": 1782613757577,
      "queuedAt": 1782613757562,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "crash-kill9",
      "promptPreview": "COMPETITION: MrBeast $1M, judged in DAYS, BOTH fresh-install-sync-speed AND long-run-robustness judged; some teams sync fast. A node that gets stuck (or needs a human) LOSES. The prime directive: "stuck-needs-a-human" must be STRUCTURALLY IMPOSSIBLE — every failure mode must auto-recover in bounded time, OR the fold always advances/names a blocker, never silently sits.
ALREADY FIXED (do not re-rep…",
      "lastProgressAt": 1782613913016,
      "tokens": 96393,
      "toolCalls": 52,
      "durationMs": 155439,
      "resultPreview": "{"category":"crash-kill9","failure_modes":[{"mode":"sqlite3_db_cacheflush failure (chain tip fsync barrier)","trigger":"Power loss or crash immediately after cacheflush returns SQLITE_IOERR during chain-tip finalization","auto_recovers":false,"remedy_or_gap":"chain_tip.c:73-79 silently logs cacheflush failure and continues without escalation. Gap: no fallback mechanism when fsync barrier fails. Co…"
    },
    {
      "type": "workflow_agent",
      "index": 6,
      "label": "map:disk-resource",
      "phaseIndex": 1,
      "phaseTitle": "Map",
      "agentId": "a579d71503750d191",
      "agentType": "Explore",
      "model": "claude-haiku-4-5-20251001",
      "state": "done",
      "startedAt": 1782613757578,
      "queuedAt": 1782613757562,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "disk-resource",
      "promptPreview": "COMPETITION: MrBeast $1M, judged in DAYS, BOTH fresh-install-sync-speed AND long-run-robustness judged; some teams sync fast. A node that gets stuck (or needs a human) LOSES. The prime directive: "stuck-needs-a-human" must be STRUCTURALLY IMPOSSIBLE — every failure mode must auto-recover in bounded time, OR the fold always advances/names a blocker, never silently sits.
ALREADY FIXED (do not re-rep…",
      "lastProgressAt": 1782613887126,
      "tokens": 91337,
      "toolCalls": 40,
      "durationMs": 129548,
      "resultPreview": "{"category":"disk-resource","failure_modes":[{"mode":"disk_full_mid_write_unguarded","trigger":"Disk free bytes fall below 1 GB while writes are in progress","auto_recovers":false,"remedy_or_gap":"CRITICAL GAP: disk_monitor_is_critical() is documented to guard write paths (disk_monitor.h:19) but is NOT checked before db_txn_begin/commit. Only checked in disk_full_pause condition itself. When write…"
    },
    {
      "type": "workflow_agent",
      "index": 7,
      "label": "attack:utxo_apply_stage: upstream hole",
      "phaseIndex": 2,
      "phaseTitle": "Attack",
      "agentId": "a01f08bbeab24df81",
      "agentType": "Explore",
      "model": "claude-haiku-4-5-20251001",
      "state": "done",
      "startedAt": 1782613951179,
      "queuedAt": 1782613951167,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "utxo_apply_stage: upstream hole (proof_validate_log row mis…",
      "promptPreview": "COMPETITION: MrBeast $1M, judged in DAYS, BOTH fresh-install-sync-speed AND long-run-robustness judged; some teams sync fast. A node that gets stuck (or needs a human) LOSES. The prime directive: "stuck-needs-a-human" must be STRUCTURALLY IMPOSSIBLE — every failure mode must auto-recover in bounded time, OR the fold always advances/names a blocker, never silently sits.
ALREADY FIXED (do not re-rep…",
      "lastProgressAt": 1782614289427,
      "tokens": 92809,
      "toolCalls": 78,
      "durationMs": 338248,
      "resultPreview": "{"mode":"utxo_apply_stage: upstream hole (proof_validate_log row missing at cursor)","really_auto_recovers":false,"scenario":"Fresh install, watchdog restart before coins_applied ever written. Upstream hole created in proof_validate_log (cursor advanced, row missing). On restart, condition detects hole but refuses refill at stage_repair_reducer_frontier_refill.c:524 because coins_applied_found==fa…"
    },
    {
      "type": "workflow_agent",
      "index": 8,
      "label": "attack:tip_finalize_stage: TF_BLOCKED_U",
      "phaseIndex": 2,
      "phaseTitle": "Attack",
      "agentId": "a974be332bd3eebc1",
      "agentType": "Explore",
      "model": "claude-haiku-4-5-20251001",
      "state": "done",
      "startedAt": 1782613951180,
      "queuedAt": 1782613951167,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "tip_finalize_stage: TF_BLOCKED_UV_ROW_MISSING",
      "promptPreview": "COMPETITION: MrBeast $1M, judged in DAYS, BOTH fresh-install-sync-speed AND long-run-robustness judged; some teams sync fast. A node that gets stuck (or needs a human) LOSES. The prime directive: "stuck-needs-a-human" must be STRUCTURALLY IMPOSSIBLE — every failure mode must auto-recover in bounded time, OR the fold always advances/names a blocker, never silently sits.
ALREADY FIXED (do not re-rep…",
      "lastProgressAt": 1782614076407,
      "tokens": 95936,
      "toolCalls": 33,
      "durationMs": 125227,
      "resultPreview": "{"mode":"tip_finalize_stage: TF_BLOCKED_UV_ROW_MISSING","really_auto_recovers":false,"scenario":"utxo_apply_log row at height H is deleted/missing. utxo_apply_stage cursor has advanced past H (forward-only monotonic). tip_finalize tries to read the row at H, finds it missing (found==0), returns JOB_IDLE. reducer_frontier_reconcile_light is called but has NO code to refill missing utxo_apply_log ro…"
    },
    {
      "type": "workflow_agent",
      "index": 9,
      "label": "attack:tip_finalize_stage: TF_BLOCKED_L",
      "phaseIndex": 2,
      "phaseTitle": "Attack",
      "agentId": "a455c1fe9273138ad",
      "agentType": "Explore",
      "model": "claude-haiku-4-5-20251001",
      "state": "done",
      "startedAt": 1782613951181,
      "queuedAt": 1782613951167,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "tip_finalize_stage: TF_BLOCKED_LOOKAHEAD_MISSING",
      "promptPreview": "COMPETITION: MrBeast $1M, judged in DAYS, BOTH fresh-install-sync-speed AND long-run-robustness judged; some teams sync fast. A node that gets stuck (or needs a human) LOSES. The prime directive: "stuck-needs-a-human" must be STRUCTURALLY IMPOSSIBLE — every failure mode must auto-recover in bounded time, OR the fold always advances/names a blocker, never silently sits.
ALREADY FIXED (do not re-rep…",
      "lastProgressAt": 1782614200337,
      "tokens": 92546,
      "toolCalls": 20,
      "durationMs": 249156,
      "resultPreview": "{"mode":"tip_finalize_stage: TF_BLOCKED_LOOKAHEAD_MISSING","really_auto_recovers":false,"scenario":"header_admit_stage blocked on missing pprev linkage, or best_header transient lag: finalize at height N with lookahead N+1; best_header->nHeight < N+1 due to header_admit not advancing; window-slot self-heal fails (N+1 not finalized), header-chain self-heal fails (best_header too low), is_canonical_…"
    },
    {
      "type": "workflow_agent",
      "index": 10,
      "label": "attack:tip_finalize_stage: chainwork_no",
      "phaseIndex": 2,
      "phaseTitle": "Attack",
      "agentId": "acfa691572150ed8b",
      "agentType": "Explore",
      "model": "claude-haiku-4-5-20251001",
      "state": "done",
      "startedAt": 1782613951181,
      "queuedAt": 1782613951168,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "tip_finalize_stage: chainwork_not_greater",
      "promptPreview": "COMPETITION: MrBeast $1M, judged in DAYS, BOTH fresh-install-sync-speed AND long-run-robustness judged; some teams sync fast. A node that gets stuck (or needs a human) LOSES. The prime directive: "stuck-needs-a-human" must be STRUCTURALLY IMPOSSIBLE — every failure mode must auto-recover in bounded time, OR the fold always advances/names a blocker, never silently sits.
ALREADY FIXED (do not re-rep…",
      "lastProgressAt": 1782614249207,
      "tokens": 86409,
      "toolCalls": 58,
      "durationMs": 298025,
      "resultPreview": "{"mode":"tip_finalize_stage: chainwork_not_greater","really_auto_recovers":false,"scenario":"Fresh install with corrupt (zero-work/invalid-PoW) block C and canonical equal-work sibling B at height H. tip_finalize writes ok=0 and advances cursor past H (lines 666-676), but active_chain_selection_candidate_beats_best cannot switch from C to B because C is never marked BLOCK_FAILED_VALID (no mechanis…"
    },
    {
      "type": "workflow_agent",
      "index": 11,
      "label": "attack:Explicit snapshot loader: corrup",
      "phaseIndex": 2,
      "phaseTitle": "Attack",
      "agentId": "a2e226522777e57c8",
      "agentType": "Explore",
      "model": "claude-haiku-4-5-20251001",
      "state": "done",
      "startedAt": 1782613951182,
      "queuedAt": 1782613951168,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "Explicit snapshot loader",
      "promptPreview": "COMPETITION: MrBeast $1M, judged in DAYS, BOTH fresh-install-sync-speed AND long-run-robustness judged; some teams sync fast. A node that gets stuck (or needs a human) LOSES. The prime directive: "stuck-needs-a-human" must be STRUCTURALLY IMPOSSIBLE — every failure mode must auto-recover in bounded time, OR the fold always advances/names a blocker, never silently sits.
ALREADY FIXED (do not re-rep…",
      "lastProgressAt": 1782614135256,
      "tokens": 59950,
      "toolCalls": 27,
      "durationMs": 184074,
      "resultPreview": "{"mode":"Explicit snapshot loader","really_auto_recovers":false,"scenario":"Fresh node boots with corrupted autodetect bundle in datadir. Datadir has filesystem constraint (read-only mount, permission denied, disk full). Node attempts to create .failed marker via fopen() at boot.c:3505. fopen() fails silently (returns NULL). autodetect_fail_marker variable still contains path string (only cleared …"
    },
    {
      "type": "workflow_agent",
      "index": 12,
      "label": "attack:Progress.kv unavailable: coins_k",
      "phaseIndex": 2,
      "phaseTitle": "Attack",
      "agentId": "aa0be4bd097f7b68b",
      "agentType": "Explore",
      "model": "claude-haiku-4-5-20251001",
      "state": "done",
      "startedAt": 1782613951183,
      "queuedAt": 1782613951168,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "Progress.kv unavailable: coins_kv database cannot open",
      "promptPreview": "COMPETITION: MrBeast $1M, judged in DAYS, BOTH fresh-install-sync-speed AND long-run-robustness judged; some teams sync fast. A node that gets stuck (or needs a human) LOSES. The prime directive: "stuck-needs-a-human" must be STRUCTURALLY IMPOSSIBLE — every failure mode must auto-recover in bounded time, OR the fold always advances/names a blocker, never silently sits.
ALREADY FIXED (do not re-rep…",
      "lastProgressAt": 1782614277099,
      "tokens": 94162,
      "toolCalls": 58,
      "durationMs": 325916,
      "resultPreview": "{"mode":"Progress.kv unavailable: coins_kv database cannot open","really_auto_recovers":false,"scenario":"Persistent filesystem issue (disk error, permission problem, full disk) prevents progress_store_open() from succeeding even after fresh file creation. Boot 1-3 attempt reindex and exit. Boot 4+ parks in boot_park_until_shutdown() with services never started, RPC unavailable, no mechanism to se…"
    },
    {
      "type": "workflow_agent",
      "index": 13,
      "label": "attack:Explicit -load-snapshot-at-own-h",
      "phaseIndex": 2,
      "phaseTitle": "Attack",
      "agentId": "a88ccdf0004bc8c9e",
      "agentType": "Explore",
      "model": "claude-haiku-4-5-20251001",
      "state": "done",
      "startedAt": 1782613951183,
      "queuedAt": 1782613951168,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "explicit -load-snapshot-at-own-height: BEGIN transaction fa…",
      "promptPreview": "COMPETITION: MrBeast $1M, judged in DAYS, BOTH fresh-install-sync-speed AND long-run-robustness judged; some teams sync fast. A node that gets stuck (or needs a human) LOSES. The prime directive: "stuck-needs-a-human" must be STRUCTURALLY IMPOSSIBLE — every failure mode must auto-recover in bounded time, OR the fold always advances/names a blocker, never silently sits.
ALREADY FIXED (do not re-rep…",
      "lastProgressAt": 1782614205984,
      "tokens": 62336,
      "toolCalls": 46,
      "durationMs": 254801,
      "resultPreview": "{"mode":"explicit -load-snapshot-at-own-height: BEGIN transaction fails","really_auto_recovers":false,"scenario":"BEGIN IMMEDIATE fails at line 1040 -> reopen_progress_store_after_verified_snapshot called -> progress_store_close() fails to actually close DB (sqlite3_close returns SQLITE_BUSY but function continues) -> quarantine_progress_file fails because file still open (rename fails) -> reopen …"
    },
    {
      "type": "workflow_agent",
      "index": 14,
      "label": "attack:Explicit -load-snapshot-at-own-h",
      "phaseIndex": 2,
      "phaseTitle": "Attack",
      "agentId": "a075197a963e7feb8",
      "agentType": "Explore",
      "model": "claude-haiku-4-5-20251001",
      "state": "done",
      "startedAt": 1782613951183,
      "queuedAt": 1782613951168,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "Explicit -load-snapshot-at-own-height: snapshot changed bet…",
      "promptPreview": "COMPETITION: MrBeast $1M, judged in DAYS, BOTH fresh-install-sync-speed AND long-run-robustness judged; some teams sync fast. A node that gets stuck (or needs a human) LOSES. The prime directive: "stuck-needs-a-human" must be STRUCTURALLY IMPOSSIBLE — every failure mode must auto-recover in bounded time, OR the fold always advances/names a blocker, never silently sits.
ALREADY FIXED (do not re-rep…",
      "lastProgressAt": 1782614084157,
      "tokens": 64636,
      "toolCalls": 18,
      "durationMs": 132974,
      "resultPreview": "{"mode":"Explicit -load-snapshot-at-own-height: snapshot changed between verification and load","really_auto_recovers":false,"scenario":"Snapshot file atomically replaced between first uss_open (line 858 SHA3-verify) and second uss_open (line 1025) with corrupt or different-height file via concurrent process (e.g., rsync, snapshot download tool). Second open fails SHA3 check or returns different h…"
    },
    {
      "type": "workflow_agent",
      "index": 15,
      "label": "attack:Explicit -load-snapshot-at-own-h",
      "phaseIndex": 2,
      "phaseTitle": "Attack",
      "agentId": "a5e79250d34d96618",
      "agentType": "Explore",
      "model": "claude-haiku-4-5-20251001",
      "state": "done",
      "startedAt": 1782613951184,
      "queuedAt": 1782613951168,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "Explicit -load-snapshot-at-own-height: record count mismatc…",
      "promptPreview": "COMPETITION: MrBeast $1M, judged in DAYS, BOTH fresh-install-sync-speed AND long-run-robustness judged; some teams sync fast. A node that gets stuck (or needs a human) LOSES. The prime directive: "stuck-needs-a-human" must be STRUCTURALLY IMPOSSIBLE — every failure mode must auto-recover in bounded time, OR the fold always advances/names a blocker, never silently sits.
ALREADY FIXED (do not re-rep…",
      "lastProgressAt": 1782614089088,
      "tokens": 63269,
      "toolCalls": 17,
      "durationMs": 137904,
      "resultPreview": "{"mode":"Explicit -load-snapshot-at-own-height: record count mismatch after load","really_auto_recovers":false,"scenario":"Snapshot file corrupted/truncated mid-load (incomplete records, header present but body truncated). First uss_open() at boot_refold_staged.c:858 detects SHA3 mismatch when recomputing over truncated body, returns NULL, node exits immediately at line 866. The retry_authority_st…"
    },
    {
      "type": "workflow_agent",
      "index": 16,
      "label": "attack:No healthy outbound peers (below",
      "phaseIndex": 2,
      "phaseTitle": "Attack",
      "agentId": "a3a280c39f42ccd21",
      "agentType": "Explore",
      "model": "claude-haiku-4-5-20251001",
      "state": "done",
      "startedAt": 1782613951184,
      "queuedAt": 1782613951168,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "No healthy outbound peers (below PEER_FLOOR_MIN_HEALTHY=3)",
      "promptPreview": "COMPETITION: MrBeast $1M, judged in DAYS, BOTH fresh-install-sync-speed AND long-run-robustness judged; some teams sync fast. A node that gets stuck (or needs a human) LOSES. The prime directive: "stuck-needs-a-human" must be STRUCTURALLY IMPOSSIBLE — every failure mode must auto-recover in bounded time, OR the fold always advances/names a blocker, never silently sits.
ALREADY FIXED (do not re-rep…",
      "lastProgressAt": 1782614134741,
      "tokens": 68846,
      "toolCalls": 45,
      "durationMs": 183557,
      "resultPreview": "{"mode":"No healthy outbound peers (below PEER_FLOOR_MIN_HEALTHY=3)","really_auto_recovers":false,"scenario":"PERMANENT STUCK: Tor bootstrap failure combined with fixed/DNS seed unreachability. (1) Fresh node with zero healthy peers. (2) Tor bootstrap fails or hangs (read_onion_address timeout at 120s without setting g_tor_ready). (3) Network partition blocks all 10 hardcoded fixed seeds (ports 80…"
    },
    {
      "type": "workflow_agent",
      "index": 17,
      "label": "attack:All peers banned due to cumulati",
      "phaseIndex": 2,
      "phaseTitle": "Attack",
      "agentId": "a2ef5094d0cb4a5b7",
      "agentType": "Explore",
      "model": "claude-haiku-4-5-20251001",
      "state": "done",
      "startedAt": 1782613951185,
      "queuedAt": 1782613951168,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "All peers banned due to cumulative misbehavior (dos=100)",
      "promptPreview": "COMPETITION: MrBeast $1M, judged in DAYS, BOTH fresh-install-sync-speed AND long-run-robustness judged; some teams sync fast. A node that gets stuck (or needs a human) LOSES. The prime directive: "stuck-needs-a-human" must be STRUCTURALLY IMPOSSIBLE — every failure mode must auto-recover in bounded time, OR the fold always advances/names a blocker, never silently sits.
ALREADY FIXED (do not re-rep…",
      "lastProgressAt": 1782614345279,
      "tokens": 93901,
      "toolCalls": 93,
      "durationMs": 394094,
      "resultPreview": "{"mode":"All peers banned due to cumulative misbehavior (dos=100)","really_auto_recovers":true,"scenario":"All peers accumulated dos >= 100 and are banned. Remedy clears addnode backoff, adds fixed seeds to addrman, and attempts onion seed discovery. Node retries unbounded every 10 minutes. After 24 hours, bans automatically expire (is_banned checks now < ban_until), allowing reconnection. Chain a…"
    },
    {
      "type": "workflow_agent",
      "index": 18,
      "label": "attack:Download queue starved: in-fligh",
      "phaseIndex": 2,
      "phaseTitle": "Attack",
      "agentId": "afd2872ecfa068d86",
      "agentType": "Explore",
      "model": "claude-haiku-4-5-20251001",
      "state": "done",
      "startedAt": 1782613951185,
      "queuedAt": 1782613951168,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "download_queue_starved",
      "promptPreview": "COMPETITION: MrBeast $1M, judged in DAYS, BOTH fresh-install-sync-speed AND long-run-robustness judged; some teams sync fast. A node that gets stuck (or needs a human) LOSES. The prime directive: "stuck-needs-a-human" must be STRUCTURALLY IMPOSSIBLE — every failure mode must auto-recover in bounded time, OR the fold always advances/names a blocker, never silently sits.
ALREADY FIXED (do not re-rep…",
      "lastProgressAt": 1782614165661,
      "tokens": 82329,
      "toolCalls": 37,
      "durationMs": 214476,
      "resultPreview": "{"mode":"download_queue_starved","really_auto_recovers":false,"scenario":"Node in SYNC_BLOCKS_DOWNLOAD state, all peers disconnect or become unresponsive. Remedy calls sync_monitor_kick_local_sync() which silently returns early (line 183-185) if activation state is not READY or AT_TIP. No blocks are queued. Witness checks if request counter advanced - it didn't - so witness fails. Condition re-tri…"
    },
    {
      "type": "workflow_agent",
      "index": 19,
      "label": "attack:Stale header validation: forged/",
      "phaseIndex": 2,
      "phaseTitle": "Attack",
      "agentId": "a3d4d8de8579c6b1e",
      "agentType": "Explore",
      "model": "claude-haiku-4-5-20251001",
      "state": "done",
      "startedAt": 1782613951186,
      "queuedAt": 1782613951168,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "stale_header_validation",
      "promptPreview": "COMPETITION: MrBeast $1M, judged in DAYS, BOTH fresh-install-sync-speed AND long-run-robustness judged; some teams sync fast. A node that gets stuck (or needs a human) LOSES. The prime directive: "stuck-needs-a-human" must be STRUCTURALLY IMPOSSIBLE — every failure mode must auto-recover in bounded time, OR the fold always advances/names a blocker, never silently sits.
ALREADY FIXED (do not re-rep…",
      "lastProgressAt": 1782614154269,
      "tokens": 81859,
      "toolCalls": 41,
      "durationMs": 203083,
      "resultPreview": "{"mode":"stale_header_validation","really_auto_recovers":false,"scenario":"Permanent solutionless header stuck: (1) A forged/solutionless header H reaches the active chain via P2P or snapshot. (2) Oracle is offline (RPC unreachable). (3) All currently-available P2P peers have the same solutionless header H. Remedy flow: header_probe_pull_range fails (oracle down) → cure_request_peer_refetch clears…"
    },
    {
      "type": "workflow_agent",
      "index": 20,
      "label": "attack:DNS seed infrastructure failure",
      "phaseIndex": 2,
      "phaseTitle": "Attack",
      "agentId": "a3537494dea380084",
      "agentType": "Explore",
      "model": "claude-haiku-4-5-20251001",
      "state": "done",
      "startedAt": 1782613951186,
      "queuedAt": 1782613951168,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "dns-seed-infra-failure",
      "promptPreview": "COMPETITION: MrBeast $1M, judged in DAYS, BOTH fresh-install-sync-speed AND long-run-robustness judged; some teams sync fast. A node that gets stuck (or needs a human) LOSES. The prime directive: "stuck-needs-a-human" must be STRUCTURALLY IMPOSSIBLE — every failure mode must auto-recover in bounded time, OR the fold always advances/names a blocker, never silently sits.
ALREADY FIXED (do not re-rep…",
      "lastProgressAt": 1782614157203,
      "tokens": 79964,
      "toolCalls": 36,
      "durationMs": 206017,
      "resultPreview": "{"mode":"dns-seed-infra-failure","really_auto_recovers":false,"scenario":"Fresh install on network where all 10 hardcoded fixed seeds (2026-04 IPs) are unreachable due to churn/offline, the 1 hardcoded onion seed is down for maintenance, operator onion-seeds config is empty, and ZSLP returns no peers (no prior chain). Node reaches peer_floor_violated condition within 60 seconds and retries recover…"
    },
    {
      "type": "workflow_agent",
      "index": 21,
      "label": "attack:snapshot SHA3 self-verify fails",
      "phaseIndex": 2,
      "phaseTitle": "Attack",
      "agentId": "ad1306ac68f3a39a2",
      "agentType": "Explore",
      "model": "claude-haiku-4-5-20251001",
      "state": "done",
      "startedAt": 1782613951187,
      "queuedAt": 1782613951168,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "snapshot SHA3 self-verify fails",
      "promptPreview": "COMPETITION: MrBeast $1M, judged in DAYS, BOTH fresh-install-sync-speed AND long-run-robustness judged; some teams sync fast. A node that gets stuck (or needs a human) LOSES. The prime directive: "stuck-needs-a-human" must be STRUCTURALLY IMPOSSIBLE — every failure mode must auto-recover in bounded time, OR the fold always advances/names a blocker, never silently sits.
ALREADY FIXED (do not re-rep…",
      "lastProgressAt": 1782614080478,
      "tokens": 64142,
      "toolCalls": 19,
      "durationMs": 129291,
      "resultPreview": "{"mode":"snapshot SHA3 self-verify fails","really_auto_recovers":false,"scenario":"Disk full (or permission denied) causes fopen() to fail creating the .failed marker at boot.c:3505. The snapshot load attempt then fails with uss_open() error. Node calls _exit() without a .failed marker. Next boot: autodetect finds the same snapshot, marker check passes (marker never existed), tries to create marke…"
    },
    {
      "type": "workflow_agent",
      "index": 22,
      "label": "attack:snapshot anchor hash mismatch wi",
      "phaseIndex": 2,
      "phaseTitle": "Attack",
      "agentId": "a7c808bfad51f4c3b",
      "agentType": "Explore",
      "model": "claude-haiku-4-5-20251001",
      "state": "done",
      "startedAt": 1782613951187,
      "queuedAt": 1782613951168,
      "attempt": 1,
      "lastToolName": "StructuredOutput",
      "lastToolSummary": "snapshot anchor hash mismatch with PoW header",
      "promptPreview": "COMPETITION: MrBeast $1M, judged in DAYS, BOTH fresh-install-sync-speed AND long-run-robustness judged; some teams sync fast. A node that gets stuck (or needs a human) LOSES. The prime directive: "stuck-needs-a-human" must be STRUCTURALLY IMPOSSIBLE — every failure mode must auto-recover in bounded time, OR the fold always advances/names a blocker, never silently sits.
ALREADY FIXED (do not re-rep…",
      "lastProgressAt": 1782614083878,
      "tokens": 38120,
      "toolCalls": 27,
      "durationMs": 132691,
      "resultPreview": "{"mode":"snapshot anchor hash mismatch with PoW header","really_auto_recovers":false,"scenario":"fopen() fails to create .failed marker file (inode limit, permission denied, filesystem error); snapshot loads anyway, hits anchor hash mismatch, _exit(); next boot's autodetect doesn't find marker file (never created), tries same snapshot again, same mismatch, _exit(); repeat indefinitely without reco…"
    },
    {
      "type": "workflow_agent",
      "index": 23,
      "label": "roadmap:hardening",
      "phaseIndex": 3,
      "phaseTitle": "Roadmap",
      "agentId": "a2b1aefbbe0c69279",
      "model": "claude-opus-4-8[1m]",
      "state": "done",
      "startedAt": 1782614345293,
      "queuedAt": 1782614345292,
      "attempt": 1,
      "lastToolName": "Read",
      "lastToolSummary": "<repo>/app/services/src/sticky_escal…",
      "promptPreview": "You are the hardening lead for the $1M competition node. Below are failure-mode maps per category + adversarial attack verdicts on the auto-recovery claims.


COMPETITION: MrBeast $1M, judged in DAYS, BOTH fresh-install-sync-speed AND long-run-robustness judged; some teams sync fast. A node that gets stuck (or needs a human) LOSES. The prime directive: "stuck-needs-a-human" must be STRUCTURALLY IM…",
      "lastProgressAt": 1782614515259,
      "tokens": 76525,
      "toolCalls": 10,
      "durationMs": 169966,
      "resultPreview": "Verified against HEAD `525d6579c`. Key finding: the de-latch package (525d6579c) was **incomplete** — it added `cooldown_secs`/`cooldown_max_rearms` to only 3 conditions and left 4 still legacy-latching. That, not the already-fixed set, is the top gap. Roadmap below.

---

# NEVER-STUCK HARDENING ROADMAP (verified @ 525d6579c)

## ★ SINGLE HIGHEST-LEVERAGE FIX — finish the de-latch package (4 cond…"
    }
  ],
  "totalTokens": 1847179,
  "totalToolCalls": 930
}

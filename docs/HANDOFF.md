> **Read this file first for current live-node state.** Session-by-session
> detail lives in `docs/work/archive/` (historical logs, not standing facts;
> full pre-slim history: [`HANDOFF-HISTORY-2026-07.md`](work/archive/HANDOFF-HISTORY-2026-07.md)).
>
> **⚡ Incoming developer, start here:** [`work/HANDOFF-2026-07-12-evening.md`](work/HANDOFF-2026-07-12-evening.md)
> — main @ `d5a682c64`; the exact un-wedge steps (mint → refold-on-copy → deploy)
> and the multi-user-server/game-platform foundation that just landed.

# HANDOFF — current state (2026-07-12)

## 1. The live node — the wedge

The public daily-driver (`~/.zclassic-c23` : 18232) is **wedged at
H\*=3,176,325** (`coins_applied` 3,176,326), blocker
`utxo_apply.anchor_backfill_gap`: `sapling_anchors` is empty below the
reducer cursor, so unknown Sprout/Sapling roots FAIL CLOSED and hold H\*.
soak (`:18242`) and dev (`:18252`) run newer builds near tip and are NOT
wedged. **Unwedging the live node via the sovereign cure is the #1 job.**
Verify with `zcl_status` / `zcl_state subsystem=reducer_frontier` before
acting — this file is a snapshot, not a live read. Root cause + mechanism:
[`docs/work/self-verified-tip-plan.md`](work/self-verified-tip-plan.md) and
memory `project_live_wedge_anchor_frontier_rootcause_2026-07-12`. The
**sovereign cure** is the self-verified UTXO/anchor rebuild that folds real
block bodies forward from the in-binary SHA3/PoW checkpoint and deletes the
borrowed `zclassicd`-minted seed path (see `CLAUDE.md` "Tenacity &
recovery").

## 2. In flight — the cure producer shipped, integration copy-prove is next

2026-07-12 (`origin/main` `a7d7baf6a`) landed the fast dev loop (`make ff` /
`code map` / `code tests` / watcher `MODE=verify`), the palace legibility
layer, and the sovereign-cure **producer**: one canonical
`coins_kv_snapshot_write()` (version-by-data, byte-identical, pinned SHA3
goldens) plus `snapshot_shielded_collect_from_db()` (Sapling+Sprout frontier
+ nullifiers), wired into `boot_mint_anchor.c`, and
`tools/snapshot_from_coinskv.c --shielded` (fast producer path, no fold).

**Blocker found driving the copy-prove:** a soak-datadir copy has the
Sapling frontier but not the historical Sprout frontier (soak was itself
seeded coins-only) — the collector correctly refuses to emit an incomplete
snapshot rather than fabricate one. Only the from-genesis `-mint-anchor`
fold on `~/.zclassic-c23-anchor-mint` (running ~70h+ — **do not touch**) has
the COMPLETE frontier. Two ways forward: (A, safe/slow) let that fold
finish, export from it, copy-prove, deploy; (B, needs a consensus answer
first) prove ZClassic's Sprout pool is genuinely empty at h≈3.17M, then fix
the collector to emit the canonical empty-Sprout root. Full detail:
[`docs/work/archive/SESSION-HANDOFF-2026-07-12.md`](work/archive/SESSION-HANDOFF-2026-07-12.md).

**Next job:** finish the integration copy-prove — `cp -a` a soak copy,
export a shielded snapshot, `cp -a` the live wedged datadir, boot the copy
with `-load-snapshot-at-own-height=<snapshot>`, gate on H\* climbing past
3,176,326 — then the owner-authorized live deploy with a staged rollback.
Do NOT delete the borrowed seed / flip `-refold-from-anchor` to default in
the same move — that is a separate, later, owner-gated step.

## 3. Zero-MCP program (secondary track)

Owner directive: delete the MCP server entirely — the native CLI
(`zclassic23 <command>`) becomes the ONLY agent interface. W0/W1-A/W1-B/C
(hot-swap re-targeted onto the command registry, `88b4e1030`) are DONE.
Next: W2 (~47 remaining call sites) then W3 (delete the MCP server), per
[`docs/work/MCP-REMOVAL-WORKLIST.md`](work/MCP-REMOVAL-WORKLIST.md) (the
114-site inventory). W3 is blocked on 3 prereqs — memory
`project_zero_mcp_w3_prereqs_2026-07-12`.

## 4. MVP status

**MRS 4/8** (do not bump without proof): C1 install, C2 Tor onion <60s, C4
receive shielded, C7 kill-9 recovery pass local operator proof. C3
cold-start, C5 file market, C6 7-day soak, C8 exact parity are gated on the
sovereign cure + accumulated soak time; current formal C6 judge is
`NOT_MET` (`operator_intervention_detected_x2`). Bar: [`docs/MVP.md`](MVP.md).
Live scorer: `tools/mvp_gate.sh`.

---

## 5. Live ops state & lanes

| Lane | Datadir | Deploy | Purpose |
|---|---|---|---|
| **live** | `$HOME/.zclassic-c23` | `make deploy` (owner-gated) | Public daily-driver node. Restart only for vetted live deploys. |
| **dev** | `$HOME/.zclassic-c23-dev` | `make deploy-dev` | Fresh-build lane for frequent development restarts. Never use live for hourly iteration. |
| **soak** | `$HOME/.zclassic-c23-soak` | deliberate re-baseline | Long-uptime / weekly evidence lane. Do not churn during development. |

The committed units declare the same intent to the binary with
`-operator-lane=canonical`, `-operator-lane=dev`, and `-operator-lane=soak`.
`zclassic23 agent`, REST `/api/v1/agent`, and MCP `zcl_operator_summary`
surface that as `operator_lane` (`zcl.operator_lane.v1`) with the lane's
restart policy. Prefer that native contract over parsing systemd names when a
lane's RPC is reachable.

`make deploy-dev` owns the dev service file and self-cleans a stale temporary
`zcl23-dev.service.d/reindex.conf` override unless
`ZCL_DEV_ALLOW_REINDEX_DROPIN=1` is set for an intentional one-off rebuild
(prevents an old override from silently forcing `-reindex-chainstate` on
every dev restart); it similarly self-cleans a stale `zz-oom-budget.conf`
drop-in unless `ZCL_DEV_ALLOW_OOM_BUDGET_DROPIN=1` is set.

`make lane-health` is the read-only three-lane status check. It reports the
public live lane, long-uptime soak lane, and fresh-build dev lane with systemd
state, RPC reachability, listener state, height, lag from the live lane, peer
count, restart count, memory pressure, role readiness, soak-evidence
eligibility, any `-reindex-chainstate` flag, and the binary-owned
`bootstrapstatus.snapshot_loader` posture: snapshot seed height, active loader
path, and `recovery_hint`. `role_ready` answers whether a lane is serving its
assigned purpose (`canonical_ready`, `soak_evidence_ready`, or
`dev_lane_ready`); the dev lane is not role-ready when its lag exceeds the lane
threshold, even if RPC/listeners are up. `soak_eligible=false` means the soak
lane is alive but not currently earning clean MVP-C6 evidence. It is an
observability/failsafe check, not an automatic failover mechanism.

`make lane-recover LANE=dev` / `LANE=soak` is the bounded noncanonical recovery
planner. It emits `zcl.lane_recovery_plan.v1`; with
`ZCL_LANE_RECOVERY_APPLY=1` it may install the tracked dev/soak unit, copy the
canonical seed snapshot into a noncanonical datadir, write the unit's optional
`ZCL_LANE_SNAPSHOT_LOADER_FLAG` drop-in, and restart only that noncanonical
service. It refuses `live`, `canonical`, and `main`; use it after
`make lane-health` reports `restart_with_load_snapshot_at_own_height` or
`install_tip_seed_snapshot` on dev/soak (header-import fallback flags exist
for a pre-RPC lane whose log says the snapshot loader skipped because
headers weren't synced yet — see `tools/scripts/lane_recover.sh`).

The live datadir runs `-load-snapshot-at-own-height` re-seeding 3,156,809 and
folding forward, so each restart is a ~13 min self-healing boot. `zclassicd`
(the C++ reference) runs as a co-located service — **never stop it.**
Ports/runbook: `docs/SYNC.md`; re-pull live state (`zcl_status`) before acting.

**Standing method:** `make deploy` rm's the binary first (a stale binary was
a real multi-day outage; `deploy_verify.sh` confirms `build_commit`).
Copy-prove every recovery path on a COPY before live, never live surgery;
gate on **H\* CLIMB**, not "booted without FATAL." Never weaken a
safety/operator gate. Gate every change: `make` + `make lint` +
`make test_parallel` (read the `N passed, M failed` line, not the pipe
exit). Replay any consensus-predicate tightening against REAL history first
(the h=478544 lesson — `docs/CONSENSUS_PARITY_DOCTRINE.md`).

---

## 6. Pointers

- [`docs/work/FORWARD_PLAN.md`](work/FORWARD_PLAN.md) — THE plan.
- [`docs/work/self-verified-tip-plan.md`](work/self-verified-tip-plan.md) — the sovereign-cure spine.
- [`docs/work/ROADMAPS.md`](work/ROADMAPS.md) — live vs superseded roadmaps.
- [`docs/AGENT_TRAPS.md`](AGENT_TRAPS.md) — looks-broken-but-isn't; read before "fixing" anything.
- [`docs/MVP.md`](MVP.md) — the v1 acceptance bar (8 criteria).
- [`docs/work/archive/`](work/archive/) — dated handoffs/audits/superseded roadmaps (history only).

## 7. Verify before you trust this file

A map, not the territory. Re-read the cited file:line before building on any
claim — trust the code you read THIS minute over this file. Architecture
reference (off the v1 path): [`docs/FRAMEWORK.md`](FRAMEWORK.md) +
[`docs/REFACTOR_STATUS.md`](REFACTOR_STATUS.md).

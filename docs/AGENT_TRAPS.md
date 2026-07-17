# AGENT_TRAPS.md — guardrails for a future Claude agent

**Verify fresh before trusting any line here; this doc itself rots.** Line numbers
shift under refactors. Read 30-50 lines around any `file:line` before acting on it.
Every claim below was true at the time it was recorded; the live code is the only
authority.

This doc exists to stop a future agent from re-discovering things that are already
done, re-proposing optimizations that already ship, or "fixing" patterns that are
intentional consensus-parity decisions.

---

## (00) MANTRA — code fearlessly; immutable history is the oracle

ZClassic history is immutable. We cannot lose or rewrite canonical historic
data by breaking a local build, a throwaway datadir, a generated snapshot, or a
test fixture. Use that aggressively: make copies, replay real blocks, pin
historical fixtures, and delete/recreate derived artifacts when that is faster
than nursing them. Prefer real-chain canaries over imagined edge cases whenever
the chain already contains the answer.

The boundary is live-first surgery: do not mutate the operator's serving
datadir to test a repair. Copy it, reproduce there, prove H* climbs or the
historical fixture passes, then deploy/restart intentionally.

---

## (0) LIVE OPS TRAPS — public service vs private candidates

- **Public connected-node tables can lag or cache old peer identity.** Verify
  the live socket before trusting a crawler row. As of 2026-07-07,
  `205.209.104.118:8033` is owned by `zclassic23`, `zclassicd-rhett.service`
  is inactive, and local peer telemetry sees that remote as
  `/ZClassic23:0.1.0/`. A stale public row may still show the old
  `MagicBean:*` service until the crawler reconnects or refreshes its peer
  cache. Cutover narrative removed from the tree; recover with
  `git log --follow -- docs/work/archive/remote-node-cutover-205209104118.md`.
- **Copied `block_index.bin` plus foreign `blocks/blk*.dat` is unsafe.** The
  block index contains source datadir file offsets. A non-empty block file with
  the same number is not enough proof that an indexed body is valid. In
  `-nolegacyimport` snapshot boots, existing block files must be untrusted
  unless the indexed block reads back and hashes to its block-index entry.
- **HODL/explorer wait regressions have a public smoke test.** Use
  `tools/scripts/public_explorer_smoke.sh`; it checks both `/api/v1/hodl` and
  `/explorer/hodl` without `jq` and fails on "refresh", "not processed",
  "retry", or "waiting" user-visible states.
- **There is currently no runtime-generation publication authority.**
  `dev.change.apply`, `dev.hotswap.apply`, publication watcher modes,
  `make hotswap`, `deploy-dev*`, `agent-deploy-fast`, `agent-stage-dev`, and
  the direct deploy/hot-swap scripts all hard-refuse. Remote update/install/
  restart variables and `lane_recover --apply` also refuse before SSH, file,
  datadir, or service mutation. A source ID, environment switch, or direct
  script call does not bypass containment. Use build, simulation, read-only
  plans and verify/check watch only; resident hot-swap probing is contained.
- **Dev recovery is plan-only during containment.** `make agent-dev-recover`
  is read-only. `ARGS=--apply`, direct `recover-dev-lane.sh --apply`, and test
  environment variables cannot relink an existing generation or restart the
  service; only the isolated inherited-FD self-test reaches recovery machinery.
- **“Non-consensus” does not automatically mean hot-swappable.** The v2 loader
  admits only exact `config/hotswap_eligible.def` entries that export a
  stateless MCP route manifest with the required ABI, capabilities, hashes,
  tests, probes, self-test, and no-quiescence contract. REST, diagnostics,
  services, models, storage, events, conditions, supervisors, networking,
  wallet/key/crypto state, reducers, and process ownership are
  `reload_required`. Never widen the allowlist to silence that blocker.
- **Do not interpret a contained hot-swap call as a transport failure.**
  `make hotswap`, `tools/dev/hotswap-running-dev.sh`, native
  `dev.hotswap.apply`, and legacy `zcl_agent_hotswap` refuse before `dlopen` or
  resident RPC mutation. There is no exit-69 reload fallback and no successful
  committed generation to inspect during containment.
- **ZVCS revert is source-only.** `dev.vcs.revert` is available with
  `relink_generation=false`. Passing `true` refuses before the source revert;
  it never rebuilds, activates, or guesses a binary generation.
- **Do not infer latency SLOs from a safe/default benchmark run.**
  `make dev-loop-bench` cannot opt into hot-swap or process-reload activation
  while containment is active. Build/check timings cannot support hot-swap or
  reload SLO claims.
- **Do not hand-maintain `compile_commands.json`.** Run `make agent-index`.
  It derives commands from the real `DEV_OBJS` recipes, including generated
  headers and the target-specific `-Og`/hot-bucket `-O2` split, then records
  hash/freshness metadata. clangd is optional and its absence is not an index
  generation failure.

---

## (1) STALE FACTS — old belief → current truth

- **getblockcount serves active_chain_height.** FALSE at HEAD. It serves `reducer_frontier_provable_tip_cached()` (H*, the provable frontier) since commit `e75b5c62c` (2026-06-20). Internal code still uses `active_chain_height` for lookahead, but only external/served RPCs use H*. → `app/controllers/src/blockchain_controller_blocks.c:50-66`.
- **P2P start_height advertises active_chain_height (or the sync-window tip).** FALSE. It advertises `reducer_frontier_provable_tip_cached()` (H*) since 2026-06-20 — only the provable height, never the lookahead tip that can rewind under a reorg. → `lib/net/src/msg_version.c:155` (comment at `:149-154`).
- **getbestblockhash / getblockchaininfo serve active_chain_height and are inconsistent with getblockcount.** FALSE. All three serve H* and are internally consistent: `getblockchaininfo.blocks` returns `reducer_frontier_provable_tip_cached()` and resolves the tip hash at that same H* height. → `blockchain_controller_blocks.c:69-93` (getbestblockhash via `rpc_provable_tip` at H*); `app/controllers/src/blockchain_controller_chain.c:55-93` (getblockchaininfo: H* at `:84`, `active_chain_at(H*)` at `:85-86`). The "getbestblockhash serves active_chain_tip (inconsistent by 1)" claim in
the removed `docs/work/archive/FUTURE-CLAUDE-FIX-THIS.md:453-457` was stale
(recover with `git log --follow -- docs/work/archive/FUTURE-CLAUDE-FIX-THIS.md`).
- **Bare `build/bin/zclassic-cli` is always the zclassic23 status target.** FALSE. It can follow local defaults, cookies, datadirs, or environment and answer from another RPC target. For zclassic23 stability checks use the C-owned agent surfaces first (`zclassic23 status`, `zclassic23 agent`, `zclassic23 agentdiagnose`, `zclassic23 getmirrorstatus`, or MCP equivalents), or make direct RPC explicit with `build/bin/zcl-rpc getblockcount` / `build/bin/zclassic-cli -rpcport=18232 getblockcount`. A bare CLI height mismatch is an operator-interface ambiguity until the target lane is proven.
- **Heal/repair never deletes the upstream validation logs.** FALSE at HEAD. Heal deletes BOTH `script_validate_log` and `proof_validate_log` in the same transaction when repairing stale/retriable verdicts. → `app/jobs/src/stage_repair_reducer_frontier_coin.c:459-462` (`delete_log_range` for both logs inside `stale_script_replay_tx`, gated by `dry_run_stale_script_replay`). The "heal never clears upstream logs" framing is explicitly marked stale in `docs/work/self-verified-tip-plan.md`.
- **`docs/work/sync-organism-map.md` "Wound 2" described current served heights.** DELETED (2026-07-11) — it was a STALE doc (2026-06-19 era) that claimed (`:69-70`) "active_chain_height — what getblockcount and P2P start_height SERVE and ADVERTISE" with refs `blockchain_controller_blocks.c:39` and `msg_version.c:148`. Both line numbers and both claims were wrong: actual lines are 50 and 155, and BOTH serve H*. `never-stuck-plan.md` (same era) records FIX-1 (wire H* to served APIs) as DONE in `e75b5c62c`, but `sync-organism-map.md` was never updated before it was removed. Keep the corrected facts above (getblockcount/start_height serve H*) as the live truth; do not re-create the file or trust anything citing it.

---

## (2) ALREADY SHIPPED / DEAD — don't re-propose; don't assume it's live

### Already shipped — do NOT re-propose

- **Deferred ECDSA / proof verification below a checkpoint height.** Done. Two gates: `checkpoint_covers()` marks any height with a checkpoint entry as covered, and `g_deferred_proof_validation_below_height` gates expensive Sapling/JoinSplit proofs. When covered, `expensive_checks=false` skips PoW, script verification, and proof re-verification. → `lib/validation/src/connect_block.c:160-166`, `:182-186`; `contextual_check_tx.c:26`, `:77-78` (skip_proofs gates JoinSplit Ed25519 `:99`, Sapling Groth16 `:106-144`, Sprout `:147-172`); `chainparams.c:13-82` (63 checkpoints, every 50k blocks genesis→3,100,000).
- **Parallel ECDSA script verification in connect_block.** Done and live on the hot path. A lazy-initialized global workpool `g_script_pool` fans jobs to workers when `num_checks >= 4`, inline fallback below that. → `connect_block.c:66-82` (`get_script_pool()`), `:556-627` (Phase 1 collect), `:660-671` (Phase 2 dispatch), `:687` (telemetry `parallel=yes/no`).
- **Parallel-dispatch threshold for small batches.** Done. `num_checks >= 4` is the explicit heuristic to avoid scheduling overhead. → `connect_block.c:663`, `:687`.
- **Precompute per-tx sighash data before parallel dispatch.** Done. `struct precomputed_tx_data` is built once per tx and shared read-only across all parallel input verifications. → `connect_block.c:52` (struct, "shared per-tx, read-only"), `:414` (batch storage), `:571` (`precompute_tx_data` once per tx), `:621` (per-input pointer share); `sighash.h:25`.
- **Per-input MoneyRange validation.** Done. Every input value is range-checked at script-collection time, before script work is batched. → `connect_block.c:587-596` (rejects `bad-txns-inputvalues-outofrange`), listed as a new addition at `:18`.
- **BIP30 duplicate-coinbase skip below the deferred-proof height.** Done, intentional for snapshot re-connection / kill-9 recovery (coinbase outputs already exist in the imported UTXO set). → `connect_block.c:290-291` (`skip_bip30`), `:293` (gated loop), comment `:286-290`.

### Advisory scaffolding — do NOT wire into consensus thinking it authenticates state

- **The `xor_accumulator`-fed commitment MMR is advisory, sparse, and not consensus binding.** `boot_services.c` currently calls `rpc_blockchain_maybe_commit()` once to bootstrap an empty commitment history; no per-tip producer builds the history during ordinary runtime. The root is exposed by `getcommitmentmmr`/`auditchain`, but no consensus path reads it. Neither this XOR accumulator nor the MMB leaf's auxiliary `utxo_root` is committed by a ZClassic header, so neither can authenticate imported state or bind peer state to PoW. Treat `audit_passed` as internal-structure coverage only until this misleading compatibility surface is removed or renamed; keep snapshots assisted until local full-history promotion.

### Shipped but DEFAULT-OFF — present, not active (see also section 3 for why)

- **Sapling-root full-parity check.** Pure recompute predicate `sapling_root_matches()` is wired but gated on `g_enforce_sapling_root` (atomic, default false; only `-enforce-sapling-root` arms it). Default behavior is byte-identical to today (rejects only all-zeros root). → `connect_block.c:134-151` (predicate), `:725-735` (gate), `src/main.c`.
- **OP_CHECKDATASIG[VERIFY] sigop counting.** `SCRIPT_VERIFY_CHECKDATASIG_SIGOPS` is conditionally ORed only when `g_enforce_checkdatasig_sigops` is true (atomic, default false; `-enforce-checkdatasig-sigops`). Default flags omit it. → `connect_block.c:352` (default flags), `:359-361` (conditional OR), `src/main.c`.

---

## (3) NOT A BUG — INTENTIONAL — apparent bug → why → what breaks if "fixed"

- **Oversize tx at h=478544 (125,811 B) is not rejected at MAX_TX_SIZE_AFTER_SAPLING=102000.** Intentional: 413 canonical oversize post-Sapling txs (heights 478544..1968856, max 1922197 B) are grandfathered via a static sorted allowlist, checked ONLY in `DOMAIN_TX_CTX_BLOCK` context. Running zclassicd nodes ACCEPT these because validated blocks are never re-checked — this reproduces LIVE behavior, not the text. **Breaks if fixed:** forks the chain against every running zclassicd node. → `domain/consensus/src/tx_structural.c:108-126` (`:121` block-context gate, `:124` bsearch); `tools/data/oversize_grandfather_txids.txt` (413 entries); `docs/CONSENSUS_PARITY_DOCTRINE.md:76-130`. Also: don't "skip the lookup for perf" — cold path fires at most 413 times in a full reindex; cost is negligible vs fork risk.
- **`wallet_view_sync` Sapling placeholder crypto is a view-only marker, not spendable note material.** Intentional: zclassicd `z_listunspent` gives balance/output metadata, not the decrypted Sapling note fields required for a real spend. zclassic23 stores deterministic placeholders as `wallet_sapling_notes.source='view'`; the fake `ivk` must not match a real keystore `ivk`, successful empty view results clear only `source='view'` rows, and a spend attempt against only those rows returns `view-only balance synced from zclassicd`. **Breaks if "fixed" by reusing real IVKs or deleting all notes on refresh:** external wallet-view data could enter spend selection or clobber durable local/catchup notes. → `app/controllers/src/wallet_view_sync.c`; `app/models/src/sapling_note.c`; `app/controllers/src/wallet_shielded_send_shielded.c`; `lib/test/src/test_wallet_funds_safety.c`.
- **`-enforce-coinbase-maturity` is DEFAULT-OFF.** Intentional. A tightening (reject) predicate must NOT ship until a full genesis→tip replay confirms ZERO false-rejects (the h=478544 lesson: you cannot assume what the canonical chain contains). Enabling before replay risks permanently wedging the node on old data. → `src/main.c:1849-1859`; `app/jobs/include/jobs/utxo_apply_delta.h:82-91`; `app/jobs/src/utxo_apply_delta.c:267-286`; `lib/test/src/test_utxo_apply_coinbase_maturity.c`.
- **`-enforce-checkdatasig-sigops` is DEFAULT-OFF.** Same reason: tightening predicate, gate on full-history replay. Default `connect_block` flags do not include the bit. **Breaks if defaulted on:** risk of false-rejecting old blocks → fork/wedge. → `connect_block.c:93-127`, `:352-361`; `domain/consensus/src/check_block.c:47-57`; `src/main.c:1860`; `lib/test/src/test_connect_block_checkdatasig_sigops.c`.
- **`-enforce-sapling-root` is DEFAULT-OFF.** Same doctrine. Must prove safe by full replay (0 false-rejects) before arming. **Breaks if defaulted on:** can permanently wedge on old blocks with now-detected shielded-commitment differences = a fork. → `src/main.c:1838-1843`; `connect_block.c:93-97`; `lib/test/src/test_connect_block_sapling_root.c`; `docs/CONSENSUS_PARITY_DOCTRINE.md:14-19`.
- **`contextual_check_block()` is skipped during IBD / historical replay.** Intentional: it IS wired on the connect path but fires ONLY near the live tip and ONLY when NOT in IBD, reproducing zclassicd's `ContextualCheckBlock`/`ContextualCheckTransaction` IBD short-circuit. A stage that halts honestly on `bad-cb-height`/`bad-txns-nonfinal` near tip is the CORRECT state, not a hang. **Breaks if the IBD gate is removed:** false-rejects old canonical blocks (non-final `nSequence`, past-height expiry, BIP34). → `app/jobs/src/script_validate_contextual.c:61-108` (proximity/IBD gate at `:85-92`, `is_initial_block_download` at `:95`); `contextual_check_block()` defined at `lib/validation/src/check_block.c:436`; `lib/test/src/test_script_validate_contextual_gate.c`.
- **JoinSplit Ed25519 sig verification on the connect path.** Intentional and load-bearing — runs BEFORE the per-joinsplit zk-SNARK loop and fails the whole block on an invalid `joinSplitSig` (`ok=0`, `first_failure_proof_type='joinsplit_sig'`), blocking the tip until a valid block replaces it. The SNARK binds `joinSplitPubKey` into `h_sig` but does NOT replace the Ed25519 sig, so this is required, not redundant. → `app/jobs/src/proof_validate_stage.c:141-147`; §2 item 3 of
`docs/work/consensus-parity-supplemental-audit-2026-06-08.md`.
- **`fCoinbaseMustBeProtected` is gated by chain params, not unconditional.** Intentional parity: fires on mainnet/testnet (`fCoinbaseMustBeProtected=true`), off on regtest (false), exactly as zclassicd gates it per-chain. **Breaks if the gate is removed:** breaks testnet/regtest compatibility. → `app/jobs/src/utxo_apply_delta.c:420-439`; `app/jobs/src/utxo_apply_stage.c:605-610`;
§2 item 5 of `docs/work/consensus-parity-supplemental-audit-2026-06-08.md`.
- **Upstream-hole (stale-replay artifact) returns JOB_IDLE, not JOB_BLOCKED.** Intentional. `JOB_BLOCKED` feeds the supervisor escalation/restart ladder, and a watchdog self-restart is what manufactures this hole class — re-blocking would re-trigger the watchdog that created it (a loop). The alarm is LOGGED+COUNTED, not escalated. **Breaks if changed to JOB_BLOCKED:** escalation loop. → `app/jobs/src/utxo_apply_stage.c:405-418` (`:409-414` comment, `:415` `upstream_hole_note()`).
- **The "waiting for Sapling params to load" wait (there is NO enum named `JOB_WAIT_PARAMS` — do not invent one) instead of erroring.** Intentional and recoverable. Two distinct shapes: the contextual path returns `SV_CTX_WAIT_PARAMS` (`script_validate_contextual.c:101-103`, enum at `jobs/script_validate_contextual.h:29` documented as "recoverable, JOB_IDLE"), driven through `script_validate_stage.c:446` (the `SV_CTX_WAIT_PARAMS` case); the proof_validate path sets `internal_error=true` / `first_failure_proof_type="params_not_loaded"` (`proof_validate_stage.c:119-124`). Params load in a background boot thread; boot only WARNs on failure (`config/src/boot_services.c:836`). Returning a hard error would permanently reject valid canonical shielded blocks. A persistent wait looks like a hang but is correct when params fail to load — fix the params path or pass `-nosaplingverify`. (The job-result enum is `JOB_BLOCKED`/`JOB_IDLE`/`JOB_FATAL` etc. in `jobs/job.h:36-37` — there is no `JOB_WAIT_PARAMS`.)

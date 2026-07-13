# Developer handoff — 2026-07-12 evening (main @ d5a682c64)

A large session just pushed **9 lanes** to `main` (`2678b7e51 → d5a682c64`, +7,640 LOC / 68 files), all parity-safe and gated green (`make lint` + `make build-only` + focused groups). This is the pickup point. Two tracks landed; **the live node is still wedged — un-wedging it is #1.**

## 1. Node sync — the #1 job (live node WEDGED)

**State (verify live before acting — `zclassic23 status`):** canonical node `~/.zclassic-c23:18232` (build `3b0de63b0`) wedged at **H\*=3,176,325**, ~2,766 blocks behind. Deterministic **shielded-state** wedge, not a network problem: block 3,176,326 has shielded txs; the Sapling/Sprout commitment-tree frontier is **empty** (node was seeded coins-only), so `fold_sapling` fail-closes at `app/jobs/src/utxo_apply_anchors.c:130`. `download_queue_starved` is a *symptom*.

**The un-wedge = ACT 3 of `docs/work/self-verified-tip-plan.md`. Exact steps:**
1. **Mint the shielded anchor artifact** (landed this push: U1's accelerated, observable, resumable mint — ~hours not days):
   ```bash
   MINTDIR=$HOME/.zclassic-c23-mint-new-copy; mkdir -p "$MINTDIR"
   build/bin/zclassic23 --importblockindex "$HOME/.zclassic" "$MINTDIR/node.db" # ~60-74s
   ZCL_FOLD_INRAM=1 ZCL_REFOLD_DRAIN_BATCH=2000 ZCL_REFOLD_TICK_MS=250 \
     build/bin/zclassic23 -datadir="$MINTDIR" -mint-anchor -fold-inram
   tail -f "$MINTDIR/mint-progress.log"     # height/rate/eta, readable from disk
   # success -> $MINTDIR/utxo-anchor.snapshot (v3 shielded, SHA3==checkpoint hard-asserted at boot_mint_anchor.c:194)
   ```
   `--importblockindex` is recognized only as argv[1]. Putting
   `-datadir="$MINTDIR"` first starts a normal node instead of importing the
   legacy block index. The imported index's body positions are valid only with
   the exact source `blk*.dat` corpus; matching a foreign block filename is not
   proof. Always use a new isolated producer path and copy-proof it—never delete
   or rewrite a running producer or serving datadir.
   NOTE: an older mint has run ~88h on `~/.zclassic-c23-anchor-mint` (opaque, in-RAM, PID 3329835) — check whether it emitted `utxo-anchor.snapshot` first; if so, reuse it.
2. **Copy-prove** (never live surgery): `cp -a` the wedged datadir; boot the copy with `-refold-from-anchor` + the artifact; **gate on G-SOV** (`self-verified-tip-plan.md:63-68,223-236`): H\* climbs past 3,176,326, `coins_applied_height==H*+1`, refold marker present. Tooling: `tools/repro_on_copy.sh`.
3. **Owner-authorized live deploy** with staged rollback. Do NOT also delete the borrowed seed / flip `-refold-from-anchor` default here (later ACT-3 step).

**Landed this push (parity-safe, gated):**
- **U1** `app/jobs/refold_cadence.{c,h}` + supervisor/reducer/boot_mint_anchor — env-tunable fold cadence (`ZCL_REFOLD_DRAIN_BATCH`/`ZCL_REFOLD_TICK_MS`, defaults 2000/250ms) that speeds mint+refold; **inert on the live hot path** (supervisor `period_us` defaults 0; guard test proves env ignored when inactive). Mint observability (`mint-progress.log`) + documented resume chain.
- **R3** `app/conditions/download_queue_starved.c` — defers the false operator page when a `BLOCKER_PERMANENT` fold blocker is active (reuses `blocker_count_by_class`, `chain_tip_watchdog.c:304`).
- **R4** `app/services/sapling_checkpoint_hook.c` + `process_block_flush_policy.c` — binds the Sapling flat-file checkpoint to the reducer's applied frontier (refuses checkpoints written ahead of it), and checkpoints in-tx beside the anchor-mint hook. Projection-only.

**Fast leads (analysis only, not built):**
- The "142s boot" is **stale** — a fix landed on main; warm restart is now 58–92s. Next bottleneck = an **untimed** section of `block_index_reconcile` (`validate_coins_chain_agreement` + `utxo_recovery_execute`, `config/src/boot.c:2802-2820`). Cheapest fix: add one `boot_submark("blkidx.validate_recover", …)` after line 2832 to split its cost out. Boot spikes correlate with the concurrent mint process's host contention.
- **Superseded fast-sync lead:** a per-height SHA3 `utxo_root` in an auxiliary
  MMB leaf is not committed by ZClassic headers and cannot make peer state
  trustless. It remains useful as advisory/copy evidence only. Fast readiness
  must stay explicitly assisted until local full-history verification promotes
  the exact complete state; see `SOVEREIGN-NETWORK-ROADMAP.md`.
- LB-1 parallel verify pool: **deleted** from the tree; rebuild per `docs/work/lb1-wiring-design.md` (throughput multiplier, **not** required for the cure).

## 2. Multi-user-server / game-platform foundation (P0 libraries — built, not yet wired into runtime)

Parity-safe library code; no runtime listener/session wired yet. This is the substrate for a crypto-authenticated multi-user shell + a P2P C23 game-dev/self-publish platform.
- **crypto**: `lib/crypto/{hkdf_sha256,x25519_safe}` + `lib/session/{noise_handshake (NK for client↔server + XX for peers, pattern-parameterized), session_transport (AEAD record framing)}`.
- **sandbox**: `lib/platform/os_sandbox_linux.c` — rootless user-namespace + Landlock + seccomp deny-list + rlimit builders + `SANDBOX_SESSION_CHILD`. **Empirically probe-verified rootless on this box** (`docs/work/session-substrate-probes.md`).
- **no-shell-outs**: `lib/util/{file_tree_ops (fd-based recursive copy/remove), spawn (no-shell fork/exec)}`. The ~11 call-site conversions + the `check-no-shell-out` gate are **NOT done** yet.
- **accounts**: `app/models/{principal,auth_challenge}` + `app/services/{auth_login_service,authz_policy}` + `config/commands/accounts.def` + the command-registry **authority ceiling** gate (`lib/kernel/command_registry.c`). secp256k1 challenge-response login; role→capability; migration **v26**; commands under `app.auth.*`/`app.account.*`.

## 3. Where the plans live
- **Architecture + phased plans** (game-dev + self-publish platform, multi-user server P1-P3, Track 2 source publishing, dev-productivity substrate): `~/.claude/plans/can-you-structure-the-proud-galaxy.md`.
- **ADR-0003** (`docs/adr/0003-os-substrate-verdict.md`) + `docs/work/os-substrate-plan.md` — the "no OS kernel, build organs" verdict + the three rungs.
- **Un-wedge spine**: `docs/work/self-verified-tip-plan.md`. **Never-stuck**: `docs/work/never-stuck-plan.md`.

## 4. Doctrine (non-negotiable)
- **Consensus parity is inviolable** — everything above is app-layer / dev-tooling; no consensus/wire/validity code was touched.
- **Copy-prove before any live-node change**; gate on **H\* CLIMB**, never "booted without FATAL." `test_parallel` green is a floor, not liveness.
- The mint/refold cadence override is **cadence-only** (parity-safe) and inert on a normal live boot.

# Sovereign cutover runbook — replace the borrowed seed with the self-minted anchor

**Status 2026-06-23.** The sovereign cure is ~95% in place. This is the exact, copy-prove-gated
procedure to execute the *last* step when the mint finishes. Verified read-only against HEAD
`9f48af7b5` (workflow `wf_6b148b3c-fa7`). Re-verify every file:line before acting — specifics rot.

## Where we are (the only remaining gate is the durable artifact, not code)

- **Parity blocker: RESOLVED** (`9fe9a8ee6`, ancestor of HEAD). `utxo_apply_delta.c:381` excludes
  `script_is_unspendable` outputs + `:168-171` excludes the genesis block by hash — matching
  zclassicd's `IsUnspendable()` (`script.h:132-136`) exactly. A genesis→anchor fold now reaches
  `count==1,354,771` + the compiled SHA3 root, so `-mint-anchor`'s hard-assert
  (`boot_mint_anchor.c:155-180`) passes instead of FATAL-ing.
- **Cutover mechanism: fully wired in HEAD, NO code work.** `-refold-from-anchor` →
  `boot_refold_from_anchor_reset` (`boot_refold_staged.c:306-469`): SHA3-binds the snapshot to the
  compiled checkpoint via `uss_open(verify_full_sha3=true)` *before any coin lands*, hard-asserts
  `coins_kv_commitment==cp->sha3_hash` AND `count==1,354,771` (`:360-386`, `_exit` on mismatch),
  resets the 8 stage cursors to anchor=3,056,758 + `coins_applied_height=anchor+1` (`:399-443`),
  then re-folds anchor+1..tip over on-disk bodies.
- **The missing piece: a durable, checkpoint-verified anchor snapshot at h=3,056,758.** It does NOT
  exist yet. (`/tmp/utxo-anchor-3056758.snapshot` is MISLABELED — its header decodes to
  height=3,151,901, count=1,344,817; the node rejects it on the count check. Do not use it.)
- **The producer:** the running mint (PID 3857784, `-mint-anchor -mint-anchor-fast`, `/dev/shm/fmram`,
  `ZCL_MINT_ANCHOR_OUT=/tmp/anchor-ram.snapshot`) at ~11.5% (h≈352k/3,056,758). On completion it
  writes the verified snapshot to **`/tmp/anchor-ram.snapshot`** (durable disk, not /dev/shm).

## The one open risk (owner call): the mint is volatile + non-resumable

`-mint-anchor` resets the fold to genesis on every boot (`boot_refold_staged.c:113,115`), so it is
NOT resumable; its working state is on volatile `/dev/shm` with system swap full. A reboot/OOM before
completion loses ~15 days. The machine currently has ~66 GB available (cache reclaimable), so it is
not presently threatening the live node — but the exposure stands for ~15 days.

- **Option A (let it ride — current recommendation):** the output lands durably at
  `/tmp/anchor-ram.snapshot`; accept the reboot exposure. Zero extra load.
- **Option B (durable + resumable, if the exposure is unacceptable):** run a SECOND fold on a DISK
  datadir WITHOUT `-mint-anchor` so it resumes from `progress.kv` and the per-block self-mint hook
  (`utxo_apply_stage.c:607`) writes the verified snapshot at the anchor crossing — durable, resumable,
  ~1.6 GB RSS (does not worsen the swap pressure), but slower (full crypto) and starts from 0. Needs
  bodies genesis..3,056,758 on disk first (207 GB free on `/`; seed via the `--importblockindex`
  two-step or a read-only copy of the live node's `blocks/`). Do NOT touch the running mint either way.

## Cutover procedure (run when `/tmp/anchor-ram.snapshot` exists)

NEVER live surgery. Copy-prove first, gate on **H\* CLIMB** (not "booted without FATAL" — that is the
`-load-verify-boot` false-green trap: it no-ops on the stamped coins_kv because
`coins_kv_is_proven_authority` returns true, `boot_refold_staged.c:884`).

1. **Confirm the artifact is the real anchor** (the mint already hard-verified it at write; re-confirm):
   - header height == 3,056,758, count == 1,354,771, body SHA3 == compiled `cp->sha3_hash` (`00e95dbd…`).
2. **Stage it** (idempotent; the node re-verifies SHA3 before trusting — a bad copy is ignored):
   ```bash
   ZCL_ANCHOR_SNAPSHOT_SRC=/tmp/anchor-ram.snapshot make seed-anchor-snapshot
   # copies to ~/.zclassic-c23/utxo-anchor.snapshot (NOT the mislabeled /tmp default)
   ```
3. **Copy-prove on a COPY of the live datadir** (never the live PID first):
   ```bash
   cp -a ~/.zclassic-c23 /scratch/c23-cutover-prove          # ensure utxo-anchor.snapshot copied too
   build/bin/zclassic23 -datadir=/scratch/c23-cutover-prove -refold-from-anchor \
     -nobgvalidation -nolegacyimport -port=8123 -rpcport=18432 -connect=<a_peer>
   ```
   - **GATE (H\* CLIMB):** `reducer_frontier_compute_hstar` / `getblockcount` (= H*) must climb
     STRICTLY past 3,056,758 toward the active tip (~3,157,0xx), with
     `coins_kv_get_applied_height == hstar+1` at every step (no rowless span). Confirm
     `zcl_state subsystem=block_index`. The decisive positive proof is
     `test_reducer_forward_progress_gate.c` PART-1 (`ok && found && hstar==N && applied==N+1`).
   - **Cross-check** the resulting block hash vs zclassicd (rpc 8232) at the anchor AND at tip.
   - Confirm body contiguity (3,056,758..tip) first — `boot_refold_body_span_contiguous`
     (`boot_refold_staged.c:928`) is NOT called on the explicit-flag path, only the auto-arm path.
4. **Flip the live node** only after the copy passes: swap the systemd drop-in from
   `-load-snapshot-at-own-height=…utxo-seed-3156809.snapshot` to `-refold-from-anchor`, restart, and
   re-confirm the H\* CLIMB gate live. Keep the old drop-in as the rollback.

## The subtraction (delete the borrowed loader — the payoff)

Once the live node runs on the self-minted anchor, delete the borrowed-seed producers
(per `sync-fix-plan-2026-06-21.md` "Deletable LOC"; ~400 LOC deletable immediately):

- `utxo_recovery_restore.c:369-371` `coins_kv_seed_from_node_db` bulk-copy + `:381-384` cold-import seed.
- `utxo_recovery_restore.c:240-242` `utxo_recovery_commit_tip(..., frontier_exempt=true)` (the only
  production call passing `true`) + `:252-254`. **CONSENSUS-PARITY-FLAGGED, parity-RESTORING.**
- `block_index_loader_rebuild.c:534-768` `block_index_loader_seed_stages_from_cold_import` + helper
  `:500-532` + defines `:486-488` + the boot fallback `boot_services.c:1524-1526` (~270 LOC).
- `utxo_recovery_seed_provenance.c` cold-import-seed writers (~90 LOC; KEEP `clear_cold_import_seed`).

A further ~2,930 LOC production (the seed-tear coin-backfill engine) is deletable AFTER the cutover
clears the **replay gate** against the real chain (h=478544 doctrine) + the STEP-1 polarity inversion.

## Optional follow-on (not required for the explicit-flag cutover)

`boot_refold_from_anchor_arm_default` does not exist yet (grep-confirmed). Making `-refold-from-anchor`
the cold-start DEFAULT (sync-fix-plan STEP 1) is a small separate change that enables the deletion of
the torn-detect gate — do it AFTER the live cutover is proven, not before.

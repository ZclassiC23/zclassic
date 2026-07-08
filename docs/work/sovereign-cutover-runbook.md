# Sovereign cutover runbook - replace the borrowed seed with the self-minted anchor

**Status 2026-07-08.** The sovereign cure is ~95% in place. This is the exact,
copy-prove-gated procedure to execute the last step when the mint finishes.
The previous transient producer was stopped before publishing an artifact after
review exposed that `-mint-anchor-fast` could still start normal runtime
services before the one-shot driver. The current fix makes `-mint-anchor` an
offline reducer driver: `app_init` returns before `app_init_services`, the eight
stages are initialized without supervisor children, and shutdown uses
`app_shutdown_offline`. A later producer run reached ~110k/3,056,758 and then
OOM-killed before producing an artifact; `-mint-anchor` is now restart-safe via
a checkpoint-bound `progress.kv` marker (`mint_anchor_in_progress_v1`) so a
restart resumes a matching interrupted fold instead of resetting to genesis.
On 2026-07-08, the resumed producer was diagnosed as torn: its coin frontier had
advanced past a missing `utxo_apply_log` / `utxo_apply_delta` row. That datadir
is preserved at `$HOME/.zclassic-c23-anchor-mint-torn-20260708-083231`, the
current tree now refuses RAM flushes without reducer witness rows, and a fresh
producer was relaunched from the stopped full-history source copy. The artifact
is still pending. Monitor the fresh run with `zclassic23 anchorstatus`; do not
cut over from a partial fold.
Re-verify every file:line before acting; specifics rot.

## Where we are (the only remaining gate is the durable artifact, not code)

- **Parity blocker: RESOLVED** (`9fe9a8ee6`, ancestor of HEAD). `utxo_apply_delta.c:381` excludes
  `script_is_unspendable` outputs + `:168-171` excludes the genesis block by hash — matching
  zclassicd's `IsUnspendable()` (`script.h:132-136`) exactly. A genesis→anchor fold now reaches
  `count==1,354,771` + the compiled SHA3 root, so `-mint-anchor`'s hard-assert
  (`boot_mint_anchor.c:155-180`) passes instead of FATAL-ing.
- **Cutover mechanism: wired but still copy-prove gated.** `-refold-from-anchor` →
  `boot_refold_from_anchor_reset` (`boot_refold_staged.c:306-469`): SHA3-binds the snapshot to the
  compiled checkpoint via `uss_open(verify_full_sha3=true)` *before any coin lands*, hard-asserts
  `coins_kv_commitment==cp->sha3_hash` AND `count==1,354,771` (`:360-386`, `_exit` on mismatch),
  resets the 8 stage cursors to anchor=3,056,758 + `coins_applied_height=anchor+1` (`:399-443`),
  confirms the explicit fold body span is present, then re-folds anchor+1..tip over on-disk bodies.
- **The missing piece: a durable, checkpoint-verified anchor snapshot at h=3,056,758.** It does NOT
  exist yet. (`/tmp/utxo-anchor-3056758.snapshot` is MISLABELED - its header decodes to
  height=3,151,901, count=1,344,817; the node rejects it on the count check. Do not use it.)
- **The producer:** no durable artifact exists yet. The isolated mint datadir was
  recreated from the stopped full-history copy
  `$HOME/.zclassic-c23-COPY-20260701-113424-stall-3166384` and launched from
  a clean `build/bin/zclassic23` built at `16c841ca8`, but that run OOM-killed
  before completion. On a post-2026-07-03 binary, restart the same datadir with
  `-mint-anchor`; current binaries make that offline path default to the
  `coins_ram` in-RAM hot store (`-fold-inram` remains accepted and is shown
  explicitly below for older binaries / operator clarity). The resume marker
  logic will adopt the legacy interrupted fold only when `refold_in_progress`
  is set and `coins_applied_height` is still within the genesis..anchor span.
  Recreate the datadir only if you intentionally want to discard the partial
  fold or the resume predicate refuses it:
  ```bash
  rm -rf $HOME/.zclassic-c23-anchor-mint
  cp -a $HOME/.zclassic-c23-COPY-20260701-113424-stall-3166384/. \
    $HOME/.zclassic-c23-anchor-mint/
  rm -f $HOME/.zclassic-c23-anchor-mint/zclassic23.pid \
    $HOME/.zclassic-c23-anchor-mint/.lock \
    $HOME/.zclassic-c23-anchor-mint/.cookie
  ```
  Then launch the fixed offline producer as:
  ```bash
  systemd-run --user --unit=zclassic23-anchor-mint \
    --property=WorkingDirectory=/home/rhett/github/zclassic23 \
    --setenv=ZCL_MINT_ANCHOR_OUT=/home/rhett/.zclassic-c23-anchor-mint/utxo-anchor.snapshot \
    /home/rhett/github/zclassic23/build/bin/zclassic23 \
    -datadir=/home/rhett/.zclassic-c23-anchor-mint \
    -nolegacyimport -mint-anchor -mint-anchor-fast -fold-inram -nobgvalidation
  ```
  On completion it writes the verified snapshot to
  `$HOME/.zclassic-c23-anchor-mint/utxo-anchor.snapshot`. At 2026-07-01
  14:03:45 UTC the active run reached `-refold-staged: staged reducer reset to
  genesis OK`, `-mint-anchor-fast: OFFLINE FAST-MINT`,
  `[boot] -mint-anchor: offline reducer stages initialized; skipping
  frontend/P2P/runtime services`, and `[mint-anchor] driving the
  genesis..3056758 fold; starting at applied-through=-1`. A negative journal
  scan since the 14:00:18 UTC launch found no `p2p_services_start`,
  peer-connection, RPC/frontend, API-cache, condition-engine, or
  `ZClassic C23 node initialized` lines. At 14:05 UTC it was active at PID
  `3160516`, memory was about 6.4 GB, the fold had emitted
  `applied_height=0`, and no snapshot artifact existed yet.

  **2026-07-03 update:** the resumed producer adopted the legacy interrupted
  fold, advanced to `coins_applied_height=116000`, and was restarted with
  `-fold-inram`; the journal confirmed `ZCL_FOLD_INRAM active` and the 115000
  to 115999 UTXO batch committed. No verified anchor snapshot exists yet, so
  the copy-prove and live cutover sections below remain blocked on the artifact.

  **2026-07-08 update:** the resumed datadir was not a trustworthy resume source.
  `anchorstatus` plus direct SQLite probes showed `coins_applied_height=164001`
  and `coins_ram_flushed_height=164000`, but no `utxo_apply_log[164000]` and no
  `utxo_apply_delta[164000]`. It was moved to
  `$HOME/.zclassic-c23-anchor-mint-torn-20260708-083231`. The current source adds
  a richer `utxo_apply_probe`, refuses RAM flush advancement without reducer
  witness rows, and replays from genesis when a mint/refold marker exists but the
  RAM flush watermark is absent. The fresh producer was recreated from
  `$HOME/.zclassic-c23-COPY-20260701-113424-stall-3166384` and relaunched at
  08:33 UTC with the same `systemd-run` command above. At 08:50 UTC it was
  active as PID `2475996`, memory about 3.7 GB, summary `mint_in_progress`,
  stage cursors at `17000` for header/body-fetch front stages and `16000` for
  UTXO apply/tip-finalize, history diagnosis
  `utxo_apply_history_consistent`, and no snapshot artifact existed yet. Use
  `build/bin/zclassic23-dev anchorstatus` for the current summary logic until
  the release binary is refreshed. `coins_ram_flushed_height=-1` is expected
  before the first RAM flush; avoid casual restarts before the first flush or
  before the boot-reconcile fix is linked into the producer binary.

## Restart posture

`-mint-anchor` no longer blindly resets the fold to genesis on every boot. A
fresh mint writes a checkpoint-bound `mint_anchor_in_progress_v1` blob after the
genesis reset; a restart with the same compiled checkpoint and a sane
`coins_applied_height` resumes from the existing `progress.kv` state, re-arms
the anchor ceiling and optional `-mint-anchor-fast` crypto pass-through, and
continues driving the fold. Older interrupted mints that predate the marker are
adopted only when the durable `refold_in_progress` signal is set and the applied
frontier is within the genesis..anchor span. A different checkpoint marker or a
frontier past the anchor resets instead of inheriting stale state.

The producer still uses a durable disk datadir and does not touch live main,
soak, or `zclassicd`. A wrong resumed state still cannot publish: the terminal
`coins_kv_commitment == compiled checkpoint SHA3` and `count == 1,354,771`
hard-assert remains the trust gate, and a mismatch unlinks the artifact and
FATALs.

## Cutover procedure (run when `$HOME/.zclassic-c23-anchor-mint/utxo-anchor.snapshot` exists)

NEVER live surgery. Copy-prove first, gate on **H\* CLIMB** (not "booted without FATAL" - that is the
`-load-verify-boot` false-green trap: it no-ops on the stamped coins_kv because
`coins_kv_is_proven_authority` returns true, `boot_refold_staged.c:884`).

1. **Confirm the artifact is the real anchor** (the mint already hard-verified it at write; re-confirm):
   - header height == 3,056,758, count == 1,354,771, body SHA3 == compiled `cp->sha3_hash` (`00e95dbd…`).
2. **Stage it** (idempotent; the node re-verifies SHA3 before trusting — a bad copy is ignored):
   ```bash
   ZCL_ANCHOR_SNAPSHOT_SRC=$HOME/.zclassic-c23-anchor-mint/utxo-anchor.snapshot make seed-anchor-snapshot
   # copies to ~/.zclassic-c23/utxo-anchor.snapshot (NOT the mislabeled /tmp default)
   ```
3. **Copy-prove on a COPY of the live datadir** (never the live PID first):
   ```bash
   make repro-on-copy SLUG=soak-refold \
     REPRO_SRC=$HOME/.zclassic-c23-soak \
     REPRO_FULL=1 \
     REPRO_CONNECT=<a_peer> \
     CLIMB_PAST=3056758 \
     ARGS='-refold-from-anchor -nobgvalidation -paramsdir=$$HOME/.zcash-params'
   ```
   - **GATE (H\* CLIMB):** `reducer_frontier_compute_hstar` / `getblockcount` (= H*) must climb
     STRICTLY past 3,056,758 toward the active tip (~3,157,0xx), with
     `coins_kv_get_applied_height == hstar+1` at every step (no rowless span). Confirm
     `zcl_state subsystem=block_index`. The decisive positive proof is
     `test_reducer_forward_progress_gate.c` PART-1 (`ok && found && hstar==N && applied==N+1`).
   - The harness refuses `-refold-from-anchor` unless this is a full copy, the
     climb gate is set, and an anchor snapshot candidate is reachable at
     `$ZCL_MINT_ANCHOR_OUT` or `<src>/utxo-anchor.snapshot`. The boot path still
     does the actual SHA3/count verification before trusting the file, and the
     harness fails unless the copy log proves that verified MINTED snapshot was
     loaded. It also requires observing H\* at/below the gate before crossing it;
     a first observed tip already above the gate is not accepted.
   - **Cross-check** the resulting block hash vs zclassicd (rpc 8232) at the anchor AND at tip.
   - Confirm body contiguity (3,056,758..tip) first. The explicit boot path now calls
     `boot_refold_body_span_contiguous` before reset/stamp, but this manual check keeps the
     copy-prove failure mode obvious before a restart.
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

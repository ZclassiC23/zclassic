# W4-1 cure runbook — 2026-07-16 (pre-staged, zero wall-clock at anchor)

**Purpose:** everything between "the mint producer reaches the anchor" and
"the live wedge is cured, verified, and reversible" pre-staged NOW so that
when `zclassic23-mint.service` hits h=3,056,758 (ETA at authoring time:
~12h, see Precondition Table below), no research is needed — only execution.

**This doc ADDS the step the existing
[`docs/work/sovereign-cutover-runbook.md`](./sovereign-cutover-runbook.md)
does not cover** (that doc was last touched at commit `e9801093c`,
2026-07-15 18:15 UTC — **before** the independent replay receipt landed at
`181b29fe4`/`c865f1195`, 19:47–20:01 UTC the same day). Its install / CAS /
rollback mechanics (§ "The mechanism", § "Owner-gated cutover", § "Rollback")
are still the correct authority and are referenced, not duplicated, below.
What it is missing — and what this doc supplies — is the
`-verify-consensus-bundle=PATH` step that must run **before**
`-install-consensus-bundle=PATH` can do anything but return
`VERIFIED_CONTAINED`.

**Read-only discipline for this lane:** every command below that could
mutate `~/.zclassic-c23-mint`, `~/.zclassic-c23-mint-receipt`, or
`~/.zclassic-c23` (live) is a **future** step for the operator/agent running
the actual cutover, not something this pre-staging pass executed. Verify
every cited flag/command against the checked-out binary before running it —
this file rots like every other doc in this repo.

---

## 0. The one fact that changes the sequencing: where the receipt must live

`consensus_state_replay_receipt_authority_available()`
(`config/src/consensus_state_replay_receipt.c:599`) is read by
`consensus_state_snapshot_install_activate.c:149` using the **install
target's own `datadir_fd`** — i.e. the receipt file
(`consensus_state_replay_receipt.v1`) must physically exist **inside the
exact datadir that `-install-consensus-bundle=PATH` is about to run
against**, and it must have been written by a run of
`-verify-consensus-bundle=PATH` using **the exact same binary image**
(`rr_verifier_binary_digest()` hashes `/proc/self/exe` and the authority
check `memcmp`s it against the receipt — a receipt written by a different
build refuses, silently staying `VERIFIED_CONTAINED`).

Consequently the cure is **not** "run verify once, install everywhere." It
is:

1. Build (or select) **one candidate binary** and use it for every verify +
   install call below — copy-prove AND live. A binary rebuild between the
   copy-prove pass and the live cutover invalidates the copy-prove's
   receipt and forces a fresh verify pass on the live path too.
2. `-verify-consensus-bundle=PATH` must run against a datadir whose *own*
   folded tables (`coins`, `sprout_anchors`, `sapling_anchors`,
   `nullifiers`) independently re-derive the bundle's digests — i.e. a
   datadir that itself folded genesis→anchor. The **only** such datadir is
   the producer's, `~/.zclassic-c23-mint`. Per this repo's protected-datadir
   rule, we never run anything against that datadir directly (even
   read-only `-verify-consensus-bundle` writes a receipt file into it —
   that is a write). Instead: **make one `cp -a` copy of the finished
   producer datadir**, verify against the copy, and treat the copy (bundle
   + receipt together) as the portable "cure kit" for every subsequent step.
3. The receipt this produces is then physically copied (as a file, next to
   the bundle) into every install target: the copy-prove COPY, and later
   the live datadir — always re-run with the same binary as step 1 if the
   datadir doesn't already carry a valid receipt for that exact bundle.

This does not change any node source — `-verify-consensus-bundle` already
takes an explicit `-datadir=` and never assumes it is the producer's own
directory; the copy trick above only avoids writing into a protected path.

---

## 1. Precondition table (read-only checks run 2026-07-16, re-verify before acting)

| # | Item | Status | Evidence |
|---|------|--------|----------|
| 1 | Producer running, honest fold in progress | **READY** | `zclassic23-mint.service` active 9h+, `zclassic23 ops producer status --datadir=$HOME/.zclassic-c23-mint` → `producer=.../.zclassic-c23-mint receipt=open height=1087999 target=3056758 remaining=1968759 rate=45.4blk/s eta=43314s` (~12h at capture time; re-derive fresh — rate is not an SLO). |
| 2 | Corrected checkpoint baked | **READY** | `core/chainparams/src/checkpoints.c:91,100,106`: height=3056758, sha3 `5817f0ec…`, utxo_count=1354769 (matches HANDOFF §0-NEW). |
| 3 | `-verify-consensus-bundle=PATH` flag wired | **READY** | `src/main.c:3381-3384` parses it into `ctx.verify_consensus_bundle`; `config/src/boot.c:3524-3525` calls `boot_verify_consensus_bundle()` before the install branch. Terminal, `_exit()`s. |
| 4 | `-install-consensus-bundle=PATH` flag wired | **READY** | `src/main.c:3377-3380` → `ctx.install_consensus_bundle`; `config/src/boot.c:3526-3528` calls `boot_install_consensus_bundle()`. Containment gate `ZCL_DEPLOY_ALLOW_CANONICAL` confirmed present at `config/src/boot_install_consensus_bundle.c:244`. |
| 5 | Independent replay receipt machinery merged to `main` | **READY** | `config/src/consensus_state_replay_receipt.c` (641 lines), header `config/include/config/consensus_state_replay_receipt.h`; landed `181b29fe4`/`c865f1195` 2026-07-15. `activate_independent_authority_available()` in `consensus_state_snapshot_install_activate.c:126-150` is the sole non-`ZCL_TESTING` authority path. |
| 6 | Copy-prove harness supports the two-phase install mode | **READY** | `tools/repro_on_copy.sh` `--install-consensus-bundle=PATH` (documented lines 76-99, gated at lines 280-296, 484-620); requires `--full` + `--expect-climb-past=H`. **GAP**: this script does **not** know about the replay-receipt file — see §0 above and the new driver script (§4/§7). |
| 7 | Bundle export path/name known | **READY** | `config/src/boot_mint_anchor.c:223-260` (`boot_mint_anchor_export_bundle`): auto-exports on ceremony finalize to `<datadir>/consensus-state-bundle-<anchor>.sqlite` — expect `$HOME/.zclassic-c23-mint/consensus-state-bundle-3056758.sqlite` once the producer finishes. Not present yet (producer mid-fold). Idempotent: a present file is treated as already-exported, never overwritten. |
| 8 | Bundle artifact currently present | **NOT MET (expected)** | `ls $HOME/.zclassic-c23-mint/consensus-state-bundle-3056758.sqlite` → absent; producer is at h≈1,088,000 of 3,056,758. This is the ~12h gate this whole doc pre-stages around. |
| 9 | Old failed producer preserved as evidence, not a source | **READY (do not touch)** | `$HOME/.zclassic-c23-mint-receipt` present, 15G. HANDOFF: binary `34d78c95…`, do NOT restart/relabel/inject. Not used by this runbook except as a negative-control fixture if ever needed. |
| 10 | Disk space for a datadir COPY | **READY** | `df -h /home` → 1.8T total, **690G available**, 61% used. `~/.zclassic-c23` (live, `--full` copy target) = 4.4G. `~/.zclassic-c23-mint` (producer, will need one `cp -a` for the receipt-verify step) = 13G today, will grow to roughly the live datadir's size (single-digit GB range at h=3,056,758) by the time it finishes — nowhere near exhausting 690G even with 2-3 concurrent COPY datadirs live at once. |
| 11 | Revert-layer artifacts (pre-authorized, per `sovereign-cutover-runbook.md` "Rollback") | **NOT YET CREATED (expected)** | `ls $HOME/.zclassic-c23/progress.kv.preinstall.*` → none. This is correct: the file is created *by* the install-activate transaction itself (`consensus_state_snapshot_install_activate.c`, `VACUUM INTO` under the process transaction lock) — it cannot exist before an install has actually run. Nothing to pre-stage here beyond confirming the mechanism is code-present (item 4/5) and the rollback doc's steps are current (they are — unchanged since `e9801093c`). |
| 12 | Live node current H\* / blocker (read-only) | **READY (unchanged from HANDOFF)** | `getblockcount` on `~/.zclassic-c23` (RPC 18232) → `3176325`. `dumpstate blocker` → active permanent blockers `utxo_apply.anchor_backfill_gap` (fire_count 1) and `utxo_apply.nullifier_backfill_gap` (fire_count 1), plus a transient `utxo_apply.apply_failed` retry loop (fire_count 164812, retry budget exhausted at 5 — cosmetic churn, not a different root cause). `dumpstate reducer_frontier` → `hstar=3176325`, `coins_applied_height=3176326`, `served_gap=0`. Matches HANDOFF §1 exactly; live node has not moved. |
| 13 | Build binary present + buildable | **READY** | `build/bin/zclassic23` present (20MB, built 2026-07-16 05:31 same day). `make build-only` was NOT re-run in this read-only pass (no source changes made); re-run it immediately before the actual cutover to get a binary built from the exact `main` HEAD at execution time — that binary becomes "the one binary" of §0 step 1. | <!-- stale-ok: dated build-artifact observation in a dated runbook -->


**Summary: 10/13 ready, 2 expected-not-met (bundle not exported yet; no
preinstall rollback file yet — both correct given current state), 1 item
(#13) is an action for the operator immediately before executing, not a
blocker.**

---

## 2. Phase timeline

```
NOW (producer ~h=1,088,000)
  │
  ├─ Phase A: WAIT — nothing to do but watch. No live-state mutation.
  │
  ▼
Producer reaches h=3,056,758, ceremony finalizes, bundle auto-exports
  │
  ├─ Phase B: BUNDLE EXPORT VERIFICATION (read-only against a COPY)
  │
  ▼
  ├─ Phase C: REPLAY RECEIPT (-verify-consensus-bundle, against the COPY)
  │
  ▼
  ├─ Phase D: COPY-PROVE INSTALL (tools/scripts/cure-copy-prove.sh)
  │            gate: G-SOV (H* CLIMB past 3,176,325 [re-derive live value
  │            fresh], coins_applied==H*+1, provenance markers true,
  │            hash-agree vs zclassicd/mirrors)
  │
  ▼
  ├─ Phase E: OWNER-GATED LIVE CUTOVER (only after Phase D is a clean PASS)
  │            — mechanics: docs/work/sovereign-cutover-runbook.md
  │              "Owner-gated cutover" section, unchanged.
  │
  ▼
  └─ Phase F: THREE-LAYER REVERT (available at every step; see §9)
```

---

## 3. Phase A — while waiting (now)

Nothing to execute. Poll, don't touch:

```bash
zclassic23 ops producer status --datadir="$HOME/.zclassic-c23-mint"
```

Abort/escalate criteria during the wait (none observed at authoring time):
- `receipt=` field changes from `open` to anything else unexpectedly, or
  the eta stops shrinking across two checks 30+ min apart (stall, not
  progress) — diagnose via `journalctl --user -u zclassic23-mint -n 50`
  before touching the unit; **do not restart it** without first reading
  HANDOFF's producer-decision note (a pv-lookahead 3× win may justify a
  planned restart on a newer binary — that is a separate, owner-gated
  decision, not an emergency response to a transient WAL-quiet window; see
  memory `project_anchor_mint_running_and_refold_facts_2026-07-11` for the
  "don't mistake stale mtime for a stall" trap).
- Disk pressure: re-run the `df -h` check in the precondition table if the
  producer datadir growth rate looks unusual.

---

## 4. Phase B — bundle export verification (once the producer finalizes)

**Expected evidence the ceremony finished cleanly** (all from
`config/src/boot_mint_anchor.c`, read-only checks against the now-idle,
one-shot-exited producer datadir — the unit does not restart,
`Restart=no` per HANDOFF):

```bash
# 1. The unit exited (one-shot ceremony, not a crash-loop):
systemctl --user status zclassic23-mint    # expect: inactive (dead), not failed

# 2. The bundle file exists and is non-empty:
ls -la "$HOME/.zclassic-c23-mint/consensus-state-bundle-3056758.sqlite"

# 3. The producer's own status now reports the terminal state (read-only):
zclassic23 ops producer status --datadir="$HOME/.zclassic-c23-mint"
#   expect height=3056758 (== target), remaining=0
```

**Abort criteria:** unit shows `failed` (not `inactive`/`dead`), or the
bundle file is absent/zero-length after the unit exits — this means the
HARD-ASSERT checkpoint match failed (the fold disagreed with the compiled
checkpoint) or the export itself failed
(`mint_bundle.export_failed` typed blocker — check
`zclassic23 dumpstate blocker --datadir=$HOME/.zclassic-c23-mint` before
touching anything). Either way: **stop, do not proceed to Phase C**, and
treat the producer datadir as evidence for root-cause, exactly as the old
failed producer (`~/.zclassic-c23-mint-receipt`) was preserved.

**Make the read-only cure kit** (the one `cp -a` that lets every subsequent
step avoid touching the protected producer datadir — see §0):

```bash
CURE_KIT="$HOME/.zclassic-c23-COPY-$(date +%Y%m%d%H%M%S)-cure-kit-source"
cp -a "$HOME/.zclassic-c23-mint" "$CURE_KIT"
# Verify byte-identical bundle before trusting the copy for the next phase:
cmp "$HOME/.zclassic-c23-mint/consensus-state-bundle-3056758.sqlite" \
    "$CURE_KIT/consensus-state-bundle-3056758.sqlite"
```

`$CURE_KIT` carries the `.zclassic-c23-COPY-` marker so it is structurally
indistinguishable, to every safety check in this codebase, from a normal
copy-prove throwaway — good, that is the property we want (never a "looks
like a live datadir" surface).

---

## 5. Phase C — the independent replay receipt

Build (or confirm) the **one candidate binary** used for the rest of this
runbook:

```bash
cd /home/rhett/github/zclassic23   # main repo, or the branch under cutover
make build-only                    # or make -j$(nproc) for the full link
BIN="$PWD/build/bin/zclassic23"
```

Run the offline verifier **against the cure-kit copy**, never the real
producer datadir:

```bash
"$BIN" -datadir="$CURE_KIT" \
  -verify-consensus-bundle="$CURE_KIT/consensus-state-bundle-3056758.sqlite"
```

**Expected evidence (stderr, exit 0):**
```
VERIFIED: -verify-consensus-bundle: replay-verified height=3056758 utxo=1354769 anchors=<N> nullifiers=<N>; receipt at <CURE_KIT>/consensus_state_replay_receipt.v1
  the independent replay receipt now authorizes -install-consensus-bundle for this exact bundle on this datadir.
```
(`config/src/boot_verify_consensus_bundle.c:43-48`; `utxo_count` must read
exactly `1354769` — anything else means the cure-kit copy's own tables
disagree with the bundle it was exported alongside, which should be
structurally impossible for a same-process export+copy but is exactly the
kind of thing this step exists to catch.)

**Abort criteria:** any `REFUSED: -verify-consensus-bundle: <reason>` line
— stop, do not attempt Phase D with this bundle. Typed reasons come from
`rr_fail()` call sites in `config/src/consensus_state_replay_receipt.c`
(mismatched component digest, incomplete-history bundle, artifact
validation failure, receipt persist/fsync failure). Re-run against a
**fresh** `cp -a` of the producer datadir before assuming the bundle itself
is bad — a corrupted cure-kit copy is cheaper to rule out than a corrupted
producer fold.

**Confirm the receipt file:**
```bash
ls -la "$CURE_KIT/consensus_state_replay_receipt.v1"
```
This file plus `consensus-state-bundle-3056758.sqlite`, both under
`$CURE_KIT`, are the two artifacts every subsequent install call needs
side-by-side. Keep `$CURE_KIT` around (do not delete) until the live
cutover (Phase E) is confirmed durable — it is the one reusable "cure kit"
for however many copy-prove attempts Phase D needs.

---

## 6. Phase D — copy-prove install (mandatory, never skip to live)

Use the new driver, `tools/scripts/cure-copy-prove.sh` (§7 below;
mechanically equivalent to, but receipt-aware where
`tools/repro_on_copy.sh` is not — see §0 gap):

```bash
# Dry run first — prints the exact plan, touches nothing:
tools/scripts/cure-copy-prove.sh --dry-run \
  --bundle="$CURE_KIT/consensus-state-bundle-3056758.sqlite" \
  --receipt="$CURE_KIT/consensus_state_replay_receipt.v1" \
  --src="$HOME/.zclassic-c23" \
  --expect-climb-past=3176325 \
  --deadline=3600

# Then the real run (creates its own throwaway *-COPY-* datadir):
tools/scripts/cure-copy-prove.sh \
  --bundle="$CURE_KIT/consensus-state-bundle-3056758.sqlite" \
  --receipt="$CURE_KIT/consensus_state_replay_receipt.v1" \
  --src="$HOME/.zclassic-c23" \
  --expect-climb-past=3176325 \
  --deadline=3600
```

Re-derive `--expect-climb-past` fresh from live `getblockcount` immediately
before running — 3,176,325 is this doc's authoring-time value and the live
node has not moved per precondition #12, but always re-check.

**Acceptance — all of G-SOV (`docs/work/self-verified-tip-plan.md`), same
5-point bar as `sovereign-cutover-runbook.md` "Acceptance":**
1. `H*` climbs strictly past the live wedge height toward header tip.
2. `coins_applied_height == H* + 1` at every observed sample (no rowless
   span) — the script polls this via `dumpstate reducer_frontier` on the
   COPY's own isolated RPC port.
3. `coins_kv_is_proven_authority()==true` **and**
   `coins_kv_contains_refold_marker()==true` on the COPY after the run.
4. Same-height hash-agree at the wedge height and at the COPY's final
   climbed tip vs `zclassicd` (RPC 8232) or the soak/dev mirrors.
5. No `download_queue_starved` escalation to `EV_OPERATOR_NEEDED`, and no
   finalized row whose upstream verdict was not `ok=1`.

**Abort criteria:** the script prints `FAIL` with the failing G-SOV item
named; do not proceed to Phase E. A `PASS` on H\* climb alone with any of
items 2-5 unresolved is a **false green** — read the script's full evidence
block, not just its final verdict line.

A copy-prove `PASS` is required **once per (binary, bundle, receipt)
triple** — if you rebuild the binary before the live cutover, Phase C and
Phase D must both re-run with the new binary (see §0 step 1).

---

## 7. `tools/scripts/cure-copy-prove.sh` — what it does and does not do

See the script itself for the authoritative contract (`--help`). Summary:

- Refuses to run unless `--copy-dir` (or its auto-generated default) carries
  the `-COPY-` marker and does not match any known live-datadir name —
  same invariant `app/controllers/src/agent_copy_prove_controller.c`'s
  `cp_path_safety_ok()` enforces for the existing RPC-driven harness.
- `--dry-run` prints the exact `cp -a` / node invocations it would run and
  exits 0 without touching disk or launching a process.
- Real mode: (1) `cp -a --src` into the fresh `--copy-dir` (the harness
  refuses if it already exists); (2) copies `--receipt` into
  `--copy-dir/consensus_state_replay_receipt.v1` (the step
  `tools/repro_on_copy.sh` does not know how to do — see §0); (3) runs the
  terminal `-install-consensus-bundle=--bundle` call against the copy with
  `build/bin/zclassic23`, requiring the literal `INSTALLED:` banner and
  exit 0; (4) boots the copy normally on isolated ephemeral ports
  (`-connect=` a dead sink, `-nolegacyimport`, `-nofilesync`, matching
  `tools/repro_on_copy.sh`'s isolation contract); (5) polls
  `getblockcount` + `dumpstate reducer_frontier` on the copy's own RPC
  port until `--expect-climb-past` clears or `--deadline` expires; (6)
  prints a PASS/FAIL verdict with the full G-SOV evidence block; (7) kills
  the copy's node process (SIGTERM then SIGKILL) unconditionally on exit,
  leaves the copy datadir on disk for inspection.
- Does **not** touch `--src` (opened read-only for the `cp -a` source only),
  does **not** set `ZCL_DEPLOY_ALLOW_CANONICAL`, does **not** restart or
  stop any systemd unit, does **not** delete anything under `$HOME` outside
  its own `--copy-dir`.

---

## 8. Phase E — owner-gated live cutover

**Only after Phase D is a clean PASS.** The mechanics are unchanged from
`docs/work/sovereign-cutover-runbook.md` § "Owner-gated cutover" — follow
that section verbatim, with two updates this doc adds:

1. **Before step 3** ("Run the one-shot install directly"), insert a
   **live-datadir receipt step**, run with the exact `$SERVICE_BIN` that
   step 2 confirmed is the running candidate — reuse `$CURE_KIT` (still on
   disk from Phase B/C; do not re-copy the producer datadir a second time
   unless it changed):
   ```bash
   systemctl --user stop zclassic23   # already required by the runbook's step 1
   # If $SERVICE_BIN differs from the binary Phase C used, re-verify against
   # the cure kit with $SERVICE_BIN first (receipts are binary-bound):
   "$SERVICE_BIN" -datadir="$CURE_KIT" \
     -verify-consensus-bundle="$CURE_KIT/consensus-state-bundle-3056758.sqlite"
   # Then copy that receipt onto the LIVE datadir (still stopped):
   cp "$CURE_KIT/consensus_state_replay_receipt.v1" \
      "$HOME/.zclassic-c23/consensus_state_replay_receipt.v1"
   ```
2. **Step 3's install call** then proceeds exactly as documented
   (`ZCL_DEPLOY_ALLOW_CANONICAL=1 "$SERVICE_BIN" -datadir=$HOME/.zclassic-c23
   -install-consensus-bundle=$HOME/.zclassic-c23-mint/consensus-state-bundle-3056758.sqlite`)
   — the authority check now finds the freshly-copied, binary-matched
   receipt in the live datadir and can actually `ADMIT`/install instead of
   returning `VERIFIED_CONTAINED`.

Steps 4-7 (normal boot, ~30-60 min forward refold watch, G-SOV
re-verification against the now-live node, HANDOFF update) are unchanged.

---

## 9. Phase F — the three-layer revert (available at every step)

1. **Pre-install refusal is a no-op by construction.** Any
   `REFUSED: -install-consensus-bundle: …` (containment, CAS non-`ADMIT`,
   bad bundle, missing/foreign receipt) leaves the prior state byte-for-byte
   unchanged — nothing to revert, just diagnose the typed reason
   (`docs/work/sovereign-cutover-runbook.md` step 3).
2. **Post-install rollback via the captured prior generation.** The
   install-activate transaction captures a `VACUUM INTO` snapshot of the
   pre-cutover progress store, named
   `<datadir>/progress.kv.preinstall.<epoch>.<pid>.<sequence>`, **before**
   the cutover write. Full recipe: `sovereign-cutover-runbook.md` §
   "Rollback" — `systemctl --user stop zclassic23`, preserve the
   post-install state for diagnosis, `cp -a` the named preinstall file back
   onto `progress.kv`, drop `-wal`/`-shm`, restart. This is currently
   **not creatable ahead of time** (precondition #11) — it is a product of
   running the actual install, not something to pre-stage.
3. **Evidence-preservation fallback.** If layers 1-2 are somehow both
   unavailable (e.g. the preinstall file itself is lost), the producer
   datadir (`~/.zclassic-c23-mint`, and the `$CURE_KIT` copy made in Phase
   B) remain a complete, independently re-verifiable source: the entire
   cure sequence (Phase C→E) can be re-run from Phase C on a **fresh** copy
   of the live datadir's last-known-good state (the pre-cutover
   `~/.zclassic-c23-mint-receipt`-style evidence-preservation discipline —
   never delete a datadir that might be the only surviving copy of a
   pre-cutover or post-cutover state until the next state is independently
   confirmed durable and correct).

---

## 10. What this pass did NOT do (by design)

- Did not run `-verify-consensus-bundle` or `-install-consensus-bundle`
  against any real datadir (no bundle exists yet to verify against).
- Did not create `$CURE_KIT` (nothing to copy from yet — the producer
  hasn't finalized).
- Did not modify `tools/repro_on_copy.sh`, any node source file, or any
  live/producer datadir.
- Did not run `make deploy` or restart any systemd unit.

Everything above is staged so that when the producer finalizes, execution
starts at Phase B with zero design/investigation latency.

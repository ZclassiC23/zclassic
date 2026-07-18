# install-e2e-proof-2026-07-18 — `-install-consensus-bundle` end-to-end proof (INSTALL lane)

Lane: `wf_install-e2e` (branch `lane/install-e2e-proof`). Scratch datadir:
`/home/rhett/.zclassic-c23-install-e2e`. Bundle under test:
`/home/rhett/.zclassic-c23-mint3/consensus-state-bundle-3056758.sqlite`
(513,867,776 bytes; `utxo_root=5817f0ec…` == compiled checkpoint @ 3,056,758 in
`core/chainparams/src/checkpoints.c:91-105`). Binary under test:
`/home/rhett/.local/lib/zclassic23-mint-candidates/warmfix-e285a79ca48e/zclassic23`.

Status: **PASS (2026-07-18)** — `-install-consensus-bundle` fired
successfully on a minimal hand-built fixture (`INSTALLED` banner, exit 0);
post-install boot serves H\* = served_floor = 3,056,758 and the installed
UTXO set recomputes to the compiled checkpoint root `5817f0ec…` both by the
node's own commitment command and by an independent Python recompute.
Recorded authority: **CHECKPOINT_CONTENT**. No installer defect found; no
gate predicate touched.

---

## 1. The precise acceptance predicate (from code, file:line)

The verb runs TERMINAL inside boot at `config/src/boot.c:3513-3516`
(`boot_install_consensus_bundle`), after the block index + CSR are loaded
(CSR wired at `config/src/boot.c:1929-1936`) but before the reducer stages
start. Every refusal path `_exit(1)` with a named reason
(`config/src/boot_install_consensus_bundle.c:44-54`).

The full predicate, in evaluation order:

### 1.1 Lane containment — `config/src/boot_install_consensus_bundle.c:238-256`
- Datadir opened by descriptor (`icb_datadir_open_classify`, lines 95-140);
  compared by (st_dev, st_ino) against `<pw_dir>/.zclassic-c23` and
  `$HOME/.zclassic-c23`. Canonical lane requires env
  `ZCL_DEPLOY_ALLOW_CANONICAL=1` (lines 247-251); anything else runs as
  `CONSENSUS_STATE_TARGET_LANE_COPY_PROOF`.

### 1.2 Bundle admission — `config/src/boot_install_consensus_bundle.c:258-269`
- `consensus_state_artifact_evidence_open(bundle_path)` recomputes the UTXO
  root/count/supply, verifies every anchor tree→root, and the nullifier digest,
  pinning the exact file descriptor+inode. Manifest copied out (lines 265-269).

### 1.3 Producer source receipt — `config/src/boot_install_consensus_bundle.c:271-287` (`icb_read_source_receipt`, lines 167-229)
- `source_receipt` singleton row: schema parses, producer commit valid,
  `source_clean ∈ {0,1}`, `validation_profile ∈ {FULL=1, CHECKPOINT_FOLD=2}`
  (enum: `lib/storage/include/storage/consensus_state_bundle_codec.h:26-30`).

### 1.4 Boot-order warm — `config/src/boot_install_consensus_bundle.c:297-307`
- `tip_finalize_stage_warm_authority_caches(progress_db, active_chain_tip, …)`
  publishes (a) the runtime authority pair from
  `tip_finalize_stage_resolve_durable_tip` and (b) the computed H* into the
  provable-tip cache — ONLY from durable state
  (`app/jobs/src/tip_finalize_stage_durable.c:106-134`). Empty stores publish
  H*=0 honestly (phantom-anchor guard, `app/jobs/src/reducer_frontier.c:609-622`).

### 1.5 Selected-chain binding — `app/services/src/consensus_state_chain_binding_service.c:286-399`, decision at lines 88-139
H* and the frontier come from `reducer_frontier_compute_hstar`
(`app/jobs/src/reducer_frontier.c:589-…`): H* = MIN over the 6 success-checked
logs (`k_logs`, lines 197-205) of each log's contiguous ok=1+valid-itag prefix
above the trusted anchor; anchor = compiled checkpoint height (3,056,758)
**only when `coins_kv_is_proven_authority`** (phantom-anchor guard,
lines 617-622; the 3 proven-authority rungs are
`lib/storage/src/coins_kv.c:831-862`: applied-height key + migration-complete
byte + non-empty coins table). Rows AT the anchor height are below every
contiguity scan (`log_contiguous_prefix`, lines 306-313 + 324-326:
`WHERE height > anchor AND height < cursor`).

`consensus_state_chain_binding_decide` requires ALL of:
1. Manifest complete+self-bound (lines 63-86): `history_complete=1`,
   `activation_boundary=0`, all source cursors 0,
   `source_fold_cursor=height+1`, profile FULL/CHECKPOINT_FOLD, nonzero
   block_hash/sapling_frontier_root/proof_manifest_digest/source_digest/
   artifact_digest, artifact-digest recompute match.
2. Frontier consistent before/after AND unchanged across two samples
   (lines 96-99) — `chain_frontier_snapshot_consistent`
   (`app/services/src/chain_frontier_snapshot_service.c:247-257`): H*
   published; authority pair known AND durable
   (`tip_finalize_stage_block_hash_at(H*)`,
   `app/jobs/src/tip_finalize_stage_durable.c:156-192`); authority hash ==
   active-chain block hash at H*; ancestry served←indexed←header; chainwork
   monotone; served ≥ `BLOCK_VALID_SCRIPTS`, indexed/header ≥
   `BLOCK_VALID_TREE`; all failure-free.
3. `durable_served_height (H*) >= manifest->height` (lines 100-102).
4. `active_chain_at(chain_active, manifest->height)` hash ==
   `manifest->block_hash` (lines 103-107, captured at lines 209-222).
5. Bundle block durable validation (lines 108-111): in-memory block index has
   `BLOCK_VALID_SCRIPTS`, no `BLOCK_FAILED_*`, AND
   `validate_headers_stage_has_pass_record(height, hash)` = an ok=1 row in
   `validate_headers_log(height, hash)` —
   `app/jobs/src/validate_headers_stage.c:732-756` (hash+height+ok only; the
   row's itag is NOT consulted here).
6. Sapling source: `block_index_get_ancestor(bundle, manifest->sapling_frontier_height)`
   exists with nonzero hash (lines 112-116, 223-229).
7. Sapling source durable validation: `BLOCK_VALID_SCRIPTS`, failure-free,
   pass record at that height+hash (lines 117-120, 230-237).
8. Sapling source header's `hashFinalSaplingRoot` ==
   `manifest->sapling_frontier_root` (lines 121-123).
9. Bundle block's own `hashFinalSaplingRoot` ==
   `manifest->sapling_frontier_root` (lines 126-128).
10. Header tip: `pindex_best_header` height ≥ bundle height, nonzero
    hash+chainwork, `BLOCK_VALID_TREE`, failure-free, descends from bundle;
    bundle descends from sapling source (lines 129-137, 238-252).

### 1.6 Publication CAS — `app/services/src/consensus_state_publication_cas.c:662-743`, decision at lines 263-377
- Lane tag valid; manifest complete/self-bound (incl. `source_clean`,
  lines 123-143).
- Artifact logical digest == manifest artifact_digest (lines 307-314).
- Chain evidence bound to THIS artifact receipt digest + lane (lines 315-325).
- Source receipt self-consistent (lines 148-173): schema **V2**, source_clean,
  all digests nonzero, commit valid, **profile FULL**, fold_cursor = H+1,
  epoch+receipt digests recompute; receipt digest == manifest.source_digest
  (lines 341-350).
- **Manifest AND receipt profile == FULL** (lines 353-360) — a
  checkpoint-fold bundle is non-serving and refuses.
- Frontier binding (lines 361-373): `capture_frontier` (lines 629-651) =
  `compute_hstar` OK AND `tip_finalize_stage_resolve_durable_tip` OK AND
  `durable_h == hstar`; require `hstar >= manifest->height`.
- Decision record persisted to `<datadir>/consensus_state_publication_decision.v1`
  (fdatasync + atomic rename + dir fsync + exact re-read, lines 501-579),
  reloaded and digest-compared by the verb
  (`config/src/boot_install_consensus_bundle.c:349-361`).

### 1.7 CHECKPOINT_CONTENT authority input — `config/src/boot_install_consensus_bundle.c:367-391`
- If `active_chain_at(chain_active, cp->height)` exists with
  `phashBlock == cp->block_hash` and nonzero `hashFinalSaplingRoot`, the verb
  passes `checkpoint_sapling_root = <that header's hashFinalSaplingRoot>` and
  `checkpoint_sapling_root_from_validated_header = true` to activate.

### 1.8 Activate — `config/src/consensus_state_snapshot_install_activate.c:821-1106`
1. Re-admit bundle; reopened artifact receipt digest == CAS-admitted receipt
   (lines 853-868).
2. Caller height/hash assertion (lines 877-884).
3. Durable ADMIT record valid + bound to artifact (lines 886-895, 747-770).
4. Complete genesis-derived history (lines 897-909).
5. **Authority gate** (lines 913-925 →
   `config/src/consensus_state_snapshot_install_checkpoint_authority.c:135-170`):
   RECEIPT (replay receipt on this datadir binding this bundle whole-file
   digest) **or** CHECKPOINT_CONTENT: `manifest.height == cp->height` AND
   `manifest.utxo_count == cp->utxo_count` AND
   `manifest.utxo_root == cp->sha3_hash` (lines 120-124) AND caller-supplied
   validated-header `hashFinalSaplingRoot` ==
   `manifest.sapling_frontier_root` (lines 128-132). Neither →
   `CONSENSUS_INSTALL_VERIFIED_CONTAINED`, nothing written.
6. Prior-generation backup `progress.kv.preinstall.<epoch>.<pid>.<seq>`
   (VACUUM INTO + fsync + quick_check + identity re-stat, lines 309-489).
7. BEGIN IMMEDIATE; data-version/change fence; CAS decision still current
   vs live frontier (lines 962-990).
8. Apply (lines 553-690): clear 9 reducer-derived tables; DELETE coins;
   anchors+nullifiers reset to complete; stream coins/anchors/nullifiers from
   the bundle (count-checked vs manifest); force the 8 stage cursors
   (upstream=H+1, tip_finalize=H); `coins_applied_height=H+1`; set
   migration-complete + self-folded markers; delete trusted-base keys.
9. Destination re-verification (lines 698-726): installed coins recompute to
   `utxo_root`+count; anchor digest/frontier parity; nullifier digest/count
   parity.
10. `tip_finalize_stage_seed_anchor(H, block_hash, trusted=true)` inside the
    cutover txn (lines 1014-1018) — which runs `seed_integrity_gate_check`
    (`app/services/src/seed_integrity_gate.c:235-273`): prev-hash linkage
    walk down from H (every present parent labeled child-1; ≤10000 steps) and,
    only when a `utxo_sha3` stamp exists AT H in node.db, a UTXO-commitment
    recompute match (lines 180-210). Absent stamp → commitment check skipped.
11. Post-install frontier equality (lines 772-819): H* == served == H,
    coins_applied == H+1, durable_tip == H with hash == manifest.block_hash,
    all 8 cursors exact.
12. COMMIT (with outcome-unknown discipline, lines 1030-1058).
13. `INSTALLED:` banner with `ares.hstar`
    (`config/src/boot_install_consensus_bundle.c:413-418`), `_exit(0)`.

**Recorded authority**: `CONSENSUS_STATE_ACTIVATE_AUTHORITY_CHECKPOINT_CONTENT`
("checkpoint_content") — printed in the activate reason string
(`consensus_state_snapshot_install_activate.c:1092-1103`), since no replay
receipt exists on the fixture datadir.

---

## 2. The minimal fixture recipe

Derived from §1: everything the gate reads that a bare `--importblockindex`
datadir does NOT already provide. *(Recipe + stamp script:
`tools/scripts/install_e2e_fixture_stamp.py`.)*

### 2.0 Base: header import + in-memory chain (real chain evidence)
```
BINARY --importblockindex /home/rhett/.zclassic <datadir>/node.db   # argv[1] form mandatory
```
Provides: blocks table with hash/prev linkage, `status` preserving zclassicd's
`BLOCK_VALID_SCRIPTS` (HAVE_DATA/HAVE_UNDO stripped —
`app/controllers/src/snapshot_controller_import.c:197-216`),
`sapling_root` from the header (lines 184-194). Empirically: 3,170,490 headers
in 118 s; block 3,056,758 = `53d65b85…` status=5 sapling_root=`dfa46bc0…`;
block 3,056,742 (sapling source) = `683af60a…` status=5 same sapling_root —
both exactly the bundle manifest's expectations. No side branches at either
height (only 2 dup heights in the whole import, both near tip).

**Empirical correction (attempt 3): the blocks TABLE is a projection only.**
No boot loader reads it into the in-memory chain (`load_block_index_flat`
needs `block_index.bin`; `load_block_index_sqlite` needs ≥1000 rows in
`block_index_cache` — empty on this datadir; the fixture's own
`blocks/index` LevelDB is empty). A `-nolegacyimport` boot comes up
genesis-only ("Block index loaded: 1 entries in 0s"). The in-memory chain
must come from the **legacy header pull** (`config/src/boot.c:2136-2326` —
the same path the two-step recipe uses): it snapshot-opens
`~/.zclassic/blocks/index` (`ldb_snapshot_make`, LOCK-safe read-only),
inserts all 3.17M records, computes nChainWork/nChainTx in one forward
pass, and saves `block_index.bin` for later boots. To keep the fixture
minimal, the sibling **UTXO auto-import is suppressed** by pre-setting
`node_state.leveldb_utxo_migrated=0x01` in node.db (the import's only gate —
`app/services/src/utxo_recovery_restore.c:118-124`).

Boot with the pull enabled then does exactly what the gate needs
(attempt-4 transcript):
- `restore anchor: derived coins-best h=3056758 (coins_kv authority)` —
  from the §2.1/2.3/2.4 stamps via `reducer_frontier_derive_coins_best`
- `coins_best_block 000002979090…855bd653 found in block_index at
  h=3056758 … status=29 … disk_backed=1`
- `csr: tip committed from=3169247 to=3056758 reason=coins_best_restore` —
  active chain clamped to exactly the anchor; `[chain-restore] disk
  ancestry rebuilt active chain: tip_h=3056758 populated=3056759`;
  post-restore integrity check OK
- `csr: header tip committed to=3169247 reason=scan_best_header` —
  pindex_best_header > bundle height, descending from it

(`boot_legacy_link_missing_block_files` hardlinks ≤256 blk*.dat from
~/.zclassic, which is what makes the anchor block `disk_backed=1` and lets
the restore clamp to it rather than "waiting for P2P".)

### 2.1 coins_kv proven authority (progress.kv)
- `progress_meta['coins_applied_height']` = 3056759 (8-byte LE) — the
  production post-anchor value (`activate_apply_in_tx`,
  `consensus_state_snapshot_install_activate.c:666-668`).
- `progress_meta['coins_kv_migration_complete']` = 0x01.
- `coins` table non-empty (1 sentinel row; activate DELETEs all coins before
  streaming the bundle's 1,354,769).
- Disarms the phantom-anchor guard so H* can sit at the compiled anchor
  (`reducer_frontier.c:617-622`); feeds
  `reducer_frontier_derive_coins_best` → boot restores the active chain tip to
  the anchor (`app/jobs/src/reducer_frontier_readers.c:38-118`,
  `app/services/src/utxo_recovery_restore.c:497-503`).

### 2.2 Stage cursors (production anchor-seed layout)
- 7 upstream stages (`header_admit`, `validate_headers`, `body_fetch`,
  `body_persist`, `script_validate`, `proof_validate`, `utxo_apply`) = 3056759
  (next-height convention).
- `tip_finalize` = 3056758 (served-tip convention).
- Every contiguity scan range `(3056758, 3056759)` is empty → each log's
  prefix = anchor → H* = 3056758 (the CAS gate's `durable_h == hstar` then
  holds via §2.3).

### 2.3 tip_finalize anchor row (the durable authority pair)
- `tip_finalize_log(height=3056758, status='anchor', ok=1,
  work_delta_*=0, utxo_size_after=0, reorg_depth=0, finalized_at=now,
  tip_hash=53d65b85…, itag=…)` — exactly the row
  `tip_finalize_stage_seed_anchor` writes in production
  (`app/jobs/src/tip_finalize_anchor.c:284-291`). Backs
  `resolve_durable_tip` (durable_h == H* == 3056758) and the
  convention-aware `block_hash_at(H*)` authority read.

### 2.4 validate_headers pass records (2 rows)
- `(3056758, hash=53d65b85…, ok=1)` and `(3056742, hash=683af60a…, ok=1)` —
  the two rows `validate_headers_stage_has_pass_record` consults (§1.5.5/7).
  Both at/below the anchor → outside every contiguity scan → itags never
  gate-evaluated (still computed correctly with the production algorithm,
  `app/jobs/src/stage_row_itag.c:38-75`).

### 2.5 Boot flags (resource-polite, offline)
`-allow-plaintext-wallet -nobgvalidation -connect=127.0.0.1:39999
-rpcport=19602 -port=19603 -fsport=19604 -httpsport=19605`
(Legacy import left ON so the header pull runs; the UTXO half is suppressed
by the `leveldb_utxo_migrated` flag.)

### What is deliberately NOT in the fixture
- No UTXO set, no anchor/nullifier tables, no reducer log rows above the
  anchor — activate installs the bundle's complete set atomically, and H* is
  the anchor floor itself, not a folded prefix.
- No replay receipt — authority is CHECKPOINT_CONTENT (utxo_root == compiled
  checkpoint + PoW-committed Sapling root from the imported header).

### Empirical cost notes
- The dominant per-attempt cost is the bundle admission
  (`consensus_state_artifact_evidence_open`): 2× whole-file SHA3 over 513 MB
  plus `consensus_state_bundle_validate` — 1.35M coin records, **631,645
  per-anchor incremental-tree deserializations + root recomputations**
  (`config/src/consensus_state_bundle_validate.c:656-707`), 1.5M nullifier
  digest rows. Measured ~40 min per admission pass on this box; the install
  path admits twice (verb + activate re-admission) and re-verifies the
  installed destination once more.
- The unstamped datadir refuses predictably: with no coins_kv proven
  authority the phantom-anchor guard drops the H* floor to 0
  (`reducer_frontier.c:617-622`), so the chain binder refuses
  "bundle height=3056758 exceeds durable H*=0"
  (`consensus_state_chain_binding_service.c:100-102`). (Attempt 2/3
  confirmed the gate is reached only after a full admission pass — do not
  iterate blind: stamp first.)
- SIGTERM does not interrupt the admission loop mid-compute; use SIGKILL on
  the fixture process only, never on live nodes.

---

## 3. Proof transcript *(empirical — filled as runs land)*

### 3.1 Attempt 1 — bare import, no `-allow-plaintext-wallet`
FATAL at wallet creation (boot gate, pre-install): "refusing to create a new
PLAINTEXT wallet …". Not an install-gate verdict.

### 3.2 Attempts 2/3 — bare import / genesis-only chain, no durable image
Boot reaches `utxo_chain_reconcile` in seconds, then spends ~40 min in the
bundle admission (the dominant cost). With no stamps the chain binder would
refuse "bundle height=3056758 exceeds durable H*=0" (phantom-anchor guard,
§2 cost notes). Attempt 2 was lost to an operator error (piped through
`tail`, the task wrapper reaped the reader; SIGPIPE killed the node) — the
datadir was unharmed. Attempt 3 confirmed the genesis-only load and the
admission cost, then was stopped to stamp first.

### 3.3 Attempt 4 — stamped fixture + legacy header pull (THE proof run) — **PASS**
Fixture state before firing:
- node.db: 3,170,490 imported headers; `leveldb_utxo_migrated=0x01`
- progress.kv: stamps per §2.1–2.4 (coins proven authority; 8 cursors at the
  production anchor layout; tip_finalize anchor row @3056758; validate_headers
  pass records @3056758 + @3056742) — plus the six empty success-checked log
  tables (§2 cost notes: `compute_hstar` LOG_FAILs on "no such table", so the
  empty tables must EXIST; added live during the admission window, before the
  verb reached the H* computation)

Command (verbatim, exit 0):
```
ZCL_DEPLOY_ALLOW_CANONICAL=  # NOT set — fixture is a copy-proof lane
/home/rhett/.local/lib/zclassic23-mint-candidates/warmfix-e285a79ca48e/zclassic23 \
  -datadir=/home/rhett/.zclassic-c23-install-e2e \
  -allow-plaintext-wallet -nobgvalidation -connect=127.0.0.1:39999 \
  -rpcport=19602 -port=19603 -fsport=19604 -httpsport=19605 \
  -install-consensus-bundle=/home/rhett/.zclassic-c23-mint3/consensus-state-bundle-3056758.sqlite
```

Boot transcript (key lines):
```
restore anchor: derived coins-best h=3056758 (coins_kv authority)
coins_best_block 000002979090…855bd653 found in block_index at h=3056758 status=29 disk_backed=1
csr: tip committed from=3169247 to=3056758 reason=coins_best_restore
[chain-restore] disk ancestry rebuilt active chain: tip_h=3056758 populated=3056759
csr: header tip committed to=3169247 reason=scan_best_header
```

Install verdict (stderr, verbatim):
```
[tip_finalize] authority publish durable h=3056758 reason=install_verb_warm
[tip_finalize] provable-tip cache warmed h=3056758 reason=install_verb_warm
[consensus_replay_receipt] WARN …: no valid replay receipt in the datadir; ACTIVATE stays contained
[consensus_bundle_activate] INFO …: activated zcl.consensus_state_bundle.v1 h=3056758 coins=1354769 anchors=631645 nullifiers=1495126 H*=3056758 applied=3056759 authority=checkpoint_content; prior generation preserved at /home/rhett/.zclassic-c23-install-e2e/progress.kv.preinstall.1784372250.489464.1
INSTALLED: -install-consensus-bundle: activated zcl.consensus_state_bundle.v1 h=3056758 coins=1354769 anchors=631645 nullifiers=1495126 H*=3056758 applied=3056759 authority=checkpoint_content; prior generation preserved at /home/rhett/.zclassic-c23-install-e2e/progress.kv.preinstall.1784372250.489464.1
  reboot normally; the reducer folds forward from H*=3056758 to tip.
```
(The `consensus_replay_receipt` WARN is the RECEIPT authority check reporting
its absence — expected on a fixture with no replay receipt; the
CHECKPOINT_CONTENT authority then lifted containment, exactly the designed
lattice: `consensus_state_snapshot_install_checkpoint_authority.c:27-28,146-152`.)

Wall time: ~2 h 03 m end-to-end (boot ~2 min incl. header pull; two full
admission passes + stream + destination re-verification for the rest).

Durable artifacts written by the install:
- `consensus_state_publication_decision.v1` (438 B) — decoded:
  `version=1 decision=1 (ADMIT) refusal=0 (NONE) bundle_height=3056758
  bundle_hash=53d65b85… validation_profile=1 (FULL) target_lane=1
  (copy-proof) expected_frontier_height=3056758 expected_frontier_hash=
  53d65b85… reason='all evidence present and mutually binding'`
  (enum values: `app/services/include/services/consensus_state_publication_cas.h:40-43`)
- `progress.kv.preinstall.1784372250.489464.1` (61 kB) — the
  physically-restorable prior generation (VACUUM INTO + fsync + quick_check,
  `consensus_state_snapshot_install_activate.c:309-489`)

### 3.4 Post-install verification (fresh normal boot, no install verb)
`tools/scripts/install_e2e_verify.sh` — booted the installed datadir with the
same offline flags, waited for RPC, then:

- `status`: `hstar=3056758 gap=112489 peer_best=unknown sync=finding_peers
  blocker=review_required_bootstrap_trust … peers=0` — the node serves the
  anchor and names its (offline, trust-review) blocker; nothing silent.
- `dumpstate reducer_frontier` (key fields):
  `hstar=3056758 served_floor=3056758 served_gap=0
  coins_applied_height=3056759 coins_best_height=3056758
  trusted_base_height=3056758 trusted_base_accepted=true`
  — all 8 stage cursors at the anchor layout (upstream 3056759, tip_finalize
  3056758); all 6 success-checked frontiers contiguous at 3056758;
  `hstar_next_blocked=false first_hstar_blocker_found=false` (NO wedge — the
  bundle's complete shielded history means no `anchor_backfill_gap`);
  `rewind_bases=[{kind=compiled_checkpoint, height=3056758,
  commitment_sha3=5817f0ec…, self_derived=true, distance_from_tip=0}]`.
- `core consensus utxo commitment` (the node's own recompute over the
  installed set, 414 ms): `ok=true status=passed sha3_hash=
  5817f0ec66738db6989cf881cf37b2148d07b978fd69e5a334855b4991ac5f85
  height=3056758 utxo_count=1354769`.
- **Independent recompute (Python hashlib, no repo code)** over the installed
  `progress.kv` coins table using the canonical record layout
  (`lib/coins/src/utxo_commitment.c:255-277`):
  `installed coins: 1354769, supply: 1036413794674881
  recomputed utxo_root: 5817f0ec66738db6989cf881cf37b2148d07b978fd69e5a334855b4991ac5f85
  checkpoint root:      5817f0ec66738db6989cf881cf37b2148d07b978fd69e5a334855b4991ac5f85
  MATCH`
  (supply = 10,364,137.94674881 ZCL — exactly the checkpoint comment's
  corrected supply, `core/chainparams/src/checkpoints.c:83`.)
- Post-install progress.kv: cursors + tip_finalize anchor row + proven
  authority keys exactly as activate wrote them; the anchor row at 3056758
  now carries the production `seed_anchor` provenance.

---

## 4. Defects found + fixes

**None in the installer.** The install verb, chain-binding gate, publication
CAS, CHECKPOINT_CONTENT authority, and the atomic cutover all behaved exactly
as written on the first datadir that satisfied the characterized predicate.
No code change was made in this lane; no gate predicate was touched.

Fixture-side findings that are NOT installer defects but shaped the recipe:
1. The blocks TABLE (from `--importblockindex`) is a projection; no boot
   loader reads it into the in-memory chain. The in-memory chain needs the
   legacy header pull (§2.0) — same as the canonical two-step recipe.
2. `reducer_frontier_compute_hstar` prepares against all six `k_logs` tables
   and LOG_FAILs on "no such table" — a hand-built store must CREATE the
   empty tables, not just the rows (fixed in the stamp script, §2).
3. Per-attempt wall time is admission-dominated (~40 min/pass, two passes +
   destination verification ≈ 2 h). The admission loop ignores SIGTERM
   mid-compute (SIGKILL needed; fixture only).

---

## 5. Exact reproduction recipe (condensed)

```bash
B=/home/rhett/.local/lib/zclassic23-mint-candidates/warmfix-e285a79ca48e/zclassic23
D=/home/rhett/.zclassic-c23-install-e2e
BUNDLE=/home/rhett/.zclassic-c23-mint3/consensus-state-bundle-3056758.sqlite

mkdir -p "$D"
$B --importblockindex /home/rhett/.zclassic "$D/node.db"          # ~2 min
python3 -c "import sqlite3; db=sqlite3.connect('$D/node.db'); \
  db.execute(\"INSERT OR REPLACE INTO node_state(key,value) VALUES('leveldb_utxo_migrated',x'01')\"); \
  db.commit()"
python3 tools/scripts/install_e2e_fixture_stamp.py "$D"           # durable image
$B -datadir="$D" -allow-plaintext-wallet -nobgvalidation \
   -connect=127.0.0.1:39999 -rpcport=19602 -port=19603 -fsport=19604 \
   -httpsport=19605 -install-consensus-bundle="$BUNDLE"           # ~2 h, exit 0
tools/scripts/install_e2e_verify.sh "$D" "$B" build/bin/zcl-rpc   # proof reads
```

---

## 6. Next steps

For the owner/cure lane:
1. The gate is proven satisfiable and the cutover is proven atomic on a
   live-shaped datadir. The remaining blocker for the CANONICAL cutover is
   not the installer but building the canonical datadir's own evidence
   (validated chain state at/above the anchor with pass records + proven
   coins authority) — exactly what the (currently Phase-2-failed) fresh-cure
   driver was constructing. This lane's stamp recipe §2.1–2.4 enumerates the
   precise rows its Phase 5 must produce via validated sync.
2. The 2-hour admission cost will also hit the canonical cutover; plan the
   maintenance window accordingly (the live node is down for the whole
   install, per the runbook).
3. If a faster iteration loop is ever needed for install-gate work, the
   admission's 631k per-anchor tree-root verifications are the cost center —
   any optimization belongs to the exporter/admission path, NOT to the gate
   predicates (do not weaken them).


# Act 3 prep — `tip_finalize_stage_seed_anchor` caller classification + Act 4a XOR resolution

> Wave-1 Lane E of [`self-verified-tip-plan.md`](./self-verified-tip-plan.md).
> **Analysis only — no production source mutated.** Verify fresh before any
> deletion; specifics rot.

Scope: classify every PRODUCTION caller of `tip_finalize_stage_seed_anchor`
(KEEP vs DELETE) so the Act-3 deletion of the borrowed-seed path cuts only the
cold-import seeders and never trips a live predicate; and resolve Act-4a — is
the commitment-MMR `xor_accumulator` feed CONSENSUS-CRITICAL or RPC-export-only.

---

## TASK 1 — caller classification

`grep -rn tip_finalize_stage_seed_anchor app/` returns 6 PRODUCTION call sites
(the rest of the grep hits are the prototype in `tip_finalize_stage.h:99`, the
definition in `tip_finalize_anchor.c:155`, comment references in
`reducer_frontier.c:278` / `tip_finalize_stage.c:776,821`, and header doc
comments in `seed_integrity_gate.h:29` / `block_index_loader.h:194`).

The function (`tip_finalize_anchor.c:155`) runs the `seed_integrity_gate`
(crash-only, returns false on a poisoned seed), inserts a `status="anchor"`
`tip_finalize_log` row, and — RAISE-ONLY — declares the durable
`REDUCER_TRUSTED_BASE_HEIGHT_KEY` / `..._HASH_KEY`. With `trusted_seed=true`
the frontier cap is exempted and the cursor is set UNCONDITIONALLY.

| # | file:line | trusted_seed | verdict | why |
|---|-----------|--------------|---------|-----|
| 1 | `app/services/src/reducer_ingest_service.c:447` | `false` (runtime re-seed) | **KEEP** | Live at-tip fold. Re-seeds the finalize cursor ONLY when it is genuinely behind the served tip (`cursor < anchor_tip->nHeight`, after a repair clamp). This is the steady-state forward-progress path — the consensus-critical live writer. Deleting it re-introduces the served-tip-trails-by-one defect (task #30). |
| 2 | `app/services/src/reducer_ingest_service.c:470` | `false` (runtime re-seed) | **KEEP** | Regtest-only on-demand bootstrap (gated `params->fMineBlocksOnDemand && nHeight==0 && cursor==0`). Seeds the genesis anchor row ONCE for a fresh genesis-only node so the first `generate` can finalize. Byte-inert off regtest (the bool is the first AND-term). Needed for the regtest test surface; not a cold-import borrow. |
| 3 | `app/services/src/snapshot_apply.c:91` | `true` (SHA3-verified snapshot) | **KEEP** | Fast-sync snapshot apply. The seed rides a SHA3-verified UTXO snapshot whose anchor hash is consensus-bound — this is the sovereign-compatible fast-sync trust root, NOT the borrowed `zclassicd`-chainstate copy. It stays after the cure (it is the FlyClient + SHA3 path the plan keeps). |
| 4 | `app/services/src/block_index_loader_rebuild.c:745` | `true` | **DELETE** (after the cure ships) | Cold-import seeder. Fires only when the node is wedged (`H - H* > COLD_IMPORT_SEED_TRIGGER_GAP`) on a legacy `--importblockindex $HOME/.zclassic` datadir; stamps `coins_applied_height = H+1` and the anchor at the imported tip `H`. This is exactly the borrowed-foundation path the cure (`-refold-from-anchor` default + delete `utxo_recovery_restore.c:369`) removes. |
| 5 | `app/services/src/reindex_epilogue.c:178` | `true` | **DELETE** (after the cure ships) | Reindex (`-reindex-chainstate`) cold-import seeder. trusted_seed=true is justified as "from-genesis full replay through connect_block", but the seed itself is the same stamp-the-cursor-to-an-imported-tip mechanism. Once the default cold start is the self-derived from-anchor refold, reindex's separate stamping path is redundant with the fold and is part of the carve. **Sequence after Act-3 #2 (flip default) — reindex must route through the from-anchor refold first, else deleting this seed leaves reindex unable to finalize.** |
| 6 | `app/jobs/src/tip_finalize_anchor.c:155` | — | n/a (DEFINITION) | The function itself. Not deleted — KEEP callers (#1–#3) still need it. The carve removes call sites #4/#5 only; the symbol survives. |

### KEEP / DELETE summary

- **KEEP (3):** `reducer_ingest_service.c:447` (live fold), `reducer_ingest_service.c:470` (regtest bootstrap), `snapshot_apply.c:91` (SHA3-verified fast-sync). These are the live / from-anchor / snapshot consensus-compatible paths the plan keeps. Deleting any of them trips a live predicate (forward-progress finalize, regtest finalize, fast-sync finalize respectively).
- **DELETE after the cure (2):** `block_index_loader_rebuild.c:745` (legacy cold-import wedge-heal), `reindex_epilogue.c:178` (reindex cold-import seed). Both are the borrowed/imported-tip stamping path; remove them in dependency order per `archive/architecture-deletion-plan.md`, AFTER Act-3 #2 makes the from-anchor refold the default cold start.
- The plan's prose named 9 callers and 5 in an earlier draft; the live HEAD has **6 production call sites** (2 of them the two `reducer_ingest_service.c` re-seeds), classified above. The plan's headline KEEP/DELETE split holds: KEEP `reducer_ingest_service.c` + `snapshot_apply.c` + the from-anchor refold; DELETE `block_index_loader_rebuild.c` + `reindex_epilogue.c`.

> Note: the plan also names a from-anchor `boot_refold_staged.c` as a KEEP
> caller, but at HEAD `boot_refold_staged.c` does NOT call
> `tip_finalize_stage_seed_anchor` directly (it is not in the grep). The
> from-anchor refold reaches the finalize cursor through the live ingest path
> (#1), not through its own seed_anchor call. KEEP intent is satisfied by #1.

---

## TASK 2 — Act 4a: is the `xor_accumulator`-fed commitment MMR consensus-critical?

### Answer: **RPC-EXPORT-ONLY. It is dead in production — neither built nor read on any live or consensus path.** → **DELETE the XOR path** (do not invest in enforcing a `boundary_root` into it).

### Evidence (every consumer traced)

`rpc_blockchain_maybe_commit` (`blockchain_controller.c:262`) is the ONLY
writer of the `xor_accumulator` into the commitment MMR
(`c.utxo_root = xor_accumulator`, `:285`; `mmr_append_commitment`, `:288`).

1. **No runtime caller of `rpc_blockchain_maybe_commit`.**
   `grep -rn rpc_blockchain_maybe_commit` over `app/ lib/ src/ tools/` returns
   only the prototype (`blockchain_controller.h:51`) and the definition
   (`blockchain_controller.c:262`). The `snapshot_tx_index_maybe_commit`
   hit (`snapshot_controller_txindex.c`) is an unrelated tx-index function.
   → the XOR feed is never invoked in production.

2. **Only appender of the commitment MMR is that same dead function.**
   `grep -rn mmr_append_commitment` (non-test) returns exactly one hit:
   `blockchain_controller.c:288`. So the commitment MMR is never populated at
   runtime.

3. **The commitment MMR root is read ONLY by RPC-export tools.** Consumers of
   `rpc_blockchain_get_commitment_mmr` / `commitment_mmr_root`:
   `blockchain_controller_mmr.c:92,101,134,140,155` — i.e. `getcommitmentmmr`
   and `auditchain`, both pure JSON-export RPCs.

4. **No consensus path reads it.** `grep` for
   `getcommitmentmmr|get_commitment_mmr|commitment_mmr_root|maybe_commit` over
   `lib/validation/`, `lib/chain/src/connect_block.c`, and `app/jobs/` returns
   nothing. connect_block, PoW, the reducer fold, and fast-sync verify never
   consult the commitment MMR.

5. **Fast-sync uses a REAL root, not this one.** Snapshot verification computes
   `fast_sync_compute_utxo_root_db` (a real SHA3 over the UTXO table) into the
   snapshot offer's `utxo_root` (`snapshot_offer.c:207`). The FlyClient-proven
   object is the `mmb_leaf`, which already carries the real persisted
   `boundary_root` (`blockchain_controller.c:189` →
   `coins_kv_boundary_root_get`), validated by `test_keystone_utxo_binding.c`.
   The commitment MMR's XOR `utxo_root` is on neither path.

### Implication

The Act-4a "enforce `boundary_root` vs delete the XOR path" fork resolves to
**delete**. The commitment MMR fed by `xor_accumulator` is forgeable AND
unreferenced — enforcing a real root into a structure nothing reads would be
adding code, not subtracting (against the project's subtraction doctrine). The
real per-height UTXO binding already lives in the MMB leaf (`boundary_root`,
keystone B1, `b2482a6ff`). Act-4a's clean move:

1. Delete `rpc_blockchain_maybe_commit` + the commitment-MMR state
   (`g_commitment_mmr`, init/save/get) — confirmed-dead scaffolding (Act-4c
   class).
2. Drop the `xor_accumulator` parameter plumbing if no other consumer survives
   (verify with a fresh grep at deletion time).
3. Decide whether `getcommitmentmmr` / `auditchain` keep reporting a
   commitment root — if kept, point them at the MMB `boundary_root`-backed MMB
   root instead of the dead XOR MMR; otherwise drop the export.

This must be its own change AFTER Act-3 (the cure) lands, with a fresh
zero-consumer grep at cut time (the snapshot/fast-sync `utxo_root` fields above
are a SEPARATE, live structure — do not delete those).

---

## Cross-checks performed

- `grep -rn tip_finalize_stage_seed_anchor app/` — 6 production call sites enumerated and read in context.
- `grep -rn 'rpc_blockchain_maybe_commit|mmr_append_commitment|get_commitment_mmr|commitment_mmr_root'` over `app/ lib/ src/ tools/` — confirmed RPC-export-only, zero consensus/runtime caller.
- Read `blockchain_controller.c:175-299`, `blockchain_controller_mmr.c:80-160`, `snapshot_apply.c:60-97`, `reducer_ingest_service.c:435-472`, `block_index_loader_rebuild.c:695-764`, `reindex_epilogue.c:150-199`, `tip_finalize_anchor.c:155-239`, `checkpoints.c:72-120`.

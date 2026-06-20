# Copying zclassicd's chainstate correctly — the verified plan

> Built 2026-06-19 from a source-grounded research pass (zclassic/src/leveldb,
> Zcash txdb.cpp, this repo's dbwrapper.c / coins_db.c / utxo_recovery_*, host FS
> facts). Goal: a procedure that yields a provably-correct UTXO set with certainty,
> not by careful copying but by verification against block bodies.

## The core answer

Certainty does NOT come from copying the LevelDB carefully. It comes from
**verifying any candidate UTXO set against a fingerprint computed from our own
immutable block bodies.** The copy can be arbitrarily messy; it is accepted only if
its whole-set SHA3 equals the body-derived SHA3 at a pinned height, else it is
rejected and the set is folded from blocks. This exactly covers the blast radius of
LevelDB copy corruption (which is keyspace-wide, see below).

## Why a live `cp -a` can be wrong (precise, source-verified)

LevelDB chainstate = immutable `.ldb` SST files + an append-only MANIFEST selected by
an atomically-renamed CURRENT + a write-ahead `.log`. SSTs are never modified in
place; compaction writes a new SST then deletes old inputs. Coins are keyed `'c'||txid`
and ordered by txid, NOT by height.

Failure classes of `cp -a` over a running daemon (each: detectable? / blast radius):
1. **Missing referenced SST** (compaction deleted it before cp reached it) — DETECTABLE
   on our writable open (`DBImpl::Recover` returns `Corruption("N missing files")`) /
   keyspace-wide. Loud, safe.
2. **Mismatched SST/MANIFEST versions** (cp captured files from two compaction states) —
   SILENT / keyspace-wide. *This produced the live failure: a coin 77 blocks below the
   anchor silently absent.*
3. **Torn MANIFEST tail** — SILENT (log_reader treats it as clean EOF) / recent-edit.
4. **Torn / mid-file `.log`** — SILENT (`paranoid_checks` defaults false, mid-file
   corruption logged "(ignoring error)") / recent coins. *Nil right now: live `.log` is
   0 bytes — memtable already flushed.*
5. **Torn SST data block** — DETECTABLE as a halt (`verify_checksums` on by default).
6. **Mid-iteration CRC error swallowed** — SILENT / keyspace-wide. *OUR code defect:*
   `node_db_import_service.c:380` discards `db_iter_check_error`'s result.
7. Unreferenced extra SST — SILENT but benign.
8. **Fork-tip image** (copy is internally consistent but `'B'` names a block zclassicd
   later disconnected) — SILENT / **height-bounded to top ~10**. *This is the wrong coin
   at h=3,151,306 (orphan 02663FF1 instead of canonical 7E7894BF).* Not a cp artifact —
   a property of which static image was captured.
9. Stale-but-clean (copy lags tip) — SILENT / height-bounded.

The dangerous ones (2,3,4,6) are SILENT and keyspace-wide, so a "reconcile the top 10
blocks" repair cannot find them. Only a whole-set commitment can.

## What's possible on THIS host

ext4 on raw `/dev/nvme0n1p2` (no reflink/CoW, no LVM), NVMe SSD, ample RAM. zclassicd
is currently down (`.log` 0 bytes → a clean flushed checkpoint exists right now).
- **No** atomic FS snapshot (no CoW/LVM). **No** online quiesce RPC (zclassicd has only
  `stop`, which doctrine forbids). **No** second-process read (exclusive `LOCK`). **No**
  `dumptxoutset` in this build (only `gettxoutsetinfo` aggregate + hash).
- So the only external copy is a raw file copy (racy while running). Certainty must come
  from verification, not copy procedure.

## Do we even need the copy?

**No — not for correctness, only for speed.** We have block bodies to ~3,153,221 and the
one bit-for-bit-verified SHA3 anchor at height 3,056,758 (count 1,354,771, root
`00e95dbd…`). Those two deterministically reconstruct the entire UTXO set with zero
zclassicd dependency. The copy buys only skipping the fold of ~94,654 blocks
(anchor→tip) — seconds vs minutes. It buys nothing for correctness, and it is the sole
source of both live failures (the keyspace-wide silent drop and the fork coin).
**If the from-blocks fold is fast enough on this NVMe box (unmeasured — likely minutes),
delete the copy mechanism entirely.** Measuring that is the decision point.

## The plan (each step + its guarantee)

0. **Snapshot the current static clean chainstate** (zclassicd down, no fd holders,
   `.log` 0 bytes → bit-exact). Use as the fixture; never touch the live datadir.
1. **Fix the silent-accept defect:** `node_db_import_service.c:380` must abort the import
   on any iterator error instead of accepting a truncated prefix. (Converts class 6 from
   silent → hard halt.)
2. **Build the body-derived oracle:** fold our `blk*.dat` from the anchor (3,056,758) to a
   pinned height, SHA3 the whole set via the consensus encoder
   `utxo_sha3_serialize_record` (txid||vout||value||script||height||is_coinbase). Depends
   only on immutable bodies + the one verified anchor.
3. **Replace the acceptance gate:** at `utxo_recovery_restore.c:323/358`, require
   `SHA3(copy) == SHA3(body-oracle)` at one pinned bestblockhash instead of
   `imported_count > 100000`. Mismatch → reject the copy, fall through to step 6.
   (Catches classes 2/3/4/8 — any keyspace-wide drop or fork coin flips the hash.)
4. **Non-circular certification:** copy and oracle are now derived from disjoint inputs
   (daemon LevelDB vs our bodies), both anchored at the one verified checkpoint. Agreement
   can't be faked by self-hashing the imported table.
5. **Top-window finality re-fold:** after a copy passes step 3, re-fold the top ~100
   blocks from our own bodies (covers the ≤10 reorg window where honest forks can differ;
   repairs the h=3,151,306 fork coin directly).
6. **Always-correct fallback / eventual default:** full body fold anchor→tip. No zclassicd
   dependency. Also writes the per-height `utxo_apply_log` rows so `reducer_frontier` H*
   can advance past the current logless gap.
7. **Remove the copy's authority:** demote `cp -a` to a hint that is INERT until it clears
   step 3. Ship only after the falsification tests pass.

## Why this is 100% certain

Block bodies are immutable and fully determine the UTXO set (a pure fold from the one
cryptographically-verified anchor). A 256-bit whole-set SHA3 over that set is a fingerprint
whose mismatch detects a dropped/substituted coin *anywhere* in the txid keyspace — exactly
the blast radius of the corruption. Every coin coins_kv ends up trusting was either folded
from bodies, or cleared a SHA3 equality against a set that was. No step trusts the copy's
bytes, a row count, or a self-hash.

## Falsification tests (must pass before deleting the cp path)

- On the fork-coin fixture, step 3 MUST report a SHA3 mismatch and refuse.
- On the clean image, step 3 MUST match and accept (resolve txid byte-order: is our stored
  txid BLOB the same internal order as zclassicd's uint256 LevelDB key?).
- Body fold genesis→3,056,758 MUST reproduce SHA3 `00e95dbd…` / count 1,354,771.
- A one-byte-corrupted SST MUST hard-halt the import (step 1 wired correctly).
- Full-history replay against the real chain: zero false-rejects across 3.15M blocks
  (h=478544 doctrine) before step 7 ships.
- Confirm `utxo_apply_delta_reorg.c` refuses unwinds deeper than `ZCL_FINALITY_DEPTH=10`
  at `in_ibd=false` (else the top-100 window is insufficient).

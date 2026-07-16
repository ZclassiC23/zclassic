# Fast operational tip via complete historical anchor/nullifier import — design spec (2026-07-16)

> **Status: design only. No node code written in this pass.** Every file:line
> below was read on 2026-07-16 against `main` and will rot — re-verify before
> acting. Copy-prove before any live cutover (this doc's §6). This is an
> **operational** readiness path, not the sovereign cure; it is a new,
> faster *source* for the "Immediate cure" already specified in
> [`SOVEREIGN-NETWORK-ROADMAP.md`](./SOVEREIGN-NETWORK-ROADMAP.md) Phase 1, run
> **in parallel** with — not instead of — the producer/parallel-fold sovereign
> promotion.

## Executive recommendation (TL;DR)

The canonical node is wedged not because its transparent state is wrong but
because it holds **only the current shielded frontier**, never the **complete
historical anchor + nullifier set** that a shielded spend can reference. The
operator's own `zclassicd` chainstate LevelDB **does** hold that complete set
(additive-only, never pruned on the canonical chain), and — critically — every
byte we need is already readable: C23 has a working clean-room reader over that
LevelDB (`chainstate_legacy_reader.c`), the Sapling anchor value is
**wire-compatible** with our own `incremental_tree_deserialize`, and the block
header commits `hashFinalSaplingRoot` so each imported Sapling frontier is
**chain-verifiable**. The fast path is therefore: **add a complete, atomic
historical anchor+nullifier importer from `zclassicd`'s chainstate, flip both
activation cursors to 0, and serve tip operationally on `proven_authority`
(self_folded=false) immediately** — while the from-genesis sovereign fold (the
producer / the just-landed parallel-state-compiler) runs in the background to
later flip `self_folded=true`. Do **both**: import gives operational tip in
minutes with no from-genesis fold; the producer/PSC gives sovereignty later.
The single load-bearing risk is **completeness** — a partial import that flips
the cursor to 0 converts a *safe halt* into a *silent false-reject / potential
fork* — so the importer must be all-or-nothing in one transaction and the
copy-prove gate must assert H\* climbs past 3,176,325 **and** exact same-height
tip-hash parity against `zclassicd` before any live cutover.

---

## 1. Mechanism verification (confirmed against code)

### 1.1 The blocker and what it checks

`utxo_apply_anchor_gap_blocker_refresh(sqlite3 *db)` —
`app/jobs/src/utxo_apply_anchors.c:264-306`. For **both** pools
(`ANCHOR_POOL_SPROUT=0`, `ANCHOR_POOL_SAPLING=1`) it reads
`anchor_kv_activation_cursor(db, pool, &activation, &found)` and sets
`incomplete=true` if the lookup errors, the row is `!found`, **or**
`activation > 0`. The permanent blocker `utxo_apply.anchor_backfill_gap`
(`BLOCKER_PERMANENT`) is raised on `incomplete`, and cleared **only** when
*both* pools have a found row with `activation_cursor == 0`
(`utxo_apply_anchors.c:271-306`).

`anchor_kv_activation_cursor` (`lib/storage/src/anchor_kv.c:176-226`) is a plain
`SELECT activation_cursor FROM anchor_state WHERE pool=?`. Semantics
(`lib/storage/include/storage/anchor_kv.h:16-21`): the cursor records "the first
height for which this store can derive anchors. **Zero means from-genesis
history.** A missing root while `activation_cursor > 0` is INCOMPLETE, never
proof the root was forged."

The nullifier side has a **separate** permanent blocker
`utxo_apply.nullifier_backfill_gap` (`nullifier_kv.h:33-49`) with its own cursor
stored as a `progress_meta` key `nullifier_kv.activation_cursor`
(`nullifier_kv.c:20`, read at `nullifier_kv.c:168-172`). Both blockers must
clear.

The per-block fold enforces the same fact at spend time: `fold_sprout` /
`fold_sapling` (`utxo_apply_anchors.c:117-195`) reject a block with
`shielded_anchor_history_gap` / `*-anchor-frontier-unavailable` whenever a needed
root resolves to `ANCHOR_KV_HISTORY_INCOMPLETE` (missing **and**
`activation_cursor > 0`) instead of `ANCHOR_KV_FOUND` (`anchor_kv_get`,
`anchor_kv.c:255-318`). This is precisely what pins H\* at 3,176,325.

### 1.2 What fills anchor_kv / nullifier_kv today — and what does not

**anchor_kv schema** (`anchor_kv.c:54-92`):
`sprout_anchors(anchor BLOB PK, height INTEGER, tree BLOB)` and
`sapling_anchors(...)` — the value is the **complete incremental frontier**, not
just the root (`anchor_kv.h:6-10`) — plus `anchor_state(pool PK,
activation_cursor)`. Sole per-root writer:
`anchor_kv_add_tree(db, pool, tree, height)` (`anchor_kv.h:75-77`,
`anchor_kv.c:383-444`) — `INSERT OR IGNORE`, idempotent, empty-root no-op.

**nullifier_kv schema** (`nullifier_kv.c:88-112`):
`nullifiers(nf BLOB, pool INTEGER, height INTEGER, PRIMARY KEY(nf,pool))` +
`idx_nullifiers_height`. Sole writer:
`nullifier_kv_add(db, nf, pool, height)` (`nullifier_kv.h:153-154`,
`nullifier_kv.c:525-556`) — `INSERT OR REPLACE`. Lookup
`nullifier_kv_get(...)` fails **closed** on any bind error
(`nullifier_kv.c:485-523`).

**Fillers today** (both pools):
1. The **incremental per-block fold** (`fold_sprout`/`fold_sapling` via
   `anchor_kv_add_tree`, `utxo_apply_anchors.c:140,188`) — appends the *next*
   frontier after the current latest row only. Never backfills history.
2. The **one-row-per-pool boot seed** from a v3 snapshot's *current* frontier
   (`config/src/boot_shielded_seed.c:139-206`).

**Confirmed: no path imports the complete historical anchor set (or complete
nullifier history) below the reducer cursor.** `seed_shielded_from_snapshot`
(`boot_shielded_seed.c:139-206`) seeds exactly one Sapling frontier row
(header-root-verified via `anchor_kv_seed_frontier_row`, `boot_shielded_seed.c:162`),
one Sprout frontier row (SHA3-trust only), and whatever nullifier list the v3
snapshot carried — then calls `reset_shielded_history_incomplete_in_tx`
(`boot_shielded_seed.c:150`) which sets **both** cursors to a positive `seed_h`.
That positive cursor is exactly the wedge. The `coins_kv_seed_from_node_db` path
(`lib/storage/src/coins_kv_boot_rebuild.c:123-185`) copies **transparent coins
only** (`INSERT ... SELECT ... FROM coinssrc.utxos`) — no shielded data.
`snapshot_shielded.c` serializes **frontier-only** anchors + whatever nullifier
rows the source DB happens to hold (`snapshot_shielded.c:73-205`) — it neither
knows nor proves completeness.

**Cursor-flip primitive already exists:**
`anchor_kv_publish_full_replay_complete_in_tx` flips `activation_cursor` to 0
**without clearing rows** (`anchor_kv.c:510-578`) — the exact operation the
importer needs after a complete bulk insert. `anchor_kv_initialize_history` /
`anchor_kv_reset_in_tx` manage the cursor and table lifecycle
(`anchor_kv.c:94-`).

---

## 2. The `zclassicd` chainstate serialization (confirmed against C++)

`txdb.cpp`/`txdb.h` are byte-identical between `/home/rhett/zclassic-cpp/src`
and `/home/rhett/dev/zclassic/src`. Chainstate LevelDB key prefixes
(`/home/rhett/zclassic-cpp/src/txdb.cpp:22-36`):

| Prefix | Const | Record |
|---|---|---|
| `'A'` | `DB_SPROUT_ANCHOR` | Sprout anchor: key `'A'‖root(32)`, value = serialized `SproutMerkleTree` |
| `'Z'` | `DB_SAPLING_ANCHOR` | Sapling anchor: key `'Z'‖root(32)`, value = serialized `SaplingMerkleTree` |
| `'s'` | `DB_NULLIFIER` | Sprout nullifier: key `'s'‖nf(32)`, value = `bool true` (1 byte; presence) |
| `'S'` | `DB_SAPLING_NULLIFIER` | Sapling nullifier: key `'S'‖nf(32)`, value = `bool true` |
| `'c'` | `DB_COINS` | transparent UTXO (context) |
| `'a'` / `'z'` | `DB_BEST_SPROUT_ANCHOR` / `DB_BEST_SAPLING_ANCHOR` | bare-prefix key → current root pointer (`uint256`) |
| `'B'` | `DB_BEST_BLOCK` | tip block hash |

Key bytes on disk = `1-byte prefix ‖ 32-byte hash` via `CDataStream` of
`std::pair<char,uint256>` (`dbwrapper.h:179-229`). **Anchor value = the full
`IncrementalMerkleTree`** (left optional-hash, right optional-hash, parents
vector of optional-hashes — `IncrementalMerkleTree.hpp:115-119`), keyed by root
hash, not by height. **Nullifier value = single byte `true`** (presence; the key
being present *is* the spent marker — `BatchWriteNullifiers`, `txdb.cpp:121-134`).

**Completeness — confirmed.** Every connected block calls
`view.PushAnchor(sprout_tree)` / `PushAnchor(sapling_tree)`
(`main.cpp:2675-2676`); `AbstractPushAnchor` inserts a new `entered=true, DIRTY`
entry unless the root is unchanged (`coins.cpp:191-223`), which flushes to a
**permanent `batch.Write`** (`txdb.cpp:141-147`). Anchors are erased **only** on
reorg disconnect (`PopAnchor`, `main.cpp:2356/2364/2366` → `coins.cpp:263-293` →
`batch.Erase`), i.e. only for blocks that leave the active chain. Under linear
forward sync no anchor is ever erased and lookups have **no age/window bound**
(`GetSaplingAnchorAt`, `txdb.cpp:59-69`; `HaveShieldedRequirements`,
`coins.cpp:565-607`). **Therefore `zclassicd`'s tip chainstate contains every
historical Sprout+Sapling root a new block could spend against.** No LevelDB
obfuscation key is present in this fork (`dbwrapper.h` serializes raw) — though
C23's own reader tolerates the XOR-obfuscation case anyway (see §4).

---

## 3. The trust question — does the header commit the Sapling root? **YES for Sapling, NO for Sprout.**

The MEMORY note "ZClassic headers do NOT commit the frontier" is **imprecise and
must be corrected**: ZClassic headers *do* commit the final **Sapling** root.

- **Header field, both codebases:** the 3rd 256-bit field is
  **`hashFinalSaplingRoot`**, not `hashReserved` / `hashLightClientRoot`.
  C++: `CBlockHeader` at `/home/rhett/zclassic-cpp/src/primitives/block.h:26-33`.
  C23: `struct block_header` at
  `lib/primitives/include/primitives/block.h:20-30` (field at line 24), mirrored
  in `struct block_index` (`lib/chain/include/chain/chain.h:89`) and the on-disk
  `block_index_db` record (`lib/storage/include/storage/block_index_db.h:38`).
- **Heartwood never activated on ZClassic.** The upgrade enum tops out at
  `UPGRADE_BUTTERCUP` in both trees (`consensus/params.h:23-33`; C23
  `core/chainparams/src/chainparams.c:134-138`) — no
  Heartwood/Blossom/Canopy/NU5. So the slot was **never repurposed**; Sapling is
  active at height 476969 (`chainparams.cpp:107-110`;
  `core/chainparams/src/chainparams.c:129-132`). The `hashFinalSaplingRoot`
  commitment is real and current.
- **`zclassicd` enforces it unconditionally** in `ConnectBlock`
  (`main.cpp:2682-2688`, `bad-sapling-root-in-block`). **C23 by default only
  rejects an all-zero root** (`connect_block.c:706-715`); full-equality
  (`sapling_root_matches`, `connect_block.c:725-734`) is behind
  `-enforce-sapling-root` (**default off**, pending a 0-false-reject
  full-history replay). C23 already computes and compares this root in
  `sapling_tree_rebuild()`
  (`app/controllers/src/sync_controller_sapling_tree.c:225-293`).

**Load-bearing conclusion:** an imported **Sapling** frontier's root is
**byte-verifiable against `block_index.hashFinalSaplingRoot`** at any height —
a strong, chain-anchored integrity bind we should assert on import (the reader
already does this per-anchor, §4). The **Sprout** frontier has **no header
commitment**; its trust bottoms at the `zclassicd` chainstate / SHA3, exactly as
`boot_shielded_seed.c:169` notes. Sprout is a pre-Sapling, effectively-drained
pool, so this asymmetry is acceptable operationally but must be stated in the
provenance record (it is why this path is `release_assisted`, not `sovereign`).

---

## 4. THE SPEC — complete, atomic historical anchor + nullifier importer

### 4.1 Foundation that already exists (reuse, do not rebuild)

`lib/storage/src/chainstate_legacy_reader.c` is a clean-room C23 reader over the
`zclassicd` chainstate LevelDB (`lib/storage/src/dbwrapper.c` +
`vendor/lib/libleveldb.a`). It already provides:

- `chainstate_legacy_open(path, &handle)` / `chainstate_legacy_close(handle)`
  (`chainstate_legacy_reader.h:54,57`).
- `chainstate_legacy_get_best_block(handle, &uint256)` — reads `'B'`
  (`reader.c:190-210`).
- `chainstate_legacy_get_sapling_anchor(handle, root, tree_out)` — reads
  `'Z'‖root`, `sapling_tree_init` + `incremental_tree_deserialize` (**value is
  wire-compatible**, `reader.c:249-258`), then **fail-closes unless the
  deserialized tree's own root re-hashes to the key** (`reader.c:260-271`).
  Returns `CHAINSTATE_ANCHOR_{FOUND,MISSING,ERROR}`
  (`chainstate_legacy_reader.h:84-99`).
- `chainstate_legacy_iter(handle, cb, ctx)` — seeks the `'c'` keyspace and
  streams decoded transparent coins (`reader.c:274-338`).
- The LevelDB iterator API: `db_iter_init/seek/valid/next/key/value/check_error`
  and `db_read` (`lib/storage/include/storage/dbwrapper.h:57-86`); `db_read` /
  `db_iter_value` already undo the obfuscation XOR if present
  (`dbwrapper.h:28`).
- The wire-compatible tree codec `incremental_tree_serialize/deserialize`
  (`lib/sapling/include/sapling/incremental_merkle_tree.h:93-97` — explicitly
  "wire-compatible with C++ boost::optional encoding"), `incremental_tree_root`,
  `sprout_tree_init` (depth 29) / `sapling_tree_init` (depth 32).
- The point-in-time LevelDB copy guard `utxo_recovery_ldb_copy.c` (FNV signature
  before/after copy, retry until stable) — reuse to obtain a torn-free chainstate
  image before iterating.

So the missing surface is small: **bulk iteration of the `'Z'`, `'A'`, `'S'`,
`'s'` keyspaces**, a **root→height map**, and an **atomic writer** into
anchor_kv + nullifier_kv followed by the cursor flip.

### 4.2 New reader entry points — add to `chainstate_legacy_reader.c/.h`

```c
/* Iterate every Sapling anchor ('Z'‖root) in the chainstate. For each, the
 * callback receives the root and the deserialized+root-verified tree. Skips
 * (does not fail) a row whose tree fails to re-hash to its key, but records the
 * skip count so the importer can refuse on any anomaly. Returns count or -1. */
typedef bool (*legacy_anchor_cb)(const struct uint256 *root,
                                 const struct incremental_merkle_tree *tree,
                                 void *ctx);
int64_t chainstate_legacy_iter_sapling_anchors(void *handle, legacy_anchor_cb, void *ctx);
int64_t chainstate_legacy_iter_sprout_anchors (void *handle, legacy_anchor_cb, void *ctx); /* 'A', sprout_tree_init */

/* Iterate every nullifier in a pool ('S' Sapling / 's' Sprout). Callback gets
 * the 32-byte nf. Value is presence-only; height is not stored in chainstate. */
typedef bool (*legacy_nullifier_cb)(const uint8_t nf[32], void *ctx);
int64_t chainstate_legacy_iter_sapling_nullifiers(void *handle, legacy_nullifier_cb, void *ctx);
int64_t chainstate_legacy_iter_sprout_nullifiers (void *handle, legacy_nullifier_cb, void *ctx);

/* Current-frontier pointers, for the tip-frontier bind (§4.4). */
bool chainstate_legacy_get_best_sapling_anchor(void *handle, struct uint256 *root_out); /* 'z' */
bool chainstate_legacy_get_best_sprout_anchor (void *handle, struct uint256 *root_out); /* 'a' */
```

Each iterator is a copy of the existing `chainstate_legacy_iter` loop
(`reader.c:274-338`) with the seek prefix changed and the per-record decode
swapped for `incremental_tree_deserialize` (anchors) or a bare 32-byte key copy
(nullifiers). For anchors, apply the **same fail-closed root re-hash check** the
point lookup already does (`reader.c:260-271`) — a tree that does not hash to its
key is a torn/corrupt record and must abort the whole import (see §7).

### 4.3 Root→height map (anchor `height` column)

anchor_kv rows need a `height`. `zclassicd` anchors are keyed by root, not
height, so build the reverse map from **our own** block index (already
`hashFinalSaplingRoot`-per-height):

- **Sapling:** scan `block_index` from Sapling activation (476969) to tip; build
  `map[hashFinalSaplingRoot] = height` (first height a root appears). For each
  imported `'Z'` anchor, look up its height; the tip/best anchor's root **must**
  equal `block_index[tip].hashFinalSaplingRoot` (chain bind, §3). A `'Z'` root
  absent from the map is an anomaly → abort.
- **Sprout:** no header commitment. Sprout roots cannot be height-verified. Set
  each Sprout anchor's `height` to a monotone import order **or** the Sprout
  activation height as a metadata sentinel; the `height` column is used only for
  `ORDER BY height DESC` in `anchor_kv_latest_tree` and for delete-range/reorg
  bookkeeping. Ensure the **best Sprout frontier** (`'a'` pointer) is written
  with the **highest** height so `anchor_kv_latest_tree` returns it for appends.
  (Optionally derive exact Sprout heights by a one-pass JoinSplit-anchor scan of
  block bodies — deferred; not consensus-load-bearing for a presence set below a
  checkpointed seed height.)

### 4.4 The importer service — one new file `app/services/src/shielded_history_import_service.c`

Signature (owner-gated, mirrors the existing recovery services):

```c
/* Import the COMPLETE historical Sprout+Sapling anchor and nullifier sets from a
 * point-in-time copy of a zclassicd chainstate LevelDB into anchor_kv +
 * nullifier_kv, atomically, then flip both activation cursors to 0. All-or-
 * nothing: on ANY anomaly (torn record, root re-hash mismatch, tip-frontier
 * root != block_index tip hash, count below a sanity floor, write error) it
 * rolls back and writes NOTHING, leaving the safe wedge in place. */
bool shielded_history_import_from_chainstate(sqlite3 *progress_db,
                                             const char *chainstate_src_path,
                                             const struct main_state *state,
                                             struct shielded_import_report *out);
```

Algorithm:

1. **Point-in-time copy** the chainstate via `utxo_recovery_ldb_copy.c` (FNV
   signature stable window). Never iterate the live `~/.zclassic` chainstate.
2. **Open** the copy (`chainstate_legacy_open`). Read `'B'` best block and assert
   it matches (or is ≥) our reducer cursor height / a known validated header —
   refuse if the chainstate tip is below H\* (we would import an incomplete set).
3. **Build** the Sapling root→height map from `block_index` (§4.3).
4. `BEGIN IMMEDIATE` on `progress_db`. Everything below is in **one**
   transaction.
5. **Sapling anchors:** `chainstate_legacy_iter_sapling_anchors`; per row
   `anchor_kv_add_tree(db, ANCHOR_POOL_SAPLING, tree, height)`. Assert the best
   Sapling anchor (`'z'`) is present and its root ==
   `block_index[tip].hashFinalSaplingRoot`.
6. **Sprout anchors:** `chainstate_legacy_iter_sprout_anchors` →
   `anchor_kv_add_tree(..., ANCHOR_POOL_SPROUT, ...)`; best Sprout (`'a'`) present
   with the max height.
7. **Sapling + Sprout nullifiers:** `chainstate_legacy_iter_*_nullifiers` →
   `nullifier_kv_add(db, nf, pool, height_sentinel)`.
8. **Sanity floors:** counts must exceed conservative minimums (non-empty pools;
   the tip-anchor bind of step 5 is the strongest single check). Any anomaly →
   `ROLLBACK`, return false, blocker stays.
9. **Flip cursors to 0** *inside the same transaction*, after all inserts:
   `anchor_kv_publish_full_replay_complete_in_tx` for both pools (already flips
   `activation_cursor`→0 without clearing rows, `anchor_kv.c:510-578`), and the
   nullifier equivalent (add a `nullifier_kv_publish_full_replay_complete_in_tx`
   that sets `NF_ACTIVATION_KEY`→0 — one-liner mirroring the anchor primitive).
10. **Provenance:** stamp a `shielded_import.provenance` row (source =
    `zclassicd chainstate`, best-block hash, per-pool counts, tip-anchor bind
    result, `self_folded=false`) — see §5.
11. `COMMIT`. On the next reducer tick,
    `utxo_apply_anchor_gap_blocker_refresh` sees both cursors == 0 and **clears**
    the blocker; the nullifier gap blocker clears likewise; the reducer resumes
    folding from 3,176,326.

**Wiring / entry point (landed):** the merged contract is a boot flag only —
`-import-complete-shielded=ZCLASSICD-DATADIR` (`src/main.c
import_complete_shielded_mode`, argv-dispatched next to `--importblockindex`),
not a native command; no `ops.recover.import-shielded-history` command was
added. It is **contained** exactly like the other recovery apply paths:
`import_shielded_is_live_datadir()` refuses `~/.zclassic-c23` and
`~/.zclassic-c23-mint` by construction, so it runs only against an operator
`-datadir=<COPY>` (the copy-prove harness flow, §6) — never the live
canonical/mint datadirs. On success it `printf`s the line
`IMPORT COMPLETE (committed=1): sapling_anchors=... sprout_anchors=...
sapling_nf=... sprout_nf=...` (matched verbatim by
`tools/scripts/import-copy-prove.sh`'s `^IMPORT COMPLETE (committed=` grep)
and exits 0; the same fact is logged via `LOG_INFO(SHI_SUBSYS, "IMPORT
COMPLETE: ...")` in `shielded_history_import_service.c`.

### 4.5 Files touched (summary)

| File | Change |
|---|---|
| `lib/storage/src/chainstate_legacy_reader.c` + `.h` | + 6 iterator/pointer entry points (§4.2) |
| `lib/storage/src/nullifier_kv.c` + `.h` | + `nullifier_kv_publish_full_replay_complete_in_tx` (cursor→0, mirror of anchor primitive) |
| `app/services/src/shielded_history_import_service.c` (new) + internal header | the importer (§4.4) |
| `src/main.c` | owner-gated `-import-complete-shielded=<zclassicd-datadir>` boot-flag mode (`import_complete_shielded_mode`); no native command |
| `app/controllers/src/diagnostics_registry.c` | register `shielded_import` dumper (§5) |
| `lib/test/src/test_*` | importer unit test on a synthetic chainstate fixture; copy-prove harness hook (§6) |

No consensus code changes. No new opcode, no wire-format change, no activation
change — the import only **fills** the two histories the reducer already
requires and already knows how to read.

---

## 5. The decoupling — operational tip now, sovereign promotion later

### 5.1 The split already exists in the data model

The operational/sovereign distinction is **already** two separate bits in
`coins_kv` — this is the load-bearing insight that makes "serve tip now" legal
without claiming sovereignty:

- **`coins_kv_is_proven_authority(db, &applied)`** (`coins_kv.c:760-791`,
  `coins_kv.h:310`) — true iff `coins_applied_height` present **and**
  `COINS_KV_MIGRATION_COMPLETE_KEY==1` **and** `coins_kv_count>0`. It is
  **true for borrowed/seeded state too** — the header says so explicitly
  (`coins_kv.h:315-318`: the borrowed `zclassicd`-chainstate copy also stamps
  the migration key). This is the **operational** "the store is populated and
  self-consistent, can serve" signal.
- **`coins_kv_contains_refold_marker(db)`** (`coins_kv.c:839-853`) reading
  `COINS_KV_SELF_FOLDED_KEY` (`coins_kv.h:332`) — the **sovereign** bit: "the coin
  set is self-derived" (checkpoint-root-verified or from-genesis bodies-only
  refold). Set by `coins_kv_mark_self_folded(db)` (`coins_kv.c:795-817`),
  cleared by `coins_kv_clear_self_folded` (folded into
  `coins_kv_reset_for_reseed`).
- **Composite** `coins_kv_tip_is_self_derived(db, hstar, reason, cap)`
  (`coins_kv.c:855-909`) — G-SOV parts 2 (`coins_applied==hstar+1`) and 3
  (`!proven_authority OR (proven_authority AND self_folded)`); part 1 (H\*
  actually climbed) is the copy-prove harness's job.

**Gap to note (verify before relying on it):** `coins_kv_mark_self_folded` was
found called **only from test code** (`coins_kv.h:325-328` claims the
`-refold-from-anchor` path SETs it, but no `config/src` production call site was
located this pass). So today the sovereign stamp is effectively produced only by
the producer's bundle/receipt path (which asserts self_folded on the *source*
before install — `consensus_state_snapshot_candidate_validate.c:594-599`), not by
a live refold. This is fine for the operational path (which does **not** set the
bit) but the sovereign-promotion path (§5.4) must make sure the self_folded stamp
is actually wired where it claims to be.

### 5.2 After the import: operational tip on proven_authority, self_folded=false

The importer (§4) fills the two shielded histories and flips both activation
cursors to 0 — it does **not** touch `COINS_KV_SELF_FOLDED_KEY`. Result:

- `coins_kv_is_proven_authority` stays **true** (transparent set already
  migrated; the import doesn't disturb it) → the node **serves tip
  operationally**: block explorer, REST/onion API, P2P relay, wallet *viewing*,
  reducer forward-fold from 3,176,326 to network tip.
- `coins_kv_contains_refold_marker` stays **false** → the node is honestly in a
  **`release_assisted`** trust state (borrowed shielded history from the
  operator's own `zclassicd`, not self-derived).

### 5.3 What is gated on sovereign (self_folded) vs available operationally

From the actual call sites (verified this pass), **self_folded gates
publication/serving of state to *other* nodes**, and nothing else today:

- **Gated on self_folded (must wait for sovereign):**
  - Consensus-state **bundle export / snapshot re-serving** —
    `bundle_exporter.c:206-215` (`bx_qualified` = proven AND refold marker;
    refuses "coins lacks self-folded refold marker").
  - **Durable consensus-state export proof** —
    `consensus_state_snapshot_export_proof.c:471-474`
    (`CONSENSUS_EXPORT_MISSING_PROOF`).
  - **Snapshot-offer advertisement** — `boot_snapshot_offer.c:213` via
    `coins_kv_tip_is_self_derived`.
  - **Bundle-candidate admission** —
    `consensus_state_snapshot_candidate_validate.c:594-599`.
- **Available operationally (proven_authority is enough):** canonical UTXO
  count (`chain_state_service.c:92`), H\* / reducer-frontier readers
  (`reducer_frontier.c:546`, `reducer_frontier_readers.c:61`), torn-anchor heal
  (`utxo_recovery_torn_anchor.c:152`), snapshot-install resume
  (`boot_snapshot_install.c:198`), and the whole forward-fold / serve path.
- **Not found gated on this bit at all (flag):** **mining and wallet
  spending**. The vision/roadmap says these should be off in assisted modes, but
  no `coins_kv_is_proven_authority`/`self_folded` call site ties to mint or
  wallet-spend authorization. **Action:** to honor
  [`SOVEREIGN-NETWORK-ROADMAP.md`](./SOVEREIGN-NETWORK-ROADMAP.md) Phase 1
  ("default mining, snapshot re-serving, and wallet spending off until background
  full-history verification promotes"), add explicit `coins_kv_tip_is_self_derived`
  guards at the mint entry and the `z_sendmany`/spend entry, refusing (with a
  clear reason) while `self_folded==false`. This is a small, separate,
  **safety-tightening** change that should land **with** this path so the
  operational mode cannot mine or spend on borrowed shielded history.

### 5.4 Sovereign promotion in the background

While the node serves tip operationally, promote to sovereign by the existing
program, unchanged:

- The **producer** (`zclassic23-mint.service`, from-genesis full-validation fold
  with the in-RAM cure) reaches the anchor, exports
  `zcl.consensus_state_bundle.v1`, and — via
  `-verify-consensus-bundle` → replay receipt →
  `-install-consensus-bundle` — installs a **self_folded=true** state that
  atomically supersedes the borrowed shielded history. The import path and the
  producer path write the *same* tables; the install is a full atomic replace,
  so there is no partial-merge hazard between them.
- The **parallel-state-compiler** (`app/jobs/src/psc_*.c`, landed `8838e0e66`
  today) is **not** the promotion mechanism yet — it is a **transparent-only,
  in-RAM SHA3 differential parity oracle** (`psc_range_fold.h:19-23`) with **no
  production caller**, no `coins_kv` write, and no `self_folded` stamp. Its role
  here is to make the eventual sovereign fold *fast* (multi-core, order-
  independent range fold, differentially gated against the serial fold) so
  `T_sovereign` shrinks. It does not shorten `T_ready` on this path.

Promotion is a **flip of one bit** (`coins_kv_mark_self_folded`, once wired)
backed by a self-derived fold — never a merge. The operational import can be
discarded wholesale by the sovereign install; they never interleave writes.

### 5.5 Make the mode dumpstate-visible

Add a `sovereignty` (or `trust_mode`) dumpstate subsystem so the operational-vs-
sovereign state is a first-class, machine-readable fact (per CLAUDE.md "Adding
state introspection"):

1. Implement `bool sovereignty_dump_state_json(struct json_value *out, const
   char *key)` — lift the already-assembled logic from
   `network_snapshot_loader_contract.c:105-172`
   (`network_push_snapshot_loader_status`): emit
   `coins_kv_proven_authority`, `self_folded_marker`,
   `coins_applied_height`, `hstar`, `self_derived_tip_static_checks`,
   `self_derived_reason`, and the plain-English `authority_posture` string
   (`unknown_no_progress_store` / `not_proven` / `self_folded_marker_present` /
   `proven_but_not_self_folded`). Add a `trust_mode` field:
   `sovereign` | `release_assisted` | `bare`. After the import the expected
   value is **`release_assisted`** with `authority_posture ==
   proven_but_not_self_folded`.
2. Register one row in
   `app/controllers/include/controllers/diagnostics_dumpers.def`
   (`DIAG_LOCAL("sovereignty", sovereignty_dump_state_json, "operational vs
   sovereign trust mode")`). Prior art: `bundle_exporter_dump_state_json`
   already surfaces a `"qualified"` bool from exactly these two predicates
   (`config/src/bundle_exporter.c:624-654`, registered
   `diagnostics_dumpers.def:353-356`). No RPC/schema change needed —
   `zclassic23 dumpstate sovereignty` just works.

---

## 6. The copy-prove plan (before any live cutover)

The existing `tools/scripts/cure-copy-prove.sh` is built around the **producer
bundle** install (`-install-consensus-bundle` + injected replay receipt) and its
verdict **requires** `coins_kv_self_folded` present (G-SOV part 3,
`cure-copy-prove.sh:261-264`). The operational import path is **self_folded=false
by design**, so it needs a sibling harness `tools/scripts/import-copy-prove.sh`
that reuses the same safety scaffolding but swaps the install step and the
verdict:

1. **Safety (unchanged):** require the `--copy-dir` to contain the literal
   `/.zclassic-c23-COPY-` marker and refuse to alias any live datadir
   (`cure-copy-prove.sh:92-107`); `cp -a` the source; strip `.pid`/`.lock`;
   `cleanup()` trap SIGKILLs the copy's node on exit
   (`cure-copy-prove.sh:150-168`).
2. **Point-in-time chainstate copy:** also copy `~/.zclassic/chainstate` via the
   `utxo_recovery_ldb_copy.c` stable-signature guard (never read the live one).
3. **Run the importer** against the copy:
   `zclassic23 -datadir="$COPY" -import-complete-shielded="$CHAINSTATE_COPY"`
   (isolated `$ISO_HOME`, `-nolegacyimport -nofilesync -connect=127.0.0.1:39999`,
   no real network — same isolation as `cure-copy-prove.sh:176-202`). FAIL unless
   the importer logs its explicit success banner and exits 0.
4. **Normal isolated boot** of the copy (`cure-copy-prove.sh:205-211` pattern).
5. **Gate — all three, no from-genesis fold anywhere in the above:**
   - **(a) Blocker cleared + H\* CLIMB past 3,176,325**: poll `getblockcount` /
     `dumpstate reducer_frontier` until H\* > 3,176,325 and rising toward tip
     within the deadline (G-SOV part 1, `cure-copy-prove.sh:220-245`). The
     `utxo_apply.anchor_backfill_gap` and `utxo_apply.nullifier_backfill_gap`
     blockers must both be **absent** in `dumpstate blocker`.
   - **(b) Continuity**: `coins_applied_height == hstar + 1`
     (`cure-copy-prove.sh:247-256`).
   - **(c) EXACT same-height tip-hash parity vs `zclassicd`**: for the pre-wedge
     height 3,176,325, the wedge+1 height 3,176,326, and the current tip,
     `getblockhash H` on the copy must equal `zclassicd`'s `getblockhash H`
     (this is the roadmap's "seed, wedge, and same-height tip hashes match
     zclassicd" immediate gate; the existing harness leaves hash cross-check
     **manual** — automate it here). This is the check that actually catches a
     **missing anchor/nullifier** (a partial import folds a *different* chain or
     re-wedges at the first below-cursor spend).
6. **Expected sovereignty posture (not a failure):** `dumpstate sovereignty`
   must report `trust_mode == release_assisted`,
   `authority_posture == proven_but_not_self_folded`, `self_folded == false`.
   The harness records this as **PASS-with-assisted-posture**, distinct from the
   sovereign harness which requires `self_folded == true`.
7. **Warm-restart + kill-9 resume:** restart the copy's node and kill-9 mid-boot;
   H\* must not regress and the blocker must not reappear (roadmap immediate
   gate: "warm restart and kill-9 resume without partial reinstall").

Only on a green (a)+(b)+(c)+(7) does the operator run the same importer against
the live canonical datadir, with the three-layer revert available (datadir
backup; the import is a single transaction so an abort leaves the prior wedge
intact; and the sovereign install can later supersede it wholesale).

---

## 7. Risk analysis — the "~103×" failure class and how completeness + copy-prove catch it

The MEMORY warning is that recycling old halt narratives **re-halted forward
sync ~103×**; the standing demand is **complete + atomic + copy-proven**. The
core danger of *this* path is specifically that **flipping `activation_cursor`
to 0 converts a safe halt into an unsafe accept**: below a positive cursor a
missing root is `HISTORY_INCOMPLETE` (block held, node safe); at cursor 0 a
missing root is `MISSING` (a genuine negative → the referencing block is
**rejected as invalid**, which can re-wedge or, worse, fork away from the
network). Every failure mode below is a variant of "cursor flipped to 0 while the
set was actually incomplete."

| Failure mode | Consequence | Caught by |
|---|---|---|
| **Partial anchor import** (torn SST, iterator ended early, a `'Z'`/`'A'` record skipped) | A later block spends the missing anchor → `MISSING` → block rejected → re-wedge or fork | Importer aborts on any anomaly and the whole thing is **one transaction** (§4.4 step 8) — a partial set never commits, so the cursor never flips on incomplete data. `db_iter_check_error` (`reader.c:331`) is the torn-SST detector, already proven against the 2026-06-09 torn-coins class (`utxo_recovery_ldb_copy.c:5-11`). Copy-prove **(c)** catches any residual gap as a hash divergence at the first below-cursor spend. |
| **A nullifier missed** | A double-spend of a shielded note is accepted (nullifier absent → `nullifier_kv_get` returns not-found → spend allowed) | `nullifier_kv_get` fails **closed** on bind error (`nullifier_kv.c:485-523`) but not on genuine absence — so completeness is the only defense: atomic all-or-nothing import + count sanity floor; **and** copy-prove **(c)** exact hash parity vs `zclassicd`, which diverges the moment a should-be-invalid spend is accepted. |
| **Frontier root doesn't match** (the imported *tip* Sapling tree is wrong) | Forward fold appends to a wrong tree → every subsequent `hashFinalSaplingRoot` diverges | The reader **re-hashes every anchor and refuses unless it matches its key** (`reader.c:260-271`), and the importer additionally asserts the best Sapling anchor's root == `block_index[tip].hashFinalSaplingRoot` (§4.4 step 5) — a **chain-committed** bind (§3). Copy-prove **(a)** would immediately fail to climb. |
| **A new block references an anchor still absent** despite cursor==0 | Block false-rejected → re-wedge | This is exactly what copy-prove **(a)** (H\* must climb *past* 3,176,325 and keep rising) + **(c)** (tip parity) exist to catch **before** live. If the copy re-wedges, the import never touches the live datadir. |
| **Sprout trust weaker than Sapling** (no header commitment) | A forged Sprout anchor could be imported | Trust bottoms at the operator's own `zclassicd` chainstate + the reader's root re-hash (a forged tree can't hash to its key). Recorded in provenance as `release_assisted`; Sprout is a drained pre-Sapling pool. The sovereign install later replaces it with a self-derived Sprout history. |
| **`activation_cursor` flipped but rows use wrong `height`** (esp. Sprout, §4.3) | `anchor_kv_latest_tree` returns the wrong frontier for appends | Assert the best-anchor pointer (`'z'`/`'a'`) is written with the max height so `ORDER BY height DESC` returns it; copy-prove **(a)** catches a wrong-frontier fold as a stalled/diverging climb. |
| **Live chainstate torn mid-copy** | Silent holes (the classic 2026-06-09 bug) | The point-in-time FNV-signature copy guard (`utxo_recovery_ldb_copy.c`) retries until the source is provably unchanged across the copy window. |

**Honest residual risks:**

- **C23's default Sapling-root enforcement is off** (`-enforce-sapling-root`
  default false, `connect_block.c:725-734`). The importer's tip-anchor bind and
  copy-prove **(c)** cover the *import*, but ongoing forward validation does not
  re-check every block's Sapling root by default. Recommend running the
  operational node with a periodic `sapling_tree_rebuild()` audit
  (`sync_controller_sapling_tree.c`) or enabling `-enforce-sapling-root` **only
  after** a 0-false-reject full-history replay — that flip is its own gated task,
  not part of this path.
- **`self_folded` wiring gap** (§5.1): if promotion later relies on a live
  `coins_kv_mark_self_folded` that is currently test-only, sovereignty could
  silently never flip. Verify the producer install path actually stamps it
  before declaring `T_sovereign`.
- **Mining/spending are not currently bit-gated** (§5.3): until the guards in
  §5.3 land, an operator *could* mine/spend on borrowed shielded history. Land
  those guards with this path.
- Copy-prove **(c)** requires `zclassicd` to be **at or above tip** and
  answering RPC during the proof — it is (the operator's reference node). If it
  were behind, the hash cross-check at tip would be unavailable; gate the proof
  on `zclassicd` height ≥ copy tip.

---

## 8. Fast path vs waiting ~9h for the producer bundle — sequencing

| | **Import path (this doc)** | **Producer bundle (sovereign cure)** |
|---|---|---|
| Time to operational tip | minutes (bulk insert + cursor flip; no fold) | ~9 h (from-genesis full-validation fold to the anchor, `pv`-dominant) |
| Trust | `release_assisted` (borrowed shielded history, Sapling chain-bound, Sprout `zclassicd`-bound) | `sovereign` (`self_folded=true`, self-derived) |
| Enables serve/explorer/relay/view | yes, immediately | yes (later) |
| Enables mining / spend / snapshot re-serve / publish | **no** (must stay gated on `self_folded`, §5.3) | yes |
| New code | ~1 importer service + 6 reader entry points + 1 cursor primitive + 1 dumper + 1 harness | already in flight (producer + bundle + receipt + `cure-copy-prove.sh`) |
| Risk if incomplete | re-wedge / fork — mitigated by atomic + copy-prove **(c)** | same class, already mitigated by receipt + `cure-copy-prove.sh` |

**They are not mutually exclusive — do both, in this order:**

1. **Now:** build + copy-prove the import path; on green (a)+(b)+(c)+(7), cut it
   over to the live canonical datadir. The node reaches and serves tip
   **operationally** in minutes, honestly labelled `release_assisted`, with
   mining/spend/re-serve **held** on `self_folded==false` (§5.3 guards land with
   it). This ends the wedge *today* without waiting on the ~9 h fold.
2. **In parallel / after:** let the producer finish its from-genesis fold, export
   `zcl.consensus_state_bundle.v1`, verify → receipt → install (the existing
   `cure-runbook-2026-07-16.md` flow). The install **atomically supersedes** the
   borrowed shielded history with self-derived state and flips `self_folded=true`
   → `trust_mode: sovereign` → mining/spend/re-serve/publish unlock. The PSC
   makes this fold progressively faster (shrinks `T_sovereign`).

The import writes the same anchor_kv/nullifier_kv/coins_kv tables the sovereign
install replaces wholesale, so step 2 never has to reconcile with step 1 — it
overwrites it in one transaction. Operational readiness and sovereignty are
**decoupled timelines on the same tables**, which is exactly the
`T_ready` / `T_sovereign` split the roadmap already mandates
(`SOVEREIGN-NETWORK-ROADMAP.md` Phase 1).

**Recommendation:** implement the import path as the fast route to operational
tip **now**, ship the §5.3 mining/spend/re-serve guards with it so borrowed
shielded history can never be used to mint/spend/publish, prove it on a copy with
the sibling harness gating on H\* climb + `coins_applied==hstar+1` + exact
same-height hash parity vs `zclassicd`, then cut over. Keep the producer /
PSC sovereign path running unchanged to promote `release_assisted → sovereign` in
the background. This gets the node useful today and sovereign soon, with the
completeness+atomic+copy-prove discipline that the ~103× re-halt history demands.

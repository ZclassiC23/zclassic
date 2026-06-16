# ZClassic23 Validation Audit Matrix

This document records EXACTLY what cryptographic validation is performed at
each stage of block processing. "Trust nothing" — every hash, every signature,
every proof is verified by one of these layers.

## Validation Stages

| STAGE | Hash | PoW | Merkle | Scripts | UTXOs | Shielded |
|-------|------|-----|--------|---------|-------|----------|
| LevelDB load          | trust   | no  | no  | no    | no        | no    |
| Block file scan       | compute | no  | no  | no    | no        | no    |
| `accept_block_header` | verify  | YES | no  | no    | no        | no    |
| `check_block`         | no      | no  | YES | no    | no        | no    |
| `contextual_check`    | no      | YES | no  | no    | no        | no    |
| `connect_block`       | no      | no  | no  | YES\* | YES       | YES\* |
| `bg_validation_svc`   | no      | YES | YES | YES   | no        | YES   |
| `bg_hash_verify_svc`  | YES     | no  | no  | no    | no        | no    |
| Boot-time checks      | no      | no  | no  | no    | count+XOR | no    |
| Post-import check     | no      | no  | no  | no    | SHA3      | no    |

\* = skipped below `g_deferred_proof_validation_below_height` (set after fast
sync, cleared when `bg_validation` completes full chain verification).

## Hash Algorithms Used

**SHA256d (double SHA-256):**
- Block header hash (`block_header_get_hash`)
- Transaction hash / txid (`transaction_compute_hash`)
- Merkle root (`compute_merkle_root_mutated`)

**Blake2b-256:**
- Equihash PoW (personalized `"ZcashPoW"` + N,K params)
- Transaction sighash (personalized `"ZcashSigHash"` + branch_id)
- Sighash preimages: `ZcashPrevoutHash`, `ZcashSequencHash`,
  `ZcashOutputsHash`, `ZcashJSplitsHash`, `ZcashSSpendsHash`,
  `ZcashSOutputHash`

**SHA3-256:**
- UTXO set commitment (canonical order: txid+vout+value+script+height)
- Snapshot integrity (FlyClient + SHA3 binding)

**RIPEMD-160 + SHA-256:**
- hash160 for P2PKH/P2SH addresses

**secp256k1 ECDSA:**
- Transaction input signature verification
- Verified in `connect_block` (live) and `bg_validation` (background)

**Ed25519:**
- Sprout JoinSplit signatures

**Groth16 (BLS12-381):**
- Sapling spend proofs
- Sapling output proofs
- Sprout JoinSplit proofs (post-Sapling)

**RedJubjub:**
- Sapling binding signatures (balance proof)

**XOR accumulator:**
- Incremental UTXO set commitment (add/remove per block)
- Verified against full recomputation on boot

## Equihash Parameters

ZClassic uses Equihash(200,9) with solution size 1344 bytes.
- Personalization: `"ZcashPoW"` + LE32(N) + LE32(K)
- Input: serialized header (version through nNonce, excluding solution)

## Sync Methods

(See [`SYNC.md`](../SYNC.md) for the operator guide.)

**Method 1 — LDB Snapshot Import (~20 seconds):**
- Copy `zclassicd chainstate/` → `zclassic-c23/chainstate/`
- Clear `leveldb_utxo_migrated` flag in `node_state`
- Boot detects `chainstate/` + no flag → parallel decode pipeline:
  LevelDB snapshot iterator → 30 decoder threads → SQLite writer
- Reads `'c'+txid` keys, decodes compressed CCoins (varint+bitmap)
- Writes 1.35M UTXOs in ~9.7s, rebuilds indexes in 16ms
- Sets `coins_best_block` from LevelDB best block hash
- SHA3-256 verification against hardcoded checkpoint
- Loads `block_index.bin` flat file (~8s for 6M entries)
- Connects remaining blocks via P2P (~2s)
- File: `config/src/boot.c`
- File: `app/controllers/src/sync_controller.c`

**Method 2 — P2P Fast Sync (~60 seconds):**
- Connect to NODE_ZCL23 peer → receive UTXO chunk manifest
- Download chunks in parallel (swarm protocol with rarest-first)
- Verify each chunk hash, write to SQLite
- FlyClient MMR proof binds UTXO set to PoW chain
- File: `lib/net/src/fast_sync.c`

**Method 3 — Full P2P Sync (~7 hours):**
- Headers → blocks → reducer stage pipeline → `connect_block`
  (sequential UTXO build inside validation stages)
- Scripts/sigs skipped below deferred proof validation (h=3,054,000)
- Full validation above deferred proof validation
- File: `app/services/src/chain_activation_controller.c`
- File: `app/jobs/src/*_stage.c`
- File: `lib/validation/src/connect_block.c`

## Self-Healing Mechanisms

**1. Reducer validation failure (missing UTXO):**
- Detect missing txid:vout from `validation_state`
- Look up source block via tx index (LevelDB `'t'+txid` key)
- Read block from disk, extract output, inject into coins cache
- Retry `connect_block` (up to 100 retries per block)
- File: `lib/validation/src/process_block_self_heal.c`
- File: `lib/validation/src/process_block_self_heal_sqlite_tx_index.c`
- File: `lib/validation/src/process_block_self_heal_chain_scan.c`
- File: `lib/validation/src/process_block_self_heal_inject.c`

**2. Reducer reorg unwind:**
- Detect branch divergence from the active-chain cursor and stage logs
- Emit inverse UTXO deltas for the stale branch
- Delete stale log/delta rows and rewind stage cursors to the fork boundary
- File: `app/jobs/src/utxo_apply_delta.c`
- File: `app/jobs/src/utxo_apply_stage.c`

**3. Wrong block on disk (hash mismatch):**
- Clear `BLOCK_HAVE_DATA` flag on the `block_index` entry
- Block gets re-requested from P2P download manager
- File: `lib/validation/src/process_block_tip_child.c`
- File: `app/conditions/src/have_data_unreadable.c`

**4. Stale `coins_best_block` after OOM kill:**
- Boot detects chain at genesis but `coins_best_block` non-null
- Triggers `BOOT_RECOVER_WIPE_WAIT` → resets to clean state
- File: `config/src/boot_index.c` (`validate_coins_chain_agreement`)

**5. Download stall (no blocks arriving):**
- Scans 10-height window above chain tip for gaps
- Requests missing blocks from alternate peers
- Clears `BLOCK_FAILED` flags if all heights exhausted
- File: `app/services/src/block_sync_service.c`

## Data Formats

**Block files (`blk*.dat`):**
- `[4B magic 0x6427e924][4B size LE][block data]`
- Magic is ZClassic mainnet. Size excludes the 8-byte frame header.

**Block header (140 bytes fixed + variable solution):**
- `nVersion(4) | hashPrevBlock(32) | hashMerkleRoot(32) |`
  `hashFinalSaplingRoot(32) | nTime(4) | nBits(4) | nNonce(32) |`
  `compact_size(nSolutionSize) | nSolution(var, typically 1344)`

**LevelDB block index (key: `'b' + hash[32]`):**
- Value: `varint(nVersion) | varint(nHeight) | varint(nStatus) |`
  `varint(nTx) | [varint(nFile) if HAVE_DATA|HAVE_UNDO] |`
  `[varint(nDataPos) if HAVE_DATA] | [varint(nUndoPos) if HAVE_UNDO] |`
  `[uint32(nCachedBranchId) if ACTIVATES_UPGRADE] |`
  `hashSproutAnchor(32) | header fields (version through solution)`

**LevelDB chainstate (key: `'c' + txid[32]`):**
- Value: `varint(nVersion) | varint(nCode) |`
  `[mask bytes if nCode implies extra outputs] |`
  `[compressed txout for each available output] |`
  `varint(nHeight)`
- `nCode` encodes: bit0=coinbase, bit1=vout[0] present, bit2=vout[1]

**LevelDB tx index (key: `'t' + txid[32]`):**
- Value (zclassicd): `varint(nFile) | varint(nDataPos) | varint(nTxOffset)`
- Value (zclassic23): raw struct `{ int nFile; uint nPos; uint nTxOffset; }`
- Reader handles both formats (`lib/storage/src/txdb.c`)

**SQLite UTXO table:**
- `utxos(txid BLOB, vout INT, value INT, script BLOB, script_type INT,`
  `address_hash BLOB, height INT, is_coinbase INT)`

**SQLite `node_state` keys:**
- `coins_best_block` — 32-byte block hash of UTXO set tip
- `tip_height` — current chain height (int64 LE)
- `tip_hash` — current chain tip hash (32 bytes)
- `leveldb_utxo_migrated` — flag: LDB import done (prevents re-import)
- `bg_validation_height` — last bg-validated block height
- `bg_hash_verification_height` — last hash-verified block height
- `utxo_commitment_xor` — XOR accumulator checkpoint (40 bytes)
- `commitment_mmr_state` — serialized MMR (peaks + leaf count)
- `sapling_tree` — incremental Sapling commitment tree
- `snapshot_mmr_root` — deferred MMR verification root
- `snapshot_mmr_height` — deferred MMR verification height
- `schema_version` — database schema version (int32 LE)

**LevelDB obfuscation:**
- Key: `0x0e 0x00 "obfuscate_key"` (15 bytes)
- Value: `compact_size(N) + N random bytes`
- All LevelDB values XORed with this key (cyclic) before storage.
- Reader deobfuscates transparently (`lib/storage/src/dbwrapper.c`)

#!/usr/bin/env python3
"""install_e2e_fixture_stamp.py — stamp the MINIMAL durable progress.kv image
the -install-consensus-bundle gate evaluates, onto a header-imported fixture
datadir. See docs/work/install-e2e-proof-2026-07-18.md for the full predicate
derivation. This writes exactly the rows a validated sync to the anchor would
have produced, no more:

  1. coins_kv proven authority (3 rungs of coins_kv_is_proven_authority):
       - progress_meta coins_applied_height   = anchor+1 (8-byte LE)
       - progress_meta coins_kv_migration_complete = 0x01
       - coins table non-empty (one sentinel row; activate DELETEs all coins
         and streams the bundle's real set)
  2. stage_cursor layout identical to the production tip_finalize anchor seed
     (activate_apply_in_tx): 7 upstream stages at anchor+1, tip_finalize at
     anchor (served-tip convention).
  3. tip_finalize_log anchor row at the anchor height: status='anchor', ok=1,
     tip_hash = the REAL anchor block hash (from the imported header chain).
     This backs tip_finalize_stage_resolve_durable_tip and the
     convention-aware block_hash_at(H*) authority read.
  4. validate_headers_log pass records (ok=1, real hash) at the anchor height
     AND at the bundle's sapling_frontier_height — the two rows
     validate_headers_stage_has_pass_record consults. Rows at/below the anchor
     are outside every contiguity scan ([anchor+1, cursor)), so their itags are
     never gate-evaluated; they are still computed correctly here.

Only the fixture datadir passed as argv[1] is touched. Idempotent (INSERT OR
REPLACE), safe to re-run after a wipe.
"""

import hashlib
import sqlite3
import sys
import time

ANCHOR = 3056758
SAPLING_SOURCE = 3056742


def itag(table: str, height: int, ok: int, status: bytes | None = None) -> bytes:
    """Mirror app/jobs/src/stage_row_itag.c:stage_row_itag_compute.

    preimage = table || 0x00 || height(u64 LE) || ok(u8)
               [|| status_len(u32 LE) || status]   # covered tables only
    tag = SHA3-256(preimage)[:16]
    """
    covers = table in ("script_validate_log", "proof_validate_log",
                       "utxo_apply_log")
    h = hashlib.sha3_256()
    h.update(table.encode())
    h.update(b"\x00")
    h.update(height.to_bytes(8, "little"))
    h.update(bytes([1 if ok else 0]))
    if covers:
        blob = status or b""
        h.update(len(blob).to_bytes(4, "little"))
        h.update(blob)
    return h.digest()[:16]


def main() -> int:
    if len(sys.argv) != 2:
        print(f"usage: {sys.argv[0]} <fixture-datadir>", file=sys.stderr)
        return 2
    datadir = sys.argv[1].rstrip("/")
    node_db = f"{datadir}/node.db"
    progress = f"{datadir}/progress.kv"

    # Real hashes from the imported header chain (never hardcode: read them).
    ndb = sqlite3.connect(f"file:{node_db}?mode=ro", uri=True)
    anchor_row = ndb.execute(
        "SELECT hash, sapling_root FROM blocks WHERE height=? AND hash=("
        "  SELECT hash FROM blocks WHERE height=? ORDER BY rowid LIMIT 1)",
        (ANCHOR, ANCHOR)).fetchone()
    # Deterministic main-chain pick: walk back from the imported tip is
    # overkill here — the gate binds by hash, so take the row whose hash the
    # compiled checkpoint names (53d65b85…); assert uniqueness instead.
    rows = ndb.execute(
        "SELECT hash, sapling_root FROM blocks WHERE height=?", (ANCHOR,)).fetchall()
    sap_rows = ndb.execute(
        "SELECT hash, sapling_root FROM blocks WHERE height=?", (SAPLING_SOURCE,)).fetchall()
    ndb.close()
    if len(rows) != 1 or len(sap_rows) != 1:
        print(f"REFUSE: side-branch ambiguity at anchor ({len(rows)}) or "
              f"sapling source ({len(sap_rows)})", file=sys.stderr)
        return 1
    anchor_hash, anchor_sroot = rows[0]
    sap_hash, sap_sroot = sap_rows[0]
    print(f"anchor h={ANCHOR} hash={anchor_hash.hex()}")
    print(f"sapling source h={SAPLING_SOURCE} hash={sap_hash.hex()}")
    print(f"anchor sapling_root={anchor_sroot.hex()}")
    print(f"source sapling_root={sap_sroot.hex()}")

    now = int(time.time())
    pdb = sqlite3.connect(progress)
    pdb.execute("PRAGMA busy_timeout=30000")
    cur = pdb.cursor()

    # Schema (production CREATE IF NOT EXISTS shapes; boot ensures them too).
    # The four empty log tables MUST exist: reducer_frontier_compute_hstar
    # prepares SELECTs against all six k_logs tables and LOG_FAILs on
    # "no such table" — the gate never reaches its own verdict without them.
    cur.executescript("""
    CREATE TABLE IF NOT EXISTS progress_meta (key TEXT PRIMARY KEY, value BLOB NOT NULL);
    CREATE TABLE IF NOT EXISTS stage_cursor (name TEXT PRIMARY KEY, cursor INTEGER NOT NULL, updated_at INTEGER NOT NULL);
    CREATE TABLE IF NOT EXISTS coins (txid BLOB NOT NULL, vout INTEGER NOT NULL, value INTEGER NOT NULL, height INTEGER NOT NULL, is_coinbase INTEGER NOT NULL, script BLOB NOT NULL, PRIMARY KEY (txid, vout)) WITHOUT ROWID;
    CREATE TABLE IF NOT EXISTS tip_finalize_log (height INTEGER PRIMARY KEY, status TEXT NOT NULL, ok INTEGER NOT NULL, work_delta_high INTEGER NOT NULL, work_delta_low INTEGER NOT NULL, utxo_size_after INTEGER NOT NULL, reorg_depth INTEGER NOT NULL, finalized_at INTEGER NOT NULL);
    CREATE TABLE IF NOT EXISTS validate_headers_log (height INTEGER PRIMARY KEY, hash BLOB NOT NULL, ok INTEGER NOT NULL, fail_reason TEXT, validated_at INTEGER NOT NULL, itag BLOB);
    CREATE TABLE IF NOT EXISTS script_validate_log (height INTEGER PRIMARY KEY, status TEXT NOT NULL, ok INTEGER NOT NULL, tx_count INTEGER NOT NULL, input_count INTEGER NOT NULL, first_failure_txid BLOB, first_failure_vin INTEGER, first_failure_serror INTEGER, validated_at INTEGER NOT NULL, block_hash BLOB, source_epoch_digest BLOB, itag BLOB);
    CREATE TABLE IF NOT EXISTS body_persist_log (height INTEGER PRIMARY KEY, source TEXT NOT NULL, ok INTEGER NOT NULL, persisted_at INTEGER NOT NULL, itag BLOB);
    CREATE TABLE IF NOT EXISTS proof_validate_log (height INTEGER PRIMARY KEY, status TEXT NOT NULL, ok INTEGER NOT NULL, sapling_spends_total INTEGER NOT NULL, sapling_outputs_total INTEGER NOT NULL, sprout_joinsplits_total INTEGER NOT NULL, block_hash BLOB, source_epoch_digest BLOB, first_failure_txid BLOB, first_failure_proof_type TEXT, validated_at INTEGER NOT NULL, itag BLOB);
    CREATE TABLE IF NOT EXISTS utxo_apply_log (height INTEGER PRIMARY KEY, status TEXT NOT NULL, ok INTEGER NOT NULL, spent_count INTEGER NOT NULL, added_count INTEGER NOT NULL, total_value_delta INTEGER NOT NULL, first_failure_kind TEXT, first_failure_detail BLOB, applied_at INTEGER NOT NULL, itag BLOB);
    CREATE TABLE IF NOT EXISTS header_admit_log (height INTEGER PRIMARY KEY, hash BLOB NOT NULL, parent_hash BLOB, admitted_at INTEGER NOT NULL);
    CREATE TABLE IF NOT EXISTS utxo_apply_delta (height INTEGER PRIMARY KEY, branch_hash BLOB NOT NULL, spent_blob BLOB NOT NULL, added_blob BLOB NOT NULL);
    CREATE TABLE IF NOT EXISTS body_fetch_log (height INTEGER PRIMARY KEY, hash BLOB NOT NULL, source TEXT NOT NULL, bytes INTEGER NOT NULL DEFAULT 0, fetched_at INTEGER NOT NULL, ok INTEGER NOT NULL, fail_reason TEXT);
    """)
    for col in ("tip_hash BLOB", "itag BLOB"):
        try:
            cur.execute(f"ALTER TABLE tip_finalize_log ADD COLUMN {col}")
        except sqlite3.OperationalError as e:
            if "duplicate column" not in str(e):
                raise

    # (1) proven authority
    cur.execute("INSERT OR REPLACE INTO progress_meta(key,value) VALUES('coins_applied_height',?)",
                ((ANCHOR + 1).to_bytes(8, "little"),))
    cur.execute("INSERT OR REPLACE INTO progress_meta(key,value) VALUES('coins_kv_migration_complete',?)",
                (b"\x01",))
    cur.execute("INSERT OR REPLACE INTO coins(txid,vout,value,height,is_coinbase,script) VALUES(?,?,?,?,?,?)",
                (b"\x00" * 32, 0, 0, ANCHOR, 0, b"\x00"))

    # (2) stage cursors — production post-anchor-seed layout
    for stage in ("header_admit", "validate_headers", "body_fetch",
                  "body_persist", "script_validate", "proof_validate",
                  "utxo_apply"):
        cur.execute("INSERT OR REPLACE INTO stage_cursor(name,cursor,updated_at) VALUES(?,?,?)",
                    (stage, ANCHOR + 1, now))
    cur.execute("INSERT OR REPLACE INTO stage_cursor(name,cursor,updated_at) VALUES('tip_finalize',?,?)",
                (ANCHOR, now))

    # (3) tip_finalize anchor row at the anchor (own-hash convention)
    cur.execute(
        "INSERT OR REPLACE INTO tip_finalize_log"
        "(height,status,ok,work_delta_high,work_delta_low,utxo_size_after,"
        " reorg_depth,finalized_at,tip_hash,itag) VALUES(?,?,1,0,0,0,0,?,?,?)",
        (ANCHOR, "anchor", now, anchor_hash,
         itag("tip_finalize_log", ANCHOR, 1)))

    # (4) validate_headers pass records (own-hash rows)
    for h, hh in ((ANCHOR, anchor_hash), (SAPLING_SOURCE, sap_hash)):
        cur.execute(
            "INSERT OR REPLACE INTO validate_headers_log"
            "(height,hash,ok,fail_reason,validated_at,itag) VALUES(?,?,1,NULL,?,?)",
            (h, hh, now, itag("validate_headers_log", h, 1)))

    pdb.commit()

    # Read-back verification of the exact rows the gate consults.
    checks = [
        ("coins_applied_height", cur.execute(
            "SELECT value FROM progress_meta WHERE key='coins_applied_height'").fetchone()[0].hex()),
        ("migration_complete", cur.execute(
            "SELECT hex(value) FROM progress_meta WHERE key='coins_kv_migration_complete'").fetchone()[0]),
        ("coins_rows", cur.execute("SELECT COUNT(*) FROM coins").fetchone()[0]),
        ("tf_anchor_row", cur.execute(
            "SELECT height,status,ok,hex(tip_hash) FROM tip_finalize_log WHERE height=?",
            (ANCHOR,)).fetchone()),
        ("vh_pass_anchor", cur.execute(
            "SELECT ok FROM validate_headers_log WHERE height=? AND hash=?",
            (ANCHOR, anchor_hash)).fetchone()),
        ("vh_pass_sapling", cur.execute(
            "SELECT ok FROM validate_headers_log WHERE height=? AND hash=?",
            (SAPLING_SOURCE, sap_hash)).fetchone()),
        ("cursors", cur.execute(
            "SELECT name,cursor FROM stage_cursor ORDER BY name").fetchall()),
    ]
    pdb.close()
    for name, val in checks:
        print(f"  {name}: {val}")
    print("STAMPED: minimal durable image written to", progress)
    return 0


if __name__ == "__main__":
    sys.exit(main())

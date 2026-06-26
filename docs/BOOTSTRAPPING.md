# Bootstrapping to tip (fast-sync starter pack)

A freshly built `zclassic23` node can reach the chain tip in **minutes** instead
of doing a full multi-hour P2P sync from genesis, by seeding from a published
starter pack: a prebuilt block index plus a SHA3-self-verified UTXO snapshot.

The snapshot is **not blindly trusted**. At boot the node recomputes the
snapshot body's SHA3 hash and checks its anchor block hash against the PoW
header compiled into the binary — a tampered or wrong-chain snapshot is refused.

> Without the starter pack a fresh node still works; it just does the long P2P
> header + body sync from genesis.

## The release

GitHub release: **[`starterpack-3155842`](https://github.com/ZclassiC23/zclassic/releases/tag/starterpack-3155842)** on `ZclassiC23/zclassic`.

| File | Size | SHA-256 |
|------|------|---------|
| `block_index.bin` | 543,021,428 | `a40b184d0d52f91438762928abdadd151a8011efc0340485c690732988d5d6e0` |
| `utxo-seed-3155842.snapshot` | 104,908,275 | `46e4f6bd090e51417a4d8b70a1b7c8a218d9c8e3cded1bba812033117f5d9e9f` |

Direct download URLs:

- https://github.com/ZclassiC23/zclassic/releases/download/starterpack-3155842/block_index.bin
- https://github.com/ZclassiC23/zclassic/releases/download/starterpack-3155842/utxo-seed-3155842.snapshot

- `block_index.bin` — contiguous block index (genesis → height 3,155,841): the
  height-sorted header graph, loaded from `<DATADIR>/block_index.bin`.
- `utxo-seed-3155842.snapshot` — a v2 UTXO + Sapling-frontier snapshot at seed
  height **3,155,842** (1,344,903 UTXOs).

## Quickstart

Build the node from `main` (see [`docs/BUILD.md`](./BUILD.md)), then:

```bash
# 1. Download both assets (into the current dir)
gh release download starterpack-3155842 -R ZclassiC23/zclassic
# (or curl the two direct URLs above)

# 2. Verify integrity (must print "OK" for both)
sha256sum -c <<'EOF'
a40b184d0d52f91438762928abdadd151a8011efc0340485c690732988d5d6e0  block_index.bin
46e4f6bd090e51417a4d8b70a1b7c8a218d9c8e3cded1bba812033117f5d9e9f  utxo-seed-3155842.snapshot
EOF

# 3. Place BOTH files inside a fresh datadir
DATADIR="$HOME/.zclassic-c23"          # or any empty directory
mkdir -p "$DATADIR"
mv block_index.bin            "$DATADIR/"
mv utxo-seed-3155842.snapshot "$DATADIR/"

# 4. Boot, pointing the loader at the snapshot
build/bin/zclassic23 \
  -datadir="$DATADIR" \
  -load-snapshot-at-own-height="$DATADIR/utxo-seed-3155842.snapshot"
```

### Expected result

`getblockcount` jumps to ~**3,155,842** within seconds (not 0), then climbs to
the network tip in minutes as the node folds forward over P2P block bodies. The
boot log shows the seed being accepted:

```
[boot] -load-snapshot-at-own-height: anchor hash MATCHES the in-binary PoW header at h=3155842 — snapshot is consensus-bound to this chain
[boot] -load-snapshot-at-own-height: SELF-verified snapshot ... (body SHA3 OK, height=3155842, count=1344903) — seeded coins_kv
```

### Notes

- `block_index.bin` is read by filename from `<DATADIR>/block_index.bin`. Place
  it there exactly.
- The snapshot path can live anywhere; pass its full path to
  `-load-snapshot-at-own-height`. The seed height is read from the snapshot's
  verified header, not from the filename.
- `-load-snapshot-at-own-height` is an explicit-only recovery loader — a normal
  boot never touches it, so leaving the snapshot file in the datadir without the
  flag does nothing.

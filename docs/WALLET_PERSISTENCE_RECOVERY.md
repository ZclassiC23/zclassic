# Wallet persistence recovery

The node refused to boot because it found wallet keys on disk that it could
not safely load. It stopped instead of generating a fresh keypool over them.
That refusal is the protection: an earlier version of this code took the
silent path and made spendable funds unspendable.

Nothing has been rewritten. Every key row is still where it was.

This page covers the three boot refusals that name it:

| code | what the node observed |
| --- | --- |
| `BOOT_WALLET_PERSISTENCE_OPEN_FAILED` | `wallet_keys` has rows, but the wallet persistence layer would not open. |
| `BOOT_WALLET_CANARY_FAILED` | The wallet opened, but its write-then-read self-test failed. |
| `BOOT_WALLET_KEYSTORE_COUNT_MISMATCH` | Fewer keys loaded into memory than there are rows on disk. |

## 1. Copy the data directory first

Before anything else:

```sh
cp -a ~/.zclassic-c23 ~/.zclassic-c23.rescue-copy
```

Work on the copy. The original is your evidence and your key material.

## 2. Where the keys actually live

Wallet key material is stored inside `<datadir>/node.db` (SQLite), in the
`wallet_keys`, `wallet_sapling_keys`, and `wallet_seed` tables. There is no
separate `wallet.dat` file to move.

`zclassic23 core storage query` and `core storage query offline` deliberately
**refuse** any statement that names those tables — they answer
`QUERY_REJECTED: query references secret wallet key material and is denied`.
Do not plan a recovery around reading them through that command; it will not
work by design.

## 3. Read the evidence line

Each of the three refusals prints an `evidence:` line with the measurements
that decided it — the SQLite error code and the `file:line` that produced it,
the canary's own error text, or the two counts that disagreed. That line names
the failing layer. Start there rather than guessing.

Common, checkable causes:

```sh
ls -l ~/.zclassic-c23/node.db ~/.zclassic-c23/node.db-wal ~/.zclassic-c23/node.db-shm
df -h  ~/.zclassic-c23
```

An open failure is usually ownership, mode, or a truncated file. A canary
failure is usually a full or read-only filesystem — the canary writes a probe
row and reads it back through the wallet's own handle, so it fails whenever
the wallet could not durably write.

`BOOT_WALLET_KEYSTORE_COUNT_MISMATCH` is different: it means the loader
dropped rows it could see. That is a zclassic23 defect, not an environment
problem. Keep the rescue copy — it is the only reproduction.

## 4. Rotated wallet backups

The node writes periodic verified wallet backups outside the datadir, so a
damaged `node.db` is not the only copy:

```sh
ls -lt ~/wallet_backups/wallet_backup_*.sqlite | head
```

Each file is a standalone SQLite database holding the wallet tables as of that
run. When `WALLET_BACKUP_PASSWORD` was set, backups are encrypted and are
decrypted with the same variable:

```sh
WALLET_BACKUP_PASSWORD=... zclassic23 --decrypt-wallet-backup <src.enc> <dst.sqlite>
```

## 5. What not to do

- Do not delete `node.db` to "start clean". That is the key store.
- Do not delete `<datadir>/zclassic23.pid` while a node is running; it is the
  single-writer lock, and two nodes on one datadir corrupt both stores.
- Do not re-run the node against the original datadir until the cause named in
  the `evidence:` line is fixed. Every refused boot is harmless; the harmful
  step is the one that writes.

## Related

- `docs/BOOT_INVARIANTS.md` — what each boot stage guarantees, including the
  `wallet_loaded` boundary these refusals sit on.
- `config/include/config/boot_error.h` — the contract these messages are
  rendered in (`code` / `phase` / `message` / `evidence` / `next[]`).

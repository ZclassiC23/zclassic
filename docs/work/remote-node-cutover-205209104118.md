# Remote Node Public Lane: 205.209.104.118

Status as of 2026-07-07 13:20 UTC: the public service on
`205.209.104.118:8033` is `zclassic23`, not `zclassicd-rhett.service`.
`zclassicd-rhett.service` is inactive, `zclassic23` owns the public P2P socket,
and the running binary reports `zclassic23 v0.1.0 (build 894beee)`.

The public crawler row can lag this live state. Trust a direct socket/RPC check
first; crawler rows may keep showing the old `MagicBean:*` identity until they
reconnect or expire cached peer state.

## Invariant

Keep public `8033` owned by a synced `zclassic23` lane that advertises
`/ZClassic23:0.1.0/`, has stable peers, and answers RPC on localhost
`18232`. If the host ever regresses to `MagicBean:*`, re-run the historical
cutover checklist below on a candidate before touching public `8033`.

## Historical Candidate Ports

- `18043` / RPC `18243`: blocks-less snapshot candidate using
  `~/.zclassic-c23`. It advertises the right subversion but started from the
  snapshot seed and is still catching up.
- `18044` / RPC `18244`: fresh legacy-datadir candidate using
  `~/.zclassic-c23-public`. This is the preferred cutover candidate if it boots
  cleanly because it uses the remote node's own `~/.zclassic` block files.

## Safe Cutover Checklist

1. Confirm the old public node is healthy while testing candidates:
   `systemctl --user status zclassicd-rhett.service`.
2. Confirm candidate identity:
   `getnetworkinfo` on the candidate RPC must include
   `"advertised_subver":"/ZClassic23:0.1.0/"`.
3. Confirm candidate height:
   candidate `getblockcount` must be near remote zclassicd `getblockcount`.
4. Confirm public port is still owned by the old service before swapping:
   `ss -ltnp | grep ':8033'`.
5. Stop the private candidate, stop `zclassicd-rhett.service`, then start the
   zclassic23 public service on `-port=8033 -rpcport=18232`.
6. Re-check public visibility from another node or explorer. The row should
   change from `MagicBean:*` to `ZClassic23:0.1.0` only after the public `8033`
   service reconnects.

## Block File Trust Trap

Do not pair a copied `block_index.bin` with foreign or partial
`blocks/blk*.dat` files. The index stores source datadir file offsets, and a
different block file at the same number can deserialize garbage or the wrong
block. In `-nolegacyimport` snapshot boot, zclassic23 must treat existing block
files as untrusted unless each indexed body reads back and hashes to its
block-index entry.

## Public Explorer Smoke

Use `tools/scripts/public_explorer_smoke.sh` after HODL/explorer changes. It
checks `https://zclnet.net/api/v1/hodl` and `/explorer/hodl` without `jq` and
fails if either surface tells users to refresh, wait, or retry.

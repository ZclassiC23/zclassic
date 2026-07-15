# P2P C23 source hosting

ZClassic23 can host a BitTorrent-like source swarm without changing ZClassic
consensus. The source system is an application protocol: the blockchain may
anchor publisher identity, release roots, payments, or optional burn receipts,
but block and transaction validity remain exactly compatible with `zclassicd`.

## Trust model

The source tree is the authority; transport is not. A release is identified by
the `content.v2` package root from `lib/vcs/package_manifest.*`: canonical
portable paths, regular-file modes, exact sizes, and ordered 1 MiB raw
SHA3-256 chunk hashes. The current root is a flat domain-separated commitment,
not a Merkle tree. A peer therefore fetches the bounded manifest, recomputes the
package root, and only then accepts independently fetched chunks whose exact
position, length, and SHA3 hash match that manifest.

The hash preimage is frozen, not prose-dependent. Both ASCII domains include
their trailing `00` byte: `zcl.package_file.v1\0` and
`zcl.package_manifest.v1\0`. The single-file KAT for path `src/main.c`, mode
`0100644`, and bytes `int main(void) { return 23; }\n` is:

```text
chunk SHA3-256 = e419941e7ce1aec1f09056b33ba2a872e652e2ca05c95702ac60fd18682ce549
file hash      = d1727ca31da57a79f3d85b9f27f271357c07db3e6b890b7c6158cc1c017c1967
package root   = 5f6f1019c07539f6b2a45fe1d88c1b7c7b820c869e6b84776be81c48876615b8
```

Manifest storage and swarm `file_index` both use strict ascending canonical
path order; caller insertion order is never a wire coordinate.

The first network primitive is `lib/vcs/package_swarm.*`. It defines strict
announce, want, data, and cancel frames with request/package binding, canonical
little-endian encoding, a one-chunk-per-frame 1 MiB ceiling, exact-length
parsing, and content.v2 verification. It is intentionally a pure codec: it has
no socket, filesystem, wallet, install, build, execution, or publication
authority.

Do not put source packages through the legacy file-market trust path. Its offer
root and possession/payment checks predate `content.v2`, and the fast file
service's public-root-derived stream key is not peer authentication. Useful
quota, worker, backpressure, and PoW mechanics may be generalized later, but a
source swarm needs a fresh authenticated transport and a staging-only CAS.

## Release identity

Each published release should contain:

- the 32-byte `content.v2` package root;
- a human package name and semantic version, both bounded and normalized;
- the publisher's compressed secp256k1 public key and key id;
- a chain id and monotonically increasing publisher sequence;
- optional parent release roots;
- a canonical low-S signature over a SHA3-256 domain-separated release id.

The wallet broker signs the release id; private keys never enter an App. A
signature establishes authorship, not safety. Downloaded source remains inert
until a separate explicit inspect/build/install transaction passes policy.

## Swarm flow

1. A peer gossips a bounded announcement containing the package root and
   internally feasible manifest/count/size hints. Hints remain untrusted: they
   never reserve storage, earn ratio credit, or establish package identity.
2. The downloader requests the manifest, parses it, and recomputes the root.
3. A scheduler assigns missing chunks across several peers, with one request id
   per in-flight object, bounded retries, timeouts, per-peer windows, and
   cancellation.
4. Each response is written to a staging CAS only after exact content.v2
   verification. Resume state is a durable bitmap keyed by package root; it is
   derived and can be rebuilt by rehashing the CAS.
5. Completion re-verifies every file and the package root. Nothing is executed.
6. An explicit operator action may inspect, build in containment, test, sign a
   local verdict, and publish/install atomically.

HTTPS and onion are transport adapters over the same package/CAS contract.
Noise XX (or a comparably authenticated session) should protect direct P2P;
the existing public-UTXO-root-derived file-service key is not sufficient.

## Ratio and optional ZCL burn credits

The primary ratio should be earned by serving verified bytes:

```text
seed_ratio = verified_bytes_uploaded / max(verified_bytes_downloaded, 1)
```

Peers can grant a small, locally configured bootstrap allowance to new keys.
Receipts are signed by both peers and bind publisher key, package root, byte
count, session nonce, and time window. Receipts are advisory reputation, never
consensus facts, and each node may ignore them.

An optional proof-of-burn boost is technically possible without a fork. A
normal ZClassic transaction can assign value to a provably unspendable
`OP_RETURN` output carrying a new application lokad id, publisher key, package
root, and nonce. A node can verify the transaction against its existing chain
and give the publisher local service credit after a configured confirmation
depth. This does **not** change consensus rules or issuance: the output is
already unspendable under existing ZClassic script behavior and reduces the
spendable supply by that output's value.

Burn credit should be deliberately weak and optional:

- never required to download public source;
- never accepted from mempool-only evidence;
- reorg-aware and bound to an exact output amount and application payload;
- capped or logarithmic so wealth cannot permanently dominate bandwidth;
- used only for queue priority/bootstrap, while seeding earns the durable
  ratio;
- previewed with the exact irreversible ZCL amount and separately confirmed by
  the wallet owner;
- enforced by local peer policy, not block validity, mining, or activation.

No burn transaction builder ships until the payload, signature binding,
confirmation/reorg model, economics, and wallet confirmation UX have independent
review and tests.

## Implementation checklist

- [x] Canonical bounded `content.v2` manifest and SHA3 chunk verification.
- [x] Pure bounded announce/want/data/cancel codec tied to package roots.
- [ ] Signed release envelope using wallet-brokered secp256k1 keys.
- [ ] Staging-only content-addressed store with quotas and atomic verified puts.
- [ ] Durable resume bitmap and multi-peer rarest-first scheduler.
- [ ] Peer inventory, backpressure, timeout, retry, and offence accounting.
- [ ] Authenticated direct transport plus HTTPS and onion adapters.
- [ ] Dual-signed verified-byte receipts and local ratio policy.
- [ ] Optional proof-of-burn parser/indexer and reorg-aware credit projection.
- [ ] Explicit inspect/build/test/install transaction; downloads never execute.
- [ ] End-to-end simulator: malicious manifest, bad chunk, replay, cancellation,
  peer loss, resume, quota exhaustion, reorg, and deterministic seed replay.

The next code slice is the signed release envelope plus staging CAS. Runtime
gossip should wait until those two foundations are fail-closed and independently
tested.

# Sovereign Identity Layer — chain-anchored master keys (Design)

One 130-byte record on-chain per identity — a master key — with an entire
signed, content-addressed data plane hanging off it, forever. The chain is
the smallest possible root of trust: a land registry of keys. Everything
else (service descriptors, names, packages, relay endpoints, messages) is
signed data moving over gossip and the ZCODE swarm, verified against the
chain, never touching it again. **Write once, sign forever.**

Status: **draft for owner review**. Nothing here is built unless cited.
All phases stay out of the byte-sealed `core/` tree and add no external
dependencies. This document supersedes the ZDIR-first framing; the relay
directory survives as one *application* of the identity layer (Phase 3),
not the headline.

## Background: the actual gap in Tor

Tor's anonymity engine (onion circuits, NTOR, rendezvous) is decentralized
and mature — it is not the target. The weak layers are identity and
directory:

- **~9-10 hardcoded Directory Authorities** vote hourly and majority-sign
  the relay consensus document; **~6 Bandwidth Authorities** decide which
  relays get traffic. Two small trust roots
  ([overview](https://arxiv.org/pdf/2004.09583)).
- A **single** compromised DirAuth can equivocate and feed a targeted
  client a malicious consensus undetectably
  ([Luo et al. 2025](https://arxiv.org/abs/2503.18345)).
- Onion services have **no PKI and no naming**: a v3 address is a raw
  ed25519 pubkey, descriptors sit in a snoop-prone HSDir DHT (address
  harvesting, intro-point DoS), and users paste 56-character blobs or buy
  commercial naming certificates.

The fillable gap is **identity and authenticity**, not circuits and not
bandwidth measurement. A fully-synced zclassic23 node already holds a
globally consistent, censorship-resistant, PoW-committed bulletin board
that every other node agrees on byte-for-byte. Prior art (Antiblok,
SmartMeasurer, TorCoin) bolted this onto foreign smart-contract chains;
nobody has done it natively — same binary, own chain, own overlays.

## The primitive: write-once master-key anchoring

**On-chain (rare, tiny):** an identity's master ed25519 key (32 bytes),
anchored once. The only further on-chain writes are rotation and
revocation.

**Off-chain (frequent, free):** signed documents of the form
`{version, master_pubkey, seq, expiry, body, signature}`. Consumers verify
the signature against the chain-anchored key and enforce a monotonic
sequence number. Rotating a document costs a signature, not a transaction.

This mirrors Tor's internal design — an offline master identity key with
medium-term signing keys — with the root of trust moved from the address
string to the chain.

### Anti-enumeration: blinded record keys

Chain data is public, so naive on-chain service records would let any
observer enumerate all services (worse than Tor v3, where you cannot even
find a descriptor without knowing the address). Service-bound records are
therefore keyed by a **blinded key**:

```
blinded = SHA3-256("zid-blind" ‖ master_pubkey ‖ period)
```

following Tor v3's blinded-pubkey pattern. Only someone who already knows
the master key (i.e. knows the service) can derive the record key.

### On-chain anchor formats

- **v1 (no new overlay):** ZNAM `SET_TEXT key="zid" value=<64-hex pubkey>`
  binds a human name to a master key using the existing registry
  (`lib/znam/include/znam/znam.h`; text values ≤128 chars). Zero new
  protocol — usable the day the client codec ships.
- **v2 (dedicated `ZID` overlay):** lokad `ZID\0`, commands
  ANCHOR/ROTATE/REVOKE carrying raw 32-byte keys — for pseudonymous
  identities that don't want a name, and for machine-readable projections.
  Same push-framing as ZNAM/ZSLP via
  `lib/script/include/script/op_return_push.h`, inside the 223-byte
  standardness cap (`lib/script/include/script/standard.h:33`).

## Applications, in build order

### A1 — Onion service descriptors (flagship)

The service's master key is anchored on-chain (v1: via ZNAM). Current
introduction-point descriptors are signed documents served from the
content-addressed ZCODE swarm (`lib/vcs/src/package_swarm.c`,
wired in `lib/net/src/msgprocessor_zcode_swarm.c`). Clients fetch the blob
from any peer and verify it against the chain-anchored key.

- Kills HSDir address harvesting: no relay ever sees descriptors for
  services it doesn't serve; blinded keys prevent chain-side enumeration.
- Rotation is free: new `seq`, new signature, no transaction.
- Availability: the service itself seeds its descriptor; optional paid
  pinning (A4 rails) covers long-lived services later.

### A2 — ZNAM naming for onion services (already built — promote)

`ZNAM_TYPE_ONION` (znam.h:33) + `/n/<name>` resolution is a chain-verified,
CA-free naming layer Tor has never had. Surface it: name-based links on
the onion site, docs, resolver UX. See the ZClassicDNS contract in
`docs/spec/power-node-contract.md`.

### A3 — ZDIR relay directory (an application, not the headline)

A relay is a master key that signs endpoint announcements:

- **On-chain:** REGISTER/DEREGISTER/TRANSFER anchor the relay's identity
  key and owner (first-input P2PKH signer, ZNAM convention). Nothing else.
- **Off-chain:** endpoint and bandwidth updates are signed gossip
  announcements — exactly how Tor relays republish descriptors to the
  DirAuths today. Liveness never touches the chain; it comes from signed
  heartbeats plus each node's own reachability probing.
- **Selection:** per-client derivation `SHA3-256(block_hash ‖ client_key)`
  — per-client reproducible, globally diversified (a single deterministic
  global guard set would be an anonymity monoculture). Weighted by
  **seniority** (registration height), capped per owner address.

### A4 — Incentives (last, only if A1–A3 earn it)

A bandwidth-credit ZSLP token settling over the existing batched daily
SEND rails (`app/services/include/services/zslp_command_service.h`).
Weight must come from seniority and measurement, never raw stake: paying
for relays at scale attracts Sybils — in 2023 the Tor Project had to
remove relays tied to a crypto scheme
([report](https://securityaffairs.com/154535/digital-id/tor-project-removed-relays.html)).
Trustless bandwidth measurement is unsolved research (FlashFlow, TorCoin);
this phase is explicitly gated on A1–A3 proving the substrate.

## Economics: low cost, high use, no spam

The rule: **the chain is a land registry, not a message bus.** Cost scales
with influence sought, not with existence.

**Where ZCL fees are required (one tx each):**

| Action | Frequency |
|---|---|
| ZNAM register / renew / transfer / set_text (incl. `zid` anchor) | per name event |
| ZDIR register / deregister / transfer | per relay identity event |
| ZSLP genesis / mint / send | settlement only — rewards batch to ~1 tx/day for all contributors |
| Descriptor master-key anchor | once per service, ever |

**Never required:** liveness heartbeats, descriptor blobs, endpoint
updates, zmsg off-chain messages, zgame traffic, file chunks, bandwidth
receipts, swarm WANT/DATA, addr gossip.

**Verified constants** (`lib/validation/include/validation/main_constants.h`):
`DEFAULT_MIN_RELAY_TX_FEE = 100` zatoshis, `MAX_BLOCK_SIZE = 2,000,000`
consensus / `DEFAULT_BLOCK_MAX_SIZE = 200,000` mining policy,
`ZCL_FINALITY_DEPTH = 10` (see below). At 150 s blocks: 576 blocks/day,
~115 MB/day soft capacity.

**Scale math:** 1,000 relays × ~27 txs/year × ~200 B ≈ 5 MB/year. 10k ZNAM
ops ≈ 2.5 MB/year. Even 100× growth is a rounding error against a chain
that reached ~10 GB in 8+ years. Bulk data lives in quota'd, pruneable
local stores (ZCODE CAS), never on-chain.

**Spam resistance without pricing out users:** fees alone don't stop spam
on a cheap chain (1M min-fee txs ≈ 0.1 ZCL). The defense is asymmetry:

- *Seniority weighting* — 10,000 freshly-registered relays buy ~zero
  selection weight; aging in requires sustained renewals. Cheap to be
  real, expensive and slow to fake.
- *Per-owner influence cap* — N relays from one address count once.
- *Deferrability* — directory writes aren't latency-critical; block-space
  attacks merely delay them while the attacker burns fees daily.
- *Projection quotas* (policy, not consensus) — max live relays per
  owner, update rate limits for ranking. Retunable without a fork.

The loop closes nicely: the identity layer's steady small fee stream funds
the very PoW that secures it.

## Finality, forks, and netsplit monitoring

`ZCL_FINALITY_DEPTH = 10` with deep-reorg refusal (`reorg_is_allowed` /
`height_is_immutable`; `main_constants.h:33-34`) means a partition
surviving >10 blocks on both sides **never reconverges**. Rules:

- **Provisional < 10 confs ≤ final.** Anchors feed hints immediately but
  confer no influence until final — kills flapping from shallow reorgs.
- **10 confs is anti-flapping, not trust.** An attacker with ~10 blocks of
  private hashpower can rewrite the recent window; influence comes only
  from seniority (hours–days), never recency.
- **Netsplit detection is directory-safety infrastructure**, wired into
  the existing `app/conditions` + supervisor liveness tree:
  1. peer tip disagreement at same-or-greater height,
  2. block-arrival rate divergence vs difficulty (minority-side signal),
  3. tip staleness beyond expected variance.
- On suspected split: **degraded mode** — new anchors gain no influence,
  pre-split final entries keep working, discovery falls back to addr
  gossip + onion seeds, and status surfaces `SUSPECTED_NETSPLIT` as a
  named blocker, never a silent halt.

## Risks and honest limits

- **Operator privacy is the deepest cost.** OP_RETURN can't ride a Sapling
  output, so anchoring is transparent: a t-address with a traceable
  funding history is permanently bound to "I operate this service/relay."
  Mitigations: fund via shielded → fresh transparent hop, one address per
  identity. The permanence is intrinsic and must be disclosed.
- **Key compromise** requires on-chain rotation — a tx, plus loss of
  seniority for influence-weighted applications. Master keys belong
  offline, like Tor's.
- **Availability vs Tor's 6 HSDir replicas** — a self-seeded descriptor
  has one seeder until pinning incentives exist (A4).
- **Directory soundness = ZClassic's PoW security** — a small chain;
  say so. The identity layer is advisory: independent discovery roots
  (hardcoded seeds, addr gossip) always remain.
- **Bandwidth measurement without trusted measurers is unsolved.**
- **Pruned nodes can't rebuild overlay projections locally** — a future
  ROM-checkpoint commitment of projection digests is a sealed-`core/`
  change, owner-gated, Phase 4+.
- This does not replace Tor's circuits, and Phase 4 incentives must be
  designed against mercenary-Sybil economics (2023 incident above).

## Phases

1. **Phase 1 — `lib/zid` codec (pure library, no networking).** Blinded
   key derivation (`SHA3-256("zid-blind" ‖ pubkey ‖ period)`), signed
   document encode/decode/verify with monotonic-seq rule, tests. No
   behavior change anywhere.
2. **Phase 2 — descriptor application.** ZNAM `zid` anchoring convention,
   descriptor blob served/fetched via the ZCODE swarm, onion-site
   surface, resolver UX for A2.
3. **Phase 3 — ZDIR as application.** Registration anchoring, signed
   endpoint gossip, seniority-capped per-client selection, netsplit
   degraded mode.
4. **Phase 4 — incentives.** Bandwidth receipts research, ZSLP credit
   token, batched settlement; gated on A1–A3.

## Concrete files

Existing (built on): `lib/znam/include/znam/znam.h`,
`lib/zslp/include/zslp/slp.h`,
`lib/script/include/script/op_return_push.h`,
`lib/script/include/script/standard.h` (223 B cap),
`app/models/src/op_return_index.c`,
`lib/vcs/src/package_swarm.c` + `lib/vcs/include/vcs/package_swarm.h`,
`lib/net/src/msgprocessor_zcode_swarm.c`,
`lib/net/src/onion_ratelimit.c`, `lib/net/src/onion_service.c`,
`lib/net/src/v2_transport.c` + `lib/session/src/noise_handshake.c`,
`lib/validation/include/validation/main_constants.h` (fee/finality/size),
`core/chainparams/src/checkpoints.c` (baked trust anchor),
`docs/spec/power-node-contract.md` (ZClassicDNS + onion gateway).

Proposed (Phase 1): `lib/zid/include/zid/zid.h`, `lib/zid/src/zid.c`,
`lib/test/src/test_zid.c`.

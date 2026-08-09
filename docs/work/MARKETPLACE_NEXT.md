# Marketplace next steps — ordered checklist (2026-08-08)

Context: the metaverse MVP lane is done and pushed (`d00d5a7fd`,
`make metaverse-score` = 100/100, `make metaverse-verify` 7/7). The owner
asked "what's next?" and picked these three tracks, in this order.

Subordinate to [`FORWARD_PLAN.md`](./FORWARD_PLAN.md): this does **not**
reorder the v1 node-sovereignty lane (C3/C5/C6/C8). It is the
metaverse/marketplace parallel lane opened by the 2026-08-08 owner
directive. Boundaries inherited from [`MARKETPLACE_PLAN.md`](./MARKETPLACE_PLAN.md):
no consensus change, ZC23 stays simulation-only, live money fail-closed,
settlement is ordinary opt-in transactions with plan/commit + fee preview.

## Phase A — two-laptop real test (owner + one helper machine)

Everything so far was proven with regtest daemons on one host. This phase
is two real machines finding each other over Tor and trading a file.

- [ ] A1. Guide written: `docs/TWO_LAPTOP_MARKET_TEST.md`
- [ ] A2. Both machines build the real-Tor binary (`vendor/tor` submodule + `libtor.a`)
- [ ] A3. Node A boots with `-tor`, serves its onion, shares the address
- [ ] A4. Node B discovers/connects to A (`/directory.json` or `-addnode`)
- [ ] A5. A offers a file; B sees it in the market listing
- [ ] A6. B buys: payment tx verifies, chunks unlock, download completes
- [ ] A7. Friction log → each snag filed and fixed

## Phase B — file-market settlement wiring (code)

The marketplace trade is wired end-to-end: seller `app market offer`
(sign → persist → content-bind → gossip) and buyer `app market purchase
plan/commit/status/retrieve` (payment → chain-verified claim →
authorize-before-read chunk delivery → root re-derivation). The legacy
`zmarket_offer`/`zmarket_buy` RPCs stay contained stubs by design.

**2026-08-09 update — B1 finding flipped Phase B:** the buyer side was
already wired end-to-end (`app market purchase plan/commit/status/retrieve`,
payment-gated chunk delivery, restart-safe retrieval). The genuine gap was
the seller side. B2 (landed in `c4bf1cb40` + `aff7ecf12`) closed it:
`app market offer` seals/persists/binds/announces a signed paid offer
(fail-closed without `-externalip` + file-service port), and the purchase
reverse-mapping gate covers the new leaf.

- [x] B1. Map the exact unwired seams — done; buyer pipeline already
  shipped, seller offer creation was the one real gap
- [x] B2. Seller offer wired end-to-end: `app market offer`
  plan/commit (content-addressed idempotent), sealed offer, content
  binding, `zfileoffer` origin flood; 11 new tests; pushed
- [ ] B3. Two-node regtest acceptance script (same shape as
  `tools/dev/zcode_dht_acceptance.sh`): seller offers, buyer pays,
  bytes arrive and re-derive the content root — IN PROGRESS
- [x] B4. `make lint` + pre-push CI green on the pushed tree (919 ran,
  0 failed); docs updated (`FILE_MARKET_PROTOCOL.md`, two-laptop runbook,
  cookbook)
- [ ] B5. **Onion-routed chunk delivery** — design record:
  [`MARKET_ONION_DELIVERY.md`](./MARKET_ONION_DELIVERY.md) (offer v2 with
  `endpoint_type=onion` + 32-byte onion pubkey, `/market/chunk` onion route
  reusing the authorize-before-read gate, GET-hex transport, fail-closed
  stub policy, `/directory.json` clearnet suppression). Today the offer
  carries the seller's clearnet `peer_ip:peer_port` and the buyer connects
  directly, exposing both IPs

## Phase C — ZC23 design (owner decision first, then code)

ZC23 is simulation-only by design. Before any real distribution or
"Proof of Participation" exists, the rules get written down and the owner
picks them. Nothing here touches consensus; mining/distribution stays
simulation until the owner explicitly promotes it.

- [ ] C1. Options doc: distribution shape, participation rules, supply,
  earning ZC23 for hosting code/content
- [ ] C2. Owner picks the rules
- [ ] C3. Implementation plan written (simulation-first, no consensus path)

## Standing rules for this checklist

- Copy-prove before live: every recovery or settlement path is proven on
  regtest/a datadir copy first.
- `make lint` + `make test-parallel` + pre-push CI before every push; no
  `--no-verify` on a red gate.
- `vendor/tor` submodule stays intentionally dirty; never commit it.

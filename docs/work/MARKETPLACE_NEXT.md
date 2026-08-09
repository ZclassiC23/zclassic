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

`zmarket_buy` is not wired end-to-end today: chunk unlock is gated on a
mempool-verified payment txid (`handle_zfilepay`, `msgprocessor.c`), but
the buy RPC → payment → transfer glue is missing.

- [ ] B1. Map the exact unwired seams (buy command → payment tx build →
  mempool verify → chunk unlock → download) — one page, named files/lines
- [ ] B2. Wire the buy flow end-to-end: plan/commit with fee preview,
  fail-closed on every custody gate
- [ ] B3. Two-node regtest acceptance script (same shape as
  `tools/dev/zcode_dht_acceptance.sh`): seller offers, buyer pays,
  bytes arrive and re-derive the content root
- [ ] B4. `make lint` + `make test-parallel` + pre-push CI green; docs updated
- [ ] B5. **Onion-routed chunk delivery** — today the offer carries the
  seller's file-service endpoint as clearnet `peer_ip:peer_port` and the
  buyer connects directly, exposing the seller IP to the buyer (and the
  buyer's connect to the seller). For the "buyer/seller IP never exposed"
  bar, the delivery endpoint must be reachable via the onion service
  (onion address in the offer, delivery requests routed through the
  embedded Tor, same authorize-before-read gate unchanged)

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

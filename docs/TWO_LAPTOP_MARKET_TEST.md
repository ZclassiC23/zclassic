# Two-laptop market test — buyer and seller over Tor

The goal: two real machines, each running one zclassic23 node, find each
other and trade one file — the seller announces a paid offer, the buyer
pays, the bytes arrive and re-derive the content root. Neither machine
needs open ports; the nodes rendezvous over Tor.

Status honesty (2026-08-08): the **buyer** pipeline (purchase
plan/commit/status/retrieve, payment-gated chunk delivery) is shipped.
The **seller** `app market offer` command is being wired in Phase B2 of
[`docs/work/MARKETPLACE_NEXT.md`](./work/MARKETPLACE_NEXT.md) — until that
lands, step 5 fails closed by design. Run steps 1–4 today; run 5–7 once
`app market offer` no longer returns PLANNED_COMMAND.

## 0. What you need

- Two Linux machines ("A" = seller, "B" = buyer). Same-build checkout on
  each. Nothing else: no open ports, no VPS, no DNS.

## 1. Build (both machines)

```bash
git clone https://github.com/ZclassiC23/zclassic && cd zclassic
make setup          # vendored deps + git hooks (first run needs network)
make tor-full       # REAL Tor — without this, -tor runs the stub (no onion)
make -j"$(nproc)"   # build/bin/zclassic23
```

## 2. Boot both nodes with Tor

On each machine:

```bash
build/bin/zclassic23 -tor -datadir=$HOME/.zcl-market-test
```

The onion address appears in:

```bash
build/bin/zclassic23 core status    # → health.checks.onion_address
```

Tor bootstrap takes ~10 s. If `onion_address` is null and the log says
Tor is disabled, you built the stub — go back to `make tor-full`.

## 3. Connect B → A

Each node serves `/directory.json` on its onion. Point B at A (either
form works; clearnet addnode is fine for the first run — see the privacy
note in step 7):

```bash
# on B, add to the boot flags or use the running node:
-addnode=<A-onion-or-ip>:8033
```

Confirm on B: `build/bin/zclassic23 core network peers` lists A.

## 4. Free coins for the test (regtest variant)

For a no-real-money first run, boot both with `-regtest` instead and mine
spendable coins on B (regtest mining is CPU-instant):

```bash
ADDR=$(ZCL_DATADIR=$HOME/.zcl-market-test build/bin/zclassic23 getnewaddress)
ZCL_DATADIR=$HOME/.zcl-market-test build/bin/zclassic23 generatetoaddress 101 "$ADDR"
```

On mainnet the same flow works with real ZCL in B's shielded wallet —
every money step is plan/commit with a fee preview, nothing moves without
an explicit `confirm:true` commit.

## 5. Seller: announce the file (machine A)

```bash
printf '%s' '{"filepath":"/home/alice/demo.bin","price_per_mb_zat":1000,"confirm":true}' \
  | build/bin/zclassic23 app market offer --input=-
```

This builds the content manifest, signs the self-authenticating offer
(ed25519, network-bound), persists it, registers the content binding so
chunks can be served, and floods `zfileoffer` to every peer.

## 6. Buyer: find it, pay for it, download it (machine B)

```bash
build/bin/zclassic23 app market list        # → offers; note offer_id + price

printf '%s' '{"wallet_scope":"dev","offer_id":"<64hex>","source_address":"<B-owned-address>","chunk_start":0,"chunks_paid":<num_chunks>,"idempotency_key":"laptop-test-001"}' \
  | build/bin/zclassic23 app market purchase plan --input=-
# → fee preview + plan_id; nothing has moved

printf '%s' '{"wallet_scope":"dev","plan_id":"<64hex>","confirm":true}' \
  | build/bin/zclassic23 app market purchase commit --input=-
# → shielded payment tx broadcast; seller notified (zfilepay)

printf '%s' '{"plan_id":"<64hex>"}' \
  | build/bin/zclassic23 app market purchase status --input=-
# → payment reconciliation (PENDING → CONFIRMED)

printf '%s' '{"plan_id":"<64hex>","destination_path":"/home/bob/demo.bin"}' \
  | build/bin/zclassic23 app market purchase retrieve --input=-
# → per-chunk signed requests, each authorized against the confirmed
#   payment before the seller reads a byte; full-file root re-derived
```

Success = `sha3(demo.bin)` on B matches the offer `root_hash`, and
`app market purchase status` shows the download complete.

## 7. Privacy note (be honest with yourself)

- P2P rendezvous over Tor: yes — steps 2–3 hide the nodes' IPs from each
  other and from the network when you use the onion (not clearnet
  `-addnode`).
- Chunk delivery: today the offer carries the seller's file-service
  endpoint as `peer_ip:peer_port`, and the buyer connects to it directly.
  **On a clearnet endpoint that exposes the seller's IP to the buyer.**
  Routing delivery over the onion service is the named follow-up seam in
  MARKETPLACE_NEXT.md; until it lands, use this test on machines whose
  IPs may see each other, or through a network namespace/VPN you control.

## 8. Friction log

Every snag — unclear error, missing hint, surprising refusal — goes in
the checklist's A7 item: file it in `docs/HANDOFF.md` or straight into a
fix. The UX rule for this test: a first-time operator should never have
to read source to complete steps 1–6.

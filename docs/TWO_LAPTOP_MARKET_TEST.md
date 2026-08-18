# Two-laptop market test — buyer and seller over Tor

The goal: two real machines, each running one z23 node, find each
other and trade one file — the seller announces a paid offer, the buyer
pays, the bytes arrive and re-derive the content root. Neither machine
needs open ports; the nodes rendezvous over Tor.

Status honesty (2026-08-09): the full trade is shipped end-to-end —
seller `app market offer` (sign → bind → announce), buyer purchase
plan/commit/status/retrieve, and (B5) onion-routed delivery. The seller
endpoint comes from one of two places: boot A with `-tor` (real-Tor
build) and the offer commits its onion endpoint automatically — nothing
else needed; or pass `-externalip=<A-public-ip>` for an explicit
clearnet endpoint. With neither, step 5 refuses ENDPOINT_UNKNOWN rather
than signing a guess.

## 0. What you need

- Two Linux machines ("A" = seller, "B" = buyer). Same-build checkout on
  each. Nothing else: no open ports, no VPS, no DNS.

## 1. Build (both machines)

```bash
git clone https://github.com/z23c/z23 && cd z23
make setup          # vendored deps + git hooks (first run needs network)
make tor-full       # REAL Tor — without this, -tor runs the stub (no onion)
make -j"$(nproc)"   # build/bin/z23
```

## 2. Boot both nodes with Tor

The wallet must be encrypted at rest or the seller offer commit refuses
(`SELLER_KEY_UNAVAILABLE`) and the buyer money gate refuses — so both
nodes boot with a wallet-passphrase credential (once per machine):

```bash
mkdir -p -m 700 $HOME/.zcl-market-cred
printf '%s\n' 'pick-a-test-passphrase' > $HOME/.zcl-market-cred/wallet-passphrase
chmod 600 $HOME/.zcl-market-cred/wallet-passphrase

env CREDENTIALS_DIRECTORY=$HOME/.zcl-market-cred \
  build/bin/z23 -tor -datadir=$HOME/.zcl-market-test
```

Gotcha: the passphrase is the file's exact bytes **including the
trailing newline** — a manual `walletunlock` must pass `"pass\n"`, and a
*wrong*-passphrase unlock wipes the daemon's RAM keystore until the
correct unlock reloads it.

The onion address appears in:

```bash
build/bin/z23 ops state --subsystem=explorer   # → data.state.onion_address
```

(`core status` does not carry the onion address; the explorer dump does.)

Tor bootstrap takes ~10 s. If `onion_address` is null and the log says
Tor is disabled, you built the stub — go back to `make tor-full`.

## 3. Connect B → A

Each node serves `/directory.json` on its onion. Point B at A
(clearnet addnode is fine for the first run — see the privacy
note in step 7):

```bash
# on B, add to the boot flags or use the running node:
-addnode=<A-ip>:8033
```

Clearnet only: `-addnode=<A-onion>` does NOT work — the addnode path
resolves the host with getaddrinfo, which cannot resolve `.onion`
("Failed to resolve addnode" in the log). For an onion-only rendezvous
use the `/directory.json` seed mechanism instead; for a first run use
A's clearnet IP (the trade bytes still ride Tor — see step 7).

Confirm on B: `build/bin/z23 core network peers` lists A.

## 4. Free coins for the test (regtest variant)

For a no-real-money first run, boot both with `-regtest
-regtestshielded` instead (shielded active from genesis — without the
second flag mining silently returns `[]`), then mine spendable coins on
B in chunks of 5 (regtest mining is CPU-instant; a single big call can
return an empty batch):

```bash
ADDR=$(ZCL_DATADIR=$HOME/.zcl-market-test build/bin/z23 getnewaddress)
n=101; while [ $n -gt 0 ]; do c=5; [ $n -lt 5 ] && c=$n
  ZCL_DATADIR=$HOME/.zcl-market-test build/bin/z23 generatetoaddress $c "$ADDR"
  n=$((n-c)); sleep 1; done
```

On mainnet the same flow works with real ZCL in B's shielded wallet —
every money step is plan/commit with a fee preview, nothing moves without
an explicit `confirm:true` commit.

## 5. Seller: announce the file (machine A)

```bash
printf '%s' '{"filepath":"/home/alice/demo.bin","price_per_mb_zat":1000,"confirm":true}' \
  | build/bin/z23 app market offer --input=-
```

This builds the content manifest, signs the self-authenticating offer
(ed25519, network-bound), persists it, registers the content binding so
chunks can be served, and floods `zfileoffer` to every peer. The seller
signs the endpoint it actually knows: booted with `-tor` (and no
`-externalip`) the offer commits its **onion endpoint** automatically —
the buyer's chunks ride Tor (see step 7); `-externalip` supplies a
clearnet IP instead (buyer connects to `peer_ip:peer_port` directly).
With neither, the commit refuses with ENDPOINT_UNKNOWN instead of
signing a guess. Calling without `confirm:true` first returns the
non-mutating plan (root hash, size, exact total) plus the commit input.

## 6. Buyer: find it, pay for it, download it (machine B)

```bash
build/bin/z23 app market list        # → offers; note offer_id + price
```

Heads-up (community content moderation): every node boots on the
`general-audience.v1` default profile, so a freshly gossiped offer
arrives `unreviewed` and is **hidden from the default list** — with an
honest `hidden_count`. To see it, either take the open view
(`app market list --input '{"profile":"open"}'`) or mark it on YOUR
node only (`app market moderation review set --input
'{"offer_id":"<64hex>","review_state":"reviewed_ok"}'`). Nothing is
ever banned or deleted network-wide — this is your node's own view of
the same gossip (see §10).

```bash
printf '%s' '{"wallet_scope":"dev","offer_id":"<64hex>","source_address":"<B-owned-address>","chunk_start":0,"chunks_paid":<num_chunks>,"idempotency_key":"laptop-test-001"}' \
  | build/bin/z23 app market purchase plan --input=-
# → fee preview + plan_id; nothing has moved

printf '%s' '{"wallet_scope":"dev","plan_id":"<64hex>","confirm":true}' \
  | build/bin/z23 app market purchase commit --input=-
# → shielded payment tx broadcast; seller notified (zfilepay)

printf '%s' '{"plan_id":"<64hex>"}' \
  | build/bin/z23 app market purchase status --input=-
# → payment reconciliation (PENDING → CONFIRMED)

printf '%s' '{"plan_id":"<64hex>","destination_path":"/home/bob/demo.bin"}' \
  | build/bin/z23 app market purchase retrieve --input=-
# → per-chunk signed requests, each authorized against the confirmed
#   payment before the seller reads a byte; full-file root re-derived
```

Success = `sha3(demo.bin)` on B matches the offer `root_hash`, and
`app market purchase status` shows the download complete.

## 7. Privacy note (be honest with yourself)

- P2P rendezvous over Tor: yes — steps 2–3 hide the nodes' IPs from each
  other and from the network when you use the onion (not clearnet
  `-addnode`).
- Chunk delivery over Tor: yes (2026-08-09, B5). When the seller boots
  with `-tor` (real-Tor build) and does NOT pass `-externalip`, the
  offer commits as endpoint_type=onion (wire v2) and the buyer retrieves
  chunks through the seller's onion service — neither side's IP appears
  on the delivery leg. On a stub build or unready Tor the commit refuses
  instead of silently downgrading. Passing `-externalip` is the explicit
  public-clearnet opt-in (v1 behavior, including publishing your IP in
  `/directory.json`).
- What still shows: offer gossip rides plaintext P2P (the first hop that
  injects an offer hints at the seller), and timing/slice sizes are
  visible to Tor guards. The full non-goals list is in
  `docs/work/MARKET_ONION_DELIVERY.md`.

## 8. Friction log

Every snag — unclear error, missing hint, surprising refusal — goes in
the checklist's A7 item: file it in `docs/HANDOFF.md` or straight into a
fix. The UX rule for this test: a first-time operator should never have
to read source to complete steps 1–6.

## 9. Proven two-server regtest run (2026-08-09)

The full trade was executed between two real servers (seller A on a
remote VPS, buyer B on a local server) with delivery witnessed in both
`tor.log` files. Final state: payment confirmed on-chain at height 207,
seller decrypted its exact Sapling note, buyer retrieved the file in
~10 s and the SHA-256 matched the seller's fixture byte-for-byte; the
pre-confirmation retrieve was refused by name (`DELIVERY_NOT_READY`).

Recipe differences from the laptop path above:

- **Boot flags**: both nodes need `-regtest -regtestshielded -tor
  -packagehost=0 -operator-lane=dev -wallet-no-phrase-backup
  -nobgvalidation -nolegacyimport -showmetrics=0`. Without
  `-regtestshielded`, `generatetoaddress` silently returns `[]` (Sapling
  must be active from genesis).
- **Wallets must be encrypted at rest** or the offer commit refuses
  `SELLER_KEY_UNAVAILABLE`: boot with `CREDENTIALS_DIRECTORY` pointing at
  a dir holding a mode-600 `wallet-passphrase` file. The passphrase is
  the file's exact bytes **including the trailing newline** — if you
  call `walletunlock` by hand, pass `"pass\n"` with the newline.
  Warning: a *wrong-passphrase* `walletunlock` wipes the daemon's RAM
  keystore (it reloads on the correct unlock); `validateaddress
  ismine=false` / `dumpprivkey "not known"` are the symptoms.
- **Peering when B is behind NAT**: two SSH tunnels reproduce the
  loopback acceptance topology exactly —
  `ssh -L 20441:127.0.0.1:20440 -N <A>` (B dials `127.0.0.1:20441`) and
  `ssh -R 20451:127.0.0.1:20450 -N <A>` (A dials `127.0.0.1:20451`).
  Both nodes must own an outbound link; a listen-only node never
  reaches `at_tip` and the seller's paid-chunk gate fails closed.
- **Sequence**: mine 101 to B in chunks of 5 → A syncs → restart B so
  the forward-folded coins set stamps authority → re-link both ways →
  `getnewaddress` top-up + `z_getnewaddress` on A → offer (plan, then
  `confirm:true`) → gossip → purchase plan/commit (retry on
  `MONEY_STATE_NOT_CURRENT` — it is transient tip-freshness, not a
  refusal) → wait A mempool → mine 1 → poll status `confirmed` →
  retrieve.
- **Mining maturity**: regtest coinbase matures after 100 blocks; mine
  to an address created in the *current* session (post-unlock), or the
  commit's `source address is not a wallet spending key` /
  `Insufficient funds from specified address` pair will send you
  debugging the keypool.

## 10. Moderation: two laptops, two opinions about the same file

Per-node listing moderation means the buyer and seller can legitimately
disagree about the same offer: each node's own profile decides what its
`app market list` shows, and nothing is ever deleted or banned
network-wide. On the laptops above, try it by hand — B boots on the
immutable `general-audience.v1` default, so a fresh offer ingests as
`unreviewed` and is hidden from B's default list (watch the honest
`hidden_count`); `app market list --input '{"profile":"open"}'` is the
explicit opt-in that shows everything annotated with its local
`review_state`; `app market moderation profile set` (plan, then commit
with the minted `plan_token`) switches B's node default to `open-view`;
`app market moderation review set` is B's own curation mark
(`reviewed_ok` shows, `sensitive` hides). A's view is unchanged
throughout — the mark never leaves B.

The one-command proof is the acceptance:

```bash
make test-market-moderation-acceptance
```

It runs the whole disagreement on two loopback regtest daemons (no Tor
needed — moderation is transport-independent): same signed offer,
different profiles, hidden ≠ rejected (one gossip-stored row on both
nodes), and the signed wire bytes byte-identical on both sides after
every moderation action. No payment is made — this is a visibility
proof, not a trade.

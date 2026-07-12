> **ARCHIVED / SUPERSEDED.** Superseded by docs/work/FORWARD_PLAN.md (THE plan); sovereign-contracts follow-on work tracks there now. See `docs/work/ROADMAPS.md` for the live roadmap index. Kept for history — do not act on this as current.

# zclassic23 Sovereign Service — Strong/Fast Node + Parity-Safe Smart-Contract Overlays

*Design of record — 2026-07-09. Lead architect's calls, not an options menu. Every claim traces to source read fresh; every lever is P2P-policy / storage-scheduling / read-model, none touches consensus (`docs/CONSENSUS_PARITY_DOCTRINE.md` §46-48). Verify file:line before editing — heights/commits rot.*

---

## 1. North star

One ~15 MB C23 binary is the operator's entire sovereign surface: a strong, fast, never-silently-stuck ZClassic full node underneath, and on top of it an **integrated identity-transport-contract stack where one ZNAM name is the operator's whole identity** (coin addresses + onion + pubkey + contract endpoints + content hashes), **the embedded onion is the transport every service is reachable over** (no clearnet, no DNS), **and the chain is the only settlement authority** — every contract app (escrow, atomic swap, name-addressed payments, shielded on-chain messaging, file market, token DAO) settles on a primitive `zclassicd` already accepts, coordinated fast over onion/P2P but authoritative only when its on-chain anchor confirms. The load-bearing invariant is the node's own: *derive every fact by folding a replayable log against your own crypto checkpoints; never serve an unproven value; never halt without naming the exact block and reason.* Nothing new is ever validated — everything is **interpreted** off frozen primitives, which is what keeps the entire overlay stack bit-for-bit parity-safe. One binary, one onion, one name per operator.

---

## 2. Strong & fast node — the foundation everything sits on

This is the floor. The overlays are worthless on a node that drifts slower every week or stalls under a long outage. Two of these defects are the exact "resource growth over months" class the project doctrine warns about — they don't crash, they quietly tax the node forever. **Fix these before, or in parallel-but-ahead-of, any overlay work.** None is on the v1/MVP bit-for-bit sync + soak critical path in a way that conflicts — they *strengthen* the soak.

Already-fixed, do NOT re-propose: the 76% pprev-walk (fast path `active_chain_extend_window_have_data`, `chainstate.c:549-589`, commit `b1c47d1d9`); the phashBlock UAF; the lock-order ABBA class; the window-extend hash-split wedge (`bde617a7e`). WAL + `synchronous=NORMAL`/`OFF`-during-IBD is already sensible (`progress_store.c:63-64,330-331`). Peer bootstrap fallback tiers exist (`connman.c:244-390`).

| # | Work item | File / function | Expected gain | Safety | Effort |
|---|-----------|-----------------|---------------|--------|--------|
| **S1** | **Make `reducer_frontier_compute_hstar` incremental.** Cache per-log `h_contiguous` watermark (6 values); finalize only scans `[last_h_contiguous+1, new_cursor)`, not `[anchor+1, cursor)`. Invalidate to `min(cache, rewind_height)` on reorg (same refresh path already fires there). | `reducer_frontier.c:240-291` (`log_contiguous_prefix`), `:584-` ; callers `tip_finalize_stage.c:55-67,75,305,634,768` | Turns O(height−anchor), **growing forever**, into flat O(batch) — for both live node and mint. Live gap already ~118k blocks over the fixed anchor 3,056,758 and climbing daily. Measured cost on mint fold: 168 ms/finalize @ h=272k → seconds/block near tip. **The single biggest win** — it's the only defect that gets worse every month with no ceiling but a manual rebuild. | Zero — internal bookkeeping of a non-authoritative H* cache; `ok=1` prefix logic unchanged. | S-M |
| **S2** | **Make `utxo_mirror_sync` delta-apply.** Replace `DELETE FROM utxos` + full re-SELECT/reinsert with upsert/delete of only coins changed since the mirror cursor (reuse `utxo_apply_log` per-height deltas). Reserve the full wipe for a genuine row-count mismatch (real corruption). | `utxo_mirror_sync_service.c:215-` (`mirror_rebuild_from_coins_kv`), drift check `:442-453`; near-tip branch always taken (`NEAR_TIP_BLOCKS=200`, 5 s tick) | Turns O(total_UTXO_count) paid on **essentially every accepted block forever** into O(coins touched by that block). >1M UTXOs and growing — likely the largest steady-state CPU/IO tax on a healthy long-running node today. | Zero — `node.db.utxos` is an explicit rebuildable non-authoritative cache (`never-stuck-plan.md` residual #4). | M |
| **S3** | **Raise tail-stage batch/period during bulk catch-up** (the 50 blk/s ceiling). Process-global refold/IBD-gated override on `*_BATCH_PER_TICK` (100) and/or the 2 s tick, same pattern as `-mint-anchor-fast`'s crypto-skip flag; off outside refold/IBD so a normal boot stays byte-identical. **Prereq bug:** `stage_set_cursor`/`stage_set_named_cursor_if_behind` (`stage.c:510,551`) do unconditional `BEGIN IMMEDIATE` that nest-fails inside the reorg-rewind batch txn — make batch-aware (SAVEPOINT/RELEASE) first. | `staged_sync_supervisor.c:239-284`; `{script_validate,proof_validate,utxo_apply,tip_finalize}_stage.h` | Batch ~2000 w/ single batched commit (already WAL) pushes cold-sync and long-outage self-heal well past 50 blk/s. Becomes the next ceiling once S1/S2 land. Also a **strength** fix: today a weeks-offline node re-folds at fixed wall-clock/block with no burst mode — self-heal time scales linearly with outage length. | Zero — scheduling cadence only. | M (+ prereq) |
| **S4** | **Fix IBD in-flight ceiling mismatch.** Declared `DL_MAX_IN_FLIGHT_TOTAL_IBD=4096` but per-peer hard-clamped to 128 × `MAX_OUTBOUND_CONNECTIONS=8` = 1024 achievable (4× under). Raise per-peer during IBD or add body-fetch-only outbound slots. **Measure after S1-S3** — fold-side dominates until then; live node has 24 peers so 8×128 may already suffice. | `download.c:864-887`; `net.h:30`, `download.h:29` | Medium, and only once fold-side is no longer binding. Profile-first per project rule. | Zero — P2P policy. | S |
| **S5** | **Fast/slow-path counter on `active_chain_extend_window_have_data`.** Log fast (`:549-589`) vs slow (`:591-679`, O(map), fires only when `pindex_best_header` NULL/off-chain) hit counts via `zcl_state subsystem=reducer_frontier`. | `chainstate.c:512-680` | Guards a shipped 380×-class win against silent regression back to O(map) live stall. | Zero — observability. | Trivial |
| **S6** | **Bootstrap fallback-of-last-resort regression test.** Kill DNS + operator addrman file simultaneously; assert hardcoded chainparams onion-seed tier alone reaches first peers within a bounded time. `grep lib/test/src/test_connman*.c test_addrman*.c` first — may already exist. | `connman.c:244-390`, `addrman.c:1060` | Directly serves the STICKINESS prime invariant (zero-maintenance bootstrap). | Zero — test only. | S |

**Note on the uncommitted working-tree diff** (`git status`: `tip_finalize_stage.c`, `stage.c`): it is a narrower refold-only gate on the S1 call (skips it while `refold_in_progress()`) plus `ZCL_FOLD_TIMING` instrumentation. **Keep the instrumentation; supersede the refold-only gate with the full incremental cache (S1)** so the always-on live path is cured, not just the offline mint. Do not commit it as-is.

---

## 3. Parity-safe contract catalog

Every entry below is `parity_safe=true`: built only from primitives `zclassicd` already accepts (OP_RETURN+lokad, Sapling 512 B memo, standard P2SH/HTLC/multisig, **absolute** CLTV/nLockTime, P2P typed msgs, Tor onion). One line each: what the operator gets · anchor primitive · onion+ZNAM role.

### Identity & addressing (ZNAM-core — highest leverage, smallest effort)
- **Name-addressed payments** — pay a human name that resolves to the current address; recipient rotates keys freely · OP_RETURN ZNAM `SET_RECORD` (multi-coin) · ZNAM *is* the addressing layer; resolvable over onion for light clients.
- **Identity profile** — portable public profile (avatar/url/email/bio/enc-pubkey) under your name · ZNAM `SET_TEXT` k/v · rendered as an onion explorer page; profile = a set of your name's TEXT records.
- **Decentralized onion DNS directory** — reach `.onion` services by memorable name, no central DNS · ZNAM `SET_TEXT` onion record · onion hosts named services + serves the name→onion directory (`serve_directory_json`).
- **PGP-style key directory** — publish signing/encryption pubkeys under your name · ZNAM `SET_TEXT` pubkey record · keyserver lookup over onion.
- **Key rotation & delegation** — rotate the controlling key / delegate a subkey without moving the name · ZNAM `UPDATE` + `SET_TEXT` · current key + delegates served over onion.
- **Name transfer marketplace** — buy/sell a registered name for coins, atomically · ZNAM `TRANSFER` + HTLC/2-of-2 for payment atomicity · listings over onion; traded asset is the name itself.

### Custody & vaults (script primitives only)
- **m-of-n multisig vault** — no single key loss/theft moves funds · P2SH `OP_m..OP_n CHECKMULTISIG` · cosigners collect partial sigs over authenticated onion REST; ZNAM publishes the P2SH descriptor.
- **2-of-3 arbitrated escrow** — buyer+seller happy path, arbiter resolves disputes · 2-of-3 P2SH · arbiter countersigns over onion; arbiter identity/reputation via ZNAM.
- **Time-locked savings / inheritance vault** — coins unspendable until a height/date, optional heir release · absolute OP_CLTV P2SH branch · unlock height queryable over onion; heir address via ZNAM.
- **Dead-man's-switch inheritance** — owner reclaims anytime; after CLTV deadline heir sweeps; owner resets by re-spending · two-branch P2SH (owner immediate / heir behind CLTV) · countdown + claim tooling over onion; heir key via ZNAM.
- **Fidelity bond / anti-sybil stake** — prove time-locked real coins to back reputation/voting weight · OP_CLTV self-payment · bond registry over onion; bond bound to a ZNAM identity.

### Trustless trade (HTLC engine — flagship value)
- **Cross-chain HTLC atomic swap (BTC/LTC/DOGE)** — trustless coin-for-coin, same 97-byte dcrdex HTLC both legs · HTLC-in-P2SH · counterparties + contracts/secrets over onion; multi-coin redeem addrs from one ZNAM record. **ZCL leg + secret coordination in scope; foreign-chain leg is a counterparty/wallet step (see §7).**
- **Same-chain ZCL↔ZSLP token DEX swap** — atomic native-for-token trade, no custodian · HTLC + ZSLP `SEND` · onion orderbook; reputation/payout via ZNAM.
- **Hash-locked bounty** — anyone revealing a preimage claims the reward · HTLC preimage branch · bounty board + submissions over onion; poster/claimant via ZNAM.

### Coordination & crowdfunding
- **All-or-nothing assurance crowdfund** — pledges only pay if the goal is fully funded · `SIGHASH_ANYONECANPAY|ALL` pledge inputs · pledge aggregation + live tally over onion; campaign branded by ZNAM.
- **Dominant assurance contract** — refund-bonus if the goal fails, making pledging dominant · assurance tx + HTLC/CLTV bonus escrow · campaign + bonus-pool status over onion; identity via ZNAM.
- **Sealed-bid / Vickrey auction** — commit hashed bids, reveal after close · OP_RETURN commit + HTLC/multisig settlement · auctioneer hosts over onion; addressed by ZNAM.
- **Decentralized subscriptions** — recurring pay as nLockTime-staggered pre-signed txs (absolute dates) · pre-signed CLTV series · merchant mgmt over onion; payout addr survives rotation via ZNAM.

### Messaging & identity data (Sapling memo + onion)
- **Shielded on-chain 1:1 messaging (ZMSG)** — permanent private p2p messaging, each msg a shielded output whose memo carries ciphertext · Sapling 512 B memo w/ ZMSG magic · inbox over onion; correspondents by ZNAM name.
- **Encrypted dead-drop** — deliver a secret only the recipient can decrypt · Sapling memo (Jubjub DH + ChaCha20-Poly1305) · optional onion "drop waiting" ping; recipient z-addr via ZNAM.
- **Encrypted secret escrow (Shamir via memo)** — split a secret into shares, one encrypted per trustee, for social recovery · Sapling memo per trustee · recovery coordination over onion; trustees by ZNAM.
- **Web-of-trust attestations** — signed vouch/rate records building a trust graph · OP_RETURN attestation lokad · per-name scores over onion; attester+subject are ZNAM names.
- **Decentralized timestamping / notary** — prove a document existed at a height · OP_RETURN 32-byte commitment/Merkle root · proof issuance+verify over onion; notary by ZNAM.

### Content & marketplace
- **Pay-to-unlock content (file market)** — pay a ZCL tx, seller unlocks encrypted chunks on mempool-txid · P2P `zfile` + plain payment tx (mempool-txid gate) · catalog + chunk delivery over onion; storefront by ZNAM.
- **Censorship-resistant publishing** — tamper-evident content reachable by name · OP_RETURN content-hash + onion HTTP delivery · onion hosts content + directory; publication addressed by ZNAM (content hash in a TEXT record).

### Governance & data feeds
- **Token-weighted voting / DAO (ZSLP)** — holders vote weighted by snapshot balance, transparent tally · OP_RETURN ballot lokad + ZSLP snapshot · proposals/tally dashboard over onion; DAO by ZNAM.
- **Oracle / signed attestation feed** — publish signed real-world data contracts pin by pubkey · OP_RETURN attestation lokad (or onion feed) + oracle sig · oracle hosts feed over onion; oracle identity via ZNAM.

### Games (existing zgame framework + escrow)
- **Commit-reveal staked games (RPS/coin-flip)** — fair staked play, auto-payout to winner · OP_RETURN commit + HTLC/multisig stake · moves over `zgame`, matchmaking over onion; players by ZNAM.
- **Prediction market (oracle-settled)** — stake into escrow paid by oracle outcome · multisig/HTLC escrow + oracle attestation · market UI over onion; market+oracle by ZNAM.

### ✂️ CUT — fail the parity filter (would need consensus changes; out of scope)
- **Bidirectional Lightning-style channel** — needs OP_CSV/BIP68 **relative** timelock for penalty revocation; `OP_NOP3` is a no-op here (`script_interp.c:185`), not CSV. Enabling CSV is a consensus change → forbidden by doctrine. *(Unidirectional Spillman channels using absolute CLTV survive and stay in scope.)*
- **Discreet log contracts (adaptor-signature)** — not a consensus break, but requires a new ECDSA adaptor-signature crypto library (XL) and belongs after the core lands; **deferred, not cut**. Prediction markets are covered by the oracle+escrow path above without it.
- **Any covenant / new opcode / state channel / relay-standardness change** — cut on principle: consensus surface.

**General rule for every survivor:** all timelocks are **absolute** (CLTV / nLockTime / Overwinter expiry) computed at funding time, never "N blocks after funding," because relative timelocks (CSV) don't exist here.

---

## 4. Shared primitives to build once — the reusable spine

Five of the six overlay stubs are the *identical* missing move: **build script/OP_RETURN → authorize → wallet-build+broadcast → fold the confirmed result into a projection.** ZSLP already proves that path end-to-end. Build it once as shared services; every contract becomes a thin composition.

| Shared module | Shape / folder | What it is | De-duplicates |
|---|---|---|---|
| **overlay lokad registry** | `lib/overlay/op_return_envelope.*` (domain-pure parser) | Generalize `script/op_return_push.h` into a table: 4-byte lokad → `{parse_fn, apply_fn}`. `explorer_index` apply-dispatch becomes table-driven instead of the hardcoded `if ZNAM / if SLP` ladder (`explorer_index.c:380-458`). | New overlays register one row instead of editing `explorer_index.c`. |
| **overlay_commit_service** | `app/services/src/overlay_commit_service.c` → `zcl_result` | Extract the proven `zslp_command_commit_with_op_return` into one call: `overlay_commit(op_return_bytes, extra_outputs[], fee) → txid`. | ZNAM (5 cmds), ZSWP ZCL-leg, market payment, escrow anchor — all broadcast through one audited path, not five copies. |
| **overlay_memo_service** | `app/services/src/overlay_memo_service.c` | Write **raw bytes** into the Sapling 512 B memo (owns the raw-bytes path — *not* the JSON-string `z_sendmany` path that skips hex-decode, `wallet_shielded_send.c:108-118`) + a memo-scan projection folding owned notes → typed rows. | Closes the ZMSG on-chain stub; reusable for any encrypted on-chain payload. |
| **overlay_bus** | `lib/net/overlay_msg.*` + one `zovl` dispatch row (`msgprocessor.c:1071-1114`) | One P2P typed message carrying `{subprotocol_id, session_id, payload}`, framed/session-managed once, keeping `zcl23_only` service-bit gating. | ZMSG/swap-coordination/escrow-negotiation/game-moves stop hand-rolling wire framing. |
| **contract_state** | `lib/storage/contract_state_projection.c` (event shape) | Event-sourced projection folded from (a) on-chain anchors via `explorer_index` apply and (b) confirmed P2SH spends. One row per `(lokad, contract_id)`, typed state machine, **authoritative only from chain events.** | Every contract gets the node's "fold a log → projection" pattern instead of ad-hoc mutable state. |
| **htlc_settlement_service** | `app/services/src/htlc_settlement_service.c` | Wire the already-tested-but-unwired `htlc_build_redeem_scriptsig` / `htlc_build_refund_scriptsig` / `htlc_extract_secret` (`lib/script/htlc.c:272-302`) into a P2SH spend, broadcast via `overlay_commit_service`. | ZSWP redeem/refund **and** escrow release/refund **and** market-payment escrow become one engine. |
| **swap_watch_stage** | `app/jobs/src/swap_watch_stage.c` (Job: advance-or-blocker) | Cursor-stamped stage observing confirmed redeem/refund/funding via `explorer_index`, transitioning `contract_state` PENDING→FUNDED→REDEEMED/REFUNDED/EXPIRED. | Fixes "state never transitions" (`swap_contract.c:48` only ever sets `SWAP_PENDING`); the contract layer inherits "never silently stuck — name a blocker." |

**The canonical contract shape (the anchor+coordinate pattern), so contracts compose not duplicate:**
1. **Anchor (authority):** OP_RETURN with the contract's lokad recording only *ids + hashes* — `{contract_id, p2sh, terms_hash, counterparty_name}`. Small on purpose (§7). Broadcast via `overlay_commit_service`, folded by `contract_state`.
2. **Coordination (liveness):** `overlay_bus` messages + onion dead-drops carry bulky/private material (full terms, secrets, orders, negotiation). Fast, off-chain, **never authoritative.**
3. **Settlement (authority):** standard P2SH spend (HTLC redeem/refund via absolute CLTV, or m-of-n co-sign) via `htlc_settlement_service` → `overlay_commit_service` → observed by `swap_watch_stage` → terminal `contract_state`.

**Contract liveness is a first-class condition.** An escrow whose absolute-CLTV refund window opens with no redeem is not a silent hang — it's an `app/conditions/escrow_refund_window_open.c` condition with an auto-terminating remedy (auto-broadcast the refund), folded like the node's 30+ existing conditions, supervised by a `contracts_supervisor`. Contracts inherit "a stall is always a named blocker."

**ZNAM record extensions** (identity spine, no new wire — reuse `SET_RECORD`/`SET_TEXT`): onion (`SET_TEXT "onion"`), pubkey (`"pubkey"`), contract endpoints (`"ep.shop"`/`"ep.escrow"`), content hashes (`"content.<label>"`), attestations (`"att.<label>"`). `SET_TEXT`'s ≤32 B key / ≤128 B value already carry all of it.

**Two correctness fixes that gate identity trust (both application-layer, must land in Wave 1):**
1. **Close the `apply_znam` authorization gap** — `SET_RECORD`/`SET_TEXT` apply with **no owner check** (`explorer_index.c:440-450`); anyone can post a record for any name. Add the `strcmp(e.owner_address, owner)` guard `UPDATE`/`TRANSFER` already use, or the entire identity layer is spoofable.
2. **`RENEW` is a silent no-op** — either add an `expiry_height` column folded from the RENEW anchor, or document names as non-expiring. Don't ship parse-and-drop.

---

## 5. Integrated architecture — how it composes (eight shapes + MCP)

```
L4 OPERATOR SURFACE  typed MCP tools + REST (one service contract)
     zcl_name_* · zcl_contract_* · zcl_escrow_* · zcl_swap_* · zcl_msg_send_onchain
L3 CONTRACT/APPLICATION  the shared core, contracts as thin services
     overlay_commit · overlay_memo · overlay_bus · contract_state · htlc_settlement
L2 TRANSPORT & HOSTING  onion serves contract UIs/orderbooks/dead-drops
     onion route table · directory.json contracts[] gossip · onion_fetch_service
L1 IDENTITY & ADDRESSING  one name → addrs + onion + pubkey + endpoints + content
     name_resolver_service over ZNAM REGISTER/SET_RECORD/SET_TEXT
L0 PRIMITIVE ENVELOPE (consensus-frozen, reused verbatim)
     OP_RETURN+lokad · Sapling memo · P2SH/HTLC/multisig/CLTV · P2P typed msg · onion
  Authority flows UP (chain→projection); coordination flows sideways at L2/L3, never authorizes.
```

Mapped to the eight code shapes (`docs/FRAMEWORK.md:201-289`), following the feature-slice recipe (`docs/AGENT_ARCHITECTURE.md`):
- **Models** (`app/models/`): `znam_record.c` (znam_addr/znam_text AR models w/ `validates_*` incl. ownership); extend `swap_contract` state machine; `peer_directory` gains a `contracts` column. Migrations in `database_migrate_features.c`.
- **Services** (`app/services/`): `name_resolver_service`, `overlay_commit_service`, `overlay_memo_service`, `htlc_settlement_service`, `onion_fetch_service`, and per-contract thin services (escrow/shop/swap).
- **Controllers** (`app/controllers/`): 5 missing ZNAM write RPCs in `name_controller.c` (mirror `rpc_name_register`, `:240-417`); `contract_controller.c` dispatched on the onion route table (`onion_service.c:805`, same functions answer clearnet + onion).
- **Jobs** (`app/jobs/`, advance-or-blocker): `swap_watch_stage` settlement watcher.
- **Conditions** (`app/conditions/`): `escrow_refund_window_open` + auto-terminating remedy.
- **Supervisors** (`app/supervisors/`): `contracts_supervisor` in the liveness tree.
- **Events** (`app/events/`): `contract_state` event-sourced projection.
- **lib/** pure domain: `op_return_envelope` parser, `overlay_msg` framing, `htlc.c` builders (existing).

**MCP surface (L4)** — every service gets a typed tool in `tools/mcp/controllers/*_controller.c` calling the *same* service contract as REST: `zcl_name_resolve` (full identity view), `zcl_name_update/_transfer/_renew/_set_record/_set_text`; `zcl_contract_publish/_fetch/_status`; `zcl_escrow_offer/_order/_fund/_release/_refund`; `zcl_swap_redeem/_refund`; `zcl_market_pay`; `zcl_msg_send_onchain`; `zcl_game_invite/_move/_state`. All contract state is introspectable via the existing `zcl_state subsystem=contract_state` primitive — register one dumper in `diagnostics_registry.c:g_dumpers` (~30 lines, no new RPC), so Claude debugs contracts exactly as it debugs sync. The operator memorizes none of it — `zcl_tools_list` enumerates live.

**Headline flow — name-addressed escrow purchase over onion:** Alice `name_register` + `name_set_text {onion,pubkey,ep.shop}` + `contract_publish shop` → Bob `zcl_name_resolve alice` → onion GET `alice.onion/contract/alice/shop` (`onion_fetch_service`) → dead-drop order sealed to `alice.pubkey` (hint, `contract_state: ORDER_PLACED`, *not* authoritative) → Bob funds a P2SH HTLC (absolute CLTV) + OP_RETURN `ESCR{order,p2sh,terms_hash}` via `overlay_commit_service` → both confirm → `contract_state: ESCROW_FUNDED` (**authoritative**) → Alice ships via file-market chunk stream → `htlc_settlement_service.redeem` reveals secret → `swap_watch_stage` sees it → `SETTLED`. Refund: after the absolute CLTV height the `escrow_refund_window_open` condition auto-broadcasts Bob's refund → `REFUNDED`. Every step an MCP tool; every authoritative transition a confirmed chain event.

---

## 6. Ranked build roadmap

Each wave = a small set of parity-checked + tested feature slices. **v1/MVP (bit-for-bit sync + soak) is not derailed** — the S-items in §2 *are* v1-aligned (they strengthen the soak), and the overlay waves are additive application-layer work behind existing service scaffolding.

### 🚀 FIRST WAVE — swarm-build now (3-5 highest-value, immediately actionable slices)
These are independent, individually shippable, individually testable, and each closes a real stub or a live-node defect. Dispatch to parallel worktrees.

1. **S1 — incremental `reducer_frontier_compute_hstar`** (per-log watermark cache + invalidate-on-rewind). The only defect that worsens every day on the live serving node. Test against `test_reducer_frontier.c` anchor-arithmetic fixtures. Broadens the uncommitted working-tree diff from refold-only to always-on; keep its `ZCL_FOLD_TIMING` instrumentation. *Foundation, not overlay — do first.*
2. **S2 — delta-apply `utxo_mirror_sync`** (upsert/delete changed coins from `utxo_apply_log`; full wipe only on row-count mismatch). Largest steady-state tax on a healthy node. *Foundation.*
3. **ZNAM identity slice** — the 5 missing write RPCs (`name_update/_transfer/_renew/_set_record/_set_text`, mirror `rpc_name_register`) + `name_resolver_service` + onion/pubkey/endpoint/content TEXT-record conventions + **the two correctness fixes** (close `apply_znam` owner-check gap `explorer_index.c:440-450`; decide RENEW expiry). *Delivers "pay/message/verify/open alice off one name" with zero new primitive — the entire identity layer in one slice.*
4. **overlay_commit_service extraction** — pull the proven ZSLP broadcast path into one `overlay_commit()` call. *Unblocks every subsequent contract; pure refactor of an audited path, high test leverage.*
5. **S3 prereq bug fix** — make `stage_set_cursor`/`stage_set_named_cursor_if_behind` batch-aware (SAVEPOINT/RELEASE) so the reorg-rewind path stops nest-failing `BEGIN IMMEDIATE`. *Small, unblocks the S3 throughput lever and removes a latent live-reorg fault.*

*(Slices 1-2 and 5 are foundation/strength; 3-4 are the identity+reuse spine. All five parity-safe, all independently mergeable.)*

### Wave 1 — the reuse core (after First Wave)
Lokad registry (`explorer_index` dispatch table-driven) · `overlay_memo_service` raw-bytes memo path · `overlay_bus` (`zovl`) framing + one dispatch row. Land the S5/S6 observability + bootstrap-test items here as low-risk parallel fill.

### Wave 2 — transport & hosting
`contract_controller` routes on the onion table · `directory.json contracts[]` gossip · `onion_fetch_service` generalized from the directory.json fetch.

### Wave 3 — contract state core
`contract_state` event-sourced projection · dead-drop endpoint · **ZMSG on-chain channel** via `overlay_memo_service` (closes the unused-enum stub — first real contract, flow B). S3 throughput lever (batch/period override) lands here now that its prereq is fixed.

### Wave 4 — settlement engine
`htlc_settlement_service` wires existing redeem/refund/extract builders → `overlay_commit_service` (ZCL leg) · `swap_watch_stage` transitions · `escrow_refund_window_open` condition. **One engine closes ZSWP redeem/refund + market buy-side payment + escrow release/refund at once.** S4 (IBD ceiling) profiled + fixed here if binding.

### Wave 5 — contracts as thin services + operator polish
Escrow (flow A), shop, swap (flow C), multisig vault, time-locked vault as ~one-file services over the core · game framework operator RPCs · token DAO ballot lokad · `zcl_state subsystem=contract_state` dumper · MCP tools for all.

*Governance/oracle/auction/crowdfund contracts (§3) are Wave-5-and-beyond thin services; adaptor-signature DLCs are explicitly deferred (XL, post-core).*

---

## 7. Risks & non-goals

**Parity traps (hard rules):**
- **No relative timelocks (OP_CSV/BIP68).** `OP_NOP3` is a no-op here (`script_interp.c:185`). Every refund/timeout window is an **absolute** height/time computed at funding time. Bidirectional Lightning channels are **cut** for this reason. Escrow/swap windows in the flows are absolute by design.
- **No new opcode, covenant, script-template, or relay-standardness change.** Consensus surface is frozen; a consensus change never ships to zclassic23 first, even framed opt-in (doctrine + `check-consensus-parity` E13).
- **Keep on-chain anchors small.** The 223 B `MAX_OP_RETURN_RELAY` is dead/unenforced here (real ceiling ~9999 B/output) but a future zclassicd relay could enforce it. Anchor **only** lokad + ids + hashes on-chain; put bulk in the Sapling memo or over onion and anchor its hash. (The node's own "finality → bake a hash, serve bulk off-chain" principle.)
- **Raw-bytes memo path required.** The `z_sendmany` memo param copies the JSON string literally (no hex-decode) and the reader truncates at `0x00`/`0xF6` (`wallet_shielded_send.c:108-118`); `overlay_memo_service` must write raw bytes and never let a payload's first byte be `0x00`/`0xF6`.
- **Authority only from chain.** A P2P/onion/dead-drop message must never advance authoritative `contract_state` — enforce in review; it is the invariant that makes the whole stack parity-safe.

**Non-goals (will NOT build):**
- The **foreign-chain leg** of cross-chain swaps. This node has no BTC/LTC/DOGE wallet; the counter-leg broadcast + watcher stays a counterparty/human step. The node owns the ZCL leg + secret coordination only. (A wallet-capability limit stated plainly, not a consensus limit.)
- **Adaptor-signature DLCs** now (needs a new crypto lib; deferred post-core — oracle+escrow prediction markets cover the use case without it).
- Any custodial/mixer-coordinator design that holds funds — coinjoin is coordinated blind-signing over onion, never custody.
- **Re-litigating closed sync classes** — the pprev-walk, phashBlock UAF, lock-order ABBA, and window-extend hash-split are fixed with regression tests; do not re-audit or re-propose (`AGENT_TRAPS.md`, memory doctrine).

**Sequencing risk:** don't let overlay enthusiasm starve the S-items. S1/S2 are live-node defects that compound daily; they lead the First Wave for that reason. Everything else is additive and can proceed in parallel worktrees without touching the sync spine.

---

*Source anchors (read-only; verify fresh before editing): `reducer_frontier.c:240-291,584-`, `tip_finalize_stage.c:55-67`, `utxo_mirror_sync_service.c:215-,381-470`, `chainstate.c:512-680`, `staged_sync_supervisor.c:239-284`, `stage.c:510,551`, `download.c:864-887`, `net.h:30`, `download.h:29`, `connman.c:244-390`, `progress_store.c:63-64,330-331`, `explorer_index.c:380-458,440-450`, `name_controller.c:240-417`, `htlc.c:272-302`, `swap_contract.c:48`, `onion_service.c:497,765,805`, `msgprocessor.c:1071-1114`, `wallet_shielded_send.c:108-118`, `op_return_push.h`, `tools/mcp/controllers/*_controller.c`; docs `CONSENSUS_PARITY_DOCTRINE.md`, `FRAMEWORK.md:201-289`, `AGENT_ARCHITECTURE.md`, `AGENT_TRAPS.md`, `refold-fold-rate-bottlenecks.md`, `never-stuck-plan.md`, `HANDOFF.md`.*
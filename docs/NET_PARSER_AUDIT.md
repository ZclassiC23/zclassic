# Network Parser Audit — bounded/fail-closed discipline

Present-tense record of a full read-through of every P2P wire-message
parser under `lib/net/src` (plus the primitive parsers in
`lib/primitives/src` that every `tx`/`block` message reduces to). Scope:
per (message, field) — is a bound enforced, what happens on overflow, what
the allocation pattern is, and what the failure action is. Verify fresh
against the live code before trusting a line number; this doc rots like
any other.

**Verdict: the codebase enforces the pedantic bound-before-parse discipline
almost everywhere.** Every count-prefixed loop that could scale an
allocation checks its declared count against a protocol cap (usually via
the shared `msg_count_exceeds()` helper in `msg_bounds_guard.c`) BEFORE
allocating, and the highest-stakes parsers (`transaction_deserialize`,
`block_deserialize`) additionally reject a declared count whose minimum
per-element wire size cannot fit in the REMAINING stream bytes, closing the
"claim 10M items, send 3 bytes" allocation-bomb shape one step earlier than
the cap alone would. Two known low-severity gaps are fixed (below); one
behavior that looks like a gap is an intentional, tested design choice —
documented below so it is not re-"discovered" and re-litigated.

## Framing layer (`lib/net/src/net.c`)

The 24-byte wire header (`pchMessageStart[4] + pchCommand[12] +
nMessageSize[4] + nChecksum[4]`) and payload are read incrementally by
`net_message_read_header` / `net_message_read_data`, driven from
`p2p_node_receive_bytes` (`net.c:431`).

| Field | Bound enforced | Overflow behavior | Allocation | Failure action |
|---|---|---|---|---|
| `pchMessageStart` (magic) | exact 4-byte match vs chain params | `net_message_read_header` returns `-1` before `in_data` is set (`net.c:209-211`) | none | `handled<0` → `p2p_node_receive_bytes` tags `PEER_OFFENCE_INVALID_HEADER` and returns `false` (`net.c:471-482`); connman disconnects (`connman.c:1772-1777`) and scores +50 via `p2p_node_score_framing_offence` (`connman.c:1784`) |
| `nMessageSize` (header phase) | `<= MAX_SIZE` (32 MiB, `protocol.h:17`) | rejected in `net_message_read_header` before `in_data=true` (`net.c:213-214`) | none | same as above (INVALID_HEADER, +50) |
| `nMessageSize` (data phase) | `<= MAX_PROTOCOL_MESSAGE_LENGTH` (2 MiB, `net.h:27`) | rejected in `net_message_read_data` **before** the `zcl_realloc` (`net.c:229`) | none | `handled<0` → tags `PEER_OFFENCE_INVALID_PAYLOAD`, returns `false` (weight 20) |
| payload bytes | `alloc = hdr.nMessageSize` (already ≤2 MiB) charged against a **process-wide** budget (`g_recv_total_bytes`, default 256 MiB, env `ZCL_MAX_RECVBUFFER_TOTAL_BYTES`) before `zcl_realloc` (`net.c:238-253`) | budget-exhausted realloc request returns `-1` without allocating | bounded, budget-gated | same as data-phase reject |
| `nChecksum` | verified (`hash256` of payload, first 4 bytes) in the dispatch loop, NOT in the framing layer (`msgprocessor.c:1641-1659`) | mismatch → message silently dropped, loop `continue`s | none | **not scored** — see "Checksum mismatch: intentional, not a gap" below |

**Per-connection buffer cap:** there is no *dedicated per-peer byte* cap on
the receive side. Three
independent bounds combine to make max-size messages sent back-to-back
safe:
1. Per-message cap: 2 MiB (`MAX_PROTOCOL_MESSAGE_LENGTH`), enforced before
   allocation (`net.c:229`).
2. Per-connection **message-count** cap: `MAX_RECV_MESSAGES` = 1024
   in-flight reassembly slots (`net.h:163`), enforced two ways — a hard
   reject once the processing thread has fallen behind
   (`net.c:441-450`), and upstream backpressure in the recv loop itself:
   `connman_recv_cap_for_queue()` (`connman.c:97-109`) shrinks the actual
   `recv()` read size as the queue nears full, throttling how fast a
   single fast sender's bytes even enter the reassembler.
3. Process-wide **byte** cap: 256 MiB (`g_recv_total_bytes`, item above),
   shared by every connection. A single peer sending max-size messages
   back-to-back is capped by (1)+(2) long before (3) would matter in
   isolation, but if many peers do it simultaneously, (3) is the backstop
   — once it trips, the NEXT allocation attempt (by any peer, including
   the one that filled it) fails and that peer's connection is dropped via
   the `handled<0` path. No unbounded buffering is possible under any
   combination of peers.

Send-side has the symmetric mirror: a process-wide 512 MiB send budget
(`g_send_total_bytes`, env `ZCL_MAX_SENDBUFFER_TOTAL_BYTES`) plus a 32 MiB
per-peer send cap (`net_send_peer_bytes_cap()`, env
`ZCL_MAX_SENDBUFFER_PEER_BYTES`) — both checked by `net_send_over_budget()`
before `process_getdata` serves another block/tx (`net.c:135-160`,
`msg_blocks.c:148`).

## Message parsers

Every row below either goes through `msg_count_exceeds()` (the shared
cap-check helper, `msg_bounds_guard.c`) or an equivalent inline bound
BEFORE any loop/allocation keyed on the declared count.

| Message | Field | Bound enforced | Overflow behavior | Allocation pattern | Failure action |
|---|---|---|---|---|---|
| `version` | `sub_version` length | `< MAX_SUBVER_LENGTH` (256, `p2p_message.h:16`) | `stream_read_bytes` never called; `LOG_FAIL` before | fixed `char[256]` field, no heap | reject, `PEER_OFFENCE_PROTOCOL_VIOLATION` (100) via `process_version` (`msg_version.c:310`) |
| `version` | `protocol_version` | `>= MIN_PEER_PROTO_VERSION` | disconnect | n/a | `node->disconnect=true` + scored (`msg_version.c:316-327`) |
| `version` | duplicate delivery | `node->version != 0` gate | rejected before touching the new payload | n/a | scored PROTOCOL_VIOLATION, `LOG_FAIL` (`msg_version.c:299-304`) |
| `addr` | element count | `msg_count_exceeds(..., MAX_ADDR_TO_SEND=1000)` (`msgprocessor_inv.c:99`) | rejected before the deserialize loop | one stack `struct net_address` per iteration, no heap | disconnect + `PEER_OFFENCE_FLOOD` (20) |
| `addr` | repeated max-legal batches | fixed 60 s window, `ADDR_RATE_MAX_PER_WINDOW = 3×MAX_ADDR_TO_SEND` (`msgprocessor_inv.c:88-89`) | 4th legal-size batch in-window rejected | n/a | disconnect + FLOOD |
| `addr` | each entry's routability | `net_addr_is_routable()` gate inside `addrman_add` (`addrman.c:634`) | non-routable entries are silently dropped (not inserted), no penalty | n/a | none (benign — see test section F) |
| `addrv2` (BIP155) | — | **not implemented** — no `addrv2`/`getaddrv2` dispatch entry exists | n/a | n/a | n/a (not a gap: nothing to parse) |
| `getaddr` | — | none needed (no peer-controlled variable field) | n/a | n/a | n/a |
| `inv` | element count | `msg_count_exceeds(..., MAX_INV_SZ=50000)` (`msg_tx.c:113`) | rejected before loop | n/a | disconnect + FLOOD |
| `getdata` | element count | inline `count > MAX_INV_SZ` (`msg_blocks.c:121`) | rejected before loop | n/a | disconnect + FLOOD |
| `getdata` | serving budget | `net_send_over_budget()` checked per item inside the loop (`msg_blocks.c:148`) | stops serving early (not an error, not a disconnect — peer re-requests later, matches Core's `fPauseSend`) | n/a | none |
| `notfound` | element count | `msg_count_exceeds(..., MAX_INV_SZ)` (`msgprocessor_inv.c:47`) | rejected before loop | n/a | disconnect + FLOOD |
| `getheaders`/`headers` | header count | `msg_count_exceeds(..., 2000)` (`msg_headers.c:475`) | rejected before loop | n/a | disconnect + FLOOD |
| `headers` | per-header sequence tracking | fixed `seq_hashes[512]`/`seq_heights[512]` with an explicit `seq_count < 512` guard before every write (`msg_headers.c:554`) | writes simply stop past 512 (headers still accepted/indexed, just not queued into the fast-path sequence) | stack arrays, no heap | none (soft cap, not a reject) |
| `tx` | `vin`/`vout` count | `<= MAX_TX_INPUTS`/`MAX_TX_OUTPUTS` (65536) **and** `count * min_wire_bytes > stream_remaining()` (e.g. `num_vin*41`, `transaction.c:508-519`) | rejected before the `~627 MB` calloc a full-cap count would otherwise touch | `zcl_calloc(count, ...)` only after both checks pass | `LOG_FAIL` → `false`; caller scores `PEER_OFFENCE_INVALID_MESSAGE` (`msg_tx.c:228`) |
| `tx` | `vShieldedSpend`/`vShieldedOutput`/`vJoinSplit` count | same two-layer pattern (cap + remaining-bytes-per-element) (`transaction.c:557-618`) | same | same | same |
| `tx`/`block` | `scriptSig`/`scriptPubKey` length | `<= MAX_SCRIPT_SIZE` (`transaction.c:255,280`) | rejected before read | fixed `unsigned char data[MAX_SCRIPT_SIZE]` field, no heap | `LOG_FAIL` → `false` |
| `block` | `vtx` count | `<= MAX_BLOCK_TRANSACTIONS` (50000) **and** `count*10 > stream_remaining()` (`block.c:76-87`) | rejected before the `~14 MB` calloc | `zcl_calloc` only after both checks | `LOG_FAIL` → `false` |
| `block` | `nSolutionSize` | `<= MAX_SOLUTION_SIZE` (`block.c:51-53`) | rejected before read | fixed field | `LOG_FAIL` → `false` |
| `ping`/`pong` | nonce | fixed 8 bytes, protocol-gated by `BIP0031_VERSION` | n/a | n/a | truncated read is non-fatal (`msgprocessor_pingpong.c:22-24,48-50`) |
| `reject` | `msg_type`/`reason` length | bounded against fixed local buffers (32 / 256 bytes) | oversized `msg_type` handled — see below | fixed stack buffers, no heap | non-fatal `return true` either way (advisory message) |
| `feefilter` | fee rate | fixed 8 bytes | n/a | n/a | truncated is non-fatal |
| `sendcmpct`/`cmpctblock`/`getblocktxn`/`blocktxn` | short-txid / prefilled-tx / index / tx counts | `msg_count_exceeds(..., MAX_COMPACT_BLOCK_TXNS=50000)` / `MAX_GETBLOCKTXN_INDICES=50000` (`compact_blocks.c:511,535,602,654`) | rejected before the corresponding `zcl_malloc`/`zcl_calloc` | bounded | reject + `PEER_OFFENCE_INVALID_PAYLOAD` at the call sites in `msg_compact.c` |
| `getblocktxn` (server side) | requested `indices[i]` vs `blk.num_vtx` | explicit per-index bound check (`msg_compact.c:305`) | rejected mid-loop, response freed | n/a | `PEER_OFFENCE_PROTOCOL_VIOLATION` (100) |
| `filterload`/`filteradd`/`filterclear` (BIP37) | — | feature gated off by default (`bip37_enabled()`) | payload never parsed when disabled | n/a | `PEER_OFFENCE_PROTOCOL_VIOLATION` when disabled (`msgprocessor.c:679-690`) |
| `zmsg` | `sender`/`recipient` length (u8) | `< ZMSG_MAX_ADDR` (128) (`zmsg.c:55-66`) | reject before read | fixed `char[128]` fields | `LOG_FAIL` → `false` |
| `zmsg` | `body` length (u16) | `< ZMSG_MAX_BODY` (4096) (`zmsg.c:72-74`) | reject before read | fixed `char[4096]` field | `LOG_FAIL` → `false` |
| `zmsgack` | — | fixed 32 bytes | n/a | n/a | n/a |
| `zfilelist` | offer count | inline `if (count > 50) count = 50;` clamp (`msgprocessor.c:767`) | excess silently clamped (not rejected — a self-clamp, not a reject-on-overflow) | one stack `struct file_offer` per iteration | n/a |
| `zfilelist` offer | `filename` length (u8) | max value 255 fits exactly in `filename[256]` (`file_market.c:84-88`) | n/a (255 is the max representable u8, buffer sized for it) | fixed field | n/a |
| `zfilelist` offer | `ttl` | clamped to `FILE_MARKET_MAX_TTL` before re-gossip (`msgprocessor.c:777-778`) | re-gossip amplification bounded regardless of peer-supplied ttl | n/a | n/a |
| `zfileoffer` | complete paid offer | exact 535-byte payload; fixed codec validates version, magic, network, exact chunks, money range, Sapling destination, endpoint, lifetime, and Ed25519 signature before admission | malformed, wrong-network, expired, or forged offer is dropped before cache/persistence; exact duplicates are idempotent | fixed 535-byte stack buffer plus one fixed `struct file_offer`; cache and peer windows are statically bounded | verification attempts limited to 32 and fresh offers to 8 per peer per 10 seconds; only newly admitted exact wire is forwarded |
| `zfilechal`/`zfileproof`/`zfilepay`/`zfileaddr` | — | fixed-size structs only (32-byte hashes, u32 indices) | n/a | n/a | n/a |
| `zgame` | `data[0]`/`data[1]` | `len < 2` guard before any index (`p2p_game.c:267`) | `GAME_RESIGN` returned | n/a | n/a |
| `zgame` `GAME_MOVE`/`GAME_STATE` payload | `len >= 3`/`len >= 14` guards per field (`p2p_game.c:280,284`) | field left at caller-provided default | n/a | n/a |
| `zgame` board values | clamped `> 2 → 0` (`p2p_game.c:288-289`) | corrupt board values normalized instead of trusted | n/a | n/a |
| `zmanifest` | `num_chunks` | `== 0 \|\| > MANIFEST_MAX_CHUNKS` reject (`msgprocessor_snapshot.c:1527`) | reject before the `zcl_calloc(num_chunks, 32, ...)` | bounded | `PEER_OFFENCE_INVALID_PAYLOAD` |
| `zmanifest` | per-chunk hash array vs merkle root | Merkle-recomputed and compared; mismatch rejects the whole manifest (`msgprocessor_snapshot.c:1561-1568`) | n/a | n/a | `PEER_OFFENCE_INVALID_PROOF` |
| `zblkmanfst` | `num_pieces` | `== 0 \|\| > 100000` AND must equal the height-range-derived `expected` count AND `stream_remaining() >= num_pieces*32` before the `zcl_calloc` (`msgprocessor_snapshot.c:1853-1874`) | triple-gated reject | bounded | `PEER_OFFENCE_INVALID_PAYLOAD`/`INVALID_MESSAGE` |
| `zblkreq`/`zblkdata` | `block_count` | `== 0 \|\| > BLOCKS_PER_PIECE(512)` (`msgprocessor_snapshot.c:2024`) | reject before the `zcl_calloc(block_count, 32, ...)` | bounded | n/a (drops the request) |
| `zblkdata` per-block payload length | `len64 == 0 \|\| > BLOCK_PIECE_MAX_BLOCK_BYTES \|\| > stream_remaining()` (`msgprocessor_snapshot.c:1026-1032`) | reject before treating the pointer range as a block | zero-copy (`refs[i].data` points into the existing message buffer, no separate alloc) | drops the piece |
| `zblkbitmap` | `bitmap_len` | `== 0 \|\| > 65536` (`msgprocessor_snapshot.c:2135`) | reject before the `zcl_calloc` | bounded | n/a (drops the message) |

## Local-file (non-wire) persistence — informational only, not audited as a wire surface

`addrman_deserialize()` (`addrman.c:1148`) loads the SHA3-sealed
`peers.dat`/`anchors.dat` from local disk, not from a live peer. Its
`nUBuckets`-driven outer loop has no explicit cap of its own, but every
`stream_read_i32_le` inside it is bounds-checked against the actual file
size (`core/math/src/serialize.c:90-99`), so a truncated/corrupt file fails
fast on the first out-of-range read rather than looping unboundedly — the
same structural protection as the wire parsers, just not exercised by a
remote peer. Left as-is; not in scope for this doc's "wire input" bar.

## Known-fixed issue: `reject` oversized `msg_type`

`lib/net/src/msgprocessor_pingpong.c::process_reject()`: when the
peer-declared `msg_type` length is `>= sizeof(msg_type)` (32), the code
skips *storing* it, and explicitly skips exactly that many bytes
(bounds-checked against `stream_remaining()` first, bailing out cleanly if
the declared length exceeds the actual message body — the "claimed >
actual" framing-lie shape) before reading `code`. `reason` needs no
equivalent handling: it is the last field on the wire, so an oversized
`reason_len` cannot misalign anything downstream — leaving it unread is a
safe truncation there, not a misparse. `reject` is advisory-only (its
parsed fields are only ever `printf`'d, never stored or acted on), so a
misparse here is a garbled log line, not a security issue — but the
"reject > bound, never silently truncate-and-continue-as-if-nothing-
happened" discipline applies regardless.

Pinned by tests **H** (oversized `msg_type`, asserts the read cursor lands
exactly at the expected post-`reason` offset) and **H2** (declared length
exceeds the actual message body) in `lib/test/src/test_net_msg_dos.c`.

## Regression-pinned, intentionally unchanged behaviors

- **`addr` batch of non-routable addresses**: correctly filtered by
  `addrman_add`'s `net_addr_is_routable()` gate — pinned by test **F**.
- **Checksum mismatch** (see below) — pinned by test **G**.

## Checksum mismatch: intentional, not a gap

`msg_process_messages` (`msgprocessor.c:1641-1659`) verifies every
message's `nChecksum` against `hash256(payload)` before dispatch, and on
mismatch silently drops the message (`continue`, no dispatch) WITHOUT
calling `peer_scoring_record`. This matches an explicit, tested precedent
one case away: an unknown/garbage command string is *also* not treated as
misbehaviour (`test_net_msg_dos.c`, case D, "Bitcoin Core parity: unknown
commands are not misbehaviour"). A checksum failure has the identical
property — it does not distinguish a malicious peer from a
corrupted/buggy one (bit flips, a version skew, a badly-implemented
alternate client), so scoring it risks banning honest peers for
non-malicious noise. Real Bitcoin Core applies the same policy (log +
drop, no `Misbehaving()` call on a checksum failure). The cost of NOT
scoring it is bounded, not unbounded: one `hash256` over the
already-framing-capped (≤2 MiB) payload, and the message occupies one of
the peer's already-capped `MAX_RECV_MESSAGES` reassembly slots until
drained — nothing scales with repetition beyond those existing caps. Left
unchanged; regression-pinned by test **G** so a future reader sees this was
decided, not missed.

## Negative-case test coverage

`lib/test/src/test_net_msg_dos.c` (group `net_msg_dos`, registered in
`lib/test/src/test_parallel.c`) and `test_net_framing_dos` (same file,
registered separately) plus `lib/test/src/test_net_handshake_adversarial.c`
(group `net_handshake_adversarial`) together cover:

- **version**: oversized declared user-agent length
  (`test_oversized_user_agent_rejected`), protocol-too-old, duplicate
  version, self-connection.
- **addr**: oversized count (case A3), repeated max-legal batches / rate
  limiting (case A5), non-routable flood (case F).
- **inv/getdata/notfound/headers**: oversized counts (cases A1/A2/A4, and
  `msg_headers.c`'s own 2000 cap).
- **framing layer**: declared size over cap at both the header phase
  (`MAX_SIZE`) and data phase (`MAX_PROTOCOL_MESSAGE_LENGTH`) — cases a/b/c
  in `test_net_framing_dos`, each asserting the connection IS penalized
  (scored + would-ban on repeat). Checksum mismatch (case G) is the one
  length/integrity-lie case that is intentionally NOT penalized (see
  above) — pinned so that stays a decision, not a gap.
- **reject parser realignment**: cases H / H2.

Every case asserts: no crash, no allocation proportional to a
declared-but-undelivered count, and — for the cases where a bound is
actually crossed — that the connection is scored via the existing typed
`peer_offence` mechanism: reject + typed misbehavior score via the
existing scoring, never silent truncation.

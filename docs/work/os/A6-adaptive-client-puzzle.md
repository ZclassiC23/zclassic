# OS-A6 — `lib/net/puzzle.{c,h}`: a load-adaptive client-puzzle primitive

Status: design only — no code in this doc has been written yet. Feasibility
spike per `os-substrate-plan.md` L6, not yet prioritized.

## 0. What already exists

Three PoW call sites today, not two:

1. **`app/controllers/src/store_controller_pow.c`** — `store_pow_challenge()`
   / `store_pow_verify_and_claim()`. Fixed difficulty
   (`FAST_SYNC_POW_BITS = 20`), client-self-chosen `peer_id`
   (`SHA3-256("store:order:pow:<product_id>")`), calls the **legacy**
   `fast_sync_verify_pow()`, and layers its own 4096-slot replay ring on top
   because the legacy primitive has no memory of accepted solutions. No load
   response — 20 bits whether the store is idle or under flood.

2. **`lib/net/src/fast_sync.c:596-624`** (`fast_sync_verify_pow`/
   `fast_sync_solve_pow`) — the **legacy** primitive this task retires.
   Client-chosen `peer_id`, fixed bits, no server challenge, no replay
   memory, no load response.

3. **`lib/net/src/fast_sync.c:642-880`** (`fast_sync_verify_pow_ex`/
   `fast_sync_solve_pow_ex` + `struct fast_sync_pow_gate`) — landed in
   `c7f7bc5b0` and is **already** most of what OS-A6 asks for: a
   server-issued rotating challenge seed (45s epoch + one grace epoch), a
   single-use accepted-solution ring (2048 slots), and a difficulty formula
   that responds to live load:
   ```c
   bits = FAST_SYNC_POW_MIN_BITS(12)
        + inflight * FAST_SYNC_POW_INFLIGHT_BITS(2)
        + max(0, accepted_in_window - FAST_SYNC_POW_SOFT_RATE(8)) / FAST_SYNC_POW_RATE_STEP(4)
   clamped to [12, 26]
   ```
   `accepted_in_window` is a **hard 10s tumbling window count**, not an
   EWMA — it resets to 0 the instant the window rolls, so load right after a
   reset reads as zero for up to 10s. This is the one gap between what's
   built and what OS-A6's brief asks for ("keyed to a rolling ... EWMA").
   Consumer: `lib/net/src/file_service.c`'s `g_fs_pow_gate`, driven from
   `fs_admit_serve_pow()`. Test: `lib/test/src/test_file_service_pow_gate.c`
   (7 cases — the KAT/behavior model to clone).

**Conclusion:** site #3 is the target shape, minus (a) a generic per-surface
home outside `fast_sync.c`, (b) a true EWMA instead of a tumbling window,
and (c) a second live instance for the store surface (replacing #1/#2). The
extraction is "promote `fast_sync_pow_gate` to `lib/net/puzzle.{c,h}` under a
surface-neutral name, swap the difficulty formula's load term for an EWMA,
and re-point both existing call sites at it" — not a green-field design.

## 1. Extraction shape

New `lib/net/puzzle.{c,h}` (picked up by the build with zero Makefile edits,
same `LIB_MODULES` mechanism as `fast_sync.c`). `puzzle_verify`/`puzzle_solve`
are the pure SHA3-256(challenge_seed‖peer_token‖ts‖nonce) primitives, body
moved verbatim from `fast_sync_verify_pow_ex`/`fast_sync_solve_pow_ex`. A
`struct puzzle_gate` (mutex, rotating seed pair, single-use ring, EWMA state)
replaces `struct fast_sync_pow_gate` — same fields, `accepted_in_window`/
`window_start` replaced by `rate_ewma_milli`/`last_update_us`. A
`struct puzzle_policy` (min/max bits, seed-rotate/skew secs, EWMA half-life,
soft-rate, rate-step, inflight-bits — all zero-defaultable) lets each
admission point size the band/epoch/soft-rate independently, since store
orders and multi-GB file streams have very different honest-request costs,
while sharing one implementation. `fast_sync.{c,h}` keeps only the legacy
`fast_sync_verify_pow`/`fast_sync_solve_pow` pair; `file_service.c` swaps its
`fast_sync_pow_gate` type for `puzzle_gate` with zero wire-format change.

Replay/expiry contract (unchanged from `fast_sync_pow_gate`, restated as the
contract `puzzle.h` owns): freshness (`ts` within `±ts_skew_secs`),
challenge binding (current seed or one-epoch-grace previous seed only),
peer binding (caller-supplied `peer_token` the attacker cannot freely
rechoose per attempt — the P2P handshake nonce for file-service, the
product-bound hash for the store), single-use (SHA3 digest recorded in the
ring on accept), ring-bounded expiry (ages out only by wraparound).

## 2. The EWMA — update rule and bits mapping

Decayed-count EWMA: each accepted request contributes a unit impulse
(`rate_ewma_milli += 1000`, fixed-point req/sec×1000), decayed by elapsed
wall time via `ewma_halflife_secs`-driven integer halving (dt clamped to
[0, 5s] so a long idle gap decays smoothly, never a multi-hour cliff from
one stale sample) plus a linear sub-halflife remainder — no float/libm, no
reset edge (unlike the tumbling window, a flood starting 1μs after the last
tick is visible immediately). Model: `stage.c`'s existing integer EWMA idiom
(`next = prev + (sample-prev)/16`), generalized to a tunable half-life.
Bits mapping is the same shape as today's `fs_pow_gate_adaptive_bits_locked`
with `rate_ewma_milli/1000` substituted for `accepted_in_window`, so
`file_service.c`'s behavior is unchanged at defaults (proven by an
equivalence test at the boundary values 8/9/12/26).

## 3. Re-basing the two existing sites

- **`file_service.c`** (already-adaptive site): mechanical rename only, wire
  layout and `fs_admit_serve_pow`'s three-way return untouched. Equivalence
  proof: the existing 7 `test_file_service_pow_gate.c` cases must pass
  unmodified against the renamed types (a compile failure is the regression
  signal) + one new `test_gate_ewma_matches_window_at_boundary` case proving
  the EWMA agrees with the old window at the `SOFT_RATE`/`SOFT_RATE+1`
  boundary.
- **`store_controller_pow.c`** (legacy site): a module-static
  `struct puzzle_gate g_store_pow_gate` replaces the fixed 20-bit gate and
  the hand-rolled 4096-slot replay ring (deleted — `puzzle_gate_verify`'s
  ring replaces it). `store_pow_bind_product`'s per-product `peer_token`
  derivation is unchanged (store-specific policy, out of `puzzle.h`'s
  scope). **Visible behavior change** (call out explicitly in the PR, not
  silent): the store's puzzle difficulty is no longer a fixed 20 bits — an
  idle store now issues ~12-bit puzzles (~60× cheaper for an honest buyer),
  a flooded store ramps to 26 bits. Recommended policy: `min=14
  (client already solves ~1M-hash puzzles per the existing comment),
  max=24, soft=2, step=2` — a store sees far fewer req/sec than a snapshot
  server, so the ramp should trigger earlier. New
  `test_store_pow_puzzle_rebase.c` proves: solve-admits, garbage-nonce
  refuses, replay refuses, per-product isolation survives the rebase, and a
  skew-window violation refuses.
- New `test_puzzle.c`: frozen KAT vectors for `puzzle_verify` (byte-frozen
  regression, not just round-trip) plus the EWMA rise/decay/no-reset-edge
  test (needs a test-only clock seam, mirroring `stage.c`'s existing
  `_test_set_ewma_us` pattern).

## 4. Abusable unauthenticated surfaces to protect as the public surface grows

Ranked by what's live/near-live per `docs/HANDOFF.md` and CLAUDE.md's
feature list, each with the `puzzle_policy` this design implies:

| Surface | Today | `puzzle_policy` |
|---|---|---|
| **File-market chunk stream** (`file_service.c`) | ALREADY GATED → rebases onto `puzzle_gate` (§3), no policy change | defaults (`min=12,max=26,soft=8,step=4`) — proven live-shaped |
| **Store order mint** (`store_controller_pow.c`) | GATED but fixed-difficulty (§3 fixes this) | `min=14,max=24,soft=2,step=2` |
| **SHA3 UTXO snapshot serving** (`fast_sync.c`'s `SNAPSHOT_REQUEST`) | **UNGATED today** — per-IP-per-hour volume limiter only, no PoW gate before the O(n) fallback build path | new work, not just rebase: wire `g_snapshot_pow_gate` the same way `file_service.c` does, `min=12,max=26,soft=8,step=4` |
| **Package/binary swarm** (named in CLAUDE.md, not yet a controller in this tree) | not built | when built: same shape as file-service, flag as template |
| **ZNAM name registration** | on-chain OP_RETURN, mempool-fee-gated already | not a puzzle-gate candidate — the fee market is the correct admission control |
| **ZMSG P2P inbox** | plaintext P2P, no PoW | if it becomes public-facing: same shape as store |

## 5. Acceptance bar

- `make build-only` green with `lib/net/puzzle.{c,h}` added and
  `fast_sync_pow_gate`/`fast_sync_verify_pow_ex`/`fast_sync_solve_pow_ex`
  removed from `fast_sync.{c,h}`.
- `test_puzzle.c` (KAT + EWMA rise/decay/no-reset-edge), green.
- `test_file_service_pow_gate.c`'s 7 existing cases compile/pass unmodified
  against the rename, plus the new boundary-equivalence case.
- `test_store_pow_puzzle_rebase.c` (5 cases), green.
- `make test-parallel` all-green (canonical runner, never `test_zcl` directly)
  and `make lint` green — no new gate required (no consensus predicate, no
  AR write, no new command-registry surface).
- PR description explicitly calls out the one visible behavior change: the
  store's puzzle difficulty is no longer a fixed 20 bits.

## 6. Consensus-parity risk

None — every touched primitive is explicitly an in-memory, never-persisted,
never-a-consensus-predicate admission gate, stated in `fast_sync.h` today and
carried into `puzzle.h`'s own comment. `CONSENSUS_PARITY_DOCTRINE.md`'s gate
and `test_consensus_parity` are untouched by this design.

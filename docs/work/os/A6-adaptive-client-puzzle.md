# OS-A6 — `lib/net/puzzle.{c,h}`: a load-adaptive client-puzzle primitive

Status: design only (per plan `WF-security: A6 puzzle primitive extraction
(sonnet) + adaptive-difficulty design doc (opus)`). No code in this doc has
been written; every anchor below was read this session. Collision check at
the end.

## 0. What already exists (read fresh — this is bigger than the plan text implies)

There are **three** PoW call sites today, not two, and the third one is
already most of what OS-A6 asks for:

1. **`app/controllers/src/store_controller_pow.c`** — `store_pow_challenge()`
   / `store_pow_verify_and_claim()`. Fixed difficulty
   (`FAST_SYNC_POW_BITS = 20`, `lib/net/include/net/fast_sync.h:58`),
   client-self-chosen `peer_id` (`SHA3-256("store:order:pow:<product_id>")`,
   `store_controller_pow.c:44-50`), calls the **legacy**
   `fast_sync_verify_pow()` (`fast_sync.c:596-624`), and layers its own
   4096-slot replay ring (`store_controller_pow.c:81-118`) on top because
   the legacy primitive has no memory of accepted solutions. No load
   response at all — 20 bits is 20 bits whether the store is idle or under
   flood.

2. **`lib/net/src/fast_sync.c:596-624`** (`fast_sync_verify_pow` /
   `fast_sync_solve_pow`, declared `fast_sync.h:66-76`) — the **legacy**
   primitive named in the task. Client-chosen `peer_id`, fixed
   `FAST_SYNC_POW_BITS`, no server-issued challenge, no replay memory, no
   load response. This is the thing OS-A6 is meant to retire.

3. **`lib/net/src/fast_sync.c:642-880`** (`fast_sync_verify_pow_ex` /
   `fast_sync_solve_pow_ex` / `struct fast_sync_pow_gate` +
   `fast_sync_pow_gate_{init,challenge,verify,serve_begin,serve_end}`,
   declared `fast_sync.h:79-174`) — landed in `c7f7bc5b0` ("PoW-gate the
   multi-GB block stream + resource caps") and is **already**: a
   server-issued rotating challenge seed (`GetRandBytes`, 45 s epoch + one
   grace epoch so an in-flight solve is never invalidated by rotation), a
   single-use accepted-solution ring (2048 slots), and a difficulty formula
   that responds to live load (`fs_pow_gate_adaptive_bits_locked`,
   `fast_sync.c:720-732`):
   ```c
   bits = FAST_SYNC_POW_MIN_BITS(12)
        + inflight * FAST_SYNC_POW_INFLIGHT_BITS(2)
        + max(0, accepted_in_window - FAST_SYNC_POW_SOFT_RATE(8)) / FAST_SYNC_POW_RATE_STEP(4)
   clamped to [12, 26]
   ```
   `accepted_in_window` is a **hard 10-second tumbling window count**
   (`fs_pow_gate_refresh_window_locked`, `fast_sync.c:707-713`), not an
   EWMA — it resets to 0 the instant `now - window_start > 10`, so load
   right after a reset reads as zero for up to 10 s. This is the one gap
   between what's built and what OS-A6's brief asks for ("keyed to a
   rolling ... EWMA"). Consumer: `lib/net/src/file_service.c` (the file
   marketplace / package-swarm surface), one instance
   `g_fs_pow_gate` (`file_service.c:487`), accessed via `fs_pow_gate()`
   (`file_service.c:500-503`), driven from `fs_admit_serve_pow()`
   (`file_service.c:663-702`) and bracketed with
   `fast_sync_pow_gate_serve_begin/end` around the actual multi-GB stream
   (`file_service.c:979,994`). Test: `lib/test/src/test_file_service_pow_gate.c`
   (7 cases, described further below — this is the KAT/behavior model to
   clone).

**Conclusion:** site #3 is not a second thing to generalize *from* — it is
the target shape, minus (a) a generic per-surface home outside `fast_sync.c`,
(b) a true EWMA instead of a tumbling window, and (c) a second live
instance for the store surface (replacing sites #1/#2). The extraction is
"promote `fast_sync_pow_gate` to `lib/net/puzzle.{c,h}` under a
surface-neutral name, swap the difficulty formula's load term for an EWMA,
and re-point both existing call sites at it" — not a green-field design.

## 1. New file: `lib/net/puzzle.{c,h}`

`lib/net` is a `LIB_MODULES` entry (`Makefile:174`,
`$(wildcard lib/$(m)/src/*.c)`), so `lib/net/src/puzzle.c` is picked up by
the build with zero Makefile edits — same mechanism that already carries
`fast_sync.c`.

### 1.1 Header — `lib/net/include/net/puzzle.h`

```c
#ifndef ZCL_NET_PUZZLE_H
#define ZCL_NET_PUZZLE_H

#include <stdbool.h>
#include <stdint.h>
#include <pthread.h>

/* ── Pure puzzle primitives (verbatim behavior of
 * fast_sync_verify_pow_ex/fast_sync_solve_pow_ex, fast_sync.c:642-676) ──
 * SHA3-256(challenge_seed || peer_token || ts || nonce) has
 * `difficulty_bits` leading zero bits. No policy (rate/replay/expiry) — the
 * gate below owns that, same split as today. */
bool puzzle_verify(const uint8_t challenge_seed[32],
                    const uint8_t peer_token[32],
                    int64_t ts, uint64_t nonce, int difficulty_bits);
bool puzzle_solve(const uint8_t challenge_seed[32],
                   const uint8_t peer_token[32],
                   int64_t ts, int difficulty_bits, uint64_t *nonce_out);

/* ── Difficulty band + load knobs. Same numeric floor/ceiling/epoch as the
 * fast_sync_pow_gate defaults (fast_sync.h:96-118) — a surface that wants
 * different economics passes its own struct puzzle_policy (1.3) instead of
 * changing these globals. */
#define PUZZLE_MIN_BITS          12
#define PUZZLE_MAX_BITS          26
#define PUZZLE_SEED_ROTATE_SECS  45
#define PUZZLE_TS_SKEW_SECS      120
#define PUZZLE_RECENT_CAP        2048

/* EWMA half-life for the per-surface accepted-request rate, in seconds.
 * Replaces fast_sync_pow_gate's 10s tumbling window (fast_sync.c:707-713),
 * whose count resets to 0 at each boundary — a flood that starts 1s after a
 * reset is read as "idle" for up to 9s. The EWMA has no reset edge: see
 * §2 for the update rule and the bits mapping. */
#define PUZZLE_EWMA_HALFLIFE_SECS 10
#define PUZZLE_EWMA_SOFT_RATE     8   /* accepted/sec before bits start rising */
#define PUZZLE_EWMA_RATE_STEP     4   /* +1 bit per this many accepted/sec over soft */
#define PUZZLE_INFLIGHT_BITS      2   /* +bits per concurrent large serve (optional) */

/* Per-surface policy — lets each admission point size the band/epoch/soft
 * rate independently (store orders vs. multi-GB file streams have very
 * different honest-request costs) while sharing one struct/verify/EWMA
 * implementation. A surface that wants the defaults above zero-inits this
 * and puzzle_gate_init() fills the zero fields from the PUZZLE_* macros. */
struct puzzle_policy {
    int      min_bits, max_bits;         /* 0 → PUZZLE_MIN/MAX_BITS   */
    int      seed_rotate_secs;           /* 0 → PUZZLE_SEED_ROTATE_SECS */
    int      ts_skew_secs;               /* 0 → PUZZLE_TS_SKEW_SECS   */
    int      ewma_halflife_secs;         /* 0 → PUZZLE_EWMA_HALFLIFE_SECS */
    int      soft_rate_per_sec;          /* 0 → PUZZLE_EWMA_SOFT_RATE */
    int      rate_step_per_sec;          /* 0 → PUZZLE_EWMA_RATE_STEP */
    int      inflight_bits;              /* 0 → PUZZLE_INFLIGHT_BITS  */
};

/* In-memory admission gate for ONE named surface. Owns the rotating
 * challenge seed, the single-use recent set, and the EWMA that drives
 * adaptive difficulty. Transient only — never persisted, never a consensus
 * predicate (same invariant as fast_sync_pow_gate, fast_sync.h:82-84). */
struct puzzle_gate {
    pthread_mutex_t lock;
    bool     initialized;
    struct puzzle_policy policy;         /* resolved (zero fields filled)   */
    uint8_t  cur_seed[32];
    int      cur_bits;
    int64_t  cur_epoch_start;
    uint8_t  prev_seed[32];
    int      prev_bits;
    bool     have_prev;
    bool     seeded;
    uint32_t inflight;
    /* EWMA state, replacing the tumbling window (fast_sync.c:707-713,720-732):
     * rate_ewma_milli is accepted-requests-per-second * 1000 (fixed point,
     * avoids float in a hot lock-held path — same idiom as
     * lib/util/src/stage.c's integer step_us_ewma, stage.c:294-319). */
    int64_t  rate_ewma_milli;
    int64_t  last_update_us;             /* platform_time_wall_time_us() */
    uint8_t  recent[PUZZLE_RECENT_CAP][32];
    uint32_t recent_head;
    uint32_t recent_count;
};

void puzzle_gate_init(struct puzzle_gate *g, const struct puzzle_policy *policy /* NULL = all defaults */);
void puzzle_gate_challenge(struct puzzle_gate *g, uint8_t out_seed[32],
                            int *out_bits, int64_t *out_server_time);
bool puzzle_gate_verify(struct puzzle_gate *g, const uint8_t peer_token[32],
                         int64_t ts, uint64_t nonce);
void puzzle_gate_serve_begin(struct puzzle_gate *g);
void puzzle_gate_serve_end(struct puzzle_gate *g);

/* Test/introspection: current EWMA rate (req/sec, fixed-point milli) and
 * live difficulty, without mutating state. Backs the EWMA unit test (§4). */
int64_t puzzle_gate_rate_ewma_milli(const struct puzzle_gate *g);
int     puzzle_gate_current_bits(const struct puzzle_gate *g);

#endif
```

### 1.2 Implementation — `lib/net/src/puzzle.c`

Body is `fast_sync.c:648-880` moved verbatim for everything except the load
term, renamed `fast_sync_pow_gate_* ` → `puzzle_gate_*`, `fs_pow_*` → `puzzle_*`,
`struct fast_sync_pow_gate` → `struct puzzle_gate`. Concretely:

- `puzzle_verify` / `puzzle_solve` = body of `fast_sync_verify_pow_ex` /
  `fast_sync_solve_pow_ex` (`fast_sync.c:648-676`), unchanged — SHA3-256
  digest layout, `fs_pow_hash_has_bits` bit-check, exhaustive nonce search.
- `puzzle_gate_init` = body of `fast_sync_pow_gate_init` (`fast_sync.c:733-756`)
  plus: resolve `policy` (any zero field → the `PUZZLE_*` default), replace
  `g->window_start = 0; g->accepted_in_window = 0;` with
  `g->rate_ewma_milli = 0; g->last_update_us = 0;`.
- Seed rotation (`fs_pow_gate_rotate_locked`, `fast_sync.c:758-772`) —
  unchanged, reads `policy.seed_rotate_secs` instead of the macro directly.
- **New**: `puzzle_ewma_tick_locked(struct puzzle_gate *g, int64_t now_us, bool accepted)`
  replaces `fs_pow_gate_refresh_window_locked`. Spec in §2.
- `puzzle_gate_adaptive_bits_locked` = `fs_pow_gate_adaptive_bits_locked`
  (`fast_sync.c:718-732`) with the load term
  `(g->accepted_in_window > soft) ? (accepted_in_window - soft) / step : 0`
  replaced by
  `(rate_per_sec_int > soft) ? (rate_per_sec_int - soft) / step : 0` where
  `rate_per_sec_int = g->rate_ewma_milli / 1000`.
- `puzzle_gate_challenge` = `fast_sync_pow_gate_challenge` (`fast_sync.c:774-793`)
  — calls the EWMA tick (accepted=false, this call doesn't itself admit a
  request) before recomputing bits, same "recompute bound to the seed just
  issued" ordering as today.
- `puzzle_gate_verify` = `fast_sync_pow_gate_verify` (`fast_sync.c:814-851`) —
  identical current-seed-then-grace-seed try, identical single-use ring
  check/remember; on the accepted path, call the EWMA tick with
  `accepted=true` instead of `g->accepted_in_window++`.
- `puzzle_gate_serve_begin/end` = verbatim (`fast_sync.c:869-886`).

`lib/net/src/fast_sync.c` then **deletes** `fast_sync_verify_pow_ex`,
`fast_sync_solve_pow_ex`, `struct fast_sync_pow_gate` and its six functions,
and `fast_sync.h` deletes the matching declarations
(`fast_sync.h:79-174`) plus the six `FAST_SYNC_POW_*` band/window macros
(`fast_sync.h:96-118`) — `fast_sync.{c,h}` keeps only the **legacy**
`fast_sync_verify_pow`/`fast_sync_solve_pow`/`struct fast_sync_pow`/
`FAST_SYNC_POW_BITS` (§3 covers whether that legacy pair is retired too).
`file_service.c` swaps its `#include "net/fast_sync.h"` PoW calls for
`#include "net/puzzle.h"` and `struct fast_sync_pow_gate g_fs_pow_gate` →
`struct puzzle_gate g_fs_pow_gate`; `fs_pow_gate()`'s return type becomes
`struct puzzle_gate *`. No change to `file_service.h`'s public shape beyond
that type rename — `fs_admit_serve_pow`, `fs_parse_serve_request`,
`FS_POW_SOLUTION_SIZE`, the wire layout, all stay byte-identical.

## 2. The EWMA — exact update rule and bits mapping

Task requirement: "difficulty keyed to a rolling per-surface request-rate
EWMA (define the EWMA update + the bits mapping)". Model: `stage.c`'s
existing integer EWMA idiom (`stage.c:297-319`, "`next = prev + (sample -
prev) / 16`", alpha = 1/16, seeded from the first sample) — reused here with
a half-life-derived alpha instead of a fixed 1/16, because puzzle admission
needs the α tunable per surface (`policy.ewma_halflife_secs`) while
`stage.c`'s is a fixed step-time metric.

**State**: `rate_ewma_milli` (int64, requests/sec × 1000, fixed-point),
`last_update_us` (int64, `platform_time_wall_time_t()*1e6` or an existing
`_us` wall-clock helper — reuse whatever `fast_sync.c` already links;
`platform/time_compat.h` is already included).

**Update, called on every `puzzle_gate_challenge` and every
`puzzle_gate_verify` attempt** (caller holds `g->lock`):

```
dt_us = now_us - last_update_us   (clamp dt_us to [0, 5_000_000] — a long
                                    idle gap decays the rate toward 0 over
                                    at most 5s of simulated elapsed time,
                                    never a multi-hour cliff from one stale
                                    sample; never negative from a clock step
                                    back)
if last_update_us == 0:            // first sample ever
    rate_ewma_milli = accepted ? 1000 : 0
else if dt_us > 0:
    // alpha per elapsed sample = 1 - 0.5^(dt_us / halflife_us), fixed-point
    // via integer halving: apply k = dt_us / (halflife_us) whole halvings
    // (bounded k <= 20 by the 5s dt clamp with a 10s half-life => k<=1,
    // so in practice this is ONE shift-right per call — cheap, no float,
    // no libm pow()).
    halflife_us = policy.ewma_halflife_secs * 1_000_000
    while (dt_us >= halflife_us && k < 20):
        rate_ewma_milli /= 2
        dt_us -= halflife_us
        k++
    // sub-halflife remainder: linear blend (good enough at this dt scale,
    // avoids a fractional-bit-shift/pow entirely)
    rate_ewma_milli -= rate_ewma_milli * dt_us / (2 * halflife_us)
last_update_us = now_us
if accepted:
    rate_ewma_milli += 1000   // this request adds "1 req" of instantaneous
                               // mass; decayed toward 0 by the next call's
                               // halving, same shape as an EWMA seeded per-
                               // event rather than per-fixed-tick
```

This is a decayed-count EWMA (each accepted request contributes a unit
impulse, decayed continuously by elapsed wall time) rather than a
fixed-window sum, so — unlike `fs_pow_gate_refresh_window_locked`'s 10s
tumbling window — there is no reset edge: a flood that starts 1 μs after
the last tick is visible immediately and decays smoothly, not in 10s steps.

**Bits mapping** (`puzzle_gate_adaptive_bits_locked`, direct successor of
`fast_sync.c:720-732`):

```c
int rate_per_sec = (int)(g->rate_ewma_milli / 1000);
int bits = policy.min_bits;
bits += (int)g->inflight * policy.inflight_bits;
if (rate_per_sec > policy.soft_rate_per_sec)
    bits += (rate_per_sec - policy.soft_rate_per_sec) / policy.rate_step_per_sec;
if (bits > policy.max_bits) bits = policy.max_bits;
if (bits < policy.min_bits) bits = policy.min_bits;
```

Same shape and same numeric constants as today's `fs_pow_gate_adaptive_bits_locked`
when `policy` is all-defaults, so `file_service.c`'s behavior does not
change (§4 equivalence test proves this at the boundary values 8/9/12/26).

## 3. Replay/expiry binding (unchanged from `fast_sync_pow_gate`, stated once here as the contract `puzzle.h` owns)

- **Freshness**: `ts` within `±policy.ts_skew_secs` of wall-clock now,
  checked in `puzzle_gate_verify` before touching the lock (cheap reject of
  stale/forward-dated `ts` — matches `fast_sync.c:825-828`).
- **Challenge binding**: solution must verify against `cur_seed` at
  `cur_bits`, or `prev_seed` at `prev_bits` during the one-epoch grace
  window (`have_prev`) — no other seed accepted, so an attacker cannot farm
  solutions against a stale challenge.
- **Peer binding**: `peer_token` is caller-supplied and MUST be a value the
  attacker cannot freely choose per attempt without cost — for
  `file_service.c` this is the handshake nonce (`session.peer_nonce`,
  `file_service.c:945-947`, "solution must be bound to it"); for the
  rebased store site (§5) it is the existing product-bound
  `SHA3-256("store:order:pow:<product_id>")` (`store_controller_pow.c:44-50`).
  `puzzle.h` does not mint `peer_token` — that stays each surface's job,
  same division as today.
- **Single-use**: `SHA3-256(seed||peer_token||ts||nonce)` recorded in a
  2048-slot ring on accept; an exact replay within the ring's retention is
  refused (`puzzle_gate_verify` mirrors `fast_sync.c:832-847`).
- **Expiry beyond the ring**: nothing evicts a `recent[]` entry early — it
  ages out only by ring wraparound (2048 accepted solutions), same as today.
  A surface issuing far more than 2048 accepted solutions per
  `ts_skew_secs` window would need a larger `PUZZLE_RECENT_CAP` or a
  policy-scoped ring size — out of scope for this design (neither existing
  site approaches that rate; flag as an open question in §6 if D-surfaces
  (§5) need it).

## 4. Re-basing the two existing sites without behavior change

### 4.1 `lib/net/src/file_service.c` (already-adaptive site)

Mechanical rename only (§1.2). Verified unchanged: `FS_POW_SOLUTION_SIZE`
wire layout (`[token(32)][ts(8 LE)][nonce(8 LE)]`, `file_service.c:944-947`),
`fs_admit_serve_pow`'s three-way return (`FS_ADMIT_CHALLENGE`/`FS_ADMIT_SERVE`/
error), `fs_parse_serve_request`'s length-based gated/ungated detection.
Equivalence proof: `lib/test/src/test_file_service_pow_gate.c`'s 7 existing
cases (`test_ungated_all_refused_gated_served`,
`test_gated_rng_parse_and_serve`, `test_replayed_solution_rejected`,
`test_solution_bound_to_connection`, plus the three IP/budget cases that
don't touch the gate) must pass unmodified against the renamed types —
prove by recompiling `test_file_service_pow_gate.c` with zero edits once
`file_service.c`'s gate type changes (a compile failure there is the
regression signal; no new assertions needed for those 4 gate-touching
cases). Add one new case, `test_gate_ewma_matches_window_at_boundary` (§4.3),
because the tumbling-window→EWMA swap is the one behavior-adjacent change
in this whole extraction.

### 4.2 `app/controllers/src/store_controller_pow.c` (legacy site)

Today: fixed 20 bits, client-chosen `peer_id`, no server challenge round
trip, hand-rolled 4096-slot replay ring
(`s_pow_used_key`/`s_pow_used_at`/`s_pow_used_next`, `store_controller_pow.c:81-118`).
Re-based shape:

- Module-static `struct puzzle_gate g_store_pow_gate;` in
  `store_controller_pow.c`, `puzzle_gate_init(&g_store_pow_gate, NULL)` at
  first use (lazy, matching `puzzle_gate_verify`'s existing
  "auto-init if `!initialized`" behavior) — the hand-rolled replay ring
  (`s_pow_used_*`) is **deleted**; `puzzle_gate_verify`'s built-in
  single-use ring replaces it.
- `store_pow_challenge()` keeps its product-binding derivation
  (`store_pow_bind_product`, `store_controller_pow.c:37-42`, unchanged —
  product isolation is store-specific policy, not puzzle-primitive
  concern) but now ALSO calls `puzzle_gate_challenge(&g_store_pow_gate, ...)`
  to hand back the live `(seed, bits, server_time)` instead of a fixed
  `FAST_SYNC_POW_BITS`. **Wire-format change**: the view layer
  (`app/views/src/store_view.c:516,544,548`, `data-pow-peer`/`data-pow-ts`/
  `data-pow-bits` form attributes) already threads a `bits` value through
  to the client JS solver (`store_view.c:183`) — it is currently always
  `FAST_SYNC_POW_BITS`; after re-basing it becomes whatever
  `puzzle_gate_challenge` returns. **This is the one visible behavior
  change this recipe introduces** — it is the intended fix (idle store now
  issues ~12-bit puzzles, ~60× cheaper for an honest buyer than today's
  fixed 20 bits, while a flooded store ramps to 26 bits) — call it out
  explicitly in the PR description, not silent.
- `store_pow_verify_and_claim()` replaces its
  `fast_sync_verify_pow(&pow)` + `store_pow_claim_once(&pow)` pair
  (`store_controller_pow.c:135-146`) with one
  `puzzle_gate_verify(&g_store_pow_gate, pow.peer_id, pow.timestamp, pow.nonce)`
  call — `pow.peer_id` (still `store_pow_bind_product`'s output) plays the
  `peer_token` role.
- `store_controller_pow.c:44-50`'s `store_pow_bind_product` stays — it is
  the store's own peer-token derivation, out of `puzzle.h`'s scope per §3.

Equivalence bar for this site (since the wire format visibly changes,
"equivalence" means "the CSRF-then-PoW-then-mint gate still refuses every
input it refused before and admits a correctly-solved current puzzle" —
not byte-identical bits): new
`lib/test/src/test_store_pow_puzzle_rebase.c` proving (a) a solve against a
freshly issued challenge admits, (b) an unsolved/garbage `pow_nonce`
refuses (`store_pow_verify_and_claim` returns false, matching
`store_controller_pow.c:143` today), (c) replay of an already-admitted
solution refuses, (d) two different `product_id`s get independent
`peer_id`s so a solve for product A does not admit an order for product B
(this was already true via `store_pow_bind_product`'s per-product hash —
must survive the rebase), (e) a `pow_ts` outside the skew window refuses.
This subsumes what the deleted `store_pow_claim_once`'s ring logic proved.

### 4.3 New unit test: EWMA behavior

`lib/test/src/test_puzzle.c`, new file, covers:

- **KAT vectors for `puzzle_verify`**: fixed `(challenge_seed, peer_token,
  ts, nonce, difficulty_bits)` tuples with the expected SHA3-256 digest and
  expected true/false — port the existing implicit coverage in
  `solve_for_gate()`/`fast_sync_solve_pow_ex` round-trips
  (`test_file_service_pow_gate.c:36-47`) into explicit fixed-vector
  assertions (compute the digests once with the current implementation,
  freeze them as the KAT — this is what turns "the hash still matches" into
  a byte-frozen regression gate, not just a round-trip smoke test).
- **EWMA unit test** (`test_ewma_rises_under_sustained_load_decays_when_idle`):
  drive `puzzle_gate_verify` with N synthetic accepted solves at a
  controlled fake `now_us` cadence (the gate needs a test-only clock seam —
  either inject `now_us` as a parameter to an internal `_locked` helper
  exposed under a `#ifdef ZCL_TEST_HOOKS` seam, mirroring how
  `stage_step_budget_exceeded_test_set_ewma_us` exposes a test-only EWMA
  setter for `stage.c`'s EWMA, per
  `lib/test/src/test_stage_step_budget_exceeded.c:105`) and assert:
  (a) `puzzle_gate_rate_ewma_milli` rises monotonically across bursts spaced
  well inside the half-life, (b) `puzzle_gate_current_bits` reaches
  `policy.max_bits` under sustained load ≥ `soft_rate + (max-min)*rate_step`
  req/sec, (c) after a synthetic idle gap of several half-lives,
  `rate_ewma_milli` decays back under `soft_rate*1000` and
  `puzzle_gate_current_bits` returns to `policy.min_bits`, (d) no reset-edge
  artifact — sampling immediately after a tick never reads exactly 0 unless
  the gate was actually freshly initialized (this is the property the
  window→EWMA swap exists to fix; assert it explicitly against the OLD
  window behavior being what it replaces, i.e. this test would have FAILED
  against `fs_pow_gate_refresh_window_locked`'s hard reset).
- **`test_gate_ewma_matches_window_at_boundary`** (lives in
  `test_file_service_pow_gate.c` per §4.1, small): drives exactly
  `PUZZLE_EWMA_SOFT_RATE` and `PUZZLE_EWMA_SOFT_RATE+1` accepted solves
  within one `PUZZLE_EWMA_HALFLIFE_SECS` window and asserts
  `puzzle_gate_current_bits` stays at `min_bits` in the first case and rises
  by exactly 1 in the second — the same boundary the old window test
  implicitly relied on (`fs_pow_gate_adaptive_bits_locked`'s
  `FAST_SYNC_POW_SOFT_RATE=8`), now on the EWMA path.

## 5. Abusable unauthenticated surfaces to protect as the public surface grows

Ranked by what's live/near-live per `docs/HANDOFF.md` and the CLAUDE.md
feature list, each with the `puzzle_policy` this design implies:

| Surface | File | Today | `puzzle_policy` |
|---|---|---|---|
| **File-market chunk stream** | `lib/net/src/file_service.c` | ALREADY GATED via `fast_sync_pow_gate` → rebases onto `puzzle_gate` (§4.1), no policy change | defaults (`min=12,max=26,soft=8,step=4`) — proven live-shaped by 07-15's real traffic |
| **Store order mint** | `app/controllers/src/store_controller_pow.c` | GATED but fixed-difficulty, non-adaptive (§4.2 fixes this) | tighter ceiling than file-service — an order write is cheaper to serve than a multi-GB stream but mints a real z-address (real CPU) + a DB row; recommend `min=14 (client already solves ~1M-hash puzzles per store_controller_pow.c's own comment), max=24, soft=2, step=2` — a store realistically sees far fewer req/sec than a snapshot server, so the ramp should trigger earlier |
| **SHA3 UTXO snapshot serving** (`fast_sync.c`'s `SNAPSHOT_REQUEST`/`zsnapreq` path, `MSG_SNAPSHOT_REQ`, `fast_sync.h:31`) | `lib/net/src/fast_sync.c` (P2P side, distinct from `file_service.c`'s HTTP-ish stream) | **UNGATED today** — `fast_sync_rate_check`/`struct fast_sync_rate_limiter` (`fast_sync.h:130-145`) bounds volume per-IP-per-hour but has no PoW gate in front of the initial `SNAPSHOT_REQUEST`; a flood of snapshot requests each triggers `fast_sync_build_offer`'s `O(n)` `fast_sync_compute_utxo_root_db` fallback path when the cache misses (`fast_sync.c:180-190`) | **new work, not just rebase**: wire a `struct puzzle_gate g_snapshot_pow_gate` the same way `file_service.c` wires `g_fs_pow_gate`, gating `SNAPSHOT_REQUEST` before `fast_sync_build_offer` runs; `min=12,max=26,soft=8,step=4` (same shape as file-service — both are "expensive server-side work behind a P2P peer-facing verb") |
| **Package/binary swarm** (named in CLAUDE.md's vision line, not yet a controller in this tree — `grep -rn "package.*swarm" app lib` finds no hits) | N/A — not built yet | not built | when built: same shape as file-service (`puzzle_gate` + IP-concurrency table modeled on `fs_ip_serve_acquire`/`fs_ip_bytes_charge`, `file_service.c:970-1002`) — flag as a template to copy, not a new pattern to invent |
| **ZNAM name registration** (`REGISTER` op, `docs/work/os-substrate-plan.md` / CLAUDE.md ZNAM section) | on-chain OP_RETURN, mempool-fee-gated already (a real tx pays a real fee) | N/A | **not a puzzle-gate candidate** — the existing fee market is the correct admission control here; a client puzzle would be redundant defense-in-depth at best, out of scope |
| **ZMSG P2P inbox** (`msg_send`/`zmsg`) | `lib/net/src/zmsg.c` | plaintext P2P, no PoW | if P2P messaging becomes public-facing (today it's peer-to-peer between connected nodes, not an open HTTP surface), same `puzzle_policy` shape as store (cheap per-message write, moderate ceiling) |

## 6. Acceptance bar

- `make build-only` green with `lib/net/puzzle.{c,h}` added,
  `fast_sync_pow_gate`/`fast_sync_verify_pow_ex`/`fast_sync_solve_pow_ex`
  and the six `FAST_SYNC_POW_{MIN,MAX}_BITS`/window macros removed from
  `fast_sync.{c,h}`.
- `lib/test/src/test_puzzle.c`: KAT vectors for `puzzle_verify` (§4.3) +
  the EWMA rise/decay/no-reset-edge test, all green.
- `lib/test/src/test_file_service_pow_gate.c`: existing 7 cases compile and
  pass unmodified against the `puzzle_gate` rename (behavioral-equivalence
  proof for site #3/file-service) + the new
  `test_gate_ewma_matches_window_at_boundary` case.
- `lib/test/src/test_store_pow_puzzle_rebase.c` (new): 5 cases per §4.2,
  green.
- `make test-parallel` — authoritative "ALL TESTS PASSED — 0/N" (per
  CLAUDE.md; `test_zcl` directly is never the gate).
- `make lint` green — no new gate is required by this design (no consensus
  predicate, no AR write, no new command-registry surface); if a reviewer
  wants a standing gate ("every `puzzle_gate` instance has a documented
  `puzzle_policy` in a comment"), that is a follow-up, not this recipe's bar.
- PR description explicitly calls out the one visible behavior change: the
  store's puzzle difficulty is no longer a fixed 20 bits (§4.2).

## 7. Collision check against in-flight lanes

Read `docs/work/os-substrate-plan.md`'s lane list is not needed here — the
governing text is the plan of record
(`/home/rhett/.claude/plans/think-more-about-our-keen-crown.md`). Checked
this session:

- **B1** (`OS-B1`, plan lines 188-193) — `zcl_command_registry_validate`,
  `zcl_command_spec`, `check-command-contract` — no file overlap.
- **A2** (`OS-A2`, plan lines 114-119) — `boot.c`, `os_sandbox_linux.c`,
  `thread_registry_spawn`, `check-sandbox-wired` — no file overlap.
- **B3b** (plan line 248, REST parity gate) — not yet fully read (its own
  file set is under `app/controllers` REST resources, per
  `docs/work/MCP-REMOVAL-WORKLIST.md`) — no `lib/net/` or
  `store_controller_pow.c` files named anywhere near it in the plan text.
- **D2** (`OS-D2`, plan lines 165-168) — `znam_projection.c`,
  `projection_util.h` — no overlap.

This design's touched/new files —
`lib/net/puzzle.{c,h}` (new), `lib/net/src/fast_sync.{c,h}` (leaf, PoW
section only), `lib/net/src/file_service.c` (leaf, gate-type rename only),
`app/controllers/src/store_controller_pow.c` (leaf),
`app/views/src/store_view.c` (leaf, `data-pow-bits` now reads a live value
instead of the `FAST_SYNC_POW_BITS` macro), plus three new/extended test
files — are disjoint from all four named lanes. **Zero collision risk.**

Consensus-parity risk: **none** — every touched primitive is explicitly an
in-memory, never-persisted, never-a-consensus-predicate admission gate
(stated in `fast_sync.h:82-84` today and carried into `puzzle.h`'s own
comment in §1.1); `CONSENSUS_PARITY_DOCTRINE.md`'s gate
(`check-consensus-parity`) and `test_consensus_parity` are untouched by
this design.

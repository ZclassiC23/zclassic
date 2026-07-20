/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

/* Pure sync reducer — the deterministic core of snapshot fast-sync.
 *
 * `sync_reduce(state, event)` is a total, side-effect-free transition
 * function: given the current POD `sync_kernel_state` and one `sync_event`
 * it returns a `sync_decision` (the next phase + a bounded list of side-effect
 * INTENTS + an optional typed blocker). It touches no clock, RNG, socket, DB,
 * or global — everything it needs is in its two by-value arguments, and its
 * output is data the impure adapter later executes. This is the same
 * OUT-struct / pure-planner discipline as sync_planner.h, taken to its limit:
 * the whole FSM is one function.
 *
 * It exists to collapse the triple-copied snapshot FSM (the atomic global in
 * sync_state.c, the singleton `.state` field, and the per-peer zsync_* shadow
 * fields) onto one authority that can be exhaustively tested and fuzzed. This
 * slice lands the kernel in SHADOW mode only — the reference service stays
 * authoritative; the adapter merely asserts agreement.
 *
 * Containment is UNREPRESENTABLE here: the action enum has no ACTIVATE /
 * PUBLISH value, mirroring g_snapsync_transitions[VERIFYING][COMPLETE]=false
 * in sync_state.c. The furthest a decision can carry state is STAGED, and the
 * only thing it can do at the activation boundary is RAISE_CONTAINMENT_BLOCKER.
 *
 * Dependency law: this header includes ONLY sync-local + core headers — no
 * net.h, no <pthread.h>, no sqlite. That keeps the kernel linkable into the
 * test harness and the (future) simulator without dragging the node in. */

#ifndef ZCL_SYNC_REDUCE_H
#define ZCL_SYNC_REDUCE_H

#include "core/zcl_ids.h"

#include <stdbool.h>
#include <stdint.h>

/* ── Phases / events / actions (generated from the .def catalog) ────── */

enum sync_phase {
#define SYNC_PHASE(id, name) SYNC_PHASE_##id,
#include "sync/sync_kernel_catalog.def"
    SYNC_PHASE_COUNT
};

enum sync_event_kind {
#define SYNC_EVENT(id, name) SYNC_EVENT_##id,
#include "sync/sync_kernel_catalog.def"
    SYNC_EVENT_COUNT
};

enum sync_action {
#define SYNC_ACTION(id, name) SYNC_ACTION_##id,
#include "sync/sync_kernel_catalog.def"
    SYNC_ACTION_COUNT
};

/* Typed containment/failure blocker a decision may raise. Kept sync-local
 * (no dependency on lib/util/blocker.h) so the kernel stays pure. */
enum sync_blocker {
    SYNC_BLOCKER_NONE = 0,
    SYNC_BLOCKER_ACTIVATION_CONTAINED,   /* verified state may NOT self-activate */
    SYNC_BLOCKER_PROOF_FAILED,
    SYNC_BLOCKER_PEER_LOST,
    SYNC_BLOCKER_TIMEOUT,
    SYNC_BLOCKER_COUNT
};

/* ── POD state / event / decision ───────────────────────────────────── */

/* Fixed cap on the intents a single transition may emit. */
#define SYNC_DECISION_MAX_ACTIONS 4

/* The complete reducer state. Plain data only — no pointers, mutexes, or
 * handles — so it copies by value and a stale `session_id` is a pure compare. */
struct sync_kernel_state {
    uint64_t               session_id;      /* 0 == no active session */
    enum sync_phase        phase;
    struct zcl_height      height;          /* target/anchor height of the artifact */
    struct zcl_chunk_index chunks_total;
    struct zcl_chunk_index chunks_received;
    struct zcl_chunk_root  chunk_root;      /* artifact content identity */
    struct zcl_utxo_root   utxo_root;       /* claimed UTXO root (unproven until VERIFYING passes) */
    struct zcl_peer_id     peer;            /* the peer this session is bound to */
    bool                   proof_ok;        /* set only by a PROOF_VERIFIED fold */
};

/* One input to the reducer. `session_id` must match the state's or the event
 * is stale (⇒ zero actions, unchanged state). */
struct sync_event {
    uint64_t               session_id;
    enum sync_event_kind   kind;
    struct zcl_chunk_index chunk_index;     /* CHUNK_RECEIVED / CHUNK_REJECTED */
    struct zcl_chunk_root  chunk_root;      /* OFFER_RECEIVED */
    struct zcl_utxo_root   utxo_root;       /* PROOF_VERIFIED */
    struct zcl_peer_id     peer;
    struct zcl_height      height;          /* OFFER_RECEIVED */
    struct zcl_chunk_index chunks_total;    /* OFFER_RECEIVED */
    bool                   proof_ok;        /* PROOF_VERIFIED payload */
};

/* The reducer's output: next phase, a bounded ordered list of side-effect
 * intents, and an optional typed blocker. Pure data; the adapter executes it.
 *
 * Layout is deliberately padding-free: `blocker` precedes `has_blocker` so the
 * trailing gap after the bool is an EXPLICIT `_reserved[]` member, not implicit
 * padding. This is load-bearing for the byte-identical-decision invariant —
 * C copies struct MEMBERS on return-by-value, not padding bytes, so a struct
 * with implicit padding would carry indeterminate padding into the caller and
 * make `memcmp` over two decisions from identical inputs non-deterministic.
 * memset(&d,0,sizeof d) in decision_base zeroes _reserved; being a real member
 * it is then copied on every return. The static_assert pins the no-padding
 * shape so a future field reorder can't silently reintroduce the gap. */
struct sync_decision {
    enum sync_phase   next;
    enum sync_action  actions[SYNC_DECISION_MAX_ACTIONS];
    int               action_count;
    enum sync_blocker blocker;
    bool              has_blocker;
    uint8_t           _reserved[3];
};
static_assert(sizeof(struct sync_decision) ==
                  sizeof(enum sync_phase) +
                  sizeof(enum sync_action) * SYNC_DECISION_MAX_ACTIONS +
                  sizeof(int) + sizeof(enum sync_blocker) +
                  sizeof(bool) + 3,
              "sync_decision must be padding-free so memcmp determinism holds");

/* ── The reducer + name lookups ─────────────────────────────────────── */

/* Total, pure, deterministic. Same inputs ⇒ byte-identical decision. A stale
 * event (event.session_id != state.session_id, with a non-zero state session)
 * yields next==state.phase and action_count==0. */
[[nodiscard]] struct sync_decision sync_reduce(struct sync_kernel_state state,
                                               struct sync_event event);

/* Stable lowercase names; NULL-safe out-of-range → "?" sentinel string. */
const char *sync_phase_name(enum sync_phase phase);
const char *sync_event_name(enum sync_event_kind kind);
const char *sync_action_name(enum sync_action action);

#endif /* ZCL_SYNC_REDUCE_H */

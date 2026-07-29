/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * body_history — "which block bodies BELOW my tip am I missing?", answered
 * with three outcomes instead of two.
 *
 * The defect this exists to remove: nothing in the node ever asked for a
 * height below its own tip. gap_fill_compute_window() builds its window as
 * [tip+1, best_header] by construction and returns "no work" the moment
 * best_header <= tip, so a node holding genesis plus the last few thousand
 * blocks — and missing 98% of the chain's bodies — went idle and reported
 * itself at tip. The hole was not merely unfixed, it was UNSAYABLE.
 *
 * The trap in fixing it: a census that answers "0 missing" when it could
 * not read the block index reproduces the same class of bug one level up.
 * "I looked and found nothing missing" and "I could not look" must never
 * be the same published value, the same absence, or the same silence. So
 * this module publishes THREE states and defaults to the pessimistic one:
 *
 *     BODY_HISTORY_UNKNOWN    (= 0)  could not determine — the DEFAULT
 *     BODY_HISTORY_INCOMPLETE        holes positively found
 *     BODY_HISTORY_COMPLETE          positively established: no holes
 *
 * UNKNOWN is zero so every zero-initialized struct, every unstarted
 * service, and every failed allocation lands there without anyone having
 * to remember to. Only a scan that positively covered every height in the
 * window can move the verdict to COMPLETE, and only COMPLETE lets the node
 * say "at tip" (see body_history_is_proven / syncsvc_plan_periodic_tip_state).
 *
 * Two range maps, both plain `struct body_coverage_map` (this module adds
 * no second range algebra — it reuses storage/body_coverage.h):
 *
 *     held      heights whose body is on disk (BLOCK_HAVE_DATA)
 *     measured  heights the census actually LOOKED AT and got a definite
 *               answer for, whether the answer was have or missing
 *
 * From those two, over a closed window [lo, hi]:
 *
 *     missing    = (measured u held) n window, minus held n window
 *     unmeasured = window, minus (measured u held) n window
 *
 * A height is only ever entered into `measured` on a DEFINITE probe result.
 * An unreadable index entry, a NULL active-chain slot, a missing block hash
 * — all classify BODY_HISTORY_PROBE_INDETERMINATE and enter neither map, so
 * they stay counted as unmeasured and hold the verdict at UNKNOWN. That is
 * the whole fail-closed contract, and lib/test/src/test_body_history.c
 * pins it.
 */

#ifndef ZCL_STORAGE_BODY_HISTORY_H
#define ZCL_STORAGE_BODY_HISTORY_H

#include "storage/body_coverage.h"
#include "core/uint256.h"

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

/* Named blocker: the node cannot prove it holds the bodies for its own
 * history — either because it found holes, or because it has not
 * established coverage at all. Both are disqualifying; the condition body
 * reports which. */
#define BODY_HISTORY_UNPROVEN_BLOCKER "chain.body_history_unproven"

/* progress_meta keys for the resumable census. */
#define BODY_HISTORY_MEASURED_META_KEY "body_history_measured"
#define BODY_HISTORY_CURSOR_META_KEY   "body_history_cursor"

/* ── The three-outcome verdict ──────────────────────────────────── */

enum body_history_status {
    /* Fail-closed default. Reached by zero-initialization, by a failed
     * allocation, by a NULL argument, and by any window with even one
     * height nobody has definitively probed. NEVER means "no holes". */
    BODY_HISTORY_UNKNOWN = 0,
    /* At least one height in the window was probed and found to have no
     * body on disk. */
    BODY_HISTORY_INCOMPLETE = 1,
    /* Every height in the window was probed AND every one had its body.
     * The only value that permits an at-tip claim. */
    BODY_HISTORY_COMPLETE = 2,
};

const char *body_history_status_name(enum body_history_status s);

struct body_history_verdict {
    enum body_history_status status;

    int64_t window_lo;
    int64_t window_hi;
    int64_t window_heights;   /* hi - lo + 1, or 0 for an unusable window */

    int64_t held_count;       /* heights in window whose body is on disk */
    int64_t missing_count;    /* probed, definite, and absent */
    int64_t lowest_missing;   /* -1 when none known */

    int64_t unmeasured_count; /* never definitively probed */
    int64_t lowest_unmeasured;/* -1 when none */
};

/* Derive the verdict for the closed window [lo, hi].
 *
 * Returns false — leaving *out at the UNKNOWN default — on any argument or
 * allocation problem. A false return is itself "could not determine": the
 * caller must publish *out as-is, never substitute a cheerier answer.
 *
 * `measured` may legitimately omit heights present in `held` (the above-tip
 * gap-fill feed writes coverage without going through the census), so the
 * two are unioned internally; the union is what "definitively probed"
 * means. */
bool body_history_evaluate(const struct body_coverage_map *held,
                           const struct body_coverage_map *measured,
                           int64_t lo, int64_t hi,
                           struct body_history_verdict *out);

/* True only for BODY_HISTORY_COMPLETE. Written as a function rather than a
 * comparison so every "may I say I am at tip?" site reads the same and no
 * site can accidentally accept UNKNOWN by testing `!= INCOMPLETE`. */
bool body_history_verdict_is_proven(const struct body_history_verdict *v);

/* ── Bounded, resumable census ──────────────────────────────────── */

/* A per-height probe answer. INDETERMINATE is 0 so a caller that forgets to
 * write a slot, or a probe that fails, lands there. */
enum body_history_probe {
    BODY_HISTORY_PROBE_INDETERMINATE = 0,
    BODY_HISTORY_PROBE_HAVE = 1,
    BODY_HISTORY_PROBE_MISSING = 2,
};

/* Probe one height.
 *
 * CONTRACT: return INDETERMINATE — never MISSING, never HAVE — when the
 * index cannot be read, the height is not indexed, or no block hash is
 * available. `out_hash` must be filled for a MISSING answer (it is what
 * gets enqueued) and may be filled for HAVE; it is ignored otherwise. */
typedef enum body_history_probe (*body_history_probe_fn)(
        int64_t height, struct uint256 *out_hash, void *ctx);

/* Bound on how many heights one census pass examines. The walk must never
 * stall the node: 4096 probes is a few hundred microseconds of array
 * indexing and flag reads, and a full 3.2M-height sweep completes in ~780
 * passes (~65 minutes at the 5 s gap-fill cadence) while never holding
 * cs_main for more than one window. */
#define BODY_HISTORY_CENSUS_BUDGET 4096

/* Bound on how many missing bodies one pass hands to the download manager.
 * The queue is height-sorted and below-tip work sorts AHEAD of tip-chasing
 * work, so an unbounded backfill would starve live sync outright. 64 per
 * pass is ~13 blocks/second of steady drip, enough to close a 3.1M-block
 * hole in the background without ever occupying more than a small fraction
 * of the in-flight window (DL_MAX_IN_FLIGHT_TOTAL is 1024 at tip). The
 * caller additionally gates on download-queue headroom. */
#define BODY_HISTORY_ENQUEUE_MAX 64

/* Resumable descending cursor. Zero-initialized state is "nothing measured,
 * no sweep completed", i.e. UNKNOWN. */
struct body_history_census {
    /* Next height to examine. The sweep walks DOWN from window_hi so the
     * heights nearest the tip — the ones a peer is most likely to ask for
     * and the reducer most likely to need — are established first. */
    int64_t cursor;
    bool    cursor_valid;

    int64_t window_lo;
    int64_t window_hi;

    uint64_t passes;
    uint64_t sweeps_completed;
    uint64_t heights_examined;
    uint64_t heights_have;
    uint64_t heights_missing;
    uint64_t heights_indeterminate;
    uint64_t blocks_enqueued;

    int64_t last_pass_lo;
    int64_t last_pass_hi;
};

struct body_history_pass_result {
    int64_t lo;
    int64_t hi;
    int64_t examined;
    int64_t have;
    int64_t missing;
    int64_t indeterminate;
};

void body_history_census_init(struct body_history_census *c);

/* Choose the next bounded descending sub-window inside [window_lo,
 * window_hi]. Returns true and fills out_lo and out_hi when there is a window
 * to probe; false when the window is unusable (empty, negative, or budget
 * <= 0). Restarting after a completed sweep is deliberate: it is how the
 * node notices a body that was pruned, corrupted, or never landed. */
bool body_history_census_plan(struct body_history_census *c,
                              int64_t window_lo, int64_t window_hi,
                              int64_t budget,
                              int64_t *out_lo, int64_t *out_hi);

/* Probe every height in [lo, hi] into `classes` (index i == height lo + i)
 * and `hashes`. Does NOT touch any map, take any lock, or call the download
 * manager — the caller runs this while holding whatever read lock the probe
 * needs, then releases it before folding and enqueueing.
 *
 * `classes` is filled with INDETERMINATE first, so an early return, a short
 * window, or a probe that never writes a slot leaves that height unmeasured
 * rather than silently "fine". Returns the number of heights attempted (0
 * on any argument problem). */
size_t body_history_census_probe_window(int64_t lo, int64_t hi,
                                        body_history_probe_fn probe,
                                        void *ctx,
                                        uint8_t *classes,
                                        struct uint256 *hashes,
                                        size_t cap);

/* Fold one classified window into the two maps and the census counters.
 * Runs of equal classification are inserted as ranges, so a contiguous
 * chain stays a handful of ranges regardless of window size.
 *
 * INDETERMINATE heights are entered into NEITHER map — that is what keeps
 * "could not look" distinguishable from "looked and it was there". Returns
 * false on a NULL argument or an insert allocation failure; on false the
 * caller must treat the pass as having measured nothing. */
bool body_history_census_fold(struct body_history_census *c,
                              struct body_coverage_map *held,
                              struct body_coverage_map *measured,
                              int64_t lo,
                              const uint8_t *classes, size_t n,
                              struct body_history_pass_result *out);

/* Advance the cursor past a folded window and count the pass. Split from
 * fold() so a caller whose fold failed does not skip the window. */
void body_history_census_advance(struct body_history_census *c,
                                 int64_t lo, int64_t hi);

/* Collect up to `cap` missing heights from a classified window, lowest
 * first, into caller arrays ready for dl_queue_blocks. Returns the count. */
size_t body_history_census_collect_missing(int64_t lo,
                                           const uint8_t *classes,
                                           const struct uint256 *hashes,
                                           size_t n,
                                           struct uint256 *out_hashes,
                                           int32_t *out_heights,
                                           size_t cap);

/* ── Process-wide singleton ─────────────────────────────────────── */

/* The census, the measured map, and the last published verdict. The `held`
 * map is NOT duplicated here — it is body_coverage_global_map(), the one
 * canonical record of which bodies are on disk. Lock/unlock brackets every
 * access to all of them; body_history_global_lock() takes the body_coverage
 * global lock too, in that order, so there is exactly one lock discipline. */
void body_history_global_lock(void);
void body_history_global_unlock(void);
struct body_history_census *body_history_global_census(void);
struct body_coverage_map   *body_history_global_measured(void);

/* Publish a verdict computed by the census driver. Copies; never retains.
 * Takes the body_history lock itself, so call it AFTER
 * body_history_global_unlock(), never inside the bracket. Publishing NULL
 * publishes ignorance (UNKNOWN) rather than leaving the last good news
 * standing — a driver that could not compute a verdict must say so. */
void body_history_publish(const struct body_history_verdict *v);

/* Read the last published verdict. Returns false (leaving *out UNKNOWN) if
 * nothing has been published yet — a node that has not run a census does
 * not get to claim one. */
bool body_history_get_verdict(struct body_history_verdict *out);

/* The single question every at-tip / completeness gate asks. False unless a
 * census has positively established full coverage. Safe before init, safe
 * on a node with no chain, safe from any thread. */
bool body_history_is_proven(void);

/* The last published status, or BODY_HISTORY_UNKNOWN when nothing has been
 * published. Pass this straight into any gate that must refuse an at-tip /
 * complete claim: an unmeasured node returns the same value a node with a
 * known hole would be refused for. */
enum body_history_status body_history_status_now(void);

/* Reset to the pristine UNKNOWN state (test + boot use). */
void body_history_reset(void);

/* Durable resume so a restart does not rescan 3.2M heights. Best-effort:
 * a failure leaves the in-memory state UNKNOWN, which is correct. */
struct sqlite3;
bool body_history_save(struct sqlite3 *db);
bool body_history_load(struct sqlite3 *db);

/* See CLAUDE.md "Adding state introspection". Reentrant-safe. */
struct json_value;
bool body_history_dump_state_json(struct json_value *out, const char *key);

#endif /* ZCL_STORAGE_BODY_HISTORY_H */

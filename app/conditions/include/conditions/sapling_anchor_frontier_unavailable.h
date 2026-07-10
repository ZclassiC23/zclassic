/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONDITIONS_SAPLING_ANCHOR_FRONTIER_UNAVAILABLE_H
#define ZCL_CONDITIONS_SAPLING_ANCHOR_FRONTIER_UNAVAILABLE_H

#include <stdbool.h>

struct sqlite3;

/* Classification of the durable Sapling anchor-history gap, used both by the
 * condition's detect and by the hermetic tests.  Pure over the store. */
enum sapling_anchor_gap_class {
    /* No curable gap: from-genesis store (activation==0), no HISTORY_INCOMPLETE
     * symptom, or a store-read error. */
    SAPLING_ANCHOR_GAP_NONE = 0,
    /* The birth defect: adoption cursor > 0 over an EMPTY anchor table, so the
     * latest-frontier lookup returns HISTORY_INCOMPLETE.  Seed-curable — a
     * header-verified initial frontier resumes the fold. */
    SAPLING_ANCHOR_GAP_EMPTY_TABLE = 1,
    /* Rows exist but a specific below-cursor historical root is absent.  NOT
     * seed-curable: the owner-gated genesis-to-cursor backfill is required. */
    SAPLING_ANCHOR_GAP_HISTORICAL = 2,
};

/* Classify the SAPLING pool's anchor gap from the progress store.  Reentrant;
 * no allocation.  See enum above. */
enum sapling_anchor_gap_class sapling_anchor_frontier_classify(struct sqlite3 *db);

void register_sapling_anchor_frontier_unavailable(void);

#ifdef ZCL_TESTING
void sapling_anchor_frontier_unavailable_test_reset(void);
int sapling_anchor_frontier_unavailable_test_remedy_calls(void);
#endif

#endif /* ZCL_CONDITIONS_SAPLING_ANCHOR_FRONTIER_UNAVAILABLE_H */

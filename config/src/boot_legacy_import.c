/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * PURPOSE: Pure boot-heuristic predicate for the automatic zclassicd-LevelDB
 * header pull (config/src/boot.c app_init, the need_zcd decision). Split out
 * of boot.c to keep boot.c under the E1 file-size ceiling — this file is the
 * "seam" the check-file-size-ceiling gate asks for. */

#include "config/boot.h"

#include <stdint.h>

/* Decide whether boot should auto-pull zclassicd's LevelDB block index.
 * Two independent triggers, either fires it (both require a legacy source
 * to actually be present — callers stat() it and pass the result in):
 *   1. RATIO: local_index_size has fewer than 90% of the expected chain
 *      height's entries — e.g. after a UTXO-snapshot sync our own index
 *      has only a handful of entries with scrambled heights.
 *   2. EMPTY DATADIR: local_index_size == 0. On a genuinely fresh/empty
 *      datadir chain_h is ALSO 0 (nothing to estimate it from yet), so
 *      the ratio test degenerates to `0 < 0` — always false — and would
 *      never fire without this explicit case. Without it a fresh node
 *      silently falls back to a slow P2P header crawl instead of the
 *      ~60-second legacy import. Invariant: local==0 with a legacy
 *      source present must ALWAYS trigger the pull. */
bool boot_need_legacy_header_pull(int64_t local_index_size, int64_t chain_h,
                                  bool legacy_source_present)
{
    if (!legacy_source_present)
        return false;
    if (local_index_size <= 0)
        return true;
    return local_index_size < chain_h * 9 / 10;
}

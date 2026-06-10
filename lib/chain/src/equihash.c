/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php.
 *
 * Thin legacy-shape wrapper around the pure domain verifier in
 * domain/consensus/equihash.{c,h}. The bool-returning signature is
 * preserved so existing call sites compile unchanged; the domain
 * function returns a zcl_result, which this wrapper inspects, logs
 * on contract failures, and collapses to a bool. */

#include "chain/equihash.h"
#include "domain/consensus/equihash.h"
#include "util/log_macros.h"

bool check_equihash_solution(const struct block_header *header,
                             const struct chain_params *params)
{
    /* The pure verifier only needs the consensus_params slice (which
     * it currently ignores anyway), not the whole chain_params bundle.
     * Pass NULL to mean "no fork gating overrides"; the legacy code
     * never read params here either. */
    (void)params;

    bool valid = false;
    struct zcl_result r =
        domain_consensus_verify_equihash_solution(header, NULL, &valid);
    if (!r.ok)
        LOG_FAIL("equihash", "verify failed: code=%d msg=%s",
                 r.code, r.message[0] ? r.message : "(none)");
    return valid;
}

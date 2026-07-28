/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * fuzz_overlay — libFuzzer harness for every overlay wire decoder that
 * reads attacker-chosen bytes: the three OP_RETURN parsers (ZSLP, ZNAM,
 * ZDIR) and the four zid record codecs (identity doc, release body,
 * inclusion proof, signed endpoint, service descriptor).
 *
 * THE REACH PATH. Anyone who can get a transaction into a block chooses
 * the bytes of an OP_RETURN output outright — there is no signature, no
 * script execution and no size discipline beyond the relay cap on the
 * payload, and every indexing node parses it:
 *
 *   peer block -> connect_block -> explorer index fold
 *     -> app/models/src/explorer_index_overlays.c
 *          slp_parse(tx->vout[0].script_pub_key.data, ...)
 *          znam_parse(script, script_len, ...)
 *     -> app/models/src/explorer_index_zdir.c:161
 *          zdir_parse(script, script_len, ...)
 *   and again on render, app/controllers/src/explorer_controller_block.c:220
 *   and explorer_controller_tx.c:379, for anyone browsing the explorer.
 *
 * The zid codecs take the same shape of input from a different pipe:
 * lib/vcs/src/zdesc_swarm.c:148 and lib/vcs/src/zendp_swarm.c:248 call
 * zid_doc_decode on swarm-fetched record bytes BEFORE any signature is
 * checked, then hand the decoded body to zdesc_decode_body /
 * zendp_decode_body. So the decoders below run on unauthenticated bytes
 * in every case; the signature, when there is one, is checked after.
 *
 * None of these seven functions allocates — they are pure codecs writing
 * into caller-owned structs (verified: slp/znam/zdir fill fixed-size
 * message structs via read_push/overlay_reader, which return pointers
 * INTO the input; lib/zid is documented "no allocation anywhere: caller
 * buffers only"). There is therefore nothing to free on either the
 * success or the failure branch, and no output buffer outlives an input.
 * What must hold is that not one of them reads past `size` or faults on
 * ANY input. Runs with -fsanitize=fuzzer,address,undefined under clang.
 *
 * Byte 0 selects the decoder and the rest is the payload, so one binary
 * reaches all of them and libFuzzer learns the leading discriminator
 * within the first few hundred execs.
 */

#include "chain/chainparams.h"
#include "zdir/zdir.h"
#include "zid/zdesc.h"
#include "zid/zendp.h"
#include "zid/zid.h"
#include "znam/znam.h"
#include "zslp/slp.h"

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Required by the dependency graph of the sources linked into this
 * binary (process/sync globals). Provided by main.c in the real binary
 * and by test.c in the suite; the fuzzer is neither, so the global lives
 * here — same as fuzz_block.c and fuzz_tx_bundle.c. */
volatile sig_atomic_t g_shutdown_requested = 0;

/* Number of demux arms. Keep in sync with the switch below and with the
 * leading byte of every seed in lib/test/fuzz_seeds/overlay/. */
#define FUZZ_OVERLAY_ARMS 7

/* Every record here is small: MAX_OP_RETURN_RELAY is 223, ZID_DOC_MAX is
 * 1139 and ZID_PROOF_WIRE_MAX is 2067. Cap well above the largest so the
 * corpus stays in the interesting range, and so the uint16_t body_len
 * casts below are always exact. */
#define FUZZ_OVERLAY_MAX_INPUT 8192u

int LLVMFuzzerInitialize(int *argc, char ***argv);
int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerInitialize(int *argc, char ***argv)
{
    (void)argc; (void)argv;
    chain_params_select(CHAIN_MAIN);
    return 0;
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0 || size > FUZZ_OVERLAY_MAX_INPUT)
        return 0;  /* libFuzzer convention: return 0 means "keep going" */

    const uint8_t arm = (uint8_t)(data[0] % FUZZ_OVERLAY_ARMS);
    const uint8_t *payload = data + 1;
    const size_t payload_len = size - 1;
    /* Exact by the size cap above. */
    const uint16_t body_len = (uint16_t)payload_len;

    switch (arm) {
    case 0: {
        /* ZSLP OP_RETURN — GENESIS/MINT/SEND/COMMIT field grammar. */
        struct slp_message msg;
        (void)slp_parse(payload, payload_len, &msg);
        break;
    }
    case 1: {
        /* ZNAM OP_RETURN — the name registry's six commands. */
        struct znam_message msg;
        (void)znam_parse(payload, payload_len, &msg);
        break;
    }
    case 2: {
        /* ZDIR OP_RETURN — the on-chain node directory record. */
        struct zdir_message msg;
        (void)zdir_parse(payload, payload_len, &msg);
        break;
    }
    case 3: {
        /* zid identity document. On a well-formed frame, push the
         * decoded body onward through the two body codecs that consume
         * it — a doc body is exactly as attacker-controlled as the frame
         * around it, and the release decoder is the one with a
         * length-prefixed string pair inside. */
        struct zid_doc doc;
        if (zid_doc_decode(&doc, payload, payload_len)) {
            struct zid_release rel;
            (void)zid_release_decode_body(&rel, doc.body, doc.body_len);

            uint64_t index = 0, num_leaves = 0;
            uint32_t proof_len = 0;
            uint8_t siblings[ZID_TREE_MAX_PEAKS][32];
            (void)zid_proof_decode(&index, &num_leaves, siblings, &proof_len,
                                   doc.body, doc.body_len);
        }
        break;
    }
    case 4: {
        /* zid signed endpoint body ("ZIDE"): flags-driven variable
         * layout, exact-length rule, then the full shape re-check. */
        struct zendp ep;
        (void)zendp_decode_body(&ep, payload, body_len);
        break;
    }
    case 5: {
        /* zid service descriptor body ("ZIDD"): intro_count-driven
         * variable layout plus per-hostname v3 onion re-validation. */
        struct zdesc desc;
        (void)zdesc_decode_body(&desc, payload, body_len);
        break;
    }
    case 6: {
        /* zid MMR inclusion proof wire. Reachable through arm 3 only
         * behind a valid doc frame, which costs libFuzzer a 51-byte
         * prefix and an exact length match; give the proof_len-driven
         * sibling read its own direct arm as well. */
        uint64_t index = 0, num_leaves = 0;
        uint32_t proof_len = 0;
        uint8_t siblings[ZID_TREE_MAX_PEAKS][32];
        (void)zid_proof_decode(&index, &num_leaves, siblings, &proof_len,
                               payload, payload_len);
        break;
    }
    default:
        break;
    }

    return 0;
}

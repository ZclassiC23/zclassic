/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * fuzz_zcode_commons — libFuzzer harness for every canonical parser added by
 * the ZC23 Living Commons foundation. These bytes may arrive from the public
 * ZCODE CAS before their signatures, roots, or cross-object authorities are
 * trusted, so parsing must be total and bounded for every possible input.
 *
 * Byte 0 selects one of six parsers. The remainder is passed at its exact
 * length. The epoch parser is the only arm that can allocate; it is freed on
 * every success or failure path. ASan+UBSan are supplied by FUZZ_CFLAGS.
 */

#include "vcs/zcode_continuity_policy.h"
#include "vcs/zcode_creation_attribution.h"
#include "vcs/zcode_epoch_creation.h"
#include "vcs/zcode_patronage.h"
#include "vcs/zcode_patronage_funding.h"
#include "vcs/zcode_patronage_settlement.h"

#include <signal.h>
#include <stddef.h>
#include <stdint.h>

volatile sig_atomic_t g_shutdown_requested = 0;

#define FUZZ_ZCODE_COMMONS_ARMS 6u
#define FUZZ_ZCODE_COMMONS_MAX_INPUT \
    (VCS_ZCODE_EPOCH_CREATION_MAX_WIRE_BYTES + 1u)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0 || size > FUZZ_ZCODE_COMMONS_MAX_INPUT)
        return 0;

    const uint8_t arm = (uint8_t)(data[0] % FUZZ_ZCODE_COMMONS_ARMS);
    const uint8_t *wire = data + 1;
    const size_t wire_len = size - 1;

    switch (arm) {
    case 0: {
        struct vcs_zcode_creation_attribution_v1 out;
        (void)vcs_zcode_creation_attribution_parse(wire, wire_len, &out);
        break;
    }
    case 1: {
        struct vcs_zcode_epoch_creation_set_v1 out;
        vcs_zcode_epoch_creation_init(&out);
        (void)vcs_zcode_epoch_creation_parse(wire, wire_len, &out);
        vcs_zcode_epoch_creation_free(&out);
        break;
    }
    case 2: {
        struct vcs_zcode_patronage_intent_v1 out;
        (void)vcs_zcode_patronage_intent_parse(wire, wire_len, &out);
        break;
    }
    case 3: {
        struct vcs_zcode_patronage_funding_v1 out;
        (void)vcs_zcode_patronage_funding_parse(wire, wire_len, &out);
        break;
    }
    case 4: {
        struct vcs_zcode_patronage_settlement_v1 out;
        (void)vcs_zcode_patronage_settlement_parse(wire, wire_len, &out);
        break;
    }
    case 5: {
        struct vcs_zcode_continuity_policy_v1 out;
        (void)vcs_zcode_continuity_policy_parse(wire, wire_len, &out);
        break;
    }
    }
    return 0;
}

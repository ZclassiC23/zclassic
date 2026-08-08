/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * fuzz_zcode_dht — libFuzzer harness for the ZCODE DHT wire parsers. DHT
 * frames and records arrive from arbitrary peers before their Noise binding,
 * delegation chain, or signatures are trusted, so parsing must be total and
 * bounded for every possible input.
 *
 * Byte 0 selects one of five parsers. The remainder is passed at its exact
 * length. No arm allocates; the verify contexts are fixed (established Noise
 * session, no chain callback) so a fuzzed input can drive each parser as far
 * as its cryptographic checks allow. ASan+UBSan are supplied by FUZZ_CFLAGS.
 */

#include "vcs/zcode_dht.h"
#include "vcs/zcode_dht_delegation.h"
#include "vcs/zcode_dht_msgs.h"
#include "vcs/zcode_dht_record.h"

#include <signal.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

volatile sig_atomic_t g_shutdown_requested = 0;

#define FUZZ_ZCODE_DHT_ARMS 5u
#define FUZZ_ZCODE_DHT_MAX_INPUT                                     \
    (VCS_ZCODE_DHT_CONTACTS_HEADER_BYTES +                           \
     VCS_ZCODE_DHT_K * VCS_ZCODE_DHT_CONTACT_ENTRY_WIRE_BYTES + 1u)

/* Fixed unix time inside any reasonable record/delegation window. */
#define FUZZ_ZCODE_DHT_NOW_UNIX UINT64_C(1800000000)

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    if (size == 0 || size > FUZZ_ZCODE_DHT_MAX_INPUT)
        return 0;

    const uint8_t arm = (uint8_t)(data[0] % FUZZ_ZCODE_DHT_ARMS);
    const uint8_t *wire = data + 1;
    const size_t wire_len = size - 1;

    switch (arm) {
    case 0: {
        struct vcs_zcode_dht_msg_verify_context verify;
        memset(&verify, 0, sizeof(verify));
        verify.noise_established = true;
        memset(verify.noise_transcript_hash, 0x01,
               sizeof(verify.noise_transcript_hash));
        verify.now_unix = FUZZ_ZCODE_DHT_NOW_UNIX;
        struct vcs_zcode_dht_msg out;
        (void)vcs_zcode_dht_msg_parse(wire, wire_len, &verify, &out);
        break;
    }
    case 1: {
        struct vcs_zcode_dht_record_verify_context verify;
        memset(&verify, 0, sizeof(verify));
        verify.now_unix = FUZZ_ZCODE_DHT_NOW_UNIX;
        struct vcs_zcode_dht_record out;
        (void)vcs_zcode_dht_record_parse(wire, wire_len, &verify, &out);
        break;
    }
    case 2: {
        struct vcs_zcode_dht_record_verify_context verify;
        memset(&verify, 0, sizeof(verify));
        verify.now_unix = FUZZ_ZCODE_DHT_NOW_UNIX;
        bool expired = false;
        struct vcs_zcode_dht_record out;
        (void)vcs_zcode_dht_record_parse_persisted(wire, wire_len, &verify,
                                                   &expired, &out);
        break;
    }
    case 3: {
        struct vcs_zcode_dht_delegation out;
        (void)vcs_zcode_dht_delegation_decode(&out, wire, wire_len);
        break;
    }
    case 4: {
        static const uint8_t genesis[32] = {0};
        static const uint8_t self[32] = {0};
        struct vcs_zcode_dht_contact contacts[VCS_ZCODE_DHT_K];
        uint32_t count = 0;
        (void)vcs_zcode_dht_contacts_parse(wire, wire_len, genesis, self,
                                           FUZZ_ZCODE_DHT_NOW_UNIX, NULL, NULL,
                                           contacts, VCS_ZCODE_DHT_K, &count);
        break;
    }
    }
    return 0;
}

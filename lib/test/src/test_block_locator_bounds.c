/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * block_locator_deserialize bounds test: a malicious peer can supply an
 * oversized getheaders/getblocks locator. block_locator_deserialize
 * (lib/primitives/src/block.c) must reject any count > MAX_LOCATOR_HASHES
 * (64) while still parsing a well-formed locator. Only the 3-hash happy
 * path is covered elsewhere (test_net.c); this asserts the upper-bound
 * rejection branch directly. */

#include "test/test_helpers.h"

#include "core/serialize.h"
#include "primitives/block.h"
#include "util/safe_alloc.h"

#include <stdint.h>
#include <stdio.h>
#include <string.h>

int test_block_locator_bounds(void)
{
    int failures = 0;

    /* REJECTION: a hand-built stream claiming 65 hashes (one over the
     * MAX_LOCATOR_HASHES=64 bound) must be refused. The 65 * 32 dummy
     * payload bytes are present so the only reason to reject is the
     * count check at block.c:115, not a short read. */
    printf("block_locator_deserialize rejects oversized count (65)... ");
    {
        struct byte_stream s;
        stream_init(&s, 4 + 9 + (size_t)(MAX_LOCATOR_HASHES + 1) * 32);
        bool built = stream_write_i32_le(&s, 170011) &&
                     stream_write_compact_size(&s,
                         (uint64_t)(MAX_LOCATOR_HASHES + 1));
        unsigned char dummy[32];
        memset(dummy, 0x5A, sizeof(dummy));
        for (int i = 0; built && i <= MAX_LOCATOR_HASHES; i++)
            built = stream_write_bytes(&s, dummy, 32);

        struct byte_stream r;
        stream_init_from_data(&r, s.data, s.size);
        struct block_locator loc;
        block_locator_init(&loc);
        bool rc = block_locator_deserialize(&loc, &r);
        block_locator_free(&loc);
        stream_free(&s);

        if (built && rc == false)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    /* HAPPY PATH: a valid 3-hash locator still serializes and parses,
     * recovering num_hashes and the hash bytes intact. */
    printf("block_locator_deserialize accepts valid 3-hash locator... ");
    {
        struct block_locator loc;
        block_locator_init(&loc);
        loc.num_hashes = 3;
        loc.vhave = zcl_calloc(3, sizeof(struct uint256), "test_locator_bounds");
        bool ok = loc.vhave != NULL;
        if (ok) {
            memset(loc.vhave[0].data, 0x11, 32);
            memset(loc.vhave[1].data, 0x22, 32);
            memset(loc.vhave[2].data, 0x33, 32);
        }

        struct byte_stream s;
        stream_init(&s, 128);
        ok = ok && block_locator_serialize(&loc, &s);

        struct byte_stream r;
        stream_init_from_data(&r, s.data, s.size);
        struct block_locator loc2;
        block_locator_init(&loc2);
        ok = ok && block_locator_deserialize(&loc2, &r);
        ok = ok &&
             loc2.num_hashes == 3 &&
             loc2.vhave[0].data[0] == 0x11 &&
             loc2.vhave[1].data[0] == 0x22 &&
             loc2.vhave[2].data[0] == 0x33;

        block_locator_free(&loc);
        block_locator_free(&loc2);
        stream_free(&s);

        if (ok)
            printf("OK\n");
        else { printf("FAIL\n"); failures++; }
    }

    return failures;
}

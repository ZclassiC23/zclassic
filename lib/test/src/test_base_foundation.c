/* Copyright 2026 Rhett Creighton - Apache License 2.0 */
#include "test/test_core.h"

#include "base/checked.h"
#include "base/cleanse.h"

#include <stdint.h>
#include <string.h>

extern void base_foundation_cleanse_probe(uint8_t *buf, size_t len);

static int test_checked_arithmetic(void)
{
    int failures = 0;
    TEST("base checked arithmetic: exact boundaries and cleared failures") {
        size_t sz = 99;
        uint64_t u = 99;
        ASSERT(zcl_size_add(SIZE_MAX - 1, 1, &sz) && sz == SIZE_MAX);
        ASSERT(!zcl_size_add(SIZE_MAX, 1, &sz) && sz == 0);
        ASSERT(zcl_size_mul(SIZE_MAX, 1, &sz) && sz == SIZE_MAX);
        ASSERT(!zcl_size_mul(SIZE_MAX, 2, &sz) && sz == 0);
        ASSERT(zcl_size_mul(0, SIZE_MAX, &sz) && sz == 0);
        ASSERT(zcl_u64_add(UINT64_MAX - 1, 1, &u) && u == UINT64_MAX);
        ASSERT(!zcl_u64_add(UINT64_MAX, 1, &u) && u == 0);
        ASSERT(zcl_u64_mul(UINT64_MAX, 1, &u) && u == UINT64_MAX);
        ASSERT(!zcl_u64_mul(UINT64_MAX, 2, &u) && u == 0);
        ASSERT(zcl_u64_add(1, 2, NULL));
        ASSERT(!zcl_u64_mul(UINT64_MAX, 2, NULL));
        PASS();
    } _test_next:;
    return failures;
}

static int test_cleanse_cross_tu(void)
{
    int failures = 0;
    TEST("base cleanse: optimized cross-translation-unit store survives") {
        uint8_t secret[97];
        memset(secret, 0xa5, sizeof(secret));
        base_foundation_cleanse_probe(secret, sizeof(secret));
        for (size_t i = 0; i < sizeof(secret); i++)
            ASSERT(secret[i] == 0);
        PASS();
    } _test_next:;
    return failures;
}

int test_base_foundation(void)
{
    return test_checked_arithmetic() + test_cleanse_cross_tu();
}

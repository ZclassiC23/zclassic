/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Truth table for rescanwitnesses' consensus guard. A rebuilt Sapling tree is
 * persistable only when it matches a non-zero header root and every witness root
 * agrees with that tree.
 */

#include "test/test_helpers.h"
#include "controllers/wallet_rescan_controller_internal.h"

static struct uint256 test_root(uint8_t seed)
{
    struct uint256 r;
    for (size_t i = 0; i < sizeof(r.data); i++)
        r.data[i] = (uint8_t)(seed + i);
    return r;
}

static void check_case(const char *name, bool got, bool want, int *failures)
{
    printf("%s... ", name);
    if (got == want) {
        printf("OK\n");
    } else {
        printf("FAIL\n");
        (*failures)++;
    }
}

int test_rescanwitnesses_diverge_guard(void)
{
    printf("\n=== rescanwitnesses divergence guard ===\n");
    int failures = 0;

    struct uint256 root = test_root(0x11);
    struct uint256 same = root;
    struct uint256 other = test_root(0x22);
    struct uint256 zero = {{0}};

    check_case("matching non-zero root with zero mismatches is valid",
               rescan_result_consensus_valid(&root, &same, 0), true,
               &failures);
    check_case("all-zero header root is invalid",
               rescan_result_consensus_valid(&root, &zero, 0), false,
               &failures);
    check_case("different header root is invalid",
               rescan_result_consensus_valid(&root, &other, 0), false,
               &failures);
    check_case("positive witness mismatch count is invalid",
               rescan_result_consensus_valid(&root, &same, 1), false,
               &failures);
    check_case("negative witness mismatch count is invalid",
               rescan_result_consensus_valid(&root, &same, -1), false,
               &failures);
    check_case("NULL rebuilt root is invalid",
               rescan_result_consensus_valid(NULL, &same, 0), false,
               &failures);
    check_case("NULL header root is invalid",
               rescan_result_consensus_valid(&root, NULL, 0), false,
               &failures);

    printf("rescanwitnesses divergence guard: %s (%d failures)\n",
           failures == 0 ? "OK" : "FAIL", failures);
    return failures;
}

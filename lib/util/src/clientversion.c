/* Copyright (c) 2012-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "util/clientversion.h"
#include <stdio.h>

const char CLIENT_NAME[] = "ZClassic23";

#ifndef ZCL_BUILD_COMMIT
#define ZCL_BUILD_COMMIT "unknown"
#endif

/* Exact 40-hex commit + clean flag, baked as -D macros by the Makefile and
 * refreshed here through the same commit stamp that keeps ZCL_BUILD_COMMIT
 * current. See the header for why this getter is not a static inline. */
#ifndef ZCL_BUILD_COMMIT_FULL
#define ZCL_BUILD_COMMIT_FULL "unknown"
#endif
#ifndef ZCL_BUILD_CLEAN
#define ZCL_BUILD_CLEAN 0
#endif

const char *zcl_build_commit(void)
{
    return ZCL_BUILD_COMMIT;
}

const char *zcl_build_commit_full(void)
{
    return ZCL_BUILD_COMMIT_FULL;
}

bool zcl_build_source_clean(void)
{
    return ZCL_BUILD_CLEAN != 0;
}

void FormatVersion(int nVersion, char *out, size_t out_size)
{
    int major = nVersion / 1000000;
    int minor = (nVersion / 10000) % 100;
    int rev = (nVersion / 100) % 100;
    int build = nVersion % 100;

    if (build < 25)
        snprintf(out, out_size, "%d.%d.%d-beta%d", major, minor, rev, build + 1);
    else if (build < 50)
        snprintf(out, out_size, "%d.%d.%d-rc%d", major, minor, rev, build - 24);
    else if (build == 50)
        snprintf(out, out_size, "%d.%d.%d", major, minor, rev);
    else
        snprintf(out, out_size, "%d.%d.%d-%d", major, minor, rev, build - 50);
}

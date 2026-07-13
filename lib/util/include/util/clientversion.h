/* Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright (c) 2016-2019 The Zcash developers
 * Copyright (C) 2022-2026 zclassic Community
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#ifndef BITCOIN_CLIENTVERSION_H
#define BITCOIN_CLIENTVERSION_H

#define CLIENT_VERSION_MAJOR 0
#define CLIENT_VERSION_MINOR 1
#define CLIENT_VERSION_REVISION 0
#define CLIENT_VERSION_BUILD 50

#if !defined(WINDRES_PREPROC)

#include <stdbool.h>
#include <stddef.h>

#define CLIENT_VERSION \
    (1000000 * CLIENT_VERSION_MAJOR + 10000 * CLIENT_VERSION_MINOR + \
     100 * CLIENT_VERSION_REVISION + CLIENT_VERSION_BUILD)

extern const char CLIENT_NAME[];

/* Defined ONLY in clientversion.c. Must not be a static inline: each TU
 * would freeze the ZCL_BUILD_COMMIT macro at its own last recompile, and
 * version reporters inside one binary then disagree about which commit is
 * running. The Makefile keeps clientversion.o fresh via a commit stamp. */
const char *zcl_build_commit(void);

/* Exact 40-hex lowercase Git commit of the build tree, or "unknown" when the
 * build was not stamped (e.g. no git). Distinct from zcl_build_commit() (the
 * short, possibly `-dirty` display form). Baked into clientversion.o only —
 * same freshness stamp as zcl_build_commit(). The producer source receipt
 * (config/consensus_state_producer_receipt.c) uses this as producer_commit. */
const char *zcl_build_commit_full(void);

/* True iff the build tree had no uncommitted modifications at compile time. */
bool zcl_build_source_clean(void);

void FormatVersion(int nVersion, char *out, size_t out_size);

#endif /* WINDRES_PREPROC */
#endif

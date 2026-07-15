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

/* Defined ONLY in clientversion.c. Must not be a static inline: each TU would
 * freeze the baked metadata at its own last recompile, and version reporters
 * inside one binary could then disagree. The Makefile keeps clientversion.o
 * fresh through the source/build identity stamp. */
const char *zcl_build_commit(void);

/* Compatibility getters for display fields. They return "external": Git
 * commit ids are deliberately not baked into the sovereign executable because
 * its exact bytes are receipt authority. GitHub publication may carry commit
 * trace metadata in an external sidecar. */
const char *zcl_build_commit_full(void);

/* Exact 64-hex lowercase SHA-256 emitted by
 * tools/dev/source-identity.sh capture (zcl.dev_source_identity.v2), or
 * "unknown" when the build was not source-stamped. This is the authoritative
 * source-tree input for producer receipt v2; Git/GitHub commit metadata is not
 * part of that receipt's digests. */
const char *zcl_build_source_id_sha256(void);

/* Legacy-named v2 capture-completeness bit. True after an exact successful
 * source inventory capture; it is not Git HEAD cleanliness. Dirty worktree
 * bytes are bound directly by zcl_build_source_id_sha256(). */
bool zcl_build_source_clean(void);

void FormatVersion(int nVersion, char *out, size_t out_size);

#endif /* WINDRES_PREPROC */
#endif

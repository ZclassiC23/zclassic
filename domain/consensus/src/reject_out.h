/* SPDX-License-Identifier: Apache-2.0
 * Copyright (c) 2009-2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Internal helpers shared by the domain/consensus structural-check
 * modules (check_block.c, header_accept.c) for writing the canonical
 * legacy reject_reason token and DoS score to the caller's out params.
 *
 * Not a public header: lives under src/, not include/, not installed.
 * Quote-include resolves it relative to the including source file. */

#ifndef DOMAIN_CONSENSUS_REJECT_OUT_H
#define DOMAIN_CONSENSUS_REJECT_OUT_H

#include <stddef.h>
#include <string.h>

/* Write the canonical legacy reject_reason token to the caller's buffer
 * (if any). NUL-terminated truncation is safe because all known reasons
 * are far shorter than the 256-byte reason caps used by callers. */
static inline void set_reject(char *buf, size_t cap, const char *reason)
{
    if (!buf || cap == 0) return;
    size_t n = strlen(reason);
    if (n >= cap) n = cap - 1;
    memcpy(buf, reason, n);
    buf[n] = '\0';
}

static inline void set_dos(int *out_dos, int dos)
{
    if (out_dos) *out_dos = dos;
}

#endif /* DOMAIN_CONSENSUS_REJECT_OUT_H */

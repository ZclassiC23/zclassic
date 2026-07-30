/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * evidence_ledger_row — see services/evidence_ledger_row.h for the contract
 * and for why the bounded tail read lives in exactly one place. Pure reader:
 * no allocation, no clock, no globals, no threads. */

// one-result-type-ok:evidence-ledger-row-pure-reader
//
// A pure parsing/IO-read helper, not a fallible service executor. Every
// "false" returned here means the CALLER passed bad arguments or the field is
// absent; a missing or unreadable ledger is data (no rows scanned), never an
// error a caller branches on.

#include "services/evidence_ledger_row.h"

#include "util/log_macros.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void evidence_copy_bounded(char *dst, size_t cap, const char *src, size_t len)
{
    if (!dst || cap == 0)
        return;
    if (!src)
        len = 0;
    if (len >= cap)
        len = cap - 1;
    if (len)
        memcpy(dst, src, len);
    dst[len] = '\0';
}

const char *evidence_find_sub(const char *hay, size_t len, const char *needle)
{
    if (!hay || !needle)
        return NULL;
    size_t nlen = strlen(needle);
    if (nlen == 0 || nlen > len)
        return NULL;
    for (size_t i = 0; i + nlen <= len; i++) {
        if (memcmp(hay + i, needle, nlen) == 0)
            return hay + i;
    }
    return NULL;
}

/* Locate `"key":` in one row and return the first byte of its value, or NULL.
 * Deliberately simple: rows are flat, single-line, unnested JSON with no
 * duplicate keys (see the header). */
static const char *row_value(const char *row, size_t len, const char *key)
{
    if (!row || !key)
        return NULL;
    char needle[64];
    if (snprintf(needle, sizeof(needle), "\"%s\":", key) >= (int)sizeof(needle))
        return NULL;
    const char *at = evidence_find_sub(row, len, needle);
    if (!at)
        return NULL;
    at += strlen(needle);
    const char *end = row + len;
    while (at < end && (*at == ' ' || *at == '\t'))
        at++;
    return at < end ? at : NULL;
}

bool evidence_row_str(const char *row, size_t len, const char *key,
                      char *dst, size_t cap)
{
    if (dst && cap)
        dst[0] = '\0';
    const char *at = row_value(row, len, key);
    if (!at || *at != '"')
        return false;
    at++;
    const char *end = row + len;
    size_t n = 0;
    while (at < end && *at != '"') {
        char c = *at;
        if (c == '\\' && at + 1 < end) {
            at++;
            c = *at;
            if (c == 'n' || c == 't' || c == 'r')
                c = ' ';
        }
        if (dst && cap && n + 1 < cap)
            dst[n++] = c;
        at++;
    }
    if (dst && cap)
        dst[n < cap ? n : cap - 1] = '\0';
    return at < end;
}

bool evidence_row_int(const char *row, size_t len, const char *key,
                      int64_t *out)
{
    const char *at = row_value(row, len, key);
    if (!at)
        return false;
    const char *end = row + len;
    bool neg = false;
    if (*at == '-') {
        neg = true;
        at++;
    }
    if (at >= end || *at < '0' || *at > '9')
        return false;
    int64_t v = 0;
    while (at < end && *at >= '0' && *at <= '9') {
        if (v > (INT64_MAX - (*at - '0')) / 10)
            return false;
        v = v * 10 + (*at - '0');
        at++;
    }
    if (out)
        *out = neg ? -v : v;
    return true;
}

bool evidence_ledger_scan_text(const char *text, size_t len,
                              evidence_row_fn fn, void *ctx)
{
    if (!fn)
        LOG_FAIL("evidence_ledger", "row callback is NULL");
    if (!text && len)
        LOG_FAIL("evidence_ledger", "ledger text is NULL with len=%zu", len);

    size_t i = 0;
    while (i < len) {
        size_t j = i;
        while (j < len && text[j] != '\n')
            j++;
        if (j > i)
            fn(text + i, j - i, ctx);
        i = (j < len) ? j + 1 : len;
    }
    return true;
}

bool evidence_ledger_scan_tail(const char *path, size_t tail_bytes,
                              evidence_row_fn fn, void *ctx,
                              unsigned *out_overlong)
{
    if (!fn)
        LOG_FAIL("evidence_ledger", "row callback is NULL");
    if (!path || !path[0])
        LOG_FAIL("evidence_ledger", "ledger path is NULL/empty");
    if (tail_bytes == 0)
        LOG_FAIL("evidence_ledger", "tail_bytes is 0 for '%s'", path);

    FILE *f = fopen(path, "rb");
    if (!f)
        return true;            /* absent ledger: no rows, not an error */

    /* Seek to the last tail_bytes so a rotated or very long ledger stays a
     * cheap read, then stream a row at a time. */
    bool partial_head = false;
    if (fseek(f, 0, SEEK_END) == 0) {
        long size = ftell(f);
        if (size > (long)tail_bytes) {
            if (fseek(f, size - (long)tail_bytes, SEEK_SET) == 0)
                partial_head = true;
            else
                rewind(f);
        } else {
            rewind(f);
        }
    }

    char row[EVIDENCE_ROW_MAX];
    bool first = true;
    bool overlong = false;
    while (fgets(row, sizeof(row), f)) {
        size_t rlen = strlen(row);
        bool complete = rlen > 0 && row[rlen - 1] == '\n';
        if (complete)
            rlen--;
        if (overlong) {
            /* Continuation of a row that did not fit — consumed, never
             * folded as a second row. */
            overlong = !complete;
            continue;
        }
        if (!complete && rlen == sizeof(row) - 1) {
            overlong = true;
            if (out_overlong)
                (*out_overlong)++;
            continue;
        }
        if (first) {
            first = false;
            /* The first line after a mid-file seek is a fragment; dropping it
             * is the difference between describing evidence and inventing a
             * sample. */
            if (partial_head)
                continue;
        }
        if (rlen > 0)
            fn(row, rlen, ctx);
    }
    fclose(f);
    return true;
}

bool evidence_ledger_resolve_path(const char *dir_env, const char *home_rel_dir,
                                 const char *file, char *out, size_t cap)
{
    if (!out || cap == 0)
        LOG_FAIL("evidence_ledger", "path output buffer is NULL/empty");
    out[0] = '\0';
    if (!dir_env || !dir_env[0])
        LOG_FAIL("evidence_ledger", "dir env var name is NULL/empty");
    if (!home_rel_dir || !home_rel_dir[0])
        LOG_FAIL("evidence_ledger", "home-relative dir is NULL/empty");
    if (!file || !file[0])
        LOG_FAIL("evidence_ledger", "ledger file name is NULL/empty");

    const char *dir = getenv(dir_env);
    if (dir && dir[0]) {
        if (snprintf(out, cap, "%s/%s", dir, file) >= (int)cap) {
            out[0] = '\0';
            LOG_FAIL("evidence_ledger", "%s path too long for buffer",
                     dir_env);
        }
        return true;
    }

    const char *home = getenv("HOME");
    if (!home || !home[0])
        LOG_FAIL("evidence_ledger",
                 "no %s and no HOME to resolve the %s ledger path from",
                 dir_env, file);
    if (snprintf(out, cap, "%s/%s/%s", home, home_rel_dir, file) >= (int)cap) {
        out[0] = '\0';
        LOG_FAIL("evidence_ledger", "default %s path too long for buffer",
                 file);
    }
    return true;
}

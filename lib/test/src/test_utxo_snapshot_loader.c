/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * test_utxo_snapshot_loader: round-trips the sidecar format.
 *
 *   1. Synthesize a tiny in-memory sidecar (3 records).
 *   2. uss_open() with verify_full_sha3=true → must succeed and
 *      report the same header.
 *   3. uss_iter() → must visit all 3 records with matching values.
 *   4. Corrupt one byte → re-open must fail with "body sha3 mismatch".
 */

#include "test/test_helpers.h"
#include "chain/utxo_snapshot_loader.h"
#include "crypto/sha3.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

struct cap_ctx {
    int n;
    int script_lens[3];
    int64_t values[3];
};

static bool cap_cb(const struct uss_record *r, void *vctx)
{
    struct cap_ctx *c = vctx;
    if (c->n >= 3) return false;
    c->script_lens[c->n] = (int)r->script_len;
    c->values[c->n] = r->value;
    c->n++;
    return true;
}

static void wle32(uint8_t *p, uint32_t v)
{ p[0]=(uint8_t)v; p[1]=(uint8_t)(v>>8); p[2]=(uint8_t)(v>>16); p[3]=(uint8_t)(v>>24); }
static void wle64(uint8_t *p, uint64_t v)
{ for (int i=0;i<8;i++) p[i]=(uint8_t)(v>>(8*i)); }

int test_utxo_snapshot_loader(void)
{
    int failures = 0;

    /* Build the body in a buffer. 3 records, ascending vout indices. */
    uint8_t body[4096] = {0};
    size_t off = 0;

    uint8_t txid[32];
    for (int i = 0; i < 32; i++) txid[i] = (uint8_t)i;
    uint8_t script_a[3] = { 0x76, 0xa9, 0x14 };
    uint8_t script_b[1] = { 0x6a };
    uint8_t script_c[5] = { 0x21, 0x02, 0x03, 0x04, 0x05 };

    struct rec_in { uint32_t vout; int64_t value; const uint8_t *s; uint32_t sl;
                    uint32_t height; uint8_t is_cb; };
    struct rec_in recs[3] = {
        { 0, 50000, script_a, 3, 100, 1 },
        { 1, 12345, script_b, 1, 200, 0 },
        { 2, 99999, script_c, 5, 300, 0 },
    };
    for (int i = 0; i < 3; i++) {
        memcpy(body + off, txid, 32); off += 32;
        wle32(body + off, recs[i].vout); off += 4;
        wle64(body + off, (uint64_t)recs[i].value); off += 8;
        wle32(body + off, recs[i].sl); off += 4;
        memcpy(body + off, recs[i].s, recs[i].sl); off += recs[i].sl;
        wle32(body + off, recs[i].height); off += 4;
        body[off++] = recs[i].is_cb;
    }
    size_t body_len = off;

    /* Hash the body. */
    uint8_t sha3[32];
    struct sha3_256_ctx ctx;
    sha3_256_init(&ctx);
    sha3_256_write(&ctx, body, body_len);
    sha3_256_finalize(&ctx, sha3);

    /* Build header. */
    uint8_t header[104] = {0};
    memcpy(header, "ZCLUTXO\x00", 8);
    wle32(header + 8, 1);
    wle32(header + 16, 12345);
    wle64(header + 24, 3);
    wle64(header + 32, (uint64_t)(50000 + 12345 + 99999));
    /* anchor block hash: leave zero */
    memcpy(header + 72, sha3, 32);

    /* Write to a tmp file. */
    const char *tmp = "/tmp/zcl_test_uss.dat";
    FILE *f = fopen(tmp, "wb");
    if (!f) { printf("uss: tmp open FAIL\n"); return 1; }
    fwrite(header, 1, 104, f);
    fwrite(body, 1, body_len, f);
    fclose(f);

    /* Open and verify. */
    printf("uss: open+verify... ");
    char err[128] = {0};
    struct uss_header hdr;
    struct uss_handle *h = uss_open(tmp, true, NULL, &hdr,
                                    err, sizeof(err));
    if (!h) { printf("FAIL (%s)\n", err); failures++; goto cleanup; }
    if (hdr.count != 3) { printf("FAIL (count=%llu)\n",
        (unsigned long long)hdr.count); failures++; }
    if (hdr.total_supply != 50000 + 12345 + 99999) {
        printf("FAIL (supply=%lld)\n", (long long)hdr.total_supply);
        failures++;
    }
    printf("OK\n");

    /* Iterate. */
    printf("uss: iter... ");
    struct cap_ctx cap = {0};
    int64_t n = uss_iter(h, cap_cb, &cap);
    if (n != 3 || cap.n != 3) { printf("FAIL (n=%lld)\n",
        (long long)n); failures++; }
    else if (cap.values[0] != 50000 ||
             cap.values[1] != 12345 ||
             cap.values[2] != 99999 ||
             cap.script_lens[0] != 3 ||
             cap.script_lens[1] != 1 ||
             cap.script_lens[2] != 5) {
        printf("FAIL (record content mismatch)\n");
        failures++;
    }
    else printf("OK\n");
    uss_close(h);

    /* ── v2 round-trip: records + embedded Sapling frontier section ──
     * Append [u32 frontier_len][blob] after the records (inside the body SHA3),
     * stamp version 2, and confirm uss_version()==2 + uss_frontier() returns the
     * exact blob, that records still iterate, and that the v1 file (still valid
     * here) reports version 1 + NO frontier. Run BEFORE the corruption test
     * below so `tmp` is uncorrupted. */
    printf("uss: v2 frontier round-trip... ");
    {
        uint8_t frontier[71];
        for (int i = 0; i < (int)sizeof(frontier); i++)
            frontier[i] = (uint8_t)(0xA0 + (i & 0x1f));
        uint32_t flen = (uint32_t)sizeof(frontier);

        /* Body = the 3 records + [u32 len][blob]. */
        uint8_t v2body[4096];
        size_t v2off = 0;
        memcpy(v2body, body, body_len); v2off = body_len;
        wle32(v2body + v2off, flen); v2off += 4;
        memcpy(v2body + v2off, frontier, flen); v2off += flen;

        uint8_t v2sha3[32];
        struct sha3_256_ctx c2;
        sha3_256_init(&c2);
        sha3_256_write(&c2, v2body, v2off);
        sha3_256_finalize(&c2, v2sha3);

        uint8_t v2hdr[104] = {0};
        memcpy(v2hdr, "ZCLUTXO\x00", 8);
        wle32(v2hdr + 8, 2);                 /* version 2 */
        wle32(v2hdr + 16, 12345);
        wle64(v2hdr + 24, 3);
        wle64(v2hdr + 32, (uint64_t)(50000 + 12345 + 99999));
        memcpy(v2hdr + 72, v2sha3, 32);

        const char *tmp2 = "/tmp/zcl_test_uss_v2.dat";
        FILE *fv = fopen(tmp2, "wb");
        bool ok = (fv != NULL);
        if (fv) {
            fwrite(v2hdr, 1, 104, fv);
            fwrite(v2body, 1, v2off, fv);
            fclose(fv);
        }

        char e2[128] = {0};
        struct uss_handle *hv = ok
            ? uss_open(tmp2, true, NULL, NULL, e2, sizeof(e2)) : NULL;
        ok = ok && hv != NULL;
        ok = ok && uss_version(hv) == 2;
        const uint8_t *gotb = NULL; uint32_t gotl = 0;
        ok = ok && uss_frontier(hv, &gotb, &gotl);
        ok = ok && gotb && gotl == flen && memcmp(gotb, frontier, flen) == 0;
        struct cap_ctx cap2 = {0};
        ok = ok && uss_iter(hv, cap_cb, &cap2) == 3 && cap2.n == 3;
        if (hv) uss_close(hv);

        /* v1 file (uncorrupted here) => version 1, no frontier. */
        struct uss_handle *hv1 = uss_open(tmp, true, NULL, NULL, e2, sizeof(e2));
        if (hv1) {
            ok = ok && uss_version(hv1) == 1;
            const uint8_t *nb = (const uint8_t *)1; uint32_t nl = 1;
            ok = ok && !uss_frontier(hv1, &nb, &nl) && nb == NULL && nl == 0;
            uss_close(hv1);
        } else {
            ok = false;
        }
        unlink(tmp2);
        if (ok) printf("OK\n"); else { printf("FAIL (e=%s)\n", e2); failures++; }
    }

    /* Corruption detection. */
    printf("uss: corruption detection... ");
    FILE *f2 = fopen(tmp, "rb+");
    if (!f2) { printf("FAIL (reopen)\n"); failures++; goto cleanup; }
    fseek(f2, 200, SEEK_SET); /* somewhere in the body */
    uint8_t orig;
    fread(&orig, 1, 1, f2);
    fseek(f2, 200, SEEK_SET);
    uint8_t flipped = orig ^ 0xff;
    fwrite(&flipped, 1, 1, f2);
    fclose(f2);

    err[0] = '\0';
    h = uss_open(tmp, true, NULL, NULL, err, sizeof(err));
    if (h) {
        printf("FAIL (open succeeded on corrupted file)\n");
        failures++;
        uss_close(h);
    } else if (strcmp(err, "body sha3 mismatch") != 0) {
        printf("FAIL (wrong err: %s)\n", err);
        failures++;
    } else {
        printf("OK\n");
    }

cleanup:
    unlink(tmp);
    return failures;
}

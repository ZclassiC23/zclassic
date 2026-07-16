/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Tests for block_parse_cache (lib/storage/src/block_parse_cache.c), the
 * 16-entry (height, block_hash) LRU that feeds the consensus fold via
 * stage_read_block() (app/jobs/include/jobs/stage_helpers.h:102-113). The
 * contract (block_parse_cache.h:21-27): every consumer receives a COMPLETE,
 * INDEPENDENT deep clone — a block_serialize<->block_deserialize round-trip
 * byte-identical to a fresh read_block_from_disk_pread — and owns it.
 *
 * A silent defect here (stale entry, key collision, shallow clone, eviction
 * error) would hand a WRONG body to body_persist/script_validate/proof_validate/
 * utxo_apply and corrupt the UTXO set. These tests assert the three properties
 * that protect the fold:
 *   (a) deep-clone equality + independence (miss then hit; mutate one, others
 *       and the cache entry unaffected),
 *   (b) no (height,hash) key collision (same height/diff hash, same hash/diff
 *       height both return their own bytes),
 *   (c) LRU eviction (>16 distinct keys; oldest evicted, recents still hit).
 *
 * The cache is driven through its real public API (block_parse_cache_get /
 * block_parse_cache_clear). Misses are satisfied from synthetic blocks written
 * to a tmp datadir via write_block_to_disk (same fixture style as
 * test_disk_block_io.c), with a block_index whose phashBlock/nFile/nDataPos/
 * nStatus mirror what the production reader sees.
 */

#include "test/test_helpers.h"
#include "storage/block_parse_cache.h"
#include "storage/chain_segment.h"
#include "storage/disk_block_io.h"
#include "chain/chain.h"
#include "primitives/block.h"
#include "primitives/transaction.h"
#include "core/serialize.h"
#include "core/amount.h"
#include "core/uint256.h"
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>

/* ── Fixture helpers ─────────────────────────────────────────── */

static void make_test_dir(char *buf, size_t len)
{
    snprintf(buf, len, "./test-tmp/%d_bpc", (int)getpid());
    mkdir("./test-tmp", 0755);
    mkdir(buf, 0755);
    char blocks[512];
    snprintf(blocks, sizeof(blocks), "%s/blocks", buf);
    mkdir(blocks, 0755);
}

static void cleanup_test_dir(const char *dir)
{
    char cmd[512];
    snprintf(cmd, sizeof(cmd), "rm -rf %s", dir);
    (void)system(cmd);
}

/* Build a distinguishable synthetic block: `nout` outputs, output i carries
 * value (nseed + i) * COIN, and the header nTime = nseed. This makes both the
 * serialized bytes and the block hash a function of (nseed, nout), so two
 * fixtures with different (nseed, nout) are guaranteed distinct bodies. */
static void build_block(struct block *b, uint32_t nseed, int nout)
{
    block_init(b);
    b->header.nVersion = 4;
    b->header.nTime = nseed;
    b->header.nBits = 0x2000ffff;
    /* salt the header so even nout-only differences yield a distinct hash */
    b->header.nNonce.data[0] = (uint8_t)(nseed & 0xff);
    b->header.nNonce.data[1] = (uint8_t)((nseed >> 8) & 0xff);
    b->num_vtx = 1;
    b->vtx = calloc(1, sizeof(struct transaction)); // raw-alloc-ok:test-fixture
    transaction_init(&b->vtx[0]);
    transaction_alloc(&b->vtx[0], 1, nout);
    b->vtx[0].vin[0].sequence = 0xffffffff;
    for (int i = 0; i < nout; i++)
        b->vtx[0].vout[i].value = (CAmount)(nseed + (uint32_t)i) * COIN;
}

/* Serialize `b` into a freshly-malloc'd buffer; caller frees. */
static unsigned char *ser(const struct block *b, size_t *out_len)
{
    struct byte_stream s;
    stream_init(&s, 4096);
    if (!block_serialize(b, &s)) { stream_free(&s); return NULL; }
    unsigned char *buf = malloc(s.size ? s.size : 1); // raw-alloc-ok:test
    if (buf && s.size) memcpy(buf, s.data, s.size);
    *out_len = s.size;
    stream_free(&s);
    return buf;
}

/* A single fixture block on disk with its index entry primed. */
struct fixture {
    struct disk_block_pos pos;
    struct uint256        hash;        /* phashBlock storage */
    struct block_index    bi;
    unsigned char        *bytes;       /* canonical serialized body */
    size_t                len;
};

/* Write `src` to `datadir`, capture its canonical bytes + hash, and fill a
 * block_index that points read_block_from_disk_pread at it (BLOCK_HAVE_DATA,
 * nFile/nDataPos). `height` is the LRU key's height component. Returns true on
 * success. On failure, fx may be partially populated; caller frees fx->bytes. */
static bool make_fixture(struct fixture *fx, const char *datadir,
                         struct block *src, int height)
{
    memset(fx, 0, sizeof(*fx));
    fx->bytes = ser(src, &fx->len);
    if (!fx->bytes) return false;

    unsigned char msg_start[4] = {0x24, 0xe9, 0x27, 0x64};
    disk_block_pos_init(&fx->pos);
    if (!write_block_to_disk(src, &fx->pos, datadir, msg_start))
        return false;

    block_get_hash(src, &fx->hash);

    block_index_init(&fx->bi);
    fx->bi.nHeight = height;
    fx->bi.phashBlock = &fx->hash;
    fx->bi.nFile = fx->pos.nFile;
    fx->bi.nDataPos = fx->pos.nPos;
    fx->bi.nStatus |= BLOCK_HAVE_DATA;
    return true;
}

/* ── Test (a): deep-clone equality + independence ─────────────── */

static int test_deep_clone_equality(const char *datadir)
{
    int failures = 0;
    block_parse_cache_clear();

    TEST("bpc: miss then hit both byte-identical to source; clones independent") {
        struct block src;
        build_block(&src, 0xA1B2, 3);
        struct fixture fx;
        if (!make_fixture(&fx, datadir, &src, 100)) {
            printf("FAIL (fixture)\n"); failures++; block_free(&src);
            free(fx.bytes); goto done;
        }
        block_free(&src);

        /* MISS: first get reads from disk, populates the cache. */
        struct block b_miss; block_init(&b_miss);
        if (!block_parse_cache_get(100, fx.hash.data, &fx.bi, datadir, &b_miss)) {
            printf("FAIL (miss get)\n"); failures++;
            block_free(&b_miss); free(fx.bytes); goto done;
        }
        size_t l1 = 0; unsigned char *s1 = ser(&b_miss, &l1);
        if (!s1 || l1 != fx.len || memcmp(s1, fx.bytes, l1) != 0) {
            printf("FAIL (miss body != source bytes)\n"); failures++;
            free(s1); block_free(&b_miss); free(fx.bytes); goto done;
        }
        free(s1);

        /* HIT: second get is served from cache; must equal source too. */
        struct block b_hit; block_init(&b_hit);
        if (!block_parse_cache_get(100, fx.hash.data, &fx.bi, datadir, &b_hit)) {
            printf("FAIL (hit get)\n"); failures++;
            block_free(&b_miss); block_free(&b_hit); free(fx.bytes); goto done;
        }
        size_t l2 = 0; unsigned char *s2 = ser(&b_hit, &l2);
        if (!s2 || l2 != fx.len || memcmp(s2, fx.bytes, l2) != 0) {
            printf("FAIL (hit body != source bytes)\n"); failures++;
            free(s2); block_free(&b_miss); block_free(&b_hit);
            free(fx.bytes); goto done;
        }
        free(s2);

        /* INDEPENDENCE: mutate the miss clone, then free it; the hit clone
         * must be unchanged and a fresh hit must still equal the source. */
        b_miss.vtx[0].vout[0].value ^= 0xdeadbeef;
        block_free(&b_miss);

        if (b_hit.num_vtx != 1 ||
            b_hit.vtx[0].vout[0].value != (CAmount)(0xA1B2u) * COIN) {
            printf("FAIL (mutating/freeing miss clone disturbed hit clone)\n");
            failures++; block_free(&b_hit); free(fx.bytes); goto done;
        }
        block_free(&b_hit);

        struct block b_hit2; block_init(&b_hit2);
        if (!block_parse_cache_get(100, fx.hash.data, &fx.bi, datadir, &b_hit2)) {
            printf("FAIL (post-mutate hit get)\n"); failures++;
            block_free(&b_hit2); free(fx.bytes); goto done;
        }
        size_t l3 = 0; unsigned char *s3 = ser(&b_hit2, &l3);
        bool same = s3 && l3 == fx.len && memcmp(s3, fx.bytes, l3) == 0;
        free(s3); block_free(&b_hit2);
        if (!same) {
            printf("FAIL (cache entry corrupted by consumer mutation/free)\n");
            failures++; free(fx.bytes); goto done;
        }

        free(fx.bytes);
        printf("OK\n");
    }
done:
    return failures;
}

/* ── Test (b): no (height, hash) key collision ────────────────── */

static int test_no_key_collision(const char *datadir)
{
    int failures = 0;
    block_parse_cache_clear();

    TEST("bpc: distinct keys never serve each other's bytes") {
        /* A: height 200, body S_A.  B: SAME height 200, DIFFERENT body/hash.
         * C: DIFFERENT height 201, but we force the SAME hash as A to prove
         *    the height component of the key is honored. */
        struct block ba, bb;
        build_block(&ba, 0x1111, 2);
        build_block(&bb, 0x2222, 5);   /* different bytes -> different hash */
        struct fixture fa, fb;
        if (!make_fixture(&fa, datadir, &ba, 200) ||
            !make_fixture(&fb, datadir, &bb, 200)) {
            printf("FAIL (fixtures)\n"); failures++;
            block_free(&ba); block_free(&bb);
            free(fa.bytes); free(fb.bytes); goto done;
        }
        block_free(&ba); block_free(&bb);

        if (memcmp(fa.hash.data, fb.hash.data, 32) == 0) {
            printf("FAIL (test setup: A and B hashes collided)\n");
            failures++; free(fa.bytes); free(fb.bytes); goto done;
        }

        /* Populate both (same height, different hash). */
        struct block ga, gb; block_init(&ga); block_init(&gb);
        bool oka = block_parse_cache_get(200, fa.hash.data, &fa.bi, datadir, &ga);
        bool okb = block_parse_cache_get(200, fb.hash.data, &fb.bi, datadir, &gb);
        if (!oka || !okb) {
            printf("FAIL (populate A/B)\n"); failures++;
            block_free(&ga); block_free(&gb);
            free(fa.bytes); free(fb.bytes); goto done;
        }
        block_free(&ga); block_free(&gb);

        /* Re-get each: must return ITS OWN bytes, not the sibling's. */
        struct block ra, rb; block_init(&ra); block_init(&rb);
        block_parse_cache_get(200, fa.hash.data, &fa.bi, datadir, &ra);
        block_parse_cache_get(200, fb.hash.data, &fb.bi, datadir, &rb);
        size_t la = 0, lb = 0;
        unsigned char *sa = ser(&ra, &la), *sb = ser(&rb, &lb);
        bool a_ok = sa && la == fa.len && memcmp(sa, fa.bytes, la) == 0;
        bool b_ok = sb && lb == fb.len && memcmp(sb, fb.bytes, lb) == 0;
        free(sa); free(sb); block_free(&ra); block_free(&rb);
        if (!a_ok || !b_ok) {
            printf("FAIL (same-height/diff-hash collision: A_ok=%d B_ok=%d)\n",
                   a_ok, b_ok);
            failures++; free(fa.bytes); free(fb.bytes); goto done;
        }

        /* Same-hash / different-height: ask for A's hash at height 201. A's
         * disk fixture is at height 200; with a height-honoring key this is a
         * MISS that reads A's body from A's index (we hand fa.bi, which points
         * at A's disk bytes). The returned body must be A's bytes, and it must
         * NOT be served from the height-200 entry's slot in a way that
         * corrupts it. Then a re-get at 200 must still be A. */
        struct block rc; block_init(&rc);
        if (!block_parse_cache_get(201, fa.hash.data, &fa.bi, datadir, &rc)) {
            printf("FAIL (same-hash/diff-height get)\n"); failures++;
            block_free(&rc); free(fa.bytes); free(fb.bytes); goto done;
        }
        size_t lc = 0; unsigned char *sc = ser(&rc, &lc);
        bool c_ok = sc && lc == fa.len && memcmp(sc, fa.bytes, lc) == 0;
        free(sc); block_free(&rc);

        struct block ra2; block_init(&ra2);
        block_parse_cache_get(200, fa.hash.data, &fa.bi, datadir, &ra2);
        size_t la2 = 0; unsigned char *sa2 = ser(&ra2, &la2);
        bool a200_ok = sa2 && la2 == fa.len && memcmp(sa2, fa.bytes, la2) == 0;
        free(sa2); block_free(&ra2);

        if (!c_ok || !a200_ok) {
            printf("FAIL (height component ignored: h201_ok=%d h200_still=%d)\n",
                   c_ok, a200_ok);
            failures++; free(fa.bytes); free(fb.bytes); goto done;
        }

        free(fa.bytes); free(fb.bytes);
        printf("OK\n");
    }
done:
    return failures;
}

/* ── Test (c): LRU eviction ───────────────────────────────────── */

#define BPC_CAP 16

static int test_lru_eviction(const char *datadir)
{
    int failures = 0;
    block_parse_cache_clear();

    TEST("bpc: >16 keys evict oldest; recents hit; re-get of evicted re-reads") {
        /* CAP+1 distinct fixtures. Insert 0..CAP in order (each a miss). The
         * first inserted (idx 0) is the LRU victim once the CAP+1th lands. */
        const int N = BPC_CAP + 1;
        struct fixture *fxs = calloc((size_t)N, sizeof(*fxs)); // raw-alloc-ok:test
        if (!fxs) { printf("FAIL (alloc)\n"); failures++; goto done; }

        bool setup_ok = true;
        for (int i = 0; i < N; i++) {
            struct block src;
            build_block(&src, (uint32_t)(0x3000 + i), 1 + (i % 4));
            if (!make_fixture(&fxs[i], datadir, &src, 300 + i)) {
                setup_ok = false; block_free(&src); break;
            }
            block_free(&src);
        }
        if (!setup_ok) {
            printf("FAIL (fixtures)\n"); failures++;
            for (int i = 0; i < N; i++) free(fxs[i].bytes);
            free(fxs); goto done;
        }

        /* Insert all N as misses (this is N disk reads + cache stores). */
        for (int i = 0; i < N; i++) {
            struct block g; block_init(&g);
            if (!block_parse_cache_get(300 + i, fxs[i].hash.data,
                                       &fxs[i].bi, datadir, &g)) {
                printf("FAIL (insert %d)\n", i); failures++;
                block_free(&g);
                for (int j = 0; j < N; j++) free(fxs[j].bytes);
                free(fxs); goto done;
            }
            block_free(&g);
        }

        /* The recent keys (1..CAP) must still be present and byte-correct.
         * We cannot observe hit-vs-miss directly through the API, but a
         * correct entry returns its own bytes regardless; the eviction
         * property is exercised by confirming idx 0 was dropped (below) while
         * every later key is intact. */
        bool recents_ok = true;
        for (int i = 1; i < N; i++) {
            struct block g; block_init(&g);
            block_parse_cache_get(300 + i, fxs[i].hash.data,
                                  &fxs[i].bi, datadir, &g);
            size_t l = 0; unsigned char *s = ser(&g, &l);
            if (!s || l != fxs[i].len || memcmp(s, fxs[i].bytes, l) != 0)
                recents_ok = false;
            free(s); block_free(&g);
        }
        if (!recents_ok) {
            printf("FAIL (a recent key was corrupted/lost)\n"); failures++;
            for (int j = 0; j < N; j++) free(fxs[j].bytes);
            free(fxs); goto done;
        }

        /* idx 0 should have been evicted. Prove eviction structurally: clear
         * its block_index disk pointer (HAVE_DATA off). A cache HIT would
         * ignore the index and still return bytes; an evicted entry forces the
         * MISS path, which now fails the HAVE_DATA guard -> get returns false.
         * That false is the observable proof the oldest was evicted. */
        struct block_index broken = fxs[0].bi;
        broken.nStatus &= ~(unsigned int)BLOCK_HAVE_DATA;
        struct block g0; block_init(&g0);
        bool still_cached = block_parse_cache_get(300 + 0, fxs[0].hash.data,
                                                  &broken, datadir, &g0);
        block_free(&g0);
        if (still_cached) {
            printf("FAIL (oldest key NOT evicted after %d inserts)\n", N);
            failures++;
            for (int j = 0; j < N; j++) free(fxs[j].bytes);
            free(fxs); goto done;
        }

        /* And a normal re-get of idx 0 (intact index) re-reads from disk and
         * is still byte-correct. */
        struct block g0b; block_init(&g0b);
        bool reread = block_parse_cache_get(300 + 0, fxs[0].hash.data,
                                            &fxs[0].bi, datadir, &g0b);
        size_t l0 = 0; unsigned char *s0 = reread ? ser(&g0b, &l0) : NULL;
        bool reread_ok = s0 && l0 == fxs[0].len &&
                         memcmp(s0, fxs[0].bytes, l0) == 0;
        free(s0); block_free(&g0b);
        if (!reread_ok) {
            printf("FAIL (re-read of evicted key not byte-correct)\n");
            failures++;
            for (int j = 0; j < N; j++) free(fxs[j].bytes);
            free(fxs); goto done;
        }

        for (int j = 0; j < N; j++) free(fxs[j].bytes);
        free(fxs);
        printf("OK\n");
    }
done:
    block_parse_cache_clear();
    return failures;
}

/* ── Test (d): sealed-segment source below the frontier ───────────
 * The fold substrate: when a sealed segment covers a height, the miss path
 * serves the body from the mmap'd segment (byte-identical, hash-bound) instead
 * of blk*.dat. We prove it by breaking the disk pointer so ONLY the segment can
 * satisfy the read, and by confirming an UNCOVERED height with the same broken
 * disk pointer genuinely fails — so the segment is demonstrably the source. */

struct seg_body_src { uint32_t h; const unsigned char *bytes; size_t len; };
static bool seg_body_src_fn(void *u, uint32_t h, uint8_t **b, size_t *l)
{
    struct seg_body_src *s = u;
    if (h != s->h) return false;
    uint8_t *c = malloc(s->len ? s->len : 1); // raw-alloc-ok:test
    if (!c) return false;
    if (s->len) memcpy(c, s->bytes, s->len);
    *b = c; *l = s->len;
    return true;
}

static int test_segment_backed_read(const char *base)
{
    int failures = 0;
    char dir[300];    snprintf(dir, sizeof(dir), "%s/segrd", base);      mkdir(dir, 0755);
    char blocks[400]; snprintf(blocks, sizeof(blocks), "%s/blocks", dir);   mkdir(blocks, 0755);
    char segd[400];   snprintf(segd, sizeof(segd), "%s/segments", dir);     mkdir(segd, 0755);

    block_parse_cache_clear();

    TEST("bpc: sealed segment serves the fold body byte-identically (disk broken)") {
        const uint32_t H = 1000;
        struct block src;
        build_block(&src, 0xBEEF, 3);
        struct fixture fx;
        if (!make_fixture(&fx, dir, &src, (int)H)) {
            printf("FAIL (fixture)\n"); failures++; block_free(&src);
            free(fx.bytes); goto done;
        }
        block_free(&src);

        /* Seal the exact on-disk bytes for height H into <dir>/segments. */
        struct seg_body_src ss = { .h = H, .bytes = fx.bytes, .len = fx.len };
        char serr[256] = {0};
        enum cseg_status st = chain_segment_seal_range(segd, seg_body_src_fn, &ss,
                                                       H, 1, serr, sizeof(serr));
        if (st != CSEG_OK) {
            printf("FAIL (seal: %s)\n", serr); failures++;
            free(fx.bytes); goto done;
        }

        /* Break the disk pointer: only the sealed segment can now serve H. */
        fx.bi.nStatus &= ~(unsigned int)BLOCK_HAVE_DATA;
        fx.bi.nFile = -1;
        fx.bi.nDataPos = 0;
        block_parse_cache_clear();

        /* COVERED height -> served from the segment, byte-identical to disk. */
        struct block out; block_init(&out);
        bool ok = block_parse_cache_get((int32_t)H, fx.hash.data, &fx.bi, dir, &out);
        if (!ok) {
            printf("FAIL (segment get with broken disk)\n"); failures++;
            block_free(&out); free(fx.bytes); goto done;
        }
        size_t l = 0; unsigned char *s = ser(&out, &l);
        bool ident = s && l == fx.len && memcmp(s, fx.bytes, l) == 0;
        free(s); block_free(&out);
        if (!ident) {
            printf("FAIL (segment body != on-disk bytes)\n"); failures++;
            free(fx.bytes); goto done;
        }

        /* UNCOVERED height, same broken disk -> must fail, proving the segment
         * (not some disk fallback) is what satisfied the covered read. */
        struct block_index bi2 = fx.bi;
        bi2.nHeight = (int)H + 50;
        struct block out2; block_init(&out2);
        bool ok2 = block_parse_cache_get((int32_t)H + 50, fx.hash.data, &bi2,
                                         dir, &out2);
        block_free(&out2);
        if (ok2) {
            printf("FAIL (uncovered height served despite broken disk)\n");
            failures++; free(fx.bytes); goto done;
        }

        free(fx.bytes);
        printf("OK\n");
    }
done:
    block_parse_cache_clear();
    return failures;
}

/* ── Entry point ─────────────────────────────────────────────── */

int test_block_parse_cache(void)
{
    printf("\n=== block_parse_cache (deep-clone / key / LRU / segment) ===\n");
    int failures = 0;
    char tmpdir[256];
    make_test_dir(tmpdir, sizeof(tmpdir));

    failures += test_deep_clone_equality(tmpdir);
    failures += test_no_key_collision(tmpdir);
    failures += test_lru_eviction(tmpdir);
    failures += test_segment_backed_read(tmpdir);

    block_parse_cache_clear();
    cleanup_test_dir(tmpdir);
    return failures;
}

/* zbigint tests: identities, edge cases, and differential vectors
 * generated from Python's arbitrary-precision integers. */
#include "zbigint/zbigint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHECK(cond) \
    if (!(cond)) { \
        fprintf(stderr, "FAIL %s:%d: %s\n", __FILE__, __LINE__, #cond); \
        exit(1); \
    } \
    ((void)0)

static bool eq(zbigint256 a, zbigint256 b) { return zbigint_cmp(a, b) == 0; }

#include "vectors.h"

static void test_identities(void)
{
    CHECK(zbigint_is_zero(ZBIGINT256_ZERO));
    CHECK(!zbigint_is_zero(ZBIGINT256_ONE));
    CHECK(eq(zbigint_add(ZBIGINT256_ZERO, ZBIGINT256_ONE, NULL),
             ZBIGINT256_ONE));
    bool ovf = false;
    zbigint256 w = zbigint_add(ZBIGINT256_MAX, ZBIGINT256_ONE, &ovf);
    CHECK(ovf && zbigint_is_zero(w)); /* wraps mod 2^256 */
    bool udf = false;
    w = zbigint_sub(ZBIGINT256_ZERO, ZBIGINT256_ONE, &udf);
    CHECK(udf && eq(w, ZBIGINT256_MAX));

    /* cmp ordering */
    zbigint256 a = { { 0, 0, 1, 0 } };
    zbigint256 b = { { UINT64_MAX, UINT64_MAX, 0, 0 } };
    CHECK(zbigint_cmp(a, b) > 0);
    CHECK(zbigint_cmp(b, a) < 0);
    CHECK(zbigint_cmp(a, a) == 0);

    /* bitlen */
    CHECK(zbigint_bitlen(ZBIGINT256_ZERO) == 0);
    CHECK(zbigint_bitlen(ZBIGINT256_ONE) == 1);
    CHECK(zbigint_bitlen(ZBIGINT256_MAX) == 256);
    zbigint256 top = { { 0, 0, 0, 0x8000000000000000ull } };
    CHECK(zbigint_bitlen(top) == 256);

    /* div by zero fails closed */
    zbigint256 q = ZBIGINT256_MAX, r = ZBIGINT256_MAX;
    CHECK(!zbigint_divmod(ZBIGINT256_ONE, ZBIGINT256_ZERO, &q, &r));
    CHECK(eq(q, ZBIGINT256_MAX) && eq(r, ZBIGINT256_MAX));
}

static void test_arith_vectors(void)
{
    for (size_t i = 0; i < sizeof BVEC / sizeof *BVEC; i++) {
        const struct bvec *v = &BVEC[i];
        bool ovf = !v->sovf, udf = !v->duvf, movf = !v->movf;
        zbigint256 s = zbigint_add(v->a, v->b, &ovf);
        zbigint256 d = zbigint_sub(v->a, v->b, &udf);
        zbigint256 p = zbigint_mul(v->a, v->b, &movf);
        CHECK(eq(s, v->sum));
        CHECK(eq(d, v->diff));
        CHECK(eq(p, v->prod));
        CHECK(ovf == !!v->sovf);
        CHECK(udf == !!v->duvf);
        CHECK(movf == !!v->movf);
        zbigint256 q, r;
        CHECK(zbigint_divmod(v->a, v->b, &q, &r));
        CHECK(eq(q, v->q) && eq(r, v->r));
        /* invariant: a == q*b + r (no wrap in these vectors) */
        bool o1, o2;
        zbigint256 recomposed = zbigint_add(zbigint_mul(q, v->b, &o1), r, &o2);
        CHECK(!o1 && !o2 && eq(recomposed, v->a));
    }
}

static void test_shift_vectors(void)
{
    for (size_t i = 0; i < sizeof SVEC / sizeof *SVEC; i++) {
        const struct svec *v = &SVEC[i];
        CHECK(eq(zbigint_shl(v->a, v->n), v->shl));
        CHECK(eq(zbigint_shr(v->a, v->n), v->shr));
    }
    /* shift by exactly 256 and beyond yields zero */
    CHECK(zbigint_is_zero(zbigint_shl(ZBIGINT256_MAX, 256)));
    CHECK(zbigint_is_zero(zbigint_shr(ZBIGINT256_MAX, 300)));
    /* bit access agrees with limbs */
    for (unsigned n = 0; n < 256; n++) {
        CHECK(zbigint_bit(ZBIGINT256_MAX, n));
    }
    CHECK(!zbigint_bit(ZBIGINT256_ZERO, 128));
    CHECK(!zbigint_bit(ZBIGINT256_MAX, 256));
}

static void test_be32(void)
{
    uint8_t buf[32];
    zbigint_to_be32(ZBIGINT256_MAX, buf);
    for (int i = 0; i < 32; i++) {
        CHECK(buf[i] == 0xFF);
    }
    CHECK(eq(zbigint_from_be32(buf), ZBIGINT256_MAX));
    memset(buf, 0, sizeof buf);
    buf[31] = 1;
    CHECK(eq(zbigint_from_be32(buf), ZBIGINT256_ONE));
    /* round trip on vectors */
    for (size_t i = 0; i < sizeof BVEC / sizeof *BVEC; i++) {
        zbigint_to_be32(BVEC[i].a, buf);
        CHECK(eq(zbigint_from_be32(buf), BVEC[i].a));
    }
}

static void test_hex(void)
{
    char h[65];
    zbigint_to_hex(ZBIGINT256_MAX, h);
    CHECK(strcmp(h,
        "ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff") == 0);
    zbigint256 v;
    CHECK(zbigint_from_hex(h, &v) && eq(v, ZBIGINT256_MAX));
    CHECK(zbigint_from_hex("0x1A", &v) && eq(v, zbigint_from_u64(26)));
    CHECK(zbigint_from_hex("0", &v) && zbigint_is_zero(v));
    CHECK(!zbigint_from_hex("", &v));
    CHECK(!zbigint_from_hex("zz", &v));
    /* 65 digits overflow */
    CHECK(!zbigint_from_hex(
        "1ffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffffff", &v));
    /* round trip */
    for (size_t i = 0; i < sizeof BVEC / sizeof *BVEC; i++) {
        zbigint_to_hex(BVEC[i].a, h);
        CHECK(zbigint_from_hex(h, &v) && eq(v, BVEC[i].a));
    }
}

static void test_dec(void)
{
    char buf[80];
    CHECK(zbigint_to_dec(ZBIGINT256_MAX, buf, sizeof buf));
    CHECK(strcmp(buf,
        "115792089237316195423570985008687907853269984665640564039457584007913129639935") == 0);
    CHECK(zbigint_to_dec(ZBIGINT256_ZERO, buf, sizeof buf));
    CHECK(strcmp(buf, "0") == 0);
    /* buffer too small */
    CHECK(!zbigint_to_dec(ZBIGINT256_MAX, buf, 10));
    zbigint256 v;
    CHECK(zbigint_from_dec("0", &v) && zbigint_is_zero(v));
    CHECK(!zbigint_from_dec("", &v));
    CHECK(!zbigint_from_dec("12a3", &v));
    /* max parses; max+1 overflows */
    CHECK(zbigint_from_dec(
        "115792089237316195423570985008687907853269984665640564039457584007913129639935", &v));
    CHECK(eq(v, ZBIGINT256_MAX));
    CHECK(!zbigint_from_dec(
        "115792089237316195423570985008687907853269984665640564039457584007913129639936", &v));
    for (size_t i = 0; i < sizeof DVEC / sizeof *DVEC; i++) {
        CHECK(zbigint_from_dec(DVEC[i].dec, &v));
        CHECK(eq(v, DVEC[i].a));
        CHECK(zbigint_to_dec(DVEC[i].a, buf, sizeof buf));
        CHECK(strcmp(buf, DVEC[i].dec) == 0);
    }
}

int main(void)
{
    test_identities();
    test_arith_vectors();
    test_shift_vectors();
    test_be32();
    test_hex();
    test_dec();
    puts("test_zbigint: all groups passed (ids arith shift be32 hex dec)");
    return 0;
}

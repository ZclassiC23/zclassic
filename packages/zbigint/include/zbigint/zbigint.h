/* zbigint — fixed-size 256-bit unsigned integer arithmetic (C23).
 *
 * A zbigint256 is four little-endian u64 limbs: value =
 * l[0] + l[1]*2^64 + l[2]*2^128 + l[3]*2^192. Fixed size means no
 * allocation, no aliasing surprises, and predictable cost — the
 * shape used by hashes, IDs, checksums, and crypto-adjacent code
 * (this is NOT constant-time; do not use for secrets).
 *
 * All arithmetic is modular (wraps mod 2^256); *_overflow variants
 * report whether the true mathematical result exceeded the modulus.
 *
 * MIT licensed. No dependencies beyond libc.
 */
#ifndef ZBIGINT_H
#define ZBIGINT_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    uint64_t l[4]; /* little-endian limbs */
} zbigint256;

extern const zbigint256 ZBIGINT256_ZERO;
extern const zbigint256 ZBIGINT256_ONE;
extern const zbigint256 ZBIGINT256_MAX; /* 2^256 - 1 */

zbigint256 zbigint_from_u64(uint64_t v);
bool       zbigint_is_zero(zbigint256 a);
int        zbigint_cmp(zbigint256 a, zbigint256 b); /* -1/0/1 */
unsigned   zbigint_bitlen(zbigint256 a);            /* 0 for zero */

zbigint256 zbigint_add(zbigint256 a, zbigint256 b, bool *overflow);
zbigint256 zbigint_sub(zbigint256 a, zbigint256 b, bool *underflow);
zbigint256 zbigint_mul(zbigint256 a, zbigint256 b, bool *overflow);
/* Division: q = a/b, r = a%b. Division by zero returns false and
 * leaves outputs untouched. */
bool zbigint_divmod(zbigint256 a, zbigint256 b,
                    zbigint256 *q, zbigint256 *r);

zbigint256 zbigint_shl(zbigint256 a, unsigned n); /* bits shifted out are lost */
zbigint256 zbigint_shr(zbigint256 a, unsigned n);
bool       zbigint_bit(zbigint256 a, unsigned n); /* n >= 256 -> false */

/* Byte order: big-endian 32-byte arrays, hash/ID style. */
void       zbigint_to_be32(zbigint256 a, uint8_t out[32]);
zbigint256 zbigint_from_be32(const uint8_t in[32]);

/* Hex: lowercase, no prefix. to_hex writes exactly 65 bytes
 * (64 hex + NUL). from_hex accepts up to 64 digits, optional 0x
 * prefix; returns false on bad characters or overflow. */
void       zbigint_to_hex(zbigint256 a, char out[65]);
bool       zbigint_from_hex(const char *s, zbigint256 *out);

/* Decimal: to_dec writes into out (needs <= 78 digits + NUL);
 * returns false when out_cap is too small. from_dec rejects
 * non-digits and overflow. */
bool zbigint_to_dec(zbigint256 a, char *out, size_t out_cap);
bool zbigint_from_dec(const char *s, zbigint256 *out);

#ifdef __cplusplus
}
#endif

#endif /* ZBIGINT_H */

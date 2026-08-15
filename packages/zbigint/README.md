# zbigint

Fixed-size 256-bit unsigned integer arithmetic in portable C23, no
dependencies beyond libc.

Four little-endian u64 limbs. Fixed size means no allocation and
predictable cost — the shape used by hashes, IDs, and checksums.
All arithmetic is modular (wraps mod 2^256); the `*_overflow` out
parameters report when the true mathematical result exceeded the
modulus. Not constant-time — do not use for secrets.

## API sketch

```c
zbigint256 a = zbigint_from_u64(42);
bool ovf;
zbigint256 b = zbigint_mul(a, ZBIGINT256_MAX, &ovf);
zbigint_divmod(b, a, &q, &r);          /* binary long division */
zbigint_to_dec(b, buf, sizeof buf);    /* exact decimal */
```

Also: cmp, bitlen, bit, shl/shr, big-endian 32-byte import/export,
canonical hex (64 lowercase digits) and decimal conversion, both
fail-closed on malformed input.

Verified against 220 differential vectors generated from Python's
arbitrary-precision integers, plus edge identities (wrap at 2^256,
division by zero, 256-bit shifts).

## CLI

```sh
cc -std=c23 -Iinclude -o zbigint app/main.c src/zbigint.c
./zbigint mul 0xffffffffffffffffffffffffffffffff 0x10001
./zbigint dec 0xdeadc0de
```

## License

MIT

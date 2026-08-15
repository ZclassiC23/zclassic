/* zbigint CLI: 256-bit unsigned arithmetic over hex or decimal args.
 *
 *   zbigint add|sub|mul A B     prints modular result (hex)
 *   zbigint div|mod A B         prints quotient / remainder
 *   zbigint shl|shr A N         prints shifted value
 *   zbigint dec A               prints decimal form
 *   zbigint hex A               prints canonical hex form
 *
 * A/B accept 0x-prefixed hex or plain decimal.
 */
#include "zbigint/zbigint.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int parse(const char *s, zbigint256 *out)
{
    if (s[0] == '0' && (s[1] == 'x' || s[1] == 'X'))
        return zbigint_from_hex(s, out);
    /* plain hex if it has hex letters, else decimal */
    if (strpbrk(s, "abcdefABCDEF")) return zbigint_from_hex(s, out);
    return zbigint_from_dec(s, out);
}

static void show(zbigint256 v)
{
    char h[65];
    zbigint_to_hex(v, h);
    printf("0x%s\n", h);
}

int main(int argc, char **argv)
{
    if (argc < 3) {
        fprintf(stderr,
            "usage: zbigint add|sub|mul|div|mod A B | shl|shr A N | dec|hex A\n");
        return 2;
    }
    zbigint256 a, b;
    if (!parse(argv[2], &a)) {
        fprintf(stderr, "zbigint: bad number %s\n", argv[2]);
        return 2;
    }
    if (strcmp(argv[1], "dec") == 0) {
        char buf[80];
        if (!zbigint_to_dec(a, buf, sizeof buf)) return 1;
        puts(buf);
        return 0;
    }
    if (strcmp(argv[1], "hex") == 0) {
        show(a);
        return 0;
    }
    if (argc < 4) {
        fprintf(stderr, "zbigint: missing operand\n");
        return 2;
    }
    if (strcmp(argv[1], "shl") == 0 || strcmp(argv[1], "shr") == 0) {
        char *end = NULL;
        unsigned long n = strtoul(argv[3], &end, 10);
        if (!end || *end) return 2;
        show(argv[1][2] == 'l' ? zbigint_shl(a, (unsigned)n)
                               : zbigint_shr(a, (unsigned)n));
        return 0;
    }
    if (!parse(argv[3], &b)) {
        fprintf(stderr, "zbigint: bad number %s\n", argv[3]);
        return 2;
    }
    char op = argv[1][0];
    if (op == 'a') show(zbigint_add(a, b, NULL));
    else if (op == 's') show(zbigint_sub(a, b, NULL));
    else if (op == 'm' && argv[1][1] == 'u') show(zbigint_mul(a, b, NULL));
    else if (op == 'd' || op == 'm') {
        zbigint256 q, r;
        if (!zbigint_divmod(a, b, &q, &r)) {
            fprintf(stderr, "zbigint: division by zero\n");
            return 1;
        }
        show(op == 'd' ? q : r);
    } else {
        fprintf(stderr, "zbigint: unknown op %s\n", argv[1]);
        return 2;
    }
    return 0;
}

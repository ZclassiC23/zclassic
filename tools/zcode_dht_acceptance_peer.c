/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Acceptance-only identity helper for tools/dev/zcode_dht_acceptance.sh.
 * It never opens a node datadir or a socket.  The live wire leg is carried
 * by the two real nodes; this helper only derives the public half of a fixed
 * 32-byte Ed25519 seed so the harness can anchor that master through the
 * operator command surface without exposing a key-derivation command in the
 * production binary.
 */

#include "base/hex.h"
#include "crypto/ed25519.h"
#include "support/cleanse.h"

#include <stdio.h>
#include <string.h>

static int usage(const char *prog)
{
    fprintf(stderr, "usage: %s pubkey <64-lower-hex-seed>\n", prog);
    return 2;
}

int main(int argc, char **argv)
{
    if (argc != 3 || strcmp(argv[1], "pubkey") != 0)
        return usage(argv[0]);

    uint8_t seed[32], pubkey[32], secret_copy[32];
    if (strlen(argv[2]) != 64 ||
        !zcl_hex_decode_lower(argv[2], seed, sizeof(seed))) {
        memory_cleanse(seed, sizeof(seed));
        fprintf(stderr, "seed must be exactly 64 lowercase hex characters\n");
        return 2;
    }
    zcl_ed25519_keypair(pubkey, secret_copy, seed);
    memory_cleanse(secret_copy, sizeof(secret_copy));
    memory_cleanse(seed, sizeof(seed));

    char hex[65];
    zcl_hex_encode(pubkey, sizeof(pubkey), hex);
    puts(hex);
    memory_cleanse(pubkey, sizeof(pubkey));
    memory_cleanse(hex, sizeof(hex));
    return 0;
}

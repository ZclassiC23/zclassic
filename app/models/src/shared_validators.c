/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ar-validate-skip:shared-helpers-not-a-row
 *   Pure helper module defining zcl_validate_zcl_address and friends
 *   used by validates_zcl_address. Has no record of its own. */

#include "models/shared_validators.h"

#include <string.h>

#include "keys/key_io.h"
#include "chain/chainparams.h"
#include "wallet/sapling_keys.h"

bool zcl_validate_zcl_address(const char *addr)
{
    if (!addr || !addr[0])
        return false;

    size_t len = strlen(addr);

    /* Charset gate: alphanumeric + underscore only. Both t-addrs and
     * zs1 Sapling addresses fit this set. Catches one-character typos
     * cheaply and prevents XSS via address echo in HTML. */
    for (size_t i = 0; i < len; i++) {
        char c = addr[i];
        bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                  (c >= '0' && c <= '9') || c == '_';
        if (!ok)
            return false;
    }

    /* Transparent address: t1 (P2PKH) or t3 (P2SH), Base58Check encoded.
     * decode_destination validates the version-prefix and checksum. */
    if ((addr[0] == 't') && (addr[1] == '1' || addr[1] == '3') &&
        len >= 26 && len <= 36) {
        const struct chain_params *cp = chain_params_get();
        size_t pk_len = 0, sc_len = 0;
        const unsigned char *pk_pfx =
            chain_params_base58_prefix(cp, B58_PUBKEY_ADDRESS, &pk_len);
        const unsigned char *sc_pfx =
            chain_params_base58_prefix(cp, B58_SCRIPT_ADDRESS, &sc_len);
        struct tx_destination dest;
        return decode_destination(addr, pk_pfx, pk_len, sc_pfx, sc_len, &dest);
    }

    /* Sapling shielded address: zs1 with bech32 checksum.
     * sapling_decode_payment_address validates HRP, checksum, and the
     * 11-byte diversifier + 32-byte pk_d split. */
    if (len >= 70 && addr[0] == 'z' && addr[1] == 's' && addr[2] == '1') {
        uint8_t d[ZC_DIVERSIFIER_SIZE];
        uint8_t pk_d[32];
        return sapling_decode_payment_address(addr, d, pk_d);
    }

    return false;
}

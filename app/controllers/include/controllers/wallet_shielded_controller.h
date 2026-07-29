/* Copyright 2026 Rhett Creighton - Apache License 2.0 */

#ifndef ZCL_CONTROLLERS_WALLET_SHIELDED_CONTROLLER_H
#define ZCL_CONTROLLERS_WALLET_SHIELDED_CONTROLLER_H

#include <stdbool.h>

struct rpc_table;

void register_wallet_shielded_rpc_commands(struct rpc_table *t);

/* True when `addr` is a Sapling payment address on the ACTIVE chain.
 * The human-readable part is read from chain_params_get() — mainnet
 * "zs1...", testnet "ztestsapling1...", regtest "zregtestsapling1..." —
 * because sapling_decode_payment_address() ignores the HRP, so this prefix
 * test is the only thing that routes a z_sendmany recipient to the shielded
 * branch. A hardcoded "zs1" sent every testnet/regtest z-recipient into the
 * transparent branch, where the send died on "Invalid transparent address".
 * Wallet-local routing only: no consensus effect, and a false answer can
 * only refuse a send, never mint one. Defined in wallet_shielded_send.c. */
bool wallet_addr_is_sapling(const char *addr);

#endif

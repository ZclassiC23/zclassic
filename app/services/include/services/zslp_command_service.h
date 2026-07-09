/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * ZSLP command service — command-side workflow helpers. */

#ifndef ZCL_ZSLP_COMMAND_SERVICE_H
#define ZCL_ZSLP_COMMAND_SERVICE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "models/zslp.h"
#include "services/zslp_service.h"
#include "util/result.h"

struct wallet;
struct wallet_tx;
struct wallet_tx_admission;

struct zcl_result zslp_command_commit_with_op_return(struct wallet *wallet,
                                        struct wallet_tx *wtx,
                                        const struct wallet_tx_admission *admission,
                                        const uint8_t *op_script,
                                        size_t script_len);
struct zcl_result zslp_command_build_genesis_base_tx(struct wallet *wallet,
                                        struct wallet_tx *wtx,
                                        int64_t *fee_paid,
                                        const char **tx_error);
struct zcl_result zslp_command_build_send_base_tx(struct wallet *wallet,
                                     const char *to_addr,
                                     struct wallet_tx *wtx,
                                     int64_t *fee_paid,
                                     const char **tx_error);

/* Build a base tx whose SOLE input is a coin the wallet controls that pays
 * owner_address (P2PKH only). Used by mutation commands (UPDATE, TRANSFER,
 * RENEW, SET_RECORD, SET_TEXT) so the resulting tx's first input — the
 * signer the ZNAM projection treats as ownership proof, see
 * app/models/src/explorer_index.c:znam_owner_address — is provably the
 * current name owner, not an arbitrary wallet address. Fails closed if the
 * wallet does not hold the owner's private key or has no spendable coin
 * under that address. Output 0 pays 546 sat (dust) back to owner_address;
 * a second change output is added if the selected coin has excess value. */
struct zcl_result zslp_command_build_owner_base_tx(struct wallet *wallet,
                                     const char *owner_address,
                                     struct wallet_tx *wtx,
                                     int64_t *fee_paid,
                                     const char **tx_error);

struct zcl_result zslp_command_finalize_genesis(const char *datadir,
                                   const char *broadcast_txid,
                                   const struct zslp_token_create_request *req,
                                   char token_id_out[ZSLP_TOKEN_KEY_MAX + 1]);

struct zcl_result zslp_command_credit_transfer(const char *datadir,
                                  const struct zslp_token_transfer_request *req);

#endif

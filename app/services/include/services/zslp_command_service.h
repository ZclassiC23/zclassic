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
struct tx_mempool;
struct wallet_tx;

struct zcl_result zslp_command_commit_with_op_return(struct wallet *wallet,
                                        struct tx_mempool *mempool,
                                        struct wallet_tx *wtx,
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

struct zcl_result zslp_command_finalize_genesis(const char *datadir,
                                   const char *broadcast_txid,
                                   const struct zslp_token_create_request *req,
                                   char token_id_out[ZSLP_TOKEN_KEY_MAX + 1]);

struct zcl_result zslp_command_credit_transfer(const char *datadir,
                                  const struct zslp_token_transfer_request *req);

#endif

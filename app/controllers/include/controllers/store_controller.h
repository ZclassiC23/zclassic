/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Store controller — ZSLP token commerce over .onion.
 *
 * Routes:
 *   GET  /store               List products
 *   GET  /store/products      List products
 *   GET  /store/products/:id  Product detail
 *   POST /store/orders        Create order, generate z-address
 *   GET  /store/orders/:id    Check payment status
 *
 * Compatibility aliases:
 *   GET  /store/product/:id
 *   POST /store/buy/:id
 *   GET  /store/order/:id
 *
 * Payment flow:
 *   1. Customer browses products (GET /store)
 *   2. Customer selects product (GET /store/product/1)
 *   3. Node generates unique z-address for payment
 *   4. Customer sends shielded ZCL to z-address
 *   5. Background thread detects payment (z_listunspent)
 *   6. Node mints ZSLP tokens to customer's t-address
 *   7. Customer accesses token-gated services */

#ifndef ZCL_CONTROLLERS_STORE_H
#define ZCL_CONTROLLERS_STORE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* Handle a store request. Returns HTTP response bytes written. */
size_t store_handle_request(const char *method, const char *path,
                             const uint8_t *body, size_t body_len,
                             uint8_t *response, size_t response_max,
                             const char *datadir);

/* Background: check pending orders for payments, mint tokens. */
void store_process_payments(const char *datadir);

/* Check if customer has enough ZSLP tokens for a service.
 * Used as before_action hook on protected routes. */
bool store_check_token_access(const char *datadir,
                               const char *customer_addr,
                               const char *token_id,
                               uint64_t required);

#endif

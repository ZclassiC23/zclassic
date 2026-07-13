/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * ZCL Names (ZNAM) HTML site views — the presentation half of the names
 * MVC slice. Renders the browse index, a name's profile page (which doubles
 * as the default hosted site when a name has no onion/url binding), and the
 * on-chain register form (CSRF token + in-browser proof-of-work solver).
 *
 * Pure rendering: every function fills a caller-owned response buffer with a
 * complete raw HTTP/1.1 response and returns the byte count (0 = would not
 * fit). No storage, no chain access — the controller reads the projection
 * and hands typed rows in. */

#ifndef ZCL_VIEWS_NAME_VIEW_H
#define ZCL_VIEWS_NAME_VIEW_H

#include "models/znam.h"

#include <stddef.h>
#include <stdint.h>

/* Shared HTTP wrappers (Content-Length-bearing). */
size_t name_html_response(const char *body, size_t body_len,
                          uint8_t *resp, size_t max);
size_t name_error_response(const char *status_code,
                           const char *body, size_t body_len,
                           uint8_t *resp, size_t max);

/* GET /names — browse index of registered names. */
size_t name_view_index(const struct znam_entry *entries, int count,
                       uint8_t *resp, size_t max);

/* GET /names/{name} and the /n/{name} profile fallback — a name's public
 * profile page (owner, primary target, resolver records). Serves as the
 * default hosted site when the name binds no onion/url target. `text`/`addr`
 * arrays may be empty. */
size_t name_view_profile(const struct znam_entry *e,
                         const struct znam_text_record *text, int ntext,
                         const struct znam_addr_record *addr, int naddr,
                         uint8_t *resp, size_t max);

/* GET /names/register — the on-chain registration form. Embeds the CSRF
 * token and the product-style proof-of-work challenge; a from-scratch
 * SHA3-256 solver runs in the browser and fills pow_nonce before submit. */
size_t name_view_register_form(const char *csrf_tok, int64_t pow_ts,
                               uint8_t *resp, size_t max);

/* POST /names/register result page. On success txid is non-empty; on refusal
 * err carries the reason. */
size_t name_view_register_result(const char *name, const char *value,
                                 const char *txid, const char *err,
                                 uint8_t *resp, size_t max);

/* 404 helper for an unknown name. */
size_t name_view_not_found(const char *name, uint8_t *resp, size_t max);

#endif /* ZCL_VIEWS_NAME_VIEW_H */

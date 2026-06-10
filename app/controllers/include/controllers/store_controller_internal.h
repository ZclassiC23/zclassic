/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Internal shared includes + helper/handler decls for the store
 * controller. Included by store_controller*.c only — not public.
 * The CSRF helpers below are defined in store_controller.c; the
 * presentation emitters + response/format helpers were moved to the
 * store view (app/views/src/store_view.c) and are declared in
 * views/store_internal.h, pulled in below. */

#ifndef ZCL_CONTROLLERS_STORE_CONTROLLER_INTERNAL_H
#define ZCL_CONTROLLERS_STORE_CONTROLLER_INTERNAL_H

#include "platform/time_compat.h"
#include "views/format_helpers.h"
#include "views/store_internal.h"
#include "controllers/store_controller.h"
#include "controllers/zslp_controller.h"
#include "models/database.h"
#include "models/shared_validators.h"
#include "models/store.h"
#include "services/zslp_service.h"
#include "script/standard.h"
#include "wallet/sapling_keys.h"
#include "crypto/hmac_sha256.h"
#include "core/random.h"
#include "util/template.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sqlite3.h>
#include "util/ar_step_readonly.h"
#include "util/log_macros.h"
#include <sqlite3.h>

/* store_controller_schema.c — SQLite schema bootstrap. */
void store_ensure_schema(sqlite3 *db, const char *datadir);

/* ── CSRF form-token helpers (defined in store_controller.c) ── */
void store_csrf_token(const char *context, char out[33]);
void store_csrf_context(char *out, size_t outmax, int64_t product_id);

#endif /* ZCL_CONTROLLERS_STORE_CONTROLLER_INTERNAL_H */

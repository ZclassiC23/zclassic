/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "wallet_diagnostic_internal.h"

#include "controllers/strong_params.h"
#include "json/json.h"
#include "services/wallet_backup_service.h"
#include "util/log_macros.h"

static void wallet_backup_status_to_json(struct json_value *result,
                                         const struct wallet_backup_status *s)
{
    json_set_object(result);
    json_push_kv_bool(result, "running", s->running);
    json_push_kv_int(result, "total_runs", s->total_runs);
    json_push_kv_int(result, "total_failures", s->total_failures);
    json_push_kv_int(result, "last_run_unix", s->last_run_unix);
    json_push_kv_int(result, "last_size_bytes", s->last_size_bytes);
    json_push_kv_int(result, "last_key_count", s->last_key_count);
    json_push_kv_int(result, "last_duration_ms", s->last_duration_ms);
    json_push_kv_str(result, "last_path", s->last_path);
    json_push_kv_str(result, "last_error", s->last_error);
}

bool rpc_walletbackupstatus(const struct json_value *params, bool help,
                            struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "walletbackupstatus\n"
        "Returns the wallet backup service status and last verified backup.");

    struct wallet_backup_status status;
    wallet_backup_status_snapshot(&status);
    wallet_backup_status_to_json(result, &status);
    return true;
}

bool rpc_walletbackupnow(const struct json_value *params, bool help,
                         struct json_value *result)
{
    (void)params;
    RPC_HELP(help, result,
        "walletbackupnow\n"
        "Runs one synchronous wallet backup and verifies the copied rows.");

    struct zcl_result r = wallet_backup_now();
    if (!r.ok) {
        json_set_object(result);
        json_push_kv_bool(result, "ok", false);
        json_push_kv_int(result, "code", r.code);
        json_push_kv_str(result, "error", r.message);
        LOG_FAIL("wallet_diag", "walletbackupnow failed (code=%d): %s",
                 r.code, r.message);
        return true;
    }

    struct wallet_backup_status status;
    wallet_backup_status_snapshot(&status);
    wallet_backup_status_to_json(result, &status);
    json_push_kv_bool(result, "ok", true);
    return true;
}

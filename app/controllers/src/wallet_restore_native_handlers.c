/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Registry handlers for the two OFFLINE wallet-recovery leaves —
 * `core.wallet.restore` and `core.wallet.backup.decrypt`.
 *
 * These are the odd ones out in the wallet command surface: every other
 * wallet leaf proxies a JSON-RPC call to a RUNNING node, and these two
 * must work when there is no running node at all. That is the whole point.
 * A user restoring onto a rebuilt machine has a backup file and an empty
 * datadir; asking them to start a node first would ask them to boot the
 * very wallet they are trying to put back. So both handlers call the
 * service directly, in-process, and `core.wallet.restore` REFUSES if a
 * node is holding the target datadir (the pidfile flock is the
 * single-writer lock — see docs/WALLET_PERSISTENCE_RECOVERY.md).
 *
 * Both carry ZCL_COMMAND_CONFIRM_PLAN_COMMIT, and the plan half is not
 * cosmetic:
 *   - restore's plan runs the FULL merge inside a transaction and rolls it
 *     back, so the counts it shows are exactly what the commit will do;
 *   - decrypt's plan names the plaintext file it would write without
 *     writing it, because that file contains every private key in the
 *     clear.
 *
 * Bound in config/commands/core.def.
 */

#include "controllers/wallet_native_handlers.h"

#include "controllers/agent_controller.h"
#include "json/json.h"
#include "kernel/command_registry.h"
#include "command/native_command.h"
#include "services/wallet_backup_service.h"
#include "services/wallet_restore_service.h"
#include "util/log_macros.h"

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WRN_TAG "native.wallet.restore"

static void wrn_fail(struct zcl_command_reply *reply,
                     enum zcl_command_exit exit_code, const char *code,
                     const char *message, const char *evidence)
{
    enum zcl_command_status status =
        exit_code == ZCL_COMMAND_EXIT_BLOCKED ? ZCL_COMMAND_STATUS_BLOCKED
                                              : ZCL_COMMAND_STATUS_FAILED;
    LOG_ERROR(WRN_TAG, "%s: %s (%s)", code, message,
              evidence && evidence[0] ? evidence : "-");
    zcl_command_reply_fail(reply, status, exit_code, code, "handle", false,
                           false, message, evidence ? evidence : "");
}

/* Deterministic non-secret token binding a plan preview to its parameters,
 * mirroring wnh_plan_token in wallet_native_handlers.c. */
static void wrn_plan_token(char out[17], const char *a, const char *b)
{
    uint64_t h = 1469598103934665603ULL;
    const char *parts[2] = { a, b };
    for (int i = 0; i < 2; i++) {
        for (const char *p = parts[i]; p && *p; p++) {
            h ^= (unsigned char)*p;
            h *= 1099511628211ULL;
        }
        h ^= 0x1f;
        h *= 1099511628211ULL;
    }
    (void)snprintf(out, 17, "%016llx", (unsigned long long)h);
}

/* The datadir a restore targets when the caller names none: the runtime's
 * own context datadir, else the stock $HOME/.zclassic-c23. Always echoed
 * back in the plan so the operator confirms the path before the commit. */
static void wrn_default_datadir(char *out, size_t cap)
{
    const char *ctx = agent_runtime_context_datadir();
    if (ctx && ctx[0]) {
        snprintf(out, cap, "%s", ctx);
        return;
    }
    const char *home = getenv("HOME");
    snprintf(out, cap, "%s/.zclassic-c23", home && home[0] ? home : ".");
}

/* One per-table row of the restore report. */
static void wrn_push_table(struct json_value *arr,
                           const struct wallet_restore_table_report *t)
{
    struct json_value row;
    json_init(&row);
    json_set_object(&row);
    (void)json_push_kv_str(&row, "table", t->table);
    (void)json_push_kv_bool(&row, "in_backup", t->in_backup);
    (void)json_push_kv_int(&row, "rows_in_backup", t->rows_in_backup);
    (void)json_push_kv_int(&row, "manifest_row_count", t->manifest_row_count);
    (void)json_push_kv_int(&row, "rows_before", t->rows_before);
    (void)json_push_kv_int(&row, "rows_inserted", t->rows_inserted);
    (void)json_push_kv_int(&row, "rows_collided", t->rows_collided);
    (void)json_push_kv_int(&row, "rows_rejected", t->rows_rejected);
    (void)json_push_kv_int(&row, "rows_after", t->rows_after);
    (void)json_push_back(arr, &row);
    json_free(&row);
}

/* Render a completed report (dry run or committed) onto the reply. */
static void wrn_push_report(struct zcl_command_reply *reply,
                            const struct wallet_restore_report *rep)
{
    (void)json_push_kv_str(&reply->data, "backup_path", rep->backup_path);
    (void)json_push_kv_str(&reply->data, "target_db", rep->target_db);
    (void)json_push_kv_bool(&reply->data, "dry_run", rep->dry_run);
    (void)json_push_kv_bool(&reply->data, "source_was_encrypted",
                            rep->source_was_encrypted);
    (void)json_push_kv_bool(&reply->data, "target_created",
                            rep->target_created);
    (void)json_push_kv_str(&reply->data, "collision_policy", "keep-existing");
    (void)json_push_kv_int(&reply->data, "tables_in_backup",
                           rep->tables_in_backup);
    (void)json_push_kv_int(&reply->data, "total_rows_in_backup",
                           rep->total_rows_in_backup);
    (void)json_push_kv_int(&reply->data, "total_inserted",
                           rep->total_inserted);
    (void)json_push_kv_int(&reply->data, "total_collided",
                           rep->total_collided);
    (void)json_push_kv_int(&reply->data, "total_rejected",
                           rep->total_rejected);
    (void)json_push_kv_int(&reply->data, "manifest_mismatches",
                           rep->manifest_mismatches);
    (void)json_push_kv_str(&reply->data, "warnings",
                           rep->warnings[0] ? rep->warnings : "none");

    struct json_value arr;
    json_init(&arr);
    json_set_array(&arr);
    for (size_t i = 0; i < rep->n_tables; i++)
        wrn_push_table(&arr, &rep->tables[i]);
    (void)json_push_kv(&reply->data, "tables", &arr);
    json_free(&arr);
}

void zcl_native_handle_wallet_restore(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    const char *from = json_get_str(json_get(request->input, "from"));
    if (!from || !from[0]) {
        wrn_fail(reply, ZCL_COMMAND_EXIT_INVALID, "MISSING_FROM",
                 "from is required: the path of the backup file to restore",
                 "core.wallet.restore");
        return;
    }
    char datadir[1024];
    const char *dd = json_get_str(json_get(request->input, "datadir"));
    if (dd && dd[0])
        snprintf(datadir, sizeof(datadir), "%s", dd);
    else
        wrn_default_datadir(datadir, sizeof(datadir));

    const char *password = json_get_str(json_get(request->input, "password"));
    bool confirm = json_get_bool_or(request->input, "confirm", false);

    char token[17];
    wrn_plan_token(token, from, datadir);

    struct wallet_restore_request req = {
        .backup_path = from,
        .datadir     = datadir,
        .password    = password,
        .dry_run     = !confirm,
    };
    struct wallet_restore_report rep;
    struct zcl_result r = wallet_restore_run(&req, &rep);

    if (!r.ok) {
        /* A refusal still reports what it learned about the file, so the
         * operator's next move is informed rather than guessed. */
        bool blocked = r.code == -34;   /* datadir held by a running node */
        wrn_push_report(reply, &rep);
        wrn_fail(reply,
                 blocked ? ZCL_COMMAND_EXIT_BLOCKED : ZCL_COMMAND_EXIT_FAILED,
                 blocked ? "DATADIR_LOCKED" : "RESTORE_REFUSED",
                 r.message, from);
        return;
    }

    wrn_push_report(reply, &rep);
    (void)json_push_kv_str(
        &reply->data, "next_steps",
        "run 'core wallet rescan' to rebuild transparent history, then "
        "'core wallet rescan-witnesses' before spending a shielded note");

    if (!confirm) {
        struct json_value ci;
        json_init(&ci);
        json_set_object(&ci);
        (void)json_push_kv_str(&ci, "from", from);
        (void)json_push_kv_str(&ci, "datadir", datadir);
        (void)json_push_kv_bool(&ci, "confirm", true);
        char commit[640];
        size_t n = json_write(&ci, commit, sizeof(commit));
        if (n == 0 || n >= sizeof(commit))
            (void)snprintf(commit, sizeof(commit), "{\"confirm\":true}");
        json_free(&ci);
        (void)json_push_kv_str(&reply->data, "stage", "plan");
        (void)json_push_kv_str(&reply->data, "action", "restore");
        (void)json_push_kv_bool(&reply->data, "committed", false);
        (void)json_push_kv_str(&reply->data, "plan_token", token);
        (void)json_push_kv_str(
            &reply->data, "confirm_hint",
            "the counts above are a rehearsal that was rolled back; re-run "
            "this command with commit_input to write them");
        (void)json_push_kv_str(&reply->data, "commit_input", commit);
        reply->error.mutated = false;
        return;
    }

    (void)json_push_kv_str(&reply->data, "stage", "committed");
    (void)json_push_kv_bool(&reply->data, "committed", true);
    (void)json_push_kv_str(&reply->data, "plan_token", token);
    reply->error.mutated = true;
}

void zcl_native_handle_wallet_backup_decrypt(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    const char *from = json_get_str(json_get(request->input, "from"));
    const char *to = json_get_str(json_get(request->input, "to"));
    if (!from || !from[0] || !to || !to[0]) {
        wrn_fail(reply, ZCL_COMMAND_EXIT_INVALID, "MISSING_PATH",
                 "both from (encrypted backup) and to (output file) are "
                 "required", "core.wallet.backup.decrypt");
        return;
    }
    const char *password = json_get_str(json_get(request->input, "password"));
    if (!password || !password[0])
        password = getenv("WALLET_BACKUP_PASSWORD");
    bool confirm = json_get_bool_or(request->input, "confirm", false);

    char token[17];
    wrn_plan_token(token, from, to);

    if (!confirm) {
        struct json_value ci;
        json_init(&ci);
        json_set_object(&ci);
        (void)json_push_kv_str(&ci, "from", from);
        (void)json_push_kv_str(&ci, "to", to);
        (void)json_push_kv_bool(&ci, "confirm", true);
        char commit[640];
        size_t n = json_write(&ci, commit, sizeof(commit));
        if (n == 0 || n >= sizeof(commit))
            (void)snprintf(commit, sizeof(commit), "{\"confirm\":true}");
        json_free(&ci);
        (void)json_push_kv_str(&reply->data, "stage", "plan");
        (void)json_push_kv_str(&reply->data, "action", "backup-decrypt");
        (void)json_push_kv_bool(&reply->data, "committed", false);
        (void)json_push_kv_str(&reply->data, "from", from);
        (void)json_push_kv_str(&reply->data, "to", to);
        (void)json_push_kv_bool(&reply->data, "password_available",
                                password && password[0]);
        (void)json_push_kv_str(
            &reply->data, "warning",
            "commit writes every wallet key to this path in the clear");
        (void)json_push_kv_str(&reply->data, "plan_token", token);
        (void)json_push_kv_str(&reply->data, "commit_input", commit);
        reply->error.mutated = false;
        return;
    }

    if (!password || !password[0]) {
        wrn_fail(reply, ZCL_COMMAND_EXIT_INVALID, "MISSING_PASSWORD",
                 "set WALLET_BACKUP_PASSWORD or pass password: the backup is "
                 "encrypted under it", from);
        return;
    }

    struct zcl_result r = wallet_backup_decrypt_file(from, to, password);
    if (!r.ok) {
        wrn_fail(reply, ZCL_COMMAND_EXIT_FAILED, "DECRYPT_FAILED", r.message,
                 from);
        return;
    }
    (void)json_push_kv_str(&reply->data, "stage", "committed");
    (void)json_push_kv_bool(&reply->data, "committed", true);
    (void)json_push_kv_str(&reply->data, "from", from);
    (void)json_push_kv_str(&reply->data, "to", to);
    (void)json_push_kv_bool(&reply->data, "decrypted", true);
    (void)json_push_kv_str(
        &reply->data, "next_steps",
        "restore it with 'core wallet restore --from=<to>'; the file holds "
        "every private key in the clear until you delete it");
    (void)json_push_kv_str(&reply->data, "plan_token", token);
    reply->error.mutated = true;
}

/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * Registry handlers for the recovery-phrase leaves —
 * `core.wallet.recovery.status` and `core.wallet.recovery.restore`.
 *
 * Like the two backup-file recovery leaves next door, these run OFFLINE:
 * they call wallet_recovery_service directly, in-process, against a datadir
 * on disk. A user typing their twelve words into a rebuilt machine has no
 * node to talk to yet.
 *
 * There is deliberately NO leaf that prints an existing wallet's phrase.
 * That is not a gap. The node keeps only the 32-byte seed the words derive,
 * and a seed cannot be turned back into words — so no such command could
 * exist without storing the phrase, and storing the phrase would make one
 * read of node.db the loss of every coin in it. The words are shown once,
 * when the wallet is created, and the help text says so.
 *
 * The phrase never leaves this file's stack: it is read out of the request
 * (or ZCL_RECOVERY_PHRASE), handed to the service, and never pushed onto a
 * reply, into a plan echo, or into a log line. The commit_input a plan
 * hands back deliberately omits it, so the phrase is not sitting in a
 * response body waiting to be pasted into a chat log.
 *
 * Bound in config/commands/core.def.
 */

#include "controllers/wallet_native_handlers.h"

#include "chain/chainparams.h"
#include "controllers/agent_controller.h"
#include "json/json.h"
#include "kernel/command_registry.h"
#include "command/native_command.h"
#include "services/wallet_recovery_service.h"
#include "support/cleanse.h"
#include "util/boot_phase.h"
#include "util/log_macros.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define WRP_TAG "native.wallet.recovery"

/* Address encoding reads chain_params_get(), which aborts if nothing ever
 * called chain_params_select(). A one-shot CLI process never boots, so it
 * has to select for itself — the same thing native_offline_query.c does.
 *
 * Guarded on the boot stage, which the unconditional call next door is not:
 * inside a RUNNING node boot has already selected the node's own network,
 * and re-selecting mainnet under a testnet or regtest node would switch the
 * whole process's consensus parameters out from under it. BOOT_STAGE_INIT
 * means no boot happened in this process, so there is no selection to
 * clobber. Mainnet-only, as the offline recovery story targets mainnet
 * datadirs; a testnet datadir needs a network hint this leaf does not
 * accept yet. */
static void wrp_select_chain_params_if_standalone(void)
{
    if (boot_stage_current() == BOOT_STAGE_INIT)
        chain_params_select(CHAIN_MAIN);
}

/* The datadir a recovery targets when the caller names none: the runtime's
 * own context datadir, else the stock $HOME/.zclassic-c23. Always echoed
 * back so the operator confirms the path before the commit. */
static void wrp_default_datadir(char *out, size_t cap)
{
    const char *ctx = agent_runtime_context_datadir();
    if (ctx && ctx[0]) {
        snprintf(out, cap, "%s", ctx);
        return;
    }
    const char *home = getenv("HOME");
    snprintf(out, cap, "%s/.zclassic-c23", home && home[0] ? home : ".");
}

/* Every public field of a report. Nothing here is secret: addresses are
 * meant to be shown, and the phrase is not in the report at all. */
static void wrp_push_report(struct zcl_command_reply *reply,
                            const struct wallet_recovery_report *rep)
{
    (void)json_push_kv_str(&reply->data, "datadir", rep->datadir);
    (void)json_push_kv_str(&reply->data, "target_db", rep->target_db);
    (void)json_push_kv_int(&reply->data, "keys_before", rep->keys_before);
    (void)json_push_kv_int(&reply->data, "keys_after", rep->keys_after);
    (void)json_push_kv_bool(&reply->data, "wallet_already_present",
                            rep->keys_before > 0 || rep->seed_present_before);
    if (rep->first_address[0])
        (void)json_push_kv_str(&reply->data, "first_address",
                               rep->first_address);
    if (rep->first_shielded_address[0])
        (void)json_push_kv_str(&reply->data, "first_shielded_address",
                               rep->first_shielded_address);
}

void zcl_native_handle_wallet_recovery_status(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    wrp_select_chain_params_if_standalone();
    char datadir[1024];
    const char *dd = json_get_str(json_get(request->input, "datadir"));
    if (dd && dd[0])
        snprintf(datadir, sizeof(datadir), "%s", dd);
    else
        wrp_default_datadir(datadir, sizeof(datadir));

    struct wallet_recovery_report rep;
    struct zcl_result r = wallet_recovery_status(datadir, &rep);
    if (!r.ok) {
        wrp_push_report(reply, &rep);
        wnh_fail(reply, ZCL_COMMAND_EXIT_FAILED, "NO_WALLET", r.message,
                 datadir);
        return;
    }

    wrp_push_report(reply, &rep);
    (void)json_push_kv_bool(&reply->data, "recoverable_from_phrase",
                            rep.seed_installed);
    if (rep.seed_installed) {
        (void)json_push_kv_str(&reply->data, "meaning",
            "this wallet's keys all descend from its recovery phrase, so the "
            "twelve words you wrote down when it was created bring it back "
            "on any machine");
        (void)json_push_kv_str(&reply->data, "phrase_shown_again",
            "never — the node stores only the key material the words derive, "
            "and the words cannot be worked back out of it. If you did not "
            "write them down, take a file backup now (core wallet backup "
            "now) and treat that file as the only copy");
    } else {
        (void)json_push_kv_str(&reply->data, "meaning",
            "this wallet was created before recovery phrases. Its keys are "
            "independently random, so no phrase can rebuild them and there "
            "is no phrase to show you");
        (void)json_push_kv_str(&reply->data, "what_to_do",
            "back it up as a file: 'core wallet backup now', and keep that "
            "file somewhere the machine's disk failing cannot take with it. "
            "To move to a phrase-backed wallet, recover a NEW empty datadir "
            "from a new phrase and send your coins there");
    }
    reply->error.mutated = false;
}

void zcl_native_handle_wallet_recovery_restore(
    const struct zcl_command_request *request, struct zcl_command_reply *reply)
{
    wrp_select_chain_params_if_standalone();
    /* argv is world-readable on this machine; the env var is the quieter
     * way in and the help text names it. */
    const char *phrase = json_get_str(json_get(request->input, "phrase"));
    if (!phrase || !phrase[0])
        phrase = getenv("ZCL_RECOVERY_PHRASE");
    if (!phrase || !phrase[0]) {
        wnh_fail(reply, ZCL_COMMAND_EXIT_INVALID, "MISSING_PHRASE",
                 "phrase is required: the words you wrote down when this "
                 "wallet was created. Pass it as the phrase input, or set "
                 "ZCL_RECOVERY_PHRASE so it stays out of your shell history",
                 "core.wallet.recovery.restore");
        return;
    }

    char datadir[1024];
    const char *dd = json_get_str(json_get(request->input, "datadir"));
    if (dd && dd[0])
        snprintf(datadir, sizeof(datadir), "%s", dd);
    else
        wrp_default_datadir(datadir, sizeof(datadir));

    bool confirm = json_get_bool_or(request->input, "confirm", false);

    struct wallet_recovery_request req = {
        .phrase  = phrase,
        .datadir = datadir,
        .dry_run = !confirm,
    };
    struct wallet_recovery_report rep;
    struct zcl_result r = wallet_recovery_run(&req, &rep);

    if (!r.ok) {
        wrp_push_report(reply, &rep);
        const char *code = "RECOVERY_REFUSED";
        enum zcl_command_exit ex = ZCL_COMMAND_EXIT_FAILED;
        if (r.code == -60) { code = "INVALID_PHRASE"; ex = ZCL_COMMAND_EXIT_INVALID; }
        else if (r.code == -61) { code = "DATADIR_LOCKED"; ex = ZCL_COMMAND_EXIT_BLOCKED; }
        else if (r.code == -62) { code = "WALLET_ALREADY_PRESENT"; ex = ZCL_COMMAND_EXIT_BLOCKED; }
        /* r.message describes the shape of the failure and never quotes
         * the phrase; the evidence field carries the datadir, not the
         * words. */
        wnh_fail(reply, ex, code, r.message, datadir);
        return;
    }

    wrp_push_report(reply, &rep);
    (void)json_push_kv_str(
        &reply->data, "next_steps",
        "start the node on this datadir, then run 'core wallet rescan' to "
        "find your transparent history and 'core wallet rescan-witnesses' "
        "before spending a shielded note — the keys came back from the "
        "phrase, but balances and history are chain state and have to be "
        "read back off the chain");

    if (!confirm) {
        /* The commit input carries the datadir and the confirm flag ONLY.
         * Putting the phrase in a response body is how a wallet ends up in
         * a paste buffer. */
        char commit[900];
        (void)snprintf(commit, sizeof(commit),
                       "{\"phrase\":\"<your words>\",\"datadir\":\"%s\","
                       "\"confirm\":true}", datadir);
        (void)json_push_kv_str(&reply->data, "stage", "plan");
        (void)json_push_kv_str(&reply->data, "action", "recovery-restore");
        (void)json_push_kv_bool(&reply->data, "committed", false);
        (void)json_push_kv_str(
            &reply->data, "confirm_hint",
            "the addresses above are what these words open — check the first "
            "one against what you remember, then re-run with confirm true to "
            "write the wallet. Nothing has been written yet");
        (void)json_push_kv_str(&reply->data, "commit_input", commit);
        (void)json_push_kv_str(&reply->data, "phrase_in_commit_input",
            "no — retype your own words; they are deliberately not echoed "
            "back to you here");
        reply->error.mutated = false;
        return;
    }

    (void)json_push_kv_str(&reply->data, "stage", "committed");
    (void)json_push_kv_bool(&reply->data, "committed", true);
    (void)json_push_kv_bool(&reply->data, "recovered", rep.seed_installed);
    reply->error.mutated = true;
}

/* Copyright (c) 2010 Satoshi Nakamoto
 * Copyright (c) 2009-2014 The Bitcoin Core developers
 * Copyright 2026 Rhett Creighton - Apache License 2.0
 * Distributed under the MIT software license, see the accompanying
 * file COPYING or http://www.opensource.org/licenses/mit-license.php. */

#include "rpc/server.h"
#include "util/sync.h"
#include "util/util.h"
#include "core/utiltime.h"
#include <stdlib.h>
#include <string.h>
#include <stdio.h>
#include <assert.h>
#include <inttypes.h>

static bool rpc_running = false;
static bool rpc_in_warmup = true;
static char warmup_status[256] = "RPC server started";
static zcl_mutex_t cs_warmup;
static bool cs_warmup_inited = false;

static void ensure_warmup_mutex(void)
{
    if (!cs_warmup_inited) {
        zcl_mutex_init(&cs_warmup);
        cs_warmup_inited = true;
    }
}

void json_request_init(struct json_request *req)
{
    json_init(&req->id);
    req->method[0] = '\0';
    json_init(&req->params);
}

void json_request_free(struct json_request *req)
{
    json_free(&req->id);
    json_free(&req->params);
}

bool json_request_parse(struct json_request *req,
                        const struct json_value *val_request)
{
    if (!req || !val_request)
        return false;
    if (val_request->type != JSON_OBJ)
        return false;

    const struct json_value *vid = json_get(val_request, "id");
    if (vid) {
        json_free(&req->id);
        json_copy(&req->id, vid);
    }

    const struct json_value *vm = json_get(val_request, "method");
    if (!vm || vm->type != JSON_STR)
        return false;
    snprintf(req->method, sizeof(req->method), "%s", json_get_str(vm));

    const struct json_value *vp = json_get(val_request, "params");
    json_free(&req->params);
    if (vp && vp->type == JSON_ARR) {
        json_init(&req->params);
        req->params.type = JSON_ARR;
        for (size_t i = 0; i < vp->num_children; i++)
            json_push_back(&req->params, &vp->children[i]);
    } else {
        json_init(&req->params);
        json_set_array(&req->params);
    }
    return true;
}

void rpc_table_init(struct rpc_table *t)
{
    memset(t, 0, sizeof(*t));
}

bool rpc_table_append(struct rpc_table *t, const struct rpc_command *cmd)
{
    if (t->running)
        return false;
    if (rpc_table_find(t, cmd->name))
        return false;
    if (t->num_commands >= MAX_RPC_COMMANDS)
        return false;
    t->commands[t->num_commands++] = *cmd;
    return true;
}

void rpc_table_must_append(struct rpc_table *t, const struct rpc_command *cmd)
{
    if (rpc_table_append(t, cmd))
        return;

    /* Determine why the append failed so the abort message names the
     * exact pathology. Boot-time invariant: every registrar registers
     * cleanly OR the operator gets a precise diagnosis. */
    const char *reason;
    if (t->running)
        reason = "table_already_running";
    else if (rpc_table_find(t, cmd->name))
        reason = "duplicate_name";
    else if (t->num_commands >= MAX_RPC_COMMANDS)
        reason = "table_full_cap_256";
    else
        reason = "unknown";

    /* Emit one node.log line so the failure is greppable via zcl_node_log,
     * then a louder FATAL on stderr so systemd journal captures it too.
     * We can't use LOG_FAIL here — that macro `return false`s, but this
     * function is void: the only acceptable outcome is abort(). The
     * abort() below is the terminal observable; pairing it with an
     * event would be lost in the noise of unwinding. */
    fprintf(stderr, "[rpc] %s:%d %s(): "  // obs-ok:abort-follows-this-line
            "registration_failed name=%s category=%s reason=%s count=%zu\n",
            __FILE__, __LINE__, __func__,
            cmd->name ? cmd->name : "(null)",
            cmd->category ? cmd->category : "(null)",
            reason, t->num_commands);
    fprintf(stderr,  // obs-ok:abort-follows-this-line
            "FATAL: rpc_table_must_append(\"%s\") failed: %s (count=%zu/%d)\n",
            cmd->name ? cmd->name : "(null)", reason,
            t->num_commands, MAX_RPC_COMMANDS);
    abort();
}

const struct rpc_command *rpc_table_find(const struct rpc_table *t,
                                         const char *name)
{
    for (size_t i = 0; i < t->num_commands; i++)
        if (strcmp(t->commands[i].name, name) == 0)
            return &t->commands[i];
    return NULL;
}

bool rpc_table_execute(const struct rpc_table *t, const char *method,
                       const struct json_value *params,
                       struct json_value *result)
{
    char status[256];
    if (rpc_is_in_warmup(status, sizeof(status))) {
        json_rpc_error_full(result, RPC_IN_WARMUP, status, method);
        return false;
    }
    const struct rpc_command *cmd = rpc_table_find(t, method);
    if (!cmd) {
        json_rpc_error_full(result, RPC_METHOD_NOT_FOUND,
                            "Method not found", method);
        return false;
    }
    return cmd->actor(params, false, result);
}



void set_rpc_warmup_status(const char *status)
{
    ensure_warmup_mutex();
    zcl_mutex_lock(&cs_warmup);
    snprintf(warmup_status, sizeof(warmup_status), "%s", status);
    zcl_mutex_unlock(&cs_warmup);
}

void set_rpc_warmup_finished(void)
{
    ensure_warmup_mutex();
    zcl_mutex_lock(&cs_warmup);
    assert(rpc_in_warmup);
    rpc_in_warmup = false;
    zcl_mutex_unlock(&cs_warmup);
}

bool rpc_is_in_warmup(char *status_out, size_t status_size)
{
    ensure_warmup_mutex();
    zcl_mutex_lock(&cs_warmup);
    if (status_out)
        snprintf(status_out, status_size, "%s", warmup_status);
    bool result = rpc_in_warmup;
    zcl_mutex_unlock(&cs_warmup);
    return result;
}



void interrupt_rpc(void)
{
    LogPrint("rpc", "Interrupting RPC\n");
    rpc_running = false;
}

/* Convert a JSON number to a CAmount (zatoshis). JSON_INT is taken as a raw
 * zatoshi count; JSON_REAL is a coin value scaled by COIN with round-to-nearest.
 * CONTRACT/CAVEAT: a non-numeric value (string/bool/null/array/object) silently
 * returns 0, which is indistinguishable from a legitimate 0 amount. The
 * signature is fixed (callers depend on it), so callers that must reject a
 * malformed amount have to type-check value->type themselves before calling. */
CAmount amount_from_value(const struct json_value *value)
{
    if (value->type == JSON_INT)
        return value->val.i;
    if (value->type == JSON_REAL)
        return (CAmount)(value->val.d * (double)COIN + 0.5);
    return 0;
}

void value_from_amount(CAmount amount, struct json_value *out)
{
    bool sign = amount < 0;
    int64_t n_abs = sign ? -amount : amount;
    int64_t quotient = n_abs / COIN;
    int64_t remainder = n_abs % COIN;
    char buf[64];
    snprintf(buf, sizeof(buf), "%s%" PRId64 ".%08" PRId64,
             sign ? "-" : "", quotient, remainder);
    json_init(out);
    json_set_str(out, buf);
}

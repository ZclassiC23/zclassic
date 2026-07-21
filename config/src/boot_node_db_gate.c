/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * boot_node_db_gate — the #7 supervision-coverage fix, split out of
 * boot.c (E1 file-size ceiling) so app_init's node.db-open branch stays a
 * short call site. See config/boot_internal.h for the declaration.
 *
 * Every other boot-storage gate (crypto_params_missing, coins_view_integrity,
 * progress_kv_open) names a typed blocker and parks alive-degraded when its
 * storage fails to open. node.db failing to open used to log
 * "Warning: SQLite database unavailable" and CONTINUE booting RAM-only —
 * every wallet key, chain-state write, and progress cursor silently vanishes
 * on the next restart with no operator page. This closes that hole: name a
 * PERMANENT blocker (an unopenable node.db is not something a bounded retry
 * fixes — it needs an operator to look at disk/permissions/corruption) and
 * park like the sibling gates instead of degrading silently. */

#include "config/boot_internal.h"

#include "event/event.h"
#include "util/blocker.h"
#include "util/log_macros.h"

#include <stdio.h>

bool boot_node_db_open_failed_gate(const char *datadir)
{
    fprintf(stderr, "Warning: SQLite database unavailable\n");
    event_emitf(EV_DB_ERROR, 0, "SQLite open failed at %s/node.db",
                datadir ? datadir : "(unset)");

    struct blocker_record rec;
    if (blocker_init(&rec, "node_db_unopened", "boot.node_db",
                     BLOCKER_PERMANENT,
                     "node.db failed to open — continuing would run "
                     "RAM-only with no persistence for wallet keys, "
                     "chain state, or progress") &&
        blocker_set(&rec) == 0)
        event_emitf(EV_OPERATOR_NEEDED, 0,
                    "check=node_db_unopened datadir=%s",
                    datadir ? datadir : "(unset)");

    LOG_WARN("boot.node_db",
             "[boot] node.db failed to open at %s/node.db — NOT continuing "
             "RAM-only; parking alive-degraded after paging the operator",
             datadir ? datadir : "(unset)");
    return boot_park_until_shutdown("node_db_unopened");
}

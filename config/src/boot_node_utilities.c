/* Copyright 2026 Rhett Creighton - Apache License 2.0
 *
 * boot_node_utilities.c — node operator utility entry points.
 *
 * Part of the boot composition root (extracted from boot_services.c). This
 * unit holds the small operator-facing utilities that hang off the live boot
 * context: -addnode connection setup (app_add_node), the metrics thread
 * lifecycle (app_start_metrics / app_stop_metrics) with its injected external
 * gauge callback (boot_metrics_external_gauges), and the async sync-state
 * observer that logs sync-pipeline transitions (boot_sync_state_logger).
 *
 * Owns no file-statics. The public app_* entry points reach the live context
 * through boot_active_svc() (declared in boot_internal.h, called from main.c);
 * boot_metrics_external_gauges is private here (only app_start_metrics injects
 * it). boot_sync_state_logger is wired by app_init_services in boot_services.c,
 * so its prototype lives in config/boot_internal.h. */

#include "config/boot_internal.h"
#include "services/node_health_service.h"
#include "services/legacy_mirror_sync_service.h"
#include "services/sync_monitor.h"
#include "chain/chainparams.h"
#include "sync/sync_state.h"
#include "event/event.h"
#include "net/connman.h"
#include <netdb.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/socket.h>
#include <netinet/in.h>

/* External-gauge callback injected into the metrics thread: snapshots
 * sync state, UTXO count, tip-advance age, mirror lag, and peer counts. */
static void boot_metrics_external_gauges(
    struct metrics_external_gauges *out,
    void *ctx)
{
    struct boot_svc_ctx *svc = ctx;
    enum sync_state state;
    struct legacy_mirror_sync_stats lms = {0};
    struct node_health_snapshot nhs = {0};

    if (!out)
        return;

    state = sync_get_state();
    out->sync_state = (int)state;
    snprintf(out->sync_state_name, sizeof(out->sync_state_name), "%s",
             sync_state_name(state));

    if (svc && svc->node_db && svc->node_db->open)
        out->utxo_count = node_db_utxo_count(svc->node_db);

    out->tip_advance_age_seconds = sync_monitor_tip_advance_age();

    legacy_mirror_sync_stats_snapshot(&lms);
    out->mirror_lag_blocks = lms.enabled && lms.lag_known
                                  ? (int64_t)lms.lag : -1;
    out->mirror_lag_breach_seconds = lms.lag_breach_seconds;
    out->mirror_lag_critical_seconds = lms.lag_critical_seconds;

    if (svc && svc->state) {
        node_health_collect(&nhs, svc->node_db, svc->state);
        out->magicbean_peer_count = (int64_t)nhs.magicbean_peer_count;
        out->zclassic_c23_peer_count = (int64_t)nhs.zclassic_c23_peer_count;
    }
}

/* ── Utility functions ─────────────────────────────────────── */

/* Resolve and open an outbound connection to a -addnode host[:port]. */
void app_add_node(const char *host, int port)
{
    struct boot_svc_ctx *svc = boot_active_svc();
    char hostbuf[256];
    snprintf(hostbuf, sizeof(hostbuf), "%s", host);

    if (port <= 0) {
        char *colon = strrchr(hostbuf, ':');
        if (colon && colon != hostbuf) {
            int p = atoi(colon + 1);
            if (p > 0 && p < 65536) {
                port = p;
                *colon = '\0';
            }
        }
    }

    uint16_t use_port = port > 0 ? (uint16_t)port
                                 : svc->connman->manager.default_port;

    struct net_address addr;
    net_address_init(&addr);
    addr.svc.port = use_port;

    struct addrinfo hints;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_UNSPEC;
    hints.ai_socktype = SOCK_STREAM;

    struct addrinfo *res = NULL;
    if (getaddrinfo(hostbuf, NULL, &hints, &res) == 0 && res) {
        if (res->ai_family == AF_INET) {
            struct sockaddr_in *s4 = (struct sockaddr_in *)res->ai_addr;
            memset(addr.svc.addr.ip, 0, 10);
            addr.svc.addr.ip[10] = 0xff;
            addr.svc.addr.ip[11] = 0xff;
            memcpy(addr.svc.addr.ip + 12, &s4->sin_addr, 4);
        } else if (res->ai_family == AF_INET6) {
            struct sockaddr_in6 *s6 = (struct sockaddr_in6 *)res->ai_addr;
            memcpy(addr.svc.addr.ip, &s6->sin6_addr, 16);
        }
        freeaddrinfo(res);

        printf("Connecting to addnode %s:%u\n", hostbuf, use_port);
        connman_open_connection(svc->connman, &addr);
    } else {
        printf("Failed to resolve addnode %s\n", hostbuf);
    }
}

/* Start the Prometheus metrics thread with injected external gauges. */
void app_start_metrics(bool mining)
{
    struct boot_svc_ctx *svc = boot_active_svc();
    svc->metrics->ms = svc->state;
    svc->metrics->cm = svc->connman;
    svc->metrics->params = chain_params_get();
    svc->metrics->mining = mining;
    svc->metrics->external_gauges = boot_metrics_external_gauges;
    svc->metrics->external_gauges_ctx = svc;
    if (!metrics_start(svc->metrics))
        fprintf(stderr, "WARNING: failed to start metrics thread\n");
}

/* Stop the metrics thread. */
void app_stop_metrics(void)
{
    struct boot_svc_ctx *svc = boot_active_svc();
    metrics_stop(svc->metrics);
}

/* ── Sync state observer ──────────────────────────────────────── *
 * Async observer that logs sync state transitions, tip updates,
 * block connections, and reorgs. Provides high-level observability
 * of the sync pipeline without blocking any P2P or validation thread.
 *
 * Registered at boot via event_observe_async() for:
 *   EV_SYNC_STATE_CHANGE — sync FSM transitions
 *   EV_TIP_UPDATED       — chain tip advances
 *   EV_BLOCK_CONNECTED    — individual blocks connected
 *   EV_REORG_START        — chain reorganization begins */
void boot_sync_state_logger(enum event_type type, uint32_t peer_id,
                               const void *payload, uint32_t payload_len,
                               void *ctx)
{
    (void)ctx;
    const char *msg = (payload_len > 0 && payload) ? (const char *)payload : "";

    switch (type) {
    case EV_SYNC_STATE_CHANGE:
        printf("[observer] sync state → %s\n", msg);
        break;
    case EV_TIP_UPDATED:
        /* Only log major milestones to avoid flooding */
        break;
    case EV_BLOCK_CONNECTED:
        break; /* too noisy for printf, event log captures it */
    case EV_REORG_START:
        fprintf(stderr, "[observer] REORG: %s (peer=%u)\n", msg, peer_id);
        break;
    default:
        break;
    }
    fflush(stdout);
}

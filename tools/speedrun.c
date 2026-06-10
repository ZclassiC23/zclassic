/* Copyright 2026 Rhett Creighton - Apache License 2.0
 * ZClassic23 Network Sync Speed Run
 *
 * Measures end-to-end time from zero to chain tip via file service.
 * Connects to a RUNNING power node's file service over TCP.
 * No source node shutdown required.
 *
 * Build: make speedrun
 * Usage: build/bin/speedrun [peer_address]
 *   Default peer: 127.0.0.1 (localhost power node)
 *
 * Phases:
 *   1. File service download (block files + block_index + consensus snapshot)
 *   2. Block index load (mmap flat file)
 *   3. UTXO import (from consensus_snapshot.db or replay)
 *   4. Delta block replay (snapshot height → tip)
 */

#include "platform/time_compat.h"
#include "config/boot.h"
#include "chain/chainparams.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <sys/stat.h>
#include <signal.h>

/* Required by process_block.c */
volatile sig_atomic_t g_shutdown_requested = 0;

static inline int64_t now_ms(void)
{
    struct timespec ts;
    platform_time_monotonic_timespec(&ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

int main(int argc, char *argv[])
{
    setbuf(stdout, NULL);
    setbuf(stderr, NULL);

    const char *peer = "127.0.0.1";
    if (argc >= 2)
        peer = argv[1];

    char dst_base[256];
    snprintf(dst_base, sizeof(dst_base), "/tmp/zcl23-speedrun-%d", (int)getpid());

    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║     ZClassic23 Network Sync Speed Run                ║\n");
    printf("╚══════════════════════════════════════════════════════╝\n\n");
    printf("Peer:    %s:18034\n", peer);
    printf("Target:  %s\n\n", dst_base);

    /* Create clean target */
    mkdir(dst_base, 0755);

    int64_t t_total = now_ms();

    /* Boot with -fileservice= pointing to the power node.
     * The boot sequence will:
     *   1. Connect to file service, download all data
     *   2. Load block index from flat file
     *   3. Import consensus snapshot or replay
     *   4. Start background UTXO replay if needed */
    struct app_context ctx;
    app_context_defaults(&ctx);
    ctx.datadir = dst_base;
    ctx.file_service_peer = peer;
    ctx.listen = false;
    ctx.p2p_port = 18044;
    ctx.rpc_port = 18245;
    ctx.tor = false;

    bool ok = app_init(&ctx);

    int64_t total_ms = now_ms() - t_total;

    printf("\n");
    printf("╔══════════════════════════════════════════════════════╗\n");
    printf("║  SPEED RUN RESULT                                    ║\n");
    printf("╠══════════════════════════════════════════════════════╣\n");
    printf("║  TOTAL (zero to tip):  %6lldms  (%4.1fs)            ║\n",
           (long long)total_ms, (double)total_ms / 1000.0);
    printf("║  Status: %s                                    ║\n",
           ok ? "SUCCESS" : "FAILED ");
    printf("╚══════════════════════════════════════════════════════╝\n");

    printf("\nTemp data at: %s\n", dst_base);
    printf("Clean up with: rm -rf %s\n", dst_base);

    return ok ? 0 : 1;
}
